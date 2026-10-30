// Module deduplication: twin-group sharing and the dedup and fusion statistics.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// Comb-fusion coverage probe: verdict histogram over Always processes plus
// the count V1 fusion left in singleton groups (the comb-fusion
// opportunity). Measurement only, printed under `--stat=comb-fuse`; the
// grouping itself is not landed.
// Module-deduplication detector (`--stat=dedup`): canonicalize each
// attributed process body to definition-relative form and report how many
// bodies across instances of reused definitions are byte-identical — the
// shareable population one compiled body per definition would cover, and
// what blocks the rest. Read-only; sharing itself is future work.
//
// Canonical form: every payload-level Signal reference under the process's
// home instance subtree is rewritten to its def-relative field path; a
// reference outside the subtree stays absolute (an unscoped/package signal
// is the same signal for every instance, so twins still match on it; a
// cross-instance hierarchical reference differs per instance and honestly
// blocks matching). V1 keeps the extra buffer raw, so a body carrying a
// signal inside an extra record (Wait / StoreNbEvent / ReadMem / FreadMem /
// EventTriggerNbDelayed) or a FunctionId (per-instance function copies)
// hashes instance-specific — counted as blockers, not miscounted as twins.
void Backend::report_dedup_stat() {
    struct Key {
        llvm::StructType* ty;
        uint64_t          hash;
        bool              operator==(const Key&) const = default;
    };
    struct KeyHash {
        uint64_t operator()(const Key& k) const {
            return ankerl::unordered_dense::detail::wyhash::hash(
              reinterpret_cast<uintptr_t>(k.ty) ^ k.hash);
        }
    };

    // Instantiation count per definition state type (the anchors' reuse rule).
    ankerl::unordered_dense::map<llvm::StructType*, uint32_t> ty_uses;
    for (const Anchor& a : anchors_) {
        ty_uses[a.ty]++;
    }

    const auto all = mod_.processes();
    uint64_t attributed = 0, eligible = 0, covered = 0, blocked_call = 0, blocked_extra_sig = 0,
             blocked_xhier = 0;
    ankerl::unordered_dense::map<Key, uint32_t, KeyHash> twins;
    std::vector<std::pair<uint32_t, Key>>                proc_keys;
    std::vector<uint8_t>                                 buf;
    for (const Process& p : all) {
        if (p.scope == Process::no_scope || p.scope >= anchor_of_scope_.size() ||
            anchor_of_scope_[p.scope] == SignalSlot::no_anchor) {
            continue;
        }
        ++attributed;
        const Anchor& home = anchors_[anchor_of_scope_[p.scope]];
        if (ty_uses[home.ty] < 2) {
            continue; // unique definition: nothing to share with
        }
        ++eligible;

        buf.clear();
        const auto put = [&](const void* d, size_t n) {
            const auto* b = static_cast<const uint8_t*>(d);
            buf.insert(buf.end(), b, b + n);
        };
        const auto put32 = [&](uint32_t v) { put(&v, 4); };
        bool       xhier = false;
        const auto canon_signal = [&](Signal sig) {
            const SignalSlot& s = slot_of(sig);
            // Own-scope signal: canonical by its def-local field path.
            // (Scopes no longer nest, so a descendant instance's signal is
            // instance-specific like any other cross-hierarchy reference.)
            if (s.anchor != SignalSlot::no_anchor &&
                &anchors_[s.anchor] == &home) {
                put32(1u);
                for (const uint32_t f : s.path) {
                    put32(f);
                }
                return;
            }
            // Unscoped/package signal: identical for every instance, still
            // shareable. A signal under another instance: instance-specific.
            xhier = xhier || s.anchor != SignalSlot::no_anchor;
            put32(2u);
            put32(static_cast<uint32_t>(sig));
        };

        put32(static_cast<uint32_t>(p.activation));
        put32(p.self_start ? 1u : 0u);
        put32(p.starts_after_procedures ? 1u : 0u);
        for (const WaitEvent& w : p.wait_events) {
            canon_signal(w.signal);
            put32(static_cast<uint32_t>(w.edge));
        }
        bool      calls = false, extra_sig = false;
        const Ir& ir    = p.ir;
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto tag = ir.tag(static_cast<Index>(k));
            put(&tag, 1);
            const Inst::Payload& pl = ir.payload(static_cast<Index>(k));
            if (payload_kind(tag) == Inst::PayloadKind::Signal) {
                canon_signal(pl.signal);
            } else {
                put(&pl, sizeof(pl));
            }
            calls = calls || tag == Inst::Tag::Call || tag == Inst::Tag::CallVirtual ||
                    tag == Inst::Tag::TaskCall || tag == Inst::Tag::TaskCallVirtual;
            extra_sig = extra_sig || tag == Inst::Tag::Wait || tag == Inst::Tag::StoreNbEvent ||
                        tag == Inst::Tag::ReadMem || tag == Inst::Tag::FreadMem ||
                        tag == Inst::Tag::EventTriggerNbDelayed;
        }
        // Raw extra buffer: region slices, operand runs, packed strings —
        // body-local and deterministic, except the embedded-Signal records
        // flagged above.
        const std::span<const uint32_t> extra = ir.extra_raw();
        put(extra.data(), extra.size() * sizeof(uint32_t));
        blocked_call += calls ? 1 : 0;
        blocked_extra_sig += extra_sig ? 1 : 0;
        blocked_xhier += xhier ? 1 : 0;
        const Key key{ home.ty,
                       ankerl::unordered_dense::detail::wyhash::hash(buf.data(), buf.size()) };
        twins[key]++;
        proc_keys.emplace_back(static_cast<uint32_t>(&p - all.data()), key);
    }

    uint64_t groups2 = 0;
    for (const auto& [k, n] : twins) {
        if (n >= 2) {
            ++groups2;
            covered += n;
        }
    }
    // Shape mode (`--stat=dedup-sets`): remember the twin sets so
    // report_dedup_shapes can compare the lowered LLVM functions after the
    // groups exist.
    if (stat_on(config_.stats, Stat::DedupSets)) {
        ankerl::unordered_dense::map<Key, uint32_t, KeyHash> set_of;
        for (const auto& [proc, key] : proc_keys) {
            if (twins[key] < 2) {
                continue;
            }
            const auto [it, fresh] = set_of.try_emplace(
              key, static_cast<uint32_t>(shape_sets_.size()));
            if (fresh) {
                shape_sets_.push_back({});
            }
            shape_sets_[it->second].push_back(proc);
        }
    }
    std::fprintf(stderr,
                 "dedup: %llu/%llu attributed procs in reused defs; %llu shareable in %llu "
                 "twin groups (%.1f%%); instance-specific hints: call=%llu extra-sig=%llu "
                 "xhier=%llu\n",
                 (unsigned long long)eligible, (unsigned long long)attributed,
                 (unsigned long long)covered, (unsigned long long)groups2,
                 eligible != 0 ? 100.0 * static_cast<double>(covered) / static_cast<double>(eligible)
                               : 0.0,
                 (unsigned long long)blocked_call, (unsigned long long)blocked_extra_sig,
                 (unsigned long long)blocked_xhier);
}

