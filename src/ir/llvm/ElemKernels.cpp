// Element kernels: the copy and destroy functions a queue element's descriptor points at.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

namespace {

// Field numbers of rt_elem_ops (runtime/Runtime.h), which elem_ops_ty mirrors.
constexpr unsigned kFieldKind    = 0;
constexpr unsigned kFieldNleaves = 1;
constexpr unsigned kFieldStride  = 2;
constexpr unsigned kFieldProto   = 3;
constexpr unsigned kFieldInner   = 4;
constexpr unsigned kFieldLeaves  = 5;
constexpr unsigned kFieldCopy    = 10;
constexpr unsigned kFieldDestroy = 11;
constexpr unsigned kFieldCount   = 12;

// What an element descriptor says about its element, read back out of the constant
// the backend built it as (elem_ops_for). The kernels are the runtime's descriptor
// interpreter (runtime/Containers.cpp: rt_elem_init, rt_elem_copy, rt_elem_deinit)
// specialized to one descriptor, so they read the descriptor itself rather than walk
// the type a second time: a leaf the table lists is a leaf the kernel handles, and
// the two cannot drift.
struct ElemOpsView {
    uint32_t              kind    = 0;
    uint32_t              nleaves = 0;
    uint64_t              stride  = 0;
    llvm::GlobalVariable* proto   = nullptr;
    // The descriptor of a container element's own element; null otherwise.
    llvm::GlobalVariable* inner = nullptr;
    // The `leaves` table of an aggregate element; null otherwise.
    const llvm::ConstantArray* leaves = nullptr;

    // One `{ i64 offset, ptr ops }` entry of the leaf table.
    const llvm::ConstantStruct* leaf(uint32_t i) const {
        assert(i < nleaves && "a leaf index is within the table");
        return llvm::cast<llvm::ConstantStruct>(leaves->getOperand(i));
    }

    uint64_t leaf_offset(uint32_t i) const {
        return llvm::cast<llvm::ConstantInt>(leaf(i)->getOperand(0))->getZExtValue();
    }

    llvm::GlobalVariable* leaf_ops(uint32_t i) const {
        return llvm::cast<llvm::GlobalVariable>(leaf(i)->getOperand(1));
    }
};

ElemOpsView view_of(const llvm::GlobalVariable* ops) {
    const auto* init = llvm::cast<llvm::ConstantStruct>(ops->getInitializer());
    assert(init->getNumOperands() == kFieldCount && "a descriptor has every rt_elem_ops field");
    ElemOpsView v;
    v.kind    = static_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(init->getOperand(kFieldKind))->getZExtValue());
    v.nleaves = static_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(init->getOperand(kFieldNleaves))->getZExtValue());
    v.stride  = llvm::cast<llvm::ConstantInt>(init->getOperand(kFieldStride))->getZExtValue();
    v.proto   = llvm::cast<llvm::GlobalVariable>(init->getOperand(kFieldProto));
    v.inner   = llvm::dyn_cast<llvm::GlobalVariable>(init->getOperand(kFieldInner));
    if (const auto* table = llvm::dyn_cast<llvm::GlobalVariable>(init->getOperand(kFieldLeaves))) {
        v.leaves = llvm::cast<llvm::ConstantArray>(table->getInitializer());
    }
    assert((v.kind == rt_elem_aggregate) == (v.leaves != nullptr) && "only an aggregate has a leaf table");
    assert(v.leaves != nullptr ? v.leaves->getNumOperands() == v.nleaves : v.nleaves == 0);
    return v;
}

} // namespace

llvm::Value* Backend::elem_byte_ptr(llvm::Value* base, uint64_t offset) {
    return offset == 0 ? base : b_.CreateConstInBoundsGEP1_64(b_.getInt8Ty(), base, offset);
}

// The runtime's rt_elem_init: put a fresh element in the uninitialized slot `p`.
// An element's power-on image is the proto, except that an event is a NEW event
// object (LRM Table 6-7), so that is the one thing a constant cannot say.
//
// An aggregate's proto image already covers every leaf whose power-on value is a
// constant, which is every leaf but an event, so the interpreter's per-leaf
// re-initialization of a string or container leaf copies bytes already there and
// is left out here.
void Backend::emit_elem_init(const llvm::GlobalVariable* ops, llvm::Value* p) {
    const ElemOpsView v = view_of(ops);
    if (v.kind == rt_elem_event) {
        b_.CreateStore(b_.CreateCall(event_new_fn()), p);
        return;
    }
    b_.CreateMemCpy(p, llvm::MaybeAlign(), v.proto, llvm::MaybeAlign(), v.stride);
    for (uint32_t i = 0; i < v.nleaves; ++i) {
        if (view_of(v.leaf_ops(i)).kind == rt_elem_event) {
            emit_elem_init(v.leaf_ops(i), elem_byte_ptr(p, v.leaf_offset(i)));
        }
    }
}

