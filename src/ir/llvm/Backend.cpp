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
    build_sched_flags();     // before compute_fusion_groups: a group is a list of def-local process ordinals
    compute_subscription_order();
    compute_fusion_groups(); // before build_wait_queues: bucket capacities count schedule-group subscriptions
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
    // A program's port copy, `assign` or net resolver (LRM 24.3.1) is what the
    // reactive continuous ring and its tag exist for, so a design with only
    // `initial`s or assertions in Reactive emits the two-ring scheduler it always did.
    design_uses_reactive_cont_ = has_reactive_continuous();
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
        if (design_uses_reactive_cont_) {
            ensure_reactive_cont_queue();
        }
        if (reactive_has_delay_) {
            ensure_re_inactive_queue();
        }
    }
    build_vcd_index();       // before build_nba_state: fills vcd_index_ / design_uses_vcd_
    build_wait_queues();
    build_instance_ctx_layout(); // after build_wait_queues (ctx sizes need wait slots) and build_sched_flags
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
        if (mod_.function(static_cast<FunctionId>(i)).scope != Process::no_scope) {
            instance_fns_.insert(fn_decls_[i]);
        }
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
    ankerl::unordered_dense::map<uint32_t, uint32_t> unit_instances;
    for (const Anchor& a : anchors_) {
        if (a.unit != InstanceScope::no_unit) {
            ++unit_instances[a.unit];
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> shares; // (instance, the instance whose code it runs)
    // The first instance lowered for each (unit, group, code key): thunk groups
    // of the other instances run its function with their own record.
    std::map<std::tuple<uint32_t, uint32_t, std::vector<uint64_t>>, SharedGroup> once;
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const uint32_t ua = group_unit_anchor_[g];
        env_ = config_.split && ua != SignalSlot::no_anchor ? &unit_env(anchors_[ua])
                                                            : root_env_.get();
        const bool candidate = config_.dedup && ua != SignalSlot::no_anchor &&
                               anchors_[ua].unit != InstanceScope::no_unit;
        if (!candidate) {
            group_fns.push_back(lower_group(groups_[g]));
            continue;
        }
        auto key = std::make_tuple(anchors_[ua].unit, group_unit_ordinal_[g], code_key(ua));
        if (const auto it = once.find(key); it != once.end()) {
            // Another instance of the unit lowered this group already: run its
            // function with this instance's record, which needs the same
            // bindings filled, and NBA blocks of its own signals.
            SharedGroup& rep = it->second;
            if (config_.verify_shared_code) {
                verify_shared_group(g, rep);
            }
            rep.fn->addFnAttr(llvm::Attribute::NoInline);
            group_fns.push_back(rep.fn);
            shares.emplace_back(ua, group_unit_anchor_[rep.g]);
            const auto base = static_cast<uint32_t>(block_commits_.size());
            for (BlockCommit& bc : shared_blocks(rep, ua)) {
                block_commits_.push_back(std::move(bc));
            }
            for (auto& fill : shared_fills(rep, ua, base)) {
                ctx_blk_fills_.push_back(std::move(fill));
            }
            continue;
        }
        const auto block_lo = static_cast<uint32_t>(block_commits_.size());
        const auto fill_lo  = ctx_blk_fills_.size();
        share_active_       = true;
        taint_why_          = nullptr;
        llvm::Function* fn  = lower_group(groups_[g]);
        share_active_       = false;
        group_fns.push_back(fn);
        if (taint_why_ == nullptr) {
            once.emplace(std::move(key), SharedGroup{ g, fn, block_lo, static_cast<uint32_t>(block_commits_.size()),
                                                      fill_lo, ctx_blk_fills_.size(), {} });
        } else if (unit_instances[anchors_[ua].unit] >= 2) {
            ++unshared_[taint_why_];
        }
    }
    // An instance that runs another's code reads the bindings that code reads.
    for (const auto& [ua, ra] : shares) {
        Anchor& a = anchors_[ua];
        a.bind_used.resize(a.externs.size(), 0);
        for (size_t k = 0; k < anchors_[ra].bind_used.size(); ++k) {
            a.bind_used[k] |= anchors_[ra].bind_used[k];
        }
    }
    env_ = root_env_.get();
    if (!shape_sets_.empty()) {
        report_dedup_shapes(group_fns);
    }
    if (stat_on(config_.stats, Stat::UnitCode)) {
        report_unit_code(group_fns);
    }
    // What the scheduler holds: each coroutine group's ramp, and for every
    // thunk schedule group the entry that runs its rows.
    const std::vector<llvm::Function*> sched_fns = emit_sched_entries(group_fns);
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
    emit_main(sched_fns);
    // Export thunks come last: they reference @clotho.state (created in
    // emit_main) and every SV function's declaration.
    emit_dpi_exports();
    // The scratch modules of the verification (see verify_modules_): nothing
    // below run() reads what lowering cached in them.
    verify_envs_.clear();
    verify_modules_.clear();
}

