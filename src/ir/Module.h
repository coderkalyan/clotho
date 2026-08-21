#pragma once

// Module: the container that owns a design's processes and the signals they
// reference. The process IR (Ir.h) models one process and refers to signals by
// opaque `Signal` handle; this is the enclosing object that resolves those
// handles to names / types and holds the lowered processes together.
//
// Ownership: a Module owns the InternPool shared by every process it holds, so
// each Process's `Ir.pool()` stays valid for the module's lifetime. The pool
// (and thus the Module) is non-movable, so a Module is the owning root,
// typically held behind a unique_ptr by the front-end.

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "InternPool.h"
#include "Ir.h"

namespace clotho::ir {

// A declared net / variable / port of the module. Its index in Module::signals
// IS the `Signal` handle the process IR carries.
struct SignalInfo {
    // Port direction, or Internal for a signal that is not a port.
    enum class Direction : uint8_t {
        Internal,
        Input,
        Output,
        Inout,
    };

    // Declared name.
    std::string name;
    // Location of the declaration's name token, for "go to declaration" and for
    // attributing a diagnostic to the signal. A bare SourceLoc, not a SourceSpan:
    // a large design has a signal per bit-ish, so 8 bytes here is a real cost and
    // an extent buys nothing a declaration lookup needs. SourceLoc{0} for a
    // compiler-synthesized signal (a driver slot, a synthesized event net).
    SourceLoc loc = SourceLoc{ 0 };
    // Interned type in the module's pool.
    Ref type = Ref::Invalid;
    // Direction relative to the module boundary.
    Direction dir = Direction::Internal;
    // A net (wire/tri/uwire), as opposed to a variable. An undriven 4-state net
    // defaults to z (LRM 6.6); an undriven variable defaults to x. Also the
    // seed condition for the may-z analysis (Analysis.h).
    bool is_net = false;
    // A named event (LRM 15.5), modeled as a synthetic 1-bit signal that owns
    // wait queues but carries no data value: `-> e` services its Any-edge queue
    // and stamps a per-event trigger time, `@(e)` / `always @(e)` subscribe, and
    // `e.triggered` compares the stamp to the current instant. `type` is a
    // 1-bit logic placeholder; the value planes are never read or written, so
    // value-oriented passes (VCD, edge classification) skip it.
    bool is_event = false;
    // Force the canonical aggregate storage mirror (never may-z-narrowed) for an
    // unpacked array, even when a plain array would otherwise take flat storage.
    // Set on a compiler temp that materializes a by-value array actual so its
    // storage is layout-compatible with the callee's canonical aggregate formal.
    bool force_agg = false;
    // Elaboration-constant contents, as the pool value whose canonical bytes are
    // this signal's whole image (element-major planes for an array). Set only on
    // a compiler-materialized constant table: a parameter with an aggregate value
    // that a runtime index reads (`localparam int P[0:3]; x = P[i]`, LRM 6.20).
    // Such a signal has no writer anywhere in the design; the backend bakes the
    // bytes into its state field's static initializer and skips the power-on
    // fill, so materializing a table costs no instructions at any point.
    Ref const_data = Ref::Invalid;
};

// One elaborated instance in the design hierarchy. The frontend flattens every
// instance's signals into the module-wide table (Signal handles stay flat and
// globally unique); this tree records which scope each signal was declared in,
// so the backend can rebuild a per-module-definition state struct type and
// compose the types nested per instance. Signals of generate blocks and
// compiler temporaries belong to the enclosing instance's scope.
struct InstanceScope {
    // Design-unique scope handle, assigned by the frontend (Module::alloc_scope_id)
    // when the node is created. Process::scope refers to it, and the backend maps
    // it to the scope's addressing anchor — the authoritative process-to-instance
    // attribution the module-deduplication work keys on.
    uint32_t id = 0;
    // Instance name (the top module's name for a top scope).
    std::string name;
    // Module definition name. Instances of one definition with the same
    // parameterization declare identical signal type sequences, so the backend
    // keys state-type deduplication on this (verified against field types).
    std::string def_name;
    // Signals declared directly in this scope, in declaration order.
    std::vector<Signal> signals;
    // Child instances, in declaration order.
    std::vector<InstanceScope> children;
};

// How a process is scheduled. The fine-grained trigger condition lives in the
// process's wait list (Process::wait_events); this only separates one-shot
// startup processes from those that re-arm and rerun whenever they are woken.
enum class Activation : uint8_t {
    // Runs once, before any Initial/Always process starts, to apply static
    // variable declaration initializers (LRM 6.8). Its stores set state without
    // generating events, so no process observes an edge from initialization.
    PreInit,
    // Runs once per time step, before any process body of that step (LRM 4.4.2.1
    // Preponed). Only compiler-synthesized: it captures the sampled values a
    // concurrent assertion reads, because LRM 16.5 defines an assertion over the
    // values a variable held before the step's updates. The backend emits it as a
    // thunk called from sim_drain's post-tick block and once from emit_main, so
    // "before any body" holds at time zero too.
    //
    // Its stores set state without generating events, like PreInit: a shadow is
    // compiler-internal and nothing may wake on it. LRM 4.4.2.9 forbids value
    // changes once Postponed is reached, and sampling is equivalent to sampling
    // in the previous Postponed region -- this is that, done where clotho can
    // observe it, and it touches no design net.
    Preponed,
    // Runs at each settled point of a time step -- after the Active / Inactive /
    // NBA set has drained, before the Reactive region (LRM 4.4.2.5 Observed).
    // Only compiler-synthesized: the region exists "for evaluation of property
    // expressions when they are triggered", which is where a concurrent
    // assertion's monitor steps its automaton. Pass/fail code is NOT run here --
    // 4.4.2.5 requires it to be scheduled into the Reactive region of the same
    // time slot, which is what Inst::Tag::ReactivePend does.
    //
    // Emitted like Preponed and for the same reason: the backend runs it
    // explicitly (emit_observed_run), so it is filtered out of the seed and
    // subscribe loops and its wait_events are never consulted. A body detects
    // its own clocking event from the values Preponed sampled, per LRM 16.5.
    //
    // Unlike Preponed, its stores are ordinary design writes that generate
    // events, so an Observed body can wake an Active process; the drain returns
    // to the active region when that happens (see emit_active_run).
    Observed,
    // Runs in the Re-NBA region (LRM 4.4.2.8), after the Reactive and Re-Inactive
    // queues of the time slot have settled and beside the update region that
    // commits the nonblocking assignments reactive code made. Only
    // compiler-synthesized, and today only for a clocking block's synchronous
    // drives: LRM 14.16 puts a zero-skew drive "in the Re-NBA region of the time
    // step corresponding to the clocking event", which is after every process that
    // could issue one -- an Active-region process woken by the clocking block
    // event, and a program's Reactive code alike.
    //
    // Emitted like Observed (emit_renba_run), so it is filtered out of the seed and
    // subscribe loops and its wait_events are never consulted. Its stores are
    // ordinary design writes: the reactive drain re-settles the active set when one
    // wakes a process, which is what makes the driven value visible in the same
    // time slot. A body must be idempotent, because LRM 4.4.2.8's loop can re-enter
    // the region within one slot (LRM 14.16.2 names the resulting glitch).
    ReNba,
    // Runs once at time zero, then completes (an `initial` block).
    Initial,
    // An `initial` procedure of a `program` block (LRM 24.3): a one-shot process
    // like Initial, but seeded onto the reactive ring instead of the active one,
    // so its body runs in the Reactive region of the time slot rather than the
    // Active one. That is the whole difference -- a program initial is an
    // ordinary process with a coroutine frame, not a zero-time thunk, so it may
    // suspend on `@`, `wait` or a `#delay` and resume. The backend restores the
    // region on each resume (emit_suspend), because a waker pushes onto the
    // active ring without knowing the region its sleeper belongs to.
    Reactive,
    // Re-armed after each run and rerun whenever one of its wait events fires
    // (an `always` block or a continuous `assign`).
    Always,
    // Runs once at the end of simulation (a `final` block, LRM 9.2.3), on both
    // the natural-drain and `$finish` exit paths, after every other process has
    // settled. Zero-time: the body contains no timing controls, so it runs to
    // completion in a single resume.
    Final,
};

// The kind of signal change a wait event triggers on.
enum class Edge : uint8_t {
    // 0 -> 1 transition: @(posedge s).
    Positive,
    // 1 -> 0 transition: @(negedge s).
    Negative,
    // Any value change: level-sensitive sensitivity (@(s), @(a or b), @*) and,
    // for now, both-edge triggers.
    Any,
};

// One entry of a process's wait list: the process wakes when `signal` undergoes
// `edge`. A list of these is a process's full sensitivity.
struct WaitEvent {
    Signal signal = Signal{ 0 };
    Edge   edge   = Edge::Any;
    // Intra-body waits only: the SSA value of the event handle to park on, when the
    // wait names an event that has no signal of its own -- an array element, a class
    // property, a subroutine formal. kNoWaitHandle otherwise, including for a plain
    // `@(e)` on a declared event, which resolves through `signal`.
    //
    // A process's STATIC sensitivity can never carry one: it is subscribed from the
    // seed loop, a different function with no access to the body's values. A
    // sensitivity list that would need a handle is left in the body instead, where
    // it lowers as an ordinary intra-body wait (build_process).
    Index handle = kNoWaitHandle;
    // Intra-body waits only: the SSA value of a virtual interface handle, when the
    // wait names a member reached through one (`@(posedge vif.clk)`, LRM 25.9).
    // `signal` is then a REPRESENTATIVE of that member -- some instance's copy,
    // naming which member -- and this says which instance is actually waited on.
    // kNoWaitHandle otherwise. Mutually exclusive with `handle`.
    Index inst = kNoWaitHandle;
};

// One lowered process plus the metadata the IR body itself does not carry: how
// it is activated and the wait events that wake it.
struct Process {
    // Whether the process runs once at startup or re-arms on its wait events.
    Activation activation = Activation::Always;
    // Sensitivity list: the signal events that wake the process. Empty for an
    // `initial` block, and (for now) for processes whose sensitivity is implicit
    // or not yet inferred (continuous assigns, @*).
    std::vector<WaitEvent> wait_events;
    // True for a continuously-active process (continuous assign, module-port copy,
    // always_comb / always_latch): it must evaluate once at time zero so it settles
    // from the declaration-initialized values, then re-evaluate on its wait events
    // (LRM 9.2.2.2, 10.3). An edge- or explicit-list-sensitive `always` leaves this
    // false: it runs only when a wait event fires. Only meaningful with a non-empty
    // wait list (a sensitivity-less process already runs once via the seed loop).
    bool self_start = false;
    // Whether code this process runs may spawn a background process — a
    // `fork … join_none` inside a FUNCTION it calls (LRM 13.4.4). The LRM allows
    // that only when "the thread calling the function is created by an initial
    // procedure, always procedure, or fork block from one of those procedures",
    // and requires an error otherwise, so this separates the two populations
    // `Activation` conflates: a user `always` and a continuous assignment are both
    // Activation::Always, and only the first is a procedure. False for every
    // compiler-synthesized process, and for a `final` block, which is neither an
    // initial nor an always procedure and runs after the drain has ended (a child
    // pushed there could never be resumed). Read by check_spawn_roots.
    bool spawn_root = false;
    // Full source extent of the AST construct that produced this process (the
    // continuous assign, always block, ...). A span rather than a point because
    // this is what an editor highlights and what an inferred-hardware hint
    // attaches to; processes number in the thousands, so the extra 8 bytes are
    // free. Empty for a compiler-synthesized process (a wire resolver, a port
    // copy, the PreInit initializer pass).
    SourceSpan span;
    // The lowered process body.
    Ir ir;
    // Scope id (InstanceScope::id) of the instance whose definition declared
    // this process, or no_scope for a process that belongs to no single
    // instance (a per-top PreInit pass, package/$unit lowering).
    static constexpr uint32_t no_scope = ~0u;
    uint32_t scope = no_scope;
};


// -- Concurrent assertions (LRM 16) ------------------------------------------
//
// A compiled concurrent assertion, as plain data. The front-end builds an SVA IR
// through libsva's C API, lowers it to a monitor, and copies the tables out into
// this; the backend reads only this. libsva therefore links into the front-end
// alone -- nothing here depends on it, and no libsva code runs in a simulated
// process. See docs/design/concurrent-assertions.md.
//
// Every mask below is a bit set over automaton states, least-significant bit of
// word zero first, `words` words per row. The backend materializes a row as one
// arbitrary-precision LLVM constant, so `words` is a transport detail rather than
// a codegen one.

// What to do with a verdict. The first four mirror libsva's sva_kind and are
// checked at the boundary.
//
// Expect is clotho's own and deliberately last: libsva has no expect kind and
// needs none -- an `expect`'s PROPERTY semantics are exactly an assert's, so it
// is compiled as SVA_ASSERT and the directive distinction is carried here, the
// way `cover_matches` carries the one libsva's single cover kind cannot. What it
// selects is the SEEDING and the blocking wait (LRM 16.17): an attempt starts
// when the statement executes rather than every cycle, and the executing process
// is parked until that attempt resolves. See Inst::Tag::ExpectWait.
enum class AssertionKind : uint8_t { Assert, Assume, Cover, Restrict, Expect };

// The verdict lattice (LRM 16.12.2, docs/design/sva-property-layer.md 5).
//
// One obligation attempt is in exactly one of these three states after every
// cycle, and the three are mutually exclusive and exhaustive by construction:
// Holds is an attempt that reached a `last` state, Fails is one with no live
// state left, and Pending is one still running. Neither of the resolved two is
// derivable from the other -- an attempt seeded into a cycle whose first letter
// does not hold is empty immediately, which is a Fails and not an absence -- so
// occupancy is tracked rather than inferred (see MonitorState::occupied).
//
// Named, rather than left implicit in the three masks the step computes, because
// `not` is what makes the third member load-bearing: negating Holds is Fails and
// negating Fails is Holds, but negating Pending is Pending, and only the
// end-of-trace disposition below tells the two apart.
enum class AssertionVerdict : uint8_t { Pending, Holds, Fails };

// What a still-pending attempt does when the trace ends (LRM 16.12.2).
enum class AssertionStrength : uint8_t { Weak, Strong };

// The whole of the weak/strong distinction: which resolved verdict Pending
// collapses to at the end of the trace. `not` swaps the strength it carries, so
// this is the one rule negation has to invert -- which is why it is a named
// function rather than a `strength == Strong` spelled at each of the places that
// asks (both end-of-trace emitters, and the strength inversion in Sva.cpp).
constexpr AssertionVerdict verdict_at_end_of_trace(AssertionStrength strength) {
    return strength == AssertionStrength::Strong ? AssertionVerdict::Fails : AssertionVerdict::Holds;
}

// How the backend tracks the attempts one assertion has in flight. A monitor
// uses exactly one of these, chosen by libsva and reported at the boundary; see
// docs/design/sva-property-layer.md 3.
//
// Ring is the bounded discipline. The obligation's longest match bounds how many
// attempts can overlap, so every attempt gets a state vector of its own out of a
// pool of `depth`. Exact, and it is what every bounded property uses.
//
// Counted is what an UNBOUNDED obligation needs -- `always p`, `s_eventually p`,
// the `until` family -- where no bound sizes a pool at all. The obligation
// automaton is DETERMINISED instead, so an attempt occupies exactly one state and
// two attempts in one state have the same future by construction; the tracker
// then carries a COUNT per state rather than a vector per attempt. The number of
// states is fixed, so the storage is too, and only the counts grow.
//
// Merging the attempts into one state vector WITHOUT determinising is unsound,
// which is why this is a second discipline and not a verdict rule: a dead attempt
// has no state to be seen in, so the union of the live sets hides its failure.
// Measured exhaustively over every 6- and 8-cycle trace in ../libsva/test/merge.zig,
// with a two-cycle counterexample on `a ##1 b`.
enum class AssertionTracking : uint8_t { Ring, Counted };

// One conjunct of a guard: an index into AssertionMonitor::roots, possibly
// complemented. `negated` is the TWO-VALUED complement of the root's collapsed bit,
// not the user's `!`.
//
// Clotho's own lowering never sets it: libsva's IR requires two-valued atoms, which
// obliges a three-valued front-end to supply negation normal form with each polarity
// as its own atom, so clotho's `!a` is an atom over `!a` and collapses x to false
// there (concurrent-assertions.md 5.2-5.3). What libsva sets it for is its own
// STRUCTURAL negation -- the determinisation's cell splitter partitions the valuation
// space into `a` and not-`a` cells -- and that partition is over the collapsed
// decision "did the guard hold", which is what keeps it total under x. See Lower.cpp's
// automaton_satisfy.
struct AssertionLiteral {
    uint32_t root = 0;
    bool negated = false;
};

// A guard cube: the conjunction of literals[start .. start + len). An empty cube
// is `true`.
struct AssertionCube {
    uint32_t start = 0;
    uint32_t len = 0;
};

// One automaton: a Glushkov position automaton. One state per letter occurrence,
// no epsilon transitions, and homogeneous -- every transition entering a state
// carries the same guard, so a guard belongs to the state rather than to an edge.
struct AssertionAutomaton {
    uint32_t state_count = 0;
    // Words per state mask: (state_count + 63) / 64.
    uint32_t words = 0;
    // Guard index per state, into AssertionMonitor::cubes.
    std::vector<uint32_t> guard_of;
    // state_count * words: the states enterable one cycle after each state.
    std::vector<uint64_t> follow;
    // States a match may begin in, and states that complete one. `words` each.
    std::vector<uint64_t> first;
    std::vector<uint64_t> last;
    // States an attempt has FAILED in. Only meaningful when the obligation is
    // determinised (AssertionTracking::Counted), where the construction is TOTAL
    // and a dead attempt sits in a real state instead of vanishing -- which is
    // exactly what lets a count be read off it.
    //
    // Wider than the trap alone: every state with no path left to a `last` one is
    // in here too. An attempt that can no longer reach an exit has already failed,
    // and waiting for it to reach the trap would move the failure's cycle.
    std::vector<uint64_t> dead;
    // cubes.size() * words: per guard, the mask of states carrying it. Lets the
    // satisfied-state mask fold over guards instead of gathering per state.
    std::vector<uint64_t> guard_states;
};

// What an abort does to the attempts it covers, once its condition holds. The
// distinction a monitor-wide reset does not draw: a reset ABANDONS an attempt
// and leaves no verdict at all, while an accept DISCHARGES it, which is a pass
// an assert's action block and a cover's tally both count.
enum class AssertionAbortEffect : uint8_t { Accept, Reject };

// One `accept_on` / `reject_on` and their `sync_` forms (LRM 16.12.7), scoped to
// part of the property rather than to the whole monitor the way `disable iff`
// is. Mirrors libsva's sva_monitor_abort_* read-out.
struct AssertionAbort {
    // Index into AssertionMonitor::roots of the abort condition.
    uint32_t root = 0;
    AssertionAbortEffect effect = AssertionAbortEffect::Reject;
    // Whether the condition wants the Preponed sample. The `sync_` forms do; the
    // plain ones read live values, the same mechanism `disable iff` uses.
    bool sampled = false;
    // Whether firing settles the whole attempt, or only clears the states in
    // `mask` and leaves the rest of it running -- a reject under a disjunction
    // kills its own disjunct and no more.
    bool resolves_attempt = true;
    // Whether the abort's scope reaches the antecedent, i.e. it sits ABOVE an
    // implication rather than below one. Then it also covers the cycles an
    // attempt spends matching its antecedent, before any obligation exists, and
    // an antecedent match ending a cycle the condition holds in resolves with
    // the abort's verdict instead of seeding one. Always false without a
    // trigger.
    bool covers_trigger = false;
    // The obligation states the abort guards, `obligation.words` of them. An
    // abort is armed for a pending attempt exactly when that attempt can reach a
    // state in here, which is what confines it to the cycles the property
    // beneath it is really being evaluated in.
    std::vector<uint64_t> mask;
};

struct AssertionMonitor {
    // The directive's source-level label, for a report. Empty when unlabelled.
    std::string label;
    AssertionKind kind = AssertionKind::Assert;
    // `cover sequence` rather than `cover property` (LRM 16.12.6). The two lower
    // to the identical automaton and libsva has one SVA_COVER for both, so this
    // rides alongside `kind` instead of inside it: all it selects is the tally,
    // sequence MATCHES against attempts that succeeded. False for every other
    // kind.
    bool cover_matches = false;
    AssertionStrength strength = AssertionStrength::Weak;

