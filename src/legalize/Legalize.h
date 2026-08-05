#pragma once

// Process legalization: what hardware does each always-block imply?
//
// Reads a frozen `ir::Module` and produces, per (process x written signal), the
// ordered list of guarded assignments that describes it -- which is simultaneously
// the mux tree, the completeness answer (hence latch inference), and the input to
// storage decomposition. See docs/design/process-legalization.md.
//
// Three properties, all load-bearing:
//
//  * **One walk, all signals.** The guard stack is shared, so the marginal cost of
//    another written signal is one vector push. yosys's proc_mux re-walks the whole
//    decision tree once per signal and is measurably O(S^2) -- 18.3 s on hazard3,
//    against 11.5 ms for Clotho's existing whole-design analysis pass.
//
//  * **Nothing is mutated.** Input is `const`. yosys's equivalent destructively
//    consumes its case tree to amortise the re-walks; not re-walking removes the
//    need.
//
//  * **No severity, no rendering, no policy.** This says "y is a latch"; whether
//    that is an error belongs to DiagnosticPolicy, which is the caller's. Findings
//    carry structured operands, never English.

#include <cstdint>
#include <span>
#include <vector>

#include "ir/Analysis.h"
#include "ir/Ir.h"
#include "ir/Module.h"

namespace clotho::legalize {

// -- Guards --------------------------------------------------------------------
//
// A guard is the path condition reaching a statement: a conjunction of atoms. It
// is stored as a persistent list into an arena rather than a copied stack, so a
// push is one append, a snapshot is a u32, and shared prefixes are shared rather
// than re-copied. Memory is O(branch edges), not O(paths x depth) -- which matters
// because a decision tree IS a prefix tree.
enum class GuardId : uint32_t { None = 0xFFFFFFFFu };

enum class AtomKind : uint8_t {
    // The `cond` predicate held / did not hold.
    CondTrue,
    CondFalse,
    // The switch selector matched this arm's key list / matched no arm.
    SwitchArm,
    SwitchDefault,
};

struct Atom {
    // The `cond` predicate, or the `switch` selector.
    ir::Index pred = ir::Index{};
    AtomKind  kind = AtomKind::CondTrue;
    // For SwitchArm, the arm's key slice; unused otherwise.
    ir::ExtraSlice keys{};
};

struct GuardNode {
    GuardId parent = GuardId::None; // None == the empty (always-true) guard
    Atom    atom{};
};

// -- Assignments ---------------------------------------------------------------

struct GuardedStore {
    GuardId   guard       = GuardId::None;
    ir::Index value       = ir::Index{}; // the RHS cone
    ir::Index store       = ir::Index{}; // the originating instruction, for spans
    bool      nonblocking = false;
    // Written inside a loop, so this store happens once per iteration. The guard
    // does not say that -- a loop adds no atom -- and a consumer that renders or
    // counts assignments has to know. Materialize refuses such a signal rather
    // than emitting the body once and silently dropping the iteration.
    bool in_loop = false;
};

// -- Findings ------------------------------------------------------------------
//
// Structured, never rendered here. The frontend maps these to Diagnostic and
// applies severity; see the three rules in the header comment.
enum class Finding : uint16_t {
    // A tag with no hardware meaning appeared in the body.
    UnsynthesizableOp,
    // Suspension (`#delay`, `wait`) outside a recognised clock wait.
    Suspends,
    // A call this legalizer will not inline.
    Call,
    // `fork`.
    Fork,
    // No trigger is free of the guards, or more than one is: no identifiable clock.
    AmbiguousClock,
    // A non-clock trigger drives this signal to a value that is not a constant.
    // No standard primitive gates a reset pin or loads a computed value on an
    // asynchronous edge, so this is reported rather than built. The note names the
    // SIGNAL and the process; the guard that made it conditional is in the decomp's
    // async entries, which is where the answer actually is -- yosys blames the
    // sensitivity list instead, which is not where the problem is.
    AsyncLoad,
};

struct Note {
    Finding        kind{};
    uint32_t       process = 0;
    ir::Signal     signal  = ir::Signal{}; // invalid when the finding is process-wide
    ir::SourceSpan span{};
    // Opcode for UnsynthesizableOp; unused otherwise. Structured, not a string.
    ir::Inst::Tag tag{};
};

// -- Result --------------------------------------------------------------------

// Every assignment to one signal within one process, in program order. Program
// order is priority order for the mux fold: the LAST matching assignment wins, so
// the tree is built from the end backwards.
// What one written signal becomes. M3 settles the KIND; the full decomposition
// (async list, sync reset, enable, next-state cone) is M5.
enum class StorageKind : uint8_t {
    // Level-sensitive and assigned on every path.
    Comb,
    // Level-sensitive and NOT assigned on every path: it holds.
    Latch,
    // Edge-triggered. `async_reset` says whether a non-clock trigger drives it to
    // a constant.
    Flop,
    // Edge-triggered, but a non-clock trigger drives it to a NON-constant value.
    // No standard primitive does this; it is reported, not built. (yosys invents
    // $aldff and warns; YosysHQ advise against it.)
    AsyncLoad,
    // The process was rejected, or no clock could be identified.
    Unclassified,
};

// One (condition, value) row of a decomposition.
//
// `value` is an IR value rather than an interned constant. The design sketch said
// `Ref`, but that cannot hold an async LOAD, whose value is a cone by definition --
// and constness is already answerable with the value in hand, so one shape covers
// reset, conditional reset and load instead of three.
struct Entry {
    // The path under which this value is written. GuardId::None is unconditional.
    // Only meaningful when the result was produced with Options::keep_guards, the
    // same contract GuardedStore::guard carries.
    GuardId   condition = GuardId::None;
    ir::Index value     = ir::Index{};
};

// What one non-clock trigger does to a signal.
//
// A LIST, not a single (trigger, value): pinning `arst` in
// `if (arst) q<=0; else if (srst) q<=8'hAA; else q<=d;` leaves
// `q <= srst ? 8'hAA : 8'h00` -- loaded asynchronously, because the block fires on
// arst's edge and srst's branch is what executes. Realizing it needs CLR on
// `arst && !srst` and SET on `arst && srst`. A (trigger, value) pair cannot express
// that, because the condition involves `srst`, which is not a trigger.
//
// Entries are verbatim from the residue, in program order: the LAST whose
// condition holds is the one that applies, exactly as the source means.
struct AsyncGroup {
    ir::Signal         trigger = ir::Signal{};
    ir::Edge           edge    = ir::Edge::Any;
    std::vector<Entry> entries;
};

// The hardware one written signal implies, past its kind.
struct Decomp {
    // Edge-triggered only.
    ir::Signal clock      = ir::Signal{};
    ir::Edge   clock_edge = ir::Edge::Any;

