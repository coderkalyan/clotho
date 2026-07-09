#pragma once

#include <cstdint>

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
    // Dynamic domain specialization: compile an eligible thunk group's body a
    // second time under the assumption that every 4-state signal it reads is
    // currently clean (no x/z bit), routed through the native 2-state operator
    // paths, and dispatch per activation on a per-group dirty counter
    // maintained at write sites (see compute_domain_spec in LlvmBackend.cpp).
    // Disable for bisection / debugging; semantics are identical either way —
    // the fast body runs only when the whole read set is clean at runtime,
    // where 2-state and 4-state lowering agree exactly.
    bool domain_spec = true;
    // Profitability floor for domain specialization: a thunk group is
    // specialized only when its bodies hold at least this many eligible
    // 4-state compute ops (and 8x its store count) — the write-side tag
    // maintenance is paid design-wide, so the fast body must be decisively
    // op-dense to fund it (measured thresholds, docs/design/domain-spec.md).
    // 0 disables the gate (used by tests to exercise the machinery on small
    // bodies).
    uint32_t domain_spec_min_ops = 64;
    // Register LLVM's perf JITEventListener on the object-linking layer so the
    // JIT'd design's functions show up in `perf` with real symbols. Emits a
    // jitdump (`$JITDUMPDIR/jit-<pid>.dump`) consumed by `perf inject --jit`.
    // Off by default: the listener adds per-object bookkeeping and writes the
    // dump file. Run-only knob (build_module ignores it).
    bool perf_jitdump = false;
};

} // namespace clotho::ir
