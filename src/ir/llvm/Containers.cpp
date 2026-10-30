// Dynamic arrays and queues: element descriptors, handle slots and container operations.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// One element's storage classification, from the ELEMENT type. Keyed on the
// element rather than the container because a nested container's leaf descriptor
// needs the same answer with no container Ref to hand (collect_elem_leaves).
Backend::DynElem Backend::elem_desc(Ref elem_type) {
    DynElem     e;
    e.elem_type    = elem_type;
    const Type& et = std::get<Type>(mod_.pool.get(elem_type));
    if (et.kind == Type::Kind::String) {
        e.is_string = true;
        e.slot_ty   = str_slot_ty();
        return e;
    }
    if (et.kind == Type::Kind::DynArray || et.kind == Type::Kind::Queue ||
        et.kind == Type::Kind::Assoc) {
        e.is_handle = true;
        e.slot_ty   = handle_slot_ty(elem_type);
        return e;
    }
    if (!is_leaf_type(elem_type)) {
        // Unpacked struct / fixed unpacked array value: the slot is the flat
        // recursive aggregate image, addressed via the is_agg lvalue path.
        e.is_agg = true;
        e.agg_ty = agg_storage_ty(elem_type);
        return e;
    }
    const Layout lay = mod_.pool.layout(elem_type);
    e.width      = lay.width == 0 ? 1 : lay.width;
    e.four_state = lay.domain == Domain::Four;
    e.has_z      = e.four_state;
    e.narrow     = !e.four_state; // 2-state / real element -> single iW slot
    e.slot_ty    = slot_type(e.width, e.four_state, e.has_z, e.narrow);
    return e;
}

Backend::DynElem Backend::dynarr_elem(Ref array_type) {
    return elem_desc(std::get<Type>(mod_.pool.get(array_type)).element);
}

// Byte size of one element slot (the runtime stride).
uint64_t Backend::dynarr_stride(const DynElem& e) {
    return env_->llmod.getDataLayout().getTypeAllocSize(e.is_agg ? e.agg_ty : e.slot_ty);
}

Backend::ContainerFmt Backend::container_fmt(Ref container_type) {
    const DynElem e = dynarr_elem(container_type);
    ContainerFmt  cf;
    cf.is_queue =
      std::get<Type>(mod_.pool.get(container_type)).kind == Type::Kind::Queue ? 1 : 0;
    cf.stride = dynarr_stride(e);
    if (e.is_string) {
        cf.kind = 2;
    } else if (mod_.pool.is_float(e.elem_type)) {
        cf.kind = 1;
    } else {
        cf.width     = e.width;
        cf.four      = e.four_state ? 1u : 0u;
        cf.has_z     = e.has_z ? 1u : 0u;
        cf.is_signed = std::get<Type>(mod_.pool.get(e.elem_type)).is_signed ? 1u : 0u;
        if (e.has_z) {
            cf.z_off = static_cast<uint32_t>(
              env_->llmod.getDataLayout().getStructLayout(e.slot_ty)->getElementOffset(1));
        }
    }
    return cf;
}

// The element power-on byte image as a typed constant: X planes for a
// 4-state scalar, zero for 2-state / real, the empty inline handle for a
// string, the null handle for a nested container.
// Power-on constant for an aggregate storage image: X planes for a 4-state
// scalar leaf, 0 for 2-state / real, the empty inline handle for a string leaf
// and the null handle for a nested container leaf, recursively over the unpacked
// struct / array. Built against the exact `storage` LLVM type so the constant's
// type matches the target global / GEP source -- agg_storage_ty mints a fresh
// (structurally-equal but distinct) StructType per call, so the constant must
// reuse the caller's instance, not a new one.
llvm::Constant* Backend::agg_proto_const(Ref type, llvm::Type* storage) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        auto*                        arr = llvm::cast<llvm::ArrayType>(storage);
        llvm::Constant*              e   = agg_proto_const(t.element, arr->getElementType());
        std::vector<llvm::Constant*> elems(array_dim(t), e);
        return llvm::ConstantArray::get(arr, elems);
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        auto*                        st = llvm::cast<llvm::StructType>(storage);
        std::vector<llvm::Constant*> fields;
        const auto                   flds = mod_.pool.struct_fields(t.fields_id);
        for (uint32_t k = 0; k < flds.size(); ++k) {
            fields.push_back(agg_proto_const(flds[k].type, st->getElementType(k)));
        }
        return llvm::ConstantStruct::get(st, fields);
    }
    if (t.kind == Type::Kind::String || t.kind == Type::Kind::DynArray || t.kind == Type::Kind::Queue ||
        t.kind == Type::Kind::Assoc) {
        // A managed leaf powers on empty / null, which is also what makes the
        // image safe to rt_elem_init a fresh slot with: the runtime then
        // deep-assigns into a valid empty leaf.
        return elem_proto_const(elem_desc(type));
    }
    const Layout   lay  = mod_.pool.layout(type);
    const uint32_t w    = lay.width == 0 ? 1 : lay.width;
    const bool     four = lay.domain == Domain::Four;
    std::vector<llvm::Constant*> fields{ four ? x_mm(w) : zero(w) };
    if (four) {
        fields.push_back(zero(w));
    }
    return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(storage), fields);
}

// A read-only global holding a POD aggregate type's power-on image, for the
// absent-key default of a whole-aggregate associative-array read (`s = a[k]`
// with `k` absent yields the element default; LRM 7.8.6 + Table 7-1).
llvm::GlobalVariable* Backend::agg_proto_global(Ref type) {
    llvm::Type* st = agg_storage_ty(type);
    return new llvm::GlobalVariable(env_->llmod, st, /*isConstant=*/true,
                                    llvm::GlobalValue::InternalLinkage,
                                    agg_proto_const(type, st), "clotho.agg.proto");
}

llvm::Constant* Backend::elem_proto_const(const DynElem& e) {
    if (e.is_agg) {
        return agg_proto_const(e.elem_type, e.agg_ty);
    }
    if (e.is_string) {
        return str_empty_const();
    }
    if (e.is_handle) {
        return llvm::Constant::getNullValue(e.slot_ty);
    }
    llvm::Constant*              mm = e.narrow ? zero(e.width) : x_mm(e.width);
    std::vector<llvm::Constant*> fields{ mm };
    if (e.has_z) {
        fields.push_back(zero(e.width));
    }
    return llvm::ConstantStruct::get(e.slot_ty, fields);
}

// The heap-owning leaves of an aggregate element type as { byte offset, ops }
// pairs, appended in ASCENDING offset order.
//
// The order is a runtime contract, not a convenience: rt_elem_copy deep-assigns
// each leaf and then raw-copies the POD gaps *between* consecutive leaves
// (runtime/Containers.cpp), so an unsorted table would copy a leaf's bytes as if they were
// POD -- shallow-copying a string handle into a slot that then double-frees.
// The walk is in declaration / index order, which IS ascending offset for both
// an LLVM struct (fields laid out in order) and an array.
void Backend::collect_elem_leaves(Ref type, uint64_t base, llvm::Type* storage, std::vector<llvm::Constant*>& out) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::UnpackedArray) {
        auto*          arr    = llvm::cast<llvm::ArrayType>(storage);
        const uint64_t stride = env_->llmod.getDataLayout().getTypeAllocSize(arr->getElementType());
        for (uint64_t k = 0; k < arr->getNumElements(); ++k) {
            collect_elem_leaves(t.element, base + k * stride, arr->getElementType(), out);
        }
        return;
    }
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        auto*       st = llvm::cast<llvm::StructType>(storage);
        const auto* sl = env_->llmod.getDataLayout().getStructLayout(st);
        uint32_t    f  = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            collect_elem_leaves(fld.type, base + sl->getElementOffset(f), st->getElementType(f), out);
            ++f;
        }
        return;
    }
    if (t.kind != Type::Kind::String && t.kind != Type::Kind::DynArray && t.kind != Type::Kind::Queue &&
        t.kind != Type::Kind::Assoc && t.kind != Type::Kind::Event) {
        return;
    }
    out.push_back(llvm::ConstantStruct::get(elem_leaf_ty(), { b_.getInt64(base), elem_ops_for(type) }));
}

