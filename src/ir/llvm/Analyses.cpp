// Design analyses run by the backend: reachability, fusion groups, subscription order, reactive and program closures.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Append the byte offset of every class-handle word in `type`'s INLINE storage
// to `out`, `base` being the offset of that storage. Covers a bare handle, and
// handles nested inside fixed unpacked arrays and unpacked structs; it does not
// descend through a dynamic array / queue / associative array, whose elements
// live in a separate heap buffer this table cannot address. Those elements ARE
// traced since P4, by their own rt_elem_ops (`handles` / `inner`, elem_ops_for),
// reached either from a container root table or from an owning object's map
// leaves (docs/design/garbage-collection.md §3.3) -- so a container handle is a
// BOUNDARY here, not a hole. reaches_class_handle answers the transitive
// question.
//
// A class handle is Kind::Class here, which is precisely what keeps a `chandle`
// out of the table: chandle lowers to a plain unsigned 64-bit integer type
// (lower/Types.cpp, lower_type) and never reaches the Class arm. Tracing one would hand
// the collector a foreign DPI pointer, which it never allocated
// (docs/design/garbage-collection.md §0, "Never in scope").
//
// Explicit worklist, never recursion (TigerStyle): the storage tree of an
// array-of-struct-of-array property is arbitrarily deep.
void Backend::collect_class_handles(Ref type, uint64_t base, llvm::Type* storage, std::vector<uint32_t>& out) {
    // Every step pops one node of a finite storage tree, so the walk terminates
    // for any type the frontend can build; the cap turns a malformed type graph
    // (a struct transitively containing itself BY VALUE) into a loud crash
    // rather than a hang (TigerStyle).
    [[maybe_unused]] static constexpr uint32_t kWalkStepsMax = 1u << 24;
    struct Node {
        Ref         type;
        uint64_t    base;
        llvm::Type* storage;
    };
    assert(storage != nullptr && "a storage mirror is required to read offsets");
    const llvm::DataLayout& dl = env_->llmod.getDataLayout();
    std::vector<Node>       work{ { type, base, storage } };
    [[maybe_unused]] uint32_t steps = 0;
    while (!work.empty()) {
        ++steps;
        assert(steps < kWalkStepsMax && "class storage walk did not converge");
        const Node  n = work.back();
        work.pop_back();
        const Type& t = std::get<Type>(mod_.pool.get(n.type));
        if (t.kind == Type::Kind::Class) {
            assert(n.base % 8 == 0 && "a class handle word is 8-byte aligned");
            out.push_back(static_cast<uint32_t>(n.base));
            continue;
        }
        if (t.kind == Type::Kind::UnpackedArray) {
            auto*          arr    = llvm::cast<llvm::ArrayType>(n.storage);
            llvm::Type*    et     = arr->getElementType();
            const uint64_t stride = dl.getTypeAllocSize(et);
            for (uint64_t k = 0; k < arr->getNumElements(); ++k) {
                work.push_back({ t.element, n.base + k * stride, et });
            }
            continue;
        }
        if (t.kind == Type::Kind::Struct && !t.is_packed) {
            auto*       st = llvm::cast<llvm::StructType>(n.storage);
            const auto* sl = dl.getStructLayout(st);
            uint32_t    f  = 0;
            for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
                work.push_back({ fld.type, n.base + sl->getElementOffset(f), st->getElementType(f) });
                ++f;
            }
        }
        // Anything else -- a scalar, a real, a string, a container handle -- owns
        // no inline class handle. A container's ELEMENTS may, and they are traced
        // through that container's own rt_elem_ops rather than from here.
    }
}

// Whether a value of `type` can transitively REACH a class handle: one in its own
// inline storage, one in the elements of a container it holds at any nesting
// depth, or -- for an associative array -- one in its KEY, which LRM 7.8.3 makes
// a strong reference.
//
// This is the collector's REACHABILITY question, and it is deliberately wider
// than the storage question collect_class_handles answers. Since P4 a container
// element may be a class handle, so a frame slot holding a queue of handles holds
// a reference with no handle word inline anywhere
// (docs/design/garbage-collection.md §3.3). It backs the frame-registration
// predicate (frame_may_hold_gc_ref) and the container-root selection in both root
// paths, which is why a false positive here costs only a needless registration
// while a false negative is a use-after-free.
//
// A Class node answers true WITHOUT descending into its properties: an object's
// own fields are traced from its rt_gc_map, not from whoever holds the handle.
// That is also exactly what makes the walk terminate on the self-referential
// `class C; C next; endclass`.
//
// A pure TYPE walk, unlike holds_class_handle before it, so it never asks
// agg_storage_ty for a storage mirror and never mints a StructType to answer a
// predicate. Memoized anyway, because the frame predicate asks once per local of
// every body; keyed by the Ref's u32, as class_body_ty_ is.
//
// Explicit worklist, never recursion (TigerStyle): a queue of dynamic
// arrays of structs of queues nests arbitrarily deep.
bool Backend::reaches_class_handle(Ref type) {
    assert(type != Ref::Invalid && "reachability needs a real type");
    const uint32_t key = static_cast<uint32_t>(type);
    if (const auto it = reaches_class_handle_.find(key); it != reaches_class_handle_.end()) {
        return it->second;
    }
    // Every step pops one node of the type DAG and a Class node never expands, so
    // the walk terminates for any type the frontend can build; the cap turns a
    // malformed cyclic type graph into a loud crash rather than a hang
    // (TigerStyle).
    [[maybe_unused]] static constexpr uint32_t kWalkStepsMax = 1u << 20;
    std::vector<Ref>          work{ type };
    [[maybe_unused]] uint32_t steps = 0;
    bool                      found = false;
    while (!work.empty() && !found) {
        ++steps;
        assert(steps < kWalkStepsMax && "type reachability walk did not converge");
        const Ref   r = work.back();
        work.pop_back();
        const Type& t = std::get<Type>(mod_.pool.get(r));
        switch (t.kind) {
            case Type::Kind::Class:
                found = true;
                break;
            case Type::Kind::UnpackedArray:
            case Type::Kind::DynArray:
            case Type::Kind::Queue:
                work.push_back(t.element);
                break;
            case Type::Kind::Assoc:
                work.push_back(t.element);
                // `fields_id` stashes the interned KEY type and `right` flags a
                // wildcard, whose key type is a canonical unsigned integral
                // placeholder and so never a handle (InternPool.h:114-122,
                // lower/Types.cpp, lower_type).
                if (t.right == 0) {
                    work.push_back(static_cast<Ref>(t.fields_id));
                }
                break;
            case Type::Kind::Struct:
                // A PACKED struct / union is one bit vector (LRM 7.2.1), and a
                // class handle is not integral, so only the unpacked form can
                // hold one.
                if (!t.is_packed) {
                    for (const Field& fld : mod_.pool.struct_fields(t.fields_id)) {
                        work.push_back(fld.type);
                    }
                }
                break;
            default:
                break; // scalar, real, string, event, chandle, virtual interface
        }
    }
    assert((std::get<Type>(mod_.pool.get(type)).kind != Type::Kind::Class || found) &&
           "a class-typed value is itself a reference");
    reaches_class_handle_.emplace(key, found);
    return found;
}

// Mark every signal a virtual interface can reach. The frontend records which
// instance scopes a `virtual` handle may name (Module::vif_scopes); an access
// through such a handle is emitted once against a base chosen at run time, so
// EVERY instance of the same interface definition must present the same
// layout and the same shape. Keyed on the definition name, which is what the
// state-type dedup keys on too.
void Backend::compute_vif_reachable() {
    vif_sig_.assign(mod_.signals().size(), false);
    if (mod_.vif_scopes.empty()) {
        return;
    }
    ankerl::unordered_dense::set<std::string> defs;
    const auto                                collect_defs = [&](auto&& self, const InstanceScope& s) -> void {
        if (std::ranges::contains(mod_.vif_scopes, s.id)) {
            defs.insert(s.def_name);
        }
        for (const InstanceScope& c : s.children) {
            self(self, c);
        }
    };
    const auto mark = [&](auto&& self, const InstanceScope& s) -> void {
        if (defs.contains(s.def_name)) {
            for (const Signal sg : s.signals) {
                vif_sig_[static_cast<uint32_t>(sg)] = true;
            }
        }
        for (const InstanceScope& c : s.children) {
            self(self, c);
        }
    };
    for (const InstanceScope& top : mod_.tops()) {
        collect_defs(collect_defs, top);
    }
    for (const InstanceScope& top : mod_.tops()) {
        mark(mark, top);
    }
}

// Which members a nonblocking write through a handle targets. The write names
// one representative instance's signal; the mark is spread across the
// definition, because the handle may name any instance and every writer of the
// member has to agree on the path.
void Backend::collect_vif_nba() {
    const auto n = mod_.signals().size();
    vif_nba_.assign(n, false);
    bool any = false;
    // A write whose lvalue reaches through a handle names it syntactically.
    const auto scan = [&](const Ir& ir, bool whole_body_is_vif) {
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            // The deferred stores count too (`vif.sig <= #d x` / `<= @e x`): the
            // mark is what gives their records the resolved instance, and it also
            // has to move the member's plain NBAs onto the same pointer-carrying
            // path so the two agree on where the member lives.
            const bool deferred =
              ir.tag(idx) == Inst::Tag::StoreNbDelay || ir.tag(idx) == Inst::Tag::StoreNbEvent;
            if (ir.tag(idx) != Inst::Tag::StoreNb && !deferred) {
                continue;
            }
            const LvalueRoot r =
              lvalue_root(ir, deferred ? ir.payload(idx).op_extra.op : ir.payload(idx).binary.l);
            if (ir.tag(r.root) == Inst::Tag::IfaceSignal) {
                vif_nba_[static_cast<uint32_t>(
                  ir.extra_data<IfaceMember>(ir.payload(r.root).op_extra.extra).signal)] = true;
                any = true;
                continue;
            }
            // Inside a subroutine reached through a handle, a bare member
            // reference IS a write through a handle -- the instance is the one
            // formal 0 named, not the one the body was lowered against. Its
            // lvalue is an ordinary Signal, so nothing about the statement says
            // so; the body does. Without this the write stages into the
            // representative's block shadow and commits into the wrong instance.
            if (whole_body_is_vif && ir.tag(r.root) == Inst::Tag::Signal) {
                vif_nba_[static_cast<uint32_t>(ir.payload(r.root).signal)] = true;
                any = true;
            }
        }
    };
    for (const Process& pr : mod_.processes()) {
        scan(pr.ir, /*whole_body_is_vif=*/false);
    }
    for (size_t f = 0; f < mod_.function_count(); ++f) {
        const Function& fn = mod_.function(static_cast<FunctionId>(f));
        scan(fn.ir, fn.takes_iface_ctx);
    }
    if (any) {
        spread_by_definition(vif_nba_);
    }
}

