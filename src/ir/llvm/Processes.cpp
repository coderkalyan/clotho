// Fork, disable, the process class, and semaphore and mailbox synchronization.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// The caller's record slot { ptr parent }: a single ptr alloca placed after the
// caller's coro.begin (so it is frame-resident across the task-call suspend) and
// reused by every task call in this coroutine — a coroutine is in at most one
// task call at a time. The task writes nothing to it on a synchronous return; on
// a blocking call the caller stores its handle here before suspending and the
// task reads it to resume the caller on completion.
llvm::Value* Backend::record_slot() {
    if (record_ == nullptr) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        record_ = ab.CreateAlloca(llvm::PointerType::getUnqual(ctx_), nullptr, "task_record");
    }
    return record_;
}

// The fork join-record type, shared by one fork site's children and its parent
// (see lower_fork_inst and the fork-child epilogue in lower_task):
//
//   { i32 count, ptr parent, i32 wake_at, i32 refs }
//
// `count` starts at the fork width and each child's epilogue decrements it.
// `wake_at` is the count value at which the parent is due to be resumed, which
// is the ONLY thing that distinguishes the join kinds at run time: 0 for `join`
// (the last child wakes the parent), W-1 for `join_any` (the first one does).
// `join_none` never parks a parent, so `parent` stays null and no count wakes
// anybody. Because `count` decreases strictly, `count == wake_at` is true at
// most once and needs no claim flag.
//
// Fields 0 and 1 are common to both kinds of record, and `join` needs ONLY those
// two: it blocks until every child is done, so its parent outlives them all, its
// record is a slot in the parent's frame reused across the coroutine's join
// sites, and the due completion is always the last one (a constant 0). It
// therefore gets the SHORT record, so a design that only ever uses `fork … join`
// pays nothing at all for the other two kinds -- not even the frame bytes.
//
// `join_any` / `join_none` add `wake_at` and `refs`, because their parent does
// NOT outlive its children: it can reach a second fork site while the first
// site's children are live (which would stomp a reused slot) and it can return
// outright (which would free the frame under them). Those records are
// heap-allocated per fork SITE and reference-counted by `refs` (the parent plus
// one per child); whoever drops the last one frees it.
llvm::StructType* Backend::fork_record_ty(bool detached) {
    auto*& cached = detached ? fork_record_ty_detached_ : fork_record_ty_;
    if (cached == nullptr) {
        auto* i32 = b_.getInt32Ty();
        auto* ptr = llvm::PointerType::getUnqual(ctx_);
        cached    = detached ? llvm::StructType::create(ctx_, { i32, ptr, i32, i32 },
                                                        "clotho.fork_record.detached")
                             : llvm::StructType::create(ctx_, { i32, ptr }, "clotho.fork_record");
    }
    return cached;
}

// The current coroutine's `join` record slot: one frame-resident alloca reused
// across this coroutine's `join` sites, which is sound because a `join`
// completes before the next fork at the same level runs. Hoisted to coro.begin
// so it spills into the frame -- it must outlive both the children's suspends
// and the parent's join suspend. Only `join` uses it; see fork_record_ty.
// The heap join record this body is currently parked on, or null. Only a design
// with process control has one: it exists so an ABORTED parent can detach itself
// from a join whose children are still live.
llvm::Value* Backend::fork_live_slot() {
    if (fork_live_ == nullptr) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        auto*             ptr = llvm::PointerType::getUnqual(ctx_);
        fork_live_            = ab.CreateAlloca(ptr, nullptr, "fork_live");
        ab.CreateStore(llvm::ConstantPointerNull::get(ptr), fork_live_);
    }
    return fork_live_;
}

// Leaving a body that may be parked at a fork: null the record's `parent` so a
// surviving child wakes nobody (this frame is about to be destroyed), and drop the
// reference this body held. A no-op on every normal path, where the join already
// cleared the slot -- one load and a not-taken branch.
void Backend::emit_fork_record_abandon() {
    if (!design_uses_process_ || fork_live_ == nullptr) {
        return;
    }
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* rty  = fork_record_ty(true);
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* rec  = b_.CreateLoad(ptr, fork_live_);
    auto*        drop = llvm::BasicBlock::Create(ctx_, "fork.abandon", fn);
    auto*        cont = llvm::BasicBlock::Create(ctx_, "fork.abandon.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(rec, null), drop, cont);
    b_.SetInsertPoint(drop);
    b_.CreateStore(null, b_.CreateStructGEP(rty, rec, 1));
    b_.CreateStore(null, fork_live_);
    emit_fork_record_release(b_, rec);
    b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
}

llvm::Value* Backend::fork_record_slot() {
    if (fork_record_ == nullptr) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        fork_record_ = ab.CreateAlloca(fork_record_ty(false), nullptr, "fork_record");
    }
    return fork_record_;
}

// Drop one reference to a heap fork record, freeing it when the last one goes.
// Called once by the parent (at the join point, whichever way it got there) and
// once by every child's epilogue. Never emitted for `join`, whose record lives
// in the parent's frame. Nothing may touch the record afterwards.
void Backend::emit_fork_record_release(llvm::IRBuilder<>& b, llvm::Value* rec) {
    auto*           rty  = fork_record_ty(true);
    auto*           i32  = b.getInt32Ty();
    llvm::Value*    rp   = b.CreateStructGEP(rty, rec, 3);
    llvm::Value*    left = b.CreateSub(b.CreateLoad(i32, rp), b.getInt32(1));
    b.CreateStore(left, rp);
    llvm::Function* fn   = b.GetInsertBlock()->getParent();
    auto*           dead = llvm::BasicBlock::Create(ctx_, "fork.rec.free", fn);
    auto*           live = llvm::BasicBlock::Create(ctx_, "fork.rec.live", fn);
    b.CreateCondBr(b.CreateICmpEQ(left, b.getInt32(0)), dead, live);
    b.SetInsertPoint(dead);
    b.CreateCall(free_fn(), { rec });
    b.CreateBr(live);
    b.SetInsertPoint(live);
}

// Lower a `Fork` (LRM 9.3.2). All three join kinds spawn the same per-child
// coroutines over one shared join record; they differ in how the parent leaves.
//
//   join      — park until every child is done   (record wake_at = 0)
//   join_any  — park until the FIRST is done, siblings stay live (wake_at = W-1)
//   join_none — do not park at all               (parent never stored)
//
// and in how a child starts. `join` resumes each child inline at the spawn, which
// is a legal order because the parent is about to block on all of them anyway,
// and it keeps children off the rings entirely. The other two must not run a
// child the parent is not waiting for: LRM 9.3.2 has the parent continue and the
// children start when it next blocks or ends. `join_none` therefore queues every
// child, and `join_any` runs them only until one completes and queues the rest --
// which is also what makes their live count unbounded (docs/design/fork-join.md).
void Backend::lower_fork_inst(Index i) {
    const auto fi     = ir_->extra_data<ForkInfo>(ir_->payload(i).extra);
    const auto kids   = ir_->region(fi.children); // FunctionId u32s
    auto*      ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto*      null   = llvm::ConstantPointerNull::get(ptr_ty);
    auto*      i32    = b_.getInt32Ty();
    const auto width  = static_cast<uint32_t>(kids.size());
    const bool none   = fi.kind == ForkKind::JoinNone;
    const bool any    = fi.kind == ForkKind::JoinAny;
    // A `join` record is normally a frame slot, which is sound because the parent
    // outlives every child. `kill` breaks that: the parent can be aborted with
    // children still live, and a child decrementing a freed frame's record is a
    // use-after-free (docs/design/process-class.md §11 hazard 2). So in a design
    // with process control every kind takes the heap, refcounted record.
    const bool heap   = fi.kind != ForkKind::Join || design_uses_process_;
    auto*      rty    = fork_record_ty(heap);

    // `join` keeps its record in the frame, reused across this coroutine's join
    // sites; the detaching kinds get one heap record per SITE, reference-counted
    // by every child plus us, because a child can outlive this frame entirely.
    llvm::Value* rec = nullptr;
    if (heap) {
        const uint64_t rsize = env_->llmod.getDataLayout().getTypeAllocSize(rty);
        rec                  = b_.CreateCall(alloc_fn(), { b_.getInt64(rsize) });
        b_.CreateStore(b_.getInt32(any ? width - 1 : 0), b_.CreateStructGEP(rty, rec, 2));
        b_.CreateStore(b_.getInt32(width + 1), b_.CreateStructGEP(rty, rec, 3));
        // Published in a frame slot for the duration of the join, so an abort
        // leaving this body can find the record and stop the children from waking
        // a handle that no longer exists (emit_fork_record_abandon). Only for a
        // kind that PARKS: `join_none` runs straight through, so there is no
        // suspend between the alloc and the release for an abort to land in, and
        // it never stores a parent for a surviving child to wake. That also keeps
        // the slot -- an alloca hoisted to coro.begin -- off the one path a plain
        // FUNCTION can reach (LRM 13.4.4, docs/design/fork-join.md §V5).
        if (design_uses_process_ && !none) {
            b_.CreateStore(rec, fork_live_slot());
        }
    } else {
        rec = fork_record_slot();
    }
    b_.CreateStore(b_.getInt32(width), b_.CreateStructGEP(rty, rec, 0));
    b_.CreateStore(null, b_.CreateStructGEP(rty, rec, 1));

    // Ramp arguments, shared by every child: (state, wheel, one pointer per
    // captured enclosing-frame local, rec). Each capture is an ArgDir::Ref
    // formal of every child (lower/Statements.cpp's lower_fork), passed exactly as
    // A capture is a GC box HANDLE by value, for every join kind
    // (docs/design/fork-join.md V4), so there is no place to bind, no relocation
    // epoch to carry and no signal-waiter snapshot to take: a handle is a handle
    // whoever holds it, and the box stays alive off the child's own frame, which
    // is a conservative GC root.
    const std::span<const Index> caps = ir_->region(fi.args);
    std::vector<llvm::Value*>    argv;
    argv.reserve(caps.size() + 4);
    argv.push_back(state_);
    // The child seeds its OWN state from this one at its ramp (lower_task): a
    // join_none / join_any child can outlive this frame, so it must not hold a
    // pointer into it.
    argv.push_back(rng_slot_);
    argv.push_back(wheel_);
    // The spawning thread's block: the child's ramp takes a pooled block of its
    // own and links itself in as a child of this one (LRM 9.6.3's descendant
    // relation).
    if (design_uses_process_) {
        argv.push_back(proc_arg());
    }
    // ... and its home region: a child inherits the spawning thread's, which is
    // this body's CURRENT one, not its lexical one (LRM 24.3.1).
    if (design_uses_home_arg_) {
        argv.push_back(callee_home());
    }
    // An `In` scalar formal is two params, must|may and z, exactly as an ordinary
    // call marshals one (lower_task_call_inst's last arm).
    for (const Index cap : caps) {
        const Plane pl = to_four(value_of(cap));
        argv.push_back(pl.mm);
        argv.push_back(pl.z);
    }
    argv.push_back(rec);

    for (const Index kid : kids) {
        llvm::Function* cf  = sym(fn_decls_[static_cast<uint32_t>(kid)]);
        llvm::Value*    hdl = b_.CreateCall(cf->getFunctionType(), cf, argv);
        if (none) {
            // Ramped and parked at its initial suspend; the drain resumes it (and
            // destroys it when it completes, like any other ring handle). Pushed
            // to this body's own region, the way a `#0` is.
            emit_region_push(b_, hdl);
            continue;
        }
        // `join_any` stops running children the moment one of them has finished:
        // the parent is due to proceed, and LRM 9.3.2 does not let the remaining
        // siblings delay it -- `fork n=n+1; n=n+2; join_any` must leave n at 1,
        // not 3, or join_any is just join with extra steps. Every child is still
        // CREATED here; the ones not run are queued and start when the parent next
        // blocks. `count == width` is "nobody has finished yet".
        llvm::BasicBlock* joined = nullptr;
        if (any) {
            llvm::Value* cnt  = b_.CreateLoad(i32, b_.CreateStructGEP(rty, rec, 0));
            auto*        run  = llvm::BasicBlock::Create(ctx_, "fork.child.run", fn_);
            auto*        park = llvm::BasicBlock::Create(ctx_, "fork.child.queue", fn_);
            joined            = llvm::BasicBlock::Create(ctx_, "fork.child.spawned", fn_);
            b_.CreateCondBr(b_.CreateICmpEQ(cnt, b_.getInt32(width)), run, park);
            b_.SetInsertPoint(park);
            emit_region_push(b_, hdl);
            b_.CreateBr(joined);
            b_.SetInsertPoint(run);
        }
        // Resume inline to the child's first suspend or to completion. A child
        // that completes synchronously has already decremented the count (parent
        // still null -> it wakes nobody); destroy its frame here, since it never
        // reached a ring for the drain to do it. One that suspends is parked in
        // the wheel / a wait queue and decrements + wakes on completion.
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        llvm::Value* done = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { hdl });
        auto*        dz   = llvm::BasicBlock::Create(ctx_, "fork.child.done", fn_);
        auto*        nx   = llvm::BasicBlock::Create(ctx_, "fork.child.next", fn_);
        b_.CreateCondBr(done, dz, nx);
        b_.SetInsertPoint(dz);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        b_.CreateBr(nx);
        b_.SetInsertPoint(nx);
        if (joined != nullptr) {
            b_.CreateBr(joined);
            b_.SetInsertPoint(joined);
        }
    }

    if (!none) {
        // Park unless the wake we would be waiting for has already happened:
        // `join` needs every child done (count 0), `join_any` needs any one of
        // them (count below the width). Otherwise store our handle and suspend —
        // the child that brings the count to wake_at resumes us, into the ring
        // that parked handle names.
        llvm::Value* cnt  = b_.CreateLoad(i32, b_.CreateStructGEP(rty, rec, 0));
        auto*        susp = llvm::BasicBlock::Create(ctx_, "fork.join.suspend", fn_);
        auto*        cont = llvm::BasicBlock::Create(ctx_, "fork.join.cont", fn_);
        llvm::Value* got  = any ? b_.CreateICmpULT(cnt, b_.getInt32(width))
                                : b_.CreateICmpEQ(cnt, b_.getInt32(0));
        b_.CreateCondBr(got, cont, susp);

        b_.SetInsertPoint(susp);
        b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(rty, rec, 1));
        // Say that this park is already scheduled, for the same reason
        // emit_zero_delay_push does: the wake we are waiting for is guaranteed to
        // arrive -- the due child delivers it from its epilogue, on the abort path
        // as much as the normal one (a killed child still decrements and still
        // wakes us, §7.1 point 1). A `kill` or `disable` landing on us meanwhile
        // must therefore stand down rather than push a second copy of this handle:
        // two copies resumed the activation twice, and the second resume jumped
        // through the frame the first one had already destroyed -- a call through
        // a null function pointer out of the drain.
        //
        // The child's wake is an unconditional push and does not consult the flag,
        // so it still lands. Every later park re-arms the flag before suspending,
        // so leaving it set here strands nothing.
        if (park_claim_wanted() && sched_ptr_ != nullptr) {
            b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), sched_ptr_);
        }
        emit_suspend("fork.join.resume");
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
    }
    // Past the join point this body never reads the record again, on any path, so
    // drop our reference — but only for a heap record. A `join` record is a frame
    // slot with no refcount at all, and its children are all done by now.
    if (heap) {
        if (design_uses_process_ && !none) {
            b_.CreateStore(llvm::ConstantPointerNull::get(ptr_ty), fork_live_slot());
        }
        emit_fork_record_release(b_, rec);
    }
}