    // What steps the monitor: one clock edge is one cycle.
    Signal clock{ 0 };
    Edge edge = Edge::Positive;

    // Boolean expressions evaluated once per cycle, one bit each, in this order.
    // Each is a root instruction in `body` producing a 1-bit value.
    std::vector<Index> roots;
    // Interned guard cubes over `roots`. Cube 0 is empty (always holds) and cube
    // 1 is the reserved unsatisfiable one.
    std::vector<AssertionCube> cubes;
    std::vector<AssertionLiteral> literals;
    // Index into `roots` of the disable-iff condition. Always a real root: an
    // assertion that never disables carries a constant false, which folds away.
    uint32_t reset_root = 0;
    // The aborts scoped to part of the property, in the order libsva lowers
    // them: INNERMOST first, because lowering appends outward. Where two of them
    // fire on one attempt in the same cycle the OUTERMOST wins, so a consumer
    // scans this list backwards -- `accept_on(a) (reject_on(b) p)` with both high
    // holds, per LRM 16.12.7. Empty for a property with none, the common case.
    std::vector<AssertionAbort> aborts;

    // The antecedent of an implication. `has_trigger` false means the property is
    // a bare sequence and an attempt starts every cycle.
    bool has_trigger = false;
    // Whether an attempt starts on the trigger's last cycle (|->) or the cycle
    // after (|=>). Meaningless without a trigger.
    bool overlap = false;
    AssertionAutomaton trigger;

