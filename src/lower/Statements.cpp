// Procedural blocks and statements: control flow, loops, case and declarations.

#include "lower/Lowerer.h"

namespace clotho::lower {

// The statement inside any sequential-block / single-statement-list wrappers.
// A labelled statement (`ap: assert property ...`) is a StatementBlockSymbol,
// so its statement arrives one or two layers down rather than bare.
const Statement& Lowerer::unwrap_blocks(const Statement& stmt) {
    const Statement* inner = &stmt;
    for (;;) {
        if (inner->kind == StatementKind::Block &&
            inner->as<BlockStatement>().blockKind == StatementBlockKind::Sequential) {
            inner = &inner->as<BlockStatement>().body;
        } else if (inner->kind == StatementKind::List && inner->as<StatementList>().list.size() == 1) {
            inner = inner->as<StatementList>().list[0];
        } else {
            return *inner;
        }
    }
}

// The procedure's outermost scope, when that scope is a NAMED block:
// `always @clk begin : blk ... end` has blk, `initial begin begin : blk … end
// end` has none (blk is nested inside the unnamed body). Only disabling this
// block flushes the procedure's deferred reports and pending assertion
// attempts (LRM 16.4.4 / 16.14.6.4); disabling a block nested any deeper, or a
// task, never flushes. A timing control is peeled because it is not a scope.
const Symbol* Lowerer::outermost_named_block(const Statement& body) {
    const Statement* s = &body;
    for (;;) {
        if (s->kind == StatementKind::List && s->as<StatementList>().list.size() == 1) {
            s = s->as<StatementList>().list[0];
        } else if (s->kind == StatementKind::Timed) {
            s = &s->as<slang::ast::TimedStatement>().stmt;
        } else {
            break;
        }
    }
    if (s->kind != StatementKind::Block || s->as<BlockStatement>().blockKind != StatementBlockKind::Sequential) {
        return nullptr;
    }

    return s->as<BlockStatement>().blockSymbol;
}

void Lowerer::lower_procedural_block(const ProceduralBlockSymbol& sym) {
    // A concurrent assertion in this body may need the procedure's own event
    // control as its inferred clock (LRM 16.14.6), so publish it for
    // lower_concurrent_assertion. Cleared first, and set below only where the
    // control is actually peeled: a module-item assertion (the early return
    // just below) and an `initial` have no enclosing event control to inherit.
    // Procedural blocks do not nest, so this needs no save/restore -- the next
    // block clears it on entry.
    proc_timing_ = nullptr;

    // Which named block (if any) a `disable` of the procedure's outermost scope
    // would name; see outermost_named_block and lower_disable.
    proc_root_block_ = outermost_named_block(sym.getBody());

    // A module-item concurrent assertion is not a process of its own. slang
    // wraps it in ProceduralBlockSymbol(Always, isFromAssertion) purely to
    // give it somewhere to live (BlockSymbols.cpp), and lowering the statement
    // emits the monitor's clocked process instead. This wrapper must not also
    // become one: an `always` with no sensitivity is a forever loop, and one
    // whose body emitted nothing spins at time zero. A module-item IMMEDIATE
    // assertion gets the same wrapper and genuinely needs the process, so this
    // keys on the statement rather than on isFromAssertion alone.
    const Statement& item = unwrap_blocks(sym.getBody());
    if (sym.isFromAssertion && item.kind == StatementKind::ConcurrentAssertion) {
        const auto& directive = item.as<slang::ast::ConcurrentAssertionStatement>();
        if (directive.assertionKind == slang::ast::AssertionKind::Expect) {
            // An `expect` is procedural: LRM 16.17 puts it "anywhere a wait
            // statement can appear", which a module item is not, and slang
            // rejects one here. The builder below is a throwaway -- a module-item
            // directive emits only its monitor's own process -- so an `expect`
            // arriving here would have its request store and its wait DROPPED
            // with the builder and block its process forever. Checked rather than
            // trusted, because the cost of being wrong is silence.
            fail("internal: an `expect` as a module item");
        }
        ir::Builder b(module_.pool);
        // `ap: assert property (...)` is the statement wrapped in a block named
        // for its label, which unwrap_blocks stepped through, so lower_stmt never
        // opened it. Open it here: the label is part of the scope the assertion's
        // report names (LRM 20.10, severity_banner).
        const Symbol* const label = proc_root_block_;
        const bool          named = label != nullptr && !label->name.empty();
        if (named) {
            named_blocks_.push_back({ &label->as<slang::ast::StatementBlockSymbol>(), kNoBlockLabel });
        }
        lower_concurrent_assertion(directive, b);
        if (named) {
            named_blocks_.pop_back();
        }
        return;
    }

    ir::Activation             activation = ir::Activation::Always;
    std::vector<ir::WaitEvent> wait_events;
    const Expression*          iff_guard = nullptr;

    if (sym.procedureKind == ProceduralBlockKind::Initial) {
        // LRM 24.3: a program's `initial` procedures execute in the Reactive
        // region, so every print they make lands after the Active-region work
        // of the same time slot. Same one-shot process either way -- only the
        // ring the backend seeds it onto differs.
        activation = in_program_ ? ir::Activation::Reactive : ir::Activation::Initial;
    } else if (sym.procedureKind == ProceduralBlockKind::Final) {
        // A `final` block runs once at simulation end (LRM 9.2.3). The backend
        // runs it, sensitivity-less, on both exit paths after every other
        // process settles. Lowered like an `initial` body; the zero-time
        // requirement (no timing controls) is checked after lowering below.
        activation = ir::Activation::Final;
    }

    // TODO: no non-sequential-behavior warning for `always_ff`. LRM 9.2.2.4
    // says tools should warn when the body does not represent sequential
    // logic, but slang's analysis library has no such diagnostic (unlike
    // inferred-latch / inferred-comb for the other two kinds), so nothing
    // surfaces it today. slang does enforce the hard 9.2.2.4 rules (exactly
    // one event control, no blocking timing controls).

    const Statement* body = &sym.getBody();

    // `always_comb` / `always_latch` infer sensitivity from the body's reads
    // minus its writes (9.2.2.2.1, legal because of the single-writer rule).
    // `always @*` carries an ImplicitEvent timing wrapping the body, inferred
    // under the subtly different 9.4.2.2 rules (see SensitivityMode).
    const bool comb =
      sym.procedureKind == ProceduralBlockKind::AlwaysComb || sym.procedureKind == ProceduralBlockKind::AlwaysLatch;
    bool infer = comb;

    // A top-level event control on an `always` is the process's sensitivity
    // list: collect its wait events here and lower the statement underneath.
    // On an `initial` the same syntax is a one-shot blocking wait (LRM 9.4):
    // hoisting it as sensitivity would run the statement unblocked at time
    // zero, so it is left in the body and lowers to a Wait through
    // lower_stmt's Timed case, exactly like a begin/end-wrapped body.
    if (activation == ir::Activation::Always && body->kind == StatementKind::Timed) {
        const TimedStatement& timed = body->as<TimedStatement>();
        if (timed.timing.kind == TimingControlKind::ImplicitEvent) {
            infer = true;
            body  = &timed.stmt;
        } else if (is_event_control(timed.timing) && !sensitivity_needs_handle(timed.timing)) {
            iff_guard    = collect_wait_events(timed.timing, wait_events);
            body         = &timed.stmt;
            proc_timing_ = &timed.timing;
        }
        // Otherwise the list names an event with no signal of its own
        // (`always @(ev[i])`). A static subscription cannot carry a runtime
        // handle, so the control is LEFT IN THE BODY: `looping` below wraps the
        // block in a forever loop and the `@(...)` lowers as an ordinary
        // intra-body wait, which re-evaluates the handle on every pass. That is
        // also the more correct reading of a re-triggering sensitivity list.
    }

    // An `always` is semantically a forever loop around its body (LRM 9.2.2).
    // Clotho models that loop implicitly when the body re-triggers on a
    // sensitivity list (event control above) or the block is combinational
    // (always_comb / always_latch / @*, inferred below). Any other `always` —
    // driven by a `#delay` or an internal `@`, or with no timing at all — has
    // no sensitivity to re-trigger it, so the forever loop must live in its
    // own body. A body with no internal timing control is a legal but
    // pathological zero-delay infinite loop (it hangs at time zero);
    // we lower it faithfully rather than reject it.
    const bool looping = activation == ir::Activation::Always && !infer && wait_events.empty();

    ir::Builder b(module_.pool);
    if (looping) {
        lower_forever(*body, b);
    } else if (iff_guard != nullptr) {
        // `always @(ev iff C) BODY` — wake on `ev`, run the body only when the
        // guard holds at event time (LRM 9.4.2.3). Gating the already-lowered
        // body with `if (C)` is observably identical: the body is
        // side-effect-free when skipped and the block re-subscribes each pass.
        emit_guarded_body(*body, *iff_guard, b, to_source_loc(sym.location));
    } else {
        lower_stmt(*body, b);
    }
    // Inferred sensitivity is read from the body after lowering, so any
    // block-local declarations it touched are already registered in sigmap_.
    if (infer) {
        collect_inferred_sensitivity(
          *body, comb ? SensitivityMode::Comb : SensitivityMode::ImplicitEvent, wait_events);
    }
    // always_comb / always_latch evaluate once at time zero (LRM 9.2.2.2), so
    // they settle from declaration-initialized values. An `always @*` or an
    // explicit `@(...)` list is a plain sensitivity list that runs only when a
    // listed signal changes, matching Verilog, so it is left non-self-starting.
    const bool self_start =
      sym.procedureKind == ProceduralBlockKind::AlwaysComb || sym.procedureKind == ProceduralBlockKind::AlwaysLatch;
    ir::Ir body_ir = b.finish();
    // A `final` block is zero-time (LRM 9.2.3): it must not suspend. slang
    // rejects a time-consuming statement in a `final` at elaboration, so this
    // guard is defense in depth (PRINCIPLES.md: never silently accept an
    // unimplemented case) — reached only if that check ever regresses.
    if (activation == ir::Activation::Final) {
        for (size_t i = 0; i < body_ir.inst_count(); ++i) {
            const ir::Inst::Tag t = body_ir.tag(static_cast<ir::Index>(i));
            if (t == ir::Inst::Tag::Delay || t == ir::Inst::Tag::Wait) {
                fail("timing control in a final block is not legal (LRM 9.2.3)");
            }
        }
    }
    // LRM 16.4.4: a REMOTE `disable` of this procedure's outermost scope has to
    // bump THIS procedure's deferred generation, so the target has to be able to
    // name the process it belongs to.
    if (proc_root_block_ != nullptr && disable_flushes_.contains(proc_root_block_) &&
        disable_roots_.contains(proc_root_block_)) {
        const auto id = disable_targets_.find(proc_root_block_);
        if (id != disable_targets_.end()) {
            if (module_.disable_flush_proc.size() < module_.disable_targets) {
                module_.disable_flush_proc.resize(module_.disable_targets, ir::Module::no_flush_proc);
            }
            module_.disable_flush_proc[id->second] = static_cast<uint32_t>(module_.processes().size());
        }
    }
    module_.add_process(ir::Process{ .activation  = activation,
                                     .wait_events = std::move(wait_events),
                                     .self_start  = self_start,
                                     // LRM 13.4.4: an `initial` / `always` procedure is a thread
                                     // whose calls may spawn background processes. A `final`
                                     // block is not one of the two, and its children could never
                                     // run anyway (the drain has ended by then).
                                     .spawn_root  = activation != ir::Activation::Final,
                                     .span        = span_of(sym),
                                     .ir          = std::move(body_ir),
                                     .scope       = cur_scope_id() });
}

void Lowerer::lower_stmt(const Statement& stmt, ir::Builder& b) {
    cur_range_ = stmt.sourceRange;
    switch (stmt.kind) {
        case StatementKind::List:
            for (const Statement* s : stmt.as<StatementList>().list) {
                lower_stmt(*s, b);
            }
            break;
        case StatementKind::Block: {
            const BlockStatement& blk = stmt.as<BlockStatement>();
            if (blk.blockKind != StatementBlockKind::Sequential) {
                lower_fork(blk, b);
                break;
            }
            if (blk.blockSymbol == nullptr || blk.blockSymbol->name.empty()) {
                lower_stmt(blk.body, b);
                break;
            }
            // A block some `disable` names becomes a NamedBlock region, so the
            // backend has an end block to branch to; every other named block
            // stays flattened, its name reaching only %m. A block named from
            // OUTSIDE additionally carries the occupant bookkeeping the remote
            // form's force-wake needs.
            const auto     targeted = disable_targets_.find(blk.blockSymbol);
            const uint32_t label    = targeted != disable_targets_.end() ? targeted->second : kNoBlockLabel;
            named_blocks_.push_back({ blk.blockSymbol, label });
            if (label == kNoBlockLabel) {
                lower_stmt(blk.body, b);
            } else {
                b.open_region();
                lower_stmt(blk.body, b);
                const ir::ExtraSlice body = b.close_region();
                b.named_block(body, label, disable_remote_.contains(blk.blockSymbol), loc(stmt));
            }
            named_blocks_.pop_back();
            break;
        }
        case StatementKind::Timed: {
            // A timing control nested inside a process body. A delay (#n) lowers
            // to a Delay instruction (suspend + reschedule on the timing wheel);
            // an event wait (@(edge sig)) lowers to a Wait instruction (suspend
            // on the signal's wait queue). Either way the body underneath runs
            // once the process resumes. (A process's top-level @(edge)
            // sensitivity is consumed in lower_procedural_block, not here.)
            const TimedStatement& timed = stmt.as<TimedStatement>();
            if (timed.timing.kind == TimingControlKind::Delay) {
                const Expression& amount = timed.timing.as<DelayControl>().expr;
                b.unary(ir::Inst::Tag::Delay, scaled_delay(amount, b), loc(stmt));
            } else if (is_event_control(timed.timing)) {
                lower_event_wait(timed.timing, b, loc(stmt));
            } else if (timed.timing.kind == TimingControlKind::CycleDelay) {
                lower_cycle_delay(timed.timing.as<CycleDelayControl>(), b, loc(stmt));
            } else {
                fail("unsupported timing control inside a process body");
            }
            lower_stmt(timed.stmt, b);
            break;
        }
        case StatementKind::ExpressionStatement: {
            const Expression& e = stmt.as<ExpressionStatement>().expr;
            if (e.kind == ExpressionKind::Assignment) {
                lower_assign(e.as<AssignmentExpression>(), b);
            } else if (e.kind == ExpressionKind::Call) {
                lower_call(e.as<CallExpression>(), b);
            } else if (e.kind == ExpressionKind::UnaryOp) {
                // A bare `i++;` / `--i;` statement: bump and discard the value.
                lower_unary(e.as<UnaryExpression>(), b);
            } else if (e.kind == ExpressionKind::NewClass) {
                // A bare `super.new(...)` statement (LRM 8.15). Base construction
                // is emitted at the constructor's entry (build_function) from
                // getBaseConstructorCall(), which is order-equivalent (slang
                // requires super.new to be the first statement) and keeps the
                // extends-arg / ctor-formal scope right. The body statement is a
                // no-op marker.
                if (!e.as<slang::ast::NewClassExpression>().isSuperClass) {
                    fail("unsupported expression statement");
                }
            } else {
                fail("unsupported expression statement");
            }
            break;
        }
        case StatementKind::Conditional:
            lower_conditional(stmt.as<ConditionalStatement>(), b);
            break;
        case StatementKind::Empty:
            break;
        case StatementKind::VariableDeclaration:
            lower_var_decl(stmt.as<VariableDeclStatement>().symbol, b);
            break;
        case StatementKind::Case:
            lower_case(stmt.as<CaseStatement>(), b);
            break;
        case StatementKind::PatternCase:
            lower_pattern_case(stmt.as<slang::ast::PatternCaseStatement>(), b);
            break;
        case StatementKind::RandCase:
            lower_randcase(stmt.as<slang::ast::RandCaseStatement>(), b);
            break;
        case StatementKind::RandSequence:
            lower_randsequence(stmt.as<RandSequenceStatement>(), b);
            break;
        case StatementKind::ForLoop:
            lower_for(stmt.as<ForLoopStatement>(), b);
            break;
        case StatementKind::WhileLoop:
            lower_while(stmt.as<WhileLoopStatement>(), b);
            break;
        case StatementKind::ForeverLoop:
            lower_forever(stmt.as<ForeverLoopStatement>().body, b);
            break;
        case StatementKind::RepeatLoop:
            lower_repeat(stmt.as<RepeatLoopStatement>(), b);
            break;
        case StatementKind::DoWhileLoop:
            lower_do_while(stmt.as<DoWhileLoopStatement>(), b);
            break;
        case StatementKind::ForeachLoop:
            lower_foreach(stmt.as<ForeachLoopStatement>(), b);
            break;
        case StatementKind::ImmediateAssertion:
            lower_immediate_assertion(stmt.as<ImmediateAssertionStatement>(), b);
            break;
        case StatementKind::Break:
            lower_break(stmt, b);
            break;
        case StatementKind::Continue:
            b.continue_(loc(stmt));
            break;
        case StatementKind::Return:
            lower_return(stmt.as<ReturnStatement>(), b);
            break;
        case StatementKind::EventTrigger:
            lower_event_trigger(stmt.as<EventTriggerStatement>(), b);
            break;
        case StatementKind::Wait:
            lower_wait_stmt(stmt.as<WaitStatement>(), b);
            break;
        case StatementKind::ProceduralAssign:
            lower_proc_assign(stmt.as<ProceduralAssignStatement>(), b);
            break;
        case StatementKind::ProceduralDeassign:
            lower_proc_deassign(stmt.as<ProceduralDeassignStatement>(), b);
            break;
        case StatementKind::ConcurrentAssertion:
            lower_concurrent_assertion(stmt.as<slang::ast::ConcurrentAssertionStatement>(), b);
            break;
        case StatementKind::Disable:
            lower_disable(stmt.as<slang::ast::DisableStatement>(), b);
            break;
        case StatementKind::DisableFork:
            // LRM 9.6.3: end every descendant of the calling process, "not just
            // immediate children", including descendants of children that have
            // already terminated -- which the control-block tree gives by
            // re-parenting a completing thread's live children
            // (docs/design/process-class.md §5.3, §8). Exactly `kill` over the
            // subtree, with the caller itself spared.
            b.proc_ctl(ir::ProcOp::DisableFork, {}, loc(stmt));
            break;
        case StatementKind::WaitFork:
            // LRM 9.6.1: block until every descendant has completed. One pointer
            // test on the control-block tree, because a completing thread
            // re-parents its live children (docs/design/process-class.md §8).
            b.proc_wait(ir::ProcOp::WaitFork, {}, loc(stmt));
            break;
        case StatementKind::WaitOrder:
            lower_wait_order(stmt.as<slang::ast::WaitOrderStatement>(), b);
            break;
        default:
            fail("unsupported statement");
    }
}

// `disable <named block>` / `disable <task>` (LRM 9.6.2). slang has already
// refused every other target shape (diag::InvalidDisableTarget), so the symbol
// here is either a StatementBlockSymbol or a task.
//
// Two forms lower. The LEXICAL one — a block that encloses this statement,
// inside a single-activation process body — is a structured multi-level exit,
// a generalized `break`, and is what LRM examples 1, 2 and 5 are. The REMOTE
// one — a sibling block, or a block in another process — marks whatever
// activation is inside the target region and force-wakes it, so the abort runs
// in that activation's own frame at its own poll point. See
// docs/design/disable.md.
void Lowerer::lower_disable(const slang::ast::DisableStatement& s, ir::Builder& b) {
    const Symbol& target = *s.target.as<slang::ast::ArbitrarySymbolExpression>().symbol;

    const auto it = disable_targets_.find(&target);
    if (it == disable_targets_.end()) {
        fail("internal: disable target was not labelled in the Collect pass");
    }

    if (target.kind != SymbolKind::StatementBlock) {
        // A task target (LRM 9.6.2): every activation ends, including ones this
        // process never started, so the abort walks the task's activation list
        // rather than a single occupant slot.
        if (disable_hard_.contains(&target)) {
            // TODO: a task whose body can park where the abort cannot claim the
            // park -- a `#0` or runtime-valued delay, a time-consuming call it
            // makes, a fork, an `expect`. The nested-call case additionally needs
            // the callee's activation ended too (9.6.2 disables downward along
            // the enable chain), which is a second list walk this does not have.
            fail("disable of a task whose body can park on a #0, a time-consuming call, a "
                 "fork or an expect is not supported yet (the abort cannot claim that "
                 "park)");
        }
        b.disable_remote(it->second, loc(s));
        // LRM 9.6.2 example 4: a task disabling ITSELF. The activation running
        // this statement is one of the ones that has to end, and it is the only
        // one the force-wake above cannot reach -- it is running, not parked.
        if (fn_ctx_ != nullptr && fn_ctx_->sym == &target) {
            b.disable_abort(loc(s));
        }
        return;
    }
    bool     found = false;
    uint32_t label = kNoBlockLabel;
    for (size_t n = named_blocks_.size(); n > 0 && !found; --n) {
        if (named_blocks_[n - 1].sym == &target) {
            found = true;
            label = named_blocks_[n - 1].label;
        }
    }

    if (found && fn_ctx_ != nullptr) {
        // A block that encloses this statement inside a body that can have
        // several activations. A bare branch would end only the one running it,
        // and LRM 9.6.2 ends them all — so mark every activation on the list
        // first (the walk skips this one, which is running, not parked) and then
        // take the ordinary structured exit out of our own copy.
        b.disable_remote(it->second, loc(s));
    }
    if (!found) {
        // The remote form. It does not care where the DISABLER runs — it marks an
        // occupant and pushes a handle — so a task, function or action block may
        // carry one. What has to hold is on the target side.
        if (disable_hard_.contains(&target)) {
            // TODO: a remote target whose region can park behind something the
            // abort cannot claim — a `#0` or a runtime-valued delay (the inactive
            // batch has no per-entry claim), a time-consuming task call or a fork
            // (the process would be resumed a second time by that activation's
            // completion), or an `expect`. A `#delay` and an `@event` wait are
            // supported, as is a task call that consumes no time.
            fail("disable of a block whose body can park on a #0, a time-consuming task "
                 "call, a fork or an expect is not supported yet from outside the block "
                 "(the abort cannot claim that park)");
        }
        if (disable_roots_.contains(&target) && disable_flushes_.contains(&target)) {
            // LRM 16.4.4: this is the target procedure's OUTERMOST scope, so its
            // pending deferred reports are dropped. The bump names the target,
            // because the counter to move is the target procedure's, not ours.
            b.defer_flush(it->second, loc(s));
        }
        b.disable_remote(it->second, loc(s));
        return;
    }
    if (label == kNoBlockLabel) {
        fail("internal: disable target was not labelled in the Collect pass");
    }
    if (&target == proc_root_block_ && disable_flushes_.contains(&target)) {
        // LRM 16.4.4 / 16.14.6.4: disabling the OUTERMOST scope of a procedure
        // drops its pending deferred reports. Lexically that procedure IS this
        // one, so the generation counter to bump is this group's own — one
        // instruction, immediately before the exit.
        b.defer_flush(ir::kNoDisableTarget, loc(s));
    }

    b.disable(label, loc(s));
}

void Lowerer::lower_fork(const BlockStatement& blk, ir::Builder& b) {
    const ir::ForkKind kind = blk.blockKind == StatementBlockKind::JoinAny  ? ir::ForkKind::JoinAny
                            : blk.blockKind == StatementBlockKind::JoinNone ? ir::ForkKind::JoinNone
                                                                           : ir::ForkKind::Join;
    const char* kind_name = kind == ir::ForkKind::JoinAny    ? "fork..join_any"
                          : kind == ir::ForkKind::JoinNone   ? "fork..join_none"
                                                             : "fork..join";

    // The fork body's top-level statements split into TWO different things, and LRM
    // 9.3.2 is what separates them: the fork's own block_item_declarations, which
    // are initialized in the parent "before any processes are spawned", and the
    // remaining statements, each of which is one child process.
    //
    // A flat walk that made every top-level statement a child was wrong twice over.
    // The declared variable landed in that declaration-child's own frame, where its
    // siblings could not see it -- reported as `reference to unknown signal 'k'` for
    // the clause's own example -- and under join_any the declaration child completes
    // immediately, satisfying the join before any real child has run.
    std::vector<const Statement*>           children;
    std::vector<const VariableDeclStatement*> decls;
    for_each_fork_stmt(blk, [&](const Statement& s) {
        if (const auto* d = s.as_if<VariableDeclStatement>()) {
            decls.push_back(d);
            return;
        }
        children.push_back(&s);
    });
    if (children.empty() && decls.empty()) {
        return; // `fork join` with no children: a no-op.
    }

    // Inside a task or a function, a child body gets its OWN frame, so a
    // reference to the enclosing formals or automatics cannot resolve the way it
    // does in a process (where every parent variable is a module signal). Each
    // such variable lives in a GC box for the whole activation, and the child
    // receives the box HANDLE by value (docs/design/fork-join.md V4).
    //
    // One rule for all three join kinds. The predecessor passed a fat
    // `ArgDir::Ref` -- a raw pointer into the enclosing frame -- under `join`
    // only, and that had two independent limits, neither of them about the join
    // kind: the ref ABI could carry only a packed integral or a dynamic array, so
    // a string / real / unpacked / queue / handle capture was refused outright;
    // and a `kill` on a joining parent frees the frame under children that abort
    // later (docs/design/process-class.md §11), so even `join` did not really
    // guarantee the lifetime the pointer needed. The box has reference semantics,
    // which is what LRM 9.3.2 asks for (an enclosing automatic is ONE variable),
    // and a live coroutine frame is a conservative GC root, so it outlives the
    // parent activation with no refcount and no ownership surgery.
    const ir::SourceLoc             l = loc(blk);
    std::vector<const ValueSymbol*> captures;

    // The fork's own block_item_declarations, materialized IN THE PARENT before any
    // child is built or spawned, in declaration order (LRM 9.3.2: "initialized to
    // their initialization value expression whenever execution enters their scope
    // and before any processes are spawned").
    //
    // Declaration order is load-bearing rather than incidental: each box is
    // registered before its own initializer is lowered and before the next
    // declaration is reached, so `automatic int a = j; automatic int b = a + 1;`
    // resolves `a` through the box just written. Once the variable is parent-side
    // storage a child referencing it is an ORDINARY CAPTURE, which is why the rest
    // of this function needs nothing new for it.
    for (const VariableDeclStatement* d : decls) {
        if (d->symbol.lifetime != slang::ast::VariableLifetime::Automatic) {
            // A STATIC declaration is precisely the one that opts OUT of the
            // per-entry initialization above, so 9.3.2 and 6.21 do not disagree
            // here. 6.21 is explicit: the `static` keyword "shall be required when
            // an initialization value is specified as part of a static variable's
            // declaration to indicate the user's intent of executing that
            // initialization only once at the beginning of simulation". 9.3.2's
            // "whenever execution enters their scope" is the per-entry rule that
            // `automatic` selects -- which is what makes the clause's own example
            // spell it `automatic int k = j` -- while its "before any processes are
            // spawned" is a rule about WHERE, and lowering in the parent at the fork
            // satisfies that for either lifetime.
            //
            // So this is ordinary static storage in the block's own scope: a module
            // signal in a process body, a persistent signal in a subroutine
            // (declare_storage), with the initializer run once in PreInit. No box
            // and no capture -- shared storage already outlives every child, which
            // is why ForkCaptureDetector skips it -- and a named fork is a
            // hierarchical scope, so `blk.decl` stays reachable by name.
            lower_var_decl(d->symbol, b);
            continue;
        }
        install_fork_decl_box(d->symbol, b, l);
        captures.push_back(&d->symbol);
        // Ordinary declaration lowering, which routes the store through the box just
        // installed (value_lvalue) and so covers an aggregate, a field default and
        // the no-initializer type default (LRM Table 6-7) exactly as anywhere else.
        lower_var_decl(d->symbol, b);
    }

    if (fn_ctx_ != nullptr) {
        // LRM 13.4.4 allows `fork … join_none` in a function -- it schedules
        // processes and returns, consuming no time -- and slang enforces the rest
        // (`join` / `join_any` there are rejected as time-consuming, and inside a
        // join_none block it clears its own function flag so the children may hold
        // anything a task may). The other two kinds cannot reach here through the
        // front end; `is_task` is the real discriminator, so the fail stays.
        //
        // Which CALLERS may reach a spawning function is a whole-call-graph
        // question and is checked once, after lowering, by check_spawn_roots.
        if (!fn_ctx_->is_task && kind != ir::ForkKind::JoinNone) {
            fail(std::string(kind_name) + " inside a function is not supported");
        }
        // "the enclosing task's" would be wrong in a function, where the captures
        // that matter are its own formals -- which is exactly the shape of LRM
        // 13.4.4's own `watch_for_zero` example.
        const char* enclosing = fn_ctx_->is_task ? "task" : "function";
        ForkCaptureDetector d;
        d.locals = &fn_ctx_->localmap;
        ankerl::unordered_dense::set<const ValueSymbol*> boxed_syms;
        for (const auto& [sym, bx] : fn_ctx_->boxed) {
            boxed_syms.insert(sym);
        }
        d.boxed    = &boxed_syms;
        d.this_var = fn_ctx_->this_var;
        // Seeded with the fork's own declarations so the detector's dedupe keeps
        // them at the front (their box is already installed) while appending
        // everything else in first-reference order -- which IS the child's formal
        // order.
        d.captured = captures;
        for (const Statement* c : children) {
            c->visit(d);
        }
        // Every capture rides a GC box: installed at body entry for an enclosing
        // automatic (BoxCandidateScan, which ran over this fork before the frame was
        // laid out), or at the fork for one of the fork's own declarations. A miss is
        // therefore a lowering bug -- the two walks disagreed -- not a source-level
        // refusal, and it is reported as one.
        for (const ValueSymbol* sym : d.captured) {
            if (!fn_ctx_->boxed.contains(sym)) {
                fail(std::string(kind_name) + " child that reads the enclosing " + enclosing + "'s '" +
                     std::string(sym->name) + "' is not supported (internal: no capture box)");
            }
        }
        captures = std::move(d.captured);
        // No spawn-time-initialization guard any more: the fork's own declarations
        // are evaluated above, in the parent, before any child exists. A
        // declaration inside a child's `begin ... end` is a different block and
        // keeps the ordinary late path, which is what 9.3.2's own example asks for
        // ("automatic int m = j; // the value of m is 4").
    }

    if (children.empty()) {
        // Declarations but no child statement, e.g. `fork automatic int k = f(); join`.
        // Their initializers have run above -- LRM 9.3.2 initializes them on entry to
        // the scope, so a side-effecting initializer still happens -- and there is
        // nothing to spawn and nothing to join.
        return;
    }

    std::vector<ir::FunctionId> ids;
    ids.reserve(children.size());
    for (const Statement* c : children) {
        ids.push_back(build_fork_child(*c, captures, kind));
    }

    // The capture actuals, in the order the children took their formals: each is
    // the box HANDLE by value, an rvalue rather than the box's own home, so nothing
    // the child holds points into this activation. Evaluated here, in the ENCLOSING
    // frame (build_fork_child's FnScope is already unwound).
    std::vector<ir::Index> args;
    args.reserve(captures.size());
    for (const ValueSymbol* sym : captures) {
        args.push_back(capture_handle(*sym, b, l));
    }
    b.fork(ids, args, kind, l);
}

// Reduce a condition expression to its 1-bit truth value for `if`/`?:`.
// LRM §12.4: the condition is true if any bit is 1, false if all bits are 0,
// and ambiguous (X) otherwise. That is exactly reduction-OR: must = any bit
// definitely 1, may = any bit could be 1 (Z bits read as X via `may`). This
// also normalizes a multi-bit predicate to the 1-bit value `b.cond`/`b.select`
// require; without it the backend emits a mismatched-width icmp.
// A string condition is true iff non-empty: the LRM is silent, and slang's
// const-eval takes it as len != 0 (docs/LRM_NOTES.md, note 17).
// RedOr of the non-negative 2-state length is exactly len != 0, definite
// 0/1, so it also satisfies the backend's definite-1 loop rule.
ir::Index Lowerer::lower_predicate(const Expression& cond, ir::Builder& b) {
    const ir::SourceLoc l = loc(cond);
    if (is_string_expr(cond)) {
        const ir::Index len = b.unary(ir::Inst::Tag::StrLen, lower_expr(cond, b), l);
        return b.unary(ir::Inst::Tag::RedOr, len, l);
    }
    const ir::Index pred = lower_expr(cond, b);
    if (cond.type->isFloating()) {
        // A real condition is true iff nonzero — a definite 1-bit result
        // (reals carry no x/z).
        return b.binary(ir::Inst::Tag::FNe, pred, real_zero(cond, b, l), l);
    }
    return b.unary(ir::Inst::Tag::RedOr, pred, l);
}

// A loop condition (12.7) has the same truth value as an `if` condition
// (12.4): true iff any bit is definitely 1. Reduce to a 1-bit predicate with
// lower_predicate so a multi-bit or real condition is normalized here, not
// left to the backend (which would emit a width-mismatched compare on the
// native-width must plane, or route a real plane through must_of).
ir::Index Lowerer::lower_loop_cond(const Expression& cond, ir::Builder& b) {
    return lower_predicate(cond, b);
}

// Whether the if/else-if chain rooted at `c` ends in an explicit `else`
// (a non-`ConditionalStatement` at the bottom of the ifFalse spine). Used to
// decide the priority-if no-match report: an explicit else always matches, so
// a chain with one can never no-match. A `begin ... end`-nested inner `if` is
// a BlockStatement, so it correctly counts as a final else (LRM 12.4.2).
bool Lowerer::chain_has_final_else(const ConditionalStatement& c) const {
    const ConditionalStatement* cur = &c;
    while (cur->ifFalse != nullptr) {
        const auto* nested = cur->ifFalse->as_if<ConditionalStatement>();
        if (nested == nullptr) {
            return true;
        }
        cur = nested;
    }
    return false;
}

void Lowerer::lower_conditional(const ConditionalStatement& c, ir::Builder& b) {
    // unique / unique0 / priority on `if` add the LRM 12.4.2 violation checks;
    // execution (ordered first-match) is identical to a plain if.
    //   - priority / unique: no-match report when no condition is true and the
    //     chain has no explicit final else (kind 0). unique0 stays silent.
    //   - unique / unique0: overlap report when >1 condition is true (kind 1).
    using Check = slang::ast::UniquePriorityCheck;
    if (c.check == Check::Unique || c.check == Check::Unique0) {
        lower_unique_if(c, b);
        return;
    }
    // priority (or the plain None case, which sets no flag): the lazy chain
    // with a no-match report threaded to its terminal.
    const bool report_no_match = c.check == Check::Priority && !chain_has_final_else(c);
    lower_if_chain(
      c, report_no_match, report_no_match ? banner_at(c.sourceRange.start()) : std::string{}, loc(c), b);
}

// Eager lowering for unique / unique0 `if` (LRM 12.4.2). The else-if spine is
// collected into arms, every arm predicate is evaluated once up front (the
// LRM permits any-order eager evaluation for unique/unique0 and requires
// continuing past the first true condition to detect overlap), and the temps
// are reused for the overlap check and the first-match dispatch. Mirrors the
// case path (lower_case_unique) over if arms instead of case items.
void Lowerer::lower_unique_if(const ConditionalStatement& c, ir::Builder& b) {
    using Check = slang::ast::UniquePriorityCheck;

    // Walk the ifFalse spine, collecting each same-series arm. The spine ends
    // at a null ifFalse (no final else) or a non-continuation statement (a
    // plain else body, a begin-end block, or a nested new-series if) — that
    // trailing statement becomes the final else.
    std::vector<IfArm>          arms;
    const Statement*            final_else = nullptr;
    const ConditionalStatement* cur        = &c;
    for (;;) {
        arms.push_back({ cur->conditions, &cur->ifTrue });
        if (cur->ifFalse == nullptr) {
            break;
        }
        const auto* nested = cur->ifFalse->as_if<ConditionalStatement>();
        if (nested == nullptr || nested->check != Check::None) {
            final_else = cur->ifFalse;
            break;
        }
        cur = nested;
    }

    // unique reports a no-match (no arm true, no final else); unique0 does not.
    const bool          no_match_report = c.check == Check::Unique && final_else == nullptr;
    const std::string   banner          = banner_at(c.sourceRange.start());
    const ir::SourceLoc rloc            = loc(c);

    std::vector<ir::Index> preds;
    preds.reserve(arms.size());
    for (const IfArm& arm : arms) {
        // Every arm's cond_predicate is evaluated once, up front — including any
        // pattern bindings and `&&&` filters. LRM 12.4.2 permits any-order eager
        // evaluation for unique/unique0 (overlap detection requires continuing
        // past the first true condition), and binding a non-selected arm's
        // pattern variables is harmless: each is a pure sub-value extraction read
        // only inside the arm its own predicate guards. Same discipline as
        // lower_pattern_case_checked.
        preds.push_back(lower_conditions(arm.conds, b, loc(*arm.conds.front().expr)));
    }

    emit_overlap_check(preds, banner, rloc, b);
    dispatch_if_arms(arms, 0, preds, final_else, no_match_report, banner, rloc, b);
}

// First-match dispatch for a unique/unique0 if, reusing the precomputed
// predicate temps: arm k's body runs when preds[k] is the first true one. The
// terminal (no arm true) runs the final else, else emits the no-match report
// when requested (unique, no final else).
void Lowerer::dispatch_if_arms(const std::vector<IfArm>&     arms,
                               size_t                        k,
                               const std::vector<ir::Index>& preds,
                               const Statement*              final_else,
                               bool                          no_match_report,
                               const std::string&            banner,
                               ir::SourceLoc                 rloc,
                               ir::Builder&                  b) {
    if (k == arms.size()) {
        if (final_else != nullptr) {
            lower_stmt(*final_else, b);
        } else if (no_match_report) {
            b.case_violation(banner, 0, rloc);
        }
        return;
    }
    b.open_region();
    lower_stmt(*arms[k].body, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    dispatch_if_arms(arms, k + 1, preds, final_else, no_match_report, banner, rloc, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(preds[k], then_region, else_region, loc(*arms[k].body));
}

// Lower an if / else-if chain. When `report_no_match` is set (a default-less
// priority chain), the flag is threaded down the ifFalse spine and the
// terminal else (reached only when no condition was true) emits a no-match
// report at the chain's `rloc`. With the flag clear this is exactly the plain
// nested-cond lowering (the nested ifFalse re-enters lower_conditional, which
// handles its own check), so non-priority ifs are unaffected.
void Lowerer::lower_if_chain(const ConditionalStatement& c,
                             bool                        report_no_match,
                             const std::string&          banner,
                             ir::SourceLoc               rloc,
                             ir::Builder&                b) {
    // `if (expr matches pattern)` (LRM 12.6.2), including `&&&` filters, which
    // slang models as additional conditions AND-ed together. The pattern's
    // bound variables are emitted into this region so the then-body sees them.
    const ir::Index pred = lower_conditions(c.conditions, b, loc(c));

    b.open_region();
    lower_stmt(c.ifTrue, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    if (c.ifFalse != nullptr) {
        const auto* nested = c.ifFalse->as_if<ConditionalStatement>();
        if (report_no_match && nested != nullptr && nested->check == slang::ast::UniquePriorityCheck::None) {
            // Continue the same priority series, carrying the flag to its end.
            lower_if_chain(*nested, report_no_match, banner, rloc, b);
        } else {
            lower_stmt(*c.ifFalse, b);
        }
    } else if (report_no_match) {
        b.case_violation(banner, 0, rloc);
    }
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, loc(c));
}

// A local declaration statement: register the variable's storage (a static
// local's persistent Signal, a per-call frame Local, or a module Signal in a
// process) and, if it has an initializer, store it. `force_frame` marks a loop
// control variable, which is per-call regardless of the function's lifetime.
void Lowerer::lower_var_decl(const VariableSymbol& vs, ir::Builder& b, bool force_frame) {
    declare_storage(vs, force_frame);
    if (is_static_local(vs, force_frame) &&
        (vs.getInitializer() != nullptr || aggregate_has_field_defaults(vs.getType()))) {
        // A static local's initializer (or struct field-level default) is
        // evaluated ONCE before time zero (LRM 13.4.2), not on each call —
        // register it in the run-once PreInit process, exactly like a
        // module-level static variable (its storage is likewise a persistent
        // signal, declare_static_local). A static initializer is a constant
        // expression, so it needs no function frame; clear fn_ctx_ so a stray
        // non-constant reference fails loudly rather than mis-resolving to a
        // frame local.
        const Restore<FnCtx*> no_frame(fn_ctx_, nullptr);
        lower_var_initializer(vs);
        return;
    }
    // An AUTOMATIC declaration is initialized on every entry to its block (LRM
    // 6.21), which is exactly where this store lands: lower_var_decl runs at the
    // declaration's own position in the statement list. With no initializer of its
    // own that means the type's default (Table 6-7) -- without it the storage kept
    // the previous entry's value, so a re-entered block behaved as if the variable
    // were static.
    const bool automatic = vs.lifetime == slang::ast::VariableLifetime::Automatic || force_frame;
    if (const Expression* init = vs.getInitializer()) {
        const ir::SourceLoc l = loc(*init);
        if (is_aggregate(vs.getType()) || is_unpacked_container(vs.getType())) {
            emit_struct_store(value_lvalue(vs, b, l), lower_type(vs.getType()), *init, ir::Inst::Tag::Store, b, l);
        } else {
            const ir::Index dst = value_lvalue(vs, b, l);
            const ir::Index rhs = lower_expr(*init, b);
            b.binary(ir::Inst::Tag::Store, dst, rhs, l);
        }
        return;
    }
    const ir::SourceLoc l = to_source_loc(vs.location);
    // An event is not a value: its trigger state is separate static per-event
    // state, and its 1-bit placeholder slot is never read (docs/design/
    // named-events.md), so there is nothing to reset.
    if (automatic) {
        emit_default_init(value_lvalue(vs, b, l), lower_type(vs.getType()), b, l);
    }
    if (aggregate_has_field_defaults(vs.getType())) {
        // The struct type declares field-level defaults (LRM 7.2 / 8.8), which
        // override the type defaults just written.
        emit_var_field_defaults(vs, value_lvalue(vs, b, l), b, l);
    }
}

// Write a variable's TYPE default (LRM 6.8, Table 6-7): x for a 4-state integral
// leaf, 0 for a 2-state one, 0.0 for a real, `""` for a string, `null` for a
// handle, an EMPTY container, and per member / element for an aggregate.
//
// Only an automatic declaration with no initializer needs it (LRM 6.21): static
// storage already powers on at the default, once, in the backend's init_state.
void Lowerer::emit_default_init(ir::Index lv, ir::Ref ty, ir::Builder& b, ir::SourceLoc l) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(ty));
    switch (t.kind) {
        case ir::Type::Kind::Queue:
            b.queue_delete(lv, lv, /*has_idx=*/false, ty, l);
            return;
        case ir::Type::Kind::DynArray:
            b.dyn_array_delete(lv, ty, l);
            return;
        case ir::Type::Kind::Assoc:
            b.assoc_delete(lv, lv, /*has_key=*/false, ty, l);
            return;
        case ir::Type::Kind::Event:
            // LRM Table 6-7: "New event". Every entry to the declaration binds a
            // FRESH synchronization object, so two concurrent activations of the
            // same task do not share one -- which is the whole reason an
            // automatic event cannot take a baked constant handle the way a
            // static one does.
            b.binary(ir::Inst::Tag::Store, lv, b.event_new(l), l);
            return;
        case ir::Type::Kind::String:
            b.binary(ir::Inst::Tag::Store, lv, b.constant(string_value_ref(""), l), l);
            return;
        case ir::Type::Kind::UnpackedArray: {
            // Unrolled per element, in declared-address order, like every other
            // fixed-array walk in the frontend.
            const uint32_t n = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
            for (uint32_t k = 0; k < n; ++k) {
                const int32_t coord =
                  t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
                const ir::Index idx =
                  b.constant(module_.pool.put_int(make_int_type(32, false, true),
                                                  static_cast<uint64_t>(static_cast<int64_t>(coord))),
                             l);
                emit_default_init(b.elem_select(lv, idx, ty, l), t.element, b, l);
            }
            return;
        }
        case ir::Type::Kind::Struct:
            if (!t.is_packed) {
                const auto fields = module_.pool.struct_fields(t.fields_id);
                for (uint32_t i = 0; i < fields.size(); ++i) {
                    emit_default_init(b.member_select(lv, i, ty, l), fields[i].type, b, l);
                }
                return;
            }
            break;
        default:
            break;
    }
    // A singular leaf: a packed value (a packed struct / union included), a real,
    // or a handle word (a class handle / chandle powers on null == 0).
    if (module_.pool.is_float(ty)) {
        b.binary(ir::Inst::Tag::Store, lv, b.constant(module_.pool.put_real(ty, 0.0), l), l);
        return;
    }
    const ir::Layout lay = module_.pool.layout(ty);
    const uint32_t   w   = lay.width == 0 ? 1 : lay.width;
    const ir::Ref dv = lay.domain == ir::Domain::Four ? svint_to_ref(slang::SVInt::createFillX(w, t.is_signed), ty)
                                                      : module_.pool.put_int(ty, 0);
    b.binary(ir::Inst::Tag::Store, lv, b.constant(dv, l), l);
}

// Apply a module-level (static) variable's declaration initializer (LRM 6.8)
// as a store in the run-once pre-init process. The store services no wait queue
// (the backend suppresses servicing for a PreInit process), so initialization
// generates no events and every Initial / Always process starts from the
// settled value, before any of them run. A variable with no initializer of
// its own, and whose struct type has no field-level defaults either, keeps
// its power-on default (X / 0), set by the backend's init_state.
void Lowerer::lower_var_initializer(const VariableSymbol& vs) {
    const Expression* init = vs.getInitializer();
    if (init == nullptr) {
        if (aggregate_has_field_defaults(vs.getType())) {
            if (!preinit_) {
                preinit_.emplace(module_.pool);
            }
            ir::Builder&        b = *preinit_;
            const ir::SourceLoc l = to_source_loc(vs.location);
            emit_var_field_defaults(vs, value_lvalue(vs, b, l), b, l);
        }
        return;
    }
    if (!preinit_) {
        preinit_.emplace(module_.pool);
    }
    ir::Builder&        b = *preinit_;
    const ir::SourceLoc l = loc(*init);
    if (is_aggregate(vs.getType()) || is_unpacked_container(vs.getType())) {
        emit_struct_store(value_lvalue(vs, b, l), lower_type(vs.getType()), *init, ir::Inst::Tag::Store, b, l);
    } else {
        const ir::Index dst = value_lvalue(vs, b, l);
        const ir::Index rhs = lower_expr(*init, b);
        b.binary(ir::Inst::Tag::Store, dst, rhs, l);
    }
}

// Emit the accumulated declaration initializers as one run-once PreInit
// process. Called once after the top instance and all its children are lowered;
// a no-op when the design has no variable initializers.
void Lowerer::finalize_preinit() {
    if (!preinit_) {
        return;
    }
    module_.add_process(ir::Process{ .activation = ir::Activation::PreInit, .ir = preinit_->finish() });
    preinit_.reset();
}

// for-loop. The initialization runs once in the enclosing region; the
// condition, body, and step become the loop's three regions. An omitted
// condition is a constant true (12.7.1); an X/Z condition exits (handled in
// the backend via the definite-1 rule).
void Lowerer::lower_for(const ForLoopStatement& f, ir::Builder& b) {
    // A `break` anywhere below binds to this loop (LRM 12.8), not to an
    // enclosing randsequence (18.17.6). Same note at every other loop.
    const Restore<uint32_t> depth(loop_depth_, loop_depth_ + 1);
    // Init: declared loop vars and assignment initializers are mutually
    // exclusive. Either is lowered in source order so a later init may read
    // an earlier one (12.7.1).
    for (const VariableSymbol* vs : f.loopVars) {
        lower_var_decl(*vs, b, true);
    }
    for (const Expression* init : f.initializers) {
        lower_for_step(*init, b);
    }

    b.open_region();
    const ir::Index      pred = f.stopExpr != nullptr ? lower_loop_cond(*f.stopExpr, b) : true_const(b, loc(f));
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_stmt(f.body, b);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    for (const Expression* s : f.steps) {
        lower_for_step(*s, b);
    }
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test*/ false, loc(f));
}

