#include "Design.h"

#include <cassert>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <utility>

#include <ankerl/unordered_dense.h>

#include "Printer.h"

namespace clotho::ir {

namespace {

constexpr uint32_t kNone = UINT32_MAX;

// A unit-local signal or function handle whose extern declaration is not yet
// resolved to a flat one.
constexpr auto kUnresolvedSignal   = static_cast<Signal>(UINT32_MAX);
constexpr auto kUnresolvedFunction = static_cast<FunctionId>(UINT32_MAX);
// A name two entities of one unit share: an import naming it is ambiguous.
constexpr uint32_t kAmbiguous = UINT32_MAX - 1;

struct FlatInstance {
    UnitId unit{};
    // Parent instance and the parent's ChildSlot index this instance fills;
    // kNone for a root.
    uint32_t parent = kNone;
    uint32_t slot   = kNone;
    // InstanceScope name: the root's name or the slot's.
    std::string name;
    // Flat signal name prefix.
    std::string prefix;
    // InstanceScope id, or Process::no_scope for a Static unit's instance.
    uint32_t scope = Process::no_scope;
    // Instance filling each of the unit's child slots.
    std::vector<uint32_t> children;
    // Unit-local handle -> flat handle.
    std::vector<Signal>     signals;
    std::vector<FunctionId> functions;
    // Unit-local process index -> flat process index, filled as they emit.
    std::vector<uint32_t> processes;
    // Unit-local `disable` target -> flat target id.
    std::vector<uint32_t> targets;
    // Unit-local covergroup type -> flat index into Module::cover_types.
    std::vector<uint32_t> cover_types;
    // The instance's own monitors, flat: [monitor_base, monitor_base + monitor_count).
    uint32_t monitor_base  = 0;
    uint32_t monitor_count = 0;
};

// Each unit's key (InstanceScope::unit_key), which names its LLVM module and the
// symbols of its code. A definition that is one unit is named by its definition
// name alone, so what calls into it (a hierarchical or package call names the
// callee's symbol) does not change when its body is edited. Units that share a
// definition name (parameterizations, a class specialization) add a hash of their
// printed body, children by definition name, and units that still print alike
// without being merged (the same body over different subtrees) take a count, in
// unit order, so keys stay unique.
std::vector<std::string> name_units(const Design& design) {
    std::vector<std::string>                            keys(design.units.size());
    ankerl::unordered_dense::map<std::string, uint32_t> by_name;
    for (const Unit& u : design.units) {
        ++by_name[u.def_name];
    }
    ankerl::unordered_dense::map<std::string, uint32_t> seen;
    for (uint32_t u = 0; u < design.units.size(); ++u) {
        std::string key = design.units[u].def_name;
        if (by_name[key] > 1) {
            std::ostringstream os;
            print_unit(design, UnitId{ u }, os);
            uint64_t h = 1469598103934665603ull; // FNV-1a
            for (const char c : os.str()) {
                h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
            }
            static constexpr char digits[] = "0123456789abcdef";
            key += ".";
            for (int shift = 28; shift >= 0; shift -= 4) {
                key += digits[((h ^ (h >> 32)) >> shift) & 0xf];
            }
        }
        const uint32_t n = seen[key]++;
        keys[u]          = n == 0 ? key : key + "." + std::to_string(n);
    }
    return keys;
}

class Flattener {
public:
    Flattener(Design& design, Module& module, std::vector<std::string> unit_keys)
      : d_(design)
      , m_(module)
      , unit_keys_(std::move(unit_keys)) {}

