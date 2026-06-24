#pragma once

// LLVM backend: lowers each process of an ir::Module to a single LLVM function
// and emits textual LLVM IR. This is the first consumer of clotho_ir that turns
// the IR into something compilable; the library is no longer "just IR".
//
// Model (see LlvmBackend.cpp for the full rationale):
//   * One shared state struct holds two integer fields per module signal — the
//     val plane and the xz plane of its 4-state value.
//   * Each process becomes `void <name>(ptr state)`; Load/Store address fields
//     of that struct.
//   * Every SSA value is lowered as a (val, xz) plane pair so X/Z propagate.

#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Module.h"

namespace clotho::ir {

// Lower every process in `m` to a fresh llvm::Module owned by `ctx`. Returns
// nullptr (and writes the reason to llvm::errs()) on backend error or if the
// generated module fails verification. The caller owns the returned module and
// must keep `ctx` alive at least as long as it.
std::unique_ptr<llvm::Module> build_module(const Module& m, llvm::LLVMContext& ctx);

// Lower every process in `m` to an LLVM function and write textual LLVM IR to
// `os`. Returns false (and writes the reason to llvm::errs()) if the generated
// module fails verification; on success the IR has already been printed to `os`.
bool emit_llvm(const Module& m, llvm::raw_ostream& os);

} // namespace clotho::ir
