// AST pre-scans and helpers that run before or beside the Lowerer.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Resolve an interface modport-port proxy to the interface instance's own
// storage symbol. A modport (LRM 25.5) exposes each interface signal through a
// distinct ModportPortSymbol whose `internalSymbol` is the underlying interface
// VariableSymbol/NetSymbol; a plain interface-port (`m(bus w); w.sig`) or direct
// hierarchical reference to the same signal already binds to that underlying
// symbol. Keying every reference on the canonical symbol makes interface storage
// shared for free — a modport read, a plain-port read, and a direct hierarchical
// read all map to one ir::Signal, with no cross-boundary copy process. The loop
// handles a modport re-exported through a nested interface's modport.
const ValueSymbol& canonical_value_symbol(const ValueSymbol& sym) {
    const ValueSymbol* cur = &sym;
    while (cur->kind == SymbolKind::ModportPort) {
        const ModportPortSymbol& mp   = cur->as<ModportPortSymbol>();
        const ValueSymbol*       next = nullptr;
        if (mp.internalSymbol != nullptr) {
            next = mp.internalSymbol->as_if<ValueSymbol>();
        } else if (const Expression* conn = mp.getConnectionExpr()) {
            // A modport expression port (`.name(expr)`, LRM 25.5.3): the port
            // maps to an explicit expression, not a direct internal symbol. A
            // plain rename (`.clean(raw)`) wraps a single whole signal that
            // shares storage, so follow it; a structural expression
            // (part-select / concat) maps to a bit sub-range with no single
            // canonical symbol and is rejected loudly in signal_of.
            if (conn->kind == ExpressionKind::NamedValue || conn->kind == ExpressionKind::HierarchicalValue) {
                next = &conn->as<ValueExpressionBase>().symbol;
            }
        }
        if (next == nullptr) {
            break;
        }
        cur = next;
    }
    return *cur;
}

// Femtoseconds in one unit of a time-scale value. Magnitude is 1/10/100 and the
// unit is a power-of-ten fraction of a second, so the result is a power of ten.
uint64_t tsv_to_fs(slang::TimeScaleValue v) {
    uint64_t fs_per_unit = 1;
    switch (v.unit) {
        case slang::TimeUnit::Femtoseconds:
            fs_per_unit = 1ULL;
            break;
        case slang::TimeUnit::Picoseconds:
            fs_per_unit = 1'000ULL;
            break;
        case slang::TimeUnit::Nanoseconds:
            fs_per_unit = 1'000'000ULL;
            break;
        case slang::TimeUnit::Microseconds:
            fs_per_unit = 1'000'000'000ULL;
            break;
        case slang::TimeUnit::Milliseconds:
            fs_per_unit = 1'000'000'000'000ULL;
            break;
        case slang::TimeUnit::Seconds:
            fs_per_unit = 1'000'000'000'000'000ULL;
            break;
    }

    return static_cast<uint64_t>(v.magnitude) * fs_per_unit;
}

// True iff `tc` is a real-valued `#delay` (LRM 3.14): a plain DelayControl whose
// amount expression is floating. Rise/fall/turn-off triples (Delay3) are rejected
// at lowering regardless, so a real one there needs no tick handling.
bool delay_is_real(const TimingControl* tc) {
    return tc != nullptr && tc->kind == TimingControlKind::Delay && tc->as<DelayControl>().expr.type->isFloating();
}

void RealDelayFinder::handle(const slang::ast::TimedStatement& s) {
    if (delay_is_real(&s.timing)) {
        found = true;
    }
    visitDefault(s);
}

void RealDelayFinder::handle(const slang::ast::AssignmentExpression& a) {
    if (delay_is_real(a.timingControl)) {
        found = true;
    }
    visitDefault(a);
}

// True iff a subroutine's own body holds a real delay. A bodyless subroutine (a
// DPI import, an extern prototype, slang's built-in `std` methods) has no
// statement to bind, so it is skipped rather than forced.
bool subroutine_has_real_delay(const SubroutineSymbol& sub) {
    if (sub.flags.has(MethodFlags::DPIImport) || sub.flags.has(MethodFlags::Pure) || sub.getSyntax() == nullptr) {
        return false;
    }
    RealDelayFinder f;
    sub.getBody().visit(f);
    return f.found;
}

// The class INSTANCE property an expression names, or null when the expression is
// not a plain reference to one. Shared by the two type-specific resolvers below.
//
// `@p` inside a method (an implicit-`this` reference, so a NamedValue) and
// `@(obj.p)` (a MemberAccess) are the two shapes. Excluded on purpose:
//
//   - a STATIC property, which has persistent module-signal storage of its own
//     (declare_static_prop) and already resolves through event_signal;
//   - an EVENT property is admitted by NEITHER type-specific resolver below (an
//     event type is neither integral nor a container) and needs no notifier: the
//     property IS an object with a waiter list, the very one `-> e` fires. The
//     wait side parks on that object instead -- `@(obj.e)` and a bare `@e` in a
//     method through event_wait_needs_handle, a `wait` condition reading one
//     through ClassPropReadScan::event_props.
const slang::ast::ClassPropertySymbol* instance_class_prop(const Expression& e) {
    const slang::ast::Symbol* member = nullptr;
    if (e.kind == ExpressionKind::NamedValue || e.kind == ExpressionKind::HierarchicalValue) {
        member = &e.as<ValueExpressionBase>().symbol;
    } else if (e.kind == ExpressionKind::MemberAccess) {
        const auto& ma = e.as<MemberAccessExpression>();
        // A virtual-interface member is a signal in some instance, not a property;
        // it has its own per-(member, edge) waiter head (LRM 25.9).
        if (ma.value().type->getCanonicalType().isVirtualInterface()) {
            return nullptr;
        }
        member = &ma.member;
    } else {
        return nullptr;
    }
    const auto* prop = member->as_if<slang::ast::ClassPropertySymbol>();
    if (prop == nullptr || prop->lifetime == slang::ast::VariableLifetime::Static) {
        return nullptr;
    }
    return prop;
}

