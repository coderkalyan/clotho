// Instruction dispatch: types, values, lower_inst, loads, strings and structured control flow.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Lower a region's instructions in order into the current insert block. A `ret`
// (function early return) terminates the block; the rest of the region is then
// unreachable, so stop emitting into the dead tail.
void Backend::lower_region(std::span<const Index> region) {
    // Bump the NBA segment epoch on entry and exit so a nested region's NBAs
    // never share a block-commit with the enclosing straight-line run (a block
    // must not span a conditional body). See block_register / cur_seg_epoch_.
    ++cur_seg_epoch_;
    for (Index i : region) {
        const Inst::Tag t = ir_->tag(i);
        try {
            lower_inst(i);
        } catch (BackendError& e) {
            // Give the refusal the location of the construct that tripped it, so a
            // backend-stage refusal reads `file:line:col: error: ... [code]` like
            // every frontend one instead of arriving as an unlocated wall of prose.
            // Only fill an EMPTY span: a nested region rethrows through this
            // handler too, and the innermost instruction is the precise one -- the
            // `$value$plusargs` call rather than the `if` around it.
            if (!e.span.valid()) {
                e.span = SourceSpan{ ir_->loc(i), ir_->end(i) };
            }
            throw;
        }
        if (b_.GetInsertBlock()->getTerminator() != nullptr) {
            break;
        }
        // Back from a park: a remote `disable` may have landed on an enclosing
        // block while we were suspended (LRM 9.6.2). Emitted only inside a
        // remotely-named block, and only after an op that can park, so nothing
        // else in the design pays for it.
        if (design_uses_disable_ && tag_suspends(t)) {
            emit_disable_poll();
            emit_task_abort_poll();
        }
        // Back from a park: a `kill` may have landed on this thread (LRM 9.7).
        // Same placement and the same reason as the `disable` polls -- after the
        // parking op's own resume bookkeeping, so a `wait` has compacted itself
        // out of its buckets before we leave.
        if (design_uses_process_ && tag_suspends(t)) {
            emit_proc_abort_poll();
        }
    }
    ++cur_seg_epoch_;
}

// Branch to `bb` unless the current block is already terminated (e.g. a region
// that ended in a `ret`). Keeps merge edges from being appended after a `ret`.
void Backend::branch_to(llvm::BasicBlock* bb) {
    if (b_.GetInsertBlock()->getTerminator() == nullptr) {
        b_.CreateBr(bb);
    }
}

ValType Backend::type_of(Index i) {
    const auto slot = static_cast<uint32_t>(i);
    if (types_valid_[slot] != 0) {
        return types_[slot];
    }
    const ValType t    = compute_type(i);
    types_[slot]       = t;
    types_valid_[slot] = 1;
    return t;
}

ValType Backend::from_ref(Ref type) const {
    const Type& ty = std::get<Type>(mod_.pool.get(type));
    if (ty.kind == Type::Kind::String) {
        return { 0, false, false, true };
    }
    const Layout lay = mod_.pool.layout(type);
    const bool fl = mod_.pool.is_float(type);
    // Reals are signed quantities (LRM 11.8.1).
    return { lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four, ty.is_signed || fl, false, fl };
}

// The element type Ref of the queue whose handle lvalue is `queue_lv`. The
// lvalue is a plain Signal (queue-of-queue is rejected at the frontend), so
// the queue type comes straight from the signal table.
Ref Backend::queue_elem_type(Index queue_lv) {
    const Signal sig = ir_->payload(queue_lv).signal;
    return std::get<Type>(mod_.pool.get(mod_.signal(sig).type)).element;
}

ValType Backend::compute_type(Index i) {
    const Inst::Payload& p = ir_->payload(i);
    switch (ir_->tag(i)) {
        case Inst::Tag::Constant:
            return from_ref(std::get<Value>(mod_.pool.get(p.ref)).type);
        case Inst::Tag::Arg:
            return from_ref(p.arg.type);
        case Inst::Tag::Local:
            return from_ref(p.local.type);
        case Inst::Tag::Call:
            return from_ref(mod_.function(ir_->extra_data<CallInfo>(p.extra).func).return_type);
        case Inst::Tag::CallVirtual:
            // All overrides share the statically-resolved method's return type.
            return from_ref(mod_.function(ir_->extra_data<VirtualCallInfo>(p.extra).func).return_type);
        case Inst::Tag::Resize:
            return from_ref(p.op_type.type);
        case Inst::Tag::Signal: {
            const SignalSlot& s = slot_of(p.signal);
            return { s.width, s.four_state, s.is_signed, s.is_string && !s.is_array,
                     s.is_real && !s.is_array };
        }
        case Inst::Tag::BitSelect: {
            const ValType base = type_of(p.binary.l);
            return { 1, base.four_state, false };
        }
        case Inst::Tag::PartSelect: {
            const auto ps = ir_->extra_data<PartSelect>(p.op_extra.extra);
            return { ps.msb - ps.lsb + 1, type_of(p.op_extra.op).four_state, false };
        }
        case Inst::Tag::DynPartSelect: {
            const auto dps = ir_->extra_data<DynPartSelect>(p.op_extra.extra);
            return { dps.width, type_of(p.op_extra.op).four_state, false };
        }
        case Inst::Tag::ElemSelect: {
            // The result is one element of the indexed array.
            const auto  es = ir_->extra_data<ElemSelect>(p.op_extra.extra);
            const Type& at = std::get<Type>(mod_.pool.get(es.array_type));
            return from_ref(at.element);
        }
        case Inst::Tag::ArrSlice: {
            // An array lvalue, never a scalar value; report the element leaf
            // type for exhaustiveness (the slice is only ever consumed as an
            // aggregate, so this is not used on a value path).
            const auto  as = ir_->extra_data<ArrSlice>(p.op_extra.extra);
            const Type& st = std::get<Type>(mod_.pool.get(as.slice_type));
            return from_ref(st.element);
        }
        case Inst::Tag::MemberSelect: {
            // The result is the selected member's own type.
            const auto   ms     = ir_->extra_data<MemberSelect>(p.op_extra.extra);
            const Type&  st     = std::get<Type>(mod_.pool.get(ms.struct_type));
            const auto   fields = mod_.pool.struct_fields(st.fields_id);
            return from_ref(fields[ms.field_index].type);
        }
        case Inst::Tag::ClassNew:
            // The object handle (a 2-state 64-bit value, per layout(Class)).
            return from_ref(p.ref);
        case Inst::Tag::IfaceRef:
            // The instance-context address, as a 2-state 64-bit handle.
            return { 64, false, false };
        case Inst::Tag::IfaceSignal: {
            // An lvalue producer (the named member of the handle's instance);
            // its shape is the representative signal's, which every instance of
            // the definition shares.
            const SignalSlot& s = slot_of(ir_->extra_data<IfaceMember>(p.op_extra.extra).signal);
            return { s.width, s.four_state, s.is_signed, s.is_string && !s.is_array,
                     s.is_real && !s.is_array };
        }
        case Inst::Tag::Load:
            return type_of(p.unary);
        case Inst::Tag::SimTime:
            return { 64, false, false };
        case Inst::Tag::EventNew:
            // An event handle: a 2-state 64-bit word.
            return { 64, false, false };
        case Inst::Tag::ProcQuery:
            // `self` is a class handle, `status` the 32-bit state enum,
            // `get_randstate` a string -- the frontend recorded which on the op.
            return from_ref(ir_->extra_data<ProcCtlInfo>(p.extra).type);
        case Inst::Tag::ProcCtl:
        case Inst::Tag::ProcWait:
            // Statements, never values.
            return { 1, false, false };
        case Inst::Tag::SyncWait:
            // 1 when the park resumed as its queue's head: a definite 2-state
            // bit the frontend ANDs with its own resource test.
            return { 1, false, false };
        case Inst::Tag::SyncCtl:
            // A statement, never a value.
            return { 1, false, false };
        case Inst::Tag::CoverQuery:
            // A covergroup handle, a coverage percentage, or a bin count -- the
            // frontend recorded which on the op.
            return from_ref(ir_->extra_data<CoverInfo>(p.extra).type);
        case Inst::Tag::CoverCtl:
            // A statement, never a value.
            return { 1, false, false };
        case Inst::Tag::EventTriggered:
            // `e.triggered` (LRM 15.5.3): a definite 2-state 1-bit `bit`.
            return { 1, false, false };
        case Inst::Tag::Random:
            return { 32, false, true };
        case Inst::Tag::Clog2:
            // $clog2 returns a 32-bit signed `integer` (4-state: the result
            // is X when the operand carries x/z).
            return { 32, true, true };
        case Inst::Tag::Ctz:
        case Inst::Tag::Clz:
            // A definite 2-state 32-bit bit index (counts of the must plane).
            return { 32, false, false };
        case Inst::Tag::CountBits:
            // $countbits family: a definite 2-state 32-bit signed `int`
            // (LRM 20.9 return type). Never X — it counts, it does not
            // propagate.
            return { 32, false, true };
        case Inst::Tag::OneHot:
        case Inst::Tag::OneHot0:
        case Inst::Tag::IsUnknown:
            // Definite 1-bit qualifier results (LRM 20.9); never X.
            return { 1, false, false };
        case Inst::Tag::FopenFd:
        case Inst::Tag::FopenMcd:
            // $fopen returns a 32-bit descriptor integer.
            return { 32, false, true };
        case Inst::Tag::Fgetc:
        case Inst::Tag::Ungetc:
        case Inst::Tag::Feof:
        case Inst::Tag::Ftell:
        case Inst::Tag::Fseek:
        case Inst::Tag::Rewind:
        case Inst::Tag::Fgets:
        case Inst::Tag::Ferror:
        case Inst::Tag::Fscanf:
        case Inst::Tag::Sscanf:
        case Inst::Tag::FreadWord:
        case Inst::Tag::FreadMem:
            // The file read / positioning / error / scan functions return i32.
            return { 32, false, true };
        case Inst::Tag::System:
            // $system returns the raw system() int (a POSIX wait-status).
            return { 32, false, true };
        case Inst::Tag::TestPlusArgs:
        case Inst::Tag::ValuePlusArgs:
            // LRM §21.6: an always-known (never x/z) boolean 'int'.
            return { 32, false, true };
        case Inst::Tag::DynArrayNew:
        case Inst::Tag::DynArrayNewInit:
        case Inst::Tag::DynArraySize:
        case Inst::Tag::DynArrayDelete:
        case Inst::Tag::DynArrayCopy:
        case Inst::Tag::DynArrayCopyQueue:
            // Only size() is a value (a definite 2-state int); new / delete /
            // copy are statements whose type is never queried.
            return { 32, false, true };
        case Inst::Tag::QueuePopBack:
        case Inst::Tag::QueuePopFront:
            // One element of the queue's element type, straight off the
            // payload's Queue type.
            return from_ref(std::get<Type>(mod_.pool.get(p.op_type.type)).element);
        case Inst::Tag::QueueSize:
        case Inst::Tag::QueuePushBack:
        case Inst::Tag::QueuePushFront:
        case Inst::Tag::QueueInsert:
        case Inst::Tag::QueueDeleteIdx:
        case Inst::Tag::QueueDeleteAll:
        case Inst::Tag::QueueCopy:
        case Inst::Tag::HandleCopyNb:
        case Inst::Tag::QueueCopyDyn:
        case Inst::Tag::QueueAppendRange:
            // size() is a definite 2-state int; the rest are statements
            // whose type is never queried.
            return { 32, false, true };
        case Inst::Tag::DynArrayReverse:
        case Inst::Tag::DynArraySort:
        case Inst::Tag::DynArrayRsort:
        case Inst::Tag::DynArrayShuffle:
        case Inst::Tag::QueueReverse:
        case Inst::Tag::QueueSort:
        case Inst::Tag::QueueRsort:
        case Inst::Tag::QueueShuffle:
            // Ordering methods (LRM 7.12.2) are void statements; their type
            // is never queried.
            return { 32, false, true };
        case Inst::Tag::AssocSelect:
            // One entry of the associative array's value element type.
            return from_ref(std::get<Type>(mod_.pool.get(
              ir_->extra_data<AssocArg>(p.op_extra.extra).assoc_type)).element);
        case Inst::Tag::AssocDefault:
            // The `'{default: v}` slot carries the same value element type.
            return from_ref(std::get<Type>(mod_.pool.get(p.op_type.type)).element);
        case Inst::Tag::AssocNum:
        case Inst::Tag::AssocExists:
        case Inst::Tag::AssocFirst:
        case Inst::Tag::AssocLast:
        case Inst::Tag::AssocNext:
        case Inst::Tag::AssocPrev:
        case Inst::Tag::AssocDeleteKey:
        case Inst::Tag::AssocDeleteAll:
        case Inst::Tag::AssocReset:
        case Inst::Tag::AssocCopy:
            // num()/exists() and the traversal status are definite 2-state
            // ints; delete/copy are statements whose type is never queried.
            return { 32, false, true };

        case Inst::Tag::Add:
        case Inst::Tag::Sub:
        case Inst::Tag::Mul:
        case Inst::Tag::Div:
        case Inst::Tag::Mod:
        // Power's result shares the left operand's type; the exponent is
        // self-determined and does not widen the result.
        case Inst::Tag::Pow:
        case Inst::Tag::Band:
        case Inst::Tag::Bor:
        case Inst::Tag::Bxor:
        case Inst::Tag::Bxnor:
        case Inst::Tag::Sll:
        case Inst::Tag::Srl:
        case Inst::Tag::Sra:
            return type_of(p.binary.l);
        case Inst::Tag::WireRes:
        case Inst::Tag::WandRes:
        case Inst::Tag::WorRes: {
            // Net resolution always yields a 4-state result at the operand
            // (net) width — a driver conflict is X, an all-high-Z net is Z —
            // even when the driver slots feeding it are 2-state.
            const ValType lt = type_of(p.binary.l);
            return { lt.width, true, false };
        }
        case Inst::Tag::StrengthRes: {
            // Strength resolution yields a 4-state net-width result, like
            // WireRes; the width is the first driver slot's width.
            const auto    info = ir_->extra_data<StrengthResInfo>(p.extra);
            const ValType lt   = type_of(ir_->region(info.values)[0]);
            return { lt.width, true, false };
        }
        case Inst::Tag::Bnot:
            return type_of(p.unary);
        case Inst::Tag::StreamReverse:
            // A bit permutation: the operand's width and domain.
            return type_of(p.op_imm.op);
        case Inst::Tag::NetPull:
            // A pulled net is 4-state (a driver conflict is still X) but
            // never Z, at the resolved value's width.
            return { type_of(p.op_imm.op).width, true, false };
        case Inst::Tag::Select:
            return type_of(ir_->extra_data<Select>(p.op_extra.extra).a);
        case Inst::Tag::SelectLazy:
            return type_of(ir_->extra_data<SelectLazy>(p.op_extra.extra).then_val);

        case Inst::Tag::FAdd:
        case Inst::Tag::FSub:
        case Inst::Tag::FMul:
        case Inst::Tag::FDiv:
        case Inst::Tag::FPow:
            return type_of(p.binary.l);
        case Inst::Tag::FNeg:
            return type_of(p.unary);
        // Real compares are definite (reals carry no x/z): 1-bit two-state.
        case Inst::Tag::FEq:
        case Inst::Tag::FNe:
        case Inst::Tag::FLt:
        case Inst::Tag::FLe:
        case Inst::Tag::FGt:
        case Inst::Tag::FGe:
            return { 1, false, false };
        case Inst::Tag::ItoF:
        case Inst::Tag::FtoI:
        case Inst::Tag::FtoITrunc:
        case Inst::Tag::FBitcast:
        case Inst::Tag::FResize:
            return from_ref(p.op_type.type);
        // Every LRM 20.8.2 routine returns a real.
        case Inst::Tag::RealMathUnary:
        case Inst::Tag::RealMathBinary:
            return { 64, false, true, false, true };

        // $dist_* / seeded $random: a packed i64 {seed:32, draw:32}, sliced
        // by the frontend. A definite 2-state word (never x/z).
        case Inst::Tag::DistUnary:
        case Inst::Tag::DistBinary:
            return { 64, false, false };
        // $urandom / $urandom_range: a 32-bit unsigned `int` (2-state). An
        // object's own draw (LRM §18.14.2) and a randc draw (LRM §18.6) have
        // the same result type; only the state they advance differs.
        case Inst::Tag::Urandom:
        case Inst::Tag::UrandomSeeded:
        case Inst::Tag::UrandomRange:
        case Inst::Tag::ObjRandom:
        case Inst::Tag::RandcNext:
            return { 32, false, false };

        case Inst::Tag::RedAnd:
        case Inst::Tag::RedOr:
        case Inst::Tag::RedXor:
        case Inst::Tag::Eq:
        case Inst::Tag::Ne:
        case Inst::Tag::Lt:
        case Inst::Tag::Le:
        case Inst::Tag::Gt:
        case Inst::Tag::Ge:
        case Inst::Tag::CaseEq:
        case Inst::Tag::CasezEq:
        case Inst::Tag::CasexEq:
        case Inst::Tag::WildEq:
        case Inst::Tag::Land:
        case Inst::Tag::Lor:
        case Inst::Tag::Lnot:
        case Inst::Tag::AggEq:
        case Inst::Tag::InsideSet:
        case Inst::Tag::Limplies:
        case Inst::Tag::Lequiv:
            return { 1, true, false };

        case Inst::Tag::ClassIsA:
        case Inst::Tag::ClassImplements:
            // A definite bit: the display entry or the interface bit either
            // matches or it does not.
            return { 1, false, false };

        case Inst::Tag::Concat: {
            std::array<Index, 4> scratch;
            uint32_t             width = 0;
            bool                 four  = false;
            for (Index part : ir_->operands(i, scratch)) {
                const ValType t = type_of(part);
                if (t.is_string) { // string concat (all parts are strings)
                    return { 0, false, false, true };
                }
                width += t.width;
                four = four || t.four_state;
            }
            return { width, four, false };
        }
        case Inst::Tag::Repl: {
            // Packed only: a string replication is StrRepl (runtime count).
            const ValType v = type_of(p.repl.value);
            return { v.width * p.repl.count, v.four_state, false };
        }
        case Inst::Tag::StrRepl:
            return { 0, false, false, true };

        case Inst::Tag::StrLen:
        case Inst::Tag::StrCmp:
        case Inst::Tag::StrICmp:
            return { 32, false, true };
        case Inst::Tag::StrGetc:
            // getc / s[i] return `byte`, which is signed (LRM 6.16.3 and
            // Table 6-8), so 0xff displays as -1 under %d.
            return { 8, false, true };
        case Inst::Tag::StrUpper:
        case Inst::Tag::StrLower:
        case Inst::Tag::StrSubstr:
        case Inst::Tag::StrFromPacked:
        case Inst::Tag::Sformatf:
        case Inst::Tag::EnumName:
        case Inst::Tag::ContainerFmtP:
        case Inst::Tag::TaggedFmtP:
            return { 0, false, false, true };
        case Inst::Tag::StrToPacked:
            return from_ref(p.op_type.type);
        case Inst::Tag::StrPutc:
            return { 0, false, false };
        case Inst::Tag::StrAtoi:
        case Inst::Tag::StrAtohex:
        case Inst::Tag::StrAtooct:
        case Inst::Tag::StrAtobin:
            // LRM 6.16.9: declared return type `integer` (4-state), though
            // the conversion itself is always definite — no x/z is
            // reachable from a string's bytes.
            return { 32, true, true };
        case Inst::Tag::StrAtoreal:
            return { 64, false, true, false, true };
        case Inst::Tag::StrItoa:
        case Inst::Tag::StrHextoa:
        case Inst::Tag::StrOctoa:
        case Inst::Tag::StrBintoa:
        case Inst::Tag::StrRealtoa:
        case Inst::Tag::StrAppendPacked:
            return { 0, false, false };

        case Inst::Tag::Store:
        case Inst::Tag::StoreNb:
        case Inst::Tag::StoreNbDelay:
        case Inst::Tag::StoreInertialDelay:
        case Inst::Tag::StoreNbEvent:
        case Inst::Tag::AggCopy:
        case Inst::Tag::Cond:
        case Inst::Tag::Switch:
        case Inst::Tag::Loop:
        case Inst::Tag::Break:
        case Inst::Tag::Continue:
        case Inst::Tag::NamedBlock:
        case Inst::Tag::Disable:
        case Inst::Tag::DisableRemote:
        case Inst::Tag::DisableAbort:
        case Inst::Tag::DeferFlush:
        case Inst::Tag::HandleDeref:
            // An lvalue producer (rooted at the object body); no value type.
        case Inst::Tag::Ret:
        case Inst::Tag::RetVoid:
        case Inst::Tag::Delay:
        case Inst::Tag::Wait:
        case Inst::Tag::Fork:
        case Inst::Tag::Display:
        case Inst::Tag::FDisplay:
        case Inst::Tag::StrobePend:
        case Inst::Tag::ReactivePend:
        case Inst::Tag::ExpectWait:
        case Inst::Tag::ExpectWake:
        case Inst::Tag::MonitorSet:
        case Inst::Tag::MonitorEmit:
        case Inst::Tag::MonitorOn:
        case Inst::Tag::MonitorOff:
        case Inst::Tag::TimeFormat:
        case Inst::Tag::Finish:
        case Inst::Tag::ProgramExit:
        case Inst::Tag::CaseViol:
        case Inst::Tag::TaggedCheck:
        case Inst::Tag::Severity:
        case Inst::Tag::Fflush:
        case Inst::Tag::Fclose:
        case Inst::Tag::Sformat:
        case Inst::Tag::ReadMem:
        case Inst::Tag::TaskCall:
        case Inst::Tag::TaskCallVirtual:
        case Inst::Tag::DumpFile:
        case Inst::Tag::DumpVars:
        case Inst::Tag::DumpLimit:
        case Inst::Tag::DumpOff:
        case Inst::Tag::DumpOn:
        case Inst::Tag::DumpAll:
        case Inst::Tag::DumpFlush:
        case Inst::Tag::EventTrigger:
        case Inst::Tag::EventTriggerNb:
        case Inst::Tag::EventTriggerNbDelayed:
        case Inst::Tag::EventTriggerNbEvent:
            // Reseeding an object's RNG state yields nothing, like the
            // per-process $urandom(seed) reseed that precedes a draw.
        case Inst::Tag::ObjRandSeed:
            return { 0, false, false };
    }
    return { 0, false, false };
}