// `disable <block in another process>` (LRM 9.6.2, remote form). Mark the
// activation inside the target region and make it ready; it aborts itself at its
// own poll point, in its own frame, so nothing is destroyed from outside.
//
// Waking it exactly once is the whole difficulty, because the source it is parked
// on will still fire later. Two cases, and the occupant table carries what each
// needs:
//
// - Parked on a #delay. Its park record is live, so CLAIM it: the wheel's
//   trampoline pushes only if it finds the claim clear, so the timer that fires
//   afterwards frees the record and wakes nobody. The scheduled flag is set
//   mid-body on purpose and says nothing about being queued, so it is not
//   consulted here.
// - Parked on an event, or already queued. Then there is no live record, and the
//   scheduled flag is the design's existing one-wake-per-process token: push only
//   if it is clear, exactly as a wait-queue scan does. That is also what keeps the
//   ring bound intact -- an occupant is a process already counted in `nproc`.
void Backend::lower_disable_remote(Index i) {
    lower_disable_list(ir_->payload(i).label);
}

// `disable <task>` (LRM 9.6.2): end EVERY activation, "whether the processes
// were forked by the calling thread or not". Walk the target's activation list,
// mark each node and wake it the same two ways a block occupant is woken —
// claim its park record if it is on a #delay, else its scheduled flag.
//
// The activation running this statement is skipped: it is not parked, so there
// is nothing to wake, and pushing its handle would resume a coroutine that goes
// on to finish. It ends through the DisableAbort the frontend emits after this.
// Force-wake the activation `node` describes -- a `disable_node_ty`, whether it
// is a `disable` target's activation list entry or the head of a thread's
// activation stack -- EXACTLY once, then branch to `after`.
//
// Exactly once is the whole difficulty, and it is why the park has to be
// claimable. The source the activation is parked on stays armed: the wheel cannot
// cancel, and a second resume lands at whatever suspend point the body has
// reached by then (a later `#delay` firing early, or a resume of a completed
// body's freed frame). So:
//   - parked on a `#delay`: claim the per-arm heap park record (field 4). The
//     wheel's trampoline pushes only if it finds the claim clear, so the timer
//     that fires later frees the record and wakes nobody.
//   - parked on an event, a wait or a `#0` batch: there is no live record, and the
//     scheduled flag is the design's existing one-wake-per-thread token.
// Shared by remote `disable` (LRM 9.6.2) and by `kill` / `disable fork` (LRM 9.7,
// 9.6.3), which need the identical protocol against the identical node shape.
void Backend::emit_node_force_wake(llvm::Value* node, llvm::Value* hdl, llvm::BasicBlock* after) {
    auto* nty    = disable_node_ty();
    auto* aw     = awaiter_ty();
    auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto* null   = llvm::ConstantPointerNull::get(ptr_ty);
    auto* fn     = b_.GetInsertBlock()->getParent();

    auto* timed = llvm::BasicBlock::Create(ctx_, "wake.timed", fn);
    auto* take  = llvm::BasicBlock::Create(ctx_, "wake.claim", fn);
    auto* evt   = llvm::BasicBlock::Create(ctx_, "wake.evented", fn);
    auto* push  = llvm::BasicBlock::Create(ctx_, "wake.push", fn);

    llvm::Value* slot = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(nty, node, 3));
    llvm::Value* rec  = b_.CreateLoad(ptr_ty, slot);
    b_.CreateCondBr(b_.CreateICmpNE(rec, null), timed, evt);

    b_.SetInsertPoint(timed);
    llvm::Value* claim = b_.CreateLoad(b_.getInt32Ty(), b_.CreateStructGEP(aw, rec, 4));
    b_.CreateCondBr(b_.CreateICmpEQ(claim, b_.getInt32(0)), take, after);

    b_.SetInsertPoint(take);
    b_.CreateStore(b_.getInt32(1), b_.CreateStructGEP(aw, rec, 4));
    b_.CreateStore(null, slot);
    b_.CreateBr(push);

    b_.SetInsertPoint(evt);
    llvm::Value* flag = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(nty, node, 4));
    b_.CreateCondBr(b_.CreateICmpEQ(b_.CreateLoad(sched_word_ty(), flag), b_.getInt64(0)), push, after);

    b_.SetInsertPoint(push);
    emit_wake_push(b_, hdl);
    b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), b_.CreateLoad(ptr_ty, b_.CreateStructGEP(nty, node, 4)));
    b_.CreateBr(after);
}

void Backend::lower_disable_list(uint32_t target) {
    auto* nty    = disable_node_ty();
    auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto* null   = llvm::ConstantPointerNull::get(ptr_ty);
    auto* fn     = b_.GetInsertBlock()->getParent();

    auto* head  = llvm::BasicBlock::Create(ctx_, "disable.walk", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "disable.node", fn);
    auto* self  = llvm::BasicBlock::Create(ctx_, "disable.node.wake", fn);
    auto* step  = llvm::BasicBlock::Create(ctx_, "disable.node.next", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "disable.walk.done", fn);

    llvm::Value* first = b_.CreateLoad(ptr_ty, disable_head_ptr(b_, target));
    auto*        entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    llvm::PHINode* n = b_.CreatePHI(ptr_ty, 2);
    n->addIncoming(first, entry);
    b_.CreateCondBr(b_.CreateICmpNE(n, null), body, done);

    // Read `next` before marking: the node is frame-resident and the activation
    // may unlink itself as soon as it runs, which cannot happen before we
    // suspend, but keeping the load first makes that independent of scheduling.
    b_.SetInsertPoint(body);
    llvm::Value* next = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(nty, n, 0));
    b_.CreateStore(b_.getInt32(1), b_.CreateStructGEP(nty, n, 5));
    llvm::Value* hdl = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(nty, n, 2));
    // Skip ourselves, when this `disable` runs inside one of the activations.
    llvm::Value* mine = hdl_ != nullptr ? b_.CreateICmpEQ(hdl, parked_handle(b_, hdl_)) : b_.getFalse();
    b_.CreateCondBr(mine, step, self);

    b_.SetInsertPoint(self);
    emit_node_force_wake(n, hdl, step);

    b_.SetInsertPoint(step);
    b_.CreateBr(head);
    n->addIncoming(next, step);

    b_.SetInsertPoint(done);
}

