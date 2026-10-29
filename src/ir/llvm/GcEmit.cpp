// Garbage-collector interface: GC maps, class descriptors, root tables, allocation, safepoints and barriers.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// The byte offset of one gc_frame_ty() field, from the DataLayout rather than a
// literal. Used both to address `next` off a possibly-null `prev` and to assert
// the `prev`-at-offset-0 identity the two emitters rely on.
uint32_t Backend::gc_frame_off(unsigned field) {
    assert(field < gc_frame_ty()->getNumElements() && "a frame field index is in range");
    const auto* sl = env_->llmod.getDataLayout().getStructLayout(gc_frame_ty());
    return static_cast<uint32_t>(sl->getElementOffset(field));
}

// sizeof(rt_gc_mutator) as our mirror computes it, handed to the runtime in
// rt_gc_config::mutator_bytes and compared there against rt_gc_mutator_bytes()
// (Gc.h:255-258). A mismatch means the mirror has drifted, so every offset
// gc_mut_off() hands the allocator is suspect and the run must fail at startup
// rather than at the first bump.
int32_t Backend::gc_mutator_bytes() {
    const uint64_t sz = env_->llmod.getDataLayout().getTypeAllocSize(gc_mutator_ty());
    assert(sz >= sizeof(void*) && "the mutator holds at least one field");
    assert(sz % sizeof(void*) == 0 && "a pointer-bearing LP64 struct is word-sized");
    return static_cast<int32_t>(sz);
}

// The byte offset of one rt_gc_mutator field, from our own DataLayout rather than
// a literal. Every field access generated code emits routes through here, so the
// mirror above is the single place a layout change has to be made.
uint32_t Backend::gc_mut_off(GcMutField f) {
    const auto     idx = static_cast<unsigned>(f);
    const auto&    dl  = env_->llmod.getDataLayout();
    const uint64_t off = dl.getStructLayout(gc_mutator_ty())->getElementOffset(idx);
    [[maybe_unused]] const uint64_t w =
      dl.getTypeStoreSize(gc_mutator_ty()->getElementType(idx));
    assert(off % w == 0 && "a mutator field is naturally aligned");
    assert(off + w <= static_cast<uint64_t>(gc_mutator_bytes()) &&
           "a mutator field lies inside the mutator context");
    return static_cast<uint32_t>(off);
}

// The address of one rt_gc_mutator field, off the pointer gc_mutator_cur() hoisted
// for the function being emitted. Every inlined mutator access in the file goes
// through here, so the mirror is the only place that knows a layout.
//
// `cursor` sits at offset 0, so its address IS the mutator pointer: emitting a
// zero GEP for it would put an instruction in every module that exists only to be
// folded away again, and the allocation fast path takes that address on every
// single `new`.
llvm::Value* Backend::gc_mut_ptr(GcMutField f, const char* nm) {
    llvm::Value*   mut = gc_mutator_cur();
    const uint32_t off = gc_mut_off(f);
    assert(mut != nullptr && "the mutator pointer is hoisted before any field access");
    assert((f != GcMutField::Cursor || off == 0) &&
           "the cursor heads the mutator, so its address is the mutator pointer");
    if (off == 0) {
        return mut;
    }
    return b_.CreateConstInBoundsGEP1_32(b_.getInt8Ty(), mut, off, nm);
}

// offsetof(rt_gc_mutator, request), handed to the runtime in
// rt_gc_config::request_offset so a mutator layout change fails loudly at
// rt_gc_init instead of leaving every safepoint polling a byte that is not the
// request flag (Gc.h:193-195).
int32_t Backend::gc_mutator_request_offset() {
    const uint32_t off = gc_mut_off(GcMutField::Request);
    assert(off % sizeof(uint32_t) == 0 && "the request flag is a naturally aligned i32");
    return static_cast<int32_t>(off);
}

// A private `[N x i32]` byte-offset constant for the handle tables the collector
// reads -- rt_gc_map::handles and rt_elem_ops::handles. An empty list yields
// null, which is the ABI's "no handles" encoding and is always paired with a
// zero count by the callers below.
llvm::Constant* Backend::gc_offsets_global(const std::vector<uint32_t>& offsets, const std::string& nm) {
    if (offsets.empty()) {
        return llvm::Constant::getNullValue(raw_ptr_ty());
    }
    std::vector<llvm::Constant*> words;
    words.reserve(offsets.size());
    for (const uint32_t off : offsets) {
        words.push_back(b_.getInt32(off));
    }
    assert(words.size() == offsets.size() && "every offset became one word");
    auto* arr_ty = llvm::ArrayType::get(b_.getInt32Ty(), words.size());
    assert(arr_ty->getNumElements() == offsets.size() && "the array length is the count");
    return new llvm::GlobalVariable(env_->llmod, arr_ty, /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage,
                                    llvm::ConstantArray::get(arr_ty, words), nm);
}

// The byte offsets, inside a NORMALIZED KEY span, of the class-handle leaves of an
// aggregate key. Appended to `out` starting at `base`.
//
// This walk exists because a key is a strong reference: an entry keyed by an object
// keeps that object alive (LRM 7.8.3), and the handle exists ONLY in the key bytes,
// so the collector has to decode it back out of them.
//
// It is the third walk over the same shape, and deliberately so: `agg_key_bytes`
// (:17080) sizes the span and `emit_agg_key_bytes` (:17158) writes it, both as
// struct fields in declaration order, then array elements in order, then
// `ceil(W/8)` bytes per leaf with no padding (assoc-keys.md §2). Keeping the same
// three-arm shape is what makes these offsets provably the ones the serializer
// produces. The offsets are SERIALIZED, not storage: a storage walk would place a
// handle after a struct's padding and read the wrong bytes, which is exactly the
// mistake a `getStructLayout` shortcut would make here.
void Backend::agg_key_handle_offsets(Ref type, uint32_t base, std::vector<uint32_t>& out) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t off = base;
        for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
            agg_key_handle_offsets(f.type, off, out);
            off += agg_key_bytes(f.type);
        }
        assert(off == base + agg_key_bytes(type) && "the field walk covered the span");
        return;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        const uint32_t n      = array_dim(t);
        const uint32_t stride = agg_key_bytes(t.element);
        for (uint32_t k = 0; k < n; ++k) {
            agg_key_handle_offsets(t.element, base + k * stride, out);
        }
        return;
    }
    if (t.kind == Type::Kind::Class) {
        // 64-bit unsigned (InternPool.cpp:177-182), so emit_key_bytes writes exactly
        // 8 big-endian bytes here with no sign flip for the collector to undo.
        assert(agg_key_bytes(type) == 8 && "a class handle normalizes to 8 key bytes");
        out.push_back(base);
    }
}

// The heap object-body LLVM type of a class (LRM 8.3): a StructType mirroring
// the property table, one field per property (`agg_storage_ty` of its type — a
// class-handle property is a leaf i64 slot, not a nested body, which is what
// makes a self-referential class resolve without a fixpoint). Cached so the
// body pointer's GEP source type (`HandleDeref` root_ty) and the MemberSelect
// field GEPs share one identity. Distinct from `agg_storage_ty(classRef)`,
// which yields the *handle* leaf slot (a class value is a handle, not a body).
// Number of leading LLVM fields reserved before a class's properties: one, the
// descriptor pointer at field 0 (LRM 8.16 / 8.20). A property's GEP index into
// the object body is its 0-based declaration index plus this offset; the
// struct_fields table and logical field path stay 0-based.
//
// Every class carries the word, not only a polymorphic one. The header has to
// be present or absent uniformly across a whole hierarchy -- prefix layout puts
// the base's properties first precisely so an upcast is the same pointer at the
// same offsets -- and whether some *derived* class introduces a virtual method
// or is `$cast` to is not a property of the class being laid out.
uint32_t Backend::class_header_words(Ref class_ref) const {
    const Type& t = std::get<Type>(mod_.pool.get(class_ref));
    return t.kind == Type::Kind::Class ? 1u : 0u;
}

llvm::StructType* Backend::class_body_ty(Ref class_ref) {
    const auto key = static_cast<uint32_t>(class_ref);
    if (const auto it = class_body_ty_.find(key); it != class_body_ty_.end()) {
        return it->second;
    }
    const Type&              t = std::get<Type>(mod_.pool.get(class_ref));
    std::vector<llvm::Type*> fields;
    // Every class carries a descriptor pointer at field 0, at the same offset
    // in every class of a hierarchy so an upcast reads it unchanged.
    fields.push_back(raw_ptr_ty());
    for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
        fields.push_back(agg_storage_ty(f.type));
    }
    auto* st              = llvm::StructType::create(ctx_, fields, "clotho.class.body");
    class_body_ty_[key]   = st;
    return st;
}

