// Stores: blocking and nonblocking stores, NBA classification, staging and commit, intra-assignment timing.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

Backend::NbaTarget Backend::nba_target(const Ir& ir, Index lv) const {
    const LvalueRoot r = lvalue_root(ir, lv);
    // A member reached through a virtual interface stages through the
    // REPRESENTATIVE's def-local shadow fields and block ordinal; which
    // instance's shadow those name is resolved at run time from the handle
    // (LValue::vif_ctx), exactly as a reused definition's own body does.
    if (ir.tag(r.root) == Inst::Tag::IfaceSignal) {
        return { ir.extra_data<IfaceMember>(ir.payload(r.root).op_extra.extra).signal, r.whole };
    }
    if (ir.tag(r.root) != Inst::Tag::Signal) {
        throw BackendError{ "unsupported nonblocking-assignment lvalue" };
    }
    return { ir.payload(r.root).signal, r.whole };
}

// Whether an lvalue select chain contains a runtime-indexed select (BitSelect /
// DynPartSelect), i.e. its written-bit mask is not a compile-time constant. A
// constant-index BitSelect is treated as dynamic too (conservative — the field is
// allocated but the commit still uses a constant mask when bit_off folds).
bool Backend::lvalue_has_dynamic_select(const Ir& ir, Index lv) {
    for (;;) {
        switch (ir.tag(lv)) {
            case Inst::Tag::BitSelect:
                return true;
            case Inst::Tag::DynPartSelect:
                return true;
            case Inst::Tag::PartSelect:
            case Inst::Tag::ElemSelect:
            case Inst::Tag::ArrSlice:
            case Inst::Tag::MemberSelect:
                lv = ir.payload(lv).op_extra.op;
                break;
            default:
                return false;
        }
    }
}

// Whether an lvalue chain contains a bit/part-select (a within-element select
// for a memory: BitSelect / PartSelect / DynPartSelect), as opposed to only an
// ElemSelect array index. Determines whether an array target needs a write mask.
bool Backend::lvalue_has_subselect(const Ir& ir, Index lv) {
    for (;;) {
        switch (ir.tag(lv)) {
            case Inst::Tag::BitSelect:
                lv = ir.payload(lv).binary.l;
                return true;
            case Inst::Tag::PartSelect:
            case Inst::Tag::DynPartSelect:
                return true;
            case Inst::Tag::ElemSelect:
            case Inst::Tag::ArrSlice:
            case Inst::Tag::MemberSelect:
                lv = ir.payload(lv).op_extra.op;
                break;
            default:
                return false;
        }
    }
}

// Whether an lvalue chain contains an unpacked-array element select (a
// runtime element index). For an aggregate NBA target this discriminates a
// static-path leaf (block-commit shadow) from a runtime one (deferred path).
bool Backend::lvalue_has_elem_select(const Ir& ir, Index lv) {
    for (;;) {
        switch (ir.tag(lv)) {
            case Inst::Tag::ElemSelect:
                return true;
            case Inst::Tag::BitSelect:
                lv = ir.payload(lv).binary.l;
                break;
            case Inst::Tag::PartSelect:
            case Inst::Tag::DynPartSelect:
            case Inst::Tag::ArrSlice:
            case Inst::Tag::MemberSelect:
                lv = ir.payload(lv).op_extra.op;
                break;
            default:
                return false;
        }
    }
}

// Whether an lvalue chain designates a `string` place: walked down from the
// root's own type, since this runs BEFORE lowering (nba_classify_ir) and there
// is no LValue yet. True for a whole string signal / local, a string member of
// an unpacked struct, and a string element of a fixed unpacked array, composed
// to any depth — exactly the places whose NBA record must hold an owned handle
// rather than bytes.
bool Backend::lvalue_is_string_place(const Ir& ir, Index lv) const {
    const Ref leaf = lvalue_place_type(ir, lv);
    return leaf != Ref::Invalid && std::get<Type>(mod_.pool.get(leaf)).kind == Type::Kind::String;
}

// The IR type an lvalue chain designates, or Ref::Invalid for a shape whose
// type cannot be walked statically (a bit / part select, a container element, a
// handle deref — none of which can be a string place).
Ref Backend::lvalue_place_type(const Ir& ir, Index lv) const {
    switch (ir.tag(lv)) {
        case Inst::Tag::Signal:
            return mod_.signal(ir.payload(lv).signal).type;
        case Inst::Tag::Local:
            return ir.payload(lv).local.type;
        case Inst::Tag::MemberSelect: {
            const auto  ms = ir.extra_data<MemberSelect>(ir.payload(lv).op_extra.extra);
            const Type& st = std::get<Type>(mod_.pool.get(ms.struct_type));
            const auto  fs = mod_.pool.struct_fields(st.fields_id);
            return ms.field_index < fs.size() ? fs[ms.field_index].type : Ref::Invalid;
        }
        case Inst::Tag::ElemSelect: {
            const Ref base = lvalue_place_type(ir, ir.payload(lv).op_extra.op);
            if (base == Ref::Invalid) {
                return Ref::Invalid;
            }
            const Type& t = std::get<Type>(mod_.pool.get(base));
            return t.kind == Type::Kind::UnpackedArray ? t.element : Ref::Invalid;
        }
        default:
            return Ref::Invalid;
    }
}

// Register a nonblocking intra-assignment (or inertial continuous-assign)
// target and record how its deferred record must be shaped. The record reuses
// plain NBA's lvalue encoding, so each target shape mirrors the masking rule
// of its plain-NBA sibling: a scalar select is masked whenever it is not the
// whole signal (like nba_partial), while an array element or aggregate leaf is
// masked only for a select *below* the element/leaf (like arr_masked /
// agg_masked) because a whole-element write replaces the element outright.
//
// An array target must NOT be marked `nba`: array signals are deliberately
// never NBA-metadata targets (they carry no nba_id, so nba_meta indexing and
// emit_nba_service would read out of bounds) and instead wake through
// service_any_waiters -- see docs/design/static-arr-nba.md. Every other shape
// keeps the flag, exactly as a plain NBA to the same target would set it.
void Backend::classify_intra_target(const Ir& ir, Index lv_idx, const NbaTarget& t) {
    SignalSlot& s  = slots_[static_cast<uint32_t>(t.sig)];
    const Ref   ty = mod_.signals()[static_cast<uint32_t>(t.sig)].type;
    if (contains_struct(ty)) {
        s.nba          = true;
        s.intra_masked = s.intra_masked || lvalue_has_subselect(ir, lv_idx);
    } else if (std::get<Type>(mod_.pool.get(ty)).kind == Type::Kind::UnpackedArray) {
        s.intra_masked = s.intra_masked || lvalue_has_subselect(ir, lv_idx);
    } else if (s.vif_rec) {
        // A member some handle writes: the destination is whichever instance the
        // handle named, so the record carries the pointer and the context rather
        // than re-deriving a static address (LRM 10.4.2). Deliberately does NOT
        // set `nba`: the member's plain NBAs take the pointer-carrying array
        // queue for the same reason, so there is no block shadow to commit from.
        s.intra_ptr    = true;
        s.intra_masked = s.intra_masked || !t.whole;
    } else {
        s.nba          = true;
        s.intra_masked = s.intra_masked || !t.whole;
    }
    if (std::find(intra_nba_sigs_.begin(), intra_nba_sigs_.end(), t.sig) == intra_nba_sigs_.end()) {
        intra_nba_sigs_.push_back(t.sig);
    }
}

// Classify the plain NBAs to signals that also take timed ones. Such a write copies
// itself onto the ready list (emit_intra_nba_ordered), so the record has to be able to
// encode its shape: classifying it as an intra site too sets the flags the record
// type is built from (`intra_masked`, `intra_ptr`). The set of intra targets is
// complete only after every body has been scanned, hence a second pass.
void Backend::classify_ordered_plain(const Ir& ir) {
    if (intra_nba_sigs_.empty()) {
        return;
    }
    for (size_t k = 0; k < ir.inst_count(); ++k) {
        const auto idx = static_cast<Index>(k);
        if (ir.tag(idx) != Inst::Tag::StoreNb) {
            continue;
        }
        const Index     lv_idx = ir.payload(idx).binary.l;
        const NbaTarget t      = nba_target(ir, lv_idx);
        // A string place stages an owned handle, which the record does not hold; its
        // writes take the string path and are not copied.
        if (is_intra_target(t.sig) && !lvalue_is_string_place(ir, lv_idx)) {
            classify_intra_target(ir, lv_idx, t);
        }
    }
}

// Mark NBA-target signals reached by one IR body (a process or a task): set
// `nba`, `nba_partial` (any bit/part-select site), `nba_dyn` (any runtime-indexed
// select), and collect unpacked-array targets for the runtime deferred queue.
void Backend::nba_classify_ir(const Ir& ir) {
    const auto sigs = mod_.signals();
    for (size_t k = 0; k < ir.inst_count(); ++k) {
        const auto idx = static_cast<Index>(k);
        // A nonblocking intra-assignment delay (`x <= #d y`) or an inertial
        // continuous-assignment delay (`assign #d x = y`, LRM 10.3.3). The
        // value is held in a deferred record on the wheel, not the shadow, so
        // the target is tracked separately for the intra-commit loop;
        // classify_intra_target also decides the record's shape.
        if (ir.tag(idx) == Inst::Tag::StoreNbDelay ||
            ir.tag(idx) == Inst::Tag::StoreInertialDelay) {
            const NbaTarget t = nba_target(ir, ir.payload(idx).op_extra.op);
            classify_intra_target(ir, ir.payload(idx).op_extra.op, t);
            if (ir.tag(idx) == Inst::Tag::StoreInertialDelay &&
                std::find(inertial_sigs_.begin(), inertial_sigs_.end(), t.sig) ==
                  inertial_sigs_.end()) {
                inertial_sigs_.push_back(t.sig);
            }
            continue;
        }
        // A nonblocking intra-assignment event (`x <= @e y`): the target is an
        // intra target (deferred record + NBA-region commit), and the trigger
        // (signal, edge) gets a dense id so both wake paths can fire its records.
        if (ir.tag(idx) == Inst::Tag::StoreNbEvent) {
            const NbaTarget t = nba_target(ir, ir.payload(idx).op_extra.op);
            classify_intra_target(ir, ir.payload(idx).op_extra.op, t);
            // Every LISTED trigger needs its own dense id: `@(a or b)` puts the
            // record on both lists, and signal_has_waiters / has_bucket key edge
            // detection off `trigger_id(sig, e) >= 0`, so a listed signal that
            // nothing else is sensitive to still has to be watched.
            const auto trigs = ir.intra_trigs(ir.payload(idx).op_extra.extra);
            for (const IntraTrigger& tr : trigs) {
                if (mod_.signals()[static_cast<uint32_t>(tr.signal)].is_event) {
                    // Waits on the event OBJECT (trigger_id_value), so it takes no
                    // dense id and needs no signal-edge detection.
                    design_uses_event_triggers_   = true;
                    design_uses_intra_event_objs_ = true;
                    continue;
                }
                intra_trigger_edge_[static_cast<uint32_t>(tr.signal)][tr.edge] = true;
                int32_t& tid = trigger_id_[static_cast<uint32_t>(tr.signal)][tr.edge];
                if (tid < 0) {
                    tid = static_cast<int32_t>(n_triggers_++);
                }
            }
            SignalSlot& ts = slots_[static_cast<uint32_t>(t.sig)];
            ts.intra_trigs = std::max(ts.intra_trigs, static_cast<uint32_t>(trigs.size()));
            continue;
        }
        if (ir.tag(idx) == Inst::Tag::EventTriggerNbEvent) {
            // `->> @(x) e` needs a dense trigger id on the watched (signal,
            // edge) -- the same id space the intra-assignment event mechanism
            // uses -- so the wake can find its registrations.
            const auto info = ir.extra_data<EventDetachInfo>(ir.payload(idx).op_extra.extra);
            if (mod_.signals()[static_cast<uint32_t>(info.signal)].is_event) {
                design_uses_event_triggers_ = true;
            } else {
                int32_t& tid = trigger_id_[static_cast<uint32_t>(info.signal)][info.edge];
                if (tid < 0) {
                    tid = static_cast<int32_t>(n_triggers_++);
                }
            }
            design_uses_detached_events_ = true;
            design_uses_nb_events_       = true; // it pends into the NBA region
            continue;
        }
        if (ir.tag(idx) == Inst::Tag::EventTriggerNb) {
            // `->> e` defers its wake to the NBA region. The target is a handle,
            // so there is no compile-time event to record: the region drains a
            // runtime pending chain instead, and only needs to know the design
            // contains at least one such trigger.
            design_uses_nb_events_ = true;
            continue;
        }
        if (ir.tag(idx) == Inst::Tag::EventTriggerNbDelayed) {
            // `->> #d e` arms a wheel timer for a positive delay, but a zero one
            // (a literal `#0`, or a runtime value that is 0 or x/z, LRM 9.4.1)
            // pends the event like `->> e`, so the NBA region needs its chain. The
            // delay reaches here as arithmetic over a constant, not a constant
            // itself, so this cannot tell a nonzero literal from a runtime value;
            // the cost is one pointer test per NBA check in such a design.
            design_uses_nb_events_ = true;
            continue;
        }
        // A whole-container nonblocking assignment (`q <= rhs`, `d <= rhs`,
        // `a <= rhs` -- its own op): the target signal defers a staged deep copy
        // into a per-signal buffer, moved into the live handle in the NBA region.
        if (ir.tag(idx) == Inst::Tag::HandleCopyNb) {
            const NbaTarget t = nba_target(ir, ir.payload(idx).op_extra.op);
            if (std::find(handle_nba_sigs_.begin(), handle_nba_sigs_.end(), t.sig) ==
                handle_nba_sigs_.end()) {
                handle_nba_sigs_.push_back(t.sig);
                // The CONTAINER's type, taken from the op the way staging does.
                // Deriving it from the signal instead does not work: the signal
                // may be a fixed array OF containers, or a struct with a
                // container member, and then its type is the array or the
                // struct. The commit sizes its record and picks its move
                // function from this, so a mismatch reads a queue's 32 bytes
                // out of an 8-byte assoc handle.
                handle_nba_types_.push_back(
                  ir.extra_data<QueueArg>(ir.payload(idx).op_extra.extra).queue_type);
            }
            continue;
        }
        if (ir.tag(idx) != Inst::Tag::StoreNb) {
            continue;
        }
        const Index     lv_idx = ir.payload(idx).binary.l;
        const NbaTarget t      = nba_target(ir, lv_idx);
        // An NBA to any string PLACE — a whole string signal, a string member of
        // an unpacked struct, a string element of a fixed array — takes its own
        // record range: the staged value is an owned rt_string, which neither the
        // plane shadow nor the byte-image array / aggregate records can hold. The
        // record carries the leaf's destination pointer, so one range per ROOT
        // signal serves every leaf of it. Must precede the aggregate and array
        // branches below, which would otherwise claim the same store.
        if (lvalue_is_string_place(ir, lv_idx)) {
            if (std::find(string_nba_sigs_.begin(), string_nba_sigs_.end(), t.sig) == string_nba_sigs_.end()) {
                string_nba_sigs_.push_back(t.sig);
            }
            continue;
        }
        // Nonblocking assignment into an aggregate. A STATIC-path leaf (struct
        // fields only) commits through the per-leaf aggregate shadow (block
        // commit). A leaf reached through a runtime array index has no
        // compile-time address, so it defers a (dest-slot-pointer, value)
        // record replayed by memcpy — the aggregate analogue of the array
        // deferred queue. A signal can take both paths (different sites).
        if (contains_struct(sigs[static_cast<uint32_t>(t.sig)].type)) {
            SignalSlot& s       = slots_[static_cast<uint32_t>(t.sig)];
            const bool  runtime = lvalue_has_elem_select(ir, lv_idx);
            const bool  partial = lvalue_has_subselect(ir, lv_idx);
            s.nba               = true;
            // A static whole-leaf write commits from the block shadow; a
            // runtime array index OR a partial (bit/part-select) leaf write
            // defers (the partial one needs a masked read-modify-write).
            if (runtime || partial) {
                if (std::find(agg_nba_sigs_.begin(), agg_nba_sigs_.end(), t.sig) == agg_nba_sigs_.end()) {
                    agg_nba_sigs_.push_back(t.sig);
                }
                s.agg_masked = s.agg_masked || partial;
            } else {
                s.nba_agg = true;
            }
            continue;
        }
        // A member some handle nonblocking-writes takes the same deferred
        // queue, for the same reason an array element does: the destination is
        // not a compile-time address. Every writer routes here, including the
        // interface's own and a module port's, so the instant's writes commit
        // in execution order rather than split across two phases.
        if (slots_[static_cast<uint32_t>(t.sig)].vif_rec) {
            if (std::find(array_nba_sigs_.begin(), array_nba_sigs_.end(), t.sig) ==
                array_nba_sigs_.end()) {
                array_nba_sigs_.push_back(t.sig);
            }
            if (lvalue_has_subselect(ir, lv_idx)) {
                slots_[static_cast<uint32_t>(t.sig)].arr_masked = true;
            }
            continue;
        }
        // An unpacked-array element NBA uses the runtime deferred queue, not
        // the per-signal shadow; record the array signal for wake servicing.
        // (slot.is_array is not set until the placement loop below, so test
        // the type directly here.)
        if (std::get<Type>(mod_.pool.get(sigs[static_cast<uint32_t>(t.sig)].type)).kind ==
            Type::Kind::UnpackedArray) {
            if (std::find(array_nba_sigs_.begin(), array_nba_sigs_.end(), t.sig) == array_nba_sigs_.end()) {
                array_nba_sigs_.push_back(t.sig);
            }
            // A within-element bit/part-select (below the element index) means
            // this array's deferred records must carry a write mask.
            if (lvalue_has_subselect(ir, lv_idx)) {
                slots_[static_cast<uint32_t>(t.sig)].arr_masked = true;
            }
            continue;
        }
        SignalSlot& s = slots_[static_cast<uint32_t>(t.sig)];
        s.nba         = true;
        s.nba_partial = s.nba_partial || !t.whole;
        s.nba_dyn     = s.nba_dyn || (!t.whole && lvalue_has_dynamic_select(ir, lv_idx));
    }
}

