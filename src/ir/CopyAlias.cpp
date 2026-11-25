// alias_copies: turn a pure continuous copy between two signals of one unit
// into a second name for one storage (docs/design/modular-compilation.md,
// "Copy aliasing").

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Design.h"
#include "Ir.h"

namespace clotho::ir {
namespace {

// A use count that no longer matters past "more than one".
constexpr uint32_t kMany = UINT32_MAX;

void bump(std::vector<uint32_t>& count, uint32_t h, uint32_t by = 1) {
    count[h] = count[h] > kMany - by ? kMany : count[h] + by;
}

bool is_select(Inst::Tag t) {
    switch (t) {
        case Inst::Tag::BitSelect:
        case Inst::Tag::PartSelect:
        case Inst::Tag::DynPartSelect:
        case Inst::Tag::ElemSelect:
        case Inst::Tag::ArrSlice:
        case Inst::Tag::MemberSelect:
            return true;
        default:
            return false;
    }
}

// The lvalue operand of a select, the one lvalue_root walks.
Index select_base(const Ir& ir, Index i) {
    return ir.tag(i) == Inst::Tag::BitSelect ? ir.payload(i).binary.l : ir.payload(i).op_extra.op;
}

// Count into `writes` every use of a signal of a body that is not a read: any
// use of the signal, or of a select of it, by anything but a Load or a further
// select, and every handle a side table names for a write or a virtual
// interface layout. Over-counting only loses an alias; missing a write would
// lose one, so the classification is a whitelist.
void count_writes(const Ir& ir, std::vector<uint32_t>& writes) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        const auto      i = static_cast<Index>(k);
        const Inst::Tag t = ir.tag(i);
        switch (t) {
            case Inst::Tag::ReadMem:
                bump(writes, static_cast<uint32_t>(ir.extra_data<ReadMemInfo>(ir.payload(i).op_extra.extra).target), kMany);
                break;
            case Inst::Tag::FreadMem:
                bump(writes, static_cast<uint32_t>(ir.extra_data<FreadMemInfo>(ir.payload(i).op_extra.extra).mem), kMany);
                break;
            case Inst::Tag::IfaceSignal:
                bump(writes, static_cast<uint32_t>(ir.extra_data<IfaceMember>(ir.payload(i).op_extra.extra).signal), kMany);
                break;
            default:
                break;
        }
        if (t == Inst::Tag::Load) {
            continue;
        }
        ir.for_each_operand(i, [&](Index op) {
            if (is_select(t) && op == select_base(ir, i)) {
                return;
            }
            const Index root = lvalue_root(ir, op).root;
            if (ir.tag(root) == Inst::Tag::Signal) {
                bump(writes, static_cast<uint32_t>(ir.payload(root).signal));
            }
        });
    }
}

// A packed integral value: one bit vector, so one storage serves both names
// with nothing a conversion or an aggregate layout would have to agree on.
bool aliasable_type(const InternPool& pool, Ref type) {
    const Type& ty = std::get<Type>(pool.get(type));
    switch (ty.kind) {
        case Type::Kind::Logic:
        case Type::Kind::Bit:
        case Type::Kind::PackedArray:
            return true;
        case Type::Kind::Struct:
            return ty.is_packed;
        default:
            return false;
    }
}

struct Copy {
    uint32_t process;
    uint32_t target;
    uint32_t source;
};

// The copy `target = source` process `p` is, if it is exactly a continuous
// assignment of one whole local signal to another of the same aliasable type,
// woken by any change of the source alone.
std::optional<Copy> pure_copy(const InternPool& pool, const Unit& u, uint32_t k) {
    const Process& p  = u.processes[k];
    const Ir&      ir = p.ir;
    // self_start: a continuous assignment, which holds from time zero (LRM
    // 10.3.2). An `always @(s) t = s` leaves t at its power-on value until s
    // first changes, so it is not one. always_comb (starts_after_procedures)
    // runs after the initial procedures start (LRM 9.2.2.2), which an alias
    // would anticipate; a program's continuous assignment runs in the Reactive
    // region (reactive_home, LRM 24.3.1), which an alias would leave.
    if (p.activation != Activation::Always || !p.self_start || p.starts_after_procedures || p.reactive_home ||
        p.spawn_root || p.wait_events.size() != 1 || ir.inst_count() != 4) {
        return std::nullopt;
    }
    const WaitEvent& w = p.wait_events[0];
    if (w.edge != Edge::Any || w.handle != kNoWaitHandle) {
        return std::nullopt;
    }
    for (uint32_t j = 0; j < ir.inst_count(); ++j) {
        const auto i = static_cast<Index>(j);
        if (ir.tag(i) != Inst::Tag::Store) {
            continue;
        }
        const Index l = ir.payload(i).binary.l;
        const Index r = ir.payload(i).binary.r;
        if (ir.tag(l) != Inst::Tag::Signal || ir.tag(r) != Inst::Tag::Load ||
            ir.tag(ir.payload(r).unary) != Inst::Tag::Signal) {
            return std::nullopt;
        }
        const auto t = static_cast<uint32_t>(ir.payload(l).signal);
        const auto s = static_cast<uint32_t>(ir.payload(ir.payload(r).unary).signal);
        if (t == s || s != static_cast<uint32_t>(w.signal)) {
            return std::nullopt;
        }
        const UnitSignal& ts = u.signals[t];
        const UnitSignal& ss = u.signals[s];
        if (ts.import != kLocal || ss.import != kLocal || ts.info.is_event || ss.info.is_event ||
            ts.info.type != ss.info.type || !aliasable_type(pool, ts.info.type)) {
            return std::nullopt;
        }
        return Copy{ k, t, s };
    }
    return std::nullopt;
}

