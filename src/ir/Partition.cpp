#include "Partition.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

#include <ankerl/unordered_dense.h>

#include "Printer.h"

namespace clotho::ir {

namespace {

constexpr uint32_t kNone = UINT32_MAX;

// A frontend invariant partition relies on does not hold: name it and stop.
[[noreturn]] void broken(const char* what, const std::string& detail) {
    std::fprintf(stderr, "partition: %s: %s\n", what, detail.c_str());
    std::abort();
}

// Where a unit's single instance sits: its root and the child slots below it.
struct UnitPos {
    uint32_t              root = 0;
    std::vector<uint32_t> path;
    // Flat signal name prefix of the instance.
    std::string prefix;
};

// Owner unit and unit-local index of one flat entity.
struct Slot {
    uint32_t unit  = kNone;
    uint32_t local = kNone;
};

// Records the handles a body refers to, leaving the body as it is.
struct Recorder final : HandleMapper {
    std::vector<std::pair<HandleFamily, uint32_t>> seen;

    uint32_t map(HandleFamily family, uint32_t h) override {
        seen.emplace_back(family, h);
        return h;
    }
};

class Partitioner {
public:
    Partitioner(Module& module, const Attribution& a, Design& design)
      : m_(module)
      , a_(a)
      , d_(design) {}

    void run() {
        make_units();
        bound_ports();
        assign_signals();
        declare_contributions();
        assign_processes();
        attribute_by_use();
        assign_tables();
        localize();
        bind_ports();
        place_children();
        design_tables();
        merge_specializations();
    }

    // The specialization of each unit (Attribution::scope_specs), UINT32_MAX
    // for the static and glue units.
    const std::vector<uint32_t>& unit_specs() const {
        return unit_spec_;
    }

private:
    uint32_t add_unit(UnitKind kind, std::string def_name, UnitPos pos, uint32_t parent = kNone) {
        d_.units.push_back(Unit{ .kind = kind, .def_name = std::move(def_name) });
        pos_.push_back(std::move(pos));
        parent_.push_back(parent);
        memo_.emplace_back();
        return static_cast<uint32_t>(d_.units.size() - 1);
    }

    // Roots: the static units, the glue unit, then each top's instance tree, one
    // unit per instance.
    void make_units() {
        for (const std::string& name : a_.statics) {
            const auto     root = static_cast<uint32_t>(d_.roots.size());
            const uint32_t u    = add_unit(UnitKind::Static, name, UnitPos{ .root = root });
            d_.roots.push_back(Root{ .unit = UnitId{ u }, .name = name });
        }
        {
            const auto root = static_cast<uint32_t>(d_.roots.size());
            glue_           = add_unit(UnitKind::Static, "$glue", UnitPos{ .root = root });
            d_.roots.push_back(Root{ .unit = UnitId{ glue_ }, .name = "$glue" });
        }

        const bool single = m_.tops().size() == 1;
        for (const InstanceScope& top : m_.tops()) {
            const auto        root   = static_cast<uint32_t>(d_.roots.size());
            const std::string prefix = single ? std::string() : top.name + ".";
            const uint32_t    tu = add_unit(UnitKind::Module, top.def_name, UnitPos{ .root = root, .prefix = prefix });
            d_.roots.push_back(Root{ .unit = UnitId{ tu }, .name = top.name, .prefix = prefix });
            note_scope(top.id, tu);

            // (scope, its unit, next child)
            struct Frame {
                const InstanceScope* scope;
                uint32_t             unit;
                uint32_t             next = 0;
            };
            std::vector<Frame> stack{ Frame{ .scope = &top, .unit = tu } };
            while (!stack.empty()) {
                Frame& f = stack.back();
                if (f.next == f.scope->children.size()) {
                    stack.pop_back();
                    continue;
                }
                const uint32_t       slot   = f.next++;
                const InstanceScope& child  = f.scope->children[slot];
                const uint32_t       parent = f.unit;
                UnitPos              pos{ .root   = root,
                                          .path   = pos_[parent].path,
                                          .prefix = pos_[parent].prefix + child.name + "." };
                pos.path.push_back(slot);
                const uint32_t cu = add_unit(UnitKind::Module, child.def_name, std::move(pos), parent);
                d_.units[parent].children.push_back(ChildSlot{ .unit = UnitId{ cu }, .name = child.name });
                note_scope(child.id, cu);
                stack.push_back(Frame{ .scope = &child, .unit = cu });
            }
        }
    }

    void note_scope(uint32_t scope, uint32_t unit) {
        if (scope >= scope_unit_.size()) {
            scope_unit_.resize(scope + 1, kNone);
        }
        scope_unit_[scope] = unit;
    }

