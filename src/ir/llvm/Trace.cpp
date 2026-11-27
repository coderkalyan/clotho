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

// The runtime's override kinds are ir::OverrideKind's, in order, so a mark names the kind of its site by it.
static_assert(RT_TRACE_OVR_ASSIGN_STATE == RT_TRACE_OVR_FORCE_STATE + static_cast<uint32_t>(OverrideKind::AssignState));
static_assert(RT_TRACE_OVR_SITE_STATE == RT_TRACE_OVR_FORCE_STATE + static_cast<uint32_t>(OverrideKind::SiteState));
static_assert(RT_TRACE_OVR_FORCE_APPLY == RT_TRACE_OVR_FORCE_STATE + static_cast<uint32_t>(OverrideKind::ForceApply));
static_assert(RT_TRACE_OVR_ASSIGN_APPLY == RT_TRACE_OVR_FORCE_STATE + static_cast<uint32_t>(OverrideKind::AssignApply));
static_assert(RT_TRACE_OVR_SITE_APPLY == RT_TRACE_OVR_FORCE_STATE + static_cast<uint32_t>(OverrideKind::SiteApply));
static_assert(RT_TRACE_OVR_RESOLVE == RT_TRACE_OVR_FORCE_STATE + static_cast<uint32_t>(OverrideKind::Resolve));

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
    auto* const    none = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    llvm::Module&  m    = *b_.GetInsertBlock()->getModule();
    if (e.inst != kNoWaitHandle) {
        llvm::Value* ctx = b_.CreateIntToPtr(must_of(e.inst), raw_ptr_ty());
        CtxOverride  ov(*this, ctx, slot_of(e.signal).anchor);
        return { storage_ptr(slot_of(e.signal)), trace_signal_name(e.signal), e.edge, trace_shape(m, e.signal) };
    }
    if (e.handle != kNoWaitHandle) {
        return { event_obj_of(must_of(e.handle)), none, RT_TRACE_OBJ, none };
    }
    if (mod_.signal(e.signal).is_event) {
        return { event_obj_for(e.signal), trace_signal_name(e.signal), RT_TRACE_OBJ, none };
    }
    return { storage_ptr(slot_of(e.signal)), trace_signal_name(e.signal), e.edge, trace_shape(m, e.signal) };
#else
    (void)e;
    return {};
#endif
}

llvm::Constant* Backend::trace_shape(llvm::Module& m, Signal sig) {
#ifdef CLOTHO_EXTRA_ASSERTS
    auto* const none = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    if (!trace_on() || mod_.signal(sig).is_event) {
        return none;
    }
    const SignalSlot& s = slot_of(sig);
    if (s.slot_ty == nullptr || s.is_agg || s.is_string || s.is_dynarr || s.is_queue || s.is_assoc) {
        return none;
    }
    const auto key = std::make_pair(static_cast<const llvm::Module*>(&m), static_cast<uint32_t>(sig));
    if (const auto it = trace_shapes_.find(key); it != trace_shapes_.end()) {
        return it->second;
    }
    const llvm::DataLayout& dl     = env_->llmod.getDataLayout();
    const uint64_t          stride = dl.getTypeAllocSize(s.slot_ty);
    const uint64_t          count  = s.is_array ? s.elem_count : 1;
    // The park copies these bytes and the slot poll reads them: a storage past this size is not
    // read (a large memory), which R6b says it skips.
    constexpr uint64_t max_bytes = 2048;
    llvm::Constant*    result    = none;
    if (stride * count <= max_bytes) {
        const uint32_t bits0 = s.four_state ? 2 * s.width : s.width;
        const uint32_t bits1 = s.four_state && s.has_z ? s.width : 0;
        const uint32_t z_off = bits1 != 0 ? static_cast<uint32_t>(dl.getStructLayout(s.slot_ty)->getElementOffset(1)) : 0;
        auto* const    i32   = llvm::Type::getInt32Ty(ctx_);
        auto* const    ty    = llvm::StructType::get(ctx_, { i32, i32, i32, i32, i32, i32 });
        auto* const    init  = llvm::ConstantStruct::get(
          ty, { llvm::ConstantInt::get(i32, s.width), llvm::ConstantInt::get(i32, bits0), llvm::ConstantInt::get(i32, bits1),
                llvm::ConstantInt::get(i32, z_off), llvm::ConstantInt::get(i32, static_cast<uint32_t>(stride)),
                llvm::ConstantInt::get(i32, static_cast<uint32_t>(count)) });
        auto* gv = new llvm::GlobalVariable(m, ty, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init,
                                            "clotho.trace.shape");
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        result = gv;
    }
    trace_shapes_.emplace(key, result);
    return result;
#else
    (void)m;
    (void)sig;
    return nullptr;
#endif
}

