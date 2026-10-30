// Process groups, functions and tasks: frames, arguments and calls.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Lower one fusion group (usually a singleton) to one coroutine. Members of
// a fused group share one frame, one sched flag (the first member's slot),
// and one wait suspend; their bodies run back-to-back per wake, in
// scheduled-rank order (see compute_fusion_groups for the legality
// argument). The per-member value tables are swapped in around each
// member's body.
llvm::Function* Backend::lower_group(const std::vector<uint32_t>& members) {
    const auto     all        = mod_.processes();
    const uint32_t proc_index = members.front();
    const Process& proc       = all[proc_index];
    ir_                       = &proc.ir;
    frame_slots_.clear(); // a process has no frame; deinit sees only temps

    // This group's scheduled-flag slot: &clotho.proc_sched[proc_index]. A
    // constant GEP so it is valid in any block of the function. Dedup prep:
    // a group fully inside one reused-definition instance instead loads the
    // instance's sched base and indexes the def-local ordinal — the same
    // slot, materialized at function entry (materialize_sched_ptr); a
    // unique-definition or cross-instance group keeps the folded constant.
    sched_anchor_      = SignalSlot::no_anchor;
    sched_group_       = group_of_[proc_index];
    const uint32_t gaid = group_anchor_of_[sched_group_];
    if (gaid != SignalSlot::no_anchor) {
        sched_anchor_  = gaid;
        sched_ordinal_ = proc_sched_ord_[proc_index];
    }
    sched_ptr_ = sched_anchor_ != SignalSlot::no_anchor ? nullptr : sched_flag_ptr(proc_index);

    // Deferred immediate assertion (LRM 16.4): a group carrying one gets a
    // per-process generation counter, bumped at each activation, that tags
    // its queued reports so a superseded activation's reports are dropped at
    // maturation (the flush of LRM 16.4.2). Such a group is a forced coroutine
    // (is_simple_always is false for a deferred Severity) and coroutines are
    // never shared across instances (only thunk groups dedup), so every
    // instance of a reused definition is its own group with its own gen
    // global keyed by the per-instance group id — per-instance correct with
    // no extra machinery. The counter is a plain per-group global.
    cur_defer_group_ = -1;
    if (std::any_of(members.begin(), members.end(),
                    [&](uint32_t m) { return facts_.processes[m].deferred_severity; })) {
        cur_defer_group_ = static_cast<int32_t>(sched_group_);
    }

    // Same-eligibility NBA prep: the group's block registrations pass
    // (instance block base + group-local ordinal) instead of the absolute
    // block id — identical value (a group's blocks are id-contiguous by
    // construction: block_register only runs while its group lowers), in the
    // def-relative form a shared body needs. Main stores the base into the
    // instance ctx after lowering, once the ids are known (ctx_blk_fills_).
    group_first_block_ = -1;

    // Name a process by its owner: a definition-unit group (split mode) is
    // @clotho.inst.<hier>.p<def-local ordinal> — instance-qualified (twin
    // instances land in one module, so a bare def-local name would
    // collide) with a def-local index, keeping the unit module's bytes
    // independent of whole-design process numbering (cache-stability
    // prep). Everything else keeps @<module>.p<flat index>; source-offset
    // naming would collide across instances of one module (processes
    // share the AST location); proc.loc still carries the source mapping.
    // A fused group is named by its first member plus its size.
    const uint32_t ua   = group_unit_anchor_[group_of_[proc_index]];
    std::string    name = config_.split && ua != SignalSlot::no_anchor
                            ? "clotho.inst." + anchors_[ua].name + ".p" +
                                std::to_string(proc_sched_ord_[proc_index])
                            : mod_.name + ".p" + std::to_string(proc_index);
    if (members.size() > 1) {
        name += ".f" + std::to_string(members.size());
    }

    // Per-member table setup, run before lowering anything of that member.
    const auto select_member = [&](uint32_t m) {
        ir_            = &all[m].ir;
        const size_t n = ir_->inst_count();
        values_.assign(n, Plane{});
        lvals_.assign(n, std::optional<LValue>{});
        types_.resize(n);
        types_valid_.assign(n, 0);
    };
    // Open the guarded region of a group: returns true when its members are
    // guarded, having ensured the wake word carries at least one of the
    // group's bits.
    //
    // That store is the fail-safe. If the word carries none of this group's
    // bits, some wake path pushed the group without naming a trigger, so set
    // them all and let every member run -- which makes a missed bit a lost
    // optimization rather than a lost execution, in a wake path this wide.
    // It also means each member's guard is a plain test with no fallback of
    // its own.
    auto open_group = [&](uint32_t g) -> bool {
        if (!group_guarded_[g]) {
            return false;
        }
        auto* mask = b_.getInt64(group_trig_mask_[g]);
        auto* w    = b_.CreateLoad(sched_word_ty(), sched_ptr_);
        auto* none = b_.CreateICmpEQ(b_.CreateAnd(w, mask), b_.getInt64(0));
        auto* set  = llvm::BasicBlock::Create(ctx_, "trig.all", fn_);
        auto* go   = llvm::BasicBlock::Create(ctx_, "trig.go", fn_);
        b_.CreateCondBr(none, set, go);
        b_.SetInsertPoint(set);
        b_.CreateStore(b_.CreateOr(w, mask), sched_ptr_);
        b_.CreateBr(go);
        b_.SetInsertPoint(go);
        return true;
    };
    // Open one member's guard: run it only if one of ITS OWN events has
    // fired. Returns the join block to branch to once the body is emitted
    // (null when the group is unguarded).
    //
    // The word is re-read per member, NOT snapshotted once for the group.
    // An earlier member's store to a group-internal signal ORs the
    // downstream member's bit in through the already-queued arm of
    // emit_static_scan_rt, and that edge is the whole point of running a
    // component in topological order: the consumer must see the producer's
    // change in THIS pass. Snapshotting at group entry loses exactly those
    // bits, which strands every consumer fed from inside the group
    // (comb-fusion.md's "the downstream member runs later in the same
    // activation with final values" -- with guards, that sentence is
    // load-bearing, not an aside).
    auto open_guard = [&](bool guarded, uint64_t mask, uint64_t group_mask) -> llvm::BasicBlock* {
        // A member sensitive to everything the group is sensitive to runs on
        // every activation, so its guard can only ever be true: emit none.
        if (!guarded || mask == group_mask) {
            return nullptr;
        }
        auto* run  = llvm::BasicBlock::Create(ctx_, "mem.run", fn_);
        auto* join = llvm::BasicBlock::Create(ctx_, "mem.skip", fn_);
        auto* bits = b_.CreateAnd(b_.CreateLoad(sched_word_ty(), sched_ptr_), b_.getInt64(mask));
        b_.CreateCondBr(b_.CreateICmpNE(bits, b_.getInt64(0)), run, join);
        b_.SetInsertPoint(run);
        return join;
    };
    auto close_guard = [&](llvm::BasicBlock* join) {
        if (join != nullptr) {
            b_.CreateBr(join);
            b_.SetInsertPoint(join);
        }
    };


    // Coroutine elision: a suspension-free always-with-sensitivity group (see
    // is_simple_always) carries no state across activations, so instead of a
    // coroutine it lowers to a plain thunk — void @<name>.thunk(state, wheel)
    // — run once per activation. The seed subscribes a tagged pointer to it
    // and the drain calls it directly (no frame, no suspend switch, no
    // CoroSplit). The sched-flag timeline matches the coroutine always-loop:
    // the flag is held set across the body and cleared at the end, so
    // same-instant re-triggers behave identically.
    // Cross-instance fused thunk group under split emission: lower each
    // member as its own plain function emitted into the member's OWN
    // definition unit, plus a root-emitted dispatcher (named like the
    // merged thunk, so seed/subscription/drain contracts are untouched)
    // calling them in scheduled-rank order. The members become
    // per-definition cacheable — the merged form parked the whole group's
    // code in the always-recompiled root. Single-scope groups keep the
    // merged body inside their unit (no call overhead). Only the population
    // that cannot otherwise leave root pays the per-member call, not every comb
    // group (docs/design/module-split.md, Partition).
    // Member cap: a cross-module member call cannot be inlined, so the
    // per-activation dispatch cost scales with member count — a 256-member
    // comb group measured 3.7x sim outlined (combchain), while the real
    // cross-instance populations on cores (2-7 member clocked groups) are
    // wall-neutral. Groups past the cap keep the merged root body.
    constexpr size_t outline_max_members = 16;
    const uint32_t g_ = group_of_[proc_index];
    // A thunk group is not a thread: no frame, no handle, no control block, and
    // proc_arg's own contract already says such a body passes null. Reset here,
    // ahead of both thunk paths, because they return before the coroutine path's
    // reset at the ramp -- a leftover value is a constant GEP naming some
    // PREVIOUSLY lowered process's block, so a function called from a thunk would
    // be handed the wrong thread. Today nothing dereferences it (a thunk cannot
    // suspend or park, so no poll or state store reads it, and a design with no
    // process op has no `self()` to observe it), which is why it has never
    // bitten; LRM 24.3's termination turning the blocks on for any program that
    // forks widens the population it is latent in, so fix it rather than reason
    // about it again.
    pcb_ = nullptr;
    if (config_.split && config_.outline && thunk_group_[g_] &&
        members.size() > 1 && members.size() <= outline_max_members &&
        group_unit_anchor_[g_] == SignalSlot::no_anchor &&
        std::any_of(members.begin(), members.end(),
                    [&](uint32_t m) { return proc_sched_anchor_[m] != ~0u; })) {
        auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto* fnty   = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);

        std::vector<llvm::Function*> member_fns;
        member_fns.reserve(members.size());
        for (const uint32_t m : members) {
            const uint32_t aid = proc_sched_anchor_[m];
            env_ = aid != ~0u ? &unit_env(anchors_[aid]) : root_env_.get();
            const std::string mname =
              aid != ~0u ? "clotho.inst." + anchors_[aid].name + ".p" +
                             std::to_string(proc_sched_ord_[m])
                         : mod_.name + ".p" + std::to_string(m);
            fn_ = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, mname, env_->llmod);
            state_ = fn_->getArg(0);
            state_->setName("state");
            wheel_ = fn_->getArg(1);
            wheel_->setName("wheel");
            fn_->addParamAttr(0, llvm::Attribute::NoAlias);
            b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn_));
            rng_slot_ = sym(rng_global_);
            // Member bodies never touch the sched flag (the dispatcher
            // owns the timeline) and address absolutely (no ctx), but
            // their NBA block ids need a member-scoped def-stable idval
            // key: block_register keys on outline_anchor_/outline_ord_
            // while these are set, with block ids member-contiguous
            // (group_first_block_ reset per member).
            outline_anchor_    = aid;
            outline_ord_       = proc_sched_ord_[m];
            group_first_block_ = -1;
            select_member(m);
            str_vals_.assign(ir_->inst_count(), nullptr);
            build_str_temps();
            build_dynarr_temps();
            build_agg_elem_temps();
            lower_region(ir_->root());
            deinit_body_strings();
            b_.CreateRetVoid();
            member_fns.push_back(fn_);
        }
        outline_anchor_    = ~0u;
        group_first_block_ = -1;

        env_ = root_env_.get();
        fn_  = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name + ".thunk", env_->llmod);
        state_ = fn_->getArg(0);
        state_->setName("state");
        wheel_ = fn_->getArg(1);
        wheel_->setName("wheel");
        fn_->addParamAttr(0, llvm::Attribute::NoAlias);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn_));
        rng_slot_ = sym(rng_global_);
        const bool guarded = open_group(g_);
        for (size_t i = 0; i < member_fns.size(); ++i) {
            llvm::BasicBlock* join = open_guard(guarded, member_mask(g_, i), group_trig_mask_[g_]);
            b_.CreateCall(fnty, sym(member_fns[i]), { state_, wheel_ });
            close_guard(join);
        }
        b_.CreateStore(b_.getInt64(0), sched_ptr_); // re-arm: ready to be woken
        b_.CreateRetVoid();
        return fn_;
    }

    if (thunk_group_[group_of_[proc_index]]) {
        auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
        auto* fnty   = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);
        fn_ = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name + ".thunk", env_->llmod);
        state_ = fn_->getArg(0);
        state_->setName("state");
        wheel_ = fn_->getArg(1);
        wheel_->setName("wheel");
        fn_->addParamAttr(0, llvm::Attribute::NoAlias);
        awaiter_          = nullptr;
        record_           = nullptr;
        fork_record_      = nullptr;
        fork_live_        = nullptr;
        disable_node_     = nullptr;
        fork_disable_nodes_.clear();
        spawn_sched_ = nullptr;
        block_nodes_.clear();
        event_wait_nodes_.clear();
    sync_nodes_.clear();
        cur_is_task_      = false;
        silent_stores_ = false;
        hdl_              = nullptr;
        auto* entry_bb = llvm::BasicBlock::Create(ctx_, "entry", fn_);
        b_.SetInsertPoint(entry_bb);
        // A thunk group cannot reach a draw (compute_fusion_groups excludes one
        // that can), so this pointer only ever travels as a callee's ABI
        // argument and is never dereferenced.
        rng_slot_ = sym(rng_global_);
        materialize_group_ctx();
        // Run each member body per activation, freeing its string temporaries
        // afterward — a thunk has no frame to persist them in (a per-call cost
        // paid only by string-using bodies). frame_slots_ is empty for a
        // process, so deinit_body_strings frees exactly this member's temps.
        const bool guarded = open_group(group_of_[proc_index]);
        for (size_t i = 0; i < members.size(); ++i) {
            select_member(members[i]);
            str_vals_.assign(ir_->inst_count(), nullptr);
            build_str_temps();
            build_dynarr_temps();
            build_agg_elem_temps();
            llvm::BasicBlock* join = open_guard(guarded, member_mask(group_of_[proc_index], i), group_trig_mask_[group_of_[proc_index]]);
            lower_region(ir_->root());
            deinit_body_strings();
            close_guard(join);
        }
        b_.CreateStore(b_.getInt64(0), sched_ptr_); // re-arm: ready to be woken
        b_.CreateRetVoid();
        return fn_;
    }

    // A process lowers to a switched-resume LLVM coroutine:
    // `ptr @<module>.<offset>(ptr noalias state, ptr wheel)`. The ramp runs the
    // body until the first #delay (or to completion) and returns the coroutine
    // handle; the scheduler resumes/destroys through that handle. A #delay (see
    // lower_delay) schedules a wheel timer and suspends; the timer's callback
    // resumes the coroutine when the delay elapses. A delay-free process runs
    // straight to the final suspend, exactly like the old plain function.
    auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto* fnty   = llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
    fn_          = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name, env_->llmod);
    // Mark it pre-split so the CoroSplit pass picks it up.
    fn_->setPresplitCoroutine();
    state_ = fn_->getArg(0);
    state_->setName("state");
    wheel_ = fn_->getArg(1);
    wheel_->setName("wheel");
    // The state pointer is the sole module memory the process touches; noalias
    // lets the allocator rematerialize signal loads instead of spilling them.
    fn_->addParamAttr(0, llvm::Attribute::NoAlias);
    awaiter_          = nullptr;
    record_           = nullptr;
    fork_record_      = nullptr;
    fork_live_        = nullptr;
    disable_node_     = nullptr;
    fork_disable_nodes_.clear();
    spawn_sched_      = nullptr;
    cur_is_task_      = false;
    // PreInit applies declaration initializers and Preponed captures sampled
    // values; neither is a design-visible write, so neither may wake a
    // process. Everything downstream keys on this rather than on the
    // activation, because the property is "this store generates no event".
    silent_stores_ = proc.activation == Activation::PreInit ||
                     proc.activation == Activation::Preponed;
    // A program block's `initial` (LRM 24.3). Drives the region-restoring
    // resume in emit_suspend and the 24.3 implicit finish below.
    in_program_body_ = proc.activation == Activation::Reactive;
    // A continuous process declared inside a program (LRM 24.3.1): reactive-
    // home like a program `initial`, but NOT one -- it never reaches its end,
    // so it must stay out of in_program_body_, which is what counts the
    // implicit-finish countdown down. Fusion groups are home-homogeneous by
    // group key (compute_fusion_groups), so the front member answers for the
    // whole group.
    in_reactive_home_body_ = proc.reactive_home;
    // A process is a thread ROOT: it has no caller to inherit a home from, so
    // its lexical answer above is the whole truth and home_ptr, if one is
    // needed at all, seeds from it. The one thing that can still change it is
    // an `expect` (LRM 16.17), which moves the thread to Reactive for good and
    // so has to be read at each park rather than folded (see home_ptr).
    body_home_is_runtime_ = ir_has_expect_wait(proc.ir);
    home_ptr_             = nullptr;
    home_arg_             = nullptr;

    auto* none     = llvm::ConstantTokenNone::get(ctx_);
    auto* null     = llvm::ConstantPointerNull::get(ptr_ty);
    auto* entry    = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    auto* alloc_bb = llvm::BasicBlock::Create(ctx_, "coro.alloc", fn_);
    auto* begin_bb = llvm::BasicBlock::Create(ctx_, "coro.begin", fn_);

    // Identify the coroutine; coro.alloc lets the optimizer skip the dynamic
    // frame allocation when the frame can live on the caller's stack.
    b_.SetInsertPoint(entry);
    materialize_group_ctx();
    auto* id   = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_id), { b_.getInt32(0), null, null, null });
    auto* need = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_alloc), { id });
    // Over-allocate by coro.align: with an over-aligned frame (e.g. an i128
    // plane spill) CoroSplit lays out fields past coro.size unless the
    // allocation covers the realignment slack (LLVM coroutine contract).
    //
    // When this frame is REGISTERED these are hoisted out of coro.alloc into the
    // entry block, because the frame registry below needs the byte count in
    // coro.begin -- including on the elided path, where coro.alloc never runs.
    // Both intrinsics are pure and CoroSplit folds each to a constant, so the
    // hoist costs nothing; it is gated only to keep the unregistered ramp's
    // instruction order free of a hoist that would serve nothing there.
    llvm::Value* size  = nullptr;
    llvm::Value* align = nullptr;
    const auto   frame_bytes = [&] {
        size  = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_size, { b_.getInt64Ty() }), {});
        align = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_align, { b_.getInt64Ty() }), {});
    };
    // Register this frame only when it can hold a reference into the collected
    // heap. A fused group's members share one coroutine, so any member that can
    // decides for the whole frame.
    const bool gc_frame = std::ranges::any_of(members, [&](uint32_t m) {
                              return frame_may_hold_gc_ref(all[m]);
                          });
    if (gc_frame) {
        frame_bytes();
    }
    b_.CreateCondBr(need, alloc_bb, begin_bb);

    // Dynamic frame: rt_alloc(coro.size). The sim owns frame memory so a frame
    // outlives the ramp while the process is suspended on the wheel.
    b_.SetInsertPoint(alloc_bb);
    if (!gc_frame) {
        frame_bytes();
    }
    auto* mem = b_.CreateCall(alloc_fn(), { b_.CreateAdd(size, align) });
    b_.CreateBr(begin_bb);

    b_.SetInsertPoint(begin_bb);
    auto* frame = b_.CreatePHI(ptr_ty, 2);
    frame->addIncoming(null, entry);
    frame->addIncoming(mem, alloc_bb);
    hdl_        = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_begin), { id, frame });
    coro_begin_ = llvm::cast<llvm::Instruction>(hdl_);
    gc_frame_node_ = nullptr;
    if (gc_frame) {
        emit_gc_frame_link(size);
    }
    // String rvalue temporaries live in the coroutine frame (CoroSplit spills
    // any that survive a suspend) and are constructed once, in the ramp —
    // per member, with each member's inst-indexed pointers snapshotted for
    // its body lowering below. (str_temp_list_ ends up holding only the
    // last member's temps; it feeds deinit_body_strings, which an
    // always-loop group never reaches.)
    std::vector<std::vector<llvm::Value*>> member_strs;
    std::vector<std::vector<llvm::Value*>> member_dynarr;
    std::vector<std::vector<llvm::Value*>> member_agg_elem;
    for (const uint32_t m : members) {
        select_member(m); // sizes the type cache build_str_temps consults
        str_vals_.assign(ir_->inst_count(), nullptr);
        build_str_temps();
        build_dynarr_temps();
        build_agg_elem_temps();
        member_strs.push_back(str_vals_);
        member_dynarr.push_back(dynarr_ret_);
        member_agg_elem.push_back(agg_elem_ret_);
    }
    ir_ = &proc.ir;

    // Shared epilogue, built up front so #delay suspends and the final suspend
    // can all branch to it. cleanup frees the frame (coro.free -> rt_free); ret
    // ends the coroutine and returns the handle to whoever resumed it.
    auto* trap_bb     = llvm::BasicBlock::Create(ctx_, "coro.trap", fn_);
    auto* free_bb     = llvm::BasicBlock::Create(ctx_, "coro.free", fn_);
    coro_cleanup_bb_  = llvm::BasicBlock::Create(ctx_, "coro.cleanup", fn_);
    coro_ret_bb_      = llvm::BasicBlock::Create(ctx_, "coro.ret", fn_);
    // Created up front, not at the end of the body, because a cooperative
    // $finish branches here too (lower_finish): the process must reach its
    // FINAL SUSPEND so the resumer's coro.done sees it and destroys the frame
    // once. Branching to coro.cleanup instead would free the frame underneath
    // the drain loop, which then reads it — a use-after-free.
    coro_final_bb_    = llvm::BasicBlock::Create(ctx_, "coro.final", fn_);
    // Where a program `initial` counts itself down and terminates its program's
    // descendant threads (LRM 24.3, emit_program_end). Its own block rather than
    // the fallthrough because an `initial` aborted by `disable` or
    // `process::kill` has "reached its end" for the clause and must count too,
    // so proc_abort_exit sends a program body here instead of straight to
    // coro.final. A $finish still branches straight there (emit_finish_exit) and
    // is deliberately not counted -- it has already ended the simulation.
    program_end_bb_ = in_program_body_ ? llvm::BasicBlock::Create(ctx_, "program.end", fn_) : nullptr;
    // The program instance a `$exit` in this body names (LRM 24.7). For a
    // program `initial` that is simply the instance it belongs to; a subroutine
    // and a fork child get theirs from the owner closure instead (build_function
    // / lower_task).
    cur_program_inst_ = in_program_body_ ? program_inst_of(proc_index) : kNoProgramInst;

    b_.SetInsertPoint(coro_cleanup_bb_);
    if (gc_frame) {
        emit_gc_frame_unlink();
    }
    auto* freed = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_free), { id, hdl_ });
    b_.CreateCondBr(b_.CreateICmpNE(freed, null), free_bb, coro_ret_bb_);

    b_.SetInsertPoint(free_bb);
    b_.CreateCall(free_fn(), { freed });
    b_.CreateBr(coro_ret_bb_);

    b_.SetInsertPoint(coro_ret_bb_);
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_end), { hdl_, b_.getFalse(), none });
    b_.CreateRet(hdl_);

    // Initial suspend: the ramp builds the frame and immediately suspends here,
    // returning the handle WITHOUT running the body. The scheduler runs the body
    // on the first coro.resume. This makes "create a process" and "wake a
    // process" the same operation — resume a handle — so both flow through the
    // active queue uniformly. The default edge returns to the creator; case 0
    // (first resume) enters the body; case 1 is a destroy before first resume.
    b_.SetInsertPoint(begin_bb);
    auto* start_bb = llvm::BasicBlock::Create(ctx_, "coro.start", fn_);
    auto* init     = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getFalse() });
    auto* sw_init  = b_.CreateSwitch(init, coro_ret_bb_, 2);
    sw_init->addCase(b_.getInt8(0), start_bb);
    sw_init->addCase(b_.getInt8(1), coro_cleanup_bb_);

    // Process body. lower_region appends into start_bb (and into any blocks a
    // #delay / @-wait suspend creates), leaving the insert point on a
    // terminator-less block that falls through to the final suspend.
    b_.SetInsertPoint(start_bb);
    // Per-process $urandom RNG (LRM §18.13): allocate the xoshiro256++ state
    // in the coroutine frame and seed it once here in the ramp, before the
    // body loop, so it persists across activations (the loop back-edge is
    // below this point) and CoroSplit frames it. The seed selector is the
    // process index — an independent, run-stable stream per process,
    // schedule-independent; $urandom(seed) reseeds in place. A process that
    // cannot reach a draw (its own or one in a subroutine it calls, a child it
    // forks or a deferred body it ramps) materializes nothing and passes the
    // module-wide state along, which by that proof nothing dereferences.
    if (rng_procs_[proc_index]) {
        rng_slot_ = b_.CreateAlloca(llvm::ArrayType::get(b_.getInt64Ty(), 4), nullptr, "urng");
        b_.CreateCall(urandom_seed_fn(), { rng_slot_, b_.getInt64(proc_index) });
    } else {
        rng_slot_ = sym(rng_global_);
    }
    // This thread's control block (LRM 9.7): a statically created process has a
    // constant slot in clotho.proc_pcb, whose initializer already holds the
    // starting state, so this costs one GEP and no stores.
    pcb_       = design_uses_process_ ? proc_pcb_slot(proc_index) : nullptr;
    emit_proc_publish_rng();
    acts_node_ = nullptr;
    emit_pcb_acts_link();
    // A program `initial` can be terminated before it has ever run, and this is
    // the only body kind that can: `$exit` (LRM 24.7) names every `initial` of
    // its program instance by that procedure's STATIC control block, including
    // one the seed loop has queued but the reactive drain has not yet resumed.
    // Such a procedure has no activation for the mark walk to force-wake --
    // pcb->acts is linked just above, AFTER the initial suspend, so it is still
    // null while the process sits queued -- and every other abort poll sits after
    // a suspending op, so without this test it would run everything up to its
    // first park after having been terminated. The same hole as an unstarted fork
    // child's (lower_task), reached by a different route; the two are the whole
    // set, since nothing can name a design process before it starts.
    //
    // It leaves through program.end, which counts it down: 24.3 asks only that a
    // procedure have "reached its end", and one terminated by $exit has.
    if (in_program_body_ && pcb_ != nullptr) {
        auto*        live = llvm::BasicBlock::Create(ctx_, "program.initial.live", fn_);
        llvm::Value* ctl  = b_.CreateLoad(b_.getInt32Ty(), pcb_field(pcb_, kPcbCtl));
        b_.CreateCondBr(
          b_.CreateICmpNE(b_.CreateAnd(ctl, b_.getInt32(kProcAbort)), b_.getInt32(0)), program_end_bb_, live);
        b_.SetInsertPoint(live);
    }
    const bool always_loop = proc.activation == Activation::Always && !proc.wait_events.empty();
    if (always_loop) {
        // An always block with a sensitivity list. It is subscribed to its
        // signals' static queues from the seed loop (emit_active_run) and left
        // parked at the initial suspend; the first wake resumes here. So the
        // loop is body-first: run the bodies for the trigger that woke us,
        // re-arm the scheduled flag, suspend until the next trigger, repeat.
        // The bodies therefore run only on a trigger, never at time zero, with
        // the flag held set across them (it is cleared just before the
        // suspend), so the group's own stores can never re-wake it mid-body.
        auto* loop_bb = llvm::BasicBlock::Create(ctx_, "always.loop", fn_);
        b_.CreateBr(loop_bb);
        b_.SetInsertPoint(loop_bb);
        emit_defer_gen_bump(); // activation start = deferred-assertion flush point
        const bool guarded = open_group(group_of_[proc_index]);
        for (size_t i = 0; i < members.size(); ++i) {
            select_member(members[i]);
            str_vals_     = member_strs[i];
            dynarr_ret_   = member_dynarr[i];
            agg_elem_ret_ = member_agg_elem[i];
            llvm::BasicBlock* join = open_guard(guarded, member_mask(group_of_[proc_index], i), group_trig_mask_[group_of_[proc_index]]);
            lower_region(ir_->root());              // member body (per trigger)
            close_guard(join);
        }
        b_.CreateStore(b_.getInt64(0), sched_ptr_); // re-arm: ready to be woken
        emit_wait_suspend();                        // park until next trigger
        b_.CreateBr(loop_bb);
        // The forever back-edge means the shared final-suspend tail below is
        // unreachable; build it in a dead block so the IR stays well-formed.
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "always.exit", fn_));
    } else {
        // Never fused: a non-always (or sensitivity-less) group is a singleton.
        emit_defer_gen_bump(); // single-shot body: one activation, one flush point
        select_member(proc_index);
        str_vals_     = member_strs.front();
        dynarr_ret_   = member_dynarr.front();
        agg_elem_ret_ = member_agg_elem.front();
        lower_region(ir_->root());
    }

    // Final suspend: the default edge returns the (now-done) handle to the
    // resumer, which destroys it; case 1 is the destroy re-entry; resuming a
    // finished coroutine (case 0) is undefined, so trap. The LRM 24.3 rules
    // run first, in program.end, which every path by which a program `initial`
    // reaches its end goes through (emit_program_end, proc_abort_exit).
    if (in_program_body_) {
        b_.CreateBr(program_end_bb_);
        b_.SetInsertPoint(program_end_bb_);
        emit_program_end(proc_index);
    }

    b_.CreateBr(coro_final_bb_);
    b_.SetInsertPoint(coro_final_bb_);
    // A completing process releases its string temporaries. (An always loop
    // reaches here only by finishing — otherwise its temps persist, bounded
    // and capacity-reused.)
    deinit_body_strings();
    emit_fork_record_abandon();
    // ... and stops being a thread (LRM 9.7): FINISHED, children re-parented,
    // `await`ers woken. An `always` body reaches this only through $finish, which
    // is also the end of its thread, so the hook is right for both shapes.
    emit_proc_complete();
    auto* res = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getTrue() });
    auto* sw  = b_.CreateSwitch(res, coro_ret_bb_, 2);
    sw->addCase(b_.getInt8(0), trap_bb);
    sw->addCase(b_.getInt8(1), coro_cleanup_bb_);

    b_.SetInsertPoint(trap_bb);
    b_.CreateUnreachable();
    in_program_body_ = false;
    in_reactive_home_body_ = false;
    body_home_is_runtime_ = false;
    home_ptr_             = nullptr;
    // Blocks of a finished function must never be reachable from the next body's
    // lowering, the event_wait_node hazard: proc_abort_exit reads this one.
    program_end_bb_   = nullptr;
    cur_program_inst_ = kNoProgramInst;
    return fn_;
}