// The three container kinds whose mutation surface a change notifier covers: a
// queue (LRM 7.10), an associative array (7.8) and a dynamic array (7.5). Tested
// in that order because slang answers isDynamicallySizedArray() for an
// associative array too (foreach_dim_ref was bitten by exactly that).
//
// A FIXED unpacked array is deliberately absent. Its storage never changes
// extent, so it has no mutation op at all -- every write to one is an element
// store -- and it is refused rather than given a notifier nothing would fire.
bool is_notified_container(const slang::ast::Type& ty) {
    return ty.isQueue() || ty.isAssociativeArray() || ty.isDynamicallySizedArray();
}

PropNotify prop_notify_kind(const slang::ast::Type& ty) {
    if (ty.isString()) {
        return PropNotify::String;
    }
    if (ty.isEvent()) {
        // Excluded deliberately: `@(obj.ev)` parks on the event OBJECT the property
        // names (event_wait_needs_handle), which is what `-> obj.ev` fires, so a
        // notifier here would be a field nothing ever reads.
        return PropNotify::None;
    }
    if (ty.isIntegral() || ty.isFloating() || ty.isClass() || ty.isCHandle() ||
        ty.isVirtualInterface()) {
        return PropNotify::Word;
    }
    return PropNotify::None;
}

// Whether `@(posedge p)` / `@(negedge p)` mean anything for this property's type.
//
// LRM 9.4.2 with Table 9-2 defines an edge as the LEAST SIGNIFICANT BIT of the
// expression moving through the 0 < x/z < 1 lattice. Only an integral value has
// such a bit: bit 0 of a heap pointer, of an IEEE-754 significand and of a
// string's first character are not logic levels, and the LRM assigns them none.
// So an edge control on any other notified type is refused rather than invented.
bool prop_has_edges(const slang::ast::Type& ty) {
    return ty.isIntegral();
}

// The property an `@` operand names (LRM 9.4.2), whose value one store can
// classify. A container property is excluded here on purpose and refused loudly in
// collect_wait_events_impl -- firing its mutation notifier from `@` would wake a
// process with no value change to observe.
const slang::ast::ClassPropertySymbol* waited_class_prop(const Expression& e) {
    const auto* prop = instance_class_prop(e);
    if (prop == nullptr ||
        prop_notify_kind(prop->getType().getCanonicalType()) == PropNotify::None) {
        return nullptr;
    }
    return prop;
}

// The CONTAINER property an expression names, whose notifier fires on mutation.
// Only a `wait` may park on one: LRM 9.4.3 re-tests the condition and parks
// again, so a fire that leaves it false costs one re-test, while a MISSED
// mutation is a permanent park.
const slang::ast::ClassPropertySymbol* waited_container_prop(const Expression& e) {
    const auto* prop = instance_class_prop(e);
    if (prop == nullptr || !is_notified_container(prop->getType().getCanonicalType())) {
        return nullptr;
    }
    return prop;
}

// Either kind: what a `wait` condition can park on. The design-wide pre-pass and
// the wait lowering MUST agree on this set, so both call this and nothing else.
const slang::ast::ClassPropertySymbol* level_waited_prop(const Expression& e) {
    if (const auto* p = waited_class_prop(e); p != nullptr) {
        return p;
    }
    return waited_container_prop(e);
}

void ClassPropReadScan::take(const Expression& e) {
    if (const auto* p = instance_class_prop(e);
        p != nullptr && p->getType().getCanonicalType().isEvent()) {
        event_props.push_back(&e);
        any_prop = true;
        return;
    }
    if (const auto* p = level_waited_prop(e); p != nullptr) {
        notifiable.push_back(&e);
        any_prop = true;
        if (is_notified_container(p->getType().getCanonicalType())) {
            any_container = true;
        }
        return;
    }
    const slang::ast::Symbol* member = nullptr;
    if (e.kind == ExpressionKind::MemberAccess) {
        member = &e.as<MemberAccessExpression>().member;
    } else {
        member = &e.as<ValueExpressionBase>().symbol;
    }
    if (const auto* p = member->as_if<slang::ast::ClassPropertySymbol>();
        p != nullptr && p->lifetime != slang::ast::VariableLifetime::Static) {
        any_prop = true;
    }
}

void ClassPropReadScan::handle(const slang::ast::NamedValueExpression& e) {
    take(e);
    visitDefault(e);
}

void ClassPropReadScan::handle(const slang::ast::HierarchicalValueExpression& e) {
    take(e);
    visitDefault(e);
}

void ClassPropReadScan::handle(const MemberAccessExpression& ma) {
    take(ma);
    // Descended into on purpose: the object expression of `a.b.p` can itself
    // read properties, and a compound condition nests member accesses.
    visitDefault(ma);
}

// Whether a fork-captured automatic of this type can carry a change notifier, i.e.
// whether a `wait` reading one can ever be woken. Exactly the class-property
// partition, because a capture box IS a one-field class object
// (docs/design/fork-join.md V4) and its field takes the same notifier machinery:
// one classified word, a string's content, or a container's mutation surface. An
// unpacked struct / union / fixed unpacked array carries none, and the wait
// lowering refuses it by name rather than parking on nothing.
bool box_can_notify(const slang::ast::Type& ty) {
    const slang::ast::Type& ct = ty.getCanonicalType();
    return prop_notify_kind(ct) != PropNotify::None || is_notified_container(ct);
}

void WaitLocalScan::handle(const slang::ast::NamedValueExpression& e) {
    out->insert(&e.symbol);
    visitDefault(e);
}

void WaitLocalScan::handle(const slang::ast::HierarchicalValueExpression& e) {
    out->insert(&e.symbol);
    visitDefault(e);
}

