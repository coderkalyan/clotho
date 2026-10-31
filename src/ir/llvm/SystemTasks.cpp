// System tasks: finish, severity, file I/O, formatting, plusargs, readmem, display and coverage.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// -- Functional coverage (LRM 19, docs/design/functional-coverage.md) ------
//
// The frontend evaluated every bin bound, so the whole model is emit-time
// constant data: four nested constant arrays plus their strings, mirroring
// src/runtime/Coverage.h field for field. rt_cover_init cross-checks the four
// struct sizes computed here against the runtime's own, so a layout
// divergence fails loudly instead of reading garbage -- the same discipline
// rt_gc_init applies to its one offset.

// A private constant array of `rows`, or a null pointer when empty: the
// runtime pairs a zero count with a null pointer everywhere, so an empty
// array must not become a zero-length global whose address is compared.
llvm::Constant* Backend::cover_array(llvm::StructType*                   row_ty,
                                     const std::vector<llvm::Constant*>& rows,
                                     const std::string&                  nm) {
    if (rows.empty()) {
        return llvm::Constant::getNullValue(raw_ptr_ty());
    }
    auto* arr_ty = llvm::ArrayType::get(row_ty, rows.size());
    return new llvm::GlobalVariable(env_->llmod,
                                    arr_ty,
                                    /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage,
                                    llvm::ConstantArray::get(arr_ty, rows),
                                    nm);
}

llvm::Constant* Backend::cover_config_global() {
    if (auto* g = env_->llmod.getNamedGlobal("clotho.cover.config")) {
        return g;
    }
    std::vector<llvm::Constant*> types;
    types.reserve(mod_.cover_types.size());
    for (size_t ti = 0; ti < mod_.cover_types.size(); ++ti) {
        const CoverType&             ct = mod_.cover_types[ti];
        const std::string            tn = "clotho.cover." + std::to_string(ti);
        std::vector<llvm::Constant*> points;
        points.reserve(ct.points.size());
        for (size_t pi = 0; pi < ct.points.size(); ++pi) {
            const CoverPoint&            cp = ct.points[pi];
            const std::string            pn = tn + "." + std::to_string(pi);
            std::vector<llvm::Constant*> bins;
            bins.reserve(cp.bins.size());
            for (size_t bi = 0; bi < cp.bins.size(); ++bi) {
                const CoverBin&              cb = cp.bins[bi];
                std::vector<llvm::Constant*> ranges;
                ranges.reserve(cb.ranges.size());
                for (const CoverRange& r : cb.ranges) {
                    ranges.push_back(
                      llvm::ConstantStruct::get(cover_range_ty(), { b_.getInt64(r.lo), b_.getInt64(r.hi) }));
                }
                bins.push_back(llvm::ConstantStruct::get(
                  cover_bin_ty(),
                  { b_.CreateGlobalString(cb.name),
                    b_.getInt32(static_cast<uint32_t>(cb.kind)),
                    b_.getInt32(static_cast<uint32_t>(cb.ranges.size())),
                    cover_array(cover_range_ty(), ranges, pn + "." + std::to_string(bi) + ".ranges") }));
            }
            points.push_back(llvm::ConstantStruct::get(cover_point_ty(),
                                                       { b_.CreateGlobalString(cp.name),
                                                         b_.CreateGlobalString(cp.banner),
                                                         b_.getInt32(static_cast<uint32_t>(cp.bins.size())),
                                                         cover_array(cover_bin_ty(), bins, pn + ".bins"),
                                                         b_.getInt32(cp.weight),
                                                         b_.getInt32(cp.goal),
                                                         b_.getInt32(cp.at_least),
                                                         b_.getInt32(cp.is_auto ? 1u : 0u),
                                                         b_.getInt32(cp.auto_total),
                                                         b_.getInt32(cp.is_signed ? 1u : 0u) }));
        }
        types.push_back(llvm::ConstantStruct::get(cover_type_ty(),
                                                  { b_.CreateGlobalString(ct.name),
                                                    b_.CreateGlobalString(ct.banner),
                                                    b_.getInt32(static_cast<uint32_t>(ct.points.size())),
                                                    cover_array(cover_point_ty(), points, tn + ".points"),
                                                    b_.getInt32(ct.weight),
                                                    b_.getInt32(ct.goal),
                                                    b_.getInt32(ct.per_instance ? 1u : 0u),
                                                    b_.CreateGlobalString(ct.scope) }));
    }
    llvm::Constant* path = config_.coverage_report.empty()
                             ? llvm::Constant::getNullValue(raw_ptr_ty())
                             : llvm::cast<llvm::Constant>(b_.CreateGlobalString(config_.coverage_report));
    auto*           dl   = &env_->llmod.getDataLayout();
    auto*           init =
      llvm::ConstantStruct::get(cover_config_ty(),
                                { cover_array(cover_type_ty(), types, "clotho.cover.types"),
                                  b_.getInt32(static_cast<uint32_t>(types.size())),
                                  b_.getInt32(static_cast<uint32_t>(dl->getTypeAllocSize(cover_type_ty()))),
                                  b_.getInt32(static_cast<uint32_t>(dl->getTypeAllocSize(cover_point_ty()))),
                                  b_.getInt32(static_cast<uint32_t>(dl->getTypeAllocSize(cover_bin_ty()))),
                                  b_.getInt32(static_cast<uint32_t>(dl->getTypeAllocSize(cover_range_ty()))),
                                  path });
    return new llvm::GlobalVariable(env_->llmod, cover_config_ty(), /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init,
                                    "clotho.cover.config");
}

// Lower a $timeformat: evaluate the units / precision / min_width operands and
// hand them, plus the literal suffix, to the runtime setter. The operands were
// lowered earlier in this region, so their values are available.
void Backend::lower_timeformat(Index i) {
    const auto   info  = ir_->extra_data<TimeFormatInfo>(ir_->payload(i).extra);
    llvm::Value* units = cast(must_of(info.units), 32, true);
    llvm::Value* prec  = cast(must_of(info.precision), 32, true);
    llvm::Value* width = cast(must_of(info.min_width), 32, true);

    const std::string suffix  = ir_->extra_bytes(info.suffix, info.suffix_len);
    llvm::Value*      sfx_ptr = b_.CreateGlobalString(suffix);
    b_.CreateCall(timeformat_set_fn(),
                  { units, prec, sfx_ptr, b_.getInt32(static_cast<uint32_t>(suffix.size())), width });
}

// Lower a `strobe_pend`: ramp the target zero-formal deferred-print task
// (build_strobe_body / lower/SystemTasks.cpp) — ramp only, never resumed here — and hand
// its coroutine handle to the runtime's pending queue. The handle is resumed
// once, at the Postponed region of this time step (see the strobe drain loop
// in emit_active_run), which is where the target body actually evaluates its
// format args and prints: LRM 21.2.2 defers the evaluation itself, not just
// the print, so nothing about this call site's own state matters afterward.
// The record is the shared null one (pend_record_slot): the pending process
// may finish, and free its frame, before Postponed.
void Backend::lower_strobe_pend(Index i) {
    const auto      info = ir_->extra_data<StrobePendInfo>(ir_->payload(i).extra);
    llvm::Function* cf   = sym(fn_decls_[static_cast<uint32_t>(info.func)]);

    std::vector<llvm::Value*> argv{ state_, rng_slot_, wheel_ };
    if (design_uses_process_) {
        argv.push_back(proc_arg());
    }
    if (design_uses_home_arg_) {
        argv.push_back(callee_home());
    }
    argv.push_back(pend_record_slot());
    llvm::Value* hdl = b_.CreateCall(cf->getFunctionType(), cf, argv);
    b_.CreateCall(strobe_pend_fn(), { hdl });
}

