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

// The shape of the bytes one value of `type` serializes to inside a normalized key
// span, as rt_key_* words (Runtime.h) appended to `out`. Returns whether those
// bytes can hold a class or event handle.
//
// This walk exists because a key is a strong reference: an entry keyed by an object
// keeps that object alive (LRM 7.8.3), and the handle exists ONLY in the key bytes,
// so the collector has to decode it back out of them.
//
// It follows the serializers arm for arm, and deliberately so: `agg_key_bytes`
// sizes a fixed span, `emit_agg_key_bytes` writes one and
// `emit_variable_agg_key_bytes` writes a variable one, all as struct fields in
// declaration order, then array elements in order, a length before a string, a
// count before a container's elements, and `ceil(W/8)` bytes per scalar leaf with
// no padding (assoc-keys.md §4). Keeping the same shape is what makes this program
// provably the one the bytes were written by. The layout is SERIALIZED, not
// storage: a storage walk would place a handle after a struct's padding.
//
// `last_skip` is the index in `out` of the skip node this level ended with, or npos,
// so that neighbouring handle-free leaves become one node.
bool Backend::key_shape(Ref type, std::vector<uint32_t>& out, size_t& last_skip) {
    static constexpr size_t npos = ~size_t{ 0 };
    const auto skip = [&](uint32_t bytes) {
        if (last_skip != npos) {
            out[last_skip + 1] += bytes;
            return;
        }
        last_skip = out.size();
        out.push_back(rt_key_skip);
        out.push_back(bytes);
    };
    const auto body_of = [&](Ref inner, std::vector<uint32_t>& body) {
        size_t none = npos;
        return key_shape(inner, body, none);
    };
    const auto words = [](const std::vector<uint32_t>& v) { return static_cast<uint32_t>(v.size()); };
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        bool handle = false;
        for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
            handle = key_shape(f.type, out, last_skip) || handle;
        }
        return handle;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        std::vector<uint32_t> body;
        const bool            handle = body_of(t.element, body);
        if (body.size() == 2 && body[0] == rt_key_skip) {
            // Elements of one fixed handle-free size are one run of bytes.
            skip(array_dim(t) * body[1]);
            return false;
        }
        out.insert(out.end(), { rt_key_rep, array_dim(t), words(body) });
        out.insert(out.end(), body.begin(), body.end());
        last_skip = npos;
        return handle;
    }
    if (t.kind == Type::Kind::String) {
        out.push_back(rt_key_string);
        last_skip = npos;
        return false;
    }
    if (t.kind == Type::Kind::Queue || t.kind == Type::Kind::DynArray) {
        std::vector<uint32_t> body;
        const bool            handle = body_of(t.element, body);
        out.insert(out.end(), { rt_key_seq, words(body) });
        out.insert(out.end(), body.begin(), body.end());
        last_skip = npos;
        return handle;
    }
    if (t.kind == Type::Kind::Assoc) {
        // Each entry carries the nested array's own key bytes behind their length,
        // so a nested key that holds no handle needs no shape (and a bare string
        // key, which is not self-delimiting, could not have one).
        const std::vector<uint32_t> key = assoc_key_shape(type);
        std::vector<uint32_t>       value;
        const bool                  handle = body_of(t.element, value);
        out.insert(out.end(), { rt_key_map, words(key), words(value) });
        out.insert(out.end(), key.begin(), key.end());
        out.insert(out.end(), value.begin(), value.end());
        last_skip = npos;
        return handle || !key.empty();
    }
    if (types::is_object_handle(mod_.pool, type)) {
        // The object's identity number (key_leaf_word): 64-bit unsigned, so exactly 8
        // big-endian bytes with no sign flip, which the collector resolves to the object.
        assert(agg_key_bytes(type) == 8 && "a handle normalizes to 8 key bytes");
        out.push_back(rt_key_handle);
        last_skip = npos;
        return true;
    }
    skip(agg_key_bytes(type));
    return false;
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
        entries.push_back(vtable_entry(fid));
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
                islots.push_back(vtable_entry(cd->iface_slots[i]));
            } else if (mod_.iface_unreachable.has_value()) {
                islots.push_back(vtable_entry(*mod_.iface_unreachable));
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
    // The referent of a weak reference (LRM 8.30) is handle-typed like any property,
    // and left out here: not being in this table is what makes it weak. The map
    // names it separately (rt_gc_map::weak_referent) so the collector can clear it.
    const WeakClass* weak = mod_.weak_class(class_ref);
    uint32_t         f    = 0;
    for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
        if (weak == nullptr || weak->referent_field != f) {
            collect_class_handles(fld.type, bl->getElementOffset(hdr + f), body->getElementType(hdr + f), out);
        }
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
// gets an entry. The event's handle word is traced through `handles` instead.
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
// for a frame slot. Null when the class owns nothing to release.
//
// This is what closes the second-order leak: emit_class_body_init initialises
// string and container properties on every `new` and, before the collector,
// nothing ever tore them down (docs/design/garbage-collection.md §0).
//
// The built-in `process` class is the one class that owns a native resource instead
// of a managed property: a reference on the control block its object names, which
// the finalizer drops and the block's weak memo of the object goes with
// (docs/design/process-class.md §4.2).
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
    const bool process = mod_.is_process_class(class_ref);
    if (!managed && !process) {
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
    if (process) {
        // Field 0 is the vptr and field 1 the block pointer, stored as a 2-state word
        // so no collector ever follows it (emit_proc_object).
        llvm::Value* w = b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(root_ty, body, hdr));
        b_.CreateCall(proc_obj_release_fn(), { b_.CreateIntToPtr(w, raw_ptr_ty()), body });
    }
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
    // A weak reference (LRM 8.30): where its referent is, and where the event is
    // that a wait on `get()` parks on, if the design has such a wait. The referent
    // is a plain word, so class_handle_offsets left it out, which is the whole of
    // what makes the reference weak; naming it here is what lets the collector
    // clear it. Offset 0 is the vptr, so 0 says "none" for both.
    uint32_t        weak_referent = 0;
    uint32_t        weak_notify   = 0;
    llvm::Constant* weak_shared   = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    if (const WeakClass* weak = mod_.weak_class(class_ref); weak != nullptr) {
        const auto*    sl      = env_->llmod.getDataLayout().getStructLayout(class_body_ty(class_ref));
        const uint32_t hdr     = class_header_words(class_ref);
        weak_referent          = static_cast<uint32_t>(sl->getElementOffset(hdr + weak->referent_field));
        const ChangeNotify* cn = mod_.change_notify(class_ref, weak->referent_field);
        if (cn != nullptr && cn->field[static_cast<uint32_t>(Edge::Any)] != kNoNotify) {
            weak_notify =
              static_cast<uint32_t>(sl->getElementOffset(hdr + cn->field[static_cast<uint32_t>(Edge::Any)]));
        }
        if (cn != nullptr && cn->shared != kNoNotify) {
            // The referents' design-wide notifier, by the address of its handle
            // (chg_shared_slot): the collector has no other way to the event.
            weak_shared = chg_shared_slot(cn->shared);
        }
        assert(weak_referent != 0 && "the referent follows the vptr");
    }
    auto* init = llvm::ConstantStruct::get(gc_map_ty(),
                                           { b_.getInt64(size),
                                             b_.getInt32(static_cast<uint32_t>(offsets.size())),
                                             b_.getInt32(static_cast<uint32_t>(leaves.size())),
                                             gc_offsets_global(offsets, nm + ".handles"),
                                             leaf_arr,
                                             class_finalizer_fn(class_ref),
                                             b_.getInt32(weak_referent),
                                             b_.getInt32(weak_notify),
                                             weak_shared });
    return new llvm::GlobalVariable(env_->llmod, gc_map_ty(), /*isConstant=*/true,
                                    llvm::GlobalValue::PrivateLinkage, init, nm);
}

