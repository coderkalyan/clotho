// SystemVerilog types to IR types, constants, and signal and storage registration.

#include "lower/Lowerer.h"

namespace clotho::lower {

// An unpacked struct / fixed unpacked array is a legal associative-array index
// type (LRM 7.8.5) when equality is defined for it. Clotho's key model
// (docs/design/assoc-keys.md) admits every leaf with a byte-serializable
// value: an integral / handle-shaped leaf gets a fixed-width big-endian span,
// a `string` leaf (P1) gets a length-prefixed one, a queue / dynamic-array
// leaf (P2) gets a count-prefixed one of its own recursively-normalized
// elements, and an associative-array leaf (P3) gets a count-prefixed
// `(key, value)` list in the nested map's own sorted-key order -- all nested
// through structs and fixed arrays to any depth, completing LRM 7.8.5's own
// example (`struct { byte B; int I[*]; }`). The backend classifies the whole
// key type as fixed or variable and picks the matching encoder
// (ir/llvm/Backend.h AssocKeyInfo::is_variable). A `real` leaf makes the index
// type illegal outright ("real or shortreal data types, or a type containing
// a real or shortreal, shall be an illegal index type").
void Lowerer::guard_agg_key_leaves(const slang::ast::Type& kt, ir::Ref kr) {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(kr));
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        guard_agg_key_leaves(kt, t.element);
        return;
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        for (const ir::Field& f : module_.pool.struct_fields(t.fields_id)) {
            guard_agg_key_leaves(kt, f.type);
        }
        return;
    }
    if (module_.pool.is_float(kr)) {
        fail("associative array key type containing a real is illegal (LRM 7.8.5)");
    }
    if (t.kind == ir::Type::Kind::String) {
        return; // P1: length-prefixed leaf, admitted.
    }
    if (t.kind == ir::Type::Kind::Queue || t.kind == ir::Type::Kind::DynArray) {
        // P2: count-prefixed leaf, each element recursively normalized. The
        // element's own admissibility was already checked by lower_type's
        // Queue / DynArray arm (guard_container_element); recurse here so an
        // Assoc leaf nested inside it is still refused (P3), not silently
        // miscompiled.
        guard_agg_key_leaves(kt, t.element);
        return;
    }
    if (t.kind == ir::Type::Kind::Assoc) {
        // P3: count-prefixed leaf, `(key, value)` pairs in the nested map's own
        // sorted-key order -- canonical because two equal-content maps then
        // produce identical bytes (assoc-keys.md §4.5). The nested map's OWN
        // key type was already fully validated when IT was declared (every
        // legal-index-type check ran then); nothing further to check here.
        return;
    }
}

// True if `type`'s normalized key span has no compile-time bound: itself a
// string / queue / dynamic array / associative array, or (through struct /
// fixed-array nesting) any leaf is. Mirrors ir/llvm/AssocArrays.cpp's
// key_type_is_variable exactly -- kept as a second, independent walk (like
// agg_key_bytes / emit_agg_key_bytes / agg_key_handle_offsets already are)
// rather than exposed across the frontend/backend boundary.
bool Lowerer::key_type_is_variable(ir::Ref type) const {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    switch (t.kind) {
        case ir::Type::Kind::String:
        case ir::Type::Kind::Queue:
        case ir::Type::Kind::DynArray:
        case ir::Type::Kind::Assoc:
            return true;
        case ir::Type::Kind::Struct:
            if (t.is_packed) {
                return false;
            }
            for (const ir::Field& f : module_.pool.struct_fields(t.fields_id)) {
                if (key_type_is_variable(f.type)) {
                    return true;
                }
            }
            return false;
        case ir::Type::Kind::UnpackedArray:
            return key_type_is_variable(t.element);
        default:
            return false;
    }
}

// True if a class handle is reachable anywhere inside `type`.
bool Lowerer::key_type_has_class_handle(ir::Ref type) const {
    const ir::Type t = std::get<ir::Type>(module_.pool.get(type));
    switch (t.kind) {
        case ir::Type::Kind::Class:
            return true;
        case ir::Type::Kind::Struct:
            for (const ir::Field& f : module_.pool.struct_fields(t.fields_id)) {
                if (key_type_has_class_handle(f.type)) {
                    return true;
                }
            }
            return false;
        case ir::Type::Kind::UnpackedArray:
        case ir::Type::Kind::Queue:
        case ir::Type::Kind::DynArray:
            return key_type_has_class_handle(t.element);
        case ir::Type::Kind::Assoc:
            return key_type_has_class_handle(t.element) ||
                   (t.right == 0 && key_type_has_class_handle(static_cast<ir::Ref>(t.fields_id)));
        default:
            return false;
    }
}

// A class handle anywhere inside a VARIABLE key is refused: the collector's
// key-handle trace (assoc_key_handle_offsets, ir/llvm/AssocArrays.cpp) is a
// compile-time STATIC offset table, correct only for a FIXED key -- a
// variable key's byte offsets depend on runtime data (a string's length, a
// container's element count), so a handle anywhere in one could land at a
// different offset on every entry, or after a sibling variable component
// that precedes it in declaration order. A runtime decode walk the collector
// could use instead is not yet built. A handle in an otherwise-FIXED key
// (P0's bare class-handle / chandle-shaped keys, or one nested only through
// fixed struct/array members) is unaffected: its static offset already works.
void Lowerer::guard_key_no_handle_in_variable(ir::Ref key_ty) {
    if (!key_type_is_variable(key_ty) || !key_type_has_class_handle(key_ty)) {
        return;
    }
    fail("associative array key type combining a class handle with a string / "
         "queue / dynamic-array member is not supported");
}

// The element types a dynamic array / queue admits, with a loud refusal for the
// rest. Both share DynElem and the rt_elem_ops descriptor, so their element
// capability is identical by construction; `what` only names the container in
// the diagnostic.
void Lowerer::guard_container_element(const slang::ast::Type& et, ir::Ref er, std::string_view what) {
    if (et.isIntegral() || et.isFloating() || et.isString()) {
        return;
    }
    // A class handle or a chandle is a plain 64-bit word: objects are never
    // freed (docs/design/classes.md §7), so an element needs no lifecycle
    // beyond the POD copy rt_elem_ops already does, `null` is the all-zero
    // power-on default Table 7-1 asks for, and `==` on two handles is the
    // identity compare a word compare gives (LRM 6.14 gives a chandle the
    // same closed == / != / null / assignment operator set). Nothing else is
    // required of it.
    //
    // The two diverge in exactly one place -- whether the GC traces the word
    // -- and that fork needs no code here. `lower_type` puts a class handle in
    // Kind::Class and a chandle in a plain Kind::Int, and collect_class_handles
    // (ir/llvm/Analyses.cpp) keys on Kind::Class alone, so a chandle element's
    // rt_elem_ops::handles table comes out empty by construction: tracing one
    // would hand the collector a foreign pointer it never allocated
    // (docs/design/garbage-collection.md §0, "Never in scope").
    if (et.isClass() || et.isCHandle()) {
        return;
    }
    // An unpacked struct / union or fixed unpacked array element: the slot holds
    // the recursive storage image and `q[i]` is a heap-anchored aggregate lvalue
    // (dyn_agg_elem_lvalue), so member / element selects and whole-element copies
    // reuse the ordinary aggregate machinery. A string or nested-container leaf
    // inside it rides the kind-4 rt_elem_ops leaf table, which the runtime uses
    // to deep-copy and free that leaf per element.
    if (is_agg_abi_ref(er)) {
        return;
    }
    // A nested container element (queue, dynamic array or associative array):
    // the slot holds ITS handle, and the element descriptor chains through
    // rt_elem_ops::inner — rt_elem_dynarr / rt_elem_queue / rt_elem_assoc — so
    // the runtime deep-copies and frees the inner container per element. `q[i]`
    // is a handle lvalue, so the inner container's own methods and element
    // selects work on it unchanged.
    const ir::Type e = std::get<ir::Type>(module_.pool.get(er));
    if (e.kind == ir::Type::Kind::Queue || e.kind == ir::Type::Kind::DynArray ||
        e.kind == ir::Type::Kind::Assoc) {
        return;
    }
    // An event element is a 64-bit handle word, like a class handle, with one
    // extra requirement its descriptor carries: rt_elem_event CONSTRUCTS a new
    // object per slot (LRM Table 6-7's "New event"), where every other kind
    // copies a constant prototype image. Copying an element copies the handle,
    // so both slots then name one object -- LRM 15.5.5's merge.
    if (e.kind == ir::Type::Kind::Event) {
        return;
    }
    fail(std::string(what) + " element type is not supported");
}

