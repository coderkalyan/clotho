#include "Jit.h"

#include <memory>
#include <mutex>

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "LlvmBackend.h"
#include "runtime/Runtime.h"

namespace clotho::ir {

namespace {

// Native target/codegen must be initialized once before any LLJIT is built;
// without the AsmPrinter the JIT can't lower IR to machine code for the host.
void ensure_native_target() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
}

} // namespace

int run_jit(const Module& m, Timings* timings) {
    ensure_native_target();

    // LLVM compile phase: build the module (IR gen + coroutine/opt passes), then
    // below, JIT codegen triggered by looking up main.
    const auto t_llvm = std::chrono::steady_clock::now();

    // Each run owns its context: the design module is built into it, then both
    // are moved into a ThreadSafeModule the JIT takes over.
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = build_module(m, *ctx);
    if (!mod) {
        return -1; // build_module already reported the reason.
    }
    llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));

    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit) {
        llvm::errs() << "jit: " << llvm::toString(jit.takeError()) << "\n";
        return -1;
    }

    // Expose the native runtime to the JIT'd design as absolute symbols, mapped
    // directly to the host function addresses. The runtime is plain native code
    // statically linked into this process.
    auto&                jd = (*jit)->getMainJITDylib();
    llvm::orc::SymbolMap rt_syms;
    auto                 expose = [&](llvm::StringRef name, auto* fn) {
        rt_syms[(*jit)->mangleAndIntern(name)] = {
            llvm::orc::ExecutorAddr::fromPtr(fn),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable,
        };
    };
    expose("rt_display", &rt_display);
    expose("rt_alloc", &rt_alloc);
    expose("rt_free", &rt_free);
    expose("rt_wheel_init", &rt_wheel_init);
    expose("rt_wheel_deinit", &rt_wheel_deinit);
    expose("rt_wheel_now", &rt_wheel_now);
    expose("rt_wheel_pending", &rt_wheel_pending);
    expose("rt_wheel_schedule", &rt_wheel_schedule);
    expose("rt_wheel_tick", &rt_wheel_tick);
    if (auto err = jd.define(llvm::orc::absoluteSymbols(std::move(rt_syms)))) {
        llvm::errs() << "jit: " << llvm::toString(std::move(err)) << "\n";
        return -1;
    }

    if (auto err = (*jit)->addIRModule(std::move(tsm))) {
        llvm::errs() << "jit: " << llvm::toString(std::move(err)) << "\n";
        return -1;
    }

    // Looking up `main` materializes its module through the compile layer, i.e.
    // eagerly compiles the whole design (one module, reachable from main).
    auto sym = (*jit)->lookup("main");
    if (!sym) {
        llvm::errs() << "jit: " << llvm::toString(sym.takeError()) << "\n";
        return -1;
    }

    auto* fn = sym->toPtr<int (*)()>();
    if (timings != nullptr) {
        timings->llvm_ms += ms_since(t_llvm);
    }

    // Simulation phase: run the compiled design.
    const auto t_sim = std::chrono::steady_clock::now();
    const int  rc    = fn();
    if (timings != nullptr) {
        timings->sim_ms += ms_since(t_sim);
    }
    return rc;
}

} // namespace clotho::ir
