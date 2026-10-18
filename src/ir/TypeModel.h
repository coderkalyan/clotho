#pragma once

#include "InternPool.h"

#include <cstdint>

// The single type-classification vocabulary shared by the frontend lowerer
// (src/Lower.cpp) and the LLVM backend (src/ir/LlvmBackend.cpp).
//
// Both sides used to carry their own near-identical predicate sets — five pairs
// were textual clones — so a change to one silently diverged from the other.
// Everything either side needs to know about a type's storage, copy semantics
// and lifetime is derived here, from `Type::Kind` alone.
//
// See docs/design/data-model.md for the model these functions encode.
namespace clotho::ir::types {

// Storage class of a type: the only classification the storage, copy, ABI and
// lifetime paths consult. Total over Type::Kind.
enum class StorageClass : uint8_t {
    // Valueless (a void function return, a void tagged-union member).
    Void,
    // One plane-packed slot: `{ i(2W) }`, `{ i(2W), iW }`, or narrow `{ iW }`.
    // Covers every integral kind, `real` / `shortreal`, a PACKED struct or
    // union (which is a single bit vector, LRM 7.2.1 / 7.3.1), and a class
    // handle or chandle (a 64-bit word with reference semantics).
    Plane,
    // A fixed-size POD handle embedded in its parent, owning at most one heap
    // allocation: `rt_string`, `rt_dynarr`, `rt_queue`, `rt_assoc`. Copy is a
    // deep copy or an ownership transfer; destruction is a deinit.
    Managed,
    // A recursive image with one slot per member / element, addressed by a GEP
    // path: an UNPACKED struct, or a fixed unpacked array.
    Aggregate,
};

inline const Type& type_of(const InternPool& pool, Ref ref) {
    return std::get<Type>(pool.get(ref));
}

inline StorageClass storage_class(const InternPool& pool, Ref ref) {
    switch (type_of(pool, ref).kind) {
        case Type::Kind::Void:
            return StorageClass::Void;
        case Type::Kind::String:
        case Type::Kind::DynArray:
        case Type::Kind::Queue:
        case Type::Kind::Assoc:
            return StorageClass::Managed;
        case Type::Kind::UnpackedArray:
            return StorageClass::Aggregate;
        case Type::Kind::Struct:
            // A packed struct / union is one bit vector; only the unpacked form
            // has per-member storage.
            return type_of(pool, ref).is_packed ? StorageClass::Plane : StorageClass::Aggregate;
        default:
            // Logic, Bit, PackedArray, Real, ShortReal, Class.
            return StorageClass::Plane;
    }
}

inline bool is_aggregate(const InternPool& pool, Ref ref) {
    return storage_class(pool, ref) == StorageClass::Aggregate;
}

// -- Exact-kind tests --

inline bool is_string(const InternPool& pool, Ref ref) {
    return type_of(pool, ref).kind == Type::Kind::String;
}
inline bool is_dynarr(const InternPool& pool, Ref ref) {
    return type_of(pool, ref).kind == Type::Kind::DynArray;
}
inline bool is_queue(const InternPool& pool, Ref ref) {
    return type_of(pool, ref).kind == Type::Kind::Queue;
}
inline bool is_assoc(const InternPool& pool, Ref ref) {
    return type_of(pool, ref).kind == Type::Kind::Assoc;
}

// A heap-backed CONTAINER handle (dynamic array, queue, associative array):
// the Managed kinds minus `string`. These share the by-pointer deep-copy
// subroutine ABI, carry a relocation epoch (docs/design/data-model.md §3.1),
// and hold elements described by an rt_elem_ops descriptor.
inline bool is_container(const InternPool& pool, Ref ref) {
    switch (type_of(pool, ref).kind) {
        case Type::Kind::DynArray:
        case Type::Kind::Queue:
        case Type::Kind::Assoc:
            return true;
        default:
            return false;
    }
}

// -- Recursive structure --

// Element count of one unpacked-array dimension.
inline uint32_t array_dim(const Type& t) {
    return (t.left >= t.right ? t.left - t.right : t.right - t.left) + 1;
}

// The non-array leaf type of a (possibly multidimensional) unpacked array;
// the type itself when it is not an unpacked array.
inline Ref leaf_type(const InternPool& pool, Ref ref) {
    const Type& t = type_of(pool, ref);
    return t.kind == Type::Kind::UnpackedArray ? leaf_type(pool, t.element) : ref;
}

// Total leaf count across every unpacked-array dimension; 1 for a non-array.
// This is the stride unit of the FLAT array storage model.
inline uint32_t array_leaf_count(const InternPool& pool, Ref ref) {
    const Type& t = type_of(pool, ref);
    if (t.kind != Type::Kind::UnpackedArray) {
        return 1;
    }
    return array_dim(t) * array_leaf_count(pool, t.element);
}

// Whether a type contains an UNPACKED STRUCT at any array nesting depth. Such a
// type has no flat bit image, so it always takes the recursive aggregate
// storage mirror. Narrower than is_agg_abi: false for a plain scalar-element
// array, which is flat in module storage but still passed by pointer.
inline bool contains_struct(const InternPool& pool, Ref ref) {
    const Type& t = type_of(pool, ref);
    if (t.kind == Type::Kind::UnpackedArray) {
        return contains_struct(pool, t.element);
    }
    return t.kind == Type::Kind::Struct && !t.is_packed;
}

// Whether a type crosses the subroutine boundary by pointer with a CANONICAL
// (never may-z-narrowed) layout: any fixed unpacked array, or an unpacked
// struct. Exactly StorageClass::Aggregate, named for the ABI role it plays.
inline bool is_agg_abi(const InternPool& pool, Ref ref) {
    return is_aggregate(pool, ref);
}

// Whether a type needs the recursive aggregate storage mirror rather than a
// flat plane slot or `[N x slot]`: it contains an unpacked struct, or an array
// dimension over a container element whose per-cell init/deinit/copy only the
// recursive walk provides.
inline bool needs_agg_storage(const InternPool& pool, Ref ref) {
    const Type& t = type_of(pool, ref);
    if (t.kind == Type::Kind::UnpackedArray) {
        return is_container(pool, t.element) || needs_agg_storage(pool, t.element);
    }
    return t.kind == Type::Kind::Struct && !t.is_packed;
}

// Whether a type owns heap anywhere — a Managed leaf at any struct / array
// nesting depth — so its storage needs a deinit walk and its copy cannot be a
// memcpy.
inline bool has_heap_leaf(const InternPool& pool, Ref ref) {
    const Type& t = type_of(pool, ref);
    switch (t.kind) {
        case Type::Kind::String:
        case Type::Kind::DynArray:
        case Type::Kind::Queue:
        case Type::Kind::Assoc:
            return true;
        case Type::Kind::UnpackedArray:
            return has_heap_leaf(pool, t.element);
        case Type::Kind::Struct: {
            if (t.is_packed) {
                return false;
            }
            for (const Field& f : pool.struct_fields(t.fields_id)) {
                if (has_heap_leaf(pool, f.type)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

// Whether a type has a leaf that must be CONSTRUCTED per storage location rather
// than copied from a constant power-on image: an event leaf binds a NEW event
// object (LRM Table 6-7), which no fixed byte pattern can express.
//
// Deliberately separate from has_heap_leaf: an event leaf owns no heap, needs no
// deinit, and IS trivially copyable -- copying the handle merges the two locations
// onto one object, which is exactly LRM 15.5.5's `a = b`. Only construction differs.
inline bool has_ctor_leaf(const InternPool& pool, Ref ref) {
    const Type& t = type_of(pool, ref);
    switch (t.kind) {
        case Type::Kind::Event:
            return true;
        case Type::Kind::UnpackedArray:
            return has_ctor_leaf(pool, t.element);
        case Type::Kind::Struct: {
            if (t.is_packed) {
                return false;
            }
            for (const Field& f : pool.struct_fields(t.fields_id)) {
                if (has_ctor_leaf(pool, f.type)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

// Whether a whole-object copy may be a single memcpy of the storage image: no
// leaf owns heap, so duplicating the bytes duplicates the value. A class-handle
// leaf IS trivially copyable — the pointer copy is the reference semantics.
//
// NOTE: distinct from trivial RELOCATABILITY, which every Clotho value type has
// (no self-pointers). The runtime containers memcpy-MOVE elements during ring
// growth and insert/delete shifts even for managed elements; only memcpy
// DUPLICATION is what this predicate gates.
inline bool is_trivially_copyable(const InternPool& pool, Ref ref) {
    return !has_heap_leaf(pool, ref);
}

// Whether a type is `string`, or an unpacked array whose leaf element is. This
// is the FLAT string-array storage question, not "does it contain a string" —
// see contains_string for that.
inline bool is_string_leaf_array(const InternPool& pool, Ref ref) {
    return is_string(pool, leaf_type(pool, ref));
}

// Whether a `string` appears anywhere in the type, through struct members,
// array dimensions and container elements alike.
//
// The predicate this replaces recursed through unpacked arrays ONLY, so a
// struct-with-string, a queue-of-string and an assoc-of-string all answered
// "no" — which is why the string port and string sensitivity guards used to
// miss those shapes entirely.
inline bool contains_string(const InternPool& pool, Ref ref) {
    const Type& t = type_of(pool, ref);
    switch (t.kind) {
        case Type::Kind::String:
            return true;
        case Type::Kind::UnpackedArray:
        case Type::Kind::DynArray:
        case Type::Kind::Queue:
            return contains_string(pool, t.element);
        case Type::Kind::Assoc:
            // The value element; a key with a string member is legal and is
            // handled by the key encoder, not by storage.
            return contains_string(pool, t.element);
        case Type::Kind::Struct: {
            if (t.is_packed) {
                return false;
            }
            for (const Field& f : pool.struct_fields(t.fields_id)) {
                if (contains_string(pool, f.type)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

} // namespace clotho::ir::types