// while-loop: a for-loop with no init and no step.
void Lowerer::lower_while(const WhileLoopStatement& w, ir::Builder& b) {
    const Restore<uint32_t> depth(loop_depth_, loop_depth_ + 1);
    b.open_region();
    const ir::Index      pred = lower_loop_cond(w.cond, b);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_stmt(w.body, b);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::ExtraSlice step = b.close_region(); // empty

    b.loop(cond, pred, body, step, /*bottom_test*/ false, loc(w));
}

// do-while (12.7.1): like while, but the body runs once before the first
// predicate test. The bottom_test flag makes the backend branch entry ->
// body; `continue` still lands on the step block, which re-evaluates the
// condition (empty step -> header).
void Lowerer::lower_do_while(const DoWhileLoopStatement& d, ir::Builder& b) {
    const Restore<uint32_t> depth(loop_depth_, loop_depth_ + 1);
    b.open_region();
    const ir::Index      pred = lower_loop_cond(d.cond, b);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_stmt(d.body, b);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::ExtraSlice step = b.close_region(); // empty

    b.loop(cond, pred, body, step, /*bottom_test*/ true, loc(d));
}

// A forever loop around `body`: a constant-true Loop. Lowers both an
// explicit `forever` statement's body and the implicit forever wrapping a
// sensitivity-less `always` body (LRM 9.2.2), which has no separate AST
// node. The body's own timing control (`#`/`@`) suspends each iteration;
// without one this is the legal zero-delay infinite loop noted in
// lower_procedural_block.
void Lowerer::lower_forever(const Statement& body, ir::Builder& b) {
    const Restore<uint32_t> depth(loop_depth_, loop_depth_ + 1);
    b.open_region();
    const ir::Index      pred = true_const(b, loc(body));
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_stmt(body, b);
    const ir::ExtraSlice body_region = b.close_region();

    b.open_region();
    const ir::ExtraSlice step = b.close_region(); // empty

    b.loop(cond, pred, body_region, step, /*bottom_test*/ false, loc(body));
}