    void run() {
        expand();
        allocate();
        resolve();
        emit();
        build_scopes();
        emit_design_tables();
    }

private:
    // Create every instance, depth-first in root order with child slots in
    // declaration order, so scope ids come out in the pre-order the backend
    // has always seen. A worklist, not recursion (docs/PRINCIPLES.md).
    void expand() {
        root_inst_.reserve(d_.roots.size());
        instances_of_.resize(d_.units.size());
        for (const Root& r : d_.roots) {
            const uint32_t ri = add_instance(r.unit, kNone, kNone, r.name, r.prefix);
            root_inst_.push_back(ri);

            // (instance, next slot to expand)
            std::vector<std::pair<uint32_t, uint32_t>> stack{ { ri, 0 } };
            while (!stack.empty()) {
                auto& [ii, next] = stack.back();
                const Unit& u    = d_.unit(insts_[ii].unit);
                if (next == u.children.size()) {
                    stack.pop_back();
                    continue;
                }
                const uint32_t   slot   = next++;
                const uint32_t   parent = ii;
                const ChildSlot& cs     = u.children[slot];
                assert(d_.unit(cs.unit).kind == UnitKind::Module && "only a module-like body is instantiated");
                assert(cs.bindings.size() == d_.unit(cs.unit).bound_ports.size());
                const uint32_t ci = add_instance(cs.unit, parent, slot, cs.name, insts_[parent].prefix + cs.name + ".");
                insts_[parent].children[slot] = ci;
                stack.emplace_back(ci, 0);
            }
        }
    }

    uint32_t add_instance(UnitId unit, uint32_t parent, uint32_t slot, std::string name, std::string prefix) {
        const Unit&    u  = d_.unit(unit);
        const uint32_t ii = static_cast<uint32_t>(insts_.size());
        assert(u.kind == UnitKind::Module || parent == kNone);
        FlatInstance fi;
        fi.unit   = unit;
        fi.parent = parent;
        fi.slot   = slot;
        fi.name   = std::move(name);
        fi.prefix = std::move(prefix);
        if (u.kind == UnitKind::Module) {
            fi.scope = m_.alloc_scope_id();
        }
        fi.children.assign(u.children.size(), kNone);
        insts_.push_back(std::move(fi));
        instances_of_[static_cast<uint32_t>(unit)].push_back(ii);
        return ii;
    }

    // Give every locally defined signal, function, `disable` target and covergroup
    // type of every instance its flat handle, in instance order then unit order.
    void allocate() {
        for (FlatInstance& fi : insts_) {
            const Unit& u = d_.unit(fi.unit);
            fi.signals.assign(u.signals.size(), kUnresolvedSignal);
            for (uint32_t h = 0; h < u.signals.size(); ++h) {
                if (u.signals[h].import != kLocal) {
                    continue;
                }
                SignalInfo info = u.signals[h].info;
                info.name       = fi.prefix + info.name;
                fi.signals[h]   = m_.add_signal(std::move(info));
            }
            fi.functions.assign(u.functions.size(), kUnresolvedFunction);
            for (uint32_t h = 0; h < u.functions.size(); ++h) {
                if (u.functions[h].import == kLocal) {
                    fi.functions[h] = m_.reserve_function(u.functions[h].fn.return_type);
                }
            }
            fi.targets.assign(u.disable_targets.size(), kNone);
            for (uint32_t h = 0; h < u.disable_targets.size(); ++h) {
                if (u.disable_targets[h].import == kLocal) {
                    fi.targets[h] = m_.disable_targets++;
                    m_.disable_target_is_task.push_back(u.disable_targets[h].is_task ? 1 : 0);
                }
            }
            fi.cover_types.assign(u.cover_types.size(), kNone);
            for (uint32_t h = 0; h < u.cover_types.size(); ++h) {
                if (u.cover_types[h].import == kLocal) {
                    fi.cover_types[h] = static_cast<uint32_t>(m_.cover_types.size());
                    m_.cover_types.push_back(u.cover_types[h].type);
                }
            }
            signal_entries_ += fi.signals.size();
            function_entries_ += fi.functions.size();
        }
    }

