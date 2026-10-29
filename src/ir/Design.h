#pragma once

// Design: the frontend's modular output. A design is a set of units -- one per
// specialization (a module, interface or program body with its parameter values,
// as slang's canonical body identifies it), package, `$unit` and class
// specialization -- composed by child slots and roots. A unit is compiled once
// however many times it is instantiated; an instance is data (its name and its
// place in the tree), never code. See docs/design/modular-compilation.md.
//
// A unit's bodies use the same IR as a Module's, but every handle in them
// indexes the unit's own tables. A handle whose entry carries an import is an
// extern declaration: its SignalInfo / Function gives the expected shape, and the
// import names, by unit-relative name, where the definition lives. Imports never
// name another unit's local handles, so a unit's IR does not depend on how any
// other unit was numbered or lowered.
//
// flatten() expands a Design into the flat Module the backend consumes today:
// one copy of each unit per instance, with extern declarations resolved to the
// definitions they import. It is the bridge until the backend compiles units
// directly.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "InternPool.h"
#include "Module.h"

namespace clotho::ir {

// Index of a unit in Design::units.
enum class UnitId : uint32_t {};

// UnitSignal::import / UnitFunction::import for an entity the unit defines.
inline constexpr uint32_t kLocal = UINT32_MAX;

enum class UnitKind : uint8_t {
    // A module, interface or program body. Instantiated by child slots and
    // roots; each instance becomes an InstanceScope of the flat module.
    Module,
    // A package, `$unit` or class unit. Instantiated exactly once, by a root,
    // and scoped to no instance: its signals are unscoped flat signals, which is
    // how the backend lays out package state.
    Static,
};

// An instance, named relative to the instance that refers to it: a base, then a
// path of child slots descended from it.
struct InstanceRef {
    enum class Base : uint8_t {
        // The referring instance's ancestor `index` levels up; 0 is the referring
        // instance itself.
        Up,
        // The instance of Design::roots[index].
        Root,
        // The instance bound to the referring unit's interface port
        // Unit::bound_ports[index].
        Port,
    };

    Base base = Base::Up;
    // Levels (Up), root index (Root) or bound-port index (Port).
    uint32_t index = 0;
    // Child slots (Unit::children indices) descended from the base, in order.
    std::vector<uint32_t> slots;

    bool operator==(const InstanceRef&) const = default;
};

// Where an extern declaration's definition lives.
struct Import {
    enum class Kind : uint8_t {
        // Whatever the instantiating parent binds to the unit's bound port
        // `port`: an inout, ref or merged-nettype port aliases the parent's
        // signal (LRM 23.3.3).
        Port,
        // The member named `member` of the instance `inst` reaches: a child's
        // port a port copy reads or writes, a hierarchical reference, a package
        // item, a hierarchical task call.
        Member,
        // The member named `member` of any instance of `unit`. Supplies layout
        // only: a member reached through a virtual interface handle takes its
        // instance from the handle (LRM 25.9, docs/design/interfaces.md).
        Representative,
    };

    Kind kind = Kind::Member;
    // Kind::Port: index into Unit::bound_ports.
    uint32_t port = 0;
    // Kind::Member.
    InstanceRef inst;
    // Kind::Representative.
    UnitId unit{};
    // Kind::Member and Kind::Representative: the target's unit-relative name,
    // as the target unit's SignalInfo::name / UnitFunction::name spell it.
    std::string member;

    bool operator==(const Import&) const = default;
};

// A port of a unit that the instantiating parent binds rather than drives: the
// LRM 23.3.3 name-binding half of the port boundary. Value-flow ports (input and
// output) are ordinary local signals the parent's port-copy processes import.
struct BoundPort {
    enum class Kind : uint8_t {
        // An inout, ref or merged-nettype port; the parent binds a signal.
        Signal,
        // An interface port; the parent binds an interface instance.
        Instance,
    };

    std::string name;
    Kind        kind = Kind::Signal;

    bool operator==(const BoundPort&) const = default;
};

// An instance a unit's body declares.
struct ChildSlot {
    UnitId unit{};
    // Instance name relative to the declaring instance, generate-block prefix
    // included ("g[0].u"). The InstanceScope name of the child.
    std::string name;
    // One entry per child bound port, in the child's Unit::bound_ports order, in
    // the PARENT's handle space: a Signal for BoundPort::Kind::Signal, an index
    // into the parent's Unit::inst_refs for BoundPort::Kind::Instance. Bindings
    // belong to the parent unit, so every instance of the parent binds the same
    // way.
    std::vector<uint32_t> bindings;
    // How many of the parent's processes precede this instance in the body's
    // source order. flatten emits the child's subtree there, so the flat module
    // keeps the order the body declares its processes and instances in.
    // Non-decreasing across a unit's children.
    uint32_t process_pos = 0;
};

// A unit's signal table entry.
struct UnitSignal {
    SignalInfo info;
    // kLocal for a signal the unit defines; else the Unit::imports index of the
    // definition this extern declaration is bound to, and `info` is only its
    // expected shape.
    uint32_t import = kLocal;
    // Listed in its instance's InstanceScope: a declared variable, which the
    // VCD dump and the scope tree show. False for storage the compiler adds to
    // a unit without declaring it there, such as a constant table.
    bool scoped = true;

