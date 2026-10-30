// VCD dumping: the signal index, dump tasks and value-change touches.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// The runtime rt_vcd_active byte, non-zero only while VCD dumping is live.
llvm::Constant* Backend::vcd_active_gv() {
    return env_->llmod.getOrInsertGlobal("rt_vcd_active", b_.getInt8Ty());
}

// Emit a VCD touch call guarded by the runtime active flag, so a design that
// compiles in $dumpvars (possibly plusarg-guarded) but is not dumping pays
// only a predictable-not-taken load+branch per store, not a runtime call.
// `make_args` runs inside the taken branch, so index loads (the NBA vcd
// table, a reused instance's vcd base) stay off the inactive hot path —
// the point of the gate (see the touch hooks in docs/VCD.md, section 8).
// `extra` narrows the guard beyond "dumping is active" -- an event carries its
// dump index in its object, so whether it is dumped at all is a runtime test.
void Backend::emit_guarded_vcd_touch(llvm::FunctionCallee                                     fn,
                                     llvm::function_ref<llvm::SmallVector<llvm::Value*, 2>()> make_args,
                                     llvm::Value*                                             extra) {
    llvm::Function*   f      = b_.GetInsertBlock()->getParent();
    llvm::Value*      active = b_.CreateLoad(b_.getInt8Ty(), vcd_active_gv());
    llvm::Value*      pred   = b_.CreateICmpNE(active, b_.getInt8(0));
    if (extra != nullptr) {
        pred = b_.CreateAnd(pred, extra);
    }
    llvm::BasicBlock* touch  = llvm::BasicBlock::Create(ctx_, "vcd.touch", f);
    llvm::BasicBlock* cont   = llvm::BasicBlock::Create(ctx_, "vcd.cont", f);
    b_.CreateCondBr(pred, touch, cont);
    b_.SetInsertPoint(touch);
    b_.CreateCall(fn, make_args());
    b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
}

// Any Dump* instruction anywhere in the design? Gates the per-instant flush and
// the close at the end of the sim loop, so a design with no dump task pays
// nothing. A $dumpfile / $dumpvars call site can live in a task body a process
// calls, so scan function bodies too (like design_has_finish) -- scanning only
// processes left the flush unemitted and silently produced a VCD holding just
// the final instant.
bool Backend::design_uses_vcd() const {
    const auto scan = [](const Ir& ir) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            switch (ir.tag(static_cast<Index>(k))) {
                case Inst::Tag::DumpFile:
                case Inst::Tag::DumpVars:
                case Inst::Tag::DumpLimit:
                case Inst::Tag::DumpOff:
                case Inst::Tag::DumpOn:
                case Inst::Tag::DumpAll:
                case Inst::Tag::DumpFlush:
                    return true;
                default:
                    break;
            }
        }
        return false;
    };
    for (const Process& p : mod_.processes()) {
        if (scan(p.ir)) {
            return true;
        }
    }
    for (uint32_t f = 0; f < mod_.function_count(); ++f) {
        if (scan(mod_.function(static_cast<FunctionId>(f)).ir)) {
            return true;
        }
    }
    return false;
}

// Assign each dumped scalar signal its runtime dump-var index, in the exact
// order rt_vcd_var / rt_vcd_array register variables (the emit_vcd_var walk):
// an unpacked array consumes elem_count indices (its always-scanned elements),
// a scalar consumes one. Precomputed before any process body so a store's touch
// uses a compile-time-constant index. Kept in lockstep with emit_vcd_var /
// emit_vcd_scope_tree; a byte-diff of the dump verifies the two match.
void Backend::build_vcd_index() {
    design_uses_vcd_ = design_uses_vcd();
    if (!design_uses_vcd_) {
        return;
    }
    vcd_index_.assign(mod_.signals().size(), -1);
    uint32_t   next = 0;
    const auto tops = mod_.tops();
    if (tops.empty()) {
        for (uint32_t si = 0; si < mod_.signals().size(); ++si) {
            vcd_index_signal(static_cast<Signal>(si), next);
        }
    } else {
        for (const InstanceScope& top : tops) {
            vcd_index_scope(top, next);
        }
    }
}