// Spread a per-signal mark across every instance of each definition, by
// (definition name, index within its own scope) -- the pair that identifies a
// member independently of which instance it belongs to.
void Backend::spread_by_definition(std::vector<bool>& mark) {
    ankerl::unordered_dense::map<std::string, std::vector<bool>> want;
    const auto note = [&](auto&& self, const InstanceScope& sc) -> void {
        auto& w = want[sc.def_name];
        w.resize(std::max(w.size(), sc.signals.size()), false);
        for (uint32_t k = 0; k < sc.signals.size(); ++k) {
            w[k] = w[k] || mark[static_cast<uint32_t>(sc.signals[k])];
        }
        for (const InstanceScope& c : sc.children) {
            self(self, c);
        }
    };
    const auto apply = [&](auto&& self, const InstanceScope& sc) -> void {
        const auto& w = want[sc.def_name];
        for (uint32_t k = 0; k < sc.signals.size() && k < w.size(); ++k) {
            mark[static_cast<uint32_t>(sc.signals[k])] =
              mark[static_cast<uint32_t>(sc.signals[k])] || w[k];
        }
        for (const InstanceScope& c : sc.children) {
            self(self, c);
        }
    };
    for (const InstanceScope& top : mod_.tops()) {
        note(note, top);
    }
    for (const InstanceScope& top : mod_.tops()) {
        apply(apply, top);
    }
}

// Which (member, edge) pairs a `@(edge vif.sig)` parks on. The wait names one
// representative instance's signal; every instance of that definition needs the
// head, because the handle may name any of them, so the mark is spread by
// definition exactly as vif_sig_ is.
void Backend::collect_vif_waits() {
    const auto n = mod_.signals().size();
    vif_wait_.assign(n, { false, false, false });
    vif_head_ord_.assign(n, { ~0u, ~0u, ~0u });
    const auto scan = [&](const Ir& ir) {
        for (size_t k = 0; k < ir.inst_count(); ++k) {
            const auto idx = static_cast<Index>(k);
            if (ir.tag(idx) != Inst::Tag::Wait) {
                continue;
            }
            for (const WaitEdge& we : ir.wait_edges(ir.payload(idx).extra)) {
                if (we.inst != kNoWaitHandle) {
                    vif_wait_[static_cast<uint32_t>(we.signal)][we.edge] = true;
                }
            }
        }
    };
    for (const Process& pr : mod_.processes()) {
        scan(pr.ir);
    }
    for (size_t f = 0; f < mod_.function_count(); ++f) {
        scan(mod_.function(static_cast<FunctionId>(f)).ir);
    }
    // Spread each mark across the definition: a representative's def-local
    // position is shared by its twins, and the layout must be too.
    std::array<bool, 3> any{ false, false, false };
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t e = 0; e < 3; ++e) {
            any[e] = any[e] || vif_wait_[i][e];
        }
    }
    if (!any[0] && !any[1] && !any[2]) {
        return;
    }
    // Index within its own scope -> the edges wanted, per definition name.
    ankerl::unordered_dense::map<std::string, std::vector<std::array<bool, 3>>> want;
    const auto note = [&](auto&& self, const InstanceScope& sc) -> void {
        auto& w = want[sc.def_name];
        w.resize(std::max(w.size(), sc.signals.size()), { false, false, false });
        for (uint32_t k = 0; k < sc.signals.size(); ++k) {
            const auto si = static_cast<uint32_t>(sc.signals[k]);
            for (uint32_t e = 0; e < 3; ++e) {
                w[k][e] = w[k][e] || vif_wait_[si][e];
            }
        }
        for (const InstanceScope& c : sc.children) {
            self(self, c);
        }
    };
    const auto apply = [&](auto&& self, const InstanceScope& sc) -> void {
        const auto& w = want[sc.def_name];
        for (uint32_t k = 0; k < sc.signals.size() && k < w.size(); ++k) {
            const auto si = static_cast<uint32_t>(sc.signals[k]);
            for (uint32_t e = 0; e < 3; ++e) {
                vif_wait_[si][e] = vif_wait_[si][e] || w[k][e];
            }
        }
        for (const InstanceScope& c : sc.children) {
            self(self, c);
        }
    };
    for (const InstanceScope& top : mod_.tops()) {
        note(note, top);
    }
    for (const InstanceScope& top : mod_.tops()) {
        apply(apply, top);
    }
}

// Subroutines reachable from program code (LRM 24.3) -- a program block's
// `initial`, or a program's continuous assign / port copy. One of the two
// reactive-home closures build_shared_home_fns intersects against ordinary
// code to find the bodies whose home region has to travel with the thread;
// the other is build_reactive_fns'. Also counts the program `initial`
// procedures for the 24.3 implicit-finish countdown.
void Backend::build_program_fns() {
    const size_t nfn = mod_.function_count();
    program_fns_.assign(nfn, false);
    program_proc_count_ = 0;

    std::vector<FunctionId> work;
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::Reactive) {
            // One process per group: a sensitivity-less body is never fused
            // (see lower_group), so this also counts the emitted bodies, which
            // is what the 24.3 countdown needs.
            ++program_proc_count_;
            collect_callees(p.ir, work);
        } else if (p.reactive_home) {
            // Program code as well, so a function it calls is program-reachable
            // and its suspends answer to the same refusal. Deliberately NOT
            // counted: LRM 24.3 scopes the countdown to `initial` procedures --
            // the simulation ends "immediately after all the threads and all
            // their descendent threads originating from all initial procedures
            // within all programs have ended" -- and a continuously-active
            // process is not one and never ends, so counting it would hold the
            // counter permanently above zero and the implicit $finish would
            // never fire.
            collect_callees(p.ir, work);
        }
    }
    while (!work.empty()) {
        const auto f = static_cast<uint32_t>(work.back());
        work.pop_back();
        if (program_fns_[f]) {
            continue;
        }
        program_fns_[f] = true;
        collect_callees(mod_.function(static_cast<FunctionId>(f)).ir, work);
    }
}

// Whether `id` is reachable from a program block (build_program_fns).
bool Backend::is_program_fn(FunctionId id) const {
    const auto k = static_cast<uint32_t>(id);
    return k < program_fns_.size() && program_fns_[k];
}

// Every call edge of `ir`, with virtual dispatch expanded to EVERY body that
// can occupy the slot rather than only to the statically named target
// `ir::collect_callees` follows.
//
// The union is required here, and in both directions. LRM 24.3.1 makes the home
// region a property of the calling thread, so the question this walk answers is
// "which bodies can a reactive thread reach" -- and a virtual task call reaches
// whichever override the handle holds, which no static target names. Following
// the static target alone would leave an override reached only through a handle
// in NEITHER closure, and it would then fold the Active home and resume a
// program's thread in the wrong region, silently. Because the same union is
// taken from the ordinary roots too, an over-approximation cannot go wrong the
// other way either: a slot both kinds of code dispatch on puts all of its
// overrides in both closures, i.e. marks them shared, and a shared body asks
// the caller instead of folding anything.
//
// build_rng_fns builds the same two tables for the same reason (an override's
// $urandom draw); kept local to each because they are cheap and the two passes
// run at different points of run().
void Backend::collect_home_callees(const Ir&                                   ir,
                                   const std::vector<std::vector<FunctionId>>& slot_fns,
                                   const std::vector<std::vector<FunctionId>>& iface_fns,
                                   std::vector<FunctionId>&                    out) const {
    collect_callees(ir, out);
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        const auto i = static_cast<Index>(k);
        if (ir.tag(i) != Inst::Tag::CallVirtual && ir.tag(i) != Inst::Tag::TaskCallVirtual) {
            continue;
        }
        const auto vci = ir.extra_data<VirtualCallInfo>(ir.payload(i).extra);
        // Two numbering spaces: ordinary vtable slots, and the design-wide
        // interface-method indices an interface-handle dispatch uses (LRM 8.26).
        const std::vector<std::vector<FunctionId>>& table = vci.iface_index != kNoIface ? iface_fns : slot_fns;
        const uint32_t                              idx = vci.iface_index != kNoIface ? vci.iface_index : vci.slot;
        if (idx < table.size()) {
            out.insert(out.end(), table[idx].begin(), table[idx].end());
        }
    }
}

