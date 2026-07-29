#include "Analysis.h"

#include <algorithm>
#include <optional>
#include <variant>

#include "Module.h"

namespace clotho::ir {

TagInfo tag_info(Inst::Tag tag) {
    // Named shorthands keep the table below scannable. `pure` is the comb-fusion
    // whitelist; `blocks` is the thunk-elision blacklist. Nothing is both: a
    // suspension point or a frame-resident RNG is never a pure value op.
    constexpr TagInfo none{};
    constexpr TagInfo pure{ .pure = true };
    constexpr TagInfo blocks{ .blocks_thunk = true };

    switch (tag) {
        // -- Pure value and control ops: no effect beyond their result, and
        // deterministic, so a spurious re-execution is unobservable. --
        case Inst::Tag::Constant:
        case Inst::Tag::Arg:
        case Inst::Tag::Signal:
        case Inst::Tag::Local:
        case Inst::Tag::BitSelect:
        case Inst::Tag::PartSelect:
        case Inst::Tag::DynPartSelect:
        case Inst::Tag::ElemSelect:
        case Inst::Tag::ArrSlice:
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
        case Inst::Tag::WireRes:
        case Inst::Tag::WandRes:
        case Inst::Tag::WorRes:
        case Inst::Tag::StrengthRes:
        case Inst::Tag::Bnot:
        case Inst::Tag::RedAnd:
        case Inst::Tag::RedOr:
        case Inst::Tag::RedXor:
        case Inst::Tag::Ctz:
        case Inst::Tag::Clz:
        case Inst::Tag::CountBits:
        case Inst::Tag::OneHot:
        case Inst::Tag::OneHot0:
        case Inst::Tag::IsUnknown:
        case Inst::Tag::Sll:
        case Inst::Tag::Srl:
        case Inst::Tag::Sra:
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
        case Inst::Tag::InsideSet:
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
        case Inst::Tag::Land:
        case Inst::Tag::Lor:
        case Inst::Tag::Lnot:
        case Inst::Tag::Limplies:
        case Inst::Tag::Lequiv:
        case Inst::Tag::Select:
        case Inst::Tag::SelectLazy:
        case Inst::Tag::Concat:
        case Inst::Tag::Repl:
        case Inst::Tag::StreamReverse:
        case Inst::Tag::NetPull:
        case Inst::Tag::Resize:
        case Inst::Tag::Cond:
        case Inst::Tag::Switch:
        case Inst::Tag::Loop:
        case Inst::Tag::Break:
        case Inst::Tag::Continue:
        case Inst::Tag::Clog2:
            return pure;

        // -- Unconditionally blocks thunk elision. --
        // Suspension points and scheduling side effects. A named-event trigger
        // wakes waiters and `.triggered` reads per-event state, both outside the
        // pure-function-of-signal-reads model thunk elision assumes.
        case Inst::Tag::Delay:
        case Inst::Tag::Wait:
        case Inst::Tag::TaskCall:
        case Inst::Tag::TaskCallVirtual:
        case Inst::Tag::EventTrigger:
        case Inst::Tag::EventTriggerNb:
        case Inst::Tag::EventTriggerNbDelayed:
        case Inst::Tag::EventTriggered:
        case Inst::Tag::Fork:
        // Frame-resident RNG: the per-process stream lives in the coroutine frame
        // (LRM 18.13 thread stability), which an elided coroutine has no room for.
        // $random / $dist_* stay eligible -- explicit seed var or global stream.
        case Inst::Tag::Urandom:
        case Inst::Tag::UrandomSeeded:
        case Inst::Tag::UrandomRange:
        // Shuffle draws from the frame RNG for the same reason; the other
        // container-ordering methods stay eligible.
        case Inst::Tag::DynArrayShuffle:
        case Inst::Tag::QueueShuffle:
        // Deferred stores need a record slot anchored on the coroutine.
        case Inst::Tag::StoreNbDelay:
        case Inst::Tag::StoreInertialDelay:
        case Inst::Tag::StoreNbEvent:
        // $strobe's pending record is likewise anchored on the coroutine.
        case Inst::Tag::StrobePend:
            return blocks;

        // -- Everything else: neither pure nor thunk-blocking. --
        // Note the two polarities differ deliberately (see TagInfo): landing here
        // makes an op ineligible for fusion (fail safe) but still thunk-eligible.
        case Inst::Tag::ClassNew:
        case Inst::Tag::HandleDeref:
        case Inst::Tag::MemberSelect:
        case Inst::Tag::Load:
        case Inst::Tag::Store:
        case Inst::Tag::StoreNb:
        case Inst::Tag::AggCopy:
        case Inst::Tag::AggEq:
        case Inst::Tag::DynArrayNew:
        case Inst::Tag::DynArrayNewInit:
        case Inst::Tag::DynArraySize:
        case Inst::Tag::DynArrayDelete:
        case Inst::Tag::DynArrayCopy:
        case Inst::Tag::QueueSize:
        case Inst::Tag::QueuePushBack:
        case Inst::Tag::QueuePushFront:
        case Inst::Tag::QueuePopBack:
        case Inst::Tag::QueuePopFront:
        case Inst::Tag::QueueInsert:
        case Inst::Tag::QueueDeleteIdx:
        case Inst::Tag::QueueDeleteAll:
        case Inst::Tag::QueueCopy:
        case Inst::Tag::QueueCopyNb:
        case Inst::Tag::QueueCopyDyn:
        case Inst::Tag::QueueAppendRange:
        case Inst::Tag::DynArrayReverse:
        case Inst::Tag::DynArraySort:
        case Inst::Tag::DynArrayRsort:
        case Inst::Tag::QueueReverse:
        case Inst::Tag::QueueSort:
        case Inst::Tag::QueueRsort:
        case Inst::Tag::AssocSelect:
        case Inst::Tag::AssocNum:
        case Inst::Tag::AssocExists:
        case Inst::Tag::AssocDeleteKey:
        case Inst::Tag::AssocDeleteAll:
        case Inst::Tag::AssocCopy:
        case Inst::Tag::AssocFirst:
        case Inst::Tag::AssocLast:
        case Inst::Tag::AssocNext:
        case Inst::Tag::AssocPrev:
        case Inst::Tag::Ret:
        case Inst::Tag::RetVoid:
        case Inst::Tag::Call:
        case Inst::Tag::CallVirtual:
        case Inst::Tag::StrLen:
        case Inst::Tag::StrGetc:
        case Inst::Tag::StrPutc:
        case Inst::Tag::StrCmp:
        case Inst::Tag::StrICmp:
        case Inst::Tag::StrUpper:
        case Inst::Tag::StrLower:
        case Inst::Tag::StrSubstr:
        case Inst::Tag::EnumName:
        case Inst::Tag::ContainerFmtP:
        case Inst::Tag::StrFromPacked:
        case Inst::Tag::StrToPacked:
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
        case Inst::Tag::Display:
        case Inst::Tag::FDisplay:
        case Inst::Tag::MonitorSet:
        case Inst::Tag::MonitorEmit:
        case Inst::Tag::MonitorOn:
        case Inst::Tag::MonitorOff:
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
        case Inst::Tag::Fscanf:
        case Inst::Tag::Sscanf:
        case Inst::Tag::FreadWord:
        case Inst::Tag::FreadMem:
        case Inst::Tag::TimeFormat:
        case Inst::Tag::Finish:
        case Inst::Tag::CaseViol:
        case Inst::Tag::Severity:
        case Inst::Tag::Fflush:
        case Inst::Tag::System:
        case Inst::Tag::ReadMem:
        case Inst::Tag::DumpFile:
        case Inst::Tag::DumpVars:
        case Inst::Tag::DumpOff:
        case Inst::Tag::DumpOn:
        case Inst::Tag::DumpAll:
        case Inst::Tag::DumpFlush:
        case Inst::Tag::SimTime:
        case Inst::Tag::Random:
        case Inst::Tag::DistUnary:
        case Inst::Tag::DistBinary:
        case Inst::Tag::TestPlusArgs:
        case Inst::Tag::ValuePlusArgs:
            return none;
    }

    return none;
}
namespace {

// ---------------------------------------------------------------------------
// Structural walk: one pass per body producing every fact that needs no
// fixpoint. Replaces what used to be eight separate whole-design walks
// (undriven, partial-undriven, may-z's own undriven call, ptr-arg, the
// subscription-order write scan, and five design_uses_* scans).
// ---------------------------------------------------------------------------

// The constant driven bit range of a packed lvalue select chain. `whole` marks a
// dynamic / unpacked / whole-storage store whose driven bits cannot be pinned
// (the caller then marks every bit driven). Composes nested constant packed
// selects: each PartSelect adds its `lsb` to the accumulated window, a constant
// BitSelect pins one bit. Mirrors the frontend's net_region_of.
struct DriveRange {
    Index    root;
    bool     whole = true;
    uint32_t lo    = 0;
    uint32_t hi    = 0;
};

// Value of a definite integral constant, or nullopt if not a constant or if it
// carries x/z (an ambiguous index cannot pin a bit).
std::optional<uint64_t> const_index(const Module& m, const Ir& ir, Index c) {
    if (ir.tag(c) != Inst::Tag::Constant) {
        return std::nullopt;
    }
    const Ref                      ref   = ir.payload(c).ref;
    const Value&                   v     = std::get<Value>(m.pool.get(ref));
    const Layout                   lay   = m.pool.layout(v.type);
    const std::span<const uint8_t> bytes = m.pool.bytes(ref);
    const uint32_t                 pb    = plane_bytes(lay.width);
    if (lay.domain == Domain::Four) {
        // A definite index has an all-zero xz plane.
        for (uint32_t k = 0; k < pb; ++k) {
            if (bytes[pb + k] != 0) {
                return std::nullopt;
            }
        }
    }
    uint64_t val = 0;
    for (uint32_t k = 0; k < pb && k < 8; ++k) {
        val |= static_cast<uint64_t>(bytes[k]) << (8 * k);
    }

    return val;
}

DriveRange drive_range(const Module& m, const Ir& ir, Index lv) {
    uint32_t lo = 0, hi = 0;
    bool     have = false, dyn = false;
    for (;;) {
        switch (ir.tag(lv)) {
            case Inst::Tag::PartSelect: {
                const PartSelect ps = ir.extra_data<PartSelect>(ir.payload(lv).op_extra.extra);
                if (!have) {
                    lo   = ps.lsb;
                    hi   = ps.msb;
                    have = true;
                } else {
                    lo += ps.lsb;
                    hi += ps.lsb;
                }
                lv = ir.payload(lv).op_extra.op;
                break;
            }
            case Inst::Tag::BitSelect: {
                const std::optional<uint64_t> b = const_index(m, ir, ir.payload(lv).binary.r);
                if (!b) {
                    dyn = true;
                } else if (!have) {
                    lo   = static_cast<uint32_t>(*b);
                    hi   = lo;
                    have = true;
                } else {
                    lo += static_cast<uint32_t>(*b);
                    hi += static_cast<uint32_t>(*b);
                }
                lv = ir.payload(lv).binary.l;
                break;
            }
            case Inst::Tag::DynPartSelect:
            case Inst::Tag::ElemSelect:
            case Inst::Tag::ArrSlice:
            case Inst::Tag::MemberSelect:
                // Runtime-offset packed slice, or an unpacked-array/struct element:
                // the driven bits cannot be pinned in the flat plane.
                dyn = true;
                lv  = ir.payload(lv).op_extra.op;
                break;
            default:
                return { lv, dyn || !have, lo, hi };
        }
    }
}

// Everything the structural walk accumulates.
struct Structural {
    const Module& m;

