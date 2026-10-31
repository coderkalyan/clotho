// Associative arrays: key encoding and normalization, element access and methods.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// rt_elem_ops::keyhandles for an ASSOCIATIVE ARRAY descriptor: where the class
// handles are in one normalized key span, ascending. Empty for a key that
// cannot contain one.
//
// A bare class-handle key is the ONE-ENTRY case, `{ 0 }`, rather than a
// separate path: assoc_key_info reduces it to the integral case at width 64,
// and emit_key_bytes writes it at offset 0. A wildcard `[*]` key is never a
// handle -- its key type interns as a canonical unsigned integral placeholder
// -- and a `string` key holds no handle either.
//
// A VARIABLE key (info.is_variable) never reaches agg_key_handle_offsets: its
// offsets are compile-time constants only for a FIXED key, and the frontend
// refuses a class handle anywhere inside a variable one precisely so this
// table never has to describe a runtime-dependent offset (lower/Types.cpp
// guard_key_no_handle_in_variable).
std::vector<uint32_t> Backend::assoc_key_handle_offsets(Ref assoc_type) {
    assert(std::get<Type>(mod_.pool.get(assoc_type)).kind == Type::Kind::Assoc &&
           "only an associative array has a key");
    std::vector<uint32_t> out;
    const AssocKeyInfo info = assoc_key_info(assoc_type);
    if (info.wildcard || info.is_string || info.is_variable) {
        return out;
    }
    if (info.is_agg) {
        agg_key_handle_offsets(info.key_ty, 0, out);
    } else if (std::get<Type>(mod_.pool.get(info.key_ty)).kind == Type::Kind::Class) {
        out.push_back(0);
    }
    assert(std::ranges::is_sorted(out) && "the key offsets are ascending");
    assert(out.empty() || out.back() + 8 <= assoc_key_bytes(info) &&
           "a key handle lies inside its span");
    return out;
}

// Whether creating an entry in this assoc stores a CLASS HANDLE into its key
// storage. That is a reference the OWNING object acquires, which is why a write to
// an `int m[Cell]` -- or an `int m[struct{Cell c; int i;}]` -- held in a class
// property needs the write barrier even though the value it writes is an int (§7).
//
// One question, one answer: this reads the same table the collector decodes keys
// with, so the barrier and the tracer cannot disagree about which associative
// arrays have handle keys. `nkeyhandles != 0` is the single source of truth; there
// is deliberately no flag bit saying the same thing a second time.
bool Backend::assoc_key_is_class_handle(Ref assoc_type) {
    assert(std::get<Type>(mod_.pool.get(assoc_type)).kind == Type::Kind::Assoc &&
           "only an associative array has a key");
    return !assoc_key_handle_offsets(assoc_type).empty();
}

// True when `type`'s normalized form needs the variable-length rt_keybuf_*
// path: itself a string, or (through struct / fixed-array nesting) any leaf
// is a string / queue / dynamic array / associative array. Only reachable
// through an is_agg key today (guard_agg_key_leaves refuses a bare queue /
// dynarray / assoc key and a nested one alike until P2/P3 widen the admit
// list); written to classify either shape uniformly once they do.
bool Backend::key_type_is_variable(Ref type) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    switch (t.kind) {
        case Type::Kind::String:
        case Type::Kind::Queue:
        case Type::Kind::DynArray:
        case Type::Kind::Assoc:
            return true;
        case Type::Kind::Struct:
            if (t.is_packed) {
                return false;
            }
            for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
                if (key_type_is_variable(f.type)) {
                    return true;
                }
            }
            return false;
        case Type::Kind::UnpackedArray:
            return key_type_is_variable(t.element);
        default:
            return false;
    }
}

Backend::AssocKeyInfo Backend::assoc_key_info(Ref assoc_type) {
    const Type&  at = std::get<Type>(mod_.pool.get(assoc_type));
    AssocKeyInfo info;
    info.wildcard = at.right != 0;
    if (info.wildcard) {
        // Wildcard keys normalize to a fixed 64-bit unsigned big-endian form,
        // so numerically-equal indices of any width collapse to one entry.
        info.W = 64;
        return info;
    }
    const Ref key_ty = static_cast<Ref>(at.fields_id);
    info.key_ty      = key_ty;
    if (is_string_type(key_ty)) {
        info.is_string = true;
        return info;
    }
    const Type& kt = std::get<Type>(mod_.pool.get(key_ty));
    if (is_agg_abi(key_ty) || kt.kind == Type::Kind::Queue || kt.kind == Type::Kind::DynArray ||
        kt.kind == Type::Kind::Assoc) {
        // An unpacked struct / fixed unpacked array key, a queue / dynamic
        // array key itself (P2), or an associative-array key itself (P3):
        // serialized leaf by leaf / count-prefixed, never memcpy'd -- see
        // normalize_agg_key / normalize_variable_agg_key. A queue / dynarray /
        // assoc key is always classified variable (its element / entry count
        // has no compile-time bound), which key_type_is_variable already
        // reports for those three kinds.
        info.is_agg      = true;
        info.is_variable = key_type_is_variable(key_ty);
        return info;
    }
    const uint32_t w = mod_.pool.layout(key_ty).width;
    info.W           = w == 0 ? 1 : w;
    info.is_signed   = at.elem_signed;
    return info;
}

// Normalized length of an aggregate key: the sum over its leaves, each
// contributing ceil(W/8) bytes. A compile-time constant, so every key of one
// array has the same length (the runtime comparator's shorter-is-less tiebreak
// never fires) and one entry-block scratch alloca serves every call site.
uint32_t Backend::agg_key_bytes(Ref type) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t n = 0;
        for (const Field& f : mod_.pool.struct_fields(t.fields_id)) {
            n += agg_key_bytes(f.type);
        }
        return n;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        return array_dim(t) * agg_key_bytes(t.element);
    }
    const uint32_t w = mod_.pool.layout(type).width;
    return ((w == 0 ? 1 : w) + 7) / 8;
}

// Total normalized key length in bytes for a fixed-length key kind.
uint32_t Backend::assoc_key_bytes(const AssocKeyInfo& info) {
    return info.is_agg ? agg_key_bytes(info.key_ty) : (info.W + 7) / 8;
}

// Write `mustval` as `W`-bit big-endian bytes at byte `off` of `scratch` (byte
// 0 = most significant), sign-flipping the top bit for a signed key so unsigned
// lexicographic byte order equals numeric order.
void Backend::emit_key_bytes(llvm::Value*     mustval,
                             uint32_t         W,
                             bool             is_signed,
                             llvm::Value*     scratch,
                             llvm::ArrayType* arr_ty,
                             uint32_t         off) {
    llvm::Value* val = cast(mustval, W, false);
    if (is_signed) {
        val = b_.CreateXor(val, llvm::ConstantInt::get(int_ty(W), llvm::APInt::getSignedMinValue(W)));
    }
    const uint32_t nbytes = (W + 7) / 8;
    llvm::Value*   wide   = cast(val, nbytes * 8, false);
    for (uint32_t k = 0; k < nbytes; ++k) {
        const uint32_t shift = 8 * (nbytes - 1 - k);
        llvm::Value*   src =
          shift == 0 ? wide : b_.CreateLShr(wide, llvm::ConstantInt::get(int_ty(nbytes * 8), shift));
        b_.CreateStore(b_.CreateTrunc(src, b_.getInt8Ty()),
                       b_.CreateConstInBoundsGEP2_32(arr_ty, scratch, 0, off + k));
    }
}