    // Each aliasing port (Attribution::port_aliases) and interface port
    // (Attribution::iface_ports) becomes a bound port of its instance's unit,
    // which then reaches the aliased signal, or the interface instance's
    // members, through the port rather than by naming them where they are.
    void bound_ports() {
        alias_.resize(d_.units.size());
        iface_port_.resize(d_.units.size());
        port_targets_.resize(d_.units.size());
        const auto add = [&](uint32_t scope, const std::string& name, BoundPort::Kind kind, uint32_t target) {
            const uint32_t u = unit_of_scope(scope);
            assert(parent_[u] != kNone && "a bound port on a root instance");
            Unit&      unit = d_.units[u];
            const auto k    = static_cast<uint32_t>(unit.bound_ports.size());
            unit.bound_ports.push_back(BoundPort{ .name = name, .kind = kind });
            port_targets_[u].push_back(target);
            return std::pair{ u, k };
        };
        for (const Attribution::PortAlias& pa : a_.port_aliases) {
            const auto [u, k] = add(pa.scope, pa.port, BoundPort::Kind::Signal, static_cast<uint32_t>(pa.target));
            alias_[u].try_emplace(static_cast<uint32_t>(pa.target), k);
        }
        for (const Attribution::IfacePort& ip : a_.iface_ports) {
            const auto [u, k] = add(ip.scope, ip.port, BoundPort::Kind::Instance, ip.target);
            iface_port_[u].try_emplace(unit_of_scope(ip.target), k);
        }
        for (const Attribution::Contribution& c : a_.contributions) {
            const auto [u, k] = add(c.scope, c.name, BoundPort::Kind::Signal, static_cast<uint32_t>(c.target));
            contributions_.push_back(PlacedContribution{ .unit = u, .port = k, .signal = static_cast<uint32_t>(c.signal) });
        }
    }

    // A contribution's flat signal was placed as a local of its instance's
    // unit; it is really an extern declaration of the bound port, bound by the
    // parent to the net, a driver slot or the parent's own contribution.
    void declare_contributions() {
        for (const PlacedContribution& c : contributions_) {
            const Slot& s = sig_[c.signal];
            if (s.unit != c.unit) {
                broken("a contribution is not a signal of its instance", std::to_string(c.signal));
            }
            Unit&       unit = d_.units[c.unit];
            UnitSignal& us   = unit.signals[s.local];
            us.import        = static_cast<uint32_t>(unit.imports.size());
            us.scoped        = false;
            us.info.name     = unit.bound_ports[c.port].name;
            us.info.loc      = SourceLoc{ 0 };
            us.info.dir      = SignalInfo::Direction::Internal;
            unit.imports.push_back(Import{ .kind = Import::Kind::Port, .port = c.port });
        }
    }

    // The parent binds each bound port to its own handle for the target: a
    // signal, or an instance reference.
    void bind_ports() {
        for (uint32_t u = 0; u < d_.units.size(); ++u) {
            const std::vector<uint32_t>& targets = port_targets_[u];
            if (targets.empty()) {
                continue;
            }
            // signal_in and iface_in grow the parent's signal, import and
            // instance-reference tables, never its children, so `c` stays put.
            const uint32_t p = parent_[u];
            ChildSlot&     c = d_.units[p].children[pos_[u].path.back()];
            assert(static_cast<uint32_t>(c.unit) == u);
            for (size_t k = 0; k < targets.size(); ++k) {
                const bool signal = d_.units[u].bound_ports[k].kind == BoundPort::Kind::Signal;
                c.bindings.push_back(signal ? signal_in(p, targets[k]) : iface_in(p, targets[k]));
            }
        }
    }

    // The instance of unit `to`, named from the instance of unit `from`:
    // through one of `from`'s interface ports when one is bound to it, so the
    // reference does not depend on where the two instances sit.
    InstanceRef instance_ref(uint32_t from, uint32_t to) const {
        if (const auto port = iface_port_[from].find(to); port != iface_port_[from].end()) {
            return InstanceRef{ .base = InstanceRef::Base::Port, .index = port->second };
        }
        return relative(from, to);
    }

    uint32_t unit_of_scope(uint32_t scope) const {
        assert(scope < scope_unit_.size() && scope_unit_[scope] != kNone);
        return scope_unit_[scope];
    }

    uint32_t unit_of(const Owner& o) const {
        switch (o.kind) {
            case Owner::Kind::Unknown:
            case Owner::Kind::Glue:
                return glue_;
            case Owner::Kind::Scope:
                return unit_of_scope(o.index);
            case Owner::Kind::Static:
                assert(o.index < a_.statics.size());
                return o.index;
        }
        std::abort();
    }

    // Scoped signals belong to their scope's unit, named relative to it; the
    // rest go where the frontend attributed them, keeping their flat names.
    void assign_signals() {
        sig_.assign(m_.signals().size(), Slot{});
        for (const InstanceScope& top : m_.tops()) {
            std::vector<const InstanceScope*> stack{ &top };
            while (!stack.empty()) {
                const InstanceScope* s = stack.back();
                stack.pop_back();
                const uint32_t u = unit_of_scope(s->id);
                for (const Signal g : s->signals) {
                    const SignalInfo& info = m_.signal(g);
                    if (!info.name.starts_with(pos_[u].prefix)) {
                        broken("a scoped signal is not named under its instance",
                               "'" + info.name + "' in '" + pos_[u].prefix + "'");
                    }
                    SignalInfo local = info;
                    local.name       = info.name.substr(pos_[u].prefix.size());
                    place_signal(static_cast<uint32_t>(g), u, std::move(local), true);
                }
                for (auto it = s->children.rbegin(); it != s->children.rend(); ++it) {
                    stack.push_back(&*it);
                }
            }
        }
        for (uint32_t g = 0; g < sig_.size(); ++g) {
            if (sig_[g].unit != kNone) {
                continue;
            }
            // Storage the frontend attributed to a unit without declaring it
            // in the instance: named under the instance where it can be.
            const Owner        o    = g < a_.signals.size() ? a_.signals[g] : Owner{};
            const uint32_t     u    = unit_of(o);
            SignalInfo         info = m_.signal(static_cast<Signal>(g));
            const std::string& pre  = pos_[u].prefix;
            if (info.name.starts_with(pre)) {
                info.name.erase(0, pre.size());
            }
            place_signal(g, u, std::move(info), false);
        }
    }