// Back from a park inside a targeted task: if a `disable` marked this
// activation, leave through the epilogue, which skips copy-out for it.
void Backend::emit_task_abort_poll() {
    if (disable_node_ == nullptr) {
        return;
    }
    auto*        fn    = b_.GetInsertBlock()->getParent();
    llvm::Value* abort = b_.CreateLoad(b_.getInt32Ty(), b_.CreateStructGEP(disable_node_ty(), disable_node_, 5));
    auto*        cont  = llvm::BasicBlock::Create(ctx_, "disable.task.resume", fn);
    b_.CreateCondBr(b_.CreateICmpNE(abort, b_.getInt32(0)), task_epilogue_bb_, cont);
    b_.SetInsertPoint(cont);
}

// After a suspend inside one or more remotely-named blocks: if a `disable`
// landed while we were parked, leave that block now. Outermost-first, because
// leaving an outer block subsumes leaving every inner one, and the branch is
// the same structured exit the lexical form uses -- so the end block's own
// bookkeeping clears the mark and the occupant slot.
//
// Emitted only after a suspending instruction, and only while some enclosing
// block is remotely named, so a design without a remote `disable` emits none of
// it. It runs after the suspending op's own resume bookkeeping (a multi-signal
// `wait` compacts itself out of its buckets first), which is why the hook is
// here and not inside emit_suspend.
void Backend::emit_disable_poll() {
    auto* fn = b_.GetInsertBlock()->getParent();
    for (const OpenNamedBlock& blk : block_targets_) {
        if (blk.remote == kNoDisableTarget) {
            continue;
        }
        llvm::Value* pend =
          b_.CreateLoad(b_.getInt32Ty(), b_.CreateStructGEP(disable_node_ty(), blk.node, 5));
        auto* cont = llvm::BasicBlock::Create(ctx_, "disable.resume", fn);
        b_.CreateCondBr(b_.CreateICmpNE(pend, b_.getInt32(0)), blk.end, cont);

        b_.SetInsertPoint(cont);
    }
}

// Whether an instruction of this tag can park the process, so a remote
// `disable` may have landed by the time it returns.
bool Backend::tag_suspends(Inst::Tag t) {
    switch (t) {
        case Inst::Tag::Delay:
        case Inst::Tag::Wait:
        case Inst::Tag::TaskCall:
        case Inst::Tag::TaskCallVirtual:
        case Inst::Tag::Fork:
        case Inst::Tag::ExpectWait:
        case Inst::Tag::ProcWait:
        case Inst::Tag::SyncWait:
            return true;
        default:
            return false;
    }
}

// Whether an abort has landed on this activation while it was parked: a
// `kill` / `disable fork` on its thread, or a remote `disable` of a block or
// task activation it is inside. Null when the design uses neither feature, so
// the caller emits no test at all.
//
// The polls this duplicates (emit_proc_abort_poll, emit_disable_poll,
// emit_task_abort_poll) are emitted by lower_region AFTER the whole
// instruction, which is enough for a park that is one instruction. It is not
// enough for an op whose own lowering is a RETRY LOOP -- `await`, `wait fork`
// -- because the loop re-parks without ever reaching the end of the
// instruction: the force-wake would be answered by a fresh park and the thread
// would never abort. Such a loop tests this on resume and leaves the loop, and
// the poll then takes the real exit.
llvm::Value* Backend::emit_abort_pending() {
    llvm::Value* any = nullptr;
    const auto   fold = [&](llvm::Value* bit) {
        any = any == nullptr ? bit : b_.CreateOr(any, bit);
    };
    if (design_uses_process_ && pcb_ != nullptr) {
        llvm::Value* ctl = b_.CreateLoad(b_.getInt32Ty(), pcb_field(pcb_, kPcbCtl));
        fold(b_.CreateICmpNE(b_.CreateAnd(ctl, b_.getInt32(kProcAbort)), b_.getInt32(0)));
    }
    if (design_uses_disable_) {
        const auto node_abort = [&](llvm::Value* node) {
            llvm::Value* mark = b_.CreateLoad(
              b_.getInt32Ty(), b_.CreateStructGEP(disable_node_ty(), node, 5));
            fold(b_.CreateICmpNE(mark, b_.getInt32(0)));
        };
        if (disable_node_ != nullptr) {
            node_abort(disable_node_);
        }
        for (const OpenNamedBlock& blk : block_targets_) {
            if (blk.remote != kNoDisableTarget) {
                node_abort(blk.node);
            }
        }
    }
    return any;
}

// On resume inside a retry-loop park: if an abort has landed, branch to `leave`
// -- the loop's own exit block -- so the instruction ends and lower_region's
// poll can take the activation's real abort exit. Emits nothing in a design
// with neither `kill` nor a remote `disable`.
void Backend::emit_wait_abort_exit(llvm::BasicBlock* leave) {
    llvm::Value* pend = emit_abort_pending();
    if (pend == nullptr) {
        return;
    }
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* cont = llvm::BasicBlock::Create(ctx_, "proc.wait.alive", fn);
    b_.CreateCondBr(pend, leave, cont);
    b_.SetInsertPoint(cont);
}

// -- Process control (the `process` class, LRM 9.7) --
//
// A thread's control block, mirroring `rt_proc` (Runtime.h) field for field --
// the two change together, the rt_event convention. Native, untraced storage:
// nothing here is a class handle, so the tree and the park state are invisible
// to any collector and every walk below is plain GEPs.
//
//   { i32 ctl, i32 refs, ptr acts, ptr waiters, ptr rng, ptr join_rec,
//     ptr obj, ptr parent, ptr first_child, ptr next_sib, ptr prev_sib }
//
// See docs/design/process-class.md §4.
llvm::StructType* Backend::pcb_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    auto* i32 = b_.getInt32Ty();
    return llvm::StructType::get(ctx_,
                                { i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr });
}