// Normalize an integral (or class-handle) must-value to its fixed big-endian
// key bytes. Returns { i8* ptr, i32 len }.
std::pair<llvm::Value*, llvm::Value*> Backend::normalize_integral_key(llvm::Value* mustval, const AssocKeyInfo& info) {
    const uint32_t nbytes  = (info.W + 7) / 8;
    auto*          arr_ty  = llvm::ArrayType::get(b_.getInt8Ty(), nbytes);
    auto*          scratch = entry_alloca(arr_ty, "assoc.key");
    emit_key_bytes(mustval, info.W, info.is_signed, scratch, arr_ty, 0);
    return { scratch, b_.getInt32(nbytes) };
}

// Serialize an aggregate key's leaves into `scratch`, in declaration order
// (depth first), returning the next free byte offset and OR-ing each leaf's
// unknown-ness into `any_x`.
//
// The raw storage image is deliberately NOT used: agg_storage_ty builds a
// non-packed LLVM struct, so it has ABI padding holes, and a frame aggregate is
// a bare alloca whose padding is never written (emit_agg_init touches leaves
// only) -- two structs with equal members could differ byte for byte, which
// would break both the hash and the ordering. Per-leaf normalization gets the
// per-leaf signedness right, and gives the ordering LRM 7.8.5 leaves free:
// lexicographic by member in declaration order. A leaf's signedness is not in its
// integer type: it is the member's (Field::is_signed) or the array's element
// (Type::elem_signed), and `is_signed` carries it down to the leaf.
uint32_t Backend::emit_agg_key_bytes(Ref              type,
                                     const LValue&    lv,
                                     llvm::Value*     scratch,
                                     llvm::ArrayType* arr_ty,
                                     uint32_t         off,
                                     llvm::Value*&    any_x,
                                     bool             is_signed) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            off = emit_agg_key_bytes(fld.type, agg_member(lv, type, f), scratch, arr_ty, off, any_x, fld.is_signed);
            ++f;
        }
        return off;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        const uint32_t n = array_dim(t);
        for (uint32_t k = 0; k < n; ++k) {
            off = emit_agg_key_bytes(t.element, agg_elem(lv, type, k), scratch, arr_ty, off, any_x, t.elem_signed);
        }
        return off;
    }
    LValue leaf = lv;
    if (leaf.is_agg) {
        fill_agg_leaf(leaf, type);
        leaf.agg_leaf = true;
    }
    const Plane  pl  = lower_load(leaf);
    llvm::Value* ux  = any_unknown(pl);
    any_x            = any_x == nullptr ? ux : b_.CreateOr(any_x, ux);
    const uint32_t w = mod_.pool.layout(type).width;
    const uint32_t W = w == 0 ? 1 : w;
    emit_key_bytes(must_of(pl), W, is_signed, scratch, arr_ty, off);
    return off + (W + 7) / 8;
}

// Normalized span of an aggregate key held in `key_lv`.
Backend::AssocKeySpan Backend::normalize_agg_key(const LValue& key_lv, const AssocKeyInfo& info) {
    const uint32_t nbytes  = agg_key_bytes(info.key_ty);
    auto*          arr_ty  = llvm::ArrayType::get(b_.getInt8Ty(), nbytes);
    auto*          scratch = entry_alloca(arr_ty, "assoc.key");
    llvm::Value*   any_x   = nullptr;
    emit_agg_key_bytes(info.key_ty, key_lv, scratch, arr_ty, 0, any_x, info.is_signed);
    llvm::Value* valid = any_x == nullptr ? nullptr : b_.CreateNot(any_x);
    if (valid != nullptr && llvm::isa<llvm::Constant>(valid)) {
        valid = nullptr; // every leaf 2-state: statically valid
    }
    return { scratch, b_.getInt32(nbytes), valid };
}

