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

// A model string, or a null pointer for an empty one: the runtime reads a null
// comment as none, and an unset option.name as the generated name.
llvm::Constant* Backend::cover_string(const std::string& s) {
    if (s.empty()) {
        return llvm::Constant::getNullValue(raw_ptr_ty());
    }
    return b_.CreateGlobalString(s);
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
                                                         b_.getInt32(cp.is_signed ? 1u : 0u),
                                                         cover_string(cp.inst_comment),
                                                         cover_string(cp.type_comment),
                                                         b_.getInt32(cp.type_weight),
                                                         b_.getInt32(cp.type_goal) }));
        }
        types.push_back(llvm::ConstantStruct::get(
          cover_type_ty(),
          { b_.CreateGlobalString(ct.name),
            b_.CreateGlobalString(ct.banner),
            b_.getInt32(static_cast<uint32_t>(ct.points.size())),
            cover_array(cover_point_ty(), points, tn + ".points"),
            b_.getInt32(ct.weight),
            b_.getInt32(ct.type_weight),
            b_.getInt32(ct.goal),
            b_.getInt32(ct.per_instance ? 1u : 0u),
            b_.getInt32(ct.at_least),
            // An empty option.name is a name, so only an unset one is null.
            ct.inst_name.has_value() ? static_cast<llvm::Constant*>(b_.CreateGlobalString(*ct.inst_name))
                                     : llvm::Constant::getNullValue(raw_ptr_ty()),
            cover_string(ct.inst_comment),
            cover_string(ct.type_comment),
            b_.getInt32(ct.type_goal) }));
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

    // A runtime suffix is a string object: the runtime copies its bytes, so its
    // data pointer and length go where the literal's would.
    llvm::Value* sfx_ptr = nullptr;
    llvm::Value* sfx_len = nullptr;
    if (info.suffix_value != kNoFormatValue) {
        llvm::Value* handle = str_of(info.suffix_value);
        sfx_ptr             = b_.CreateCall(str_data_fn(), { handle });
        sfx_len             = b_.CreateTrunc(b_.CreateCall(str_len_fn(), { handle }), b_.getInt32Ty());
    } else {
        const std::string suffix = ir_->extra_bytes(info.suffix, info.suffix_len);
        sfx_ptr                  = b_.CreateGlobalString(suffix);
        sfx_len                  = b_.getInt32(static_cast<uint32_t>(suffix.size()));
    }
    b_.CreateCall(timeformat_set_fn(), { units, prec, sfx_ptr, sfx_len, width });
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

    std::vector<llvm::Value*> argv{ callee_record(info.func), rng_slot_, wheel_ };
    argv.push_back(proc_arg());
    argv.push_back(callee_home());
    argv.push_back(pend_record_slot());
    llvm::Value* hdl = b_.CreateCall(cf->getFunctionType(), cf, argv);
    emit_trace_spawn(b_, hdl, RT_TRACE_ORG_STROBE, trace_here(), in_coroutine_body() ? hdl_ : nullptr);
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

    std::vector<llvm::Value*> argv{ callee_record(info.func), rng_slot_, wheel_ };
    argv.push_back(proc_arg());
    // NOT the pending body's home. LRM 4.4.2.5 hands an action block to the
    // Reactive region of this time slot no matter which region decided the
    // verdict -- the pend below pushes straight onto the reactive ring -- so the
    // thread this ramp starts is reactive-home by construction rather than by
    // inheritance from the Observed process that pended it.
    argv.push_back(b_.getInt8(kReactiveHomeTag));
    argv.push_back(pend_record_slot());
    llvm::Value* hdl = b_.CreateCall(cf->getFunctionType(), cf, argv);
    emit_trace_spawn(b_, hdl, RT_TRACE_ORG_PEND, trace_here(), in_coroutine_body() ? hdl_ : nullptr);
    emit_trace_wake(b_, hdl, { .kind = RT_TRACE_PEND, .ring = b_.getInt32(RT_TRACE_QUEUE_REACTIVE) });
    emit_reactive_push(b_, hdl);
}

// The waiter queue of one `expect` (LRM 16.17): `{ i32 count, [cap x ptr] parked
// handles }`, created on first use as a root global and keyed by flat monitor
// index. Both halves of an `expect` name it, the wait to push and the wake to
// drain, and both are code of the unit that declares it, which reads the queue's
// address from a field of its instance's record (expect_queue_ptr) rather than
// name the monitor's number.
//
// Shaped like the inactive batch rather than like a ring, because a cohort is
// drained whole and never partially: the count goes back to zero in the same
// step that empties it, so there is no wraparound to mask for.
llvm::GlobalVariable* Backend::expect_queue(uint32_t monitor) {
    if (const auto it = expect_queue_.find(monitor); it != expect_queue_.end()) {
        return it->second;
    }
    // queue_arr_ty_ is shared by every queue, and the drain pushes onto the
    // reactive ring, so both must exist before the first waiter parks.
    ensure_reactive_queue();
    auto* ty = expect_queue_ty();
    auto* gv = new llvm::GlobalVariable(root_env_->llmod, ty, false, llvm::GlobalValue::InternalLinkage,
                                        llvm::ConstantAggregateZero::get(ty),
                                        "clotho.expect" + std::to_string(monitor));
    expect_queue_.emplace(monitor, gv);
    return gv;
}

// The queue of `monitor` as the body being lowered reaches it: the field of the
// own instance's record that holds its address (a unit's code, which names no
// monitor of the design), or the global itself for a monitor no instance
// declares (a static unit's).
llvm::Value* Backend::expect_queue_ptr(uint32_t monitor) {
    const auto [owner, ordinal] = expect_owner_.at(monitor);
    if (owner == SignalSlot::no_anchor) {
        return sym(expect_queue(monitor));
    }
    if (owner != own_anchor_) {
        taint("names the `expect` queue of another instance");
        return sym(expect_queue(monitor));
    }
    // The queue is created where the record is filled; the bodies that reach it
    // need the ring it drains onto.
    ensure_reactive_queue();
    return own_field(anchors_[owner].ctx_expect_base + ordinal, llvm::PointerType::getUnqual(ctx_));
}

// What the trace names the `expect` of `monitor` by, in a waiter's park and in the verdict that wakes
// it: the own instance's record where the queue is a field of it, which tells two instances sharing
// code apart even if their queues were one (B43); else the queue itself.
llvm::Value* Backend::expect_trace_id(uint32_t monitor) {
    const auto [owner, ordinal] = expect_owner_.at(monitor);
    (void)ordinal;
    if (owner != SignalSlot::no_anchor && owner == own_anchor_) {
        return own_ctx_ != nullptr ? static_cast<llvm::Value*>(own_ctx_) : static_cast<llvm::Value*>(ensure_ctx(anchors_[owner]));
    }
    return expect_queue_ptr(monitor);
}

// The type of an `expect` queue: the count, then the parked handles.
llvm::StructType* Backend::expect_queue_ty() {
    return llvm::StructType::get(ctx_, { llvm::Type::getInt32Ty(ctx_), queue_arr_ty_ });
}

