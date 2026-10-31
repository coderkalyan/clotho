// Runtime checks: bounds, null handles, reference epochs, tagged unions and allocation sites.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// The index value a report shows: the design's own, widened to i64 with the
// index expression's own signedness. The bounds checks themselves lean on
// unsigned wrap to catch a negative index in one compare; a report that
// reused that value would print 4294967295 where the design wrote -1.
llvm::Value* Backend::bounds_index(Index idx_expr) {
    return cast(must_of(idx_expr), 64, true);
}

// One constant rt_bounds_site per instrumented access. Everything the report
// knows at compile time lives here instead of in the argument list, which
// keeps the call to the values that are genuinely dynamic. `left`/`right`
// are ignored by the runtime when the check passes an extent instead.
//
// `idx` locates the index expression and drives the message's location line
// and caret; `acc` locates the expression being indexed, underlined beside
// it. Pass the same SourceText for both where there is no separate index
// (an empty-queue pop).
llvm::Constant* Backend::bounds_site_global(const SourceText&  idx,
                                            const SourceText&  acc,
                                            const std::string& name,
                                            BoundsWhat         what,
                                            int32_t            left,
                                            int32_t            right,
                                            int32_t            width) {
    auto*       i32 = b_.getInt32Ty();
    auto* const rec = bounds_site_ty();
    // The snippet is the index's line, so an access position on some other
    // line has no column to underline there.
    const bool  same = acc.line_no == idx.line_no;
    auto*       init =
      llvm::ConstantStruct::get(rec, { b_.CreateGlobalString(idx.where, "bounds.at"),
                                       b_.CreateGlobalString(idx.line, "bounds.src"),
                                       b_.CreateGlobalString(name, "bounds.of"),
                                       llvm::ConstantInt::get(i32, idx.line_no),
                                       llvm::ConstantInt::get(i32, idx.column),
                                       llvm::ConstantInt::get(i32, idx.end_column),
                                       llvm::ConstantInt::get(i32, same ? acc.line_no : 0),
                                       llvm::ConstantInt::get(i32, same ? acc.column : 0),
                                       llvm::ConstantInt::get(i32, same ? acc.end_column : 0),
                                       llvm::ConstantInt::get(i32, static_cast<int32_t>(what)),
                                       llvm::ConstantInt::get(i32, static_cast<uint32_t>(left)),
                                       llvm::ConstantInt::get(i32, static_cast<uint32_t>(right)),
                                       llvm::ConstantInt::get(i32, static_cast<uint32_t>(width)) });
    return new llvm::GlobalVariable(env_->llmod, rec, /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init, "clotho.bounds.site");
}

// The name to report for whatever `lv` addresses. A local / formal has none
// in the backend (frame slots carry no name), so it says what it is.
std::string Backend::bounds_name(const LValue& lv) const {
    return lv.is_signal ? mod_.signal(lv.sig).name : std::string("<local>");
}

// Record an array-element check on `target` (the lvalue the select produces)
// for later reporting. `named` is the lvalue that knows what was indexed --
// the same as `target` for an in-place fixed-array select, the base lvalue
// for a dynamic array or queue whose element lvalue is built fresh. Pass
// `extent` for a runtime-sized container, or null plus the declared
// `left`/`right` for one bounded by its declaration.
//
// Reported at the access rather than here: a select is not yet a read or a
// write, and the direction is half the report.
void Backend::add_elem_bounds_check(LValue&       target,
                                    Index         at,
                                    const LValue& named,
                                    Index         idx_expr,
                                    llvm::Value*  inrange,
                                    llvm::Value*  unknown,
                                    llvm::Value*  extent,
                                    int32_t       left,
                                    int32_t       right) {
    if (!sanitizer_on(config_.sanitizers, Sanitizer::Bounds)) {
        return;
    }
    target.bounds_checks.push_back(BoundsCheck{
      .inrange = inrange,
      .index   = bounds_index(idx_expr),
      .unknown = unknown,
      .extent  = extent,
      .site    = bounds_site_global(loc_text(idx_expr), loc_text(at), bounds_name(named),
                                    BoundsWhat::Element, left, right),
    });
}