// rt_gc_map::size for a class: the body's alloc size rounded up to the heap's
// granule (Gc.h). This is the byte count rt_gc_alloc bumps by, so the ClassNew
// site and the map constant MUST agree -- hence one helper feeding both rather
// than the same expression written twice.
uint64_t Backend::gc_object_bytes(Ref class_ref) {
    const uint64_t body_bytes =
      env_->llmod.getDataLayout().getTypeAllocSize(class_body_ty(class_ref));
    const uint64_t size = (body_bytes + RT_GC_ALIGN - 1) & ~(uint64_t{ RT_GC_ALIGN } - 1);
    assert(size >= body_bytes && "object size must cover the body");
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
//
// The third half is the NBA SHADOW of a target that can hold a handle (`h <= new C`,
// `s.h <= x`): the value sits in the shadow from staging to the NBA commit, and no
// signal holds it yet. Nothing separated staging from commit by a collection until an
// allocation could collect (design §6.1), so shadows were never roots; now a handle
// that exists only there would be freed. The shadow is registered as a conservatively
// scanned range (rt_gc_range_add) rather than described by offsets: it is a flat
// staging struct of masks and planes beside the handle, and a stale word in it is
// retention at worst, which the validation of every candidate bounds.
//
// Each of the three is registered by one loop over a table of its regions
// (emit_rows), not by a call per region: a region is an instance's state, and
// root's code must not grow with the number of instances.
void Backend::emit_rows(const std::vector<llvm::Constant*>& rows, llvm::Type* row_ty, const char* name,
                        llvm::function_ref<void(llvm::Value* row)> body) {
    if (rows.empty()) {
        return;
    }
    auto* i32    = b_.getInt32Ty();
    auto* arr_ty = llvm::ArrayType::get(row_ty, rows.size());
    auto* table  = new llvm::GlobalVariable(env_->llmod, arr_ty, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantArray::get(arr_ty, rows), name);
    auto* fn     = b_.GetInsertBlock()->getParent();
    auto* head   = llvm::BasicBlock::Create(ctx_, "rows.head", fn);
    auto* each   = llvm::BasicBlock::Create(ctx_, "rows.body", fn);
    auto* done   = llvm::BasicBlock::Create(ctx_, "rows.done", fn);
    auto* entry  = b_.GetInsertBlock();
    b_.CreateBr(head);
    b_.SetInsertPoint(head);
    auto* idx = b_.CreatePHI(i32, 2);
    idx->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(idx, b_.getInt32(static_cast<uint32_t>(rows.size()))), each, done);
    b_.SetInsertPoint(each);
    body(b_.CreateInBoundsGEP(arr_ty, table, { b_.getInt32(0), idx }));
    idx->addIncoming(b_.CreateAdd(idx, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);
    b_.SetInsertPoint(done);
}

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
    auto*                        ptr     = raw_ptr_ty();
    uint32_t                     emitted = 0;
    std::vector<llvm::Constant*> root_rows;
    for (size_t r = 0; r < by_region.size(); ++r) {
        if (by_region[r].empty()) {
            continue; // a region with no class handle needs no table
        }
        llvm::Constant* base =
          r == root_region ? sym(state_global_) : sym(anchors_[r].state_gv);
        root_rows.push_back(gc_roots_global(base, by_region[r], emitted++));
    }
    assert(emitted <= by_region.size() && "one table per region at most");
    emit_rows(root_rows, ptr, "clotho.gcroots.regions", [&](llvm::Value* row) {
        b_.CreateCall(gc_roots_add_fn(), { b_.CreateLoad(ptr, row) });
    });
    uint32_t                     cont_emitted = 0;
    std::vector<llvm::Constant*> cont_rows;
    for (size_t r = 0; r < cont_by_region.size(); ++r) {
        if (cont_by_region[r].empty()) {
            continue; // a region with no handle-bearing container needs no table
        }
        llvm::Constant* base =
          r == root_region ? sym(state_global_) : sym(anchors_[r].state_gv);
        cont_rows.push_back(gc_containers_global(base, cont_by_region[r], cont_emitted++));
    }
    emit_rows(cont_rows, ptr, "clotho.gccontainers.regions", [&](llvm::Value* row) {
        b_.CreateCall(gc_containers_add_fn(), { b_.CreateLoad(ptr, row) });
    });
    // NBA shadows that can stage a handle, one range per region that has any.
    std::vector<bool> shadow_roots(anchors_.size() + 1, false);
    for (const SignalSlot& s : slots_) {
        if (s.nba && reaches_class_handle(s.ir_type)) {
            shadow_roots[s.anchor == SignalSlot::no_anchor ? root_region : s.anchor] = true;
        }
    }
    auto* const                  range_ty = llvm::StructType::get(ctx_, { ptr, b_.getInt64Ty() });
    std::vector<llvm::Constant*> shadow_rows;
    for (size_t r = 0; r < shadow_roots.size(); ++r) {
        llvm::GlobalVariable* shadow = r == root_region ? shadow_global_ : anchors_[r].shadow_gv;
        if (!shadow_roots[r] || shadow == nullptr) {
            continue; // no handle staged here, or only through deferred records, which are roots
        }
        shadow_rows.push_back(llvm::ConstantStruct::get(
          range_ty, { sym(shadow), b_.getInt64(dl.getTypeAllocSize(shadow->getValueType())) }));
    }
    emit_rows(shadow_rows, range_ty, "clotho.gcshadows.regions", [&](llvm::Value* row) {
        b_.CreateCall(gc_range_add_fn(),
                      { b_.CreateLoad(ptr, b_.CreateStructGEP(range_ty, row, 0)),
                        b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(range_ty, row, 1)) });
    });
    emit_gc_nba_queues();
    assert(cont_emitted <= cont_by_region.size() && "one container table per region at most");
}