// A class's runtime descriptor: its ancestor display followed by its vtable
// slots. One per class per module (deduped by name).
//
// The object header points at slot 0, not at the descriptor base, so a virtual
// dispatch is one load plus a constant-index GEP and the display sits at
// constant NEGATIVE offsets -- the same shape the Itanium C++ ABI gives
// `typeinfo` at vptr[-1]. `display[d]` is the type id of the ancestor at depth
// d, padded with 0 ("no class") past this class's own depth, so a `$cast` to a
// class at depth d is one load of display[d] and one compare against that
// class's id, with no dependence on the object's own depth.
//
// The trace map pointer is PREPENDED, so it sits immediately below the display --
// one load from any handle, at a design-wide constant negative offset from the
// vptr (Gc.h:68-84). Prepending is what keeps the offset independent of the
// vtable slot count; reaching the map from the descriptor base instead would make
// it depend on it.
//
// `$cast`'s display arithmetic does not have to know about the map, and the reason
// is structural: the display still ends exactly where slot 0 begins, so display[d]
// is at `vptr - 4*kMaxClassDepth + 4*d` -- literally the offset ClassIsA computes
// below (`4 * class_depth(dest) - 4 * kMaxClassDepth`). The map pointer lands
// BELOW the display, i.e. further negative, so no display offset depends on it.
// Interface classes (LRM 8.26) add two more regions, and BOTH sit below the
// trace-map pointer: the implemented-interface bitmap a `$cast` to an interface
// type tests, then the interface-method table an interface-handle dispatch loads
// its callee from. Placing them further negative is what keeps every offset
// above them -- the map's and the whole display's -- exactly where they were, so
// the runtime's own computation of the map offset and its startup cross-check
// need no notion of interface classes at all.
//
// Both regions have DESIGN-WIDE extents (Module::iface_count /
// iface_slot_count), which is what makes an entry's offset a constant at a site
// that does not know the object's class. They are emitted only when the design
// declares an interface class; otherwise this is byte-for-byte the descriptor
// clotho has always emitted (docs/design/interface-classes.md §4.3, §9).
llvm::StructType* Backend::class_desc_ty(size_t slots) {
    auto* display = llvm::ArrayType::get(b_.getInt32Ty(), kMaxClassDepth);
    auto* vtable  = llvm::ArrayType::get(raw_ptr_ty(), slots);
    if (mod_.iface_count == 0) {
        return llvm::StructType::get(ctx_, { raw_ptr_ty(), display, vtable });
    }
    auto* islots = llvm::ArrayType::get(raw_ptr_ty(), mod_.iface_slot_count);
    auto* impl   = llvm::ArrayType::get(b_.getInt64Ty(), iface_bitmap_words());
    return llvm::StructType::get(ctx_, { islots, impl, raw_ptr_ty(), display, vtable });
}

// Words in the implemented-interface bitmap: one bit per interface class in the
// design, rounded up. Zero when the design declares none.
uint32_t Backend::iface_bitmap_words() const {
    return (mod_.iface_count + 63) / 64;
}

// Element indices into class_desc_ty, which gains two leading fields when the
// design declares an interface class.
unsigned Backend::desc_map_field() const {
    return mod_.iface_count == 0 ? 0 : 2;
}

unsigned Backend::desc_vtable_field() const {
    return mod_.iface_count == 0 ? 2 : 4;
}

// The constant negative byte offset from a vptr to element `field` of its own
// descriptor, computed from the DataLayout rather than hardcoded. The slot count
// cannot affect it: every field this is asked about precedes the vtable.
int64_t Backend::desc_field_vptr_offset(unsigned field) {
    auto*       ty = class_desc_ty(/*slots=*/1);
    const auto* sl = env_->llmod.getDataLayout().getStructLayout(ty);
    return static_cast<int64_t>(sl->getElementOffset(field)) -
           static_cast<int64_t>(sl->getElementOffset(desc_vtable_field()));
}

// The offset handed to the runtime in rt_gc_config::map_vptr_offset, so a
// descriptor layout change fails loudly at rt_gc_init instead of reading garbage
// (Gc.h:167-170). Unchanged by the interface regions, deliberately (above).
int32_t Backend::gc_map_vptr_offset() {
    const int64_t off = desc_field_vptr_offset(desc_map_field());
    assert(off < 0 && "the map pointer sits below the vptr");
    return static_cast<int32_t>(off);
}

llvm::GlobalVariable* Backend::class_desc_global(Ref class_ref) {
    const std::string nm = "clotho.classdesc." + std::to_string(static_cast<uint32_t>(class_ref));
    if (auto* g = env_->llmod.getNamedGlobal(nm)) {
        return g;
    }
    const ClassDesc* cd = mod_.class_desc(class_ref);
    if (cd == nullptr) {
        throw BackendError{ "internal: class without a descriptor" };
    }
    std::vector<llvm::Constant*> entries;
    entries.reserve(cd->slots.size());
    for (const FunctionId fid : cd->slots) {
        entries.push_back(sym(fn_decls_[static_cast<uint32_t>(fid)]));
    }
    std::vector<llvm::Constant*> disp(kMaxClassDepth, b_.getInt32(0));
    // Walk to the root filling the display back to front: `element` is the base
    // class Ref, so the chain length is the class's depth.
    uint32_t depth = class_depth(class_ref);
    for (Ref k = class_ref; k != Ref::Invalid; k = std::get<Type>(mod_.pool.get(k)).element) {
        const ClassDesc* anc = mod_.class_desc(k);
        if (anc == nullptr) {
            throw BackendError{ "internal: class without a descriptor" };
        }
        disp[depth--] = b_.getInt32(anc->type_id);
    }
    auto*                        ty = class_desc_ty(entries.size());
    std::vector<llvm::Constant*> init_fields;
    if (mod_.iface_count != 0) {
        // The interface-method table, padded to the design-wide extent so an
        // entry's offset is one constant (LRM 8.26). A class that implements no
        // interface class emits an all-holes table: it can never be the object
        // behind an interface handle, so no dispatch reads it.
        std::vector<llvm::Constant*> islots;
        islots.reserve(mod_.iface_slot_count);
        for (uint32_t i = 0; i < mod_.iface_slot_count; ++i) {
            if (i < cd->iface_slots.size()) {
                islots.push_back(sym(fn_decls_[static_cast<uint32_t>(cd->iface_slots[i])]));
            } else if (mod_.iface_unreachable.has_value()) {
                islots.push_back(sym(fn_decls_[static_cast<uint32_t>(*mod_.iface_unreachable)]));
            } else {
                // The design declares an interface class but nothing implements
                // one, so no dispatch can land in this table at all: the only
                // handle that could reach it is null, and the null guard ahead of
                // the vtable load fires first (LRM 8.4).
                islots.push_back(llvm::Constant::getNullValue(raw_ptr_ty()));
            }
        }
        init_fields.push_back(
          llvm::ConstantArray::get(llvm::ArrayType::get(raw_ptr_ty(), mod_.iface_slot_count), islots));
        // The implemented-interface bitmap: bit `id` of word `id / 64`.
        std::vector<uint64_t> words(iface_bitmap_words(), 0);
        for (const uint32_t id : cd->iface_ids) {
            words[id / 64] |= uint64_t{ 1 } << (id % 64);
        }
        std::vector<llvm::Constant*> bits;
        bits.reserve(words.size());
        for (const uint64_t w : words) {
            bits.push_back(b_.getInt64(w));
        }
        init_fields.push_back(
          llvm::ConstantArray::get(llvm::ArrayType::get(b_.getInt64Ty(), words.size()), bits));
    }
    init_fields.push_back(class_gc_map_global(class_ref));
    init_fields.push_back(
      llvm::ConstantArray::get(llvm::ArrayType::get(b_.getInt32Ty(), kMaxClassDepth), disp));
    init_fields.push_back(
      llvm::ConstantArray::get(llvm::ArrayType::get(raw_ptr_ty(), entries.size()), entries));
    auto* init = llvm::ConstantStruct::get(ty, init_fields);
    return new llvm::GlobalVariable(
      env_->llmod, ty, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init, nm);
}

// Depth of a class in its hierarchy: 0 for a root, +1 per base.
uint32_t Backend::class_depth(Ref class_ref) const {
    uint32_t d = 0;
    for (Ref k = std::get<Type>(mod_.pool.get(class_ref)).element; k != Ref::Invalid;
         k     = std::get<Type>(mod_.pool.get(k)).element) {
        ++d;
    }
    return d;
}

// The value an object body's header holds: the address of its class's vtable
// slot 0, i.e. the descriptor global past the trace map pointer, the display,
// and (in a design with interface classes) the two interface regions that
// precede them.
llvm::Constant* Backend::class_vptr(Ref class_ref) {
    llvm::GlobalVariable* g = class_desc_global(class_ref);
    return llvm::ConstantExpr::getGetElementPtr(
      g->getValueType(), g,
      std::array<llvm::Constant*, 3>{ b_.getInt32(0), b_.getInt32(desc_vtable_field()), b_.getInt32(0) });
}

