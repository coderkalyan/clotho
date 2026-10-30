// Instance hierarchy: the Collect/Classify/Emit walk, generate blocks, interfaces and port wiring.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Flatten one elaborated instance into the module under `prefix` (empty for
// the top, "<inst>." per level below). In Collect: open this instance's
// hierarchy scope, register its signals, recurse to collect children. In
// Emit: `node` is this instance's already-built scope; lower its processes
// and recurse. Repeated instantiations disambiguate for free — slang gives
// each instance distinct ValueSymbols, so sigmap_ (keyed by symbol) maps
// each to its own signal.
void Lowerer::lower_instance(const InstanceSymbol& inst,
                             std::string_view      prefix,
                             Pass                  pass,
                             ir::InstanceScope*    node,
                             std::string_view      leaf_name) {
    if (prefix.empty() && pass == Pass::Collect) {
        module_.name = std::string(inst.name);
    }
    const std::string         self_prefix(prefix);
    const InstanceBodySymbol& body = inst.body;

    // A program (LRM 24) elaborates exactly like a module -- same scope, same
    // signals, same subroutines -- and differs only in how its procedures are
    // scheduled: an `initial` runs in the Reactive region, not the Active one.
    // So the whole instance path is reused and only this flag is threaded
    // through, read by lower_procedural_block.
    //
    // slang already enforces most of 24.3 for us: an `always` procedure, a
    // nested program declaration and a member of an anonymous program are all
    // rejected as "member not allowed in program declaration", and
    // instantiating a program inside a program is rejected outright.
    const bool is_program = inst.getDefinition().definitionKind == slang::ast::DefinitionKind::Program;
    // Program PORTS are module ports plus exactly one region rule, and both
    // halves are now in place, so nothing about a port is refused here.
    //
    // LRM 24.3: "Program port declaration syntax and semantics are the same as
    // those of modules (see 23.2.2)", so a program's connections elaborate
    // through build_inout_aliases and wire_ports unchanged. LRM 24.3.2 adds
    // only WHERE the copy runs: "variables on the other side of a program port
    // connection are updated in the reactive region set. Similarly, the driving
    // and resolution of nets on the other side of a program port connection
    // also occurs in the reactive region set. [...] Design processes sensitive
    // to those cross-region variables and nets are scheduled for wake up in the
    // active region set." wire_ports marks the copies it emits for a program
    // child reactive-home for that sentence; the input direction is 24.3.1's
    // own `assign tclk = clk;`, a construct inside the program sensitive to a
    // design signal, which that clause puts in the Reactive region too.
    //
    // By port kind, all four shapes need no instance-level check:
    //  - interface / modport: no copy process exists at all, since
    //    canonical_value_symbol resolves both sides onto the interface
    //    instance's own signal (docs/design/interfaces.md), so the program's
    //    reactive code reads and writes it exactly as it does through a
    //    hierarchical name -- which is already how the landed tests reach it;
    //  - input / output value port: a reactive-home copy (wire_ports);
    //  - inout / ref: build_inout_aliases collapses both sides onto one signal,
    //    so again no copy -- the program writes the shared signal from Reactive
    //    and design readers wake into Active by the ordinary wake path, which
    //    is 24.3.2's last sentence verbatim;
    //  - unconnected: no copy process either way, which is also why a
    //    TOP-LEVEL program with ports needs no special case. A top instance has
    //    no port connections at all, so there is nothing to copy and nothing to
    //    classify. (The refusal this replaced keyed on connections and so let
    //    that case through for the wrong reason.)
    //
    // A multiply-driven net on either side of the boundary is no longer a
    // refusal: its resolver is reactive-home when any driver is program-side
    // (alloc_driver_slot / emit_wire_resolvers, phase A5 of
    // docs/design/program-blocks.md). What is still refused lives where the
    // answer is known rather than here: the shape refusals wire_ports /
    // build_inout_aliases already apply to any instance.

    // Collect builds the scope node; Emit reuses the node Collect built.
    // A parent's children vector only grows during Collect (before this
    // child pops), so stacked pointers stay stable across that recursion;
    // by Emit the tree is frozen, so a child-node pointer is stable too.
    ir::InstanceScope* self = node;
    if (pass == Pass::Collect) {
        ir::InstanceScope fresh;
        fresh.id = module_.alloc_scope_id();
        // The scope-node name must be unique among an instance's children
        // and feeds the backend's per-instance symbol anchor
        // (@clotho.inst.<hier>): `node_rel_` carries the generate-block
        // segments (loop[i]/cond-block) since the enclosing instance node,
        // which folds no node of its own (see lower_generate_block), so two
        // instances of the same name in distinct generate entries do not
        // collide. `leaf_name` is the instance segment (its own name, or
        // arr[i] for an instance array).
        fresh.name     = node_rel_ + std::string(leaf_name);
        fresh.def_name = std::string(inst.getDefinition().name);
        if (scope_stack_.empty()) {
            top_scope_ = std::move(fresh);
            self       = &top_scope_;
        } else {
            scope_stack_.back()->children.push_back(std::move(fresh));
            self = &scope_stack_.back()->children.back();
        }
    }
    scope_stack_.push_back(self);
    // This instance's own members are named relative to it, so the
    // generate-path accumulator resets here and is restored after the walk.
    const std::string saved_node_rel = node_rel_;
    node_rel_.clear();

    // This instance's delay / $time multiplier: its time unit divided by the
    // wheel tick. Both are powers of ten and the unit is never finer than the
    // tick, so the ratio is an exact integer (LRM §3.14). `prec_over_tick_` is
    // the same for the module's precision — the tick divides the precision of
    // any module carrying a real delay (design_tick_fs folds it in), so a real
    // delay rounds to precision then scales by this to ticks. Both stay 1 in
    // unitless mode. Saved here so the post-recursion restore below (with
    // prefix_ / scope_) returns them to this level after a child runs.
    if (tick_fs_ != 0) {
        apply_time_scale(body);
    }
    const uint64_t self_mul = time_mul_;

    prefix_ = self_prefix;
    scope_  = &body;
    if (pass == Pass::Collect) {
        // Alias this instance's inout/ref port internals to their parent
        // connection signals BEFORE collect_signals registers them, so the
        // shared storage is set up when the child body's signals are created.
        build_inout_aliases(inst);
        collect_signals(body);
    }
    size_t child_cursor = 0;
    // Restored after the walk: a program's members lower under the flag, and
    // the module that instantiates it must not inherit it.
    const bool saved_in_program = in_program_;
    in_program_                 = is_program;
    lower_members(body, self_prefix, self_mul, pass, *self, child_cursor);
    in_program_ = saved_in_program;
    if (pass == Pass::Collect && inst.isInterface()) {
        register_iface_instance(body, self->id);
    }

    node_rel_ = saved_node_rel;
    scope_stack_.pop_back();
}

// Every signal of one interface instance that a handle can reach, including
// those declared inside a generate block of the interface body. A generate
// block is not a separate instance -- its signals are fields of this
// instance's own state struct, on this instance's anchor -- so a handle
// reaches them exactly like a top-level member. A nested interface INSTANCE is
// not walked: it owns a separate state struct and a separate context record,
// which this handle's record says nothing about (see vif_member_signal).
void Lowerer::collect_iface_members(const slang::ast::Scope& scope, VifInstance& rec, bool top_level) {
    for (const slang::ast::Symbol& s : scope.members()) {
        if (const auto* vs = s.as_if<slang::ast::ValueSymbol>()) {
            if (const auto it = sigmap_.find(vs); it != sigmap_.end()) {
                rec.syms.emplace(vs, it->second);
                if (top_level) {
                    rec.members.emplace(std::string(vs->name), it->second);
                }
            }
            continue;
        }
        if (s.kind == SymbolKind::InstanceBody || s.kind == SymbolKind::Instance) {
            continue;
        }
        if (const auto* child = s.as_if<slang::ast::Scope>()) {
            collect_iface_members(*child, rec, /*top_level=*/false);
        }
    }
}