// Which subroutines have a home region that must travel with the calling
// THREAD instead of being folded into the body -- LRM 24.3.1:
//
//   Once a program process starts a thread of execution, all subsequent
//   blocking statements in that thread are scheduled in the Reactive region.
//   This includes subroutine code called by the thread, even if the subroutine
//   code is declared in a module, package, or interface. Effectively, a section
//   of sequential code anywhere in the design or testbench inherits the
//   scheduling region of the thread that calls it.
//
// and 24.5, which states the same rule for the one shape it is easiest to hit:
// "When a task within a design module is called from a program, it shall use
// the reactive region set for its scheduling activities."
//
// That is exactly an INTERSECTION. A body only reactive-home code reaches is
// reactive-home for good, a body only ordinary code reaches is active-home for
// good, and each folds its answer at every park (reactive_home). A body BOTH
// reach has no lexical answer at all -- it is lowered once, and 24.5's own
// example is a `task T` a module declares, one program calling it and one
// module `initial` calling it in the same design -- so it reads its home from
// the calling thread instead (body_home_is_runtime_ / home_ptr), which is the
// clause's own wording made operational.
//
// The two closures:
//   R  reactive-home. Seeded from build_program_fns' and build_reactive_fns'
//      results (a program `initial`, a program's continuous assign / port copy,
//      an assertion action block, and everything they statically call) PLUS the
//      virtual-dispatch union out of every reactive-home PROCESS body, which
//      those two passes cannot see because they never walk a process with this
//      walker.
//   A  active-home. Every other process, plus the same catch-all
//      build_reactive_fns used: a function OUTSIDE R contributes its callees,
//      so a chain nothing else reaches still lands somewhere.
//
// A body in neither closure is unreachable -- nothing calls it and no dispatch
// site can land in it -- so its folded Active home can never be observed, and
// it deliberately does not turn the ramp parameter on.
void Backend::build_shared_home_fns() {
    const size_t nfn = mod_.function_count();
    shared_home_fns_.assign(nfn, false);
    reactive_home_fns_.assign(nfn, false);
    design_uses_home_arg_ = false;

    std::vector<std::vector<FunctionId>> slot_fns;
    std::vector<std::vector<FunctionId>> iface_fns;
    const auto collect = [](std::vector<std::vector<FunctionId>>& into, const std::vector<FunctionId>& from) {
        if (into.size() < from.size()) {
            into.resize(from.size());
        }
        for (size_t s = 0; s < from.size(); ++s) {
            into[s].push_back(from[s]);
        }
    };
    for (const ClassDesc& vt : mod_.class_descs()) {
        collect(slot_fns, vt.slots);
        collect(iface_fns, vt.iface_slots);
    }
    // Drain `seeds` into `into`, walking each newly added body's own call edges.
    const auto close = [&](std::vector<bool>& into, std::vector<FunctionId>& seeds) {
        while (!seeds.empty()) {
            const auto f = static_cast<uint32_t>(seeds.back());
            seeds.pop_back();
            if (into[f]) {
                continue;
            }
            into[f] = true;
            collect_home_callees(mod_.function(static_cast<FunctionId>(f)).ir, slot_fns, iface_fns, seeds);
        }
    };

    std::vector<FunctionId> work;
    for (size_t k = 0; k < nfn; ++k) {
        const auto id = static_cast<FunctionId>(k);
        if (is_program_fn(id) || is_reactive_fn(id)) {
            work.push_back(id);
        }
    }
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::Reactive || p.reactive_home) {
            collect_home_callees(p.ir, slot_fns, iface_fns, work);
        }
    }
    close(reactive_home_fns_, work);

    std::vector<bool> active(nfn, false);
    for (const Process& p : mod_.processes()) {
        if (p.activation != Activation::Reactive && !p.reactive_home) {
            collect_home_callees(p.ir, slot_fns, iface_fns, work);
        }
    }
    for (size_t k = 0; k < nfn; ++k) {
        if (!reactive_home_fns_[k]) {
            collect_home_callees(mod_.function(static_cast<FunctionId>(k)).ir, slot_fns, iface_fns, work);
        }
    }
    close(active, work);

    for (size_t k = 0; k < nfn; ++k) {
        shared_home_fns_[k]   = reactive_home_fns_[k] && active[k];
        design_uses_home_arg_ = design_uses_home_arg_ || shared_home_fns_[k];
        // A reactive-home subroutine may now suspend, so its `#0` can park in
        // the re-inactive batch (emit_zero_delay_push) -- for a shared body at
        // run time, on the branch emit_by_home picks. Decided here because
        // run() has to create the batch before any body lowers: lower_finish
        // empties whichever queues exist, whatever order the bodies came in.
        if (reactive_home_fns_[k] && ir_has_delay(mod_.function(static_cast<FunctionId>(k)).ir)) {
            reactive_has_delay_ = true;
        }
    }
}

// Whether `id`'s LEXICAL home is Reactive, i.e. whether reactive-home code
// reaches it (build_shared_home_fns). Bounds-checked like is_program_fn: the
// table is only sized for a design that has reactive code at all.
bool Backend::is_reactive_home_fn(FunctionId id) const {
    const auto k = static_cast<uint32_t>(id);
    return k < reactive_home_fns_.size() && reactive_home_fns_[k];
}

// Whether `id` is reached from BOTH reactive-home and ordinary code, and so has
// no lexical home at all (build_shared_home_fns).
bool Backend::is_shared_home_fn(FunctionId id) const {
    const auto k = static_cast<uint32_t>(id);
    return k < shared_home_fns_.size() && shared_home_fns_[k];
}

// Classify every function reachable as reactive code -- the action-block
// tasks a ReactivePend names, plus their transitive callees -- and reject
// what the reactive region set cannot yet run. The set is also one of
// build_shared_home_fns' two seeds, which is what actually decides the region
// a #0 inside one of them parks in (emit_zero_delay_push).
//
// Virtual dispatch is followed through its statically named target only. An
// action block that reached an override through a class handle would have its
// body missed here; the frontend cannot currently produce one (an action block
// lowers to severity tasks and $display), so this is a documented bound rather
// than a silent one -- and it bounds only the REJECTIONS below, since
// build_shared_home_fns re-closes the same seed over the whole vtable slot
// before answering the home question.
void Backend::build_reactive_fns() {
    const size_t nfn = mod_.function_count();
    reactive_fns_.assign(nfn, false);

    std::vector<FunctionId> work;
    const auto seed = [&work](const Ir& ir) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto i = static_cast<Index>(k);
            if (ir.tag(i) == Inst::Tag::ReactivePend) {
                work.push_back(ir.extra_data<StrobePendInfo>(ir.payload(i).extra).func);
            }
        }
    };
    for (const Process& p : mod_.processes()) {
        seed(p.ir);
    }
    for (size_t k = 0; k < nfn; ++k) {
        seed(mod_.function(static_cast<FunctionId>(k)).ir);
    }
    while (!work.empty()) {
        const auto f = static_cast<uint32_t>(work.back());
        work.pop_back();
        if (reactive_fns_[f]) {
            continue;
        }
        reactive_fns_[f] = true;
        collect_callees(mod_.function(static_cast<FunctionId>(f)).ir, work);
    }

    for (size_t k = 0; k < nfn; ++k) {
        if (reactive_fns_[k]) {
            const Function& f = mod_.function(static_cast<FunctionId>(k));
            reject_unsupported_reactive(f.name, f.ir);
        }
    }
    for (const Process& p : mod_.processes()) {
        if (p.activation == Activation::Observed) {
            reject_resume_once(p.ir, "Observed", "property evaluation, LRM 4.4.2.5");
        } else if (p.activation == Activation::ReNba) {
            reject_resume_once(p.ir, "Re-NBA", "clocking block drives, LRM 4.4.2.8");
        } else if (p.activation == Activation::Reactive || p.reactive_home) {
            // A program `initial` (LRM 24.3) is reactive-home too, and its #0
            // parks in the re-inactive batch exactly as an action block's does.
            // Noted here, before ensure_re_inactive_queue runs, so the batch
            // exists whatever order the bodies lower in -- lower_finish empties
            // whichever queues were created and must see this one.
            //
            // A reactive-home CONTINUOUS process (24.3.1) is scanned on the same
            // terms. No such body can hold a Delay today -- a continuous assign
            // lowers to a store, and `assign #d` to an inertial store, neither of
            // which suspends -- so this is the local statement of the invariant
            // emit_zero_delay_push depends on rather than a live case. The
            // SUBROUTINE half of the same invariant is build_shared_home_fns',
            // which scans every reactive-home body it classifies for a Delay.
            for (uint32_t k = 0; k < p.ir.inst_count(); ++k) {
                if (p.ir.tag(static_cast<Index>(k)) == Inst::Tag::Delay) {
                    reactive_has_delay_ = true;
                    break;
                }
            }
        }
    }
}

// What reactive code may not contain. Every rejection here is a hole in the
// region set, not a source error, so each names what is missing. The
// frontend's ReactiveSafetyChecker refuses the same shapes with a source
// location; this is the backend's independent backstop and the two are kept
// in agreement deliberately.
void Backend::reject_unsupported_reactive(const std::string& where, const Ir& ir) {
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        switch (ir.tag(static_cast<Index>(k))) {
            case Inst::Tag::StoreNbDelay:
            case Inst::Tag::StoreNbEvent:
            case Inst::Tag::StoreInertialDelay:
                // TODO: no longer a wake-path problem -- none of these three
                // suspends the issuing process, so the parked handle has
                // nothing to say about them. What is unsettled is the COMMIT
                // region. A plain `x <= y` issued in Reactive commits in
                // Re-NBA (re_nba.commit in emit_active_run). These carry their
                // own timing, so the update lands in a LATER time slot, where
                // emit_intra_commit runs it in that slot's ordinary NBA region
                // -- not its Re-NBA one. LRM 4.4.2.8 describes Re-NBA as the
                // region for nonblocking updates issued from the reactive
                // region set, without saying which slot's region a delayed one
                // belongs to, and the LRM does not settle it. Refused rather than
                // guessed.
                throw BackendError{ "a delayed or event-controlled nonblocking assignment in "
                                    "reactive code (the action block of a concurrent assertion, "
                                    "here in '" +
                                    where + "') is not supported" };
            case Inst::Tag::ExpectWait:
                // Owned by the `expect` lowering (ExpectStmt); recorded here
                // because this function is the backend's single backstop for
                // what reactive code may contain. ExpectWake needs no case: it
                // is emitted only by emit_monitor into its own Observed
                // process and can never appear in a body a user wrote.
                throw BackendError{ "an `expect` inside reactive code (the action block of a "
                                    "concurrent assertion) is not supported: it blocks the "
                                    "executing process until its property completes, and an "
                                    "action block is a zero-time thunk the reactive drain "
                                    "resumes exactly once" };
            case Inst::Tag::Delay:
                // A #0 in reactive code parks in the re-inactive batch, which
                // has to exist before any body lowers (see run()).
                reactive_has_delay_ = true;
                break;
            default:
                break;
        }
    }
}