    // Per-signal.
    std::vector<bool> driven;  // any driver at all (inverted into `undriven`)
    std::vector<bool> ptr_arg; // passed as a non-In actual somewhere
    // Partial-undriven bookkeeping, per candidate signal only.
    std::vector<bool>                  partial_candidate;
    std::vector<uint32_t>              partial_width;
    std::vector<bool>                  partial_touched;
    std::vector<bool>                  partial_whole;
    std::vector<std::vector<uint64_t>> partial_driven;

    DesignUses uses;

    // Per-body impurity verdict, filled while walking. CombFuse::Eligible means
    // "nothing in the body objected"; the caller still has to apply the
    // simple-always, edge and read-containment checks in the right order.
    CombFuse body_verdict = CombFuse::Eligible;
    bool     saw_defer    = false;

    void note_verdict(CombFuse v) {
        // First objection wins, matching the original's early return.
        if (body_verdict == CombFuse::Eligible) {
            body_verdict = v;
        }
    }

    void mark_driven_root(const Ir& ir, Index lv) {
        const Index root = lvalue_root(ir, lv).root;
        if (ir.tag(root) == Inst::Tag::Signal) {
            driven[static_cast<uint32_t>(ir.payload(root).signal)] = true;
        }
    }

    void mark_sig_whole(uint32_t sig) {
        if (partial_candidate[sig]) {
            partial_touched[sig] = true;
            partial_whole[sig]   = true;
        }
    }

    void mark_whole_root(const Ir& ir, Index lv) {
        const Index root = lvalue_root(ir, lv).root;
        if (ir.tag(root) == Inst::Tag::Signal) {
            mark_sig_whole(static_cast<uint32_t>(ir.payload(root).signal));
        }
    }

    void set_range(uint32_t sig, uint32_t a, uint32_t b) {
        const uint32_t words = (partial_width[sig] + 63) / 64;
        if (partial_driven[sig].empty()) {
            partial_driven[sig].assign(words, 0);
        }
        const uint32_t lo = std::min(a, b);
        const uint32_t hi = std::min(std::max(a, b), partial_width[sig] - 1);
        for (uint32_t bit = lo; bit <= hi; ++bit) {
            partial_driven[sig][bit / 64] |= 1ull << (bit % 64);
        }
    }

    // A store pins its driven range (or marks the whole signal when the range
    // cannot be resolved statically).
    void mark_partial(const Ir& ir, Index lv) {
        const DriveRange dr = drive_range(m, ir, lv);
        if (ir.tag(dr.root) != Inst::Tag::Signal) {
            return;
        }
        const auto sig = static_cast<uint32_t>(ir.payload(dr.root).signal);
        if (!partial_candidate[sig]) {
            return;
        }
        partial_touched[sig] = true;
        if (dr.whole) {
            partial_whole[sig] = true;
        } else {
            set_range(sig, dr.lo, dr.hi);
        }
    }