// The position of `m` inside its interface body, as the chain of member
// ordinals from the body down. Two instances of one definition with equal
// parameter values elaborate identically -- that is exactly what
// `InstanceBodySymbol::hasSameType` asserts -- so the same chain reaches the
// corresponding member of any of them. Used instead of a rendered path name
// because a generate block's naming is clotho's own, not slang's.
std::vector<uint32_t> Lowerer::member_ordinal_path(const slang::ast::Symbol& m, const slang::ast::Scope& body) {
    std::vector<uint32_t>     rev;
    const slang::ast::Symbol* cur = &m;
    while (true) {
        const slang::ast::Scope* parent = cur->getParentScope();
        if (parent == nullptr) {
            return {};
        }
        uint32_t ord   = 0;
        bool     found = false;
        for (const slang::ast::Symbol& s : parent->members()) {
            if (&s == cur) {
                found = true;
                break;
            }
            ++ord;
        }
        if (!found) {
            return {};
        }
        rev.push_back(ord);
        if (parent == &body) {
            std::reverse(rev.begin(), rev.end());
            return rev;
        }
        cur = &parent->asSymbol();
    }
}

// Walk an ordinal path from `body` to the symbol it names, or null.
const slang::ast::Symbol* Lowerer::symbol_at_path(const slang::ast::Scope& body, const std::vector<uint32_t>& path) {
    const slang::ast::Scope* scope = &body;
    for (size_t k = 0; k < path.size(); ++k) {
        uint32_t                  ord = 0;
        const slang::ast::Symbol* hit = nullptr;
        for (const slang::ast::Symbol& s : scope->members()) {
            if (ord++ == path[k]) {
                hit = &s;
                break;
            }
        }
        if (hit == nullptr) {
            return nullptr;
        }
        if (k + 1 == path.size()) {
            return hit;
        }
        scope = hit->as_if<slang::ast::Scope>();
        if (scope == nullptr) {
            return nullptr;
        }
    }
    return nullptr;
}

// Record an elaborated interface instance so a virtual interface handle can
// name it (see VifDefs). Its members are captured by name from this Lowerer's
// sigmap_, which the Collect walk above has just filled -- a later Lowerer
// resolving a handle through this definition reads them from here rather than
// from its own map, which never saw this instance.
void Lowerer::register_iface_instance(const slang::ast::InstanceBodySymbol& body, uint32_t scope) {
    VifInstance rec;
    rec.body  = &body;
    rec.scope = scope;
    collect_iface_members(body, rec, /*top_level=*/true);
    // A clocking block's storage is not a body member: its event and its
    // clockvars live in the block's own scope. collect_clocking_block has
    // already created all of it (the Collect walk above), and every piece is a
    // signal of THIS instance's scope, so a handle reaches them like any other
    // member -- see docs/design/interfaces.md.
    for (const auto& cb : body.membersOfType<slang::ast::ClockingBlockSymbol>()) {
        const auto st = clocking_.find(&cb);
        if (st == clocking_.end()) {
            continue; // refused during collection; the reader fails on its own
        }
        rec.members.emplace(std::string(cb.name), st->second.event);
        for (const ClockVarState& cv : st->second.vars) {
            rec.clockvars.emplace(std::string(cb.name) + "." + std::string(cv.sym->name),
                                  VifClockVar{ .sampled    = cv.sampled,
                                               .drive_val  = cv.drive_val,
                                               .drive_pend = cv.drive_pend,
                                               .drives     = cv.drives });
        }
    }
    vif_defs_->insts.push_back(std::move(rec));
}

// The representative's record for a clockvar reached through a handle, or
// nullopt when no instance of the interface exists (the caller then emits the
// null-handle fatal, as for any other member).
std::optional<VifClockVar> Lowerer::vif_clockvar(const slang::ast::ClockVarSymbol&     cv,
                                                 const slang::ast::InstanceBodySymbol* named,
                                                 std::string_view                      what) {
    const auto& cb = cv.getParentScope()->asSymbol().as<slang::ast::ClockingBlockSymbol>();
    const auto& placeholder =
      cb.getParentScope()->asSymbol().as<slang::ast::InstanceBodySymbol>();
    // Same rule as an ordinary member: the block must belong to the body the
    // handle's type names, not to an interface instantiated inside it.
    if (named != nullptr && !named->hasSameType(placeholder)) {
        fail(std::string(what) + " through a virtual interface is not supported: clocking block '" +
             std::string(cb.name) + "' belongs to interface '" +
             std::string(placeholder.getDefinition().name) + "' instantiated inside '" +
             std::string(named->getDefinition().name) +
             "', a separate instance the handle does not name");
    }
    const std::string key = std::string(cb.name) + "." + std::string(cv.name);
    for (const VifInstance& inst : vif_defs_->insts) {
        if (!inst.body->hasSameType(placeholder)) {
            continue;
        }
        if (const auto it = inst.clockvars.find(key); it != inst.clockvars.end()) {
            return it->second;
        }
        fail(std::string(what) + " through a virtual interface is not supported: clocking block '" +
             std::string(cb.name) + "' of interface '" + std::string(placeholder.getDefinition().name) +
             "' provides no clockvar '" + std::string(cv.name) + "'");
    }
    report(Severity::Warning,
           "virtual-interface-never-bound",
           "no instance of interface '" + std::string(placeholder.getDefinition().name) +
             "' exists in the design, so this virtual interface is null for the whole run and " +
             std::string(what) + " through it is a run-time fatal",
           to_source_span(cur_range_));
    return std::nullopt;
}

// The place a member access resolves to when the handle is provably null for
// the whole run (no instance of its interface exists). LRM 25.9 makes using a
// null virtual interface a fatal run-time error, so that is what this emits;
// the throwaway storage exists only to give the surrounding read or write a
// well-formed target on the unreachable path after it.
ir::Index Lowerer::vif_null_place(const slang::ast::Type& type, ir::Builder& b, ir::SourceLoc l) {
    const std::string banner = banner_at(cur_range_.start());
    b.severity(static_cast<uint32_t>(ir::Severity::Fatal),
               severity_banner(cur_range_.start()),
               "use of a null virtual interface",
               {},
               l);
    b.finish(banner + "virtual interface", 1, l);
    return b.signal(declare_temp(lower_type(type), "vif_null"), l);
}

// The InstanceScope id of an elaborated interface instance, for a handle that
// names it. Registered by register_iface_instance during Collect, so every
// instance in the design is present by the time any body lowers.
uint32_t Lowerer::iface_scope_of(const InstanceSymbol& inst) {
    for (const VifInstance& rec : vif_defs_->insts) {
        if (rec.body == &inst.body) {
            return rec.scope;
        }
    }
    fail("virtual interface assignment from '" + std::string(inst.name) +
         "' is not supported: it is not an elaborated interface instance");
}

// The interface body a virtual interface handle's TYPE names (LRM 25.9): the
// placeholder instance slang caches per (definition, parameter values). Null
// when the expression is not a handle, which no caller here allows.
const slang::ast::InstanceBodySymbol* Lowerer::vif_named_body(const Expression& handle) {
    const slang::ast::Type& t = handle.type->getCanonicalType();
    if (!t.isVirtualInterface()) {
        return nullptr;
    }
    return &t.as<slang::ast::VirtualInterfaceType>().iface.body;
}