    // The property's sequence. One attempt per seeding.
    AssertionAutomaton obligation;
    // Attempts that can be pending at once -- the obligation's longest match, so
    // the number of state vectors the backend allocates. Meaningful only under
    // AssertionTracking::Ring. A counted monitor has no pool to size, because its
    // obligation has no bounded longest match for one to be derived from.
    uint32_t depth = 1;
    // Antecedent attempts the trigger must tell apart: how many attempt ids a
    // backend rotates through, one id SET per trigger state. LRM 16.14.1 starts an
    // evaluation of the consequent per antecedent MATCH, so a cycle several matches
    // end in owes a verdict per match, and telling them apart is what this sizes.
    //
    // ZERO when one bit answers "did the antecedent match" and the whole mechanism
    // folds away, which is the common case: no trigger at all, an antecedent matching
    // in a single cycle (`a |=> b`), or one whose matches all span the SAME number of
    // cycles (`(a ##1 b) |-> c`) -- distinct start cycles then end in distinct cycles,
    // so at most one match can end per cycle. libsva's number, not derived from the
    // trigger's longest match, because that last case is invisible to a length.
    //
    // Never unbounded: an antecedent with no bounded longest match is refused
    // (SVA_ERROR_UNBOUNDED_ANTECEDENT), so a non-zero window is always a real bound.
    uint32_t trigger_window = 0;
    // Which discipline the backend must emit for this monitor.
    AssertionTracking tracking = AssertionTracking::Ring;