void Backend::vcd_index_scope(const InstanceScope& sc, uint32_t& next) {
    const uint32_t aid =
      sc.id < anchor_of_scope_.size() ? anchor_of_scope_[sc.id] : SignalSlot::no_anchor;
    for (const Signal sig : sc.signals) {
        const uint32_t before = next;
        vcd_index_signal(sig, next);
        if (next != before && aid != SignalSlot::no_anchor && anchors_[aid].first_vcd < 0) {
            anchors_[aid].first_vcd = static_cast<int32_t>(before);
        }
    }
    for (const InstanceScope& child : sc.children) {
        vcd_index_scope(child, next);
    }
}

void Backend::vcd_index_signal(Signal sig, uint32_t& next) {
    const SignalInfo& info = mod_.signal(sig);
    if (info.name.empty()) {
        return;
    }
    if (vcd_unrepresentable(info.type) != nullptr) {
        // Never registers: emit_vcd_var refuses a declared one and skips a
        // compiler-synthesized one, so neither consumes a dump-var index.
        return;
    }
    const SignalSlot& s = slot_of(sig);
    if (s.is_array) {
        // Record the first element's dump-var index; element k is base + k. An
        // element write touches base + normalized index (blocking) or the whole
        // range (NBA), instead of being scanned every instant.
        vcd_index_[static_cast<uint32_t>(sig)] = static_cast<int32_t>(next);
        next += s.elem_count;
        return;
    }
    vcd_index_[static_cast<uint32_t>(sig)] = static_cast<int32_t>(next++);
}

// The rt_vcd_touch index of a dumped signal: def-relative (instance vcd base
// + def-local offset) when the owner is a reused instance, the folded
// constant otherwise — dedup prep, same split as storage / sched anchors.
llvm::Value* Backend::vcd_index_value(Signal sig, int32_t base) {
    const SignalSlot& s = slot_of(sig);
    if (s.anchor != SignalSlot::no_anchor && def_relative_for(s)) {
        Anchor& a = anchors_[s.anchor];
        if ((a.indirect || ctx_override_for(s)) && a.first_vcd >= 0) {
            llvm::Value* b0 = ctx_field(a, 3, b_.getInt32Ty());
            return b_.CreateAdd(b0, b_.getInt32(static_cast<uint32_t>(base - a.first_vcd)));
        }
    }
    if (abs_id_eligible(s) && anchors_[s.anchor].first_vcd >= 0) {
        Anchor& a = anchors_[s.anchor];
        return abs_id_const("vcd." + a.name, static_cast<uint32_t>(a.first_vcd),
                            static_cast<uint32_t>(base - a.first_vcd));
    }
    return b_.getInt32(static_cast<uint32_t>(base));
}

// Emit rt_vcd_touch for a blocking write to a dumped signal, marking it in the
// dirty set. A scalar touches its index; an array element touches base + its
// normalized element index (a whole-array store touches the range). A no-op for
// a non-dumped signal or a PreInit store (before $dumpvars, subsumed by the
// initial dump).
void Backend::vcd_touch_lvalue(const LValue& lv) {
    if (!design_uses_vcd_ || silent_stores_ || !lv.is_signal) {
        return;
    }
    const int32_t base = vcd_index_[static_cast<uint32_t>(lv.sig)];
    if (base < 0) {
        return;
    }
    const SignalSlot& s = slot_of(lv.sig);
    if (s.is_array) {
        if (lv.elem_idx != nullptr) {
            emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
                return llvm::SmallVector<llvm::Value*, 2>{
                    b_.CreateAdd(vcd_index_value(lv.sig, base), lv.elem_idx)
                };
            });
        } else {
            emit_guarded_vcd_touch(vcd_touch_range_fn(), [&] {
                return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(lv.sig, base),
                                                           b_.getInt32(s.elem_count) };
            });
        }
        return;
    }
    emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
        return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(lv.sig, base) };
    });
}

