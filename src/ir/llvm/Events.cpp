// Events and waits: delays, wait queues, edge detection, event objects and wakeups.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Lower a #delay, then suspend; execution continues in a fresh resume block.
//
// A zero delay (#0) does not advance time: per the LRM the process parks in
// the inactive queue and is promoted back to active at the same instant,
// after everything else there settles. So route ticks==0 to the inactive
// queue (no wheel timer, no awaiter) and a nonzero delay to a wheel timer for
// `now + ticks`. When the amount is a compile-time constant we emit only the
// taken path; otherwise we branch at runtime (covers a delay expression that
// happens to evaluate to 0, which is also inactive).
//
// Reactive code parks a #0 in the re-inactive batch instead -- its own
// region's dual (LRM 4.4.2.7) -- so a body that yields comes back in
// Reactive rather than Active; emit_zero_delay_push picks. A nonzero delay
// needs nothing special either: the awaiter records a parked handle, and the
// wheel's expiry trampoline wakes it into the ring that handle names. The
// later time slot runs its regions in order (the tick branches to the active
// drain, which settles into NBA, then Observed, then the reactive drain), so
// a reactive-home sleeper delivered by the wheel at the TOP of that slot is
// resumed in that slot's Reactive region -- which is where LRM 4.4.2.6 puts
// it.
void Backend::lower_delay(Index amount) {
    llvm::Value* ticks = cast(must_of(amount), 64, false);

    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ticks)) {
        if (c->isZero()) {
            emit_zero_delay_push(b_, hdl_);
        } else {
            // A nonzero delay is region-aware through the awaiter, above.
            emit_delay_timer(ticks);
        }
        emit_suspend("coro.resume", { .kind = RT_TRACE_CTL_DELAY, .delay = ticks });
        return;
    }

    // Runtime amount: branch on zero. Both arms fall through to one suspend,
    // and the zero arm parks region-aware exactly as the constant one does.
    auto* zero  = llvm::BasicBlock::Create(ctx_, "delay.zero", fn_);
    auto* timer = llvm::BasicBlock::Create(ctx_, "delay.timer", fn_);
    auto* susp  = llvm::BasicBlock::Create(ctx_, "delay.susp", fn_);
    b_.CreateCondBr(b_.CreateICmpEQ(ticks, b_.getInt64(0)), zero, timer);

    b_.SetInsertPoint(zero);
    emit_zero_delay_push(b_, hdl_);
    b_.CreateBr(susp);

    b_.SetInsertPoint(timer);
    emit_delay_timer(ticks);
    b_.CreateBr(susp);

    b_.SetInsertPoint(susp);
    emit_suspend("coro.resume", { .kind = RT_TRACE_CTL_DELAY, .delay = ticks });
}

// Schedule a wheel timer for `now + ticks` that resumes this coroutine: stash
// the handle in the awaiter so the timer callback can find it, then arm the
// wheel. Parked, so the trampoline knows which ring to wake it into. Caller
// emits the suspend.
//
// Where park_claim_wanted() holds -- a design with a remote `disable`, or with process
// control (a `process` class operation, `disable fork`, `wait fork`, or a program `initial`
// in a design that forks) -- the record is a fresh heap allocation per arm, remembered in
// the frame slot so a force-waker can find and claim it; the wheel frees it when it
// eventually fires. Fresh per arm because a force-woken process can carry on with its old
// timer still armed, so one reused record would have two owners.
//
// The scheduled flag is held SET across this park, as a `#0` park holds it
// (emit_zero_delay_push), and under the same gate: only a force-waker reads it here, and a
// design with neither a remote `disable` nor process control has none. The flag is what a
// force-wake (`disable`, `kill`,
// `disable fork`) reads once the park record is gone: when the timer fires, the
// trampoline pushes the handle and nulls the frame slot, and from then until the
// activation runs, "no record" must mean "already queued". An event wake sets the
// flag when it pushes and a `#0` park sets it when it parks, but nothing set it
// here, so an activation that had never waited on an event (an `initial` with
// only delays, a task) still had it clear. A force-wake landing in that window
// pushed a second copy of the handle, and the activation was resumed twice: the
// second resume cut its next `#delay` short, or, when the first had run the body
// to its end, resumed a finished coroutine (a crash). A body that holds the flag
// mid-body already (an `always` loop) is unchanged by this store.
void Backend::emit_delay_timer(llvm::Value* ticks) {
    auto*        aw_ty = awaiter_ty();
    auto*        slot  = awaiter();
    llvm::Value* aw    = slot;
    if (park_claim_wanted()) {
        const uint64_t size = env_->llmod.getDataLayout().getTypeAllocSize(aw_ty);
        aw                  = b_.CreateCall(alloc_fn(), { b_.getInt64(size) });
        b_.CreateStore(slot, b_.CreateStructGEP(aw_ty, aw, 3));
        b_.CreateStore(b_.getInt32(0), b_.CreateStructGEP(aw_ty, aw, 4));
        b_.CreateStore(aw, slot);
        if (sched_ptr_ != nullptr) {
            b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), sched_ptr_);
        }
    }
    b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(aw_ty, aw, 2));
    // Schedule-relative: the runtime adds `now` so this #delay crosses the ABI
    // once (rt_wheel_schedule_rel) rather than twice (rt_wheel_now then
    // rt_wheel_schedule).
    b_.CreateCall(wheel_schedule_rel_fn(), { wheel_, aw, ticks, enqueue_trampoline() });
}

// Store a { handle, flag_ptr } entry at a chosen slot of a queue array. The
// edge is no longer stored per entry — it is encoded by which sub-queue the
// entry lives in. The slot is addressed by element, so a writer of another
// unit's bucket names no capacity.
void Backend::emit_entry(llvm::Value* q, llvm::Value* slot_idx, llvm::Value* handle, llvm::Value* flag_ptr) {
    llvm::StructType* elem = wait_elem_ty();
    llvm::Value*      slot = b_.CreateInBoundsGEP(elem, q, slot_idx);
    b_.CreateStore(handle, b_.CreateStructGEP(elem, slot, 0));
    b_.CreateStore(flag_ptr, b_.CreateStructGEP(elem, slot, 1));
}

// Persistent (always-block) subscription, planned: where a process's entry goes. The
// seed's tables hold it (emit_seed), and one loop of root makes the stores. A static
// bucket takes the entry at a compile-time slot; slots are handed out in process /
// wait-event order, matching the capacities build_wait_queues computed, so each array
// fills exactly once and is never compacted. The store is a plain one of the
// process's ramp handle and scheduled-flag pointer, never a resume of the coroutine,
// so it does not inhibit coro-elide of other processes. All subscriptions land before
// any servicing, so the fixed-trip scans never read an empty slot.
//
// An event has no buckets: a node of the contiguous persistent array links onto the
// event object's waiter list. Edge is meaningless for an event (a trigger is not a
// transition), so it is ignored here -- every event subscription is recorded once, on
// the one list. The object is the one the variable's own handle names when the links
// are made, which a declaration initializer (`event e2 = e1`) may have changed.
Backend::StaticSub Backend::plan_static_subscribe(Signal sig, uint32_t edge) {
    const auto i = static_cast<uint32_t>(sig);
    StaticSub  sub;
    if (mod_.signal(sig).is_event) {
        sub.is_event = true;
        sub.slot     = llvm::ConstantExpr::getInBoundsGetElementPtr(
          event_nodes_arr_ty_, event_nodes_gv_,
          llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(event_node_next_++) });
        sub.handle = storage_address(slots_[i]);
        return sub;
    }
    const uint32_t slot = sta_next_[i][edge]++;
    sub.slot            = llvm::ConstantExpr::getInBoundsGetElementPtr(
      wait_elem_ty(), sym(wait_sta_gv_[i][edge]), llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(slot) });
    return sub;
}

// One-shot (intra-body @(sig)) registration: append this coroutine to the
// signal's dynamic queue, bumping .dyn.len. The entry is parked, so the scan
// that wakes it knows whether to push it onto the active or the reactive
// ring.
//
// Capacity is the count of distinct processes that intra-body-wait on the
// signal, plus one process-chain's worth for every (signal, edge) a task waits
// on, plus the fork-child activations that are not process chains at all
// (wait_counts). A process holds at most one dynamic slot per signal at a time.
//
// Guarded, because two waiters are not process chains and the capacity for them
// is an over-approximation rather than an exact count. A fork child is one (a
// width-N fork whose children all wait on the same edge parks N frames here);
// wait_counts now counts those by activation over the spawn / call graph, and
// this check is what makes a miscount a located FATAL instead of a store past
// the end of the bucket. Gated on design_forks(), so a design with no fork
// emits nothing: `len` is loaded for the push regardless.
//
// TODO: the other non-chain waiter is an action block pended by a concurrent
// assertion -- lower_reactive_pend builds a FRESH frame per failing attempt,
// and nothing serializes them, so N attempts that each park on the same
// (signal, edge) hold N slots at once, which no static count bounds. That one
// still needs a growable bucket, and in a design without a fork it is
// unguarded. Sizing it to a constant would only move the cliff.
void Backend::emit_dynamic_register(Signal sig, uint32_t edge) {
    const auto        i       = static_cast<uint32_t>(sig);
    const SignalSlot& s       = slots_[i];
    llvm::Value*      len_ptr = wait_base(WaitPart::DynamicLen, sig, edge);
    llvm::Value*      q       = wait_base(WaitPart::Dynamic, sig, edge);
    auto*             i32     = b_.getInt32Ty();
    llvm::Value*      len     = b_.CreateLoad(i32, len_ptr);
    // The bucket's capacity is every waiter of the signal's, the owner's own
    // among them, so a signal of another unit's code reads it from the signal's
    // port instead of baking it.
    llvm::Value* cap =
      port_reachable(s) && binding_of(s) != no_bind
        ? static_cast<llvm::Value*>(b_.CreateLoad(i32, port_elem_ptr(s, port_of(s)->dyn[edge].cap)))
        : b_.getInt32(static_cast<uint32_t>(wait_dyn_ty_[i][edge]->getNumElements()));
    emit_len_full_check(b_, len, cap, kRingWaitDynamic);
    emit_entry(q, len, parked_handle(b_, hdl_), sched_ptr_);
    b_.CreateStore(b_.CreateAdd(len, b_.getInt32(1)), len_ptr);
}

// Suspend the coroutine and continue in a fresh resume block, woken by a wait
// queue rather than a wheel timer. Like a #delay's suspend: the default edge
// returns to the resumer, case 0 resumes here, case 1 is a destroy while
// suspended.
void Backend::emit_wait_suspend(const TracePark& park) {
    emit_suspend("coro.wait_resume", park);
}

// Lower an intra-body `@(a or posedge b ...)` event wait: register on EVERY
// listed (signal, edge)'s dynamic queue, arm our scheduled flag, then suspend
// once. A matching blocking store to any of the signals wakes us
// (service_dynamic_queue pushes our handle onto the active queue and clears
// that bucket's length); the scheduler resumes us in the fresh resume block.
//
// Flag arming: service_dynamic_queue now reads the flag, so we must store 0 to
// our &proc_sched slot before suspending or we would never be woken again
// (emit_arm_wake_word). That store is also why a process with a static
// subscription cannot park here: its entry reads the same flag, finds it armed
// and resumes the process at this wait (B51), so a process whose body holds a
// Wait is not subscribed (waits_at_head).
//
// Eager removal (multi-signal only): when we resume, the bucket that fired is
// already cleared, but the OTHER buckets still hold our stale entry. Left in
// place they would spuriously wake us later and, in a forever loop, grow those
// buckets past their capacity (overflow). So compact ourselves out of each
// bucket on resume. A single-signal wait is in exactly one bucket, cleared on
// fire, so it needs no removal.
void Backend::lower_wait(ExtraIndex info) {
    lower_wait_edges(ir_->wait_edges(info));
}

// A process that waits at its head (waits_at_head) calls this with its leading list,
// at the top of its loop.
void Backend::lower_wait_edges(std::span<const WaitEdge> events, const SourceText* where) {
    // An event waiter links a frame-resident node onto the event object's list;
    // a signal waiter appends to the (signal, edge) dynamic bucket. A mixed
    // `@(e or clk)` does both -- they are independent registrations before one
    // suspend.
    uint32_t ev_k = 0;
    for (const WaitEdge& e : events) {
        if (e.inst != kNoWaitHandle) {
            // `@(edge vif.sig)`: park on the member's waiter list in the
            // instance the handle names. Frame-resident, so there is no
            // capacity to size -- the bucket path could not express this at all.
            llvm::Value* ctx = b_.CreateIntToPtr(must_of(e.inst), raw_ptr_ty());
            CtxOverride  ov(*this, ctx, slot_of(e.signal).anchor);
            emit_event_link(vif_head_ptr(e.signal, e.edge), event_wait_node(ev_k++),
                            parked_handle(b_, hdl_), sched_ptr_);
        } else if (e.handle != kNoWaitHandle) {
            // An event with no signal of its own -- an array element, a class
            // property, a formal. The frontend lowered its handle just before
            // this wait, so park on whatever object it names now.
            emit_event_link(event_obj_of(must_of(e.handle)), event_wait_node(ev_k++),
                            parked_handle(b_, hdl_), sched_ptr_);
        } else if (mod_.signal(e.signal).is_event) {
            // Resolve the variable's CURRENT handle: after `a = b`, `@(a)` must
            // park on b's object (LRM 15.5.5).
            emit_event_link(event_obj_for(e.signal), event_wait_node(ev_k++),
                            parked_handle(b_, hdl_), sched_ptr_);
        } else {
            emit_dynamic_register(e.signal, e.edge);
        }
    }
    emit_arm_wake_word(); // ready to be woken
    emit_wait_suspend({ .kind = RT_TRACE_CTL_EVENT, .edges = events, .where = where }); // park until any event fires
    // A remote `disable` is a second way to be resumed here, and it neither
    // fires nor clears the bucket we are in -- so with one in the design a
    // single-signal wait has to compact itself out too, or its stale entry would
    // wake us later at some unrelated suspend point.
    //
    // An event node is ALWAYS unlinked, even for a single-event wait: firing does
    // not clear the list (each waiter owns its node), so a node left linked would
    // be woken again by the next trigger.
    const bool remove_sigs = events.size() > 1 || park_claim_wanted();
    ev_k                   = 0;
    for (const WaitEdge& e : events) {
        if (e.inst != kNoWaitHandle) {
            // The head is read back from the node, like an event's: the handle
            // may have been retargeted while we were parked.
            emit_event_unlink(event_wait_node(ev_k++));
        } else if (e.handle != kNoWaitHandle) {
            // The object is read back from the node: the expression that named
            // it may not even be evaluable here (a loop index may have moved).
            emit_event_unlink(event_wait_node(ev_k++));
        } else if (mod_.signal(e.signal).is_event) {
            // Read back from the node as well: the variable may have been
            // reassigned while we were parked.
            emit_event_unlink(event_wait_node(ev_k++));
        } else if (remove_sigs) {
            emit_dynamic_remove(e.signal, e.edge);
        }
    }
}