// Record a packed bit / part-select check (LRM 11.5.1). Unlike an array
// element, the index the backend sees is the frontend's LSB-relative bit
// position (lower_packed_offset normalized it against the declared range),
// so the report names the container by WIDTH rather than by a declared
// [msb:lsb] it cannot reconstruct. For the usual `[N-1:0]` declaration the
// two coincide.
// TODO(xsan): carry the declared range on the select records so a
// `logic [15:8]` vector reports the index the design actually wrote.
void Backend::add_select_bounds_check(LValue&              target,
                                      Index                at,
                                      Index                off_expr,
                                      const SelectVerdict& v,
                                      BoundsWhat           what,
                                      uint32_t             container_width,
                                      uint32_t             sel_width) {
    if (!sanitizer_on(config_.sanitizers, Sanitizer::Bounds)) {
        return;
    }
    target.bounds_checks.push_back(BoundsCheck{
      .inrange = v.fully,
      .index   = bounds_index(off_expr),
      .unknown = any_unknown(off_expr),
      .extent  = b_.getInt64(container_width),
      .partial = v.partial,
      .site    = bounds_site_global(loc_text(off_expr), loc_text(at), bounds_name(target), what,
                                    0, 0, static_cast<int32_t>(sel_width)),
    });
}

// A string character index (LRM 6.16.2 / 6.16.3). The bound lives only
// inside rt_string_getc / rt_string_putc, which silently return 0 / do
// nothing, so unlike every other site this one materializes its own
// predicate: a length load and two compares, emitted only under the
// sanitizer. Reported on the spot -- getc is a read and putc a write, with
// no lvalue to carry the check to.
void Backend::emit_string_bounds_check(Index              at,
                                       llvm::Value*       str,
                                       Index              idx_expr,
                                       const std::string& name,
                                       bool               is_write) {
    if (!sanitizer_on(config_.sanitizers, Sanitizer::Bounds)) {
        return;
    }
    llvm::Value* len   = b_.CreateCall(str_len_fn(), { str });
    llvm::Value* index = bounds_index(idx_expr);
    emit_bounds_report(BoundsCheck{
                         .inrange = b_.CreateAnd(b_.CreateICmpSGE(index, b_.getInt64(0)),
                                                 b_.CreateICmpSLT(index, len)),
                         .index   = index,
                         .unknown = any_unknown(idx_expr),
                         .extent  = len,
                         .site = bounds_site_global(loc_text(idx_expr), loc_text(at), name,
                                                    BoundsWhat::Char, 0, 0),
                       },
                       is_write);
}

// The index argument of a queue method (LRM 7.10.2.2 - 7.10.2.5). Same
// shape as a string index: the bound is enforced inside the runtime, so the
// check loads the queue's length itself. `limit` is the largest legal index
// -- `len` for insert, whose index may equal the size (an append), `len - 1`
// for delete.
void Backend::emit_queue_bounds_check(Index         at,
                                      llvm::Value*  handle,
                                      Index         idx_expr,
                                      const LValue& named,
                                      bool          allow_append) {
    if (!sanitizer_on(config_.sanitizers, Sanitizer::Bounds)) {
        return;
    }
    llvm::Value* len   = b_.CreateLoad(b_.getInt64Ty(),
                                       b_.CreateStructGEP(queue_slot_ty(), handle, 2));
    llvm::Value* index = bounds_index(idx_expr);
    llvm::Value* hi    = allow_append ? b_.CreateICmpSLE(index, len) : b_.CreateICmpSLT(index, len);
    emit_bounds_report(BoundsCheck{
                         .inrange = b_.CreateAnd(b_.CreateICmpSGE(index, b_.getInt64(0)), hi),
                         .index   = index,
                         .unknown = any_unknown(idx_expr),
                         .extent  = len,
                         .site = bounds_site_global(loc_text(idx_expr), loc_text(at),
                                                    bounds_name(named), BoundsWhat::QueueOp, 0, 0),
                       },
                       /*is_write=*/true);
}

