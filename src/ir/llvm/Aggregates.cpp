// Aggregate storage: leaf layout, initialization, copy and constant images.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Leaf slot type for a scalar / real aggregate leaf. A 4-state leaf carries a
// z plane conservatively (there is no per-leaf may-z analysis under an
// aggregate), so z assigned to a member is representable; a 2-state / real
// leaf is the narrow single-iW form.
llvm::StructType* Backend::agg_leaf_slot_ty(Ref type) {
    const Layout   lay  = mod_.pool.layout(type);
    const uint32_t w    = lay.width == 0 ? 1 : lay.width;
    const bool     four = lay.domain == Domain::Four;
    return slot_type(w, four, /*has_z=*/four, /*narrow=*/!four);
}

// Recursive LLVM storage type mirroring an (aggregate) IR type: a nested
// StructType per unpacked struct, one ArrayType per unpacked-array dimension,
// and a leaf slot for each scalar / string / real. Unlike the flat memory
// path this does NOT flatten array dimensions, so arbitrary nesting
// (array-of-struct, struct-with-array-member, ...) is a direct mirror.
llvm::Type* Backend::agg_storage_ty(Ref type) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        return llvm::ArrayType::get(agg_storage_ty(t.element), array_dim(t));
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        std::vector<llvm::Type*> fields;
        for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
            fields.push_back(agg_storage_ty(f.type));
        }
        return llvm::StructType::create(ctx_, fields, "clotho.agg");
    }
    if (t.kind == Type::Kind::String) {
        return str_slot_ty();
    }
    if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue ||
        t.kind == Type::Kind::Assoc) {
        return handle_slot_ty(type);
    }
    return agg_leaf_slot_ty(type);
}

// Power-on the properties of a freshly-allocated class object body (LRM 8.8):
// the caller has already zeroed the block (2-state leaves and class-handle
// properties power on 0 / null), so this writes X into 4-state leaves and
// initializes string leaves, per-property, reusing emit_agg_init.
void Backend::emit_class_body_init(Ref class_ref, llvm::Value* body_ptr) {
    const Type&               t       = std::get<Type>(mod_.pool.get(class_ref));
    llvm::Type*               root_ty = class_body_ty(class_ref);
    std::vector<llvm::Value*> path;
    const uint32_t            hdr = class_header_words(class_ref);
    uint32_t                  f   = 0;
    for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
        path.push_back(b_.getInt32(hdr + f++));
        emit_agg_init(fld.type, body_ptr, path, root_ty);
        path.pop_back();
    }
}

// Copy a whole aggregate from src to dst (both pointers to its storage). One
// memcpy when no leaf owns heap — its byte image is then the value; otherwise a
// per-leaf deep copy, since duplicating a string / container leaf's bytes would
// duplicate ownership of its allocation (emit_agg_deep_copy).
void Backend::emit_agg_storage_copy(llvm::Value* dst, llvm::Value* src, Ref type) {
    if (has_heap_leaf(type)) {
        std::vector<llvm::Value*> path;
        emit_agg_deep_copy(type, dst, src, path, agg_storage_ty(type));
        return;
    }
    llvm::Type* st = agg_storage_ty(type);
    const auto  sz = env_->llmod.getDataLayout().getTypeAllocSize(st);
    const auto  al = env_->llmod.getDataLayout().getABITypeAlign(st);
    b_.CreateMemCpy(dst, al, src, al, sz);
}

// Fill an LValue's leaf metadata (slot type, width, domain flags) from the
// scalar / string / handle leaf type a select chain has resolved to.
// Conservative z for a 4-state leaf, matching agg_leaf_slot_ty.
void Backend::fill_agg_leaf(LValue& lv, Ref leaf_type) {
    const Type& t = std::get<Type>(mod_.pool.get(leaf_type));
    if (t.kind == Type::Kind::String) {
        lv.is_string = true;
        lv.slot_ty   = str_slot_ty();
        return;
    }
    if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue ||
        t.kind == Type::Kind::Assoc) {
        // A handle leaf is consumed only by the queue / dynamic-array /
        // associative-array ops (which address it via lv_handle_ptr) and by
        // ElemSelect / AssocSelect descent; a plane Load/Store on it is a
        // lowering bug and traps.
        lv.is_handle   = true;
        lv.handle_type = leaf_type;
        lv.slot_ty     = handle_slot_ty(leaf_type);
        return;
    }
    const Layout   lay  = mod_.pool.layout(leaf_type);
    const uint32_t w    = lay.width == 0 ? 1 : lay.width;
    const bool     four = lay.domain == Domain::Four;
    lv.store_width = w;
    lv.width       = w;
    lv.has_z       = four;
    lv.narrow      = !four;
    lv.slot_ty     = slot_type(w, four, four, !four);
    lv.bit_off     = b_.getInt32(0);
}

// Enumerate an aggregate's scalar/string leaves reachable by a STATIC path
// (struct fields only). Recursion stops at an unpacked-array dimension — a
// leaf under an array has a runtime element index, so it is not static and
// takes the deferred nonblocking path, not the block-commit shadow. `path`
// accumulates the constant GEP field indices from the aggregate root.
void Backend::collect_static_leaves(Ref type, std::vector<uint32_t>& path, std::vector<AggLeaf>& out) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        return;
    }
    // A handle leaf (dynamic array / queue) can never be nonblocking-staged
    // (guarded in the frontend), so it gets no shadow entry.
    if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue) {
        return;
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            path.push_back(f++);
            collect_static_leaves(fld.type, path, out);
            path.pop_back();
        }
        return;
    }
    AggLeaf leaf;
    leaf.path = path;
    if (t.kind == Type::Kind::String) {
        leaf.is_string = true;
        leaf.slot_ty   = str_slot_ty();
    } else {
        const Layout lay = mod_.pool.layout(type);
        leaf.width       = lay.width == 0 ? 1 : lay.width;
        leaf.four_state  = lay.domain == Domain::Four;
        leaf.has_z       = leaf.four_state;
        leaf.narrow      = !leaf.four_state;
        leaf.slot_ty     = slot_type(leaf.width, leaf.four_state, leaf.four_state, !leaf.four_state);
    }
    out.push_back(leaf);
}