// The value a call passes as the callee's `proc` parameter: this thread's block,
// or null where there is no thread (a DPI export thunk, the $monitor ramp, a
// fused thunk group).
llvm::Value* Backend::proc_arg() {
    return pcb_ != nullptr ? pcb_
                           : static_cast<llvm::Value*>(
                               llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
}

llvm::Value* Backend::pcb_field(llvm::Value* pcb, uint32_t field) {
    return b_.CreateStructGEP(pcb_ty(), pcb, field);
}

// The static control blocks: one per process, at a constant address, because
// `mod_.processes()` is a compile-time set. The INITIALIZER carries the starting
// state, so a design pays no startup code for them at all -- and every process's
// block is valid from time zero, before its own ramp has run.
llvm::GlobalVariable* Backend::proc_pcb_table() {
    if (proc_pcb_gv_ == nullptr) {
        const auto n  = std::max<size_t>(mod_.processes().size(), 1);
        auto*      ty = llvm::ArrayType::get(pcb_ty(), n);
        auto*      i32 = b_.getInt32Ty();
        auto*      ptr = llvm::PointerType::getUnqual(ctx_);
        auto*      null = llvm::ConstantPointerNull::get(ptr);
        std::vector<llvm::Constant*> fields{ llvm::ConstantInt::get(i32, kProcRunning | kProcStatic),
                                             llvm::ConstantInt::get(i32, 1) };
        for (uint32_t k = 2; k <= kPcbNextMarked; ++k) {
            fields.push_back(null);
        }
        auto* one = llvm::ConstantStruct::get(pcb_ty(), fields);
        proc_pcb_arr_ty_ = ty;
        proc_pcb_gv_ =
          new llvm::GlobalVariable(root_env_->llmod, ty, false, llvm::GlobalValue::InternalLinkage,
                                   llvm::ConstantArray::get(ty, std::vector<llvm::Constant*>(n, one)),
                                   "clotho.proc_pcb");
    }
    return proc_pcb_gv_;
}

llvm::Value* Backend::proc_pcb_slot(uint32_t proc_index) {
    auto* gv = proc_pcb_table();
    return b_.CreateInBoundsGEP(proc_pcb_arr_ty_, sym(gv),
                                { b_.getInt32(0), b_.getInt32(proc_index) });
}

// The per-program-instance TERMINATION ROOT (LRM 24.3): one static control block
// per program instance, which is a tree NODE and never a thread.
//
// It exists because the clause terminates a program's descendant threads only
// "when all initial procedures within a program have reached their end", while a
// thread's own completion hands its live children to its PARENT
// (rt_proc_complete) -- and a program `initial` is a statically created process,
// whose parent is null, so the first `initial` of a two-`initial` program would
// drop its children out of the tree before the second one ended. Every program
// body therefore hands its children here on its way out (rt_proc_adopt), which
// makes the subtree of root[i] exactly "every thread still live that descends
// from an `initial` of program instance i" -- the set the clause names, closed
// transitively because the tree is.
//
// Rooting the kill here also keeps the walk off the `initial`s themselves: they
// stay parentless, as today, so the thread running the LAST one is not marked
// KILLED on its way to an ordinary completion, and `wait fork` / `disable fork`
// inside an `initial` keep seeing that thread's own descendants and nothing else
// -- a live `initial` is never handed another one's orphans.
//
// FINISHED rather than RUNNING because it is not a thread and never runs;
// kProcStatic keeps it off the pool freelist (proc_maybe_free), like every block
// in clotho.proc_pcb. Created on first use, so a design with no program never
// emits it.
llvm::Value* Backend::program_root_pcb(uint32_t inst) {
    if (program_root_gv_ == nullptr) {
        const auto                   n    = std::max<size_t>(program_inst_procs_.size(), 1);
        auto*                        ty   = llvm::ArrayType::get(pcb_ty(), n);
        auto*                        i32  = b_.getInt32Ty();
        auto*                        ptr  = llvm::PointerType::getUnqual(ctx_);
        auto*                        null = llvm::ConstantPointerNull::get(ptr);
        std::vector<llvm::Constant*> fields{ llvm::ConstantInt::get(i32, kProcFinished | kProcStatic),
                                             llvm::ConstantInt::get(i32, 1) };
        for (uint32_t k = 2; k <= kPcbNextMarked; ++k) {
            fields.push_back(null);
        }
        auto* one            = llvm::ConstantStruct::get(pcb_ty(), fields);
        program_root_arr_ty_ = ty;
        program_root_gv_ =
          new llvm::GlobalVariable(root_env_->llmod,
                                   ty,
                                   false,
                                   llvm::GlobalValue::InternalLinkage,
                                   llvm::ConstantArray::get(ty, std::vector<llvm::Constant*>(n, one)),
                                   "clotho.program_root");
    }
    return b_.CreateInBoundsGEP(program_root_arr_ty_, sym(program_root_gv_), { b_.getInt32(0), b_.getInt32(inst) });
}

llvm::FunctionCallee Backend::proc_alloc_fn() {
    return rt_fn("rt_proc_alloc", raw_ptr_ty(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_complete_fn() {
    return rt_fn("rt_proc_complete", raw_ptr_ty(), { raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_mark_kill_fn() {
    return rt_fn("rt_proc_mark_kill", raw_ptr_ty(), { raw_ptr_ty(), b_.getInt32Ty() });
}

llvm::FunctionCallee Backend::proc_adopt_fn() {
    return rt_fn("rt_proc_adopt", b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_thread_done_fn() {
    return rt_fn("rt_proc_thread_done", b_.getVoidTy(), { raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_obj_retain_fn() {
    return rt_fn("rt_proc_obj_retain", b_.getVoidTy(), { raw_ptr_ty() });
}

// Move this thread's LRM 9.7 state from `from` to `to`, preserving the flag bits
// that share the word and leaving any OTHER state alone. Conditional, and that is
// load-bearing rather than defensive: the park/resume pair below maintains
// RUNNING <-> WAITING at every suspension, and an unconditional store there would
// erase a SUSPENDED set by another thread (its `resume` would then find no
// suspended thread to restart) or a KILLED set by `kill`. Branch-free: one load,
// one compare, one select, one store.
//
// A no-op in a body with no control block: either the design never names
// `process`, or this is a function, which is not a thread.
void Backend::emit_proc_state(uint32_t from, uint32_t to) {
    if (pcb_ == nullptr) {
        return;
    }
    auto*        i32 = b_.getInt32Ty();
    llvm::Value* p   = pcb_field(pcb_, kPcbCtl);
    llvm::Value* cur = b_.CreateLoad(i32, p);
    llvm::Value* is  = b_.CreateICmpEQ(b_.CreateAnd(cur, b_.getInt32(kProcStateMask)),
                                      b_.getInt32(from));
    llvm::Value* moved =
      b_.CreateOr(b_.CreateAnd(cur, b_.getInt32(~kProcStateMask)), b_.getInt32(to));
    b_.CreateStore(b_.CreateSelect(is, moved, cur), p);
}

// The `process` object naming `pcb`, created on first demand and memoized in the
// block. One object per thread is what makes handle identity a pointer compare:
// `process::self() == process::self()` has to hold, and clotho compares class
// handles with a bare icmp (docs/design/process-class.md §5.1).
llvm::Value* Backend::emit_proc_object(llvm::Value* pcb, Ref class_ref) {
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* ptr   = llvm::PointerType::getUnqual(ctx_);
    auto* null  = llvm::ConstantPointerNull::get(ptr);
    auto* slot  = pcb_field(pcb, kPcbObj);
    auto* have  = b_.CreateLoad(ptr, slot);
    auto* make  = llvm::BasicBlock::Create(ctx_, "proc.obj.new", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "proc.obj", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateCondBr(b_.CreateICmpEQ(have, null), make, done);

    b_.SetInsertPoint(make);
    llvm::Type*    body_ty = class_body_ty(class_ref);
    const uint64_t size    = env_->llmod.getDataLayout().getTypeAllocSize(body_ty);
    llvm::Value*   obj     = b_.CreateCall(alloc_fn(), { b_.getInt64(size) });
    b_.CreateMemSet(obj, b_.getInt8(0), b_.getInt64(size), llvm::MaybeAlign(64));
    b_.CreateStore(class_vptr(class_ref), b_.CreateStructGEP(body_ty, obj, 0));
    // The one body field: the block this handle names (a chandle-shaped word, so
    // no collector ever follows it).
    b_.CreateStore(b_.CreatePtrToInt(pcb, b_.getInt64Ty()), b_.CreateStructGEP(body_ty, obj, 1));
    b_.CreateStore(obj, slot);
    b_.CreateCall(proc_obj_retain_fn(), { pcb });
    b_.CreateBr(done);

    b_.SetInsertPoint(done);
    llvm::PHINode* result = b_.CreatePHI(ptr, 2);
    result->addIncoming(have, entry);
    result->addIncoming(obj, make);
    return b_.CreatePtrToInt(result, b_.getInt64Ty());
}

// The control block a `process` handle names, with the null-handle guard every
// class-handle dereference gets (LRM 8.4).
llvm::Value* Backend::proc_pcb_of(llvm::Value* handle, Index at) {
    emit_null_handle_check(handle, at);
    llvm::Value* obj = b_.CreateIntToPtr(handle, raw_ptr_ty());
    // Field 1 of the object body is the block pointer; field 0 is the vptr.
    llvm::Value* w = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(proc_obj_body_ty(), obj, 1));
    return b_.CreateIntToPtr(w, raw_ptr_ty());
}

// The object body shape of the `process` class: { ptr vptr, i64 pcb }. Built
// here rather than through class_body_ty so a use site does not need the class
// Ref (the frontend puts it on the instruction that needs it).
llvm::StructType* Backend::proc_obj_body_ty() {
    return llvm::StructType::get(ctx_, { llvm::PointerType::getUnqual(ctx_), b_.getInt64Ty() });
}

// Wake every thread parked in `p.await()` on this block. The list is the event
// waiter list shape (event_node_ty: next / prev / handle / flag / owner), and the
// walk is the same one emit_event_wake_list runs, for the same reasons: read
// `next` before the wake, because the woken thread unlinks its own node, and gate
// on the scheduled flag, the design's one-wake-per-thread token.
void Backend::emit_proc_wake_waiters(llvm::Value* head) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = event_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    auto* walk = llvm::BasicBlock::Create(ctx_, "await.walk", fn);
    auto* body = llvm::BasicBlock::Create(ctx_, "await.node", fn);
    auto* wake = llvm::BasicBlock::Create(ctx_, "await.wake", fn);
    auto* step = llvm::BasicBlock::Create(ctx_, "await.next", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "await.done", fn);

    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(walk);

    b_.SetInsertPoint(walk);
    llvm::PHINode* n = b_.CreatePHI(ptr, 2);
    n->addIncoming(head, entry);
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
    b_.CreateBr(walk);
    n->addIncoming(next, step);

    b_.SetInsertPoint(done);
}

// End the calling thread: publish its final state, hand its live children to its
// own parent, wake everything awaiting it, and drop the reference the thread
// held. Emitted at a process body's natural end and in a fork child's epilogue --
// the two places a thread stops existing. An `always` body never reaches one,
// which is correct: only `kill`, `disable` or `$finish` end it.
void Backend::emit_proc_complete() {
    if (pcb_ == nullptr) {
        return;
    }
    // FINISHED or KILLED is decided by the abort mark the runtime reads off the
    // block, not by the emitting site: one hook serves the normal exit and the
    // aborted one, which arrive at the same place.
    // Two waiter lists come back: this thread's own `await`ers, and -- when this
    // completion emptied the parent's subtree -- whatever is parked in the
    // parent's `wait fork`.
    llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
    llvm::Value*      forkw = ab.CreateAlloca(llvm::PointerType::getUnqual(ctx_), nullptr, "fork_waiters");
    llvm::Value* waiters    = b_.CreateCall(proc_complete_fn(), { pcb_, forkw });
    emit_proc_wake_waiters(waiters);
    emit_proc_wake_waiters(b_.CreateLoad(llvm::PointerType::getUnqual(ctx_), forkw));
    b_.CreateCall(proc_thread_done_fn(), { pcb_ });
}

// Publish this activation on its thread's activation stack (LRM 9.7). The head is
// the frame the thread is actually parked in, which is what a `kill` has to claim
// -- so a thread parked behind a time-consuming task call or a fork is reachable,
// the case remote `disable` still refuses.
//
// The node is `disable_node_ty`: the same six words, published for the same
// purpose (handle, the frame slot naming the live park record, the scheduled
// flag), so one force-wake protocol serves both.
void Backend::emit_pcb_acts_link() {
    if (pcb_ == nullptr || hdl_ == nullptr) {
        return;
    }
    auto* nty  = disable_node_ty();
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
    acts_node_ = ab.CreateAlloca(nty, nullptr, "acts_node");

    b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(nty, acts_node_, 2));
    b_.CreateStore(awaiter(), b_.CreateStructGEP(nty, acts_node_, 3));
    b_.CreateStore(sched_ptr_, b_.CreateStructGEP(nty, acts_node_, 4));
    b_.CreateStore(b_.getInt32(0), b_.CreateStructGEP(nty, acts_node_, 5));

    llvm::Value* headp = pcb_field(pcb_, kPcbActs);
    llvm::Value* old   = b_.CreateLoad(ptr, headp);
    b_.CreateStore(null, b_.CreateStructGEP(nty, acts_node_, 1));
    b_.CreateStore(old, b_.CreateStructGEP(nty, acts_node_, 0));
    auto* fix  = llvm::BasicBlock::Create(ctx_, "acts.link.fix", fn);
    auto* tail = llvm::BasicBlock::Create(ctx_, "acts.link.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(old, null), fix, tail);
    b_.SetInsertPoint(fix);
    b_.CreateStore(acts_node_, b_.CreateStructGEP(nty, old, 1));
    b_.CreateBr(tail);
    b_.SetInsertPoint(tail);
    b_.CreateStore(acts_node_, headp);
}

// Pop this activation off its thread's stack, at the frame's own exit.
void Backend::emit_pcb_acts_unlink() {
    if (pcb_ == nullptr || acts_node_ == nullptr) {
        return;
    }
    auto* nty  = disable_node_ty();
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* prev = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, acts_node_, 1));
    llvm::Value* next = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, acts_node_, 0));
    auto* has_prev = llvm::BasicBlock::Create(ctx_, "acts.unlink.prev", fn);
    auto* no_prev  = llvm::BasicBlock::Create(ctx_, "acts.unlink.head", fn);
    auto* mid      = llvm::BasicBlock::Create(ctx_, "acts.unlink.mid", fn);
    auto* has_next = llvm::BasicBlock::Create(ctx_, "acts.unlink.next", fn);
    auto* tail     = llvm::BasicBlock::Create(ctx_, "acts.unlink.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(prev, null), has_prev, no_prev);
    b_.SetInsertPoint(has_prev);
    b_.CreateStore(next, b_.CreateStructGEP(nty, prev, 0));
    b_.CreateBr(mid);
    b_.SetInsertPoint(no_prev);
    b_.CreateStore(next, pcb_field(pcb_, kPcbActs));
    b_.CreateBr(mid);
    b_.SetInsertPoint(mid);
    b_.CreateCondBr(b_.CreateICmpNE(next, null), has_next, tail);
    b_.SetInsertPoint(has_next);
    b_.CreateStore(prev, b_.CreateStructGEP(nty, next, 1));
    b_.CreateBr(tail);
    b_.SetInsertPoint(tail);
}

// The block this body leaves through when its thread is killed: a process body
// takes the cooperative-$finish path to its FINAL suspend (deinit, then the
// resumer's coro.done destroys the frame exactly once -- branching to
// coro.cleanup instead would free the frame under the drain), a task or
// fork-child activation takes its ordinary epilogue, which wakes its caller so
// the caller polls and aborts in turn. Null in a function, which cannot park and
// so has no poll point.
llvm::BasicBlock* Backend::proc_abort_exit() const {
    if (cur_is_task_) {
        return task_epilogue_bb_;
    }
    if (hdl_ == nullptr) {
        return nullptr;
    }
    // A program `initial` aborted while parked has "reached its end" for LRM
    // 24.3: the clause counts procedures that have ended, not ones that ended
    // normally, and an `initial` killed by `disable` or `process::kill` is never
    // going to end any other way. So it leaves through program.end, which counts
    // it down and terminates its program's descendants, and falls into the
    // ordinary final suspend from there. Null for every other body -- and for a
    // $finish, which branches straight to coro.final (emit_finish_exit) because
    // it has already ended the simulation and must not be counted twice.
    return program_end_bb_ != nullptr ? program_end_bb_ : coro_final_bb_;
}

// Back from a park: if `kill` (or `disable fork`) landed on this thread while it
// was suspended, leave now. Emitted after any op that can park, and only in a
// body that has a thread and an exit to take.
void Backend::emit_proc_abort_poll() {
    llvm::BasicBlock* exit = proc_abort_exit();
    if (pcb_ == nullptr || exit == nullptr) {
        return;
    }
    auto*        fn   = b_.GetInsertBlock()->getParent();
    llvm::Value* ctl  = b_.CreateLoad(b_.getInt32Ty(), pcb_field(pcb_, kPcbCtl));
    llvm::Value* kill = b_.CreateAnd(ctl, b_.getInt32(kProcAbort));
    auto*        cont = llvm::BasicBlock::Create(ctx_, "proc.alive", fn);
    b_.CreateCondBr(b_.CreateICmpNE(kill, b_.getInt32(0)), exit, cont);
    b_.SetInsertPoint(cont);
    // Suspended while we were parked (LRM 9.7): park again, remembering that the
    // wake already arrived, until `resume`.
    emit_proc_park_while_suspended();
}

// `p.kill()` (LRM 9.7) and `disable fork` (LRM 9.6.3). Mark the subtree, then
// force-wake every marked thread exactly once so each aborts at its own poll
// point; nothing is destroyed from outside. Killing the calling thread is the one
// activation no wake can reach -- it is running, not parked -- so it takes its
// abort exit directly, exactly as a self-`disable` does.
//
// `branch_on_self` is false for exactly one caller, `$exit` (LRM 24.7,
// lower_program_exit): it kills SEVERAL roots in a row, and the self-branch would
// leave the walks after the matching one unreachable. It ends the calling thread
// with one unconditional branch once every root is marked instead.
void Backend::lower_proc_kill(llvm::Value* target, bool with_self, bool branch_on_self) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = disable_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* chain =
      b_.CreateCall(proc_mark_kill_fn(), { target, b_.getInt32(with_self ? 1 : 0) });

    auto* walk = llvm::BasicBlock::Create(ctx_, "kill.walk", fn);
    auto* body = llvm::BasicBlock::Create(ctx_, "kill.node", fn);
    auto* wake = llvm::BasicBlock::Create(ctx_, "kill.wake", fn);
    auto* step = llvm::BasicBlock::Create(ctx_, "kill.next", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "kill.done", fn);

    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(walk);

    b_.SetInsertPoint(walk);
    llvm::PHINode* p = b_.CreatePHI(ptr, 2);
    p->addIncoming(chain, entry);
    b_.CreateCondBr(b_.CreateICmpNE(p, null), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* next = b_.CreateLoad(ptr, pcb_field(p, kPcbNextMarked));
    llvm::Value* acts = b_.CreateLoad(ptr, pcb_field(p, kPcbActs));
    // A thread with no live activation has nothing to wake: it is either this one
    // (handled below) or one that never entered a frame.
    llvm::Value* mine = pcb_ != nullptr ? b_.CreateICmpEQ(p, pcb_) : b_.getFalse();
    b_.CreateCondBr(b_.CreateOr(b_.CreateICmpEQ(acts, null), mine), step, wake);

    b_.SetInsertPoint(wake);
    llvm::Value* hdl = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, acts, 2));
    emit_node_force_wake(acts, hdl, step);

    b_.SetInsertPoint(step);
    b_.CreateBr(walk);
    p->addIncoming(next, step);

    b_.SetInsertPoint(done);
    // Did this kill name the calling thread? Then abort here: nothing else will
    // resume us to notice the mark.
    if (with_self && branch_on_self && pcb_ != nullptr && proc_abort_exit() != nullptr) {
        auto* cont = llvm::BasicBlock::Create(ctx_, "kill.other", fn);
        b_.CreateCondBr(b_.CreateICmpEQ(target, pcb_), proc_abort_exit(), cont);
        b_.SetInsertPoint(cont);
    }
}

// A frame-resident `await` waiter node, hoisted with the other frame allocas so
// CoroSplit spills it: one per await site, reused across iterations of the retry
// loop below (only one park is live at a time).
llvm::Value* Backend::proc_wait_node() {
    llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
    return ab.CreateAlloca(event_node_ty(), nullptr, "await_node");
}

llvm::FunctionCallee Backend::proc_await_self_fn() {
    return rt_fn("rt_proc_await_self", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() });
}

// `process::self()`, `p.status()`, `p.get_randstate()` (LRM 9.7).
void Backend::lower_proc_query(Index i) {
    const auto  info = ir_->extra_data<ProcCtlInfo>(ir_->payload(i).extra);
    const auto  args = ir_->region(info.args);
    auto*       i32  = b_.getInt32Ty();
    switch (info.op) {
        case ProcOp::Self: {
            if (pcb_ == nullptr) {
                // No calling thread: a DPI export thunk or the $monitor ramp. The
                // gate is on (this op set it), so the only way here is one of
                // those, and there is no honest answer to give.
                throw BackendError{ "internal: process::self() with no calling thread" };
            }
            set_value(i, two_val(emit_proc_object(pcb_, info.type)));
            return;
        }
        case ProcOp::Status: {
            llvm::Value* pcb = proc_pcb_of(must_of(value_of(args[0])), i);
            llvm::Value* ctl = b_.CreateLoad(i32, pcb_field(pcb, kPcbCtl));
            set_value(i, two_val(b_.CreateAnd(ctl, b_.getInt32(kProcStateMask))));
            return;
        }
        case ProcOp::RandstateGet: {
            llvm::Value* rng = proc_rng_of(proc_pcb_of(must_of(value_of(args[0])), i));
            b_.CreateCall(proc_randstate_get_fn(),
                          { str_of(i), rng, alloc_site_global(loc_text(i), "string") });
            return;
        }
        default:
            throw BackendError{ "internal: non-query op on a proc_query" };
    }
}

// Park while this thread is SUSPENDED (LRM 9.7). Two callers: the poll after every
// park -- a thread suspended while it was blocked notices here -- and a
// self-suspend, which parks immediately.
//
// Nothing is desensitised: the wheel cannot cancel and a static wait bucket cannot
// be compacted, so the source the thread was parked on stays armed. When it fires
// the thread is woken, lands here, records WAKE_PENDING and parks again with no
// source of its own; `resume` then pushes it and it continues in the current time
// step, which is exactly what LRM 9.7 asks for ("if the wait condition is now true
// or the original delay has transpired ... scheduled onto the Active or Reactive
// region"). If the source has NOT fired, the registration is untouched and the
// re-sensitisation the same sentence asks for is free.
void Backend::emit_proc_park_while_suspended() {
    if (pcb_ == nullptr || hdl_ == nullptr) {
        return;
    }
    auto* i32  = b_.getInt32Ty();
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* test  = llvm::BasicBlock::Create(ctx_, "susp.test", fn);
    auto* park  = llvm::BasicBlock::Create(ctx_, "susp.park", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "susp.done", fn);
    b_.CreateBr(test);

    b_.SetInsertPoint(test);
    llvm::Value* ctlp = pcb_field(pcb_, kPcbCtl);
    llvm::Value* ctl  = b_.CreateLoad(i32, ctlp);
    llvm::Value* susp = b_.CreateICmpEQ(b_.CreateAnd(ctl, b_.getInt32(kProcStateMask)),
                                        b_.getInt32(kProcSuspended));
    b_.CreateCondBr(susp, park, done);

    b_.SetInsertPoint(park);
    // Record that the wake we were parked for has already been delivered, so
    // `resume` schedules instead of waiting for a source that will not fire twice.
    b_.CreateStore(b_.CreateOr(b_.CreateLoad(i32, ctlp), b_.getInt32(kProcWakePend)), ctlp);
    emit_suspend("susp.resume");
    b_.CreateBr(test);

    b_.SetInsertPoint(done);
}

// `p.suspend()` (LRM 9.7). On another thread this is one store: it stays parked
// where it is, and its poll turns the next wake into a re-park. On THIS thread the
// suspension "takes place immediately", so the park happens here.
void Backend::emit_proc_suspend(llvm::Value* pcb) {
    auto* i32  = b_.getInt32Ty();
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* set  = llvm::BasicBlock::Create(ctx_, "susp.set", fn);
    auto* skip = llvm::BasicBlock::Create(ctx_, "susp.skip", fn);

    llvm::Value* ctlp  = pcb_field(pcb, kPcbCtl);
    llvm::Value* ctl   = b_.CreateLoad(i32, ctlp);
    llvm::Value* state = b_.CreateAnd(ctl, b_.getInt32(kProcStateMask));
    // A completed thread cannot be suspended: LRM 9.7 has no transition out of
    // FINISHED or KILLED.
    llvm::Value* over = b_.CreateOr(b_.CreateICmpEQ(state, b_.getInt32(kProcFinished)),
                                    b_.CreateICmpEQ(state, b_.getInt32(kProcKilled)));
    b_.CreateCondBr(over, skip, set);

    b_.SetInsertPoint(set);
    llvm::Value* self = pcb_ != nullptr ? b_.CreateICmpEQ(pcb, pcb_) : b_.getFalse();
    llvm::Value* mask = b_.CreateOr(b_.CreateAnd(ctl, b_.getInt32(~kProcStateMask)),
                                    b_.getInt32(kProcSuspended));
    // A self-suspend also records that no source can wake it.
    b_.CreateStore(b_.CreateOr(mask, b_.CreateSelect(self, b_.getInt32(kProcSelfPark),
                                                     b_.getInt32(0))),
                   ctlp);
    if (pcb_ != nullptr && hdl_ != nullptr) {
        auto* mine = llvm::BasicBlock::Create(ctx_, "susp.self", fn);
        b_.CreateCondBr(self, mine, skip);
        b_.SetInsertPoint(mine);
        emit_proc_park_while_suspended();
        b_.CreateBr(skip);
    } else {
        b_.CreateBr(skip);
    }
    b_.SetInsertPoint(skip);
}

// `p.resume()` (LRM 9.7). A no-op unless the thread is SUSPENDED; otherwise clear
// the state and, if the wake it was parked for already arrived (or it suspended
// itself, so no wake ever will), push it now. The push is unconditional rather
// than scheduled-flag-gated: a thread parked in the suspension loop is in no ring,
// and the flag is typically already set by whatever scan woke it into that loop.
void Backend::emit_proc_resume(llvm::Value* pcb) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* nty  = disable_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    auto* go   = llvm::BasicBlock::Create(ctx_, "resume.go", fn);
    auto* push = llvm::BasicBlock::Create(ctx_, "resume.push", fn);
    auto* skip = llvm::BasicBlock::Create(ctx_, "resume.skip", fn);

    llvm::Value* ctlp = pcb_field(pcb, kPcbCtl);
    llvm::Value* ctl  = b_.CreateLoad(i32, ctlp);
    b_.CreateCondBr(b_.CreateICmpEQ(b_.CreateAnd(ctl, b_.getInt32(kProcStateMask)),
                                    b_.getInt32(kProcSuspended)),
                    go, skip);

    b_.SetInsertPoint(go);
    llvm::Value* pend = b_.CreateAnd(ctl, b_.getInt32(kProcWakePend | kProcSelfPark));
    b_.CreateStore(b_.CreateOr(b_.CreateAnd(ctl, b_.getInt32(~(kProcStateMask | kProcWakePend |
                                                              kProcSelfPark))),
                               b_.getInt32(kProcRunning)),
                   ctlp);
    llvm::Value* acts = b_.CreateLoad(ptr, pcb_field(pcb, kPcbActs));
    b_.CreateCondBr(b_.CreateAnd(b_.CreateICmpNE(pend, b_.getInt32(0)),
                                 b_.CreateICmpNE(acts, null)),
                    push, skip);

    b_.SetInsertPoint(push);
    llvm::Value* hdl = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, acts, 2));
    emit_wake_push(b_, hdl);
    b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), b_.CreateLoad(ptr, b_.CreateStructGEP(nty, acts, 4)));
    b_.CreateBr(skip);

    b_.SetInsertPoint(skip);
}