// Lower a `reactive_pend`: ramp the target zero-formal action-block task and
// push its coroutine handle onto the reactive ring. Structurally
// lower_strobe_pend with the destination region changed -- an Observed-region
// body evaluates a property but may not run the pass/fail code it decides on,
// so LRM 4.4.2.5 hands that code to the Reactive region of this same time
// slot, which the reactive drain in emit_active_run empties before Postponed.
//
// The ramp is emitted at the call site, so every execution builds its own
// frame: an assertion that fails on N clock ticks pends N distinct handles.
// The record is the shared null one; see pend_record_slot for why the
// pending body's frame will not do.
void Backend::lower_reactive_pend(Index i) {
    const auto      info = ir_->extra_data<StrobePendInfo>(ir_->payload(i).extra);
    llvm::Function* cf   = sym(fn_decls_[static_cast<uint32_t>(info.func)]);

    std::vector<llvm::Value*> argv{ state_, rng_slot_, wheel_ };
    if (design_uses_process_) {
        argv.push_back(proc_arg());
    }
    // NOT the pending body's home. LRM 4.4.2.5 hands an action block to the
    // Reactive region of this time slot no matter which region decided the
    // verdict -- the pend below pushes straight onto the reactive ring -- so the
    // thread this ramp starts is reactive-home by construction rather than by
    // inheritance from the Observed process that pended it.
    if (design_uses_home_arg_) {
        argv.push_back(b_.getInt8(kReactiveHomeTag));
    }
    argv.push_back(pend_record_slot());
    llvm::Value* hdl = b_.CreateCall(cf->getFunctionType(), cf, argv);
    emit_reactive_push(b_, hdl);
}

// The waiter queue of one `expect` (LRM 16.17): an array of parked handles and
// a count, created on first use and keyed by monitor index -- which is what
// both halves of an expect name, the wait to push and the wake to drain.
//
// Shaped like the inactive batch rather than like a ring, because a cohort is
// drained whole and never partially: the count goes back to zero in the same
// step that empties it, so there is no wraparound to mask for.
std::pair<llvm::GlobalVariable*, llvm::GlobalVariable*> Backend::expect_queue(uint32_t monitor) {
    if (const auto it = expect_queue_.find(monitor); it != expect_queue_.end()) {
        return it->second;
    }
    // queue_arr_ty_ is shared by every queue, and the drain pushes onto the
    // reactive ring, so both must exist before the first waiter parks.
    ensure_reactive_queue();
    auto*             i32  = llvm::Type::getInt32Ty(ctx_);
    const std::string base = "clotho.expect" + std::to_string(monitor);
    auto*             arr  = new llvm::GlobalVariable(env_->llmod, queue_arr_ty_, false,
                                                      llvm::GlobalValue::InternalLinkage,
                                                      llvm::ConstantAggregateZero::get(queue_arr_ty_), base);
    auto*             top  = new llvm::GlobalVariable(env_->llmod, i32, false,
                                                      llvm::GlobalValue::InternalLinkage,
                                                      llvm::ConstantInt::get(i32, 0), base + "_top");
    const std::pair   entry{ arr, top };
    expect_queue_.emplace(monitor, entry);
    return entry;
}

// Lower an `expect_wait` (LRM 16.17): park this process in the monitor's
// waiter queue and suspend. The request store that asks the monitor for an
// attempt is a separate Store the frontend already emitted above this.
//
// The wake pushes straight onto the reactive ring (lower_expect_wake), so there
// is no region to decide on the way back in -- the handle is already where it
// belongs. That is also why the handle is stored bare rather than through
// parked_handle: a home-region tag would have to be masked off again before the
// push, and an expect's destination is not its home region anyway. It is the
// verdict's region.
void Backend::lower_expect_wait(Index i) {
    if (cur_fn_ != nullptr) {
        // A task's epilogue resumes its caller through the caller's own wake
        // path, so an `expect` in here would resolve in Reactive and then hand
        // the rest of the CALLER's statement back in whatever region that path
        // chooses -- Active, for an ordinary process. The blocking is right and
        // the region is not, which is a divergence with no diagnostic, so it is
        // refused instead.
        // TODO: `expect` inside a task (LRM 16.17 puts no restriction on where
        // the statement appears). Needs the resume region to travel with the
        // REASON for the wake rather than with the parked body, so that a task
        // returning into a blocked caller can carry it up the chain.
        throw BackendError{ "an `expect` inside a task or function is not supported: the process it "
                            "blocks would resume in the Active region" };
    }
    const auto info      = ir_->extra_data<ExpectInfo>(ir_->payload(i).extra);
    const auto [arr, top] = expect_queue(info.monitor);

    auto*        i32  = b_.getInt32Ty();
    llvm::Value* sp   = b_.CreateLoad(i32, sym(top));
    llvm::Value* slot = b_.CreateInBoundsGEP(queue_arr_ty_, sym(arr), { b_.getInt32(0), sp });
    b_.CreateStore(hdl_, slot);
    b_.CreateStore(b_.CreateAdd(sp, b_.getInt32(1)), sym(top));
    // Armed for the same reason a `wait` arms: while parked here this process
    // sits in no other queue, so the flag must say "not scheduled" or a later
    // event wake would be dropped as a duplicate.
    b_.CreateStore(b_.getInt64(0), sched_ptr_);
    emit_suspend("expect.resume");

    // Past the resume point, so this thread is now running in the Reactive
    // region -- and by LRM 24.3.1 it stays there: "all subsequent blocking
    // statements in that thread are scheduled in the Reactive region". So the
    // home region is updated for every LATER park in this body, and deliberately
    // not before the suspend above, which would have moved the park we just took.
    // Without this, the next `@` or `#delay` in the process would park with home
    // = Active and silently migrate the thread back one statement later.
    b_.CreateStore(b_.getInt8(kReactiveHomeTag), home_ptr());
}