// Arm the wake word: store 0, so that it reads "not queued" and a waker of the queue the
// process is about to park in pushes it (a waker pushes only if the word is 0, and sets it: S2).
// Every park that joins a queue of its own arms here -- an event control or `wait`, `expect`,
// a sync queue, `wait fork`, `await` -- and each of those is a TagInfo::arms_wake op.
//
// A statically subscribed body must never get here. Its static entry tests this word, so
// armed here it would wake the body at this park for an event the park does not name (B51).
// subscribed_statically excludes the bodies that hold such an op, so reaching this in one is
// an arming op that TagInfo does not name.
void Backend::emit_arm_wake_word() {
    assert(!static_sub_body_ && "a statically subscribed body parks where its own static entry wakes it (B51): "
                                "the op is missing TagInfo::arms_wake");
    b_.CreateStore(b_.getInt64(0), sched_ptr_);
}

// A handle-carrying wait edge names no signal (it parks on an object the body
// computes), so it must never be counted into a signal's bucket -- signal 0's,
// in particular, which its zero placeholder would otherwise inflate.
bool Backend::edge_is_handle(const WaitEdge& e) {
    return e.handle != kNoWaitHandle;
}

Backend::WaitCounts Backend::wait_counts() const {
    const auto n = mod_.signals().size();
    WaitCounts c{ std::vector<std::array<uint32_t, 3>>(n, { 0, 0, 0 }),
                  std::vector<std::array<uint32_t, 3>>(n, { 0, 0, 0 }) };
    // Static capacities count what the seed loop will actually subscribe:
    // one entry per schedule group (signal, edge) — a fused group subscribes
    // its canonical set once, not once per member, and the groups of several
    // instances that share a wake set subscribe once between them. The scan
    // reads every slot up to the capacity, so over-allocation would read a
    // null flag ptr.
    for (const SchedGroup& sg : sched_) {
        for (const WaitEvent& w : sg.events) {
            c.sta[static_cast<uint32_t>(w.signal)][static_cast<uint32_t>(w.edge)]++;
        }
    }
    const auto procs = mod_.processes();
    for (uint32_t k = 0; k < procs.size(); ++k) {
        const Process& p = procs[k];
        // Gather the distinct (signal, edge) pairs this process intra-body
        // waits on, across every Wait's event list. A process is a single
        // coroutine suspended at one wait at a time, so it holds at most one
        // dynamic slot per (signal, edge) — count each distinct pair once.
        std::vector<std::pair<Signal, uint32_t>> seen;
        // A process that waits at its head waits there for its leading list, as a
        // dynamic waiter like any other: those pairs come first, and a Wait on one of
        // them in its body costs no further slot.
        if (waits_at_head(k)) {
            for (const WaitEvent& w : p.wait_events) {
                const std::pair<Signal, uint32_t> key{ w.signal, static_cast<uint32_t>(w.edge) };
                if (!std::ranges::contains(seen, key)) {
                    seen.push_back(key);
                    c.dyn[static_cast<uint32_t>(w.signal)][static_cast<uint32_t>(w.edge)]++;
                }
            }
        }
        for (size_t i = 0; i < p.ir.inst_count(); ++i) {
            const auto idx = static_cast<Index>(i);
            if (p.ir.tag(idx) != Inst::Tag::Wait) {
                continue;
            }
            for (const WaitEdge& we : p.ir.wait_edges(p.ir.payload(idx).extra)) {
                if (edge_is_handle(we)) {
                    continue; // parks on an object, not a signal bucket
                }
                const std::pair<Signal, uint32_t> key{ we.signal, we.edge };
                if (!std::ranges::contains(seen, key)) {
                    seen.push_back(key);
                    c.dyn[static_cast<uint32_t>(we.signal)][we.edge]++;
                }
            }
        }
    }

    // Tasks also do intra-body @event waits, but a task is a coroutine created
    // per call and not counted above. Without fork, each process is a single
    // call-stack, so at most one task frame per process chain is suspended on a
    // given (signal, edge) at once — bound the extra dynamic slots by the
    // process count for every (signal, edge) any task waits on. (#delay uses the
    // wheel, not these queues, so it needs no capacity here.)
    const auto chains = static_cast<uint32_t>(mod_.processes().size());
    if (chains > 0) {
        std::vector<std::array<bool, 3>> task_waits(n, { false, false, false });
        for (size_t fi = 0; fi < mod_.function_count(); ++fi) {
            const Function& f = mod_.function(static_cast<FunctionId>(fi));
            if (!f.is_task) {
                continue;
            }
            for (size_t i = 0; i < f.ir.inst_count(); ++i) {
                const auto idx = static_cast<Index>(i);
                if (f.ir.tag(idx) != Inst::Tag::Wait) {
                    continue;
                }
                for (const WaitEdge& we : f.ir.wait_edges(f.ir.payload(idx).extra)) {
                    if (edge_is_handle(we)) {
                        continue; // parks on an object, not a signal bucket
                    }
                    task_waits[static_cast<uint32_t>(we.signal)][we.edge] = true;
                }
            }
        }
        for (uint32_t s = 0; s < n; ++s) {
            for (uint32_t e = 0; e < 3; ++e) {
                if (task_waits[s][e]) {
                    c.dyn[s][e] += chains;
                }
            }
        }
    }

    // A fork child is NOT a process chain, and neither is a task it calls, so
    // the chain term above does not cover either: it bounds task frames by the
    // number of single-call-stack process chains, which is exactly the invariant
    // a fork breaks by construction (the same one the active ring had to stop
    // assuming -- see body_activations). A width-N fork whose children all
    // `@(posedge clk)` parks N frames on one bucket while `chains` reserved
    // `mod_.processes().size()`, and emit_dynamic_register's store past the end
    // corrupts whatever global follows.
    //
    // So count the frames the chain term misses the way the ring counts them --
    // by activations over the call / spawn graph -- and add them per
    // (signal, edge). Restricted to bodies reachable from a fork child, because
    // everything else is already covered by `chains`; additive rather than a
    // replacement, so a design without a fork keeps exactly its previous
    // capacities and cannot regress.
    if (fork_child_count() != 0) {
        const BodyActivations            act = body_activations();
        std::vector<std::array<size_t, 3>> fork_dyn(n, { 0, 0, 0 });
        for (size_t fi = 0; fi < mod_.function_count(); ++fi) {
            const Function& f = mod_.function(static_cast<FunctionId>(fi));
            if (!f.is_task || act.from_fork[fi] == 0) {
                continue;
            }
            // One frame parks at one wait at a time, so each DISTINCT
            // (signal, edge) this body waits on costs mult[fi] slots -- the same
            // per-body dedup the process loop above does.
            std::vector<std::pair<Signal, uint32_t>> seen;
            for (size_t i = 0; i < f.ir.inst_count(); ++i) {
                const auto idx = static_cast<Index>(i);
                if (f.ir.tag(idx) != Inst::Tag::Wait) {
                    continue;
                }
                for (const WaitEdge& we : f.ir.wait_edges(f.ir.payload(idx).extra)) {
                    if (edge_is_handle(we)) {
                        continue; // parks on an object, not a signal bucket
                    }
                    const std::pair<Signal, uint32_t> key{ we.signal, we.edge };
                    if (std::ranges::contains(seen, key)) {
                        continue;
                    }
                    seen.push_back(key);
                    auto& slot = fork_dyn[static_cast<uint32_t>(we.signal)][we.edge];
                    slot       = std::min(slot + act.mult[fi], kMaxForkRingSlots);
                }
            }
        }
        for (uint32_t s = 0; s < n; ++s) {
            for (uint32_t e = 0; e < 3; ++e) {
                c.dyn[s][e] += static_cast<uint32_t>(fork_dyn[s][e]);
            }
        }
    }
    return c;
}

// One wait-queue entry, shared by both the static and dynamic queues:
// { ptr handle, ptr flag_ptr }.
//   handle   — the suspended coroutine to resume.
//   flag_ptr — &clotho.proc_sched[process]. Both queues read it to avoid
//              waking a process twice when several of the signals it waits on
//              change together (a multi-signal sensitivity or a multi-signal
//              @(a or b) intra-body wait).
// The persistent/transient distinction is encoded by which queue the entry
// lives in (.sta vs .dyn); the edge is encoded by which per-edge sub-queue it
// lives in (.pos/.neg/.any). Neither is a per-entry field anymore.
llvm::StructType* Backend::wait_elem_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(ctx_, { ptr, ptr });
}

// The event object (LRM 15.5): a collected object the runtime allocates
// (rt_event_new) and a handle names by address. Mirror of rt_event (Runtime.h).
//
//   { ptr vptr, i64 trig, ptr waiters, ptr next_pend, i32 vcd, i32 trig_id, i8 pending }
//     vptr      — the collector's descriptor word; generated code never reads it.
//     trig      — sim tick of the last fire, UINT64_MAX until the first one.
//                 `e.triggered` is `trig == now` (LRM 15.5.3).
//     waiters   — head of the intrusive waiter list (event_node_ty), or null.
//     next_pend — next object on the NBA pending chain of `->> e` (LRM 15.5.1).
//     vcd       — dump-var index, or -1 when no dump watches the object.
//     trig_id   — the object's id as a `<= @e` / `->> @(e)` control; 0 until it
//                 is one (rt_event_trig_id).
//     pending   — a `->> e` awaiting the NBA region.
//
// An event owns a LIST rather than the fixed-capacity per-(signal, edge)
// buckets a real signal uses, because a trigger has no value transition and
// because the waiter set of an event reached through a handle cannot be
// enumerated at compile time. See docs/design/event-handles.md.
llvm::StructType* Backend::event_obj_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(
      ctx_, { ptr, b_.getInt64Ty(), ptr, ptr, b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt8Ty() });
}

// One waiter on an event: an intrusive doubly-linked node.
//
//   { ptr next, ptr prev, ptr handle, ptr flag }
//
// `handle` and `flag` are exactly wait_elem_ty's two words -- the parked
// coroutine handle WITH its region tag bits intact (emit_wake_push strips
// them) and the opaque scheduled-flag pointer, which is a constant GEP into
// clotho.proc_sched for a process and a frame alloca for a task. Never
// interpret `flag`; store and compare it as an address.
//
// The two links are what buy an unbounded waiter set: a persistent
// (`always @(e)`) node comes from the contiguous clotho.event_nodes array, a
// one-shot (`@(e)` in a body) node is frame-resident, and both link and unlink
// in O(1) with no capacity to overflow. This is the discipline the remote
// `disable` activation list already uses (disable_node_ty).
llvm::StructType* Backend::event_node_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(ctx_, { ptr, ptr, ptr, ptr, ptr });
}

// The object an event HANDLE names. The handle is the object's address, so this
// is a cast -- except that a null handle names the design's null object
// (clotho.event_null). Nothing triggers that object, so its waiter list is only
// ever parked on and its trigger tick stays the never-triggered sentinel: LRM
// 15.5.5's `@(null)` and `null.triggered == 0`, for a select and no branch.
llvm::Value* Backend::event_obj_of(llvm::Value* handle) {
    return b_.CreateSelect(b_.CreateICmpEQ(handle, b_.getInt64(0)), sym(event_null_gv_),
                           event_obj_live(handle));
}

// The same for a handle the caller has already found non-null.
llvm::Value* Backend::event_obj_live(llvm::Value* handle) {
    return b_.CreateIntToPtr(handle, llvm::PointerType::getUnqual(ctx_));
}

// The 64-bit handle currently stored in an event variable's own signal slot.
// An event signal is a 2-state 64-bit plane, so this is a single load of
// field 0 of its slot.
llvm::Value* Backend::load_event_handle(Signal sig) {
    return b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(slot_of(sig).slot_ty,
                                                             storage_ptr(slot_of(sig)), 0));
}

llvm::Value* Backend::event_field(llvm::Value* obj, uint32_t field) {
    return b_.CreateStructGEP(event_obj_ty(), obj, field);
}

// The .pos/.neg/.any suffix for an edge bucket index (0=pos, 1=neg, 2=any).
const char* Backend::edge_suffix(uint32_t edge) {
    switch (static_cast<Edge>(edge)) {
        case Edge::Positive: return "pos";
        case Edge::Negative: return "neg";
        case Edge::Any: return "any";
    }
    return "any";
}

// The object an event VARIABLE names now.
llvm::Value* Backend::event_obj_for(Signal sig) {
    return event_obj_of(load_event_handle(sig));
}

