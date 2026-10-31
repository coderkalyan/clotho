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
        emit_suspend("coro.resume");
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
    emit_suspend("coro.resume");
}

// Schedule a wheel timer for `now + ticks` that resumes this coroutine: stash
// the handle in the awaiter so the timer callback can find it, then arm the
// wheel. Parked, so the trampoline knows which ring to wake it into. Caller
// emits the suspend.
//
// With a remote `disable` in the design the record is a fresh heap allocation per
// arm, remembered in the frame slot so a `disable` can find and claim it; the
// wheel frees it when it eventually fires. Fresh per arm because a force-woken
// process can carry on with its old timer still armed, so one reused record would
// have two owners. The scheduled flag is deliberately left alone: mid-body it is
// set so the process's own sensitivity cannot re-wake it, and that must stay true.
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
    }
    b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(aw_ty, aw, 2));
    // Schedule-relative: the runtime adds `now` so this #delay crosses the ABI
    // once (rt_wheel_schedule_rel) rather than twice (rt_wheel_now then
    // rt_wheel_schedule).
    b_.CreateCall(wheel_schedule_rel_fn(), { wheel_, aw, ticks, enqueue_trampoline() });
}

// Store a { handle, flag_ptr } entry at a chosen slot of a queue array. The
// edge is no longer stored per entry — it is encoded by which sub-queue the
// entry lives in.
void Backend::emit_entry(llvm::ArrayType* arr_ty,
                         llvm::Value*     q,
                         llvm::Value*     slot_idx,
                         llvm::Value*     handle,
                         llvm::Value*     flag_ptr) {
    llvm::StructType* elem = wait_elem_ty();
    llvm::Value*      slot = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), slot_idx });
    b_.CreateStore(handle, b_.CreateStructGEP(elem, slot, 0));
    b_.CreateStore(flag_ptr, b_.CreateStructGEP(elem, slot, 1));
}

// Persistent (always-block) subscription: write a process's entry into the
// signal's static queue at a compile-time slot. Slots are handed out in
// process / wait-event order, matching the capacities build_wait_queues
// computed, so each array fills exactly once and is never compacted. Issued
// from the seed loop (emit_active_run) with the process's ramp handle and
// scheduled-flag pointer — by plain stores, never by resuming the coroutine,
// so it does not inhibit coro-elide of other processes. All subscriptions land
// before any servicing, so the fixed-trip scans never read an empty slot.
void Backend::emit_static_subscribe(Signal sig, uint32_t edge, llvm::Value* handle, llvm::Value* flag_ptr) {
    const auto i = static_cast<uint32_t>(sig);
    if (mod_.signal(sig).is_event) {
        // An event has no buckets: link a node from the contiguous persistent
        // array onto the event object's waiter list. Edge is meaningless for an
        // event (a trigger is not a transition), so it is ignored here -- every
        // event subscription is recorded once, on the one list.
        // A persistent subscription is never unlinked, so it needs no owner.
        emit_event_link(event_obj_for(sig), next_static_event_node(), handle, flag_ptr,
                        /*record_owner=*/false);
        return;
    }
    const uint32_t slot = sta_next_[i][edge]++;
    emit_entry(wait_sta_ty_[i][edge], sym(wait_sta_gv_[i][edge]), b_.getInt32(slot), handle, flag_ptr);
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
    const auto   i       = static_cast<uint32_t>(sig);
    llvm::Value* len_ptr = wait_base(wait_slot_len_[i][edge], wait_dyn_len_gv_[i][edge], i);
    llvm::Value* q       = wait_base(wait_slot_dyn_[i][edge], wait_dyn_gv_[i][edge], i);
    auto*        i32     = b_.getInt32Ty();
    llvm::Value* len     = b_.CreateLoad(i32, len_ptr);
    emit_len_full_check(b_, len, static_cast<uint32_t>(wait_dyn_ty_[i][edge]->getNumElements()),
                        kRingWaitDynamic);
    emit_entry(wait_dyn_ty_[i][edge], q, len, parked_handle(b_, hdl_), sched_ptr_);
    b_.CreateStore(b_.CreateAdd(len, b_.getInt32(1)), len_ptr);
}

// Suspend the coroutine and continue in a fresh resume block, woken by a wait
// queue rather than a wheel timer. Like a #delay's suspend: the default edge
// returns to the resumer, case 0 resumes here, case 1 is a destroy while
// suspended.
void Backend::emit_wait_suspend() {
    emit_suspend("coro.wait_resume");
}

