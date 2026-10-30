// Links the runtime's own functions, compiled to bitcode at build time, into a generated module.

#include "ir/llvm/RuntimeBitcode.h"

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Function.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Transforms/IPO/Internalize.h>

namespace clotho::ir {

namespace {

// The bitcode defines only the queue engine, and the runtime names every queue
// operation rt_queue_*. A module with no declaration of that shape has nothing to
// link, which is what lets a design without queues skip the whole step.
constexpr std::string_view kEnginePrefix = "rt_queue_";

bool declares_engine_function(const llvm::Module& m) {
    for (const llvm::Function& f : m) {
        if (f.isDeclaration() && f.getName().starts_with(kEnginePrefix)) {
            return true;
        }
    }
    return false;
}

// The bitcode is compiled for clang's baseline CPU and carries that CPU as string
// attributes (target-cpu, target-features, tune-cpu, ...). A caller with none, which
// is every generated function (the target machine picks the CPU), is "conflicting"
// to the inliner and the call is refused, so they go; what is left are the
// attributes that describe the code (nounwind, memory, ...).
void strip_string_attributes(llvm::Function& f) {
    std::vector<std::string> names;
    for (const llvm::Attribute a : f.getAttributes().getFnAttrs()) {
        if (a.isStringAttribute()) {
            names.push_back(a.getKindAsString().str());
        }
    }
    for (const std::string& name : names) {
        f.removeFnAttr(name);
    }
}

// Whether the bitcode defines a function the module only declares.
bool defines_declared_function(const llvm::Module& src, const llvm::Module& m) {
    for (const llvm::Function& f : src) {
        if (f.isDeclaration()) {
            continue;
        }
        const llvm::Function* d = m.getFunction(f.getName());
        if (d != nullptr && d->isDeclaration()) {
            return true;
        }
    }
    return false;
}

} // namespace

bool link_runtime_bitcode(llvm::Module& m) {
    if (!declares_engine_function(m)) {
        return false;
    }
    const std::span<const unsigned char> image = runtime_bitcode_image();
    assert(!image.empty() && "the build embeds the runtime bitcode");
    const llvm::MemoryBufferRef buffer(llvm::StringRef(reinterpret_cast<const char*>(image.data()), image.size()),
                                       "clotho.runtime.bc");

    // Lazy: only the functions the link asks for are read out of the image.
    auto parsed = llvm::getLazyBitcodeModule(buffer, m.getContext());
    if (!parsed) {
        llvm::report_fatal_error(llvm::Twine("clotho: the embedded runtime bitcode does not parse: ") +
                                 llvm::toString(parsed.takeError()));
    }
    std::unique_ptr<llvm::Module> src = std::move(*parsed);
    if (!defines_declared_function(*src, m)) {
        return false;
    }

    // The bitcode was compiled for the machine clotho was built on, the module is
    // for the one it will run on or be built for (--cpu changes the CPU, never the
    // triple), and generated code reads the queue handle by byte offset, so the
    // layout of the two is the module's to state.
    src->setTargetTriple(m.getTargetTriple());
    src->setDataLayout(m.getDataLayout());
    for (llvm::Function& f : *src) {
        strip_string_attributes(f);
        if (f.isDeclaration()) {
            // A declaration the bitcode makes (rt_queue_grow) is as external as the
            // ones the backend makes: the runtime may sit beyond a direct call's
            // reach in a JIT.
            f.setDSOLocal(false);
        }
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
    return true;
}

} // namespace clotho::ir