// Find every NBA-target signal across all processes and tasks and mark its slot:
// `nba`, `nba_partial`, and a dense `nba_id` (signal order). A task NBA stages
// into the same per-signal shadow and commits in its caller's NBA region, so its
// targets must be classified here too. Bucket assignment happens later in
// build_nba_state once widths are final.
void Backend::nba_classify() {
    trigger_id_.assign(mod_.signals().size(), { -1, -1, -1 });
    intra_trigger_edge_.assign(mod_.signals().size(), { false, false, false });
    for (const Process& p : mod_.processes()) {
        nba_classify_ir(p.ir);
    }
    // Scan every subroutine body: a nonblocking assignment to a module signal is
    // legal in both a task and a function (LRM 13.4.4), and either stages into the
    // signal's shadow. A subroutine with no NBA records nothing.
    for (uint32_t i = 0; i < mod_.function_count(); ++i) {
        nba_classify_ir(mod_.function(static_cast<FunctionId>(i)).ir);
    }
    for (const Process& p : mod_.processes()) {
        classify_ordered_plain(p.ir);
    }
    for (uint32_t i = 0; i < mod_.function_count(); ++i) {
        classify_ordered_plain(mod_.function(static_cast<FunctionId>(i)).ir);
    }
    n_nba_          = 0;
    const auto sigs = mod_.signals();
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        if (slots_[i].nba) {
            slots_[i].nba_id = n_nba_++;
        }
    }
}

// Build the nonblocking-assignment update state: a constant per-signal metadata
// table (live/shadow/mask offsets plus the wait-queue base/cap/len pointers the
// block commit services from). Must run after build_state_layout (slot offsets,
// nba flags) and build_wait_queues (queue globals).
void Backend::build_nba_state() {
    auto* i32   = llvm::Type::getInt32Ty(ctx_);
    auto* ptr   = llvm::PointerType::getUnqual(ctx_);
    auto* arr3p = llvm::ArrayType::get(ptr, 3);
    auto* arr3i = llvm::ArrayType::get(i32, 3);
    nba_meta_ty_ = llvm::StructType::get(ctx_, { arr3p, arr3i, arr3p, arr3p });

    const auto sigs = mod_.signals();

    // Every scalar (non-array) NBA target commits through the block-commit path.
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        SignalSlot& s = slots_[i];
        if (s.nba) {
            s.nba_block = !s.is_array;
        }
    }

    // Constant metadata, one record per nba-id (signal order, matching
    // nba_id). The commit loop walks state with runtime byte offsets read
    // as the per-(signal, edge) wait-queue base/cap/len pointers the block commit
    // hands to emit_nba_service. Live/shadow addressing uses compile-time offsets
    // (storage_ptr / shadow_ptr), so no byte offsets are baked here.
    auto*                        nullp = llvm::ConstantPointerNull::get(ptr);
    std::vector<llvm::Constant*> metas;
    // Parallel to `metas` (nba_id order): the dump-var index of each NBA-target
    // signal, or -1 when it is not a dumped scalar. Read in the commit loop to
    // mark the signal in the VCD dirty set.
    std::vector<llvm::Constant*> nba_vcd;
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        const SignalSlot& s = slots_[i];
        if (!s.nba) {
            continue;
        }
        // Parallel to metas: this NBA target's dump-var index, or -1 when it is
        // not a dumped scalar. The commit loop reads it to mark the signal dirty.
        nba_vcd.push_back(llvm::ConstantInt::get(
          i32, static_cast<uint64_t>(static_cast<int64_t>(design_uses_vcd_ ? vcd_index_[i] : -1))));
        std::array<llvm::Constant*, 3> sb{}, sc{}, db{}, dl{};
        for (uint32_t e = 0; e < 3; ++e) {
            sb[e] = wait_sta_gv_[i][e] ? llvm::cast<llvm::Constant>(sym(wait_sta_gv_[i][e])) : nullp;
            sc[e] = llvm::ConstantInt::get(i32, wait_sta_true_[i][e]);
            db[e] = wait_dyn_gv_[i][e] ? llvm::cast<llvm::Constant>(sym(wait_dyn_gv_[i][e])) : nullp;
            dl[e] = wait_dyn_len_gv_[i][e] ? llvm::cast<llvm::Constant>(sym(wait_dyn_len_gv_[i][e])) : nullp;
        }
        metas.push_back(llvm::ConstantStruct::get(
          nba_meta_ty_, { llvm::ConstantArray::get(arr3p, sb), llvm::ConstantArray::get(arr3i, sc),
                          llvm::ConstantArray::get(arr3p, db), llvm::ConstantArray::get(arr3p, dl) }));
    }
    if (metas.empty()) {
        metas.push_back(llvm::ConstantAggregateZero::get(nba_meta_ty_));
    }
    nba_meta_arr_ty_ = llvm::ArrayType::get(nba_meta_ty_, metas.size());
    nba_meta_gv_     = new llvm::GlobalVariable(env_->llmod, nba_meta_arr_ty_, true, llvm::GlobalValue::InternalLinkage,
                                                llvm::ConstantArray::get(nba_meta_arr_ty_, metas), "clotho.nba_meta");

    // Parallel dump-var index array (nba_id order), consumed by the commit loop
    // to mark committed signals dirty in the VCD dirty set.
    if (design_uses_vcd_ && !nba_vcd.empty()) {
        auto* ty    = llvm::ArrayType::get(i32, nba_vcd.size());
        nba_vcd_gv_ = new llvm::GlobalVariable(env_->llmod, ty, true, llvm::GlobalValue::InternalLinkage,
                                               llvm::ConstantArray::get(ty, nba_vcd), "clotho.nba_vcd");
    }

    // Declare the block-commit enqueue stub up front so StoreNb lowering can call
    // it (its body + the worklist/pending globals are emitted post-lowering by
    // finalize_block_nba, once the block count is known). Declared only when some
    // signal actually takes the block path.
    const bool any_block = std::any_of(slots_.begin(), slots_.end(), [](const SignalSlot& s) { return s.nba_block; });
    if (any_block) {
        auto* fnty = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), { i32 }, false);
        nba_block_enqueue_fn_ =
          llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.nba_block_enqueue", env_->llmod);
    }
}

// Register the current straight-line block (the live insert basic block) as the
// owner of a whole-signal scalar NBA to `sig`. The first whole-scalar StoreNb in
// a block emits one enqueue call (deduped at runtime by block_pending); every
// target in the block is gathered into the block's commit function. Keyed on the
// LLVM insert block, so back-to-back fused members that share one block coalesce
// into a single enqueue + commit.
void Backend::block_register(Signal sig, const llvm::APInt& mask, bool dynamic) {
    // Open a new block on the first NBA of a segment (the enqueue call is emitted
    // once, here, at that first store's insert point; it runs unconditionally
    // because same-epoch stores are straight-line co-executed).
    if (cur_block_epoch_ != static_cast<int64_t>(cur_seg_epoch_)) {
        cur_block_id_    = static_cast<uint32_t>(block_commits_.size());
        block_commits_.push_back({});
        cur_block_epoch_ = static_cast<int64_t>(cur_seg_epoch_);
        llvm::Value* id  = b_.getInt32(cur_block_id_);
        if (sched_anchor_ != SignalSlot::no_anchor) {
            Anchor&        a     = anchors_[sched_anchor_];
            const uint32_t field = a.ctx_blk_base + group_def_ordinal_[sched_group_];
            if (group_first_block_ < 0) {
                group_first_block_ = static_cast<int64_t>(cur_block_id_);
                ctx_blk_fills_.emplace_back(sched_anchor_, field, cur_block_id_);
            }
            id = b_.CreateAdd(
              ctx_field(a, field, b_.getInt32Ty()),
              b_.getInt32(cur_block_id_ - static_cast<uint32_t>(group_first_block_)));
        } else if (config_.split && unit_emission()) {
            // Unit-module code of a unique definition: the block id via a
            // root-defined value global keyed by (instance, def-local
            // ordinal) — the owning group's, or the member's own when an
            // outlined cross-instance member is lowering — with the
            // group-/member-local block ordinal as the addend.
            if (group_first_block_ < 0) {
                group_first_block_ = static_cast<int64_t>(cur_block_id_);
            }
            const std::string key =
              outline_anchor_ != ~0u
                ? "blk." + anchors_[outline_anchor_].name + ".p" + std::to_string(outline_ord_)
                : "blk." + anchors_[group_unit_anchor_[sched_group_]].name + ".g" +
                    std::to_string(group_unit_ordinal_[sched_group_]);
            id = abs_id_const(key, static_cast<uint32_t>(group_first_block_),
                              cur_block_id_ - static_cast<uint32_t>(group_first_block_));
        }
        b_.CreateCall(sym(nba_block_enqueue_fn_), { id });
    }
    std::vector<BlockSig>& sigs = block_commits_[cur_block_id_].sigs;
    auto it = std::find_if(sigs.begin(), sigs.end(), [&](const BlockSig& b) { return b.sig == sig; });
    if (it == sigs.end()) {
        sigs.push_back({ sig, mask, dynamic });
    } else {
        it->dynamic = it->dynamic || dynamic;
        if (!dynamic) {
            it->mask |= mask; // accumulate this block's static writes to the signal
        }
    }
}

// Sentinel stored in block_next[id] when block id is not enqueued (doubles as
// the dedup marker); distinct from null, which marks the chain end.
llvm::Constant* Backend::block_not_queued() {
    return llvm::ConstantExpr::getIntToPtr(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1),
                                           llvm::PointerType::getUnqual(ctx_));
}

// Threaded-dispatch tail for block `id`'s commit function, emitted at its single
// exit block after the copy/service work: reset this block's link to NOT_QUEUED,
// then musttail-call the next pending block (or ret at the chain end).
void Backend::emit_block_commit_tail(uint32_t id) {
    auto*        ptr   = llvm::PointerType::getUnqual(ctx_);
    auto*        nullp = llvm::ConstantPointerNull::get(ptr);
    llvm::Value* nextp = b_.CreateInBoundsGEP(block_next_ty_, sym(block_next_gv_), { b_.getInt32(0), b_.getInt32(id) });
    llvm::Value* next  = b_.CreateLoad(ptr, nextp);
    b_.CreateStore(block_not_queued(), nextp); // reset self for next instant
    auto* fn     = b_.GetInsertBlock()->getParent();
    auto* tailbb = llvm::BasicBlock::Create(ctx_, "thr.tail", fn);
    auto* retbb  = llvm::BasicBlock::Create(ctx_, "thr.ret", fn);
    b_.CreateCondBr(b_.CreateICmpEQ(next, nullp), retbb, tailbb);

    b_.SetInsertPoint(tailbb);
    auto* cty  = llvm::FunctionType::get(b_.getVoidTy(), { ptr }, false);
    auto* call = b_.CreateCall(cty, next, { state_ });
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    b_.CreateRetVoid();

    b_.SetInsertPoint(retbb);
    b_.CreateRetVoid();
}

// Emit block `id`'s commit function `void(state)`: one straight-line copy body per
// whole-signal scalar target the block writes (shadow planes -> live state) and,
// where the target has waiters, service its wait queues; then the threaded tail.
void Backend::emit_block_commit_fn(uint32_t id) {
    auto* voidty = llvm::Type::getVoidTy(ctx_);
    auto* ptr    = llvm::PointerType::getUnqual(ctx_);
    auto* fnty   = llvm::FunctionType::get(voidty, { ptr }, false);
    auto* fn     = llvm::Function::Create(fnty, llvm::Function::InternalLinkage,
                                          "clotho.nba.commit_blk" + std::to_string(id), env_->llmod);
    block_commits_[id].fn = fn;

    // Save/restore the builder + current-state pointer: storage_ptr / shadow_ptr
    // read through `state`, emit_nba_service builds control flow at the insert point.
    llvm::IRBuilderBase::InsertPoint saved       = b_.saveIP();
    llvm::Value*                     saved_state = state_;
    state_      = fn->getArg(0);
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
    b_.SetInsertPoint(entry);
    emit_block_commit_body(id);
    emit_block_commit_tail(id);
    b_.restoreIP(saved);
    state_ = saved_state;
}

// Block `id`'s commit body: one straight-line copy per signal (no loop), using
// compile-time slot/shadow offsets. Emits into the current insert point (state_
// already set to the commit fn's arg). No-waiter targets skip the edge compute +
// wait-queue scan entirely.
void Backend::emit_block_commit_body(uint32_t id) {
    for (const BlockSig& bs : block_commits_[id].sigs) {
        const SignalSlot& s       = slot_of(bs.sig);
        if (s.nba_agg) {
            emit_agg_commit(bs.sig, bs.mask);
            continue;
        }
        const uint32_t    W       = s.width;
        auto*             mmty     = int_ty(s.four_state ? 2 * W : W);
        auto*             zty      = int_ty(W);
        llvm::Value*      live     = storage_ptr(s);
        llvm::Value*      live_mm  = b_.CreateStructGEP(s.slot_ty, live, 0);
        llvm::Value*      sh_mm    = shadow_ptr(s, s.sh_mm_field);
        llvm::Value*      live_z   = s.has_z ? b_.CreateStructGEP(s.slot_ty, live, 1) : nullptr;
        llvm::Value*      sh_z     = s.has_z ? shadow_ptr(s, s.sh_z_field) : nullptr;
        llvm::Value*      old_mm   = b_.CreateLoad(mmty, live_mm);
        llvm::Value*      new_mm   = nullptr;
        llvm::Value*      old_z    = nullptr;
        llvm::Value*      new_z    = nullptr;

        if (!bs.dynamic && bs.mask.isAllOnes()) {
            // Whole-signal (or full-width) target: copy the planes directly.
            new_mm = b_.CreateLoad(mmty, sh_mm);
            b_.CreateStore(new_mm, live_mm);
            if (s.has_z) {
                old_z = b_.CreateLoad(zty, live_z);
                new_z = b_.CreateLoad(zty, sh_z);
                b_.CreateStore(new_z, live_z);
            }
        } else {
            // Masked RMW: mask is a compile-time immediate for a static-select
            // target, or the runtime dirty field for a dynamic-select one.
            llvm::Value* maskw = bs.dynamic
                                   ? static_cast<llvm::Value*>(b_.CreateLoad(zty, shadow_ptr(s, s.sh_mask_field)))
                                   : static_cast<llvm::Value*>(llvm::ConstantInt::get(zty, bs.mask));
            // A 2-state (iW) word masks directly; a packed (i2W) word replicates
            // the W-bit mask into both must|may halves.
            llvm::Value* mm_mask = !s.four_state ? maskw
                                                 : b_.CreateOr(b_.CreateZExt(maskw, mmty),
                                                               b_.CreateShl(b_.CreateZExt(maskw, mmty),
                                                                            llvm::ConstantInt::get(mmty, W)));
            llvm::Value* sh_mm_v = b_.CreateLoad(mmty, sh_mm);
            new_mm = b_.CreateOr(b_.CreateAnd(old_mm, b_.CreateNot(mm_mask)), b_.CreateAnd(sh_mm_v, mm_mask));
            b_.CreateStore(new_mm, live_mm);
            if (s.has_z) {
                old_z                = b_.CreateLoad(zty, live_z);
                llvm::Value* sh_z_v  = b_.CreateLoad(zty, sh_z);
                new_z = b_.CreateOr(b_.CreateAnd(old_z, b_.CreateNot(maskw)), b_.CreateAnd(sh_z_v, maskw));
                b_.CreateStore(new_z, live_z);
            }
            if (bs.dynamic) {
                b_.CreateStore(zero(W), shadow_ptr(s, s.sh_mask_field)); // clear for next instant
            }
        }
        if (signal_has_waiters(bs.sig)) {
            llvm::Value* meta = b_.CreateInBoundsGEP(nba_meta_arr_ty_, sym(nba_meta_gv_),
                                                     { b_.getInt32(0), b_.getInt32(s.nba_id) });
            emit_nba_service(bs.sig, meta, old_mm, new_mm, W, s.four_state, old_z, new_z);
        }
        // Mark a dumped scalar target dirty in the VCD set (rt_vcd_touch ignores
        // the -1 index of a non-dumped signal). Commit is unconditional, so a
        // no-change commit over-touches; the flush diff suppresses it.
        if (design_uses_vcd_ && nba_vcd_gv_ != nullptr) {
            emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
                return llvm::SmallVector<llvm::Value*, 2>{ b_.CreateLoad(
                  b_.getInt32Ty(), b_.CreateInBoundsGEP(nba_vcd_gv_->getValueType(), sym(nba_vcd_gv_),
                                                        { b_.getInt32(0), b_.getInt32(s.nba_id) })) };
            });
        }
    }
}