// rt_elem_ops::handles for one element type: the byte offsets of the class-handle
// words inside ONE element image, ascending. Empty for a container element --
// whose handles are reached through `inner`.
//
// collect_class_handles is the same walk clotho.gcroots and the class trace map
// use, and it answers this with no second walker: its Class arm reports `base`, so
// a bare class-handle element yields exactly { 0 } and an aggregate yields every
// handle reachable without leaving its image. It does not descend a container
// handle (:2110-2113), which is exactly the "a nested container element lists
// none" half of the contract.
//
// Measured against the SAME image type that feeds `stride` and `proto`, so an
// offset here indexes the bytes the runtime actually memcpys.
std::vector<uint32_t> Backend::elem_handle_offsets(Ref elem_type, const DynElem& e) {
    std::vector<uint32_t> out;
    collect_class_handles(elem_type, 0, e.is_agg ? e.agg_ty : e.slot_ty, out);
    assert((out.empty() || !e.is_handle) &&
           "a container element lists no inline handle; it is reached through inner");
    std::ranges::sort(out);
    assert(std::ranges::adjacent_find(out) == out.end() && "a handle word was listed twice");
    return out;
}

// The constant rt_elem_ops descriptor for one element type, memoized.
//
// Nested containers chain through `inner` (finite: type nesting is a DAG). An
// aggregate element is kind 0 (POD) when every leaf is plain bytes -- memcpy
// copy over the proto image, no per-element lifecycle -- and kind 4 when it owns
// a string / container leaf, which the leaf table then makes the runtime
// deep-copy and free.
//
// This descriptor is also how a class handle inside a container is TRACED
// (docs/design/garbage-collection.md §3.3), through the four appended fields:
//
//   bare class handle  -> nhandles 1, handles { 0 }
//   aggregate          -> every handle reachable without leaving the element
//                         image, by byte offset within it
//   nested container   -> nhandles 0; its handles are reached through `inner`
//   string / event / other POD -> nhandles 0
//
// and, for an associative array, its key-handle offsets in `keyhandles`.
llvm::Constant* Backend::elem_ops_for(Ref elem_type) {
    if (const auto it = env_->elem_ops.find(static_cast<uint32_t>(elem_type)); it != env_->elem_ops.end()) {
        return it->second;
    }
    const DynElem   e       = elem_desc(elem_type);
    uint32_t        kind    = 0;
    llvm::Constant* inner = llvm::Constant::getNullValue(raw_ptr_ty());
    llvm::Constant* leaves  = llvm::Constant::getNullValue(raw_ptr_ty());
    uint32_t        nleaves = 0;
    std::vector<uint32_t> key_handle_offs;
    if (std::get<Type>(mod_.pool.get(elem_type)).kind == Type::Kind::Event) {
        // An event element is a 64-bit handle word like a class handle, but
        // unlike one it needs per-slot CONSTRUCTION: LRM Table 6-7 binds every
        // new event storage location to a new object, which a shared prototype
        // image cannot express. Hence its own kind rather than POD.
        kind = 6;
    } else if (e.is_string) {
        kind = 1;
    } else if (e.is_handle) {
        // A nested container element: the slot holds ITS handle, and the
        // descriptor chains through `inner`, which describes the inner
        // container's own element (its value element, for an assoc). Each
        // container kind needs its own descriptor kind because the runtime
        // reads a different handle struct out of the slot.
        //
        // gc_container_kind is the ONE place that maps a container type to its
        // rt_elem kind, so an rt_gc_container entry and the descriptor it points
        // at cannot disagree -- an equality the collector asserts.
        const Type& et = std::get<Type>(mod_.pool.get(elem_type));
        kind           = gc_container_kind(elem_type);
        if (et.kind == Type::Kind::Assoc) {
            key_handle_offs = assoc_key_handle_offsets(elem_type);
        }
        inner = elem_ops_for(et.element);
    } else if (e.is_agg && (has_heap_leaf(elem_type) || types::has_ctor_leaf(mod_.pool, elem_type))) {
        std::vector<llvm::Constant*> table;
        collect_elem_leaves(elem_type, 0, e.agg_ty, table);
        auto* arr_ty = llvm::ArrayType::get(elem_leaf_ty(), table.size());
        leaves       = new llvm::GlobalVariable(env_->llmod,
                                                arr_ty,
                                                /*isConstant=*/true,
                                                llvm::GlobalValue::InternalLinkage,
                                                llvm::ConstantArray::get(arr_ty, table),
                                                "clotho.elem.leaves");
        nleaves      = static_cast<uint32_t>(table.size());
        kind         = 4;
    }
    const std::vector<uint32_t> handle_offs = elem_handle_offsets(elem_type, e);
    llvm::Type* proto_ty = e.is_agg ? e.agg_ty : e.slot_ty;
    auto* proto = new llvm::GlobalVariable(env_->llmod, proto_ty, /*isConstant=*/true,
                                           llvm::GlobalValue::InternalLinkage,
                                           elem_proto_const(e), "clotho.elem.proto");
    auto*       init     = llvm::ConstantStruct::get(
      elem_ops_ty(),
      { b_.getInt32(kind), b_.getInt32(nleaves), b_.getInt64(dynarr_stride(e)), proto, inner,
        leaves, b_.getInt32(static_cast<uint32_t>(handle_offs.size())),
        b_.getInt32(static_cast<uint32_t>(key_handle_offs.size())),
        gc_offsets_global(handle_offs, "clotho.elem.handles"),
        gc_offsets_global(key_handle_offs, "clotho.elem.keyhandles") });
    auto* gv = new llvm::GlobalVariable(env_->llmod, elem_ops_ty(), /*isConstant=*/true,
                                        llvm::GlobalValue::InternalLinkage, init,
                                        "clotho.elem.ops");
    env_->elem_ops.emplace(static_cast<uint32_t>(elem_type), gv);
    return gv;
}

llvm::Constant* Backend::elem_ops_global(Ref container_type) {
    return elem_ops_for(std::get<Type>(mod_.pool.get(container_type)).element);
}

Backend::SortDesc Backend::dynarr_sort_desc(Ref array_type) const {
    const Ref     elem = std::get<Type>(mod_.pool.get(array_type)).element;
    const ValueType vt   = value_type_of(mod_.pool, elem);
    if (vt.is_real) {
        return { 2u, vt.width };
    }
    if (vt.four_state) {
        return { vt.is_signed ? 4u : 3u, vt.width };
    }
    return { vt.is_signed ? 1u : 0u, vt.width };
}

// Whether a type is a queue (LRM 7.10).
bool Backend::is_queue_type(Ref type) const {
    return types::is_queue(mod_.pool, type);
}

// A heap-backed handle value type (dynamic array, queue, or associative
// array): all share the by-pointer, deep-copy subroutine ABI, differing only
// in the runtime slot struct and the copy / free functions.
bool Backend::is_handle_type(Ref type) const {
    return types::is_container(mod_.pool, type);
}

