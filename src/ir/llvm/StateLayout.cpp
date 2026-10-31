// Design state layout: scheduling flags, signal slots, scope and instance structs, context access.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Per-process wake word, one i64 per process. Bit 0 is the "scheduled"
// flag: non-zero while the process is already queued to run this instant,
// which is how a persistent (always-block) wait entry avoids being woken
// more than once per instant when several of its sensitivity signals change
// together. Bits 1..63 are TRIGGER bits: a wake site ORs in the colour of
// the (signal, edge) it fired for (compute_trigger_masks), so a fused
// group's body can tell which of its members' own sensitivities actually
// fired and skip the rest. A site that cannot name its trigger sets every
// bit, so an unclassified wake runs every member exactly as before.
//
// The whole word is tested against zero, never against one, and cleared to
// zero to re-arm -- so the queued/not-queued discipline is unchanged by the
// trigger bits riding above it.
//
// Words live in one array per owning instance scope
// (@clotho.inst.<hier>.sched, indexed by a dense def-local ordinal -- twin
// instances lower identical ordinals) plus a root @clotho.proc_sched for
// unowned (package / `$unit`) processes.
void Backend::build_sched_flags() {
    const auto procs = mod_.processes();
    auto*      word  = llvm::Type::getInt64Ty(ctx_);
    proc_sched_anchor_.assign(std::max<size_t>(procs.size(), 1), ~0u);
    proc_sched_ord_.assign(std::max<size_t>(procs.size(), 1), 0);
    std::vector<uint32_t> owned(anchors_.size(), 0);
    uint32_t              n_root = 0;
    for (uint32_t k = 0; k < procs.size(); ++k) {
        if (const uint32_t aid = process_anchor(procs[k]); aid != SignalSlot::no_anchor) {
            proc_sched_anchor_[k] = aid;
            proc_sched_ord_[k]    = owned[aid]++;
        } else {
            proc_sched_ord_[k] = n_root++;
        }
    }
    for (uint32_t a = 0; a < anchors_.size(); ++a) {
        if (owned[a] == 0) {
            continue;
        }
        Anchor& an  = anchors_[a];
        an.sched_ty = llvm::ArrayType::get(word, owned[a]);
        an.sched_gv = new llvm::GlobalVariable(env_->llmod, an.sched_ty, false,
                                               llvm::GlobalValue::InternalLinkage,
                                               llvm::ConstantAggregateZero::get(an.sched_ty),
                                               "clotho.inst." + an.name + ".sched");
    }
    sched_arr_ty_ = llvm::ArrayType::get(word, std::max<uint32_t>(n_root, 1));
    sched_gv_     = new llvm::GlobalVariable(env_->llmod, sched_arr_ty_, false, llvm::GlobalValue::InternalLinkage,
                                             llvm::ConstantAggregateZero::get(sched_arr_ty_), "clotho.proc_sched");
}

// The anchor of the instance scope that owns process `p`: no_anchor for a
// process of no instance (PreInit / package) or of a scope without one.
uint32_t Backend::process_anchor(const Process& p) const {
    return p.scope != Process::no_scope && p.scope < anchor_of_scope_.size() ? anchor_of_scope_[p.scope]
                                                                              : SignalSlot::no_anchor;
}

// The LLVM type of a wake word. One place, because materialize_group_ctx
// uses it as a GEP STRIDE: get it wrong there and a deduped instance
// addresses another process's word.
llvm::IntegerType* Backend::sched_word_ty() {
    return llvm::Type::getInt64Ty(ctx_);
}

// Bits a wake site sets when it cannot name the (signal, edge) it fired
// for: every trigger bit, so every guard in the woken group passes. Always
// every bit, whether or not the design guards a group: that is a fact about
// the whole design, and unit code (a park's claim of its own flag) must not
// change when another unit gains the first guarded group. Bit 0 stays the
// only one anything else reads, so the extra bits cost an immediate.
uint64_t Backend::unclassified_trig_bits() const {
    return ~1ull;
}

// Trigger bits of member `i` of group `g`: the bits of its OWN events. Zero
// for an unguarded group, which is what makes open_guard a no-op there.
uint64_t Backend::member_mask(uint32_t g, size_t i) const {
    return g < group_member_mask_.size() && i < group_member_mask_[g].size() ? group_member_mask_[g][i] : 0;
}

// Constant pointer to process k's scheduled flag in its owner's array.
llvm::Constant* Backend::sched_flag_ptr(uint32_t k) {
    const uint32_t aid  = proc_sched_anchor_[k];
    auto*          base = aid != ~0u ? sym(anchors_[aid].sched_gv) : sym(sched_gv_);
    auto*          ty   = aid != ~0u ? anchors_[aid].sched_ty : sched_arr_ty_;
    return llvm::ConstantExpr::getInBoundsGetElementPtr(
      ty, base, llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(proc_sched_ord_[k]) });
}

// The `{ i(2W) must|may, iW z }` slot struct for a 4-state value of the given
// width -- just `{ i(2W) }` when the slot needs no z word (a proven never-z
// signal; frame locals always keep z) -- or the native `{ iW }` for a 2-state
// one. Identified and memoized per (width, domain, has_z) so every signal of
// one shape shares one named LLVM type (%logic.8, %logic.nz.4, %bit.4, ...); the
// single-field form keeps the struct wrapper so GEP paths and array-of-slot
// addressing stay uniform.
llvm::StructType* Backend::slot_type(uint32_t width, bool four_state, bool has_z) {
    assert((four_state || !has_z) && "a 2-state slot has no z word");
    for (const auto& [w, four, z, ty] : slot_types_) {
        if (w == width && four == four_state && z == has_z) {
            return ty;
        }
    }
    const std::string name =
      std::string(four_state ? (has_z ? "logic." : "logic.nz.") : "bit.") + std::to_string(width);
    std::vector<llvm::Type*> fields{ int_ty(four_state ? 2 * width : width) };
    if (has_z) {
        fields.push_back(int_ty(width));
    }
    auto* ty = llvm::StructType::create(ctx_, fields, name);
    slot_types_.emplace_back(width, four_state, has_z, ty);
    return ty;
}

// The opaque rt_string handle slot type
// (`%clotho.string = { i64, [sizeof(rt_string) - 8 x i8] }`), memoized. A struct
// wrapper keeps GEP paths and array-of-slot addressing uniform with the plane
// slots.
//
// The leading i64 is what carries the runtime type's alignment: rt_string is
// `alignas(8)`, and a plain `{ [32 x i8] }` mirror is align 1, so a string leaf
// inside an aggregate landed at whatever offset the preceding field left it (an
// `int` puts it at +4) and every runtime access to it was misaligned -- UB that
// UBSan reports on a struct-with-string signal and on a container element alike.
// The trailing bytes stay a byte array so the empty-string image (whose only
// non-zero byte is the inline capacity, at the end) is expressible without
// endianness games.
llvm::StructType* Backend::str_slot_ty() {
    if (str_slot_ty_ == nullptr) {
        static_assert(sizeof(rt_string) > 8 && sizeof(rt_string) % 8 == 0);
        str_slot_ty_ = llvm::StructType::create(
          ctx_, { b_.getInt64Ty(), llvm::ArrayType::get(b_.getInt8Ty(), sizeof(rt_string) - 8) }, "clotho.string");
    }
    return str_slot_ty_;
}

// The empty (owned, inline) rt_string image as a typed constant: all zero bytes
// but the trailing inline-capacity byte.
llvm::Constant* Backend::str_empty_const() {
    std::vector<uint8_t> bytes(sizeof(rt_string) - 8, 0);
    bytes[bytes.size() - 1] = static_cast<uint8_t>(rt_string_inline_cap);
    return llvm::ConstantStruct::get(str_slot_ty(), { b_.getInt64(0), llvm::ConstantDataArray::get(ctx_, bytes) });
}

