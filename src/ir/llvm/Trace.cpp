// The scheduler trace's emission (docs/design/scheduler-trace.md): the calls into the
// runtime's checker (runtime/Trace.h) that report each spawn, park, wake, dispatch, resume
// and end of an activation. Planted only in a build with CLOTHO_EXTRA_ASSERTS, when the
// config asks for the check (SimulationConfig::trace_check); otherwise nothing is emitted
// and the code the backend generates is what it was without the trace.

#include "ir/llvm/Backend.h"

#include "runtime/Trace.h"

namespace clotho::ir::backend {

// The runtime's edge numbers are ir::Edge's, so a list entry names its edge by it.
static_assert(RT_TRACE_POS == static_cast<uint32_t>(Edge::Positive));
static_assert(RT_TRACE_NEG == static_cast<uint32_t>(Edge::Negative));
static_assert(RT_TRACE_ANY == static_cast<uint32_t>(Edge::Any));

bool Backend::trace_on() const {
#ifdef CLOTHO_EXTRA_ASSERTS
    return config_.trace_check;
#else
    return false;
#endif
}

// Where the instruction being lowered is. Without a resolver (the tests) a location is
// "<unknown>", which would make every control of one kind the same one, so the body and the
// instruction stand in for it.
std::string Backend::trace_here() {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return {};
    }
    if (!resolve_) {
        // Twin instances lower a function under names that differ by LLVM's ".N" suffix.
        std::string name = fn_ != nullptr ? fn_->getName().str() : std::string("<unknown>");
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot + 1 < name.size() &&
            name.find_first_not_of("0123456789", dot + 1) == std::string::npos) {
            name.erase(dot);
        }
        return name + "#" + std::to_string(static_cast<uint32_t>(cur_inst_));
    }
    return loc_text(cur_inst_).where;
#else
    return {};
#endif
}

std::string Backend::trace_where(const TracePark& park) {
    return park.where != nullptr ? park.where->where : trace_here();
}

SourceText Backend::trace_process_text(const Process& proc) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (trace_on() && resolve_ && proc.span.valid()) {
        return resolve_(proc.span);
    }
#endif
    (void)proc;
    return SourceText{ .where = "<unknown>" };
}

// A NUL-terminated string constant of module `m`, once per text.
llvm::Constant* Backend::trace_string(llvm::Module& m, const std::string& text) {
#ifdef CLOTHO_EXTRA_ASSERTS
    const auto key = std::make_pair(static_cast<const llvm::Module*>(&m), text);
    if (const auto it = trace_strings_.find(key); it != trace_strings_.end()) {
        return it->second;
    }
    llvm::Constant* init = llvm::ConstantDataArray::getString(ctx_, text);
    auto*           gv   = new llvm::GlobalVariable(m, init->getType(), /*isConstant=*/true,
                                                    llvm::GlobalValue::PrivateLinkage, init, "clotho.trace.str");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    trace_strings_.emplace(key, gv);
    return gv;
#else
    (void)m;
    (void)text;
    return nullptr;
#endif
}

llvm::Constant* Backend::trace_control(llvm::Module& m, uint32_t kind, const std::string& where) {
#ifdef CLOTHO_EXTRA_ASSERTS
    const auto key = std::make_tuple(static_cast<const llvm::Module*>(&m), kind, where);
    if (const auto it = trace_controls_.find(key); it != trace_controls_.end()) {
        return it->second;
    }
    auto* const ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* const i32  = llvm::Type::getInt32Ty(ctx_);
    auto* const ty   = llvm::StructType::get(ctx_, { ptr, i32 });
    auto* const init = llvm::ConstantStruct::get(ty, { trace_string(m, where), llvm::ConstantInt::get(i32, kind) });
    auto*       gv   = new llvm::GlobalVariable(m, ty, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init,
                                                "clotho.trace.ctl");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    trace_controls_.emplace(key, gv);
    return gv;
#else
    (void)m;
    (void)kind;
    (void)where;
    return nullptr;
#endif
}

// A signal's name carries its instance path, which shared code must not bake, so the name is the
// part after the last dot; a signal the body reaches through a binding has none.
llvm::Constant* Backend::trace_signal_name(Signal sig) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (binding_of(slot_of(sig)) != no_bind) {
        return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    }
    const std::string& full = mod_.signal(sig).name;
    const size_t       dot  = full.rfind('.');
    return trace_string(*b_.GetInsertBlock()->getModule(), dot == std::string::npos ? full : full.substr(dot + 1));
#else
    (void)sig;
    return nullptr;
#endif
}