    // Bind every extern declaration to the definition its import names.
    void resolve() {
        for (uint32_t ii = 0; ii < insts_.size(); ++ii) {
            for (uint32_t h = 0; h < insts_[ii].signals.size(); ++h) {
                if (insts_[ii].signals[h] == kUnresolvedSignal) {
                    resolve_signal(ii, h);
                }
            }
            for (uint32_t h = 0; h < insts_[ii].functions.size(); ++h) {
                if (insts_[ii].functions[h] == kUnresolvedFunction) {
                    resolve_function(ii, h);
                }
            }
            // A target is declared where it lives, so an import names a local
            // target directly: no chain to follow.
            const Unit& u = d_.unit(insts_[ii].unit);
            for (uint32_t h = 0; h < u.disable_targets.size(); ++h) {
                const uint32_t imp = u.disable_targets[h].import;
                if (imp == kLocal) {
                    continue;
                }
                const Import& i = u.imports[imp];
                assert(i.kind == Import::Kind::Member && "a disable target is imported by member");
                const uint32_t t      = walk(ii, i.inst);
                const uint32_t local  = target_named(insts_[t].unit, i.member);
                insts_[ii].targets[h] = insts_[t].targets[local];
            }
            for (uint32_t h = 0; h < u.cover_types.size(); ++h) {
                const uint32_t imp = u.cover_types[h].import;
                if (imp == kLocal) {
                    continue;
                }
                const Import& i = u.imports[imp];
                assert(i.kind == Import::Kind::Member && "a covergroup type is imported by member");
                const uint32_t t          = walk(ii, i.inst);
                const uint32_t local      = cover_type_named(insts_[t].unit, i.member);
                insts_[ii].cover_types[h] = insts_[t].cover_types[local];
            }
        }
    }

    // Follow a chain of extern declarations (a port bound to a parent signal
    // that is itself a hierarchical reference, ...) to its definition, then
    // record the answer at every link. An acyclic chain visits each (instance,
    // handle) at most once, which bounds it by the number of signal entries.
    Signal resolve_signal(uint32_t ii, uint32_t h) {
        std::vector<std::pair<uint32_t, uint32_t>> chain;
        for (size_t step = 0; step <= signal_entries_; ++step) {
            const Signal flat = insts_[ii].signals[h];
            if (flat != kUnresolvedSignal) {
                for (const auto& [ci, ch] : chain) {
                    insts_[ci].signals[ch] = flat;
                }
                return flat;
            }
            chain.emplace_back(ii, h);
            const Unit&   u   = d_.unit(insts_[ii].unit);
            const Import& imp = u.imports[u.signals[h].import];
            switch (imp.kind) {
                case Import::Kind::Port: {
                    assert(imp.port < u.bound_ports.size() && u.bound_ports[imp.port].kind == BoundPort::Kind::Signal);
                    const uint32_t parent = insts_[ii].parent;
                    assert(parent != kNone && "a top-level instance has no parent to bind its port");
                    h  = d_.unit(insts_[parent].unit).children[insts_[ii].slot].bindings[imp.port];
                    ii = parent;
                    break;
                }
                case Import::Kind::Member:
                    ii = walk(ii, imp.inst);
                    h  = signal_named(insts_[ii].unit, imp.member);
                    break;
                case Import::Kind::Representative:
                    ii = representative(imp.unit);
                    h  = signal_named(imp.unit, imp.member);
                    break;
            }
        }
        assert(false && "extern signal declarations form a cycle");
        std::abort();
    }

    FunctionId resolve_function(uint32_t ii, uint32_t h) {
        std::vector<std::pair<uint32_t, uint32_t>> chain;
        for (size_t step = 0; step <= function_entries_; ++step) {
            const FunctionId flat = insts_[ii].functions[h];
            if (flat != kUnresolvedFunction) {
                for (const auto& [ci, ch] : chain) {
                    insts_[ci].functions[ch] = flat;
                }
                return flat;
            }
            chain.emplace_back(ii, h);
            const Unit&   u   = d_.unit(insts_[ii].unit);
            const Import& imp = u.imports[u.functions[h].import];
            switch (imp.kind) {
                case Import::Kind::Port:
                    assert(false && "a port binds a signal or an instance, never a function");
                    std::abort();
                case Import::Kind::Member:
                    ii = walk(ii, imp.inst);
                    h  = function_named(insts_[ii].unit, imp.member);
                    break;
                case Import::Kind::Representative:
                    ii = representative(imp.unit);
                    h  = function_named(imp.unit, imp.member);
                    break;
            }
        }
        assert(false && "extern function declarations form a cycle");
        std::abort();
    }