// Field index of the relocation epoch inside each container's handle slot
// (docs/design/data-model.md §3.1). A `ref` formed through a place rooted in
// one of these carries `&handle[epoch_field]` so the callee can detect a
// restructuring that happened while the ref was live.
uint32_t Backend::handle_epoch_field(Ref type) const {
    if (is_assoc_type(type)) {
        return 1; // { impl, epoch }
    }
    return is_queue_type(type) ? 4u  // { base, head, len, cap, epoch }
                               : 2u; // { data, len, epoch }
}

// Whether a type is a dynamic array (LRM 7.5).
bool Backend::is_dynarr_type(Ref type) const {
    return types::is_dynarr(mod_.pool, type);
}

// Whether a type is an associative array (LRM 7.8/7.9).
bool Backend::is_assoc_type(Ref type) const {
    return types::is_assoc(mod_.pool, type);
}

// Build one instance scope's state struct: the slots of every signal the
// instance defines (its unit's locals, or the scope's signals for a module
// built without units), in that order. Children are NOT nested as fields —
// each scope owns its own state global, so a child scope recurses into its own
// anchor and struct. Assigns every covered signal's scope-relative GEP path
// ({k}). Struct types are deduplicated per module definition: the instances of
// one unit produce identical field lists (join_twin_slots) and share one
// identified type. Each call is one scope occurrence and records one Anchor;
// storage_ptr addresses signals anchor-relative.
void Backend::scope_type(const InstanceScope& scope, std::vector<bool>& claimed, const std::string& hier) {
    const auto aid = static_cast<uint32_t>(anchors_.size());
    anchors_.push_back(Anchor{ .name = hier, .unit = scope.unit });
    if (scope.id >= anchor_of_scope_.size()) {
        anchor_of_scope_.resize(scope.id + 1, SignalSlot::no_anchor);
    }
    anchor_of_scope_[scope.id] = aid;
    for (const Signal e : scope.externs) {
        if (e == InstanceScope::no_extern) {
            continue;
        }
        Anchor& an = anchors_[aid];
        // Two declarations can name one signal in one instance (an inout port
        // and the contribution a child drives through it, when the child is the
        // net's only driver). A flat signal says nothing of which declaration the
        // code named, so the first stands for all of them: code is shared
        // between instances whose declarations alias alike (code_key).
        an.bind_of.try_emplace(static_cast<uint32_t>(e), static_cast<uint32_t>(an.externs.size()));
        an.externs.push_back(e);
    }
    // The functions the instance defines, and the ones its declarations import.
    // As with signals, two declarations can name one function; the first stands
    // for all of them.
    {
        Anchor& an    = anchors_[aid];
        an.fn_locals  = scope.fn_locals;
        an.fn_externs = scope.fn_externs;
        for (uint32_t k = 0; k < an.fn_externs.size(); ++k) {
            an.fn_bind_of.try_emplace(static_cast<uint32_t>(an.fn_externs[k]), k);
        }
    }
    const std::vector<Signal>& defined = scope.unit == InstanceScope::no_unit ? scope.signals : scope.locals;
    std::vector<llvm::Type*>   fields;
    for (uint32_t k = 0; k < defined.size(); ++k) {
        const auto si = static_cast<uint32_t>(defined[k]);
        if (si >= slots_.size() || claimed[si]) {
            throw BackendError{ "malformed instance hierarchy: signal claimed twice" };
        }
        claimed[si] = true;
        slots_[si].path   = { k };
        slots_[si].anchor = aid;
        fields.push_back(slots_[si].storage_ty);
    }
    // Waiter-list heads for members some `@(edge vif.sig)` parks on (LRM 25.9).
    // They live in the definition's own state struct, so a waiter reaches the
    // head exactly the way it reaches the member: the instance base the handle
    // gave plus a def-local field index. One head per (member, edge) actually
    // waited on, shaped like an event object so the intrusive list primitives
    // serve both without a second engine.
    anchors_[aid].vif_head_base = static_cast<uint32_t>(fields.size());
    for (const Signal sig : defined) {
        const auto si = static_cast<uint32_t>(sig);
        for (uint32_t e = 0; e < 3; ++e) {
            if (vif_wait_[si][e]) {
                vif_head_ord_[si][e] = static_cast<uint32_t>(fields.size()) -
                                       anchors_[aid].vif_head_base;
                fields.push_back(event_obj_ty());
            }
        }
    }
    for (const InstanceScope& child : scope.children) {
        scope_type(child, claimed, hier + "." + child.name);
    }

    for (const auto& [def, flds, ty] : def_types_) {
        if (def == scope.def_name && flds == fields) {
            anchors_[aid].ty = ty;
            return;
        }
    }
    auto* ty = llvm::StructType::create(ctx_, fields, scope.def_name + ".state");
    def_types_.emplace_back(scope.def_name, fields, ty);
    anchors_[aid].ty = ty;
}

// Group the instances of every unit with more than one, and list for each of
// the unit's signals the flat signal every instance holds for it.
void Backend::collect_unit_twins() {
    ankerl::unordered_dense::map<uint32_t, std::vector<const InstanceScope*>> by_unit;
    std::vector<const InstanceScope*>                                         work;
    for (const InstanceScope& top : mod_.tops()) {
        work.push_back(&top);
    }
    while (!work.empty()) {
        const InstanceScope* s = work.back();
        work.pop_back();
        if (s->unit != InstanceScope::no_unit) {
            by_unit[s->unit].push_back(s);
        }
        for (const InstanceScope& c : s->children) {
            work.push_back(&c);
        }
    }
    unit_twins_.clear();
    for (const auto& [unit, scopes] : by_unit) {
        if (scopes.size() < 2) {
            continue;
        }
        const size_t n = scopes.front()->locals.size();
        for (size_t k = 0; k < n; ++k) {
            std::vector<Signal>& set = unit_twins_.emplace_back();
            for (const InstanceScope* s : scopes) {
                assert(s->locals.size() == n && "instances of one unit define different signals");
                set.push_back(s->locals[k]);
            }
        }
    }
}

// Give every instance of a unit the same slot shape for each of its signals.
// z storage is a may-property, so the join is an OR: an instance whose writers
// cannot produce z pays the z word its twin needs. A canonical shape (a `ref`
// actual, a member a virtual interface reaches) is a must, and the slot loop
// already gives it the z word of a 4-state signal, so the OR imposes it on all
// the instances. A 2-state signal is the native word in every one. Power-on
// values (init_z, undriven_mask) stay per instance: they are initializer data,
// not layout.
void Backend::join_twin_slots() {
    for (const std::vector<Signal>& set : unit_twins_) {
        bool has_z = false;
        for (const Signal sig : set) {
            has_z = has_z || slots_[static_cast<uint32_t>(sig)].has_z;
        }
        for (const Signal sig : set) {
            SignalSlot& s = slots_[static_cast<uint32_t>(sig)];
            if (s.slot_ty == nullptr || s.is_string || s.is_agg || s.is_dynarr || s.is_queue || s.is_assoc) {
                continue;
            }
            s.has_z      = s.four_state && has_z;
            s.slot_ty    = slot_type(s.width, s.four_state, s.has_z);
            s.storage_ty = s.is_array ? static_cast<llvm::Type*>(llvm::ArrayType::get(s.slot_ty, s.elem_count))
                                      : s.slot_ty;
        }
    }
}