// `wait fork` (LRM 9.6.1): park until every descendant of this thread has
// completed. The barrier is one pointer test -- a completing thread re-parents its
// live children, so an empty child list means the whole subtree is done -- and the
// wake comes from the last descendant's completion, which takes this thread's
// waiter list when it sees the WAIT_FORK flag.
void Backend::lower_wait_fork() {
    if (pcb_ == nullptr || hdl_ == nullptr) {
        throw BackendError{ "internal: `wait fork` with no calling thread" };
    }
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* nty  = event_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* node = proc_wait_node();
    auto*        test = llvm::BasicBlock::Create(ctx_, "waitfork.test", fn);
    auto*        park = llvm::BasicBlock::Create(ctx_, "waitfork.park", fn);
    auto*        done = llvm::BasicBlock::Create(ctx_, "waitfork.done", fn);
    b_.CreateBr(test);

    b_.SetInsertPoint(test);
    llvm::Value* kid = b_.CreateLoad(ptr, pcb_field(pcb_, kPcbFirstChild));
    b_.CreateCondBr(b_.CreateICmpEQ(kid, null), done, park);

    b_.SetInsertPoint(park);
    llvm::Value* ctlp = pcb_field(pcb_, kPcbCtl);
    b_.CreateStore(b_.CreateOr(b_.CreateLoad(i32, ctlp), b_.getInt32(kProcWaitFork)), ctlp);
    llvm::Value* headp = pcb_field(pcb_, kPcbWaiters);
    llvm::Value* old   = b_.CreateLoad(ptr, headp);
    b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(nty, node, kNodeHandle));
    b_.CreateStore(sched_ptr_, b_.CreateStructGEP(nty, node, kNodeFlag));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kNodePrev));
    b_.CreateStore(old, b_.CreateStructGEP(nty, node, kNodeNext));
    b_.CreateStore(pcb_, b_.CreateStructGEP(nty, node, kNodeOwner));
    auto* fix   = llvm::BasicBlock::Create(ctx_, "waitfork.link.fix", fn);
    auto* armed = llvm::BasicBlock::Create(ctx_, "waitfork.armed", fn);
    b_.CreateCondBr(b_.CreateICmpNE(old, null), fix, armed);
    b_.SetInsertPoint(fix);
    b_.CreateStore(node, b_.CreateStructGEP(nty, old, kNodePrev));
    b_.CreateBr(armed);
    b_.SetInsertPoint(armed);
    b_.CreateStore(node, headp);
    b_.CreateStore(b_.getInt64(0), sched_ptr_);
    emit_suspend("waitfork.resume");
    emit_proc_unlink_node(node);
    // Killed while parked (LRM 9.7): leave the barrier now. Re-testing instead
    // would re-park on a still-live child list and the force-wake would be lost;
    // lower_region's poll, right after this instruction, takes the real exit.
    emit_wait_abort_exit(done);
    b_.CreateBr(test);

    b_.SetInsertPoint(done);
}

