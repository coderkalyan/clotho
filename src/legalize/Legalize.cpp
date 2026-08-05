#include "Legalize.h"

#include <algorithm>

#include "ir/Eval.h"

namespace clotho::legalize {

namespace {

// Per-process state for one walk. `guards_` grows monotonically and is handed to
// the Result only when the caller asked to keep it.
class Walker {
public:
    // `eval` is null for the base walk. When present, a `cond` whose predicate
    // folds under the pin descends only into the taken arm -- which is exactly
    // §4A.5's "pin the trigger and fold", and it makes must_assign report
    // completeness in the PINNED world rather than the general one.
    Walker(uint32_t proc_index, std::vector<GuardNode>& guards, std::vector<Note>& notes, ir::Eval* eval)
      : proc_(proc_index)
      , guards_(guards)
      , notes_(notes)
      , eval_(eval) {}

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

    // The arms of a switch, decoded from the flat extra run.
    static std::vector<ir::SwitchArm> arms_of(const ir::Ir& ir, const ir::SwitchInfo& info) {
        const std::span<const ir::Index> raw    = ir.region(info.arms);
        constexpr size_t                 stride = sizeof(ir::SwitchArm) / sizeof(uint32_t);
        const auto*                      base   = reinterpret_cast<const ir::SwitchArm*>(raw.data());

        return { base, base + raw.size() / stride };
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
                    slot(s).stores.push_back({ guard, ir.payload(i).binary.r, i, tag == ir::Inst::Tag::StoreNb });
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
                    walk(ir, ir.region(info.cond), guard);
                    walk(ir, ir.region(info.body), guard);
                    walk(ir, ir.region(info.step), guard);
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
    ir::Eval*               eval_ = nullptr;
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

    std::vector<GuardNode> guards;
    for (size_t k = 0; k < module.processes().size(); ++k) {
        const ir::Process& proc = module.processes()[k];
        const ir::Ir&      ir   = proc.ir;

        // Guards are always built: classification needs them to find the clock.
        // They are only HANDED BACK when the caller asked.
        const size_t                     arena_base = guards.size();
        Walker                           w(static_cast<uint32_t>(k), guards, result.notes, nullptr);
        ProcessResult                    pr       = w.run(ir);
        const std::vector<ir::WaitEvent> triggers = edge_triggers(proc);

        for (SignalStores& e : pr.signals) {
            if (!pr.synthesizable) {
                e.kind = StorageKind::Unclassified;
                continue;
            }
            if (triggers.empty()) {
                e.kind = e.has_hold ? StorageKind::Latch : StorageKind::Comb;
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
            e.kind  = StorageKind::Flop;
            e.clock = free_triggers[0].signal;

            // Pin each non-clock trigger asserted and re-walk. A posedge pins 1, a
            // negedge pins 0 -- that is the whole of polarity handling, and a
            // mismatched condition simply folds the other way (§4A.1).
            for (const ir::WaitEvent& t : triggers) {
                if (t.signal == e.clock) {
                    continue;
                }
                const uint64_t         asserted = t.edge == ir::Edge::Negative ? 0u : 1u;
                ir::Eval               ev(module, ir, t.signal, asserted);
                std::vector<GuardNode> scratch;
                std::vector<Note>      ignored;
                Walker                 pw(static_cast<uint32_t>(k), scratch, ignored, &ev);
                const ProcessResult    residue = pw.run(ir);

                for (const SignalStores& rs : residue.signals) {
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
                    }
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