// The slot of parked handle `idx` of an `expect` queue: the handles follow the
// count, kExpectHandlesOffset bytes in, so the code that reaches the queue through
// a record field names no capacity.
llvm::Value* Backend::expect_handle_slot(llvm::Value* queue, llvm::Value* idx) {
    assert(root_env_->llmod.getDataLayout().getStructLayout(expect_queue_ty())->getElementOffset(1) ==
           kExpectHandlesOffset);
    auto* handles = b_.CreateInBoundsGEP(b_.getInt8Ty(), queue, b_.getInt64(kExpectHandlesOffset));
    return b_.CreateInBoundsGEP(llvm::PointerType::getUnqual(ctx_), handles, idx);
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
    const auto info = ir_->extra_data<ExpectInfo>(ir_->payload(i).extra);
    llvm::Value* q  = expect_queue_ptr(info.monitor);

    auto*        i32  = b_.getInt32Ty();
    llvm::Value* sp   = b_.CreateLoad(i32, q);
    llvm::Value* slot = expect_handle_slot(q, sp);
    b_.CreateStore(hdl_, slot);
    b_.CreateStore(b_.CreateAdd(sp, b_.getInt32(1)), q);
    // Armed for the same reason a `wait` arms: while parked here this process
    // sits in no other queue, so the flag must say "not scheduled" or a later
    // event wake would be dropped as a duplicate.
    emit_arm_wake_word();
    emit_suspend("expect.resume", { .kind = RT_TRACE_CTL_EXPECT, .rec = trace_on() ? expect_trace_id(info.monitor) : nullptr });

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
    const auto&  p      = ir_->payload(i).op_imm;
    auto*        i32    = b_.getInt32Ty();
    auto*        ptr_ty = llvm::PointerType::getUnqual(ctx_);

    auto* drain = llvm::BasicBlock::Create(ctx_, "expect.drain", fn_);
    auto* head  = llvm::BasicBlock::Create(ctx_, "expect.drain.head", fn_);
    auto* body  = llvm::BasicBlock::Create(ctx_, "expect.drain.body", fn_);
    auto* done  = llvm::BasicBlock::Create(ctx_, "expect.drain.done", fn_);

    // must=1 holds only for a definite 1, so an x verdict bit cannot resolve
    // anybody -- the same reading lower_cond gives every condition.
    b_.CreateCondBr(b_.CreateICmpNE(must_of(p.op), zero(1)), drain, done);

    b_.SetInsertPoint(drain);
    llvm::Value* q        = expect_queue_ptr(p.imm);
    llvm::Value* trace_id = trace_on() ? expect_trace_id(p.imm) : nullptr;
    llvm::Value* n = b_.CreateLoad(i32, q);
    // Emptied before the pushes, not after: a woken process may execute this
    // very `expect` again once it runs, and that push must land in an empty
    // queue rather than be swallowed by this drain's own store.
    b_.CreateStore(b_.getInt32(0), q);
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    llvm::PHINode* idx = b_.CreatePHI(i32, 2, "waiter");
    idx->addIncoming(b_.getInt32(0), drain);
    b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* slot   = expect_handle_slot(q, idx);
    llvm::Value* parked = b_.CreateLoad(ptr_ty, slot);
    emit_trace_wake(b_, parked, { .kind = RT_TRACE_EXPECT, .obj = trace_id, .ring = b_.getInt32(RT_TRACE_QUEUE_REACTIVE) });
    emit_reactive_push(b_, parked);
    llvm::Value* next = b_.CreateAdd(idx, b_.getInt32(1));
    idx->addIncoming(next, b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
}

// The task-call record a pended body ($strobe, an action block) is ramped with.
// It CANNOT be an alloca in the pending body's frame: that frame can be freed
// before the drain resumes the pended body -- an Observed process is ramped,
// resumed and destroyed in one go (emit_run_once), and an `initial` that
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
        pend_record_gv_ = new llvm::GlobalVariable(root_env_->llmod,
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
            // Thunk-elided process: a plain void body. A body has one of these
            // exits per call that could reach a $finish, so they share a block.
            llvm::Function* const fn = b_.GetInsertBlock()->getParent();
            if (thunk_finish_bb_ == nullptr || thunk_finish_bb_->getParent() != fn) {
                const llvm::IRBuilderBase::InsertPointGuard guard(b_);
                thunk_finish_bb_ = llvm::BasicBlock::Create(ctx_, "finish.exit", fn);
                b_.SetInsertPoint(thunk_finish_bb_);
                deinit_body_strings();
                b_.CreateRetVoid();
            }
            b_.CreateBr(thunk_finish_bb_);
        }
        return;
    }
    if (cur_is_task_) {
        // Task: the same epilogue an explicit `return` takes.
        b_.CreateBr(leave_blocks(0, task_epilogue_bb_));
        return;
    }
    // Function: its epilogue past the copy-out (lower_function). The result is
    // dead — the caller tests rt_finishing and leaves without reading it — so
    // whatever the result slot holds is the honest thing to hand back.
    b_.CreateBr(fn_teardown_bb_);
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
    // The ring exists in every design (emit_active_run makes it), so it is made
    // here rather than tested for: a body lowered before the first push to it
    // would otherwise lack this reset, and two instances of one unit lowered
    // either side of that push would be different code.
    ensure_active_queue();
    b_.CreateStore(b_.CreateLoad(b_.getInt32Ty(), sym(active_tail_gv_)), sym(active_head_gv_));
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
// prefix, the simulation time of the violation, the message kind and the report
// generation of the thread executing it. The runtime queues it and reports in the
// Observed region, where LRM 12.4.2 / 12.5.3 place them (docs/LRM_NOTES.md, note
// 18), unless the thread reached a flush point first (LRM 16.4.2), so the region
// continues normally.
void Backend::lower_case_viol(Index i) {
    const auto        info   = ir_->extra_data<CaseViolInfo>(ir_->payload(i).extra);
    const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
    llvm::Value*      now    = b_.CreateCall(wheel_now_fn(), { wheel_ });
    llvm::Value*      gen    = gen_gv_ != nullptr ? load_gen()
                                                  : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    b_.CreateCall(observed_case_violation_fn(),
                  { b_.CreateGlobalString(banner), now, b_.getInt32(info.kind), gen });
}