    // Full extent of the assert/cover statement, for the failure report.
    SourceSpan span;
    // Precomputed "<file>:<line>: " prefix, so the backend stays slang-free.
    std::string banner;
};

// Direction of a subroutine formal argument (LRM 13.5). Out / Inout copy the
// formal back to the caller at completion (tasks only for now). Ref binds the
// formal to the actual's live storage — no copy either way; the callee reads and
// writes it through a passed pointer (functions and tasks). arg_dirs is populated
// for functions too once any formal is non-In.
enum class ArgDir : uint8_t {
    // Copy in at call (input).
    In,
    // Copy out at completion (output).
    Out,
    // Copy in at call and out at completion (inout).
    Inout,
    // Pass by reference: the formal aliases the actual's storage, accessed live
    // through a pointer. No copy-in / copy-out.
    Ref,
};

// The DPI-C canonical scalar type of a DPI import formal or return (LRM 35.5.6,
// the by-value subset of Table 35-1): each maps to a native C scalar passed in a
// register, so the boundary is a width coercion, not a marshal. Wider SV types
// (packed vectors, open arrays, strings) use the svBitVecVal / svLogicVecVal
// pointer ABI and are not lowered yet — the frontend loud-fails them, so this
// enum has no "not scalar" member. Only meaningful on an is_dpi Function.
enum class DpiCType : uint8_t {
    Void,        // void return
    Byte,        // SV byte      -> char       (i8, signed)
    Shortint,    // SV shortint  -> short      (i16, signed)
    Int,         // SV int       -> int        (i32, signed)
    Longint,     // SV longint   -> long long  (i64, signed)
    Real,        // SV real      -> double
    Shortreal,   // SV shortreal -> float
    BitScalar,   // SV bit       -> svBit   (unsigned char: 0/1)
    LogicScalar, // SV logic/reg -> svLogic (unsigned char: 0/1/2=Z/3=X)
    Chandle,     // SV chandle   -> void*   (opaque handle, i64 <-> ptr)
    String,      // SV string    -> char*   (input: rt_string_data; return: copy-in)
    // Packed vectors wider than a scalar map to a by-pointer chunk array (LRM
    // 35.5.6.2). Always passed as a pointer, every direction. Never a return type
    // (slang rejects a packed-array DPI return — a return is a "small value").
    BitVec,   // SV 2-state packed (bit[N] / time / packed struct) -> svBitVecVal*
    LogicVec, // SV 4-state packed (logic[N] / integer)           -> svLogicVecVal*
    // An unsized `[]` (open) array formal (LRM 35.5.6.2) -> svOpenArrayHandle. The
    // element's own DpiCType is carried in Function::dpi_arg_elem_ctypes; the
    // bounds come from the actual at each call.
    OpenArray,
};

// A lowered SystemVerilog function or task. Its body is an Ir whose leaves are
// Arg (parameters), Local (frame storage), and Signal (module signals it
// references); it ends every path in a Ret. A function becomes one plain LLVM
// function called at each `call` site (recursion / `automatic` lifetime fall out
// of normal stack frames). A task (is_task) becomes a switched-resume coroutine,
// like a process, so it can consume time (#delay / @event); it is driven at each
// `task_call` site and blocks the caller until it completes (LRM 13.3).
struct Function {
    // Function / task name (for the emitted symbol and diagnostics).
    std::string name;
    // Return type; `pool.void_type` for a void function (no return value). A task
    // is always void.
    Ref return_type = Ref::Invalid;
    // Parameter types, in declaration order. The frontend resizes each input call
    // argument to the matching type; output / inout arguments pass by pointer.
    std::vector<Ref> param_types;
    // Direction of each parameter, parallel to param_types. Populated whenever any
    // formal is non-In (tasks always, and a function with a ref formal); empty for
    // a plain all-input function.
    std::vector<ArgDir> arg_dirs;
    // Storage type of every frame local, indexed by the Local handle's index:
    // the parameter copies, the return variable, and body-local declarations.
    std::vector<Ref> local_types;
    // Set for a subroutine reached through a VIRTUAL INTERFACE handle (LRM 25.9):
    // formal 0 is the handle, and the whole body addresses signals through the
    // instance it names rather than the one the body was lowered against.
    // `ctx_member` is any signal of that interface -- it names the definition whose
    // layout the instance record must describe for the substitution to be sound.
    bool   takes_iface_ctx = false;
    Signal ctx_member{ 0 };
    // True for a task: lowered as a coroutine that may suspend on timing controls.
    bool is_task = false;
    // True for a synthetic fork-child body (LRM 9.3.2): a coroutine spawned at a
    // `fork`, whose completion epilogue decrements a shared join record and wakes
    // the parent when the count reaches the record's wake_at (rather than a task's
    // unconditional single-parent resume). Its only formals are the enclosing frame
    // locals the fork captured by reference (ForkInfo::args), so a child of a fork
    // in a process body is parameterless. Implies is_task.
    bool is_fork_child = false;
    // The `disable` target id naming this task (LRM 9.6.2), or kNoDisableTarget.
    // A targeted task links each activation onto the target's list at entry and
    // unlinks in the epilogue, so a `disable` can end EVERY activation, which is
    // what 9.6.2 requires and what a single occupant slot cannot express.
    uint32_t disable_target = kNoDisableTarget;
    // Join discipline of the fork site that owns this child; only meaningful with
    // is_fork_child, and identical for every child of a site. The child's epilogue
    // needs it because the record's OWNERSHIP differs: under `join` the parent
    // outlives every child, so the record is a slot in the parent's frame and no
    // child ever frees it; the other two kinds let a child outlive the parent, so
    // the record is heap-allocated and reference-counted and the last one out frees
    // it (lower_fork_inst).
    ForkKind fork_kind = ForkKind::Join;
    // A DPI-C import (LRM 35): the body is native C resolved at JIT link time by
    // extern_name, not lowered from IR. The backend emits an external declaration
    // (dpi_arg_ctypes / dpi_ret_ctype give the native C signature) and marshals
    // each scalar argument at the call site (see lower_dpi_call). `ir` is a
    // trivial void return, never lowered.
    bool is_dpi = false;
    // A `pure` DPI import (LRM 35.6): side-effect-free and value-only, so the
    // backend marks its declaration readnone + willreturn for the optimizer.
    bool is_pure = false;
    // C linkage name of a DPI import (the `c_identifier`, or the SV name when the
    // import omits it — LRM 35.5.4). Empty for a non-DPI function.
    std::string extern_name;
    // DPI C scalar type of each parameter (parallel to param_types) and the
    // return. Only populated when is_dpi.
    std::vector<DpiCType> dpi_arg_ctypes;
    DpiCType              dpi_ret_ctype = DpiCType::Void;
    // For each OpenArray argument, the element's own DpiCType (Byte/Int/BitVec/
    // LogicVec/...); DpiCType::Void for a non-OpenArray argument. Parallel to
    // dpi_arg_ctypes; only populated when is_dpi.
    std::vector<DpiCType> dpi_arg_elem_ctypes;
    // Full source extent of the subroutine declaration. Empty for a
    // compiler-synthesized body (a $strobe/$monitor thunk, a fork child).
    SourceSpan span;
    // The lowered body.
    Ir ir;
};

// An `export "DPI-C"` directive (LRM 35.5): a SystemVerilog function made
// callable from native C. The backend emits a C-ABI thunk named `extern_name`
// that marshals C arguments to the SV function's plane ABI, calls it against the
// design-state global, and marshals the result back. `func` is the lowered SV
// function (an ordinary IR Function).
struct DpiExport {
    FunctionId            func;
    std::string           extern_name;   // C linkage name (the c_identifier)
    std::vector<DpiCType> arg_ctypes;    // parallel to the function's formals
    std::vector<ArgDir>   arg_dirs;      // direction of each formal (In / Out / Inout)
    DpiCType              ret_ctype;
};

// Deepest inheritance chain a class may have (LRM 8.13 puts no limit on it).
// Every class descriptor carries a fixed-width ancestor display, so the depth of
// the destination class is a constant offset at a `$cast` site; the width has to
// be one design-wide constant for that offset to be independent of the object's
// own class. Exceeding it is a loud frontend failure.
inline constexpr uint32_t kMaxClassDepth = 16;

// A class's runtime descriptor: its virtual-dispatch table (LRM 8.20) plus the
// type identity a dynamic `$cast` tests (LRM 8.16). Every class has one, so an
// object body always carries a descriptor pointer at field 0.
struct ClassDesc {
    Ref class_ref;
    // slots[k] is the FunctionId of the override occupying vtable slot k for this
    // concrete class.
    std::vector<FunctionId> slots;
    // Design-wide unique id of this class, never 0. Ids are unique and nothing
    // more -- no order, no density -- so minting one needs no knowledge of any
    // class but this one. See docs/design/classes.md.
    uint32_t type_id;
};

class Module {
public:
    // Shared intern pool backing every process's constant / type handles. Owned
    // here so all processes outlive their pool reference.
    InternPool pool;
    // DPI-C exports (LRM 35.5): SV functions exposed as C-callable thunks.
    std::vector<DpiExport> dpi_exports;
    // Compiled concurrent assertions (LRM 16). Indexed by the monitor id a
    // process body refers to; see AssertionMonitor.
    std::vector<AssertionMonitor> monitors;
    // The first top instance's name (LRM 35.5.3). Every top gets its own DPI
    // svScope, registered from `tops()`; this is only the fallback name for a
    // design with no top instances at all, and the default `name` for a multi-top
    // module.
    std::string top_name;
    // Module name.
    std::string name;
    // Femtoseconds per wheel tick (the design's finest time unit). Used by %t to
    // convert a tick-valued time argument to real time. 1 when unitless.
    uint64_t tick_fs = 1;
    // InstanceScope ids of the interface instances a virtual interface handle can
    // name (LRM 25.9). Every instance of the same definition must then present one
    // runtime context record and one canonical signal shape, because an access
    // through a handle is emitted once for the definition. Empty when the design
    // declares no virtual interface.
    std::vector<uint32_t> vif_scopes;
    // Default %t time-unit exponent (10^n seconds): the smallest time precision
    // across all design timescales (LRM Table 20-3). Seeds the runtime's
    // $timeformat default at startup.
    int32_t timeformat_units = 0;