    bool operator==(const UnitSignal&) const = default;
};

// A unit's function table entry.
struct UnitFunction {
    Function fn;
    // The name other units import this function by: its declaration's path
    // relative to the unit ("C::new", "g[0].f"). Distinct from fn.name, which is
    // the subroutine's own name for diagnostics. Empty for a function nothing
    // outside the unit can name.
    std::string name;
    // kLocal for a function the unit defines; else the Unit::imports index of
    // the definition, and `fn` carries only the signature.
    uint32_t import = kLocal;

    bool operator==(const UnitFunction&) const = default;
};

// A unit's `disable` target table entry: a named block or task some `disable`
// names (LRM 9.6.2). A remote `disable` can name one in another instance, so an
// entry can be an extern declaration like a signal.
struct UnitDisableTarget {
    // The target is a task, whose every activation a `disable` ends.
    bool is_task = false;
    // The unit-local process whose deferred-assertion reports a `disable` of
    // the target flushes, or Module::no_flush_proc (Module::disable_flush_proc).
    uint32_t flush_proc = Module::no_flush_proc;
    // The name other units import the target by: its declaration's path
    // relative to the unit.
    std::string name;
    // kLocal for a target the unit declares; else the Unit::imports index of
    // the declaration.
    uint32_t import = kLocal;

    bool operator==(const UnitDisableTarget&) const = default;
};

struct Unit {
    UnitKind kind = UnitKind::Module;
    // Definition name (InstanceScope::def_name of every instance).
    std::string def_name;
    // Index == Signal handle within the unit.
    std::vector<UnitSignal> signals;
    // Index == FunctionId within the unit. A deque, like Module's function
    // table, so a slot can be reserved before its body is built.
    std::deque<UnitFunction> functions;
    // Process::scope is unused here: every process belongs to its unit's
    // instance, and flatten assigns the scope.
    std::vector<Process> processes;
    std::vector<Import>  imports;
    // Interface instances an IfaceRef names; IfaceInst::scope indexes this.
    std::vector<InstanceRef> inst_refs;
    std::vector<ChildSlot>   children;
    std::vector<BoundPort>   bound_ports;
    // Indexed by the unit-local monitor id (ExpectInfo, ExpectWake).
    std::vector<AssertionMonitor> monitors;
    // Indexed by the unit-local covergroup type index (CoverInfo::type_idx).
    // CoverType::scope is instance data: flatten sets it.
    std::vector<CoverType> cover_types;
    // Indexed by the unit-local `disable` target id (LRM 9.6.2).
    std::vector<UnitDisableTarget> disable_targets;
    // Class descriptors, FunctionIds unit-local.
    std::vector<ClassDesc> class_descs;
    // DPI-C exports, FunctionIds unit-local.
    std::vector<DpiExport> dpi_exports;
};

// A tree root: a top-level instance, or the one instance of a Static unit.
struct Root {
    UnitId unit{};
    // InstanceScope name of a top; for a Static unit, only diagnostics use it.
    std::string name;
    // Prefix of the flat signal names of the root's instance ("top." for a top);
    // a descendant's prefix is its parent's plus its slot name and a dot.
    std::string prefix;
};

// A Function in a particular unit, for the few design-level references.
struct UnitFunctionRef {
    UnitId     unit{};
    FunctionId fn{};
};

class Design {
public:
    // Intern pool shared by every unit's bodies. Heap-held so flatten can hand
    // it to the Module without moving it: built IR refers to it by address.
    std::unique_ptr<InternPool> pool = std::make_unique<InternPool>();
    // A deque: a Unit is not nothrow-movable (its function deque), and a
    // reference to one must survive adding another while lowering.
    std::deque<Unit> units;
    // Static-unit roots first, then tops, in the order the flat module numbers
    // them.
    std::vector<Root> roots;

    // Design-level settings and tables the flat Module carries verbatim (see the
    // fields of the same names there).
    std::string                           name;
    std::string                           top_name;
    uint64_t                              tick_fs          = 1;
    int32_t                               timeformat_units = 0;
    uint32_t                              iface_count      = 0;
    uint32_t                              iface_slot_count = 0;
    std::optional<UnitFunctionRef>        iface_unreachable;
    std::vector<std::pair<Ref, uint32_t>> iface_type_ids;
    std::vector<Ref>                      process_classes;
    std::vector<ChangeNotify>             change_notifies;
    // Units whose every instance a virtual interface handle can name; flatten
    // turns them into Module::vif_scopes.
    std::vector<UnitId> vif_units;

    Unit& unit(UnitId id) {
        return units[static_cast<uint32_t>(id)];
    }
    const Unit& unit(UnitId id) const {
        return units[static_cast<uint32_t>(id)];
    }
};

// Every field of `f` but its body, which a caller rebases separately: a
// Function is move-only because its body is.
Function signature_of(const Function& f);

// Expand `design` into the flat Module: every unit copied once per instance,
// with each extern declaration resolved to the definition its import names. The
// Module takes over the design's pool. Instances are numbered depth-first in
// root order, child slots in declaration order.
std::unique_ptr<Module> flatten(Design&& design);

} // namespace clotho::ir
