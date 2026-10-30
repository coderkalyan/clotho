// Shared Backend utilities: cross-unit symbols, integer and plane helpers, runtime declarations.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Definition-unit modules created by the split emission (lower_group loop),
// in creation order. The caller links them alongside the root module.
std::vector<std::unique_ptr<llvm::Module>> Backend::take_unit_modules() {
    return std::move(unit_modules_);
}

void Backend::run() {
    // Whole-design facts, computed once and shared by every stage below
    // (Analysis.h). Must precede build_state_types, which reads the may-z /
    // undriven / ptr-arg bits while laying out signal slots.
    facts_ = analyze(mod_);
    build_state_types();
    assign_event_slots(); // before emit_state_global: an event's power-on value IS its handle
    emit_state_global();  // before any body: storage_ptr resolves through the anchors
    build_rng_global();  // before any body: every call site passes an RNG pointer
    build_wheel_global(); // before any body: a function reads the clock from it
    if (stat_on(config_.stats, Stat::Dedup)) {
        report_dedup_stat();
    }
    build_rng_fns();         // before compute_fusion_groups: gates thunk eligibility
    compute_subscription_order();
    compute_fusion_groups(); // before build_wait_queues: bucket capacities count group subscriptions
    compute_trigger_masks(); // after fusion: colours come from the group event unions
    if (stat_on(config_.stats, Stat::CombFuse)) {
        report_comb_fuse_stat();
    }
    may_finish_ = compute_may_finish(); // before any body: lower_call reads it
    if (stat_on(config_.stats, Stat::Finish)) {
        report_finish_stat();
    }
    design_uses_strobe_   = facts_.uses.strobe;   // gates the Postponed-region drain loop
    design_uses_monitor_  = facts_.uses.monitor;
    design_uses_observed_ = facts_.uses.observed; // gates the Observed-region flush
    design_uses_defer_    = facts_.uses.defer;    // gates the deferred-assertion flushes
    // Gates the remote-`disable` occupant tables, the force-wake and the poll
    // after each suspend inside a targeted region -- and widens the
    // ring-overflow guard, which was fork-only until force-wake existed.
    design_uses_disable_  = facts_.uses.disable;
    // Gates every process-control site (docs/design/process-class.md §9).
    // LRM 24.3 turns it on with no mention of `process` in the source at all:
    // when a program's `initial` procedures have all ended, that program
    // terminates their descendant threads, and that IS the `kill` subtree walk,
    // whose tree is the control blocks. Gated on the design being able to spawn
    // a descendant at all -- with no fork anywhere a program `initial` has
    // none, the walk is provably empty, and a program-only design keeps paying
    // nothing for the blocks.
    design_uses_process_ = facts_.uses.process || (has_program_initial() && design_forks());
    // The reactive region set (LRM 4.4.2.5 - 4.4.2.7): wanted when there is
    // pass/fail code to schedule, a process in Observed that could schedule
    // some, or a program block, whose `initial` procedures run in Reactive
    // (LRM 24.3). Everything it costs sits behind this one flag.
    design_uses_reactive_ = facts_.uses.reactive || has_observed() || has_program() || has_renba();
    if (design_uses_reactive_) {
        // Before any body lowers: the classification decides which region a #0
        // inside reactive code parks in, both queues have to exist ahead of
        // lower_finish, which empties whichever of them were created, and
        // build_shared_home_fns decides the SIGNATURE every subroutine ramp
        // gets (see declare_task), so it has to precede declare_function.
        build_reactive_fns();
        build_program_fns();
        build_shared_home_fns(); // after both: intersects their two closures
        build_program_instances();
        build_program_exit_owners(); // after build_program_instances: seeds from its partition
        ensure_reactive_queue();
        if (reactive_has_delay_) {
            ensure_re_inactive_queue();
        }
    }
    build_vcd_index();       // before build_nba_state: fills vcd_index_ / design_uses_vcd_
    build_wait_queues();
    build_instance_ctx_layout(); // after build_wait_queues: ctx sizes need wait slots
    build_sched_flags();
    build_nba_state();

    // Functions and tasks: declare every one up front (so calls resolve,
    // including recursion and forward references), then lower their bodies. A
    // task is a coroutine (lower_task); a function is a plain call.
    const size_t nf = mod_.function_count();
    // Identify functions by address: the ABI helpers below take a `const
    // Function&` and need its id to ask the analysis about a formal. The
    // function table is a deque, so these addresses are stable.
    fn_ids_.reserve(nf);
    for (uint32_t i = 0; i < nf; ++i) {
        fn_ids_.emplace(&mod_.function(static_cast<FunctionId>(i)), i);
    }
    fn_decls_.assign(nf, nullptr);
    for (uint32_t i = 0; i < nf; ++i) {
        fn_decls_[i] = declare_function(static_cast<FunctionId>(i));
    }
    for (uint32_t i = 0; i < nf; ++i) {
        if (mod_.function(static_cast<FunctionId>(i)).is_task) {
            lower_task(static_cast<FunctionId>(i));
        } else {
            lower_function(static_cast<FunctionId>(i));
        }
    }

    // Lower every process and seed every one at startup. An `initial` runs its
    // body once; an `always` with a sensitivity list immediately subscribes to
    // its signals and suspends (its body runs only when woken). Both kinds are
    // created suspended and enter the active queue together.
    // TODO: an `always` / continuous assign with no sensitivity list (e.g. a
    // continuous `assign`, whose RHS sensitivity is not yet inferred) still
    // runs its body just once — it does not re-trigger on input changes.
    std::vector<llvm::Function*> group_fns;
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const uint32_t ua = group_unit_anchor_[g];
        env_ = config_.split && ua != SignalSlot::no_anchor ? &unit_env(anchors_[ua])
                                                            : root_env_.get();
        group_fns.push_back(lower_group(groups_[g]));
    }
    env_ = root_env_.get();
    if (!shape_sets_.empty()) {
        report_dedup_shapes(group_fns);
    }
    if (config_.dedup && config_.thunk) {
        share_twin_groups(group_fns);
    }
    // Every interface instance a virtual interface can name needs its context
    // record to exist before main fills it, whether or not any body happened
    // to address that instance def-relatively: the handle IS the context
    // address (LRM 25.9).
    for (const uint32_t sc : mod_.vif_scopes) {
        if (sc < anchor_of_scope_.size() && anchor_of_scope_[sc] != SignalSlot::no_anchor) {
            ensure_ctx(anchors_[anchor_of_scope_[sc]]);
        }
    }
    // Blocks are discovered during the lowering above; size the block-commit
    // globals and emit the commit / enqueue functions now, before emit_main
    // wires the NBA-region drain that references them.
    finalize_block_nba();
    emit_main(group_fns);
    // Export thunks come last: they reference @clotho.state (created in
    // emit_main) and every SV function's declaration.
    emit_dpi_exports();
}

