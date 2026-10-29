#pragma once

// partition: the frontend's bridge from the flat Module it lowers into to the
// Design of units (Design.h). Every elaborated instance becomes a unit of its
// own, every package and `$unit` a static unit, and whatever belongs to no
// single instance -- a per-top initializer pass, a resolver of a net driven
// from several scopes -- a static "glue" unit of the design. Every body is
// rebased onto its unit's tables, and every reference across units becomes an
// extern declaration importing the target by name. flatten(partition(m)) is
// the module again, up to numbering and process order within a body.
//
// One unit per instance is the frontend's starting point, not the goal:
// docs/design/modular-compilation.md, step 2.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Design.h"
#include "Module.h"

namespace clotho::ir {

// The unit a flat entity belongs to, as the frontend records it.
struct Owner {
    enum class Kind : uint8_t {
        // Not recorded: partition attributes it from the bodies that use it.
        Unknown,
        // The design's glue unit.
        Glue,
        // The unit of the instance whose InstanceScope::id is `index`.
        Scope,
        // The static unit Attribution::statics[index].
        Static,
    };

    Kind     kind  = Kind::Unknown;
    uint32_t index = 0;
};

// What the flat module cannot say about itself: which unit owns the entities no
// InstanceScope lists, and the names other units import them by.
struct Attribution {
    // Definition names of the static units (packages, `$unit`), in root order.
    std::vector<std::string> statics;
    // Owner of each flat signal no InstanceScope lists, indexed by Signal; a
    // signal past the end, or Unknown, belongs to the glue unit. A signal an
    // InstanceScope lists belongs to that scope's unit regardless.
    std::vector<Owner> signals;
    // Owner of each flat process that no InstanceScope claims
    // (Process::no_scope), indexed by process; Unknown belongs to the glue unit.
    std::vector<Owner> processes;
    // Owner and import name (UnitFunction::name) of each flat function, indexed
    // by FunctionId. An Unknown function belongs to the unit of the first body
    // that refers to it, or to the glue unit when none does.
    std::vector<Owner>       functions;
    std::vector<std::string> function_names;
    // Import name of each flat `disable` target, indexed by target id: the
    // path of the named block or task relative to its unit.
    std::vector<std::string> disable_target_names;
    // Specialization of each instance, indexed by InstanceScope::id: instances
    // with one specialization elaborate the same body (slang's canonical body).
    std::vector<uint32_t> scope_specs;
    // Ports that alias the signal the parent connects rather than copying it
    // (inout, ref and event ports, LRM 23.3.3): the child declares no storage
    // for one, and its body names the parent's signal. partition turns those
    // references into a bound port of the child's unit.
    struct PortAlias {
        // InstanceScope::id of the child.
        uint32_t scope = 0;
        // The port's name in the child.
        std::string port;
        // The flat signal it aliases.
        Signal target{};
    };
    std::vector<PortAlias> port_aliases;
    // A driver's store through an aliasing port (LRM 23.3.3, 6.6.1): the driving
    // instance names a contribution of its own, a bound port its parent binds,
    // so its body is the same whatever the net's other drivers are. The parent
    // binds it to the net itself when this is the net's only driver, to a driver
    // slot of the net's owner when the net resolves, and, when the parent
    // aliases the net through a port of its own, to a contribution of the
    // parent's that re-exports this one.
    struct Contribution {
        // InstanceScope::id of the instance whose body names the contribution.
        uint32_t scope = 0;
        // The bound port's name in that instance ("io$drv0").
        std::string name;
        // The scoped flat signal of `scope` the body stores through. partition
        // makes it an extern declaration of the bound port.
        Signal signal{};
        // The flat signal the parent binds the port to.
        Signal target{};
    };
    std::vector<Contribution> contributions;
    // Interface ports bound to one interface instance (LRM 25.3): the child's
    // body names that instance's members, which partition reaches through a
    // bound port of the child's unit.
    struct IfacePort {
        // InstanceScope::id of the child.
        uint32_t scope = 0;
        // The port's name in the child.
        std::string port;
        // InstanceScope::id of the interface instance it is bound to.
        uint32_t target = 0;
    };
    std::vector<IfacePort> iface_ports;
};

// Split `module` into units. Takes the module's pool; the module must not be
// used afterwards except to be destroyed. When `unit_specs` is set it receives
// the specialization of each instance unit (UINT32_MAX for the others).
std::unique_ptr<Design> partition(Module&                module,
                                  const Attribution&     attribution,
                                  std::vector<uint32_t>* unit_specs = nullptr);

// How far the instances of each specialization are from sharing one unit: for
// every specialization with more than one instance, whether their units'
// printed IR is identical, and the first difference of a few that are not.
std::string describe_sharing(const Design& design, std::span<const uint32_t> unit_specs);

} // namespace clotho::ir