    uint32_t representative(UnitId unit) const {
        const auto& of = instances_of_[static_cast<uint32_t>(unit)];
        assert(!of.empty() && "a representative of a unit with no instance");
        return of.front();
    }

    // The instance `r` names, relative to instance `ii`. An interface-port base
    // is the parent's binding, itself relative to the parent, so the walk hops
    // up one level per binding and applies the deferred slot paths innermost
    // first once a base resolves.
    uint32_t walk(uint32_t ii, const InstanceRef& r) {
        std::vector<const std::vector<uint32_t>*> deferred;
        const InstanceRef*                        ref = &r;
        while (ref->base == InstanceRef::Base::Port) {
            assert(deferred.size() <= insts_.size() && "interface port bindings form a cycle");
            [[maybe_unused]] const Unit& u = d_.unit(insts_[ii].unit);
            assert(ref->index < u.bound_ports.size() && u.bound_ports[ref->index].kind == BoundPort::Kind::Instance);
            const uint32_t parent = insts_[ii].parent;
            assert(parent != kNone && "a top-level instance has no parent to bind its interface port");
            const Unit&    pu      = d_.unit(insts_[parent].unit);
            const uint32_t binding = pu.children[insts_[ii].slot].bindings[ref->index];
            assert(binding < pu.inst_refs.size());
            deferred.push_back(&ref->slots);
            ii  = parent;
            ref = &pu.inst_refs[binding];
        }
        if (ref->base == InstanceRef::Base::Up) {
            for (uint32_t n = 0; n < ref->index; ++n) {
                assert(insts_[ii].parent != kNone && "an upward reference above a root");
                ii = insts_[ii].parent;
            }
        } else {
            assert(ref->index < root_inst_.size());
            ii = root_inst_[ref->index];
        }
        ii = descend(ii, ref->slots);
        while (!deferred.empty()) {
            ii = descend(ii, *deferred.back());
            deferred.pop_back();
        }
        return ii;
    }

    uint32_t descend(uint32_t ii, const std::vector<uint32_t>& slots) const {
        for (const uint32_t s : slots) {
            assert(s < insts_[ii].children.size());
            ii = insts_[ii].children[s];
        }
        return ii;
    }

    // A unit's members, by name: what it defines, plus its ports the parent
    // binds (they are members of the unit's interface even though the storage
    // is the parent's). Extern declarations of other units' members are not
    // members of this one.
    uint32_t signal_named(UnitId unit, const std::string& name) {
        auto&      names = names_of(signal_names_, unit, [&](const Unit& u, NameMap& out) {
            for (uint32_t h = 0; h < u.signals.size(); ++h) {
                const uint32_t imp = u.signals[h].import;
                if (imp == kLocal || u.imports[imp].kind == Import::Kind::Port) {
                    note_name(out, u.signals[h].info.name, h);
                }
            }
        });
        return lookup(names, unit, name);
    }

    uint32_t function_named(UnitId unit, const std::string& name) {
        auto& names = names_of(function_names_, unit, [&](const Unit& u, NameMap& out) {
            for (uint32_t h = 0; h < u.functions.size(); ++h) {
                if (u.functions[h].import == kLocal && !u.functions[h].name.empty()) {
                    note_name(out, u.functions[h].name, h);
                }
            }
        });
        return lookup(names, unit, name);
    }