Plane Backend::value_of(Index i) const {
    return values_[static_cast<uint32_t>(i)];
}

void Backend::set_value(Index i, Plane p) {
    // Keep a value's encoding in lockstep with its IR type's domain. slang
    // unifies operand *types* (inserting conversions), not our Plane flags, so
    // the "operands share a domain" invariant the operator dispatch relies on
    // holds only if every value's `two` flag matches type_of(i). Producers that
    // always emit the packed form (Arg, Call, SimTime, string ops, ...) are
    // projected to a single iW here when their type is 2-state; the projection
    // is lossless because such a value has must == may and no z.
    const ValType t = type_of(i);
    // A real value stays fp-only; pack/unpack happens at the storage / ABI
    // boundaries (see the real helpers).
    if (!t.is_string && !t.is_real) {
        if (t.four_state && p.two) {
            p = to_four(p);
        } else if (!t.four_state && !p.two) {
            p = two_val(must_of(p));
        }
    }
    values_[static_cast<uint32_t>(i)] = p;
}

// The descriptor an earlier lvalue-derivation instruction recorded for `i`.
//
// The engaged check is not defensive padding: every place consumer in the backend
// funnels through here, so this is the one spot that can turn "an operand the IR
// used as a place was lowered as a VALUE" into a located refusal. Without it the
// disengaged optional was dereferenced and its all-zero buffer read as an LValue,
// which faults on the first null member (bit_off, in ref_actual_ptr) with no
// diagnostic at all. A call whose actuals did not line up with its signature was
// how that happened; lower_call_common rejects that shape up front now, and this
// backstops every other route to it.
const LValue& Backend::lvalue_of(Index i) const {
    const std::optional<LValue>& lv = lvals_[static_cast<uint32_t>(i)];
    if (!lv.has_value()) {
        throw BackendError{ "internal: operand used as a place was lowered as a value" };
    }
    return *lv;
}

void Backend::set_lvalue(Index i, LValue lv) {
    lvals_[static_cast<uint32_t>(i)] = lv;
}

// rt_string handle pointer of a string-valued instruction (parallel to
// values_; null for non-string values).
llvm::Value* Backend::str_of(Index i) const {
    return str_vals_[static_cast<uint32_t>(i)];
}

void Backend::set_str(Index i, llvm::Value* handle) {
    str_vals_[static_cast<uint32_t>(i)] = handle;
}

// The rt_string handle pointer of a string lvalue (one array element when
// elem_idx selects it).
//
// `use` says WHY, and it has no default on purpose (StringUse). A waited-on
// string class property notifies from a content comparison around the write, so
// a `Write` here must sit inside a str_chg_snapshot / str_chg_after bracket;
// that is checked rather than trusted, because a missing notify is a silent
// permanent park and the check can only fire for a property someone waits on.
llvm::Value* Backend::lv_string_ptr(const LValue& lv, StringUse use) {
    if (use == StringUse::Write && lv.chg != nullptr && str_chg_open_ == 0) {
        throw BackendError{ "internal: write to a waited-on string class property "
                            "outside a change-notify bracket (LRM 9.4.2)" };
    }
    llvm::Value* ptr = nullptr;
    if (lv.array_ty != nullptr) {
        llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
        ptr = b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx });
    } else if (lv.is_agg) {
        // A string leaf inside an aggregate (struct member / class property)
        // is reached through the aggregate GEP path, not the bare base.
        ptr = agg_slot_ptr(lv);
    } else {
        ptr = lv.base;
    }
    if (lv.dyn_inrange != nullptr) {
        // Out-of-range dynamic-array / queue string element: redirect to the
        // writable sink handle, reset to owned-empty right here — an OOB
        // read yields "" (Table 7-1 string default) and an OOB write /
        // mutating method lands in the sink and is discarded (LRM 7.5.1).
        llvm::Value* sink = str_sink_global();
        b_.CreateCall(str_deinit_fn(), { sink });
        b_.CreateCall(str_init_fn(), { sink });
        ptr = b_.CreateSelect(lv.dyn_inrange, ptr, sink);
    }
    return ptr;
}

// The shared out-of-range string-element sink: a writable rt_string handle
// that powers on owned-empty and is reset before every use (lv_string_ptr).
llvm::Value* Backend::str_sink_global() {
    if (env_->str_sink == nullptr) {
        llvm::Constant* init = str_empty_const();
        env_->str_sink = new llvm::GlobalVariable(env_->llmod, str_slot_ty(), /*isConstant=*/false,
                                             llvm::GlobalValue::InternalLinkage, init,
                                             "clotho.str.sink");
    }
    return env_->str_sink;
}

// A read-only owned-empty rt_string handle. Every string consumer borrows its
// operand pointer and deep-copies what it keeps (Runtime.h: handles never
// share storage), so a value nobody may write can be shared by every reader.
// Only lower_string_mux uses it, for the string type's default value.
llvm::Value* Backend::str_empty_global() {
    if (env_->str_empty == nullptr) {
        env_->str_empty = new llvm::GlobalVariable(env_->llmod, str_slot_ty(), /*isConstant=*/true,
                                                   llvm::GlobalValue::InternalLinkage, str_empty_const(),
                                                   "clotho.str.empty");
    }
    return env_->str_empty;
}

// Whether an instruction produces a fresh string value that needs its own
// 32-byte handle (as opposed to yielding a pointer at existing storage).
bool Backend::produces_string_temp(Index i) {
    switch (ir_->tag(i)) {
        case Inst::Tag::StrUpper:
        case Inst::Tag::StrLower:
        case Inst::Tag::StrSubstr:
        case Inst::Tag::StrFromPacked:
        case Inst::Tag::Sformatf:
        case Inst::Tag::EnumName:
        case Inst::Tag::ContainerFmtP:
        case Inst::Tag::TaggedFmtP:
        case Inst::Tag::StrRepl:
            return true;
        // `p.get_randstate()` renders a fresh string (LRM 9.7); every other
        // process query yields a word.
        case Inst::Tag::ProcQuery:
            return type_of(i).is_string;
        case Inst::Tag::Constant:
        case Inst::Tag::Concat:
        case Inst::Tag::Call:
        case Inst::Tag::CallVirtual:
        // A pop of a string-element queue relocates the element into the
        // instruction's own temp handle (lower_queue_pop).
        case Inst::Tag::QueuePopBack:
        case Inst::Tag::QueuePopFront:
            return type_of(i).is_string;
        default:
            return false;
    }
}

// A string Store whose RHS concat builds directly into the store target,
// making the store itself a no-op. Matches the exact frontend shape of
// `s = {...}` — consecutive [Concat, lvalue leaf, Store] — so nothing reads
// the target between the concat write and the store, and the concat has no
// earlier consumer. rt_string_concat is alias-safe for a target appearing as
// a prefix part (s = {s, chunk} appends in place, the O(n^2)->O(n) fix for
// repeated append); a whole-string leaf target keeps the handle derivable in
// the prologue, before the leaf instruction itself lowers.
bool Backend::forwarded_string_store(Index j) {
    if (ir_->tag(j) != Inst::Tag::Store) {
        return false;
    }
    const Inst::Payload& p  = ir_->payload(j);
    const auto           ji = static_cast<uint32_t>(j);
    if (static_cast<uint32_t>(p.binary.r) + 2 != ji || static_cast<uint32_t>(p.binary.l) + 1 != ji) {
        return false;
    }
    if (ir_->tag(p.binary.r) != Inst::Tag::Concat || !type_of(p.binary.r).is_string) {
        return false;
    }
    switch (ir_->tag(p.binary.l)) {
        case Inst::Tag::Signal: {
            const SignalSlot& s = slot_of(ir_->payload(p.binary.l).signal);
            return s.is_string && !s.is_array;
        }
        case Inst::Tag::Local: {
            const uint32_t idx = ir_->payload(p.binary.l).local.index;
            return idx < frame_slots_.size() && frame_slots_[idx].is_string;
        }
        default:
            return false;
    }
}

// Alloca + init one handle per string-producing instruction, in the body
// prologue (entry block of a function; right after coro.begin of a process /
// task). Producers overwrite their temp in place, so a loop reuses its
// capacity instead of allocating each iteration. Paired with
// deinit_body_strings at the body's exits. A concat consumed by an adjacent
// forwarded store gets the store target's handle instead of a temp (never
// init'd — the target already holds a live string).
// The callee's (statically-resolved) FunctionId for a Call or CallVirtual — its
// signature / return type, shared by every override of a virtual method.
FunctionId Backend::call_func_id(Index i) const {
    return ir_->tag(i) == Inst::Tag::CallVirtual
               ? ir_->extra_data<VirtualCallInfo>(ir_->payload(i).extra).func
               : ir_->extra_data<CallInfo>(ir_->payload(i).extra).func;
}

void Backend::build_str_temps() {
    str_temp_list_.clear();
    for (size_t k = 0; k < ir_->inst_count(); ++k) {
        const auto i = static_cast<Index>(k);
        if (!produces_string_temp(i)) {
            continue;
        }
        if (ir_->tag(i) == Inst::Tag::Concat && k + 2 < ir_->inst_count() &&
            forwarded_string_store(static_cast<Index>(k + 2))) {
            const Inst::Payload& lp = ir_->payload(static_cast<Index>(k + 1));
            const LValue lv = ir_->tag(static_cast<Index>(k + 1)) == Inst::Tag::Signal
                                ? signal_lvalue(lp.signal)
                                : local_lvalue(lp.local.index);
            set_str(i, lv_string_ptr(lv, StringUse::Place));
            continue;
        }
        auto* temp = b_.CreateAlloca(str_slot_ty(), nullptr, "str.tmp");
        temp->setAlignment(llvm::Align(alignof(rt_string)));
        b_.CreateCall(str_init_fn(), { temp });
        set_str(i, temp);
        str_temp_list_.push_back(temp);
    }
}

// Return type of a Call / CallVirtual instruction, or Ref::Invalid when the
// instruction is neither. Used before lowering, when only the IR is available.
Ref Backend::call_return_type(Index i) const {
    if (ir_->tag(i) == Inst::Tag::Call) {
        return mod_.function(ir_->extra_data<CallInfo>(ir_->payload(i).extra).func).return_type;
    }
    if (ir_->tag(i) == Inst::Tag::CallVirtual) {
        // The statically-resolved prototype: every override shares its return
        // type (LRM 8.20), which is what sizes the result buffer.
        return mod_.function(ir_->extra_data<VirtualCallInfo>(ir_->payload(i).extra).func).return_type;
    }
    return Ref::Invalid;
}

// Overwrite an element buffer with the element type's power-on image.
void Backend::emit_agg_proto_fill(llvm::Value* dst, const DynElem& e) {
    const uint64_t size = env_->llmod.getDataLayout().getTypeAllocSize(e.agg_ty);
    b_.CreateMemCpy(dst, llvm::MaybeAlign(), agg_proto_global(e.elem_type), llvm::MaybeAlign(), b_.getInt64(size));
}