// The representative signal for a member reached through a virtual interface:
// some real instance's copy of it. Every instance of one definition lays its
// members out identically, so which one is picked does not matter -- it only
// has to be a member of the body the handle's type names, which `named` is.
std::optional<ir::Signal> Lowerer::vif_member_signal(const slang::ast::Symbol&             member,
                                                     const slang::ast::InstanceBodySymbol* named,
                                                     std::string_view                      what) {
    const slang::ast::Symbol* m = &member;
    if (const auto* mp = m->as_if<slang::ast::ModportPortSymbol>()) {
        // A modport-qualified handle resolves to the modport's port symbol;
        // the storage is the interface's own member behind it.
        if (mp->internalSymbol == nullptr) {
            fail("a modport expression port reached through a virtual interface is not supported");
        }
        m = mp->internalSymbol;
    }
    if (const auto* cv = m->as_if<slang::ast::ClockVarSymbol>()) {
        // `vif.cb.d` (LRM 25.9.1): the block's sampled storage for that
        // clockvar, which is an ordinary per-instance signal in the interface's
        // scope. An output-only clockvar has none -- slang rejects reading one
        // (ClockVarOutputRead), so reaching here with a zero signal would be a
        // write, which routes through the drive path instead.
        const std::optional<VifClockVar> rec = vif_clockvar(*cv, named, what);
        if (!rec) {
            return std::nullopt;
        }
        if (rec->sampled == ir::Signal{ 0 }) {
            fail(std::string(what) + " through a virtual interface is not readable: it is an "
                 "output-only clockvar");
        }
        return rec->sampled;
    }
    const slang::ast::Scope* owner = m->getParentScope();
    while (owner != nullptr && owner->asSymbol().kind != SymbolKind::InstanceBody) {
        owner = owner->asSymbol().getParentScope();
    }
    if (owner == nullptr) {
        fail("internal: virtual interface member outside an interface body");
    }
    const auto& placeholder = owner->asSymbol().as<slang::ast::InstanceBodySymbol>();
    // The body the member lives in must be the body the HANDLE'S TYPE names.
    // `vif.i.d`, where `i` is an interface instantiated inside the interface,
    // reaches a nested instance: a separate state struct with a separate
    // context record, which this handle's record does not describe. Matching it
    // against the registry by type alone would resolve the nested instance's
    // own signal and then read it off the outer instance's record -- the wrong
    // layout off the wrong base.
    //
    // TODO: a member of an interface instantiated INSIDE the interface a handle
    // names (LRM 25.9) -- needs the context record to carry its children's
    // records, so the nested instance's base can be reached from the outer's.
    if (named != nullptr && !named->hasSameType(placeholder)) {
        fail(std::string(what) + " through a virtual interface is not supported: it is a member "
             "of interface '" + std::string(placeholder.getDefinition().name) +
             "' instantiated inside '" + std::string(named->getDefinition().name) +
             "', a separate instance the handle does not name");
    }
    const std::vector<uint32_t> path = member_ordinal_path(*m, placeholder);
    for (const VifInstance& inst : vif_defs_->insts) {
        if (!inst.body->hasSameType(placeholder)) {
            continue;
        }
        // Top-level members resolve by name; anything deeper (a generate block's
        // signal) by its position, which is identical across instances of one
        // definition and needs no path rendering.
        if (path.size() == 1) {
            if (const auto it = inst.members.find(std::string(m->name)); it != inst.members.end()) {
                return it->second;
            }
        } else if (!path.empty()) {
            if (const slang::ast::Symbol* rep = symbol_at_path(*inst.body, path)) {
                if (const auto* vs = rep->as_if<slang::ast::ValueSymbol>()) {
                    if (const auto it = inst.syms.find(vs); it != inst.syms.end()) {
                        return it->second;
                    }
                }
            }
        }
        break;
    }
    // No elaborated instance of this interface exists anywhere in the design.
    // Nothing can ever be assigned to the handle, so it is null for the whole
    // run and every access through it is the LRM 25.9 run-time fatal. There is
    // also no layout to resolve the member against, so the caller emits that
    // fatal directly rather than an access.
    //
    // Warned rather than refused: the design is legal and simulates, and this
    // is a fact clotho proved about it (DiagnosticOrigin::Analysis).
    //
    // TODO: suppression. Clotho's own diagnostic codes have no `-Wno-<code>`
    // path yet (apply_severity_policy re-ranks slang's engine only), so a
    // deliberate forward declaration cannot silence this.
    for (const VifInstance& inst : vif_defs_->insts) {
        if (inst.body->hasSameType(placeholder)) {
            // An instance DOES exist; the member itself is what has no storage
            // (a parameter, or a shape with no lowering here yet).
            fail(std::string(what) + " through a virtual interface is not supported: interface '" +
                 std::string(placeholder.getDefinition().name) + "' provides no signal '" +
                 std::string(m->name) + "'");
        }
    }
    report(Severity::Warning,
           "virtual-interface-never-bound",
           "no instance of interface '" + std::string(placeholder.getDefinition().name) +
             "' exists in the design, so this virtual interface is null for the whole run and " +
             std::string(what) + " through it is a run-time fatal",
           to_source_span(cur_range_));
    return std::nullopt;
}

// Lower one top instance and its whole hierarchy, then emit the accumulated
// declaration initializers as this Lowerer's pre-init process. finalize_preinit
// runs here, per top, rather than inside lower_instance: a multi-top design
// gives every top a "<name>." prefix, so gating on an empty prefix would drop
// all initializers (each top gets its own Lowerer, so this covers each one).
void Lowerer::lower_top(const InstanceSymbol& top, std::string_view prefix) {
    top_elided_    = prefix.empty();
    top_inst_      = &top;
    reads_scanned_ = false;
    lower_instance(top, prefix, Pass::Collect, nullptr, top.name);
    // Between the passes: Collect has just enumerated every assertion and
    // said whether the design calls an LRM 20.12 control task, and the
    // scope tree it built is now frozen, so the control state can be
    // allocated into the right scope. Emit needs it to already exist --
    // both the monitors that read it and the tasks that write it lower
    // there, in an order neither can depend on.
    alloc_assert_control();
    // Classify reuses the scope tree Collect built (Emit has not yet moved
    // it out); it only reads node pointers to walk children in order.
    lower_instance(top, prefix, Pass::Classify, &top_scope_, top.name);
    lower_instance(top, prefix, Pass::Emit, &top_scope_, top.name);
    emit_wire_resolvers();
    // Static class property initializers of classes declared under this top; a
    // package / $unit class already ran on the globals Lowerer, whose sigmap_
    // seed makes the redeclaration here a cache hit. After Emit so the module
    // signals a module-scope class's initializer may read exist and are
    // initialized; before drain_pending_bodies, which builds any method body an
    // initializer's call pulled in. The signals land in this top's root scope
    // node so the VCD representability check still sees them.
    prefix_ = prefix;
    scope_stack_.push_back(&top_scope_);
    init_static_class_props(top.body);
    // And the package / $unit generics this top specializes with its own classes,
    // which the globals pass left to it.
    init_local_class_specializations(top);
    scope_stack_.pop_back();
    // Every body compute_vtable reserved while lowering this top's types, now
    // that the top's signals and scope tree are complete. Before the finalizers,
    // which close the processes a body's own lowering can still contribute to.
    drain_pending_bodies();
    // The drain above pulls the shared globals Lowerer's queue too. Its
    // vtable-forced package/$unit class-method bodies wait for THIS boundary
    // on purpose (see lower_globals): only now has Collect registered the
    // interface instances this top elaborates, so a virtual-interface member
    // access inside a class task resolves instead of warning never-bound. A
    // body drained here may append function-static initializers to the
    // globals PreInit builder, which lower_globals already finalized; flush
    // them into a process of their own, still ahead of this top's preinit.
    if (globals_ != nullptr && globals_ != this) {
        globals_->finalize_preinit();
    }
    // Before finalize_preinit, which closes the process the seeds go into.
    finalize_sva_history();
    finalize_preinit();
    finalize_preponed();
    module_.add_top(std::move(top_scope_));
}