// Whether an IR type is a scalar / string / real leaf (as opposed to an
// unpacked struct or array that a select must descend further).
bool Backend::is_leaf_type(Ref type) const {
    return !types::is_aggregate(mod_.pool, type);
}

// Derive member `field_index`'s lvalue from an aggregate lvalue: push the
// constant field index onto the aggregate GEP path and resolve the leaf
// metadata if the member is a scalar/string (a struct/array member stays
// interior for a further select). Shared by MemberSelect and the AggEq
// member walk.
LValue Backend::agg_member(LValue lv, Ref struct_type, uint32_t field_index) {
    const Type& st = std::get<Type>(mod_.pool.get(struct_type));
    const Ref   mt = mod_.pool.struct_fields(st.fields_id)[field_index].type;
    // Skip a polymorphic class's vtable-pointer header (LLVM field 0); the
    // logical field path stays 0-based (only signal-shadow leaf matching uses
    // it, and class objects are heap, not signals).
    // True while nothing has been selected yet, i.e. this member is reached
    // DIRECTLY off the object body -- read before the push below.
    const bool off_body = lv.agg_path.empty();
    lv.agg_path.push_back(b_.getInt32(field_index + class_header_words(struct_type)));
    lv.agg_field_path.push_back(field_index);
    // The change notifiers of a waited-on class property (LRM 9.4.2), resolved
    // here because this is the one member-GEP chokepoint. Only for a property off
    // the body, so `chg_body` is that body and a notifier is a sibling field of
    // it. Assigned unconditionally, so descending into an interior aggregate
    // clears it: a notifier belongs to the whole property, not to a leaf in it.
    lv.chg = off_body ? mod_.change_notify(struct_type, field_index) : nullptr;
    if (lv.chg != nullptr) {
        lv.chg_body    = lv.base;
        lv.chg_body_ty = lv.root_ty;
    }
    // The write barrier's precision knob (§7): a member that cannot reach a class
    // handle cannot be the store that needs remembering, so `obj.count = 5` and
    // `obj.name = "x"` drop the owner here and emit nothing, while `obj.h`,
    // `obj.q` and `obj.nested.h` keep it. reaches_class_handle is the transitive
    // question, so an aggregate member and a nested struct member keep the owner
    // of the object they belong to -- which is the right owner, since an interior
    // aggregate is part of that same object's traced image.
    if (lv.gc_owner != nullptr && !reaches_class_handle(mt)) {
        lv.gc_owner = nullptr;
        // Root and owner travel together, so that `gc_container_root != nullptr`
        // always implies an owner. `obj.qq[1].count` reaches an int leaf inside a
        // payload: no GC edge can land there, so a `ref` to it needs no container
        // registration any more than it needs a barrier.
        lv.gc_container_root = nullptr;
        lv.gc_container_type = Ref::Invalid;
    }
    lv.agg_leaf = is_leaf_type(mt);
    if (lv.agg_leaf) {
        fill_agg_leaf(lv, mt);
    }
    return lv;
}

// Derive the `pos`-th (0-based) element's lvalue from a fixed unpacked-array
// lvalue, for a constant-index aggregate walk (whole-array `==` / copy). A
// recursive-storage (is_agg) array pushes a nested-GEP index step; a flat
// memory advances the single flattened leaf index by pos * inner leaf count
// -- the same two storage models the runtime ElemSelect path handles, minus
// the dyn_inrange gate (an in-bounds constant position is always valid).
LValue Backend::agg_elem(LValue lv, Ref array_type, uint32_t pos) {
    const Type& at = std::get<Type>(mod_.pool.get(array_type));
    if (lv.is_agg) {
        lv.agg_path.push_back(b_.getInt32(pos));
        lv.agg_leaf = is_leaf_type(at.element);
        if (lv.agg_leaf) {
            fill_agg_leaf(lv, at.element);
        }
        return lv;
    }
    const uint32_t stride = array_leaf_count(at.element);
    llvm::Value*   off    = b_.getInt32(pos * stride);
    lv.elem_idx           = lv.elem_idx != nullptr ? b_.CreateAdd(lv.elem_idx, off) : off;
    lv.bit_off            = b_.getInt32(0);
    return lv;
}

// Recursive power-on fill for an aggregate signal: X into every 4-state leaf
// and rt_string_init on every string leaf. 2-state / real leaves are already
// zero from the state global's zeroinitializer. Array dimensions become
// runtime loops (keeping init O(size) in emitted instructions), struct fields
// straight-line GEPs. `root_ty` is the aggregate's storage type; `path`
// accumulates GEP indices from its root.
void Backend::emit_agg_init(Ref type, llvm::Value* base, std::vector<llvm::Value*>& path, llvm::Type* root_ty) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        const uint32_t n     = array_dim(t);
        auto*          i32   = b_.getInt32Ty();
        auto*          fn    = b_.GetInsertBlock()->getParent();
        auto*          head  = llvm::BasicBlock::Create(ctx_, "ainit.head", fn);
        auto*          body  = llvm::BasicBlock::Create(ctx_, "ainit.body", fn);
        auto*          done  = llvm::BasicBlock::Create(ctx_, "ainit.done", fn);
        auto*          entry = b_.GetInsertBlock();
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(n)), body, done);
        b_.SetInsertPoint(body);
        path.push_back(idx);
        emit_agg_init(t.element, base, path, root_ty);
        path.pop_back();
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
        return;
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            path.push_back(b_.getInt32(f++));
            emit_agg_init(fld.type, base, path, root_ty);
            path.pop_back();
        }
        return;
    }
    // Leaf: GEP through the path to the slot, then init only if it needs a
    // non-zero power-on (4-state -> X, string -> handle).
    std::vector<llvm::Value*> idx;
    idx.reserve(path.size() + 2);
    idx.push_back(b_.getInt32(0));
    for (llvm::Value* s : path) {
        idx.push_back(s);
    }
    if (t.kind == Type::Kind::Event) {
        // LRM Table 6-7: an event's default value is a NEW event, so every event
        // leaf of an aggregate -- an array element, a struct member, a class
        // property -- binds its own object. This one arm serves all three,
        // because module signals (init_state), frame locals (init_frame) and
        // class object bodies (emit_class_body_init) all reach this walk.
        b_.CreateStore(b_.CreateCall(event_new_fn()),
                       b_.CreateInBoundsGEP(root_ty, base, idx));
        return;
    }
    if (t.kind == Type::Kind::String) {
        b_.CreateCall(str_init_fn(), { b_.CreateInBoundsGEP(root_ty, base, idx) });
        return;
    }
    if (t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue ||
        t.kind == Type::Kind::Assoc) {
        // Empty handle (null buffer, zero counts). A signal slot is already
        // zero from the state global's initializer; a frame alloca is not.
        b_.CreateStore(llvm::Constant::getNullValue(handle_slot_ty(type)),
                       b_.CreateInBoundsGEP(root_ty, base, idx));
        return;
    }
    const Layout lay  = mod_.pool.layout(type);
    const bool   four = lay.domain == Domain::Four;
    if (!four) {
        return; // 2-state / real: zeroinitializer is the power-on value.
    }
    const uint32_t w = lay.width == 0 ? 1 : lay.width;
    idx.push_back(b_.getInt32(0)); // mm field of the leaf slot
    b_.CreateStore(x_mm(w), b_.CreateInBoundsGEP(root_ty, base, idx));
}