void Backend::lower_inst(Index i) {
    // The dereference guard below (emit_ref_epoch_check) reports against the
    // access that faulted, which is this instruction; nothing else reads it.
    cur_inst_              = i;
    const Inst::Payload& p = ir_->payload(i);
    switch (ir_->tag(i)) {
        // Lvalue derivation: record a descriptor, emit no value.
        case Inst::Tag::Signal:
            set_lvalue(i, signal_lvalue(p.signal));
            return;
        case Inst::Tag::Local:
            set_lvalue(i, local_lvalue(p.local.index));
            return;
        case Inst::Tag::BitSelect: {
            LValue lv = lvalue_of(p.binary.l);
            if (lv.is_string) {
                // String indexing lowers to StrGetc / StrPutc in the frontend.
                throw BackendError{ "internal: string value in bit_select" };
            }
            // A bit-select is bounds-checked against its container (LRM
            // 11.5.1): an out-of-range or x/z index reads x / writes nothing.
            const Plane        idxp   = value_of(p.binary.r);
            llvm::Value* const cbase  = lv.bit_off != nullptr ? lv.bit_off : b_.getInt32(0);
            const uint32_t     cwidth = lv.width;
            llvm::Value*       idx    = cast(must_of(idxp), 32, false);
            lv.bit_off = lv.bit_off != nullptr ? b_.CreateAdd(lv.bit_off, idx) : idx;
            lv.width   = 1;
            const SelectVerdict sv = set_select_bounds(lv, cbase, cwidth, 1, any_unknown(idxp));
            add_select_bounds_check(lv, i, p.binary.r, sv, BoundsWhat::Bit, cwidth, 1);
            set_lvalue(i, lv);
            return;
        }
        case Inst::Tag::PartSelect: {
            LValue     lv = lvalue_of(p.op_extra.op);
            const auto ps = ir_->extra_data<PartSelect>(p.op_extra.extra);
            lv.bit_off    = b_.CreateAdd(lv.bit_off, b_.getInt32(ps.lsb));
            lv.width      = ps.msb - ps.lsb + 1;
            set_lvalue(i, lv);
            return;
        }
        case Inst::Tag::DynPartSelect: {
            // Runtime low-bit offset, constant width: add the offset to the
            // lvalue's bit_off exactly as a dynamic BitSelect does, then the
            // shared load/store path shifts/masks by the runtime bit_off.
            // Bounds-checked against its container (LRM 11.5.1): bits of the
            // selection that fall outside read x / are not written; a wholly
            // out-of-range or x/z offset makes the whole select out of range.
            LValue             lv     = lvalue_of(p.op_extra.op);
            const auto         dps    = ir_->extra_data<DynPartSelect>(p.op_extra.extra);
            const Plane        offp   = value_of(dps.offset);
            llvm::Value* const cbase  = lv.bit_off != nullptr ? lv.bit_off : b_.getInt32(0);
            const uint32_t     cwidth = lv.width;
            llvm::Value*       off    = cast(must_of(offp), 32, false);
            lv.bit_off = lv.bit_off != nullptr ? b_.CreateAdd(lv.bit_off, off) : off;
            lv.width   = dps.width;
            const SelectVerdict sv =
              set_select_bounds(lv, cbase, cwidth, dps.width, any_unknown(offp));
            add_select_bounds_check(lv, i, dps.offset, sv, BoundsWhat::Part, cwidth, dps.width);
            set_lvalue(i, lv);
            return;
        }
        case Inst::Tag::ElemSelect: {
            // Advance the lvalue's flat leaf index to the selected element:
            // elem_idx += normalized_index * leaf-elements-per-selected-element
            // (1 for a leaf select; the inner dimensions' product for an outer
            // dimension of a multidimensional memory). The base lvalue's
            // slot_ty and store_width already describe a leaf element (set by
            // the array's signal slot), so this only shifts to the element and
            // resets any inherited bit offset.
            LValue         lv     = lvalue_of(p.op_extra.op);
            const auto     es     = ir_->extra_data<ElemSelect>(p.op_extra.extra);
            const Type&    at     = std::get<Type>(mod_.pool.get(es.array_type));
            const bool     inner_container = at.kind == Type::Kind::DynArray || at.kind == Type::Kind::Queue;
            if (lv.is_assoc && inner_container) {
                // `a[k][i]`: the base is a DEFERRED assoc entry, and the ring /
                // bounds arithmetic below needs its slot address now, so the
                // entry is resolved here rather than at the consumer. Which way
                // is the access's own, threaded from the frontend
                // (ir::ElemSelect::for_write): a read resolves get_ptr and
                // creates nothing (LRM 7.8.6) -- an absent key then yields the
                // empty-container sink, so the element read is the element
                // default -- while a write resolves put_ptr and creates the
                // entry (LRM 7.8), after which an index past the fresh empty
                // container drops its write (7.10).
                //
                // Only a dyn/queue value needs this: its element address is not a
                // function of the base alone (it loads base/head/len/cap), so the
                // entry must be resolved first. An AGGREGATE value still composes
                // deferred -- the select only extends the GEP path, and the
                // consumer resolves with its own direction, which is both cheaper
                // and what already worked.
                lv = assoc_elem_lvalue(lv, es.for_write != 0);
            }
            if (at.kind == Type::Kind::DynArray) {
                // Dynamic array (LRM 7.5): the base lvalue addresses the
                // { data, len } handle. Load the heap data pointer and length,
                // and build a fresh element lvalue over `[0 x elemslot]` at the
                // 0-based index, carrying the in-range predicate so the shared
                // load/store applies bounds (out-of-range read -> default,
                // write -> no-op; the heap is never touched past `len`).
                const DynElem e       = dynarr_elem(es.array_type);
                // `d[i] = v` mutates the property's contents, so a for-write
                // select of a waited-on container notifies here -- the store the
                // frontend paired this select with follows with no suspend
                // between (ir::ElemSelect::for_write threads the direction).
                llvm::Value*  handle  = lv_handle_ptr(lv, es.for_write != 0 ? HandleUse::Mutate : HandleUse::Read);
                llvm::Value*  dataptr = b_.CreateLoad(
                  raw_ptr_ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 0));
                llvm::Value* len = b_.CreateLoad(
                  b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 1));
                // The element place is rooted in this handle's heap buffer, so
                // a `ref` formed through it records the handle's relocation
                // epoch (docs/design/data-model.md §3.1).
                llvm::Value* epoch = b_.CreateStructGEP(
                  dynarr_slot_ty(), handle, handle_epoch_field(es.array_type));
                // LRM 7.4.5 covers "an unpacked array of any kind": an x/z
                // index is invalid exactly like one past the end, so the
                // unknown term belongs in the gate and not only in the report.
                const Plane  idxp    = value_of(es.index);
                llvm::Value* idx64   = cast(must_of(idxp), 64, false);
                llvm::Value* unknown = any_unknown(idxp);
                llvm::Value* inrange =
                  b_.CreateAnd(b_.CreateICmpULT(idx64, len), b_.CreateNot(unknown));
                if (e.is_string) {
                    // String element: an rt_string handle slot in the heap
                    // buffer. Bounds ride lv_string_ptr's sink redirect.
                    LValue sel;
                    sel.base          = dataptr;
                    sel.is_string     = true;
                    sel.slot_ty       = e.slot_ty;
                    sel.array_ty      = llvm::ArrayType::get(e.slot_ty, 0);
                    sel.elem_idx      = cast(idx64, 32, false);
                    sel.is_signal     = lv.is_signal;
                    sel.sig           = lv.sig;
                    sel.dyn_inrange   = inrange;
                    sel.bounds_checks = lv.bounds_checks;
                    sel.epoch_chain   = lv.epoch_chain;
                    sel.epoch_chain.push_back(epoch);
                    add_elem_bounds_check(sel, i, lv, es.index, inrange, unknown, len);
                    set_lvalue(i, sel);
                    return;
                }
                if (e.is_agg || e.is_handle) {
                    LValue sel      = dyn_agg_elem_lvalue(lv, e, dataptr, idx64, inrange);
                    sel.epoch_chain = lv.epoch_chain;
                    sel.epoch_chain.push_back(epoch);
                    note_gc_container_root(sel, lv, handle, es.array_type);
                    add_elem_bounds_check(sel, i, lv, es.index, inrange, unknown, len);
                    set_lvalue(i, sel);
                    return;
                }
                LValue       el;
                el.epoch_chain   = lv.epoch_chain;
                el.epoch_chain.push_back(epoch);
                el.gc_owner      = lv.gc_owner;
                note_gc_container_root(el, lv, handle, es.array_type);
                el.base          = dataptr;
                el.slot_ty       = e.slot_ty;
                el.has_z         = e.has_z;
                el.narrow        = e.narrow;
                el.array_ty      = llvm::ArrayType::get(e.slot_ty, 0);
                el.elem_idx      = cast(idx64, 32, false);
                el.store_width   = e.width;
                el.bit_off       = b_.getInt32(0);
                el.width         = e.width;
                el.is_signal     = lv.is_signal;
                el.sig           = lv.sig;
                el.dyn_inrange   = inrange;
                el.bounds_checks = lv.bounds_checks;
                add_elem_bounds_check(el, i, lv, es.index, inrange, unknown, len);
                set_lvalue(i, el);
                return;
            }
            if (at.kind == Type::Kind::Queue) {
                // Queue (LRM 7.10): the base lvalue addresses the
                // { base, head, len, cap } ring handle. Element i lives at the
                // physical slot (head + i) mod cap; build a fresh element
                // lvalue over `[0 x elemslot]` at that physical index with the
                // in-range predicate i < len (an empty queue has len == 0, so
                // the modulo divisor is clamped to 1 to keep it defined on the
                // never-taken out-of-range path).
                const DynElem e       = dynarr_elem(es.array_type);
                llvm::Value*  handle  = lv_handle_ptr(lv, es.for_write != 0 ? HandleUse::Mutate : HandleUse::Read);
                llvm::Value*  baseptr = b_.CreateLoad(
                  raw_ptr_ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 0));
                llvm::Value* head = b_.CreateLoad(
                  b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 1));
                llvm::Value* len = b_.CreateLoad(
                  b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 2));
                llvm::Value* cap = b_.CreateLoad(
                  b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 3));
                // A place rooted in this ring buffer records the handle's
                // relocation epoch, so a `ref` formed through it detects a later
                // push / pop / insert / delete (docs/design/data-model.md §3.1).
                llvm::Value* epoch = b_.CreateStructGEP(
                  queue_slot_ty(), handle, handle_epoch_field(es.array_type));
                // An x/z index is invalid for an unpacked array of any kind
                // (LRM 7.4.5), so it gates the access, not just the report.
                const Plane  idxp    = value_of(es.index);
                llvm::Value* idx64   = cast(must_of(idxp), 64, false);
                llvm::Value* unknown = any_unknown(idxp);
                llvm::Value* inrange =
                  b_.CreateAnd(b_.CreateICmpULT(idx64, len), b_.CreateNot(unknown));
                llvm::Value* safe_cap =
                  b_.CreateSelect(b_.CreateICmpEQ(cap, b_.getInt64(0)), b_.getInt64(1), cap);
                llvm::Value* phys = b_.CreateURem(b_.CreateAdd(head, idx64), safe_cap);
                if (e.is_string) {
                    LValue sel;
                    sel.base          = baseptr;
                    sel.is_string     = true;
                    sel.slot_ty       = e.slot_ty;
                    sel.array_ty      = llvm::ArrayType::get(e.slot_ty, 0);
                    sel.elem_idx      = cast(phys, 32, false);
                    sel.is_signal     = lv.is_signal;
                    sel.sig           = lv.sig;
                    sel.dyn_inrange   = inrange;
                    sel.bounds_checks = lv.bounds_checks;
                    sel.epoch_chain   = lv.epoch_chain;
                    sel.epoch_chain.push_back(epoch);
                    add_elem_bounds_check(sel, i, lv, es.index, inrange, unknown, len);
                    set_lvalue(i, sel);
                    return;
                }
                if (e.is_agg || e.is_handle) {
                    LValue sel    = dyn_agg_elem_lvalue(lv, e, baseptr, phys, inrange);
                    sel.epoch_chain = lv.epoch_chain;
                    sel.epoch_chain.push_back(epoch);
                    note_gc_container_root(sel, lv, handle, es.array_type);
                    add_elem_bounds_check(sel, i, lv, es.index, inrange, unknown, len);
                    set_lvalue(i, sel);
                    return;
                }
                LValue       el;
                el.base          = baseptr;
                el.slot_ty       = e.slot_ty;
                el.has_z         = e.has_z;
                el.narrow        = e.narrow;
                el.array_ty      = llvm::ArrayType::get(e.slot_ty, 0);
                el.elem_idx      = cast(phys, 32, false);
                el.store_width   = e.width;
                el.bit_off       = b_.getInt32(0);
                el.width         = e.width;
                el.is_signal     = lv.is_signal;
                el.sig           = lv.sig;
                el.dyn_inrange   = inrange;
                el.bounds_checks = lv.bounds_checks;
                el.epoch_chain   = lv.epoch_chain;
                el.epoch_chain.push_back(epoch);
                el.gc_owner      = lv.gc_owner;
                note_gc_container_root(el, lv, handle, es.array_type);
                add_elem_bounds_check(el, i, lv, es.index, inrange, unknown, len);
                set_lvalue(i, el);
                return;
            }
            // A variable index into a fixed-size unpacked array has no static
            // guarantee of staying in range (LRM 7.4.5 / 11.5.1), and an X/Z
            // index is itself out of range (Table 7-1 draws no distinction
            // between an unknown index and one that misses the declared
            // bound -- both read the element default / no-op the write).
            // Normalize to a 0-based position and gate the element with the
            // same dyn_inrange machinery a dynamic-array/queue element uses:
            // out-of-range read yields the element default, write is a
            // no-op, the storage itself is never addressed out-of-bounds. An
            // unsigned compare also catches a normalized index that went
            // negative (wrapped to a huge unsigned value).
            const Plane  idxp = value_of(es.index);
            llvm::Value* idx  = cast(must_of(idxp), 32, false);
            // Normalize the declared-range coordinate to a 0-based position.
            llvm::Value* norm = at.left <= at.right ? b_.CreateSub(idx, b_.getInt32(at.left))
                                                    : b_.CreateSub(b_.getInt32(at.left), idx);
            const uint32_t count = (at.left <= at.right ? at.right - at.left : at.left - at.right) + 1;
            llvm::Value*   inrange =
                b_.CreateAnd(b_.CreateICmpULT(norm, b_.getInt32(count)), b_.CreateNot(any_unknown(idxp)));
            lv.dyn_inrange = lv.dyn_inrange != nullptr ? b_.CreateAnd(lv.dyn_inrange, inrange)
                                                       : inrange;
            add_elem_bounds_check(lv, i, lv, es.index, inrange, any_unknown(idxp), nullptr,
                                  static_cast<int32_t>(at.left), static_cast<int32_t>(at.right));
            if (lv.is_agg) {
                // Recursive storage is one ArrayType per dimension (not
                // flattened): push a nested-GEP index step. A runtime element
                // index makes the path non-static (deferred nonblocking path).
                // The GEP step itself still uses the raw (unclamped) norm --
                // an out-of-range dimension is caught by dyn_inrange above,
                // which gates the eventual leaf load/store, not the GEP path.
                lv.agg_path.push_back(norm);
                lv.agg_static = false;
                lv.agg_leaf   = is_leaf_type(at.element);
                if (lv.agg_leaf) {
                    fill_agg_leaf(lv, at.element);
                }
                set_lvalue(i, lv);
                return;
            }
            // Flat memory: advance the single flattened leaf index by
            // normalized_index * inner-dimension leaf count.
            const uint32_t stride = array_leaf_count(at.element);
            llvm::Value*   off    = stride == 1 ? norm : b_.CreateMul(norm, b_.getInt32(stride));
            lv.elem_idx           = lv.elem_idx != nullptr ? b_.CreateAdd(lv.elem_idx, off) : off;
            lv.bit_off            = b_.getInt32(0);
            set_lvalue(i, lv);
            return;
        }
        case Inst::Tag::AssocSelect: {
            // a[key] (LRM 7.8/7.9): build a deferred assoc element lvalue.
            // The get_ptr (read) vs put_ptr (write) resolution is left to the
            // Load / Store consumer (see assoc_elem_lvalue), which is also
            // where the key is normalized -- see the LValue field comment for
            // why that has to happen lazily, at each resolution, rather than
            // once here.
            const auto    aa = ir_->extra_data<AssocArg>(p.op_extra.extra);
            const DynElem e  = dynarr_elem(aa.assoc_type);
            // A deferred base means `a[k1][k2]`: the outer entry must be
            // resolved before its map handle can be read, and the direction is
            // this access's (AssocArg::for_write) -- resolving a READ with
            // put_ptr created the outer entry, which LRM 7.8.6 forbids.
            const LValue raw_hlv = lvalue_of(p.op_extra.op);
            const LValue hlv =
              raw_hlv.is_assoc ? assoc_elem_lvalue(raw_hlv, aa.for_write != 0) : raw_hlv;
            LValue el;
            el.is_assoc          = true;
            el.assoc_handle      = lv_handle_ptr(hlv, HandleUse::Place);
            // The MAP's change notifier travels onto the deferred element, so
            // that resolving this place for a write fires it: `a[k] = v` creates
            // or replaces an entry, which is a mutation of the property even when
            // the stored value is the one already there (LRM 9.4.3 waiters read
            // `a.num()` as often as `a[k]`). assoc_elem_lvalue is the one place
            // that resolution happens, so it is the one place that fires.
            el.chg         = hlv.chg;
            el.chg_body    = hlv.chg_body;
            el.chg_body_ty = hlv.chg_body_ty;
            // The signal-map analog of `chg`: `a[k] = v` on a module-level /
            // static-property map must wake the map SIGNAL's Any waiters.
            el.mut_sig_valid = hlv.is_signal || hlv.mut_sig_valid;
            el.mut_sig       = hlv.is_signal ? hlv.sig : hlv.mut_sig;
            // An entry's slot lives until that entry is erased, so a `ref`
            // through it records the map's relocation epoch, appended to whatever
            // containers the MAP itself sits inside (docs/design/data-model.md
            // §3.1) — `d[i][k]` watches the dynamic array and then the map.
            el.epoch_chain       = hlv.epoch_chain;
            el.epoch_chain.push_back(b_.CreateStructGEP(assoc_slot_ty(), el.assoc_handle,
                                                        handle_epoch_field(aa.assoc_type)));
            el.assoc_key_arg     = aa.arg;
            el.assoc_key_type    = aa.assoc_type;
            // x/z-key validity (LRM 7.8.4 / 7.8.5) is computed alongside the
            // normalized span, inside assoc_elem_lvalue -- not here.
            el.assoc_ops         = elem_ops_global(aa.assoc_type);
            el.assoc_elem_type   = std::get<Type>(mod_.pool.get(aa.assoc_type)).element;
            el.assoc_val_string  = e.is_string;
            el.assoc_val_handle  = e.is_handle;
            el.gc_owner          = hlv.gc_owner;
            note_gc_container_root(el, hlv, el.assoc_handle, aa.assoc_type);
            // A class-handle KEY is a reference the owner acquires when an entry is
            // created, even when the value is an int (`int m[Cell]`), and no
            // further member selection is party to it -- so it cannot ride the
            // value path's owner. assoc_elem_lvalue emits the barrier beside the
            // put_ptr that writes the key.
            el.assoc_key_handle  = el.gc_owner != nullptr &&
                                   assoc_key_is_class_handle(aa.assoc_type);
            if (e.is_agg) {
                // Aggregate value: a deferred is_agg lvalue. A following
                // MemberSelect / ElemSelect rides along (agg_member / agg_elem
                // only push the GEP path, never touch base); the base resolves
                // to the get_ptr / put_ptr'd slot at Load / Store time
                // (assoc_elem_lvalue). Leaf facts are filled by the descent.
                el.is_agg  = true;
                el.root_ty = e.agg_ty;
                set_lvalue(i, el);
                return;
            }
            // Managed value elements carry only the slot type (a string / a
            // container handle); the plane facts below are meaningless for
            // them and are lifted per-kind in assoc_elem_lvalue.
            el.slot_ty     = e.slot_ty;
            el.handle_type = e.elem_type;
            el.has_z       = e.has_z;
            el.narrow      = e.narrow;
            el.store_width = e.width;
            // A selection offset of zero, not a null one: a bit or part select
            // stacked on this deferred place ADDS to bit_off, and PartSelect's
            // unconditional CreateAdd crashed codegen on a null base. A packed
            // struct or union value element reaches exactly that path.
            el.bit_off     = b_.getInt32(0);
            el.width       = e.width;
            set_lvalue(i, el);
            return;
        }
        case Inst::Tag::AssocDefault: {
            // The `'{default: v}` slot (LRM 7.9.11): the same deferred assoc
            // lvalue as a[key], minus the key -- assoc_elem_lvalue resolves it
            // through rt_assoc_default_ptr, which always yields a live slot.
            const auto&   pt = p.op_type;
            const DynElem e  = dynarr_elem(pt.type);
            LValue        el;
            el.is_assoc         = true;
            el.assoc_default    = true;
            const LValue& dlv   = lvalue_of(pt.op);
            el.assoc_handle     = lv_handle_ptr(dlv, HandleUse::Place);
            // Same as a[k]: writing the pattern-default slot changes what an
            // absent key reads back, so it is a mutation of the property.
            el.chg              = dlv.chg;
            el.chg_body         = dlv.chg_body;
            el.chg_body_ty      = dlv.chg_body_ty;
            el.mut_sig_valid    = dlv.is_signal || dlv.mut_sig_valid;
            el.mut_sig          = dlv.is_signal ? dlv.sig : dlv.mut_sig;
            el.epoch_chain      = dlv.epoch_chain;
            el.epoch_chain.push_back(b_.CreateStructGEP(assoc_slot_ty(), el.assoc_handle,
                                                        handle_epoch_field(pt.type)));
            el.assoc_ops        = elem_ops_global(pt.type);
            el.assoc_elem_type  = std::get<Type>(mod_.pool.get(pt.type)).element;
            el.assoc_val_string = e.is_string;
            el.assoc_val_handle = e.is_handle;
            // No key span, so no key handle can be stored: the `'{default: v}`
            // slot is reached through rt_assoc_default_ptr (LRM 7.9.11).
            el.gc_owner         = dlv.gc_owner;
            note_gc_container_root(el, dlv, el.assoc_handle, pt.type);
            if (e.is_agg) {
                el.is_agg  = true;
                el.root_ty = e.agg_ty;
                set_lvalue(i, el);
                return;
            }
            el.slot_ty     = e.slot_ty;
            el.handle_type = e.elem_type;
            el.has_z       = e.has_z;
            el.narrow      = e.narrow;
            el.store_width = e.width;
            // A selection offset of zero, not a null one: a bit or part select
            // stacked on this deferred place ADDS to bit_off, and PartSelect's
            // unconditional CreateAdd crashed codegen on a null base. A packed
            // struct or union value element reaches exactly that path.
            el.bit_off     = b_.getInt32(0);
            el.width       = e.width;
            set_lvalue(i, el);
            return;
        }
        case Inst::Tag::MemberSelect: {
            const auto ms = ir_->extra_data<MemberSelect>(p.op_extra.extra);
            set_lvalue(i, agg_member(lvalue_of(p.op_extra.op), ms.struct_type, ms.field_index));
            return;
        }
        case Inst::Tag::ArrSlice: {
            // Pre-advance the base array lvalue to the slice's first element
            // (position 0). Downstream elem_select / agg_elem then index the
            // slice relative to this anchor. Flat-storage arrays only -- an
            // aggregate-element (array-of-struct) slice is frontend-guarded.
            LValue     lv = lvalue_of(p.op_extra.op);
            const auto as = ir_->extra_data<ArrSlice>(p.op_extra.extra);
            const Type& st = std::get<Type>(mod_.pool.get(as.slice_type));
            if (lv.is_agg) {
                throw BackendError{ "aggregate-element unpacked-array slice not supported" };
            }
            const uint32_t stride = array_leaf_count(st.element);
            if (as.first_pos != 0) {
                llvm::Value* off = b_.getInt32(as.first_pos * stride);
                lv.elem_idx      = lv.elem_idx != nullptr ? b_.CreateAdd(lv.elem_idx, off) : off;
            }
            lv.bit_off = b_.getInt32(0);
            set_lvalue(i, lv);
            return;
        }
        case Inst::Tag::IfaceRef: {
            // The handle a `vif = bus;` stores: the address of that instance's
            // context record, as an i64 (LRM 25.9). A link-time constant.
            const auto info = ir_->extra_data<IfaceInst>(p.extra);
            if (info.scope >= anchor_of_scope_.size() ||
                anchor_of_scope_[info.scope] == SignalSlot::no_anchor) {
                throw BackendError{ "internal: virtual interface names an unanchored scope" };
            }
            Anchor& a = anchors_[anchor_of_scope_[info.scope]];
            set_value(i, two_val(b_.CreatePtrToInt(ensure_ctx(a), b_.getInt64Ty())));
            return;
        }
        case Inst::Tag::IfaceSignal: {
            // A member of the instance the handle names. The representative
            // supplies every def-local constant -- field path, slot shape, wait
            // slots, shadow fields, dump-var offset -- and `vif_ctx` supplies
            // the instance those constants are resolved against, so this is the
            // ordinary signal lvalue with one substituted base.
            const auto   rep    = ir_->extra_data<IfaceMember>(p.op_extra.extra).signal;
            llvm::Value* handle = must_of(value_of(p.op_extra.op));
            emit_null_handle_check(handle, i, NullKind::Iface);
            llvm::Value* ctx = b_.CreateIntToPtr(handle, raw_ptr_ty());
            CtxOverride  ov(*this, ctx, slot_of(rep).anchor);
            LValue       lv = signal_lvalue(rep);
            lv.vif_ctx      = ctx;
            set_lvalue(i, lv);
            return;
        }
        case Inst::Tag::ClassNew: {
            // Allocate the object body from the collected heap, X-init its
            // 4-state leaves, init its string leaves, and yield the handle as an
            // i64 (reference semantics: the pointer is the value).
            //
            // The allocator returns the HANDLE -- the body base, one word above
            // the GC header -- with the body already zeroed (2-state and handle
            // leaves power on 0 / null) and the GC word already written, so no
            // memset is emitted here. The header sits BELOW the handle precisely
            // so that every property offset and the vptr store are plain
            // non-negative offsets off the value a program holds (Gc.h:41-46).
            //
            // The common case emits NO call at all: the bump sequence is inlined
            // (emit_gc_alloc_fast, design §4.3) and reaches rt_gc_alloc_slow only
            // when the mutator's current hole cannot satisfy the request. A class
            // at or above the large-object threshold has no hole to bump and keeps
            // the out-of-line rt_gc_alloc. Either way the mutator pointer costs
            // one call per emitted function rather than one per `new`
            // (gc_mutator_cur), and it stays a LOADED pointer rather than a
            // link-time constant address so that multi-threaded simulation stays
            // possible.
            const Ref    class_ref = p.ref;
            llvm::Value* body      = nullptr;
            if (gc_alloc_is_inline(class_ref)) {
                body = emit_gc_alloc_fast(class_ref);
            } else {
                body = b_.CreateCall(gc_alloc_fn(),
                                     { gc_mutator_cur(), b_.getInt64(gc_object_bytes(class_ref)),
                                       class_gc_map_global(class_ref) });
            }
            emit_class_body_init(class_ref, body);
            // Install the descriptor pointer at the object header (field 0), so
            // a later virtual dispatch (LRM 8.20) and a later `$cast` (LRM 8.16)
            // both resolve against the concrete type.
            b_.CreateStore(class_vptr(class_ref), b_.CreateStructGEP(class_body_ty(class_ref), body, 0));
            set_value(i, two_val(b_.CreatePtrToInt(body, b_.getInt64Ty())));
            return;
        }
        case Inst::Tag::HandleDeref: {
            // Turn a class handle value (i64 pointer bits) into an aggregate
            // lvalue rooted at the object body. No copy — the handle IS the
            // object (reference semantics), unlike a value struct addressed
            // inline. A following MemberSelect addresses a property.
            llvm::Value* handle = must_of(value_of(p.op_type.op));
            // A handle straight out of ClassNew is the address the allocator just
            // returned, so the property default-initializer stores every `new`
            // emits need no guard -- and no runtime declaration carries a nonnull
            // attribute (rt_fn adds none) for the optimizer to fold one away with.
            const bool fresh = ir_->tag(p.op_type.op) == Inst::Tag::ClassNew;
            if (!fresh) {
                emit_null_handle_check(handle, i);
            }
            LValue       lv{};
            lv.base      = b_.CreateIntToPtr(handle, raw_ptr_ty());
            lv.root_ty   = class_body_ty(p.op_type.type);
            lv.is_agg    = true;
            lv.is_signal = false;
            // THE place a write barrier's owner is established: every store into a
            // class property descends from here, and nothing else in this file
            // produces an object-rooted lvalue, so "no barrier for design state,
            // statics or frame locals" holds by construction (§7).
            //
            // The one exemption is the same fact the null check just used: a handle
            // straight out of ClassNew names an object rt_gc_alloc zeroed a few
            // instructions ago, whose GC word therefore has mark bits 0 and OLD
            // clear (Gc.cpp:985). Only a collection sets OLD, and no safepoint can
            // intervene inside a statement -- the sole poll site is the drain
            // loop's quiescent edge (:11542) -- so the barrier's test provably
            // cannot fire and the store needs none. This is exactly the "a `new` in
            // the same statement" case, decided rather than assumed.
            if (!fresh) {
                lv.gc_owner = lv.base;
            }
            set_lvalue(i, lv);
            return;
        }
        case Inst::Tag::ClassIsA: {
            // `$cast` type test (LRM 8.16). Read the destination's depth entry
            // out of the object's ancestor display and compare it against the
            // destination's id: equal exactly when the object's class is the
            // destination or descends from it, since display[d] is the ancestor
            // at depth d and is 0 past the object's own depth.
            //
            // Branch-free. A null handle reads the all-zero display of a static
            // sentinel object rather than faulting, and answers 1 separately:
            // assigning null to a cast-compatible destination is legal.
            const Ref        dest = p.op_type.type;
            const ClassDesc* cd   = mod_.class_desc(dest);
            if (cd == nullptr) {
                throw BackendError{ "internal: class without a descriptor" };
            }
            llvm::Value* handle = must_of(value_of(p.op_type.op));
            llvm::Value* isnull = b_.CreateICmpEQ(handle, b_.getInt64(0));
            llvm::Value* obj =
              b_.CreateSelect(isnull, null_object_global(), b_.CreateIntToPtr(handle, raw_ptr_ty()));
            llvm::Value*  vptr = b_.CreateLoad(raw_ptr_ty(), obj);
            const int64_t off  = static_cast<int64_t>(4 * class_depth(dest)) - 4 * kMaxClassDepth;
            llvm::Value* ent = b_.CreateLoad(b_.getInt32Ty(), b_.CreateGEP(b_.getInt8Ty(), vptr, b_.getInt64(off)));
            llvm::Value* ok  = b_.CreateICmpEQ(ent, b_.getInt32(cd->type_id));
            set_value(i, two_val(b_.CreateOr(ok, isnull)));
            return;
        }
        case Inst::Tag::ClassImplements: {
            // `$cast` to an interface-class type (LRM 8.26.5). The object's
            // descriptor carries a bitmap of the interfaces its class implements
            // -- transitively and through its base chain -- so the test is one
            // load of the word holding the destination interface's bit and one
            // mask, both offsets constant at the site.
            //
            // Branch-free, and null-safe by the same sentinel ClassIsA uses: the
            // stand-in object's bitmap is all zeros ("implements nothing"), and
            // the null answer comes from the separate compare against 0.
            const Ref                     iface = p.op_type.type;
            const std::optional<uint32_t> id    = mod_.iface_type_id(iface);
            if (!id.has_value()) {
                throw BackendError{ "internal: interface class without an id" };
            }
            llvm::Value* handle = must_of(value_of(p.op_type.op));
            llvm::Value* isnull = b_.CreateICmpEQ(handle, b_.getInt64(0));
            llvm::Value* obj =
              b_.CreateSelect(isnull, null_object_global(), b_.CreateIntToPtr(handle, raw_ptr_ty()));
            llvm::Value* vptr = b_.CreateLoad(raw_ptr_ty(), obj);
            const int64_t off = desc_field_vptr_offset(/*field=*/1) +
                                static_cast<int64_t>(*id / 64) * 8;
            llvm::Value* word =
              b_.CreateLoad(b_.getInt64Ty(), b_.CreateGEP(b_.getInt8Ty(), vptr, b_.getInt64(off)));
            llvm::Value* bit = b_.CreateAnd(word, b_.getInt64(uint64_t{ 1 } << (*id % 64)));
            llvm::Value* ok  = b_.CreateICmpNE(bit, b_.getInt64(0));
            set_value(i, two_val(b_.CreateOr(ok, isnull)));
            return;
        }

        case Inst::Tag::Load: {
            const LValue& raw = lvalue_of(p.unary);
            // A place reached through a virtual interface resolves every
            // def-relative base against the instance the handle named.
            CtxOverride ov(*this, raw.vif_ctx, lvalue_ctx_anchor(raw));
            emit_bounds_reports(raw, /*is_write=*/false);
            // An assoc element resolves its base FIRST (get_ptr for a read: an
            // absent key yields no slot and no entry is created, LRM 7.8.6), and
            // only then dispatches on the selected place's storage class. Doing
            // it in this order is what lets a MANAGED LEAF of an aggregate value
            // work -- `a[k].name` where `name` is a string reaches here with both
            // is_assoc and is_string set, and the leaf pointer cannot be formed
            // until the entry's slot is known.
            const LValue lv = raw.is_assoc ? assoc_elem_lvalue(raw, /*for_write=*/false) : raw;
            if (lv.is_handle) {
                // Defense in depth: a container handle has no plane value; every
                // legal consumer goes through the dedicated ops (frontend
                // value_cat dispatch) rather than a Load.
                throw BackendError{ "plane load of a container handle" };
            }
            if (lv.is_string) {
                // A string load is the storage handle pointer, not a copy:
                // every mutation point (Store, string ops) deep-copies, and
                // producers write into distinct temps. Out of range (or an absent
                // key) lv_string_ptr redirects to the owned-empty sink, so the
                // read yields "".
                set_str(i, lv_string_ptr(lv, StringUse::Read));
                return;
            }
            const ValType t = type_of(i);
            // A gated place (an out-of-range element index, or an absent assoc
            // key) reads the element default rather than its storage.
            Plane v = lv.dyn_inrange != nullptr ? dyn_elem_load(lv) : lower_load(lv);
            if (t.is_real) {
                v = real_plane(planes_to_real(v, t.width));
            }
            set_value(i, v);
            return;
        }
        case Inst::Tag::Store: {
            // A blocking assignment stores, then services the target signal's
            // wait queue: any process waiting for an edge this store produced
            // is woken. Snapshot the old value first so the transition can be
            // detected from old -> new.
            const LValue& raw = lvalue_of(p.binary.l);
            CtxOverride   ov(*this, raw.vif_ctx, lvalue_ctx_anchor(raw));
            emit_bounds_reports(raw, /*is_write=*/true);
            // As in Load, an assoc element resolves its base before anything
            // dispatches on the place's storage class -- put_ptr here, which
            // creates and proto-inits the entry (LRM 7.8: a write to a
            // nonexistent element adds it). Resolving first is what makes a
            // managed LEAF of an aggregate value writable.
            const bool   deferred = raw.is_assoc;
            const LValue lv       = deferred ? assoc_elem_lvalue(raw, /*for_write=*/true) : raw;
            if (lv.is_handle) {
                throw BackendError{ "plane store to a container handle" };
            }
            if (lv.is_string) {
                // A forwarded concat already built its result in the target
                // (build_str_temps handed it the target's handle). Only a Signal
                // or Local target can be forwarded, so an assoc element never is.
                if (!deferred && forwarded_string_store(i)) {
                    return;
                }
                // Deep copy, bracketed by the property's CONTENT compare: a
                // waited-on `obj.s` fires only when the characters differ (LRM
                // 9.4.2 over a 6.16 value), so `obj.s = obj.s` and a re-assignment
                // of equal text from a different handle wake nobody. A string
                // SIGNAL carries no notifier (refused in the frontend), so this
                // emits nothing for one.
                llvm::Value* snap = str_chg_snapshot(lv);
                b_.CreateCall(str_assign_fn(), { lv_string_ptr(lv, StringUse::Write), str_of(p.binary.r),
                                                 alloc_site_global(loc_text(i), "string") });
                str_chg_after(lv, snap);
                return;
            }
            // The generational write barrier (§7). One site covers all three store
            // paths below -- a deferred assoc element, a bounds-gated dynamic
            // element, and a plain leaf -- because gc_owner is non-null for exactly
            // the places whose storage a young collection does not scan: a class
            // property, an interior aggregate of one, or an element of a container
            // one owns. The string path returned above: a string leaf can never
            // hold a handle, so it never carries an owner.
            //
            // Emitted unconditionally rather than only for a class-typed RHS: the
            // owner test already proves a handle can land here, and a `obj.h = null`
            // that logs its owner costs a scan, never a miss. Placed BEFORE the
            // store only because a bounds-gated store may branch away; the
            // remembered set is consulted at the next collection either way, and no
            // safepoint can intervene (the sole poll site is the drain loop's
            // quiescent edge, :11542).
            emit_gc_barrier_for(lv);
            if (deferred) {
                // Assoc elements are guarded out of sensitivity lists, so no wait
                // servicing. An aggregate leaf reached through a runtime element
                // index into a fixed-array member carries a bounds gate; an
                // out-of-range write is a no-op (LRM 7.4.5), as is a write under
                // an invalid (x/z) key (7.8.6).
                if (lv.dyn_inrange != nullptr) {
                    dyn_elem_store(lv, value_of(p.binary.r));
                } else {
                    lower_store(lv, value_of(p.binary.r));
                }
                return;
            }
            if (lv.dyn_inrange != nullptr) {
                // Out-of-range write is a no-op (LRM 7.5.1): guard the store
                // (and its wait servicing) behind the in-range predicate.
                dyn_elem_store(lv, value_of(p.binary.r));
                return;
            }
            // The snapshot covers the selected storage (whole signal, or one
            // array element) so an element write wakes Any-edge waiters on a
            // change to that element rather than always reading element 0.
            const ServiceSnapshot old = service_snapshot(lv);
            lower_store(lv, value_of(p.binary.r));
            service_after_write(lv, old);
            return;
        }
        // A nonblocking assignment samples its RHS now but defers the write: it
        // stages the value into the target's shadow and enqueues the target for
        // the NBA update region (see emit_nba_region), which commits shadow ->
        // live and services wait queues only after every active process has run.
        case Inst::Tag::StoreNb: {
            const LValue& lv  = lvalue_of(p.binary.l);
            CtxOverride   ov(*this, lv.vif_ctx, lvalue_ctx_anchor(lv));
            emit_bounds_reports(lv, /*is_write=*/true);
            if (lv.is_string) {
                // A whole-string NBA stages an owned deep copy in its own record
                // and moves it into the target in the NBA region. Must precede
                // value_of below: a string operand has no plane form. No GC
                // barrier — a string can never hold a class handle.
                emit_string_whole_nba(lv, p.binary.r);
                return;
            }
            Plane         rhs = value_of(p.binary.r);
            if (type_of(p.binary.r).is_real) {
                rhs = real_to_planes(fp_of(rhs));
            }
            // The barrier for a nonblocking write into a class property (§7). It is
            // emitted at the STAGING site, not at the commit, because the commit is
            // the runtime's own memcpy out of a staging pool and carries no owner
            // identity. That is sound because staging and commit are never separated
            // by a safepoint: the NBA region drains before the drain loop reaches
            // its quiescent edge, which is the only place a collection can happen
            // (:11542). The staged image is itself a conservatively scanned root
            // while pending (Gc.h:301-306), so the handle cannot die in between
            // either.
            emit_gc_barrier_for(lv);
            // Deferral splits three ways: an unpacked-array element defers a
            // typed record; an aggregate leaf reached through a runtime array
            // index defers a memcpy record; everything else (a plain signal,
            // scalar select, or aggregate static leaf) uses the block shadow.
            const bool agg_partial =
              lv.is_agg && lv.agg_leaf && (lv.width != lv.store_width || !is_zero_const(lv.bit_off));
            if (lv.is_signal && (slot_of(lv.sig).is_array || slot_of(lv.sig).vif_rec)) {
                emit_array_nba(lv, rhs);
            } else if (lv.is_agg && (!lv.agg_static || agg_partial)) {
                emit_agg_nba(lv, rhs);
            } else {
                lower_store_nb(lv, rhs);
            }
            return;
        }
        case Inst::Tag::AggCopy: {
            // Whole-object blocking copy: one memcpy of the storage. The
            // frontend emits this only for a trivially copyable object, so the byte
            // image is the value (no heap-owning leaf to deep-copy).
            const auto info = ir_->extra_data<AggCopyInfo>(p.op_extra.extra);
            LValue     dlv  = lvalue_of(p.op_extra.op);
            LValue     slv  = lvalue_of(info.src);
            emit_bounds_reports(dlv, /*is_write=*/true);
            emit_bounds_reports(slv, /*is_write=*/false);
            // A whole (trivially-copyable) aggregate value of an associative
            // array: resolve the deferred slot to its map storage. The dst
            // put_ptr creates the entry; the src get_ptr may miss, redirecting
            // the copy source to the element x-init default (absent-key read).
            llvm::Value* src_override = nullptr;
            if (dlv.is_assoc) {
                dlv = assoc_elem_lvalue(dlv, /*for_write=*/true);
            }
            if (slv.is_assoc) {
                slv                  = assoc_elem_lvalue(slv, /*for_write=*/false);
                llvm::Value* slot    = agg_slot_ptr(slv);
                llvm::Value* proto   = agg_proto_global(info.type);
                src_override         = b_.CreateSelect(slv.dyn_inrange, slot, proto);
            }
            // A trivially copyable aggregate is exactly where a class handle hides
            // in a whole-object assignment: a handle IS trivially copyable (F4, a
            // bare i64 word), so `obj.rec = other.rec` memcpys handles into the
            // owner's image with no per-leaf store to instrument. Emitted after the
            // assoc resolution so `dlv` is the final destination, and before both
            // exits below so the bounds-gated path is covered too. gc_owner is
            // non-null only when the destination's type can reach a handle, so a
            // handle-free record copy emits nothing (§7).
            emit_gc_barrier_for(dlv);
            // A variable element index on either side (`dst = arr[i]`,
            // `arr[i] = src`) can miss the array. LRM 7.4.5: reading an
            // invalid element yields the Table 7-1 image of its type, and
            // writing one performs no operation. Both sides used to be GEPed
            // and memcpy'd regardless, which walks off the array -- a wild
            // index segfaults.
            //
            // Source: substitute the type's power-on image, exactly as the
            // absent-key case above already does. Destination: skip the copy
            // outright, which also has to cover the zdiff and wait-service
            // paths below, so it gates the whole body.
            if (src_override == nullptr && slv.dyn_inrange != nullptr) {
                llvm::Value* slot = slv.is_agg ? agg_slot_ptr(slv) : flat_elem_ptr(slv);
                src_override      = b_.CreateSelect(slv.dyn_inrange, slot,
                                                    agg_copy_proto(info.type, slv));
                slv.elem_idx      = nullptr; // the override is the whole source
            }
            if (dlv.dyn_inrange != nullptr) {
                auto* fn   = b_.GetInsertBlock()->getParent();
                auto* dobb = llvm::BasicBlock::Create(ctx_, "aggcopy.do", fn);
                auto* cont = llvm::BasicBlock::Create(ctx_, "aggcopy.cont", fn);
                b_.CreateCondBr(dlv.dyn_inrange, dobb, cont);
                b_.SetInsertPoint(dobb);
                LValue clean      = dlv;
                clean.dyn_inrange = nullptr;
                clean.bounds_checks.clear();
                emit_agg_copy(info, clean, slv, src_override);
                b_.CreateBr(cont);
                b_.SetInsertPoint(cont);
                return;
            }
            emit_agg_copy(info, dlv, slv, src_override);
            return;
        }

        // Nonblocking intra-assignment delay (`x <= #d y`, LRM 9.4.5): sample
        // the RHS now into a deferred record, arm it on the wheel for `now + d`,
        // and continue without suspending. The NBA region of `now + d` commits
        // it (emit_intra_commit).
        case Inst::Tag::StoreNbDelay: {
            const auto    info  = ir_->extra_data<StoreNbDelayInfo>(p.op_extra.extra);
            const LValue& lv    = lvalue_of(p.op_extra.op);
            emit_bounds_reports(lv, /*is_write=*/true);
            // No write barrier here, and the reason is structural rather than a
            // judgement: all three deferred stores take a whole-SIGNAL lvalue
            // (Ir.h:1147-1160) and their record subsystem is keyed by Signal
            // (intra_nba_sigs_, emit_intra_commit), so the target is design state --
            // scanned in full at every collection, young or not (Gc.h:105-107).
            //
            // That matters more here than at the other bare sites, because these
            // three are the one family whose commit lands in a LATER instant, across
            // safepoints, where an arm-site barrier could be consumed before the
            // store happens. So the shape is asserted away rather than trusted: if
            // the frontend ever widens these to a class property, this fails loudly
            // instead of silently missing a remembered-set entry (PRINCIPLES.md).
            assert(lv.gc_owner == nullptr && "a deferred store targets a signal");
            const Plane   rhs   = value_of(info.value);
            llvm::Value*  ticks = cast(must_of(value_of(info.delay)), 64, false);
            emit_intra_nba_arm(lv, rhs, ticks);
            return;
        }

        // Inertial continuous-assignment delay (`assign #d x = y`, LRM 10.3.3):
        // evaluate the RHS now, ask the runtime whether to (re)arm a detached
        // store — a change superseding a still-pending store cancels it (pulse
        // rejection) — and arm it on the wheel for `now + d`. Commits in the NBA
        // region of the fired instant (emit_intra_commit), like `x <= #d`.
        case Inst::Tag::StoreInertialDelay: {
            const auto    info  = ir_->extra_data<StoreNbDelayInfo>(p.op_extra.extra);
            const LValue& lv    = lvalue_of(p.op_extra.op);
            // Deliberately bare, and asserted rather than assumed: see StoreNbDelay.
            assert(lv.gc_owner == nullptr && "a deferred store targets a signal");
            const Plane   rhs   = value_of(info.value);
            llvm::Value*  ticks = cast(must_of(value_of(info.delay)), 64, false);
            emit_inertial_arm(lv, rhs, ticks);
            return;
        }

        // Nonblocking intra-assignment event (`x <= @e y`, LRM 9.4.5): sample the
        // RHS + count now into a deferred record, register it on the trigger's
        // edge, continue without suspending. The trigger's wake path fires it and
        // the NBA region commits (emit_intra_commit).
        case Inst::Tag::StoreNbEvent: {
            const auto    info  = ir_->extra_data<StoreNbEventInfo>(p.op_extra.extra);
            const LValue& lv    = lvalue_of(p.op_extra.op);
            emit_bounds_reports(lv, /*is_write=*/true);
            // Deliberately bare, and asserted rather than assumed: see StoreNbDelay.
            assert(lv.gc_owner == nullptr && "a deferred store targets a signal");
            const Plane  rhs   = value_of(info.value);
            llvm::Value* count = must_of(value_of(info.count));
            llvm::SmallVector<llvm::Value*, 4> ids;
            for (const IntraTrigger& tr : ir_->intra_trigs(p.op_extra.extra)) {
                ids.push_back(trigger_id_value(tr.signal, tr.edge));
            }
            emit_intra_event_arm(lv, rhs, count, ids);
            return;
        }

        case Inst::Tag::DynArrayNew:
            lower_dyn_array_new(i);
            return;
        case Inst::Tag::DynArrayNewInit:
            lower_dyn_array_new_init(i);
            return;
        case Inst::Tag::DynArraySize:
            lower_dyn_array_size(i);
            return;
        case Inst::Tag::DynArrayDelete: {
            const auto& pd = ir_->payload(i).op_type;
            emit_handle_delete(lv_handle_ptr(lvalue_of(pd.op), HandleUse::Mutate), pd.type);
            return;
        }
        case Inst::Tag::DynArrayCopy:
            lower_dyn_array_copy(i);
            return;
        case Inst::Tag::DynArrayCopyQueue:
            lower_dyn_array_copy_queue(i);
            return;

        case Inst::Tag::QueueSize:
            lower_queue_size(i);
            return;
        case Inst::Tag::QueuePushBack:
            lower_queue_push(i, /*front=*/false);
            return;
        case Inst::Tag::QueuePushFront:
            lower_queue_push(i, /*front=*/true);
            return;
        case Inst::Tag::QueuePopBack:
            lower_queue_pop(i, /*front=*/false);
            return;
        case Inst::Tag::QueuePopFront:
            lower_queue_pop(i, /*front=*/true);
            return;
        case Inst::Tag::QueueInsert:
            lower_queue_insert(i);
            return;
        case Inst::Tag::QueueDeleteIdx:
            lower_queue_delete_idx(i);
            return;
        case Inst::Tag::QueueDeleteAll:
            lower_queue_delete_all(i);
            return;
        case Inst::Tag::QueueCopy:
            lower_queue_copy(i);
            return;
        case Inst::Tag::QueueCopyDyn:
            lower_queue_copy_dynarr(i);
            return;
        case Inst::Tag::HandleCopyNb:
            emit_handle_whole_nba(i);
            return;
        case Inst::Tag::QueueAppendRange:
            lower_queue_append_range(i);
            return;

        case Inst::Tag::DynArrayReverse:
            lower_dyn_array_reverse(i);
            return;
        case Inst::Tag::DynArraySort:
            lower_dyn_array_sort(i, /*reversed=*/false);
            return;
        case Inst::Tag::DynArrayRsort:
            lower_dyn_array_sort(i, /*reversed=*/true);
            return;
        case Inst::Tag::DynArrayShuffle:
            lower_dyn_array_shuffle(i);
            return;
        case Inst::Tag::QueueReverse:
            lower_queue_reverse(i);
            return;
        case Inst::Tag::QueueSort:
            lower_queue_sort(i, /*reversed=*/false);
            return;
        case Inst::Tag::QueueRsort:
            lower_queue_sort(i, /*reversed=*/true);
            return;
        case Inst::Tag::QueueShuffle:
            lower_queue_shuffle(i);
            return;

        case Inst::Tag::AssocNum:
            lower_assoc_num(i);
            return;
        case Inst::Tag::AssocExists:
            lower_assoc_exists(i);
            return;
        case Inst::Tag::AssocDeleteKey:
            lower_assoc_delete_key(i);
            return;
        case Inst::Tag::AssocDeleteAll:
            lower_assoc_delete_all(i);
            return;
        case Inst::Tag::AssocReset:
            lower_assoc_reset(i);
            return;
        case Inst::Tag::AssocCopy:
            lower_assoc_copy(i);
            return;
        case Inst::Tag::AssocFirst:
        case Inst::Tag::AssocLast:
        case Inst::Tag::AssocNext:
        case Inst::Tag::AssocPrev:
            lower_assoc_traverse(i, ir_->tag(i));
            return;

        case Inst::Tag::Constant: {
            const Value& v = std::get<Value>(mod_.pool.get(p.ref));
            if (std::get<Type>(mod_.pool.get(v.type)).kind == Type::Kind::String) {
                lower_string_constant(i, p.ref);
                return;
            }
            set_value(i, lower_constant(p.ref));
            return;
        }

        case Inst::Tag::Add:
        case Inst::Tag::Sub:
        case Inst::Tag::Mul:
            set_value(i, lower_arith(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
            return;

        case Inst::Tag::Div:
        case Inst::Tag::Mod: {
            const ValType lt = type_of(p.binary.l);
            // Wide operands legalize to the 128-bit __udivti3 / __umodti3 (and signed
            // siblings) libcalls, resolved from compiler-rt/libgcc in the JIT (see run_jit).
            set_value(i, lower_divmod(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r), lt.is_signed));
            return;
        }

        case Inst::Tag::Pow: {
            // Result signedness follows the base (op1); the exponent (op2) is
            // self-determined. Both flags feed Table 11-4's signed corners.
            const ValType lt = type_of(p.binary.l);
            const ValType rt = type_of(p.binary.r);
            set_value(i, lower_pow(value_of(p.binary.l), value_of(p.binary.r), lt.is_signed, rt.is_signed));
            return;
        }

        case Inst::Tag::Band:
        case Inst::Tag::Bor:
        case Inst::Tag::Bxor:
        case Inst::Tag::Bxnor:
            set_value(i, lower_bitwise(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
            return;
        case Inst::Tag::WireRes:
        case Inst::Tag::WandRes:
        case Inst::Tag::WorRes:
            set_value(i, lower_net_res(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
            return;
        case Inst::Tag::StrengthRes:
            set_value(i, lower_strength_res(i));
            return;
        case Inst::Tag::Bnot:
            set_value(i, lower_bnot(value_of(p.unary)));
            return;

        case Inst::Tag::RedAnd:
        case Inst::Tag::RedOr:
        case Inst::Tag::RedXor:
            set_value(i, lower_reduce(ir_->tag(i), value_of(p.unary)));
            return;

        case Inst::Tag::Clog2:
            set_value(i, lower_clog2(value_of(p.unary)));
            return;

        case Inst::Tag::Ctz:
        case Inst::Tag::Clz:
            set_value(i, lower_bit_index(ir_->tag(i), value_of(p.unary)));
            return;

        case Inst::Tag::CountBits:
            set_value(i,
                      lower_count_bits(value_of(p.op_extra.op),
                                       ir_->extra_data<CountBitsInfo>(p.op_extra.extra).mask));
            return;

        case Inst::Tag::OneHot:
            set_value(i, lower_onehot(value_of(p.unary), /*allow_zero=*/false));
            return;

        case Inst::Tag::OneHot0:
            set_value(i, lower_onehot(value_of(p.unary), /*allow_zero=*/true));
            return;

        case Inst::Tag::IsUnknown:
            set_value(i, lower_isunknown(value_of(p.unary)));
            return;

        case Inst::Tag::Sll:
        case Inst::Tag::Srl:
        case Inst::Tag::Sra:
            set_value(
              i,
              lower_shift(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r), type_of(p.binary.l).is_signed));
            return;

        case Inst::Tag::Eq:
        case Inst::Tag::Ne:
        case Inst::Tag::Lt:
        case Inst::Tag::Le:
        case Inst::Tag::Gt:
        case Inst::Tag::Ge:
            if (type_of(p.binary.l).is_string) {
                set_value(i, lower_string_relation(ir_->tag(i), p.binary.l, p.binary.r));
                return;
            }
            set_value(i,
                      lower_compare(
                        ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r), type_of(p.binary.l).is_signed));
            return;

        case Inst::Tag::CaseEq:
        case Inst::Tag::CasezEq:
        case Inst::Tag::CasexEq:
            set_value(i, lower_case_compare(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
            return;

        case Inst::Tag::AggEq: {
            const auto info = ir_->extra_data<AggEqInfo>(p.op_extra.extra);
            set_value(i, lower_agg_eq(lvalue_of(p.op_extra.op), lvalue_of(info.rhs), info.type));
            return;
        }

        case Inst::Tag::WildEq:
            set_value(i, lower_wildeq(value_of(p.binary.l), value_of(p.binary.r)));
            return;

        case Inst::Tag::InsideSet:
            set_value(i, lower_inside_set(i));
            return;

        case Inst::Tag::FAdd:
        case Inst::Tag::FSub:
        case Inst::Tag::FMul:
        case Inst::Tag::FDiv:
        case Inst::Tag::FPow:
            set_value(i, lower_real_arith(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
            return;
        case Inst::Tag::FNeg:
            set_value(i, real_plane(b_.CreateFNeg(fp_of(value_of(p.unary)))));
            return;
        case Inst::Tag::FEq:
        case Inst::Tag::FNe:
        case Inst::Tag::FLt:
        case Inst::Tag::FLe:
        case Inst::Tag::FGt:
        case Inst::Tag::FGe:
            set_value(i, lower_real_compare(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
            return;

        case Inst::Tag::ItoF: {
            // The must half reads x/z bits as 0 (LRM 6.12.1); signedness
            // comes from the source operand type.
            const ValType st = type_of(p.op_type.op);
            const ValType dt = from_ref(p.op_type.type);
            llvm::Value*  iv = must_of(value_of(p.op_type.op));
            llvm::Value*  fv =
              st.is_signed ? b_.CreateSIToFP(iv, fp_ty(dt.width)) : b_.CreateUIToFP(iv, fp_ty(dt.width));
            set_value(i, real_plane(fv));
            return;
        }
        case Inst::Tag::FtoI:
        case Inst::Tag::FtoITrunc:
            set_value(i, lower_ftoi(ir_->tag(i), value_of(p.op_type.op), from_ref(p.op_type.type)));
            return;
        case Inst::Tag::FBitcast: {
            const ValType st = type_of(p.op_type.op);
            const ValType dt = from_ref(p.op_type.type);
            if (dt.is_real) {
                // $bitstoreal / $bitstoshortreal: integral pattern -> float.
                llvm::Value* pat = cast(must_of(value_of(p.op_type.op)), dt.width, false);
                set_value(i, real_plane(b_.CreateBitCast(pat, fp_ty(dt.width))));
            } else {
                // $realtobits / $shortrealtobits: float -> integral pattern.
                llvm::Value* pat = b_.CreateBitCast(fp_of(value_of(p.op_type.op)), int_ty(st.width));
                pat              = cast(pat, dt.width, false);
                set_value(i, packed(pat, pat, zero(dt.width)));
            }
            return;
        }
        case Inst::Tag::FResize: {
            const ValType st = type_of(p.op_type.op);
            const ValType dt = from_ref(p.op_type.type);
            llvm::Value*  v  = fp_of(value_of(p.op_type.op));
            if (st.width < dt.width) {
                v = b_.CreateFPExt(v, fp_ty(dt.width));
            } else if (st.width > dt.width) {
                v = b_.CreateFPTrunc(v, fp_ty(dt.width));
            }
            set_value(i, real_plane(v));
            return;
        }
        case Inst::Tag::RealMathUnary:
            set_value(i, lower_real_math_unary(i));
            return;
        case Inst::Tag::RealMathBinary:
            set_value(i, lower_real_math_binary(i));
            return;

        case Inst::Tag::Land:
        case Inst::Tag::Lor:
            set_value(i, lower_logical(ir_->tag(i), value_of(p.binary.l), value_of(p.binary.r)));
            return;
        case Inst::Tag::Lnot:
            set_value(i, lower_lnot(value_of(p.unary)));
            return;
        case Inst::Tag::Limplies:
            set_value(i, lower_limplies(value_of(p.binary.l), value_of(p.binary.r)));
            return;
        case Inst::Tag::Lequiv:
            set_value(i, lower_lequiv(value_of(p.binary.l), value_of(p.binary.r)));
            return;

        case Inst::Tag::Select: {
            const auto select = ir_->extra_data<Select>(p.op_extra.extra);
            if (type_of(select.a).is_string) {
                set_str(i, lower_string_mux(value_of(p.op_extra.op), select.a, select.b));
            } else if (type_of(select.a).is_real) {
                set_value(i, lower_real_mux(value_of(p.op_extra.op), value_of(select.a), value_of(select.b)));
            } else {
                set_value(i, lower_select(value_of(p.op_extra.op), value_of(select.a), value_of(select.b)));
            }
            return;
        }
        case Inst::Tag::Concat:
            if (type_of(i).is_string) {
                lower_string_concat(i);
                return;
            }
            set_value(i, lower_concat(i));
            return;
        case Inst::Tag::Repl:
            // Depth guard: a string replication lowers to StrRepl (runtime
            // count); Repl's count-times-width result type has no meaning
            // for a string operand.
            if (type_of(p.repl.value).is_string) {
                throw BackendError{ "internal: string value in repl" };
            }
            set_value(i, lower_repl(value_of(p.repl.value), type_of(p.repl.value).width, p.repl.count));
            return;
        case Inst::Tag::StrRepl: {
            // {count{s}} with a RUNTIME count — the deliberate LRM 11.4.12.1
            // string deviation (see lower/Expr.cpp's Replication case). Zero is
            // the clause's own legal case and yields "" in rt_string_repl;
            // a negative count is refused there (the count crosses the ABI
            // signed, like rt_dynarr_new's). An x/z count is probed HERE,
            // because only the JIT still sees the planes — and only when the
            // count's plane is 4-state, so a 2-state count (`int m`, the UVM
            // shape) pays nothing.
            const Plane     count = value_of(p.binary.r);
            llvm::Constant* site  = alloc_site_global(loc_text(i), "string replication");
            if (!count.two) {
                auto* fn   = b_.GetInsertBlock()->getParent();
                auto* bad  = llvm::BasicBlock::Create(ctx_, "strrepl.xz", fn);
                auto* cont = llvm::BasicBlock::Create(ctx_, "strrepl.ok", fn);
                auto* br   = b_.CreateCondBr(any_unknown(count), bad, cont);
                br->setMetadata(llvm::LLVMContext::MD_prof,
                                llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));
                b_.SetInsertPoint(bad);
                llvm::Value* now  = b_.CreateCall(wheel_now_fn(), { wheel_ });
                auto*        call = b_.CreateCall(str_repl_invalid_fn(),
                                                  { b_.getInt64(0), b_.getInt32(1), site, now });
                call->addFnAttr(llvm::Attribute::NoReturn);
                call->addFnAttr(llvm::Attribute::Cold);
                b_.CreateUnreachable();
                b_.SetInsertPoint(cont);
            }
            llvm::Value* n = cast(must_of(count), 64, type_of(p.binary.r).is_signed);
            b_.CreateCall(str_repl_fn(), { str_of(i), str_of(p.binary.l), n, site });
            return;
        }
        case Inst::Tag::StreamReverse:
            set_value(i, lower_stream_reverse(value_of(p.op_imm.op), p.op_imm.imm));
            return;
        case Inst::Tag::NetPull:
            set_value(i, lower_net_pull(value_of(p.op_imm.op), p.op_imm.imm));
            return;
        case Inst::Tag::Resize: {
            // Truncate or extend all planes (sign per target type).
            if (type_of(p.op_type.op).is_string) {
                throw BackendError{ "internal: string value in resize" };
            }
            const ValType to = from_ref(p.op_type.type);
            // Depth guard: an integer resize would silently zext / trunc a
            // float bit pattern. The frontend must emit ItoF / FtoI /
            // FResize instead.
            if (type_of(p.op_type.op).is_real || to.is_real) {
                throw BackendError{ "internal: resize on a real value" };
            }
            // Resize is also the domain-conversion site: slang lowers an
            // implicit 2-state <-> 4-state conversion (LRM 6.3.1) as a
            // ConversionExpression -> Resize to the target type.
            set_value(i, convert(value_of(p.op_type.op), to.width, to.four_state, to.is_signed));
            return;
        }

        case Inst::Tag::StrLen: {
            llvm::Value* n = b_.CreateCall(str_len_fn(), { str_of(p.unary) });
            llvm::Value* v = b_.CreateTrunc(n, int_ty(32));
            set_value(i, packed(v, v, zero(32)));
            return;
        }
        case Inst::Tag::StrGetc: {
            llvm::Value* str = str_of(p.binary.l);
            llvm::Value* idx =
              cast(must_of(value_of(p.binary.r)), 32, type_of(p.binary.r).is_signed);
            emit_string_bounds_check(i, str, p.binary.r, "<string>", /*is_write=*/false);
            llvm::Value* c = b_.CreateCall(str_getc_fn(), { str, idx });
            llvm::Value* v = b_.CreateTrunc(c, int_ty(8));
            set_value(i, packed(v, v, zero(8)));
            return;
        }
        case Inst::Tag::StrPutc: {
            const auto    sp = ir_->extra_data<StrPutcInfo>(p.op_extra.extra);
            const LValue& lv = lvalue_of(p.op_extra.op);
            emit_bounds_reports(lv, /*is_write=*/true);
            llvm::Value*  snap = str_chg_snapshot(lv);
            llvm::Value*  str  = lv_string_ptr(lv, StringUse::Write);
            llvm::Value*  idx =
              cast(must_of(value_of(sp.index)), 32, type_of(sp.index).is_signed);
            llvm::Value* ch = cast(must_of(value_of(sp.ch)), 32, false);
            emit_string_bounds_check(i, str, sp.index, bounds_name(lv), /*is_write=*/true);
            b_.CreateCall(str_putc_fn(), { str, idx, ch });
            str_chg_after(lv, snap);
            return;
        }
        case Inst::Tag::StrAtoi:
        case Inst::Tag::StrAtohex:
        case Inst::Tag::StrAtooct:
        case Inst::Tag::StrAtobin: {
            using T          = Inst::Tag;
            const T tag      = ir_->tag(i);
            auto    fn       = tag == T::StrAtoi    ? str_atoi_fn()
                              : tag == T::StrAtohex ? str_atohex_fn()
                              : tag == T::StrAtooct ? str_atooct_fn()
                                                    : str_atobin_fn();
            llvm::Value* v = b_.CreateCall(fn, { str_of(p.unary) });
            set_value(i, packed(v, v, zero(32)));
            return;
        }
        case Inst::Tag::StrAtoreal: {
            llvm::Value* v = b_.CreateCall(str_atoreal_fn(), { str_of(p.unary) });
            set_value(i, real_plane(v));
            return;
        }
        case Inst::Tag::StrItoa:
        case Inst::Tag::StrHextoa:
        case Inst::Tag::StrOctoa:
        case Inst::Tag::StrBintoa: {
            using T          = Inst::Tag;
            const T tag      = ir_->tag(i);
            auto    fn       = tag == T::StrItoa      ? str_itoa_fn()
                              : tag == T::StrHextoa   ? str_hextoa_fn()
                              : tag == T::StrOctoa    ? str_octtoa_fn()
                                                      : str_bintoa_fn();
            const LValue& lv = lvalue_of(p.binary.l);
            llvm::Value*  v =
              cast(must_of(value_of(p.binary.r)), 32, type_of(p.binary.r).is_signed);
            llvm::Value* snap = str_chg_snapshot(lv);
            b_.CreateCall(
              fn, { lv_string_ptr(lv, StringUse::Write), v, alloc_site_global(loc_text(i), "string") });
            str_chg_after(lv, snap);
            return;
        }
        case Inst::Tag::StrRealtoa: {
            const LValue& lv = lvalue_of(p.binary.l);
            llvm::Value*  v  = fp_of(value_of(p.binary.r));
            llvm::Value* snap = str_chg_snapshot(lv);
            b_.CreateCall(str_realtoa_fn(), { lv_string_ptr(lv, StringUse::Write), v,
                                              alloc_site_global(loc_text(i), "string") });
            str_chg_after(lv, snap);
            return;
        }
        case Inst::Tag::StrCmp:
        case Inst::Tag::StrICmp: {
            auto         fn = ir_->tag(i) == Inst::Tag::StrCmp ? str_cmp_fn() : str_icmp_fn();
            llvm::Value* c  = b_.CreateCall(fn, { str_of(p.binary.l), str_of(p.binary.r) });
            set_value(i, packed(c, c, zero(32)));
            return;
        }
        case Inst::Tag::StrUpper:
        case Inst::Tag::StrLower: {
            auto fn = ir_->tag(i) == Inst::Tag::StrUpper ? str_upper_fn() : str_lower_fn();
            b_.CreateCall(
              fn, { str_of(i), str_of(p.unary), alloc_site_global(loc_text(i), "string") });
            return;
        }
        case Inst::Tag::StrSubstr: {
            const auto   ss = ir_->extra_data<StrSubstrInfo>(p.op_extra.extra);
            llvm::Value* si = cast(must_of(value_of(ss.i)), 32, type_of(ss.i).is_signed);
            llvm::Value* sj = cast(must_of(value_of(ss.j)), 32, type_of(ss.j).is_signed);
            b_.CreateCall(str_substr_fn(), { str_of(i), str_of(p.op_extra.op), si, sj,
                                             alloc_site_global(loc_text(i), "string") });
            return;
        }
        case Inst::Tag::EnumName: {
            // Materialize the member table: one contiguous value-bytes blob
            // plus parallel name-pointer / length globals, then let the runtime
            // byte-compare the operand's must plane against each member value.
            const auto         info   = ir_->extra_data<EnumNameInfo>(p.op_extra.extra);
            const auto         start  = static_cast<uint32_t>(info.entries.start);
            const auto         end    = static_cast<uint32_t>(info.entries.end);
            constexpr uint32_t stride = sizeof(EnumNameEntry) / sizeof(uint32_t);
            llvm::Type* const  i32    = b_.getInt32Ty();
            std::string                  blob; // count * vbytes value bytes
            std::vector<llvm::Constant*> names, lens;
            uint32_t                     vbytes = 0;
            for (uint32_t w = start; w < end; w += stride) {
                const auto e = ir_->extra_data<EnumNameEntry>(static_cast<ExtraIndex>(w));
                vbytes       = e.val_len;
                blob += ir_->extra_bytes(e.val_at, e.val_len);
                names.push_back(b_.CreateGlobalString(ir_->extra_bytes(e.name_at, e.name_len)));
                lens.push_back(llvm::ConstantInt::get(i32, e.name_len));
            }
            const auto count   = static_cast<uint32_t>(names.size());
            auto*      ptr_arr = llvm::ArrayType::get(raw_ptr_ty(), count);
            auto*      len_arr = llvm::ArrayType::get(i32, count);
            auto*      blob_c  = llvm::ConstantDataArray::get(
              ctx_, llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(blob.data()), blob.size()));
            auto make_gv = [&](llvm::Type* ty, llvm::Constant* init, const char* nm) {
                return new llvm::GlobalVariable(env_->llmod, ty, true, llvm::GlobalValue::InternalLinkage, init, nm);
            };
            llvm::Value* vg = make_gv(blob_c->getType(), blob_c, "clotho.enum.vals");
            llvm::Value* ng = make_gv(ptr_arr, llvm::ConstantArray::get(ptr_arr, names), "clotho.enum.names");
            llvm::Value* lg = make_gv(len_arr, llvm::ConstantArray::get(len_arr, lens), "clotho.enum.lens");
            // Spill the operand's must plane to a byte buffer for the compare.
            const uint32_t spill  = plane_bytes(type_of(p.op_extra.op).width);
            llvm::Value*   valbuf = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(spill), "enum.val");
            b_.CreateAlignedStore(must_of(value_of(p.op_extra.op)), valbuf, llvm::Align(1));
            b_.CreateCall(enum_name_fn(), { str_of(i), valbuf, b_.getInt32(vbytes),
                                            b_.getInt32(count), vg, ng, lg,
                                            b_.getInt32(type_of(p.op_extra.op).width),
                                            b_.getInt32(info.fmt_fallback),
                                            b_.getInt32(type_of(p.op_extra.op).is_signed ? 1 : 0),
                                            alloc_site_global(loc_text(i), "string") });
            return;
        }
        case Inst::Tag::ContainerFmtP: {
            // %p of a queue / dynamic array: render at runtime over the live
            // element count; the result lands in this instruction's string temp.
            const auto&        pt = p.op_type;
            const ContainerFmt cf = container_fmt(pt.type);
            b_.CreateCall(container_fmt_p_fn(),
                          { str_of(i),
                            lv_handle_ptr(lvalue_of(pt.op), HandleUse::Read),
                            b_.getInt32(cf.is_queue),
                            b_.getInt32(cf.kind),
                            b_.getInt32(cf.width),
                            b_.getInt32(cf.is_signed),
                            b_.getInt32(cf.four),
                            b_.getInt32(cf.has_z),
                            b_.getInt32(cf.z_off),
                            b_.getInt64(cf.stride),
                            alloc_site_global(loc_text(i), "string") });
            return;
        }
        case Inst::Tag::TaggedFmtP: {
            // %p of a tagged union: only the run-time tag says which member is
            // valid, so unlike a struct this cannot be expanded to literal text
            // at lower time. Marshal the flat value through the ordinary rt_arg
            // path (planes of any width) and let the runtime pick the member.
            const auto  info = ir_->extra_data<TaggedFmtInfo>(p.op_extra.extra);
            const Index val  = p.op_extra.op;
            llvm::Value* const arr = build_rt_args({ &val, 1 }, "%p of a tagged union");

            constexpr uint32_t stride = sizeof(TaggedFmtEntry) / sizeof(uint32_t);
            llvm::Type* const  i32    = b_.getInt32Ty();
            std::vector<llvm::Constant*> names, lens, widths, signs;
            for (uint32_t w = static_cast<uint32_t>(info.members.start);
                 w < static_cast<uint32_t>(info.members.end); w += stride) {
                const auto e = ir_->extra_data<TaggedFmtEntry>(static_cast<ExtraIndex>(w));
                names.push_back(b_.CreateGlobalString(ir_->extra_bytes(e.name_at, e.name_len)));
                lens.push_back(llvm::ConstantInt::get(i32, e.name_len));
                widths.push_back(llvm::ConstantInt::get(i32, e.width));
                signs.push_back(llvm::ConstantInt::get(i32, e.is_signed));
            }
            const auto count = static_cast<uint32_t>(names.size());

            auto* const nty = llvm::ArrayType::get(raw_ptr_ty(), count);
            auto* const uty = llvm::ArrayType::get(i32, count);
            auto* const ng  = new llvm::GlobalVariable(env_->llmod, nty, /*isConstant=*/true,
                                                       llvm::GlobalValue::PrivateLinkage,
                                                       llvm::ConstantArray::get(nty, names),
                                                       "clotho.tagfmt.names");
            auto* const lg  = new llvm::GlobalVariable(env_->llmod, uty, /*isConstant=*/true,
                                                       llvm::GlobalValue::PrivateLinkage,
                                                       llvm::ConstantArray::get(uty, lens),
                                                       "clotho.tagfmt.lens");
            auto* const wg  = new llvm::GlobalVariable(env_->llmod, uty, /*isConstant=*/true,
                                                       llvm::GlobalValue::PrivateLinkage,
                                                       llvm::ConstantArray::get(uty, widths),
                                                       "clotho.tagfmt.widths");
            auto* const sg  = new llvm::GlobalVariable(env_->llmod, uty, /*isConstant=*/true,
                                                       llvm::GlobalValue::PrivateLinkage,
                                                       llvm::ConstantArray::get(uty, signs),
                                                       "clotho.tagfmt.signs");

            b_.CreateCall(tagged_fmt_p_fn(),
                          { str_of(i), arr, b_.getInt32(info.width), b_.getInt32(info.tag_bits),
                            b_.getInt32(count), ng, lg, wg, sg,
                            alloc_site_global(loc_text(i), "string") });
            return;
        }
        case Inst::Tag::StrFromPacked: {
            // Spill the operand's val plane to a stack buffer (X bits read as
            // 0 — the runtime consumes must-bits only, a documented
            // simplification of the LRM cast).
            const ValType  t   = type_of(p.unary);
            const uint32_t pb  = plane_bytes(t.width);
            llvm::Value*   buf = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(pb), "str.pack");
            b_.CreateAlignedStore(must_of(value_of(p.unary)), buf, llvm::Align(1));
            b_.CreateCall(str_from_packed_fn(), { str_of(i), buf, b_.getInt32(t.width),
                                                  alloc_site_global(loc_text(i), "string") });
            return;
        }
        case Inst::Tag::StrToPacked: {
            const ValType  to  = from_ref(p.op_type.type);
            const uint32_t pb  = plane_bytes(to.width);
            llvm::Value*   buf = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(pb), "str.unpack");
            b_.CreateCall(str_to_packed_fn(), { str_of(p.op_type.op), buf, b_.getInt32(to.width) });
            llvm::Value* v = b_.CreateAlignedLoad(int_ty(to.width), buf, llvm::Align(1));
            set_value(i, packed(v, v, zero(to.width)));
            return;
        }
        case Inst::Tag::StrAppendPacked: {
            // Same must-plane spill as StrFromPacked above; the runtime keeps
            // 0x00 bytes here because a stream has a known bit count.
            const ValType  t   = type_of(p.binary.r);
            const uint32_t pb  = plane_bytes(t.width);
            llvm::Value*   buf = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(pb), "str.append");
            b_.CreateAlignedStore(must_of(value_of(p.binary.r)), buf, llvm::Align(1));
            const LValue& alv  = lvalue_of(p.binary.l);
            llvm::Value*  snap = str_chg_snapshot(alv);
            b_.CreateCall(str_append_packed_fn(),
                          { lv_string_ptr(alv, StringUse::Write), buf, b_.getInt32(t.width),
                            alloc_site_global(loc_text(i), "string") });
            str_chg_after(alv, snap);
            return;
        }

        case Inst::Tag::Cond:
            lower_cond(i);
            return;

        case Inst::Tag::Switch:
            lower_switch(i);
            return;

        case Inst::Tag::SelectLazy:
            lower_select_lazy(i);
            return;

        case Inst::Tag::Loop:
            lower_loop(i);
            return;

        case Inst::Tag::Break:
            // Terminator: branch to the innermost loop's exit block. slang
            // guarantees a break appears only inside a loop.
            b_.CreateBr(loop_targets_.back().second);
            return;
        case Inst::Tag::Continue:
            // Terminator: branch to the innermost loop's step block (which
            // re-tests the predicate via the back-edge).
            b_.CreateBr(loop_targets_.back().first);
            return;

        case Inst::Tag::NamedBlock:
            lower_named_block(i);
            return;
        case Inst::Tag::Disable:
            // Terminator: branch to the end of the enclosing block carrying
            // this label (LRM 9.6.2). The frontend only emits a Disable for a
            // block it is lexically inside, so the label is always live here.
            b_.CreateBr(block_end(p.label));
            return;
        case Inst::Tag::DisableRemote:
            // Not a terminator: the disabling process carries on, and the target
            // activation aborts itself at its own poll point (LRM 9.6.2).
            lower_disable_remote(i);
            return;
        case Inst::Tag::DeferFlush:
            // LRM 16.4.4: disabling a procedure's outermost scope drops its
            // pending deferred reports. Same generation bump an activation start
            // does, so a report queued before this point never matures. A target
            // id names ANOTHER procedure's counter -- the remote case, where the
            // reports to drop are not this group's.
            if (p.label == kNoDisableTarget) {
                emit_defer_gen_bump();
            } else {
                emit_defer_gen_bump_at(defer_gen_ptr_for(group_of_[mod_.disable_flush_proc[p.label]]));
            }
            return;
        case Inst::Tag::DisableAbort:
            // Terminator: this activation was named by a `disable` it ran itself
            // (LRM 9.6.2 example 4). Leave through the epilogue, which sees the
            // abort mark and skips copy-out.
            b_.CreateBr(task_epilogue_bb_);
            return;

        case Inst::Tag::Delay:
            // A suspend ends the current NBA segment: post-suspend NBAs stage at a
            // later instant and must commit in a different NBA region.
            ++cur_seg_epoch_;
            lower_delay(p.unary);
            return;

        case Inst::Tag::Wait:
            ++cur_seg_epoch_;
            lower_wait(p.extra);
            return;

        case Inst::Tag::EventNew:
            // LRM Table 6-7's "New event": bind a fresh object and yield its
            // handle. Only storage whose power-on handle cannot be a compile-time
            // constant reaches here.
            set_value(i, two_val(b_.CreateCall(event_new_fn())));
            return;

        case Inst::Tag::EventTrigger:
            // `-> e` (LRM 15.5.2): resolve the handle, stamp the trigger time and
            // wake every waiter now. Zero-time — the current process does not
            // suspend, so it does not end the NBA segment.
            emit_event_fire_guarded(must_of(value_of(p.unary)), wheel_);
            return;

        case Inst::Tag::EventTriggerNb:
            // `->> e` (LRM 15.5.4): link the object onto the pending chain; the
            // NBA region stamps + wakes. The triggering process does not block.
            emit_event_pend_guarded(must_of(value_of(p.unary)));
            return;

        case Inst::Tag::EventTriggerNbEvent: {
            // `->> @(x) e`: register a detached one-shot subscription on the
            // edge and carry on. The wake comes from fire_event_trigger on that
            // (signal, edge), which pends the event for the NBA region.
            const auto info = ir_->extra_data<EventDetachInfo>(p.op_extra.extra);
            b_.CreateCall(event_detach_register_fn(),
                          { trigger_id_value(info.signal, info.edge),
                            must_of(value_of(p.op_extra.op)), b_.getInt32(info.count) });
            return;
        }

        case Inst::Tag::EventTriggerNbDelayed: {
            // `->> #d e` (LRM 15.5.4): schedule a detached wheel timer that fires
            // the event `d` ticks from now. The process does not block. The timer
            // node carries the handle, so ONE thunk serves the whole design (the
            // target may be any event expression, not a fixed declared one).
            llvm::Value* handle   = must_of(value_of(p.binary.r));
            llvm::Value* ticks    = cast(must_of(value_of(p.binary.l)), 64, false);
            llvm::Value* deadline = b_.CreateAdd(b_.CreateCall(wheel_now_fn(), { wheel_ }), ticks);
            auto*        nty      = event_timer_ty();
            llvm::Value* node     = b_.CreateCall(
              alloc_fn(), { b_.getInt64(env_->llmod.getDataLayout().getTypeAllocSize(nty)) });
            b_.CreateStore(handle, b_.CreateStructGEP(nty, node, 2));
            b_.CreateCall(wheel_schedule_fn(), { wheel_, node, deadline, event_delay_thunk() });
            return;
        }

        case Inst::Tag::Display:
            lower_display(i);
            return;

        case Inst::Tag::FDisplay:
            lower_fdisplay(i);
            return;

        case Inst::Tag::StrobePend:
            lower_strobe_pend(i);
            return;
        case Inst::Tag::ReactivePend:
            lower_reactive_pend(i);
            return;
        case Inst::Tag::ExpectWait:
            lower_expect_wait(i);
            return;
        case Inst::Tag::ExpectWake:
            lower_expect_wake(i);
            return;
        case Inst::Tag::ProcQuery:
            lower_proc_query(i);
            return;
        case Inst::Tag::ProcCtl:
            lower_proc_ctl(i);
            return;
        case Inst::Tag::ProcWait:
            lower_proc_wait(i);
            return;
        case Inst::Tag::SyncWait:
            lower_sync_wait(i);
            return;
        case Inst::Tag::SyncCtl:
            lower_sync_ctl(i);
            return;
        case Inst::Tag::CoverQuery:
            lower_cover_query(i);
            return;
        case Inst::Tag::CoverCtl:
            lower_cover_ctl(i);
            return;
        case Inst::Tag::MonitorSet:
            b_.CreateCall(monitor_set_fn(),
                          { sym(fn_decls_[static_cast<uint32_t>(
                              ir_->extra_data<StrobePendInfo>(ir_->payload(i).extra).func)]) });
            return;
        case Inst::Tag::MonitorEmit: {
            const auto& p = ir_->payload(i).op_extra;
            emit_display_to(ir_->extra_data<DisplayInfo>(p.extra),
                            cast(must_of(value_of(p.op)), 32, true), monitor_emit_fn());
            return;
        }
        case Inst::Tag::MonitorOn:
            b_.CreateCall(monitor_enable_fn(), { b_.getInt32(1) });
            return;
        case Inst::Tag::MonitorOff:
            b_.CreateCall(monitor_enable_fn(), { b_.getInt32(0) });
            return;

        case Inst::Tag::TimeFormat:
            lower_timeformat(i);
            return;

        case Inst::Tag::Finish:
            // $finish / $stop: rt_finish prints the banner then exits, never
            // returning (noreturn), so the optimizer prunes the region tail.
            lower_finish(i);
            return;

        case Inst::Tag::ProgramExit:
            // $exit (LRM 24.7): end this program's `initial` procedures and their
            // descendants, then leave through this body's abort exit.
            lower_program_exit(i);
            return;

        case Inst::Tag::CaseViol:
            // unique/priority case violation: the report is queued for the
            // Observed region (rt_observed_case_violation), not emitted here,
            // so the Active region continues normally after it.
            lower_case_viol(i);
            return;

        case Inst::Tag::TaggedCheck:
            // Tagged-union member access guard (LRM 11.9): falls through when
            // the tag selects the accessed member, otherwise ends the run.
            lower_tagged_check(i);
            return;

        case Inst::Tag::Severity:
            // $info / $warning / $error / $fatal report (LRM 20.10) or an
            // immediate-assertion default failure (LRM 16.3): rt_severity
            // renders the message and returns (non-fatal). $fatal's
            // termination is a separate Finish following this node.
            lower_severity(i);
            return;

        case Inst::Tag::Fflush:
            lower_fflush(i);
            return;

        case Inst::Tag::System:
            lower_system(i);
            return;

        case Inst::Tag::FopenFd:
            lower_fopen_fd(i);
            return;

        case Inst::Tag::FopenMcd:
            lower_fopen_mcd(i);
            return;

        case Inst::Tag::Fclose:
            lower_fclose(i);
            return;

        case Inst::Tag::Fgetc:
            lower_file_unary(i, "rt_fgetc");
            return;
        case Inst::Tag::Feof:
            lower_file_unary(i, "rt_feof");
            return;
        case Inst::Tag::Ftell:
            lower_file_unary(i, "rt_ftell");
            return;
        case Inst::Tag::Rewind:
            lower_file_unary(i, "rt_rewind");
            return;
        case Inst::Tag::Ungetc:
            lower_ungetc(i);
            return;
        case Inst::Tag::Fseek:
            lower_fseek(i);
            return;

        case Inst::Tag::Fgets:
            lower_fgets(i);
            return;

        case Inst::Tag::Sformat:
            lower_sformat(i);
            return;

        case Inst::Tag::Sformatf:
            lower_sformatf(i);
            return;

        case Inst::Tag::Ferror:
            lower_ferror(i);
            return;

        case Inst::Tag::Fscanf:
        case Inst::Tag::Sscanf:
            lower_scanf(i);
            return;

        case Inst::Tag::TestPlusArgs:
            lower_test_plusargs(i);
            return;

        case Inst::Tag::ValuePlusArgs:
            lower_value_plusargs(i);
            return;

        case Inst::Tag::FreadWord:
            lower_fread_word(i);
            return;

        case Inst::Tag::FreadMem:
            lower_fread_mem(i);
            return;

        case Inst::Tag::ReadMem:
            lower_readmem(i);
            return;

        case Inst::Tag::DumpFile:
            lower_dump_file(i);
            return;

        case Inst::Tag::DumpVars:
            lower_dump_vars();
            return;

        case Inst::Tag::DumpLimit:
            lower_dump_limit(i);
            return;

        case Inst::Tag::DumpOff:
        case Inst::Tag::DumpOn:
        case Inst::Tag::DumpAll:
        case Inst::Tag::DumpFlush:
            lower_dump_control(ir_->tag(i));
            return;

        case Inst::Tag::SimTime: {
            // The clock value is always known: must == may, z == 0. The wheel
            // counts in the design's smallest time unit; scale the tick count
            // down to the calling module's time unit, rounding to nearest
            // (LRM §20.3.1). `scale` is a power of ten, so scale/2 is exact and
            // (now + scale/2) / scale rounds half up.
            llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel_ });
            if (p.scale > 1) {
                now = b_.CreateUDiv(b_.CreateAdd(now, b_.getInt64(p.scale / 2)),
                                    b_.getInt64(p.scale));
            }
            set_value(i, packed(now, now, zero(64)));
            return;
        }

        case Inst::Tag::EventTriggered:
            set_value(i, lower_event_triggered(event_obj_of(must_of(value_of(p.unary)))));
            return;

        case Inst::Tag::Random: {
            // The PRNG draw is always known (no x/z): must == may, z == 0.
            llvm::Value* draw = b_.CreateCall(random_fn(), {});
            set_value(i, packed(draw, draw, zero(32)));
            return;
        }

        case Inst::Tag::DistUnary: {
            // $dist_* / seeded $random: call the reference routine with the
            // seed word and parameter(s) (must planes, sign-extended to
            // i32); the packed {seed, draw} i64 is a definite 2-state value
            // the frontend slices.
            const auto&  p    = ir_->payload(i).op_extra;
            const auto   info = ir_->extra_data<DistUnaryInfo>(p.extra);
            llvm::Value* seed = cast(must_of(value_of(p.op)), 32, true);
            llvm::Value* p1   = cast(must_of(value_of(info.p1)), 32, true);
            set_value(i, two_val(b_.CreateCall(dist_fn(dist_symbol(info.fn), 1), { seed, p1 })));
            return;
        }
        case Inst::Tag::DistBinary: {
            const auto&  p    = ir_->payload(i).op_extra;
            const auto   info = ir_->extra_data<DistBinaryInfo>(p.extra);
            llvm::Value* seed = cast(must_of(value_of(p.op)), 32, true);
            llvm::Value* p1   = cast(must_of(value_of(info.p1)), 32, true);
            llvm::Value* p2   = cast(must_of(value_of(info.p2)), 32, true);
            set_value(
              i, two_val(b_.CreateCall(dist_fn(dist_symbol(info.fn), 2), { seed, p1, p2 })));
            return;
        }

        case Inst::Tag::Urandom:
            set_value(i, lower_urandom());
            return;

        case Inst::Tag::UrandomSeeded: {
            // $urandom(seed): reseed the process RNG from `seed`, then draw.
            llvm::Value* seed = cast(must_of(value_of(ir_->payload(i).unary)), 64, false);
            b_.CreateCall(urandom_seed_fn(), { rng_slot_, seed });
            set_value(i, lower_urandom());
            return;
        }

        case Inst::Tag::UrandomRange: {
            // $urandom_range(maxval, minval): Lemire multiply-shift of one
            // draw into [lo, hi]; swap when min > max (LRM §18.13.2). The
            // negligible bias of the un-rejected multiply-shift is acceptable
            // (the algorithm is LRM-unspecified).
            llvm::Value* mx = cast(must_of(value_of(ir_->payload(i).binary.l)), 32, false);
            llvm::Value* mn = cast(must_of(value_of(ir_->payload(i).binary.r)), 32, false);
            llvm::Value* ge = b_.CreateICmpUGE(mx, mn);
            llvm::Value* hi = b_.CreateSelect(ge, mx, mn);
            llvm::Value* lo = b_.CreateSelect(ge, mn, mx);
            // range = (hi - lo) + 1, in [1, 2^32], in 64 bits.
            llvm::Value* range =
              b_.CreateAdd(b_.CreateZExt(b_.CreateSub(hi, lo), b_.getInt64Ty()), b_.getInt64(1));
            // draw32 = high 32 bits of the 64-bit result (already zero-extended
            // into the low half by the shift).
            llvm::Value* draw32 = b_.CreateLShr(emit_xoshiro_next(rng_slot_), 32);
            llvm::Value* off =
              b_.CreateTrunc(b_.CreateLShr(b_.CreateMul(draw32, range), 32), b_.getInt32Ty());
            set_value(i, two_val(b_.CreateAdd(lo, off)));
            return;
        }

        case Inst::Tag::ObjRandom: {
            // An object's own draw (LRM §18.14.2): the $urandom high-32
            // extraction, but advancing the 4x64 state the operand lvalue
            // names rather than the body's frame state.
            //
            // There is deliberately no ir_draws_rng / blocks_thunk arm for
            // this tag. That machinery exists to give a coroutine FRAME its
            // own RNG state; this state is an ordinary unpacked-array class
            // property, heap-resident in the object. The single draw of the
            // creating thread that seeds it is a real Urandom the frontend
            // emits beside the ClassNew, so the frame machinery already
            // accounts for every draw that touches a process stream.
            llvm::Value* rng = agg_slot_ptr(lvalue_of(ir_->payload(i).unary));
            set_value(i, lower_urandom(rng));
            return;
        }

        case Inst::Tag::ObjRandSeed: {
            // obj.srandom(seed) (LRM §18.14.2): splitmix64-expand `seed` over
            // the object's own state. The same runtime entry as the
            // per-process reseed -- only the state pointer differs.
            const auto&  p    = ir_->payload(i).binary;
            llvm::Value* rng  = agg_slot_ptr(lvalue_of(p.l));
            llvm::Value* seed = cast(must_of(value_of(p.r)), 64, false);
            b_.CreateCall(urandom_seed_fn(), { rng, seed });
            return;
        }

        case Inst::Tag::RandcNext: {
            // One cyclic draw for a randc property (LRM §18.6). The 256-bit
            // remaining-value bitmap and the object's RNG are both class
            // properties, so both reach the runtime as plain pointers and the
            // bit selection stays out of the JIT.
            const auto&  p    = ir_->payload(i).op_extra;
            const auto   info = ir_->extra_data<RandcInfo>(p.extra);
            llvm::Value* seen = agg_slot_ptr(lvalue_of(p.op));
            llvm::Value* rng  = agg_slot_ptr(lvalue_of(info.rng));
            set_value(i, two_val(b_.CreateCall(randc_next_fn(),
                                               { seen, b_.getInt32(info.nvals), rng })));
            return;
        }

        case Inst::Tag::Arg: {
            // A function / task parameter, located via the per-signature index
            // table (a string formal is one ptr, others two params).
            const uint32_t k    = p.arg.position;
            const uint32_t base = param_llvm_index_[k];
            if (is_string_type(cur_fn_->param_types[k])) {
                set_str(i, fn_->getArg(base));
                return;
            }
            const ValType t = from_ref(p.arg.type);
            Plane         v{ fn_->getArg(base), fn_->getArg(base + 1) };
            if (t.is_real) {
                v = real_plane(planes_to_real(v, t.width));
            }
            set_value(i, v);
            return;
        }
        case Inst::Tag::Ret:
            lower_ret(p.unary);
            return;
        case Inst::Tag::RetVoid:
            lower_ret_void();
            return;
        case Inst::Tag::CallVirtual:
            lower_virtual_call_inst(i);
            return;
        case Inst::Tag::Call:
            lower_call_inst(i);
            return;
        case Inst::Tag::TaskCall:
        case Inst::Tag::TaskCallVirtual:
            // A task call may suspend the caller (task with #delay / @event) and
            // stages its own NBAs into the shared shadow; end the segment so the
            // caller's pre/post-call NBAs never share a block with it.
            ++cur_seg_epoch_;
            lower_task_call_inst(i);
            ++cur_seg_epoch_;
            return;
        case Inst::Tag::Fork:
            // A fork spawns child coroutines and (for `join`) suspends the
            // parent; end the segment so pre/post-fork NBAs never share a block.
            ++cur_seg_epoch_;
            lower_fork_inst(i);
            ++cur_seg_epoch_;
            return;
    }
}

