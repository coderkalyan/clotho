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

// An aggregate element with more heap leaves than this is copied and destroyed by a
// loop over its leaf table, not by one statement per leaf: a kernel's size then
// follows the number of DISTINCT leaf types, which is what `string tags[1000]` needs
// it to, and not the number of leaves. Below it the unrolled form is smaller than
// the loop and the optimizer can see through it. The number is where a kernel stops
// being small enough to be inlined into the engine anyway (about a call and two
// address computations per leaf against the inliner's default threshold), so past it
// the loop costs nothing the unrolled form would have kept.
constexpr uint32_t kElemKernelLeavesUnrolledMax = 16;

// What an element descriptor says about its element, read back out of the constant
// the backend built it as (elem_ops_for). The kernels are the interpreter of a
// descriptor (rt_elem_init, rt_elem_copy and rt_elem_deinit in runtime/Containers.cpp,
// which the other containers still use) specialized to one descriptor, so they read
// the descriptor itself rather than walk the type a second time: a leaf the table
// lists is a leaf the kernel handles, and the two cannot drift.
struct ElemOpsView {
    uint32_t              kind    = 0;
    uint32_t              nleaves = 0;
    uint64_t              stride  = 0;
    llvm::GlobalVariable* proto   = nullptr;
    // The descriptor of a container element's own element; null otherwise.
    llvm::GlobalVariable* inner = nullptr;
    // The `leaves` table of an aggregate element and its initializer; null otherwise.
    const llvm::GlobalVariable* table  = nullptr;
    const llvm::ConstantArray*  leaves = nullptr;

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

    bool looped() const {
        return nleaves > kElemKernelLeavesUnrolledMax;
    }
};

ElemOpsView view_of(const llvm::Constant* ops) {
    // A descriptor is its global, or the initializer that global holds.
    const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(ops);
    const auto* init   = llvm::cast<llvm::ConstantStruct>(global != nullptr ? global->getInitializer() : ops);
    assert(init->getNumOperands() == kFieldCount && "a descriptor has every rt_elem_ops field");
    ElemOpsView v;
    v.kind    = static_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(init->getOperand(kFieldKind))->getZExtValue());
    v.nleaves = static_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(init->getOperand(kFieldNleaves))->getZExtValue());
    v.stride  = llvm::cast<llvm::ConstantInt>(init->getOperand(kFieldStride))->getZExtValue();
    v.proto   = llvm::cast<llvm::GlobalVariable>(init->getOperand(kFieldProto));
    v.inner   = llvm::dyn_cast<llvm::GlobalVariable>(init->getOperand(kFieldInner));
    if (const auto* table = llvm::dyn_cast<llvm::GlobalVariable>(init->getOperand(kFieldLeaves))) {
        v.table  = table;
        v.leaves = llvm::cast<llvm::ConstantArray>(table->getInitializer());
    }
    assert((v.kind == rt_elem_aggregate) == (v.leaves != nullptr) && "only an aggregate has a leaf table");
    assert(v.leaves != nullptr ? v.leaves->getNumOperands() == v.nleaves : v.nleaves == 0);
    return v;
}

// The distinct leaf descriptors of an aggregate whose kind `wants`, in the order the
// table first lists them.
std::vector<const llvm::GlobalVariable*> leaf_types(const ElemOpsView& v, bool (*wants)(uint32_t kind)) {
    std::vector<const llvm::GlobalVariable*> out;
    for (uint32_t i = 0; i < v.nleaves; ++i) {
        const llvm::GlobalVariable* ops = v.leaf_ops(i);
        if (wants(view_of(ops).kind) && std::ranges::find(out, ops) == out.end()) {
            out.push_back(ops);
        }
    }
    return out;
}

// Leaves a destroy has work for: everything that owns heap. An event is never freed.
bool owns_heap(uint32_t kind) {
    return kind != rt_elem_pod && kind != rt_elem_event;
}

// Leaves a construction has work for beyond the prototype image: an event binds a
// new object (LRM Table 6-7), which no constant can say.
bool is_event(uint32_t kind) {
    return kind == rt_elem_event;
}

bool any_leaf(uint32_t /*kind*/) {
    return true;
}

