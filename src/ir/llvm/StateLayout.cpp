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
// unowned (PreInit / package) processes.
void Backend::build_sched_flags() {
    const auto procs = mod_.processes();
    auto*      word  = llvm::Type::getInt64Ty(ctx_);
    proc_sched_anchor_.assign(std::max<size_t>(procs.size(), 1), ~0u);
    proc_sched_ord_.assign(std::max<size_t>(procs.size(), 1), 0);
    std::vector<uint32_t> owned(anchors_.size(), 0);
    uint32_t              n_root = 0;
    for (uint32_t k = 0; k < procs.size(); ++k) {
        const uint32_t sc = procs[k].scope;
        if (sc != Process::no_scope && sc < anchor_of_scope_.size() &&
            anchor_of_scope_[sc] != SignalSlot::no_anchor) {
            const uint32_t aid    = anchor_of_scope_[sc];
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

// The LLVM type of a wake word. One place, because materialize_group_ctx
// uses it as a GEP STRIDE: get it wrong there and a deduped instance
// addresses another process's word.
llvm::IntegerType* Backend::sched_word_ty() {
    return llvm::Type::getInt64Ty(ctx_);
}

// Bits a wake site sets when it cannot name the (signal, edge) it fired
// for: every trigger bit, so every guard in the woken group passes. Zero
// when no group is guarded, which keeps the unguarded emission identical.
uint64_t Backend::unclassified_trig_bits() const {
    return any_guarded_ ? ~1ull : 0ull;
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

// The `{ i(2W) must|may, iW z }` slot struct for a value of the given width
// and domain — just `{ i(2W) }` when the slot needs no z word (a proven
// never-z signal; frame locals always keep z). Identified and memoized per
// (width, domain, has_z) so every signal of one shape shares one named LLVM
// type (%logic.8, %bit.nz.4, ...); the single-field form keeps the struct
// wrapper so GEP paths and array-of-slot addressing stay uniform.
// `narrow` requests the genuine single-iW 2-state slot (`{ iW }`, named
// `%bit.W`) instead of the packed must|may word — used for a 2-state signal
// proven decoupled from every path that still reads the packed form (ABI, NBA,
// arrays). Only valid for a 2-state (four_state == false), never-z slot.
llvm::StructType* Backend::slot_type(uint32_t width, bool four_state, bool has_z, bool narrow) {
    for (const auto& [w, four, z, nr, ty] : slot_types_) {
        if (w == width && four == four_state && z == has_z && nr == narrow) {
            return ty;
        }
    }
    const std::string name = std::string(four_state ? "logic." : "bit.")
                           + (narrow ? "" : (has_z ? "" : "nz.")) + std::to_string(width);
    std::vector<llvm::Type*> fields{ int_ty(narrow ? width : 2 * width) };
    if (has_z) {
        fields.push_back(int_ty(width));
    }
    auto* ty = llvm::StructType::create(ctx_, fields, name);
    slot_types_.emplace_back(width, four_state, has_z, narrow, ty);
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

// Build one instance scope's state struct: its own signals' slots (field k
// == scope signal k). Children are NOT nested as fields — each scope owns
// its own state global, so a child scope recurses into its own anchor and
// struct. Assigns every covered signal's scope-relative GEP path ({k}).
// Struct types are deduplicated per module definition — instances of one
// definition (same parameterization) produce identical field lists and
// share one identified type. Each call is one scope occurrence and records
// one Anchor; storage_ptr addresses signals anchor-relative.
void Backend::scope_type(const InstanceScope& scope, std::vector<bool>& claimed, const std::string& hier) {
    const auto aid = static_cast<uint32_t>(anchors_.size());
    anchors_.push_back(Anchor{ .name = hier });
    if (scope.id >= anchor_of_scope_.size()) {
        anchor_of_scope_.resize(scope.id + 1, SignalSlot::no_anchor);
    }
    anchor_of_scope_[scope.id] = aid;
    std::vector<llvm::Type*> fields;
    for (uint32_t k = 0; k < scope.signals.size(); ++k) {
        const auto si = static_cast<uint32_t>(scope.signals[k]);
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
    for (uint32_t k = 0; k < scope.signals.size(); ++k) {
        const auto si = static_cast<uint32_t>(scope.signals[k]);
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

// Does a materialized constant table's image carry a z bit? The pool stores
// canonical val / xz planes per element, where z is (xz & ~val); a table with
// none can drop its z word like any never-z signal.
bool Backend::const_image_has_z(const SignalInfo& info) const {
    if (info.const_data == Ref::Invalid) {
        return false;
    }
    const Layout lay = mod_.pool.layout(info.type);
    if (lay.domain != Domain::Four || lay.width == 0) {
        return false;
    }
    const std::span<const uint8_t> img = mod_.pool.bytes(info.const_data);
    const uint32_t                 pb  = plane_bytes(lay.width);
    for (size_t off = 0; off + 2 * pb <= img.size(); off += 2 * pb) {
        for (uint32_t k = 0; k < pb; ++k) {
            if ((img[off + pb + k] & ~img[off + k]) != 0) {
                return true;
            }
        }
    }
    return false;
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
    // Before classification: which members a handle nonblocking-writes decides
    // their staging path, and every writer of such a member must agree on it.
    collect_vif_nba();
    for (uint32_t i = 0; i < sigs.size(); ++i) {
        slots_[i].vif_rec = vif_nba_[i];
    }
    // Classify NBA targets up front so the slot loop can keep an NBA-target
    // 2-state signal on the packed word (its shadow / commit still expect it).
    nba_classify();

    // A slot keeps its z word when the may-z analysis cannot prove it z-free
    // or the task ABI needs z storage behind it. Elision is analysis-driven,
    // not domain-driven: a 2-state signal assigned a z-carrying expression
    // round-trips the z bit today (Resize does not clear z across domains),
    // and that behavior is preserved — ordinary 2-state signals are never-z
    // through the analysis anyway.
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
        slot.is_signed   = ty.is_signed;
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
        // 2-state word: never z, so it takes the narrow single-iW form
        // below (pool layout gives Domain::Two for floats).
        slot.is_real     = mod_.pool.is_float(leaf_type(sigs[i].type));
        slot.init_z      = undriven[i] && sigs[i].is_net && slot.four_state;
        // A partially-driven packed net: its undriven bits power on z. Only set
        // for a 4-state, non-array net (the analysis's candidate set matches).
        // Fully-undriven takes init_z; fully-driven has no mask.
        const std::span<const uint64_t> pmask = facts_.undriven_mask(static_cast<Signal>(i));
        if (!slot.init_z && slot.four_state && !slot.is_array && !pmask.empty()) {
            slot.undriven_mask.assign(pmask.begin(), pmask.end());
        }
        // A materialized constant table has no store anywhere, so the may-z
        // analysis proves it never-z from an empty seed set — but a parameter
        // may itself hold z (`localparam logic [3:0] P[0:1] = '{4'bzzzz, 0}`).
        // Its own image is the authority.
        slot.has_z = may_z[i] || ptr_arg[i] || slot.init_z || !slot.undriven_mask.empty() ||
                     const_image_has_z(sigs[i]);
        // A 2-state signal drops the packed must|may word for a genuine iW slot
        // (or `[N x { iW }]` memory). The only requirement is never-z: a 2-state
        // value carries no z, and has_z is forced only by a task out/inout target
        // (the packed pointer ABI) or a may-z analysis result — both of which
        // keep the packed word. Every other 2-state path (scalar/array NBA
        // shadow + commit, $readmem via rt_readmem) is width-driven off `narrow`.
        slot.narrow = !slot.four_state && !slot.has_z;
        // A `ref` actual overrides all of that with the CANONICAL type-derived
        // shape: a ref formal ALIASES this storage rather than copying through
        // it, and the formal's own shape is fixed by its type (the same rule an
        // aggregate leaf follows), so the two only agree if the actual is shaped
        // by type as well. Safe to override ptr_arg here because a user signal
        // becomes ptr_arg ONLY by being a ref actual — an out / inout actual is
        // passed as a compiler-materialized temp, never the variable itself
        // (lower/Subroutines.cpp's make_arg_temp), and a temp is never a ref actual.
        if (facts_.ref_arg[i]) {
            slot.has_z  = slot.four_state;
            slot.narrow = !slot.four_state;
        }
        // A member reachable through a virtual interface takes the same
        // canonical, type-derived shape a `ref` actual does, and for the same
        // reason: the access is emitted once per definition against a base
        // chosen at run time, so it cannot depend on what the may-z analysis
        // proved about one particular instance's writers (LRM 25.9).
        if (vif_sig_[i]) {
            slot.has_z  = slot.four_state;
            slot.narrow = !slot.four_state;
        }
        slot.slot_ty     = slot_type(slot.width, slot.four_state, slot.has_z, slot.narrow);
        if (ty.kind == Type::Kind::UnpackedArray) {
            slot.is_array   = true;
            slot.elem_count = array_leaf_count(sigs[i].type);
            slot.storage_ty = llvm::ArrayType::get(slot.slot_ty, slot.elem_count);
        } else {
            slot.storage_ty = slot.slot_ty;
        }
    }

    std::vector<llvm::Type*> root_fields;
    std::vector<bool>        claimed(sigs.size(), false);
    for (const InstanceScope& top : mod_.tops()) {
        scope_type(top, claimed, top.name);
    }
    // A definition state type produced by more than one scope occurrence is
    // a reused module: those instances address indirectly (see Anchor).
    for (Anchor& a : anchors_) {
        uint32_t uses = 0;
        for (const Anchor& b : anchors_) {
            uses += b.ty == a.ty ? 1u : 0u;
        }
        a.indirect = uses > 1;
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
                        sf.push_back(int_ty(m.narrow ? m.width : 2 * m.width));
                        if (m.has_z) {
                            m.sh_z_field = static_cast<uint32_t>(sf.size());
                            sf.push_back(int_ty(m.width));
                        }
                    }
                }
                return;
            }
            slot.sh_mm_field = static_cast<uint32_t>(sf.size());
            // A narrow 2-state target shadows a single iW word (no z shadow).
            sf.push_back(int_ty(slot.narrow ? slot.width : 2 * slot.width));
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

// Pointer to a signal's whole storage (its slot struct, or its [N x slot]
// memory), addressed through its owning scope's own state global:
// `@clotho.inst.<hier>.state + the scope-relative field path`. A real
// symbol plus a def-local addend, so the GEP folds to a link-time-constant
// address and the code bytes never depend on whole-design layout. A
// shareable reused-definition body instead loads the base pointer from its
// instance context (one shared body cannot bake N instance symbols). An
// unscoped signal (flat root scope) keeps the direct root GEP.
llvm::Value* Backend::storage_ptr(const SignalSlot& s) {
    std::vector<llvm::Value*> idx;
    idx.push_back(b_.getInt32(0));
    if (s.anchor == SignalSlot::no_anchor) {
        for (const uint32_t f : s.path) {
            idx.push_back(b_.getInt32(f));
        }
        return b_.CreateInBoundsGEP(state_ty_, sym(state_global_), idx);
    }
    Anchor&      a    = anchors_[s.anchor];
    llvm::Value* base = (a.indirect || ctx_override_for(s)) && def_relative_for(s)
                          ? ctx_field(a, 0, llvm::PointerType::getUnqual(ctx_))
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
    // Exactly storage_ptr's base rule: a shared body of a reused definition
    // must load its instance base rather than bake one instance's symbol.
    llvm::Value* base = (a.indirect || ctx_override_for(s)) && def_relative_for(s)
                          ? ctx_field(a, 0, llvm::PointerType::getUnqual(ctx_))
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
// runtime structures def-relatively. True only inside a single-scope group
// of `owner`'s own instance — the population body sharing can cover.
// Everything else (cross-instance fused groups, tasks, commit functions,
// cross-hierarchy references) is unshareable by construction and keeps the
// absolute/folded form, which the optimizer amortizes to one hoisted base:
// a 32-member cross-instance fused NBA thunk measured +23% -O sim with
// unconditional indirection (32 per-instance base loads GVN cannot merge).
// Either form addresses the same storage; this is codegen policy only.
bool Backend::def_relative_for(const SignalSlot& owner) const {
    // A virtual interface access is def-relative by construction: the instance
    // is not known until run time, so there is no absolute form to fold to.
    return ctx_override_for(owner) ||
           (sched_anchor_ != SignalSlot::no_anchor && owner.anchor == sched_anchor_);
}

// `owner` is the signal whose (def-local, for an anchored signal) shadow
// field is addressed. A reused definition's own single-scope group loads
// its instance's shadow base (@clotho.inst.*.shadow); every other context
// folds the sub-struct index through the root base; unscoped signals keep
// the flat root field. Every path loads a base pointer, preserving the
// opacity requirement above.
llvm::Value* Backend::shadow_ptr(const SignalSlot& owner, uint32_t field) {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    if (owner.anchor != SignalSlot::no_anchor) {
        // An NBA target's anchor always carries a shadow struct (the
        // fields were assigned per anchor in build_state_types).
        Anchor&      a    = anchors_[owner.anchor];
        llvm::Value* base = (a.indirect || ctx_override_for(owner)) && def_relative_for(owner)
                              ? ctx_field(a, 1, ptr)
                              : b_.CreateLoad(ptr, anchor_shadow_base(a));
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

// Lazily create the instance context global (see Anchor). The type is
// computed by build_instance_ctx_layout; only instances actually addressed
// def-relatively materialize a global (and a fill in main).
llvm::GlobalVariable* Backend::ensure_ctx(Anchor& a) {
    if (a.ctx_gv == nullptr) {
        a.ctx_gv = new llvm::GlobalVariable(env_->llmod, a.ctx_ty, false,
                                            llvm::GlobalValue::InternalLinkage,
                                            llvm::ConstantAggregateZero::get(a.ctx_ty),
                                            "clotho.inst." + a.name + ".ctx");
    }
    return sym(a.ctx_gv);
}

// A def-relative load of one instance-context field. `ctx_override_` redirects
// it to an instance chosen at run time -- a virtual interface access, whose
// whole point is that the def-local field constants stay while the record they
// are read from is whichever instance the handle names (LRM 25.9).
llvm::Value* Backend::ctx_field(Anchor& a, uint32_t field, llvm::Type* ty) {
    if (ctx_override_ != nullptr) {
        return b_.CreateLoad(ty, b_.CreateInBoundsGEP(a.ctx_ty, ctx_override_,
                                                      { b_.getInt32(0), b_.getInt32(field) }));
    }
    auto* gep = llvm::ConstantExpr::getInBoundsGetElementPtr(
      a.ctx_ty, ensure_ctx(a), llvm::ArrayRef<llvm::Constant*>{ b_.getInt32(0), b_.getInt32(field) });
    return b_.CreateLoad(ty, gep);
}

// The anchor a vif-rooted lvalue's context record describes: its representative
// signal's own scope. no_anchor when the lvalue is not vif-rooted, which makes
// the CtxOverride a no-op.
uint32_t Backend::lvalue_ctx_anchor(const LValue& lv) const {
    return lv.vif_ctx != nullptr && lv.is_signal ? slot_of(lv.sig).anchor : SignalSlot::no_anchor;
}

// Whether the installed instance context (if any) describes `owner`'s scope,
// and may therefore supply its bases.
bool Backend::ctx_override_for(const SignalSlot& owner) const {
    return ctx_override_ != nullptr && owner.anchor == ctx_override_anchor_;
}

// Materialize the group's deferred def-relative context at the current
// insert point (a function entry, so it dominates every use): the sched-flag
// pointer (instance sched base + def-local ordinal) and, when the group can
// register NBA blocks, the instance's block-id base (block_register adds the
// group-local block ordinal). No-op when lower_group chose the absolute
// constants; a dead base load in a group that ends up registering no block
// is swept by the tier-0 dce pass.
void Backend::materialize_group_ctx() {
    if (sched_ptr_ != nullptr) {
        return;
    }
    Anchor&      a    = anchors_[sched_anchor_];
    llvm::Value* base = ctx_field(a, 2, llvm::PointerType::getUnqual(ctx_));
    sched_ptr_ = b_.CreateInBoundsGEP(sched_word_ty(), base, b_.getInt32(sched_ordinal_));
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

// The power-on image of one leaf slot: what an out-of-range read of it must
// yield (LRM 7.4.5 / Table 7-1 -- x for a 4-state element, 0 for 2-state).
llvm::Constant* Backend::slot_default_const(llvm::StructType* slot_ty, uint32_t width, bool narrow, bool has_z) {
    std::vector<llvm::Constant*> fields{ narrow ? zero(width) : x_mm(width) };
    if (has_z) {
        fields.push_back(zero(width));
    }
    return llvm::ConstantStruct::get(slot_ty, fields);
}

// The words a `ref` actual passes (docs/design/data-model.md §3): the aliased
// storage pointer, and — only for a formal bound somewhere to a container
// element (§3.0) — the relocation epoch of the container the place is rooted in
// and that epoch's value now.
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
RefBinding Backend::ref_actual_arg(const LValue& raw) {
    // An associative-array element is a DEFERRED place, and a `ref` is its third
    // consumer alongside Load and Store: it needs the address itself. Resolve it
    // with put_ptr, because LRM 13.5.2 binds the formal to a VARIABLE — there is
    // no variable to alias until the entry exists, so forming the ref creates it,
    // exactly as a mutating method on `a[k]` does.
    const LValue lv = raw.is_assoc ? assoc_elem_lvalue(raw, /*for_write=*/true) : raw;
    RefBinding   rb;
    // One witness per container level, outermost first, each sampled now. A
    // stable place has none; the caller pads to the formal's declared depth with
    // the compile-time zero word so the callee's unrolled check is uniform.
    for (llvm::Value* ep : lv.epoch_chain) {
        rb.epochs.push_back(ep);
        rb.expects.push_back(b_.CreateLoad(b_.getInt64Ty(), ep, "ref.epoch.at.call"));
    }
    rb.slot = ref_actual_ptr(lv);
    return rb;
}

// Push a ref binding's witness pairs into a call's argv, padded to the formal's
// declared `depth`. A call site may bind SHALLOWER than the formal's deepest
// caller (or bind a stable place through a formal some other site nests), and the
// signature is fixed, so the unused levels get the compile-time zero word: it
// never advances, so it never reports stale.
void Backend::push_ref_epochs(std::vector<llvm::Value*>& argv, const RefBinding& rb, uint32_t depth) {
    for (uint32_t lvl = 0; lvl < depth; ++lvl) {
        const bool have = lvl < rb.epochs.size();
        argv.push_back(have ? rb.epochs[lvl] : no_epoch_global());
        argv.push_back(have ? rb.expects[lvl] : b_.getInt64(0));
    }
}

// Pointer to a ref actual's storage: the aliased place the callee holds.
// A whole scalar is its slot pointer; an unpacked-array or container element is
// `&array[idx]`; an aggregate is its image pointer; a managed value is its
// handle pointer.
llvm::Value* Backend::ref_actual_ptr(const LValue& lv) {
    if (lv.is_agg) {
        // An aggregate (or a managed leaf inside one) is addressed by its GEP
        // path; the ABI guarantees the canonical layout on both sides.
        return agg_slot_ptr(lv);
    }
    if (lv.is_string || lv.is_handle) {
        // A managed place: the ref aliases the handle itself, so the callee's
        // deep-copy / mutation lands on the actual.
        return lv.is_string ? lv_string_ptr(lv, StringUse::Place) : lv_handle_ptr(lv, HandleUse::Place);
    }
    const bool whole =
      lv.range_mask == nullptr && is_zero_const(lv.bit_off) && lv.width == lv.store_width;
    if (!whole) {
        throw BackendError{ "ref actual must be a whole variable, element or member" };
    }
    if (lv.array_ty == nullptr) {
        return lv.base;
    }
    llvm::Value* idx = lv.elem_idx != nullptr ? lv.elem_idx : b_.getInt32(0);
    llvm::Value* ptr = b_.CreateInBoundsGEP(lv.array_ty, lv.base, { b_.getInt32(0), idx });
    if (lv.dyn_inrange == nullptr) {
        return ptr;
    }
    // A `ref` formal aliases storage for the whole call, so an out-of-range
    // element index cannot simply be dropped the way a store can -- the
    // callee would hold a live pointer past the array and read and write
    // through it. Redirect to a discard slot, freshly reset to the element
    // default: the callee's reads see what LRM 7.4.5 / Table 7-1 say an
    // invalid element read yields, and its writes go nowhere, which is the
    // no-operation the same clause requires. Mirrors lv_string_ptr's sink
    // redirect for an out-of-range string element.
    //
    // The clause does not spell out `ref` binding to an invalid element, so
    // this is the reading of it that keeps both halves of 7.4.5 true; the
    // alternative (aliasing real storage) is memory corruption.
    auto* sink = entry_alloca(lv.slot_ty, "ref.sink");
    b_.CreateStore(slot_default_const(lv.slot_ty, lv.store_width, lv.narrow, lv.has_z), sink);
    emit_bounds_reports(lv, /*is_write=*/true);
    return b_.CreateSelect(lv.dyn_inrange, ptr, sink);
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

// Compute each instance context's type (see Anchor): shared per definition,
// fields sized from the def-local single-scope group count and wait-bucket
// slots. Runs after fusion grouping and wait-queue sizing, before lowering.
// Also attributes each fusion group to its scope anchor + def-local
// ordinal (the shareable-group identity every def-relative site keys on).
void Backend::build_instance_ctx_layout() {
    const auto all = mod_.processes();
    group_anchor_of_.assign(groups_.size(), SignalSlot::no_anchor);
    group_def_ordinal_.assign(groups_.size(), ~0u);
    group_unit_anchor_.assign(groups_.size(), SignalSlot::no_anchor);
    group_unit_ordinal_.assign(groups_.size(), ~0u);
    std::vector<uint32_t> scope_counter(anchors_.size(), 0);
    std::vector<uint32_t> unit_counter(anchors_.size(), 0);
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const uint32_t sc   = all[groups_[g].front()].scope;
        bool           same = sc != Process::no_scope;
        for (const uint32_t m : groups_[g]) {
            same = same && all[m].scope == sc;
        }
        if (!same || sc >= anchor_of_scope_.size() ||
            anchor_of_scope_[sc] == SignalSlot::no_anchor) {
            continue;
        }
        const uint32_t aid = anchor_of_scope_[sc];
        // Any single-scope group belongs to its definition's emission
        // unit (split mode), whether or not the definition is reused.
        group_unit_anchor_[g]  = aid;
        group_unit_ordinal_[g] = unit_counter[aid]++;
        if (!anchors_[aid].indirect || anchors_[aid].first_proc == ~0u) {
            continue;
        }
        group_anchor_of_[g]   = aid;
        group_def_ordinal_[g] = scope_counter[aid]++;
    }
    // Twin instances see the same def-local group count; size blk fields by
    // the definition-wide maximum so layouts agree even if a member's group
    // list degenerated (they should be equal by construction).
    ankerl::unordered_dense::map<llvm::StructType*, uint32_t> def_groups;
    for (uint32_t a = 0; a < anchors_.size(); ++a) {
        auto& n = def_groups[anchors_[a].ty];
        n       = std::max(n, scope_counter[a]);
    }

    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    auto* i32 = b_.getInt32Ty();
    ankerl::unordered_dense::map<llvm::StructType*, llvm::StructType*> def_ctx;
    for (Anchor& a : anchors_) {
        // A virtual interface addresses its target instance through this
        // record whatever the definition's instance count, so an interface a
        // handle can name needs the layout even when it is unique.
        const bool vif_anchor =
          std::ranges::any_of(anchor_signals_[&a - anchors_.data()], [&](Signal sj) {
              return sj != Signal{ ~0u } && vif_sig_[static_cast<uint32_t>(sj)];
          });
        if (!a.indirect && !vif_anchor) {
            continue;
        }
        a.n_groups     = def_groups[a.ty];
        a.ctx_blk_base = 4;
        uint32_t nwait = 0;
        for (const Signal sj : anchor_signals_[&a - anchors_.data()]) {
            if (sj == Signal{ ~0u }) {
                continue;
            }
            const auto si = static_cast<uint32_t>(sj);
            for (uint32_t e = 0; e < 3; ++e) {
                nwait += wait_slot_sta_[si][e] != no_wait_slot ? 1 : 0;
                nwait += wait_slot_dyn_[si][e] != no_wait_slot ? 2 : 0;
            }
        }
        a.ctx_wait_base = a.ctx_blk_base + a.n_groups;
        const auto [it, fresh] = def_ctx.try_emplace(a.ty, nullptr);
        if (fresh) {
            std::vector<llvm::Type*> f{ ptr, ptr, ptr, i32 };
            f.insert(f.end(), a.n_groups, i32);
            f.insert(f.end(), nwait, ptr);
            it->second = llvm::StructType::create(
              ctx_, f, a.ty->getName().str() + ".ctx");
        }
        a.ctx_ty = it->second;
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
