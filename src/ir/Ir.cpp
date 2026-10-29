#include "Ir.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace clotho::ir {

std::span<const Index> Ir::operands(Index inst, std::array<Index, 4>& scratch) const {
    const auto  tag = this->tag(inst);
    const auto& p   = payload(inst);

    switch (payload_kind(tag)) {
        case Inst::PayloadKind::None:
        case Inst::PayloadKind::Label:
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

        case Inst::PayloadKind::CallVirtual: {
            // call_virtual: its arguments (this handle + formals) are the SSA
            // operands; the slot / class / signature-func are not.
            const auto vci = extra_data<VirtualCallInfo>(p.extra);
            return index_slice(vci.args);
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

        case Inst::PayloadKind::Extra:
            // Which record `extra` points at is tag-dependent, so the shape
            // table decides; an unclassified tag yields no operands rather than
            // a span decoded from the wrong record.
            switch (extra_operand_shape(tag)) {
                case ExtraOperandShape::None:
                    return {};
                case ExtraOperandShape::StrengthResValues: {
                    // strength_res: the driver slot values are the SSA operands;
                    // the parallel strength constants are not.
                    const auto info = extra_data<StrengthResInfo>(p.extra);
                    return index_slice(info.values);
                }
                case ExtraOperandShape::SliceIsOperands: {
                    // concat: variadic operands live in the extra buffer.
                    const auto slice = extra_data<ExtraSlice>(p.extra);
                    return index_slice(slice);
                }
                case ExtraOperandShape::ForkArgs: {
                    // fork: the captured frame lvalues each child aliases through a
                    // ref formal are the SSA operands; the children are FunctionIds.
                    const auto info = extra_data<ForkInfo>(p.extra);
                    return index_slice(info.args);
                }
                case ExtraOperandShape::ProcArgs: {
                    // process control (LRM 9.7): the receiver handle plus any value
                    // argument; the op selector and the result type are not operands.
                    const auto info = extra_data<ProcCtlInfo>(p.extra);
                    return index_slice(info.args);
                }
                case ExtraOperandShape::SyncArgs: {
                    // wait queue (LRM 15.3 / 15.4): the queue's head-word and
                    // tail-word lvalues; the site id and op selector are not.
                    const auto info = extra_data<SyncInfo>(p.extra);
                    return index_slice(info.args);
                }
                case ExtraOperandShape::CoverArgs: {
                    // functional coverage (LRM 19): the covergroup handle plus the
                    // op's value arguments; the type and coverpoint indices, the op
                    // selector and the result type are not operands.
                    const auto info = extra_data<CoverInfo>(p.extra);
                    return index_slice(info.args);
                }
                case ExtraOperandShape::NotExtra:
                    break;
            }

            return {};

        case Inst::PayloadKind::Display: {
            // display: the value args are an index slice in the DisplayInfo record;
            // the format string is not an SSA operand.
            const auto info = extra_data<DisplayInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::FDisplay: {
            // fdisplay: like display; the descriptor (op_extra.op) cannot ride
            // the same span as the variadic args (see the header NOTE).
            const auto info = extra_data<DisplayInfo>(p.op_extra.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Severity: {
            // severity: the message value args are an index slice in the
            // SeverityInfo record; the format string and banner are not operands.
            const auto info = extra_data<SeverityInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Sformat: {
            // sformat: the value args live in the SformatInfo record. The format
            // bytes are not operands, the destination lvalue and a runtime format
            // value are (both held back to the tail; see for_each_operand).
            const auto info = extra_data<SformatInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Sformatf: {
            // sformatf: the value args live in the SformatfInfo record. The format
            // bytes are not operands; a runtime format value is, held back to the
            // tail.
            const auto info = extra_data<SformatfInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::Scanf: {
            // scanf: the output lvalue operands live in the ScanfInfo record; the
            // format bytes and the source operand are read directly.
            const auto info = extra_data<ScanfInfo>(p.extra);
            return index_slice(info.args);
        }

        case Inst::PayloadKind::OpImm:
            scratch[0] = p.op_imm.op;
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
                scratch[0]        = p.op_extra.op;
                scratch[1]        = select.a;
                scratch[2]        = select.b;
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
            // pow exposes its base and the exponent value; the signedness flags
            // are not operands.
            if (tag == Inst::Tag::Pow) {
                const auto pw = extra_data<PowInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = pw.exp;
                return { scratch.data(), 2 };
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
            // store_nb_delay / store_inertial_delay expose their lvalue, the
            // (sampled / freshly-evaluated) value, and the delay.
            if (tag == Inst::Tag::StoreNbDelay || tag == Inst::Tag::StoreInertialDelay) {
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
            if (tag == Inst::Tag::AggEq) {
                // Both compared aggregate lvalues are referenced.
                const auto info = extra_data<AggEqInfo>(p.op_extra.extra);
                scratch[0]      = p.op_extra.op;
                scratch[1]      = info.rhs;
                return { scratch.data(), 2 };
            }
            // dyn_array_new / dyn_array_copy / dyn_array_copy_queue expose the
            // handle lvalue plus their DynArrayArg operand (element count /
            // copy source).
            if (tag == Inst::Tag::DynArrayNew || tag == Inst::Tag::DynArrayCopy ||
                tag == Inst::Tag::DynArrayCopyQueue) {
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
                tag == Inst::Tag::QueueDeleteIdx || tag == Inst::Tag::QueueCopy || tag == Inst::Tag::HandleCopyNb ||
                tag == Inst::Tag::QueueCopyDyn) {
                const auto qa = extra_data<QueueArg>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = qa.arg;
                return { scratch.data(), 2 };
            }
            // queue_append_range exposes the dst handle lvalue, the src handle
            // lvalue, and the low / high index operands.
            if (tag == Inst::Tag::QueueAppendRange) {
                const auto qs = extra_data<QueueSliceInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = qs.src;
                scratch[2]    = qs.lo;
                scratch[3]    = qs.hi;
                return { scratch.data(), 4 };
            }
            // queue_insert exposes the handle lvalue, position, and element value.
            if (tag == Inst::Tag::QueueInsert) {
                const auto qi = extra_data<QueueInsertInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = qi.idx;
                scratch[2]    = qi.val;
                return { scratch.data(), 3 };
            }
            // The one-extra-operand associative-array ops expose the handle
            // lvalue plus their AssocArg operand (key value / key lvalue / copy
            // source).
            if (tag == Inst::Tag::AssocSelect || tag == Inst::Tag::AssocExists || tag == Inst::Tag::AssocDeleteKey ||
                tag == Inst::Tag::AssocCopy || tag == Inst::Tag::AssocFirst || tag == Inst::Tag::AssocLast ||
                tag == Inst::Tag::AssocNext || tag == Inst::Tag::AssocPrev) {
                const auto aa = extra_data<AssocArg>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = aa.arg;
                return { scratch.data(), 2 };
            }
            // randc_next exposes both lvalues it advances: the property's
            // used-this-cycle bitmap and the object's RNG state. `nvals` is a
            // compile-time bound, not an operand.
            if (tag == Inst::Tag::RandcNext) {
                const auto rc = extra_data<RandcInfo>(p.op_extra.extra);
                scratch[0]    = p.op_extra.op;
                scratch[1]    = rc.rng;
                return { scratch.data(), 2 };
            }
            scratch[0] = p.op_extra.op;
            return { scratch.data(), 1 };
    }

    return {};
}

namespace {

template<class T>
bool same_bytes(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

} // namespace

bool Ir::operator==(const Ir& other) const {
    return pool_ == other.pool_ && static_cast<uint32_t>(root_.start) == static_cast<uint32_t>(other.root_.start) &&
           static_cast<uint32_t>(root_.end) == static_cast<uint32_t>(other.root_.end) &&
           same_bytes(tags_, other.tags_) && same_bytes(payloads_, other.payloads_) && same_bytes(locs_, other.locs_) &&
           same_bytes(extra_, other.extra_) && ends_ == other.ends_;
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
            case Inst::Tag::ArrSlice:
            case Inst::Tag::MemberSelect:
                lv    = ir.payload(lv).op_extra.op;
                whole = false;
                break;
            default:
                return { lv, whole };
        }
    }
}

namespace {

// Overwrite the record at `at` in a copied extra table.
template<ExtraPayload T>
void put_extra(std::vector<uint32_t>& extra, ExtraIndex at, const T& value) {
    constexpr size_t n     = sizeof(T) / sizeof(uint32_t);
    const auto       words = std::bit_cast<std::array<uint32_t, n>>(value);
    std::copy(words.begin(), words.end(), extra.begin() + static_cast<uint32_t>(at));
}

} // namespace

Ir Ir::rebased(HandleMapper& m) const {
    using T = Inst::Tag;
    using F = HandleFamily;

    // A reserved slot whose body was never built (a recovered lowering failure)
    // has no pool and nothing to rebase.
    if (pool_ == nullptr) {
        return Ir{};
    }

    std::vector<Inst::Tag>     tags(tags_.data(), tags_.data() + tags_.size());
    std::vector<Inst::Payload> payloads(payloads_.data(), payloads_.data() + payloads_.size());
    std::vector<SourceLoc>     locs(locs_.data(), locs_.data() + locs_.size());
    std::vector<uint32_t>      extra(extra_.data(), extra_.data() + extra_.size());

    const auto sig = [&](Signal s) { return static_cast<Signal>(m.map(F::Signal, static_cast<uint32_t>(s))); };
    const auto fn = [&](FunctionId f) { return static_cast<FunctionId>(m.map(F::Function, static_cast<uint32_t>(f))); };
    const auto target = [&](uint32_t id) { return id == kNoDisableTarget ? id : m.map(F::DisableTarget, id); };

    // Records are read from this body and written into the copy, so a record two
    // instructions share is rebased once, not twice.
    for (uint32_t i = 0; i < tags.size(); ++i) {
        Inst::Payload& p = payloads[i];
        switch (tags[i]) {
            case T::Signal:
                p.signal = sig(p.signal);
                break;
            case T::Disable:
            case T::DisableRemote:
            case T::DeferFlush:
                p.label = target(p.label);
                break;
            case T::ExpectWake:
                p.op_imm.imm = m.map(F::Monitor, p.op_imm.imm);
                break;
            case T::Call: {
                auto info = extra_data<CallInfo>(p.extra);
                info.func = fn(info.func);
                put_extra(extra, p.extra, info);
                break;
            }
            case T::TaskCall: {
                auto info = extra_data<TaskCallInfo>(p.extra);
                info.func = fn(info.func);
                put_extra(extra, p.extra, info);
                break;
            }
            case T::CallVirtual:
            case T::TaskCallVirtual: {
                auto info = extra_data<VirtualCallInfo>(p.extra);
                info.func = fn(info.func);
                put_extra(extra, p.extra, info);
                break;
            }
            case T::Fork: {
                const auto info = extra_data<ForkInfo>(p.extra);
                for (uint32_t w = static_cast<uint32_t>(info.children.start);
                     w < static_cast<uint32_t>(info.children.end);
                     ++w) {
                    extra[w] = static_cast<uint32_t>(fn(static_cast<FunctionId>(extra_[w])));
                }
                break;
            }
            case T::StrobePend:
            case T::ReactivePend:
            case T::MonitorSet: {
                auto info = extra_data<StrobePendInfo>(p.extra);
                info.func = fn(info.func);
                put_extra(extra, p.extra, info);
                break;
            }
            case T::ExpectWait: {
                auto info = extra_data<ExpectInfo>(p.extra);
                info.monitor = m.map(F::Monitor, info.monitor);
                put_extra(extra, p.extra, info);
                break;
            }
            case T::IfaceRef: {
                auto info = extra_data<IfaceInst>(p.extra);
                info.scope = m.map(F::IfaceScope, info.scope);
                put_extra(extra, p.extra, info);
                break;
            }
            case T::IfaceSignal: {
                auto info   = extra_data<IfaceMember>(p.op_extra.extra);
                info.signal = static_cast<Signal>(m.map(F::Layout, static_cast<uint32_t>(info.signal)));
                put_extra(extra, p.op_extra.extra, info);
                break;
            }
            case T::StoreNbEvent: {
                const auto si     = extra_data<StoreNbEventInfo>(p.op_extra.extra);
                const auto start  = static_cast<uint32_t>(si.trigs.start);
                const auto stride = static_cast<uint32_t>(sizeof(IntraTrigger) / sizeof(uint32_t));
                for (uint32_t w = start; w < static_cast<uint32_t>(si.trigs.end); w += stride) {
                    auto trig   = extra_data<IntraTrigger>(static_cast<ExtraIndex>(w));
                    trig.signal = sig(trig.signal);
                    put_extra(extra, static_cast<ExtraIndex>(w), trig);
                }
                break;
            }
            case T::Wait: {
                const auto wi     = extra_data<WaitInfo>(p.extra);
                const auto start  = static_cast<uint32_t>(wi.events.start);
                const auto stride = static_cast<uint32_t>(sizeof(WaitEdge) / sizeof(uint32_t));
                for (uint32_t w = start; w < static_cast<uint32_t>(wi.events.end); w += stride) {
                    auto e = extra_data<WaitEdge>(static_cast<ExtraIndex>(w));
                    // A wait on an event handle carries a placeholder signal that
                    // names nothing (lower_event_list), so it stays as it is.
                    if (e.handle == kNoWaitHandle) {
                        e.signal = sig(e.signal);
                        put_extra(extra, static_cast<ExtraIndex>(w), e);
                    }
                }
                break;
            }
            case T::EventTriggerNbEvent: {
                auto info   = extra_data<EventDetachInfo>(p.op_extra.extra);
                info.signal = sig(info.signal);
                put_extra(extra, p.op_extra.extra, info);
                break;
            }
            case T::ReadMem: {
                auto info   = extra_data<ReadMemInfo>(p.op_extra.extra);
                info.target = sig(info.target);
                put_extra(extra, p.op_extra.extra, info);
                break;
            }
            case T::FreadMem: {
                auto info = extra_data<FreadMemInfo>(p.op_extra.extra);
                info.mem  = sig(info.mem);
                put_extra(extra, p.op_extra.extra, info);
                break;
            }
            case T::NamedBlock: {
                auto info  = extra_data<BlockInfo>(p.extra);
                info.label = target(info.label);
                put_extra(extra, p.extra, info);
                break;
            }
            // A wait site is an identity within its body (SyncInfo::site), not a
            // handle into any table.
            case T::SyncWait:
            case T::SyncCtl:
                break;
            case T::CoverQuery:
            case T::CoverCtl: {
                auto info = extra_data<CoverInfo>(p.extra);
                if (cover_op_names_type(info.op)) {
                    info.type_idx = m.map(F::CoverType, info.type_idx);
                    put_extra(extra, p.extra, info);
                }
                break;
            }
            case T::Constant:
            case T::EventNew:
            case T::Arg:
            case T::Local:
            case T::EventTrigger:
            case T::EventTriggered:
            case T::EventTriggerNb:
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
            case T::FopenMcd:
            case T::Fgetc:
            case T::Feof:
            case T::Ftell:
            case T::Rewind:
            case T::Fflush:
            case T::DumpFile:
            case T::DumpLimit:
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
            case T::OneHot:
            case T::OneHot0:
            case T::IsUnknown:
            case T::UrandomSeeded:
            case T::ObjRandom:
            case T::BitSelect:
            case T::Store:
            case T::StoreNb:
            case T::Add:
            case T::Sub:
            case T::Mul:
            case T::UDiv:
            case T::SDiv:
            case T::UMod:
            case T::SMod:
            case T::Pow:
            case T::Band:
            case T::Bor:
            case T::Bxor:
            case T::Bxnor:
            case T::WireRes:
            case T::WandRes:
            case T::WorRes:
            case T::Sll:
            case T::Srl:
            case T::Sra:
            case T::Eq:
            case T::Ne:
            case T::ULt:
            case T::ULe:
            case T::UGt:
            case T::UGe:
            case T::SLt:
            case T::SLe:
            case T::SGt:
            case T::SGe:
            case T::CaseEq:
            case T::CasezEq:
            case T::CasexEq:
            case T::WildEq:
            case T::Land:
            case T::Lor:
            case T::Limplies:
            case T::Lequiv:
            case T::StrGetc:
            case T::StrCmp:
            case T::StrICmp:
            case T::StrRepl:
            case T::StrItoa:
            case T::StrHextoa:
            case T::StrOctoa:
            case T::StrBintoa:
            case T::StrRealtoa:
            case T::StrAppendPacked:
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
            case T::UrandomRange:
            case T::Fgets:
            case T::Ferror:
            case T::FreadWord:
            case T::FopenFd:
            case T::Ungetc:
            case T::EventTriggerNbDelayed:
            case T::ObjRandSeed:
            case T::ZExt:
            case T::SExt:
            case T::Trunc:
            case T::ToFour:
            case T::ToTwo:
            case T::StrToPacked:
            case T::UItoF:
            case T::SItoF:
            case T::FtoI:
            case T::FtoITrunc:
            case T::FBitcast:
            case T::FResize:
            case T::HandleDeref:
            case T::ClassIsA:
            case T::ClassImplements:
            case T::QueueSize:
            case T::QueuePopBack:
            case T::QueuePopFront:
            case T::QueueDeleteAll:
            case T::ContainerFmtP:
            case T::DynArraySize:
            case T::DynArrayDelete:
            case T::DynArrayReverse:
            case T::DynArraySort:
            case T::DynArrayRsort:
            case T::DynArrayShuffle:
            case T::QueueReverse:
            case T::QueueSort:
            case T::QueueRsort:
            case T::QueueShuffle:
            case T::AssocNum:
            case T::AssocDeleteAll:
            case T::AssocDefault:
            case T::AssocReset:
            case T::ClassNew:
            case T::Repl:
            case T::Concat:
            case T::StrengthRes:
            case T::Loop:
            case T::Display:
            case T::FDisplay:
            case T::Sformat:
            case T::Sformatf:
            case T::Fscanf:
            case T::Sscanf:
            case T::RealMathUnary:
            case T::StreamReverse:
            case T::NetPull:
            case T::System:
            case T::TimeFormat:
            case T::Break:
            case T::Continue:
            case T::DisableAbort:
            case T::ProgramExit:
            case T::RetVoid:
            case T::DumpVars:
            case T::DumpOff:
            case T::DumpOn:
            case T::DumpAll:
            case T::DumpFlush:
            case T::Random:
            case T::Urandom:
            case T::SimTime:
            case T::Finish:
            case T::CaseViol:
            case T::TestPlusArgs:
            case T::ProcQuery:
            case T::ProcCtl:
            case T::ProcWait:
            case T::MonitorEmit:
            case T::MonitorOn:
            case T::MonitorOff:
            case T::Severity:
            case T::StoreNbDelay:
            case T::StoreInertialDelay:
            case T::Select:
            case T::SelectLazy:
            case T::PartSelect:
            case T::DynPartSelect:
            case T::ElemSelect:
            case T::ArrSlice:
            case T::MemberSelect:
            case T::AggCopy:
            case T::AggEq:
            case T::DynArrayNew:
            case T::DynArrayNewInit:
            case T::DynArrayCopy:
            case T::DynArrayCopyQueue:
            case T::QueuePushBack:
            case T::QueuePushFront:
            case T::QueueInsert:
            case T::QueueDeleteIdx:
            case T::QueueCopy:
            case T::HandleCopyNb:
            case T::QueueCopyDyn:
            case T::QueueAppendRange:
            case T::AssocSelect:
            case T::AssocExists:
            case T::AssocDeleteKey:
            case T::AssocCopy:
            case T::AssocFirst:
            case T::AssocLast:
            case T::AssocNext:
            case T::AssocPrev:
            case T::Cond:
            case T::Switch:
            case T::Fseek:
            case T::RealMathBinary:
            case T::DistUnary:
            case T::DistBinary:
            case T::InsideSet:
            case T::StrPutc:
            case T::StrSubstr:
            case T::EnumName:
            case T::TaggedCheck:
            case T::TaggedFmtP:
            case T::CountBits:
            case T::ValuePlusArgs:
            case T::RandcNext:
                break;
        }
    }

    return Ir(*pool_, std::move(tags), std::move(payloads), std::move(locs), ends_, std::move(extra), root_);
}

} // namespace clotho::ir
