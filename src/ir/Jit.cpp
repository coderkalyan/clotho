#include "Jit.h"

#include <csetjmp>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
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
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Threading.h>
#include <llvm/Support/ThreadPool.h>
#include <llvm/Support/raw_ostream.h>

#include "LlvmBackend.h"
#include "runtime/Runtime.h"
#include "runtime/svdpi.h"

#if defined(__MINGW32__)
// libgcc's stack-probe helper. Declared here only to take its address; the
// calling convention is private to the compiler, so it is never called from C++.
extern "C" void ___chkstk_ms(void);
#endif

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

int run_jit(const Module& m, Timings* timings, const SimulationConfig& config,
           std::span<const std::string_view> plusargs) {
    ensure_native_target();

    // LLVM compile phase: build the module (IR gen + coroutine/opt passes), then
    // below, JIT codegen triggered by looking up main.
    const auto t_llvm = std::chrono::steady_clock::now();

    // Each run owns its context: the design modules are built into it, then
    // (parallel_compile off, or a single module) moved into ThreadSafeModules
    // sharing one ThreadSafeContext the JIT takes over — same as before. With
    // parallel_compile on and >1 module, each unit is instead migrated into
    // its own fresh LLVMContext below (a bitcode round-trip) so unrelated
    // modules can be optimized and codegen'd concurrently: LLVMContext is not
    // thread-safe, and ORC's ThreadSafeContext lock serializes any modules
    // that still share one (docs/design/module-split.md).
    auto ctx = std::make_unique<llvm::LLVMContext>();
    // The JIT needs runnable code: coroutine lowering is mandatory regardless
    // of what the caller's config says (optimize=false is an emit_llvm
    // inspection mode). Deferred (build_modules_unoptimized, not
    // build_modules) so optimize_module can run per module below, after
    // context migration when parallel.
    SimulationConfig jit_config = config;
    jit_config.optimize         = true;
    const bool stat             = getenv("CLOTHO_PARALLEL_STAT") != nullptr;
    auto       built            = build_modules_unoptimized(m, *ctx, jit_config);
    if (built.modules.empty()) {
        return -1; // build_modules_unoptimized already reported the reason.
    }
    if (stat) {
        fprintf(stderr, "PARALLEL_STAT build_modules_unoptimized: %.1f ms\n", ms_since(t_llvm));
    }

    // Parallel compile needs its own context per module to mean anything;
    // a single-module build (e.g. --no-split) falls through to the plain
    // sequential path unconditionally.
    const bool     parallel = config.parallel_compile && built.modules.size() > 1;
    const unsigned n_jobs =
      parallel ? llvm::hardware_concurrency(config.compile_jobs).compute_thread_count() : 1;

    // Machine-code generation matches the IR tier: -O0/FastISel for tier 0
    // (the backend pipeline already did the cheap cleanup; codegen quality is
    // deliberately traded for compile speed) and Aggressive under -O.
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        llvm::errs() << "jit: " << llvm::toString(jtmb.takeError()) << "\n";
        return -1;
    }
    jtmb->setCodeGenOptLevel(config.o3 ? llvm::CodeGenOptLevel::Aggressive
                                       : llvm::CodeGenOptLevel::None);

    llvm::orc::LLJITBuilder builder;
    builder.setJITTargetMachineBuilder(std::move(*jtmb));
    if (parallel) {
        // Each unit module is migrated to its own LLVMContext below (bitcode
        // round-trip), so ORC can materialize them concurrently: this makes
        // LLJIT select ConcurrentIRCompiler (a fresh TargetMachine per
        // compile call) and dispatch per-module codegen onto its own thread
        // pool when lookup("main") transitively pulls in every unit.
        builder.setNumCompileThreads(n_jobs);
    }

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
    expose("rt_strobe_pend", &rt_strobe_pend);
    expose("rt_strobe_pop", &rt_strobe_pop);
    expose("rt_monitor_set", &rt_monitor_set);
    expose("rt_monitor_enable", &rt_monitor_enable);
    expose("rt_monitor_active_ramp", &rt_monitor_active_ramp);
    expose("rt_monitor_emit", &rt_monitor_emit);
    expose("rt_fopen_mcd", &rt_fopen_mcd);
    expose("rt_fopen_fd", &rt_fopen_fd);
    expose("rt_system", &rt_system);
    expose("rt_fclose", &rt_fclose);
    expose("rt_fgetc", &rt_fgetc);
    expose("rt_ungetc", &rt_ungetc);
    expose("rt_feof", &rt_feof);
    expose("rt_ftell", &rt_ftell);
    expose("rt_fseek", &rt_fseek);
    expose("rt_rewind", &rt_rewind);
    expose("rt_fgets", &rt_fgets);
    expose("rt_fgets_str", &rt_fgets_str);
    expose("rt_sformat", &rt_sformat);
    expose("rt_sformat_str", &rt_sformat_str);
    expose("rt_ferror", &rt_ferror);
    expose("rt_ferror_str", &rt_ferror_str);
    expose("rt_fscanf", &rt_fscanf);
    expose("rt_sscanf", &rt_sscanf);
    expose("rt_fread", &rt_fread);
    expose("rt_timeformat_init", &rt_timeformat_init);
    expose("rt_timeformat_set", &rt_timeformat_set);
    expose("rt_finish", &rt_finish);
    expose("rt_observed_case_violation", &rt_observed_case_violation);
    expose("rt_observed_flush", &rt_observed_flush);
    expose("rt_defer_report", &rt_defer_report);
    expose("rt_defer_flush_observed", &rt_defer_flush_observed);
    expose("rt_defer_flush_final", &rt_defer_flush_final);
    expose("rt_oscillation_abort", &rt_oscillation_abort);
    expose("rt_queue_assign_dynarr", &rt_queue_assign_dynarr);
    expose("rt_severity", &rt_severity);
    expose("rt_sim_run", &rt_sim_run);
    expose("rt_fflush", &rt_fflush);
    expose("rt_readmem", &rt_readmem);
    expose("rt_random", &rt_random);
    expose("rt_dist_uniform", &rt_dist_uniform);
    expose("rt_dist_normal", &rt_dist_normal);
    expose("rt_dist_exponential", &rt_dist_exponential);
    expose("rt_dist_poisson", &rt_dist_poisson);
    expose("rt_dist_chi_square", &rt_dist_chi_square);
    expose("rt_dist_t", &rt_dist_t);
    expose("rt_dist_erlang", &rt_dist_erlang);
    expose("rt_urandom_seed", &rt_urandom_seed);
    expose("rt_test_plusargs", &rt_test_plusargs);
    expose("rt_value_plusargs", &rt_value_plusargs);
    expose("rt_vcd_open", &rt_vcd_open);
    expose("rt_vcd_begin", &rt_vcd_begin);
    expose("rt_vcd_scope", &rt_vcd_scope);
    expose("rt_vcd_upscope", &rt_vcd_upscope);
    expose("rt_vcd_var", &rt_vcd_var);
    expose("rt_vcd_array", &rt_vcd_array);
    expose("rt_vcd_start", &rt_vcd_start);
    expose("rt_vcd_touch", &rt_vcd_touch);
    expose("rt_vcd_touch_range", &rt_vcd_touch_range);
    // rt_vcd_active is a data symbol (the VCD-active flag the JIT loads to gate
    // touch calls), so it is exposed without the Callable flag.
    rt_syms[(*jit)->mangleAndIntern("rt_vcd_active")] = {
        llvm::orc::ExecutorAddr::fromPtr(&rt_vcd_active),
        llvm::JITSymbolFlags::Exported,
    };
    expose("rt_vcd_flush", &rt_vcd_flush);
    expose("rt_vcd_off", &rt_vcd_off);
    expose("rt_vcd_on", &rt_vcd_on);
    expose("rt_vcd_all", &rt_vcd_all);
    expose("rt_vcd_dumpflush", &rt_vcd_dumpflush);
    expose("rt_vcd_close", &rt_vcd_close);
    expose("rt_alloc", &rt_alloc);
    expose("rt_free", &rt_free);
    expose("rt_nba_arr_slot", &rt_nba_arr_slot);
    expose("rt_nba_arr_base", &rt_nba_arr_base);
    expose("rt_nba_arr_count", &rt_nba_arr_count);
    expose("rt_nba_arr_any", &rt_nba_arr_any);
    expose("rt_nba_arr_reset", &rt_nba_arr_reset);
    expose("rt_dynarr_new", &rt_dynarr_new);
    expose("rt_dynarr_new_copy", &rt_dynarr_new_copy);
    expose("rt_dynarr_delete", &rt_dynarr_delete);
    expose("rt_dynarr_assign", &rt_dynarr_assign);
    expose("rt_dynarr_reverse", &rt_dynarr_reverse);
    expose("rt_dynarr_sort", &rt_dynarr_sort);
    expose("rt_dynarr_shuffle", &rt_dynarr_shuffle);
    expose("rt_queue_push_back", &rt_queue_push_back);
    expose("rt_queue_push_front", &rt_queue_push_front);
    expose("rt_queue_pop_back", &rt_queue_pop_back);
    expose("rt_queue_pop_front", &rt_queue_pop_front);
    expose("rt_queue_insert", &rt_queue_insert);
    expose("rt_queue_delete_idx", &rt_queue_delete_idx);
    expose("rt_queue_delete_all", &rt_queue_delete_all);
    expose("rt_queue_assign", &rt_queue_assign);
    expose("rt_queue_append_range", &rt_queue_append_range);
    expose("rt_queue_move", &rt_queue_move);
    expose("rt_container_format_p", &rt_container_format_p);
    expose("rt_container_eq", &rt_container_eq);
    expose("rt_queue_reverse", &rt_queue_reverse);
    expose("rt_queue_sort", &rt_queue_sort);
    expose("rt_queue_shuffle", &rt_queue_shuffle);
    expose("rt_assoc_get_ptr", &rt_assoc_get_ptr);
    expose("rt_assoc_put_ptr", &rt_assoc_put_ptr);
    expose("rt_assoc_exists", &rt_assoc_exists);
    expose("rt_assoc_delete_key", &rt_assoc_delete_key);
    expose("rt_assoc_delete_all", &rt_assoc_delete_all);
    expose("rt_assoc_num", &rt_assoc_num);
    expose("rt_assoc_first", &rt_assoc_first);
    expose("rt_assoc_last", &rt_assoc_last);
    expose("rt_assoc_next", &rt_assoc_next);
    expose("rt_assoc_prev", &rt_assoc_prev);
    expose("rt_assoc_assign", &rt_assoc_assign);
    expose("rt_intra_alloc", &rt_intra_alloc);
    expose("rt_intra_cb", &rt_intra_cb);
    expose("rt_intra_ready_base", &rt_intra_ready_base);
    expose("rt_intra_ready_count", &rt_intra_ready_count);
    expose("rt_intra_any", &rt_intra_any);
    expose("rt_intra_release", &rt_intra_release);
    expose("rt_intra_reset", &rt_intra_reset);
    expose("rt_intra_event_register", &rt_intra_event_register);
    expose("rt_intra_event_fire", &rt_intra_event_fire);
    expose("rt_inertial_decide", &rt_inertial_decide);
    expose("rt_inertial_cb", &rt_inertial_cb);
    expose("rt_dpi_pack_logicvec", &rt_dpi_pack_logicvec);
    expose("rt_dpi_unpack_logicvec", &rt_dpi_unpack_logicvec);
    expose("rt_dpi_str_out", &rt_dpi_str_out);
    // svdpi.h helper API (LRM Annex H): the packed-vector accessors DPI C calls.
    // A DPI object (--dpi-lib *.o) resolves these against the JITDylib.
    expose("svDpiVersion", &svDpiVersion);
    expose("svGetBitselBit", &svGetBitselBit);
    expose("svGetBitselLogic", &svGetBitselLogic);
    expose("svPutBitselBit", &svPutBitselBit);
    expose("svPutBitselLogic", &svPutBitselLogic);
    expose("svGetPartselBit", &svGetPartselBit);
    expose("svGetPartselLogic", &svGetPartselLogic);
    expose("svPutPartselBit", &svPutPartselBit);
    expose("svPutPartselLogic", &svPutPartselLogic);
    expose("svGetScope", &svGetScope);
    expose("svSetScope", &svSetScope);
    expose("svGetNameFromScope", &svGetNameFromScope);
    expose("svGetScopeFromName", &svGetScopeFromName);
    expose("svPutUserData", &svPutUserData);
    expose("svGetUserData", &svGetUserData);
    expose("rt_svdpi_scope_init", &rt_svdpi_scope_init);
    expose("svDimensions", &svDimensions);
    expose("svSize", &svSize);
    expose("svLeft", &svLeft);
    expose("svRight", &svRight);
    expose("svLow", &svLow);
    expose("svHigh", &svHigh);
    expose("svIncrement", &svIncrement);
    expose("svSizeOfArray", &svSizeOfArray);
    expose("svGetArrayPtr", &svGetArrayPtr);
    expose("svGetArrElemPtr1", &svGetArrElemPtr1);
    expose("svGetBitArrElem1VecVal", &svGetBitArrElem1VecVal);
    expose("svPutBitArrElem1VecVal", &svPutBitArrElem1VecVal);
    expose("svGetLogicArrElem1VecVal", &svGetLogicArrElem1VecVal);
    expose("svPutLogicArrElem1VecVal", &svPutLogicArrElem1VecVal);
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
    expose("rt_enum_name", &rt_enum_name);
    expose("rt_string_toupper", &rt_string_toupper);
    expose("rt_string_tolower", &rt_string_tolower);
    expose("rt_string_putc", &rt_string_putc);
    expose("rt_string_getc", &rt_string_getc);
    expose("rt_string_from_packed", &rt_string_from_packed);
    expose("rt_string_to_packed", &rt_string_to_packed);
    expose("rt_string_atoi", &rt_string_atoi);
    expose("rt_string_atohex", &rt_string_atohex);
    expose("rt_string_atooct", &rt_string_atooct);
    expose("rt_string_atobin", &rt_string_atobin);
    expose("rt_string_atoreal", &rt_string_atoreal);
    expose("rt_string_itoa", &rt_string_itoa);
    expose("rt_string_hextoa", &rt_string_hextoa);
    expose("rt_string_octtoa", &rt_string_octtoa);
    expose("rt_string_bintoa", &rt_string_bintoa);
    expose("rt_string_realtoa", &rt_string_realtoa);
    expose("rt_wheel_init", &rt_wheel_init);
    expose("rt_wheel_deinit", &rt_wheel_deinit);
    expose("rt_wheel_now", &rt_wheel_now);
    expose("rt_wheel_pending", &rt_wheel_pending);
    expose("rt_wheel_schedule", &rt_wheel_schedule);
    expose("rt_wheel_schedule_rel", &rt_wheel_schedule_rel);
    expose("rt_wheel_tick", &rt_wheel_tick);
    expose("rt_wheel_advance", &rt_wheel_advance);
    // libc mem* helpers that LLVM's loop-idiom pass synthesizes under -O (a
    // zeroing loop becomes memset, a copy loop memcpy, etc.). A dynamically
    // linked clotho resolves these through the process symbol table below, but
    // glibc's mem* are IFUNCs that dlsym cannot resolve in a fully static
    // binary, so bind them to the host implementations directly.
    expose("memset", &std::memset);
    expose("memcpy", &std::memcpy);
    expose("memmove", &std::memmove);
    expose("memcmp", &std::memcmp);
