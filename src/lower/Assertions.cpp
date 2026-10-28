// Immediate, deferred and concurrent assertion front end, assertion control, and Preponed sampling.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Immediate assertion (LRM 16.3): assert / assume / cover. A true condition
// runs the optional pass action. `assert`/`assume` (which check identically
// for simulation, LRM 16.4.3) additionally run the `else` action on a false
// condition, or emit the default failure report (error severity) when there
// is none. `cover` has no fail action — a false condition does nothing; its
// coverage count/report is deliberately unimplemented (docs/LRM_NOTES.md, note 21).
// Emit one deferred assertion action (LRM 16.4): a single severity system
// task ($error / $warning / $info), or the default $error when `action` is
// null on a failing assert/assume. The report is rendered as a Severity node
// with `defer` set (1 = observed `#0`, 2 = final), so the backend queues it
// for the Observed / Postponed region; its arguments are lowered here and so
// sampled at the Active-region execution point (LRM 16.4.1). Action shapes
// Phase 1 does not defer are rejected loudly.
void Lowerer::emit_deferred_action(const Statement*      action,
                                   uint32_t              defer_mode,
                                   bool                  is_fail,
                                   slang::SourceLocation site,
                                   ir::Builder&          b) {
    // A bare `assert #0 (e)` carries an empty action for the pass branch; a
    // missing branch is null. Either way there is no explicit action.
    const bool no_action = action == nullptr || action->kind == slang::ast::StatementKind::Empty;
    if (no_action) {
        // Only the fail branch of assert/assume has a default report; a
        // missing pass action (or cover) emits nothing.
        if (is_fail) {
            b.severity(static_cast<uint32_t>(ir::Severity::Error),
                       severity_banner(site),
                       "assertion failed",
                       {},
                       to_source_loc(site),
                       defer_mode);
        }
        return;
    }
    const auto* es   = action->as_if<slang::ast::ExpressionStatement>();
    const auto* call = es != nullptr ? es->expr.as_if<CallExpression>() : nullptr;
    if (call == nullptr || !call->isSystemCall()) {
        // TODO: deferred action blocks that are a user task / void-function
        // call (LRM 16.4 allows any single subroutine) need a generic
        // deferred-call ABI (sample args now, invoke at maturation).
        fail("deferred assertion action block must be a single severity system "
             "task ($error / $warning / $info) — Phase 1");
    }
    const std::string_view nm = call->getSubroutineName();
    ir::Severity           sev;
    if (nm == "$error") {
        sev = ir::Severity::Error;
    } else if (nm == "$warning") {
        sev = ir::Severity::Warning;
    } else if (nm == "$info") {
        sev = ir::Severity::Info;
    } else {
        // TODO: $fatal (terminates — needs the deferred $finish), and
        // $display / $write (stdout, no label) as deferred actions.
        fail("deferred assertion action '" + std::string(nm) +
             "' is not supported (only $error / $warning / $info) — Phase 1");
    }
    std::string            fmt;
    std::vector<ir::Index> args;
    build_format(call->arguments(), fmt, args, b);
    b.severity(static_cast<uint32_t>(sev), severity_banner(call->sourceRange.start()), fmt, args, loc(*call),
               defer_mode);
}

// Deferred immediate assertion (LRM 16.4): `assert/assume/cover #0` (observed,
// matures in the Observed region) or `... final` (matures in the Postponed
// region). The expression is evaluated here (Active region, as for a simple
// immediate assertion); only the action is deferred. The backend's per-process
// generation flush gives the glitch suppression (LRM 16.4.2). Phase 1 restricts
// the action to a severity report and requires the assertion to sit directly in
// a process body (a deferred assertion inside a function/task needs the caller
// process's generation threaded through the call ABI).
void Lowerer::lower_deferred_assertion(const ImmediateAssertionStatement& a, ir::Builder& b) {
    using AK = slang::ast::AssertionKind;
    if (fn_ctx_ != nullptr) {
        // TODO: thread the calling process's deferred-assertion generation
        // through the function/task ABI (LRM 16.4.1 short-circuit example).
        fail("deferred assertion inside a function / task is not supported — Phase 1");
    }
    const bool is_cover = a.assertionKind == AK::CoverProperty;
    if (a.assertionKind != AK::Assert && a.assertionKind != AK::Assume && !is_cover) {
        fail("only deferred assert / assume / cover are supported");
    }
    const uint32_t  defer_mode = a.isFinal ? 2u : 1u;
    const ir::Index pred       = lower_predicate(a.cond, b);

    b.open_region();
    emit_deferred_action(a.ifTrue, defer_mode, /*is_fail=*/false, a.sourceRange.start(), b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    if (!is_cover) {
        emit_deferred_action(a.ifFalse, defer_mode, /*is_fail=*/true, a.sourceRange.start(), b);
    }
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, loc(a));
}

// The event inside `local` that came from the syntax node `want`, or null if
// there is none. `local` is the event control of the procedure being lowered,
// so the result is an AST node of THIS instance -- which is the whole reason
// the inference verdict travels as syntax (see InferredClockMap).
//
// An event list is searched one level down: `always @(posedge clk or negedge
// rst_n)` is the ordinary reset-flop shape, and 16.14.6 infers the one event
// whose terms the body does not otherwise read -- usually `clk`, because the
// body tests `rst_n`. There is no deeper nesting to search: an event list
// cannot contain another.
const TimingControl* Lowerer::local_event_with_syntax(const TimingControl*             local,
                                                      const slang::syntax::SyntaxNode* want) {
    if (local == nullptr || want == nullptr) {
        return nullptr;
    }
    if (local->syntax == want) {
        return local;
    }
    if (local->kind == TimingControlKind::EventList) {
        for (const TimingControl* event : local->as<slang::ast::EventListControl>().events) {
            if (event->syntax == want) {
                return event;
            }
        }
    }
    return nullptr;
}

// The first clocking event written *inside* a property or sequence body
// (LRM 16.13.1, clock flow). A declaration may carry its own clock --
//
//     sequence s; @(posedge clk) a ##1 b; endsequence
//     assert property (s);
//
// -- which puts the only clocking event the assertion has below the
// AssertionInstanceExpression, where peeling the top of `propertySpec`
// cannot see it.
//
// Hoisting one event out of the body is sound only because sva::lower
// compares every Clocking node it walks against the event chosen here and
// refuses any that is not structurally equivalent to it (Sva.cpp,
// AssertionExprKind::Clocking). Two consequences follow. A body carrying two
// different events still lands on the multiclock rejection, whichever of
// them this picks. And a body carrying one event is uniformly clocked by it,
// because every position with no event of its own inherits one from its
// context, and the only two candidates -- this event and the assertion's --
// are then the same event. The same sequence instantiated twice is one event
// for the same reason: the comparison asks for equivalence, not identity.
//
// The walk descends exactly where sva::lower descends, and no further.
// Over-collecting is the dangerous direction: an event lifted out of a
// subtree the lowering never walks would clock the assertion with nothing
// downstream left to check it against.
const TimingControl* Lowerer::body_clock(const AssertionExpr& expr) {
    using AEK = slang::ast::AssertionExprKind;
    switch (expr.kind) {
        case AEK::Clocking:
            return &expr.as<ClockingAssertionExpr>().clocking;
        case AEK::Simple: {
            // A named sequence or property use; slang hands back a `body`
            // that already has the actual arguments substituted.
            const auto& simple = expr.as<SimpleAssertionExpr>();
            if (simple.expr.kind != ExpressionKind::AssertionInstance) {
                return nullptr;
            }
            return body_clock(simple.expr.as<AssertionInstanceExpression>().body);
        }
        case AEK::SequenceConcat: {
            for (const auto& element : expr.as<SequenceConcatExpr>().elements) {
                if (const TimingControl* clocking = body_clock(*element.sequence)) {
                    return clocking;
                }
            }
            return nullptr;
        }
        case AEK::SequenceWithMatch:
            return body_clock(expr.as<SequenceWithMatchExpr>().expr);
        case AEK::Binary: {
            const auto&          binary = expr.as<BinaryAssertionExpr>();
            const TimingControl* left   = body_clock(binary.left);
            return left != nullptr ? left : body_clock(binary.right);
        }
        case AEK::FirstMatch:
            return body_clock(expr.as<FirstMatchAssertionExpr>().seq);
        case AEK::StrongWeak:
            return body_clock(expr.as<StrongWeakAssertionExpr>().expr);
        case AEK::DisableIff:
            return body_clock(expr.as<DisableIffAssertionExpr>().expr);
        default:
            // Unary, Abort, Conditional, Case and Invalid: sva::lower refuses
            // each of them without walking an operand, so a clocking event
            // underneath one could never be checked against the choice made
            // here. Not descending keeps that impossible rather than unlikely.
            return nullptr;
    }
}

