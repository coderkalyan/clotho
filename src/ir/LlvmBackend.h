#pragma once

// LLVM backend: lowers each process of an ir::Module to a single LLVM function
// and emits textual LLVM IR. This is the first consumer of clotho_ir that turns
// the IR into something compilable; the library is no longer "just IR".
//
// Model (see ir/llvm/Backend.h for the full rationale):
//   * One shared state struct holds a slot per module signal: a 2-state signal is
//     one native word, a 4-state one a packed must|may word plus a z word -- the z
//     word elided when the may-z analysis (Analysis.h) proves the signal never
//     carries z.
//   * Each process becomes a switched-resume coroutine `ptr <name>(ptr state)`;
//     Load/Store address fields of that struct.
//   * A 4-state SSA value is lowered as a (must|may, z) plane pair so X/Z
//     propagate; a 2-state value is one native integer.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include "Module.h"
#include "SimConfig.h"
#include "Timing.h"

namespace clotho::ir {

// The lowered design: modules[0] is the root module (state globals, scheduler
// machinery, functions, the `clotho_main` entry rt_run calls), followed by one
// module per elaborated-definition unit holding its single-scope process groups
// (config.split; a single root module otherwise).
struct BuiltDesign {
    std::vector<std::unique_ptr<llvm::Module>> modules;
};

// The machine Clotho runs on as a compilation target: its triple, CPU and CPU
// features. Initializes LLVM's native target and its code generator on first use.
llvm::Expected<llvm::orc::JITTargetMachineBuilder> host_target();

// Lower every process in `m` to llvm::Modules owned by `ctx`. Returns empty
// modules on backend error or if any generated module fails verification. The
// caller owns the returned modules and must keep `ctx` alive at least as long
// as them; the JIT links the whole set into one dylib.
//
// `target` fixes the modules' triple and DataLayout. Every struct layout, baked
// byte offset and access alignment the backend computes derives from that
// DataLayout, so the modules must be compiled to code for the same target.
//
// `resolve` (in practice `SourceContext::render`) decodes source locations for
// two consumers: the reports a sanitizer bakes into the emitted code, and a
// backend refusal, which is written to llvm::errs() in the same
// `file:line:col: error: <message> [<code>]` form as a frontend diagnostic.
// Without it both read "<unknown>", which is what a caller with no
// SourceContext gets.
BuiltDesign build_modules(const Module& m, llvm::LLVMContext& ctx, const llvm::TargetMachine& target,
                          const SimulationConfig& config = {}, const LocResolver& resolve = {});

// Same construction + first-verification pass as build_modules, but returns
// before running optimize_module (coro intrinsics intact regardless of
// `config.lower_coro`) and before the second verify. run_jit and the object
// emitters use this and hand the result to for_each_optimized_module, which
// migrates each module into its own LLVMContext (a bitcode round-trip) before
// optimizing, so unrelated modules can be optimized and code-generated
// concurrently -- LLVMContext is not thread-safe, so this can't happen while
// modules still share `ctx`. build_modules itself keeps the single shared
// context and a sequential optimize_module loop, for emit_llvm and tests.
BuiltDesign build_modules_unoptimized(const Module& m, llvm::LLVMContext& ctx,
                                      const llvm::TargetMachine& target,
                                      const SimulationConfig&    config  = {},
                                      const LocResolver&         resolve = {});

// Link the runtime bitcode a module calls into it (ir/llvm/RuntimeBitcode.h), then run
// Clotho's coroutine-lowering pipeline (Tier 0 or, under `o3`, LLVM's
// default<O3>) on one module in place. Returns false (reason on errs()) only
// if the pipeline string is rejected; see build_modules for the call sequence
// (optimize_module then re-verify).
bool optimize_module(llvm::Module& m, bool o3);

// A module and the LLVMContext it lives in. The context is declared first, so it
// is destroyed after the module.
struct OwnedModule {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module>      module;
};

// What becomes of one module `for_each_optimized_module` has finished with: `index`
// is its place in `BuiltDesign::modules` (root is 0). Called on a worker thread,
// concurrently with the calls for other modules.
using ModuleConsumer = llvm::function_ref<llvm::Error(size_t index, OwnedModule&& optimized)>;

// The per-module half of compilation, shared by `run` (the consumer hands the module
// to the JIT) and `build` (it generates an object), so the two cannot drift. The
// modules of `built`, an unoptimized build_modules_unoptimized result, are written
// to bitcode on the calling thread (`build_ctx`, the context that holds them, is
// not thread-safe) and both are released: `built` is left empty and `build_ctx`
// is destroyed. Then, on `config.compile_jobs` worker threads (0 means the
// machine's), each module is read back into a context of its own, run through
// optimize_module (`config.o3`), verified, and passed to `consume`. The first
// failure is returned, with its reason, and modules not yet started are
// skipped. `config.stats` `parallel` reports the phase's timings on stderr.
llvm::Error for_each_optimized_module(BuiltDesign&                       built,
                                      std::unique_ptr<llvm::LLVMContext> build_ctx,
                                      const SimulationConfig&            config,
                                      ModuleConsumer                     consume);

// Lower every process in `m` to an LLVM function and write textual LLVM IR for
// the host target to `os`. Returns false (and writes the reason to
// llvm::errs()) if the generated module fails verification; on success the IR
// has already been printed to `os`. `config` is forwarded to build_modules. If
// `timings` is non-null, the IR generation + pipeline duration is accumulated
// into its `llvm_ms`.
bool emit_llvm(const Module& m, llvm::raw_ostream& os, const SimulationConfig& config = {},
               Timings* timings = nullptr, const LocResolver& resolve = {});

// One built module as text, named by its module identifier. The first entry is
// the root module.
struct ModuleIr {
    std::string name;
    std::string ir;
};

// Build the design as emit_llvm does and append each module's IR to `out`
// separately (root first), without linking them: what one unit's code is is
// what its own text says. Returns false (reason on llvm::errs()) on a backend
// error. Exposed for `--emit llvm-units` and the modular-compilation gate test.
bool emit_llvm_modules(const Module& m, std::vector<ModuleIr>& out, const SimulationConfig& config = {},
                       const LocResolver& resolve = {});

// Lower the design as build_modules does, with `verify_shared_code` on and no
// optimization, verification or code generation, and report whether the code its
// instances share is what each lowers for itself. `Mismatch` carries the report
// (the unit, group, instances and first differing line, as `file:line:col:
// error:` text); `Refused` is any other refusal of the backend, which says
// nothing about sharing. Used by twin mode (Lower.h).
struct SharedCodeCheck {
    enum class Outcome : uint8_t { Ok, Refused, Mismatch };
    Outcome     outcome = Outcome::Ok;
    std::string message;
};
SharedCodeCheck check_shared_code(const Module& m, const LocResolver& resolve = {});

// Compile the design for `target`'s triple, CPU and features and write it to
// `os` as one relocatable object file, holding the code of every module. The
// object defines `clotho_main`, the entry rt_run takes, and `clotho_design`, the
// rt_design an executable's main reads; every runtime symbol it references is
// left undefined, for a link against the runtime. The modules are optimized in
// parallel (for_each_optimized_module), then linked back into the root module,
// so the object holds exactly the code `run` would compile, and generated as one
// on the calling thread. Code generation uses `config.o3` as run_jit does, the
// platform's default code model and position-independent code, so the object
// links into a PIE. Returns false with the reason on llvm::errs().
bool emit_object(const Module&                             m,
                 llvm::raw_ostream&                        os,
                 const llvm::orc::JITTargetMachineBuilder& target,
                 const SimulationConfig&                   config  = {},
                 Timings*                                  timings = nullptr,
                 const LocResolver&                        resolve = {});

// Compile the design as emit_object does, but write each module as an object file
// of its own into the existing directory `dir`, generated in parallel on
// `config.compile_jobs` threads (for_each_optimized_module, so the same pool and
// pipeline `run` uses). `objects` is set to the files' paths, root first: the
// order a link takes them in. Between them they define `clotho_main` and
// `clotho_design`; a symbol one module takes from another is undefined in the
// one and defined in the other, to be resolved by a link of them all. Returns false
// with the reason on llvm::errs().
bool emit_objects(const Module&                             m,
                  const std::string&                        dir,
                  std::vector<std::string>&                 objects,
                  const llvm::orc::JITTargetMachineBuilder& target,
                  const SimulationConfig&                   config  = {},
                  Timings*                                  timings = nullptr,
                  const LocResolver&                        resolve = {});

} // namespace clotho::ir