// Per-signal wait queues, split into up to three per-edge sub-queues, each
// created only if it has at least one waiter:
//
//   clotho.wait.<sig>.sta.{pos,neg,any}  — persistent always-block
//     subscriptions for that edge. Filled at startup (priming) and never
//     pushed/popped; serviced by a fixed-trip scan (capacity is a compile-time
//     constant, so the optimizer can unroll it). No length global.
//   clotho.wait.<sig>.dyn.{pos,neg,any}  — one-shot intra-body @(edge sig)
//     waiters for that edge, each with its own .len of live entries; serviced
//     by a straight scan that wakes every entry (they all share the matched
//     edge) and resets the length.
//
// Splitting by edge hoists the per-entry edge test out of the scan: servicing
// guards each whole bucket with a single branch on the matching transition
// fact. Both kinds share the { handle, flag_ptr } element type.
void Backend::build_wait_queues() {
    const auto sigs = mod_.signals();
    WaitCounts c    = wait_counts();

    // Module-dedup prep, option (a): every instance of a reused definition
    // gets the per-def MAXIMUM bucket capacity for each def-local signal,
    // so twin bodies emit identical fixed-trip service scans regardless of
    // per-instance fan-out (subscribers cross instance boundaries through
    // ports). A padded slot is zeroinit — null flag pointer — and the
    // inline scan skips it (emit_static_scan_rt `padded`). The true
    // per-instance counts are kept for the NBA meta table, whose
    // commit-side scans stay exact and check-free.
    wait_sta_true_.assign(sigs.size(), { 0, 0, 0 });
    wait_sta_padded_.assign(sigs.size(), { 0, 0, 0 });
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        wait_sta_true_[i] = c.sta[i];
    }
    // Def-local signal table per scope occurrence: slot.path's last
    // element is the signal's field index within its own scope. Shared by
    // the capacity padding below, the instance-context layout, and block
    // translation (anchor_signals_).
    anchor_signals_.assign(anchors_.size(), {});
    std::vector<std::vector<Signal>>& by_anchor = anchor_signals_;
    {
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            const SignalSlot& sl = slots_[i];
            if (sl.anchor == SignalSlot::no_anchor) {
                continue;
            }
            const auto k = sl.path.back();
            auto&      v = by_anchor[sl.anchor];
            if (k >= v.size()) {
                v.resize(k + 1, Signal{ ~0u });
            }
            v[k] = static_cast<Signal>(i);
        }
        ankerl::unordered_dense::map<llvm::StructType*, std::vector<uint32_t>> groups;
        for (uint32_t a = 0; a < anchors_.size(); ++a) {
            groups[anchors_[a].ty].push_back(a);
        }
        for (const auto& [ty, members] : groups) {
            if (members.size() < 2) {
                continue;
            }
            const size_t nsig = by_anchor[members.front()].size();
            for (size_t j = 0; j < nsig; ++j) {
                for (uint32_t e = 0; e < 3; ++e) {
                    uint32_t max_sta = 0, max_dyn = 0;
                    uint32_t min_sta = ~0u;
                    for (const uint32_t a : members) {
                        const auto si = static_cast<uint32_t>(by_anchor[a][j]);
                        max_sta       = std::max(max_sta, c.sta[si][e]);
                        min_sta       = std::min(min_sta, c.sta[si][e]);
                        max_dyn       = std::max(max_dyn, c.dyn[si][e]);
                    }
                    // The padded (null-checked) scan shape must be uniform
                    // across every instance of the def — including the one
                    // whose true count equals the maximum — or twin bodies
                    // would still diverge.
                    for (const uint32_t a : members) {
                        const auto si = static_cast<uint32_t>(by_anchor[a][j]);
                        wait_sta_padded_[si][e] = min_sta < max_sta ? 1 : 0;
                        c.sta[si][e]            = max_sta;
                        c.dyn[si][e]            = max_dyn;
                    }
                }
            }
        }
    }
    wait_sta_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
    wait_sta_ty_.assign(sigs.size(), { nullptr, nullptr, nullptr });
    wait_sta_cap_.assign(sigs.size(), { 0, 0, 0 });
    wait_dyn_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
    wait_dyn_ty_.assign(sigs.size(), { nullptr, nullptr, nullptr });
    wait_dyn_len_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });
    sta_next_.assign(sigs.size(), { 0, 0, 0 });
    wait_sta_trig_gv_.assign(sigs.size(), { nullptr, nullptr, nullptr });

    // What events need of the design: the null object, the pending-chain head and
    // the persistent-node pool, the last sized by the same static-subscription
    // count that would have sized the .sta bucket. The nodes are contiguous so an
    // event's permanent waiter prefix stays sequential in memory even though the
    // walk is a pointer chase; one-shot waiters are frame-resident and need no
    // capacity at all. The objects themselves are the collector's (rt_event_new).
    uint32_t nnodes = 0;
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        if (sigs[i].is_event) {
            nnodes += c.sta[i][static_cast<uint32_t>(Edge::Any)];
        }
    }
    auto* obj_ty = event_obj_ty();
    auto* nullp  = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    // The object a null handle resolves to (event_obj_of): never triggered, never
    // dumped, and not a collected object, since no handle holds its address.
    event_null_gv_ = new llvm::GlobalVariable(
      env_->llmod, obj_ty, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantStruct::get(obj_ty, { nullp, llvm::ConstantInt::get(b_.getInt64Ty(), ~uint64_t{ 0 }),
                                          nullp, nullp,
                                          llvm::ConstantInt::get(b_.getInt32Ty(), ~uint32_t{ 0 }),
                                          llvm::ConstantInt::get(b_.getInt32Ty(), 0),
                                          llvm::ConstantInt::get(b_.getInt8Ty(), 0) }),
      "clotho.event_null");
    // Head of the NBA pending chain (`->> e`). A runtime list rather than a
    // compile-time set of target events, because the target is a handle.
    event_pend_head_gv_ = new llvm::GlobalVariable(
      env_->llmod, llvm::PointerType::getUnqual(ctx_), false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)),
      "clotho.event_pend_head");
    // The design-wide change notifiers: one handle each, filled by init_state.
    uint32_t nshared = 0;
    for (const SignalInfo& s : sigs) {
        if (s.shared_notify != kNoNotify) {
            nshared = std::max(nshared, s.shared_notify + 1);
        }
    }
    if (nshared != 0) {
        chg_shared_ty_ = llvm::ArrayType::get(b_.getInt64Ty(), nshared);
        chg_shared_gv_ = new llvm::GlobalVariable(env_->llmod,
                                                  chg_shared_ty_,
                                                  false,
                                                  llvm::GlobalValue::InternalLinkage,
                                                  llvm::ConstantAggregateZero::get(chg_shared_ty_),
                                                  "clotho.chg_shared");
    }
    event_nodes_arr_ty_ = llvm::ArrayType::get(event_node_ty(), nnodes);
    event_nodes_gv_ =
      nnodes == 0 ? nullptr
                  : new llvm::GlobalVariable(env_->llmod, event_nodes_arr_ty_, false,
                                             llvm::GlobalValue::InternalLinkage,
                                             llvm::ConstantAggregateZero::get(event_nodes_arr_ty_),
                                             "clotho.event_nodes");
    event_node_next_ = 0;

    auto* i32  = llvm::Type::getInt32Ty(ctx_);
    auto* elem = wait_elem_ty();
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        if (sigs[i].is_event) {
            // An event's waiters live on its object's list, so it needs no
            // per-edge buckets at all -- skip them rather than allocating
            // arrays nothing ever fills or scans.
            continue;
        }
        for (uint32_t e = 0; e < 3; ++e) {
            const std::string base = "clotho.wait." + sigs[i].name + "." ;
            if (c.sta[i][e] > 0) {
                auto* arr           = llvm::ArrayType::get(elem, c.sta[i][e]);
                wait_sta_ty_[i][e]  = arr;
                wait_sta_cap_[i][e] = c.sta[i][e];
                wait_sta_gv_[i][e]  = new llvm::GlobalVariable(
                  env_->llmod, arr, false, llvm::GlobalValue::InternalLinkage,
                  llvm::ConstantAggregateZero::get(arr), base + "sta." + edge_suffix(e));
                if (slots_[i].anchor == SignalSlot::no_anchor) {
                    // No record describes this bucket (a package variable's): its
                    // trigger bits live in a global of their own, which writers in
                    // any unit read (static_trig_bits).
                    wait_sta_trig_gv_[i][e] = new llvm::GlobalVariable(
                      env_->llmod, b_.getInt64Ty(), false, llvm::GlobalValue::InternalLinkage,
                      llvm::ConstantInt::get(b_.getInt64Ty(), 0), base + "sta." + edge_suffix(e) + ".trig");
                }
            }
            if (c.dyn[i][e] > 0) {
                auto* arr          = llvm::ArrayType::get(elem, c.dyn[i][e]);
                wait_dyn_ty_[i][e] = arr;
                wait_dyn_gv_[i][e] = new llvm::GlobalVariable(
                  env_->llmod, arr, false, llvm::GlobalValue::InternalLinkage,
                  llvm::ConstantAggregateZero::get(arr), base + "dyn." + edge_suffix(e));
                wait_dyn_len_gv_[i][e] = new llvm::GlobalVariable(
                  env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                  llvm::ConstantInt::get(i32, 0), base + "dyn." + edge_suffix(e) + ".len");
            }
        }
    }
}

// The dense trigger id for a (signal, edge) that fires a `<= @e` deferred store,
// or -1. Both wake paths call this to decide whether to fire event records.
int32_t Backend::trigger_id(Signal sig, uint32_t edge) const {
    return trigger_id_.empty() ? -1 : trigger_id_[static_cast<uint32_t>(sig)][edge];
}

bool Backend::signal_has_waiters(Signal sig) const {
    const auto i = static_cast<uint32_t>(sig);
    for (uint32_t e = 0; e < 3; ++e) {
        // A `<= @e` trigger has no coroutine waiter but its edge must still be
        // watched so a store/commit fires its deferred records.
        if (wait_sta_gv_[i][e] != nullptr || wait_dyn_gv_[i][e] != nullptr ||
            vif_wait_[i][e] || trigger_id(sig, e) >= 0) {
            return true;
        }
    }
    return false;
}

// Whether any waiter subscribed a level (any-change) wait on the signal. Only
// these consume the z word in edge classification, so a store may skip the z
// snapshot when none exist.
bool Backend::signal_has_level_waiters(Signal sig) const {
    const auto i = static_cast<uint32_t>(sig);
    const auto e = static_cast<uint32_t>(Edge::Any);
    return wait_sta_gv_[i][e] != nullptr || wait_dyn_gv_[i][e] != nullptr;
}

bool Backend::signal_has_bucket(Signal sig, Edge e) const {
    const auto i = static_cast<uint32_t>(sig);
    const auto k = static_cast<uint32_t>(e);
    return wait_sta_gv_[i][k] != nullptr || wait_dyn_gv_[i][k] != nullptr || vif_wait_[i][k] ||
           trigger_id(sig, k) >= 0;
}

// Load a signal's packed must|may state word (field 0 of the slot struct).
llvm::Value* Backend::load_mm(Signal sig) {
    const SignalSlot& s      = slot_of(sig);
    llvm::Value*      mm_ptr = b_.CreateStructGEP(s.slot_ty, storage_ptr(s), 0);
    return b_.CreateAlignedLoad(
      int_ty(2 * s.width), mm_ptr,
      env_->llmod.getDataLayout().getABITypeAlign(int_ty(2 * s.width)));
}

// Load the packed must|may word of an lvalue's selected storage: the whole
// signal for a plain target, or one element for an unpacked-array element.
// Used to snapshot old / new state around a blocking store so wait-queue
// servicing detects an element-level change, not just element 0.
// Bit width of a slot's stored value word: iW for a 2-state slot, the packed
// i(2W) of a 4-state one.
uint32_t Backend::slot_mm_bits(const LValueInfo& lv) const {
    return lv.four_state ? 2 * lv.store_width : lv.store_width;
}

llvm::Value* Backend::load_lv_mm(const LValue& lv) {
    return b_.CreateLoad(int_ty(slot_mm_bits(lv)), lv_mm_ptr(lv));
}

llvm::Value* Backend::load_lv_z(const LValue& lv) {
    return b_.CreateLoad(int_ty(lv.store_width), lv_z_ptr(lv));
}