bool Backend::is_void(Ref type) const {
    return type == mod_.pool.void_type;
}

// The LLVM type a function returns: void, or the `{ i(2W), iW }` result planes.
llvm::Type* Backend::fn_return_ty(Ref return_type) {
    if (is_void(return_type)) {
        return b_.getVoidTy();
    }
    const uint32_t w = width_of(return_type);
    return llvm::StructType::get(ctx_, { int_ty(2 * w), int_ty(w) });
}

// The native LLVM type of a DPI-C scalar (LRM 35.5.6): the by-value C type
// each SV scalar maps to. Wider SV types never reach here (frontend-guarded).
llvm::Type* Backend::dpi_c_llvm_ty(DpiCType t) {
    switch (t) {
        case DpiCType::Void:
            return b_.getVoidTy();
        case DpiCType::Byte:
        case DpiCType::BitScalar:   // svBit
        case DpiCType::LogicScalar: // svLogic
            return b_.getInt8Ty();
        case DpiCType::Shortint:
            return b_.getInt16Ty();
        case DpiCType::Int:
            return b_.getInt32Ty();
        case DpiCType::Longint:
            return b_.getInt64Ty();
        case DpiCType::Real:
            return b_.getDoubleTy();
        case DpiCType::Shortreal:
            return b_.getFloatTy();
        case DpiCType::Chandle:
            return raw_ptr_ty(); // void*
        case DpiCType::String:
            return raw_ptr_ty(); // char* (input const char* by value; return char*)
        case DpiCType::BitVec:
        case DpiCType::LogicVec:
        case DpiCType::OpenArray:
            // A packed vector / open array is always passed by pointer and is
            // never a return type; declare_dpi_import emits the pointer directly,
            // so this is never reached for a by-value slot.
            throw BackendError{ "internal: DPI vec / open array has no by-value C type" };
    }
    throw BackendError{ "internal: unknown DPI C type" };
}