// Pointer to an aggregate leaf's mm (0) / z (1) field, via the leaf's static
// constant GEP path from the aggregate storage root.
llvm::Value* Backend::agg_leaf_ptr(llvm::Value*                 base,
                                   llvm::Type*                  root_ty,
                                   const std::vector<uint32_t>& path,
                                   uint32_t                     field) {
    std::vector<llvm::Value*> idx;
    idx.reserve(path.size() + 2);
    idx.push_back(b_.getInt32(0));
    for (const uint32_t f : path) {
        idx.push_back(b_.getInt32(f));
    }
    idx.push_back(b_.getInt32(field));
    return b_.CreateInBoundsGEP(root_ty, base, idx);
}

// Commit an aggregate NBA target within a block: copy each static leaf whose
// leaf-set bit is set from its shadow slot to live storage, then wake level
// (Any-edge) readers if any leaf changed. A leaf is a whole slot (not a bit
// range), so this copies planes, not masked words; edge sensitivity on an
// aggregate member is rejected in the frontend, so Any is the only bucket.
void Backend::emit_agg_commit(Signal sig, const llvm::APInt& mask) {
    const SignalSlot& s       = slot_of(sig);
    llvm::Value*      live    = storage_ptr(s);
    llvm::Value*      changed = b_.getFalse();
    for (uint32_t k = 0; k < s.static_leaves.size(); ++k) {
        if (!mask[k]) {
            continue;
        }
        const AggLeaf& m       = s.static_leaves[k];
        auto*          mmty    = int_ty(m.four_state ? 2 * m.width : m.width);
        llvm::Value*   live_mm = agg_leaf_ptr(live, s.storage_ty, m.path, 0);
        llvm::Value*   old_mm  = b_.CreateLoad(mmty, live_mm);
        llvm::Value*   new_mm  = b_.CreateLoad(mmty, shadow_ptr(s, m.sh_mm_field));
        b_.CreateStore(new_mm, live_mm);
        changed = b_.CreateOr(changed, b_.CreateICmpNE(old_mm, new_mm));
        if (m.has_z) {
            auto*        zty    = int_ty(m.width);
            llvm::Value* live_z = agg_leaf_ptr(live, s.storage_ty, m.path, 1);
            llvm::Value* old_z  = b_.CreateLoad(zty, live_z);
            llvm::Value* new_z  = b_.CreateLoad(zty, shadow_ptr(s, m.sh_z_field));
            b_.CreateStore(new_z, live_z);
            changed = b_.CreateOr(changed, b_.CreateICmpNE(old_z, new_z));
        }
    }
    if (signal_has_waiters(sig)) {
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* svc  = llvm::BasicBlock::Create(ctx_, "uagg.svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "uagg.cont", fn);
        b_.CreateCondBr(changed, svc, cont);
        b_.SetInsertPoint(svc);
        service_any_waiters(sig);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// Finalize the block-commit path after all bodies are lowered: size the
// threaded-dispatch globals to the discovered block count, emit each block's
// commit function, and emit the enqueue stub body. No-op when the path was never
// used.
void Backend::finalize_block_nba() {
    sched_anchor_ = SignalSlot::no_anchor; // commit machinery addresses absolutely
    if (nba_block_enqueue_fn_ == nullptr) {
        return;
    }
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    // A declared-but-unused stub (no block ever registered) still needs a body to
    // verify; it is never called.
    const uint32_t n = std::max<uint32_t>(static_cast<uint32_t>(block_commits_.size()), 1);

    // Threaded-dispatch globals. Must exist before the commit-fn loop below,
    // whose emitted tails reference block_next. Links init to NOT_QUEUED, head
    // to null (empty chain).
    block_next_ty_ = llvm::ArrayType::get(ptr, n);
    block_next_gv_ = new llvm::GlobalVariable(
      env_->llmod, block_next_ty_, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantArray::get(block_next_ty_, std::vector<llvm::Constant*>(n, block_not_queued())),
      "clotho.nba_block_next");
    block_head_gv_ = new llvm::GlobalVariable(env_->llmod, ptr, false, llvm::GlobalValue::InternalLinkage,
                                              llvm::ConstantPointerNull::get(ptr), "clotho.nba_block_head");

    std::vector<llvm::Constant*> fns;
    for (uint32_t id = 0; id < block_commits_.size(); ++id) {
        emit_block_commit_fn(id);
        fns.push_back(sym(block_commits_[id].fn));
    }
    if (fns.empty()) {
        fns.push_back(llvm::ConstantPointerNull::get(ptr));
    }
    block_commit_fns_ty_ = llvm::ArrayType::get(ptr, fns.size());
    block_commit_fns_gv_ = new llvm::GlobalVariable(env_->llmod, block_commit_fns_ty_, true,
                                                    llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantArray::get(block_commit_fns_ty_, fns),
                                                    "clotho.nba_block_commit_fns");

    // Enqueue stub body: if block id is not queued (block_next[id] == NOT_QUEUED),
    // push it on the intrusive chain (block_next[id] = block_head;
    // block_head = commit_fns[id]); else it is already pending, so do nothing.
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", nba_block_enqueue_fn_);
    auto* push  = llvm::BasicBlock::Create(ctx_, "push", nba_block_enqueue_fn_);
    auto* done  = llvm::BasicBlock::Create(ctx_, "done", nba_block_enqueue_fn_);
    llvm::IRBuilder<> tb(entry);
    llvm::Value*      id    = nba_block_enqueue_fn_->getArg(0);
    llvm::Value*      nextp = tb.CreateInBoundsGEP(block_next_ty_, sym(block_next_gv_), { tb.getInt32(0), id });
    tb.CreateCondBr(tb.CreateICmpEQ(tb.CreateLoad(ptr, nextp), block_not_queued()), push, done);
    tb.SetInsertPoint(push);
    tb.CreateStore(tb.CreateLoad(ptr, sym(block_head_gv_)), nextp);
    llvm::Value* fn = tb.CreateLoad(
      ptr, tb.CreateInBoundsGEP(block_commit_fns_ty_, sym(block_commit_fns_gv_), { tb.getInt32(0), id }));
    tb.CreateStore(fn, sym(block_head_gv_));
    tb.CreateBr(done);
    tb.SetInsertPoint(done);
    tb.CreateRetVoid();
}

// Service an NBA target's wait queues after its shadow committed to live. Same
// edge facts as service_wait_queue, but the per-edge queue pointers come from
// the meta record (runtime) so this drives any signal with one body. Which
// per-edge queues are populated is not known at compile time here (the commit
// loop is shared per bucket), so all three facts are computed. `width` is the
// bucket's static width; old_z / new_z are null for a never-z bucket. A
// 2-state bucket's word is a single iW with no may plane, so it must take the
// plain 0<->1 classifier — deriving `may` via LShr on the iW word would read
// 0 and suppress every posedge.
void Backend::emit_nba_service(Signal       sig,
                               llvm::Value* meta,
                               llvm::Value* old_mm,
                               llvm::Value* new_mm,
                               uint32_t     width,
                               bool         four_state,
                               llvm::Value* old_z,
                               llvm::Value* new_z) {
    const EdgeFacts f = emit_edge_facts(old_mm, new_mm, width, four_state, old_z,
                                        new_z, /*need_pos=*/true, /*need_neg=*/true,
                                        /*need_any=*/true);
    // One change is one release moment for the `<= @` records it matures, however many
    // of the buckets below it satisfies.
    emit_release_moment(sig, f);
    const std::array<std::pair<uint32_t, llvm::Value*>, 3> buckets = {
        std::make_pair(static_cast<uint32_t>(Edge::Any), f.changed),
        std::make_pair(static_cast<uint32_t>(Edge::Positive), f.pos),
        std::make_pair(static_cast<uint32_t>(Edge::Negative), f.neg),
    };
    for (const auto& [e, fact] : buckets) {
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* body = llvm::BasicBlock::Create(ctx_, "nsvc.body", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "nsvc.cont", fn);
        b_.CreateCondBr(fact, body, cont);
        b_.SetInsertPoint(body);
        auto* ptr = raw_ptr_ty();
        auto* i32 = b_.getInt32Ty();
        emit_static_scan_rt(meta_at(meta, 0, e, ptr), meta_at(meta, 1, e, i32), false,
                            trig_bits(sig, e));
        emit_dynamic_scan_rt(meta_at(meta, 2, e, ptr), meta_at(meta, 3, e, ptr));
        if (vif_wait_[static_cast<uint32_t>(sig)][e]) {
            // Waiters that reached this member through a virtual interface. The
            // meta table carries only the bucket bases, so the list is addressed
            // directly -- `sig` is compile-time here, and the commit runs in the
            // owning instance's own code (a handle-written target commits
            // through the same block as any other write to it).
            emit_event_wake_list(vif_head_ptr(sig, e));
        }
        // An NBA commit to a `<= @e` trigger fires its records too (the other
        // wake path is a blocking store, in service_wait_queue).
        fire_event_trigger(sig, e);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// Whether any nonblocking update is staged: the block-commit chain head, the
// shared array / aggregate / queue record buffers, the intra-assignment
// record pools, and each `->> e` pending flag.
//
// Every one of these is CONSUMED by emit_nba_commit, so this test re-arms:
// asking it again later in the same instant reports only what was staged
// since the last commit. That is what lets the Re-NBA region reuse both
// halves after the NBA region has already run and cleared them.
llvm::Value* Backend::emit_nba_any_staged() {
    auto*        ptr_ty = llvm::PointerType::getUnqual(ctx_);
    llvm::Value* any    = b_.getFalse();
    if (!array_nba_sigs_.empty() || !agg_nba_sigs_.empty() || !handle_nba_sigs_.empty() ||
        !string_nba_sigs_.empty()) {
        // rt_nba_arr_any is the total across the shared array + aggregate +
        // queue + string buffers.
        any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateCall(nba_arr_any_fn(), {}), b_.getInt32(0)));
    }
    if (!block_commits_.empty()) {
        any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateLoad(ptr_ty, sym(block_head_gv_)),
                                               llvm::ConstantPointerNull::get(ptr_ty)));
    }
    if (!intra_nba_sigs_.empty()) {
        any = b_.CreateOr(any, b_.CreateICmpNE(b_.CreateCall(intra_any_fn(), {}), b_.getInt32(0)));
    }
    if (design_uses_nb_events_) {
        // Any `->> e` staged this instant: the pending chain is non-empty.
        llvm::Value* head = b_.CreateLoad(llvm::PointerType::getUnqual(ctx_), sym(event_pend_head_gv_));
        any = b_.CreateOr(
          any, b_.CreateICmpNE(head, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_))));
    }
    return any;
}

// Commit every staged nonblocking update at the builder's insert point. The
// emitted code is left unterminated, so the caller supplies the branch: the
// NBA region loops back to the active drain, the Re-NBA region to the
// reactive one.
//
// Position-independent by construction -- every block this creates hangs off
// b_.GetInsertBlock()->getParent(), as do the four commit helpers it calls --
// which is what makes a second copy after the Reactive region possible.
void Backend::emit_nba_commit(llvm::Value* state, llvm::Value* wheel) {
    auto*      ptr_ty   = llvm::PointerType::getUnqual(ctx_);
    const bool uses_vcd = design_uses_vcd();
    // Block-commit scalar NBAs: call the head block's commit function; it
    // musttail-chains through every pending block and returns here.
    if (!block_commits_.empty()) {
        auto*        cfn    = b_.GetInsertBlock()->getParent();
        auto*        run    = llvm::BasicBlock::Create(ctx_, "nblk.run", cfn);
        auto*        after  = llvm::BasicBlock::Create(ctx_, "nblk.after", cfn);
        llvm::Value* h      = b_.CreateLoad(ptr_ty, sym(block_head_gv_));
        b_.CreateCondBr(b_.CreateICmpNE(h, llvm::ConstantPointerNull::get(ptr_ty)), run, after);
        b_.SetInsertPoint(run);
        b_.CreateStore(llvm::ConstantPointerNull::get(ptr_ty), sym(block_head_gv_)); // empty chain for next instant
        auto* cty = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty }, false);
        b_.CreateCall(cty, h, { state });
        b_.CreateBr(after);
        b_.SetInsertPoint(after);
    }
    // Deferred writes through a runtime array index: replay each array's
    // typed records and each aggregate's memcpy records (both from the
    // shared rt_nba_arr_* buffers), waking Any-edge readers, then reset.
    if (!array_nba_sigs_.empty() || !agg_nba_sigs_.empty() || !handle_nba_sigs_.empty() ||
        !string_nba_sigs_.empty()) {
        for (const Signal sig : array_nba_sigs_) {
            emit_array_commit(sig, state);
        }
        for (const Signal sig : agg_nba_sigs_) {
            emit_agg_array_commit(sig, state);
        }
        for (const Signal sig : handle_nba_sigs_) {
            emit_handle_whole_commit(sig);
        }
        for (const Signal sig : string_nba_sigs_) {
            emit_string_whole_commit(sig);
        }
        // VCD: touch each dumped array that took an NBA write this instant,
        // so the flush scans its elements only on write instants (not every
        // instant). The runtime queue has no per-element index, so touch the
        // whole element range; the flush diff still emits only what changed.
        if (uses_vcd) {
            for (const Signal sig : array_nba_sigs_) {
                if (vcd_index_[static_cast<uint32_t>(sig)] < 0 || slot_of(sig).vif_rec) {
                    continue; // a vif member touched its own index per record
                }
                auto* fn     = b_.GetInsertBlock()->getParent();
                auto* svc    = llvm::BasicBlock::Create(ctx_, "vcd.arr.svc", fn);
                auto* nextbb = llvm::BasicBlock::Create(ctx_, "vcd.arr.next", fn);
                // Nonzero record count == the array took writes this instant
                // (records are not reset until nba_arr_reset below).
                auto* dirty  = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(arr_idx(sig)) });
                b_.CreateCondBr(b_.CreateICmpNE(dirty, b_.getInt32(0)), svc, nextbb);
                b_.SetInsertPoint(svc);
                vcd_touch_array(sig);
                b_.CreateBr(nextbb);
                b_.SetInsertPoint(nextbb);
            }
        }
        b_.CreateCall(nba_arr_reset_fn(), {});
    }
    // Deferred nonblocking intra-assignment stores that fired this instant:
    // each site commits its ready records (last fire wins), wakes readers,
    // and returns the records to its pool.
    if (!intra_nba_sigs_.empty()) {
        for (const Signal sig : intra_nba_sigs_) {
            emit_intra_commit(sig);
        }
    }
    // Nonblocking event triggers (`->> e`, LRM 15.5.1): for each pending
    // event, stamp the trigger time and wake its Any-edge waiters, then
    // clear the flag. Done in the NBA region so the wake lands after this
    // instant's nonblocking updates; the caller's loop-back then drains the
    // woken waiters.
    if (design_uses_nb_events_) {
        emit_event_pend_drain(wheel);
    }
}

// Per intra-NBA signal: dense id and the record StructType. The rt_timer
// (fields 0-1) sits at offset 0 so the record base is the timer pointer handed
// to the wheel; the runtime reads `idx` (field 2) to route the fire and owns
// `seq` (field 3); `count` (field 4) is the `@e` event countdown (unused by
// `#d`); `epoch` (field 5) is the inertial continuous-assign deschedule stamp
// (unused by transport records). Header layout mirrors rt_intra_rec
// (Runtime.h); the runtime never interprets the tail.
//
// The tail carries whatever the target shape needs, mirroring the plain-NBA
// record for the same shape so the two read alike:
//   scalar signal { iMM mm_val, [iW z_val], [iW mask] }
//   array element { iMM mm_val, [iW z_val], [iW mask], ptr mm_ptr, [ptr z_ptr] }
//   aggregate leaf { ptr dst, i32 nbytes, [i32 pad], [maxb x i8] payload, [maxb x i8] mask }
// A scalar destination is re-derived at commit from storage_ptr (a link-time
// constant); an array element or aggregate leaf must carry the pointer that
// the select resolved to when the statement executed (LRM 9.4.5).
uint32_t Backend::intra_idx(Signal sig) const {
    const auto it = std::find(intra_nba_sigs_.begin(), intra_nba_sigs_.end(), sig);
    return static_cast<uint32_t>(it - intra_nba_sigs_.begin());
}

