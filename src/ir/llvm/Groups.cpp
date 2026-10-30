// Fusion groups, formed per unit, and the schedule groups that let the groups of
// several instances share one subscription.

#include "ir/llvm/Backend.h"

#include <functional>
#include <queue>
#include <set>

namespace clotho::ir::backend {

namespace {

// A process's (signal, edge) events as a sorted, deduplicated list: what two
// processes must agree on to share a wake set.
using EventSet = std::vector<std::pair<uint32_t, uint32_t>>;

EventSet canonical_events(const std::vector<WaitEvent>& evs) {
    EventSet key;
    key.reserve(evs.size());
    for (const WaitEvent& w : evs) {
        key.emplace_back(static_cast<uint32_t>(w.signal), static_cast<uint32_t>(w.edge));
    }
    std::sort(key.begin(), key.end());
    key.erase(std::unique(key.begin(), key.end()), key.end());
    return key;
}

// A fusion group of a unit: def-local process ordinals in the order the group
// runs them. The same list instantiates in every instance of the unit.
struct UnitGroup {
    std::vector<uint32_t> ords;
    // Formed by comb fusion (more than one member, different sensitivities).
    bool comb = false;
};

// The processes of one unit's instances, by def-local ordinal, and what the
// groups of the unit are. A process that belongs to no instance forms a unit of
// one pseudo-instance (anchor no_anchor).
struct Unit {
    std::vector<uint32_t>              anchors;
    std::vector<std::vector<uint32_t>> procs; // [instance][ordinal] -> flat process index
    std::vector<UnitGroup>             ug;
    std::vector<uint32_t>              ug_of;  // ordinal -> index into ug
    std::vector<uint8_t>               thunk;  // ordinal -> lowers into a thunk
    std::vector<uint8_t>               cand;   // ordinal -> may join a comb group
    std::vector<std::vector<uint32_t>> adj;    // wake edges between ordinals, over every instance
};

// The nodes of `adj` (over 0..n-1, restricted to `live`) that sit on a cycle: a
// strongly connected component of more than one node. Iterative Tarjan.
std::vector<uint8_t> cyclic_nodes(uint32_t n, const std::vector<std::vector<uint32_t>>& adj,
                                  const std::vector<uint8_t>& live) {
    constexpr uint32_t    none = UINT32_MAX;
    std::vector<uint32_t> index(n, none);
    std::vector<uint32_t> low(n, 0);
    std::vector<uint8_t>  on_stack(n, 0);
    std::vector<uint8_t>  cyclic(n, 0);
    std::vector<uint32_t> stack;
    uint32_t              next_index = 0;
    struct Frame {
        uint32_t v;
        uint32_t child;
    };
    std::vector<Frame> dfs;
    for (uint32_t root = 0; root < n; ++root) {
        if (live[root] == 0 || index[root] != none) {
            continue;
        }
        dfs.push_back({ root, 0 });
        index[root] = low[root] = next_index++;
        stack.push_back(root);
        on_stack[root] = 1;
        while (!dfs.empty()) {
            Frame& f = dfs.back();
            if (f.child < adj[f.v].size()) {
                const uint32_t w = adj[f.v][f.child++];
                if (live[w] == 0) {
                    continue;
                }
                if (index[w] == none) {
                    index[w] = low[w] = next_index++;
                    stack.push_back(w);
                    on_stack[w] = 1;
                    dfs.push_back({ w, 0 });
                }
                else if (on_stack[w] != 0) {
                    low[f.v] = std::min(low[f.v], index[w]);
                }
                continue;
            }
            const uint32_t v = f.v;
            dfs.pop_back();
            if (!dfs.empty()) {
                low[dfs.back().v] = std::min(low[dfs.back().v], low[v]);
            }
            if (low[v] == index[v]) {
                size_t from = stack.size();
                while (stack[from - 1] != v) {
                    --from;
                }
                --from;
                const bool multi = stack.size() - from > 1;
                for (size_t i = from; i < stack.size(); ++i) {
                    on_stack[stack[i]] = 0;
                    cyclic[stack[i]]   = multi ? 1 : 0;
                }
                stack.resize(from);
            }
        }
    }
    return cyclic;
}

// The longest-path level of each live node of the DAG `adj`: how many producers
// deep it sits, so running nodes by ascending level (ties by ordinal) runs every
// producer before its consumers.
std::vector<uint32_t> dag_levels(uint32_t n, const std::vector<std::vector<uint32_t>>& adj,
                                 const std::vector<uint8_t>& live) {
    std::vector<uint32_t> indeg(n, 0);
    std::vector<uint32_t> level(n, 0);
    for (uint32_t v = 0; v < n; ++v) {
        if (live[v] == 0) {
            continue;
        }
        for (const uint32_t w : adj[v]) {
            indeg[w] += live[w] != 0 ? 1 : 0;
        }
    }
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
    for (uint32_t v = 0; v < n; ++v) {
        if (live[v] != 0 && indeg[v] == 0) {
            ready.push(v);
        }
    }
    while (!ready.empty()) {
        const uint32_t v = ready.top();
        ready.pop();
        for (const uint32_t w : adj[v]) {
            if (live[w] == 0) {
                continue;
            }
            level[w] = std::max(level[w], level[v] + 1);
            if (--indeg[w] == 0) {
                ready.push(w);
            }
        }
    }
    return level;
}

} // namespace

// Fusion groups. A group is ONE function lowered from a unit's code and run
// with an instance's record, so which processes fuse, in what order, and whether
// the group is a thunk are decided on the unit, never on an instance's context:
// every instance of a unit gets the same groups, in the same order, from the
// same ordinals. A group never mixes instances. What differs between instances
// (which global signal a port is bound to, what drives it, what sits in a cycle
// through the parent) enters as a fact joined over them: processes fuse only if
// they would in EVERY instance, and are left alone if they should not in ANY.
// With one instance that is exactly the decision of the instance's own context.
//
// V1 static process fusion: processes with an identical canonical sensitivity set
// and self_start flag run as one group, so the fused wake set equals every
// member's own wake set -- each body still runs exactly when its own trigger
// fires, and activation counts are unchanged by construction (no
// over-evaluation possible). Running members back-to-back fixes one of the
// LRM-allowed same-instant orders (members run in the unit's order: by ordinal).
//
// A process is fusable only when:
//  - it is an Always with a static sensitivity list;
//  - its body has no suspension point (no #delay, no intra-body @/wait, no
//    task call — a suspended fused body would stall its siblings);
//  - it does not blocking-write a signal in its own sensitivity set (the
//    write would re-wake an already-ran sibling in the unfused schedule,
//    but lands mid-body under the group's held sched flag when fused).
//
// Comb fusion merges dependent comb-eligible singletons within an instance, in
// producer-before-consumer order, to run in one activation on the union
// sensitivity (docs/design/comb-fusion.md has the LRM as-if argument). Two
// facts carry the legality: the sched flag is held across the fused body, so an
// intra-group store cannot re-enqueue the group (the downstream member already
// runs later in the same activation with final values); and change detection
// makes a spuriously-run member (pure, reads unchanged) write no changes and wake
// nobody. Groups are connected components of the wake graph so independent cones
// are never yoked together (a union sensitivity across unrelated cones would
// run every body per trigger); nontrivial SCCs (comb loops) keep re-wake fixpoint
// iteration. The wake graph is the union, over the instances, of the edges
// between processes of one instance (a wake edge to another instance's process
// is an ordinary wake: the groups are scheduled normally), so a member order
// that is topological for it is topological in every instance.
//
// Then the groups of different instances that wait on the same events become
// the rows of one schedule group (build_sched_groups).
void Backend::compute_fusion_groups() {
    const auto all    = mod_.processes();
    const auto nprocs = static_cast<uint32_t>(all.size());
    groups_.clear();
    group_of_.assign(nprocs, 0);
    group_events_.clear();
    group_comb_.clear();
    group_unit_.clear();
    group_unit_ordinal_.clear();
    group_unit_anchor_.clear();
    thunk_group_.clear();
    comb_group_sizes_.clear();

    // Position of each process in sub_order_ (its scheduled rank).
    std::vector<uint32_t> pos(nprocs, 0);
    for (uint32_t i = 0; i < nprocs; ++i) {
        pos[sub_order_[i]] = i;
    }

    // Whether process k can lower into a thunk group: a suspension-free
    // always-with-sensitivity (is_simple_always) the thunk lowering may take.
    //
    // A member that can reach a $urandom draw is excluded whatever its shape:
    // a thunk has no frame to keep the xoshiro state in, so its stream would
    // restart on every activation. is_simple_always already rejects a body
    // holding a draw ITSELF (tag_info().blocks_thunk); this covers the ones
    // that reach one through a call.
    //
    // A reactive-home process is excluded too, for a tagged-pointer reason: its
    // static subscription has to carry kReactiveHomeTag (bit 1) so the wake
    // path picks the reactive ring, and a thunk "handle" is a function pointer
    // carrying the thunk tag (bit 0). The two tags are mutually exclusive by
    // construction -- see kReactiveHomeTag -- so a pointer holding both would
    // read as neither reactive (the test is bit 1 set AND bit 0 clear) nor as a
    // maskable thunk, and the drain would call two bytes past the function.
    // Costs a coroutine frame for each of a program's continuous assigns, a
    // handful of processes in a testbench; the thunk path exists for wide
    // combinational designs, which are Active-home by definition.
    //
    // The verdict is part of every fusion key below, so a group is a thunk
    // exactly when its members are.
    const auto thunkable = [&](uint32_t k) {
        return config_.thunk && facts_.processes[k].simple_always && !all[k].reactive_home && !rng_procs_[k];
    };

    // Group size is capped so one fused function cannot grow arbitrarily large
    // (function size / frame size / compile time). Rows of a schedule group are
    // not capped: they are calls, not code.
    constexpr size_t group_cap = 256;

    // -- The units: which processes are the same process in each instance.
    //
    // Flatten expands a unit's processes into every instance in the unit's own
    // order, so a process is named by its def-local ordinal (the index in the
    // instance's list, which is also its flag's index in the instance's sched
    // array). The processes of no instance are one pseudo-instance.
    std::vector<Unit> units;
    {
        std::vector<std::vector<uint32_t>> by_anchor(anchors_.size());
        std::vector<uint32_t>              of_none;
        for (uint32_t k = 0; k < nprocs; ++k) {
            const uint32_t a = proc_sched_anchor_[k];
            (a == SignalSlot::no_anchor ? of_none : by_anchor[a]).push_back(k);
        }
        ankerl::unordered_dense::map<uint32_t, uint32_t> of_design_unit;
        for (uint32_t a = 0; a < anchors_.size(); ++a) {
            if (by_anchor[a].empty()) {
                continue;
            }
            const uint32_t du = anchors_[a].unit;
            uint32_t       idx;
            if (du == InstanceScope::no_unit) {
                idx = static_cast<uint32_t>(units.size());
                units.emplace_back();
            }
            else {
                const auto [it, fresh] = of_design_unit.try_emplace(du, static_cast<uint32_t>(units.size()));
                if (fresh) {
                    units.emplace_back();
                }
                idx = it->second;
            }
            units[idx].anchors.push_back(a);
            units[idx].procs.push_back(std::move(by_anchor[a]));
        }
        if (!of_none.empty()) {
            units.emplace_back();
            units.back().anchors.push_back(SignalSlot::no_anchor);
            units.back().procs.push_back(std::move(of_none));
        }
    }
    // Instances of one unit own the same processes, shaped alike (partition
    // merges them only when their IR is identical). A unit whose instances
    // disagree anyway -- which nothing should produce -- is formed instance by
    // instance instead of trusting its ordinals.
    {
        const auto same_shape = [&](uint32_t p, uint32_t q) {
            const Process& a = all[p];
            const Process& b = all[q];
            return a.activation == b.activation && a.self_start == b.self_start &&
                   a.reactive_home == b.reactive_home && a.wait_events.size() == b.wait_events.size() &&
                   a.ir.inst_count() == b.ir.inst_count();
        };
        std::vector<Unit> checked;
        for (Unit& u : units) {
            bool uniform = true;
            for (size_t j = 1; j < u.procs.size() && uniform; ++j) {
                uniform = u.procs[j].size() == u.procs[0].size();
                for (size_t o = 0; o < u.procs[j].size() && uniform; ++o) {
                    uniform = same_shape(u.procs[0][o], u.procs[j][o]);
                }
            }
            if (uniform) {
                checked.push_back(std::move(u));
                continue;
            }
            for (size_t j = 0; j < u.procs.size(); ++j) {
                Unit one;
                one.anchors.push_back(u.anchors[j]);
                one.procs.push_back(std::move(u.procs[j]));
                checked.push_back(std::move(one));
            }
        }
        units = std::move(checked);
    }

    // -- V1: bucket fusable processes by (the canonical events in every
    // instance, self_start, reactive_home, thunkable), in ordinal order;
    // everything else is its own group.
    //
    // The events are part of the key per instance, because two ports bound to
    // one signal in one instance and to two in another are one sensitivity in
    // the first and two in the second: they fuse only if they are one in all.
    //
    // reactive_home is part of the key because a group is ONE function pushed
    // onto ONE ring: the seed loop makes a single push decision for it and its
    // static subscription records a single home tag, so fusing a program's
    // continuous assign (LRM 24.3.1, reactive-home) with a design one would
    // run half the group in the wrong region.
    for (Unit& u : units) {
        const auto m = static_cast<uint32_t>(u.procs.front().size());
        u.thunk.assign(m, 0);
        u.cand.assign(m, 0);
        u.ug_of.assign(m, 0);
        std::map<std::tuple<std::vector<uint32_t>, bool, bool, bool>, uint32_t> by_key;
        for (uint32_t o = 0; o < m; ++o) {
            const Process& p0     = all[u.procs.front()[o]];
            bool           simple = config_.fuse;
            bool           thunk  = true;
            for (const auto& pj : u.procs) {
                simple = simple && facts_.processes[pj[o]].simple_always;
                thunk  = thunk && thunkable(pj[o]);
            }
            u.thunk[o] = thunk ? 1 : 0;
            if (simple) {
                std::vector<uint32_t> events;
                for (const auto& pj : u.procs) {
                    for (const auto& [s, e] : canonical_events(all[pj[o]].wait_events)) {
                        events.push_back(s);
                        events.push_back(e);
                    }
                    events.push_back(UINT32_MAX);
                }
                auto       key = std::make_tuple(std::move(events), p0.self_start, p0.reactive_home, thunk);
                const auto it  = by_key.find(key);
                if (it != by_key.end() && u.ug[it->second].ords.size() < group_cap) {
                    u.ug[it->second].ords.push_back(o);
                    u.ug_of[o] = it->second;
                    continue;
                }
                by_key[std::move(key)] = static_cast<uint32_t>(u.ug.size());
            }
            u.ug_of[o] = static_cast<uint32_t>(u.ug.size());
            u.ug.push_back({ { o }, false });
        }
    }

    // -- Comb fusion.
    if (config_.fuse && config_.comb_fuse && nprocs >= 2) {
        // Candidates: a comb-eligible singleton outside every nontrivial SCC
        // (joined: eligible in all instances, on no cycle in any). The wake
        // graph over ordinals is the union, over the instances, of the edges
        // between two processes of one instance; a cycle only the union
        // closes (one instance wires A before B, another B before A) leaves no
        // order that works for both, so those processes stay on the re-wake path
        // too. Edges join processes alike in self_start, reactive_home and
        // thunkability: the group makes one seed decision and has one home
        // ring.
        for (Unit& u : units) {
            const auto m = static_cast<uint32_t>(u.procs.front().size());
            for (uint32_t o = 0; o < m; ++o) {
                if (u.ug[u.ug_of[o]].ords.size() != 1) {
                    continue;
                }
                bool ok = true;
                for (const auto& pj : u.procs) {
                    ok = ok && scc_nontrivial_[pj[o]] == 0 && facts_.processes[pj[o]].comb_fuse == CombFuse::Eligible;
                }
                u.cand[o] = ok ? 1 : 0;
            }
            u.adj.assign(m, {});
            for (size_t j = 0; j < u.procs.size(); ++j) {
                for (uint32_t o = 0; o < m; ++o) {
                    if (u.cand[o] == 0) {
                        continue;
                    }
                    const uint32_t p = u.procs[j][o];
                    for (const uint32_t q : wake_adj_[p]) {
                        if (proc_sched_anchor_[q] != u.anchors[j]) {
                            continue;
                        }
                        const uint32_t o2 = proc_sched_ord_[q];
                        if (u.cand[o2] != 0 && all[q].self_start == all[p].self_start &&
                            all[q].reactive_home == all[p].reactive_home && u.thunk[o2] == u.thunk[o]) {
                            u.adj[o].push_back(o2);
                        }
                    }
                }
            }
            for (auto& a : u.adj) {
                std::sort(a.begin(), a.end());
                a.erase(std::unique(a.begin(), a.end()), a.end());
            }
            const std::vector<uint8_t> cyclic = cyclic_nodes(m, u.adj, u.cand);
            for (uint32_t o = 0; o < m; ++o) {
                u.cand[o] = cyclic[o] != 0 ? 0 : u.cand[o];
            }
        }

        // Interior guard: only members whose ENTIRE sensitivity is
        // candidate-written may merge. A member with a private external
        // trigger (e.g. star fan-out: `assign c_i = s ^ x_i` with hot
        // x_i) would drag the whole group awake on every private change —
        // measured 10.9x slower than unfused on a 256-consumer star.
        // Interior members wake only via changes propagating through the
        // component, which is exactly the wave the fused topo order
        // replaces. Non-interior candidates stay singletons but still
        // provide edges/writes (their stores wake the groups they feed).
        std::vector<uint8_t> cand_writes(static_cast<uint32_t>(mod_.signals().size()), 0);
        for (const Unit& u : units) {
            for (const auto& pj : u.procs) {
                for (uint32_t o = 0; o < pj.size(); ++o) {
                    if (u.cand[o] == 0) {
                        continue;
                    }
                    const Ir& ir = all[pj[o]].ir;
                    for (size_t j = 0; j < ir.inst_count(); ++j) {
                        const auto idx = static_cast<Index>(j);
                        if (ir.tag(idx) != Inst::Tag::Store) {
                            continue;
                        }
                        const LvalueRoot r = lvalue_root(ir, ir.payload(idx).binary.l);
                        if (ir.tag(r.root) == Inst::Tag::Signal) {
                            cand_writes[static_cast<uint32_t>(ir.payload(r.root).signal)] = 1;
                        }
                    }
                }
            }
        }

        for (Unit& u : units) {
            const auto m = static_cast<uint32_t>(u.procs.front().size());
            std::vector<uint8_t> interior(m, 0);
            for (uint32_t o = 0; o < m; ++o) {
                if (u.cand[o] == 0) {
                    continue;
                }
                interior[o] = 1;
                for (const auto& pj : u.procs) {
                    for (const WaitEvent& w : all[pj[o]].wait_events) {
                        if (cand_writes[static_cast<uint32_t>(w.signal)] == 0) {
                            interior[o] = 0;
                        }
                    }
                }
            }
            // Components over all candidates, so two interior members joined
            // through a non-interior producer land in one group.
            std::vector<uint32_t> uf(m);
            for (uint32_t o = 0; o < m; ++o) {
                uf[o] = o;
            }
            const auto find = [&](uint32_t x) {
                while (uf[x] != x) {
                    uf[x] = uf[uf[x]];
                    x     = uf[x];
                }
                return x;
            };
            for (uint32_t o = 0; o < m; ++o) {
                for (const uint32_t o2 : u.adj[o]) {
                    if (u.cand[o] != 0 && u.cand[o2] != 0) {
                        uf[find(o)] = find(o2);
                    }
                }
            }
            const std::vector<uint32_t> level = dag_levels(m, u.adj, u.cand);
            // Component members in producer-before-consumer order, then chunked
            // at the group cap; a leftover chunk of one stays a singleton.
            std::vector<std::vector<uint32_t>> comp(m);
            for (uint32_t o = 0; o < m; ++o) {
                if (interior[o] != 0) {
                    comp[find(o)].push_back(o);
                }
            }
            bool merged = false;
            for (auto& c : comp) {
                std::sort(c.begin(), c.end(), [&](uint32_t a, uint32_t b) {
                    return level[a] != level[b] ? level[a] < level[b] : a < b;
                });
                for (size_t off = 0; off + 1 < c.size(); off += group_cap) {
                    const size_t end = std::min(c.size(), off + group_cap);
                    if (end - off < 2) {
                        break;
                    }
                    UnitGroup g;
                    g.comb = true;
                    for (size_t j = off; j < end; ++j) {
                        u.ug[u.ug_of[c[j]]].ords.clear();
                        g.ords.push_back(c[j]);
                    }
                    u.ug.push_back(std::move(g));
                    merged = true;
                }
            }
            if (merged) {
                std::erase_if(u.ug, [](const UnitGroup& g) { return g.ords.empty(); });
            }
        }
    }

    // -- Instantiate: every instance of a unit gets the unit's groups. Groups are
    // numbered by their first process (declaration order, as the seed loop and
    // the subscription order expect), and a group's number in its unit -- the
    // key the unit-code statistic compares by -- is its position among the unit's
    // groups by first ordinal.
    struct Placed {
        uint32_t              unit;
        uint32_t              ordinal;
        uint32_t              anchor;
        bool                  comb;
        bool                  thunk;
        std::vector<uint32_t> procs;
    };
    std::vector<Placed> placed;
    for (uint32_t ui = 0; ui < units.size(); ++ui) {
        Unit& u = units[ui];
        const auto first_ord = [](const UnitGroup& g) { return *std::min_element(g.ords.begin(), g.ords.end()); };
        std::sort(u.ug.begin(), u.ug.end(),
                  [&](const UnitGroup& a, const UnitGroup& b) { return first_ord(a) < first_ord(b); });
        for (size_t j = 0; j < u.procs.size(); ++j) {
            for (uint32_t t = 0; t < u.ug.size(); ++t) {
                Placed p{ .unit = ui, .ordinal = t, .anchor = u.anchors[j], .comb = u.ug[t].comb,
                          .thunk = u.thunk[u.ug[t].ords.front()] != 0, .procs = {} };
                for (const uint32_t o : u.ug[t].ords) {
                    p.procs.push_back(u.procs[j][o]);
                }
                placed.push_back(std::move(p));
            }
        }
    }
    const auto first_proc = [](const Placed& p) { return *std::min_element(p.procs.begin(), p.procs.end()); };
    std::stable_sort(placed.begin(), placed.end(),
                     [&](const Placed& a, const Placed& b) { return first_proc(a) < first_proc(b); });

    groups_.reserve(placed.size());
    for (Placed& p : placed) {
        const auto g = static_cast<uint32_t>(groups_.size());
        for (const uint32_t k : p.procs) {
            group_of_[k] = g;
        }
        group_unit_.push_back(p.unit);
        group_unit_ordinal_.push_back(p.ordinal);
        group_unit_anchor_.push_back(p.anchor);
        group_comb_.push_back(p.comb);
        thunk_group_.push_back(p.thunk);
        if (p.comb) {
            comb_group_sizes_.push_back(static_cast<uint32_t>(p.procs.size()));
        }
        groups_.push_back(std::move(p.procs));
    }

    // A group subscribes the events of its members: the canonical deduped union
    // for a fused group, the member's raw list for a singleton (V1 groups: the
    // union equals the front member's set; comb groups need the true union).
    group_events_.resize(groups_.size());
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        if (groups_[g].size() == 1) {
            // Only an Always group subscribes (the seed loop in emit_active_run).
            // A Preponed or Observed process may still carry wait_events naming
            // the edge its own body tests for, but it is run explicitly and never
            // woken. wait_counts sizes the static buckets from this list, and the
            // scan walks every slot up to that capacity, so leaving those events
            // here would reserve a slot nothing ever fills and the scan would
            // read a null flag pointer out of it.
            if (all[groups_[g].front()].activation == Activation::Always) {
                group_events_[g] = all[groups_[g].front()].wait_events;
            }
            continue;
        }
        std::vector<WaitEvent> evs;
        for (const uint32_t m : groups_[g]) {
            evs.insert(evs.end(), all[m].wait_events.begin(), all[m].wait_events.end());
        }
        for (const auto& [s, e] : canonical_events(evs)) {
            group_events_[g].push_back(WaitEvent{ static_cast<Signal>(s), static_cast<Edge>(e) });
        }
    }