    uint32_t target_named(UnitId unit, const std::string& name) {
        auto& names = names_of(target_names_, unit, [&](const Unit& u, NameMap& out) {
            for (uint32_t h = 0; h < u.disable_targets.size(); ++h) {
                if (u.disable_targets[h].import == kLocal && !u.disable_targets[h].name.empty()) {
                    note_name(out, u.disable_targets[h].name, h);
                }
            }
        });
        return lookup(names, unit, name);
    }

    uint32_t cover_type_named(UnitId unit, const std::string& name) {
        auto& names = names_of(cover_type_names_, unit, [&](const Unit& u, NameMap& out) {
            for (uint32_t h = 0; h < u.cover_types.size(); ++h) {
                if (u.cover_types[h].import == kLocal && !u.cover_types[h].name.empty()) {
                    note_name(out, u.cover_types[h].name, h);
                }
            }
        });
        return lookup(names, unit, name);
    }

    // The member `name` of `unit`. A miss or an ambiguity is a frontend bug;
    // name it before stopping.
    uint32_t lookup(const auto& names, UnitId unit, const std::string& name) const {
        const auto it = names.find(name);
        if (it == names.end() || it->second == kAmbiguous) {
            std::fprintf(stderr,
                         "flatten: unit '%s' has %s member '%s'\n",
                         d_.unit(unit).def_name.c_str(),
                         it == names.end() ? "no" : "more than one",
                         name.c_str());
            assert(false && "an import names no single member of its target");
            std::abort();
        }
        return it->second;
    }

    using NameMap = ankerl::unordered_dense::map<std::string, uint32_t>;

    template<typename Fill>
    NameMap& names_of(std::vector<std::optional<NameMap>>& cache, UnitId unit, Fill&& fill) {
        cache.resize(d_.units.size());
        auto& slot = cache[static_cast<uint32_t>(unit)];
        if (!slot) {
            slot.emplace();
            fill(d_.unit(unit), *slot);
        }
        return *slot;
    }

    static void note_name(NameMap& out, const std::string& name, uint32_t h) {
        const auto [it, inserted] = out.try_emplace(name, h);
        if (!inserted) {
            it->second = kAmbiguous;
        }
    }

    // Maps one instance's unit-local handles onto the flat module: signals,
    // functions, covergroup types and `disable` targets through the instance's
    // tables, the per-unit monitor ids by the instance's base.
    struct FlatMapper final : HandleMapper {
        const FlatInstance*   fi = nullptr;
        std::vector<uint32_t> iface_scopes;
        uint32_t              monitor_base = 0;

        uint32_t map(HandleFamily family, uint32_t h) override {
            switch (family) {
                case HandleFamily::Signal:
                case HandleFamily::Layout:
                    assert(h < fi->signals.size());
                    return static_cast<uint32_t>(fi->signals[h]);
                case HandleFamily::Function:
                    assert(h < fi->functions.size());
                    return static_cast<uint32_t>(fi->functions[h]);
                case HandleFamily::IfaceScope:
                    assert(h < iface_scopes.size());
                    return iface_scopes[h];
                case HandleFamily::Monitor:
                    return monitor_base + h;
                case HandleFamily::CoverType:
                    assert(h < fi->cover_types.size());
                    return fi->cover_types[h];
                case HandleFamily::DisableTarget:
                    assert(h < fi->targets.size());
                    return fi->targets[h];
            }
            std::abort();
        }
    };