// Append one aggregate key's leaves to the shared rt_keybuf_* scratch, in
// declaration order (depth first) -- the VARIABLE-length twin of
// emit_agg_key_bytes. A plain scalar leaf still writes fixed big-endian bytes
// (through a tiny per-leaf alloca, since rt_keybuf_append takes a pointer); a
// STRING leaf is length-prefixed (u32 big-endian length || bytes), since raw
// concatenation of two variable-length leaves is not injective (assoc-keys.md
// §3) -- a bare top-level string key keeps its own unprefixed encoding
// (normalize_assoc_key_value's info.is_string arm), since the two never mix
// in one span.
// `any_x_slot`: an i1* alloca the caller owns, ORed with this leaf's (or
// subtree's) unknown-ness. Memory-backed rather than an SSA value threaded
// by reference: a Queue / DynArray / Assoc leaf recurses inside a RUNTIME
// loop of unknown trip count, so a leaf's unknown-bit value is defined
// inside the loop body, which does not dominate the code after the loop --
// an SSA any_x carried past the loop that way is a verifier-rejected
// dominance violation (only latent for a 2-state leaf, whose any_unknown()
// is a compile-time constant with no defining block to violate). A plain
// load/store through the slot is correct regardless of trip count or
// nesting depth.
void Backend::emit_variable_agg_key_bytes(Ref type, const LValue& lv, llvm::Value* any_x_slot, bool is_signed) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            emit_variable_agg_key_bytes(fld.type, agg_member(lv, type, f), any_x_slot, fld.is_signed);
            ++f;
        }
        return;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        const uint32_t n = array_dim(t);
        for (uint32_t k = 0; k < n; ++k) {
            emit_variable_agg_key_bytes(t.element, agg_elem(lv, type, k), any_x_slot, t.elem_signed);
        }
        return;
    }
    if (t.kind == Type::Kind::String) {
        llvm::Value* sh  = lv_string_ptr(lv, StringUse::Read);
        llvm::Value* dat = b_.CreateCall(str_data_fn(), { sh });
        llvm::Value* len = cast(b_.CreateCall(str_len_fn(), { sh }), 32, false);
        auto*        lty = llvm::ArrayType::get(b_.getInt8Ty(), 4);
        auto*        lsc = entry_alloca(lty, "assoc.vkey.slen");
        emit_key_bytes(len, 32, false, lsc, lty, 0);
        b_.CreateCall(keybuf_append_fn(), { lsc, b_.getInt32(4) });
        b_.CreateCall(keybuf_append_fn(), { dat, len });
        return;
    }
    if (t.kind == Type::Kind::Queue || t.kind == Type::Kind::DynArray) {
        // P2: u32 be count || each element recursively normalized in its own
        // (queue: logical, ring-unwrapped) order -- element order is the
        // container's own identity (`==` on queues is element-wise, 11.4.5 /
        // 7.10), so this is canonical without needing a sort.
        llvm::Value*    handle    = lv_handle_ptr(lv, HandleUse::Read);
        const auto [len, data]    = container_len_base(handle, type);
        const DynElem   e         = dynarr_elem(type);
        llvm::Value*    count32   = cast(len, 32, false);
        auto*           cty       = llvm::ArrayType::get(b_.getInt8Ty(), 4);
        auto*           csc       = entry_alloca(cty, "assoc.vkey.qcount");
        emit_key_bytes(count32, 32, false, csc, cty, 0);
        b_.CreateCall(keybuf_append_fn(), { csc, b_.getInt32(4) });
        auto* idx_a = entry_alloca(b_.getInt64Ty(), "assoc.vkey.qidx");
        b_.CreateStore(b_.getInt64(0), idx_a);
        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* head = llvm::BasicBlock::Create(ctx_, "vkey.q.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx_, "vkey.q.body", fn);
        auto* done = llvm::BasicBlock::Create(ctx_, "vkey.q.done", fn);
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idx_a);
        b_.CreateCondBr(b_.CreateICmpULT(i, len), body, done);
        b_.SetInsertPoint(body);
        LValue elem_lv = container_elem_lvalue(handle, data, type, e, i);
        emit_variable_agg_key_bytes(t.element, elem_lv, any_x_slot, t.elem_signed);
        b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(), idx_a), b_.getInt64(1)), idx_a);
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
        return;
    }
    if (t.kind == Type::Kind::Assoc) {
        // P3: u32 be count || (key, value) pairs in the nested map's own
        // sorted-key order (rt_assoc_first/next), which is what makes this
        // canonical -- two equal-content maps produce identical bytes
        // regardless of insertion order, or key equality would break.
        //
        // Each pair is `u32 be klen || klen raw key bytes || value bytes`.
        // The key bytes are copied VERBATIM from the source entry rather than
        // re-normalized from a decoded value: the map already stores them in
        // exactly this span's own format, and copying is what lets denormalize
        // (P3's decode side) hand them straight to rt_assoc_put_ptr with no
        // decode/re-encode round trip. They still need their own length
        // prefix -- unlike every other leaf here, a BARE nested key of type
        // `string` is the one shape with no self-delimiting encoding of its
        // own (normalize_assoc_key_value's info.is_string arm keeps it raw and
        // unprefixed, for LRM 6.16.8 ordering), so klen cannot be inferred by
        // walking the copied bytes. The value bytes need no such prefix: its
        // OWN type-directed walk (this same function, recursively) is already
        // self-delimiting for every leaf kind, including a value-position
        // string (whose length prefix lives in the String arm above).
        llvm::Value*    handle  = lv_handle_ptr(lv, HandleUse::Read);
        llvm::Value*    count64 = b_.CreateCall(assoc_num_fn(), { handle });
        llvm::Value*    count32 = cast(count64, 32, false);
        auto*           cty     = llvm::ArrayType::get(b_.getInt8Ty(), 4);
        auto*           csc     = entry_alloca(cty, "assoc.vkey.acount");
        emit_key_bytes(count32, 32, false, csc, cty, 0);
        b_.CreateCall(keybuf_append_fn(), { csc, b_.getInt32(4) });

        auto* kptr_a = entry_alloca(raw_ptr_ty(), "assoc.vkey.akptr");
        auto* klen_a = entry_alloca(b_.getInt32Ty(), "assoc.vkey.aklen");
        llvm::Value* got0 = b_.CreateCall(assoc_end_fn(/*last=*/false), { handle, kptr_a, klen_a });
        auto*        have_a = entry_alloca(b_.getInt1Ty(), "assoc.vkey.ahave");
        b_.CreateStore(b_.CreateTrunc(got0, b_.getInt1Ty()), have_a);

        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* head = llvm::BasicBlock::Create(ctx_, "vkey.a.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx_, "vkey.a.body", fn);
        auto* done = llvm::BasicBlock::Create(ctx_, "vkey.a.done", fn);
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        b_.CreateCondBr(b_.CreateLoad(b_.getInt1Ty(), have_a), body, done);
        b_.SetInsertPoint(body);
        llvm::Value* kptr = b_.CreateLoad(raw_ptr_ty(), kptr_a);
        llvm::Value* klen = b_.CreateLoad(b_.getInt32Ty(), klen_a);
        auto*        lty2 = llvm::ArrayType::get(b_.getInt8Ty(), 4);
        auto*        lsc2 = entry_alloca(lty2, "assoc.vkey.aklenpfx");
        emit_key_bytes(klen, 32, false, lsc2, lty2, 0);
        b_.CreateCall(keybuf_append_fn(), { lsc2, b_.getInt32(4) });
        b_.CreateCall(keybuf_append_fn(), { kptr, klen });
        llvm::Value*  vslot = b_.CreateCall(assoc_get_ptr_fn(), { handle, kptr, klen });
        const DynElem ve    = dynarr_elem(type);
        LValue        vlv   = tmp_elem_lvalue(vslot, ve, b_.getInt64(0));
        emit_variable_agg_key_bytes(t.element, vlv, any_x_slot, /*is_signed=*/false);
        llvm::Value* got = b_.CreateCall(assoc_step_fn(/*prev=*/false), { handle, kptr, klen, kptr_a, klen_a });
        b_.CreateStore(b_.CreateTrunc(got, b_.getInt1Ty()), have_a);
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
        return;
    }
    // Every non-scalar leaf kind is handled above; what remains is a plain
    // scalar leaf (integral / class handle / event / chandle / virtual
    // interface / packed struct or union).
    LValue leaf = lv;
    if (leaf.is_agg) {
        fill_agg_leaf(leaf, type);
        leaf.agg_leaf = true;
    }
    const Plane  pl = lower_load(leaf);
    llvm::Value* ux = any_unknown(pl);
    b_.CreateStore(b_.CreateOr(b_.CreateLoad(b_.getInt1Ty(), any_x_slot), cast(ux, 1, false)), any_x_slot);
    const uint32_t w       = mod_.pool.layout(type).width;
    const uint32_t W       = w == 0 ? 1 : w;
    const uint32_t nbytes  = (W + 7) / 8;
    auto*           arr_ty = llvm::ArrayType::get(b_.getInt8Ty(), nbytes);
    auto*           scratch = entry_alloca(arr_ty, "assoc.vkey.leaf");
    emit_key_bytes(must_of(pl), W, is_signed, scratch, arr_ty, 0);
    b_.CreateCall(keybuf_append_fn(), { scratch, b_.getInt32(nbytes) });
}

