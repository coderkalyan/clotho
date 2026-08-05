#pragma once

// Whole-design analysis: every fact a consumer of the IR needs about a frozen
// Module, computed once, in one place.
//
// Why this exists. The facts collected here used to live in two places and be
// recomputed several times over: three separate whole-module walks in
// ZElision.cpp, and roughly a dozen private methods of the LLVM backend's
// lowering class. That had three costs. Each analysis walked the instruction
// arrays again. None of the backend-private ones was reachable by any other
// consumer, even though `comb_fuse_verdict` (purity + sensitivity containment) is
// a general-purpose question. And -- worst -- classifying a NEW IR opcode meant
// finding every scattered switch and whitelist that had an opinion about it.
//
// So: one `analyze()`, one `DesignFacts`, and one `tag_info()` carrying the
// static per-opcode classifications. Adding an opcode now fails to compile in
// exactly two exhaustive switches (tag_info here, and the value sweep in
// Analysis.cpp) until it has been classified.
//
// What this is NOT. Nothing here mutates the IR; `Ir` is frozen by construction
// and Clotho deliberately has no IR-rewriting pass (see
// docs/design/synthesis-and-lsp-scoping.md §2). Every result is a side table a
// consumer reads while lowering. Facts that iterate -- the monotone may-x /
// may-z lattices -- iterate over the lattice, never over the IR.
//
// What is deliberately NOT here. Analyses whose *products* are backend index
// spaces rather than facts: `nba_classify_ir` (membership order in its vectors
// IS the deferred-record ABI), subscription ordering, fusion grouping, and
// dedup hashing. Those stay with the code generator that owns their meaning.
// This module supplies their ingredients.

#include <cstdint>
#include <span>
#include <vector>

#include "Ir.h"

namespace clotho::ir {

class Module;

// Static, unconditional per-opcode classification. Consolidates classifications
// that were previously separate lists which could -- and did -- drift.
//
// Only UNCONDITIONAL properties belong here. Several real classifications are
// operand- or payload-dependent (a `Severity` disqualifies a thunk only when its
// `defer` flag is set; a `Store` only when its target is in the process's own
// sensitivity list), and those stay as explicit code in the analysis rather than
// being flattened into a lie here.
struct TagInfo {
    // Pure: no effect beyond producing a value, and deterministic. This is the
    // comb-fusion purity WHITELIST: a spurious re-execution of a pure body is
    // unobservable, which is what makes combinational fusion legal
    // (docs/design/comb-fusion.md). Defaults to false, so a newly added opcode is
    // ineligible for fusion until someone classifies it -- fail safe.
    bool pure = false;

    // Unconditionally disqualifies a process from coroutine elision (thunk
    // lowering). This is a BLACKLIST and is NOT the inverse of `pure`: it defaults
    // to false, so an unclassified opcode stays eligible. Keeping the two
    // polarities apart is deliberate -- `pure` fails closed because mis-fusing is
    // a correctness bug, while thunk elision is a shape restriction whose
    // violations are enumerable. Do not merge them.
    bool blocks_thunk = false;
};

// Exhaustive over Inst::Tag: a new opcode fails to compile here (-Werror=switch)
// until classified.
//
// Note what is NOT here: z-absorption. It looks like it belongs, but the value
// sweep in Analysis.cpp already needs an exhaustive per-tag switch to know how
// each op PROPAGATES z, and "absorbs" is just one of its arms. Lifting that one
// arm out would mean two places enumerating tags for the same question -- the
// exact duplication this module exists to remove.
[[nodiscard]] TagInfo tag_info(Inst::Tag tag);

// Why a process is or is not eligible for combinational fusion. Ordered, and the
// order is load bearing: `report_comb_fuse_stat` indexes a histogram by the
// enumerator value, and `Count` sizes it.
enum class CombFuse : uint8_t {
    Eligible,
    // Fails the simple-always shape: suspension point, task call, self-sensitivity
    // write, or not an Always with a static sensitivity.
    NotSimpleAlways,
    // Sensitivity has a posedge/negedge entry. Comb fusion is level-only;
    // edge-triggered members belong to identical-sensitivity fusion.
    EdgeSensitive,
    // Body op with an observable effect or non-deterministic value
    // ($display, file I/O, $random, $time, string ops, ...).
    ImpureOp,
    // Function call: purity closure over callees not implemented yet.
    HasCall,
    // Nonblocking store: commits in the NBA region, outside the group's
    // execution, which breaks the internal-signal subscription reasoning.
    NbaWrite,
    // Store whose root is a string or unpacked array: those store paths' change
    // detection is not verified, so the no-change argument does not cover them.
    UnsupportedWrite,
    // Reads a signal absent from the sensitivity list: a spurious run would
    // observe a value the unfused schedule never samples (e.g. `@(a)` reading b).
    ReadOutsideSens,
    // Count of verdicts, for histogram sizing.
    Count,
};

// Facts about one lowered body (a process, or a function/task).
struct BodyFacts {
    // Signals this body blocking-writes: `Store` roots plus out/inout task-call
    // actuals (copy-out writes the actual in the caller). Roots that are not
    // signals (frame locals, formals) are skipped.
    //
    // DIRECT writes only -- not closed over callees. The transitive closure is the
    // consumer's job because the backend's closure is deliberately imprecise for a
    // recursive callee (an in-progress function contributes what it has so far,
    // which only perturbs ordering), and that imprecision must not be silently
    // "fixed" here. `calls` is the edge set to close over.
    //
    // Neither deduplicated nor sorted: consumers squash duplicates with a stamp
    // array, and imposing an order here would change nothing except allocation.
    std::vector<Signal> writes;
    // Signals this body reads, resolved to the base signal of each `Load`'s select
    // chain. Also undeduplicated -- every consumer does membership tests.
    std::vector<Signal> reads;
    // Tasks this body enables, in encounter order. The edge set for closing
    // `writes` transitively.
    std::vector<FunctionId> calls;
};

// Facts about one process, beyond its body.
struct ProcessFacts {
    BodyFacts body;
    // Shape eligible for coroutine elision: an Always with a static sensitivity
    // list, no suspension point, no task call, no deferred store, no $strobe, and
    // no blocking write to a signal in its own sensitivity list.
    bool simple_always = false;
    // Carries a deferred immediate assertion (`Severity` with defer set). Needed
    // per process, not just design-wide: the backend also asks this per fusion
    // group while lowering.
    bool deferred_severity = false;
    // Combinational-fusion verdict.
    CombFuse comb_fuse = CombFuse::NotSimpleAlways;
};

// Design-wide feature presence. Pure codegen elision gates: when false, the
// corresponding scheduler machinery is never emitted, so a design that does not
// use $strobe pays nothing for it.
struct DesignUses {
    bool strobe   = false; // StrobePend, in processes or functions
    bool monitor  = false; // MonitorSet, in processes or functions
    bool observed = false; // CaseViol, in processes or functions
    // ReactivePend, in processes or functions. Gates the reactive region set --
    // its queues, its drain blocks, and the Observed run that feeds it.
    bool reactive = false;
    // Severity with defer set. PROCESSES ONLY, unlike its three siblings: a
    // deferred assertion inside a called function or task is rejected in the
    // frontend, so a process scan already covers every case.
    bool defer = false;
    // Any $dumpfile / $dumpvars / $dump* control. Processes only.
    bool vcd = false;
};

// Everything analyze() knows about a Module.
//
// Per-signal facts are parallel arrays (structure-of-arrays): a design can have
// millions of signals, consumers scan one fact at a time, and `std::vector<bool>`
// is bit-packed. Per-process and per-function facts are structs, since there are
// thousands at most and their members are read together.
struct DesignFacts {
    // ---- Per-signal, indexed by Signal. ----