// repeat-loop (12.7.2): runs the body N times, where N is the count
// expression evaluated EXACTLY ONCE on entry. Lowers to the generic Loop with
// a hidden counter and a hidden bound: `for (counter = 0; counter < bound;
// counter++) body`. The count's own type (width + signedness) carries through
// so a negative signed count yields zero iterations (0 < negative is false),
// and an X/Z count yields zero iterations (the Lt predicate is non-definite-1,
// which the backend's definite-1 loop rule treats as exit).
//
// A real count is counted in a signed 64-bit type instead, rounded into it
// half away from zero (LRM 6.12.1's real -> integral conversion, which coerce
// ends in an FtoI): signed keeps 12.7.2's negative-is-zero rule (`repeat
// (-0.6)` rounds to -1 and runs the body zero times), and 64 bits is where
// FtoI saturates, so a huge real cannot wrap into a small positive count.
void Lowerer::lower_repeat(const RepeatLoopStatement& r, ir::Builder& b) {
    const Restore<uint32_t> depth(loop_depth_, loop_depth_ + 1);
    const ir::SourceLoc l = loc(r);
    const ir::Ref ty      = r.count.type->isFloating() ? make_int_type(64, false, true) : lower_type(*r.count.type);

    // Hidden bound holds the once-evaluated count; hidden counter starts at 0.
    // Both live in the enclosing region, like a for-loop's init, and both are
    // per-activation cells (a frame Local inside a subroutine) so a recursive
    // or concurrently-invoked task keeps its own count.
    const TempCell  bound     = make_temp_cell(ty, "repeat_bound");
    const TempCell  counter   = make_temp_cell(ty, "repeat_count");
    const ir::Index bound_lv  = bound.lv(b, l);
    const ir::Index bound_val = coerce(lower_expr(r.count, b), *r.count.type, ty, b, l);
    b.binary(ir::Inst::Tag::Store, bound_lv, bound_val, l);
    const ir::Index counter_lv = counter.lv(b, l);
    const ir::Index zero       = b.constant(module_.pool.put_int(ty, 0), l);
    b.binary(ir::Inst::Tag::Store, counter_lv, zero, l);

    b.open_region();
    const ir::Index      cur  = b.unary(ir::Inst::Tag::Load, counter.lv(b, l), l);
    const ir::Index      lim  = b.unary(ir::Inst::Tag::Load, bound.lv(b, l), l);
    const ir::Index      pred = b.binary(ir::Inst::Tag::Lt, cur, lim, l);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_stmt(r.body, b);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, counter.lv(b, l), l);
    const ir::Index one      = b.constant(module_.pool.put_int(ty, 1), l);
    const ir::Index next     = b.binary(ir::Inst::Tag::Add, step_cur, one, l);
    b.binary(ir::Inst::Tag::Store, counter.lv(b, l), next, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
}