// A constant as numbers appended to `key`: the same bytes in the same shape give the
// same numbers, whatever the (nominal) LLVM type they are written in. A prefix code,
// so two constants that differ in shape cannot give one sequence. What it does not
// know it keys by identity, which only ever separates.
void append_constant(const llvm::Constant* c, const llvm::DataLayout& dl, std::vector<uint64_t>& key) {
    if (const auto* number = llvm::dyn_cast<llvm::ConstantInt>(c)) {
        key.push_back(1);
        key.push_back(number->getBitWidth());
        const llvm::APInt& value = number->getValue();
        for (unsigned w = 0; w < value.getNumWords(); ++w) {
            key.push_back(value.getRawData()[w]);
        }
    } else if (llvm::isa<llvm::ConstantAggregateZero>(c) || llvm::isa<llvm::ConstantPointerNull>(c) ||
               llvm::isa<llvm::UndefValue>(c)) {
        key.push_back(2 + 4 * static_cast<uint64_t>(llvm::isa<llvm::UndefValue>(c)));
        key.push_back(dl.getTypeAllocSize(c->getType()));
    } else if (const auto* data = llvm::dyn_cast<llvm::ConstantDataSequential>(c)) {
        key.push_back(3);
        key.push_back(data->getNumElements());
        key.push_back(dl.getTypeAllocSize(data->getElementType()));
        for (const char byte : data->getRawDataValues()) {
            key.push_back(static_cast<unsigned char>(byte));
        }
    } else if (const auto* aggregate = llvm::dyn_cast<llvm::ConstantStruct>(c)) {
        const llvm::StructLayout* layout = dl.getStructLayout(aggregate->getType());
        key.push_back(4);
        key.push_back(aggregate->getNumOperands());
        key.push_back(layout->getSizeInBytes());
        for (unsigned i = 0; i < aggregate->getNumOperands(); ++i) {
            key.push_back(layout->getElementOffset(i));
            append_constant(aggregate->getOperand(i), dl, key);
        }
    } else if (const auto* array = llvm::dyn_cast<llvm::ConstantArray>(c)) {
        key.push_back(5);
        key.push_back(array->getNumOperands());
        key.push_back(dl.getTypeAllocSize(array->getType()->getElementType()));
        for (unsigned i = 0; i < array->getNumOperands(); ++i) {
            append_constant(array->getOperand(i), dl, key);
        }
    } else {
        key.push_back(6);
        key.push_back(reinterpret_cast<uintptr_t>(c));
    }
}

// Everything an element descriptor says that the engine and the kernels read, as
// numbers appended to `key`: two descriptors with the same numbers describe one
// layout, and what the kernels do is a function of the layout alone. A container's
// element and an aggregate's leaves are descriptors in their own right and are
// walked. The kernels' own pointers are not part of it: they follow from the rest.
void append_layout(const llvm::Constant* ops, const llvm::DataLayout& dl, std::vector<uint64_t>& key) {
    const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(ops);
    const auto* init   = llvm::cast<llvm::ConstantStruct>(global != nullptr ? global->getInitializer() : ops);
    assert(init->getNumOperands() == kFieldCount && "a descriptor has every rt_elem_ops field");
    for (unsigned f = 0; f < kFieldCount; ++f) {
        if (f == kFieldCopy || f == kFieldDestroy) {
            continue;
        }
        const llvm::Constant* field = init->getOperand(f);
        if (const auto* number = llvm::dyn_cast<llvm::ConstantInt>(field)) {
            key.push_back(number->getZExtValue());
            continue;
        }
        const auto* target = llvm::dyn_cast<llvm::GlobalVariable>(field);
        if (target == nullptr) {
            key.push_back(0); // a null pointer: no inner descriptor, no leaves, no handles
        } else if (f == kFieldInner) {
            key.push_back(1);
            append_layout(target, dl, key);
        } else if (f == kFieldLeaves) {
            const auto* leaves = llvm::cast<llvm::ConstantArray>(target->getInitializer());
            for (unsigned i = 0; i < leaves->getNumOperands(); ++i) {
                const auto* leaf = llvm::cast<llvm::ConstantStruct>(leaves->getOperand(i));
                key.push_back(llvm::cast<llvm::ConstantInt>(leaf->getOperand(0))->getZExtValue());
                append_layout(llvm::cast<llvm::GlobalVariable>(leaf->getOperand(1)), dl, key);
            }
        } else {
            append_constant(target->getInitializer(), dl, key); // the prototype image, the handle tables
        }
    }
}

} // namespace

llvm::Value* Backend::elem_byte_ptr(llvm::Value* base, uint64_t offset) {
    return offset == 0 ? base : b_.CreateConstInBoundsGEP1_64(b_.getInt8Ty(), base, offset);
}