// A stand-in object body for a null handle in a `$cast` type test: one word
// pointing just past an all-zero display, so reading any display entry off it
// yields 0 ("no class") instead of faulting. Lets the test stay branch-free;
// the null answer itself comes from the separate compare against 0.
//
// With interface classes in the design the same word must also stand in for the
// implemented-interface bitmap an interface test reads, which sits below the
// display -- so the zero region becomes a whole slot-less descriptor and the
// word points at where its vtable would start. A zeroed bitmap reads as
// "implements nothing", the closed-fail answer, exactly as a zeroed display
// reads as "no class".
llvm::Constant* Backend::null_object_global() {
    if (auto* g = env_->llmod.getNamedGlobal("clotho.nullobj")) {
        return g;
    }
    llvm::Constant* past = nullptr;
    if (mod_.iface_count == 0) {
        auto* disp_ty = llvm::ArrayType::get(b_.getInt32Ty(), kMaxClassDepth);
        auto* disp    = new llvm::GlobalVariable(env_->llmod,
                                                 disp_ty,
                                                 /*isConstant=*/true,
                                                 llvm::GlobalValue::PrivateLinkage,
                                                 llvm::ConstantAggregateZero::get(disp_ty),
                                                 "clotho.nulldisplay");
        past          = llvm::ConstantExpr::getGetElementPtr(
          disp_ty, disp, std::array<llvm::Constant*, 2>{ b_.getInt32(0), b_.getInt32(kMaxClassDepth) });
    } else {
        auto* desc_ty = class_desc_ty(/*slots=*/0);
        auto* desc    = new llvm::GlobalVariable(env_->llmod,
                                                 desc_ty,
                                                 /*isConstant=*/true,
                                                 llvm::GlobalValue::PrivateLinkage,
                                                 llvm::ConstantAggregateZero::get(desc_ty),
                                                 "clotho.nulldesc");
        past          = llvm::ConstantExpr::getGetElementPtr(
          desc_ty,
          desc,
          std::array<llvm::Constant*, 3>{ b_.getInt32(0), b_.getInt32(desc_vtable_field()), b_.getInt32(0) });
    }
    return new llvm::GlobalVariable(
      env_->llmod, raw_ptr_ty(), /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, past, "clotho.nullobj");
}

// rt_gc_map::handles for a class: the byte offsets, FROM THE HANDLE (the body
// base, which is where getStructLayout measures from), of every class-handle
// word in the body.
//
// The flattened property table already carries every INHERITED property, deepest
// base first (lower/Classes.cpp, lower_class_type), so a derived class's map covers the base
// prefix with no special case and a base class gets its own map covering just
// that prefix.
std::vector<uint32_t> Backend::class_handle_offsets(Ref class_ref) {
    const Type& t = std::get<Type>(mod_.pool.get(class_ref));
    assert(t.kind == Type::Kind::Class && "a trace map describes a class body");
    auto*                 body = class_body_ty(class_ref);
    const auto*           bl   = env_->llmod.getDataLayout().getStructLayout(body);
    const uint32_t        hdr  = class_header_words(class_ref);
    std::vector<uint32_t> out;
    uint32_t              f = 0;
    for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
        collect_class_handles(fld.type, bl->getElementOffset(hdr + f), body->getElementType(hdr + f), out);
        ++f;
    }
    // Ascending, so the collector's scan is a forward walk and so the table is
    // stable across builds. The worklist pops LIFO, so it is not sorted by
    // construction.
    std::ranges::sort(out);
    assert(std::ranges::adjacent_find(out) == out.end() && "a handle word was listed twice");
    assert((out.empty() ||
            out.back() + 8 <= env_->llmod.getDataLayout().getTypeAllocSize(body)) &&
           "a handle offset landed outside the class body");
    return out;
}

// rt_gc_map::leaves for a class: the `{ byte offset, rt_elem_ops* }` pairs of the
// class's managed properties (string / dynamic array / queue / assoc / event),
// offsets FROM THE HANDLE exactly as class_handle_offsets measures them.
//
// This is how a container inside a class body is traced. `class C; int q[$];
// endclass` holding handles has no handle word anywhere in its own image, so
// clotho.classgc.<Ref>.handles is empty and the queue's elements are reachable
// ONLY through this table (docs/design/garbage-collection.md §3.3). A
// class-property container is deliberately NOT a container root: it is found from
// its owning object, so a container inside a dead object keeps nothing alive
// (Gc.h:242-244).
//
// Reuses collect_elem_leaves, the same walk that builds an aggregate ELEMENT's
// kind-4 leaf table, so a leaf's descriptor and offset are computed exactly once
// in the tree. That walk flattens nested arrays and structs, so no leaf is itself
// an aggregate and every offset is flat within the body.
//
// Gated per property on has_heap_leaf, which is the SAME predicate
// class_finalizer_fn selects on. That is what makes `nleaves > 0` imply a non-null
// `finalize`: a class with no has_heap_leaf property contributes no leaf and gets
// no finalizer, and any class that contributes one has at least one such property
// and therefore gets a finalizer. The collector asserts that implication, because
// dropping a dead object's start bit is what guarantees a live object's leaf bytes
// are still its own. An `event`-only class is precisely why the gate is per
// property and not per leaf: collect_elem_leaves would emit an event leaf, but
// has_heap_leaf is false for an event (TypeModel.h:179-203), so neither table
// gets an entry.
std::vector<llvm::Constant*> Backend::class_elem_leaves(Ref class_ref) {
    const Type& t = std::get<Type>(mod_.pool.get(class_ref));
    assert(t.kind == Type::Kind::Class && "a trace map describes a class body");
    const auto                   flds = mod_.pool.struct_fields(t.fields_id);
    auto*                        body = class_body_ty(class_ref);
    const auto*                  bl   = env_->llmod.getDataLayout().getStructLayout(body);
    const uint32_t               hdr  = class_header_words(class_ref);
    std::vector<llvm::Constant*> out;
    uint32_t                     f = 0;
    for (const Field& fld : flds) {
        const uint32_t idx = hdr + f++;
        if (!has_heap_leaf(fld.type)) {
            continue; // owns no heap: no leaf here, and no finalizer walk either
        }
        collect_elem_leaves(fld.type, bl->getElementOffset(idx),
                            body->getElementType(idx), out);
    }
    assert(f == flds.size() && "every property was visited exactly once");
    assert((out.empty() ||
            std::ranges::any_of(flds, [&](const Field& p) { return has_heap_leaf(p.type); })) &&
           "a traced leaf implies the has_heap_leaf property class_finalizer_fn needs");
    return out;
}

// The per-class finalizer, `clotho.classfini.<Ref>`: a void(ptr body) that tears
// down the class's managed properties (string / dynamic array / queue / assoc)
// through exactly the per-property emit_agg_deinit walk deinit_body_strings runs
// for a frame slot. Null when the class owns no managed property.
//
// This is what closes the second-order leak: emit_class_body_init initialises
// string and container properties on every `new` and, before the collector,
// nothing ever tore them down (docs/design/garbage-collection.md §0).
//
// It RUNS DURING A COLLECTION, from rt_gc_collect's finalize pass, so it must
// not allocate (the heap is mid-sweep and the mutator's bump range is not the
// collector's to move) and must not store a class handle anywhere (marking is
// already complete, so a handle published now would be missed and its object
// reclaimed underneath the store). Both hold by construction: every call below
// is a deinit, and a deinit only frees.
llvm::Constant* Backend::class_finalizer_fn(Ref class_ref) {
    const Type& t = std::get<Type>(mod_.pool.get(class_ref));
    const std::string nm = "clotho.classfini." + std::to_string(static_cast<uint32_t>(class_ref));
    if (auto* g = env_->llmod.getFunction(nm)) {
        return g;
    }
    const auto flds = mod_.pool.struct_fields(t.fields_id);
    const bool managed =
      std::ranges::any_of(flds, [&](const Field& f) { return has_heap_leaf(f.type); });
    if (!managed) {
        return llvm::Constant::getNullValue(raw_ptr_ty());
    }
    // Emitted lazily from a descriptor request, which can happen mid-body, so the
    // insert point is saved and restored exactly as event_delay_thunk does.
    auto* saved_bb = b_.GetInsertBlock();
    auto  saved_pt = b_.GetInsertPoint();
    auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { raw_ptr_ty() }, false);
    auto* fn   = llvm::Function::Create(fnty, llvm::Function::InternalLinkage, nm, env_->llmod);
    fn->addParamAttr(0, llvm::Attribute::NoAlias);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
    llvm::Type*    root_ty = class_body_ty(class_ref);
    llvm::Value*   body    = fn->getArg(0);
    const uint32_t hdr     = class_header_words(class_ref);
    uint32_t       f       = 0;
    for (const Field& fld : flds) {
        const uint32_t idx = hdr + f++;
        if (!has_heap_leaf(fld.type)) {
            continue; // a plane-only property owns nothing; emit no walk
        }
        std::vector<llvm::Value*> path{ b_.getInt32(idx) };
        emit_agg_deinit(fld.type, body, path, root_ty);
    }
    b_.CreateRetVoid();
    assert(f == flds.size() && "every property was visited exactly once");
    if (saved_bb != nullptr) {
        b_.SetInsertPoint(saved_bb, saved_pt);
    }
    return fn;
}