ir::Ref Lowerer::lower_type(const slang::ast::Type& type) {
    const slang::ast::Type& ct = type.getCanonicalType();
    if (ct.isVoid()) {
        return module_.pool.void_type;
    }
    if (const auto* arr = ct.as_if<slang::ast::FixedSizeUnpackedArrayType>()) {
        // Fixed-size unpacked array (memory). Recurse on the element (a nested
        // unpacked array gives a multidimensional memory; otherwise an integral
        // element). Bounds come straight from the declared range.
        const slang::ConstantRange r = arr->range;
        if (r.left < 0 || r.right < 0) {
            // TODO: negative element-index bounds (e.g. arr[-1:1]). The ir Type
            // stores bounds as uint32; supporting them needs signed normalization
            // in the backend element-select. Rare in RTL memories.
            fail("unpacked array with negative index bounds is not supported");
        }
        ir::Type t;
        t.kind        = ir::Type::Kind::UnpackedArray;
        t.element     = lower_type(arr->elementType);
        t.elem_signed = arr->elementType.isSigned();
        t.left        = static_cast<uint32_t>(r.left);
        t.right       = static_cast<uint32_t>(r.right);
        return module_.pool.put(ir::Item{ t });
    }
    if (const auto* da = ct.as_if<slang::ast::DynamicArrayType>()) {
        // Dynamic array (LRM 7.5): a heap-backed rt_dynarr handle. Elements ride
        // the rt_elem_ops descriptor, which is why the admitted set is shared
        // with a queue (guard_container_element).
        const ir::Ref er = lower_type(da->elementType);
        guard_container_element(da->elementType.getCanonicalType(), er, "dynamic array");
        ir::Type t;
        t.kind        = ir::Type::Kind::DynArray;
        t.element     = er;
        t.elem_signed = da->elementType.isSigned();

        return module_.pool.put(ir::Item{ t });
    }
    if (const auto* qt = ct.as_if<slang::ast::QueueType>()) {
        // Queue (LRM 7.10): a heap-backed rt_queue ring handle. Same element
        // capability as a dynamic array -- both drive rt_elem_ops.
        const ir::Ref er = lower_type(qt->elementType);
        guard_container_element(qt->elementType.getCanonicalType(), er, "queue");
        ir::Type t;
        t.kind        = ir::Type::Kind::Queue;
        t.element     = er;
        t.elem_signed = qt->elementType.isSigned();
        // maxBound is the `[$:N]` max index (0 == unbounded); stored in left.
        t.left = qt->maxBound;

        return module_.pool.put(ir::Item{ t });
    }
    if (const auto* aa = ct.as_if<slang::ast::AssociativeArrayType>()) {
        // Associative array (LRM 7.8/7.9): a heap-backed rt_assoc handle
        // (ankerl map + lazy sorted key index). The value element rides the same
        // rt_elem_ops descriptor a queue / dynamic-array element does, so it
        // admits the same set (guard_container_element); keys reach the runtime
        // as normalized order-preserving byte spans built in the backend
        // (docs/design/dynamic-arrays.md §4a).
        //
        // What is special about an assoc value is only that `a[k]` is a DEFERRED
        // place: the entry's slot address depends on whether the access reads
        // (get_ptr, which must not create the entry — LRM 7.8.6) or writes
        // (put_ptr, which must). Load / Store and the container-method surface
        // each know their own direction, so every whole-value form works; the one
        // shape that does not is a further ELEMENT SELECT through a
        // container-valued entry (`a[k][i]`), which would have to defer twice.
        // The backend names that case precisely.
        const slang::ast::Type& et = aa->elementType.getCanonicalType();
        const ir::Ref           er = lower_type(aa->elementType);
        guard_container_element(et, er, "associative array value");
        ir::Type t;
        t.kind    = ir::Type::Kind::Assoc;
        t.element = er;
        if (aa->hasWildcardIndexType()) {
            // Wildcard index (`[*]`): keys are unsigned integral of arbitrary
            // size, normalized to a fixed 64-bit form. `fields_id` names a
            // canonical unsigned placeholder; `right` flags wildcard.
            t.right     = 1;
            t.fields_id = static_cast<uint32_t>(make_int_type(64, false));
        } else {
            t.elem_signed              = aa->indexType->isSigned();
            const slang::ast::Type& kt = aa->indexType->getCanonicalType();
            const ir::Ref           kr = lower_type(*aa->indexType);
            // A class-handle key (LRM 7.8.3) is its handle word: identity
            // equality, `null` a valid key, and an ordering that 7.8.3 leaves
            // "deterministic but arbitrary" -- here the handle's own value,
            // which is stable for the whole run (objects are never moved or
            // freed). An unpacked struct / fixed unpacked array key (7.8.5) is
            // serialized leaf by leaf in the backend. Both fold into the same
            // normalized byte span as an integral or string key.
            if (!kt.isClass() && !kt.isIntegral() && !kt.isString() && !is_agg_abi_type(kt) &&
                !kt.isCHandle() && !kt.isEvent() && !kt.isVirtualInterface() && !kt.isUnpackedUnion() &&
                kt.as_if<slang::ast::QueueType>() == nullptr &&
                kt.as_if<slang::ast::DynamicArrayType>() == nullptr &&
                kt.as_if<slang::ast::AssociativeArrayType>() == nullptr) {
                // 7.8.5 makes a real (or real-containing) index type illegal; every
                // other type slang gives `==` normalizes to a byte span (assoc-keys.md):
                // a chandle / event / virtual interface already lowers to a flat 64-bit
                // handle word, an unpacked union already lowers identically to a packed
                // union (one flat is_packed integral value), a queue / dynamic array (P2)
                // count-prefixes its normalized elements the same way a string
                // length-prefixes its bytes (P1), and an associative array (P3)
                // count-prefixes its (key, value) pairs in sorted-key order -- all fall
                // into assoc_key_info's integral-width branch or its variable-length
                // walk below.
                fail("associative array key type is not supported (integral, enum, "
                     "packed, string, class handle, chandle, event, virtual interface, "
                     "unpacked union, unpacked struct / array, queue, dynamic array, "
                     "associative array, or wildcard [*])");
            }
            if (is_agg_abi_type(kt) || kt.as_if<slang::ast::QueueType>() != nullptr ||
                kt.as_if<slang::ast::DynamicArrayType>() != nullptr ||
                kt.as_if<slang::ast::AssociativeArrayType>() != nullptr) {
                guard_agg_key_leaves(*aa->indexType, kr);
                guard_key_no_handle_in_variable(kr);
            }
            // Typed integral keys of any width normalize via the backend's
            // per-byte big-endian emit (wide keys included). String keys pass
            // their bytes through. Both are handled downstream.
            t.right     = 0;
            t.fields_id = static_cast<uint32_t>(kr);
        }

        return module_.pool.put(ir::Item{ t });
    }
    if (ct.isString()) {
        return module_.pool.string_type;
    }
    if (ct.isFloating()) {
        // realtime is synonymous with real (LRM 6.12); both canonicalize to
        // the pool's real type.
        const auto& ft = ct.as<slang::ast::FloatingType>();
        return ft.floatKind == slang::ast::FloatingType::ShortReal ? module_.pool.shortreal_type
                                                                   : module_.pool.real_type;
    }
    if (ct.isStruct()) {
        // Checked ahead of the isIntegral() fallback below: PackedStructType
        // derives IntegralType in slang (LRM 7.2.1 - a packed struct is a
        // single bit-vector value), so without this branch a packed struct
        // would silently fall through to make_int_type and lose its field
        // identity (member access / assignment patterns need it; whole-
        // object declare/assign works either way since the storage is the
        // same bit-vector shape).
        if (const auto cached = struct_type_cache_.find(&ct); cached != struct_type_cache_.end()) {
            return cached->second;
        }

        const bool   is_packed = !ct.isUnpackedStruct();
        const Scope& body      = is_packed ? static_cast<const Scope&>(ct.as<slang::ast::PackedStructType>())
                                           : static_cast<const Scope&>(ct.as<slang::ast::UnpackedStructType>());
        std::vector<ir::FieldRow> fields;
        for (const auto& f : body.membersOfType<slang::ast::FieldSymbol>()) {
            // Packed member: `bitOffset` is the field's bit position. Unpacked
            // member: addressed by declaration index (`fieldIndex`), no flat
            // offset — store the index so the field table is self-describing.
            fields.push_back({ f.name,
                               lower_type(f.getType()),
                               is_packed ? static_cast<uint32_t>(f.bitOffset) : f.fieldIndex,
                               !is_packed && f.getType().isSigned() });
        }

        ir::Type t;
        t.kind      = ir::Type::Kind::Struct;
        t.is_packed = is_packed;
        // Packed: the whole-value scalar domain / width (shares PackedArray
        // layout; a packed struct with any 4-state member is wholly 4-state
        // per LRM 7.2.1, so the single domain is exact). Unpacked: element /
        // bounds are meaningless — each member's domain lives in its own
        // field-table type, so no flattened whole-struct domain exists; the
        // element stays void so no consumer can mistake it for one.
        t.element =
          is_packed ? (ct.isFourState() ? module_.pool.logic_type : module_.pool.bit_type) : module_.pool.void_type;
        t.left      = is_packed ? ct.getBitWidth() - 1 : 0;
        t.right     = 0;
        t.fields_id = module_.pool.intern_struct_fields(std::move(fields));

        const ir::Ref ref = module_.pool.put(ir::Item{ t });
        struct_type_cache_.emplace(&ct, ref);

        return ref;
    }
    if (ct.isPackedUnion()) {
        // A packed union (LRM 7.3.1) is a single bit-vector value whose members
        // all overlay the least-significant bits (every member is the same
        // width). Like a packed struct it derives IntegralType in slang, so this
        // branch must sit ahead of the isIntegral() fallback or the union loses
        // its field identity (member access needs it). Reuses Kind::Struct with
        // is_packed=true: every field sits at bit_offset 0, so the packed
        // member-select path (a constant PartSelect) produces correct
        // reads/writes with no backend changes. Unpacked unions are a separate,
        // larger change (overlapping aggregate storage) and stay loud-failed.
        const auto& ut = ct.as<slang::ast::PackedUnionType>();
        if (const auto cached = struct_type_cache_.find(&ct); cached != struct_type_cache_.end()) {
            return cached->second;
        }

        // A tagged packed union (LRM 7.3.2) is still a flat bit-vector, just
        // with the tag reserved in the high `tagBits` bits — slang's
        // getBitWidth() already includes them, and every member payload
        // still overlays the LSB (bit_offset 0), so the same flat path
        // lowers it. The tag high bits are set by the `tagged` setter /
        // member-write and tested by pattern matching (docs/design/
        // tagged-unions.md); a `void` member has no payload and lowers to a
        // void field placeholder that is never part-selected.
        std::vector<ir::FieldRow> fields;
        for (const auto& f : static_cast<const Scope&>(ut).membersOfType<slang::ast::FieldSymbol>()) {
            fields.push_back({ f.name, lower_type(f.getType()), 0 });
        }

        ir::Type t;
        t.kind      = ir::Type::Kind::Struct;
        t.is_packed = true;
        t.is_union  = true;
        t.element   = ct.isFourState() ? module_.pool.logic_type : module_.pool.bit_type;
        t.left      = ct.getBitWidth() - 1;
        t.right     = 0;
        t.fields_id = module_.pool.intern_struct_fields(std::move(fields));

        const ir::Ref ref = module_.pool.put(ir::Item{ t });
        struct_type_cache_.emplace(&ct, ref);

        return ref;
    }
    if (ct.isUnpackedUnion()) {
        // An unpacked union (LRM 7.3) overlays members of possibly different
        // sizes at the same storage. LRM 7.3 does not fix a bit-level overlay
        // for the unpacked case (unlike the packed union of 7.3.1), so Clotho
        // picks the natural, hardware/verilator-matching one: a single flat
        // plane value sized to the widest member, every member overlaying the
        // least-significant bits. This lowers identically to a packed union
        // (Kind::Struct, is_packed, all field bit_offsets 0) — a member select
        // is a low part-select, a narrower member write leaves the high bits
        // untouched — and so reuses the whole packed value path (storage, NBA,
        // VCD, change detection) with no aggregate machinery. Members are
        // restricted to integral (packed) types: a real / string / unpacked
        // aggregate member has no flat-bit overlay and is gated loud.
        const auto& ut = ct.as<slang::ast::UnpackedUnionType>();
        if (const auto cached = struct_type_cache_.find(&ct); cached != struct_type_cache_.end()) {
            return cached->second;
        }

        std::vector<ir::FieldRow> fields;
        uint32_t                  max_width = 0;
        uint32_t                  nmembers  = 0;
        bool                      four      = false;
        for (const auto& f : static_cast<const Scope&>(ut).membersOfType<slang::ast::FieldSymbol>()) {
            ++nmembers;
            const slang::ast::Type& mt = f.getType().getCanonicalType();
            if (mt.isVoid()) {
                // A tagged union (LRM 7.3.2) may carry a `void` member: no
                // payload, only a tag encoding. Lowered as a void field
                // placeholder that is never part-selected.
                fields.push_back({ f.name, module_.pool.void_type, 0 });
                continue;
            }
            if (!mt.isIntegral()) {
                // TODO: unpacked-union member that is not a flat bit-vector
                // (real / string / unpacked struct or array / dynamic array /
                // queue / class) — no defined bit-level overlay (LRM 7.3). For
                // a tagged union this is the Phase 3 gap (docs/design/
                // tagged-unions.md).
                fail("unpacked union member must be an integral type");
            }
            max_width = std::max<uint32_t>(max_width, mt.getBitWidth());
            four |= mt.isFourState();
            // Every member overlays the LSB (bit_offset 0).
            fields.push_back({ f.name, lower_type(f.getType()), 0 });
        }

        // A tagged unpacked union reserves high tag bits, laid out above the
        // widest member's payload — the same {tag, payload} bit layout a
        // packed tagged union gets from slang, synthesized here since the
        // unpacked union has no slang-dictated width. tagBits = ceil(log2 n).
        const uint32_t tag_bits =
          ut.isTagged && nmembers > 1 ? static_cast<uint32_t>(std::bit_width(nmembers - 1)) : 0;

        ir::Type t;
        t.kind      = ir::Type::Kind::Struct;
        t.is_packed = true;
        t.is_union  = true;
        t.element   = four ? module_.pool.logic_type : module_.pool.bit_type;
        t.left      = max_width + tag_bits - 1;
        t.right     = 0;
        t.fields_id = module_.pool.intern_struct_fields(std::move(fields));

        const ir::Ref ref = module_.pool.put(ir::Item{ t });
        struct_type_cache_.emplace(&ct, ref);

        return ref;
    }
    if (ct.isEvent()) {
        // Named event (LRM 15.5): a reference type. The variable holds a 64-bit
        // handle `{ u32 slot, u32 gen }` naming an entry in the backend's event
        // object array (which carries the trigger timestamp, the waiter list and
        // the `->>` pending state). Handle semantics come out of the ordinary
        // scalar machinery -- assignment aliases, `==` is identity, `null` is the
        // all-zero power-on default -- exactly as for a class handle.
        // See docs/design/event-handles.md.
        return module_.pool.event_type;
    }
    if (ct.isCHandle()) {
        // chandle (LRM 6.14): an opaque pointer-width handle. Lowered as a
        // 2-state 64-bit value — the only legal operations are null /
        // assignment / equality (slang rejects arithmetic), all integer ops on
        // the handle bits. Enables DPI chandle args (marshaled to/from void*).
        return make_int_type(64, false);
    }
    if (ct.isClass()) {
        return lower_class_type(ct.as<slang::ast::ClassType>());
    }
    if (ct.isVirtualInterface()) {
        // A virtual interface variable (LRM 25.9) holds a 64-bit handle to one
        // elaborated interface instance. Only assignment, `==`/`!=` and `null`
        // apply to the variable itself, all of which the plane machinery gives;
        // a member reached through it lowers to IfaceSignal. Recording the
        // placeholder body here is what lets lower_compilation decide which
        // real instances a handle can name -- every instance of the same
        // definition, whether or not any assignment is visible.
        const auto& vit = ct.as<slang::ast::VirtualInterfaceType>();
        if (!std::ranges::contains(vif_defs_->wanted, &vit.iface.body)) {
            vif_defs_->wanted.push_back(&vit.iface.body);
        }
        return module_.pool.viface_type;
    }
    if (ct.isCovergroup()) {
        // A covergroup type (LRM 19) is a nullable handle -- slang's own
        // default value for one is `null` -- so it lowers exactly like a
        // chandle: a 2-state 64-bit word carrying the address of the instance
        // record the coverage runtime owns. Deliberately NOT a Kind::Class: an
        // instance must outlive the design, because LRM 19.11.3 aggregates type
        // coverage over instances the design has already dropped, so it cannot
        // be a collected object. Assignment, `== null`, and storing a handle in
        // a class property or a container all come out of the ordinary scalar
        // machinery. See docs/design/functional-coverage.md.
        return make_int_type(64, false);
    }
    if (!ct.isIntegral()) {
        // TODO: any OTHER type kind reaching here is a genuine gap; every kind
        // slang currently defines is handled by a branch above or by
        // isCovergroup() just above, so this should be unreachable in practice.
        fail("unsupported non-integral type");
    }

    return make_int_type(ct.getBitWidth(), ct.isFourState());
}