    void place_signal(uint32_t g, uint32_t u, SignalInfo info, bool scoped) {
        assert(sig_[g].unit == kNone && "a signal two scopes list");
        Unit& unit = d_.units[u];
        sig_[g]    = Slot{ .unit = u, .local = static_cast<uint32_t>(unit.signals.size()) };
        unit.signals.push_back(UnitSignal{ .info = std::move(info), .scoped = scoped });
    }

    void assign_processes() {
        const auto procs = m_.processes();
        proc_.assign(procs.size(), Slot{});
        for (uint32_t p = 0; p < procs.size(); ++p) {
            uint32_t u = glue_;
            if (procs[p].scope != Process::no_scope) {
                u = unit_of_scope(procs[p].scope);
            } else if (p < a_.processes.size()) {
                u = unit_of(a_.processes[p]);
            }
            proc_[p] = Slot{ .unit = u, .local = static_cast<uint32_t>(unit_procs_(u).size()) };
            unit_procs_(u).push_back(p);
        }
    }

    std::vector<uint32_t>& unit_procs_(uint32_t u) {
        if (u >= procs_of_.size()) {
            procs_of_.resize(d_.units.size());
        }
        return procs_of_[u];
    }

    // Functions, monitors and covergroup types the frontend did not
    // attribute belong to the first body that uses them, walking from the
    // processes outward through the bodies they reach. A `disable` target
    // belongs to the body that declares it: the named block's, or the task.
    void attribute_by_use() {
        const size_t nf = m_.function_count();
        fn_unit_.assign(nf, kNone);
        for (uint32_t f = 0; f < nf; ++f) {
            if (f < a_.functions.size() && a_.functions[f].kind != Owner::Kind::Unknown) {
                fn_unit_[f] = unit_of(a_.functions[f]);
            }
        }
        mon_unit_.assign(m_.monitors.size(), kNone);
        cover_unit_.assign(m_.cover_types.size(), kNone);
        target_unit_.assign(m_.disable_targets, kNone);
        // Where each target's forked children run, for the targets no block
        // declares (below).
        std::vector<uint32_t> forked_unit(m_.disable_targets, kNone);

        // (is_function, index)
        std::vector<std::pair<bool, uint32_t>> work;
        for (uint32_t p = 0; p < m_.processes().size(); ++p) {
            work.emplace_back(false, p);
        }
        for (uint32_t f = 0; f < nf; ++f) {
            if (fn_unit_[f] != kNone) {
                work.emplace_back(true, f);
            }
        }
        std::vector<uint8_t> scanned(nf, 0);
        for (uint32_t f = 0; f < nf; ++f) {
            if (fn_unit_[f] != kNone) {
                scanned[f] = 1;
            }
        }

        const auto scan = [&](const Ir& body, uint32_t u) {
            for (uint32_t i = 0; i < body.inst_count(); ++i) {
                if (body.tag(static_cast<Index>(i)) == Inst::Tag::NamedBlock) {
                    const auto bi = body.extra_data<BlockInfo>(body.payload(static_cast<Index>(i)).extra);
                    claim(target_unit_, bi.label, u, "a disable target two bodies declare");
                }
            }
            Recorder rec;
            (void)body.rebased(rec);
            for (const auto& [family, h] : rec.seen) {
                switch (family) {
                    case HandleFamily::Function:
                        if (fn_unit_[h] == kNone) {
                            fn_unit_[h] = u;
                        }
                        if (scanned[h] == 0) {
                            scanned[h] = 1;
                            work.emplace_back(true, h);
                        }
                        break;
                    case HandleFamily::Monitor:
                        if (mon_unit_[h] == kNone) {
                            mon_unit_[h] = u;
                        }
                        break;
                    case HandleFamily::CoverType:
                        if (cover_unit_[h] == kNone) {
                            cover_unit_[h] = u;
                        }
                        break;
                    case HandleFamily::Signal:
                    case HandleFamily::Layout:
                    case HandleFamily::IfaceScope:
                    case HandleFamily::DisableTarget:
                        break;
                }
            }
        };

        uint32_t next_unscanned = 0;
        for (size_t w = 0;; ++w) {
            if (w == work.size()) {
                // Nothing reached the rest. Dead code is still emitted, so it
                // still needs a unit: the one whose state it touches or whose
                // functions it calls, which keeps its references local -- a
                // production body only a rand join's steps would have called,
                // say -- and the glue unit when it names nothing.
                while (next_unscanned < nf && scanned[next_unscanned] != 0) {
                    ++next_unscanned;
                }
                if (next_unscanned == nf) {
                    break;
                }
                const uint32_t f = next_unscanned;
                fn_unit_[f] =
                  fn_unit_[f] != kNone ? fn_unit_[f] : unit_by_reference(m_.function(static_cast<FunctionId>(f)).ir);
                scanned[f] = 1;
                work.emplace_back(true, f);
            }
            const auto [is_fn, idx] = work[w];
            if (is_fn) {
                const Function& fn = m_.function(static_cast<FunctionId>(idx));
                if (fn.disable_target != kNoDisableTarget) {
                    claim(target_unit_, fn.disable_target, fn_unit_[idx], "a disable target two bodies declare");
                }
                // A named `fork` is a target no block instruction declares: its
                // children are the activity it ends and the only bodies that name
                // it, so it belongs to the unit they run in, the forking body's.
                // Only where no body declares the target, and the first child
                // reached stands for all of them: a child left behind by a failed
                // lowering is placed wherever it happens to refer to, and must not
                // contradict the body that does declare the target.
                for (const uint32_t t : fn.fork_targets) {
                    assert(t < forked_unit.size());
                    if (forked_unit[t] == kNone) {
                        forked_unit[t] = fn_unit_[idx];
                    }
                }
                scan(fn.ir, fn_unit_[idx]);
            } else {
                scan(m_.processes()[idx].ir, proc_[idx].unit);
            }
        }
        for (uint32_t t = 0; t < target_unit_.size(); ++t) {
            if (target_unit_[t] == kNone) {
                target_unit_[t] = forked_unit[t];
            }
        }
    }

