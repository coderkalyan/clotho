#include "Ir.h"

namespace clotho::ir {

Inst::PayloadKind payload_kind(Inst::Tag tag) {
    using T = Inst::Tag;
    using K = Inst::PayloadKind;
    switch (tag) {
        case T::Constant:
            return K::Ref;
        case T::Signal:
            return K::Signal;
        case T::Arg:
            return K::Arg;

        case T::Load:
        case T::Bnot:
        case T::RedAnd:
        case T::RedOr:
        case T::RedXor:
        case T::Lnot:
        case T::Ret:
            return K::Unary;

        case T::BitSelect:
        case T::Store:
        case T::StoreNb:
        case T::Add:
        case T::Sub:
        case T::Mul:
        case T::Band:
        case T::Bor:
        case T::Bxor:
        case T::Sll:
        case T::Srl:
        case T::Sra:
        case T::Eq:
        case T::Ne:
        case T::Lt:
        case T::Le:
        case T::Gt:
        case T::Ge:
        case T::Land:
        case T::Lor:
            return K::Binary;

        case T::Resize:
            return K::OpType;
        case T::Repl:
            return K::Repl;
        case T::Concat:
            return K::Extra;
        case T::Display:
            return K::Display;

        case T::Mux:
        case T::PartSelect:
        case T::Cond:
            return K::OpExtra;
    }

    return K::None;
}

std::span<const Index> Ir::operands(Index inst, std::array<Index, 3>& scratch) const {
    const auto  tag = this->tag(inst);
    const auto& p   = payload(inst);

    switch (payload_kind(tag)) {
        case Inst::PayloadKind::None:
        case Inst::PayloadKind::Ref:
        case Inst::PayloadKind::Signal:
        case Inst::PayloadKind::Arg:
            return {};

        case Inst::PayloadKind::Unary:
            scratch[0] = p.unary;
            return { scratch.data(), 1 };

        case Inst::PayloadKind::OpType:
            scratch[0] = p.op_type.op;
            return { scratch.data(), 1 };

        case Inst::PayloadKind::Repl:
            scratch[0] = p.repl.value;
            return { scratch.data(), 1 };

        case Inst::PayloadKind::Binary:
            scratch[0] = p.binary.l;
            scratch[1] = p.binary.r;
            return { scratch.data(), 2 };

        case Inst::PayloadKind::Extra: {
            // concat: variadic operands live in the extra buffer.
            const auto slice = extra_data<ExtraSlice>(p.extra);
            return index_slice(slice);
        }

        case Inst::PayloadKind::Display: {
            // display: the value args are an index slice in the DisplayInfo record;
            // the format string is not an SSA operand.
            const auto info = extra_data<DisplayInfo>(p.display);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::OpExtra:
            // mux exposes cond + the two selected values; cond / part_select
            // expose only their primary operand (regions / bounds aren't SSA
            // operands).
            if (tag == Inst::Tag::Mux) {
                const auto mux = extra_data<Mux>(p.op_extra.extra);
                scratch[0]     = p.op_extra.op;
                scratch[1]     = mux.a;
                scratch[2]     = mux.b;
                return { scratch.data(), 3 };
            }
            scratch[0] = p.op_extra.op;
            return { scratch.data(), 1 };
    }

    return {};
}

} // namespace clotho::ir