// The `disable iff` condition to hoist out of the body, or null.
//
// LRM 16.12 attaches the clause to a property_expr to yield a property_spec,
// and a named property's declaration body IS a property_spec -- so `property
// p; @(posedge clk) disable iff (rst) a |=> b; endproperty` carries a reset
// that belongs to every assertion instantiating p. slang substitutes the
// actuals into the body it hands back, so the condition arrives already in the
// instantiation's terms and can be lifted verbatim.
//
// The soundness constraint is body_clock's, and so is the shape of the walk:
// descend exactly where sva::lower descends, and no further. Over-collecting
// is again the dangerous direction, and here it is worse than a missed
// cross-check -- a `disable iff` under `and` or `until` would reset ONE
// operand, and lifting it out would silently widen it to the whole assertion.
// Not descending leaves that node where it is for sva::lower's DisableIff arm,
// which refuses every clause it was not told about.
//
// Three kinds are walked and each is a step sva::lower's property() takes: a
// Simple that is a property instance, into the substituted body; the Clocking
// wrapper the declaration may carry above the clause; and the clause itself.
// That is also the shape slang polices with diag::NestedDisableIff
// (SimpleAssertionExpr::fromSyntax), so a clause reached this way is one slang
// has already ruled legal where it stands.
const Expression* Lowerer::body_disable(const AssertionExpr& expr) {
    using AEK = slang::ast::AssertionExprKind;
    switch (expr.kind) {
        case AEK::DisableIff:
            return &expr.as<DisableIffAssertionExpr>().condition;
        case AEK::Clocking:
            return body_disable(expr.as<ClockingAssertionExpr>().expr);
        case AEK::Simple: {
            const auto& simple = expr.as<SimpleAssertionExpr>();
            if (simple.expr.kind != ExpressionKind::AssertionInstance) {
                return nullptr;
            }
            return body_disable(simple.expr.as<AssertionInstanceExpression>().body);
        }
        default:
            return nullptr;
    }
}

// The control code one of the nine named tasks stands for (LRM 20.12's
// equivalence table), or 0 for a name that is not one of them.
int32_t Lowerer::named_control_code(std::string_view name) {
    if (name == "$asserton") {
        return static_cast<int32_t>(AssertCtl::On);
    }
    if (name == "$assertoff") {
        return static_cast<int32_t>(AssertCtl::Off);
    }
    if (name == "$assertkill") {
        return static_cast<int32_t>(AssertCtl::Kill);
    }
    if (name == "$assertpasson") {
        return static_cast<int32_t>(AssertCtl::PassOn);
    }
    if (name == "$assertpassoff") {
        return static_cast<int32_t>(AssertCtl::PassOff);
    }
    if (name == "$assertfailon") {
        return static_cast<int32_t>(AssertCtl::FailOn);
    }
    if (name == "$assertfailoff") {
        return static_cast<int32_t>(AssertCtl::FailOff);
    }
    if (name == "$assertnonvacuouson") {
        return static_cast<int32_t>(AssertCtl::NonvacuousOn);
    }
    if (name == "$assertvacuousoff") {
        return static_cast<int32_t>(AssertCtl::VacuousOff);
    }

    return 0;
}

bool Lowerer::is_assert_control_task(std::string_view name) {
    return name == "$assertcontrol" || named_control_code(name) != 0;
}

// Collect-pass scan of one procedural block: enumerate the concurrent
// assertions in it, and notice whether it calls a control task. Both
// answers must exist before Emit allocates any monitor state, and this is
// the walk that already visits every block.
//
// Calls into user subroutines are followed, because `task off(); $assertoff;
// endtask` is an ordinary way to write this and the call graph Emit lowers
// is the same one. What a static call cannot reach -- a class method
// arrived at only through a virtual dispatch -- is caught by the loud
// refusal in lower_assert_control rather than silently losing the control.
void Lowerer::scan_assert_control(const ProceduralBlockSymbol& blk) {
    struct Scan : ASTVisitor<Scan, VisitFlags::AllGood> {
        Lowerer*                                   self;
        const Symbol*                              owner = nullptr;
        const Symbol*                              label = nullptr;
        ankerl::unordered_dense::set<const Symbol*> entered;

        void handle(const slang::ast::BlockStatement& s) {
            const Symbol* saved = label;
            if (s.blockSymbol != nullptr) {
                label = s.blockSymbol;
            }
            visitDefault(s);
            label = saved;
        }
        void handle(const slang::ast::ConcurrentAssertionStatement& s) {
            self->record_assert_site(s, owner, label);
            visitDefault(s);
        }
        void handle(const ImmediateAssertionStatement& s) {
            // Only whether one EXISTS: an immediate assertion is in the
            // default assertion_type set, and lower_assert_control refuses
            // rather than silently leaving it uncontrolled.
            self->immediate_assertion_seen_ = true;
            visitDefault(s);
        }
        void handle(const CallExpression& c) {
            if (c.subroutine.index() == 0) {
                const SubroutineSymbol* sub = std::get<0>(c.subroutine);
                // `entered` is recursion protection, not memoization: a
                // subroutine reached twice has nothing new to say.
                if (sub != nullptr && entered.insert(sub).second) {
                    sub->getBody().visit(*this);
                }
            } else if (is_assert_control_task(c.getSubroutineName())) {
                self->assert_control_used_ = true;
            }
            visitDefault(c);
        }
    };
    Scan v;
    v.self  = this;
    v.owner = &blk;
    blk.getBody().visit(v);
}

// Collect: the named blocks some `disable` in this process targets, how each
// one is named (lexically from inside itself, or remotely from outside), and
// whether disabling the outermost one would have to flush a deferred queue
// (LRM 16.4.4). All of it has to be known before Emit, because a block lowers
// BEFORE the `disable` nested inside it: only a targeted block is emitted as
// an ir::NamedBlock, and whether it carries occupant bookkeeping is decided at
// the block, not at the `disable`.
//
// The scan keeps its own enclosing-block stack rather than reusing
// named_blocks_, which does not exist yet at Collect. A `disable` whose target
// is on that stack is the lexical form; anything else is remote, including a
// sibling block in this same process.
//
// Calls ARE followed, because the remote form works from anywhere: a `disable`
// inside a task does not care where it runs, it marks an occupant and pushes.
// What matters is where the TARGET lives, so a block declared inside a
// subroutine or a fork child is recorded as such — each activation of one is a
// separate occupant, which the single slot cannot name.
void Lowerer::scan_disable_targets(const ProceduralBlockSymbol& blk) {
    struct Scan : ASTVisitor<Scan, VisitFlags::AllGood> {
        // Targets named lexically (the `disable` is inside the target) and
        // remotely (it is not). A block can be in both.
        std::vector<const Symbol*> lexical;
        std::vector<const Symbol*> remote;
        // Named blocks that cannot have a single live occupant: one inside a fork
        // child, or one declared in a subroutine body (any number of activations).
        std::vector<const Symbol*> multi;
        // Every named block seen, with its body, so the enclosing Lowerer can ask
        // whether the body parks in a way an abort cannot neutralize.
        std::vector<std::pair<const Symbol*, const Statement*>> bodies;
        // A deferred assertion or a procedural concurrent assertion in this
        // process: both queue reports a disable of the outermost scope must
        // drop rather than let mature.
        bool flushes = false;

        std::vector<const Symbol*>                       open;
        uint32_t                                         fork_depth = 0;
        uint32_t                                         sub_depth  = 0;
        ankerl::unordered_dense::set<const SubroutineSymbol*> entered;

        // Follow a user subroutine once. `sub_depth` marks everything found
        // inside as multi-activation; `open` is swapped empty, because a
        // `disable` in the body can only name blocks the body itself opened.
        void handle(const CallExpression& c) {
            visitDefault(c);
            if (c.isSystemCall() || !std::holds_alternative<const SubroutineSymbol*>(c.subroutine)) {
                return;
            }
            const auto* sub = std::get<const SubroutineSymbol*>(c.subroutine);
            if (sub == nullptr || !entered.insert(sub).second) {
                return; // recursion, or already walked
            }
            std::vector<const Symbol*> saved;
            saved.swap(open);
            ++sub_depth;
            sub->getBody().visit(*this);
            --sub_depth;
            open.swap(saved);
        }
        void handle(const BlockStatement& s) {
            const bool forks  = s.blockKind != StatementBlockKind::Sequential;
            const bool named  = s.blockSymbol != nullptr && !s.blockSymbol->name.empty();
            fork_depth       += forks ? 1 : 0;
            if (named) {
                open.push_back(s.blockSymbol);
                if (fork_depth != 0 || sub_depth != 0) {
                    multi.push_back(s.blockSymbol);
                }
                bodies.emplace_back(s.blockSymbol, &s.body);
            }
            visitDefault(s);
            if (named) {
                open.pop_back();
            }
            fork_depth -= forks ? 1 : 0;
        }
        void handle(const slang::ast::DisableStatement& s) {
            const Symbol* t = &*s.target.as<slang::ast::ArbitrarySymbolExpression>().symbol;
            // Lexical only when the target encloses it AND one activation is all
            // there can be. Inside a task or a fork child there may be several
            // inside the same block at once, and LRM 9.6.2 ends all of them, so
            // that use needs the list exactly as a remote one does.
            const bool encloses = std::find(open.begin(), open.end(), t) != open.end();
            (encloses && sub_depth == 0 && fork_depth == 0 ? lexical : remote).push_back(t);
            visitDefault(s);
        }
        void handle(const ImmediateAssertionStatement& s) {
            flushes = flushes || s.isDeferred;
            visitDefault(s);
        }
        void handle(const slang::ast::ConcurrentAssertionStatement& s) {
            flushes = true;
            visitDefault(s);
        }
    };
    Scan v;
    blk.getBody().visit(v);

    // The block that IS this procedure's outermost scope, recorded per target
    // rather than per procedure: a remote `disable` reaches a target whose
    // procedure is not the one being lowered, so the 16.4.4 question has to be
    // answerable from the target alone.
    // Recorded for EVERY procedure, not only one that contains a `disable`: a
    // remote disabler asks these questions about a procedure it is not in, and
    // that procedure may hold no `disable` of its own.
    const Symbol* root = outermost_named_block(blk.getBody());
    if (root != nullptr) {
        disable_roots_.insert(root);
        if (v.flushes) {
            disable_flushes_.insert(root);
        }
    }
    for (const Symbol* t : v.multi) {
        disable_multi_.insert(t);
    }
    for (const auto& [sym, body] : v.bodies) {
        if (hard_park(*body)) {
            disable_hard_.insert(sym);
        }
    }
    for (const std::vector<const Symbol*>* set : { &v.lexical, &v.remote }) {
        for (const Symbol* t : *set) {
            const bool is_task = t->kind == SymbolKind::Subroutine;
            if (!is_task && t->kind != SymbolKind::StatementBlock) {
                continue; // slang admits nothing else as a target
            }
            // Design-unique, not per-Lowerer: a multi-top design gives every
            // top its own Lowerer sharing one Module, so the id has to come
            // from the module the way a scope id does.
            if (disable_targets_.try_emplace(t, module_.disable_targets).second) {
                ++module_.disable_targets;
                module_.disable_target_is_task.push_back(is_task ? 1 : 0);
            }
            if (is_task) {
                // A task target is always "remote" in the sense that matters: it
                // ends every activation, never just the one running. Its parks
                // are checked against the task's own body.
                const auto& sub = t->as<SubroutineSymbol>();
                disable_remote_.insert(t);
                if (hard_park(sub.getBody())) {
                    disable_hard_.insert(t);
                }
                continue;
            }
            if (set == &v.remote) {
                disable_remote_.insert(t);
            }
        }
    }
}

