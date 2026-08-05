#include "Legalize.h"

#include <algorithm>

#include "ir/Eval.h"

namespace clotho::legalize {

namespace {

// The arms of a switch, decoded from the flat extra run.
std::vector<ir::SwitchArm> arms_of(const ir::Ir& ir, const ir::SwitchInfo& info) {
    const std::span<const ir::Index> raw    = ir.region(info.arms);
    constexpr size_t                 stride = sizeof(ir::SwitchArm) / sizeof(uint32_t);
    const auto*                      base   = reinterpret_cast<const ir::SwitchArm*>(raw.data());

    return { base, base + raw.size() / stride };
}

// Is this function synthesizable as an OPERATOR?
//
// A pure function's internal branching decides its return VALUE and nothing else:
// it never changes the caller's statement structure, so from the caller's side a
// call is an ordinary value-producing opcode like `add`. That is why nothing is
// inlined here and no IR is rewritten -- the whole of "function synthesis" is
// deciding whether this operator exists.
//
// It has to be contextual rather than a TagInfo bit, because `call` is legal or
// not depending entirely on who is being called.
class FnLegality {
public:
    explicit FnLegality(const ir::Module& m)
      : m_(m) {}

    // Memoised and cycle-safe: a function reached while it is still being decided
    // is recursive, and recursion has no bounded hardware form.
    bool ok(ir::FunctionId id) {
        const auto key = static_cast<uint32_t>(id);
        const auto it  = memo_.find(key);
        if (it != memo_.end()) {
            return it->second == State::Ok;
        }
        memo_[key]        = State::Deciding;
        const bool result = decide(id);
        memo_[key]        = result ? State::Ok : State::Bad;

        return result;
    }

private:
    enum class State : uint8_t { Deciding, Ok, Bad };

    bool decide(ir::FunctionId id) {
        const ir::Function& f = m_.function(id);
        // A DPI call leaves the design and a fork child is a process; neither has
        // a hardware form.
        //
        // `is_task` is deliberately NOT checked. Suspension is a property of the
        // BODY -- `Delay` and `Wait` are ungated opcodes, so body_ok rejects a task
        // that can suspend without being told that tasks suspend. A task that
        // writes only its own frame and cannot suspend is a no-op, and its call is
        // legal precisely because it does nothing. picorv32's `empty_statement`,
        // the shim its `assert directive expands to, is exactly that. Rejecting on
        // the declaration keyword would have thrown away four processes over a
        // subroutine whose body is empty.
        if (f.is_dpi || f.is_fork_child) {
            return false;
        }
        // An output / inout / ref formal writes the CALLER's storage, so the call
        // is a statement with effects rather than a value. Rejected for now.
        for (const ir::ArgDir d : f.arg_dirs) {
            if (d != ir::ArgDir::In) {
                return false;
            }
        }

        return body_ok(f.ir, f.ir.root());
    }

