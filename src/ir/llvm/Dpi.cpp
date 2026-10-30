// DPI-C: import and export marshalling and open arrays.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// 32-bit chunk count of a width-W packed vector (the svBitVecVal / svLogicVecVal
// element count).
uint32_t Backend::dpi_nchunks(uint32_t w) {
    return (w + 31) / 32;
}

// Spill an iW plane into a fresh entry-block buffer of `nchunks` 32-bit words
// (zero-extended above W), returned as a pointer the runtime reads as uint32*.
llvm::Value* Backend::dpi_spill_words(llvm::Value* plane, uint32_t nchunks) {
    auto* bufty = int_ty(nchunks * 32);
    auto* buf   = entry_alloca(bufty, "dpi.words");
    b_.CreateStore(b_.CreateZExtOrTrunc(plane, bufty), buf);
    return buf;
}

// Load an iW value back from an `nchunks`-word buffer.
llvm::Value* Backend::dpi_load_words(llvm::Value* buf, uint32_t nchunks, uint32_t w) {
    return b_.CreateTrunc(b_.CreateLoad(int_ty(nchunks * 32), buf), int_ty(w));
}

// A fresh svVecVal scratch: nchunks 32-bit words for a bit vector, 2*nchunks
// (interleaved {aval,bval}) for a logic vector.
llvm::Value* Backend::dpi_vec_alloca(DpiCType ct, uint32_t nchunks) {
    return entry_alloca(int_ty((ct == DpiCType::BitVec ? 1u : 2u) * nchunks * 32), "dpi.vec");
}

// Fill an svVecVal scratch from a value Plane (input build / inout copy-in).
void Backend::dpi_vec_copyin(DpiCType ct, Plane pl, uint32_t w, llvm::Value* scratch) {
    const uint32_t nc = dpi_nchunks(w);
    if (ct == DpiCType::BitVec) {
        // svBitVecVal: the (2-state) value plane chunked is already the layout.
        b_.CreateStore(b_.CreateZExtOrTrunc(must_of(pl), int_ty(nc * 32)), scratch);
        return;
    }
    // svLogicVecVal: spill must/may/z; the runtime computes aval/bval + interleave.
    llvm::Value* zv = (pl.two || pl.z == nullptr) ? zero(w) : pl.z;
    b_.CreateCall(dpi_pack_logicvec_fn(), { dpi_spill_words(must_of(pl), nc),
                                            dpi_spill_words(may_of(pl), nc),
                                            dpi_spill_words(zv, nc), b_.getInt32(nc), scratch });
}

// Reconstruct a value Plane from an svVecVal scratch (output / inout copy-out).
Plane Backend::dpi_vec_copyout(DpiCType ct, llvm::Value* scratch, uint32_t w) {
    const uint32_t nc = dpi_nchunks(w);
    if (ct == DpiCType::BitVec) {
        return two_val(dpi_load_words(scratch, nc, w));
    }
    llvm::Value* must = entry_alloca(int_ty(nc * 32), "dpi.must");
    llvm::Value* may  = entry_alloca(int_ty(nc * 32), "dpi.may");
    llvm::Value* zv   = entry_alloca(int_ty(nc * 32), "dpi.z");
    b_.CreateCall(dpi_unpack_logicvec_fn(), { scratch, b_.getInt32(nc), must, may, zv });
    return packed(dpi_load_words(must, nc, w), dpi_load_words(may, nc, w),
                  dpi_load_words(zv, nc, w));
}

// Element GEP into a marshalled svVecVal buffer: `[N*nch x elem]` where elem is
// `{ i32, i32 }` (svLogicVecVal) or `i32` (svBitVecVal). `flat` is the element's
// flat position k*nch + c.
llvm::Value* Backend::dpi_oa_buf_elem(llvm::Value* buf, uint32_t total, bool is_logic, llvm::Value* flat) {
    auto* i32   = b_.getInt32Ty();
    auto* elem  = is_logic ? static_cast<llvm::Type*>(llvm::StructType::get(ctx_, { i32, i32 }))
                           : static_cast<llvm::Type*>(i32);
    auto* arrty = llvm::ArrayType::get(elem, total);
    return b_.CreateInBoundsGEP(arrty, buf, { b_.getInt32(0), flat });
}

// Inline per-element pack (design `[N x slot]` storage -> svVecVal buffer) for a
// marshalled open-array element, any width. N is a compile-time constant (fixed
// unpacked-array actual), so this is a constant-trip loop; each element loads the
// native iW / i2W slot fields and LLVM legalizes the ops at any width — the
// unaligned `may` region (mm bits [W, 2W)) is a single `lshr i2W, W`, no
// hand-rolled funnel shift, and no separate C marshal to keep in sync.
void Backend::emit_dpi_oa_pack_ir(llvm::Value*      storage,
                                  llvm::Value*      buf,
                                  llvm::StructType* slot,
                                  uint32_t          n,
                                  uint32_t          ew,
                                  uint32_t          nch,
                                  bool              has_z,
                                  bool              is_logic) {
    auto* iW    = int_ty(ew);
    auto* i32   = b_.getInt32Ty();
    auto* arrty = llvm::ArrayType::get(slot, n);
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "oapk.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "oapk.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "oapk.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);
    b_.SetInsertPoint(head);
    auto* k = b_.CreatePHI(i32, 2);
    k->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(k, b_.getInt32(n)), body, done);
    b_.SetInsertPoint(body);

    auto*        slotptr = b_.CreateInBoundsGEP(arrty, storage, { b_.getInt32(0), k });
    llvm::Value* aval    = nullptr; // iW: value-or-X
    llvm::Value* bval    = nullptr; // iW: X-or-Z (null for 2-state)
    if (is_logic) {
        auto* i2w  = int_ty(2 * ew);
        auto*        mm = b_.CreateLoad(i2w, b_.CreateStructGEP(slot, slotptr, 0));
        llvm::Value* z  = has_z ? static_cast<llvm::Value*>(
                                    b_.CreateLoad(iW, b_.CreateStructGEP(slot, slotptr, 1)))
                                : static_cast<llvm::Value*>(llvm::ConstantInt::get(iW, 0));
        auto* must = b_.CreateTrunc(mm, iW);
        auto* may  = b_.CreateTrunc(b_.CreateLShr(mm, llvm::ConstantInt::get(i2w, ew)), iW);
        aval       = b_.CreateAnd(may, b_.CreateNot(z));
        bval       = b_.CreateXor(must, may);
    } else {
        // 2-state: the value is the low W bits of field 0 (narrow iW, or the must
        // region of a non-narrow i2W) — mirrors the C pack_bit low-W read.
        auto* raw = b_.CreateLoad(slot->getElementType(0), b_.CreateStructGEP(slot, slotptr, 0));
        aval      = b_.CreateZExtOrTrunc(raw, iW);
    }
    auto* base = b_.CreateMul(k, b_.getInt32(nch));
    for (uint32_t c = 0; c < nch; ++c) {
        auto* flat  = b_.CreateAdd(base, b_.getInt32(c));
        auto* eptr  = dpi_oa_buf_elem(buf, n * nch, is_logic, flat);
        auto* shamt = llvm::ConstantInt::get(iW, 32u * c); // 32c < ew, never poison
        // ZExtOrTrunc, not Trunc: iW may be narrower than i32 (w < 32).
        auto* ac    = b_.CreateZExtOrTrunc(b_.CreateLShr(aval, shamt), i32);
        if (is_logic) {
            auto* i32x2 = llvm::StructType::get(ctx_, { i32, i32 });
            auto* bc    = b_.CreateZExtOrTrunc(b_.CreateLShr(bval, shamt), i32);
            b_.CreateStore(ac, b_.CreateStructGEP(i32x2, eptr, 0));
            b_.CreateStore(bc, b_.CreateStructGEP(i32x2, eptr, 1));
        } else {
            b_.CreateStore(ac, eptr);
        }
    }
    k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);
    b_.SetInsertPoint(done);
}