// Whether a block's body can park in a way an abort cannot yet neutralize, which
// is what decides whether the REMOTE form may name it. A `#delay` and an
// `@event` wait are fine: a `disable` claims the delay's park record, so the
// timer that fires later wakes nobody, and the wait compacts itself out of its
// buckets on resume. The rest are not, each for its own reason -- a `#0` parks
// in the inactive batch, which has no per-entry claim; a TIME-CONSUMING task call
// or a fork parks the process behind ANOTHER activation's completion, which
// would resume it
// a second time; an `expect` parks on a reactive verdict. Refused rather than
// silently double-woken.
bool Lowerer::hard_park(const Statement& body) {
    struct Park : ASTVisitor<Park, VisitFlags::AllGood> {
        Lowerer* self  = nullptr;
        bool     found = false;

        void handle(const BlockStatement& s) {
            found = found || s.blockKind != StatementBlockKind::Sequential;
            visitDefault(s);
        }
        void handle(const slang::ast::ConcurrentAssertionStatement& s) {
            // Only `expect` parks the executing process; the other concurrent
            // forms are their own monitor process.
            found = found || s.assertionKind == slang::ast::AssertionKind::Expect;
            visitDefault(s);
        }
        // A delay of any amount is fine. A nonzero one is a wheel timer, whose
        // park record the disabler claims. A `#0` parks in the inactive batch,
        // which has no claim -- and needs none: mid-body the scheduled flag is
        // SET, so the disabler stands down and the batch's own promotion, which
        // happens later in the same instant, delivers the single wake. The poll
        // then sees the mark. That is also why the batch never double-wakes.
        void handle(const CallExpression& c) {
            if (!c.isSystemCall() && std::holds_alternative<const SubroutineSymbol*>(c.subroutine)) {
                const auto* sub = std::get<const SubroutineSymbol*>(c.subroutine);
                // Only a call that can PARK matters. A task that consumes no time
                // runs to completion inside the call, so the process is never
                // suspended behind it and there is nothing to double-wake — which
                // is most helper tasks, and refusing them was over-broad.
                found = found || (sub != nullptr && self->subroutine_parks(*sub));
            }
            visitDefault(c);
        }
    };
    Park p;
    p.self = this;
    body.visit(p);

    return p.found;
}

// Whether calling this subroutine can suspend the caller: a timing control, a
// wait, a fork or an `expect` anywhere in its body, transitively through the
// calls it makes. Memoized, and a recursive cycle answers "no" on the way back
// in — the cycle itself parks only if some member does, which that member
// reports.
bool Lowerer::subroutine_parks(const SubroutineSymbol& sub) {
    if (const auto it = sub_parks_.find(&sub); it != sub_parks_.end()) {
        return it->second;
    }
    // Provisional false breaks recursion; overwritten below.
    sub_parks_.emplace(&sub, false);

    struct Park : ASTVisitor<Park, VisitFlags::AllGood> {
        Lowerer* self  = nullptr;
        bool     found = false;

        void handle(const slang::ast::TimedStatement& s) {
            found = true;
            visitDefault(s);
        }
        void handle(const slang::ast::WaitStatement& s) {
            found = true;
            visitDefault(s);
        }
        void handle(const slang::ast::WaitForkStatement& s) {
            found = true;
            visitDefault(s);
        }
        void handle(const BlockStatement& s) {
            found = found || s.blockKind != StatementBlockKind::Sequential;
            visitDefault(s);
        }
        void handle(const slang::ast::ConcurrentAssertionStatement& s) {
            found = found || s.assertionKind == slang::ast::AssertionKind::Expect;
            visitDefault(s);
        }
        void handle(const CallExpression& c) {
            if (!c.isSystemCall() && std::holds_alternative<const SubroutineSymbol*>(c.subroutine)) {
                const auto* callee = std::get<const SubroutineSymbol*>(c.subroutine);
                found = found || (callee != nullptr && self->subroutine_parks(*callee));
            }
            visitDefault(c);
        }
    };
    Park p;
    p.self = this;
    sub.getBody().visit(p);
    sub_parks_[&sub] = p.found;

    return p.found;
}

void Lowerer::record_assert_site(const slang::ast::ConcurrentAssertionStatement& s,
                                 const Symbol*                                   owner,
                                 const Symbol*                                   label) {
    site_of_[&s].push_back(static_cast<uint32_t>(sites_.size()));
    sites_.push_back(AssertSite{ .stmt   = &s,
                                 .scope  = cur_scope_id(),
                                 .kind   = s.assertionKind,
                                 .owner  = owner,
                                 .label  = label,
                                 .prefix = prefix_,
                                 .loc    = to_source_loc(s.sourceRange.start()) });
}

// The scope node with `id` under `root`. Walked once per assertion while
// control state is allocated, so the linear search costs nothing measurable
// and saves Collect from maintaining a parallel index as the tree grows.
ir::InstanceScope* Lowerer::find_scope_node(ir::InstanceScope& root, uint32_t id) {
    if (root.id == id) {
        return &root;
    }
    for (ir::InstanceScope& child : root.children) {
        if (ir::InstanceScope* hit = find_scope_node(child, id); hit != nullptr) {
            return hit;
        }
    }

    return nullptr;
}

// Allocate every assertion's control state, between Collect and Classify.
// Collect has just finished, so the scope tree is complete and frozen -- a
// node pointer taken DURING Collect would not have been, because a parent's
// children vector reallocates as its later siblings are pushed. Nothing
// runs at all unless Collect saw a control task, which is what leaves a
// design without one emitting the signals it always did.
void Lowerer::alloc_assert_control() {
    if (!assert_control_used_) {
        return;
    }
    const std::string saved_prefix = prefix_;
    for (uint32_t i = 0; i < sites_.size(); ++i) {
        AssertSite&        site = sites_[i];
        ir::InstanceScope* node = find_scope_node(top_scope_, site.scope);
        if (node == nullptr) {
            fail("internal: an assertion was enumerated outside the scope tree");
        }
        // Named by position in this table rather than by monitor id: the
        // monitor has no id yet (Emit hands those out), and an assertion
        // that later refuses to lower would leave a hole in that numbering.
        const std::string base = "sva.ctl" + std::to_string(i) + ".";
        scope_stack_.push_back(node);
        prefix_       = site.prefix;
        site.ctl.off  = new_monitor_state(base + "off", mask_type(1), site.loc);
        site.ctl.kill = new_monitor_state(base + "kill", mask_type(1), site.loc);
        site.ctl.poff = new_monitor_state(base + "poff", mask_type(1), site.loc);
        site.ctl.foff = new_monitor_state(base + "foff", mask_type(1), site.loc);
        site.ctl.lock = new_monitor_state(base + "lock", mask_type(1), site.loc);
        scope_stack_.pop_back();
    }
    prefix_ = saved_prefix;
}

// The control state of the assertion about to be lowered, or null when this
// design calls no control task.
const Lowerer::MonitorControl* Lowerer::control_for(const slang::ast::ConcurrentAssertionStatement& a) {
    if (!assert_control_used_) {
        return nullptr;
    }
    if (const auto it = site_of_.find(&a); it != site_of_.end()) {
        for (const uint32_t i : it->second) {
            if (sites_[i].scope == cur_scope_id()) {
                return &sites_[i].ctl;
            }
        }
    }
    // Collect walks the same procedural blocks Emit lowers, so a miss means
    // the two disagree about what a block contains -- and the assertion
    // would silently ignore every control task aimed at it.
    fail("internal: this concurrent assertion was not enumerated for assertion control");
}