Backend::TraceSeen Backend::trace_seen_words(llvm::Value* old_mm, llvm::Value* new_mm, llvm::Value* old_z,
                                             llvm::Value* new_z, uint32_t width, bool four, bool has_z) {
    TraceSeen t;
#ifdef CLOTHO_EXTRA_ASSERTS
    auto* const i1 = b_.getInt1Ty();
    t.four         = four;
    t.has_z        = has_z;
    t.old_must     = b_.CreateTrunc(old_mm, i1);
    t.new_must     = b_.CreateTrunc(new_mm, i1);
    if (four) {
        auto* const sh = llvm::ConstantInt::get(old_mm->getType(), width);
        t.old_may      = b_.CreateTrunc(b_.CreateLShr(old_mm, sh), i1);
        t.new_may      = b_.CreateTrunc(b_.CreateLShr(new_mm, sh), i1);
    }
    t.diff = b_.CreateICmpNE(old_mm, new_mm);
    if (old_z != nullptr && new_z != nullptr) {
        t.zdiff = b_.CreateICmpNE(old_z, new_z);
    }
#else
    (void)old_mm;
    (void)new_mm;
    (void)old_z;
    (void)new_z;
    (void)width;
    (void)four;
    (void)has_z;
#endif
    return t;
}

void Backend::emit_trace_change(uint32_t path, std::optional<Signal> sig, llvm::Value* x, const TraceSeen& t) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const    ptr   = llvm::PointerType::getUnqual(ctx_);
    auto* const    i32   = b_.getInt32Ty();
    llvm::Module&  m     = *b_.GetInsertBlock()->getModule();
    llvm::Constant* name = sig.has_value() ? trace_signal_name(*sig) : llvm::ConstantPointerNull::get(ptr);
    const std::string where = trace_here();
    const auto        key   = std::make_tuple(static_cast<const llvm::Module*>(&m), where, static_cast<const llvm::Constant*>(name), path);
    llvm::Constant*   site  = nullptr;
    if (const auto it = trace_change_sites_.find(key); it != trace_change_sites_.end()) {
        site = it->second;
    } else {
        auto* const ty   = llvm::StructType::get(ctx_, { ptr, ptr, i32 });
        auto* const init = llvm::ConstantStruct::get(ty, { trace_string(m, where), name, llvm::ConstantInt::get(i32, path) });
        auto*       gv   = new llvm::GlobalVariable(m, ty, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init,
                                                    "clotho.trace.chg");
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        trace_change_sites_.emplace(key, gv);
        site = gv;
    }
    uint32_t     flags = 0;
    llvm::Value* dyn   = nullptr;
    const auto   add   = [&](llvm::Value* v, uint32_t bit) {
        llvm::Value* bitv = b_.CreateShl(b_.CreateZExt(v, i32), bit);
        dyn               = dyn != nullptr ? b_.CreateOr(dyn, bitv) : bitv;
    };
    if (t.four) {
        flags |= RT_TRACE_SEEN_FOUR;
    }
    if (t.has_z) {
        flags |= RT_TRACE_SEEN_HAS_Z;
    }
    if (t.level) {
        flags |= RT_TRACE_SEEN_LEVEL;
    } else {
        if (t.old_must != nullptr && t.new_must != nullptr) {
            flags |= RT_TRACE_SEEN_BIT0;
            add(t.old_must, 0);
            add(t.new_must, 3);
            if (t.four) {
                add(t.old_may, 1);
                add(t.new_may, 4);
            }
        }
        if (t.diff != nullptr) {
            flags |= RT_TRACE_SEEN_DIFF_KNOWN;
            add(t.diff, 10);
        }
        if (t.zdiff != nullptr) {
            flags |= RT_TRACE_SEEN_ZDIFF_KNOWN;
            add(t.zdiff, 12);
        }
        if (t.pos != nullptr) {
            flags |= RT_TRACE_SEEN_POS_KNOWN;
            add(t.pos, 16);
        }
        if (t.neg != nullptr) {
            flags |= RT_TRACE_SEEN_NEG_KNOWN;
            add(t.neg, 17);
        }
        if (t.any != nullptr) {
            flags |= RT_TRACE_SEEN_ANY_KNOWN;
            add(t.any, 18);
        }
    }
    llvm::Value* seen = b_.getInt32(flags);
    if (dyn != nullptr) {
        seen = b_.CreateOr(seen, dyn);
    }
    b_.CreateCall(rt_fn("rt_trace_change", b_.getVoidTy(), { ptr, ptr, i32 }),
                  { site, x != nullptr ? x : llvm::ConstantPointerNull::get(ptr), seen });
