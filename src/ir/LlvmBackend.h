#pragma once

// LLVM backend: lowers each process of an ir::Module to a single LLVM function
// and emits textual LLVM IR. This is the first consumer of clotho_ir that turns
// the IR into something compilable; the library is no longer "just IR".
//
// Model (see LlvmBackend.cpp for the full rationale):
//   * One shared state struct holds a slot per module signal: a packed must|may
//     word plus a z word — the z word elided when the may-z analysis
//     (ZElision.h) proves the signal never carries z.
//   * Each process becomes a switched-resume coroutine `ptr <name>(ptr state)`;
//     Load/Store address fields of that struct.
//   * Every SSA value is lowered as a (must|may, z) plane pair so X/Z propagate.

#include <memory>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Module.h"
#include "SimConfig.h"

namespace clotho::ir {

// The lowered design: modules[0] is the root module (state globals, scheduler
// machinery, functions, main), followed by one module per elaborated-
// definition unit holding its single-scope process groups (config.split; a
// single root module otherwise).
struct BuiltDesign {
    std::vector<std::unique_ptr<llvm::Module>> modules;
};

// Lower every process in `m` to llvm::Modules owned by `ctx`. Returns empty
// modules (and writes the reason to llvm::errs()) on backend error or if any
// generated module fails verification. The caller owns the returned modules
// and must keep `ctx` alive at least as long as them; the JIT links the whole
// set into one dylib.
BuiltDesign build_modules(const Module& m, llvm::LLVMContext& ctx,
                          const SimulationConfig& config = {});

// Same construction + first-verification pass as build_modules, but returns
// before running optimize_module (coro intrinsics intact regardless of
// `config.optimize`) and before the second verify. run_jit uses this to
// migrate each module into its own LLVMContext (a bitcode round-trip, see
// Jit.cpp) before optimizing, so unrelated modules can be optimized/codegen'd
// concurrently — LLVMContext is not thread-safe, so this can't happen while
// modules still share `ctx`. build_modules itself is unchanged (single shared
// context, sequential optimize_module loop) for emit_llvm / tests.
BuiltDesign build_modules_unoptimized(const Module& m, llvm::LLVMContext& ctx,
                                      const SimulationConfig& config = {});

// Run Clotho's coroutine-lowering pipeline (Tier 0 or, under `o3`, LLVM's
// default<O3>) on one module in place. Returns false (reason on errs()) only
// if the pipeline string is rejected; see build_modules for the call sequence
// (optimize_module then re-verify).
bool optimize_module(llvm::Module& m, bool o3);

// Lower every process in `m` to an LLVM function and write textual LLVM IR to
// `os`. Returns false (and writes the reason to llvm::errs()) if the generated
// module fails verification; on success the IR has already been printed to `os`.
// `config` is forwarded to build_module.
bool emit_llvm(const Module& m, llvm::raw_ostream& os, const SimulationConfig& config = {});

} // namespace clotho::ir