// A packed-vector DPI arg (svBitVecVal* / svLogicVecVal*), passed by pointer in
// every direction — as opposed to a by-value scalar.
bool Backend::is_dpi_vec(DpiCType ct) {
    return ct == DpiCType::BitVec || ct == DpiCType::LogicVec;
}

// Declare a DPI-C import (LRM 35) as an external C function with its native
// scalar signature (no `state` param, no plane-pair expansion). extern_name
// resolves at JIT link time against a --dpi-lib shared object. A `pure` import
// is side-effect-free, so mark it readnone + willreturn + nounwind so the
// optimizer can CSE / hoist / DCE the call.
llvm::Function* Backend::declare_dpi_import(FunctionId id) {
    const Function&          f   = mod_.function(id);
    auto*                    ptr = llvm::PointerType::getUnqual(ctx_);
    std::vector<llvm::Type*> params;
    params.reserve(f.dpi_arg_ctypes.size());
    for (size_t k = 0; k < f.dpi_arg_ctypes.size(); ++k) {
        const ArgDir   dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
        const DpiCType ct  = f.dpi_arg_ctypes[k];
        // A packed vector is a pointer in every direction (svBitVecVal* /
        // svLogicVecVal*); an open array is a pointer (svOpenArrayHandle); a
        // scalar is by value for input, by pointer for output / inout (the C
        // function writes through it).
        const bool by_ptr = is_dpi_vec(ct) || ct == DpiCType::OpenArray || dir != ArgDir::In;
        params.push_back(by_ptr ? ptr : dpi_c_llvm_ty(ct));
    }
    // An import TASK's C prototype returns `int` even though the SV task has no
    // return value (LRM 35.8: "imported tasks return an int value that is used
    // to indicate that the imported task has acknowledged a disable"). The SV
    // side never sees it -- dpi_ret_ctype stays Void, so lower_dpi_call binds no
    // result Plane and drops the value. Reading it is the disable protocol (LRM
    // 35.9), phase T3.
    llvm::Type* rty  = f.is_dpi_task ? b_.getInt32Ty() : dpi_c_llvm_ty(f.dpi_ret_ctype);
    auto* fnty   = llvm::FunctionType::get(rty, params, false);
    auto  callee = env_->llmod.getOrInsertFunction(f.extern_name, fnty);
    auto* fn     = llvm::dyn_cast<llvm::Function>(callee.getCallee());
    if (fn == nullptr) {
        throw BackendError{ "DPI import '" + f.extern_name +
                            "' conflicts with an existing symbol of a different type" };
    }
    if (f.is_pure) {
        fn->setDoesNotAccessMemory();
        fn->setWillReturn();
        fn->setDoesNotThrow();
    }
    return fn;
}

// Create (but do not yet lower) a function's declaration, so calls — including
// recursive and forward ones — resolve before any body is emitted.
llvm::Function* Backend::declare_function(FunctionId id) {
    const Function& f = mod_.function(id);
    if (f.is_dpi) {
        return declare_dpi_import(id);
    }
    if (f.is_task) {
        return declare_task(id);
    }
    auto*                    ptr = llvm::PointerType::getUnqual(ctx_);
    std::vector<llvm::Type*> params;
    params.push_back(ptr); // state
    // The caller's xoshiro256++ state (LRM 18.13). Unconditional rather than
    // gated on whether the callee draws: every entry of a vtable slot and every
    // $monitor body is called through ONE FunctionType, which no per-callee bit
    // can vary -- the monitor ramp pointer even round-trips through the C
    // runtime as a void*. One pointer register on a call that does not need it
    // is the price of that uniformity.
    params.push_back(ptr); // rng
    // The calling THREAD's process control block (LRM 9.7), so
    // `process::self()` inside a subroutine names the caller rather than the
    // callee -- a task or function is not a thread. Present only in a design
    // that names `process`, and uniform across the design for the same reason
    // `rng` is unconditional: one FunctionType per vtable slot.
    if (design_uses_process_) {
        params.push_back(ptr); // proc
    }
    // The calling THREAD's scheduling home region (LRM 24.3.1): "a section of
    // sequential code anywhere in the design or testbench inherits the
    // scheduling region of the thread that calls it". A FUNCTION cannot suspend
    // and so has no home of its own to answer for -- but LRM 13.4.4 lets one
    // spawn a `fork ... join_none` child, and that child is a thread whose park
    // sites need the answer. Sits beside `proc` because it is the same kind of
    // thing: the second of the two facts a callee inherits from its caller's
    // thread rather than from its own declaration. Gated design-wide, on the
    // same uniformity argument as `proc` (see home_param_index).
    if (design_uses_home_arg_) {
        params.push_back(llvm::Type::getInt8Ty(ctx_)); // home
    }
    // TODO: pass a 2-state formal as a single iW param (and a 2-state result as
    // a bare iW) instead of the packed (mm, z) pair. The wire is packed today —
    // a 2-state arg lifts to must==may with a constant-0 z, which LLVM folds —
    // so this is a call-ABI size micro-opt, not a correctness gap.
    for (size_t k = 0; k < f.param_types.size(); ++k) {
        const Ref     pt  = f.param_types[k];
        const ArgDir dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
        if (dir == ArgDir::Ref) {
            // The actual's storage pointer, plus -- only when some call site
            // binds this formal to a place inside a container's heap buffer --
            // the relocation epoch of that container, its value at the call, and
            // the site record for the staleness report (§3.0).
            params.push_back(ptr);                 // &actual storage
            for (uint32_t lvl = 0; lvl < ref_epoch_depth(f, k); ++lvl) {
                params.push_back(ptr);             // &epoch, outermost first
                params.push_back(b_.getInt64Ty()); // expected epoch
            }
            continue;
        }
        // A dynamic array or queue is one handle pointer regardless of
        // direction (the callee copies in / out through it); never a (mm, z) pair.
        if (is_handle_type(pt)) {
            params.push_back(ptr); // &actual (rt_dynarr* / rt_queue*)
            continue;
        }
        if ((dir == ArgDir::Out || dir == ArgDir::Inout) && !is_agg_abi(pt) && !is_string_type(pt)) {
            params.push_back(ptr); // &caller_temp.mm
            params.push_back(ptr); // &caller_temp.z (copy-out at each return)
            continue;
        }
        if (is_string_type(pt) || is_agg_abi(pt)) {
            // A string handle or an aggregate's storage passes by ONE pointer in
            // every direction: In copies in through it, an out / inout formal
            // copies the frame Local back out through the same pointer (a string
            // by rt_string_move, a struct-bearing aggregate by storage copy).
            // (A dynamic array is handled above, one pointer too.)
            params.push_back(ptr);
            continue;
        }
        const uint32_t w = width_of(pt);
        params.push_back(int_ty(2 * w)); // must|may
        params.push_back(int_ty(w));     // z
    }
    // A string, aggregate, dynamic-array, or queue result returns through a
    // trailing out-param the caller owns (its call-site temp) — no plane form.
    llvm::Type* rty = fn_return_ty(f.return_type);
    if (is_string_type(f.return_type) || is_agg_abi(f.return_type) ||
        is_handle_type(f.return_type)) {
        rty = b_.getVoidTy();
        params.push_back(ptr);
    }
    auto* fnty = llvm::FunctionType::get(rty, params, false);
    auto* fn   = llvm::Function::Create(fnty, llvm::Function::InternalLinkage,
                                        mod_.name + ".fn." + f.name, env_->llmod);
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    return fn;
}

// A task is a switched-resume coroutine. Its signature is:
//   ptr @<mod>.task.<name>(ptr noalias state, ptr rng, ptr wheel,
//                          [ptr proc,] [i8 home,] <per formal>, ptr record)
// where each input formal contributes (i(2W) mm, iW z) value planes, each
// output / inout formal contributes (ptr mm_ptr, ptr z_ptr) into a caller temp,
// and an unpacked aggregate formal contributes ONE storage pointer in every
// direction (like a function's, see declare_function).
// `record` is a { ptr parent } slot: the body stores the completing coroutine's
// continuation there (see lower_task / lower_task_call_inst). It returns the
// coroutine handle, like a process.
llvm::Function* Backend::declare_task(FunctionId id) {
    const Function&          f   = mod_.function(id);
    auto*                    ptr = llvm::PointerType::getUnqual(ctx_);
    std::vector<llvm::Type*> params;
    params.push_back(ptr); // state
    params.push_back(ptr); // rng
    params.push_back(ptr); // wheel
    if (design_uses_process_) {
        params.push_back(ptr); // proc — the calling thread's control block
    }
    // The calling thread's home region, LRM 24.3.1's "inherits the scheduling
    // region of the thread that calls it". This is the load-bearing one: a task
    // reached from both a program `initial` and an ordinary process is lowered
    // ONCE, so its park sites cannot fold a home, and the only authority on
    // which region the running thread belongs to is the caller. See
    // build_shared_home_fns for who actually reads it, and home_param_index for
    // why it is gated design-wide rather than per callee.
    if (design_uses_home_arg_) {
        params.push_back(llvm::Type::getInt8Ty(ctx_)); // home
    }
    for (size_t k = 0; k < f.param_types.size(); ++k) {
        if (f.arg_dirs[k] == ArgDir::Ref) {
            // The same shape declare_function builds, narrow or fat.
            params.push_back(ptr);                 // &actual storage
            for (uint32_t lvl = 0; lvl < ref_epoch_depth(f, k); ++lvl) {
                params.push_back(ptr);             // &epoch, outermost first
                params.push_back(b_.getInt64Ty()); // expected epoch
            }
        } else if (is_handle_type(f.param_types[k])) {
            params.push_back(ptr); // &actual (rt_dynarr* / rt_queue*), any direction
        } else if (is_agg_abi(f.param_types[k])) {
            // An unpacked struct / whole unpacked array: one pointer to the
            // caller's storage, in every direction. The prologue copies it into
            // the frame Local (emit_agg_copyin) and the completion epilogue
            // copies an output / inout Local back through it (emit_copyout), so
            // the pass-by-value copy straddles the task's suspends.
            params.push_back(ptr);
        } else if (is_string_type(f.param_types[k])) {
            // One rt_string handle pointer in every direction, like a container:
            // In deep-copies it into the frame slot, an out / inout formal moves
            // the frame slot back through it in the completion epilogue.
            params.push_back(ptr);
        } else if (f.arg_dirs[k] == ArgDir::In) {
            const uint32_t w = width_of(f.param_types[k]);
            params.push_back(int_ty(2 * w)); // must|may
            params.push_back(int_ty(w));     // z
        } else {
            params.push_back(ptr); // &caller_temp.mm
            params.push_back(ptr); // &caller_temp.z
        }
    }
    params.push_back(ptr); // record { ptr parent }
    auto* fnty = llvm::FunctionType::get(ptr, params, false);
    auto* fn   = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage,
                                        mod_.name + ".task." + f.name, env_->llmod);
    fn->setPresplitCoroutine();
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    return fn;
}

// How many LLVM parameters formal k occupies:
//   4 — a `ref` fat pointer { &storage, &epoch, expect, site }
//   1 — an aggregate storage pointer, a container handle pointer, or a string
//       handle (each one pointer in every direction)
//   2 — everything else: the (must|may, z) plane pair, or the
//       (&mm, &z) out / inout pointer pair
// How many relocation-epoch witness PAIRS ref formal k of `f` carries: one per
// container level some call site nests its actual inside, 0 for a ref to a
// variable, a fixed-array element, a struct member, a class property or a whole
// container HANDLE — storage that cannot move, so the callee emits no check
// (docs/design/data-model.md §3.0).
uint32_t Backend::ref_epoch_depth(const Function& f, size_t k) const {
    const auto it = fn_ids_.find(&f);
    // A function the map has not seen cannot be reasoned about; keep the guard,
    // and keep it at the one level every heap place has at minimum.
    return it == fn_ids_.end() ? 1u : facts_.ref_epoch_depth(it->second, k);
}

bool Backend::ref_needs_epoch(const Function& f, size_t k) const {
    return ref_epoch_depth(f, k) != 0;
}

uint32_t Backend::formal_param_count(const Function& f, size_t k) const {
    if (!f.arg_dirs.empty() && f.arg_dirs[k] == ArgDir::Ref) {
        // slot, plus (ptr epoch, i64 expect) per level.
        return 1u + 2u * ref_epoch_depth(f, k);
    }
    // An aggregate is one storage pointer, in every direction (function or task).
    if (is_agg_abi(f.param_types[k])) {
        return 1;
    }
    // A container handle is one pointer in every direction (input / output /
    // inout all pass &actual; the callee copies in/out through it).
    if (is_handle_type(f.param_types[k])) {
        return 1;
    }
    // A string is one rt_string handle pointer in every direction, like the other
    // Managed types (docs/design/data-model.md §6): In deep-copies through it,
    // out / inout moves the callee's frame slot back through it.
    if (is_string_type(f.param_types[k])) {
        return 1;
    }
    return 2;
}

// LLVM arg index of the gated `home` parameter (declare_function /
// declare_task): right after the gated `proc`, which is right after the fixed
// head -- `state, rng` for a function, `state, rng, wheel` for a task.
//
// Gated DESIGN-WIDE rather than per callee, exactly as `proc` is and for the
// same reason: a virtual call reaches its callee through one vtable slot, so
// every body that can occupy that slot must share one FunctionType, and no
// per-callee bit can vary it. The design-wide bit costs one i8 register on
// calls that ignore it, and only in a design that has a shared body at all.
uint32_t Backend::home_param_index(bool is_task) const {
    return (is_task ? 3u : 2u) + (design_uses_process_ ? 1u : 0u);
}