// pop_front / pop_back on an empty queue (LRM 7.10.2.4 - 7.10.2.5: the
// result is the Table 7-1 element default, the queue is untouched, and the
// clause explicitly permits a warning). No index to report -- the finding
// is that the queue had nothing in it.
void Backend::emit_empty_pop_bounds_check(Index at, llvm::Value* handle, const LValue& named) {
    if (!sanitizer_on(config_.sanitizers, Sanitizer::Bounds)) {
        return;
    }
    llvm::Value* len = b_.CreateLoad(b_.getInt64Ty(),
                                     b_.CreateStructGEP(queue_slot_ty(), handle, 2));
    emit_bounds_report(BoundsCheck{
                         .inrange = b_.CreateICmpNE(len, b_.getInt64(0)),
                         .index   = b_.getInt64(0),
                         .extent  = len,
                         .site    = bounds_site_global(loc_text(at), loc_text(at),
                                                       bounds_name(named), BoundsWhat::EmptyPop,
                                                       0, 0),
                       },
                       /*is_write=*/false);
}

// LRM 7.8.6 (P4b): an index that is or contains x or z is an invalid
// associative-array key. The read-yields-default / write-is-dropped
// behaviour is already implemented by the `kvalid` gate this wraps
// (assoc_elem_lvalue); only the WARNING the clause also asks for is new
// here, and only under --sanitize=bounds -- Clotho's existing, deliberate
// performance/compliance tradeoff for this whole category of runtime
// report (matches RT_BOUNDS_EMPTY_POP and friends). `kvalid` null means
// the key type is 2-state (or has no x/z concept at all -- string, queue,
// dynamic array, associative array): statically always valid, nothing to
// report.
void Backend::emit_assoc_key_bounds_check(llvm::Value* kvalid, const LValue& lv, bool for_write) {
    if (!sanitizer_on(config_.sanitizers, Sanitizer::Bounds) || kvalid == nullptr) {
        return;
    }
    emit_bounds_report(BoundsCheck{
                         .inrange = kvalid,
                         .index   = b_.getInt64(0),
                         .unknown = b_.getInt1(true),
                         .extent  = b_.getInt64(0),
                         .site    = bounds_site_global(loc_text(lv.assoc_key_arg), loc_text(lv.assoc_key_arg),
                                                       bounds_name(lv), BoundsWhat::AssocKey, 0, 0),
                       },
                       for_write);
}

// One --sanitize=bounds check: a branch on the in-range predicate the access
// itself uses, into a cold block that reports and falls through. Emitted
// BEFORE the access, so the report fires whether or not the LRM then lets
// the access proceed (an out-of-range read still yields the element
// default, an out-of-range write is still dropped).
void Backend::emit_bounds_report(const BoundsCheck& bc, bool is_write) {
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* bad  = llvm::BasicBlock::Create(ctx_, "bounds.bad", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "bounds.ok", fn);
    b_.CreateCondBr(bc.inrange, cont, bad);

    b_.SetInsertPoint(bad);
    auto* const  i32   = b_.getInt32Ty();
    // bit 0 = write, bit 1 = the index carried x/z, bit 2 = a part-select
    // that kept its in-range bits.
    llvm::Value* flags = b_.getInt32(is_write ? 1 : 0);
    if (bc.unknown != nullptr) {
        flags = b_.CreateOr(flags, b_.CreateShl(b_.CreateZExt(bc.unknown, i32), b_.getInt32(1)));
    }
    if (bc.partial != nullptr) {
        flags = b_.CreateOr(flags, b_.CreateShl(b_.CreateZExt(bc.partial, i32), b_.getInt32(2)));
    }
    // -1 marks "bounded by the declared range in the site record".
    llvm::Value* extent = bc.extent != nullptr ? bc.extent : b_.getInt64(-1);
    llvm::Value* now    = b_.CreateCall(wheel_now_fn(), { wheel_ });
    b_.CreateCall(bounds_report_fn(), { bc.site, bc.index, extent, flags, now });
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
}