// At what hierarchy depth `target` names this assertion: 1 for the instance
// the assertion sits in, 2 for that instance's parent, and so on; 0 for a
// target that is nowhere above it. This is $dumpvars' `levels` counting
// (LRM 21.7.1.3), which 20.12 adopts by reference.
uint32_t Lowerer::control_depth(const AssertSite& site, const Symbol& target) {
    // A label, or a named block the assertion sits in, is inside the
    // assertion's own instance, so it is depth 1.
    if (site.label == &target) {
        return 1;
    }
    uint32_t     depth = 1;
    const Scope* scope = site.owner->getParentScope();
    while (scope != nullptr) {
        const Symbol& sym = scope->asSymbol();
        if (&sym == &target) {
            return depth;
        }
        if (sym.kind != SymbolKind::InstanceBody) {
            scope = sym.getParentScope();
            continue;
        }
        // Leaving an instance is what a level counts. The target names the
        // InstanceSymbol rather than the body slang hangs the members off,
        // so the comparison steps across that pair.
        const InstanceSymbol* parent = sym.as<InstanceBodySymbol>().parentInstance;
        if (parent == nullptr) {
            return 0;
        }
        if (parent == &target) {
            return depth;
        }
        ++depth;
        scope = parent->getParentScope();
    }

    return 0;
}

// Whether any listed target names this assertion within `levels` of
// hierarchy. 0 means the target scope and every level below it; N means N
// levels starting at the target.
bool Lowerer::targets_cover(const AssertSite& site, const std::vector<const Symbol*>& targets, int32_t levels) {
    for (const Symbol* target : targets) {
        const uint32_t depth = control_depth(site, *target);
        if (depth != 0 && (levels == 0 || depth <= static_cast<uint32_t>(levels))) {
            return true;
        }
    }

    return false;
}

// LRM Table 20-7. `restrict` deliberately has no bit: it is a formal-only
// directive with no simulation verdict, and 20.12 gives no directive_type
// that selects one, so a control task never reaches it.
int32_t Lowerer::directive_bit(slang::ast::AssertionKind kind) {
    switch (kind) {
        case slang::ast::AssertionKind::Assert:
            return 1;
        case slang::ast::AssertionKind::CoverProperty:
        case slang::ast::AssertionKind::CoverSequence:
            return 2;
        case slang::ast::AssertionKind::Assume:
            return 4;
        case slang::ast::AssertionKind::Expect:
            // Not a directive_type at all: LRM Table 20-7 defines only assert,
            // cover and assume, and an expect is selected by assertion_type bit
            // 16 instead (Table 20-6). Zero here is what keeps every control
            // write off an expect's state -- the site loop tests
            // `directive_bit(kind) & dtype`, and no dtype can select 0 -- and bit
            // 16 itself is refused in lower_assert_control. Spelled out rather
            // than left to the default arm, because it is the whole of why an
            // $assertoff cannot silently gate an expect's seed.
            return 0;
        case slang::ast::AssertionKind::Restrict:
            // No verdict, so nothing to switch off (LRM 16.12).
            return 0;
    }

    return 0;
}

// Whether the design contains an `expect`, for the assertion-control refusal.
// Read from the Collect-pass site table rather than a flag of its own: that walk
// has already visited every concurrent directive by the time any control task is
// lowered, and a second flag could only ever disagree with it.
bool Lowerer::expect_seen() const {
    return std::any_of(sites_.begin(), sites_.end(), [](const AssertSite& site) {
        return site.kind == slang::ast::AssertionKind::Expect;
    });
}

// One of an assertion control task's integer arguments, constant-folded. A
// non-constant one is refused rather than lowered: these arguments do not
// feed the emitted code, they DECIDE it -- which of five bits are written,
// in which assertions, over how much of the hierarchy. A run-time value
// means a run-time dispatch over every assertion in scope, which is a
// different mechanism rather than a wider version of this one.
int32_t Lowerer::const_control_arg(const Expression& e, const char* what) {
    const std::optional<int32_t> value = try_const_int(e);
    if (!value.has_value()) {
        // TODO: non-constant assertion control arguments (LRM 20.12).
        fail("the " + std::string(what) +
             " argument of an assertion control task must be a constant; only the "
             "constant-argument subset of LRM 20.12 is supported");
    }

    return *value;
}

// A 1-bit control signal, written unconditionally.
void Lowerer::store_control_bit(ir::Signal sig, bool set, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index dst = b.signal(sig, l);
    const ir::Index val = set ? true_const(b, l) : zero_mask(1, b, l);
    b.binary(ir::Inst::Tag::Store, dst, val, l);
}

// The same, but holding its current value while the assertion is locked
// (LRM 20.12 Lock). A select, never a branch: this is straight-line code in
// whatever process the task was written in.
void Lowerer::store_control_locked(const MonitorControl& ctl,
                                   ir::Signal            sig,
                                   bool                  set,
                                   ir::Builder&          b,
                                   ir::SourceLoc         l) {
    const ir::Index want   = set ? true_const(b, l) : zero_mask(1, b, l);
    const ir::Index dst    = b.signal(sig, l);
    const ir::Index locked = load_signal(ctl.lock, b, l);
    const ir::Index cur    = load_signal(sig, b, l);
    const ir::Index val    = b.select(locked, cur, want, l);
    b.binary(ir::Inst::Tag::Store, dst, val, l);
}

// One control task's effect on one assertion.
void Lowerer::emit_control_write(const MonitorControl& ctl, AssertCtl code, ir::Builder& b, ir::SourceLoc l) {
    switch (code) {
        case AssertCtl::Lock:
            store_control_bit(ctl.lock, true, b, l);
            return;
        case AssertCtl::Unlock:
            store_control_bit(ctl.lock, false, b, l);
            return;
        case AssertCtl::On:
            store_control_locked(ctl, ctl.off, false, b, l);
            return;
        case AssertCtl::Off:
            store_control_locked(ctl, ctl.off, true, b, l);
            return;
        case AssertCtl::Kill:
            // Kill is Off plus an abandonment of what is already running,
            // and stays off until a subsequent On.
            store_control_locked(ctl, ctl.off, true, b, l);
            store_control_locked(ctl, ctl.kill, true, b, l);
            return;
        case AssertCtl::PassOn:
            store_control_locked(ctl, ctl.poff, false, b, l);
            return;
        case AssertCtl::PassOff:
            store_control_locked(ctl, ctl.poff, true, b, l);
            return;
        case AssertCtl::FailOn:
            store_control_locked(ctl, ctl.foff, false, b, l);
            return;
        case AssertCtl::FailOff:
            store_control_locked(ctl, ctl.foff, true, b, l);
            return;
        case AssertCtl::NonvacuousOn:
            // "Enable the pass action on NONVACUOUS success." Every pass a
            // monitor here reports is nonvacuous: report_pass is built out
            // of obligations that completed (emit_monitor's `matched`), and
            // an attempt whose antecedent never matched produces no verdict
            // bit at all rather than a vacuous one. The nonvacuous half is
            // therefore the whole of it, and this is exactly PassOn.
            store_control_locked(ctl, ctl.poff, false, b, l);
            return;
        case AssertCtl::VacuousOff:
            // "Stop the pass action on VACUOUS success" -- already the
            // behaviour, and not a gap: by the paragraph above no vacuous
            // success ever reaches an action block here, so there is nothing
            // to switch off and no bit worth carrying for it. Writing
            // PassOff instead would suppress the nonvacuous passes the LRM
            // is explicit about keeping.
            return;
    }
}

