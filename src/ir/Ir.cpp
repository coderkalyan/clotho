#include "Ir.h"

namespace clotho::ir {

Inst::PayloadKind payload_kind(Inst::Tag tag) {
    using T = Inst::Tag;
    using K = Inst::PayloadKind;
    switch (tag) {
        case T::Constant:
            return K::Ref;
        case T::Signal:
        case T::EventTrigger:
        case T::EventTriggered:
        case T::EventTriggerNb:
            return K::Signal;
        case T::Arg:
            return K::Arg;
        case T::Local:
            return K::Local;

        case T::Load:
        case T::Bnot:
        case T::RedAnd:
        case T::RedOr:
        case T::RedXor:
        case T::Lnot:
        case T::FNeg:
        case T::Ret:
        case T::Delay:
        case T::Fclose:
        case T::Fflush:
        case T::DumpFile:
        case T::StrLen:
        case T::StrUpper:
        case T::StrLower:
        case T::StrFromPacked:
        case T::StrAtoi:
        case T::StrAtohex:
        case T::StrAtooct:
        case T::StrAtobin:
        case T::StrAtoreal:
        case T::Clog2:
        case T::Ctz:
        case T::Clz:
            return K::Unary;

        case T::BitSelect:
        case T::Store:
        case T::StoreNb:
        case T::Add:
        case T::Sub:
        case T::Mul:
        case T::Div:
        case T::Mod:
        case T::Pow:
        case T::Band:
        case T::Bor:
        case T::Bxor:
        case T::Bxnor:
        case T::Sll:
        case T::Srl:
        case T::Sra:
        case T::Eq:
        case T::Ne:
        case T::Lt:
        case T::Le:
        case T::Gt:
        case T::Ge:
        case T::CaseEq:
        case T::CasezEq:
        case T::CasexEq:
        case T::WildEq:
        case T::Land:
        case T::Lor:
        case T::StrGetc:
        case T::StrCmp:
        case T::StrICmp:
        case T::StrItoa:
        case T::StrHextoa:
        case T::StrOctoa:
        case T::StrBintoa:
        case T::StrRealtoa:
        case T::FAdd:
        case T::FSub:
        case T::FMul:
        case T::FDiv:
        case T::FPow:
        case T::FEq:
        case T::FNe:
        case T::FLt:
        case T::FLe:
        case T::FGt:
        case T::FGe:
            return K::Binary;

        case T::Resize:
        case T::StrToPacked:
        case T::ItoF:
        case T::FtoI:
        case T::FtoITrunc:
        case T::FBitcast:
        case T::FResize:
            return K::OpType;
        case T::Repl:
            return K::Repl;
        case T::Concat:
        case T::Loop:
            return K::Extra;
        case T::Call:
            return K::Call;
        case T::TaskCall:
            return K::TaskCall;
        case T::Display:
            return K::Display;
        case T::FileValueOp:
            return K::FileOp;
        case T::Sformat:
            return K::Sformat;
        case T::Sformatf:
            return K::Sformatf;
        case T::Scanf:
            return K::Scanf;
        case T::RealMath:
            return K::RealMath;
        case T::TimeFormat:
            return K::TimeFormat;
        case T::Break:
        case T::Continue:
        case T::DumpVars:
        case T::DumpOff:
        case T::DumpOn:
        case T::DumpAll:
        case T::DumpFlush:
        case T::SimTime:
        case T::Random:
            return K::None;
        case T::Finish:
        case T::CaseViol:
        case T::TestPlusArgs:
            return K::Extra;
        case T::Severity:
            return K::Severity;
        case T::Wait:
            return K::Wait;

        case T::Select:
        case T::SelectLazy:
        case T::PartSelect:
        case T::DynPartSelect:
        case T::ElemSelect:
        case T::Cond:
        case T::Switch:
        case T::Fopen:
        case T::Fgets:
        case T::Ferror:
        case T::FreadWord:
        case T::FreadMem:
        case T::ReadMem:
        case T::StrPutc:
        case T::StrSubstr:
        case T::EnumName:
        case T::CountBits:
        case T::ValuePlusArgs:
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
        case Inst::PayloadKind::Local:
        case Inst::PayloadKind::Wait:
            return {};

        case Inst::PayloadKind::Call: {
            // call: its arguments are the SSA operands; the callee handle is not.
            const auto ci = extra_data<CallInfo>(p.extra);
            return index_slice(ci.args);
        }

        case Inst::PayloadKind::TaskCall: {
            // task_call: its per-formal operands (values and lvalue temps) are the
            // SSA operands; directions / the callee handle are not.
            const auto ti = extra_data<TaskCallInfo>(p.extra);
            return index_slice(ti.args);
        }

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
            // loop: its predicate and regions are not top-level SSA operands
            // (the predicate lives inside the cond region, walked via region()).
            // finish: its FinishInfo carries only the diag level and banner bytes,
            // no SSA operands. test_plusargs: its TestPlusArgsInfo carries only
            // the query-string bytes, no SSA operands.
            if (tag == Inst::Tag::Loop || tag == Inst::Tag::Finish ||
                tag == Inst::Tag::TestPlusArgs) {
                return {};
            }
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

        case Inst::PayloadKind::Severity: {
            // severity: the message value args are an index slice in the
            // SeverityInfo record; the format string and banner are not operands.
            const auto info = extra_data<SeverityInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::FileOp: {
            // file_value_op: the value args live in the FileOpInfo record; the
            // opcode is not an SSA operand.
            const auto info = extra_data<FileOpInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Sformat: {
            // sformat: the value args live in the SformatInfo record; the format
            // bytes and the destination lvalue are not value operands.
            const auto info = extra_data<SformatInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Sformatf: {
            // sformatf: the value args live in the SformatfInfo record; the
            // format bytes are not value operands.
            const auto info = extra_data<SformatfInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Scanf: {
            // scanf: the output lvalue operands live in the ScanfInfo record; the
            // format bytes and the source operand are read directly.
            const auto info = extra_data<ScanfInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::RealMath: {
            // real_math: the value args live in the RealMathInfo record; the
            // opcode is not an SSA operand.
            const auto info = extra_data<RealMathInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::TimeFormat: {
            // timeformat: units / precision / min_width are SSA operands; the
            // suffix bytes are not.
            const auto info = extra_data<TimeFormatInfo>(p.timeformat);
            scratch[0]      = info.units;
            scratch[1]      = info.precision;
            scratch[2]      = info.min_width;
            return { scratch.data(), 3 };
        }

        case Inst::PayloadKind::OpExtra:
            // select exposes cond + the two selected values; cond / part_select
            // expose only their primary operand (regions / bounds aren't SSA
            // operands).
            if (tag == Inst::Tag::Select) {
                const auto select = extra_data<Select>(p.op_extra.extra);
                scratch[0]     = p.op_extra.op;
                scratch[1]     = select.a;
                scratch[2]     = select.b;
                return { scratch.data(), 3 };
            }
            // elem_select exposes its base and the index value (the index is read
            // to compute the element address); the array-type handle is not an
            // operand. The bounds of a part_select / regions of a cond are not.
            if (tag == Inst::Tag::ElemSelect) {
                const auto es = extra_data<ElemSelect>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = es.index;
                return { scratch.data(), 2 };
            }
            // dyn_part_select exposes its base and the runtime offset value; the
            // constant width is not an operand.
            if (tag == Inst::Tag::DynPartSelect) {
                const auto dps = extra_data<DynPartSelect>(p.op_extra.extra);
                scratch[0]     = p.op_extra.op;
                scratch[1]     = dps.offset;
                return { scratch.data(), 2 };
            }
            // read_mem exposes its filename value plus the optional start / finish
            // address operands; the target memory is a Signal handle, not an operand.
            if (tag == Inst::Tag::ReadMem) {
                const auto rm = extra_data<ReadMemInfo>(p.op_extra.extra);
                size_t     n  = 0;
                scratch[n++]  = p.op_extra.op;
                if (rm.has_start) {
                    scratch[n++] = rm.start;
                }
                if (rm.has_finish) {
                    scratch[n++] = rm.finish;
                }
                return { scratch.data(), n };
            }
            // str_putc / str_substr expose the string op plus their two extra
            // value operands (index+char / start+end).
            if (tag == Inst::Tag::StrPutc) {
                const auto sp = extra_data<StrPutcInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = sp.index;
                scratch[2]    = sp.ch;
                return { scratch.data(), 3 };
            }
            if (tag == Inst::Tag::StrSubstr) {
                const auto ss = extra_data<StrSubstrInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = ss.i;
                scratch[2]    = ss.j;
                return { scratch.data(), 3 };
            }
            scratch[0] = p.op_extra.op;
            return { scratch.data(), 1 };
    }

    return {};
}

LvalueRoot lvalue_root(const Ir& ir, Index lv) {
    bool whole = true;
    for (;;) {
        switch (ir.tag(lv)) {
            case Inst::Tag::BitSelect:
                lv    = ir.payload(lv).binary.l;
                whole = false;
                break;
            case Inst::Tag::PartSelect:
            case Inst::Tag::DynPartSelect:
            case Inst::Tag::ElemSelect:
                lv    = ir.payload(lv).op_extra.op;
                whole = false;
                break;
            default:
                return { lv, whole };
        }
    }
}

} // namespace clotho::ir
