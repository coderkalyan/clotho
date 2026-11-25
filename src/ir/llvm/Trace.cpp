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

} // namespace clotho::ir::backend