// Normalized span of a VARIABLE aggregate key (docs/design/assoc-keys.md
// P1): built into the shared rt_keybuf_* scratch rather than a fixed alloca,
// since its length depends on runtime data (a string leaf's length, and --
// once P2/P3 land -- a container's element count). Self-contained: begin,
// build, read the span, pop, all in this one call, so the mark never
// survives past it -- see the LValue field comment on assoc_key_arg for why
// that matters (a deferred a[k] place can resolve its key twice).
Backend::AssocKeySpan Backend::normalize_variable_agg_key(const LValue& key_lv, const AssocKeyInfo& info) {
    llvm::Value* mark      = b_.CreateCall(keybuf_begin_fn(), {});
    llvm::Value* any_x_slot = entry_alloca(b_.getInt1Ty(), "assoc.vkey.anyx");
    b_.CreateStore(b_.getInt1(false), any_x_slot);
    emit_variable_agg_key_bytes(info.key_ty, key_lv, any_x_slot, info.is_signed);
    llvm::Value* ptr_slot = entry_alloca(raw_ptr_ty(), "assoc.vkey.ptr");
    llvm::Value* len_slot = entry_alloca(b_.getInt32Ty(), "assoc.vkey.len");
    b_.CreateCall(keybuf_span_fn(), { mark, ptr_slot, len_slot });
    llvm::Value* ptr = b_.CreateLoad(raw_ptr_ty(), ptr_slot);
    llvm::Value* len = b_.CreateLoad(b_.getInt32Ty(), len_slot);
    b_.CreateCall(keybuf_end_fn(), { mark });
    // Not folded to null even when every leaf is provably 2-state (the
    // memory-backed accumulator loses that compile-time fact) -- the JIT's
    // own optimization pass folds the resulting always-false gate away.
    llvm::Value* valid = b_.CreateNot(b_.CreateLoad(b_.getInt1Ty(), any_x_slot));
    return { ptr, len, valid };
}

// The i1 validity of an integral key plane: false when it holds an x/z bit
// (LRM 7.8.4). Null when the key is 2-state, so nothing is gated.
llvm::Value* Backend::integral_key_valid(Plane pl) {
    if (pl.two || pl.mm == nullptr) {
        return nullptr; // 2-state (or a real, which the frontend rejects as a key)
    }
    llvm::Value* ux = any_unknown(pl);
    // A LITERAL x/z key (e.g. `a[8'hxx]`) folds any_unknown() to a compile-
    // time constant TRUE, not false -- "it's a Constant" alone does not mean
    // "statically valid" (P4b: this used to wrongly treat that as always-
    // valid, so `a[8'hxx] = 7; a[8'hzz] = 8;` silently merged into ONE
    // entry via the all-zero must-plane span instead of dropping both).
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ux)) {
        return c->isZero() ? nullptr : b_.getInt1(false); // statically good / statically invalid
    }
    return b_.CreateNot(ux);
}

// Normalized key span for a key operand of a[k] / exists / delete. An
// aggregate key arrives as an lvalue (there is no aggregate plane value);
// everything else as a value.
Backend::AssocKeySpan Backend::normalize_assoc_key_value(Index key, Ref assoc_type) {
    const AssocKeyInfo info = assoc_key_info(assoc_type);
    if (info.is_string) {
        llvm::Value* sh = str_of(key);
        return { b_.CreateCall(str_data_fn(), { sh }),
                 cast(b_.CreateCall(str_len_fn(), { sh }), 32, false),
                 nullptr };
    }
    if (info.is_agg) {
        return info.is_variable ? normalize_variable_agg_key(lvalue_of(key), info)
                                : normalize_agg_key(lvalue_of(key), info);
    }
    const Plane pl    = value_of(key);
    auto [kptr, klen] = normalize_integral_key(must_of(pl), info);
    return { kptr, klen, integral_key_valid(pl) };
}

// Normalized key span for the CURRENT value of a key VARIABLE lvalue (next /
// prev traversal).
Backend::AssocKeySpan Backend::normalize_assoc_key_lvalue(const LValue& key_lv, Ref assoc_type) {
    const AssocKeyInfo info = assoc_key_info(assoc_type);
    if (info.is_string) {
        llvm::Value* sh = lv_string_ptr(key_lv, StringUse::Read);
        return { b_.CreateCall(str_data_fn(), { sh }),
                 cast(b_.CreateCall(str_len_fn(), { sh }), 32, false),
                 nullptr };
    }
    if (info.is_agg) {
        return info.is_variable ? normalize_variable_agg_key(key_lv, info) : normalize_agg_key(key_lv, info);
    }
    const Plane pl    = lower_load(key_lv);
    auto [kptr, klen] = normalize_integral_key(must_of(pl), info);
    return { kptr, klen, integral_key_valid(pl) };
}

// Write a key span (kptr / klen) back into the key-variable lvalue (foreach /
// first / next readout): inverse of the normalization above.
void Backend::denormalize_assoc_key(llvm::Value*  kptr,
                                    llvm::Value*  klen,
                                    Ref           assoc_type,
                                    const LValue& key_lv,
                                    Index         at) {
    const AssocKeyInfo info = assoc_key_info(assoc_type);
    if (info.is_string) {
        llvm::Value* snap = str_chg_snapshot(key_lv);
        b_.CreateCall(str_assign_bytes_fn(), { lv_string_ptr(key_lv, StringUse::Write), kptr,
                                               cast(klen, 64, false),
                                               alloc_site_global(loc_text(at), "string") });
        str_chg_after(key_lv, snap);
        return;
    }
    if (info.is_agg) {
        if (info.is_variable) {
            llvm::Value* off = b_.getInt32(0);
            denormalize_variable_agg_key(info.key_ty, key_lv, kptr, off, at, info.is_signed);
        } else {
            uint32_t off = 0;
            denormalize_agg_key(info.key_ty, key_lv, kptr, off, info.is_signed);
        }
        return;
    }
    lower_store(key_lv, known_plane(denormalize_key_word(kptr, 0, info.W, info.is_signed, key_lv.store_width),
                                    !key_lv.narrow));
}

// Rebuild one leaf value from `nbytes` big-endian key bytes at `off`: reassemble,
// un-flip the sign bit, then resize to the destination slot's width.
llvm::Value* Backend::denormalize_key_word(llvm::Value* kptr,
                                           uint32_t     off,
                                           uint32_t     W,
                                           bool         is_signed,
                                           uint32_t     store_width) {
    const uint32_t nbytes = (W + 7) / 8;
    llvm::Value*   wide   = zero(nbytes * 8);
    for (uint32_t k = 0; k < nbytes; ++k) {
        llvm::Value* byte =
          b_.CreateLoad(b_.getInt8Ty(), b_.CreateInBoundsGEP(b_.getInt8Ty(), kptr, b_.getInt32(off + k)));
        llvm::Value* contrib =
          b_.CreateShl(cast(byte, nbytes * 8, false),
                       llvm::ConstantInt::get(int_ty(nbytes * 8), 8 * (nbytes - 1 - k)));
        wide = b_.CreateOr(wide, contrib);
    }
    llvm::Value* val = cast(wide, W, false);
    if (is_signed) {
        val = b_.CreateXor(val, llvm::ConstantInt::get(int_ty(W), llvm::APInt::getSignedMinValue(W)));
    }
    return cast(val, store_width, is_signed);
}