    // Record every non-In actual of a call as written (copy-out) and pointer-passed.
    void call_actuals(const Ir& ir, FunctionId fid, ExtraSlice argslice, bool copy_out, std::vector<Signal>* writes) {
        const Function& callee = m.function(fid);
        const auto      args   = ir.region(argslice);
        for (size_t a = 0; a < args.size(); ++a) {
            if (callee.arg_dirs.empty() || callee.arg_dirs[a] == ArgDir::In) {
                continue;
            }
            const Index root = lvalue_root(ir, args[a]).root;
            if (ir.tag(root) != Inst::Tag::Signal) {
                continue;
            }
            const Signal s                    = ir.payload(root).signal;
            ptr_arg[static_cast<uint32_t>(s)] = true;
            if (!copy_out) {
                continue;
            }
            driven[static_cast<uint32_t>(s)] = true;
            mark_sig_whole(static_cast<uint32_t>(s));
            if (writes != nullptr) {
                writes->push_back(s);
            }
        }
    }

    // One walk over one body. `facts` collects the per-body read/write/call sets;
    // `for_process` enables the process-only accounting (VCD use, deferred
    // severity, the comb-fusion body verdict).
    void walk(const Ir& ir, BodyFacts& facts, bool for_process) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const Index          i = static_cast<Index>(k);
            const Inst::Payload& p = ir.payload(i);
            const Inst::Tag      t = ir.tag(i);

            // Comb-fusion purity: a whitelist, so anything unclassified objects.
            // Tags with their own handling below (loads, stores, calls) are
            // exempted there; everything else must be `pure`.
            if (for_process && !tag_info(t).pure) {
                switch (t) {
                    case Inst::Tag::Load:
                    case Inst::Tag::Store:
                        break; // handled below
                    case Inst::Tag::StoreNb:
                    case Inst::Tag::StoreNbDelay:
                    case Inst::Tag::StoreInertialDelay:
                    case Inst::Tag::StoreNbEvent:
                        note_verdict(CombFuse::NbaWrite);
                        break;
                    case Inst::Tag::Call:
                    case Inst::Tag::CallVirtual:
                        note_verdict(CombFuse::HasCall);
                        break;
                    default:
                        note_verdict(CombFuse::ImpureOp);
                        break;
                }
            }