// The NBA staging queues (the shared rt_nba_arr_* buffers) whose records can hold a class
// handle, registered once at start-up beside the root tables. The fourth half of the roots:
// a plain nonblocking assignment to an array element, a member written through a virtual
// interface, an aggregate leaf reached through a runtime index, or a whole container
// stages into one of these, and from staging to the NBA commit the record can be the only
// reference to what it carries. Which kind of record a queue holds is the signal's staging
// path (Stores.cpp: arr_rec_ty, agg_rec_ty, handle_rec_ty), and the same type test that
// decides a root table's existence decides whether a queue needs one, so a design that
// stages no handle registers nothing. Index spaces as the staging code's: arrays, then
// aggregates, then whole containers.
void Backend::emit_gc_nba_queues() {
    const llvm::DataLayout& dl = env_->llmod.getDataLayout();
    const auto stride_of = [&](llvm::StructType* rt) {
        return b_.getInt32(static_cast<uint32_t>(dl.getTypeAllocSize(rt)));
    };
    for (const Signal sig : array_nba_sigs_) {
        const SignalSlot& s = slot_of(sig);
        if (reaches_class_handle(s.ir_type)) {
            b_.CreateCall(gc_nba_words_add_fn(), { b_.getInt32(arr_idx(sig)), stride_of(arr_rec_ty(s)) });
        }
    }
    for (const Signal sig : agg_nba_sigs_) {
        const SignalSlot& s = slot_of(sig);
        if (agg_rec_rooted(s)) {
            b_.CreateCall(gc_nba_words_add_fn(), { b_.getInt32(agg_buf_idx(sig)), stride_of(agg_rec_ty(s)) });
        }
    }
    for (const Signal sig : handle_nba_sigs_) {
        const Ref type = handle_nba_types_[handle_nba_slot(sig)];
        if (!reaches_class_handle(type)) {
            continue;
        }
        llvm::StructType* rt     = handle_rec_ty(type);
        const uint32_t    offset = static_cast<uint32_t>(dl.getStructLayout(rt)->getElementOffset(1));
        b_.CreateCall(gc_nba_container_add_fn(),
                      { b_.getInt32(handle_buf_idx(sig)), stride_of(rt), b_.getInt32(offset),
                        b_.getInt32(gc_container_kind(type)), elem_ops_for(type) });
    }
}