// foreach-loop (12.7.3): nested loops over an array's declared dimensions.
// `loopDims` has one entry per bracketed slot the user wrote, in array-
// dimension order (leftmost = outermost, matching "outer loops correspond
// to lower cardinality indices"); a skipped slot (empty loop var) means
// "no iteration over that dimension" per the LRM -- not a loop that runs
// once, no loop at all, so it just advances to the next slot in the same
// region. Dimension bounds come straight from slang (`LoopDim::range`),
// valid for both packed and unpacked fixed-range dimensions alike, so no
// separate walk of Clotho's own (unpacked-only) array Type is needed --
// whatever indexing the body performs on the result is ordinary,
// pre-existing ElemSelect/BitSelect lowering, untouched by this loop nest.
void Lowerer::lower_foreach(const ForeachLoopStatement& f, ir::Builder& b) {
    const Restore<uint32_t> depth(loop_depth_, loop_depth_ + 1);
    lower_foreach_dim(f, 0, b);
}

// The container dimension `dim` iterates: the array the `foreach` names, with one
// dimension peeled per preceding slot. Peeled through the interned IR type, whose
// kinds are exact — slang answers isDynamicallySizedArray() for an associative
// array too, so peeling by slang predicate measured a map as a dynamic array. A
// skipped slot (`a[,j]`) peels as well: it names a dimension without iterating it.
ir::Ref Lowerer::foreach_dim_ref(const ForeachLoopStatement& f, size_t dim) {
    ir::Ref t = lower_type(*f.arrayRef.type);
    for (size_t k = 0; k < dim; ++k) {
        const ir::Type& ty = std::get<ir::Type>(module_.pool.get(t));
        if (ty.element == ir::Ref::Invalid) {
            return t;
        }
        t = ty.element;
    }
    return t;
}

// An lvalue for dimension `dim`'s container: the array the `foreach` names, with
// one element select per already-iterated dimension, each keyed by that
// dimension's live loop variable. `a[k, i]` over an assoc of queues therefore
// measures and indexes `a[k]`, not `a`.
//
// Re-emitted on demand rather than cached, because a loop's cond / body / step are
// three separate instruction regions and each needs its own leaf — the same reason
// TempCell::lv re-emits. Read-only: measuring a container never creates an entry
// (LRM 7.8.6), and the body's own accesses carry their own direction.
ir::Index Lowerer::foreach_base_lv(const ForeachLoopStatement& f, size_t dim, ir::Builder& b, ir::SourceLoc l) {
    ir::Index base = lower_lvalue(f.arrayRef, b);
    for (size_t k = 0; k < dim; ++k) {
        const ir::Ref   cref = foreach_dim_ref(f, k);
        const ir::Type& cty  = std::get<ir::Type>(module_.pool.get(cref));
        const ForeachLoopStatement::LoopDim& d = f.loopDims[k];
        if (d.loopVar == nullptr) {
            // `foreach (a[, i])` where dimension k is runtime-sized: there is no
            // loop variable, so no entry to descend into and no per-entry bound to
            // read. A fixed dimension needs no descent (its range is declared),
            // which is why only the nested runtime case reaches this.
            // TODO: iterate every entry of a skipped runtime dimension implicitly.
            fail("foreach over a runtime-sized dimension whose outer dimension is "
                 "skipped is not supported");
        }
        const ir::Index iter = b.unary(ir::Inst::Tag::Load, value_lvalue(*d.loopVar, b, l), l);
        base = cty.kind == ir::Type::Kind::Assoc ? b.assoc_select(base, iter, cref, l)
                                                 : b.elem_select(base, iter, cref, l);
    }
    return base;
}

void Lowerer::lower_foreach_dim(const ForeachLoopStatement& f, size_t dim, ir::Builder& b) {
    if (dim == f.loopDims.size()) {
        lower_stmt(f.body, b);
        return;
    }
    const ForeachLoopStatement::LoopDim& d = f.loopDims[dim];
    if (d.loopVar == nullptr) {
        lower_foreach_dim(f, dim + 1, b);
        return;
    }

    const ir::SourceLoc l = loc(f);
    declare_storage(*d.loopVar, /*force_frame*/ true);
    const ir::Ref   ty      = lower_type(d.loopVar->getType());
    const ir::Index iter_lv = value_lvalue(*d.loopVar, b, l);

    if (!d.range.has_value()) {
        // A runtime-sized dimension: snapshot the length once and iterate
        // 0..len-1 (0-based, matching StrGetc and element addressing), or walk the
        // keys for an assoc.
        //
        // The container measured is THIS dimension's, not `f.arrayRef`: for a
        // nested runtime dimension (`foreach (a[k, i])` over an assoc of queues)
        // the bound is the length of the entry the outer dimension is currently
        // sitting on. Measuring `f.arrayRef` for every dimension re-walked the
        // OUTER container with the inner loop variable, which for an assoc of
        // queues bound the inner index to a key.
        const ir::Ref cref = foreach_dim_ref(f, dim);
        if (ir::types::is_string(module_.pool, cref)) {
            const ir::Index len =
              b.unary(ir::Inst::Tag::StrLen,
                      b.unary(ir::Inst::Tag::Load, foreach_base_lv(f, dim, b, l), l), l);
            lower_foreach_runtime_dim(f, dim, ty, iter_lv, len, b, l);
            return;
        }
        if (ir::types::is_assoc(module_.pool, cref)) {
            lower_foreach_assoc_dim(f, dim, ty, iter_lv, b, l);
            return;
        }
        if (ir::types::is_queue(module_.pool, cref)) {
            const ir::Index len = b.queue_size(foreach_base_lv(f, dim, b, l), cref, l);
            lower_foreach_runtime_dim(f, dim, ty, iter_lv, len, b, l);
            return;
        }
        if (ir::types::is_dynarr(module_.pool, cref)) {
            const ir::Index len = b.dyn_array_size(foreach_base_lv(f, dim, b, l), cref, l);
            lower_foreach_runtime_dim(f, dim, ty, iter_lv, len, b, l);
            return;
        }
        fail("foreach over an unsupported runtime-sized dimension");
    }

    // Fixed-range dimension (packed or unpacked): iterate the declared
    // range directly, ascending or descending as declared (LRM's own
    // example iterates a `[5:1]` dimension 5 down to 1). The range is an
    // elaboration-time constant, so unlike a runtime count there is
    // nothing to snapshot.
    const slang::ConstantRange r         = *d.range;
    const bool                 ascending = r.left <= r.right;
    b.binary(ir::Inst::Tag::Store,
             iter_lv,
             b.constant(module_.pool.put_int(ty, static_cast<uint64_t>(static_cast<uint32_t>(r.left))), l),
             l);

    b.open_region();
    const ir::Index cur = b.unary(ir::Inst::Tag::Load, iter_lv, l);
    const ir::Index lim =
      b.constant(module_.pool.put_int(ty, static_cast<uint64_t>(static_cast<uint32_t>(r.right))), l);
    const ir::Index      pred = b.binary(ascending ? ir::Inst::Tag::Le : ir::Inst::Tag::Ge, cur, lim, l);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_foreach_dim(f, dim + 1, b);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, iter_lv, l);
    const ir::Index one      = b.constant(module_.pool.put_int(ty, 1), l);
    const ir::Index next     = b.binary(ascending ? ir::Inst::Tag::Add : ir::Inst::Tag::Sub, step_cur, one, l);
    b.binary(ir::Inst::Tag::Store, iter_lv, next, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
}

