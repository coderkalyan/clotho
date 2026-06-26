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
#include <memory>
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

// A lowered SystemVerilog function: a real callable, not inlined. Its body is an
// Ir whose leaves are Arg (parameters), Local (frame storage), and Signal (module
// signals it references); it ends every path in a Ret. The backend emits one LLVM
// function per Function, called at each `call` site, so recursion and `automatic`
// lifetime fall out of normal stack frames.
struct Function {
    // Function name (for the emitted symbol and diagnostics).
    std::string name;
    // Return type; `pool.void_type` for a void function (no return value).
    Ref return_type = Ref::Invalid;
    // Parameter types, in declaration order (inputs only for now). The frontend
    // resizes each call argument to the matching type.
    std::vector<Ref> param_types;
    // Storage type of every frame local, indexed by the Local handle's index:
    // the parameter copies, the return variable, and body-local declarations.
    std::vector<Ref> local_types;
    // The lowered body.
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

    // Reserve a function slot and return its id before the body is built, so a
    // recursive call can resolve the id while the body is still being lowered. The
    // slot must be filled with set_function before the module is used.
    FunctionId reserve_function() {
        const auto index = static_cast<uint32_t>(functions_.size());
        functions_.push_back(nullptr);

        return static_cast<FunctionId>(index);
    }

    // Fill a previously reserved slot with the built function.
    void set_function(FunctionId id, Function fn) {
        functions_[static_cast<uint32_t>(id)] = std::make_unique<Function>(std::move(fn));
    }

    const Function& function(FunctionId id) const {
        return *functions_[static_cast<uint32_t>(id)];
    }

    size_t function_count() const {
        return functions_.size();
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
    // Lowered functions; index == FunctionId handle. Held behind unique_ptr so a
    // slot can be reserved (recursion) before its body exists.
    std::vector<std::unique_ptr<Function>> functions_;
};

} // namespace clotho::ir