// `clotho.classgc.<Ref>`, the rt_gc_map constant the collector reaches from any
// handle in two loads (the vptr, then this pointer at gc_map_vptr_offset()).
// One per class per module, deduped by name exactly as the descriptor is.
//
// `leaves` is the real managed-property table since P4 (class_elem_leaves): a
// class handle CAN now be a container element, so a queue or associative array
// held in a class body is traced through it. It doubles as the finalization table
// Gc.h:81-85 anticipated -- same offsets, same descriptors.
llvm::Constant* Backend::class_gc_map_global(Ref class_ref) {
    const std::string nm = "clotho.classgc." + std::to_string(static_cast<uint32_t>(class_ref));
    if (auto* g = env_->llmod.getNamedGlobal(nm)) {
        return g;
    }
    const std::vector<uint32_t>        offsets = class_handle_offsets(class_ref);
    const std::vector<llvm::Constant*> leaves  = class_elem_leaves(class_ref);
    llvm::Constant* leaf_arr = llvm::Constant::getNullValue(raw_ptr_ty());
    if (!leaves.empty()) {
        auto* arr_ty = llvm::ArrayType::get(elem_leaf_ty(), leaves.size());
        leaf_arr     = new llvm::GlobalVariable(env_->llmod, arr_ty, /*isConstant=*/true,
                                                llvm::GlobalValue::PrivateLinkage,
                                                llvm::ConstantArray::get(arr_ty, leaves),
                                                nm + ".leaves");
    }
    assert((leaves.empty() == llvm::isa<llvm::ConstantPointerNull>(leaf_arr)) &&
           "a zero leaf count pairs with a null leaf pointer");
    const uint64_t size = gc_object_bytes(class_ref);
    auto* init = llvm::ConstantStruct::get(
      gc_map_ty(), { b_.getInt64(size), b_.getInt32(static_cast<uint32_t>(offsets.size())),
                     b_.getInt32(static_cast<uint32_t>(leaves.size())),
                     gc_offsets_global(offsets, nm + ".handles"), leaf_arr,
                     class_finalizer_fn(class_ref) });
    return new llvm::GlobalVariable(env_->llmod, gc_map_ty(), /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init, nm);
}

// rt_gc_map::size for a class: the header plus the body's alloc size, rounded up
// to the heap's object alignment (Gc.h:75-78). This is the byte count rt_gc_alloc
// bumps by, so the ClassNew site and the map constant MUST agree -- hence one
// helper feeding both rather than the same expression written twice.
uint64_t Backend::gc_object_bytes(Ref class_ref) {
    const uint64_t body_bytes =
      env_->llmod.getDataLayout().getTypeAllocSize(class_body_ty(class_ref));
    const uint64_t want = body_bytes + RT_GC_HEADER_BYTES;
    const uint64_t size = (want + RT_GC_ALIGN - 1) & ~(uint64_t{ RT_GC_ALIGN } - 1);
    assert(size >= want && "object size must cover header and body");
    assert(size % RT_GC_ALIGN == 0 && "object size must be heap-aligned");
    return size;
}

// Append every container (dynamic array / queue / associative array) inside
// `type`'s INLINE storage whose contents can reach a class handle, by byte offset
// from `base`.
//
// A container handle is a BOUNDARY: the walk never descends into its elements.
// A container is registered as one unit and its elements are described by its own
// rt_elem_ops (`handles` / `inner`), so registering an inner container separately
// would both trace it twice and name storage the OUTER container owns and
// reallocates -- a dangling entry the first time the outer ring grows.
//
// This is the shape-locating half of both root paths: the state path turns these
// offsets into a constant rt_gc_container table, the frame path turns them into
// rt_gc_container_add / _remove calls. One walk feeding both is what makes the
// add and the remove agree about which containers exist -- an unpaired add is a
// stale entry the collector later dereferences.
//
// Unpacked array dimensions UNROLL, exactly as collect_class_handles unrolls them
// for the precise root table: one entry per element, bounded by the declared
// dimension.
//
// Explicit worklist, never recursion (TigerStyle): this is precisely where
// nesting gets deep -- a struct of arrays of structs holding a queue of dynamic
// arrays of handles.
void Backend::collect_gc_containers(Ref type, uint64_t base, llvm::Type* storage, std::vector<GcContainer>& out) {
    assert(storage != nullptr && "a storage mirror is required to read offsets");
    // Every step pops one node of a finite storage tree; the cap turns a malformed
    // type graph into a loud crash rather than a hang (TigerStyle).
    [[maybe_unused]] static constexpr uint32_t kWalkStepsMax = 1u << 24;
    struct Node {
        Ref         type;
        uint64_t    base;
        llvm::Type* storage;
    };
    const llvm::DataLayout&   dl = env_->llmod.getDataLayout();
    std::vector<Node>         work{ { type, base, storage } };
    [[maybe_unused]] uint32_t steps = 0;
    while (!work.empty()) {
        ++steps;
        assert(steps < kWalkStepsMax && "container storage walk did not converge");
        const Node n = work.back();
        work.pop_back();
        const Type& t = std::get<Type>(mod_.pool.get(n.type));
        if (types::is_container(mod_.pool, n.type)) {
            if (reaches_class_handle(n.type)) {
                out.push_back({ n.base, n.type });
            }
            continue; // a boundary either way: never descend into its elements
        }
        if (t.kind == Type::Kind::UnpackedArray) {
            auto*          arr    = llvm::cast<llvm::ArrayType>(n.storage);
            llvm::Type*    et     = arr->getElementType();
            const uint64_t stride = dl.getTypeAllocSize(et);
            for (uint64_t k = 0; k < arr->getNumElements(); ++k) {
                work.push_back({ t.element, n.base + k * stride, et });
            }
            continue;
        }
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            auto*       st = llvm::cast<llvm::StructType>(n.storage);
            const auto* sl = dl.getStructLayout(st);
            uint32_t    f  = 0;
            for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
                work.push_back({ fld.type, n.base + sl->getElementOffset(f),
                                 st->getElementType(f) });
                ++f;
            }
        }
        // Anything else -- a scalar, a real, a string, an event, a class handle --
        // is not a container. A class handle here is a precise root the handle
        // tables already cover, and is none of this walk's business.
    }
}

// The rt_elem_kind (Runtime.h:898-907) of a container type: rt_elem_dynarr,
// rt_elem_queue or rt_elem_assoc. Feeds BOTH the `kind` of an rt_gc_container and
// the `kind` of that container's own rt_elem_ops descriptor, so the equality the
// collector asserts between them holds by construction rather than by two
// switches agreeing.
uint32_t Backend::gc_container_kind(Ref container_type) {
    const Type& t = std::get<Type>(mod_.pool.get(container_type));
    assert(types::is_container(mod_.pool, container_type) &&
           "only a container has an rt_elem container kind");
    if (t.kind == Type::Kind::DynArray) {
        return 2;
    }
    if (t.kind == Type::Kind::Queue) {
        return 3;
    }
    assert(t.kind == Type::Kind::Assoc && "a container is a dynarr, queue or assoc");
    return 5;
}

void Backend::emit_gc_container_roots(GcRootOp op, Ref type, llvm::Value* base, llvm::Type* storage) {
    assert(base != nullptr && "a container root needs its storage address");
    std::vector<GcContainer> found;
    collect_gc_containers(type, 0, storage, found);
    for (const GcContainer& c : found) {
        llvm::Value* ptr = c.offset == 0
                             ? base
                             : b_.CreateConstInBoundsGEP1_64(b_.getInt8Ty(), base, c.offset);
        if (op == GcRootOp::Remove) {
            b_.CreateCall(gc_container_remove_fn(), { ptr });
            continue;
        }
        b_.CreateCall(gc_container_add_fn(),
                      { ptr, b_.getInt32(gc_container_kind(c.type)), elem_ops_for(c.type) });
    }
}

// One rt_gc_roots table per storage region that can hold a class handle, each
// registered once at startup so the runtime borrows a table whose lifetime is
// the JIT'd module's. The regions are the root design state buffer
// (@clotho.state) and each per-instance @clotho.inst.<hier>.state.
//
// STATIC CLASS PROPERTIES need no region of their own: a static property is not
// part of any object body, it gets persistent module-signal storage through the
// ordinary register_signal path (lower/Types.cpp), so it is already a
// SignalSlot in the list walked here -- covered by the same tables, and a
// missed root would be a use-after-free.
//
// The slot list is the one emit_main's teardown loop walks, filtered to the
// slots whose IR type carries a class handle.
//
// The same walk also emits the STATE-RESIDENT half of the container roots
// (clotho.gccontainers.<n>): a signal that is, or contains, a dynamic array /
// queue / associative array whose contents can reach a class handle. Those live in
// the same regions and never move, so they are a constant table registered once
// beside the handle offsets rather than an add / remove pair
// (docs/design/garbage-collection.md §3.3). Static class properties are ordinary
// SignalSlots, as above, so `static Cell q[$]` is covered here too.
void Backend::emit_gc_root_tables() {
    const llvm::DataLayout& dl = env_->llmod.getDataLayout();
    // Anchor index -> offsets, with the root state buffer in the last bucket:
    // a slot names its region by SignalSlot::anchor, no_anchor meaning the root.
    std::vector<std::vector<uint32_t>>    by_region(anchors_.size() + 1);
    std::vector<std::vector<GcContainer>> cont_by_region(anchors_.size() + 1);
    const size_t root_region = anchors_.size();
    for (const SignalSlot& s : slots_) {
        const size_t region = s.anchor == SignalSlot::no_anchor ? root_region : s.anchor;
        assert(region < by_region.size() && "a slot named a region that does not exist");
        llvm::Type* owner = region == root_region ? state_ty_ : anchors_[s.anchor].ty;
        std::vector<llvm::Value*> idx{ b_.getInt32(0) };
        for (const uint32_t f : s.path) {
            idx.push_back(b_.getInt32(f));
        }
        const int64_t off = dl.getIndexedOffsetInType(owner, idx);
        assert(off >= 0 && "a signal cannot sit before its own state buffer");
        signal_handle_offsets(s, static_cast<uint64_t>(off), by_region[region]);
        signal_gc_containers(s, static_cast<uint64_t>(off), cont_by_region[region]);
    }
    uint32_t emitted = 0;
    for (size_t r = 0; r < by_region.size(); ++r) {
        if (by_region[r].empty()) {
            continue; // a region with no class handle needs no table
        }
        llvm::Constant* base =
          r == root_region ? sym(state_global_) : sym(anchors_[r].state_gv);
        b_.CreateCall(gc_roots_add_fn(), { gc_roots_global(base, by_region[r], emitted++) });
    }
    assert(emitted <= by_region.size() && "one table per region at most");
    uint32_t cont_emitted = 0;
    for (size_t r = 0; r < cont_by_region.size(); ++r) {
        if (cont_by_region[r].empty()) {
            continue; // a region with no handle-bearing container needs no table
        }
        llvm::Constant* base =
          r == root_region ? sym(state_global_) : sym(anchors_[r].state_gv);
        b_.CreateCall(gc_containers_add_fn(),
                      { gc_containers_global(base, cont_by_region[r], cont_emitted++) });
    }
    assert(cont_emitted <= cont_by_region.size() && "one container table per region at most");
}