// Emit an rt_vcd_touch for one dumped scalar signal. Used by the deferred-record
// replay of a member written through a virtual interface, where the index is
// the bound instance's (an installed CtxOverride supplies its base).
void Backend::vcd_touch_signal(Signal sig) {
    if (!design_uses_vcd_) {
        return;
    }
    const int32_t base = vcd_index_[static_cast<uint32_t>(sig)];
    if (base >= 0) {
        emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
            return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(sig, base) };
        });
    }
}

// Emit an rt_vcd_touch_range for a dumped array signal (whole element range).
// Used at the array-NBA commit, where the runtime queue does not carry the
// per-element index, and after $readmem / $fread bulk writes.
void Backend::vcd_touch_array(Signal sig) {
    if (!design_uses_vcd_) {
        return;
    }
    const int32_t base = vcd_index_[static_cast<uint32_t>(sig)];
    if (base >= 0) {
        emit_guarded_vcd_touch(vcd_touch_range_fn(), [&] {
            return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(sig, base),
                                                       b_.getInt32(slot_of(sig).elem_count) };
        });
    }
}

// Emit an rt_vcd_touch for a dumped named event. Called from emit_event_fire,
// the one place every trigger form converges (`-> e` in a process body and the
// NBA-region service of `->> e` / `->> #d e`), so the dump sees an occurrence
// however it was produced.
void Backend::vcd_touch_event_obj(llvm::Value* obj) {
    if (!design_uses_vcd_) {
        return;
    }
    llvm::Value* idx = b_.CreateLoad(b_.getInt32Ty(), event_field(obj, kEventVcd));
    emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
        return llvm::SmallVector<llvm::Value*, 2>{ idx };
    }, b_.CreateICmpSGE(idx, b_.getInt32(0)));
}

void Backend::vcd_touch_event_unused(Signal sig) {
    if (!design_uses_vcd_) {
        return;
    }
    const int32_t base = vcd_index_[static_cast<uint32_t>(sig)];
    if (base >= 0) {
        emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
            return llvm::SmallVector<llvm::Value*, 2>{ vcd_index_value(sig, base) };
        });
    }
}

// $dumpfile: record the output path. A string-object filename is read straight
// from its rt_string handle (natural byte order, forward=1); otherwise the
// value is a packed-ASCII vector whose must-bits spill to a byte buffer
// (forward=0), the same split as $readmem's filename.
void Backend::lower_dump_file(Index i) {
    const Index   fname = ir_->payload(i).unary;
    const ValType fty   = type_of(fname);
    llvm::Value*  path  = nullptr;
    llvm::Value*  len   = nullptr;
    uint32_t      fwd   = 0;
    if (fty.is_string) {
        llvm::Value* handle = str_of(fname);
        path                = b_.CreateCall(str_data_fn(), { handle });
        len                 = b_.CreateTrunc(b_.CreateCall(str_len_fn(), { handle }), b_.getInt32Ty());
        fwd                 = 1;
    } else {
        const uint32_t fbytes = std::max<uint32_t>((fty.width + 7) / 8, 1);
        path                  = b_.CreateAlloca(b_.getInt8Ty(), b_.getInt32(fbytes), "dumpfile.path");
        len                   = b_.getInt32(fbytes);
        b_.CreateAlignedStore(must_of(value_of(fname)), path, llvm::Align(1));
    }
    b_.CreateCall(vcd_open_fn(), { path, len, b_.getInt32(fwd) });
}

// $dumplimit(n): cap the dump at `n` bytes. The count is a plain integral
// value; its known bits are widened to the runtime's 64-bit byte count.
void Backend::lower_dump_limit(Index i) {
    const Index n = ir_->payload(i).unary;
    b_.CreateCall(vcd_limit_fn(), { cast(must_of(value_of(n)), 64, false) });
}

