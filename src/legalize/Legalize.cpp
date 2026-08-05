#include "Legalize.h"

#include <algorithm>

namespace clotho::legalize {

namespace {

// Per-process state for one walk. `guards_` grows monotonically and is handed to
// the Result only when the caller asked to keep it.
class Walker {
public:
    Walker(uint32_t proc_index, std::vector<GuardNode>& guards, std::vector<Note>& notes)
      : proc_(proc_index)
      , guards_(guards)
      , notes_(notes) {}

    ProcessResult run(const ir::Ir& ir) {
        walk(ir, ir.root(), GuardId::None);
        // Held == assigned somewhere, but not on every path.
        const SigSet always = must_assign(ir, ir.root());
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
    static SigSet must_assign(const ir::Ir& ir, std::span<const ir::Index> body) {
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
                    const auto   r = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
                    const SigSet t = must_assign(ir, ir.region(r.then_));
                    for (const uint32_t s : must_assign(ir, ir.region(r.else_))) {
                        if (t.contains(s)) {
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
                    SigSet common = must_assign(ir, dflt);
                    for (const ir::SwitchArm& a : arms_of(ir, info)) {
                        const SigSet mine = must_assign(ir, ir.region(a.body));
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
    ProcessResult           result_{};
};

} // namespace

Result run(const ir::Module& module, const ir::DesignFacts* /*facts*/, Options options) {
    Result result;
    result.processes.reserve(module.processes().size());

    std::vector<GuardNode> guards;
    for (size_t k = 0; k < module.processes().size(); ++k) {
        Walker w(static_cast<uint32_t>(k), guards, result.notes);
        result.processes.push_back(w.run(module.processes()[k].ir));
    }
    if (options.keep_guards) {
        result.guards = std::move(guards);
    }

    return result;
}

} // namespace clotho::legalize