// Lower an intra-body `@(a or posedge b ...)` event wait: register on EVERY
// listed (signal, edge)'s dynamic queue, arm our scheduled flag, then suspend
// once. A matching blocking store to any of the signals wakes us
// (service_dynamic_queue pushes our handle onto the active queue and clears
// that bucket's length); the scheduler resumes us in the fresh resume block.
//
// Flag arming: service_dynamic_queue now reads the flag, so we must store 0 to
// our &proc_sched slot before suspending or we would never be woken again.
//
// Eager removal (multi-signal only): when we resume, the bucket that fired is
// already cleared, but the OTHER buckets still hold our stale entry. Left in
// place they would spuriously wake us later and, in a forever loop, grow those
// buckets past their capacity (overflow). So compact ourselves out of each
// bucket on resume. A single-signal wait is in exactly one bucket, cleared on
// fire, so it needs no removal.
void Backend::lower_wait(ExtraIndex info) {
    const auto events = ir_->wait_edges(info);
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
            // park on b's object (LRM 15.5.5). The declared object is only the
            // power-on binding.
            emit_event_link(event_obj_for(e.signal), event_wait_node(ev_k++),
                            parked_handle(b_, hdl_), sched_ptr_,
                            /*record_owner=*/!event_is_mono(e.signal));
        } else {
            emit_dynamic_register(e.signal, e.edge);
        }
    }
    b_.CreateStore(b_.getInt64(0), sched_ptr_); // arm: ready to be woken
    emit_wait_suspend();                        // park until any event fires
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
            emit_event_unlink(event_wait_node(ev_k++),
                              event_is_mono(e.signal) ? event_obj(e.signal) : nullptr);
        } else if (remove_sigs) {
            emit_dynamic_remove(e.signal, e.edge);
        }
    }
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
    // one entry per group (signal, edge) — a fused group subscribes its
    // canonical set once, not once per member. The scan reads every slot
    // up to the capacity, so over-allocation would read a null flag ptr.
    for (const std::vector<WaitEvent>& evs : group_events_) {
        for (const WaitEvent& w : evs) {
            c.sta[static_cast<uint32_t>(w.signal)][static_cast<uint32_t>(w.edge)]++;
        }
    }
    for (const Process& p : mod_.processes()) {
        // Gather the distinct (signal, edge) pairs this process intra-body
        // waits on, across every Wait's event list. A process is a single
        // coroutine suspended at one wait at a time, so it holds at most one
        // dynamic slot per (signal, edge) — count each distinct pair once.
        std::vector<std::pair<Signal, uint32_t>> seen;
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

// The event object (LRM 15.5). One per statically-declared named event, laid
// out in the design-wide `clotho.events` array and addressed by a slot index:
//
//   { i64 trig, ptr waiters, i8 pending }
//     trig    — sim tick of the last fire, UINT64_MAX until the first one.
//               `e.triggered` is `trig == now` (LRM 15.5.3). FIELD 0 ON PURPOSE:
//               VCD registers a raw pointer to the tick at $dumpvars time, so
//               the object address and the tick address must coincide.
//     waiters — head of the intrusive waiter list (event_node_ty), or null.
//     pending — a `->> e` awaiting the NBA region (LRM 15.5.4).
//
// An event owns a LIST rather than the fixed-capacity per-(signal, edge)
// buckets a real signal uses, because a trigger has no value transition and
// because the waiter set of an event reached through a handle cannot be
// enumerated at compile time. See docs/design/event-handles.md.
llvm::StructType* Backend::event_obj_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(ctx_, { b_.getInt64Ty(), ptr, b_.getInt8Ty(), b_.getInt32Ty(), ptr });
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

// The object of a statically-declared event, as a folded constant GEP into
// clotho.events. Slot 0 is the reserved null object, so a real event's slot is
// always >= 1.
//
// A compile-time slot is only sound because a module definition that declares
// an event is never body-shared by module dedup: the event signal registers as
// an instance-specific hint (`extra-sig`), so each instance keeps its own
// signal, its own slot, and its own object. The previous per-event named
// globals relied on exactly the same property.
llvm::Value* Backend::event_obj(Signal sig) {
    const auto slot = event_slot_[static_cast<uint32_t>(sig)];
    return b_.CreateInBoundsGEP(events_arr_ty_, sym(events_gv_),
                                { b_.getInt32(0), b_.getInt32(slot) });
}

// The object a runtime event HANDLE names. The handle is { u32 slot, u32 gen }
// packed into an i64, so the slot is the low half; `gen` is carried for the
// dynamic-lifetime work and is not consulted while every object is statically
// bound and immortal.
//
// A null handle is all-zero, so it indexes slot 0 -- the reserved null object.
// Its waiter list is permanently empty and its trigger tick is the
// never-triggered sentinel, which is exactly LRM 15.5.5's `-> null` no-op and
// `null.triggered == 0`, with no branch on the hot path.
//
// The index is masked to the array bound so a corrupted or stale handle reads
// some object rather than off the end of the global; with only statically bound
// objects this is unreachable, and it costs one `and` against a constant.
llvm::Value* Backend::event_obj_of(llvm::Value* handle) {
    llvm::Value* slot = b_.CreateTrunc(handle, b_.getInt32Ty());
    // A compile-time slot in the static block folds to a constant address --
    // the whole point of giving scalar declared events their own tier.
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(slot);
        c != nullptr && c->getZExtValue() < event_slot_count_) {
        return b_.CreateInBoundsGEP(events_arr_ty_, sym(events_gv_), { b_.getInt32(0), slot });
    }
    // Otherwise the slot may name either tier, so let the pool resolve it: it
    // owns the static block as its first chunk, so one call covers both.
    return b_.CreateCall(event_ptr_fn(), { slot });
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

// Number one event object slot per declared event, reserving slot 0 for the null
// object. Split out of build_wait_queues because slot_const needs the numbering
// to bake each event's own handle into the state initializer, and that happens
// much earlier in the pipeline (emit_state_global).
//
// The walk is in signal order, so numbering is deterministic -- which is what
// lets `-> e` on a statically-bound event resolve to a constant object address
// with no indirection at all.
void Backend::assign_event_slots() {
    const auto sigs = mod_.signals();
    event_slot_.assign(sigs.size(), 0);
    event_slot_count_ = 1;
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        if (sigs[i].is_event) {
            event_slot_[i] = event_slot_count_++;
        }
    }
    analyze_event_monomorphism();
}

