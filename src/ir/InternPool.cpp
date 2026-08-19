#include "InternPool.h"

#include <bit>
#include <cassert>
#include <cstring>

// Value encoding (canonical, shared by interned constants and runtime state):
// a width-W value is `domain` parallel packed planes, val plane first, each
// plane_bytes(W) wide, both stored contiguously. Bits above W are zero in every
// plane (canonical padding) so equality / hashing are mask-free byte compares.
//   - 2-state (Two):  1 plane  — val.
//   - 4-state (Four): 2 planes — val, xz (VPI: 0=(0,0) 1=(1,0) X=(1,1) Z=(0,1)).
//   - 9-state (Nine): 4 planes — a 4-bit per-bit code, bit-sliced LSB-first.
//     Provisional code: U=0 X=1 0=2 1=3 Z=4 W=5 L=6 H=7 '-'=8. Refined later.

namespace clotho::ir {

InternPool::InternPool()
  : set_(0, Hash{ &items_, &value_bytes_ }, Eq{ &items_, &value_bytes_ }) {
    void_type      = put(Type{ .kind = Type::Kind::Void });
    logic_type     = put(Type{ .kind = Type::Kind::Logic });
    bit_type       = put(Type{ .kind = Type::Kind::Bit });
    string_type    = put(Type{ .kind = Type::Kind::String });
    event_type     = put(Type{ .kind = Type::Kind::Event });
    viface_type    = put(Type{ .kind = Type::Kind::VirtualIface });
    real_type      = put(Type{ .kind = Type::Kind::Real });
    shortreal_type = put(Type{ .kind = Type::Kind::ShortReal });
}

Ref InternPool::put(const Item& item) {
    if (auto it = set_.find(view_of(item, value_bytes_)); it != set_.end()) {
        return static_cast<Ref>(*it);
    }
    const auto index = static_cast<uint32_t>(items_.size());
    items_.push_back(item);
    set_.insert(index);

    return static_cast<Ref>(index);
}

Ref InternPool::put_value(Ref type, std::span<const uint8_t> bytes) {
    // Probe by content before touching the store — a duplicate mutates nothing.
    const ItemView probe{ .is_type = false, .value_type = type, .value_bytes = bytes };
    if (auto it = set_.find(probe); it != set_.end()) {
        return static_cast<Ref>(*it);
    }

    Value v;
    v.type       = type;
    v.byte_count = static_cast<uint32_t>(bytes.size());
    if (v.byte_count <= kInlineValueBytes) {
        std::memcpy(v.data.inline_bytes, bytes.data(), v.byte_count);
    } else {
        v.data.spill_offset = static_cast<uint32_t>(value_bytes_.size());
        value_bytes_.insert(value_bytes_.end(), bytes.begin(), bytes.end());
    }

    const auto index = static_cast<uint32_t>(items_.size());
    items_.push_back(Item{ v });
    set_.insert(index);

    return static_cast<Ref>(index);
}

Ref InternPool::put_int(Ref type, uint64_t bits) {
    // A Real / ShortReal bit pattern must come through put_real; an integer
    // interned against a float type would alias the wrong canonical bytes.
    assert(!is_float(type) && "put_int on a floating type");
    const Layout lay = layout(type);
    const auto   pb  = plane_bytes(lay.width);

    const uint64_t mask = lay.width >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << lay.width) - 1);
    const uint64_t val  = bits & mask;

    // Val plane holds the known bits, zero-extended to the plane width; any xz
    // plane stays zero (all bits known). A >64-bit type needs a plane wider
    // than the source word, so only the low sizeof(val) bytes are copied and a
    // wide value takes the heap buffer.
    const size_t copy = std::min<size_t>(pb, sizeof(val));
    if (lay.byte_count <= 16) {
        std::array<uint8_t, 16> buf{};
        std::memcpy(buf.data(), &val, copy);
        return put_value(type, { buf.data(), lay.byte_count });
    }
    std::vector<uint8_t> buf(lay.byte_count, 0);
    std::memcpy(buf.data(), &val, copy);

    return put_value(type, buf);
}

Ref InternPool::put_real(Ref type, double value) {
    assert(is_float(type) && "put_real on a non-floating type");
    const Layout lay = layout(type);

    // Canonical bytes are the IEEE-754 bit pattern: 64-bit for Real, rounded
    // to the nearest float (C cast semantics, LRM 20.5 NOTE 1) for ShortReal.
    std::array<uint8_t, 8> buf{};
    if (lay.width == 64) {
        const uint64_t pat = std::bit_cast<uint64_t>(value);
        std::memcpy(buf.data(), &pat, 8);
    } else {
        const uint32_t pat = std::bit_cast<uint32_t>(static_cast<float>(value));
        std::memcpy(buf.data(), &pat, 4);
    }

    return put_value(type, { buf.data(), lay.byte_count });
}

bool InternPool::is_float(Ref type) const {
    const Type& t = std::get<Type>(get(type));

    return t.kind == Type::Kind::Real || t.kind == Type::Kind::ShortReal;
}

const Item& InternPool::get(Ref ref) const {
    return items_[static_cast<uint32_t>(ref)];
}

std::span<const uint8_t> InternPool::bytes(Ref ref) const {
    return value_bytes(std::get<Value>(get(ref)), value_bytes_);
}

