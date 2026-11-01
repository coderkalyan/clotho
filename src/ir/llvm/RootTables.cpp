// Root from tables: what differs per instance is data, not code (docs/design/
// modular-compilation.md, step 3). The instance records are the initializers of
// their globals, so no code of root fills them.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

// The power-on value of every instance record, as the initializer of its global, and
// the other constants main used to store: the entry of each thunk group's wake pair
// and the trigger bits of a bucket no record describes. Nothing here is code, so
// nothing grows with the instance count but data.
//
// A record is filled from facts the compile knows: where the instance's storage is,
// what its ports hold of the owner's side (ids, dump-var index, shadow, wake
// buckets), the instances its bindings name. Only what a run changes is not: the
// wake words (the seed sets the ones that start now) and the counters.
void Backend::fill_instance_records(const std::vector<llvm::Function*>& procs) {
    auto* const ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto* const null   = llvm::ConstantPointerNull::get(ptr_ty);
    // A binding names the record of the instance that owns its signal or its
    // function, so that record must exist before anything is filled; creating it
    // can make the owner an instance with bindings of its own to fill.
    for (bool grew = true; grew;) {
        grew = false;
        for (Anchor& a : anchors_) {
            if (a.ctx_gv == nullptr) {
                continue;
            }
            for (uint32_t k = 0; k < a.bind_used.size(); ++k) {
                const SignalSlot& s = slot_of(a.externs[k]);
                if (a.bind_used[k] != 0 && s.anchor != SignalSlot::no_anchor && anchors_[s.anchor].ctx_gv == nullptr) {
                    ensure_ctx(anchors_[s.anchor]);
                    grew = true;
                }
            }
            for (uint32_t k = 0; k < a.fbind_used.size(); ++k) {
                const uint32_t owner = subroutine_anchor(mod_.function(a.fn_externs[k]));
                if (a.fbind_used[k] != 0 && owner != SignalSlot::no_anchor && anchors_[owner].ctx_gv == nullptr) {
                    ensure_ctx(anchors_[owner]);
                    grew = true;
                }
            }
        }
    }

    // Every record field starts as zero; `ports` holds the elements of each port
    // field and `wakes` the wake pairs, which are fields of their own.
    struct Record {
        std::vector<llvm::Constant*>              fields;
        std::vector<std::vector<llvm::Constant*>> ports;
        std::vector<llvm::Constant*>              wakes;
    };
    std::vector<Record> records(anchors_.size());
    auto* const         no_wake = llvm::ConstantAggregateZero::get(wake_ty());
    for (uint32_t ai = 0; ai < anchors_.size(); ++ai) {
        const Anchor& a = anchors_[ai];
        if (a.ctx_gv == nullptr) {
            continue;
        }
        Record& r = records[ai];
        for (uint32_t f = 0; f < a.ctx_ty->getNumElements(); ++f) {
            r.fields.push_back(llvm::Constant::getNullValue(a.ctx_ty->getElementType(f)));
        }
        for (uint32_t p = 0; p < a.n_port; ++p) {
            auto* const pty  = llvm::cast<llvm::StructType>(a.ctx_ty->getElementType(a.ctx_port_base + p));
            auto&       port = r.ports.emplace_back();
            for (uint32_t e = 0; e < pty->getNumElements(); ++e) {
                port.push_back(llvm::Constant::getNullValue(pty->getElementType(e)));
            }
        }
        r.wakes.assign(a.n_blk, no_wake);
    }
    const auto set = [&](uint32_t ai, uint32_t field, llvm::Constant* v) {
        assert(records[ai].fields[field]->getType() == v->getType() && "a record field takes a value of its own type");
        records[ai].fields[field] = v;
    };
    const auto set_port = [&](uint32_t ai, uint32_t rank, uint32_t elem, llvm::Constant* v) {
        assert(records[ai].ports[rank][elem]->getType() == v->getType() && "a port element takes a value of its own type");
        records[ai].ports[rank][elem] = v;
    };
    const auto anchor_index = [&](const Anchor& a) { return static_cast<uint32_t>(&a - anchors_.data()); };
    const auto gep   = [&](llvm::Type* ty, llvm::Constant* base, std::initializer_list<uint32_t> idx) {
        std::vector<llvm::Constant*> v;
        for (const uint32_t i : idx) {
            v.push_back(b_.getInt32(i));
        }
        return llvm::ConstantExpr::getInBoundsGetElementPtr(ty, base, v);
    };

    for (Anchor& a : anchors_) {
        if (a.ctx_gv == nullptr) {
            continue;
        }
        const uint32_t ai = anchor_index(a);
        set(ai, kCtxState, sym(a.state_gv));
        if (a.shadow_ty != nullptr) {
            set(ai, kCtxShadow, sym(a.shadow_gv));
        }
        if (a.first_vcd >= 0) {
            set(ai, kCtxFirstVcd, b_.getInt32(static_cast<uint32_t>(a.first_vcd)));
        }
        // The instance's name, which its code reads for `%m` and banners.
        set(ai, a.ctx_name_base, sym(inst_path_global(ai)));
        // The flat ids of the `disable` targets and covergroup types it declares.
        for (uint32_t k = 0; k < a.disable_locals.size(); ++k) {
            set(ai, a.ctx_dis_base + k, b_.getInt32(a.disable_locals[k]));
        }
        for (uint32_t k = 0; k < a.cover_locals.size(); ++k) {
            set(ai, a.ctx_cov_base + k, b_.getInt32(a.cover_locals[k]));
        }
        // The waiter queue of each `expect` it declares, which both halves of the
        // statement reach through the record.
        for (uint32_t k = 0; k < a.expect_locals.size(); ++k) {
            set(ai, a.ctx_expect_base + k, sym(expect_queue(a.expect_locals[k])));
        }
        // The imports this instance's code reads: where each signal is, who owns it
        // (nobody, for a package's) and where its port is in the owner's record
        // (nowhere, for a signal with no port).
        for (uint32_t k = 0; k < a.bind_used.size(); ++k) {
            if (a.bind_used[k] == 0) {
                continue;
            }
            const SignalSlot& s = slot_of(a.externs[k]);
            set(ai, a.ctx_bind_base + 3 * k, storage_address(s));
            set(ai, a.ctx_bind_base + 3 * k + 1,
                s.anchor != SignalSlot::no_anchor ? static_cast<llvm::Constant*>(sym(anchors_[s.anchor].ctx_gv)) : null);
            set(ai, a.ctx_bind_base + 3 * k + 2, port_of(s) != nullptr ? port_address(s) : null);
        }
        // The functions this instance's code calls in other instances: whose record
        // each is passed.
        for (uint32_t k = 0; k < a.fbind_used.size(); ++k) {
            if (a.fbind_used[k] == 0) {
                continue;
            }
            set(ai, a.ctx_fbind_base + k, sym(anchors_[subroutine_anchor(mod_.function(a.fn_externs[k]))].ctx_gv));
        }
        for (const Signal sj : anchor_signals_[ai]) {
            if (sj == Signal{ ~0u }) {
                continue;
            }
            const auto        si = static_cast<uint32_t>(sj);
            const SignalSlot& s  = slots_[si];
            const PortLayout* pl = port_of(s);
            if (pl == nullptr) {
                continue;
            }
            // The signal's port: its ids, dump-var index, shadow slots, wake buckets
            // (with this instance's exact subscriber count: its slots are filled
            // densely from 0, so its own scan needs no padding) and vif waiter heads.
            const uint32_t rank = port_field_[si];
            const auto     put  = [&](uint32_t elem, llvm::Constant* v) { set_port(ai, rank, elem, v); };
            for (uint32_t f = 0; f < kIdFamilies; ++f) {
                if (pl->id[f] != no_port_elem && id_val_[si][f] != no_id) {
                    put(pl->id[f], b_.getInt32(id_val_[si][f]));
                }
            }
            if (pl->vcd != no_port_elem && vcd_index_[si] >= 0) {
                put(pl->vcd, b_.getInt32(static_cast<uint32_t>(vcd_index_[si])));
            }
            for (uint32_t k = 0; k < s.sh_count && k < pl->n_shadow; ++k) {
                put(pl->shadow + k, gep(a.shadow_ty, sym(a.shadow_gv), { 0, s.sh_first + k }));
            }
            for (uint32_t e = 0; e < 3; ++e) {
                if (pl->sta[e].base != no_port_elem && wait_sta_gv_[si][e] != nullptr) {
                    put(pl->sta[e].base, sym(wait_sta_gv_[si][e]));
                    put(pl->sta[e].trig, b_.getInt64(trig_bits(sj, e)));
                    put(pl->sta[e].cap, b_.getInt32(wait_sta_true_[si][e]));
                }
                if (pl->dyn[e].base != no_port_elem && wait_dyn_gv_[si][e] != nullptr) {
                    put(pl->dyn[e].base, sym(wait_dyn_gv_[si][e]));
                    put(pl->dyn[e].len, sym(wait_dyn_len_gv_[si][e]));
                    put(pl->dyn[e].cap, b_.getInt32(static_cast<uint32_t>(wait_dyn_ty_[si][e]->getNumElements())));
                }
                if (pl->vif[e] != no_port_elem && vif_head_ord_[si][e] != ~0u) {
                    put(pl->vif[e], gep(a.ty, sym(a.state_gv), { 0, a.vif_head_base + vif_head_ord_[si][e] }));
                }
            }
        }
    }
    for (const auto& [aid, field, first_id] : ctx_blk_fills_) {
        assert(anchors_[aid].ctx_gv != nullptr && "a block id is filled into a record that exists");
        set(aid, field, b_.getInt32(first_id));
    }
    // Each owned process's design-wide index, which its instance's code reads for
    // its control block and its random stream, and each program instance's index.
    for (uint32_t k = 0; k < mod_.processes().size(); ++k) {
        const uint32_t pa = proc_sched_anchor_[k];
        if (pa != SignalSlot::no_anchor && anchors_[pa].ctx_gv != nullptr && anchors_[pa].n_proc != 0) {
            set(pa, anchors_[pa].ctx_proc_base + proc_sched_ord_[k], b_.getInt32(k));
        }
    }
    for (uint32_t inst = 0; inst < program_inst_procs_.size(); ++inst) {
        const uint32_t pa = program_inst_anchor(inst);
        if (pa != SignalSlot::no_anchor && anchors_[pa].ctx_gv != nullptr) {
            set(pa, anchors_[pa].ctx_prog_base, b_.getInt32(inst));
        }
    }
    // Trigger masks of the guarded groups an instance owns: the design-global
    // colours, so data in its record rather than immediates in its code. A group
    // spanning instances is guarded by its root-side dispatcher, which bakes them.
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        if (!group_guarded_[g] || group_unit_anchor_[g] == SignalSlot::no_anchor) {
            continue;
        }
        const uint32_t ai = group_unit_anchor_[g];
        const Anchor&  a  = anchors_[ai];
        assert(a.ctx_gv != nullptr && "a guarded group's masks are filled into a record that exists");
        set(ai, a.ctx_gmask_base + proc_sched_ord_[groups_[g].front()], b_.getInt64(group_trig_mask_[g]));
        for (size_t i = 0; i < groups_[g].size(); ++i) {
            set(ai, a.ctx_mmask_base + proc_sched_ord_[groups_[g][i]], b_.getInt64(member_mask(g, i)));
        }
    }
    // The entry each thunk group's handle runs, beside its wake word: the handle is
    // the address of the pair (the seed) and the drain calls what it finds there.
    std::vector<llvm::Constant*> root_wakes(sched_arr_ty_->getNumElements(), no_wake);
    for (uint32_t g = 0; g < procs.size(); ++g) {
        if (!sched_[g].thunk) {
            continue;
        }
        const uint32_t        k      = sched_[g].front;
        const uint32_t        aid    = proc_sched_anchor_[k];
        llvm::Constant* const word   = llvm::ConstantInt::get(sched_word_ty(), 0);
        llvm::Constant* const entry  = sym(procs[g]);
        llvm::Constant* const pair   = llvm::ConstantStruct::get(wake_ty(), { word, entry });
        if (aid == SignalSlot::no_anchor) {
            root_wakes[proc_sched_ord_[k]] = pair;
        } else {
            assert(anchors_[aid].ctx_gv != nullptr && "a thunk group's record exists: the seed addressed its wake word");
            records[aid].wakes[proc_sched_ord_[k]] = pair;
        }
    }
    sched_gv_->setInitializer(llvm::ConstantArray::get(sched_arr_ty_, root_wakes));
    // The trigger bits of a static bucket no record describes (a signal of no
    // instance), which any unit's writer reads.
    for (uint32_t si = 0; si < wait_sta_trig_gv_.size(); ++si) {
        for (uint32_t e = 0; e < 3; ++e) {
            if (wait_sta_trig_gv_[si][e] != nullptr) {
                wait_sta_trig_gv_[si][e]->setInitializer(
                  llvm::ConstantInt::get(b_.getInt64Ty(), trig_bits(static_cast<Signal>(si), e)));
            }
        }
    }
    for (uint32_t ai = 0; ai < anchors_.size(); ++ai) {
        Anchor& a = anchors_[ai];
        if (a.ctx_gv == nullptr) {
            continue;
        }
        Record& r = records[ai];
        for (uint32_t p = 0; p < a.n_port; ++p) {
            auto* const pty = llvm::cast<llvm::StructType>(a.ctx_ty->getElementType(a.ctx_port_base + p));
            r.fields[a.ctx_port_base + p] = llvm::ConstantStruct::get(pty, r.ports[p]);
        }
        r.fields[a.ctx_sched_base] =
          llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(a.ctx_ty->getElementType(a.ctx_sched_base)), r.wakes);
        a.ctx_gv->setInitializer(llvm::ConstantStruct::get(a.ctx_ty, r.fields));
    }
}

} // namespace clotho::ir::backend