// The byte offsets of every class- or event-handle word inside one signal's
// storage, appended relative to `base` (that storage's offset in its region).
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
    if (!types::is_object_handle(mod_.pool, leaf)) {
        return;
    }
    assert(s.slot_ty != nullptr && "a handle slot has a leaf slot type");
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
// A class above the runtime's large-object threshold keeps the out-of-line
// rt_gc_alloc: such an object never comes out of a block at all, it owns a
// mapping of its own, so there is no bump range to bump and nothing the inline
// sequence could do with one. RT_GC_LARGE_OBJECT_BYTES is the runtime's own macro
// and Gc.cpp's kLargeObjectBytes is defined from it, so the threshold cannot drift
// between the two files. The comparison is `<=`, matching rt_gc_alloc's own
// `size <= kLargeObjectBytes` exactly, so a class of precisely threshold size is
// block-resident on both paths rather than on whichever one happened to round
// differently.
bool Backend::gc_alloc_is_inline(Ref class_ref) {
    const uint64_t size = gc_object_bytes(class_ref);
    assert(size >= sizeof(void*) && "an object holds at least its vptr");
    assert(size % RT_GC_ALIGN == 0 && "an object size is granule-aligned");
    return size <= RT_GC_LARGE_OBJECT_BYTES;
}

