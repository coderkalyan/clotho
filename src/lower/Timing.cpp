// Timing controls: event and wait controls, triggers, and clocking blocks.

#include "lower/Lowerer.h"

namespace clotho::lower {

// True if `tc` is an @-style event control (the kinds usable as a process's
// sensitivity), as opposed to a delay (#) or other timing. Only these are
// consumed as sensitivity; everything else reaches lower_stmt's Timed case.
bool Lowerer::is_event_control(const TimingControl& tc) {
    switch (tc.kind) {
        case TimingControlKind::SignalEvent:
        case TimingControlKind::EventList:
        case TimingControlKind::ImplicitEvent:
            return true;
        default:
            return false;
    }
}

// Collect the signal events of a timing control into `out`, descending into
// event lists (@(posedge clk or a or negedge rst)). Each SignalEventControl
// contributes one wait event; an implicit (@*) or other control adds nothing.
// Returns the `iff` guard expression (LRM 9.4.2.3) when the control carries
// one, else null; the caller gates the wake on it. A guard is representable
// only on a single-event control: on a multi-event list the woken process
// cannot tell which event fired, so it cannot select the matching per-event
// guard (the shared-wake engine carries no waking-source id).
const Expression* Lowerer::collect_wait_events(const TimingControl& tc, std::vector<ir::WaitEvent>& out) {
    const Expression* iff      = nullptr;
    uint32_t          n_events = 0;
    collect_wait_events_impl(tc, out, iff, n_events);
    if (iff != nullptr && n_events > 1) {
        // TODO: per-event `iff` in a multi-event list needs waking-source
        // identification to pick the matching guard (or a synthesized
        // edge-and-guard net per event). Rare in practice.
        fail("iff qualifier on a multi-event control is not supported");
    }
    return iff;
}

// Recursive worker for collect_wait_events. Accumulates the single `iff`
// guard seen (last wins; the multi-event validity check is on n_events) and
// counts source SignalEvents so the caller can reject an unrepresentable
// guarded list.
void Lowerer::collect_wait_events_impl(const TimingControl&        tc,
                                       std::vector<ir::WaitEvent>& out,
                                       const Expression*&          iff,
                                       uint32_t&                   n_events) {
    switch (tc.kind) {
        case TimingControlKind::SignalEvent: {
            const SignalEventControl& ev = tc.as<SignalEventControl>();
            ++n_events;
            if (ev.iffCondition != nullptr) {
                iff = ev.iffCondition;
            }
            // An EVENT member reached through a handle is excluded here on
            // purpose: an event is woken through its own object's intrusive
            // list, not through the member's signal wait buckets, and the
            // object lives in the instance the handle names. Falling through to
            // the handle path below parks on exactly that object -- the same
            // mechanism `@(ev[i])` uses. Parking it on the vif waiter list
            // instead would never wake: `-> vif.e` fires the object.
            if (wait_builder_ != nullptr && ev.expr.kind == ExpressionKind::MemberAccess &&
                !ev.expr.type->isEvent() &&
                ev.expr.as<MemberAccessExpression>().value().type->getCanonicalType().isVirtualInterface()) {
                // `@(edge vif.sig)` (LRM 25.9): park on the member of whichever
                // instance the handle names right now. The handle is evaluated
                // here, immediately before the wait, for the same reason an
                // event array element's is.
                const auto&                     ma = ev.expr.as<MemberAccessExpression>();
                const std::optional<ir::Signal> rep =
                  vif_member_signal(ma.member, vif_named_body(ma.value()),
                                    "'" + std::string(ma.member.name) + "'");
                const ir::Index inst = lower_expr(ma.value(), *wait_builder_);
                if (!rep) {
                    // Null for the whole run (the never-bound analysis warning
                    // is already reported). LRM 25.9 makes USING a null virtual
                    // interface a run-time error, and reaching this wait is the
                    // use -- so emit the same located fatal the access paths
                    // emit (vif_null_place) instead of silently dropping the
                    // event from the list.
                    (void)vif_null_place(*ev.expr.type, *wait_builder_, loc(ev.expr));
                    wait_dead_ = true;
                    break;
                }
                if (ev.edge == slang::ast::EdgeKind::BothEdges) {
                    out.push_back(
                      ir::WaitEvent{ .signal = *rep, .edge = ir::Edge::Positive, .inst = inst });
                    out.push_back(
                      ir::WaitEvent{ .signal = *rep, .edge = ir::Edge::Negative, .inst = inst });
                } else {
                    out.push_back(
                      ir::WaitEvent{ .signal = *rep, .edge = to_edge(ev.edge), .inst = inst });
                }
                break;
            }
            if (wait_builder_ != nullptr && event_wait_needs_handle(ev.expr)) {
                // An event with no signal of its own: park on the handle the
                // expression evaluates to, right here, so `@(ev[i])` uses the
                // index's value at the moment the wait executes (LRM 15.5.5's
                // "the assignment only affects subsequent event control").
                out.push_back(ir::WaitEvent{ .signal = ir::Signal{ 0 },
                                             .edge   = ir::Edge::Any,
                                             .handle = lower_expr(ev.expr, *wait_builder_) });
                break;
            }
            if (const auto* prop = waited_class_prop(ev.expr); prop != nullptr) {
                // A class property (LRM 9.4.2 on a heap value). Park on its hidden
                // change notifier for this edge, resolving the OBJECT here, right
                // before the wait -- which is the whole point: resolving it when
                // the process was created instead dereferenced a handle that
                // `new` had not yet filled in.
                push_prop_wait_events(ev, *prop, out);
                break;
            }
            refuse_container_event_control(ev);
            const ir::Signal sig = event_signal(ev.expr);
            if (ev.edge == slang::ast::EdgeKind::BothEdges) {
                // `edge` means posedge-or-negedge (LRM 9.4.2), not any change:
                // an x<->z transition is neither and must not wake. Subscribe
                // both edge buckets; the scheduled flag dedups a double wake.
                out.push_back(ir::WaitEvent{ .signal = sig, .edge = ir::Edge::Positive });
                out.push_back(ir::WaitEvent{ .signal = sig, .edge = ir::Edge::Negative });
            } else {
                out.push_back(ir::WaitEvent{ .signal = sig, .edge = to_edge(ev.edge) });
            }
            break;
        }
        case TimingControlKind::EventList:
            for (const TimingControl* e : tc.as<EventListControl>().events) {
                collect_wait_events_impl(*e, out, iff, n_events);
            }
            break;
        default:
            // ImplicitEvent (@*) and the like: no explicit signals to collect.
            break;
    }
}

// Build the inferred sensitivity of a combinational process body and append
// it to `out`, each signal as a level-sensitive (any-change) wait. Under
// Comb rules (always_comb / always_latch) signals the body writes are
// subtracted (9.2.2.2.1 (b)); under ImplicitEvent (@*) rules a signal read
// anywhere stays sensitive even if also written — LRM 9.4.2.2's example
// makes `tmp1 = a & b; y = tmp1;` sensitive to tmp1, which another process
// may legally write (@* permits multiple drivers).
void Lowerer::collect_inferred_sensitivity(const Statement&            body,
                                           SensitivityMode             mode,
                                           std::vector<ir::WaitEvent>& out) {
    const auto declare_static = [this](const slang::ast::ClassPropertySymbol& p) {
        return declare_static_prop(p);
    };
    SensitivityCollector c(sigmap_, mode, declare_static);
    body.visit(c);
    for (const ir::Signal s : c.reads) {
        if (mode == SensitivityMode::Comb && c.written.contains(static_cast<uint32_t>(s))) {
            continue;
        }
        if (is_string_ref(module_.signal(s).type)) {
            // TODO: string signals in inferred (@*) sensitivity — string
            // stores skip wait-queue servicing, so the process would
            // never re-trigger.
            fail("string in inferred combinational sensitivity is not supported");
        }
        out.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
    }
}

// Map a slang edge kind to the IR edge. Only None (a level wait) reaches the
// default; BothEdges is expanded to two wait events by collect_wait_events.
ir::Edge Lowerer::to_edge(slang::ast::EdgeKind edge) {
    switch (edge) {
        case slang::ast::EdgeKind::PosEdge:
            return ir::Edge::Positive;
        case slang::ast::EdgeKind::NegEdge:
            return ir::Edge::Negative;
        default:
            return ir::Edge::Any;
    }
}

// The wait events for `@[edge] <class property>` (LRM 9.4.2). One per edge the
// control asks for, each parking on that edge's hidden change notifier.
//
// The WaitEvent's own edge is Any for all of them, and that is the point of the
// design: the old -> new classification happens at the STORE, which then fires
// only the notifiers whose edge the transition actually produced. So which
// notifier a waiter picks IS its edge test, and a fire is never spurious --
// a write that does not change the value fires nothing at all.
void Lowerer::push_prop_wait_events(const SignalEventControl&              ev,
                                    const slang::ast::ClassPropertySymbol& prop,
                                    std::vector<ir::WaitEvent>&            out) {
    if (wait_builder_ == nullptr) {
        // A process's static sensitivity cannot carry a handle, so a property
        // control must have been left in the body (sensitivity_needs_handle).
        fail("internal: a class-property event control with no body builder");
    }
    if (ev.edge != slang::ast::EdgeKind::None &&
        !prop_has_edges(prop.getType().getCanonicalType())) {
        // Unreachable through slang, and kept as the backstop that keeps the
        // MESSAGE accurate if it ever becomes reachable. An edge on any of these
        // types is meaningless and the LANGUAGE already says so: LRM 9.4.2 with
        // Table 9-2 defines posedge and negedge as the least significant bit of
        // the expression moving through the 0 < x/z < 1 lattice, so an
        // edge-qualified event expression must be integral -- and slang enforces
        // exactly that, rejecting `@(posedge p)` on a real, shortreal, string,
        // chandle, class handle or virtual interface with "expression type 'T' is
        // not integral" before lowering runs at all. Bit 0 of a heap pointer, of
        // an IEEE-754 significand or of the first character is not a logic level,
        // so there is nothing to classify; without this guard the design would
        // reach prop_notifier_handle and report an "internal:" gap instead.
        fail("an edge event control on a class property of type '" +
             std::string(prop.getType().toString()) +
             "' is not supported: LRM 9.4.2 and Table 9-2 define posedge and "
             "negedge on the least significant bit of the expression, which this "
             "type does not have (use a non-edge `@` or `wait`)");
    }
    const auto park = [&](ir::Edge edge) {
        out.push_back(ir::WaitEvent{ .signal = ir::Signal{ 0 },
                                     .edge   = ir::Edge::Any,
                                     .handle = prop_notifier_handle(ev.expr, prop, edge, *wait_builder_) });
    };
    if (ev.edge == slang::ast::EdgeKind::BothEdges) {
        // `edge` is posedge-or-negedge, not any change (LRM 9.4.2): an x <-> z
        // transition is neither, so this takes the two edge notifiers rather than
        // the any-change one. The scheduled flag dedups a double wake.
        park(ir::Edge::Positive);
        park(ir::Edge::Negative);
        return;
    }
    park(to_edge(ev.edge));
}

// Refuse `@` over an expression that reads a CONTAINER class property, loudly.
//
// LRM 9.4.2: "a non-edge implicit event shall be detected on any CHANGE in the
// value of the expression". A container property's notifier fires on MUTATION,
// and the two are different predicates -- `q.push_back(x); q.pop_back();`
// mutates twice and leaves `q.size()` exactly where it started -- so handing
// `@` the mutation notifier would be an observable wrong wake, not a harmless
// extra one. `wait` (9.4.3) is level-sensitive: it re-tests the condition and
// parks again, so the same fire costs it one re-test. That asymmetry is the
// whole reason `wait` is supported here and `@` is not.
//
// TODO: `@` over a container-dependent expression (LRM 9.4.2). It needs the
// expression's value snapshotted in the WAITER's own frame and compared on
// every wake, since the value is computed rather than stored and the write
// side has nothing to classify. That is a different mechanism from the
// classify-at-the-store one this file implements, not an extension of it.
void Lowerer::refuse_container_event_control(const SignalEventControl& ev) {
    ClassPropReadScan scan;
    ev.expr.visit(scan);
    if (!scan.any_container) {
        return;
    }
    fail("an event control on a container class property (queue / dynamic array / "
         "associative array) is not supported; only `wait` can park on one");
}

// Load the handle of one change notifier of a class property, dereferencing the
// object the reference names AT THIS POINT in the body. That timing is the fix
// for `@(c.p)` in a forked process: the handle used to be resolved while the
// process was being built, which read `c` before `c = new` had run and aborted on
// a null dereference at 0 s.
ir::Index Lowerer::prop_notifier_handle(const Expression&                      e,
                                        const slang::ast::ClassPropertySymbol& prop,
                                        ir::Edge                               edge,
                                        ir::Builder&                           b) {
    const ir::SourceLoc l    = to_source_loc(e.sourceRange.start());
    const auto          slot = prop_notify_.find(&prop);
    if (slot == prop_notify_.end() || slot->second[static_cast<uint32_t>(edge)] == ir::kNoNotify) {
        // The design-wide pre-pass decides which notifiers exist and this decides
        // which are used; they read the same resolver, so a gap is a bug here, not
        // an unsupported design.
        fail("internal: no change notifier for class property '" + std::string(prop.name) + "'");
    }
    ir::Index      handle{};
    ir::Ref        cls{};
    ir::SourceSpan span{};
    if (e.kind == ExpressionKind::MemberAccess) {
        const auto& ma = e.as<MemberAccessExpression>();
        handle         = lower_expr(ma.value(), b);
        cls            = lower_type(*ma.value().type);
        span           = to_source_span(ma.value().sourceRange);
    } else if (ctor_this_) {
        // A property default initializer: `this` is the object being built. Same
        // precedence as value_lvalue's, and for the same reason.
        handle = ctor_this_->handle;
        cls    = ctor_this_->cls;
    } else if (fn_ctx_ != nullptr && fn_ctx_->has_this) {
        handle = this_handle(b, l);
        cls    = fn_ctx_->this_class;
    } else {
        fail("a bare class property in an event control needs an enclosing method");
    }
    const ir::Index body = b.handle_deref(handle, cls, span.valid() ? span.start : l);
    if (span.end != ir::SourceLoc{ 0 }) {
        b.set_end(body, span.end);
    }
    return b.unary(
      ir::Inst::Tag::Load, b.member_select(body, slot->second[static_cast<uint32_t>(edge)], cls, l), l);
}

bool Lowerer::event_wait_needs_handle(const Expression& e) {
    if (!e.type->isEvent()) {
        return false;
    }
    // Forms event_signal resolves to a synthetic signal of their own rather
    // than to an event object: a clocking block's hidden toggle (`@(cb)`) and
    // the global clocking event (`@($global_clock)`). Both are event-typed but
    // neither is an event VARIABLE, and neither has a handle to evaluate.
    if (e.kind == ExpressionKind::ArbitrarySymbol || global_clock_call(e) != nullptr) {
        return false;
    }
    if (e.kind != ExpressionKind::NamedValue && e.kind != ExpressionKind::HierarchicalValue) {
        return true; // an element select, a member access, ...
    }
    // A frame-resident event (a formal or an automatic local) has no signal.
    if (fn_ctx_ != nullptr && fn_ctx_->localmap.contains(&e.as<ValueExpressionBase>().symbol)) {
        return true;
    }
    // A bare `@e` in a method, `e` an event-typed INSTANCE property (LRM 9.4.2
    // over 15.5.1): the property is the handle slot of the very object `-> e`
    // fires, so park on the handle it holds when the wait executes -- the same
    // object `@(this.e)` (a MemberAccess, admitted above) parks on. A STATIC
    // event property stays on the signal path (signal_of ->
    // declare_static_prop); instance_class_prop already excludes it.
    if (instance_class_prop(e) != nullptr) {
        return true;
    }
    // With merging anywhere in the design, even a plain `@(e)` has to re-read
    // the handle on each pass, which only the body-resident wait can do.
    return events_merge_;
}

// Whether any event in a sensitivity list would need a handle, which a process's
// static subscription cannot carry. build_process leaves such a list in the body.
bool Lowerer::sensitivity_needs_handle(const TimingControl& tc) {
    switch (tc.kind) {
        case TimingControlKind::SignalEvent: {
            const Expression& e = tc.as<SignalEventControl>().expr;
            // A class property's notifier is reached through the object handle, so
            // `always @(c.p)` has to stay in the body too -- the seed loop that
            // issues a static subscription cannot evaluate `c`.
            return event_wait_needs_handle(e) || waited_class_prop(e) != nullptr;
        }
        case TimingControlKind::EventList:
            for (const TimingControl* e : tc.as<EventListControl>().events) {
                if (sensitivity_needs_handle(*e)) {
                    return true;
                }
            }
            return false;
        default:
            return false;
    }
}

// Whether an expression reaches through a virtual interface handle anywhere.
// Such a place names an instance chosen at run time, which the signal-keyed
// wait engine cannot subscribe to.
bool Lowerer::reads_vif(const Expression& e) {
    struct Scan : slang::ast::ASTVisitor<Scan, slang::ast::VisitFlags::AllGood> {
        bool found = false;
        void handle(const MemberAccessExpression& ma) {
            found = found || ma.value().type->getCanonicalType().isVirtualInterface();
            visitDefault(ma);
        }
    };
    Scan scan;
    e.visit(scan);
    return scan.found;
}

// Every `vif.member` read in an expression, in source order and deduplicated by
// member symbol: one wait entry per distinct member is enough, and a repeated
// read of the same one would otherwise park twice on the same list.
std::vector<const MemberAccessExpression*> Lowerer::vif_member_reads(const Expression& e) {
    struct Scan : slang::ast::ASTVisitor<Scan, slang::ast::VisitFlags::AllGood> {
        std::vector<const MemberAccessExpression*> hits;
        void handle(const MemberAccessExpression& ma) {
            if (ma.value().type->getCanonicalType().isVirtualInterface() &&
                std::ranges::none_of(hits, [&](const MemberAccessExpression* h) {
                    return &h->member == &ma.member;
                })) {
                hits.push_back(&ma);
            }
            visitDefault(ma);
        }
    };
    Scan scan;
    e.visit(scan);
    return std::move(scan.hits);
}

ir::Signal Lowerer::event_signal(const Expression& e) {
    if (reads_vif(e)) {
        // A direct `@(edge vif.sig)` never reaches here: collect_wait_events
        // parks it on the member's waiter list in the instance the handle
        // names. What is left is a COMPOSITE expression over one (`@(vif.a &
        // vif.b)`), which the temp-net path would answer by dereferencing the
        // handle from a continuous process -- before it is assigned, and then
        // frozen onto whichever instance was bound at time zero.
        //
        // TODO: an event control on an expression over virtual interface
        // members (LRM 25.9) — needs the synthesized net to re-read the handle.
        fail("an event control on an expression through a virtual interface is not supported");
    }
    if (is_string_expr(e)) {
        // TODO: waiting on a string change (needs string-aware wait-queue
        // servicing; string stores skip edge detection).
        fail("string in an event control is not supported");
    }
    if (e.kind == ExpressionKind::ArbitrarySymbol) {
        // `@(cb)`: waiting on a clocking block (LRM 14.15) waits on the event
        // the block triggers at its clocking event, NOT on the clock itself --
        // that is what puts the waiter after the block's samples are updated
        // (LRM 14.13). slang binds the reference as a void-typed
        // ArbitrarySymbolExpression, so it would otherwise reach the integral
        // check below.
        const Symbol& sym = *e.as<ArbitrarySymbolExpression>().symbol;
        if (sym.kind == SymbolKind::ClockingBlock) {
            return clocking_state(sym.as<ClockingBlockSymbol>()).event;
        }
    }
    if (const ClockingBlockSymbol* gc = global_clock_call(e)) {
        // `@($global_clock)` (LRM 14.14): the global clocking event, which is the
        // event of the block that declares it -- the same wait as `@(cb)` on it.
        return clocking_state(*gc).event;
    }
    if (e.kind == ExpressionKind::NamedValue || e.kind == ExpressionKind::HierarchicalValue) {
        // Waiting on a hierarchical signal (`@(posedge top.dut.clk)`, LRM 23.6)
        // subscribes to the target instance's signal directly. A clockvar
        // (`@(posedge cb.sig)`, LRM 14.15) resolves the same way and lands on
        // its sampled storage, so the edge seen is the edge of the sampled
        // value stream, which is what 14.15 specifies.
        const ValueSymbol& vs = e.as<ValueExpressionBase>().symbol;
        if (e.type->isEvent() && fn_ctx_ != nullptr && fn_ctx_->localmap.contains(&vs) &&
            wait_builder_ == nullptr) {
            // An event whose handle lives in a call frame -- a formal, or an
            // automatic local -- has no signal for the wait list to name. The
            // trigger forms take the handle as a value and work; the wait does
            // not, because a WaitEdge names a signal.
            // TODO: carry the handle value on a WaitEdge so `@(<any event
            // expression>)` works; see docs/design/event-handles.md §6.
            fail("waiting on an event held in a subroutine argument or an "
                 "automatic local is not supported (triggering one is)");
        }
        return signal_of(vs);
    }
    // A non-simple event expression (struct/union member, array element,
    // bit/part-select, arithmetic): the wait engine subscribes by whole-signal
    // id and detects edges on the whole signal's LSB, so a sub-expression at a
    // nonzero bit offset cannot be watched directly. Synthesize a temp net
    // continuously assigned from the expression and subscribe to it: the net's
    // bit 0 is the expression's LSB (exact for edge detection) and its whole
    // word feeds the level `changed` compare. Costs one delta of latency vs. a
    // direct signal (the continuous process runs first) — identical to
    // hand-writing the intermediate `assign`.
    if (!e.type->isIntegral()) {
        // A real / aggregate / other non-integral event expression has no flat
        // packed net to extract onto; the temp-net store expects a scalar.
        fail("unsupported event expression (non-integral)");
    }
    return synth_event_net(e);
}

// Synthesize the temp net an expression event control subscribes to: a fresh
// internal signal of the expression's type, driven by a self-starting
// continuous process sensitive to the expression's reads (LRM 4.9.1).
ir::Signal Lowerer::synth_event_net(const Expression& e) {
    // A synthesized event net is a module-level continuous process with no
    // frame. If the operand references a frame-local (a task/function formal
    // or automatic var, e.g. `@(posedge ack[p])` with `p` a task argument),
    // the lowered IR would embed a Tag::Local into a frameless process and
    // SIGSEGV in the backend. Reject loudly until frame-local event operands
    // are supported (would need the index value captured at wait time).
    if (fn_ctx_ != nullptr) {
        LocalRefDetector d;
        d.locals = &fn_ctx_->localmap;
        e.visit(d);
        if (d.found) {
            fail("event control on an expression that reads a local variable / "
                 "formal argument (e.g. a variable bit-select `@(posedge ack[p])` "
                 "inside a task) is not supported");
        }
    }
    const ir::Ref       ty  = lower_type(*e.type);
    const ir::Signal    tmp = declare_temp(ty, "event");
    const ir::SourceLoc l   = loc(e);

    ir::Builder     b(module_.pool);
    const ir::Index dst = b.signal(tmp, l);
    const ir::Index rhs = b.resize(lower_expr(e, b), ty, l);
    b.binary(ir::Inst::Tag::Store, dst, rhs, l);
    add_continuous_process(continuous_events(e), l, b);
    return tmp;
}

// A frame Local with no symbol behind it, for a compiler-synthesized value that
// must be per-activation rather than per-design.
uint32_t Lowerer::anon_local(ir::Ref type) {
    const auto idx = static_cast<uint32_t>(fn_ctx_->local_types.size());
    fn_ctx_->local_types.push_back(type);
    return idx;
}

// `@(<expression over virtual interface members>)` (LRM 9.4.2 + 25.9). The
// general path synthesizes a net driven by a continuous process, which cannot
// work through a handle: it would read the handle before it is assigned, and
// then track whichever instance was bound at time zero. So this desugars to
// exactly what the clause asks for -- wait until the expression's value
// changes -- over the per-member waits that do work:
//
//   sample = expr;
//   forever begin @(each member read) if (expr !== sample) break; sample = expr; end
//
// The sample lives in the call frame inside a subroutine (so two activations of
// one task do not share it) and in module storage in a process body, which is a
// single coroutine and cannot re-enter its own wait.
void Lowerer::lower_vif_expr_wait(const SignalEventControl& ev, ir::Builder& b, ir::SourceLoc l) {
    if (ev.edge != slang::ast::EdgeKind::None) {
        // TODO: an edge qualifier on an expression over virtual interface
        // members (LRM 9.4.2 takes the edge of the expression's LSB). The
        // change form below is the one that occurs in practice.
        fail("an edge qualifier on an expression through a virtual interface is not supported");
    }
    if (ev.iffCondition != nullptr) {
        fail("an `iff` qualifier on an expression through a virtual interface is not supported");
    }
    const ir::Ref ty = lower_type(*ev.expr.type);
    // The sample's storage, as a maker so each use re-forms the lvalue.
    const bool       in_frame = fn_ctx_ != nullptr;
    const uint32_t   slot     = in_frame ? anon_local(ty) : 0;
    const ir::Signal tmp      = in_frame ? ir::Signal{ 0 } : declare_temp(ty, "vif_event");
    const auto       place    = [&](ir::Builder& bb) {
        return in_frame ? bb.local(slot, ty, l) : bb.signal(tmp, l);
    };
    const ir::Index dst = place(b);
    const ir::Index rhs = b.resize(lower_expr(ev.expr, b), ty, l);
    b.binary(ir::Inst::Tag::Store, dst, rhs, l);

    b.open_region();
    const ir::Index      lpred = true_const(b, l);
    const ir::ExtraSlice lcond = b.close_region();

    b.open_region();
    {
        std::vector<ir::WaitEdge> edges;
        for (const MemberAccessExpression* ma : vif_member_reads(ev.expr)) {
            const std::optional<ir::Signal> rep =
              vif_member_signal(ma->member, vif_named_body(ma->value()),
                                "'" + std::string(ma->member.name) + "'");
            const ir::Index inst = lower_expr(ma->value(), b);
            if (rep) {
                edges.push_back(ir::WaitEdge{ *rep, static_cast<uint32_t>(ir::Edge::Any),
                                              ir::kNoWaitHandle, inst });
            }
        }
        std::vector<ir::WaitEvent> reads;
        collect_expr_reads(ev.expr, reads);
        for (const ir::WaitEvent& e : reads) {
            edges.push_back(ir::WaitEdge{ e.signal, static_cast<uint32_t>(e.edge) });
        }
        b.wait(edges, l);
        const ir::Index cur     = b.resize(lower_expr(ev.expr, b), ty, l);
        const ir::Index sampled = b.unary(ir::Inst::Tag::Load, place(b), l);
        const ir::Index same    = b.binary(ir::Inst::Tag::CaseEq, cur, sampled, l);
        b.open_region();
        b.break_(l);
        const ir::ExtraSlice then_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice else_r = b.close_region();
        b.cond(b.unary(ir::Inst::Tag::Lnot, same, l), then_r, else_r, l);
        b.binary(ir::Inst::Tag::Store, place(b), cur, l);
    }
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::ExtraSlice step = b.close_region();

    b.loop(lcond, lpred, body, step, /*bottom_test*/ false, l);
}

// `vif.cb.d <= expr` (LRM 14.16 reached through LRM 25.9). Identical to the
// ordinary synchronous drive -- two stores into the block's per-clockvar
// pending state, applied by that block's Re-NBA process at the clocking event
// -- except that the handle picks which INSTANCE's pending slot is written.
// There is no aliasing hazard the way there is for an NBA: the slot is
// selected at run time, and two drives landing in one slot in a cycle
// overwriting each other is exactly LRM 14.16.2.
void Lowerer::lower_vif_clocking_drive(const MemberAccessExpression& ma,
                                       const AssignmentExpression&   a,
                                       ir::Builder&                  b) {
    const auto& cv = ma.member.as<slang::ast::ClockVarSymbol>();
    if (!a.isNonBlocking()) {
        fail("a clocking block drive must use `<=` (LRM 14.16)");
    }
    if (a.timingControl != nullptr) {
        fail("a cycle delay on a synchronous drive is not supported");
    }
    const std::optional<VifClockVar> rec =
      vif_clockvar(cv, vif_named_body(ma.value()), "'" + std::string(cv.name) + "'");
    const ir::SourceLoc l = loc(a);
    if (!rec) {
        (void)vif_null_place(*ma.type, b, l);
        return;
    }
    if (!rec->drives) {
        fail("a drive of a clocking block input is not legal SystemVerilog");
    }
    const ir::Index handle = lower_expr(ma.value(), b);
    const ir::Index val    = b.iface_signal(handle, rec->drive_val, l);
    b.binary(ir::Inst::Tag::Store, val,
             b.resize(lower_expr(a.right(), b), module_.signal(rec->drive_val).type, l), l);
    const ir::Index pend = b.iface_signal(handle, rec->drive_pend, l);
    const ir::Index yes  = true_const(b, l);
    b.binary(ir::Inst::Tag::Store, pend, yes, l);
}

// Lower an intra-body event control `@(...)` to a Wait instruction. A
// multi-signal list (@(a or posedge b)) registers the process on every listed
// (signal, edge) queue at once; it wakes when any of them fires.
void Lowerer::lower_event_wait(const TimingControl& tc, ir::Builder& b, ir::SourceLoc l) {
    if (tc.kind == TimingControlKind::SignalEvent) {
        const auto& ev = tc.as<SignalEventControl>();
        if (reads_vif(ev.expr) && ev.expr.kind != ExpressionKind::MemberAccess) {
            lower_vif_expr_wait(ev, b, l);
            return;
        }
    }
    std::vector<ir::WaitEvent> events;
    // Handle expressions in this list lower into `b`, immediately before the
    // Wait, so an `@(ev[i])` parks on the object `ev[i]` names right now.
    ir::Builder* const saved = wait_builder_;
    wait_builder_            = &b;
    const Expression* iff    = collect_wait_events(tc, events);
    wait_builder_            = saved;
    const bool dead          = wait_dead_;
    wait_dead_               = false;
    if (events.empty()) {
        if (dead) {
            // Every listed event was a never-bound virtual interface member;
            // the located fatal above replaces the wait, and execution cannot
            // pass it.
            return;
        }
        // Reachable for an event control that resolves to nothing at all --
        // an implicit `@*` inside a procedural body infers no sensitivity
        // here. An event class property no longer lands here: it parks on its
        // object through the handle path (event_wait_needs_handle).
        fail("an event control here names no signal or event object (an "
             "implicit '@*' inside a procedural body is not supported)");
    }
    std::vector<ir::WaitEdge> edges;
    edges.reserve(events.size());
    for (const ir::WaitEvent& e : events) {
        edges.push_back(
          ir::WaitEdge{ e.signal, static_cast<uint32_t>(e.edge), e.handle, e.inst });
    }
    if (iff != nullptr) {
        emit_guarded_wait(edges, *iff, b, l);
    } else {
        b.wait(edges, l);
    }
}

// Lower `wait_order(a, b, c) pass; else fail;` (LRM 15.5.4): suspend until every
// listed event has triggered left to right, failing the moment a not-yet-reached
// event triggers out of turn.
//
// Desugared over the existing IR -- no new op. The list length is a compile-time
// constant, so the stages are UNROLLED, which is what lets each stage wait on a
// different set (the events still ahead of it):
//
//   failed = 0;
//   stage k:  if (!failed) forever { @(e_k or e_k+1 or ... or e_N-1);
//                                    if (e_k.triggered) break;
//                                    failed = 1; break; }
//   if (failed) <fail> else <pass>
//
// Waiting on the whole remaining tail is what detects an out-of-order trigger:
// any later event wakes the stage, and `e_k.triggered` is then 0. An event
// already passed is not in any later stage's list, so re-triggering it is
// ignored -- LRM: "preceding events are not limited to occur only once".
//
// Stage 0 additionally accepts the PERSISTENT triggered state before waiting,
// which the LRM allows for the first event only ("only the first event in the
// list can wait for the persistent triggered event").
void Lowerer::lower_wait_order(const slang::ast::WaitOrderStatement& s, ir::Builder& b) {
    const ir::SourceLoc l = loc(s);
    const auto          n = s.events.size();

    const TempCell    failed = make_temp_cell(module_.pool.logic_type, "wait_order");
    const ir::Index   zero   = b.constant(module_.pool.put_int(module_.pool.logic_type, 0), l);
    b.binary(ir::Inst::Tag::Store, failed.lv(b, l), zero, l);

    for (size_t k = 0; k < n; ++k) {
        // if (!failed) { ...stage k... }
        const ir::Index not_failed =
          b.unary(ir::Inst::Tag::Bnot, b.unary(ir::Inst::Tag::Load, failed.lv(b, l), l), l);
        b.open_region();
        {
            // Stage 0 may be satisfied by the persistent triggered state.
            b.open_region();
            const ir::Index lpred =
              k == 0 ? b.unary(ir::Inst::Tag::Bnot, b.event_triggered(event_handle(*s.events[0], b), l), l)
                     : true_const(b, l);
            const ir::ExtraSlice lcond = b.close_region();

            b.open_region();
            {
                // Wait on every event still ahead, this one included.
                std::vector<ir::WaitEdge> edges;
                edges.reserve(n - k);
                for (size_t j = k; j < n; ++j) {
                    edges.push_back(ir::WaitEdge{ ir::Signal{ 0 },
                                                  static_cast<uint32_t>(ir::Edge::Any),
                                                  event_handle(*s.events[j], b) });
                }
                b.wait(edges, l);

                const ir::Index hit = b.event_triggered(event_handle(*s.events[k], b), l);
                b.open_region();
                b.break_(l);
                const ir::ExtraSlice then_r = b.close_region();
                b.open_region();
                {
                    const ir::Index dst = failed.lv(b, l);
                    const ir::Index yes = true_const(b, l);
                    b.binary(ir::Inst::Tag::Store, dst, yes, l);
                    b.break_(l);
                }
                const ir::ExtraSlice else_r = b.close_region();
                b.cond(hit, then_r, else_r, l);
            }
            const ir::ExtraSlice body = b.close_region();
            b.open_region();
            const ir::ExtraSlice step = b.close_region();
            b.loop(lcond, lpred, body, step, /*bottom_test*/ false, l);
        }
        const ir::ExtraSlice stage = b.close_region();
        b.open_region();
        const ir::ExtraSlice skip = b.close_region();
        b.cond(not_failed, stage, skip, l);
    }

    // The action block. With no `else`, LRM 15.5.4 makes a failure a run-time
    // error rather than a silent skip.
    const ir::Index fail_now = b.unary(ir::Inst::Tag::Load, failed.lv(b, l), l);
    b.open_region();
    if (s.ifFalse != nullptr) {
        lower_stmt(*s.ifFalse, b);
    } else {
        // LRM 15.5.4: "If the fail statement is not specified, a failure
        // generates a run-time error." $error is the located, non-fatal
        // severity the LRM's wording matches.
        b.severity(static_cast<uint32_t>(ir::Severity::Error),
                   severity_banner(s.sourceRange.start()),
                   "wait_order: events triggered out of order", {}, l);
    }
    const ir::ExtraSlice on_fail = b.close_region();
    b.open_region();
    if (s.ifTrue != nullptr) {
        lower_stmt(*s.ifTrue, b);
    }
    const ir::ExtraSlice on_pass = b.close_region();
    b.cond(fail_now, on_fail, on_pass, l);
}

// Lower `if (guard) body` as an entire process body: the `iff` qualifier on a
// top-level sensitivity list (LRM 9.4.2.3). The guard predicate is evaluated
// in the body's own scope, so it sees the event-time values.
void Lowerer::emit_guarded_body(const Statement& body, const Expression& guard, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index c = lower_predicate(guard, b);
    b.open_region();
    lower_stmt(body, b);
    const ir::ExtraSlice then_r = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_r = b.close_region(); // empty
    b.cond(c, then_r, else_r, l);
}

// Lower an intra-body `@(ev iff C)` (LRM 9.4.2.3): re-wait until the edge
// fires with the guard true at event time, `forever { @(ev); if (C) break; }`.
// The guard is re-tested on every wake, so a fire with C false parks again.
void Lowerer::emit_guarded_wait(const std::vector<ir::WaitEdge>& edges,
                                const Expression&                guard,
                                ir::Builder&                     b,
                                ir::SourceLoc                    l) {
    b.open_region();
    const ir::Index      lpred = true_const(b, l);
    const ir::ExtraSlice lcond = b.close_region();

    b.open_region();
    {
        b.wait(edges, l);
        const ir::Index c = lower_predicate(guard, b);
        b.open_region();
        b.break_(l);
        const ir::ExtraSlice then_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice else_r = b.close_region(); // empty
        b.cond(c, then_r, else_r, l);
    }
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::ExtraSlice step = b.close_region(); // empty

    b.loop(lcond, lpred, body, step, /*bottom_test*/ false, l);
}

// Emit the suspend for a blocking intra-assignment timing control
// (`lhs = #d rhs` / `lhs = @e rhs` / `lhs = repeat(N) @e rhs`, LRM 9.4.5).
// The caller has already sampled the RHS; this blocks the process on the
// delay/event, after which the caller stores the sampled value. A repeat
// event control waits for its child event N times (count evaluated once,
// N <= 0 falls through with no wait). The nonblocking form is not routed
// here — it needs a detached scheduled update (see lower_assign).
void Lowerer::lower_intra_timing(const TimingControl& tc, ir::Builder& b, ir::SourceLoc l) {
    switch (tc.kind) {
        case TimingControlKind::Delay:
            b.unary(ir::Inst::Tag::Delay, scaled_delay(tc.as<DelayControl>().expr, b), l);
            return;
        case TimingControlKind::SignalEvent:
        case TimingControlKind::EventList:
            lower_event_wait(tc, b, l);
            return;
        case TimingControlKind::RepeatedEvent: {
            const RepeatedEventControl& rec = tc.as<RepeatedEventControl>();
            if (!is_event_control(rec.event)) {
                fail("repeat intra-assignment timing requires an event control");
            }
            // A real count is counted in a signed 64-bit type, rounded into it
            // half away from zero (LRM 6.12.1's real -> integral conversion, the
            // FtoI coerce ends in) -- signed and 64-bit for the same reasons as
            // the repeat statement (lower_repeat). 9.4.5's "the assignment occurs
            // as if there is no repeat construct" for a count <= 0 then falls out
            // of the Lt below unchanged: a negative bound waits zero times, so
            // the caller's store happens immediately.
            const ir::Ref ty =
              rec.expr.type->isFloating() ? make_int_type(64, false, true) : lower_type(*rec.expr.type);
            const TempCell  bound     = make_temp_cell(ty, "repeat_bound");
            const TempCell  counter   = make_temp_cell(ty, "repeat_count");
            const ir::Index bound_lv  = bound.lv(b, l);
            const ir::Index bound_val = coerce(lower_expr(rec.expr, b), *rec.expr.type, ty, b, l);
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
            lower_event_wait(rec.event, b, l);
            const ir::ExtraSlice body = b.close_region();

            b.open_region();
            const ir::Index sc   = b.unary(ir::Inst::Tag::Load, counter.lv(b, l), l);
            const ir::Index one  = b.constant(module_.pool.put_int(ty, 1), l);
            const ir::Index slot = counter.lv(b, l);
            const ir::Index next = b.binary(ir::Inst::Tag::Add, sc, one, l);
            b.binary(ir::Inst::Tag::Store, slot, next, l);
            const ir::ExtraSlice step = b.close_region();

            b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
            return;
        }
        default:
            fail("unsupported intra-assignment timing control");
    }
}

// Resolve the target of `-> e` to the event's synthetic signal, verifying it
// is an event (defensive — slang types the target as an event already).
//
// A hierarchical target (`-> u.e`, `-> i.e` through an interface port or a
// modport) resolves like any other cross-instance reference: slang binds the
// name to the SAME ValueSymbol a local `e` would, so signal_of lands on the
// one shared signal and the trigger reaches the instance that owns it. This
// matches event_signal, which has always accepted HierarchicalValue for
// `@(u.e)` / `u.e.triggered` — the two paths bottom out in the same
// signal_of, and only accepting the narrower kind here is what made a
// submodule's event waitable but not triggerable.
ir::Signal Lowerer::trigger_target(const Expression& target) {
    const bool named = target.kind == ExpressionKind::NamedValue ||
                       target.kind == ExpressionKind::HierarchicalValue;
    if (!named || !target.type->isEvent()) {
        // TODO: triggering an event element of an array / struct member, or an
        // event through a handle expression (docs/design/event-handles.md).
        fail("unsupported event trigger target");
    }
    return signal_of(target.as<ValueExpressionBase>().symbol);
}

// Resolve any event expression to its 64-bit handle value. Because an event is
// now an ordinary plane-typed value, this is just the generic rvalue path -- a
// declared event reads its signal, an array element reads its element, a class
// property reads its field, a formal reads its frame slot -- so no event-shaped
// resolver is needed and every target form works the same way.
ir::Index Lowerer::event_handle(const Expression& e, ir::Builder& b) {
    if (!e.type->isEvent()) {
        // Defensive: slang types a trigger target and a `.triggered` receiver as
        // an event already.
        fail("expected an event expression");
    }
    return lower_expr(e, b);
}

// The `repeat (n)` count of a detached trigger, as a compile-time constant.
// A runtime count would need the registration to carry a value, which the
// record could hold -- but LRM 15.5.1's form is a fixed repetition in every
// real use, and a constant keeps the record a plain integer.
uint32_t Lowerer::repeat_count_const(const Expression& e) {
    slang::ast::ASTContext     actx(*scope_, slang::ast::LookupLocation::max);
    slang::ast::EvalContext    ectx(actx);
    const slang::ConstantValue cv = e.eval(ectx);
    if (!cv.isInteger()) {
        // TODO: a runtime repeat count on `->> repeat(n) @(x) e`.
        fail("a non-constant repeat count on an event-controlled nonblocking "
             "trigger is not supported");
    }
    const auto n = cv.integer().as<int64_t>();
    if (!n.has_value() || *n <= 0) {
        // LRM 11.4.13-style: a non-positive repetition means the trigger is
        // immediate; treat it as one occurrence rather than never maturing.
        return 1;
    }
    return static_cast<uint32_t>(*n);
}

// Lower `-> e` / `->> e` / `->> #d e` (LRM 15.5.2, 15.5.4).
void Lowerer::lower_event_trigger(const EventTriggerStatement& ev, ir::Builder& b) {
    if (ev.timing != nullptr) {
        // Only `->>` carries a timing control (grammar), so a delayed trigger
        // is always nonblocking. A fixed delay `->> #d e` schedules a detached
        // wheel timer; the event-controlled form is not yet lowered.
        if (ev.timing->kind == slang::ast::TimingControlKind::Delay) {
            const Expression& amount = ev.timing->as<slang::ast::DelayControl>().expr;
            // Evaluate the delay first, then the target: the LRM leaves the order
            // unspecified, and this keeps the handle load adjacent to its use.
            const ir::Index ticks = scaled_delay(amount, b);
            b.event_trigger_nb_delayed(event_handle(ev.target, b), ticks, loc(ev));
            return;
        }
        // `->> @(x) e` / `->> repeat(n) @(x) e` (LRM 15.5.1): register a
        // DETACHED one-shot subscription on the edge and fall through. No
        // coroutine hosts the wait, so the process does not block; when the
        // edge matures the subscription, the event is triggered in the NBA
        // region, which is what `->>` means.
        const TimingControl* inner = &*ev.timing;
        uint32_t             count = 1;
        if (inner->kind == slang::ast::TimingControlKind::RepeatedEvent) {
            const auto& rec = inner->as<slang::ast::RepeatedEventControl>();
            count           = repeat_count_const(rec.expr);
            inner           = &rec.event;
        }
        if (!is_event_control(*inner)) {
            fail("event-controlled nonblocking trigger requires an event control");
        }
        std::vector<ir::WaitEvent> evs;
        if (collect_wait_events(*inner, evs) != nullptr) {
            // TODO: `iff` on the controlling edge of a detached trigger.
            fail("an iff qualifier on an event-controlled nonblocking trigger is "
                 "not supported");
        }
        if (evs.size() != 1) {
            // TODO: a multi-edge control (`->> @(a or b) e`) needs one
            // registration per edge plus a shared one-shot claim, so the first
            // edge to mature cancels the others.
            fail("an event-controlled nonblocking trigger on a multi-event "
                 "control is not supported");
        }
        b.event_trigger_nb_event(event_handle(ev.target, b), evs[0].signal,
                                 static_cast<uint32_t>(evs[0].edge), count, loc(ev));
        return;
    }
    const ir::Index h = event_handle(ev.target, b);
    if (ev.isNonBlocking) {
        b.event_trigger_nb(h, loc(ev)); // `->> e`: wake in the NBA region
    } else {
        b.event_trigger(h, loc(ev)); // `-> e`: wake now
    }
}

// If `e` is an `<event>.triggered` system-method call (LRM 15.5.3), return the
// event operand; else nullptr. slang models the property as a system method
// whose receiver is arguments()[0].
const Expression* Lowerer::triggered_event(const Expression& e) {
    if (e.kind != ExpressionKind::Call) {
        return nullptr;
    }
    const CallExpression& call = e.as<CallExpression>();
    if (!call.isSystemCall() || call.getSubroutineName() != "triggered") {
        return nullptr;
    }
    if (call.arguments().empty() || !call.arguments()[0]->type->isEvent()) {
        return nullptr;
    }
    return call.arguments()[0];
}

// The OTHER `.triggered`. On a named sequence rather than an event, slang
// models `s.triggered` and `s.matched` the same way -- a system method with
// the sequence instance as arguments()[0] -- but they are a different feature
// entirely: each reports on the attempts of THAT sequence, which would have to
// be compiled to an automaton of its own and stepped beside the property
// reading it. Returns the method name, or empty when the call is neither.
//
// Kept as one predicate because both refusal sites want the name in the
// message: an assertion expression reaches reject_unsampled_call, and
// everywhere else reaches lower_sysfunc.
std::string_view Lowerer::sequence_method(const CallExpression& call) {
    const std::string_view name = call.getSubroutineName();
    if (!call.isSystemCall() || (name != "triggered" && name != "matched")) {
        return {};
    }
    if (call.arguments().empty() || !call.arguments()[0]->type->isSequenceType()) {
        return {};
    }
    return name;
}

// The (clock, edge) a clocking block steps on. LRM 14.3 allows any event
// expression; supported here is one posedge or negedge of one signal -- the
// same subset an assertion clock is held to, for the same reason: the Observed
// body detects its own edge from a one-slot history, which a level-sensitive
// clock, an `edge` clock, an `iff` guard or a multi-signal list has no single
// answer for.
const SignalEventControl& Lowerer::clocking_event(const ClockingBlockSymbol& cb) {
    const TimingControl& tc = cb.getEvent();
    if (tc.kind != TimingControlKind::SignalEvent) {
        // TODO: a multi-signal clocking event, or one naming another clocking
        // block (LRM 14.3 / 14.6), which shares its synchronization event.
        fail("only a single-signal clocking event is supported");
    }
    const SignalEventControl& ev = tc.as<SignalEventControl>();
    if (ev.iffCondition != nullptr) {
        fail("an `iff` qualifier on a clocking event is not supported");
    }
    if (ev.edge != slang::ast::EdgeKind::PosEdge && ev.edge != slang::ast::EdgeKind::NegEdge) {
        // TODO: a level-sensitive (`@(clk)`) or `edge` clocking event. Both step
        // the block on more than one kind of transition, and 14.4's edge-based
        // skews are defined against them.
        fail("a clocking event must be a posedge or a negedge");
    }
    return ev;
}

// Whether a signal is a PROGRAM signal, which LRM 24.3 defines by declaring
// scope and nothing else:
//
//   Variables declared within the scope of a program, including variables
//   declared as ports, are called program variables. Similarly, nets declared
//   within the scope of a program are called program nets. Program variables
//   and nets are collectively termed program signals.
//
// and whose dual the same subclause fixes just as tightly: "Any net or
// variable declared within a module, interface, package, or $unit is
// considered to be a design signal." So the test is the first enclosing
// instance body, and a program PORT is deliberately included however the
// design drives the other side of it.
//
// Canonicalized first, which is what keeps the recommended shape legal: a
// signal reached through an interface port or a modport collapses onto the
// INTERFACE instance's own storage (docs/design/interfaces.md), and the clause
// above calls that a design signal.
//
// The walk stops at the first InstanceBody because climbing past one reaches
// the DECLARATION site rather than the enclosing instance (see owning_top), so
// a program instantiated in a module must not read as a module signal's scope
// and a module instantiated -- illegally, but slang decides that -- in a
// program must not read as a program's.
bool Lowerer::is_program_signal(const ValueSymbol& raw) {
    const ValueSymbol& sym = canonical_value_symbol(raw);
    for (const Scope* s = sym.getParentScope(); s != nullptr;) {
        const Symbol& owner = s->asSymbol();
        if (owner.kind == SymbolKind::InstanceBody) {
            return owner.as<InstanceBodySymbol>().getDefinition().definitionKind ==
                   slang::ast::DefinitionKind::Program;
        }
        s = owner.getParentScope();
    }
    return false;
}

// The first program signal an event expression references, or null. Shared by
// the clocking-block and concurrent-assertion clocks, which have one and the
// same defect for one and the same reason -- both are Observed-region bodies
// detecting their own edge -- and it is a silent wrong answer, not a crash, so
// it is refused at both sites.
//
// LRM 24.3.1:
//
//   If an assertion is clocked by activity on a program object (not
//   recommended), the scheduler will iterate from the reactive region set back
//   around the outer loop in Figure 4-1, through the Observed region, where the
//   assertion is evaluated.
//
// Clotho's loop does not close that way. In emit_active_run: `wheel.check` runs
// emit_observed_run and then re-settles to the active drain (`check`) when the
// ACTIVE ring is non-empty; the reactive drain's `reactive.done` re-settle
// branches to `check` on the same test and otherwise falls through to
// `reactive.settled` -> Postponed -> the wheel. Observed <-> Active is a closed
// loop, but nothing ever re-enters Observed from the reactive region set. So a
// clock written only in Reactive -- a program `initial`, a program's continuous
// assign, or an input port copy, all reactive-home per 24.3.1 -- is not seen
// until the NEXT time slot's Observed pass, one whole slot late. It bites only
// when no Active-region process reads the same signal, because any Active
// reader's wake leaves the ring non-empty at `reactive.done` and Observed is
// then re-entered by accident.
//
// TODO: two candidate fixes, both larger than clause 24, so neither is taken
// here. Recorded in full so the next person does not re-derive them
// (docs/design/program-blocks.md §10):
//   1. Point `reactive.done`'s else-edge at `wheel.check`. Rejected: it HANGS.
//      `wheel.check`'s own re-settle leaves only via `check`, and the
//      reactive->Observed edge pops no ring entry, so it bumps no oscillation
//      counter and would spin without ever tripping the guard that aborts a
//      non-converging slot. A hang is a worse failure than a refusal.
//   2. Re-enter Observed conditionally -- "did reactive code write something an
//      Observed process samples". Rejected for now: it re-runs every Observed
//      body a second time in the slot. A clocking block survives that (it
//      detects its edge against `clocking_clk`, a one-slot latch, and
//      finalize_preponed clears `clocking_fired` per step), but an SVA monitor
//      detects its clock edge from the Preponed shadow, which does not change
//      within a step, so it would step its automaton twice for one clocking
//      event. Making that safe needs a per-monitor per-step latch, which is
//      clause 16 work, plus a termination argument for the extra pass.
//
// Deliberately conservative in one place, and it is cheaper to over-refuse a
// construct LRM 24.4 tells a testbench not to write than to decide it: an
// `inout` / `ref` program port collapses onto the design's own storage
// (build_inout_aliases), so clocking off one is safe as long as no program code
// writes it -- which needs a driver analysis this site does not run. 24.3 calls
// it a program signal, so it is refused with the rest.
const ValueSymbol* Lowerer::program_clock_signal(const Expression& e) {
    struct Scan : slang::ast::ASTVisitor<Scan, slang::ast::VisitFlags::AllGood> {
        const ValueSymbol* hit = nullptr;
        void               take(const ValueSymbol& vs) {
            if (hit == nullptr && is_program_signal(vs)) {
                hit = &vs;
            }
        }
        void handle(const NamedValueExpression& nv) {
            take(nv.symbol);
        }
        void handle(const HierarchicalValueExpression& hv) {
            take(hv.symbol);
        }
    };
    // A composite clock (`@(posedge (a & b))`) is refused when ANY operand is a
    // program signal: every operand drives the synthesized event net
    // (synth_event_net), so one reactive-home operand puts the whole edge one
    // slot late.
    Scan scan;
    e.visit(scan);
    return scan.hit;
}

// Whether this input clockvar's effective skew is an explicit `#0`. LRM 14.4's
// built-in default is `#1step`, a block-level `default input` item overrides
// that, and the item's own skew overrides both; slang folds neither fallback
// into ClockVarSymbol, which carries only what its own item wrote.
bool Lowerer::zero_input_skew(const ClockingBlockSymbol& cb, const ClockVarSymbol& cv) {
    const ClockingSkew skew = cv.inputSkew.hasValue() ? cv.inputSkew : cb.getDefaultInputSkew();
    if (!skew.hasValue()) {
        return false;
    }
    if (skew.edge != slang::ast::EdgeKind::None) {
        // TODO: an edge-based skew (`input posedge`, LRM 14.4): the sample is
        // taken on the named edge of the clocking signal, a second event for the
        // block to step on.
        fail("an edge-based clocking skew is not supported");
    }
    if (skew.delay->kind == TimingControlKind::OneStepDelay) {
        return false;
    }
    if (skew.delay->kind == TimingControlKind::Delay &&
        const_uint(skew.delay->as<DelayControl>().expr, "a clocking skew delay (LRM 14.4)") == 0) {
        return true;
    }
    // TODO: a nonzero input skew other than `#1step`. The value sampled is the
    // one the signal held `skew` time units before the clocking event (LRM
    // 14.13), which is not knowable when the edge arrives: it needs a
    // per-clockvar history keyed by time, not a one-step Preponed shadow.
    fail("an input skew other than `#1step` or `#0` is not supported");
}

// Whether this clockvar's effective output skew is the LRM 14.4 default of
// `#0`, which drives in the Re-NBA region of the clocking event's own time
// step. Anything else is refused.
void Lowerer::check_output_skew(const ClockingBlockSymbol& cb, const ClockVarSymbol& cv) {
    const ClockingSkew skew = cv.outputSkew.hasValue() ? cv.outputSkew : cb.getDefaultOutputSkew();
    if (!skew.hasValue()) {
        return;
    }
    if (skew.edge != slang::ast::EdgeKind::None) {
        // TODO: an edge-based output skew (`output negedge`, LRM 14.4), which
        // drives on the named edge of the clocking signal instead.
        fail("an edge-based clocking skew is not supported");
    }
    if (skew.delay->kind == TimingControlKind::Delay &&
        const_uint(skew.delay->as<DelayControl>().expr, "a clocking skew delay (LRM 14.4)") == 0) {
        return;
    }
    // TODO: a nonzero output skew, which drives in the Re-NBA region of a FUTURE
    // time step (LRM 14.16) and so needs a scheduled maturation rather than this
    // block's own event.
    fail("an output skew other than `#0` is not supported");
}

// Collect: the block's own state, and storage for each clockvar. Registered here
// rather than in Emit for the reason collect_signals registers ordinary
// variables there -- the InstanceScope tree is frozen after Collect, and a
// reader in another scope (`@(bus.cb)`, `bus.cb.sig`) resolves against storage
// that must already exist.
//
// A `global clocking` block (LRM 14.14) lowers exactly like any other: its
// grammar admits no clocking items, so it is an event and nothing else, and
// `$global_clock` names that event.
void Lowerer::collect_clocking_block(const ClockingBlockSymbol& cb) {
    // Checked in Collect so an unsupported clock is reported before any reader
    // resolves a clockvar of this block. The signal itself resolves in Emit,
    // once every scope has been collected.
    const SignalEventControl& cev = clocking_event(cb);
    // Same defect as the assertion clock above, same region, same reason: the
    // block's sampling process is an Observed body (LRM 14.13), and a clock
    // written only in the reactive region set is not seen until the next slot's
    // Observed pass. Checked here in Collect, with the event shape, so no reader
    // of a clockvar of this block ever resolves against storage that is about to
    // be refused. Full analysis, and the two candidate fixes, on
    // program_clock_signal.
    if (const ValueSymbol* psig = program_clock_signal(cev.expr)) {
        fail("a clocking block clocked by the program signal `" + std::string(psig->name) +
             "` is not supported: the block samples in the Observed region, which Clotho does "
             "not re-enter after the reactive region set drains (LRM 24.3.1 requires the "
             "scheduler to iterate back around the outer loop through Observed), so it would "
             "sample a full time slot late -- clock it from a design signal, which is what LRM "
             "24.4 asks for anyway");
    }

    ClockingBlockState st;
    st.event   = declare_temp(mask_type(1), "clocking_ev");
    st.clkprev = declare_temp(mask_type(1), "clocking_clk");
    // Only a `default` block may be unnamed, and an unnamed block's members
    // cannot be referenced (LRM 14.3), so this name reaches nothing but the
    // state field and the dump.
    const std::string base =
      cb.name.empty() ? "__clocking" + std::to_string(temp_id_++) : std::string(cb.name);
    for (const Symbol& m : cb.members()) {
        if (m.kind != SymbolKind::ClockVar) {
            // TODO: an `assertion_item_declaration` inside a clocking block
            // (LRM 14.3), which takes the block's event as its leading clocking
            // event under the restrictions of LRM 16.16(b).
            fail("a " + std::string(toString(m.kind)) + " declared inside a clocking block is not supported");
        }
        const ClockVarSymbol& cv = m.as<ClockVarSymbol>();
        const bool is_input  = cv.direction != slang::ast::ArgumentDirection::Out;
        const bool is_output = cv.direction != slang::ast::ArgumentDirection::In;
        const Expression* init = cv.getInitializer();
        if (init == nullptr ||
            (init->kind != ExpressionKind::NamedValue && init->kind != ExpressionKind::HierarchicalValue)) {
            // TODO: a hierarchical expression, slice or concatenation as a
            // clocking item (LRM 14.5). slang binds the plain `input a;` form to
            // the enclosing scope's `a` as a value reference, so anything else is
            // an explicit `= expression` binding.
            fail("a clocking item bound to an expression rather than to a signal is not supported");
        }
        const ir::Ref type = lower_type(cv.getType());
        if (value_cat(type) != ValueCat::scalar) {
            // TODO: a clockvar of string / unpacked aggregate / container type.
            // The sample is one whole-signal Load feeding one whole-signal Store,
            // so the type must have a flat plane value.
            fail("a clocking item must have a packed value");
        }
        ClockVarState var{ .sym = &cv };
        if (is_input) {
            // An `inout` is two clockvars sharing one name and one signal (LRM
            // 14.3), and reading it yields the sample, never the driven value --
            // which is why the two never share storage (LRM 14.16.1).
            ir::SignalInfo info;
            info.name    = prefix_ + base + "." + std::string(cv.name);
            info.loc     = to_source_loc(cv.location);
            info.type    = type;
            info.dir     = ir::SignalInfo::Direction::Internal;
            var.sampled  = register_signal(std::move(info));
            var.zero_skew = zero_input_skew(cb, cv);
            // Reads resolve through the ordinary signal path; an `output`-only
            // clockvar is never read (slang rejects one with ClockVarOutputRead),
            // so it deliberately gets no sigmap_ entry.
            sigmap_[&cv] = var.sampled;
        }
        if (is_output) {
            check_output_skew(cb, cv);
            var.drives     = true;
            var.drive_val  = declare_temp(type, "clocking_drv");
            var.drive_pend = declare_temp(mask_type(1), "clocking_pend");
            st.has_drives  = true;
        }
        st.vars.push_back(var);
    }
    // Always allocated, not just for a block with drives: `##0` (LRM 14.11) asks
    // whether this block's event has already occurred in the current time step,
    // and that is the same latch.
    st.fired = declare_temp(mask_type(1), "clocking_fired");
    preponed_clears_.push_back(st.fired);
    clocking_.emplace(&cb, std::move(st));
}

// Emit: the block's process. One per instance of the enclosing module, in the
// Observed region.
void Lowerer::lower_clocking_block(const ClockingBlockSymbol& cb) {
    const auto it = clocking_.find(&cb);
    if (it == clocking_.end()) {
        // Collect refused this block and recorded the diagnostic; emitting a
        // process against storage that does not exist would crash on top of it.
        return;
    }
    const ClockingBlockState& st = it->second;
    const SignalEventControl&  ev = clocking_event(cb);
    const ir::SourceLoc        l  = to_source_loc(cb.location);
    const ir::Signal           clock = event_signal(ev.expr);
    const ir::Edge             edge  = to_edge(ev.edge);

    ir::Builder b(module_.pool);
    // This slot's clocking signal against the value it held at the previous
    // slot's Observed point -- the detection a monitor uses, for the same
    // reason. Taking it at the settled point is what makes the block step once
    // per cycle however many deltas the clock's own driver needed.
    const ir::Index clk_now = b.resize(load_signal(clock, b, l), module_.pool.bit_type, l);
    const ir::Index clk_was = load_signal(st.clkprev, b, l);
    const ir::Index fired   = edge == ir::Edge::Positive
                                ? b.binary(ir::Inst::Tag::Band, clk_now, bit_not(clk_was, b, l), l)
                                : b.binary(ir::Inst::Tag::Band, clk_was, bit_not(clk_now, b, l), l);
    b.binary(ir::Inst::Tag::Store, b.signal(st.clkprev, l), clk_now, l);

    // "This block's clocking event occurred in the current time step": sticky
    // across a second Observed pass of the same slot, which sees no edge, and
    // cleared from the top of the next step by the Preponed process. It is what
    // makes a drive mature at a clocking event rather than at every settled
    // point (LRM 14.16), and what `##0` tests (LRM 14.11).
    const ir::Index fired_lv  = b.signal(st.fired, l);
    const ir::Index fired_val = b.binary(ir::Inst::Tag::Bor, load_signal(st.fired, b, l), fired, l);
    b.binary(ir::Inst::Tag::Store, fired_lv, fired_val, l);

    b.open_region();
    for (const ClockVarState& cv : st.vars) {
        if (cv.sampled == ir::Signal{ 0 }) {
            continue; // output-only: nothing to sample
        }
        const ir::Signal live = signal_of(cv.sym->getInitializer()->as<ValueExpressionBase>().symbol);
        // #1step reads the shadow of the step before; an explicit #0 reads the
        // live signal, here in Observed (LRM 14.4, 14.13).
        const ir::Signal src = cv.zero_skew ? live : sampled_shadow_of(live, "a clocking block");
        const ir::Index  dst = b.signal(cv.sampled, l);
        const ir::Index  val = load_signal(src, b, l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
    }
    // Last, after every sample above: LRM 14.13 guarantees a process that waits
    // for the clocking block reads the updated values.
    const ir::Index event_lv  = b.signal(st.event, l);
    const ir::Index event_val = bit_not(load_signal(st.event, b, l), b, l);
    b.binary(ir::Inst::Tag::Store, event_lv, event_val, l);
    const ir::ExtraSlice then_r = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_r = b.close_region(); // empty
    b.cond(fired, then_r, else_r, l);

    module_.add_process(ir::Process{ .activation = ir::Activation::Observed,
                                     // Not a subscription: an Observed process is resumed once per
                                     // time slot, never woken by a net event. Kept as the record of
                                     // which event steps this block, which is what the edge
                                     // detection above is derived from.
                                     .wait_events = { ir::WaitEvent{ .signal = clock, .edge = edge } },
                                     .self_start  = false,
                                     .span        = ir::SourceSpan{ l, l },
                                     .ir          = b.finish(),
                                     .scope       = cur_scope_id() });
}

// Emit: the block's Re-NBA process, which performs the synchronous drives that
// matured at this time slot's clocking event (LRM 14.16).
//
// Why a process in that region rather than a deferred nonblocking record: LRM
// 14.16 puts the value change in "the Re-NBA region of the time step
// corresponding to the clocking event", and a drive may be issued by code that
// runs at that very event -- an Active-region process woken by the clocking
// block event, or a program's Reactive code -- so the maturation cannot be
// decided any earlier than this. Everything the drive needs was already
// evaluated at drive time (the RHS is sampled immediately, LRM 14.16), so the
// body is a per-clockvar `if (pending) drive`.
//
// Idempotent: it clears each pending flag and the block's `fired` flag, so a
// second trip through the region in one time slot (LRM 4.4.2.8's loop) drives
// nothing more. LRM 14.16.2 describes exactly that as the glitch case.
void Lowerer::lower_clocking_drives(const ClockingBlockSymbol& cb) {
    const auto it = clocking_.find(&cb);
    if (it == clocking_.end() || !it->second.has_drives) {
        return;
    }
    const ClockingBlockState& st = it->second;
    const ir::SourceLoc       l  = to_source_loc(cb.location);

    ir::Builder     b(module_.pool);
    const ir::Index fired = load_signal(st.fired, b, l);
    b.open_region();
    for (const ClockVarState& cv : st.vars) {
        if (!cv.drives) {
            continue;
        }
        const ir::Signal live = signal_of(cv.sym->getInitializer()->as<ValueExpressionBase>().symbol);
        if (module_.signal(live).is_net) {
            // TODO: a net-typed clocking block output (LRM 14.16). The block owns
            // a `'z`-initialized (strong1, strong0) driver on the net, resolved
            // against the net's other drivers, rather than writing it directly --
            // which is a driver-registration question, not a scheduling one.
            fail("a clocking block output on a net is not supported");
        }
        const ir::Index pend = load_signal(cv.drive_pend, b, l);
        b.open_region();
        const ir::Index live_lv   = b.signal(live, l);
        const ir::Index drive_val = load_signal(cv.drive_val, b, l);
        b.binary(ir::Inst::Tag::Store, live_lv, drive_val, l);
        const ir::Index pend_lv = b.signal(cv.drive_pend, l);
        const ir::Index zero    = zero_mask(1, b, l);
        b.binary(ir::Inst::Tag::Store, pend_lv, zero, l);
        const ir::ExtraSlice drive_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice skip_r = b.close_region(); // empty
        b.cond(pend, drive_r, skip_r, l);
    }
    const ir::ExtraSlice then_r = b.close_region();
    b.open_region();
    const ir::ExtraSlice else_r = b.close_region(); // empty
    b.cond(fired, then_r, else_r, l);

    module_.add_process(ir::Process{ .activation = ir::Activation::ReNba,
                                     .self_start = false,
                                     .span       = ir::SourceSpan{ l, l },
                                     .ir         = b.finish(),
                                     .scope      = cur_scope_id() });
}

// The `default clocking` in scope (LRM 14.12), which is what a cycle delay
// counts the events of (LRM 14.11). slang resolves 14.12's scope rule -- the
// containing instance or checker, never an instantiated one -- and has already
// reported a `##n` with no default clocking in scope (diag::NoDefaultClocking),
// so a null here would be a clotho bug rather than a source error.
const Lowerer::ClockingBlockState& Lowerer::default_clocking_state() {
    const Symbol* cb = scope_->getCompilation().getDefaultClocking(*scope_);
    if (cb == nullptr) {
        fail("a cycle delay needs a `default clocking` in scope (LRM 14.11)");
    }
    return clocking_state(cb->as<ClockingBlockSymbol>());
}

// `##n` as a statement (LRM 14.11): "wait for the specified number of clocking
// block events" of the default clocking. The count is evaluated once, and
// because it is the block's EVENT that is waited on rather than the clock, a
// `##1` executed away from the edge delays a fraction of a cycle, exactly as
// 14.11 describes.
//
// `##0` is the special case: it suspends only if the clocking event has not
// already occurred in this time step, and falls through if it has. The count is
// tested at run time rather than folded, so a non-constant one behaves the same.
//
// Unlike a repeat count, the count here can only be integral: 14.11 says it
// "can be any SystemVerilog expression that evaluates to a non-negative integer
// value", and slang enforces that at bind time (CycleDelayControl::fromSyntax
// calls requireIntegral, so `##(2.5)` is diag::ExprMustBeIntegral and never
// reaches lowering). A real here would be a clotho bug, not a source error,
// and would silently intern a float type into the counter cells below.
void Lowerer::lower_cycle_delay(const CycleDelayControl& cd, ir::Builder& b, ir::SourceLoc l) {
    const ClockingBlockState& st = default_clocking_state();
    if (cd.expr.type->isFloating()) {
        fail("internal: a real cycle delay count reached lowering");
    }
    const std::vector<ir::WaitEdge> edges{ ir::WaitEdge{ st.event, static_cast<uint32_t>(ir::Edge::Any) } };
    const ir::Ref                   ty         = lower_type(*cd.expr.type);
    const TempCell                  bound      = make_temp_cell(ty, "cycle_bound");
    const TempCell                  counter    = make_temp_cell(ty, "cycle_count");
    const ir::Index                 bound_lv   = bound.lv(b, l);
    const ir::Index                 bound_init = b.resize(lower_expr(cd.expr, b), ty, l);
    b.binary(ir::Inst::Tag::Store, bound_lv, bound_init, l);
    const ir::Index counter_lv   = counter.lv(b, l);
    const ir::Index counter_init = b.constant(module_.pool.put_int(ty, 0), l);
    b.binary(ir::Inst::Tag::Store, counter_lv, counter_init, l);

    const ir::Index bound_val = b.unary(ir::Inst::Tag::Load, bound.lv(b, l), l);
    const ir::Index zero      = b.constant(module_.pool.put_int(ty, 0), l);
    const ir::Index n_is_zero = b.binary(ir::Inst::Tag::Eq, bound_val, zero, l);

    // ##0: synchronise to this cycle's event, or continue if it already happened.
    b.open_region();
    {
        const ir::Index seen = load_signal(st.fired, b, l);
        b.open_region();
        b.wait(edges, l);
        const ir::ExtraSlice wait_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice done_r = b.close_region(); // empty
        b.cond(bit_not(seen, b, l), wait_r, done_r, l);
    }
    const ir::ExtraSlice zero_r = b.close_region();

    // ##n, n > 0: wait for the event n times. Same shape as a `repeat (N) @e`
    // intra-assignment control, and for the same reason.
    b.open_region();
    {
        b.open_region();
        const ir::Index      cur  = b.unary(ir::Inst::Tag::Load, counter.lv(b, l), l);
        const ir::Index      lim  = b.unary(ir::Inst::Tag::Load, bound.lv(b, l), l);
        const ir::Index      pred = b.binary(ir::Inst::Tag::Lt, cur, lim, l);
        const ir::ExtraSlice cond = b.close_region();

        b.open_region();
        b.wait(edges, l);
        const ir::ExtraSlice body = b.close_region();

        b.open_region();
        const ir::Index sc   = b.unary(ir::Inst::Tag::Load, counter.lv(b, l), l);
        const ir::Index one  = b.constant(module_.pool.put_int(ty, 1), l);
        const ir::Index slot = counter.lv(b, l);
        const ir::Index next = b.binary(ir::Inst::Tag::Add, sc, one, l);
        b.binary(ir::Inst::Tag::Store, slot, next, l);
        const ir::ExtraSlice step = b.close_region();

        b.loop(cond, pred, body, step, /*bottom_test*/ false, l);
    }
    const ir::ExtraSlice many_r = b.close_region();

    b.cond(n_is_zero, zero_r, many_r, l);
}

// The `global clocking` block `$global_clock` names, resolved from `scope_` (LRM
// 14.14: the enclosing instance scope, then upward through parent instance
// scopes). slang implements that walk and has already reported the absence of one
// (diag::NoGlobalClocking), so a null here would be a clotho bug.
const ClockingBlockSymbol& Lowerer::global_clocking() {
    const Symbol* gc = scope_->getCompilation().getGlobalClockingAndNoteUse(*scope_);
    if (gc == nullptr) {
        fail("`$global_clock` needs a `global clocking` declaration (LRM 14.14)");
    }
    return gc->as<ClockingBlockSymbol>();
}

// The clocking block a `$global_clock` call names, or null for any other
// expression. slang binds `$global_clock` as an event-typed system call, so there
// is no symbol on it to follow -- the block comes from the scope instead.
const ClockingBlockSymbol* Lowerer::global_clock_call(const Expression& e) {
    if (e.kind != ExpressionKind::Call) {
        return nullptr;
    }
    const CallExpression& call = e.as<CallExpression>();
    if (!call.isSystemCall() || call.getSubroutineName() != "$global_clock") {
        return nullptr;
    }
    return &global_clocking();
}

// The event of the clocking block an event control NAMES, for the two forms that
// name one instead of writing a signal edge: `@(cb)` (LRM 14.15, and 16.14.6's
// inferable "event expression consisting solely of a clocking block identifier")
// and `@($global_clock)` (LRM 14.14). Both stand for the block's own clocking
// event, which is the same substitution LRM 16.16(a) makes for a default
// clocking. Any other control is returned unchanged.
const TimingControl& Lowerer::clocking_event_of(const TimingControl& tc) {
    if (tc.kind != TimingControlKind::SignalEvent) {
        return tc;
    }
    const Expression& e = tc.as<SignalEventControl>().expr;
    if (e.kind == ExpressionKind::ArbitrarySymbol) {
        const Symbol& sym = *e.as<ArbitrarySymbolExpression>().symbol;
        if (sym.kind == SymbolKind::ClockingBlock) {
            return clocking_event(sym.as<ClockingBlockSymbol>());
        }
    }
    if (const ClockingBlockSymbol* gc = global_clock_call(e)) {
        return clocking_event(*gc);
    }
    return tc;
}

// `cb.sig <= expr` (LRM 14.16): a synchronous drive. It does not block, and it
// does not write the target now -- the RHS is evaluated immediately and the
// drive is performed at the clocking event, by the block's Re-NBA process. So
// this is two ordinary stores into that block's per-clockvar state, and a
// second drive in the same cycle simply overwrites them, which is LRM 14.16.2's
// "the last value is the only value driven".
void Lowerer::lower_clocking_drive(const ClockVarSymbol& cv, const AssignmentExpression& a, ir::Builder& b) {
    const auto& cb = cv.getParentScope()->asSymbol().as<ClockingBlockSymbol>();
    const ClockingBlockState& st = clocking_state(cb);
    const ClockVarState*      var = nullptr;
    for (const ClockVarState& c : st.vars) {
        if (c.sym == &cv) {
            var = &c;
        }
    }
    if (var == nullptr || !var->drives) {
        fail("a drive of a clocking block input is not legal SystemVerilog");
    }
    if (a.timingControl != nullptr) {
        // TODO: `cb.sig <= ##n expr` (LRM 14.16), which postpones the drive by n
        // cycles of this clockvar's clocking block. slang has already rejected
        // every other intra-assignment delay form here (ClockVarBadTiming).
        fail("a cycle delay on a synchronous drive is not supported");
    }
    if (a.left().kind != ExpressionKind::NamedValue && a.left().kind != ExpressionKind::HierarchicalValue) {
        // TODO: a bit-select or slice of a clockvar as the drive target (LRM
        // 14.16 allows both). The pending value is one whole-signal store, so a
        // partial drive needs a per-bit pending mask.
        fail("a synchronous drive of a bit-select or slice of a clockvar is not supported");
    }
    const ir::SourceLoc l        = loc(a);
    const ir::Index     drive_lv = b.signal(var->drive_val, l);
    const ir::Index     drive    = b.resize(lower_expr(a.right(), b), module_.signal(var->drive_val).type, l);
    b.binary(ir::Inst::Tag::Store, drive_lv, drive, l);
    const ir::Index pend_lv = b.signal(var->drive_pend, l);
    const ir::Index yes     = true_const(b, l);
    b.binary(ir::Inst::Tag::Store, pend_lv, yes, l);
}

// The lowered state of a clocking block, for a reader outside it (`@(cb)`).
// Collect fills the table for the whole design before Emit begins, so a
// missing entry means the block was refused and this reader is unreachable.
const Lowerer::ClockingBlockState& Lowerer::clocking_state(const ClockingBlockSymbol& cb) {
    const auto it = clocking_.find(&cb);
    if (it == clocking_.end()) {
        fail("reference to a clocking block that could not be lowered");
    }
    return it->second;
}

// Level-sensitive wait (LRM 9.4.3): suspend until `cond` is true, then run
// the body. Lowered as `forever begin if (cond) break; @(any-change on cond's
// reads); end` followed by the body. An already-true cond breaks on the first
// iteration and falls straight through in zero time (LRM: no suspend); a
// false cond parks on an any-change wait of every signal cond reads and
// re-tests on wake. The break uses `if`'s truth (definite 1 iff any bit is
// definitely 1), so an X cond takes the else path and keeps waiting until a
// definite 1 — matching iverilog. (A top-tested `while (!cond)` is
// wrong here: RedOr of a 4-state cond yields X, not a must-plane 0, so the
// negated loop predicate is non-definite and the loop exits on X.)
//
// A constant-false or read-less cond (`wait(0)`) yields an empty wait event
// list: a suspend with no waker, so the process parks permanently while the
// rest of the simulation proceeds. iverilog does the same (and additionally
// warns at elaboration). Verified against iverilog 13: zero-time
// passthrough, false->true resume, X-blocks, and const-false permanent park
// all match.
void Lowerer::lower_wait_stmt(const WaitStatement& w, ir::Builder& b) {
    const ir::SourceLoc l = loc(w);
    // The wait sensitivity: every signal the condition reads, each as an
    // any-change (level) event. Fails loudly on a string operand (wait
    // queues are not string-aware — see collect_expr_reads).
    std::vector<ir::WaitEvent> reads;
    collect_expr_reads(w.cond, reads);
    // A member read through a virtual interface is not one of those signals:
    // the collector sees the handle variable, not the member. Gather the member
    // accesses separately and park on each one's instance (LRM 25.9). The
    // handles are evaluated inside the loop body, so a retargeted handle is
    // re-read on every pass, and so that the values dominate the wait.
    const std::vector<const MemberAccessExpression*> vif_reads = vif_member_reads(w.cond);
    // Class properties the condition reads (LRM 9.4.3). They are not signals
    // either -- a property is heap storage inside an object -- so each one parks
    // on its own hidden any-change notifier, whose object handle is lowered inside
    // the loop body like a virtual interface's, so a reassigned handle is re-read
    // on every pass.
    ClassPropReadScan prop_reads;
    w.cond.visit(prop_reads);
    // Fork-captured automatics the condition reads. Each lives in a GC box (a
    // one-field synthetic class, docs/design/fork-join.md V4), not a signal, so
    // the signal collector above sees none of them; each parks on its box's
    // hidden change notifier instead, minted by box_class_for because the
    // design-wide pre-pass (WaitPropScan) saw this very statement. A boxed read
    // whose type can carry no notifier is refused loudly below, exactly like
    // the class-property case.
    std::vector<const ValueSymbol*> boxed_reads;
    {
        ankerl::unordered_dense::set<const slang::ast::ValueSymbol*> named;
        WaitLocalScan                                                scan;
        scan.out = &named;
        w.cond.visit(scan);
        for (const ValueSymbol* vs : named) {
            if (!is_boxed_symbol(*vs)) {
                continue;
            }
            if (!box_can_notify(vs->getType().getCanonicalType())) {
                // TODO: same aggregate shapes as the property arm below, plus
                // `event`, whose `.triggered` a capture box cannot notify on yet.
                fail("wait on the fork-captured automatic '" + std::string(vs->name) + "' of type " +
                     vs->getType().toString() +
                     " is not supported: a capture box of this type has no change notifier");
            }
            boxed_reads.push_back(vs);
        }
        // Set iteration order is hash-dependent; sort by name then location so
        // the emitted IR is deterministic.
        std::ranges::sort(boxed_reads, [](const ValueSymbol* a, const ValueSymbol* c) {
            if (a->name != c->name) {
                return a->name < c->name;
            }
            return a->location < c->location;
        });
    }
    if (reads.empty() && vif_reads.empty() && prop_reads.notifiable.empty() &&
        prop_reads.event_props.empty() && boxed_reads.empty() && prop_reads.any_prop) {
        // The condition reads a class property that can carry no notifier -- an
        // unpacked struct or union, or a FIXED unpacked array -- and nothing else
        // that can wake this process. Lowering it anyway is what used to happen,
        // and it produced a suspend with an EMPTY event list: a permanent park
        // with no diagnostic, in a design whose author had every reason to expect
        // a working wait. `wait(0)` still parks silently, because that one reads
        // nothing at all and is the documented LRM 9.4.3 behaviour.
        //
        // TODO: the two aggregate shapes. Both need a comparison over the whole
        // aggregate rather than over one word: an unpacked struct or union needs
        // the store path to walk its leaves, and a fixed unpacked array needs the
        // notifier to survive the element-store descent (agg_member clears it on
        // purpose today, see ir/llvm/Aggregates.cpp).
        fail("wait on a class property of a type with no change notifier is not "
             "supported (an unpacked struct, union or fixed unpacked array "
             "property cannot be waited on)");
    }

    // forever { if (cond) break; @(reads); }
    b.open_region();
    const ir::Index      lpred = true_const(b, l);
    const ir::ExtraSlice lcond = b.close_region();

    b.open_region();
    {
        const ir::Index c = lower_predicate(w.cond, b);
        b.open_region();
        b.break_(l);
        const ir::ExtraSlice then_r = b.close_region();
        b.open_region();
        const ir::ExtraSlice else_r = b.close_region(); // empty
        b.cond(c, then_r, else_r, l);
        std::vector<ir::WaitEdge> edges;
        edges.reserve(reads.size() + vif_reads.size() + prop_reads.notifiable.size() +
                      prop_reads.event_props.size());
        for (const ir::WaitEvent& e : reads) {
            edges.push_back(ir::WaitEdge{ e.signal, static_cast<uint32_t>(e.edge) });
        }
        for (const MemberAccessExpression* ma : vif_reads) {
            const std::optional<ir::Signal> rep =
              vif_member_signal(ma->member, vif_named_body(ma->value()),
                                "'" + std::string(ma->member.name) + "'");
            const ir::Index inst = lower_expr(ma->value(), b);
            if (rep) {
                edges.push_back(ir::WaitEdge{ *rep, static_cast<uint32_t>(ir::Edge::Any),
                                              ir::kNoWaitHandle, inst });
            }
        }
        for (const Expression* pe : prop_reads.notifiable) {
            edges.push_back(ir::WaitEdge{ ir::Signal{ 0 },
                                          static_cast<uint32_t>(ir::Edge::Any),
                                          prop_notifier_handle(*pe, *level_waited_prop(*pe), ir::Edge::Any, b) });
        }
        for (const Expression* pe : prop_reads.event_props) {
            // Park on the event OBJECT the property holds right now, exactly
            // what `-> e` fires (LRM 9.4.3 re-tests the condition on wake, so
            // a trigger that leaves it false costs one re-test). The handle is
            // loaded inside the loop body, so a reassigned property is re-read
            // on every pass, like a vif handle above.
            edges.push_back(ir::WaitEdge{ ir::Signal{ 0 },
                                          static_cast<uint32_t>(ir::Edge::Any),
                                          lower_expr(*pe, b) });
        }
        for (const ValueSymbol* vs : boxed_reads) {
            // Park on the box's notifier event, dereferencing the box handle AT
            // THIS POINT in the body: a fresh iteration of an enclosing loop can
            // re-box (install_fork_decl_box allocates per entry), so the handle
            // is re-read on every pass, like a vif handle above.
            edges.push_back(ir::WaitEdge{ ir::Signal{ 0 },
                                          static_cast<uint32_t>(ir::Edge::Any),
                                          boxed_notifier_handle(*vs, b, l) });
        }
        // An empty list is legal and meaningful: `wait(0)` reads nothing and
        // parks the process for the rest of the run.
        b.wait(edges, l);
    }
    const ir::ExtraSlice body = b.close_region();

    b.open_region();
    const ir::ExtraSlice step = b.close_region(); // empty

    b.loop(lcond, lpred, body, step, /*bottom_test*/ false, l);

    lower_stmt(w.stmt, b);
}

} // namespace clotho::lower