    build_sched_groups(pos);
}

// The schedule groups: what the seed loop creates one handle for and subscribes
// once. Fusion groups of one or several instances that wait on the very same
// (signal, edge) set (every instance's clocked group on one shared clock) are
// the ROWS of one schedule group: one subscription, one scheduled flag, one
// dispatcher running each row's group function with that row's instance record.
// Every instance of a unit forms the same rows, so which schedule group a unit's
// group lands in is the only thing that varies, and it is link glue.
//
// Only thunk groups share, and only the ones whose members all wait on the same
// events (everything but a comb group): a member of such a group never writes a
// signal of that set, so no row can be woken by another's store, whatever order
// they run in. A comb group's members wake each other by design and are ordered
// for it, within their instance only. It is its own schedule group.
void Backend::build_sched_groups(const std::vector<uint32_t>& pos) {
    const auto all = mod_.processes();
    sched_.clear();
    sched_of_.assign(groups_.size(), 0);
    std::map<std::tuple<EventSet, bool, bool>, uint32_t> shared;
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const Process& front = all[groups_[g].front()];
        if (thunk_group_[g] && !group_comb_[g] && !group_events_[g].empty()) {
            auto       key = std::make_tuple(canonical_events(group_events_[g]), front.self_start, front.reactive_home);
            const auto [it, fresh] = shared.try_emplace(std::move(key), static_cast<uint32_t>(sched_.size()));
            if (!fresh) {
                sched_[it->second].rows.push_back(g);
                sched_of_[g] = it->second;
                continue;
            }
        }
        sched_of_[g] = static_cast<uint32_t>(sched_.size());
        sched_.push_back(SchedGroup{ .rows = { g } });
    }
    // Rows run in scheduled-rank order (the order the unfused FIFO would have
    // popped them); the group subscribes at its earliest member's rank.
    const auto rank = [&](uint32_t g) {
        uint32_t r = UINT32_MAX;
        for (const uint32_t k : groups_[g]) {
            r = std::min(r, pos[k]);
        }
        return r;
    };
    for (SchedGroup& sg : sched_) {
        std::stable_sort(sg.rows.begin(), sg.rows.end(), [&](uint32_t a, uint32_t b) { return rank(a) < rank(b); });
        sg.front = groups_[sg.rows.front()].front();
        sg.thunk = thunk_group_[sg.rows.front()];
        if (sg.rows.size() == 1) {
            sg.events = group_events_[sg.rows.front()];
            continue;
        }
        for (const auto& [s, e] : canonical_events(group_events_[sg.rows.front()])) {
            sg.events.push_back(WaitEvent{ static_cast<Signal>(s), static_cast<Edge>(e) });
        }
    }
}