// Inline per-element unpack (svVecVal buffer -> design storage) — the reverse of
// emit_dpi_oa_pack_ir, for an output/inout element of any width. Reassembles
// aval/bval from the chunks, reconstructs mm = must | (may << W) with a single
// `shl i2W`, and writes the z field when present.
void Backend::emit_dpi_oa_unpack_ir(llvm::Value*      buf,
                                    llvm::Value*      storage,
                                    llvm::StructType* slot,
                                    uint32_t          n,
                                    uint32_t          ew,
                                    uint32_t          nch,
                                    bool              has_z,
                                    bool              is_logic) {
    auto* iW    = int_ty(ew);
    auto* i32   = b_.getInt32Ty();
    auto* arrty = llvm::ArrayType::get(slot, n);
    auto* fn    = b_.GetInsertBlock()->getParent();
    auto* head  = llvm::BasicBlock::Create(ctx_, "oaupk.head", fn);
    auto* body  = llvm::BasicBlock::Create(ctx_, "oaupk.body", fn);
    auto* done  = llvm::BasicBlock::Create(ctx_, "oaupk.done", fn);
    auto* entry = b_.GetInsertBlock();
    b_.CreateBr(head);
    b_.SetInsertPoint(head);
    auto* k = b_.CreatePHI(i32, 2);
    k->addIncoming(b_.getInt32(0), entry);
    b_.CreateCondBr(b_.CreateICmpULT(k, b_.getInt32(n)), body, done);
    b_.SetInsertPoint(body);

    auto*        i32x2 = llvm::StructType::get(ctx_, { i32, i32 });
    auto*        base  = b_.CreateMul(k, b_.getInt32(nch));
    llvm::Value* aval  = llvm::ConstantInt::get(iW, 0);
    llvm::Value* bval  = llvm::ConstantInt::get(iW, 0);
    for (uint32_t c = 0; c < nch; ++c) {
        auto* flat  = b_.CreateAdd(base, b_.getInt32(c));
        auto* eptr  = dpi_oa_buf_elem(buf, n * nch, is_logic, flat);
        auto* shamt = llvm::ConstantInt::get(iW, 32u * c);
        auto* ac    = is_logic ? b_.CreateLoad(i32, b_.CreateStructGEP(i32x2, eptr, 0))
                               : b_.CreateLoad(i32, eptr);
        // ZExtOrTrunc, not ZExt: iW may be narrower than i32 (w < 32), and the
        // trunc also drops the chunk's undefined bits above W.
        aval = b_.CreateOr(aval, b_.CreateShl(b_.CreateZExtOrTrunc(ac, iW), shamt));
        if (is_logic) {
            auto* bc = b_.CreateLoad(i32, b_.CreateStructGEP(i32x2, eptr, 1));
            bval     = b_.CreateOr(bval, b_.CreateShl(b_.CreateZExtOrTrunc(bc, iW), shamt));
        }
    }
    auto* slotptr = b_.CreateInBoundsGEP(arrty, storage, { b_.getInt32(0), k });
    if (is_logic) {
        auto* i2w  = int_ty(2 * ew);
        auto* must = b_.CreateAnd(aval, b_.CreateNot(bval));
        auto* may  = b_.CreateOr(aval, bval);
        auto* mm   = b_.CreateOr(b_.CreateZExt(must, i2w),
                                 b_.CreateShl(b_.CreateZExt(may, i2w), llvm::ConstantInt::get(i2w, ew)));
        b_.CreateStore(mm, b_.CreateStructGEP(slot, slotptr, 0));
        if (has_z) {
            auto* z = b_.CreateAnd(bval, b_.CreateNot(aval));
            b_.CreateStore(z, b_.CreateStructGEP(slot, slotptr, 1));
        }
    } else {
        b_.CreateStore(b_.CreateZExtOrTrunc(aval, slot->getElementType(0)),
                       b_.CreateStructGEP(slot, slotptr, 0));
    }
    k->addIncoming(b_.CreateAdd(k, b_.getInt32(1)), b_.GetInsertBlock());
    b_.CreateBr(head);
    b_.SetInsertPoint(done);
}