// Whether `ir` holds a `Fork`, or calls something that does. A `Fork` used to
// be possible only in a coroutine body, so a syntactic scan of the process was
// enough; LRM 13.4.4 allows `fork … join_none` in a plain FUNCTION, so the scan
// has to close over calls or a fork reached one call deep goes unseen. The
// frontend refuses this with a source location (check_spawn_roots); this is the
// independent backstop, so the two are kept in agreement deliberately.
bool Backend::reaches_fork(const Ir& ir) {
    const auto holds_fork = [](const Ir& body) {
        for (uint32_t k = 0; k < body.inst_count(); ++k) {
            if (body.tag(static_cast<Index>(k)) == Inst::Tag::Fork) {
                return true;
            }
        }
        return false;
    };
    if (holds_fork(ir)) {
        return true;
    }
    std::vector<uint8_t>    seen(mod_.function_count(), 0);
    std::vector<FunctionId> work;
    collect_callees(ir, work);
    while (!work.empty()) {
        const auto f = static_cast<uint32_t>(work.back());
        work.pop_back();
        if (f >= seen.size() || seen[f] != 0) {
            continue;
        }
        seen[f]          = 1;
        const Ir& body   = mod_.function(static_cast<FunctionId>(f)).ir;
        if (holds_fork(body)) {
            return true;
        }
        collect_callees(body, work);
    }
    return false;
}

// What a resume-once region process may not contain -- an Observed body
// (emit_observed_run) or a Re-NBA one (emit_renba_run). Both are run to
// completion by a single resume at a point where the NBA region has already
// committed, so neither may suspend, and neither is in the reactive region set
// that LRM 4.4.2.8 scopes the update region to.
void Backend::reject_resume_once(const Ir& ir, std::string_view region, std::string_view what) {
    if (reaches_fork(ir)) {
        // Same reason as the Delay / Wait case below, one call deep: a spawned
        // child would be pushed onto a ring this region's straight-line run never
        // drains. LRM 13.4.4 refuses the source shape too (check_spawn_roots).
        throw BackendError{ "a fork in a " + std::string(region) + "-region process (" +
                            std::string(what) + ") is not supported" };
    }
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        switch (ir.tag(static_cast<Index>(k))) {
            case Inst::Tag::Delay:
            case Inst::Tag::Wait:
                // TODO: a body that suspends needs its region to be a queue that
                // can be re-entered, not a straight-line run. Resuming it once
                // would silently truncate the body.
                throw BackendError{ "a timing control in a " + std::string(region) + "-region process (" +
                                    std::string(what) + ") is not supported" };
            case Inst::Tag::StoreNb:
            case Inst::Tag::StoreNbDelay:
            case Inst::Tag::StoreNbEvent:
            case Inst::Tag::StoreInertialDelay:
            case Inst::Tag::EventTriggerNb:
            case Inst::Tag::EventTriggerNbDelayed:
                // LRM 4.4.2.8 scopes the update region to nonblocking assignments
                // made from the REACTIVE region set, and neither of these
                // processes is in it; committing one there would be clotho
                // inventing a schedule the LRM does not give. A clocking block
                // drive is not one of these: LRM 14.16 puts the value change
                // itself in Re-NBA, which is where that body already runs, so it
                // stores directly.
                throw BackendError{ "a nonblocking assignment in a " + std::string(region) +
                                    "-region process (" + std::string(what) + ") is not supported" };
            default:
                break;
        }
    }
}

// Static-subscription order: a topological sort of processes over the
// blocking-write wake graph (edge P -> Q iff P blocking-writes a signal in
// Q's sensitivity list). Wait-queue bucket entries fill in subscription
// order and the FIFO drain pops in bucket order, so subscribing in this
// order runs producers before consumers within a settle wave: an acyclic
// blocking cascade settles in one wave regardless of source order.
//
// NBA writes are deliberately not edges — a commit lands between drains and
// starts a fresh wave, so they cannot cause same-wave re-evaluation, and
// excluding them removes the clocked feedback that would otherwise make
// every sequential design one big SCC.
//
// Cycles (combinational loops): Tarjan SCC + condensation order, so only
// the processes inside a loop keep declaration order among themselves;
// everything downstream of the loop is still placed after it. Convergence
// is unaffected either way — re-wake-on-change still iterates SCCs.
//
// Mixed home regions need nothing here. An edge whose endpoints sit in
// different rings (a program's reactive-home continuous assign feeding a
// design process, LRM 24.3.2) can never be a same-wave ordering constraint:
// the two rings drain in separate passes, and the reader is only reached by
// the active re-settle after the whole reactive drain (emit_active_run), which
// is exactly 24.3.2's "next iteration of the overall scheduling loop". So a
// cross-home edge costs at worst a conservative rank, never a lost wave. The
// other consumer of these edges, comb fusion, refuses to merge across homes
// outright (compute_fusion_groups).
void Backend::compute_subscription_order() {
    const auto     all    = mod_.processes();
    const auto     nprocs = static_cast<uint32_t>(all.size());
    const uint32_t nsigs  = static_cast<uint32_t>(mod_.signals().size());
    sub_order_.resize(nprocs);
    for (uint32_t k = 0; k < nprocs; ++k) {
        sub_order_[k] = k;
    }
    wake_adj_.assign(nprocs, {});
    scc_nontrivial_.assign(nprocs, 0);
    if (nprocs < 2) {
        return;
    }

    // Blocking-written signals of one IR body: Store roots plus out/inout
    // TaskCall actuals (copy-out writes the actual in the caller). Roots
    // that are not signals (function locals / formals) are skipped.

    // Per-function write sets, closed transitively over calls (memoized
    // DFS; an in-progress callee — recursion — contributes what it has so
    // far, an over- or under-approximation that only perturbs ordering).
    const auto                       nf = static_cast<uint32_t>(mod_.function_count());
    std::vector<std::vector<Signal>> fn_writes(nf);
    std::vector<uint8_t>             fn_state(nf, 0); // 0 new, 1 visiting, 2 done
    const auto close_fn = [&](this const auto& self, uint32_t f) -> void {
        if (fn_state[f] != 0) {
            return;
        }
        fn_state[f] = 1;
        const BodyFacts& bf = facts_.functions[f];
        fn_writes[f].insert(fn_writes[f].end(), bf.writes.begin(), bf.writes.end());
        for (const FunctionId c : bf.calls) {
            self(static_cast<uint32_t>(c));
            const auto& cw = fn_writes[static_cast<uint32_t>(c)];
            fn_writes[f].insert(fn_writes[f].end(), cw.begin(), cw.end());
        }
        fn_state[f] = 2;
    };

    // Readers: processes statically subscribed to each signal (any edge).
    std::vector<std::vector<uint32_t>> readers(nsigs);
    for (uint32_t k = 0; k < nprocs; ++k) {
        if (all[k].activation != Activation::Always) {
            continue;
        }
        for (const WaitEvent& w : all[k].wait_events) {
            readers[static_cast<uint32_t>(w.signal)].push_back(k);
        }
    }

    // Adjacency, deduplicated with a per-source stamp. Kept as a member:
    // the comb-fusion pass reuses these edges for component grouping.
    std::vector<std::vector<uint32_t>>& adj = wake_adj_;
    std::vector<uint32_t>               stamp(nprocs, UINT32_MAX);
    for (uint32_t k = 0; k < nprocs; ++k) {
        std::vector<Signal> writes = facts_.processes[k].body.writes;
        for (const FunctionId c : facts_.processes[k].body.calls) {
            close_fn(static_cast<uint32_t>(c));
            const auto& cw = fn_writes[static_cast<uint32_t>(c)];
            writes.insert(writes.end(), cw.begin(), cw.end());
        }
        for (const Signal s : writes) {
            for (const uint32_t q : readers[static_cast<uint32_t>(s)]) {
                if (q == k || stamp[q] == k) {
                    continue;
                }
                stamp[q] = k;
                adj[k].push_back(q);
            }
        }
    }

    // Iterative Tarjan. SCCs complete in reverse condensation-topo order
    // (an SCC finishes only after everything it reaches), so assigning
    // descending ranks as they complete numbers SCCs in topo order.
    std::vector<uint32_t> scc_rank(nprocs, 0);
    uint32_t              next_rank = nprocs; // descending as SCCs complete
    {
        constexpr uint32_t    none = UINT32_MAX;
        std::vector<uint32_t> index(nprocs, none);
        std::vector<uint32_t> low(nprocs, 0);
        std::vector<uint8_t>  on_stack(nprocs, 0);
        std::vector<uint32_t> stack;
        uint32_t              next_index = 0;
        struct Frame {
            uint32_t v;
            uint32_t child;
        };
        std::vector<Frame> dfs;
        for (uint32_t root = 0; root < nprocs; ++root) {
            if (index[root] != none) {
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
                    if (index[w] == none) {
                        index[w] = low[w] = next_index++;
                        stack.push_back(w);
                        on_stack[w] = 1;
                        dfs.push_back({ w, 0 });
                    }
                    else if (on_stack[w]) {
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
                    --next_rank;
                    uint32_t w;
                    do {
                        w = stack.back();
                        stack.pop_back();
                        on_stack[w] = 0;
                        scc_rank[w] = next_rank;
                    } while (w != v);
                }
            }
        }
    }

    // Nontrivial SCC membership (multi-process combinational loop): those
    // processes have no producer-before-consumer order, so comb fusion
    // must leave them on the re-wake iteration path.
    {
        std::vector<uint32_t> scc_size(nprocs, 0);
        for (uint32_t k = 0; k < nprocs; ++k) {
            scc_size[scc_rank[k]]++;
        }
        for (uint32_t k = 0; k < nprocs; ++k) {
            scc_nontrivial_[k] = scc_size[scc_rank[k]] > 1 ? 1 : 0;
        }
    }

    // Order by longest-path level in the condensation, not by raw SCC
    // number: distinct SCC numbers would also order UNRELATED processes by
    // arbitrary DFS completion, gratuitously shuffling them against
    // declaration order (measured as a few percent of extra comb
    // re-evaluation on picorv32). Levels give every process the earliest
    // slot its producers allow; everything else keeps declaration order
    // via the stable sort.
    const uint32_t        nscc = nprocs - next_rank;
    const auto            dense = [&](uint32_t p) { return scc_rank[p] - next_rank; };
    std::vector<uint32_t> level(nscc, 0);
    std::vector<uint32_t> by_rank = sub_order_;
    std::stable_sort(by_rank.begin(), by_rank.end(),
                     [&](uint32_t a, uint32_t b) { return scc_rank[a] < scc_rank[b]; });
    // Cross-SCC edges always point to a higher rank (the successor SCC
    // completed first), so one ascending pass relaxes in topo order.
    for (const uint32_t u : by_rank) {
        for (const uint32_t v : adj[u]) {
            if (dense(v) != dense(u)) {
                level[dense(v)] = std::max(level[dense(v)], level[dense(u)] + 1);
            }
        }
    }
    std::stable_sort(sub_order_.begin(), sub_order_.end(),
                     [&](uint32_t a, uint32_t b) { return level[dense(a)] < level[dense(b)]; });
}