llvm::StructType* Backend::handle_slot_ty(Ref type) {
    if (is_assoc_type(type)) {
        return assoc_slot_ty();
    }
    return is_queue_type(type) ? queue_slot_ty() : dynarr_slot_ty();
}

// Deep-copy one handle into another (value semantics), picking the runtime
// function and element descriptor from the type.
//
// Emitted for subroutine copy-in / copy-out and return copy-out. Those are ABI
// plumbing at the call boundary rather than a lowered instruction, so no Index
// is in scope and the allocation reports its location as "<unknown>".
void Backend::emit_handle_assign(llvm::Value* dst, llvm::Value* src, Ref type) {
    const SourceText at{ .where = "<unknown>" };
    if (is_assoc_type(type)) {
        b_.CreateCall(
          assoc_assign_fn(),
          { dst, src, elem_ops_global(type), alloc_site_global(at, "associative array") });
        return;
    }
    const bool q = is_queue_type(type);
    b_.CreateCall(q ? queue_assign_fn() : dynarr_assign_fn(),
                  { dst, src, elem_ops_global(type),
                    alloc_site_global(at, q ? "queue" : "dynamic array") });
}

// Destroy a handle's elements and free its storage (reset to empty).
void Backend::emit_handle_delete(llvm::Value* handle, Ref type) {
    if (is_assoc_type(type)) {
        // Teardown / whole-value replacement drops the pattern default too;
        // a.delete() keeps it and takes rt_assoc_delete_all instead.
        b_.CreateCall(assoc_reset_fn(), { handle, elem_ops_global(type) });
        return;
    }
    b_.CreateCall(is_queue_type(type) ? queue_delete_all_fn() : dynarr_delete_fn(),
                  { handle, elem_ops_global(type) });
}

// The shared out-of-range sink for a nested-container element: a writable
// handle global per element type, emptied before every use.
//
// Mirrors the string-element sink (lv_string_ptr): with it, an out-of-range
// `q[i]` where the element is itself a queue / dynamic array reads as an EMPTY
// container (the Table 7-1 default, so `q[99].size()` is 0) and an out-of-range
// mutating method lands in the sink and is discarded (LRM 7.4.5), instead of
// addressing the heap buffer past its length.
llvm::Value* Backend::handle_sink_global(Ref elem_type) {
    const auto key = static_cast<uint32_t>(elem_type);
    if (const auto it = env_->handle_sinks.find(key); it != env_->handle_sinks.end()) {
        return it->second;
    }
    auto* slot = handle_slot_ty(elem_type);
    auto* gv   = new llvm::GlobalVariable(env_->llmod,
                                          slot,
                                          /*isConstant=*/false,
                                          llvm::GlobalValue::InternalLinkage,
                                          llvm::ConstantAggregateZero::get(slot),
                                          "clotho.handle.sink");
    env_->handle_sinks.emplace(key, gv);
    return gv;
}

// Pointer to the rt handle slot an lvalue designates: the resolved
// aggregate leaf slot for a struct-member / class-property / array-element
// handle, or the bare base for a whole signal / frame local.
//
// A handle reached THROUGH a dynamic element index (`q[i]` where the element is
// itself a container, or `q[i].inner` where the element is a struct with a
// container member) redirects out of range to the sink, so the heap buffer is
// never addressed past its live length.
//
// `a[k]` where the assoc VALUE is a container is a deferred place, so it is
// resolved here, and the direction decides which way: a mutating method
// (`a[k].push_back(x)`) must create the entry, because LRM 7.10's methods
// operate on a queue variable and there is no variable until the entry exists;
// a query (`a[k].size()`) must not (LRM 7.8.6 -- reading a nonexistent element
// creates nothing), and reads the empty-container default out of the sink.
// `use` says WHY the pointer is being taken, and it has no default on purpose.
// A waited-on container class property fires its change notifier at exactly the
// `Mutate` uses (LRM 9.4.3, notify_container_mutation), so this signature is
// what keeps the mutation surface closed: a call site added later cannot omit
// the answer, and a mutation miscategorised as a read is a permanent park --
// the bug class the notifier mechanism exists to remove.
llvm::Value* Backend::lv_handle_ptr(const LValue& lv, HandleUse use) {
    if (lv.is_assoc) {
        // The deferred assoc entry resolves first; a for-write resolution of a
        // waited-on map fires there, so this recursion must not fire again for
        // the element it produced.
        const LValue el = assoc_elem_lvalue(lv, use != HandleUse::Read);
        return lv_handle_ptr(el, use == HandleUse::Mutate ? HandleUse::Place : use);
    }
    if (use == HandleUse::Mutate) {
        notify_container_mutation(lv);
    }
    llvm::Value* ptr = lv.is_agg ? agg_slot_ptr(lv) : lv.base;
    if (lv.dyn_inrange == nullptr || !lv.is_handle) {
        return ptr;
    }
    if (lv.handle_type == Ref::Invalid) {
        throw BackendError{ "internal: bounds-checked handle lvalue with no element type" };
    }
    llvm::Value* sink = handle_sink_global(lv.handle_type);
    emit_handle_delete(sink, lv.handle_type);
    return b_.CreateSelect(lv.dyn_inrange, ptr, sink);
}

// Fire a waited-on CONTAINER class property's change notifier (LRM 9.4.3), or
// emit nothing at all when the property carries none -- which is every property
// in a design that waits on no container.
//
// UNCONDITIONAL, unlike the integral service_change_notify: the property's value
// is the container's CONTENTS, and `q.size()` is computed rather than stored, so
// there is no old -> new word for the write to classify. `wait` is
// level-sensitive -- it re-tests its condition and parks again -- so a fire that
// leaves the condition false costs one re-test, while a MISSED mutation is a
// permanent park. `@` over a container expression is refused in the frontend
// precisely because it cannot absorb the same imprecision (LRM 9.4.2 wants a
// change in VALUE, and a push followed by a pop is two mutations and no change).
//
// Emitted BEFORE the mutation the caller is about to perform, which is sound:
// emit_event_wake_list only pushes woken activations onto the ready list, and a
// woken process cannot run before this one suspends. Every `Mutate` site
// completes its mutation with no intervening suspend; the two that cannot -- a
// container bound to a `ref`/`inout` formal of a task -- use `Place` here and
// fire from the call's own after-write bracket instead.
void Backend::notify_container_mutation(const LValue& lv) {
    if (lv.chg == nullptr) {
        // A container living in a SIGNAL -- a module-level queue / dynamic /
        // associative array, or a static class property's shared slot -- has
        // no hidden notifier object: its waiters park on the signal's own Any
        // bucket (`wait(q.size() != 0)` registers there). A mutation is not a
        // Store, so the store-side service bracket never sees it; fire the Any
        // bucket here, unconditionally, for the same reason the property
        // notifier below fires unconditionally. `@` over a container is
        // refused by the type system, so every Any waiter on such a signal is
        // level-sensitive and re-tests its condition (LRM 9.4.3). PreInit
        // initializers generate no events (same rule as service_snapshot).
        if (!silent_stores_) {
            if (lv.is_signal) {
                service_signal_container_mutation(lv.sig);
            } else if (lv.mut_sig_valid) {
                // A deferred assoc element of a signal-resident map.
                service_signal_container_mutation(lv.mut_sig);
            }
        }
        return;
    }
    if (!lv.chg->container) {
        return;
    }
    if (lv.chg->field[static_cast<uint32_t>(Edge::Any)] == kNoNotify) {
        throw BackendError{ "internal: container change notifier with no any-change field" };
    }
    fire_prop_notifier(lv, Edge::Any);
}