// Intern an integer literal, preserving x/z bits. Slang stores a 4-state
// SVInt as [value words][unknown words] with X=(val0,unk1), Z=(val1,unk1) —
// the opposite val convention from the pool's VPI planes (X=(val1,xz1),
// Z=(val0,xz1)). The unknown word IS the xz plane; the val plane is
// value^unknown so the per-bit codes line up. 2-state literals take the fast
// uint64 path.
ir::Ref Lowerer::lower_int_literal(const IntegerLiteral& lit) {
    return svint_to_ref(lit.getValue(), lower_type(*lit.type));
}

// Convert a slang SVInt into a pooled IR constant of the given type.
ir::Ref Lowerer::svint_to_ref(const slang::SVInt& v, ir::Ref ty) {
    const uint32_t width = v.getBitWidth();
    // Fast path: a known value that fits a machine word.
    if (!v.hasUnknown() && width <= 64) {
        return module_.pool.put_int(ty, v.as<uint64_t>().value_or(0));
    }

    // General path: pack the raw words into val / xz planes. A wide (>64-bit)
    // value heap-allocates even when fully known (e.g. a packed string literal
    // like a 96-bit "hello world!"), so the fast path above must not swallow it
    // via a 64-bit truncation. getRawPtr lays out value words, followed by the
    // unknown words only when hasUnknown.
    const uint32_t        words = (width + 63) / 64;
    const uint64_t*       raw   = v.getRawPtr();
    const bool            unk   = v.hasUnknown();
    std::vector<uint64_t> planes(static_cast<size_t>(words) * 2, 0);
    for (uint32_t i = 0; i < words; ++i) {
        planes[i]         = unk ? (raw[i] ^ raw[words + i]) : raw[i]; // val plane (VPI)
        planes[words + i] = unk ? raw[words + i] : 0;                 // xz plane (unknown)
    }

    const uint32_t       pb = ir::plane_bytes(width);
    std::vector<uint8_t> bytes(static_cast<size_t>(pb) * 2, 0);
    std::memcpy(bytes.data(), planes.data(), pb);
    std::memcpy(bytes.data() + pb, planes.data() + words, pb);

    return module_.pool.put_value(ty, bytes);
}