Plane Backend::lower_constant(Ref ref) {
    const Value&                   v     = std::get<Value>(mod_.pool.get(ref));
    const Layout                   lay   = mod_.pool.layout(v.type);
    const uint32_t                 w     = lay.width == 0 ? 1 : lay.width;
    const std::span<const uint8_t> bytes = mod_.pool.bytes(ref);
    const uint32_t                 pb    = plane_bytes(lay.width);

    // A real constant is its IEEE-754 bit pattern in the val plane.
    if (mod_.pool.is_float(v.type)) {
        llvm::Value* pat = const_from_bytes(bytes.subspan(0, pb), w);
        return real_plane(b_.CreateBitCast(pat, fp_ty(w)));
    }

    // The pool stores canonical val/xz; ingest to must/may/z so 0/1/x/z stay
    // distinct (case-equality needs to tell a literal x from a literal z).
    // VPI: x=(val1,xz1), z=(val0,xz1) => must=val&~xz, may=val|xz, z=xz&~val.
    llvm::Value* val = const_from_bytes(bytes.subspan(0, pb), w);
    // A 2-state constant has no x/z: its value is the single iW word.
    if (lay.domain != Domain::Four) {
        return two_val(val);
    }
    llvm::Value* xz = const_from_bytes(bytes.subspan(pb, pb), w);
    return packed(b_.CreateAnd(val, b_.CreateNot(xz)), b_.CreateOr(val, xz), b_.CreateAnd(xz, b_.CreateNot(val)));
}