// Wake the Any bucket of a SIGNAL holding a container, after a mutation of it.
// The unconditional-fire counterpart of service_wait_queue: a mutation has no
// old -> new word to classify (`q.size()` is computed, not stored), so every
// Any waiter is woken and re-tests. A signal nothing waits on emits no code.
void Backend::service_signal_container_mutation(Signal sig) {
    const auto i = static_cast<uint32_t>(sig);
    const auto e = static_cast<uint32_t>(Edge::Any);
    if (wait_sta_gv_[i][e] != nullptr) {
        service_static_queue(sig, e);
    }
    if (wait_dyn_gv_[i][e] != nullptr) {
        service_dynamic_queue(sig, e);
    }
    if (vif_wait_[i][e]) {
        emit_event_wake_list(vif_head_ptr(sig, e));
    }
    fire_event_trigger(sig, e);
}

// Wake everything parked on one of a class property's change notifiers. Shared
// by the three write paths that reach a notifier -- the classified integral /
// handle / real store, the container mutation and the string content compare --
// because the addressing is delicate: the notifier is an `event` PROPERTY of the
// same object, so its slot is the object body's GEP past the class header, and
// it is minted by emit_class_body_init at `new`, so the handle is always live
// and needs no null guard (as a plain `-> e` on a declared event needs none).
void Backend::fire_prop_notifier(const LValue& lv, Edge edge) {
    const uint32_t at = lv.chg->field[static_cast<uint32_t>(edge)];
    llvm::Value*   slot =
      b_.CreateStructGEP(lv.chg_body_ty, lv.chg_body, at + class_header_words(lv.chg->class_ref));
    emit_event_wake_list(event_obj_of(b_.CreateLoad(b_.getInt64Ty(), slot)));
}

// Pre-write CONTENT snapshot of a waited-on STRING class property (LRM 9.4.2 on
// a 6.16 value), or null when nothing waits on this place -- which is every
// string place in a design that waits on no string property.
//
// A string is the one notified type whose value is NOT the word the place holds.
// `obj.s = t` replaces the rt_string's buffer, so comparing handles would call
// every write a change; and two distinct handles over the same characters are
// the SAME value, which LRM 9.4.2's "any change in the value" makes observable
// -- re-assigning the same text must fire nothing. So the content is copied out
// before the write and compared with rt_string_cmp after it.
//
// The copy costs a malloc, paid only for a property some `@` / `wait` names. The
// scratch handle is an ENTRY-block alloca, so a bracket inside a loop reuses one
// slot rather than growing the stack, and the init / deinit pair is balanced
// across the bracket so each pass copies into a fresh handle.
llvm::Value* Backend::str_chg_snapshot(const LValue& lv) {
    if (lv.chg == nullptr) {
        return nullptr;
    }
    if (lv.chg->field[static_cast<uint32_t>(Edge::Any)] == kNoNotify) {
        // `@(posedge s)` is refused in the frontend (no LSB lattice, Table 9-2),
        // so a notified string property carries the any-change field and nothing
        // else. A gap means the two sides disagreed.
        throw BackendError{ "internal: string change notifier with no any-change field" };
    }
    auto* snap = entry_alloca(str_slot_ty(), "chg.str");
    snap->setAlignment(llvm::Align(alignof(rt_string)));
    ++str_chg_open_;
    b_.CreateCall(str_init_fn(), { snap });
    b_.CreateCall(str_assign_fn(),
                  { snap, lv_string_ptr(lv, StringUse::Read),
                    alloc_site_global(SourceText{ .where = "<change notifier>" }, "string") });
    return snap;
}

// Close a str_chg_snapshot bracket: fire the property's any-change notifier iff
// the characters differ, then release the snapshot.
void Backend::str_chg_after(const LValue& lv, llvm::Value* snap) {
    if (snap == nullptr) {
        return;
    }
    --str_chg_open_;
    llvm::Value* changed =
      b_.CreateICmpNE(b_.CreateCall(str_cmp_fn(), { snap, lv_string_ptr(lv, StringUse::Read) }),
                      b_.getInt32(0));
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* body = llvm::BasicBlock::Create(ctx_, "chgs.body", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "chgs.cont", fn);
    b_.CreateCondBr(changed, body, cont);
    b_.SetInsertPoint(body);
    fire_prop_notifier(lv, Edge::Any);
    b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
    b_.CreateCall(str_deinit_fn(), { snap });
}

// Alloca an empty result handle for each dynamic-array-returning call in the
// current body (the callee copies its return value into it). The handle is the
// call's lvalue, so a `z = f()` assignment / `f().size()` / `foreach(f())`
// reads it; it is freed at every body exit (deinit_body_strings).
void Backend::build_dynarr_temps() {
    dynarr_temp_list_.clear();
    dynarr_ret_.assign(ir_->inst_count(), nullptr);
    for (size_t k = 0; k < ir_->inst_count(); ++k) {
        const auto i = static_cast<Index>(k);
        if (ir_->tag(i) != Inst::Tag::Call && ir_->tag(i) != Inst::Tag::CallVirtual) {
            continue;
        }
        const Ref rty = mod_.function(call_func_id(i)).return_type;
        if (!is_handle_type(rty)) {
            continue;
        }
        auto* slot = handle_slot_ty(rty);
        auto* temp = b_.CreateAlloca(slot, nullptr, "handle.ret");
        temp->setAlignment(llvm::Align(8));
        b_.CreateStore(llvm::Constant::getNullValue(slot), temp); // empty handle
        // The temp OWNS the returned container for the body's lifetime, and
        // its payload is malloc'd, so it is a root exactly like a container
        // local. Registered while empty and never re-registered: the address
        // is fixed, so the entry keeps describing whatever container the slot
        // holds. Paired with the Remove in deinit_body_strings' temp loop.
        emit_gc_container_roots(GcRootOp::Add, rty, temp, slot);
        dynarr_ret_[k] = temp;
        dynarr_temp_list_.push_back({ temp, rty });
    }
}