// Recursive teardown for an aggregate's heap-owning leaves: rt_string_deinit
// on every string leaf, rt_dynarr_delete / rt_queue_delete_all on every
// handle leaf. The mirror of emit_agg_init; plane leaves need nothing. Call
// sites gate on has_heap_leaf so a plane-only aggregate emits no walk.
void Backend::emit_agg_deinit(Ref type, llvm::Value* base, std::vector<llvm::Value*>& path, llvm::Type* root_ty) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        if (!has_heap_leaf(t.element)) {
            return;
        }
        const uint32_t n     = array_dim(t);
        auto*          i32   = b_.getInt32Ty();
        auto*          fn    = b_.GetInsertBlock()->getParent();
        auto*          head  = llvm::BasicBlock::Create(ctx_, "adeinit.head", fn);
        auto*          body  = llvm::BasicBlock::Create(ctx_, "adeinit.body", fn);
        auto*          done  = llvm::BasicBlock::Create(ctx_, "adeinit.done", fn);
        auto*          entry = b_.GetInsertBlock();
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(n)), body, done);
        b_.SetInsertPoint(body);
        path.push_back(idx);
        emit_agg_deinit(t.element, base, path, root_ty);
        path.pop_back();
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
        return;
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            path.push_back(b_.getInt32(f++));
            emit_agg_deinit(fld.type, base, path, root_ty);
            path.pop_back();
        }
        return;
    }
    const bool is_str    = t.kind == Type::Kind::String;
    const bool is_handle = t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue ||
                           t.kind == Type::Kind::Assoc;
    if (!is_str && !is_handle) {
        return;
    }
    std::vector<llvm::Value*> idx;
    idx.reserve(path.size() + 1);
    idx.push_back(b_.getInt32(0));
    for (llvm::Value* s : path) {
        idx.push_back(s);
    }
    llvm::Value* slot = b_.CreateInBoundsGEP(root_ty, base, idx);
    if (is_str) {
        b_.CreateCall(str_deinit_fn(), { slot });
    } else {
        emit_handle_delete(slot, type);
    }
}

// Recursive per-leaf deep copy of one aggregate storage image into another (both
// pointers to `root_ty`, addressed by the same GEP path): every string leaf is
// rt_string_assign'd and every container leaf deep-copied, so the destination
// ends up owning its own heap. A subtree with no heap leaf is one memcpy — there
// the byte image IS the value.
//
// This is what a BY-VALUE copy of a managed-leaf aggregate needs and what
// emit_agg_storage_copy cannot do: duplicating the bytes of a string or
// container leaf would leave two owners of one allocation, and the second
// deinit would free it twice (docs/design/data-model.md §6, the "Aggregate,
// managed leaf" row). The sibling of emit_agg_init / emit_agg_deinit, walking
// the same array loop and struct recursion.
//
// Precondition: the destination's managed leaves are already CONSTRUCTED, which
// every storage this runs on satisfies — a frame slot (init_frame), a signal
// (init_state), a proto-filled call temp. rt_string_assign / the handle assign
// release what the destination held, so a repeated copy into the same storage
// neither leaks nor double-frees.
void Backend::emit_agg_deep_copy(Ref                        type,
                                 llvm::Value*               dst,
                                 llvm::Value*               src,
                                 std::vector<llvm::Value*>& path,
                                 llvm::Type*                root_ty) {
    const auto sub_ptr = [&](llvm::Value* base) {
        std::vector<llvm::Value*> idx;
        idx.reserve(path.size() + 1);
        idx.push_back(b_.getInt32(0));
        for (llvm::Value* s : path) {
            idx.push_back(s);
        }
        return b_.CreateInBoundsGEP(root_ty, base, idx);
    };
    if (!has_heap_leaf(type)) {
        // Plane-only subtree: one memcpy of its image, whatever its shape. This
        // is the LLVM-side equivalent of rt_elem_copy's POD-gap walk.
        llvm::Type* st = agg_storage_ty(type);
        const auto  sz = env_->llmod.getDataLayout().getTypeAllocSize(st);
        const auto  al = env_->llmod.getDataLayout().getABITypeAlign(st);
        b_.CreateMemCpy(sub_ptr(dst), al, sub_ptr(src), al, sz);
        return;
    }
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        const uint32_t n     = array_dim(t);
        auto*          i32   = b_.getInt32Ty();
        auto*          fn    = b_.GetInsertBlock()->getParent();
        auto*          head  = llvm::BasicBlock::Create(ctx_, "acopy.head", fn);
        auto*          body  = llvm::BasicBlock::Create(ctx_, "acopy.body", fn);
        auto*          done  = llvm::BasicBlock::Create(ctx_, "acopy.done", fn);
        auto*          entry = b_.GetInsertBlock();
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* idx = b_.CreatePHI(i32, 2);
        idx->addIncoming(b_.getInt32(0), entry);
        b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(n)), body, done);
        b_.SetInsertPoint(body);
        path.push_back(idx);
        emit_agg_deep_copy(t.element, dst, src, path, root_ty);
        path.pop_back();
        idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
        return;
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            path.push_back(b_.getInt32(f++));
            emit_agg_deep_copy(fld.type, dst, src, path, root_ty);
            path.pop_back();
        }
        return;
    }
    // A heap-owning leaf: has_heap_leaf above proved it is a string or a
    // container handle, so no plane leaf reaches here.
    llvm::Value* d = sub_ptr(dst);
    llvm::Value* s = sub_ptr(src);
    if (t.kind == Type::Kind::String) {
        b_.CreateCall(str_assign_fn(), { d, s, alloc_site_global(SourceText{ .where = "<unknown>" }, "string") });
        return;
    }
    emit_handle_assign(d, s, type);
}