// Record this thread's generator state on its control block, so `p.srandom()` and
// the randstate pair can reach it through a handle (LRM 9.7, 18.14).
void Backend::emit_proc_publish_rng() {
    if (pcb_ == nullptr || rng_slot_ == nullptr) {
        return;
    }
    b_.CreateStore(rng_slot_, pcb_field(pcb_, kPcbRng));
}

llvm::FunctionCallee Backend::proc_randstate_get_fn() {
    return rt_fn("rt_proc_randstate_get", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() });
}

llvm::FunctionCallee Backend::proc_randstate_set_fn() {
    return rt_fn("rt_proc_randstate_set", b_.getVoidTy(),
                 { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
}

// The generator state of the thread `pcb` names. Non-null in a design that uses
// the randstate ops at all, because build_rng_fns then gives every thread its own.
llvm::Value* Backend::proc_rng_of(llvm::Value* pcb) {
    return b_.CreateLoad(llvm::PointerType::getUnqual(ctx_), pcb_field(pcb, kPcbRng));
}

// `p.kill()`, `disable fork` (LRM 9.7, 9.6.3). The other control ops are still
// refused in the frontend.
void Backend::lower_proc_ctl(Index i) {
    const auto info = ir_->extra_data<ProcCtlInfo>(ir_->payload(i).extra);
    const auto args = ir_->region(info.args);
    switch (info.op) {
        case ProcOp::Kill:
            lower_proc_kill(proc_pcb_of(must_of(value_of(args[0])), i), /*with_self=*/true);
            return;
        case ProcOp::DisableFork:
            if (pcb_ == nullptr) {
                throw BackendError{ "internal: `disable fork` with no calling thread" };
            }
            // Descendants only: 9.6.3 kills what the caller forked, not the caller.
            lower_proc_kill(pcb_, /*with_self=*/false);
            return;
        case ProcOp::Suspend:
            emit_proc_suspend(proc_pcb_of(must_of(value_of(args[0])), i));
            return;
        case ProcOp::Resume:
            emit_proc_resume(proc_pcb_of(must_of(value_of(args[0])), i));
            return;
        case ProcOp::Srandom: {
            // LRM 18.14: seed that thread's generator. Same splitmix64 expansion
            // `$urandom(seed)` uses, so a seed means the same thing either way.
            llvm::Value* rng = proc_rng_of(proc_pcb_of(must_of(value_of(args[0])), i));
            b_.CreateCall(urandom_seed_fn(),
                          { rng, b_.CreateZExt(cast(must_of(value_of(args[1])), 32, false),
                                               b_.getInt64Ty()) });
            return;
        }
        case ProcOp::RandstateSet: {
            llvm::Value* rng = proc_rng_of(proc_pcb_of(must_of(value_of(args[0])), i));
            llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel_ });
            b_.CreateCall(proc_randstate_set_fn(),
                          { rng, str_of(args[1]),
                            null_site_global(loc_text(i), NullKind::Class), now });
            return;
        }
        default:
            throw BackendError{ "internal: unimplemented process control op" };
    }
}

// `p.await()` (LRM 9.7): park until the target thread completes. A loop, not a
// single park, so a spurious wake (this thread is on other queues too) re-tests
// rather than falling through -- and so a target that is already FINISHED costs
// no suspend at all.
void Backend::lower_proc_wait(Index i) {
    const auto info = ir_->extra_data<ProcCtlInfo>(ir_->payload(i).extra);
    const auto args = ir_->region(info.args);
    if (info.op == ProcOp::WaitFork) {
        lower_wait_fork();
        return;
    }
    if (info.op != ProcOp::Await) {
        throw BackendError{ "internal: unimplemented process wait op" };
    }
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* i32  = b_.getInt32Ty();
    auto* nty  = event_node_ty();
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* null = llvm::ConstantPointerNull::get(ptr);

    llvm::Value* pcb = proc_pcb_of(must_of(value_of(args[0])), i);
    // LRM 9.7: "it shall be an error to call this task on the current process".
    // Dynamic, because the handle is a value -- `process::self().await()` and a
    // handle that happens to name this thread are the same case.
    if (pcb_ != nullptr) {
        auto* bad  = llvm::BasicBlock::Create(ctx_, "await.self", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "await.other", fn);
        b_.CreateCondBr(b_.CreateICmpEQ(pcb, pcb_), bad, cont);
        b_.SetInsertPoint(bad);
        llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel_ });
        auto* call = b_.CreateCall(proc_await_self_fn(),
                                   { null_site_global(loc_text(i), NullKind::Class), now });
        call->addFnAttr(llvm::Attribute::NoReturn);
        call->addFnAttr(llvm::Attribute::Cold);
        b_.CreateUnreachable();
        b_.SetInsertPoint(cont);
    }

    llvm::Value* node = proc_wait_node();
    auto*        test = llvm::BasicBlock::Create(ctx_, "await.test", fn);
    auto*        park = llvm::BasicBlock::Create(ctx_, "await.park", fn);
    auto*        done = llvm::BasicBlock::Create(ctx_, "await.over", fn);
    b_.CreateBr(test);

    b_.SetInsertPoint(test);
    llvm::Value* state = b_.CreateAnd(b_.CreateLoad(i32, pcb_field(pcb, kPcbCtl)),
                                      b_.getInt32(kProcStateMask));
    // FINISHED (0) or KILLED (4) both mean "completed" for await's purposes.
    llvm::Value* over = b_.CreateOr(b_.CreateICmpEQ(state, b_.getInt32(kProcFinished)),
                                    b_.CreateICmpEQ(state, b_.getInt32(kProcKilled)));
    b_.CreateCondBr(over, done, park);

    b_.SetInsertPoint(park);
    // Push-front onto the target's waiter list, recording the region-tagged
    // handle and this thread's scheduled flag -- the same two words a wait bucket
    // entry holds, read back by emit_proc_wake_waiters.
    llvm::Value* headp = pcb_field(pcb, kPcbWaiters);
    llvm::Value* old   = b_.CreateLoad(ptr, headp);
    b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(nty, node, kNodeHandle));
    b_.CreateStore(sched_ptr_, b_.CreateStructGEP(nty, node, kNodeFlag));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kNodePrev));
    b_.CreateStore(old, b_.CreateStructGEP(nty, node, kNodeNext));
    b_.CreateStore(pcb, b_.CreateStructGEP(nty, node, kNodeOwner));
    auto* fixup = llvm::BasicBlock::Create(ctx_, "await.link.fix", fn);
    auto* armed = llvm::BasicBlock::Create(ctx_, "await.armed", fn);
    b_.CreateCondBr(b_.CreateICmpNE(old, null), fixup, armed);
    b_.SetInsertPoint(fixup);
    b_.CreateStore(node, b_.CreateStructGEP(nty, old, kNodePrev));
    b_.CreateBr(armed);
    b_.SetInsertPoint(armed);
    b_.CreateStore(node, headp);
    // Armed for the reason `wait` and `expect` arm: parked here this thread sits
    // in no other queue, so the flag has to read "not scheduled" or the wake is
    // dropped as a duplicate.
    b_.CreateStore(b_.getInt64(0), sched_ptr_);
    emit_suspend("await.resume");
    // Unlink on resume, whoever woke us: the waker only walks and pushes, it does
    // not unlink, which is what keeps a multi-waiter list correct without a
    // compaction pass.
    emit_proc_unlink_node(node);
    // Killed while awaiting: same reasoning as `wait fork` -- the target has not
    // completed, so a re-test would re-park and the abort would never be seen.
    emit_wait_abort_exit(done);
    b_.CreateBr(test);

    b_.SetInsertPoint(done);
}