// Lower a severity report ($info / $warning / $error / $fatal, or an
// immediate-assertion default failure): build the message descriptor array
// and call the runtime, passing the severity label, the banner's pieces, and
// the format string. Needs no wheel (the report carries no simulation time), so
// it lowers cleanly inside a pure function too.
//
// The banner is not built here. Its leading text, the instance's path (the
// borrowed handle of an InstPath, null when the banner is all text) and its
// trailing text (null when empty) go to the runtime as they are, which joins them
// when it issues the line. Building the banner at the call site would put its
// temporaries in the prologue of every body that holds a report, paid by every
// activation, to name an instance on the rare one that fires.
void Backend::lower_severity(Index i) {
    const auto        info   = ir_->extra_data<SeverityInfo>(ir_->payload(i).extra);
    const std::string banner = ir_->extra_bytes(info.banner, info.banner_len);
    const std::string tail   = ir_->extra_bytes(info.tail, info.tail_len);
    const std::string fmt    = ir_->extra_bytes(info.fmt, info.fmt_len);

    auto*        null = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    llvm::Value* path = info.path != kNoIndex ? str_of(info.path) : static_cast<llvm::Value*>(null);
    llvm::Value* text = tail.empty() ? static_cast<llvm::Value*>(null) : b_.CreateGlobalString(tail);
    const std::span<const Index> args = ir_->region(info.args);
    llvm::Value*                 arr  = build_rt_args(args, ir_->words(info.signs));
    if (info.defer == 0) {
        b_.CreateCall(severity_fn(), { b_.getInt32(static_cast<int32_t>(info.level)),
                                       b_.CreateGlobalString(banner), path, text, b_.CreateGlobalString(fmt),
                                       b_.getInt32(static_cast<uint32_t>(args.size())), arr });
        return;
    }
    // Deferred immediate assertion action (LRM 16.4): queue the rendered
    // report tagged with the process's current generation. The generation
    // pointer is this group's counter (defer_gen_ptr) — always set when a
    // deferred Severity lowers, since is_simple_always forces the group to a
    // coroutine and lower_group set cur_defer_group_. defer == 2 selects the
    // Postponed (final) maturation, defer == 1 the Observed (`#0`).
    // In a subroutine the report belongs to the thread that called it: its published generation
    // (a task keeps what it read at entry in its frame; a fork child's is its own cell), which is
    // null at run time when that thread has no counter.
    llvm::Value* gen = cur_fn_ == nullptr ? defer_gen_ptr()
                       : gen_gv_ == nullptr ? nullptr
                       : gen_slot_ != nullptr ? static_cast<llvm::Value*>(b_.CreateLoad(raw_ptr_ty(), gen_slot_))
                                              : load_gen();
    auto*        i64 = b_.getInt64Ty();
    llvm::Value* gen_val = static_cast<llvm::Value*>(b_.getInt64(0));
    if (gen != nullptr && cur_fn_ == nullptr) {
        gen_val = b_.CreateLoad(i64, gen);
    } else if (gen != nullptr) {
        auto* zero = new llvm::GlobalVariable(env_->llmod, i64, true, llvm::GlobalValue::PrivateLinkage,
                                              b_.getInt64(0), "clotho.gen.zero");
        gen_val = b_.CreateLoad(i64, b_.CreateSelect(b_.CreateIsNull(gen), zero, gen));
    }
    llvm::Value* gen_arg =
      gen != nullptr ? gen
                     : static_cast<llvm::Value*>(null);
    b_.CreateCall(defer_report_fn(),
                  { b_.getInt32(static_cast<int32_t>(info.level)),
                    b_.getInt64(info.defer == 2 ? 1 : 0), gen_arg, gen_val,
                    b_.CreateGlobalString(banner), path, text, b_.CreateGlobalString(fmt),
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

// The flat covergroup type `t` as an IR value: a type the instance whose code is
// being lowered declares is its unit's ordinal-th, whose index sits in the
// instance's record (so the code names no type); a type of no instance (a
// package's, a class's) is the same in every instance and is the constant; another
// instance's is that instance's own, which a shared lowering cannot have.
llvm::Value* Backend::cover_type_value(uint32_t t) {
    const auto [owner, ordinal] = cover_owner_.at(t);
    if (owner == SignalSlot::no_anchor) {
        return b_.getInt32(t);
    }
    if (owner == own_anchor_ && own_ctx_ != nullptr) {
        Anchor& a = anchors_[owner];
        return ctx_field(a, a.ctx_cov_base + ordinal, b_.getInt32Ty());
    }
    taint("names a covergroup type of another instance");
    return b_.getInt32(t);
}

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
        case CoverOp::New: {
            // `ctx` is the enclosing object for a class-embedded covergroup, so
            // the generated sample body can reach the class properties its
            // coverpoints name, and null otherwise. The constructing instance's
            // name prefix arrives in two pieces the runtime joins when it copies
            // them into the new instance's name: the instance's path (the
            // borrowed handle of an InstPath, null outside every instance) and
            // the text after it.
            if (args.empty() || args.size() > 2) {
                throw BackendError{ "internal: cover_query new takes a context and at most an instance path" };
            }
            const std::string text = ir_->extra_bytes(info.text, info.text_len);
            llvm::Value* path = args.size() == 2 ? str_of(args[1])
                                                 : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(
                                                     llvm::PointerType::getUnqual(ctx_)));
            hval(b_.CreateCall(cover_new_fn(),
                               { cover_type_value(info.type_idx),
                                 cast(must_of(args[0]), 64, false),
                                 path,
                                 b_.CreateGlobalString(text),
                                 b_.getInt64(text.size()) }));
            return;
        }
        case CoverOp::Ctx:
            hval(b_.CreateCall(cover_ctx_fn(), { handle() }));
            return;
        case CoverOp::First:
            hval(b_.CreateCall(cover_first_fn(), { cover_type_value(info.type_idx) }));
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
                                               { cover_type_value(info.type_idx) })));
            return;
        case CoverOp::TypeCovered:
            ival(b_.CreateCall(cover_type_query_fn("rt_cover_type_covered", i32), { cover_type_value(info.type_idx) }));
            return;
        case CoverOp::TypeTotal:
            ival(b_.CreateCall(cover_type_query_fn("rt_cover_type_total", i32), { cover_type_value(info.type_idx) }));
            return;
        case CoverOp::TypePointCoverage:
            set_value(i,
                      real_plane(b_.CreateCall(cover_type_point_query_fn("rt_cover_type_point_coverage", dbl),
                                               { cover_type_value(info.type_idx), b_.getInt32(info.point) })));
            return;
        case CoverOp::TypePointCovered:
            ival(b_.CreateCall(cover_type_point_query_fn("rt_cover_type_point_covered", i32),
                               { cover_type_value(info.type_idx), b_.getInt32(info.point) }));
            return;
        case CoverOp::TypePointTotal:
            ival(b_.CreateCall(cover_type_point_query_fn("rt_cover_type_point_total", i32),
                               { cover_type_value(info.type_idx), b_.getInt32(info.point) }));
            return;
        case CoverOp::OptGet:
            if (args.size() != 2) {
                throw BackendError{ "internal: cover_query opt_get takes an option id" };
            }
            set_value(i, two_val(b_.CreateCall(cover_opt_get_fn(),
                                               { handle(), b_.getInt32(info.point), cast(must_of(args[1]), 32, false) })));
            return;
        case CoverOp::OptGetStr:
            if (args.size() != 2) {
                throw BackendError{ "internal: cover_query opt_get_str takes an option id" };
            }
            b_.CreateCall(cover_opt_get_str_fn(),
                          { str_of(i), handle(), b_.getInt32(info.point), cast(must_of(args[1]), 32, false),
                            alloc_site_global(loc_text(i), "string") });
            return;
        case CoverOp::TypeOptGet:
            if (args.size() != 1) {
                throw BackendError{ "internal: cover_query type_opt_get takes an option id" };
            }
            set_value(i, two_val(b_.CreateCall(cover_type_opt_get_fn(),
                                               { cover_type_value(info.type_idx), b_.getInt32(info.point),
                                                 cast(must_of(args[0]), 32, false) })));
            return;
        case CoverOp::TypeOptGetStr:
            if (args.size() != 1) {
                throw BackendError{ "internal: cover_query type_opt_get_str takes an option id" };
            }
            b_.CreateCall(cover_type_opt_get_str_fn(),
                          { str_of(i), cover_type_value(info.type_idx), b_.getInt32(info.point),
                            cast(must_of(args[0]), 32, false), alloc_site_global(loc_text(i), "string") });
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
        case CoverOp::StartPoint:
            if (args.size() != 2) {
                throw BackendError{ "internal: cover_ctl start_point takes an enable" };
            }
            b_.CreateCall(cover_start_point_fn(),
                          { h, b_.getInt32(info.point), cast(must_of(args[1]), 32, false) });
            return;
        case CoverOp::OptSet:
            if (args.size() != 3) {
                throw BackendError{ "internal: cover_ctl opt_set takes an option id and a value" };
            }
            b_.CreateCall(cover_opt_set_fn(),
                          { h, b_.getInt32(info.point), cast(must_of(args[1]), 32, false), cast(must_of(args[2]), 64, true) });
            return;
        case CoverOp::OptSetStr: {
            if (args.size() != 3) {
                throw BackendError{ "internal: cover_ctl opt_set_str takes an option id and a string" };
            }
            const AsciiArg s = spill_ascii_arg(args[2], "cover.opt");
            b_.CreateCall(cover_opt_set_str_fn(),
                          { h, b_.getInt32(info.point), cast(must_of(args[1]), 32, false), s.ptr,
                            b_.CreateZExt(s.len, b_.getInt64Ty()) });
            return;
        }
        case CoverOp::TypeOptSet:
            if (args.size() != 2) {
                throw BackendError{ "internal: cover_ctl type_opt_set takes an option id and a value" };
            }
            b_.CreateCall(cover_type_opt_set_fn(),
                          { cover_type_value(info.type_idx), b_.getInt32(info.point),
                            cast(must_of(args[0]), 32, false), cast(must_of(args[1]), 64, true) });
            return;
        case CoverOp::TypeOptSetStr: {
            if (args.size() != 2) {
                throw BackendError{ "internal: cover_ctl type_opt_set_str takes an option id and a string" };
            }
            const AsciiArg s = spill_ascii_arg(args[1], "cover.type_opt");
            b_.CreateCall(cover_type_opt_set_str_fn(),
                          { cover_type_value(info.type_idx), b_.getInt32(info.point),
                            cast(must_of(args[0]), 32, false), s.ptr, b_.CreateZExt(s.len, b_.getInt64Ty()) });
            return;
        }
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
    const Guarded lv = write_dest(dst);
    if (lv.is_string) {
        llvm::Value* snap = str_chg_snapshot(lv);
        llvm::Value* r    = b_.CreateCall(
          fgets_str_fn(), { lv_string_ptr(lv, StringUse::Write), fd, alloc_site_global(loc_text(i), "string") });
        str_chg_after(lv, snap);
        set_value(i, two_val(r));
        return;
    }
    const auto [mm, z, width, flags] = string_dest(lv, "$fgets");
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
//
// The pointers are the place's own where its select is valid and a scratch slot's
// where not (safe_lvalue), so the runtime never writes through a wild address and
// an invalid target is a no-op (LRM 7.4.5). The caller wakes waiters through
// service_snapshot / service_after_write, which skip an invalid place as well.
std::tuple<llvm::Value*, llvm::Value*, uint32_t, uint32_t> Backend::string_dest(const Guarded& g, const char* who) {
    if (g.is_string) {
        // Every caller (fgets/ferror/sformat) checks lv.is_string and branches to
        // its own rt_string-handle path before reaching here; this is defense in
        // depth against a future caller doing otherwise.
        throw BackendError{ std::string(who) + " into a string object is not supported" };
    }
    if (g.width != g.store_width) {
        // TODO: a part-select target (s[15:0]) writes only a slice.
        throw BackendError{ std::string(who) + " into a part-select target is not supported" };
    }
    if (g.store_width % 8 != 0) {
        // TODO: a non-byte-multiple width leaves the must|may plane halves
        // byte-unaligned; targets are byte vectors in practice.
        throw BackendError{ std::string(who) +
                            " into a target whose width is not a multiple of 8 is not supported" };
    }
    // A never-z target (z elision) has no z word; pass null and the runtime
    // skips the z write.
    const LValue lv = safe_lvalue(g);
    llvm::Value* mm = lv_mm_ptr(lv);
    llvm::Value* z =
      lv.has_z ? lv_z_ptr(lv) : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    return { mm, z, lv.store_width, !lv.four_state ? 1u : 0u };
}