// Alloca a result buffer for each aggregate-element queue pop in the current
// body, power-on-initialized so an empty pop reads the element default.
//
// Body-lifetime rather than per-site scratch for the same reason a string temp
// is: `rt_queue_pop_*` RELOCATES the element into the buffer, so the buffer owns
// whatever heap leaves the element had, and someone has to free them. The pop
// site releases the previous contents before reusing it (a pop in a loop), and
// deinit_body_strings releases the last one at every body exit.
void Backend::build_agg_elem_temps() {
    agg_elem_temp_list_.clear();
    agg_elem_ret_.assign(ir_->inst_count(), nullptr);
    for (size_t k = 0; k < ir_->inst_count(); ++k) {
        const auto i = static_cast<Index>(k);
        if (ir_->tag(i) != Inst::Tag::QueuePopBack && ir_->tag(i) != Inst::Tag::QueuePopFront) {
            continue;
        }
        const DynElem e = dynarr_elem(ir_->payload(i).op_type.type);
        if (e.is_agg) {
            auto* temp = b_.CreateAlloca(e.agg_ty, nullptr, "q.pop.agg");
            emit_agg_proto_fill(temp, e);
            // The proto image leaves every container leaf empty, so this
            // registers empty containers at fixed addresses. It is deliberately
            // NOT repeated at the pop site: that site deinits the previous
            // contents and re-fills the proto at the SAME address, so the entry
            // stays valid across every pop and only the body exit removes it.
            emit_gc_container_roots(GcRootOp::Add, e.elem_type, temp, e.agg_ty);
            agg_elem_ret_[k] = temp;
            agg_elem_temp_list_.push_back({ temp, e.elem_type });
            continue;
        }
        if (e.is_handle) {
            // A nested container element pops as a handle the runtime relocates
            // into this buffer, so the buffer owns that container's heap storage
            // exactly as an aggregate buffer owns its leaves.
            auto* temp = b_.CreateAlloca(e.slot_ty, nullptr, "q.pop.handle");
            b_.CreateStore(llvm::Constant::getNullValue(e.slot_ty), temp);
            emit_gc_container_roots(GcRootOp::Add, e.elem_type, temp, e.slot_ty);
            agg_elem_ret_[k] = temp;
            agg_elem_temp_list_.push_back({ temp, e.elem_type });
        }
    }
    // An aggregate-returning CALL whose result owns heap needs the same
    // treatment, and for the same reason: the callee copies its result out
    // through the trailing out-param PER LEAF (emit_agg_deep_copy), which
    // requires constructed leaves to assign over, and whatever heap the result
    // carries has to be released at the body's exit. A bare per-call alloca (the
    // plane-only case below in lower_call_common) would give it neither.
    for (size_t k = 0; k < ir_->inst_count(); ++k) {
        const auto i = static_cast<Index>(k);
        if (ir_->tag(i) != Inst::Tag::Call && ir_->tag(i) != Inst::Tag::CallVirtual) {
            continue;
        }
        const Ref ret = call_return_type(i);
        if (ret == Ref::Invalid || !is_agg_abi(ret) || !has_heap_leaf(ret)) {
            continue;
        }
        llvm::Type*    st   = agg_storage_ty(ret);
        auto*          temp = b_.CreateAlloca(st, nullptr, "call.agg.ret");
        const uint64_t size = env_->llmod.getDataLayout().getTypeAllocSize(st);
        b_.CreateMemCpy(temp, llvm::MaybeAlign(), agg_proto_global(ret), llvm::MaybeAlign(), b_.getInt64(size));
        emit_gc_container_roots(GcRootOp::Add, ret, temp, st);
        agg_elem_ret_[k] = temp;
        agg_elem_temp_list_.push_back({ temp, ret });
    }
}

// Dynamic-array operations over the handle lvalue (LRM 7.5). Element access
// rides the ElemSelect dynarr path, not these. Element facts come from the
// payload's own array-type handle, so each op works for a module signal or
// a frame-local dynamic array alike. Handles resolve through lv_handle_ptr
// so a struct-member / class-property array works too.
void Backend::lower_dyn_array_new(Index i) {
    const auto&   p  = ir_->payload(i).op_extra;
    const auto    da = ir_->extra_data<DynArrayArg>(p.extra);
    // The count is a signed number: it sign-extends so the runtime sees the
    // negative the design actually wrote and can reject it (LRM 7.5.1);
    // zero-extending would turn `new[-1]` on a 32-bit int into a 4294967295-element
    // request. The frontend zero-extends an unsigned count past its top bit.
    llvm::Value* n = cast(must_of(value_of(da.arg)), 64, true);
    b_.CreateCall(dynarr_new_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    elem_ops_global(da.array_type),
                    n,
                    alloc_site_global(loc_text(i), "dynamic array") });
}

void Backend::lower_dyn_array_new_init(Index i) {
    const auto&   p  = ir_->payload(i).op_extra;
    const auto    da = ir_->extra_data<DynArrayNewInitInfo>(p.extra);
    // Sign-extend a signed count, as in lower_dyn_array_new.
    llvm::Value* n = cast(must_of(value_of(da.size)), 64, true);
    // `new[n](src)` copies src's elements in, so the owner acquires every reference
    // src held. Plain `new[n]` (lower_dyn_array_new) is deliberately bare: it
    // preserves or drops elements and adds no reference. See lower_queue_push.
    emit_gc_barrier_for(lvalue_of(p.op));
    b_.CreateCall(dynarr_new_copy_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    elem_ops_global(da.array_type),
                    n,
                    lv_handle_ptr(lvalue_of(da.src), HandleUse::Read),
                    alloc_site_global(loc_text(i), "dynamic array") });
}

void Backend::lower_dyn_array_size(Index i) {
    llvm::Value* handle = lv_handle_ptr(lvalue_of(ir_->payload(i).op_type.op), HandleUse::Read);
    llvm::Value* len =
      b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 1));
    set_value(i, two_val(cast(len, 32, false)));
}

void Backend::lower_dyn_array_copy(Index i) {
    const auto& p  = ir_->payload(i).op_extra;
    const auto  da = ir_->extra_data<DynArrayArg>(p.extra);
    // Whole-array assign: the owner acquires every reference the source held.
    // See lower_queue_push.
    emit_gc_barrier_for(lvalue_of(p.op));
    b_.CreateCall(dynarr_assign_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    lv_handle_ptr(lvalue_of(da.arg), HandleUse::Read),
                    elem_ops_global(da.array_type),
                    alloc_site_global(loc_text(i), "dynamic array") });
}

void Backend::lower_dyn_array_copy_queue(Index i) {
    const auto& p  = ir_->payload(i).op_extra;
    const auto  da = ir_->extra_data<DynArrayArg>(p.extra);
    // Whole-array assign from a queue (LRM 7.5.1's resizeable assignment).
    // See lower_queue_push.
    emit_gc_barrier_for(lvalue_of(p.op));
    b_.CreateCall(dynarr_assign_queue_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    lv_handle_ptr(lvalue_of(da.arg), HandleUse::Read),
                    elem_ops_global(da.array_type),
                    alloc_site_global(loc_text(i), "dynamic array") });
}

// Array ordering methods (LRM 7.12.2). Each carries only the handle lvalue +
// interned type (op_type payload). Reverse is element-agnostic; Sort/Rsort
// pass the value comparator descriptor derived from the element type;
// Shuffle threads the calling thread's RNG state pointer.
//
// All three resolve the handle through lv_handle_ptr, so a struct-member,
// class-property or container-element dynamic array orders in place rather
// than addressing its parent aggregate's root.
void Backend::lower_dyn_array_reverse(Index i) {
    const auto& p = ir_->payload(i).op_type;
    b_.CreateCall(dynarr_reverse_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate), elem_ops_global(p.type) });
}

void Backend::lower_dyn_array_sort(Index i, bool reversed) {
    const auto&    p = ir_->payload(i).op_type;
    const SortDesc d = dynarr_sort_desc(p.type);
    b_.CreateCall(dynarr_sort_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    elem_ops_global(p.type),
                    b_.getInt32(d.kind),
                    b_.getInt32(d.width),
                    b_.getInt32(reversed ? 1 : 0) });
}

void Backend::lower_dyn_array_shuffle(Index i) {
    const auto& p = ir_->payload(i).op_type;
    // rng_slot_ is non-null in every body: a drawing process owns a frame
    // state, a subroutine takes its caller's, and the rest carry the fallback.
    assert(rng_slot_ != nullptr && "shuffle needs an RNG");
    b_.CreateCall(dynarr_shuffle_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate), elem_ops_global(p.type), rng_slot_ });
}