// Assign trigger bits, and decide which groups get member guards.
//
// A fused group subscribes to the UNION of its members' sensitivities and
// runs every member per activation. That is legal (comb-fusion.md) but it
// over-evaluates: a stimulus touching one branch of a wide component re-runs
// the untouched branch. A guard fixes it exactly, provided the body can tell
// which of its members' own events fired -- which is what the wake word's
// trigger bits carry.
//
// The bit index of a (signal, edge) is a DESIGN-GLOBAL colour, not a
// per-group index. That is the whole trick: a wake site scans a bucket whose
// entries belong to several groups, and per-group indices would force a mask
// field into wait_elem_ty -- memory spent in every design, which that type
// deliberately refuses (see its comment). One colour per (signal, edge)
// makes the mask a function of the pair alone at the scan site.
//
// Colours are assigned greedily, first-fit, so two events of the same group
// can collide once the colour space is crowded. A collision costs PRECISION,
// never correctness: the two events share a bit, so one fires the other's
// members too -- exactly the over-evaluation the guard set out to remove, and
// exactly what the unguarded emission does today. `trig_collisions_` counts
// them for --stat.
//
// Whether a group is guarded is a property of its unit's code, so one verdict
// serves every instance: guarded when its members differ in any of them. The
// colours and masks are the design's and per instance; they reach code only as
// data (the record's mask fields, the buckets' trigger bits).
void Backend::compute_trigger_masks() {
    const auto all   = mod_.processes();
    const auto sigs  = mod_.signals();
    trig_colour_.assign(sigs.size(), { 0, 0, 0 });
    group_guarded_.assign(groups_.size(), false);
    group_member_mask_.assign(groups_.size(), {});
    group_trig_mask_.assign(groups_.size(), 0);
    any_guarded_     = false;
    trig_collisions_ = 0;
    if (!config_.regions) {
        return;
    }

    // Colours 1..63; bit 0 is the queued flag and is never a colour.
    constexpr uint32_t first_colour = 1;
    constexpr uint32_t last_colour  = 63;

    // Identical-sensitivity groups (compute_fusion_groups phase 1) have one
    // sensitivity for the whole group, so every member's mask would be the union
    // and every guard would be vacuously true. Only a group whose members differ
    // -- a comb-fused component -- can gain.
    const auto differ = [&](uint32_t g) {
        const auto& members = groups_[g];
        return members.size() >= 2 && !group_events_[g].empty() &&
               std::any_of(members.begin() + 1, members.end(), [&](uint32_t m) {
                   return all[m].wait_events.size() != all[members.front()].wait_events.size() ||
                          !std::equal(all[m].wait_events.begin(), all[m].wait_events.end(),
                                      all[members.front()].wait_events.begin(),
                                      [](const WaitEvent& x, const WaitEvent& y) {
                                          return x.signal == y.signal && x.edge == y.edge;
                                      });
               });
    };
    std::set<std::pair<uint32_t, uint32_t>> unit_guarded;
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        if (differ(g)) {
            unit_guarded.emplace(group_unit_[g], group_unit_ordinal_[g]);
        }
    }

    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const auto& members = groups_[g];
        if (members.size() < 2 || group_events_[g].empty() ||
            !unit_guarded.contains({ group_unit_[g], group_unit_ordinal_[g] })) {
            continue;
        }

        // Colours already held by this group's events constrain the rest.
        uint64_t used = 0;
        for (const WaitEvent& w : group_events_[g]) {
            const uint8_t c = colour_of(w);
            if (c != 0) {
                if ((used >> c & 1) != 0) {
                    ++trig_collisions_;
                }
                used |= 1ull << c;
            }
        }
        // A component wider than the colour space SHARES colours rather than
        // giving up its guards: n events over 63 bits means about n/63 members
        // per bit, so a change still skips most of the component instead of
        // none of it. Precision degrades with width; it does not fall off a
        // cliff at 64 (measured: a 64-wide fan-out went from 1.00x to 1.2x by
        // sharing instead of bailing).
        uint32_t rotate = first_colour;
        for (const WaitEvent& w : group_events_[g]) {
            if (colour_of(w) != 0) {
                continue;
            }
            uint32_t c = first_colour;
            while (c <= last_colour && (used >> c & 1) != 0) {
                ++c;
            }
            if (c > last_colour) {
                c      = rotate;
                rotate = rotate == last_colour ? first_colour : rotate + 1;
                ++trig_collisions_;
            }
            else {
                used |= 1ull << c;
            }
            trig_colour_[static_cast<uint32_t>(w.signal)][static_cast<uint32_t>(w.edge)] = static_cast<uint8_t>(c);
        }

        // Per-member mask: the bits of the member's OWN events.
        auto& masks = group_member_mask_[g];
        masks.assign(members.size(), 0);
        uint64_t all_bits = 0;
        for (uint32_t i = 0; i < members.size(); ++i) {
            for (const WaitEvent& w : all[members[i]].wait_events) {
                masks[i] |= 1ull << colour_of(w);
            }
            // A member of a fused Always group always has a sensitivity, so
            // its mask is never empty -- an empty one would mean a member
            // that can never run.
            if (masks[i] == 0) {
                throw BackendError{ "internal: fused member with no trigger mask" };
            }
            all_bits |= masks[i];
        }
        group_trig_mask_[g] = all_bits;
        group_guarded_[g]   = true;
        any_guarded_        = true;
    }
}