// Drain every check `lv` accumulated on the way to this access. No check
// exists unless the sanitizer is on, so this is a no-op otherwise and the
// emitted code is unchanged.
void Backend::emit_bounds_reports(const LValue& lv, bool is_write) {
    for (const BoundsCheck& bc : lv.bounds_checks) {
        emit_bounds_report(bc, is_write);
    }
}

// One constant rt_null_site per guarded dereference. Everything the fatal
// knows is compile-time, so only the simulation time travels as an argument.
llvm::Constant* Backend::null_site_global(const SourceText& at, NullKind kind) {
    auto* const i32  = b_.getInt32Ty();
    auto* const rec  = null_site_ty();
    auto*       init = llvm::ConstantStruct::get(
      rec, { b_.CreateGlobalString(at.where, "null.at"),
             b_.CreateGlobalString(at.line, "null.src"),
             llvm::ConstantInt::get(i32, at.line_no), llvm::ConstantInt::get(i32, at.column),
             llvm::ConstantInt::get(i32, at.end_column),
             llvm::ConstantInt::get(i32, static_cast<int32_t>(kind)) });
    return new llvm::GlobalVariable(env_->llmod, rec, /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init,
                                    "clotho.null.site");
}

// Guard a class-handle dereference against null (LRM 8.4). The handle is raw
// pointer bits, so an unguarded inttoptr hands the property GEP address zero
// and the design dies in a segfault that names nothing; this turns it into a
// located fatal instead. Cost on the hot path is one compare against zero and
// a not-taken branch, weighted so the cold block sinks out of line; where the
// same handle value reaches two derefs unchanged, CSE folds the second
// compare away.
void Backend::emit_null_handle_check(llvm::Value* handle, Index at, NullKind kind) {
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* bad  = llvm::BasicBlock::Create(ctx_, "null.deref", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "deref.ok", fn);
    auto* br   = b_.CreateCondBr(b_.CreateICmpEQ(handle, b_.getInt64(0)), bad, cont);
    br->setMetadata(llvm::LLVMContext::MD_prof,
                    llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

    b_.SetInsertPoint(bad);
    llvm::Value* now  = b_.CreateCall(wheel_now_fn(), { wheel_ });
    auto*        call =
      b_.CreateCall(null_handle_fn(), { null_site_global(loc_text(at), kind), now });
    call->addFnAttr(llvm::Attribute::NoReturn);
    call->addFnAttr(llvm::Attribute::Cold);
    b_.CreateUnreachable();

    b_.SetInsertPoint(cont);
}

// The zero word a `ref` through a STABLE place points its epoch at: a module
// signal, a frame local, a fixed unpacked array element or a class property
// never moves, so there is nothing to compare against.
//
// Internal and constant rather than a runtime export, so that where the
// optimizer can see the caller it constant-folds the callee's load to 0, the
// compare to true, and deletes the check outright. A ref through such a place
// therefore costs nothing once inlined or argument-specialized.
llvm::Constant* Backend::no_epoch_global() {
    if (env_->no_epoch == nullptr) {
        env_->no_epoch = new llvm::GlobalVariable(
          env_->llmod, b_.getInt64Ty(), /*isConstant=*/true,
          llvm::GlobalValue::InternalLinkage, b_.getInt64(0), "clotho.ref.noepoch");
    }
    return env_->no_epoch;
}

// The site record for one dereference of a ref formal, built in the CALLEE:
// where the stale access is, and the callee's name for the formal. The
// container's name is not here — the runtime resolves it from the epoch pointer
// (rt_ref_names, §3.3), so no part of the report rides on the call.
llvm::Constant* Backend::ref_site_global(const SourceText& at, std::string_view formal) {
    auto* const i32  = b_.getInt32Ty();
    auto* const rec  = ref_site_ty();
    auto*       init = llvm::ConstantStruct::get(
      rec, { b_.CreateGlobalString(at.where, "ref.at"),
             b_.CreateGlobalString(at.line, "ref.src"),
             llvm::ConstantInt::get(i32, at.line_no), llvm::ConstantInt::get(i32, at.column),
             llvm::ConstantInt::get(i32, at.end_column),
             b_.CreateGlobalString(formal, "ref.formal") });
    return new llvm::GlobalVariable(env_->llmod, rec, /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init,
                                    "clotho.ref.site");
}

// Register the design's epoch-to-name table before any process runs
// (docs/design/data-model.md §3.3). One entry per container signal, keyed by
// the address of its relocation epoch, which is the only handle a staleness
// report holds. Signals only: their handles live in the state global at a fixed
// address, whereas a container in a class body or inside another container's
// buffer has none to key on and is reported unnamed.
//
// Emitted only for a design that can actually produce a report -- one with a
// ref formal carrying an epoch -- so the ordinary design pays nothing.
void Backend::emit_ref_name_table() {
    if (facts_.ref_epoch_formals.empty()) {
        return;
    }
    auto* const                  rec = ref_name_ty();
    std::vector<llvm::Constant*> rows;
    for (uint32_t i = 0; i < mod_.signals().size(); ++i) {
        const auto        sig  = static_cast<Signal>(i);
        const SignalInfo& info = mod_.signal(sig);
        if (!is_handle_type(info.type)) {
            continue;
        }
        auto* const epoch = llvm::dyn_cast<llvm::Constant>(b_.CreateStructGEP(
          handle_slot_ty(info.type), signal_lvalue(sig).base, handle_epoch_field(info.type)));
        // A signal reached through a runtime anchor load has no constant
        // address; it cannot be a table key, and is reported unnamed.
        if (epoch == nullptr) {
            continue;
        }
        rows.push_back(llvm::ConstantStruct::get(
          rec, { epoch, b_.CreateGlobalString(info.name, "ref.container") }));
    }
    if (rows.empty()) {
        return;
    }
    auto* const arr = llvm::ArrayType::get(rec, rows.size());
    auto* const gv =
      new llvm::GlobalVariable(env_->llmod, arr, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage,
                               llvm::ConstantArray::get(arr, rows), "clotho.ref.names");
    b_.CreateCall(rt_fn("rt_ref_names", b_.getVoidTy(), { raw_ptr_ty(), b_.getInt64Ty() }),
                  { gv, b_.getInt64(rows.size()) });
}

// Guard one dereference of a `ref` formal against its actual's container
// having been restructured since the ref was formed (docs/design/data-model.md
// §3). Emitted by local_lvalue, so it covers every read and write through the
// formal, including those that straddle a task's suspend — which is exactly
// when a container can move under a live ref.
//
// The site record is built HERE, in the callee, and names this dereference:
// both of its fields are callee knowledge (the formal's name, and where the
// stale access happens), so no site travels with the call.
//
// Hot path is one load, one compare and a not-taken branch weighted so both
// cold blocks sink out of line; repeated accesses through the same formal
// within a block fold to one check, and a ref through a stable place folds
// away entirely (no_epoch_global). rt_ref_stale may return (warn mode), so
// the stale block rejoins rather than trapping.
void Backend::emit_ref_epoch_check(const RefBinding& rb) {
    if (rb.epochs.empty()) {
        // A stable place, or a fork child's captured formal — bound before any
        // actual exists. Nothing to check.
        return;
    }
    llvm::Constant* const site = ref_site_global(loc_text(cur_inst_), rb.formal);
    // The time each report stamps is read in its own cold block: reading it
    // before the branch would put a wheel call on the hot path.
    const auto report_now = [&] { return b_.CreateCall(wheel_now_fn(), { wheel_ }); };

    // One check per container level, OUTERMOST FIRST, each guarding the next:
    // level n's witness lives in level n-1's buffer, so it is only safe to load
    // once level n-1 has been proven current. A mismatch at any level reports and
    // (in warn mode) rejoins WITHOUT reading the deeper witnesses, whose storage
    // the reported restructuring may have released.
    for (size_t lvl = 0; lvl < rb.epochs.size(); ++lvl) {
        llvm::Value* const ep  = rb.epochs[lvl];
        llvm::Value* const exp = rb.expects[lvl];
        auto* fn      = b_.GetInsertBlock()->getParent();
        auto* bad     = llvm::BasicBlock::Create(ctx_, "ref.moved", fn);
        auto* regress = llvm::BasicBlock::Create(ctx_, "ref.regress", fn);
        auto* stale   = llvm::BasicBlock::Create(ctx_, "ref.stale", fn);
        auto* cont    = llvm::BasicBlock::Create(ctx_, "ref.ok", fn);
        auto* done    = llvm::BasicBlock::Create(ctx_, "ref.done", fn);

        llvm::Value* cur = b_.CreateLoad(b_.getInt64Ty(), ep, "ref.epoch");
        auto*        br  = b_.CreateCondBr(b_.CreateICmpNE(cur, exp), bad, cont);
        br->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

        // Epochs only ever advance, so a current value BELOW the expected one
        // means the epoch plumbing itself is wrong, not the design.
        b_.SetInsertPoint(bad);
        b_.CreateCondBr(b_.CreateICmpULT(cur, exp), regress, stale);

        b_.SetInsertPoint(regress);
        auto* rcall = b_.CreateCall(ref_epoch_regress_fn(), { site, ep, exp, cur, report_now() });
        rcall->addFnAttr(llvm::Attribute::NoReturn);
        rcall->addFnAttr(llvm::Attribute::Cold);
        b_.CreateUnreachable();

        b_.SetInsertPoint(stale);
        b_.CreateCall(ref_stale_fn(), { site, ep, exp, cur, report_now() })
          ->addFnAttr(llvm::Attribute::Cold);
        // Warn mode returns: skip the deeper levels rather than dereference a
        // witness the reported move may have freed.
        b_.CreateBr(done);

        b_.SetInsertPoint(cont);
        b_.CreateBr(done);
        b_.SetInsertPoint(done);
    }
}

// Guard one tagged-union member access against the union's current tag (LRM
// 11.9). Cost on the hot path is one equality compare against the member's tag
// constant (plus, for a 4-state union, one must-vs-may compare that rejects an
// x/z tag) and a not-taken branch, weighted so the cold block sinks out of
// line; where the same tag value reaches two accesses unchanged, CSE folds the
// second compare away.
void Backend::lower_tagged_check(Index i) {
    const auto& p    = ir_->payload(i);
    const auto  info = ir_->extra_data<TaggedCheckInfo>(p.op_extra.extra);
    const Plane pl   = value_of(p.op_extra.op);
    const bool  four = type_of(p.op_extra.op).four_state;
    llvm::Value* const must = must_of(pl, four);
    llvm::Value* const may  = may_of(pl, four);

    // A tag bit that is x or z has must != may, and LRM 11.9 leaves an
    // uninitialized tagged union's tag bits undefined — such a tag selects no
    // member, so it must report rather than compare equal to one.
    llvm::Value* const defined = b_.CreateICmpEQ(must, may);
    llvm::Value* const hit =
      b_.CreateICmpEQ(must, llvm::ConstantInt::get(must->getType(), info.expected));
    llvm::Value* const ok = b_.CreateAnd(defined, hit);

    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* bad  = llvm::BasicBlock::Create(ctx_, "tagged.mismatch", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "tagged.ok", fn);
    auto* br   = b_.CreateCondBr(ok, cont, bad);
    br->setMetadata(llvm::LLVMContext::MD_prof,
                    llvm::MDBuilder(ctx_).createBranchWeights(1u << 20, 1));

    b_.SetInsertPoint(bad);
    llvm::Value* now  = b_.CreateCall(wheel_now_fn(), { wheel_ });
    auto*        call = b_.CreateCall(
      tagged_access_fn(),
      { tagged_site_global(loc_text(i), info),
        b_.CreateZExt(must, b_.getInt64Ty()),
        b_.CreateZExt(b_.CreateNot(defined), b_.getInt32Ty()), now });
    call->addFnAttr(llvm::Attribute::NoReturn);
    call->addFnAttr(llvm::Attribute::Cold);
    b_.CreateUnreachable();

    b_.SetInsertPoint(cont);
}

// One constant rt_tagged_site per guarded access, plus the member-name table
// the report indexes by the tag actually held.
llvm::Constant* Backend::tagged_site_global(const SourceText& at, const TaggedCheckInfo& info) {
    auto* const i32 = b_.getInt32Ty();
    auto* const rec = tagged_site_ty();

    constexpr uint32_t stride = sizeof(TaggedNameEntry) / sizeof(uint32_t);
    std::vector<llvm::Constant*> names, lens;
    for (uint32_t w = static_cast<uint32_t>(info.names.start);
         w < static_cast<uint32_t>(info.names.end); w += stride) {
        const auto e = ir_->extra_data<TaggedNameEntry>(static_cast<ExtraIndex>(w));
        names.push_back(b_.CreateGlobalString(ir_->extra_bytes(e.name_at, e.name_len)));
        lens.push_back(llvm::ConstantInt::get(i32, e.name_len));
    }
    const auto count = static_cast<uint32_t>(names.size());

    auto* const nty = llvm::ArrayType::get(raw_ptr_ty(), count);
    auto* const lty = llvm::ArrayType::get(i32, count);
    auto* const ng  = new llvm::GlobalVariable(env_->llmod, nty, /*isConstant=*/true,
                                               llvm::GlobalValue::PrivateLinkage,
                                               llvm::ConstantArray::get(nty, names),
                                               "clotho.tagged.names");
    auto* const lg  = new llvm::GlobalVariable(env_->llmod, lty, /*isConstant=*/true,
                                               llvm::GlobalValue::PrivateLinkage,
                                               llvm::ConstantArray::get(lty, lens),
                                               "clotho.tagged.lens");

    auto* init = llvm::ConstantStruct::get(
      rec, { b_.CreateGlobalString(at.where, "tagged.at"),
             b_.CreateGlobalString(at.line, "tagged.src"),
             llvm::ConstantInt::get(i32, at.line_no), llvm::ConstantInt::get(i32, at.column),
             llvm::ConstantInt::get(i32, at.end_column),
             b_.CreateGlobalString(ir_->extra_bytes(info.name_at, info.name_len),
                                   "tagged.member"),
             ng, lg, llvm::ConstantInt::get(i32, count),
             llvm::ConstantInt::get(i32, info.is_write) });
    return new llvm::GlobalVariable(env_->llmod, rec, /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init,
                                    "clotho.tagged.site");
}

// One constant rt_alloc_site per allocating call. Everything the fatal knows
// is compile-time, so only the simulation time travels as an argument.
// `what` is the diagnostic noun naming the container being grown.
llvm::Constant* Backend::alloc_site_global(const SourceText& at, std::string_view what) {
    auto* const i32  = b_.getInt32Ty();
    auto* const rec  = alloc_site_ty();
    auto*       init = llvm::ConstantStruct::get(
      rec, { b_.CreateGlobalString(at.where, "alloc.at"),
             b_.CreateGlobalString(at.line, "alloc.src"),
             b_.CreateGlobalString(what, "alloc.what"),
             llvm::ConstantInt::get(i32, at.line_no), llvm::ConstantInt::get(i32, at.column),
             llvm::ConstantInt::get(i32, at.end_column) });
    return new llvm::GlobalVariable(env_->llmod, rec, /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init,
                                    "clotho.alloc.site");
}

} // namespace clotho::ir::backend