// Register the persistent state a package or the `$unit` compilation-unit
// scope declares (LRM 6.8): a package/$unit variable has no instance
// multiplicity, so it gets exactly one signal, shared by every top instance
// that references it. Must run once, on a Lowerer of its own, before any
// top's own Lowerer starts (see lower_compilation) so `sigmap()` can seed
// every top's map with these symbols; its own declaration initializers land
// in this Lowerer's PreInit process, so a package variable initializes
// exactly once regardless of how many tops import it.
void Lowerer::lower_globals(const Compilation& comp, const RootSymbol& root) {
    // Three phases, in this order, because each one lowers code that may read
    // whatever the previous one produced. Registration only needs types, so it
    // cannot observe a half-built anything; an initializer can call a function
    // that reads any global, so every global has to exist first; and a class
    // static's initializer may read any of those, so classes follow the
    // variables. Doing these per-variable, as one loop, is what left
    // `uvm_object::m_set_local_rsrc` unregistered when a variable declared
    // above it pulled its class's vtable in.
    //
    // A VTABLE-FORCED method body (function_id under defer_bodies_) is NOT
    // drained here, on purpose: a package/$unit class task may wait on or
    // assign through a virtual interface member, and the interface instances a
    // handle can name are registered by each top's Collect pass, which has not
    // run yet. The first top's drain_pending_bodies pulls this Lowerer's queue
    // (and lower_compilation drains any remainder), so those bodies build once
    // the registry is complete. Initializer EXPRESSIONS still lower right
    // here -- only bodies they merely reserved wait.
    for (const PackageSymbol* pkg : comp.getPackages()) {
        register_global_scope_signals(*pkg);
    }
    for (const CompilationUnitSymbol* unit : root.compilationUnits) {
        register_global_scope_signals(*unit);
    }
    for (const PackageSymbol* pkg : comp.getPackages()) {
        init_global_scope_signals(*pkg);
    }
    for (const CompilationUnitSymbol* unit : root.compilationUnits) {
        init_global_scope_signals(*unit);
    }
    // Every static class property initializer of every package / $unit class,
    // including a parameterized specialization named only in a typedef: LRM 6.21
    // requires the initialization "only once at the beginning of simulation",
    // and LRM 8.9 lets a static property be used without any object existing,
    // so waiting for an access site (declare_static_prop's other callers) would
    // silently skip a property nobody reads. UVM's factory registration is
    // exactly that shape: uvm_registry.svh's `local static bit m__initialized =
    // __deferred_init()` has no reader at all. After the variable loops for the
    // same reason those are sequenced: an initializer here may call a function
    // reading any package variable.
    for (const PackageSymbol* pkg : comp.getPackages()) {
        init_static_class_props(*pkg);
    }
    for (const CompilationUnitSymbol* unit : root.compilationUnits) {
        init_static_class_props(*unit);
    }
    finalize_preinit();
}

// The globals Lowerer's last phase, run by lower_compilation once every top
// has lowered: build any package/$unit class-method body still queued (a
// design with no top instance, or a body the last top's own drain queued) --
// every top's interface instances are registered by now, which is what those
// bodies were deferred FOR (see lower_globals) -- then flush the
// function-static initializers they may have appended into a PreInit process
// of their own.
void Lowerer::finalize_globals() {
    drain_pending_bodies();
    finalize_preinit();
}

// Force declaration -- and thereby time-zero initialization -- of every static
// class property with an initializer declared under `scope`. declare_static_prop
// is idempotent through sigmap_, so an access site reaching the same property
// (before or after this) never queues the initializer twice; a class with no
// initialized statics contributes nothing. Initializers run in declaration order
// within a class; across classes the order is collect_class_types's walk
// (declaration order, specializations by rendered name), which the LRM leaves
// implementation-defined. An initializer that READS another class's static
// forces that property's declaration mid-emission (value_lvalue ->
// declare_static_prop appends the whole nested store first), so it observes the
// initialized value, not the power-up one.
void Lowerer::init_static_class_props(const Scope& scope) {
    std::vector<const ClassType*> classes;
    collect_class_types(scope, classes);
    if (is_global_scope(&scope)) {
        // A generic specialized with a module's own class is that module's.
        std::erase_if(classes, [](const ClassType* cls) { return !is_global_scope(cls); });
    }
    init_static_props_of(classes);
}

void Lowerer::init_local_class_specializations(const InstanceSymbol& top) {
    Compilation&                  comp = top.body.getCompilation();
    std::vector<const ClassType*> classes;
    for (const PackageSymbol* pkg : comp.getPackages()) {
        collect_class_types(*pkg, classes);
    }
    for (const CompilationUnitSymbol* unit : comp.getRoot().compilationUnits) {
        collect_class_types(*unit, classes);
    }
    std::erase_if(classes, [&](const ClassType* cls) {
        const Scope* owner = enclosing_instance_scope(cls);
        return owner == nullptr || owning_top(owner) != &top;
    });
    init_static_props_of(classes);
}

void Lowerer::init_static_props_of(const std::vector<const ClassType*>& classes) {
    // A static initializer's own call graph reserves class-method FunctionIds
    // the same way compute_vtable does (function_id / rand_body_id /
    // copy_thunk_id / pure_stub_id all grow module_'s function table with no
    // rollback): the id is recorded in fnmap_ / copy_thunk_ / randomize_fn_ /
    // pure_stub_ BEFORE the callee's body is built. recover() below undoes
    // preinit_ and named_blocks_ on an "unsupported construct" LowerError, but
    // has no notion of a reserved-and-cached-but-never-built FunctionId: that
    // id survives the catch, permanently unset, and the backend's unconditional
    // declare_function pass over every id segfaults on its Ref::Invalid return
    // type the first time codegen reaches it. drain_pending_bodies already
    // solves exactly this for its own vtable-forced bodies by making
    // build_function/build_task degrade an unsupported construct to a run-time
    // fatal stub instead of throwing (stub_eligible); this eager path reaches
    // the very same class-method bodies (LRM 6.21 forces every
    // factory-registered class's statics, whether or not the design ever
    // calls into it) through a DIFFERENT, synchronous route that never turns
    // the flag on, so it needs the identical treatment.
    const Restore<bool> stub(stub_unsupported_bodies_, true);
    for (const ClassType* cls : classes) {
        scope_ = cls;
        for (const Symbol& m : cls->members()) {
            const auto* prop = m.as_if<ClassPropertySymbol>();
            if (prop == nullptr || prop->lifetime != slang::ast::VariableLifetime::Static ||
                prop->getInitializer() == nullptr) {
                continue;
            }
            cur_range_ = slang::SourceRange{ prop->location, prop->location };
            recover([&] { declare_static_prop(*prop); });
        }
    }
}

// Seed this Lowerer's symbol->signal map with globally-shared entries
// (package / $unit variables) collected by lower_globals, so references to
// them resolve through the ordinary signal_of path with no special casing.
void Lowerer::seed_signals(const ankerl::unordered_dense::map<const ValueSymbol*, ir::Signal>& seed) {
    for (const auto& [sym, sig] : seed) {
        sigmap_[sym] = sig;
    }
}

