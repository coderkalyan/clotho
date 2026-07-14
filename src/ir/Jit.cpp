#include "Jit.h"

#include <csetjmp>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

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
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "LlvmBackend.h"
#include "runtime/Runtime.h"
#include "runtime/svdpi.h"

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
    // moved into ThreadSafeModules sharing one ThreadSafeContext the JIT takes
    // over.
    auto ctx = std::make_unique<llvm::LLVMContext>();
    // The JIT needs runnable code: coroutine lowering is mandatory regardless
    // of what the caller's config says (optimize=false is an emit_llvm
    // inspection mode).
    SimulationConfig jit_config = config;
    jit_config.optimize         = true;
    auto mods                   = build_modules(m, *ctx, jit_config);
    if (mods.empty()) {
        return -1; // build_modules already reported the reason.
    }
    llvm::orc::ThreadSafeContext tsc(std::move(ctx));

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
    expose("rt_case_violation", &rt_case_violation);
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
    expose("rt_queue_reverse", &rt_queue_reverse);
    expose("rt_queue_sort", &rt_queue_sort);
    expose("rt_queue_shuffle", &rt_queue_shuffle);
    expose("rt_intra_alloc", &rt_intra_alloc);
    expose("rt_intra_cb", &rt_intra_cb);
    expose("rt_intra_ready_base", &rt_intra_ready_base);
    expose("rt_intra_ready_count", &rt_intra_ready_count);
    expose("rt_intra_any", &rt_intra_any);
    expose("rt_intra_release", &rt_intra_release);
    expose("rt_intra_reset", &rt_intra_reset);
    expose("rt_intra_event_register", &rt_intra_event_register);
    expose("rt_intra_event_fire", &rt_intra_event_fire);
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

    for (auto& mod : mods) {
        if (auto err = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), tsc))) {
            llvm::errs() << "jit: " << llvm::toString(std::move(err)) << "\n";
            return -1;
        }
    }

    // Looking up `main` materializes the root module through the compile
    // layer; its references to the unit modules' group functions materialize
    // those in turn, i.e. eagerly compiles the whole design.
    auto sym = (*jit)->lookup("main");
    if (!sym) {
        llvm::errs() << "jit: " << llvm::toString(sym.takeError()) << "\n";
        return -1;
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