#else
    (void)path;
    (void)sig;
    (void)x;
    (void)t;
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
        b_.CreateCall(rt_fn("rt_trace_waits_for", b_.getVoidTy(), { ptr, ptr, ptr, i64, ptr }),
                      { hdl_, t.x, t.name, b_.getInt64(t.edge), t.shape });
    }
    // The park's operand: a delay's ticks, or the record or queue it waits on, which the wake that
    // ends it names too. The handle carries the thread's home tag, which says which batch a `#0`
    // waits in.
    llvm::Value* arg = park.delay != nullptr ? park.delay
                       : park.rec != nullptr ? b_.CreatePtrToInt(park.rec, i64)
                                             : static_cast<llvm::Value*>(b_.getInt64(0));
    b_.CreateCall(rt_fn("rt_trace_park", b_.getVoidTy(), { ptr, ptr, i64 }), { parked_handle(b_, hdl_), ctl, arg });
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
    // The cause, and the ring the push goes onto in the bits above it.
    llvm::Value* cause = b.getInt32(why.kind);
    if (why.ring != nullptr) {
        cause = b.CreateOr(cause, b.CreateShl(why.ring, RT_TRACE_QUEUE_SHIFT));
    }
    b.CreateCall(rt_fn("rt_trace_wake", b.getVoidTy(), { ptr, i32, ptr, i32 }), { a, cause, x, b.getInt32(why.fact) });
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

void Backend::emit_trace_block(llvm::Value* a, llvm::Value* list, uint32_t how) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on() || a == nullptr) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_block", b_.getVoidTy(), { ptr, ptr, b_.getInt32Ty() }), { a, list, b_.getInt32(how) });
#else
    (void)a;
    (void)list;
    (void)how;
#endif
}

void Backend::emit_trace_disable(llvm::Value* list) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_disable", b_.getVoidTy(), { ptr, ptr }),
                  { hdl_ != nullptr ? static_cast<llvm::Value*>(hdl_) : llvm::ConstantPointerNull::get(ptr), list });
#else
    (void)list;
#endif
}

void Backend::emit_trace_abort(llvm::Value* a, llvm::Value* list, uint32_t kind) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_abort", b_.getVoidTy(), { ptr, ptr, b_.getInt32Ty() }),
                  { a, list != nullptr ? list : llvm::ConstantPointerNull::get(ptr), b_.getInt32(kind) });
#else
    (void)a;
    (void)list;
    (void)kind;
#endif
}

void Backend::emit_trace_fork(llvm::Value* rec, uint32_t kind, uint32_t width) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const i32 = b_.getInt32Ty();
    // A function has no activation: a fork there can only be a join_none, which parks nobody.
    llvm::Value* a = in_coroutine_body() ? static_cast<llvm::Value*>(hdl_) : llvm::ConstantPointerNull::get(ptr);
    b_.CreateCall(rt_fn("rt_trace_fork", b_.getVoidTy(), { ptr, ptr, i32, i32 }),
                  { a, rec, b_.getInt32(kind), b_.getInt32(width) });
#else
    (void)rec;
    (void)kind;
    (void)width;