// Walk the leaf table of an aggregate in a loop, threading one i64 through it: `body`
// is emitted once, inside the loop, and returns the value to carry to the next
// leaf. Returns the value after the last one. The table is a constant global, so
// the leaf's offset and descriptor are two loads; the loop's header block dominates
// its exit, which is why the value carried at the header is also the one after.
llvm::Value* Backend::emit_leaf_loop(
  const llvm::GlobalVariable*                                                                          table,
  uint32_t                                                                                             nleaves,
  llvm::Value*                                                                                         carried_init,
  const std::function<llvm::Value*(llvm::Value* offset, llvm::Value* leaf_ops, llvm::Value* carried)>& body) {
    assert(nleaves > 0 && "a loop over no leaves is the caller's to skip");
    auto* fn      = b_.GetInsertBlock()->getParent();
    auto* before  = b_.GetInsertBlock();
    auto* head    = llvm::BasicBlock::Create(ctx_, "leaf.head", fn);
    auto* leaf_bb = llvm::BasicBlock::Create(ctx_, "leaf.body", fn);
    auto* done    = llvm::BasicBlock::Create(ctx_, "leaf.done", fn);
    b_.CreateBr(head);

    b_.SetInsertPoint(head);
    auto* index   = b_.CreatePHI(b_.getInt64Ty(), 2, "leaf.index");
    auto* carried = b_.CreatePHI(b_.getInt64Ty(), 2, "leaf.carried");
    index->addIncoming(b_.getInt64(0), before);
    carried->addIncoming(carried_init, before);
    b_.CreateCondBr(b_.CreateICmpULT(index, b_.getInt64(nleaves)), leaf_bb, done);

    b_.SetInsertPoint(leaf_bb);
    llvm::Type*  table_ty = table->getValueType();
    llvm::Value* offset =
      b_.CreateLoad(b_.getInt64Ty(),
                    b_.CreateInBoundsGEP(
                      table_ty, const_cast<llvm::GlobalVariable*>(table), { b_.getInt64(0), index, b_.getInt32(0) }));
    llvm::Value* leaf_ops =
      b_.CreateLoad(raw_ptr_ty(),
                    b_.CreateInBoundsGEP(
                      table_ty, const_cast<llvm::GlobalVariable*>(table), { b_.getInt64(0), index, b_.getInt32(1) }));
    llvm::Value* next_carried = body(offset, leaf_ops, carried);
    index->addIncoming(b_.CreateAdd(index, b_.getInt64(1)), b_.GetInsertBlock());
    carried->addIncoming(next_carried, b_.GetInsertBlock());
    b_.CreateBr(head);

    b_.SetInsertPoint(done);
    return carried;
}

// `arm(D)` for the descriptor `leaf_ops` points at, among `types`: a compare and branch
// per distinct type, and nothing for a descriptor the list does not hold.
void Backend::emit_leaf_dispatch(const std::vector<const llvm::GlobalVariable*>&         types,
                                 llvm::Value*                                            leaf_ops,
                                 const std::function<void(const llvm::GlobalVariable*)>& arm) {
    assert(!types.empty() && "a dispatch over no descriptors is the caller's to skip");
    auto* fn   = b_.GetInsertBlock()->getParent();
    auto* join = llvm::BasicBlock::Create(ctx_, "leaf.join", fn);
    for (const llvm::GlobalVariable* ops : types) {
        auto* arm_bb = llvm::BasicBlock::Create(ctx_, "leaf.arm", fn);
        auto* next   = llvm::BasicBlock::Create(ctx_, "leaf.next", fn);
        b_.CreateCondBr(b_.CreateICmpEQ(leaf_ops, const_cast<llvm::GlobalVariable*>(ops)), arm_bb, next);
        b_.SetInsertPoint(arm_bb);
        arm(ops);
        b_.CreateBr(join);
        b_.SetInsertPoint(next);
    }
    b_.CreateBr(join);
    b_.SetInsertPoint(join);
}