// Shape-equality proof (`--stat=dedup-sets`): for every twin set the
// detector found, print each member's lowered LLVM function and compare
// them after normalizing the only legitimately instance-specific tokens —
// the function's own name and the @clotho.inst.* symbols (numbered by
// first occurrence per function, so two distinct symbols at one position
// versus one reused symbol still differ). A remaining mismatch is a
// per-instance bake the def-relative families missed; sharing must not
// proceed past a DIFF. Only singleton fusion groups compare — a fused
// function mixes members, so per-process comparison is meaningless there.
std::string Backend::normalize_shape(llvm::Function* fn, llvm::ModuleSlotTracker* mst) {
    std::string raw;
    {
        llvm::raw_string_ostream os(raw);
        if (mst != nullptr) {
            // Function::print(OS, AAW*) hides Value's slot-tracker overload.
            fn->Value::print(os, *mst);
        } else {
            fn->print(os);
        }
    }
    const auto ident = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '$' || c == '-';
    };
    ankerl::unordered_dense::map<std::string, uint32_t> inst_ids;
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '@') {
            out += raw[i++];
            continue;
        }
        size_t j = i + 1;
        while (j < raw.size() && ident(raw[j])) {
            ++j;
        }
        const std::string tok = raw.substr(i, j - i);
        if (tok.starts_with("@clotho.inst.")) {
            const auto [it, fresh] =
              inst_ids.try_emplace(tok, static_cast<uint32_t>(inst_ids.size()));
            out += "@INST" + std::to_string(it->second);
        } else if (tok == "@" + fn->getName().str()) {
            out += "@FN";
        } else {
            out += tok;
        }
        i = j;
    }
    return out;
}

