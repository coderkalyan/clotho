// Scheduler kernel interface: region queues and runs, suspension, program end and main.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// The awaiter for a process #delay: an rt_timer ({ next, on_expire }) followed
// by the coroutine handle. &awaiter aliases the rt_timer the wheel links; the
// resume trampoline recovers the handle from field 2.
//
// { Timer(next, on_expire), parked handle } — and, when the design has a remote
// `disable`, two more fields that let the wheel and a `disable` compete for the
// same park without ever waking it twice: the frame slot naming the live record,
// and a `state` claim (0 = armed, 1 = claimed by whoever is waking us).
//
// Without a remote `disable` this is one record per process, reused across
// delays, because a process waits on at most one timer at a time. With one, a
// force-woken process can carry on while its old timer is still armed, so the
// records must not alias: each arm gets a fresh heap record, and the wheel frees
// it when it finally fires.
//
// The claim is what the scheduled flag cannot be: mid-body that flag is SET, on
// purpose, so the process's own sensitivity cannot re-wake it -- so a `disable`
// must not read it as "already queued", and must not clear it either.
llvm::StructType* Backend::awaiter_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    if (!park_claim_wanted()) {
        return llvm::StructType::get(ctx_, { ptr, ptr, ptr });
    }
    return llvm::StructType::get(ctx_, { ptr, ptr, ptr, ptr, b_.getInt32Ty() });
}

// Lazily allocate the awaiter, placed just after coro.begin so CoroSplit spills
// it into the frame (its address escapes to the wheel and it is live across the
// suspend). With a remote `disable` in the design the frame instead holds a
// POINTER to the live heap record, for the aliasing reason above.
llvm::Value* Backend::awaiter() {
    if (awaiter_ == nullptr) {
        // Insert right after coro.begin — using the block + iterator form so it
        // works even when coro.begin is the last instruction so far (a #delay as
        // the process's first statement).
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        if (park_claim_wanted()) {
            awaiter_ = ab.CreateAlloca(llvm::PointerType::getUnqual(ctx_), nullptr, "park_rec");
            ab.CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)), awaiter_);
        } else {
            awaiter_ = ab.CreateAlloca(awaiter_ty(), nullptr, "awaiter");
        }
    }
    return awaiter_;
}

// The shared timer callback: void @clotho.enqueue_timer(ptr timer, ptr ctx).
// When a timer fires it does not resume the coroutine directly; it recovers the
// handle from the awaiter and pushes it onto the active queue. The scheduler
// resumes it (with everything else woken at the same instant) when it next
// drains the queue. Built once per module.
// A JIT-local forwarder for a native wheel-expiry callback.
//
// The wheel stores an expiry callback in the timer node and calls it when the
// timer fires, so the address has to be materialized into memory. Doing that
// for a *runtime* symbol is a different relocation from calling one (address
// materialization rather than a branch), and JITLink gets it wrong on
// AArch64 COFF: the stored pointer is bad and the process dies the instant
// the timer fires. Every design using `assign #d`, a gate delay, or `<= #d`
// segfaulted on windows-arm64 for this reason, while process #delay and
// delayed event triggers -- whose callbacks are JIT-emitted functions --
// were fine on the same host.
//
// Forwarding through a JIT-local function puts every wheel callback on that
// same working footing: the address materialized into the timer is a local
// symbol, and reaching the runtime is an ordinary call.
llvm::Function* Backend::native_cb_trampoline(const char* name, llvm::FunctionCallee target, llvm::Function*& cache) {
    if (cache != nullptr) {
        return sym(cache);
    }
    auto* ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false);
    cache      = llvm::Function::Create(fnty, llvm::Function::InternalLinkage, name, env_->llmod);

    llvm::IRBuilder<> tb(ctx_);
    tb.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", cache));
    tb.CreateCall(target, { cache->getArg(0), cache->getArg(1) });
    tb.CreateRetVoid();
    return sym(cache);
}

llvm::Function* Backend::intra_cb_trampoline() {
    return native_cb_trampoline("clotho.intra_cb", intra_cb_fn(), intra_cb_tramp_);
}

llvm::Function* Backend::inertial_cb_trampoline() {
    return native_cb_trampoline("clotho.inertial_cb", inertial_cb_fn(), inertial_cb_tramp_);
}

// The wheel-expiry callback for a process #delay: pull the handle the awaiter
// parked and wake it into the ring it named. One trampoline serves every
// timer in the design, so the region cannot be baked in here -- a program
// `initial` and an ordinary always block both come through this function.
llvm::Function* Backend::enqueue_trampoline() {
    if (enqueue_fn_ != nullptr) {
        return sym(enqueue_fn_);
    }
    auto* ptr   = llvm::PointerType::getUnqual(ctx_);
    auto* fnty  = llvm::FunctionType::get(b_.getVoidTy(), { ptr, ptr }, false);
    enqueue_fn_ = llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.enqueue_timer", env_->llmod);

    llvm::IRBuilder<> tb(ctx_);
    tb.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", enqueue_fn_));
    if (!park_claim_wanted()) {
        llvm::Value* hp  = tb.CreateStructGEP(awaiter_ty(), enqueue_fn_->getArg(0), 2);
        llvm::Value* hdl = tb.CreateLoad(ptr, hp);
        emit_wake_push(tb, hdl);
        tb.CreateRetVoid();
        return sym(enqueue_fn_);
    }
    // A design with a remote `disable` owns each record on the heap, and a
    // `disable` may already have claimed this park to wake the activation early.
    // Whoever claims it pushes; the loser only frees. A claimed-but-not-fired
    // record is exactly how an abandoned timer stays harmless: the activation has
    // long since walked away, and this never touches its frame again.
    auto*        rec   = enqueue_fn_->getArg(0);
    auto*        aw    = awaiter_ty();
    auto*        fn    = enqueue_fn_;
    auto*        mine  = llvm::BasicBlock::Create(ctx_, "timer.claim", fn);
    auto*        done  = llvm::BasicBlock::Create(ctx_, "timer.done", fn);
    llvm::Value* state = tb.CreateLoad(tb.getInt32Ty(), tb.CreateStructGEP(aw, rec, 4));
    tb.CreateCondBr(tb.CreateICmpEQ(state, tb.getInt32(0)), mine, done);

    tb.SetInsertPoint(mine);
    tb.CreateStore(tb.getInt32(1), tb.CreateStructGEP(aw, rec, 4));
    // Tell the activation it is no longer parked, so a later `disable` looks at
    // its scheduled flag instead of at a record that is about to be freed.
    tb.CreateStore(llvm::ConstantPointerNull::get(ptr),
                   tb.CreateLoad(ptr, tb.CreateStructGEP(aw, rec, 3)));
    emit_wake_push(tb, tb.CreateLoad(ptr, tb.CreateStructGEP(aw, rec, 2)));
    tb.CreateBr(done);

    tb.SetInsertPoint(done);
    tb.CreateCall(free_fn(), { rec });
    tb.CreateRetVoid();
    return sym(enqueue_fn_);
}

// Suspend the coroutine and continue in a fresh resume block named `label`.
// The save token pairs the suspend with this resume point; the default edge
// returns control to the resumer (coro.ret), case 0 resumes in the new block,
// case 1 is a destroy while suspended. Shared by every suspend site (#delay,
// #0, event wait, task enable, join, expect, wait fork).
//
// Coming back in the right region is the parked handle's business, not this
// function's: every site that records a handle for a later wake records it
// through parked_handle, and every site that wakes one reads the region back
// out through emit_wake_push.
//
// This used to refuse a suspend inside any program-reachable subroutine that
// was not a fork child, because a body lowered once could record only one home
// region. build_shared_home_fns removes the premise: a body two kinds of
// caller reach records no home at all and reads the calling thread's instead
// (LRM 24.3.1), so 24.5's `module ... task T; #5; endtask` called from a
// program now lowers. The fork-child exemption is gone with it, and was never
// the right argument anyway: it said a child body belongs to one fork SITE, but
// the home is a property of the spawning THREAD, and a child of a shared task
// is itself shared under the classification and inherits at spawn like any
// other callee.
void Backend::emit_suspend(const char* label) {
    // LRM 9.7's WAITING is "waiting in a blocking statement", which is exactly
    // this: every park goes through here, so the status word is maintained in one
    // place rather than at each parking construct. Two stores, in a design that
    // names `process` and nowhere else.
    emit_proc_state(kProcRunning, kProcWaiting);
    auto* save   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_save), { hdl_ });
    auto* susp   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { save, b_.getFalse() });
    auto* resume = llvm::BasicBlock::Create(ctx_, label, fn_);
    auto* sw     = b_.CreateSwitch(susp, coro_ret_bb_, 2);
    sw->addCase(b_.getInt8(0), resume);
    sw->addCase(b_.getInt8(1), coro_cleanup_bb_);
    b_.SetInsertPoint(resume);
    emit_proc_state(kProcWaiting, kProcRunning);
}

// Run every `final` block once, in source order, at the end of simulation
// (LRM 9.2.3). A final block is zero-time, so — like the pre-init pass — one
// resume runs its body to completion; destroy reclaims the frame. Emitted as
// a separate `sim_finals(state, wheel)` so rt_sim_run can invoke it on both
// the natural-drain and $finish exit paths.
void Backend::emit_finals_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
    auto* ptr_ty    = llvm::PointerType::getUnqual(ctx_);
    auto* proc_fnty = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
    const auto all  = mod_.processes();
    for (uint32_t g = 0; g < procs.size(); ++g) {
        if (all[groups_[g].front()].activation != Activation::Final) {
            continue;
        }
        llvm::Value* hdl = b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    }
}

// Whether the design has anything to sample. A design with no concurrent
// assertions must pay nothing for this region existing.
bool Backend::has_preponed() const {
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::Preponed) {
            return true;
        }
    }
    return false;
}

// Run every Preponed process once (LRM 4.4.2.1). Called at the top of a new
// time step -- after the wheel has fired its timers, which only enqueues
// handles, and before any body of that step executes -- and once from
// emit_main so time zero is sampled too.
//
// A Preponed body has no timing, so one resume runs it to completion, and its
// stores generate no events (silent_stores_), so nothing wakes on a shadow.
void Backend::emit_preponed_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
    auto* ptr_ty    = llvm::PointerType::getUnqual(ctx_);
    auto* proc_fnty = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
    const auto all  = mod_.processes();
    for (uint32_t g = 0; g < procs.size(); ++g) {
        if (all[groups_[g].front()].activation != Activation::Preponed) {
            continue;
        }
        llvm::Value* hdl = b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    }
}

// Whether any process runs in the Observed region. A design with no
// concurrent assertions must pay nothing for the region existing.
bool Backend::has_observed() const {
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::Observed) {
            return true;
        }
    }
    return false;
}

// Whether any process belongs to the reactive region set by being program
// code (LRM 24.3): a program block's `initial`, or a continuous construct
// declared inside a program, whose home is Reactive too (24.3.1). Either
// presence alone means the design needs the reactive region set even with no
// concurrent assertion anywhere -- and a program whose only member is an
// `assign` has no Activation::Reactive process at all, so both arms are
// load-bearing. A design with no program pays nothing, exactly like
// has_observed.
bool Backend::has_program() const {
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::Reactive || p.reactive_home) {
            return true;
        }
    }
    return false;
}

// Whether any process is a program block's `initial` PROCEDURE (LRM 24.3).
// Deliberately narrower than has_program(): the clause's two rules -- terminate
// a program's descendant threads when all its `initial`s have ended, then end
// the simulation when every program's have -- count `initial` procedures, and a
// program's reactive-home continuous code (24.3.1) is not one and never reaches
// an end. Keyed on the activation alone so it cannot start answering yes for
// one.
bool Backend::has_program_initial() const {
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::Reactive) {
            return true;
        }
    }
    return false;
}

// Run every Observed process once (LRM 4.4.2.5), at the settled point where
// the Active / Inactive / NBA set has drained. This is where a concurrent
// assertion steps its automaton; the pass/fail code a verdict calls for is
// not run here but pended onto the reactive ring (lower_reactive_pend), which
// the drain just past this call site empties.
//
// An Observed body has no timing, so one resume runs it to completion (a
// suspension is rejected in build_reactive_fns). Unlike Preponed its stores
// are ordinary design writes, NOT silent_stores_: it may wake an Active
// process, and emit_active_run re-settles the active set when it does.
void Backend::emit_observed_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
    auto* ptr_ty    = llvm::PointerType::getUnqual(ctx_);
    auto* proc_fnty = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
    const auto all  = mod_.processes();
    for (uint32_t g = 0; g < procs.size(); ++g) {
        if (all[groups_[g].front()].activation != Activation::Observed) {
            continue;
        }
        llvm::Value* hdl = b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    }
}

// Whether any process runs in the Re-NBA region. A design with no clocking
// block pays nothing for the region existing, exactly like has_observed.
bool Backend::has_renba() const {
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::ReNba) {
            return true;
        }
    }
    return false;
}

// Run every Re-NBA process once (LRM 4.4.2.8), at the point where the reactive
// region set has settled -- so after every process that could have issued a
// clocking block synchronous drive this time slot, which is what LRM 14.16
// requires of one.
//
// Emitted like emit_observed_run, and its stores are ordinary design writes for
// the same reason: a driven signal must wake its readers. The caller re-settles
// the active set afterwards.
void Backend::emit_renba_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
    auto* ptr_ty    = llvm::PointerType::getUnqual(ctx_);
    auto* proc_fnty = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
    const auto all  = mod_.processes();
    for (uint32_t g = 0; g < procs.size(); ++g) {
        if (all[groups_[g].front()].activation != Activation::ReNba) {
            continue;
        }
        llvm::Value* hdl = b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    }
}