bool Backend::is_intra_target(Signal sig) const {
    return std::find(intra_nba_sigs_.begin(), intra_nba_sigs_.end(), sig) != intra_nba_sigs_.end();
}

// Whether this signal's intra records stage a class handle, which makes a record in flight a
// GC root: until its delay elapses it may hold the only reference to the object it carries
// (LRM 9.4.5 samples the right-hand side now and lets the program drop its own references).
// Only an aggregate or an array gets here -- a scalar handle target is refused in the frontend --
// and the answer is the type's, so a design that stages no handle emits exactly what it did.
bool Backend::intra_rec_rooted(const SignalSlot& s) {
    return (s.is_agg || s.is_array) && reaches_class_handle(s.ir_type);
}

// Index of an aggregate record's payload; the mask follows it. The payload is a byte image of
// one leaf's slot, so a handle leaf sits at its start, and a rooted record is scanned by the
// collector one aligned word at a time (rt_gc_range_add): after `i32 nbytes` the payload would
// start 4 bytes off a word boundary and the handle would be invisible. A pad moves it there --
// for a rooted site only, so every other aggregate record keeps its layout.
uint32_t Backend::intra_agg_payload(const SignalSlot& s) {
    return intra_rec_rooted(s) ? 9 : 8;
}

llvm::StructType* Backend::intra_rec_ty(const SignalSlot& s) {
    auto*                    ptr = raw_ptr_ty();
    auto*                    i32 = b_.getInt32Ty();
    auto*                    i64 = b_.getInt64Ty();
    std::vector<llvm::Type*> f   = { ptr, ptr, i32, i32, i32, i64 };
    if (s.is_agg) {
        // An aggregate has no flat value (s.width / s.slot_ty are unused), and
        // one signal's leaves differ in width, so the payload is a byte image
        // of the widest leaf replayed by memcpy -- exactly agg_rec_ty's
        // width-agnostic encoding.
        auto* bytes = llvm::ArrayType::get(b_.getInt8Ty(), agg_max_leaf_bytes(s.ir_type));
        f.push_back(ptr);
        f.push_back(i32);
        if (intra_rec_rooted(s)) {
            f.push_back(i32); // see intra_agg_payload
        }
        f.push_back(bytes);
        if (s.intra_masked) {
            f.push_back(bytes);
        }
        return llvm::StructType::get(ctx_, f);
    }
    f.push_back(int_ty(s.four_state ? 2 * s.width : s.width));
    if (s.has_z) {
        f.push_back(int_ty(s.width));
    }
    if (s.intra_masked) {
        f.push_back(int_ty(s.width));
    }
    if (s.is_array || s.intra_ptr) {
        f.push_back(ptr);
        if (s.has_z) {
            f.push_back(ptr);
        }
        if (s.intra_ptr) {
            // The instance the handle resolved to, so the commit's wake reports
            // the transition of the instance the value actually landed in --
            // mirrors arr_rec_ctx on the plain-NBA record for the same member.
            f.push_back(ptr);
        }
    }
    if (s.intra_trigs > 1) {
        // The dense trigger ids this record registered on, so rt_intra_event_fire
        // can remove it from the ones that did not fire. It lives in the RECORD
        // rather than in a constant global because a trigger id is a per-INSTANCE
        // link-time constant (abs_id_const): twin instances of one definition
        // share the emitted code and have different ids, so a baked-in array
        // would name the wrong instance's triggers. Placed last so that adding it
        // moves no existing tail index.
        f.push_back(llvm::ArrayType::get(i32, s.intra_trigs));
    }
    return llvm::StructType::get(ctx_, f);
}

// Tail field indices of the scalar / array variant (mm_val=6, z_val=7 are
// fixed). Each is meaningful only when the corresponding field exists, like
// the arr_rec_* accessors.
uint32_t Backend::intra_rec_mask(const SignalSlot& s) const {
    return s.has_z ? 8 : 7;
}

uint32_t Backend::intra_rec_mm_ptr(const SignalSlot& s) const {
    return intra_rec_mask(s) + (s.intra_masked ? 1 : 0);
}

uint32_t Backend::intra_rec_z_ptr(const SignalSlot& s) const {
    return intra_rec_mm_ptr(s) + 1;
}

uint32_t Backend::intra_rec_ctx(const SignalSlot& s) const {
    return intra_rec_mm_ptr(s) + (s.has_z ? 2 : 1);
}

// Index of the trailing `[intra_trigs x i32]`; only when s.intra_trigs > 1.
uint32_t Backend::intra_rec_trigs(const SignalSlot& s) const {
    uint32_t i = intra_rec_mm_ptr(s);
    if (s.is_array || s.intra_ptr) {
        i += s.has_z ? 2 : 1;
    }
    if (s.intra_ptr) {
        i += 1;
    }
    return i;
}

// Per array-NBA signal: dense id (buffer index) and the record StructType
// { ptr mm_ptr, iMM mm_val, [ptr z_ptr, iW z_val], [iW mask] }.
uint32_t Backend::arr_idx(Signal sig) const {
    const auto it = std::find(array_nba_sigs_.begin(), array_nba_sigs_.end(), sig);
    return static_cast<uint32_t>(it - array_nba_sigs_.begin());
}

llvm::StructType* Backend::arr_rec_ty(const SignalSlot& s) {
    auto*                    ptr  = raw_ptr_ty();
    std::vector<llvm::Type*> f    = { ptr, int_ty(s.four_state ? 2 * s.width : s.width) };
    if (s.has_z) {
        f.push_back(ptr);
        f.push_back(int_ty(s.width));
    }
    if (s.arr_masked) {
        f.push_back(int_ty(s.width));
    }
    if (s.vif_rec) {
        // The instance the write landed in, so the replay can wake ITS waiters
        // rather than the representative's.
        f.push_back(ptr);
    }
    return llvm::StructType::get(ctx_, f);
}

// Field indices within the record (mm_ptr=0, mm_val=1 are fixed).
uint32_t Backend::arr_rec_z_ptr(const SignalSlot&) const {
    return 2;
}

uint32_t Backend::arr_rec_z_val(const SignalSlot&) const {
    return 3;
}

uint32_t Backend::arr_rec_mask(const SignalSlot& s) const {
    return s.has_z ? 4 : 2;
}

uint32_t Backend::arr_rec_ctx(const SignalSlot& s) const {
    return (s.has_z ? 4u : 2u) + (s.arr_masked ? 1u : 0u);
}

void Backend::lower_store(const LValue& lv, Plane rhs) {
    // A real RHS stores as its bit pattern; reals have no bit-selects, so
    // a real store always takes a whole-storage path (2-state real slot:
    // the single iW word below).
    if (rhs.fp != nullptr) {
        rhs = real_to_planes(fp_of(rhs));
    }
    // A runtime-offset select on storage too wide to read-modify-write inline:
    // the runtime merges into the containing word (rt_wide_ins) and drops the
    // bits outside the container (LRM 11.5.1).
    if (lv.wide_dyn) {
        wide_dyn_store(lv, rhs);
        return;
    }

    const uint32_t s_width = lv.store_width;
    llvm::Value*   mp      = lv_mm_ptr(lv);

    // 2-state slot: store the single iW value (the must plane of a 4-state RHS
    // drops any x/z to 0 per LRM 6.3.1; a 2-state RHS is the value itself). No z
    // word.
    if (!lv.four_state) {
        llvm::Value* v = must_of(rhs, false);
        if (selects_whole(lv)) {
            b_.CreateStore(v, mp);
            return;
        }
        llvm::Value* old  = b_.CreateLoad(int_ty(s_width), mp);
        llvm::Value* mask = store_mask(lv, s_width);
        b_.CreateStore(merge_sel(old, v, lv, s_width, mask), mp);
        return;
    }

    // A packed slot is a 4-state slot, and ir::verify's store-domain rule gives it a
    // 4-state value.

    // Whole-storage store: the frontend context-sizes the RHS to the lvalue
    // width (via an inserted conversion), so rhs already matches the storage width
    // and the planes store directly. A never-z slot drops the RHS z plane —
    // the may-z analysis proved it zero here.
    if (selects_whole(lv)) {
        b_.CreateStore(rhs.mm, mp);
        if (lv.has_z) {
            b_.CreateStore(rhs.z, lv_z_ptr(lv));
        }
        return;
    }

    // Partial store: read-modify-write each plane at the storage width.
    llvm::Value* zp  = lv.has_z ? lv_z_ptr(lv) : nullptr;
    const Plane  old = { b_.CreateLoad(int_ty(2 * s_width), mp),
                         lv.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(s_width), zp))
                                  : zero(s_width) };
    llvm::Value* mask = store_mask(lv, s_width);
    llvm::Value* must = merge_sel(must_of(old, true), must_of(rhs, true), lv, s_width, mask);
    llvm::Value* may  = merge_sel(may_of(old, true), may_of(rhs, true), lv, s_width, mask);
    llvm::Value* z    = merge_sel(old.z, rhs.z, lv, s_width, mask);
    b_.CreateStore(packed(must, may, z).mm, mp);
    if (lv.has_z) {
        b_.CreateStore(z, zp);
    }
}

// Stage a nonblocking assignment: write the sampled RHS into the target's shadow
// (whole-signal direct, or merged-with-mask for a part-select target) and register
// the enclosing block on the threaded commit chain (with this site's mask). Repeat
// NBAs to one signal in a block accumulate in the shadow. No wait queues are
// serviced here — that happens in the NBA region after the active drain.
void Backend::lower_store_nb(const LValue& lv, Plane rhs) {
    const SignalSlot& s  = slot_of(lv.sig);

    // Aggregate static-path leaf: stage the whole leaf into its shadow slot
    // and register its leaf-set bit. The value is leaf-width (frontend
    // conversion), so no bit merge. (Runtime-path aggregate NBA is rejected in
    // nba_classify, so the path is static here.)
    if (lv.is_agg) {
        const int      k = agg_static_leaf_index(s, lv.agg_field_path);
        const AggLeaf& m = s.static_leaves[static_cast<uint32_t>(k)];
        if (!m.four_state) {
            b_.CreateStore(must_of(rhs, false), shadow_ptr(s, m.sh_mm_field));
        } else {
            const Plane& f = rhs;
            b_.CreateStore(f.mm, shadow_ptr(s, m.sh_mm_field));
            if (m.has_z) {
                b_.CreateStore(f.z, shadow_ptr(s, m.sh_z_field));
            }
        }
        block_register(lv.sig, leaf_set_mask(s, static_cast<uint32_t>(k)), /*dynamic=*/false);
        return;
    }

    // A runtime-offset select on storage too wide to read-modify-write inline:
    // stage into the shadow through rt_wide_ins, and record the same bits in the
    // dirty mask, which is itself one 2-state plane of the signal's width -- so
    // the same call stages it, and the commit's word-wise merge is unchanged.
    if (lv.wide_dyn) {
        wide_dyn_store_planes(lv, shadow_ptr(s, s.sh_mm_field),
                              s.has_z ? shadow_ptr(s, s.sh_z_field) : nullptr, s.width,
                              s.four_state, rhs);
        wide_dyn_stage_mask(lv, s);
        register_block_store(lv, s);
        return;
    }

    llvm::Value* mp = shadow_ptr(s, s.sh_mm_field);

    // 2-state target: stage the single iW value into the iW shadow. No z.
    if (!s.four_state) {
        llvm::Value* v = must_of(rhs, false);
        if (!s.nba_partial) {
            b_.CreateStore(v, mp);
        } else {
            llvm::Value* old   = b_.CreateLoad(int_ty(s.width), mp);
            llvm::Value* bmask = store_mask(lv, s.width);
            b_.CreateStore(merge_sel(old, v, lv, s.width, bmask), mp);
            stage_dyn_mask(s, bmask);
        }
        register_block_store(lv, s);
        return;
    }

    // A never-z target stages no z shadow (and its commit loop skips z).
    llvm::Value* zp = s.has_z ? shadow_ptr(s, s.sh_z_field) : nullptr;

    if (!s.nba_partial) {
        // Whole-signal target: the RHS is already signal-width (frontend conversion),
        // so the shadow planes store directly — no mask, no read-modify-write.
        b_.CreateStore(rhs.mm, mp);
        if (s.has_z) {
            b_.CreateStore(rhs.z, zp);
        }
    } else {
        // Partial-capable target: merge the selected bits into the shadow. A
        // whole-signal site here selects every bit, so it merges the full word.
        const Plane  old   = { b_.CreateLoad(int_ty(2 * s.width), mp),
                               s.has_z ? static_cast<llvm::Value*>(b_.CreateLoad(int_ty(s.width), zp))
                                       : zero(s.width) };
        llvm::Value* bmask = store_mask(lv, s.width);
        llvm::Value* must  = merge_sel(must_of(old, true), must_of(rhs, true), lv, s.width, bmask);
        llvm::Value* may   = merge_sel(may_of(old, true), may_of(rhs, true), lv, s.width, bmask);
        llvm::Value* z     = merge_sel(old.z, rhs.z, lv, s.width, bmask);
        b_.CreateStore(packed(must, may, z).mm, mp);
        if (s.has_z) {
            b_.CreateStore(z, zp);
        }
        stage_dyn_mask(s, bmask);
    }
    register_block_store(lv, s);
}

// For a dynamic-mask signal, OR this site's written bits into its runtime dirty
// mask shadow field (the commit reads it). Static-only signals have no field and
// use a compile-time mask instead.
void Backend::stage_dyn_mask(const SignalSlot& s, llvm::Value* bmask) {
    if (!s.nba_dyn) {
        return;
    }
    llvm::Value* maskp = shadow_ptr(s, s.sh_mask_field);
    b_.CreateStore(b_.CreateOr(b_.CreateLoad(int_ty(s.width), maskp), bmask), maskp);
}

// The wide-select form of stage_dyn_mask: set the selected in-range bits of the
// dirty mask instead of OR-ing a storage-width value into it, whose construction
// is the O(width^2) shift this path exists to avoid. The mask is one `s.width`-bit
// 2-state plane, so rt_wide_ins addresses it like any other; writing all-ones over
// exactly the bits the site wrote is the same accumulation, since the helper only
// touches those bits and never clears one.
void Backend::wide_dyn_stage_mask(const LValue& lv, const SignalSlot& s) {
    if (!s.nba_dyn) {
        return; // A dynamic site always makes the signal nba_dyn; belt and braces.
    }
    wide_dyn_store_planes(lv, shadow_ptr(s, s.sh_mask_field), /*zp=*/nullptr, s.width,
                          /*four=*/false, two_val(ones(lv.width)));
}

// Compile-time mask (signal-width APInt) of the bits this static site writes:
// all-ones for a whole-signal site, else the constant [lsb, lsb+width) range, cut
// to the signal: a constant select that hangs over it writes only the bits inside
// (LRM 11.5.1). `lsb` is signed, below zero for `v[1:-2]`.
// Precondition: the site is static (constant bit_off), i.e. the signal is not
// nba_dyn.
llvm::APInt Backend::site_const_mask(const LValue& lv, const SignalSlot& s) const {
    if (lv.width == s.width && is_zero_const(lv.bit_off)) {
        return llvm::APInt::getAllOnes(s.width);
    }
    const int64_t lsb = llvm::cast<llvm::ConstantInt>(lv.bit_off)->getSExtValue();
    const int64_t lo  = std::max<int64_t>(lsb, 0);
    const int64_t hi  = std::min<int64_t>(lsb + lv.width, s.width);
    return lo < hi ? llvm::APInt::getBitsSet(s.width, static_cast<unsigned>(lo), static_cast<unsigned>(hi))
                   : llvm::APInt(s.width, 0);
}

// Index of the aggregate static leaf whose constant field path equals `fpath`
// (the MemberSelect chain's field indices). Linear over the small static-leaf
// list; -1 if not found (never, for a well-formed static-path NBA).
int Backend::agg_static_leaf_index(const SignalSlot& s, const std::vector<uint32_t>& fpath) const {
    for (uint32_t k = 0; k < s.static_leaves.size(); ++k) {
        if (s.static_leaves[k].path == fpath) {
            return static_cast<int>(k);
        }
    }
    return -1;
}

// Leaf-set mask for an aggregate block registration: one bit per static leaf,
// bit k set for the written leaf k. Unioned per block; the aggregate commit
// copies the leaves whose bit is set. Width is the static-leaf count — an
// aggregate signal's BlockSig mask lives in this space.
llvm::APInt Backend::leaf_set_mask(const SignalSlot& s, uint32_t leaf) const {
    llvm::APInt m(static_cast<uint32_t>(s.static_leaves.size()), 0);
    m.setBit(leaf);
    return m;
}

// Register this NBA store's target on the current block, recording the commit
// mask: a runtime dirty-mask field for a dynamic-select signal, else the union
// of this site's compile-time mask.
void Backend::register_block_store(const LValue& lv, const SignalSlot& s) {
    if (s.nba_dyn) {
        block_register(lv.sig, llvm::APInt(s.width, 0), /*dynamic=*/true);
    } else {
        block_register(lv.sig, site_const_mask(lv, s), /*dynamic=*/false);
    }
}