    // Allot the next design-unique InstanceScope id (frontend, at node creation).
    uint32_t alloc_scope_id() {
        return next_scope_id_++;
    }
    // Which `disable` target ids name a TASK rather than a named block. A task
    // target is aborted through its activation list, a block target through its
    // single occupant slot, and the id alone does not say which.
    std::vector<uint8_t> disable_target_is_task;
    // For a target that IS its procedure's outermost scope and whose procedure holds
    // deferred assertions: that procedure's process index, so a REMOTE `disable` can
    // bump the target's generation counter rather than its own (LRM 16.4.4). Filled
    // as processes are created; `no_flush_proc` where the rule does not apply.
    static constexpr uint32_t no_flush_proc = ~0u;
    std::vector<uint32_t>     disable_flush_proc;

    // Number of `disable` target ids allocated (LRM 9.6.2). One id per targeted
    // named block per elaborated instance, so this is the length of the backend's
    // per-target occupant tables. Zero for a design with no `disable`.
    uint32_t disable_targets = 0;

    // Append a signal; the returned Signal handle is its index in `signals`.
    Signal add_signal(SignalInfo info) {
        const auto index = static_cast<uint32_t>(signals_.size());
        signals_.push_back(std::move(info));

        return static_cast<Signal>(index);
    }