// The byte offsets of every class-handle word inside one signal's storage,
// appended relative to `base` (that storage's offset in its region).
void Backend::signal_handle_offsets(const SignalSlot& s, uint64_t base, std::vector<uint32_t>& out) {
    if (s.is_agg) {
        // An aggregate slot's storage_ty IS the recursive agg_storage_ty mirror,
        // the exact shape collect_class_handles walks.
        collect_class_handles(s.ir_type, base, s.storage_ty, out);
        return;
    }
    // A memory instead FLATTENS every dimension into one [elem_count x slot_ty]
    // (SignalSlot::is_array), so the nested IR type cannot be walked against it.
    // Take the leaf type and the flat stride directly.
    // Type nesting is a DAG and an unpacked array's element is strictly smaller,
    // so the descent terminates; the cap makes a cyclic type Ref a loud crash
    // rather than a hang (TigerStyle).
    [[maybe_unused]] static constexpr uint32_t kArrayDepthMax = 64;
    Ref                       leaf           = s.ir_type;
    [[maybe_unused]] uint32_t depth          = 0;
    while (std::get<Type>(mod_.pool.get(leaf)).kind == Type::Kind::UnpackedArray) {
        ++depth;
        assert(depth < kArrayDepthMax && "unpacked array nesting did not bottom out");
        leaf = std::get<Type>(mod_.pool.get(leaf)).element;
    }
    if (std::get<Type>(mod_.pool.get(leaf)).kind != Type::Kind::Class) {
        return;
    }
    assert(s.slot_ty != nullptr && "a class-handle slot has a leaf slot type");
    const uint64_t stride = env_->llmod.getDataLayout().getTypeAllocSize(s.slot_ty);
    const uint32_t n      = s.is_array ? s.elem_count : 1u;
    for (uint32_t k = 0; k < n; ++k) {
        out.push_back(static_cast<uint32_t>(base + k * stride));
    }
}

// The handle-bearing containers inside one signal's storage, relative to `base`.
// The container-root counterpart of signal_handle_offsets, and the STATE half of
// the distinction in the container-roots section above.
void Backend::signal_gc_containers(const SignalSlot& s, uint64_t base, std::vector<GcContainer>& out) {
    assert(s.ir_type != Ref::Invalid && "every slot carries its interned IR type");
    if (s.is_agg) {
        // An aggregate slot's storage_ty IS the recursive agg_storage_ty mirror,
        // the exact shape collect_gc_containers walks. An unpacked array OVER a
        // container element takes this path too, because needs_agg_storage sends
        // any such array to the recursive mirror (TypeModel.h:168-174) -- so the
        // flattened-memory case below can never hold a container.
        collect_gc_containers(s.ir_type, base, s.storage_ty, out);
        return;
    }
    if (!types::is_container(mod_.pool, s.ir_type)) {
        return; // a plane, string or memory slot owns no container
    }
    assert((s.is_dynarr || s.is_queue || s.is_assoc) &&
           "a container-typed slot is classified as one");
    if (reaches_class_handle(s.ir_type)) {
        out.push_back({ base, s.ir_type });
    }
}

// `clotho.gcroots.<n>`: an rt_gc_roots constant plus its [N x i32] offset array.
llvm::Constant* Backend::gc_roots_global(llvm::Constant* base, const std::vector<uint32_t>& offsets, uint32_t index) {
    assert(!offsets.empty() && "an empty root table would register nothing");
    const std::string nm  = "clotho.gcroots." + std::to_string(index);
    llvm::Constant*   arr = gc_offsets_global(offsets, nm + ".offsets");
    auto*             init = llvm::ConstantStruct::get(
      gc_roots_ty(),
      { base, b_.getInt32(static_cast<uint32_t>(offsets.size())), b_.getInt32(0), arr });
    return new llvm::GlobalVariable(env_->llmod, gc_roots_ty(), /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init, nm);
}

// `clotho.gccontainers.<n>`: an rt_gc_container_table constant plus its
// [N x rt_gc_container] entry array. `ops` is each container's OWN descriptor, so
// its kind matches the entry's kind (gc_container_kind feeds both).
llvm::Constant* Backend::gc_containers_global(llvm::Constant*                 base,
                                              const std::vector<GcContainer>& entries,
                                              uint32_t                        index) {
    assert(!entries.empty() && "an empty container table would register nothing");
    const std::string            nm = "clotho.gccontainers." + std::to_string(index);
    std::vector<llvm::Constant*> rows;
    rows.reserve(entries.size());
    for (const GcContainer& c : entries) {
        rows.push_back(llvm::ConstantStruct::get(
          gc_container_ty(), { b_.getInt64(c.offset), b_.getInt32(gc_container_kind(c.type)),
                               b_.getInt32(0), elem_ops_for(c.type) }));
    }
    assert(rows.size() == entries.size() && "every container became one entry");
    auto* arr_ty = llvm::ArrayType::get(gc_container_ty(), rows.size());
    auto* arr    = new llvm::GlobalVariable(env_->llmod, arr_ty, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arr_ty, rows),
                                            nm + ".entries");
    auto* init   = llvm::ConstantStruct::get(
      gc_container_table_ty(),
      { base, b_.getInt32(static_cast<uint32_t>(rows.size())), b_.getInt32(0), arr });
    return new llvm::GlobalVariable(env_->llmod, gc_container_table_ty(), /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init, nm);
}

// `clotho.gc.config`: the rt_gc_config rt_gc_init reads. Every field is an
// emit-time constant, so the collector's knobs are baked into the module rather
// than armed at run start -- one lowered module belongs to exactly one collector
// configuration.
llvm::Constant* Backend::gc_config_global() {
    if (auto* g = env_->llmod.getNamedGlobal("clotho.gc.config")) {
        return g;
    }
    auto* init = llvm::ConstantStruct::get(gc_config_ty(),
                                           { b_.getInt64(config_.gc_nursery_bytes),
                                             b_.getInt64(config_.gc_heap_bytes_max),
                                             b_.getInt32(config_.gc_stress ? 1u : 0u),
                                             b_.getInt32(config_.gc_stats ? 1u : 0u),
                                             b_.getInt32(gc_map_vptr_offset()),
                                             b_.getInt32(gc_mutator_request_offset()),
                                             b_.getInt32(static_cast<uint32_t>(gc_mutator_bytes())),
                                             // 0 (the --gc-full-every default) hands the young/full split to the
                                             // runtime's heap-growth heuristic (Gc.h:241-243). `reserved` pads the
                                             // struct to a multiple of 8 explicitly rather than letting the two
                                             // layouts disagree about tail padding.
                                             b_.getInt32(config_.gc_full_every),
                                             b_.getInt32(0) });
    return new llvm::GlobalVariable(env_->llmod,
                                    gc_config_ty(),
                                    /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage,
                                    init,
                                    "clotho.gc.config");
}

