#include "Ir.h"

namespace clotho::ir {

Inst::PayloadKind payloadKind(Inst::Tag tag) {
    using T = Inst::Tag;
    using K = Inst::PayloadKind;
    switch (tag) {
        case T::constant:
            return K::ref;
        case T::add:
            return K::binary;
        case T::call:
            return K::unary_extra;
        case T::ret:
            return K::unary;
    }

    return K::none;
}

std::span<const Index> Ir::operands(Index inst, std::array<Index, 2>& scratch) const {
    const auto  tag = this->tag(inst);
    const auto& p   = payload(inst);

    switch (payloadKind(tag)) {
        case Inst::PayloadKind::none:
        case Inst::PayloadKind::ref:
            return {};

        case Inst::PayloadKind::unary:
            scratch[0] = p.unary;
            return { scratch.data(), 1 };

        case Inst::PayloadKind::binary:
            scratch[0] = p.binary.l;
            scratch[1] = p.binary.r;
            return { scratch.data(), 2 };

        case Inst::PayloadKind::unary_extra: {
            // call: variadic args live in the extra buffer.
            const auto slice = extraData<ExtraSlice>(p.unary_extra.extra);
            return indexSlice(slice);
        }
    }

    return {};
}

} // namespace clotho::ir