// Take `node` off whatever waiter list it went onto (its recorded owner), and
// clear its links so a second unlink is a no-op.
void Backend::emit_proc_unlink_node(llvm::Value* node) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = event_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* owner = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kNodeOwner));
    llvm::Value* prev  = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kNodePrev));
    llvm::Value* next  = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kNodeNext));

    auto* has_prev = llvm::BasicBlock::Create(ctx_, "await.unlink.prev", fn);
    auto* no_prev  = llvm::BasicBlock::Create(ctx_, "await.unlink.head", fn);
    auto* mid      = llvm::BasicBlock::Create(ctx_, "await.unlink.mid", fn);
    auto* has_next = llvm::BasicBlock::Create(ctx_, "await.unlink.next", fn);
    auto* tail     = llvm::BasicBlock::Create(ctx_, "await.unlink.done", fn);

    b_.CreateCondBr(b_.CreateICmpNE(prev, null), has_prev, no_prev);
    b_.SetInsertPoint(has_prev);
    b_.CreateStore(next, b_.CreateStructGEP(nty, prev, kNodeNext));
    b_.CreateBr(mid);
    b_.SetInsertPoint(no_prev);
    b_.CreateStore(next, pcb_field(owner, kPcbWaiters));
    b_.CreateBr(mid);
    b_.SetInsertPoint(mid);
    b_.CreateCondBr(b_.CreateICmpNE(next, null), has_next, tail);
    b_.SetInsertPoint(has_next);
    b_.CreateStore(prev, b_.CreateStructGEP(nty, next, kNodePrev));
    b_.CreateBr(tail);
    b_.SetInsertPoint(tail);
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kNodePrev));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kNodeNext));
}

// -- Wait queue of a built-in synchronization object (LRM 15.3, 15.4) --
//
// A `semaphore` / `mailbox` object is an ordinary class object whose synthetic
// body carries the resource plus one FIFO wait queue per resource, each two
// words: a head pointer and a tail pointer, held as the 2-state 64-bit
// (chandle-shaped) members the frontend selects and hands over as lvalues. So
// nothing here knows the object's layout, and the collector never sees a
// coroutine frame pointer (a `chandle` member carries no class handle).
//
// The node is the event waiter node plus the two queue words it is linked into,
// which is what lets `Leave` repair the tail without assuming the head and tail
// members are adjacent in the body. See docs/design/mailbox-and-semaphore.md
// §5.
llvm::StructType* Backend::sync_node_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(ctx_, { ptr, ptr, ptr, ptr, ptr, ptr });
}

// The frame-resident node of one wait SITE, hoisted to the ramp so CoroSplit
// spills it into the coroutine frame -- event_wait_node's placement, and for
// the same reason: the object holds a pointer to it while this thread is
// parked. Zeroed there too, because the enqueue guard READS `q_head` before
// anything has written it.
llvm::Value* Backend::sync_wait_node(uint32_t site) {
    auto [it, fresh] = sync_nodes_.try_emplace(site, nullptr);
    if (fresh || it->second == nullptr ||
        llvm::cast<llvm::Instruction>(it->second)->getFunction() != b_.GetInsertBlock()->getParent()) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        auto*             node = ab.CreateAlloca(sync_node_ty(), nullptr, "sync_node");
        ab.CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)),
                       ab.CreateStructGEP(sync_node_ty(), node, kSyncQHead));
        it->second = node;
    }
    return it->second;
}

// The address of one queue word, from the frontend's member lvalue. The word is
// a 2-state 64-bit member, so a node pointer rides it as an integer -- the same
// shape the `process` object's `chandle` field uses for its control block.
llvm::Value* Backend::sync_word_ptr(Index lv) {
    return lv_handle_ptr(lvalue_of(lv), HandleUse::Place);
}

llvm::Value* Backend::sync_word_load(llvm::Value* word) {
    return b_.CreateIntToPtr(b_.CreateLoad(b_.getInt64Ty(), word), llvm::PointerType::getUnqual(ctx_));
}

void Backend::sync_word_store(llvm::Value* word, llvm::Value* node) {
    b_.CreateStore(b_.CreatePtrToInt(node, b_.getInt64Ty()), word);
}

// Link `node` at the TAIL, so the queue is FIFO (LRM 15.3.3 / 15.4.5: arrival
// order is preserved). A node already on a queue is left where it is: that is
// the whole point of the guard, since a woken waiter whose resource test fails
// must not go to the back of the line.
void Backend::emit_sync_enqueue(llvm::Value* node, llvm::Value* qhead, llvm::Value* qtail) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = sync_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    auto*        link = llvm::BasicBlock::Create(ctx_, "sync.link", fn);
    auto*        done = llvm::BasicBlock::Create(ctx_, "sync.linked", fn);
    llvm::Value* onq  = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kSyncQHead));
    b_.CreateCondBr(b_.CreateICmpEQ(onq, null), link, done);

    b_.SetInsertPoint(link);
    b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(nty, node, kSyncHandle));
    b_.CreateStore(sched_ptr_, b_.CreateStructGEP(nty, node, kSyncFlag));
    b_.CreateStore(qhead, b_.CreateStructGEP(nty, node, kSyncQHead));
    b_.CreateStore(qtail, b_.CreateStructGEP(nty, node, kSyncQTail));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kSyncNext));
    llvm::Value* tail = sync_word_load(qtail);
    b_.CreateStore(tail, b_.CreateStructGEP(nty, node, kSyncPrev));
    auto* append = llvm::BasicBlock::Create(ctx_, "sync.append", fn);
    auto* first  = llvm::BasicBlock::Create(ctx_, "sync.first", fn);
    auto* settle = llvm::BasicBlock::Create(ctx_, "sync.settle", fn);
    b_.CreateCondBr(b_.CreateICmpNE(tail, null), append, first);
    b_.SetInsertPoint(append);
    b_.CreateStore(node, b_.CreateStructGEP(nty, tail, kSyncNext));
    b_.CreateBr(settle);
    b_.SetInsertPoint(first);
    sync_word_store(qhead, node);
    b_.CreateBr(settle);
    b_.SetInsertPoint(settle);
    // Either way this node is now last.
    sync_word_store(qtail, node);
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
}

// Take `node` off its queue and clear the links, so a second unlink is a no-op
// and the enqueue guard reads "not linked" again.
void Backend::emit_sync_unlink(llvm::Value* node) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = sync_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* qhead = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kSyncQHead));
    auto*        work  = llvm::BasicBlock::Create(ctx_, "sync.unlink", fn);
    auto*        skip  = llvm::BasicBlock::Create(ctx_, "sync.unlinked", fn);
    b_.CreateCondBr(b_.CreateICmpNE(qhead, null), work, skip);

    b_.SetInsertPoint(work);
    llvm::Value* qtail = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kSyncQTail));
    llvm::Value* prev  = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kSyncPrev));
    llvm::Value* next  = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, kSyncNext));

    auto* has_prev = llvm::BasicBlock::Create(ctx_, "sync.un.prev", fn);
    auto* no_prev  = llvm::BasicBlock::Create(ctx_, "sync.un.head", fn);
    auto* mid      = llvm::BasicBlock::Create(ctx_, "sync.un.mid", fn);
    b_.CreateCondBr(b_.CreateICmpNE(prev, null), has_prev, no_prev);
    b_.SetInsertPoint(has_prev);
    b_.CreateStore(next, b_.CreateStructGEP(nty, prev, kSyncNext));
    b_.CreateBr(mid);
    b_.SetInsertPoint(no_prev);
    sync_word_store(qhead, next);
    b_.CreateBr(mid);

    b_.SetInsertPoint(mid);
    auto* has_next = llvm::BasicBlock::Create(ctx_, "sync.un.next", fn);
    auto* no_next  = llvm::BasicBlock::Create(ctx_, "sync.un.tail", fn);
    auto* tail     = llvm::BasicBlock::Create(ctx_, "sync.un.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(next, null), has_next, no_next);
    b_.SetInsertPoint(has_next);
    b_.CreateStore(prev, b_.CreateStructGEP(nty, next, kSyncPrev));
    b_.CreateBr(tail);
    b_.SetInsertPoint(no_next);
    sync_word_store(qtail, prev);
    b_.CreateBr(tail);

    b_.SetInsertPoint(tail);
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kSyncPrev));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kSyncNext));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kSyncQHead));
    b_.CreateBr(skip);
    b_.SetInsertPoint(skip);
}