    // The unit of the first signal, or else the first attributed function, a
    // body refers to; the glue unit when it refers to neither.
    uint32_t unit_by_reference(const Ir& body) const {
        Recorder rec;
        (void)body.rebased(rec);
        for (const auto& [family, h] : rec.seen) {
            if (family == HandleFamily::Signal) {
                return sig_[h].unit;
            }
        }
        for (const auto& [family, h] : rec.seen) {
            if (family == HandleFamily::Function && fn_unit_[h] != kNone) {
                return fn_unit_[h];
            }
        }
        return glue_;
    }

    static void claim(std::vector<uint32_t>& owner, uint32_t id, uint32_t u, const char* what) {
        assert(id < owner.size());
        if (owner[id] != kNone && owner[id] != u) {
            broken(what, std::to_string(id));
        }
        owner[id] = u;
    }

    // Give every function, monitor, covergroup type and disable target its
    // unit-local index, in flat order.
    void assign_tables() {
        fn_.assign(m_.function_count(), Slot{});
        for (uint32_t f = 0; f < fn_.size(); ++f) {
            const uint32_t u    = fn_unit_[f];
            Unit&          unit = d_.units[u];
            fn_[f]              = Slot{ .unit = u, .local = static_cast<uint32_t>(unit.functions.size()) };
            unit.functions.push_back(UnitFunction{ .name = f < a_.function_names.size() ? a_.function_names[f] : "" });
        }
        mon_.assign(m_.monitors.size(), Slot{});
        for (uint32_t i = 0; i < mon_.size(); ++i) {
            const uint32_t u = mon_unit_[i] == kNone ? glue_ : mon_unit_[i];
            mon_[i]          = Slot{ .unit = u, .local = static_cast<uint32_t>(d_.units[u].monitors.size()) };
            d_.units[u].monitors.emplace_back();
        }
        cover_.assign(m_.cover_types.size(), Slot{});
        for (uint32_t i = 0; i < cover_.size(); ++i) {
            const uint32_t u = cover_unit_[i] == kNone ? glue_ : cover_unit_[i];
            cover_[i]        = Slot{ .unit = u, .local = static_cast<uint32_t>(d_.units[u].cover_types.size()) };
            d_.units[u].cover_types.push_back(m_.cover_types[i]);
        }
        target_.assign(m_.disable_targets, Slot{});
        for (uint32_t t = 0; t < target_.size(); ++t) {
            const uint32_t u    = target_unit_[t] == kNone ? glue_ : target_unit_[t];
            Unit&          unit = d_.units[u];
            target_[t]          = Slot{ .unit = u, .local = static_cast<uint32_t>(unit.disable_targets.size()) };
            uint32_t flush      = Module::no_flush_proc;
            if (t < m_.disable_flush_proc.size() && m_.disable_flush_proc[t] != Module::no_flush_proc) {
                const Slot& p = proc_[m_.disable_flush_proc[t]];
                if (p.unit != u) {
                    broken("a disable target flushes a process of another unit", std::to_string(t));
                }
                flush = p.local;
            }
            unit.disable_targets.push_back(UnitDisableTarget{
              .is_task    = t < m_.disable_target_is_task.size() && m_.disable_target_is_task[t] != 0,
              .flush_proc = flush,
              .name       = t < a_.disable_target_names.size() ? a_.disable_target_names[t] : "",
            });
        }
    }

    // Rebase every body onto its unit's tables, creating extern declarations
    // for whatever it reaches in another unit.
    class Localizer final : public HandleMapper {
    public:
        Localizer(Partitioner& p, uint32_t unit)
          : p_(p)
          , u_(unit) {}

        uint32_t map(HandleFamily family, uint32_t h) override {
            switch (family) {
                case HandleFamily::Signal:
                    return p_.signal_in(u_, h);
                case HandleFamily::Layout:
                    return p_.layout_in(u_, h);
                case HandleFamily::Function:
                    return p_.function_in(u_, h);
                case HandleFamily::IfaceScope:
                    return p_.iface_in(u_, h);
                case HandleFamily::Monitor:
                    return local_of(p_.mon_, h, "an assertion monitor another unit owns");
                case HandleFamily::CoverType:
                    return local_of(p_.cover_, h, "a covergroup type another unit owns");
                case HandleFamily::DisableTarget:
                    return p_.target_in(u_, h);
            }
            std::abort();
        }