// One leaf's slot constant from its canonical val / xz planes: `mm` packs
// must | may << W (or the bare value for a narrow 2-state slot), plus the z
// plane when the slot keeps one. The compile-time twin of lower_constant's
// VPI ingest.
llvm::Constant* Backend::const_leaf(llvm::StructType*        slot_ty,
                                    uint32_t                 width,
                                    bool                     four_state,
                                    bool                     narrow,
                                    bool                     has_z,
                                    std::span<const uint8_t> planes) {
    const uint32_t    pb  = plane_bytes(width);
    const llvm::APInt val = apint_from_bytes(planes.subspan(0, pb), width);
    const llvm::APInt xz =
      four_state ? apint_from_bytes(planes.subspan(pb, pb), width) : llvm::APInt(width, 0);

    std::vector<llvm::Constant*> fields;
    if (narrow) {
        fields.push_back(llvm::ConstantInt::get(ctx_, val));
    } else {
        const llvm::APInt must = val & ~xz;
        const llvm::APInt may  = val | xz;
        fields.push_back(llvm::ConstantInt::get(
          ctx_, must.zext(2 * width) | (may.zext(2 * width) << width)));
    }
    if (has_z) {
        fields.push_back(llvm::ConstantInt::get(ctx_, xz & ~val));
    }
    return llvm::ConstantStruct::get(slot_ty, fields);
}

llvm::Constant* Backend::const_slot(const SignalSlot& s, std::span<const uint8_t> planes) {
    return const_leaf(s.slot_ty, s.width, s.four_state, s.narrow, s.has_z, planes);
}

// Recursive baked image for an aggregate constant table, walking the IR type
// and the signal's own storage type in lockstep and consuming the pool image
// leaf-major (the order append_const_image wrote it). The storage type is
// threaded rather than rebuilt because agg_storage_ty mints a fresh
// StructType per call, so only the signal's own types have the right
// identity. An aggregate leaf slot always carries its z plane
// (agg_leaf_slot_ty), which is why the flags here are type-derived.
llvm::Constant* Backend::const_agg(Ref type, llvm::Type* storage, std::span<const uint8_t>& cursor) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        auto*                        at = llvm::cast<llvm::ArrayType>(storage);
        std::vector<llvm::Constant*> elems;
        elems.reserve(at->getNumElements());
        for (uint64_t k = 0; k < at->getNumElements(); ++k) {
            elems.push_back(const_agg(t.element, at->getElementType(), cursor));
        }
        return llvm::ConstantArray::get(at, elems);
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        auto*                        st = llvm::cast<llvm::StructType>(storage);
        std::vector<llvm::Constant*> fields;
        uint32_t                     f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            fields.push_back(const_agg(fld.type, st->getElementType(f++), cursor));
        }
        return llvm::ConstantStruct::get(st, fields);
    }
    const Layout   lay  = mod_.pool.layout(type);
    const uint32_t w    = lay.width == 0 ? 1 : lay.width;
    const bool     four = lay.domain == Domain::Four;
    const uint32_t used = (four ? 2u : 1u) * plane_bytes(w);
    llvm::Constant* leaf =
      const_leaf(llvm::cast<llvm::StructType>(storage), w, four, !four, four, cursor.subspan(0, used));
    cursor = cursor.subspan(used);
    return leaf;
}

// The whole storage image of a materialized constant table (Module.h
// SignalInfo::const_data): the pool holds its canonical planes leaf-major, so
// each leaf decodes independently into this signal's slot layout.
llvm::Constant* Backend::const_data_init(const SignalSlot& s, Ref const_data) {
    std::span<const uint8_t> img = mod_.pool.bytes(const_data);
    if (s.is_agg) {
        return const_agg(s.ir_type, s.storage_ty, img);
    }
    const uint32_t el = (s.four_state ? 2u : 1u) * plane_bytes(s.width);
    if (!s.is_array) {
        return const_slot(s, img.subspan(0, el));
    }
    std::vector<llvm::Constant*> elems;
    elems.reserve(s.elem_count);
    for (uint32_t k = 0; k < s.elem_count; ++k) {
        elems.push_back(const_slot(s, img.subspan(static_cast<size_t>(k) * el, el)));
    }
    return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(s.storage_ty), elems);
}