// The ir::Type Ref of a DPI open-array actual: resolve the operand's lvalue leaf
// to its root Signal / Local / Arg and read that root's declared type (the whole
// unpacked-array type, carrying element / left / right). The actual is always a
// whole array (build_call_args passes its lvalue leaf).
Ref Backend::open_array_type_ref(Index a) {
    const Index root = lvalue_root(*ir_, a).root;
    switch (ir_->tag(root)) {
        case Inst::Tag::Signal:
            return mod_.signal(ir_->payload(root).signal).type;
        case Inst::Tag::Local:
            return ir_->payload(root).local.type;
        case Inst::Tag::Arg:
            return ir_->payload(root).arg.type;
        default:
            throw BackendError{ "internal: DPI open-array actual is not a "
                                "signal / local / arg root" };
    }
}

// Materialize an rt_svopen descriptor on the stack from its seven fields (data
// pointer + six i32s). Fields may be runtime Values (dynamic-array length) or
// constants (fixed array), so all are passed as Values.
llvm::Value* Backend::emit_svopen(llvm::Value* data,
                                  llvm::Value* left,
                                  llvm::Value* right,
                                  llvm::Value* nelem,
                                  llvm::Value* elem_bytes,
                                  llvm::Value* ew,
                                  llvm::Value* is_logic) {
    auto* ty   = rt_svopen_ty();
    auto* desc = entry_alloca(ty, "dpi.oa.desc");
    b_.CreateStore(data, b_.CreateStructGEP(ty, desc, 0));
    b_.CreateStore(left, b_.CreateStructGEP(ty, desc, 1));
    b_.CreateStore(right, b_.CreateStructGEP(ty, desc, 2));
    b_.CreateStore(nelem, b_.CreateStructGEP(ty, desc, 3));
    b_.CreateStore(elem_bytes, b_.CreateStructGEP(ty, desc, 4));
    b_.CreateStore(ew, b_.CreateStructGEP(ty, desc, 5));
    b_.CreateStore(is_logic, b_.CreateStructGEP(ty, desc, 6));
    return desc;
}

// Build the svOpenArrayHandle for a dynamic-array open-array actual (LRM 7.5).
// The actual is a runtime `{ data, len }` handle; for a named-scalar element the
// heap buffer is already a contiguous C `T[len]`, so the descriptor points
// straight at it (zero-copy, all directions — the C reads/writes the heap in
// place; a fixed-size view, so it never resizes). Bounds are the array's natural
// 0-based [0 : len-1]. Packed elements (a runtime-sized marshal scratch) are a
// follow-up. NOTE: an empty array (len == 0) yields right == -1; a C consumer
// must use svSize, not svLow..svHigh, to bound an empty array.
llvm::Value* Backend::build_dpi_dynarray_desc(const LValue& lv, Ref arr_ref, DpiCType elem_ct) {
    const DynElem e            = dynarr_elem(arr_ref);
    const bool    named_scalar = elem_ct == DpiCType::Byte || elem_ct == DpiCType::Shortint ||
                              elem_ct == DpiCType::Int || elem_ct == DpiCType::Longint;
    if (!named_scalar || !e.narrow) {
        throw BackendError{ "DPI open-array over a dynamic array supports only "
                            "byte/shortint/int/longint (zero-copy) elements so far" };
    }
    llvm::Value* handle = lv.base; // &rt_dynarr { data, len }
    llvm::Value* data =
      b_.CreateLoad(raw_ptr_ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 0));
    llvm::Value* len = b_.CreateZExtOrTrunc(
      b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(dynarr_slot_ty(), handle, 1)),
      b_.getInt32Ty());
    const uint32_t ew = e.width;
    return emit_svopen(data, b_.getInt32(0), b_.CreateSub(len, b_.getInt32(1)), len,
                       b_.getInt32(ew / 8), b_.getInt32(ew), b_.getInt32(0));
}