// Lower a slang compile-time constant (a resolved parameter value or a
// folded constant-required expression) into a pooled IR constant.
//
// An aggregate constant has no single pooled value; a runtime reference to
// one is materialized as a constant table instead (const_table_signal).
ir::Ref Lowerer::lower_constant(const slang::ConstantValue& cv, const slang::ast::Type& type) {
    // Real / shortreal parameters (LRM 6.20), including untyped
    // `parameter r = 5.7`, whose type slang resolves to real.
    if (cv.isReal()) {
        return module_.pool.put_real(lower_type(type), static_cast<double>(cv.real()));
    }
    if (cv.isShortReal()) {
        return module_.pool.put_real(lower_type(type), static_cast<double>(cv.shortReal()));
    }
    if (cv.isString()) {
        // A `string` parameter (LRM 6.20.2) is the same value a string
        // literal folds to: its characters, interned once and assigned into
        // the reader's handle like any string constant.
        return string_value_ref(cv.str());
    }
    if (cv.isNullHandle()) {
        // The only elaboration-time value of a class / chandle / event type
        // (no object exists before simulation): the null handle, a 2-state
        // zero in the handle's own 64-bit slot.
        return module_.pool.put_int(lower_type(type), 0);
    }
    if (!cv.isInteger()) {
        fail("only integral, real, string and null constant parameters are supported");
    }
    return svint_to_ref(cv.integer(), lower_type(type));
}

// Append one integral constant's canonical plane bytes to `out`: the val
// plane, then the xz plane for a 4-state type. Same VPI convention and same
// packing as svint_to_ref, but writing into a larger image instead of
// interning a value of its own.
void Lowerer::append_svint_bytes(const slang::SVInt& src, ir::Ref ty, std::vector<uint8_t>& out) {
    const ir::Layout   lay = module_.pool.layout(ty);
    const slang::SVInt v   = src.getBitWidth() == lay.width ? src : src.resize(lay.width);

    const uint32_t        words = (lay.width + 63) / 64;
    const uint64_t*       raw   = v.getRawPtr();
    const bool            unk   = v.hasUnknown();
    std::vector<uint64_t> planes(static_cast<size_t>(words) * 2, 0);
    for (uint32_t i = 0; i < words; ++i) {
        planes[i]         = unk ? (raw[i] ^ raw[words + i]) : raw[i];
        planes[words + i] = unk ? raw[words + i] : 0;
    }

    const uint32_t pb    = ir::plane_bytes(lay.width);
    const size_t   start = out.size();
    out.resize(start + lay.byte_count, 0);
    std::memcpy(out.data() + start, planes.data(), pb);
    if (lay.domain == ir::Domain::Four) {
        std::memcpy(out.data() + start + pb, planes.data() + words, pb);
    }
}

// Flatten an elaboration constant into the canonical image of `ty`,
// leaf-major: an unpacked array is its elements' images back to back (what
// InternPool::compute_layout sizes an UnpackedArray value at), an unpacked
// struct is its fields' images in declaration order (the aggregate storage
// mirror's walk order — such a type has no pooled layout of its own, so the
// image is only ever read back through the same walk), a leaf its planes.
// Heap-backed leaves (string, queue, dynamic / associative array) have no
// static image at all and never reach here: const_table_signal routes a type
// containing one to the seeded path instead.
void Lowerer::append_const_image(const slang::ConstantValue& cv, ir::Ref ty, std::vector<uint8_t>& out) {
    const ir::Type& t = std::get<ir::Type>(module_.pool.get(ty));
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        const uint32_t count = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        if (!cv.isUnpacked() || cv.elements().size() != count) {
            fail("constant table value does not match its array type");
        }
        for (const slang::ConstantValue& e : cv.elements()) {
            append_const_image(e, t.element, out);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        const auto fields = module_.pool.struct_fields(t.fields_id);
        if (!cv.isUnpacked() || cv.elements().size() != fields.size()) {
            fail("constant table value does not match its struct type");
        }
        uint32_t f = 0;
        for (const slang::ConstantValue& e : cv.elements()) {
            append_const_image(e, fields[f++].type, out);
        }
        return;
    }

    const ir::Layout lay = module_.pool.layout(ty);
    if (lay.byte_count == 0) {
        fail("a constant table of this element type is not supported");
    }
    const size_t start = out.size();
    if (cv.isNullHandle()) {
        // The only elaboration-time value of a class / chandle leaf: a null
        // handle, which is the zero the resize below already wrote.
        out.resize(start + lay.byte_count, 0);
        return;
    }
    if (module_.pool.is_float(ty)) {
        if (!cv.isReal() && !cv.isShortReal()) {
            fail("constant table value does not match its element type");
        }
        const double d = cv.isReal() ? static_cast<double>(cv.real()) : static_cast<double>(cv.shortReal());
        out.resize(start + lay.byte_count, 0);
        if (lay.width == 64) {
            const uint64_t pat = std::bit_cast<uint64_t>(d);
            std::memcpy(out.data() + start, &pat, 8);
        } else {
            const uint32_t pat = std::bit_cast<uint32_t>(static_cast<float>(d));
            std::memcpy(out.data() + start, &pat, 4);
        }
        return;
    }
    if (!cv.isInteger()) {
        fail("constant table value does not match its element type");
    }
    append_svint_bytes(cv.integer(), ty, out);
}

