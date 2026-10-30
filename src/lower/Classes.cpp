// Class types: layout, change notifiers, vtables, construction and copy.

#include "lower/Lowerer.h"

namespace clotho::lower {

// Publish a lowered class: cache it against its ClassType and record its declared
// name in the pool. One call for both because they are one fact -- this Ref is now
// the design's handle on this class -- and because the name has exactly one chance
// to be recorded. The IR printer prints a class BY NAME instead of expanding its
// body (Printer.cpp's render_class_label), so a class lowered without this shows up
// in `--emit ir` as a bare `class#<ref>`.
//
// The name is the type's own rendering, UNQUALIFIED by scope but carrying a
// specialization's arguments: `uvm_queue#(uvm_callback)`, not `uvm_queue`. Both
// halves of that matter to the printer, whose label has to identify ONE class --
// slang monomorphizes each `C#(args)` into its own ClassType but leaves every one
// of them named plainly `C` (ClassType::name is the syntax name; the arguments live
// in genericParameters, which only the type printer appends --
// TypePrinter.cpp:373-379), so a bare name would collide across every
// specialization of a generic class, of which UVM has many.
//
// This is where the label differs from what `%p` prints for a handle
// (handle_p_name), which is the bare `ClassType::name`: `%p` renders a value for
// someone reading simulation output and LRM 21.2.1.6 leaves its form
// implementation-defined, so it owes no uniqueness. Scope qualification is left
// off in both, because it would not make a label unique on its own -- one
// ClassType lowers once per top, each Lowerer minting its own nominal Ref -- so
// the printer's Ref qualifier is needed either way.
void Lowerer::bind_class_type(const slang::ast::ClassType& clt, ir::Ref ref) {
    class_type_cache_.emplace(&clt, ref);
    module_.pool.set_class_name(ref, clt.toString());
}

// Lower a class type (LRM 8) to a Kind::Class reference type. A class variable
// holds a handle (a pointer, stored as a 2-state 64-bit value); the object body
// layout is the property table, reused from the struct Field machinery but
// NOMINAL (each class gets a fresh, never-deduped fields_id, so two structurally
// identical classes — even two empty ones — never unify). Cached per ClassType
// symbol; the class Ref is interned BEFORE its properties are lowered, so a
// self-referential property (`class Node; Node next; endclass`) resolves through
// the handle indirection with no fixpoint. See docs/design/classes.md.
ir::Ref Lowerer::lower_class_type(const slang::ast::ClassType& clt) {
    if (const auto cached = class_type_cache_.find(&clt); cached != class_type_cache_.end()) {
        return cached->second;
    }
    // The synthesized randomization bodies (compute_vtable reserves their ids)
    // are built by the OUTERMOST frame of this recursion, once every class it
    // reached has its vtable numbering. See rand_pending_.
    const Restore<uint32_t> depth(class_depth_, class_depth_ + 1);
    // `semaphore` (LRM 15.3), `mailbox #(T)` (LRM 15.4) and
    // `std::weak_reference` (LRM 8.30) are built-in classes slang synthesizes
    // into the `std` package. Slang builds their *signatures* only -- every
    // method is a bodyless SubroutineSymbol whose semantics the simulator is
    // expected to supply -- so lowering one walks into a method with no
    // statements and no formals to bind, and used to die on an out-of-range
    // formal index instead of saying anything. Each needs a real runtime: a key
    // count plus a wait queue for a semaphore, a bounded FIFO with blocking
    // put/get/peek for a mailbox.
    //
    // `process` (LRM 9.7) is the one that is implemented: it has its own body
    // shape and its own method lowering, because a `process` object is not a
    // user class but a handle onto a thread's control block. See
    // docs/design/process-class.md.
    //
    // TODO: built-in std classes (semaphore / mailbox / weak_reference) -- need
    // the runtime support described above.
    if (const slang::ast::Scope* parent = clt.getParentScope();
        parent != nullptr && parent->asSymbol().kind == SymbolKind::Package && parent->asSymbol().name == "std") {
        if (clt.name == "process") {
            return lower_process_class(clt);
        }
        if (clt.name == "semaphore") {
            return lower_semaphore_class(clt);
        }
        if (clt.name == "mailbox") {
            return lower_mailbox_class(clt);
        }
        fail("the built-in '" + std::string(clt.name) + "' class is not supported");
    }
    // An interface class (LRM 8.26) is a handle TYPE with no object: it declares
    // only pure virtual prototypes, typedefs and parameters (slang rejects
    // anything else in the parser), it can never be `new`d, and `implements`
    // inherits nothing. So it interns as an ordinary nominal Kind::Class with an
    // EMPTY field table -- which is all dispatch needs from a receiver's static
    // type, since the vtable pointer lives at body field 0 of every class -- and
    // stops there: no property chain, no ancestor display id, no vtable, no
    // copy thunk and no randomization body, all of which describe an object that
    // cannot exist. Its own id space is the descriptor bitmap's
    // (docs/design/interface-classes.md §4).
    if (clt.isInterface) {
        const uint32_t fid = module_.pool.reserve_struct_fields();
        ir::Type       t;
        t.kind      = ir::Type::Kind::Class;
        t.fields_id = fid;
        // No base: slang routes an interface class's `extends` list into the
        // IMPLEMENTS clause (Parser_members.cpp:1160-1166), so getBaseClass() is
        // always null here and the interface DAG lives in iface_closure().
        t.element         = ir::Ref::Invalid;
        const ir::Ref ref = module_.pool.put(ir::Item{ t });
        bind_class_type(clt, ref);
        module_.pool.set_struct_fields(fid, {});
        module_.set_iface_type_id(ref, iface_id(clt));
        return ref;
    }
    // An abstract class (`virtual class`) is lowered normally (it can be a
    // handle type and a base); slang rejects a direct `new` on it (LRM 8.21),
    // and its pure-virtual slots are handled in compute_vtable.
    //
    // A parameterized class (`class C #(...)`, LRM 8.25) needs no special
    // handling: slang monomorphizes each `C#(args)` into a distinct, fully
    // parameter-substituted ClassType (genericClass points back to the generic
    // definition). Each specialization lowers here as an independent nominal
    // class — distinct members, methods (own FunctionIds), and static storage
    // (own ClassPropertySymbols → own signals), keyed per ClassType by
    // class_type_cache_. So a `genericClass != nullptr` specialization is just a
    // normal class from this point on.
    // Intern-first: reserve the field slot and build the class Ref before
    // lowering the base or properties, so a self-handle resolves via the cache.
    const uint32_t fid = module_.pool.reserve_struct_fields();
    ir::Type       t;
    t.kind      = ir::Type::Kind::Class;
    t.fields_id = fid;
    // Single inheritance (LRM 8.13): the object body lays the base chain's
    // properties FIRST (prefix layout, deepest base first), so an upcast is the
    // same pointer and an inherited property keeps one field index in every class
    // that inherits it. `element` records the base-class Ref (Invalid for a root
    // class).
    ir::Ref base_ref = ir::Ref::Invalid;
    if (const slang::ast::Type* base = clt.getBaseClass()) {
        base_ref = lower_class_type(base->getCanonicalType().as<slang::ast::ClassType>());
        // Lowering the base can recurse back into `clt` itself before this frame
        // ever interns it: a base's own property can hold a forward-declared
        // handle to the derived class being laid out right now (UVM's
        // uvm_sequence_item::m_parent_sequence : uvm_sequence_base, where
        // uvm_sequence_base extends uvm_sequence_item). That nested call finds
        // `clt` uncached -- this frame has not reached bind_class_type yet -- so
        // it mints and fully registers its own Ref for `clt`, correctly: this is
        // exactly the shape ensure_vtable's reentrancy contract exists for.
        // Without this re-check, this frame would go on to mint a SECOND Ref for
        // the same class, and compute_vtable's own early-return (keyed on the
        // AST pointer, not the Ref) would treat it as already numbered and skip
        // set_class_desc -- leaving a structurally valid but never-registered Ref
        // that crashes the backend the first time codegen needs its descriptor.
        if (const auto cached = class_type_cache_.find(&clt); cached != class_type_cache_.end()) {
            // The reentrant frame that fully registered `clt` ran its OWN
            // compute_vtable (as part of completing before it could return),
            // which may have reserved -- via rand_body_id -- randomize/randops
            // FunctionIds now sitting undrained in rand_pending_: it was not at
            // class_depth_ == 1, so it correctly deferred draining to the
            // outermost frame. That outermost frame IS this one whenever
            // class_depth_ == 1 here, so it must drain before returning early,
            // exactly like the ordinary (non-reentrant) return points below --
            // otherwise those ids are reserved but never module_.set_function'd,
            // and the backend segfaults on their Ref::Invalid return type the
            // first time codegen reaches them (ir/llvm/Subroutines.cpp declare_function
            // -> fn_return_ty -> width_of -> an invalid-Ref layout lookup).
            if (class_depth_ == 1) {
                drain_rand_bodies();
            }
            return cached->second;
        }
    }
    t.element         = base_ref;
    const ir::Ref ref = module_.pool.put(ir::Item{ t });
    bind_class_type(clt, ref);
    // If anything from here throws before this function returns -- a property
    // whose type is fundamentally unsupported, most concretely a covergroup
    // (LRM 19: UVM's `apb_coverage_monitor extends uvm_subscriber#(...)` embeds
    // one, and `` `uvm_component_utils`` forces every registered class's
    // factory proxy through here via its eager `local static this_type me =
    // get();` singleton, LRM 6.21, whether or not the design ever constructs
    // one) -- this entry must not survive: the cache check at the top of this
    // function treats class_type_cache_ membership as "this class is fully
    // registered, reuse its Ref", with no notion of an abandoned attempt. Left
    // in place, a LATER, unrelated reference to the same class -- reached
    // through a completely different call chain than the one that failed, e.g.
    // the registry's own `create()` casting to it, uvm_registry.svh:116's
    // `$cast(create, obj)` -- would silently reuse this Ref as if it were
    // valid: a structurally well-typed class with an intact field table but no
    // vtable and no descriptor, which crashes the LLVM backend as an unlocated
    // "internal: class without a descriptor" the first time codegen needs it
    // (ir/llvm/Inst.cpp's ClassIsA case, mod_.class_desc(dest) == nullptr).
    // Erase on the way out so every later reference gets its own honest,
    // located LowerError instead of inheriting a landmine planted one
    // class-lowering attempt ago. Mirrors compute_vtable's own VslotGuard for
    // the exact same "done but undescribed" hazard, one step earlier.
    struct InternGuard {
        ankerl::unordered_dense::map<const slang::ast::ClassType*, ir::Ref>& table;
        const slang::ast::ClassType*                                        key;
        bool                                                                committed = false;
        ~InternGuard() {
            if (!committed) {
                table.erase(key);
            }
        }
    } intern_guard{ class_type_cache_, &clt };

    // The chain, deepest base first — the field order of the object body.
    std::vector<const slang::ast::ClassType*> chain;
    for (const slang::ast::ClassType* k = &clt; k != nullptr; k = base_class_of(*k)) {
        chain.push_back(k);
    }
    std::ranges::reverse(chain);

    // Two passes, both over the whole chain.
    //
    // Over the CHAIN and not the base's own already-flattened field table, because
    // a property's type lowers without bound: a class-handle property lowers its
    // pointee class, whose compute_vtable lowers method bodies, any of which can
    // reach back to a class still laying itself out below on this stack. Copying
    // the base's table then reads an empty table and silently gives this class's
    // own properties the base's indices. Rebuilding from the property symbols
    // costs a cached lower_type per inherited property and depends on no other
    // class's layout state. (`uvm_component extends uvm_report_object`, and
    // uvm_report_object's `uvm_report_handler m_rh` reaches uvm_component.)
    //
    // TWO passes, because every index must be assigned before the first
    // lower_type: a body reached from that recursion can read any property of any
    // class in this chain, and assigning one at a time left every property
    // declared after the recursing one unindexed (`uvm_report_object`'s
    // `local bit m_rh_set`, declared after `m_rh`).
    std::vector<ir::FieldRow> fields;
    // Owns the composed change-notifier field names; see
    // emit_change_notifier_fields for why the rows cannot hold a temporary.
    std::deque<std::string> chg_names;
    uint32_t                idx = 0;
    ClassRand               rand;
    for (const slang::ast::ClassType* k : chain) {
        if (rand.unsupported.empty() && has_constraint_blocks(*k)) {
            rand.unsupported = "constraint blocks are not supported";
        }
        for (const auto& prop : k->membersOfType<slang::ast::ClassPropertySymbol>()) {
            if (prop.lifetime == slang::ast::VariableLifetime::Static) {
                // A static property (LRM 8.9) is ONE slot shared across all
                // instances, not part of the per-object body — it gets persistent
                // module-signal storage (declare_static_prop), declared eagerly at
                // time zero (init_static_class_props) and on first access. Skip it
                // here.
                continue;
            }
            if (prop.randMode != slang::ast::RandMode::None && rand.unsupported.empty()) {
                if (const char* why = rand_type_unsupported(prop.getType()); why != nullptr) {
                    rand.unsupported = why;
                } else {
                    rand.props.push_back(admit_rand_prop(prop, idx, static_cast<uint32_t>(rand.props.size())));
                }
            }
            class_prop_index_.emplace(&prop, idx);
            const uint32_t prop_idx = idx;
            ++idx;
            // A property something in the design waits on carries one hidden
            // `event` property per waited edge (ir::ChangeNotify), placed
            // IMMEDIATELY AFTER it. Immediately after, rather than appended at
            // the end like the rand block, because a BASE-class method writing
            // an inherited property must find the notifier at the index the
            // base's own layout gave it: interleaving keeps the base's whole
            // flattened layout a prefix of every derived class's, which is the
            // invariant prefix layout already rests on. A class no `@` / `wait`
            // names gets nothing here.
            idx += reserve_change_notifiers(prop, ref, prop_idx, idx);
        }
    }
    // The rand block (docs/design/constrained-randomization.md §5): the object's
    // own RNG (LRM 18.14.2), its rand_mode mask (LRM 18.8) and one cyclic-history
    // bitmap per `randc` property, appended AFTER every declared property so no
    // property index moves and the base's layout stays a prefix of this one.
    // Carrying them as ordinary fields is what keeps the backend out of this: a
    // member_select reaches them like any other property, and because none of
    // them is a class handle the GC's trace map is unchanged.
    //
    // The block sits after the properties, so its offset differs between a base
    // and a class derived from it. That is sound only because it is never
    // addressed from a statically unknown type: every access is inside a body
    // synthesized for this exact class and reached through the vtable.
    //
    // Its indices are decided, and PUBLISHED, before the second pass for the same
    // reason the property indices are: lowering a property's type can reach a
    // method body that constructs an object of this very class, and the seeding
    // at that `new` needs the block it is about to get.
    // A reason found in a LATER chain class invalidates the earlier admissions:
    // the whole chain's randomize is refused, so no rand block is laid out and
    // the class pays nothing for the rand machinery it cannot use.
    if (!rand.unsupported.empty()) {
        rand.props.clear();
    }
    uint32_t block = idx;
    if (!rand.props.empty()) {
        rand.present    = true;
        rand.rng_field  = block++;
        rand.mode_field = block++;
        for (RandProp& p : rand.props) {
            if (p.cyclic) {
                p.randc_field = block++;
            }
        }
    }
    class_rand_.emplace(&clt, rand);

    for (const slang::ast::ClassType* k : chain) {
        for (const auto& prop : k->membersOfType<slang::ast::ClassPropertySymbol>()) {
            if (prop.lifetime == slang::ast::VariableLifetime::Static) {
                continue;
            }
            fields.push_back({ prop.name, lower_type(prop.getType()), class_prop_index_.at(&prop) });
            emit_change_notifier_fields(prop, fields, chg_names);
        }
    }
    if (rand.present) {
        const ir::Ref state_ty = rng_state_type();
        fields.push_back({ "$rand.rng", state_ty, rand.rng_field });
        fields.push_back({ "$rand.mode", make_int_type(64, false), rand.mode_field });
        for (const RandProp& p : rand.props) {
            if (p.cyclic) {
                fields.push_back({ "$rand.randc", state_ty, p.randc_field });
            }
        }
    }
    module_.pool.set_struct_fields(fid, std::move(fields));

    // Build the class's runtime descriptor: its dispatch table (LRM 8.20) and
    // its type identity (LRM 8.16). Every class gets one.
    compute_vtable(clt, ref);

    if (class_depth_ == 1) {
        drain_rand_bodies();
    }
    intern_guard.committed = true;
    return ref;
}

// Reserve the object-body field indices of one property's change notifiers, and
// publish them for both the store path (ir::ChangeNotify, read by the backend at
// every member select) and the wait path (prop_notify_, read when lowering the
// `@`). Returns how many indices were taken, starting at `next`.
//
// Called from BOTH the base's and the derived class's layout pass over the same
// property, and must answer identically in each: it keys only on the design-wide
// pre-pass and on `next`, never on which class is being laid out.
uint32_t Lowerer::reserve_change_notifiers(const slang::ast::ClassPropertySymbol& prop,
                                           ir::Ref                                class_ref,
                                           uint32_t                               prop_field,
                                           uint32_t                               next) {
    if (waited_props_ == nullptr || !waited_props_->contains(prop)) {
        return 0;
    }
    constexpr std::array<uint32_t, 3> none{ ir::kNoNotify, ir::kNoNotify, ir::kNoNotify };
    auto [slot, fresh] = prop_notify_.try_emplace(&prop, none);
    uint32_t taken     = 0;
    for (const ir::Edge edge : { ir::Edge::Positive, ir::Edge::Negative, ir::Edge::Any }) {
        if (!waited_props_->has(prop, edge)) {
            continue;
        }
        const auto     e  = static_cast<uint32_t>(edge);
        const uint32_t at = next + taken++;
        if (!fresh && slot->second[e] != at) {
            // Prefix layout: a base's own pass over this property saw the same
            // running index, so a derived class's pass must land on the same slot.
            // Checked rather than assumed -- a mismatch would make a base-class
            // method fire whatever field happened to sit there.
            fail("internal: change-notifier index moved between a base and a "
                 "derived class for property '" +
                 std::string(prop.name) + "'");
        }
        slot->second[e] = at;
        module_.set_change_notify(class_ref, prop_field, edge, at, waited_props_->is_container(prop));
    }
    return taken;
}

// The field rows for one property's notifiers, in the index order
// reserve_change_notifiers handed out. Named `$chg.` so they cannot collide with
// a declared property (a SystemVerilog identifier has no `$`), the same way the
// rand block's `$rand.` rows are kept out of the user's namespace.
//
// `names` owns the composed strings, because ir::FieldRow::name is a
// string_view the pool copies only at set_struct_fields -- a temporary here
// dangles, and did: two notifiers in one class printed a corrupted name.
// A deque, so an earlier row's view survives a later push_back.
void Lowerer::emit_change_notifier_fields(const slang::ast::ClassPropertySymbol& prop,
                                          std::vector<ir::FieldRow>&             fields,
                                          std::deque<std::string>&               names) {
    const auto slots = prop_notify_.find(&prop);
    if (slots == prop_notify_.end()) {
        return;
    }
    const ir::Ref ev = module_.pool.event_type;
    for (const auto& [edge, suffix] : { std::pair{ ir::Edge::Positive, ".pos" },
                                       std::pair{ ir::Edge::Negative, ".neg" },
                                       std::pair{ ir::Edge::Any, "" } }) {
        const uint32_t at = slots->second[static_cast<uint32_t>(edge)];
        if (at != ir::kNoNotify) {
            names.push_back("$chg." + std::string(prop.name) + suffix);
            fields.push_back({ names.back(), ev, at });
        }
    }
}

// True if `m` is a user-declared virtual method (LRM 8.20) — excludes slang's
// built-in virtual `randomize` (MethodFlags::BuiltIn), not a dispatch target.
bool Lowerer::is_user_virtual(const slang::ast::SubroutineSymbol& m) {
    return m.isVirtual() && !m.flags.has(slang::ast::MethodFlags::BuiltIn);
}

// Canonical base class of `clt`, or null for a root class.
const slang::ast::ClassType* Lowerer::base_class_of(const slang::ast::ClassType& clt) {
    return clt.getBaseClass() != nullptr ? &clt.getBaseClass()->getCanonicalType().as<slang::ast::ClassType>()
                                         : nullptr;
}

// Design-wide id of a class, minted on first sight. Ids are unique and nothing
// more, so this needs to know about no other class -- see docs/design/classes.md
// on why the alternative, a dense pre-order numbering of the whole inheritance
// forest, was rejected. The map is shared by every Lowerer: one ClassType has one
// id even when two tops intern it into two nominal Refs.
uint32_t Lowerer::class_type_id(const slang::ast::ClassType& clt) {
    const auto it = class_ids_->ids.find(&clt);
    if (it != class_ids_->ids.end()) {
        return it->second;
    }
    // The display a $cast reads is indexed by the destination's depth and is one
    // fixed width for every class, so a chain deeper than that width has nowhere
    // to record its ancestors.
    uint32_t depth = 0;
    for (const slang::ast::ClassType* k = base_class_of(clt); k != nullptr; k = base_class_of(*k)) {
        ++depth;
    }
    if (depth >= ir::kMaxClassDepth) {
        fail("class inheritance deeper than " + std::to_string(ir::kMaxClassDepth) + " levels is not supported");
    }
    const uint32_t id = class_ids_->next++;
    class_ids_->ids.emplace(&clt, id);
    return id;
}

// Design-wide bit of an interface class in every implementing class's
// descriptor bitmap (LRM 8.26.5), minted on first sight. Dense from 0.
uint32_t Lowerer::iface_id(const slang::ast::ClassType& clt) {
    const auto it = iface_ids_->ids.find(&clt);
    if (it != iface_ids_->ids.end()) {
        return it->second;
    }
    const auto id = static_cast<uint32_t>(iface_ids_->ids.size());
    iface_ids_->ids.emplace(&clt, id);
    return id;
}

// Design-wide index of an interface METHOD NAME in every implementing class's
// interface-method table, minted on first sight. Name keying is LRM 8.26.6.1;
// see IfaceIds.
uint32_t Lowerer::iface_slot(std::string_view name) {
    const auto it = iface_ids_->slots.find(name);
    if (it != iface_ids_->slots.end()) {
        return it->second;
    }
    const auto idx = static_cast<uint32_t>(iface_ids_->slots.size());
    iface_ids_->slots.emplace(name, idx);
    return idx;
}

// Every interface class `clt` implements, transitively (LRM 8.26).
//
// slang's `getImplementedInterfaces()` is already closed over the interface
// `extends` DAG, but NOT over the class's own base chain: it is written only
// when a class carries its own `implements` clause, so
// `class D extends B;` where `B implements I` reports nothing (the doc comment
// at ClassSymbols.h:86-87 says otherwise and is wrong; slang's own
// `Type::implements` walks the base chain at query time for exactly this
// reason). LRM 8.26.2 is explicit that a subclass implicitly implements
// everything its superclass implements, so the base walk is mandatory -- and
// omitting it is the bug Verilator 5.050 ships
// (docs/design/interface-classes.md §11.1).
//
// For an interface class this is its own `extends` closure, which is what
// slang stores in the same list.
std::vector<const slang::ast::ClassType*> Lowerer::iface_closure(const slang::ast::ClassType& clt) {
    std::vector<const slang::ast::ClassType*> out;
    const auto push = [&](const slang::ast::ClassType& iface) {
        // getDeclaredInterfaces is not deduplicated by slang and the flattened
        // list of two parents can name one interface twice (a diamond, LRM
        // 8.26.6.3), so dedupe here. The set is small enough that a linear scan
        // beats a hash set.
        if (std::ranges::find(out, &iface) == out.end()) {
            out.push_back(&iface);
        }
    };
    for (const slang::ast::ClassType* k = &clt; k != nullptr; k = base_class_of(*k)) {
        for (const slang::ast::Type* it : k->getImplementedInterfaces()) {
            const slang::ast::Type& canon = it->getCanonicalType();
            if (!canon.isClass()) {
                // An erroneous `implements` operand; slang has already reported it.
                continue;
            }
            push(canon.as<slang::ast::ClassType>());
        }
    }
    return out;
}

// The pure-virtual prototype named `name` reachable in `clt`'s hierarchy (LRM
// 8.21) or in an interface class it implements (LRM 8.26), or null. Two uses:
// the stub that fills an abstract class's un-overridden slot, and the signature
// of a dispatch bound to a pure method.
//
// The interface search cannot be folded into the base-chain walk: prototypes an
// interface inherited from its parents are TransparentMemberSymbols in its
// scope, and membersOfType<T> filters on the symbol kind without unwrapping
// them (Scope.h:224-227), so each interface must be visited in its own right --
// which is what iface_closure yields.
const slang::ast::MethodPrototypeSymbol* Lowerer::find_pure_proto(const slang::ast::ClassType& clt,
                                                                  std::string_view             name) {
    for (const slang::ast::ClassType* k = &clt; k != nullptr; k = base_class_of(*k)) {
        for (const auto& p : k->membersOfType<slang::ast::MethodPrototypeSymbol>()) {
            if (p.name == name && p.flags.has(slang::ast::MethodFlags::Pure)) {
                return &p;
            }
        }
    }
    for (const slang::ast::ClassType* iface : iface_closure(clt)) {
        for (const auto& p : iface->membersOfType<slang::ast::MethodPrototypeSymbol>()) {
            if (p.name == name && p.flags.has(slang::ast::MethodFlags::Pure)) {
                return &p;
            }
        }
    }
    return nullptr;
}

// Assign vtable slots and register the descriptor of a class (LRM 8.16/8.20/8.21).
// Slots are keyed by method NAME per hierarchy (a base's virtuals take slots
// 0..k-1; an override — including an implicit one, or the override of a pure
// prototype whose `getOverride()` slang leaves null — reuses the base slot of the
// same name; a newly-introduced virtual appends). Name-keying is what lets a pure
// virtual (a MethodPrototypeSymbol, invisible to membersOfType<SubroutineSymbol>)
// share a slot with its concrete overrides. The concrete table maps each slot to
// the most-derived body of that name; a pure slot never overridden (only in an
// abstract class) is filled by a prototype-built stub, which supplies the slot's
// signature but is never executed (an abstract class is never `new`'d, so its
// vtable is never materialized). Called after the base is lowered.
void Lowerer::compute_vtable(const slang::ast::ClassType& clt, ir::Ref class_ref) {
    if (class_vslot_.contains(&clt)) {
        // Already numbered, by ensure_vtable reaching this class from a virtual
        // call site before the class's own lower_class_type frame got here. The
        // numbering is the part that has to happen once; the table below is keyed
        // off it and was filled by that same earlier call.
        return;
    }
    const slang::ast::ClassType* base = base_class_of(clt);
    // The base's numbering, which the derived class extends. `ensure_vtable` and
    // not `class_vslot_[base]`: operator[] INSERTS an empty map for a base that
    // has not been numbered yet, and the derived class then numbers its own
    // virtuals from 0, silently aliasing every inherited slot.
    VtableSlots slots = base != nullptr ? ensure_vtable(*base) : VtableSlots{};
    uint32_t    size  = static_cast<uint32_t>(slots.size());
    for (const auto& m : clt.membersOfType<slang::ast::SubroutineSymbol>()) {
        if (is_user_virtual(m) && !slots.contains(m.name)) {
            slots.emplace(m.name, size++);
        }
    }
    for (const auto& p : clt.membersOfType<slang::ast::MethodPrototypeSymbol>()) {
        // A pure (or extern) virtual prototype. A pure slot needs no shape
        // restriction here: build_pure_stub rebuilds every signature the concrete
        // override can have — function or task, scalar or non-scalar result.
        if (p.isVirtual() && !slots.contains(p.name)) {
            slots.emplace(p.name, size++);
        }
    }
    // Every class also carries a shallow-copy thunk (LRM 8.12), so `new src`
    // through a base-typed handle allocates the source's own class. The slot
    // lives in the same name-keyed map under a name no method can have, so it
    // inherits down the hierarchy like any other slot and a derived class's own
    // virtuals number after it.
    if (!slots.contains(kCopySlot)) {
        slots.emplace(kCopySlot, size++);
    }
    // And the two randomization entry points (LRM 18.4/18.8/18.14.2), reserved
    // the same way and for the same reason: `randomize()` is virtual, and both
    // bodies address a rand block whose offset only their own class knows, so a
    // base-typed handle has to arrive at the most-derived body.
    if (!slots.contains(kRandomizeSlot)) {
        slots.emplace(kRandomizeSlot, size++);
    }
    if (!slots.contains(kRandOpsSlot)) {
        slots.emplace(kRandOpsSlot, size++);
    }
    class_vslot_[&clt] = slots;
    // If anything from here throws (a LowerError from build_copy_thunk,
    // rand_body_id, the pure-stub loop below, or build_iface_table) before
    // set_class_desc runs, this entry must not survive: ensure_vtable's
    // reentrancy check above treats class_vslot_ membership as "descriptor
    // fully registered", so a half-registered class left behind here would be
    // permanently unreachable by any later retry -- exactly what the backend
    // would otherwise report as an unlocated "internal: class without a
    // descriptor" crash. Un-number on the way out so the LowerError propagates
    // as the located refusal it already is, instead of a silent ghost entry.
    struct VslotGuard {
        ankerl::unordered_dense::map<const slang::ast::ClassType*, VtableSlots>& table;
        const slang::ast::ClassType*                                            key;
        bool                                                                    committed = false;
        ~VslotGuard() {
            if (!committed) {
                table.erase(key);
            }
        }
    } vslot_guard{ class_vslot_, &clt };
    // Fill each slot with the most-derived body of that name (an override is an
    // override regardless of its own `virtual` keyword — match by name).
    //
    // Reserved, not built. Everything from here is reached from lowering a TYPE,
    // and a body lowers arbitrary code: it reads globals, constructs objects, and
    // pulls in further classes. Building one here ran it against whatever
    // half-finished state the type lowering above us was in the middle of -- a
    // property table mid-index, a package scope mid-registration, a class not yet
    // numbered. drain_pending_bodies builds them all at the next phase boundary.
    const Restore<bool>         defer(defer_bodies_, true);
    std::vector<ir::FunctionId> table(size);
    std::vector<bool>           filled(size, false);
    for (const slang::ast::ClassType* k = &clt; k != nullptr; k = base_class_of(*k)) {
        for (const auto& m : k->membersOfType<slang::ast::SubroutineSymbol>()) {
            const auto it = slots.find(m.name);
            if (it == slots.end() || filled[it->second]) {
                continue;
            }
            table[it->second]  = function_id(m);
            filled[it->second] = true;
        }
        // An `extern virtual` method (LRM 8.24): the class member is the prototype
        // and the body is declared outside the class, so it is not a member
        // SubroutineSymbol and the loop above never sees it. The out-of-block
        // implementation is the body of that slot; a prototype with none is pure
        // and gets a stub below. A class cannot declare both an inline body and a
        // prototype for one name, so this never contends with the loop above.
        for (const auto& p : k->membersOfType<slang::ast::MethodPrototypeSymbol>()) {
            const auto it = slots.find(p.name);
            if (it == slots.end() || filled[it->second]) {
                continue;
            }
            const slang::ast::SubroutineSymbol* impl = p.getSubroutine();
            if (impl == nullptr) {
                continue;
            }
            table[it->second]  = function_id(*impl);
            filled[it->second] = true;
        }
    }
    const uint32_t copy_slot = slots.at(kCopySlot);
    table[copy_slot]         = copy_thunk_id(clt, class_ref);
    filled[copy_slot]        = true;
    // The ids only; the bodies are built by drain_rand_bodies once every class in
    // this lowering has its vtable numbering (a `rand` handle can point back at a
    // class still laying itself out below this frame).
    const uint32_t randomize_slot = slots.at(kRandomizeSlot);
    const uint32_t randops_slot   = slots.at(kRandOpsSlot);
    table[randomize_slot]         = rand_body_id(clt, class_ref, make_int_type(32, false), randomize_fn_);
    filled[randomize_slot]        = true;
    table[randops_slot]           = rand_body_id(clt, class_ref, make_int_type(64, false), randops_fn_);
    filled[randops_slot]          = true;
    // Un-overridden slots: a pure virtual in an abstract class. Fill with a
    // prototype-built stub (the slot's signature source).
    for (const auto& [name, s] : slots) {
        if (filled[s]) {
            continue;
        }
        if (!clt.isAbstract) {
            fail("concrete class has an unimplemented pure virtual method");
        }
        const auto* proto = find_pure_proto(clt, name);
        if (proto == nullptr) {
            fail("internal: pure virtual slot without a prototype");
        }
        table[s] = pure_stub_id(*proto);
    }
    auto [iface_slots, iface_ids] = build_iface_table(clt, slots, table);
    module_.set_class_desc(
      class_ref, std::move(table), class_type_id(clt), std::move(iface_slots), std::move(iface_ids));
    vslot_guard.committed = true;
}

// The interface-dispatch half of a class's descriptor (LRM 8.26): the bodies it
// supplies for the interface methods it implements, indexed by the DESIGN-WIDE
// method name index, and the ids of every interface it implements.
//
// Both are empty for a class that implements nothing, which is also why nothing
// is emitted for a design with no interface class at all. A class that does
// implement one gets a table whose holes -- indices whose name belongs only to
// interfaces this class does not implement -- point at a shared never-executed
// body, so a dispatch bug is a located fatal rather than a jump to address 0.
//
// `slots` / `table` are this class's own name-keyed vtable numbering and its
// filled entries: an interface method's implementation is by construction the
// same body that occupies the ordinary slot of that name, whether the class
// declares it, inherits it from a base (LRM 8.26.2), or re-declares it as a
// pure prototype in a partially-implemented virtual class (LRM 8.26.7).
std::pair<std::vector<ir::FunctionId>, std::vector<uint32_t>> Lowerer::build_iface_table(
  const slang::ast::ClassType&       clt,
  const VtableSlots&                 slots,
  const std::vector<ir::FunctionId>& table) {
    const std::vector<const slang::ast::ClassType*> ifaces = iface_closure(clt);
    if (ifaces.empty()) {
        return {};
    }
    std::vector<uint32_t> ids;
    ids.reserve(ifaces.size());
    // name index -> the body, collected first because the table's extent is the
    // highest index this class needs and the indices are minted as we walk.
    std::vector<std::pair<uint32_t, ir::FunctionId>> entries;
    for (const slang::ast::ClassType* iface : ifaces) {
        ids.push_back(iface_id(*iface));
        for (const auto& p : iface->membersOfType<slang::ast::MethodPrototypeSymbol>()) {
            if (!p.flags.has(slang::ast::MethodFlags::Pure)) {
                continue;
            }
            const auto it = slots.find(p.name);
            if (it == slots.end()) {
                // slang requires an implementation for every prototype reachable
                // through `implements` (diag::IfaceMethodNoImpl), and a virtual
                // class that re-declares one keeps a prototype of that name, so
                // the name is in this class's own numbering either way.
                fail("internal: implemented interface method '" + std::string(p.name) +
                     "' has no vtable slot");
            }
            entries.emplace_back(iface_slot(p.name), table[it->second]);
        }
    }
    // `randomize()` is legal through an interface-class handle (LRM 8.26.9), and
    // the two reserved randomization slots are numbered per hierarchy, so an
    // interface handle cannot reach them by slot. Give them design-wide indices
    // too; every consumer already looks them up by name, so this costs nothing
    // but two entries.
    entries.emplace_back(iface_slot(kRandomizeSlot), table[slots.at(kRandomizeSlot)]);
    entries.emplace_back(iface_slot(kRandOpsSlot), table[slots.at(kRandOpsSlot)]);
    uint32_t extent = 0;
    for (const auto& [idx, fid] : entries) {
        extent = std::max(extent, idx + 1);
    }
    std::vector<ir::FunctionId> iface_slots(extent, iface_unreachable_id());
    for (const auto& [idx, fid] : entries) {
        iface_slots[idx] = fid;
    }
    return { std::move(iface_slots), std::move(ids) };
}

// The shared never-executed body an interface-method table hole points at (see
// build_iface_table). Reaching it means a dispatch resolved to a class that does
// not implement the interface, which the assignment rules make impossible -- so
// it is a clotho bug, and it says so with a location rather than faulting.
ir::FunctionId Lowerer::iface_unreachable_id() {
    // Cached on the MODULE, not this Lowerer: several Lowerers share one module
    // (one per top plus the globals one), and the backend also pads short tables
    // with this body, so there must be exactly one.
    if (module_.iface_unreachable.has_value()) {
        return *module_.iface_unreachable;
    }
    const ir::FunctionId id   = module_.reserve_function(module_.pool.void_type);
    module_.iface_unreachable = id;
    ir::Builder b(module_);
    b.severity(static_cast<uint32_t>(ir::Severity::Fatal),
               "",
               "internal: interface method dispatched on a class that does not implement it",
               {},
               ir::SourceLoc{});
    b.finish("", 1, ir::SourceLoc{});
    b.ret_void(ir::SourceLoc{});
    module_.set_function(id,
                        ir::Function{ .name        = "$iface.unreachable",
                                      .return_type = module_.pool.void_type,
                                      .ir          = b.finish() });
    return id;
}

// The FunctionId of the never-executed stub for a pure virtual prototype (its
// signature source; see compute_vtable). Cached per prototype.
ir::FunctionId Lowerer::pure_stub_id(const slang::ast::MethodPrototypeSymbol& proto) {
    const auto it = pure_stub_.find(&proto);
    if (it != pure_stub_.end()) {
        return it->second;
    }
    const ir::Ref return_type =
      proto.getReturnType().isVoid() ? module_.pool.void_type : lower_type(proto.getReturnType());
    const ir::FunctionId id = module_.reserve_function(return_type);
    pure_stub_[&proto]      = id;
    build_pure_stub(proto, id);
    return id;
}

// Build a bodiless stub matching a pure virtual's signature (this + formals ->
// result, LRM 8.21). The signature comes from the PROTOTYPE, not slang's
// degenerate associated subroutine (null thisVar / returnValVar, empty args). It
// yields whatever an override that never assigns its return variable would (X, an
// empty string, an empty container — init_frame's per-call state) and is never
// called: it only supplies the CallVirtual / TaskCallVirtual FunctionType and
// result type for a dispatch resolved to the pure method on an abstract handle.
void Lowerer::build_pure_stub(const slang::ast::MethodPrototypeSymbol& proto, ir::FunctionId id) {
    const auto&             owner = proto.getParentScope()->asSymbol().as<slang::ast::ClassType>();
    std::vector<ir::Ref>    param_types;
    std::vector<ir::ArgDir> arg_dirs;
    std::vector<ir::Ref>    local_types;
    // `this` + each formal occupies a frame Local (index == param index), which
    // the backend's emit_copyin copies the parameter into — mirroring
    // build_function. The return Local follows.
    param_types.push_back(lower_class_type(owner)); // `this`
    arg_dirs.push_back(ir::ArgDir::In);
    local_types.push_back(param_types.back());
    for (const slang::ast::FormalArgumentSymbol* fa : proto.getArguments()) {
        const ir::Ref pt = lower_type(fa->getType());
        param_types.push_back(pt);
        arg_dirs.push_back(to_arg_dir(fa->direction));
        local_types.push_back(pt);
    }
    // A task carries no return value (LRM 13.3) — slang gives its prototype the
    // void type, so the void path below already emits the right body — but it is a
    // coroutine, and `is_task` on the Function record is what selects the task ABI
    // (state, wheel, <formals>, record) over the function one in declare_task.
    // This stub IS the FunctionType a TaskCallVirtual on an abstract handle drives
    // the concrete override's ramp through (lower_task_call_inst), so the flag has
    // to mirror build_task's or the call site and the override disagree on the
    // signature.
    const bool          is_task     = proto.subroutineKind == slang::ast::SubroutineKind::Task;
    const bool          is_void     = proto.getReturnType().isVoid();
    const ir::Ref       return_type = is_void ? module_.pool.void_type : lower_type(proto.getReturnType());
    ir::Builder         b(module_);
    const ir::SourceLoc el = to_source_loc(proto.location);
    if (is_void) {
        b.ret_void(el);
    } else {
        const bool     agg = is_agg_abi_type(proto.getReturnType());
        const uint32_t rl = static_cast<uint32_t>(local_types.size());
        local_types.push_back(return_type);
        const ir::Index rv = b.local(rl, return_type, el);
        // The Ret operand takes build_function's fall-through shape, so the stub's
        // signature is the one a real override produces: an aggregate's and a
        // container's result travels as its frame lvalue (the backend copies the
        // storage out through the trailing return out-param), while a scalar, a
        // string handle and a class handle travel as a loaded value.
        const bool by_lvalue = agg || is_handle_ref(return_type);
        b.unary(ir::Inst::Tag::Ret, by_lvalue ? rv : b.unary(ir::Inst::Tag::Load, rv, el), el);
    }
    module_.set_function(id,
                         ir::Function{ .name        = std::string(proto.name) + ".pure",
                                       .return_type = return_type,
                                       .param_types = std::move(param_types),
                                       .arg_dirs    = std::move(arg_dirs),
                                       .local_types = std::move(local_types),
                                       .is_task     = is_task,
                                       .span        = span_of(proto),
                                       .ir          = b.finish() });
}

// The slot map of `clt`, numbering the class first if nothing has yet.
//
// A function body is lowered on demand, at its first call, and such a body can
// reach a virtual call whose DECLARING class has not been through compute_vtable
// yet. A small design never does: every class it mentions is lowered before any
// body is. UVM's call graph is deep and mutually referential enough that the
// deferred path (`lower_user_call` -> `function_id` -> `build_function` ->
// `lower_user_call` -> ...) arrives at a virtual dispatch ahead of the target
// class, and the lookup found nothing -- an uncaught std::out_of_range with no
// source location, on `uvm_object::get_full_name` in a bare `uvm_pkg` elaboration.
//
// Numbering on demand mirrors how function_id lowers a callee on demand, and only
// the numbering is needed here: the call site wants a slot index, not a filled
// table.
const Lowerer::VtableSlots& Lowerer::ensure_vtable(const slang::ast::ClassType& clt) {
    const auto it = class_vslot_.find(&clt);
    if (it != class_vslot_.end()) {
        return it->second;
    }
    const auto cached = class_type_cache_.find(&clt);
    if (cached != class_type_cache_.end()) {
        // Interned but not yet numbered: its own lower_class_type frame is below
        // us on the stack, still walking properties (lowering a property's type
        // recurses into bodies). Number it here; compute_vtable is idempotent, so
        // that frame's own call becomes a no-op.
        //
        // This call bypasses lower_class_type entirely, so it needs the SAME
        // class_depth_ bookkeeping that function tracks around its own
        // compute_vtable call -- otherwise a randomize/randops FunctionId
        // compute_vtable reserves here (via rand_body_id) sits in rand_pending_
        // with no lower_class_type frame above us ever reaching class_depth_==1
        // to drain it: reserved but never module_.set_function'd, which
        // segfaults the backend on its Ref::Invalid return type the first time
        // codegen reaches it. Most calls here land at class_depth_ 0 (this runs
        // from body lowering, on demand, well after any lower_class_type
        // recursion that reached `clt` has already returned) -- this frame IS
        // then the outermost one and must drain itself; a genuinely nested call
        // (a class's own field layout virtual-dispatching into a sibling
        // method's vtable) leaves the draining to whichever frame is actually
        // at depth 1, exactly as it already does for lower_class_type's callers.
        const Restore<uint32_t> depth(class_depth_, class_depth_ + 1);
        compute_vtable(clt, cached->second);
        if (class_depth_ == 1) {
            drain_rand_bodies();
        }
    } else {
        lower_class_type(clt);
    }
    return class_vslot_.at(&clt);
}

// The vtable slot of virtual method `sub` (LRM 8.20), looked up by name in the
// slot map of its declaring class (walk out to the enclosing ClassType — a pure
// method's degenerate subroutine nests under its prototype scope). The slot is
// identical across the hierarchy, so any class containing the name works.
uint32_t Lowerer::virtual_slot_of(const slang::ast::SubroutineSymbol& sub) {
    return ensure_vtable(declaring_class(sub)).at(sub.name);
}

// The class scope that declares method `sub` (walk out past a pure method's
// prototype scope to the enclosing ClassType).
const slang::ast::ClassType& Lowerer::declaring_class(const slang::ast::SubroutineSymbol& sub) {
    const slang::ast::Scope* sc = sub.getParentScope();
    while (sc != nullptr && sc->asSymbol().kind != slang::ast::SymbolKind::ClassType) {
        sc = sc->asSymbol().getParentScope();
    }
    return sc->asSymbol().as<slang::ast::ClassType>();
}

// The FunctionId whose signature + return type describe a virtual dispatch to
// `sub` (the CallVirtual/TaskCallVirtual `func`). A concrete method is its own
// signature; a pure method's degenerate subroutine cannot be lowered, so its
// prototype stub stands in. This is sourced here (not the receiver's vtable) so
// it is available even while the receiver class's own vtable is mid-computation
// — a virtual method whose body virtual-calls a sibling method of the same class.
ir::FunctionId Lowerer::virtual_sig_id(const slang::ast::SubroutineSymbol& sub) {
    if (!sub.flags.has(slang::ast::MethodFlags::Pure)) {
        return function_id(sub);
    }
    const auto* proto = find_pure_proto(declaring_class(sub), sub.name);
    if (proto == nullptr) {
        fail("internal: pure virtual call without a prototype");
    }
    return pure_stub_id(*proto);
}

// True if `call` is a `super.method(...)` call (LRM 8.20): a super call binds
// STATICALLY to the base method, bypassing virtual dispatch. slang lowers it
// with a null `thisClass` (an implicit `this`), indistinguishable from a bare
// in-method call at the AST level, so detect it syntactically: the callee name
// is a scoped name whose left-most component is the `super` handle.
bool Lowerer::is_super_call(const slang::ast::CallExpression& call) {
    const slang::syntax::SyntaxNode* s = call.syntax;
    if (s == nullptr) {
        return false;
    }
    if (s->kind == slang::syntax::SyntaxKind::InvocationExpression) {
        s = s->as<slang::syntax::InvocationExpressionSyntax>().left;
    }
    while (s != nullptr && s->kind == slang::syntax::SyntaxKind::ScopedName) {
        s = s->as<slang::syntax::ScopedNameSyntax>().left;
    }
    return s != nullptr && s->kind == slang::syntax::SyntaxKind::SuperHandle;
}

// Lower a class `new` expression (LRM 8.7): allocate + power-on the object body
// (ClassNew yields the handle), then run the constructor with the fresh handle
// spliced as `this`. Yields the object handle (a 2-state 64-bit value). A
// default (implicit) constructor with no initializers has no constructorCall
// and only the power-on defaults apply.
ir::Index Lowerer::lower_new_class(const slang::ast::NewClassExpression& nc, ir::Builder& b) {
    const ir::SourceLoc l = loc(nc);
    // super.new is handled at the constructor's entry (build_function) and
    // no-op'd as a statement, so it never reaches here as an rvalue.
    const auto& clt = nc.type->getCanonicalType().as<slang::ast::ClassType>();
    if (clt.isAbstract) {
        // An abstract class cannot be instantiated (LRM 8.21). slang already
        // rejects this; guard for defense in depth (PRINCIPLES.md).
        fail("cannot instantiate an abstract (virtual) class");
    }
    const ir::Ref   class_ref = lower_type(*nc.type);
    const ir::Index handle    = b.class_new(class_ref, l);
    if (is_sync_class(clt)) {
        // `semaphore::new(int keyCount = 0)` is a BODYLESS built-in constructor
        // (LRM 15.3.1): emit_ctor_call would lower slang's empty prototype and
        // die on an out-of-range formal index, so the argument is stored into the
        // synthetic body here instead. The rest of the body powers on all-zero,
        // which is exactly an empty wait queue.
        const ir::Ref i64 = make_int_type(64, false);
        ir::Index     arg = b.constant(module_.pool.put_int(i64, 0), l);
        if (const Expression* cc = nc.constructorCall()) {
            const CallExpression& call = cc->as<CallExpression>();
            if (call.arguments().size() == 1) {
                const Expression& a = *call.arguments()[0];
                // coerce, not resize: the actual is a signed `int`, and a
                // negative bound / key count must sign-extend to be seen as one.
                arg = coerce(lower_expr(a, b), *a.type, i64, b, l);
            }
        }
        const bool is_mbx = clt.name == "mailbox";
        if (is_mbx) {
            // LRM 15.4.1: "The bound shall be positive. Negative bounds are
            // illegal and can result in indeterminate behavior, but
            // implementations can issue a warning." Clotho never silently
            // accepts, so it reports -- at run time, because the argument need
            // not be constant.
            const ir::Index bad = b.binary(ir::Inst::Tag::SLt, arg, b.constant(module_.pool.put_int(i64, 0), l), l);
            b.open_region();
            {
                const std::string banner = banner_at(nc.sourceRange.start());
                b.severity(static_cast<uint32_t>(ir::Severity::Fatal), severity_banner(nc.sourceRange.start()),
                           "mailbox bound is negative (LRM 15.4.1)", {}, l);
                b.finish(banner + "mailbox", 1, l);
            }
            const ir::ExtraSlice then_r = b.close_region();
            b.open_region();
            const ir::ExtraSlice else_r = b.close_region();
            b.cond(bad, then_r, else_r, l, /*unlikely=*/true);
        }
        b.binary(ir::Inst::Tag::Store,
                 b.member_select(b.handle_deref(handle, class_ref, l),
                                 is_mbx ? kMbxBound : kSemKeys, class_ref, l),
                 arg, l);
        return handle;
    }
    emit_object_rng_seed(clt, handle, class_ref, b, l);
    // Non-static property default initializers run at construction, in
    // declaration order, BEFORE any constructor body (LRM 8.8). slang attaches
    // them to the property (not the constructor body); apply the whole
    // inheritance chain's defaults here, base-first, up front. Constructor
    // bodies then run via emit_ctor_call.
    emit_class_property_inits(clt, handle, class_ref, b);
    const SubroutineSymbol*            ctor = nullptr;
    std::span<const Expression* const> actuals;
    if (const Expression* cc = nc.constructorCall()) {
        const CallExpression& call = cc->as<CallExpression>();
        ctor                       = std::get<0>(call.subroutine);
        actuals                    = call.arguments();
    }
    emit_ctor_call(clt, ctor, actuals, handle, b, l);
    return handle;
}

// Object stability (LRM 18.14.2): an object's RNG is seeded, when the object is
// created, from the next random value of the thread that creates it. The draw is
// the ordinary `$urandom` one, so no analysis changes: a process that constructs
// a randomizable object is already, by that instruction, a process that draws —
// it materializes its own frame state and leaves the comb-thunk path.
//
// Unconditional for a rand-bearing class, whether or not the object is ever
// randomized: a draw that appeared only on first use would shift every later
// seed in the creating thread the moment a randomize() call was added, which is
// the instability LRM 18.14 exists to prevent.
void Lowerer::emit_object_rng_seed(const slang::ast::ClassType& clt,
                                   ir::Index                    handle,
                                   ir::Ref                      class_ref,
                                   ir::Builder&                 b,
                                   ir::SourceLoc                l) {
    const ClassRand& rd = class_rand_.at(&clt);
    if (!rd.present) {
        return;
    }
    const ir::Ref   u64  = make_int_type(64, false);
    const ir::Index body = b.handle_deref(handle, class_ref, l);
    const ir::Index rng  = b.member_select(body, rd.rng_field, class_ref, l);
    const ir::Index seed = b.convert(b.urandom(l), u64, false, l);
    b.obj_rand_seed(rng, seed, l);
    // Every rand property starts enabled (LRM 18.8).
    const ir::Index mode = b.member_select(body, rd.mode_field, class_ref, l);
    const ir::Index all  = b.constant(module_.pool.put_int(u64, ~0ull), l);
    b.binary(ir::Inst::Tag::Store, mode, all, l);
}

// Lower a class shallow copy (LRM 8.12): `dst = new src`. Allocates a second
// object and copies every non-static property of `src` into it member-wise. Per
// 8.12's three steps the constructor does NOT run and neither do declaration
// initializers, which is the whole difference from lower_new_class above.
//
// What each property does is already decided by its type: emit_class_body_copy
// routes through emit_agg_copy, so a string / queue / dynamic array /
// associative array property (a value type that owns a heap buffer) deep-copies
// and a class-handle property copies as a handle -- 8.12's "objects, however,
// are not copied, only their handles".
//
// The object allocated must be of the SOURCE's dynamic type, not of the
// expression's static type: `new b` where `b` is a base-typed handle holding a
// derived object copies the derived object, which is observable through a
// virtual call on the copy and through a later `$cast` of it. So the copy
// dispatches through the source's own descriptor to its class's synthesized
// copy thunk, which allocates exactly its own class.
ir::Index Lowerer::lower_copy_class(const slang::ast::CopyClassExpression& cc, ir::Builder& b) {
    const ir::SourceLoc  l         = loc(cc);
    const auto&          clt       = cc.type->getCanonicalType().as<slang::ast::ClassType>();
    const ir::Ref        class_ref = lower_type(*cc.type);
    const ir::Index      src       = lower_expr(cc.sourceExpr(), b);
    const ir::FunctionId sig       = copy_thunk_id(clt, class_ref);
    const uint32_t       slot      = ensure_vtable(clt).at(kCopySlot);
    return b.call_virtual(sig, slot, class_ref, std::array<ir::Index, 1>{ src }, l);
}

// Copy every property of object `src` into object `dst`, both of class
// `class_ref` (LRM 8.12 step 2). lower_class_type flattens the inheritance chain
// into one field table with the base's properties as a prefix, so this single
// flat loop covers inherited and own properties in body order; static properties
// are not in the body (they are shared, LRM 8.9) and so are correctly untouched.
void Lowerer::emit_class_body_copy(ir::Index dst, ir::Index src, ir::Ref class_ref, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index dst_body = b.handle_deref(dst, class_ref, l);
    const ir::Index src_body = b.handle_deref(src, class_ref, l);
    const ir::Type  t        = std::get<ir::Type>(module_.pool.get(class_ref));
    const auto      n        = static_cast<uint32_t>(module_.pool.struct_fields(t.fields_id).size());
    // A change notifier is NOT part of the copied value (LRM 9.4.2 gives it no
    // source-level existence at all). Copying the source's handle over the
    // destination's would alias one waiter list into two objects AND orphan every
    // waiter already parked on the destination's own notifier -- a silent hang for
    // a process that was correctly waiting. Skipped by name, the one thing that
    // marks these rows apart from properties.
    const auto is_notifier = [&](uint32_t i) {
        return module_.pool.field_name(module_.pool.struct_fields(t.fields_id)[i]).starts_with("$chg.");
    };
    for (uint32_t i = 0; i < n; ++i) {
        if (is_notifier(i)) {
            continue;
        }
        // Re-read the row each iteration: emit_agg_copy can intern a type and
        // reallocate the field table underneath a held span (lower/Assign.cpp's
        // emit_agg_store documents the same hazard).
        const ir::Ref   field_ty  = module_.pool.struct_fields(t.fields_id)[i].type;
        const ir::Index dst_field = b.member_select(dst_body, i, class_ref, l);
        const ir::Index src_field = b.member_select(src_body, i, class_ref, l);
        emit_agg_copy(dst_field, src_field, field_ty, ir::Inst::Tag::Store, b, l);
    }
}

// The FunctionId of class `clt`'s shallow-copy thunk: `(src handle) -> handle`,
// allocating and filling an object of exactly `clt`. One per polymorphic class,
// installed in its vtable's reserved kCopySlot by compute_vtable, so `new src`
// through a base-typed handle reaches the concrete class's thunk and allocates
// the concrete size. Cached per class.
ir::FunctionId Lowerer::copy_thunk_id(const slang::ast::ClassType& clt, ir::Ref class_ref) {
    const auto it = copy_thunk_.find(&clt);
    if (it != copy_thunk_.end()) {
        return it->second;
    }
    const ir::FunctionId id = module_.reserve_function(class_ref);
    copy_thunk_[&clt]       = id;
    build_copy_thunk(clt, class_ref, id);
    return id;
}

// Build the copy thunk body. Takes the source handle as its one parameter (the
// receiver slot every vtable dispatch passes, so the thunk's LLVM signature is
// the same `(i64) -> i64` in every class of the hierarchy) and returns the new
// object's handle. An abstract class gets one too: its vtable is never
// materialized because it is never instantiated, so the body is unreachable, and
// filling the slot uniformly keeps the numbering identical across the hierarchy.
void Lowerer::build_copy_thunk(const slang::ast::ClassType& clt, ir::Ref class_ref, ir::FunctionId id) {
    // The parameter occupies frame Local 0, as in build_pure_stub / build_function.
    std::vector<ir::Ref>    param_types{ class_ref };
    std::vector<ir::ArgDir> arg_dirs{ ir::ArgDir::In };
    std::vector<ir::Ref>    local_types{ class_ref };
    ir::Builder             b(module_);
    const ir::SourceLoc     el  = to_source_loc(clt.location);
    const ir::Index         src = b.unary(ir::Inst::Tag::Load, b.local(0, class_ref, el), el);
    const ir::Index         dst = b.class_new(class_ref, el);
    emit_class_body_copy(dst, src, class_ref, b, el);
    b.unary(ir::Inst::Tag::Ret, dst, el);
    module_.set_function(id,
                         ir::Function{ .name        = std::string(clt.name) + ".copy",
                                       .return_type = class_ref,
                                       .param_types = std::move(param_types),
                                       .arg_dirs    = std::move(arg_dirs),
                                       .local_types = std::move(local_types),
                                       .span        = span_of(clt),
                                       .ir          = b.finish() });
}

// Run class `clt`'s constructor `ctor` (may be null) against object `handle`
// with the given actual argument expressions (LRM 8.7). A non-null ctor is
// called directly; its OWN base construction is emitted at its entry (implicit)
// or by its body's `super.new` — not here. A null ctor (the class declares no
// `new`) still constructs the class's base, since a ctor-less class contributes
// its base construction plus its property defaults (the latter already applied
// at allocation). Mutually recursive with emit_base_construction.
void Lowerer::emit_ctor_call(const slang::ast::ClassType&       clt,
                             const SubroutineSymbol*            ctor,
                             std::span<const Expression* const> actuals,
                             ir::Index                          handle,
                             ir::Builder&                       b,
                             ir::SourceLoc                      l) {
    if (ctor == nullptr) {
        // No constructor: only the base part needs constructing. A ctor-less
        // class cannot take constructor arguments.
        emit_base_construction(clt, handle, b, l);
        return;
    }
    const ir::FunctionId id      = function_id(*ctor); // splices `this` as formal 0
    const auto           formals = ctor->getArguments();
    if (actuals.size() > formals.size()) {
        fail("constructor argument count mismatch");
    }
    // A class that declares no `new` constructs its base with an implicit
    // `super.new()` and therefore supplies no actuals (LRM 8.7 / 8.15) -- but the
    // base constructor may still declare formals, every one of which must then
    // have a default for that to be legal (13.5.3). slang materializes defaults
    // into the argument list of a real CallExpression, which is why a written
    // `super.new()` or `new()` needs nothing here; this call is synthesized by
    // emit_base_construction, so its defaults are filled in below.
    //
    // `uvm_callbacks_base extends uvm_object` is the shape: no `new` of its own,
    // and uvm_object's takes `string name=""`.
    std::vector<const Expression*> filled(actuals.begin(), actuals.end());
    for (size_t k = actuals.size(); k < formals.size(); k++) {
        const Expression* def = formals[k]->getDefaultValue();
        if (def == nullptr) {
            fail("constructor argument count mismatch");
        }
        filled.push_back(def);
    }
    std::vector<CopyOutArg> outs;
    // A defaulted constructor argument evaluates against the object under
    // construction (LRM 8.7 + 13.5.3): its implicit `this` is `handle`, typed
    // as the class DECLARING the constructor — a base ctor's default sees the
    // base class, not the most-derived one being built.
    std::vector<ir::Index> args = build_call_args(
      formals, filled, outs, b, l,
      CtorThis{ .handle = handle, .cls = lower_type(clt) });
    args.insert(args.begin(), handle);
    b.call(id, args, l); // the constructor is void
    emit_call_copyouts(outs, b, l);
}

// Construct the base-class part of an object during class `clt`'s construction
// (LRM 8.15), for the IMPLICIT / `extends B(args)` cases — an explicit
// `super.new` handles itself via the statement path. Runs the base constructor
// against the same `handle`. Emitted either at a constructor's entry (where an
// `extends`-arg referencing a ctor formal is in scope) or from emit_ctor_call
// when recursing through a ctor-less class (whose base args are context-free).
void Lowerer::emit_base_construction(const slang::ast::ClassType& clt,
                                     ir::Index                    handle,
                                     ir::Builder&                 b,
                                     ir::SourceLoc                l) {
    const slang::ast::Type* base = clt.getBaseClass();
    if (base == nullptr) {
        return;
    }
    const auto&       base_clt  = base->getCanonicalType().as<slang::ast::ClassType>();
    const Expression* base_call = clt.getBaseConstructorCall();
    // Both an explicit `super.new(args)` (a NewClassExpression) and an
    // `extends B(args)` clause (a CallExpression) carry the base constructor +
    // actual args; extract the underlying call uniformly. The args may
    // reference this constructor's formals (in scope at its entry).
    if (base_call != nullptr) {
        const CallExpression* cc = nullptr;
        if (base_call->kind == ExpressionKind::NewClass) {
            if (const Expression* inner = base_call->as<slang::ast::NewClassExpression>().constructorCall()) {
                cc = &inner->as<CallExpression>();
            }
        } else if (base_call->kind == ExpressionKind::Call) {
            cc = &base_call->as<CallExpression>();
        }
        if (cc != nullptr) {
            emit_ctor_call(base_clt, std::get<0>(cc->subroutine), cc->arguments(), handle, b, l);
            return;
        }
        // `super.new()` to a ctor-less base: fall through to no-arg construction.
    }
    // Implicit no-arg base construction (base's own defaults applied already).
    emit_ctor_call(base_clt, base_clt.getConstructor(), {}, handle, b, l);
}

// Apply a class's non-static property default initializers to a freshly-new'd
// object `handle` (LRM 8.8): a store per property that has an initializer, in
// declaration order. Lowered at the `new` site rather than inside a constructor
// body, so `ctor_this_` carries the object under construction for the duration:
// LRM 8.7 runs these initializers as part of construction, which makes a bare
// `f()` here an ordinary call on `this` (resolve_method_this). A bare reference
// to ANOTHER property is still a documented V1 gap, failing loudly via signal_of.
void Lowerer::emit_class_property_inits(const slang::ast::ClassType& clt,
                                        ir::Index                    handle,
                                        ir::Ref                      class_ref,
                                        ir::Builder&                 b) {
    // Re-installed identically by the base-class recursion below, and restored on
    // the throwing path, so a nested `new` in an initializer (whose own inits run
    // against ITS object) cannot leak the wrong handle back out.
    const Restore<std::optional<CtorThis>> ctor(ctor_this_, CtorThis{ .handle = handle, .cls = class_ref });
    // Base-class defaults first (LRM 8.8 / 8.13 declaration order runs the base
    // part before the derived part). `class_ref` stays the *derived* (allocated)
    // type so class_member_lvalue derefs the full body; an inherited property's
    // prefix index (class_prop_index_) then addresses the right slot.
    if (const slang::ast::Type* base = clt.getBaseClass()) {
        emit_class_property_inits(base->getCanonicalType().as<slang::ast::ClassType>(), handle, class_ref, b);
    }
    for (const auto& prop : clt.membersOfType<slang::ast::ClassPropertySymbol>()) {
        // A static property is not per-object: it has one shared slot,
        // initialized once at time zero in PreInit (declare_static_prop), not on
        // every `new`.
        if (prop.lifetime == slang::ast::VariableLifetime::Static) {
            continue;
        }
        const Expression* init = prop.getInitializer();
        if (init == nullptr) {
            continue;
        }
        const ir::SourceLoc il = loc(*init);
        const ir::Index     lv = class_member_lvalue(handle, prop, class_ref, b, il);
        if (is_aggregate(prop.getType()) || is_unpacked_container(prop.getType())) {
            // An unpacked struct / fixed array / queue / dynamic array /
            // associative array property: the same element-wise, handle-
            // aware store a module-level declaration initializer takes
            // (lower_var_initializer) and a static property's shared slot
            // takes (declare_static_prop) -- LRM 8.7 runs a property
            // initializer as part of construction, the same declaration-
            // initializer semantics as those two, just per-object rather
            // than per-frame or once. A plain Store (the two arms below)
            // would bit-blast the aggregate's storage, or the container's
            // {ptr,len}-shaped handle, instead of building it.
            emit_struct_store(lv, lower_type(prop.getType()), *init, ir::Inst::Tag::Store, b, il);
        } else if (prop.getType().getCanonicalType().isString()) {
            b.binary(ir::Inst::Tag::Store, lv, lower_expr(*init, b), il);
        } else {
            b.binary(ir::Inst::Tag::Store,
                     lv,
                     coerce(lower_expr(*init, b), *init->type, lower_type(prop.getType()), b, il),
                     il);
        }
    }
}

} // namespace clotho::lower