#if defined(__MINGW32__)
    // Two symbols a MinGW target emits that live in the static CRT and libgcc,
    // where the process-symbol generator below cannot see them:
    //
    //   __main        — the static-constructor hook LLVM plants at the top of
    //                   any function literally named `main`, which is what the
    //                   design's entry point is called. A JIT'd module has no
    //                   C++ constructors to run, and calling the host's would
    //                   re-enter its global init, so bind a no-op.
    //   ___chkstk_ms  — the stack-probe helper for frames larger than a page.
    //                   Process bodies with wide state hit this readily.
    expose("__main", +[] {});
    expose("___chkstk_ms", &___chkstk_ms);
#endif
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

    // Load each --dpi-lib for DPI-C (LRM 35), dispatched by file format — the two
    // ORC loaders differ in linking model, not in import-vs-export:
    //   * .o / .a (relocatable object): LLJIT::addObjectFile links it INTO the
    //     JITDylib, so its undefined symbols resolve against the design — the C's
    //     `import` calls bind to the design AND its `export` calls bind to the
    //     JIT-compiled export thunks. Two-way; required for export.
    //   * .so (sealed shared object): dlopen'd via DynamicLibrarySearchGenerator;
    //     ORC can only read symbols out of it (import), never inject a JIT'd thunk
    //     into it. One-way, import only.
    for (const std::string& lib : config.dpi_libs) {
        const bool is_object = lib.ends_with(".o") || lib.ends_with(".a");
        if (is_object) {
            auto buf = llvm::MemoryBuffer::getFile(lib);
            if (!buf) {
                llvm::errs() << "jit: cannot read DPI object '" << lib
                             << "': " << buf.getError().message() << "\n";
                return -1;
            }
            if (auto err = (*jit)->addObjectFile(jd, std::move(*buf))) {
                llvm::errs() << "jit: cannot link DPI object '" << lib
                             << "': " << llvm::toString(std::move(err)) << "\n";
                return -1;
            }
        } else {
            auto gen = llvm::orc::DynamicLibrarySearchGenerator::Load(
              lib.c_str(), (*jit)->getDataLayout().getGlobalPrefix());
            if (!gen) {
                llvm::errs() << "jit: cannot load DPI library '" << lib
                             << "': " << llvm::toString(gen.takeError()) << "\n";
                return -1;
            }
            jd.addGenerator(std::move(*gen));
        }
    }

    if (parallel) {
        // Serialize every module to bitcode while they still share `ctx` — a
        // read-only traversal, no writes into the context's uniquing tables,
        // so this stays single-threaded rather than risk concurrent reads of
        // one LLVMContext. Then drop the shared modules/context: each buffer
        // below is independently reparsed into its own fresh LLVMContext, so
        // optimize_module + verify + addIRModule for different units run
        // concurrently instead of serializing on one shared context's lock.
        const auto t_serialize = std::chrono::steady_clock::now();
        std::vector<llvm::SmallVector<char, 0>> bitcode(built.modules.size());
        for (size_t i = 0; i < built.modules.size(); ++i) {
            llvm::raw_svector_ostream os(bitcode[i]);
            llvm::WriteBitcodeToFile(*built.modules[i], os);
        }
        built.modules.clear();
        ctx.reset();
        if (stat) {
            fprintf(stderr, "PARALLEL_STAT serialize: %.1f ms\n", ms_since(t_serialize));
        }

        const auto          t_parallel = std::chrono::steady_clock::now();
        std::vector<double> stat_ms(stat ? bitcode.size() : 0);

        llvm::DefaultThreadPool pool(llvm::hardware_concurrency(config.compile_jobs));
        std::mutex              err_mu;
        std::string             first_err;
        std::vector<std::shared_future<void>> tasks;
        tasks.reserve(bitcode.size());
        for (size_t i = 0; i < bitcode.size(); ++i) {
            tasks.push_back(pool.async([&, i] {
                const auto              t0      = std::chrono::steady_clock::now();
                auto                   mod_ctx = std::make_unique<llvm::LLVMContext>();
                llvm::MemoryBufferRef  buf(llvm::StringRef(bitcode[i].data(), bitcode[i].size()),
                                          "clotho.unit");
                auto                   parsed = llvm::parseBitcodeFile(buf, *mod_ctx);
                const auto             fail   = [&](std::string msg) {
                    std::lock_guard<std::mutex> lock(err_mu);
                    if (first_err.empty()) {
                        first_err = std::move(msg);
                    }
                };
                if (!parsed) {
                    fail("jit: parallel module reload failed: " + llvm::toString(parsed.takeError()));
                    return;
                }
                if (!optimize_module(**parsed, config.o3)) {
                    fail("jit: parallel module optimize failed");
                    return;
                }
                if (llvm::verifyModule(**parsed, &llvm::errs())) {
                    fail("jit: parallel module failed verification after optimize");
                    return;
                }
                llvm::orc::ThreadSafeContext mod_tsc(std::move(mod_ctx));
                if (auto err = (*jit)->addIRModule(
                      llvm::orc::ThreadSafeModule(std::move(*parsed), std::move(mod_tsc)))) {
                    fail("jit: " + llvm::toString(std::move(err)));
                }
                if (stat) {
                    stat_ms[i] = ms_since(t0);
                }
            }));
        }
        for (auto& t : tasks) {
            t.wait();
        }
        if (stat) {
            fprintf(stderr, "PARALLEL_STAT parallel section wall: %.1f ms\n", ms_since(t_parallel));
        }
        if (stat) {
            double sum = 0.0, max_ms = 0.0;
            size_t max_i = 0;
            for (size_t i = 0; i < stat_ms.size(); ++i) {
                sum += stat_ms[i];
                if (stat_ms[i] > max_ms) {
                    max_ms = stat_ms[i];
                    max_i  = i;
                }
            }
            // max_ms is the longest single module's optimize+verify+addIRModule
            // time — a hard wall-time floor no thread count can beat. sum/max_ms
            // is the speedup ceiling (vs a hypothetical fully-serial run of just
            // this phase); compare against the measured speedup to see how much
            // headroom load imbalance (a few big modules dominating) is costing.
            fprintf(stderr,
                    "PARALLEL_STAT modules=%zu jobs=%u sum_ms=%.1f max_ms=%.1f (module %zu) "
                    "ceiling=%.2fx\n",
                    stat_ms.size(), n_jobs, sum, max_ms, max_i, sum / std::max(max_ms, 1e-6));
            for (size_t i = 0; i < stat_ms.size(); ++i) {
                fprintf(stderr, "  module %zu: %.1f ms (bitcode %zu B)\n", i, stat_ms[i],
                        bitcode[i].size());
            }
        }
        if (!first_err.empty()) {
            llvm::errs() << first_err << "\n";
            return -1;
        }
    } else {
        llvm::orc::ThreadSafeContext tsc(std::move(ctx));
        for (auto& mod : built.modules) {
            if (!optimize_module(*mod, config.o3)) {
                llvm::errs() << "jit: module optimize failed\n";
                return -1;
            }
            if (llvm::verifyModule(*mod, &llvm::errs())) {
                llvm::errs() << "jit: module failed verification after optimize\n";
                return -1;
            }
            if (auto err = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), tsc))) {
                llvm::errs() << "jit: " << llvm::toString(std::move(err)) << "\n";
                return -1;
            }
        }
    }

    // Looking up `main` materializes the root module through the compile
    // layer; its references to the unit modules' group functions materialize
    // those in turn, i.e. eagerly compiles the whole design.
    const auto t_lookup = std::chrono::steady_clock::now();
    auto       sym      = (*jit)->lookup("main");
    if (!sym) {
        llvm::errs() << "jit: " << llvm::toString(sym.takeError()) << "\n";
        return -1;
    }
    if (stat) {
        fprintf(stderr, "PARALLEL_STAT lookup(main) [ORC codegen]: %.1f ms\n", ms_since(t_lookup));
    }

    auto* fn = sym->toPtr<int (*)()>();
    if (timings != nullptr) {
        timings->llvm_ms += ms_since(t_llvm);
    }

    // Populate the plusarg list (LRM §21.6) before the design runs. A host-side
    // call, not exposed to the JIT — $test$plusargs / $value$plusargs only read
    // it via the rt_* calls above.
    std::vector<const char*> pa_ptrs;
    std::vector<uint32_t>    pa_lens;
    pa_ptrs.reserve(plusargs.size());
    pa_lens.reserve(plusargs.size());
    for (const std::string_view pa : plusargs) {
        pa_ptrs.push_back(pa.data());
        pa_lens.push_back(static_cast<uint32_t>(pa.size()));
    }
    rt_plusargs_init(pa_ptrs.data(), pa_lens.data(), static_cast<uint32_t>(pa_ptrs.size()));

    // Arm the $system access tier for the runtime backstop (rt_system). The
    // compile-time gate in lowering is primary; this mirrors the operator's
    // opt-in into the runtime so a bypassed gate still refuses to exec.
    rt_system_set_access(static_cast<int32_t>(config.allow_system));

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
    // CLOTHO_DOM_DEBUG: dump the domain-specialization dispatch counts the
    // backend exported (see compute_domain_spec) — per-group fast vs generic
    // reader-side dispatches. Pairs with the backend's compile-time stderr
    // dump of the read sets.
    if (getenv("CLOTHO_DOM_DEBUG") != nullptr) {
        const auto addr = [&](const char* name) -> void* {
            auto s = (*jit)->lookup(name);
            return s ? s->toPtr<void*>() : nullptr;
        };
        const auto* ng    = static_cast<const uint32_t*>(addr("clotho.dom_ngroups"));
        const auto* nfast = static_cast<const uint64_t*>(addr("clotho.dom_nfast"));
        const auto* ngen  = static_cast<const uint64_t*>(addr("clotho.dom_ngen"));
        if (ng && nfast && ngen) {
            for (uint32_t g = 0; g < *ng; ++g) {
                if (nfast[g] + ngen[g] > 0) {
                    fprintf(stderr, "DOM run g=%u fast=%llu gen=%llu\n", g,
                            static_cast<unsigned long long>(nfast[g]),
                            static_cast<unsigned long long>(ngen[g]));
                }
            }
        }
    }
    return rc;
}

} // namespace clotho::ir