// Inverse of emit_agg_key_bytes: one store per leaf, in the same declaration
// order, so a `foreach` / `first` / `next` key variable of aggregate type reads
// back exactly the key that was inserted.
void Backend::denormalize_agg_key(Ref type, const LValue& lv, llvm::Value* kptr, uint32_t& off, bool is_signed) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            denormalize_agg_key(fld.type, agg_member(lv, type, f), kptr, off, fld.is_signed);
            ++f;
        }
        return;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        const uint32_t n = array_dim(t);
        for (uint32_t k = 0; k < n; ++k) {
            denormalize_agg_key(t.element, agg_elem(lv, type, k), kptr, off, t.elem_signed);
        }
        return;
    }
    LValue leaf = lv;
    if (leaf.is_agg) {
        fill_agg_leaf(leaf, type);
        leaf.agg_leaf = true;
    }
    const uint32_t w = mod_.pool.layout(type).width;
    const uint32_t W = w == 0 ? 1 : w;
    lower_store(leaf, known_plane(denormalize_key_word(kptr, off, W, is_signed, leaf.store_width), !leaf.narrow));
    off += (W + 7) / 8;
}

// Inverse of emit_variable_agg_key_bytes: the VARIABLE-length twin of
// denormalize_agg_key. `off` is a RUNTIME byte offset (a string leaf's
// encoded length is only known once decoded), so each leaf is read through a
// fresh GEP off `kbase` rather than the fixed helper's compile-time offset --
// `denormalize_key_word` still does the big-endian reassembly / sign-unflip,
// just relative to that GEP instead of `kbase` directly.
void Backend::denormalize_variable_agg_key(Ref           type,
                                           const LValue& lv,
                                           llvm::Value*  kbase,
                                           llvm::Value*& off,
                                           Index         at,
                                           bool          is_signed) {
    const Type& t = std::get<Type>(mod_.pool.get(type));
    if (t.kind == Type::Kind::Struct && !t.is_packed) {
        uint32_t f = 0;
        for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
            denormalize_variable_agg_key(fld.type, agg_member(lv, type, f), kbase, off, at, fld.is_signed);
            ++f;
        }
        return;
    }
    if (t.kind == Type::Kind::UnpackedArray) {
        const uint32_t n = array_dim(t);
        for (uint32_t k = 0; k < n; ++k) {
            denormalize_variable_agg_key(t.element, agg_elem(lv, type, k), kbase, off, at, t.elem_signed);
        }
        return;
    }
    llvm::Value* leafptr = b_.CreateInBoundsGEP(b_.getInt8Ty(), kbase, off);
    if (t.kind == Type::Kind::String) {
        llvm::Value* len32  = denormalize_key_word(leafptr, 0, 32, false, 32);
        llvm::Value* datptr = b_.CreateInBoundsGEP(b_.getInt8Ty(), leafptr, b_.getInt32(4));
        LValue       leaf   = lv;
        if (leaf.is_agg) {
            fill_agg_leaf(leaf, type);
            leaf.agg_leaf = true;
        }
        llvm::Value* snap = str_chg_snapshot(leaf);
        b_.CreateCall(str_assign_bytes_fn(), { lv_string_ptr(leaf, StringUse::Write), datptr,
                                               cast(len32, 64, false),
                                               alloc_site_global(loc_text(at), "string") });
        str_chg_after(leaf, snap);
        off = b_.CreateAdd(off, b_.CreateAdd(b_.getInt32(4), len32));
        return;
    }
    if (t.kind == Type::Kind::Queue || t.kind == Type::Kind::DynArray) {
        // P2: inverse of the count-prefixed emit_variable_agg_key_bytes
        // branch. Materializes into a scratch LINEAR (dynarray-shaped)
        // handle first -- `tmp_elem_lvalue` addresses it with no ring wrap,
        // regardless of whether the destination is a queue or a dynamic
        // array -- then moves (dynarray destination) or deep-copies (queue
        // destination, which has no bulk "allocate n empty elements"
        // primitive of its own) into the actual destination handle.
        llvm::Value*    n64 = cast(denormalize_key_word(leafptr, 0, 32, false, 32), 64, false);
        llvm::Constant* ops = elem_ops_global(type);
        const DynElem   e   = dynarr_elem(type);
        auto* tmp = entry_alloca(dynarr_slot_ty(), "assoc.vkey.qtmp");
        b_.CreateStore(llvm::Constant::getNullValue(dynarr_slot_ty()), tmp);
        b_.CreateCall(dynarr_new_fn(),
                     { tmp, ops, n64, alloc_site_global(loc_text(at), "assoc key traversal") });
        llvm::Value* tdata = b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(dynarr_slot_ty(), tmp, 0));

        // The element bytes start right after the 4-byte count prefix; a
        // running offset threaded through the loop, since a variable-shaped
        // element (a nested string / container) has no fixed stride to skip.
        auto* eoff_a = entry_alloca(b_.getInt32Ty(), "assoc.vkey.qeoff");
        b_.CreateStore(b_.CreateAdd(off, b_.getInt32(4)), eoff_a);
        auto* idx_a = entry_alloca(b_.getInt64Ty(), "assoc.vkey.qidx");
        b_.CreateStore(b_.getInt64(0), idx_a);

        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* head = llvm::BasicBlock::Create(ctx_, "vkeyd.q.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx_, "vkeyd.q.body", fn);
        auto* done = llvm::BasicBlock::Create(ctx_, "vkeyd.q.done", fn);
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idx_a);
        b_.CreateCondBr(b_.CreateICmpULT(i, n64), body, done);
        b_.SetInsertPoint(body);
        llvm::Value* eoff     = b_.CreateLoad(b_.getInt32Ty(), eoff_a);
        LValue       elem_lv  = tmp_elem_lvalue(tdata, e, i);
        denormalize_variable_agg_key(t.element, elem_lv, kbase, eoff, at, t.elem_signed);
        b_.CreateStore(eoff, eoff_a);
        b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(), idx_a), b_.getInt64(1)), idx_a);
        b_.CreateBr(head);
        b_.SetInsertPoint(done);

        llvm::Value* dst = lv_handle_ptr(lv, HandleUse::Mutate);
        if (t.kind == Type::Kind::DynArray) {
            b_.CreateCall(dynarr_move_fn(), { dst, tmp, ops });
        } else {
            b_.CreateCall(queue_assign_dynarr_fn(),
                         { dst, tmp, ops, alloc_site_global(loc_text(at), "assoc key traversal") });
            b_.CreateCall(dynarr_delete_fn(), { tmp, ops });
        }
        off = b_.CreateLoad(b_.getInt32Ty(), eoff_a);
        return;
    }
    if (t.kind == Type::Kind::Assoc) {
        // P3: inverse of the count-prefixed `(key, value)` pair emitter.
        // Materialization needs no decode-then-renormalize round trip for the
        // key half: the copied bytes are already a valid normalized span for
        // the nested map's OWN key type (that's what makes the encoder's
        // verbatim copy correct in the first place), so they go straight to
        // rt_assoc_put_ptr. Only the value half is decoded, directly into the
        // slot that call returns.
        llvm::Value*    count32 = denormalize_key_word(leafptr, 0, 32, false, 32);
        llvm::Value*    count64 = cast(count32, 64, false);
        llvm::Constant* ops     = elem_ops_global(type); // the nested map's VALUE ops
        llvm::Value*    dst     = lv_handle_ptr(lv, HandleUse::Mutate);
        b_.CreateCall(assoc_reset_fn(), { dst, ops }); // drop stale entries + default first

        auto* eoff_a = entry_alloca(b_.getInt32Ty(), "assoc.vkey.aeoff");
        b_.CreateStore(b_.CreateAdd(off, b_.getInt32(4)), eoff_a);
        auto* idx_a = entry_alloca(b_.getInt64Ty(), "assoc.vkey.aidx");
        b_.CreateStore(b_.getInt64(0), idx_a);

        auto* fn   = b_.GetInsertBlock()->getParent();
        auto* head = llvm::BasicBlock::Create(ctx_, "vkeyd.a.head", fn);
        auto* body = llvm::BasicBlock::Create(ctx_, "vkeyd.a.body", fn);
        auto* done = llvm::BasicBlock::Create(ctx_, "vkeyd.a.done", fn);
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        llvm::Value* i = b_.CreateLoad(b_.getInt64Ty(), idx_a);
        b_.CreateCondBr(b_.CreateICmpULT(i, count64), body, done);
        b_.SetInsertPoint(body);
        llvm::Value* eoff      = b_.CreateLoad(b_.getInt32Ty(), eoff_a);
        llvm::Value* klenptr   = b_.CreateInBoundsGEP(b_.getInt8Ty(), kbase, eoff);
        llvm::Value* klen      = denormalize_key_word(klenptr, 0, 32, false, 32);
        llvm::Value* kbytesptr = b_.CreateInBoundsGEP(b_.getInt8Ty(), klenptr, b_.getInt32(4));
        llvm::Value* slot      = b_.CreateCall(assoc_put_ptr_fn(),
                                               { dst, ops, kbytesptr, klen,
                                                 alloc_site_global(loc_text(at), "assoc key traversal") });
        llvm::Value*  voff = b_.CreateAdd(b_.CreateAdd(eoff, b_.getInt32(4)), klen);
        const DynElem ve   = dynarr_elem(type);
        LValue        vlv  = tmp_elem_lvalue(slot, ve, b_.getInt64(0));
        denormalize_variable_agg_key(t.element, vlv, kbase, voff, at, /*is_signed=*/false);
        b_.CreateStore(voff, eoff_a);
        b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(), idx_a), b_.getInt64(1)), idx_a);
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
        off = b_.CreateLoad(b_.getInt32Ty(), eoff_a);
        return;
    }
    // Every non-scalar leaf kind is handled above; see emit_variable_agg_key_bytes.
    LValue leaf = lv;
    if (leaf.is_agg) {
        fill_agg_leaf(leaf, type);
        leaf.agg_leaf = true;
    }
    const uint32_t w = mod_.pool.layout(type).width;
    const uint32_t W = w == 0 ? 1 : w;
    lower_store(leaf, known_plane(denormalize_key_word(leafptr, 0, W, is_signed, leaf.store_width), !leaf.narrow));
    off = b_.CreateAdd(off, b_.getInt32((W + 7) / 8));
}