// One entry of a list, from the IR fact lower_wait_edges registers by: the storage of a signal
// in its own instance (through the virtual interface handle where the wait names a member
// reached through one), or the object an event control names now.
Backend::TraceEntry Backend::trace_entry(const WaitEdge& e) {
#ifdef CLOTHO_EXTRA_ASSERTS
    auto* const none = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    if (e.inst != kNoWaitHandle) {
        llvm::Value* ctx = b_.CreateIntToPtr(must_of(e.inst), raw_ptr_ty());
        CtxOverride  ov(*this, ctx, slot_of(e.signal).anchor);
        return { storage_ptr(slot_of(e.signal)), trace_signal_name(e.signal), e.edge };
    }
    if (e.handle != kNoWaitHandle) {
        return { event_obj_of(must_of(e.handle)), none, RT_TRACE_OBJ };
    }
    if (mod_.signal(e.signal).is_event) {
        return { event_obj_for(e.signal), trace_signal_name(e.signal), RT_TRACE_OBJ };
    }
    return { storage_ptr(slot_of(e.signal)), trace_signal_name(e.signal), e.edge };
#else
    (void)e;
    return {};
#endif
}

void Backend::emit_trace_spawn(llvm::IRBuilder<>& b, llvm::Value* a, uint32_t origin, const std::string& where,
                               llvm::Value* parent) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr  = llvm::PointerType::getUnqual(ctx_);
    llvm::Value* org = trace_control(*b.GetInsertBlock()->getModule(), origin, where);
    b.CreateCall(rt_fn("rt_trace_spawn", b.getVoidTy(), { ptr, ptr, ptr }),
                 { a, org, parent != nullptr ? parent : llvm::ConstantPointerNull::get(ptr) });
#else
    (void)b;
    (void)a;
    (void)origin;
    (void)where;
    (void)parent;
#endif
}

llvm::Constant* Backend::emit_trace_park(const TracePark& park) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return nullptr;
    }
    auto* const     ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const     i64 = b_.getInt64Ty();
    llvm::Constant* ctl = trace_control(*b_.GetInsertBlock()->getModule(), park.kind, trace_where(park));
    for (const WaitEdge& e : park.edges) {
        const TraceEntry t = trace_entry(e);
        b_.CreateCall(rt_fn("rt_trace_waits_for", b_.getVoidTy(), { ptr, ptr, ptr, i64 }),
                      { hdl_, t.x, t.name, b_.getInt64(t.edge) });
    }
    b_.CreateCall(rt_fn("rt_trace_park", b_.getVoidTy(), { ptr, ptr, i64 }),
                  { hdl_, ctl, park.delay != nullptr ? park.delay : b_.getInt64(0) });
    return ctl;
#else
    (void)park;
    return nullptr;
#endif
}

void Backend::emit_trace_wake(llvm::IRBuilder<>& b, llvm::Value* a, const TraceCause& why) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const  ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const  i32 = b.getInt32Ty();
    llvm::Value* x   = llvm::ConstantPointerNull::get(ptr);
    if (why.sig.has_value()) {
        // The storage the writer reaches, as the waiter's list names it.
        assert(&b == &b_ && "a signal cause is emitted by the body's own builder");
        x = storage_ptr(slot_of(*why.sig));
    } else if (why.obj != nullptr) {
        x = why.obj;
    }
    b.CreateCall(rt_fn("rt_trace_wake", b.getVoidTy(), { ptr, i32, ptr, i32 }),
                 { a, b.getInt32(why.kind), x, b.getInt32(why.fact) });
#else
    (void)b;
    (void)a;
    (void)why;
#endif
}

llvm::Value* Backend::emit_trace_dispatch(llvm::IRBuilder<>& b, llvm::Value* a, uint32_t how) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return nullptr;
    }
    auto* const  ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const  i32 = b.getInt32Ty();
    llvm::Value* go  = b.CreateCall(rt_fn("rt_trace_dispatch", i32, { ptr, i32 }), { a, b.getInt32(how) });
    return b.CreateICmpNE(go, b.getInt32(0));
#else
    (void)b;
    (void)a;
    (void)how;
    return nullptr;
#endif
}

void Backend::emit_trace_resume(llvm::Value* a, llvm::Constant* control) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (control == nullptr) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_resume", b_.getVoidTy(), { ptr, ptr }), { a, control });
#else
    (void)a;
    (void)control;
#endif
}

void Backend::emit_trace_end(llvm::Value* a) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    b_.CreateCall(rt_fn("rt_trace_end", b_.getVoidTy(), { llvm::PointerType::getUnqual(ctx_) }), { a });
#else
    (void)a;
#endif
}

