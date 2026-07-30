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

// Backend statistics selected by `--stat=<list>`, as a bitmask in
// `SimulationConfig::stats`. Diagnosis only: each one prints a report to stderr
// and changes nothing about the emitted code.
enum class Stat : uint32_t {
    // Twin-group dedup: how many groups collapsed onto a shared body, and the
    // time the comparison took (see share_twin_groups).
    Dedup = 1u << 0,
    // Dedup, plus the member sets behind each identity key.
    DedupSets = 1u << 1,
    // Comb-fusion eligibility histogram and the groups that formed.
    CombFuse = 1u << 2,
    // Which processes may reach $finish (drives the cooperative-finish path).
    Finish = 1u << 3,
    // Parallel compile: per-module optimize time, the serial sum, and the
    // speedup ceiling. `run` only.
    Parallel = 1u << 4,
};

[[nodiscard]] inline bool stat_on(uint32_t stats, Stat s) {
    return (stats & static_cast<uint32_t>(s)) != 0;
}

// Dynamic checks selected by `--sanitize=<list>`, as a bitmask in
// `SimulationConfig::sanitizers`. A sanitizer is not a `Stat`: it changes the
// emitted code (instrumentation, and a runtime report when a check trips) but
// never the design's own semantics, so a sanitized run computes exactly what
// an unsanitized one does.
enum class Sanitizer : uint32_t {
    // xsan, the x-provenance sanitizer: flag the point where an x/z value
    // decides something the design cannot recover from, and report the chain
    // of assignments that produced it. See docs/design/x-semantics.md.
    XProv = 1u << 0,
};

[[nodiscard]] inline bool sanitizer_on(uint32_t sanitizers, Sanitizer s) {
    return (sanitizers & static_cast<uint32_t>(s)) != 0;
}

struct SimulationConfig {
    // Run the coroutine-lowering pipeline (CoroSplit et al.), required to
    // produce runnable code — run_jit forces it on. False emits the raw
    // pre-split module (llvm.coro.* intrinsics intact) for manual review
    // (`build --emit=llvm-raw`).
    bool lower_coro = true;
    // Drop the z word from the state slot of signals the may-z analysis
    // (Analysis.h) proves never carry z. Semantics are identical either way.
    //
    // This and the feature gates below (`fuse`, `comb_fuse`, `thunk`, `dedup`,
    // `outline`, `split`, `parallel_compile`) exist for differential testing:
    // an as-if optimization must produce byte-identical output with the gate
    // off, which is what the tests A/B. They are deliberately NOT on the CLI —
    // an operator has no reason to reach for one.
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
    // Worker threads for parallel_compile; 0 = llvm::hardware_concurrency().
    // Taken from slang's `-j/--threads`, so one option sizes both its parser and
    // this pool. Run-only knob (build_module ignores it).
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
    // Statistics to report (`--stat=<list>`): a mask of `Stat` bits, tested
    // with stat_on. Zero reports nothing.
    uint32_t stats = 0;
    // Dynamic checks to instrument (`--sanitize=<list>`): a mask of
    // `Sanitizer` bits, tested with sanitizer_on. Zero instruments nothing,
    // which is the default — a sanitizer costs code size and simulation time.
    uint32_t sanitizers = 0;
};

} // namespace clotho::ir