// A runtime-sized foreach dimension (LRM 12.7.3: a `string` "is treated as a
// dynamic array of bytes indexed from 0 to N-1"; a dynamic array likewise).
// The bound `len` is snapshotted once on entry like a repeat-loop's count --
// resizing the container mid-loop is undefined behavior per the LRM (same
// clause), so one read is enough. 0-based, matching StrGetc / element
// addressing.
void Lowerer::lower_foreach_runtime_dim(const ForeachLoopStatement& f,
                                        size_t                      dim,
                                        ir::Ref                     ty,
                                        ir::Index                   iter_lv,
                                        ir::Index                   len,
                                        ir::Builder&                b,
                                        ir::SourceLoc               l) {
    const TempCell  bound     = make_temp_cell(ty, "foreach_bound");
    const ir::Index bound_lv  = bound.lv(b, l);
    const ir::Index bound_val = b.resize(len, ty, l);
    b.binary(ir::Inst::Tag::Store, bound_lv, bound_val, l);
    b.binary(ir::Inst::Tag::Store, iter_lv, b.constant(module_.pool.put_int(ty, 0), l), l);

    b.open_region();
    const ir::Index      cur  = b.unary(ir::Inst::Tag::Load, iter_lv, l);
    const ir::Index      lim  = b.unary(ir::Inst::Tag::Load, bound.lv(b, l), l);
    const ir::Index      pred = b.binary(ir::Inst::Tag::Lt, cur, lim, l);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_foreach_dim(f, dim + 1, b);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index step_cur = b.unary(ir::Inst::Tag::Load, iter_lv, l);
    const ir::Index one      = b.constant(module_.pool.put_int(ty, 1), l);
    b.binary(ir::Inst::Tag::Store, iter_lv, b.binary(ir::Inst::Tag::Add, step_cur, one, l), l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
}

// foreach over an associative-array dimension (LRM 7.9.3/12.7.3): iterate the
// keys in ascending order, binding the loop variable `iter_lv` to each key.
// `first(iter_lv)` seeds the min key; `next(iter_lv)` advances to the
// successor. Each returns 1 while a key remains, 0 at the end.
void Lowerer::lower_foreach_assoc_dim(const ForeachLoopStatement& f,
                                      size_t                      dim,
                                      ir::Ref /*ty*/,
                                      ir::Index     iter_lv,
                                      ir::Builder&  b,
                                      ir::SourceLoc l) {
    const ir::Ref  bit1        = make_int_type(1, false, false);
    const ir::Ref  aty         = foreach_dim_ref(f, dim);
    const TempCell ok          = make_temp_cell(bit1, "foreach_ok");
    using Step                 = ir::Builder::AssocStep;
    const ir::Index first_lv   = ok.lv(b, l);
    const ir::Index first_base = foreach_base_lv(f, dim, b, l);
    const ir::Index first_hit  = b.assoc_traverse(first_base, iter_lv, Step::First, aty, l);
    const ir::Index first_ok   = b.resize(first_hit, bit1, l);
    b.binary(ir::Inst::Tag::Store, first_lv, first_ok, l);

    b.open_region();
    const ir::Index      pred = b.unary(ir::Inst::Tag::Load, ok.lv(b, l), l);
    const ir::ExtraSlice cond = b.close_region();

    b.open_region();
    lower_foreach_dim(f, dim + 1, b);
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::Index next_lv   = ok.lv(b, l);
    const ir::Index next_base = foreach_base_lv(f, dim, b, l);
    const ir::Index next_hit  = b.assoc_traverse(next_base, iter_lv, Step::Next, aty, l);
    const ir::Index next_ok   = b.resize(next_hit, bit1, l);
    b.binary(ir::Inst::Tag::Store, next_lv, next_ok, l);
    const ir::ExtraSlice step = b.close_region();

    b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
}

// A for_initialization assignment or a for_step (12.7.1, A.6.8): a simple
// assignment, or a pre/post increment/decrement. Compound assignments and
// function-call steps are not supported yet.
void Lowerer::lower_for_step(const Expression& e, ir::Builder& b) {
    if (e.kind == ExpressionKind::Assignment) {
        lower_assign(e.as<AssignmentExpression>(), b);
        return;
    }
    if (e.kind == ExpressionKind::UnaryOp) {
        // A step discards the result; ++/-- lower via lower_unary (which
        // rejects any other unary operator, none of which are legal steps).
        lower_unary(e.as<UnaryExpression>(), b);
        return;
    }
    fail("unsupported for-step expression (only assignment and ++/-- are supported)");
}

// `(* full_case *)` / `(* parallel_case *)`: a designer claim about the case
// that Clotho can test, because it has the item values and the LRM's matching
// rules in hand.
//
// These pragmas exist only to tell a synthesiser something the simulator
// cannot see, which is exactly why they cause RTL-vs-netlist mismatch, and
// Cummings' sharpest observation about them is that the divergence tracks the
// optimisation benefit: "The full_case and parallel_case directives are
// always most dangerous when they work!" So a lie here is a real defect, not
// a style opinion, and it is silent in simulation, this simulator included.
//
// Deliberately checked against a 2-state fullness rule rather than the
// simulation one: a full_case claim speaks about the hardware, where x and z are
// not values, so only 2-state values count toward covering the selector and an
// item's x/z bits count for nothing. So an item carrying x/z
// contributes NOTHING to coverage under `case`, which is what makes an
// all-x default no help to fullness.
//
// Only ever reports what it can prove. A non-constant item, an
// undecidably-wide selector or `case ... inside` all yield silence, never a
// guess -- the whole value of this check is that a finding is trustworthy.
// The case pragmas as bits, for the IR: bit 0 full_case, bit 1 parallel_case.
// An attribute with no value means 1 (LRM 5.12), and `= 0` withdraws the claim.
uint32_t Lowerer::case_pragma_bits(const CaseStatement& c) const {
    if (scope_ == nullptr) {
        return 0;
    }
    // `unique` / `priority` / `unique0` are the same two claims spelled as
    // keywords, and yosys's parser and sv2v map them this way.
    using Check   = slang::ast::UniquePriorityCheck;
    uint32_t bits = 0;
    if (c.check == Check::Priority || c.check == Check::Unique) {
        bits |= 1u; // full_case
    }
    if (c.check == Check::Unique0 || c.check == Check::Unique) {
        bits |= 2u; // parallel_case
    }
    for (const slang::ast::AttributeSymbol* a : scope_->getCompilation().getAttributes(c)) {
        if (!a->getValue().isTrue()) {
            continue;
        }
        if (a->name == "full_case") {
            bits |= 1u;
        } else if (a->name == "parallel_case") {
            bits |= 2u;
        }
    }

    return bits;
}

void Lowerer::check_case_pragmas(const CaseStatement& c) {
    if (scope_ == nullptr) {
        return;
    }
    // Attribute lookup first: almost no case carries these, and bailing here
    // keeps the coverage expansion off the common path entirely.
    bool want_full     = false;
    bool want_parallel = false;
    for (const slang::ast::AttributeSymbol* a : scope_->getCompilation().getAttributes(c)) {
        // An attribute with no value means 1 (LRM 5.12), so `(* full_case *)`
        // and `(* full_case = 1 *)` are the same claim -- and `= 0` withdraws
        // it, which must not be reported against.
        if (!a->getValue().isTrue()) {
            continue;
        }
        if (a->name == "full_case") {
            want_full = true;
        } else if (a->name == "parallel_case") {
            want_parallel = true;
        }
    }
    if (!want_full && !want_parallel) {
        return;
    }
    // `inside` matching admits ranges and set membership, whose coverage is a
    // different computation; not attempted rather than approximated.
    if (c.condition == CaseStatementCondition::Inside) {
        return;
    }
    if (!c.expr.type->isIntegral()) {
        return;
    }
    uint32_t width = c.expr.type->getBitWidth();
    for (const CaseStatement::ItemGroup& item : c.items) {
        for (const Expression* e : item.expressions) {
            if (!e->type->isIntegral()) {
                return;
            }
            width = std::max(width, e->type->getBitWidth());
        }
    }
    // The value space is enumerated directly, so cap it. 16 bits is 65536
    // patterns, which covers the selectors these pragmas actually appear on
    // (state registers, one-hot vectors, opcode fields); a wider one is
    // reported on by nothing rather than approximated.
    constexpr uint32_t max_width = 16;
    if (width == 0 || width > max_width) {
        return;
    }
    const size_t space = size_t{ 1 } << width;

    // Per value: how many items match it. Saturated at 2 -- the only
    // questions are "none" (not full) and "more than one" (not parallel).
    std::vector<uint8_t>    hits(space, 0);
    slang::ast::ASTContext  actx(*scope_, slang::ast::LookupLocation::max);
    slang::ast::EvalContext ectx(actx);

    for (const CaseStatement::ItemGroup& item : c.items) {
        for (const Expression* e : item.expressions) {
            const slang::ConstantValue cv = e->eval(ectx);
            if (cv.bad() || !cv.isInteger()) {
                return; // Not decidable; say nothing.
            }
            const slang::SVInt v = cv.integer().resize(width);
            // Which bit positions this label leaves free. casez treats z (and
            // `?`) as don't-care, casex treats x as well; plain case treats
            // neither -- and an x/z bit there makes the label unmatchable by
            // the 2-state rule, so it covers nothing.
            std::vector<uint32_t> wild;
            bool                  dead = false;
            for (uint32_t bit = 0; bit < width; ++bit) {
                const slang::logic_t l = v[static_cast<int32_t>(bit)];
                if (!l.isUnknown()) {
                    continue;
                }
                const bool is_z = l.value == slang::logic_t::z.value;
                switch (c.condition) {
                    case CaseStatementCondition::WildcardXOrZ:
                        wild.push_back(bit);
                        break;
                    case CaseStatementCondition::WildcardJustZ:
                        if (is_z) {
                            wild.push_back(bit);
                        } else {
                            dead = true;
                        }
                        break;
                    default:
                        dead = true;
                        break;
                }
            }
            if (dead) {
                continue;
            }
            // Base pattern with every wildcard bit cleared, then every
            // assignment of the wildcards enumerated over it.
            size_t base = 0;
            for (uint32_t bit = 0; bit < width; ++bit) {
                const slang::logic_t l = v[static_cast<int32_t>(bit)];
                if (!l.isUnknown() && l.value != 0) {
                    base |= size_t{ 1 } << bit;
                }
            }
            const size_t combos = size_t{ 1 } << wild.size();
            for (size_t k = 0; k < combos; ++k) {
                size_t value = base;
                for (size_t w = 0; w < wild.size(); ++w) {
                    if ((k >> w) & 1U) {
                        value |= size_t{ 1 } << wild[w];
                    }
                }
                if (hits[value] < 2) {
                    ++hits[value];
                }
            }
        }
    }

    size_t uncovered  = 0;
    size_t overlapped = 0;
    for (const uint8_t h : hits) {
        uncovered += h == 0 ? 1 : 0;
        overlapped += h > 1 ? 1 : 0;
    }

    // A default arm makes the case full by construction, so `full_case` is
    // merely redundant there rather than untrue -- nothing to report.
    if (want_full && c.defaultCase == nullptr && uncovered != 0) {
        report(Severity::Warning,
               "full-case-not-full",
               "(* full_case *) on a case that is not full: " + std::to_string(uncovered) + " of " +
                 std::to_string(space) +
                 " selector values match no item, so synthesis may implement them as don't-care while "
                 "simulation holds the previous value",
               to_source_span(c.sourceRange));
    }
    if (want_parallel && overlapped != 0) {
        report(Severity::Warning,
               "parallel-case-overlap",
               "(* parallel_case *) on a case with overlapping items: " + std::to_string(overlapped) +
                 " selector values match more than one item, so synthesis may drop the priority "
                 "simulation gives them",
               to_source_span(c.sourceRange));
    }
}

// The width/state/signedness all case_expressions are compared at: the
// longest of the selector and every item label, four-state if any operand is,
// signed only if all are (IEEE 1800-2023 12.5).
Lowerer::CaseCompareType Lowerer::common_case_type(const CaseStatement& c) {
    // A real operand never reaches here: lower_case routes the whole
    // statement to lower_case_real, which compares without a common width.
    uint32_t width = c.expr.type->getBitWidth();
    bool     four  = c.expr.type->isFourState();
    bool     signd = c.expr.type->isSigned();
    for (const CaseStatement::ItemGroup& item : c.items) {
        for (const Expression* e : item.expressions) {
            width = std::max(width, e->type->getBitWidth());
            four  = four || e->type->isFourState();
            signd = signd && e->type->isSigned();
        }
    }

    return { make_int_type(width, four, signd), width, four };
}

// Whether a range bound is (possibly conversion-wrapped) the `$` literal.
// slang's binder wraps an UnboundedLiteral operand in an implicit Conversion
// to the range's common type, so the literal survives only as the
// innermost operand -- peel every Conversion layer to find it.
bool Lowerer::is_unbounded_bound(const Expression& e) {
    const Expression* cur = &e;
    while (cur->kind == ExpressionKind::Conversion) {
        cur = &cur->as<ConversionExpression>().operand();
    }
    return cur->kind == ExpressionKind::UnboundedLiteral;
}

// Common compare type for `case ... inside` (12.5.4). As common_case_type,
// but a range_list member may be a [lo:hi] ValueRangeExpression (itself
// typed void) instead of a plain expression, so its bound types fold in
// place of the range's own type. Tolerance ranges (`+/-`, `+%-`) and `$`
// bounds guard loudly here (TODO: LRM 11.4.13 defines both, but neither
// is exercised by ranged case-inside labels yet).
Lowerer::CaseCompareType Lowerer::common_inside_type(const CaseStatement& c) {
    uint32_t width = c.expr.type->getBitWidth();
    bool     four  = c.expr.type->isFourState();
    bool     signd = c.expr.type->isSigned();
    auto     fold  = [&](const Expression& e) {
        // Real operands are routed away in lower_case (lower_case_real).
        width = std::max(width, e.type->getBitWidth());
        four  = four || e.type->isFourState();
        signd = signd && e.type->isSigned();
    };
    for (const CaseStatement::ItemGroup& item : c.items) {
        for (const Expression* e : item.expressions) {
            if (e->kind != ExpressionKind::ValueRange) {
                fold(*e);
                continue;
            }
            const auto& vr = e->as<ValueRangeExpression>();
            if (vr.rangeKind != ValueRangeKind::Simple) {
                // TODO: absolute/relative tolerance ranges (LRM 11.4.13).
                fail("case ... inside tolerance ranges ('+/-' / '+%-') are not supported");
            }
            if (is_unbounded_bound(vr.left()) || is_unbounded_bound(vr.right())) {
                // TODO: `$` range bound (lowest/highest value of the selector's type).
                fail("case ... inside unbounded ('$') range bounds are not supported");
            }
            fold(vr.left());
            fold(vr.right());
        }
    }
    return { make_int_type(width, four, signd), width, four };
}

// Predicate for one case-inside range_list member (LRM 11.4.13 / 12.5.4).
// A plain expression uses wildcard equality (`==?`): the case_expression is
// the inside operator's definite left operand, so only the label's own x/z
// bits are wildcards (WildEq already matches this exactly). A [lo:hi] range
// tests inclusive membership with Ge/Le, ANDed -- an x/z bound or selector
// makes the comparison ambiguous, which Land propagates as x rather than a
// match, and b.cond already branches to else on anything but a definite 1.
ir::Index Lowerer::lower_inside_label_pred(const Expression& label, ir::Index sel, ir::Ref ty, ir::Builder& b) {
    if (label.kind == ExpressionKind::ValueRange) {
        const auto&     vr = label.as<ValueRangeExpression>();
        const ir::Index lo = b.resize(lower_expr(vr.left(), b), ty, loc(vr.left()));
        const ir::Index hi = b.resize(lower_expr(vr.right(), b), ty, loc(vr.right()));
        const ir::Index ge = b.binary(ir::Inst::Tag::Ge, sel, lo, loc(label));
        const ir::Index le = b.binary(ir::Inst::Tag::Le, sel, hi, loc(label));
        return b.binary(ir::Inst::Tag::Land, ge, le, loc(label));
    }
    const ir::Index val = b.resize(lower_expr(label, b), ty, loc(label));
    return b.binary(ir::Inst::Tag::WildEq, sel, val, loc(label));
}

// OR of a case-inside item's range_list member predicates (12.5.4: a
// case_item is matched when the inside operation returns 1'b1 for any of
// its members). No dedup: 11.4.13 explicitly allows repeated/overlapping
// members, so unlike case_dedup nothing is ever dropped here.
ir::Index Lowerer::lower_inside_item_pred(const CaseStatement::ItemGroup& item,
                                          ir::Index                       sel,
                                          ir::Ref                         ty,
                                          ir::Builder&                    b) {
    ir::Index pred = lower_inside_label_pred(*item.expressions[0], sel, ty, b);
    for (size_t j = 1; j < item.expressions.size(); j++) {
        const Expression* e   = item.expressions[j];
        const ir::Index   one = lower_inside_label_pred(*e, sel, ty, b);
        pred                  = b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e));
    }
    return pred;
}

// Lazy first-match chain for `case ... inside`, mirroring lower_case_item:
// item k's range_list predicate guards its body, with the rest of the chain
// (eventually the default) as the else branch, so labels not yet reached
// are evaluated lazily.
// Lazy first-match chain for `case ... inside`'s residual (range/wildcard)
// items, mirroring lower_case_item -- but first tries find_switch_run at
// every step, so a run of plain clean-constant singular labels between
// range items still jump-tables instead of chaining one at a time.
void Lowerer::lower_case_inside_item(const CaseStatement&                  c,
                                     size_t                                k,
                                     ir::Index                             sel,
                                     ir::Ref                               ty,
                                     uint32_t                              width,
                                     uint32_t                              key_width,
                                     const std::vector<std::vector<char>>& keep,
                                     const slang::ast::ASTContext&         astCtx,
                                     bool                                  report_viol,
                                     ir::Builder&                          b) {
    if (k == c.items.size()) {
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        } else if (report_viol) {
            b.case_violation(case_banner(c), 0, loc(c));
        }
        return;
    }

    if (std::optional<SwitchRun> run = find_switch_run(c, k, width, key_width, &keep, astCtx)) {
        if (run->arms.empty()) {
            lower_case_inside_item(c, k + run->len, sel, ty, width, key_width, keep, astCtx, report_viol, b);
            return;
        }
        const std::vector<ir::Builder::SwitchArmBuild> arms = lower_switch_arm_bodies(run->arms, b);
        b.open_region();
        lower_case_inside_item(c, k + run->len, sel, ty, width, key_width, keep, astCtx, report_viol, b);
        const ir::ExtraSlice default_region = b.close_region();
        // four-state is exactly `key_width != width`: the key encoding doubles
        // only for a 4-state selector.
        b.switch_(sel, arms, default_region, width, key_width != width ? 1u : 0u, case_pragma_bits(c), loc(c.expr));
        return;
    }

    const ir::Index pred = lower_inside_item_pred(c.items[k], sel, ty, b);

    b.open_region();
    lower_stmt(*c.items[k].stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    lower_case_inside_item(c, k + 1, sel, ty, width, key_width, keep, astCtx, report_viol, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, loc(*c.items[k].stmt));
}

// Eager overlap-checked lowering for unique/unique0 `case ... inside`
// (mirrors lower_case_unique): every item's predicate is computed once up
// front and shared by emit_overlap_check and dispatch_preds.
void Lowerer::lower_case_inside_unique(const CaseStatement&          c,
                                       ir::Index                     sel,
                                       ir::Ref                       ty,
                                       uint32_t                      width,
                                       uint32_t                      key_width,
                                       const slang::ast::ASTContext& astCtx,
                                       bool                          no_match_report,
                                       ir::Builder&                  b) {
    std::vector<ir::Index> preds;
    preds.reserve(c.items.size());
    for (const CaseStatement::ItemGroup& item : c.items) {
        preds.push_back(lower_inside_item_pred(item, sel, ty, b));
    }
    emit_overlap_check(preds, case_banner(c), loc(c), b);
    dispatch_preds(c, 0, sel, width, key_width, astCtx, preds, no_match_report, b);
}