// Lower the non-signal members of a scope (an instance body or a generate
// block) into processes: continuous assigns, procedural blocks, child
// instances, and nested generate constructs. `self_prefix` is the
// hierarchical name prefix of `scope`; `self_mul` is the enclosing instance's
// time multiplier, restored after a child instance recurses. Signals are
// already registered by collect_signals, which walked the same generate tree.
void Lowerer::lower_members(const Scope&       scope,
                            const std::string& self_prefix,
                            uint64_t           self_mul,
                            Pass               pass,
                            ir::InstanceScope& node,
                            size_t&            child_cursor) {
    // prefix_ / scope_ hold this level's values on entry to each iteration:
    // set by the caller and restored after any case that descends and changes
    // them, so signal-creating process lowering always sees this level. In
    // Collect only the structural cases (instances / generate constructs) do
    // work — signals were registered by collect_signals; behavioral members
    // lower in Emit. `child_cursor` walks this scope's node.children in the
    // order Collect pushed them, so Emit hands each recursion its own node.
    const bool emit = pass == Pass::Emit;
    // Continuous drivers (assign / net init / gate / port copy) run in both
    // Classify (record driver regions, detect multi-driver nets) and Emit
    // (store, routing resolved nets to slots); behavioral-only members
    // (procedural blocks, variable initializers) run in Emit alone.
    const bool driver_pass = pass != Pass::Collect;
    cur_pass_              = pass;
    for (const Symbol& m : scope.members()) {
        cur_range_ = slang::SourceRange{ m.location, m.location };
        switch (m.kind) {
            case SymbolKind::ContinuousAssign:
                if (driver_pass)
                    recover([&] { lower_continuous_assign(m.as<ContinuousAssignSymbol>()); });
                break;
            case SymbolKind::ProceduralBlock:
                // Collect: register a hidden force_site signal per whole-var
                // `force` target so writers gate in Emit (LRM 10.6.2),
                // enumerate the concurrent assertions / control tasks the
                // block holds (LRM 20.12, see scan_assert_control), and label
                // the named blocks a `disable` targets (LRM 9.6.2, see
                // scan_disable_targets).
                if (pass == Pass::Collect) {
                    scan_force_targets(m.as<ProceduralBlockSymbol>().getBody());
                    scan_assert_control(m.as<ProceduralBlockSymbol>());
                    scan_disable_targets(m.as<ProceduralBlockSymbol>());
                } else if (emit)
                    recover([&] { lower_procedural_block(m.as<ProceduralBlockSymbol>()); });
                break;
            case SymbolKind::Variable:
                // Storage was registered by collect_signals; here only a
                // declaration initializer (LRM 6.8) needs lowering, into the
                // pre-init process.
                if (emit)
                    recover([&] { lower_var_initializer(m.as<VariableSymbol>()); });
                break;
            case SymbolKind::Net:
                // Storage was registered by collect_signals; a net declaration
                // assignment (`wire w = expr`) is a continuous assignment.
                if (driver_pass)
                    recover([&] { lower_net_initializer(m.as<NetSymbol>()); });
                break;
            case SymbolKind::Instance: {
                const InstanceSymbol& child = m.as<InstanceSymbol>();
                // Emit wires ports after the child recurses: a connection's
                // parent-side expression resolves against signals in this
                // (parent) body. Recursion leaves prefix_ at the child level,
                // so restore it before wiring.
                ir::InstanceScope* child_node = driver_pass ? &node.children[child_cursor++] : nullptr;
                // prec_over_tick_ is only changed by lower_instance and is the
                // enclosing value here (unchanged since entry / last restore),
                // so save it locally rather than threading a second param.
                const uint64_t self_prec = prec_over_tick_;
                lower_instance(child, self_prefix + std::string(child.name) + ".", pass, child_node, child.name);
                prefix_         = self_prefix;
                scope_          = &scope;
                time_mul_       = self_mul;
                prec_over_tick_ = self_prec;
                if (driver_pass)
                    wire_ports(child);
                break;
            }
            case SymbolKind::GenerateBlock: {
                // A conditional/bare generate block elaborated to a single
                // scope. An unmatched if/case branch is uninstantiated and
                // contributes nothing (its body is not part of the design).
                const auto& gb = m.as<GenerateBlockSymbol>();
                if (!gb.isUninstantiated) {
                    // A generate block folds no scope node of its own, so its
                    // name segment accumulates into node_rel_ to keep the
                    // enclosed instances' node names unique (see
                    // lower_instance / lower_generate_block).
                    const std::string saved_node_rel = node_rel_;
                    node_rel_ += std::string(gb.getExternalName()) + ".";
                    lower_generate_block(
                      gb, self_prefix + gb.getExternalName() + ".", self_mul, pass, node, child_cursor);
                    node_rel_ = saved_node_rel;
                    prefix_   = self_prefix;
                    scope_    = &scope;
                }
                break;
            }
            case SymbolKind::GenerateBlockArray: {
                // A loop generate: one entry block per genvar iteration, named
                // <array>[<index>] so flattened signal paths stay unique.
                const auto& ga = m.as<GenerateBlockArraySymbol>();
                for (const GenerateBlockSymbol* e : ga.entries) {
                    if (e->isUninstantiated) {
                        continue;
                    }
                    const std::string saved_node_rel = node_rel_;
                    node_rel_ += gen_entry_name(ga, *e) + ".";
                    lower_generate_block(
                      *e, self_prefix + gen_entry_name(ga, *e) + ".", self_mul, pass, node, child_cursor);
                    node_rel_ = saved_node_rel;
                }
                prefix_ = self_prefix;
                scope_  = &scope;
                break;
            }
            case SymbolKind::PrimitiveInstance:
                if (driver_pass)
                    recover([&] { lower_gate(m.as<PrimitiveInstanceSymbol>()); });
                break;
            case SymbolKind::InstanceArray: {
                // An array of instances — gate primitives (LRM 28.3.6) or
                // modules/interfaces (LRM 23.3.6). Slang has already
                // elaborated one leaf symbol per element with per-instance
                // bit-sliced connections, so this only needs to walk to
                // the leaves and lower each like its scalar counterpart.
                lower_instance_array(
                  m.as<InstanceArraySymbol>(), self_prefix, scope, self_mul, pass, node, child_cursor);
                prefix_   = self_prefix;
                scope_    = &scope;
                time_mul_ = self_mul;
                break;
            }
            // A clocking block (LRM 14). Not inert: it owns a clocking event,
            // per-clockvar sampled storage and one Observed-region process, and
            // an interface body reaches this same walk, so an interface-scoped
            // block (LRM 14.9 / 25.5.5) lowers here too.
            case SymbolKind::ClockingBlock:
                if (pass == Pass::Collect) {
                    recover([&] { collect_clocking_block(m.as<ClockingBlockSymbol>()); });
                } else if (emit) {
                    recover([&] {
                        lower_clocking_block(m.as<ClockingBlockSymbol>());
                        lower_clocking_drives(m.as<ClockingBlockSymbol>());
                    });
                }
                break;
            // slang resolves a defparam, but the type it gives its target is
            // checked here; see check_defparam_type. In Collect, so the whole
            // design knows which parameters are refused before Emit reads any.
            case SymbolKind::DefParam:
                if (pass == Pass::Collect)
                    recover([&] { check_defparam_type(m.as<DefParamSymbol>()); });
                break;
            // Known-inert members: elaboration artifacts and declarations
            // that bear no simulation behavior of their own. Ports are wired
            // by wire_ports; parameters/types/genvars are resolved by slang;
            // subroutines lower on demand at call sites; statement blocks
            // lower with their enclosing procedure; elab system tasks were
            // already issued as elaboration diagnostics.
            case SymbolKind::Port:
            // Interface members with no simulation behavior of their own.
            // An interface body is walked when lower_instance recurses into
            // the interface instance; a Modport is a named view (LRM 25.5),
            // an InterfacePort/ModportPort is a reference proxy resolved at
            // use via canonical_value_symbol. Their signals are ordinary
            // Variable/Net members handled above and by collect_signals.
            case SymbolKind::Modport:
            case SymbolKind::InterfacePort:
            case SymbolKind::ModportPort:
            case SymbolKind::Parameter:
            case SymbolKind::TypeParameter:
            case SymbolKind::TypeAlias:
            // `typedef class C;` (LRM 6.18 / 8.27): pure elaboration-time name
            // binding, entirely resolved by slang's own forward-decl machinery
            // (ForwardingTypedefSymbol::addForwardDecl / checkType) before this
            // walk ever sees it. No type, value, or statement content of its
            // own -- the class it names lowers on demand like any other
            // ClassType, via lower_class_type's intern-first symbol-pointer
            // cache, which already treats a forward decl and its later full
            // definition as the same ClassType symbol.
            case SymbolKind::ForwardingTypedef:
            // A class declaration is a type, not a per-instance member: its
            // methods and object body lower on demand when a variable of the
            // class is declared / used (lower_class_type).
            case SymbolKind::ClassType:
            // A covergroup declaration (LRM 19.3) is a type, like a class: its
            // coverage model, its sample body and its coverage-event process are
            // built on demand at the first `new` of it (cover_type_of), where
            // the lowering context is the one its coverpoint expressions resolve
            // against. A covergroup nothing ever instantiates samples nothing,
            // which is exactly what LRM 19.4 says of an embedded one left
            // unconstructed.
            case SymbolKind::CovergroupType:
            case SymbolKind::GenericClassDef:
            case SymbolKind::TransparentMember:
            case SymbolKind::Genvar:
            case SymbolKind::Subroutine:
            case SymbolKind::StatementBlock:
            case SymbolKind::WildcardImport:
            case SymbolKind::ExplicitImport:
            case SymbolKind::EmptyMember:
            case SymbolKind::ElabSystemTask:
            case SymbolKind::Specparam:
            // A named `sequence` / `property` declaration is a template, not
            // a member with behavior of its own: nothing elaborates until it
            // is instantiated, and slang expands the body with the actual
            // arguments substituted at each use
            // (AssertionInstanceExpression::body), which src/Sva.cpp recurses
            // into. The declaration's own scope holds only formal argument
            // and local variable symbols, and collect_scope_signals descends
            // only into generate blocks, so there is no storage to register
            // for it either.
            case SymbolKind::Sequence:
            case SymbolKind::Property:
            // A `let` is the same kind of template, and an even simpler one:
            // slang expands a let call straight to the substituted
            // Expression rather than wrapping it in an
            // AssertionInstanceExpression (MiscExpressions.cpp:1023-1025), so
            // every use is already an ordinary expression by the time clotho
            // sees it, inside an assertion or out of one.
            case SymbolKind::LetDecl:
                break;
            // An instance of a module the compilation was told to tolerate
            // not having (CompilationFlags::IgnoreUnknownModules, which the
            // language server's buffer entry point sets so that one file of
            // a design can be analysed on its own). slang deliberately
            // reports nothing for it, so refusing here would restate "the
            // rest of your design is not open" as "clotho does not
            // implement this" -- a false claim, and the loudest kind.
            //
            // Without that flag an unknown module is a genuine error slang
            // has already reported, and anything reaching here is a
            // construct clotho really does not handle, so it still fails.
            case SymbolKind::UninstantiatedDef:
                if (!scope.getCompilation().hasFlag(slang::ast::CompilationFlags::IgnoreUnknownModules)) {
                    fail("unsupported module member kind=" + std::string(toString(m.kind)));
                }
                break;
            default:
                // Anything else (gate primitives, specify blocks, checkers,
                // nettype aliases, ...) may carry behavior; refusing loudly
                // beats silently dropping it (see PRINCIPLES.md).
                fail("unsupported module member kind=" + std::string(toString(m.kind)));
        }
    }
}