bool WaitedProps::waits_local(const slang::ast::ValueSymbol& vs) const {
    return locals.contains(&vs);
}

void WaitedProps::add(const slang::ast::ClassPropertySymbol& prop, ir::Edge edge) {
    props[&prop].edges |= static_cast<uint8_t>(1u << static_cast<uint32_t>(edge));
}

// A container property is always and only any-change: `@` over one is refused,
// so no edge notifier can ever be asked for.
void WaitedProps::add_container(const slang::ast::ClassPropertySymbol& prop) {
    Waited& w = props[&prop];
    w.edges |= static_cast<uint8_t>(1u << static_cast<uint32_t>(ir::Edge::Any));
    w.container = true;
}

bool WaitedProps::contains(const slang::ast::ClassPropertySymbol& prop) const {
    return props.contains(&prop);
}

bool WaitedProps::has(const slang::ast::ClassPropertySymbol& prop, ir::Edge edge) const {
    const auto it = props.find(&prop);
    return it != props.end() && (it->second.edges & static_cast<uint8_t>(1u << static_cast<uint32_t>(edge))) != 0;
}

bool WaitedProps::is_container(const slang::ast::ClassPropertySymbol& prop) const {
    const auto it = props.find(&prop);
    return it != props.end() && it->second.container;
}

void WaitPropScan::handle(const slang::ast::SignalEventControl& ev) {
    if (const auto* prop = waited_class_prop(ev.expr); prop != nullptr) {
        // An edge on a property whose type has no LSB lattice mints no notifier.
        // slang rejects such a design outright ("expression type 'T' is not
        // integral", LRM 9.4.2 / Table 9-2), so this is not the diagnostic; it is
        // what keeps the LAYOUT correct-by-construction, since a stray `.pos` row
        // here would shift every later field and break the prefix invariant a
        // base-class method writes through.
        const bool edges = prop_has_edges(prop->getType().getCanonicalType());
        switch (ev.edge) {
            case slang::ast::EdgeKind::PosEdge:
                if (edges) {
                    out->add(*prop, ir::Edge::Positive);
                }
                break;
            case slang::ast::EdgeKind::NegEdge:
                if (edges) {
                    out->add(*prop, ir::Edge::Negative);
                }
                break;
            case slang::ast::EdgeKind::BothEdges:
                // `edge` is posedge-or-negedge (LRM 9.4.2), not any change, so it
                // takes both edge notifiers and NOT the any-change one -- an
                // x <-> z transition must not wake it.
                if (edges) {
                    out->add(*prop, ir::Edge::Positive);
                    out->add(*prop, ir::Edge::Negative);
                }
                break;
            default:
                out->add(*prop, ir::Edge::Any);
                break;
        }
    }
    visitDefault(ev);
}

void WaitPropScan::handle(const slang::ast::WaitStatement& w) {
    // A level wait re-tests its condition on every wake, so any change in any
    // property the condition reads is enough: the any-change notifier only.
    ClassPropReadScan scan;
    w.cond.visit(scan);
    // Every value symbol the condition names, for the capture-box notifiers: a
    // fork-captured automatic in a wait condition is heap storage like a class
    // property, and its box needs a notifier minted before the box class exists.
    WaitLocalScan locals;
    locals.out = &out->locals;
    w.cond.visit(locals);
    for (const Expression* e : scan.notifiable) {
        const slang::ast::ClassPropertySymbol& prop = *level_waited_prop(*e);
        if (is_notified_container(prop.getType().getCanonicalType())) {
            out->add_container(prop);
        } else {
            out->add(prop, ir::Edge::Any);
        }
    }
    visitDefault(w);
}