// The interpreter's rt_elem_init: put a fresh element in the uninitialized slot `p`.
// An element's power-on image is the proto, except that an event is a NEW event
// object (LRM Table 6-7), so that is the one thing a constant cannot say.
//
// An aggregate's proto image already covers every leaf whose power-on value is a
// constant, which is every leaf but an event, so the interpreter's per-leaf
// re-initialization of a string or container leaf copies bytes already there and
// is left out here.
void Backend::emit_elem_init(const llvm::Constant* ops, llvm::Value* p) {
    const ElemOpsView v = view_of(ops);
    if (v.kind == rt_elem_event) {
        b_.CreateStore(b_.CreateCall(event_new_fn()), p);
        return;
    }
    b_.CreateMemCpy(p, llvm::MaybeAlign(), v.proto, llvm::MaybeAlign(), v.stride);
    if (!v.looped()) {
        for (uint32_t i = 0; i < v.nleaves; ++i) {
            if (is_event(view_of(v.leaf_ops(i)).kind)) {
                emit_elem_init(v.leaf_ops(i), elem_byte_ptr(p, v.leaf_offset(i)));
            }
        }
        return;
    }
    const std::vector<const llvm::GlobalVariable*> events = leaf_types(v, is_event);
    if (events.empty()) {
        return;
    }
    emit_leaf_loop(
      v.table, v.nleaves, b_.getInt64(0), [&](llvm::Value* offset, llvm::Value* leaf_ops, llvm::Value* carried) {
          emit_leaf_dispatch(events, leaf_ops, [&](const llvm::GlobalVariable* leaf) {
              emit_elem_init(leaf, b_.CreateInBoundsGEP(b_.getInt8Ty(), p, offset));
          });
          return carried;
      });
}

// The interpreter's rt_elem_copy: assign the element at `src` over the constructed one at `dst`.
void Backend::emit_elem_copy(const llvm::Constant* ops, llvm::Value* dst, llvm::Value* src, llvm::Value* site) {
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
// contract the interpreter relies on (collect_elem_leaves).
void Backend::emit_elem_copy_aggregate(const llvm::Constant* ops,
                                       llvm::Value*          dst,
                                       llvm::Value*          src,
                                       llvm::Value*          site) {
    const ElemOpsView v = view_of(ops);
    assert(v.kind == rt_elem_aggregate && "only an aggregate has leaves to walk");
    if (!v.looped()) {
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
        return;
    }

    // Past the cap: the same two passes over the table. Every leaf first, through the
    // arm for its descriptor ...
    const std::vector<const llvm::GlobalVariable*> types = leaf_types(v, any_leaf);
    emit_leaf_loop(
      v.table, v.nleaves, b_.getInt64(0), [&](llvm::Value* offset, llvm::Value* leaf_ops, llvm::Value* carried) {
          emit_leaf_dispatch(types, leaf_ops, [&](const llvm::GlobalVariable* leaf) {
              emit_elem_copy(leaf,
                             b_.CreateInBoundsGEP(b_.getInt8Ty(), dst, offset),
                             b_.CreateInBoundsGEP(b_.getInt8Ty(), src, offset),
                             site);
          });
          return carried;
      });
    // ... then the bytes between them, carrying the end of the previous leaf. A leaf
    // is as long as its own descriptor says, which differs between leaf types, so it
    // is read from the descriptor the table names rather than fixed here.
    const auto copy_gap = [&](llvm::Value* from, llvm::Value* to) {
        auto*        fn    = b_.GetInsertBlock()->getParent();
        auto*        gap   = llvm::BasicBlock::Create(ctx_, "leaf.gap", fn);
        auto*        after = llvm::BasicBlock::Create(ctx_, "leaf.gap.done", fn);
        llvm::Value* size  = b_.CreateSub(to, from);
        b_.CreateCondBr(b_.CreateICmpNE(size, b_.getInt64(0)), gap, after);
        b_.SetInsertPoint(gap);
        b_.CreateMemCpy(b_.CreateInBoundsGEP(b_.getInt8Ty(), dst, from),
                        llvm::MaybeAlign(),
                        b_.CreateInBoundsGEP(b_.getInt8Ty(), src, from),
                        llvm::MaybeAlign(),
                        size);
        b_.CreateBr(after);
        b_.SetInsertPoint(after);
    };
    llvm::Value* end = emit_leaf_loop(
      v.table, v.nleaves, b_.getInt64(0), [&](llvm::Value* offset, llvm::Value* leaf_ops, llvm::Value* previous_end) {
          copy_gap(previous_end, offset);
          llvm::Value* leaf_stride =
            b_.CreateLoad(b_.getInt64Ty(), b_.CreateStructGEP(elem_ops_ty(), leaf_ops, kFieldStride));
          return b_.CreateAdd(offset, leaf_stride);
      });
    copy_gap(end, b_.getInt64(v.stride));
}

// The interpreter's rt_elem_deinit: release whatever the element in `p` owns.
void Backend::emit_elem_deinit(const llvm::Constant* ops, llvm::Value* p) {
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
        case rt_elem_aggregate: {
            if (!v.looped()) {
                for (uint32_t i = 0; i < v.nleaves; ++i) {
                    emit_elem_deinit(v.leaf_ops(i), elem_byte_ptr(p, v.leaf_offset(i)));
                }
                return;
            }
            const std::vector<const llvm::GlobalVariable*> types = leaf_types(v, owns_heap);
            if (types.empty()) {
                return;
            }
            emit_leaf_loop(v.table,
                           v.nleaves,
                           b_.getInt64(0),
                           [&](llvm::Value* offset, llvm::Value* leaf_ops, llvm::Value* carried) {
                               emit_leaf_dispatch(types, leaf_ops, [&](const llvm::GlobalVariable* leaf) {
                                   emit_elem_deinit(leaf, b_.CreateInBoundsGEP(b_.getInt8Ty(), p, offset));
                               });
                               return carried;
                           });
            return;
        }
        default:
            llvm_unreachable("an element descriptor has a known kind");
    }
}

