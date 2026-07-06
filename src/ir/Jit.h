#pragma once

// JIT simulator: the primary driver behavior. Rather than emitting LLVM IR for
// an external toolchain, build the design's module in-process, hand it to an
// ORC LLJIT, eagerly compile it for the host, and run its `main()`. The IR
// emission paths (emit_llvm / print_module) remain for debugging.

#include "Module.h"
#include "SimConfig.h"
#include "Timing.h"

namespace clotho::ir {

// Build, JIT-compile and execute `main()` for `m` on the host. Returns the i32
// value `main()` returns (intended as the process exit code). On any setup or
// compilation failure, writes the reason to llvm::errs() and returns a negative
// sentinel so the driver can exit non-zero. If `timings` is non-null, the LLVM
// compile and simulation phase durations are recorded into it. `config` is
// forwarded to build_module.
int run_jit(const Module& m, Timings* timings = nullptr, const SimulationConfig& config = {});

} // namespace clotho::ir