    private:
        uint32_t local_of(const std::vector<Slot>& table, uint32_t h, const char* what) const {
            assert(h < table.size());
            if (table[h].unit != u_) {
                broken(what, std::to_string(h) + " from unit '" + p_.d_.units[u_].def_name + "'");
            }
            return table[h].local;
        }

        Partitioner& p_;
        uint32_t     u_;
    };

    uint32_t signal_in(uint32_t u, uint32_t g) {
        assert(g < sig_.size());
        const Slot& s = sig_[g];
        if (s.unit == u) {
            return s.local;
        }
        auto [it, inserted] = memo_[u].signals.try_emplace(g, 0);
        if (inserted) {
            Unit&             unit   = d_.units[u];
            const auto        imp    = static_cast<uint32_t>(unit.imports.size());
            const SignalInfo& target = d_.units[s.unit].signals[s.local].info;
            SignalInfo        shape  = target;
            shape.dir                = SignalInfo::Direction::Internal;
            if (const auto port = alias_[u].find(g); port != alias_[u].end()) {
                // Declared by the port, so shaped like the port: the parent's
                // name and location for the net differ between instances.
                unit.imports.push_back(Import{ .kind = Import::Kind::Port, .port = port->second });
                shape.name = unit.bound_ports[port->second].name;
                shape.loc  = SourceLoc{ 0 };
            } else {
                unit.imports.push_back(
                  Import{ .kind = Import::Kind::Member, .inst = instance_ref(u, s.unit), .member = target.name });
            }
            it->second = static_cast<uint32_t>(unit.signals.size());
            unit.signals.push_back(UnitSignal{ .info = std::move(shape), .import = imp });
        }
        return it->second;
    }

    // A signal standing only for a layout (HandleFamily::Layout): imported
    // from any instance of its unit, so the reference does not name the
    // instance the frontend happened to pick.
    uint32_t layout_in(uint32_t u, uint32_t g) {
        assert(g < sig_.size());
        const Slot& s = sig_[g];
        if (s.unit == u) {
            return s.local;
        }
        auto [it, inserted] = memo_[u].layouts.try_emplace(g, 0);
        if (inserted) {
            Unit&             unit   = d_.units[u];
            const auto        imp    = static_cast<uint32_t>(unit.imports.size());
            const SignalInfo& target = d_.units[s.unit].signals[s.local].info;
            unit.imports.push_back(
              Import{ .kind = Import::Kind::Representative, .unit = UnitId{ s.unit }, .member = target.name });
            SignalInfo shape = target;
            shape.dir        = SignalInfo::Direction::Internal;
            it->second       = static_cast<uint32_t>(unit.signals.size());
            unit.signals.push_back(UnitSignal{ .info = std::move(shape), .import = imp });
        }
        return it->second;
    }

    uint32_t function_in(uint32_t u, uint32_t f) {
        assert(f < fn_.size());
        const Slot& s = fn_[f];
        if (s.unit == u) {
            return s.local;
        }
        auto [it, inserted] = memo_[u].functions.try_emplace(f, 0);
        if (inserted) {
            Unit&              unit = d_.units[u];
            const std::string& name = d_.units[s.unit].functions[s.local].name;
            if (name.empty()) {
                broken("a function another unit calls has no import name",
                       "'" + m_.function(static_cast<FunctionId>(f)).name + "' from unit '" + unit.def_name + "'");
            }
            const auto imp = static_cast<uint32_t>(unit.imports.size());
            unit.imports.push_back(
              Import{ .kind = Import::Kind::Member, .inst = instance_ref(u, s.unit), .member = name });
            it->second = static_cast<uint32_t>(unit.functions.size());
            unit.functions.push_back(
              UnitFunction{ .fn = signature_of(m_.function(static_cast<FunctionId>(f))), .import = imp });
        }
        return it->second;
    }

    uint32_t target_in(uint32_t u, uint32_t t) {
        assert(t < target_.size());
        const Slot& s = target_[t];
        if (s.unit == u) {
            return s.local;
        }
        auto [it, inserted] = memo_[u].targets.try_emplace(t, 0);
        if (inserted) {
            Unit&                    unit   = d_.units[u];
            const UnitDisableTarget& target = d_.units[s.unit].disable_targets[s.local];
            if (target.name.empty()) {
                broken("a disable target another unit names has no import name",
                       std::to_string(t) + " from unit '" + unit.def_name + "'");
            }
            const auto imp = static_cast<uint32_t>(unit.imports.size());
            unit.imports.push_back(
              Import{ .kind = Import::Kind::Member, .inst = instance_ref(u, s.unit), .member = target.name });
            it->second = static_cast<uint32_t>(unit.disable_targets.size());
            unit.disable_targets.push_back(UnitDisableTarget{ .is_task = target.is_task, .import = imp });
        }
        return it->second;
    }

    uint32_t iface_in(uint32_t u, uint32_t scope) {
        auto [it, inserted] = memo_[u].iface.try_emplace(scope, 0);
        if (inserted) {
            Unit& unit = d_.units[u];
            it->second = static_cast<uint32_t>(unit.inst_refs.size());
            unit.inst_refs.push_back(instance_ref(u, unit_of_scope(scope)));
        }
        return it->second;
    }