    // Append a process; its index in `processes` is its identity.
    void add_process(Process proc) {
        processes_.push_back(std::move(proc));
    }

    // Reserve a function slot and return its id before the body is built, so a
    // recursive call can resolve the id while the body is still being lowered. The
    // slot must be filled with set_function before the module is used.
    FunctionId reserve_function() {
        const auto index = static_cast<uint32_t>(functions_.size());
        functions_.emplace_back();

        return static_cast<FunctionId>(index);
    }

    // Fill a previously reserved slot with the built function.
    void set_function(FunctionId id, Function fn) {
        functions_[static_cast<uint32_t>(id)] = std::move(fn);
    }

    const Function& function(FunctionId id) const {
        return functions_[static_cast<uint32_t>(id)];
    }

    size_t function_count() const {
        return functions_.size();
    }

    const SignalInfo& signal(Signal s) const {
        return signals_[static_cast<uint32_t>(s)];
    }

    std::span<const SignalInfo> signals() const {
        return signals_;
    }

    std::span<const Process> processes() const {
        return processes_;
    }

    // Append one top instance's completed scope tree (one per top-level module).
    void add_top(InstanceScope scope) {
        tops_.push_back(std::move(scope));
    }

    // The design hierarchy, one tree per top-level module. Empty for a module
    // built without hierarchy info; the backend then treats every signal as
    // belonging to one flat root scope.
    std::span<const InstanceScope> tops() const {
        return tops_;
    }