// Lower one generate block's members under `block_prefix`. Genvar values are
// implicit localparams in the block scope, so const folding and parameter
// reads resolve them once scope_ points at the block.
void Lowerer::lower_generate_block(const GenerateBlockSymbol& block,
                                   const std::string&         block_prefix,
                                   uint64_t                   self_mul,
                                   Pass                       pass,
                                   ir::InstanceScope&         node,
                                   size_t&                    child_cursor) {
    // A generate block gets no scope node of its own: its signals fold into
    // the enclosing instance's scope (with prefixed names), and any instances
    // it contains become children of that instance's node. So `node` /
    // `child_cursor` thread straight through.
    prefix_ = block_prefix;
    scope_  = &block;
    lower_members(block, block_prefix, self_mul, pass, node, child_cursor);
}

// Recurse through one (possibly multidimensional) array of instances —
// gate primitives (LRM 28.3.6) or modules/interfaces (LRM 23.3.6). Slang
// builds a nested InstanceArraySymbol per extra unpacked dimension (`foo
// u[3][4] (...)`), each ultimately bottoming out at a leaf symbol that
// already carries its own bit-sliced port/terminal connections — this
// only walks to those leaves and lowers each like its scalar counterpart.
// A module/interface leaf flattens under <array>[<index>], mirroring
// generate-loop arrays (gen_entry_name); a gate leaf needs no hierarchy
// bookkeeping of its own.
void Lowerer::lower_instance_array(const InstanceArraySymbol& arr,
                                   const std::string&         self_prefix,
                                   const Scope&               parent_scope,
                                   uint64_t                   self_mul,
                                   Pass                       pass,
                                   ir::InstanceScope&         node,
                                   size_t&                    child_cursor) {
    const bool driver_pass = pass != Pass::Collect;
    for (const Symbol* elem : arr.elements) {
        switch (elem->kind) {
            case SymbolKind::InstanceArray:
                lower_instance_array(
                  elem->as<InstanceArraySymbol>(), self_prefix, parent_scope, self_mul, pass, node, child_cursor);
                break;
            case SymbolKind::PrimitiveInstance:
                if (driver_pass)
                    lower_gate(elem->as<PrimitiveInstanceSymbol>());
                break;
            case SymbolKind::Instance: {
                const InstanceSymbol& child      = elem->as<InstanceSymbol>();
                ir::InstanceScope*    child_node = driver_pass ? &node.children[child_cursor++] : nullptr;
                const uint64_t        self_prec  = prec_over_tick_;
                lower_instance(child,
                               self_prefix + instance_array_element_name(child) + ".",
                               pass,
                               child_node,
                               instance_array_element_name(child));
                prefix_         = self_prefix;
                scope_          = &parent_scope;
                time_mul_       = self_mul;
                prec_over_tick_ = self_prec;
                if (driver_pass)
                    wire_ports(child);
                break;
            }
            default:
                // A checker instance array: checkers aren't lowered at
                // all today, so fail the same way a lone one would.
                fail("unsupported instance array element kind=" + std::string(toString(elem->kind)));
        }
    }
}

// Hierarchical-path segment for one instance-array element: <array>[<i0>]
// [<i1>].... Mirrors slang's own hierarchical-path formatting (slang's
// Symbol::getHierarchicalPath): arrayPath holds one canonical (always
// zero-based) index per dimension, and adding each dimension's lower
// bound recovers the source-code index regardless of whether that
// dimension's range counts up or down.
std::string Lowerer::instance_array_element_name(const InstanceSymbol& elem) {
    std::string                                 name(elem.getArrayName());
    slang::SmallVector<slang::ConstantRange, 4> dims;
    elem.getArrayDimensions(dims);
    SLANG_ASSERT(dims.size() == elem.arrayPath.size());
    for (size_t i = 0; i < dims.size(); i++) {
        name += '[';
        name += std::to_string(int32_t(elem.arrayPath[i]) + dims[i].lower());
        name += ']';
    }
    return name;
}

// Hierarchical-path segment for one loop-generate entry: <array>[<index>].
// The entry's own name is empty; the genvar value (its arrayIndex) supplies
// the subscript.
std::string Lowerer::gen_entry_name(const GenerateBlockArraySymbol& array, const GenerateBlockSymbol& entry) {
    std::string name = array.getExternalName();
    if (entry.arrayIndex != nullptr) {
        name += '[';
        name += entry.arrayIndex->toString(slang::LiteralBase::Decimal, /*includeBase=*/false);
        name += ']';
    }
    return name;
}