// Use a global (defined in whichever unit created it) as an operand in the
// module currently being emitted. Identity while the definition is local —
// always the case with one design-wide unit — otherwise a matching external
// declaration created once per (env, symbol) and memoized in env_->decls.
// Every use of a cached GlobalVariable*/Function* member as an IR operand
// must route through here so the per-definition split can relink it.
llvm::Function* Backend::sym(llvm::Function* f) {
    if (f->getParent() == &env_->llmod) {
        return f;
    }
    auto [it, inserted] = env_->decls.try_emplace(f, nullptr);
    if (inserted) {
        // First cross-unit use: the definition must be visible to the JIT
        // linker, so promote it out of local linkage.
        if (f->hasLocalLinkage()) {
            f->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
        auto* decl = llvm::Function::Create(f->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
                                            f->getName(), env_->llmod);
        // Everything links into one JIT dylib: mark the declaration
        // dso_local so codegen keeps the direct (non-GOT, non-cached)
        // addressing form it uses for local definitions.
        decl->setDSOLocal(true);
        it->second = decl;
    }
    return llvm::cast<llvm::Function>(it->second);
}

llvm::GlobalVariable* Backend::sym(llvm::GlobalVariable* g) {
    if (g->getParent() == &env_->llmod) {
        return g;
    }
    auto [it, inserted] = env_->decls.try_emplace(g, nullptr);
    if (inserted) {
        if (g->hasLocalLinkage()) {
            g->setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
        auto* decl = new llvm::GlobalVariable(env_->llmod, g->getValueType(), g->isConstant(),
                                              llvm::GlobalValue::ExternalLinkage, nullptr, g->getName());
        decl->setAlignment(g->getAlign());
        decl->setDSOLocal(true);
        it->second = decl;
    }
    return llvm::cast<llvm::GlobalVariable>(it->second);
}

// True while emission targets a definition-unit module (split mode);
// root-module code keeps plain immediates for the id families below.
bool Backend::unit_emission() const {
    return env_ != root_env_.get();
}

// A design-numbering integer id, referenced from unit-module code without
// baking the number: a load of a root-defined i32 value global
// (@clotho.idval.<key>), named by hierarchy + def-local ordinal, so the
// unit's bytes stay independent of whole-design numbering (root always
// recompiles and carries the value). Deliberately NOT an absolute-symbol
// relocation: a truncating-ptrtoint constant in operand position measured
// ~+3KB codegen per hot pico thunk -- SelectionDAG (the -O0 fallback for
// blocks FastISel misses) keeps the symbol address live across the whole
// function and spills it, costing more executed instructions per
// activation than this one L1-hot 4-byte load per site (llc A/B on the
// extracted thunk: constexpr 72248 B, instruction-form 72169 B, this load
// form 70992 B, literal floor 69298 B).
llvm::Value* Backend::abs_id_const(const std::string& key, uint32_t value, uint32_t addend) {
    std::string name = "clotho.idval." + key;
    if (addend != 0) {
        name += "+" + std::to_string(addend);
    }
    auto [it, inserted] = idval_gvs_.try_emplace(name, nullptr);
    if (inserted) {
        auto* i32  = b_.getInt32Ty();
        it->second = new llvm::GlobalVariable(root_env_->llmod, i32, /*isConstant=*/true,
                                              llvm::GlobalValue::InternalLinkage,
                                              llvm::ConstantInt::get(i32, value + addend), name);
        it->second->setAlignment(llvm::Align(4));
    } else if (llvm::cast<llvm::ConstantInt>(it->second->getInitializer())->getZExtValue() !=
               value + addend) {
        throw BackendError{ "id value global '" + name + "' bound to two values" };
    }
    return b_.CreateLoad(b_.getInt32Ty(), sym(it->second));
}

// Whether `s`'s design-numbering ids should emit as absolute-symbol
// relocations in the current emission context: unit-module code only, and
// only for scope-owned signals (an unscoped signal referenced from a unit
// keeps the immediate — rare, costs only that unit's cache stability).
bool Backend::abs_id_eligible(const SignalSlot& s) const {
    return config_.split && unit_emission() && s.anchor != SignalSlot::no_anchor;
}

// rt_nba_arr_* buffer index of an array-NBA signal, as an IR value.
llvm::Value* Backend::arr_id_value(Signal sig) {
    const uint32_t    id = arr_idx(sig);
    const SignalSlot& s  = slot_of(sig);
    if (abs_id_eligible(s)) {
        return abs_id_const("arr." + anchors_[s.anchor].name + ".s" + std::to_string(s.path.front()),
                            id, 0);
    }
    return b_.getInt32(id);
}

// rt_nba_arr_* buffer index of an aggregate-NBA signal, as an IR value.
llvm::Value* Backend::agg_id_value(Signal sig) {
    const uint32_t    id = agg_buf_idx(sig);
    const SignalSlot& s  = slot_of(sig);
    if (abs_id_eligible(s)) {
        return abs_id_const("agg." + anchors_[s.anchor].name + ".s" + std::to_string(s.path.front()),
                            id, 0);
    }
    return b_.getInt32(id);
}

// rt_intra_* record-buffer index of an intra-assignment target, as an IR value.
llvm::Value* Backend::intra_id_value(Signal sig) {
    const uint32_t    id = intra_idx(sig);
    const SignalSlot& s  = slot_of(sig);
    if (abs_id_eligible(s)) {
        return abs_id_const("intra." + anchors_[s.anchor].name + ".s" + std::to_string(s.path.front()),
                            id, 0);
    }
    return b_.getInt32(id);
}

// Trigger id of (signal, edge), as an IR value. A dense `<= @e` id for a value
// signal. For a declared event it is RT_TRIG_EVENT | the slot of the object the
// variable names NOW (LRM 15.5.5.1: a control captures the object when it
// executes), which is what emit_event_fire counts down.
llvm::Value* Backend::trigger_id_value(Signal sig, uint32_t edge) {
    if (event_slot_[static_cast<uint32_t>(sig)] != 0) {
        llvm::Value* slot = event_is_mono(sig)
                              ? static_cast<llvm::Value*>(b_.getInt32(event_slot_[static_cast<uint32_t>(sig)]))
                              : b_.CreateTrunc(load_event_handle(sig), b_.getInt32Ty());
        return b_.CreateOr(slot, b_.getInt32(RT_TRIG_EVENT));
    }
    const auto        id = static_cast<uint32_t>(trigger_id(sig, edge));
    const SignalSlot& s  = slot_of(sig);
    if (abs_id_eligible(s)) {
        return abs_id_const("trig." + anchors_[s.anchor].name + ".s" + std::to_string(s.path.front()) +
                              ".e" + std::to_string(edge),
                            id, 0);
    }
    return b_.getInt32(id);
}

// The definition unit's env for an anchored scope, created (with its
// llvm::Module) on first use. Units are keyed by definition state-struct
// identity: twin instances of one definition land in one unit.
ModuleEnv& Backend::unit_env(const Anchor& a) {
    auto [it, inserted] = unit_env_of_.try_emplace(a.ty, nullptr);
    if (inserted) {
        std::string name = a.ty->getName().str();
        if (name.ends_with(".state")) {
            name.resize(name.size() - 6);
        }
        auto m = std::make_unique<llvm::Module>("clotho.def." + name, ctx_);
        m->setTargetTriple(root_env_->llmod.getTargetTriple());
        m->setDataLayout(root_env_->llmod.getDataLayout());
        unit_envs_.push_back(std::make_unique<ModuleEnv>(*m));
        unit_modules_.push_back(std::move(m));
        it->second = unit_envs_.back().get();
    }
    return *it->second;
}

// Every value's width reaches here through the frontend's check against this limit, so the
// integer types built from it fit LLVM's own; the two numbers are one fact.
static_assert(max_integer_bits == static_cast<uint32_t>(llvm::IntegerType::MAX_INT_BITS),
              "ir::max_integer_bits is LLVM's IntegerType::MAX_INT_BITS");

llvm::IntegerType* Backend::int_ty(uint32_t width) const {
    return llvm::IntegerType::get(ctx_, width == 0 ? 1 : width);
}

llvm::ConstantInt* Backend::zero(uint32_t width) const {
    return llvm::ConstantInt::get(int_ty(width), 0);
}

llvm::ConstantInt* Backend::ones(uint32_t width) const {
    return llvm::ConstantInt::get(ctx_, llvm::APInt::getAllOnes(width == 0 ? 1 : width));
}

// The all-X packed i(2W) must|may word: every bit X (must 0, may 1), i.e.
// the high W bits set. Also the must|may half of all-Z.
llvm::ConstantInt* Backend::x_mm(uint32_t width) const {
    return llvm::ConstantInt::get(ctx_, llvm::APInt::getHighBitsSet(2 * width, width));
}

// The power-on z-plane constant of a 4-state slot: all-z for a fully undriven
// net (init_z), the per-bit undriven mask for a partially-driven net, else 0.
llvm::ConstantInt* Backend::init_z_plane(const SignalSlot& s) const {
    if (s.init_z) {
        return ones(s.width);
    }
    if (!s.undriven_mask.empty()) {
        return llvm::ConstantInt::get(ctx_, llvm::APInt(s.width, s.undriven_mask));
    }
    return zero(s.width);
}

// Cast an integer value to width `to` (zero/sign extend or truncate).
llvm::Value* Backend::cast(llvm::Value* v, uint32_t to, bool sign) {
    const auto from = llvm::cast<llvm::IntegerType>(v->getType())->getBitWidth();
    if (from == to) {
        return v;
    }
    if (from > to) {
        return b_.CreateTrunc(v, int_ty(to));
    }
    return sign ? b_.CreateSExt(v, int_ty(to)) : b_.CreateZExt(v, int_ty(to));
}

// Whether a type is a string, or an unpacked array whose leaf element is.
bool Backend::is_string_type(Ref type) const {
    return types::is_string_leaf_array(mod_.pool, type);
}

// The scalar leaf type of a (possibly nested) unpacked-array type.
Ref Backend::leaf_type(Ref type) const {
    return types::leaf_type(mod_.pool, type);
}

// Total leaf-element count of an unpacked-array type (product of all
// dimensions); 1 for a scalar leaf.
uint32_t Backend::array_leaf_count(Ref type) const {
    return types::array_leaf_count(mod_.pool, type);
}

// Whether a type contains an unpacked struct anywhere (at any array/struct
// nesting depth). Such a signal is routed through the recursive aggregate
// storage / addressing / init path, not the flat scalar-or-array fast paths.
bool Backend::contains_struct(Ref type) const {
    return types::contains_struct(mod_.pool, type);
}

// Whether a type is an unpacked aggregate passed / stored via the recursive
// canonical (never may-z-narrowed) storage mirror: any fixed unpacked array,
// or an unpacked struct. Broader than contains_struct (which is false for a
// plain scalar-element array) — the function-call ABI and frame-local layout
// use this so a whole array passes by pointer with a canonical layout, memcpy
// -compatible with the caller's canonical actual (a flat module array may be
// z-elided narrower, so the caller materializes a canonical temp first).
bool Backend::is_agg_abi(Ref type) const {
    return types::is_agg_abi(mod_.pool, type);
}

// Whether a type takes the recursive aggregate storage mirror: it contains
// an unpacked struct, or a fixed array dimension over a heap-handle
// (dynamic array / queue) element whose per-cell init/deinit/copy only the
// recursive walks provide. Flat string arrays keep the dedicated
// string-array path (is_string_type is tested first at both call sites).
bool Backend::needs_agg_storage(Ref type) const {
    return types::needs_agg_storage(mod_.pool, type);
}

// Whether a type owns heap anywhere (string / dynamic array / queue leaf
// at any nesting depth), so its aggregate storage needs a deinit walk.
bool Backend::has_heap_leaf(Ref type) const {
    return types::has_heap_leaf(mod_.pool, type);
}

// Element count of one unpacked-array dimension.
uint32_t Backend::array_dim(const Type& t) const {
    return types::array_dim(t);
}

// The packed width W of a value type (the ==0 -> 1 scalar rule).
uint32_t Backend::width_of(Ref type) const {
    const uint32_t w = mod_.pool.layout(type).width;
    return w == 0 ? 1 : w;
}

// Declare (or fetch) an llvm.coro.* intrinsic. `tys` supplies the overload
// type for the few overloaded ones (e.g. coro.size -> i64).
llvm::Function* Backend::coro_intr(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> tys) {
    return llvm::Intrinsic::getOrInsertDeclaration(&env_->llmod, id, tys);
}

// An alloca placed at the top of the current function's entry block (not the
// current insert point), so a scratch buffer used inside a loop body is
// allocated once per activation rather than growing the stack per iteration.
llvm::AllocaInst* Backend::entry_alloca(llvm::Type* ty, const llvm::Twine& name) {
    auto*             fn = b_.GetInsertBlock()->getParent();
    auto&             eb = fn->getEntryBlock();
    llvm::IRBuilder<> ab(&eb, eb.getFirstInsertionPt());
    return ab.CreateAlloca(ty, nullptr, name);
}

// Where an instruction came from, for a sanitizer report baked into the
// emitted code. A caller with no SourceContext (the tests) leaves the
// resolver empty and gets "<unknown>" with no snippet, rather than a blank,
// so a report is never silently unattributed.
SourceText Backend::loc_text(Index i) const {
    if (!resolve_) {
        return SourceText{ .where = "<unknown>" };
    }
    // `end` is SourceLoc{0} unless the frontend recorded an extent, which
    // the resolver reads as "no width -- a bare caret".
    return resolve_(SourceSpan{ ir_->loc(i), ir_->end(i) });
}

void Backend::store_field(llvm::StructType* sty, llvm::Value* elt, unsigned idx, llvm::Value* v) {
    b_.CreateStore(v, b_.CreateStructGEP(sty, elt, idx));
}

uint32_t Backend::bits_of(llvm::Value* v) {
    return llvm::cast<llvm::IntegerType>(v->getType())->getBitWidth();
}

// Value width W of a plane. A 2-state value's mm IS the iW value; a 4-state
// value packs must|may into i(2W).
uint32_t Backend::width_of(Plane a) {
    return a.two ? bits_of(a.mm) : bits_of(a.mm) / 2;
}

// The must (low W) and may (high W) halves of a plane. For a 2-state value
// both are the value itself (must == may, no unknown bits).
llvm::Value* Backend::must_of(Plane a) {
    if (a.mm == nullptr) {
        // Depth guard: a real value reached an integer-plane lowering; the
        // frontend must convert reals explicitly (float tags), never route
        // them through integer ops.
        throw BackendError{ "internal: integer plane of a real value" };
    }
    return a.two ? a.mm : b_.CreateTrunc(a.mm, int_ty(width_of(a)));
}

llvm::Value* Backend::may_of(Plane a) {
    if (a.two) {
        return a.mm;
    }
    const uint32_t w = width_of(a);
    return b_.CreateTrunc(b_.CreateLShr(a.mm, w), int_ty(w));
}

// The z plane, materialized as a zero constant for a 2-state (never-z) value.
llvm::Value* Backend::z_of(Plane a) {
    return a.two ? zero(width_of(a)) : a.z;
}

// The result of a whole-aggregate compare in the encoding of the domain its IR
// type has: lower_agg_eq builds a 1-bit packed answer for an associative-array
// compare whatever the leaves are, while the type of an `==` over 2-state leaves
// is 2-state. This converts a producer's result; it is not a storage or ABI
// boundary, and an operator never converts its operands.
Plane Backend::in_type_domain(Plane p, ValueType t) {
    if (t.is_string || t.is_real) {
        return p;
    }
    if (t.four_state) {
        assert(!p.two && "a 4-state value is read from packed planes");

        return p;
    }

    return p.two ? p : two_val(must_of(p));
}

// A value that is never x or z, in the encoding of the domain its IR type has.
Plane Backend::known_plane(llvm::Value* v, bool four_state) {
    return four_state ? packed(v, v, zero(v->getType()->getIntegerBitWidth())) : two_val(v);
}

// Wrap a single iW value as a 2-state Plane.
Plane Backend::two_val(llvm::Value* v) {
    return { v, nullptr, true };
}

// Lift a 2-state value into the 4-state must|may encoding (must == may == v,
// z = 0); a no-op on an already-4-state value. Used at storage / ABI sinks
// that still consume the packed i(2W) word.
Plane Backend::to_four(Plane a) {
    if (!a.two) {
        return a;
    }
    const uint32_t w = bits_of(a.mm);
    return packed(a.mm, a.mm, zero(w));
}

// Pack iW must + iW may (+ iW z) into a Plane (mm is i(2W)).
Plane Backend::packed(llvm::Value* must, llvm::Value* may, llvm::Value* z) {
    const uint32_t w    = bits_of(must);
    auto*          wide = int_ty(2 * w);
    llvm::Value*   mm   = b_.CreateOr(b_.CreateZExt(must, wide), b_.CreateShl(b_.CreateZExt(may, wide), w));
    return { mm, z };
}

// i1: does this value contain any X/Z bit? Unknown bits are exactly must!=may.
// A 2-state value never has any.
llvm::Value* Backend::any_unknown(Plane a) {
    if (a.two) {
        return b_.getFalse();
    }
    llvm::Value* u = b_.CreateXor(must_of(a), may_of(a));
    return b_.CreateICmpNE(u, llvm::ConstantInt::get(u->getType(), 0));
}

// Pack an i1 must + i1 may into a width-1 Plane (z cleared).
Plane Backend::bit_plane(llvm::Value* must_i1, llvm::Value* may_i1) {
    return packed(must_i1, may_i1, zero(1));
}

// Cast all planes to width `to`. No-op when already that width — avoids a
// pointless unpack/repack of the packed mm. Preserves the value's domain: a
// 2-state value stays a single iW.
Plane Backend::cast_plane(Plane a, uint32_t to, bool sign) {
    if (width_of(a) == to) {
        return a;
    }
    if (a.two) {
        return two_val(cast(a.mm, to, sign));
    }
    return packed(cast(must_of(a), to, sign), cast(may_of(a), to, sign), cast(a.z, to, sign));
}

// A conversion instruction (ZExt, SExt, Trunc, ToFour, ToTwo). The extension is
// the opcode's: nothing here reads a sign from a type.
//
// ToTwo follows LRM 1800-2023 6.3.1: a 4-state value assigned to a 2-state target
// drops x/z to 0, which is exactly the must plane (must is 0 for both x and z).
// ToFour duplicates the value into both planes with z = 0.
Plane Backend::lower_conversion(Inst::Tag tag, Plane a, ValueType to) {
    switch (tag) {
        case Inst::Tag::ZExt:
        case Inst::Tag::Trunc:
            return cast_plane(a, to.width, false);
        case Inst::Tag::SExt:
            return cast_plane(a, to.width, true);
        case Inst::Tag::ToFour:
            return to_four(a);
        default: // ToTwo
            return a.two ? a : two_val(must_of(a));
    }
}

// Combined per-bit X/Z mask (iW) of two same-width operands, tested once
// for the pair. The X mask of a packed value is `must ^ may`, which sits
// in the low W bits of `mm ^ (mm >> W)`. X-ness is nonlinear, so each
// operand's mask is taken separately before OR-ing (merging the mm words
// first would be wrong).
llvm::Value* Backend::xz_mask(Plane a, Plane b, uint32_t w) {
    llvm::Value* xa = b_.CreateXor(a.mm, b_.CreateLShr(a.mm, w));
    llvm::Value* xb = b_.CreateXor(b.mm, b_.CreateLShr(b.mm, w));
    return b_.CreateTrunc(b_.CreateOr(xa, xb), int_ty(w));
}

} // namespace clotho::ir::backend