    // May carry a nonzero z plane. A false bit is a conservative PROOF of never-z,
    // which lets the backend drop the z word from the signal's state slot.
    //
    // Soundness: the z-source seed set is enumerated in Analysis.cpp. Any new
    // z-producing construct MUST seed there or elision becomes unsound -- and the
    // failure mode is a silent wrong value, not a crash.
    std::vector<bool> may_z;

    // May carry a nonzero x (unknown) plane. The may-plane analog of `may_z`.
    //
    // Coarse by construction: "never-x iff every operand is never-x", per
    // docs/design/never-x-inference.md §5. Sound, and sufficient for deciding
    // 2-state storage narrowing. NOT precise enough to drive a user-facing
    // diagnostic -- it does not model absorption, so `x & 1'b0` reports may-x even
    // though every semantics gives 0. A report needs the per-bit value lattice
    // that arrives with the abstract interpreter, not this bit.
    std::vector<bool> may_x;

    // Nothing anywhere in the design drives the signal: no store, no $readmem
    // target, no task out/inout copy-out. An undriven 4-state NET powers on to z
    // (LRM 6.6); an undriven variable powers on to x. Semantics, not optimization.
    std::vector<bool> undriven;

    // Passed as an output / inout / ref actual somewhere. Such a signal must keep
    // its z word even when proven never-z, because the pointer ABI reads and
    // writes the full { must|may, z } slot. A storage constraint, not a may-z
    // fact, which is why it is a separate bit.
    std::vector<bool> ptr_arg;

    // Per-bit undriven mask for a PARTIALLY driven packed 4-state net -- the
    // refinement of `undriven`, which is whole-signal (`w[3:0] = ...` marks all of
    // `w` driven, so its remaining bits would wrongly power on x instead of z).
    //
    // Flat rather than a vector-of-vectors: the mask is absent for almost every
    // signal, and per-signal vectors meant one heap allocation each. Read it
    // through undriven_mask().
    std::vector<uint64_t> undriven_bits;
    // Offsets into `undriven_bits`, size signals+1. Signal s owns
    // [undriven_off[s], undriven_off[s+1]); an empty range means no mask.
    std::vector<uint32_t> undriven_off;

    // ---- Per-body. ----
    std::vector<ProcessFacts> processes; // indexed as Module::processes()
    std::vector<BodyFacts>    functions; // indexed by FunctionId

    // ---- Design-wide. ----
    DesignUses uses;

    // Undriven-bit mask words of `s`, LSB-first; a set bit means undriven, hence
    // powers on to z. Empty for every signal without a partial mask -- fully
    // undriven (see `undriven`), fully driven, 2-state, or not a net.
    [[nodiscard]] std::span<const uint64_t> undriven_mask(Signal s) const {
        const auto i = static_cast<uint32_t>(s);

        return { undriven_bits.data() + undriven_off[i], undriven_off[i + 1] - undriven_off[i] };
    }
};

// Analyze a frozen module. Pure: no IR is modified and no global state is
// touched, so the result depends only on `m`.
[[nodiscard]] DesignFacts analyze(const Module& m);

} // namespace clotho::ir