// Register this coroutine's frame with the collector, at the ramp, immediately
// after coro.begin. A suspended SV process has no stack at all: its live handles
// are in a heap frame whose layout CoroSplit chooses and we cannot describe, so
// the frame is scanned conservatively and every hit validated and pinned.
//
// The node MUST be an alloca placed AFTER coro.begin. CoroSplit then hoists it
// into the heap frame and rewrites every use to a stable frame-interior address,
// which is precisely what makes the registry keep working while the process is
// suspended. An alloca before coro.begin does not get that treatment.
//
// The region is `hdl_` for coro.size bytes, NOT size+align. The ramp
// over-allocates by coro.align so coro.begin may realign the pointer, so the
// bytes certainly ours start at hdl_ and run for coro.size; scanning size+align
// from hdl_ would read up to align bytes past the end of the allocation on any
// frame coro.begin actually moved.
//
// INLINED since P2 (design §6.4). `frames_head` lives in the mutator
// (Gc.h:177-181) rather than in a runtime file-static, so the whole registration
// is four stores into the node plus a head swap: pointer surgery, not a call.
// This was one of the two per-event costs the collector charged designs that
// never allocate -- rt_gc_frame_link AND rt_gc_frame_unlink ran on every single
// process activation, which is what cost wait_bench.v +10.3% simulate time.
//
// BRANCHLESS, deliberately. `prev` is field 0 of rt_gc_frame, so &head->prev ==
// head, and when the registry is empty the "fix the old head's prev" store can be
// aimed at &mutator->frames_head instead -- where it writes exactly the value the
// following head store writes anyway, making the pair idempotent rather than
// conditional. That keeps the ramp's CFG free of an extra block: splitting the
// block that holds coro.begin to host a null test would be a change to coroutine
// machinery, and this needs none.
void Backend::emit_gc_frame_link(llvm::Value* frame_bytes) {
    assert(frame_bytes != nullptr && "the frame byte count must reach coro.begin");
    // The redirect below is silently wrong if `prev` ever stops being field 0, so
    // the identity is asserted from the DataLayout and not merely trusted.
    assert(gc_frame_off(kGcFramePrev) == 0 &&
           "rt_gc_frame::prev is field 0, so a node's address is its prev slot");
    llvm::StructType* nty  = gc_frame_ty();
    llvm::Constant*   null = llvm::Constant::getNullValue(raw_ptr_ty());
    gc_frame_node_         = b_.CreateAlloca(nty, nullptr, "gc_frame");
    llvm::Value* node      = gc_frame_node_;
    llvm::Value* headp     = gc_mut_ptr(GcMutField::FramesHead, "gc.headp");
    llvm::Value* head      = b_.CreateLoad(raw_ptr_ty(), headp, "gc.head");
    b_.CreateStore(null, b_.CreateStructGEP(nty, node, kGcFramePrev));
    b_.CreateStore(head, b_.CreateStructGEP(nty, node, kGcFrameNext));
    b_.CreateStore(hdl_, b_.CreateStructGEP(nty, node, kGcFrameBase));
    b_.CreateStore(frame_bytes, b_.CreateStructGEP(nty, node, kGcFrameBytes));
    llvm::Value* empty = b_.CreateICmpEQ(head, null, "gc.head.null");
    llvm::Value* oldp  = b_.CreateSelect(empty, headp, head, "gc.oldprevp");
    b_.CreateStore(node, oldp);
    b_.CreateStore(node, headp);
}

// Unlink before the frame dies. Emitted at the top of coro.cleanup, which is
// inside the coroutine (so the node alloca is addressable) and dominates the
// coro.free -> rt_free block, so every frame free is preceded by an unlink.
//
// Cleanup rather than the free block itself, because cleanup also covers the
// coro-elided case: coro.free returns null, no rt_free happens, and the frame
// still dies with the caller's activation. A node left linked past its frame is
// a use-after-free inside the collector (Gc.h:116-118).
//
// Inlined and branchless on the same terms as emit_gc_frame_link, with both
// redirects aimed at a slot whose write is a no-op:
//   next->prev = prev -- when `next` is null this node is the tail, so the store
//       is aimed at the node's OWN prev slot (&node->prev == node), which already
//       holds `prev`: a self-store of the value just loaded.
//   prev->next = next -- when `prev` is null this node IS the head, so the store
//       is aimed at &mutator->frames_head, which is exactly the head fixup the
//       registry needs in that case.
//
// The node's own prev/next are left as they are. rt_gc_frame_unlink nulled them
// to keep its O(1) "is this node still linked" assertion meaningful across a
// second call (Gc.cpp:1764-1785); inline there is no second call -- one unlink per
// ramp, in the block that dominates every free -- and the frame's bytes are
// released immediately after, with no safepoint in between at which the collector
// could walk the registry and observe them.
void Backend::emit_gc_frame_unlink() {
    assert(gc_frame_node_ != nullptr && "every linked frame must unlink exactly once");
    assert(gc_frame_off(kGcFramePrev) == 0 &&
           "rt_gc_frame::prev is field 0, so a node's address is its prev slot");
    llvm::StructType* nty  = gc_frame_ty();
    llvm::Constant*   null = llvm::Constant::getNullValue(raw_ptr_ty());
    llvm::Value*      node = gc_frame_node_;
    llvm::Value* prev  = b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(nty, node, kGcFramePrev),
                                      "gc.prev");
    llvm::Value* next  = b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(nty, node, kGcFrameNext),
                                      "gc.next");
    llvm::Value* headp = gc_mut_ptr(GcMutField::FramesHead, "gc.headp");
    llvm::Value* tail  = b_.CreateICmpEQ(next, null, "gc.tail");
    b_.CreateStore(prev, b_.CreateSelect(tail, node, next, "gc.nextprevp"));
    // A plain, NOT inbounds, GEP: on the head path this address is formed from a
    // null `prev` and then discarded by the select, and an inbounds GEP off null
    // is poison rather than merely unused.
    llvm::Value* pnextp =
      b_.CreateConstGEP1_32(b_.getInt8Ty(), prev, gc_frame_off(kGcFrameNext), "gc.prevnextp");
    llvm::Value* was_head = b_.CreateICmpEQ(prev, null, "gc.washead");
    b_.CreateStore(next, b_.CreateSelect(was_head, headp, pnextp, "gc.prevnextp.sel"));
}

// Whether `new` for this class allocates through the inlined bump sequence.
//
// A class at or above the runtime's large-object threshold keeps the out-of-line
// rt_gc_alloc: such an object never comes out of a block hole at all, it owns a
// separately-allocated cell with no start bit and no line marks (Gc.cpp:192-195,
// 556-560), so there is no bump range to bump and nothing the inline sequence
// could do with one. RT_GC_LARGE_OBJECT_BYTES is the runtime's own macro
// (Gc.h:52) and Gc.cpp's kLargeObjectBytes is defined from it, so the threshold
// cannot drift between the two files. The comparison is `<=`, matching
// rt_gc_alloc's own `size <= kLargeObjectBytes` (Gc.cpp:1697) exactly, so a class
// of precisely threshold size is block-resident on both paths rather than on
// whichever one happened to round differently.
bool Backend::gc_alloc_is_inline(Ref class_ref) {
    const uint64_t size = gc_object_bytes(class_ref);
    assert(size > RT_GC_HEADER_BYTES && "an object is a header plus a body");
    assert(size % RT_GC_ALIGN == 0 && "an object size is grain-aligned");
    return size <= RT_GC_LARGE_OBJECT_BYTES;
}

// `bitmap[byte] |= bit`, for one of the two bitmaps the mutator caches.
//
// OR, never a plain store: a bitmap byte covers eight grains and this object owns
// exactly one of them, so assigning would clear up to seven neighbours' bits. The
// runtime asserts against an OR for the same reason (Gc.cpp:544).
void Backend::emit_gc_bitmap_set(GcMutField f, llvm::Value* byi, llvm::Value* bit, const char* nm) {
    assert(byi != nullptr && bit != nullptr && "a bitmap set needs both a byte and a bit");
    assert((f == GcMutField::StartBitmap || f == GcMutField::FinBitmap) &&
           "only the start and finalizable bitmaps are set inline");
    llvm::Value* bmpp = gc_mut_ptr(f, "gc.bmpp");
    llvm::Value* bmp  = b_.CreateLoad(raw_ptr_ty(), bmpp, "gc.bitmap");
    llvm::Value* slot = b_.CreateInBoundsGEP(b_.getInt8Ty(), bmp, byi, nm);
    llvm::Value* old  = b_.CreateLoad(b_.getInt8Ty(), slot);
    b_.CreateStore(b_.CreateOr(old, bit), slot);
}

// The object-start bit, plus the finalizable bit when this class has a finalizer.
//
// Both bitmaps are indexed identically and `addr` is the OBJECT START -- the
// gcword, one word below the handle -- not the handle: the bit index is
// `((addr - block_base) >> 4)`, the byte is `index >> 3` and the bit is
// `index & 7` (Gc.h:169-176). block_base, start_bitmap and fin_bitmap are
// published together with cursor and limit and always describe the SAME block, so
// a fast-path allocation cannot index another block's metadata.
//
// Neither bit is optional bookkeeping, which is why this is emitted before the
// handle can escape:
//   start -- an object whose start bit is clear is INVISIBLE to
//       rt_gc_find_object, which is both the conservative-scan validator and the
//       interior-pointer resolver (Gc.h:271-276). A live object would be missed by
//       a frame scan, and a `ref` into one of its properties would not resolve.
//   fin   -- lose it and the class's finalizer never runs, so every `string` and
//       container buffer the body owns leaks. It is set from the same
//       compile-time predicate that decides the gcword's FINALIZE bit, so the two
//       cannot disagree.
void Backend::emit_gc_start_bits(llvm::Value* start, bool fin) {
    assert(start != nullptr && "an object start is needed to index the bitmaps");
    constexpr unsigned kGrainShift = 4;  // log2(RT_GC_ALIGN)
    constexpr unsigned kByteShift  = 3;  // log2(bits per bitmap byte)
    static_assert((1u << kGrainShift) == RT_GC_ALIGN, "the grain shift is log2(RT_GC_ALIGN)");
    static_assert((1u << kByteShift) == 8u, "a bitmap byte holds eight grains");
    llvm::Value* basep = gc_mut_ptr(GcMutField::BlockBase, "gc.basep");
    llvm::Value* base  = b_.CreateLoad(raw_ptr_ty(), basep, "gc.blockbase");
    llvm::Value* delta = b_.CreateSub(b_.CreatePtrToInt(start, b_.getInt64Ty()),
                                      b_.CreatePtrToInt(base, b_.getInt64Ty()), "gc.delta");
    llvm::Value* idx = b_.CreateLShr(delta, kGrainShift, "gc.grain");
    llvm::Value* byi = b_.CreateLShr(idx, kByteShift, "gc.bitbyte");
    llvm::Value* sh  = b_.CreateAnd(b_.CreateTrunc(idx, b_.getInt8Ty()),
                                    b_.getInt8((1u << kByteShift) - 1u), "gc.bitsh");
    llvm::Value* bit = b_.CreateShl(b_.getInt8(1), sh, "gc.bit");
    emit_gc_bitmap_set(GcMutField::StartBitmap, byi, bit, "gc.startp");
    if (fin) {
        emit_gc_bitmap_set(GcMutField::FinBitmap, byi, bit, "gc.finp");
    }
}