// Power-on constant for one signal's storage. A materialized constant table
// is its own baked image (nothing ever writes it, so the static initializer
// IS the value). Other arrays and string arrays are zeroinitialized (filled
// at runtime by `init_state`); a scalar string is the empty inline handle
// (`rt_string_init`: byte 31 == str_inline_cap); a scalar plane is X
// (4-state) or 0 (2-state) with the z word set for an undriven-net may-z seed.
llvm::Constant* Backend::slot_const(const SignalSlot& s) {
    const Ref const_data = mod_.signals()[static_cast<size_t>(&s - slots_.data())].const_data;
    if (const_data != Ref::Invalid) {
        return const_data_init(s, const_data);
    }
    const auto sidx = static_cast<uint32_t>(&s - slots_.data());
    if (!event_slot_.empty() && event_slot_[sidx] != 0) {
        // A declared event powers on holding the handle of its OWN object:
        // `{ u32 slot, u32 gen }` with gen 0, the generation a statically-bound
        // object keeps forever. Baked into the state initializer rather than
        // stored at startup, so binding an event costs nothing at run time --
        // and `-> e` in a PreInit process already sees a valid handle.
        //
        // A null handle is all-zero, i.e. slot 0, which is why real slots start
        // at 1: an uninitialized or explicitly nulled event resolves to the
        // reserved null object and its trigger is the no-op LRM 15.5.5 asks for.
        // Build to the slot's ACTUAL shape: a narrow slot is one iW word, a
        // wide one packs must | may << W (a handle is definite, so both planes
        // carry it), and a slot forced to keep a z plane needs that field too.
        // Assuming the narrow shape here produced a struct initializer with the
        // wrong field count for a class-property event, whose slot is widened.
        const uint64_t                h = event_slot_[sidx];
        std::vector<llvm::Constant*>  fields;
        if (s.narrow) {
            fields.push_back(llvm::ConstantInt::get(int_ty(s.width), h));
        } else {
            llvm::APInt mm(2 * s.width, 0);
            mm |= llvm::APInt(2 * s.width, h);
            mm |= llvm::APInt(2 * s.width, h) << s.width;
            fields.push_back(llvm::ConstantInt::get(int_ty(2 * s.width), mm));
        }
        if (s.has_z) {
            fields.push_back(llvm::ConstantInt::get(int_ty(s.width), 0));
        }
        return llvm::ConstantStruct::get(s.slot_ty, fields);
    }
    if (s.is_array || s.is_agg) {
        // Zeroinitialized; the runtime fill (init_state -> emit_array_init /
        // emit_agg_init) writes X into 4-state leaves and inits string leaves.
        return llvm::ConstantAggregateZero::get(s.storage_ty);
    }
    if (s.is_dynarr) {
        // Empty array handle: { null data, 0 len } == zeroinitializer.
        return llvm::ConstantAggregateZero::get(s.storage_ty);
    }
    if (s.is_queue) {
        // Empty queue handle: { null base, 0 head, 0 len, 0 cap } == zeroinitializer.
        return llvm::ConstantAggregateZero::get(s.storage_ty);
    }
    if (s.is_assoc) {
        // Empty associative-array handle: { null impl } == zeroinitializer.
        return llvm::ConstantAggregateZero::get(s.storage_ty);
    }
    if (s.is_string) {
        // Empty inline handle (mirrors rt_string_init): the trailing length-marker
        // byte holds rt_string_inline_cap for len 0, and byte[0] is the NUL
        // terminator (already 0 from the zero-fill).
        return str_empty_const();
    }
    llvm::Constant* mm =
      s.narrow ? zero(s.width) : (s.four_state ? x_mm(s.width) : zero(2 * s.width));
    std::vector<llvm::Constant*> fields{ mm };
    if (s.has_z) {
        fields.push_back(init_z_plane(s));
    }
    return llvm::ConstantStruct::get(s.slot_ty, fields);
}

// Apply `callee` (rt_string_init / rt_string_deinit) to a string signal's
// handle, or to every element handle of an array-of-string signal (a runtime
// loop, like emit_array_init).
void Backend::emit_string_slot_op(const SignalSlot& s, llvm::FunctionCallee callee) {
    llvm::Value* base = storage_ptr(s);
    if (!s.is_array) {
        b_.CreateCall(callee, { base });
        return;
    }
    auto* arr_ty = llvm::cast<llvm::ArrayType>(s.storage_ty);
    auto* i32    = b_.getInt32Ty();
    auto* fn     = b_.GetInsertBlock()->getParent();
    auto* head   = llvm::BasicBlock::Create(ctx_, "strop.head", fn);
    auto* body   = llvm::BasicBlock::Create(ctx_, "strop.body", fn);
    auto* done   = llvm::BasicBlock::Create(ctx_, "strop.done", fn);
    auto* entry  = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(s.elem_count)), body, done);

    b_.SetInsertPoint(body);
    b_.CreateCall(callee, { b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), idx }) });
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), body);
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
}

// Initialise an unpacked-array signal: a runtime loop over its elements
// storing the per-element power-on value (X for 4-state, 0 for 2-state) into
// each leaf slot. A loop keeps init O(elem_count) emitted instructions, so a
// large memory does not bloat the IR.
void Backend::emit_array_init(const SignalSlot& s) {
    const uint32_t w   = s.width;
    auto*          i32 = b_.getInt32Ty();
    // An event array takes the flat storage model (its element is a plain
    // 64-bit word), but its power-on value is not a constant: LRM Table 6-7
    // binds each element to its OWN new object, so the loop stores a fresh
    // handle per element instead of one shared image.
    const bool     evt = std::get<Type>(mod_.pool.get(s.ir_type)).kind == Type::Kind::Event ||
                     types::has_ctor_leaf(mod_.pool, s.ir_type);
    llvm::Value*   mm  = s.narrow ? zero(w) : (s.four_state ? x_mm(w) : zero(2 * w));
    auto*        arr_ty = llvm::cast<llvm::ArrayType>(s.storage_ty);
    llvm::Value* base   = storage_ptr(s);

    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "ainit.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "ainit.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "ainit.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(s.elem_count)), body, done);

    b_.SetInsertPoint(body);
    b_.CreateStore(evt ? b_.CreateCall(event_new_fn()) : mm,
                   b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), idx, b_.getInt32(0) }));
    if (s.has_z) {
        // An undriven net array's elements default to z like any net.
        b_.CreateStore(s.init_z ? ones(w) : zero(w),
                       b_.CreateInBoundsGEP(arr_ty, base, { b_.getInt32(0), idx, b_.getInt32(1) }));
    }
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), body);
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
}

// `&array[elem_idx]` for a flat (non-aggregate) unpacked-array element
// lvalue, or the base when the whole storage is addressed.
llvm::Value* Backend::flat_elem_ptr(const LValue& lv) {
    if (lv.elem_idx == nullptr) {
        return lv.base;
    }
    if (lv.array_ty != nullptr) {
        return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), lv.elem_idx });
    }
    return b_.CreateInBoundsGEP(slot_of(lv.sig).storage_ty, lv.base,
                                { b_.getInt32(0), lv.elem_idx });
}

// THE resolver for "a pointer to this place's whole storage image", whichever
// storage model it is in: the recursive aggregate mirror, or flat `[N x slot]`.
//
// A fixed unpacked array has two storage models -- flat with per-signal
// z-elision, and the canonical mirror when anything struct- or container-
// bearing is involved -- so a whole-object copy can legitimately have one model
// on each side (`v.m = tmp`, a struct member assigned a plain array). Every
// caller that resolved only one of them crashed on the other: agg_slot_ptr
// dereferences a null root_ty for a flat lvalue.
llvm::Value* Backend::agg_image_ptr(const LValue& lv) {
    return lv.is_agg ? agg_slot_ptr(lv) : flat_elem_ptr(lv);
}

