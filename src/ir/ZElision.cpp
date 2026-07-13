#include "ZElision.h"

// May-z analysis. Z enters a design through exactly two constructs today (the
// seed set), then flows only through value-preserving ops; every logic /
// arithmetic / comparison op absorbs z into x (its lowering emits a zero z
// plane — see the plane rationale in LlvmBackend.cpp).
//
// Seeds:
//   1. Undriven 4-state nets: a net with no driver holds its default value z
//      (LRM 6.6). Undriven variables default to x, not z.
//   2. Interned constants with z bits (a 4-state literal like 4'bz01x, or a
//      parameter folded from one).
//   3. $readmem targets with a 4-state element type (the file may contain z
//      digits; a 2-state memory reads x/z as 0 — see rt_readmem).
//   4. $fscanf / $sscanf 4-state output targets (%h / %b / %o accept z and ?
//      digits per LRM Table 21-7; a 2-state target drops x/z to 0 — see
//      scan_run / write_word_planes).
// Not seeds: gate primitives, force/release, inout ports, and net kinds with
// resolution/pull semantics (wand, tri0, trireg, ...) are unimplemented
// loud-fails; when implemented, their z-producing forms MUST seed here.
//
// The lattice is one monotone false->true bit per storage node: module signals,
// plus each function's formals, frame locals, and return value. Each sweep
// walks every process / function body computing per-instruction may-z bottom-up
// (the flat store is in creation order, so operands precede users); stores fold
// the rhs bit into the root storage node. Loop back-edges and cross-process
// flow ride the storage bits, so sweeping to fixpoint covers them.

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "Ir.h"
#include "Module.h"

namespace clotho::ir {

namespace {

struct Analysis {
    // Per-function facts, indexed by FunctionId.
    struct FnState {
        // The function these facts belong to (for local storage types).
        const Function* func = nullptr;
        // May-z of each formal, joined over every call site.
        std::vector<uint8_t> param_z;
        // May-z of each frame local.
        std::vector<uint8_t> local_z;
        // May-z of the return value, joined over every Ret.
        uint8_t result_z = 0;
    };

    const Module& m;

    // May-z per module signal; the analysis result.
    std::vector<uint8_t> sig_z;
    std::vector<FnState> fns;
    // Whether the current sweep flipped any storage bit.
    bool changed = false;

    // Per-instruction may-z of the Ir being swept, reset per body.
    std::vector<uint8_t> val_z;

    void mark(uint8_t& bit) {
        if (bit == 0) {
            bit     = 1;
            changed = true;
        }
    }

    // True when an interned constant carries any z bit. Canonical 4-state
    // values are [val plane][xz plane] with VPI encoding, so z = xz & ~val.
    bool constant_has_z(Ref ref) const {
        const Value& v = std::get<Value>(m.pool.get(ref));
        const Type&  t = std::get<Type>(m.pool.get(v.type));
        if (t.kind == Type::Kind::UnpackedArray) {
            // NOTE: no frontend interns unpacked-array values today; their plane
            // layout would be per-element, which the half-split below misreads.
            // Stay sound if one ever appears.
            return true;
        }
        if (m.pool.layout(v.type).domain != Domain::Four) {
            return false;
        }
        const std::span<const uint8_t> bytes = m.pool.bytes(ref);
        const size_t                   half  = bytes.size() / 2;
        for (size_t k = 0; k < half; ++k) {
            if ((bytes[half + k] & ~bytes[k]) != 0) {
                return true;
            }
        }

        return false;
    }

    // Whether the storage behind an lvalue is 4-state (can hold a z bit at
    // all). Unknown leaves are conservatively 4-state.
    bool storage_four_state(const Ir& ir, const FnState* fn, Index lv) const {
        const Index root = lvalue_root(ir, lv).root;
        switch (ir.tag(root)) {
            case Inst::Tag::Signal:
                return m.pool.layout(m.signal(ir.payload(root).signal).type).domain == Domain::Four;
            case Inst::Tag::Local:
                return fn == nullptr ||
                       m.pool.layout(fn->func->local_types[ir.payload(root).local.index]).domain == Domain::Four;
            default:
                return true;
        }
    }