// Resolve an assoc element lvalue (deferred in AssocSelect) to a concrete
// element lvalue over the heap slot: get_ptr for a read (absent -> the
// dyn_inrange=false default path, no entry created), put_ptr for a write
// (creates + proto-inits the entry).
//
// The resolver is shared with lower_agg_eq, which recurses over aggregate
// leaves without an Index, so no instruction location reaches here and an
// entry-creating allocation reports "<unknown>".
LValue Backend::assoc_elem_lvalue(const LValue& lv, bool for_write) {
    const SourceText at{ .where = "<unknown>" };
    // The key is normalized HERE, fresh for every resolution, rather than once
    // at AssocSelect-lowering time -- see the LValue field comment for why: a
    // span cached across two resolutions of one deferred place (`a[k] += 1`
    // reads via get_ptr, then writes via put_ptr, with the read/store's own
    // operand evaluation free to build and pop OTHER keys in between) could be
    // overwritten before the second resolution reads it. Building it fresh
    // keeps every rt_keybuf_begin/append/span/end call self-contained: the span
    // it returns is consumed by exactly the one runtime call below and nothing
    // else. The pattern-default slot has no key at all.
    llvm::Value* kptr   = nullptr;
    llvm::Value* klen   = nullptr;
    llvm::Value* kvalid = nullptr;
    if (!lv.assoc_default) {
        const AssocKeySpan span = normalize_assoc_key_value(lv.assoc_key_arg, lv.assoc_key_type);
        kptr                    = span.ptr;
        klen                    = span.len;
        kvalid                  = span.valid;
        emit_assoc_key_bounds_check(kvalid, lv, for_write);
    }
    // The resolved slot pointer. The pattern-default slot (LRM 7.9.11) always
    // exists once asked for, so it needs neither the read/write split nor a
    // found-check; a keyed entry resolves through put_ptr on a write (creates)
    // and get_ptr on a read (null when absent and no default is set).
    // A class-handle KEY is stored into the map's key bytes when put_ptr creates an
    // entry (:1759-1767 spells out the encoding), so the owner acquires a reference
    // that the VALUE path knows nothing about -- `int m[Cell]` in a class property
    // writes an int and still needs the barrier (§7). Emitted here, on the write
    // resolution, because this is the one place an entry is created; a read
    // (get_ptr) adds no reference and gets nothing. assoc_key_handle is false unless
    // the map is reached through a class property AND its key is a bare handle.
    if (for_write && lv.assoc_key_handle) {
        emit_gc_barrier_for(lv);
    }
    // A write resolution creates or replaces an entry, so it mutates a waited-on
    // map property (LRM 9.4.3). This is the ONE place an assoc element place is
    // resolved -- a[k] = v, a[k].push_back(x), a `ref` to a[k], the '{default: v}
    // slot -- so it is the one place the notify belongs. Firing here rather than
    // after the store is deliberate and matches notify_container_mutation's
    // contract: a woken activation cannot run before this process suspends.
    if (for_write) {
        notify_container_mutation(lv);
    }
    const auto slot_ptr = [&]() -> llvm::Value* {
        if (lv.assoc_default) {
            return b_.CreateCall(assoc_default_ptr_fn(),
                                 { lv.assoc_handle, lv.assoc_ops,
                                   alloc_site_global(at, "associative array") });
        }
        if (for_write) {
            if (kvalid == nullptr) {
                return b_.CreateCall(assoc_put_ptr_fn(),
                                     { lv.assoc_handle, lv.assoc_ops, kptr, klen,
                                       alloc_site_global(at, "associative array") });
            }
            // An invalid (x/z) key must not even allocate the entry, or
            // `num`/`exists` would see a key the write was supposed to drop
            // (LRM 7.8.6). Branch around put_ptr and hand back a null slot; the
            // store itself is gated by the same predicate below.
            auto* fn    = b_.GetInsertBlock()->getParent();
            auto* putbb = llvm::BasicBlock::Create(ctx_, "assoc.key.ok", fn);
            auto* cont  = llvm::BasicBlock::Create(ctx_, "assoc.key.cont", fn);
            auto* from  = b_.GetInsertBlock();
            b_.CreateCondBr(kvalid, putbb, cont);
            b_.SetInsertPoint(putbb);
            llvm::Value* p = b_.CreateCall(assoc_put_ptr_fn(),
                                          { lv.assoc_handle, lv.assoc_ops, kptr, klen,
                                            alloc_site_global(at, "associative array") });
            auto* putend = b_.GetInsertBlock();
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
            auto* phi = b_.CreatePHI(raw_ptr_ty(), 2);
            phi->addIncoming(llvm::Constant::getNullValue(raw_ptr_ty()), from);
            phi->addIncoming(p, putend);
            return phi;
        }
        if (lv.assoc_elem_type != Ref::Invalid &&
            types::has_ctor_leaf(mod_.pool, lv.assoc_elem_type)) {
            // The element's Table 7-1 default is a NEW event, which is not a
            // constant image the load path can materialize -- and returning a
            // null handle instead would be wrong twice over: `aa[absent] == null`
            // must read 0 (LRM 15.5.5.3), and `@(aa[absent])` must block rather
            // than be undefined.
            //
            // So an absent key resolves to the array's default slot, which
            // rt_elem_init binds to one event when it is first asked for. That
            // event is reachable only through absent-key reads of this array, so
            // triggering it wakes exactly the processes that waited the same way.
            // LRM 7.9.11 does not say whether two distinct absent keys yield the
            // same event or different ones; one per array is chosen because a
            // fresh object per READ would allocate without bound.
            auto* fn    = b_.GetInsertBlock()->getParent();
            auto* miss  = llvm::BasicBlock::Create(ctx_, "assoc.absent", fn);
            auto* cont  = llvm::BasicBlock::Create(ctx_, "assoc.found", fn);
            llvm::Value* got = b_.CreateCall(assoc_get_ptr_fn(), { lv.assoc_handle, kptr, klen });
            auto* from = b_.GetInsertBlock();
            b_.CreateCondBr(b_.CreateICmpEQ(got, llvm::Constant::getNullValue(raw_ptr_ty())),
                            miss, cont);
            b_.SetInsertPoint(miss);
            llvm::Value* def = b_.CreateCall(assoc_default_ptr_fn(),
                                             { lv.assoc_handle, lv.assoc_ops,
                                               alloc_site_global(at, "associative array") });
            auto* missend = b_.GetInsertBlock();
            b_.CreateBr(cont);
            b_.SetInsertPoint(cont);
            auto* phi = b_.CreatePHI(raw_ptr_ty(), 2);
            phi->addIncoming(got, from);
            phi->addIncoming(def, missend);
            return phi;
        }
        return b_.CreateCall(assoc_get_ptr_fn(), { lv.assoc_handle, kptr, klen });
    };
    // A found-check is only meaningful for a key read: everything else yields a
    // live slot.
    const bool checked = !lv.assoc_default && !for_write;
    // Aggregate value (unpacked struct / fixed array): the resolved slot is
    // the flat aggregate image. The is_agg lvalue (root_ty + agg_path + leaf
    // facts) was built by the MemberSelect / ElemSelect descent; only the base
    // is deferred. A read of an absent key finds no slot -- the found-check
    // (ANDed with any array-bound gate from a runtime element index, and with
    // the key-validity gate) drives dyn_elem_load to the per-leaf element
    // default (Table 7-1), no entry created.
    llvm::Value* slot    = slot_ptr();
    llvm::Value* nonnull = checked
                             ? b_.CreateICmpNE(slot, llvm::Constant::getNullValue(raw_ptr_ty()))
                             : nullptr;
    const auto   gate    = [&](llvm::Value* base_gate) -> llvm::Value* {
        if (base_gate == nullptr) {
            return kvalid;
        }
        return kvalid != nullptr ? b_.CreateAnd(kvalid, base_gate) : base_gate;
    };
    if (lv.is_agg) {
        LValue el      = lv;
        el.is_assoc    = false;
        el.is_signal   = false;
        el.base        = slot;
        el.dyn_inrange = gate(nonnull);
        return el;
    }
    LValue el;
    el.base        = slot;
    el.dyn_inrange = gate(nonnull);
    // An entry's slot dies with the entry, so a `ref` formed through it carries
    // the map's relocation epoch exactly as a queue element does — and whatever
    // containers the map itself sits inside, ahead of it.
    el.epoch_chain = lv.epoch_chain;
    // The owner travels with the resolved element: a handle stored into an assoc
    // entry lands in the map's malloc'd payload, which only the OWNING object's
    // trace map reaches (Gc.h:324-326), so the store still needs the barrier (§7).
    el.gc_owner    = lv.gc_owner;
    // Root and owner travel together; see agg_member for the invariant.
    el.gc_container_root = lv.gc_container_root;
    el.gc_container_type = lv.gc_container_type;
    // Managed value elements (string / nested container): the resolved slot
    // IS the handle. put_ptr proto-inits a fresh entry (owned-empty string /
    // null container), so a write never sees null; a read of an absent key
    // returns null, which the string sink redirect / handle default handles.
    if (lv.assoc_val_string || lv.assoc_val_handle) {
        el.is_string = lv.assoc_val_string;
        el.is_handle = lv.assoc_val_handle;
        el.slot_ty   = lv.slot_ty;
        // The sink redirect lv_handle_ptr / lv_string_ptr apply when the key is
        // absent needs the element's own type to pick the empty image.
        el.handle_type = lv.handle_type;
        return el;
    }
    el.slot_ty     = lv.slot_ty;
    el.has_z       = lv.has_z;
    el.narrow      = lv.narrow;
    el.array_ty    = llvm::ArrayType::get(lv.slot_ty, 0);
    el.elem_idx    = b_.getInt32(0);
    el.store_width = lv.store_width;
    // Carry the SELECTION the chain accumulated on the deferred lvalue rather
    // than resetting it. `a[k]` can have a bit or part select stacked on top --
    // a packed-struct or packed-union member of the value element lowers to a
    // PartSelect -- and its offset, width and in-range mask all live here.
    // Zeroing bit_off silently read the value's low bits instead of the member.
    el.bit_off          = lv.bit_off;
    el.width            = lv.width;
    el.range_mask       = lv.range_mask;
    el.range_mask_outer = lv.range_mask_outer;
    el.nest_valid       = lv.nest_valid;
    el.wide_dyn         = lv.wide_dyn;
    el.sel_base         = lv.sel_base;
    el.sel_cwidth       = lv.sel_cwidth;
    el.sel_unknown      = lv.sel_unknown;
    return el;
}