// LLVM arg index of each formal's first parameter for `f`'s signature shape:
// functions start after `state, rng`, tasks after `state, rng, wheel`. The
// record slot follows all formals.
std::vector<uint32_t> Backend::param_indices(const Function& f) const {
    std::vector<uint32_t> idx(f.param_types.size());
    // ... plus the gated `proc` and `home` parameters, which sit right after
    // them, in that order.
    uint32_t cur = home_param_index(f.is_task) + (design_uses_home_arg_ ? 1u : 0u);
    for (size_t k = 0; k < f.param_types.size(); ++k) {
        idx[k] = cur;
        cur += formal_param_count(f, k);
    }
    return idx;
}

// LLVM param index of the task's record slot: right after the last formal.
uint32_t Backend::task_record_param(const Function& f) const {
    const auto idx = param_indices(f);
    if (idx.empty()) {
        return home_param_index(/*is_task=*/true) + (design_uses_home_arg_ ? 1u : 0u);
    }
    return idx.back() + formal_param_count(f, f.param_types.size() - 1);
}

// Build a function's frame struct: one slot field per local, in Local-handle
// order (a Local's index is its GEP field index).
// Locals that cannot use the narrow iW slot (they stay packed `{ i(2W), iW }`):
// a local passed as a task-call output / inout argument, because the callee
// writes it back through the packed-word (&mm, &z) pointer ABI.
//
// A `ref` actual is deliberately NOT marked. A ref formal's own slot takes the
// canonical type-derived shape (narrow for 2-state, packed for 4-state), the
// same rule an aggregate leaf follows, so forcing the actual packed would
// instead GUARANTEE a mismatch — which is what a fixed-array element ref
// actual hit: the callee wrote an i64 through a narrow i32 element slot and
// overran into its neighbour. Matching by type on both sides is the invariant;
// see docs/design/data-model.md §3.
std::vector<bool> Backend::frame_out_targets(const Function& f) const {
    std::vector<bool> out(f.local_types.size(), false);
    const Ir&         ir = f.ir;
    const auto        mark_packed_args = [&](FunctionId fid, ExtraSlice argslice) {
        const Function& callee = mod_.function(fid);
        const auto      args   = ir.region(argslice);
        for (size_t a = 0; a < args.size(); ++a) {
            if (callee.arg_dirs.empty() || callee.arg_dirs[a] == ArgDir::In ||
                callee.arg_dirs[a] == ArgDir::Ref) {
                continue;
            }
            const Index root = lvalue_root(ir, args[a]).root;
            if (ir.tag(root) == Inst::Tag::Local) {
                out[ir.payload(root).local.index] = true;
            }
        }
    };
    for (size_t k = 0; k < ir.inst_count(); ++k) {
        const auto idx = static_cast<Index>(k);
        if (ir.tag(idx) == Inst::Tag::TaskCall) {
            const auto ti = ir.extra_data<TaskCallInfo>(ir.payload(idx).extra);
            mark_packed_args(ti.func, ti.args);
        } else if (ir.tag(idx) == Inst::Tag::TaskCallVirtual) {
            const auto vci = ir.extra_data<VirtualCallInfo>(ir.payload(idx).extra);
            mark_packed_args(vci.func, vci.args);
        } else if (ir.tag(idx) == Inst::Tag::Call) {
            const auto ci = ir.extra_data<CallInfo>(ir.payload(idx).extra);
            mark_packed_args(ci.func, ci.args);
        } else if (ir.tag(idx) == Inst::Tag::CallVirtual) {
            const auto vci = ir.extra_data<VirtualCallInfo>(ir.payload(idx).extra);
            mark_packed_args(vci.func, vci.args);
        } else if (ir.tag(idx) == Inst::Tag::Fork) {
            // Every fork capture is an ArgDir::In box HANDLE of every child
            // (lower/Statements.cpp's lower_fork), i.e. an rvalue, not a place at all, so
            // there is no frame slot to force packed. Nothing to mark.
            continue;
        }
    }
    return out;
}

void Backend::build_frame_layout(const Function& f) {
    const size_t n = f.local_types.size();
    frame_slots_.assign(n, FrameSlot{});
    ref_ptr_.assign(n, RefBinding{});
    const std::vector<bool>  out_target = frame_out_targets(f);
    std::vector<llvm::Type*> fields(n);
    for (uint32_t i = 0; i < n; ++i) {
        const Layout lay           = mod_.pool.layout(f.local_types[i]);
        frame_slots_[i].ir_type    = f.local_types[i];
        frame_slots_[i].width      = lay.width == 0 ? 1 : lay.width;
        frame_slots_[i].four_state = lay.domain == Domain::Four;
        // A ref formal (local k for formal k) has no own storage: its dead field
        // is kept for index alignment and loads / stores redirect to the passed
        // actual pointer. The redirect GEPs a packed slot, so force one here.
        const bool is_ref_formal = i < f.arg_dirs.size() && f.arg_dirs[i] == ArgDir::Ref;
        frame_slots_[i].is_ref   = is_ref_formal;
        // A fixed unpacked array or unpacked struct — including a ref formal of
        // one — takes the canonical aggregate storage mirror, so element and
        // member addressing, copy-in and whole-object copies all reuse the
        // aggregate path. For a ref formal the frame field stays dead and
        // local_lvalue re-bases onto the passed pointer, which the ABI
        // guarantees has this exact canonical layout.
        if (needs_agg_storage(f.local_types[i]) || is_agg_abi(f.local_types[i])) {
            frame_slots_[i].is_agg     = true;
            frame_slots_[i].storage_ty = agg_storage_ty(f.local_types[i]);
            fields[i]                  = frame_slots_[i].storage_ty;
            continue;
        }
        // A ref string formal aliases the caller's handle rather than owning
        // one, so it must not be init'd or deinit'd as a body string; the
        // is_string shape is still what load / store needs, and init_frame /
        // deinit_body_strings already skip every is_ref local.
        if (is_string_type(f.local_types[i])) {
            frame_slots_[i].is_string = true;
            frame_slots_[i].slot_ty   = str_slot_ty();
            fields[i]                 = frame_slots_[i].slot_ty;
            continue;
        }
        // A container local, including a ref formal of one: the ref's frame
        // field stays dead and local_lvalue re-bases onto the passed handle
        // pointer, but the handle slot shape is still what every consumer
        // needs. init_frame and deinit_body_strings skip every is_ref local, so
        // a borrowed handle is neither emptied on entry nor freed on exit.
        if (is_handle_type(f.local_types[i])) {
            frame_slots_[i].is_dynarr   = is_dynarr_type(f.local_types[i]);
            frame_slots_[i].is_queue    = is_queue_type(f.local_types[i]);
            frame_slots_[i].is_assoc    = is_assoc_type(f.local_types[i]);
            frame_slots_[i].handle_type = f.local_types[i];
            frame_slots_[i].slot_ty     = handle_slot_ty(f.local_types[i]);
            fields[i]                   = frame_slots_[i].slot_ty;
            continue;
        }
        // A 2-state local uses the genuine iW slot; the packed-word ABI wire is
        // bridged by copy-in/out. A 4-state local keeps its z field (the ABI and
        // copy-in/out use it; LLVM folds the zero-z traffic on its own). A local
        // passed as a task out/inout target stays packed — the callee writes it
        // back through the (mm, z) pointer ABI.
        //
        // A ref formal takes the CANONICAL type-derived shape, i.e. exactly what
        // an aggregate leaf of the same type gets (agg_leaf_slot_ty). That is the
        // one rule that makes both sides of an alias agree without either side
        // knowing the other: the actual is forced to the same shape (see
        // DesignFacts::ref_arg and frame_out_targets).
        frame_slots_[i].narrow = !frame_slots_[i].four_state && !out_target[i];
        // A non-narrow slot keeps its z field: a 4-state local needs it, and a
        // packed 2-state out-target local is written back through the (mm, z)
        // pointer ABI. A narrow local has no z.
        frame_slots_[i].slot_ty = slot_type(frame_slots_[i].width, frame_slots_[i].four_state,
                                            !frame_slots_[i].narrow, frame_slots_[i].narrow);
        fields[i]                  = frame_slots_[i].slot_ty;
    }
    frame_ty_ = llvm::StructType::create(ctx_, fields, mod_.name + ".fn." + f.name + ".frame");
}

// Power up the frame: 4-state locals come up X (must 0, may 1), 2-state 0; z
// is cleared. Formal-parameter locals are immediately overwritten by copy-in;
// an unwritten return variable thus reads X (its uninitialized value).
void Backend::init_frame() {
    for (uint32_t i = 0; i < frame_slots_.size(); ++i) {
        const FrameSlot& s    = frame_slots_[i];
        // A ref formal's field is dead (accesses redirect to the actual); no init.
        if (s.is_ref) {
            continue;
        }
        llvm::Value*     slot = b_.CreateStructGEP(frame_ty_, frame_, i);
        if (s.is_agg) {
            // Recursive fill (X into 4-state leaves, init string leaves); an
            // input formal's slot is then overwritten by copy-in.
            std::vector<llvm::Value*> path;
            emit_agg_init(s.ir_type, slot, path, s.storage_ty);
            // Any handle-bearing container leaf is now an empty container at a
            // stable frame address: a root for this activation. Paired with the
            // Remove in deinit_body_strings' matching arm.
            emit_gc_container_roots(GcRootOp::Add, s.ir_type, slot, s.storage_ty);
            continue;
        }
        if (s.is_string) {
            b_.CreateCall(str_init_fn(), { slot });
            continue;
        }
        if (s.is_dynarr || s.is_queue || s.is_assoc) {
            // Empty handle ({0}): the frame alloca is not zeroed, so store the
            // empty handle (null buffer pointer, zero length).
            b_.CreateStore(llvm::Constant::getNullValue(s.slot_ty), slot);
            // A container local whose elements can reach a class handle is a
            // root while this activation lives; nothing else names the
            // malloc'd payload (Gc.h:228-232). Paired with the Remove in
            // deinit_body_strings' matching arm.
            emit_gc_container_roots(GcRootOp::Add, s.ir_type, slot, s.slot_ty);
            continue;
        }
        // A narrow 2-state local is a single iW field (comes up 0); a 4-state
        // local is the packed word (X) plus a cleared z field.
        if (s.narrow) {
            b_.CreateStore(zero(s.width), b_.CreateStructGEP(s.slot_ty, slot, 0));
            continue;
        }
        llvm::Value*     mm   = s.four_state ? x_mm(s.width) : zero(2 * s.width);
        b_.CreateStore(mm, b_.CreateStructGEP(s.slot_ty, slot, 0));
        b_.CreateStore(zero(s.width), b_.CreateStructGEP(s.slot_ty, slot, 1));
    }
}

// Release every string owned by the current body: frame-local string slots
// plus the string rvalue temporaries. Emitted before each function `ret`, in
// a task's completion epilogue, and before a process's final suspend.
void Backend::deinit_body_strings() {
    for (uint32_t i = 0; i < frame_slots_.size(); ++i) {
        // A ref formal owns nothing: it borrows the actual's storage, which the
        // caller still owns and will release itself. Freeing through it would
        // destroy a live caller value.
        if (frame_slots_[i].is_ref) {
            continue;
        }
        if (frame_slots_[i].is_string) {
            b_.CreateCall(str_deinit_fn(), { b_.CreateStructGEP(frame_ty_, frame_, i) });
        } else if (frame_slots_[i].is_dynarr || frame_slots_[i].is_queue ||
                   frame_slots_[i].is_assoc) {
            // A dynamic-array / queue / associative-array local owns its heap
            // storage (value semantics): free it at every body exit. Unregister
            // BEFORE the free, or the collector holds an entry naming a released
            // ring -- the Add is in init_frame's matching arm.
            llvm::Value* slot = b_.CreateStructGEP(frame_ty_, frame_, i);
            emit_gc_container_roots(GcRootOp::Remove, frame_slots_[i].ir_type, slot,
                                    frame_slots_[i].slot_ty);
            emit_handle_delete(slot, frame_slots_[i].handle_type);
        } else if (frame_slots_[i].is_agg && has_heap_leaf(frame_slots_[i].ir_type)) {
            // An aggregate local with string / handle leaves owns their heap
            // buffers at any nesting depth: recursive teardown. Same ordering rule
            // -- unregister every container leaf before emit_agg_deinit frees it.
            llvm::Value* slot = b_.CreateStructGEP(frame_ty_, frame_, i);
            emit_gc_container_roots(GcRootOp::Remove, frame_slots_[i].ir_type, slot,
                                    frame_slots_[i].storage_ty);
            std::vector<llvm::Value*> path;
            emit_agg_deinit(frame_slots_[i].ir_type, slot, path, frame_slots_[i].storage_ty);
        }
    }
    for (llvm::Value* temp : str_temp_list_) {
        b_.CreateCall(str_deinit_fn(), { temp });
    }
    for (const auto& [temp, type] : dynarr_temp_list_) {
        emit_gc_container_roots(GcRootOp::Remove, type, temp, handle_slot_ty(type));
        emit_handle_delete(temp, type);
    }
    for (const auto& [temp, type] : agg_elem_temp_list_) {
        // A popped element was RELOCATED into this buffer, so whatever heap
        // storage it carries is ours to free: a nested container's buffer, or an
        // aggregate's string / container leaves (a POD-leaf element owns nothing).
        if (is_handle_type(type)) {
            emit_gc_container_roots(GcRootOp::Remove, type, temp, handle_slot_ty(type));
            emit_handle_delete(temp, type);
            continue;
        }
        if (!has_heap_leaf(type)) {
            continue;
        }
        // The aggregate case: agg_storage_ty is the same mirror build_agg_elem_temps
        // allocated the buffer as, so the container-leaf offsets match the Add's.
        emit_gc_container_roots(GcRootOp::Remove, type, temp, agg_storage_ty(type));
        std::vector<llvm::Value*> path;
        emit_agg_deinit(type, temp, path, agg_storage_ty(type));
    }
}