// The per-element slot type of a whole-array place, per its storage model. Two
// places of the same array TYPE agree byte for byte exactly when these agree,
// which is what decides memcpy vs an element-wise copy.
//
// Always answered from the place whose bytes it describes, never from the type
// alone: the two models disagree precisely when one side's may-z analysis elided
// a z plane the other keeps, and that is invisible in the type.
llvm::StructType* Backend::image_elem_slot(const LValue& lv, Ref type) {
    if (lv.is_agg) {
        return agg_leaf_slot_ty(leaf_type(type));
    }
    // A flat aggregate place is always a module signal, for the reasons
    // emit_agg_ret_copy enumerates (every non-signal aggregate lvalue is rooted
    // in the canonical mirror). Loud rather than an assert, because the Release
    // fallback is slot_of(Signal{ 0 }) -- some unrelated signal's element shape,
    // i.e. exactly the silent stride corruption this function decides.
    if (!lv.is_signal) {
        throw BackendError{ "internal: flat aggregate place is not a signal" };
    }
    return slot_of(lv.sig).slot_ty;
}

// A read-only image of what an out-of-range whole-object read must yield
// (LRM 7.4.5 / Table 7-1), laid out exactly like the source it stands in
// for. An aggregate source shares the type-derived recursive storage, so
// agg_proto_global serves; a flat array's per-element slot is z-elided per
// SIGNAL, so its image has to be built against that signal's slot type.
llvm::Constant* Backend::agg_copy_proto(Ref type, const LValue& slv) {
    if (slv.is_agg) {
        return agg_proto_global(type);
    }
    const SignalSlot& ss   = slot_of(slv.sig);
    auto*             arr  = llvm::ArrayType::get(ss.slot_ty, array_leaf_count(type));
    llvm::Constant*   leaf = slot_default_const(ss.slot_ty, ss.width, ss.narrow, ss.four_state, ss.has_z);
    auto*             init = llvm::ConstantArray::get(
      arr, std::vector<llvm::Constant*>(arr->getNumElements(), leaf));
    return new llvm::GlobalVariable(env_->llmod, arr, /*isConstant=*/true,
                                    llvm::GlobalValue::InternalLinkage, init,
                                    "clotho.arr.proto");
}