Layout InternPool::layout(Ref type) const {
    const auto idx = static_cast<uint32_t>(type);
    if (idx < layout_valid_.size() && layout_valid_[idx] != 0) {
        return layout_cache_[idx];
    }
    const Layout lay = compute_layout(type);
    if (layout_cache_.size() < items_.size()) {
        layout_cache_.resize(items_.size());
        layout_valid_.resize(items_.size(), 0);
    }
    layout_cache_[idx] = lay;
    layout_valid_[idx] = 1;

    return lay;
}

Layout InternPool::compute_layout(Ref type) const {
    const Type& t = std::get<Type>(get(type));
    switch (t.kind) {
        case Type::Kind::Void:
            return { Domain::Two, 0, 0 };
        case Type::Kind::Logic:
            return { Domain::Four, 1, plane_bytes(1) * 2 };
        case Type::Kind::Bit:
            return { Domain::Two, 1, plane_bytes(1) };
        case Type::Kind::Real:
            return { Domain::Two, 64, plane_bytes(64) };
        case Type::Kind::ShortReal:
            return { Domain::Two, 32, plane_bytes(32) };
        case Type::Kind::PackedArray: {
            const Layout   el    = layout(t.element);
            const uint32_t count = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            const uint32_t width = count * el.width;
            const auto     bytes = static_cast<uint32_t>(static_cast<uint8_t>(el.domain)) * plane_bytes(width);
            return { el.domain, width, bytes };
        }
        case Type::Kind::String:
            // Not plane-sized: a string is an opaque runtime handle, and an
            // interned string constant's bytes are its characters (any length).
            // Width 0 is the sentinel; consumers branch on the kind first.
            return { Domain::Two, 0, 0 };
        case Type::Kind::DynArray:
            // Not plane-sized: an opaque rt_dynarr handle. Width 0 is the
            // sentinel; the backend queries `element` for per-element sizing.
            return { Domain::Two, 0, 0 };
        case Type::Kind::Queue:
            // Not plane-sized: an opaque rt_queue ring handle. Width 0 is the
            // sentinel; the backend queries `element` for per-element sizing.
            return { Domain::Two, 0, 0 };
        case Type::Kind::Assoc:
            // Not plane-sized: an opaque rt_assoc handle. Width 0 is the
            // sentinel; the backend queries `element`/`fields_id` for the value
            // and key types.
            return { Domain::Two, 0, 0 };
        case Type::Kind::Class:
            // A class variable holds a handle: the pointer to a heap object body,
            // stored as a 2-state 64-bit value (identical storage/assign/compare
            // to chandle). The object body layout is backend-owned (built from the
            // property table `fields_id`), reached by dereferencing the handle.
            return { Domain::Two, 64, plane_bytes(64) };
        case Type::Kind::Event:
            // A named event variable holds a handle: { u32 slot, u32 gen } naming an
            // entry in the backend's event object array, stored as one 2-state
            // 64-bit value. Identical storage / assign / compare to a class handle,
            // which is what makes every container and ABI path work unchanged.
            return { Domain::Two, 64, plane_bytes(64) };
        case Type::Kind::VirtualIface:
            // A virtual interface variable holds a handle to one interface
            // instance's runtime context record, stored as one 2-state 64-bit
            // value -- the class / event handle shape, for the same reason.
            return { Domain::Two, 64, plane_bytes(64) };
        case Type::Kind::UnpackedArray: {
            // Report the leaf element's domain / width (recursion drills through
            // nested arrays), so callers sizing a per-element access see the
            // element, not a flattened whole. `byte_count` is the flattened
            // canonical-value size (count * element bytes); runtime element
            // storage uses the backend's own per-element slot sizing, which
            // differs from this packed-value layout.
            const Layout   el    = layout(t.element);
            const uint32_t count = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            return { el.domain, el.width, count * el.byte_count };
        }
        case Type::Kind::Struct: {
            if (!t.is_packed) {
                // Aggregate storage, no plane-packed value (mirrors String's
                // width-0 sentinel) — the backend builds its own layout.
                return { Domain::Two, 0, 0 };
            }
            const Layout   el    = layout(t.element);
            const uint32_t width = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            const auto     bytes = static_cast<uint32_t>(static_cast<uint8_t>(el.domain)) * plane_bytes(width);
            return { el.domain, width, bytes };
        }
    }

    return { Domain::Two, 0, 0 };
}

uint32_t InternPool::intern_struct_fields(std::vector<FieldRow> rows) {
    field_runs_.emplace_back(0, 0);
    const auto id = static_cast<uint32_t>(field_runs_.size() - 1);
    set_struct_fields(id, std::move(rows));

    return id;
}

uint32_t InternPool::reserve_struct_fields() {
    // A placeholder run; the rows (and their real (start, count)) are appended by
    // set_struct_fields once the class's properties are lowered. Empty until then.
    field_runs_.emplace_back(0, 0);

    return static_cast<uint32_t>(field_runs_.size() - 1);
}

void InternPool::set_struct_fields(uint32_t fields_id, std::vector<FieldRow> rows) {
    const auto start = static_cast<uint32_t>(struct_fields_.size());
    for (const FieldRow& r : rows) {
        const auto name_at = static_cast<uint32_t>(field_names_.size());
        field_names_.insert(field_names_.end(), r.name.begin(), r.name.end());
        struct_fields_.push_back({ name_at, static_cast<uint32_t>(r.name.size()), r.type, r.bit_offset });
    }
    field_runs_[fields_id] = { start, static_cast<uint32_t>(rows.size()) };
}

std::span<const Field> InternPool::struct_fields(uint32_t fields_id) const {
    const auto [start, count] = field_runs_[fields_id];

    return { struct_fields_.data() + start, count };
}

} // namespace clotho::ir