// Build the svOpenArrayHandle (rt_svopen*) for a DPI open-array argument
// (LRM 35.5.6.2). Named-scalar (byte/shortint/int/longint) elements are zero-copy
// — Clotho's narrow `[N x iW]` element storage is already a contiguous C array —
// so the descriptor points straight at the design array (the C reads and, for
// output/inout, writes it directly). Packed bit/logic elements are marshalled in
// inline LLVM IR into a contiguous svVecVal buffer: packed in for input/inout,
// and recorded in `backs` for the unpack-back after the call for output/inout.
llvm::Value* Backend::build_dpi_open_array_desc(Index                       a,
                                                DpiCType                    elem_ct,
                                                ArgDir                      dir,
                                                std::vector<OaMarshalBack>& backs) {
    const LValue lv = lvalue_of(a);
    const Ref    arr_ref = open_array_type_ref(a);
    const Type&  at      = std::get<Type>(mod_.pool.get(arr_ref));
    if (at.kind == Type::Kind::DynArray) {
        // Dynamic array actual (LRM 7.5): a runtime `{ data, len }` handle. A
        // named-scalar element is zero-copy — the heap buffer is already the C
        // array; packed/queue elements (marshal to a runtime-sized scratch) are a
        // follow-up (see docs/design/dpi-c.md).
        return build_dpi_dynarray_desc(lv, arr_ref, elem_ct);
    }
    if (at.kind != Type::Kind::UnpackedArray) {
        // Queue actuals need a ring-de-indexed marshal; deferred.
        throw BackendError{ "DPI open-array actual must be a fixed-size unpacked "
                            "array or dynamic array (queue deferred)" };
    }
    if (lv.array_ty == nullptr || lv.slot_ty == nullptr) {
        throw BackendError{ "internal: DPI open-array actual is not a whole memory" };
    }
    const uint32_t n     = array_dim(at);
    const int32_t  left  = static_cast<int32_t>(at.left);
    const int32_t  right = static_cast<int32_t>(at.right);
    const uint32_t ew    = lv.store_width; // element bit width
    const uint32_t nch   = (ew + 31) / 32; // svVecVal words per marshalled element
    const bool     pack_in  = dir != ArgDir::Out;   // input / inout read the actual
    const bool     copy_out = dir != ArgDir::In;    // output / inout write it back

    // Use the actual element storage slot (`lv.slot_ty`) — not a conservative
    // reconstruction — so may-z elision (a `{ i2W }` slot with no z word) is
    // reflected in the has_z flag the marshal needs.
    llvm::StructType* slot   = lv.slot_ty;
    const auto        stride = static_cast<uint32_t>(env_->llmod.getDataLayout().getTypeAllocSize(slot));
    llvm::Value*      storage = lv.base; // whole `[N x slot]` storage pointer

    llvm::Value* data;
    uint32_t     elem_bytes;
    uint32_t     is_logic;
    switch (elem_ct) {
        case DpiCType::Byte:
        case DpiCType::Shortint:
        case DpiCType::Int:
        case DpiCType::Longint: {
            // Zero-copy requires the narrow single-iW element slot (stride ==
            // C element size). A never-narrow named scalar (its z word forced by
            // a task out/inout use elsewhere) would not be C-layout — marshal it
            // as a bit vector instead (the mm plane's low W bits are the value).
            if (lv.narrow && stride == ew / 8) {
                // The C reads/writes the design array directly; an output/inout
                // write lands in storage with no copy-back (and, per DPI, does not
                // trigger wait/NBA scheduling on the array — a documented gap).
                data       = storage;
                elem_bytes = ew / 8;
                is_logic   = 0;
                break;
            }
            auto* buf = entry_alloca(llvm::ArrayType::get(b_.getInt32Ty(), n * nch), "dpi.oa.bit");
            if (pack_in) {
                emit_dpi_oa_pack_ir(storage, buf, slot, n, ew, nch, /*has_z=*/false, /*is_logic=*/false);
            }
            if (copy_out) {
                backs.push_back({ false, buf, storage, slot, ew, n, nch, 0 });
            }
            data       = buf;
            elem_bytes = 4 * nch;
            is_logic   = 0;
            break;
        }
        case DpiCType::BitVec: {
            auto* buf = entry_alloca(llvm::ArrayType::get(b_.getInt32Ty(), n * nch), "dpi.oa.bit");
            if (pack_in) {
                emit_dpi_oa_pack_ir(storage, buf, slot, n, ew, nch, /*has_z=*/false, /*is_logic=*/false);
            }
            if (copy_out) {
                backs.push_back({ false, buf, storage, slot, ew, n, nch, 0 });
            }
            data       = buf;
            elem_bytes = 4 * nch; // svBitVecVal[nch]
            is_logic   = 0;
            break;
        }
        case DpiCType::LogicVec: {
            auto* svl = llvm::StructType::get(ctx_, { b_.getInt32Ty(), b_.getInt32Ty() });
            auto* buf = entry_alloca(llvm::ArrayType::get(svl, n * nch), "dpi.oa.logic");
            const uint32_t has_z = lv.has_z ? 1u : 0u;
            if (pack_in) {
                emit_dpi_oa_pack_ir(storage, buf, slot, n, ew, nch, has_z != 0, /*is_logic=*/true);
            }
            if (copy_out) {
                backs.push_back({ true, buf, storage, slot, ew, n, nch, has_z });
            }
            data       = buf;
            elem_bytes = 8 * nch; // svLogicVecVal[nch] {aval,bval}
            is_logic   = 1;
            break;
        }
        default:
            throw BackendError{ "internal: bad DPI open-array element ctype" };
    }

    return emit_svopen(data, b_.getInt32(static_cast<uint32_t>(left)),
                       b_.getInt32(static_cast<uint32_t>(right)), b_.getInt32(n),
                       b_.getInt32(elem_bytes), b_.getInt32(ew), b_.getInt32(is_logic));
}

// Lazily create one of the DPI caller-identity globals (see the member
// declarations). Internal linkage: a DPI `.o` links into the same JITDylib, but
// nothing outside the generated module has any business reading these -- the
// protocol is entirely between an import-task call site and an export-task
// thunk. Created on first use, so a design with no DPI tasks emits none.
llvm::GlobalVariable* Backend::dpi_caller_gv(llvm::GlobalVariable*& slot, llvm::Type* ty, const char* name) {
    if (slot == nullptr) {
        slot = new llvm::GlobalVariable(env_->llmod, ty, false, llvm::GlobalValue::InternalLinkage,
                                        llvm::Constant::getNullValue(ty), name);
    }
    return sym(slot);
}

llvm::GlobalVariable* Backend::dpi_in_task_gv() {
    return dpi_caller_gv(dpi_in_task_gv_, b_.getInt8Ty(), "clotho.dpi.in_import_task");
}

llvm::GlobalVariable* Backend::dpi_caller_proc_gv() {
    return dpi_caller_gv(dpi_caller_proc_gv_, raw_ptr_ty(), "clotho.dpi.caller_proc");
}

llvm::GlobalVariable* Backend::dpi_caller_home_gv() {
    return dpi_caller_gv(dpi_caller_home_gv_, b_.getInt8Ty(), "clotho.dpi.caller_home");
}

llvm::GlobalVariable* Backend::dpi_caller_rng_gv() {
    return dpi_caller_gv(dpi_caller_rng_gv_, raw_ptr_ty(), "clotho.dpi.caller_rng");
}

// Publish this call site's identity, returning what was there. Only the pieces
// the design's task ABI actually carries are written (the same design_uses_*
// gates declare_task uses); in_import_task is unconditional because LRM 35.8's
// legality rule is.
Backend::DpiCallerSave Backend::dpi_publish_caller() {
    DpiCallerSave saved;
    auto* i8 = b_.getInt8Ty();
    saved.in_task = b_.CreateLoad(i8, dpi_in_task_gv(), "dpi.in_task.save");
    b_.CreateStore(b_.getInt8(1), dpi_in_task_gv());
    if (design_uses_process_) {
        // The calling THREAD's control block: an exported task reached from this
        // C runs in that thread (LRM 9.7), so `process::self()` inside it must
        // name the process that entered the import task, not "no caller".
        saved.proc = b_.CreateLoad(raw_ptr_ty(), dpi_caller_proc_gv(), "dpi.proc.save");
        b_.CreateStore(proc_arg(), dpi_caller_proc_gv());
    }
    if (design_uses_home_arg_) {
        // ... and its scheduling home region (LRM 24.3.1), which decides where a
        // `#0` inside the exported task lands. callee_home() is this thread's
        // answer here, exactly as at an ordinary task call.
        saved.home = b_.CreateLoad(i8, dpi_caller_home_gv(), "dpi.home.save");
        b_.CreateStore(callee_home(), dpi_caller_home_gv());
    }
    // ... and its RNG stream. LRM 18.13 gives every thread an independent
    // stream, and an exported task is not a thread -- it runs on the caller's,
    // so a `$urandom` inside it must continue the caller's sequence. This is
    // where an export TASK differs from an export FUNCTION, which correctly
    // passes the module-wide state (see emit_dpi_export): a function reached
    // from C has no calling thread at all, a task always does.
    saved.rng = b_.CreateLoad(raw_ptr_ty(), dpi_caller_rng_gv(), "dpi.rng.save");
    b_.CreateStore(rng_slot_, dpi_caller_rng_gv());
    return saved;
}