// Deferred-buffer index for an aggregate NBA signal: the aggregate buffers
// follow the array buffers in the shared runtime index space (rt_nba_arr_*).
uint32_t Backend::agg_buf_idx(Signal sig) const {
    const auto it = std::find(agg_nba_sigs_.begin(), agg_nba_sigs_.end(), sig);
    return static_cast<uint32_t>(array_nba_sigs_.size()) +
           static_cast<uint32_t>(it - agg_nba_sigs_.begin());
}

// Runtime buffer index of a whole-container NBA signal — after the array and
// aggregate buffers in the shared rt_nba_arr_* index space.
// Position of a signal within handle_nba_sigs_ / handle_nba_types_.
uint32_t Backend::handle_nba_slot(Signal sig) const {
    const auto it = std::find(handle_nba_sigs_.begin(), handle_nba_sigs_.end(), sig);
    return static_cast<uint32_t>(it - handle_nba_sigs_.begin());
}

uint32_t Backend::handle_buf_idx(Signal sig) const {
    return static_cast<uint32_t>(array_nba_sigs_.size() + agg_nba_sigs_.size()) +
           handle_nba_slot(sig);
}

// Deferred whole-container NBA record: { ptr dest_handle, <handle slot> staged }.
// The staged field is the container's own runtime handle struct, so one record
// shape covers a queue, a dynamic array and an associative array; its
// ownership moves to dest_handle at commit.
llvm::StructType* Backend::handle_rec_ty(Ref container_type) {
    return llvm::StructType::get(ctx_, { raw_ptr_ty(), handle_slot_ty(container_type) });
}

// Move one staged container handle into a live one (the NBA commit): ownership
// transfer, so the destination's prior elements are freed and no second deep
// copy happens.
void Backend::emit_handle_move(llvm::Value* dst, llvm::Value* src, Ref type) {
    if (is_assoc_type(type)) {
        b_.CreateCall(assoc_move_fn(), { dst, src, elem_ops_global(type) });
        return;
    }
    b_.CreateCall(is_queue_type(type) ? queue_move_fn() : dynarr_move_fn(),
                  { dst, src, elem_ops_global(type) });
}

// Stage a whole-container nonblocking assignment: reserve a record, capture the
// live dest handle pointer (stable — the signal slot never moves, only its heap
// buffer does, which the commit's move handles), zero the staged handle to the
// empty image, and deep-copy the RHS into it. The NBA region moves the staged
// buffer into dest.
void Backend::emit_handle_whole_nba(Index i) {
    const auto&       p    = ir_->payload(i).op_extra;
    const auto        qa   = ir_->extra_data<QueueArg>(p.extra);
    const LValue      dlv  = lvalue_of(p.op);
    // Signal-keyed like the deferred store family (handle_buf_idx(dlv.sig)), so the
    // destination is design state and needs no barrier. Asserted, not assumed.
    assert(dlv.gc_owner == nullptr && "a whole-container NBA targets a signal");
    auto*             rt   = handle_rec_ty(qa.queue_type);
    const auto        strd = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    llvm::Value*      rec  = b_.CreateCall(nba_arr_slot_fn(),
                                           { b_.getInt32(handle_buf_idx(dlv.sig)), b_.getInt32(strd) });
    b_.CreateAlignedStore(lv_handle_ptr(dlv, HandleUse::Place), b_.CreateStructGEP(rt, rec, 0), llvm::Align{ 1 });
    // Every container handle's empty image is all-zero, so a memset constructs
    // the staging handle the deep copy below then assigns into.
    llvm::Value* staged = b_.CreateStructGEP(rt, rec, 1);
    const auto   hsz    = env_->llmod.getDataLayout().getTypeAllocSize(handle_slot_ty(qa.queue_type));
    b_.CreateMemSet(staged, b_.getInt8(0), b_.getInt64(hsz), llvm::MaybeAlign{ 1 });
    emit_handle_assign(staged, lv_handle_ptr(lvalue_of(qa.arg), HandleUse::Read), qa.queue_type);
}

// Replay a container's deferred whole-NBA records: move each staged handle into
// its dest handle (push order, later write wins), then wake Any-edge readers.
void Backend::emit_handle_whole_commit(Signal sig) {
    // The CONTAINER's type, as recorded when this signal was registered --
    // never re-derived from the signal. `int fa[2][string]` is an array of
    // assocs and `struct { int a[string]; }` is a struct with one, so the
    // signal's type is the array or the struct in those cases while staging
    // sized its record from the assoc. The mismatch made the commit take the
    // wrong stride and dispatch rt_dynarr_move over an 8-byte assoc handle.
    const Ref      type = handle_nba_types_[handle_nba_slot(sig)];
    auto*          rt   = handle_rec_ty(type);
    const auto     strd = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    auto*          i8   = b_.getInt8Ty();
    const uint32_t idx  = handle_buf_idx(sig);
    llvm::Value*   base  = b_.CreateCall(nba_arr_base_fn(), { b_.getInt32(idx) });
    llvm::Value*   count = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(idx) });

    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "hnba.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "hnba.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "hnba.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);
    b_.SetInsertPoint(head);
    auto* k = b_.CreatePHI(b_.getInt32Ty(), 2);
    k->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(k, count), body, done);
    b_.SetInsertPoint(body);
    llvm::Value* rec    = b_.CreateInBoundsGEP(i8, base, { b_.CreateMul(k, b_.getInt32(strd)) });
    llvm::Value* dest   = b_.CreateAlignedLoad(raw_ptr_ty(), b_.CreateStructGEP(rt, rec, 0), llvm::Align{ 1 });
    llvm::Value* staged = b_.CreateStructGEP(rt, rec, 1);
    emit_handle_move(dest, staged, type);
    k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);
    b_.SetInsertPoint(done);

    if (signal_has_waiters(sig)) {
        auto* svc  = llvm::BasicBlock::Create(ctx_, "hnba.svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "hnba.cont", fn);
        b_.CreateCondBr(b_.CreateICmpNE(count, b_.getInt32(0)), svc, cont);
        b_.SetInsertPoint(svc);
        service_any_waiters(sig);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// Position of a signal within string_nba_sigs_.
uint32_t Backend::str_nba_slot(Signal sig) const {
    const auto it = std::find(string_nba_sigs_.begin(), string_nba_sigs_.end(), sig);
    return static_cast<uint32_t>(it - string_nba_sigs_.begin());
}

// Runtime buffer index of a whole-string NBA signal — the fourth range of the
// shared rt_nba_arr_* index space, after array, aggregate and container.
uint32_t Backend::str_buf_idx(Signal sig) const {
    return static_cast<uint32_t>(array_nba_sigs_.size() + agg_nba_sigs_.size() + handle_nba_sigs_.size()) +
           str_nba_slot(sig);
}

// Deferred whole-string NBA record: { ptr dest_handle, rt_string staged }. The
// staged handle OWNS its value between the Active region (where it is assigned)
// and the NBA region (where it is moved into dest), which is what gives the LRM
// 4.9.4 sample-now / commit-later semantics for a heap-owning value.
llvm::StructType* Backend::str_rec_ty() {
    return llvm::StructType::get(ctx_, { raw_ptr_ty(), str_slot_ty() });
}

// Stage a whole-string nonblocking assignment: reserve a record, capture the
// live destination handle pointer (stable — the signal slot never moves, only
// its heap buffer does, which the commit's move releases), CONSTRUCT the staged
// handle and deep-copy the RHS into it.
//
// Unlike a container handle, an empty rt_string is NOT all-zero (byte 31 holds
// the inline capacity), so the staging handle is built with rt_string_init, not
// a memset. Getting that wrong would make rt_string_assign read a bogus
// capacity out of the record.
void Backend::emit_string_whole_nba(const LValue& lv, Index rhs) {
    auto*        rt   = str_rec_ty();
    const auto   strd = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    llvm::Value* rec  = b_.CreateCall(nba_arr_slot_fn(), { b_.getInt32(str_buf_idx(lv.sig)), b_.getInt32(strd) });
    b_.CreateAlignedStore(lv_string_ptr(lv, StringUse::Place), b_.CreateStructGEP(rt, rec, 0), llvm::Align{ 1 });
    llvm::Value* staged = b_.CreateStructGEP(rt, rec, 1);
    b_.CreateCall(str_init_fn(), { staged });
    b_.CreateCall(str_assign_fn(),
                  { staged, str_of(rhs), alloc_site_global(SourceText{ .where = "<unknown>" }, "string") });
}

// Replay a string's deferred whole-NBA records: move each staged handle into its
// destination (push order, so the last write of the instant wins and every
// superseded buffer is freed exactly once — the move releases what the
// destination held). rt_string_move re-inits the staged handle, so the record's
// retained capacity is left as a valid empty string for the next instant.
//
// No wake and no VCD touch, unlike the container commit: a string is refused
// from sensitivity lists and event controls in the frontend (so no process can
// be waiting on one) and has no $var form.
void Backend::emit_string_whole_commit(Signal sig) {
    auto*          rt    = str_rec_ty();
    const auto     strd  = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    auto*          i8    = b_.getInt8Ty();
    const uint32_t idx   = str_buf_idx(sig);
    llvm::Value*   base  = b_.CreateCall(nba_arr_base_fn(), { b_.getInt32(idx) });
    llvm::Value*   count = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(idx) });

    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "snba.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "snba.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "snba.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);
    b_.SetInsertPoint(head);
    auto* k = b_.CreatePHI(b_.getInt32Ty(), 2);
    k->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(k, count), body, done);
    b_.SetInsertPoint(body);
    llvm::Value* rec    = b_.CreateInBoundsGEP(i8, base, { b_.CreateMul(k, b_.getInt32(strd)) });
    llvm::Value* dest   = b_.CreateAlignedLoad(raw_ptr_ty(), b_.CreateStructGEP(rt, rec, 0), llvm::Align{ 1 });
    llvm::Value* staged = b_.CreateStructGEP(rt, rec, 1);
    b_.CreateCall(str_move_fn(), { dest, staged });
    k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);
    b_.SetInsertPoint(done);
}

// Largest leaf slot byte size across an aggregate type (sizes the deferred
// record's payload so any leaf's value fits).
uint32_t Backend::agg_max_leaf_bytes(Ref type) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        return agg_max_leaf_bytes(t.element);
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t m = 0;
        for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
            m = std::max(m, agg_max_leaf_bytes(f.type));
        }
        return m;
    }
    if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue) {
        // A handle leaf is never a nonblocking target (frontend-guarded),
        // so it contributes nothing to the record payload size.
        return 0;
    }
    llvm::Type* slot = t.kind == Type::Kind::String ? static_cast<llvm::Type*>(str_slot_ty())
                                                    : agg_leaf_slot_ty(type);
    return static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(slot));
}

// Deferred aggregate-NBA record: { ptr dest_slot, i32 nbytes, [max x i8]
// payload [, [max x i8] mask] }. `payload` holds the new leaf value in the
// leaf's own slot byte layout. Without a mask the commit memcpies `nbytes` of
// it to `dest_slot`; with a mask (any partial site in the aggregate) the
// commit does a byte-wise masked read-modify-write. Either way a leaf of any
// width / domain replays uniformly (no per-leaf monomorphization).
llvm::StructType* Backend::agg_rec_ty(const SignalSlot& s) {
    const uint32_t maxb   = agg_max_leaf_bytes(s.ir_type);
    auto*          bytes  = llvm::ArrayType::get(b_.getInt8Ty(), maxb);
    std::vector<llvm::Type*> f{ raw_ptr_ty(), b_.getInt32Ty(), bytes };
    if (s.agg_masked) {
        f.push_back(bytes);
    }
    return llvm::StructType::get(ctx_, f);
}

// Pointer to the leaf SLOT an aggregate lvalue designates (its agg_path,
// without the final mm/z field select).
// Pointer to a whole aggregate ARGUMENT's storage image. An `input` actual may
// be an associative-array value, which is a deferred place: resolve it with
// get_ptr, because passing by value only reads it and LRM 7.8.6 forbids a read
// creating the entry. An out / inout aggregate actual is a caller temp
// (lower/Subroutines.cpp's make_canonical_agg_temp), so it never arrives deferred.
llvm::Value* Backend::agg_arg_ptr(const LValue& lv) {
    return lv.is_assoc ? agg_image_ptr(assoc_elem_lvalue(lv, /*for_write=*/false))
                       : agg_image_ptr(lv);
}

llvm::Value* Backend::agg_slot_ptr(const LValue& lv) {
    if (lv.is_assoc) {
        // Every supported aggregate-assoc access resolves the map slot (base)
        // via assoc_elem_lvalue BEFORE reaching here. A deferred assoc lvalue
        // arriving with is_assoc still set is a whole-aggregate use in an
        // unhandled context (a by-value subroutine argument, a raw rvalue
        // read) whose base is null -- fail loud rather than GEP null.
        throw BackendError{ "whole unpacked struct / array value of an associative "
                            "array in this context is not supported" };
    }
    std::vector<llvm::Value*> idx;
    idx.reserve(lv.agg_path.size() + 1);
    idx.push_back(b_.getInt32(0));
    for (llvm::Value* step : lv.agg_path) {
        idx.push_back(step);
    }
    return b_.CreateInBoundsGEP(lv.root_ty, lv.base, idx);
}

// Stage a nonblocking write to an aggregate leaf reached through a runtime
// array index: reserve a record and fill it with the destination slot pointer,
// the leaf's slot byte count, and the new value written in the leaf's own slot
// layout. The NBA region replays it by memcpy (later write to the same leaf
// wins). Whole-leaf only — a partial leaf write is guarded in the frontend.
void Backend::emit_agg_nba(const LValue& lv, Plane rhs) {
    if (lv.dyn_inrange != nullptr) {
        // An aggregate leaf under a variable array index outside the
        // declared bound (LRM 7.4.5: the write performs no operation), so
        // no record is staged at all. Without this, agg_slot_ptr below GEPs
        // past the array and the NBA region memcpys through that pointer
        // unconditionally when it replays the record -- `arr[9].f <= v` on
        // an `arr[0:3]` corrupts state, and a wild index segfaults.
        // emit_array_nba guards its own path the same way; this one was
        // missed.
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* dobb = llvm::BasicBlock::Create(ctx_, "aggnba.do", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "aggnba.cont", fn);
        b_.CreateCondBr(lv.dyn_inrange, dobb, cont);

        b_.SetInsertPoint(dobb);
        LValue clean      = lv;
        clean.dyn_inrange = nullptr;
        clean.bounds_checks.clear();
        emit_agg_nba(clean, rhs);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        return;
    }
    const SignalSlot& s      = slot_of(lv.sig);
    const uint32_t    W      = lv.store_width;
    const llvm::Align a1{ 1 };
    auto*             i8     = b_.getInt8Ty();
    auto*             rt     = agg_rec_ty(s);
    const auto        stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    llvm::Value* rec = b_.CreateCall(nba_arr_slot_fn(), { agg_id_value(lv.sig), b_.getInt32(stride) });
    b_.CreateAlignedStore(agg_slot_ptr(lv), b_.CreateStructGEP(rt, rec, 0), a1);
    const auto nbytes = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(lv.slot_ty));
    b_.CreateAlignedStore(b_.getInt32(nbytes), b_.CreateStructGEP(rt, rec, 1), a1);

    // Byte offset of the leaf slot's z field (4-state leaves only).
    const uint32_t zoff = (lv.four_state && lv.has_z)
                            ? static_cast<uint32_t>(env_->llmod.getDataLayout().getStructLayout(lv.slot_ty)->getElementOffset(1))
                            : 0;
    // Write a plane value into the payload at the slot's field offset.
    auto put = [&](llvm::Value* region, llvm::Value* v, uint32_t off) {
        llvm::Value* p = off == 0 ? region : b_.CreateInBoundsGEP(i8, region, { b_.getInt32(off) });
        b_.CreateAlignedStore(v, p, a1);
    };

    llvm::Value* payload = b_.CreateStructGEP(rt, rec, 2);
    // The value, positioned at the leaf's bit offset (partial select), in slot
    // layout: mm plane at offset 0, z plane at zoff.
    if (!lv.four_state) {
        put(payload, place_val(lv, must_of(rhs, false), W), 0);
    } else {
        const Plane& f = rhs;
        llvm::Value* mm = packed(place_val(lv, must_of(f, true), W), place_val(lv, may_of(f, true), W),
                                 place_val(lv, f.z, W)).mm;
        put(payload, mm, 0);
        if (lv.has_z) {
            put(payload, place_val(lv, f.z, W), zoff);
        }
    }
    // The write mask, same layout: which bits this store touches (all-ones for
    // a whole-leaf write). Padding bytes of the mask stay garbage but only
    // ever gate the dest slot's own padding, which is never read.
    if (s.agg_masked) {
        llvm::Value* mask   = store_mask(lv, W);
        llvm::Value* mask_p = b_.CreateStructGEP(rt, rec, 3);
        if (!lv.four_state) {
            put(mask_p, mask, 0);
        } else {
            put(mask_p, packed(mask, mask, zero(W)).mm, 0);
            if (lv.has_z) {
                put(mask_p, mask, zoff);
            }
        }
    }
}