    // Copy each instance's functions and per-unit tables into the flat module in
    // instance order, then its processes in source order (emit_processes).
    void emit() {
        mappers_.resize(insts_.size());
        for (uint32_t ii = 0; ii < insts_.size(); ++ii) {
            const FlatInstance& fi = insts_[ii];
            const Unit&         u  = d_.unit(fi.unit);
            // Copying a class descriptor or a DPI export per instance would
            // register one class or one C symbol several times; lowering per
            // specialization has to settle both before a unit holding one is
            // instantiated twice.
            assert((u.class_descs.empty() && u.dpi_exports.empty()) ||
                   instances_of_[static_cast<uint32_t>(fi.unit)].size() == 1);

            FlatMapper& mapper = mappers_[ii];
            mapper.fi          = &fi;
            for (const InstanceRef& r : u.inst_refs) {
                const uint32_t target = walk(ii, r);
                assert(insts_[target].scope != Process::no_scope && "an interface reference names a static unit");
                mapper.iface_scopes.push_back(insts_[target].scope);
            }
            mapper.monitor_base      = static_cast<uint32_t>(m_.monitors.size());
            insts_[ii].monitor_base  = mapper.monitor_base;
            insts_[ii].monitor_count = static_cast<uint32_t>(u.monitors.size());

            for (uint32_t h = 0; h < u.functions.size(); ++h) {
                if (u.functions[h].import != kLocal) {
                    continue;
                }
                const Function& src = u.functions[h].fn;
                assert(src.scope == Process::no_scope && "a unit function carries no scope; flatten assigns it");
                Function copy = signature_of(src);
                copy.ir       = src.ir.rebased(mapper);
                copy.scope    = fi.scope;
                copy.unit_key = unit_keys_[static_cast<uint32_t>(fi.unit)];
                if (src.takes_iface_ctx) {
                    copy.ctx_member =
                      static_cast<Signal>(mapper.map(HandleFamily::Layout, static_cast<uint32_t>(src.ctx_member)));
                }
                if (src.disable_target != kNoDisableTarget) {
                    copy.disable_target = mapper.map(HandleFamily::DisableTarget, src.disable_target);
                }
                for (uint32_t& t : copy.fork_targets) {
                    t = mapper.map(HandleFamily::DisableTarget, t);
                }
                m_.set_function(fi.functions[h], std::move(copy));
            }

            for (const AssertionMonitor& mon : u.monitors) {
                AssertionMonitor copy = mon;
                copy.clock = static_cast<Signal>(mapper.map(HandleFamily::Signal, static_cast<uint32_t>(mon.clock)));
                m_.monitors.push_back(std::move(copy));
            }

            for (const ClassDesc& cd : u.class_descs) {
                m_.set_class_desc(cd.class_ref,
                                  map_functions(mapper, cd.slots),
                                  cd.type_id,
                                  map_functions(mapper, cd.iface_slots),
                                  cd.iface_ids);
            }

            for (const DpiExport& e : u.dpi_exports) {
                DpiExport copy = e;
                copy.func = static_cast<FunctionId>(mapper.map(HandleFamily::Function, static_cast<uint32_t>(e.func)));
                m_.dpi_exports.push_back(std::move(copy));
            }
        }

        for (const uint32_t ri : root_inst_) {
            emit_processes(ri);
        }

        // A flush target names a process of its own unit, now that every
        // process has its flat index.
        m_.disable_flush_proc.assign(m_.disable_targets, Module::no_flush_proc);
        for (const FlatInstance& fi : insts_) {
            const Unit& u = d_.unit(fi.unit);
            for (uint32_t h = 0; h < u.disable_targets.size(); ++h) {
                const UnitDisableTarget& t = u.disable_targets[h];
                if (t.import == kLocal && t.flush_proc != Module::no_flush_proc) {
                    m_.disable_flush_proc[fi.targets[h]] = fi.processes[t.flush_proc];
                }
            }
        }
    }