void Backend::dpi_restore_caller(const DpiCallerSave& saved) {
    b_.CreateStore(saved.in_task, dpi_in_task_gv());
    if (saved.proc != nullptr) {
        b_.CreateStore(saved.proc, dpi_caller_proc_gv());
    }
    if (saved.home != nullptr) {
        b_.CreateStore(saved.home, dpi_caller_home_gv());
    }
    b_.CreateStore(saved.rng, dpi_caller_rng_gv());
}

// Lower a `call` to a DPI-C import (LRM 35): marshal each argument — scalar by
// value (input) or through a C-native scratch pointer (output / inout), packed
// vector always through an svBitVecVal* / svLogicVecVal* scratch — call the
// external symbol directly (no state pointer), and convert the C return back to
// a Plane. The frontend guards unsupported types, so no ref servicing.
void Backend::lower_dpi_call(Index i, const CallInfo& ci, const Function& f) {
    // Same signature-arity invariant as lower_call_common / lower_task_call_inst:
    // the loop below indexes dpi_arg_ctypes / arg_dirs / param_types by the
    // actual's position. A DPI import is never a class method, so the receiver
    // splice that motivated the check cannot reach here -- the guard is uniform
    // across all three marshalling loops so the invariant has one statement.
    if (ir_->region(ci.args).size() != f.param_types.size()) {
        throw BackendError{ std::format("internal: DPI call to '{}' passes {} arguments for {} formals",
                                        f.name, ir_->region(ci.args).size(),
                                        f.param_types.size()) };
    }
    llvm::Function*           cf = sym(fn_decls_[static_cast<uint32_t>(ci.func)]);
    std::vector<llvm::Value*> argv;
    // output / inout scratches to read back into the caller temp after the
    // call; the frontend's CopyOutArg then copies temp -> actual. `cty` is null
    // for a vec scratch (read back via dpi_vec_copyout instead).
    struct DpiOut {
        Index        temp;
        DpiCType     ct;
        llvm::Value* scratch;
        llvm::Type*  cty;
        uint32_t     w;
    };
    std::vector<DpiOut>        outs;
    std::vector<OaMarshalBack> oa_backs; // marshalled open-array out/inout copy-backs
    size_t                     k = 0;
    for (Index a : ir_->region(ci.args)) {
        const DpiCType ct  = f.dpi_arg_ctypes[k];
        const ArgDir   dir = f.arg_dirs.empty() ? ArgDir::In : f.arg_dirs[k];
        const uint32_t w   = width_of(f.param_types[k]);
        // Scratch allocas are entry-block-hoisted (entry_alloca), so a DPI call
        // in a loop does not grow the stack per iteration.
        if (ct == DpiCType::String) {
            // input string -> const char*: the handle's bytes are already NUL-
            // terminated (rt_string_data), so pass the pointer directly, no
            // copy. Valid for the call's duration; the C must not retain it.
            // output / inout string is frontend-guarded.
            argv.push_back(b_.CreateCall(str_data_fn(), { str_of(a) }));
        } else if (ct == DpiCType::OpenArray) {
            // Open array (LRM 35.5.6.2): build the svOpenArrayHandle descriptor
            // (zero-copy or marshalled) and pass it; a marshalled output/inout is
            // recorded in oa_backs for the unpack-back after the call.
            argv.push_back(
              build_dpi_open_array_desc(a, f.dpi_arg_elem_ctypes[k], dir, oa_backs));
        } else if (is_dpi_vec(ct)) {
            // Packed vector: always by pointer. Input builds the scratch from the
            // value; output leaves it empty for the C to fill; inout copies in.
            llvm::Value* scratch = dpi_vec_alloca(ct, dpi_nchunks(w));
            if (dir == ArgDir::In) {
                dpi_vec_copyin(ct, value_of(a), w, scratch);
            } else if (dir == ArgDir::Inout) {
                dpi_vec_copyin(ct, lower_load(lvalue_of(a)), w, scratch);
            }
            argv.push_back(scratch);
            if (dir != ArgDir::In) {
                outs.push_back({ a, ct, scratch, nullptr, w });
            }
        } else if (dir == ArgDir::In) {
            argv.push_back(dpi_arg_to_c(ct, value_of(a)));
        } else {
            llvm::Type*  cty     = dpi_c_llvm_ty(ct);
            llvm::Value* scratch = entry_alloca(cty, "dpi.out");
            if (dir == ArgDir::Inout) {
                b_.CreateStore(dpi_arg_to_c(ct, lower_load(lvalue_of(a))), scratch);
            }
            argv.push_back(scratch);
            outs.push_back({ a, ct, scratch, cty, w });
        }
        ++k;
    }
    // An import TASK publishes the calling thread's identity for the duration of
    // the C call, so an exported task the C reaches can run on that thread
    // (LRM 35.8). An import FUNCTION publishes nothing: 35.8 makes an exported
    // task illegal from one, and the export-task thunk detects exactly that by
    // finding in_import_task still 0.
    const DpiCallerSave saved = f.is_dpi_task ? dpi_publish_caller() : DpiCallerSave{};
    llvm::Value*        r     = b_.CreateCall(cf->getFunctionType(), cf, argv);
    if (f.is_dpi_task) {
        dpi_restore_caller(saved);
    }
    for (const DpiOut& o : outs) {
        const Plane pl = is_dpi_vec(o.ct)
                           ? dpi_vec_copyout(o.ct, o.scratch, o.w)
                           : dpi_c_to_plane(o.ct, b_.CreateLoad(o.cty, o.scratch));
        lower_store(lvalue_of(o.temp), pl);
    }
    // Marshalled open-array output/inout: unpack the C-written svVecVal scratch
    // back into the design element storage (inline IR, native iW/i2W). A
    // zero-copy element was written in place, not here.
    for (const OaMarshalBack& o : oa_backs) {
        emit_dpi_oa_unpack_ir(o.buf, o.storage, o.slot, o.n, o.ew, o.nch, o.has_z != 0, o.is_logic);
    }
    if (f.dpi_ret_ctype == DpiCType::String) {
        // The C returns a NUL-terminated char*; copy it into this call's result
        // handle (str_of(i), alloc'd by build_str_temps). C keeps ownership.
        b_.CreateCall(dpi_str_out_fn(),
                      { str_of(i), r, alloc_site_global(loc_text(i), "string") });
    } else if (f.dpi_ret_ctype != DpiCType::Void) {
        set_value(i, dpi_c_to_plane(f.dpi_ret_ctype, r));
    }
    // An import task falls through both arms: dpi_ret_ctype is Void because the
    // SV task has no result, so its i32 C return -- 35.9's disable
    // acknowledgement -- is discarded. TODO: T3 reads it, and a 1 must then
    // unwind the enclosing disable rather than being dropped here.
}