// The whole-object copy itself, once both ends are known good: one memcpy of
// the storage, or an element-wise pass when z-elision left the two arrays
// different per-element layouts. Split out of the AggCopy case so an
// out-of-range destination can skip all of it (LRM 7.4.5: the write performs
// no operation) without the gate having to thread through every exit.
void Backend::emit_agg_copy(const AggCopyInfo& info, const LValue& dlv, const LValue& slv, llvm::Value* src_override) {
    // Both sides resolve through the ONE image resolver, so either may be in
    // either storage model. That matters because a whole-object copy can
    // legitimately mix them: `v.m = tmp` assigns a plain array into a struct
    // member, and the member is in the canonical mirror while `tmp` is flat.
    llvm::Value* const dst = agg_image_ptr(dlv);
    llvm::Value* const src =
      src_override != nullptr ? src_override : agg_image_ptr(slv);
    llvm::Type* st = nullptr;
    // Which storage model a place is in is decided by its TYPE for everything
    // struct- or container-bearing (needs_agg_storage), so both sides of such a
    // copy are necessarily in the canonical mirror and its own type is the
    // memcpy shape. Only a PLAIN scalar-leaf array can differ between the two
    // sides, and only in its per-element slot, which is may-z-analysed per
    // signal in the flat model and type-derived in the mirror.
    if (needs_agg_storage(info.type) || !is_agg_abi(info.type)) {
        st = agg_storage_ty(info.type);
    } else {
        // Each side's element slot comes from ITS OWN place. A src_override
        // redirects the source POINTER only -- to an associative entry's slot, or
        // to the Table 7-1 image an out-of-range index must read (LRM 7.4.5) --
        // and every override is built in the SOURCE's shape: agg_image_ptr of
        // `slv`, and agg_copy_proto, which lays its image out against
        // slot_of(slv.sig) for precisely that reason. Taking d_slot for the
        // source instead sized the memcpy for the destination AND strode the
        // source with the destination's element size, so every element past the
        // first read the wrong bytes, silently, whenever the two elisions
        // disagreed. It was reachable for any offset source, because an
        // ElemSelect on a fixed unpacked array always gates on dyn_inrange (even
        // for a constant, statically in-range index), hence always takes an
        // override: a sub-array actual on a 4-state NET port copied 8 bytes out
        // of the 4-byte z-elided variable behind it.
        llvm::StructType* const d_slot = image_elem_slot(dlv, info.type);
        llvm::StructType* const s_slot = image_elem_slot(slv, info.type);
        if (d_slot != s_slot) {
            // The two element layouts differ only in the z word's presence (the
            // mm plane is an identical i(2W)), so copy element-wise, filling the
            // dest z with 0 when the source is never-z. The leaf facts come from
            // the TYPE, so this works whichever model each side is in.
            const Layout lay = mod_.pool.layout(leaf_type(info.type));
            emit_array_zdiff_copy(dst, src, d_slot, s_slot, lay.width == 0 ? 1 : lay.width,
                                  lay.domain == Domain::Four, array_leaf_count(info.type),
                                  dlv.sig,
                                  !silent_stores_ && dlv.is_signal &&
                                    signal_has_waiters(dlv.sig));
            return;
        }
        // Equal slots make the two images byte-identical -- both are row-major
        // runs of the same leaf -- so the copy stays ONE memcpy. A design that
        // never mixes a z-carrying place with a z-elided one therefore emits
        // exactly the code it did before.
        //
        // The count comes from the copy TYPE, not the whole signal: a multi-dim
        // element select (`arr[i][j]`) addresses a SUB-array, whose flat leaf
        // offset the resolver above has already applied.
        st = llvm::ArrayType::get(d_slot, array_leaf_count(info.type));
    }
    const auto sz = env_->llmod.getDataLayout().getTypeAllocSize(st);
    const auto al = env_->llmod.getDataLayout().getABITypeAlign(st);
    // A blocking whole-object copy into a signal must wake its Any-edge
    // readers (a continuous / port copy or a comb process feeds off the
    // destination), but only when the bytes actually change — an
    // unconditional wake would never settle a feedback delta wave. Detect
    // the change with a memcmp of old dest vs new source *before* the copy.
    const bool service = !silent_stores_ && dlv.is_signal &&
                         signal_has_waiters(dlv.sig);
    llvm::Value* changed = nullptr;
    if (service) {
        auto cmp = rt_fn("memcmp", b_.getInt32Ty(),
                         { raw_ptr_ty(), raw_ptr_ty(), b_.getInt64Ty() });
        changed   = b_.CreateICmpNE(
          b_.CreateCall(cmp, { dst, src, b_.getInt64(sz) }), b_.getInt32(0));
    }
    b_.CreateMemCpy(dst, al, src, al, sz);
    if (service) {
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* svc  = llvm::BasicBlock::Create(ctx_, "aggcopy.svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "aggcopy.cont", fn);
        b_.CreateCondBr(changed, svc, cont);
        b_.SetInsertPoint(svc);
        service_any_waiters(dlv.sig);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// The copy-out of a whole aggregate FUNCTION RESULT (LRM 13.4.1's by-value
// return): the callee writes the returned object into the trailing out-param
// the caller owns, which lower_call_common always allocates in the CANONICAL
// storage mirror (agg_storage_ty).
//
// The returned object need not be in that model, and usually is not. A
// function local declared without `automatic` has STATIC lifetime (LRM
// 13.4.2), so `return a;` returns a module SIGNAL -- and a plain scalar-leaf
// array signal keeps FLAT `[N x slot]` storage whose element slot is may-z
// analysed per signal, which is NARROWER than the type-derived canonical leaf
// whenever the analysis wins. `logic [7:0] a[0:1]` written only with 2-state
// constants gets `[2 x { i16 }]`, four bytes, against the canonical
// `[2 x { i16, i8 }]`'s eight. A memcpy of agg_storage_ty bytes would read
// four bytes past the end of the signal, so the element SHAPES decide the copy
// here exactly as they do for a place-to-place AggCopy above.
void Backend::emit_agg_ret_copy(llvm::Value* dst, const LValue& slv, Ref type) {
    // Resolves either storage model, and keeps agg_slot_ptr's loud refusal of
    // a deferred associative place (its base is null; nothing to copy from).
    llvm::Value* const src = agg_image_ptr(slv);
    if (slv.is_agg) {
        // Both ends are the same type-derived mirror, so the whole-storage
        // copy -- deep, when a leaf owns heap -- is exact.
        emit_agg_storage_copy(dst, src, type);
        return;
    }
    // A flat aggregate place is always a module signal: build_frame_layout
    // gives every is_agg_abi local the canonical mirror, and every other
    // aggregate lvalue is rooted in one (a call temp, a class property, a
    // container element). Loud rather than an assert, because the Release
    // fallback would be slot_of(Signal{ 0 }) -- some unrelated signal's element
    // shape, i.e. the silent corruption this function exists to prevent.
    if (!slv.is_signal) {
        throw BackendError{ "internal: flat aggregate return value is not a signal" };
    }
    assert(!needs_agg_storage(type) &&
           "a struct- or container-bearing type is in the mirror on both sides");
    const SignalSlot& ss = slot_of(slv.sig);
    if (is_string_type(type)) {
        // A string-leaf array is flat `[N x rt_string]` and its mirror is that
        // same leaf slot nested once per dimension: identical bytes in the same
        // row-major order. So the per-leaf deep copy a string leaf needs (it
        // GEPs the mirror from BOTH pointers) addresses the flat source
        // correctly, and no shape conversion arises -- a string slot carries no
        // planes for the may-z analysis to narrow.
        assert(ss.slot_ty == str_slot_ty() && "a string leaf has one slot shape");
        emit_agg_storage_copy(dst, src, type);
        return;
    }
    // Nothing past the string case owns heap: a container leaf would have made
    // needs_agg_storage true, and a struct leaf with a managed member likewise.
    assert(!has_heap_leaf(type) && "a plane-leaf array's byte image is its value");
    llvm::StructType* const canon = agg_leaf_slot_ty(leaf_type(type));
    const uint32_t          count = array_leaf_count(type);
    if (ss.slot_ty == canon) {
        // Equal element slots make the two images byte-identical, since a
        // nested mirror and a flat run of the same leaf are both row-major with
        // no padding between elements. Asserted rather than trusted: it is the
        // whole licence for the memcpy.
        assert(env_->llmod.getDataLayout().getTypeAllocSize(agg_storage_ty(type)) ==
                 env_->llmod.getDataLayout().getTypeAllocSize(
                   llvm::ArrayType::get(canon, count)) &&
               "the nested mirror and the flat run are the same bytes");
        emit_agg_storage_copy(dst, src, type);
        return;
    }
    // The shapes differ, so convert element by element. Both images are
    // row-major over their own leaf slot, which is what lets the multi-dim
    // destination mirror be addressed as one flat run of `count` leaves. No
    // wait service: the destination is the caller's private buffer, not state.
    const Layout lay = mod_.pool.layout(leaf_type(type));
    emit_array_zdiff_copy(dst, src, canon, ss.slot_ty, lay.width == 0 ? 1 : lay.width,
                          lay.domain == Domain::Four, count, Signal{ 0 },
                          /*service=*/false);
}

// Element-wise whole-(sub-)array copy for two plain (non-struct) 4-state
// unpacked arrays of the SAME element width whose z-elided slots differ only in
// the z word's presence (the dest is may-z, the source proven never-z, or vice
// versa). A memcpy is invalid (element strides differ); the mm plane is an
// identical i(2W) in both, so loop over `count` elements copying each mm word
// and, into a z-capable dest, its z word (0 when the source is never-z).
// `d_base` / `s_base` point at the first copied element (already offset for a
// sub-array). `service` wakes the dest's Any-edge readers on any change.
// Element-wise whole-(sub-)array copy between two storage images whose element
// slots differ in SHAPE, not just in the z word's presence: one side narrow
// (a single iW value word), the other the packed must|may pair, or a 2-state
// pair of differing widths. A memcpy and even a word move are both wrong here,
// so each element goes through the ordinary lower_load / lower_store pair, which
// is where narrow <-> packed conversion already lives and is already exercised
// by every scalar assignment between differently-shaped slots.
//
// Reached when the two models meet: a class property or struct member is always
// in the canonical, type-derived mirror while a module-scope array signal's
// element slot is may-z-analysed and may be independently widened.
void Backend::emit_array_convert_copy(llvm::Value*      d_base,
                                      llvm::Value*      s_base,
                                      llvm::StructType* d_slot,
                                      llvm::StructType* s_slot,
                                      bool              d_narrow,
                                      bool              s_narrow,
                                      uint32_t          leaf_width,
                                      uint32_t          count,
                                      Signal            dsig,
                                      bool              service) {
    auto* dst_arr = llvm::ArrayType::get(d_slot, count);
    auto* src_arr = llvm::ArrayType::get(s_slot, count);

    const auto elem_lv = [&](llvm::Value* base, llvm::ArrayType* arr,
                             llvm::StructType* slot, bool narrow,
                             llvm::Value* idx) {
        LValue lv;
        lv.base        = base;
        lv.slot_ty     = slot;
        lv.narrow      = narrow;
        lv.has_z       = !narrow && slot->getNumElements() > 1;
        lv.array_ty    = arr;
        lv.elem_idx    = idx;
        lv.store_width = leaf_width;
        lv.bit_off     = b_.getInt32(0);
        lv.width       = leaf_width;
        return lv;
    };

    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "cvcopy.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "cvcopy.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "cvcopy.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(b_.getInt32Ty(), 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(count)), body, done);

    b_.SetInsertPoint(body);
    lower_store(elem_lv(d_base, dst_arr, d_slot, d_narrow, idx),
                lower_load(elem_lv(s_base, src_arr, s_slot, s_narrow, idx)));
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
    // A converting copy cannot cheaply prove nothing changed (the two images are
    // not comparable word for word), so it wakes unconditionally. Correct, and
    // only reachable for the model-crossing shapes above.
    if (service) {
        service_any_waiters(dsig);
    }
}

void Backend::emit_array_zdiff_copy(llvm::Value*      d_base,
                                    llvm::Value*      s_base,
                                    llvm::StructType* d_slot,
                                    llvm::StructType* s_slot,
                                    uint32_t          leaf_width,
                                    bool              four_state,
                                    uint32_t          count,
                                    Signal            dsig,
                                    bool              service) {
    // A NARROW slot's single word is the value itself, not a must|may pair, so
    // when the two sides disagree about narrowness the mm words are different
    // widths and no word move is sound. Route those through the ordinary
    // load/store pair, which is exactly where narrow <-> packed conversion
    // already lives; the word-move loop below then only ever sees two packed
    // slots differing in the z word's presence.
    const bool d_narrow = d_slot->getNumElements() == 1 &&
                          d_slot->getElementType(0) == int_ty(leaf_width);
    const bool s_narrow = s_slot->getNumElements() == 1 &&
                          s_slot->getElementType(0) == int_ty(leaf_width);
    if (d_narrow != s_narrow || !four_state) {
        emit_array_convert_copy(d_base, s_base, d_slot, s_slot, d_narrow, s_narrow,
                                leaf_width, count, dsig, service);
        return;
    }
    auto* dst_arr = llvm::ArrayType::get(d_slot, count);
    auto* src_arr = llvm::ArrayType::get(s_slot, count);
    auto* mm_ty   = int_ty(2 * leaf_width);
    auto* z_ty    = int_ty(leaf_width);
    auto* i32     = b_.getInt32Ty();

    llvm::AllocaInst* changed = nullptr;
    if (service) {
        changed = entry_alloca(b_.getInt1Ty(), "acopy.changed");
        b_.CreateStore(b_.getFalse(), changed);
    }
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "acopy.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "acopy.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "acopy.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(count)), body, done);

    b_.SetInsertPoint(body);
    auto* d_mm_ptr = b_.CreateInBoundsGEP(dst_arr, d_base, { b_.getInt32(0), idx, b_.getInt32(0) });
    auto* s_mm     = b_.CreateLoad(
      mm_ty, b_.CreateInBoundsGEP(src_arr, s_base, { b_.getInt32(0), idx, b_.getInt32(0) }));
    llvm::Value* s_z =
      s_slot->getNumElements() > 1 ? static_cast<llvm::Value*>(b_.CreateLoad(
                   z_ty, b_.CreateInBoundsGEP(src_arr, s_base, { b_.getInt32(0), idx, b_.getInt32(1) })))
               : zero(leaf_width);
    llvm::Value* d_z_ptr =
      d_slot->getNumElements() > 1 ? b_.CreateInBoundsGEP(dst_arr, d_base, { b_.getInt32(0), idx, b_.getInt32(1) }) : nullptr;
    if (service) {
        llvm::Value* diff = b_.CreateICmpNE(b_.CreateLoad(mm_ty, d_mm_ptr), s_mm);
        if (d_z_ptr != nullptr) {
            diff = b_.CreateOr(diff, b_.CreateICmpNE(b_.CreateLoad(z_ty, d_z_ptr), s_z));
        }
        b_.CreateStore(b_.CreateOr(b_.CreateLoad(b_.getInt1Ty(), changed), diff), changed);
    }
    b_.CreateStore(s_mm, d_mm_ptr);
    if (d_z_ptr != nullptr) {
        b_.CreateStore(s_z, d_z_ptr);
    }
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), body);
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
    if (service) {
        auto* svc  = llvm::BasicBlock::Create(ctx_, "acopy.svc", fn);
        auto* cont = llvm::BasicBlock::Create(ctx_, "acopy.cont", fn);
        b_.CreateCondBr(b_.CreateLoad(b_.getInt1Ty(), changed), svc, cont);
        b_.SetInsertPoint(svc);
        service_any_waiters(dsig);
        b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

// Pointer to a state field at a runtime byte offset (the NBA commit loop
// reads its offsets from the meta table, erasing the field types). Offsets
// are DataLayout field offsets, so accesses stay naturally aligned even
// though the GEP is dynamic.
llvm::Value* Backend::field_ptr_var(llvm::Value* base, llvm::Value* off) {
    return b_.CreateInBoundsGEP(b_.getInt8Ty(), base, off);
}

} // namespace clotho::ir::backend
