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
    // Fuse dependent combinational processes (pure, level-sensitive, read set
    // covered by sensitivity — see comb_fuse_verdict) into rank-contiguous
    // groups running producer-before-consumer per activation, subscribed to
    // the union sensitivity (see docs/design/comb-fusion.md for the LRM as-if
    // argument). Disable for bisection / debugging. Requires `fuse`.
    bool comb_fuse = true;
    // Lower a suspension-free always-process (no #delay / intra-body wait / task
    // call, and no blocking write to its own sensitivity) to a plain thunk
    // called once per activation instead of an LLVM coroutine — eliding the
    // frame, the suspend state machine, and CoroSplit (see is_simple_always /
    // the thunk path in LlvmBackend.cpp). Disable for bisection / debugging;
    // semantics are byte-identical (same sched-flag timeline, same FIFO order).
    bool thunk = true;
    // Dynamic domain specialization (OPT-IN, off by default): compile every
    // eligible thunk group's body a second time under the assumption that its
    // 4-state reads are currently clean (no x/z bit), routed through the native
    // 2-state operator paths, and pick fast-vs-generic per activation with a
    // reader-side guard over the read set (see compute_domain_spec in
    // LlvmBackend.cpp). Semantics are identical either way — the fast body runs
    // only when the whole read set is clean at runtime, where 2-state and
    // 4-state lowering agree exactly. Off by default: the fast body pays
    // 4-state<->2-state boundary conversions that only op-dense bodies
    // amortize, so it is not a universal win; it targets op-dense workloads
    // over transiently-x (reset-then-run) signals a static analysis cannot
    // prove clean. When off, none of the specialization machinery is emitted
    // (the design compiles exactly as without the feature).
    bool domain_spec = false;
    // Register LLVM's perf JITEventListener on the object-linking layer so the
    // JIT'd design's functions show up in `perf` with real symbols. Emits a
    // jitdump (`$JITDUMPDIR/jit-<pid>.dump`) consumed by `perf inject --jit`.
    // Off by default: the listener adds per-object bookkeeping and writes the
    // dump file. Run-only knob (build_module ignores it).
    bool perf_jitdump = false;
};

} // namespace clotho::ir