// Whether the design has any DPI import or export — gates the svScope init.
bool Backend::design_uses_dpi() const {
    if (!mod_.dpi_exports.empty()) {
        return true;
    }
    for (size_t i = 0; i < mod_.function_count(); ++i) {
        if (mod_.function(static_cast<FunctionId>(i)).is_dpi) {
            return true;
        }
    }
    return false;
}

// Emit one C-ABI thunk per `export "DPI-C"` (LRM 35.5). Runs last (after
// emit_main creates @clotho.state and every SV function is declared).
void Backend::emit_dpi_exports() {
    for (const DpiExport& ex : mod_.dpi_exports) {
        emit_dpi_export(ex);
    }
}

// The C-visible parameter type of an export formal: an input scalar passes by
// value; output/inout scalars, packed vectors (any direction), and input
// strings pass by pointer — mirroring declare_dpi_import.
llvm::Type* Backend::dpi_export_c_param_ty(DpiCType ct, ArgDir dir) {
    if (is_dpi_vec(ct) || ct == DpiCType::String || dir != ArgDir::In) {
        return raw_ptr_ty();
    }
    return dpi_c_llvm_ty(ct);
}

// Convert a Plane (a value the C side provided) into the SV function's input
// formal ABI — the packed (mm, z) pair, appended to argv (real via
// real_to_planes). Shared by scalar and vec input args.
void Backend::dpi_push_plane_arg(std::vector<llvm::Value*>& argv, Plane pl) {
    const Plane four = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
    argv.push_back(four.mm);
    argv.push_back(four.z);
}

// Copy every output / inout cell back to its C pointer and release the input
// string temporaries. Shared by the function and task arms of emit_dpi_export:
// a task reaches it only on the completed path, so it has exactly one caller
// per arm, but the marshal is delicate enough (real reconstruction, vec
// chunking, x/z) that a second copy would be a standing correctness hazard.
void Backend::emit_dpi_export_copyout(const std::vector<ExpOut>& outs, const std::vector<llvm::Value*>& str_temps) {
    for (const ExpOut& o : outs) {
        Plane pl{ b_.CreateLoad(int_ty(2 * o.w), o.mm_ptr), b_.CreateLoad(int_ty(o.w), o.z_ptr) };
        if (is_dpi_vec(o.ct)) {
            dpi_vec_copyin(o.ct, pl, o.w, o.c_ptr); // plane -> C svVecVal
        } else {
            if (o.ct == DpiCType::Real || o.ct == DpiCType::Shortreal) {
                pl = real_plane(planes_to_real(pl, o.w));
            }
            b_.CreateStore(dpi_arg_to_c(o.ct, pl), o.c_ptr);
        }
    }
    for (llvm::Value* h : str_temps) {
        b_.CreateCall(str_deinit_fn(), { h });
    }
}