    bool body_ok(const ir::Ir& ir, std::span<const ir::Index> body) {
        for (const ir::Index i : body) {
            const ir::Inst::Tag tag = ir.tag(i);
            switch (tag) {
                case ir::Inst::Tag::Store:
                case ir::Inst::Tag::StoreNb: {
                    // Writing a module signal from a function is legal Verilog and
                    // is a side effect, which an operator does not have. Writes to
                    // the frame (formals, the return variable, temporaries) are
                    // exactly what a function body is made of.
                    const ir::LvalueRoot lv = ir::lvalue_root(ir, ir.payload(i).binary.l);
                    if (ir.tag(lv.root) == ir::Inst::Tag::Signal) {
                        return false;
                    }
                    break;
                }
                case ir::Inst::Tag::Call: {
                    const auto info = ir.extra_data<ir::CallInfo>(ir.payload(i).extra);
                    const auto key  = static_cast<uint32_t>(info.func);
                    const auto seen = memo_.find(key);
                    if (seen != memo_.end() && seen->second == State::Deciding) {
                        return false; // recursion
                    }
                    if (!ok(info.func)) {
                        return false;
                    }
                    break;
                }
                case ir::Inst::Tag::Cond: {
                    const auto r = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
                    if (!body_ok(ir, ir.region(r.then_)) || !body_ok(ir, ir.region(r.else_))) {
                        return false;
                    }
                    break;
                }
                case ir::Inst::Tag::Switch: {
                    const auto info = ir.extra_data<ir::SwitchInfo>(ir.payload(i).op_extra.extra);
                    if (!body_ok(ir, ir.region(info.default_))) {
                        return false;
                    }
                    for (const ir::SwitchArm& a : arms_of(ir, info)) {
                        if (!body_ok(ir, ir.region(a.body))) {
                            return false;
                        }
                    }
                    break;
                }
                case ir::Inst::Tag::Loop: {
                    const auto info = ir.extra_data<ir::LoopInfo>(ir.payload(i).extra);
                    if (!body_ok(ir, ir.region(info.cond)) || !body_ok(ir, ir.region(info.body)) ||
                        !body_ok(ir, ir.region(info.step))) {
                        return false;
                    }
                    break;
                }
                // A lazy select's branch VALUES live in regions, so a ternary is a
                // region-bearing op even though it produces a value. `f = c ? 1 :
                // f(x-1)` hides its recursive call in here.
                case ir::Inst::Tag::SelectLazy: {
                    const auto sl = ir.extra_data<ir::SelectLazy>(ir.payload(i).op_extra.extra);
                    if (!body_ok(ir, ir.region(sl.then_)) || !body_ok(ir, ir.region(sl.else_))) {
                        return false;
                    }
                    break;
                }
                default:
                    if (!ir::tag_info(tag).synthesizable) {
                        return false;
                    }
                    break;
            }
        }

        return true;
    }

    const ir::Module&                             m_;
    ankerl::unordered_dense::map<uint32_t, State> memo_;
};

// Per-process state for one walk. `guards_` grows monotonically and is handed to
// the Result only when the caller asked to keep it.
class Walker {
public:
    // `eval` is null for the base walk. When present, a `cond` whose predicate
    // folds under the pin descends only into the taken arm -- which is exactly
    // §4A.5's "pin the trigger and fold", and it makes must_assign report
    // completeness in the PINNED world rather than the general one.
    Walker(uint32_t                proc_index,
           std::vector<GuardNode>& guards,
           std::vector<Note>&      notes,
           ir::Eval*               eval,
           FnLegality&             fns)
      : proc_(proc_index)
      , guards_(guards)
      , notes_(notes)
      , eval_(eval)
      , fns_(fns) {}

    ProcessResult run(const ir::Ir& ir) {
        walk(ir, ir.root(), GuardId::None);
        // Held == assigned somewhere, but not on every path.
        const SigSet always = must_assign(ir, ir.root(), eval_);
        for (SignalStores& s : result_.signals) {
            s.has_hold = !always.contains(static_cast<uint32_t>(s.signal));
        }
        std::sort(result_.signals.begin(), result_.signals.end(), [](const SignalStores& a, const SignalStores& b) {
            return a.signal < b.signal;
        });

        return std::move(result_);
    }

private:
    // Append an atom, returning the extended guard. O(1), and prefixes are shared.
    GuardId push(GuardId parent, Atom atom) {
        guards_.push_back({ parent, atom });

        return static_cast<GuardId>(guards_.size() - 1);
    }

    SignalStores& slot(ir::Signal s) {
        for (SignalStores& e : result_.signals) {
            if (e.signal == s) {
                return e;
            }
        }
        result_.signals.push_back({ s, {}, false });

        return result_.signals.back();
    }

    void reject(Finding f, ir::Inst::Tag tag, ir::SourceSpan span) {
        result_.synthesizable = false;
        notes_.push_back({ f, proc_, ir::Signal{}, span, tag });
    }

    using SigSet = ankerl::unordered_dense::set<uint32_t>;

