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

// Whether a process settles combinationally or updates on a clock edge.
enum class ProcessKind : uint8_t {
    Comb,
    Sequential,
};

// Active edge of a sequential process's clock.
enum class Edge : uint8_t {
    Posedge,
    Negedge,
};

// One lowered process plus the metadata the IR body itself does not carry
// (kind, and for sequential processes the clock signal and its active edge).
struct Process {
    // Combinational or sequential.
    ProcessKind kind = ProcessKind::Comb;
    // Clock signal; meaningful only when kind == Sequential.
    Signal clock = Signal{ 0 };
    // Active clock edge; meaningful only when kind == Sequential.
    Edge clock_edge = Edge::Posedge;
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