// Lower a function body into its (already declared) LLVM function. Reuses the
// shared instruction lowering — Arg reads a parameter, Local addresses the
// frame, Ret is a real return, Call invokes another function.
void Backend::lower_function(FunctionId id) {
    sched_anchor_ = SignalSlot::no_anchor; // functions are not a shareable group
    const Function& f = mod_.function(id);
    // A subroutine reached through a virtual interface handle addresses its
    // interface's signals through the instance formal 0 names, for the whole
    // body (LRM 25.9). Scoped to this lowering, and to that interface's anchor:
    // any other signal the body touches keeps the absolute form.
    const BodyCtx body_ctx(*this, f);
    if (f.is_dpi) {
        // A DPI import is only an external declaration (declare_dpi_import);
        // its body is native C, so there is nothing to lower.
        return;
    }
    ir_ = &f.ir;
    cur_fn_           = &f;
    // The body's LEXICAL home region (LRM 24.3.1): Reactive when reactive code
    // -- a program block, an assertion action block -- is what reaches it. Only
    // meaningful for a body one KIND of caller reaches; see reactive_home.
    in_reactive_home_fn_ = is_reactive_home_fn(id);
    cur_program_inst_    = program_exit_owner_of(id);
    // Take the home from the calling THREAD instead of folding it when the body
    // has no single lexical answer -- reached from both reactive and ordinary
    // code, LRM 24.3.1's "inherits the scheduling region of the thread that
    // calls it" -- or when an `expect` can move that thread's home mid-body
    // (LRM 16.17). See home_ptr.
    body_home_is_runtime_ = is_shared_home_fn(id) || ir_has_expect_wait(f.ir);
    home_ptr_         = nullptr;
    cur_is_task_      = false;
    param_llvm_index_ = param_indices(f);
    build_frame_layout(f);

    fn_            = fn_decls_[static_cast<uint32_t>(id)];
    const size_t n = ir_->inst_count();
    values_.assign(n, Plane{});
    lvals_.assign(n, std::optional<LValue>{});
    types_.resize(n);
    types_valid_.assign(n, 0);
    str_vals_.assign(n, nullptr);

    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    b_.SetInsertPoint(entry);
    state_ = fn_->getArg(0);
    state_->setName("state");
    // A function body runs inside the caller's thread, so its $urandom draws
    // come from the caller's state (LRM 18.13). Unlike a task's, this pointer
    // cannot outlive the call: a function is atomic.
    rng_slot_ = fn_->getArg(1);
    rng_slot_->setName("rng");
    // A function is not a thread either: `process::self()` inside one names the
    // caller, so the block travels as a parameter (LRM 9.7).
    pcb_ = nullptr;
    if (design_uses_process_) {
        pcb_ = fn_->getArg(2);
        pcb_->setName("proc");
    }
    // Nor does it have a home region of its own: a function cannot suspend, so
    // it only carries the calling thread's home onward -- to a `fork ...
    // join_none` child it spawns (LRM 13.4.4), whose park sites do need it.
    // Straight from the parameter, with no frame slot: there is no suspend for
    // the value to have to survive.
    home_arg_ = nullptr;
    if (design_uses_home_arg_) {
        home_arg_ = fn_->getArg(home_param_index(/*is_task=*/false));
        home_arg_->setName("home");
    }
    // A function takes no wheel parameter, but the clock is a global, so event
    // ops and any other time read resolve here exactly as they do in a task.
    wheel_ = wheel_gv_ != nullptr ? sym(wheel_gv_) : nullptr;

    frame_ = b_.CreateAlloca(frame_ty_, nullptr, "frame");
    // Ref formals bind FIRST: they emit no code, but build_str_temps resolves
    // local lvalues (to forward a concat straight into its target handle), and
    // a ref local's address is the passed pointer rather than a frame field.
    bind_ref_formals(f);
    init_frame();
    emit_agg_copyin(f);
    build_str_temps();
    build_dynarr_temps();
    build_agg_elem_temps();
    emit_copyin(f);
    arm_body_ctx();

    lower_region(ir_->root());
    // The frontend appends a trailing Ret, so the entry path always terminates;
    // guard defensively in case a body lowered to nothing (an implicit return
    // still copies output / inout formals back).
    if (b_.GetInsertBlock()->getTerminator() == nullptr) {
        emit_copyout(f);
        deinit_body_strings();
        // Check the LLVM return type, not the IR one: a string / dynamic-array
        // result is void at the ABI (it returns through a trailing out-param).
        if (fn_->getReturnType()->isVoidTy()) {
            b_.CreateRetVoid();
        } else {
            b_.CreateRet(llvm::ConstantAggregateZero::get(fn_->getReturnType()));
        }
    }

    cur_fn_ = nullptr;
    in_reactive_home_fn_  = false;
    cur_program_inst_     = kNoProgramInst;
    body_home_is_runtime_ = false;
    home_ptr_ = nullptr;
    home_arg_             = nullptr;
    frame_  = nullptr;
}

// Copy each aggregate input / inout formal's caller storage (passed by pointer)
// into its frame slot at entry (LRM 13.5 pass-by-value). Scalar / string formals
// copy in via emit_copyin, which reads their parameters directly; an aggregate
// formal's copy is the whole storage image, so it is done here. Shared by
// functions (entry) and tasks (the body's start block, after the ramp's initial
// suspend).
void Backend::emit_agg_copyin(const Function& f) {
    for (uint32_t k = 0; k < f.param_types.size(); ++k) {
        if (!is_agg_abi(f.param_types[k])) {
            continue;
        }
        // A ref formal has no frame storage to seed: it aliases the actual, so
        // there is nothing to copy either way.
        if (f.arg_dirs[k] == ArgDir::Ref) {
            continue;
        }
        // An output formal is written by the body, not seeded from the caller
        // (LRM 13.5): copy in only input / inout. Its frame Local starts at the
        // init_frame default until the body assigns it.
        if (f.arg_dirs[k] == ArgDir::Out) {
            continue;
        }
        emit_agg_storage_copy(b_.CreateStructGEP(frame_ty_, frame_, k),
                              fn_->getArg(param_llvm_index_[k]), f.param_types[k]);
    }
}

// Materialize the body-wide instance context at the current insert point (the
// entry block, so it dominates every use). No-op unless the body takes one.
void Backend::arm_body_ctx() {
    if (body_ctx_fn_ == nullptr) {
        return;
    }
    const SignalSlot& s = slot_of(body_ctx_fn_->ctx_member);
    // Formal 0 is the handle. Read it from its frame Local rather than the raw
    // argument: the parameter travels in the packed scalar ABI, and the copy-in
    // has already narrowed it to the 64-bit value the frame slot holds.
    llvm::Value* handle =
      b_.CreateLoad(int_ty(64), b_.CreateStructGEP(frame_ty_, frame_, 0), "vif.ctx");
    emit_null_handle_check(handle, Index{ 0 }, NullKind::Iface);
    ctx_override_        = b_.CreateIntToPtr(handle, raw_ptr_ty());
    ctx_override_anchor_ = s.anchor;
}

void Backend::lower_task(FunctionId id) {
    const Function& f = mod_.function(id);
    const BodyCtx   body_ctx(*this, f);
    ir_               = &f.ir;
    cur_fn_           = &f;
    // Same two answers a function's prologue records, and here they are the ones
    // that matter: a task suspends, so its park sites are where the home region
    // is actually spent. LRM 24.5's `module ... task T; #5; endtask` called from
    // a program is exactly the shared case body_home_is_runtime_ names.
    in_reactive_home_fn_  = is_reactive_home_fn(id);
    cur_program_inst_     = program_exit_owner_of(id);
    body_home_is_runtime_ = is_shared_home_fn(id) || ir_has_expect_wait(f.ir);
    home_ptr_         = nullptr;
    cur_is_task_      = true;
    param_llvm_index_ = param_indices(f);
    build_frame_layout(f);

    fn_            = fn_decls_[static_cast<uint32_t>(id)];
    const size_t n = ir_->inst_count();
    values_.assign(n, Plane{});
    lvals_.assign(n, std::optional<LValue>{});
    types_.resize(n);
    types_valid_.assign(n, 0);
    str_vals_.assign(n, nullptr);

    auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
    state_       = fn_->getArg(0);
    state_->setName("state");
    wheel_ = fn_->getArg(2);
    wheel_->setName("wheel");
    awaiter_ = nullptr;
    record_  = nullptr;
    // Reset per llvm::Function, like record_: a task may contain a fork, and the
    // slot is an alloca hoisted into THIS coroutine's frame. Keeping a previous
    // body's alloca would name storage in another function.
    fork_record_ = nullptr;
    disable_node_ = nullptr;
    fork_disable_nodes_.clear();
    spawn_sched_ = nullptr;
    block_nodes_.clear();
    event_wait_nodes_.clear();
    sync_nodes_.clear();

    auto* none     = llvm::ConstantTokenNone::get(ctx_);
    auto* null     = llvm::ConstantPointerNull::get(ptr_ty);
    auto* entry    = llvm::BasicBlock::Create(ctx_, "entry", fn_);
    auto* alloc_bb = llvm::BasicBlock::Create(ctx_, "coro.alloc", fn_);
    auto* begin_bb = llvm::BasicBlock::Create(ctx_, "coro.begin", fn_);

    b_.SetInsertPoint(entry);
    auto* cid  = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_id), { b_.getInt32(0), null, null, null });
    auto* need = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_alloc), { cid });
    // Over-allocate by coro.align: with an over-aligned frame (e.g. an i128
    // plane spill) CoroSplit lays out fields past coro.size unless the
    // allocation covers the realignment slack (LLVM coroutine contract).
    // Hoisted out of coro.alloc when this frame is registered, for the reason the
    // process ramp gives: the frame registry in coro.begin needs the byte count on
    // the elided path too.
    llvm::Value* size        = nullptr;
    llvm::Value* align       = nullptr;
    const auto   frame_bytes = [&] {
        size  = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_size, { b_.getInt64Ty() }), {});
        align = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_align, { b_.getInt64Ty() }), {});
    };
    // Register this frame only when it can hold a reference into the collected
    // heap (frame_may_hold_gc_ref); a task whose frame cannot holds nothing the
    // collector needs to find, so it pays neither the link nor the unlink.
    const bool gc_frame = frame_may_hold_gc_ref(f);
    if (gc_frame) {
        frame_bytes();
    }
    b_.CreateCondBr(need, alloc_bb, begin_bb);

    b_.SetInsertPoint(alloc_bb);
    if (!gc_frame) {
        frame_bytes();
    }
    auto* mem = b_.CreateCall(alloc_fn(), { b_.CreateAdd(size, align) });
    b_.CreateBr(begin_bb);

    b_.SetInsertPoint(begin_bb);
    auto* frame_phi = b_.CreatePHI(ptr_ty, 2);
    frame_phi->addIncoming(null, entry);
    frame_phi->addIncoming(mem, alloc_bb);
    hdl_           = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_begin), { cid, frame_phi });
    coro_begin_    = llvm::cast<llvm::Instruction>(hdl_);
    gc_frame_node_ = nullptr;
    if (gc_frame) {
        emit_gc_frame_link(size);
    }

    // Frame buffer and the task's own scheduled flag, placed just after
    // coro.begin so CoroSplit spills them into the coroutine frame (both live
    // across suspends; the frame's address escapes to callees). The flag backs
    // intra-body @event waits, exactly like a process's proc_sched slot.
    frame_     = b_.CreateAlloca(frame_ty_, nullptr, "frame");
    sched_ptr_ = b_.CreateAlloca(sched_word_ty(), nullptr, "task_sched");
    sched_anchor_      = SignalSlot::no_anchor;
    group_first_block_ = -1;
    // And, for a task some `disable` names, this activation's list node — same
    // placement and for the same reason: a `disable` reads it while we are parked.
    disable_node_ = f.disable_target != kNoDisableTarget
      ? b_.CreateAlloca(disable_node_ty(), nullptr, "disable_node") : nullptr;
    // The RNG (LRM 18.13). A plain task runs in its caller's thread, so it
    // draws straight from the caller's state; a body that can outlive the ramp
    // site -- a fork child, a deferred $strobe / $monitor / action body -- takes
    // one draw from it and seeds a state of its own here instead, both because
    // the pointer would dangle and because LRM 18.14.1 gives a spawned thread
    // its own generator seeded from its parent's. Placed with the frame allocas
    // so CoroSplit spills it, and in the RAMP so the seeding draw happens while
    // the ramping context is still on the stack.
    rng_slot_ = fn_->getArg(1);
    rng_slot_->setName("rng");
    if (rng_own_[static_cast<uint32_t>(id)] && rng_fns_[static_cast<uint32_t>(id)]) {
        llvm::Value* seed = emit_xoshiro_next(rng_slot_);
        rng_slot_ = b_.CreateAlloca(llvm::ArrayType::get(b_.getInt64Ty(), 4), nullptr, "urng");
        b_.CreateCall(urandom_seed_fn(), { rng_slot_, seed });
    }
    // Process control (LRM 9.7). A fork child IS a thread, so it takes a pooled
    // control block and links itself in as a child of the spawning thread -- that
    // link is the descendant relation `kill` / `disable fork` / `wait fork` walk.
    // A plain task activation is NOT a thread: it runs in its caller's, so it
    // carries the caller's block through unchanged (LRM 13.3).
    pcb_ = nullptr;
    if (design_uses_process_) {
        llvm::Value* caller = fn_->getArg(3);
        caller->setName("proc");
        pcb_ = f.is_fork_child ? b_.CreateCall(proc_alloc_fn(), { caller }) : caller;
        // A fork child owns its state (it was reseeded above), so it publishes its
        // own; a plain task activation publishes nothing -- the thread's block
        // already names the caller's state.
        if (f.is_fork_child) {
            emit_proc_publish_rng();
        }
    }
    // The calling thread's home region (LRM 24.3.1). Read in the RAMP, where the
    // caller's context is still live; home_ptr spills it into the frame at
    // coro.begin so it survives every suspend, and lower_task_call_inst hands
    // whatever is in that slot to the next callee down -- which is how "all
    // subsequent blocking statements in that thread" stays true through a chain
    // of task enables. A fork child inherits the same way: it is spawned with
    // the spawning thread's current home (lower_fork), not with a lexical one.
    home_arg_ = nullptr;
    if (design_uses_home_arg_) {
        home_arg_ = fn_->getArg(home_param_index(/*is_task=*/true));
        home_arg_->setName("home");
    }
    acts_node_ = nullptr;
    fork_live_ = nullptr;
    // A fork child enclosed in targeted blocks (Function::fork_targets) is an
    // activation of each of them (LRM 9.6.2: "all activities enabled within the named
    // block ... shall be terminated as well"), so it joins their lists HERE, in the
    // ramp, not when its body starts: a `join_none` / `join_any` sibling sits queued
    // before it has run, and a `disable` landing then must still find it. The nodes
    // publish a word held at 1 as their scheduled flag for that window, so the
    // force-wake marks the child without pushing it onto a ring it is already on; the
    // body replaces it with the real flag when it starts.
    if (f.is_fork_child && design_uses_disable_ && !f.fork_targets.empty()) {
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        spawn_sched_ = ab.CreateAlloca(sched_word_ty(), nullptr, "spawn_sched");
        b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), spawn_sched_);
        for (const uint32_t target : f.fork_targets) {
            llvm::Value* node = ab.CreateAlloca(disable_node_ty(), nullptr, "fork_node");
            emit_disable_link(target, node, spawn_sched_);
            fork_disable_nodes_.emplace_back(target, node);
        }
        // The link splits the ramp's block; the initial suspend below goes at its end.
        begin_bb = b_.GetInsertBlock();
    }

    auto* trap_bb    = llvm::BasicBlock::Create(ctx_, "coro.trap", fn_);
    auto* free_bb    = llvm::BasicBlock::Create(ctx_, "coro.free", fn_);
    coro_cleanup_bb_ = llvm::BasicBlock::Create(ctx_, "coro.cleanup", fn_);
    coro_ret_bb_     = llvm::BasicBlock::Create(ctx_, "coro.ret", fn_);

    b_.SetInsertPoint(coro_cleanup_bb_);
    if (gc_frame) {
        emit_gc_frame_unlink();
    }
    auto* freed = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_free), { cid, hdl_ });
    b_.CreateCondBr(b_.CreateICmpNE(freed, null), free_bb, coro_ret_bb_);

    b_.SetInsertPoint(free_bb);
    b_.CreateCall(free_fn(), { freed });
    b_.CreateBr(coro_ret_bb_);

    b_.SetInsertPoint(coro_ret_bb_);
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_end), { hdl_, b_.getFalse(), none });
    b_.CreateRet(hdl_);

    // Initial suspend: park after building the frame, return the handle without
    // running the body. The caller runs the body on the first resume (case 0).
    b_.SetInsertPoint(begin_bb);
    auto* start_bb = llvm::BasicBlock::Create(ctx_, "coro.start", fn_);
    auto* init     = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getFalse() });
    auto* sw_init  = b_.CreateSwitch(init, coro_ret_bb_, 2);
    sw_init->addCase(b_.getInt8(0), start_bb);
    sw_init->addCase(b_.getInt8(1), coro_cleanup_bb_);

    // The completion epilogue and the final suspend are shared by the
    // fall-through end and every explicit `return` (lower_ret branches here).
    auto* epilogue_bb = llvm::BasicBlock::Create(ctx_, "task.epilogue", fn_);
    auto* final_bb    = llvm::BasicBlock::Create(ctx_, "coro.final", fn_);
    task_epilogue_bb_ = epilogue_bb;

    // Body: initialize the frame, copy parameters in (per direction), then run.
    // The aggregate copy-in belongs here and not in the ramp: it reads the
    // caller's storage through the passed pointer, which is the call's input
    // value, and the body starts on the caller's first resume — the same instant
    // the call is made (LRM 13.5.1 copies in at the call).
    b_.SetInsertPoint(start_bb);
    bind_ref_formals(f);
    init_frame();
    emit_agg_copyin(f);
    build_str_temps();
    build_dynarr_temps();
    build_agg_elem_temps();
    emit_copyin(f);
    arm_body_ctx();
    // On the target's activation list before the body can park, off it again in
    // the epilogue: a `disable` must see every activation that is live, and none
    // that is not (LRM 9.6.2).
    if (f.disable_target != kNoDisableTarget) {
        emit_disable_link(f.disable_target, disable_node_);
    }
    emit_pcb_acts_link();
    if (!fork_disable_nodes_.empty()) {
        // The body is starting: publish the real scheduled flag in place of the one the
        // ramp held at 1 ...
        for (const auto& [target, node] : fork_disable_nodes_) {
            b_.CreateStore(sched_ptr_, b_.CreateStructGEP(disable_node_ty(), node, 4));
        }
        // ... and give up at once if a `disable` already ended this child while it was
        // queued. The mark is the only thing that reached it: it never parked, so no
        // other abort poll would run before its first statement.
        for (const auto& [target, node] : fork_disable_nodes_) {
            auto*        alive = llvm::BasicBlock::Create(ctx_, "fork.child.enabled", fn_);
            llvm::Value* mark  = b_.CreateLoad(b_.getInt32Ty(), b_.CreateStructGEP(disable_node_ty(), node, 5));
            b_.CreateCondBr(b_.CreateICmpNE(mark, b_.getInt32(0)), epilogue_bb, alive);
            b_.SetInsertPoint(alive);
        }
    }
    // A thread can be aborted before it has ever run: `kill`, `disable fork` and
    // LRM 24.3's program termination all mark a just-ramped fork child whose body
    // has not started, and the mark walk finds no activation to force-wake for it
    // -- pcb->acts is linked immediately above, in the BODY, so it is still null
    // while the child sits queued at its initial suspend. Without this test such a
    // child would go on to run everything up to its first park (printing, storing,
    // even forking threads of its own) after having been terminated, since every
    // other abort poll sits AFTER a suspending op. One load, in a fork child of a
    // process-control design only.
    if (f.is_fork_child && pcb_ != nullptr) {
        auto*        alive = llvm::BasicBlock::Create(ctx_, "fork.child.alive", fn_);
        llvm::Value* ctl   = b_.CreateLoad(b_.getInt32Ty(), pcb_field(pcb_, kPcbCtl));
        b_.CreateCondBr(
          b_.CreateICmpNE(b_.CreateAnd(ctl, b_.getInt32(kProcAbort)), b_.getInt32(0)), epilogue_bb, alive);
        b_.SetInsertPoint(alive);
    }
    lower_region(ir_->root());
    branch_to(epilogue_bb);

    // Epilogue: copy output / inout formals back through their pointers, then
    // resume the caller (the record's parent, if any) and fall into the final
    // suspend. In the synchronous path the caller never wrote a parent (it reads
    // the task `done` and resumes inline), so the load is null and nothing is
    // enqueued; in the blocked path the caller stored its handle before
    // suspending, so we push it onto the ring that handle names -- the caller
    // may be reactive code, and emit_wake_push is what reads which.
    b_.SetInsertPoint(epilogue_bb);
    // Off every list this child joined in its ramp, before anything can wake it.
    for (const auto& [target, node] : fork_disable_nodes_) {
        emit_disable_unlink(target, node);
    }
    if (f.disable_target != kNoDisableTarget) {
        emit_disable_unlink(f.disable_target, disable_node_);
        // A disabled activation skips copy-out and nothing else: LRM 9.6.2 leaves
        // "results of output and inout arguments" unspecified, and the caller
        // resume this epilogue owes is exactly what 9.6.2 asks for -- execution
        // continues after the task-enabling statement.
        auto* skip = llvm::BasicBlock::Create(ctx_, "task.copyout.skip", fn_);
        auto* copy = llvm::BasicBlock::Create(ctx_, "task.copyout", fn_);
        llvm::Value* aborted =
          b_.CreateLoad(b_.getInt32Ty(), b_.CreateStructGEP(disable_node_ty(), disable_node_, 5));
        b_.CreateCondBr(b_.CreateICmpNE(aborted, b_.getInt32(0)), skip, copy);
        b_.SetInsertPoint(copy);
        emit_copyout(f);
        b_.CreateBr(skip);
        b_.SetInsertPoint(skip);
    } else {
        emit_copyout(f);
    }
    deinit_body_strings(); // string locals + temps die with the call
    // Off the thread's activation stack before the frame goes away: a `kill`
    // walking it must never reach a dead frame.
    emit_pcb_acts_unlink();
    emit_fork_record_abandon();
    if (f.is_fork_child) {
        // A fork child is a THREAD, so this is where it stops existing (LRM 9.7):
        // publish FINISHED, hand its live children to its own parent, wake
        // everything in `p.await()` on it, and drop the thread's reference to the
        // block. Before the join bookkeeping below, so an awaiting thread and the
        // joining parent are queued in that order.
        emit_proc_complete();
    }
    llvm::Value* rec    = fn_->getArg(task_record_param(f));
    auto*        has_bb = llvm::BasicBlock::Create(ctx_, "task.wake", fn_);
    if (f.is_fork_child) {
        // Fork-child epilogue (LRM 9.3.2). Decrement the shared live count, then
        // resume the parent iff this completion is the one it is waiting for. A
        // null `parent` means nobody is parked -- the parent has not suspended yet
        // (a child finishing synchronously inside the spawn loop), has already
        // been resumed, or never parks at all (`join_none`).
        //
        // Under `join` the due completion is the LAST one, so the test is against
        // a constant 0 and the record needs neither wake_at nor a refcount: it is
        // a slot in the parent's frame, and the parent is still parked on us.
        // Under the detaching kinds the record is heap and the due count is
        // whatever the site stored in wake_at (W-1 for `join_any`, never reached
        // for `join_none`); since count only decreases, that is true at most once
        // and needs no claim flag. Those children also drop a reference on the way
        // out, because the parent may be long gone -- and once dropped, nothing
        // below may touch the record.
        const bool   heap   = f.fork_kind != ForkKind::Join || design_uses_process_;
        auto*        rty    = fork_record_ty(heap);
        auto*        cntp   = b_.CreateStructGEP(rty, rec, 0);
        llvm::Value* cnt    = b_.CreateSub(b_.CreateLoad(b_.getInt32Ty(), cntp), b_.getInt32(1));
        b_.CreateStore(cnt, cntp);
        llvm::Value* parent = b_.CreateLoad(ptr_ty, b_.CreateStructGEP(rty, rec, 1));
        llvm::Value* due    = heap
          ? b_.CreateICmpEQ(cnt, b_.CreateLoad(b_.getInt32Ty(), b_.CreateStructGEP(rty, rec, 2)))
          : b_.CreateICmpEQ(cnt, b_.getInt32(0));
        llvm::Value* wake   = b_.CreateAnd(due, b_.CreateICmpNE(parent, null));
        auto*        after  = heap ? llvm::BasicBlock::Create(ctx_, "fork.child.release", fn_)
                                   : final_bb;
        b_.CreateCondBr(wake, has_bb, after);
        b_.SetInsertPoint(has_bb);
        emit_wake_push(b_, parent);
        b_.CreateBr(after);
        if (heap) {
            b_.SetInsertPoint(after);
            emit_fork_record_release(b_, rec);
            b_.CreateBr(final_bb);
        }
    } else {
        llvm::Value* parent = b_.CreateLoad(ptr_ty, rec);
        b_.CreateCondBr(b_.CreateICmpNE(parent, null), has_bb, final_bb);
        b_.SetInsertPoint(has_bb);
        emit_wake_push(b_, parent);
        b_.CreateBr(final_bb);
    }

    b_.SetInsertPoint(final_bb);
    auto* res = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_suspend), { none, b_.getTrue() });
    auto* sw  = b_.CreateSwitch(res, coro_ret_bb_, 2);
    sw->addCase(b_.getInt8(0), trap_bb);
    sw->addCase(b_.getInt8(1), coro_cleanup_bb_);

    b_.SetInsertPoint(trap_bb);
    b_.CreateUnreachable();

    cur_fn_           = nullptr;
    in_reactive_home_fn_  = false;
    cur_program_inst_     = kNoProgramInst;
    body_home_is_runtime_ = false;
    home_ptr_         = nullptr;
    home_arg_             = nullptr;
    cur_is_task_      = false;
    frame_            = nullptr;
    task_epilogue_bb_ = nullptr;
}