// The runtime's rt_elem_copy: assign the element at `src` over the constructed one at `dst`.
void Backend::emit_elem_copy(const llvm::GlobalVariable* ops, llvm::Value* dst, llvm::Value* src, llvm::Value* site) {
    const ElemOpsView v = view_of(ops);
    switch (v.kind) {
        case rt_elem_pod:
        case rt_elem_event:
            // An event copies its HANDLE, so both elements name one sync object
            // (LRM 15.5.5's `a = b`).
            b_.CreateMemCpy(dst, llvm::MaybeAlign(), src, llvm::MaybeAlign(), v.stride);
            return;
        case rt_elem_string:
            b_.CreateCall(str_assign_fn(), { dst, src, site });
            return;
        case rt_elem_dynarr:
            b_.CreateCall(dynarr_assign_fn(), { dst, src, v.inner, site });
            return;
        case rt_elem_queue:
            b_.CreateCall(queue_assign_fn(), { dst, src, v.inner, site });
            return;
        case rt_elem_assoc:
            b_.CreateCall(assoc_assign_fn(), { dst, src, v.inner, site });
            return;
        case rt_elem_aggregate:
            emit_elem_copy_aggregate(ops, dst, src, site);
            return;
        default:
            llvm_unreachable("an element descriptor has a known kind");
    }
}

// An aggregate: deep-assign every heap leaf, then raw-copy the plain bytes between
// the leaf slots. The gap walk relies on the table being sorted by offset, the same
// contract the runtime's interpreter relies on (collect_elem_leaves).
void Backend::emit_elem_copy_aggregate(const llvm::GlobalVariable* ops,
                                       llvm::Value*                dst,
                                       llvm::Value*                src,
                                       llvm::Value*                site) {
    const ElemOpsView v = view_of(ops);
    assert(v.kind == rt_elem_aggregate && "only an aggregate has leaves to walk");
    for (uint32_t i = 0; i < v.nleaves; ++i) {
        const uint64_t off = v.leaf_offset(i);
        emit_elem_copy(v.leaf_ops(i), elem_byte_ptr(dst, off), elem_byte_ptr(src, off), site);
    }
    uint64_t pos = 0;
    for (uint32_t i = 0; i <= v.nleaves; ++i) {
        const uint64_t end = i < v.nleaves ? v.leaf_offset(i) : v.stride;
        assert(end >= pos && "the leaf table is sorted and its leaves do not overlap");
        if (end > pos) {
            b_.CreateMemCpy(
              elem_byte_ptr(dst, pos), llvm::MaybeAlign(), elem_byte_ptr(src, pos), llvm::MaybeAlign(), end - pos);
        }
        if (i < v.nleaves) {
            pos = v.leaf_offset(i) + view_of(v.leaf_ops(i)).stride;
        }
    }
}

// The runtime's rt_elem_deinit: release whatever the element in `p` owns.
void Backend::emit_elem_deinit(const llvm::GlobalVariable* ops, llvm::Value* p) {
    const ElemOpsView v = view_of(ops);
    switch (v.kind) {
        case rt_elem_pod:
            return;
        case rt_elem_event:
            // Event objects are never freed (LRM 15.5.5.2's reclamation is the
            // collector's, not the element's).
            return;
        case rt_elem_string:
            b_.CreateCall(str_deinit_fn(), { p });
            return;
        case rt_elem_dynarr:
            b_.CreateCall(dynarr_delete_fn(), { p, v.inner });
            return;
        case rt_elem_queue:
            b_.CreateCall(queue_delete_all_fn(), { p, v.inner });
            return;
        case rt_elem_assoc:
            b_.CreateCall(assoc_reset_fn(), { p, v.inner });
            return;
        case rt_elem_aggregate:
            for (uint32_t i = 0; i < v.nleaves; ++i) {
                emit_elem_deinit(v.leaf_ops(i), elem_byte_ptr(p, v.leaf_offset(i)));
            }
            return;
        default:
            llvm_unreachable("an element descriptor has a known kind");
    }
}