    // One group per non-clock trigger, in sensitivity-list order.
    std::vector<AsyncGroup> async;

    // The synchronous body: what runs with every async trigger deasserted, split
    // by whether the value is a constant. `sync` is the synchronous reset (several
    // rows when the reset value is itself conditional); `next` is the next-state
    // cone. Both in program order, last match wins.
    std::vector<Entry> sync;
    std::vector<Entry> next;

    // Enable is the negation of hold. When false, the enabling condition is the
    // disjunction of the entries' conditions -- which the caller can form, and
    // legalize deliberately does not, because building it would mean creating IR.
    bool always_enabled = true;
};

struct SignalStores {
    ir::Signal                signal = ir::Signal{};
    std::vector<GuardedStore> stores;
    // True when some path through the body assigns nothing. This is the whole of
    // latch inference and it is structural -- an `if` with no `else` leaves an
    // empty region, and that region is right there in the IR.
    bool has_hold = false;

    StorageKind kind = StorageKind::Unclassified;
    // The trigger that appears in no guard. Valid when kind is Flop or AsyncLoad.
    ir::Signal clock = ir::Signal{};
    // True when some non-clock trigger drives this signal to a constant.
    bool async_reset = false;

    // The full description. Empty for an Unclassified signal.
    Decomp decomp;
};

struct ProcessResult {
    // False when the body contains something with no hardware meaning. The stores
    // below are still populated for whatever did legalize, because a partial
    // answer is what an editor wants.
    bool                      synthesizable = true;
    std::vector<SignalStores> signals;
};

struct Options {
    // Retain the guard arena. Consumers that only want classification do not pay
    // for it; Materialize does.
    bool keep_guards = false;
};

struct Result {
    std::vector<ProcessResult> processes; // indexed as Module::processes()
    std::vector<GuardNode>     guards;    // empty unless Options::keep_guards
    std::vector<Note>          notes;

    // Walk a guard from leaf to root. Empty when the arena was not kept.
    [[nodiscard]] std::span<const GuardNode> arena() const {
        return guards;
    }
};

// `facts` may be null; absent means the facts-dependent findings are skipped
// rather than guessed.
[[nodiscard]] Result run(const ir::Module& module, const ir::DesignFacts* facts, Options options = {});

} // namespace clotho::legalize