// True when instruction `i` in `ir` is an array shuffle (DynArrayShuffle /
// QueueShuffle), which draws from the thread's RNG like $urandom.
bool Backend::is_shuffle(const Ir& ir, Index i) {
    const Inst::Tag tag = ir.tag(i);
    return tag == Inst::Tag::DynArrayShuffle || tag == Inst::Tag::QueueShuffle;
}

// True when a body draws from an RNG state directly: the $urandom family, or
// an array shuffle (which advances the same state, through the runtime rather
// than inline -- see rt_dynarr_shuffle). The seed set of build_rng_fns.
bool Backend::ir_draws_rng(const Ir& ir) {
    for (size_t k = 0; k < ir.inst_count(); ++k) {
        const auto idx = static_cast<Index>(k);
        switch (ir.tag(idx)) {
            case Inst::Tag::Urandom:
            case Inst::Tag::UrandomSeeded:
            case Inst::Tag::UrandomRange:
                return true;
            default:
                if (is_shuffle(ir, idx)) {
                    return true;
                }
                break;
        }
    }
    return false;
}

// Which bodies can reach an RNG draw, and which own their state.
//
// The `ptr rng` ABI parameter itself is unconditional (declare_function), so
// this decides only where a state is MATERIALIZED: a process allocates and
// seeds one in its coroutine frame iff it can reach a draw, and everything
// else hands callees the module-wide fallback. Must run before
// compute_fusion_groups, which reads rng_procs_ to keep a reaching process
// off the thunk path.
void Backend::build_rng_fns() {
    const size_t nfn   = mod_.function_count();
    const auto   procs = mod_.processes();
    rng_fns_.assign(nfn, false);
    rng_own_.assign(nfn, false);
    rng_procs_.assign(procs.size(), false);

    // Virtual dispatch picks the body out of a vtable SLOT at run time, so a
    // virtual call reaches every function that can occupy that slot -- the same
    // union the ref ABI takes (Analysis.cpp). Following the statically named
    // target alone, as collect_callees does, would miss an override's draw and
    // silently leave its caller without a state to hand it.
    // Two numbering spaces: ordinary vtable slots, and the design-wide
    // interface-method indices an interface-handle dispatch uses (LRM 8.26).
    std::vector<std::vector<FunctionId>> slot_fns;
    std::vector<std::vector<FunctionId>> iface_fns;
    const auto collect = [](std::vector<std::vector<FunctionId>>& into, const std::vector<FunctionId>& from) {
        if (into.size() < from.size()) {
            into.resize(from.size());
        }
        for (size_t s = 0; s < from.size(); ++s) {
            into[s].push_back(from[s]);
        }
    };
    for (const ClassDesc& vt : mod_.class_descs()) {
        collect(slot_fns, vt.slots);
        collect(iface_fns, vt.iface_slots);
    }

    // Every body whose draws are charged to `ir`'s own state: its callees, the
    // fork children it spawns and the deferred bodies it ramps (both seed
    // themselves from this state at the ramp, so this body needs one).
    // Deliberately NOT MonitorSet: that only registers the body, which the
    // Postponed drain ramps from main against the module-wide state.
    const auto callees = [&](const Ir& ir, std::vector<FunctionId>& out) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto           i = static_cast<Index>(k);
            const Inst::Payload& p = ir.payload(i);
            switch (ir.tag(i)) {
                case Inst::Tag::Call:
                    out.push_back(ir.extra_data<CallInfo>(p.extra).func);
                    break;
                case Inst::Tag::TaskCall:
                    out.push_back(ir.extra_data<TaskCallInfo>(p.extra).func);
                    break;
                case Inst::Tag::CallVirtual:
                case Inst::Tag::TaskCallVirtual: {
                    const auto vci = ir.extra_data<VirtualCallInfo>(p.extra);
                    const std::vector<std::vector<FunctionId>>& table =
                      vci.iface_index != kNoIface ? iface_fns : slot_fns;
                    const uint32_t idx = vci.iface_index != kNoIface ? vci.iface_index : vci.slot;
                    if (idx < table.size()) {
                        out.insert(out.end(), table[idx].begin(), table[idx].end());
                    }
                    break;
                }
                case Inst::Tag::Fork:
                    for (const Index kid : ir.region(ir.extra_data<ForkInfo>(p.extra).children)) {
                        out.push_back(static_cast<FunctionId>(kid));
                    }
                    break;
                case Inst::Tag::StrobePend:
                case Inst::Tag::ReactivePend:
                    out.push_back(ir.extra_data<StrobePendInfo>(p.extra).func);
                    break;
                default:
                    break;
            }
        }
    };

    // A body that can outlive the pointer it was handed owns its state instead
    // of sharing it: a fork child (its parent's frame goes away under join_none
    // / join_any) and a deferred $strobe / $monitor / action body (ramped now,
    // resumed in a later region, possibly from main).
    const auto note_detached = [&](const Ir& ir) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto i = static_cast<Index>(k);
            switch (ir.tag(i)) {
                case Inst::Tag::StrobePend:
                case Inst::Tag::ReactivePend:
                case Inst::Tag::MonitorSet:
                    rng_own_[static_cast<uint32_t>(
                      ir.extra_data<StrobePendInfo>(ir.payload(i).extra).func)] = true;
                    break;
                default:
                    break;
            }
        }
    };

    // Reverse edges, then a worklist from every direct drawer: `rng_fns_[f]`
    // ends up true iff f can reach a draw.
    std::vector<std::vector<uint32_t>> callers(nfn);
    std::vector<FunctionId>            tmp;
    std::vector<uint32_t>              work;
    for (uint32_t f = 0; f < nfn; ++f) {
        const Function& fu = mod_.function(static_cast<FunctionId>(f));
        rng_own_[f]        = rng_own_[f] || fu.is_fork_child;
        if (ir_draws_rng(fu.ir)) {
            rng_fns_[f] = true;
            work.push_back(f);
        }
        note_detached(fu.ir);
        tmp.clear();
        callees(fu.ir, tmp);
        for (const FunctionId c : tmp) {
            callers[static_cast<uint32_t>(c)].push_back(f);
        }
    }
    while (!work.empty()) {
        const uint32_t f = work.back();
        work.pop_back();
        for (const uint32_t c : callers[f]) {
            if (!rng_fns_[c]) {
                rng_fns_[c] = true;
                work.push_back(c);
            }
        }
    }
    for (size_t k = 0; k < procs.size(); ++k) {
        note_detached(procs[k].ir);
        if (ir_draws_rng(procs[k].ir)) {
            rng_procs_[k] = true;
            continue;
        }
        tmp.clear();
        callees(procs[k].ir, tmp);
        rng_procs_[k] = std::any_of(tmp.begin(), tmp.end(), [&](FunctionId c) {
            return rng_fns_[static_cast<uint32_t>(c)];
        });
    }
    // `p.srandom(seed)` / `p.set_randstate(s)` name their target through a HANDLE,
    // so which thread gets reseeded is not statically known (LRM 9.7, 18.14). A
    // design that uses any of the three therefore gives every thread its own
    // generator state instead of letting one share the module-wide fallback --
    // reseeding that would perturb every other sharer's stream.
    if (facts_.uses.process_randstate) {
        std::fill(rng_procs_.begin(), rng_procs_.end(), true);
        for (uint32_t f = 0; f < nfn; ++f) {
            if (mod_.function(static_cast<FunctionId>(f)).is_fork_child) {
                rng_own_[f] = true;
                rng_fns_[f] = true;
            }
        }
    }
}

// The module-wide fallback xoshiro256++ state. Created once in run() so main
// can seed it before anything runs; 32 bytes and one splitmix64 call, paid
// unconditionally so that every context without a process of its own -- a DPI
// export thunk, the $monitor ramp -- has a defined stream.
void Backend::build_rng_global() {
    auto* arrty = llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx_), 4);
    rng_global_ = new llvm::GlobalVariable(env_->llmod, arrty, false,
                                           llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantAggregateZero::get(arrty), "clotho.rng");
}

// May-finish closure (`--stat=finish`), measurement only.
//
// Sizing study for the cooperative-$finish migration
// (docs/design/cooperative-finish.md). Replacing rt_finish's longjmp with an
// ordinary return means a call that can reach a $finish must be followed by
// a test of the finishing flag, so the caller can return too. That test is
// the whole cost of the scheme in designs that never finish, and it is only
// needed at call sites whose callee can actually reach a $finish — which is
// what this counts.
//
// A subroutine is may-finish if its own body contains a Finish ($finish,
// $stop, or the finish $fatal appends) or if it calls a may-finish callee.
// A virtual call can dispatch to any override, so once anything in the design
// finishes, a body that makes a virtual call is may-finish too (matching
// virtual_call_may_finish at the call site itself).
//
// Scan one body for a Finish, collecting its call edges on the way. The edges
// are ir::collect_callees', so a `$finish` inside a fork child marks the
// forking body may-finish: a child is a body of its parent, and the parent
// blocks on it at a `join`.
bool Backend::scan_finish(const Ir& ir, std::vector<FunctionId>* out) {
    bool finishes = false;
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        const auto idx = static_cast<Index>(k);
        if (ir.tag(idx) == Inst::Tag::Finish) {
            finishes = true;
        }
        if (out != nullptr) {
            inst_callees(ir, idx, *out);
        }
    }
    return finishes;
}