// The element lvalue of an aggregate-element dynamic array / queue: a
// heap-anchored aggregate root at logical slot `slot_index` of `data`.
//
// `base` is advanced to the element itself because agg_slot_ptr GEPs `root_ty`
// from `base` and ignores `elem_idx`. A following MemberSelect / ElemSelect then
// descends it exactly as it descends a module signal's aggregate (agg_member /
// agg_elem only push the GEP path, never touch the base), which is what makes
// `q[i].f` and `q[i][j]` fall out with no container-specific code. `dyn_inrange`
// carries LRM 7.4.5: an out-of-range leaf read yields the element default, a
// write is dropped, and the heap is never addressed past the live length.
LValue Backend::dyn_agg_elem_lvalue(const LValue&  container,
                                    const DynElem& e,
                                    llvm::Value*   data,
                                    llvm::Value*   slot_index,
                                    llvm::Value*   inrange) {
    LValue sel;
    sel.base = b_.CreateInBoundsGEP(b_.getInt8Ty(), data, b_.CreateMul(slot_index, b_.getInt64(dynarr_stride(e))));
    if (e.is_handle) {
        // A nested container element: the slot holds ITS handle, so the element
        // lvalue is a handle lvalue whose base is that slot. Out-of-range access
        // redirects to the sink (lv_handle_ptr), which is why the predicate has to
        // travel with it.
        sel.is_handle   = true;
        sel.handle_type = e.elem_type;
        sel.slot_ty     = e.slot_ty;
    } else {
        sel.is_agg  = true;
        sel.root_ty = e.agg_ty;
    }
    sel.is_signal     = container.is_signal;
    sel.sig           = container.sig;
    sel.dyn_inrange   = inrange;
    sel.bounds_checks = container.bounds_checks;
    // A handle written into a dynamic-array / queue element lands in the
    // container's malloc'd payload, reachable only through the owning object's
    // trace map (Gc.h:324-326), so the owner travels with the element lvalue. No
    // reachability gate here, unlike agg_member: a container reaches a class handle
    // exactly when its element does, so the owner is already null if it cannot.
    sel.gc_owner      = container.gc_owner;
    return sel;
}

// An lvalue addressing a single element slot at raw pointer `ptr` (a scratch
// alloca), for the shared lower_load / lower_store plane machinery.
LValue Backend::elem_scratch_lvalue(llvm::Value* ptr, const DynElem& e) {
    LValue lv;
    lv.base        = ptr;
    lv.slot_ty     = e.slot_ty;
    lv.has_z       = e.has_z;
    lv.narrow      = e.narrow;
    lv.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
    lv.elem_idx    = b_.getInt32(0);
    lv.store_width = e.width;
    lv.bit_off     = b_.getInt32(0);
    lv.width       = e.width;
    lv.is_signal   = false;
    return lv;
}

// The element power-on default as an in-register constant (X for 4-state, 0
// for 2-state / real), used to pre-fill a pop scratch so an empty-queue pop
// yields the default.
llvm::Constant* Backend::elem_default_const(const DynElem& e) {
    llvm::Constant*              mm = e.narrow ? zero(e.width) : x_mm(e.width);
    std::vector<llvm::Constant*> fields{ mm };
    if (e.has_z) {
        fields.push_back(zero(e.width));
    }
    return llvm::ConstantStruct::get(e.slot_ty, fields);
}

// Queue operations over the handle lvalue (LRM 7.10.2). Indexed element
// access q[i] rides the ElemSelect queue path, not these. Element values
// cross to the runtime through a scratch element slot (push / insert stage
// the value in; pop copies out). Handles resolve through lv_handle_ptr so a
// struct-member / class-property queue works too.
void Backend::lower_queue_size(Index i) {
    llvm::Value* handle = lv_handle_ptr(lvalue_of(ir_->payload(i).op_type.op), HandleUse::Read);
    llvm::Value* len =
      b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 2));
    set_value(i, two_val(cast(len, 32, false)));
}

void Backend::lower_queue_push(Index i, bool front) {
    const auto&   p     = ir_->payload(i).op_extra;
    const auto    qa    = ir_->extra_data<QueueArg>(p.extra);
    const DynElem e     = dynarr_elem(qa.queue_type);
    llvm::Value*  ops   = elem_ops_global(qa.queue_type);
    llvm::Value*  bound = b_.getInt64(std::get<Type>(mod_.pool.get(qa.queue_type)).left);
    // The write barrier for a container mutation reached through a class property
    // (§7). The barrier goes on the OWNING OBJECT rather than on the container,
    // because the container ABI carries no owner identity -- rt_queue_push_back
    // takes an rt_queue and an element image, and nothing in that call names the
    // object whose trace map is the only path to those elements (Gc.h:324-326).
    // The base handle is known HERE, which is why this is the site.
    //
    // Remembering the owner is also what makes Runtime.h's "elements are relocated
    // by raw memcpy" licence (ring growth, insert/delete shifts, sort, shuffle,
    // rt_*_move) need no instrumentation of its own: the remembered set names an
    // object, and the young collection re-traces that object's containers through
    // its element descriptors wherever the elements have moved to. Motion changes
    // no reference, so only the ops that ADD one are instrumented; pop, delete,
    // reset, resize and the ordering methods are deliberately bare.
    //
    // gc_owner is null unless the container is reached through a class property,
    // so a queue in design state, a static, or a frame local emits nothing -- all
    // three are scanned in full at every collection (Gc.h:319-323).
    emit_gc_barrier_for(lvalue_of(p.op));
    if (e.is_string) {
        // A string value is already an rt_string handle pointer; the runtime
        // deep-copies it into the new slot, so it is passed directly.
        b_.CreateCall(queue_push_fn(front),
                      { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                        ops,
                        bound,
                        str_of(qa.arg),
                        alloc_site_global(loc_text(i), "queue") });
        return;
    }
    if (e.is_agg || e.is_handle) {
        // Aggregate / nested-container element: the runtime takes `elem` by
        // pointer and deep-copies out of it (rt_elem_copy never retains it), so
        // the source's own storage image -- or its handle -- is handed over
        // directly. The frontend guarantees an lvalue operand here
        // (lower_container_element_value), staging a '{...} pattern into a
        // canonical aggregate temp when needed.
        llvm::Value* src =
          e.is_agg ? agg_slot_ptr(lvalue_of(qa.arg)) : lv_handle_ptr(lvalue_of(qa.arg), HandleUse::Read);
        b_.CreateCall(queue_push_fn(front),
                      { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                        ops,
                        bound,
                        src,
                        alloc_site_global(loc_text(i), "queue") });
        return;
    }
    auto* scratch = entry_alloca(e.slot_ty, "q.elem");
    lower_store(elem_scratch_lvalue(scratch, e), value_of(qa.arg));
    b_.CreateCall(queue_push_fn(front),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    ops,
                    bound,
                    scratch,
                    alloc_site_global(loc_text(i), "queue") });
}