// Whether `id` is reachable as reactive code (build_reactive_fns).
bool Backend::is_reactive_fn(FunctionId id) const {
    const auto k = static_cast<uint32_t>(id);
    return k < reactive_fns_.size() && reactive_fns_[k];
}

// The LRM 24.3 live-program counter: how many program `initial` procedures
// have not yet completed, DESIGN-WIDE. Created on first use, so a design with
// no program never emits it.
//
// This is the clause's SECOND sentence: "the entire simulation shall terminate
// by means of an implicit call to the $finish system task immediately after all
// the threads and all their descendent threads originating from all initial
// procedures within all programs have ended". Counting only the `initial`
// procedures is enough for it precisely BECAUSE of the first sentence: each
// program instance terminates its own descendants the instant its own count
// reaches zero (program_end_bb_ in lower_group), so by the time the last
// `initial` in the design has ended there is provably no descendant of any of
// them left running, and "all their descendent threads have ended" reduces to
// "this counter is zero".
llvm::Constant* Backend::program_live_ptr() {
    if (program_live_gv_ == nullptr) {
        auto* i32        = llvm::Type::getInt32Ty(ctx_);
        program_live_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false,
                                                    llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantInt::get(i32, program_proc_count_),
                                                    "clotho.program_live");
    }
    return sym(program_live_gv_);
}

// Partition the program `initial` procedures into program INSTANCES, which is
// the granularity LRM 24.3's FIRST sentence works at: "when all initial
// procedures within a program have reached their end, that program shall
// immediately terminate all descendent threads of initial procedures within
// that program". Process::scope is the InstanceScope::id of the instance whose
// definition declared the process, so two instances of one program definition
// partition apart for free and each gets its own countdown and its own
// termination root.
//
// The instance index is folded into each body as a COMPILE-TIME constant. That
// is sound because a program `initial` body is never shared between instances:
// the only body sharing in the backend is share_twin_groups, which takes only
// thunk groups, and thunk eligibility is is_simple_always, which requires
// Activation::Always (src/ir/Analysis.cpp) -- so an Activation::Reactive
// process is always a coroutine of its own, one per instance, exactly as
// lower_group's deferred-assertion note already relies on. No instance-context
// plumbing is needed, and docs/design/program-blocks.md §5 is wrong to say it
// is.
void Backend::build_program_instances() {
    const auto   all   = mod_.processes();
    const size_t nproc = all.size();
    program_inst_procs_.clear();
    program_inst_of_proc_.clear();
    if (!has_program_initial()) {
        return;
    }
    program_inst_of_proc_.assign(nproc, kNoProgramInst);
    // Scope id per instance index. A linear scan because a design has a handful
    // of program instances, never thousands.
    std::vector<uint32_t> scope_of_inst;
    for (uint32_t k = 0; k < nproc; ++k) {
        if (all[k].activation != Activation::Reactive) {
            continue;
        }
        uint32_t inst = kNoProgramInst;
        for (uint32_t i = 0; i < scope_of_inst.size(); ++i) {
            if (scope_of_inst[i] == all[k].scope) {
                inst = i;
                break;
            }
        }
        if (inst == kNoProgramInst) {
            inst = static_cast<uint32_t>(scope_of_inst.size());
            scope_of_inst.push_back(all[k].scope);
            program_inst_procs_.emplace_back();
        }
        program_inst_of_proc_[k] = inst;
        program_inst_procs_[inst].push_back(k);
    }
}

// Which program instance's `initial` procedures reach each subroutine, for LRM
// 24.7's `$exit`. The clause scopes `$exit` by the thread -- "from a thread or
// its descendent thread originating in an initial procedure of a program block"
// -- and a subroutine body is lowered once for every caller, so the only threads
// it can name are the ones that statically reach it:
//
//   kNoProgramInst        no program `initial` reaches it, so a `$exit` inside it
//                         is the clause's "shall be ignored" arm.
//   <inst>                exactly one program instance's `initial`s reach it, and
//                         nothing else does, so `$exit` names that instance.
//   kAmbiguousProgramInst two instances' `initial`s reach it, or an `initial` and
//                         some process that is not one. Refused in
//                         lower_program_exit.
//
// A fork child is covered by the same table: collect_callees follows a Fork's
// children like any other call edge, so a child of a program `initial` is
// reachable only from that `initial` and lands on its instance.
void Backend::build_program_exit_owners() {
    const size_t nfn = mod_.function_count();
    program_exit_owner_.assign(nfn, kNoProgramInst);
    if (program_inst_procs_.empty()) {
        return;
    }
    std::vector<FunctionId> work;
    // Per-instance visit stamp, so each instance's walk descends through a
    // function exactly once. A shared "already ambiguous, stop" test would be
    // wrong: it would leave a function reached only THROUGH the ambiguous one
    // owned by whichever instance got there first.
    std::vector<uint32_t> stamp(nfn, kNoProgramInst);
    for (uint32_t inst = 0; inst < program_inst_procs_.size(); ++inst) {
        work.clear();
        for (const uint32_t p : program_inst_procs_[inst]) {
            collect_callees(mod_.processes()[p].ir, work);
        }
        while (!work.empty()) {
            const auto f = static_cast<uint32_t>(work.back());
            work.pop_back();
            if (stamp[f] == inst) {
                continue;
            }
            stamp[f]               = inst;
            program_exit_owner_[f] = program_exit_owner_[f] == kNoProgramInst ? inst : kAmbiguousProgramInst;
            collect_callees(mod_.function(static_cast<FunctionId>(f)).ir, work);
        }
    }
    // Now every process that is NOT a program `initial`. The seed is the clause's
    // own test, not an approximation of it: 24.7 keys on where the calling thread
    // ORIGINATES -- "a thread or its descendent thread that does not originate in
    // an initial procedure in a program shall be ignored" -- so a design process
    // and a program's own reactive-home continuous process (24.3.1) both land on
    // the ignored arm, because neither is an `initial` procedure and nothing
    // spawns either from one. `reactive_home` answers a different question (which
    // REGION the body runs in) and deliberately does not appear here.
    //
    // A subroutine such a process shares with a program `initial` therefore has
    // two answers -- terminate that program for the `initial` caller, ignore for
    // the other -- even when both callers sit in the SAME program instance, so it
    // is ambiguous for the same reason two instances are.
    work.clear();
    for (const Process& p : mod_.processes()) {
        if (p.activation != Activation::Reactive) {
            collect_callees(p.ir, work);
        }
    }
    std::vector<bool> from_design(nfn, false);
    while (!work.empty()) {
        const auto f = static_cast<uint32_t>(work.back());
        work.pop_back();
        if (from_design[f]) {
            continue;
        }
        from_design[f] = true;
        if (program_exit_owner_[f] != kNoProgramInst) {
            program_exit_owner_[f] = kAmbiguousProgramInst;
        }
        collect_callees(mod_.function(static_cast<FunctionId>(f)).ir, work);
    }
}

// The program instance a `$exit` in function `id` names (build_program_exit_owners).
// Bounds-checked like is_program_fn: the table is only sized for a design that has
// a program.
uint32_t Backend::program_exit_owner_of(FunctionId id) const {
    const auto k = static_cast<uint32_t>(id);
    return k < program_exit_owner_.size() ? program_exit_owner_[k] : kNoProgramInst;
}

// `$exit()` (LRM 24.7): "Calling $exit from a thread or its descendent thread
// originating in an initial procedure of a program block shall terminate all
// initial procedures and their descendent threads within that originating program
// block. Calling $exit from a thread or its descendent thread that does not
// originate in an initial procedure in a program shall be ignored, and a warning
// may be issued to indicate that the call to $exit has been ignored."
//
// Which program instance the calling thread originates in is `cur_program_inst_`,
// decided per BODY (lower_group for a program `initial`, the owner closure for a
// subroutine or a fork child) because the op carries no payload.
//
// The two-sided part. `$exit` terminates the `initial`s AND every descendant, and
// the calling thread is itself one of those things -- so the set to mark is each
// of the instance's `initial` threads (subtree included, which is where a
// descendant of a still-running `initial` lives) plus the instance's termination
// root (where the descendants of `initial`s that ALREADY ended were handed on,
// see program_root_pcb). What this deliberately does NOT do is any counting: the
// per-instance and design-wide countdowns live in program.end (emit_program_end),
// every marked `initial` reaches program.end through its own abort exit
// (proc_abort_exit), and the abort protocol delivers it there exactly once --
// rt_proc_mark_kill skips a thread that is already FINISHED, KILLED or marked, and
// the force-wake is claim-or-flag. So each `initial` is counted once whether it
// ended normally, earlier, or by this `$exit`, and the design-wide implicit
// $finish still fires exactly once. Counting here instead would need a second
// "has this one been counted?" flag per procedure, which is precisely the
// double-decrement this avoids.
void Backend::lower_program_exit(Index i) {
    (void)i; // no payload: the owner is a property of the body, not of the call
    if (cur_program_inst_ == kNoProgramInst) {
        // 24.7's second sentence. The frontend already warned where it could tell
        // statically (a design process); here we are in a subroutine no program
        // `initial` reaches, so the call is ignored and nothing is emitted.
        return;
    }
    if (cur_program_inst_ == kAmbiguousProgramInst) {
        // TODO: `$exit` from a subroutine reached by threads with different
        // origins -- two program instances' `initial`s, or an `initial` and any
        // process that is not one (a design process, or a program's own
        // reactive-home continuous process). 24.7 answers by where the CALLING
        // THREAD originates, and a shared body lowered once cannot name it: that
        // needs per-thread program identity in the control block (the same thing
        // the thread-carried home region needs, docs/design/program-blocks.md §6).
        // Refused rather than guessing an instance or silently ignoring the call.
        throw BackendError{ "`$exit` in a subroutine reachable from more than one program "
                            "instance's `initial` procedures, or from an `initial` and from "
                            "code that is not one, is not supported: LRM 24.7 answers by "
                            "where the calling thread originates and this body is lowered "
                            "once for every caller" };
    }
    llvm::BasicBlock* exit = proc_abort_exit();
    if (exit == nullptr) {
        // TODO: `$exit` inside a FUNCTION. A function is not a coroutine and has
        // no abort exit; its caller would keep running, because the abort poll is
        // emitted after a suspending op and a plain Call is not one. The shape that
        // would carry it is may_finish_ / emit_finish_check, which does exactly
        // this job for $finish.
        throw BackendError{ "`$exit` inside a function is not supported: the function has no "
                            "abort exit, so its caller would keep running past the point "
                            "LRM 24.7 ends the thread" };
    }
    if (pcb_ == nullptr) {
        // Unreachable: the ProgramExit tag sets DesignUses::process (Analysis.cpp),
        // so every thread in the design has a control block. Checked because the
        // marks below are the whole effect of the statement.
        throw BackendError{ "internal: `$exit` with no thread control block" };
    }
    // Each `initial` of this instance, subtree included. with_self, because 24.7
    // terminates the procedures themselves and not merely their descendants;
    // branch_on_self suppressed so one matching root does not strand the walks
    // after it.
    for (const uint32_t p : program_inst_procs_[cur_program_inst_]) {
        lower_proc_kill(proc_pcb_slot(p), /*with_self=*/true, /*branch_on_self=*/false);
    }
    // ... and the descendants that outlived an `initial` which already ended.
    lower_proc_kill(program_root_pcb(cur_program_inst_),
                    /*with_self=*/false,
                    /*branch_on_self=*/false);
    // The calling thread is in the marked set but is RUNNING, so no wake can reach
    // it (lower_proc_kill skips its own block deliberately). It leaves here: an
    // `initial` through program.end, which counts it down, a fork child or task
    // through its epilogue, which wakes its caller so the caller polls and aborts
    // in turn.
    b_.CreateBr(exit);
    // The rest of the region is dead but still gets lowered, exactly as after a
    // $finish (lower_finish): park it in a block with no predecessors.
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "exit.dead", fn_));
}

// The program instance `proc_index` belongs to, or kNoProgramInst for a process
// that is not a program `initial`. Bounds-checked like is_program_fn: the table
// is only sized for a design that has a program.
uint32_t Backend::program_inst_of(uint32_t proc_index) const {
    return proc_index < program_inst_of_proc_.size() ? program_inst_of_proc_[proc_index] : kNoProgramInst;
}

// The per-program-instance live-`initial` counter (LRM 24.3, first sentence):
// how many `initial` procedures of instance `inst` have not yet reached their
// end. One i32 per program instance, initialized to that instance's count.
// Created on first use, so a design with no program never emits it.
llvm::Value* Backend::program_inst_live_ptr(uint32_t inst) {
    if (program_inst_live_gv_ == nullptr) {
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* ty  = llvm::ArrayType::get(i32, std::max<size_t>(program_inst_procs_.size(), 1));
        std::vector<llvm::Constant*> init;
        init.reserve(program_inst_procs_.size());
        for (const std::vector<uint32_t>& procs : program_inst_procs_) {
            init.push_back(llvm::ConstantInt::get(i32, procs.size()));
        }
        if (init.empty()) {
            init.push_back(llvm::ConstantInt::get(i32, 0));
        }
        program_inst_live_arr_ty_ = ty;
        program_inst_live_gv_     = new llvm::GlobalVariable(root_env_->llmod,
                                                             ty,
                                                             false,
                                                             llvm::GlobalValue::InternalLinkage,
                                                             llvm::ConstantArray::get(ty, init),
                                                             "clotho.program_inst_live");
    }
    return b_.CreateInBoundsGEP(
      program_inst_live_arr_ty_, sym(program_inst_live_gv_), { b_.getInt32(0), b_.getInt32(inst) });
}