// Lower an `expect_wake`: when the operand says this monitor's attempt reached
// a verdict this cycle, move every waiter it holds onto the reactive ring.
//
// The whole cohort, unconditionally, because the whole cohort belongs to the
// attempt that just resolved -- the frontend refuses a process that tries to
// join while an attempt is in flight, so the queue can hold nobody else.
//
// Emitted from the monitor's Observed process, after the same step pended the
// pass/fail action blocks: the ring drains in push order, so an `expect`'s own
// action runs before the statements that follow the `expect`.
void Backend::lower_expect_wake(Index i) {
    const auto&  p        = ir_->payload(i).op_imm;
    const auto [arr, top] = expect_queue(p.imm);
    auto*        i32      = b_.getInt32Ty();
    auto*        ptr_ty   = llvm::PointerType::getUnqual(ctx_);

    auto* drain = llvm::BasicBlock::Create(ctx_, "expect.drain", fn_);
    auto* head  = llvm::BasicBlock::Create(ctx_, "expect.drain.head", fn_);
    auto* body  = llvm::BasicBlock::Create(ctx_, "expect.drain.body", fn_);
    auto* done  = llvm::BasicBlock::Create(ctx_, "expect.drain.done", fn_);

    // must=1 holds only for a definite 1, so an x verdict bit cannot resolve
    // anybody -- the same reading lower_cond gives every condition.
    b_.CreateCondBr(b_.CreateICmpNE(must_of(p.op), zero(1)), drain, done);

    b_.SetInsertPoint(drain);
    llvm::Value* n = b_.CreateLoad(i32, sym(top));
    // Emptied before the pushes, not after: a woken process may execute this
    // very `expect` again once it runs, and that push must land in an empty
    // queue rather than be swallowed by this drain's own store.
    b_.CreateStore(b_.getInt32(0), sym(top));
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    llvm::PHINode* idx = b_.CreatePHI(i32, 2, "waiter");
    idx->addIncoming(b_.getInt32(0), drain);
    b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* slot = b_.CreateInBoundsGEP(queue_arr_ty_, sym(arr), { b_.getInt32(0), idx });
    emit_reactive_push(b_, b_.CreateLoad(ptr_ty, slot));
    llvm::Value* next = b_.CreateAdd(idx, b_.getInt32(1));
    idx->addIncoming(next, b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
}

// The task-call record a pended body ($strobe, an action block) is ramped with.
// It CANNOT be an alloca in the pending body's frame: that frame can be freed
// before the drain resumes the pended body -- an Observed process is ramped,
// resumed and destroyed in one go (emit_observed_run), and an `initial` that
// pends a $strobe and runs off its end in the same time step is destroyed
// before Postponed -- and a task's epilogue loads the parent handle back out of
// the record to resume whoever called it (lower_task), so reading it there
// pushes a garbage handle onto the active queue.
//
// A pended body has no caller to resume: the drain runs it, nothing blocks on
// it, and no code ever writes a parent here. So one slot that stays null
// serves every pend, however many are live at once.
llvm::Value* Backend::pend_record_slot() {
    if (pend_record_gv_ == nullptr) {
        auto* ptr_ty    = llvm::PointerType::getUnqual(ctx_);
        pend_record_gv_ = new llvm::GlobalVariable(env_->llmod,
                                                   ptr_ty,
                                                   false,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantPointerNull::get(ptr_ty),
                                                   "clotho.pend_record");
    }
    return sym(pend_record_gv_);
}

// Leave the body being lowered the way a $finish must: through its own
// epilogue, so its frame is released and its resumer sees an ordinary
// completion. One shape per kind of body.
void Backend::emit_finish_exit() {
    if (cur_fn_ == nullptr) {
        if (hdl_ != nullptr) {
            // Process coroutine: the final suspend, exactly as running off the
            // end of the body does, so the resumer destroys the frame once.
            b_.CreateBr(leave_blocks(0, coro_final_bb_));
        } else {
            // Thunk-elided process: a plain void body.
            deinit_body_strings();
            b_.CreateRetVoid();
        }
        return;
    }
    if (cur_is_task_) {
        // Task: the same epilogue an explicit `return` takes.
        b_.CreateBr(leave_blocks(0, task_epilogue_bb_));
        return;
    }
    // Function: no shared epilogue, so return here. The result is dead — the
    // caller tests rt_finishing and leaves without reading it — so a poison
    // value is the honest thing to hand back.
    deinit_body_strings();
    if (fn_->getReturnType()->isVoidTy()) {
        b_.CreateRetVoid();
    } else {
        b_.CreateRet(llvm::PoisonValue::get(fn_->getReturnType()));
    }
}

// After a call that could have reached a $finish, leave if it did. Emitted
// only for callees the may-finish closure marks, which is no call at all in
// most designs (docs/design/cooperative-finish.md).
void Backend::emit_finish_check(bool callee_may_finish) {
    if (!callee_may_finish) {
        return;
    }
    auto* leave = llvm::BasicBlock::Create(ctx_, "finish.leave", fn_);
    auto* go    = llvm::BasicBlock::Create(ctx_, "finish.cont", fn_);
    b_.CreateCondBr(
      b_.CreateICmpNE(b_.CreateLoad(b_.getInt8Ty(), finishing_gv()), b_.getInt8(0)), leave, go);
    b_.SetInsertPoint(leave);
    emit_finish_exit();
    b_.SetInsertPoint(go);
}

// Does a call to `id` need the post-call test?
bool Backend::callee_may_finish(FunctionId id) const {
    const auto k = static_cast<uint32_t>(id);
    return k < may_finish_.size() && may_finish_[k] != 0;
}

// A virtual call resolves at run time, so the statically named target is not
// the whole answer — an override could finish where its prototype does not.
// Treat any virtual call as may-finish whenever the design has a may-finish
// subroutine at all: sound, and the sets are small enough that the
// over-approximation costs nothing measurable.
bool Backend::virtual_call_may_finish() const {
    return std::any_of(may_finish_.begin(), may_finish_.end(), [](uint8_t v) { return v != 0; });
}

// Lower a $finish / $stop: pass the runtime the compile-time banner prefix,
// the current simulation time, and the diagnostic level, then it prints the
// banner and exits. The call never returns, so the rest of the region is dead
// but stays valid IR — rt_finish is noreturn and the optimizer prunes the
// unreachable tail.
//
// The time is exact in every body, including a plain function: the wheel is a
// module global (clotho.wheel) that every body binds, not a body parameter.
void Backend::lower_finish(Index i) {
    const auto        info   = ir_->extra_data<FinishInfo>(ir_->payload(i).extra);
    const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
    llvm::Value*      now    = b_.CreateCall(wheel_now_fn(), { wheel_ });
    llvm::Value* args[] = { b_.CreateGlobalString(banner), now, b_.getInt32(info.arg) };

    // The runtime reports and returns; control leaves through this body's own
    // epilogue, so every frame between here and the drain loop is released by
    // its owner rather than jumped over.
    b_.CreateCall(finish_coop_fn(), args);
    // End the current settle wave from here: emptying the active ring means
    // the drain's existing head != tail test falls through to its finished
    // edge, so no queued process runs after the $finish and the hot path
    // needs no test of its own.
    if (active_head_gv_ != nullptr) {
        b_.CreateStore(b_.CreateLoad(b_.getInt32Ty(), sym(active_tail_gv_)),
                       sym(active_head_gv_));
    }
    // Same for the reactive rings and the batch: a $fatal in an action block
    // would otherwise leave the rest of this instant's pass/fail code queued,
    // and the reactive drain tests the finish flag only on its empty edge.
    if (reactive_head_gv_ != nullptr) {
        b_.CreateStore(b_.CreateLoad(b_.getInt32Ty(), sym(reactive_tail_gv_)),
                       sym(reactive_head_gv_));
    }
    if (reactive_cont_head_gv_ != nullptr) {
        b_.CreateStore(b_.CreateLoad(b_.getInt32Ty(), sym(reactive_cont_tail_gv_)), sym(reactive_cont_head_gv_));
    }
    if (re_inactive_top_gv_ != nullptr) {
        b_.CreateStore(b_.getInt32(0), sym(re_inactive_top_gv_));
    }
    emit_finish_exit();
    // The rest of the region is dead (LRM 20.2 ends the simulation here) but
    // still gets lowered, so park it in a block with no predecessors — the
    // same pruning the noreturn attribute used to buy.
    auto* dead = llvm::BasicBlock::Create(ctx_, "finish.dead", fn_);
    b_.SetInsertPoint(dead);
}

// Lower a unique/priority case violation report: pass the compile-time banner
// prefix, the simulation time of the violation, and the message kind. The
// runtime queues it and reports in the Observed region, where LRM 12.4.2 /
// 12.5.3 place it (docs/LRM_NOTES.md, note 18), rather than emitting here in
// the Active region, so the region continues normally.
void Backend::lower_case_viol(Index i) {
    const auto        info   = ir_->extra_data<CaseViolInfo>(ir_->payload(i).extra);
    const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
    llvm::Value*      now    = b_.CreateCall(wheel_now_fn(), { wheel_ });
    b_.CreateCall(observed_case_violation_fn(),
                  { b_.CreateGlobalString(banner), now, b_.getInt32(info.kind) });
}

// Lower a severity report ($info / $warning / $error / $fatal, or an
// immediate-assertion default failure): build the message descriptor array
// and call the runtime, passing the severity label, compile-time banner, and
// format string. Needs no wheel (the report carries no simulation time), so
// it lowers cleanly inside a pure function too.
void Backend::lower_severity(Index i) {
    const auto  info = ir_->extra_data<SeverityInfo>(ir_->payload(i).extra);
    const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
    const std::string fmt    = ir_->extra_bytes(info.fmt, info.fmt_len);

    const std::span<const Index> args = ir_->region(info.args);
    llvm::Value*                 arr  = build_rt_args(args, ir_->words(info.signs));
    if (info.defer == 0) {
        b_.CreateCall(severity_fn(), { b_.getInt32(static_cast<int32_t>(info.level)),
                                       b_.CreateGlobalString(banner), b_.CreateGlobalString(fmt),
                                       b_.getInt32(static_cast<uint32_t>(args.size())), arr });
        return;
    }
    // Deferred immediate assertion action (LRM 16.4): queue the rendered
    // report tagged with the process's current generation. The generation
    // pointer is this group's counter (defer_gen_ptr) — always set when a
    // deferred Severity lowers, since is_simple_always forces the group to a
    // coroutine and lower_group set cur_defer_group_. defer == 2 selects the
    // Postponed (final) maturation, defer == 1 the Observed (`#0`).
    llvm::Value* gen = defer_gen_ptr();
    auto*        i64 = b_.getInt64Ty();
    llvm::Value* gen_val =
      gen != nullptr ? b_.CreateLoad(i64, gen)
                     : static_cast<llvm::Value*>(b_.getInt64(0));
    llvm::Value* gen_arg =
      gen != nullptr ? gen
                     : static_cast<llvm::Value*>(
                         llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
    b_.CreateCall(defer_report_fn(),
                  { b_.getInt32(static_cast<int32_t>(info.level)),
                    b_.getInt64(info.defer == 2 ? 1 : 0), gen_arg, gen_val,
                    b_.CreateGlobalString(banner), b_.CreateGlobalString(fmt),
                    b_.getInt32(static_cast<uint32_t>(args.size())), arr });
}

// Lower a $fflush: flush the descriptor operand's buffers (a constant 0 for the
// no-argument form, which the runtime reads as "flush everything").
void Backend::lower_fflush(Index i) {
    llvm::Value* desc = cast(must_of(ir_->payload(i).unary), 32, true);
    b_.CreateCall(fflush_fn(), { desc });
}

// Lower a two-argument $fopen: materialize the filename and mode (each a packed
// vector or a dynamic string) and call the FD opener; the result is the
// descriptor.
void Backend::lower_fopen_fd(Index i) {
    const auto&    p    = ir_->payload(i).binary;
    const AsciiArg n    = spill_ascii_arg(p.l, "fopen.name");
    const AsciiArg m    = spill_ascii_arg(p.r, "fopen.mode");
    llvm::Value*   desc = b_.CreateCall(fopen_fd_fn(), { n.ptr, n.len, n.fwd, m.ptr, m.len, m.fwd });
    // The descriptor is a plain known integer (no x/z): must == may, z == 0.
    set_value(i, two_val(desc));
}

// Lower a one-argument $fopen: materialize the filename and open a fresh MCD.
void Backend::lower_fopen_mcd(Index i) {
    const AsciiArg n    = spill_ascii_arg(ir_->payload(i).unary, "fopen.name");
    llvm::Value*   desc = b_.CreateCall(fopen_mcd_fn(), { n.ptr, n.len, n.fwd });
    set_value(i, two_val(desc));
}

// Lower a $system: materialize the command string (packed-ASCII vector or
// dynamic string) and call rt_system. imm == 0 is the no-arg $system(),
// which passes has_arg = 0 so the runtime execs system(NULL); the command
// operand is then an (unused) empty-string constant. The compile-time gate
// (allow_system) is enforced in the frontend before this op is ever emitted;
// rt_system re-checks the access tier as a runtime backstop.
void Backend::lower_system(Index i) {
    const auto&    p       = ir_->payload(i).op_imm;
    const AsciiArg c       = spill_ascii_arg(p.op, "system.cmd");
    llvm::Value*   has_arg = b_.getInt32(p.imm != 0 ? 1 : 0);
    llvm::Value*   rc      = b_.CreateCall(system_fn(), { c.ptr, c.len, c.fwd, has_arg });
    // The return is a plain known integer (no x/z): must == may, z == 0.
    set_value(i, two_val(rc));
}

// -- Functional coverage (LRM 19) ------------------------------------------
//
// Every op is one call into the coverage runtime; the model it indexes is the
// constant data cover_config_global emitted. Nothing here touches the
// collector: a covergroup instance is runtime-owned storage that outlives the
// design (LRM 19.11.3 aggregates dropped instances), so its handle is an
// opaque 2-state 64-bit word and never a traced root.
void Backend::lower_cover_query(Index i) {
    const auto info = ir_->extra_data<CoverInfo>(ir_->payload(i).extra);
    const auto args = ir_->region(info.args);
    auto*      i32  = b_.getInt32Ty();
    auto*      dbl  = b_.getDoubleTy();
    // Every instance-addressed op takes the handle as operand 0; the
    // type-addressed ones carry the index on the op instead.
    const auto handle = [&]() -> llvm::Value* {
        if (args.empty()) {
            throw BackendError{ "internal: cover_query with no handle operand" };
        }
        return cast(must_of(args[0]), 64, false);
    };
    const auto ival = [&](llvm::Value* v) { set_value(i, two_val(v)); };
    const auto hval = [&](llvm::Value* v) { set_value(i, two_val(v)); };
    switch (info.op) {
        case CoverOp::New:
            // `ctx` is the enclosing object for a class-embedded covergroup, so
            // the generated sample body can reach the class properties its
            // coverpoints name, and null otherwise.
            hval(b_.CreateCall(cover_new_fn(),
                               { b_.getInt32(info.type_idx),
                                 args.empty() ? b_.getInt64(0) : cast(must_of(args[0]), 64, false) }));
            return;
        case CoverOp::Ctx:
            hval(b_.CreateCall(cover_ctx_fn(), { handle() }));
            return;
        case CoverOp::First:
            hval(b_.CreateCall(cover_first_fn(), { b_.getInt32(info.type_idx) }));
            return;
        case CoverOp::Next:
            hval(b_.CreateCall(cover_next_fn(), { handle() }));
            return;
        case CoverOp::InstCoverage:
            set_value(i,
                      real_plane(b_.CreateCall(cover_inst_query_fn("rt_cover_inst_coverage", dbl), { handle() })));
            return;
        case CoverOp::InstCovered:
            ival(b_.CreateCall(cover_inst_query_fn("rt_cover_inst_covered", i32), { handle() }));
            return;
        case CoverOp::InstTotal:
            ival(b_.CreateCall(cover_inst_query_fn("rt_cover_inst_total", i32), { handle() }));
            return;
        case CoverOp::PointCoverage:
            set_value(i,
                      real_plane(b_.CreateCall(cover_point_query_fn("rt_cover_point_coverage", dbl),
                                               { handle(), b_.getInt32(info.point) })));
            return;
        case CoverOp::PointCovered:
            ival(b_.CreateCall(cover_point_query_fn("rt_cover_point_covered", i32),
                               { handle(), b_.getInt32(info.point) }));
            return;
        case CoverOp::PointTotal:
            ival(b_.CreateCall(cover_point_query_fn("rt_cover_point_total", i32),
                               { handle(), b_.getInt32(info.point) }));
            return;
        case CoverOp::TypeCoverage:
            set_value(i,
                      real_plane(b_.CreateCall(cover_type_query_fn("rt_cover_type_coverage", dbl),
                                               { b_.getInt32(info.type_idx) })));
            return;
        case CoverOp::TypeCovered:
            ival(b_.CreateCall(cover_type_query_fn("rt_cover_type_covered", i32), { b_.getInt32(info.type_idx) }));
            return;
        case CoverOp::TypeTotal:
            ival(b_.CreateCall(cover_type_query_fn("rt_cover_type_total", i32), { b_.getInt32(info.type_idx) }));
            return;
        case CoverOp::DesignCoverage:
            set_value(i, real_plane(b_.CreateCall(cover_design_coverage_fn(), {})));
            return;
        default:
            throw BackendError{ "internal: non-query op on a cover_query" };
    }
}

void Backend::lower_cover_ctl(Index i) {
    const auto info = ir_->extra_data<CoverInfo>(ir_->payload(i).extra);
    const auto args = ir_->region(info.args);
    if (args.empty()) {
        throw BackendError{ "internal: cover_ctl with no handle operand" };
    }
    llvm::Value* h = cast(must_of(args[0]), 64, false);
    switch (info.op) {
        case CoverOp::Hit: {
            if (args.size() != 3) {
                throw BackendError{ "internal: cover_ctl hit takes a value and an x mask" };
            }
            // The unknown-bit mask is what makes 19.5.3's "automatic bins only
            // consider 2-state values" a runtime test rather than a frontend
            // refusal: a sampled x/z matches no bin at all.
            b_.CreateCall(cover_hit_fn(),
                          { h, b_.getInt32(info.point), cast(must_of(args[1]), 64, false),
                            cast(must_of(args[2]), 64, false) });
            return;
        }
        case CoverOp::Start:
            if (args.size() != 2) {
                throw BackendError{ "internal: cover_ctl start takes an enable" };
            }
            b_.CreateCall(cover_start_fn(),
                          { h, cast(must_of(args[1]), 32, false) });
            return;
        case CoverOp::SetName: {
            if (args.size() != 2) {
                throw BackendError{ "internal: cover_ctl set_inst_name takes a string" };
            }
            const AsciiArg s = spill_ascii_arg(args[1], "cover.name");
            b_.CreateCall(cover_set_name_fn(),
                          { h, s.ptr, b_.CreateZExt(s.len, b_.getInt64Ty()) });
            return;
        }
        default:
            throw BackendError{ "internal: non-statement op on a cover_ctl" };
    }
}

// Lower a $fclose: close the descriptor operand.
void Backend::lower_fclose(Index i) {
    llvm::Value* desc = cast(must_of(ir_->payload(i).unary), 32, true);
    b_.CreateCall(fclose_fn(), { desc });
}

// The file read / positioning system functions take i32 operands and
// return i32; the results are always-known integers.
// $fgetc / $feof / $ftell / $rewind: one descriptor operand.
void Backend::lower_file_unary(Index i, const char* fn) {
    llvm::Value* fd   = cast(must_of(ir_->payload(i).unary), 32, true);
    auto*        i32  = b_.getInt32Ty();
    auto*        fnty = llvm::FunctionType::get(i32, { i32 }, false);
    llvm::Value* r    = b_.CreateCall(env_->llmod.getOrInsertFunction(fn, fnty), { fd });
    set_value(i, two_val(r));
}

// $ungetc(c, fd).
void Backend::lower_ungetc(Index i) {
    const auto&  p    = ir_->payload(i).binary;
    llvm::Value* c    = cast(must_of(p.l), 32, true);
    llvm::Value* fd   = cast(must_of(p.r), 32, true);
    auto*        i32  = b_.getInt32Ty();
    auto*        fnty = llvm::FunctionType::get(i32, { i32, i32 }, false);
    llvm::Value* r = b_.CreateCall(env_->llmod.getOrInsertFunction("rt_ungetc", fnty), { c, fd });
    set_value(i, two_val(r));
}

// $fseek(fd, offset, operation).
void Backend::lower_fseek(Index i) {
    const auto&  p    = ir_->payload(i).op_extra;
    const auto   info = ir_->extra_data<FseekInfo>(p.extra);
    llvm::Value* fd   = cast(must_of(p.op), 32, true);
    llvm::Value* off  = cast(must_of(info.offset), 32, true);
    llvm::Value* op   = cast(must_of(info.operation), 32, true);
    auto*        i32  = b_.getInt32Ty();
    auto*        fnty = llvm::FunctionType::get(i32, { i32, i32, i32 }, false);
    llvm::Value* r = b_.CreateCall(env_->llmod.getOrInsertFunction("rt_fseek", fnty), { fd, off, op });
    set_value(i, two_val(r));
}

// Lower a $fgets: a string-object destination goes straight to the runtime's
// rt_string handle (no fixed width, no wait-queue plane snapshot -- mirrors
// lower_sformat's string branch). A packed-vector destination hands the
// runtime the plane pointers plus bit width and the descriptor; the runtime
// reads a line, stores it right-justified into the must|may plane, clears the
// z plane, and returns the character count.
void Backend::lower_fgets(Index i) {
    const Index  dst = ir_->payload(i).binary.l;
    llvm::Value* fd  = cast(must_of(ir_->payload(i).binary.r), 32, true);
    const LValue& lv = lvalue_of(dst);
    if (lv.is_string) {
        llvm::Value* snap = str_chg_snapshot(lv);
        llvm::Value* r    = b_.CreateCall(
          fgets_str_fn(), { lv_string_ptr(lv, StringUse::Write), fd, alloc_site_global(loc_text(i), "string") });
        str_chg_after(lv, snap);
        set_value(i, two_val(r));
        return;
    }
    const auto [mm, z, width, flags] = string_dest(dst, "$fgets");
    const ServiceSnapshot old          = service_snapshot(lv);
    llvm::Value* r = b_.CreateCall(fgets_fn(), { mm, z, b_.getInt32(width), fd, b_.getInt32(flags) });
    service_after_write(lv, old);
    set_value(i, two_val(r));
}

// Resolve a whole packed-vector lvalue to (must|may plane ptr, z plane ptr, bit
// width, runtime flags) for a string write-back ($fgets / $sformat / $ferror).
// Flags bit0 marks a 2-state slot, so the runtime writes only the iW
// value word. Part-select and non-byte-multiple targets loud-fail: the runtime
// fills a whole byte-aligned vector. `who` names the caller.
std::tuple<llvm::Value*, llvm::Value*, uint32_t, uint32_t> Backend::string_dest(Index dst, const char* who) {
    const LValue& lv = lvalue_of(dst);
    if (lv.is_string) {
        // Every caller (fgets/ferror/sformat) checks lv.is_string and branches to
        // its own rt_string-handle path before reaching here; this is defense in
        // depth against a future caller doing otherwise.
        throw BackendError{ std::string(who) + " into a string object is not supported" };
    }
    if (lv.width != lv.store_width) {
        // TODO: a part-select target (s[15:0]) writes only a slice.
        throw BackendError{ std::string(who) + " into a part-select target is not supported" };
    }
    if (lv.store_width % 8 != 0) {
        // TODO: a non-byte-multiple width leaves the must|may plane halves
        // byte-unaligned; targets are byte vectors in practice.
        throw BackendError{ std::string(who) +
                            " into a target whose width is not a multiple of 8 is not supported" };
    }
    // A never-z target (z elision) has no z word; pass null and the runtime
    // skips the z write.
    llvm::Value* mm = lv_mm_ptr(lv);
    llvm::Value* z =
      lv.has_z ? lv_z_ptr(lv) : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    return { mm, z, lv.store_width, !lv.four_state ? 1u : 0u };
}

Backend::FormatArg Backend::format_arg(Index i, Index fmt_value, ExtraIndex fmt, uint32_t fmt_len, TickScale scale) {
    if (fmt_value == kNoFormatValue) {
        return { b_.CreateGlobalString(ir_->extra_bytes(fmt, fmt_len)),
                 llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)) };
    }
    llvm::StructType* sty = fmt_dyn_struct_ty();
    llvm::Value*      ctx = b_.CreateAlloca(sty, nullptr, "fmt_dyn");
    b_.CreateStore(b_.getInt64(scale.value()), b_.CreateStructGEP(sty, ctx, 0));
    b_.CreateStore(alloc_site_global(loc_text(i), "format string"), b_.CreateStructGEP(sty, ctx, 1));
    return { b_.CreateCall(str_data_fn(), { str_of(fmt_value) }), ctx };
}