void Backend::report_dedup_shapes(const std::vector<llvm::Function*>& group_fns) {
    uint64_t ok = 0, differ = 0, skipped = 0;
    for (size_t k = 0; k < shape_sets_.size(); ++k) {
        const std::vector<uint32_t>& procs = shape_sets_[k];
        bool singleton = true;
        for (const uint32_t p : procs) {
            singleton = singleton && groups_[group_of_[p]].size() == 1;
        }
        if (!singleton) {
            ++skipped;
            continue;
        }
        const std::string first = normalize_shape(group_fns[group_of_[procs.front()]]);
        bool              same  = true;
        for (size_t m = 1; m < procs.size() && same; ++m) {
            const std::string other = normalize_shape(group_fns[group_of_[procs[m]]]);
            if (other != first) {
                same = false;
                // First mismatching line, for diagnosis.
                std::istringstream a(first), b(other);
                std::string        la, lb;
                while (std::getline(a, la) && std::getline(b, lb)) {
                    if (la != lb) {
                        std::fprintf(stderr, "dedup-shape DIFF set %zu (procs %u vs %u):\n  - %s\n  + %s\n",
                                     k, procs.front(), procs[m], la.c_str(), lb.c_str());
                        break;
                    }
                }
            }
        }
        ok += same ? 1 : 0;
        differ += same ? 0 : 1;
    }
    std::fprintf(stderr,
                 "dedup-shape: %zu twin sets: %llu identical, %llu DIFFER, %llu skipped (fused)\n",
                 shape_sets_.size(), (unsigned long long)ok, (unsigned long long)differ,
                 (unsigned long long)skipped);
}

// The single @clotho.inst.* global a function references, or null when it
// references none, more than one, or one from a PHI operand (a shape the
// ctx-to-parameter rewrite below does not handle). Eligible bodies
// reference exactly their own instance context: every def-relative site
// routes through ctx_field and def_relative_for restricts that to the
// group's own anchor.
llvm::GlobalVariable* Backend::only_inst_global(llvm::Function* fn) {
    llvm::GlobalVariable* found = nullptr;
    bool                  bad   = false;
    auto scan = [&](auto&& self, llvm::Constant* c) -> void {
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(c)) {
            if (gv->getName().starts_with("clotho.inst.")) {
                bad   = bad || (found != nullptr && found != gv);
                found = gv;
            }
            return;
        }
        if (llvm::isa<llvm::GlobalValue>(c)) {
            return;
        }
        for (const llvm::Use& op : c->operands()) {
            if (auto* oc = llvm::dyn_cast<llvm::Constant>(op)) {
                self(self, oc);
            }
        }
    };
    for (llvm::BasicBlock& bb : *fn) {
        for (llvm::Instruction& in : bb) {
            for (const llvm::Use& op : in.operands()) {
                auto* c = llvm::dyn_cast<llvm::Constant>(op);
                if (c == nullptr) {
                    continue;
                }
                llvm::GlobalVariable* before = found;
                scan(scan, c);
                bad = bad || (llvm::isa<llvm::PHINode>(in) && found != before);
            }
        }
    }
    return bad ? nullptr : found;
}