// Everything LRM 24.3 requires when ONE program `initial` procedure reaches its
// end, emitted into program.end -- the block every exit of a program body takes
// except the one that must not count (lower_group, proc_abort_exit,
// emit_finish_exit). `proc_index` is the procedure that is ending.
//
// The clause, in two sentences that work at two different granularities:
//
//   "When all initial procedures within a program have reached their end, that
//    program shall immediately terminate all descendent threads of initial
//    procedures within that program. If there is at least one initial procedure
//    within at least one program block, the entire simulation shall terminate by
//    means of an implicit call to the $finish system task immediately after all
//    the threads and all their descendent threads originating from all initial
//    procedures within all programs have ended."
//
// So: a per-INSTANCE countdown that terminates that instance's descendants, and
// a DESIGN-WIDE one that ends the run. Both are plain counters -- nothing here
// walks the process table at run time -- and the instance index is a compile-time
// constant (build_program_instances).
void Backend::emit_program_end(uint32_t proc_index) {
    auto*          i32  = b_.getInt32Ty();
    const uint32_t inst = program_inst_of(proc_index);
    if (inst == kNoProgramInst) {
        // in_program_body_ is exactly Activation::Reactive and
        // build_program_instances partitions every one of those, so this is
        // unreachable. Checked rather than assumed: silently skipping a
        // program's termination is the failure mode this whole block exists to
        // remove.
        throw BackendError{ "internal: a program `initial` with no program instance" };
    }

    // FIRST SENTENCE, per program instance. Only when the design has control
    // blocks at all: design_uses_process_ is false exactly when nothing in the
    // design forks (see run()), and then an `initial` has no descendant thread
    // to move or to terminate, so the whole per-instance counter is dead and is
    // never even emitted.
    if (pcb_ != nullptr) {
        // Hand this procedure's live descendants to the instance's termination
        // root BEFORE the count is tested. Two reasons, both load-bearing: it
        // keeps them reachable when a sibling `initial` of the same program is
        // still running (rt_proc_complete would otherwise re-parent them onto
        // this static process's null parent and lose them -- see
        // program_root_pcb), and it puts them under the single node the kill
        // below is rooted at, so one subtree walk covers every descendant of
        // every `initial` of this instance, transitively.
        llvm::Value* root = program_root_pcb(inst);
        b_.CreateCall(proc_adopt_fn(), { root, pcb_ });
        llvm::Value* ilive = program_inst_live_ptr(inst);
        llvm::Value* ni    = b_.CreateSub(b_.CreateLoad(i32, ilive), b_.getInt32(1));
        b_.CreateStore(ni, ilive);
        auto* term = llvm::BasicBlock::Create(ctx_, "program.inst.last", fn_);
        auto* rest = llvm::BasicBlock::Create(ctx_, "program.inst.more", fn_);
        b_.CreateCondBr(b_.CreateICmpEQ(ni, b_.getInt32(0)), term, rest);

        b_.SetInsertPoint(term);
        // "shall immediately terminate all descendent threads": the `kill` /
        // `disable fork` subtree abort (LRM 9.7, 9.6.3), rooted at this
        // instance's node and NOT including it, since the node is not a thread.
        // Every marked thread is force-woken exactly once and leaves through its
        // own abort exit, so nothing is destroyed from outside and there is no
        // second termination protocol (docs/design/process-class.md §7.1).
        // Before the implicit $finish below, which is the clause's own order.
        lower_proc_kill(root, /*with_self=*/false);
        b_.CreateBr(rest);
        b_.SetInsertPoint(rest);
    }

    // SECOND SENTENCE, design-wide: when every program `initial` has ended the
    // simulation ends on its own, without waiting for the modules.
    llvm::Value* live = program_live_ptr();
    llvm::Value* n    = b_.CreateSub(b_.CreateLoad(i32, live), b_.getInt32(1));
    b_.CreateStore(n, live);
    auto* last = llvm::BasicBlock::Create(ctx_, "program.last", fn_);
    auto* more = llvm::BasicBlock::Create(ctx_, "program.more", fn_);
    b_.CreateCondBr(b_.CreateICmpEQ(n, b_.getInt32(0)), last, more);

    b_.SetInsertPoint(last);
    // Same cooperative stop $finish takes (lower_finish): report, then empty the
    // rings so nothing else queued at this instant runs. The body still leaves
    // through its own final suspend, so its frame is released by the resumer
    // exactly as on a normal completion.
    llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel_ });
    b_.CreateCall(finish_coop_fn(),
                  { b_.CreateGlobalString("implicit $finish from program"), now, b_.getInt32(0) });
    if (active_head_gv_ != nullptr) {
        b_.CreateStore(b_.CreateLoad(i32, sym(active_tail_gv_)), sym(active_head_gv_));
    }
    if (reactive_head_gv_ != nullptr) {
        b_.CreateStore(b_.CreateLoad(i32, sym(reactive_tail_gv_)), sym(reactive_head_gv_));
    }
    if (re_inactive_top_gv_ != nullptr) {
        b_.CreateStore(b_.getInt32(0), sym(re_inactive_top_gv_));
    }
    b_.CreateBr(more);
    b_.SetInsertPoint(more);
}

// -- Top-level entry --
//
// Emit `i32 @clotho_main()`, the entry rt_run calls: heap-allocate the shared
// state, power it up, build the timing wheel, then run every process through
// the active task queue and drain the wheel (advancing time, resuming
// delayed/woken processes) until idle, then run the `final` blocks. Returns
// 0. Not `main`: a linked executable has its own, which calls rt_run.
void Backend::emit_main(const std::vector<llvm::Function*>& procs) {
    sched_anchor_ = SignalSlot::no_anchor; // startup code addresses absolutely
    auto* ptr_ty  = llvm::PointerType::getUnqual(ctx_);
    auto* subfnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);

    // sim_drain(state, wheel): seed every non-final process and run the event
    // loop to quiescence. Split out of main so rt_sim_run can wrap it in the
    // finals — a $finish stops the drain, which returns into rt_sim_run below
    // main's frame, so the heap state pointer stays live (the wheel is the
    // @clotho.wheel global, build_wheel_global, and outlives every frame) and
    // the final blocks still observe the settled design state.
    auto* drain = llvm::Function::Create(subfnty, llvm::Function::InternalLinkage, "sim_drain", env_->llmod);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", drain));
    emit_active_run(drain->getArg(0), drain->getArg(1), procs);
    b_.CreateRetVoid();

    // sim_finals(state, wheel): run every `final` block once.
    auto* finals = llvm::Function::Create(subfnty, llvm::Function::InternalLinkage, "sim_finals", env_->llmod);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", finals));
    emit_finals_run(finals->getArg(0), finals->getArg(1), procs);
    b_.CreateRetVoid();

    // The constant an executable's main reads beside clotho_main: rt_design
    // (Runtime.h), whose fields are this struct's, in order.
    {
        auto* top     = llvm::ConstantDataArray::getString(ctx_, mod_.top_name);
        auto* top_var = new llvm::GlobalVariable(
          env_->llmod, top->getType(), true, llvm::GlobalValue::PrivateLinkage, top, "clotho.design.top");
        top_var->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        auto* design_ty =
          llvm::StructType::get(ctx_, { b_.getInt64Ty(), b_.getInt32Ty(), llvm::PointerType::getUnqual(ctx_) });
        auto* design = llvm::ConstantStruct::get(
          design_ty,
          { b_.getInt64(mod_.tick_fs), b_.getInt32(static_cast<uint32_t>(config_.allow_system)), top_var });
        new llvm::GlobalVariable(
          env_->llmod, design_ty, true, llvm::GlobalValue::ExternalLinkage, design, "clotho_design");
    }

    auto* i32  = llvm::Type::getInt32Ty(ctx_);
    auto* fnty = llvm::FunctionType::get(i32, false);
    auto* main = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, "clotho_main", env_->llmod);

    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", main);
    b_.SetInsertPoint(entry);
    // Before anything can allocate: rt_gc_init cross-checks our own
    // DataLayout computations -- the vptr-to-map offset against its
    // rt_gc_map_vptr_offset(), the request-flag offset against its
    // rt_gc_mutator_request_offset() -- so a descriptor or mutator layout
    // change fails loudly here instead of reading garbage at the first
    // trace or polling the wrong byte at the first safepoint
    // (Gc.h:167-173).
    b_.CreateCall(gc_init_fn(), { gc_config_global() });
    // Every process-global runtime pool back to power-on, before anything can
    // allocate out of one. Unconditional: build_wait_queues runs for every
    // design and always creates clotho.events with at least the reserved null
    // slot, and the static block has to be re-registered as the pool's chunk 0
    // on every run because it is a JIT global that died with the previous
    // LLJIT — init_state below already binds aggregate event leaves through it.
    // The same seam owns the process control-block pool and the detached
    // `->> @(x) e` subscription table, which otherwise carry a finished
    // design's pointers and trigger ids into this one (Runtime.h:1472).
    b_.CreateCall(run_init_fn(), { sym(events_gv_), b_.getInt32(event_slot_count_) });
    // The coverage model (LRM 19), before any `new` can register an instance
    // against it. Only for a design that has a covergroup: rt_cover_init would
    // otherwise be a declaration nothing needs, and the runtime treats an
    // uninitialized model as an empty one anyway.
    if (!mod_.cover_types.empty()) {
        b_.CreateCall(cover_init_fn(), { cover_config_global() });
    }
    emit_gc_root_tables();
    // The fallback RNG (LRM 18.13), seeded before anything can draw from it.
    // Its selector is deliberately outside the process-index space every
    // per-process stream is seeded from, so the two never coincide.
    b_.CreateCall(urandom_seed_fn(), { sym(rng_global_), b_.getInt64(~0ull) });

    // The design state struct holds every signal, including large memories.
    // It is a single internal global whose initializer is the whole power-on
    // state (`state_initializer`) — the optimizer can then constant-fold the
    // fixed base into every signal GEP, and no per-signal init store is
    // emitted (only arrays keep a runtime fill loop; see `init_state`).
    // 64-byte aligned to cover any packed-vector alignment in the struct.
    // Created up front by emit_state_global (bodies address it through the
    // instance anchors); reused-definition anchor bases are stored here,
    // before init_state and any process, so every later load sees them.
    llvm::Value* state = sym(state_global_);
    const auto ctx_ptr = [&](Anchor& a, uint32_t field) {
        return b_.CreateInBoundsGEP(a.ctx_ty, sym(a.ctx_gv), { b_.getInt32(0), b_.getInt32(field) });
    };
    for (Anchor& a : anchors_) {
        if (a.ctx_gv == nullptr) {
            continue;
        }
        b_.CreateStore(sym(a.state_gv), ctx_ptr(a, 0));
        if (a.shadow_ty != nullptr) {
            b_.CreateStore(sym(a.shadow_gv), ctx_ptr(a, 1));
        }
        if (a.sched_gv != nullptr) {
            b_.CreateStore(sym(a.sched_gv), ctx_ptr(a, 2));
        }
        if (a.first_vcd >= 0) {
            b_.CreateStore(b_.getInt32(static_cast<uint32_t>(a.first_vcd)), ctx_ptr(a, 3));
        }
        for (const Signal sj : anchor_signals_[static_cast<size_t>(&a - anchors_.data())]) {
            if (sj == Signal{ ~0u }) {
                continue;
            }
            const auto si = static_cast<uint32_t>(sj);
            for (uint32_t e = 0; e < 3; ++e) {
                if (wait_slot_sta_[si][e] != no_wait_slot) {
                    b_.CreateStore(sym(wait_sta_gv_[si][e]),
                                   ctx_ptr(a, a.ctx_wait_base + wait_slot_sta_[si][e]));
                }
                if (wait_slot_dyn_[si][e] != no_wait_slot) {
                    b_.CreateStore(sym(wait_dyn_gv_[si][e]),
                                   ctx_ptr(a, a.ctx_wait_base + wait_slot_dyn_[si][e]));
                    b_.CreateStore(sym(wait_dyn_len_gv_[si][e]),
                                   ctx_ptr(a, a.ctx_wait_base + wait_slot_len_[si][e]));
                }
            }
        }
    }
    for (const auto& [aid, field, first_id] : ctx_blk_fills_) {
        b_.CreateStore(b_.getInt32(first_id), ctx_ptr(anchors_[aid], field));
    }
    init_state();
    // Shadow bases fill only when some shadow access loaded them (lazy):
    // the root base covers unscoped targets, the per-instance bases each
    // scope's unshared accesses; shareable reused-definition accesses go
    // through the ctx fills above instead.
    if (shadow_global_ != nullptr && shadow_base_gv_ != nullptr) {
        b_.CreateStore(sym(shadow_global_), sym(shadow_base_gv_));
    }
    for (Anchor& a : anchors_) {
        if (a.shadow_base_gv != nullptr) {
            b_.CreateStore(sym(a.shadow_gv), sym(a.shadow_base_gv));
        }
    }

    emit_ref_name_table();

    // Register one DPI scope per top (svGetScope / user-data, LRM 35.5.3) before
    // any process runs, so a context import can read it. Only when DPI is used.
    // Index 0 clears the runtime table, so the first top stays the default scope
    // and a second simulation in one process starts clean. A design with no top
    // instances (package-only) still registers one scope so svGetScope is never
    // null.
    if (design_uses_dpi()) {
        const llvm::FunctionCallee reg = rt_fn("rt_svdpi_scope_register", b_.getVoidTy(),
                                               { raw_ptr_ty(), b_.getInt32Ty(), b_.getInt32Ty() });
        const auto emit_reg = [&](const std::string& name, uint32_t index) {
            b_.CreateCall(reg, { b_.CreateGlobalString(name),
                                 b_.getInt32(static_cast<uint32_t>(name.size())),
                                 b_.getInt32(index) });
        };
        if (mod_.tops().empty()) {
            emit_reg(mod_.top_name, 0);
        } else {
            uint32_t index = 0;
            for (const InstanceScope& top : mod_.tops()) {
                emit_reg(top.name, index++);
            }
        }
    }

    // Timing wheel: the event scheduler. Processes schedule timers on it at
    // #delay and the drain loop ticks it to advance time and resume them.
    //
    // A GLOBAL rather than main's alloca, so a body with no wheel parameter can
    // still read the clock. A plain function is the case that needs it: every
    // event op stamps or compares the current tick, and a function's ABI carries
    // state and rng but no wheel. One global costs nothing (the wheel is a
    // singleton either way) and avoids widening every function signature.
    llvm::Value* wheel = sym(wheel_gv_);
    b_.CreateCall(wheel_init_fn(), { wheel });

    // Seed the %t formatter with the design tick size (fs) and the default
    // time-unit exponent (smallest design precision) before any process runs.
    b_.CreateCall(timeformat_init_fn(),
                  { b_.getInt64(mod_.tick_fs), b_.getInt32(static_cast<uint32_t>(mod_.timeformat_units)) });

    // Run the drain under the $finish catch, then the final blocks. Both exit
    // paths (natural drain, $finish) return here with `state` still live.
    b_.CreateCall(sim_run_fn(), { state, wheel, drain, finals });

    // Release heap owned by string signals. Reached on every exit path: the
    // drain returns whether it ran out of work or a $finish stopped it, and a
    // $finish inside a final block returns through that block's epilogue.
    for (const SignalSlot& s : slots_) {
        if (s.is_string) {
            emit_string_slot_op(s, str_deinit_fn());
        } else if (s.is_dynarr || s.is_queue || s.is_assoc) {
            emit_handle_delete(storage_ptr(s), s.ir_type);
        } else if (s.is_agg && has_heap_leaf(s.ir_type)) {
            // Aggregate signal with string / handle leaves: recursive
            // teardown of every heap-owning leaf.
            std::vector<llvm::Value*> path;
            emit_agg_deinit(s.ir_type, storage_ptr(s), path, s.storage_ty);
        }
    }
    // Runs every remaining finalizer and releases the heap, on exactly the
    // same "every exit path" the signal teardown above relies on -- natural
    // drain, $finish in a process, and $finish inside a final block all
    // return through sim_run above. Not needed for correctness (the process
    // is exiting) but it is what makes LeakSanitizer meaningful for class
    // objects, which is the acceptance evidence for the collector
    // (Gc.h:169-172). After the signal teardown, so a finalizer and a signal
    // deinit can never both reach one managed buffer: a class body's managed
    // properties are reachable only from the body.
    b_.CreateCall(gc_shutdown_fn(), {});

    b_.CreateCall(wheel_deinit_fn(), { wheel });
    b_.CreateRet(llvm::ConstantInt::get(i32, 0));
}