// The address of the metadata byte for the granule `obj` points at: the chunk is
// the address with its low bits cleared, and the byte is RT_GC_META_OFFSET plus
// the granule's index into the chunk (Gc.h). Arithmetic on the address alone, so
// it needs nothing from the mutator and holds for any object, a large one
// included, whose mapping is aligned as a chunk is.
llvm::Value* Backend::emit_gc_meta_ptr(llvm::Value* obj, const char* nm) {
    assert(obj != nullptr && obj->getType()->isPointerTy() && "a metadata byte is found from a pointer");
    constexpr unsigned kGranuleShift = 4;
    static_assert((1u << kGranuleShift) == RT_GC_ALIGN, "the granule shift is log2(RT_GC_ALIGN)");
    static_assert((RT_GC_CHUNK_BYTES & (RT_GC_CHUNK_BYTES - 1)) == 0, "a chunk is a power of two");
    const uint64_t mask  = uint64_t{ RT_GC_CHUNK_BYTES } - 1;
    llvm::Value*   addr  = b_.CreatePtrToInt(obj, b_.getInt64Ty());
    llvm::Value*   chunk = b_.CreateIntrinsic(llvm::Intrinsic::ptrmask, { raw_ptr_ty(), b_.getInt64Ty() },
                                              { obj, b_.getInt64(~mask) }, nullptr, "gc.chunk");
    llvm::Value*   gran  = b_.CreateLShr(b_.CreateAnd(addr, b_.getInt64(mask)), kGranuleShift, "gc.granule");
    llvm::Value*   off   = b_.CreateAdd(gran, b_.getInt64(RT_GC_META_OFFSET), "gc.metaoff", /*HasNUW=*/true,
                                        /*HasNSW=*/true);
    return b_.CreateGEP(b_.getInt8Ty(), chunk, off, nm);
}