// $asserton / $assertoff / $assertkill, the four action tasks,
// $assertnonvacuouson / $assertvacuousoff, and $assertcontrol (LRM 20.12).
//
// slang binds all ten through one AssertControlTask (its SystemTasks.cpp):
// it has already checked the arity and resolved every trailing argument to
// a scope or an assertion instance, rejecting anything else. So what
// arrives here is a control code, the filters, and a target list that is
// already a list of symbols.
void Lowerer::lower_assert_control(std::string_view name, const CallExpression& call, ir::Builder& b) {
    const auto args = call.arguments();
    const bool full = name == "$assertcontrol";

    // LRM 20.12's defaults: every assertion type and every directive type.
    // The nine named tasks are defined as $assertcontrol(code, 15, 7,
    // levels, list) -- 15 rather than 31 because none of them affects an
    // `expect`, which is the bit 31 adds.
    int32_t code   = full ? 0 : named_control_code(name);
    int32_t atype  = full ? 31 : 15;
    int32_t dtype  = 7;
    int32_t levels = 0;
    size_t  first  = full ? 4 : 1;
    if (full) {
        code = const_control_arg(*args[0], "control_type");
        if (args.size() > 1) {
            atype = const_control_arg(*args[1], "assertion_type");
        }
        if (args.size() > 2) {
            dtype = const_control_arg(*args[2], "directive_type");
        }
        if (args.size() > 3) {
            levels = const_control_arg(*args[3], "levels");
        }
    } else if (!args.empty()) {
        levels = const_control_arg(*args[0], "levels");
    }

    if (code < static_cast<int32_t>(AssertCtl::Lock) || code > static_cast<int32_t>(AssertCtl::VacuousOff)) {
        fail("'" + std::string(name) + "' control_type " + std::to_string(code) +
             " is not one of LRM Table 20-5's codes 1..11");
    }
    if (levels < 0) {
        fail("'" + std::string(name) + "' levels must not be negative");
    }
    if ((dtype & ~int32_t{ 7 }) != 0) {
        fail("'" + std::string(name) + "' directive_type " + std::to_string(dtype) +
             " has bits LRM Table 20-7 does not define (1 assert, 2 cover, 4 assume)");
    }
    if ((atype & (32 | 64 | 128)) != 0) {
        // TODO: assertion control over unique / unique0 / priority violation
        // reporting (LRM Table 20-6, bits 32/64/128). Those are not
        // assertions with state of their own -- the check is emitted inline
        // at the `if` / `case` -- so gating them is a different mechanism.
        fail("'" + std::string(name) + "' over unique/unique0/priority violation checks is not supported");
    }
    if ((atype & (2 | 4 | 8)) != 0 && immediate_assertion_seen_) {
        // TODO: assertion control over immediate and deferred assertions (LRM
        // Table 20-6, bits 2/4/8). They lower inline into the process that holds
        // them, with no per-assertion state for a control task to write, so an
        // enable would have to be threaded through lower_immediate_assertion
        // instead.
        fail("'" + std::string(name) +
             "' over immediate assertions is not supported; select concurrent assertions alone with "
             "$assertcontrol(<control_type>, 1, ...)");
    }
    if ((atype & 16) != 0 && expect_seen()) {
        // Bit 16 is the `expect` (LRM Table 20-6), and it is the reason the nine
        // named tasks pass 15 rather than 31. An expect HAS per-assertion state
        // now, so this is no longer covered by the immediate-assertion refusal
        // above -- and it cannot be waved through either: `$assertkill` on an
        // expect would abandon an attempt a process is blocked on, and the seed
        // gate an $assertoff writes is the same one the request drives.
        // TODO: assertion control over an `expect` (LRM 20.12 with
        // assertion_type bit 16, which 16.17 explicitly extends to an expect's
        // pass and fail statements). Needs a disposition for a blocked process
        // whose attempt is killed, which is the `disable iff` question again.
        fail("'" + std::string(name) +
             "' over `expect` statements is not supported; select concurrent assertions alone with "
             "$assertcontrol(<control_type>, 1, ...)");
    }

    std::vector<const Symbol*> targets;
    targets.reserve(args.size() - std::min(first, args.size()));
    for (size_t k = first; k < args.size(); ++k) {
        const auto* as = args[k]->as_if<slang::ast::ArbitrarySymbolExpression>();
        if (as == nullptr) {
            fail("'" + std::string(name) + "' trailing arguments must name a scope or an assertion");
        }
        targets.push_back(as->symbol.get());
    }

    if ((atype & 1) == 0) {
        // Every type this task selects is one the guards above already
        // proved absent from the design, so it names no assertion. Not a
        // silent drop: there is nothing here to control.
        return;
    }
    if (!assert_control_used_) {
        // Collect decides whether any control state exists, and it reaches a
        // task only through a procedural block or a call it can resolve
        // statically. One arriving here unannounced was reached some other
        // way, and there is nothing allocated for it to write.
        // TODO: assertion control from a class method reached only through a
        // virtual dispatch, or from any body scan_assert_control cannot
        // follow a call into.
        fail("'" + std::string(name) +
             "' in this position is not supported: the control state of its target assertions is allocated "
             "from the declaration walk, which never reached this call");
    }

    const ir::SourceLoc l = loc(call);
    for (const AssertSite& site : sites_) {
        if ((directive_bit(site.kind) & dtype) == 0) {
            continue;
        }
        // An empty list is the whole design (LRM 20.12: `$assertcontrol(OFF)`
        // uses the default values of every other argument), and then `levels`
        // has no scope to count from and does not apply.
        if (!targets.empty() && !targets_cover(site, targets, levels)) {
            continue;
        }
        emit_control_write(site.ctl, static_cast<AssertCtl>(code), b, l);
    }
}

