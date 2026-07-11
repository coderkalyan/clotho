#pragma once

// Interning pool: deduplicates types and values into a single unified handle
// space — every interned item gets one `Ref`, indexing one shared store. The
// instruction stream references items by that compact 4-byte slot instead of
// embedding the value.
//
// Dedup keeps the item bytes exactly once: the store (`items_`) is the single
// source of truth, and the dedup set holds only u32 indices, hashing and
// comparing them by dereferencing back into the store.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace clotho::ir {

// Handle to any interned item (type or value).
enum class Ref : uint32_t {
    // Absent reference.
    Invalid = std::numeric_limits<uint32_t>::max(),
};

// State encoding of a value, as the number of bit-planes it stores. A value of
// width W is `plane count` parallel packed W-bit planes (bit-sliced), val plane
// first. The enumerator value IS the plane count.
enum class Domain : uint8_t {
    // 2-state: val plane only (0, 1).
    Two = 1,
    // 4-state: val then xz plane, VPI 0=(0,0) 1=(1,0) X=(1,1) Z=(0,1).
    Four = 2,
    // 9-state: a 4-bit per-bit code, bit-sliced across 4 planes (see
    // value-encoding note in InternPool.cpp). Provisional; no type yields it yet.
    Nine = 4,
};

// Canonical bytes of one plane for a packed value of the given width (IEEE-style
// sizing): W<=8 -> 1B, <=16 -> 2B, <=32 -> 4B, <=64 -> 8B, else ceil(W/64) words.
constexpr uint32_t plane_bytes(uint32_t width) {
    if (width == 0) {
        return 0;
    }
    if (width <= 8) {
        return 1;
    }
    if (width <= 16) {
        return 2;
    }
    if (width <= 32) {
        return 4;
    }
    if (width <= 64) {
        return 8;
    }

    return ((width + 63) / 64) * 8;
}

// A SystemVerilog type. Aliases (reg, int, byte, ...) are lowered to these
// before interning. Composite kinds refer to their element via another interned
// Ref, so the pool is self-referential and dedup stays meaningful.
struct Type {
    enum class Kind : uint8_t {
        // Valueless: void function / task returns.
        Void,
        // 4-state scalar bit (0, 1, x, z).
        Logic,
        // 2-state scalar bit (0, 1).
        Bit,
        // IEEE-754 double: SV `real` / `realtime` (LRM 6.12). 2-state — no x/z;
        // the value is the 64-bit bit pattern in a single val plane.
        Real,
        // IEEE-754 float: SV `shortreal` (LRM 6.12). 32-bit pattern, one plane.
        ShortReal,
        // Packed array / vector: contiguous bits of `element`, e.g. logic [7:0].
        PackedArray,
        // Unpacked array (memory): collection of `element` over [left:right].
        UnpackedArray,
        // Dynamic string (LRM §6.16). Not plane-sized: layout() reports width 0
        // and runtime storage is an opaque rt_string handle, so every consumer
        // must branch on the kind before using layout facts.
        String,
        // struct / union (LRM §7.2 / §7.3), declaration-order member list in the
        // pool's field-table side array (see `fields_id` / `struct_fields`).
        // Packed: a single bit-vector value (LRM 7.2.1) — `element`/`left`/
        // `right` describe it exactly like PackedArray, so it shares layout()'s
        // PackedArray math. Unpacked: a heterogeneous aggregate with no
        // plane-packed value representation (mirrors String's width-0 sentinel);
        // the backend owns its own storage layout for it.
        Struct,
    };

    // Discriminant for this type.
    Kind kind = Kind::Void;
    // Signedness, for Logic / Bit / PackedArray, and packed Struct.
    bool is_signed = false;
    // Kind::Struct only: true for a packed struct/union (bit-vector value
    // semantics), false for unpacked (aggregate storage).
    bool is_packed = false;
    // Element type, for PackedArray / UnpackedArray, and packed Struct (the
    // scalar - logic or bit - whose domain the whole packed value shares).
    Ref element = Ref::Invalid;
    // Declared most-significant bound [left:right], for array kinds and packed
    // Struct (width - 1, mirroring PackedArray's convention).
    uint32_t left = 0;
    // Declared least-significant bound [left:right], for array kinds and packed
    // Struct (always 0).
    uint32_t right = 0;
    // Kind::Struct only: index into the pool's struct field-table side array.
    uint32_t fields_id = 0;