// This (signal, edge)'s colour, or 0 when it has none.
uint8_t Backend::colour_of(const WaitEvent& w) const {
    const uint32_t i = static_cast<uint32_t>(w.signal);
    return i < trig_colour_.size() ? trig_colour_[i][static_cast<uint32_t>(w.edge)] : 0;
}

// Trigger bits a wake of (sig, edge) must set: the pair's colour, or none
// when no guarded group waits on it.
uint64_t Backend::trig_bits(Signal sig, uint32_t edge) const {
    const uint32_t i = static_cast<uint32_t>(sig);
    if (i >= trig_colour_.size()) {
        return 0;
    }
    const uint8_t c = trig_colour_[i][edge];
    return c == 0 ? 0ull : 1ull << c;
}

// If the wake word carries none of `mask`'s bits, some wake path pushed the
// group without naming a trigger, so set them all and let every member run --
// which makes a missed bit a lost optimization rather than a lost execution, in
// a wake path this wide. It also means each member's guard is a plain test with
// no fallback of its own.
void Backend::emit_trigger_failsafe(llvm::Value* flag, llvm::Value* mask) {
    auto* w    = b_.CreateLoad(sched_word_ty(), flag);
    auto* none = b_.CreateICmpEQ(b_.CreateAnd(w, mask), b_.getInt64(0));
    auto* set  = llvm::BasicBlock::Create(ctx_, "trig.all", fn_);
    auto* go   = llvm::BasicBlock::Create(ctx_, "trig.go", fn_);
    b_.CreateCondBr(none, set, go);
    b_.SetInsertPoint(set);
    b_.CreateStore(b_.CreateOr(w, mask), flag);
    b_.CreateBr(go);
    b_.SetInsertPoint(go);
}