    // The instance of unit `to`, named from the instance of unit `from`.
    InstanceRef relative(uint32_t from, uint32_t to) const {
        const UnitPos& a = pos_[from];
        const UnitPos& b = pos_[to];
        if (a.root != b.root) {
            return InstanceRef{ .base = InstanceRef::Base::Root, .index = b.root, .slots = b.path };
        }
        size_t common = 0;
        while (common < a.path.size() && common < b.path.size() && a.path[common] == b.path[common]) {
            ++common;
        }
        return InstanceRef{ .base  = InstanceRef::Base::Up,
                            .index = static_cast<uint32_t>(a.path.size() - common),
                            .slots = std::vector<uint32_t>(b.path.begin() + static_cast<std::ptrdiff_t>(common),
                                                           b.path.end()) };
    }

    void localize() {
        for (uint32_t u = 0; u < d_.units.size(); ++u) {
            Localizer loc(*this, u);
            for (const uint32_t p : unit_procs_(u)) {
                const Process& src = m_.processes()[p];
                Process        copy{
                    .activation              = src.activation,
                    .wait_events             = src.wait_events,
                    .self_start              = src.self_start,
                    .starts_after_procedures = src.starts_after_procedures,
                    .reactive_home           = src.reactive_home,
                    .spawn_root              = src.spawn_root,
                    .span                    = src.span,
                    .ir                      = src.ir.rebased(loc),
                };
                for (WaitEvent& e : copy.wait_events) {
                    if (e.handle == kNoWaitHandle) {
                        e.signal = static_cast<Signal>(loc.map(HandleFamily::Signal, static_cast<uint32_t>(e.signal)));
                    }
                }
                d_.units[u].processes.push_back(std::move(copy));
            }
        }
        for (uint32_t f = 0; f < fn_.size(); ++f) {
            const Function& src = m_.function(static_cast<FunctionId>(f));
            const uint32_t  u   = fn_[f].unit;
            Localizer       loc(*this, u);
            Function        copy = signature_of(src);
            copy.ir              = src.ir.rebased(loc);
            if (src.takes_iface_ctx) {
                copy.ctx_member =
                  static_cast<Signal>(loc.map(HandleFamily::Layout, static_cast<uint32_t>(src.ctx_member)));
            }
            if (src.disable_target != kNoDisableTarget) {
                copy.disable_target = loc.map(HandleFamily::DisableTarget, src.disable_target);
            }
            for (uint32_t& t : copy.fork_targets) {
                t = loc.map(HandleFamily::DisableTarget, t);
            }
            d_.units[u].functions[fn_[f].local].fn = std::move(copy);
        }
        for (uint32_t i = 0; i < mon_.size(); ++i) {
            const uint32_t   u = mon_[i].unit;
            Localizer        loc(*this, u);
            AssertionMonitor copy = m_.monitors[i];
            copy.clock = static_cast<Signal>(loc.map(HandleFamily::Signal, static_cast<uint32_t>(copy.clock)));
            d_.units[u].monitors[mon_[i].local] = std::move(copy);
        }
        // Class descriptors and DPI exports are design-level here: the glue unit
        // holds them, importing the functions they name.
        Localizer glue(*this, glue_);
        for (const ClassDesc& cd : m_.class_descs()) {
            ClassDesc copy = cd;
            for (FunctionId& f : copy.slots) {
                f = static_cast<FunctionId>(glue.map(HandleFamily::Function, static_cast<uint32_t>(f)));
            }
            for (FunctionId& f : copy.iface_slots) {
                f = static_cast<FunctionId>(glue.map(HandleFamily::Function, static_cast<uint32_t>(f)));
            }
            d_.units[glue_].class_descs.push_back(std::move(copy));
        }
        for (const DpiExport& e : m_.dpi_exports) {
            DpiExport copy = e;
            copy.func      = static_cast<FunctionId>(glue.map(HandleFamily::Function, static_cast<uint32_t>(e.func)));
            d_.units[glue_].dpi_exports.push_back(std::move(copy));
        }
    }

    // Place each child instance in its parent's process sequence where the
    // flat module had its first process, so flatten emits the same order.
    void place_children() {
        // Smallest flat process index anywhere in each unit's subtree. Children
        // follow their parent in unit numbering, so a reverse sweep sees every
        // child before its parent.
        std::vector<uint32_t> first(d_.units.size(), kNone);
        for (uint32_t u = static_cast<uint32_t>(d_.units.size()); u-- > 0;) {
            uint32_t best = unit_procs_(u).empty() ? kNone : unit_procs_(u).front();
            for (const ChildSlot& c : d_.units[u].children) {
                best = std::min(best, first[static_cast<uint32_t>(c.unit)]);
            }
            first[u] = best;
        }
        for (uint32_t u = 0; u < d_.units.size(); ++u) {
            const std::vector<uint32_t>& procs = unit_procs_(u);
            uint32_t                     pos   = 0;
            for (ChildSlot& c : d_.units[u].children) {
                const uint32_t at     = first[static_cast<uint32_t>(c.unit)];
                const auto     before = static_cast<uint32_t>(std::ranges::lower_bound(procs, at) - procs.begin());
                pos                   = std::max(pos, at == kNone ? pos : before);
                c.process_pos         = pos;
            }
        }
    }