// `case ... inside` (12.5.4): set-membership matching via the inside
// operator (11.4.13). A run of clean-constant singular labels jump-tables
// via find_switch_run (shared with plain/casex/casez case); a range_list
// member or the one-hot idiom's ElementSelect labels never form a run, so
// those keep the existing predicate chain unchanged.
void Lowerer::lower_case_inside(const CaseStatement& c, ir::Builder& b) {
    using Check = slang::ast::UniquePriorityCheck;
    const bool no_match_report =
      (c.check == Check::Priority || c.check == Check::Unique) && c.defaultCase == nullptr;
    const bool needs_overlap = c.check == Check::Unique || c.check == Check::Unique0;

    const CaseCompareType  ct        = common_inside_type(c);
    const ir::Ref          ty        = ct.ty;
    const uint32_t         key_width = ct.four ? 2 * ct.width : ct.width;
    const ir::Index        sel       = b.resize(lower_expr(c.expr, b), ty, loc(c.expr));
    slang::ast::ASTContext astCtx(*scope_, slang::ast::LookupLocation::max);

    if (needs_overlap) {
        lower_case_inside_unique(c, sel, ty, ct.width, key_width, astCtx, no_match_report, b);
        return;
    }
    const std::vector<std::vector<char>> keep = case_dedup(c, ct.width, astCtx);
    lower_case_inside_item(c, 0, sel, ty, ct.width, key_width, keep, astCtx, no_match_report, b);
}

// Whether any operand of a case statement -- the selector, an item label, or
// a `case ... inside` range bound -- is real or shortreal.
bool Lowerer::has_real_case_operand(const CaseStatement& c) {
    if (c.expr.type->isFloating()) {
        return true;
    }
    for (const CaseStatement::ItemGroup& item : c.items) {
        for (const Expression* e : item.expressions) {
            if (e->kind == ExpressionKind::ValueRange) {
                const auto& vr = e->as<ValueRangeExpression>();
                if (vr.left().type->isFloating() || vr.right().type->isFloating()) {
                    return true;
                }
                continue;
            }
            if (e->type->isFloating()) {
                return true;
            }
        }
    }
    return false;
}

// One case_item label against a real selector, both already at `real`.
//
// LRM 12.5 words case matching as a bit-for-bit `===` over a common width,
// and Table 11-1 bars `===` on a real operand -- so the standard defines the
// comparison only for integral operands. A real carries no x or z, which is
// the whole content of the 4-state matching rule, so `===` degenerates to
// `==` exactly as it does for a string (6.16.9, the same call clotho's string
// comparison lowering already makes). Clotho therefore matches with IEEE-754
// equality: `-0.0` matches `0.0`, and a NaN selector matches nothing and
// falls to `default` (docs/LRM_NOTES.md, note 19). iverilog 13 agrees. casez / casex
// are the same comparison: their do-not-care bits are a property of a
// 4-state vector, and iverilog treats a real casez / casex exactly as a
// plain case.
//
// An integral operand on either side converts to real first (`coerce`, i.e.
// ItoF), the ordinary mixed-operand rule of 11.8.1.
ir::Index Lowerer::real_case_label_pred(const Expression& label, ir::Index sel, ir::Ref rty, ir::Builder& b) {
    if (label.kind == ExpressionKind::ValueRange) {
        // `case ... inside` range member (12.5.4 / 11.4.13): membership is the
        // pair of relational comparisons, which Table 11-1 DOES define for
        // reals, so this arm needs no interpretation beyond 11.4.13's own.
        const auto& vr = label.as<ValueRangeExpression>();
        if (vr.rangeKind != ValueRangeKind::Simple) {
            // TODO: absolute/relative tolerance ranges (LRM 11.4.13), as for
            // the integral path in common_inside_type.
            fail("case ... inside tolerance ranges ('+/-' / '+%-') are not supported");
        }
        if (is_unbounded_bound(vr.left()) || is_unbounded_bound(vr.right())) {
            // TODO: `$` bounds on a real range (LRM 11.4.13), as for the
            // integral path.
            fail("case ... inside with a '$' range bound is not supported");
        }
        const ir::Index lo = coerce(lower_expr(vr.left(), b), *vr.left().type, rty, b, loc(vr.left()));
        const ir::Index hi = coerce(lower_expr(vr.right(), b), *vr.right().type, rty, b, loc(vr.right()));
        const ir::Index ge = b.binary(ir::Inst::Tag::FGe, sel, lo, loc(label));
        const ir::Index le = b.binary(ir::Inst::Tag::FLe, sel, hi, loc(label));
        return b.binary(ir::Inst::Tag::Land, ge, le, loc(label));
    }
    const ir::Index val = coerce(lower_expr(label, b), *label.type, rty, b, loc(label));
    return b.binary(ir::Inst::Tag::FEq, sel, val, loc(label));
}

// OR of one case_item's label predicates, as the integral paths build it.
ir::Index Lowerer::real_case_item_pred(const CaseStatement::ItemGroup& item,
                                       ir::Index                       sel,
                                       ir::Ref                         rty,
                                       ir::Builder&                    b) {
    ir::Index pred = real_case_label_pred(*item.expressions[0], sel, rty, b);
    for (size_t j = 1; j < item.expressions.size(); j++) {
        const Expression* e   = item.expressions[j];
        const ir::Index   one = real_case_label_pred(*e, sel, rty, b);
        pred                  = b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e));
    }
    return pred;
}

// -- `case` over a string selector --
//
// LRM AMBIGUITY, decided here. 12.5 words a `case` match as a bit-for-bit `===`
// between selector and label, over operands zero-extended to the widest. Neither
// half of that is defined for `string`: Table 11-1 bars `===` on a string exactly
// as it does on a real, and 6.16 makes `string` a dynamic-length type with no bits
// and no width to extend to. 6.16.9 defines `==` / `!=` (and the relational
// operators) over string content, and defines no `===`. So a string `case` is not
// *sanctioned* by 12.5 -- and it is not prohibited either: 12.5 puts no type
// restriction on the case expression, and slang binds it.
//
// Read as content equality (`==`, 6.16.9). It is the only reading that is both
// well defined and degenerates correctly: a string carries no x/z, which is the
// whole content of the 4-state rule, so `===` collapses onto `==` -- the same call
// the real path already makes (lower_case_real, TODO.md "case on a real
// selector"). The zero-extension half is dropped rather than reinterpreted: there
// is no width to extend to, and padding one side would contradict 6.16.9's
// content comparison. A string literal's NUL bytes are already dropped where the
// literal becomes a string (6.16), so `"\000A"` and `"A"` are one value here and
// match -- Verilator 5.050 agrees.
//
// No oracle: iverilog 13 does not compile a string `case` at all (it dies
// in `draw_eval_vec4` on an assertion), so per PRINCIPLES.md this is pinned to the
// LRM text above rather than to a simulator. Verilator 5.050 does accept it and
// agrees with content equality, which is corroboration and not authority.
//
// UVM is why this exists: `uvm_string_to_verbosity` and `uvm_string_to_action`
// (uvm_globals.svh) dispatch `+UVM_VERBOSITY=` and `+uvm_set_action` this way,
// and there are nine such statements in the parts of uvm-core a testbench reaches.
ir::Index Lowerer::string_case_label_pred(const Expression& label, ir::Index sel, ir::Builder& b) {
    if (label.kind == ExpressionKind::ValueRange) {
        // TODO: `case (s) inside ["a":"z"]` — 11.4.13 membership over a range is
        // the pair of relational comparisons, which 6.16.9 does define for strings
        // (lexicographic), so this is implementable; nothing reaches it yet.
        fail("case ... inside with a string range bound is not supported");
    }
    if (!is_string_expr(label)) {
        // Unreachable from source: slang rejects a mixed string case itself
        // ("case expression of type 'bit[7:0]' is not comparable to common type
        // 'string'"), because 12.5 needs one common type across selector and
        // labels. Kept because the reading below assumes both sides are strings,
        // and 6.16 does define string/packed conversion, so a future slang could
        // start admitting it.
        fail("a non-string label in a case statement over strings is not supported");
    }
    return b.binary(ir::Inst::Tag::Eq, sel, lower_expr(label, b), loc(label));
}

// OR of one case_item's label predicates, as every other case path builds it.
ir::Index Lowerer::string_case_item_pred(const CaseStatement::ItemGroup& item, ir::Index sel, ir::Builder& b) {
    ir::Index pred = string_case_label_pred(*item.expressions[0], sel, b);
    for (size_t j = 1; j < item.expressions.size(); j++) {
        const Expression* e   = item.expressions[j];
        const ir::Index   one = string_case_label_pred(*e, sel, b);
        pred                  = b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e));
    }
    return pred;
}

// Lazy first-match chain, the shape lower_case_real_item uses and for the same
// reason: item k's predicate is built in the enclosing else region, so a later
// label -- and any side effect in it -- is evaluated only if every earlier item
// missed (12.5).
void Lowerer::lower_case_string_item(const CaseStatement& c,
                                     size_t               k,
                                     ir::Index            sel,
                                     bool                 report_viol,
                                     ir::Builder&         b) {
    if (k == c.items.size()) {
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        } else if (report_viol) {
            b.case_violation(case_banner(c), 0, loc(c));
        }
        return;
    }
    const ir::Index pred = string_case_item_pred(c.items[k], sel, b);

    b.open_region();
    lower_stmt(*c.items[k].stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    lower_case_string_item(c, k + 1, sel, report_viol, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, loc(*c.items[k].stmt));
}

// `case` with a string selector. The selector is lowered ONCE (12.5: the case
// expression is evaluated once, before any label), and every label compares
// against that value.
//
// `(* full_case *)` / `(* parallel_case *)` are skipped for the reason the real
// path skips them: both enumerate the selector's integral value space, and a
// string does not have one. unique / unique0 / priority keep their 12.5.3 checks.
void Lowerer::lower_case_string(const CaseStatement& c, ir::Builder& b) {
    using Check = slang::ast::UniquePriorityCheck;
    if (c.condition == CaseStatementCondition::WildcardJustZ ||
        c.condition == CaseStatementCondition::WildcardXOrZ) {
        // Unreachable from source: slang rejects it itself ("invalid type 'string'
        // for casez expression"). Kept because the do-not-care positions of
        // `casez` / `casex` are bits of a 4-state vector (12.5.1) and a string has
        // none, so if it ever did arrive there would be nothing for the wildcard
        // to mean -- and silently demoting it to `case` would be a wrong answer
        // rather than a refusal.
        fail("casez / casex over strings is not supported");
    }
    const bool no_match_report =
      (c.check == Check::Priority || c.check == Check::Unique) && c.defaultCase == nullptr;
    const bool needs_overlap = c.check == Check::Unique || c.check == Check::Unique0;

    const ir::Index sel = lower_expr(c.expr, b);
    if (!needs_overlap) {
        lower_case_string_item(c, 0, sel, no_match_report, b);
        return;
    }
    // unique / unique0 evaluate every item's predicate once up front (12.5.3
    // permits it), share them with the overlap check, and dispatch off the same
    // temps -- the real path's arrangement, and `allow_switch=false` for the same
    // reason: there is no integral key to switch on.
    std::vector<ir::Index> preds;
    preds.reserve(c.items.size());
    for (const CaseStatement::ItemGroup& item : c.items) {
        preds.push_back(string_case_item_pred(item, sel, b));
    }
    emit_overlap_check(preds, case_banner(c), loc(c), b);
    slang::ast::ASTContext astCtx(*scope_, slang::ast::LookupLocation::max);
    dispatch_preds(c,
                   0,
                   sel,
                   /*width=*/0,
                   /*key_width=*/0,
                   astCtx,
                   preds,
                   no_match_report,
                   b,
                   /*allow_switch=*/false);
}

// Lazy first-match chain for a real case, mirroring lower_case_item without
// its jump-table attempt (a real has no integral key) and without case_dedup
// (which compares labels as SVInt constants). Item k's predicate is built in
// the enclosing else region, so a later label -- and any side effect in it --
// is evaluated only if every earlier item missed (12.5).
void Lowerer::lower_case_real_item(const CaseStatement& c,
                                   size_t               k,
                                   ir::Index            sel,
                                   ir::Ref              rty,
                                   bool                 report_viol,
                                   ir::Builder&         b) {
    if (k == c.items.size()) {
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        } else if (report_viol) {
            b.case_violation(case_banner(c), 0, loc(c));
        }
        return;
    }
    const ir::Index pred = real_case_item_pred(c.items[k], sel, rty, b);

    b.open_region();
    lower_stmt(*c.items[k].stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    lower_case_real_item(c, k + 1, sel, rty, report_viol, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, loc(*c.items[k].stmt));
}

// `case` / `casez` / `casex` / `case ... inside` with a real operand. The
// integral machinery above is packed-only end to end -- a common compare
// width, an SVInt dedup, a jump table keyed on the selector -- so a real
// statement takes its own comparison chain instead (see real_case_label_pred
// for the matching rule).
//
// The `(* full_case *)` / `(* parallel_case *)` analysis is skipped: it
// enumerates the selector's integral value space, which a real does not have.
// unique / unique0 / priority keep their LRM 12.5.3 violation checks, built
// from the same per-item predicates the integral eager path uses.
void Lowerer::lower_case_real(const CaseStatement& c, ir::Builder& b) {
    using Check = slang::ast::UniquePriorityCheck;
    const bool no_match_report =
      (c.check == Check::Priority || c.check == Check::Unique) && c.defaultCase == nullptr;
    const bool needs_overlap = c.check == Check::Unique || c.check == Check::Unique0;

    // Both sides compare at `real`: a shortreal widens exactly (LRM 6.12.2),
    // so the wider type is always the safe common one.
    const ir::Ref   rty = module_.pool.real_type;
    const ir::Index sel = coerce(lower_expr(c.expr, b), *c.expr.type, rty, b, loc(c.expr));

    if (!needs_overlap) {
        lower_case_real_item(c, 0, sel, rty, no_match_report, b);
        return;
    }
    // unique / unique0 evaluate every item's predicate once up front (12.5.3
    // permits it), share them with the overlap check, and dispatch off the
    // same temps.
    std::vector<ir::Index> preds;
    preds.reserve(c.items.size());
    for (const CaseStatement::ItemGroup& item : c.items) {
        preds.push_back(real_case_item_pred(item, sel, rty, b));
    }
    emit_overlap_check(preds, case_banner(c), loc(c), b);
    slang::ast::ASTContext astCtx(*scope_, slang::ast::LookupLocation::max);
    dispatch_preds(c,
                   0,
                   sel,
                   /*width=*/0,
                   /*key_width=*/0,
                   astCtx,
                   preds,
                   no_match_report,
                   b,
                   /*allow_switch=*/false);
}

// Priority-order case dedup (LRM 12.5). The first matching case_item wins,
// so a later label that always evaluates identically to an earlier one is
// dead -- its body can never run. A constant label's value is fixed, so a
// later exact duplicate (=== at the common compare width) is always dead,
// immune to any intervening side effect. Handles casez/casex too: an
// identical label yields an identical match predicate regardless of its
// wildcard bits.
//
// TODO: dedup non-constant duplicate labels (a repeated `k` / `a+b`). Only
// safe when the label and every label between the two occurrences are
// side-effect-free -- else an intervening f() or ++ could mutate the
// variables it reads, or a conforming sim must still run the dropped label's
// own effect (is_speculation_safe + a pure-label memo cleared at any impure
// label gates it). Dropped for now: rare, and the constant path is what the
// downstream jump-table lowering needs.
//
// Skipped for unique/priority checks: a unique/unique0 overlap is a
// reportable violation, so duplicate labels must survive for
// lower_case_unique's predicate sum to detect them.
//
// `width` is the statement's single common compare width (from
// common_case_type/common_inside_type) -- passed in rather than
// re-derived here so this pass and find_switch_run's per-run key
// extraction always agree on the same width as `sel`. A range_list
// member (`case ... inside`, itself typed void) is skipped before eval:
// it is never treated as a duplicate-constant candidate (a genuinely
// repeated *range* is a missed dead-code opportunity, not a correctness
// gap -- see find_switch_run, which never puts a range in a switch key
// either).
//
// Returns keep[k][j] = whether item k's label j survives.
std::vector<std::vector<char>> Lowerer::case_dedup(const CaseStatement&          c,
                                                   uint32_t                      width,
                                                   const slang::ast::ASTContext& astCtx) {
    std::vector<std::vector<char>> keep(c.items.size());
    for (size_t k = 0; k < c.items.size(); k++) {
        keep[k].assign(c.items[k].expressions.size(), 1);
    }
    if (c.check != slang::ast::UniquePriorityCheck::None) {
        return keep;
    }

    std::vector<slang::SVInt> seen_const;

    for (size_t k = 0; k < c.items.size(); k++) {
        const std::span<const Expression* const> exprs = c.items[k].expressions;
        for (size_t j = 0; j < exprs.size(); j++) {
            const Expression& e = *exprs[j];
            if (e.kind == ExpressionKind::ValueRange) {
                continue;
            }
            slang::ast::EvalContext    ev(astCtx);
            const slang::ConstantValue cv = e.eval(ev);
            if (!cv.isInteger()) {
                continue;
            }
            slang::SVInt v = cv.integer();
            if (v.getBitWidth() < width) {
                v = v.extend(width, e.type->isSigned());
            }
            bool dup = false;
            for (const slang::SVInt& s : seen_const) {
                if (exactlyEqual(s, v)) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                keep[k][j] = 0;
            } else {
                seen_const.push_back(std::move(v));
            }
        }
    }
    return keep;
}