// Every class declared under `scope`, appended to `out`. This is what makes static
// property initialization eager (LRM 6.21 / 8.9): the initializers must run once at
// the beginning of simulation whether or not anything ever names the property, so
// the set of classes cannot be "the ones some access site happened to reach".
//
// A generic class is the reason this is a walk of its own rather than a member walk
// or an ASTVisitor. slang monomorphizes each `C#(args)` into its own ClassType which
// is a member of NO scope -- it lives only in the generic definition's
// specialization map -- so `R#(Foo)` named by nothing but a typedef is unreachable
// from `Scope::members()`. That map is a hash map, so specializations are emitted
// sorted by their rendered `C#(args)` name (ClassType::toString, which unlike
// ClassType::name carries the arguments), which is what makes the initialization
// order reproducible rather than hash-dependent.
//
// A package member is deliberately NOT descended into: the caller enumerates
// packages itself (lower_globals), so that a package's classes are initialized once
// on the globals Lowerer rather than once per top that imports it.
void collect_class_types(const Scope& scope, std::vector<const ClassType*>& out) {
    for (const Symbol& m : scope.members()) {
        switch (m.kind) {
            case SymbolKind::ClassType: {
                const auto& cls = m.as<ClassType>();
                out.push_back(&cls);
                collect_class_types(cls, out); // a nested class
                break;
            }
            case SymbolKind::GenericClassDef: {
                std::vector<std::pair<std::string, const ClassType*>> specs;
                for (const Type& spec : m.as<GenericClassDefSymbol>().specializations()) {
                    // A specialization whose arguments failed to elaborate is an
                    // ErrorType, and has no members to initialize.
                    if (const auto* cls = spec.as_if<ClassType>()) {
                        specs.emplace_back(cls->toString(), cls);
                    }
                }
                std::ranges::stable_sort(specs, [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
                for (const auto& [name, cls] : specs) {
                    out.push_back(cls);
                    collect_class_types(*cls, out);
                }
                break;
            }
            case SymbolKind::Instance:
                // Not opaque, unlike the timescale walk below: a class declared in a
                // child module is still a class this design must initialize.
                collect_class_types(m.as<InstanceSymbol>().body, out);
                break;
            case SymbolKind::InstanceArray:
                // The elements are members of the array scope, one nested array per
                // extra unpacked dimension, so this bottoms out at the instances.
                collect_class_types(m.as<InstanceArraySymbol>(), out);
                break;
            case SymbolKind::GenerateBlock: {
                const auto& gb = m.as<GenerateBlockSymbol>();
                if (!gb.isUninstantiated) {
                    collect_class_types(gb, out);
                }
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (const GenerateBlockSymbol* e : m.as<GenerateBlockArraySymbol>().entries) {
                    if (!e->isUninstantiated) {
                        collect_class_types(*e, out);
                    }
                }
                break;
            default:
                break;
        }
    }
}

// True iff this scope (an instance body or one of its generate blocks — sharing
// the enclosing instance's timescale — or a package / `$unit` compilation unit)
// textually contains a real delay, in a procedural block, a subroutine body, an
// intra-assign, a continuous assign, a net declaration, or a gate primitive.
// Nested instances are opaque: they carry their own timescale and fold their own
// precision, so this stops at the instance boundary. A subroutine counts here
// because its delays are scaled by the scope that DECLARES it (LRM 3.14.2.3), so
// the rounding they need is this scope's precision, not any caller's.
bool scope_has_own_real_delay(const Scope& scope) {
    for (const Symbol& m : scope.members()) {
        switch (m.kind) {
            case SymbolKind::ProceduralBlock: {
                RealDelayFinder f;
                m.as<ProceduralBlockSymbol>().getBody().visit(f);
                if (f.found) {
                    return true;
                }
                break;
            }
            case SymbolKind::ContinuousAssign:
                if (delay_is_real(m.as<ContinuousAssignSymbol>().getDelay())) {
                    return true;
                }
                break;
            case SymbolKind::Net:
                if (delay_is_real(m.as<NetSymbol>().getDelay())) {
                    return true;
                }
                break;
            case SymbolKind::PrimitiveInstance:
                if (delay_is_real(m.as<PrimitiveInstanceSymbol>().getDelay())) {
                    return true;
                }
                break;
            case SymbolKind::Subroutine:
                if (subroutine_has_real_delay(m.as<SubroutineSymbol>())) {
                    return true;
                }
                break;
            case SymbolKind::ClassType:
                // A class method is a subroutine of the class scope, which
                // inherits this scope's timescale.
                if (scope_has_own_real_delay(m.as<slang::ast::ClassType>())) {
                    return true;
                }
                break;
            case SymbolKind::GenerateBlock: {
                const auto& gb = m.as<GenerateBlockSymbol>();
                if (!gb.isUninstantiated && scope_has_own_real_delay(gb)) {
                    return true;
                }
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (const GenerateBlockSymbol* e : m.as<GenerateBlockArraySymbol>().entries) {
                    if (!e->isUninstantiated && scope_has_own_real_delay(*e)) {
                        return true;
                    }
                }
                break;
            default:
                break;
        }
    }
    return false;
}

// Smallest child time unit (fs) reachable from a scope, descending into nested
// instances and through generate blocks/arrays (whose instances share the
// enclosing instance's timescale but may instantiate further modules).
// Uninstantiated generate branches contribute nothing.
uint64_t scope_min_tick_fs(const Scope& scope, bool& any_explicit) {
    uint64_t   min_fs   = 0;
    const auto consider = [&](uint64_t child) {
        if (child != 0 && (min_fs == 0 || child < min_fs)) {
            min_fs = child;
        }
    };
    for (const Symbol& m : scope.members()) {
        switch (m.kind) {
            case SymbolKind::Instance:
                consider(design_tick_fs(m.as<InstanceSymbol>(), any_explicit));
                break;
            case SymbolKind::GenerateBlock: {
                const auto& gb = m.as<GenerateBlockSymbol>();
                if (!gb.isUninstantiated) {
                    consider(scope_min_tick_fs(gb, any_explicit));
                }
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (const GenerateBlockSymbol* e : m.as<GenerateBlockArraySymbol>().entries) {
                    if (!e->isUninstantiated) {
                        consider(scope_min_tick_fs(*e, any_explicit));
                    }
                }
                break;
            default:
                break;
        }
    }

    return min_fs;
}

uint64_t design_tick_fs(const InstanceSymbol& inst, bool& any_explicit) {
    uint64_t   min_fs   = 0;
    const auto consider = [&](uint64_t f) {
        if (f != 0 && (min_fs == 0 || f < min_fs)) {
            min_fs = f;
        }
    };
    if (inst.getDefinition().timeScale.has_value()) {
        any_explicit = true;
    }
    if (const auto ts = inst.body.getTimeScale()) {
        consider(tsv_to_fs(ts->base));
        // A real delay in this module rounds to its precision, so the tick must
        // divide that precision. Only fold it when a real delay is actually
        // present, so integer-only designs keep the coarser minimum-unit tick.
        if (scope_has_own_real_delay(inst.body)) {
            consider(tsv_to_fs(ts->precision));
        }
    }
    consider(scope_min_tick_fs(inst.body, any_explicit));

    return min_fs;
}

// Smallest time *precision* (fs) across the design's effective timescales. This
// is the default %t time unit (LRM Table 20-3), distinct from the wheel tick,
// which tracks the smallest *unit*. Returns 0 if no scope has a timescale.
uint64_t design_min_precision_fs(const InstanceSymbol& inst) {
    uint64_t min_fs = 0;
    if (const auto ts = inst.body.getTimeScale()) {
        min_fs = tsv_to_fs(ts->precision);
    }
    for (const Symbol& m : inst.body.members()) {
        if (m.kind == SymbolKind::Instance) {
            const uint64_t child = design_min_precision_fs(m.as<InstanceSymbol>());
            if (child != 0 && (min_fs == 0 || child < min_fs)) {
                min_fs = child;
            }
        }
    }

    return min_fs;
}

// True iff a package / `$unit` scope declares anything whose body can carry
// time: a subroutine, or a class (whose methods are subroutines).
bool scope_declares_subroutine(const Scope& scope) {
    for (const Symbol& m : scope.members()) {
        switch (m.kind) {
            case SymbolKind::Subroutine:
            case SymbolKind::GenericClassDef:
                return true;
            case SymbolKind::ClassType:
                if (scope_declares_subroutine(m.as<slang::ast::ClassType>())) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

// Wheel-tick contribution of a package or `$unit` compilation unit. Such a scope
// sits outside the instance tree, so design_tick_fs never sees it -- yet a
// subroutine declared there scales its delays and its $time reads by THIS
// scope's timescale, not the caller's (LRM 3.14.2.3), so the tick has to divide
// that unit or the delay cannot be represented at all. Folds the same two terms
// a module does: the unit, plus the precision when a real delay is present.
//
// Gated on the scope actually declaring a subroutine. A package of types,
// parameters and variables reaches simulation without ever scaling a time value,
// and folding its (possibly much finer) unit would shrink the tick -- and with
// it the default %t unit -- for nothing.
uint64_t global_scope_tick_fs(const Scope& scope, bool& any_explicit) {
    const auto ts = scope.getTimeScale();
    if (!ts || !scope_declares_subroutine(scope)) {
        return 0;
    }
    any_explicit        = true;
    const uint64_t unit = tsv_to_fs(ts->base);
    const uint64_t prec = tsv_to_fs(ts->precision);
    return scope_has_own_real_delay(scope) && prec < unit ? prec : unit;
}

// Time-unit exponent (10^n seconds) of a power-of-ten femtosecond magnitude. A
// timescale value is always a power of ten, so the base-10 log is exact;
// fs = 10^(n+15) s, so the unit exponent is log10(fs) - 15.
int32_t fs_to_unit_exponent(uint64_t fs) {
    return static_cast<int32_t>(std::lround(std::log10(static_cast<double>(fs)))) - 15;
}

ir::SignalInfo::Direction direction_of(slang::ast::ArgumentDirection d) {
    switch (d) {
        case slang::ast::ArgumentDirection::In:
            return ir::SignalInfo::Direction::Input;
        case slang::ast::ArgumentDirection::Out:
            return ir::SignalInfo::Direction::Output;
        case slang::ast::ArgumentDirection::InOut:
            return ir::SignalInfo::Direction::Inout;
        case slang::ast::ArgumentDirection::Ref:
            return ir::SignalInfo::Direction::Internal;
    }

    return ir::SignalInfo::Direction::Internal;
}

// The signal backing a symbol, or nullopt when it has none: a genvar, an enum
// member, a parameter, or a frame-resident local.
std::optional<ir::Signal> SensitivityCollector::lookup(const ValueSymbol& sym) {
    const ValueSymbol& csym = canonical_value_symbol(sym);
    if (const auto it = sigmap.find(&csym); it != sigmap.end()) {
        return it->second;
    }
    const auto* prop = csym.as_if<slang::ast::ClassPropertySymbol>();
    if (prop == nullptr || prop->lifetime != slang::ast::VariableLifetime::Static ||
        !declare_static) {
        return std::nullopt;
    }
    return declare_static(*prop);
}

void SensitivityCollector::record_read(const ValueSymbol& sym) {
    if (sym.kind == SymbolKind::Parameter || sym.kind == SymbolKind::Specparam) {
        return;
    }
    const std::optional<ir::Signal> sig = lookup(sym);
    if (!sig) {
        return;
    }
    if (seen.insert(static_cast<uint32_t>(*sig)).second) {
        reads.push_back(*sig);
    }
}

void SensitivityCollector::mark_written(const ValueSymbol& sym) {
    if (const std::optional<ir::Signal> sig = lookup(sym)) {
        written.insert(static_cast<uint32_t>(*sig));
    }
}

// Record the write target(s) of an lvalue: the base name is written, but any
// selector expression on it (an index or a non-constant part-select bound) is
// a read and is descended into.
void SensitivityCollector::record_write(const Expression& lhs) {
    switch (lhs.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue:
            mark_written(lhs.as<ValueExpressionBase>().symbol);
            break;
        case ExpressionKind::ElementSelect: {
            const ElementSelectExpression& es = lhs.as<ElementSelectExpression>();
            record_write(es.value());
            es.selector().visit(*this);
            break;
        }
        case ExpressionKind::RangeSelect: {
            const RangeSelectExpression& rs = lhs.as<RangeSelectExpression>();
            record_write(rs.value());
            rs.left().visit(*this);
            rs.right().visit(*this);
            break;
        }
        case ExpressionKind::Concatenation:
            for (const Expression* op : lhs.as<ConcatenationExpression>().operands()) {
                record_write(*op);
            }
            break;
        case ExpressionKind::SimpleAssignmentPattern:
        case ExpressionKind::StructuredAssignmentPattern:
        case ExpressionKind::ReplicatedAssignmentPattern: {
            // `'{a, b} = rhs` (LRM 10.9): each pattern member is a write
            // target, same shape as a concatenation lvalue. Keyed/replicated
            // are illegal as an lvalue (rejected at lowering) but are walked
            // the same way here — sensitivity analysis is best-effort ahead
            // of that guard.
            const auto& pat = static_cast<const slang::ast::AssignmentPatternExpressionBase&>(lhs);
            for (const Expression* op : pat.elements()) {
                record_write(*op);
            }
            break;
        }
        case ExpressionKind::Conversion:
            record_write(lhs.as<ConversionExpression>().operand());
            break;
        default:
            // An unrecognized lvalue shape: descend conservatively so any
            // reads inside it are still picked up.
            lhs.visit(*this);
            break;
    }
}

void SensitivityCollector::handle(const NamedValueExpression& nv) {
    record_read(nv.symbol);
}

// A hierarchical read (`@* … = top.u1.sig`, LRM 23.6) makes the process
// sensitive to the target instance's signal, same as a local read.
void SensitivityCollector::handle(const HierarchicalValueExpression& hv) {
    record_read(hv.symbol);
}

void SensitivityCollector::handle(const AssignmentExpression& a) {
    a.right().visit(*this); // RHS: pure reads
    if (a.isCompound()) {
        // `a += b` reads its target too. In Comb mode the write below
        // subtracts it back out; in ImplicitEvent mode it stays sensitive.
        a.left().visit(*this);
    }
    record_write(a.left());
}

// Identifiers that appear only in a timing-control (wait / event / delay)
// expression carry no sensitivity (9.2.2.2.1 (c), 9.4.2.2): skip the timing
// control and walk only the controlled statement. Reachable only from
// `always @*` (Comb procedures reject timing controls at elaboration).
void SensitivityCollector::handle(const TimedStatement& t) {
    t.stmt.visit(*this);
}

// A `fork` block's children are independent concurrent processes (LRM 9.3.2)
// with their own sensitivity — their reads never wake the parent process, so
// do not descend into them. A plain sequential block descends as usual.
void SensitivityCollector::handle(const BlockStatement& blk) {
    if (blk.blockKind == slang::ast::StatementBlockKind::Sequential) {
        visitDefault(blk);
    }
}

void SensitivityCollector::handle(const UnaryExpression& u) {
    switch (u.op) {
        case UnaryOperator::Preincrement:
        case UnaryOperator::Postincrement:
        case UnaryOperator::Predecrement:
        case UnaryOperator::Postdecrement:
            // A ++/-- target is both read and written, so it drops out.
            record_write(u.operand());
            break;
        default:
            break;
    }
    visitDefault(u);
}

void SensitivityCollector::handle(const VariableDeclStatement& d) {
    mark_written(d.symbol); // a block-local temporary, not an input
    visitDefault(d);
}

void SensitivityCollector::handle(const ForLoopStatement& f) {
    for (const VariableSymbol* v : f.loopVars) {
        mark_written(*v);
    }
    visitDefault(f);
}

// A called subroutine's argument expressions are always sensitive. Its body
// contributes only for a *function* under Comb rules (9.2.2.2.1): a function
// may reference module signals directly, not only through its arguments, so
// descend once into the callee body — the callee's formals / locals are not
// module signals (not in sigmap), so they are ignored, while module signals
// it reads / writes are recorded as usual. Task bodies never contribute
// (9.2.2.2.1: "the contents of the tasks do not add anything"), and under
// ImplicitEvent (@*) rules no callee body does (9.2.2.2.2).
void SensitivityCollector::handle(const CallExpression& c) {
    for (const Expression* arg : c.arguments()) {
        arg->visit(*this);
    }
    if (c.isSystemCall() || mode != SensitivityMode::Comb) {
        return;
    }
    const SubroutineSymbol* sub = std::get<0>(c.subroutine);
    if (sub != nullptr && sub->subroutineKind == SubroutineKind::Function && entered.insert(sub).second) {
        sub->getBody().visit(*this);
    }
}

// Whether this node, ignoring its children, is safe to evaluate eagerly.
bool SpeculationScan::node_safe(const slang::ast::Expression& e) {
    switch (e.kind) {
        // Literals: no storage read, nothing to trap on.
        case ExpressionKind::IntegerLiteral:
        case ExpressionKind::RealLiteral:
        case ExpressionKind::TimeLiteral:
        case ExpressionKind::UnbasedUnsizedIntegerLiteral:
        case ExpressionKind::NullLiteral:
        case ExpressionKind::UnboundedLiteral:
        case ExpressionKind::StringLiteral:
        // Pure value construction over the operands.
        case ExpressionKind::BinaryOp:
        case ExpressionKind::ConditionalOp:
        case ExpressionKind::Inside:
        case ExpressionKind::ValueRange:
        case ExpressionKind::Concatenation:
        case ExpressionKind::Streaming:
        case ExpressionKind::Conversion:
        case ExpressionKind::TaggedUnion:
        case ExpressionKind::SimpleAssignmentPattern:
        case ExpressionKind::StructuredAssignmentPattern:
        case ExpressionKind::ReplicatedAssignmentPattern:
        // Indexing: out of range is a defined default, never a trap.
        case ExpressionKind::ElementSelect:
        case ExpressionKind::RangeSelect:
        // A named interface instance where a virtual interface is expected
        // (LRM 25.9); unlike a virtual-interface handle it cannot be null.
        case ExpressionKind::ArbitrarySymbol:
            return true;
        case ExpressionKind::Replication:
            // A packed replication's multiplier is an elaboration-time
            // constant: pure value construction, no trap. A STRING
            // replication may carry a RUNTIME multiplier (the deliberate
            // LRM 11.4.12.1 deviation, see lower_expr's Replication case),
            // and a negative or x/z value there is a located fatal — a
            // trap, so it must not run unless its arm is chosen.
            return !e.type->getCanonicalType().isString();
        case ExpressionKind::UnaryOp:
            switch (e.as<UnaryExpression>().op) {
                case UnaryOperator::Preincrement:
                case UnaryOperator::Postincrement:
                case UnaryOperator::Predecrement:
                case UnaryOperator::Postdecrement:
                    return false;
                default:
                    return true;
            }
        case ExpressionKind::NamedValue:
            return !is_ref_formal(e.as<NamedValueExpression>().symbol);
        case ExpressionKind::HierarchicalValue:
            return !is_ref_formal(e.as<HierarchicalValueExpression>().symbol);
        case ExpressionKind::MemberAccess:
            return member_base_safe(e.as<MemberAccessExpression>());
        default:
            // Call; Assignment; NewClass / NewArray / NewCovergroup (a heap
            // allocation and a GC safepoint -- and a constructor-less `new` has
            // no call inside it for a blacklist to have caught); CopyClass (a
            // virtual dispatch through a possibly-null source); Dist; MinTypMax;
            // ClockingEvent; AssertionInstance; DataType; TypeReference;
            // LValueReference; EmptyArgument; Invalid.
            return false;
    }
}

// A read of a `ref` formal emits a relocation-epoch check that is a located
// fatal by default (docs/design/ref-args.md, docs/design/data-model.md), so it
// is not speculatable even though it is spelled as a bare name.
bool SpeculationScan::is_ref_formal(const slang::ast::Symbol& sym) {
    const auto* fa = sym.as_if<slang::ast::FormalArgumentSymbol>();
    return fa != nullptr && fa->direction == slang::ast::ArgumentDirection::Ref;
}

// `obj.m` traps whenever the base has to be dereferenced or tag-checked. A
// BARE property inside a method is not this case: it arrives as a NamedValue on
// the ClassPropertySymbol and derefs `this`, which cannot be null.
bool SpeculationScan::member_base_safe(const MemberAccessExpression& ma) {
    const slang::ast::Type& bt = ma.value().type->getCanonicalType();
    return !bt.isClass() && !bt.isVirtualInterface() && !bt.isTaggedUnion();
}

bool is_speculation_safe(const slang::ast::Expression& e) {
    SpeculationScan s;
    e.visit(s);
    return !s.unsafe;
}

void LocalRefDetector::check(const slang::ast::ValueSymbol& sym) {
    if (locals->contains(&sym)) {
        found = true;
    }
}

void LocalRefDetector::handle(const NamedValueExpression& nv) {
    check(nv.symbol);
}

void LocalRefDetector::handle(const HierarchicalValueExpression& hv) {
    check(hv.symbol);
}

// A declaration's initializer hangs off the SYMBOL, not the statement, so a
// statement walk does not reach it. Without this, `fork begin automatic int k = j;
// ... end join_none` -- LRM 9.3.2's own idiom -- never records the reference to
// `j`, and the child then failed to resolve it as an unknown signal instead of
// capturing it.
void ValueRefScan::handle(const VariableDeclStatement& d) {
    if (const Expression* init = d.symbol.getInitializer()) {
        init->visit(*this);
    }
    visitDefault(d);
}

// A bare in-method CALL is the other way `this` is referenced without being
// written: `exec_task(comp, phase)` inside a method resolves against the enclosing
// `this` (resolve_method_this), and nothing in the expression names it. Without
// this, `this` was not treated as a capture and a fork child calling a sibling
// method failed with "class method call without an object handle" -- which is
// exactly what uvm_task_phase::execute does.
//
// The condition mirrors resolve_method_this: no explicit receiver, and a callee
// that is an instance method. A pure virtual has a null thisVar in slang's
// degenerate representation yet is still one, hence the virtualness test; the
// BuiltIn exclusion keeps slang's synthesized `randomize` out, as is_user_virtual
// does.
void ValueRefScan::handle(const CallExpression& call) {
    if (implicit_this != nullptr && call.thisClass() == nullptr && call.subroutine.index() == 0) {
        const auto& sub = *std::get<0>(call.subroutine);
        const bool  virt =
          sub.isVirtual() && !sub.flags.has(slang::ast::MethodFlags::BuiltIn);
        if (sub.thisVar != nullptr || virt) {
            *implicit_this = true;
        }
    }
    visitDefault(call);
}

void ValueRefScan::handle(const NamedValueExpression& nv) {
    take(nv.symbol);
}

void ValueRefScan::handle(const HierarchicalValueExpression& hv) {
    take(hv.symbol);
}

void ValueRefScan::take(const slang::ast::ValueSymbol& sym) {
    out->insert(&sym);
    if (implicit_this == nullptr) {
        return;
    }
    // A bare `p` inside a method arrives as a NamedValue on the PROPERTY, not as
    // anything rooted at `this` (value_lvalue is what re-roots it), so the `this`
    // reference is implied rather than written and has to be recovered here.
    if (const auto* prop = sym.as_if<slang::ast::ClassPropertySymbol>()) {
        if (prop->lifetime != slang::ast::VariableLifetime::Static) {
            *implicit_this = true;
        }
    }
}

void BoxCandidateScan::handle(const BlockStatement& blk) {
    if (is_fork_block(blk)) {
        ValueRefScan rs;
        rs.out           = &candidates;
        rs.implicit_this = &implicit_this;
        // NEITHER HALF of one of the fork's own declarations is a capture, because
        // LRM 9.3.2 initializes such a variable "before any processes are spawned":
        // lower_fork materializes it in the PARENT at the fork and gives it a fresh
        // box per ENTRY, not the per-activation box install_capture_boxes hands out.
        // Two consequences, and the clause's example turns on both:
        //
        //   - the variable itself must not be boxed per activation, or one box
        //     would be shared by every iteration's children and the clause's
        //     example would print 333 instead of 123;
        //   - its INITIALIZER is evaluated in the parent, so what that initializer
        //     reads is not captured at all. That is what makes the clause's own
        //     carve-out implementable -- referring to a formal passed by reference
        //     is legal there even under join_any / join_none, precisely because no
        //     child-side access is involved.
        //
        // A `begin ... end` nested inside the fork is a DIFFERENT block, so its
        // declarations are none of this and keep the ordinary late path: the
        // clause's own example says the nested `automatic int m = j` "is 4".
        for_each_fork_stmt(blk, [&](const Statement& s) {
            if (const auto* d = s.as_if<VariableDeclStatement>()) {
                fork_scope_decls.insert(&d->symbol);
                return;
            }
            s.visit(rs);
        });
    }
    visitDefault(blk);
}

void ForkCaptureDetector::check(const slang::ast::ValueSymbol& sym) {
    if (const auto* prop = sym.as_if<slang::ast::ClassPropertySymbol>()) {
        if (prop->lifetime != slang::ast::VariableLifetime::Static) {
            // The property itself is heap storage the child reaches through the
            // handle; what has to travel is the handle.
            if (this_var != nullptr) {
                check(*this_var);
            }
            return;
        }
    }
    if (!locals->contains(&sym) && !(boxed != nullptr && boxed->contains(&sym))) {
        return; // a module signal or a static local: shared storage already
    }
    if (std::find(captured.begin(), captured.end(), &sym) == captured.end()) {
        captured.push_back(&sym);
    }
}

void ForkCaptureDetector::handle(const NamedValueExpression& nv) {
    check(nv.symbol);
}

void ForkCaptureDetector::handle(const HierarchicalValueExpression& hv) {
    check(hv.symbol);
}

// A declaration's initializer hangs off the SYMBOL, not the statement, so a
// statement walk does not reach it. Without this, `fork begin automatic int k = j;
// ... end join_none` -- LRM 9.3.2's own idiom -- never records the reference to
// `j`, and the child then failed to resolve it as an unknown signal instead of
// capturing it.
void ForkCaptureDetector::handle(const VariableDeclStatement& d) {
    if (const Expression* init = d.symbol.getInitializer()) {
        init->visit(*this);
    }
    visitDefault(d);
}

// A bare in-method CALL references `this` without naming it, exactly as a bare
// property reference does: resolve_method_this resolves it against the enclosing
// method's handle, so that handle is what has to travel. Without this a child
// calling a sibling method compiled to a call with no receiver and was refused
// ("class method call without an object handle") -- which is what
// uvm_task_phase::execute does with `exec_task(comp, phase)`.
//
// Same condition as resolve_method_this: no explicit receiver and an instance
// callee. A pure virtual has a null thisVar in slang's degenerate representation
// yet is still one, hence the virtualness test.
void ForkCaptureDetector::handle(const CallExpression& call) {
    if (this_var != nullptr && call.thisClass() == nullptr && call.subroutine.index() == 0) {
        const auto& sub  = *std::get<0>(call.subroutine);
        const bool  virt = sub.isVirtual() && !sub.flags.has(slang::ast::MethodFlags::BuiltIn);
        if (sub.thisVar != nullptr || virt) {
            check(*this_var);
        }
    }
    visitDefault(call);
}

void ReactiveSafetyChecker::reject(std::string_view what, std::string_view why) {
    if (reason.empty()) {
        reason = std::string(what)
                   .append(copy == Copy::Pended ? " in an assertion action block is not supported: "
                                                : " in the action block of a strong assertion is not supported: ")
                   .append(why);
    }
}

// A construct that suspends and nothing more. Legal in the pended copy, where
// the wake carries the handle's home region -- and a delay is now one of them:
// a wheel timer records its target as a parked handle, so it fires into the
// reactive ring of the slot it matures in and that slot's drain resumes it
// there (emit_delay_timer / emit_wake_push in LlvmBackend.cpp). Never legal in
// the inlined copy, which has no second resume at all.
//
// So `#0`, `#5` and a run-time `#d` are one case here rather than three. They
// used to be told apart by constant-folding the amount, because only `#0` had
// a region to come back through; the fold is gone with the distinction.
void ReactiveSafetyChecker::suspends(std::string_view what) {
    if (copy == Copy::Inlined) {
        reject(what, kFinalCopy);
    }
}

void ReactiveSafetyChecker::handle(const AssignmentExpression& a) {
    // A plain `x <= y` is fine: LRM 4.4.2.8 gives the reactive region set its
    // own update region, and the backend commits it there (re_nba.commit).
    //
    // A DELAYED or event-controlled one is not, and the reason is the commit
    // and not the wake -- none of the three forms suspends the process that
    // issues it, so the parked handle has nothing to say about them. The update
    // lands in a LATER time slot, where emit_intra_commit runs it in that slot's
    // ordinary NBA region rather than its Re-NBA one, and 4.4.2.8 scopes Re-NBA
    // to updates issued from the reactive region set without saying which
    // slot's region a delayed one belongs to. The LRM does not settle it, so it
    // is refused rather than guessed; the backend refuses the same
    // three tags with the same reason.
    if (a.timingControl != nullptr) {
        reject("an intra-assignment delay",
               "the update commits in a later time slot's ordinary NBA region rather than that "
               "slot's Re-NBA region, and which of the two LRM 4.4.2.8 means for an assignment "
               "issued from reactive code is not settled");
    }
    visitDefault(a);
}

void ReactiveSafetyChecker::handle(const CallExpression& c) {
    // A system task ($display / $error / ...) is zero-time by construction. A
    // user task is not, and clotho does not trace the body -- so it cannot rule
    // out a construct this walk would have refused had the user written it in
    // the action block itself: a `forever`, an intra-assignment delay, a
    // deferred immediate assertion. Suspending is no longer among them.
    //
    // Sharing the task with ordinary code is no longer among them either. That
    // used to be a second, independent reason -- a body records one home region
    // and is lowered once -- and the backend refused it separately; LRM 24.3.1
    // now puts the home on the THREAD, so a shared body reads its caller's
    // (build_shared_home_fns in LlvmBackend.cpp) and the question is settled.
    // What remains is only the untraced-body one.
    //
    // A user *function* needs no check at all: LRM 13.4.4 forbids a timing
    // control in one, so calling it cannot suspend, and a function that cannot
    // suspend is safe to share between reactive and ordinary callers.
    if (c.subroutine.index() == 0 && std::get<0>(c.subroutine)->subroutineKind == SubroutineKind::Task) {
        reject("a task call",
               "clotho does not trace the body, so it cannot rule out a construct there that "
               "would be refused if it were written here");
    }
    // Arguments only, rather than visitDefault: a call's other child nodes are
    // the `with` constraint block of a randomize(), which is declarative and
    // holds no statement this walk has anything to say about.
    for (const Expression* arg : c.arguments()) {
        arg->visit(*this);
    }
}

} // namespace clotho::lower