// A blocking write to a module signal must service the signal's wait queues
// with the old -> new transition. This pair brackets every such write path
// (Store, plus the runtime pointer write-backs: $fgets / $sformat / $ferror /
// $*scanf / scalar $fread): snapshot the selected storage before the write,
// then detect the transition and service after. The snapshot is empty — and
// servicing a no-op — for a target that never wakes anyone: a frame local, a
// signal without waiters, or a PreInit store (variable declaration initializers
// generate no events).
//
// A place that may be invalid (an element select outside its container) is
// snapshotted through its stand-in (safe_lvalue), so the old value is defined and
// no wild address is read, and serviced only where it is valid: an invalid write
// changes nothing and wakes nobody.
Backend::ServiceSnapshot Backend::service_snapshot(const Guarded& g) {
    if (silent_stores_) {
        // A PreInit store: declaration initializers generate no events, and no
        // waiter can exist yet anyway.
        return {};
    }
    if (g.ref_wake_fn != nullptr && !g.is_string) {
        // A write through a `ref` formal: the old words, and the thunk the call passed to wake
        // the actual's waiters (null at run time when it has none).
        const LValue    lv = safe_lvalue(g);
        ServiceSnapshot old;
        old.mm      = load_lv_mm(lv);
        old.z       = lv.has_z ? load_lv_z(lv) : nullptr;
        old.ref_fn  = g.ref_wake_fn;
        old.ref_ctx = g.ref_wake_ctx;
        return old;
    }
    if (g.is_string) {
        // A string's value is its CONTENT, not the handle the place holds, so the
        // old value has to be copied out rather than loaded as a word. Covers
        // every string write path by construction, including a `ref` / `out` /
        // `inout` string formal, which reaches this same bracket.
        return ServiceSnapshot{ .str = str_chg_snapshot(g) };
    }
    if (g.chg != nullptr && g.chg->container) {
        // A container property never reaches here through a plane store (the
        // frontend has no such op) -- only through the `ref`/`inout` brackets,
        // whose lvalue is a handle with no mm word to load.
        return ServiceSnapshot{ .mm = nullptr, .z = nullptr, .container = true };
    }
    if (g.chg != nullptr) {
        // A waited-on class property (LRM 9.4.2). Snapshot unconditionally: the
        // notifier that exists is the one some `@`/`wait` asked for, so there is no
        // "has waiters" question to fold in -- an empty waiter list makes the wake
        // walk a single null load.
        const LValue    lv = safe_lvalue(g);
        ServiceSnapshot old;
        old.mm = load_lv_mm(lv);
        old.z  = lv.has_z ? load_lv_z(lv) : nullptr;
        return old;
    }
    if (!g.is_signal || !signal_has_waiters(g.sig)) {
        return {};
    }
    const LValue lv = safe_lvalue(g);
    // A store through the wide-select path writes only the words around its offset.
    // Loading and comparing the whole slot, as below, is O(width) for a one-bit
    // store: 16 Kbytes of loads and a 131072-bit compare for a 64 Kbit 4-state
    // vector. wide_window_snapshot takes just the words the store can touch.
    if (lv.wide_dyn) {
        if (std::optional<WideWindow> win = wide_window_snapshot(lv)) {
            ServiceSnapshot old;
            old.wide = std::move(win);
            return old;
        }
    }
    ServiceSnapshot old;
    old.mm = load_lv_mm(lv);
    if (lv.has_z && signal_has_level_waiters(lv.sig)) {
        old.z = load_lv_z(lv);
    }
    return old;
}

void Backend::service_after_write(const Guarded& g, ServiceSnapshot old) {
    g.when_valid(b_, [&](const LValue& lv) {
        // Wake buckets, trigger bits and the dump-var index of a written signal are
        // the owner's: its record, when the write reached it through a binding or a
        // virtual interface handle.
        CtxOverride ov(*this, lv.vif_ctx, lvalue_ctx_anchor(lv));
        if (old.ref_fn != nullptr) {
            // The callee classifies its own write (it knows the formal's leaf) and the thunk
            // the call passed wakes what the actual's owner has waiting.
            llvm::Value* new_mm = load_lv_mm(lv);
            llvm::Value* new_z  = old.z != nullptr ? load_lv_z(lv) : nullptr;
            const EdgeFacts f   = emit_edge_facts(old.mm, new_mm, lv.store_width, lv.four_state, old.z,
                                                  new_z, true, true, true);
            if (trace_on()) {
                // The callee's own bits; the call's thunk reports the write to the actual's waiters.
                TraceSeen t = trace_seen_words(old.mm, new_mm, old.z, new_z, lv.store_width, lv.four_state, lv.has_z);
                t.pos       = f.pos;
                t.neg       = f.neg;
                t.any       = f.changed;
                emit_trace_change(RT_TRACE_CHG_REF, std::nullopt, nullptr, t);
            }
            emit_ref_wake_call(old.ref_fn, old.ref_ctx, f.changed, f.pos, f.neg);
        } else if (old.str != nullptr) {
            str_chg_after(lv, old.str);
        } else if (old.container) {
            notify_container_mutation(lv);
        } else if (old.wide) {
            service_wide_window(lv, *old.wide);
        } else if (old.mm != nullptr && lv.chg != nullptr) {
            service_change_notify(lv, old.mm, load_lv_mm(lv), old.z,
                                  old.z != nullptr ? load_lv_z(lv) : nullptr);
        } else if (old.mm != nullptr) {
            // A dumped scalar's touch may ride on the Any bucket's scan, made only
            // on a change (the flush ignores a touch without one either way).
            const int32_t fold = design_uses_vcd_ && !silent_stores_ && lv.is_signal && !slot_of(lv.sig).is_array
                                   ? vcd_index_[static_cast<uint32_t>(lv.sig)]
                                   : -1;
            if (service_wait_queue(lv.sig, old.mm, load_lv_mm(lv), old.z, old.z != nullptr ? load_lv_z(lv) : nullptr,
                                   RT_TRACE_CHG_STORE, fold)) {
                return;
            }
        }
        // Mark the signal changed for the VCD dirty set. Independent of wait-queue
        // servicing: a dumped signal with no waiters still needs a touch.
        vcd_touch_lvalue(lv);
    });
}

// Classify a store's old -> new transition of a packed must|may word into wake
// facts per LRM 9.4.2 / Table 9-2. The edge state of bit 0 forms the lattice
// 0 < x/z < 1 — x and z encode identically in the planes (must 0 / may 1), and
// the LRM defines no edge between them — so posedge is any move up the lattice
// (0->1, 0->x, x->1) and negedge any move down. A 2-state slot cannot hold x,
// so its classifier stays the plain 0<->1 compare. Edges read only the LSB of
// each plane (LRM 9.4.2: an edge is detected only on the LSB).
//
// `changed` compares whole words: both planes and, when the slot carries a z
// word (old_z / new_z non-null), the z word too — x->z is a value change for a
// level wait even though the planes agree. Facts whose need flag is false stay
// null and emit no code.
Backend::EdgeFacts Backend::emit_edge_facts(llvm::Value* old_mm,
                                            llvm::Value* new_mm,
                                            uint32_t     width,
                                            bool         four_state,
                                            llvm::Value* old_z,
                                            llvm::Value* new_z,
                                            bool         need_pos,
                                            bool         need_neg,
                                            bool         need_any) {
    auto*     i1 = b_.getInt1Ty();
    EdgeFacts f;
    if (need_pos || need_neg) {
        llvm::Value* old_m   = b_.CreateTrunc(old_mm, i1); // must LSB
        llvm::Value* new_m   = b_.CreateTrunc(new_mm, i1);
        llvm::Value* old_may = nullptr;
        llvm::Value* new_may = nullptr;
        if (four_state) {
            auto* sh = llvm::ConstantInt::get(old_mm->getType(), width);
            old_may  = b_.CreateTrunc(b_.CreateLShr(old_mm, sh), i1);
            new_may  = b_.CreateTrunc(b_.CreateLShr(new_mm, sh), i1);
        }
        f = emit_edge_bits(old_m, new_m, old_may, new_may, need_pos, need_neg);
    }
    if (need_any) {
        f.changed = b_.CreateICmpNE(old_mm, new_mm);
        if (old_z != nullptr) {
            f.changed = b_.CreateOr(f.changed, b_.CreateICmpNE(old_z, new_z));
        }
    }
    return f;
}

// Table 9-2 posedge / negedge from the LSBs of the must and may planes before and
// after a store (see emit_edge_facts for the lattice). `old_may` / `new_may` are
// null for a 2-state slot, which cannot hold x, so it is the plain 0 <-> 1 compare.
Backend::EdgeFacts Backend::emit_edge_bits(llvm::Value* old_m,
                                           llvm::Value* new_m,
                                           llvm::Value* old_may,
                                           llvm::Value* new_may,
                                           bool         need_pos,
                                           bool         need_neg) {
    EdgeFacts f;
    if (old_may != nullptr) {
        if (need_pos) {
            f.pos = b_.CreateAnd(b_.CreateAnd(b_.CreateNot(old_m), new_may),
                                 b_.CreateOr(b_.CreateNot(old_may), new_m));
        }
        if (need_neg) {
            f.neg = b_.CreateAnd(b_.CreateAnd(old_may, b_.CreateNot(new_m)),
                                 b_.CreateOr(b_.CreateNot(new_may), old_m));
        }
    } else {
        if (need_pos) {
            f.pos = b_.CreateAnd(b_.CreateNot(old_m), new_m);
        }
        if (need_neg) {
            f.neg = b_.CreateAnd(old_m, b_.CreateNot(new_m));
        }
    }
    return f;
}

// The part of a wide-select store's target that the store can change, for
// service_snapshot to take in place of the whole slot. The store (rt_wide_ins)
// writes only the bits [off, off + n) that lie inside its container and the vector,
// so they all sit in K = ((n + 62) >> 6) + 1 consecutive 64-bit words of each
// plane (the most words an n-bit run can span) starting at the word of the offset.
// The offset is clamped into the vector and the window into the plane, so the
// loads stay inside the slot for ANY offset: a store that is out of range, has an
// x/z index or hangs off an end writes nothing outside the clamped window (or
// nothing at all), and its window compares equal. A select as wide as the vector
// leaves no window to save (nullopt: the caller takes the whole slot).
//
// A 4-state slot is { mm: must in bits [0, W), may in [W, 2W) ; z }: the may plane
// gets a window of its own, and so does the z field when a level waiter consumes it
// (the same condition the whole-slot snapshot uses). The `changed` fact compares
// exactly these. An edge reads only bit 0 of the must and may planes (LRM 9.4.2),
// so those two words are snapshotted at their fixed place instead of being found
// in the window, which keeps the edge facts independent of where the window is: the
// store reaches bit 0 only through a clamped offset of 0, i.e. a window that starts
// at word 0, so `pos` / `neg` still imply `changed`.
std::optional<Backend::WideWindow> Backend::wide_window_snapshot(const LValue& lv) {
    const uint32_t W     = lv.store_width;
    const uint32_t K     = ((lv.width + 62) >> 6) + 1;
    const uint32_t nw    = (W + 63) / 64; // words of one W-bit plane
    const uint32_t total = lv.four_state ? (2 * W + 63) / 64 : nw;
    if (K > nw) {
        return std::nullopt;
    }
    WideWindow w;
    w.words     = K;
    w.width     = W;
    w.four      = lv.four_state;
    w.need_any  = signal_has_bucket(lv.sig, Edge::Any);
    w.need_edge = signal_has_bucket(lv.sig, Edge::Positive) || signal_has_bucket(lv.sig, Edge::Negative);

    auto*        i64 = b_.getInt64Ty();
    llvm::Value* mm  = lv_mm_ptr(lv);
    const auto   at  = [&](llvm::Value* base, llvm::Value* word) {
        return b_.CreateInBoundsGEP(i64, base, word);
    };
    if (w.need_any) {
        llvm::Value* off = b_.CreateSExt(lv.bit_off, i64);
        llvm::Value* o   = smax(smin(off, llvm::ConstantInt::get(i64, W - 1)), llvm::ConstantInt::get(i64, 0));
        const auto   clamp = [&](llvm::Value* word, uint32_t last) {
            llvm::Value* hi = llvm::ConstantInt::get(i64, last);
            return b_.CreateSelect(b_.CreateICmpULT(word, hi), word, hi);
        };
        llvm::Value* wm = clamp(b_.CreateLShr(o, 6), nw - K);
        w.mm_ptr        = at(mm, wm);
        if (w.four) {
            w.may_ptr = at(mm, clamp(b_.CreateLShr(b_.CreateAdd(o, llvm::ConstantInt::get(i64, W)), 6), total - K));
        }
        if (lv.has_z && signal_has_level_waiters(lv.sig)) {
            w.z_ptr = at(lv_z_ptr(lv), wm);
        }
    }
    if (w.need_edge) {
        w.lsb_mm_ptr = mm;
        if (w.four) {
            w.lsb_may_ptr = at(mm, llvm::ConstantInt::get(i64, W >> 6));
        }
    }
    load_wide_window(w);
    return w;
}

// Load every plane window of `w` through its pointer into its value field.
void Backend::load_wide_window(WideWindow& w) {
    const auto load = [&](llvm::Value* p, uint32_t words) {
        return b_.CreateAlignedLoad(int_ty(64 * words), p, llvm::Align(8));
    };
    w.mm      = w.mm_ptr != nullptr ? load(w.mm_ptr, w.words) : nullptr;
    w.may     = w.may_ptr != nullptr ? load(w.may_ptr, w.words) : nullptr;
    w.z       = w.z_ptr != nullptr ? load(w.z_ptr, w.words) : nullptr;
    w.lsb_mm  = w.lsb_mm_ptr != nullptr ? load(w.lsb_mm_ptr, 1) : nullptr;
    w.lsb_may = w.lsb_may_ptr != nullptr ? load(w.lsb_may_ptr, 1) : nullptr;
}