// Which event variables provably name ONE object for the whole run.
//
// An event's handle only ever changes by being assigned (LRM 15.5.5's merge and
// `= null`; event subroutine arguments and ports are refused today, and an
// aggregate element has no signal of its own). So an event signal that is never
// the target of a store keeps its power-on binding forever, and every reference
// to it can use the CONSTANT object address instead of loading the handle and
// indexing the array.
//
// That is the difference between paying for handles everywhere and paying for
// them only where they are used: an ordinary design, which never merges, gets
// exactly the code it got before handles existed.
//
// Conservative by construction: anything that stores to an event signal, or any
// store whose lvalue root cannot be resolved to a signal at all, clears the bit
// (the latter clears every event's, since the target is unknown).
void Backend::analyze_event_monomorphism() {
    event_mono_.assign(mod_.signals().size(), true);
    const auto visit = [&](const Ir& ir) {
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            // Every store form reaches its target through an lvalue in the same
            // operand position: Store/StoreNb are binary (l is the lvalue), the
            // delayed / intra forms are op_extra with `op` as the lvalue.
            Index lv{};
            switch (ir.tag(idx)) {
                case Inst::Tag::Store:
                case Inst::Tag::StoreNb:
                    lv = ir.payload(idx).binary.l;
                    break;
                case Inst::Tag::StoreNbDelay:
                case Inst::Tag::StoreInertialDelay:
                case Inst::Tag::StoreNbEvent:
                    lv = ir.payload(idx).op_extra.op;
                    break;
                default:
                    continue;
            }
            const LvalueRoot r = lvalue_root(ir, lv);
            if (ir.tag(r.root) != Inst::Tag::Signal) {
                continue; // a frame local or a formal: never an event today
            }
            const auto s = static_cast<uint32_t>(ir.payload(r.root).signal);
            if (s < event_slot_.size() && event_slot_[s] != 0) {
                event_mono_[s] = false;
            }
        }
    };
    for (const Process& p : mod_.processes()) {
        visit(p.ir);
    }
    for (uint32_t i = 0; i < mod_.function_count(); ++i) {
        visit(mod_.function(static_cast<FunctionId>(i)).ir);
    }
}

// True when `sig` is an event whose object is fixed at compile time.
bool Backend::event_is_mono(Signal sig) const {
    const auto i = static_cast<uint32_t>(sig);
    return i < event_mono_.size() && event_mono_[i];
}