// A string literal: copy its interned characters into this instruction's
// pre-built temp from a private constant global. Length is passed explicitly
// — never rely on the NUL byte CreateGlobalString appends.
void Backend::lower_string_constant(Index i, Ref ref) {
    const std::span<const uint8_t> bytes = mod_.pool.bytes(ref);
    llvm::Value*                   glob  = b_.CreateGlobalString(
      llvm::StringRef(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "str.lit");
    b_.CreateCall(str_assign_bytes_fn(), { str_of(i), glob, b_.getInt64(bytes.size()),
                                           alloc_site_global(loc_text(i), "string") });
}

// String concatenation: gather the part handles into a stack array and hand
// them to the runtime, which builds into this instruction's temp (alias-safe
// even when the temp aliases a part).
void Backend::lower_string_concat(Index i) {
    std::array<Index, 4> scratch;
    const auto           parts  = ir_->operands(i, scratch);
    const auto           n      = static_cast<uint32_t>(parts.size());
    auto*                arr_ty = llvm::ArrayType::get(llvm::PointerType::getUnqual(ctx_), n);
    llvm::Value*         arr    = b_.CreateAlloca(arr_ty, nullptr, "str.parts");
    for (uint32_t k = 0; k < n; ++k) {
        b_.CreateStore(str_of(parts[k]),
                       b_.CreateInBoundsGEP(arr_ty, arr, { b_.getInt32(0), b_.getInt32(k) }));
    }
    b_.CreateCall(str_concat_fn(), { str_of(i), arr, b_.getInt32(n),
                                     alloc_site_global(loc_text(i), "string") });
}

// A string comparison operator: rt_string_cmp then a native compare against
// zero. Strings carry no X/Z, so the 1-bit result is always definite.
Plane Backend::lower_string_relation(Inst::Tag tag, Index l, Index r) {
    llvm::Value*             c = b_.CreateCall(str_cmp_fn(), { str_of(l), str_of(r) });
    llvm::CmpInst::Predicate pred;
    switch (tag) {
        case Inst::Tag::Eq: pred = llvm::CmpInst::ICMP_EQ; break;
        case Inst::Tag::Ne: pred = llvm::CmpInst::ICMP_NE; break;
        case Inst::Tag::Lt: pred = llvm::CmpInst::ICMP_SLT; break;
        case Inst::Tag::Le: pred = llvm::CmpInst::ICMP_SLE; break;
        case Inst::Tag::Gt: pred = llvm::CmpInst::ICMP_SGT; break;
        default:            pred = llvm::CmpInst::ICMP_SGE; break;
    }
    llvm::Value* bit = b_.CreateICmp(pred, c, b_.getInt32(0));
    return bit_plane(bit, bit);
}

// Assemble an APInt of `width` bits from little-endian canonical plane bytes.
llvm::APInt Backend::apint_from_bytes(std::span<const uint8_t> bytes, uint32_t width) {
    const unsigned        words = (width + 63) / 64;
    std::vector<uint64_t> buf(words, 0);
    std::memcpy(buf.data(), bytes.data(), std::min<size_t>(bytes.size(), static_cast<size_t>(words) * 8));
    return llvm::APInt(width, llvm::ArrayRef<uint64_t>(buf.data(), words));
}

// Build an iW constant from little-endian canonical plane bytes.
llvm::ConstantInt* Backend::const_from_bytes(std::span<const uint8_t> bytes, uint32_t width) {
    return llvm::ConstantInt::get(ctx_, apint_from_bytes(bytes, width));
}

// Whole-storage lvalue for a module signal (state struct).
LValue Backend::signal_lvalue(Signal sig) {
    const SignalSlot& s = slot_of(sig);
    if (s.is_agg) {
        // The aggregate root; a following MemberSelect / ElemSelect chain
        // extends agg_path and (at a scalar/string leaf) fills the leaf
        // metadata. A bare aggregate lvalue is never loaded/stored directly.
        LValue lv{};
        lv.base      = storage_ptr(s);
        lv.root_ty   = s.storage_ty;
        lv.is_agg    = true;
        lv.is_signal = true;
        lv.sig       = sig;
        return lv;
    }
    return LValue{ .base        = storage_ptr(s),
                   .slot_ty     = s.slot_ty,
                   .has_z       = s.has_z,
                   .narrow      = s.narrow,
                   .array_ty    = s.is_array ? llvm::cast<llvm::ArrayType>(s.storage_ty) : nullptr,
                   .store_width = s.width,
                   .bit_off     = b_.getInt32(0),
                   .width       = s.width,
                   .is_signal   = true,
                   .sig         = sig,
                   .is_string   = s.is_string };
}

// Whole-storage lvalue for a function-frame local (per-call frame struct). A ref
// formal has no frame storage: its base is the borrowed actual pointer from the
// fat ref (bound by bind_ref_formals), through which every load / store is live.
//
// Resolving a ref local emits the relocation-epoch check first, so an access
// through a ref whose actual was a container element that has since moved is
// reported rather than performed (docs/design/data-model.md §3).
LValue Backend::local_lvalue(uint32_t index) {
    const FrameSlot& s    = frame_slots_[index];
    llvm::Value*     base = nullptr;
    if (s.is_ref) {
        emit_ref_epoch_check(ref_ptr_[index]);
        base = ref_ptr_[index].slot;
    }
    if (s.is_agg) {
        LValue lv{};
        lv.base      = base != nullptr ? base : b_.CreateStructGEP(frame_ty_, frame_, index);
        lv.root_ty   = s.storage_ty;
        lv.is_agg    = true;
        lv.is_signal = false;
        return lv;
    }
    if (base == nullptr) {
        base = b_.CreateStructGEP(frame_ty_, frame_, index);
    }
    return LValue{ .base        = base,
                   .slot_ty     = s.slot_ty,
                   .has_z       = !s.narrow,
                   .narrow      = s.narrow,
                   .store_width = s.width,
                   .bit_off     = b_.getInt32(0),
                   .width       = s.width,
                   .is_signal   = false,
                   .sig         = Signal{ 0 },
                   .is_string   = s.is_string };
}

Plane Backend::lower_load(const LValue& lv) {
    const uint32_t w  = lv.store_width;

    // A runtime-offset select on storage too wide to shift: the runtime reads the
    // containing word (rt_wide_sel), which also applies LRM 11.5.1's per-bit
    // out-of-range rule, so no storage-width mask is ever built.
    if (lv.wide_dyn) {
        return wide_dyn_load(lv);
    }

    // Narrow 2-state slot: the stored word IS the iW value.
    if (lv.narrow) {
        llvm::Value* v = b_.CreateLoad(int_ty(w), lv_mm_ptr(lv));
        if (lv.width == w && is_zero_const(lv.bit_off)) {
            return two_val(v);
        }
        // A bounds-checked select aligns poison-free and reads 0 outside range
        // (LRM 11.5.1: an out-of-range read of a 2-state value is 0).
        if (lv.range_mask != nullptr) {
            llvm::Value* shifted = align_shl(v, b_.CreateNeg(lv.bit_off), w);
            llvm::Value* valid   = align_shl(lv.range_mask, b_.CreateNeg(lv.bit_off), w);
            return two_val(cast(b_.CreateAnd(shifted, valid), lv.width, false));
        }
        llvm::Value* off = cast(lv.bit_off, w, false);
        return two_val(cast(b_.CreateLShr(v, off), lv.width, false));
    }

    llvm::Value*   mm = b_.CreateLoad(int_ty(2 * w), lv_mm_ptr(lv));
    // A never-z slot stores no z word; its z plane is a constant zero.
    llvm::Value* z = lv.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(w), lv_z_ptr(lv))) : zero(w);
    if (lv.width == w && is_zero_const(lv.bit_off)) {
        return { mm, z };
    }
    const Plane full = { mm, z };
    // A bounds-checked select aligns poison-free and forces x on the bits that
    // fall outside the vector (LRM 11.5.1: must=0, may=1, z=0).
    if (lv.range_mask != nullptr) {
        llvm::Value* down  = b_.CreateNeg(lv.bit_off);
        llvm::Value* must  = align_shl(must_of(full), down, w);
        llvm::Value* may   = align_shl(may_of(full), down, w);
        llvm::Value* zp    = align_shl(z, down, w);
        llvm::Value* valid = align_shl(lv.range_mask, down, w);
        llvm::Value* oob   = b_.CreateNot(valid);
        return packed(cast(b_.CreateAnd(must, valid), lv.width, false),
                      cast(b_.CreateOr(may, oob), lv.width, false),
                      cast(b_.CreateAnd(zp, valid), lv.width, false));
    }
    // Bit/part select: unpack, shift each plane down by the offset, re-pack.
    llvm::Value* off = cast(lv.bit_off, w, false);
    return packed(cast(b_.CreateLShr(must_of(full), off), lv.width, false),
                  cast(b_.CreateLShr(may_of(full), off), lv.width, false),
                  cast(b_.CreateLShr(z, off), lv.width, false));
}