// A thunk group is an activation named by the address of its wake pair with bit 0 set, which
// is also what the seed hands out; its entry returns to its leading control.
void Backend::emit_trace_thunk_park(llvm::Value* wake, const std::string& where) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const  ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const  i64 = b_.getInt64Ty();
    llvm::Value* a   = b_.CreateIntToPtr(b_.CreateOr(b_.CreatePtrToInt(wake, i64), b_.getInt64(1)), ptr);
    llvm::Value* ctl = trace_control(*b_.GetInsertBlock()->getModule(), RT_TRACE_CTL_STATIC, where);
    b_.CreateCall(rt_fn("rt_trace_park", b_.getVoidTy(), { ptr, ptr, i64 }), { a, ctl, b_.getInt64(0) });
#else
    (void)wake;
    (void)where;
#endif
}

void Backend::emit_trace_seed_row(llvm::Value* h, llvm::Value* origin, llvm::Value* ctl, llvm::Value* list,
                                  llvm::Value* n, llvm::Value* parked_now) {
#ifdef CLOTHO_EXTRA_ASSERTS
    auto* const ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* const i32  = b_.getInt32Ty();
    auto* const i64  = b_.getInt64Ty();
    b_.CreateCall(rt_fn("rt_trace_spawn", b_.getVoidTy(), { ptr, ptr, ptr }),
                  { h, origin, llvm::ConstantPointerNull::get(ptr) });
    b_.CreateCall(rt_fn("rt_trace_subscribe", b_.getVoidTy(), { ptr, ptr, i32 }), { h, list, n });
    llvm::Function* fn   = b_.GetInsertBlock()->getParent();
    auto* const     park = llvm::BasicBlock::Create(ctx_, "trace.park", fn);
    auto* const     done = llvm::BasicBlock::Create(ctx_, "trace.parked", fn);
    b_.CreateCondBr(parked_now, park, done);
    b_.SetInsertPoint(park);
    b_.CreateCall(rt_fn("rt_trace_park", b_.getVoidTy(), { ptr, ptr, i64 }), { h, ctl, b_.getInt64(0) });
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
#else
    (void)h;
    (void)origin;
    (void)ctl;
    (void)list;
    (void)n;
    (void)parked_now;
#endif
}

void Backend::emit_trace_subscribe_event(llvm::Value* h, llvm::Value* obj, llvm::Value* name) {
#ifdef CLOTHO_EXTRA_ASSERTS
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_subscribe_event", b_.getVoidTy(), { ptr, ptr, ptr }), { h, obj, name });
#else
    (void)h;
    (void)obj;
    (void)name;
#endif
}

// A nonblocking statement's constant record (rt_trace_nba_site), once per statement in module `m`.
llvm::Constant* Backend::trace_nba_site(llvm::Module& m, const TraceNba& t, uint32_t path) {
#ifdef CLOTHO_EXTRA_ASSERTS
    const std::string where = trace_here();
    const uint32_t    flags = path | t.flags;
    const auto        key   = std::make_tuple(static_cast<const llvm::Module*>(&m), where,
                                              static_cast<const llvm::Constant*>(t.name), flags, t.bits0, t.bits1);
    if (const auto it = trace_nba_sites_.find(key); it != trace_nba_sites_.end()) {
        return it->second;
    }
    auto* const     ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* const     i32  = llvm::Type::getInt32Ty(ctx_);
    auto* const     ty   = llvm::StructType::get(ctx_, { ptr, ptr, i32, i32, i32 });
    llvm::Constant* name = t.name != nullptr ? t.name : llvm::ConstantPointerNull::get(ptr);
    auto* const     init = llvm::ConstantStruct::get(
      ty, { trace_string(m, where), name, llvm::ConstantInt::get(i32, flags), llvm::ConstantInt::get(i32, t.bits0),
            llvm::ConstantInt::get(i32, t.bits1) });
    auto* gv = new llvm::GlobalVariable(m, ty, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init, "clotho.trace.nba");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    trace_nba_sites_.emplace(key, gv);
    return gv;
#else
    (void)m;
    (void)t;
    (void)path;
    return nullptr;
#endif
}

