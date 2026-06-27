#pragma once

// Module: the container that owns a design's processes and the signals they
// reference. The process IR (Ir.h) models one process and refers to signals by
// opaque `Signal` handle; this is the enclosing object that resolves those
// handles to names / types and holds the lowered processes together.
//
// Ownership: a Module owns the InternPool shared by every process it holds, so
// each Process's `Ir.pool` reference stays valid for the module's lifetime. The
// pool (and thus the Module) is non-movable, so a Module is the owning root,
// typically held behind a unique_ptr by the front-end.

#include <cstdint>
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
    // Interned type in the module's pool.
    Ref type = Ref::Invalid;
    // Direction relative to the module boundary.
    Direction dir = Direction::Internal;
};

// Handle to a process: an index into Module::processes.
enum class ProcessId : uint32_t {};

// How a process is scheduled. The fine-grained trigger condition lives in the
// process's wait list (Process::wait_events); this only separates one-shot
// startup processes from those that re-arm and rerun whenever they are woken.
enum class Activation : uint8_t {
    // Runs once at time zero, then completes (an `initial` block).
    Initial,
    // Re-armed after each run and rerun whenever one of its wait events fires
    // (an `always` block or a continuous `assign`).
    Always,
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
    // Location of the AST construct that produced this process (the continuous
    // assign, always block, ...).
    SourceLoc loc = SourceLoc{ 0 };
    // The lowered process body.
    Ir ir;
};

class Module {
public:
    // Shared intern pool backing every process's constant / type handles. Owned
    // here so all processes outlive their pool reference.
    InternPool pool;
    // Module name.
    std::string name;
    // Femtoseconds per wheel tick (the design's finest time unit). Used by %t to
    // convert a tick-valued time argument to real time. 1 when unitless.
    uint64_t tick_fs = 1;
    // Default %t time-unit exponent (10^n seconds): the smallest time precision
    // across all design timescales (LRM Table 20-3). Seeds the runtime's
    // $timeformat default at startup.
    int32_t timeformat_units = 0;

    // Append a signal; the returned Signal handle is its index in `signals`.
    Signal add_signal(SignalInfo info) {
        const auto index = static_cast<uint32_t>(signals_.size());
        signals_.push_back(std::move(info));

        return static_cast<Signal>(index);
    }

    // Append a process; the returned ProcessId is its index in `processes`.
    ProcessId add_process(Process proc) {
        const auto index = static_cast<uint32_t>(processes_.size());
        processes_.push_back(std::move(proc));

        return static_cast<ProcessId>(index);
    }

    const SignalInfo& signal(Signal s) const {
        return signals_[static_cast<uint32_t>(s)];
    }

    const Process& process(ProcessId id) const {
        return processes_[static_cast<uint32_t>(id)];
    }

    std::span<const SignalInfo> signals() const {
        return signals_;
    }

    std::span<const Process> processes() const {
        return processes_;
    }

private:
    // Signal table; index == Signal handle.
    std::vector<SignalInfo> signals_;
    // Lowered processes; index == ProcessId handle.
    std::vector<Process> processes_;
};

} // namespace clotho::ir
