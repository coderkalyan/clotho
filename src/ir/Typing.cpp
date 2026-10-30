#include "ir/Typing.h"

#include <variant>

#include "ir/TypeModel.h"

namespace clotho::ir {

ValueType value_type_of(const InternPool& pool, Ref type) {
    const Type& t = std::get<Type>(pool.get(type));
    if (t.kind == Type::Kind::String) {
        return { 0, false, true };
    }
    const Layout lay = pool.layout(type);
    const bool   fl  = pool.is_float(type);
    return { lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four, false, fl };
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
        return { 1, false, !array, false };
    }
    const Layout lay = m.pool.layout(type);
    return { lay.width == 0 ? 1 : lay.width, lay.domain == Domain::Four, false, m.pool.is_float(leaf) && !array };
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
        case Inst::Tag::ZExt:
        case Inst::Tag::SExt:
        case Inst::Tag::Trunc:
        case Inst::Tag::ToFour:
        case Inst::Tag::ToTwo:
            return value_type_of(pool, p.op_type.type);
        case Inst::Tag::Signal:
            return signal_type(m, p.signal);
        case Inst::Tag::BitSelect: {
            const ValueType base = ty(p.binary.l);
            return { 1, base.four_state };
        }
        case Inst::Tag::PartSelect: {
            const auto ps = ir.extra_data<PartSelect>(p.op_extra.extra);
            return { ps.msb - ps.lsb + 1, ty(p.op_extra.op).four_state };
        }
        case Inst::Tag::DynPartSelect: {
            const auto dps = ir.extra_data<DynPartSelect>(p.op_extra.extra);
            return { dps.width, ty(p.op_extra.op).four_state };
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
            return { 64, false };
        case Inst::Tag::IfaceSignal: {
            // An lvalue producer (the named member of the handle's instance);
            // its shape is the representative signal's, which every instance of
            // the definition shares.
            return signal_type(m, ir.extra_data<IfaceMember>(p.op_extra.extra).signal);
        }
        case Inst::Tag::Load:
            return ty(p.unary);
        case Inst::Tag::SimTime:
            return { 64, false };
        case Inst::Tag::EventNew:
            // An event handle: a 2-state 64-bit word.
            return { 64, false };
        case Inst::Tag::ProcQuery:
            // `self` is a class handle, `status` the 32-bit state enum,
            // `get_randstate` a string -- the frontend recorded which on the op.
            return value_type_of(pool, ir.extra_data<ProcCtlInfo>(p.extra).type);
        case Inst::Tag::ProcCtl:
        case Inst::Tag::ProcWait:
            // Statements, never values.
            return { 1, false };
        case Inst::Tag::SyncWait:
            // 1 when the park resumed as its queue's head: a definite 2-state
            // bit the frontend ANDs with its own resource test.
            return { 1, false };
        case Inst::Tag::SyncCtl:
            // A statement, never a value.
            return { 1, false };
        case Inst::Tag::CoverQuery:
            // A covergroup handle, a coverage percentage, or a bin count -- the
            // frontend recorded which on the op.
            return value_type_of(pool, ir.extra_data<CoverInfo>(p.extra).type);
        case Inst::Tag::CoverCtl:
            // A statement, never a value.
            return { 1, false };
        case Inst::Tag::EventTriggered:
            // `e.triggered` (LRM 15.5.3): a definite 2-state 1-bit `bit`.
            return { 1, false };
        case Inst::Tag::Random:
            return { 32, false };
        case Inst::Tag::Clog2:
            // $clog2 returns a 32-bit signed `integer` (4-state: the result
            // is X when the operand carries x/z).
            return { 32, true };
        case Inst::Tag::Ctz:
        case Inst::Tag::Clz:
            // A definite 2-state 32-bit bit index (counts of the must plane).
            return { 32, false };
        case Inst::Tag::CountBits:
            // $countbits family: a definite 2-state 32-bit signed `int`
            // (LRM 20.9 return type). Never X — it counts, it does not
            // propagate.
            return { 32, false };
        case Inst::Tag::OneHot:
        case Inst::Tag::OneHot0:
        case Inst::Tag::IsUnknown:
            // Definite 1-bit qualifier results (LRM 20.9); never X.
            return { 1, false };
        case Inst::Tag::FopenFd:
        case Inst::Tag::FopenMcd:
            // $fopen returns a 32-bit descriptor integer.
            return { 32, false };
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
            return { 32, false };
        case Inst::Tag::System:
            // $system returns the raw system() int (a POSIX wait-status).
            return { 32, false };
        case Inst::Tag::TestPlusArgs:
        case Inst::Tag::ValuePlusArgs:
            // LRM §21.6: an always-known (never x/z) boolean 'int'.
            return { 32, false };
        case Inst::Tag::DynArrayNew:
        case Inst::Tag::DynArrayNewInit:
        case Inst::Tag::DynArraySize:
        case Inst::Tag::DynArrayDelete:
        case Inst::Tag::DynArrayCopy:
        case Inst::Tag::DynArrayCopyQueue:
            // Only size() is a value (a definite 2-state int); new / delete /
            // copy are statements whose type is never queried.
            return { 32, false };
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
            return { 32, false };
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
            return { 32, false };
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
            return { 32, false };

        case Inst::Tag::Add:
        case Inst::Tag::Sub:
        case Inst::Tag::Mul:
        case Inst::Tag::UDiv:
        case Inst::Tag::SDiv:
        case Inst::Tag::UMod:
        case Inst::Tag::SMod:
        case Inst::Tag::Band:
        case Inst::Tag::Bor:
        case Inst::Tag::Bxor:
        case Inst::Tag::Bxnor:
        case Inst::Tag::Sll:
        case Inst::Tag::Srl:
        case Inst::Tag::Sra:
            return ty(p.binary.l);
        // Power's result shares the base's type; the exponent is self-determined
        // and does not widen the result.
        case Inst::Tag::Pow:
            return ty(p.op_extra.op);
        case Inst::Tag::WireRes:
        case Inst::Tag::WandRes:
        case Inst::Tag::WorRes: {
            // Net resolution always yields a 4-state result at the operand
            // (net) width — a driver conflict is X, an all-high-Z net is Z —
            // even when the driver slots feeding it are 2-state.
            const ValueType lt = ty(p.binary.l);
            return { lt.width, true };
        }
        case Inst::Tag::StrengthRes: {
            // Strength resolution yields a 4-state net-width result, like
            // WireRes; the width is the first driver slot's width.
            const auto      info = ir.extra_data<StrengthResInfo>(p.extra);
            const ValueType lt   = ty(ir.region(info.values)[0]);
            return { lt.width, true };
        }
        case Inst::Tag::Bnot:
            return ty(p.unary);
        case Inst::Tag::StreamReverse:
            // A bit permutation: the operand's width and domain.
            return ty(p.op_imm.op);
        case Inst::Tag::NetPull:
            // A pulled net is 4-state (a driver conflict is still X) but
            // never Z, at the resolved value's width.
            return { ty(p.op_imm.op).width, true };
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
            return { 1, false };
        case Inst::Tag::UItoF:
        case Inst::Tag::SItoF:
        case Inst::Tag::FtoI:
        case Inst::Tag::FtoITrunc:
        case Inst::Tag::FBitcast:
        case Inst::Tag::FResize:
            return value_type_of(pool, p.op_type.type);
        // Every LRM 20.8.2 routine returns a real.
        case Inst::Tag::RealMathUnary:
        case Inst::Tag::RealMathBinary:
            return { 64, false, false, true };

        // $dist_* / seeded $random: a packed i64 {seed:32, draw:32}, sliced
        // by the frontend. A definite 2-state word (never x/z).
        case Inst::Tag::DistUnary:
        case Inst::Tag::DistBinary:
            return { 64, false };
        // $urandom / $urandom_range: a 32-bit unsigned `int` (2-state). An
        // object's own draw (LRM §18.14.2) and a randc draw (LRM §18.6) have
        // the same result type; only the state they advance differs.
        case Inst::Tag::Urandom:
        case Inst::Tag::UrandomSeeded:
        case Inst::Tag::UrandomRange:
        case Inst::Tag::ObjRandom:
        case Inst::Tag::RandcNext:
            return { 32, false };

        // A reduction or logical negation over a 2-state operand is a definite
        // 2-state bit; over a 4-state one it can be x (LRM 11.4.9, 11.4.7).
        case Inst::Tag::RedAnd:
        case Inst::Tag::RedOr:
        case Inst::Tag::RedXor:
        case Inst::Tag::Lnot:
            return { 1, ty(p.unary).four_state };

        // A comparison, a wildcard equality and a logical operator are 4-state when
        // either operand is, and only then can the result be x (LRM 11.4.4, 11.4.5,
        // 11.4.6, 11.4.7). The operands of the logical operators are self-determined,
        // so their widths and domains are independent; the others pair equal ones.
        case Inst::Tag::Eq:
        case Inst::Tag::Ne:
        case Inst::Tag::ULt:
        case Inst::Tag::SLt:
        case Inst::Tag::ULe:
        case Inst::Tag::SLe:
        case Inst::Tag::UGt:
        case Inst::Tag::SGt:
        case Inst::Tag::UGe:
        case Inst::Tag::SGe:
        case Inst::Tag::WildEq:
        case Inst::Tag::Land:
        case Inst::Tag::Lor:
        case Inst::Tag::Limplies:
        case Inst::Tag::Lequiv:
            return { 1, ty(p.binary.l).four_state || ty(p.binary.r).four_state };

        // Case equality compares the planes bit for bit and its result is always a
        // known value (LRM 11.4.5), so it is a definite 2-state bit whatever the
        // operands.
        case Inst::Tag::CaseEq:
        case Inst::Tag::CasezEq:
        case Inst::Tag::CasexEq:
            return { 1, false };

        // An aggregate compare ANDs the leaf compares, so it can be x only when a
        // leaf can carry x or z.
        case Inst::Tag::AggEq:
            return { 1, types::has_four_state_leaf(pool, ir.extra_data<AggEqInfo>(p.op_extra.extra).type), false };

        // The selector decides: slang converts every member to its type, so a
        // member is x-free exactly when the selector is.
        case Inst::Tag::InsideSet:
            return { 1, ty(p.op_extra.op).four_state };

        case Inst::Tag::ClassIsA:
        case Inst::Tag::ClassImplements:
            // A definite bit: the display entry or the interface bit either
            // matches or it does not.
            return { 1, false };

        case Inst::Tag::Concat: {
            uint32_t width = 0;
            bool     four  = false;
            for (Index part : ir.region(ir.extra_data<ExtraSlice>(p.extra))) {
                const ValueType t = ty(part);
                if (t.is_string) { // string concat (all parts are strings)
                    return { 0, false, true };
                }
                width += t.width;
                four = four || t.four_state;
            }
            return { width, four };
        }
        case Inst::Tag::Repl: {
            // Packed only: a string replication is StrRepl (runtime count).
            const ValueType v = ty(p.repl.value);
            return { v.width * p.repl.count, v.four_state };
        }
        case Inst::Tag::StrRepl:
            return { 0, false, true };

        case Inst::Tag::StrLen:
        case Inst::Tag::StrCmp:
        case Inst::Tag::StrICmp:
            return { 32, false };
        case Inst::Tag::StrGetc:
            // getc / s[i] return `byte`, which is signed (LRM 6.16.3 and
            // Table 6-8), so 0xff displays as -1 under %d.
            return { 8, false };
        case Inst::Tag::StrUpper:
        case Inst::Tag::StrLower:
        case Inst::Tag::StrSubstr:
        case Inst::Tag::StrFromPacked:
        case Inst::Tag::Sformatf:
        case Inst::Tag::EnumName:
        case Inst::Tag::ContainerFmtP:
        case Inst::Tag::TaggedFmtP:
            return { 0, false, true };
        case Inst::Tag::StrToPacked:
            return value_type_of(pool, p.op_type.type);
        case Inst::Tag::StrPutc:
            return { 0, false };
        case Inst::Tag::StrAtoi:
        case Inst::Tag::StrAtohex:
        case Inst::Tag::StrAtooct:
        case Inst::Tag::StrAtobin:
            // LRM 6.16.9: declared return type `integer` (4-state), though
            // the conversion itself is always definite — no x/z is
            // reachable from a string's bytes.
            return { 32, true };
        case Inst::Tag::StrAtoreal:
            return { 64, false, false, true };
        case Inst::Tag::StrItoa:
        case Inst::Tag::StrHextoa:
        case Inst::Tag::StrOctoa:
        case Inst::Tag::StrBintoa:
        case Inst::Tag::StrRealtoa:
        case Inst::Tag::StrAppendPacked:
            return { 0, false };

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
            return { 0, false };
    }
    return { 0, false };
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
