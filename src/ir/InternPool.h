#pragma once

// Minimal interning pool for the IR scaffolding.
//
// Deduplicates types and constant values into a single unified handle space:
// every interned item — type or constant — gets one `Ref`, indexing one shared
// store. The instruction stream references items by that compact 4-byte slot
// instead of embedding the value.
//
// Dedup keeps the item bytes exactly once: the store (`items_`) is the single
// source of truth, and the dedup set holds only u32 indices, hashing and
// comparing them by dereferencing back into the store (a transparent set).
//
// Scaffolding: the value/type kinds below are a representative sample (scalar
// int/float values; int, float, and a structural pointer type). The full set
// is filled in in a later step.

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace clotho::ir {

// Handle to any interned item (type or constant).
enum class Ref : uint32_t {
    // Absent reference.
    invalid = std::numeric_limits<uint32_t>::max(),
};

// A structural type. `ptr` refers to its pointee via another interned Ref, so
// the pool is self-referential and dedup stays meaningful for composite types.
struct Type {
    enum class Kind : uint8_t {
        // Signed integer scalar.
        int_,
        // Floating-point scalar.
        float_,
        // Pointer to `elem`.
        ptr,
    };

    // Discriminant for this type.
    Kind kind = Kind::int_;
    // Pointee for `ptr`, else invalid.
    Ref elem = Ref::invalid;

    bool operator==(const Type&) const = default;
};

// A constant value, tagged by kind. Carries its own type handle.
struct Constant {
    enum class Kind : uint8_t {
        // Signed integer constant; value in int_val.
        int_,
        // Floating-point constant; value in float_val.
        float_,
    };

    // Discriminant for this constant.
    Kind kind = Kind::int_;
    // Type of the constant.
    Ref ty = Ref::invalid;
    // Payload for int_.
    int64_t int_val = 0;
    // Payload for float_.
    double float_val = 0.0;

    bool operator==(const Constant&) const = default;
};

// One entry in the unified store: a type or a constant.
using Item = std::variant<Type, Constant>;

} // namespace clotho::ir

// wyhash-based hashes for the dedup set. Fields are marshalled into padding-
// free words so the byte hash is deterministic (a raw struct hash would mix in
// padding, and the double rules out std::has_unique_object_representations).
template<>
struct ankerl::unordered_dense::hash<clotho::ir::Type> {
    using is_avalanching = void;

    auto operator()(const clotho::ir::Type& t) const noexcept -> uint64_t {
        const uint64_t packed = (static_cast<uint64_t>(t.kind) << 32) | static_cast<uint32_t>(t.elem);
        return detail::wyhash::hash(packed);
    }
};

template<>
struct ankerl::unordered_dense::hash<clotho::ir::Constant> {
    using is_avalanching = void;

    auto operator()(const clotho::ir::Constant& c) const noexcept -> uint64_t {
        const std::array<uint64_t, 3> words{
            (static_cast<uint64_t>(c.kind) << 32) | static_cast<uint32_t>(c.ty),
            static_cast<uint64_t>(c.int_val),
            std::bit_cast<uint64_t>(c.float_val),
        };
        return detail::wyhash::hash(words.data(), sizeof(words));
    }
};

template<>
struct ankerl::unordered_dense::hash<clotho::ir::Item> {
    using is_avalanching = void;

    auto operator()(const clotho::ir::Item& item) const noexcept -> uint64_t {
        const uint64_t inner =
          std::visit([](const auto& v) { return ankerl::unordered_dense::hash<std::decay_t<decltype(v)>>{}(v); }, item);
        // Fold in the alternative so a Type and Constant can't collide cheaply.
        return detail::wyhash::hash(inner + item.index());
    }
};

namespace clotho::ir {

class InternPool {
public:
    // Built-in scalar types, interned at construction.
    Ref int_type;
    Ref float_type;

    InternPool();

    // Non-movable: the dedup set's hash/eq hold a pointer to items_.
    InternPool(const InternPool&)            = delete;
    InternPool& operator=(const InternPool&) = delete;

    // Intern an item, returning a stable deduplicated handle.
    Ref put(const Item& item);

    const Item& get(Ref ref) const;

private:
    // Hashes and compares stored u32 indices (or a probe Item) by dereferencing
    // into items_, so the set stores only indices and never copies item bytes.
    struct Hash {
        using is_transparent = void;
        using is_avalanching = void;
        const std::vector<Item>* items;

        auto operator()(uint32_t i) const -> uint64_t {
            return ankerl::unordered_dense::hash<Item>{}((*items)[i]);
        }
        auto operator()(const Item& item) const -> uint64_t {
            return ankerl::unordered_dense::hash<Item>{}(item);
        }
    };

    struct Eq {
        using is_transparent = void;
        const std::vector<Item>* items;

        auto operator()(uint32_t a, uint32_t b) const -> bool {
            return (*items)[a] == (*items)[b];
        }
        auto operator()(uint32_t a, const Item& b) const -> bool {
            return (*items)[a] == b;
        }
        auto operator()(const Item& a, uint32_t b) const -> bool {
            return a == (*items)[b];
        }
    };

    // Unified store of interned items, indexed by Ref. Single source of truth.
    std::vector<Item> items_;
    // Dedup index: u32 handles into items_, hashed/compared via the store.
    ankerl::unordered_dense::set<uint32_t, Hash, Eq> set_;
};

} // namespace clotho::ir
