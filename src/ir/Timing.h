#pragma once

// Per-phase wall-clock timings for one clotho run, in milliseconds. Optionally
// filled in by the pipeline when the --time flag is set; a zero field means that
// phase did not run (e.g. simulation under --emit=llvm).

#include <chrono>

namespace clotho {

struct Timings {
    // slang parse + elaborate (front end).
    double frontend_ms = 0.0;
    // AST -> clotho IR lowering.
    double analysis_ms = 0.0;
    // LLVM IR generation, optimization, and JIT codegen.
    double llvm_ms = 0.0;
    // Running the compiled simulation.
    double sim_ms = 0.0;
};

// Milliseconds elapsed since `start`, on the monotonic clock.
inline double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace clotho