    bool operator==(const Type&) const = default;
};

// One declared member of a struct/union type, in declaration order.
struct Field {
    // Declared member name (LRM 7.2.1 / 7.3: struct/union members are always
    // named).
    std::string name;
    // Field's own interned type.
    Ref type = Ref::Invalid;
    // Bit offset from the LSB (packed only; LRM 7.2.1 - first declared member
    // is the most-significant). Unused (0) for an unpacked member — those are
    // addressed by declaration index (this entry's position in the field list)
    // instead.
    uint32_t bit_offset = 0;
};

// An interned value, in the canonical plane-packed format shared with runtime
// state. Equality / hashing are content-based and live in the pool (spilled
// bytes need the side array to resolve), so this carries no operator==.
struct Value {
    // Type of this value (gives width and domain via the pool's layout).
    Ref type = Ref::Invalid;
    // Total canonical bytes across all planes.
    uint32_t byte_count = 0;
    // Canonical plane bytes, val plane first: inline when they fit, else an
    // offset into the pool's value-byte side array.
    union {
        uint8_t  inline_bytes[16];
        uint32_t spill_offset;
    } data{};
};

// One entry in the unified store: a type or a value.
using Item = std::variant<Type, Value>;

// Largest value that stores its bytes inline rather than spilling.
inline constexpr uint32_t kInlineValueBytes = sizeof(Value{}.data.inline_bytes);

// Resolve a value's canonical bytes against the pool's spill array.
inline std::span<const uint8_t> value_bytes(const Value& v, const std::vector<uint8_t>& spill) {
    if (v.byte_count <= kInlineValueBytes) {
        return { v.data.inline_bytes, v.byte_count };
    }

    return { spill.data() + v.data.spill_offset, v.byte_count };
}

} // namespace clotho::ir

// wyhash for interned types. Fields are marshalled into padding-free words so
// the byte hash is deterministic.
template<>
struct ankerl::unordered_dense::hash<clotho::ir::Type> {
    using is_avalanching = void;

    auto operator()(const clotho::ir::Type& t) const noexcept -> uint64_t {
        const std::array<uint32_t, 5> words{
            (static_cast<uint32_t>(t.kind) << 2) | (static_cast<uint32_t>(t.is_signed) << 1) |
              static_cast<uint32_t>(t.is_packed),
            static_cast<uint32_t>(t.element),
            t.left,
            t.right,
            t.fields_id,
        };
        return detail::wyhash::hash(words.data(), sizeof(words));
    }
};

namespace clotho::ir {

// Canonical comparison form of an item: a type, or a value's (type, bytes).
// Both stored indices and not-yet-interned candidates resolve to this, so
// hashing and equality have a single implementation (hash_view / eq_view), and a
// candidate can be probed without first being placed in the store.
struct ItemView {
    bool                     is_type    = false;
    Type                     type       = {};
    Ref                      value_type = Ref::Invalid;
    std::span<const uint8_t> value_bytes;
};

// Resolve a stored item to its view (values dereference the spill array).
inline ItemView view_of(const Item& item, const std::vector<uint8_t>& spill) {
    if (const Type* t = std::get_if<Type>(&item)) {
        return { .is_type = true, .type = *t };
    }
    const Value& v = std::get<Value>(item);

    return { .is_type = false, .value_type = v.type, .value_bytes = value_bytes(v, spill) };
}

inline uint64_t hash_view(const ItemView& v) {
    if (v.is_type) {
        return ankerl::unordered_dense::hash<Type>{}(v.type);
    }
    const uint64_t h = ankerl::unordered_dense::detail::wyhash::hash(v.value_bytes.data(), v.value_bytes.size());

    return ankerl::unordered_dense::detail::wyhash::hash(h ^ (static_cast<uint64_t>(v.value_type) + 1));
}

inline bool eq_view(const ItemView& a, const ItemView& b) {
    if (a.is_type != b.is_type) {
        return false;
    }
    if (a.is_type) {
        return a.type == b.type;
    }

    return a.value_type == b.value_type && a.value_bytes.size() == b.value_bytes.size() &&
           std::equal(a.value_bytes.begin(), a.value_bytes.end(), b.value_bytes.begin());
}

// Cached layout facts derived from a type (computed on demand for now).
struct Layout {
    // State encoding / plane count.
    Domain domain = Domain::Two;
    // Flattened packed width in bits.
    uint32_t width = 0;
    // Canonical bytes of a whole value: plane_count * plane_bytes(width).
    uint32_t byte_count = 0;
};

class InternPool {
public:
    // Built-in scalar types, interned at construction.
    Ref void_type;
    // 1-bit unsigned logic / bit.
    Ref logic_type;
    Ref bit_type;
    // Dynamic string.
    Ref string_type;
    // IEEE-754 double / float (SV real, realtime / shortreal).
    Ref real_type;
    Ref shortreal_type;

