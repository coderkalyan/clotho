#pragma once

// LLVM backend: lowers each process of an ir::Module to a single LLVM function
// and emits textual LLVM IR. This is the first consumer of clotho_ir that turns
// the IR into something compilable; the library is no longer "just IR".
//
// Model (see LlvmBackend.cpp for the full rationale):
//   * One shared state struct holds a slot per module signal: a packed must|may
//     word plus a z word — the z word elided when the may-z analysis
//     (Analysis.h) proves the signal never carries z.
//   * Each process becomes a switched-resume coroutine `ptr <name>(ptr state)`;
//     Load/Store address fields of that struct.
//   * Every SSA value is lowered as a (must|may, z) plane pair so X/Z propagate.

#include <memory>
#include <vector>

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
// `config.lower_coro`) and before the second verify. run_jit uses this to
// migrate each module into its own LLVMContext (a bitcode round-trip, see
// Jit.cpp) before optimizing, so unrelated modules can be optimized/codegen'd
// concurrently — LLVMContext is not thread-safe, so this can't happen while
// modules still share `ctx`. build_modules itself is unchanged (single shared
// context, sequential optimize_module loop) for emit_llvm / tests.
BuiltDesign build_modules_unoptimized(const Module& m, llvm::LLVMContext& ctx,
                                      const llvm::TargetMachine& target,
                                      const SimulationConfig&    config  = {},
                                      const LocResolver&         resolve = {});

// Run Clotho's coroutine-lowering pipeline (Tier 0 or, under `o3`, LLVM's
// default<O3>) on one module in place. Returns false (reason on errs()) only
// if the pipeline string is rejected; see build_modules for the call sequence
// (optimize_module then re-verify).
bool optimize_module(llvm::Module& m, bool o3);

// Lower every process in `m` to an LLVM function and write textual LLVM IR for
// the host target to `os`. Returns false (and writes the reason to
// llvm::errs()) if the generated module fails verification; on success the IR
// has already been printed to `os`. `config` is forwarded to build_modules. If
// `timings` is non-null, the IR generation + pipeline duration is accumulated
// into its `llvm_ms`.
bool emit_llvm(const Module& m, llvm::raw_ostream& os, const SimulationConfig& config = {},
               Timings* timings = nullptr, const LocResolver& resolve = {});

// Compile the design for `target` and write it to `os` as one relocatable
// object file. The object defines `clotho_main`, the entry rt_run takes, and
// every runtime symbol it references is left undefined, for a link against
// clotho_runtime. Per-definition units are linked back into the root module
// first, so the object holds exactly the code `run` would compile. Code
// generation uses `config.o3` as run_jit does, and position-independent code so
// the object links into a PIE. Returns false with the reason on llvm::errs().
bool emit_object(const Module& m, llvm::raw_ostream& os, llvm::orc::JITTargetMachineBuilder target,
                 const SimulationConfig& config = {}, Timings* timings = nullptr,
                 const LocResolver& resolve = {});

} // namespace clotho::ir