// The object an event VARIABLE currently names: the folded constant when the
// variable is monomorphic, else a load of its handle and an index into the
// array.
llvm::Value* Backend::event_obj_for(Signal sig) {
    return event_is_mono(sig) ? event_obj(sig) : event_obj_of(load_event_handle(sig));
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

    // Event objects. Slots were numbered by assign_event_slots (it has to run
    // before emit_state_global, because a declared event's power-on value IS its
    // own handle). Here the array and the persistent-node pool are created, the
    // latter sized by the same static-subscription count that would have sized
    // the .sta bucket. The nodes are contiguous so an event's permanent waiter
    // prefix stays sequential in memory even though the walk is a pointer chase;
    // one-shot waiters are frame-resident and need no capacity at all.
    const uint32_t nslots = event_slot_count_;
    uint32_t       nnodes = 0;
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        if (sigs[i].is_event) {
            nnodes += c.sta[i][static_cast<uint32_t>(Edge::Any)];
        }
    }
    auto* obj_ty   = event_obj_ty();
    events_arr_ty_ = llvm::ArrayType::get(obj_ty, nslots);
    // Per-slot initializer: never-triggered sentinel, empty waiter list, not
    // pending, and the slot's dump-var index (-1 when it is not dumped, which is
    // every slot in a design without $dumpvars and the null object always).
    std::vector<llvm::Constant*> slot_init(
      nslots, llvm::ConstantStruct::get(
                obj_ty, { llvm::ConstantInt::get(b_.getInt64Ty(), ~uint64_t{ 0 }),
                          llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)),
                          llvm::ConstantInt::get(b_.getInt8Ty(), 0),
                          llvm::ConstantInt::get(b_.getInt32Ty(), ~uint32_t{ 0 }),
                          llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)) }));
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        const uint32_t slot = event_slot_[i];
        if (slot == 0 || !design_uses_vcd_ || vcd_index_[i] < 0) {
            continue;
        }
        slot_init[slot] = llvm::ConstantStruct::get(
          obj_ty, { llvm::ConstantInt::get(b_.getInt64Ty(), ~uint64_t{ 0 }),
                    llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)),
                    llvm::ConstantInt::get(b_.getInt8Ty(), 0),
                    llvm::ConstantInt::get(b_.getInt32Ty(),
                                           static_cast<uint64_t>(vcd_index_[i])),
                    llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)) });
    }
    events_gv_ = new llvm::GlobalVariable(
      env_->llmod, events_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantArray::get(events_arr_ty_, slot_init), "clotho.events");
    // Head of the NBA pending chain (`->> e`). A runtime list rather than a
    // compile-time set of target events, because the target is a handle.
    event_pend_head_gv_ = new llvm::GlobalVariable(
      env_->llmod, llvm::PointerType::getUnqual(ctx_), false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)),
      "clotho.event_pend_head");
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

    // Def-local wait-bucket slots (module-dedup prep): for a reused
    // definition, body-side queue access goes through a def-local slot in
    // the instance context (ctx_wait_base + slot) instead of a per-instance
    // queue global. Slot numbering follows the def-local signal walk and
    // the (padded, so def-uniform) bucket existence — identical across
    // twin instances by construction. main fills the pointers at startup.
    wait_slot_sta_.assign(sigs.size(), { no_wait_slot, no_wait_slot, no_wait_slot });
    wait_slot_dyn_.assign(sigs.size(), { no_wait_slot, no_wait_slot, no_wait_slot });
    wait_slot_len_.assign(sigs.size(), { no_wait_slot, no_wait_slot, no_wait_slot });
    for (uint32_t a = 0; a < anchors_.size(); ++a) {
        // A virtual interface reaches an instance chosen at run time, so its
        // buckets have to be addressable through the context record even when
        // the definition is instantiated once and its own body bakes symbols.
        const bool vif_anchor =
          std::ranges::any_of(by_anchor[a], [&](Signal sj) {
              return sj != Signal{ ~0u } && vif_sig_[static_cast<uint32_t>(sj)];
          });
        if (!anchors_[a].indirect && !vif_anchor) {
            continue;
        }
        uint32_t next = 0;
        for (const Signal sj : by_anchor[a]) {
            if (sj == Signal{ ~0u }) {
                continue;
            }
            const auto si = static_cast<uint32_t>(sj);
            for (uint32_t e = 0; e < 3; ++e) {
                if (wait_sta_gv_[si][e] != nullptr) {
                    wait_slot_sta_[si][e] = next++;
                }
                if (wait_dyn_gv_[si][e] != nullptr) {
                    wait_slot_dyn_[si][e] = next++;
                    wait_slot_len_[si][e] = next++;
                }
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
uint32_t Backend::slot_mm_bits(const LValue& lv) const {
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
Backend::ServiceSnapshot Backend::service_snapshot(const LValue& lv) {
    if (silent_stores_) {
        // A PreInit store: declaration initializers generate no events, and no
        // waiter can exist yet anyway.
        return {};
    }
    if (lv.is_string) {
        // A string's value is its CONTENT, not the handle the place holds, so the
        // old value has to be copied out rather than loaded as a word. Covers
        // every string write path by construction, including a `ref` / `out` /
        // `inout` string formal, which reaches this same bracket.
        return ServiceSnapshot{ .str = str_chg_snapshot(lv) };
    }
    if (lv.chg != nullptr && lv.chg->container) {
        // A container property never reaches here through a plane store (the
        // frontend has no such op) -- only through the `ref`/`inout` brackets,
        // whose lvalue is a handle with no mm word to load.
        return ServiceSnapshot{ .mm = nullptr, .z = nullptr, .container = true };
    }
    if (lv.chg != nullptr) {
        // A waited-on class property (LRM 9.4.2). Snapshot unconditionally: the
        // notifier that exists is the one some `@`/`wait` asked for, so there is no
        // "has waiters" question to fold in -- an empty waiter list makes the wake
        // walk a single null load.
        ServiceSnapshot old;
        old.mm = load_lv_mm(lv);
        old.z  = lv.has_z ? load_lv_z(lv) : nullptr;
        return old;
    }
    if (!lv.is_signal || !signal_has_waiters(lv.sig)) {
        return {};
    }
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

void Backend::service_after_write(const LValue& lv, ServiceSnapshot old) {
    if (old.str != nullptr) {
        str_chg_after(lv, old.str);
    } else if (old.container) {
        notify_container_mutation(lv);
    } else if (old.wide) {
        service_wide_window(lv, *old.wide);
    } else if (old.mm != nullptr && lv.chg != nullptr) {
        service_change_notify(lv, old.mm, load_lv_mm(lv), old.z, old.z != nullptr ? load_lv_z(lv) : nullptr);
    } else if (old.mm != nullptr) {
        service_wait_queue(lv.sig, old.mm, load_lv_mm(lv), old.z,
                           old.z != nullptr ? load_lv_z(lv) : nullptr);
    }
    // Mark the signal changed for the VCD dirty set. Independent of wait-queue
    // servicing: a dumped signal with no waiters still needs a touch.
    vcd_touch_lvalue(lv);
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
void Backend::service_wait_queue(Signal       sig,
                                 llvm::Value* old_mm,
                                 llvm::Value* new_mm,
                                 llvm::Value* old_z,
                                 llvm::Value* new_z) {
    if (!signal_has_waiters(sig)) {
        return;
    }
    const SignalSlot& slot = slot_of(sig);
    const EdgeFacts   f =
      emit_edge_facts(old_mm, new_mm, slot.width, slot.four_state, old_z, new_z,
                      signal_has_bucket(sig, Edge::Positive), signal_has_bucket(sig, Edge::Negative),
                      signal_has_bucket(sig, Edge::Any));
    service_wake_buckets(sig, f);
}

// Fire each of `sig`'s per-edge buckets that exists behind one branch on its fact.
void Backend::service_wake_buckets(Signal sig, const EdgeFacts& f) {
    const auto i = static_cast<uint32_t>(sig);

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
            emit_event_wake_list(vif_head_ptr(sig, e));
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
    const auto          have = [&](Edge e) { return cn.field[static_cast<uint32_t>(e)] != kNoNotify; };
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

// Static sub-queue scan (one edge bucket): a fixed-trip loop over the
// persistent subscriptions for this (signal, edge). The bucket's edge already
// matched the transition (the caller guarded the whole scan), so there is no
// per-entry edge check. Emitted via the generic runtime scan with a constant
// non-null base and constant cap: simplifycfg (lower_coroutines) folds the
// dead null guard and the constant trip count still lets the optimizer
// unroll. Membership never changes — no compaction, no length.
// Body-side base pointer of a wait bucket: loaded from the owning
// instance's wait-pointer table when its definition is reused (def-local
// slot constant — twin bodies stay identical), the absolute queue global
// otherwise. Commit functions / the seed loop keep the globals directly.
llvm::Value* Backend::wait_base(uint32_t slot, llvm::Constant* gv, uint32_t sig) {
    if (slot == no_wait_slot || !def_relative_for(slots_[sig])) {
        return sym(llvm::cast<llvm::GlobalVariable>(gv));
    }
    Anchor& a = anchors_[slots_[sig].anchor];
    return ctx_field(a, a.ctx_wait_base + slot, llvm::PointerType::getUnqual(ctx_));
}

void Backend::service_static_queue(Signal sig, uint32_t edge) {
    const auto i = static_cast<uint32_t>(sig);
    emit_static_scan_rt(wait_base(wait_slot_sta_[i][edge], wait_sta_gv_[i][edge], i),
                        b_.getInt32(wait_sta_cap_[i][edge]), wait_sta_padded_[i][edge] != 0,
                        trig_bits(sig, edge));
}

// Dynamic sub-queue scan (one edge bucket): a straight loop over the live
// one-shot waiters for this (signal, edge), via the generic runtime scan
// with a constant non-null base. The whole bucket is consumed (length
// resets to 0); the firing process deregisters its stale entries from any
// OTHER buckets itself on resume (eager removal in lower_wait).
void Backend::service_dynamic_queue(Signal sig, uint32_t edge) {
    const auto i = static_cast<uint32_t>(sig);
    emit_dynamic_scan_rt(wait_base(wait_slot_dyn_[i][edge], wait_dyn_gv_[i][edge], i),
                         wait_base(wait_slot_len_[i][edge], wait_dyn_len_gv_[i][edge], i));
}

// The detached timer node a `->> #d e` schedules: an rt_timer ({next,
// on_expire}) followed by the event handle to trigger. Carrying the handle is
// what lets ONE thunk serve every delayed trigger in the design -- the target of
// `->> #d e` can be any event expression, so there is no declared event to
// specialise on.
llvm::StructType* Backend::event_timer_ty() {
    auto* ptr = raw_ptr_ty();
    return llvm::StructType::get(ctx_, { ptr, ptr, b_.getInt64Ty() });
}

// The design's one detached on-expiry thunk. The wheel calls it as
// on_expire(rt_timer* t, void* ctx) (ctx, the wheel, is unused here); it reads
// the handle out of the node, pends that event for the NBA region, then frees the
// node the schedule site allocated.
//
// LRM 15.5.1: the update event of `->> #d e` "shall trigger the referenced event
// in the nonblocking assignment region" of the instant the delay expires. The
// wheel fires this at the top of that instant, ahead of its Active region, so
// firing the event here would wake the waiter in the Active region; pending it
// leaves the wake to the NBA drain, exactly as for `->> e`.
//
// Defined in whichever module first needs it and reached from every other one
// through sym(), like enqueue_trampoline: returning the bare definition to a
// second module is a cross-module global reference, which LLVM rejects
// (docs/design/module-split.md).
llvm::Function* Backend::event_delay_thunk() {
    if (event_delay_thunk_ != nullptr) {
        return sym(event_delay_thunk_);
    }
    auto*             ptr  = raw_ptr_ty();
    auto*             fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false);
    llvm::IRBuilder<> save(ctx_);
    auto*             saved_bb = b_.GetInsertBlock();
    auto              saved_pt = b_.GetInsertPoint();
    event_delay_thunk_ =
      llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.ev_delay", env_->llmod);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", event_delay_thunk_));
    auto* nty = event_timer_ty();
    llvm::Value* handle =
      b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(nty, event_delay_thunk_->getArg(0), 2));
    emit_event_pend_guarded(handle);
    b_.CreateCall(free_fn(), { event_delay_thunk_->getArg(0) });
    b_.CreateRetVoid();
    if (saved_bb != nullptr) {
        b_.SetInsertPoint(saved_bb, saved_pt);
    }
    return sym(event_delay_thunk_);
}

// Link an event object onto the NBA pending chain (LRM 15.5.4), unless it is
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
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(handle); c != nullptr && !c->isZero()) {
        emit_event_pend(event_obj_of(handle));
        return;
    }
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* pend = llvm::BasicBlock::Create(ctx_, "evpend.live", fn);
    auto* skip = llvm::BasicBlock::Create(ctx_, "evpend.null", fn);
    b_.CreateCondBr(b_.CreateICmpNE(handle, b_.getInt64(0)), pend, skip);
    b_.SetInsertPoint(pend);
    emit_event_pend(event_obj_of(handle));
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
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
    (void)null;
}

// Drain the NBA pending chain: fire every `->> e` staged this instant, clearing
// each object's pending mark as it goes. Reads `next` before firing, because a
// waiter woken by the fire may itself `->>` another event -- that link goes onto
// the (now empty) head and is drained by the NBA region's next pass, which is
// the deferral LRM 15.5.4 asks for.
void Backend::emit_event_pend_drain(llvm::Value* wheel) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* head = llvm::BasicBlock::Create(ctx_, "evpend.walk", fn);
    auto* body = llvm::BasicBlock::Create(ctx_, "evpend.fire", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "evpend.drained", fn);

    llvm::Value* headp = sym(event_pend_head_gv_);
    llvm::Value* first = b_.CreateLoad(ptr, headp);
    b_.CreateStore(null, headp);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    llvm::PHINode* n = b_.CreatePHI(ptr, 2);
    n->addIncoming(first, entry);
    b_.CreateCondBr(b_.CreateICmpNE(n, null), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* next = b_.CreateLoad(ptr, event_field(n, kEventNextPend));
    b_.CreateStore(null, event_field(n, kEventNextPend));
    b_.CreateStore(b_.getInt8(0), event_field(n, kEventPending));
    emit_event_fire(n, wheel);
    n->addIncoming(next, b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
}

// Stamp event `sig`'s trigger time (from `wheel`) and wake every waiter on its
// object's list. Shared by `-> e` in a process body (wheel_), the NBA-region
// `->> e` service (the drain's local wheel — wheel_ is stale there, so it must
// be passed in).
//
// `trig_id` is the object's RT_TRIG_EVENT id when the caller has it: a handle names its
// slot in its low half, so `-> e` gets it for free. The NBA service has only the object
// and asks the runtime.
void Backend::emit_event_fire(llvm::Value* obj, llvm::Value* wheel, llvm::Value* trig_id) {
    b_.CreateStore(b_.CreateCall(wheel_now_fn(), { wheel }), event_field(obj, kEventTrig));
    vcd_touch_event_obj(obj);
    emit_event_wake_list(obj);
    // An event is also a control for `x <= @e v` / `->> @(e) f`, which wait on the
    // object rather than on a signal edge. Every trigger form converges here.
    if (design_uses_event_triggers_) {
        emit_trigger_fire(trig_id != nullptr ? trig_id : b_.CreateCall(event_trig_id_fn(), { obj }));
    }
}

// `-> null` / `->> null` is a no-op (LRM 15.5.5): a null handle names nothing to
// trigger. Skipping the whole fire -- rather than letting it land on the
// reserved null object -- is what keeps `null.triggered` reading 0 afterwards,
// since a stamp on slot 0 would otherwise be observable through any null handle.
//
// Emitted only where the handle is not a compile-time non-null constant, so a
// plain `-> e` on a declared event pays nothing.
void Backend::emit_event_fire_guarded(llvm::Value* handle, llvm::Value* wheel) {
    // The object's trigger id, from the slot in the handle's low half.
    const auto trig = [&]() -> llvm::Value* {
        return design_uses_event_triggers_
                 ? b_.CreateOr(b_.CreateTrunc(handle, b_.getInt32Ty()), b_.getInt32(RT_TRIG_EVENT))
                 : nullptr;
    };
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(handle); c != nullptr && !c->isZero()) {
        emit_event_fire(event_obj_of(handle), wheel, trig());
        return;
    }
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* fire = llvm::BasicBlock::Create(ctx_, "ev.fire", fn);
    auto* skip = llvm::BasicBlock::Create(ctx_, "ev.null", fn);
    b_.CreateCondBr(b_.CreateICmpNE(handle, b_.getInt64(0)), fire, skip);
    b_.SetInsertPoint(fire);
    emit_event_fire(event_obj_of(handle), wheel, trig());
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
void Backend::emit_event_wake_list(llvm::Value* obj) {
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
    emit_wake_push(b_, hdl);
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
// `record_owner` is false when the caller can name the same object again at
// unlink time (a monomorphic event), which is the common case: the owner field
// then costs nothing at all.
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

// Unlink `node` from `obj`'s waiter list. Idempotent for an already-unlinked
// node only if it was cleared on unlink, which this does: both links are nulled
// so a second unlink is a no-op head-relink of a node that is no longer the
// head. Callers unlink exactly once, on resume.
// `obj` null means "read it back from the node", which a polymorphic event
// needs: the variable may have been reassigned while we were parked, and the
// node must come off the object it actually went onto.
void Backend::emit_event_unlink(llvm::Value* node, llvm::Value* obj) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = event_node_ty();
    if (obj == nullptr) {
        obj = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kNodeOwner));
    }
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

// A persistent (`always @(e)`) waiter's node: the next slot of the contiguous
// clotho.event_nodes array. Handed out in the same subscription order the
// static bucket slots used to be, so the count build_wait_queues reserved is
// filled exactly once.
llvm::Value* Backend::next_static_event_node() {
    return b_.CreateInBoundsGEP(event_nodes_arr_ty_, sym(event_nodes_gv_),
                                { b_.getInt32(0), b_.getInt32(event_node_next_++) });
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
    const auto        i      = static_cast<uint32_t>(sig);
    llvm::Value*      q      = wait_base(wait_slot_dyn_[i][edge], wait_dyn_gv_[i][edge], i);
    auto*             arr_ty = wait_dyn_ty_[i][edge];
    llvm::Value*      len_gv = wait_base(wait_slot_len_[i][edge], wait_dyn_len_gv_[i][edge], i);
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
    llvm::Value* src  = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), idx });
    llvm::Value* h    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 0));
    llvm::Value* f    = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(elem, src, 1));
    llvm::Value* mine = b_.CreateICmpEQ(h, parked_handle(b_, hdl_));
    b_.CreateCondBr(mine, cont, keep); // ours -> drop; else -> keep

    b_.SetInsertPoint(keep);
    llvm::Value* dst = b_.CreateInBoundsGEP(arr_ty, q, { b_.getInt32(0), w });
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