// Use a global (defined in whichever unit created it) as an operand in the
// module currently being emitted. Identity while the definition is local —
// always the case with one design-wide unit — otherwise a matching external
// declaration created once per (env, symbol) and memoized in env_->decls.
// Every use of a cached GlobalVariable*/Function* member as an IR operand
// must route through here so the per-definition split can relink it.
llvm::Function* Backend::sym(llvm::Function* f) {
    if (share_active_ && instance_fns_.contains(f)) {
        taint("calls a function copy of its instance");
    }
    if (f->getParent() == &env_->llmod) {
        return f;
    }
    auto [it, inserted] = env_->decls.try_emplace(f, nullptr);
    if (inserted) {
        // First cross-unit use: the definition must be visible to the JIT
        // linker, so promote it out of local linkage. Not for a scratch module
        // (verify_shared_group): it is never linked, and the definition is the
        // real modules' business.
        if (f->hasLocalLinkage() && !verifying_) {
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
    // A global of one instance (its state, record, flags, shadow): shared code
    // names it only through a record.
    if (share_active_ && g->getName().starts_with("clotho.inst.")) {
        taint("names a symbol of one instance");
    }
    if (g->getParent() == &env_->llmod) {
        return g;
    }
    auto [it, inserted] = env_->decls.try_emplace(g, nullptr);
    if (inserted) {
        if (g->hasLocalLinkage() && !verifying_) {
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

// A design-numbering id the code being lowered uses, as an IR value. The ids of
// a signal's instance sit in that instance's record (assign_id_fields), so the
// code that uses one names no number and the unit's code is the same whichever
// instance it runs for: the own instance's record, or the record of the
// instance a binding or an installed context names, at the signal's def-local
// field. Where no record describes the signal -- code outside a group, a signal
// of no instance -- the id is the immediate it has always been.
llvm::Value* Backend::id_value(Signal sig, IdFamily family, uint32_t id) {
    const SignalSlot& s = slot_of(sig);
    if (s.anchor != SignalSlot::no_anchor && !id_ord_.empty()) {
        const uint32_t ord = id_ord_[static_cast<uint32_t>(sig)][static_cast<uint32_t>(family)];
        if (ord != no_id) {
            Anchor& a = anchors_[s.anchor];
            if (def_relative_for(s)) {
                return ctx_field(a, a.ctx_id_base + ord, b_.getInt32Ty());
            }
            if (llvm::Value* rec = bound_record(s); rec != nullptr) {
                return b_.CreateLoad(
                  b_.getInt32Ty(),
                  b_.CreateInBoundsGEP(a.ctx_ty, rec, { b_.getInt32(0), b_.getInt32(a.ctx_id_base + ord) }));
            }
        }
    }
    return b_.getInt32(id);
}

// rt_nba_arr_* buffer index of an array-NBA signal, as an IR value.
llvm::Value* Backend::arr_id_value(Signal sig) {
    return id_value(sig, IdFamily::Arr, arr_idx(sig));
}

// rt_nba_arr_* buffer index of an aggregate-NBA signal, as an IR value.
llvm::Value* Backend::agg_id_value(Signal sig) {
    return id_value(sig, IdFamily::Agg, agg_buf_idx(sig));
}

// rt_nba_arr_* buffer index of a whole-container NBA signal, as an IR value.
llvm::Value* Backend::handle_id_value(Signal sig) {
    return id_value(sig, IdFamily::Handle, handle_buf_idx(sig));
}

// rt_nba_arr_* buffer index of a whole-string NBA signal, as an IR value.
llvm::Value* Backend::str_id_value(Signal sig) {
    return id_value(sig, IdFamily::Str, str_buf_idx(sig));
}

// rt_intra_* record-buffer index of an intra-assignment target, as an IR value.
llvm::Value* Backend::intra_id_value(Signal sig) {
    return id_value(sig, IdFamily::Intra, intra_idx(sig));
}

// Trigger id of (signal, edge), as an IR value. A dense `<= @e` id for a value
// signal. For a declared event it is RT_TRIG_EVENT | the slot of the object the
// variable names NOW (LRM 15.5.5.1: a control captures the object when it
// executes), which is what emit_event_fire counts down.
llvm::Value* Backend::trigger_id_value(Signal sig, uint32_t edge) {
    if (event_slot_[static_cast<uint32_t>(sig)] != 0) {
        llvm::Value* slot = event_slot_value(sig);
        return b_.CreateOr(slot, b_.getInt32(RT_TRIG_EVENT));
    }
    return id_value(sig, static_cast<IdFamily>(static_cast<uint32_t>(IdFamily::TrigPos) + edge),
                    static_cast<uint32_t>(trigger_id(sig, edge)));
}

// A lowering for sharing found something it cannot share (see `taint`).
void Backend::taint(const char* why) {
    if (share_active_ && taint_why_ == nullptr) {
        taint_why_ = why;
    }
}

// What decides the code of the groups of the instance of `anchor`, besides the
// unit: for each binding, which declaration stands for it (declarations that
// name one signal in one instance are one binding), and the owner definition and
// def-local position of the signal it names, whose layout the code addresses
// the owner's record and wake buckets by. Bindings that name the same member of
// the same owner (a child's port) agree in every instance; a port the parent
// binds to a net of its own choosing (an inout, a driver slot) does not. An
// event port is keyed by its declaration alone: the code reads the event's
// object from the handle in its storage and addresses nothing of the owner's
// record or buckets, so instances bound to different events of their parents
// still share.
std::vector<uint64_t> Backend::code_key(uint32_t anchor) const {
    const Anchor&         a = anchors_[anchor];
    std::vector<uint64_t> key;
    key.reserve(2 * a.externs.size());
    for (uint32_t k = 0; k < a.externs.size(); ++k) {
        const SignalSlot& s = slot_of(a.externs[k]);
        key.push_back(a.bind_of.at(static_cast<uint32_t>(a.externs[k])));
        if (mod_.signal(a.externs[k]).is_event) {
            continue;
        }
        key.push_back(s.anchor == SignalSlot::no_anchor
                        ? ~uint64_t{ 0 }
                        : reinterpret_cast<uintptr_t>(anchors_[s.anchor].ctx_ty));
        key.push_back(s.path.empty() ? ~uint64_t{ 0 } : s.path.back());
    }
    return key;
}

// The NBA blocks an instance registers when it runs the code `rep` lowered: the
// same blocks, of the signals that sit at the same def-local fields of its own
// instance.
std::vector<Backend::BlockCommit> Backend::shared_blocks(const SharedGroup& rep, uint32_t anchor) const {
    std::vector<BlockCommit> blocks;
    blocks.reserve(rep.block_hi - rep.block_lo);
    for (uint32_t b = rep.block_lo; b < rep.block_hi; ++b) {
        BlockCommit bc;
        for (const BlockSig& bs : block_commits_[b].sigs) {
            const uint32_t field = slots_[static_cast<uint32_t>(bs.sig)].path.back();
            bc.sigs.push_back({ anchor_signals_[anchor][field], bs.mask, bs.dynamic });
        }
        blocks.push_back(std::move(bc));
    }
    return blocks;
}

// The record fills of those blocks' first ids, for an instance whose blocks
// start at id `base`: the field of the same def-local process ordinal, in the
// instance's own record.
std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> Backend::shared_fills(const SharedGroup& rep, uint32_t anchor,
                                                                            uint32_t base) const {
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> fills;
    fills.reserve(rep.fill_hi - rep.fill_lo);
    for (size_t f = rep.fill_lo; f < rep.fill_hi; ++f) {
        const auto& fill = ctx_blk_fills_[f];
        fills.emplace_back(anchor, std::get<1>(fill), base + (std::get<2>(fill) - rep.block_lo));
    }
    return fills;
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
uint32_t Backend::width_of(Plane a, bool four) {
    return four ? bits_of(a.mm) / 2 : bits_of(a.mm);
}

// The must (low W) and may (high W) halves of a plane. For a 2-state value
// both are the value itself (must == may, no unknown bits).
llvm::Value* Backend::must_of(Plane a, bool four) {
    if (a.mm == nullptr) {
        // Depth guard: a real value reached an integer-plane lowering; the
        // frontend must convert reals explicitly (float tags), never route
        // them through integer ops.
        throw BackendError{ "internal: integer plane of a real value" };
    }
    return four ? b_.CreateTrunc(a.mm, int_ty(width_of(a, four))) : a.mm;
}

// The must half of the value instruction `i` produced, in the domain of its IR type.
llvm::Value* Backend::must_of(Index i) {
    return must_of(value_of(i), type_of(i).four_state);
}

llvm::Value* Backend::may_of(Plane a, bool four) {
    if (!four) {
        return a.mm;
    }
    const uint32_t w = width_of(a, four);
    return b_.CreateTrunc(b_.CreateLShr(a.mm, w), int_ty(w));
}

// The z plane, materialized as a zero constant for a 2-state (never-z) value.
llvm::Value* Backend::z_of(Plane a, bool four) {
    return four ? a.z : zero(width_of(a, four));
}

// A value that is never x or z, in the encoding of the domain its IR type has.
Plane Backend::known_plane(llvm::Value* v, bool four_state) {
    return four_state ? packed(v, v, zero(v->getType()->getIntegerBitWidth())) : two_val(v);
}

// Wrap a single iW value as a 2-state Plane.
Plane Backend::two_val(llvm::Value* v) {
    return Plane{ .mm = v };
}

// The 4-state must|may encoding of `a` (must == may == v, z = 0) when it is
// 2-state; `a` itself when `four`. Used where an operator or a storage / ABI sink
// consumes the packed i(2W) word.
Plane Backend::to_four(Plane a, bool four) {
    if (four) {
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
llvm::Value* Backend::any_unknown(Plane a, bool four) {
    if (!four) {
        return b_.getFalse();
    }
    llvm::Value* u = b_.CreateXor(must_of(a, four), may_of(a, four));
    return b_.CreateICmpNE(u, llvm::ConstantInt::get(u->getType(), 0));
}

llvm::Value* Backend::any_unknown(Index i) {
    return any_unknown(value_of(i), type_of(i).four_state);
}

// Pack an i1 must + i1 may into a width-1 Plane (z cleared).
Plane Backend::bit_plane(llvm::Value* must_i1, llvm::Value* may_i1) {
    return packed(must_i1, may_i1, zero(1));
}

// Cast all planes to width `to`. No-op when already that width — avoids a
// pointless unpack/repack of the packed mm. Preserves the value's domain: a
// 2-state value stays a single iW.
Plane Backend::cast_plane(Plane a, bool four, uint32_t to, bool sign) {
    if (width_of(a, four) == to) {
        return a;
    }
    if (!four) {
        return two_val(cast(a.mm, to, sign));
    }
    return packed(cast(must_of(a, four), to, sign), cast(may_of(a, four), to, sign), cast(a.z, to, sign));
}

// A conversion instruction (ZExt, SExt, Trunc, ToFour, ToTwo) from the domain
// `from` to `to`. The extension is the opcode's: nothing here reads a sign from a type.
//
// ToTwo follows LRM 1800-2023 6.3.1: a 4-state value assigned to a 2-state target
// drops x/z to 0, which is exactly the must plane (must is 0 for both x and z).
// ToFour duplicates the value into both planes with z = 0.
Plane Backend::lower_conversion(Inst::Tag tag, Plane a, ValueType from, ValueType to) {
    switch (tag) {
        case Inst::Tag::ZExt:
        case Inst::Tag::Trunc:
            return cast_plane(a, from.four_state, to.width, false);
        case Inst::Tag::SExt:
            return cast_plane(a, from.four_state, to.width, true);
        case Inst::Tag::ToFour:
            return to_four(a, from.four_state);
        default: // ToTwo
            return from.four_state ? two_val(must_of(a, true)) : a;
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