void Lowerer::lower_concurrent_assertion(const slang::ast::ConcurrentAssertionStatement& a, ir::Builder& b) {
    using AK  = slang::ast::AssertionKind;
    using AEK = slang::ast::AssertionExprKind;

    const ir::AssertionKind kind = [&] {
        switch (a.assertionKind) {
            case AK::Assert:
                return ir::AssertionKind::Assert;
            case AK::Assume:
                return ir::AssertionKind::Assume;
            case AK::CoverProperty:
            case AK::CoverSequence:
                return ir::AssertionKind::Cover;
            case AK::Restrict:
                // Accepted, lowered, and then deliberately silent. `restrict`
                // constrains the input space a FORMAL tool may explore; LRM 16.12
                // gives it no simulation verdict at all, and slang enforces that
                // by rejecting an action block on one. Lowering it anyway is not
                // pointless: the property is bound, its roots are sampled, and a
                // shape libsva cannot handle is still refused here rather than
                // waved through. What emit_monitor then skips is the verdict, the
                // report and the action pend -- see the switch there.
                return ir::AssertionKind::Restrict;
            case AK::Expect:
                // LRM 16.17. A procedural, BLOCKING directive: the same property
                // layer as an assert, plus a one-shot seed and a wait. slang binds
                // it as a ConcurrentAssertionStatement and dispatches it as a
                // statement, so what arrives here is indistinguishable from an
                // `assert property` until this point -- and everything downstream of
                // the monitor is what tells them apart (bare_seed_gate,
                // emit_expect_step, and the wait emitted at the bottom of this
                // function).
                return ir::AssertionKind::Expect;
        }
        __builtin_unreachable();
    }();
    // `cover sequence` counts every MATCH of the sequence where `cover
    // property` counts the attempts that succeeded (LRM 16.12.6). Same
    // automaton, same SVA_COVER to libsva, so the distinction is carried
    // beside the kind and read only by the tally.
    const bool cover_matches = a.assertionKind == AK::CoverSequence;

    // Action blocks run in the Reactive region (LRM 4.4.2.6). The monitor
    // itself is an Observed process, and LRM 4.4.2.5 is explicit that a
    // property evaluated in Observed schedules its pass/fail code into
    // Reactive -- so each action becomes a zero-formal task that the verdict
    // pends (build_action_body / emit_action_pends). The trailing `;` of an
    // action-less assertion binds as an Empty statement rather than as
    // nothing, so an empty action is no action -- slang draws the same
    // distinction in its own `restrict` check.
    const auto is_action = [](const Statement* action) {
        return action != nullptr && action->kind != StatementKind::Empty;
    };
    const AssertionActions actions{ .pass = is_action(a.ifTrue) ? a.ifTrue : nullptr,
                                    .fail = is_action(a.ifFalse) ? a.ifFalse : nullptr };
    if (actions.pass != nullptr) {
        require_reactive_safe(*actions.pass);
    }
    if (actions.fail != nullptr) {
        require_reactive_safe(*actions.fail);
    }

    // Resolve the clocking event, in the order LRM 16.13/16.14.6 imply. An
    // explicit event at the top of the assertion wins. Failing that, one
    // written inside the property or sequence body flows out to the whole
    // assertion (16.13.1; see body_clock). Failing both, it is inferred from
    // the enclosing procedure (16.14.6): the analysis pass already decided
    // WHICH event control that is, and inferred_clocks_ carries that verdict
    // as a syntax node. Turning it back into a TimingControl is done here
    // rather than there, against `proc_timing_` -- the event control of the
    // procedure this Lowerer is lowering right now -- because the analysis
    // sees one instance body per group of duplicates and its AST would name
    // the wrong instance's clock signal (see InferredClockMap).
    //
    // Once resolved, an inferred or hoisted clock is a TimingControl exactly
    // like an explicit one, so everything below is unchanged by where it came
    // from. The one thing that is not: a clock hoisted out of the body leaves
    // its ClockingAssertionExpr in place, so sva::lower is told which event
    // became the assertion's and skips exactly that one.
    const slang::ast::AssertionExpr* body                   = &a.propertySpec;
    const TimingControl*             clocking               = nullptr;
    bool                             inference_untranslated = false;
    if (body->kind == AEK::Clocking) {
        const auto& wrapper = body->as<slang::ast::ClockingAssertionExpr>();
        clocking            = &wrapper.clocking;
        body                = &wrapper.expr;
    } else if ((clocking = body_clock(*body)) == nullptr && inferred_clocks_ != nullptr && a.syntax != nullptr) {
        const auto it = inferred_clocks_->find(a.syntax);
        if (it != inferred_clocks_->end() && it->second != nullptr) {
            clocking               = local_event_with_syntax(proc_timing_, it->second);
            inference_untranslated = clocking == nullptr;
        }
    }
    if (clocking == nullptr) {
        // Nothing explicit, nothing hoisted out of the body, and nothing the
        // procedure's own event control could supply -- either because slang
        // inferred an event this procedure does not wait on, or because it
        // inferred none at all. LRM 16.14.6 answers both the same way: "if no
        // clock can be inferred from the procedural context, then the clocks
        // shall be inferred from the default clocking (14.12), as if the
        // assertion were instantiated immediately before the procedure", which
        // LRM 16.16(a) restates as the default clocking event being treated as
        // though written as the leading clocking event. So it is not a guess: it
        // is the last rule in the precedence chain (16.16: explicit > contextual
        // > default).
        //
        // `getEvent()` is bound in the block's own scope, so it is already this
        // instance's TimingControl -- no InferredClockMap round trip.
        if (const Symbol* def = scope_->getCompilation().getDefaultClocking(*scope_)) {
            const auto& cb = def->as<ClockingBlockSymbol>();
            clocking       = &clocking_event(cb);
            // Whatever else this assertion reads, it is now clocked by a block
            // whose own state must exist: refuse rather than lower against a
            // block Collect turned down.
            (void)clocking_state(cb);
        } else if (inference_untranslated) {
            // A `$inferred_clock` expansion, or an event control buried in the
            // body rather than leading it, with no default clocking to fall back
            // on. Picking the procedure's event control anyway would silently
            // clock the assertion on the wrong edge.
            // TODO: `$inferred_clock` (LRM 16.14.7) as an assertion clock.
            fail("the clocking event inferred for this assertion is not one the "
                 "enclosing procedure waits on");
        } else {
            // Unreachable for a well-formed design: slang's own analysis rejects
            // an assertion with neither an explicit nor an inferable clock
            // (diag::AssertionNoClock), and lowering never runs on a design that
            // errored. It still fails loudly rather than guessing, because the
            // table is only as complete as the analysis pass that filled it.
            fail("a concurrent assertion needs a clocking event, and none could be "
                 "inferred from its context");
        }
    }
    // An event control that NAMES a clocking block -- `@(cb)`, or
    // `@($global_clock)` -- stands for that block's own clocking event (LRM
    // 14.15 / 14.14, and 16.14.6(c)(1) admits it as an inferable clock). The
    // substitution is the one 16.16(a) already makes for a default clocking, so
    // from here on such an assertion is indistinguishable from an explicitly
    // clocked one.
    clocking = &clocking_event_of(*clocking);
    if (clocking->kind != TimingControlKind::SignalEvent) {
        // TODO: multiclock assertions (LRM 16.13).
        fail("only a single-signal clocking event is supported");
    }
    const auto& event = clocking->as<SignalEventControl>();
    if (event.iffCondition != nullptr) {
        // TODO: `@(posedge clk iff cond)` as an assertion clock.
        fail("an `iff` qualifier on an assertion clock is not supported");
    }
    if (event.edge != slang::ast::EdgeKind::PosEdge && event.edge != slang::ast::EdgeKind::NegEdge) {
        // A level-sensitive or both-edge assertion clock does not delimit
        // cycles the way the automaton assumes.
        // TODO: `edge` and level-sensitive assertion clocks.
        fail("an assertion clock must be a posedge or a negedge");
    }
    // A program signal as the leading clocking event: LRM 24.3.1 requires the
    // scheduler to iterate from the reactive region set back around the outer
    // loop, through the OBSERVED region where the assertion is evaluated, and
    // clotho's loop never re-enters Observed once the reactive drain empties.
    // The monitor would step one whole time slot late, silently. Reachable only
    // for an assertion INSIDE a program: 24.3 makes a reference to a program
    // signal "from outside any program block" an error and slang enforces it.
    // Full analysis, and the two candidate fixes, on program_clock_signal.
    if (const ValueSymbol* psig = program_clock_signal(event.expr)) {
        fail("a concurrent assertion clocked by the program signal `" + std::string(psig->name) +
             "` is not supported: the assertion is evaluated in the Observed region, which "
             "Clotho does not re-enter after the reactive region set drains (LRM 24.3.1 "
             "requires the scheduler to iterate back around the outer loop through Observed), "
             "so it would step a full time slot late -- clock it from a design signal, which "
             "is what LRM 24.4 asks for anyway");
    }

    // Peel the disable condition. LRM 16.15: it is deliberately NOT sampled,
    // so it is evaluated from live values while every other root reads the
    // sampled shadow.
    //
    // Two places may carry one, and LRM 16.12 makes them mutually exclusive:
    // "Nesting of disable iff clauses, explicitly or through property
    // instantiations, is not allowed". So there is no precedence to define and
    // none is invented here -- an assertion carrying its own clause while the
    // property it instantiates carries another is illegal, slang has already
    // diagnosed it (diag::NestedDisableIff), and clotho refuses the pair
    // rather than picking a winner. A reset condition is not something to
    // guess at, and either choice would be a semantics clotho made up.
    //
    // The declaration's clause is left standing in the tree exactly like a
    // hoisted clock, so sva::lower is told which node became the assertion's
    // reset and steps over that one alone.
    const Expression* disable = nullptr;
    if (body->kind == AEK::DisableIff) {
        const auto& wrapper = body->as<slang::ast::DisableIffAssertionExpr>();
        disable             = &wrapper.condition;
        body                = &wrapper.expr;
    }
    if (const Expression* declared = body_disable(*body); declared != nullptr) {
        if (disable != nullptr) {
            fail("an assertion and the named property it instantiates cannot both carry a "
                 "`disable iff`");
        }
        disable = declared;
    }

    const sva::Request request{
        .property = body,
        .disable  = disable,
        .clock    = clocking,
        .kind     = kind,
    };
    sva::Result lowered = sva::lower(request);
    if (!lowered.ok) {
        // A libsva rejection is a property of the source, so it is reported
        // against the offending sub-expression when one is named.
        slang::SourceRange where = a.sourceRange;
        if (lowered.blame != nullptr && lowered.blame->syntax != nullptr) {
            where = lowered.blame->syntax->sourceRange();
        }
        cur_range_ = where;
        fail(std::move(lowered.error));
    }

    ir::AssertionMonitor& monitor = lowered.monitor;
    monitor.clock                 = event_signal(event.expr);
    monitor.edge   = event.edge == slang::ast::EdgeKind::PosEdge ? ir::Edge::Positive : ir::Edge::Negative;
    monitor.span   = to_source_span(a.sourceRange);
    // A labelled assertion (`ap: assert property ...`) is lowered inside the
    // block slang makes of its label, so its report names the label as well.
    monitor.banner = severity_banner(a.sourceRange.start());
    // Not round-tripped through libsva, which has one cover kind: the tally
    // is clotho's own choice and this is where the choice is recorded.
    monitor.cover_matches = cover_matches;

    if (kind == ir::AssertionKind::Expect) {
        expect_shape_check(monitor, disable);
    }

    // Shadow every variable the assertion samples, before anything reads one.
    sva_sample_roots(lowered);

    // The step is emitted from here as ordinary IR (see the section below),
    // so `monitor.roots` stays empty: it indexes instructions of a body the
    // monitor would own, and there is none -- the roots are ordinary values
    // inside the process emitted here.
    const uint32_t    id    = static_cast<uint32_t>(module_.monitors.size());
    const ExpectPorts ports = emit_monitor(lowered, monitor, id, actions, control_for(a));
    module_.monitors.push_back(std::move(monitor));

    if (kind == ir::AssertionKind::Expect) {
        emit_expect_wait(id, ports, a, b);
    }
}

// The two property shapes an `expect` cannot be given a verdict for, refused
// before any of it is emitted. Both refusals are about the same thing: the
// process is only ever unblocked by a verdict, so a shape that can finish
// WITHOUT one parks it for the rest of the run.
void Lowerer::expect_shape_check(const ir::AssertionMonitor& monitor, const Expression* disable) {
    if (monitor.has_trigger) {
        // An implication whose antecedent never matches is a VACUOUS success,
        // and emit_monitor deliberately produces no verdict bit for one (see the
        // pass report there): an attempt that never reached its obligation has
        // nothing to report. For an assert that is exactly right; for an expect
        // it is a process that never wakes.
        // TODO: `expect` over an implication (LRM 16.17 accepts "the same syntax
        // used to assert a property"). Needs the vacuous success of an
        // antecedent that failed to match to become a real verdict bit, which is
        // a change to the assert path shared with every other directive.
        fail("an `expect` over an implication is not supported: a vacuous success "
             "produces no verdict, and the process would never unblock");
    }
    if (disable != nullptr) {
        // LRM 16.15 ABANDONS the attempts a disable covers: no verdict, by
        // design. An assert simply reports nothing for them; an expect's process
        // would wait forever on the verdict that was abandoned.
        // TODO: `disable iff` on an `expect`. Needs a disposition for an
        // abandoned attempt that has a process blocked on it -- resume it with
        // neither action, or leave it blocked -- which LRM 16.17 does not
        // settle.
        fail("a `disable iff` on an `expect` is not supported: an abandoned "
             "attempt produces no verdict, and the process would never unblock");
    }
}