// Replay an aggregate's deferred records: memcpy each record's payload to its
// destination slot (push order, later write wins), then wake level (Any-edge)
// readers if any record was staged.
void Backend::emit_agg_array_commit(Signal sig, llvm::Value* /*state*/) {
    const SignalSlot& s      = slot_of(sig);
    const uint32_t    idx    = agg_buf_idx(sig);
    auto*             rt     = agg_rec_ty(s);
    const auto        stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    auto*             i8     = b_.getInt8Ty();
    llvm::Value*      base   = b_.CreateCall(nba_arr_base_fn(), { b_.getInt32(idx) });
    llvm::Value*      count  = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(idx) });

    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "aggnba.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "aggnba.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "aggnba.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);
    b_.SetInsertPoint(head);
    auto* k = b_.CreatePHI(b_.getInt32Ty(), 2);
    k->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(k, count), body, done);
    b_.SetInsertPoint(body);
    llvm::Value* rec     = b_.CreateInBoundsGEP(i8, base, { b_.CreateMul(k, b_.getInt32(stride)) });
    llvm::Value* dest    = b_.CreateAlignedLoad(raw_ptr_ty(), b_.CreateStructGEP(rt, rec, 0), llvm::Align{ 1 });
    llvm::Value* nbytes  = b_.CreateAlignedLoad(b_.getInt32Ty(), b_.CreateStructGEP(rt, rec, 1), llvm::Align{ 1 });
    llvm::Value* payload = b_.CreateStructGEP(rt, rec, 2);
    if (!s.agg_masked) {
        b_.CreateMemCpy(dest, llvm::MaybeAlign{ 1 }, payload, llvm::MaybeAlign{ 1 }, nbytes);
    } else {
        // Byte-wise masked read-modify-write: dest[j] = (dest[j] & ~m) | (val[j] & m).
        // Width-agnostic — a mask byte of 0 leaves dest[j] unchanged, so an
        // unselected bit (or a garbage padding byte gating dest padding) is a
        // no-op. Mirrors the array masked commit without monomorphizing width.
        llvm::Value* mask_r = b_.CreateStructGEP(rt, rec, 3);
        auto*        bh     = llvm::BasicBlock::Create(ctx_, "aggnba.bh", fn);
        auto*        bb     = llvm::BasicBlock::Create(ctx_, "aggnba.bb", fn);
        auto*        bd     = llvm::BasicBlock::Create(ctx_, "aggnba.bd", fn);
        auto*        pre    = b_.GetInsertBlock();
        b_.CreateBr(bh);
        b_.SetInsertPoint(bh);
        auto* j = b_.CreatePHI(b_.getInt32Ty(), 2);
        j->addIncoming(b_.getInt32(0), pre);
        b_.CreateCondBr(b_.CreateICmpULT(j, nbytes), bb, bd);
        b_.SetInsertPoint(bb);
        llvm::Value* dj  = b_.CreateInBoundsGEP(i8, dest, { j });
        llvm::Value* m   = b_.CreateLoad(i8, b_.CreateInBoundsGEP(i8, mask_r, { j }));
        llvm::Value* v   = b_.CreateLoad(i8, b_.CreateInBoundsGEP(i8, payload, { j }));
        llvm::Value* old = b_.CreateLoad(i8, dj);
        b_.CreateStore(b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(m)), b_.CreateAnd(v, m)), dj);
        j->addIncoming(b_.CreateAdd(j, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(bh);
        b_.SetInsertPoint(bd);
    }
    k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);
    b_.SetInsertPoint(done);

    if (signal_has_waiters(sig)) {
        auto* svc  = llvm::BasicBlock::Create(ctx_, "aggnba.svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "aggnba.cont", fn);
        b_.CreateCondBr(b_.CreateICmpNE(count, b_.getInt32(0)), svc, cont);
        b_.SetInsertPoint(svc);
        service_any_waiters(sig);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// Stage a nonblocking write to an unpacked-array element: reserve a fixed-stride
// record slot in this array's deferred buffer (rt_nba_arr_slot) and fill it with
// the destination pointer(s), the positioned value, and (for a masked target) the
// write mask. The NBA region replays it. Element width is unrestricted — the
// value is a native iMM word of any size.
void Backend::emit_array_nba(const LValue& lv, Plane rhs) {
    if (lv.dyn_inrange != nullptr) {
        // A variable-indexed element outside the array's declared bound
        // (LRM 7.4.5 / 11.5.1): the write is a no-op, so no record is
        // staged at all -- lv_mm_ptr(lv) below would otherwise compute an
        // out-of-bounds destination pointer that the NBA region later
        // dereferences unconditionally when replaying the record.
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* dobb = llvm::BasicBlock::Create(ctx_, "arrnba.do", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "arrnba.cont", fn);
        b_.CreateCondBr(lv.dyn_inrange, dobb, cont);

        b_.SetInsertPoint(dobb);
        LValue clean      = lv;
        clean.dyn_inrange = nullptr;
        clean.bounds_checks.clear();
        emit_array_nba(clean, rhs);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        return;
    }
    const uint32_t    W = lv.store_width;
    const SignalSlot& s = slot_of(lv.sig);
    const llvm::Align a1{ 1 }; // records live in an unaligned byte buffer

    // A bounds-checked select restricts the mask to the in-range bits and aligns
    // the value poison-free (LRM 11.5.1); a whole/constant target's mask folds to
    // all-ones and is elided (unmasked signals carry no mask field).
    llvm::Value* mask_w = s.arr_masked ? store_mask(lv, W) : nullptr;
    llvm::Value* mm_val;
    if (!s.four_state) {
        mm_val = place_val(lv, must_of(rhs, false), W); // iW
    } else {
        llvm::Value* must = place_val(lv, must_of(rhs, true), W);
        llvm::Value* may  = place_val(lv, may_of(rhs, true), W);
        llvm::Value* zval = place_val(lv, rhs.z, W);
        mm_val            = packed(must, may, zval).mm; // i2W: must | (may << W)
    }

    llvm::StructType* rt     = arr_rec_ty(s);
    const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    llvm::Value*      rec    = b_.CreateCall(nba_arr_slot_fn(), { arr_id_value(lv.sig), b_.getInt32(stride) });
    b_.CreateAlignedStore(lv_mm_ptr(lv), b_.CreateStructGEP(rt, rec, 0), a1);
    b_.CreateAlignedStore(mm_val, b_.CreateStructGEP(rt, rec, 1), a1);
    if (s.has_z) {
        // has_z implies a 4-state element, so rhs is 4-state here.
        b_.CreateAlignedStore(lv_z_ptr(lv), b_.CreateStructGEP(rt, rec, arr_rec_z_ptr(s)), a1);
        b_.CreateAlignedStore(place_val(lv, rhs.z, W), b_.CreateStructGEP(rt, rec, arr_rec_z_val(s)), a1);
    }
    if (s.arr_masked) {
        b_.CreateAlignedStore(mask_w, b_.CreateStructGEP(rt, rec, arr_rec_mask(s)), a1);
    }
    if (s.vif_rec) {
        // Through a handle, the instance it named; inside a subroutine reached
        // through one, the instance formal 0 named (the body-wide override);
        // from the member's own instance, that instance's own record. Either
        // way the replay wakes the instance the value actually landed in.
        llvm::Value* ctx = lv.vif_ctx;
        if (ctx == nullptr && ctx_override_for(s)) {
            ctx = ctx_override_;
        }
        if (ctx == nullptr) {
            ctx = ensure_ctx(anchors_[s.anchor]);
        }
        b_.CreateAlignedStore(ctx, b_.CreateStructGEP(rt, rec, arr_rec_ctx(s)), a1);
    }
}

// Replay array `sig`'s deferred records in push order (later write to an element
// wins): a monomorphized loop of typed masked/plain stores into the live element
// slots, then wake its Any-edge readers if it took any write.
// Arm a nonblocking intra-assignment delay (`x <= #d y`): reserve a stable
// record from the target signal's pool, fill it with the sampled value planes
// (monomorphized per width / z-ness), and schedule it on the wheel for
// `now + ticks` with rt_intra_cb. The process does not suspend. The NBA region
// of the fired instant commits the record (emit_intra_commit).
// Pack a freshly-sampled intra-assignment RHS into the deferred record's mm
// form for a whole-scalar target of slot `s`: a real rides as its IEEE-754 bit
// pattern in the native slot, a 2-state target as its value, and a 4-state
// target as the packed must|may word. `rhs` is replaced in place by a real's bit
// pattern; z is stored by the caller under s.has_z (a real / 2-state slot has
// none). Shared by the delay (`<= #d`) and event (`<= @e`) arms.
llvm::Value* Backend::intra_pack_mm(const SignalSlot& s, Plane& rhs) {
    if (rhs.fp != nullptr) {
        rhs = real_to_planes(fp_of(rhs));
    }
    if (!s.four_state) {
        return must_of(rhs, false);
    }
    return rhs.mm;
}

// Fill a deferred record's payload from the sampled RHS: the value positioned
// at the target's bit offset inside its destination word, the write mask when
// the target writes less than that word, and -- for an array element or
// aggregate leaf -- the destination pointer this select resolves to right now.
// LRM 9.4.5 evaluates the LHS index and the RHS when the statement executes and
// defers only the update, so both are baked in here rather than recomputed at
// commit. Mirrors emit_array_nba / emit_agg_nba, whose plain-NBA records carry
// the same fields; positioning each plane separately is why this cannot reuse
// intra_pack_mm's already-packed word. Shared by the delay (`<= #d`) and event
// (`<= @e`) arms so the two cannot drift.
void Backend::fill_intra_payload(const LValue& lv, Plane rhs, llvm::StructType* rt, llvm::Value* rec) {
    const SignalSlot& s = slot_of(lv.sig);
    if (rhs.fp != nullptr) {
        // A real rides as its IEEE-754 bit pattern in the native slot.
        rhs = real_to_planes(fp_of(rhs));
    }
    if (lv.wide_dyn) {
        // TODO: a runtime-offset select into storage wider than
        // wide_dyn_select_bits addresses 64-bit words through rt_wide_ins
        // (wide_dyn_store_planes is where plain NBA routes one) rather than
        // building a storage-width mask, which is the only encoding the
        // deferred record has room for.
        throw BackendError{ "nonblocking intra-assignment timing on a runtime-offset "
                            "select of a very wide vector is not supported" };
    }
    if (s.is_agg) {
        fill_intra_agg_payload(lv, rhs, rt, rec);
        return;
    }
    const llvm::Align a1{ 1 }; // record lives in an unaligned pooled block
    const uint32_t    W = lv.store_width;
    // A whole target's bit_off is a constant zero and its mask folds to
    // all-ones (and is elided when no site of this signal is partial), so the
    // hot whole-scalar path keeps emitting a bare plane store. A bounds-checked
    // dynamic select positions poison-free and restricts to the in-range bits
    // (LRM 11.5.1).
    llvm::Value* mm_val;
    if (!s.four_state) {
        mm_val = place_val(lv, must_of(rhs, false), W);
    } else {
        llvm::Value* must = place_val(lv, must_of(rhs, true), W);
        llvm::Value* may  = place_val(lv, may_of(rhs, true), W);
        llvm::Value* zval = place_val(lv, rhs.z, W);
        mm_val            = packed(must, may, zval).mm; // i2W: must | (may << W)
    }
    b_.CreateAlignedStore(mm_val, b_.CreateStructGEP(rt, rec, 6), a1);
    if (s.has_z) {
        // has_z implies a 4-state target, so rhs is 4-state here.
        b_.CreateAlignedStore(place_val(lv, rhs.z, W), b_.CreateStructGEP(rt, rec, 7), a1);
    }
    if (s.intra_masked) {
        b_.CreateAlignedStore(store_mask(lv, W), b_.CreateStructGEP(rt, rec, intra_rec_mask(s)), a1);
    }
    if (s.is_array || s.intra_ptr) {
        b_.CreateAlignedStore(lv_mm_ptr(lv), b_.CreateStructGEP(rt, rec, intra_rec_mm_ptr(s)), a1);
        if (s.has_z) {
            b_.CreateAlignedStore(lv_z_ptr(lv), b_.CreateStructGEP(rt, rec, intra_rec_z_ptr(s)), a1);
        }
    }
    if (s.intra_ptr) {
        // Through a handle, the instance it named; inside a subroutine reached
        // through one, the instance formal 0 named (the body-wide override);
        // from the member's own instance, that instance's own record. Resolved
        // HERE, at statement execution, which is what LRM 10.4.2 requires of a
        // "virtual interface reference" in an lvalue. Same resolution order as
        // emit_array_nba's arr_rec_ctx, deliberately -- the two paths must agree
        // on which instance a given statement writes.
        llvm::Value* ctx = lv.vif_ctx;
        if (ctx == nullptr && ctx_override_for(s)) {
            ctx = ctx_override_;
        }
        if (ctx == nullptr) {
            ctx = ensure_ctx(anchors_[s.anchor]);
        }
        b_.CreateAlignedStore(ctx, b_.CreateStructGEP(rt, rec, intra_rec_ctx(s)), a1);
    }
}

// Fill an aggregate-leaf record: the destination slot pointer, its byte count,
// and the leaf's byte image (the value, plus a mask when the write is partial)
// in slot layout. One aggregate's leaves differ in width, so the payload is the
// width-agnostic byte encoding of agg_rec_ty, replayed by memcpy at commit.
void Backend::fill_intra_agg_payload(const LValue& lv, Plane rhs, llvm::StructType* rt, llvm::Value* rec) {
    const SignalSlot& s  = slot_of(lv.sig);
    const uint32_t    W  = lv.store_width;
    const llvm::Align a1{ 1 };
    auto*             i8 = b_.getInt8Ty();
    b_.CreateAlignedStore(agg_slot_ptr(lv), b_.CreateStructGEP(rt, rec, 6), a1);
    const auto nbytes = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(lv.slot_ty));
    b_.CreateAlignedStore(b_.getInt32(nbytes), b_.CreateStructGEP(rt, rec, 7), a1);

    // Byte offset of the leaf slot's z field (4-state leaves only).
    const uint32_t zoff =
      (lv.four_state && lv.has_z)
        ? static_cast<uint32_t>(
            env_->llmod.getDataLayout().getStructLayout(lv.slot_ty)->getElementOffset(1))
        : 0;
    auto put = [&](llvm::Value* region, llvm::Value* v, uint32_t off) {
        llvm::Value* p = off == 0 ? region : b_.CreateInBoundsGEP(i8, region, { b_.getInt32(off) });
        b_.CreateAlignedStore(v, p, a1);
    };
    llvm::Value* payload = b_.CreateStructGEP(rt, rec, intra_agg_payload(s));
    if (!lv.four_state) {
        put(payload, place_val(lv, must_of(rhs, false), W), 0);
    } else {
        const Plane& f = rhs;
        llvm::Value* mm = packed(place_val(lv, must_of(f, true), W), place_val(lv, may_of(f, true), W),
                                 place_val(lv, f.z, W))
                            .mm;
        put(payload, mm, 0);
        if (lv.has_z) {
            put(payload, place_val(lv, f.z, W), zoff);
        }
    }
    // Padding bytes of the mask stay garbage but only ever gate the
    // destination slot's own padding, which is never read.
    if (s.intra_masked) {
        llvm::Value* mask   = store_mask(lv, W);
        llvm::Value* mask_p = b_.CreateStructGEP(rt, rec, intra_agg_payload(s) + 1);
        if (!lv.four_state) {
            put(mask_p, mask, 0);
        } else {
            put(mask_p, packed(mask, mask, zero(W)).mm, 0);
            if (lv.has_z) {
                put(mask_p, mask, zoff);
            }
        }
    }
}

void Backend::emit_intra_nba_arm(const LValue& lv, Plane rhs, llvm::Value* ticks) {
    if (lv.dyn_inrange != nullptr) {
        // A variable index outside the declared bound (LRM 7.4.5 / 11.5.1): the
        // write is a no-op, so arm nothing at all. Otherwise the record would
        // carry an out-of-bounds destination pointer that the NBA region
        // dereferences unconditionally when it replays -- and for a scalar
        // select, an unrestricted mask. Mirrors emit_array_nba.
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* dobb = llvm::BasicBlock::Create(ctx_, "intranba.do", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "intranba.cont", fn);
        b_.CreateCondBr(lv.dyn_inrange, dobb, cont);

        b_.SetInsertPoint(dobb);
        LValue clean      = lv;
        clean.dyn_inrange = nullptr;
        clean.bounds_checks.clear();
        emit_intra_nba_arm(clean, rhs, ticks);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        return;
    }
    const SignalSlot& s      = slot_of(lv.sig);
    llvm::StructType* rt     = intra_rec_ty(s);
    const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    llvm::Value*      idxv   = intra_id_value(lv.sig);

    llvm::Value* rec =
      b_.CreateCall(intra_rec_rooted(s) ? intra_alloc_rooted_fn() : intra_alloc_fn(), { idxv, b_.getInt32(stride) });
    b_.CreateStore(idxv, b_.CreateStructGEP(rt, rec, 2)); // idx routes the fire
    fill_intra_payload(lv, rhs, rt, rec);

    // Arm the wheel for `now + ticks`. A zero delay cannot arm the wheel
    // (deadline must be > now) and its update lands in the current instant's
    // NBA region, so link the record straight onto the ready list (rt_intra_now).
    // A compile-time-constant delay emits only the taken path; a runtime delay
    // that happens to be zero branches to the same immediate path.
    auto  schedule  = [&] {
        b_.CreateCall(wheel_schedule_rel_fn(), { wheel_, rec, ticks, intra_cb_trampoline() });
    };
    auto immediate = [&] { b_.CreateCall(intra_now_fn(), { rec }); };

    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ticks)) {
        if (c->isZero()) {
            immediate();
        } else {
            schedule();
        }
        return;
    }
    auto* zbb   = llvm::BasicBlock::Create(ctx_, "intra.zero", fn_);
    auto* nzbb  = llvm::BasicBlock::Create(ctx_, "intra.sched", fn_);
    auto* contb = llvm::BasicBlock::Create(ctx_, "intra.armed", fn_);
    b_.CreateCondBr(b_.CreateICmpEQ(ticks, b_.getInt64(0)), zbb, nzbb);
    b_.SetInsertPoint(zbb);
    immediate();
    b_.CreateBr(contb);
    b_.SetInsertPoint(nzbb);
    schedule();
    b_.CreateBr(contb);
    b_.SetInsertPoint(contb);
}