    // Signals DEFINITELY assigned on every path through `body`.
    //
    // This is the whole of latch inference, and it has to be computed per PATH,
    // not per arm: `q = b; if (en) q = a;` assigns q on every path even though the
    // `if` has no else. Comparing sibling arms gets that wrong, which is what the
    // first version of this did.
    //
    // Sequence unions, `cond` intersects its two arms, and a `switch` intersects
    // its arms with its default -- a MISSING default is a path that assigns
    // nothing, so it contributes nothing. That is conservative for a case whose
    // labels happen to cover the selector; proving that is coverage analysis (M6),
    // and until then an uncovered case reads as a hold, which is the safe
    // direction.
    static SigSet must_assign(const ir::Ir& ir, std::span<const ir::Index> body, ir::Eval* eval) {
        SigSet out;
        for (const ir::Index i : body) {
            switch (ir.tag(i)) {
                case ir::Inst::Tag::Store:
                case ir::Inst::Tag::StoreNb: {
                    const ir::LvalueRoot lv = ir::lvalue_root(ir, ir.payload(i).binary.l);
                    // A partial write leaves the other bits holding, so it does not
                    // complete the signal.
                    if (lv.whole && ir.tag(lv.root) == ir::Inst::Tag::Signal) {
                        out.insert(static_cast<uint32_t>(ir.payload(lv.root).signal));
                    }
                    break;
                }
                case ir::Inst::Tag::Cond: {
                    const auto      r    = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
                    const ir::Index pred = ir.payload(i).op_extra.op;
                    // Under a pin, a folded predicate makes one arm unreachable, so
                    // the other arm's assignments are unconditional here.
                    if (eval != nullptr) {
                        const ir::Tri t = eval->truth(pred);
                        if (t == ir::Tri::True) {
                            const SigSet only = must_assign(ir, ir.region(r.then_), eval);
                            out.insert(only.begin(), only.end());
                            break;
                        }
                        if (t == ir::Tri::False) {
                            const SigSet only = must_assign(ir, ir.region(r.else_), eval);
                            out.insert(only.begin(), only.end());
                            break;
                        }
                    }
                    const SigSet t2 = must_assign(ir, ir.region(r.then_), eval);
                    for (const uint32_t s : must_assign(ir, ir.region(r.else_), eval)) {
                        if (t2.contains(s)) {
                            out.insert(s);
                        }
                    }
                    break;
                }
                case ir::Inst::Tag::Switch: {
                    const auto                       info = ir.extra_data<ir::SwitchInfo>(ir.payload(i).op_extra.extra);
                    const std::span<const ir::Index> dflt = ir.region(info.default_);
                    if (dflt.empty()) {
                        break; // no default: a path that assigns nothing
                    }
                    SigSet common = must_assign(ir, dflt, eval);
                    for (const ir::SwitchArm& a : arms_of(ir, info)) {
                        const SigSet mine = must_assign(ir, ir.region(a.body), eval);
                        SigSet       next;
                        for (const uint32_t s : common) {
                            if (mine.contains(s)) {
                                next.insert(s);
                            }
                        }
                        common = std::move(next);
                    }
                    out.insert(common.begin(), common.end());
                    break;
                }
                // A loop body may run zero times, so nothing inside it is
                // DEFINITELY assigned. Conservative and correct.
                case ir::Inst::Tag::Loop:
                default:
                    break;
            }
        }

        return out;
    }