// The statement half of an `expect` (LRM 16.17), emitted into the process that
// executes it. Two instructions and a guard.
//
// The request is stored BEFORE the wait, and that order is the protocol: the
// store lands in whatever region this process runs in, the monitor reads it at
// its next clocking edge in Observed, and Observed follows every active-region
// store of the same time slot. So an `expect` executed at t=1 with a posedge at
// t=5 starts its attempt at t=5 -- "the first evaluation shall take place on the
// next clocking event", which is the offset tests/sva/diff_expect.sv measures.
//
// The guard is the one shape this design cannot serve. Every waiter in a
// monitor's queue is woken by the same verdict, which is exact for a cohort that
// joined before the same seeding edge -- attempts starting on one edge see one
// signal history, so they cannot disagree -- and wrong for anyone who joins
// while an earlier attempt is still running. `expect_busy` is precisely "an
// earlier attempt is still running", and it is read HERE rather than in the
// monitor because this is where the process still exists to be told.
void Lowerer::emit_expect_wait(uint32_t                                        id,
                               const ExpectPorts&                              ports,
                               const slang::ast::ConcurrentAssertionStatement& a,
                               ir::Builder&                                    b) {
    if (ports.req == ir::Signal{ 0 } || ports.busy == ir::Signal{ 0 }) {
        fail("internal: an `expect` monitor without its request state");
    }
    if (fn_ctx_ != nullptr) {
        // An expect resumes its process into Reactive and LRM 24.3.1 keeps the
        // thread there for every later blocking statement. The home region that
        // carries that lives in the resumed body's own frame, so a task can
        // change its OWN and not its caller's: the caller would come back
        // through the task's completion handshake into Active, and every
        // statement after the call would be in the wrong region with no
        // diagnostic. Refused here rather than only in the backend so the
        // message names the construct instead of the lowering.
        // TODO: `expect` inside a task or function (LRM 16.17 puts it "anywhere
        // a wait statement can appear", which includes one). Needs the resume
        // region to travel with the REASON for the wake up the call chain,
        // rather than with the frame that parked.
        fail("an `expect` inside a task or function is not supported: the process "
             "it blocks would resume in the Active region");
    }
    const ir::SourceLoc l = to_source_loc(a.sourceRange.start());

    // TODO: two attempts of one `expect` in flight at once, which is two
    // processes reaching it in different cycles, or one re-executing it before
    // its previous attempt resolved. Each attempt would have to carry which
    // waiters belong to it -- an attempt id per obligation slot, matched against
    // a generation recorded with each parked handle -- and under
    // AssertionTracking::Counted there are no slots to hang one on at all. Loud
    // at run time rather than refused at compile time because two processes
    // reaching one statement is not a static property of the source.
    b.open_region();
    const std::string banner = banner_at(a.sourceRange.start());
    b.severity(static_cast<uint32_t>(ir::Severity::Fatal),
               severity_banner(a.sourceRange.start()),
               "an `expect` whose previous attempt is still in flight is not supported",
               {},
               l);
    b.finish(banner + "expect", 1, l);
    const ir::ExtraSlice then_region = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_region = b.close_region();
    b.cond(load_signal(ports.busy, b, l), then_region, else_region, l, /*unlikely=*/true);

    // One bit, set: the monitor's next edge consumes it and clears it. Several
    // processes storing it before that edge are one cohort and share the attempt,
    // which is exact rather than an approximation -- see the guard above.
    const ir::Index req = b.signal(ports.req, l);
    const ir::Index yes = true_const(b, l);
    b.binary(ir::Inst::Tag::Store, req, yes, l);
    b.expect_wait(id, l);
}

// An action block is pended into the Reactive region as its own coroutine (LRM
// 4.4.2.6) and the drain resumes it there. It may suspend: a parked handle
// carries the region it parked from and every wake path pushes it back onto
// that ring, so a delay, an event control or a `wait` in an action block comes
// back in Reactive rather than Active. LRM 24.3.1 says that of the region set's
// other inhabitant and says it of the THREAD rather than of the wake site, and
// an action block is a thread of the same region set by 4.4.2.6.
//
// What is refused is what no wake can fix: a body that never returns, a report
// that matures in a region already past, an update whose commit region is
// unsettled, and a construct clotho does not lower inside a task at all. See
// ReactiveSafetyChecker for the case-by-case reason.
//
// Whitelisted, not blacklisted. A statement kind this walk does not recognize
// is refused -- PRINCIPLES.md forbids silently accepting a construct that is not
// correctly lowered, and "the walker did not recognize it" is no evidence that
// it is safe.
void Lowerer::require_reactive_safe(const Statement& action) {
    ReactiveSafetyChecker checker;
    action.visit(checker);
    if (!checker.reason.empty()) {
        fail(checker.reason);
    }
}

// The second, tighter check, for the copy of the fail action that
// emit_monitor_finish lowers INLINE into a Final process. Only a strong
// property gets that copy, so only a strong property pays for it: this runs
// from emit_monitor_finish rather than beside require_reactive_safe above,
// which is bound before the property's strength is known.
//
// A Final process is resumed exactly once and then destroyed, so the inlined
// copy may not suspend at all -- not even on the `#0` the pended copy is
// allowed. Without this check that `#0` was accepted and its tail silently
// dropped at the end of the trace, which is exactly the outcome the whitelist
// exists to prevent.
//
// The same rule a USER `final` block already lives under -- LRM 9.2.3, checked
// over the emitted IR in lower_procedural_block -- reaching the other producer
// of a Final process. Checked over the AST instead of the IR here because this
// one is a source-level refusal a user has to act on, so it wants the statement
// named and a source location, not a tag.
void Lowerer::require_finish_inlinable(const Statement& action) {
    ReactiveSafetyChecker checker;
    checker.copy = ReactiveSafetyChecker::Copy::Inlined;
    action.visit(checker);
    if (!checker.reason.empty()) {
        fail(checker.reason);
    }
}

// Lower one action block into a synthetic zero-formal task, the same shape as
// build_fork_child: a fresh, empty FnCtx, run in the *parent's* scope context
// (scope_ / prefix_ / time_mul_ unchanged) so the body's references to the
// enclosing module's signals still resolve. It takes no formals because there
// is nothing to pass -- the verdict that pends it is the whole input.
ir::FunctionId Lowerer::build_action_body(const Statement& body, std::string_view name) {
    const ir::FunctionId id = module_.reserve_function();

    FnCtx ctx;
    ctx.name_prefix = std::string(name) + ".";
    ctx.is_task     = true;
    const FnScope fn_scope(*this, &ctx);

    ir::Builder                b(module_.pool);
    const ir::Builder::Mark    body_mark = b.mark();
    std::optional<LowerError>  failure;
    try {
        lower_stmt(body, b);
    } catch (const LowerError& e) {
        // See build_fork_child: `id` is already committed with no rollback;
        // complete the reservation with a dead body before propagating.
        b.rollback(body_mark);
        failure = e;
    }
    const ir::SourceLoc el = loc(body);
    b.ret_void(el);

    module_.set_function(id,
                         ir::Function{ .name        = std::string(name),
                                       .return_type = module_.pool.void_type,
                                       .param_types = {},
                                       .arg_dirs    = {},
                                       .local_types = std::move(ctx.local_types),
                                       .is_task     = true,
                                       .ir          = b.finish() });
    if (failure) {
        throw *failure;
    }
    return id;
}

// The default failure report, as the same kind of zero-formal task an explicit
// `else` becomes. An assertion with no action block still has a fail action --
// this is it -- so both take one path into the Reactive region and the report
// keeps its LRM 4.4.2.6 ordering against a user-written one.
//
// The severity is immediate (defer = 0) where the inline report used to be
// deferred. Deferring bought glitch suppression (LRM 16.4.2) against a monitor
// that could step several times in one time slot; an Observed process runs
// exactly once per slot, so there is no second evaluation left to glitch
// against, and a deferred report queued from Reactive would be flushing into
// an Observed queue this slot has already drained.
ir::FunctionId Lowerer::build_default_fail_body(const ir::AssertionMonitor& mon) {
    const ir::FunctionId id = module_.reserve_function();
    const ir::SourceLoc  l  = mon.span.start;

    ir::Builder b(module_.pool);
    b.severity(static_cast<uint32_t>(ir::Severity::Error), mon.banner, "assertion failed", {}, l, 0);
    b.ret_void(l);

    module_.set_function(id,
                         ir::Function{ .name        = "sva.fail",
                                       .return_type = module_.pool.void_type,
                                       .param_types = {},
                                       .arg_dirs    = {},
                                       .local_types = {},
                                       .is_task     = true,
                                       .ir          = b.finish() });
    return id;
}

void Lowerer::lower_immediate_assertion(const ImmediateAssertionStatement& a, ir::Builder& b) {
    if (a.isDeferred || a.isFinal) {
        lower_deferred_assertion(a, b);
        return;
    }
    using AK            = slang::ast::AssertionKind;
    const bool is_cover = a.assertionKind == AK::CoverProperty;
    if (a.assertionKind != AK::Assert && a.assertionKind != AK::Assume && !is_cover) {
        // TODO: immediate restrict / expect (LRM 16.3) need a property engine;
        // concurrent cover (CoverSequence) needs SVA.
        fail("only immediate assert / assume / cover are supported");
    }
    const ir::Index pred = lower_predicate(a.cond, b);

    b.open_region();
    if (a.ifTrue != nullptr) {
        lower_stmt(*a.ifTrue, b);
    }
    const ir::ExtraSlice then_region = b.close_region();

    // False branch: cover does nothing; assert/assume run the else action or
    // the default failure report.
    b.open_region();
    if (!is_cover) {
        if (a.ifFalse != nullptr) {
            lower_stmt(*a.ifFalse, b);
        } else {
            b.severity(static_cast<uint32_t>(ir::Severity::Error),
                       severity_banner(a.sourceRange.start()),
                       "assertion failed",
                       {},
                       loc(a));
        }
    }
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, loc(a));
}