#endif
}

void Backend::emit_trace_child_done(llvm::Value* a, llvm::Value* rec) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_child_done", b_.getVoidTy(), { ptr, ptr }), { a, rec });
#else
    (void)a;
    (void)rec;
#endif
}

void Backend::emit_trace_join(llvm::Value* rec) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_join", b_.getVoidTy(), { ptr, ptr }), { hdl_, rec });
#else
    (void)rec;
#endif
}

void Backend::emit_trace_once(uint32_t kind) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    b_.CreateCall(rt_fn("rt_trace_once", b_.getVoidTy(), { b_.getInt32Ty() }), { b_.getInt32(kind) });
#else
    (void)kind;
#endif
}

// A thunk group is an activation named by the address of its wake slot with bit 0 set, which
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
                                  llvm::Value* n, llvm::Value* parked_now, const TraceMembers& members) {
#ifdef CLOTHO_EXTRA_ASSERTS
    auto* const ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* const i32  = b_.getInt32Ty();
    auto* const i64  = b_.getInt64Ty();
    b_.CreateCall(rt_fn("rt_trace_spawn", b_.getVoidTy(), { ptr, ptr, ptr }),
                  { h, origin, llvm::ConstantPointerNull::get(ptr) });
    b_.CreateCall(rt_fn("rt_trace_subscribe", b_.getVoidTy(), { ptr, ptr, i32 }), { h, list, n });
    llvm::Function* fn      = b_.GetInsertBlock()->getParent();
    auto* const     guarded = llvm::BasicBlock::Create(ctx_, "trace.members", fn);
    auto* const     park    = llvm::BasicBlock::Create(ctx_, "trace.park", fn);
    auto* const     done    = llvm::BasicBlock::Create(ctx_, "trace.parked", fn);
    auto* const     decided = llvm::BasicBlock::Create(ctx_, "trace.membered", fn);
    // Only a guarded group has members to report.
    b_.CreateCondBr(b_.CreateICmpNE(members.members, b_.getInt32(0)), guarded, decided);
    b_.SetInsertPoint(guarded);
    b_.CreateCall(rt_fn("rt_trace_members", b_.getVoidTy(), { ptr, ptr, i32, i32, i32 }),
                  { h, members.list, members.n, members.members, members.exact });
    b_.CreateBr(decided);
    b_.SetInsertPoint(decided);
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
    (void)members;
#endif
}

void Backend::emit_trace_member(uint32_t index) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const i64 = b_.getInt64Ty();
    // A thunk row is no coroutine: its activation is the wake slot its flag points at, with bit 0 set
    // (emit_trace_thunk_park).
    llvm::Value* a = hdl_ != nullptr
                       ? static_cast<llvm::Value*>(hdl_)
                       : b_.CreateIntToPtr(b_.CreateOr(b_.CreatePtrToInt(sched_ptr_, i64), b_.getInt64(1)), ptr);
    b_.CreateCall(rt_fn("rt_trace_member", b_.getVoidTy(), { ptr, b_.getInt32Ty() }), { a, b_.getInt32(index) });
#else
    (void)index;
#endif
}

void Backend::emit_trace_inertial_arm(llvm::Value* idx, llvm::Value* rec, llvm::Value* ticks, llvm::Value* epoch) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    b_.CreateCall(rt_fn("rt_trace_inertial_arm", b_.getVoidTy(), { b_.getInt32Ty(), ptr, b_.getInt64Ty(), b_.getInt64Ty() }),
                  { idx, rec, ticks, epoch });
#else
    (void)idx;
    (void)rec;
    (void)ticks;
    (void)epoch;
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

void Backend::emit_trace_nba(llvm::Value* rec, const TraceNba& t, uint32_t path, llvm::Value* ticks) {
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
    b_.CreateCall(rt_fn("rt_trace_nba_stage", b_.getVoidTy(), { ptr, b_.getInt64Ty(), ptr, ptr, ptr, ptr, ptr, ptr, ptr }),
                  { rec, ticks != nullptr ? ticks : b_.getInt64(0), site, t.dst0, or_none(t.dst1), or_none(t.val0),
                    or_none(t.mask0), or_none(t.val1), or_none(t.mask1) });
#else
    (void)rec;
    (void)t;
    (void)path;
    (void)ticks;
#endif
}