// The shared leading arguments of the two wide-select helpers, for the planes of
// one `width`-bit slot: where they live, the container the selection clamps to,
// the signed storage offset, the selected width, and the plane layout facts.
// `zp` null means the slot has no z plane; `four` false means it is 2-state (`mmp`
// is `width` value bits, not the packed must|may pair).
//
// Taking the planes as parameters is what lets the NBA path stage into the shadow
// slot -- and its dirty mask, a 2-state plane of the same width -- with the same
// two calls the live store uses.
std::vector<llvm::Value*> Backend::wide_dyn_args(const LValue& lv,
                                                 llvm::Value*  mmp,
                                                 llvm::Value*  zp,
                                                 uint32_t      width,
                                                 bool          four) {
    auto* null = llvm::Constant::getNullValue(raw_ptr_ty());
    return { mmp,
             zp != nullptr ? zp : null,
             b_.getInt32(width),
             lv.sel_base,
             b_.getInt32(lv.sel_cwidth),
             b_.CreateSExt(lv.bit_off, b_.getInt64Ty()),
             b_.getInt32(lv.width),
             b_.getInt32(four ? 1 : 0),
             b_.getInt32(zp != nullptr ? 1 : 0),
             b_.CreateZExt(lv.sel_unknown, b_.getInt32Ty()) };
}

