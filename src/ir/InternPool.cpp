#include "InternPool.h"

namespace clotho::ir {

InternPool::InternPool()
  : set_(0, Hash{ &items_ }, Eq{ &items_ }) {
    int_type   = put(Type{ .kind = Type::Kind::int_ });
    float_type = put(Type{ .kind = Type::Kind::float_ });
}

Ref InternPool::put(const Item& item) {
    if (auto it = set_.find(item); it != set_.end()) {
        return static_cast<Ref>(*it);
    }

    const auto index = static_cast<uint32_t>(items_.size());
    items_.push_back(item);
    set_.insert(index);
    return static_cast<Ref>(index);
}

const Item& InternPool::get(Ref ref) const {
    return items_[static_cast<uint32_t>(ref)];
}

} // namespace clotho::ir
