// Module deduplication: twin-group sharing, the check of shared code, and the dedup and fusion statistics.

#include "ir/llvm/Backend.h"
#include "ir/llvm/CodeText.h"

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
//
// With `number_inst_symbols` off only the function's own name is normalized:
// two functions that print the same then name the very same symbols, which is
// what merging them into one function needs.
std::string Backend::normalize_shape(llvm::Function* fn, llvm::ModuleSlotTracker* mst, bool number_inst_symbols) {
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
        if (tok == "@" + fn->getName().str()) {
            out += "@FN";
        } else if (number_inst_symbols && tok.starts_with("@clotho.inst.")) {
            const auto [it, fresh] =
              inst_ids.try_emplace(tok, static_cast<uint32_t>(inst_ids.size()));
            out += "@INST" + std::to_string(it->second);
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

// `--stat=unit-code`: what is still emitted once per instance of a unit rather
// than once per unit (docs/design/modular-compilation.md, step 3). For every
// unit with several instances, the groups are counted per unit (its fusion
// groups) and per instance, with the functions emitted for them: a group is
// lowered once per unit and every instance runs it with its own record, so the
// two numbers agree unless the code names something of the instance it was
// lowered for (a function copy only it calls, its name, a covergroup index, a
// disable target, another instance's nonblocking staging, a process-local
// stream or control block). Those are counted by what stopped sharing. A group
// of no instance has no unit to share in and is counted apart. The functions
// and tasks are counted the same way on a line of their own: per unit and per
// instance, with the functions emitted for them.
void Backend::report_unit_code(const std::vector<llvm::Function*>& group_fns) {
    ankerl::unordered_dense::set<uint64_t>            unit_groups;
    ankerl::unordered_dense::set<llvm::Function*>     emitted;
    uint64_t                                          instance_groups = 0, coroutines = 0, unanchored = 0;
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const uint32_t aid = group_unit_anchor_[g];
        if (aid == SignalSlot::no_anchor) {
            ++unanchored;
            continue;
        }
        const uint32_t unit = anchors_[aid].unit;
        if (unit == InstanceScope::no_unit || unit_instances_.at(unit) < 2) {
            continue;
        }
        unit_groups.insert((static_cast<uint64_t>(unit) << 32) | group_unit_ordinal_[g]);
        emitted.insert(group_fns[g]);
        ++instance_groups;
        coroutines += thunk_group_[g] ? 0 : 1;
    }
    uint64_t shared = 0, rows = 0;
    for (const SchedGroup& sg : sched_) {
        if (sg.rows.size() > 1) {
            ++shared;
            rows += sg.rows.size();
        }
    }
    std::string why;
    for (const auto& [reason, n] : unshared_) {
        why += std::format("\n  {} groups lowered per instance: {}", n, reason);
    }
    std::fprintf(stderr,
                 "unit-code: %llu groups of multi-instance units over %llu instances' groups, %llu functions "
                 "emitted (%llu of the groups are coroutines); %llu schedule groups run several "
                 "groups (%llu rows), %llu groups belong to no instance%s\n",
                 (unsigned long long)unit_groups.size(), (unsigned long long)instance_groups,
                 (unsigned long long)emitted.size(), (unsigned long long)coroutines, (unsigned long long)shared,
                 (unsigned long long)rows, (unsigned long long)unanchored, why.c_str());

    ankerl::unordered_dense::set<uint64_t>        unit_subs;
    ankerl::unordered_dense::set<llvm::Function*> emitted_subs;
    uint64_t                                      instance_subs = 0, tasks = 0;
    for (uint32_t f = 0; f < subroutine_home_.size(); ++f) {
        const SubroutineHome home = subroutine_home_[f];
        const Function&      fu   = mod_.function(static_cast<FunctionId>(f));
        if (home.anchor == SignalSlot::no_anchor || fu.is_dpi || unit_instances_.at(anchors_[home.anchor].unit) < 2) {
            continue;
        }
        unit_subs.insert((static_cast<uint64_t>(anchors_[home.anchor].unit) << 32) | home.ordinal);
        emitted_subs.insert(fn_decls_[f]);
        ++instance_subs;
        tasks += fu.is_task ? 1 : 0;
    }
    std::string why_subs;
    for (const auto& [reason, n] : unshared_subroutines_) {
        why_subs += std::format("\n  {} subroutines lowered per instance: {}", n, reason);
    }
    std::fprintf(stderr,
                 "unit-code: %llu subroutines of multi-instance units over %llu instances' subroutines, %llu "
                 "functions emitted (%llu of the subroutines are tasks)%s\n",
                 (unsigned long long)unit_subs.size(), (unsigned long long)instance_subs,
                 (unsigned long long)emitted_subs.size(), (unsigned long long)tasks, why_subs.c_str());
    if (config_.verify_shared_code) {
        std::fprintf(stderr,
                     "unit-code: %llu shared groups lowered again for their own instance and compared with "
                     "the code they share, %lld ms\n",
                     (unsigned long long)verified_groups_,
                     (long long)std::chrono::duration_cast<std::chrono::milliseconds>(verify_time_).count());
        std::fprintf(stderr,
                     "unit-code: %llu shared subroutines lowered again for their own instance and compared with "
                     "the code they share, %lld ms\n",
                     (unsigned long long)verified_subroutines_,
                     (long long)std::chrono::duration_cast<std::chrono::milliseconds>(verify_subroutine_time_).count());
    }
}

// -- Verifying shared code --
//
// SimulationConfig::verify_shared_code. A unit's group is lowered once and run
// by every instance whose imports are alike, so what an instance's own lowering
// would have baked (and the taint list missed) is silently wrong code for the
// others. The check lowers the group again for each such instance, the way it
// was lowered before groups were shared, and compares.

// Lower group `g` for its own instance into a scratch module and take back out
// of the backend what lowering registers outside it. The function stays in the
// scratch module (see verify_modules_).
Backend::OwnLowering Backend::lower_for_own_instance(uint32_t g) {
    Anchor&        a = anchors_[group_unit_anchor_[g]];
    llvm::Module&  scratch = *verify_modules_.emplace_back(std::make_unique<llvm::Module>("clotho.verify", ctx_));
    scratch.setTargetTriple(root_env_->llmod.getTargetTriple());
    scratch.setDataLayout(root_env_->llmod.getDataLayout());
    ModuleEnv* const           real_env = env_;
    env_                                = verify_envs_.emplace_back(std::make_unique<ModuleEnv>(scratch)).get();
    const auto                 block_lo = static_cast<std::ptrdiff_t>(block_commits_.size());
    const auto                 fill_lo  = static_cast<std::ptrdiff_t>(ctx_blk_fills_.size());
    const std::vector<uint8_t> bound    = a.bind_used;
    const std::vector<uint8_t> fbound   = a.fbind_used;
    verifying_                          = true;
    share_active_                       = true;
    taint_why_                          = nullptr;
    OwnLowering own;
    own.fn        = lower_group(groups_[g]);
    own.taint     = std::exchange(taint_why_, nullptr);
    verifying_    = false;
    share_active_ = false;
    env_          = real_env;
    own.blocks.assign(std::make_move_iterator(block_commits_.begin() + block_lo),
                      std::make_move_iterator(block_commits_.end()));
    block_commits_.erase(block_commits_.begin() + block_lo, block_commits_.end());
    own.fills.assign(ctx_blk_fills_.begin() + fill_lo, ctx_blk_fills_.end());
    ctx_blk_fills_.erase(ctx_blk_fills_.begin() + fill_lo, ctx_blk_fills_.end());
    a.bind_used  = bound;
    a.fbind_used = fbound;
    return own;
}

// How the own lowering of group `g` differs from the code its instance runs when
// it shares `rep`'s: the printed code, the NBA blocks the shared code registers
// for it against the ones it registers itself, and the record fields that hold
// their first ids. Empty when it does not.
std::string Backend::shared_code_differs(uint32_t g, SharedGroup& rep, const OwnLowering& own) {
    const uint32_t ua = group_unit_anchor_[g];
    std::string    differs;
    if (own.taint != nullptr) {
        differs += std::format("  lowered for itself, the group bakes something of its instance: {}\n", own.taint);
    }
    if (rep.text.empty()) {
        rep.text = CodeText(rep.fn).joined();
    }
    const CodeText own_text(own.fn);
    if (own_text.joined() != rep.text) {
        differs += "  " + CodeText::difference(CodeText(rep.fn), own_text);
    }
    const std::vector<BlockCommit> shared = shared_blocks(rep, ua);
    const auto                     describe = [&](const BlockCommit& bc) {
        std::string out = "{";
        for (const BlockSig& bs : bc.sigs) {
            out += std::format("{}{} #{}{}", out.size() > 1 ? ", " : "", mod_.signals()[static_cast<size_t>(bs.sig)].name,
                               static_cast<uint32_t>(bs.sig), bs.dynamic ? " (dynamic mask)" : "");
        }
        return out + "}";
    };
    const auto same_block = [](const BlockCommit& x, const BlockCommit& y) {
        return x.sigs.size() == y.sigs.size() &&
               std::equal(x.sigs.begin(), x.sigs.end(), y.sigs.begin(), [](const BlockSig& p, const BlockSig& q) {
                   return p.sig == q.sig && p.dynamic == q.dynamic && p.mask.getBitWidth() == q.mask.getBitWidth() &&
                          p.mask == q.mask;
               });
    };
    if (shared.size() != own.blocks.size()) {
        differs += std::format("  it registers {} NBA blocks, and the shared code {}\n", own.blocks.size(), shared.size());
    }
    for (size_t b = 0; b < shared.size() && b < own.blocks.size(); ++b) {
        if (!same_block(shared[b], own.blocks[b])) {
            differs += std::format("  NBA block {} of the group commits {} for it, and its own lowering {}\n", b,
                                   describe(shared[b]), describe(own.blocks[b]));
            break;
        }
    }
    // Its own lowering numbered its blocks from where the backend stood.
    if (shared_fills(rep, ua, static_cast<uint32_t>(block_commits_.size())) != own.fills) {
        differs += "  the record fields that hold the first id of its NBA blocks differ from the ones the shared "
                   "code's blocks are filled into\n";
    }
    return differs;
}

void Backend::verify_shared_group(uint32_t g, SharedGroup& rep) {
    const auto         started = std::chrono::steady_clock::now();
    const OwnLowering  own     = lower_for_own_instance(g);
    const std::string  differs = shared_code_differs(g, rep, own);
    ++verified_groups_;
    verify_time_ += std::chrono::steady_clock::now() - started;
    if (differs.empty()) {
        return;
    }
    const Anchor& a    = anchors_[group_unit_anchor_[g]];
    std::string   unit = a.ty->getName().str();
    if (unit.ends_with(".state")) {
        unit.resize(unit.size() - 6);
    }
    const Process& first = mod_.processes()[groups_[g].front()];
    std::string    at;
    if (resolve_ && first.span.valid()) {
        at = " (" + resolve_(first.span).where + ")";
    }
    throw BackendError{ std::format("internal: the code shared by the instances of unit '{}' is not what instance '{}' "
                                    "lowers for itself\n  group {}{}, lowered once for '{}' and run by '{}' too\n{}"
                                    "  a fact of one instance is baked into code the others run: move it into the "
                                    "instance record, or taint the lowering (Backend::taint)",
                                    unit, a.name, group_unit_ordinal_[g], at, anchors_[group_unit_anchor_[rep.g]].name,
                                    a.name, differs),
                        first.span };
}

// Lower subroutine `f` again, for its own instance and into a scratch module as
// it would have been without sharing, and check it is the function `rep` shares
// with it: the same printed code (CodeText), with nothing of its instance
// baked in (a taint). The declaration it lowers into is a scratch one, and
// everything the lowering marked in its instance's record is put back, so the
// emitted modules are the same with the check on and off.
void Backend::verify_shared_subroutine(uint32_t f, SharedSubroutine& rep) {
    const auto           started = std::chrono::steady_clock::now();
    const auto           id      = static_cast<FunctionId>(f);
    const SubroutineHome home    = subroutine_home_[f];
    Anchor&              a       = anchors_[home.anchor];
    llvm::Module& scratch = *verify_modules_.emplace_back(std::make_unique<llvm::Module>("clotho.verify", ctx_));
    scratch.setTargetTriple(root_env_->llmod.getTargetTriple());
    scratch.setDataLayout(root_env_->llmod.getDataLayout());
    ModuleEnv* const           real_env  = env_;
    llvm::Function* const      real_decl = fn_decls_[f];
    const std::vector<uint8_t> bound     = a.bind_used;
    const std::vector<uint8_t> fbound    = a.fbind_used;
    const auto                 block_lo  = static_cast<std::ptrdiff_t>(block_commits_.size());
    const auto                 fill_lo   = static_cast<std::ptrdiff_t>(ctx_blk_fills_.size());
    env_                                 = verify_envs_.emplace_back(std::make_unique<ModuleEnv>(scratch)).get();
    verifying_                           = true;
    share_active_                        = true;
    taint_why_                           = nullptr;
    fn_decls_[f]                         = declare_function(id);
    lower_subroutine(id);
    llvm::Function* const own_fn = fn_decls_[f];
    const char* const     taint  = std::exchange(taint_why_, nullptr);
    fn_decls_[f]                 = real_decl;
    verifying_                   = false;
    share_active_                = false;
    env_                         = real_env;
    a.bind_used                  = bound;
    a.fbind_used                 = fbound;
    std::vector<BlockCommit> own_blocks(std::make_move_iterator(block_commits_.begin() + block_lo),
                                        std::make_move_iterator(block_commits_.end()));
    block_commits_.erase(block_commits_.begin() + block_lo, block_commits_.end());
    const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> own_fills(ctx_blk_fills_.begin() + fill_lo,
                                                                          ctx_blk_fills_.end());
    ctx_blk_fills_.erase(ctx_blk_fills_.begin() + fill_lo, ctx_blk_fills_.end());

    std::string differs;
    if (taint != nullptr) {
        differs += std::format("  lowered for itself, the subroutine bakes something of its instance: {}\n", taint);
    }
    // The shared function may have been promoted to external linkage by a caller
    // in another module since; compared at the linkage it was declared with.
    const llvm::GlobalValue::LinkageTypes linkage = rep.fn->getLinkage();
    rep.fn->setLinkage(own_fn->getLinkage());
    if (rep.text.empty()) {
        rep.text = CodeText(rep.fn).joined();
    }
    const CodeText own_text(own_fn);
    if (own_text.joined() != rep.text) {
        differs += "  " + CodeText::difference(CodeText(rep.fn), own_text);
    }
    rep.fn->setLinkage(linkage);
    // The NBA blocks it registers for the instance, and the record fields that hold their
    // first ids, against the ones the shared code's blocks give it.
    const auto shared_bl = shared_blocks_of(subroutine_home_[rep.f].anchor, rep.block_lo, rep.block_hi, home.anchor);
    const auto same_block = [](const BlockCommit& x, const BlockCommit& y) {
        return x.sigs.size() == y.sigs.size() &&
               std::equal(x.sigs.begin(), x.sigs.end(), y.sigs.begin(), [](const BlockSig& p, const BlockSig& q) {
                   return p.sig == q.sig && p.dynamic == q.dynamic && p.mask.getBitWidth() == q.mask.getBitWidth() &&
                          p.mask == q.mask;
               });
    };
    if (shared_bl.size() != own_blocks.size() ||
        !std::equal(shared_bl.begin(), shared_bl.end(), own_blocks.begin(), same_block)) {
        differs += std::format("  it registers {} NBA blocks, and the shared code's differ ({}) from them\n",
                               own_blocks.size(), shared_bl.size());
    }
    if (shared_fills_of(rep.block_lo, rep.fill_lo, rep.fill_hi, home.anchor,
                        static_cast<uint32_t>(block_commits_.size())) != own_fills) {
        differs += "  the record fields that hold the first id of its NBA blocks differ from the ones the shared "
                   "code's blocks are filled into\n";
    }
    ++verified_subroutines_;
    verify_subroutine_time_ += std::chrono::steady_clock::now() - started;
    if (differs.empty()) {
        return;
    }
    const Function& fu   = mod_.function(id);
    std::string     unit = a.ty->getName().str();
    if (unit.ends_with(".state")) {
        unit.resize(unit.size() - 6);
    }
    std::string at;
    if (resolve_ && fu.span.valid()) {
        at = " (" + resolve_(fu.span).where + ")";
    }
    throw BackendError{ std::format("internal: the code shared by the instances of unit '{}' is not what instance '{}' "
                                    "lowers for itself\n  {} '{}'{}, lowered once for '{}' and run by '{}' too\n{}"
                                    "  a fact of one instance is baked into code the others run: move it into the "
                                    "instance record, add it to the family key (Backend::subroutine_key), or taint "
                                    "the lowering (Backend::taint)",
                                    unit, a.name, fu.is_task ? "task" : "function", fu.name, at,
                                    anchors_[subroutine_home_[rep.f].anchor].name, a.name, differs),
                        fu.span };
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