            switch (t) {
                case Inst::Tag::Load: {
                    const Index root = lvalue_root(ir, p.unary).root;
                    if (ir.tag(root) == Inst::Tag::Signal) {
                        facts.reads.push_back(ir.payload(root).signal);
                    }
                    break;
                }
                case Inst::Tag::Store: {
                    const Index root = lvalue_root(ir, p.binary.l).root;
                    if (ir.tag(root) == Inst::Tag::Signal) {
                        facts.writes.push_back(ir.payload(root).signal);
                        if (for_process) {
                            // Change detection is only verified for packed
                            // storage; a string / unpacked-array store path is not
                            // covered by the no-change argument.
                            const auto  sig = static_cast<uint32_t>(ir.payload(root).signal);
                            const auto& ty  = std::get<Type>(m.pool.get(m.signals()[sig].type));
                            if (ty.kind == Type::Kind::String || ty.kind == Type::Kind::UnpackedArray) {
                                note_verdict(CombFuse::UnsupportedWrite);
                            }
                        }
                    }
                    mark_driven_root(ir, p.binary.l);
                    mark_partial(ir, p.binary.l);
                    break;
                }
                case Inst::Tag::StoreNb:
                    mark_driven_root(ir, p.binary.l);
                    mark_partial(ir, p.binary.l);
                    break;
                case Inst::Tag::StoreInertialDelay:
                    // An inertially delayed driver drives its net even before the
                    // first propagation commits, so the net powers up x (its
                    // driver's initial output), not undriven z.
                    mark_driven_root(ir, p.op_extra.op);
                    mark_partial(ir, p.op_extra.op);
                    break;
                case Inst::Tag::ReadMem: {
                    const Signal target                   = ir.extra_data<ReadMemInfo>(p.op_extra.extra).target;
                    driven[static_cast<uint32_t>(target)] = true;
                    mark_sig_whole(static_cast<uint32_t>(target));
                    break;
                }
                case Inst::Tag::TaskCall: {
                    const TaskCallInfo ti = ir.extra_data<TaskCallInfo>(p.extra);
                    call_actuals(ir, ti.func, ti.args, /*copy_out=*/true, &facts.writes);
                    facts.calls.push_back(ti.func);
                    break;
                }
                case Inst::Tag::TaskCallVirtual: {
                    const VirtualCallInfo vci = ir.extra_data<VirtualCallInfo>(p.extra);
                    call_actuals(ir, vci.func, vci.args, /*copy_out=*/true, &facts.writes);
                    facts.calls.push_back(vci.func);
                    break;
                }
                case Inst::Tag::Call: {
                    const CallInfo ci = ir.extra_data<CallInfo>(p.extra);
                    call_actuals(ir, ci.func, ci.args, /*copy_out=*/false, nullptr);
                    break;
                }
                case Inst::Tag::CallVirtual: {
                    const VirtualCallInfo vci = ir.extra_data<VirtualCallInfo>(p.extra);
                    call_actuals(ir, vci.func, vci.args, /*copy_out=*/false, nullptr);
                    break;
                }
                case Inst::Tag::StrobePend:
                    uses.strobe = true;
                    break;
                case Inst::Tag::MonitorSet:
                    uses.monitor = true;
                    break;
                case Inst::Tag::CaseViol:
                    uses.observed = true;
                    break;
                case Inst::Tag::Severity:
                    // Deferred assertions are processes-only: the frontend rejects
                    // one inside a called function or task, so a process scan
                    // already covers every case the backend must handle.
                    if (for_process && ir.extra_data<SeverityInfo>(p.extra).defer != 0) {
                        uses.defer = true;
                        saw_defer  = true;
                    }
                    break;
                case Inst::Tag::DumpFile:
                case Inst::Tag::DumpVars:
                case Inst::Tag::DumpOff:
                case Inst::Tag::DumpOn:
                case Inst::Tag::DumpAll:
                case Inst::Tag::DumpFlush:
                    if (for_process) {
                        uses.vcd = true;
                    }
                    break;
                default:
                    break;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Value sweep: the monotone may-z / may-x lattices, iterated to fixpoint.
//
// One bit each per storage node (module signals, plus every function's formals,
// frame locals and return value). Each sweep walks every body computing
// per-instruction may-z / may-x bottom-up -- the flat instruction store is in
// creation order, so operands always precede their users -- and stores fold the
// rhs bits into the root storage node. Loop back-edges and cross-process flow
// ride the storage bits, so sweeping to fixpoint covers them.
//
// may-z SEED SET (exhaustive; a new z-producing construct MUST be added here or
// elision becomes unsound, and the failure mode is a silent wrong value):
//   1. Undriven 4-state nets -- a net with no driver holds z (LRM 6.6).
//      Undriven variables default to x, not z.
//   2. Interned constants with z bits (a literal like 4'bz01x, or a parameter
//      folded from one).
//   3. $readmem targets with a 4-state element type (the file may hold z digits).
//   4. $fscanf / $sscanf 4-state output targets (%h/%b/%o accept z and ? per LRM
//      Table 21-7).
//   5. Wire / wand / wor / strength resolution results (all drivers high-Z).
//   6. DPI imports with a 4-state return or output (opaque C body).
// Not seeds: gate primitives and force/release are lowered through the stores
// and resolvers above, so they need no separate entry.
//
// may-x is deliberately COARSER: "x if any operand is x OR z", plus an explicit
// generator set. Treating a z operand as producing x over-approximates for the
// z-transparent ops (a Concat of z yields z, not x), which is sound -- may_x is a
// may-analysis, so a spurious true only forgoes an optimization. Precision is
// explicitly deferred: see Analysis.h on why this bit cannot drive a diagnostic.
// ---------------------------------------------------------------------------
struct ValueSweep {
    struct FnState {
        const Function*      func = nullptr;
        std::vector<uint8_t> param_z, param_x;
        std::vector<uint8_t> local_z, local_x;
        uint8_t              result_z = 0;
        uint8_t              result_x = 0;
    };

    const Module&        m;
    std::vector<uint8_t> sig_z;
    std::vector<uint8_t> sig_x;
    std::vector<FnState> fns;
    bool                 changed = false;

    std::vector<uint8_t> val_z;
    std::vector<uint8_t> val_x;

    void mark(uint8_t& bit) {
        if (bit == 0) {
            bit     = 1;
            changed = true;
        }
    }

    uint8_t z_of(Index i) const {
        return val_z[static_cast<uint32_t>(i)];
    }
    uint8_t x_of(Index i) const {
        return val_x[static_cast<uint32_t>(i)];
    }

    // True when an interned constant carries any z (resp. x) bit. Canonical
    // 4-state values are [val plane][xz plane] with VPI encoding, so
    // z = xz & ~val and x = xz & val.
    bool constant_has(Ref ref, bool want_z) const {
        const Value& v = std::get<Value>(m.pool.get(ref));
        const Type&  t = std::get<Type>(m.pool.get(v.type));
        if (t.kind == Type::Kind::UnpackedArray) {
            // No frontend interns unpacked-array values today; their plane layout
            // would be per-element, which the half-split below misreads. Stay sound
            // if one ever appears.
            return true;
        }
        if (m.pool.layout(v.type).domain != Domain::Four) {
            return false;
        }
        const std::span<const uint8_t> bytes = m.pool.bytes(ref);
        const size_t                   half  = bytes.size() / 2;
        for (size_t k = 0; k < half; ++k) {
            const uint8_t xz  = bytes[half + k];
            const uint8_t val = bytes[k];
            if ((want_z ? (xz & static_cast<uint8_t>(~val)) : (xz & val)) != 0) {
                return true;
            }
        }

        return false;
    }

    // Whether the storage behind an lvalue is 4-state (can hold x/z at all).
    // Unknown leaves are conservatively 4-state.
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

    bool storage_x(const Ir& ir, const FnState* fn, Index lv) const {
        const Index root = lvalue_root(ir, lv).root;
        switch (ir.tag(root)) {
            case Inst::Tag::Signal:
                return sig_x[static_cast<uint32_t>(ir.payload(root).signal)] != 0;
            case Inst::Tag::Local:
                return fn == nullptr || fn->local_x[ir.payload(root).local.index] != 0;
            default:
                return true;
        }
    }

    // Fold may-z / may-x into the storage node behind a store's lhs chain. A
    // partial store is a plain union: a may-analysis needs no kill.
    void store_into(const Ir& ir, FnState* fn, Index lhs, bool rhs_z, bool rhs_x) {
        if (!rhs_z && !rhs_x) {
            return;
        }
        const Index root = lvalue_root(ir, lhs).root;
        switch (ir.tag(root)) {
            case Inst::Tag::Signal: {
                const auto s = static_cast<uint32_t>(ir.payload(root).signal);
                if (rhs_z) {
                    mark(sig_z[s]);
                }
                if (rhs_x) {
                    mark(sig_x[s]);
                }
                break;
            }
            case Inst::Tag::Local:
                if (fn != nullptr) {
                    const uint32_t l = ir.payload(root).local.index;
                    if (rhs_z) {
                        mark(fn->local_z[l]);
                    }
                    if (rhs_x) {
                        mark(fn->local_x[l]);
                    }
                }
                break;
            default:
                // Args are read-only; no other leaf is a store target.
                break;
        }
    }

    // Conservative may-x of a value-producing op: x if any operand is x or z.
    // Uses Ir::operands, which is exhaustive apart from the variadic system tasks
    // and InsideSet -- all handled explicitly at their arms below.
    uint8_t operand_x(const Ir& ir, Index i) const {
        std::array<Index, 4> scratch{};
        uint8_t              x = 0;
        for (const Index op : ir.operands(i, scratch)) {
            x |= x_of(op) | z_of(op);
        }

        return x;
    }

    // One forward pass over a body. `fn` is the enclosing function's state, or
    // null for a process. Exhaustive over tags so a new op fails to compile here
    // until its x/z behavior is classified.
    void sweep(const Ir& ir, FnState* fn) {
        val_z.assign(ir.inst_count(), 0);
        val_x.assign(ir.inst_count(), 0);
        for (uint32_t raw = 0; raw < ir.inst_count(); ++raw) {
            const Index          i = static_cast<Index>(raw);
            const Inst::Payload& p = ir.payload(i);
            // NOTE: the may-x default is applied per arm, NOT here, because the
            // x/z rule differs per tag -- so only the arms whose result is the
            // OR of their operands call operand_x. This is a semantic split, not
            // a safety one: Ir::operands is sound for every tag (it used to
            // decode any Extra payload as an ExtraSlice, fabricating a span on
            // tags like MonitorSet whose record is a StrobePendInfo).

            switch (ir.tag(i)) {
                case Inst::Tag::Constant:
                    val_z[raw] = constant_has(p.ref, /*want_z=*/true);
                    val_x[raw] = constant_has(p.ref, /*want_z=*/false);
                    break;
                case Inst::Tag::Signal:
                    val_z[raw] = sig_z[static_cast<uint32_t>(p.signal)];
                    val_x[raw] = sig_x[static_cast<uint32_t>(p.signal)];
                    break;
                case Inst::Tag::Arg:
                    // An Arg outside a function cannot occur; stay conservative.
                    val_z[raw] = fn != nullptr ? fn->param_z[p.arg.position] : 1;
                    val_x[raw] = fn != nullptr ? fn->param_x[p.arg.position] : 1;
                    break;
                case Inst::Tag::Local:
                    val_z[raw] = fn != nullptr ? fn->local_z[p.local.index] : 1;
                    val_x[raw] = fn != nullptr ? fn->local_x[p.local.index] : 1;
                    break;

                // Selects narrow their base; a runtime index can additionally make
                // the result x (out of range), never z.
                case Inst::Tag::BitSelect:
                    val_z[raw] = z_of(p.binary.l);
                    val_x[raw] = operand_x(ir, i);
                    break;
                case Inst::Tag::PartSelect:
                case Inst::Tag::MemberSelect:
                case Inst::Tag::ArrSlice:
                    val_z[raw] = z_of(p.op_extra.op);
                    val_x[raw] = operand_x(ir, i);
                    break;
                case Inst::Tag::DynPartSelect:
                    val_z[raw] = z_of(p.op_extra.op);
                    val_x[raw] = 1; // runtime offset: out-of-range reads x
                    break;
                case Inst::Tag::ElemSelect: {
                    // A fixed-array element narrows its base's z. A dynamic-array or
                    // queue element lives in a separate heap slot that keeps its own
                    // z plane, so a 4-state element may carry z regardless of the
                    // handle; report conservatively.
                    const auto  es = ir.extra_data<ElemSelect>(p.op_extra.extra);
                    const Type& at = std::get<Type>(m.pool.get(es.array_type));
                    if (at.kind == Type::Kind::DynArray || at.kind == Type::Kind::Queue) {
                        val_z[raw] = m.pool.layout(at.element).domain == Domain::Four ? 1 : 0;
                    } else {
                        val_z[raw] = z_of(p.op_extra.op);
                    }
                    val_x[raw] = 1; // runtime index: out-of-range reads x
                    break;
                }

                case Inst::Tag::AssocSelect: {
                    // An associative-array element lives in its own heap slot that
                    // keeps its own z plane, so a 4-state value element may carry z
                    // regardless of the handle; report conservatively.
                    const auto  aa   = ir.extra_data<AssocArg>(p.op_extra.extra);
                    const Type& at   = std::get<Type>(m.pool.get(aa.assoc_type));
                    const bool  four = m.pool.layout(at.element).domain == Domain::Four;
                    val_z[raw]       = four ? 1 : 0;
                    val_x[raw]       = four ? 1 : 0;
                    break;
                }
                case Inst::Tag::AssocNum:
                case Inst::Tag::AssocExists:
                case Inst::Tag::AssocDeleteKey:
                case Inst::Tag::AssocDeleteAll:
                case Inst::Tag::AssocCopy:
                case Inst::Tag::AssocFirst:
                case Inst::Tag::AssocLast:
                case Inst::Tag::AssocNext:
                case Inst::Tag::AssocPrev:
                    // num()/exists() and the traversal status are definite ints;
                    // delete/copy write the handle, not a value plane slot.
                    val_z[raw] = 0;
                    val_x[raw] = 0;
                    break;

                case Inst::Tag::ClassNew:
                    // The object handle is a 2-state 64-bit pointer; never x/z.
                    val_z[raw] = 0;
                    val_x[raw] = 0;
                    break;
                case Inst::Tag::HandleDeref:
                    // An aggregate lvalue rooted at the heap object body, whose
                    // 4-state property leaves keep their own planes; report
                    // conservatively.
                    val_z[raw] = 1;
                    val_x[raw] = 1;
                    break;
                case Inst::Tag::Load:
                    val_z[raw] = z_of(p.unary);
                    val_x[raw] = x_of(p.unary);
                    break;
                case Inst::Tag::Store:
                case Inst::Tag::StoreNb:
                    store_into(ir, fn, p.binary.l, z_of(p.binary.r) != 0, x_of(p.binary.r) != 0);
                    val_x[raw] = 0;
                    break;
                case Inst::Tag::StoreNbDelay:
                case Inst::Tag::StoreInertialDelay: {
                    const auto info = ir.extra_data<StoreNbDelayInfo>(p.op_extra.extra);
                    store_into(ir, fn, p.op_extra.op, z_of(info.value) != 0, x_of(info.value) != 0);
                    val_x[raw] = 0;
                    break;
                }
                case Inst::Tag::StoreNbEvent: {
                    const auto info = ir.extra_data<StoreNbEventInfo>(p.op_extra.extra);
                    store_into(ir, fn, p.op_extra.op, z_of(info.value) != 0, x_of(info.value) != 0);
                    val_x[raw] = 0;
                    break;
                }
                case Inst::Tag::AggCopy: {
                    const auto info = ir.extra_data<AggCopyInfo>(p.op_extra.extra);
                    store_into(ir, fn, p.op_extra.op, storage_z(ir, fn, info.src), storage_x(ir, fn, info.src));
                    val_x[raw] = 0;
                    break;
                }
                case Inst::Tag::DynArrayNew:
                case Inst::Tag::DynArrayNewInit:
                case Inst::Tag::DynArraySize:
                case Inst::Tag::DynArrayDelete:
                case Inst::Tag::DynArrayCopy:
                    // size() is a definite int; new / delete / copy write the
                    // handle, not a plane slot.
                    val_z[raw] = 0;
                    val_x[raw] = 0;
                    break;
                case Inst::Tag::QueuePopBack:
                case Inst::Tag::QueuePopFront: {
                    // pop* yields an element; each element keeps its own planes in
                    // the heap buffer, so report conservatively by element domain.
                    const bool four =
                      m.pool.layout(std::get<Type>(m.pool.get(p.op_type.type)).element).domain == Domain::Four;
                    val_z[raw] = four ? 1 : 0;
                    val_x[raw] = four ? 1 : 0;
                    break;
                }
                case Inst::Tag::QueueSize:
                case Inst::Tag::QueuePushBack:
                case Inst::Tag::QueuePushFront:
                case Inst::Tag::QueueInsert:
                case Inst::Tag::QueueDeleteIdx:
                case Inst::Tag::QueueDeleteAll:
                case Inst::Tag::QueueCopy:
                case Inst::Tag::QueueCopyNb:
                case Inst::Tag::QueueCopyDyn:
                case Inst::Tag::QueueAppendRange:
                case Inst::Tag::DynArrayReverse:
                case Inst::Tag::DynArraySort:
                case Inst::Tag::DynArrayRsort:
                case Inst::Tag::DynArrayShuffle:
                case Inst::Tag::QueueReverse:
                case Inst::Tag::QueueSort:
                case Inst::Tag::QueueRsort:
                case Inst::Tag::QueueShuffle:
                    // size() is a definite int; the rest are void statements that
                    // permute or write the handle's buffer, not a plane slot.
                    val_z[raw] = 0;
                    val_x[raw] = 0;
                    break;

                // Z-transparent value ops: the z plane rides through their
                // lowerings (an unknown shift count / select condition forces x
                // with z = 0, so joining only the data operands is exact for z).
                case Inst::Tag::Sll:
                case Inst::Tag::Srl:
                case Inst::Tag::Sra:
                    val_z[raw] = z_of(p.binary.l);
                    val_x[raw] = operand_x(ir, i);
                    break;
                case Inst::Tag::Select: {
                    const Select select = ir.extra_data<Select>(p.op_extra.extra);
                    val_z[raw]          = z_of(select.a) | z_of(select.b);
                    val_x[raw]          = operand_x(ir, i);
                    break;
                }
                case Inst::Tag::SelectLazy: {
                    // Either branch's value may reach the result; the predicate
                    // only gates control flow and carries no z into it.
                    const SelectLazy s = ir.extra_data<SelectLazy>(p.op_extra.extra);
                    val_z[raw]         = z_of(s.then_val) | z_of(s.else_val);
                    val_x[raw]         = operand_x(ir, i);
                    break;
                }
                case Inst::Tag::Concat:
                    for (const Index part : ir.region(ir.extra_data<ExtraSlice>(p.extra))) {
                        val_z[raw] |= z_of(part);
                        val_x[raw] |= x_of(part) | z_of(part);
                    }
                    break;
                case Inst::Tag::Repl:
                    val_z[raw] = z_of(p.repl.value);
                    val_x[raw] = operand_x(ir, i);
                    break;
                case Inst::Tag::StreamReverse:
                    // A pure bit permutation: z rides through unchanged.
                    val_z[raw] = z_of(p.op_imm.op);
                    val_x[raw] = operand_x(ir, i);
                    break;
                case Inst::Tag::Resize:
                    // Sign extension replicates a z MSB; truncation keeps low z
                    // bits; a 4->2-state resize does NOT clear z (the backend casts
                    // all planes).
                    val_z[raw] = z_of(p.op_type.op);
                    val_x[raw] = operand_x(ir, i);
                    break;

                // Seed 5: resolution can produce genuine Z (all drivers high-Z) or
                // X (a driver conflict) regardless of operand domains.
                case Inst::Tag::WireRes:
                case Inst::Tag::WandRes:
                case Inst::Tag::WorRes:
                case Inst::Tag::StrengthRes:
                    val_z[raw] = 1;
                    val_x[raw] = 1;
                    break;

                // A tri0/tri1 pull replaces every Z bit with 0 / 1, so its result
                // never carries Z -- the pulled net can be Z-elided even though its
                // driver slots (and the resolution fold) may be Z.
                case Inst::Tag::NetPull:
                // Z-absorbing ops: their lowerings emit a zero z plane (a z input
                // reads as X through the must/may planes). Their may-x is the
                // default operand join computed above, which already treats an
                // absorbed z as producing x.
                case Inst::Tag::Add:
                case Inst::Tag::Sub:
                case Inst::Tag::Mul:
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
                case Inst::Tag::AggEq:
                case Inst::Tag::Limplies:
                case Inst::Tag::Lequiv:
                    val_x[raw] = operand_x(ir, i);
                    break;

                // Division-family x generators: a zero divisor yields x whatever
                // the operands' own cleanliness (LRM 11.4.1).
                case Inst::Tag::Div:
                case Inst::Tag::Mod:
                case Inst::Tag::Pow:
                    val_x[raw] = 1;
                    break;

                // The `inside` operator: Ir::operands cannot express its member
                // operands (they live in its info record), so the default join
                // would under-approximate. Conservative.
                case Inst::Tag::InsideSet:
                    val_x[raw] = 1;
                    break;

                // Real ops absorb z too: a real value carries no z plane and every
                // float-tag lowering emits z = 0 (ItoF reads the must half;
                // FtoI / FBitcast produce definite bits). A real carries no x plane
                // either, so the integral->float and compare forms are clean.
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
                    val_x[raw] = 0;
                    break;

                case Inst::Tag::Ret:
                    if (fn != nullptr) {
                        if (z_of(p.unary) != 0) {
                            mark(fn->result_z);
                        }
                        if (x_of(p.unary) != 0) {
                            mark(fn->result_x);
                        }
                    }
                    val_x[raw] = 0;
                    break;
                case Inst::Tag::RetVoid:
                    val_x[raw] = 0;
                    break;
                case Inst::Tag::Call: {
                    // A function shares the task argument ABI: In copies the
                    // actual's value in; Inout copies storage in and out; Out copies
                    // out; Ref aliases the actual's storage live.
                    const CallInfo               ci     = ir.extra_data<CallInfo>(p.extra);
                    const Function&              callee = m.function(ci.func);
                    FnState&                     cs     = fns[static_cast<uint32_t>(ci.func)];
                    const std::span<const Index> args   = ir.region(ci.args);
                    for (size_t k = 0; k < args.size(); ++k) {
                        const ArgDir dir = callee.arg_dirs.empty() ? ArgDir::In : callee.arg_dirs[k];
                        if (dir == ArgDir::In) {
                            if (z_of(args[k]) != 0) {
                                mark(cs.param_z[k]);
                            }
                            if (x_of(args[k]) != 0) {
                                mark(cs.param_x[k]);
                            }
                        }
                        if (dir == ArgDir::Inout || dir == ArgDir::Ref) {
                            if (storage_z(ir, fn, args[k])) {
                                mark(cs.param_z[k]);
                            }
                            if (storage_x(ir, fn, args[k])) {
                                mark(cs.param_x[k]);
                            }
                        }
                        if (dir != ArgDir::In) {
                            store_into(ir, fn, args[k], cs.local_z[k] != 0, cs.local_x[k] != 0);
                        }
                    }
                    val_z[raw] = cs.result_z;
                    val_x[raw] = cs.result_x;
                    break;
                }
                case Inst::Tag::CallVirtual:
                case Inst::Tag::TaskCallVirtual: {
                    // The concrete callee is chosen at run time (any override in the
                    // hierarchy), so a single callee's FnState is not sound. Be
                    // conservative: the result may carry x/z, and every
                    // out/inout/ref actual may be written with x/z. Over-marking
                    // only forgoes an optimization, never correctness.
                    const VirtualCallInfo        vci    = ir.extra_data<VirtualCallInfo>(p.extra);
                    const Function&              callee = m.function(vci.func);
                    const std::span<const Index> args   = ir.region(vci.args);
                    for (size_t k = 0; k < args.size(); ++k) {
                        const ArgDir dir = callee.arg_dirs.empty() ? ArgDir::In : callee.arg_dirs[k];
                        if (dir != ArgDir::In) {
                            store_into(ir, fn, args[k], true, true);
                        }
                    }
                    val_z[raw] = 1;
                    val_x[raw] = 1;
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
                        // copy-out writes the formal's frame local back. A ref
                        // formal reads the actual's storage on entry and writes any
                        // x/z it produces straight back (live), like an inout.
                        if (dir == ArgDir::In) {
                            if (z_of(args[k]) != 0) {
                                mark(cs.param_z[k]);
                            }
                            if (x_of(args[k]) != 0) {
                                mark(cs.param_x[k]);
                            }
                        }
                        if (dir == ArgDir::Inout || dir == ArgDir::Ref) {
                            if (storage_z(ir, fn, args[k])) {
                                mark(cs.param_z[k]);
                            }
                            if (storage_x(ir, fn, args[k])) {
                                mark(cs.param_x[k]);
                            }
                        }
                        if (dir != ArgDir::In) {
                            store_into(ir, fn, args[k], cs.local_z[k] != 0, cs.local_x[k] != 0);
                        }
                    }
                    val_x[raw] = 0;
                    break;
                }

                case Inst::Tag::ReadMem: {
                    // Seed 3: the file may contain x and z digits. A 2-state memory
                    // reads x/z as 0 and stays clean.
                    const ReadMemInfo info = ir.extra_data<ReadMemInfo>(p.op_extra.extra);
                    if (m.pool.layout(m.signal(info.target).type).domain == Domain::Four) {
                        mark(sig_z[static_cast<uint32_t>(info.target)]);
                        mark(sig_x[static_cast<uint32_t>(info.target)]);
                    }
                    val_x[raw] = 0;
                    break;
                }
                case Inst::Tag::Fscanf:
                case Inst::Tag::Sscanf: {
                    // Seed 4: %h / %b / %o accept x, z and ? digits (LRM Table
                    // 21-7), which land in a 4-state target's planes. A 2-state
                    // target has no such plane; the runtime drops parsed x/z to 0.
                    const ScanfInfo info = ir.extra_data<ScanfInfo>(p.extra);
                    for (const Index out : ir.region(info.args)) {
                        if (storage_four_state(ir, fn, out)) {
                            store_into(ir, fn, out, true, true);
                        }
                    }
                    val_x[raw] = 0;
                    break;
                }
                case Inst::Tag::ValuePlusArgs: {
                    // Writes its target from clean ASCII digits, or 'bx on an
                    // illegal character (LRM 21.6 says "'bx", never z), so it seeds
                    // x but not z. The status result itself is definite.
                    std::array<Index, 4> scratch{};
                    for (const Index out : ir.operands(i, scratch)) {
                        if (storage_four_state(ir, fn, out)) {
                            store_into(ir, fn, out, false, true);
                        }
                    }
                    val_x[raw] = 0;
                    break;
                }

                // Valueless / always-known: no x or z effect. Region bodies are
                // swept flatly by this same loop (their stores land through the
                // Store cases), so only the node itself is nil here.
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
                case Inst::Tag::StrobePend:
                case Inst::Tag::MonitorSet:
                case Inst::Tag::MonitorEmit:
                case Inst::Tag::MonitorOn:
                case Inst::Tag::MonitorOff:
                case Inst::Tag::TimeFormat:
                case Inst::Tag::Finish:
                case Inst::Tag::CaseViol:
                case Inst::Tag::Severity:
                case Inst::Tag::Fflush:
                case Inst::Tag::SimTime:
                case Inst::Tag::Random:
                case Inst::Tag::DistUnary:
                case Inst::Tag::DistBinary:
                case Inst::Tag::Urandom:
                case Inst::Tag::UrandomSeeded:
                case Inst::Tag::UrandomRange:
                case Inst::Tag::EventTrigger:
                case Inst::Tag::EventTriggerNb:
                case Inst::Tag::EventTriggerNbDelayed:
                case Inst::Tag::EventTriggered:
                case Inst::Tag::Clog2:
                case Inst::Tag::Ctz:
                case Inst::Tag::Clz:
                case Inst::Tag::CountBits:
                case Inst::Tag::OneHot:
                case Inst::Tag::OneHot0:
                case Inst::Tag::IsUnknown:
                case Inst::Tag::StrLen:
                case Inst::Tag::StrGetc:
                case Inst::Tag::StrPutc:
                case Inst::Tag::StrCmp:
                case Inst::Tag::StrICmp:
                case Inst::Tag::StrUpper:
                case Inst::Tag::StrLower:
                case Inst::Tag::StrSubstr:
                case Inst::Tag::EnumName:
                case Inst::Tag::ContainerFmtP:
                case Inst::Tag::StrFromPacked:
                case Inst::Tag::StrToPacked:
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
                case Inst::Tag::System:
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
                case Inst::Tag::TestPlusArgs:
                case Inst::Tag::DumpFile:
                case Inst::Tag::DumpVars:
                case Inst::Tag::DumpOff:
                case Inst::Tag::DumpOn:
                case Inst::Tag::DumpAll:
                case Inst::Tag::DumpFlush:
                    val_x[raw] = 0;
                    break;
            }
        }
    }
};

// Shape eligible for coroutine elision. A BLACKLIST: `tag_info().blocks_thunk`
// covers the unconditional disqualifiers, and the two payload-dependent ones are
// spelled out here. Note the polarity -- an unclassified opcode stays eligible,
// unlike the comb-fusion whitelist. See TagInfo in Analysis.h.
bool is_simple_always(const Process& p) {
    if (p.activation != Activation::Always || p.wait_events.empty()) {
        return false;
    }
    for (uint32_t k = 0; k < p.ir.inst_count(); ++k) {
        const Index     idx = static_cast<Index>(k);
        const Inst::Tag t   = p.ir.tag(idx);
        if (tag_info(t).blocks_thunk) {
            return false;
        }
        switch (t) {
            case Inst::Tag::Severity:
                // A deferred immediate assertion needs a record slot anchored on
                // the coroutine; a plain $error / $info does not.
                if (p.ir.extra_data<SeverityInfo>(p.ir.payload(idx).extra).defer != 0) {
                    return false;
                }
                break;
            case Inst::Tag::Store: {
                // A blocking write to a signal in the process's own sensitivity
                // list would self-retrigger, which an elided coroutine cannot
                // service. Membership in the raw wait list is identical to
                // membership in the deduped canonical set, so scan it directly.
                const LvalueRoot r = lvalue_root(p.ir, p.ir.payload(idx).binary.l);
                if (p.ir.tag(r.root) != Inst::Tag::Signal) {
                    break;
                }
                const Signal sig = p.ir.payload(r.root).signal;
                for (const WaitEvent& w : p.wait_events) {
                    if (w.signal == sig) {
                        return false;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    return true;
}

// Assemble the comb-fusion verdict. The CHECK ORDER IS LOAD BEARING: the
// simple-always test must come first, which is what keeps the deferred-store arms
// of `body_verdict` unreachable and the verdict histogram stable.
CombFuse comb_fuse_verdict(const Process& p, const ProcessFacts& pf, CombFuse body_verdict) {
    if (!pf.simple_always) {
        return CombFuse::NotSimpleAlways;
    }
    // Level-only: comb fusion reasons about settled values, so an edge-triggered
    // member belongs to identical-sensitivity fusion instead.
    for (const WaitEvent& w : p.wait_events) {
        if (w.edge != Edge::Any) {
            return CombFuse::EdgeSensitive;
        }
    }
    if (body_verdict != CombFuse::Eligible) {
        return body_verdict;
    }
    // Every read signal must be in the sensitivity list, else a spurious run
    // observes state the unfused schedule never samples. Verified here rather than
    // trusting the frontend's inference: explicit lists and any inference gap fail
    // mechanically.
    for (const Signal s : pf.body.reads) {
        bool in_sens = false;
        for (const WaitEvent& w : p.wait_events) {
            if (w.signal == s) {
                in_sens = true;
                break;
            }
        }
        if (!in_sens) {
            return CombFuse::ReadOutsideSens;
        }
    }

    return CombFuse::Eligible;
}

} // namespace

DesignFacts analyze(const Module& m) {
    const size_t nsig  = m.signals().size();
    const size_t nproc = m.processes().size();
    const size_t nfn   = m.function_count();

    DesignFacts facts;
    facts.processes.resize(nproc);
    facts.functions.resize(nfn);

    // ---- One structural walk per body. ----
    Structural st{ .m = m };
    st.driven.assign(nsig, false);
    st.ptr_arg.assign(nsig, false);
    st.partial_candidate.assign(nsig, false);
    st.partial_width.assign(nsig, 0);
    st.partial_touched.assign(nsig, false);
    st.partial_whole.assign(nsig, false);
    st.partial_driven.resize(nsig);

    // A partial-undriven mask is only meaningful for a packed 4-state net: a
    // 2-state signal has no z plane, an unpacked array has per-element storage
    // (whole-signal power-on, a follow-up), and a variable powers on x not z.
    for (uint32_t i = 0; i < nsig; ++i) {
        const SignalInfo& s = m.signals()[i];
        if (!s.is_net || s.force_agg) {
            continue;
        }
        if (std::get<Type>(m.pool.get(s.type)).kind == Type::Kind::UnpackedArray) {
            continue;
        }
        const Layout lay = m.pool.layout(s.type);
        if (lay.domain != Domain::Four || lay.width == 0) {
            continue;
        }
        st.partial_candidate[i] = true;
        st.partial_width[i]     = lay.width;
    }

    std::vector<CombFuse> body_verdict(nproc, CombFuse::Eligible);
    for (size_t k = 0; k < nproc; ++k) {
        st.body_verdict = CombFuse::Eligible;
        st.saw_defer    = false;
        st.walk(m.processes()[k].ir, facts.processes[k].body, /*for_process=*/true);
        body_verdict[k]                      = st.body_verdict;
        facts.processes[k].deferred_severity = st.saw_defer;
    }
    for (size_t f = 0; f < nfn; ++f) {
        st.walk(m.function(static_cast<FunctionId>(f)).ir,
                facts.functions[f],
                /*for_process=*/false);
    }

    facts.undriven = st.driven;
    facts.undriven.flip();
    facts.ptr_arg = std::move(st.ptr_arg);
    facts.uses    = st.uses;

    // Flatten the per-signal partial masks. A signal gets a mask only when it is a
    // candidate that was touched, has some driven bits, was never written whole,
    // and has at least one bit left undriven; everything else gets an empty range.
    facts.undriven_off.assign(nsig + 1, 0);
    for (uint32_t i = 0; i < nsig; ++i) {
        facts.undriven_off[i] = static_cast<uint32_t>(facts.undriven_bits.size());
        if (!st.partial_candidate[i] || !st.partial_touched[i] || st.partial_whole[i] || st.partial_driven[i].empty()) {
            continue;
        }
        const uint32_t        w     = st.partial_width[i];
        const uint32_t        words = (w + 63) / 64;
        std::vector<uint64_t> un(words, 0);
        bool                  any = false;
        for (uint32_t bit = 0; bit < w; ++bit) {
            if ((st.partial_driven[i][bit / 64] & (1ull << (bit % 64))) == 0) {
                un[bit / 64] |= 1ull << (bit % 64);
                any = true;
            }
        }
        if (any) {
            facts.undriven_bits.insert(facts.undriven_bits.end(), un.begin(), un.end());
        }
    }
    facts.undriven_off[nsig] = static_cast<uint32_t>(facts.undriven_bits.size());

    // ---- Per-process shape and fusion verdicts. ----
    for (size_t k = 0; k < nproc; ++k) {
        const Process& p                 = m.processes()[k];
        facts.processes[k].simple_always = is_simple_always(p);
        facts.processes[k].comb_fuse     = comb_fuse_verdict(p, facts.processes[k], body_verdict[k]);
    }

    // ---- Value lattices, iterated to fixpoint. ----
    ValueSweep vs{ .m = m };
    vs.sig_z.assign(nsig, 0);
    vs.sig_x.assign(nsig, 0);
    vs.fns.resize(nfn);
    for (size_t f = 0; f < nfn; ++f) {
        const Function& fn = m.function(static_cast<FunctionId>(f));
        vs.fns[f].func     = &fn;
        vs.fns[f].param_z.assign(fn.param_types.size(), 0);
        vs.fns[f].param_x.assign(fn.param_types.size(), 0);
        vs.fns[f].local_z.assign(fn.local_types.size(), 0);
        vs.fns[f].local_x.assign(fn.local_types.size(), 0);
        if (fn.is_dpi) {
            // Seed 6: a DPI import's C body is opaque (its fake IR body returns a
            // constant), so it cannot be analyzed. Conservatively assume it can
            // produce x/z on any 4-state return / output / inout -- otherwise
            // elision drops the plane of a DPI logic result.
            if (fn.return_type != m.pool.void_type && m.pool.layout(fn.return_type).domain == Domain::Four) {
                vs.fns[f].result_z = 1;
                vs.fns[f].result_x = 1;
            }
            for (size_t k = 0; k < fn.param_types.size(); ++k) {
                if (m.pool.layout(fn.param_types[k]).domain == Domain::Four) {
                    vs.fns[f].local_z[k] = 1;
                    vs.fns[f].local_x[k] = 1;
                }
            }
        }
    }

    // Seed 1: an undriven 4-state net holds its default value z (LRM 6.6). The
    // backend initializes exactly these to z.
    for (uint32_t i = 0; i < nsig; ++i) {
        if (facts.undriven[i] && m.signals()[i].is_net && m.pool.layout(m.signals()[i].type).domain == Domain::Four) {
            vs.sig_z[i] = 1;
        }
    }

    // may-x seed: an uninitialized 4-state variable powers on to x (LRM 6.6). We
    // cannot prove initialization in general -- that needs the reset-before-read
    // temporal analysis docs/design/never-x-inference.md §6 rates as the hard part
    // -- so approximate soundly: a 4-state signal is may-x unless a PreInit
    // process (declaration initializers, LRM 6.8) writes it. Deliberately coarse;
    // §8 of that doc already predicts low coverage for v1.
    std::vector<bool> preinit_written(nsig, false);
    for (size_t k = 0; k < nproc; ++k) {
        if (m.processes()[k].activation != Activation::PreInit) {
            continue;
        }
        for (const Signal s : facts.processes[k].body.writes) {
            preinit_written[static_cast<uint32_t>(s)] = true;
        }
    }
    for (uint32_t i = 0; i < nsig; ++i) {
        if (!preinit_written[i] && m.pool.layout(m.signals()[i].type).domain == Domain::Four) {
            vs.sig_x[i] = 1;
        }
    }

    do {
        vs.changed = false;
        // The backend copies formal k into frame Local k at entry (tasks have no
        // explicit copy in the IR; functions do, making this a harmless join).
        for (ValueSweep::FnState& fs : vs.fns) {
            for (size_t k = 0; k < fs.param_z.size(); ++k) {
                if (fs.param_z[k] != 0) {
                    vs.mark(fs.local_z[k]);
                }
                if (fs.param_x[k] != 0) {
                    vs.mark(fs.local_x[k]);
                }
            }
        }
        for (const Process& proc : m.processes()) {
            vs.sweep(proc.ir, nullptr);
        }
        for (size_t f = 0; f < nfn; ++f) {
            vs.sweep(m.function(static_cast<FunctionId>(f)).ir, &vs.fns[f]);
        }
    } while (vs.changed);

    facts.may_z = { vs.sig_z.begin(), vs.sig_z.end() };
    facts.may_x = { vs.sig_x.begin(), vs.sig_x.end() };

    return facts;
}

} // namespace clotho::ir