// The live storage planes of a select lvalue.
std::vector<llvm::Value*> Backend::wide_dyn_args(const LValue& lv) {
    return wide_dyn_args(lv, lv_mm_ptr(lv), lv.has_z ? lv_z_ptr(lv) : nullptr, lv.store_width,
                         !lv.narrow);
}

// A selection plane's spill width: `n` rounded up to whole 64-bit words, which is
// both what the helpers index and the in-memory layout of an i(n), so one store /
// load moves a whole plane. Rounding up (rather than using i(n) itself) keeps
// every byte of the buffer defined.
uint32_t Backend::sel_buf_bits(uint32_t n) {
    return ((n + 63) / 64) * 64;
}

llvm::AllocaInst* Backend::sel_plane_buf(uint32_t n, const char* name) {
    return entry_alloca(int_ty(sel_buf_bits(n)), name);
}

// Read a runtime-offset selection out of very wide storage.
Plane Backend::wide_dyn_load(const LValue& lv) {
    const uint32_t n  = lv.width;
    auto*          bt = int_ty(sel_buf_bits(n));
    auto*          om = sel_plane_buf(n, "wsel.must");
    auto*          oy = sel_plane_buf(n, "wsel.may");
    auto*          oz = sel_plane_buf(n, "wsel.z");

    std::vector<llvm::Value*> args = wide_dyn_args(lv);
    args.push_back(om);
    args.push_back(oy);
    args.push_back(oz);
    b_.CreateCall(wide_sel_fn(), args);

    llvm::Value* must = cast(b_.CreateLoad(bt, om), n, false);
    if (lv.narrow) {
        return two_val(must);
    }
    return packed(must, cast(b_.CreateLoad(bt, oy), n, false),
                  cast(b_.CreateLoad(bt, oz), n, false));
}