// Record each ref formal's fat pointer into ref_ptr_ (Local k == formal k), so
// local_lvalue aliases the actual's storage live and can check the actual's
// container epoch on every access. Called in the callee prologue of both
// functions and tasks. A ref formal takes no copy-in / copy-out.
void Backend::bind_ref_formals(const Function& f) {
    for (uint32_t k = 0; k < f.param_types.size(); ++k) {
        if (f.arg_dirs.empty() || f.arg_dirs[k] != ArgDir::Ref) {
            continue;
        }
        const uint32_t p = param_llvm_index_[k];
        ref_ptr_[k].slot = fn_->getArg(p);
        for (uint32_t lvl = 0; lvl < ref_epoch_depth(f, k); ++lvl) {
            ref_ptr_[k].epochs.push_back(fn_->getArg(p + 1 + 2 * lvl));
            ref_ptr_[k].expects.push_back(fn_->getArg(p + 2 + 2 * lvl));
        }
        // `formal` names the binding in a staleness report. It is a property of
        // the CALLEE, so it is held here rather than shipped per call.
        ref_ptr_[k].formal = f.name + " argument " + std::to_string(k + 1);
    }
}

// Prologue (function and task): copy each formal's parameter into its frame
// Local (Local k == formal k). Inputs read their value planes; inouts load
// through their caller pointer; outputs keep the frame's default (X / 0) until
// the body assigns them. Ref formals are bound separately (bind_ref_formals) and
// skipped here.
void Backend::emit_copyin(const Function& f) {
    for (uint32_t k = 0; k < f.param_types.size(); ++k) {
        // An aggregate formal is passed by pointer and copied in by
        // emit_agg_copyin (function entry / task body start).
        if (is_agg_abi(f.param_types[k])) {
            continue;
        }
        const uint32_t w    = width_of(f.param_types[k]);
        const uint32_t base = param_llvm_index_[k];
        if (f.arg_dirs[k] == ArgDir::Ref) {
            continue;
        }
        if (is_handle_type(f.param_types[k])) {
            // Deep copy the caller's handle into the frame handle for input /
            // inout (value semantics; init_frame left the slot empty). Output
            // keeps the empty frame — it is filled by the body, copied out at
            // return.
            if (f.arg_dirs[k] == ArgDir::In || f.arg_dirs[k] == ArgDir::Inout) {
                emit_handle_assign(b_.CreateStructGEP(frame_ty_, frame_, k), fn_->getArg(base),
                                   f.param_types[k]);
            }
            continue;
        }
        if (is_string_type(f.param_types[k])) {
            // Deep copy the caller's handle into the frame local for input /
            // inout (value semantics; the frame slot was init'd by init_frame).
            // An output formal keeps that empty string — the body fills it and
            // the epilogue moves it out (LRM 13.5). Prologue copy-in is ABI
            // plumbing with no Index in scope, so the allocation reports its
            // location as "<unknown>".
            if (f.arg_dirs[k] == ArgDir::In || f.arg_dirs[k] == ArgDir::Inout) {
                b_.CreateCall(str_assign_fn(),
                              { b_.CreateStructGEP(frame_ty_, frame_, k),
                                fn_->getArg(base),
                                alloc_site_global(SourceText{ .where = "<unknown>" }, "string") });
            }
            continue;
        }
        if (f.arg_dirs[k] == ArgDir::In) {
            lower_store(local_lvalue(k), Plane{ fn_->getArg(base), fn_->getArg(base + 1) });
        } else if (f.arg_dirs[k] == ArgDir::Inout) {
            llvm::Value* mm = b_.CreateLoad(int_ty(2 * w), fn_->getArg(base));
            llvm::Value* z  = b_.CreateLoad(int_ty(w), fn_->getArg(base + 1));
            lower_store(local_lvalue(k), Plane{ mm, z });
        }
    }
}

// Copy each output / inout formal's frame Local back through its caller pointer
// (LRM 13.5 copy-out at completion). For a task this runs once in the completion
// epilogue; for a function it runs before every `ret` (each return point).
void Backend::emit_copyout(const Function& f) {
    for (uint32_t k = 0; k < f.param_types.size(); ++k) {
        // In needs no copy-out; a ref formal is live (aliased), never copied back.
        if (f.arg_dirs[k] == ArgDir::In || f.arg_dirs[k] == ArgDir::Ref) {
            continue;
        }
        const uint32_t base = param_llvm_index_[k];
        // An aggregate output / inout formal: copy the frame Local's storage back
        // through the actual's pointer (the reverse of emit_agg_copyin). For a
        // struct-bearing formal that pointer is the actual's own storage; for a
        // plain (flat-stored) array it is the caller's canonical temp, which the
        // frontend then copies into the actual (emit_call_copyouts).
        if (is_agg_abi(f.param_types[k])) {
            emit_agg_storage_copy(fn_->getArg(base),
                                  b_.CreateStructGEP(frame_ty_, frame_, k), f.param_types[k]);
            continue;
        }
        // A dynamic-array / queue output / inout formal deep-copies its frame
        // handle back into the caller's handle (the single passed pointer).
        if (is_handle_type(f.param_types[k])) {
            emit_handle_assign(fn_->getArg(base), b_.CreateStructGEP(frame_ty_, frame_, k),
                               f.param_types[k]);
            continue;
        }
        // A string output / inout formal transfers its frame handle into the
        // caller's (the single passed pointer): rt_string_move releases the
        // caller's old buffer, takes the frame slot's image whole, and re-inits
        // the frame slot. Legal here — and cheaper than a deep copy — because
        // copy-out is immediately followed by deinit_body_strings on every exit
        // path (function ret, task completion epilogue), so nothing reads the
        // frame slot again and its own deinit finds an empty string.
        if (is_string_type(f.param_types[k])) {
            b_.CreateCall(str_move_fn(), { fn_->getArg(base), b_.CreateStructGEP(frame_ty_, frame_, k) });
            continue;
        }
        // The caller's out-target is still the packed word; lift a narrow local.
        const Plane    v    = to_four(lower_load(local_lvalue(k)));
        b_.CreateStore(v.mm, fn_->getArg(base));
        b_.CreateStore(v.z, fn_->getArg(base + 1));
    }
}