    // May-z of the storage behind an lvalue. Unknown leaves are conservatively z.
    bool storage_z(const Ir& ir, const FnState* fn, Index lv) const {
        const Index root = lvalue_root(ir, lv).root;
        switch (ir.tag(root)) {
            case Inst::Tag::Signal:
                return sig_z[static_cast<uint32_t>(ir.payload(root).signal)] != 0;
            case Inst::Tag::Local:
                return fn == nullptr || fn->local_z[ir.payload(root).local.index] != 0;
            default:
                return true;
        }
    }

    // Fold a may-z rhs into the storage node behind a store's lhs chain. A
    // partial store is a plain union: a may-analysis needs no kill.
    void store_into(const Ir& ir, FnState* fn, Index lhs, bool rhs_z) {
        if (!rhs_z) {
            return;
        }
        const Index root = lvalue_root(ir, lhs).root;
        switch (ir.tag(root)) {
            case Inst::Tag::Signal:
                mark(sig_z[static_cast<uint32_t>(ir.payload(root).signal)]);
                break;
            case Inst::Tag::Local:
                if (fn != nullptr) {
                    mark(fn->local_z[ir.payload(root).local.index]);
                }
                break;
            default:
                // Args are read-only; no other leaf is a store target.
                break;
        }
    }

    uint8_t z_of(Index i) const {
        return val_z[static_cast<uint32_t>(i)];
    }