bool Backend::constant_uses(const llvm::Constant* c, const llvm::GlobalVariable* gv) {
    if (c == gv) {
        return true;
    }
    if (llvm::isa<llvm::GlobalValue>(c)) {
        return false;
    }
    for (const llvm::Use& op : c->operands()) {
        if (auto* oc = llvm::dyn_cast<llvm::Constant>(op); oc != nullptr && constant_uses(oc, gv)) {
            return true;
        }
    }
    return false;
}

// Clone `rep` into the definition's shared body: same (ptr, ptr) -> void
// signature, with every constant reference to the rep instance's ctx
// global rewritten to the first parameter. noinline keeps ONE compiled
// copy — the inliner would otherwise re-expand the body into every stub,
// undoing the compile-time win.
llvm::Function* Backend::clone_with_ctx_param(llvm::Function* rep, llvm::GlobalVariable* ctx_gv) {
    auto* shared = llvm::Function::Create(rep->getFunctionType(), llvm::Function::InternalLinkage,
                                          rep->getName() + ".shared", *rep->getParent());
    llvm::ValueToValueMapTy vmap;
    auto                    ni = shared->arg_begin();
    for (llvm::Argument& a : rep->args()) {
        vmap[&a] = &*ni;
        ++ni;
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> rets;
    llvm::CloneFunctionInto(shared, rep, vmap, llvm::CloneFunctionChangeType::LocalChangesOnly, rets);
    // After the clone: CloneFunctionInto copies the rep's attributes,
    // which would reset these.
    shared->setDSOLocal(true);
    shared->addFnAttr(llvm::Attribute::NoInline);
    llvm::Argument* ctx_arg = shared->getArg(0);
    ctx_arg->setName("ictx");
    for (llvm::BasicBlock& bb : *shared) {
        for (llvm::Instruction& in : bb) {
            for (unsigned i = 0; i < in.getNumOperands(); ++i) {
                llvm::Value* op = in.getOperand(i);
                if (op == ctx_gv) {
                    in.setOperand(i, ctx_arg);
                    continue;
                }
                auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(op);
                if (ce == nullptr || !constant_uses(ce, ctx_gv)) {
                    continue;
                }
                llvm::Instruction* repl = ce->getAsInstruction();
                repl->insertBefore(in.getIterator());
                for (unsigned k = 0; k < repl->getNumOperands(); ++k) {
                    if (repl->getOperand(k) == ctx_gv) {
                        repl->setOperand(k, ctx_arg);
                    }
                }
                in.setOperand(i, repl);
            }
        }
    }
    return shared;
}

// Replace `fn`'s body with a musttail stub into the shared body, passing
// this instance's ctx global as the first argument (identical (ptr, ptr)
// prototypes, so the subscription/dispatch ABI is untouched).
void Backend::rewrite_to_stub(llvm::Function* fn, llvm::Function* shared, llvm::GlobalVariable* ctx_gv) {
    fn->deleteBody();
    llvm::IRBuilderBase::InsertPoint saved = b_.saveIP();
    b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
    auto* call = b_.CreateCall(shared->getFunctionType(), shared, { ctx_gv, fn->getArg(1) });
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    b_.CreateRetVoid();
    b_.restoreIP(saved);
}

// Share one compiled body across twin instances of a reused definition.
// Candidates: thunk groups attributed to a reused-definition instance
// (group_anchor_of_) whose lowered function leaves the state argument
// unused (thunk bodies address storage through the anchors) and
// references exactly its own instance-context global. Groups bucket by
// normalize_shape — literal IR identity modulo the function's own name
// and that one symbol — so the transformation applies only where it is
// provably exact; the canonical-hash detector (report_dedup_stat) is a
// statistic, not the gate. Each bucket of >= 2 keeps one shared body (a
// clone of the first member with the ctx global replaced by a parameter)
// and every member becomes a two-instruction musttail stub passing its
// own ctx. Per-member NBA blocks / commit functions / ctx fills, VCD
// indices, and wait machinery are untouched — members registered their
// own during lowering; only body code is shared. Runs before
// finalize_block_nba / emit_main (nothing reads group function bodies,
// only their addresses). Disabled by --no-dedup.
void Backend::share_twin_groups(const std::vector<llvm::Function*>& group_fns) {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t   eligible = 0;
    struct Cand {
        uint32_t              g;
        llvm::GlobalVariable* ctx;
    };
    // Cheap structural pre-key: definition identity (ctx type) and size
    // counts — NOT the def-local ordinal: twins whose ordinal never
    // reaches the IR (no NBA block, sched constant equal by shape) may
    // share across ordinals, and shape equality is the actual gate. Only
    // pre-buckets of >= 2 pay the IR print — a singleton can share with
    // nothing.
    struct PreKey {
        const void* ctx_ty;
        uint64_t    sizes;
        bool        operator==(const PreKey&) const = default;
    };
    struct PreKeyHash {
        uint64_t operator()(const PreKey& k) const {
            return ankerl::unordered_dense::detail::wyhash::hash(
              reinterpret_cast<uintptr_t>(k.ctx_ty) ^ k.sizes);
        }
    };
    ankerl::unordered_dense::map<PreKey, std::vector<Cand>, PreKeyHash> pre;
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        if (!thunk_group_[g] || group_anchor_of_[g] == SignalSlot::no_anchor) {
            continue;
        }
        llvm::Function* fn = group_fns[g];
        if (!fn->getArg(0)->use_empty()) {
            continue;
        }
        llvm::GlobalVariable* ctx = only_inst_global(fn);
        if (ctx == nullptr || ctx != anchors_[group_anchor_of_[g]].ctx_gv) {
            continue;
        }
        ++eligible;
        uint64_t insts = 0, bbs = 0;
        for (const llvm::BasicBlock& bb : *fn) {
            ++bbs;
            insts += bb.size();
        }
        const Anchor& a = anchors_[group_anchor_of_[g]];
        pre[PreKey{ a.ctx_ty, bbs << 24 | insts << 40 }].push_back({ g, ctx });
    }
    llvm::ModuleSlotTracker mst(&env_->llmod, /*ShouldInitializeAllMetadata=*/false);
    ankerl::unordered_dense::map<std::string, std::vector<Cand>> buckets;
    for (auto& [pk, cs] : pre) {
        if (cs.size() < 2) {
            continue;
        }
        for (const Cand& c : cs) {
            buckets[normalize_shape(group_fns[c.g], &mst)].push_back(c);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    uint64_t sets = 0, members = 0;
    for (auto& [shape, cs] : buckets) {
        if (cs.size() < 2) {
            continue;
        }
        ++sets;
        members += cs.size();
        llvm::Function* shared = clone_with_ctx_param(group_fns[cs.front().g], cs.front().ctx);
        for (const Cand& c : cs) {
            rewrite_to_stub(group_fns[c.g], shared, c.ctx);
        }
    }
    if (stat_on(config_.stats, Stat::Dedup)) {
        const auto t2 = std::chrono::steady_clock::now();
        const auto ms = [](auto d) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
        };
        std::fprintf(stderr,
                     "dedup-share: %llu bodies -> %llu shared (%llu eligible; bucket %lld ms, "
                     "rewrite %lld ms)\n",
                     (unsigned long long)members, (unsigned long long)sets,
                     (unsigned long long)eligible, (long long)ms(t1 - t0), (long long)ms(t2 - t1));
    }
}

void Backend::report_finish_stat() const {
    const auto nf = static_cast<uint32_t>(mod_.function_count());

    // Seeds and closure from the very computation the emitter gates on. This
    // used to be a byte-for-byte second copy of compute_may_finish, scan
    // lambda included, and it drifted: the copy could not gain a fix the
    // original got. A stat that disagrees with the code it reports on is worse
    // than no stat.
    std::vector<uint8_t>       direct;
    const std::vector<uint8_t> may = compute_may_finish(&direct);

    // The population that matters: call sites needing a post-call test.
    uint64_t   sites = 0, proc_direct = 0, fn_direct = 0, fn_may = 0;
    const auto count_sites = [&](const Ir& ir) {
        std::vector<FunctionId> cs;
        scan_finish(ir, &cs);
        for (const FunctionId c : cs) {
            sites += may[static_cast<uint32_t>(c)] != 0 ? 1 : 0;
        }
    };
    for (const Process& p : mod_.processes()) {
        proc_direct += scan_finish(p.ir, nullptr) ? 1 : 0;
        count_sites(p.ir);
    }
    for (uint32_t f = 0; f < nf; ++f) {
        fn_direct += direct[f];
        fn_may += may[f];
        count_sites(mod_.function(static_cast<FunctionId>(f)).ir);
    }

    fprintf(stderr,
            "FINISH_STAT processes=%zu (with $finish: %llu)  subroutines=%u "
            "(direct: %llu, may-finish: %llu)  instrumented call sites=%llu\n",
            mod_.processes().size(), static_cast<unsigned long long>(proc_direct), nf,
            static_cast<unsigned long long>(fn_direct),
            static_cast<unsigned long long>(fn_may),
            static_cast<unsigned long long>(sites));
    for (uint32_t f = 0; f < nf; ++f) {
        if (may[f] != 0) {
            const Function& fn = mod_.function(static_cast<FunctionId>(f));
            fprintf(stderr, "FINISH_STAT   may-finish %s %s%s\n", fn.is_task ? "task" : "function",
                    fn.name.c_str(), direct[f] != 0 ? " (direct)" : " (via a callee)");
        }
    }
}

void Backend::report_comb_fuse_stat() const {
    const auto all = mod_.processes();
    uint64_t   counts[static_cast<size_t>(CombFuse::Count)] = {};
    uint64_t   always = 0, singleton_eligible = 0;
    for (uint32_t k = 0; k < all.size(); ++k) {
        if (all[k].activation != Activation::Always) {
            continue;
        }
        ++always;
        const CombFuse v = facts_.processes[k].comb_fuse;
        counts[static_cast<size_t>(v)]++;
        if (v == CombFuse::Eligible && groups_[group_of_[k]].size() == 1) {
            ++singleton_eligible;
        }
    }
    std::fprintf(stderr,
                 "comb-fuse: %llu/%llu always procs eligible (%llu unfused singletons); "
                 "excluded: not-simple=%llu edge=%llu impure=%llu call=%llu nba=%llu "
                 "write=%llu read!sens=%llu\n",
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::Eligible)],
                 (unsigned long long)always, (unsigned long long)singleton_eligible,
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::NotSimpleAlways)],
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::EdgeSensitive)],
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::ImpureOp)],
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::HasCall)],
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::NbaWrite)],
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::UnsupportedWrite)],
                 (unsigned long long)counts[static_cast<size_t>(CombFuse::ReadOutsideSens)]);
    if (!comb_group_sizes_.empty()) {
        uint64_t members = 0, mx = 0;
        for (const uint32_t s : comb_group_sizes_) {
            members += s;
            mx = std::max<uint64_t>(mx, s);
        }
        std::fprintf(stderr, "comb-fuse: %zu comb groups, %llu members (max %llu): ",
                     comb_group_sizes_.size(), (unsigned long long)members,
                     (unsigned long long)mx);
        std::vector<uint32_t> sorted(comb_group_sizes_.begin(), comb_group_sizes_.end());
        std::sort(sorted.rbegin(), sorted.rend());
        for (size_t i = 0; i < sorted.size() && i < 24; ++i) {
            std::fprintf(stderr, "%u ", sorted[i]);
        }
        std::fprintf(stderr, "%s\n", sorted.size() > 24 ? "..." : "");
    }
    uint64_t guarded = 0, guarded_members = 0;
    for (uint32_t g = 0; g < group_guarded_.size(); ++g) {
        if (group_guarded_[g]) {
            ++guarded;
            guarded_members += groups_[g].size();
        }
    }
    if (guarded != 0 || trig_collisions_ != 0) {
        std::fprintf(stderr,
                     "comb-fuse: %llu guarded groups, %llu guarded members, "
                     "%u colour collisions\n",
                     (unsigned long long)guarded, (unsigned long long)guarded_members,
                     trig_collisions_);
    }
}

} // namespace clotho::ir::backend