    // Emit the processes of the subtree under `root`, each body's processes
    // interleaved with its child instances at their ChildSlot::process_pos.
    void emit_processes(uint32_t root) {
        struct Frame {
            uint32_t inst;
            uint32_t next_process = 0;
            uint32_t next_slot    = 0;
        };
        std::vector<Frame> stack{ Frame{ .inst = root } };
        while (!stack.empty()) {
            Frame&              f  = stack.back();
            const FlatInstance& fi = insts_[f.inst];
            const Unit&         u  = d_.unit(fi.unit);
            if (f.next_slot < u.children.size() &&
                (u.children[f.next_slot].process_pos <= f.next_process || f.next_process == u.processes.size())) {
                assert(u.children[f.next_slot].process_pos <= u.processes.size());
                const uint32_t child = fi.children[f.next_slot++];
                stack.push_back(Frame{ .inst = child });
                continue;
            }
            if (f.next_process == u.processes.size()) {
                stack.pop_back();
                continue;
            }
            const Process& p = u.processes[f.next_process++];
            assert(p.scope == Process::no_scope && "a unit process carries no scope; flatten assigns it");
            FlatMapper& mapper = mappers_[f.inst];
            Process     copy{
                .activation              = p.activation,
                .wait_events             = p.wait_events,
                .self_start              = p.self_start,
                .starts_after_procedures = p.starts_after_procedures,
                .reactive_home           = p.reactive_home,
                .spawn_root              = p.spawn_root,
                .span                    = p.span,
                .ir                      = p.ir.rebased(mapper),
                .scope                   = fi.scope,
            };
            for (WaitEvent& e : copy.wait_events) {
                if (e.handle == kNoWaitHandle) {
                    e.signal = static_cast<Signal>(mapper.map(HandleFamily::Signal, static_cast<uint32_t>(e.signal)));
                }
            }
            insts_[f.inst].processes.push_back(static_cast<uint32_t>(m_.processes().size()));
            m_.add_process(std::move(copy));
        }
    }

    static std::vector<FunctionId> map_functions(HandleMapper& m, const std::vector<FunctionId>& ids) {
        std::vector<FunctionId> out;
        out.reserve(ids.size());
        for (const FunctionId f : ids) {
            out.push_back(static_cast<FunctionId>(m.map(HandleFamily::Function, static_cast<uint32_t>(f))));
        }
        return out;
    }

    // Build each top's InstanceScope tree. Pre-order numbering puts every child
    // after its parent, so a reverse sweep finishes children before parents.
    void build_scopes() {
        std::vector<std::optional<InstanceScope>> scopes(insts_.size());
        for (uint32_t ii = static_cast<uint32_t>(insts_.size()); ii-- > 0;) {
            const FlatInstance& fi = insts_[ii];
            const Unit&         u  = d_.unit(fi.unit);
            if (u.kind != UnitKind::Module) {
                continue;
            }
            InstanceScope s;
            s.id       = fi.scope;
            s.name     = fi.name;
            s.def_name = u.def_name;
            s.unit_key = unit_keys_[static_cast<uint32_t>(fi.unit)];
            s.unit     = static_cast<uint32_t>(fi.unit);
            for (uint32_t h = 0; h < u.signals.size(); ++h) {
                const uint32_t imp = u.signals[h].import;
                if (imp != kLocal) {
                    s.externs.push_back(u.imports[imp].kind == Import::Kind::Representative ? InstanceScope::no_extern
                                                                                            : fi.signals[h]);
                    continue;
                }
                s.locals.push_back(fi.signals[h]);
                if (u.signals[h].scoped) {
                    s.signals.push_back(fi.signals[h]);
                }
            }
            for (uint32_t h = 0; h < u.functions.size(); ++h) {
                const uint32_t imp = u.functions[h].import;
                if (imp == kLocal) {
                    s.fn_locals.push_back(fi.functions[h]);
                    continue;
                }
                assert(u.imports[imp].kind == Import::Kind::Member && "a function is imported by member, never as a layout");
                s.fn_externs.push_back(fi.functions[h]);
            }
            for (uint32_t h = 0; h < u.disable_targets.size(); ++h) {
                if (u.disable_targets[h].import == kLocal) {
                    s.disable_locals.push_back(fi.targets[h]);
                }
            }
            for (uint32_t h = 0; h < u.cover_types.size(); ++h) {
                if (u.cover_types[h].import == kLocal) {
                    s.cover_locals.push_back(fi.cover_types[h]);
                }
            }
            s.monitor_base  = fi.monitor_base;
            s.monitor_count = fi.monitor_count;
            s.children.reserve(fi.children.size());
            for (const uint32_t ci : fi.children) {
                s.children.push_back(std::move(*scopes[ci]));
                scopes[ci].reset();
            }
            scopes[ii] = std::move(s);
        }
        for (const uint32_t ri : root_inst_) {
            if (scopes[ri]) {
                m_.add_top(std::move(*scopes[ri]));
            }
        }
    }

