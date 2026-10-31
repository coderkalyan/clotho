// Links the runtime's own functions, compiled to bitcode at build time, into a generated module.

#include "ir/llvm/RuntimeBitcode.h"

#include "runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/ADT/StringSet.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Transforms/IPO/Internalize.h>
#include <llvm/Transforms/Utils/Cloning.h>

namespace clotho::ir {

namespace {

// The engine functions a design always gets inlined: one element, constant work.
constexpr std::array<std::string_view, 4> kInlined = { "rt_queue_push_back",
                                                       "rt_queue_push_front",
                                                       "rt_queue_pop_back",
                                                       "rt_queue_pop_front" };

// The engine functions that move a run of elements, and which of their parameters is
// the element descriptor (runtime/Queue.cpp). A copy of one per descriptor has the
// stride and the kernel calls folded in; the call to it drops that parameter.
struct Specialized {
    std::string_view name;
    unsigned         descriptor_param;
};
constexpr std::array<Specialized, 3> kSpecialized = {
    Specialized{ "rt_queue_insert", 1 },
    Specialized{ "rt_queue_delete_idx", 1 },
    Specialized{ "rt_queue_append_range", 2 },
};

// What the bitcode defines: the seven functions above, and what they call is the
// runtime's. The rest of the queue's functions (assign, delete_all, move, ...) are the
// runtime's own and are not in it, so a module that declares only those has nothing
// to link, and nothing is parsed for it. That includes every design with a queue, whose
// teardown calls rt_queue_delete_all, and every one without.
bool is_engine_entry(std::string_view name) {
    return std::ranges::find(kInlined, name) != kInlined.end() ||
           std::ranges::any_of(kSpecialized, [&](const Specialized& s) { return s.name == name; });
}

// The module's own name for the engine's copy of `name`. It is not the runtime's name:
// a call that runs at most once keeps calling the runtime's function of that name, and
// both can be in one module.
std::string engine_copy_name(std::string_view name) {
    return std::string(name) + ".engine";
}

// The calls of `f` that may run more than once, which is all of them that the backend
// has not marked cold (Backend::queue_engine_call).
std::vector<llvm::CallBase*> repeated_calls(llvm::Function& f) {
    std::vector<llvm::CallBase*> calls;
    for (llvm::User* user : f.users()) {
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(user);
            call != nullptr && call->getCalledFunction() == &f && !call->hasFnAttr(llvm::Attribute::Cold)) {
            calls.push_back(call);
        }
    }
    return calls;
}

// Whether the module calls an engine function from somewhere that may run more than once.
bool has_repeated_engine_call(llvm::Module& m) {
    for (llvm::Function& f : m) {
        if (f.isDeclaration() && is_engine_entry(f.getName()) && !repeated_calls(f).empty()) {
            return true;
        }
    }
    return false;
}

// The bitcode is compiled for clang's baseline CPU and carries that CPU as string
// attributes (target-cpu, target-features, tune-cpu, ...). A caller with none, which
// is every generated function (the target machine picks the CPU), is "conflicting"
// to the inliner and the call is refused, so they go; what is left are the
// attributes that describe the code (nounwind, memory, ...). The stack protector and
// unwind table a distribution's clang adds by default describe the build, not the
// code, and are not wanted in a design's functions either.
void strip_build_attributes(llvm::Function& f) {
    std::vector<std::string> names;
    for (const llvm::Attribute a : f.getAttributes().getFnAttrs()) {
        if (a.isStringAttribute()) {
            names.push_back(a.getKindAsString().str());
        }
    }
    for (const std::string& name : names) {
        f.removeFnAttr(name);
    }
    for (const llvm::Attribute::AttrKind kind : { llvm::Attribute::UWTable,
                                                  llvm::Attribute::StackProtect,
                                                  llvm::Attribute::StackProtectStrong,
                                                  llvm::Attribute::StackProtectReq,
                                                  llvm::Attribute::NoCfCheck }) {
        f.removeFnAttr(kind);
    }
}

// Give the engine's copy of every engine function the bitcode defines a name of its own
// (engine_copy_name), and point the calls that may run more than once at it, with a
// declaration in `m` for the link to resolve. A call that runs at most once stays a
// call of the runtime's function, which the bitcode no longer defines under that name,
// so the link cannot resolve it either. Returns whether any call was pointed there.
bool route_repeated_calls(llvm::Module& m, llvm::Module& src) {
    for (llvm::Function& f : src) {
        if (!f.isDeclaration() && is_engine_entry(f.getName())) {
            f.setName(engine_copy_name(f.getName()));
        }
    }
    std::vector<llvm::Function*> entries;
    for (llvm::Function& f : m) {
        if (f.isDeclaration() && is_engine_entry(f.getName())) {
            entries.push_back(&f);
        }
    }
    bool routed = false;
    for (llvm::Function* runtime : entries) {
        const std::string name = engine_copy_name(runtime->getName());
        const std::vector<llvm::CallBase*> calls = repeated_calls(*runtime);
        if (calls.empty() || src.getFunction(name) == nullptr) {
            continue;
        }
        auto* copy = llvm::Function::Create(runtime->getFunctionType(), llvm::GlobalValue::ExternalLinkage, name, &m);
        for (llvm::CallBase* call : calls) {
            call->setCalledFunction(copy);
        }
        if (runtime->use_empty()) {
            runtime->eraseFromParent();
        }
        routed = true;
    }
    return routed;
}

// `generic` with its descriptor parameter replaced by the constant `ops`, as a new
// internal function without that parameter. Cloned with the constant propagated and
// the branches it decides pruned, so what is copied is the code this descriptor
// needs; the optimizer's own folding does the rest.
llvm::Function* clone_for_descriptor(llvm::Function& generic, unsigned descriptor_param, llvm::GlobalVariable* ops) {
    const llvm::FunctionType* generic_ty = generic.getFunctionType();
    assert(descriptor_param < generic_ty->getNumParams() && "the descriptor is a parameter");
    assert(generic_ty->getParamType(descriptor_param)->isPointerTy() && "the descriptor is passed by pointer");
    std::vector<llvm::Type*> params;
    for (unsigned i = 0; i < generic_ty->getNumParams(); ++i) {
        if (i != descriptor_param) {
            params.push_back(generic_ty->getParamType(i));
        }
    }
    auto* clone = llvm::Function::Create(llvm::FunctionType::get(generic_ty->getReturnType(), params, false),
                                         llvm::GlobalValue::InternalLinkage,
                                         generic.getName() + "." + ops->getName(),
                                         generic.getParent());
    llvm::ValueToValueMapTy vmap;
    unsigned                next = 0;
    for (llvm::Argument& arg : generic.args()) {
        vmap[&arg] = arg.getArgNo() == descriptor_param ? static_cast<llvm::Value*>(ops) : clone->getArg(next++);
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
    llvm::CloneAndPruneFunctionInto(clone, &generic, vmap, /*ModuleLevelChanges=*/false, returns);
    // Out of line is the point: the copy is shared by every call site with this
    // descriptor, and inlining it back into each would undo that.
    clone->addFnAttr(llvm::Attribute::NoInline);
    return clone;
}

// The field numbers of rt_elem_ops (runtime/Runtime.h) the link step reads; the
// backend's mirror of the struct (ir/llvm/RuntimeAbi.cpp) keeps them in lockstep.
constexpr unsigned kDescriptorKindField   = 0;
constexpr unsigned kDescriptorStrideField = 2;

// What decides which copy of an engine function a descriptor gets. A plain-bytes
// element (rt_elem_pod) is moved and copied by its stride alone: the engine reads no
// other field of its descriptor, and hands it on only to rt_queue_drop_tail, which
// reads the same two, so every plain-bytes descriptor of one stride can use the copy
// built for the first, and a design with many such element types (a queue of each
// width) pays for a few copies, not one per type. Any other descriptor has kernels of
// its own and so a copy of its own.
struct CopyKey {
    const llvm::GlobalVariable* descriptor = nullptr; // null for plain bytes
    uint64_t                    stride     = 0;       // 0 otherwise

    auto operator<=>(const CopyKey&) const = default;
};

CopyKey copy_key(const llvm::GlobalVariable* ops) {
    if (!ops->hasInitializer()) {
        return { ops, 0 };
    }
    const auto* init = llvm::cast<llvm::ConstantStruct>(ops->getInitializer());
    assert(init->getNumOperands() > kDescriptorStrideField && "a descriptor has every rt_elem_ops field");
    const uint64_t kind = llvm::cast<llvm::ConstantInt>(init->getOperand(kDescriptorKindField))->getZExtValue();
    if (kind != rt_elem_pod) {
        return { ops, 0 };
    }
    return { nullptr, llvm::cast<llvm::ConstantInt>(init->getOperand(kDescriptorStrideField))->getZExtValue() };
}

// Replace every call to `generic` that names a constant descriptor with a call to that
// descriptor's copy (copy_key), and drop `generic` when nothing else calls it.
void specialize_on_descriptor(llvm::Function& generic, unsigned descriptor_param) {
    std::vector<llvm::CallBase*> sites;
    for (llvm::User* user : generic.users()) {
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(user);
            call != nullptr && call->getCalledFunction() == &generic) {
            sites.push_back(call);
        }
    }
    std::map<CopyKey, llvm::Function*> clones;
    for (llvm::CallBase* call : sites) {
        auto* ops = llvm::dyn_cast<llvm::GlobalVariable>(call->getArgOperand(descriptor_param));
        if (ops == nullptr) {
            continue;
        }
        llvm::Function*& clone = clones[copy_key(ops)];
        if (clone == nullptr) {
            clone = clone_for_descriptor(generic, descriptor_param, ops);
        }
        std::vector<llvm::Value*> args;
        for (unsigned i = 0; i < call->arg_size(); ++i) {
            if (i != descriptor_param) {
                args.push_back(call->getArgOperand(i));
            }
        }
        llvm::IRBuilder<> at(call);
        at.CreateCall(clone->getFunctionType(), clone, args);
        call->eraseFromParent();
    }
    if (generic.use_empty()) {
        generic.eraseFromParent();
    }
}

// What the engine functions linked into `m` are to become (header comment).
void apply_inlining_policy(llvm::Module& m) {
    for (const std::string_view name : kInlined) {
        if (llvm::Function* f = m.getFunction(engine_copy_name(name)); f != nullptr && !f->isDeclaration()) {
            f->addFnAttr(llvm::Attribute::AlwaysInline);
        }
    }
    for (const Specialized& s : kSpecialized) {
        if (llvm::Function* f = m.getFunction(engine_copy_name(s.name)); f != nullptr && !f->isDeclaration()) {
            specialize_on_descriptor(*f, s.descriptor_param);
        }
    }
}

} // namespace