// Path-halving find over a unit's signal classes.
uint32_t find(std::vector<uint32_t>& parent, uint32_t h) {
    while (parent[h] != h) {
        parent[h] = parent[parent[h]];
        h         = parent[h];
    }
    return h;
}

} // namespace

void alias_copies(Design& design) {
    const InternPool& pool = *design.pool;
    std::vector<std::vector<uint32_t>> writes(design.units.size());
    for (uint32_t ui = 0; ui < design.units.size(); ++ui) {
        const Unit& u = design.units[ui];
        writes[ui].assign(u.signals.size(), 0);
        for (const Process& p : u.processes) {
            count_writes(p.ir, writes[ui]);
        }
        for (const UnitFunction& f : u.functions) {
            if (f.import == kLocal) {
                count_writes(f.fn.ir, writes[ui]);
            }
        }
    }

    // The members other units write through an import, by unit-relative name.
    // A name rather than a (unit, handle) pair because resolving an import needs
    // the instance tree; sharing a name with a written member of an unrelated
    // unit only costs an alias. A layout import (a virtual interface's member)
    // may be written through any handle at run time, so it blocks outright.
    ankerl::unordered_dense::set<std::string> written_names;
    for (uint32_t ui = 0; ui < design.units.size(); ++ui) {
        const Unit& u = design.units[ui];
        for (uint32_t h = 0; h < u.signals.size(); ++h) {
            if (u.signals[h].import == kLocal) {
                continue;
            }
            const Import& imp = u.imports[u.signals[h].import];
            if (imp.kind == Import::Kind::Representative ||
                (imp.kind == Import::Kind::Member && writes[ui][h] != 0)) {
                written_names.insert(imp.member);
            }
        }
    }

    std::vector<uint8_t> is_vif(design.units.size(), 0);
    for (const UnitId v : design.vif_units) {
        is_vif[static_cast<uint32_t>(v)] = 1;
    }

    std::vector<uint32_t> parent;
    std::vector<uint32_t> class_writes;
    std::vector<uint8_t>  drop;
    for (uint32_t ui = 0; ui < design.units.size(); ++ui) {
        Unit& u = design.units[ui];
        if (u.kind != UnitKind::Module || is_vif[ui] != 0) {
            continue;
        }
        const size_t n = u.signals.size();
        // A signal is writable outside this unit's bodies when another unit
        // writes it by name or a child port binds it (an inout, ref or
        // contribution the child stores into).
        class_writes = writes[ui];
        for (uint32_t h = 0; h < n; ++h) {
            if (u.signals[h].import == kLocal && written_names.contains(u.signals[h].info.name)) {
                class_writes[h] = kMany;
            }
        }
        for (const ChildSlot& c : u.children) {
            const Unit& cu = design.unit(c.unit);
            for (uint32_t b = 0; b < c.bindings.size(); ++b) {
                if (cu.bound_ports[b].kind == BoundPort::Kind::Signal) {
                    class_writes[c.bindings[b]] = kMany;
                }
            }
        }
        std::vector<uint8_t> flushes(u.processes.size(), 0);
        for (const UnitDisableTarget& t : u.disable_targets) {
            if (t.import == kLocal && t.flush_proc != Module::no_flush_proc) {
                flushes[t.flush_proc] = 1;
            }
        }

        // Classes of signals sharing one storage, rooted at the member that
        // keeps it; class_writes of a root counts the non-read uses of every
        // member. A port is never made an alias: the instantiating parent and the
        // flat scope tree both name it as a port of this instance.
        parent.resize(n);
        for (uint32_t h = 0; h < n; ++h) {
            parent[h] = h;
        }
        const auto is_port = [&](uint32_t h) { return u.signals[h].info.dir != SignalInfo::Direction::Internal; };
        drop.assign(u.processes.size(), 0);
        bool any = false;
        for (uint32_t k = 0; k < u.processes.size(); ++k) {
            const std::optional<Copy> c = flushes[k] != 0 ? std::nullopt : pure_copy(pool, u, k);
            if (!c) {
                continue;
            }
            const uint32_t rt = find(parent, c->target);
            const uint32_t rs = find(parent, c->source);
            // The copy's own store is the target class's only write, so once
            // the copy is gone the class holds the source's value at every
            // instant. A cycle of copies (rt == rs) keeps its processes.
            if (rt == rs || class_writes[rt] != 1) {
                continue;
            }
            if (!is_port(rt)) {
                // The target's names move onto the source's storage.
                parent[rt] = rs;
            } else if (!is_port(rs) && class_writes[rs] != kMany &&
                       u.signals[rs].info.is_net == u.signals[rt].info.is_net) {
                // The source's names, and its writers, move onto the target's
                // storage. A writer of the source writes the target's storage
                // instead, so the storage's power-on value (undriven: z for a
                // net, x for a variable) must be the source's: hence is_net.
                parent[rs]       = rt;
                class_writes[rt] = class_writes[rs];
            } else {
                continue;
            }
            drop[k] = 1;
            any     = true;
        }
        if (!any) {
            continue;
        }

        for (uint32_t h = 0; h < n; ++h) {
            const uint32_t r = find(parent, h);
            if (r != h) {
                u.signals[h].alias = r;
            }
        }
        remove_processes(u, drop);
    }
}

} // namespace clotho::ir