// service_wait_queue for a windowed snapshot: the same wake facts, from the words
// the store could touch (wide_window_snapshot) rather than the whole slot.
void Backend::service_wide_window(const LValue& lv, const WideWindow& old) {
    WideWindow now = old;
    load_wide_window(now);

    EdgeFacts f;
    if (old.need_any) {
        f.changed = b_.CreateICmpNE(old.mm, now.mm);
        if (old.may != nullptr) {
            f.changed = b_.CreateOr(f.changed, b_.CreateICmpNE(old.may, now.may));
        }
        if (old.z != nullptr) {
            f.changed = b_.CreateOr(f.changed, b_.CreateICmpNE(old.z, now.z));
        }
    }
    if (old.need_edge) {
        auto*        i1      = b_.getInt1Ty();
        llvm::Value* old_m   = b_.CreateTrunc(old.lsb_mm, i1);
        llvm::Value* new_m   = b_.CreateTrunc(now.lsb_mm, i1);
        llvm::Value* old_may = nullptr;
        llvm::Value* new_may = nullptr;
        if (old.four) {
            auto* sh = llvm::ConstantInt::get(b_.getInt64Ty(), old.width & 63);
            old_may  = b_.CreateTrunc(b_.CreateLShr(old.lsb_may, sh), i1);
            new_may  = b_.CreateTrunc(b_.CreateLShr(now.lsb_may, sh), i1);
        }
        const EdgeFacts e = emit_edge_bits(old_m, new_m, old_may, new_may,
                                           signal_has_bucket(lv.sig, Edge::Positive),
                                           signal_has_bucket(lv.sig, Edge::Negative));
        f.pos = e.pos;
        f.neg = e.neg;
    }
    if (trace_on()) {
        // Bit 0 from the two words that hold it, the planes from the window the store can touch.
        TraceSeen t;
        t.four  = old.four;
        t.has_z = lv.has_z;
        if (old.need_edge) {
            auto*        i1 = b_.getInt1Ty();
            t.old_must      = b_.CreateTrunc(old.lsb_mm, i1);
            t.new_must      = b_.CreateTrunc(now.lsb_mm, i1);
            if (old.four) {
                auto* sh   = llvm::ConstantInt::get(b_.getInt64Ty(), old.width & 63);
                t.old_may  = b_.CreateTrunc(b_.CreateLShr(old.lsb_may, sh), i1);
                t.new_may  = b_.CreateTrunc(b_.CreateLShr(now.lsb_may, sh), i1);
            }
        }
        if (old.need_any) {
            t.diff = b_.CreateICmpNE(old.mm, now.mm);
            if (old.may != nullptr) {
                t.diff = b_.CreateOr(t.diff, b_.CreateICmpNE(old.may, now.may));
            }
            if (old.z != nullptr) {
                t.zdiff = b_.CreateICmpNE(old.z, now.z);
            }
        }
        t.pos = f.pos;
        t.neg = f.neg;
        t.any = f.changed;
        emit_trace_change(RT_TRACE_CHG_WINDOW, lv.sig, storage_ptr(slot_of(lv.sig)), t);
    }
    service_wake_buckets(lv.sig, f);
}

// Service a signal's wait queues after a blocking assignment to it. Computes
// the transition's edge facts once, then for each per-edge bucket that exists
// guards its whole scan with a single branch on the matching fact — no
// per-entry edge check. Emits nothing for a signal with no waiters. old_z /
// new_z are the z-word snapshots, or null when the slot has no z word or no
// level waiter consumes it.
//
// The three buckets are serviced independently (not else-if): a 0->1 LSB
// transition is both `changed` (any) and `pos`, so it services both buckets.
//
// `fold_vcd`: see the declaration. Only the Any bucket's port scan can take the
// touch, so a signal without one, or whose bucket another writer scans without
// a port, leaves it to the caller.
bool Backend::service_wait_queue(Signal       sig,
                                 llvm::Value* old_mm,
                                 llvm::Value* new_mm,
                                 llvm::Value* old_z,
                                 llvm::Value* new_z,
                                 uint32_t     path,
                                 int32_t      fold_vcd) {
    if (!signal_has_waiters(sig)) {
        return false;
    }
    const SignalSlot& slot = slot_of(sig);
    const EdgeFacts   f =
      emit_edge_facts(old_mm, new_mm, slot.width, slot.four_state, old_z, new_z,
                      signal_has_bucket(sig, Edge::Positive), signal_has_bucket(sig, Edge::Negative),
                      signal_has_bucket(sig, Edge::Any));
    if (trace_on()) {
        TraceSeen t = trace_seen_words(old_mm, new_mm, old_z, new_z, slot.width, slot.four_state, slot.has_z);
        t.pos       = f.pos;
        t.neg       = f.neg;
        t.any       = f.changed;
        emit_trace_change(path, sig, storage_ptr(slot), t);
    }
    // Saved, not asserted empty: a wake this one emits may service another
    // signal's queues, which must not take this store's touch.
    const std::optional<VcdFold> outer = std::exchange(vcd_fold_, std::nullopt);
    if (fold_vcd >= 0) {
        vcd_fold_ = VcdFold{ .sig = sig, .base = fold_vcd };
    }
    service_wake_buckets(sig, f);
    const bool taken = vcd_fold_.has_value() && vcd_fold_->taken;
    vcd_fold_        = outer;
    return taken;
}

// Fire each of `sig`'s per-edge buckets that exists behind one branch on its fact.
void Backend::service_wake_buckets(Signal sig, const EdgeFacts& f) {
    const auto i = static_cast<uint32_t>(sig);

    // One change is one release moment for the `<= @` records it matures, however many
    // of the buckets below it satisfies.
    emit_release_moment(sig, f);

    // Bucket -> matching fact. `pos`/`neg` each imply `changed`, so the Any
    // bucket also fires on an LSB transition.
    const std::array<std::pair<uint32_t, llvm::Value*>, 3> buckets = {
        std::make_pair(static_cast<uint32_t>(Edge::Any), f.changed),
        std::make_pair(static_cast<uint32_t>(Edge::Positive), f.pos),
        std::make_pair(static_cast<uint32_t>(Edge::Negative), f.neg),
    };
    for (const auto& [e, fact] : buckets) {
        if (fact == nullptr) {
            continue;
        }
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* body = llvm::BasicBlock::Create(ctx_, "wait.body", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "wait.cont", fn);
        b_.CreateCondBr(fact, body, cont);

        b_.SetInsertPoint(body);
        if (wait_sta_gv_[i][e] != nullptr) {
            service_static_queue(sig, e);
        }
        if (wait_dyn_gv_[i][e] != nullptr) {
            service_dynamic_queue(sig, e);
        }
        if (vif_wait_[i][e]) {
            // Waiters that reached this member through a virtual interface: an
            // intrusive list on the instance the write is landing in, walked
            // exactly like an event object's (each waiter unlinks itself).
            emit_event_wake_list(vif_head_ptr(sig, e), { .kind = RT_TRACE_VIF, .sig = sig, .fact = e });
        }
        fire_event_trigger(sig, e);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// Service a waited-on class property's change notifiers after a blocking write to
// it (LRM 9.4.2). Structurally service_wait_queue with the waiter lists reached
// through the OBJECT rather than through per-signal globals: classify the old ->
// new transition once, then for each notifier the design laid out, guard its whole
// wake walk on the matching fact.
//
// Classifying HERE, at the write, is what makes the waiter side trivial and exact.
// A fire means the value genuinely changed in the way that waiter asked for, so
// `@obj.p` wakes unconditionally on resume -- no snapshot in the waiter's frame,
// no re-comparison, and `p = p` fires nothing at all rather than waking a waiter
// that then has to discover the write was a no-op.
//
// The three notifiers are serviced independently, not else-if, for the same reason
// the three buckets are: a 0->1 transition is both `changed` and `pos`.
void Backend::service_change_notify(const LValue& lv,
                                    llvm::Value*  old_mm,
                                    llvm::Value*  new_mm,
                                    llvm::Value*  old_z,
                                    llvm::Value*  new_z) {
    const ChangeNotify& cn   = *lv.chg;
    const auto          have = [&](Edge e) { return cn.wants(e); };
    // `four_state` is "the mm word is the packed must|may pair", which is what
    // emit_edge_facts means by four_state (slot_mm_bits reads the same flag).
    const EdgeFacts                                    f         = emit_edge_facts(old_mm,
                                                                                   new_mm,
                                                                                   lv.store_width,
                                                                                   lv.four_state,
                                                                                   old_z,
                                                                                   new_z,
                                                                                   have(Edge::Positive),
                                                                                   have(Edge::Negative),
                                                                                   have(Edge::Any));
    if (trace_on()) {
        TraceSeen t = trace_seen_words(old_mm, new_mm, old_z, new_z, lv.store_width, lv.four_state, lv.has_z);
        t.pos       = f.pos;
        t.neg       = f.neg;
        t.any       = f.changed;
        emit_trace_change(RT_TRACE_CHG_PROPERTY, std::nullopt, lv_mm_ptr(lv), t);
    }
    const std::array<std::pair<Edge, llvm::Value*>, 3> notifiers = {
        std::make_pair(Edge::Any, f.changed),
        std::make_pair(Edge::Positive, f.pos),
        std::make_pair(Edge::Negative, f.neg),
    };
    for (const auto& [edge, fact] : notifiers) {
        if (fact == nullptr) {
            continue;
        }
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* body = llvm::BasicBlock::Create(ctx_, "chg.body", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "chg.cont", fn);
        b_.CreateCondBr(fact, body, cont);

        b_.SetInsertPoint(body);
        fire_prop_notifier(lv, edge);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// Start a release moment (rt_intra_moment) when a change of `sig` satisfies an edge that
// some `<= @(...)` control lists. The records its buckets mature were created together:
// they commit in the order their statements executed, and behind every record an earlier
// occurrence matured -- not in the order the Any, Positive and Negative buckets are
// scanned, which counting once here, before the first of them, keeps out of it
// (docs/design/intra-assignment-timing.md, "Order against plain NBAs"). A signal no
// `<= @` control lists emits nothing.
void Backend::emit_release_moment(Signal sig, const EdgeFacts& f) {
    if (intra_trigger_edge_.empty()) {
        return;
    }
    const auto&  listed = intra_trigger_edge_[static_cast<uint32_t>(sig)];
    llvm::Value* hit    = nullptr;
    const auto   add    = [&](Edge e, llvm::Value* fact) {
        if (fact != nullptr && listed[static_cast<uint32_t>(e)]) {
            hit = hit != nullptr ? b_.CreateOr(hit, fact) : fact;
        }
    };
    add(Edge::Any, f.changed);
    add(Edge::Positive, f.pos);
    add(Edge::Negative, f.neg);
    if (hit == nullptr) {
        return;
    }
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* body = llvm::BasicBlock::Create(ctx_, "moment.body", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "moment.cont", fn);
    b_.CreateCondBr(hit, body, cont);
    b_.SetInsertPoint(body);
    b_.CreateCall(intra_moment_fn(), {});
    b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
}

// If (sig, edge) fires a `<= @e` deferred store, count down its records and
// move any that reach zero to their target ready lists (rt_intra_event_fire).
// Called from both wake paths inside a block already guarded by the edge fact.
void Backend::fire_event_trigger(Signal sig, uint32_t edge) {
    if (trigger_id(sig, edge) < 0) {
        return;
    }
    emit_trigger_fire(trigger_id_value(sig, edge));
}

// Count down the `<= @e` records and the detached `->> @(x) e` subscriptions
// registered on `trig_id` (a dense (signal, edge) id, or an event-object id, see
// rt_intra_event_register). Shared by the two signal wake paths and by the
// event-object fire, so every way a control can occur reaches the same records.
void Backend::emit_trigger_fire(llvm::Value* trig_id) {
    b_.CreateCall(intra_event_fire_fn(), { trig_id });
    // Detached `->> @(x) e` subscriptions on the same trigger: count them
    // down and pend every event whose countdown reached zero. The wake is
    // NONBLOCKING (LRM 15.5.1: the update event triggers the referenced event in
    // the NBA region), so this pends rather than fires.
    if (!design_uses_detached_events_) {
        return;
    }
    auto* i32  = b_.getInt32Ty();
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* head = llvm::BasicBlock::Create(ctx_, "evdet.head", fn);
    auto* body = llvm::BasicBlock::Create(ctx_, "evdet.body", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "evdet.done", fn);
    llvm::Value* n     = b_.CreateCall(event_detach_fire_fn(), { trig_id });
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);
    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);
    b_.SetInsertPoint(body);
    emit_event_pend_guarded(b_.CreateCall(event_detach_ready_fn(), { idx }));
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);
    b_.SetInsertPoint(done);
}

// Body-side base pointer of a wait bucket piece: loaded from the signal's port
// where the body reaches it (the own instance's record, or the port address a
// binding holds for a signal of another instance), the bucket's own global
// otherwise. Root's code (commit functions, the seed loop) keeps the globals
// directly.
llvm::Value* Backend::wait_base(WaitPart part, Signal sig, uint32_t edge) {
    const auto        i = static_cast<uint32_t>(sig);
    const SignalSlot& s = slots_[i];
    if (port_reachable(s)) {
        const PortLayout& pl   = *port_of(s);
        const uint32_t    elem = part == WaitPart::Static    ? pl.sta[edge].base
                                 : part == WaitPart::Dynamic ? pl.dyn[edge].base
                                                             : pl.dyn[edge].len;
        return b_.CreateLoad(llvm::PointerType::getUnqual(ctx_), port_elem_ptr(s, elem));
    }
    if (s.anchor != SignalSlot::no_anchor) {
        taint("names a wait bucket of one instance");
    }
    llvm::GlobalVariable* gv = part == WaitPart::Static    ? wait_sta_gv_[i][edge]
                               : part == WaitPart::Dynamic ? wait_dyn_gv_[i][edge]
                                                           : wait_dyn_len_gv_[i][edge];
    return sym(gv);
}

// The trigger bits a wake of (sig, edge) ORs into a subscriber's wake word, as
// data: they are design-global colours (compute_trigger_masks), so no unit's
// code may hold them as an immediate. The owner's code finds them in its own
// record's port, a writer of another instance's signal in the port its binding
// names, any other writer (root's) in the owner's record global (a design
// symbol, like the bucket it scans), and a signal of no instance has a global of
// its own per bucket.
llvm::Value* Backend::static_trig_bits(Signal sig, uint32_t edge) {
    const auto        i = static_cast<uint32_t>(sig);
    const SignalSlot& s = slots_[i];
    if (s.anchor != SignalSlot::no_anchor && wait_sta_gv_[i][edge] != nullptr) {
        return b_.CreateLoad(b_.getInt64Ty(), port_elem_ptr(s, port_of(s)->sta[edge].trig));
    }
    if (wait_sta_trig_gv_[i][edge] != nullptr) {
        return b_.CreateLoad(b_.getInt64Ty(), sym(wait_sta_trig_gv_[i][edge]));
    }
    // The colours of a signal of some instance are that instance's own.
    if (s.anchor != SignalSlot::no_anchor) {
        taint("names the trigger bits of a signal of one instance");
    }
    return b_.getInt64(trig_bits(sig, edge));
}

// Static sub-queue scan (one edge bucket): a loop over the persistent
// subscriptions for this (signal, edge). The bucket's edge already matched the
// transition (the caller guarded the whole scan), so there is no per-entry
// edge check. Membership never changes — no compaction, no length.
//
// The loop is a function of its own per module and shape, and a store site
// holds one call to it: the scan is the bulk of a wake, and inlined at every
// store it was most of a design's code (static_scan_fn).
//
// A body that reaches the signal's port (the owner's own code, or a writer of
// another instance's signal through its binding) passes the port's {base, trig,
// cap} triple, the exact count of that instance, so the scan needs no padding,
// and the store's dump-var touch rides along when service_wait_queue offers it
// (vcd_fold_). Any other writer passes the bucket, the per-definition maximum
// (null-checked when twins differ) and the trigger bits (static_trig_bits).
void Backend::service_static_queue(Signal sig, uint32_t edge) {
    const auto                       i = static_cast<uint32_t>(sig);
    const SignalSlot&                s = slots_[i];
    llvm::SmallVector<llvm::Value*, 4> args;
    llvm::Function*                  scan = nullptr;
    if (port_reachable(s)) {
        const PortLayout::Static& pl = port_of(s)->sta[edge];
        assert(pl.trig == pl.base + 1 && pl.cap == pl.base + 2 && "the port holds a bucket's triple in order");
        args.push_back(port_elem_ptr(s, pl.base));
        const bool vcd = vcd_fold_.has_value() && vcd_fold_->sig == sig && edge == static_cast<uint32_t>(Edge::Any);
        if (vcd) {
            args.push_back(vcd_index_value(sig, vcd_fold_->base));
            vcd_fold_->taken = true;
        }
        scan = static_scan_port_fn(vcd, edge);
    } else {
        llvm::Value* const trig      = static_trig_bits(sig, edge);
        auto* const        none      = llvm::dyn_cast<llvm::ConstantInt>(trig);
        const bool         trig_zero = none != nullptr && none->isZero();
        args.push_back(wait_base(WaitPart::Static, sig, edge));
        args.push_back(b_.getInt32(wait_sta_cap_[i][edge]));
        if (!trig_zero) {
            args.push_back(trig);
        }
        scan = static_scan_fn(wait_sta_padded_[i][edge] != 0, trig_zero, edge);
    }
    if (trace_on()) {
        // The storage the writer reaches, as the waiter's list names it.
        args.push_back(storage_ptr(slot_of(sig)));
    }
    b_.CreateCall(scan, args);
}

// Dynamic sub-queue scan (one edge bucket): a straight loop over the live
// one-shot waiters for this (signal, edge), via the generic runtime scan
// with a constant non-null base. The whole bucket is consumed (length
// resets to 0); the firing process deregisters its stale entries from any
// OTHER buckets itself on resume (eager removal in lower_wait).
void Backend::service_dynamic_queue(Signal sig, uint32_t edge) {
    emit_dynamic_scan_rt(wait_base(WaitPart::Dynamic, sig, edge), wait_base(WaitPart::DynamicLen, sig, edge),
                         { .kind = RT_TRACE_DYNAMIC, .sig = sig, .fact = edge });
}

// The design's one detached on-expiry thunk of `->> #d e`. The wheel calls it as
// on_expire(rt_timer* t, void* ctx) (ctx, the wheel, is unused here) on the node
// rt_event_timer_new made, which carries the event's handle -- so ONE thunk
// serves every delayed trigger in the design, whose target can be any event
// expression. It takes the handle back, which frees the node, and pends that
// event for the NBA region.
//
// LRM 15.5.1: the update event of `->> #d e` "shall trigger the referenced event
// in the nonblocking assignment region" of the instant the delay expires. The
// wheel fires this at the top of that instant, ahead of its Active region, so
// firing the event here would wake the waiter in the Active region; pending it
// leaves the wake to the NBA drain, exactly as for `->> e`.
//
// Defined in root and reached from every unit through sym(), like
// enqueue_trampoline: returning the bare definition to a second module is a
// cross-module global reference, which LLVM rejects.
llvm::Function* Backend::event_delay_thunk() {
    if (event_delay_thunk_ == nullptr) {
        InRoot in_root(*this);
        build_event_delay_thunk();
    }
    return sym(event_delay_thunk_);
}

void Backend::build_event_delay_thunk() {
    auto*             ptr  = raw_ptr_ty();
    auto*             fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false);
    llvm::IRBuilder<> save(ctx_);
    auto*             saved_bb = b_.GetInsertBlock();
    auto              saved_pt = b_.GetInsertPoint();
    event_delay_thunk_ =
      llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.ev_delay", env_->llmod);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", event_delay_thunk_));
    emit_event_pend_guarded(b_.CreateCall(event_timer_take_fn(), { event_delay_thunk_->getArg(0) }));
    b_.CreateRetVoid();
    if (saved_bb != nullptr) {
        b_.SetInsertPoint(saved_bb, saved_pt);
    }
}