// Try to consume a maximal run of switch-eligible items starting at index
// k (generalizes the old whole-statement-only build_switch_arms to a
// sub-range, so one range/wildcard/non-constant item no longer disables
// switch dispatch for the rest of the statement -- mirrors how rustc's
// match lowering groups a maximal run of SwitchInt-compatible arms and
// recurses into the "otherwise" branch for the rest).
//
// An item is eligible iff every label is a constant with no x/z bits
// ("clean") -- the backend keys a clean value on its bare bits
// ({may^must, must}), so this needs the packed key to fit 64 bits, checked
// once for the whole statement via `key_width` (not per-run: a statement
// too wide for a switch never forms any run at all, matching the old
// whole-statement gate). The run stops at the first ineligible item; a
// ValueRangeExpression label (`case ... inside`) always disqualifies its
// item, so it always ends a run.
//
// Clean-and-constant is NOT enough on its own: casex/casez (CasexEq/
// CasezEq, see lower_case_compare in ir/llvm/Ops.cpp) wildcard on the
// SELECTOR's own x/z bits too ("dc = a.z | b.z" etc., operand-symmetric),
// which no fixed compile-time switch key can express -- an x/z selector
// must still be able to match a clean casex/casez label, but a switch's
// densified key makes any x/z selector land outside every case value.
// WildEq (`case ... inside`'s singular labels) only wildcards the LABEL
// operand, so a clean Inside label degenerates exactly to plain equality
// and is switch-safe. So this is gated to Normal/Inside; casex/casez never
// form a run (every call returns nullopt immediately), leaving their
// existing chain-only lowering untouched.
//
// `keep`, when non-null (plain/priority dispatch), filters labels the
// single whole-statement case_dedup pass already proved dead -- this
// function never re-derives its own cross-item dedup in that mode, so a
// duplicate straddling two different runs (or a run and a residual item)
// is still resolved consistently by the one shared pass. When null
// (unique/unique0 dispatch, which skips case_dedup so every item's label
// survives for the overlap-check sum), a fresh set scoped to just this
// call dedups keys for THIS run's own switch validity only: a duplicate
// constant in an EARLIER run or residual item needs no cross-run
// tracking, because that earlier item is always reached first by the
// recursion regardless (dispatch skips a claimed key only because it
// dispatches first, not because of any bookkeeping here) -- the overlap
// itself is still reported by the separate, unconditional
// emit_overlap_check pass over every item's own predicate, which this
// function has no bearing on.
//
// Returns nullopt only when the run at k is empty (item k itself isn't
// eligible). A non-nullopt run may still have empty `arms` (every label
// in it deduped away, e.g. a run of items that all repeat an earlier
// item's constant) -- callers must skip switch emission and recurse
// directly at k + len in that case.
std::optional<Lowerer::SwitchRun> Lowerer::find_switch_run(const CaseStatement&                  c,
                                                           size_t                                k,
                                                           uint32_t                              width,
                                                           uint32_t                              key_width,
                                                           const std::vector<std::vector<char>>* keep,
                                                           const slang::ast::ASTContext&         astCtx) {
    if (key_width == 0 || key_width > 64 ||
        (c.condition != CaseStatementCondition::Normal && c.condition != CaseStatementCondition::Inside)) {
        return std::nullopt;
    }

    std::vector<SwitchArmPlan>             arms;
    ankerl::unordered_dense::set<uint64_t> local_seen; // only used when keep == nullptr.
    size_t                                 i = k;
    for (; i < c.items.size(); i++) {
        const CaseStatement::ItemGroup& item = c.items[i];
        std::vector<uint64_t>           keys;
        bool                            eligible = true;
        for (size_t j = 0; j < item.expressions.size(); j++) {
            const Expression* e = item.expressions[j];
            if (e->kind == ExpressionKind::ValueRange) {
                eligible = false;
                break;
            }
            if (keep != nullptr && !(*keep)[i][j]) {
                continue; // already proven dead by the whole-statement case_dedup.
            }
            slang::ast::EvalContext    ev(astCtx);
            const slang::ConstantValue cv = e->eval(ev);
            if (!cv.isInteger() || cv.integer().hasUnknown()) {
                eligible = false;
                break;
            }
            slang::SVInt v = cv.integer();
            if (v.getBitWidth() < width) {
                v = v.extend(width, e->type->isSigned());
            }
            const std::optional<uint64_t> u = v.as<uint64_t>();
            if (!u) {
                eligible = false;
                break;
            }
            if (keep != nullptr) {
                // case_dedup already guarantees this key is unique statement-wide.
                keys.push_back(*u);
            } else if (local_seen.insert(*u).second) {
                keys.push_back(*u);
            }
        }
        if (!eligible) {
            break;
        }
        if (!keys.empty()) {
            arms.push_back({ std::move(keys), item.stmt });
        }
    }
    if (i == k) {
        return std::nullopt;
    }
    return SwitchRun{ i - k, std::move(arms) };
}

// Lower a switch run's surviving arms to region-backed SwitchArmBuilds,
// shared by every chain flavor (Normal/casex/casez, case-inside,
// unique/unique0 dispatch) that consumes a find_switch_run result.
std::vector<ir::Builder::SwitchArmBuild> Lowerer::lower_switch_arm_bodies(const std::vector<SwitchArmPlan>& plan,
                                                                          ir::Builder&                      b) {
    std::vector<ir::Builder::SwitchArmBuild> arms;
    arms.reserve(plan.size());
    for (const SwitchArmPlan& ap : plan) {
        b.open_region();
        lower_stmt(*ap.body, b);
        const ir::ExtraSlice body = b.close_region();
        arms.push_back({ std::span<const uint64_t>(ap.keys), body });
    }
    return arms;
}

// Detect the one-hot reverse-case idiom `case (1'b1)` where every arm is a
// single-bit select `base[i]` of a common packed base with a constant index.
// First arm in source order whose bit is a definite 1 wins (LRM 12.5). When
// the arms' IR bit positions run monotonically -- ascending (lower Ctz) or
// descending (Clz) -- source-order priority equals lowest/highest-set-bit
// priority for ANY set-bit pattern, so it lowers to
// `switch (ctz/clz(base)) { i_k -> arm_k }`: a jump table keyed on the
// winning bit index (LLVM owns the table and permutes the arbitrary case
// values). x/z bits are not definite 1, so they never match, matching `===`.
//
// TODO(unique): `unique`/`unique0 case (1'b1)` asserts one-hot (<= 1 bit
// set), which makes ANY arm order safe (order is unobservable with one bit)
// -- accept non-monotonic orders there once the unique/unique0 overlap
// violation report lands (a TODO shared with the plain switch path).
// TODO(repack): a non-monotonic plain case is not handled -- source-order
// priority then differs from bit order under multiple simultaneous set bits.
// A cheap gather re-packing the selected bits into source order (bit k =
// base[i_k]) before Ctz would restore it; deferred (O(N) ops/eval, rare).
std::optional<Lowerer::OneHotPlan> Lowerer::build_onehot_plan(const CaseStatement& c) {
    if (c.condition != CaseStatementCondition::Normal || c.check != slang::ast::UniquePriorityCheck::None) {
        return std::nullopt;
    }
    // Selector must be exactly the 1-bit constant 1.
    {
        slang::ast::ASTContext     ctx(*scope_, slang::ast::LookupLocation::max);
        slang::ast::EvalContext    ev(ctx);
        const slang::ConstantValue cv = c.expr.eval(ev);
        if (!cv.isInteger() || cv.integer().getBitWidth() != 1 || cv.integer().hasUnknown() ||
            cv.integer().as<uint64_t>().value_or(0) != 1) {
            return std::nullopt;
        }
    }

    const slang::ast::ValueSymbol*                     base_sym   = nullptr;
    const Expression*                                  base_expr  = nullptr;
    uint32_t                                           base_width = 0;
    std::vector<std::pair<uint64_t, const Statement*>> arms;
    for (const CaseStatement::ItemGroup& item : c.items) {
        if (item.expressions.size() != 1) {
            return std::nullopt; // grouped labels: fallback (see TODO(repack)).
        }
        const Expression& e = *item.expressions[0];
        if (e.kind != ExpressionKind::ElementSelect) {
            return std::nullopt;
        }
        const ElementSelectExpression& es = e.as<ElementSelectExpression>();
        if (es.value().kind != ExpressionKind::NamedValue) {
            return std::nullopt;
        }
        const slang::ast::ValueSymbol& sym = es.value().as<NamedValueExpression>().symbol;
        if (base_sym == nullptr) {
            base_sym   = &sym;
            base_expr  = &es.value();
            base_width = es.value().type->getBitWidth();
            if (base_width == 0 || !es.value().type->isIntegral()) {
                return std::nullopt;
            }
        } else if (&sym != base_sym) {
            return std::nullopt; // arms must share one base signal.
        }
        if (!e.type->isIntegral() || e.type->getBitWidth() != 1) {
            return std::nullopt;
        }
        const std::optional<int32_t> decl = try_const_int(es.selector());
        if (!decl) {
            return std::nullopt;
        }
        const slang::ConstantRange range = packed_range_of(es.value());
        if (!range.containsPoint(*decl)) {
            return std::nullopt;
        }
        arms.push_back({ static_cast<uint64_t>(range.translateIndex(*decl)), item.stmt });
    }
    if (arms.size() < 2) {
        return std::nullopt;
    }

    // Monotonic in IR bit position: ascending -> Ctz, descending -> Clz.
    bool ascending = true, descending = true;
    for (size_t k = 1; k < arms.size(); k++) {
        ascending  = ascending && arms[k].first > arms[k - 1].first;
        descending = descending && arms[k].first < arms[k - 1].first;
    }
    if (!ascending && !descending) {
        return std::nullopt; // see TODO(unique) / TODO(repack).
    }
    return OneHotPlan{ base_expr, std::move(arms), !ascending, base_width };
}

void Lowerer::emit_onehot(const CaseStatement& c, const OneHotPlan& plan, ir::Builder& b) {
    const ir::Index base_val = lower_expr(*plan.base, b);
    ir::Index       selector = base_val;
    // Mask to the selected bits so a set base bit with no arm can't shadow a
    // higher-priority selected bit. Skipped when every base bit is an arm.
    if (plan.arms.size() != plan.base_width) {
        slang::SVInt       mask(plan.base_width, 0, false);
        const slang::SVInt one(plan.base_width, 1, false);
        for (const std::pair<uint64_t, const Statement*>& arm : plan.arms) {
            mask |= one.shl(static_cast<uint32_t>(arm.first));
        }
        const ir::Index mcst = b.constant(svint_to_ref(mask, lower_type(*plan.base->type)), loc(*plan.base));
        selector             = b.binary(ir::Inst::Tag::Band, base_val, mcst, loc(*plan.base));
    }
    const ir::Index key = b.unary(plan.descending ? ir::Inst::Tag::Clz : ir::Inst::Tag::Ctz, selector, loc(c.expr));

    std::vector<std::vector<uint64_t>>       keystore;
    std::vector<ir::Builder::SwitchArmBuild> arms;
    keystore.reserve(plan.arms.size());
    arms.reserve(plan.arms.size());
    for (const std::pair<uint64_t, const Statement*>& arm : plan.arms) {
        b.open_region();
        lower_stmt(*arm.second, b);
        const ir::ExtraSlice reg = b.close_region();
        keystore.push_back({ arm.first });
        arms.push_back({ std::span<const uint64_t>(keystore.back()), reg });
    }
    b.open_region();
    if (c.defaultCase != nullptr) {
        lower_stmt(*c.defaultCase, b);
    }
    const ir::ExtraSlice default_region = b.close_region();
    // key_width 0: the key here is a bit INDEX from ctz/clz, not the selector,
    // so its value space is not the thing fullness is asking about. A one-hot
    // case is full only if the selector is provably one-hot, which the switch
    // alone cannot say. Decline rather than answer the wrong question.
    b.switch_(key, arms, default_region, /*sel_width=*/0, /*sel_four=*/0, case_pragma_bits(c), loc(c.expr));
}

void Lowerer::lower_case(const CaseStatement& c, ir::Builder& b) {
    if (unwrap_conversion(c.expr).kind == ExpressionKind::TypeReference) {
        // type() case selector (LRM 6.23): a type reference is a compile-time
        // marker with no runtime value, so this resolves entirely at lowering
        // time via Type::isMatching, rather than through the packed-comparison
        // machinery below (which cannot represent a type-ref key). Not routed
        // through slang's own CaseStatement::getKnownBranch: that fold keys off
        // `c.expr`'s own (possibly Conversion-wrapped) ExpressionKind directly,
        // so a plain `case (type(x))` selector -- wrapped here just like any
        // other case selector -- would silently fail its TypeReference check.
        const slang::ast::Type& sel_ty = unwrap_conversion(c.expr).as<TypeReferenceExpression>().targetType;
        for (const auto& group : c.items) {
            for (const Expression* item : group.expressions) {
                const Expression& ue = unwrap_conversion(*item);
                if (ue.kind != ExpressionKind::TypeReference) {
                    fail("case statement over type() with a non-type() item is not supported");
                }
                if (ue.as<TypeReferenceExpression>().targetType.isMatching(sel_ty)) {
                    lower_stmt(*group.stmt, b);
                    return;
                }
            }
        }
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        }
        return;
    }
    if (is_string_expr(c.expr)) {
        // String selector: its own comparison chain, because everything below is
        // packed-only (a common compare width, an SVInt label dedup, a jump table
        // keyed on the selector). See lower_case_string for the LRM reading.
        lower_case_string(c, b);
        return;
    }
    if (has_real_case_operand(c)) {
        // Real selector or real label, `inside` included: its own comparison
        // chain (lower_case_real), because everything below is packed-only.
        lower_case_real(c, b);
        return;
    }
    if (c.condition == CaseStatementCondition::Inside) {
        lower_case_inside(c, b);
        return;
    }
    check_case_pragmas(c);
    ir::Inst::Tag cmp;
    switch (c.condition) {
        case CaseStatementCondition::Normal:
            cmp = ir::Inst::Tag::CaseEq;
            break;
        case CaseStatementCondition::WildcardJustZ:
            cmp = ir::Inst::Tag::CasezEq;
            break;
        case CaseStatementCondition::WildcardXOrZ:
            cmp = ir::Inst::Tag::CasexEq;
            break;
        default:
            fail("unsupported case condition"); // unreachable: Inside handled above.
    }
    // unique / unique0 / priority (c.check) add the LRM 12.5.3 violation
    // checks; first-match execution is conforming for all of them.
    //   - no-match (priority or unique, no default): kind-0 report. unique0
    //     does NOT report no-match; a default can never no-match.
    //   - overlap (unique / unique0, >1 item matches): kind-1 report,
    //     detected by emit_overlap_check summing every item's own predicate
    //     unconditionally -- independent of how dispatch is structured, so
    //     a switch run's own key dedup (see find_switch_run) never needs to
    //     abandon the switch just because a duplicate constant exists
    //     somewhere in the statement.
    using Check = slang::ast::UniquePriorityCheck;
    const bool no_match_report =
      (c.check == Check::Priority || c.check == Check::Unique) && c.defaultCase == nullptr;
    const bool needs_overlap = c.check == Check::Unique || c.check == Check::Unique0;

    // The one-hot reverse-case idiom `case (1'b1)` over base[i] selects lowers
    // to ctz/clz + jump table (check==None only; unique/priority keep the
    // chain, see build_onehot_plan's TODO(unique)).
    if (std::optional<OneHotPlan> oh = build_onehot_plan(c)) {
        emit_onehot(c, *oh, b);
        return;
    }

    const CaseCompareType ct        = common_case_type(c);
    const ir::Ref         ty        = ct.ty;
    const uint32_t        key_width = ct.four ? 2 * ct.width : ct.width;
    // The selector is evaluated exactly once, up front (12.5 / 12.5.3).
    const ir::Index        sel = b.resize(lower_expr(c.expr, b), ty, loc(c.expr));
    slang::ast::ASTContext astCtx(*scope_, slang::ast::LookupLocation::max);

    // unique / unique0 need the eager overlap check; priority / plain use the
    // lazy first-match chain. Both segment switch-eligible runs internally
    // (find_switch_run), so a whole-statement all-clean-constant case still
    // reproduces one jump table as the degenerate single-run case.
    if (needs_overlap) {
        lower_case_unique(c, sel, cmp, ty, ct.width, key_width, astCtx, no_match_report, b);
        return;
    }
    const std::vector<std::vector<char>> keep = case_dedup(c, ct.width, astCtx);
    lower_case_item(c, 0, sel, cmp, ty, ct.width, key_width, keep, astCtx, no_match_report, b);
}