void Backend::lower_assoc_num(Index i) {
    const auto&  p   = ir_->payload(i).op_type;
    llvm::Value* n   = b_.CreateCall(assoc_num_fn(), { lv_handle_ptr(lvalue_of(p.op), HandleUse::Read) });
    set_value(i, two_val(cast(n, 32, false)));
}

// a.exists(k) (LRM 7.9.1). An invalid (x/z) key names no entry, so it answers 0
// without probing the map.
void Backend::lower_assoc_exists(Index i) {
    const auto&  p    = ir_->payload(i).op_extra;
    const auto   aa   = ir_->extra_data<AssocArg>(p.extra);
    const auto   span = normalize_assoc_key_value(aa.arg, aa.assoc_type);
    llvm::Value* r =
      b_.CreateCall(assoc_exists_fn(), { lv_handle_ptr(lvalue_of(p.op), HandleUse::Read), span.ptr, span.len });
    if (span.valid != nullptr) {
        r = b_.CreateSelect(span.valid, r, zero(32));
    }
    set_value(i, two_val(cast(r, 32, false)));
}

// a.delete(k) (LRM 7.9.2). An invalid key names no entry, so the call is skipped
// entirely rather than deleting some other entry's normalized bytes.
void Backend::lower_assoc_delete_key(Index i) {
    const auto& p    = ir_->payload(i).op_extra;
    const auto  aa   = ir_->extra_data<AssocArg>(p.extra);
    const auto  span = normalize_assoc_key_value(aa.arg, aa.assoc_type);
    const auto  call = [&] {
        b_.CreateCall(assoc_delete_key_fn(),
                      { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                        elem_ops_global(aa.assoc_type),
                        span.ptr,
                        span.len });
    };
    if (span.valid == nullptr) {
        call();
        return;
    }
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* dobb = llvm::BasicBlock::Create(ctx_, "assoc.del.do", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "assoc.del.cont", fn);
    b_.CreateCondBr(span.valid, dobb, cont);
    b_.SetInsertPoint(dobb);
    call();
    b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
}