    void emit_design_tables() {
        m_.name             = d_.name;
        m_.top_name         = d_.top_name;
        m_.tick_fs          = d_.tick_fs;
        m_.timeformat_units = d_.timeformat_units;
        m_.iface_count      = d_.iface_count;
        m_.iface_slot_count = d_.iface_slot_count;
        if (d_.iface_unreachable) {
            const auto& of = instances_of_[static_cast<uint32_t>(d_.iface_unreachable->unit)];
            assert(of.size() == 1 && "the unreachable interface-method body lives in a static unit");
            m_.iface_unreachable = insts_[of.front()].functions[static_cast<uint32_t>(d_.iface_unreachable->fn)];
        }
        for (const auto& [ref, id] : d_.iface_type_ids) {
            m_.set_iface_type_id(ref, id);
        }
        for (const Ref r : d_.process_classes) {
            m_.set_process_class(r);
        }
        for (const ChangeNotify& cn : d_.change_notifies) {
            for (uint32_t e = 0; e < cn.field.size(); ++e) {
                if (cn.field[e] != kNoNotify) {
                    m_.set_change_notify(cn.class_ref, cn.prop_field, static_cast<Edge>(e), cn.field[e], cn.container);
                }
            }
        }
        for (const UnitId u : d_.vif_units) {
            for (const uint32_t ii : instances_of_[static_cast<uint32_t>(u)]) {
                m_.vif_scopes.push_back(insts_[ii].scope);
            }
        }
    }

    Design&                             d_;
    Module&                             m_;
    std::vector<FlatInstance>           insts_;
    std::vector<std::string>            unit_keys_;
    std::vector<uint32_t>               root_inst_;
    std::vector<std::vector<uint32_t>>  instances_of_;
    std::vector<std::optional<NameMap>> signal_names_;
    std::vector<std::optional<NameMap>> function_names_;
    std::vector<std::optional<NameMap>> target_names_;
    std::vector<std::optional<NameMap>> cover_type_names_;
    // Total unit-local signal / function entries over every instance: the
    // longest acyclic extern-declaration chain.
    size_t signal_entries_   = 0;
    size_t function_entries_ = 0;
    // One per instance, indexed like insts_.
    std::vector<FlatMapper> mappers_;
};

} // namespace

Function signature_of(const Function& f) {
    return Function{
        .name                = f.name,
        .return_type         = f.return_type,
        .param_types         = f.param_types,
        .arg_dirs            = f.arg_dirs,
        .local_types         = f.local_types,
        .takes_iface_ctx     = f.takes_iface_ctx,
        .ctx_member          = f.ctx_member,
        .is_task             = f.is_task,
        .is_fork_child       = f.is_fork_child,
        .disable_target      = f.disable_target,
        .fork_kind           = f.fork_kind,
        .fork_targets        = f.fork_targets,
        .is_dpi              = f.is_dpi,
        .is_pure             = f.is_pure,
        .is_dpi_task         = f.is_dpi_task,
        .extern_name         = f.extern_name,
        .dpi_arg_ctypes      = f.dpi_arg_ctypes,
        .dpi_ret_ctype       = f.dpi_ret_ctype,
        .dpi_arg_elem_ctypes = f.dpi_arg_elem_ctypes,
        .span                = f.span,
        .scope               = f.scope,
    };
}

std::unique_ptr<Module> flatten(Design&& design) {
    // Before the module takes the design's pool: printing a unit reads it.
    auto keys   = name_units(design);
    auto module = std::make_unique<Module>(std::move(design.pool));
    Flattener(design, *module, std::move(keys)).run();
    return module;
}

} // namespace clotho::ir