// void copy(void* dst, const void* src, const rt_alloc_site* site): construct a copy
// of the element at `src` in the uninitialized slot `dst`. This is what a push
// does to the new slot, and the engine calls it through rt_elem_ops::copy.
llvm::Function* Backend::elem_copy_kernel(const llvm::Constant* ops) {
    auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { raw_ptr_ty(), raw_ptr_ty(), raw_ptr_ty() }, false);
    auto* fn   = llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.elem.ops.copy", env_->llmod);
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
llvm::Function* Backend::elem_destroy_kernel(const llvm::Constant* ops) {
    auto* fnty = llvm::FunctionType::get(b_.getVoidTy(), { raw_ptr_ty() }, false);
    auto* fn   = llvm::Function::Create(fnty, llvm::Function::InternalLinkage, "clotho.elem.ops.destroy", env_->llmod);
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->getArg(0)->setName("slot");
    const llvm::IRBuilderBase::InsertPointGuard guard(b_);
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
    emit_elem_deinit(ops, fn->getArg(0));
    b_.CreateRetVoid();
    return fn;
}

// The descriptor of a queue's element: the initializer of elem_ops_for as a global of
// its own, with the element's kernels filled in when it owns heap. A plain-bytes
// element has no kernels to call (the engine copies it with memcpy), so its
// descriptor is the plain one. The kernels are the only way the rt_queue_*
// functions copy or destroy a managed element, so a queue is never handed a
// descriptor without them, whichever of those functions it reaches, and the
// descriptor chain (`inner`) of a queue of queues is the same form at every level.
// The plain global is not made for an element only queues use: the initializer is
// all the kernels read.
llvm::Constant* Backend::queue_elem_ops(Ref elem_type) {
    llvm::ConstantStruct* init = elem_ops_init(elem_type);
    if (view_of(init).kind == rt_elem_pod) {
        return elem_ops_for(elem_type);
    }
    const auto key = static_cast<uint32_t>(elem_type);
    if (const auto it = env_->elem_ops_kernels.find(key); it != env_->elem_ops_kernels.end()) {
        return it->second;
    }
    // Two element types of one layout are one descriptor with one pair of kernels:
    // what the kernels do is a function of the layout alone.
    std::vector<uint64_t> layout_key;
    append_layout(init, env_->llmod.getDataLayout(), layout_key);
    if (const auto it = env_->elem_ops_layouts.find(layout_key); it != env_->elem_ops_layouts.end()) {
        env_->elem_ops_kernels.emplace(key, it->second);
        return it->second;
    }
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
    fields[kFieldCopy]    = elem_copy_kernel(init);
    fields[kFieldDestroy] = elem_destroy_kernel(init);
    auto* kernels         = new llvm::GlobalVariable(env_->llmod,
                                                     elem_ops_ty(),
                                                     /*isConstant=*/true,
                                                     llvm::GlobalValue::InternalLinkage,
                                                     llvm::ConstantStruct::get(elem_ops_ty(), fields),
                                                     "clotho.elem.ops");
    env_->elem_ops_kernels.emplace(key, kernels);
    env_->elem_ops_layouts.emplace(std::move(layout_key), kernels);
    return kernels;
}

} // namespace clotho::ir::backend