// Which subroutines can reach a $finish, directly or through a callee. A call
// to one of these needs a post-call test of rt_finishing so the caller can
// leave too; every other call site is untouched. `direct_out`, when given,
// receives the un-closed seeds (the stat report distinguishes them).
std::vector<uint8_t> Backend::compute_may_finish(std::vector<uint8_t>* direct_out) const {
    const auto                           nf = static_cast<uint32_t>(mod_.function_count());
    std::vector<uint8_t>                 direct(nf, 0);
    std::vector<std::vector<FunctionId>> callees(nf);
    for (uint32_t f = 0; f < nf; ++f) {
        direct[f] = scan_finish(mod_.function(static_cast<FunctionId>(f)).ir, &callees[f]) ? 1 : 0;
    }
    // A DPI import has no IR body, so the scan above gives it no callees at all
    // -- yet its native C can call straight back into any `export "DPI-C"`
    // subroutine, and an exported body is ordinary SV that may `$finish`. Add
    // that edge explicitly: without it, an import whose C reaches a $finish is
    // not in the closure, its call site emits no post-call rt_finishing test,
    // and the finish does not unwind -- the caller runs on past a $finish, which
    // is a wrong answer rather than a missing optimization. Which export the C
    // picks is unknowable here, so the edge goes to every one; that is exact
    // whenever no export finishes (the overwhelmingly common case adds nothing)
    // and conservative otherwise. This matters most for an import TASK, whose
    // whole purpose is to reach an exported task, but an import FUNCTION calling
    // an exported function has always had the same reachability.
    if (!mod_.dpi_exports.empty()) {
        for (uint32_t f = 0; f < nf; ++f) {
            if (!mod_.function(static_cast<FunctionId>(f)).is_dpi) {
                continue;
            }
            for (const DpiExport& ex : mod_.dpi_exports) {
                callees[f].push_back(ex.func);
            }
        }
    }
    // Memoized DFS, the same shape as the per-function write-set closure. A
    // cycle contributes what the in-progress callee has so far; a recursive
    // subroutine that finishes is still caught by its own seed.
    std::vector<uint8_t> may(direct);
    // Seeded before the closure so a virtual call's may-finish reaches every
    // caller up the chain. Without it the call site tested rt_finishing and left,
    // but its caller, not in the closure, ran on with whatever the abandoned call
    // returned -- a garbage class handle in UVM, after a stubbed method's $fatal.
    if (std::any_of(direct.begin(), direct.end(), [](uint8_t v) { return v != 0; })) {
        for (uint32_t f = 0; f < nf; ++f) {
            const Ir& ir = mod_.function(static_cast<FunctionId>(f)).ir;
            for (uint32_t k = 0; k < ir.inst_count() && may[f] == 0; ++k) {
                const Inst::Tag tag = ir.tag(static_cast<Index>(k));
                if (tag == Inst::Tag::CallVirtual || tag == Inst::Tag::TaskCallVirtual) {
                    may[f] = 1;
                }
            }
        }
    }
    std::vector<uint8_t> state(nf, 0); // 0 new, 1 visiting, 2 done
    const auto           close = [&](this const auto& self, uint32_t f) -> void {
        if (state[f] != 0) {
            return;
        }
        state[f] = 1;
        for (const FunctionId c : callees[f]) {
            const auto ci = static_cast<uint32_t>(c);
            self(ci);
            may[f] = static_cast<uint8_t>(may[f] | may[ci]);
        }
        state[f] = 2;
    };
    for (uint32_t f = 0; f < nf; ++f) {
        close(f);
    }
    if (direct_out != nullptr) {
        *direct_out = std::move(direct);
    }
    return may;
}