// Lower a void `return` (no operand): a task branches to its copy-out +
// caller-resume epilogue; a void function copies out formals, releases the
// body's strings, and returns void.
void Backend::lower_ret_void() {
    if (cur_is_task_) {
        b_.CreateBr(leave_blocks(0, task_epilogue_bb_));
        return;
    }
    emit_copyout(*cur_fn_);
    deinit_body_strings();
    b_.CreateRetVoid();
}

// Lower a non-void function `return`: a real `ret`, so it terminates the
// block and any instructions after it in the region are unreachable
// (lower_region stops). The result is packed into the `{ i(2W), iW }` return
// struct (string / aggregate / handle results copy out through the trailing
// out-param instead).
void Backend::lower_ret(Index value) {
    // A function copies output / inout formals back through their pointers at
    // each return point (LRM 13.5), before releasing the body and returning.
    emit_copyout(*cur_fn_);
    if (is_string_type(cur_fn_->return_type) && !is_agg_abi(cur_fn_->return_type)) {
        // Copy the result out through the trailing out-param handle BEFORE
        // releasing the body's strings (the value may be one of them).
        b_.CreateCall(str_assign_fn(), { fn_->getArg(fn_->arg_size() - 1), str_of(value),
                                         alloc_site_global(loc_text(value), "string") });
        deinit_body_strings();
        b_.CreateRetVoid();
        return;
    }
    if (is_agg_abi(cur_fn_->return_type)) {
        // Copy the aggregate result (the Ret operand is its lvalue) out through
        // the trailing storage-pointer out-param the caller owns.
        emit_agg_ret_copy(fn_->getArg(fn_->arg_size() - 1), lvalue_of(value),
                          cur_fn_->return_type);
        deinit_body_strings();
        b_.CreateRetVoid();
        return;
    }
    if (is_handle_type(cur_fn_->return_type)) {
        // Deep-copy the returned handle (an lvalue) into the caller's result
        // handle (the trailing out-param), before freeing the body's handles
        // (the value may be a frame local or a call result temp). Resolved
        // through lv_handle_ptr so returning a struct member, class property
        // or container element copies THAT handle, not its parent's root.
        emit_handle_assign(
          fn_->getArg(fn_->arg_size() - 1), lv_handle_ptr(lvalue_of(value), HandleUse::Read), cur_fn_->return_type);
        deinit_body_strings();
        b_.CreateRetVoid();
        return;
    }
    // The return ABI is the packed (must|may, z) pair; lift a 2-state result
    // (a real result returns as its bit-pattern planes).
    Plane pl = value_of(value);
    pl       = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
    auto* rty = llvm::cast<llvm::StructType>(fn_->getReturnType());
    llvm::Value* agg = llvm::UndefValue::get(rty);
    agg              = b_.CreateInsertValue(agg, pl.mm, { 0 });
    agg              = b_.CreateInsertValue(agg, pl.z, { 1 });
    deinit_body_strings();
    b_.CreateRet(agg);
}

// Lower a `call`: pass the state pointer plus each argument as its (must|may,
// z) plane pair (the frontend has resized each to its parameter width), then
// unpack the returned struct into a Plane. A void callee yields no value.
void Backend::lower_call_inst(Index i) {
    const auto      ci = ir_->extra_data<CallInfo>(ir_->payload(i).extra);
    const Function& f  = mod_.function(ci.func);
    if (f.is_dpi) {
        lower_dpi_call(i, ci, f);
        // A DPI import needs the post-call rt_finishing test as much as an SV
        // callee does: its native C can call back into an `export "DPI-C"`
        // subroutine whose SV body reaches $finish, and compute_may_finish
        // models exactly that edge. Without this the finish would set the flag
        // and then the caller would run on past the call site.
        emit_finish_check(callee_may_finish(ci.func));
        return;
    }
    llvm::Function* cf = sym(fn_decls_[static_cast<uint32_t>(ci.func)]);
    lower_call_common(i, f, cf->getFunctionType(), cf, ci.args);
    emit_finish_check(callee_may_finish(ci.func));
}

// Lower a virtual method call (LRM 8.20): resolve the callee from the receiver
// object's vtable, then share the direct-call marshaling. The receiver handle
// is the first argument operand; its object body holds the vtable pointer at
// field 0, and slot `vci.slot` selects the concrete override.
// Load the callee (function or task ramp) for a virtual dispatch from the
// receiver object's vtable: handle (arg 0) -> object body -> vtable pointer
// (body field 0) -> slot. Shared by virtual function and virtual task calls.
// The receiver takes the same null guard a property access does: LRM 8.4 puts
// a virtual method reached through a null handle in exactly the class of
// illegal accesses, and the vtable load is the first thing that would fault.
llvm::Value* Backend::load_vtable_callee(const VirtualCallInfo& vci, Index at) {
    const std::span<const Index> args   = ir_->region(vci.args);
    llvm::Value*                 handle = must_of(value_of(args[0]));
    emit_null_handle_check(handle, at);
    llvm::Value* body = b_.CreateIntToPtr(handle, raw_ptr_ty());
    llvm::Value* vptr = b_.CreateLoad(
        raw_ptr_ty(), b_.CreateStructGEP(class_body_ty(vci.class_ref), body, 0));
    if (vci.iface_index != kNoIface) {
        // A dispatch through an interface-class handle (LRM 8.26) indexes the
        // interface-method table instead, which sits at a design-wide constant
        // NEGATIVE offset from the vptr. Same instruction count as an ordinary
        // virtual call: one vptr load, one callee load, both at constant offsets
        // (docs/design/interface-classes.md §5.3).
        const int64_t off = desc_field_vptr_offset(/*field=*/0) +
                            static_cast<int64_t>(vci.iface_index) * 8;
        llvm::Value* entry = b_.CreateGEP(b_.getInt8Ty(), vptr, b_.getInt64(off));
        return b_.CreateLoad(raw_ptr_ty(), entry);
    }
    llvm::Value* slot = b_.CreateGEP(raw_ptr_ty(), vptr, { b_.getInt32(vci.slot) });
    return b_.CreateLoad(raw_ptr_ty(), slot);
}

void Backend::lower_virtual_call_inst(Index i) {
    const auto      vci  = ir_->extra_data<VirtualCallInfo>(ir_->payload(i).extra);
    const Function& f    = mod_.function(vci.func); // signature shared by all overrides
    llvm::FunctionType* fnty =
        fn_decls_[static_cast<uint32_t>(vci.func)]->getFunctionType();
    lower_call_common(i, f, fnty, load_vtable_callee(vci, i), vci.args);
    emit_finish_check(virtual_call_may_finish());
}

// Shared marshaling + result handling for a direct or a virtual method call
// (they differ only in how the callee is obtained). Passes the state and RNG
// pointers plus each argument as its (must|may, z) plane pair (the frontend has
// resized each to its parameter width), calls `callee` with `fnty`, and unpacks
// the returned struct into a Plane. A void callee yields no value.
void Backend::lower_call_common(Index               i,
                                const Function&     f,
                                llvm::FunctionType* fnty,
                                llvm::Value*        callee,
                                ExtraSlice          arg_slice) {
    // Every arm of the marshaling loop indexes the signature by the actual's
    // position, so an actual list that is not exactly as long as `param_types`
    // reads a neighbouring formal's direction, or off the end of the vector.
    // Checked once here rather than trusted at each of the seven indexing sites:
    // the frontend splicing a receiver the callee has no formal for is exactly
    // how it went wrong, and it cost a segfault with no diagnostic.
    if (ir_->region(arg_slice).size() != f.param_types.size()) {
        throw BackendError{ std::format("internal: call to '{}' passes {} arguments for {} formals",
                                        f.name, ir_->region(arg_slice).size(),
                                        f.param_types.size()) };
    }
    std::vector<llvm::Value*> argv;
    argv.push_back(state_);
    // A function body runs in the caller's thread, so $urandom inside one
    // continues the caller's stream (LRM 18.13, 18.14.1).
    argv.push_back(rng_slot_);
    // A function runs in its caller's thread, so `process::self()` inside one
    // names the caller (LRM 9.7).
    if (design_uses_process_) {
        argv.push_back(proc_arg());
    }
    // ... and its scheduling region, for the same reason (LRM 24.3.1). A
    // function cannot suspend, so this only matters for a `fork ... join_none`
    // child it spawns.
    if (design_uses_home_arg_) {
        argv.push_back(callee_home());
    }
    // A ref actual that is a signal with waiters: the callee writes it live, so
    // its wait queue must be serviced with the call's net old -> new transition.
    // Snapshot each before the (atomic, zero-time) function call; service after.
    std::vector<std::pair<LValue, ServiceSnapshot>> ref_svc;
    size_t k = 0;
    for (Index a : ir_->region(arg_slice)) {
        const ArgDir dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
        if (dir == ArgDir::Ref) {
            // The fat ref: aliased storage, the actual's container epoch, that
            // epoch now, and the site the callee reports a stale ref through.
            // Every storage class goes through here — a plane slot, a managed
            // handle, an aggregate image — so `ref` has one shape.
            const LValue&    lv = lvalue_of(a);
            // A `ref` formal is a raw interior pointer into the actual's storage that
            // the CALLEE writes through, and the callee has no owner identity to
            // remember: a `ref` bound to a class property is exactly the "reference
            // the frame holds with no class-typed value anywhere in sight" that
            // frame_may_hold_gc_ref's door 3 and design §8's prerequisite 2 are
            // about. So the barrier goes HERE, at the one place the owner is known.
            //
            // Airtight for a function: a function cannot suspend and the only
            // safepoint is the drain loop's quiescent edge (:11542), so no collection
            // can consume the remembered-set entry between this barrier and the
            // callee's store. lower_task_call_inst has the same barrier for the same
            // reason, where that argument is weaker -- see the note there.
            emit_gc_barrier_for(lv);
            const RefBinding rb = ref_actual_arg(lv);
            argv.push_back(rb.slot);
            push_ref_epochs(argv, rb, ref_epoch_depth(f, k));
            if (!is_handle_type(f.param_types[k])) {
                ref_svc.emplace_back(lv, service_snapshot(lv));
            } else if ((lv.chg != nullptr && lv.chg->container) || lv.is_signal ||
                       lv.mut_sig_valid) {
                // A `ref` to a waited-on container property: the callee mutates
                // through a raw aliased pointer and never copies back, so the
                // notify can only be at the call's RETURN. The fire is then one
                // per call rather than one per mutation inside it, which a level
                // `wait` absorbs -- it re-tests the condition once (LRM 9.4.3).
                ref_svc.emplace_back(lv, ServiceSnapshot{ .container = true });
            }
        } else if (is_handle_type(f.param_types[k])) {
            // A dynamic array / queue / associative array passes one handle
            // pointer (the actual) in every by-value direction; the callee
            // copies in / out through it.
            const LValue& alv = lvalue_of(a);
            argv.push_back(lv_handle_ptr(alv, HandleUse::Place));
            // An out / inout container actual is filled BY THE CALLEE through this
            // handle pointer, so the owner acquires whatever references the callee's
            // elements hold. An `In` container is only read. Same reasoning as the
            // ref case above.
            if (dir == ArgDir::Out || dir == ArgDir::Inout) {
                emit_gc_barrier_for(alv);
                if ((alv.chg != nullptr && alv.chg->container) || alv.is_signal ||
                    alv.mut_sig_valid) {
                    ref_svc.emplace_back(alv, ServiceSnapshot{ .container = true });
                }
            }
        } else if (is_string_type(f.param_types[k]) && !is_agg_abi(f.param_types[k])) {
            // One rt_string handle pointer in every direction (data-model.md §6):
            // In passes the actual's handle and the callee deep-copies it into
            // its frame slot; out / inout passes the caller TEMP's handle, which
            // the callee moves its frame slot into at completion (the frontend
            // then copies the temp to the actual — emit_call_copyouts). No GC
            // barrier: a string leaf can never hold a class handle, so the
            // caller's storage acquires no reference through this write.
            argv.push_back(dir == ArgDir::In ? str_of(a) : lv_string_ptr(lvalue_of(a), StringUse::Place));
        } else if ((dir == ArgDir::Out || dir == ArgDir::Inout) && !is_agg_abi(f.param_types[k])) {
            // An output / inout scalar temp: pass pointers to its mm and z
            // planes; the callee copies out through them (the caller copies the
            // temp back to the actual after the call — frontend
            // emit_call_copyouts).
            const LValue& lv = lvalue_of(a);
            argv.push_back(lv_mm_ptr(lv));
            argv.push_back(lv_z_ptr(lv));
        } else if (is_agg_abi(f.param_types[k])) {
            // An aggregate actual passes its storage pointer, one per formal in
            // every direction (the frontend emitted it as an lvalue): In copies
            // it in, a struct-bearing out / inout copies the frame Local back
            // out through the same pointer.
            argv.push_back(agg_arg_ptr(lvalue_of(a)));
            // A struct-bearing out / inout formal is copied back out through THIS
            // pointer by the callee itself (emit_copyout, :5706-5708), so if the
            // actual is a class property the callee writes handles into an object it
            // cannot name. Same reasoning as the ref case above.
            if (dir == ArgDir::Out || dir == ArgDir::Inout) {
                emit_gc_barrier_for(lvalue_of(a));
            }
        } else {
            Plane pl = value_of(a);
            pl       = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
            argv.push_back(pl.mm);
            argv.push_back(pl.z);
        }
        ++k;
    }
    if (is_agg_abi(f.return_type)) {
        // Safe-temp aggregate return: the callee copies its result into a caller
        // temp we pass as the trailing out-param; the call result is that temp
        // lvalue (the frontend copies it into the target). A plane-only result is
        // a plain stack alloca — its byte image is its value and it owns nothing.
        // A MANAGED-LEAF result instead uses the body-lifetime buffer
        // build_agg_elem_temps made for this call: the callee's copy-out is
        // per-leaf, so it needs constructed leaves to assign over, and the heap
        // it hands back has to be released at a body exit rather than leaked with
        // the stack slot.
        auto*        st  = agg_storage_ty(f.return_type);
        llvm::Value* tmp = agg_elem_ret_[static_cast<uint32_t>(i)];
        if (tmp == nullptr) {
            tmp = b_.CreateAlloca(st, nullptr, "call.agg.ret");
        }
        argv.push_back(tmp);
        b_.CreateCall(fnty, callee, argv);
        LValue lv{};
        lv.base    = tmp;
        lv.root_ty = st;
        lv.is_agg  = true;
        set_lvalue(i, lv);
        return;
    }
    if (is_string_type(f.return_type)) {
        // The callee assigns the result into this call's temp handle (the
        // trailing out-param); str_of(i) was pre-set by build_str_temps.
        argv.push_back(str_of(i));
        b_.CreateCall(fnty, callee, argv);
        for (auto& [lv, snap] : ref_svc) {
            service_after_write(lv, snap);
        }
        return;
    }
    if (is_handle_type(f.return_type)) {
        // The callee copies its return handle into this call's result handle
        // (the trailing out-param, alloc'd by build_dynarr_temps). Publish it as
        // the call's lvalue so downstream consumers (assign / .size() / element)
        // read it directly.
        llvm::Value* temp = dynarr_ret_[static_cast<uint32_t>(i)];
        LValue       lv;
        lv.base    = temp;
        lv.slot_ty = handle_slot_ty(f.return_type);
        set_lvalue(i, lv);
        argv.push_back(temp);
        b_.CreateCall(fnty, callee, argv);
        for (auto& [lv2, snap] : ref_svc) {
            service_after_write(lv2, snap);
        }
        return;
    }
    llvm::Value* r = b_.CreateCall(fnty, callee, argv);
    for (auto& [lv, snap] : ref_svc) {
        service_after_write(lv, snap);
    }
    if (f.return_type != mod_.pool.void_type) {
        Plane         pl{ b_.CreateExtractValue(r, { 0 }), b_.CreateExtractValue(r, { 1 }) };
        const ValType t = from_ref(f.return_type);
        if (t.is_real) {
            pl = real_plane(planes_to_real(pl, t.width));
        }
        set_value(i, pl);
    }
}