// Build the design's state types: per-signal slot / storage types, one
// struct per module definition (own signals only — every instance scope
// owns a separate state global, children own theirs), and the root
// `%clotho.state` = { <unscoped signals...> }. Unscoped signals only occur
// for a module built without hierarchy info (they form one flat root
// scope). The NBA shadow region is a separate root-level global, not part
// of any definition struct, because staging is a backend artifact of which
// signals the design nonblocking-assigns, not part of a module's declared
// state.
void Backend::build_state_types() {
    const auto sigs = mod_.signals();
    slots_.resize(sigs.size());
    collect_unit_twins();
    // Before classification: which members a handle nonblocking-writes decides
    // their staging path, and every writer of such a member must agree on it.
    collect_vif_nba();
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        slots_[i].vif_rec = vif_nba_[i];
    }
    // Classify NBA targets up front: which signals nonblocking-assign decides
    // their shadow and commit layout.
    nba_classify();

    // A slot keeps its z word when the may-z analysis cannot prove it z-free
    // or a pointer ABI (out / inout / ref) needs z storage behind it. The
    // analysis owns every source of z, a constant table's image included
    // (ir/Analysis.cpp).
    const std::vector<bool> may_z =
      config_.z_elision ? facts_.may_z : std::vector<bool>(sigs.size(), true);
    const std::vector<bool>& ptr_arg = facts_.ptr_arg;
    // Semantics, not optimization: computed regardless of config_.z_elision.
    // An undriven 4-state net powers up z (LRM 6.6); everything else x / 0. A
    // partially-driven packed net powers its undriven bits z per this mask.
    const std::vector<bool>& undriven = facts_.undriven;
    compute_vif_reachable();
    collect_vif_waits();

    for (uint32_t i = 0; i < sigs.size(); ++i) {
        const Layout lay = mod_.pool.layout(sigs[i].type);
        const Type&  ty  = std::get<Type>(mod_.pool.get(sigs[i].type));

        SignalSlot& slot = slots_[i];
        slot.ir_type     = sigs[i].type;
        slot.width       = lay.width == 0 ? 1 : lay.width;
        slot.four_state  = lay.domain == Domain::Four;
        // A string slot is an opaque rt_string handle: no planes, no z word.
        if (is_string_type(sigs[i].type)) {
            slot.is_string = true;
            slot.has_z     = false;
            slot.slot_ty   = str_slot_ty();
            if (ty.kind == Type::Kind::UnpackedArray) {
                slot.is_array   = true;
                slot.elem_count = array_leaf_count(sigs[i].type);
                slot.storage_ty = llvm::ArrayType::get(slot.slot_ty, slot.elem_count);
            } else {
                slot.storage_ty = slot.slot_ty;
            }
            continue;
        }
        // A signal whose type contains an unpacked struct (at any nesting),
        // or a fixed array over a queue / dynamic-array element, is an
        // aggregate: a recursive storage mirror (nested StructType /
        // ArrayType / leaf slots), addressed by a composed GEP path. Its
        // scalar/string leaves reachable by a static (struct-field-only) path
        // are enumerated for nonblocking staging.
        //
        // A `ref` actual of plain array type deliberately does NOT switch models:
        // it keeps flat storage and only its ELEMENT SHAPE is canonicalized
        // below. That is sufficient, because a flat `[N x slot]` over canonical
        // element slots is byte-identical to the canonical mirror (row-major,
        // same leaf slot type) — only the GEP shape differs, and both sides
        // compute the same address. Switching models here instead would leave a
        // ref-actual array and an ordinary one of the SAME type with different
        // storage, which a whole-array copy between the two cannot express.
        if (needs_agg_storage(sigs[i].type) ||
            (sigs[i].force_agg && is_agg_abi(sigs[i].type))) {
            slot.is_agg     = true;
            slot.has_z      = false;
            slot.storage_ty = agg_storage_ty(sigs[i].type);
            std::vector<uint32_t> path;
            collect_static_leaves(sigs[i].type, path, slot.static_leaves);
            continue;
        }
        // A dynamic-array slot is an opaque rt_dynarr handle ({ data, len }):
        // no planes, no z word; elements live in its heap buffer.
        if (ty.kind == Type::Kind::DynArray) {
            slot.is_dynarr  = true;
            slot.has_z      = false;
            slot.slot_ty    = dynarr_slot_ty();
            slot.storage_ty = slot.slot_ty;
            continue;
        }
        // A queue slot is an opaque rt_queue ring handle ({ base, head, len,
        // cap }): no planes, no z word; elements live in its heap ring buffer.
        if (ty.kind == Type::Kind::Queue) {
            slot.is_queue   = true;
            slot.has_z      = false;
            slot.slot_ty    = queue_slot_ty();
            slot.storage_ty = slot.slot_ty;
            continue;
        }
        // An associative-array slot is an opaque rt_assoc handle ({ impl }):
        // no planes, no z word; entries live in its heap map.
        if (ty.kind == Type::Kind::Assoc) {
            slot.is_assoc   = true;
            slot.has_z      = false;
            slot.slot_ty    = assoc_slot_ty();
            slot.storage_ty = slot.slot_ty;
            continue;
        }
        // A real / shortreal slot stores the IEEE-754 bit pattern in a
        // 2-state word: never z, so it takes the native single-iW form
        // below (pool layout gives Domain::Two for floats).
        slot.is_real     = mod_.pool.is_float(leaf_type(sigs[i].type));
        slot.init_z      = undriven[i] && sigs[i].is_net && slot.four_state;
        // A partially-driven net: its undriven bits power on z. Only set for a
        // 4-state net of packed elements (the analysis's candidate set matches);
        // an unpacked array's mask spans every element. Fully-undriven takes
        // init_z; fully-driven has no mask.
        const std::span<const uint64_t> pmask = facts_.undriven_mask(static_cast<Signal>(i));
        if (!slot.init_z && slot.four_state && !pmask.empty()) {
            slot.undriven_mask.assign(pmask.begin(), pmask.end());
        }
        // A signal is the slot its domain gives it. A 2-state signal is the native
        // iW (or `[N x { iW }]` memory): it carries no z, whatever the may-z
        // analysis says of the values written to it, so only a 4-state signal has a
        // z word to elide. A 4-state signal keeps it unless the analysis proves it
        // never z -- and always for an actual a callee reaches through a pointer
        // (a `ref` actual, or the out / inout temp of a call; a user signal is
        // ptr_arg only by being a ref actual, see make_arg_temp), and for a member
        // reachable through a virtual interface (the access is emitted once per
        // definition against a base chosen at run time, so it cannot depend on what
        // the analysis proved about one instance's writers, LRM 25.9). Those take
        // the CANONICAL type-derived shape, the same rule an aggregate leaf follows,
        // so that both sides of an alias agree without either knowing the other.
        slot.has_z  = slot.four_state && (may_z[i] || slot.init_z || !slot.undriven_mask.empty() ||
                                          ptr_arg[i] || vif_sig_[i]);
        slot.slot_ty = slot_type(slot.width, slot.four_state, slot.has_z);
        if (ty.kind == Type::Kind::UnpackedArray) {
            slot.is_array   = true;
            slot.elem_count = array_leaf_count(sigs[i].type);
            slot.storage_ty = llvm::ArrayType::get(slot.slot_ty, slot.elem_count);
        } else {
            slot.storage_ty = slot.slot_ty;
        }
    }
    join_twin_slots();

    std::vector<llvm::Type*> root_fields;
    std::vector<bool>        claimed(sigs.size(), false);
    for (const InstanceScope& top : mod_.tops()) {
        scope_type(top, claimed, top.name);
    }
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        if (!claimed[i]) {
            slots_[i].path = { static_cast<uint32_t>(root_fields.size()) };
            root_fields.push_back(slots_[i].storage_ty);
        }
    }

    // Shadow staging fields for NBA targets. A shadow mirrors the live
    // i(2W) / iW planes; a partial target adds a dirty mask. Fields are
    // grouped into one sub-struct per instance scope (per-definition type,
    // deduplicated like the state structs), then the unscoped signals'
    // fields flat behind them: an anchored signal's sh_* indices are
    // def-local, so twin instances stage through identical constants
    // (shadow_ptr resolves the instance base). Root shadow layout is
    // otherwise as before — one struct behind the opaque shadow base.
    if (n_nba_ > 0) {
        const auto add_fields = [&](SignalSlot& slot, std::vector<llvm::Type*>& sf) {
            // An aggregate target shadows each static leaf's mm (and z, for a
            // 4-state leaf) plane; the commit selects leaves by the block's
            // static-leaf-set mask. A runtime-path-only aggregate (deferred
            // records) needs no block shadow at all.
            if (slot.is_agg) {
                if (slot.nba_agg) {
                    for (AggLeaf& m : slot.static_leaves) {
                        m.sh_mm_field = static_cast<uint32_t>(sf.size());
                        sf.push_back(int_ty(m.four_state ? 2 * m.width : m.width));
                        if (m.has_z) {
                            m.sh_z_field = static_cast<uint32_t>(sf.size());
                            sf.push_back(int_ty(m.width));
                        }
                    }
                }
                return;
            }
            slot.sh_mm_field = static_cast<uint32_t>(sf.size());
            // A 2-state target shadows a single iW word (no z shadow).
            sf.push_back(int_ty(slot.four_state ? 2 * slot.width : slot.width));
            // A never-z target stages no z shadow; sh_z_field stays unused.
            if (slot.has_z) {
                slot.sh_z_field = static_cast<uint32_t>(sf.size());
                sf.push_back(int_ty(slot.width));
            }
            // Only a dynamic-select target needs a runtime dirty-mask field; a
            // static-select target's mask is a compile-time immediate in its
            // block commit.
            if (slot.nba_dyn) {
                slot.sh_mask_field = static_cast<uint32_t>(sf.size());
                sf.push_back(int_ty(slot.width));
            }
        };

        std::vector<llvm::Type*> sf;
        std::vector<std::tuple<llvm::StructType*, std::vector<llvm::Type*>, llvm::StructType*>>
          shadow_defs;
        for (uint32_t a = 0; a < anchors_.size(); ++a) {
            std::vector<llvm::Type*> af;
            for (uint32_t i = 0; i < sigs.size(); ++i) {
                if (slots_[i].anchor == a && slots_[i].nba) {
                    add_fields(slots_[i], af);
                }
            }
            if (af.empty()) {
                continue;
            }
            Anchor& an = anchors_[a];
            for (const auto& [ty, flds, st] : shadow_defs) {
                if (ty == an.ty && flds == af) {
                    an.shadow_ty = st;
                    break;
                }
            }
            if (an.shadow_ty == nullptr) {
                an.shadow_ty = llvm::StructType::create(ctx_, af, an.ty->getName().str() + ".shadow");
                shadow_defs.emplace_back(an.ty, std::move(af), an.shadow_ty);
            }
        }
        for (uint32_t i = 0; i < sigs.size(); ++i) {
            if (slots_[i].anchor == SignalSlot::no_anchor && slots_[i].nba) {
                add_fields(slots_[i], sf);
            }
        }
        // Anchored scopes' staging lives in per-instance shadow globals
        // (Anchor::shadow_gv); the root @clotho.nba.shadow holds only the
        // unscoped signals' fields, and is skipped entirely when every
        // NBA target is scope-owned.
        if (!sf.empty()) {
            shadow_ty_ = llvm::StructType::create(ctx_, sf, "clotho.nba.shadow");
        }
    }

    state_ty_ = llvm::StructType::create(ctx_, root_fields, "clotho.state");
}