// Lower a $sformat / $swrite: build the format-arg descriptor array and hand
// it, the format string, and the destination — a string object's handle, or
// a packed vector's planes — to the runtime.
void Backend::lower_sformat(Index i) {
    const auto info = ir_->extra_data<SformatInfo>(ir_->payload(i).extra);

    const std::span<const Index> args = ir_->region(info.args);
    llvm::Value*                 arr  = build_rt_args(args, ir_->words(info.signs));
    llvm::Value*                 argc = b_.getInt32(static_cast<uint32_t>(args.size()));
    const FormatArg f = format_arg(i, info.fmt_value, info.fmt, info.fmt_len, info.tick_scale);
    const LValue&   lv = lvalue_of(info.dst);
    if (lv.is_string) {
        llvm::Value* snap = str_chg_snapshot(lv);
        b_.CreateCall(sformat_str_fn(), { lv_string_ptr(lv, StringUse::Write), f.fmt, argc, arr,
                                          alloc_site_global(loc_text(i), "string"), f.dyn });
        str_chg_after(lv, snap);
        return;
    }
    const auto [mm, z, width, flags] = string_dest(info.dst, "$sformat / $swrite");
    const ServiceSnapshot old          = service_snapshot(lvalue_of(info.dst));
    b_.CreateCall(sformat_fn(),
                  { mm, z, b_.getInt32(width), f.fmt, argc, arr, b_.getInt32(flags), f.dyn });
    service_after_write(lvalue_of(info.dst), old);
}

