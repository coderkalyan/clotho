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

#include <ankerl/unordered_dense.h>

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

    // Maps to hardware. This is the process-legalization WHITELIST and it defaults
    // to false, so an unclassified opcode makes its process unsynthesizable rather
    // than silently legalizing into the wrong thing -- the same fail-safe polarity
    // as `pure`, and for the same reason.
    //
    // Deliberately NOT the inverse of anything else here. A float op is `pure` and
    // not synthesizable; `Store` is neither pure nor thunk-blocking and IS
    // synthesizable. Three independent questions, three fields.
    //
    // "Synthesizable" here is per-opcode and structural only. Shape restrictions
    // that depend on operands rather than the opcode -- a loop whose bound is not
    // static, a call that cannot be inlined -- are the legalizer's job, not this
    // table's.
    bool synthesizable = false;
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

// Where a call edge lives in the IR, and the only place that answers it. Seven
// independent per-body scans used to each re-derive the tag set and disagree:
// some had no `Fork` arm, some collected nothing for a value `Call`. Classifying
// a new call-shaped opcode now means editing one switch, and every consumer
// gains the edge at once.
//
// Fork children count. The frontend splits each child statement of a `fork` into
// its own fork-child task and records their FunctionIds in `ForkInfo::children`
// (Ir.h:483-503), so a child is a body of THIS one that the parent enables. Leave
// them out and a child of an action block or of a program `initial` parks a
// handle with no home region, and a combinational cycle whose back edge closes
// through a child never forms an SCC.
//
// Virtual dispatch contributes its STATICALLY resolved target only --
// `VirtualCallInfo::func`, documented at Ir.h:461-473 as not necessarily the
// function actually called. A consumer that must cover every override unions the
// per-slot lists itself, as the by-pointer argument scan does; a consumer that
// only needs a lower bound (may-finish) treats the figure as one and says so.
//
// `out` is APPENDED to, in encounter order, neither sorted nor deduplicated:
// every caller either worklists with a seen-stamp or closes with a memo, so an
// order or a set here would buy nothing but an allocation.
void inst_callees(const Ir& ir, Index i, std::vector<FunctionId>& out);
// Every call edge of a whole body: `inst_callees` over each instruction of `ir`.
void collect_callees(const Ir& ir, std::vector<FunctionId>& out);

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
    // Every function this body calls, enables or forks, in encounter order --
    // `inst_callees` above defines that set, and this is exactly it. The edge set
    // for closing `writes` transitively.
    //
    // Conservative rather than exact, in two directions, deliberately. A
    // `join_any` / `join_none` child outlives the fork, so a write of its may land
    // in a LATER instant than the edge claims; and a virtual call contributes its
    // static target, which need not be the body that runs. The sole consumer --
    // the wake graph in `compute_subscription_order` -- is over-approximation
    // tolerant BY CONSTRUCTION: a spurious edge costs a conservative SCC (fewer
    // fusions, a longer settle), a missing one costs a dropped loop iteration. So
    // this set errs towards too many edges, never too few.
    //
    // `reads` is NOT closed over this, and must not be. A fork parent does not
    // inherit its children's sensitivity: docs/design/fork-join.md §D
    // ("Sensitivity attribution") gives each child its own subscription, and
    // absorbing a child's reads here would wake the parent on signals its own
    // body never samples.
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
    // Any $dumpfile / $dumpvars / $dump* control, in processes or functions.
    bool vcd = false;
    // A remote `disable` (LRM 9.6.2) anywhere: in processes or functions, unlike
    // `defer`. Gates the per-target occupant slots, the force-wake, and the poll
    // after each suspend inside a targeted region -- a design with no remote
    // `disable` emits none of it.
    bool disable = false;
    // Any `process` class op (LRM 9.7): ProcQuery / ProcCtl / ProcWait, in
    // processes or functions, like `disable`. Gates the per-thread control blocks,
    // the ABI argument carrying the calling thread's block, the status stores at
    // every park and the pooled blocks fork children take -- a design that never
    // names `process` emits none of it (docs/design/process-class.md §9).
    bool process = false;
    // A `process` randstate op (srandom / get_randstate / set_randstate). Forces every
    // thread to OWN its generator state rather than sharing the module-wide one: the
    // target of `p.srandom()` is a handle, so which thread gets reseeded is not
    // statically known (docs/design/process-class.md §7.4).
    bool process_randstate = false;
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

    // Passed as a `ref` actual somewhere. Stronger than ptr_arg: a `ref` formal
    // ALIASES this storage rather than copying through it, so for an unpacked
    // aggregate the two sides' layouts must be IDENTICAL, not merely both
    // readable. The callee's formal always has the canonical (never-narrowed)
    // aggregate mirror, so the actual must take it too — a flat array signal's
    // per-signal z-elision would otherwise give the callee a different element
    // stride, and a write through the ref would land in a neighbouring element's
    // z word.
    std::vector<bool> ref_arg;

    // Per-bit undriven mask for a PARTIALLY driven 4-state net -- the refinement
    // of `undriven`, which is whole-signal (`w[3:0] = ...` marks all of `w`
    // driven, so its remaining bits would wrongly power on x instead of z). For a
    // fixed unpacked array of packed elements the mask spans every leaf, bit
    // `leaf ordinal * element width + bit` (leaves in row-major order).
    //
    // Flat rather than a vector-of-vectors: the mask is absent for almost every
    // signal, and per-signal vectors meant one heap allocation each. Read it
    // through undriven_mask().
    std::vector<uint64_t> undriven_bits;
    // Offsets into `undriven_bits`, size signals+1. Signal s owns
    // [undriven_off[s], undriven_off[s+1]); an empty range means no mask.
    std::vector<uint32_t> undriven_off;

    // `ref` formals whose actual, at some call site, is a place inside a container's
    // HEAP storage — a dynamic-array / queue element, an associative-array entry, or
    // a member or element reached through one. Keyed `(fid << 32) | formal index`,
    // mapping to the NESTING DEPTH: how many relocatable containers the place sits
    // inside, maxed over every call site.
    //
    // Only those formals carry the relocation-epoch words of the fat ref
    // (docs/design/data-model.md §3): the epoch detects that the buffer moved out
    // from under an interior pointer, and it necessarily lives in the handle rather
    // than the buffer, since a stale pointer must not be what reads its own witness.
    // A ref to anything else — a variable, a fixed-array element, a struct member, a
    // class property, or a whole container handle — aliases storage that cannot
    // move, so its formal is one plain pointer and the callee emits no check.
    //
    // The depth is what makes a NESTED place checkable. `q[i]` sits in one buffer
    // and needs one witness, which lives in the queue's handle — design state, so
    // safe to read unconditionally. `a[k][i]` sits in two: the inner queue's handle
    // lives inside the map's payload, so its witness may itself have been freed, and
    // reading it is only safe once the OUTER witness has been checked. Hence one
    // (epoch, expect) pair per level, outermost first, checked in order.
    //
    // Conservative by construction: a formal is absent only when EVERY call site was
    // seen and none was container-rooted, and the answer is unioned across each
    // virtual-dispatch slot so an override can never disagree with its caller.
    ankerl::unordered_dense::map<uint64_t, uint32_t> ref_epoch_formals;

    // How many (epoch, expect) pairs ref formal `k` of `fid` carries; 0 = a plain
    // pointer with no check.
    [[nodiscard]] uint32_t ref_epoch_depth(uint32_t fid, size_t k) const {
        const auto it = ref_epoch_formals.find((static_cast<uint64_t>(fid) << 32) | k);
        return it == ref_epoch_formals.end() ? 0u : it->second;
    }

    // Whether ref formal `k` of `fid` carries any epoch words.
    [[nodiscard]] bool ref_needs_epoch(uint32_t fid, size_t k) const {
        return ref_epoch_depth(fid, k) != 0;
    }

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