// Lower a task enable (LRM 13.3). Create the task coroutine, copy inputs in /
// pass output-inout temp pointers, drive it once, and either finish inline (the
// task consumed no time) or park the caller until the task completes and resumes
// it. Only valid inside a coroutine — the frontend never emits a TaskCall in a
// plain function.
void Backend::lower_task_call_inst(Index i) {
    // Direct (TaskCall) vs virtual (TaskCallVirtual) differ only in how the task
    // ramp callee is obtained; a virtual task loads it from the receiver's vtable
    // (the slot holds the task ramp pointer, like a function). The FunctionType
    // comes from the resolved method's declared task signature (shared by every
    // override).
    const bool          virt = ir_->tag(i) == Inst::Tag::TaskCallVirtual;
    const FunctionId    fid  = virt ? ir_->extra_data<VirtualCallInfo>(ir_->payload(i).extra).func
                                    : ir_->extra_data<TaskCallInfo>(ir_->payload(i).extra).func;
    const ExtraSlice    argslice = virt ? ir_->extra_data<VirtualCallInfo>(ir_->payload(i).extra).args
                                        : ir_->extra_data<TaskCallInfo>(ir_->payload(i).extra).args;
    const Function&     tf   = mod_.function(fid);
    llvm::FunctionType* fnty = fn_decls_[static_cast<uint32_t>(fid)]->getFunctionType();
    llvm::Value*        cf =
        virt ? load_vtable_callee(ir_->extra_data<VirtualCallInfo>(ir_->payload(i).extra), i)
             : static_cast<llvm::Value*>(sym(fn_decls_[static_cast<uint32_t>(fid)]));
    auto*           ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto*           null   = llvm::ConstantPointerNull::get(ptr_ty);

    // Reset the record so a synchronous task reads a null parent (no enqueue).
    llvm::Value* record = record_slot();
    b_.CreateStore(null, record);

    // Ramp arguments: state, rng, wheel, per-formal planes / pointers, record.
    // A task runs in the calling process's thread (LRM 13.3), so it draws from
    // the caller's RNG rather than one of its own.
    const std::span<const Index> args = ir_->region(argslice);
    // The same signature-arity check lower_call_common makes, for the same
    // reason: the loop below indexes tf.arg_dirs / tf.param_types by the
    // actual's position, and a task enable is marshaled from the same
    // frontend argument list a function call is.
    if (args.size() != tf.param_types.size()) {
        throw BackendError{ std::format("internal: task enable of '{}' passes {} arguments for {} formals",
                                        tf.name, args.size(), tf.param_types.size()) };
    }
    std::vector<llvm::Value*>    argv;
    argv.push_back(state_);
    argv.push_back(rng_slot_);
    argv.push_back(wheel_);
    // A task runs in its caller's thread too, so it carries the caller's block.
    if (design_uses_process_) {
        argv.push_back(proc_arg());
    }
    // ... and its home region, which is the whole of LRM 24.5: "when a task
    // within a design module is called from a program, it shall use the reactive
    // region set for its scheduling activities". callee_home() is this thread's
    // answer at this call, so a task calling a task passes on what it inherited
    // rather than what its own declaration would say.
    if (design_uses_home_arg_) {
        argv.push_back(callee_home());
    }
    // Snapshot each ref-actual signal with waiters before the call; the wait
    // queue is serviced at the continuation with the net old -> new transition.
    // NOTE: a task that writes the same ref signal across a #delay (multiple time
    // steps) wakes level/edge waiters only once, at task completion, with the net
    // transition — not per intermediate write. Correct for a whole-signal ref
    // written in one time step (the common case) and for functions (atomic).
    // Per-write mid-suspend waking needs callee-side servicing (a generic
    // runtime wake keyed by signal id); TODO once that exists.
    std::vector<std::pair<LValue, ServiceSnapshot>> ref_svc;
    // Outer containers registered as dynamically-scoped roots for this call, removed
    // at cont_bb. Empty unless a ref actual aims inside a container a class object
    // owns, which is the one shape neither the barrier nor the frame scan covers.
    std::vector<llvm::Value*> ref_containers;
    for (uint32_t k = 0; k < args.size(); ++k) {
        if (tf.arg_dirs[k] == ArgDir::Ref) {
            // The fat ref, as at a function call: aliased storage, the actual's
            // container epoch, that epoch now, and the report site. A task is
            // where the epoch earns its keep — it can suspend, and the container
            // can be restructured by another process while the ref stays live.
            const LValue&    lv = lvalue_of(args[k]);
            // The same barrier lower_call_common emits, for the same reason: the
            // callee writes through a raw interior pointer and cannot name the owner.
            //
            // REQUIRED, not belt-and-braces. Here the dominance argument that makes
            // it airtight for a function does not hold -- a task can suspend, so a
            // collection can run between this barrier and the callee's store and
            // consume the remembered-set entry. Two separate windows result, and this
            // barrier is the only thing covering the first:
            //
            //   no collection intervenes (a task that never suspends before storing)
            //     -- nothing else logs the object at all, so this call is load-bearing;
            //   a collection intervenes
            //     -- the collector re-enters every conservatively-reached object into
            //        the remembered set for the next collection
            //        (relog_conservative_roots in Gc.cpp, for young AND full, since a
            //        full collection also clears LOGGED and drops the set). To perform
            //        the store after resuming, the activation must hold the interior
            //        pointer across the suspend that PRECEDES the store, so CoroSplit
            //        keeps it in the frame, the frame scan resolves it
            //        (rt_gc_find_object resolves interior pointers, Gc.h:433-439) and
            //        it is re-logged there.
            //
            // What does NOT close it, and was an earlier draft of this comment: a
            // conservatively-reached OLD object is not traced in a young collection --
            // mark_and_push refuses OLD uniformly, so the frame scan pins and stops.
            // Tracing it would not have sufficed anyway, since the pointer need not
            // still be live at the first safepoint AFTER the store.
            emit_gc_barrier_for(lv);
            // A ref aiming INSIDE a container this object owns hands the callee a
            // payload-interior pointer, which the frame scan cannot resolve, so the
            // re-log above cannot carry the owner across the suspend. Register the
            // OUTER container for the call's duration instead; see
            // emit_gc_ref_container_add for why it is the outer one and how it both
            // reaches the elements and keeps the owner alive. Paired at cont_bb,
            // which every completion and resume path below reaches.
            if (llvm::Value* root = emit_gc_ref_container_add(lv); root != nullptr) {
                ref_containers.push_back(root);
            }
            const RefBinding rb = ref_actual_arg(lv);
            argv.push_back(rb.slot);
            push_ref_epochs(argv, rb, ref_epoch_depth(tf, k));
            if (!is_handle_type(tf.param_types[k])) {
                ref_svc.emplace_back(lv, service_snapshot(lv));
            } else if ((lv.chg != nullptr && lv.chg->container) || lv.is_signal ||
                       lv.mut_sig_valid) {
                // See the function-call site: a `ref` container is mutated
                // through a raw pointer, so the notify lands at cont_bb, past
                // every suspend the task took.
                ref_svc.emplace_back(lv, ServiceSnapshot{ .container = true });
            }
        } else if (is_handle_type(tf.param_types[k])) {
            // One handle pointer for every by-value direction; callee copies
            // in / out through it.
            const LValue& alv = lvalue_of(args[k]);
            argv.push_back(lv_handle_ptr(alv, HandleUse::Place));
            if (tf.arg_dirs[k] == ArgDir::Out || tf.arg_dirs[k] == ArgDir::Inout) {
                emit_gc_barrier_for(alv);
                if ((alv.chg != nullptr && alv.chg->container) || alv.is_signal ||
                    alv.mut_sig_valid) {
                    ref_svc.emplace_back(alv, ServiceSnapshot{ .container = true });
                }
            }
        } else if (is_agg_abi(tf.param_types[k])) {
            // An unpacked struct / whole unpacked array actual: one storage
            // pointer for every direction (the frontend emitted it as an lvalue —
            // the actual itself for a struct-bearing formal, a canonical temp for
            // a flat array). The callee copies in at its body start and copies an
            // output / inout back at completion, so the caller must keep the
            // storage alive across the task's suspends: both shapes are frame
            // Locals / signals, never a short-lived alloca.
            argv.push_back(agg_arg_ptr(lvalue_of(args[k])));
            if (tf.arg_dirs[k] == ArgDir::Out || tf.arg_dirs[k] == ArgDir::Inout) {
                emit_gc_barrier_for(lvalue_of(args[k]));
            }
        } else if (is_string_type(tf.param_types[k])) {
            // One rt_string handle pointer in every direction: In passes the
            // actual's handle (deep-copied into the frame slot at body start),
            // out / inout passes the caller temp's handle, which the completion
            // epilogue moves the frame slot into. The temp is a caller frame Local
            // or a module signal — never a short-lived alloca — so the pointer
            // stays valid across the task's suspends, like the aggregate case
            // above. No GC barrier: a string can never hold a class handle.
            argv.push_back(tf.arg_dirs[k] == ArgDir::In ? str_of(args[k])
                                                        : lv_string_ptr(lvalue_of(args[k]), StringUse::Place));
        } else if (tf.arg_dirs[k] == ArgDir::In) {
            Plane pl = value_of(args[k]);
            pl       = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
            argv.push_back(pl.mm);
            argv.push_back(pl.z);
        } else {
            // An output / inout temp: pass pointers to its mm and z planes.
            const LValue& lv = lvalue_of(args[k]);
            argv.push_back(lv_mm_ptr(lv));
            argv.push_back(lv_z_ptr(lv));
        }
    }
    argv.push_back(record);

    llvm::Value* task_hdl = b_.CreateCall(fnty, cf, argv);
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { task_hdl });
    llvm::Value* done = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { task_hdl });

    auto* sync_bb  = llvm::BasicBlock::Create(ctx_, "task.sync", fn_);
    auto* async_bb = llvm::BasicBlock::Create(ctx_, "task.async", fn_);
    auto* cont_bb  = llvm::BasicBlock::Create(ctx_, "task.cont", fn_);
    b_.CreateCondBr(done, sync_bb, async_bb);

    // Synchronous: the task ran to completion with no time. It already copied
    // outputs back; destroy it and continue inline.
    b_.SetInsertPoint(sync_bb);
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { task_hdl });
    b_.CreateBr(cont_bb);

    // Blocked: park our handle so the task resumes us, into the ring that
    // handle names, when it completes; then suspend. The task drives itself
    // through the wheel / wait queues and the scheduler destroys it on
    // completion (coro.done in the drain loop).
    b_.SetInsertPoint(async_bb);
    b_.CreateStore(parked_handle(b_, hdl_), record);
    emit_suspend("task.resume");
    b_.CreateBr(cont_bb);

    b_.SetInsertPoint(cont_bb);
    for (auto& [lv, snap] : ref_svc) {
        service_after_write(lv, snap);
    }
    for (llvm::Value* root : ref_containers) {
        b_.CreateCall(gc_container_remove_fn(), { root });
    }
    emit_finish_check(virt ? virtual_call_may_finish() : callee_may_finish(fid));
}

// Build the runtime rt_arg[] descriptor array shared by every $display-family
// format call: stack-allocate one slot per value operand plus one shared word
// buffer holding every integral operand's must / xz / z planes (ceil(w/64)
// words each, zero-extended above the operand width). The descriptor's plane
// fields point into that buffer, so an operand of any width is passed in full.
// Returns the descriptor array pointer (null when there are no operands).
// `who` is unused now that no width is rejected; kept for call-site clarity.
llvm::Value* Backend::build_rt_args(std::span<const Index> args, const char* /*who*/) {
    const auto n = static_cast<uint32_t>(args.size());
    if (n == 0) {
        return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    }

    // Pass 1: size the shared plane buffer (three planes per integral operand,
    // one word per real; strings carry the handle in `val` directly).
    std::vector<uint32_t> words(n, 0);
    uint32_t              total = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const ValType t = type_of(args[k]);
        if (t.is_string) {
            continue;
        }
        if (t.is_real) {
            words[k] = 1;
            total += 1;
            continue;
        }
        words[k] = (t.width + 63) / 64;
        total += 3 * words[k];
    }

    llvm::StructType* sty    = arg_struct_ty();
    llvm::Value*      arr    = b_.CreateAlloca(sty, b_.getInt32(n), "fmt_args");
    auto*             i64    = b_.getInt64Ty();
    llvm::Value*      planes = total != 0 ? b_.CreateAlloca(i64, b_.getInt32(total), "fmt_planes") : nullptr;
    auto*             null   = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));

    // Store an iW plane, zero-extended to a whole number of words, into the
    // shared buffer at word offset `off`; return the element pointer.
    uint32_t off = 0;
    auto     put = [&](llvm::Value* plane, uint32_t nwords) {
        llvm::Value* ep = b_.CreateConstInBoundsGEP1_32(i64, planes, off);
        b_.CreateAlignedStore(b_.CreateZExtOrBitCast(plane, int_ty(nwords * 64)), ep, llvm::Align(8));
        off += nwords;
        return ep;
    };

    for (uint32_t k = 0; k < n; ++k) {
        const Plane   p    = value_of(args[k]);
        const ValType t    = type_of(args[k]);
        llvm::Value*  elt  = b_.CreateConstInBoundsGEP1_32(sty, arr, k);
        if (t.is_string) {
            // A string-object argument (%s): flag bit 1, val = the rt_string
            // handle pointer itself; the width / plane fields are unused.
            store_field(sty, elt, 0, b_.getInt32(0));
            store_field(sty, elt, 1, b_.getInt32(2));
            store_field(sty, elt, 2, str_of(args[k]));
            store_field(sty, elt, 3, null);
            store_field(sty, elt, 4, null);
            continue;
        }
        if (t.is_real) {
            // A real rides one `val` word as a double bit pattern (flags bit
            // 2); a shortreal widens to double first so the runtime handles
            // one representation. Reals are signed and have no x/z.
            llvm::Value* fv = fp_of(p);
            if (t.width == 32) {
                fv = b_.CreateFPExt(fv, b_.getDoubleTy());
            }
            store_field(sty, elt, 0, b_.getInt32(64));
            store_field(sty, elt, 1, b_.getInt32(0b101u));
            store_field(sty, elt, 2, put(b_.CreateBitCast(fv, i64), 1));
            store_field(sty, elt, 3, null);
            store_field(sty, elt, 4, null);
            continue;
        }
        // xz = must ^ may marks every unknown bit (X and Z); z marks the Z
        // subset. The runtime distinguishes them per bit.
        llvm::Value* must = must_of(p);
        llvm::Value* xz   = b_.CreateXor(must, may_of(p));
        store_field(sty, elt, 0, b_.getInt32(t.width));
        store_field(sty, elt, 1, b_.getInt32(t.is_signed ? 1u : 0u));
        store_field(sty, elt, 2, put(must, words[k]));
        store_field(sty, elt, 3, put(xz, words[k]));
        store_field(sty, elt, 4, put(z_of(p), words[k]));
    }
    return arr;
}

} // namespace clotho::ir::backend