// The update the store `lv <= rhs` creates, from the lvalue and the value in the way the staging
// code computes the bits it writes (merge_sel: the value placed at the select's offset, under the
// store mask), whichever path then stages it. The planes are spilled to word buffers, as the wide
// selects' are (sel_plane_buf).
Backend::TraceNba Backend::trace_nba_of(const LValue& lv, Plane rhs, bool gated) {
    TraceNba t;
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return t;
    }
    t.on    = true;
    t.flags = gated ? RT_TRACE_NBA_GATED : 0;
    t.name  = lv.is_signal ? trace_signal_name(lv.sig) : nullptr;
    t.dst0  = lv_mm_ptr(lv);
    // The widest plane of an update that the trace compares. A wider one costs two word buffers of
    // stack in every frame that has the site and a run of wide operations at it, and the vectors
    // that wide are memories and buses whose NBAs the narrower ones exercise just the same.
    constexpr uint32_t max_bits = 1024;
    if (lv.wide_dyn || (lv.four_state ? 2 * lv.store_width : lv.store_width) > max_bits) {
        // A runtime-offset select of a very wide vector stages through rt_wide_ins, which builds no
        // mask the trace could read, and a plane this wide is not spilled: the target's bytes are not
        // compared.
        t.flags |= RT_TRACE_NBA_OPAQUE;
        return t;
    }
    if (rhs.fp != nullptr) {
        rhs = real_to_planes(fp_of(rhs));
    }
    const uint32_t W    = lv.store_width;
    llvm::Value*   mask = selects_whole(lv) ? nullptr : store_mask(lv, W);
    const auto     spill = [&](llvm::Value* v, uint32_t bits) -> llvm::Value* {
        llvm::AllocaInst* buf = sel_plane_buf(bits, "nba.img");
        b_.CreateStore(cast(v, sel_buf_bits(bits), false), buf);
        return buf;
    };
    if (!lv.four_state) {
        t.bits0 = W;
        t.val0  = spill(place_val(lv, must_of(rhs, false), W), W);
        t.mask0 = mask != nullptr ? spill(mask, W) : nullptr;
        return t;
    }
    llvm::Value* must = place_val(lv, must_of(rhs, true), W);
    llvm::Value* may  = place_val(lv, may_of(rhs, true), W);
    t.bits0           = 2 * W;
    t.val0            = spill(packed(must, may, zero(W)).mm, 2 * W);
    t.mask0           = mask != nullptr ? spill(packed(mask, mask, zero(W)).mm, 2 * W) : nullptr;
    if (lv.has_z) {
        t.dst1  = lv_z_ptr(lv);
        t.bits1 = W;
        t.val1  = spill(place_val(lv, rhs.z != nullptr ? rhs.z : zero(W), W), W);
        t.mask1 = mask != nullptr ? spill(mask, W) : nullptr;
    }
#else
    (void)lv;
    (void)rhs;
    (void)gated;
#endif
    return t;
}

void Backend::emit_trace_nba(llvm::Value* rec, const TraceNba& t, uint32_t path) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!t.on) {
        return;
    }
    auto* const     ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* const     none = llvm::ConstantPointerNull::get(ptr);
    llvm::Constant* site = trace_nba_site(*b_.GetInsertBlock()->getModule(), t, path);
    const auto      or_none = [&](llvm::Value* v) { return v != nullptr ? v : static_cast<llvm::Value*>(none); };
    if (rec == nullptr) {
        b_.CreateCall(rt_fn("rt_trace_nba_create", b_.getVoidTy(), { ptr, ptr, ptr, ptr, ptr, ptr, ptr }),
                      { site, t.dst0, or_none(t.dst1), or_none(t.val0), or_none(t.mask0), or_none(t.val1), or_none(t.mask1) });
        return;
    }
    b_.CreateCall(rt_fn("rt_trace_nba_stage", b_.getVoidTy(), { ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr }),
                  { rec, site, t.dst0, or_none(t.dst1), or_none(t.val0), or_none(t.mask0), or_none(t.val1), or_none(t.mask1) });
#else
    (void)rec;
    (void)t;
    (void)path;
#endif
}

void Backend::emit_trace_region(uint32_t region) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const i32 = b_.getInt32Ty();
    b_.CreateCall(rt_fn("rt_trace_region", b_.getVoidTy(), { i32 }), { b_.getInt32(region) });
#else
    (void)region;
#endif
}

void Backend::emit_trace_nba_pass(bool begin, uint32_t region) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const i32 = b_.getInt32Ty();
    b_.CreateCall(rt_fn("rt_trace_nba_pass", b_.getVoidTy(), { i32, i32 }), { b_.getInt32(begin ? 1 : 0), b_.getInt32(region) });
#else
    (void)begin;
    (void)region;
#endif
}

void Backend::emit_trace_pend(llvm::Value* obj) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    b_.CreateCall(rt_fn("rt_trace_pend", b_.getVoidTy(), { llvm::PointerType::getUnqual(ctx_) }), { obj });
#else
    (void)obj;
#endif
}

void Backend::emit_trace_trigger(llvm::Value* obj, uint32_t kind) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    b_.CreateCall(rt_fn("rt_trace_trigger", b_.getVoidTy(), { llvm::PointerType::getUnqual(ctx_), b_.getInt32Ty() }),
                  { obj, b_.getInt32(kind) });
#else
    (void)obj;
    (void)kind;
#endif
}

} // namespace clotho::ir::backend