// A plain NBA to a signal that also takes `<= #d` / `<= @e`. NBA updates are performed in
// the order they were created (LRM 4.6 b), 10.4.2, 15.5.1; docs/LRM_NOTES.md, note 51): a
// plain NBA's is created when it executes, a timed one's when its delay elapses or its
// event occurs. So this write is later than every deferred update of the signal released
// before it executed: those records are already on the ready list. But the write stages
// into the block shadow (or the array / aggregate queue), which commits BEFORE the
// ready list, so left alone it would lose to them. When such a record is waiting, also
// copy the write onto the list behind it: the record carries the value and mask the
// staged write used, so replaying it re-applies exactly the bits this write selects.
//
// The staged write itself is unchanged: a record released AFTER this write executed goes
// behind it on the list (its batch is later, intra_ready_push), so the write commits first
// and the update after it, as creation order says. A write executed while no record is
// waiting anywhere costs one load of rt_intra_total.
void Backend::emit_intra_nba_ordered(const LValue& lv, Plane rhs) {
    if (lv.wide_dyn) {
        // TODO: a runtime-offset select of a very wide vector stages through
        // rt_wide_ins, which the deferred record has no room to encode (see
        // fill_intra_payload). Refused rather than left ordered wrongly against the
        // timed updates of the same vector.
        throw BackendError{ "nonblocking assignment to a runtime-offset select of a very wide "
                            "vector that also takes a timed nonblocking assignment is not "
                            "supported" };
    }
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* probe = llvm::BasicBlock::Create(ctx_, "intraord.probe", fn);
    auto* copy  = llvm::BasicBlock::Create(ctx_, "intraord.copy", fn);
    auto* cont  = llvm::BasicBlock::Create(ctx_, "intraord.cont", fn);
    // A word every simulation keeps, ahead of the per-site count: it is zero for almost
    // every write, and the count is a call into the runtime.
    b_.CreateCondBr(b_.CreateICmpNE(b_.CreateLoad(b_.getInt32Ty(), intra_total_gv()), b_.getInt32(0)), probe, cont);

    b_.SetInsertPoint(probe);
    llvm::Value* waiting =
      b_.CreateICmpNE(b_.CreateCall(intra_ready_count_fn(), { intra_id_value(lv.sig) }), b_.getInt32(0));
    b_.CreateCondBr(waiting, copy, cont);

    b_.SetInsertPoint(copy);
    emit_intra_nba_arm(lv, rhs, b_.getInt64(0));
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
}

// Arm (or reschedule) an inertial continuous-assignment delay (`assign #d x = y`,
// LRM 10.3.3). Evaluate the RHS now and ask rt_inertial_decide, comparing it to
// both the target net's current value and any still-pending scheduled value:
//   - equal to the pending value  -> keep the running timer (step b), arm nothing;
//   - equal to the current net    -> deschedule the pending store (step c), nothing;
//   - otherwise                   -> deschedule + arm a fresh epoch-stamped store
//                                     for `now + d` (steps a/d).
// A stale (descheduled) record still on the wheel is dropped when it fires
// (rt_inertial_cb), which is what rejects a pulse narrower than the delay. The
// committed record rides the same NBA-region commit as `x <= #d` (emit_intra_commit).
void Backend::emit_inertial_arm(const LValue& lv, Plane rhs, llvm::Value* ticks) {
    const SignalSlot& s      = slot_of(lv.sig);
    const llvm::Align a1{ 1 };
    llvm::StructType* rt     = intra_rec_ty(s);
    const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    llvm::Value*      idxv   = intra_id_value(lv.sig);
    const uint32_t    W      = s.width;
    auto*             mmty   = int_ty(s.four_state ? 2 * W : W);
    auto*             zty    = int_ty(W);

    // Freshly-evaluated RHS planes for the decide compare below, unpositioned:
    // the record's own copy is filled (and positioned) by fill_intra_payload.
    llvm::Value* mm_val;
    if (!s.four_state) {
        mm_val = must_of(rhs, false);
    } else {
        mm_val = rhs.mm; // i2W: must | (may << W)
    }
    llvm::Value* z_val = s.has_z ? rhs.z : nullptr;

    // Current net value planes (the step-c "equals current LHS" compare).
    llvm::Value* cur_mm = b_.CreateLoad(mmty, lv_mm_ptr(lv));
    llvm::Value* cur_z  = s.has_z ? b_.CreateLoad(zty, lv_z_ptr(lv)) : nullptr;

    // The site's persistent pending-value buffer (rt_inertial_buf): holds
    // whatever was armed last time, at any width. Every compare below is an
    // ordinary `icmp eq` on values loaded into registers, never a runtime
    // byte-memcmp of memory -- an odd (non-byte-multiple) width's padding
    // bits are otherwise unspecified across two independently materialized
    // buffers, so `icmp` is the only representation-independent way to
    // compare an arbitrary-width iN for equality.
    // NOTE: StructType::get(Context, {single_elem}) is genuinely ambiguous
    // with StructType::get(Context, isPacked) -- a lone Type* argument binds
    // to `bool isPacked` (a standard pointer-to-bool conversion) in
    // preference to ArrayRef<Type*>'s initializer_list constructor (a
    // user-defined conversion), silently building an EMPTY packed struct.
    // An explicit SmallVector sidesteps the ambiguity entirely.
    llvm::SmallVector<llvm::Type*, 2> buf_fields{ mmty };
    if (s.has_z) {
        buf_fields.push_back(zty);
    }
    llvm::StructType* buf_ty          = llvm::StructType::get(ctx_, buf_fields);
    const uint32_t    buf_bytes       = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(buf_ty));
    llvm::Value*      buf             = b_.CreateCall(inertial_buf_fn(), { idxv, b_.getInt32(buf_bytes) });
    llvm::Value*      pend_mm_ptr     = b_.CreateStructGEP(buf_ty, buf, 0);
    llvm::Value*      pend_mm         = b_.CreateLoad(mmty, pend_mm_ptr);
    llvm::Value*      same_as_pending = b_.CreateICmpEQ(mm_val, pend_mm);
    llvm::Value*      same_as_cur     = b_.CreateICmpEQ(mm_val, cur_mm);
    llvm::Value*      pend_z_ptr      = nullptr;
    if (s.has_z) {
        pend_z_ptr          = b_.CreateStructGEP(buf_ty, buf, 1);
        llvm::Value* pend_z = b_.CreateLoad(zty, pend_z_ptr);
        same_as_pending     = b_.CreateAnd(same_as_pending, b_.CreateICmpEQ(z_val, pend_z));
        same_as_cur         = b_.CreateAnd(same_as_cur, b_.CreateICmpEQ(z_val, cur_z));
    }
    auto*        i32 = b_.getInt32Ty();
    llvm::Value* ep = b_.CreateCall(inertial_decide_fn(),
                                    { idxv, b_.CreateZExt(same_as_pending, i32), b_.CreateZExt(same_as_cur, i32) });

    // ep == 0 -> arm nothing (kept timer / no change). Only the taken block
    // reserves a record; a spurious alloc on the not-taken path would leak.
    auto* armbb = llvm::BasicBlock::Create(ctx_, "inertial.arm", fn_);
    auto* contb = llvm::BasicBlock::Create(ctx_, "inertial.done", fn_);
    b_.CreateCondBr(b_.CreateICmpNE(ep, b_.getInt64(0)), armbb, contb);

    b_.SetInsertPoint(armbb);
    // Remember this arm's value for the next call's "same as pending" compare.
    b_.CreateStore(mm_val, pend_mm_ptr);
    if (s.has_z) {
        b_.CreateStore(z_val, pend_z_ptr);
    }
    llvm::Value* rec = b_.CreateCall(intra_alloc_fn(), { idxv, b_.getInt32(stride) });
    b_.CreateStore(idxv, b_.CreateStructGEP(rt, rec, 2)); // idx routes the fire
    b_.CreateAlignedStore(ep, b_.CreateStructGEP(rt, rec, 5), a1); // deschedule stamp
    // One record contract: the shared filler positions the planes and writes the
    // mask this signal's record type carries. A continuous-assign target is a
    // whole net, where positioning folds away and the mask is all-ones -- but a
    // delayed gate driving one bit of a vector (emit_gate_as_process, LRM 28.16)
    // reaches here with a bit-select lvalue and does need both.
    fill_intra_payload(lv, rhs, rt, rec);

    // Arm the wheel for `now + ticks` with rt_inertial_cb. A zero delay cannot
    // arm the wheel, so its store lands in the current instant's NBA region via
    // an immediate callback (the epoch guard passes — ep is the freshest stamp).
    auto* nullp    = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    auto  schedule = [&] {
        b_.CreateCall(wheel_schedule_rel_fn(), { wheel_, rec, ticks, inertial_cb_trampoline() });
    };
    auto immediate = [&] { b_.CreateCall(inertial_cb_fn(), { rec, nullp }); };

    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ticks)) {
        if (c->isZero()) {
            immediate();
        } else {
            schedule();
        }
        b_.CreateBr(contb);
    } else {
        auto* zbb  = llvm::BasicBlock::Create(ctx_, "inertial.zero", fn_);
        auto* nzbb = llvm::BasicBlock::Create(ctx_, "inertial.sched", fn_);
        b_.CreateCondBr(b_.CreateICmpEQ(ticks, b_.getInt64(0)), zbb, nzbb);
        b_.SetInsertPoint(zbb);
        immediate();
        b_.CreateBr(contb);
        b_.SetInsertPoint(nzbb);
        schedule();
        b_.CreateBr(contb);
    }
    b_.SetInsertPoint(contb);
}

// Arm a nonblocking intra-assignment event (`x <= @e y` / `repeat(N) @e`):
// reserve a record, fill the sampled value planes and the countdown, and
// register it on trigger `trig_id`. The process does not suspend. When the
// trigger edge fires (blocking store or NBA commit), rt_intra_event_fire counts
// the record down and, at zero, moves it to the target ready list for the NBA
// region (emit_intra_commit). Same value/plane layout as emit_intra_nba_arm.
void Backend::emit_intra_event_arm(const LValue&                lv,
                                   Plane                        rhs,
                                   llvm::Value*                 count,
                                   llvm::ArrayRef<llvm::Value*> trig_ids) {
    if (lv.dyn_inrange != nullptr) {
        // Out-of-range variable index: register nothing (see emit_intra_nba_arm).
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* dobb = llvm::BasicBlock::Create(ctx_, "intraev.do", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "intraev.cont", fn);
        b_.CreateCondBr(lv.dyn_inrange, dobb, cont);

        b_.SetInsertPoint(dobb);
        LValue clean      = lv;
        clean.dyn_inrange = nullptr;
        clean.bounds_checks.clear();
        emit_intra_event_arm(clean, rhs, count, trig_ids);
        b_.CreateBr(cont);

        b_.SetInsertPoint(cont);
        return;
    }
    const SignalSlot& s      = slot_of(lv.sig);
    llvm::StructType* rt     = intra_rec_ty(s);
    const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    llvm::Value*      idxv   = intra_id_value(lv.sig);

    llvm::Value* rec =
      b_.CreateCall(intra_rec_rooted(s) ? intra_alloc_rooted_fn() : intra_alloc_fn(), { idxv, b_.getInt32(stride) });
    b_.CreateStore(idxv, b_.CreateStructGEP(rt, rec, 2));
    b_.CreateStore(cast(count, 32, false), b_.CreateStructGEP(rt, rec, 4)); // event countdown
    fill_intra_payload(lv, rhs, rt, rec);
    if (trig_ids.size() == 1) {
        b_.CreateCall(intra_event_register_fn(), { trig_ids[0], rec });
        return;
    }
    // `@(a or b)`: one record on several trigger lists, where the whole list is
    // the event and the FIRST edge to bring the countdown to zero matures the
    // store. The runtime then has to remove the record from the triggers that
    // did not fire, which it does by pointer identity -- so it needs the id
    // list, and the list has to outlive the arm.
    //
    // It goes in the RECORD's own tail: the record is stable from arm to commit
    // (the wheel and the pending lists both hold pointers into it), and a
    // trigger id is a per-INSTANCE link-time constant, so a compile-time
    // constant global would bake one instance's ids into code twin instances
    // share. The tail is sized to the longest list over this signal's sites, so
    // a signal whose every site names one trigger carries no array at all.
    //
    // Lazy removal ("mark it matured and let the other lists skip it") is
    // rejected: a committed record goes back to its site freelist, so a stale
    // pointer left on another trigger's list can later match a REUSED record and
    // mature it early.
    auto* i32     = b_.getInt32Ty();
    auto* arr_ty  = llvm::ArrayType::get(i32, s.intra_trigs);
    auto* ids     = b_.CreateStructGEP(rt, rec, intra_rec_trigs(s));
    for (uint32_t k = 0; k < trig_ids.size(); ++k) {
        b_.CreateAlignedStore(trig_ids[k], b_.CreateConstInBoundsGEP2_32(arr_ty, ids, 0, k),
                              llvm::Align{ 1 });
    }
    b_.CreateCall(intra_event_register_group_fn(),
                  { ids, b_.getInt32(static_cast<uint32_t>(trig_ids.size())), rec });
}