    void walk(const ir::Ir& ir, std::span<const ir::Index> body, GuardId guard) {
        for (const ir::Index i : body) {
            const ir::Inst::Tag tag = ir.tag(i);
            switch (tag) {
                case ir::Inst::Tag::Store:
                case ir::Inst::Tag::StoreNb: {
                    const ir::Index      lhs = ir.payload(i).binary.l;
                    const ir::LvalueRoot lv  = ir::lvalue_root(ir, lhs);
                    if (ir.tag(lv.root) != ir::Inst::Tag::Signal) {
                        break; // a frame local: not module storage, not our unit
                    }
                    const ir::Signal s = ir.payload(lv.root).signal;
                    slot(s).stores.push_back(
                      { guard, ir.payload(i).binary.r, i, tag == ir::Inst::Tag::StoreNb, in_loop_ });
                    break;
                }

                case ir::Inst::Tag::Cond: {
                    const auto                       r    = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
                    const ir::Index                  pred = ir.payload(i).op_extra.op;
                    const std::span<const ir::Index> then_body = ir.region(r.then_);
                    const std::span<const ir::Index> else_body = ir.region(r.else_);

                    if (eval_ != nullptr) {
                        const ir::Tri t = eval_->truth(pred);
                        if (t == ir::Tri::True) {
                            walk(ir, then_body, guard);
                            break;
                        }
                        if (t == ir::Tri::False) {
                            walk(ir, else_body, guard);
                            break;
                        }
                    }
                    walk(ir, then_body, push(guard, { pred, AtomKind::CondTrue, {} }));
                    walk(ir, else_body, push(guard, { pred, AtomKind::CondFalse, {} }));
                    break;
                }

                case ir::Inst::Tag::Switch: {
                    const auto                       info = ir.extra_data<ir::SwitchInfo>(ir.payload(i).op_extra.extra);
                    const ir::Index                  sel  = ir.payload(i).op_extra.op;
                    const std::vector<ir::SwitchArm> arms = arms_of(ir, info);
                    const std::span<const ir::Index> dflt = ir.region(info.default_);

                    for (const ir::SwitchArm& a : arms) {
                        walk(ir, ir.region(a.body), push(guard, { sel, AtomKind::SwitchArm, a.keys }));
                    }
                    walk(ir, dflt, push(guard, { sel, AtomKind::SwitchDefault, {} }));
                    break;
                }

                // A loop's contents must be checked even though the loop
                // itself may unroll: `always begin #5 ...; end` is a forever
                // wrapper, and the Delay inside it is the real problem. Whether
                // the bound is static enough to unroll is a separate question
                // (deferred), so the guard is unchanged across the body -- an
                // unrolled iteration inherits the loop's path condition.
                case ir::Inst::Tag::Loop: {
                    const auto info = ir.extra_data<ir::LoopInfo>(ir.payload(i).extra);
                    const bool was  = in_loop_;
                    in_loop_        = true;
                    walk(ir, ir.region(info.cond), guard);
                    walk(ir, ir.region(info.body), guard);
                    walk(ir, ir.region(info.step), guard);
                    in_loop_ = was;
                    break;
                }

                // Same hole on the process side, and here it matters more: without
                // this, `q = s ? noisy(a) : 0` never has `noisy` checked and the
                // process is legalized as something it is not. Value regions carry
                // no guard -- the branch is inside the value, not the statement.
                case ir::Inst::Tag::SelectLazy: {
                    const auto sl = ir.extra_data<ir::SelectLazy>(ir.payload(i).op_extra.extra);
                    walk(ir, ir.region(sl.then_), guard);
                    walk(ir, ir.region(sl.else_), guard);
                    break;
                }

                // A task call is a statement, not a value. It is legal on the same
                // terms: the callee must write no module signal, so the call has no
                // observable effect and contributes nothing to the guarded list.
                // (A task that DOES write module signals needs its writes attributed
                // to the caller's guards, which needs operand identity across two
                // Irs -- guards and store values are bare ir::Index today. Rejected
                // until that exists.)
                case ir::Inst::Tag::TaskCall: {
                    const auto info = ir.extra_data<ir::TaskCallInfo>(ir.payload(i).extra);
                    if (!fns_.ok(info.func)) {
                        reject(Finding::Call, ir::Inst::Tag::TaskCall, {});
                    }
                    break;
                }

                // A call is legal exactly when the callee is an operator. The
                // value is then produced like any other opcode's -- nothing is
                // inlined, and the caller's statement structure is untouched.
                case ir::Inst::Tag::Call: {
                    const auto info = ir.extra_data<ir::CallInfo>(ir.payload(i).extra);
                    if (!fns_.ok(info.func)) {
                        reject(Finding::Call, ir::Inst::Tag::Call, {});
                    }
                    break;
                }

                default: {
                    // The legality gate. `synthesizable` defaults to false in
                    // TagInfo, so an unclassified opcode rejects its process rather
                    // than being silently legalized as something it is not.
                    if (!ir::tag_info(tag).synthesizable) {
                        Finding f = Finding::UnsynthesizableOp;
                        switch (tag) {
                            case ir::Inst::Tag::Delay:
                            case ir::Inst::Tag::Wait:
                                f = Finding::Suspends;
                                break;
                            case ir::Inst::Tag::Call:
                            case ir::Inst::Tag::CallVirtual:
                            case ir::Inst::Tag::TaskCall:
                            case ir::Inst::Tag::TaskCallVirtual:
                                f = Finding::Call;
                                break;
                            case ir::Inst::Tag::Fork:
                                f = Finding::Fork;
                                break;
                            default:
                                break;
                        }
                        reject(f, tag, {});
                    }
                    break;
                }
            }
        }
    }