void Backend::lower_queue_pop(Index i, bool front) {
    const auto&   p      = ir_->payload(i).op_type;
    const DynElem e      = dynarr_elem(p.type);
    llvm::Value*  ops    = elem_ops_global(p.type);
    const LValue& qlv    = lvalue_of(p.op);
    llvm::Value*  handle = lv_handle_ptr(qlv, HandleUse::Mutate);
    emit_empty_pop_bounds_check(i, handle, qlv);
    if (e.is_string) {
        // The result handle is this instruction's string temp
        // (produces_string_temp). Reset it to the owned-empty state, then
        // the runtime RELOCATES the popped element into it (an empty pop
        // leaves the empty string, the Table 7-1 default).
        llvm::Value* out = str_of(i);
        b_.CreateCall(str_deinit_fn(), { out });
        b_.CreateCall(str_init_fn(), { out });
        b_.CreateCall(queue_pop_fn(front), { handle, ops, out });
        return;
    }
    if (e.is_agg) {
        // Aggregate element: the runtime RELOCATES the popped element into a
        // body-lifetime buffer (build_agg_elem_temps), which therefore owns any
        // heap leaves it carries. Release the previous contents first -- a pop in
        // a loop reuses the buffer -- then restore the power-on image so an empty
        // pop yields the Table 7-1 default rather than the last value. The
        // buffer's lvalue is the result, the same safe-temp shape an
        // aggregate-returning call hands back.
        llvm::Value* out = agg_elem_ret_[static_cast<uint32_t>(i)];
        if (has_heap_leaf(e.elem_type)) {
            std::vector<llvm::Value*> path;
            emit_agg_deinit(e.elem_type, out, path, e.agg_ty);
        }
        emit_agg_proto_fill(out, e);
        b_.CreateCall(queue_pop_fn(front), { handle, ops, out });
        LValue lv{};
        lv.base    = out;
        lv.root_ty = e.agg_ty;
        lv.is_agg  = true;
        set_lvalue(i, lv);
        return;
    }
    if (e.is_handle) {
        // Nested container element: same relocation shape as the aggregate case.
        // Empty the buffer first (a pop in a loop reuses it, and an empty-queue pop
        // leaves it untouched -- which is then the empty container Table 7-1 asks
        // for), then hand its lvalue back as a handle.
        llvm::Value* out = agg_elem_ret_[static_cast<uint32_t>(i)];
        emit_handle_delete(out, e.elem_type);
        b_.CreateCall(queue_pop_fn(front), { handle, ops, out });
        LValue lv{};
        lv.base        = out;
        lv.slot_ty     = e.slot_ty;
        lv.is_handle   = true;
        lv.handle_type = e.elem_type;
        set_lvalue(i, lv);
        return;
    }
    auto* scratch = entry_alloca(e.slot_ty, "q.elem");
    // Pre-fill with the default: the runtime leaves an empty-queue pop
    // untouched, so the load below then reads the element default.
    b_.CreateStore(elem_default_const(e), scratch);
    b_.CreateCall(queue_pop_fn(front), { handle, ops, scratch });
    Plane         v = lower_load(elem_scratch_lvalue(scratch, e));
    const ValueType t = type_of(i);
    if (t.is_real) {
        v = real_plane(planes_to_real(v, t.width));
    }
    set_value(i, v);
}

void Backend::lower_queue_insert(Index i) {
    const auto&   p     = ir_->payload(i).op_extra;
    const auto    qi    = ir_->extra_data<QueueInsertInfo>(p.extra);
    const DynElem e     = dynarr_elem(qi.queue_type);
    llvm::Value*  ops   = elem_ops_global(qi.queue_type);
    llvm::Value*  idx   = cast(must_of(value_of(qi.idx)), 64, false);
    llvm::Value*  bound = b_.getInt64(std::get<Type>(mod_.pool.get(qi.queue_type)).left);
    const LValue& qlv   = lvalue_of(p.op);
    llvm::Value*  handle = lv_handle_ptr(qlv, HandleUse::Mutate);
    // A new element, so a new reference. See lower_queue_push.
    emit_gc_barrier_for(qlv);
    // Inserting AT the size is an append, so the legal range is [0, size].
    emit_queue_bounds_check(i, handle, qi.idx, qlv, /*allow_append=*/true);
    if (e.is_string) {
        b_.CreateCall(queue_insert_fn(), { handle, ops, bound, idx, str_of(qi.val),
                                           alloc_site_global(loc_text(i), "queue") });
        return;
    }
    if (e.is_agg || e.is_handle) {
        llvm::Value* src =
          e.is_agg ? agg_slot_ptr(lvalue_of(qi.val)) : lv_handle_ptr(lvalue_of(qi.val), HandleUse::Read);
        b_.CreateCall(queue_insert_fn(), { handle, ops, bound, idx, src, alloc_site_global(loc_text(i), "queue") });
        return;
    }
    auto* scratch = entry_alloca(e.slot_ty, "q.elem");
    lower_store(elem_scratch_lvalue(scratch, e), value_of(qi.val));
    b_.CreateCall(queue_insert_fn(),
                  { handle, ops, bound, idx, scratch, alloc_site_global(loc_text(i), "queue") });
}

void Backend::lower_queue_delete_idx(Index i) {
    const auto&   p      = ir_->payload(i).op_extra;
    const auto    qa     = ir_->extra_data<QueueArg>(p.extra);
    llvm::Value*  idx    = cast(must_of(value_of(qa.arg)), 64, false);
    const LValue& qlv    = lvalue_of(p.op);
    llvm::Value*  handle = lv_handle_ptr(qlv, HandleUse::Mutate);
    emit_queue_bounds_check(i, handle, qa.arg, qlv, /*allow_append=*/false);
    b_.CreateCall(queue_delete_idx_fn(), { handle, elem_ops_global(qa.queue_type), idx });
}

void Backend::lower_queue_delete_all(Index i) {
    const auto& p = ir_->payload(i).op_type;
    emit_handle_delete(lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate), p.type);
}

void Backend::lower_queue_copy(Index i) {
    const auto& p  = ir_->payload(i).op_extra;
    const auto  qa = ir_->extra_data<QueueArg>(p.extra);
    // Whole-container assign: the owner acquires every reference the source held.
    // See lower_queue_push.
    emit_gc_barrier_for(lvalue_of(p.op));
    b_.CreateCall(queue_assign_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    lv_handle_ptr(lvalue_of(qa.arg), HandleUse::Read),
                    elem_ops_global(qa.queue_type),
                    alloc_site_global(loc_text(i), "queue") });
}

// q = d: deep copy a dynamic array (arg) into a queue (op). Source handle is
// an rt_dynarr, so this routes to rt_queue_assign_dynarr, not rt_queue_assign.
void Backend::lower_queue_copy_dynarr(Index i) {
    const auto& p  = ir_->payload(i).op_extra;
    const auto  qa = ir_->extra_data<QueueArg>(p.extra);
    // Whole-container assign from a dynamic array. See lower_queue_push.
    emit_gc_barrier_for(lvalue_of(p.op));
    b_.CreateCall(queue_assign_dynarr_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    lv_handle_ptr(lvalue_of(qa.arg), HandleUse::Read),
                    elem_ops_global(qa.queue_type),
                    alloc_site_global(loc_text(i), "queue") });
}

void Backend::lower_queue_append_range(Index i) {
    const auto& p     = ir_->payload(i).op_extra;
    const auto  qs    = ir_->extra_data<QueueSliceInfo>(p.extra);
    llvm::Value* bound = b_.getInt64(std::get<Type>(mod_.pool.get(qs.queue_type)).left);
    // Slice bounds are signed indices (`q[-1:2]` clamps low to 0); sign-extend.
    llvm::Value* lo = cast(must_of(value_of(qs.lo)), 64, /*sign=*/true);
    llvm::Value* hi = cast(must_of(value_of(qs.hi)), 64, /*sign=*/true);
    // A slice append copies a range of elements in. See lower_queue_push.
    emit_gc_barrier_for(lvalue_of(p.op));
    b_.CreateCall(queue_append_range_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    lv_handle_ptr(lvalue_of(qs.src), HandleUse::Read),
                    elem_ops_global(qs.queue_type),
                    bound,
                    lo,
                    hi,
                    alloc_site_global(loc_text(i), "queue") });
}