// The function the scheduler holds for each schedule group: the coroutine ramp
// of a coroutine group, and for a thunk group the entry that runs its rows.
//
//   entry(state, wheel):
//     [guarded group: the fail-safe on its trigger bits]
//     row_1(record_1, wheel [, flag]) ... row_n(record_n, wheel [, flag])
//     flag = 0                                  re-arm: ready to be woken
//
// The entry owns the group's scheduled flag, held across every row, so a store
// of any row finds the group queued and cannot re-enqueue it; that is the
// timeline a fused body has always had. A row is a group function taking its
// instance's record, which addresses everything of its own instance through it.
//
// One row's entry sits in its row's unit module, next to a body the inliner may
// fold into it, so a group that shares nothing costs what a function of its own
// always cost. Several rows span units: the dispatcher is link glue in root, and
// its calls are the cost of having no code of one unit inline in another.
std::vector<llvm::Function*> Backend::emit_sched_entries(const std::vector<llvm::Function*>& group_fns) {
    auto*                        ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto*                        fnty   = llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty, ptr_ty }, false);
    std::vector<llvm::Function*> fns(sched_.size(), nullptr);
    for (uint32_t s = 0; s < sched_.size(); ++s) {
        const SchedGroup& sg = sched_[s];
        if (!sg.thunk) {
            fns[s] = group_fns[sg.rows.front()];
            continue;
        }
        const uint32_t g0  = sg.rows.front();
        const uint32_t ua  = group_unit_anchor_[g0];
        const bool     one = sg.rows.size() == 1;
        size_t         procs = 0;
        for (const uint32_t r : sg.rows) {
            procs += groups_[r].size();
        }
        env_ = config_.split && one && ua != SignalSlot::no_anchor ? &unit_env(anchors_[ua]) : root_env_.get();
        std::string name = one && ua != SignalSlot::no_anchor
                             ? "clotho.inst." + anchors_[ua].name + ".p" + std::to_string(proc_sched_ord_[sg.front])
                             : mod_.name + ".p" + std::to_string(sg.front);
        if (procs > 1) {
            name += ".f" + std::to_string(procs);
        }
        fn_ = llvm::Function::Create(fnty, llvm::Function::ExternalLinkage, name + ".thunk", env_->llmod);
        state_ = fn_->getArg(0);
        state_->setName("state");
        wheel_ = fn_->getArg(1);
        wheel_->setName("wheel");
        fn_->addParamAttr(0, llvm::Attribute::NoAlias);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn_));
        own_anchor_ = SignalSlot::no_anchor;
        own_ctx_    = nullptr;
        llvm::Value* const flag = sched_flag_ptr(sg.front);
        sched_ptr_              = flag;

        // Only a single-row group is ever guarded.
        if (group_guarded_[g0]) {
            llvm::Value* mask = nullptr;
            if (ua != SignalSlot::no_anchor) {
                Anchor& a = anchors_[ua];
                mask      = ctx_field(a, a.ctx_gmask_base + proc_sched_ord_[groups_[g0].front()], b_.getInt64Ty());
            }
            else {
                mask = b_.getInt64(group_trig_mask_[g0]);
            }
            emit_trigger_failsafe(flag, mask);
        }
        for (const uint32_t r : sg.rows) {
            const uint32_t aid = group_unit_anchor_[r];
            llvm::Value*   rec = aid != SignalSlot::no_anchor ? static_cast<llvm::Value*>(ensure_ctx(anchors_[aid]))
                                                              : llvm::ConstantPointerNull::get(ptr_ty);
            std::vector<llvm::Value*> args{ rec, wheel_ };
            if (group_guarded_[r]) {
                args.push_back(flag);
            }
            b_.CreateCall(group_fns[r]->getFunctionType(), sym(group_fns[r]), args);
        }
        b_.CreateStore(b_.getInt64(0), flag); // re-arm: ready to be woken
        b_.CreateRetVoid();
        fns[s] = fn_;
    }
    env_ = root_env_.get();
    return fns;
}

} // namespace clotho::ir::backend