Guarded Backend::write_dest(Index i) {
    const Guarded& g = lvalue_of(i);
    return g.is_assoc ? assoc_elem_lvalue(g, /*for_write=*/true) : g;
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
    const Guarded   lv = write_dest(info.dst);
    if (lv.is_string) {
        llvm::Value* snap = str_chg_snapshot(lv);
        b_.CreateCall(sformat_str_fn(), { lv_string_ptr(lv, StringUse::Write), f.fmt, argc, arr,
                                          alloc_site_global(loc_text(i), "string"), f.dyn });
        str_chg_after(lv, snap);
        return;
    }
    const auto [mm, z, width, flags] = string_dest(lv, "$sformat / $swrite");
    const ServiceSnapshot old          = service_snapshot(lv);
    b_.CreateCall(sformat_fn(),
                  { mm, z, b_.getInt32(width), f.fmt, argc, arr, b_.getInt32(flags), f.dyn });
    service_after_write(lv, old);
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
    const Guarded lv   = write_dest(dst);
    if (lv.is_string) {
        llvm::Value* snap = str_chg_snapshot(lv);
        llvm::Value* r    = b_.CreateCall(
          ferror_str_fn(), { fd, lv_string_ptr(lv, StringUse::Write), alloc_site_global(loc_text(i), "string") });
        str_chg_after(lv, snap);
        set_value(i, two_val(r));
        return;
    }
    const auto [mm, z, width, flags] = string_dest(lv, "$ferror");
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
    // (null where a target needs no servicing), and the places they belong to,
    // resolved once so a deferred associative element is not resolved twice.
    std::vector<ServiceSnapshot> olds(n);
    std::vector<Guarded>         dests;
    dests.reserve(n);
    if (n > 0) {
        arr = b_.CreateAlloca(sty, b_.getInt32(n), "scan_args");
        for (uint32_t k = 0; k < n; ++k) {
            const Guarded lv   = dests.emplace_back(write_dest(outs[k]));
            llvm::Value*  elt = b_.CreateConstInBoundsGEP1_32(sty, arr, k);
            llvm::Value*  nul = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
            if (lv.is_string) {
                // A `string` object destination (flags bit1): the descriptor
                // carries the rt_string handle and its alloc site instead of
                // plane pointers, and the engine assigns a %s token verbatim or
                // stores another conversion's value cast to string, and a `%5s`
                // field width caps
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
            // word (parsed x/z digits drop to 0 per LRM 6.3.1). An invalid
            // target hands the runtime a scratch slot (safe_lvalue).
            const LValue dest = safe_lvalue(lv);
            llvm::Value* mm   = lv_mm_ptr(dest);
            llvm::Value* z    = dest.has_z ? lv_z_ptr(dest) : nul;
            b_.CreateStore(mm, b_.CreateStructGEP(sty, elt, 0));
            b_.CreateStore(z, b_.CreateStructGEP(sty, elt, 1));
            b_.CreateStore(b_.getInt32(dest.store_width), b_.CreateStructGEP(sty, elt, 2));
            b_.CreateStore(b_.getInt32(!dest.four_state ? 1u : 0u), b_.CreateStructGEP(sty, elt, 3));
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
        service_after_write(dests[k], olds[k]);
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

    const Guarded lv = write_dest(dst);
    if (lv.is_string) {
        llvm::Value* snap = str_chg_snapshot(lv);
        llvm::Value* r    = b_.CreateCall(
          value_plusargs_str_fn(),
          { prefix_ptr, prefix_len, b_.getInt32(info.conv), lv_string_ptr(lv, StringUse::Write),
            alloc_site_global(loc_text(i), "string") });
        str_chg_after(lv, snap);
        set_value(i, two_val(r));
        return;
    }
    if (lv.width != lv.store_width) {
        // TODO: a part-select $value$plusargs target (x[7:0]).
        throw BackendError{ "$value$plusargs into a part-select target is not supported" };
    }

    // An invalid target hands the runtime a scratch slot (safe_lvalue).
    const LValue      dest = safe_lvalue(lv);
    llvm::StructType* sty  = scan_arg_struct_ty();
    llvm::Value*      arg  = b_.CreateAlloca(sty, nullptr, "plusarg_val");
    llvm::Value*      mm   = lv_mm_ptr(dest);
    llvm::Value*      z    = dest.has_z ? lv_z_ptr(dest)
                                        : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    b_.CreateStore(mm, b_.CreateStructGEP(sty, arg, 0));
    b_.CreateStore(z, b_.CreateStructGEP(sty, arg, 1));
    b_.CreateStore(b_.getInt32(dest.store_width), b_.CreateStructGEP(sty, arg, 2));
    b_.CreateStore(b_.getInt32(!dest.four_state ? 1u : 0u), b_.CreateStructGEP(sty, arg, 3));

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

// The unpacked dimensions of the memory `array_type` below its highest, as the
// constant (left, right) pairs rt_readmem and rt_fread order the leaves of one word
// by (LRM 21.4.3), outermost first, with their count. A memory of one dimension has
// none: null and 0.
std::pair<llvm::Value*, uint32_t> Backend::mem_inner_dims(Ref array_type) {
    std::vector<llvm::Constant*> bounds;
    for (Ref e = std::get<Type>(mod_.pool.get(array_type)).element;;) {
        const Type t = std::get<Type>(mod_.pool.get(e));
        if (t.kind != Type::Kind::UnpackedArray) {
            break;
        }
        bounds.push_back(b_.getInt64(t.left));
        bounds.push_back(b_.getInt64(t.right));
        e = t.element;
    }
    if (bounds.empty()) {
        return { llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_)), 0 };
    }
    auto* arr_ty = llvm::ArrayType::get(b_.getInt64Ty(), bounds.size());
    auto* gv     = new llvm::GlobalVariable(env_->llmod, arr_ty, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arr_ty, bounds), "clotho.mem.dims");
    return { gv, static_cast<uint32_t>(bounds.size() / 2) };
}

// The array a $readmem or $fread loads (LRM 21.4, 21.3.4.4), resolved for the runtime.
//
// `target` is an array lvalue: a memory held in a module signal, a member of an
// unpacked struct, a class property or an interface member that is an array, an
// element of one that is itself an array, or a dynamic array or queue wherever it
// lives. `type` is the array the load fills, and the runtime numbers its words by that
// type's highest dimension, so an address is an index into the array the task names.
// An indexed slice `target[base +: n]` / `[base -: n]` is not an lvalue (its first
// word is at a runtime position): `slice` says so, `slice_base` is its base and
// `base_type` the type of `target`. Its addresses are the indices it selects, as for
// `target[a:b]` (21.4: `a[2 +: 4]` is `a[2:5]`), and only the words that lie in `target`'s
// array exist: 7.4.5 makes writing an array through an invalid index a no-operation,
// which Clotho applies to each word, as it does to a write of a slice
// (emit_indexed_slice_copy), so a slice that hangs off the array keeps the alignment
// of its words. An x or z base leaves none. A slice with constant bounds takes the same
// form, its base the first of its bounds.
//
// A slice of a queue (`slice` is kMemSliceQueue, `slice_base` and `slice_end` its first
// and last index) is the elements of the queue LRM 7.10.1 gives it: from the first index
// but not below 0 to the last but not past the end of the queue, none when that range is
// empty or a bound has an x or z bit. Its words are addressed by their index in the
// queue, so its range is that of the elements that are left, and its first word is
// where the ring puts the first of them.
//
// Where the leaves are depends on the lvalue. A memory in a module signal is
// `[N x slot]`, the slot the may-z analysis chose. Every other array has its type's
// canonical slot: an array inside an aggregate lies one slot after another from the
// pointer its path reaches, and a dynamic array or queue is `len` words of its element
// in the buffer its handle holds, addressed 0 to len - 1. LRM 21.4.1 fixes their size:
// a load does not resize them, and an address past the last word is an address past
// the end of a memory. A queue's words sit in its ring, which the runtime walks from
// the head slot (rt_readmem).
//
// An associative array is the one target that is not storage the load finds: LRM 21.4.1
// has a loaded address create the element of that index. The load goes through the handle
// and the array's descriptor (rt_readmem_assoc, rt_fread_assoc), and the slot facts are
// those of one element, an array of them (an element that is itself an array) being
// the leaves of one word. An entry that is itself an array or a container, `aa[k]`, is
// loaded as the array it is, and created if it is not there.
//
// A target that can be invalid (a runtime index outside its array, or with an x or z
// bit) names no word then: the runtime is handed a word count of 0 and a scratch slot
// to point at, and does nothing.
Backend::MemTarget Backend::resolve_mem_target(Index       target,
                                               Ref         type,
                                               Ref         base_type,
                                               uint32_t    slice,
                                               Index       slice_base,
                                               Index       slice_end,
                                               const char* task) {
    Guarded g = lvalue_of(target);
    // An entry of an associative array that is itself an array or a container, `aa[k]`, is the
    // target of the load, which creates it as a store to it would (LRM 7.8.7). An x or z key
    // names none, which the verdict of the place carries.
    if (g.is_assoc) {
        g = assoc_elem_lvalue(g, /*for_write=*/true);
    }
    if (g.is_string) {
        throw BackendError{ std::string(task) + " target is an array of strings, which is not supported" };
    }
    emit_bounds_reports(g, /*is_write=*/true);

    const Ref  named     = slice != kMemSliceNone ? base_type : type;
    const Type named_ty  = std::get<Type>(mod_.pool.get(named));
    const Type loaded    = std::get<Type>(mod_.pool.get(type));
    const bool queue     = named_ty.kind == Type::Kind::Queue;
    const bool assoc     = named_ty.kind == Type::Kind::Assoc;
    const bool container = queue || assoc || named_ty.kind == Type::Kind::DynArray;
    // Only a slice of a queue has runtime bounds, and nothing else is cut by them.
    if (slice != kMemSliceNone && (slice == kMemSliceQueue) != queue) {
        throw BackendError{ "internal: a slice of a queue is a queue slice, and no other slice is" };
    }

    // The leaf slot. A memory held in a signal has the one the may-z analysis chose; the
    // arrays of aggregates and containers the canonical one of their type, as every
    // other access to them uses (agg_leaf_slot_ty).
    const bool        flat = !container && !g.is_agg;
    llvm::StructType* slot_ty;
    bool              four_state;
    bool              has_z;
    uint32_t          width;
    if (flat) {
        if (!g.is_signal || g.array_ty == nullptr) {
            throw BackendError{ std::string(task) + " target is not a memory held in a module signal" };
        }
        slot_ty    = g.slot_ty;
        four_state = g.four_state;
        has_z      = g.has_z;
        width      = g.store_width;
    } else {
        const Ref  leaf = types::leaf_type(mod_.pool, container ? named_ty.element : type);
        const Type lt   = std::get<Type>(mod_.pool.get(leaf));
        // The words of an array of containers have no fixed size, and a string, a class
        // handle or an unpacked struct in a slot is not a value a file can fill.
        if (lt.kind == Type::Kind::DynArray || lt.kind == Type::Kind::Queue || lt.kind == Type::Kind::Assoc) {
            throw BackendError{ std::string(task) + " target is an array of dynamic arrays, queues or associative "
                                                    "arrays, which is not supported" };
        }
        const bool plain = lt.kind == Type::Kind::Logic || lt.kind == Type::Kind::Bit ||
                           lt.kind == Type::Kind::PackedArray || lt.kind == Type::Kind::Real ||
                           lt.kind == Type::Kind::ShortReal || (lt.kind == Type::Kind::Struct && lt.is_packed);
        if (!plain) {
            throw BackendError{ std::string(task) + " target's elements are not integral or real values" };
        }
        const Layout lay = mod_.pool.layout(leaf);
        width            = lay.width == 0 ? 1 : lay.width;
        four_state       = lay.domain == Domain::Four;
        has_z            = four_state;
        slot_ty          = agg_leaf_slot_ty(leaf);
    }

    // An associative array has no planes to point at: its elements are created by the load,
    // which the runtime does through the handle (rt_readmem_assoc, rt_fread_assoc). A
    // target that can be invalid names no array then, and the runtime does nothing.
    if (assoc) {
        const AssocKeyInfo key = assoc_key_info(named);
        if (key.is_string || key.is_agg) {
            throw BackendError{ "internal: the index of an associative array a memory load fills is not integral" };
        }
        const llvm::DataLayout& dl = env_->llmod.getDataLayout();
        MemTarget               t;
        t.assoc        = true;
        t.assoc_handle = lv_handle_ptr(g, HandleUse::Place);
        t.assoc_ops    = elem_ops_global(named);
        t.key_width    = key.W;
        t.key_signed   = key.is_signed;
        t.stride       = static_cast<uint32_t>(dl.getTypeAllocSize(slot_ty));
        t.width        = width;
        t.four_state   = four_state;
        t.z_off        = has_z ? static_cast<uint32_t>(dl.getStructLayout(slot_ty)->getElementOffset(1)) : 0;
        t.inner        = mem_inner_dims(type);
        t.assoc_valid  = g.valid();
        t.place        = static_cast<const LValueInfo&>(g);
        t.container    = true;
        return t;
    }

    // The words of the array `target` names, and of the one the load fills. A dynamic
    // array or queue has as many as it holds now.
    const auto dim = [](const Type& t) -> int64_t {
        return (t.left <= t.right ? int64_t{ t.right } - t.left : int64_t{ t.left } - t.right) + 1;
    };
    llvm::Value* zero = b_.getInt64(0);
    llvm::Value* data = nullptr;
    llvm::Value* head = zero;
    llvm::Value* cap  = zero;
    llvm::Value* len  = nullptr;
    if (container) {
        // Nothing here notifies: the load does, once it knows it changed a leaf.
        llvm::Value* h = lv_handle_ptr(g, HandleUse::Place);
        if (queue) {
            data = b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(queue_slot_ty(), h, 0));
            head = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), h, 1));
            len  = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), h, 2));
            cap  = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), h, 3));
        } else {
            data = b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(dynarr_slot_ty(), h, 0));
            len  = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), h, 1));
        }
    }
    const int64_t words = dim(loaded);
    llvm::Value*  size  = container ? len : b_.getInt64(dim(named_ty));
    llvm::Value*  count = container && slice == kMemSliceNone ? len : b_.getInt64(words);
    llvm::Value*  lo    = zero;
    // The bounds of the array the load fills, which its addresses are numbered by.
    llvm::Value* left_addr  = container && slice == kMemSliceNone ? zero : b_.getInt64(loaded.left);
    llvm::Value* right_addr = container && slice == kMemSliceNone ? b_.CreateSub(len, b_.getInt64(1))
                                                                   : b_.getInt64(loaded.right);
    // The leaf of the whole array where the first word that exists starts.
    llvm::Value* first = g.elem_idx != nullptr ? cast(g.elem_idx, 64, false) : zero;
    // The ring slot of the first word that exists, for a queue.
    llvm::Value* ring_first = head;
    if (slice == kMemSliceQueue) {
        // LRM 7.10.1: the indices from the first bound, but not below 0, to the last, but not
        // past the end of the queue. They are the addresses of the load, and there are none
        // when the range is empty or a bound has an x or z bit.
        llvm::Value* from = select_index(value_of(slice_base), type_of(slice_base));
        llvm::Value* to   = select_index(value_of(slice_end), type_of(slice_end));
        left_addr         = smax(from, zero);
        right_addr        = smin(to, b_.CreateSub(len, b_.getInt64(1)));
        count             = b_.CreateSelect(b_.CreateICmpSLE(left_addr, right_addr),
                                            b_.CreateAdd(b_.CreateSub(right_addr, left_addr), b_.getInt64(1)), zero);
        count = b_.CreateSelect(b_.CreateOr(any_unknown(slice_base), any_unknown(slice_end)), zero, count);
        // The word at `left_addr` is that many slots on from the head, which wraps once at
        // most: the head is below the capacity and a word that exists below the length.
        llvm::Value* at = b_.CreateAdd(head, left_addr);
        ring_first      = b_.CreateSelect(b_.CreateICmpUGE(at, cap), b_.CreateSub(at, cap), at);
    } else if (slice != kMemSliceNone) {
        // A dynamic array is always 0-based and ascending (LRM 7.5).
        const bool     ascending   = container || named_ty.left <= named_ty.right;
        const int64_t  left        = container ? 0 : int64_t{ named_ty.left };
        const uint32_t word_leaves = array_leaf_count(named_ty.element);
        llvm::Value*   at          = select_index(value_of(slice_base), type_of(slice_base));
        // The lowest coordinate the slice selects. Its first word is that one for an
        // ascending array and the highest for a descending one (LRM 11.5.1 keeps the
        // declared direction), and its position in `target`'s array is counted from
        // the array's left bound.
        llvm::Value* low = slice == kMemSliceUp ? at : b_.CreateSub(at, b_.getInt64(words - 1));
        // The slice's own addresses are the indices it selects, whichever way it is written:
        // `a[2 +: 4]` is `a[2:5]` (LRM 21.4: start and finish lie within the slice's range).
        llvm::Value* high = b_.CreateAdd(low, b_.getInt64(words - 1));
        left_addr         = ascending ? low : high;
        right_addr        = ascending ? high : low;
        llvm::Value* pos = ascending ? b_.CreateSub(low, b_.getInt64(left))
                                     : b_.CreateSub(b_.getInt64(left), b_.CreateAdd(low, b_.getInt64(words - 1)));
        // The slice's words at positions [wlo, whi) are in `target`'s array.
        llvm::Value* wlo = smax(zero, b_.CreateNeg(pos));
        llvm::Value* whi = smin(b_.getInt64(words), b_.CreateSub(size, pos));
        count            = b_.CreateSelect(b_.CreateICmpSGT(whi, wlo), b_.CreateSub(whi, wlo), zero);
        count            = b_.CreateSelect(any_unknown(slice_base), zero, count);
        lo               = wlo;
        first = b_.CreateAdd(first, b_.CreateMul(b_.CreateAdd(pos, wlo), b_.getInt64(word_leaves)));
    }
    if (llvm::Value* ok = g.valid()) {
        count = b_.CreateSelect(ok, count, zero);
    }
    // A target that names no word is handed leaf 0 of the array, which exists, so no
    // address is formed outside its storage.
    first = b_.CreateSelect(b_.CreateICmpSGT(count, zero), first, zero);

    const auto scratch = [&]() -> llvm::Value* {
        return entry_alloca(slot_ty, "mem.scratch");
    };
    // A never-z memory (always the case for a 2-state element type) stores no z words;
    // a null z_base tells the runtime to skip the z writes. A 4-state readmem target
    // is a z seed in compute_may_z, so it always keeps its z field.
    llvm::Value* no_z = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    MemTarget    t;
    if (flat) {
        llvm::Value* slot = g.pointer_or(
          b_,
          [&](const LValue& lv) -> llvm::Value* {
              return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), first });
          },
          scratch);
        const LValue leaf = slot_lvalue(g, slot);
        t.mm_base         = lv_mm_ptr(leaf);
        t.z_base          = has_z ? lv_z_ptr(leaf) : no_z;
    } else {
        // The ring of a queue is walked by the runtime from its slot 0.
        llvm::Value* slot = data;
        if (!queue) {
            llvm::Value* array = container ? data
                                           : g.pointer_or(
                                               b_, [&](const LValue& lv) -> llvm::Value* { return agg_slot_ptr(lv); },
                                               scratch);
            slot = b_.CreateInBoundsGEP(slot_ty, array, { first });
        }
        t.mm_base = slot;
        t.z_base  = has_z ? b_.CreateStructGEP(slot_ty, slot, 1) : no_z;
    }
    t.stride     = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(slot_ty));
    t.width      = width;
    t.four_state = four_state;
    t.inner      = mem_inner_dims(type);
    t.left       = left_addr;
    t.right      = right_addr;
    t.lo_word    = lo;
    t.word_count = count;
    t.ring_head  = queue ? ring_first : zero;
    t.ring_cap   = queue ? cap : zero;
    t.place      = static_cast<const LValueInfo&>(g);
    t.container  = container;
    return t;
}