// Seed every $past ring entry with its variable's time-zero value, appended
// to the declaration initializers so it runs in the same PreInit process,
// after them.
//
// The ordering is what makes the value right, and all of it is already fixed:
// PreInit applies declaration initializers, and the backend takes the initial
// Preponed sample AFTER PreInit (LlvmBackend's emit_main), so reading the LIVE
// variable here yields exactly what that first sample will capture. Appending
// rather than prepending matters for the same reason -- a seed emitted before
// `logic [3:0] v = 4'd7` would copy the power-up X instead of the 7.
//
// Called before finalize_preinit, which is what closes the process.
void Lowerer::finalize_sva_history() {
    if (sva_history_seeds_.empty()) {
        return;
    }
    if (!preinit_) {
        preinit_.emplace(module_.pool);
    }
    ir::Builder& b = *preinit_;
    for (const auto& [live, entry] : sva_history_seeds_) {
        const ir::SourceLoc l   = module_.signal(entry).loc;
        const ir::Index     dst = b.signal(entry, l);
        const ir::Index     val = b.unary(ir::Inst::Tag::Load, b.signal(live, l), l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
    }
    sva_history_seeds_.clear();
}

// -- Concurrent assertion sampling (LRM 16.5, 4.4.2.1) --
//
// An assertion expression reads SAMPLED values, and what is sampled is the
// VARIABLE, not the expression: LRM 16.6 has an operand captured before the
// expression is evaluated, so a capture is a plain copy with no operator in
// it. One shadow per variable, shared by every assertion mentioning it --
// including two assertions in different asserting scopes -- and copied by
// the single Preponed process finalize_preponed emits.
//
// The shadow keeps the live signal's exact type -- full width, full 4-state.
// Narrowing it to the one bit a boolean root needs would move operators into
// the sampling region and read everything but the base live: `mem[i]` sampled
// as a bit would resolve `i` at sample time rather than at evaluation time.
// See docs/design/concurrent-assertions.md 4.
ir::Signal Lowerer::sva_shadow_of(ir::Signal live) {
    return sampled_shadow_of(live, "a concurrent assertion");
}

// The Preponed shadow of `live`, allocated once per live signal and shared by
// every reader. `what` names the construct doing the sampling, for the
// packed-value refusal below: an assertion (LRM 16.5) and a clocking block
// input whose skew is `#1step` (LRM 14.4) want the identical value and so
// share one copy.
ir::Signal Lowerer::sampled_shadow_of(ir::Signal live, std::string_view what) {
    const auto key = static_cast<uint32_t>(live);
    if (const auto it = sva_shadows_.find(key); it != sva_shadows_.end()) {
        return it->second;
    }

    // A shadow is per-instance state, so it belongs to the scope that
    // declares `live` -- which need not be the scope lowering the
    // assertion. A hierarchical reference through a directly-instantiated
    // interface (or any deeper instance path) samples a variable one or
    // more hops away, and a package / $unit variable belongs to no
    // instance at all. signal_scope_ (register_signal's own inverse) names
    // `live`'s declaring scope id regardless of which scope is currently
    // open; find_scope_node walks the scope tree to the node itself,
    // which by now is frozen (alloc_assert_control's comment). A miss in
    // signal_scope_ means `live` was never registered by THIS top's
    // Lowerer -- a package / $unit-scope variable, always registered by
    // the standalone globals Lowerer, whose scope_stack_ is always empty.
    ir::InstanceScope* declaring_scope = nullptr;
    if (const auto sit = signal_scope_.find(key); sit != signal_scope_.end()) {
        declaring_scope = find_scope_node(top_scope_, sit->second);
        if (declaring_scope == nullptr) {
            fail("internal: a sampled variable's declaring scope is missing "
                 "from the scope tree");
        }
    }

    const ir::SignalInfo& live_info = module_.signal(live);
    // The copy is one whole-signal Load feeding one whole-signal Store, so the
    // type must have a flat plane value. LRM 16.6 explicitly contemplates
    // sampling an element of a dynamic array / queue / associative array, and
    // an unpacked aggregate would need a per-leaf copy.
    // TODO: sample an aggregate / container variable leaf-wise.
    if (live_info.is_event || value_cat(live_info.type) != ValueCat::scalar) {
        fail(std::string(what) + " may only sample a variable with a packed "
                                 "value; a string, an event, an unpacked aggregate, and a dynamic "
                                 "array / queue / associative array are not supported");
    }

    ir::SignalInfo info;
    info.name = "sva.shadow." + live_info.name;
    info.loc  = live_info.loc;
    info.type = live_info.type;
    // Left a variable (is_net stays false): nothing drives a shadow but the
    // sampling process, so it must neither take a net's undriven-z power-up nor
    // join net resolution.

    // Registered in `live`'s OWN declaring scope, not scope_stack_.back()
    // (the asserting scope): the two coincide for the common same-scope
    // case, but a hierarchical or package live signal needs its shadow
    // where the backend already knows how to address it relative to
    // `live` itself -- the same per-instance state addressing an ordinary
    // hierarchical read of `live` already uses (Anchor,
    // src/ir/LlvmBackend.cpp), unlocked here simply by asking
    // register_signal to attach the shadow to the right scope rather than
    // whichever one happens to be open. A null declaring_scope (package /
    // $unit) leaves the shadow unscoped too, so it becomes a flat
    // root-level field beside the live signal it shadows (build_state_types)
    // instead of being pulled into whichever instance is asserting.
    ir::Signal shadow;
    if (declaring_scope != nullptr) {
        scope_stack_.push_back(declaring_scope);
        shadow = register_signal(std::move(info));
        scope_stack_.pop_back();
    } else {
        shadow = module_.add_signal(std::move(info));
    }
    sva_shadows_.emplace(key, shadow);
    sva_shadow_pairs_.emplace_back(live, shadow);

    return shadow;
}

// Register a shadow for every variable the assertion's boolean roots read.
// The roots root_reads_live names are skipped -- `disable iff` and the
// asynchronous aborts read live values, so a variable appearing only there
// needs no shadow and must not pay for one -- unless a $sampled names it,
// which is the one read in such a root that IS sampled (LRM 16.9.3). Atom
// lowering resolves a read through sva_shadow_of as well, so this pass is not
// what makes a shadow exist -- it fixes the copy order to the source's atom
// order rather than to the order the monitor happens to emit in.
void Lowerer::sva_sample_roots(const sva::Result& lowered) {
    ankerl::unordered_dense::set<uint32_t> atoms;
    ankerl::unordered_dense::set<uint32_t> seen;
    std::vector<uint32_t>                  work;
    for (uint32_t r = 0; r < lowered.root_index.size(); r++) {
        if (!root_reads_live(lowered.monitor, r)) {
            work.push_back(lowered.root_index[r]);
        }
    }
    // Operands point backwards but may be shared, so a plain DFS would revisit
    // a common subterm once per parent.
    while (!work.empty()) {
        const uint32_t at = work.back();
        work.pop_back();
        if (!seen.insert(at).second) {
            continue;
        }
        const sva::Result::RootNode& n = lowered.nodes[at];
        switch (n.op) {
            case sva::Result::RootNode::Op::Atom:
                atoms.insert(n.lhs);
                break;
            case sva::Result::RootNode::Op::Const:
                break;
            case sva::Result::RootNode::Op::Not:
                work.push_back(n.lhs);
                break;
            case sva::Result::RootNode::Op::And:
            case sva::Result::RootNode::Op::Or:
                work.push_back(n.lhs);
                work.push_back(n.rhs);
                break;
        }
    }

    // Walked in atom order, so the shadow set is a function of the source
    // rather than of the traversal. A boolean leaf is an ordinary expression:
    // its operands are the variables sampled, and a function-internal read is
    // not an operand of it (the same rule continuous_events applies).
    for (uint32_t i = 0; i < lowered.atoms.size(); i++) {
        if (!atoms.contains(i) || lowered.atoms[i].expr == nullptr) {
            continue;
        }
        const auto declare_static = [this](const slang::ast::ClassPropertySymbol& p) {
            return declare_static_prop(p);
        };
        SensitivityCollector c(sigmap_, SensitivityMode::ImplicitEvent, declare_static);
        lowered.atoms[i].expr->visit(c);
        if (lowered.atoms[i].match != nullptr) {
            // A `case` property guard reads both sides of its comparison.
            lowered.atoms[i].match->visit(c);
        }
        for (const ir::Signal s : c.reads) {
            sva_shadow_of(s);
        }
    }
}

// Emit the Preponed process (LRM 4.4.2.1: it runs before any body of the time
// step, and is equivalent to sampling the previous Postponed region): the copy
// of every sampled variable, and the clear of every per-step latch. One process
// for the whole top, like the PreInit pass -- the copies are independent
// whole-signal stores, so their order carries no meaning and one dispatch per
// step beats one per scope. A design with no concurrent assertion and no
// clocking block registers neither, and so gets no process and no state at all.
void Lowerer::finalize_preponed() {
    if (sva_shadow_pairs_.empty() && preponed_clears_.empty()) {
        return;
    }
    ir::Builder b(module_.pool);
    for (const auto& [live, shadow] : sva_shadow_pairs_) {
        const ir::SourceLoc l   = module_.signal(shadow).loc;
        const ir::Index     dst = b.signal(shadow, l);
        const ir::Index     val = b.unary(ir::Inst::Tag::Load, b.signal(live, l), l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
    }
    // "Did this clocking block's event occur in the current time step" (LRM
    // 14.11's `##0`, and the maturation window of a drive): set in Observed,
    // and false again from the top of the next step, which is here.
    for (const ir::Signal sig : preponed_clears_) {
        const ir::SourceLoc l    = module_.signal(sig).loc;
        const ir::Index     dst  = b.signal(sig, l);
        const ir::Index     zero = zero_mask(1, b, l);
        b.binary(ir::Inst::Tag::Store, dst, zero, l);
    }
    module_.add_process(ir::Process{ .activation = ir::Activation::Preponed, .ir = b.finish() });
    sva_shadow_pairs_.clear();
    preponed_clears_.clear();
}

} // namespace clotho::lower