    // Register a class's runtime descriptor. Every class has one: the slots drive
    // virtual dispatch (LRM 8.20) and `type_id` plus the base chain give the
    // ancestor display a dynamic `$cast` tests (LRM 8.16).
    void set_class_desc(Ref class_ref, std::vector<FunctionId> slots, uint32_t type_id) {
        class_descs_.push_back({ class_ref, std::move(slots), type_id });
    }

    // The descriptor of a class type, or null when the type has not been lowered
    // as a class (every lowered class registers one).
    const ClassDesc* class_desc(Ref class_ref) const {
        for (const ClassDesc& cd : class_descs_) {
            if (cd.class_ref == class_ref) {
                return &cd;
            }
        }
        return nullptr;
    }

    // Every registered class descriptor. Whole-design analyses need the slot
    // layout to reason about virtual dispatch, which can reach any function
    // occupying a slot.
    [[nodiscard]] std::span<const ClassDesc> class_descs() const {
        return class_descs_;
    }

private:
    // Signal table; index == Signal handle.
    std::vector<SignalInfo> signals_;
    // Lowered processes, in creation order.
    std::vector<Process> processes_;
    // Lowered functions; index == FunctionId handle. A deque so a slot can be
    // reserved default-constructed (recursion) and assigned later, with
    // references handed out by function() staying stable as the table grows.
    std::deque<Function> functions_;
    // Instance hierarchy, one scope tree per top-level module.
    std::vector<InstanceScope> tops_;
    // Class descriptors, one per lowered class.
    std::vector<ClassDesc> class_descs_;
    // Next InstanceScope::id (see alloc_scope_id).
    uint32_t next_scope_id_ = 0;
};

} // namespace clotho::ir