// void copy(void* dst, const void* src, const rt_alloc_site* site): construct a copy
// of the element at `src` in the uninitialized slot `dst`. This is what a push
// does to the new slot, and the engine calls it through rt_elem_ops::copy.
llvm::Function* Backend::elem_copy_kernel(const llvm::GlobalVariable* ops) {
    auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() }, false);
    auto* fn =
      llvm::Function::Create(fnty, llvm::Function::InternalLinkage, std::string(ops->getName()) + ".copy", env_->llmod);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->getArg(0)->setName("dst");
    fn->getArg(1)->setName("src");
    fn->getArg(2)->setName("site");
    // Kernels are made on demand from the middle of another function's body.
    const llvm::IRBuilderBase::InsertPointGuard guard(b_);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
    emit_elem_init(ops, fn->getArg(0));
    emit_elem_copy(ops, fn->getArg(0), fn->getArg(1), fn->getArg(2));
    b_.CreateRetVoid();
    return fn;
}

// void destroy(void* slot): release what the element in `slot` owns.
llvm::Function* Backend::elem_destroy_kernel(const llvm::GlobalVariable* ops) {
    auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { raw_ptr_ty() }, false);
    auto* fn   = llvm::Function::Create(
      fnty, llvm::Function::InternalLinkage, std::string(ops->getName()) + ".destroy", env_->llmod);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->getArg(0)->setName("slot");
    const llvm::IRBuilderBase::InsertPointGuard guard(b_);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
    emit_elem_deinit(ops, fn->getArg(0));
    b_.CreateRetVoid();
    return fn;
}

// The descriptor a queue operation that the runtime bitcode implements passes. With
// config_.rt_bitcode that is the element's descriptor plus its kernels, so the
// engine, once inlined, calls them directly; a plain-bytes element has no kernels to
// call (the engine copies it with memcpy) and keeps the ordinary descriptor, as does
// every operation when the gate is off.
llvm::Constant* Backend::queue_elem_ops(Ref queue_type) {
    llvm::Constant* plain = elem_ops_global(queue_type);
    if (!config_.rt_bitcode) {
        return plain;
    }
    auto* gv = llvm::cast<llvm::GlobalVariable>(plain);
    if (view_of(gv).kind == rt_elem_pod) {
        return plain;
    }
    const auto key = static_cast<uint32_t>(std::get<Type>(mod_.pool.get(queue_type)).element);
    if (const auto it = env_->elem_ops_kernels.find(key); it != env_->elem_ops_kernels.end()) {
        return it->second;
    }
    const auto*                  init = llvm::cast<llvm::ConstantStruct>(gv->getInitializer());
    std::vector<llvm::Constant*> fields;
    for (unsigned f = 0; f < kFieldCount; ++f) {
        fields.push_back(init->getOperand(f));
    }
    assert(fields[kFieldCopy]->isNullValue() && "a plain descriptor has no copy kernel");
    assert(fields[kFieldDestroy]->isNullValue() && "a plain descriptor has no destroy kernel");
    // The engine reads the kernels by the runtime struct's offsets, the mirror lays
    // them out by the module's DataLayout: the two have to agree.
    [[maybe_unused]] const llvm::StructLayout* layout = env_->llmod.getDataLayout().getStructLayout(elem_ops_ty());
    assert(layout->getSizeInBytes() == sizeof(rt_elem_ops) && "the mirror is as big as rt_elem_ops");
    assert(layout->getElementOffset(kFieldCopy) == offsetof(rt_elem_ops, copy) && "copy is where the engine reads it");
    assert(layout->getElementOffset(kFieldDestroy) == offsetof(rt_elem_ops, destroy) && "destroy likewise");
    fields[kFieldCopy]    = elem_copy_kernel(gv);
    fields[kFieldDestroy] = elem_destroy_kernel(gv);
    auto* kernels         = new llvm::GlobalVariable(env_->llmod,
                                                     elem_ops_ty(),
                                                     /*isConstant=*/true,
                                                     llvm::GlobalValue::InternalLinkage,
                                                     llvm::ConstantStruct::get(elem_ops_ty(), fields),
                                                     "clotho.elem.ops");
    env_->elem_ops_kernels.emplace(key, kernels);
    return kernels;
}

} // namespace clotho::ir::backend