    // One forward pass over a body. `fn` is the enclosing function's state, or
    // null for a process. Exhaustive over tags so a new op fails to compile
    // here until its z behavior is classified.
    void sweep(const Ir& ir, FnState* fn) {
        val_z.assign(ir.inst_count(), 0);
        for (uint32_t raw = 0; raw < ir.inst_count(); ++raw) {
            const Index          i = static_cast<Index>(raw);
            const Inst::Payload& p = ir.payload(i);
            switch (ir.tag(i)) {
                case Inst::Tag::Constant:
                    val_z[raw] = constant_has_z(p.ref);
                    break;
                case Inst::Tag::Signal:
                    val_z[raw] = sig_z[static_cast<uint32_t>(p.signal)];
                    break;
                case Inst::Tag::Arg:
                    // An Arg outside a function cannot occur; stay conservative.
                    val_z[raw] = fn != nullptr ? fn->param_z[p.arg.position] : 1;
                    break;
                case Inst::Tag::Local:
                    val_z[raw] = fn != nullptr ? fn->local_z[p.local.index] : 1;
                    break;

                // Selects narrow their base; the index operand can only make the
                // result X, never z.
                case Inst::Tag::BitSelect:
                    val_z[raw] = z_of(p.binary.l);
                    break;
                case Inst::Tag::PartSelect:
                case Inst::Tag::DynPartSelect:
                case Inst::Tag::MemberSelect:
                    val_z[raw] = z_of(p.op_extra.op);
                    break;
                case Inst::Tag::ElemSelect: {
                    // A fixed-array element narrows its base's z. A dynamic-array
                    // element lives in a separate heap slot that keeps its own z
                    // plane (dynarr_elem), so a 4-state element may carry z
                    // regardless of the handle; report conservatively so a
                    // downstream signal is not z-elided.
                    const auto  es = ir.extra_data<ElemSelect>(p.op_extra.extra);
                    const Type& at = std::get<Type>(m.pool.get(es.array_type));
                    if (at.kind == Type::Kind::DynArray || at.kind == Type::Kind::Queue) {
                        val_z[raw] = m.pool.layout(at.element).domain == Domain::Four ? 1 : 0;
                    } else {
                        val_z[raw] = z_of(p.op_extra.op);
                    }
                    break;
                }

                case Inst::Tag::ClassNew:
                    // The object handle is a 2-state 64-bit pointer value; never z.
                    val_z[raw] = 0;
                    break;
                case Inst::Tag::HandleDeref:
                    // An aggregate lvalue rooted at the heap object body, whose
                    // 4-state property leaves keep their own z plane (like a
                    // dynamic-array element); report conservatively so a member
                    // read cannot z-elide a downstream signal.
                    val_z[raw] = 1;
                    break;
                case Inst::Tag::Load:
                    val_z[raw] = z_of(p.unary);
                    break;
                case Inst::Tag::Store:
                case Inst::Tag::StoreNb:
                    store_into(ir, fn, p.binary.l, z_of(p.binary.r) != 0);
                    break;
                case Inst::Tag::StoreNbDelay: {
                    // The deferred store writes the target with the sampled value's
                    // z-ness (drives whether the target keeps a z plane).
                    const auto info = ir.extra_data<StoreNbDelayInfo>(p.op_extra.extra);
                    store_into(ir, fn, p.op_extra.op, z_of(info.value) != 0);
                    break;
                }
                case Inst::Tag::StoreNbEvent: {
                    const auto info = ir.extra_data<StoreNbEventInfo>(p.op_extra.extra);
                    store_into(ir, fn, p.op_extra.op, z_of(info.value) != 0);
                    break;
                }
                case Inst::Tag::AggCopy: {
                    // A whole-object copy propagates the source's may-z to the
                    // destination (a plain-array memcpy requires both to share the
                    // z layout; an aggregate uses conservative z regardless).
                    const auto info = ir.extra_data<AggCopyInfo>(p.op_extra.extra);
                    store_into(ir, fn, p.op_extra.op, storage_z(ir, fn, info.src));
                    break;
                }
                case Inst::Tag::DynArrayNew:
                case Inst::Tag::DynArrayNewInit:
                case Inst::Tag::DynArraySize:
                case Inst::Tag::DynArrayDelete:
                case Inst::Tag::DynArrayCopy:
                    // size() is a definite int (no z); new / delete / copy write
                    // the handle, not a plane slot, so nothing propagates here.
                    val_z[raw] = 0;
                    break;
                case Inst::Tag::QueuePopBack:
                case Inst::Tag::QueuePopFront:
                    // pop* yields an element, which for a 4-state element may
                    // carry z (each element keeps its own z plane in the heap
                    // buffer); report conservatively by element domain.
                    val_z[raw] =
                      m.pool.layout(std::get<Type>(m.pool.get(p.op_type.type)).element).domain ==
                          Domain::Four
                        ? 1
                        : 0;
                    break;
                case Inst::Tag::QueueSize:
                case Inst::Tag::QueuePushBack:
                case Inst::Tag::QueuePushFront:
                case Inst::Tag::QueueInsert:
                case Inst::Tag::QueueDeleteIdx:
                case Inst::Tag::QueueDeleteAll:
                case Inst::Tag::QueueCopy:
                    // size() is a definite int (no z); the rest are statements
                    // that write the handle, not a plane slot.
                    val_z[raw] = 0;
                    break;

                // Z-transparent value ops: the z plane rides through their
                // lowerings (an unknown shift count / select condition forces X
                // with z = 0, so joining only the data operands is exact).
                case Inst::Tag::Sll:
                case Inst::Tag::Srl:
                case Inst::Tag::Sra:
                    val_z[raw] = z_of(p.binary.l);
                    break;
                case Inst::Tag::Select: {
                    const Select select = ir.extra_data<Select>(p.op_extra.extra);
                    val_z[raw]    = z_of(select.a) | z_of(select.b);
                    break;
                }
                case Inst::Tag::SelectLazy: {
                    // Either branch's value may reach the result; the predicate
                    // only gates control flow and carries no z into it.
                    const SelectLazy s = ir.extra_data<SelectLazy>(p.op_extra.extra);
                    val_z[raw]     = z_of(s.then_val) | z_of(s.else_val);
                    break;
                }
                case Inst::Tag::Concat:
                    for (const Index part : ir.region(ir.extra_data<ExtraSlice>(p.extra))) {
                        val_z[raw] |= z_of(part);
                    }
                    break;
                case Inst::Tag::Repl:
                    val_z[raw] = z_of(p.repl.value);
                    break;
                case Inst::Tag::Resize:
                    // Sign extension replicates a z MSB; truncation keeps low z
                    // bits; a 4->2-state resize does NOT clear z (the backend
                    // casts all planes).
                    val_z[raw] = z_of(p.op_type.op);
                    break;

                // Z-absorbing ops: their lowerings emit a zero z plane (a z
                // input reads as X through the must/may planes).
                case Inst::Tag::Add:
                case Inst::Tag::Sub:
                case Inst::Tag::Mul:
                case Inst::Tag::Div:
                case Inst::Tag::Mod:
                case Inst::Tag::Pow:
                case Inst::Tag::Band:
                case Inst::Tag::Bor:
                case Inst::Tag::Bxor:
                case Inst::Tag::Bxnor:
                case Inst::Tag::Bnot:
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
                // Aggregate equality and the inside operator are compare +
                // logical reductions: z-absorbing.
                case Inst::Tag::AggEq:
                case Inst::Tag::InsideSet:
                    break;

                // Real ops absorb z too: a real value carries no z plane and
                // every float-tag lowering emits z = 0 (ItoF reads the must
                // half; FtoI / FBitcast produce definite bits).
                case Inst::Tag::FAdd:
                case Inst::Tag::FSub:
                case Inst::Tag::FMul:
                case Inst::Tag::FDiv:
                case Inst::Tag::FPow:
                case Inst::Tag::FNeg:
                case Inst::Tag::FEq:
                case Inst::Tag::FNe:
                case Inst::Tag::FLt:
                case Inst::Tag::FLe:
                case Inst::Tag::FGt:
                case Inst::Tag::FGe:
                case Inst::Tag::ItoF:
                case Inst::Tag::FtoI:
                case Inst::Tag::FtoITrunc:
                case Inst::Tag::FBitcast:
                case Inst::Tag::FResize:
                case Inst::Tag::RealMathUnary:
                case Inst::Tag::RealMathBinary:
                    break;

                case Inst::Tag::Ret:
                    if (fn != nullptr && z_of(p.unary) != 0) {
                        mark(fn->result_z);
                    }
                    break;
                case Inst::Tag::RetVoid:
                    // No value returned; nothing to join.
                    break;
                case Inst::Tag::Call: {
                    // A function shares the task argument ABI: In copies the actual's
                    // value in; Inout copies storage in and out; Out copies out; Ref
                    // aliases the actual's storage live. Same z-flow as TaskCall below.
                    const CallInfo               ci     = ir.extra_data<CallInfo>(p.extra);
                    const Function&              callee = m.function(ci.func);
                    FnState&                     cs     = fns[static_cast<uint32_t>(ci.func)];
                    const std::span<const Index> args   = ir.region(ci.args);
                    for (size_t k = 0; k < args.size(); ++k) {
                        const ArgDir dir = callee.arg_dirs.empty() ? ArgDir::In : callee.arg_dirs[k];
                        if (dir == ArgDir::In && z_of(args[k]) != 0) {
                            mark(cs.param_z[k]);
                        }
                        if ((dir == ArgDir::Inout || dir == ArgDir::Ref) && storage_z(ir, fn, args[k])) {
                            mark(cs.param_z[k]);
                        }
                        if (dir != ArgDir::In && cs.local_z[k] != 0) {
                            store_into(ir, fn, args[k], true);
                        }
                    }
                    val_z[raw] = cs.result_z;
                    break;
                }
                case Inst::Tag::TaskCall: {
                    const TaskCallInfo           ti     = ir.extra_data<TaskCallInfo>(p.extra);
                    const Function&              callee = m.function(ti.func);
                    FnState&                     cs     = fns[static_cast<uint32_t>(ti.func)];
                    const std::span<const Index> args   = ir.region(ti.args);
                    for (size_t k = 0; k < args.size(); ++k) {
                        const ArgDir dir = callee.arg_dirs[k];
                        // Copy-in reads the actual's value (In) or storage (Inout);
                        // copy-out writes the formal's frame local back (Out/Inout). A
                        // ref formal reads the actual's storage on entry and writes any
                        // z it produces straight back (live), like an inout.
                        if (dir == ArgDir::In && z_of(args[k]) != 0) {
                            mark(cs.param_z[k]);
                        }
                        if ((dir == ArgDir::Inout || dir == ArgDir::Ref) && storage_z(ir, fn, args[k])) {
                            mark(cs.param_z[k]);
                        }
                        if (dir != ArgDir::In && cs.local_z[k] != 0) {
                            store_into(ir, fn, args[k], true);
                        }
                    }
                    break;
                }

                case Inst::Tag::ReadMem: {
                    // Seed: the file may contain z digits. A 2-state memory
                    // reads x/z as 0 and stays clean.
                    const ReadMemInfo info = ir.extra_data<ReadMemInfo>(p.op_extra.extra);
                    if (m.pool.layout(m.signal(info.target).type).domain == Domain::Four) {
                        mark(sig_z[static_cast<uint32_t>(info.target)]);
                    }
                    break;
                }
                case Inst::Tag::Fscanf:
                case Inst::Tag::Sscanf: {
                    // Seed: %h / %b / %o accept z and ? digits (LRM Table 21-7),
                    // which land in a 4-state target's z plane. A 2-state target
                    // has no z plane; the runtime drops parsed x/z to 0 there
                    // (LRM 6.3.1) and it stays clean.
                    const ScanfInfo info = ir.extra_data<ScanfInfo>(p.extra);
                    for (const Index out : ir.region(info.args)) {
                        if (storage_four_state(ir, fn, out)) {
                            store_into(ir, fn, out, true);
                        }
                    }
                    break;
                }

                // Valueless / always-known: no z effect. Switch arm and default
                // bodies are swept flatly by this same loop (their stores land
                // through the Store cases), so only the node itself is nil here.
                case Inst::Tag::Cond:
                case Inst::Tag::Switch:
                case Inst::Tag::Loop:
                case Inst::Tag::Break:
                case Inst::Tag::Continue:
                case Inst::Tag::Delay:
                case Inst::Tag::Wait:
                case Inst::Tag::Fork:
                case Inst::Tag::Display:
                case Inst::Tag::FDisplay:
                case Inst::Tag::TimeFormat:
                case Inst::Tag::Finish:
                case Inst::Tag::CaseViol:
                case Inst::Tag::Severity:
                case Inst::Tag::Fflush:
                case Inst::Tag::SimTime:
                case Inst::Tag::Random:
                // $dist_* / seeded $random return a packed {seed, draw} i64 and
                // $urandom family a 2-state draw — all definite bits, z = 0. The
                // seed writeback is a plain Store swept by the Store cases.
                case Inst::Tag::DistUnary:
                case Inst::Tag::DistBinary:
                case Inst::Tag::Urandom:
                case Inst::Tag::UrandomSeeded:
                case Inst::Tag::UrandomRange:
                // `-> e` / `->> e` / `->> #d e` are valueless; `e.triggered` is a
                // definite 2-state bit. A named event carries no bit planes, so
                // none seed z.
                case Inst::Tag::EventTrigger:
                case Inst::Tag::EventTriggerNb:
                case Inst::Tag::EventTriggerNbDelayed:
                case Inst::Tag::EventTriggered:
                // $clog2 yields a count (or X when the operand has unknown bits);
                // its result z plane is always zero.
                case Inst::Tag::Clog2:
                // Ctz / Clz count the must plane -> a definite bit index, z = 0.
                case Inst::Tag::Ctz:
                case Inst::Tag::Clz:
                // $countbits family and its one-hot / isunknown qualifiers: a
                // definite 2-state result, z = 0.
                case Inst::Tag::CountBits:
                case Inst::Tag::OneHot:
                case Inst::Tag::OneHot0:
                case Inst::Tag::IsUnknown:
                // String values carry no bit planes and the integral string ops
                // (len / getc / cmp / to_packed) always produce known bits.
                case Inst::Tag::StrLen:
                case Inst::Tag::StrGetc:
                case Inst::Tag::StrPutc:
                case Inst::Tag::StrCmp:
                case Inst::Tag::StrICmp:
                case Inst::Tag::StrUpper:
                case Inst::Tag::StrLower:
                case Inst::Tag::StrSubstr:
                case Inst::Tag::EnumName:
                case Inst::Tag::StrFromPacked:
                case Inst::Tag::StrToPacked:
                // atoi/atohex/atooct/atobin/atoreal parse a string's bytes into a
                // definite value (no z plane possible from ASCII digits);
                // itoa/hextoa/octtoa/bintoa/realtoa produce no value at all.
                case Inst::Tag::StrAtoi:
                case Inst::Tag::StrAtohex:
                case Inst::Tag::StrAtooct:
                case Inst::Tag::StrAtobin:
                case Inst::Tag::StrAtoreal:
                case Inst::Tag::StrItoa:
                case Inst::Tag::StrHextoa:
                case Inst::Tag::StrOctoa:
                case Inst::Tag::StrBintoa:
                case Inst::Tag::StrRealtoa:
                // File I/O: descriptors and read counts are always-known integers,
                // and these write-back forms ($fgets / $sformat / $ferror / $fread)
                // store known bytes with a zero z plane, so none seed z into their
                // target ($*scanf, which can parse z digits, seeds above). A
                // never-z target's z word is elided and the runtime is handed a
                // null z pointer.
                case Inst::Tag::FopenFd:
                case Inst::Tag::FopenMcd:
                case Inst::Tag::Fclose:
                case Inst::Tag::Fgetc:
                case Inst::Tag::Ungetc:
                case Inst::Tag::Feof:
                case Inst::Tag::Ftell:
                case Inst::Tag::Fseek:
                case Inst::Tag::Rewind:
                case Inst::Tag::Fgets:
                case Inst::Tag::Sformat:
                case Inst::Tag::Sformatf:
                case Inst::Tag::Ferror:
                case Inst::Tag::FreadWord:
                case Inst::Tag::FreadMem:
                // $test$plusargs never writes a signal. $value$plusargs writes
                // its target from clean ASCII digits, or 'bx on an illegal
                // character (LRM §21.6 says "'bx", never z) — a 4-state
                // target's z plane is always zero.
                case Inst::Tag::TestPlusArgs:
                case Inst::Tag::ValuePlusArgs:
                // VCD dump tasks read signal planes but write no design value, so
                // they seed no z (DumpFile's filename value is always-known bytes).
                case Inst::Tag::DumpFile:
                case Inst::Tag::DumpVars:
                case Inst::Tag::DumpOff:
                case Inst::Tag::DumpOn:
                case Inst::Tag::DumpAll:
                case Inst::Tag::DumpFlush:
                    break;
            }
        }
    }
};

} // namespace

std::vector<bool> compute_undriven(const Module& m) {
    std::vector<bool> driven(m.signals().size(), false);
    const auto        mark_root = [&](const Ir& ir, Index lv) {
        const Index root = lvalue_root(ir, lv).root;
        if (ir.tag(root) == Inst::Tag::Signal) {
            driven[static_cast<uint32_t>(ir.payload(root).signal)] = true;
        }
    };
    const auto scan = [&](const Ir& ir) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const Index          i = static_cast<Index>(k);
            const Inst::Payload& p = ir.payload(i);
            switch (ir.tag(i)) {
                case Inst::Tag::Store:
                case Inst::Tag::StoreNb:
                    mark_root(ir, p.binary.l);
                    break;
                case Inst::Tag::ReadMem:
                    driven[static_cast<uint32_t>(ir.extra_data<ReadMemInfo>(p.op_extra.extra).target)] = true;
                    break;
                case Inst::Tag::TaskCall: {
                    // Copy-out writes each out/inout actual at task completion.
                    const TaskCallInfo ti     = ir.extra_data<TaskCallInfo>(p.extra);
                    const Function&    callee = m.function(ti.func);
                    const auto         args   = ir.region(ti.args);
                    for (size_t a = 0; a < args.size(); ++a) {
                        if (callee.arg_dirs[a] != ArgDir::In) {
                            mark_root(ir, args[a]);
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    };
    for (const Process& p : m.processes()) {
        scan(p.ir);
    }
    for (uint32_t f = 0; f < m.function_count(); ++f) {
        scan(m.function(static_cast<FunctionId>(f)).ir);
    }

    driven.flip();
    return driven;
}

std::vector<bool> compute_may_z(const Module& m) {
    Analysis a{ .m = m };
    a.sig_z.assign(m.signals().size(), 0);
    a.fns.resize(m.function_count());
    for (size_t f = 0; f < m.function_count(); ++f) {
        const Function& fn = m.function(static_cast<FunctionId>(f));
        a.fns[f].func      = &fn;
        a.fns[f].param_z.assign(fn.param_types.size(), 0);
        a.fns[f].local_z.assign(fn.local_types.size(), 0);
        if (fn.is_dpi) {
            // A DPI import's C body is opaque (its fake IR body returns a constant),
            // so it cannot be analyzed for z. Conservatively assume it can produce z
            // on any 4-state return / output / inout — otherwise z-elision drops the
            // z plane of a DPI logic result (the Call result reads result_z; an
            // output arg copies out only if local_z[k] is set).
            if (fn.return_type != m.pool.void_type &&
                m.pool.layout(fn.return_type).domain == Domain::Four) {
                a.fns[f].result_z = 1;
            }
            for (size_t k = 0; k < fn.param_types.size(); ++k) {
                if (m.pool.layout(fn.param_types[k]).domain == Domain::Four) {
                    a.fns[f].local_z[k] = 1;
                }
            }
        }
    }

    // Seed 1: an undriven 4-state net holds its default value z (LRM 6.6).
    // The backend initializes exactly these to z (init_state).
    const std::vector<bool> undriven = compute_undriven(m);
    const auto              sigs     = m.signals();
    for (size_t i = 0; i < sigs.size(); ++i) {
        if (undriven[i] && sigs[i].is_net && m.pool.layout(sigs[i].type).domain == Domain::Four) {
            a.sig_z[i] = 1;
        }
    }

    do {
        a.changed = false;
        // The backend copies formal k into frame Local k at entry (tasks have no
        // explicit copy in the IR; functions do, making this a harmless join).
        for (Analysis::FnState& fs : a.fns) {
            for (size_t k = 0; k < fs.param_z.size(); ++k) {
                if (fs.param_z[k] != 0) {
                    a.mark(fs.local_z[k]);
                }
            }
        }
        for (const Process& proc : m.processes()) {
            a.sweep(proc.ir, nullptr);
        }
        for (size_t f = 0; f < m.function_count(); ++f) {
            a.sweep(m.function(static_cast<FunctionId>(f)).ir, &a.fns[f]);
        }
    } while (a.changed);

    return { a.sig_z.begin(), a.sig_z.end() };
}

} // namespace clotho::ir