// Lower a $sformatf: format into the instruction's string temp.
void Backend::lower_sformatf(Index i) {
    const auto info = ir_->extra_data<SformatfInfo>(ir_->payload(i).extra);

    const std::span<const Index> args = ir_->region(info.args);
    llvm::Value*                 arr  = build_rt_args(args, ir_->words(info.signs));
    const FormatArg f = format_arg(i, info.fmt_value, info.fmt, info.fmt_len, info.tick_scale);
    b_.CreateCall(sformat_str_fn(),
                  { str_of(i), f.fmt, b_.getInt32(static_cast<uint32_t>(args.size())), arr,
                    alloc_site_global(loc_text(i), "string"), f.dyn });
}

// Lower a $ferror: a string-object destination goes straight to the runtime's
// rt_string handle (mirrors lower_fgets's string branch); a packed-vector
// destination passes the descriptor plus the destination's planes. Either way
// the runtime writes the message and returns the error code.
void Backend::lower_ferror(Index i) {
    const Index   dst = ir_->payload(i).binary.l;
    llvm::Value*  fd  = cast(must_of(ir_->payload(i).binary.r), 32, true);
    const LValue& lv  = lvalue_of(dst);
    if (lv.is_string) {
        llvm::Value* snap = str_chg_snapshot(lv);
        llvm::Value* r    = b_.CreateCall(
          ferror_str_fn(), { fd, lv_string_ptr(lv, StringUse::Write), alloc_site_global(loc_text(i), "string") });
        str_chg_after(lv, snap);
        set_value(i, two_val(r));
        return;
    }
    const auto [mm, z, width, flags] = string_dest(dst, "$ferror");
    const ServiceSnapshot old          = service_snapshot(lv);
    llvm::Value* r = b_.CreateCall(ferror_fn(), { fd, mm, z, b_.getInt32(width), b_.getInt32(flags) });
    service_after_write(lv, old);
    set_value(i, two_val(r));
}