// `new`, inlined: bump the mutator's cursor within its current hole and call out
// only when the hole cannot satisfy the request (design §4.3). Returns the HANDLE,
// exactly as rt_gc_alloc does, leaving the insert point on the join block.
//
// The two paths produce one handle value and identical observable state: the fast
// block does by hand precisely what rt_gc_alloc_slow does in the runtime -- set the
// start bit, set the finalizable bit when the class has a finalizer, write the
// gcword, zero the body -- so the phi's two inputs are indistinguishable to
// everything downstream, including emit_class_body_init and the vptr store the
// caller emits once, after the join.
//
// The nursery budget is deliberately absent. rt_gc_alloc_slow charges it per HOLE
// handed out rather than per object (Gc.h:243-253), which is what keeps this
// sequence free of budget arithmetic while leaving the accounting exact in bytes
// made available.
llvm::Value* Backend::emit_gc_alloc_fast(Ref class_ref) {
    assert(gc_alloc_is_inline(class_ref) && "a large object keeps the out-of-line call");
    const uint64_t  size = gc_object_bytes(class_ref);
    llvm::Constant* map  = class_gc_map_global(class_ref);
    // FINALIZE is the only header bit `new` ever sets, and whether it is set is a
    // compile-time property of the class. Mark bits 0-1 stay 0, which is "never
    // marked" and is what makes the next collection trace the object (Gc.h:47-52);
    // PINNED, HAS_ID and FORWARDED are the collector's own and start clear; the
    // payload is empty until an object id or a forwarding address lands in it.
    // This is the same word rt_gc_alloc writes (Gc.cpp:1704), and the bit MUST
    // agree with `map->finalize` being non-null because run_finalizers() asserts
    // the two agree (Gc.cpp:1297, 1337).
    const bool     fin  = !class_finalizer_fn(class_ref)->isNullValue();
    const uint64_t word = fin ? static_cast<uint64_t>(RT_GC_FLAG_FINALIZE) : 0ull;

    llvm::Function* fn   = b_.GetInsertBlock()->getParent();
    auto*           fast = llvm::BasicBlock::Create(ctx_, "gc.alloc.fast", fn);
    auto*           slow = llvm::BasicBlock::Create(ctx_, "gc.alloc.slow", fn);
    auto*           cont = llvm::BasicBlock::Create(ctx_, "gc.alloc.cont", fn);

    llvm::Value* curp = gc_mut_ptr(GcMutField::Cursor, "gc.curp");
    llvm::Value* limp = gc_mut_ptr(GcMutField::Limit, "gc.limp");
    llvm::Value* cur  = b_.CreateLoad(raw_ptr_ty(), curp, "gc.cur");
    llvm::Value* lim  = b_.CreateLoad(raw_ptr_ty(), limp, "gc.lim");
    // NOT inbounds, deliberately. `cur + size` is exactly the address whose
    // legality is in question: when the hole is too small it runs past the region,
    // and an inbounds GEP would make it poison, so the icmp below would be poison
    // and the branch on it undefined behaviour. The flag would buy nothing here --
    // the pointer comes from a load of runtime-owned storage LLVM knows no bounds
    // for -- and costs the same instruction either way.
    llvm::Value* next = b_.CreateConstGEP1_64(b_.getInt8Ty(), cur, size, "gc.next");
    // Unsigned, and `ule` not `ult`: an object that exactly consumes the hole's
    // last byte is a legal allocation. The no-hole state needs no separate test --
    // cursor and limit are published together and are both null then, so the
    // compare fails and the slow path refills (Gc.h:169-176).
    llvm::Value*      fits = b_.CreateICmpULE(next, lim, "gc.fits");
    llvm::BranchInst* br   = b_.CreateCondBr(fits, fast, slow);
    br->setMetadata(llvm::LLVMContext::MD_prof,
                    llvm::MDBuilder(ctx_).createBranchWeights(1u << 20, 1));

    b_.SetInsertPoint(fast);
    b_.CreateStore(next, curp);
    emit_gc_start_bits(cur, fin);
    b_.CreateStore(b_.getInt64(word), cur);
    llvm::Value* hdl =
      b_.CreateConstInBoundsGEP1_32(b_.getInt8Ty(), cur, RT_GC_HEADER_BYTES, "gc.hdl");
    // The body, from the handle to the end of the object: exactly the bytes
    // rt_gc_alloc zeroes (Gc.cpp:1707), padding included. 2-state leaves and handle
    // properties power on 0 / null (LRM Table 7-1) and these bytes are recycled
    // heap, so this is required rather than merely tidy. A constant size lowers to
    // stores. The handle is RT_GC_HEADER_BYTES past a grain boundary, which is the
    // alignment gcword_ptr asserts (Gc.cpp:319) and all that may be claimed here.
    b_.CreateMemSet(hdl, b_.getInt8(0), b_.getInt64(size - RT_GC_HEADER_BYTES),
                    llvm::MaybeAlign(RT_GC_HEADER_BYTES));
    b_.CreateBr(cont);

    b_.SetInsertPoint(slow);
    llvm::Value* slow_hdl =
      b_.CreateCall(gc_alloc_slow_fn(), { gc_mutator_cur(), b_.getInt64(size), map },
                    "gc.slowhdl");
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    llvm::PHINode* out = b_.CreatePHI(raw_ptr_ty(), 2, "gc.obj");
    out->addIncoming(hdl, fast);
    out->addIncoming(slow_hdl, slow);
    return out;
}