    InternPool();

    // Non-movable: the dedup set's hash/eq hold pointers to items_ / value_bytes_.
    InternPool(const InternPool&)            = delete;
    InternPool& operator=(const InternPool&) = delete;

    // Intern an item (used for types). Returns a stable deduplicated handle.
    Ref put(const Item& item);

    // Intern a value from its canonical plane bytes (length must match the
    // type's layout).
    Ref put_value(Ref type, std::span<const uint8_t> bytes);
    // Intern a known integer value (width <= 64, 2- or 4-state); the xz plane,
    // if any, is all-zero. Asserts the type is not Real / ShortReal.
    Ref put_int(Ref type, uint64_t bits);
    // Intern a floating-point value as its IEEE-754 bit pattern (double for
    // Real, rounded to float for ShortReal).
    Ref put_real(Ref type, double value);

    // Whether the type is Real / ShortReal.
    bool is_float(Ref type) const;

    const Item& get(Ref ref) const;
    // Canonical bytes of an interned value.
    std::span<const uint8_t> bytes(Ref ref) const;

    // Packed layout (domain / width / size) of a type.
    Layout layout(Ref type) const;

    // Intern a struct/union's declaration-order field list, returning the id a
    // Type::Kind::Struct stores in `fields_id`. Not deduplicated against
    // existing entries (each struct/union AST symbol lowers its own field list
    // at most once — the caller is expected to cache by symbol, as
    // Lower.cpp's lower_type does).
    uint32_t intern_struct_fields(std::vector<Field> fields);
    // Declared member list of a Kind::Struct type, in declaration order.
    std::span<const Field> struct_fields(uint32_t fields_id) const;

private:
    // Transparent functors: a stored u32 index and an ItemView probe both fold
    // into the same view, so hashing / equality have one implementation and a
    // candidate can be looked up before it is stored.
    struct Hash {
        using is_transparent = void;
        using is_avalanching = void;
        const std::vector<Item>*    items;
        const std::vector<uint8_t>* spill;

        auto operator()(uint32_t i) const -> uint64_t {
            return hash_view(view_of((*items)[i], *spill));
        }
        auto operator()(const ItemView& v) const -> uint64_t {
            return hash_view(v);
        }
    };

    struct Eq {
        using is_transparent = void;
        const std::vector<Item>*    items;
        const std::vector<uint8_t>* spill;

        auto operator()(uint32_t a, uint32_t b) const -> bool {
            return eq_view(view_of((*items)[a], *spill), view_of((*items)[b], *spill));
        }
        auto operator()(uint32_t a, const ItemView& v) const -> bool {
            return eq_view(view_of((*items)[a], *spill), v);
        }
        auto operator()(const ItemView& v, uint32_t a) const -> bool {
            return eq_view(v, view_of((*items)[a], *spill));
        }
    };

    // Unified store of interned items, indexed by Ref. Single source of truth.
    std::vector<Item> items_;
    // Canonical bytes of spilled (wide) values.
    std::vector<uint8_t> value_bytes_;
    // Struct/union field-table storage; a Kind::Struct type's `fields_id`
    // indexes here.
    std::vector<std::vector<Field>> struct_fields_;
    // Dedup index: u32 handles into items_, hashed / compared via the store.
    ankerl::unordered_dense::set<uint32_t, Hash, Eq> set_;
};

} // namespace clotho::ir