// Link an event object onto the NBA pending chain (LRM 15.5.1), unless it is
// already on it. The chain replaces the old compile-time list of `->>` target
// signals, which a handle-reached event could not join; it also means the NBA
// region visits exactly the events that fired rather than every `->>` target in
// the design.
//
// `next_pend` doubles as the on-chain marker: a linked object stores the next
// object pointer (or the chain terminator, itself, for the last one), and an
// unlinked object stores null. `pending` stays as the boolean the "anything
// staged" test reads.
void Backend::emit_event_pend_guarded(llvm::Value* handle) {
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* pend = llvm::BasicBlock::Create(ctx_, "evpend.live", fn);
    auto* skip = llvm::BasicBlock::Create(ctx_, "evpend.null", fn);
    b_.CreateCondBr(b_.CreateICmpNE(handle, b_.getInt64(0)), pend, skip);
    b_.SetInsertPoint(pend);
    emit_event_pend(event_obj_live(handle));
    b_.CreateBr(skip);
    b_.SetInsertPoint(skip);
}

void Backend::emit_event_pend(llvm::Value* obj) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* link = llvm::BasicBlock::Create(ctx_, "evpend.link", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "evpend.done", fn);

    llvm::Value* pendp = event_field(obj, kEventPending);
    b_.CreateCondBr(b_.CreateICmpEQ(b_.CreateLoad(b_.getInt8Ty(), pendp), b_.getInt8(0)), link, done);

    b_.SetInsertPoint(link);
    b_.CreateStore(b_.getInt8(1), pendp);
    llvm::Value* headp = sym(event_pend_head_gv_);
    b_.CreateStore(b_.CreateLoad(ptr, headp), event_field(obj, kEventNextPend));
    b_.CreateStore(obj, headp);
    emit_trace_pend(obj);
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
    (void)null;
}

// Drain the NBA pending chain: fire every `->> e` staged this instant, clearing
// each object's pending mark as it goes. `->> e` creates a nonblocking assign update
// event that triggers the event in the NBA region (LRM 15.5.1), and NBAs are performed in
// the order their statements executed (4.6 b)), so the events trigger in that order: the
// records a `<= @e` control holds commit in the order their events occurred
// (docs/LRM_NOTES.md, note 51). The chain links the newest event first, so it is reversed
// before the walk. Reads `next` before firing, because a waiter woken by the fire may
// itself `->>` another event -- that link goes onto the (now empty) head and is drained
// by the NBA region's next pass, which is the deferral LRM 15.5.1 asks for.
void Backend::emit_event_pend_drain(llvm::Value* wheel) {
    auto* ptr   = llvm::PointerType::getUnqual(ctx_);
    auto* null  = llvm::ConstantPointerNull::get(ptr);
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* flip  = llvm::BasicBlock::Create(ctx_, "evpend.flip", fn);
    auto* relnk = llvm::BasicBlock::Create(ctx_, "evpend.relink", fn);
    auto* head  = llvm::BasicBlock::Create(ctx_, "evpend.walk", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "evpend.fire", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "evpend.drained", fn);

    llvm::Value* headp  = sym(event_pend_head_gv_);
    llvm::Value* newest = b_.CreateLoad(ptr, headp);
    b_.CreateStore(null, headp);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(flip);

    // Reverse the chain in place: `cur` walks it newest first, `oldest_first` is the
    // part already relinked.
    b_.SetInsertPoint(flip);
    llvm::PHINode* cur          = b_.CreatePHI(ptr, 2);
    llvm::PHINode* oldest_first = b_.CreatePHI(ptr, 2);
    cur->addIncoming(newest, entry);
    oldest_first->addIncoming(null, entry);
    b_.CreateCondBr(b_.CreateICmpNE(cur, null), relnk, head);

    b_.SetInsertPoint(relnk);
    llvm::Value* older = b_.CreateLoad(ptr, event_field(cur, kEventNextPend));
    b_.CreateStore(oldest_first, event_field(cur, kEventNextPend));
    cur->addIncoming(older, relnk);
    oldest_first->addIncoming(cur, relnk);
    b_.CreateBr(flip);

    b_.SetInsertPoint(head);
    llvm::PHINode* n = b_.CreatePHI(ptr, 2);
    n->addIncoming(oldest_first, flip);
    b_.CreateCondBr(b_.CreateICmpNE(n, null), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* next = b_.CreateLoad(ptr, event_field(n, kEventNextPend));
    b_.CreateStore(null, event_field(n, kEventNextPend));
    b_.CreateStore(b_.getInt8(0), event_field(n, kEventPending));
    emit_event_fire(n, wheel, RT_TRACE_TRIGGER_NB);
    n->addIncoming(next, b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
}

// Stamp an event object's trigger time (from `wheel`) and wake every waiter on its
// list. Shared by `-> e` in a process body (wheel_) and the NBA-region `->> e`
// service (the drain's local wheel — wheel_ is stale there, so it must be passed
// in).
void Backend::emit_event_fire(llvm::Value* obj, llvm::Value* wheel, uint32_t trigger) {
    emit_trace_trigger(obj, trigger);
    b_.CreateStore(b_.CreateCall(wheel_now_fn(), { wheel }), event_field(obj, kEventTrig));
    vcd_touch_event_obj(obj);
    emit_event_wake_list(obj, { .kind = RT_TRACE_EVENT, .obj = obj });
    // Some `@` or `wait` calls a method that reads an event property (LRM 9.4.2).
    // The method can reach any event of the design through any object, so each
    // trigger wakes those waiters to re-evaluate. The frontend hangs their notifier
    // on the event type (Lowerer::declare_shared_notifiers); no other design has one.
    if (const ChangeNotify* cn = mod_.change_notify(mod_.pool.event_type, 0);
        cn != nullptr && cn->shared != kNoNotify) {
        llvm::Value* any = event_obj_live(b_.CreateLoad(b_.getInt64Ty(), chg_shared_slot(cn->shared)));
        emit_event_wake_list(any, { .kind = RT_TRACE_EVENT, .obj = any });
    }
    // An event is also a control for `x <= @e v` / `->> @(e) f`, which wait on the
    // object rather than on a signal edge. Every trigger form converges here, and
    // only an object some such statement registered on has an id to fire.
    if (!design_uses_event_triggers_) {
        return;
    }
    auto*        fn   = b_.GetInsertBlock()->getParent();
    auto*        ctrl = llvm::BasicBlock::Create(ctx_, "ev.control", fn);
    auto*        done = llvm::BasicBlock::Create(ctx_, "ev.fired", fn);
    llvm::Value* id   = b_.CreateLoad(b_.getInt32Ty(), event_field(obj, kEventTrigId));
    b_.CreateCondBr(b_.CreateICmpNE(id, b_.getInt32(0)), ctrl, done);
    b_.SetInsertPoint(ctrl);
    // One occurrence of the event is one release moment for the records it matures.
    if (design_uses_intra_event_objs_) {
        b_.CreateCall(intra_moment_fn(), {});
    }
    emit_trigger_fire(b_.CreateOr(id, b_.getInt32(RT_TRIG_EVENT)));
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
}

// `-> null` / `->> null` is a no-op (LRM 15.5.5): a null handle names nothing to
// trigger. Skipping the whole fire -- rather than letting it land on the null
// object -- is what keeps `null.triggered` reading 0 afterwards, since a stamp
// there would otherwise be observable through any null handle.
void Backend::emit_event_fire_guarded(llvm::Value* handle, llvm::Value* wheel, uint32_t trigger) {
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* fire = llvm::BasicBlock::Create(ctx_, "ev.fire", fn);
    auto* skip = llvm::BasicBlock::Create(ctx_, "ev.null", fn);
    b_.CreateCondBr(b_.CreateICmpNE(handle, b_.getInt64(0)), fire, skip);
    b_.SetInsertPoint(fire);
    emit_event_fire(event_obj_live(handle), wheel, trigger);
    b_.CreateBr(skip);
    b_.SetInsertPoint(skip);
}

// Wake every waiter on an event object's list.
//
// A trigger has no value transition, so — unlike a signal store — there are no
// edge facts to classify and no per-edge split: every waiter on the list is
// woken unconditionally, which is what `@(e)` / `always @(e)` mean.
//
// `next` is read BEFORE the wake, because the woken activation unlinks its own
// node as soon as it runs (it cannot run before we suspend, but keeping the
// load first makes that independent of scheduling — the same discipline
// lower_disable_list uses on its activation list).
//
// The scheduled flag is the design's existing one-wake-per-process token: skip
// an entry whose flag is already set, else push and set it. That collapses a
// multi-event `@(a or b)` wait, and a process subscribed to one event several
// ways, into a single wake.
//
// One-shot nodes are NOT bulk-cleared the way a dynamic bucket is (which resets
// its whole length on fire): each waiter unlinks itself on resume, which is also
// what makes a multi-event wait correct without a compaction pass.
void Backend::emit_event_wake_list(llvm::Value* obj, const TraceCause& why) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = event_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    auto* head = llvm::BasicBlock::Create(ctx_, "ev.walk", fn);
    auto* body = llvm::BasicBlock::Create(ctx_, "ev.node", fn);
    auto* wake = llvm::BasicBlock::Create(ctx_, "ev.wake", fn);
    auto* step = llvm::BasicBlock::Create(ctx_, "ev.next", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "ev.done", fn);

    llvm::Value* first = b_.CreateLoad(ptr, event_field(obj, kEventWaiters));
    auto*        entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    llvm::PHINode* n = b_.CreatePHI(ptr, 2);
    n->addIncoming(first, entry);
    b_.CreateCondBr(b_.CreateICmpNE(n, null), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* next = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, n, kNodeNext));
    llvm::Value* hdl  = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, n, kNodeHandle));
    llvm::Value* flag = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, n, kNodeFlag));
    b_.CreateCondBr(b_.CreateICmpEQ(b_.CreateLoad(sched_word_ty(), flag), b_.getInt64(0)), wake, step);

    b_.SetInsertPoint(wake);
    emit_wake_push(b_, hdl, why);
    b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), flag);
    b_.CreateBr(step);

    b_.SetInsertPoint(step);
    b_.CreateBr(head);
    n->addIncoming(next, step);

    b_.SetInsertPoint(done);
}