const SignalSlot& Backend::slot_of(Signal s) const {
    return slots_[static_cast<uint32_t>(s)];
}

// The binding through which the code being lowered reaches `s`: the own
// instance's extern declaration bound to this very signal. Only code that
// belongs to an instance has one; a signal of the own instance, and one an
// installed instance context describes, are addressed through the record or the
// context instead.
uint32_t Backend::binding_of(const SignalSlot& s) {
    if (own_anchor_ == SignalSlot::no_anchor || s.anchor == own_anchor_ || ctx_override_for(s)) {
        return no_bind;
    }
    if (&s < slots_.data() || &s >= slots_.data() + slots_.size()) {
        return no_bind;
    }
    Anchor&    a  = anchors_[own_anchor_];
    const auto it = a.bind_of.find(static_cast<uint32_t>(&s - slots_.data()));
    if (it == a.bind_of.end()) {
        return no_bind;
    }
    a.bind_used.resize(a.externs.size(), 0);
    a.bind_used[it->second] = 1;
    return it->second;
}

// A field of the own instance's record: the parameter of a thunk row, else the
// instance's record global.
llvm::Value* Backend::own_field(uint32_t field, llvm::Type* ty) {
    Anchor& a = anchors_[own_anchor_];
    if (own_ctx_ != nullptr) {
        return b_.CreateLoad(ty, b_.CreateInBoundsGEP(a.ctx_ty, own_ctx_, { b_.getInt32(0), b_.getInt32(field) }));
    }
    auto* gep = llvm::ConstantExpr::getInBoundsGetElementPtr(
      a.ctx_ty, ensure_ctx(a), llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(field) });
    return b_.CreateLoad(ty, gep);
}

// The address of a signal's storage as a link-time constant: what a binding to
// the signal holds.
llvm::Constant* Backend::storage_address(const SignalSlot& s) {
    std::vector<llvm::Constant*> idx{ b_.getInt32(0) };
    for (const uint32_t f : s.path) {
        idx.push_back(b_.getInt32(f));
    }
    if (s.anchor == SignalSlot::no_anchor) {
        return llvm::ConstantExpr::getInBoundsGetElementPtr(state_ty_, sym(state_global_), idx);
    }
    Anchor& a = anchors_[s.anchor];
    return llvm::ConstantExpr::getInBoundsGetElementPtr(a.ty, sym(a.state_gv), idx);
}

llvm::Value* Backend::bound_record(const SignalSlot& s) {
    const uint32_t k = binding_of(s);
    if (k == no_bind || s.anchor == SignalSlot::no_anchor) {
        return nullptr;
    }
    return own_field(anchors_[own_anchor_].ctx_bind_base + 2 * k + 1, llvm::PointerType::getUnqual(ctx_));
}

// Pointer to a signal's whole storage (its slot struct, or its [N x slot]
// memory). A signal of another instance or of a package is the address of a
// binding of the own instance. One of the own instance (or one an installed
// instance context describes) is `@clotho.inst.<hier>.state + the
// scope-relative field path` -- a real symbol plus a def-local addend -- or,
// inside a group of the instance, the base loaded from its record. An unscoped
// signal outside a group keeps the direct root GEP.
llvm::Value* Backend::storage_ptr(const SignalSlot& s) {
    if (const uint32_t k = binding_of(s); k != no_bind) {
        // Another instance's (or a package's) signal: its address is a binding
        // of the own instance, filled at startup, so the code names no storage
        // of a neighbour.
        return own_field(anchors_[own_anchor_].ctx_bind_base + 2 * k, llvm::PointerType::getUnqual(ctx_));
    }
    std::vector<llvm::Value*> idx;
    idx.push_back(b_.getInt32(0));
    if (s.anchor == SignalSlot::no_anchor) {
        for (const uint32_t f : s.path) {
            idx.push_back(b_.getInt32(f));
        }
        return b_.CreateInBoundsGEP(state_ty_, sym(state_global_), idx);
    }
    Anchor&      a    = anchors_[s.anchor];
    llvm::Value* base = def_relative_for(s) ? ctx_field(a, 0, llvm::PointerType::getUnqual(ctx_))
                                            : static_cast<llvm::Value*>(sym(a.state_gv));
    for (const uint32_t f : s.path) {
        idx.push_back(b_.getInt32(f));
    }
    return b_.CreateInBoundsGEP(a.ty, base, idx);
}