// Lower a $fscanf / $sscanf: build the rt_scan_arg[] descriptor array from the
// output lvalues, then call the runtime scanner with the source (an FD, or a
// spilled string for $sscanf). A packed target must be whole-storage; a
// `string` object target rides the descriptor's flags-bit1 variant.
void Backend::lower_scanf(Index i) {
    const auto        info = ir_->extra_data<ScanfInfo>(ir_->payload(i).extra);
    const std::string fmt  = ir_->extra_bytes(info.fmt, info.fmt_len);

    const std::span<const Index> outs = ir_->region(info.args);
    const auto                   n    = static_cast<uint32_t>(outs.size());

    llvm::StructType*         sty = scan_arg_struct_ty();
    llvm::Value*              arr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    // Old-value snapshots for wait-queue servicing, parallel to `outs`
    // (null where a target needs no servicing).
    std::vector<ServiceSnapshot> olds(n);
    if (n > 0) {
        arr = b_.CreateAlloca(sty, b_.getInt32(n), "scan_args");
        for (uint32_t k = 0; k < n; ++k) {
            const LValue& lv  = lvalue_of(outs[k]);
            llvm::Value*  elt = b_.CreateConstInBoundsGEP1_32(sty, arr, k);
            llvm::Value*  nul = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
            if (lv.is_string) {
                // A `string` object destination (flags bit1): the descriptor
                // carries the rt_string handle and its alloc site instead of
                // plane pointers, and the engine assigns the %s token verbatim.
                // Only %s pairs with a string (the frontend refuses the other
                // conversions per LRM 21.3.4.3), and a `%5s` field width caps
                // the token like any other %s. The snapshot must open before
                // the Write-use handle is taken (str_chg bracket).
                olds[k] = service_snapshot(lv);
                b_.CreateStore(nul, b_.CreateStructGEP(sty, elt, 0));
                b_.CreateStore(nul, b_.CreateStructGEP(sty, elt, 1));
                b_.CreateStore(b_.getInt32(0), b_.CreateStructGEP(sty, elt, 2));
                b_.CreateStore(b_.getInt32(2u), b_.CreateStructGEP(sty, elt, 3));
                b_.CreateStore(lv_string_ptr(lv, StringUse::Write), b_.CreateStructGEP(sty, elt, 4));
                b_.CreateStore(alloc_site_global(loc_text(i), "string"), b_.CreateStructGEP(sty, elt, 5));
                continue;
            }
            if (lv.width != lv.store_width) {
                // TODO: a part-select scan target (x[7:0]).
                throw BackendError{ "$fscanf / $sscanf into a part-select target is not supported" };
            }
            // A never-z target (z elision) passes a null z pointer. Flags bit0
            // marks a 2-state slot: the runtime writes only the iW value
            // word (parsed x/z digits drop to 0 per LRM 6.3.1).
            llvm::Value* mm = lv_mm_ptr(lv);
            llvm::Value* z  = lv.has_z ? lv_z_ptr(lv) : nul;
            b_.CreateStore(mm, b_.CreateStructGEP(sty, elt, 0));
            b_.CreateStore(z, b_.CreateStructGEP(sty, elt, 1));
            b_.CreateStore(b_.getInt32(lv.store_width), b_.CreateStructGEP(sty, elt, 2));
            b_.CreateStore(b_.getInt32(!lv.four_state ? 1u : 0u), b_.CreateStructGEP(sty, elt, 3));
            b_.CreateStore(nul, b_.CreateStructGEP(sty, elt, 4));
            b_.CreateStore(nul, b_.CreateStructGEP(sty, elt, 5));
            olds[k] = service_snapshot(lv);
        }
    }

    llvm::Value* fmt_ptr = b_.CreateGlobalString(fmt);
    llvm::Value* r       = nullptr;
    if (ir_->tag(i) == Inst::Tag::Sscanf) {
        const AsciiArg s = spill_ascii_arg(info.source, "sscanf.buf");
        r = b_.CreateCall(sscanf_fn(), { s.ptr, s.len, s.fwd, fmt_ptr, b_.getInt32(n), arr });
    } else {
        llvm::Value* fd = cast(must_of(info.source), 32, true);
        r               = b_.CreateCall(fscanf_fn(), { fd, fmt_ptr, b_.getInt32(n), arr });
    }
    // Every conversion has landed; wake anything a written target changed
    // (an unconverted target reloads its old value and stays quiet).
    for (uint32_t k = 0; k < n; ++k) {
        service_after_write(lvalue_of(outs[k]), olds[k]);
    }
    set_value(i, two_val(r));
}

// Lower $test$plusargs(str): the query string is a compile-time literal
// (frontend-checked), so its bytes go straight to the runtime scan.
void Backend::lower_test_plusargs(Index i) {
    const auto        info = ir_->extra_data<TestPlusArgsInfo>(ir_->payload(i).extra);
    const std::string str  = ir_->extra_bytes(info.str, info.str_len);
    llvm::Value*       str_ptr = b_.CreateGlobalString(str);
    llvm::Value*       r       = b_.CreateCall(
      test_plusargs_fn(), { str_ptr, b_.getInt32(static_cast<uint32_t>(str.size())) });
    set_value(i, two_val(r));
}

