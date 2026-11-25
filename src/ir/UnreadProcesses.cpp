// remove_unread_processes: delete the processes of a unit whose results nothing
// reads (docs/design/modular-compilation.md, "Unread logic").

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Analysis.h"
#include "Design.h"
#include "Ir.h"

namespace clotho::ir {
namespace {

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

// Counts every signal handle a body names, through Ir::rebased, whose switch
// lists every tag: a handle in a side table (a wait edge, a $readmem target, a
// virtual interface member) is seen however a new tag carries it.
class Mentions final : public HandleMapper {
public:
    Mentions(std::vector<uint32_t>& count, std::vector<uint32_t>& touched)
      : count_(count)
      , touched_(touched) {}

    uint32_t map(HandleFamily family, uint32_t handle) override {
        if ((family == HandleFamily::Signal || family == HandleFamily::Layout) && count_[handle]++ == 0) {
            touched_.push_back(handle);
        }
        return handle;
    }

private:
    std::vector<uint32_t>& count_;
    std::vector<uint32_t>& touched_;
};

// One body's signal uses, in the unit's handles.
struct BodyUses {
    // Signals the body reads: loaded, named by a side table, or used as a place
    // by anything but a load, a store's target or a select of one.
    std::vector<uint32_t> reads;
    // Local signals the body stores into with Store / StoreNb.
    std::vector<uint32_t> writes;
    // Whether the body's only effects are those stores: every other
    // instruction is a pure value op (TagInfo::pure) and no loop can keep it
    // from finishing.
    bool store_only = true;
};

class Scanner {
public:
    explicit Scanner(const Unit& u)
      : u_(u)
      , total_(u.signals.size(), 0)
      , direct_(u.signals.size(), 0) {}

    void scan(const Ir& ir, BodyUses& out) {
        out.reads.clear();
        out.writes.clear();
        out.store_only = true;
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto      i = static_cast<Index>(k);
            const Inst::Tag t = ir.tag(i);
            if (t == Inst::Tag::Signal) {
                ++direct_[static_cast<uint32_t>(ir.payload(i).signal)];
            }
            const bool store = t == Inst::Tag::Store || t == Inst::Tag::StoreNb;
            if (t == Inst::Tag::Load) {
                read_root(ir, ir.payload(i).unary, out);
            } else if (store) {
                const Index root = lvalue_root(ir, ir.payload(i).binary.l).root;
                if (ir.tag(root) == Inst::Tag::Signal &&
                    u_.signals[static_cast<uint32_t>(ir.payload(root).signal)].import == kLocal) {
                    out.writes.push_back(static_cast<uint32_t>(ir.payload(root).signal));
                } else if (ir.tag(root) != Inst::Tag::Local) {
                    out.store_only = false;
                }
            } else if ((!tag_info(t).pure && t != Inst::Tag::MemberSelect) || t == Inst::Tag::Loop) {
                // A member select only names a place, like the other selects.
                out.store_only = false;
            }
            ir.for_each_operand(i, [&](Index op) {
                const Inst::Tag ot = ir.tag(op);
                if (ot != Inst::Tag::Signal && !is_select(ot)) {
                    return;
                }
                const bool place = (t == Inst::Tag::Load && op == ir.payload(i).unary) ||
                                   (store && op == ir.payload(i).binary.l) ||
                                   (is_select(t) && op == select_base(ir, i));
                if (!place) {
                    read_root(ir, op, out);
                }
            });
        }
        // Whatever names a signal other than a Signal instruction is a read.
        // Only a body with an impure op can have one: no pure op, Load or Store
        // carries a handle of its own.
        if (!out.store_only) {
            Mentions mentions(total_, touched_);
            (void)ir.rebased(mentions);
            for (const uint32_t h : touched_) {
                if (total_[h] > direct_[h]) {
                    out.reads.push_back(h);
                }
                total_[h] = 0;
            }
            touched_.clear();
        }
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto i = static_cast<Index>(k);
            if (ir.tag(i) == Inst::Tag::Signal) {
                direct_[static_cast<uint32_t>(ir.payload(i).signal)] = 0;
            }
        }
    }

private:
    static void read_root(const Ir& ir, Index place, BodyUses& out) {
        const Index root = lvalue_root(ir, place).root;
        if (ir.tag(root) == Inst::Tag::Signal) {
            out.reads.push_back(static_cast<uint32_t>(ir.payload(root).signal));
        }
    }

    const Unit&           u_;
    std::vector<uint32_t> total_;
    std::vector<uint32_t> direct_;
    std::vector<uint32_t> touched_;
};

bool body_dumps(const Ir& ir) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        if (ir.tag(static_cast<Index>(k)) == Inst::Tag::DumpVars) {
            return true;
        }
    }
    return false;
}

} // namespace

