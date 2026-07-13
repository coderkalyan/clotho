#include "Ir.h"

namespace clotho::ir {

std::span<const Index> Ir::operands(Index inst, std::array<Index, 3>& scratch) const {
    const auto  tag = this->tag(inst);
    const auto& p   = payload(inst);

    switch (payload_kind(tag)) {
        case Inst::PayloadKind::None:
        case Inst::PayloadKind::Scale:
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
            const auto info = extra_data<DisplayInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Severity: {
            // severity: the message value args are an index slice in the
            // SeverityInfo record; the format string and banner are not operands.
            const auto info = extra_data<SeverityInfo>(p.extra);
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

        case Inst::PayloadKind::FnUnary:
            scratch[0] = p.fn_unary.op;
            return { scratch.data(), 1 };

        case Inst::PayloadKind::TimeFormat: {
            // timeformat: units / precision / min_width are SSA operands; the
            // suffix bytes are not.
            const auto info = extra_data<TimeFormatInfo>(p.extra);
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
            // dist_unary / dist_binary expose the seed word plus the
            // distribution parameter(s); the DistOp selector is not an operand.
            if (tag == Inst::Tag::DistUnary) {
                const auto d = extra_data<DistUnaryInfo>(p.op_extra.extra);
                scratch[0]   = p.op_extra.op;
                scratch[1]   = d.p1;
                return { scratch.data(), 2 };
            }
            if (tag == Inst::Tag::DistBinary) {
                const auto d = extra_data<DistBinaryInfo>(p.op_extra.extra);
                scratch[0]   = p.op_extra.op;
                scratch[1]   = d.p1;
                scratch[2]   = d.p2;
                return { scratch.data(), 3 };
            }
            // real_math_binary exposes its two real operands; the routine
            // selector is not an operand.
            if (tag == Inst::Tag::RealMathBinary) {
                const auto rm = extra_data<RealMathBinaryInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = rm.rhs;
                return { scratch.data(), 2 };
            }
            // fseek exposes its descriptor plus the offset / operation operands.
            if (tag == Inst::Tag::Fseek) {
                const auto fs = extra_data<FseekInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = fs.offset;
                scratch[2]    = fs.operation;
                return { scratch.data(), 3 };
            }
            // dyn_part_select exposes its base and the runtime offset value; the
            // constant width is not an operand.
            if (tag == Inst::Tag::DynPartSelect) {
                const auto dps = extra_data<DynPartSelect>(p.op_extra.extra);
                scratch[0]     = p.op_extra.op;
                scratch[1]     = dps.offset;
                return { scratch.data(), 2 };
            }
            // fread_mem exposes its fd value plus the optional start / count
            // operands; the target memory is a Signal handle, not an operand.
            if (tag == Inst::Tag::FreadMem) {
                const auto fm = extra_data<FreadMemInfo>(p.op_extra.extra);
                size_t     n  = 0;
                scratch[n++]  = p.op_extra.op;
                if (fm.has_start) {
                    scratch[n++] = fm.start;
                }
                if (fm.has_count) {
                    scratch[n++] = fm.count;
                }
                return { scratch.data(), n };
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
            // store_nb_delay exposes its lvalue, the sampled value, and the delay.
            if (tag == Inst::Tag::StoreNbDelay) {
                const auto info = extra_data<StoreNbDelayInfo>(p.op_extra.extra);
                scratch[0]      = p.op_extra.op;
                scratch[1]      = info.value;
                scratch[2]      = info.delay;
                return { scratch.data(), 3 };
            }
            // store_nb_event exposes its lvalue, the sampled value, and the count
            // (the trigger signal is a subscription target, not an SSA operand).
            if (tag == Inst::Tag::StoreNbEvent) {
                const auto info = extra_data<StoreNbEventInfo>(p.op_extra.extra);
                scratch[0]      = p.op_extra.op;
                scratch[1]      = info.value;
                scratch[2]      = info.count;
                return { scratch.data(), 3 };
            }
            if (tag == Inst::Tag::AggCopy) {
                // Both the destination (op) and source lvalues are referenced.
                const auto info = extra_data<AggCopyInfo>(p.op_extra.extra);
                scratch[0]      = p.op_extra.op;
                scratch[1]      = info.src;
                return { scratch.data(), 2 };
            }
            // dyn_array_new / dyn_array_copy expose the handle lvalue plus
            // their DynArrayArg operand (element count / copy source).
            if (tag == Inst::Tag::DynArrayNew || tag == Inst::Tag::DynArrayCopy) {
                const auto da = extra_data<DynArrayArg>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = da.arg;
                return { scratch.data(), 2 };
            }
            // dyn_array_new_init exposes the handle lvalue, element count, and
            // the copy-source handle lvalue.
            if (tag == Inst::Tag::DynArrayNewInit) {
                const auto da = extra_data<DynArrayNewInitInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = da.size;
                scratch[2]    = da.src;
                return { scratch.data(), 3 };
            }
            // The one-extra-operand queue ops expose the handle lvalue plus
            // their QueueArg operand (element value / position / copy source).
            if (tag == Inst::Tag::QueuePushBack || tag == Inst::Tag::QueuePushFront ||
                tag == Inst::Tag::QueueDeleteIdx || tag == Inst::Tag::QueueCopy) {
                const auto qa = extra_data<QueueArg>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = qa.arg;
                return { scratch.data(), 2 };
            }
            // queue_insert exposes the handle lvalue, position, and element value.
            if (tag == Inst::Tag::QueueInsert) {
                const auto qi = extra_data<QueueInsertInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = qi.idx;
                scratch[2]    = qi.val;
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
            case Inst::Tag::MemberSelect:
                lv    = ir.payload(lv).op_extra.op;
                whole = false;
                break;
            default:
                return { lv, whole };
        }
    }
}

} // namespace clotho::ir