// Eager lowering for unique / unique0 (LRM 12.5.3). Unlike the lazy
// first-match chain, every case_item predicate is evaluated once up front
// (the LRM permits any-order eager evaluation for unique/unique0: the
// case_expression is evaluated once before the items, items may be compared
// in any order, and side effects "may cause nondeterministic results"). The
// predicates are held in temps and reused for both the overlap check and the
// first-match dispatch, so labels are never re-evaluated.
//
// Overlap: sum the 1-bit predicates; > 1 match is a uniqueness violation
// (kind 1). The check is marked unlikely so the report stays off the hot
// path. First-match dispatch and the no-match report (unique only) reuse the
// temps and match the plain-case semantics exactly.
void Lowerer::lower_case_unique(const CaseStatement&          c,
                                ir::Index                     sel,
                                ir::Inst::Tag                 cmp,
                                ir::Ref                       ty,
                                uint32_t                      width,
                                uint32_t                      key_width,
                                const slang::ast::ASTContext& astCtx,
                                bool                          no_match_report,
                                ir::Builder&                  b) {
    const size_t           n = c.items.size();
    std::vector<ir::Index> preds;
    preds.reserve(n);
    for (const CaseStatement::ItemGroup& item : c.items) {
        // A case_item's predicate is the OR of (selector <cmp> label) over its
        // labels; every item has at least one label. No dedup (a duplicate is
        // a reportable overlap, not a dead label).
        ir::Index pred{};
        bool      have = false;
        for (const Expression* e : item.expressions) {
            const ir::Index val = b.resize(lower_expr(*e, b), ty, loc(*e));
            const ir::Index one = b.binary(cmp, sel, val, loc(*e));
            pred                = have ? b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e)) : one;
            have                = true;
        }
        preds.push_back(pred);
    }

    emit_overlap_check(preds, case_banner(c), loc(c), b);
    dispatch_preds(c, 0, sel, width, key_width, astCtx, preds, no_match_report, b);
}

// Emit the unique/unique0 overlap check (LRM 12.5.3 / 12.4.2) from precomputed
// 1-bit predicate temps: sum them and, on an unlikely branch, report a kind-1
// violation when more than one matched. The predicates are definite 2-state
// (=== / a reduced if-condition never yields x), so the zero-extended 32-bit
// sum is exact for any realistic arm count. Shared by the case and if paths.
void Lowerer::emit_overlap_check(const std::vector<ir::Index>& preds,
                                 const std::string&            banner,
                                 ir::SourceLoc                 rloc,
                                 ir::Builder&                  b) {
    const ir::Ref sumty = make_int_type(32, false, false);
    ir::Index     sum   = b.resize(preds[0], sumty, rloc);
    for (size_t k = 1; k < preds.size(); k++) {
        sum = b.binary(ir::Inst::Tag::Add, sum, b.resize(preds[k], sumty, rloc), rloc);
    }
    const ir::Index one_c   = b.constant(module_.pool.put_int(sumty, 1), rloc);
    const ir::Index overlap = b.binary(ir::Inst::Tag::Gt, sum, one_c, rloc);

    b.open_region();
    b.case_violation(banner, 1, rloc);
    const ir::ExtraSlice report_region = b.close_region();
    b.open_region();
    const ir::ExtraSlice empty_region = b.close_region();
    b.cond(overlap, report_region, empty_region, rloc, /*unlikely=*/true);
}

// First-match dispatch for a unique/unique0 case, reusing the precomputed
// predicate temps: item k's body runs when preds[k] is the first true one.
// The terminal else (no item matched) runs the default, else emits the
// no-match report when the caller requested it (unique, no default).
//
// Tries find_switch_run first, same as the plain/priority chain -- but
// its switch is built fresh from `sel` (a switch case value has to be the
// raw selector, not an equality-result boolean), never from `preds[k]`.
// `preds` remains used only by the caller's emit_overlap_check; passing
// `keep = nullptr` here means find_switch_run dedups a run's own keys
// locally (unique/unique0 skip the whole-statement case_dedup, since a
// dropped duplicate there would be a swallowed overlap violation) -- the
// overlap itself is still reported by the separate, already-computed
// `preds` sum regardless of which items a run's switch absorbs.
//
// `allow_switch` is false for a real selector, whose predicates are IEEE-754
// comparisons: there is no integral key for find_switch_run to build a jump
// table from, and `width` / `key_width` carry no meaning there.
void Lowerer::dispatch_preds(const CaseStatement&          c,
                             size_t                        k,
                             ir::Index                     sel,
                             uint32_t                      width,
                             uint32_t                      key_width,
                             const slang::ast::ASTContext& astCtx,
                             const std::vector<ir::Index>& preds,
                             bool                          no_match_report,
                             ir::Builder&                  b,
                             bool                          allow_switch) {
    if (k == c.items.size()) {
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        } else if (no_match_report) {
            b.case_violation(case_banner(c), 0, loc(c));
        }
        return;
    }

    if (std::optional<SwitchRun> run =
          allow_switch ? find_switch_run(c, k, width, key_width, /*keep=*/nullptr, astCtx) : std::nullopt) {
        if (run->arms.empty()) {
            dispatch_preds(c, k + run->len, sel, width, key_width, astCtx, preds, no_match_report, b, allow_switch);
            return;
        }
        const std::vector<ir::Builder::SwitchArmBuild> arms = lower_switch_arm_bodies(run->arms, b);
        b.open_region();
        dispatch_preds(c, k + run->len, sel, width, key_width, astCtx, preds, no_match_report, b, allow_switch);
        const ir::ExtraSlice default_region = b.close_region();
        // four-state is exactly `key_width != width`: the key encoding doubles
        // only for a 4-state selector.
        b.switch_(sel, arms, default_region, width, key_width != width ? 1u : 0u, case_pragma_bits(c), loc(c.expr));
        return;
    }

    b.open_region();
    lower_stmt(*c.items[k].stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    dispatch_preds(c, k + 1, sel, width, key_width, astCtx, preds, no_match_report, b, allow_switch);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(preds[k], then_region, else_region, loc(*c.items[k].stmt));
}

// Emit the if-else chain for items[k..] into the current region: item k's
// match test guards its body, with the rest of the chain (eventually the
// default) as the else branch. Items are tested in textual order and the
// first match wins; labels for items not yet reached are evaluated lazily,
// inside the else regions, so they run only until a match is found.
//
// `report_viol` is set for a default-less priority/unique case: the terminal
// else (reached only when no item matched) emits a no-match violation report
// (LRM 12.5.3, kind 0). It is mutually exclusive with a default (a
// default-less case has no default branch here). This is the lazy chain used
// for priority / plain checks; unique/unique0 use the eager lower_case_unique.
//
// Tries find_switch_run at every step first: a run of clean-constant labels
// jump-tables (generalizing the old whole-statement-only fast path to a
// sub-range), with a non-constant/x-z-bearing item (or, for casex/casez,
// any item at all -- see find_switch_run) falling through to the predicate
// test below and recursing at k+1 as before.
void Lowerer::lower_case_item(const CaseStatement&                  c,
                              size_t                                k,
                              ir::Index                             sel,
                              ir::Inst::Tag                         cmp,
                              ir::Ref                               ty,
                              uint32_t                              width,
                              uint32_t                              key_width,
                              const std::vector<std::vector<char>>& keep,
                              const slang::ast::ASTContext&         astCtx,
                              bool                                  report_viol,
                              ir::Builder&                          b) {
    if (k == c.items.size()) {
        if (c.defaultCase != nullptr) {
            lower_stmt(*c.defaultCase, b);
        } else if (report_viol) {
            b.case_violation(case_banner(c), 0, loc(c));
        }
        return;
    }

    if (std::optional<SwitchRun> run = find_switch_run(c, k, width, key_width, &keep, astCtx)) {
        if (run->arms.empty()) {
            lower_case_item(c, k + run->len, sel, cmp, ty, width, key_width, keep, astCtx, report_viol, b);
            return;
        }
        const std::vector<ir::Builder::SwitchArmBuild> arms = lower_switch_arm_bodies(run->arms, b);
        b.open_region();
        lower_case_item(c, k + run->len, sel, cmp, ty, width, key_width, keep, astCtx, report_viol, b);
        const ir::ExtraSlice default_region = b.close_region();
        // four-state is exactly `key_width != width`: the key encoding doubles
        // only for a 4-state selector.
        b.switch_(sel, arms, default_region, width, key_width != width ? 1u : 0u, case_pragma_bits(c), loc(c.expr));
        return;
    }

    const CaseStatement::ItemGroup& item = c.items[k];

    // Match predicate: OR of (selector <cmp> label) over the group's labels,
    // skipping labels dropped by case_dedup.
    ir::Index pred{};
    bool      have = false;
    for (size_t j = 0; j < item.expressions.size(); j++) {
        if (!keep[k][j]) {
            continue;
        }
        const Expression* e   = item.expressions[j];
        const ir::Index   val = b.resize(lower_expr(*e, b), ty, loc(*e));
        const ir::Index   one = b.binary(cmp, sel, val, loc(*e));
        pred                  = have ? b.binary(ir::Inst::Tag::Lor, pred, one, loc(*e)) : one;
        have                  = true;
    }
    if (!have) {
        // No surviving label (empty group, or every label deduped away): the
        // body is unreachable, so continue the chain in the current region.
        lower_case_item(c, k + 1, sel, cmp, ty, width, key_width, keep, astCtx, report_viol, b);
        return;
    }

    b.open_region();
    lower_stmt(*item.stmt, b);
    const ir::ExtraSlice then_region = b.close_region();

    b.open_region();
    lower_case_item(c, k + 1, sel, cmp, ty, width, key_width, keep, astCtx, report_viol, b);
    const ir::ExtraSlice else_region = b.close_region();

    b.cond(pred, then_region, else_region, loc(*item.stmt));
}

// `break` terminates the innermost enclosing loop (LRM 12.8). Inside a
// randsequence production code block with no loop around it, 18.17.6 makes it
// terminate the whole randsequence instead: raise the production's abort flag
// and return, which the caller observes after its call and repeats, all the
// way out of the block. The backend's Break arm assumes an enclosing loop and
// indexes loop_targets_.back() unconditionally, so this must never reach it.
void Lowerer::lower_break(const Statement& stmt, ir::Builder& b) {
    const ir::SourceLoc l = loc(stmt);
    if (loop_depth_ == 0 && fn_ctx_ != nullptr && fn_ctx_->in_randseq) {
        const ir::Ref   bit1 = make_int_type(1, false, false);
        const ir::Index dst  = b.local(fn_ctx_->randseq_abort, bit1, l);
        const ir::Index yes  = b.constant(module_.pool.put_int(bit1, 1), l);
        b.binary(ir::Inst::Tag::Store, dst, yes, l);
        b.ret_void(l);
        return;
    }
    b.break_(l);
}

void Lowerer::lower_return(const ReturnStatement& r, ir::Builder& b) {
    if (fn_ctx_ == nullptr) {
        fail("return statement outside of a function");
    }
    const ir::SourceLoc l = loc(r);
    if (fn_ctx_->in_randseq) {
        // Inside a production, `return` aborts THIS production and generation
        // continues with the next (LRM 18.17.6) -- which is exactly returning
        // from the production's own function. `return expr` first stores the
        // value into the result output formal (18.17.7); slang bound the
        // expression against the production's declared return type, so no
        // further conversion is implied beyond the width coercion below.
        if (r.expr != nullptr) {
            if (!fn_ctx_->randseq_has_result) {
                // slang emits MissingReturnValueProd for the converse; this
                // direction it leaves to the return type, which is void here.
                fail("return with a value in a void randsequence production");
            }
            const ir::Index slot = b.local(fn_ctx_->randseq_result, fn_ctx_->randseq_result_ty, l);
            if (is_handle_ref(fn_ctx_->randseq_result_ty)) {
                // A queue / dynamic array / associative array result: the same
                // deep-copy rules a whole-container assignment applies, reached
                // without an AssignmentExpression.
                emit_container_store(slot, fn_ctx_->randseq_result_ty, *r.expr, b, l);
            } else if (is_agg_abi_ref(fn_ctx_->randseq_result_ty)) {
                emit_agg_store(
                  slot, fn_ctx_->randseq_result_ty, unwrap_conversion(*r.expr), ir::Inst::Tag::Store, b, l);
            } else if (is_string_ref(fn_ctx_->randseq_result_ty)) {
                b.binary(ir::Inst::Tag::Store, slot, lower_expr(*r.expr, b), l);
            } else {
                b.binary(ir::Inst::Tag::Store,
                         slot,
                         coerce(lower_expr(*r.expr, b), *r.expr->type, fn_ctx_->randseq_result_ty, b, l),
                         l);
            }
        }
        if (fn_ctx_->randseq_has_stop) {
            // In a `rand join` operand's step body the function IS one unit, so
            // returning from it only ends that unit. Tell the driver the whole
            // production aborted, so it stops advancing this sequence.
            const ir::Ref   bit1 = make_int_type(1, false, false);
            const ir::Index dst  = b.local(fn_ctx_->randseq_stop, bit1, l);
            const ir::Index yes  = b.constant(module_.pool.put_int(bit1, 1), l);
            b.binary(ir::Inst::Tag::Store, dst, yes, l);
        }
        b.ret_void(l);
        return;
    }
    if (r.expr != nullptr) {
        if (!fn_ctx_->has_ret_local) {
            fail("return with a value in a void function");
        }
        // An aggregate / whole-array result's Ret operand is the returned
        // object's lvalue (the backend copies its storage out through the
        // return out-param).
        if (is_agg_abi_ref(fn_ctx_->ret_type)) {
            b.unary(ir::Inst::Tag::Ret, agg_return_operand(unwrap_conversion(*r.expr), b, l), l);
            return;
        }
        // A dynamic-array result is returned as its handle lvalue; the backend
        // deep-copies it into the caller's result handle (LRM 13.4 value return).
        if (is_handle_ref(fn_ctx_->ret_type)) {
            b.unary(ir::Inst::Tag::Ret, lower_lvalue(*r.expr, b), l);
            return;
        }
        // A string result needs no width resize (slang already converted).
        const ir::Index v = lower_expr(*r.expr, b);
        b.unary(ir::Inst::Tag::Ret,
                is_string_ref(fn_ctx_->ret_type) ? v : coerce(v, *r.expr->type, fn_ctx_->ret_type, b, l),
                l);
    } else if (fn_ctx_->has_ret_local) {
        // `return;` in a non-void function yields the current function-var value:
        // an aggregate / whole array / dynamic array yields its lvalue (copied
        // out by the backend), else the loaded value.
        if (is_agg_abi_ref(fn_ctx_->ret_type)) {
            b.unary(ir::Inst::Tag::Ret, ret_var_ref(*fn_ctx_, l, b), l);
            return;
        }
        const ir::Index rv = ret_var_ref(*fn_ctx_, l, b);
        b.unary(ir::Inst::Tag::Ret, is_handle_ref(fn_ctx_->ret_type) ? rv : b.unary(ir::Inst::Tag::Load, rv, l), l);
    } else {
        b.ret_void(l);
    }
}

} // namespace clotho::lower