// Wire one child instance's ports to the parent. Each connection becomes a
// copy process across the boundary, carrying inferred sensitivity so the value
// propagates on change (it must not inherit the empty-wait-list gap of plain
// continuous assigns). Both sides' signals are already registered: the child's
// by the just-completed recursion, the parent's by this body's collect_signals.
//
// slang models a connection asymmetrically: an output yields a full
// AssignmentExpression (sink = source, with width conversions baked in), while
// an input yields the bare source rvalue to drive the child net with.
void Lowerer::wire_ports(const InstanceSymbol& child) {
    // Every copy emitted below for a PROGRAM child is reactive-home. LRM
    // 24.3.2: "variables on the other side of a program port connection are
    // updated in the reactive region set. Similarly, the driving and resolution
    // of nets on the other side of a program port connection also occurs in the
    // reactive region set." That sentence is about the OUTPUT direction; the
    // input direction is 24.3.1's own example -- "The continuous assignment
    // assign tclk = clk; would also be scheduled in the Reactive region" -- an
    // input copy being exactly that assignment, a construct inside the program
    // sensitive to a design signal. So both directions, one flag.
    //
    // Asked of the child definition rather than of `in_program_`:
    // add_continuous_process ORs `in_program_` in, but that is the PARENT's
    // answer here (this runs after lower_instance(child) restored it) and is
    // always false, because LRM 24.3.2 also says program ports "are always
    // connected to design objects (nets and variables), since programs can only
    // be instantiated in design scopes".
    const bool child_is_program = child.getDefinition().definitionKind == slang::ast::DefinitionKind::Program;
    for (const PortConnection* pc : child.getPortConnections()) {
        if (pc->port.kind == SymbolKind::InterfacePort) {
            // An interface port carries no value to copy: the interface
            // instance owns the storage and every reference to it (plain,
            // modport, or hierarchical) resolves to that shared ir::Signal
            // via canonical_value_symbol (LRM 25, docs/design/interfaces.md).
            // Nothing to wire here.
            continue;
        }
        if (pc->port.kind != SymbolKind::Port) {
            // TODO: any other non-value port kind.
            fail("unsupported port connection kind");
        }
        const PortSymbol& p = pc->port.as<PortSymbol>();
        if (p.direction == slang::ast::ArgumentDirection::InOut || p.direction == slang::ast::ArgumentDirection::Ref ||
            coerced_ports_.contains(&p)) {
            // inout / ref ports share storage via build_inout_aliases (the child
            // internal net/var aliases the parent signal), so there is no
            // one-way copy to wire here — both sides already reference one
            // signal and any drivers fold through net resolution. A port coerced to
            // inout (needs_port_coercion) is aliased the same way.
            continue;
        }
        if (p.internalSymbol == nullptr) {
            // TODO: a port with no internal net (e.g. an unnamed expression
            // port) — has no signal storage to copy to/from.
            fail("port without an internal net is not supported");
        }
        const Expression* expr = pc->getExpression();
        if (expr == nullptr) {
            // Unconnected port (LRM 23.3.2.7). An unconnected output discards
            // the child's driven value: nothing in the parent reads it, so emit
            // no copy process. An unconnected input leaves the child's internal
            // net with no driver, so it holds the undriven default from
            // init_state (z for a 4-state net per LRM 6.6, x for a variable,
            // 0 for 2-state), unless its module was declared under
            // `unconnected_drive (LRM 22.9), which pulls it up or down. A port
            // carrying an explicit default value arrives here with a non-null
            // expression, so a null expression means there is genuinely nothing
            // to connect.
            if (p.direction == slang::ast::ArgumentDirection::In) {
                const slang::ast::UnconnectedDrive drive = child.getDefinition().unconnectedDrive;
                if (drive != slang::ast::UnconnectedDrive::None) {
                    lower_unconnected_drive(p, drive == slang::ast::UnconnectedDrive::Pull1, child_is_program);
                }
            }
            continue;
        }

        // A non-ANSI port reference with a constant select (`module m(a[3:2])`,
        // LRM 23.2.2.1) is those bits of its internal symbol and nothing else; any
        // other port is the whole symbol. p.getType() is the port's own type
        // either way: the connection is converted to it, and an output port's
        // value has it.
        const Expression* sel = p.getInternalExpr();

        if (cur_pass_ == Pass::Classify) {
            // The connection is a continuous driver: an output drives the
            // parent-side lvalue, an input drives the port's own bits of the
            // child net.
            if (expr->kind == ExpressionKind::Assignment) {
                record_net_driver(expr->as<AssignmentExpression>().left());
            } else if (sel != nullptr) {
                record_net_driver(*sel);
            } else {
                record_net_driver_whole(p.internalSymbol->as<ValueSymbol>());
            }
            continue;
        }

        const ir::Signal           child_sig = signal_of(p.internalSymbol->as<ValueSymbol>());
        const ir::SourceLoc        l         = to_source_loc(p.location);
        ir::Builder                b(module_);
        std::vector<ir::WaitEvent> sens;
        const slang::ast::Type&    ptype = p.getType();
        // The port's own storage: the child net, or the bits of it a select port
        // names.
        const auto port_rvalue = [&] {
            return sel != nullptr ? lower_expr(*sel, b) : b.unary(ir::Inst::Tag::Load, b.signal(child_sig, l), l);
        };
        const auto port_lvalue = [&] { return sel != nullptr ? lower_lvalue(*sel, b) : b.signal(child_sig, l); };
        // A connected container port. The port copy is a continuous assignment,
        // so it would have to deep-copy the handle whenever the source changes
        // (value semantics, LRM 7.5 / 7.9 / 7.10) and detect that change through
        // a heap buffer rather than a plane compare. Neither exists.
        //
        // Guarded here rather than at the declaration: an UNCONNECTED container
        // port lowers fine and the synthesis path deliberately reports it as a
        // type with no hardware form, so refusing the declaration would take that
        // away. Guarded at all because the plane-copy path below would otherwise
        // reach the backend as a plane load of a handle, which throws there —
        // a backend error with no source location instead of a diagnostic.
        // TODO: container ports (a handle-aware continuous copy). Reached by a
        // `program` port too, hence no "module" in the message.
        if (ir::types::is_container(module_.pool, lower_type(ptype))) {
            fail("a dynamic-array, queue or associative-array port connection "
                 "is not supported");
        }
        if (expr->kind == ExpressionKind::Assignment) {
            // Output: parent_target = child_net. slang gives an assignment whose
            // LHS is the parent target and whose RHS is the port value, a
            // placeholder (EmptyArgument) in the implicit conversion to the target
            // type, so read the child net directly and convert it by that
            // conversion (lower_port_output). Wakes on the child net.
            const AssignmentExpression& a = expr->as<AssignmentExpression>();
            if (const std::optional<ir::Signal> net = resolved_net_of(a.left())) {
                // The parent net has multiple drivers: stage this child's
                // output (whole net, or a constant sub-range with a Z
                // background) into its own slot; the resolver folds them.
                // A program child's output copy is reactive-home, and so is
                // the resolver of the design net it drives: LRM 24.3.2, "the
                // driving and resolution of nets on the other side of a
                // program port connection also occurs in the reactive region
                // set". child_is_program records the net for
                // emit_wire_resolvers, which states what a reactive resolver
                // costs the net's design-side drivers. Identical question,
                // identical answer as a program's hierarchical `assign` to a
                // resolved net (lower_continuous_assign).
                if (is_aggregate(*a.left().type)) {
                    // TODO: aggregate driver of a multiply-driven net.
                    fail("aggregate driver of a multiply-driven net is not supported");
                }
                const ir::Index  child_val = lower_port_output(a, port_rvalue(), b);
                const ir::Index  val  = build_driver_slot_value(*net, a.left(), child_val, b, l);
                const ir::Signal slot = alloc_driver_slot(*net, l, /*strength=*/0x0303, child_is_program);
                b.binary(ir::Inst::Tag::Store, b.signal(slot, l), val, l);
                sens.push_back(ir::WaitEvent{ .signal = child_sig, .edge = ir::Edge::Any });
                add_continuous_process(std::move(sens), l, b, child_is_program);
                continue;
            }
            if (is_aggregate(*a.left().type) || is_unpacked_array_type(*a.left().type)) {
                // A whole unpacked-struct / unpacked-array / aggregate port: the
                // copy is a per-leaf (or memcpy) whole-object copy, not a scalar
                // store. Port types are equivalent (LRM 23.3.3), so both sides
                // share the recursive storage layout (a plain unpacked array is
                // trivially copyable, so emit_port_agg_copy memcpy's it).
                const ir::Index dst = lower_lvalue(a.left(), b);
                const ir::Index src = port_lvalue();
                emit_port_agg_copy(dst, src, lower_type(*a.left().type), b, l);
            } else if (a.left().kind == ExpressionKind::Concatenation) {
                // Output connected to a concatenation target `.o({a,b})`:
                // sample the child net once and scatter it MSB-first into the
                // operands, the same destructure as `{a,b} = x` (store_concat).
                reject_concat_resolved_driver(a.left());
                const ir::Index val = lower_port_output(a, port_rvalue(), b);
                store_concat(a.left().as<ConcatenationExpression>(), val, ir::Inst::Tag::Store, b, l);
            } else {
                const ir::Index val = port_rvalue();
                const ir::Index dst = lower_lvalue(a.left(), b);
                const ir::Index src = lower_port_output(a, val, b);
                // The copy is an implicit continuous assignment (LRM 4.9.6), so a `force`
                // on the variable it drives overrides it and a `release` re-evaluates it
                // (LRM 10.6.2), like `assign v = child_net;`. The gate is only where the
                // root is a force target; every other copy is the plain store.
                if (ForceInfo* fi = pca_info_of_lvalue(a.left())) {
                    store_or_gate(fi, ir::Inst::Tag::Store, dst, src, b, l);
                } else {
                    b.binary(ir::Inst::Tag::Store, dst, src, l);
                }
            }
            sens.push_back(ir::WaitEvent{ .signal = child_sig, .edge = ir::Edge::Any });
            sens = with_force_wake(std::move(sens), a.left());
        } else {
            // Input: drive the child net from the source rvalue, resized to the
            // port width; wakes on the source's reads. The copy is a continuous
            // driver of the port's own bits of the child-side net: all of it, or
            // the bits a select port names.
            if (resolved_nets_.contains(static_cast<uint32_t>(child_sig))) {
                // The child net has more than one driver total: this input
                // copy plus at least one other, possibly reached from a
                // DIFFERENT instance scope (e.g. a `bind`-inserted module
                // driving a shared interface net through an upward
                // hierarchical reference, LRM 23.8's own example shape).
                // Stage this copy as an ordinary driver slot instead of a
                // direct store -- the same way a resolved OUTPUT port
                // connection already does above -- so emit_wire_resolvers
                // folds it together with every other driver regardless of
                // which scope discovered each one (alloc_driver_slot no
                // longer requires every driver of one net to share a scope).
                // A program child's input copy is reactive-home, so the net
                // it drives resolves in the reactive region set too: LRM
                // 24.3.1 schedules constructs within a program block that are
                // sensitive to changes on design signals in the Reactive
                // region, and the reader that would otherwise see the fold a
                // region late is the program's OWN code. child_is_program
                // records the net for emit_wire_resolvers, which states what
                // a reactive resolver costs the net's other drivers.
                //
                // The second driver can only come from another program: LRM
                // 24.3 makes a design reference to a program signal an error
                // but explicitly allows "hierarchical references to extend
                // from one program scope to another program scope".
                if (is_aggregate(ptype) || is_unpacked_array_type(ptype)) {
                    // TODO: aggregate driver of a multiply-driven net (matches
                    // the OUTPUT port and continuous-assign refusals above).
                    fail("aggregate driver of a multiply-driven net is not supported");
                }
                const ir::Ref   ty     = lower_type(ptype);
                const ir::Index driven = coerce(lower_expr(*expr, b), *expr->type, ty, b, l);
                // A select port drives its bits only: its slot holds z, the
                // resolution identity, in the rest.
                const ir::Index  val = sel != nullptr ? build_driver_slot_value(child_sig, *sel, driven, b, l) : driven;
                const ir::Signal slot = alloc_driver_slot(child_sig, l, /*strength=*/0x0303, child_is_program);
                b.binary(ir::Inst::Tag::Store, b.signal(slot, l), val, l);
                collect_expr_reads(*expr, sens);
                add_continuous_process(std::move(sens), l, b, child_is_program);
                continue;
            }
            const ir::Ref ty = lower_type(ptype);
            if (is_aggregate(ptype) || is_unpacked_array_type(ptype)) {
                // A whole unpacked-struct / unpacked-array / aggregate input
                // port: whole-object copy of the source into the child net (a
                // plain unpacked array memcpy's via emit_port_agg_copy). An
                // lvalue-shaped actual (the common case -- a plain variable /
                // member / element) passes straight through with no extra
                // copy beyond the port's own; anything else (a `'{...}`
                // pattern, an aggregate `?:`, a call, a concatenation) is
                // staged into a fresh canonical temp first, by the same
                // machinery an aggregate CALL actual already uses
                // (agg_input_actual) -- LRM 23.3.3 makes a port connection a
                // continuous copy exactly like a value-typed call argument,
                // so the same staging is correct here, not just reused for
                // convenience.
                const Expression& src = unwrap_conversion(*expr);
                const ir::Index   av  = is_struct_lvalue_shape(src) ? lower_lvalue(src, b)
                                                                   : agg_input_actual(src, ty, b, l);
                emit_port_agg_copy(port_lvalue(), av, ty, b, l);
            } else {
                const ir::Index dst = port_lvalue();
                const ir::Index val = coerce(lower_expr(*expr, b), *expr->type, ty, b, l);
                b.binary(ir::Inst::Tag::Store, dst, val, l);
            }
            collect_expr_reads(*expr, sens);
        }
        // A port connection is a continuous copy: evaluate at time zero so the
        // child sees the parent's declaration-initialized value even when the
        // parent signal never changes afterward (LRM 23.3.3).
        add_continuous_process(std::move(sens), l, b, child_is_program);
    }
}