bool link_runtime_bitcode(llvm::Module& m) {
    if (!has_repeated_engine_call(m)) {
        return false;
    }
    const std::span<const unsigned char> image = runtime_bitcode_image();
    if (image.empty()) {
        return false;
    }
    const llvm::MemoryBufferRef buffer(llvm::StringRef(reinterpret_cast<const char*>(image.data()), image.size()),
                                       "clotho.runtime.bc");

    // Lazy: only the functions the link asks for are read out of the image.
    auto parsed = llvm::getLazyBitcodeModule(buffer, m.getContext());
    if (!parsed) {
        llvm::report_fatal_error(llvm::Twine("clotho: the embedded runtime bitcode does not parse: ") +
                                 llvm::toString(parsed.takeError()));
    }
    std::unique_ptr<llvm::Module> src = std::move(*parsed);
    if (!route_repeated_calls(m, *src)) {
        return false;
    }

    // The bitcode was compiled for the machine clotho was built on, the module is
    // for the one it will run on or be built for (--cpu changes the CPU, never the
    // triple), and generated code reads the queue handle by byte offset, so the
    // layout of the two is the module's to state.
    src->setTargetTriple(m.getTargetTriple());
    src->setDataLayout(m.getDataLayout());
    for (llvm::Function& f : *src) {
        strip_build_attributes(f);
        if (f.isDeclaration()) {
            // A declaration the bitcode makes (rt_queue_grow) is as external as the
            // ones the backend makes: the runtime may sit beyond a direct call's
            // reach in a JIT.
            f.setDSOLocal(false);
        }
    }
    // What describes the build (module flags, the compiler's ident, errno's TBAA
    // node) is not the design's to inherit.
    std::vector<llvm::NamedMDNode*> metadata;
    for (llvm::NamedMDNode& node : src->named_metadata()) {
        metadata.push_back(&node);
    }
    for (llvm::NamedMDNode* node : metadata) {
        node->eraseFromParent();
    }

    // Internalize what came in, and only that: the module's own definitions are
    // not this link's to change.
    const auto internalize = [](llvm::Module& dest, const llvm::StringSet<>& linked) {
        llvm::internalizeModule(dest,
                                [&linked](const llvm::GlobalValue& gv) { return !linked.contains(gv.getName()); });
    };
    if (llvm::Linker::linkModules(m, std::move(src), llvm::Linker::Flags::LinkOnlyNeeded, internalize)) {
        llvm::report_fatal_error("clotho: cannot link the embedded runtime bitcode into the design");
    }
    apply_inlining_policy(m);
    return true;
}

} // namespace clotho::ir