// Link `node` onto the front of `obj`'s waiter list, recording the parked
// handle and scheduled-flag pointer. Push-front keeps this O(1) with no tail
// pointer; wake order within one instant is unspecified by the LRM (LRM 4.7
// leaves the order of processes woken by one event arbitrary).
// The node records the object it is linked onto unless `record_owner` is false,
// which a persistent subscription passes: it is never unlinked. For a parked
// waiter that word is also what keeps the object alive, through the scan of its
// frame, when no variable names the object any longer.
void Backend::emit_event_link(llvm::Value* obj,
                              llvm::Value* node,
                              llvm::Value* handle,
                              llvm::Value* flag,
                              bool         record_owner) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = event_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    b_.CreateStore(handle, b_.CreateStructGEP(nty, node, kNodeHandle));
    b_.CreateStore(flag, b_.CreateStructGEP(nty, node, kNodeFlag));
    if (record_owner) {
        b_.CreateStore(obj, b_.CreateStructGEP(nty, node, kNodeOwner));
    }
    llvm::Value* headp = event_field(obj, kEventWaiters);
    llvm::Value* old   = b_.CreateLoad(ptr, headp);
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kNodePrev));
    b_.CreateStore(old, b_.CreateStructGEP(nty, node, kNodeNext));

    auto* fix  = llvm::BasicBlock::Create(ctx_, "ev.link.fix", fn);
    auto* tail = llvm::BasicBlock::Create(ctx_, "ev.link.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(old, null), fix, tail);
    b_.SetInsertPoint(fix);
    b_.CreateStore(node, b_.CreateStructGEP(nty, old, kNodePrev));
    b_.CreateBr(tail);
    b_.SetInsertPoint(tail);
    b_.CreateStore(node, headp);
}

// Unlink `node` from the waiter list it is on. Idempotent for an already-unlinked
// node only if it was cleared on unlink, which this does: both links are nulled
// so a second unlink is a no-op head-relink of a node that is no longer the
// head. Callers unlink exactly once, on resume.
// The object is read back from the node: the variable or expression that named it
// may name another by now, and the node must come off the object it went onto.
void Backend::emit_event_unlink(llvm::Value* node) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = event_node_ty();
    llvm::Value* obj = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kNodeOwner));
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* prev = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kNodePrev));
    llvm::Value* next = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kNodeNext));

    auto* has_prev = llvm::BasicBlock::Create(ctx_, "ev.unlink.prev", fn);
    auto* no_prev  = llvm::BasicBlock::Create(ctx_, "ev.unlink.head", fn);
    auto* mid      = llvm::BasicBlock::Create(ctx_, "ev.unlink.mid", fn);
    auto* has_next = llvm::BasicBlock::Create(ctx_, "ev.unlink.next", fn);
    auto* tail     = llvm::BasicBlock::Create(ctx_, "ev.unlink.done", fn);

    b_.CreateCondBr(b_.CreateICmpNE(prev, null), has_prev, no_prev);
    b_.SetInsertPoint(has_prev);
    b_.CreateStore(next, b_.CreateStructGEP(nty, prev, kNodeNext));
    b_.CreateBr(mid);
    b_.SetInsertPoint(no_prev);
    b_.CreateStore(next, event_field(obj, kEventWaiters));
    b_.CreateBr(mid);

    b_.SetInsertPoint(mid);
    b_.CreateCondBr(b_.CreateICmpNE(next, null), has_next, tail);
    b_.SetInsertPoint(has_next);
    b_.CreateStore(prev, b_.CreateStructGEP(nty, next, kNodePrev));
    b_.CreateBr(tail);

    b_.SetInsertPoint(tail);
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kNodeNext));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kNodePrev));
}

// Lower `e.triggered` (LRM 15.5.3) to a definite 2-state 1-bit: 1 iff the last
// trigger tick equals the current tick (true across all delta cycles of the
// step, false once time advances — no per-step reset needed).
Plane Backend::lower_event_triggered(llvm::Value* obj) {
    llvm::Value* stamp = b_.CreateLoad(b_.getInt64Ty(), event_field(obj, kEventTrig));
    llvm::Value* now   = b_.CreateCall(wheel_now_fn(), { wheel_ });
    return two_val(b_.CreateICmpEQ(stamp, now));
}

// Compacting removal of this coroutine's entries from one (signal, edge)
// dynamic bucket, used on resume from a multi-signal wait: the firing bucket
// already cleared us, but the others still hold our now-stale entry. Slides
// every entry that is not ours down to fill the gaps and trims the length:
//   w = 0; for i in 0..len: if Q[i].handle != mine { Q[w++] = Q[i] } ; len = w
// `mine` is the PARKED form of this handle -- what emit_dynamic_register
// wrote -- so the comparison matches for reactive code too.
void Backend::emit_dynamic_remove(Signal sig, uint32_t edge) {
    llvm::Value*      q      = wait_base(WaitPart::Dynamic, sig, edge);
    llvm::Value*      len_gv = wait_base(WaitPart::DynamicLen, sig, edge);
    llvm::StructType* elem   = wait_elem_ty();
    auto*             ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto*             i32    = b_.getInt32Ty();

    llvm::Value* n     = b_.CreateLoad(i32, len_gv);
    auto*        fn    = b_.GetInsertBlock()->getParent();
    auto*        head  = llvm::BasicBlock::Create(ctx_, "rm.head", fn);
    auto*        body  = llvm::BasicBlock::Create(ctx_, "rm.body", fn);
    auto*        keep  = llvm::BasicBlock::Create(ctx_, "rm.keep", fn);
    auto*        cont  = llvm::BasicBlock::Create(ctx_, "rm.cont", fn);
    auto*        done  = llvm::BasicBlock::Create(ctx_, "rm.done", fn);
    auto*        entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    auto* w   = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    w->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* src  = b_.CreateInBoundsGEP(elem, q, idx);
    llvm::Value* h    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 0));
    llvm::Value* f    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 1));
    llvm::Value* mine = b_.CreateICmpEQ(h, parked_handle(b_, hdl_));
    b_.CreateCondBr(mine, cont, keep); // ours -> drop; else -> keep

    b_.SetInsertPoint(keep);
    llvm::Value* dst = b_.CreateInBoundsGEP(elem, q, w);
    b_.CreateStore(h, b_.CreateStructGEP(elem, dst, 0));
    b_.CreateStore(f, b_.CreateStructGEP(elem, dst, 1));
    llvm::Value* w_keep = b_.CreateAdd(w, b_.getInt32(1));
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    auto* w_next = b_.CreatePHI(i32, 2);
    w_next->addIncoming(w, body);
    w_next->addIncoming(w_keep, keep);
    llvm::Value* idx_next = b_.CreateAdd(idx, b_.getInt32(1));
    b_.CreateBr(head);
    idx->addIncoming(idx_next, cont);
    w->addIncoming(w_next, cont);

    b_.SetInsertPoint(done);
    b_.CreateStore(w, len_gv);
}

// -- Wait-queue scans --

// Generic static-queue scan (base + capacity values), null base => no
// waiters. Serves service_static_queue, which passes the bucket's base and
// capacity (a constant global, or fields of the owner's record). Each
// entry wakes its process only if its scheduled flag is 0, then sets the
// flag — collapsing a multi-signal sensitivity (the same process
// subscribed under several signals) into a single wake.
// `padded`: the bucket is capacity-padded to its definition's maximum (see
// build_wait_queues), so trailing slots are zeroinit — skip a null flag
// pointer instead of dereferencing it. Exact buckets keep the check-free
// scan.
// `trig`: trigger bits this (signal, edge) contributes to the wake word
// (compute_trigger_masks): a constant, or a value read from the owner's
// instance record. Note the already-queued path: it must still OR the bits
// in, or a group woken by two of its events in one instant loses the second
// one's bit and skips the members that were waiting for it. With a constant 0
// that path is empty and the emission is exactly the unguarded scan.
void Backend::emit_static_scan_rt(llvm::Value* base, llvm::Value* cap, bool padded, llvm::Value* trig, const TraceCause& why) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* word = sched_word_ty();
    auto* elem = wait_elem_ty();
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* body = llvm::BasicBlock::Create(ctx_, "nsta.body", fn);
    auto* wake = llvm::BasicBlock::Create(ctx_, "nsta.wake", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "nsta.cont", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "nsta.done", fn);
    auto* entry = b_.GetInsertBlock();
    // A bucket nobody subscribed to has no entries, whatever its base is. A loop
    // that tests at its bottom and carries one pointer and one count is what
    // tier-0 code generation (which keeps every value that outlives a block in
    // memory) makes the least of: this is the shape of every wake a store makes.
    b_.CreateCondBr(b_.CreateICmpNE(cap, b_.getInt32(0)), body, done);

    b_.SetInsertPoint(body);
    auto* cur  = b_.CreatePHI(ptr, 2);
    auto* left = b_.CreatePHI(i32, 2);
    cur->addIncoming(base, entry);
    left->addIncoming(cap, entry);
    llvm::Value* h    = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, cur, 0));
    llvm::Value* fptr = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, cur, 1));
    if (padded) {
        auto* live = llvm::BasicBlock::Create(ctx_, "nsta.live", fn);
        b_.CreateCondBr(b_.CreateICmpNE(fptr, llvm::ConstantPointerNull::get(ptr)), live, cont);
        b_.SetInsertPoint(live);
    }
    llvm::Value* old    = b_.CreateLoad(word, fptr);
    auto*        queued = b_.CreateICmpNE(old, b_.getInt64(0));
    auto*        none   = llvm::dyn_cast<llvm::ConstantInt>(trig);
    if (none != nullptr && none->isZero()) {
        b_.CreateCondBr(queued, cont, wake);
        b_.SetInsertPoint(wake);
        emit_wake_push(b_, h, why);
        b_.CreateStore(b_.getInt64(1), fptr);
    } else {
        // The word becomes the entry's bits OR-ed in if it was already queued, else
        // queued with them: one store, so only the push is a block of its own.
        b_.CreateStore(b_.CreateSelect(queued, b_.CreateOr(old, trig), b_.CreateOr(trig, b_.getInt64(1))), fptr);
        b_.CreateCondBr(queued, cont, wake);
        b_.SetInsertPoint(wake);
        emit_wake_push(b_, h, why);
    }
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    cur->addIncoming(b_.CreateInBoundsGEP(elem, cur, b_.getInt32(1)), cont);
    llvm::Value* next = b_.CreateSub(left, b_.getInt32(1));
    left->addIncoming(next, cont);
    b_.CreateCondBr(b_.CreateICmpNE(next, b_.getInt32(0)), body, done);

    b_.SetInsertPoint(done);
}

namespace {

// An internal function of `m`, never inlined: the point is that store sites
// share it.
llvm::Function* shared_fn(llvm::Module& m, llvm::FunctionType* ty, const std::string& name) {
    auto* const f = llvm::Function::Create(ty, llvm::Function::InternalLinkage, name, m);
    f->addFnAttr(llvm::Attribute::NoInline);
    f->addFnAttr(llvm::Attribute::NoUnwind);
    return f;
}

} // namespace

// (base ptr, cap i32[, trig i64][, traced storage ptr]). The cap is the caller's
// constant bucket size, an argument because the function serves every bucket.
llvm::Function* Backend::static_scan_fn(bool padded, bool trig_zero, uint32_t edge) {
    const bool          traced = trace_on();
    llvm::Module* const m      = &env_->llmod;
    const auto          key    = std::make_tuple(m, padded, trig_zero, traced ? edge : 0u);
    if (const auto it = static_scan_fns_.find(key); it != static_scan_fns_.end()) {
        return it->second;
    }
    auto* const              ptr = llvm::PointerType::getUnqual(ctx_);
    std::vector<llvm::Type*> params{ ptr, b_.getInt32Ty() };
    if (!trig_zero) {
        params.push_back(b_.getInt64Ty());
    }
    if (traced) {
        params.push_back(ptr);
    }
    std::string name = std::string("clotho.notify.sta") + (padded ? ".pad" : "") + (trig_zero ? "" : ".trig");
    if (traced) {
        name += ".e" + std::to_string(edge);
    }
    llvm::Function* const f = shared_fn(*m, llvm::FunctionType::get(b_.getVoidTy(), params, false), name);
    static_scan_fns_.emplace(key, f);

    const llvm::IRBuilderBase::InsertPointGuard guard(b_);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", f));
    llvm::Value* const trig = trig_zero ? static_cast<llvm::Value*>(b_.getInt64(0)) : f->getArg(2);
    TraceCause         why{ .kind = RT_TRACE_STATIC, .fact = edge };
    if (traced) {
        why.obj = f->getArg(f->arg_size() - 1);
    }
    emit_static_scan_rt(f->getArg(0), f->getArg(1), padded, trig, why);
    b_.CreateRetVoid();
    return f;
}