// A C-callable entry point `<c_ret> <cid>(<c_args>)` (external linkage) that
// marshals C arguments into the exported subroutine's plane ABI, calls it
// against the design-state global, and marshals results back to C. Input
// scalar/chandle/real/vec and input string args; output/inout
// scalar/chandle/real/vec args; scalar/chandle/real/void return (the frontend
// guards the rest). The marshal reuses the import helpers in reverse.
//
// An exported TASK (LRM 35.8) shares all of that -- 35.7's argument rules apply
// to a task unchanged -- and differs in three places: its C prototype returns
// `int` whatever the SV side says, the callee is a coroutine that must be
// ramped and resumed rather than called, and the caller identity is real
// rather than absent (see dpi_publish_caller).
void Backend::emit_dpi_export(const DpiExport& ex) {
    const Function& f    = mod_.function(ex.func);
    llvm::Function* svfn = sym(fn_decls_[static_cast<uint32_t>(ex.func)]);

    std::vector<llvm::Type*> cparams;
    cparams.reserve(ex.arg_ctypes.size());
    for (size_t k = 0; k < ex.arg_ctypes.size(); ++k) {
        cparams.push_back(dpi_export_c_param_ty(ex.arg_ctypes[k], ex.arg_dirs[k]));
    }
    // LRM 35.8: "The return value of an exported task is an int value that
    // indicates if a disable is active or not on the current execution thread."
    // An SV task has no result, so this return is generated, never marshalled;
    // T1 always answers 0, since there is no disable protocol yet (T3).
    auto* cret  = ex.is_task ? b_.getInt32Ty() : dpi_c_llvm_ty(ex.ret_ctype);
    auto* fnty  = llvm::FunctionType::get(cret, cparams, false);
    auto* thunk = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, ex.extern_name, env_->llmod);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", thunk));

    // LRM 35.8's legality rule, enforced where it becomes decidable. "It is
    // never legal to call an exported task from within an imported function.
    // This semantics is identical to native SystemVerilog semantics, in which it
    // is illegal for a function to perform a task enable." The SV half is
    // slang's ("cannot invoke a task from within a function"); the C half is
    // only knowable at run time, because WHICH DPI frame is live is a property
    // of the call chain rather than of this thunk. clotho.dpi.in_import_task is
    // 1 exactly while an import-TASK call is in flight, so 0 here means the
    // innermost DPI frame belongs to an imported function -- or that no DPI
    // import reached this C at all, which is equally unrunnable: there would be
    // no thread whose time, region and RNG stream the task could borrow.
    if (ex.is_task) {
        auto* legal_bb = llvm::BasicBlock::Create(ctx_, "dpi.task.legal", thunk);
        auto* bad_bb   = llvm::BasicBlock::Create(ctx_, "dpi.task.no_frame", thunk);
        b_.CreateCondBr(b_.CreateICmpNE(b_.CreateLoad(b_.getInt8Ty(), dpi_in_task_gv(), "dpi.in_task"),
                                        b_.getInt8(0)),
                        legal_bb, bad_bb);
        b_.SetInsertPoint(bad_bb);
        b_.CreateCall(dpi_export_task_no_task_frame_fn(), { b_.CreateGlobalString(ex.extern_name) });
        b_.CreateUnreachable();
        b_.SetInsertPoint(legal_bb);
    }

    std::vector<ExpOut>       outs;
    std::vector<llvm::Value*> str_temps; // input rt_string handles to deinit

    auto*                     ptr_ty = llvm::PointerType::getUnqual(ctx_);
    std::vector<llvm::Value*> argv;
    argv.push_back(sym(state_global_));
    // The RNG stream the exported subroutine draws from (LRM 18.13). A FUNCTION
    // has no calling process -- native C drove the call -- so it draws from the
    // module-wide state main seeds at startup. A TASK does have one: it runs on
    // the thread that entered the import task, so a `$urandom` inside it has to
    // continue THAT thread's sequence, which the import-task call site parked in
    // clotho.dpi.caller_rng.
    argv.push_back(ex.is_task ? static_cast<llvm::Value*>(
                                  b_.CreateLoad(ptr_ty, dpi_caller_rng_gv(), "dpi.rng"))
                              : static_cast<llvm::Value*>(sym(rng_global_)));
    // A task's ABI carries the wheel (declare_task); a function's does not, since
    // a function cannot reach a timing control.
    if (ex.is_task) {
        argv.push_back(sym(wheel_gv_));
    }
    // The calling thread. A function has none, so `process::self()` inside an
    // exported function has no answer and reports a located fatal rather than
    // inventing one. A task has one, and carrying it is the whole point of the
    // caller-identity globals: `process::self()` inside an exported task names
    // the process that entered the import task.
    if (design_uses_process_) {
        argv.push_back(ex.is_task ? static_cast<llvm::Value*>(
                                      b_.CreateLoad(ptr_ty, dpi_caller_proc_gv(), "dpi.proc"))
                                  : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(ptr_ty)));
    }
    // Its scheduling home region (LRM 24.3.1), the same split: an exported
    // function inherits nothing because no thread called it; an exported task
    // inherits the region of the thread that did, which is what decides where a
    // `#0` inside it would land.
    if (design_uses_home_arg_) {
        argv.push_back(ex.is_task ? static_cast<llvm::Value*>(
                                      b_.CreateLoad(b_.getInt8Ty(), dpi_caller_home_gv(), "dpi.home"))
                                  : static_cast<llvm::Value*>(b_.getInt8(0)));
    }
    for (size_t k = 0; k < ex.arg_ctypes.size(); ++k) {
        const DpiCType ct     = ex.arg_ctypes[k];
        const ArgDir   dir    = ex.arg_dirs[k];
        const uint32_t w      = width_of(f.param_types[k]);
        llvm::Value*   cparam = thunk->getArg(k);

        if (ct == DpiCType::String) { // input only (guarded)
            auto* h = entry_alloca(str_slot_ty(), "dpi.exp.str");
            b_.CreateCall(str_init_fn(), { h });
            // h receives a copy of the C const char*. The export thunk is
            // emitted per exported subroutine rather than from a lowered
            // instruction, so no Index locates this copy.
            b_.CreateCall(dpi_str_out_fn(),
                          { h, cparam,
                            alloc_site_global(SourceText{ .where = "<unknown>" }, "string") });
            argv.push_back(h);
            str_temps.push_back(h);
        } else if (dir == ArgDir::In) {
            // Input value -> plane. A vec unpacks the C svVecVal; a scalar decodes
            // the C value directly.
            const Plane pl = is_dpi_vec(ct) ? dpi_vec_copyout(ct, cparam, w)
                                            : dpi_c_to_plane(ct, cparam);
            dpi_push_plane_arg(argv, pl);
        } else {
            // output / inout: the SV ABI is two pointers to (i(2W) mm, iW z)
            // cells the callee writes. inout copies the C value in first.
            llvm::Value* mm_ptr = entry_alloca(int_ty(2 * w), "dpi.exp.mm");
            llvm::Value* z_ptr  = entry_alloca(int_ty(w), "dpi.exp.z");
            if (dir == ArgDir::Inout) {
                const Plane pl = is_dpi_vec(ct)
                                   ? dpi_vec_copyout(ct, cparam, w)
                                   : dpi_c_to_plane(ct, b_.CreateLoad(dpi_c_llvm_ty(ct), cparam));
                const Plane four = pl.fp != nullptr ? real_to_planes(fp_of(pl)) : to_four(pl);
                b_.CreateStore(four.mm, mm_ptr);
                b_.CreateStore(four.z, z_ptr);
            }
            argv.push_back(mm_ptr);
            argv.push_back(z_ptr);
            outs.push_back({ cparam, mm_ptr, z_ptr, ct, w });
        }
    }

    if (!ex.is_task) {
        llvm::Value* r = b_.CreateCall(svfn->getFunctionType(), svfn, argv);
        emit_dpi_export_copyout(outs, str_temps);
        if (ex.ret_ctype == DpiCType::Void) {
            b_.CreateRetVoid();
            return;
        }
        // Unpack the SV {mm, z} return struct into a Plane, then marshal to C.
        Plane         rp{ b_.CreateExtractValue(r, { 0 }), b_.CreateExtractValue(r, { 1 }) };
        const ValueType t = value_type_of(mod_.pool, f.return_type);
        if (t.is_real) {
            rp = real_plane(planes_to_real(rp, t.width));
        }
        b_.CreateRet(dpi_arg_to_c(ex.ret_ctype, rp));
        return;
    }

    // A task is a coroutine, so drive it exactly the way lower_task_call_inst
    // drives an SV task enable: null the record so the callee sees no parent and
    // enqueues nobody, ramp it, resume once, then ask whether it finished.
    llvm::Value* record = entry_alloca(ptr_ty, "dpi.task_record");
    b_.CreateStore(llvm::ConstantPointerNull::get(ptr_ty), record);
    argv.push_back(record);
    llvm::Value* hdl  = b_.CreateCall(svfn->getFunctionType(), svfn, argv);
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
    llvm::Value* done = b_.CreateCall(coro_intr(llvm::Intrinsic::coro_done), { hdl });

    auto* done_bb = llvm::BasicBlock::Create(ctx_, "dpi.task.done", thunk);
    auto* susp_bb = llvm::BasicBlock::Create(ctx_, "dpi.task.suspended", thunk);
    b_.CreateCondBr(done, done_bb, susp_bb);

    // Not done: the task parked on a delay / event control / `wait`.
    // lower_task_call_inst's async arm would store the CALLER's coroutine handle
    // in the record and suspend, but this caller is a native C frame -- it has
    // no handle and no way to be parked, which is exactly what phase T2 adds.
    // Refuse loudly rather than return to C with the task half-run: its outputs
    // are unwritten and its body never completes, so continuing would be a
    // silent wrong answer (PRINCIPLES.md).
    // TODO: T2 -- hand the baton to a pooled worker stack here and take
    // lower_task_call_inst's async arm at the import-task call site instead.
    b_.SetInsertPoint(susp_bb);
    b_.CreateCall(dpi_export_task_suspended_fn(), { b_.CreateGlobalString(ex.extern_name) });
    b_.CreateUnreachable();

    // Done: it ran to completion in zero time and its epilogue has already
    // written the output / inout cells, so destroy the frame and marshal back.
    b_.SetInsertPoint(done_bb);
    b_.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    emit_dpi_export_copyout(outs, str_temps);
    // 0 = "no disable is active on the current execution thread" (LRM 35.8), the
    // only answer T1 can give. TODO: T3 returns 1 for a disable-terminated task.
    b_.CreateRet(b_.getInt32(0));
}