void remove_unread_processes(Design& design) {
    // $dumpvars reads every signal it dumps, and the whole-design form is the
    // only one Clotho implements.
    for (const Unit& u : design.units) {
        for (const Process& p : u.processes) {
            if (body_dumps(p.ir)) {
                return;
            }
        }
        for (const UnitFunction& f : u.functions) {
            if (f.import == kLocal && body_dumps(f.fn.ir)) {
                return;
            }
        }
    }

    // The members other units name, by unit-relative name, read or written: a
    // name rather than a resolved handle because resolving an import needs the
    // instance tree, and a name shared with an unrelated member only keeps
    // that member.
    ankerl::unordered_dense::set<std::string> imported;
    for (const Unit& u : design.units) {
        for (const Import& imp : u.imports) {
            if (imp.kind != Import::Kind::Port) {
                imported.insert(imp.member);
            }
        }
    }
    std::vector<uint8_t> is_vif(design.units.size(), 0);
    for (const UnitId v : design.vif_units) {
        is_vif[static_cast<uint32_t>(v)] = 1;
    }

    BodyUses              uses;
    std::vector<uint8_t>  live_sig;
    std::vector<uint32_t> work;
    for (uint32_t ui = 0; ui < design.units.size(); ++ui) {
        Unit& u = design.units[ui];
        if (u.kind != UnitKind::Module || is_vif[ui] != 0) {
            continue;
        }
        const auto n  = static_cast<uint32_t>(u.signals.size());
        const auto np = static_cast<uint32_t>(u.processes.size());
        // A copy alias names its target's storage.
        const auto canon = [&](uint32_t h) { return u.signals[h].alias == kLocal ? h : u.signals[h].alias; };
        live_sig.assign(n, 0);
        work.clear();
        const auto mark = [&](uint32_t h) {
            h = canon(h);
            if (live_sig[h] == 0) {
                live_sig[h] = 1;
                work.push_back(h);
            }
        };

        // Roots: what something outside the unit's processes can read.
        for (uint32_t h = 0; h < n; ++h) {
            const UnitSignal& s = u.signals[h];
            if (s.import == kLocal &&
                (s.info.dir != SignalInfo::Direction::Internal || imported.contains(s.info.name))) {
                mark(h);
            }
        }
        for (const ChildSlot& c : u.children) {
            const Unit& cu = design.unit(c.unit);
            for (uint32_t b = 0; b < c.bindings.size(); ++b) {
                if (cu.bound_ports[b].kind == BoundPort::Kind::Signal) {
                    mark(c.bindings[b]);
                }
            }
        }
        for (const AssertionMonitor& m : u.monitors) {
            mark(static_cast<uint32_t>(m.clock));
        }
        Scanner scanner(u);
        for (const UnitFunction& f : u.functions) {
            if (f.import != kLocal) {
                continue;
            }
            if (f.fn.takes_iface_ctx) {
                mark(static_cast<uint32_t>(f.fn.ctx_member));
            }
            // A function may be called from anywhere; everything it names is
            // read, stores included.
            scanner.scan(f.fn.ir, uses);
            for (const uint32_t h : uses.reads) {
                mark(h);
            }
            for (const uint32_t h : uses.writes) {
                mark(h);
            }
        }

        std::vector<uint8_t> flushes(np, 0);
        for (const UnitDisableTarget& t : u.disable_targets) {
            if (t.import == kLocal && t.flush_proc != Module::no_flush_proc) {
                flushes[t.flush_proc] = 1;
            }
        }
        // Per removable process, its reads (CSR) and, per signal, the removable
        // processes storing into it. Every other process is live from the start.
        std::vector<uint8_t>               live_proc(np, 0);
        std::vector<uint32_t>              read_off(np + 1, 0);
        std::vector<uint32_t>              read_sig;
        std::vector<std::vector<uint32_t>> writers(n);
        for (uint32_t k = 0; k < np; ++k) {
            const Process& p = u.processes[k];
            scanner.scan(p.ir, uses);
            for (const WaitEvent& w : p.wait_events) {
                if (w.handle == kNoWaitHandle) {
                    uses.reads.push_back(static_cast<uint32_t>(w.signal));
                } else {
                    uses.store_only = false;
                }
            }
            // An initial block, or an always block that waits on something or is
            // a continuous assignment: a body that never finishes is not here, so
            // deleting one cannot turn a hang into progress. Which region a body
            // runs in (reactive_home) and whether it could spawn (spawn_root, a
            // call's question) do not matter to a body with no effect.
            const bool removable = uses.store_only && flushes[k] == 0 &&
                                   (p.activation == Activation::Initial ||
                                    (p.activation == Activation::Always && (!p.wait_events.empty() || p.self_start)));
            if (removable) {
                read_sig.insert(read_sig.end(), uses.reads.begin(), uses.reads.end());
                for (const uint32_t h : uses.writes) {
                    writers[canon(h)].push_back(k);
                }
            } else {
                live_proc[k] = 1;
                for (const uint32_t h : uses.reads) {
                    mark(h);
                }
            }
            read_off[k + 1] = static_cast<uint32_t>(read_sig.size());
        }

        while (!work.empty()) {
            const uint32_t s = work.back();
            work.pop_back();
            for (const uint32_t k : writers[s]) {
                if (live_proc[k] != 0) {
                    continue;
                }
                live_proc[k] = 1;
                for (uint32_t r = read_off[k]; r < read_off[k + 1]; ++r) {
                    mark(read_sig[r]);
                }
            }
        }

        std::vector<uint8_t> drop(np, 0);
        bool                 any = false;
        for (uint32_t k = 0; k < np; ++k) {
            drop[k] = live_proc[k] == 0 ? 1 : 0;
            any     = any || drop[k] != 0;
        }
        if (any) {
            remove_processes(u, drop);
        }
    }
}

} // namespace clotho::ir