// What a bulk load of `t` owes the design once the runtime has written it: wake what
// waits on the memory when the load `changed` a leaf, and mark it for the dump. A load
// has no per-element edge facts, so an array signal's Any-edge readers are woken
// whenever one landed, as the deferred array-NBA commit does (rt_nba_arr_dirty ->
// service_any_waiters). A dynamic array or queue wakes through its container notifier
// (notify_container_mutation), which a waiter on its signal and on a class property
// each hear; so does an array bound to a `ref` argument, whose actual's waiters the
// call's thunk wakes.
void Backend::finish_mem_load(const MemTarget& t, llvm::Value* changed, const char* prefix) {
    if (silent_stores_) {
        return;
    }
    const LValueInfo& p = t.place;
    // The wake buckets and the dump-var index are the owner's: the instance a virtual
    // interface handle names, when the memory was reached through one.
    CtxOverride ov(*this, p.vif_ctx, lvalue_ctx_anchor(p));
    const bool waited_signal = p.is_signal && signal_has_waiters(p.sig);
    const bool wakes = p.ref_wake_fn != nullptr ||
                       ((t.container && p.chg != nullptr) ? p.chg->container : waited_signal);
    if (wakes) {
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* svc  = llvm::BasicBlock::Create(ctx_, std::string(prefix) + ".svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, std::string(prefix) + ".cont", fn);
        b_.CreateCondBr(b_.CreateICmpNE(changed, b_.getInt32(0)), svc, cont);
        b_.SetInsertPoint(svc);
        if (t.container || p.ref_wake_fn != nullptr) {
            notify_container_mutation(p);
        } else {
            service_signal_container_mutation(p.sig);
        }
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
    if (!t.container && p.is_signal) {
        vcd_touch_array(p.sig);   // bulk write: mark the array for the dump
    }
}

// Lower a $fread into a single vector: one element, no addressing. The runtime
// reads ceil(width/8) bytes MSB-first into the destination planes.
void Backend::lower_fread_word(Index i) {
    const Index dst = ir_->payload(i).binary.l;

    const Guarded lv = write_dest(dst);
    if (lv.is_string) {
        throw BackendError{ "$fread into a string object is not supported" };
    }
    if (lv.width != lv.store_width) {
        throw BackendError{ "$fread into a part-select target is not supported" };
    }
    // A never-z target (z elision) passes a null z pointer; a 2-state
    // slot sets flags bit2 so the runtime writes only the iW value word. An
    // invalid target hands the runtime a scratch slot (safe_lvalue).
    const LValue dest = safe_lvalue(lv);
    llvm::Value* mm   = lv_mm_ptr(dest);
    llvm::Value* z =
      dest.has_z ? lv_z_ptr(dest) : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    llvm::Value* fd = cast(must_of(ir_->payload(i).binary.r), 32, true);

    const uint32_t four   = type_of(dst).four_state ? 1u : 0u;
    const uint32_t flags  = !dest.four_state ? 4u : 0u;
    const ServiceSnapshot old = service_snapshot(lv);
    const auto     no_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    llvm::Value*   r      = b_.CreateCall(fread_fn(),
                                          { fd, mm, z, b_.getInt32(0), b_.getInt32(dest.store_width),
                                            b_.getInt32(four), no_ptr, b_.getInt32(0), b_.getInt64(0),
                                            b_.getInt64(0), b_.getInt64(0), b_.getInt64(1), b_.getInt64(0),
                                            b_.getInt64(0), b_.getInt64(0),
                                            b_.getInt64(0), b_.getInt32(flags), no_ptr, b_.getInt64(0) });
    service_after_write(lv, old);
    set_value(i, two_val(r));
}

// Lower a $fread into a memory: reuse the $readmem geometry (plane bases, stride,
// the highest dimension's declared bounds and the dimensions below it) with optional
// start-address / word-count operands, and read binary bytes MSB-first per leaf.
void Backend::lower_fread_mem(Index i) {
    const Index fdi  = ir_->payload(i).op_extra.op;
    const auto  info = ir_->extra_data<FreadMemInfo>(ir_->payload(i).op_extra.extra);
    const MemTarget t = resolve_mem_target(
      info.target, info.type, info.base_type, info.slice, info.slice_base, info.slice_end, "$fread");

    llvm::Value*   fd     = cast(must_of(fdi), 32, true);
    const auto     i64_of = [&](Index idx) { return cast(must_of(idx), 64, true); };
    llvm::Value*   start  = info.has_start ? i64_of(info.start) : b_.getInt64(0);
    llvm::Value*   cnt    = info.has_count ? i64_of(info.count) : b_.getInt64(0);
    const uint32_t flags  = (info.has_start ? 1u : 0u) | (info.has_count ? 2u : 0u) | (!t.four_state ? 4u : 0u);

    if (t.assoc) {
        // An associative array: its elements are created as the bytes arrive.
        llvm::Value* aflags = b_.getInt32(flags | (t.four_state ? 8u : 0u) | (t.key_signed ? 16u : 0u));
        if (t.assoc_valid != nullptr) {
            aflags = b_.CreateOr(aflags, b_.CreateSelect(t.assoc_valid, b_.getInt32(0), b_.getInt32(32)));
        }
        llvm::Value* r = b_.CreateCall(
          fread_assoc_fn(), { fd, t.assoc_handle, t.assoc_ops, b_.getInt32(t.key_width), b_.getInt32(t.stride),
                              b_.getInt32(t.width), b_.getInt32(t.z_off), t.inner.first,
                              b_.getInt32(t.inner.second), start, cnt, aflags,
                              alloc_site_global(loc_text(i), "associative array"),
                              null_site_global(loc_text(i), NullKind::Class) });
        finish_mem_load(t, r, "fread");
        set_value(i, two_val(r));
        return;
    }

    // Only a start address the memory does not have is reported, so the site is the
    // call and the time is read once, ahead of the runtime call.
    llvm::Value* now = b_.CreateCall(wheel_now_fn(), { wheel_ });
    llvm::Value* r   = b_.CreateCall(
      fread_fn(), { fd, t.mm_base, t.z_base, b_.getInt32(t.stride), b_.getInt32(t.width),
                    b_.getInt32(t.four_state ? 1u : 0u), t.inner.first, b_.getInt32(t.inner.second), t.left,
                    t.right, t.lo_word, t.word_count, t.ring_head, t.ring_cap, start, cnt, b_.getInt32(flags),
                    null_site_global(loc_text(i), NullKind::Class), now });
    finish_mem_load(t, r, "fread");
    set_value(i, two_val(r));
}

// Lower a $readmemh / $readmemb: hand the runtime the filename bytes (spilled
// to a stack buffer, or read from the rt_string handle for a string-typed
// filename) plus the destination array's plane bases and geometry
// (resolve_mem_target). The runtime opens the file and writes leaf planes straight
// into `state`. The mm and z words of leaf i sit at the plane bases + i*stride (one
// slot struct per leaf), so the two region bases plus the stride address every leaf.
void Backend::lower_readmem(Index i) {
    const auto      info  = ir_->extra_data<ReadMemInfo>(ir_->payload(i).op_extra.extra);
    const Index     fname = ir_->payload(i).op_extra.op;
    const MemTarget t = resolve_mem_target(
      info.target, info.type, info.base_type, info.slice, info.slice_base, info.slice_end, "$readmem");

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

    const auto   i64_of = [&](Index idx) { return cast(must_of(idx), 64, true); };
    llvm::Value* start  = info.has_start ? i64_of(info.start) : b_.getInt64(0);
    llvm::Value* finish = info.has_finish ? i64_of(info.finish) : b_.getInt64(0);

    const uint32_t flags = (info.hex ? 1u : 0u) | (info.has_start ? 2u : 0u) |
                           (info.has_finish ? 4u : 0u) | (t.four_state ? 8u : 0u) |
                           (fname_is_str ? 16u : 0u) | (!t.four_state ? 32u : 0u) |
                           (info.slice != kMemSliceNone ? 64u : 0u);

    if (t.assoc) {
        // An associative array: its elements are created as the numbers arrive.
        llvm::Value* aflags = b_.getInt32(flags | (t.key_signed ? 128u : 0u));
        if (t.assoc_valid != nullptr) {
            aflags = b_.CreateOr(aflags, b_.CreateSelect(t.assoc_valid, b_.getInt32(0), b_.getInt32(256)));
        }
        llvm::Value* changed = b_.CreateCall(
          readmem_assoc_fn(), { path, path_len, t.assoc_handle, t.assoc_ops, b_.getInt32(t.key_width),
                                b_.getInt32(t.stride), b_.getInt32(t.width), b_.getInt32(t.z_off), t.inner.first,
                                b_.getInt32(t.inner.second), start, finish, aflags,
                                alloc_site_global(loc_text(i), "associative array"),
                                null_site_global(loc_text(i), NullKind::Class) });
        finish_mem_load(t, changed, "readmem");
        return;
    }

    llvm::Value* changed =
      b_.CreateCall(readmem_fn(),
                    { path, path_len, t.mm_base, t.z_base, b_.getInt32(t.stride), b_.getInt32(t.width),
                      t.inner.first, b_.getInt32(t.inner.second), t.left, t.right, t.lo_word, t.word_count,
                      t.ring_head, t.ring_cap, start, finish, b_.getInt32(flags),
                      null_site_global(loc_text(i), NullKind::Class) });
    finish_mem_load(t, changed, "readmem");
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
// the bump has to land on the TARGET's counter. The counter of a group of an
// instance is a field of that instance's record, at the def-local ordinal of its
// front process: the own instance's record when it is the code's own, so the
// code names no counter, else the owner's record global (a hierarchical disable
// of another instance's procedure, which names a symbol of that instance and so
// is not shared). A group of no instance has a global of root, created there so
// either side can be lowered first and still get the same global.
llvm::Value* Backend::defer_gen_ptr_for(uint32_t g) {
    const uint32_t ga = group_unit_anchor_[g];
    if (ga != SignalSlot::no_anchor) {
        Anchor&      a   = anchors_[ga];
        llvm::Value* rec = nullptr;
        if (ga == own_anchor_ && own_ctx_ != nullptr) {
            rec = own_ctx_;
        } else {
            taint("names the deferred-assertion counter of another instance");
            rec = ensure_ctx(a);
        }
        return b_.CreateInBoundsGEP(
          a.ctx_ty, rec, { b_.getInt32(0), b_.getInt32(a.ctx_defer_base + proc_sched_ord_[groups_[g].front()]) });
    }
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

void Backend::emit_defer_gen_bump_at(llvm::Value* gen, uint64_t delta) {
    if (gen == nullptr) {
        return;
    }
    auto* i64 = b_.getInt64Ty();
    b_.CreateStore(b_.CreateAdd(b_.CreateLoad(i64, gen), b_.getInt64(delta)), gen);
}

// The ptr global holding the report generation of the running thread. Created at the
// start of the lowering (Backend::run) in a design that can use it, so that no body
// lowering makes it: root must come out of a verifying re-lowering as it went in.
llvm::Value* Backend::gen_global() {
    if (gen_gv_ == nullptr) {
        auto* null = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
        gen_gv_    = new llvm::GlobalVariable(root_env_->llmod, raw_ptr_ty(), false,
                                              llvm::GlobalValue::InternalLinkage, null, "clotho.gen");
    }
    return sym(gen_gv_);
}

llvm::Value* Backend::load_gen() {
    return b_.CreateLoad(raw_ptr_ty(), gen_global());
}

void Backend::store_gen(llvm::Value* gen) {
    b_.CreateStore(gen, gen_global());
}

// The generation this body republishes after a resume: a process group's own
// counter, or what a task read at its entry. Null for a body that does not need it.
llvm::Value* Backend::body_gen() {
    if (gen_slot_ != nullptr) {
        return b_.CreateLoad(raw_ptr_ty(), gen_slot_);
    }
    if (cur_fn_ == nullptr && cur_defer_group_ >= 0) {
        return defer_gen_ptr();
    }
    return nullptr;
}

bool Backend::fn_needs_gen(FunctionId id) const {
    const auto k = static_cast<uint32_t>(id);
    return k < facts_.fn_gen.size() && facts_.fn_gen[k] != 0;
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