// Lower $value$plusargs(user_string, var) (LRM §21.6).
//
// A `string`-typed destination goes straight to the runtime's rt_string handle
// (as lower_fgets / lower_ferror do), which is the form UVM's own
// `$value$plusargs("UVM_TESTNAME=%s", test_name)` takes. No plane snapshot: a
// string is guarded out of sensitivity lists by the frontend, so there is no
// wait queue to service (see the StrAssign store path).
//
// A packed-vector destination builds a single rt_scan_arg descriptor for the
// output lvalue (as lower_scanf does per conversion) and hands the runtime the
// compile-time-literal prefix plus the format char. That target must be
// whole-storage, like a scanf target.
void Backend::lower_value_plusargs(Index i) {
    const Index dst  = ir_->payload(i).op_extra.op;
    const auto  info = ir_->extra_data<ValuePlusArgsInfo>(ir_->payload(i).op_extra.extra);
    const std::string prefix = ir_->extra_bytes(info.prefix, info.prefix_len);
    llvm::Value*      prefix_ptr = b_.CreateGlobalString(prefix);
    llvm::Value*      prefix_len = b_.getInt32(static_cast<uint32_t>(prefix.size()));

    const LValue& lv = lvalue_of(dst);
    if (lv.is_string) {
        if (info.conv != 's') {
            // TODO: a numeric conversion into a `string` object. LRM §21.6 stores
            // the CONVERTED value, so this is LRM 6.16's integral-to-string
            // assignment of it -- a one-byte string holding a character code, which
            // is a separate question from the "%s -- string (no conversion)" form
            // the clause's examples and UVM actually use.
            throw BackendError{ std::string("$value$plusargs %") +
                                static_cast<char>(info.conv) +
                                " into a string object is not supported" };
        }
        llvm::Value* snap = str_chg_snapshot(lv);
        llvm::Value* r    = b_.CreateCall(
          value_plusargs_str_fn(),
          { prefix_ptr, prefix_len, lv_string_ptr(lv, StringUse::Write),
            alloc_site_global(loc_text(i), "string") });
        str_chg_after(lv, snap);
        set_value(i, two_val(r));
        return;
    }
    if (lv.width != lv.store_width) {
        // TODO: a part-select $value$plusargs target (x[7:0]).
        throw BackendError{ "$value$plusargs into a part-select target is not supported" };
    }

    llvm::StructType* sty = scan_arg_struct_ty();
    llvm::Value*      arg = b_.CreateAlloca(sty, nullptr, "plusarg_val");
    llvm::Value*       mm = lv_mm_ptr(lv);
    llvm::Value*       z  = lv.has_z ? lv_z_ptr(lv)
                                     : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    b_.CreateStore(mm, b_.CreateStructGEP(sty, arg, 0));
    b_.CreateStore(z, b_.CreateStructGEP(sty, arg, 1));
    b_.CreateStore(b_.getInt32(lv.store_width), b_.CreateStructGEP(sty, arg, 2));
    b_.CreateStore(b_.getInt32(!lv.four_state ? 1u : 0u), b_.CreateStructGEP(sty, arg, 3));

    const ServiceSnapshot old = service_snapshot(lv);
    llvm::Value*           r   = b_.CreateCall(
      value_plusargs_fn(), { prefix_ptr, prefix_len, b_.getInt32(info.conv), arg });
    service_after_write(lv, old);
    set_value(i, two_val(r));
}

// Spill a packed-ASCII string value's must-bits to a stack byte buffer and
// return (pointer, byte length). The value is a packed vector (a string literal
// or a reg holding characters); the runtime reconstructs the string from these
// bytes. Shared by $readmem (filename) and $fopen (name / mode).
std::pair<llvm::Value*, uint32_t> Backend::spill_ascii(Index v, const char* name) {
    const uint32_t bytes = std::max<uint32_t>((type_of(v).width + 7) / 8, 1);
    llvm::Value*   buf   = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(bytes), name);
    b_.CreateAlignedStore(must_of(v), buf, llvm::Align(1));
    return { buf, bytes };
}

Backend::AsciiArg Backend::spill_ascii_arg(Index v, const char* name) {
    if (type_of(v).is_string) {
        llvm::Value* h = str_of(v);
        return { b_.CreateCall(str_data_fn(), { h }),
                 b_.CreateTrunc(b_.CreateCall(str_len_fn(), { h }), b_.getInt32Ty()),
                 b_.getInt32(1) };
    }
    const auto [ptr, len] = spill_ascii(v, name);
    return { ptr, b_.getInt32(len), b_.getInt32(0) };
}

// Lower a $fread into a single vector: one element, no addressing. The runtime
// reads ceil(width/8) bytes MSB-first into the destination planes.
void Backend::lower_fread_word(Index i) {
    const Index dst = ir_->payload(i).binary.l;

    const LValue& lv = lvalue_of(dst);
    if (lv.is_string) {
        throw BackendError{ "$fread into a string object is not supported" };
    }
    if (lv.width != lv.store_width) {
        throw BackendError{ "$fread into a part-select target is not supported" };
    }
    // A never-z target (z elision) passes a null z pointer; a 2-state
    // slot sets flags bit2 so the runtime writes only the iW value word.
    llvm::Value* mm = lv_mm_ptr(lv);
    llvm::Value* z =
      lv.has_z ? lv_z_ptr(lv) : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    llvm::Value* fd = cast(must_of(ir_->payload(i).binary.r), 32, true);

    const uint32_t four   = type_of(dst).four_state ? 1u : 0u;
    const uint32_t flags  = !lv.four_state ? 4u : 0u;
    const ServiceSnapshot old = service_snapshot(lv);
    llvm::Value*   r      = b_.CreateCall(fread_fn(),
                                          { fd, mm, z, b_.getInt32(0), b_.getInt32(1), b_.getInt32(lv.store_width),
                                            b_.getInt32(four), b_.getInt64(0), b_.getInt64(0), b_.getInt64(0),
                                            b_.getInt64(0), b_.getInt32(flags) });
    service_after_write(lv, old);
    set_value(i, two_val(r));
}

