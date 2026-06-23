#include "InternPool.h"

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
    void_type  = put(Type{ .kind = Type::Kind::Void });
    logic_type = put(Type{ .kind = Type::Kind::Logic });
    bit_type   = put(Type{ .kind = Type::Kind::Bit });
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
    const Layout lay = layout(type);
    const auto   pb  = plane_bytes(lay.width);

    const uint64_t mask = lay.width >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << lay.width) - 1);
    const uint64_t val  = bits & mask;

    // Val plane holds the known bits; any xz plane stays zero (all bits known).
    std::array<uint8_t, 16> buf{};
    std::memcpy(buf.data(), &val, pb);

    return put_value(type, { buf.data(), lay.byte_count });
}

const Item& InternPool::get(Ref ref) const {
    return items_[static_cast<uint32_t>(ref)];
}

std::span<const uint8_t> InternPool::bytes(Ref ref) const {
    return value_bytes(std::get<Value>(get(ref)), value_bytes_);
}

Layout InternPool::layout(Ref type) const {
    const Type& t = std::get<Type>(get(type));
    switch (t.kind) {
        case Type::Kind::Void:
            return { Domain::Two, 0, 0 };
        case Type::Kind::Logic:
            return { Domain::Four, 1, plane_bytes(1) * 2 };
        case Type::Kind::Bit:
            return { Domain::Two, 1, plane_bytes(1) };
        case Type::Kind::PackedArray: {
            const Layout   el    = layout(t.element);
            const uint32_t count = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            const uint32_t width = count * el.width;
            const auto     bytes = static_cast<uint32_t>(static_cast<uint8_t>(el.domain)) * plane_bytes(width);
            return { el.domain, width, bytes };
        }
        case Type::Kind::UnpackedArray:
            // Aggregate values (row-major element blobs) are not interned yet.
            return { Domain::Two, 0, 0 };
    }

    return { Domain::Two, 0, 0 };
}

} // namespace clotho::ir
