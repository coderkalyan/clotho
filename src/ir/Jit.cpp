#include "Jit.h"

#include <csetjmp>
#include <memory>
#include <mutex>

#include <llvm/ExecutionEngine/JITEventListener.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CodeGen.h>
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

int run_jit(const Module& m, Timings* timings, const SimulationConfig& config) {
    ensure_native_target();

    // LLVM compile phase: build the module (IR gen + coroutine/opt passes), then
    // below, JIT codegen triggered by looking up main.
    const auto t_llvm = std::chrono::steady_clock::now();

    // Each run owns its context: the design module is built into it, then both
    // are moved into a ThreadSafeModule the JIT takes over.
    auto ctx = std::make_unique<llvm::LLVMContext>();
    // The JIT needs runnable code: coroutine lowering is mandatory regardless
    // of what the caller's config says (optimize=false is an emit_llvm
    // inspection mode).
    SimulationConfig jit_config = config;
    jit_config.optimize         = true;
    auto mod                    = build_module(m, *ctx, jit_config);
    if (!mod) {
        return -1; // build_module already reported the reason.
    }
    llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));

    // Pin the JIT's machine-code generation to -O0. The IR is already optimized
    // by the backend's own pass pipeline before it reaches ORC, so the codegen
    // optimizer adds compile time for little gain on the JIT path.
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        llvm::errs() << "jit: " << llvm::toString(jtmb.takeError()) << "\n";
        return -1;
    }
    jtmb->setCodeGenOptLevel(llvm::CodeGenOptLevel::None);

    llvm::orc::LLJITBuilder builder;
    builder.setJITTargetMachineBuilder(std::move(*jtmb));

    // Register the perf jitdump listener on the object-linking layer. The
    // default LLJIT layer is RTDyldObjectLinkingLayer; supply our own so we can
    // attach the listener. createPerfJITEventListener returns null unless LLVM
    // was built -DLLVM_USE_PERF=ON; skip registration (no symbols in perf) then.
    if (config.perf_jitdump) {
        builder.setObjectLinkingLayerCreator(
          [](llvm::orc::ExecutionSession& es)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
              auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
                es, [](const llvm::MemoryBuffer&) {
                    return std::make_unique<llvm::SectionMemoryManager>();
                });
              if (auto* perf = llvm::JITEventListener::createPerfJITEventListener()) {
                  // Leaked deliberately: the listener must outlive the layer, and
                  // both live until process exit. LLVM offers no owning handle.
                  layer->registerJITEventListener(*perf);
              } else {
                  llvm::errs() << "jit: perf jitdump requested but this LLVM lacks "
                                  "perf support (LLVM_USE_PERF); no symbols emitted\n";
              }
              return layer;
          });
    }

    auto jit = builder.create();
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
    expose("rt_fwrite", &rt_fwrite);
    expose("rt_fopen_mcd", &rt_fopen_mcd);
    expose("rt_fopen_fd", &rt_fopen_fd);
    expose("rt_fclose", &rt_fclose);
    expose("rt_fgetc", &rt_fgetc);
    expose("rt_ungetc", &rt_ungetc);
    expose("rt_feof", &rt_feof);
    expose("rt_ftell", &rt_ftell);
    expose("rt_fseek", &rt_fseek);
    expose("rt_rewind", &rt_rewind);
    expose("rt_fgets", &rt_fgets);
    expose("rt_sformat", &rt_sformat);
    expose("rt_sformat_str", &rt_sformat_str);
    expose("rt_ferror", &rt_ferror);
    expose("rt_fscanf", &rt_fscanf);
    expose("rt_sscanf", &rt_sscanf);
    expose("rt_fread", &rt_fread);
    expose("rt_timeformat_init", &rt_timeformat_init);
    expose("rt_timeformat_set", &rt_timeformat_set);
    expose("rt_finish", &rt_finish);
    expose("rt_sim_run", &rt_sim_run);
    expose("rt_fflush", &rt_fflush);
    expose("rt_readmem", &rt_readmem);
    expose("rt_random", &rt_random);
    expose("rt_alloc", &rt_alloc);
    expose("rt_free", &rt_free);
    expose("rt_nba_arr_push", &rt_nba_arr_push);
    expose("rt_nba_arr_pending", &rt_nba_arr_pending);
    expose("rt_nba_arr_commit", &rt_nba_arr_commit);
    expose("rt_nba_arr_dirty", &rt_nba_arr_dirty);
    expose("rt_nba_arr_reset", &rt_nba_arr_reset);
    expose("rt_string_init", &rt_string_init);
    expose("rt_string_deinit", &rt_string_deinit);
    expose("rt_string_assign", &rt_string_assign);
    expose("rt_string_assign_bytes", &rt_string_assign_bytes);
    expose("rt_string_len", &rt_string_len);
    expose("rt_string_data", &rt_string_data);
    expose("rt_string_concat", &rt_string_concat);
    expose("rt_string_repl", &rt_string_repl);
    expose("rt_string_cmp", &rt_string_cmp);
    expose("rt_string_icmp", &rt_string_icmp);
    expose("rt_string_substr", &rt_string_substr);
    expose("rt_string_toupper", &rt_string_toupper);
    expose("rt_string_tolower", &rt_string_tolower);
    expose("rt_string_putc", &rt_string_putc);
    expose("rt_string_getc", &rt_string_getc);
    expose("rt_string_from_packed", &rt_string_from_packed);
    expose("rt_string_to_packed", &rt_string_to_packed);
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

    // Resolve the compiler-rt/libgcc builtins the backend emits for wide
    // (>64-bit) arithmetic: __multi3 for multiply, __udivti3 / __umodti3 and
    // their signed siblings for divide/modulo. They are linked into this process
    // (libgcc_s), so expose the process symbol table to the JIT for the libcalls.
    auto builtins = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
      (*jit)->getDataLayout().getGlobalPrefix());
    if (!builtins) {
        llvm::errs() << "jit: " << llvm::toString(builtins.takeError()) << "\n";
        return -1;
    }
    jd.addGenerator(std::move(*builtins));

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

    // Simulation phase: run the compiled design. A $finish longjmps back here
    // (rt_finish is noreturn to the design) so the embedding process survives;
    // the sim's live coroutine frames are abandoned, which is fine — it is over.
    const auto   t_sim = std::chrono::steady_clock::now();
    std::jmp_buf finish_env;
    // Volatile: an auto variable modified between setjmp and longjmp is
    // otherwise indeterminate after the jump.
    volatile int rc = 0;
    if (setjmp(finish_env) == 0) {
        rt_finish_host(&finish_env);
        rc = fn();
    }
    rt_finish_host(nullptr);
    if (timings != nullptr) {
        timings->sim_ms += ms_since(t_sim);
    }
    return rc;
}

} // namespace clotho::ir