// Lower a $fread into a memory: reuse the $readmem geometry (plane bases,
// stride, count, declared bounds) with optional start-address / element-count
// operands, and read binary bytes MSB-first per element.
void Backend::lower_fread_mem(Index i) {
    const Index fdi  = ir_->payload(i).op_extra.op;
    const auto  info = ir_->extra_data<FreadMemInfo>(ir_->payload(i).op_extra.extra);

    const SignalSlot& s = slot_of(info.mem);
    if (!s.is_array) {
        throw BackendError{ "$fread memory target is not a memory" };
    }
    // Element i's planes at base + i*stride (one slot struct per element); a
    // never-z memory stores no z word, so pass a null z_base.
    auto*        arr_ty  = llvm::cast<llvm::ArrayType>(s.storage_ty);
    llvm::Value* storage = storage_ptr(s);
    llvm::Value* mm_base =
      b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(0) });
    llvm::Value* z_base =
      s.has_z ? b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(1) })
              : llvm::cast<llvm::Value>(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
    const auto  stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(s.slot_ty));
    const Type& at     = std::get<Type>(mod_.pool.get(mod_.signal(info.mem).type));

    llvm::Value*   fd     = cast(must_of(fdi), 32, true);
    const auto     i64_of = [&](Index idx) { return cast(must_of(idx), 64, true); };
    llvm::Value*   start  = info.has_start ? i64_of(info.start) : b_.getInt64(0);
    llvm::Value*   cnt    = info.has_count ? i64_of(info.count) : b_.getInt64(0);
    const uint32_t flags  = (info.has_start ? 1u : 0u) | (info.has_count ? 2u : 0u) | (!s.four_state ? 4u : 0u);

    llvm::Value* r = b_.CreateCall(
      fread_fn(), { fd, mm_base, z_base, b_.getInt32(stride), b_.getInt32(s.elem_count),
                    b_.getInt32(s.width), b_.getInt32(s.four_state ? 1u : 0u), b_.getInt64(at.left),
                    b_.getInt64(at.right), start, cnt, b_.getInt32(flags) });
    // A whole-array write has no per-element edge facts; wake the memory's
    // Any-edge readers when any byte landed, mirroring the deferred
    // array-NBA commit (rt_nba_arr_dirty -> service_any_waiters).
    if (!silent_stores_ && signal_has_waiters(info.mem)) {
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* svc  = llvm::BasicBlock::Create(ctx_, "fread.svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "fread.cont", fn);
        b_.CreateCondBr(b_.CreateICmpNE(r, b_.getInt32(0)), svc, cont);
        b_.SetInsertPoint(svc);
        service_any_waiters(info.mem);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
    if (!silent_stores_) {
        vcd_touch_array(info.mem);   // bulk write: mark the array for the dump
    }
    set_value(i, two_val(r));
}

// Lower a $readmemh / $readmemb: hand the runtime the filename bytes (spilled
// to a stack buffer, or read from the rt_string handle for a string-typed
// filename) plus the destination memory's plane bases (element 0) and geometry. The
// runtime opens the file and writes element planes straight into `state`. The mm
// and z words of element i sit at the plane bases + i*stride (one slot struct
// per element), so the two region bases plus the stride address every element.
void Backend::lower_readmem(Index i) {
    const auto        info  = ir_->extra_data<ReadMemInfo>(ir_->payload(i).op_extra.extra);
    const Index       fname = ir_->payload(i).op_extra.op;
    const SignalSlot& s     = slot_of(info.target);
    if (!s.is_array) {
        throw BackendError{ "$readmem target is not a memory" };
    }

    // A string-object filename (LRM §21.4 allows a string-typed expression) is
    // read straight from its rt_string handle: bytes in natural order, so the
    // runtime is told to skip the packed-vector byte reversal (flags bit 4).
    // Otherwise spill the filename's must-bits to a byte buffer: the value is
    // a packed-ASCII vector (string literal or reg), so its low bytes are the
    // path.
    const ValueType fty          = type_of(fname);
    llvm::Value*  path         = nullptr;
    llvm::Value*  path_len     = nullptr;
    const bool    fname_is_str = fty.is_string;
    if (fname_is_str) {
        llvm::Value* handle = str_of(fname);
        path                = b_.CreateCall(str_data_fn(), { handle });
        path_len            = b_.CreateTrunc(b_.CreateCall(str_len_fn(), { handle }), b_.getInt32Ty());
    } else {
        const uint32_t fbytes = std::max<uint32_t>((fty.width + 7) / 8, 1);
        path                  = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(fbytes), "readmem.path");
        path_len              = b_.getInt32(fbytes);
        b_.CreateAlignedStore(must_of(fname), path, llvm::Align(1));
    }

    // The runtime addresses element i's planes at base + i*stride, so hand it
    // element 0's typed field pointers and the slot struct's alloc size.
    auto*        arr_ty  = llvm::cast<llvm::ArrayType>(s.storage_ty);
    llvm::Value* storage = storage_ptr(s);
    llvm::Value* mm_base =
      b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(0) });
    // A never-z memory (always the case for a 2-state element type) stores no
    // z words; a null z_base tells the runtime to skip the z writes. A
    // 4-state readmem target is a z seed in compute_may_z, so it always keeps
    // its z field.
    llvm::Value* z_base =
      s.has_z ? b_.CreateInBoundsGEP(arr_ty, storage, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(1) })
              : llvm::cast<llvm::Value>(llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)));
    const auto stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(s.slot_ty));

    const Type& at = std::get<Type>(mod_.pool.get(mod_.signal(info.target).type));

    const auto   i64_of = [&](Index idx) { return cast(must_of(idx), 64, true); };
    llvm::Value* start  = info.has_start ? i64_of(info.start) : b_.getInt64(0);
    llvm::Value* finish = info.has_finish ? i64_of(info.finish) : b_.getInt64(0);

    const uint32_t flags = (info.hex ? 1u : 0u) | (info.has_start ? 2u : 0u) |
                           (info.has_finish ? 4u : 0u) | (s.four_state ? 8u : 0u) |
                           (fname_is_str ? 16u : 0u) | (!s.four_state ? 32u : 0u);

    b_.CreateCall(readmem_fn(),
                  { path, path_len, mm_base, z_base, b_.getInt32(stride),
                    b_.getInt32(s.elem_count), b_.getInt32(s.width), b_.getInt64(at.left),
                    b_.getInt64(at.right), start, finish, b_.getInt32(flags) });
    if (!silent_stores_) {
        vcd_touch_array(info.target);   // bulk write: mark the array for the dump
    }
    // TODO: $readmem services no wait queues — a process already waiting on
    // the memory sleeps through the load (pre-existing gap). rt_readmem would
    // need a did-write result (it returns void) to gate an Any-edge wake the
    // way lower_fread_mem does.
}

// The runtime rt_finishing byte, set once a $finish / $stop has been taken.
llvm::Constant* Backend::finishing_gv() {
    return env_->llmod.getOrInsertGlobal("rt_finishing", b_.getInt8Ty());
}

// The current group's deferred-assertion generation counter (lazily created
// in the group's own module), or null when the group carries no deferred
// assertion. Bumped once per activation and read at each deferred report to
// tag it; see lower_group / emit_defer_gen_bump / lower_severity.
llvm::Value* Backend::defer_gen_ptr() {
    return cur_defer_group_ < 0 ? nullptr : defer_gen_ptr_for(static_cast<uint32_t>(cur_defer_group_));
}

// The same counter named by GROUP rather than by "the one being lowered", which
// is what a `disable` of another procedure's outermost scope needs (LRM 16.4.4):
// the bump has to land on the TARGET's counter. Created in the root unit so
// either side can be lowered first and still get the same global.
llvm::Value* Backend::defer_gen_ptr_for(uint32_t g) {
    if (defer_gen_gv_.size() <= g) {
        defer_gen_gv_.resize(g + 1, nullptr);
    }
    if (defer_gen_gv_[g] == nullptr) {
        defer_gen_gv_[g] = new llvm::GlobalVariable(
          root_env_->llmod, b_.getInt64Ty(), false, llvm::GlobalValue::InternalLinkage,
          b_.getInt64(0), "clotho.defer.gen." + std::to_string(g));
    }
    return sym(defer_gen_gv_[g]);
}

// Bump the current group's deferred-assertion generation at an activation
// start (LRM 16.4.2 flush point): reports queued by an earlier activation now
// carry a stale generation and are dropped at maturation. A no-op for a group
// without deferred assertions.
void Backend::emit_defer_gen_bump() {
    emit_defer_gen_bump_at(defer_gen_ptr());
}

void Backend::emit_defer_gen_bump_at(llvm::Value* gen) {
    if (gen == nullptr) {
        return;
    }
    auto* i64 = b_.getInt64Ty();
    b_.CreateStore(b_.CreateAdd(b_.CreateLoad(i64, gen), b_.getInt64(1)), gen);
}

// Lower a $display / $write / $fdisplay / $fwrite: build the descriptor array
// and call the runtime formatter with the destination descriptor.
void Backend::lower_display(Index i) {
    // $display / $write target the standard-output MCD (bit 0).
    emit_display(ir_->extra_data<DisplayInfo>(ir_->payload(i).extra), b_.getInt32(1));
}

void Backend::lower_fdisplay(Index i) {
    // $fdisplay / $fwrite carry an explicit descriptor operand.
    const auto& p = ir_->payload(i).op_extra;
    emit_display(ir_->extra_data<DisplayInfo>(p.extra), cast(must_of(p.op), 32, true));
}

void Backend::emit_display(const DisplayInfo& info, llvm::Value* desc) {
    emit_display_to(info, desc, fwrite_fn());
}

// Render a DisplayInfo's format + args and call `sink` with the rt_fwrite-shaped
// (desc, fmt, argc, argv) tuple. `sink` is rt_fwrite for $display/$fdisplay, or
// rt_monitor_emit (same signature, but change-detecting) for a monitor body.
void Backend::emit_display_to(const DisplayInfo& info, llvm::Value* desc, llvm::FunctionCallee sink) {
    std::string fmt = ir_->extra_bytes(info.fmt, info.fmt_len);
    if (info.newline) {
        fmt += '\n';
    }
    const std::span<const Index> args    = ir_->region(info.args);
    llvm::Value*                 arr     = build_rt_args(args, ir_->words(info.signs));
    llvm::Value*                 fmt_ptr = b_.CreateGlobalString(fmt);
    b_.CreateCall(sink, { desc, fmt_ptr, b_.getInt32(static_cast<uint32_t>(args.size())), arr });
}

} // namespace clotho::ir::backend