// $dumpvars (whole-design form): walk the design scope tree, registering every
// dumpable signal with the runtime, then arm the dump.
void Backend::lower_dump_vars() {
    b_.CreateCall(vcd_begin_fn(),
                  { wheel_, b_.getInt64(mod_.tick_fs), b_.getInt32(mod_.timeformat_units) });
    const auto tops = mod_.tops();
    if (tops.empty()) {
        emit_vcd_scope(mod_.name);
        for (uint32_t s = 0; s < mod_.signals().size(); ++s) {
            emit_vcd_var(static_cast<Signal>(s));
        }
        b_.CreateCall(vcd_upscope_fn(), {});
    } else {
        for (const InstanceScope& top : tops) {
            emit_vcd_scope_tree(top);
        }
    }
    b_.CreateCall(vcd_start_fn(), {});
}

void Backend::emit_vcd_scope(std::string_view name) {
    llvm::Value* np = b_.CreateGlobalString(llvm::StringRef(name.data(), name.size()), "vcd.scope");
    b_.CreateCall(vcd_scope_fn(), { np, b_.getInt32(static_cast<uint32_t>(name.size())) });
}

void Backend::emit_vcd_scope_tree(const InstanceScope& sc) {
    emit_vcd_scope(sc.name);
    for (const Signal sig : sc.signals) {
        emit_vcd_var(sig);
    }
    for (const InstanceScope& child : sc.children) {
        emit_vcd_scope_tree(child);
    }
    b_.CreateCall(vcd_upscope_fn(), {});
}

// Why a signal's type has no $var form, or nullptr when it has one. Mirrors
// lower/Lower.cpp's vcd_unrepresentable, which is where a declared signal is actually
// refused; this keeps the crash impossible if a path ever reaches codegen
// without that check (an aggregate has no leaf slot type, so the scalar $var
// path below would dereference null).
const char* Backend::vcd_unrepresentable(Ref type) const {
    Ref leaf = type;
    while (std::get<Type>(mod_.pool.get(leaf)).kind == Type::Kind::UnpackedArray) {
        leaf = std::get<Type>(mod_.pool.get(leaf)).element;
    }
    const Type& t = std::get<Type>(mod_.pool.get(leaf));
    switch (t.kind) {
        case Type::Kind::String:
            return "string";
        case Type::Kind::Queue:
            return "queue";
        case Type::Kind::DynArray:
            return "dynamic array";
        case Type::Kind::Assoc:
            return "associative array";
        case Type::Kind::Class:
            return "class handle";
        case Type::Kind::Struct:
            return t.is_packed ? nullptr : "unpacked struct";
        default:
            return nullptr;
    }
}