// The metadata bytes of a fresh object of `granules` granules, as a constant the
// fast path copies into place: the first byte says an object starts here and
// whether it owes a finalizer, and each later byte holds its distance back to the
// first, capped (Gc.h). The runtime's set_object_meta writes the same bytes, and
// rt_gc_verify() checks both against one description.
//
// Neither half is optional bookkeeping, which is why the copy is emitted before
// the handle can escape: an object without its first byte is INVISIBLE to
// rt_gc_find_object, which is both the conservative-scan validator and the
// interior-pointer resolver, and one without FINALIZE never has its finalizer
// run, so every `string` and container buffer the body owns leaks.
llvm::Constant* Backend::gc_meta_pattern(uint64_t granules, bool fin) {
    assert(granules >= 1 && granules <= RT_GC_BLOCK_BYTES / RT_GC_ALIGN && "an inlined object fits a block");
    const std::string nm = "clotho.gc.meta." + std::to_string(granules) + (fin ? ".fin" : "");
    if (auto* g = env_->llmod.getNamedGlobal(nm)) {
        return g;
    }
    std::vector<uint8_t> bytes(granules);
    bytes[0] = static_cast<uint8_t>(RT_GC_META_START | (fin ? RT_GC_META_FINALIZE : 0));
    for (uint64_t k = 1; k < granules; ++k) {
        bytes[k] = static_cast<uint8_t>(k < RT_GC_META_BACK_MAX ? k : RT_GC_META_BACK_MAX);
    }
    llvm::Constant* init = llvm::ConstantDataArray::get(ctx_, llvm::ArrayRef<uint8_t>(bytes));
    auto* g = new llvm::GlobalVariable(env_->llmod, init->getType(), /*isConstant=*/true,
                                       llvm::GlobalValue::PrivateLinkage, init, nm);
    g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return g;
}

