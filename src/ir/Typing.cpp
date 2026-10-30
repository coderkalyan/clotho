#include "ir/Typing.h"

#include <variant>

#include "ir/TypeModel.h"

namespace clotho::ir {

ValueType value_type_of(const InternPool& pool, Ref type) {
    const Type& t = std::get<Type>(pool.get(type));
    if (t.kind == Type::Kind::String) {
        return { 0, false, false, true };
    }
    const Layout lay = pool.layout(type);
    const bool   fl  = pool.is_float(type);
    // Reals are signed quantities (LRM 11.8.1).
    return { lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four, t.is_signed || fl, false, fl };
}

namespace {

// A signal's value type as the backend's slot layout reports it: the leaf
// element's width and domain, the declared type's own signedness, and string /
// real only for a scalar (an array of them is an aggregate, never a value).
ValueType signal_type(const Module& m, Signal sig) {
    const Ref   type  = m.signal(sig).type;
    const Type& t     = std::get<Type>(m.pool.get(type));
    const bool  array = t.kind == Type::Kind::UnpackedArray;
    const Ref   leaf  = types::leaf_type(m.pool, type);
    if (types::is_string(m.pool, leaf)) {
        return { 1, false, t.is_signed, !array, false };
    }
    const Layout lay = m.pool.layout(type);
    return {
        lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four, t.is_signed, false, m.pool.is_float(leaf) && !array
    };
}

} // namespace

ValueType compute_type(const Module& m, const IrView& ir, Index i, std::span<const ValueType> done) {
    const InternPool& pool = m.pool;
    const auto        ty   = [&](Index x) {
        const auto k = static_cast<uint32_t>(x);
        return k < done.size() ? done[k] : ValueType{};
    };
    const Inst::Payload& p = ir.payload(i);
    switch (ir.tag(i)) {

        case Inst::Tag::Constant:
            return value_type_of(pool, std::get<Value>(pool.get(p.ref)).type);
        case Inst::Tag::Arg:
            return value_type_of(pool, p.arg.type);
        case Inst::Tag::Local:
            return value_type_of(pool, p.local.type);
        case Inst::Tag::Call:
            return value_type_of(pool, m.function(ir.extra_data<CallInfo>(p.extra).func).return_type);
        case Inst::Tag::CallVirtual:
            // All overrides share the statically-resolved method's return type.
            return value_type_of(pool, m.function(ir.extra_data<VirtualCallInfo>(p.extra).func).return_type);
        case Inst::Tag::Resize:
            return value_type_of(pool, p.op_type.type);
        case Inst::Tag::Signal:
            return signal_type(m, p.signal);
        case Inst::Tag::BitSelect: {
            const ValueType base = ty(p.binary.l);
            return { 1, base.four_state, false };
        }
        case Inst::Tag::PartSelect: {
            const auto ps = ir.extra_data<PartSelect>(p.op_extra.extra);
            return { ps.msb - ps.lsb + 1, ty(p.op_extra.op).four_state, false };
        }
        case Inst::Tag::DynPartSelect: {
            const auto dps = ir.extra_data<DynPartSelect>(p.op_extra.extra);
            return { dps.width, ty(p.op_extra.op).four_state, false };
        }
        case Inst::Tag::ElemSelect: {
            // The result is one element of the indexed array.
            const auto  es = ir.extra_data<ElemSelect>(p.op_extra.extra);
            const Type& at = std::get<Type>(pool.get(es.array_type));
            return value_type_of(pool, at.element);
        }
        case Inst::Tag::ArrSlice: {
            // An array lvalue, never a scalar value; report the element leaf
            // type for exhaustiveness (the slice is only ever consumed as an
            // aggregate, so this is not used on a value path).
            const auto  as = ir.extra_data<ArrSlice>(p.op_extra.extra);
            const Type& st = std::get<Type>(pool.get(as.slice_type));
            return value_type_of(pool, st.element);
        }
        case Inst::Tag::MemberSelect: {
            // The result is the selected member's own type.
            const auto  ms     = ir.extra_data<MemberSelect>(p.op_extra.extra);
            const Type& st     = std::get<Type>(pool.get(ms.struct_type));
            const auto  fields = pool.struct_fields(st.fields_id);
            return value_type_of(pool, fields[ms.field_index].type);
        }
        case Inst::Tag::ClassNew:
            // The object handle (a 2-state 64-bit value, per layout(Class)).
            return value_type_of(pool, p.ref);
        case Inst::Tag::IfaceRef:
            // The instance-context address, as a 2-state 64-bit handle.
            return { 64, false, false };
        case Inst::Tag::IfaceSignal: {
            // An lvalue producer (the named member of the handle's instance);
            // its shape is the representative signal's, which every instance of
            // the definition shares.
            return signal_type(m, ir.extra_data<IfaceMember>(p.op_extra.extra).signal);
        }
        case Inst::Tag::Load:
            return ty(p.unary);
        case Inst::Tag::SimTime:
            return { 64, false, false };
        case Inst::Tag::EventNew:
            // An event handle: a 2-state 64-bit word.
            return { 64, false, false };
        case Inst::Tag::ProcQuery:
            // `self` is a class handle, `status` the 32-bit state enum,
            // `get_randstate` a string -- the frontend recorded which on the op.
            return value_type_of(pool, ir.extra_data<ProcCtlInfo>(p.extra).type);
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
            return value_type_of(pool, ir.extra_data<CoverInfo>(p.extra).type);
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
            return value_type_of(pool, std::get<Type>(pool.get(p.op_type.type)).element);
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
            return value_type_of(
              pool, std::get<Type>(pool.get(ir.extra_data<AssocArg>(p.op_extra.extra).assoc_type)).element);
        case Inst::Tag::AssocDefault:
            // The `'{default: v}` slot carries the same value element type.
            return value_type_of(pool, std::get<Type>(pool.get(p.op_type.type)).element);
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
            return ty(p.binary.l);
        case Inst::Tag::WireRes:
        case Inst::Tag::WandRes:
        case Inst::Tag::WorRes: {
            // Net resolution always yields a 4-state result at the operand
            // (net) width — a driver conflict is X, an all-high-Z net is Z —
            // even when the driver slots feeding it are 2-state.
            const ValueType lt = ty(p.binary.l);
            return { lt.width, true, false };
        }
        case Inst::Tag::StrengthRes: {
            // Strength resolution yields a 4-state net-width result, like
            // WireRes; the width is the first driver slot's width.
            const auto      info = ir.extra_data<StrengthResInfo>(p.extra);
            const ValueType lt   = ty(ir.region(info.values)[0]);
            return { lt.width, true, false };
        }
        case Inst::Tag::Bnot:
            return ty(p.unary);
        case Inst::Tag::StreamReverse:
            // A bit permutation: the operand's width and domain.
            return ty(p.op_imm.op);
        case Inst::Tag::NetPull:
            // A pulled net is 4-state (a driver conflict is still X) but
            // never Z, at the resolved value's width.
            return { ty(p.op_imm.op).width, true, false };
        case Inst::Tag::Select:
            return ty(ir.extra_data<Select>(p.op_extra.extra).a);
        case Inst::Tag::SelectLazy:
            return ty(ir.extra_data<SelectLazy>(p.op_extra.extra).then_val);

        case Inst::Tag::FAdd:
        case Inst::Tag::FSub:
        case Inst::Tag::FMul:
        case Inst::Tag::FDiv:
        case Inst::Tag::FPow:
            return ty(p.binary.l);
        case Inst::Tag::FNeg:
            return ty(p.unary);
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
            return value_type_of(pool, p.op_type.type);
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
            uint32_t width = 0;
            bool     four  = false;
            for (Index part : ir.region(ir.extra_data<ExtraSlice>(p.extra))) {
                const ValueType t = ty(part);
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
            const ValueType v = ty(p.repl.value);
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
            return value_type_of(pool, p.op_type.type);
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

std::vector<ValueType> value_types(const Module& m, const Ir& ir) {
    std::vector<ValueType> done;
    done.reserve(ir.inst_count());
    const IrView view = ir.view();
    for (size_t k = 0; k < ir.inst_count(); ++k) {
        done.push_back(compute_type(m, view, static_cast<Index>(k), done));
    }
    return done;
}

} // namespace clotho::ir