void Backend::ensure_active_queue() {
    if (active_gv_ != nullptr) {
        return;
    }
    auto*      ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto*      i32    = llvm::Type::getInt32Ty(ctx_);
    // One LIVE slot per process suffices even with tasks: the scheduled flag
    // dedupes wakes, and (without fork) each process is a single call-stack
    // with at most one ready coroutine at a time — a task handle, or the
    // parent it re-enqueues on completion, reuses that chain's one slot.
    // Rounded up to a power of two so the monotone head/tail indices wrap
    // with a mask (total pushes per drain are unbounded — a process may be
    // re-woken after it runs — only the live count is bounded).
    //
    // `fork … join` (LRM 9.3.2) adds live children, one ring slot each. How many
    // can be live at one instant is not the number of child BODIES -- a task
    // body containing a fork is shared by every call site and can have several
    // live activations at once -- so fork_ring_slots() counts activations over
    // the spawn / call graph (its derivation is there, and in
    // docs/design/fork-join.md). Whatever it returns, emit_active_push checks
    // the bound at run time and reports rather than overwriting a live handle.
    // join_any / join_none are unbounded and rejected by the frontend until
    // V2's growable ring.
    const auto cap =
      std::bit_ceil(std::max<size_t>(mod_.processes().size() + fork_ring_slots(), 1));
    active_mask_   = static_cast<uint32_t>(cap - 1);

    queue_arr_ty_ = llvm::ArrayType::get(ptr_ty, cap);
    active_gv_    = new llvm::GlobalVariable(env_->llmod, queue_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                             llvm::ConstantAggregateZero::get(queue_arr_ty_), "clotho.active");
    // Ring indices: pop at head, push at tail, empty iff equal. Monotone
    // u32s (masked on access), so u32 wraparound is harmless.
    active_head_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                               llvm::ConstantInt::get(i32, 0), "clotho.active_head");
    active_tail_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                               llvm::ConstantInt::get(i32, 0), "clotho.active_tail");
    // Count of process activations at the current time instant. Reset when the
    // wheel advances time; a runaway count means a zero-delay oscillation the
    // scheduler cannot converge, caught in the drain loop (rt_oscillation_abort).
    auto* i64      = llvm::Type::getInt64Ty(ctx_);
    osc_count_gv_  = new llvm::GlobalVariable(env_->llmod, i64, false, llvm::GlobalValue::InternalLinkage,
                                              llvm::ConstantInt::get(i64, 0), "clotho.osc_count");
}

// The inactive queue: same shape and capacity as the active queue, holding
// handles parked on a zero delay (#0). Promoted into the active queue at the
// same instant, after active drains and before NBA (see emit_active_run).
void Backend::ensure_inactive_queue() {
    if (inactive_gv_ != nullptr) {
        return;
    }
    ensure_active_queue(); // queue_arr_ty_ is shared by both queues
    auto* i32    = llvm::Type::getInt32Ty(ctx_);
    inactive_gv_ = new llvm::GlobalVariable(env_->llmod, queue_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                            llvm::ConstantAggregateZero::get(queue_arr_ty_), "clotho.inactive");
    inactive_top_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                                llvm::ConstantInt::get(i32, 0), "clotho.inactive_top");
}

// The ring-overflow guard the capacity argument above leans on: a ring holding
// `cap` live handles cannot take another without overwriting one, and the
// overwritten entry is a live coroutine -- corruption with no diagnostic, the
// one failure mode PRINCIPLES.md forbids. Report it and end the simulation instead
// (docs/design/fork-join.md, "Defense in depth"). `ring` is the runtime's ring
// selector (0 active, 1 reactive), a value rather than a constant because
// emit_wake_push picks its ring at run time.
//
// Monotone counters make `tail - head` the exact live count, so the compare is
// unsigned >= cap: an off-by-one elsewhere trips it too, which is the point of
// a guard that only ever fires on a bug.
//
// Emitted only for a design that forks, because only the fork term of the
// capacity is an over-approximation. Without a fork the bound is the
// one-live-slot-per-process invariant the scheduler has always held (a process
// is a single call chain with at most one resumable coroutine, and the
// scheduled flag dedupes wakes), so the check could never fire -- and it is not
// free: a compare, a branch and a cold block on every push cost ~40% sim time
// on a wake-heavy design (combchain) and ~25% of LLVM codegen time.
void Backend::emit_ring_full_check(llvm::IRBuilder<>& b, llvm::Value* head_ptr, llvm::Value* tail, llvm::Value* ring) {
    if (!ring_guard_wanted()) {
        return;
    }
    llvm::Function* fn   = b.GetInsertBlock()->getParent();
    const uint32_t  cap  = active_mask_ + 1;
    llvm::Value*    live = b.CreateSub(tail, b.CreateLoad(b.getInt32Ty(), head_ptr));
    auto*           bad  = llvm::BasicBlock::Create(ctx_, "ring.overflow", fn);
    auto*           ok   = llvm::BasicBlock::Create(ctx_, "ring.push", fn);
    b.CreateCondBr(b.CreateICmpUGE(live, b.getInt32(cap)), bad, ok);
    b.SetInsertPoint(bad);
    b.CreateCall(ring_overflow_fn(), { ring, b.getInt32(cap) });
    b.CreateUnreachable();
    b.SetInsertPoint(ok);
}

// The same guard for a buffer whose index IS its live count: the two `#0`
// batches (a bump-allocated `top`, drained whole and reset to zero) and a
// dynamic wait bucket (`.len`). `len` is a value the caller already had to load
// for the push itself, and the `design_forks()` gate is checked before anything
// is emitted, so a non-forking design pays exactly nothing.
//
// These were unguarded until the bound below stopped covering them. Without a
// fork every one of them is provably within capacity -- a process is a single
// call chain, so it can hold at most one `#0` and one wait entry at a time, and
// capacity is at least the process count. A fork child is an extra chain, which
// is what breaks that and why the check appears exactly when a fork does.
void Backend::emit_len_full_check(llvm::IRBuilder<>& b, llvm::Value* len, uint32_t cap, uint32_t which) {
    if (!ring_guard_wanted()) {
        return;
    }
    llvm::Function* fn  = b.GetInsertBlock()->getParent();
    auto*           bad = llvm::BasicBlock::Create(ctx_, "ring.overflow", fn);
    auto*           ok  = llvm::BasicBlock::Create(ctx_, "ring.push", fn);
    b.CreateCondBr(b.CreateICmpUGE(len, b.getInt32(cap)), bad, ok);
    b.SetInsertPoint(bad);
    b.CreateCall(ring_overflow_fn(), { b.getInt32(which), b.getInt32(cap) });
    b.CreateUnreachable();
    b.SetInsertPoint(ok);
}

// Whether the overflow guards are emitted at all. A fork child is an extra call
// chain, which is what breaks the one-live-slot-per-process bound the capacity
// rests on. A remote `disable` is the second reason: its force-wake makes a
// handle ready from outside the wake paths the bound was reasoned about, so the
// guard has to cover a design that disables but never forks -- which it did not
// when the gate was `design_forks()` alone.
bool Backend::ring_guard_wanted() {
    return design_forks() || design_uses_disable_ || design_uses_process_;
}

// Whether a park must be CLAIMABLE: the per-arm heap `#delay` record with its
// claim word, the scheduled flag a `#0` batch push sets, and a single-signal
// `wait` compacting itself out of its bucket. All three exist so a force-wake
// fires exactly once, and both force-wakers want them -- a remote `disable`
// (LRM 9.6.2) and `kill` / `disable fork` (LRM 9.7, 9.6.3).
bool Backend::park_claim_wanted() const {
    return design_uses_disable_ || design_uses_process_;
}

// Push a handle onto the active ring: queue[tail++ & mask] = handle.
void Backend::emit_active_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
    ensure_active_queue();
    auto*        i32  = b.getInt32Ty();
    llvm::Value* tl   = b.CreateLoad(i32, sym(active_tail_gv_));
    emit_ring_full_check(b, sym(active_head_gv_), tl, b.getInt32(kRingActive));
    llvm::Value* idx  = b.CreateAnd(tl, b.getInt32(active_mask_));
    llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, sym(active_gv_), { b.getInt32(0), idx });
    b.CreateStore(handle, slot);
    b.CreateStore(b.CreateAdd(tl, b.getInt32(1)), sym(active_tail_gv_));
}

// Push a handle onto the inactive queue: inactive[top++] = handle.
void Backend::emit_inactive_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
    ensure_inactive_queue();
    auto*        i32  = b.getInt32Ty();
    llvm::Value* sp   = b.CreateLoad(i32, sym(inactive_top_gv_));
    emit_len_full_check(b, sp, active_mask_ + 1, kRingInactive);
    llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, sym(inactive_gv_), { b.getInt32(0), sp });
    b.CreateStore(handle, slot);
    b.CreateStore(b.CreateAdd(sp, b.getInt32(1)), sym(inactive_top_gv_));
}

// The reactive ring (LRM 4.4.2.6): where a concurrent assertion's action
// block runs, and where a program `initial` (LRM 24.3) lives. Same shape,
// capacity and mask as the active ring, and mostly the same bound justifies
// it: a program `initial` is a Process, so it is already counted in the
// capacity, and a pend ramps one frame that the drain empties before time
// can advance.
//
// TODO: that last clause is no longer the whole story. An action block may
// now block (an event control, a fork, a #delay), so a frame pended at one
// instant can still be parked when the next attempt pends another, and the
// peak number of READY reactive frames is bounded by how many attempts fail
// while one is blocked -- which is not a static quantity. Overrunning the
// ring corrupts it silently. The same growable ring that join_any / join_none
// are waiting on is what fixes this; it is named here rather than guessed at,
// because sizing it to a constant would only move the cliff.
//
// Created once, lazily, and only for a design that has reactive code.
void Backend::ensure_reactive_queue() {
    if (reactive_gv_ != nullptr) {
        return;
    }
    ensure_active_queue(); // queue_arr_ty_ and active_mask_ are shared
    auto* i32         = llvm::Type::getInt32Ty(ctx_);
    reactive_gv_      = new llvm::GlobalVariable(env_->llmod, queue_arr_ty_, false,
                                                 llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantAggregateZero::get(queue_arr_ty_),
                                                 "clotho.reactive");
    reactive_head_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantInt::get(i32, 0), "clotho.reactive_head");
    reactive_tail_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantInt::get(i32, 0), "clotho.reactive_tail");
    auto* ptr_ty        = llvm::PointerType::getUnqual(ctx_);
    reactive_record_gv_ = new llvm::GlobalVariable(env_->llmod, ptr_ty, false,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantPointerNull::get(ptr_ty),
                                                   "clotho.reactive_record");
}