// Address of the waiter-list head for one (member, edge) of the instance the
// current access names: the same base a member access uses, plus the head's
// def-local field index (see scope_type). Shaped like an event object, so the
// intrusive list primitives apply unchanged.
llvm::Value* Backend::vif_head_ptr(Signal sig, uint32_t edge) {
    const SignalSlot& s   = slot_of(sig);
    const auto        si  = static_cast<uint32_t>(sig);
    const uint32_t    ord = vif_head_ord_[si][edge];
    if (s.anchor == SignalSlot::no_anchor || ord == ~0u) {
        throw BackendError{ "internal: no virtual-interface waiter head for this member" };
    }
    Anchor&      a    = anchors_[s.anchor];
    // Exactly storage_ptr's base rule: an instance's own group loads its
    // instance base rather than bake the instance's symbol.
    llvm::Value* base = def_relative_for(s) ? ctx_field(a, 0, llvm::PointerType::getUnqual(ctx_))
                                            : static_cast<llvm::Value*>(sym(a.state_gv));
    return b_.CreateInBoundsGEP(a.ty, base,
                                { b_.getInt32(0), b_.getInt32(a.vif_head_base + ord) });
}

// Pointer to one field of the root NBA shadow struct, addressed through a base
// pointer loaded from a global rather than a constant offset from `state`.
//
// NOTE: the indirection is a deliberate defense against CPU memory-dependence
// mis-speculation, not an abstraction nicety. The shadow region mirrors the
// live signal region at a constant distance, so state-relative addressing
// gives a shadow store and a nearby live load of a *different* signal the same
// (base register, displacement mod 1024) form whenever the distance's low bits
// line up (e.g. 256 32-bit NBA targets). Zen 4's memory-renaming predictor
// keys on exactly that form and falsely forwards the store to the load; each
// false match is a pipeline resync (~1 per staged signal, 3x sim time on
// register-file-shaped designs, at any codegen level). Loading the shadow base
// from a global keeps codegen from folding shadow addresses into
// state+constant, so the colliding form cannot arise.
// Whether the code being emitted should address `owner`'s per-instance
// runtime structures def-relatively, through the instance record: inside a
// single-scope group or a member function of a group spanning instances, for
// the signals of the instance that function was emitted for (own_anchor_), and
// for a virtual interface access. Everything else (tasks, functions, commit
// functions, signals of another instance a body reaches by hierarchical
// reference) keeps the absolute/folded form until bindings supply those
// records too. Either form addresses the same storage; this is codegen policy
// only.
bool Backend::def_relative_for(const SignalSlot& owner) const {
    // A virtual interface access is def-relative by construction: the instance
    // is not known until run time, so there is no absolute form to fold to.
    return ctx_override_for(owner) ||
           (own_anchor_ != SignalSlot::no_anchor && owner.anchor == own_anchor_);
}

// `owner` is the signal whose (def-local, for an anchored signal) shadow
// field is addressed. An instance's own code (def_relative_for) loads its
// record's shadow base (@clotho.inst.*.shadow); every other context
// folds the sub-struct index through the root base; unscoped signals keep
// the flat root field. Every path loads a base pointer, preserving the
// opacity requirement above.
llvm::Value* Backend::shadow_ptr(const SignalSlot& owner, uint32_t field) {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    if (owner.anchor != SignalSlot::no_anchor) {
        // An NBA target's anchor always carries a shadow struct (the
        // fields were assigned per anchor in build_state_types).
        Anchor&      a    = anchors_[owner.anchor];
        llvm::Value* base = def_relative_for(owner) ? ctx_field(a, 1, ptr) : b_.CreateLoad(ptr, anchor_shadow_base(a));
        return b_.CreateInBoundsGEP(a.shadow_ty, base, { b_.getInt32(0), b_.getInt32(field) });
    }
    llvm::Value* base = b_.CreateLoad(ptr, shadow_base_gv());
    return b_.CreateInBoundsGEP(shadow_ty_, base, { b_.getInt32(0), b_.getInt32(field) });
}

// Per-instance opaque shadow base (@clotho.inst.<hier>.shadow_base):
// created on first unshared shadow access, filled by main right after the
// ctx fills. Same one-load opacity as the root shadow_base_gv.
llvm::GlobalVariable* Backend::anchor_shadow_base(Anchor& a) {
    if (a.shadow_base_gv == nullptr) {
        auto* ptr        = llvm::PointerType::getUnqual(ctx_);
        a.shadow_base_gv = new llvm::GlobalVariable(env_->llmod, ptr, false,
                                                    llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantPointerNull::get(ptr),
                                                    "clotho.inst." + a.name + ".shadow_base");
    }
    return sym(a.shadow_base_gv);
}

// Create the state globals — one per instance scope
// (@clotho.inst.<hier>.state, own signals only) plus the root
// @clotho.state for unscoped signals and the standalone NBA shadow global
// — and record each anchor's lowest owned process index, before any body
// is emitted (storage_ptr resolves through the anchors). Every global's
// initializer is its power-on state (see emit_main's comment). A reused
// definition's shareable accesses instead go through its instance context
// (ensure_ctx), which main fills at startup before init_state and every
// process.
void Backend::emit_state_global() {
    auto* state_gv = new llvm::GlobalVariable(env_->llmod, state_ty_, false,
                                              llvm::GlobalValue::InternalLinkage,
                                              root_state_initializer(), "clotho.state");
    state_gv->setAlignment(llvm::Align(64));
    state_global_ = state_gv;

    for (Anchor& a : anchors_) {
        a.state_gv = new llvm::GlobalVariable(env_->llmod, a.ty, false,
                                              llvm::GlobalValue::InternalLinkage,
                                              scope_state_initializer(a),
                                              "clotho.inst." + a.name + ".state");
        // ABI alignment (no explicit override): small scope structs pack
        // into shared cache lines; 64-byte over-alignment measured +1.5%
        // hazard3 sim from scattering hot signals across lines.
    }

    for (Anchor& a : anchors_) {
        if (a.shadow_ty != nullptr) {
            a.shadow_gv = new llvm::GlobalVariable(env_->llmod, a.shadow_ty, false,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantAggregateZero::get(a.shadow_ty),
                                                   "clotho.inst." + a.name + ".shadow");
        }
    }
    if (shadow_ty_ != nullptr) {
        shadow_global_ = new llvm::GlobalVariable(env_->llmod, shadow_ty_, false,
                                                  llvm::GlobalValue::InternalLinkage,
                                                  llvm::ConstantAggregateZero::get(shadow_ty_),
                                                  "clotho.nba.shadow");
    }

    const auto procs = mod_.processes();
    for (uint32_t k = 0; k < procs.size(); ++k) {
        const uint32_t sc = procs[k].scope;
        if (sc == Process::no_scope || sc >= anchor_of_scope_.size() ||
            anchor_of_scope_[sc] == SignalSlot::no_anchor) {
            continue;
        }
        uint32_t& first = anchors_[anchor_of_scope_[sc]].first_proc;
        first           = std::min(first, k);
    }
}

// Lazily create the instance record global (see Anchor). The type is computed
// by build_instance_ctx_layout; only instances actually addressed through
// their record materialize a global (and a fill in main).
llvm::GlobalVariable* Backend::ensure_ctx(Anchor& a) {
    if (a.ctx_gv == nullptr) {
        a.ctx_gv = new llvm::GlobalVariable(env_->llmod, a.ctx_ty, false,
                                            llvm::GlobalValue::InternalLinkage,
                                            llvm::ConstantAggregateZero::get(a.ctx_ty),
                                            "clotho.inst." + a.name + ".ctx");
    }
    return sym(a.ctx_gv);
}

