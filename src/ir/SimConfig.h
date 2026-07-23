#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Configuration knobs shared by the compile (LlvmBackend.h) and run (Jit.h)
// entry points. Grows as new optimizations and simulation modes land.

namespace clotho::ir {

// Access tier for the $system shell-exec task (LRM §20.17.1). $system runs an
// arbitrary shell command, so it is refused by default and gated behind an
// explicit `--allow-system=<tier>` operator opt-in. The enum values are stable
// (passed to the runtime backstop rt_system_set_access as an int32_t).
enum class SystemAccess : uint8_t {
    // Default: any $system in the source is a hard compile error; no exec code
    // is emitted at all.
    Off = 0,
    // $system allowed only when its command string constant-folds at
    // elaboration (so the command cannot depend on any runtime input). A
    // non-constant command string is a compile error.
    Const = 1,
    // $system allowed with a runtime-computed command string. Full trust in the
    // design and all of its runtime inputs.
    Full = 2,
};

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
    // Share one compiled body across twin instances of a reused module
    // definition: thunk groups whose lowered functions are identical modulo
    // their instance-context symbol collapse to ONE shared body (ctx passed
    // as a parameter) plus a two-instruction musttail stub per instance
    // (see share_twin_groups in LlvmBackend.cpp). Sharing is gated on
    // literal post-lowering IR identity, so semantics are byte-identical
    // either way. Disable for bisection / debugging.
    bool dedup = true;
    // Under `split`: lower a cross-instance fused thunk group as per-member
    // functions emitted into each member's own definition unit plus a root
    // dispatcher calling them in rank order, so the member code is
    // per-definition cacheable (the merged form lives in the always-
    // recompiled root). Costs one call per member per activation. Disable
    // for bisection; no effect with split off.
    bool outline = true;
    // Emit one llvm::Module per elaborated definition (unit = the definition
    // state-struct identity) holding its single-scope process groups, plus a
    // root module with everything else (state globals, scheduler machinery,
    // functions/tasks, cross-instance groups, main). Prep for per-definition
    // object caching; the JIT links the set into one dylib, so semantics are
    // identical either way. Disable for bisection / debugging (one module,
    // exactly the pre-split emission).
    bool split = true;
    // Register LLVM's perf JITEventListener on the object-linking layer so the
    // JIT'd design's functions show up in `perf` with real symbols. Emits a
    // jitdump (`$JITDUMPDIR/jit-<pid>.dump`) consumed by `perf inject --jit`.
    // Off by default: the listener adds per-object bookkeeping and writes the
    // dump file. Run-only knob (build_module ignores it).
    bool perf_jitdump = false;
    // Compile the split per-definition modules concurrently: each is migrated
    // into its own LLVMContext (a bitcode round-trip; LLVMContext is not
    // thread-safe, so this can't happen while modules still share one) and
    // optimize_module + ORC codegen for independent modules run across a
    // thread pool instead of one sequential loop. Semantics are identical
    // either way (same per-module IR, same optimize_module/verify sequence).
    // Disable for bisection / debugging, or when isolating a single-module
    // compile-time regression. Run-only knob (build_module ignores it).
    bool parallel_compile = true;
    // Worker thread cap for parallel_compile; 0 = llvm::hardware_concurrency().
    // Run-only knob (build_module ignores it).
    unsigned compile_jobs = 0;
    // Shared libraries (`--dpi-lib <path>`, repeatable) searched to resolve
    // `import "DPI-C"` symbols at JIT link time. Each is added to the JIT
    // dylib as a DynamicLibrarySearchGenerator (see run_jit).
    std::vector<std::string> dpi_libs;
    // Access tier for the $system shell-exec task (`--allow-system=const|full`).
    // Off by default: $system is refused at compile time and no exec code is
    // emitted. This is a security boundary, not an optimization knob — running a
    // shell command is arbitrary code execution, gated on an explicit opt-in.
    SystemAccess allow_system = SystemAccess::Off;
};

} // namespace clotho::ir