// -- NBA update region --
//
// After the active queue drains, the NBA region commits every staged update:
// Load edge element `e` of meta array field `field`. Meta layout (per nba-id):
// 0 sta_base[3], 1 sta_cap[3], 2 dyn_base[3], 3 dyn_len[3] — the wait-queue
// pointers emit_nba_service scans per edge.
llvm::Value* Backend::meta_at(llvm::Value* meta, unsigned field, uint32_t e, llvm::Type* ty) {
    return b_.CreateLoad(
      ty, b_.CreateInBoundsGEP(nba_meta_ty_, meta, { b_.getInt32(0), b_.getInt32(field), b_.getInt32(e) }));
}

// Generic static-queue scan (base + capacity values), null base => no
// waiters. Serves both callers: service_static_queue passes a constant
// global and cap; the NBA commit loop passes values read from meta. Each
// entry wakes its process only if its scheduled flag is 0, then sets the
// flag — collapsing a multi-signal sensitivity (the same process
// subscribed under several signals) into a single wake.
// `padded`: the bucket is capacity-padded to its definition's maximum (see
// build_wait_queues), so trailing slots are zeroinit — skip a null flag
// pointer instead of dereferencing it. Exact buckets keep the check-free
// scan.
// `trig`: trigger bits this (signal, edge) contributes to the wake word
// (compute_trigger_masks), or 0 when no guarded group waits on it. Note the
// already-queued path: it must still OR the bits in, or a group woken by two
// of its events in one instant loses the second one's bit and skips the
// members that were waiting for it. With `trig` 0 that path is empty and the
// emission is exactly the unguarded scan.
void Backend::emit_static_scan_rt(llvm::Value* base, llvm::Value* cap, bool padded, uint64_t trig) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* word = sched_word_ty();
    auto* elem = wait_elem_ty();
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* head = llvm::BasicBlock::Create(ctx_, "nsta.head", fn);
    auto* body = llvm::BasicBlock::Create(ctx_, "nsta.body", fn);
    auto* wake = llvm::BasicBlock::Create(ctx_, "nsta.wake", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "nsta.cont", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "nsta.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateCondBr(b_.CreateICmpNE(base, llvm::ConstantPointerNull::get(ptr)), head, done);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, cap), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* src  = b_.CreateInBoundsGEP(elem, base, idx);
    llvm::Value* h    = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 0));
    llvm::Value* fptr = b_.CreateLoad(ptr, b_.CreateStructGEP(elem, src, 1));
    if (padded) {
        auto* live = llvm::BasicBlock::Create(ctx_, "nsta.live", fn);
        b_.CreateCondBr(b_.CreateICmpNE(fptr, llvm::ConstantPointerNull::get(ptr)), live, cont);
        b_.SetInsertPoint(live);
    }
    llvm::Value* old   = b_.CreateLoad(word, fptr);
    auto*        queued = b_.CreateICmpNE(old, b_.getInt64(0));
    if (trig != 0) {
        auto* arm = llvm::BasicBlock::Create(ctx_, "nsta.arm", fn);
        b_.CreateCondBr(queued, arm, wake);
        b_.SetInsertPoint(arm);
        b_.CreateStore(b_.CreateOr(old, b_.getInt64(trig)), fptr);
        b_.CreateBr(cont);
    } else {
        b_.CreateCondBr(queued, cont, wake);
    }

    b_.SetInsertPoint(wake);
    emit_wake_push(b_, h);
    b_.CreateStore(b_.getInt64(1 | trig), fptr);
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), cont);
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
}

// Generic dynamic-queue scan (base + length pointer values), null base =>
// skip. Serves both callers, like emit_static_scan_rt. Wakes every live
// entry (flag-deduped) and resets the length to 0.
void Backend::emit_dynamic_scan_rt(llvm::Value* base, llvm::Value* lenptr) {
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
    emit_wake_push(b_, h);
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

} // namespace clotho::ir::backend
