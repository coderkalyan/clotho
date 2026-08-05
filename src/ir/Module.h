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
    // Runs once at time zero, then completes (an `initial` block).
    Initial,
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

// What to do with a verdict. Mirrors libsva's sva_kind, checked at the boundary.
enum class AssertionKind : uint8_t { Assert, Assume, Cover, Restrict };

// What a still-pending attempt does when the trace ends (LRM 16.12.2).
enum class AssertionStrength : uint8_t { Weak, Strong };

// One conjunct of a guard: an index into AssertionMonitor::roots, possibly
// complemented. Clotho's own lowering never sets `negated` -- SVA booleans are
// three-valued, so negation lives in the atom (concurrent-assertions.md 5.3) --
// but the field is what libsva reports, so it is carried faithfully.
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
    // cubes.size() * words: per guard, the mask of states carrying it. Lets the
    // satisfied-state mask fold over guards instead of gathering per state.
    std::vector<uint64_t> guard_states;
};

struct AssertionMonitor {
    // The directive's source-level label, for a report. Empty when unlabelled.
    std::string label;
    AssertionKind kind = AssertionKind::Assert;
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
    // the number of state vectors the backend allocates.
    uint32_t depth = 1;

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
    // True for a task: lowered as a coroutine that may suspend on timing controls.
    bool is_task = false;
    // True for a synthetic fork-child body (LRM 9.3.2): a parameterless coroutine
    // spawned at a `fork`, whose completion epilogue decrements a shared join
    // record and wakes the parent when the count reaches zero (rather than a
    // task's unconditional single-parent resume). Implies is_task.
    bool is_fork_child = false;
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

// A polymorphic class's virtual-dispatch table (LRM 8.20): slots[k] is the
// FunctionId of the override occupying vtable slot k for this concrete class.
struct Vtable {
    Ref                     class_ref;
    std::vector<FunctionId> slots;
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
    // The top instance's name, used as the DPI svScope name (LRM 35.5.3). For a
    // multi-top design it is the first top's name (multi-top DPI export is
    // guarded; a multi-top context import's scope is an edge case).
    std::string top_name;
    // Module name.
    std::string name;
    // Femtoseconds per wheel tick (the design's finest time unit). Used by %t to
    // convert a tick-valued time argument to real time. 1 when unitless.
    uint64_t tick_fs = 1;
    // Default %t time-unit exponent (10^n seconds): the smallest time precision
    // across all design timescales (LRM Table 20-3). Seeds the runtime's
    // $timeformat default at startup.
    int32_t timeformat_units = 0;

    // Allot the next design-unique InstanceScope id (frontend, at node creation).
    uint32_t alloc_scope_id() {
        return next_scope_id_++;
    }

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

    // Register the virtual-dispatch table of a polymorphic class (LRM 8.20):
    // slots[k] is the FunctionId of the override occupying vtable slot k for this
    // concrete class. Only polymorphic classes (those with a virtual method) get an
    // entry; a class with no entry carries no vtable pointer in its object body.
    void set_vtable(Ref class_ref, std::vector<FunctionId> slots) {
        vtables_.push_back({ class_ref, std::move(slots) });
    }

    // The vtable for a class type, or null if the class is non-polymorphic.
    const Vtable* vtable_for(Ref class_ref) const {
        for (const Vtable& vt : vtables_) {
            if (vt.class_ref == class_ref) {
                return &vt;
            }
        }
        return nullptr;
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
    // Virtual-dispatch tables, one per polymorphic class (LRM 8.20).
    std::vector<Vtable> vtables_;
    // Next InstanceScope::id (see alloc_scope_id).
    uint32_t next_scope_id_ = 0;
};

} // namespace clotho::ir