// An output port's value as the connected expression receives it. slang builds
// the connection as `connected = <port>`: the port is an EmptyArgument typed as
// the port, inside the implicit conversions that every assignment gets (LRM
// 23.3.3.8: "assigned to the object on the other side of the port using the same
// rules as an assignment"). Lowering that right-hand side with the port's value
// bound to the placeholder applies each conversion as any assignment does: a
// narrower value extends by the port's own sign (LRM 10.7; a signed port is
// sign-extended whatever the connected net's sign), and a real becomes integral
// or the reverse. The type of `a.right()` is already the converted one, so
// coerce() by it would extend by the connected expression's sign.
ir::Index Lowerer::lower_port_output(const AssignmentExpression& a, ir::Index port_value, ir::Builder& b) {
    const Expression* leaf = &a.right();
    while (leaf->kind == ExpressionKind::Conversion) {
        leaf = &leaf->as<ConversionExpression>().operand();
    }
    if (leaf->kind != ExpressionKind::EmptyArgument) {
        fail("malformed output port connection");
    }
    const Restore<std::optional<ir::Index>> bind(port_value_, port_value);
    return lower_expr(a.right(), b);
}

// Append the sensitivity of an rvalue expression (each signal it reads, as an
// any-change wait) to `out`. Mirrors collect_inferred_sensitivity for an
// expression with no writes.
void Lowerer::collect_expr_reads(const Expression& expr, std::vector<ir::WaitEvent>& out) {
    // A port connection is a continuous copy (LRM 23.3.3), so it follows the
    // continuous-assign sensitivity rule: function-internal reads do not
    // contribute (ImplicitEvent), only the expression's own operands and any
    // call arguments.
    const auto declare_static = [this](const slang::ast::ClassPropertySymbol& p) {
        return declare_static_prop(p);
    };
    SensitivityCollector c(sigmap_, SensitivityMode::ImplicitEvent, declare_static);
    expr.visit(c);
    for (const ir::Signal s : c.reads) {
        if (c.written.contains(static_cast<uint32_t>(s))) {
            continue;
        }
        if (is_string_ref(module_.signal(s).type)) {
            // TODO: string signals in inferred sensitivity — string stores
            // skip wait-queue servicing, so the process would never
            // re-trigger.
            fail("string in inferred sensitivity is not supported");
        }
        out.push_back(ir::WaitEvent{ .signal = s, .edge = ir::Edge::Any });
    }
}

} // namespace clotho::lower