// The re-inactive batch (LRM 4.4.2.7): reactive code parked on a #0, the dual
// of the inactive queue. Promoted into the reactive ring at the same instant,
// after the ring drains (see the reactive drain in emit_active_run).
void Backend::ensure_re_inactive_queue() {
    if (re_inactive_gv_ != nullptr) {
        return;
    }
    ensure_reactive_queue(); // queue_arr_ty_ is shared by every queue
    auto* i32           = llvm::Type::getInt32Ty(ctx_);
    re_inactive_gv_     = new llvm::GlobalVariable(env_->llmod, queue_arr_ty_, false,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantAggregateZero::get(queue_arr_ty_),
                                                   "clotho.re_inactive");
    re_inactive_top_gv_ = new llvm::GlobalVariable(env_->llmod, i32, false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantInt::get(i32, 0), "clotho.re_inactive_top");
}

// Push a handle onto the reactive ring: reactive[tail++ & mask] = handle.
void Backend::emit_reactive_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
    ensure_reactive_queue();
    auto*        i32  = b.getInt32Ty();
    llvm::Value* tl   = b.CreateLoad(i32, sym(reactive_tail_gv_));
    emit_ring_full_check(b, sym(reactive_head_gv_), tl, b.getInt32(kRingReactive));
    llvm::Value* idx  = b.CreateAnd(tl, b.getInt32(active_mask_));
    llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, sym(reactive_gv_), { b.getInt32(0), idx });
    b.CreateStore(handle, slot);
    b.CreateStore(b.CreateAdd(tl, b.getInt32(1)), sym(reactive_tail_gv_));
}

// Push a handle onto the re-inactive batch: re_inactive[top++] = handle.
void Backend::emit_re_inactive_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
    ensure_re_inactive_queue();
    auto*        i32  = b.getInt32Ty();
    llvm::Value* sp   = b.CreateLoad(i32, sym(re_inactive_top_gv_));
    emit_len_full_check(b, sp, active_mask_ + 1, kRingReInactive);
    llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, sym(re_inactive_gv_), { b.getInt32(0), sp });
    b.CreateStore(handle, slot);
    b.CreateStore(b.CreateAdd(sp, b.getInt32(1)), sym(re_inactive_top_gv_));
}

// The body's LEXICAL home region: whether a wake of THIS body comes back in
// Reactive -- a concurrent assertion's action block, a program `initial` (LRM
// 24.3), a program's continuous assign or port copy (LRM 24.3.1), or a
// subroutine / fork child that only such code reaches. All live in the
// reactive region set.
//
// Valid only where the home is NOT thread-carried. A body reached from both
// reactive-home and ordinary code has no lexical home at all, and the answer
// here is not the one to use: every consumer (parked_handle, emit_by_home)
// tests body_home_is_runtime_ first, and home_ptr consults this only to seed a
// body that has no caller to inherit from.
bool Backend::reactive_home() const {
    return in_program_body_ || in_reactive_home_body_ || in_reactive_home_fn_;
}

// OR the reactive-home tag onto a handle whose home is known from the PROCESS
// rather than from the body being lowered. One caller needs that: the static
// subscription of a reactive-home continuous process, recorded by the seed loop
// (emit_active_run) from outside any body. Everything else parks through
// parked_handle, which asks the body.
//
// Gated on design_uses_reactive_ so the tag can only be emitted where
// emit_wake_push's masking path is emitted too; see parked_handle.
llvm::Value* Backend::tag_reactive_home(llvm::IRBuilder<>& b, llvm::Value* handle, bool reactive) {
    if (!reactive || !design_uses_reactive_) {
        return handle;
    }
    auto* i64 = b.getInt64Ty();
    return b.CreateIntToPtr(
      b.CreateOr(b.CreatePtrToInt(handle, i64), llvm::ConstantInt::get(i64, kReactiveHomeTag)),
      llvm::PointerType::getUnqual(ctx_));
}

// The running THREAD's home region as an i8 -- 0 for Active, kReactiveHomeTag
// for Reactive, so it can be OR'd straight into a handle with no select --
// spilled into the coroutine frame.
//
// This is LRM 24.3.1's rule made operational. The clause puts the region on the
// thread, not on the body: "Once a program process starts a thread of execution,
// all subsequent blocking statements in that thread are scheduled in the
// Reactive region. This includes subroutine code called by the thread, even if
// the subroutine code is declared in a module, package, or interface." Two
// things make a body need the slot rather than a folded constant, and they are
// the same phenomenon seen from two sides:
//
//   - it is SHARED (build_shared_home_fns): both a reactive-home thread and an
//     ordinary one can be executing it, so only the caller knows. Seeded from
//     home_arg_, which is the caller's answer, passed as a ramp parameter.
//   - it runs an `expect` (LRM 16.17), which moves the thread into Reactive
//     mid-body and leaves it there. That is a STORE to this slot
//     (lower_expect_wait), not a different mechanism.
//
// A shared body that also runs an `expect` needs no extra thought: it seeds
// from the caller and the expect overwrites it, later store wins, which is
// exactly "all subsequent blocking statements in that thread".
//
// Created only for such a body (body_home_is_runtime_), so no other design pays
// a load per park.
//
// In the frame rather than in a per-process global: a frame alloca is
// per-instance for free, which a global indexed by process is not under
// module dedup (see the anchor bookkeeping in build_sched_flags), and it
// serves a task body on the same terms as a process body. Hoisted to
// coro.begin, like the awaiter and the fork record, because it must outlive
// every suspend.
llvm::Value* Backend::home_ptr() {
    if (home_ptr_ == nullptr) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        home_ptr_ = ab.CreateAlloca(ab.getInt8Ty(), nullptr, "home_region");
        // The caller's answer when there is a caller -- a task or fork-child
        // ramp -- and the lexical one for a PROCESS, which is a thread root and
        // has nobody to ask. A body that never migrates then behaves exactly as
        // the folded form would.
        ab.CreateStore(home_arg_ != nullptr
                         ? home_arg_
                         : static_cast<llvm::Value*>(ab.getInt8(reactive_home() ? kReactiveHomeTag : 0)),
                       home_ptr_);
    }
    return home_ptr_;
}

// This thread's home region right now, as an i8. Folded to a constant unless
// the body carries its home at run time; a coroutine reads the frame slot,
// which is where a value has to live to survive a suspend, and a plain FUNCTION
// -- which cannot suspend -- reads its incoming parameter directly.
//
// A shared body always HAS that parameter: design_uses_home_arg_ is exactly
// "some body in this design is shared", so wherever this can be reached with
// body_home_is_runtime_ set for sharing, the signature carries it. An `expect`
// body without the parameter is a coroutine by construction (16.17 blocks), so
// it took the frame path.
llvm::Value* Backend::current_home(llvm::IRBuilder<>& b) {
    if (!body_home_is_runtime_) {
        return b.getInt8(reactive_home() ? kReactiveHomeTag : 0);
    }
    return in_coroutine_body() ? b.CreateLoad(b.getInt8Ty(), home_ptr()) : home_arg_;
}

// Whether the body being lowered is a coroutine. Asked of coro_begin_ AND of
// whose function it belongs to, because the pointer is never cleared between
// bodies: a plain function lowered after a task would otherwise look like a
// coroutine and hoist its allocas into the task's entry block. The same
// comparison the mutator-pointer anchor asserts.
bool Backend::in_coroutine_body() const {
    return coro_begin_ != nullptr && coro_begin_->getFunction() == fn_;
}

// The home region to hand a callee: whatever this thread's is at the call, so a
// task that calls a task passes its own CURRENT answer down rather than its
// lexical one. That is what makes 24.3.1's "inherits the scheduling region of
// the thread that calls it" transitive -- by induction over the call chain, and
// over an `expect` too, since the store lands before any later call reads it.
// Null when the parameter is not in the signature, which is every design with
// no shared body.
llvm::Value* Backend::callee_home() {
    return design_uses_home_arg_ ? current_home(b_) : nullptr;
}

// The value to record when parking this coroutine for a later wake. Folded at
// lowering time unless the home travels with the thread (home_ptr), so ordinary
// code emits the bare handle and every wake of it reads Active with no tag ever
// materialized.
llvm::Value* Backend::parked_handle(llvm::IRBuilder<>& b, llvm::Value* handle) {
    // Tie the tag to the exact flag emit_wake_push tests before masking it
    // off. A tag emitted while that path is compiled out would never be
    // stripped, and the ring would hold a handle two bytes past its frame --
    // a crash, not merely a wrong region. Both sides read design_uses_reactive_
    // so the pair cannot drift; a body can only be reactive-home, or shared, or
    // run an expect, in a design where this is already true.
    if (!design_uses_reactive_) {
        return handle;
    }
    if (!body_home_is_runtime_) {
        return reactive_home() ? tag_reactive_home(b, handle, /*reactive=*/true) : handle;
    }
    auto*        i64 = b.getInt64Ty();
    llvm::Value* tag = b.CreateZExt(current_home(b), i64);
    return b.CreateIntToPtr(b.CreateOr(b.CreatePtrToInt(handle, i64), tag), llvm::PointerType::getUnqual(ctx_));
}

// Whether `ir` executes an `expect`, i.e. whether its home region can change
// while it runs. Direct occurrences only: an expect inside a task would need
// the region to travel up the call chain to the process's frame, which this
// per-frame slot cannot do, and the `expect` lowering refuses that shape.
bool Backend::ir_has_expect_wait(const Ir& ir) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        if (ir.tag(static_cast<Index>(k)) == Inst::Tag::ExpectWait) {
            return true;
        }
    }
    return false;
}

// Whether `ir` holds a `#delay`. Asked of a reactive-home body so run() knows
// to create the re-inactive batch before anything lowers: a `#0` in reactive
// code parks there rather than in the inactive queue (emit_zero_delay_push),
// and lower_finish has to see the batch to empty it.
bool Backend::ir_has_delay(const Ir& ir) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        if (ir.tag(static_cast<Index>(k)) == Inst::Tag::Delay) {
            return true;
        }
    }
    return false;
}

// Wake a parked handle into the ring it named when it parked. Branch-free
// like the rest of the scheduler: the two rings share queue_arr_ty_ and
// active_mask_, so only the array and the tail counter differ and both are
// selected. Replaces emit_active_push at every site that wakes somebody
// else's handle -- the two wait-queue scans, the wheel timer trampoline and
// the task / fork-join epilogue. The seed loop still pushes directly: it
// creates the handles, so it knows their region without asking.
void Backend::emit_wake_push(llvm::IRBuilder<>& b, llvm::Value* parked) {
    if (!design_uses_reactive_) {
        // No body in this design can be reactive-home, so nothing was ever
        // tagged and the reactive ring does not exist. Same code as before.
        emit_active_push(b, parked);
        return;
    }
    ensure_active_queue();
    ensure_reactive_queue();
    auto*        i64  = b.getInt64Ty();
    auto*        i32  = b.getInt32Ty();
    llvm::Value* addr = b.CreatePtrToInt(parked, i64);
    llvm::Value* home = b.CreateICmpEQ(b.CreateAnd(addr, llvm::ConstantInt::get(i64, 3)),
                                       llvm::ConstantInt::get(i64, kReactiveHomeTag));
    llvm::Value* bare = b.CreateAnd(addr, llvm::ConstantInt::get(i64, ~kReactiveHomeTag));
    llvm::Value* hdl  = b.CreateIntToPtr(b.CreateSelect(home, bare, addr),
                                         llvm::PointerType::getUnqual(ctx_));
    llvm::Value* arr  = b.CreateSelect(home, sym(reactive_gv_), sym(active_gv_));
    llvm::Value* tp   = b.CreateSelect(home, sym(reactive_tail_gv_), sym(active_tail_gv_));
    llvm::Value* hp   = b.CreateSelect(home, sym(reactive_head_gv_), sym(active_head_gv_));
    llvm::Value* tl   = b.CreateLoad(i32, tp);
    // The ring is chosen at run time here, so the guard's selector is too: the
    // fatal names the ring the handle was actually headed for.
    emit_ring_full_check(b, hp, tl,
                         b.CreateSelect(home, b.getInt32(kRingReactive), b.getInt32(kRingActive)));
    llvm::Value* idx  = b.CreateAnd(tl, b.getInt32(active_mask_));
    llvm::Value* slot = b.CreateInBoundsGEP(queue_arr_ty_, arr, { b.getInt32(0), idx });
    b.CreateStore(hdl, slot);
    b.CreateStore(b.CreateAdd(tl, b.getInt32(1)), tp);
}

// A #0 parks in the inactive queue, or in its reactive dual when the body
// running it belongs to the reactive region set (LRM 4.4.2.7): a suspended
// action block, or a suspended program `initial`, has to come back in
// Reactive, not Active, or the rest of the region order is wrong. The batch
// promotes into the reactive ring untagged, exactly as it is pushed here --
// a ring entry is a plain handle, never a parked one.
void Backend::emit_zero_delay_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
    // A batch entry has no claim field, and it does not need one: the batch's own
    // promotion later in this instant IS the wake. What it does need is to say so
    // -- a `disable` arriving meanwhile must stand down rather than push a second
    // copy of a handle the promotion will deliver, which resumed the activation
    // twice and, once it had finished, jumped through a dead coroutine.
    //
    // The scheduled flag is exactly that statement, and setting it here is also
    // the truth: a `#0`-parked activation is already scheduled. The promotion
    // does not consult the flag, so it still lands.
    if (park_claim_wanted() && sched_ptr_ != nullptr) {
        b.CreateStore(b.getInt64(1 | unclassified_trig_bits()), sched_ptr_);
    }
    emit_by_home(b, [&] { emit_re_inactive_push(b, handle); }, [&] { emit_inactive_push(b, handle); });
}

// Make a freshly ramped coroutine ready in the region the body doing the
// spawning belongs to. Used by a `join_none` fork, whose children must NOT run
// at the fork (LRM 9.3.2) and so are queued rather than resumed inline. The
// handle is plain, not parked: a ring entry never carries the home tag, and a
// child of a reactive body belongs in the reactive ring for the same reason its
// parent's `#0` does.
void Backend::emit_region_push(llvm::IRBuilder<>& b, llvm::Value* handle) {
    emit_by_home(b, [&] { emit_reactive_push(b, handle); }, [&] { emit_active_push(b, handle); });
}

