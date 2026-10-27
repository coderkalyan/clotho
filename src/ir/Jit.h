#pragma once

// JIT simulator: the primary driver behavior. Rather than emitting LLVM IR for
// an external toolchain, build the design's module in-process, hand it to an
// ORC LLJIT, eagerly compile it for the host, and run its `main()`. The IR
// emission paths (emit_llvm / print_module, behind `clotho build`) remain for debugging.

#include <span>
#include <string_view>

#include "Module.h"
#include "SimConfig.h"
#include "Timing.h"

namespace clotho::ir {

// Build, JIT-compile and execute `main()` for `m` on the host. Returns the
// process exit code of the run: 0, unless the run ended in a `$fatal` (1, LRM
// 20.10) or was interrupted by SIGINT (130). A `$finish` of any level, `$error`,
// `$warning` and a failed assertion leave it 0. On any setup or compilation
// failure, writes the reason to llvm::errs() and returns a negative sentinel so
// the driver can exit non-zero. If `timings` is non-null, the LLVM compile and
// simulation phase durations are recorded into it. `config` is forwarded to
// build_modules; `config.stop_time_fs` also ends the run at that simulation time
// (see rt_stop_time_set). `plusargs` are the command-line arguments that followed
// a leading `+` (LRM §21.6), in source order, with the `+` itself stripped; made
// available to $test$plusargs / $value$plusargs. `resolve` decodes the source
// locations a sanitizer bakes into its reports and the one a backend refusal
// points at (see build_modules); without it they read "<unknown>".
int run_jit(const Module& m, Timings* timings = nullptr, const SimulationConfig& config = {},
           std::span<const std::string_view> plusargs = {}, const LocResolver& resolve = {});

} // namespace clotho::ir