// A def-relative load of one instance-record field. The record is the one
// body being emitted was handed (own_ctx_: a member function's first
// parameter) or, for a single-scope group, the instance's record global.
// `ctx_override_` redirects it to an instance chosen at run time -- a virtual
// interface access, whose whole point is that the def-local field constants
// stay while the record they are read from is whichever instance the handle
// names (LRM 25.9).
llvm::Value* Backend::ctx_field(Anchor& a, uint32_t field, llvm::Type* ty) {
    // An installed context describes ONE instance; a field of any other
    // instance's record (the own instance's, while it writes through a binding)
    // comes from that record.
    llvm::Value* rec = nullptr;
    if (ctx_override_ != nullptr && ctx_override_anchor_ != SignalSlot::no_anchor &&
        &a == &anchors_[ctx_override_anchor_]) {
        rec = ctx_override_;
    } else if (own_ctx_ != nullptr && &a == &anchors_[own_anchor_]) {
        rec = own_ctx_;
    }
    if (rec != nullptr) {
        return b_.CreateLoad(ty, b_.CreateInBoundsGEP(a.ctx_ty, rec, { b_.getInt32(0), b_.getInt32(field) }));
    }
    auto* gep = llvm::ConstantExpr::getInBoundsGetElementPtr(
      a.ctx_ty, ensure_ctx(a), llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(field) });
    return b_.CreateLoad(ty, gep);
}

// The record of the instance a deferred store to `s` lands in, which its
// replay hands to the wait-queue service so it wakes the instance the value
// actually reached: through a virtual interface handle the one the handle
// named; inside a subroutine reached through one, the instance formal 0
// named (the body-wide override); from a member function its own record; from
// a single-scope group the instance's record global.
llvm::Value* Backend::record_ptr(const SignalSlot& s, llvm::Value* vif_ctx) {
    if (vif_ctx != nullptr) {
        return vif_ctx;
    }
    if (ctx_override_for(s)) {
        return ctx_override_;
    }
    if (own_ctx_ != nullptr && s.anchor == own_anchor_) {
        return own_ctx_;
    }
    return ensure_ctx(anchors_[s.anchor]);
}

// The anchor a vif-rooted lvalue's context record describes: its representative
// signal's own scope. no_anchor when the lvalue is not vif-rooted, which makes
// the CtxOverride a no-op.
uint32_t Backend::lvalue_ctx_anchor(const LValueInfo& lv) const {
    return lv.vif_ctx != nullptr && lv.is_signal ? slot_of(lv.sig).anchor : SignalSlot::no_anchor;
}

// Whether the installed instance context (if any) describes `owner`'s scope,
// and may therefore supply its bases.
bool Backend::ctx_override_for(const SignalSlot& owner) const {
    return ctx_override_ != nullptr && owner.anchor == ctx_override_anchor_;
}

// Materialize a single-scope group's deferred sched-flag pointer at the
// current insert point (a function entry, so it dominates every use): the
// instance's sched base (record field 2) plus the group's def-local ordinal.
// No-op when lower_group chose the absolute constant.
void Backend::materialize_group_ctx() {
    if (sched_ptr_ != nullptr) {
        return;
    }
    Anchor&      a    = anchors_[own_anchor_];
    llvm::Value* base = ctx_field(a, 2, llvm::PointerType::getUnqual(ctx_));
    sched_ptr_ = b_.CreateInBoundsGEP(sched_word_ty(), base, b_.getInt32(own_ordinal_));
}

// Global holding &state->shadow; created on first use, initialized in
// emit_main right after the state allocation (before any process runs).
llvm::GlobalVariable* Backend::shadow_base_gv() {
    if (shadow_base_gv_ == nullptr) {
        auto* ptr       = llvm::PointerType::getUnqual(ctx_);
        shadow_base_gv_ = new llvm::GlobalVariable(env_->llmod, ptr, false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantPointerNull::get(ptr), "clotho.nba.shadow_base");
    }
    return sym(shadow_base_gv_);
}

// Pointer to an lvalue's mm (field 0) or z (field 1) word, resolving the
// selected leaf element for a memory target.
llvm::Value* Backend::lv_field_ptr(const LValue& lv, uint32_t field) {
    if (lv.is_agg) {
        // Walk the composed GEP path from the aggregate root to the leaf slot,
        // then select the leaf's mm (0) / z (1) field. Struct fields and array
        // indices are indistinguishable to the GEP — both are index operands.
        std::vector<llvm::Value*> idx;
        idx.reserve(lv.agg_path.size() + 2);
        idx.push_back(b_.getInt32(0));
        for (llvm::Value* step : lv.agg_path) {
            idx.push_back(step);
        }
        idx.push_back(b_.getInt32(field));
        return b_.CreateInBoundsGEP(lv.root_ty, lv.base, idx);
    }
    if (lv.array_ty != nullptr) {
        llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
        return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx, b_.getInt32(field) });
    }
    return b_.CreateStructGEP(lv.slot_ty, lv.base, field);
}

// The address of the leaf slot an lvalue designates: lv_field_ptr without the
// final field select. A guard that has to name the slot itself, to pick it or a
// stand-in (safe_lvalue, a `ref` binding), forms it here; the ordinary load / store
// path keeps lv_field_ptr's single GEP.
llvm::Value* Backend::slot_ptr(const LValue& lv) {
    if (lv.is_agg) {
        return agg_slot_ptr(lv);
    }
    if (lv.array_ty != nullptr) {
        llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
        return b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx });
    }
    return lv.base;
}

// `sel` rooted at the slot `slot` itself: the path and the element index it carried
// were spent forming `slot`, so none is left to apply to it.
LValue Backend::slot_lvalue(const LValueInfo& sel, llvm::Value* slot) {
    LValue lv;
    static_cast<LValueInfo&>(lv) = sel;
    lv.base                      = slot;
    lv.array_ty                  = nullptr;
    lv.elem_idx                  = nullptr;
    lv.is_agg                    = false;
    lv.root_ty                   = nullptr;
    lv.agg_path.clear();
    return lv;
}

// An lvalue a runtime routine can write through, or that can be read for a snapshot,
// whether or not `g` is valid. An invalid place stands in for a scratch slot that
// starts at the element default (LRM 7.4.5), so what the routine writes goes nowhere
// and what is read is defined. The caller wakes waiters only for the valid place
// (`g.when_valid`).
LValue Backend::safe_lvalue(const Guarded& g) {
    if (g.valid() == nullptr) {
        return g.unguarded();
    }
    assert(!g.is_string && !g.is_handle && "a managed place has its own sink");
    const auto scratch = [&]() -> llvm::Value* {
        auto* slot = entry_alloca(g.slot_ty, "valid.scratch");
        b_.CreateStore(slot_default_const(g.slot_ty, g.store_width, g.four_state, g.has_z), slot);
        return slot;
    };
    return slot_lvalue(
      g, g.pointer_or(b_, [&](const LValue& lv) { return slot_ptr(lv); }, scratch));
}

// The power-on image of one leaf slot: what an out-of-range read of it must
// yield (LRM 7.4.5 / Table 7-1 -- x for a 4-state element, 0 for 2-state).
llvm::Constant* Backend::slot_default_const(llvm::StructType* slot_ty, uint32_t width, bool four_state,
                                            bool has_z) {
    std::vector<llvm::Constant*> fields{ four_state ? x_mm(width) : zero(width) };
    if (has_z) {
        fields.push_back(zero(width));
    }
    return llvm::ConstantStruct::get(slot_ty, fields);
}