// a.delete() (LRM 7.9.2): drop every entry but keep the pattern default,
// which 7.9.11 makes independent of the array's contents.
void Backend::lower_assoc_delete_all(Index i) {
    const auto& p = ir_->payload(i).op_type;
    b_.CreateCall(assoc_delete_all_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate), elem_ops_global(p.type) });
}

// Whole-array replacement by a `'{...}` literal: entries and default both go.
void Backend::lower_assoc_reset(Index i) {
    const auto& p = ir_->payload(i).op_type;
    emit_handle_delete(lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate), p.type);
}

void Backend::lower_assoc_copy(Index i) {
    const auto& p  = ir_->payload(i).op_extra;
    const auto  aa = ir_->extra_data<AssocArg>(p.extra);
    // Whole-map assign: every entry's key and value is copied in, so the owner
    // acquires every reference the source held. See lower_queue_push.
    emit_gc_barrier_for(lvalue_of(p.op));
    b_.CreateCall(assoc_assign_fn(),
                  { lv_handle_ptr(lvalue_of(p.op), HandleUse::Mutate),
                    lv_handle_ptr(lvalue_of(aa.arg), HandleUse::Read),
                    elem_ops_global(aa.assoc_type),
                    alloc_site_global(loc_text(i), "associative array") });
}

// first / last / next / prev (LRM 7.9.3/7.9.4): write the ordered key into
// the key-variable lvalue and yield the 0/1 found status.
void Backend::lower_assoc_traverse(Index i, Inst::Tag tag) {
    const auto&  p      = ir_->payload(i).op_extra;
    const auto   aa     = ir_->extra_data<AssocArg>(p.extra);
    llvm::Value* handle = lv_handle_ptr(lvalue_of(p.op), HandleUse::Place);
    const LValue key_lv = lvalue_of(aa.arg);
    auto*        outptr = entry_alloca(raw_ptr_ty(), "assoc.kp");
    auto*        outlen = entry_alloca(b_.getInt32Ty(), "assoc.kl");
    llvm::Value* status = nullptr;
    if (tag == Inst::Tag::AssocFirst || tag == Inst::Tag::AssocLast) {
        status = b_.CreateCall(assoc_end_fn(tag == Inst::Tag::AssocLast),
                               { handle, outptr, outlen });
    } else {
        // next / prev take the current key as a PROBE (LRM 7.9.4 allows one that
        // is not present), so an x/z probe is normalized as its must bits rather
        // than gated -- the clause defines no invalid-probe behaviour.
        const auto cur = normalize_assoc_key_lvalue(key_lv, aa.assoc_type);
        status =
          b_.CreateCall(assoc_step_fn(tag == Inst::Tag::AssocPrev), { handle, cur.ptr, cur.len, outptr, outlen });
    }
    auto* got  = b_.CreateICmpNE(status, zero(32));
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* dobb = llvm::BasicBlock::Create(ctx_, "assoc.trav.do", fn);
    auto* cont = llvm::BasicBlock::Create(ctx_, "assoc.trav.cont", fn);
    b_.CreateCondBr(got, dobb, cont);
    b_.SetInsertPoint(dobb);
    // first / last / next / prev WRITE the found key into the key variable, and a
    // key can be a class handle (`int m[Cell]`, :1759-1767), so this is a
    // class-handle store that never passes through Tag::Store -- `m.first(this.k)`
    // puts a handle straight into a class property. No key-type test is needed: an
    // owner survives agg_member only for a member that reaches a class handle, so a
    // non-empty gc_owner here already proves the key variable can hold one (§7).
    // Inside the found block, so a miss writes nothing and logs nothing.
    emit_gc_barrier_for(key_lv);
    denormalize_assoc_key(b_.CreateLoad(raw_ptr_ty(), outptr),
                          b_.CreateLoad(b_.getInt32Ty(), outlen), aa.assoc_type, key_lv, i);
    b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
    set_value(i, two_val(cast(status, 32, false)));
}

} // namespace clotho::ir::backend