// Wake the queue's head, or every waiter on it. Each woken thread re-tests and
// either takes the resource or parks again on the node it never left, so a
// spurious wake costs one activation and no queue surgery. `next` is read
// before the wake for the reason emit_event_wake_list reads it first: the woken
// activation may unlink its own node as soon as it runs.
void Backend::emit_sync_wake(llvm::Value* qhead, bool all) {
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* nty  = sync_node_ty();
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    // One waiter: read the head, wake it if its flag is clear. No loop, so a
    // hand-off costs a load and a compare.
    const auto wake_one = [&](llvm::Value* n) {
        auto* push = llvm::BasicBlock::Create(ctx_, "sync.wake.push", fn);
        auto* skip = llvm::BasicBlock::Create(ctx_, "sync.wake.skip", fn);
        llvm::Value* hdl  = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, n, kSyncHandle));
        llvm::Value* flag = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, n, kSyncFlag));
        // One wake per thread per instant, the design's existing scheduled-flag
        // token: a thread already queued elsewhere is not pushed twice.
        b_.CreateCondBr(b_.CreateICmpEQ(b_.CreateLoad(sched_word_ty(), flag), b_.getInt64(0)), push,
                        skip);
        b_.SetInsertPoint(push);
        emit_wake_push(b_, hdl);
        b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), flag);
        b_.CreateBr(skip);
        b_.SetInsertPoint(skip);
    };

    if (!all) {
        auto*        one  = llvm::BasicBlock::Create(ctx_, "sync.wake.one", fn);
        auto*        done = llvm::BasicBlock::Create(ctx_, "sync.wake.done", fn);
        llvm::Value* head = sync_word_load(qhead);
        b_.CreateCondBr(b_.CreateICmpNE(head, null), one, done);
        b_.SetInsertPoint(one);
        wake_one(head);
        b_.CreateBr(done);
        b_.SetInsertPoint(done);
        return;
    }

    auto*        walk  = llvm::BasicBlock::Create(ctx_, "sync.wake.walk", fn);
    auto*        body  = llvm::BasicBlock::Create(ctx_, "sync.wake.node", fn);
    auto*        done  = llvm::BasicBlock::Create(ctx_, "sync.wake.done", fn);
    llvm::Value* first = sync_word_load(qhead);
    auto*        pre   = b_.GetInsertBlock();
    b_.CreateBr(walk);

    b_.SetInsertPoint(walk);
    llvm::PHINode* n = b_.CreatePHI(ptr, 2, "n");
    n->addIncoming(first, pre);
    b_.CreateCondBr(b_.CreateICmpNE(n, null), body, done);

    b_.SetInsertPoint(body);
    // Read `next` BEFORE the wake: a woken activation may unlink its own node
    // as soon as it runs, exactly as emit_event_wake_list must.
    llvm::Value* nxt = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, n, kSyncNext));
    wake_one(n);
    n->addIncoming(nxt, b_.GetInsertBlock());
    b_.CreateBr(walk);

    b_.SetInsertPoint(done);
}

// `SyncWait`: park on a queue until something wakes us, and report whether we
// came back as its head. The abort test is HERE rather than left to
// lower_region's poll because the frontend's retest loop re-parks without
// reaching the end of the instruction -- the shape that made a killed `await`
// unkillable (docs/design/mailbox-and-semaphore.md §6.3).
void Backend::lower_sync_wait(Index i) {
    if (hdl_ == nullptr || sched_ptr_ == nullptr) {
        throw BackendError{ "internal: a blocking synchronization method with no thread" };
    }
    const auto info = ir_->extra_data<SyncInfo>(ir_->payload(i).extra);
    const auto args = ir_->region(info.args);
    if (args.size() != 2) {
        throw BackendError{ "internal: sync_wait takes a head and a tail word" };
    }
    llvm::Value* qhead = sync_word_ptr(args[0]);
    llvm::Value* qtail = sync_word_ptr(args[1]);
    llvm::Value* node  = sync_wait_node(info.site);

    emit_sync_enqueue(node, qhead, qtail);
    // Armed for the reason every intra-body park arms: parked here this thread
    // sits in no other queue, so the flag must read "not scheduled" or the wake
    // is dropped as a duplicate.
    b_.CreateStore(b_.getInt64(0), sched_ptr_);
    emit_suspend("coro.sync_resume");
    if (llvm::Value* aborting = emit_abort_pending(); aborting != nullptr) {
        auto* fn    = b_.GetInsertBlock()->getParent();
        auto* leave = llvm::BasicBlock::Create(ctx_, "sync.abort", fn);
        auto* alive = llvm::BasicBlock::Create(ctx_, "sync.alive", fn);
        b_.CreateCondBr(aborting, leave, alive);
        b_.SetInsertPoint(leave);
        // Off the queue before the poll takes this activation's exit: nothing
        // may be left pointing into a frame that is about to unwind.
        emit_sync_unlink(node);
        // And hand off, for the reason `Leave` hands off: an aborting thread may
        // be the HEAD, and the resource it was about to take is still there --
        // one `put` of several keys satisfies several waiters. Without this the
        // wake dies with the killed thread and every waiter behind it strands,
        // which a `put(2)` racing a `kill` of the head reproduces exactly.
        emit_sync_wake(qhead, /*all=*/false);
        b_.CreateBr(alive);
        b_.SetInsertPoint(alive);
    }
    llvm::Value* head = b_.CreateICmpEQ(sync_word_load(qhead), node);
    set_value(i, two_val(head));
}

// `SyncCtl`: leave a queue (and hand off to the new head), or wake one.
void Backend::lower_sync_ctl(Index i) {
    const auto info = ir_->extra_data<SyncInfo>(ir_->payload(i).extra);
    const auto args = ir_->region(info.args);
    if (args.size() != 2) {
        throw BackendError{ "internal: sync_ctl takes a head and a tail word" };
    }
    llvm::Value* qhead = sync_word_ptr(args[0]);
    switch (info.op) {
        case SyncOp::Leave:
            emit_sync_unlink(sync_wait_node(info.site));
            // A `put` of several keys can satisfy several waiters, and only the
            // thread leaving knows the queue moved.
            emit_sync_wake(qhead, /*all=*/false);
            return;
        case SyncOp::WakeHead:
            emit_sync_wake(qhead, /*all=*/false);
            return;
        case SyncOp::WakeAll:
            emit_sync_wake(qhead, /*all=*/true);
            return;
        case SyncOp::Park:
            throw BackendError{ "internal: Park is SyncWait's op, not SyncCtl's" };
    }
}

// The per-target tables the remote form needs, sized by the frontend's target
// count and emitted only for a design that has a remote `disable`. Flat arrays,
// not the per-instance anchored shape proc_sched uses: an id is allocated per
// targeted block symbol and slang gives every elaborated instance its own, so
// one id already names one block in one instance.
llvm::GlobalVariable* Backend::disable_table(llvm::GlobalVariable*& gv,
                                             const char*            name,
                                             llvm::Type*            elem,
                                             llvm::Constant*        init) {
    if (gv == nullptr) {
        const auto n  = static_cast<size_t>(mod_.disable_targets);
        auto*      ty = llvm::ArrayType::get(elem, n);
        // Defined in the root unit and referenced through sym(), like every
        // other cross-unit global. A group holding either half of the remote
        // form is forced to a real coroutine (is_simple_always), and only a
        // thunk group is ever shared across instances, so a baked per-instance
        // target id can never reach a shared body.
        gv = new llvm::GlobalVariable(root_env_->llmod, ty, false, llvm::GlobalValue::InternalLinkage,
                                      llvm::ConstantArray::get(ty, std::vector<llvm::Constant*>(n, init)),
                                      std::string("clotho.disable.") + name);
    }
    return gv;
}

llvm::Value* Backend::disable_slot(llvm::IRBuilder<>&     b,
                                   llvm::GlobalVariable*& gv,
                                   const char*            name,
                                   llvm::Type*            elem,
                                   llvm::Constant*        init,
                                   uint32_t               target) {
    auto* ty = llvm::ArrayType::get(elem, static_cast<size_t>(mod_.disable_targets));
    return b.CreateInBoundsGEP(ty, sym(disable_table(gv, name, elem, init)),
                               { b.getInt32(0), b.getInt32(target) });
}

// One live activation of a `disable`-targeted task (LRM 9.6.2), frame-resident
// and doubly linked so the epilogue can unlink in constant time:
//   { ptr next, ptr prev, ptr handle, ptr park_slot, ptr sched_flag, i32 abort }
// The list, not a single slot, is what "disabling a task shall disable all
// activations of the task" needs -- and it is the only thing a task target needs
// that a block target does not, because the caller resume it owes is the one the
// ordinary epilogue already does.
llvm::StructType* Backend::disable_node_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(ctx_, { ptr, ptr, ptr, ptr, ptr, b_.getInt32Ty() });
}

// Head of target `t`'s activation list.
llvm::Value* Backend::disable_head_ptr(llvm::IRBuilder<>& b, uint32_t t) {
    auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
    return disable_slot(b, disable_head_gv_, "head", ptr_ty, llvm::ConstantPointerNull::get(ptr_ty), t);
}

// Link this activation at the head of its target's list. Emitted in the task
// prologue, right after the frame exists.
void Backend::emit_disable_link(uint32_t target, llvm::Value* node) {
    auto* nty  = disable_node_ty();
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* head = disable_head_ptr(b_, target);
    llvm::Value* old  = b_.CreateLoad(ptr, head);
    b_.CreateStore(parked_handle(b_, hdl_), b_.CreateStructGEP(nty, node, 2));
    b_.CreateStore(awaiter(), b_.CreateStructGEP(nty, node, 3));
    b_.CreateStore(sched_ptr_, b_.CreateStructGEP(nty, node, 4));
    b_.CreateStore(b_.getInt32(0), b_.CreateStructGEP(nty, node, 5));
    b_.CreateStore(old, b_.CreateStructGEP(nty, node, 0));
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, 1));

    auto* has  = llvm::BasicBlock::Create(ctx_, "disable.link.prev", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "disable.link.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(old, null), has, done);
    b_.SetInsertPoint(has);
    b_.CreateStore(node, b_.CreateStructGEP(nty, old, 1));
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
    b_.CreateStore(node, head);
}

// Unlink it again. Emitted at the top of the epilogue, so an activation is off
// the list before it can be woken by a `disable` that arrives while it finishes.
void Backend::emit_disable_unlink(uint32_t target, llvm::Value* node) {
    auto* nty  = disable_node_ty();
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* null = llvm::ConstantPointerNull::get(ptr);
    auto* fn   = b_.GetInsertBlock()->getParent();

    llvm::Value* next = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, 0));
    llvm::Value* prev = b_.CreateLoad(ptr, b_.CreateStructGEP(nty, node, 1));

    auto* has_prev = llvm::BasicBlock::Create(ctx_, "disable.unlink.prev", fn);
    auto* no_prev  = llvm::BasicBlock::Create(ctx_, "disable.unlink.head", fn);
    auto* tail     = llvm::BasicBlock::Create(ctx_, "disable.unlink.next", fn);
    b_.CreateCondBr(b_.CreateICmpNE(prev, null), has_prev, no_prev);

    b_.SetInsertPoint(has_prev);
    b_.CreateStore(next, b_.CreateStructGEP(nty, prev, 0));
    b_.CreateBr(tail);

    b_.SetInsertPoint(no_prev);
    b_.CreateStore(next, disable_head_ptr(b_, target));
    b_.CreateBr(tail);

    b_.SetInsertPoint(tail);
    auto* has_next = llvm::BasicBlock::Create(ctx_, "disable.unlink.back", fn);
    auto* done     = llvm::BasicBlock::Create(ctx_, "disable.unlink.done", fn);
    b_.CreateCondBr(b_.CreateICmpNE(next, null), has_next, done);
    b_.SetInsertPoint(has_next);
    b_.CreateStore(prev, b_.CreateStructGEP(nty, next, 1));
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
}

} // namespace clotho::ir::backend