    void design_tables() {
        d_.name             = m_.name;
        d_.top_name         = m_.top_name;
        d_.tick_fs          = m_.tick_fs;
        d_.timeformat_units = m_.timeformat_units;
        d_.iface_count      = m_.iface_count;
        d_.iface_slot_count = m_.iface_slot_count;
        if (m_.iface_unreachable) {
            const Slot& s        = fn_[static_cast<uint32_t>(*m_.iface_unreachable)];
            d_.iface_unreachable = UnitFunctionRef{ .unit = UnitId{ s.unit }, .fn = static_cast<FunctionId>(s.local) };
        }
        d_.iface_type_ids.assign(m_.iface_type_ids().begin(), m_.iface_type_ids().end());
        d_.process_classes.assign(m_.process_classes().begin(), m_.process_classes().end());
        d_.change_notifies.assign(m_.change_notifies().begin(), m_.change_notifies().end());
        for (const uint32_t scope : m_.vif_scopes) {
            d_.vif_units.push_back(UnitId{ unit_of_scope(scope) });
        }
    }

    // Give the instances of one specialization one unit wherever their units
    // came out identical (modular-compilation.md, step 2c). A merged instance
    // takes the representative's child slots too, so its whole subtree must
    // match the representative's, pair by pair. Instances whose bodies still
    // depend on where they sit stay units of their own.
    void merge_specializations() {
        const auto            n = static_cast<uint32_t>(d_.units.size());
        std::vector<uint32_t> spec(n, UINT32_MAX);
        for (uint32_t scope = 0; scope < scope_unit_.size(); ++scope) {
            if (scope_unit_[scope] != kNone && scope < a_.scope_specs.size()) {
                spec[scope_unit_[scope]] = a_.scope_specs[scope];
            }
        }
        std::vector<uint32_t> canon(n);
        for (uint32_t u = 0; u < n; ++u) {
            canon[u] = u;
        }
        ankerl::unordered_dense::map<uint32_t, uint32_t> first;
        for (uint32_t u = 0; u < n; ++u) {
            if (spec[u] == UINT32_MAX || canon[u] != u) {
                continue;
            }
            const auto [it, fresh] = first.try_emplace(spec[u], u);
            if (fresh) {
                continue;
            }
            const uint32_t rep = canon[it->second];
            if (!same_subtree(u, rep, spec)) {
                continue;
            }
            std::vector<std::pair<uint32_t, uint32_t>> pairs{ { u, rep } };
            while (!pairs.empty()) {
                const auto [x, y] = pairs.back();
                pairs.pop_back();
                canon[x] = canon[y];
                for (size_t k = 0; k < d_.units[x].children.size(); ++k) {
                    pairs.emplace_back(static_cast<uint32_t>(d_.units[x].children[k].unit),
                                       static_cast<uint32_t>(d_.units[y].children[k].unit));
                }
            }
        }
        compact(canon, spec);
    }

    // Whether the subtrees under units `a` and `b` are the same specialization
    // with the same IR, unit by unit.
    bool same_subtree(uint32_t a, uint32_t b, const std::vector<uint32_t>& spec) const {
        std::vector<std::pair<uint32_t, uint32_t>> pairs{ { a, b } };
        while (!pairs.empty()) {
            const auto [x, y] = pairs.back();
            pairs.pop_back();
            if (x == y) {
                continue;
            }
            if (spec[x] != spec[y] || !same_unit(d_.units[x], d_.units[y])) {
                return false;
            }
            for (size_t k = 0; k < d_.units[x].children.size(); ++k) {
                pairs.emplace_back(static_cast<uint32_t>(d_.units[x].children[k].unit),
                                   static_cast<uint32_t>(d_.units[y].children[k].unit));
            }
        }
        return true;
    }

    // Two units' own content, child units aside (same_subtree pairs those). A
    // unit holding assertion monitors or covergroup types never merges yet:
    // their records are not compared.
    static bool same_unit(const Unit& a, const Unit& b) {
        if (!a.monitors.empty() || !b.monitors.empty() || !a.cover_types.empty() || !b.cover_types.empty() ||
            !a.class_descs.empty() || !b.class_descs.empty() || !a.dpi_exports.empty() || !b.dpi_exports.empty()) {
            return false;
        }
        if (a.kind != b.kind || a.def_name != b.def_name || a.children.size() != b.children.size()) {
            return false;
        }
        for (size_t k = 0; k < a.children.size(); ++k) {
            const ChildSlot& x = a.children[k];
            const ChildSlot& y = b.children[k];
            if (x.name != y.name || x.bindings != y.bindings || x.process_pos != y.process_pos) {
                return false;
            }
        }
        return a.signals == b.signals && a.functions == b.functions && a.processes == b.processes &&
               a.imports == b.imports && a.inst_refs == b.inst_refs && a.bound_ports == b.bound_ports &&
               a.disable_targets == b.disable_targets;
    }

    // Drop the merged units, renumbering the survivors and every reference to
    // a unit.
    void compact(const std::vector<uint32_t>& canon, const std::vector<uint32_t>& spec) {
        const auto            n = static_cast<uint32_t>(d_.units.size());
        std::vector<uint32_t> renum(n, kNone);
        std::deque<Unit>      kept;
        for (uint32_t u = 0; u < n; ++u) {
            if (canon[u] == u) {
                renum[u] = static_cast<uint32_t>(kept.size());
                kept.push_back(std::move(d_.units[u]));
                unit_spec_.push_back(spec[u]);
            }
        }
        const auto to = [&](UnitId id) { return UnitId{ renum[canon[static_cast<uint32_t>(id)]] }; };
        for (Unit& u : kept) {
            for (ChildSlot& c : u.children) {
                c.unit = to(c.unit);
            }
            for (Import& i : u.imports) {
                if (i.kind == Import::Kind::Representative) {
                    i.unit = to(i.unit);
                }
            }
        }
        for (Root& r : d_.roots) {
            r.unit = to(r.unit);
        }
        if (d_.iface_unreachable) {
            d_.iface_unreachable->unit = to(d_.iface_unreachable->unit);
        }
        std::vector<UnitId> vif;
        for (const UnitId u : d_.vif_units) {
            const UnitId c = to(u);
            if (std::ranges::find(vif, c) == vif.end()) {
                vif.push_back(c);
            }
        }
        d_.vif_units = std::move(vif);
        d_.units     = std::move(kept);
    }

