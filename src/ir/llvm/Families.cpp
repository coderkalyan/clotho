// Functions and tasks once per family: the order they are lowered in, what
// decides a family, and how an instance's subroutine becomes the function of
// the instance that lowered the family's code first.
// (docs/design/modular-compilation.md, step 3c)

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

void Backend::direct_callees(const Ir& ir, std::vector<FunctionId>& out) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        const auto i = static_cast<Index>(k);
        inst_callees(ir, i, out);
        switch (ir.tag(i)) {
            case Inst::Tag::StrobePend:
            case Inst::Tag::ReactivePend:
            case Inst::Tag::MonitorSet:
                out.push_back(ir.extra_data<StrobePendInfo>(ir.payload(i).extra).func);
                break;
            default:
                break;
        }
    }
}

// Where every function belongs, and the order they are lowered in: the strongly
// connected components of the call graph, callees before callers (Tarjan's
// algorithm emits a component once everything it reaches is out), the functions
// in id order wherever the graph leaves a choice.
void Backend::plan_subroutines() {
    const auto nf = static_cast<uint32_t>(mod_.function_count());
    subroutine_home_.assign(nf, SubroutineHome{});
    unit_instances_.clear();
    for (uint32_t a = 0; a < anchors_.size(); ++a) {
        const Anchor& an = anchors_[a];
        if (an.unit == InstanceScope::no_unit) {
            continue;
        }
        ++unit_instances_[an.unit];
        for (uint32_t k = 0; k < an.fn_locals.size(); ++k) {
            subroutine_home_[static_cast<uint32_t>(an.fn_locals[k])] = SubroutineHome{ .anchor = a, .ordinal = k };
        }
    }

    std::vector<std::vector<uint32_t>> callees(nf);
    {
        std::vector<FunctionId> named;
        for (uint32_t f = 0; f < nf; ++f) {
            named.clear();
            direct_callees(mod_.function(static_cast<FunctionId>(f)).ir, named);
            std::vector<uint32_t>& out = callees[f];
            out.reserve(named.size());
            for (const FunctionId c : named) {
                out.push_back(static_cast<uint32_t>(c));
            }
            std::ranges::sort(out);
            out.erase(std::ranges::unique(out).begin(), out.end());
        }
    }

    constexpr uint32_t unvisited = ~0u;
    std::vector<uint32_t> index(nf, unvisited);
    std::vector<uint32_t> low(nf, 0);
    std::vector<bool>     on_stack(nf, false);
    std::vector<uint32_t> stack;
    struct Frame {
        uint32_t node;
        size_t   next;
    };
    std::vector<Frame> work;
    uint32_t           counter = 0;
    subroutine_order_.clear();
    subroutine_order_.reserve(nf);
    subroutine_cycle_.assign(nf, 1);
    const auto visit = [&](uint32_t v) {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        on_stack[v] = true;
        work.push_back(Frame{ .node = v, .next = 0 });
    };
    for (uint32_t root = 0; root < nf; ++root) {
        if (index[root] != unvisited) {
            continue;
        }
        visit(root);
        while (!work.empty()) {
            const uint32_t v = work.back().node;
            if (work.back().next < callees[v].size()) {
                const uint32_t w = callees[v][work.back().next++];
                if (index[w] == unvisited) {
                    visit(w);
                } else if (on_stack[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
                continue;
            }
            if (low[v] == index[v]) {
                size_t at = stack.size();
                do {
                    --at;
                } while (stack[at] != v);
                const auto size = static_cast<uint32_t>(stack.size() - at);
                for (size_t k = at; k < stack.size(); ++k) {
                    on_stack[stack[k]]          = false;
                    subroutine_cycle_[stack[k]] = size;
                    subroutine_order_.push_back(stack[k]);
                }
                stack.resize(at);
            }
            work.pop_back();
            if (!work.empty()) {
                low[work.back().node] = std::min(low[work.back().node], low[v]);
            }
        }
    }
    assert(subroutine_order_.size() == nf && "every function is in one component");
}

ModuleEnv* Backend::subroutine_env(FunctionId id) {
    const Function& f = mod_.function(id);
    const uint32_t  a = subroutine_anchor(f);
    return config_.split && a != SignalSlot::no_anchor && !f.is_dpi ? &unit_env(anchors_[a]) : root_env_.get();
}

// The instance's name and the subroutine's own, so a symbol says whose it is in
// a profile. Every module links into one, so a symbol is unique across them:
// two functions of one name (a package's and an instance's, say) get a count.
std::string Backend::subroutine_symbol(const Function& f, std::string_view kind) {
    const uint32_t a    = subroutine_anchor(f);
    std::string    base = a != SignalSlot::no_anchor ? unit_symbol(anchors_[a]) : mod_.name;
    base += kind;
    base += f.name;
    if (verifying_) {
        // A scratch module is never linked, and its code is compared by shape.
        return base;
    }
    const uint32_t n = subroutine_symbols_[base]++;
    return n == 0 ? base : base + "." + std::to_string(n);
}

// What decides the code of subroutine `f`. Two subroutines of one unit's
// instances with equal keys lower alike: the same body over the same layout, the
// same facts of how the design uses it and the same calls. The facts are those
// lowering reads of a function rather than of its body (the home region its
// parks answer to, the random stream it owns, whether a `$exit` in it ends the
// program of its own instance or another's), and a call's one fact of its callee
// (whether it may finish, which decides
// whether a test of rt_finishing follows the call). A callee is named by the
// function it is declared as: callees are settled before their callers, so
// instances whose callee is the one function of a family call it alike, and
// instances whose callees were lowered apart do not match. A call of the
// function itself is one of the family's own.
std::vector<uint64_t> Backend::subroutine_key(uint32_t f, const std::vector<uint32_t>& scc) const {
    const auto           id   = static_cast<FunctionId>(f);
    const Function&      fu   = mod_.function(id);
    const SubroutineHome home = subroutine_home_[f];
    const auto           code = code_key(home.anchor);
    std::vector<uint64_t> key{ anchors_[home.anchor].unit, home.ordinal, code.size() };
    key.insert(key.end(), code.begin(), code.end());
    key.push_back(is_reactive_home_fn(id) ? 1 : 0);
    key.push_back(is_shared_home_fn(id) ? 1 : 0);
    key.push_back(rng_own_[f] && rng_fns_[f] ? 1 : 0);
    key.push_back(program_exit_relation(id, home.anchor));
    std::vector<FunctionId> callees;
    direct_callees(fu.ir, callees);
    for (const FunctionId c : callees) {
        const auto at = std::ranges::find(scc, static_cast<uint32_t>(c));
        if (at != scc.end()) {
            // A member of the cycle f is in is settled with it: named by its place in it.
            key.push_back((uint64_t{ 1 } << 40) | static_cast<uint64_t>(at - scc.begin()));
        } else {
            key.push_back(c == id ? 0 : reinterpret_cast<uintptr_t>(fn_decls_[static_cast<uint32_t>(c)]));
        }
        key.push_back(callee_may_finish(c) ? 1 : 0);
    }
    return key;
}

// What decides the code of group `g` besides its unit and position: the key of
// its instance's bindings, and the functions its processes call, as declared.
std::vector<uint64_t> Backend::group_key(uint32_t g, uint32_t anchor) const {
    std::vector<uint64_t> key = code_key(anchor);
    std::vector<FunctionId> callees;
    for (const uint32_t p : groups_[g]) {
        direct_callees(mod_.processes()[p].ir, callees);
    }
    key.push_back(callees.size());
    for (const FunctionId c : callees) {
        key.push_back(reinterpret_cast<uintptr_t>(fn_decls_[static_cast<uint32_t>(c)]));
    }
    return key;
}

void Backend::lower_subroutine(FunctionId id) {
    if (mod_.function(id).is_task) {
        lower_task(id);
    } else {
        lower_function(id);
    }
}

// Lower every function and task, callees first. The ones of an instance of a
// unit that has several are shared: the first whose key is new is lowered, in
// its unit's module, with a taint watching what it bakes of its instance; every
// later one with the same key is that function when the lowering baked nothing
// (and an instance that did bake something lowers its own). Every other
// subroutine is lowered as it always was, apart from living in its unit's module.
void Backend::lower_subroutines() {
    for (size_t oi = 0; oi < subroutine_order_.size(); ++oi) {
        const uint32_t f = subroutine_order_[oi];
        if (subroutine_cycle_[f] > 1) {
            // The members of a cycle are consecutive in the order.
            std::vector<uint32_t> members(subroutine_order_.begin() + static_cast<std::ptrdiff_t>(oi),
                                          subroutine_order_.begin() + static_cast<std::ptrdiff_t>(oi + subroutine_cycle_[f]));
            oi += members.size() - 1;
            lower_cycle(std::move(members));
            continue;
        }
        const auto      id   = static_cast<FunctionId>(f);
        const Function& fu   = mod_.function(id);
        const uint32_t  anch = subroutine_home_[f].anchor;
        if (fu.is_dpi) {
            continue; // a declaration of native code: nothing to lower
        }
        env_ = subroutine_env(id);
        const bool many = anch != SignalSlot::no_anchor && unit_instances_.at(anchors_[anch].unit) >= 2;
        if (!many || !config_.dedup) {
            lower_subroutine(id);
            continue;
        }
        auto       key = subroutine_key(f);
        const auto it  = subroutine_families_.find(key);
        if (it != subroutine_families_.end() && it->second.taint == nullptr) {
            SharedSubroutine& rep = it->second;
            if (config_.verify_shared_code) {
                verify_shared_subroutine(f, rep);
            }
            // Nothing names the declaration the function was given up front:
            // its callers come after it, and a callee's own were settled first.
            assert(fn_decls_[f]->use_empty() && "a subroutine is settled before anything calls it");
            fn_decls_[f]->eraseFromParent();
            fn_decls_[f] = rep.fn;
            shared_instances_.emplace_back(anch, subroutine_home_[rep.f].anchor);
            adopt_shared_blocks(rep, anch);
            continue;
        }
        if (it != subroutine_families_.end()) {
            // The family's first baked something of its instance, so this one
            // does too: lowered for itself.
            lower_subroutine(id);
            instance_fns_.insert(fn_decls_[f]);
            ++unshared_subroutines_[it->second.taint];
            continue;
        }
        share_active_       = true;
        taint_why_          = nullptr;
        const auto block_lo = static_cast<uint32_t>(block_commits_.size());
        const auto fill_lo  = ctx_blk_fills_.size();
        lower_subroutine(id);
        share_active_ = false;
        if (taint_why_ != nullptr) {
            instance_fns_.insert(fn_decls_[f]);
            ++unshared_subroutines_[taint_why_];
        }
        SharedSubroutine first{ f, fn_decls_[f], taint_why_, {} };
        note_blocks(first, block_lo, fill_lo);
        subroutine_families_.emplace(std::move(key), std::move(first));
    }
    env_ = root_env_.get();
}

// Lower a cycle of subroutines (a strongly connected component of the call graph).
// The members of one instance of a unit that has several are a family together when
// they all belong to that instance: the key of the cycle is its members' keys in
// the unit's order, each naming the members it calls by their place in the cycle,
// so a cycle lowered once is the cycle of every instance whose key matches. The
// first of a key is lowered with a taint watching the whole cycle; a later one is
// those functions when nothing was baked. Any other cycle (a member of another
// instance, a unit of one instance) is lowered as it always was, member by member.
void Backend::lower_cycle(std::vector<uint32_t> members) {
    const auto anchor_of = [&](uint32_t m) { return subroutine_home_[m].anchor; };
    const uint32_t anch  = anchor_of(members.front());
    bool           share = config_.dedup && anch != SignalSlot::no_anchor && unit_instances_.at(anchors_[anch].unit) >= 2;
    for (const uint32_t m : members) {
        share = share && anchor_of(m) == anch && !mod_.function(static_cast<FunctionId>(m)).is_dpi;
    }
    if (!share) {
        for (const uint32_t m : members) {
            const auto id = static_cast<FunctionId>(m);
            if (mod_.function(id).is_dpi) {
                continue;
            }
            env_ = subroutine_env(id);
            lower_subroutine(id);
            if (anchor_of(m) != SignalSlot::no_anchor && unit_instances_.at(anchors_[anchor_of(m)].unit) >= 2) {
                instance_fns_.insert(fn_decls_[m]);
                ++unshared_subroutines_["calls itself through subroutines of another instance"];
            }
        }
        env_ = root_env_.get();
        return;
    }
    std::ranges::sort(members, {}, [&](uint32_t m) { return subroutine_home_[m].ordinal; });
    std::vector<uint64_t> key{ ~uint64_t{ 0 }, members.size() };
    for (const uint32_t m : members) {
        const auto k = subroutine_key(m, members);
        key.push_back(k.size());
        key.insert(key.end(), k.begin(), k.end());
    }
    env_ = subroutine_env(static_cast<FunctionId>(members.front()));
    const auto it = cycle_families_.find(key);
    if (it != cycle_families_.end() && it->second.front().taint == nullptr) {
        std::vector<SharedSubroutine>& rep = it->second;
        for (size_t k = 0; k < members.size(); ++k) {
            assert(fn_decls_[members[k]]->use_empty() && "a cycle is settled before anything outside it calls it");
            fn_decls_[members[k]]->eraseFromParent();
            fn_decls_[members[k]] = rep[k].fn;
            shared_instances_.emplace_back(anch, subroutine_home_[rep[k].f].anchor);
        }
        if (config_.verify_shared_code) {
            for (size_t k = 0; k < members.size(); ++k) {
                verify_shared_subroutine(members[k], rep[k]);
            }
        }
        for (size_t k = 0; k < members.size(); ++k) {
            adopt_shared_blocks(rep[k], anch);
        }
        env_ = root_env_.get();
        return;
    }
    const bool fresh = it == cycle_families_.end();
    if (fresh) {
        share_active_ = true;
        taint_why_    = nullptr;
    }
    std::vector<SharedSubroutine> rep;
    for (const uint32_t m : members) {
        const auto block_lo = static_cast<uint32_t>(block_commits_.size());
        const auto fill_lo  = ctx_blk_fills_.size();
        lower_subroutine(static_cast<FunctionId>(m));
        SharedSubroutine s{ m, fn_decls_[m], nullptr, {} };
        note_blocks(s, block_lo, fill_lo);
        rep.push_back(std::move(s));
    }
    const char* why = fresh ? taint_why_ : it->second.front().taint;
    share_active_   = false;
    if (why != nullptr) {
        for (const uint32_t m : members) {
            instance_fns_.insert(fn_decls_[m]);
            ++unshared_subroutines_[why];
        }
    }
    if (fresh) {
        for (SharedSubroutine& s : rep) {
            s.taint = why;
        }
        cycle_families_.emplace(std::move(key), std::move(rep));
    }
    env_ = root_env_.get();
}

} // namespace clotho::ir::backend