// Does `ty` hold a heap-backed leaf (string / queue / dynamic array /
// associative array) anywhere? Such a value lives behind a runtime handle
// that only the runtime can construct, so a constant of this type cannot be
// baked into a static image and must be seeded at time zero instead.
bool Lowerer::has_heap_leaf(ir::Ref ty) {
    const ir::Type& t = std::get<ir::Type>(module_.pool.get(ty));
    switch (t.kind) {
        case ir::Type::Kind::String:
        case ir::Type::Kind::DynArray:
        case ir::Type::Kind::Queue:
        case ir::Type::Kind::Assoc:
            return true;
        case ir::Type::Kind::UnpackedArray:
            return has_heap_leaf(t.element);
        case ir::Type::Kind::Struct: {
            if (t.is_packed) {
                return false;
            }
            for (const ir::Field& f : module_.pool.struct_fields(t.fields_id)) {
                if (has_heap_leaf(f.type)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

// Can this type's value be carried by one element op (a queue push, an
// associative-array entry store)? A plane, a real, or a string handle can; an
// aggregate or nested container has to be built element-wise through an
// lvalue, which those ops do not take.
bool Lowerer::is_scalar_element(ir::Ref ty) {
    const ir::Type& t = std::get<ir::Type>(module_.pool.get(ty));
    switch (t.kind) {
        case ir::Type::Kind::UnpackedArray:
        case ir::Type::Kind::DynArray:
        case ir::Type::Kind::Queue:
        case ir::Type::Kind::Assoc:
            return false;
        case ir::Type::Kind::Struct:
            return t.is_packed;
        default:
            return true;
    }
}

// Write an elaborated constant into storage, one store per leaf, for a value
// whose type holds a heap-backed leaf (string / queue / dynamic array /
// associative array). Only the runtime can construct those handles, so such a
// constant table has no static image and is instead seeded once in the
// run-once PreInit process — the same shape a `const` variable declaration of
// the type already lowers to.
//
// It walks the elaborated ConstantValue rather than the parameter's
// initializer expression because the expression is not always the value: a
// defparam / config / command-line override installs a value with no
// initializer at all, and an instance override's expression is bound in the
// instantiating scope rather than the one being lowered (slang
// ParameterBuilder::createParam).
void Lowerer::emit_const_seed(ir::Index                   lhs,
                              ir::Ref                     ty,
                              const slang::ast::Type&     sv_type,
                              const slang::ConstantValue& cv,
                              ir::Builder&                b,
                              ir::SourceLoc               l) {
    // By value: interning a constant below can reallocate the pool's items.
    const ir::Type          t  = std::get<ir::Type>(module_.pool.get(ty));
    const slang::ast::Type& ct = sv_type.getCanonicalType();
    if (t.kind == ir::Type::Kind::UnpackedArray) {
        const uint32_t count = (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
        if (!cv.isUnpacked() || cv.elements().size() != count) {
            fail("constant table value does not match its array type");
        }
        const slang::ast::Type& et = *ct.getArrayElementType();
        for (uint32_t k = 0; k < count; ++k) {
            const int32_t coord =
              t.left <= t.right ? static_cast<int32_t>(t.left + k) : static_cast<int32_t>(t.left - k);
            const ir::Index idx = b.constant(
              module_.pool.put_int(make_int_type(32, false), static_cast<uint64_t>(static_cast<int64_t>(coord))), l);
            emit_const_seed(b.elem_select(lhs, idx, ty, l), t.element, et, cv.elements()[k], b, l);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Struct && !t.is_packed) {
        if (!cv.isUnpacked()) {
            fail("constant table value does not match its struct type");
        }
        uint32_t i = 0;
        for (const auto& f : ct.as<slang::ast::UnpackedStructType>().membersOfType<slang::ast::FieldSymbol>()) {
            if (i >= cv.elements().size()) {
                fail("constant table value does not match its struct type");
            }
            // Re-fetch the row each step: lowering a field can intern a new
            // struct's field table and invalidate a held span.
            const ir::Field fld = module_.pool.struct_fields(t.fields_id)[i];
            emit_const_seed(b.member_select(lhs, i, ty, l), fld.type, f.getType(), cv.elements()[i], b, l);
            ++i;
        }
        return;
    }
    if (t.kind == ir::Type::Kind::DynArray) {
        if (!cv.isUnpacked()) {
            fail("constant table value does not match its dynamic array type");
        }
        const auto              n  = static_cast<uint32_t>(cv.elements().size());
        const slang::ast::Type& et = *ct.getArrayElementType();
        const ir::Index         sz = b.constant(module_.pool.put_int(make_int_type(32, false), n), l);
        b.dyn_array_new(lhs, sz, lhs, /*has_init=*/false, ty, l);
        for (uint32_t k = 0; k < n; ++k) {
            const ir::Index idx = b.constant(module_.pool.put_int(make_int_type(32, false), k), l);
            emit_const_seed(b.elem_select(lhs, idx, ty, l), t.element, et, cv.elements()[k], b, l);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Queue) {
        if (!cv.isQueue()) {
            fail("constant table value does not match its queue type");
        }
        const slang::ast::Type& et = *ct.getArrayElementType();
        if (!is_scalar_element(t.element)) {
            // A push takes one element VALUE, so an element that is itself a
            // container or an aggregate would need a temp built per entry.
            fail("a constant queue of aggregate or container elements is not supported");
        }
        b.queue_delete(lhs, lhs, /*has_idx=*/false, ty, l);
        for (const slang::ConstantValue& e : *cv.queue()) {
            b.queue_push(lhs, b.constant(lower_constant(e, et), l), /*front=*/false, ty, l);
        }
        return;
    }
    if (t.kind == ir::Type::Kind::Assoc) {
        if (!cv.isMap()) {
            fail("constant table value does not match its associative array type");
        }
        const auto& at = ct.as<slang::ast::AssociativeArrayType>();
        if (at.hasWildcardIndexType()) {
            // TODO: `int a[*]` as a constant — a wildcard key has no declared
            // type to intern the key constant against, and the runtime keys on
            // the byte span, so a mismatched width would never be found.
            fail("a constant associative array with a wildcard index is not supported");
        }
        if (!is_scalar_element(t.element)) {
            fail("a constant associative array of aggregate or container elements is not supported");
        }
        b.assoc_reset(lhs, ty, l);
        const slang::ConstantValue& dflt = cv.map()->defaultValue;
        if (!dflt.bad()) {
            const ir::Index slot = b.assoc_default(lhs, ty, l);
            const ir::Index val  = b.constant(lower_constant(dflt, at.elementType), l);
            b.binary(ir::Inst::Tag::Store, slot, val, l);
        }
        for (const auto& [key, val] : *cv.map()) {
            // P4c (docs/design/assoc-keys.md): `lower_constant` cannot pool a
            // queue / dynamic-array / associative-array / string-in-struct key
            // as a compile-time constant (it has no static byte image -- that
            // is exactly why the Queue/DynArray/Assoc branches above build
            // those VALUES step by step instead of pooling them). Currently
            // unreachable: a parameter whose type needs one of those key
            // kinds is refused earlier by the general "only integral, real,
            // string and null constant parameters" restriction, independent
            // of this feature. Would need the same per-kind step-by-step
            // construction this function already gives queue / dynarray /
            // assoc VALUES, applied to the key instead, if that restriction
            // is ever lifted.
            const ir::Index k        = b.constant(lower_constant(key, *at.indexType), l);
            const ir::Index slot     = b.assoc_select(lhs, k, ty, l);
            const ir::Index elem_val = b.constant(lower_constant(val, at.elementType), l);
            b.binary(ir::Inst::Tag::Store, slot, elem_val, l);
        }
        return;
    }

    // A leaf: a plane value, a real, or a string handle — a Store of the
    // pooled constant, which the backend routes to a deep string assign when
    // the destination is a string.
    b.binary(ir::Inst::Tag::Store, lhs, b.constant(lower_constant(cv, sv_type), l), l);
}

// Register the Variable members of a package or `$unit` scope as signals
// (see lower_globals). Only `Variable` needs action here: nets can't
// legally appear at this scope (LRM 26.2 / A.1.2), and every other member
// kind (typedefs, parameters, functions/tasks, imports, classes, ...)
// either folds at each reference or lowers lazily at its own call/use site
// — including the slang-injected `std` package, whose class declarations
// Clotho does not yet support but which no design can reference without
// hitting that unsupported-type guard independently. `scope_` is set so
// an initializer expression (ASTContext-driven constant fold, format-arg
// eval, ...) has a scope to resolve against, matching every other
// signal-collection path.
void Lowerer::register_global_scope_signals(const Scope& scope) {
    scope_ = &scope;
    for (const Symbol& m : scope.members()) {
        if (m.kind != SymbolKind::Variable) {
            continue;
        }
        const auto& vs = m.as<VariableSymbol>();
        cur_range_     = slang::SourceRange{ vs.location, vs.location };

        ir::SignalInfo info;
        info.name     = std::string(vs.name);
        info.loc      = to_source_loc(vs.location);
        info.type     = lower_type(vs.getType());
        info.is_event = vs.getType().isEvent();
        info.dir      = ir::SignalInfo::Direction::Internal;

        sigmap_[&vs] = register_signal(std::move(info));
    }
}

// The declaration initializers of the same scope, as this Lowerer's pre-init
// process. Separate from registration because an initializer lowers arbitrary
// code -- a call whose body reads another global -- and must therefore run only
// once EVERY global scope is registered (lower_globals sequences the two).
void Lowerer::init_global_scope_signals(const Scope& scope) {
    scope_ = &scope;
    for (const Symbol& m : scope.members()) {
        if (m.kind != SymbolKind::Variable) {
            continue;
        }
        const auto& vs = m.as<VariableSymbol>();
        cur_range_     = slang::SourceRange{ vs.location, vs.location };
        lower_var_initializer(vs);
    }
}

// For each inout/ref port of `inst`, alias the child-side internal symbol to
// the parent connection's signal (LRM 23.3.3): both sides share one storage,
// so no copy process is wired (wire_ports skips them). Phase 1 requires the
// parent connection to be a whole signal (a single canonical net/variable);
// a bit-select / part-select / concat connection has no single collapse target
// and is rejected loud. An unconnected inout leaves the child net standalone.
//
// A `program`'s inout/ref port takes this same collapse, and the collapse is
// what makes it correct under LRM 24.3.2 with no region work at all: with one
// shared signal there is no copy process to place, the program writes that
// signal from its Reactive code, and "design processes sensitive to those
// cross-region variables and nets are scheduled for wake up in the active
// region set" is then just the ordinary wake path.
void Lowerer::build_inout_aliases(const InstanceSymbol& inst) {
    for (const PortConnection* pc : inst.getPortConnections()) {
        if (pc->port.kind != SymbolKind::Port) {
            continue;
        }
        const PortSymbol& p = pc->port.as<PortSymbol>();
        // An event port is an ALIASING port in every direction (LRM 15.5.5): a
        // port-connected event names the same sync object on both sides, so the
        // two symbols collapse onto one signal exactly as an inout does. Copying
        // the handle instead would be wrong -- a later merge on one side would
        // not be seen on the other -- and there is nothing to propagate, since
        // an event carries occurrences rather than a value.
        const bool event_port =
          p.internalSymbol != nullptr && p.internalSymbol->kind == SymbolKind::Variable &&
          p.internalSymbol->as<VariableSymbol>().getType().isEvent();
        if (!event_port && p.direction != slang::ast::ArgumentDirection::InOut &&
            p.direction != slang::ast::ArgumentDirection::Ref) {
            continue;
        }
        if (p.internalSymbol == nullptr) {
            continue;
        }
        const Expression* expr = pc->getExpression();
        if (expr == nullptr) {
            continue; // unconnected inout/ref: the child net stands alone
        }
        // slang may wrap the connection in an AssignmentExpression; the shared
        // net is the underlying value reference.
        const Expression& conn =
          expr->kind == ExpressionKind::Assignment ? expr->as<AssignmentExpression>().left() : *expr;
        if (conn.kind != ExpressionKind::NamedValue && conn.kind != ExpressionKind::HierarchicalValue) {
            // TODO: inout/ref port with a bit/part-select or concat connection
            // (per-bit collapse or resolved-net partial-driver slots).
            fail("inout/ref port with a non-whole-signal connection is not supported");
        }
        const ValueSymbol& parent      = canonical_value_symbol(conn.as<ValueExpressionBase>().symbol);
        inout_alias_[p.internalSymbol] = signal_of(parent);
    }
}

void Lowerer::collect_signals(const InstanceBodySymbol& body) {
    // First pass: record port directions by the symbol they back. Ports exist
    // only on the instance body, never inside a generate block.
    ankerl::unordered_dense::map<const Symbol*, ir::SignalInfo::Direction> port_dir;
    for (const Symbol& m : body.members()) {
        if (m.kind == SymbolKind::Port) {
            const PortSymbol& p = m.as<PortSymbol>();
            if (p.internalSymbol) {
                port_dir[p.internalSymbol] = direction_of(p.direction);
            }
        }
    }

    collect_scope_signals(body, prefix_, &port_dir);
}

// Register the nets and variables of one scope as signals under `prefix`,
// recursing through instantiated generate blocks/arrays (their nets carry the
// block's name in the path). `port_dir`, present only for an instance body,
// tags port-backed signals with their direction; generate-block signals are
// always internal. Child instances are handled by lower_instance, not here.
void Lowerer::collect_scope_signals(
  const Scope&                                                                  scope,
  const std::string&                                                            prefix,
  const ankerl::unordered_dense::map<const Symbol*, ir::SignalInfo::Direction>* port_dir) {
    for (const Symbol& m : scope.members()) {
        switch (m.kind) {
            case SymbolKind::Net:
            case SymbolKind::Variable: {
                const ValueSymbol& vs = m.as<ValueSymbol>();
                cur_range_            = slang::SourceRange{ vs.location, vs.location };

                // An inout/ref port internal aliased to its parent connection
                // (build_inout_aliases) shares that signal instead of getting
                // its own storage — the collapse that makes both sides one net.
                if (const auto ia = inout_alias_.find(&m); ia != inout_alias_.end()) {
                    sigmap_[&vs] = ia->second;
                    break;
                }

                ir::SignalInfo info;
                if (m.kind == SymbolKind::Net) {
                    const NetSymbol& net = m.as<NetSymbol>();
                    // Modeled net kinds: wire/tri (+ uwire, single-driver
                    // per slang's MultipleUWireDrivers pass) resolve to X on
                    // conflict; wand/triand and wor/trior carry wired-AND /
                    // wired-OR resolution (net_resolve_tag); tri0/tri1 carry a
                    // resistive pull to 0 / 1 (net_pull_value); supply0/supply1
                    // carry a Supply-strength pull. All Z-default when undriven,
                    // except tri0/tri1/supply0/supply1 which pull to 0 / 1.
                    // TODO: trireg carries charge semantics clotho does not model.
                    switch (net.netType.netKind) {
                        case NetType::Wire:
                        case NetType::Tri:
                        case NetType::UWire:
                        case NetType::WAnd:
                        case NetType::TriAnd:
                        case NetType::WOr:
                        case NetType::TriOr:
                        case NetType::Tri0:
                        case NetType::Tri1:
                        case NetType::Supply0:
                        case NetType::Supply1:
                            break;
                        default:
                            fail("unsupported net type (only wire/tri/uwire/wand/wor/tri0/tri1/"
                                 "supply0/supply1 are modeled)");
                    }
                    // A delay3 on the net_declaration syntax is either a
                    // true net delay (LRM 28.16, no net_decl_assignment) or,
                    // when the declaration also carries an assignment
                    // (`wire #5 w = expr;`), that ONE assignment's own
                    // continuous-assign delay (LRM 10.3.3's last paragraph:
                    // "shall not be added to the delay of other drivers on
                    // the net") — lowered by lower_net_initializer instead,
                    // exactly like a bare `assign #d`.
                    if (net.getDelay() != nullptr && net.getInitializer() == nullptr) {
                        if (net.getDelay()->kind != TimingControlKind::Delay) {
                            // TODO: rise/fall/turn-off net delay
                            // (`wire #(a,b,c) w;`), LRM 28.16 — same gap as
                            // the continuous-assign / gate delay triple.
                            fail("net delay rise/fall/turn-off triples (wire #(a,b,c)) are "
                                 "not supported");
                        }
                        if (is_aggregate(vs.getType())) {
                            // TODO: net delay on an unpacked-aggregate net —
                            // needs a leaf-wise inertial replay (like the
                            // intra-assignment aggregate commit), not the
                            // flat packed-plane resolver fold this forces
                            // every other net delay through.
                            fail("net delay on an unpacked-aggregate net is not supported");
                        }
                    }
                    info.is_net = true;
                }
                info.name     = prefix + std::string(vs.name);
                info.loc      = to_source_loc(vs.location);
                info.type     = lower_type(vs.getType());
                info.is_event = vs.getType().isEvent();
                info.dir      = ir::SignalInfo::Direction::Internal;
                if (port_dir != nullptr) {
                    const auto found = port_dir->find(&m);
                    if (found != port_dir->end()) {
                        info.dir = found->second;
                    }
                }
                if (info.dir != ir::SignalInfo::Direction::Internal &&
                    ir::types::contains_string(module_.pool, info.type)) {
                    // TODO: string module ports (needs string-aware port copies
                    // and change propagation). The predicate walks struct members
                    // and container elements as well as array dimensions, so a
                    // struct-with-string port and a queue-of-string port are
                    // caught too -- the old array-only walk let both through.
                    fail("string module ports are not supported, including a string "
                         "reached through a struct member or a container element");
                }

                const ir::Signal sig = register_signal(std::move(info));
                sigmap_[&vs]         = sig;
                if (m.kind == SymbolKind::Net) {
                    const NetSymbol&       nsym = m.as<NetSymbol>();
                    const NetType::NetKind nk   = nsym.netType.netKind;
                    // A tri0/tri1 / supply0/supply1 net always needs a resolver
                    // so its pull can fill undriven bits — even with zero or one
                    // driver. Record it here (its home scope) so a driverless one
                    // still emits.
                    if (const std::optional<int> pull = net_pull_value(nk)) {
                        net_pull_[static_cast<uint32_t>(sig)] =
                          PullInfo{ *pull, cur_scope_id(), to_source_loc(vs.location), net_pull_strength(nk) };
                    }
                    // A true net delay (validated above) applies to every
                    // driver's contribution to the net, after resolution —
                    // force the net onto the per-driver-slot path even with
                    // a single real driver (like `force`/cmos do), using
                    // this net kind's own fold op rather than assuming
                    // WireRes, so a delayed wand/wor net still resolves by
                    // value instead of by the wrong reducer.
                    if (nsym.getDelay() != nullptr && nsym.getInitializer() == nullptr) {
                        net_delay_[static_cast<uint32_t>(sig)] = &nsym.getDelay()->as<DelayControl>().expr;
                        if (resolved_nets_.insert(static_cast<uint32_t>(sig)).second) {
                            net_resolve_op_[static_cast<uint32_t>(sig)] = net_resolve_tag(nk);
                        }
                    }
                }
                break;
            }
            case SymbolKind::GenerateBlock: {
                const auto& gb = m.as<GenerateBlockSymbol>();
                if (!gb.isUninstantiated) {
                    collect_scope_signals(gb, prefix + gb.getExternalName() + ".", nullptr);
                }
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (const GenerateBlockSymbol* e : m.as<GenerateBlockArraySymbol>().entries) {
                    if (!e->isUninstantiated) {
                        collect_scope_signals(
                          *e, prefix + gen_entry_name(m.as<GenerateBlockArraySymbol>(), *e) + ".", nullptr);
                    }
                }
                break;
            default:
                break;
        }
    }
}

// Append a signal to the module and record it in the currently open
// hierarchy scope, and in signal_scope_, its own inverse: Signal ->
// declaring InstanceScope::id, which sampled_shadow_of uses to find a
// sampled variable's home scope regardless of which scope is currently
// open. Every signal-creating path funnels through here so the scope tree
// (and signal_scope_) cover the signal table exactly. scope_stack_ is
// empty only on the standalone Lowerer that registers package/$unit-scope
// state (globals; see lower_globals) and package-scope function statics
// reached through it (see function_id's is_global_subroutine delegation)
// — those signals belong to no top's InstanceScope tree (no signal_scope_
// entry either), and the backend already treats an unclaimed signal as a
// flat root-level field (build_state_types).
ir::Signal Lowerer::register_signal(ir::SignalInfo info) {
    const ir::Signal sig = module_.add_signal(std::move(info));
    if (!scope_stack_.empty()) {
        scope_stack_.back()->signals.push_back(sig);
        signal_scope_[static_cast<uint32_t>(sig)] = scope_stack_.back()->id;
    }

    return sig;
}

ir::Signal Lowerer::signal_of(const ValueSymbol& sym) {
    const ValueSymbol& csym = canonical_value_symbol(sym);
    if (csym.kind == SymbolKind::ModportPort) {
        // TODO: modport expression port with a structural connection
        // (`.name(sig[hi:lo])` or a concat, LRM 25.5.3) — maps to a bit
        // sub-range, not a whole signal, so it must lower as that
        // sub-lvalue. Plain renames resolve above.
        fail("modport expression port with a non-trivial connection "
             "expression is not supported");
    }
    const auto it = sigmap_.find(&csym);
    if (it != sigmap_.end()) {
        return it->second;
    }
    if (csym.kind == SymbolKind::ClassProperty &&
        csym.as<slang::ast::ClassPropertySymbol>().lifetime == slang::ast::VariableLifetime::Static) {
        // A static class property gets persistent module-signal storage
        // (declare_static_prop): eagerly for an initialized property, on first
        // access otherwise, and the value paths do that themselves. A reference
        // that arrives here instead --
        // `@(C::e)` on a static event property, which resolves through
        // event_signal -- must trigger the same registration rather than report
        // the property as unknown.
        return declare_static_prop(csym.as<slang::ast::ClassPropertySymbol>());
    }
    fail("reference to unknown signal '" + std::string(csym.name) + "'");
}

// Register a procedural local variable (a for-loop control variable or a
// block-local declaration) as backing signal storage, reusing the signal
// machinery for its loads/stores. Idempotent.
//
// NOTE: this gives the local static, signal-like storage rather than true
// per-entry automatic lifetime (12.7.1). That is correct for the
// non-recursive, single-instance processes the simulator models today, and
// matches how module variables are already handled.
ir::Signal Lowerer::declare_local(const ValueSymbol& vs) {
    const auto it = sigmap_.find(&vs);
    if (it != sigmap_.end()) {
        return it->second;
    }

    ir::SignalInfo info;
    info.name = prefix_ + std::string(vs.name);
    info.loc  = to_source_loc(vs.location);
    info.type = lower_type(vs.getType());
    // Every site that registers a *declared variable* must derive is_event from
    // the type: the backend allocates an event's trigger-timestamp global only
    // for a signal carrying the flag, so a cleared flag on an event makes
    // `-> e` / `e.triggered` store through a null global. A block-local
    // `event e;` reaches exactly here (declare_storage routes fn_ctx_ == null
    // here), so omitting it crashed the backend rather than failing loudly.
    info.is_event = vs.getType().isEvent();
    info.dir      = ir::SignalInfo::Direction::Internal;

    const ir::Signal sig = register_signal(std::move(info));
    sigmap_[&vs]         = sig;

    return sig;
}

// A parameter / specparam (LRM 6.20, 23.10) whose value a runtime index has
// to address: `localparam int P[0:3] = '{...}; x = P[i]`. Neither route an
// ordinary reference takes works — an elaboration constant has no storage to
// address, and a non-constant selector defeats the fold — so the value is
// materialized as a read-only table instead. Genvars and enum members are
// deliberately excluded: an enum member always folds, and a genvar's symbol
// is shared across loop iterations, so one baked value would be wrong.
bool Lowerer::is_const_table_symbol(const ValueSymbol& s) {
    return s.kind == SymbolKind::Parameter || s.kind == SymbolKind::Specparam;
}

// Materialize such a constant as a signal whose whole image is interned once
// (SignalInfo::const_data); the backend bakes it into the state field's
// static initializer, so the table costs no instructions at elaboration or
// at time zero and every reader is an ordinary indexed load. Idempotent.
//
// Registered unscoped, like package-scope state: the table belongs to no
// instance's hierarchy (nothing writes it, nothing dumps it, and its value is
// fixed at elaboration), and keeping it out of the InstanceScope tree leaves
// per-scope signal order — and so the backend's state layout — exactly as the
// Collect pass built it, which is what lets this run lazily during Emit.
ir::Signal Lowerer::const_table_signal(const ValueSymbol& vs) {
    const auto it = sigmap_.find(&vs);
    if (it != sigmap_.end()) {
        return it->second;
    }

    const slang::ConstantValue* cv = nullptr;
    if (const auto* p = vs.as_if<slang::ast::ParameterSymbol>()) {
        cv = &p->getValue();
    } else if (const auto* sp = vs.as_if<slang::ast::SpecparamSymbol>()) {
        cv = &sp->getValue();
    }
    if (cv == nullptr || cv->bad()) {
        fail("reference to unknown signal '" + std::string(vs.name) + "'");
    }

    const ir::Ref  ty     = lower_type(vs.getType());
    const bool     seeded = has_heap_leaf(ty);
    ir::SignalInfo info;
    info.name = prefix_ + std::string(vs.name);
    info.loc  = to_source_loc(vs.location);
    info.type = ty;
    info.dir  = ir::SignalInfo::Direction::Internal;
    if (!seeded) {
        std::vector<uint8_t> image;
        image.reserve(module_.pool.layout(ty).byte_count);
        append_const_image(*cv, ty, image);
        info.const_data = module_.pool.put_value(ty, image);
    }

    const ir::Signal sig = module_.add_signal(std::move(info));
    sigmap_[&vs]         = sig;

    if (seeded) {
        // The handles this value needs only exist at run time, so the table is
        // built once in PreInit instead of baked. PreInit stores service no
        // wait queue and run before every process, so the table is settled
        // before the first reader — including a declaration initializer that
        // reads it, whose own store is appended after these.
        if (!preinit_) {
            preinit_.emplace(module_);
        }
        ir::Builder&        pb = *preinit_;
        const ir::SourceLoc pl = to_source_loc(vs.location);
        emit_const_seed(pb.signal(sig, pl), ty, vs.getType(), *cv, pb, pl);
    }

    return sig;
}

// A static-lifetime function local: it persists across calls (LRM 13.4.2), so
// it gets persistent module-signal storage, initialized once at time zero —
// not the per-call frame. Named with the function qualifier to avoid clashes.
ir::Signal Lowerer::declare_static_local(const ValueSymbol& vs) {
    const auto it = sigmap_.find(&vs);
    if (it != sigmap_.end()) {
        return it->second;
    }
    ir::SignalInfo info;
    info.name            = prefix_ + fn_ctx_->name_prefix + std::string(vs.name);
    info.loc             = to_source_loc(vs.location);
    info.type            = lower_type(vs.getType());
    info.is_event        = vs.getType().isEvent(); // see declare_local
    info.dir             = ir::SignalInfo::Direction::Internal;
    const ir::Signal sig = register_signal(std::move(info));
    sigmap_[&vs]         = sig;

    return sig;
}

// A static class property (LRM 8.9): ONE slot shared across every instance of
// the class (and its derived classes), not part of the per-object body. Like a
// static function local — persistent module-signal storage, keyed by the
// property symbol, class-qualified name — but resolved wherever the property is
// referenced (`obj.p`, `C::p`, or a bare `p` in a method). Declared EAGERLY for
// every class the design elaborates (init_static_class_props): LRM 6.21 runs a
// static's initializer "only once at the beginning of simulation", reader or
// no reader, so waiting for an access site would silently skip a write-only
// property (UVM's whole factory registration). Access sites still land here
// first when they precede the eager pass (a package variable's initializer
// reading `C::x`); the sigmap_ cache is what makes the two paths meet at one
// signal and ONE queued initializer. The initializer runs at time zero via the
// PreInit process (LRM 8.9.2), like a module-level static variable.
ir::Signal Lowerer::declare_static_prop(const slang::ast::ClassPropertySymbol& prop) {
    const auto it = sigmap_.find(&prop);
    if (it != sigmap_.end()) {
        return it->second;
    }
    std::string cls;
    if (const slang::ast::Scope* sc = prop.getParentScope()) {
        cls = std::string(sc->asSymbol().name) + "::";
    }
    const ir::Ref  pty = lower_type(prop.getType());
    ir::SignalInfo info;
    info.name            = prefix_ + cls + std::string(prop.name);
    info.loc             = to_source_loc(prop.location);
    info.type            = pty;
    info.is_event        = prop.getType().isEvent(); // see declare_local
    info.dir             = ir::SignalInfo::Direction::Internal;
    const ir::Signal sig = register_signal(std::move(info));
    sigmap_[&prop]       = sig;

    // Queue the initializer once (this is the first-access path). LRM 8.9's own
    // example initializes a static property with `$fopen("data", "r")`, so this
    // is not restricted to constant expressions.
    if (const Expression* init = prop.getInitializer()) {
        if (!preinit_) {
            preinit_.emplace(module_);
        }
        ir::Builder&        pb = *preinit_;
        const ir::SourceLoc il = loc(*init);
        // A static property's initializer is class scope, not any method's
        // frame: LRM 8.11 permits `this` only inside a non-static method, so
        // nothing here may bind to a frame local or to an object. First access
        // can happen from inside an unrelated method body or another class's
        // property initializer, so drop both handles — a stray non-static
        // reference then fails loudly instead of silently reading whichever
        // frame / object happened to be mid-lowering.
        const Restore<FnCtx*>                  no_frame(fn_ctx_, nullptr);
        const Restore<std::optional<CtorThis>> no_ctor_this(ctor_this_, std::nullopt);
        const Restore<std::optional<CtorThis>> no_dflt_this(default_arg_this_, std::nullopt);
        if (is_aggregate(prop.getType()) || is_unpacked_container(prop.getType())) {
            // The shared slot is ordinary aggregate signal storage (register_signal
            // with the lowered type, exactly like a module-level variable), so an
            // unpacked array / queue / dynamic array / associative array
            // initializer builds into it through the same element-wise,
            // handle-aware path a module-level declaration initializer takes
            // (lower_var_initializer). A plain scalar Store would bit-blast the
            // container's handle instead.
            emit_struct_store(pb.signal(sig, il), pty, *init, ir::Inst::Tag::Store, pb, il);
        } else if (prop.getType().getCanonicalType().isString()) {
            // A string assigns as a whole handle, never through coerce: its
            // Resize rejects a string value outright (Resize truncates/extends
            // planes). Mirrors emit_class_property_inits' string case.
            const ir::Index dst = pb.signal(sig, il);
            const ir::Index val = lower_expr(*init, pb);
            pb.binary(ir::Inst::Tag::Store, dst, val, il);
        } else {
            const ir::Index dst = pb.signal(sig, il);
            const ir::Index val = coerce(lower_expr(*init, pb), *init->type, pty, pb, il);
            pb.binary(ir::Inst::Tag::Store, dst, val, il);
        }
    }
    return sig;
}

// True if `vs` is a static-lifetime procedural local, i.e. one whose storage
// persists across entries to its block or call: a module Signal in a process, a
// persistent Signal in a subroutine. A loop control variable is always treated as
// per-call (force_frame), since the loop initialization resets it on every entry
// regardless, and LRM 6.21 makes a declared loop variable automatic anyway.
//
// The lifetime is what decides WHEN the declaration initializer runs, so this is
// not conditioned on being inside a subroutine: `static int n = 0;` in an
// `initial` block's loop body initializes once before time zero (LRM 6.21's
// worked example prints 1..9 for it, not 1 2 3 three times), exactly like the
// same declaration inside a function.
bool Lowerer::is_static_local(const VariableSymbol& vs, bool force_frame) const {
    return !force_frame && vs.lifetime == slang::ast::VariableLifetime::Static;
}

// Ensure storage exists for a procedural local declaration / loop variable.
// Outside a function: a module Signal. Inside a function: a persistent Signal
// for a static local, else a per-call frame Local.
void Lowerer::declare_storage(const VariableSymbol& vs, bool force_frame) {
    // A variable living in a GC box has no storage of its own: value_lvalue routes
    // every access through the box, so a slot allocated here would never be read,
    // and in a process body a SIGNAL allocated here would sit in the design state
    // (and in a dump) never written. Covers a captured automatic and a fork-scope
    // declaration alike.
    if ((fn_ctx_ != nullptr && fn_ctx_->boxed.contains(&vs)) || proc_fork_boxes_.contains(&vs)) {
        return;
    }
    // A fixed unpacked array frame local is a per-call frame Local with the
    // canonical aggregate storage mirror (build_frame_layout); its managed leaves
    // are constructed by init_frame and released by deinit_body_strings, so a
    // string / container element needs nothing extra here. A static-lifetime
    // array local keeps persistent module-memory storage below.
    if (fn_ctx_ == nullptr) {
        declare_local(vs);
    } else if (is_static_local(vs, force_frame)) {
        declare_static_local(vs);
    } else {
        fn_local(vs);
    }
}

} // namespace clotho::lower