// The mutator context pointer for the function being emitted, materialized once
// and shared by every site in it that touches the mutator: the inlined safepoint
// poll, the inlined allocation fast path, and the frame registry link/unlink.
// rt_gc_mutator_current() is loop-invariant -- one mutator per thread for the
// whole run (Gc.h:152-158) -- but it is an opaque external call, so nothing
// downstream may hoist it: a call per site would be a call per wheel advance and
// a call per `new`, the two costs P2 exists to remove.
//
// WHERE it is materialized depends on whether the function is a coroutine, and
// that is the whole subtlety of this helper:
//
//   plain function -- the entry block's first insertion point, which dominates
//       every block, so the cached value is legal at any later insert point.
//   presplit coroutine -- immediately after coro.begin. The entry block is NOT
//       available: a call placed there would precede coro.id, and its result would
//       be live across a suspend while the frame that would have to spill it does
//       not exist yet. coro.begin dominates the body, coro_cleanup_bb_ and the
//       final suspend, so it is both the earliest legal anchor and a dominating
//       one. CoroSplit then spills the pointer into the frame exactly as it does
//       the registry node and the awaiter -- which is the shape design §4.3
//       already names for MT ("a field seeded into the coroutine frame at
//       resume", garbage-collection.md:434-442). One mutator today means the
//       spilled value is the same pointer for the whole run (Gc.cpp:1688 asserts
//       it), and an MT scheduler reseeds that field at resume rather than
//       re-architecting this.
//
// The cache is keyed on the llvm::Function rather than cleared by each emitter,
// because an SSA value may not cross a function boundary and a forgotten reset
// would be a verifier failure in whichever body allocated next.
llvm::Value* Backend::gc_mutator_cur() {
    llvm::Function* fn = b_.GetInsertBlock()->getParent();
    assert(fn != nullptr && "a mutator access is emitted into some function");
    if (gc_mutator_cur_ != nullptr &&
        llvm::cast<llvm::Instruction>(gc_mutator_cur_)->getFunction() == fn) {
        return gc_mutator_cur_;
    }
    llvm::BasicBlock* saved_bb = b_.GetInsertBlock();
    auto              saved_pt = b_.GetInsertPoint();
    if (fn->isPresplitCoroutine()) {
        assert(coro_begin_ != nullptr && coro_begin_->getFunction() == fn &&
               "a coroutine anchors its mutator pointer on its own coro.begin");
        b_.SetInsertPoint(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
    } else {
        llvm::BasicBlock& entry = fn->getEntryBlock();
        b_.SetInsertPoint(&entry, entry.getFirstInsertionPt());
    }
    gc_mutator_cur_ = b_.CreateCall(gc_mutator_fn(), {}, "gc.mut");
    b_.SetInsertPoint(saved_bb, saved_pt);
    return gc_mutator_cur_;
}

// A safepoint, inlined: load rt_gc_mutator::request and call the out-of-line
// half only when it is set. rt_gc_safepoint_slow collects unconditionally --
// reaching it MEANS a collection is due (Gc.h:235-240) -- so this branch is the
// entire poll, and it is weighted cold exactly the way emit_null_handle_check
// weights its trap edge, leaving the quiescent path straight-line: a load, a
// test and a not-taken branch.
//
// Stress mode needs nothing extra here: rt_gc_init arms `request` and every
// collection re-arms it, so this same poll fires at every safepoint
// (Gc.h:144-149).
void Backend::emit_gc_safepoint() {
    llvm::Function* fn = b_.GetInsertBlock()->getParent();
    // Every poll site is scheduler-level (emit_active_run, lowered into
    // sim_drain), never inside a body. gc_mutator_cur() serves coroutines too now
    // that the allocator inlines its fast path, so this invariant is asserted
    // here, where it is a property of the caller, rather than there.
    assert(fn != nullptr && !fn->isPresplitCoroutine() &&
           "a poll site lives in a plain function");
    auto*        collect = llvm::BasicBlock::Create(ctx_, "gc.collect", fn);
    auto*        done    = llvm::BasicBlock::Create(ctx_, "gc.done", fn);
    llvm::Value* slot    = gc_mut_ptr(GcMutField::Request, "gc.reqp");
    llvm::Value*      req = b_.CreateLoad(b_.getInt32Ty(), slot, "gc.request");
    llvm::Value*      due = b_.CreateICmpNE(req, b_.getInt32(0), "gc.due");
    llvm::BranchInst* br  = b_.CreateCondBr(due, collect, done);
    br->setMetadata(llvm::LLVMContext::MD_prof,
                    llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

    b_.SetInsertPoint(collect);
    b_.CreateCall(gc_safepoint_slow_fn(), {});
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
}

// The generational write barrier, inlined (docs/design/garbage-collection.md §7).
// `owner` is the class object that has just acquired a reference, as the body
// pointer -- the same value HandleDeref already built, so the barrier adds no
// inttoptr of its own.
//
// Emitted at exactly the stores a YOUNG collection does not scan: a young
// collection treats an OLD object as live-and-opaque instead of tracing it, so a
// handle written into an OLD object -- or into a container the OLD object owns --
// is invisible, and the object it names would be reclaimed while still referenced.
// Design state, statics and frame locals need nothing: they are scanned in full at
// every collection, young or not (Gc.h:105-107). LValue::gc_owner is what decides
// which stores those are, and it is non-null only for a place inside an object.
//
// One mask and one compare rather than two tests: OLD says the young collection
// will not trace this object and LOGGED says the remembered set already holds it,
// so the exact value OLD-without-LOGGED is the only case that needs the call. The
// branch is weighted cold the way emit_gc_safepoint and emit_null_handle_check
// weight theirs, which sinks the call out of line and leaves four instructions --
// a GEP, a load, an and/compare pair folded by the backend, and a not-taken
// branch -- on the path a young object or an already-logged object takes.
void Backend::emit_gc_barrier(llvm::Value* owner) {
    assert(owner != nullptr && "a barrier needs the object acquiring the reference");
    llvm::Function* fn = b_.GetInsertBlock()->getParent();
    assert(fn != nullptr && "a barrier is emitted into some function");
    // The object body as a pointer, which is what LValue::gc_owner always holds
    // (HandleDeref's own inttoptr, reused rather than repeated). A caller holding
    // only the i64 handle bits converts once at its own site.
    assert(owner->getType()->isPointerTy() && "an owner is a class body pointer");
    // The GC word sits BELOW the handle, so the header is at handle[-8]. That
    // placement is what keeps every property GEP and the $cast display arithmetic
    // a plain offset off the handle a program holds, with the header out of their
    // way (Gc.h:54-59).
    const int64_t hdr_off = -static_cast<int64_t>(RT_GC_HEADER_BYTES);
    llvm::Value* hdr = b_.CreateGEP(b_.getInt8Ty(), owner, b_.getInt64(hdr_off), "gc.hdr");
    llvm::Value*  w   = b_.CreateLoad(b_.getInt64Ty(), hdr, "gc.word");
    llvm::Value*  age = b_.CreateAnd(
      w, b_.getInt64(RT_GC_FLAG_OLD | RT_GC_FLAG_LOGGED), "gc.age");
    llvm::Value* due = b_.CreateICmpEQ(age, b_.getInt64(RT_GC_FLAG_OLD), "gc.unlogged");
    auto*        remember = llvm::BasicBlock::Create(ctx_, "gc.remember", fn);
    auto*        done     = llvm::BasicBlock::Create(ctx_, "gc.logged", fn);
    llvm::BranchInst* br  = b_.CreateCondBr(due, remember, done);
    br->setMetadata(llvm::LLVMContext::MD_prof,
                    llvm::MDBuilder(ctx_).createBranchWeights(1, 1u << 20));

    b_.SetInsertPoint(remember);
    auto* call = b_.CreateCall(gc_remember_fn(), { owner });
    call->addFnAttr(llvm::Attribute::Cold);
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
}

// The barrier for a store through `lv`, or nothing when `lv` does not name a place
// inside a class object. THE call site for every instrumented store: the owner
// test lives here, once, rather than repeated at each of the eleven store sites,
// so a new store site is instrumented by routing through this function and cannot
// forget it.
void Backend::emit_gc_barrier_for(const LValue& lv) {
    if (lv.gc_owner == nullptr) {
        return;
    }
    // Both halves of the propagation invariant, asserted where it is consumed: a
    // string leaf cannot hold a class handle, so the two string element lvalues
    // deliberately do not carry an owner, and an owner is always a class body --
    // never an i1 or a plane value picked up by accident.
    assert(!lv.is_string && "a string place never carries a GC owner");
    assert(lv.gc_owner->getType()->isPointerTy() &&
           "an owner is a class body pointer (HandleDeref's inttoptr)");
    emit_gc_barrier(lv.gc_owner);
}

// Record the OUTER container on an element lvalue that has just re-based into that
// container's malloc'd payload. Only the outermost is kept -- `obj.qq[1][2]` records
// `obj.qq`, whose handle is in the body -- because that is the only address on the
// path that a ring growth cannot relocate, and the collector's container walk
// reaches every nested element from it (visit_elem_image pushes a nested container
// and drain_containers runs both worklists to a fixpoint, Gc.cpp:1266-1271, :1381).
// `parent` is the container's own lvalue, so an already-payload-rebased parent hands
// its record down and this only records where the OUTERMOST one is still unknown.
// Inheriting and recording in one call is deliberate: split across two statements at
// five element sites, one of them would eventually be written without the other.
//
// Skipped when there is no owner, which also skips the case that needs nothing: a
// frame-local container is already a registered root for its activation, so its
// elements are reached without any of this.
void Backend::note_gc_container_root(LValue& lv, const LValue& parent, llvm::Value* handle, Ref container_type) {
    assert(handle != nullptr && "a container root needs its handle address");
    assert(container_type != Ref::Invalid && "a container root names its type");
    if (lv.gc_owner == nullptr) {
        return;
    }
    lv.gc_container_root = parent.gc_container_root;
    lv.gc_container_type = parent.gc_container_type;
    if (lv.gc_container_root != nullptr) {
        return; // an outer container already stands in for this whole path
    }
    lv.gc_container_root = handle;
    lv.gc_container_type = container_type;
}

// A `ref` actual aiming INSIDE a container owned by a class object hands the callee
// a payload-interior pointer. rt_gc_find_object cannot resolve one -- it is
// indistinguishable from an integer -- so the conservative frame scan cannot reach
// the owner and relog_conservative_roots cannot carry it across the suspend that
// precedes the callee's store. Registering the outer container STANDS IN for that
// resolution, and therefore has to do both halves of what the frame scan does:
//
//   reach the elements -- push_container_roots walks a registered container in full
//     at every collection of either kind, so the handle the callee stores is found
//     from the root path whatever the owner's age and whether or not it is logged;
//   keep the owner alive -- rt_gc_container_add resolves the owner once at
//     registration and push_container_roots marks it before reading the payload, so
//     an unpaired registration can only LEAK its owner and can never name memory a
//     dead object's finalizer freed.
//
// That makes the nested case behave exactly like a direct property container, where
// the frame scan already pins and marks the owner off a body-interior pointer.
//
// Task calls only. A function cannot suspend, and the only safepoint is the drain
// loop's quiescent edge (:11542), so no collection can occur inside a function call
// and the call-site barrier alone covers it.
llvm::Value* Backend::emit_gc_ref_container_add(const LValue& lv) {
    if (lv.gc_container_root == nullptr) {
        return nullptr;
    }
    assert(lv.gc_owner != nullptr && "a container root is recorded only inside an object");
    assert(lv.gc_container_type != Ref::Invalid && "a registered container names its type");
    b_.CreateCall(gc_container_add_fn(),
                  { lv.gc_container_root,
                    b_.getInt32(gc_container_kind(lv.gc_container_type)),
                    elem_ops_for(lv.gc_container_type) });
    return lv.gc_container_root;
}

} // namespace clotho::ir::backend