// The words a `ref` actual passes (docs/design/data-model.md §3): the aliased
// storage pointer, and — for a place inside a container's heap buffer (§3.0) —
// the relocation epoch of each container the place is rooted in and that
// epoch's value now (ref_witness_chain).
//
// No site record. A staleness report needs the formal's name, which is the
// callee's own, and the container's name, which the runtime recovers from the
// epoch pointer (§3.3) — so nothing about the report is caller knowledge that
// has to be shipped per call.
//
// `slot` is the whole storage of the selected place, whatever its class: a
// plane slot, a managed handle, or an aggregate image. A bit / part selection
// is not a place a pointer can name, so it is rejected — the frontend guards
// it too (is_whole_ref_actual), and this is the defensive backstop.
RefActual Backend::ref_actual_arg(const Guarded& raw, Ref type) {
    // An associative-array element is a DEFERRED place, and a `ref` is its third
    // consumer alongside Load and Store: it needs the address itself. Resolve it
    // with put_ptr, because LRM 13.5.2 binds the formal to a VARIABLE — there is
    // no variable to alias until the entry exists, so forming the ref creates it,
    // exactly as a mutating method on `a[k]` does.
    const Guarded g = raw.is_assoc ? assoc_elem_lvalue(raw, /*for_write=*/true) : raw;
    RefActual     ra;
    // One witness per container level, outermost first, each sampled now. A
    // stable place has none.
    for (llvm::Value* ep : g.epoch_chain) {
        ra.epochs.push_back(ep);
        ra.expects.push_back(b_.CreateLoad(b_.getInt64Ty(), ep, "ref.epoch.at.call"));
    }
    ra.slot = ref_actual_ptr(g, type);
    return ra;
}

// The witness chain a `ref` call passes: null for a place that cannot move (a
// module signal, a frame local, a fixed unpacked array element, a struct member,
// a class property or a whole container handle, which the callee then never
// checks), else a caller-frame array of (&epoch, expected epoch) pairs, outermost
// first, ended by a pair whose epoch is null. One shape whatever the actual, so
// the formal's signature does not depend on any caller.
llvm::Value* Backend::ref_witness_chain(const RefActual& ra) {
    auto* const ptr = llvm::PointerType::getUnqual(ctx_);
    if (ra.epochs.empty()) {
        return llvm::ConstantPointerNull::get(ptr);
    }
    auto* const pair  = ref_witness_ty();
    auto* const arr   = llvm::ArrayType::get(pair, ra.epochs.size() + 1);
    llvm::Value* chain = nullptr;
    if (in_coroutine_body()) {
        // Frame-resident, like the task-call record: a task's frame is what the
        // callee reads through while the caller is suspended inside the call.
        llvm::IRBuilder<> ab(coro_begin_->getParent(), std::next(coro_begin_->getIterator()));
        chain = ab.CreateAlloca(arr, nullptr, "ref.chain");
    } else {
        chain = entry_alloca(arr, "ref.chain");
    }
    for (size_t lvl = 0; lvl < ra.epochs.size(); ++lvl) {
        b_.CreateStore(ra.epochs[lvl], b_.CreateInBoundsGEP(arr, chain, { b_.getInt32(0), b_.getInt32(static_cast<uint32_t>(lvl)), b_.getInt32(0) }));
        b_.CreateStore(ra.expects[lvl], b_.CreateInBoundsGEP(arr, chain, { b_.getInt32(0), b_.getInt32(static_cast<uint32_t>(lvl)), b_.getInt32(1) }));
    }
    b_.CreateStore(llvm::ConstantPointerNull::get(ptr),
                   b_.CreateInBoundsGEP(arr, chain, { b_.getInt32(0), b_.getInt32(static_cast<uint32_t>(ra.epochs.size())), b_.getInt32(0) }));
    return chain;
}

// Pointer to a ref actual's storage: the aliased place the callee holds.
// A whole scalar is its slot pointer; an unpacked-array or container element is
// `&array[idx]`; an aggregate is its image pointer; a managed value is its
// handle pointer. `type` is the formal's type, which names the image an invalid
// aggregate binds to.
//
// A `ref` formal aliases storage for the whole call, so an invalid element (an
// out-of-range or x/z index, an absent key) cannot simply be dropped the way a
// store can -- the callee would hold a live pointer past the array and read and
// write through it. It binds to a discard slot instead, freshly reset to the
// element default: the callee's reads see what LRM 7.4.5 / Table 7-1 say an
// invalid element read yields, and its writes go nowhere, which is the
// no-operation the same clause requires. Mirrors lv_string_ptr's sink redirect
// for an invalid string element.
//
// The clause does not spell out `ref` binding to an invalid element, so this is
// the reading of it that keeps both halves of 7.4.5 true; the alternative
// (aliasing real storage) is memory corruption.
llvm::Value* Backend::ref_actual_ptr(const Guarded& g, Ref type) {
    emit_bounds_reports(g, /*is_write=*/true);
    if (g.is_string || g.is_handle) {
        // A managed place: the ref aliases the handle itself, so the callee's
        // deep-copy / mutation lands on the actual.
        return g.is_string ? lv_string_ptr(g, StringUse::Place) : lv_handle_ptr(g, HandleUse::Place);
    }
    if (g.is_agg && !g.agg_leaf) {
        // An aggregate is addressed by its GEP path; the ABI guarantees the
        // canonical layout on both sides.
        return agg_place_ptr(g, type);
    }
    if (!g.is_agg) {
        const bool whole = !g.masked() && is_zero_const(g.bit_off) && g.width == g.store_width;
        if (!whole) {
            throw BackendError{ "ref actual must be a whole variable, element or member" };
        }
    }
    // A plane slot: the whole variable, `&array[idx]`, or a leaf of an aggregate.
    const auto sink = [&]() -> llvm::Value* {
        auto* slot = entry_alloca(g.slot_ty, "ref.sink");
        b_.CreateStore(slot_default_const(g.slot_ty, g.store_width, g.four_state, g.has_z), slot);
        return slot;
    };
    return g.pointer_or(b_, [&](const LValue& lv) { return slot_ptr(lv); }, sink);
}

llvm::Value* Backend::lv_mm_ptr(const LValue& lv) {
    return lv_field_ptr(lv, 0);
}

llvm::Value* Backend::lv_z_ptr(const LValue& lv) {
    // A never-z slot has no field 1; any consumer must branch on has_z
    // before asking for the pointer.
    assert(lv.has_z);
    return lv_field_ptr(lv, 1);
}

// Give every signal an instance owns a field in its record for each design-wide
// id it has (IdFamily), in the signal's def-local order, so the ordinals are the
// same in every instance of a unit, and remember the id each field holds for the
// startup fill. Presence is uniform across a unit's instances because the sets
// the ids index are joined over them (nba_classify).
void Backend::assign_id_fields() {
    const size_t n = mod_.signals().size();
    id_ord_.assign(n, [] {
        std::array<uint32_t, kIdFamilies> a;
        a.fill(no_id);
        return a;
    }());
    id_val_ = id_ord_;
    const auto note = [&](const std::vector<Signal>& sigs, IdFamily f, uint32_t base) {
        for (uint32_t k = 0; k < sigs.size(); ++k) {
            id_val_[static_cast<uint32_t>(sigs[k])][static_cast<uint32_t>(f)] = base + k;
        }
    };
    const auto na = static_cast<uint32_t>(array_nba_sigs_.size());
    const auto ng = static_cast<uint32_t>(agg_nba_sigs_.size());
    const auto nh = static_cast<uint32_t>(handle_nba_sigs_.size());
    note(array_nba_sigs_, IdFamily::Arr, 0);
    note(agg_nba_sigs_, IdFamily::Agg, na);
    note(handle_nba_sigs_, IdFamily::Handle, na + ng);
    note(string_nba_sigs_, IdFamily::Str, na + ng + nh);
    note(intra_nba_sigs_, IdFamily::Intra, 0);
    for (uint32_t i = 0; i < n && i < trigger_id_.size(); ++i) {
        for (uint32_t e = 0; e < 3; ++e) {
            if (trigger_id_[i][e] >= 0) {
                id_val_[i][static_cast<uint32_t>(IdFamily::TrigPos) + e] = static_cast<uint32_t>(trigger_id_[i][e]);
            }
        }
    }
    for (uint32_t ai = 0; ai < anchors_.size(); ++ai) {
        uint32_t next = 0;
        for (const Signal sj : anchor_signals_[ai]) {
            if (sj == Signal{ ~0u }) {
                continue;
            }
            const auto si = static_cast<uint32_t>(sj);
            for (uint32_t f = 0; f < kIdFamilies; ++f) {
                if (id_val_[si][f] != no_id) {
                    id_ord_[si][f] = next++;
                }
            }
        }
        anchors_[ai].n_id = next;
    }
}

