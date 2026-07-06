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

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Module.h"
#include "SimConfig.h"

namespace clotho::ir {

// Lower every process in `m` to a fresh llvm::Module owned by `ctx`. Returns
// nullptr (and writes the reason to llvm::errs()) on backend error or if the
// generated module fails verification. The caller owns the returned module and
// must keep `ctx` alive at least as long as it.
//
std::unique_ptr<llvm::Module> build_module(const Module& m, llvm::LLVMContext& ctx,
                                           const SimulationConfig& config = {});

// Lower every process in `m` to an LLVM function and write textual LLVM IR to
// `os`. Returns false (and writes the reason to llvm::errs()) if the generated
// module fails verification; on success the IR has already been printed to `os`.
// `config` is forwarded to build_module.
bool emit_llvm(const Module& m, llvm::raw_ostream& os, const SimulationConfig& config = {});

} // namespace clotho::ir