// `new`, inlined: bump the mutator's cursor within its current hole and call out
// only when the hole cannot satisfy the request (design §4.3). Returns the HANDLE,
// exactly as rt_gc_alloc does, leaving the insert point on the join block.
//
// The two paths produce one handle value and identical observable state: the fast
// block does by hand precisely what rt_gc_alloc_slow does in the runtime -- write
// the object's metadata bytes, with FINALIZE when the class has a finalizer, and
// zero the body -- so the phi's two inputs are indistinguishable to
// everything downstream, including emit_class_body_init and the vptr store the
// caller emits once, after the join.
//
// The nursery budget is deliberately absent. rt_gc_alloc_slow charges it per HOLE
// handed out rather than per object (Gc.h:243-253), which is what keeps this
// sequence free of budget arithmetic while leaving the accounting exact in bytes
// made available. For the same reason the inlined path is also where nothing can
// collect: only the slow call can, and it collects before it allocates, so what this
// function returns is never an object a collection has seen half-built (Gc.h, rt_gc_alloc).
//
// `site` names the statement for the slow path's located fatal; the fast path never
// reads it, so it costs the cold block one constant argument and nothing else.
llvm::Value* Backend::emit_gc_alloc_fast(Ref class_ref, llvm::Constant* site) {
    assert(gc_alloc_is_inline(class_ref) && "a large object keeps the out-of-line call");
    const uint64_t  size = gc_object_bytes(class_ref);
    llvm::Constant* map  = class_gc_map_global(class_ref);
    // FINALIZE is the only flag `new` ever sets, and whether it is set is a
    // compile-time property of the class. MARK stays clear, which is "young" and is
    // what makes the next collection trace the object; LOGGED and PINNED are the
    // collector's own and start clear. The flag MUST agree with `map->finalize`
    // being non-null: rt_gc_verify() asserts the two agree.
    const bool fin = !class_finalizer_fn(class_ref)->isNullValue();

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
    // A constant length, so this lowers to a store or two for a class of ordinary
    // size. The granules are free, their bytes zero, so a copy and not an OR.
    const uint64_t granules = size / RT_GC_ALIGN;
    b_.CreateMemCpy(emit_gc_meta_ptr(cur, "gc.meta"), llvm::MaybeAlign(1), gc_meta_pattern(granules, fin),
                    llvm::MaybeAlign(1), b_.getInt64(granules));
    // The body. 2-state leaves and handle properties power on 0 / null (LRM Table
    // 7-1) and these bytes are recycled heap, so this is required rather than merely
    // tidy. The padding that rounds the object up to a granule is left as it is:
    // nothing reads it, and rt_gc_alloc zeroing it too changes nothing a program or
    // the collector can see. The handle is the object's first byte, on a granule
    // boundary.
    const uint64_t body = env_->llmod.getDataLayout().getTypeAllocSize(class_body_ty(class_ref));
    assert(body <= size && size - body < RT_GC_ALIGN && "an object is its body rounded up to a granule");
    b_.CreateMemSet(cur, b_.getInt8(0), b_.getInt64(body), llvm::MaybeAlign(RT_GC_ALIGN));
    b_.CreateBr(cont);

    b_.SetInsertPoint(slow);
    llvm::Value* slow_hdl =
      b_.CreateCall(gc_alloc_slow_fn(),
                    { gc_mutator_cur(), b_.getInt64(size), map,
                      site != nullptr ? site : llvm::Constant::getNullValue(raw_ptr_ty()) },
                    "gc.slowhdl");
    b_.CreateBr(cont);

    b_.SetInsertPoint(cont);
    llvm::PHINode* out = b_.CreatePHI(raw_ptr_ty(), 2, "gc.obj");
    out->addIncoming(cur, fast);
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
//
// A collection here is the one that clears weak references, and it puts the
// notifier of each one it clears that a wait reads get() of on the pending chain
// (LRM 8.30.3; runtime/Gc.cpp, clear_weak_referents). Those are fired as soon as it
// returns: every process is parked at this edge, so the waiters woken are the ones
// that were waiting when the reference was cleared, and they run in the time step
// that is about to begin. The chain holds nothing else, since the NBA region
// emptied it before time moved. The trigger is an immediate one, as a store's
// notification is, not the update event of a `->>` (which belongs to an NBA pass);
// the order among the notifiers of one collection means nothing. Only a design with
// such a wait has the code, on the cold side of the poll.
void Backend::emit_gc_safepoint(llvm::Value* wheel) {
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
    const bool weak_waits = std::ranges::any_of(mod_.weak_classes(), [&](const WeakClass& weak) {
        return mod_.change_notify(weak.class_ref, weak.referent_field) != nullptr;
    });
    if (weak_waits) {
        auto*        ptr   = llvm::PointerType::getUnqual(ctx_);
        auto*        null  = llvm::ConstantPointerNull::get(ptr);
        auto*        walk  = llvm::BasicBlock::Create(ctx_, "gc.weak.walk", fn);
        auto*        fire  = llvm::BasicBlock::Create(ctx_, "gc.weak.fire", fn);
        auto*        fired = llvm::BasicBlock::Create(ctx_, "gc.weak.fired", fn);
        llvm::Value* headp = sym(event_pend_head_gv_);
        llvm::Value* first = b_.CreateLoad(ptr, headp);
        b_.CreateStore(null, headp);
        auto* entry = b_.GetInsertBlock();
        b_.CreateBr(walk);

        b_.SetInsertPoint(walk);
        llvm::PHINode* n = b_.CreatePHI(ptr, 2);
        n->addIncoming(first, entry);
        b_.CreateCondBr(b_.CreateICmpNE(n, null), fire, fired);

        // The link is read before the fire, as emit_event_pend_drain reads it.
        b_.SetInsertPoint(fire);
        llvm::Value* next = b_.CreateLoad(ptr, event_field(n, kEventNextPend));
        b_.CreateStore(null, event_field(n, kEventNextPend));
        b_.CreateStore(b_.getInt8(0), event_field(n, kEventPending));
        emit_event_fire(n, wheel, RT_TRACE_TRIGGER_NOW);
        n->addIncoming(next, b_.GetInsertBlock());
        b_.CreateBr(walk);

        b_.SetInsertPoint(fired);
    }
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
// One mask and one compare rather than two tests: MARK says the object is old, so
// a young collection will not trace it, and LOGGED says the remembered set already
// holds it, so the exact value MARK-without-LOGGED is the only case that needs the
// call. The branch is weighted cold the way emit_gc_safepoint and
// emit_null_handle_check weight theirs, which sinks the call out of line and
// leaves the address arithmetic, a byte load, an and/compare pair folded by the
// backend, and a not-taken branch on the path a young object or an already-logged
// object takes.
void Backend::emit_gc_barrier(llvm::Value* owner) {
    assert(owner != nullptr && "a barrier needs the object acquiring the reference");
    llvm::Function* fn = b_.GetInsertBlock()->getParent();
    assert(fn != nullptr && "a barrier is emitted into some function");
    // The object body as a pointer, which is what LValue::gc_owner always holds
    // (HandleDeref's own inttoptr, reused rather than repeated). A caller holding
    // only the i64 handle bits converts once at its own site.
    assert(owner->getType()->isPointerTy() && "an owner is a class body pointer");
    // The flags are in the metadata byte of the object's first granule, which the
    // handle's address alone locates (emit_gc_meta_ptr).
    llvm::Value* w   = b_.CreateLoad(b_.getInt8Ty(), emit_gc_meta_ptr(owner, "gc.metap"), "gc.meta");
    llvm::Value* age = b_.CreateAnd(w, b_.getInt8(RT_GC_META_MARK | RT_GC_META_LOGGED), "gc.age");
    llvm::Value* due = b_.CreateICmpEQ(age, b_.getInt8(RT_GC_META_MARK), "gc.unlogged");
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
// inside a class object or reached through a `ref` formal. THE call site for every
// instrumented store: the owner test lives here, once, rather than repeated at each of
// the eleven store sites, so a new store site is instrumented by routing through this
// function and cannot forget it.
void Backend::emit_gc_barrier_for(const LValueInfo& lv) {
    assert((lv.gc_owner == nullptr || lv.gc_ref_slot == nullptr) &&
           "a place is rooted at an object or at a ref formal, not both");
    if (lv.gc_ref_slot != nullptr) {
        assert(!lv.is_string && "a string place never carries a GC owner");
        emit_gc_note_ref_store(lv.gc_ref_slot, lv.gc_ref_chain);
        return;
    }
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

// What an element, a member or a resolved entry of `from` inherits for the write barrier:
// the object it is inside, or the `ref` formal it is reached through.
void Backend::carry_gc_place(LValueInfo& to, const LValueInfo& from) {
    to.gc_owner     = from.gc_owner;
    to.gc_ref_slot  = from.gc_ref_slot;
    to.gc_ref_chain = from.gc_ref_chain;
}

// The barrier of a store through an address the code cannot attribute to an owner
// statically: the aliased place of a `ref` formal, or the actual of an output formal.
// The runtime finds the object that holds `slot`, or the one that owns the container
// `chain` names when the place is an element of one, and logs it if it is old
// (rt_gc_note_ref_store). A call and not an inlined test: the owner's age is not known
// without finding the owner, and a handle stored through a `ref` is rare enough that the
// lookup is not a cost worth a second code path. Emitted at each store and not once at
// the call, because a task can suspend across a collection between two stores.
void Backend::emit_gc_note_ref_store(llvm::Value* slot, llvm::Value* chain) {
    assert(slot != nullptr && "a ref store names the place it aliases");
    auto* null = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    auto* call = b_.CreateCall(gc_note_ref_store_fn(), { slot, chain != nullptr ? chain : null });
    call->addFnAttr(llvm::Attribute::Cold);
}

} // namespace clotho::ir::backend