// Register one dumped signal: leaf name, width/flags, and live plane pointers.
// An unpacked array expands to one $var per element (rt_vcd_array); a real /
// shortreal carries RT_VCD_FLAG_REAL and dumps as `$var real` (the runtime
// formats the slot's IEEE-754 bit pattern as a decimal); a named event carries
// RT_VCD_FLAG_EVENT and dumps as `$var event` (see below). A whole-design dump
// must represent every declared signal, so an unrepresentable one (string,
// container, aggregate, class handle, multidimensional array) loud-fails rather
// than being silently omitted (PRINCIPLES.md); an unnamed compiler temporary is not
// a declared variable and is skipped.
void Backend::emit_vcd_var(Signal sig) {
    const SignalInfo& info = mod_.signal(sig);
    if (info.name.empty()) {
        return;
    }
    std::string leaf = info.name;
    if (const auto pos = leaf.rfind('.'); pos != std::string::npos) {
        leaf = leaf.substr(pos + 1);
    }
    auto* nullp = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    if (info.is_event) {
        // A named event (LRM 15.5) has no value -- its 1-bit placeholder slot
        // is never written -- so what the dump carries is occurrences. The
        // runtime watches the event's last-trigger tick (clotho.event_trig,
        // stamped by emit_event_fire for every trigger form) and emits `1<id>`
        // at each instant that stamp moves.
        llvm::Value* np   = b_.CreateGlobalString(leaf, "vcd.var");
        llvm::Value* nlen = b_.getInt32(static_cast<uint32_t>(leaf.size()));
        b_.CreateCall(vcd_var_fn(), { np, nlen, b_.getInt32(1), b_.getInt32(16),
                                      event_field(event_obj(sig), kEventTrig), nullp });
        return;
    }
    const SignalSlot& s = slot_of(sig);
    // Second line of defence behind the frontend's whole-design check
    // (vcd_unrepresentable in lower/Lower.cpp): a string / container / aggregate /
    // class-handle signal has no $var form. A DECLARED one is refused; a
    // compiler-synthesized one (SourceLoc{0}: a gate input net, a materialized
    // container temp) is not design state and is simply not dumped, which is
    // also why the frontend check skips it.
    if (const char* what = vcd_unrepresentable(info.type); what != nullptr) {
        if (info.loc == SourceLoc{ 0 }) {
            return;
        }
        throw BackendError{ std::string("VCD dump of ") + what + " signal '" + info.name + "' is not supported" };
    }
    // Flag bits mirror RT_VCD_FLAG_* (Runtime.h): four-state, net, narrow,
    // real; the event bit is set on the branch above.
    const uint32_t flags = (s.four_state ? 1u : 0u) | (info.is_net ? 2u : 0u) | (s.narrow ? 4u : 0u)
                         | (s.is_real ? 8u : 0u);
    llvm::Value*   np    = b_.CreateGlobalString(leaf, "vcd.var");
    llvm::Value*   nlen  = b_.getInt32(static_cast<uint32_t>(leaf.size()));

    if (!s.is_array) {
        llvm::Value* storage = storage_ptr(s);
        llvm::Value* mm      = b_.CreateStructGEP(s.slot_ty, storage, 0);
        llvm::Value* zp = s.has_z ? b_.CreateStructGEP(s.slot_ty, storage, 1) : llvm::cast<llvm::Value>(nullp);
        b_.CreateCall(vcd_var_fn(), { np, nlen, b_.getInt32(s.width), b_.getInt32(flags), mm, zp });
        return;
    }

    // Unpacked array (memory): one $var per element. Multidimensional arrays
    // (a nested UnpackedArray element type) need per-dimension index naming and
    // are deferred.
    const Type& at = std::get<Type>(mod_.pool.get(info.type));
    if (std::get<Type>(mod_.pool.get(at.element)).kind == Type::Kind::UnpackedArray) {
        throw BackendError{ "VCD dump of multidimensional array '" + info.name + "' is not supported" };
    }
    auto*        arr_ty  = llvm::cast<llvm::ArrayType>(s.storage_ty);
    llvm::Value* base    = storage_ptr(s);
    llvm::Value* mm_base = b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(0) });
    llvm::Value* z_base =
      s.has_z ? b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), b_.getInt32(0), b_.getInt32(1) })
              : llvm::cast<llvm::Value>(nullp);
    const auto stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(s.slot_ty));
    b_.CreateCall(vcd_array_fn(), { np, nlen, b_.getInt32(s.elem_count), b_.getInt32(s.width),
                                    b_.getInt32(flags), mm_base, z_base, b_.getInt32(stride),
                                    b_.getInt64(at.left), b_.getInt64(at.right) });
}

// $dumpoff / $dumpon / $dumpall / $dumpflush: forward to the matching runtime
// control entry.
void Backend::lower_dump_control(Inst::Tag tag) {
    switch (tag) {
        case Inst::Tag::DumpOff:
            b_.CreateCall(vcd_nullary_fn("rt_vcd_off"), {});
            return;
        case Inst::Tag::DumpOn:
            b_.CreateCall(vcd_nullary_fn("rt_vcd_on"), {});
            return;
        case Inst::Tag::DumpAll:
            b_.CreateCall(vcd_nullary_fn("rt_vcd_all"), {});
            return;
        case Inst::Tag::DumpFlush:
            b_.CreateCall(vcd_nullary_fn("rt_vcd_dumpflush"), {});
            return;
        default:
            throw BackendError{ "internal: non-control dump tag" };
    }
}

} // namespace clotho::ir::backend