// Write a runtime-offset selection into one wide slot's planes: the runtime merges
// the bits into the containing words and drops the ones outside the container, so
// there is no read-modify-write of the whole vector here either. `four` / `zp`
// describe the target planes (the live slot, or an NBA shadow of the same shape).
void Backend::wide_dyn_store_planes(const LValue& lv,
                                    llvm::Value*  mmp,
                                    llvm::Value*  zp,
                                    uint32_t      width,
                                    bool          four,
                                    Plane         rhs) {
    const uint32_t n  = lv.width;
    auto*          bt = int_ty(sel_buf_bits(n));
    // Spill each plane zero-extended to the buffer's word count.
    const auto spill = [&](llvm::Value* v, const char* name) -> llvm::Value* {
        auto* buf = sel_plane_buf(n, name);
        b_.CreateStore(cast(v, sel_buf_bits(n), false), buf);
        return buf;
    };

    std::vector<llvm::Value*> args = wide_dyn_args(lv, mmp, zp, width, four);
    if (!four) {
        auto* zbuf = sel_plane_buf(n, "wins.zero");
        b_.CreateStore(llvm::ConstantInt::get(bt, 0), zbuf);
        args.push_back(spill(must_of(rhs), "wins.must"));
        args.push_back(zbuf);
        args.push_back(zbuf);
    } else {
        const Plane f = to_four(rhs);
        args.push_back(spill(must_of(f), "wins.must"));
        args.push_back(spill(may_of(f), "wins.may"));
        args.push_back(spill(f.z != nullptr ? f.z : zero(n), "wins.z"));
    }
    b_.CreateCall(wide_ins_fn(), args);
}

// The live-storage form: what a blocking store to a wide dynamic select does.
void Backend::wide_dyn_store(const LValue& lv, Plane rhs) {
    wide_dyn_store_planes(lv, lv_mm_ptr(lv), lv.has_z ? lv_z_ptr(lv) : nullptr, lv.store_width,
                          !lv.narrow, rhs);
}

// Bounds-checked load of a dynamic-array element (LRM 7.5.1): in range, load
// from the heap buffer; out of range, yield the element default (X for
// 4-state, 0 for 2-state). The heap is only dereferenced on the in-range
// path, so an out-of-range or negative index never touches memory. Returns
// the same Plane shape lower_load would (narrow -> two_val; else { i2W, iW }),
// so the caller's real / domain handling is unchanged.
Plane Backend::dyn_elem_load(const LValue& lv) {
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* inbb  = llvm::BasicBlock::Create(ctx_, "dyn.load.in", fn);
    auto* defbb = llvm::BasicBlock::Create(ctx_, "dyn.load.def", fn);
    auto* cont  = llvm::BasicBlock::Create(ctx_, "dyn.load.cont", fn);
    b_.CreateCondBr(lv.dyn_inrange, inbb, defbb);

    b_.SetInsertPoint(inbb);
    LValue clean       = lv;
    clean.dyn_inrange  = nullptr;
    clean.bounds_checks.clear();
    const Plane vin    = lower_load(clean);
    auto*       inend  = b_.GetInsertBlock();
    b_.CreateBr(cont);

    b_.SetInsertPoint(defbb);
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    if (lv.narrow) {
        auto* phi = b_.CreatePHI(int_ty(lv.width), 2);
        phi->addIncoming(vin.mm, inend);
        phi->addIncoming(zero(lv.width), defbb);
        return two_val(phi);
    }
    auto* mmphi = b_.CreatePHI(int_ty(2 * lv.width), 2);
    mmphi->addIncoming(vin.mm, inend);
    mmphi->addIncoming(x_mm(lv.width), defbb);
    auto* zphi = b_.CreatePHI(int_ty(lv.width), 2);
    zphi->addIncoming(z_of(vin), inend);
    zphi->addIncoming(zero(lv.width), defbb);
    return { mmphi, zphi };
}

// Bounds-checked store of a dynamic-array element (LRM 7.5.1): the store and
// its wait-queue servicing run only when the index is in [0, len); an
// out-of-range write is a no-op.
void Backend::dyn_elem_store(const LValue& lv, Plane rhs) {
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* dobb = llvm::BasicBlock::Create(ctx_, "dyn.store.do", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "dyn.store.cont", fn);
    b_.CreateCondBr(lv.dyn_inrange, dobb, cont);

    b_.SetInsertPoint(dobb);
    LValue clean              = lv;
    clean.dyn_inrange         = nullptr;
    clean.bounds_checks.clear();
    const ServiceSnapshot old = service_snapshot(clean);
    lower_store(clean, rhs);
    service_after_write(clean, old);
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
}

void Backend::lower_cond(Index i) {
    const Inst::Payload& p    = ir_->payload(i);
    const auto           regs = ir_->extra_data<Regions>(p.op_extra.extra);
    const Plane          cond = value_of(p.op_extra.op);

    // must=1 holds only for a definite 1 (invariant must => may), so X/Z fall
    // through to else automatically.
    llvm::Value* take = b_.CreateICmpNE(must_of(cond), zero(1));

    auto* then_bb = llvm::BasicBlock::Create(ctx_, "then", fn_);
    auto* else_bb = llvm::BasicBlock::Create(ctx_, "else", fn_);
    auto* cont_bb = llvm::BasicBlock::Create(ctx_, "endif", fn_);
    // A cold branch (violation check) biases weights toward the else path so
    // codegen lays the then-block out of line.
    llvm::MDNode* weights =
      regs.unlikely ? llvm::MDBuilder(ctx_).createBranchWeights(1, 2000) : nullptr;
    b_.CreateCondBr(take, then_bb, else_bb, weights);

    b_.SetInsertPoint(then_bb);
    lower_region(ir_->region(regs.then_));
    branch_to(cont_bb);

    b_.SetInsertPoint(else_bb);
    lower_region(ir_->region(regs.else_));
    branch_to(cont_bb);

    b_.SetInsertPoint(cont_bb);
}

// Multiway `case` -> llvm switch. Packs the selector planes into an integer
// match key: for a 2-state selector the key is the bare value; for 4-state
// it is {may^must, must} with `must` on the low W bits. A clean value keys
// on its low W bits alone (its unknown half is zero), so the clean cases sit
// densely in [0, 2^W) and LLVM can build a jump table; any x/z selector sets
// a bit in the high half, landing outside every case value and falling to
// the default. This exactly reproduces `===`: the frontend supplies each
// clean label's value as its case key and guarantees the keys are unique.
void Backend::lower_switch(Index i) {
    const Inst::Payload& p    = ir_->payload(i);
    const auto           info = ir_->extra_data<SwitchInfo>(p.op_extra.extra);
    const Plane          sel  = value_of(p.op_extra.op);

    const uint32_t w    = width_of(sel);
    llvm::Value*   must = must_of(sel);
    llvm::Value*   key  = must;
    uint32_t       keyw = w;
    if (!sel.two) {
        keyw            = 2 * w;
        llvm::Type* kty = int_ty(keyw);
        llvm::Value* u  = b_.CreateXor(must, may_of(sel));
        key = b_.CreateOr(b_.CreateZExt(must, kty), b_.CreateShl(b_.CreateZExt(u, kty), w));
    }
    llvm::Type* kty = int_ty(keyw);

    const std::span<const Index> arm_words = ir_->region(info.arms);
    constexpr uint32_t           arm_len   = sizeof(SwitchArm) / sizeof(uint32_t);
    const uint32_t               n_arms    = static_cast<uint32_t>(arm_words.size()) / arm_len;

    auto* default_bb = llvm::BasicBlock::Create(ctx_, "case.default", fn_);
    auto* cont_bb    = llvm::BasicBlock::Create(ctx_, "case.end", fn_);

    llvm::SwitchInst* sw = b_.CreateSwitch(key, default_bb, n_arms);
    for (uint32_t a = 0; a < n_arms; ++a) {
        const auto arm = ir_->extra_data<SwitchArm>(
          static_cast<ExtraIndex>(static_cast<uint32_t>(info.arms.start) + a * arm_len));
        auto*      arm_bb = llvm::BasicBlock::Create(ctx_, "case.arm", fn_);

        const std::span<const Index> kw = ir_->region(arm.keys);
        for (size_t k = 0; k + 1 < kw.size(); k += 2) {
            const uint64_t val = static_cast<uint64_t>(kw[k]) | (static_cast<uint64_t>(kw[k + 1]) << 32);
            sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(kty), val), arm_bb);
        }

        b_.SetInsertPoint(arm_bb);
        lower_region(ir_->region(arm.body));
        branch_to(cont_bb);
    }

    b_.SetInsertPoint(default_bb);
    lower_region(ir_->region(info.default_));
    branch_to(cont_bb);

    b_.SetInsertPoint(cont_bb);
}

// Value-producing conditional with lazily evaluated branches: short-circuit
// &&/||/-> (lower_short_circuit) and a `?:` whose arm must not be
// speculated (lower_lazy_conditional). Branches on the predicate's must bit
// (a definite 2-state value), lowers only the taken branch's region, and
// merges the two branch values with a PHI. A side-effecting operand lives
// inside its own region, so it is never evaluated when the predicate rules
// it out. LLVM folds the whole thing back to a straight-line select when
// both branches are side-effect free.
void Backend::lower_select_lazy(Index i) {
    const Inst::Payload& p    = ir_->payload(i);
    const auto           sel  = ir_->extra_data<SelectLazy>(p.op_extra.extra);
    const Plane          pred = value_of(p.op_extra.op);

    llvm::Value* take = b_.CreateICmpNE(must_of(pred), zero(1));

    auto* then_bb = llvm::BasicBlock::Create(ctx_, "sel.then", fn_);
    auto* else_bb = llvm::BasicBlock::Create(ctx_, "sel.else", fn_);
    auto* cont_bb = llvm::BasicBlock::Create(ctx_, "sel.end", fn_);
    b_.CreateCondBr(take, then_bb, else_bb);

    // A string arm carries a borrowed rt_string handle pointer rather than a
    // Plane, so its merge is one pointer PHI. That is sound because
    // build_str_temps walks the flat instruction array and allocas + inits
    // every string temp in the prologue: the not-taken arm's handle exists and
    // holds "" even though its producer never ran, and the PHI only forwards
    // the pointer from the block that did execute.
    const bool is_str = type_of(i).is_string;
    // A real arm carries no integer plane at all — it rides Plane.fp — so
    // its merge is one fp PHI, the lazy analogue of the eager Select's
    // lower_real_mux dispatch. Reached from lower_lazy_conditional whenever
    // LRM 11.4.11 makes a `?:` real (one arm real, the other cast to real
    // by slang) and an arm is not speculation-safe, which is what
    // `(t == 0) ? $realtime : t` is. Without it the arm's fp-only Plane
    // reached must_of and tripped its real-value depth guard. The
    // short-circuit operators never arrive here with a real:
    // lower_short_circuit reduces a real operand to a truth bit (FNe
    // against 0.0) before the merge.
    const bool is_real = type_of(i).is_real;
    // The two arms may disagree in Plane domain (a 2-state constant against a
    // 4-state Land/Lor result), so each is normalized to the instruction's own
    // domain inside its block — the PHIs need identical value types.
    const bool four = type_of(i).four_state;
    const auto norm = [&](Index v) { return four ? to_four(value_of(v)) : Plane{ must_of(value_of(v)), nullptr, true }; };

    b_.SetInsertPoint(then_bb);
    lower_region(ir_->region(sel.then_));
    const Plane       tv       = is_str || is_real ? Plane{} : norm(sel.then_val);
    llvm::Value*      ts       = is_str ? str_of(sel.then_val) : nullptr;
    llvm::Value*      tf       = is_real ? fp_of(value_of(sel.then_val)) : nullptr;
    llvm::BasicBlock* then_end = b_.GetInsertBlock();
    branch_to(cont_bb);

    b_.SetInsertPoint(else_bb);
    lower_region(ir_->region(sel.else_));
    const Plane       ev       = is_str || is_real ? Plane{} : norm(sel.else_val);
    llvm::Value*      es       = is_str ? str_of(sel.else_val) : nullptr;
    llvm::Value*      ef       = is_real ? fp_of(value_of(sel.else_val)) : nullptr;
    llvm::BasicBlock* else_end = b_.GetInsertBlock();
    branch_to(cont_bb);

    b_.SetInsertPoint(cont_bb);
    if (is_str) {
        llvm::PHINode* h = b_.CreatePHI(ts->getType(), 2);
        h->addIncoming(ts, then_end);
        h->addIncoming(es, else_end);
        set_str(i, h);
        return;
    }
    if (is_real) {
        llvm::PHINode* f = b_.CreatePHI(tf->getType(), 2);
        f->addIncoming(tf, then_end);
        f->addIncoming(ef, else_end);
        set_value(i, real_plane(f));
        return;
    }
    llvm::PHINode* mm = b_.CreatePHI(tv.mm->getType(), 2);
    mm->addIncoming(tv.mm, then_end);
    mm->addIncoming(ev.mm, else_end);
    if (!four) {
        set_value(i, two_val(mm));
        return;
    }
    llvm::PHINode* z = b_.CreatePHI(tv.z->getType(), 2);
    z->addIncoming(tv.z, then_end);
    z->addIncoming(ev.z, else_end);
    set_value(i, { mm, z });
}

void Backend::lower_loop(Index i) {
    const Inst::Payload& p  = ir_->payload(i);
    const auto           lp = ir_->extra_data<LoopInfo>(p.extra);

    auto* head_bb = llvm::BasicBlock::Create(ctx_, "loop.head", fn_);
    auto* body_bb = llvm::BasicBlock::Create(ctx_, "loop.body", fn_);
    auto* step_bb = llvm::BasicBlock::Create(ctx_, "loop.step", fn_);
    auto* exit_bb = llvm::BasicBlock::Create(ctx_, "loop.exit", fn_);

    // do-while runs the body before the first test; every other loop tests
    // first. `continue` -> step, `break` -> exit, innermost loop only.
    b_.CreateBr(lp.bottom_test ? body_bb : head_bb);

    // Header: recompute the predicate, then test it. must=1 only for a
    // definite 1, so an X/Z condition exits the loop.
    b_.SetInsertPoint(head_bb);
    lower_region(ir_->region(lp.cond));
    llvm::Value* take = b_.CreateICmpNE(must_of(value_of(lp.pred)), zero(1));
    b_.CreateCondBr(take, body_bb, exit_bb);

    b_.SetInsertPoint(body_bb);
    loop_targets_.push_back({ step_bb, exit_bb });
    lower_region(ir_->region(lp.body));
    loop_targets_.pop_back();
    branch_to(step_bb); // a `return`/`break`/`continue` in the body terminates it; don't double-branch

    // Step, then the back-edge to re-test the condition.
    b_.SetInsertPoint(step_bb);
    lower_region(ir_->region(lp.step));
    b_.CreateBr(head_bb);

    b_.SetInsertPoint(exit_bb);
}

void Backend::lower_named_block(Index i) {
    const auto bi = ir_->extra_data<BlockInfo>(ir_->payload(i).extra);

    auto* end_bb = llvm::BasicBlock::Create(ctx_, "block.end", fn_);

    // A remotely-named block puts THIS activation on the target's list while it
    // is inside the region, so a `disable` anywhere can find and wake it. The
    // list, rather than one slot per target, is what lets the block sit in a body
    // that has several activations at once -- a task, a fork child -- where "the
    // occupant" is not a single thing. A process body simply never has more than
    // one node on it.
    //
    // A thunk-elided group links nothing: it has no handle to record and, having
    // no suspend, cannot be inside the region while any other process runs -- so
    // the list stays empty and a `disable` naming its block is the no-op LRM
    // 9.6.2 already prescribes for a block that is not currently executing.
    const uint32_t remote = bi.remote != 0 && hdl_ != nullptr ? bi.label : kNoDisableTarget;
    llvm::Value*   node   = nullptr;
    if (remote != kNoDisableTarget) {
        node = block_node(bi.label);
        // awaiter() is forced here rather than at the first #delay: the node
        // publishes the park slot, and a delay inside the body would create it
        // too late for that.
        emit_disable_link(remote, node);
    }

    block_targets_.push_back({ bi.label, remote, node, end_bb });
    lower_region(ir_->region(bi.body));
    block_targets_.pop_back();
    branch_to(end_bb); // a `return` / `break` / `disable` already terminated it

    b_.SetInsertPoint(end_bb);
    // Leaving the region, normally or by a disable: this activation is no longer
    // inside it, and its mark must not survive into a later entry.
    if (remote != kNoDisableTarget) {
        emit_disable_unlink(remote, node);
        b_.CreateStore(b_.getInt32(0), b_.CreateStructGEP(disable_node_ty(), node, 5));
    }
}

// The frame-resident list node for one targeted block in the body being lowered.
// One per block per activation: a block cannot be inside itself, so a single
// activation is never on the same block's list twice, and re-entering it in a
// loop links and unlinks the same node in turn.
llvm::Value* Backend::block_node(uint32_t label) {
    auto [it, fresh] = block_nodes_.try_emplace(label, nullptr);
    if (fresh) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        it->second = ab.CreateAlloca(disable_node_ty(), nullptr, "block_node");
    }
    return it->second;
}

// The end block of the enclosing named block with this label. Innermost
// first, because a labelled block cannot enclose itself, so the first match
// walking down is the one the `disable` names.
llvm::BasicBlock* Backend::block_end(uint32_t label) const {
    for (size_t n = block_targets_.size(); n > 0; --n) {
        if (block_targets_[n - 1].label == label) {
            return block_targets_[n - 1].end;
        }
    }

    throw BackendError{ "internal: disable of a block that does not enclose it" };
}

} // namespace clotho::ir::backend
