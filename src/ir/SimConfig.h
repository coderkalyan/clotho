#pragma once

// Configuration knobs shared by the compile (LlvmBackend.h) and run (Jit.h)
// entry points. Grows as new optimizations and simulation modes land.

namespace clotho::ir {

struct SimulationConfig {
    // Run the coroutine-lowering pipeline (CoroSplit et al.), required to
    // produce runnable code — run_jit forces it on. False emits the raw
    // pre-split module (llvm.coro.* intrinsics intact) for manual review.
    bool optimize = true;
    // Drop the z word from the state slot of signals the may-z analysis
    // (ZElision.h) proves never carry z. Disable for bisection / debugging;
    // semantics are identical either way.
    bool z_elision = true;
    // Opt-in aggressive tier: replace the tier-0 coroutine pipeline with
    // LLVM's default<O3> module pipeline (which includes the coro passes) and
    // raise machine codegen from None/FastISel to Aggressive. Costs compile
    // time; wins simulation time on long traces.
    bool o3 = false;
    // Fuse always-processes with identical canonical sensitivity into one
    // coroutine (see compute_fusion_groups in LlvmBackend.cpp). Disable for
    // bisection / debugging; semantics are identical either way (the fused
    // member order is one of the LRM-allowed same-instant orders).
    bool fuse = true;
    // Lower a suspension-free always-process (no #delay / intra-body wait / task
    // call, and no blocking write to its own sensitivity) to a plain thunk
    // called once per activation instead of an LLVM coroutine — eliding the
    // frame, the suspend state machine, and CoroSplit (see is_simple_always /
    // the thunk path in LlvmBackend.cpp). Disable for bisection / debugging;
    // semantics are byte-identical (same sched-flag timeline, same FIFO order).
    bool thunk = true;
    // Commit whole-signal scalar NBAs per straight-line block instead of per
    // signal: each block's writes register one deferred commit in the NBA region,
    // so enqueue cost is O(branchiness) not O(signals written). Semantics are
    // byte-identical (same shadow last-write-wins, same NBA-region timing).
    // Partial (bit/part-select) and array-element NBAs are unaffected. Disable
    // for bisection / debugging. Measured: wideff N=256 sim 116 -> 59 ms,
    // picorv32 1710 -> 1527 ms.
    bool block_nba = true;
    // Register LLVM's perf JITEventListener on the object-linking layer so the
    // JIT'd design's functions show up in `perf` with real symbols. Emits a
    // jitdump (`$JITDUMPDIR/jit-<pid>.dump`) consumed by `perf inject --jit`.
    // Off by default: the listener adds per-object bookkeeping and writes the
    // dump file. Run-only knob (build_module ignores it).
    bool perf_jitdump = false;
};

} // namespace clotho::ir