// Queue ordering methods (LRM 7.12.2) — same shape as the dynarr variants
// over the ring's logical order (op_type payload: handle lvalue + type).
void Backend::lower_queue_reverse(Index i) {
    const auto& p = ir_->payload(i).op_type;
    b_.CreateCall(queue_reverse_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate), elem_ops_global(p.type) });
}

void Backend::lower_queue_sort(Index i, bool reversed) {
    const auto&    p = ir_->payload(i).op_type;
    const SortDesc d = dynarr_sort_desc(p.type);
    b_.CreateCall(queue_sort_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    elem_ops_global(p.type),
                    b_.getInt32(d.kind),
                    b_.getInt32(d.width),
                    b_.getInt32(reversed ? 1 : 0) });
}

void Backend::lower_queue_shuffle(Index i) {
    const auto& p = ir_->payload(i).op_type;
    assert(rng_slot_ != nullptr && "shuffle needs an RNG");
    b_.CreateCall(queue_shuffle_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate), elem_ops_global(p.type), rng_slot_ });
}

// The live element count of a container handle, and its heap data base.
std::pair<llvm::Value*, llvm::Value*> Backend::container_len_base(llvm::Value* handle, Ref type) {
    if (is_queue_type(type)) {
        return { b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 2)),
                 b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 0)) };
    }
    return { b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 1)),
             b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 0)) };
}

// The physical ring slot of logical queue position `idx`: head + idx, less cap
// when that reaches it. This is rt_queue_slot (Runtime.h), which has the
// reasoning: it needs idx < cap and head < cap, so head + idx < 2 * cap and one
// subtraction wraps it. Every caller gates the element access on idx < len <= cap,
// so for any other idx the slot is never addressed; it is still a defined integer,
// since the add, the compare and the subtract carry no nuw / nsw and nothing here
// can become poison. There is no clamp for a zero capacity either: an empty queue
// has len == 0, so every idx is out of range.
llvm::Value* Backend::queue_ring_slot(llvm::Value* head, llvm::Value* idx, llvm::Value* cap) {
    llvm::Value* sum = b_.CreateAdd(head, idx);
    return b_.CreateSelect(b_.CreateICmpUGE(sum, cap), b_.CreateSub(sum, cap), sum);
}

// The lvalue of logical element `idx` of a container, with no bounds predicate:
// for callers that already walk within the live length. A queue's logical index
// wraps through its ring (queue_ring_slot).
LValue Backend::container_elem_lvalue(llvm::Value*   handle,
                                      llvm::Value*   data,
                                      Ref            type,
                                      const DynElem& e,
                                      llvm::Value*   idx) {
    llvm::Value* phys = idx;
    if (is_queue_type(type)) {
        llvm::Value* head = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 1));
        llvm::Value* cap  = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(queue_slot_ty(), handle, 3));
        phys              = queue_ring_slot(head, idx, cap);
    }
    if (e.is_agg || e.is_handle) {
        return dyn_agg_elem_lvalue(LValue{}, e, data, phys, /*inrange=*/nullptr);
    }
    // A plane element: the same `[0 x slot]` shape the ElemSelect arms build, with
    // no bounds predicate (the caller walks within the live length).
    LValue el;
    el.base        = data;
    el.slot_ty     = e.slot_ty;
    el.has_z       = e.has_z;
    el.narrow      = e.narrow;
    el.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
    el.elem_idx    = cast(phys, 32, false);
    el.store_width = e.width;
    el.bit_off     = b_.getInt32(0);
    el.width       = e.width;
    return el;
}

// Element `idx` of a plain LINEAR data buffer (no ring wrap): the same body
// as container_elem_lvalue's non-queue path, but callable with no `type` Ref
// at all -- for denormalize_variable_agg_key's scratch dynarray (P2), which
// is always linear regardless of whether the destination key leaf is a
// queue or a dynamic array, so it never needs the ring-wrap branch that
// container_elem_lvalue's `type` parameter exists only to select.
LValue Backend::tmp_elem_lvalue(llvm::Value* data, const DynElem& e, llvm::Value* idx) {
    if (e.is_agg || e.is_handle) {
        return dyn_agg_elem_lvalue(LValue{}, e, data, idx, /*inrange=*/nullptr);
    }
    LValue el;
    el.base        = data;
    el.slot_ty     = e.slot_ty;
    el.has_z       = e.has_z;
    el.narrow      = e.narrow;
    el.array_ty    = llvm::ArrayType::get(e.slot_ty, 0);
    el.elem_idx    = cast(idx, 32, false);
    el.store_width = e.width;
    el.bit_off     = b_.getInt32(0);
    el.width       = e.width;
    return el;
}

// `a == b` over a container whose elements are aggregates or nested containers
// (LRM 11.4.5 / 7.10): equal sizes and every element pair equal, X-propagating
// through the same Land reduction the struct / array cases use.
//
// A loop rather than the flat runtime comparator because the element has no
// single-word value image: each pair recurses through lower_agg_eq, which is
// also what gives a queue of queues (or of structs holding queues) a compare at
// all. The must / may accumulators live in allocas so the loop needs no PHI
// plumbing; LLVM promotes them.
Plane Backend::emit_container_eq_loop(const LValue& lhs, const LValue& rhs, Ref type, const DynElem& e) {
    llvm::Value* ha      = lv_handle_ptr(lhs, HandleUse::Read);
    llvm::Value* hb      = lv_handle_ptr(rhs, HandleUse::Read);
    const auto [la, ba]  = container_len_base(ha, type);
    const auto [lb, bb]  = container_len_base(hb, type);
    llvm::Value* size_eq = b_.CreateICmpEQ(la, lb);

    auto* i1   = b_.getInt1Ty();
    auto* must = entry_alloca(i1, "ceq.must");
    auto* may  = entry_alloca(i1, "ceq.may");
    auto* idx  = entry_alloca(b_.getInt64Ty(), "ceq.idx");
    b_.CreateStore(size_eq, must);
    b_.CreateStore(size_eq, may);
    b_.CreateStore(b_.getInt64(0), idx);

    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* head = llvm::BasicBlock::Create(ctx_, "ceq.head", fn);
    auto* body = llvm::BasicBlock::Create(ctx_, "ceq.body", fn);
    auto* done = llvm::BasicBlock::Create(ctx_, "ceq.done", fn);
    // Sizes differing is already a definite 0, so the element walk is skipped.
    b_.CreateCondBr(size_eq, head, done);

    b_.SetInsertPoint(head);
    llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idx);
    b_.CreateCondBr(b_.CreateICmpULT(i, la), body, done);

    b_.SetInsertPoint(body);
    llvm::Value* k    = b_.CreateLoad(b_.getInt64Ty(), idx);
    const Plane  e_eq = lower_agg_eq(
      container_elem_lvalue(ha, ba, type, e, k), container_elem_lvalue(hb, bb, type, e, k), e.elem_type);
    const Plane acc =
      lower_logical(Inst::Tag::Land, bit_plane(b_.CreateLoad(i1, must), b_.CreateLoad(i1, may)), e_eq);
    b_.CreateStore(b_.CreateTrunc(must_of(acc), i1), must);
    b_.CreateStore(b_.CreateTrunc(may_of(acc), i1), may);
    b_.CreateStore(b_.CreateAdd(k, b_.getInt64(1)), idx);
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
    return bit_plane(b_.CreateLoad(i1, must), b_.CreateLoad(i1, may));
}

} // namespace clotho::ir::backend