// Marshal an SV scalar value (a Plane the frontend resized to the formal type)
// to its DPI-C native argument (LRM 35.5.6). A 2-state integer passes its value
// plane; a scalar logic packs to svLogic (0/1/2=Z/3=X); a real rides its fp.
llvm::Value* Backend::dpi_arg_to_c(DpiCType t, Plane p) {
    switch (t) {
        case DpiCType::Byte:
            return cast(must_of(p), 8, true);
        case DpiCType::Shortint:
            return cast(must_of(p), 16, true);
        case DpiCType::Int:
            return cast(must_of(p), 32, true);
        case DpiCType::Longint:
            return cast(must_of(p), 64, true);
        case DpiCType::Real:
        case DpiCType::Shortreal:
            return fp_of(p);
        case DpiCType::Chandle:
            // The i64 handle bits reinterpreted as an opaque void*.
            return b_.CreateIntToPtr(cast(must_of(p), 64, false), raw_ptr_ty());
        case DpiCType::BitScalar:
            return cast(must_of(p), 8, false); // zext the 1-bit value to svBit
        case DpiCType::LogicScalar: {
            // svLogic: sv_0=0, sv_1=1, sv_z=2, sv_x=3. z wins; else 1 for a
            // definite 1; else 3 (X) for any other unknown; else 0. The formal
            // is width 1. A 4-state Plane may carry no z plane (never-z), so
            // materialize the z bit safely rather than through z_of (nullptr).
            const uint32_t w    = width_of(p);
            llvm::Value*   zbit = (p.two || p.z == nullptr) ? b_.getFalse()
                                                            : b_.CreateICmpNE(p.z, zero(w));
            return b_.CreateSelect(
              zbit, b_.getInt8(2),
              b_.CreateSelect(b_.CreateICmpNE(must_of(p), zero(w)), b_.getInt8(1),
                              b_.CreateSelect(b_.CreateICmpNE(may_of(p), zero(w)),
                                              b_.getInt8(3), b_.getInt8(0))));
        }
        default:
            throw BackendError{ "internal: DPI void argument" };
    }
}

// Convert a DPI-C native return value back to a Plane of the SV return type.
Plane Backend::dpi_c_to_plane(DpiCType t, llvm::Value* r) {
    switch (t) {
        case DpiCType::Byte:
        case DpiCType::Shortint:
        case DpiCType::Int:
        case DpiCType::Longint:
            return two_val(r); // the C iW is the 2-state value plane
        case DpiCType::Real:
        case DpiCType::Shortreal:
            return real_plane(r);
        case DpiCType::Chandle:
            // The returned void* reinterpreted as the i64 handle value.
            return two_val(b_.CreatePtrToInt(r, int_ty(64)));
        case DpiCType::BitScalar:
            return two_val(b_.CreateTrunc(r, int_ty(1)));
        case DpiCType::LogicScalar: {
            // Decode svLogic (i8): 0->0, 1->1, 2->Z, 3->X into must/may/z (i1).
            llvm::Value* must = b_.CreateICmpEQ(r, b_.getInt8(1));
            llvm::Value* may  = b_.CreateICmpNE(r, b_.getInt8(0));
            llvm::Value* z    = b_.CreateICmpEQ(r, b_.getInt8(2));
            return packed(must, may, z);
        }
        default:
            throw BackendError{ "internal: DPI void return has no value" };
    }
}

} // namespace clotho::ir::backend