void Backend::emit_trace_nba_events(llvm::Value* rec, llvm::Value* count, llvm::ArrayRef<TraceTrigger> trigs) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (!trace_on()) {
        return;
    }
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    auto* const i32 = b_.getInt32Ty();
    for (const TraceTrigger& t : trigs) {
        b_.CreateCall(rt_fn("rt_trace_nba_event", b_.getVoidTy(), { ptr, i32, ptr, i32 }),
                      { rec, count, t.x, b_.getInt32(t.edge) });
    }
#else
    (void)rec;
    (void)count;
    (void)trigs;
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

// A store of a force or a procedural `assign` (its mark says so), reported after it. A state store reports the
// storage of the state signal it wrote, an apply the target, its range and the state of the layer that owns it
// (and, for a net's force statement, the signal that holds the forced value), and the resolver's store the net.
// Only a plain signal is read: a place that can be invalid, an aggregate or a string is not one a force targets,
// and an apply to one reports no target, which still settles what its statement owes.
void Backend::emit_trace_override(Index store, const OverrideMark& mark, const Guarded& g) {
#ifdef CLOTHO_EXTRA_ASSERTS
    (void)store;
    const bool apply = mark.kind == OverrideKind::ForceApply || mark.kind == OverrideKind::AssignApply ||
                       mark.kind == OverrideKind::SiteApply;
    const bool plain = g.valid() == nullptr && !g.never_valid() && g.is_signal && !g.is_agg && !g.is_string &&
                       g.array_ty == nullptr;
    if (!trace_on() || (!plain && !apply)) {
        return;
    }
    auto* const           ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* const           i32  = b_.getInt32Ty();
    auto* const           none = llvm::ConstantPointerNull::get(ptr);
    llvm::Module&         m    = *b_.GetInsertBlock()->getModule();
    llvm::Constant* const site = trace_control(m, RT_TRACE_OVR_FORCE_STATE + static_cast<uint32_t>(mark.kind), trace_here());
    const auto            apply_fn = [&] {
        return rt_fn("rt_trace_override_apply", b_.getVoidTy(), { ptr, ptr, ptr, ptr, ptr, i32, i32, ptr, ptr });
    };
    if (apply) {
        const LValue state = lvalue_of(mark.state).unguarded();
        if (!plain) {
            b_.CreateCall(apply_fn(), { site, slot_ptr(state), none, none, none, b_.getInt32(0), b_.getInt32(0), none, none });
            return;
        }
    }
    const LValue lv = g.unguarded();
    switch (mark.kind) {
        case OverrideKind::ForceState:
        case OverrideKind::AssignState:
        case OverrideKind::SiteState:
            b_.CreateCall(rt_fn("rt_trace_override_state", b_.getVoidTy(), { ptr, ptr }), { site, slot_ptr(lv) });
            return;
        case OverrideKind::ForceApply:
        case OverrideKind::AssignApply:
        case OverrideKind::SiteApply: {
            const LValue    state  = lvalue_of(mark.state).unguarded();
            llvm::Value*    val    = none;
            llvm::Constant* vshape = none;
            if (mark.kind == OverrideKind::SiteApply) {
                const LValue forced = lvalue_of(mark.shadow).unguarded();
                val                 = slot_ptr(forced);
                vshape              = trace_shape(m, forced.sig);
            }
            b_.CreateCall(apply_fn(), { site, slot_ptr(state), slot_ptr(lv), trace_signal_name(lv.sig),
                                        trace_shape(m, lv.sig), b_.CreateTrunc(lv.bit_off, i32), b_.getInt32(lv.width),
                                        val, vshape });
            return;
        }
        case OverrideKind::Resolve:
            b_.CreateCall(rt_fn("rt_trace_override_resolve", b_.getVoidTy(), { ptr, ptr, ptr, ptr }),
                          { site, slot_ptr(lv), trace_signal_name(lv.sig), trace_shape(m, lv.sig) });
            return;
    }
#else
    (void)store;
    (void)mark;
    (void)g;
#endif
}

} // namespace clotho::ir::backend
