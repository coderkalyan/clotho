#include "Legalize.h"

#include <algorithm>

#include "ir/Eval.h"

namespace clotho::legalize {

namespace {

// How a region leaves its enclosing loop iteration.
//
// Deliberately does NOT descend into a nested Loop: a `break` in there belongs to
// that loop, not this one.
enum class Jumps : uint8_t {
    // No jump: control reaches the end.
    None,
    // Every path jumps, so nothing after this region runs.
    Always,
    // Some path jumps. Turning this into a guard term needs sv2v's jump variable,
    // which is state; reported rather than approximated.
    Maybe,
};

std::span<const ir::SwitchArm> arms_of(const ir::Ir& ir, const ir::SwitchInfo& info);
Jumps                          region_jumps(const ir::Ir& ir, std::span<const ir::Index> body);

Jumps combine_arms(Jumps a, Jumps b) {
    if (a == Jumps::Always && b == Jumps::Always) {
        return Jumps::Always;
    }
    if (a == Jumps::None && b == Jumps::None) {
        return Jumps::None;
    }

    return Jumps::Maybe;
}

Jumps region_jumps(const ir::Ir& ir, std::span<const ir::Index> body) {
    Jumps out = Jumps::None;
    for (const ir::Index i : body) {
        switch (ir.tag(i)) {
            case ir::Inst::Tag::Break:
            case ir::Inst::Tag::Continue:
                return Jumps::Always; // the rest of this region is unreachable
            case ir::Inst::Tag::Cond: {
                const auto  r = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
                const Jumps both =
                  combine_arms(region_jumps(ir, ir.region(r.then_)), region_jumps(ir, ir.region(r.else_)));
                if (both == Jumps::Always) {
                    return Jumps::Always;
                }
                if (both == Jumps::Maybe) {
                    out = Jumps::Maybe;
                }
                break;
            }
            case ir::Inst::Tag::Switch: {
                const auto info = ir.extra_data<ir::SwitchInfo>(ir.payload(i).op_extra.extra);
                Jumps      acc  = region_jumps(ir, ir.region(info.default_));
                for (const ir::SwitchArm& a : arms_of(ir, info)) {
                    acc = combine_arms(acc, region_jumps(ir, ir.region(a.body)));
                }
                if (acc == Jumps::Always) {
                    return Jumps::Always;
                }
                if (acc == Jumps::Maybe) {
                    out = Jumps::Maybe;
                }
                break;
            }
            default:
                break; // a nested Loop's jumps are its own
        }
    }

    return out;
}

// Is a defaultless case FULL -- do its arms cover every selector value?
//
// Counting, not materialising. yosys's BitPatternPool is exponential in wildcard
// density and OOMs at W=32 with two open bugs since 2022; direct enumeration has a
// hard ceiling. Neither is needed here, because a `Switch` only ever carries
// fully-defined, deduplicated keys -- the frontend routes casez / casex / unknown
// keys down the cond-chain path instead (find_switch_run rejects hasUnknown). So
// there are no wildcards to expand and no overlaps to subtract: fullness is
// `distinct keys == 2^width`, an O(arms) count with no allocation, exact at any
// width the frontend can state.
//
// Uses the selector's VALUE width, not the key encoding: a 4-state key packs value
// and unknown bits into 2*width, which is not the space fullness asks about.
//
// Declines rather than guesses when the width is unknown (0) or 64 -- 2^64 does not
// fit, and no case enumerates it anyway.
bool case_is_full(const ir::Ir& ir, const ir::SwitchInfo& info, std::span<const ir::SwitchArm> arms) {
    if (info.sel_width == 0 || info.sel_width >= 64) {
        return false;
    }
    uint64_t count = 0;
    for (const ir::SwitchArm& a : arms) {
        // Two u32 words per key.
        count += ir.region(a.keys).size() / 2;
        if (count > (uint64_t{ 1 } << info.sel_width)) {
            break;
        }
    }

    return count == (uint64_t{ 1 } << info.sel_width);
}

// The arms of a switch, as a view over the flat extra run.
//
// A span, not a vector: coverage made must_assign descend into defaultless cases
// that used to short-circuit, so this is now called on every arm of every case on
// every walk, and a heap allocation per call showed up immediately on hazard3. The
// records are contiguous by construction (Builder::switch_ lays keys first, then
// the record run), so no copy is needed.
std::span<const ir::SwitchArm> arms_of(const ir::Ir& ir, const ir::SwitchInfo& info) {
    const std::span<const ir::Index> raw    = ir.region(info.arms);
    constexpr size_t                 stride = sizeof(ir::SwitchArm) / sizeof(uint32_t);
    const auto*                      base   = reinterpret_cast<const ir::SwitchArm*>(raw.data());

    return { base, raw.size() / stride };
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
    Walker(const ir::Module&       module,
           uint32_t                proc_index,
           std::vector<GuardNode>& guards,
           std::vector<Note>&      notes,
           ir::Eval*               eval,
           FnLegality&             fns,
           bool                    honour_full,
           bool                    honour_parallel)
      : module_(module)
      , proc_(proc_index)
      , guards_(guards)
      , notes_(notes)
      , eval_(eval)
      , fns_(fns)
      , honour_full_(honour_full)
      , honour_parallel_(honour_parallel) {}

    ProcessResult run(const ir::Ir& ir) {
        walk(ir, ir.root(), GuardId::None);
        // Held == assigned somewhere, but not on every path.
        const SigSet always = must_assign(ir, ir.root(), eval_, honour_full_);
        for (SignalStores& s : result_.signals) {
            s.has_hold = !always.contains(static_cast<uint32_t>(s.signal));
        }
        std::sort(result_.signals.begin(), result_.signals.end(), [](const SignalStores& a, const SignalStores& b) {
            return a.signal < b.signal;
        });

        return std::move(result_);
    }

private:
    // The parallel claim, only when the caller asked for it to be honoured.
    [[nodiscard]] uint32_t options_parallel(const ir::SwitchInfo& info) const {
        return honour_parallel_ && (info.pragmas & 2u) != 0 ? 1u : 0u;
    }

    // Both arms of a `cond`, with the pin folding one away when it resolves.
    // Factored out because the jump path above walks the same statement before
    // narrowing the guard for whatever follows it.
    void walk_cond(const ir::Ir& ir, ir::Index i, GuardId guard) {
        const auto                       r         = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
        const ir::Index                  pred      = ir.payload(i).op_extra.op;
        const std::span<const ir::Index> then_body = ir.region(r.then_);
        const std::span<const ir::Index> else_body = ir.region(r.else_);

        if (eval_ != nullptr) {
            const ir::Tri t = eval_->truth(pred);
            if (t == ir::Tri::True) {
                walk(ir, then_body, guard);

                return;
            }
            if (t == ir::Tri::False) {
                walk(ir, else_body, guard);

                return;
            }
        }
        walk(ir, then_body, push(guard, { pred, AtomKind::CondTrue, {} }));
        walk(ir, else_body, push(guard, { pred, AtomKind::CondFalse, {} }));
    }

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
    static SigSet must_assign(const ir::Ir& ir, std::span<const ir::Index> body, ir::Eval* eval, bool honour_full) {
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
                            const SigSet only = must_assign(ir, ir.region(r.then_), eval, honour_full);
                            out.insert(only.begin(), only.end());
                            break;
                        }
                        if (t == ir::Tri::False) {
                            const SigSet only = must_assign(ir, ir.region(r.else_), eval, honour_full);
                            out.insert(only.begin(), only.end());
                            break;
                        }
                    }
                    const SigSet t2 = must_assign(ir, ir.region(r.then_), eval, honour_full);
                    for (const uint32_t s : must_assign(ir, ir.region(r.else_), eval, honour_full)) {
                        if (t2.contains(s)) {
                            out.insert(s);
                        }
                    }
                    break;
                }
                case ir::Inst::Tag::Switch: {
                    const auto                       info = ir.extra_data<ir::SwitchInfo>(ir.payload(i).op_extra.extra);
                    const std::span<const ir::Index> dflt = ir.region(info.default_);
                    const std::span<const ir::SwitchArm> all = arms_of(ir, info);
                    // A defaultless case whose arms cover the selector is complete:
                    // the "missing default" path does not exist. Without this every
                    // fully-enumerated case reads as a latch.
                    // Complete when the arms cover the selector, or when the
                    // designer declared the case full and the caller asked for that
                    // reading. The base slot is the analysis's to fill, which is
                    // exactly why the pragma can still take effect here -- yosys
                    // cannot do this in its legalizer because its lowering has
                    // already committed to a hold (§8).
                    // `full_case` says no other selector value occurs, so the
                    // default region is UNREACHABLE and is excluded from the
                    // intersection whether or not it exists. That matters for
                    // `unique case`, whose default is not empty: it holds the
                    // violation report the keyword implies, which assigns nothing
                    // and would otherwise read as a hold.
                    const bool declared_full = honour_full && (info.pragmas & 1u) != 0;
                    if (dflt.empty() && !declared_full && !case_is_full(ir, info, all)) {
                        break; // no default, not full: a path that assigns nothing
                    }
                    SigSet common;
                    bool   first = true;
                    if (!dflt.empty() && !declared_full) {
                        common = must_assign(ir, dflt, eval, honour_full);
                        first  = false;
                    }
                    for (const ir::SwitchArm& a : all) {
                        SigSet mine = must_assign(ir, ir.region(a.body), eval, honour_full);
                        if (first) {
                            common = std::move(mine);
                            first  = false;
                            continue;
                        }
                        SigSet next;
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

    // `guard` is taken by value and NARROWS as the body is walked: a jump makes
    // everything after it conditional on not having jumped. That is sv2v's jump
    // variable expressed as a guard term, which is all it ever was for the shapes
    // that do not need real state -- §5.3 predicted exactly this.
    void walk(const ir::Ir& ir, std::span<const ir::Index> body, GuardId guard) {
        for (const ir::Index i : body) {
            const ir::Inst::Tag tag = ir.tag(i);
            // A bare jump: nothing after it in this body runs, at all.
            if (tag == ir::Inst::Tag::Break || tag == ir::Inst::Tag::Continue) {
                return;
            }
            // `if (p) break;` -- everything after is guarded by !p. The mirrored
            // form guards by p. Both arms jumping kills the rest outright. Anything
            // subtler needs a jump variable, which is state this analysis will not
            // invent, so it is reported rather than under-guarded.
            if (tag == ir::Inst::Tag::Cond) {
                const auto  r  = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
                const Jumps jt = region_jumps(ir, ir.region(r.then_));
                const Jumps je = region_jumps(ir, ir.region(r.else_));
                if (jt != Jumps::None || je != Jumps::None) {
                    const ir::Index pred = ir.payload(i).op_extra.op;
                    walk_cond(ir, i, guard);
                    if (jt == Jumps::Always && je == Jumps::Always) {
                        return;
                    }
                    if (jt == Jumps::Always && je == Jumps::None) {
                        guard = push(guard, { pred, AtomKind::CondFalse, {}, 0, 0 });
                        continue;
                    }
                    if (je == Jumps::Always && jt == Jumps::None) {
                        guard = push(guard, { pred, AtomKind::CondTrue, {}, 0, 0 });
                        continue;
                    }
                    reject(Finding::UnmodelledJump, tag, {});

                    return;
                }
            }
            // A signal whose TYPE has no hardware form makes the process
            // unsynthesizable however it is touched. The tag gate below cannot see
            // it: Signal / Load / Store are ordinary opcodes, and it is the type
            // that has no meaning. Without this, `q[0] = x` over a queue passed
            // legalization (ElemSelect is a perfectly good hardware select) and the
            // writer emitted a bit-select of a 1-bit `logic q`.
            if (tag == ir::Inst::Tag::Signal &&
                unsynthesizable_type(module_.pool, module_.signal(ir.payload(i).signal).type) != nullptr) {
                reject(Finding::UnsynthesizableOp, tag, {});
                continue;
            }
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

                case ir::Inst::Tag::Cond:
                    walk_cond(ir, i, guard);
                    break;

                case ir::Inst::Tag::Switch: {
                    const auto      info = ir.extra_data<ir::SwitchInfo>(ir.payload(i).op_extra.extra);
                    const ir::Index sel  = ir.payload(i).op_extra.op;
                    const std::span<const ir::SwitchArm> arms = arms_of(ir, info);
                    const std::span<const ir::Index>     dflt = ir.region(info.default_);

                    // Complete for the synthesiser, a latch for the simulator, from
                    // one source. Reported here rather than silently resolved either
                    // way, because which reading is right depends on whether the
                    // selector can actually be x -- which this analysis cannot know
                    // and the designer can.
                    // Reported when the claim is what removes the hold: the arms do
                    // not cover the selector and the default assigns nothing. A
                    // `priority case` with a real default is complete anyway, and
                    // noting it would be noise.
                    if (honour_full_ && (info.pragmas & 1u) != 0 && !case_is_full(ir, info, arms) &&
                        must_assign(ir, dflt, nullptr, honour_full_).empty()) {
                        notes_.push_back(
                          { Finding::FullCasePragmaUnsound, proc_, ir::Signal{}, {}, ir::Inst::Tag::Switch });
                    }
                    if (dflt.empty() && info.sel_four != 0 && case_is_full(ir, info, arms)) {
                        notes_.push_back(
                          { Finding::FullCaseXSelector, proc_, ir::Signal{}, {}, ir::Inst::Tag::Switch });
                    }

                    for (const ir::SwitchArm& a : arms) {
                        walk(ir,
                             ir.region(a.body),
                             push(guard, { sel, AtomKind::SwitchArm, a.keys, info.sel_width, options_parallel(info) }));
                    }
                    walk(ir,
                         dflt,
                         push(guard, { sel, AtomKind::SwitchDefault, {}, info.sel_width, options_parallel(info) }));
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

    const ir::Module&       module_;
    uint32_t                proc_;
    std::vector<GuardNode>& guards_;
    std::vector<Note>&      notes_;
    bool                    honour_full_     = false;
    bool                    honour_parallel_ = false;
    bool                    in_loop_         = false;
    ir::Eval*               eval_            = nullptr;
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

const char* unsynthesizable_type(const ir::InternPool& pool, ir::Ref type) {
    ir::Ref leaf = type;
    while (std::get<ir::Type>(pool.get(leaf)).kind == ir::Type::Kind::UnpackedArray) {
        leaf = std::get<ir::Type>(pool.get(leaf)).element;
    }
    const ir::Type& t = std::get<ir::Type>(pool.get(leaf));
    switch (t.kind) {
        case ir::Type::Kind::String:
            return "string";
        case ir::Type::Kind::Queue:
            return "queue";
        case ir::Type::Kind::DynArray:
            return "dynamic array";
        case ir::Type::Kind::Assoc:
            return "associative array";
        case ir::Type::Kind::Class:
            return "class handle";
        case ir::Type::Kind::Real:
        case ir::Type::Kind::ShortReal:
            return "real";
        case ir::Type::Kind::Struct:
            return t.is_packed ? nullptr : "unpacked struct";
        default:
            return nullptr;
    }
}

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
        Walker                           w(module,
                                           static_cast<uint32_t>(k),
                                           guards,
                                           result.notes,
                                           nullptr,
                                           fns,
                                           options.honour_full_case,
                                           options.honour_parallel_case);
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
                Walker         pw(module,
                                  static_cast<uint32_t>(k),
                                  guards,
                                  ignored,
                                  &ev,
                                  fns,
                                  options.honour_full_case,
                                  options.honour_parallel_case);
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
                    Walker            pw(module,
                                         static_cast<uint32_t>(k),
                                         guards,
                                         ignored,
                                         &ev,
                                         fns,
                                         options.honour_full_case,
                                         options.honour_parallel_case);
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