void Backend::emit_active_run(llvm::Value* state, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
    ensure_active_queue();
    auto* ptr_ty        = llvm::PointerType::getUnqual(ctx_);
    auto* i32           = b_.getInt32Ty();
    auto* proc_fnty     = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
    const bool uses_vcd = design_uses_vcd();

    // Init: empty the stack, then create every process (the ramp call builds
    // the frame and stops at the initial suspend, no body yet).
    //
    // An always block with a sensitivity list is subscribed to its signals'
    // static queues right here, by plain stores of its ramp handle — never by
    // resuming it (resuming would defeat coro-elide of the other processes).
    // It is left parked at the initial suspend and NOT enqueued; the first
    // blocking store that matches its edge wakes it, resuming straight into its
    // body. Subscribing before the drain guarantees every static slot is filled
    // before any servicing scans it. Everything else (initial blocks,
    // sensitivity-less always / continuous assigns) is enqueued to run during
    // the drain.
    b_.CreateStore(b_.getInt32(0), sym(active_head_gv_));
    b_.CreateStore(b_.getInt32(0), sym(active_tail_gv_));
    b_.CreateStore(b_.getInt64(0), sym(osc_count_gv_));
    const bool has_inactive = inactive_gv_ != nullptr; // some #0 was lowered
    if (has_inactive) {
        b_.CreateStore(b_.getInt32(0), sym(inactive_top_gv_));
    }
    // The reactive ring and its batch, when the design has reactive code.
    if (reactive_gv_ != nullptr) {
        b_.CreateStore(b_.getInt32(0), sym(reactive_head_gv_));
        b_.CreateStore(b_.getInt32(0), sym(reactive_tail_gv_));
    }
    if (re_inactive_gv_ != nullptr) {
        b_.CreateStore(b_.getInt32(0), sym(re_inactive_top_gv_));
    }
    // The array-NBA record buffers are process-global; clear any state a prior
    // simulation in this process left staged (e.g. a $finish between staging and
    // the NBA commit) so this run starts empty.
    if (!array_nba_sigs_.empty() || !agg_nba_sigs_.empty() || !handle_nba_sigs_.empty() ||
        !string_nba_sigs_.empty()) {
        b_.CreateCall(nba_arr_reset_fn(), {});
    }
    // Same for the deferred intra-assignment record pools — reclaim any records
    // a prior $finish left armed on the wheel.
    if (!intra_nba_sigs_.empty()) {
        b_.CreateCall(intra_reset_fn(), {});
    }
    const auto all = mod_.processes();

    // Pre-init pass first (LRM 6.8): apply variable declaration initializers
    // before any Initial / Always process is created or subscribed. A PreInit
    // body has no timing, so resuming it once runs it to completion; its stores
    // generate no events (see lower_process), so the always blocks created
    // below subscribe to already-settled values. Run before the seed loop so no
    // wait queue is scanned before priming.
    for (uint32_t g = 0; g < procs.size(); ++g) {
        if (all[groups_[g].front()].activation != Activation::PreInit) {
            continue;
        }
        llvm::Value* hdl = b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    }

    // Time zero has no preceding wheel tick, so its Preponed sample happens
    // here rather than in the post-tick block. After PreInit, so a sample sees
    // declaration initializers; before the seed loop, so it precedes every
    // body of the first step. This is also the default sampled value LRM
    // 16.5.1 wants for a value-change function called at or before the first
    // clocking event.
    emit_preponed_run(state, wheel, procs);

    // One handle per fusion group; a group's activation facts come from its
    // first member (identical across fused members by the group key).
    std::vector<llvm::Value*> hdls(procs.size(), nullptr);
    for (uint32_t g = 0; g < procs.size(); ++g) {
        const uint32_t k = groups_[g].front();
        const Process& p = all[k];
        if (p.activation == Activation::PreInit) {
            continue; // already run above
        }
        if (p.activation == Activation::Preponed) {
            continue; // run once per timestep from the post-tick block
        }
        if (p.activation == Activation::Observed) {
            continue; // run explicitly at each settled point (emit_observed_run)
        }
        if (p.activation == Activation::ReNba) {
            continue; // run explicitly in the Re-NBA region (emit_renba_run)
        }
        if (p.activation == Activation::Final) {
            continue; // deferred to emit_finals_run at end of simulation
        }
        // A thunk group is not ramped (no frame): its "handle" is the thunk
        // function pointer with the low bit set, so the drain calls it
        // directly instead of resuming a coroutine. Function addresses are
        // aligned, so bit 0 is a free tag; coro handles (rt_alloc) leave it
        // clear. The tagged pointer flows through subscription / self_start
        // push unchanged.
        llvm::Value* hdl = thunk_group_[g]
          ? b_.CreateIntToPtr(
              b_.CreateOr(b_.CreatePtrToInt(sym(procs[g]), b_.getInt64Ty()), b_.getInt64(1)), ptr_ty)
          : b_.CreateCall(proc_fnty, sym(procs[g]), { state, wheel });
        hdls[g]          = hdl;
        if (p.activation == Activation::Always && !p.wait_events.empty()) {
            // Subscribed in the topo-ordered pass below.
            //
            // A continuously-active process (continuous assign / port copy /
            // always_comb) also runs once now, so it settles from the pre-init
            // values rather than waiting for an input edge that a declaration
            // initializer never generates. It runs body-first then parks on its
            // (subscribed-below) wait, so later input changes still wake it.
            //
            // Mark it scheduled as it is enqueued: a seed-time store from a
            // process draining earlier in the same first drain (an initial
            // block) services this static subscription and would re-push the
            // already-queued handle without the flag, so N such processes
            // overflow the nproc-sized active queue. The flag is cleared
            // when the body reaches its wait-suspend (lower_process always-loop).
            if (p.self_start) {
                llvm::Constant* flag = sched_flag_ptr(k);
                // The time-zero settle run must evaluate every member.
                b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), flag);
                // A reactive-home group -- a program's continuous assign (LRM
                // 24.3.1) -- settles in slot 0's REACTIVE region rather than its
                // Active one, so its stores land after the design has settled and
                // design readers of what it drives are woken into the Active
                // region of the next iteration of 4.4.2's loop. That is 24.3.2's
                // own sentence. A ring entry is a bare handle either way: the
                // home tag belongs to a PARKED handle, and the seed loop created
                // this one so it knows the ring without reading a tag back.
                if (p.reactive_home) {
                    emit_reactive_push(b_, hdl);
                } else {
                    emit_active_push(b_, hdl);
                }
            }
        } else if (p.activation == Activation::Reactive) {
            // A program `initial` (LRM 24.3) starts in the Reactive region, so
            // it is seeded onto the reactive ring instead of the active one.
            // Its body keeps itself there across every suspend (emit_suspend).
            //
            // Mark it scheduled as it is enqueued, for the reason the self_start
            // arm above gives -- it is QUEUED from here, so anything that pushes
            // it again before it runs double-queues it -- which for this arm is
            // not a static subscription (a sensitivity-less body has none) but a
            // FORCE-WAKE. `$exit` (LRM 24.7) names every `initial` of its program
            // instance by static control block, so a `fork ... join` child that
            // exits during its parent's FIRST activation force-wakes a parent that
            // is running on the stack rather than parked: its park record is null,
            // so emit_node_force_wake falls to the flag, and with the flag clear it
            // would push the handle of a coroutine that is about to reach its final
            // suspend and be destroyed by the resumer -- the drain would then resume
            // a freed frame. The flag is cleared by the body itself before each
            // park (lower_wait, emit_delay), so "set while running, clear while
            // parked" holds for this arm exactly as it does for an always loop.
            b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), sched_flag_ptr(k));
            emit_reactive_push(b_, hdl);
        } else {
            emit_active_push(b_, hdl);
        }
    }

    // Static subscriptions, in topological order of the blocking-write wake
    // graph (compute_subscription_order): bucket entry order is subscription
    // order, the FIFO drain pops in bucket order, so a settle wave runs
    // producers before consumers and each acyclic cascade settles in one
    // wave. A separate pass so only wake order changes — creation and
    // enqueue order above stay declaration order. All subscriptions still
    // land before the drain, so no scan reads an empty slot.
    // A fused group subscribes once, at its earliest member's rank, with the
    // group's canonical event set and the group's flag/handle.
    std::vector<bool> subscribed(procs.size(), false);
    for (const uint32_t k : sub_order_) {
        const uint32_t g = group_of_[k];
        const uint32_t f = groups_[g].front();
        if (subscribed[g] || all[f].activation != Activation::Always || all[f].wait_events.empty()) {
            continue;
        }
        subscribed[g]        = true;
        llvm::Constant* flag = sched_flag_ptr(f);
        // A reactive-home group needs no separate subscription path: the
        // predicate above selects it like any other Always group, and the ring
        // is picked at WAKE time from the handle's home tag (emit_wake_push).
        // What it does need is the tag ON the handle recorded here. A static
        // subscription records a handle exactly once, from this loop, and the
        // always-loop's park never re-records it (emit_wait_suspend only
        // suspends) -- so unlike an intra-body wait, which parks through
        // parked_handle at each `@`, this is the only place the home can be
        // stated. Untagged, every wake of a program's continuous assign would
        // push it onto the active ring, which is the bug this phase fixes.
        llvm::Value* sub_hdl = tag_reactive_home(b_, hdls[g], all[f].reactive_home);
        for (const WaitEvent& w : group_events_[g]) {
            emit_static_subscribe(w.signal, static_cast<uint32_t>(w.edge), sub_hdl, flag);
        }
    }

    auto* fn      = b_.GetInsertBlock()->getParent();
    auto* check   = llvm::BasicBlock::Create(ctx_, "active.check", fn);
    auto* body    = llvm::BasicBlock::Create(ctx_, "active.body", fn);
    auto* destroy = llvm::BasicBlock::Create(ctx_, "active.destroy", fn);
    auto* cont    = llvm::BasicBlock::Create(ctx_, "active.cont", fn);
    auto* wcheck  = llvm::BasicBlock::Create(ctx_, "wheel.check", fn);
    auto* wtick   = llvm::BasicBlock::Create(ctx_, "wheel.tick", fn);
    auto* wdone   = llvm::BasicBlock::Create(ctx_, "wheel.done", fn);
    // Any thunk group present means the active queue can carry tagged thunk
    // pointers, so the drain must discriminate; a coro-only design keeps the
    // straight resume path (no per-pop tag test).
    const bool has_thunk = std::any_of(thunk_group_.begin(), thunk_group_.end(),
                                        [](bool t) { return t; });
    auto* thunk_fnty = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);
    // NBA region: only when the design has nonblocking assignments. Entered when
    // the active queue drains; if any update is staged it commits them all and
    // re-drains the active queue at the SAME instant; otherwise it falls through
    // to the wheel. So active and NBA iterate until both settle before time moves.
    const bool has_nba = !array_nba_sigs_.empty() || !agg_nba_sigs_.empty() || !handle_nba_sigs_.empty() ||
                         !string_nba_sigs_.empty() || !block_commits_.empty() || design_uses_nb_events_ ||
                         !intra_nba_sigs_.empty();
    auto*      ncheck  = has_nba ? llvm::BasicBlock::Create(ctx_, "nba.check", fn) : wcheck;
    auto*      ncommit = has_nba ? llvm::BasicBlock::Create(ctx_, "nba.commit", fn) : nullptr;
    // Inactive region: only when the design has a #0. When the active queue
    // drains, promote the whole inactive batch back into the (now empty)
    // active queue and re-drain at the SAME instant — before NBA. So active
    // and inactive iterate until both settle, then NBA, then the wheel.
    auto* icheck   = has_inactive ? llvm::BasicBlock::Create(ctx_, "inactive.check", fn) : ncheck;
    auto* ipromote = has_inactive ? llvm::BasicBlock::Create(ctx_, "inactive.promote", fn) : nullptr;
    b_.CreateBr(check);

    // Drain the active ring: while head != tail, pop active[head++ & mask]
    // and resume it.
    b_.SetInsertPoint(check);
    llvm::Value* hd = b_.CreateLoad(i32, sym(active_head_gv_));
    llvm::Value* tl = b_.CreateLoad(i32, sym(active_tail_gv_));
    // The $finish test sits on the queue-EMPTY edge, not after each resume: a
    // cooperative finish empties the active queue itself (lower_finish), so
    // the wave it was in ends on the next trip round this test and control
    // arrives here. That keeps the per-activation path free of it — worth
    // caring about because sim_drain is codegen'd at -O0/FastISel, where a
    // load and a branch on the hot path measured +2.2% on picorv32.
    // Skipping straight to wdone also leaves any staged NBAs uncommitted,
    // which is what the longjmp did.
    // A design with no $finish anywhere cannot set the flag, so it gets the
    // plain edge and pays nothing at all.
    if (design_has_finish()) {
        auto* fincheck = llvm::BasicBlock::Create(ctx_, "active.finished", fn);
        b_.CreateCondBr(b_.CreateICmpNE(hd, tl), body, fincheck);
        b_.SetInsertPoint(fincheck);
        b_.CreateCondBr(
          b_.CreateICmpNE(b_.CreateLoad(b_.getInt8Ty(), finishing_gv()), b_.getInt8(0)), wdone,
          icheck);
    } else {
        b_.CreateCondBr(b_.CreateICmpNE(hd, tl), body, icheck);
    }

    b_.SetInsertPoint(body);
    // Zero-delay oscillation guard: count activations at the current instant
    // (reset when the wheel advances). A design whose active queue never drains
    // at one time step (two `always @*` writing a shared variable each is
    // sensitive to, LRM 9.4.2.2) would hang forever; past the limit, abort loud.
    // The limit is far above any real single-instant settle wave.
    {
        auto* osc_ok    = llvm::BasicBlock::Create(ctx_, "active.osc_ok", fn);
        auto* osc_abort = llvm::BasicBlock::Create(ctx_, "active.osc_abort", fn);
        auto* i64       = b_.getInt64Ty();
        llvm::Value* n  = b_.CreateAdd(b_.CreateLoad(i64, sym(osc_count_gv_)), b_.getInt64(1));
        b_.CreateStore(n, sym(osc_count_gv_));
        b_.CreateCondBr(b_.CreateICmpUGT(n, b_.getInt64(kOscillationLimit)), osc_abort, osc_ok);
        b_.SetInsertPoint(osc_abort);
        b_.CreateCall(oscillation_abort_fn(), { wheel });
        b_.CreateUnreachable();
        b_.SetInsertPoint(osc_ok);
    }
    b_.CreateStore(b_.CreateAdd(hd, b_.getInt32(1)), sym(active_head_gv_));
    llvm::Value* idx  = b_.CreateAnd(hd, b_.getInt32(active_mask_));
    llvm::Value* slot = b_.CreateInBoundsGEP(queue_arr_ty_, sym(active_gv_), { b_.getInt32(0), idx });
    llvm::Value* hdl  = b_.CreateLoad(ptr_ty, slot);
    if (has_thunk) {
        // Low bit set => a tagged thunk pointer: call it directly (no frame,
        // nothing to destroy). Clear => a coroutine handle: resume/done/
        // destroy as before.
        auto* thunk_bb = llvm::BasicBlock::Create(ctx_, "active.thunk", fn);
        auto* coro_bb  = llvm::BasicBlock::Create(ctx_, "active.coro", fn);
        auto* addr     = b_.CreatePtrToInt(hdl, b_.getInt64Ty());
        b_.CreateCondBr(b_.CreateTrunc(addr, b_.getInt1Ty()), thunk_bb, coro_bb);

        b_.SetInsertPoint(thunk_bb);
        auto* fnp = b_.CreateIntToPtr(b_.CreateAnd(addr, ~static_cast<uint64_t>(1)), ptr_ty);
        b_.CreateCall(thunk_fnty, fnp, { state, wheel });
        b_.CreateBr(cont);

        b_.SetInsertPoint(coro_bb);
    }
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
    llvm::Value* fin = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { hdl });
    b_.CreateCondBr(fin, destroy, cont);

    b_.SetInsertPoint(destroy);
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    b_.CreateBr(check);

    if (has_inactive) {
        // Active queue is empty here. If anything parked on a #0, move the
        // whole batch into the active queue (it is empty, so a plain copy)
        // and re-drain. Promoting the batch all at once is what makes every
        // #0 process at this instant become active together, per the LRM.
        b_.SetInsertPoint(icheck);
        llvm::Value* n = b_.CreateLoad(i32, sym(inactive_top_gv_));
        b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), ipromote, ncheck);

        // for (i = 0; i < n; ++i) active[i] = inactive[i];
        // head = 0, tail = n (ring empty here, n <= cap); inactive_top = 0;
        // then loop back to the drain.
        auto* phead = llvm::BasicBlock::Create(ctx_, "inactive.copy", fn);
        auto* pbody = llvm::BasicBlock::Create(ctx_, "inactive.copy.body", fn);
        auto* pdone = llvm::BasicBlock::Create(ctx_, "inactive.copy.done", fn);
        b_.SetInsertPoint(ipromote);
        b_.CreateBr(phead);

        b_.SetInsertPoint(phead);
        auto* i = b_.CreatePHI(i32, 2);
        i->addIncoming(b_.getInt32(0), ipromote);
        b_.CreateCondBr(b_.CreateICmpULT(i, n), pbody, pdone);

        b_.SetInsertPoint(pbody);
        llvm::Value* src = b_.CreateInBoundsGEP(queue_arr_ty_, sym(inactive_gv_), { b_.getInt32(0), i });
        llvm::Value* dst = b_.CreateInBoundsGEP(queue_arr_ty_, sym(active_gv_), { b_.getInt32(0), i });
        b_.CreateStore(b_.CreateLoad(ptr_ty, src), dst);
        i->addIncoming(b_.CreateAdd(i, b_.getInt32(1)), pbody);
        b_.CreateBr(phead);

        b_.SetInsertPoint(pdone);
        b_.CreateStore(b_.getInt32(0), sym(active_head_gv_));
        b_.CreateStore(n, sym(active_tail_gv_));
        b_.CreateStore(b_.getInt32(0), sym(inactive_top_gv_));
        b_.CreateBr(check);
    }

    if (has_nba) {
        // Any staged update? OR the block chain head and the deferred array-write
        // queue's length; if so, commit them and loop back to drain the processes
        // the commits just woke.
        b_.SetInsertPoint(ncheck);
        llvm::Value* any = emit_nba_any_staged();
        b_.CreateCondBr(any, ncommit, wcheck);

        b_.SetInsertPoint(ncommit);
        emit_nba_commit(state, wheel);
        b_.CreateBr(check);
    }

    // Queue empty: this instant has fully settled.
    b_.SetInsertPoint(wcheck);
    // Observed region (LRM 4.4.2.5): run the processes that evaluate property
    // expressions, ahead of the flushes below that mature this instant's
    // reports. Emitted like emit_preponed_run and for the same reason -- an
    // Observed process is in no queue and no event wakes it.
    if (has_observed()) {
        emit_observed_run(state, wheel, procs);
        // An Observed body's stores are ordinary design writes, so it can wake
        // an Active process. The LRM's time-slot loop always re-enters the
        // highest-priority non-empty region, so settle the active set again
        // before Reactive rather than running reactive code against a design
        // state that has not converged.
        //
        // Re-running the bodies on the way back through here is what that loop
        // does, and it is harmless: a monitor triggers off values Preponed
        // sampled, which no active code can change within the step, so the
        // second pass sees no clocking event and does nothing. A body that
        // keeps waking the same process is a zero-delay oscillation, and the
        // active drain's guard counts those activations and aborts.
        auto*        settled = llvm::BasicBlock::Create(ctx_, "observed.settled", fn);
        llvm::Value* ohd     = b_.CreateLoad(i32, sym(active_head_gv_));
        llvm::Value* otl     = b_.CreateLoad(i32, sym(active_tail_gv_));
        b_.CreateCondBr(b_.CreateICmpNE(ohd, otl), check, settled);
        b_.SetInsertPoint(settled);
    }
    // Observed region (LRM 4.4.2.4): emit every report queued while the
    // Active/NBA regions settled this instant — unique/priority case
    // violations (LRM 12.4.2 / 12.5.3) and observed (`#0`) deferred assertion
    // reports (LRM 16.4). Runs before the Postponed region below, per the LRM
    // region order. The flushes no-op when nothing is queued, so these calls
    // per settled instant are cheap.
    if (design_uses_observed_) {
        b_.CreateCall(observed_flush_fn(), {});
    }
    if (design_uses_defer_) {
        b_.CreateCall(defer_flush_observed_fn(), {});
    }
    // Reactive region (LRM 4.4.2.6) and its Re-Inactive dual (4.4.2.7): drain
    // the action-block handles the Observed region just pended, exactly as the
    // active drain works its own ring. When the ring empties, promote the whole
    // re-inactive batch into it and re-drain, so every #0 in reactive code
    // becomes reactive again together; when both settle, fall through to
    // Postponed. Emitted only for a design that has reactive code.
    if (design_uses_reactive_) {
        auto* rcheck    = llvm::BasicBlock::Create(ctx_, "reactive.check", fn);
        auto* rbody     = llvm::BasicBlock::Create(ctx_, "reactive.body", fn);
        auto* rdestroy  = llvm::BasicBlock::Create(ctx_, "reactive.destroy", fn);
        auto* rcont     = llvm::BasicBlock::Create(ctx_, "reactive.cont", fn);
        auto* rdone     = llvm::BasicBlock::Create(ctx_, "reactive.done", fn);
        const bool has_re_inactive = re_inactive_gv_ != nullptr; // some #0 in reactive code
        // Re-NBA (LRM 4.4.2.8), the reactive region set's dual of the NBA
        // region. Gated on has_nba exactly as nba.check/nba.commit are: a
        // design that stages no nonblocking updates emits none of it.
        auto* rncheck   = (has_nba || has_renba()) ? llvm::BasicBlock::Create(ctx_, "re_nba.check", fn) : rdone;
        auto* rncommit  = has_nba ? llvm::BasicBlock::Create(ctx_, "re_nba.commit", fn) : nullptr;
        auto* ripromote = has_re_inactive
          ? llvm::BasicBlock::Create(ctx_, "re_inactive.promote", fn) : nullptr;
        // Falls through to Re-NBA, not straight to done: the batch settling is
        // what makes the update region the next non-empty one.
        auto* richeck   = has_re_inactive
          ? llvm::BasicBlock::Create(ctx_, "re_inactive.check", fn) : rncheck;
        b_.CreateBr(rcheck);

        b_.SetInsertPoint(rcheck);
        llvm::Value* rhd = b_.CreateLoad(i32, sym(reactive_head_gv_));
        llvm::Value* rtl = b_.CreateLoad(i32, sym(reactive_tail_gv_));
        // The $finish test sits on the queue-empty edge for the reason it does
        // in the active drain: a cooperative finish empties this ring itself
        // (lower_finish), so the wave it was in ends on the next trip here.
        if (design_has_finish()) {
            auto* rfin = llvm::BasicBlock::Create(ctx_, "reactive.finished", fn);
            b_.CreateCondBr(b_.CreateICmpNE(rhd, rtl), rbody, rfin);
            b_.SetInsertPoint(rfin);
            b_.CreateCondBr(
              b_.CreateICmpNE(b_.CreateLoad(b_.getInt8Ty(), finishing_gv()), b_.getInt8(0)), wdone,
              richeck);
        } else {
            b_.CreateCondBr(b_.CreateICmpNE(rhd, rtl), rbody, richeck);
        }

        b_.SetInsertPoint(rbody);
        // Reactive activations spend the same per-instant oscillation budget as
        // active ones: an action block that writes a signal its own assertion
        // samples can ping-pong through Observed just as two always blocks can
        // through Active, and the counter is reset by the same wheel tick.
        {
            auto*        osc_ok    = llvm::BasicBlock::Create(ctx_, "reactive.osc_ok", fn);
            auto*        osc_abort = llvm::BasicBlock::Create(ctx_, "reactive.osc_abort", fn);
            auto*        i64       = b_.getInt64Ty();
            llvm::Value* n         = b_.CreateAdd(b_.CreateLoad(i64, sym(osc_count_gv_)), b_.getInt64(1));
            b_.CreateStore(n, sym(osc_count_gv_));
            b_.CreateCondBr(b_.CreateICmpUGT(n, b_.getInt64(kOscillationLimit)), osc_abort, osc_ok);
            b_.SetInsertPoint(osc_abort);
            b_.CreateCall(oscillation_abort_fn(), { wheel });
            b_.CreateUnreachable();
            b_.SetInsertPoint(osc_ok);
        }
        b_.CreateStore(b_.CreateAdd(rhd, b_.getInt32(1)), sym(reactive_head_gv_));
        llvm::Value* ridx  = b_.CreateAnd(rhd, b_.getInt32(active_mask_));
        llvm::Value* rslot = b_.CreateInBoundsGEP(queue_arr_ty_, sym(reactive_gv_), { b_.getInt32(0), ridx });
        llvm::Value* rhdl  = b_.CreateLoad(ptr_ty, rslot);
        if (has_thunk) {
            // Same tagged-pointer discipline as the active drain. Nothing pends
            // a thunk today (a pend always ramps a frame), but the tag is a
            // property of the pointers this design builds rather than of the
            // ring it puts them in, so the two rings stay interchangeable and a
            // coro-only design still pays nothing.
            auto* rthunk = llvm::BasicBlock::Create(ctx_, "reactive.thunk", fn);
            auto* rcoro  = llvm::BasicBlock::Create(ctx_, "reactive.coro", fn);
            auto* raddr  = b_.CreatePtrToInt(rhdl, b_.getInt64Ty());
            b_.CreateCondBr(b_.CreateTrunc(raddr, b_.getInt1Ty()), rthunk, rcoro);

            b_.SetInsertPoint(rthunk);
            auto* rfnp = b_.CreateIntToPtr(b_.CreateAnd(raddr, ~static_cast<uint64_t>(1)), ptr_ty);
            b_.CreateCall(thunk_fnty, rfnp, { state, wheel });
            b_.CreateBr(rcont);

            b_.SetInsertPoint(rcoro);
        }
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { rhdl });
        llvm::Value* rfin = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { rhdl });
        b_.CreateCondBr(rfin, rdestroy, rcont);

        b_.SetInsertPoint(rdestroy);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { rhdl });
        b_.CreateBr(rcont);

        b_.SetInsertPoint(rcont);
        b_.CreateBr(rcheck);

        if (has_re_inactive) {
            // The reactive ring is empty here, so promoting the batch is a plain
            // copy into the front of it -- the inactive.promote shape exactly.
            b_.SetInsertPoint(richeck);
            llvm::Value* rn = b_.CreateLoad(i32, sym(re_inactive_top_gv_));
            b_.CreateCondBr(b_.CreateICmpNE(rn, b_.getInt32(0)), ripromote, rncheck);

            auto* rphead = llvm::BasicBlock::Create(ctx_, "re_inactive.copy", fn);
            auto* rpbody = llvm::BasicBlock::Create(ctx_, "re_inactive.copy.body", fn);
            auto* rpdone = llvm::BasicBlock::Create(ctx_, "re_inactive.copy.done", fn);
            b_.SetInsertPoint(ripromote);
            b_.CreateBr(rphead);

            b_.SetInsertPoint(rphead);
            auto* rk = b_.CreatePHI(i32, 2);
            rk->addIncoming(b_.getInt32(0), ripromote);
            b_.CreateCondBr(b_.CreateICmpULT(rk, rn), rpbody, rpdone);

            b_.SetInsertPoint(rpbody);
            llvm::Value* rsrc = b_.CreateInBoundsGEP(queue_arr_ty_, sym(re_inactive_gv_), { b_.getInt32(0), rk });
            llvm::Value* rdst = b_.CreateInBoundsGEP(queue_arr_ty_, sym(reactive_gv_), { b_.getInt32(0), rk });
            b_.CreateStore(b_.CreateLoad(ptr_ty, rsrc), rdst);
            rk->addIncoming(b_.CreateAdd(rk, b_.getInt32(1)), rpbody);
            b_.CreateBr(rphead);

            b_.SetInsertPoint(rpdone);
            b_.CreateStore(b_.getInt32(0), sym(reactive_head_gv_));
            b_.CreateStore(rn, sym(reactive_tail_gv_));
            b_.CreateStore(b_.getInt32(0), sym(re_inactive_top_gv_));
            b_.CreateBr(rcheck);
        }

        if (has_nba || has_renba()) {
            // Re-NBA (LRM 4.4.2.8). Both reactive queues have settled here, so
            // anything still staged was staged BY reactive code: the NBA region
            // ran and drained long before the Observed region pended any of
            // this. Commit it and re-drain the reactive ring at the SAME
            // instant -- the Re-Inactive -> Reactive loop one region up, with
            // the update region in place of the batch.
            //
            // Sharing the active region's staging state is safe because the
            // commit CONSUMES it: the block chain head is nulled before the
            // chain runs, rt_nba_arr_reset empties the array / aggregate /
            // queue records, each intra site returns its records to its pool,
            // and every `->> e` flag is cleared as it fires. So this second
            // commit of the instant sees only what reactive code staged after
            // the first, and can neither re-apply an active-region update nor
            // drop a reactive one.
            b_.SetInsertPoint(rncheck);
            // The region's processes first: a clocking block's synchronous
            // drives (LRM 14.16) belong to this region, and every process that
            // could have issued one -- Active code woken by the clocking block
            // event, and a program's Reactive code -- has now run. Their stores
            // are direct, so rdone's re-settle is what publishes them; the
            // commit below is the separate business of the update region, and
            // LRM 14.16.2 leaves the order between a drive and a same-slot
            // nonblocking write to the same variable arbitrary.
            if (has_renba()) {
                emit_renba_run(state, wheel, procs);
            }
            if (has_nba) {
                b_.CreateCondBr(emit_nba_any_staged(), rncommit, rdone);

                b_.SetInsertPoint(rncommit);
                emit_nba_commit(state, wheel);
                b_.CreateBr(rcheck);
            } else {
                b_.CreateBr(rdone);
            }
        }

        b_.SetInsertPoint(rdone);
        // LRM 4.4.2's outer loop: the reactive region set can put work back
        // into the active one -- a Re-NBA commit services wait queues onto the
        // active ring, and an action block's blocking store does the same --
        // and the time slot is only over once BOTH sets are empty. Re-settle
        // instead of falling through, or a process woken from Reactive would
        // not run until the wheel had advanced, a full time step late.
        //
        // Terminates for the same reason the Observed re-settle above does:
        // every trip through here is counted by the active drain's
        // oscillation guard, which aborts a design that never converges.
        auto*        rsettled = llvm::BasicBlock::Create(ctx_, "reactive.settled", fn);
        llvm::Value* rahd     = b_.CreateLoad(i32, sym(active_head_gv_));
        llvm::Value* ratl     = b_.CreateLoad(i32, sym(active_tail_gv_));
        b_.CreateCondBr(b_.CreateICmpNE(rahd, ratl), check, rsettled);
        b_.SetInsertPoint(rsettled);
    }
    // Postponed region (LRM §21.7): final (`assert final`) deferred assertion
    // reports mature here (LRM 16.4), and the VCD dump samples the current
    // time before the wheel advances, so each variable's final settled value
    // is recorded once per instant.
    if (design_uses_defer_) {
        b_.CreateCall(defer_flush_final_fn(), {});
    }
    if (uses_vcd) {
        b_.CreateCall(vcd_flush_fn(), { b_.CreateCall(wheel_now_fn(), { wheel }) });
    }
    // $strobe (LRM 21.2.2): drain every pending deferred-print handle queued
    // this instant. Each was ramped — never resumed — at its call site
    // (lower_strobe_pend); resuming it here, at the settled-instant point, is
    // where the target body actually reads $time / signals and prints, giving
    // exactly the Postponed-region timing the LRM specifies. A strobe body
    // never suspends (is_simple_always forces its caller to a coroutine and
    // the frontend rejects timing controls in a $strobe argument list), so
    // one resume always runs it to completion.
    if (design_uses_strobe_) {
        auto* fn      = b_.GetInsertBlock()->getParent();
        auto* pop_bb  = llvm::BasicBlock::Create(ctx_, "strobe.pop", fn);
        auto* run_bb  = llvm::BasicBlock::Create(ctx_, "strobe.run", fn);
        auto* done_bb = llvm::BasicBlock::Create(ctx_, "strobe.done", fn);
        b_.CreateBr(pop_bb);
        b_.SetInsertPoint(pop_bb);
        llvm::Value* hdl = b_.CreateCall(strobe_pop_fn(), {});
        b_.CreateCondBr(
          b_.CreateICmpNE(hdl, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_))),
          run_bb, done_bb);
        b_.SetInsertPoint(run_bb);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        b_.CreateBr(pop_bb);
        b_.SetInsertPoint(done_bb);
    }
    // $monitor (LRM 21.2.2): at every settled instant, if a monitor is
    // registered and enabled, ramp its body here (in the Postponed region) and
    // resume it. The body renders its argument list and prints only when the
    // output changed since the last emit (rt_monitor_emit), giving the "print
    // once per time step on any argument change" semantics. The body never
    // suspends (timing controls in the arg list are rejected), so one resume
    // runs it to completion. Ramping is indirect: rt_monitor_active_ramp yields
    // the registered body's ramp function pointer (or null), which we call with
    // the task ABI (state, rng, wheel, record). There is no calling process
    // here, so the body seeds its own state from the module-wide one.
    if (design_uses_monitor_) {
        auto* fn      = b_.GetInsertBlock()->getParent();
        auto* run_bb  = llvm::BasicBlock::Create(ctx_, "monitor.run", fn);
        auto* cont_bb = llvm::BasicBlock::Create(ctx_, "monitor.cont", fn);
        auto* ptr_ty  = llvm::PointerType::getUnqual(ctx_);
        llvm::Value* ramp = b_.CreateCall(monitor_active_ramp_fn(), {});
        b_.CreateCondBr(b_.CreateICmpNE(ramp, llvm::ConstantPointerNull::get(ptr_ty)),
                        run_bb, cont_bb);
        b_.SetInsertPoint(run_bb);
        llvm::Value* record = b_.CreateAlloca(ptr_ty, nullptr, "monitor.record");
        b_.CreateStore(llvm::ConstantPointerNull::get(ptr_ty), record);
        // Reconstructed rather than read off a declaration, because the callee
        // arrives as an opaque pointer from the runtime -- so it MUST track
        // declare_task's gated parameters exactly. It did not: `proc` was pushed
        // into margv but never into this type, so a design with both $monitor
        // and process control built a call whose argument count did not match
        // its signature (an assert in a debug LLVM, malformed IR in a release
        // one). Both gates are honoured below now.
        std::vector<llvm::Type*> rampparams{ ptr_ty, ptr_ty, ptr_ty }; // state, rng, wheel
        if (design_uses_process_) {
            rampparams.push_back(ptr_ty); // proc
        }
        if (design_uses_home_arg_) {
            rampparams.push_back(llvm::Type::getInt8Ty(ctx_)); // home
        }
        rampparams.push_back(ptr_ty); // record
        auto*                     rampty = llvm::FunctionType::get(ptr_ty, rampparams, false);
        std::vector<llvm::Value*> margv{ state, sym(rng_global_), wheel };
        if (design_uses_process_) {
            margv.push_back(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
        }
        // No calling thread on this path either (the Postponed drain ramps the
        // registered body from main), so the region it would inherit is the
        // default Active one.
        if (design_uses_home_arg_) {
            margv.push_back(b_.getInt8(0));
        }
        margv.push_back(record);
        llvm::Value* hdl = b_.CreateCall(rampty, ramp, margv);
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
        b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
        b_.CreateBr(cont_bb);
        b_.SetInsertPoint(cont_bb);
    }
    // Advance one instant (firing any timers, which enqueue their handles) and
    // re-drain. rt_wheel_advance folds the pending-check into the tick — one ABI
    // crossing per timestep, no separate occupancy scan — and returns non-zero
    // iff it fired something (an idle wheel ends the sim). The tick ctx is the
    // wheel: coroutine-timer trampolines ignore it, but a detached `->> #d e`
    // on-expiry thunk reads the current time from it (event_delay_thunk).
    llvm::Value* ticked = b_.CreateCall(wheel_advance_fn(), { wheel, wheel });
    // THE safepoint (docs/design/garbage-collection.md §2, F1). This is the
    // drain loop's quiescent edge: the active ring, the inactive queue, the
    // NBA region and the reactive ring have all drained to reach here, so
    // every process in the design is parked at a suspend inside its own
    // coroutine frame and the native stack is exactly
    // main -> rt_sim_run -> sim_drain. No JIT frame can be live: every
    // emitted body is reached only by a coro.resume from one of the drains
    // above, and each of those returned before this point -- a resume that
    // has not returned is, by construction, not this edge.
    //
    // Immediately AFTER rt_wheel_advance rather than before it, so the
    // timers this tick fired have already enqueued their handles onto the
    // active ring, where the root scan reaches them, instead of being live
    // only inside the wheel mid-tick.
    //
    // The poll itself is inlined: a load of rt_gc_mutator::request, a test
    // and a cold not-taken branch, because the out-of-line call this
    // replaced cost real simulate time on designs that never allocate
    // (Gc.h:144-149).
    emit_gc_safepoint();
    // Time advanced: a new instant begins, so reset the oscillation counter.
    b_.CreateStore(b_.getInt64(0), sym(osc_count_gv_));
    const bool preponed = has_preponed();
    if (preponed || !inertial_sigs_.empty()) {
        // Preponed (LRM 4.4.2.1) and inertial continuous-assignment delay
        // (LRM 10.3.3) both belong at the top of a new time step, ahead of the
        // active drain, and in that order: sampling precedes every update of
        // the step it names, including a CA release.
        //
        // Inertial continuous-assignment delay (LRM 10.3.3): a CA updates its net
        // in the ACTIVE region, so a store that fired on this wheel tick must be
        // visible to same-instant processes the tick also resumed. Commit those
        // records here, ahead of the active drain, unlike a procedural `<= #d`
        // (nonblocking) which the NBA region commits after the active drain. The
        // NBA-region intra loop still runs (below), but only sees records armed
        // during this instant's active region (a `#0` continuous assign), the
        // wheel-fired ones already released here.
        auto* post_tick = llvm::BasicBlock::Create(ctx_, "wheel.posttick", fn);
        b_.CreateCondBr(b_.CreateICmpNE(ticked, b_.getInt32(0)), post_tick, wdone);
        b_.SetInsertPoint(post_tick);
        if (preponed) {
            emit_preponed_run(state, wheel, procs);
        }
        for (const Signal sig : inertial_sigs_) {
            emit_intra_commit(sig);
        }
        b_.CreateBr(check);
    } else {
        b_.CreateCondBr(b_.CreateICmpNE(ticked, b_.getInt32(0)), check, wdone);
    }

    b_.SetInsertPoint(wtick); // unreachable now; kept so block wiring stays valid
    b_.CreateBr(wdone);

    b_.SetInsertPoint(wdone);
    if (uses_vcd) {
        b_.CreateCall(vcd_close_fn(), {});
    }
}