    struct Memo {
        ankerl::unordered_dense::map<uint32_t, uint32_t> signals;
        ankerl::unordered_dense::map<uint32_t, uint32_t> functions;
        ankerl::unordered_dense::map<uint32_t, uint32_t> iface;
        ankerl::unordered_dense::map<uint32_t, uint32_t> targets;
        ankerl::unordered_dense::map<uint32_t, uint32_t> layouts;
    };

    Module&            m_;
    const Attribution& a_;
    Design&            d_;
    uint32_t           glue_ = kNone;
    // Per unit.
    std::vector<UnitPos>               pos_;
    std::vector<Memo>                  memo_;
    // Unit of each unit's instantiating parent, kNone for a root.
    std::vector<uint32_t> parent_;
    // Per unit: flat signal its aliasing port reaches -> bound port index.
    std::vector<ankerl::unordered_dense::map<uint32_t, uint32_t>> alias_;
    // Per unit: unit of an interface instance an interface port is bound to ->
    // bound port index.
    std::vector<ankerl::unordered_dense::map<uint32_t, uint32_t>> iface_port_;
    // Per unit, parallel to Unit::bound_ports: the flat signal or the
    // InstanceScope::id the port is bound to.
    std::vector<std::vector<uint32_t>> port_targets_;
    // The contributions (Attribution::contributions), as placed in their units.
    struct PlacedContribution {
        uint32_t unit   = 0;
        uint32_t port   = 0;
        uint32_t signal = 0;
    };
    std::vector<PlacedContribution> contributions_;
    std::vector<std::vector<uint32_t>> procs_of_;
    // InstanceScope id -> unit.
    std::vector<uint32_t> scope_unit_;
    // Per flat entity.
    std::vector<Slot>     sig_;
    std::vector<Slot>     proc_;
    std::vector<Slot>     fn_;
    std::vector<Slot>     mon_;
    std::vector<Slot>     cover_;
    std::vector<Slot>     target_;
    std::vector<uint32_t> fn_unit_;
    std::vector<uint32_t> mon_unit_;
    std::vector<uint32_t> cover_unit_;
    std::vector<uint32_t> target_unit_;
    // Specialization of each unit once merged (unit_specs).
    std::vector<uint32_t> unit_spec_;
};

} // namespace

std::unique_ptr<Design> partition(Module& module, const Attribution& attribution, std::vector<uint32_t>* unit_specs) {
    auto        design = std::make_unique<Design>();
    Partitioner p(module, attribution, *design);
    p.run();
    if (unit_specs != nullptr) {
        *unit_specs = p.unit_specs();
    }
    design->pool = module.release_pool();
    return design;
}

std::string describe_sharing(const Design& design, std::span<const uint32_t> unit_specs) {
    // Instances of each unit, by expanding the roots through the child slots.
    std::vector<uint64_t> instances(design.units.size(), 0);
    std::vector<UnitId>   stack;
    for (const Root& r : design.roots) {
        stack.push_back(r.unit);
    }
    while (!stack.empty()) {
        const UnitId u = stack.back();
        stack.pop_back();
        ++instances[static_cast<uint32_t>(u)];
        for (const ChildSlot& c : design.unit(u).children) {
            stack.push_back(c.unit);
        }
    }

    // Instance units by specialization, in unit order.
    ankerl::unordered_dense::map<uint32_t, std::vector<uint32_t>> by_spec;
    uint64_t                                                      total = 0;
    size_t                                                        units = 0;
    for (uint32_t u = 0; u < unit_specs.size(); ++u) {
        if (unit_specs[u] != UINT32_MAX) {
            by_spec[unit_specs[u]].push_back(u);
            total += instances[u];
            ++units;
        }
    }
    const auto render = [&](uint32_t u) {
        std::ostringstream os;
        print_unit(design, UnitId{ u }, os);
        return os.str();
    };

    size_t             split = 0;
    std::ostringstream samples;
    for (const auto& [spec, us] : by_spec) {
        if (us.size() < 2) {
            continue;
        }
        if (++split > 8) {
            continue;
        }
        const std::string  first = render(us[0]);
        const std::string  other = render(us[1]);
        std::istringstream a(first);
        std::istringstream b(other);
        std::string        la;
        std::string        lb;
        size_t             line = 0;
        while (std::getline(a, la) && std::getline(b, lb) && la == lb) {
            ++line;
        }
        samples << "  " << design.units[us[0]].def_name << ": " << us.size() << " units, line " << line << ":\n    "
                << la << "\n    " << lb << "\n";
    }
    std::ostringstream os;
    os << "units: " << total << " instances of " << by_spec.size() << " specializations in " << units << " units; "
       << split << " specializations still split across instances\n"
       << samples.str();
    return os.str();
}

} // namespace clotho::ir