// V1 static process fusion: partition processes into groups that lower to a
// single coroutine each. A group's members share an identical canonical
// sensitivity set and self_start flag, so the fused wake set equals every
// member's own wake set — each body still runs exactly when its own trigger
// fires, and activation counts are unchanged by construction (no
// over-evaluation possible). Running members back-to-back fixes one of the
// LRM-allowed same-instant orders (members are ordered by their
// sub_order_ rank, matching the unfused schedule).
//
// A process is fusable only when:
//  - it is an Always with a static sensitivity list;
//  - its body has no suspension point (no #delay, no intra-body @/wait, no
//    task call — a suspended fused body would stall its siblings);
//  - it does not blocking-write a signal in its own sensitivity set (the
//    write would re-wake an already-ran sibling in the unfused schedule,
//    but lands mid-body under the group's held sched flag when fused).
void Backend::compute_fusion_groups() {
    const auto all    = mod_.processes();
    const auto nprocs = static_cast<uint32_t>(all.size());
    groups_.clear();
    group_of_.assign(nprocs, 0);
    group_events_.clear();

    // Position of each process in sub_order_ (its scheduled rank).
    std::vector<uint32_t> pos(nprocs, 0);
    for (uint32_t i = 0; i < nprocs; ++i) {
        pos[sub_order_[i]] = i;
    }

    const auto canonical = [](const std::vector<WaitEvent>& evs) {
        std::vector<std::pair<uint32_t, uint32_t>> key;
        key.reserve(evs.size());
        for (const WaitEvent& w : evs) {
            key.emplace_back(static_cast<uint32_t>(w.signal), static_cast<uint32_t>(w.edge));
        }
        std::sort(key.begin(), key.end());
        key.erase(std::unique(key.begin(), key.end()), key.end());
        return key;
    };

    // Bucket fusable processes by (canonical events, self_start,
    // reactive_home), preserving first-seen order; everything else is its own
    // singleton group. Group size is capped so one fused coroutine cannot grow
    // arbitrarily large (function size / frame size / compile time).
    //
    // reactive_home is part of the key because a group is ONE function pushed
    // onto ONE ring: the seed loop makes a single push decision for it and its
    // static subscription records a single home tag, so fusing a program's
    // continuous assign (LRM 24.3.1, reactive-home) with a design one would
    // run half the group in the wrong region. A reactive-only group is fine --
    // both rings drain the same handle shapes.
    constexpr size_t group_cap = 256;
    std::map<std::tuple<std::vector<std::pair<uint32_t, uint32_t>>, bool, bool>, uint32_t> by_key;
    for (uint32_t k = 0; k < nprocs; ++k) {
        const Process& p = all[k];
        if (config_.fuse && facts_.processes[k].simple_always) {
            const auto key = std::make_tuple(canonical(p.wait_events), p.self_start, p.reactive_home);
            const auto it  = by_key.find(key);
            if (it != by_key.end() && groups_[it->second].size() < group_cap) {
                groups_[it->second].push_back(k);
                group_of_[k] = it->second;
                continue;
            }
            by_key[key] = static_cast<uint32_t>(groups_.size());
        }
        group_of_[k] = static_cast<uint32_t>(groups_.size());
        groups_.push_back({ k });
    }

    // Comb fusion: merge dependent comb-eligible singletons into
    // rank-contiguous chunks running producer-before-consumer in one
    // activation, subscribed to the union sensitivity (see
    // docs/design/comb-fusion.md for the LRM as-if argument). Two facts
    // carry the legality: the sched flag is held across the fused body,
    // so an intra-group store cannot re-enqueue the group (the downstream
    // member already runs later in the same activation with final
    // values); and change detection makes a spuriously-run member (pure,
    // reads unchanged) write no changes and wake nobody. Grouping is by
    // connected component of the wake graph so independent cones are
    // never yoked together (a union sensitivity across unrelated cones
    // would run every body per trigger); nontrivial SCCs (comb loops)
    // keep re-wake fixpoint iteration.
    if (config_.fuse && config_.comb_fuse && nprocs >= 2) {
        std::vector<uint8_t> cand(nprocs, 0);
        for (const auto& g : groups_) {
            if (g.size() != 1) {
                continue;
            }
            const uint32_t k = g.front();
            if (scc_nontrivial_[k] == 0 &&
                facts_.processes[k].comb_fuse == CombFuse::Eligible) {
                cand[k] = 1;
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
        for (uint32_t k = 0; k < nprocs; ++k) {
            if (cand[k] == 0) {
                continue;
            }
            const Ir& ir = all[k].ir;
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
        std::vector<uint8_t> interior(nprocs, 0);
        for (uint32_t k = 0; k < nprocs; ++k) {
            if (cand[k] == 0) {
                continue;
            }
            interior[k] = 1;
            for (const WaitEvent& w : all[k].wait_events) {
                if (cand_writes[static_cast<uint32_t>(w.signal)] == 0) {
                    interior[k] = 0;
                    break;
                }
            }
        }
        // Union-find over wake edges between candidates. self_start and
        // reactive_home must both match: the group makes one seed decision and
        // has one home ring (see the bucket key above). Comb populations are
        // uniformly self_start and Active-home in practice; a mismatch just
        // splits.
        std::vector<uint32_t> uf(nprocs);
        for (uint32_t k = 0; k < nprocs; ++k) {
            uf[k] = k;
        }
        const auto find = [&](uint32_t x) {
            while (uf[x] != x) {
                uf[x] = uf[uf[x]];
                x     = uf[x];
            }
            return x;
        };
        for (uint32_t k = 0; k < nprocs; ++k) {
            if (cand[k] == 0) {
                continue;
            }
            for (const uint32_t q : wake_adj_[k]) {
                if (cand[q] != 0 && all[q].self_start == all[k].self_start &&
                    all[q].reactive_home == all[k].reactive_home) {
                    uf[find(k)] = find(q);
                }
            }
        }
        // Component members in scheduled-rank order (sub_order_ iteration
        // yields them pre-sorted), then chunked at the group cap; a
        // leftover chunk of one stays a singleton. Only interior members
        // merge; components are still discovered over all candidates so
        // two interior members joined through a non-interior producer
        // land in one group.
        std::vector<std::vector<uint32_t>> comp(nprocs);
        for (const uint32_t k : sub_order_) {
            if (interior[k] != 0) {
                comp[find(k)].push_back(k);
            }
        }
        bool merged = false;
        comb_group_sizes_.clear();
        for (const auto& c : comp) {
            for (size_t off = 0; off + 1 < c.size(); off += group_cap) {
                const size_t end = std::min(c.size(), off + group_cap);
                if (end - off < 2) {
                    break;
                }
                const uint32_t g0 = group_of_[c[off]];
                for (size_t j = off + 1; j < end; ++j) {
                    const uint32_t m = c[j];
                    groups_[group_of_[m]].clear();
                    groups_[g0].push_back(m);
                    group_of_[m] = g0;
                    merged       = true;
                }
                comb_group_sizes_.push_back(static_cast<uint32_t>(end - off));
            }
        }
        // Compact the emptied singleton slots.
        if (merged) {
            std::vector<uint32_t> remap(groups_.size(), 0);
            uint32_t              w = 0;
            for (uint32_t g = 0; g < groups_.size(); ++g) {
                if (!groups_[g].empty()) {
                    remap[g] = w;
                    if (w != g) {
                        groups_[w] = std::move(groups_[g]);
                    }
                    ++w;
                }
            }
            groups_.resize(w);
            for (uint32_t k = 0; k < nprocs; ++k) {
                group_of_[k] = remap[group_of_[k]];
            }
        }
    }

    // Members run in scheduled-rank order (the order the unfused FIFO would
    // have popped them); the group subscribes at its earliest member's rank.
    group_events_.resize(groups_.size());
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        std::sort(groups_[g].begin(), groups_[g].end(),
                  [&](uint32_t a, uint32_t b) { return pos[a] < pos[b]; });
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
        // Fused: subscribe the canonical deduped union of member events
        // once (wait_counts sizes the buckets to match). For V1 groups
        // the union equals the front member's set; comb groups need the
        // true union.
        std::vector<WaitEvent> evs;
        for (const uint32_t m : groups_[g]) {
            evs.insert(evs.end(), all[m].wait_events.begin(), all[m].wait_events.end());
        }
        for (const auto& [s, e] : canonical(evs)) {
            group_events_[g].push_back(WaitEvent{ static_cast<Signal>(s), static_cast<Edge>(e) });
        }
    }

    // A group is thunk-eligible when its (front) member is a suspension-free
    // always-with-sensitivity: fused members all passed is_simple_always, so
    // the front's verdict holds for the group. Computed here so lower_group
    // and the seed/drain read one table. Empty is fine when --no-thunk.
    //
    // A member that can reach a $urandom draw is excluded whatever its shape:
    // a thunk has no frame to keep the xoshiro state in, so its stream would
    // restart on every activation. is_simple_always already rejects a body
    // holding a draw ITSELF (tag_info().blocks_thunk); this covers the ones
    // that reach one through a call.
    //
    // A reactive-home group is excluded too, for a tagged-pointer reason: its
    // static subscription has to carry kReactiveHomeTag (bit 1) so the wake
    // path picks the reactive ring, and a thunk "handle" is a function pointer
    // carrying the thunk tag (bit 0). The two tags are mutually exclusive by
    // construction -- see kReactiveHomeTag -- so a pointer holding both would
    // read as neither reactive (the test is bit 1 set AND bit 0 clear) nor as a
    // maskable thunk, and the drain would call two bytes past the function.
    // Costs a coroutine frame for each of a program's continuous assigns, a
    // handful of processes in a testbench; the thunk path exists for wide
    // combinational designs, which are Active-home by definition.
    thunk_group_.assign(groups_.size(), false);
    if (config_.thunk) {
        for (uint32_t g = 0; g < groups_.size(); ++g) {
            thunk_group_[g] =
              facts_.processes[groups_[g].front()].simple_always && !all[groups_[g].front()].reactive_home &&
              std::none_of(groups_[g].begin(), groups_[g].end(), [&](uint32_t m) { return rng_procs_[m]; });
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
// makes the mask a compile-time constant at the scan site instead.
//
// Colours are assigned greedily, first-fit, so two events of the same group
// can collide once the colour space is crowded. A collision costs PRECISION,
// never correctness: the two events share a bit, so one fires the other's
// members too -- exactly the over-evaluation the guard set out to remove, and
// exactly what the unguarded emission does today. `trig_collisions_` counts
// them for --stat.
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

    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const auto& members = groups_[g];
        if (members.size() < 2 || group_events_[g].empty()) {
            continue;
        }
        // Identical-sensitivity groups (compute_fusion_groups phase 1) have
        // one sensitivity for the whole group, so every member's mask would
        // be the union and every guard would be vacuously true. Only a group
        // whose members differ -- a comb-fused component -- can gain.
        const bool differ = std::any_of(members.begin() + 1, members.end(), [&](uint32_t m) {
            return all[m].wait_events.size() != all[members.front()].wait_events.size() ||
                   !std::equal(all[m].wait_events.begin(), all[m].wait_events.end(),
                               all[members.front()].wait_events.begin(),
                               [](const WaitEvent& x, const WaitEvent& y) {
                                   return x.signal == y.signal && x.edge == y.edge;
                               });
        });
        if (!differ) {
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
                c = rotate;
                rotate = rotate == last_colour ? first_colour : rotate + 1;
                ++trig_collisions_;
            } else {
                used |= 1ull << c;
            }
            trig_colour_[static_cast<uint32_t>(w.signal)][static_cast<uint32_t>(w.edge)] =
              static_cast<uint8_t>(c);
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
// when no guarded group waits on it (which keeps that wake site's emission
// exactly the unguarded one).
uint64_t Backend::trig_bits(Signal sig, uint32_t edge) const {
    const uint32_t i = static_cast<uint32_t>(sig);
    if (i >= trig_colour_.size()) {
        return 0;
    }
    const uint8_t c = trig_colour_[i][edge];
    return c == 0 ? 0ull : 1ull << c;
}

// The active task queue: ready-to-run coroutine handles held in a fixed-size
// global used as a FIFO ring (active events may fire in any order per the LRM,
// so the discipline is free). FIFO over LIFO is deliberate: wakes enqueue
// behind the current batch, so combinational cascades settle breadth-first —
// each process runs at most once per wave and the worst case is bounded by
// net depth, where LIFO's depth-first re-entry is exponential on dense
// fan-in. It also runs same-instant processes in declaration order. Created
// once, lazily, so the timer callback (built while lowering a process) and
// emit_active_run share it.
// Number of fork-child bodies in the design = Σ fork widths (each fork child
// is one Function). Says whether the design forks at all; the ring bound needs
// the per-body activation counts below, not this raw total.
size_t Backend::fork_child_count() const {
    size_t n = 0;
    for (size_t k = 0; k < mod_.function_count(); ++k) {
        if (mod_.function(static_cast<FunctionId>(k)).is_fork_child) {
            ++n;
        }
    }
    return n;
}

// Whether the design forks at all. Memoized: every ring push asks, and the
// answer cannot change once the module is built.
bool Backend::design_forks() {
    if (!design_forks_.has_value()) {
        design_forks_ = fork_child_count() != 0;
    }
    return *design_forks_;
}

Backend::BodyActivations Backend::body_activations() const {
    const size_t nfn = mod_.function_count();
    // Edges into each function, as (source, callee) pairs. kProcessSource is
    // the source for a process body, whose multiplicity is fixed at one.
    static constexpr uint32_t kProcessSource = ~0u;
    // Spawned by a fork whose parent does not wait for it, so the number that
    // can be live at once is a run-time quantity: contributes the ceiling.
    static constexpr uint32_t kUnboundedSource = ~1u;
    struct Edge {
        uint32_t from;
        uint32_t to;
    };
    std::vector<Edge> edges;
    bool              imprecise = false;
    const auto        walk      = [&](const Ir& ir, uint32_t from) {
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto           i = static_cast<Index>(k);
            const Inst::Payload& p = ir.payload(i);
            switch (ir.tag(i)) {
                case Inst::Tag::TaskCall:
                    edges.push_back({ from, static_cast<uint32_t>(ir.extra_data<TaskCallInfo>(p.extra).func) });
                    break;
                case Inst::Tag::Call:
                    edges.push_back({ from, static_cast<uint32_t>(ir.extra_data<CallInfo>(p.extra).func) });
                    break;
                case Inst::Tag::TaskCallVirtual:
                case Inst::Tag::CallVirtual:
                    edges.push_back({ from, static_cast<uint32_t>(ir.extra_data<VirtualCallInfo>(p.extra).func) });
                    imprecise = true;
                    break;
                case Inst::Tag::Fork: {
                    // `join` blocks the parent until every child is done, so a
                    // site contributes one live child per activation of the body
                    // containing it. The other two kinds do not wait, so a loop
                    // around the fork can have any number live at once.
                    const auto& info = ir.extra_data<ForkInfo>(p.extra);
                    const uint32_t src =
                      info.kind == ForkKind::Join ? from : kUnboundedSource;
                    for (const Index kid : ir.region(info.children)) {
                        edges.push_back({ src, static_cast<uint32_t>(kid) });
                    }
                    break;
                }
                default:
                    break;
            }
        }
    };
    for (const Process& p : mod_.processes()) {
        walk(p.ir, kProcessSource);
    }
    for (size_t f = 0; f < nfn; ++f) {
        walk(mod_.function(static_cast<FunctionId>(f)).ir, static_cast<uint32_t>(f));
    }

    // Reachable-from-a-fork-child, relaxed over the same edge set. Seeded with
    // the fork children themselves; a body a child calls is just as uncounted
    // by the process-chain bound as the child is.
    std::vector<uint8_t> from_fork(nfn, 0);
    for (size_t f = 0; f < nfn; ++f) {
        from_fork[f] = mod_.function(static_cast<FunctionId>(f)).is_fork_child ? 1 : 0;
    }
    const auto synthetic = [](uint32_t from) {
        return from == kProcessSource || from == kUnboundedSource;
    };
    for (bool grew = true; grew;) {
        grew = false;
        for (const Edge& e : edges) {
            // A child of an unbounded fork is a fork child itself, so it is
            // already seeded; only real body-to-body edges propagate.
            if (!synthetic(e.from) && from_fork[e.from] != 0 && from_fork[e.to] == 0) {
                from_fork[e.to] = 1;
                grew            = true;
            }
        }
    }

    std::vector<size_t> mult(nfn, 0);
    bool                converged = false;
    for (size_t round = 0; !converged && round <= nfn; ++round) {
        std::vector<size_t> next(nfn, 0);
        for (const Edge& e : edges) {
            const size_t src = e.from == kProcessSource     ? 1
                             : e.from == kUnboundedSource   ? kMaxForkRingSlots
                                                            : mult[e.from];
            next[e.to]       = std::min(next[e.to] + src, kMaxForkRingSlots);
        }
        converged = next == mult;
        mult      = std::move(next);
    }
    if (!converged || imprecise) {
        return { std::vector<size_t>(nfn, kMaxForkRingSlots), std::move(from_fork), false };
    }
    return { std::move(mult), std::move(from_fork), true };
}

// The ring slots `fork … join` needs beyond the one-per-process baseline:
// Σ over fork-child bodies of their live activations (see body_activations).
size_t Backend::fork_ring_slots() const {
    if (fork_child_count() == 0) {
        return 0;
    }
    const BodyActivations act = body_activations();
    if (!act.exact) {
        return kMaxForkRingSlots;
    }
    size_t slots = 0;
    for (size_t f = 0; f < mod_.function_count(); ++f) {
        if (mod_.function(static_cast<FunctionId>(f)).is_fork_child) {
            slots = std::min(slots + act.mult[f], kMaxForkRingSlots);
        }
    }
    return slots;
}

// Whether a coroutine frame for this body can hold a reference into the collected
// heap, and therefore MUST be registered. Deliberately asymmetric: a false
// positive costs two out-of-line calls per activation, a false negative is a
// use-after-free the first time a collection runs while the frame holds the only
// reference to an object -- so anything not provably safe answers true.
//
// A frame word comes to hold a heap reference through exactly five doors, and the
// list is exhaustive because a coroutine frame holds precisely the coroutine's
// allocas plus the SSA values live across a suspend, nothing else:
//
//   1. a declared local, whose alloca CoroSplit hoists into the frame;
//   2. a formal, including a method's implicit `this` (lower/Subroutines.cpp, splice_this). The
//      parameter copies are frame locals as well, but an output / inout / ref
//      formal's slot holds a pointer instead of the value, so both lists are
//      walked rather than assuming one covers the other;
//   3. a `ref` formal, whose ACTUAL the callee cannot see. A `ref` bound to a
//      class property is a raw interior pointer into an object body carrying
//      clotho.ref.noepoch (:3587-3606) -- a reference the frame holds with no
//      class-typed value anywhere in sight, and one rt_gc_find_object resolves
//      back to its object precisely so it needs no epoch machinery
//      (Gc.h:281-287). Unconditional, because the direction is caller-chosen:
//      that includes a fork child, whose every formal is an ArgDir::Ref capture
//      of an enclosing frame slot (lower/Statements.cpp, lower_fork) and which may capture a
//      formal that is itself a `ref` bound to an object interior;
//   4. an SSA temporary CoroSplit spills at an offset nobody controls, which is
//      why the last test is over VALUES and not only over declared storage;
//   5. a local or formal whose type is, or contains, a CONTAINER whose contents
//      can reach a class handle. This door was closed by fiat until P4: the shape
//      was refused outright, so an earlier draft of this comment could argue that
//      no container element is ever a reference. elem_ops_for now describes it
//      instead (docs/design/garbage-collection.md §3.3), so the door is real, and
//      it is a door of its own rather than a case of 1 and 2 because the reference
//      is not IN the frame at all: the frame holds a handle to a malloc'd payload
//      that holds the handles, which a conservative scan of the frame cannot
//      follow (Gc.h:228-232). Registering the frame is still required, because the
//      container's own rt_gc_container registration covers the payload while the
//      SAME activation may separately spill a bare handle loaded out of it.
//
// Doors 1, 2 and 5 are one test: reaches_class_handle is the transitive question,
// so it answers true for a bare handle, for a handle inside an aggregate, AND for
// a container whose elements or keys can reach one. It replaced a storage-only
// walk (holds_class_handle) that answered false for `Cell q[$]` -- which was
// sound only while the shape was refused.
bool Backend::frame_may_hold_gc_ref(const Function& f) {
    assert((f.arg_dirs.empty() || f.arg_dirs.size() == f.param_types.size()) &&
           "arg_dirs is absent or parallel to param_types (Module.h:534-537)");
    if (std::ranges::find(f.arg_dirs, ArgDir::Ref) != f.arg_dirs.end()) {
        return true; // door 3
    }
    for (const Ref t : f.local_types) {
        if (reaches_class_handle(t)) {
            return true; // doors 1 and 5
        }
    }
    for (const Ref t : f.param_types) {
        if (reaches_class_handle(t)) {
            return true; // doors 2 and 5
        }
    }
    return ir_may_hold_gc_ref(f.ir); // door 4
}

// The same question for a process body. A process declares no formal and no frame
// local -- its variables are module signals, which is why frame_slots_ is empty
// for one (:3677) and why the precise global root tables already cover that
// storage -- so the only door left open is door 4, the spilled temporary.
bool Backend::frame_may_hold_gc_ref(const Process& p) {
    return ir_may_hold_gc_ref(p.ir);
}

// Door 4: does any instruction of this body produce or consume a value that is,
// or points into, a class object?
//
// Every class-typed value in a body traces back to one of five producers, so
// testing those plus the class-specific consumers covers every temporary
// CoroSplit could spill:
//   * `new` (ClassNew);
//   * a read of class-typed module storage (Signal -- a module-scope handle
//     variable, or a static class property block);
//   * a call whose return type is a handle (Call), or any virtual dispatch;
//   * a reference to a formal or a declared local of handle type (Arg / Local);
//   * `process::self()` (ProcQuery), which reads no storage and calls nothing: its
//     object is a collected class body (docs/design/process-class.md §4.2) that only
//     a handle in a frame word may keep alive across a suspend.
// Everything else -- HandleDeref, ClassIsA, a store, a `$cast`, passing an
// argument -- can only forward a handle one of those already produced. TaskCall
// needs no case of its own: a task returns nothing (LRM 13.3) and writes its
// output / inout formals through pointers to the CALLER's storage, which the
// caller names with its own Local / Arg / Signal.
//
// A flat scan of the instruction column, like ir_has_expect_wait (:9942): the
// region tree is irrelevant here, only whether a tag occurs at all.
bool Backend::ir_may_hold_gc_ref(const Ir& ir) {
    const size_t n = ir.inst_count();
    for (size_t k = 0; k < n; ++k) {
        const auto           i = static_cast<Index>(k);
        const Inst::Payload& p = ir.payload(i);
        switch (ir.tag(i)) {
            case Inst::Tag::HandleDeref:
            case Inst::Tag::ClassIsA:
            case Inst::Tag::ClassImplements:
            // A virtual dispatch loads the vtable out of its receiver's object
            // body (load_vtable_callee), so it always has a handle in hand.
            case Inst::Tag::CallVirtual:
            case Inst::Tag::TaskCallVirtual:
                return true;
            case Inst::Tag::Signal:
                if (reaches_class_handle(mod_.signal(p.signal).type)) {
                    return true;
                }
                break;
            case Inst::Tag::Local:
            case Inst::Tag::Arg:
                // Redundant for a task, whose local_types / param_types the
                // caller already walked, but NOT for a process: a process has no
                // frame-local table (:3313), so an `automatic` handle in an always
                // block reaches its frame as a spilled temporary and this is the
                // only place its declared type is visible.
                if (reaches_class_handle(ir.tag(i) == Inst::Tag::Local ? p.local.type
                                                                      : p.arg.type)) {
                    return true;
                }
                break;
            case Inst::Tag::Call: {
                const Ref ret =
                  mod_.function(ir.extra_data<CallInfo>(p.extra).func).return_type;
                // A void return has no type image to walk at all; any other return
                // type is a value the caller holds.
                if (ret != mod_.pool.void_type && reaches_class_handle(ret)) {
                    return true;
                }
                break;
            }
            case Inst::Tag::ProcQuery:
                // Only `self()` yields a handle; `status()` is a state number and
                // `get_randstate()` a string.
                if (ir.extra_data<ProcCtlInfo>(p.extra).op == ProcOp::Self) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

// Does any process or subroutine in the design contain a $finish / $stop --
// or can the design end by LRM 24.3's implicit one? Gates the drain loop's
// finished test; memoized, since emit_active_run asks once per unit module.
//
// A program `initial` counts even though no body holds an Inst::Tag::Finish:
// 24.3's "the entire simulation shall terminate by means of an implicit call
// to the $finish system task" is emitted by emit_program_end, and it stops the
// run the same cooperative way an explicit $finish does -- set rt_finishing,
// empty the rings, and let each drain notice on its empty edge. Without this
// the drains never test the flag, so a design whose only way to end is the
// implicit finish (no explicit $finish anywhere) ran forever.
bool Backend::design_has_finish() {
    if (!design_has_finish_.has_value()) {
        const auto scan = [](const Ir& ir) {
            for (size_t k = 0; k < ir.inst_count(); ++k) {
                if (ir.tag(static_cast<Index>(k)) == Inst::Tag::Finish) {
                    return true;
                }
            }
            return false;
        };
        bool found = has_program_initial();
        for (const Process& p : mod_.processes()) {
            found = found || scan(p.ir);
        }
        for (uint32_t f = 0; f < mod_.function_count() && !found; ++f) {
            found = found || scan(mod_.function(static_cast<FunctionId>(f)).ir);
        }
        design_has_finish_ = found;
    }
    return *design_has_finish_;
}

} // namespace clotho::ir::backend