// Commit intra-NBA signal `sig`'s fired records in the NBA region: apply each
// record's sampled planes to its destination (ready-list order, so the
// last-armed write to a given bit wins, LRM 10.4.2), then wake readers once and
// return the records to the pool.
//
// The destination depends on the target shape. A whole scalar or a select of one
// lives at a link-time-constant address, so storage_ptr is hoisted out of the
// loop and the wake compares the net old->new value with edge semantics. An
// array element's address was resolved when the statement executed and travels
// in the record, so it is loaded per record and the wake is level-only.
void Backend::emit_intra_commit(Signal sig) {
    const SignalSlot& s = slot_of(sig);
    if (s.is_agg) {
        emit_intra_agg_commit(sig);
        return;
    }
    const uint32_t    W    = s.width;
    auto*             i32  = b_.getInt32Ty();
    auto*             ptr  = raw_ptr_ty();
    auto*             mmty = int_ty(s.four_state ? 2 * W : W);
    auto*             zty  = int_ty(W);
    auto*             rt   = intra_rec_ty(s);
    const llvm::Align a1{ 1 };
    const uint32_t    idx  = intra_idx(sig);
    // A scalar destination is a typed GEP into design state; an array element
    // pointer arrives opaque from the record, but design state is rt_alloc'd
    // (64-byte aligned) and elements sit at naturally aligned offsets, so the
    // ABI alignment holds either way -- emit_array_commit relies on the same.
    const llvm::Align mm_al = env_->llmod.getDataLayout().getABITypeAlign(mmty);
    const llvm::Align z_al  = env_->llmod.getDataLayout().getABITypeAlign(zty);

    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* run   = llvm::BasicBlock::Create(ctx_, "intra.run", fn);
    auto* head  = llvm::BasicBlock::Create(ctx_, "intra.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "intra.body", fn);
    auto* after = llvm::BasicBlock::Create(ctx_, "intra.after", fn);
    auto* skip  = llvm::BasicBlock::Create(ctx_, "intra.skip", fn);

    // No fired record for this signal -> nothing to commit / wake / release.
    llvm::Value* n = b_.CreateCall(intra_ready_count_fn(), { b_.getInt32(idx) });
    b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), run, skip);

    b_.SetInsertPoint(run);
    llvm::Value* base    = b_.CreateCall(intra_ready_base_fn(), { b_.getInt32(idx) });
    llvm::Value* live_mm = nullptr;
    llvm::Value* live_z  = nullptr;
    llvm::Value* old_mm  = nullptr;
    llvm::Value* old_z   = nullptr;
    if (!s.is_array && !s.intra_ptr) {
        llvm::Value* live = storage_ptr(s);
        live_mm           = b_.CreateStructGEP(s.slot_ty, live, 0);
        live_z            = s.has_z ? b_.CreateStructGEP(s.slot_ty, live, 1) : nullptr;
        old_mm            = b_.CreateAlignedLoad(mmty, live_mm, mm_al);
        old_z             = s.has_z ? b_.CreateAlignedLoad(zty, live_z, z_al) : nullptr;
    }
    auto* pre = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* i = b_.CreatePHI(i32, 2);
    i->addIncoming(b_.getInt32(0), pre);
    b_.CreateCondBr(b_.CreateICmpULT(i, n), body, after);

    b_.SetInsertPoint(body);
    // A handle-written member wakes inside this loop, and the wake can release a record of
    // this very site: that pushes onto the list the loop reads through, and a push may
    // reallocate it. So that path re-reads the list's base for each record rather than trust
    // the pointer taken before the loop.
    llvm::Value* rec_base = s.intra_ptr ? b_.CreateCall(intra_ready_base_fn(), { b_.getInt32(idx) }) : base;
    llvm::Value* recp     = b_.CreateLoad(ptr, b_.CreateInBoundsGEP(ptr, rec_base, i));
    llvm::Value* dst_mm = live_mm;
    llvm::Value* dst_z  = live_z;
    if (s.is_array || s.intra_ptr) {
        dst_mm = b_.CreateAlignedLoad(ptr, b_.CreateStructGEP(rt, recp, intra_rec_mm_ptr(s)), a1);
        dst_z  = s.has_z
                   ? b_.CreateAlignedLoad(ptr, b_.CreateStructGEP(rt, recp, intra_rec_z_ptr(s)), a1)
                   : nullptr;
    }
    // A handle-written member wakes PER RECORD, not once for the pass: two
    // records of the same signal may have landed in different instances, and a
    // scalar member has real edge semantics (`@(posedge intf.sig)` is legal,
    // unlike `@(posedge mem[i])`). Snapshot before the store so the transition
    // can be classified after it -- exactly emit_array_commit's vif_rec path.
    llvm::Value* rec_old_mm =
      s.intra_ptr ? b_.CreateAlignedLoad(mmty, dst_mm, mm_al) : nullptr;
    llvm::Value* rec_old_z =
      (s.intra_ptr && s.has_z) ? b_.CreateAlignedLoad(zty, dst_z, z_al) : nullptr;
    llvm::Value* mm_val = b_.CreateAlignedLoad(mmty, b_.CreateStructGEP(rt, recp, 6), a1);
    llvm::Value* mask   = nullptr;
    if (s.intra_masked) {
        // A partial write merges into the live word. The mask is one bit per
        // storage bit, so the packed must|may word needs it doubled.
        mask               = b_.CreateAlignedLoad(zty, b_.CreateStructGEP(rt, recp, intra_rec_mask(s)), a1);
        llvm::Value* mask2 = !s.four_state ? mask
                                           : b_.CreateOr(b_.CreateZExt(mask, mmty),
                                                         b_.CreateShl(b_.CreateZExt(mask, mmty),
                                                                      llvm::ConstantInt::get(mmty, W)));
        llvm::Value* old   = b_.CreateAlignedLoad(mmty, dst_mm, mm_al);
        b_.CreateAlignedStore(
          b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask2)), b_.CreateAnd(mm_val, mask2)), dst_mm,
          mm_al);
    } else {
        b_.CreateAlignedStore(mm_val, dst_mm, mm_al);
    }
    if (s.has_z) {
        llvm::Value* z_val = b_.CreateAlignedLoad(zty, b_.CreateStructGEP(rt, recp, 7), a1);
        if (s.intra_masked) {
            llvm::Value* old = b_.CreateAlignedLoad(zty, dst_z, z_al);
            b_.CreateAlignedStore(
              b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask)), b_.CreateAnd(z_val, mask)), dst_z,
              z_al);
        } else {
            b_.CreateAlignedStore(z_val, dst_z, z_al);
        }
    }
    if (s.intra_ptr) {
        // Re-read through the recorded pointer: a masked merge means the
        // committed word is not the staged one.
        llvm::Value* new_mm  = b_.CreateAlignedLoad(mmty, dst_mm, mm_al);
        llvm::Value* new_z   = s.has_z ? b_.CreateAlignedLoad(zty, dst_z, z_al) : nullptr;
        llvm::Value* rec_ctx = b_.CreateAlignedLoad(ptr, b_.CreateStructGEP(rt, recp, intra_rec_ctx(s)), a1);
        CtxOverride  ov(*this, rec_ctx, s.anchor);
        service_wait_queue(sig, rec_old_mm, new_mm, rec_old_z, new_z);
        vcd_touch_signal(sig);
    }
    i->addIncoming(b_.CreateAdd(i, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(after);
    if (s.intra_ptr) {
        // Already woken per record above, with that record's own instance and
        // edge facts; a second whole-pass wake would report the representative
        // instance's transition instead.
    } else if (s.is_array) {
        // An element write carries no edge semantics (slang rejects
        // @(posedge mem[i])), so wake the array's level readers -- the service
        // the plain array commit uses. An array signal has no nba_id, so the
        // edge-aware nba_meta path is not merely unnecessary but invalid: it
        // would index the meta table out of bounds. `after` is reached only when
        // a record committed, which is the write-happened guard.
        if (signal_has_waiters(sig)) {
            service_any_waiters(sig);
        }
        vcd_touch_array(sig);
    } else {
        // Wake edge readers once, on the net change over all committed records.
        if (signal_has_waiters(sig)) {
            llvm::Value* new_mm = b_.CreateAlignedLoad(mmty, live_mm, mm_al);
            llvm::Value* new_z  = s.has_z ? b_.CreateAlignedLoad(zty, live_z, z_al) : nullptr;
            llvm::Value* meta   = b_.CreateInBoundsGEP(nba_meta_arr_ty_, sym(nba_meta_gv_),
                                                       { b_.getInt32(0), b_.getInt32(s.nba_id) });
            emit_nba_service(sig, meta, old_mm, new_mm, W, s.four_state, old_z, new_z);
        }
        if (design_uses_vcd_ && nba_vcd_gv_ != nullptr) {
            emit_guarded_vcd_touch(vcd_touch_fn(), [&] {
                return llvm::SmallVector<llvm::Value*, 2>{ b_.CreateLoad(
                  i32, b_.CreateInBoundsGEP(nba_vcd_gv_->getValueType(), sym(nba_vcd_gv_),
                                            { b_.getInt32(0), b_.getInt32(s.nba_id) })) };
            });
        }
    }
    // Only the `n` records this commit applied are released. The wake above can satisfy a
    // `<= @` control of this very signal, and the record it releases is an update created
    // in this region: it commits in the next pass, not freed unapplied with the rest.
    b_.CreateCall(intra_rec_rooted(s) ? intra_release_rooted_fn() : intra_release_fn(), { b_.getInt32(idx), n });
    b_.CreateBr(skip);

    b_.SetInsertPoint(skip);
}

// Commit an aggregate intra-NBA signal's fired records: memcpy each record's
// byte payload into the destination slot it captured at arm time, or a
// byte-wise masked read-modify-write when the leaf write is partial. Mirrors
// emit_agg_array_commit. An aggregate has no flat value, so readers wake
// level-only rather than through the edge-aware nba_meta comparison.
void Backend::emit_intra_agg_commit(Signal sig) {
    const SignalSlot& s   = slot_of(sig);
    auto*             i32 = b_.getInt32Ty();
    auto*             i8  = b_.getInt8Ty();
    auto*             ptr = raw_ptr_ty();
    auto*             rt  = intra_rec_ty(s);
    const llvm::Align a1{ 1 };
    const uint32_t    idx = intra_idx(sig);

    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* run   = llvm::BasicBlock::Create(ctx_, "intraagg.run", fn);
    auto* head  = llvm::BasicBlock::Create(ctx_, "intraagg.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "intraagg.body", fn);
    auto* after = llvm::BasicBlock::Create(ctx_, "intraagg.after", fn);
    auto* skip  = llvm::BasicBlock::Create(ctx_, "intraagg.skip", fn);

    llvm::Value* n = b_.CreateCall(intra_ready_count_fn(), { b_.getInt32(idx) });
    b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), run, skip);

    b_.SetInsertPoint(run);
    llvm::Value* base = b_.CreateCall(intra_ready_base_fn(), { b_.getInt32(idx) });
    auto*        pre  = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* k = b_.CreatePHI(i32, 2);
    k->addIncoming(b_.getInt32(0), pre);
    b_.CreateCondBr(b_.CreateICmpULT(k, n), body, after);

    b_.SetInsertPoint(body);
    llvm::Value* recp    = b_.CreateLoad(ptr, b_.CreateInBoundsGEP(ptr, base, k));
    llvm::Value* dest    = b_.CreateAlignedLoad(ptr, b_.CreateStructGEP(rt, recp, 6), a1);
    llvm::Value* nbytes  = b_.CreateAlignedLoad(i32, b_.CreateStructGEP(rt, recp, 7), a1);
    llvm::Value* payload = b_.CreateStructGEP(rt, recp, intra_agg_payload(s));
    if (!s.intra_masked) {
        b_.CreateMemCpy(dest, llvm::MaybeAlign{ 1 }, payload, llvm::MaybeAlign{ 1 }, nbytes);
    } else {
        // Byte-wise masked read-modify-write: dest[j] = (dest[j] & ~m) | (val[j] & m).
        // Width-agnostic -- a mask byte of 0 leaves dest[j] unchanged, so an
        // unselected bit (or a garbage padding byte gating dest padding) is a
        // no-op.
        llvm::Value* mask_r = b_.CreateStructGEP(rt, recp, intra_agg_payload(s) + 1);
        auto*        bh     = llvm::BasicBlock::Create(ctx_, "intraagg.bh", fn);
        auto*        bb     = llvm::BasicBlock::Create(ctx_, "intraagg.bb", fn);
        auto*        bd     = llvm::BasicBlock::Create(ctx_, "intraagg.bd", fn);
        auto*        bpre   = b_.GetInsertBlock();
        b_.CreateBr(bh);
        b_.SetInsertPoint(bh);
        auto* j = b_.CreatePHI(i32, 2);
        j->addIncoming(b_.getInt32(0), bpre);
        b_.CreateCondBr(b_.CreateICmpULT(j, nbytes), bb, bd);
        b_.SetInsertPoint(bb);
        llvm::Value* dj  = b_.CreateInBoundsGEP(i8, dest, { j });
        llvm::Value* m   = b_.CreateLoad(i8, b_.CreateInBoundsGEP(i8, mask_r, { j }));
        llvm::Value* v   = b_.CreateLoad(i8, b_.CreateInBoundsGEP(i8, payload, { j }));
        llvm::Value* old = b_.CreateLoad(i8, dj);
        b_.CreateStore(b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(m)), b_.CreateAnd(v, m)), dj);
        j->addIncoming(b_.CreateAdd(j, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(bh);
        b_.SetInsertPoint(bd);
    }
    k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(after);
    if (signal_has_waiters(sig)) {
        service_any_waiters(sig);
    }
    b_.CreateCall(intra_rec_rooted(s) ? intra_release_rooted_fn() : intra_release_fn(), { b_.getInt32(idx), n });
    b_.CreateBr(skip);

    b_.SetInsertPoint(skip);
}

void Backend::emit_array_commit(Signal sig, llvm::Value* state) {
    (void)state;
    const SignalSlot& s      = slot_of(sig);
    const uint32_t    W      = s.width;
    auto*             i32    = b_.getInt32Ty();
    auto*             ptr    = raw_ptr_ty();
    auto*             i8     = b_.getInt8Ty();
    auto*             mmty   = int_ty(s.four_state ? 2 * W : W);
    auto*             zty    = int_ty(W);
    auto*             rt     = arr_rec_ty(s);
    const uint32_t    stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(rt));
    const llvm::Align a1{ 1 };
    // The live element pointer arrives opaque from the record, but the design
    // state is rt_alloc'd (64-byte aligned) and array elements sit at naturally
    // aligned offsets, so each element word meets its ABI alignment.
    const llvm::Align mm_al  = env_->llmod.getDataLayout().getABITypeAlign(mmty);
    const llvm::Align z_al   = env_->llmod.getDataLayout().getABITypeAlign(zty);

    llvm::Value* n    = b_.CreateCall(nba_arr_count_fn(), { b_.getInt32(arr_idx(sig)) });
    llvm::Value* base = b_.CreateCall(nba_arr_base_fn(), { b_.getInt32(arr_idx(sig)) });
    auto*        fn    = b_.GetInsertBlock()->getParent();
    auto*        head  = llvm::BasicBlock::Create(ctx_, "arr.head", fn);
    auto*        body  = llvm::BasicBlock::Create(ctx_, "arr.body", fn);
    auto*        done  = llvm::BasicBlock::Create(ctx_, "arr.done", fn);
    auto*        entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, n), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* recp   = b_.CreateInBoundsGEP(i8, base, b_.CreateMul(idx, b_.getInt32(stride)));
    llvm::Value* mm_ptr = b_.CreateLoad(ptr, b_.CreateStructGEP(rt, recp, 0));
    llvm::Value* mm_val = b_.CreateAlignedLoad(mmty, b_.CreateStructGEP(rt, recp, 1), a1);
    // A member written through a virtual interface wakes per record, not once
    // for the whole pass: each record may have landed in a different instance,
    // and a scalar member has real edge semantics (an array element does not).
    // Snapshot before the store so the transition can be classified after it.
    llvm::Value* old_mm = s.vif_rec ? b_.CreateAlignedLoad(mmty, mm_ptr, mm_al) : nullptr;
    llvm::Value* old_z  = nullptr;
    llvm::Value* mask   = nullptr;
    if (s.arr_masked) {
        mask = b_.CreateAlignedLoad(zty, b_.CreateStructGEP(rt, recp, arr_rec_mask(s)), a1);
        llvm::Value* mask2 = !s.four_state ? mask
                                           : b_.CreateOr(b_.CreateZExt(mask, mmty),
                                                         b_.CreateShl(b_.CreateZExt(mask, mmty),
                                                                      llvm::ConstantInt::get(mmty, W)));
        llvm::Value* old   = b_.CreateAlignedLoad(mmty, mm_ptr, mm_al);
        b_.CreateAlignedStore(
          b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask2)), b_.CreateAnd(mm_val, mask2)), mm_ptr, mm_al);
    } else {
        b_.CreateAlignedStore(mm_val, mm_ptr, mm_al);
    }
    llvm::Value* new_z = nullptr;
    if (s.has_z) {
        llvm::Value* z_ptr = b_.CreateLoad(ptr, b_.CreateStructGEP(rt, recp, arr_rec_z_ptr(s)));
        llvm::Value* z_val = b_.CreateAlignedLoad(zty, b_.CreateStructGEP(rt, recp, arr_rec_z_val(s)), a1);
        if (s.vif_rec) {
            old_z = b_.CreateAlignedLoad(zty, z_ptr, z_al);
        }
        if (s.arr_masked) {
            llvm::Value* old = b_.CreateAlignedLoad(zty, z_ptr, z_al);
            b_.CreateAlignedStore(
              b_.CreateOr(b_.CreateAnd(old, b_.CreateNot(mask)), b_.CreateAnd(z_val, mask)), z_ptr, z_al);
        } else {
            b_.CreateAlignedStore(z_val, z_ptr, z_al);
        }
    }
    if (s.vif_rec) {
        // Re-read through the recorded pointer: the store above may have been a
        // masked merge, so the committed word is not the staged one.
        llvm::Value* new_mm = b_.CreateAlignedLoad(mmty, mm_ptr, mm_al);
        if (s.has_z) {
            llvm::Value* z_ptr =
              b_.CreateLoad(ptr, b_.CreateStructGEP(rt, recp, arr_rec_z_ptr(s)));
            new_z = b_.CreateAlignedLoad(zty, z_ptr, z_al);
        }
        llvm::Value* rec_ctx = b_.CreateLoad(ptr, b_.CreateStructGEP(rt, recp, arr_rec_ctx(s)));
        CtxOverride  ov(*this, rec_ctx, s.anchor);
        service_wait_queue(sig, old_mm, new_mm, old_z, new_z);
        vcd_touch_signal(sig);
    }
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
    // Wake the array's level readers if it took any write this pass. A vif
    // member woke per record above, with its own edge facts.
    if (!s.vif_rec && signal_has_waiters(sig)) {
        auto* svc  = llvm::BasicBlock::Create(ctx_, "arr.svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "arr.svc.cont", fn);
        b_.CreateCondBr(b_.CreateICmpNE(n, b_.getInt32(0)), svc, cont);
        b_.SetInsertPoint(svc);
        service_any_waiters(sig);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

} // namespace clotho::ir::backend