    uint32_t                proc_;
    std::vector<GuardNode>& guards_;
    std::vector<Note>&      notes_;
    bool                    in_loop_ = false;
    ir::Eval*               eval_    = nullptr;
    FnLegality&             fns_;
    ProcessResult           result_{};
};

} // namespace

namespace {

// Is `value` a constant this legalizer can see? Reset classification turns on it:
// a constant residue is a reset, a non-constant one is an async load.
//
// Sees through Resize because a sized literal lowers as Resize(Constant) -- the
// width cast is not a computation. It does NOT constant-fold arithmetic: a reset
// value of `8'h0F + 1` reads as non-constant and reports as an async load, which
// is a conservative misclassification rather than a wrong one. Folding it needs a
// full constant evaluator (ir::Eval answers 1-bit questions only).
bool is_constant(const ir::Ir& ir, ir::Index value) {
    ir::Index v = value;
    while (ir.tag(v) == ir::Inst::Tag::Resize) {
        v = ir.payload(v).unary;
    }
    // A concatenation of constants is a constant: `{4'ha, 4'h5}` is an ordinary
    // reset value, and the corpus has one.
    if (ir.tag(v) == ir::Inst::Tag::Concat) {
        bool all = true;
        ir.for_each_operand(v, [&](ir::Index op) { all = all && is_constant(ir, op); });

        return all;
    }

    return ir.tag(v) == ir::Inst::Tag::Constant;
}

// Does `value` read exactly the signal being assigned? `if (rst) q <= q` does not
// reset anything: the edge assigns q its own value, so q is a plain flop whose
// clock enable excludes the reset. Reporting it as an async load of q by q would
// be true and useless.
bool is_self(const ir::Ir& ir, ir::Index value, ir::Signal target) {
    ir::Index v = value;
    while (ir.tag(v) == ir::Inst::Tag::Resize) {
        v = ir.payload(v).unary;
    }
    if (ir.tag(v) != ir::Inst::Tag::Load) {
        return false;
    }
    const ir::Index src = ir.payload(v).unary;

    return ir.tag(src) == ir::Inst::Tag::Signal && ir.payload(src).signal == target;
}

// The edge triggers of a process, if any. A level-sensitive list yields none,
// which is what separates combinational/latch from sequential.
std::vector<ir::WaitEvent> edge_triggers(const ir::Process& p) {
    std::vector<ir::WaitEvent> out;
    for (const ir::WaitEvent& w : p.wait_events) {
        if (w.edge != ir::Edge::Any) {
            out.push_back(w);
        }
    }

    return out;
}

// Does `root`'s operand cone read signal `t`?
//
// A real cone walk, not an index scan: `for_each_operand` is the exhaustive
// accessor, so a signal read through a variadic tail (concat, a call's args) is
// found too. Cones are small and the visited set caps the work at cone size, so
// a shared operand is not re-walked.
bool cone_reads(const ir::Ir& ir, ir::Index root, ir::Signal t, ankerl::unordered_dense::set<uint32_t>& seen) {
    if (!seen.insert(static_cast<uint32_t>(root)).second) {
        return false;
    }
    if (ir.tag(root) == ir::Inst::Tag::Signal && ir.payload(root).signal == t) {
        return true;
    }
    // A lazy select holds its branch values in REGIONS, and regions are not
    // operands. `if (rst || rst2)` short-circuits into one of these, so without
    // this the second reset is invisible in the guard and counts as a free
    // trigger -- which made the clock ambiguous and classified nothing.
    if (ir.tag(root) == ir::Inst::Tag::SelectLazy) {
        const auto sl = ir.extra_data<ir::SelectLazy>(ir.payload(root).op_extra.extra);
        for (const ir::Index i : ir.region(sl.then_)) {
            if (cone_reads(ir, i, t, seen)) {
                return true;
            }
        }
        for (const ir::Index i : ir.region(sl.else_)) {
            if (cone_reads(ir, i, t, seen)) {
                return true;
            }
        }
        if (cone_reads(ir, sl.then_val, t, seen) || cone_reads(ir, sl.else_val, t, seen)) {
            return true;
        }
    }

    bool found = false;
    ir.for_each_operand(root, [&](ir::Index op) { found = found || cone_reads(ir, op, t, seen); });

    return found;
}

// Does any guard of any store to this signal test `t`?
//
// The clock is the trigger that appears in NO guard (§4A.5). This is position-
// free: a reset read as data in the else arm still classifies, because appearing
// in a guard is what matters, not appearing at all.
bool trigger_in_guards(const ir::Ir& ir, const std::vector<GuardNode>& arena, const SignalStores& e, ir::Signal t) {
    ankerl::unordered_dense::set<uint32_t> seen;
    for (const GuardedStore& st : e.stores) {
        for (GuardId g = st.guard; g != GuardId::None;) {
            const GuardNode& n = arena[static_cast<uint32_t>(g)];
            if (cone_reads(ir, n.atom.pred, t, seen)) {
                return true;
            }
            g = n.parent;
        }
    }

    return false;
}

} // namespace

Result run(const ir::Module& module, const ir::DesignFacts* /*facts*/, Options options) {
    Result result;
    result.processes.reserve(module.processes().size());

    FnLegality             fns(module);
    std::vector<GuardNode> guards;
    for (size_t k = 0; k < module.processes().size(); ++k) {
        const ir::Process& proc = module.processes()[k];
        const ir::Ir&      ir   = proc.ir;

        // Guards are always built: classification needs them to find the clock.
        // They are only HANDED BACK when the caller asked.
        const size_t                     arena_base = guards.size();
        Walker                           w(static_cast<uint32_t>(k), guards, result.notes, nullptr, fns);
        ProcessResult                    pr       = w.run(ir);
        const std::vector<ir::WaitEvent> triggers = edge_triggers(proc);

        // Pin each trigger asserted and re-walk, ONCE per trigger.
        //
        // The residue depends on the trigger, not on the signal, so doing this
        // inside the per-signal loop below would re-walk the whole process S x T
        // times -- which is exactly the O(S^2) shape that costs yosys's proc_mux
        // 18.3 s on this design. Measured: hoisting it took hazard3 from 3.8 ms
        // back to ~1 ms once functions made its big CSR process classifiable.
        //
        // A posedge pins 1 and a negedge pins 0; that is the whole of polarity
        // handling, and a mismatched condition simply folds the other way (4A.1).
        // The residues share the caller's guard arena rather than a scratch one, so
        // the conditions they hand to Decomp stay addressable. They are truncated
        // with everything else when the caller did not ask to keep guards.
        std::vector<ProcessResult> residues;
        residues.reserve(triggers.size());
        ankerl::unordered_dense::map<uint32_t, ProcessResult> sync_by_clock;

        if (pr.synthesizable) {
            std::vector<Note> ignored;
            for (const ir::WaitEvent& t : triggers) {
                const uint64_t asserted = t.edge == ir::Edge::Negative ? 0u : 1u;
                ir::Eval       ev(module, ir, t.signal, asserted);
                Walker         pw(static_cast<uint32_t>(k), guards, ignored, &ev, fns);
                residues.push_back(pw.run(ir));
            }
        }

        for (SignalStores& e : pr.signals) {
            if (!pr.synthesizable) {
                e.kind = StorageKind::Unclassified;
                continue;
            }
            if (triggers.empty()) {
                e.kind                  = e.has_hold ? StorageKind::Latch : StorageKind::Comb;
                e.decomp.always_enabled = !e.has_hold;
                // A level-sensitive signal has no clock and no async list; its
                // whole description is the value it takes, split the same way.
                for (const GuardedStore& st : e.stores) {
                    (is_constant(ir, st.value) ? e.decomp.sync : e.decomp.next).push_back({ st.guard, st.value });
                }
                continue;
            }

            // Exactly one trigger must be free of the guards, or the clock is
            // ambiguous -- the rejection `proc_dff` also makes.
            std::vector<ir::WaitEvent> free_triggers;
            for (const ir::WaitEvent& t : triggers) {
                if (!trigger_in_guards(ir, guards, e, t.signal)) {
                    free_triggers.push_back(t);
                }
            }
            if (free_triggers.size() != 1) {
                e.kind = StorageKind::Unclassified;
                result.notes.push_back(
                  { Finding::AmbiguousClock, static_cast<uint32_t>(k), e.signal, proc.span, ir::Inst::Tag::Constant });
                continue;
            }
            e.kind                  = StorageKind::Flop;
            e.clock                 = free_triggers[0].signal;
            e.decomp.clock          = free_triggers[0].signal;
            e.decomp.clock_edge     = free_triggers[0].edge;
            e.decomp.always_enabled = !e.has_hold;

            for (size_t ti = 0; ti < triggers.size(); ++ti) {
                if (triggers[ti].signal == e.clock) {
                    continue;
                }
                for (const SignalStores& rs : residues[ti].signals) {
                    if (rs.signal != e.signal || rs.stores.empty()) {
                        continue;
                    }
                    // Every surviving store constant, and the signal assigned on
                    // every path: an async reset. Non-constant: an async load.
                    bool all_const = true;
                    bool all_self  = true;
                    for (const GuardedStore& st : rs.stores) {
                        all_const = all_const && is_constant(ir, st.value);
                        all_self  = all_self && is_self(ir, st.value, rs.signal);
                    }
                    if (all_self) {
                        continue; // the edge does not change this signal
                    }
                    if (rs.has_hold) {
                        continue; // the trigger does not settle this signal
                    }
                    if (all_const) {
                        e.async_reset = true;
                    } else {
                        e.kind = StorageKind::AsyncLoad;
                        result.notes.push_back({ Finding::AsyncLoad,
                                                 static_cast<uint32_t>(k),
                                                 e.signal,
                                                 proc.span,
                                                 ir::Inst::Tag::Constant });
                    }
                }
            }

            // The async groups: the residue, verbatim. Recorded for every non-clock
            // trigger that assigns this signal, including the ones that turned out
            // to be loads rather than resets -- the description is the product, and
            // whether one primitive holds it is the target tool's problem.
            for (size_t ti = 0; ti < triggers.size(); ++ti) {
                if (triggers[ti].signal == e.clock) {
                    continue;
                }
                AsyncGroup g{ triggers[ti].signal, triggers[ti].edge, {} };
                for (const SignalStores& rs : residues[ti].signals) {
                    if (rs.signal != e.signal) {
                        continue;
                    }
                    for (const GuardedStore& st : rs.stores) {
                        g.entries.push_back({ st.guard, st.value });
                    }
                }
                if (!g.entries.empty()) {
                    e.decomp.async.push_back(std::move(g));
                }
            }

            // The synchronous body is what runs with EVERY async trigger
            // deasserted. One pin cannot express two of them, which is why
            // ir::Eval grew a multi-pin form.
            //
            // Memoised on the clock, NOT computed per signal: the pin set depends
            // only on which trigger is the clock, and a process almost always has
            // one. Doing this per signal is the same O(S x T) re-walk that costs
            // yosys's proc_mux 18.3 s -- it cost hazard3 2.4 ms here before the
            // measurement caught it, against 0.95 ms memoised.
            const SignalStores* sync_for = nullptr;
            if (triggers.size() == 1) {
                // The only trigger is the clock: the base walk IS the sync body.
                for (const SignalStores& b : pr.signals) {
                    if (b.signal == e.signal) {
                        sync_for = &b;
                    }
                }
            } else {
                const auto key = static_cast<uint32_t>(e.clock);
                auto       hit = sync_by_clock.find(key);
                if (hit == sync_by_clock.end()) {
                    std::vector<ir::Pin> off;
                    for (const ir::WaitEvent& t : triggers) {
                        if (t.signal != e.clock) {
                            off.push_back({ t.signal, t.edge == ir::Edge::Negative ? 1u : 0u });
                        }
                    }
                    std::vector<Note> ignored;
                    ir::Eval          ev(module, ir, off);
                    Walker            pw(static_cast<uint32_t>(k), guards, ignored, &ev, fns);
                    hit = sync_by_clock.emplace(key, pw.run(ir)).first;
                }
                for (const SignalStores& b : hit->second.signals) {
                    if (b.signal == e.signal) {
                        sync_for = &b;
                    }
                }
            }
            if (sync_for != nullptr) {
                for (const GuardedStore& st : sync_for->stores) {
                    (is_constant(ir, st.value) ? e.decomp.sync : e.decomp.next).push_back({ st.guard, st.value });
                }
            }
        }

        if (!options.keep_guards) {
            guards.resize(arena_base);
        }
        result.processes.push_back(std::move(pr));
    }
    if (options.keep_guards) {
        result.guards = std::move(guards);
    }

    return result;
}

} // namespace clotho::legalize