// The timing wheel's storage, as a module global rather than main's alloca.
// Created before any body is lowered, because a function body binds `wheel_` to
// it: a function's ABI carries state and rng but no wheel, and every event op
// needs the current tick. main initialises it (rt_wheel_init) before the drain.
void Backend::build_wheel_global() {
    auto* wheel_ty = llvm::ArrayType::get(b_.getInt8Ty(), sizeof(rt_wheel));
    wheel_gv_      = new llvm::GlobalVariable(env_->llmod, wheel_ty, false,
                                              llvm::GlobalValue::InternalLinkage,
                                              llvm::ConstantAggregateZero::get(wheel_ty),
                                              "clotho.wheel");
    wheel_gv_->setAlignment(llvm::Align(alignof(rt_wheel)));
}

// Reference-routine symbol for a distribution selector (LRM §20.15.2).
const char* Backend::dist_symbol(uint32_t fn) {
    switch (static_cast<DistOp>(fn)) {
        case DistOp::Uniform:
            return "rt_dist_uniform";
        case DistOp::Normal:
            return "rt_dist_normal";
        case DistOp::Exponential:
            return "rt_dist_exponential";
        case DistOp::Poisson:
            return "rt_dist_poisson";
        case DistOp::ChiSquare:
            return "rt_dist_chi_square";
        case DistOp::StudentT:
            return "rt_dist_t";
        case DistOp::Erlang:
            return "rt_dist_erlang";
    }
    throw BackendError{ "internal: unknown dist op" };
}

} // namespace clotho::ir::backend