// Compute each instance context's type (see Anchor): shared per unit, fields
// sized from the instance's process count (one block-id field per owned
// process, and when some group is guarded two trigger-mask fields each), wait
// buckets, bindings and ids. Runs after fusion grouping, trigger colouring,
// wait-queue sizing and build_sched_flags (the process ordinals), before
// lowering.
//
// The block-id field of a group function is indexed by the def-local ordinal of
// its front process, the same ordinal that names the function and indexes its
// flag in the sched array. The function's NBA blocks are contiguous, so one
// field holds their first id, and the field is the same in every instance of a
// unit because every instance has the same groups.
void Backend::build_instance_ctx_layout() {
    assign_id_fields();
    // Twin instances own the same number of processes; size blk fields by the
    // definition-wide maximum so layouts agree even if one of them degenerated
    // (they should be equal by construction).
    ankerl::unordered_dense::map<llvm::StructType*, uint32_t> def_blk;
    for (const Anchor& a : anchors_) {
        auto& n = def_blk[a.ty];
        n       = std::max(n, a.sched_ty != nullptr ? static_cast<uint32_t>(a.sched_ty->getNumElements()) : 0u);
    }

    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    any_defer_ = std::any_of(facts_.processes.begin(), facts_.processes.end(),
                             [](const auto& p) { return p.deferred_severity; });
    auto* i32 = b_.getInt32Ty();
    // Per unit, not per state type: two units can lay their own signals out
    // alike (the same definition at different parameters) yet import different
    // things, and the record is as much a unit's as its code is.
    std::map<std::pair<llvm::StructType*, uint32_t>, llvm::StructType*> def_ctx;
    for (Anchor& a : anchors_) {
        a.n_blk        = def_blk[a.ty];
        a.n_mask       = any_guarded_ ? a.n_blk : 0;
        a.n_bind       = static_cast<uint32_t>(a.externs.size());
        a.n_fbind      = static_cast<uint32_t>(a.fn_externs.size());
        a.n_defer      = any_defer_ ? a.n_blk : 0;
        a.ctx_blk_base = 4;
        uint32_t nwait = 0;
        uint32_t nsta  = 0;
        for (const Signal sj : anchor_signals_[&a - anchors_.data()]) {
            if (sj == Signal{ ~0u }) {
                continue;
            }
            const auto si = static_cast<uint32_t>(sj);
            for (uint32_t e = 0; e < 3; ++e) {
                nwait += wait_slot_sta_[si][e] != no_wait_slot ? 1 : 0;
                nwait += wait_slot_dyn_[si][e] != no_wait_slot ? 2 : 0;
                nsta += wait_slot_sta_[si][e] != no_wait_slot ? 1 : 0;
            }
        }
        a.ctx_wait_base        = a.ctx_blk_base + a.n_blk;
        a.ctx_cap_base         = a.ctx_wait_base + nwait;
        a.ctx_trig_base        = a.ctx_cap_base + nsta;
        a.ctx_mmask_base       = a.ctx_trig_base + nsta;
        a.ctx_gmask_base       = a.ctx_mmask_base + a.n_mask;
        a.ctx_bind_base        = a.ctx_gmask_base + a.n_mask;
        a.ctx_fbind_base       = a.ctx_bind_base + 2 * a.n_bind;
        a.ctx_id_base          = a.ctx_fbind_base + a.n_fbind;
        a.ctx_name_base        = a.ctx_id_base + a.n_id;
        a.ctx_defer_base       = a.ctx_name_base + 1;
        const auto [it, fresh] = def_ctx.try_emplace({ a.ty, a.unit }, nullptr);
        if (fresh) {
            std::vector<llvm::Type*> f{ ptr, ptr, ptr, i32 };
            f.insert(f.end(), a.n_blk, i32);
            f.insert(f.end(), nwait, ptr);
            f.insert(f.end(), nsta, i32);
            f.insert(f.end(), nsta, b_.getInt64Ty());
            f.insert(f.end(), 2 * a.n_mask, b_.getInt64Ty());
            f.insert(f.end(), 2 * a.n_bind, ptr);
            f.insert(f.end(), a.n_fbind, ptr);
            f.insert(f.end(), a.n_id, i32);
            f.push_back(ptr); // the instance's name handle (ctx_name_base)
            f.insert(f.end(), a.n_defer, b_.getInt64Ty());
            it->second = llvm::StructType::create(
              ctx_, f, a.ty->getName().str() + ".ctx");
        }
        a.ctx_ty = it->second;
        assert(a.ctx_ty->getNumElements() == a.ctx_defer_base + a.n_defer && "instances of one unit lay their records out alike");
    }
}

// Power-up the state struct: undriven 4-state nets come up Z (LRM 6.6 —
// must 0, may 1, z all-ones), other 4-state signals X (z 0), 2-state 0.
// Runtime power-on init: only unpacked arrays (memories). Every scalar
// signal's power-on value — X for a 4-state plane, an empty inline string
// handle, a clear nba dirty mask — is a compile-time constant folded into
// the state global's initializer (`state_initializer`), so no per-signal
// GEP+store is emitted here. Arrays stay a runtime loop: materialising a
// large memory's X-fill as a global constant would bloat the JIT object's
// `.data` (a non-zero pattern cannot go in `.bss`), so it is filled in
// O(1) emitted instructions instead.
void Backend::init_state() {
    for (const SignalSlot& s : slots_) {
        if (mod_.signals()[static_cast<size_t>(&s - slots_.data())].const_data != Ref::Invalid) {
            // A materialized constant table is already its own state
            // initializer (slot_const); an X-fill would erase it.
            continue;
        }
        if (s.is_agg) {
            std::vector<llvm::Value*> path;
            emit_agg_init(s.ir_type, storage_ptr(s), path, s.storage_ty);
            continue;
        }
        if (!s.is_array) {
            continue;
        }
        if (s.is_string) {
            emit_string_slot_op(s, str_init_fn());
        } else {
            emit_array_init(s);
        }
    }
}

// Constant power-on initializer for one state global. Every state struct
// is now flat — field k is signal k's storage (scope-relative path {k}) —
// so the initializer is a direct per-field loop: each signal gets its
// power-on constant (slot_const), array leaves zeroinitializer (filled at
// runtime by `init_state`). The shadow global is all-zero (written before
// read; masks power on clear).
llvm::Constant* Backend::state_init_fields(llvm::StructType* ty, const std::vector<const SignalSlot*>& by_field) {
    std::vector<llvm::Constant*> elems;
    elems.reserve(ty->getNumElements());
    for (uint32_t f = 0; f < ty->getNumElements(); ++f) {
        elems.push_back(by_field[f] != nullptr
                          ? slot_const(*by_field[f])
                          : llvm::Constant::getNullValue(ty->getElementType(f)));
    }
    return llvm::ConstantStruct::get(ty, elems);
}

llvm::Constant* Backend::root_state_initializer() {
    std::vector<const SignalSlot*> by_field(state_ty_->getNumElements(), nullptr);
    for (const SignalSlot& s : slots_) {
        if (s.anchor == SignalSlot::no_anchor && !s.path.empty()) {
            by_field[s.path.front()] = &s;
        }
    }
    return state_init_fields(state_ty_, by_field);
}

llvm::Constant* Backend::scope_state_initializer(const Anchor& a) {
    const auto aid = static_cast<uint32_t>(&a - anchors_.data());
    std::vector<const SignalSlot*> by_field(a.ty->getNumElements(), nullptr);
    for (const SignalSlot& s : slots_) {
        if (s.anchor == aid) {
            by_field[s.path.front()] = &s;
        }
    }
    return state_init_fields(a.ty, by_field);
}

} // namespace clotho::ir::backend