// (port triple ptr[, dump-var index i32][, traced storage ptr]). The triple is
// {ptr base, i64 trig, i32 cap}, laid out in that order by build_ports. The
// caller reaches the function only when the bucket's fact holds, so for the Any
// bucket only when the value changed, which is when a dump-var touch matters.
llvm::Function* Backend::static_scan_port_fn(bool vcd, uint32_t edge) {
    const bool          traced = trace_on();
    llvm::Module* const m      = &env_->llmod;
    const auto          key    = std::make_tuple(m, vcd, traced ? edge : 0u);
    if (const auto it = static_scan_port_fns_.find(key); it != static_scan_port_fns_.end()) {
        return it->second;
    }
    auto* const              ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const              i64 = b_.getInt64Ty();
    auto* const              i32 = b_.getInt32Ty();
    std::vector<llvm::Type*> params{ ptr };
    if (vcd) {
        params.push_back(i32);
    }
    if (traced) {
        params.push_back(ptr);
    }
    std::string name = vcd ? "clotho.notify.port.vcd" : "clotho.notify.port";
    if (traced) {
        name += ".e" + std::to_string(edge);
    }
    llvm::Function* const f = shared_fn(*m, llvm::FunctionType::get(b_.getVoidTy(), params, false), name);
    static_scan_port_fns_.emplace(key, f);

    const llvm::IRBuilderBase::InsertPointGuard guard(b_);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", f));
    auto* const        triple = llvm::StructType::get(ctx_, { ptr, i64, i32 });
    llvm::Value* const desc   = f->getArg(0);
    llvm::Value* const base   = b_.CreateLoad(ptr, b_.CreateStructGEP(triple, desc, 0));
    llvm::Value* const trig   = b_.CreateLoad(i64, b_.CreateStructGEP(triple, desc, 1));
    llvm::Value* const cap    = b_.CreateLoad(i32, b_.CreateStructGEP(triple, desc, 2));
    TraceCause         why{ .kind = RT_TRACE_STATIC, .fact = edge };
    if (traced) {
        why.obj = f->getArg(f->arg_size() - 1);
    }
    emit_static_scan_rt(base, cap, false, trig, why);
    if (vcd) {
        llvm::Value* const idx = f->getArg(1);
        emit_guarded_vcd_touch(vcd_touch_fn(), [&] { return llvm::SmallVector<llvm::Value*, 2>{ idx }; });
    }
    b_.CreateRetVoid();
    return f;
}

// Generic dynamic-queue scan (base + length pointer values), null base =>
// skip. Serves both callers, like emit_static_scan_rt. Wakes every live
// entry (flag-deduped) and resets the length to 0.
void Backend::emit_dynamic_scan_rt(llvm::Value* base, llvm::Value* lenptr, const TraceCause& why) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* elem = wait_elem_ty();
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* chk   = llvm::BasicBlock::Create(ctx_, "ndyn.chk", fn);
    auto* head  = llvm::BasicBlock::Create(ctx_, "ndyn.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "ndyn.body", fn);
    auto* wake  = llvm::BasicBlock::Create(ctx_, "ndyn.wake", fn);
    auto* cont  = llvm::BasicBlock::Create(ctx_, "ndyn.cont", fn);
    auto* reset = llvm::BasicBlock::Create(ctx_, "ndyn.reset", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "ndyn.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(base, llvm::ConstantPointerNull::get(ptr)), chk, done);

    b_.SetInsertPoint(chk);
    llvm::Value* n = b_.CreateLoad(i32, lenptr);
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), chk);
    b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, reset);

    b_.SetInsertPoint(body);
    llvm::Value* src  = b_.CreateInBoundsGEP(elem, base, idx);
    llvm::Value* h    = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 0));
    llvm::Value* fptr = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 1));
    // No trigger bits: a dynamic entry is an intra-body park (`@(...)`, a
    // task wait), and a guarded group is suspension-free by construction, so
    // no entry here can name one.
    b_.CreateCondBr(b_.CreateICmpNE(b_.CreateLoad(sched_word_ty(), fptr), b_.getInt64(0)), cont, wake);

    b_.SetInsertPoint(wake);
    emit_wake_push(b_, h, why);
    b_.CreateStore(b_.getInt64(1), fptr);
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), cont);
    b_.CreateBr(head);

    b_.SetInsertPoint(reset);
    b_.CreateStore(b_.getInt32(0), lenptr);
    b_.CreateBr(done);

    b_.SetInsertPoint(done);
}

// Wake the Any-edge waiters of an array signal unconditionally (an element
// write has no edge semantics; readers re-evaluate). Used after the deferred
// queue commits. Arrays only ever carry Any-edge (level) sensitivity — slang
// rejects edge events on memory elements (@(posedge mem[i])) — so the Any
// bucket is the only one a memory can have.
void Backend::service_any_waiters(Signal sig) {
    const auto i = static_cast<uint32_t>(sig);
    const auto e = static_cast<uint32_t>(Edge::Any);
    if (trace_on()) {
        TraceSeen t;
        t.level = true;
        emit_trace_change(RT_TRACE_CHG_LEVEL, sig, storage_ptr(slot_of(sig)), t);
    }
    if (wait_sta_gv_[i][e] != nullptr) {
        service_static_queue(sig, e);
    }
    if (wait_dyn_gv_[i][e] != nullptr) {
        service_dynamic_queue(sig, e);
    }
}

// The frame-resident waiter node for the `k`th event of an intra-body `@(...)`
// in the body being lowered. One per index per activation, allocated next to
// coro_begin_ so it lives in the coroutine frame and survives the suspend it is
// linked across (an ordinary loop-body alloca would grow the stack per pass and
// would not be frame-resident).
//
// Sharing by index across wait sites is safe: a coroutine is suspended at one
// wait at a time, and every node it linked is unlinked before it reaches the
// next one -- so at most one wait site's nodes are live per activation.
// The cache is keyed by index only, so it MUST be per body. Rather than trust
// every body-start site to clear it, check ownership: an entry left over from a
// previously lowered body belongs to another function, and using it would emit
// "Referring to an instruction in another function".
llvm::Value* Backend::event_wait_node(uint32_t k) {
    auto* owner      = coro_begin_->getFunction();
    auto [it, fresh] = event_wait_nodes_.try_emplace(k, nullptr);
    if (fresh || llvm::cast<llvm::Instruction>(it->second)->getFunction() != owner) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        it->second = ab.CreateAlloca(event_node_ty(), nullptr, "event_wait_node");
    }
    return it->second;
}

// The wake of a `ref` actual that has waiters, as a function the callee calls after each write
// through the formal (LRM 9.4.2: every change wakes at the change; 13.5.2: the formal is the
// actual). The callee holds only a pointer, while the buckets are static per signal or reached
// through a class object, so the call site makes `void(ptr ctx, i1 changed, i1 pos, i1 neg)`:
// the callee classifies its own write (it knows the leaf it wrote) and the thunk services what
// the actual's owner has waiting. `ctx` is the record of the code that made the call, the
// owner's for a virtual interface member, or the object body of a class property, so the thunk
// addresses the buckets as that code does. Built like a block commit function: in the module of
// the caller, with its lowering state put back after.
llvm::Function* Backend::emit_ref_wake_thunk(const LValueInfo& lv) {
    auto*                          ptr = llvm::PointerType::getUnqual(ctx_);
    auto*                          i1  = b_.getInt1Ty();
    const std::vector<llvm::Type*> pt{ ptr, i1, i1, i1 };
    const std::string name = fn_->getName().str() + ".refwake" + std::to_string(ref_wake_seq_[fn_]++);
    auto*             th   = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), pt, false),
                                                    llvm::Function::InternalLinkage, name, env_->llmod);

    llvm::IRBuilderBase::InsertPoint saved_ip     = b_.saveIP();
    llvm::Function* const            saved_fn     = fn_;
    llvm::Value* const               saved_state  = state_;
    const uint32_t                   saved_anchor = own_anchor_;
    llvm::Value* const               saved_ctx    = own_ctx_;
    llvm::Value* const               saved_ovr    = ctx_override_;
    const uint32_t                   saved_ovr_a  = ctx_override_anchor_;
    const bool                       saved_silent = silent_stores_;
    fn_                  = th;
    state_               = sym(state_global_);
    ctx_override_        = nullptr;
    ctx_override_anchor_ = SignalSlot::no_anchor;
    silent_stores_       = false;
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", th));
    llvm::Value* ctx_arg = th->getArg(0);
    if (lv.chg != nullptr || lv.vif_ctx != nullptr || saved_ctx == nullptr) {
        own_anchor_ = SignalSlot::no_anchor;
        own_ctx_    = nullptr;
    } else {
        own_ctx_ = ctx_arg;
    }
    {
        CtxOverride ov(*this, lv.vif_ctx != nullptr ? ctx_arg : nullptr, lvalue_ctx_anchor(lv));
        llvm::Value* const changed = th->getArg(1);
        llvm::Value* const pos     = th->getArg(2);
        llvm::Value* const neg     = th->getArg(3);
        if (lv.chg != nullptr) {
            // A class property: the notifiers live in the object body, which `ctx` is.
            LValueInfo li;
            li.chg         = lv.chg;
            li.chg_body    = ctx_arg;
            li.chg_body_ty = lv.chg_body_ty;
            const std::array<std::pair<Edge, llvm::Value*>, 3> notifiers = {
                std::make_pair(Edge::Any, changed),
                std::make_pair(Edge::Positive, pos),
                std::make_pair(Edge::Negative, neg),
            };
            for (const auto& [edge, fact] : notifiers) {
                if (!lv.chg->wants(edge)) {
                    continue;
                }
                auto* body = llvm::BasicBlock::Create(ctx_, "chg.body", th);
                auto* cont = llvm::BasicBlock::Create(ctx_, "chg.cont", th);
                b_.CreateCondBr(fact, body, cont);
                b_.SetInsertPoint(body);
                fire_prop_notifier(li, edge);
                b_.CreateBr(cont);
                b_.SetInsertPoint(cont);
            }
        } else {
            const Signal sig = lv.is_signal ? lv.sig : lv.mut_sig;
            EdgeFacts    f;
            f.changed = signal_has_bucket(sig, Edge::Any) ? changed : nullptr;
            f.pos     = signal_has_bucket(sig, Edge::Positive) ? pos : nullptr;
            f.neg     = signal_has_bucket(sig, Edge::Negative) ? neg : nullptr;
            if (trace_on()) {
                // The callee classified its own write: only its facts are known here.
                TraceSeen t;
                t.pos = f.pos;
                t.neg = f.neg;
                t.any = f.changed;
                emit_trace_change(RT_TRACE_CHG_REF, sig, storage_ptr(slot_of(sig)), t);
            }
            service_wake_buckets(sig, f);
            if (slot_of(sig).is_array) {
                vcd_touch_array(sig);
            } else {
                vcd_touch_signal(sig);
            }
        }
        b_.CreateRetVoid();
    }
    silent_stores_       = saved_silent;
    ctx_override_anchor_ = saved_ovr_a;
    ctx_override_        = saved_ovr;
    own_ctx_             = saved_ctx;
    own_anchor_          = saved_anchor;
    state_               = saved_state;
    fn_                  = saved_fn;
    b_.restoreIP(saved_ip);

    return th;
}

// Call the wake thunk of a `ref` formal after a write, when the call passed one.
void Backend::emit_ref_wake_call(llvm::Value* fn, llvm::Value* ctx, llvm::Value* changed, llvm::Value* pos,
                                 llvm::Value* neg) {
    auto* f    = b_.GetInsertBlock()->getParent();
    auto* call = llvm::BasicBlock::Create(ctx_, "refwake.call", f);
    auto* cont = llvm::BasicBlock::Create(ctx_, "refwake.cont", f);
    b_.CreateCondBr(b_.CreateIsNotNull(fn), call, cont);
    b_.SetInsertPoint(call);
    auto* i1  = b_.getInt1Ty();
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(llvm::FunctionType::get(b_.getVoidTy(), { ptr, i1, i1, i1 }, false), fn,
                  { ctx, changed, pos, neg });
    b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
}

bool Backend::push_ref_wake(std::vector<llvm::Value*>& argv, const Guarded& lv, Ref) {
    if (!design_ref_wake_) {
        return false;
    }
    auto* null = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    if (lv.ref_wake_fn != nullptr) {
        // A ref formal passed on: the next callee wakes through the same thunk.
        argv.push_back(lv.ref_wake_fn);
        argv.push_back(lv.ref_wake_ctx);
        return true;
    }
    const bool by_signal = (lv.is_signal && signal_has_waiters(lv.sig)) ||
                           (!lv.is_signal && lv.mut_sig_valid && signal_has_waiters(lv.mut_sig));
    if (silent_stores_ || lv.is_assoc || (lv.chg == nullptr && !by_signal)) {
        argv.push_back(null);
        argv.push_back(null);
        return false;
    }
    llvm::Function* th  = emit_ref_wake_thunk(lv);
    llvm::Value*    ctx = lv.chg != nullptr       ? lv.chg_body
                          : lv.vif_ctx != nullptr ? lv.vif_ctx
                          : own_ctx_ != nullptr   ? own_ctx_
                                                  : static_cast<llvm::Value*>(null);
    argv.push_back(th);
    argv.push_back(ctx);
    return true;
}

} // namespace clotho::ir::backend
