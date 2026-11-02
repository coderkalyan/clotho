// Root from tables: what differs per instance or per group is data, and the code of
// root is a few loops over it, the same size for any design (docs/design/
// modular-compilation.md, step 3).
//
//   - the instance records are the initializers of their globals;
//   - the groups that run once (PreInit, Preponed, Observed, Re-NBA, Final) are a
//     table behind `clotho.run_once`;
//   - the seed creates the handle of every group, starts the ones that start now and
//     subscribes the sensitive ones, from the rows of `clotho.seed.*` behind
//     `clotho.seed`.
//
// A table is built in the order the straight-line code used to be emitted in, so
// the order of what has an order (the rings' contents, the links of an event's
// waiter list) is what it always was.

#include "ir/llvm/Backend.h"

namespace clotho::ir::backend {

namespace {

// What a row of the seed says of its group (the `kind` word).
constexpr uint32_t kSeedFlag      = 1; // set the group's wake word before it is queued
constexpr uint32_t kSeedRingShift = 1; // the ring it is queued on, two bits (SeedRing)
constexpr uint32_t kSeedRingMask  = 3;
constexpr uint32_t kSeedLate      = 8;  // queued after every other group, not in its turn
constexpr uint32_t kSeedTag       = 16; // its subscription carries the reactive continuous tag

enum SeedRing : uint32_t { RingNone, RingActive, RingReactive, RingCont };

} // namespace

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

// `clotho.run_once(table, count, wheel)`: for each row `{ ramp, first argument }`,
// build the group's frame, run its body to completion and release it. What a
// region's groups are is the table's business, so this is one function for every
// region and every design.
llvm::Function* Backend::run_once_fn() {
    if (run_once_fn_ != nullptr) {
        return run_once_fn_;
    }
    InRoot in_root(*this);
    auto* const ptr  = llvm::PointerType::getUnqual(ctx_);
    auto* const i32  = b_.getInt32Ty();
    auto* const ramp = llvm::FunctionType::get(ptr, { ptr, ptr }, false);
    run_once_fn_     = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), { ptr, i32, ptr }, false),
                                              llvm::Function::InternalLinkage, "clotho.run_once", env_->llmod);
    run_once_fn_->addFnAttr(llvm::Attribute::NoInline);
    llvm::Value* const table = run_once_fn_->getArg(0);
    llvm::Value* const count = run_once_fn_->getArg(1);
    llvm::Value* const wheel = run_once_fn_->getArg(2);
    table->setName("table");
    count->setName("count");
    wheel->setName("wheel");

    llvm::IRBuilder<> tb(ctx_);
    auto* const       entry = llvm::BasicBlock::Create(ctx_, "entry", run_once_fn_);
    auto* const       head  = llvm::BasicBlock::Create(ctx_, "once.head", run_once_fn_);
    auto* const       body  = llvm::BasicBlock::Create(ctx_, "once.body", run_once_fn_);
    auto* const       done  = llvm::BasicBlock::Create(ctx_, "once.done", run_once_fn_);
    tb.SetInsertPoint(entry);
    tb.CreateBr(head);
    tb.SetInsertPoint(head);
    auto* const i = tb.CreatePHI(i32, 2);
    i->addIncoming(tb.getInt32(0), entry);
    tb.CreateCondBr(tb.CreateICmpULT(i, count), body, done);
    tb.SetInsertPoint(body);
    auto* const  row = wait_elem_ty();
    llvm::Value* r   = tb.CreateInBoundsGEP(row, table, i);
    llvm::Value* fn  = tb.CreateLoad(ptr, tb.CreateStructGEP(row, r, 0));
    llvm::Value* arg = tb.CreateLoad(ptr, tb.CreateStructGEP(row, r, 1));
    llvm::Value* hdl = tb.CreateCall(ramp, fn, { arg, wheel });
    tb.CreateCall(coro_intr(llvm::Intrinsic::coro_resume), { hdl });
    tb.CreateCall(coro_intr(llvm::Intrinsic::coro_destroy), { hdl });
    i->addIncoming(tb.CreateAdd(i, tb.getInt32(1)), body);
    tb.CreateBr(head);
    tb.SetInsertPoint(done);
    tb.CreateRetVoid();
    return run_once_fn_;
}

// Run every group of `which` once: a call of `clotho.run_once` on the table of the
// groups, which is built the first time and shared by every site that runs them
// (the Preponed groups run at time zero and at every step).
//
// A group of these activations is lowered as a coroutine whose body has no timing,
// so one resume runs it to completion and destroy releases the frame.
void Backend::emit_run_once(Activation which, llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
    auto it = run_tables_.find(which);
    if (it == run_tables_.end()) {
        const auto                   all = mod_.processes();
        std::vector<llvm::Constant*> rows;
        for (uint32_t g = 0; g < procs.size(); ++g) {
            if (all[sched_[g].front].activation != which) {
                continue;
            }
            rows.push_back(llvm::ConstantStruct::get(wait_elem_ty(), { sym(procs[g]), ramp_arg(g) }));
        }
        llvm::GlobalVariable* table = nullptr;
        if (!rows.empty()) {
            auto* const arr = llvm::ArrayType::get(wait_elem_ty(), rows.size());
            table           = new llvm::GlobalVariable(root_env_->llmod, arr, /*isConstant=*/true,
                                                       llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(arr, rows),
                                                       "clotho.run." + std::to_string(static_cast<unsigned>(which)));
        }
        it = run_tables_.emplace(which, std::make_pair(table, static_cast<uint32_t>(rows.size()))).first;
    }
    const auto& [table, count] = it->second;
    if (table == nullptr) {
        return;
    }
    b_.CreateCall(run_once_fn(), { table, b_.getInt32(count), wheel });
}

// The seed. Every group the drain runs (not the ones that run once, above) has a row
// of `clotho.seed.rows`, in the order the groups were created in:
//
//   { ramp, first argument, wake word, kind, subscriptions }
//
// `clotho.seed` makes the group's handle (the ramp's frame, or for a thunk group, with
// no ramp, the address of its wake pair with bit 0 set), sets its wake word if it
// starts now, and queues it on its ring in its turn. The groups that start after every
// other body (always_comb, LRM 9.2.2.2) are queued afterwards, in `clotho.seed.late`
// order, and the buckets then take the subscriptions: `clotho.seed.slots` lists, row
// after row, the slot of each static bucket that holds the group's { handle, flag }
// entry. A persistent event waiter is linked onto its object in `clotho.seed.events`
// order, which is the order of the waiter list the straight-line code made.
//
// Nothing is decided at run time that the compile did not: which slot a subscription
// fills, in what order the buckets' entries land (immaterial: each slot is written once
// and nothing scans before the drain), which ring a group goes on.
void Backend::emit_seed(llvm::Value* wheel, const std::vector<llvm::Function*>& procs) {
    const auto all      = mod_.processes();
    auto* const ptr_ty  = llvm::PointerType::getUnqual(ctx_);
    auto* const i32     = b_.getInt32Ty();
    auto* const i64     = b_.getInt64Ty();
    auto* const null_ptr = llvm::ConstantPointerNull::get(ptr_ty);

    struct Row {
        llvm::Constant* ramp = nullptr;
        llvm::Constant* arg  = nullptr;
        llvm::Constant* wake = nullptr;
        uint32_t        kind = 0;
        uint32_t        subs = 0;
    };
    std::vector<Row>      rows;
    std::vector<uint32_t> row_of(procs.size(), ~0u);
    std::vector<uint32_t> late;
    std::array<bool, 4>   rings{};

    // Schedule groups with an always_comb / always_latch member start after every other
    // body. A fused group is ONE body, so a continuous assignment fused with such a
    // procedure starts with it: 4.9.1 and 4.7 leave the order of its time-zero
    // evaluation open, and keeping the fusion is what the procedure's cone is worth.
    // `g` numbers SCHEDULE groups, which are fewer than the fusion groups once rows
    // merge, so the members are those of the group's rows, never `groups_[g]`. The
    // rows of a schedule group share one handle and one seed decision, so one late
    // row starts them all late.
    const auto starts_late = [&](uint32_t g) {
        return std::any_of(sched_[g].rows.begin(), sched_[g].rows.end(), [&](uint32_t row) {
            return std::any_of(groups_[row].begin(), groups_[row].end(),
                               [&](uint32_t m) { return all[m].starts_after_procedures; });
        });
    };
    for (uint32_t g = 0; g < procs.size(); ++g) {
        const uint32_t k = sched_[g].front;
        const Process& p = all[k];
        switch (p.activation) {
            case Activation::PreInit:   // run once, before the seed
            case Activation::Preponed:  // run once per timestep from the post-tick block
            case Activation::Observed:  // run explicitly at each settled point
            case Activation::ReNba:     // run explicitly in the Re-NBA region
            case Activation::Final:     // deferred to the end of simulation
                continue;
            default:
                break;
        }
        Row row;
        row.wake = sched_flag_ptr(k);
        if (!sched_[g].thunk) {
            row.ramp = sym(procs[g]);
            row.arg  = ramp_arg(g);
        }
        uint32_t ring = RingNone;
        if (p.activation == Activation::Always && !p.wait_events.empty()) {
            // Subscribed below. A continuously-active process (continuous assign / port
            // copy / always_comb) also runs once now, so it settles from the pre-init
            // values rather than waiting for an input edge that a declaration
            // initializer never generates. It runs body-first then parks on its wait,
            // so later input changes still wake it.
            //
            // Marked scheduled as it is enqueued: a seed-time store from a process
            // draining earlier in the same first drain (an initial block) services this
            // static subscription and would re-push the already-queued handle without
            // the flag, so N such processes overflow the nproc-sized active queue. The
            // flag is cleared when the body reaches its wait-suspend (lower_process).
            if (p.self_start) {
                row.kind |= kSeedFlag;
                if (starts_late(g)) {
                    // always_comb / always_latch (LRM 9.2.2.2): queued behind every other
                    // body, so it runs once they have all started.
                    row.kind |= kSeedLate;
                    ring = p.reactive_home ? RingCont : RingActive;
                } else if (p.reactive_home) {
                    // A reactive-home group -- a program's continuous assign (LRM
                    // 24.3.1) -- settles in slot 0's REACTIVE region rather than its
                    // Active one, so its stores land after the design has settled and
                    // design readers of what it drives are woken into the Active region
                    // of the next iteration of 4.4.2's loop. A ring entry is a bare
                    // handle either way: the home tag belongs to a PARKED handle. Its
                    // ring is the reactive CONTINUOUS one, which the reactive drain
                    // empties before the reactive ring a program `initial` is seeded
                    // on, so no program code starts ahead of the copy that feeds it.
                    ring = RingCont;
                } else {
                    ring = RingActive;
                }
            }
        } else if (p.activation == Activation::Reactive) {
            // A program `initial` (LRM 24.3) starts in the Reactive region, so it is
            // seeded onto the reactive ring instead of the active one. Marked scheduled
            // as it is enqueued, for the reason above: anything that pushes it again
            // before it runs double-queues it, here by a FORCE-WAKE (`$exit`, LRM 24.7,
            // names every `initial` of its program instance by static control block).
            row.kind |= kSeedFlag;
            ring = RingReactive;
        } else if (starts_late(g)) {
            // An always_comb / always_latch that reads nothing has no wait list; it
            // still runs once, after the others.
            row.kind |= kSeedLate;
            ring = p.reactive_home ? RingCont : RingActive;
        } else {
            ring = RingActive;
        }
        row.kind |= ring << kSeedRingShift;
        rings[ring] = true;
        // A static subscription of a reactive-home group records its handle with the
        // continuous home tag, the only place the home can be stated: the always-loop's
        // park never re-records it.
        if (p.reactive_home && design_uses_reactive_) {
            row.kind |= kSeedTag;
        }
        if ((row.kind & kSeedLate) != 0) {
            late.push_back(static_cast<uint32_t>(rows.size()));
        }
        row_of[g] = static_cast<uint32_t>(rows.size());
        rows.push_back(row);
    }
    if (rows.empty()) {
        return;
    }

    // The static subscriptions, in topological order of the blocking-write wake graph
    // (compute_subscription_order): bucket entry order is subscription order, the FIFO
    // drain pops in bucket order, so a settle wave runs producers before consumers and
    // each acyclic cascade settles in one wave. A schedule group subscribes once, at
    // its earliest member's rank, with its canonical event set and its flag/handle.
    std::vector<bool>                         subscribed(procs.size(), false);
    std::vector<std::vector<llvm::Constant*>> slots_of(procs.size());
    struct EventSub {
        uint32_t  row;
        StaticSub sub;
    };
    std::vector<EventSub> event_subs;
    for (const uint32_t k : sub_order_) {
        const uint32_t g = sched_of_[group_of_[k]];
        const uint32_t f = sched_[g].front;
        if (subscribed[g] || all[f].activation != Activation::Always || all[f].wait_events.empty()) {
            continue;
        }
        subscribed[g] = true;
        assert(row_of[g] != ~0u && "a group that subscribes is seeded");
        assert((!all[f].reactive_home || !design_uses_reactive_ || design_uses_reactive_cont_) &&
               "a continuous tag needs the continuous ring's strip");
        for (const WaitEvent& w : sched_[g].events) {
            StaticSub s = plan_static_subscribe(w.signal, static_cast<uint32_t>(w.edge));
            if (s.is_event) {
                event_subs.push_back({ row_of[g], s });
            } else {
                slots_of[g].push_back(s.slot);
            }
        }
    }
    std::vector<llvm::Constant*> slots;
    for (uint32_t g = 0; g < procs.size(); ++g) {
        if (row_of[g] == ~0u) {
            continue;
        }
        rows[row_of[g]].subs = static_cast<uint32_t>(slots_of[g].size());
        slots.insert(slots.end(), slots_of[g].begin(), slots_of[g].end());
    }

    // The tables.
    auto* const row_ty = llvm::StructType::get(ctx_, { ptr_ty, ptr_ty, ptr_ty, i32, i32 });
    llvm::GlobalVariable* const row_gv = [&] {
        std::vector<llvm::Constant*> v;
        for (const Row& r : rows) {
            v.push_back(llvm::ConstantStruct::get(
              row_ty, { r.ramp != nullptr ? r.ramp : null_ptr, r.arg != nullptr ? r.arg : null_ptr,
                        r.wake, b_.getInt32(r.kind), b_.getInt32(r.subs) }));
        }
        auto* const arr = llvm::ArrayType::get(row_ty, v.size());
        return new llvm::GlobalVariable(root_env_->llmod, arr, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(arr, v), "clotho.seed.rows");
    }();
    auto* const handles_ty = llvm::ArrayType::get(ptr_ty, rows.size());
    auto* const handles_gv = new llvm::GlobalVariable(root_env_->llmod, handles_ty, /*isConstant=*/false,
                                                      llvm::GlobalValue::PrivateLinkage,
                                                      llvm::ConstantAggregateZero::get(handles_ty), "clotho.seed.handles");
    const auto table = [&](llvm::Type* elem, const std::vector<llvm::Constant*>& v, const char* name) {
        auto* const arr = llvm::ArrayType::get(elem, v.size());
        return new llvm::GlobalVariable(root_env_->llmod, arr, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                                        llvm::ConstantArray::get(arr, v), name);
    };
    llvm::GlobalVariable* slots_gv = slots.empty() ? nullptr : table(ptr_ty, slots, "clotho.seed.slots");
    llvm::GlobalVariable* late_gv  = nullptr;
    if (!late.empty()) {
        std::vector<llvm::Constant*> v;
        for (const uint32_t r : late) {
            v.push_back(b_.getInt32(r));
        }
        late_gv = table(i32, v, "clotho.seed.late");
    }
    auto* const           event_ty = llvm::StructType::get(ctx_, { ptr_ty, ptr_ty, i32, i32 });
    llvm::GlobalVariable* events_gv = nullptr;
    if (!event_subs.empty()) {
        std::vector<llvm::Constant*> v;
        for (const EventSub& e : event_subs) {
            v.push_back(llvm::ConstantStruct::get(
              event_ty, { e.sub.slot, e.sub.obj, b_.getInt32(e.row), b_.getInt32(e.sub.by_handle ? 1u : 0u) }));
        }
        events_gv = table(event_ty, v, "clotho.seed.events");
    }
    const bool any_tag = std::any_of(rows.begin(), rows.end(), [](const Row& r) { return (r.kind & kSeedTag) != 0; });

    // `clotho.seed(wheel)`.
    InRoot in_root(*this);
    const auto ip = b_.saveIP();
    auto* const seed = llvm::Function::Create(llvm::FunctionType::get(b_.getVoidTy(), { ptr_ty }, false),
                                              llvm::Function::InternalLinkage, "clotho.seed", root_env_->llmod);
    seed->addFnAttr(llvm::Attribute::NoInline);
    llvm::Value* const w = seed->getArg(0);
    w->setName("wheel");
    const auto block = [&](const char* name) { return llvm::BasicBlock::Create(ctx_, name, seed); };
    b_.SetInsertPoint(block("entry"));

    // Queue `h` on the ring `ring` names (an i32 value); a row of no ring queues nothing.
    const auto push = [&](llvm::Value* ring, llvm::Value* h) {
        auto* const done = block("seed.queued");
        auto* const sw   = b_.CreateSwitch(ring, done, 3);
        const auto  arm  = [&](SeedRing r, const char* name, auto&& emit) {
            if (!rings[r]) {
                return;
            }
            auto* const bb = block(name);
            sw->addCase(b_.getInt32(r), bb);
            b_.SetInsertPoint(bb);
            emit(b_, h);
            b_.CreateBr(done);
        };
        arm(RingActive, "seed.active", [&](llvm::IRBuilder<>& b, llvm::Value* v) { emit_active_push(b, v); });
        arm(RingReactive, "seed.reactive", [&](llvm::IRBuilder<>& b, llvm::Value* v) { emit_reactive_push(b, v); });
        arm(RingCont, "seed.cont", [&](llvm::IRBuilder<>& b, llvm::Value* v) { emit_reactive_cont_push(b, v); });
        b_.SetInsertPoint(done);
    };
    const auto ring_of = [&](llvm::Value* kind) {
        return b_.CreateAnd(b_.CreateLShr(kind, b_.getInt32(kSeedRingShift)), b_.getInt32(kSeedRingMask));
    };
    const auto row_field = [&](llvm::Value* i, uint32_t f) {
        return b_.CreateStructGEP(row_ty, b_.CreateInBoundsGEP(row_ty, row_gv, i), f);
    };
    const auto handle_slot = [&](llvm::Value* i) { return b_.CreateInBoundsGEP(handles_ty, handles_gv, { b_.getInt32(0), i }); };
    // for (i = 0; i < n; ++i) body(i)
    const auto count_loop = [&](uint32_t n, const char* name, auto&& body) {
        auto* const pre  = b_.GetInsertBlock();
        auto* const head = block((std::string(name) + ".head").c_str());
        auto* const bb   = block((std::string(name) + ".body").c_str());
        auto* const done = block((std::string(name) + ".done").c_str());
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* const i = b_.CreatePHI(i32, 2);
        i->addIncoming(b_.getInt32(0), pre);
        b_.CreateCondBr(b_.CreateICmpULT(i, b_.getInt32(n)), bb, done);
        b_.SetInsertPoint(bb);
        body(static_cast<llvm::Value*>(i));
        i->addIncoming(b_.CreateAdd(i, b_.getInt32(1)), b_.GetInsertBlock());
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
    };
    // The handle a subscription records: the group's, with the continuous home tag when
    // the row says so.
    const auto sub_handle = [&](llvm::Value* i, llvm::Value* kind) -> llvm::Value* {
        llvm::Value* h = b_.CreateLoad(ptr_ty, handle_slot(i));
        if (!any_tag) {
            return h;
        }
        llvm::Value* tag = b_.CreateSelect(b_.CreateICmpNE(b_.CreateAnd(kind, b_.getInt32(kSeedTag)), b_.getInt32(0)),
                                           b_.getInt64(kReactiveContTag), b_.getInt64(0));
        return b_.CreateIntToPtr(b_.CreateOr(b_.CreatePtrToInt(h, i64), tag), ptr_ty);
    };

    // Create and start, in the order of the groups.
    count_loop(static_cast<uint32_t>(rows.size()), "seed", [&](llvm::Value* i) {
        llvm::Value* ramp = b_.CreateLoad(ptr_ty, row_field(i, 0));
        llvm::Value* wake = b_.CreateLoad(ptr_ty, row_field(i, 2));
        llvm::Value* kind = b_.CreateLoad(i32, row_field(i, 3));
        auto* const  thunk = block("seed.thunk");
        auto* const  coro  = block("seed.coro");
        auto* const  join  = block("seed.join");
        b_.CreateCondBr(b_.CreateICmpEQ(ramp, llvm::ConstantPointerNull::get(ptr_ty)), thunk, coro);
        // A thunk group is not ramped (no frame): its "handle" is the address of its wake
        // pair with the low bit set, so the drain calls the entry stored there directly
        // instead of resuming a coroutine. Pairs are aligned, so bit 0 is a free tag; coro
        // handles (rt_alloc) leave it clear. The tagged pointer flows through subscription
        // and queue push unchanged.
        b_.SetInsertPoint(thunk);
        llvm::Value* tagged = b_.CreateIntToPtr(b_.CreateOr(b_.CreatePtrToInt(wake, i64), b_.getInt64(1)), ptr_ty);
        b_.CreateBr(join);
        b_.SetInsertPoint(coro);
        llvm::Value* arg  = b_.CreateLoad(ptr_ty, row_field(i, 1));
        llvm::Value* made = b_.CreateCall(llvm::FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false), ramp, { arg, w });
        b_.CreateBr(join);
        b_.SetInsertPoint(join);
        auto* const h = b_.CreatePHI(ptr_ty, 2);
        h->addIncoming(tagged, thunk);
        h->addIncoming(made, coro);
        b_.CreateStore(h, handle_slot(i));
        // The time-zero settle run must evaluate every member.
        auto* const flag = block("seed.flag");
        auto* const next = block("seed.next");
        b_.CreateCondBr(b_.CreateICmpNE(b_.CreateAnd(kind, b_.getInt32(kSeedFlag)), b_.getInt32(0)), flag, next);
        b_.SetInsertPoint(flag);
        b_.CreateStore(b_.getInt64(1 | unclassified_trig_bits()), wake);
        b_.CreateBr(next);
        b_.SetInsertPoint(next);
        // A group that starts late is queued below; every other in its turn.
        llvm::Value* in_turn = b_.CreateSelect(b_.CreateICmpNE(b_.CreateAnd(kind, b_.getInt32(kSeedLate)), b_.getInt32(0)),
                                               b_.getInt32(RingNone), ring_of(kind));
        push(in_turn, h);
    });

    // The time-zero run of an always_comb / always_latch (LRM 9.2.2.2, 9.2.2.3): "after
    // all initial and always procedures have been started so that the outputs of the
    // procedure are consistent with the inputs". Both rings are FIFO, so enqueuing
    // these after every other body makes each of them run after those have started. A
    // continuous assignment is not a procedure and keeps its source position above.
    if (late_gv != nullptr) {
        count_loop(static_cast<uint32_t>(late.size()), "late", [&](llvm::Value* j) {
            llvm::Value* i = b_.CreateLoad(i32, b_.CreateInBoundsGEP(late_gv->getValueType(), late_gv, { b_.getInt32(0), j }));
            push(ring_of(b_.CreateLoad(i32, row_field(i, 3))), b_.CreateLoad(ptr_ty, handle_slot(i)));
        });
    }

    // The buckets' entries: each row's slots are the next `subs` of the list.
    if (slots_gv != nullptr) {
        auto* const pre   = b_.GetInsertBlock();
        auto* const head  = block("sub.head");
        auto* const outer = block("sub.row");
        auto* const ihead = block("sub.slot.head");
        auto* const ibody = block("sub.slot.body");
        auto* const latch = block("sub.next");
        auto* const done  = block("sub.done");
        b_.CreateBr(head);
        b_.SetInsertPoint(head);
        auto* const i = b_.CreatePHI(i32, 2);
        auto* const j = b_.CreatePHI(i32, 2);
        i->addIncoming(b_.getInt32(0), pre);
        j->addIncoming(b_.getInt32(0), pre);
        b_.CreateCondBr(b_.CreateICmpULT(i, b_.getInt32(static_cast<uint32_t>(rows.size()))), outer, done);
        b_.SetInsertPoint(outer);
        llvm::Value* kind = b_.CreateLoad(i32, row_field(i, 3));
        llvm::Value* subs = b_.CreateLoad(i32, row_field(i, 4));
        llvm::Value* wake = b_.CreateLoad(ptr_ty, row_field(i, 2));
        llvm::Value* h    = sub_handle(i, kind);
        b_.CreateBr(ihead);
        b_.SetInsertPoint(ihead);
        auto* const t = b_.CreatePHI(i32, 2);
        t->addIncoming(b_.getInt32(0), outer);
        b_.CreateCondBr(b_.CreateICmpULT(t, subs), ibody, latch);
        b_.SetInsertPoint(ibody);
        llvm::Value* slot = b_.CreateLoad(ptr_ty, b_.CreateInBoundsGEP(slots_gv->getValueType(), slots_gv,
                                                                       { b_.getInt32(0), b_.CreateAdd(j, t) }));
        auto* const  elem = wait_elem_ty();
        b_.CreateStore(h, b_.CreateStructGEP(elem, slot, 0));
        b_.CreateStore(wake, b_.CreateStructGEP(elem, slot, 1));
        t->addIncoming(b_.CreateAdd(t, b_.getInt32(1)), ibody);
        b_.CreateBr(ihead);
        b_.SetInsertPoint(latch);
        i->addIncoming(b_.CreateAdd(i, b_.getInt32(1)), latch);
        j->addIncoming(b_.CreateAdd(j, subs), latch);
        b_.CreateBr(head);
        b_.SetInsertPoint(done);
    }

    // The persistent event waiters: a node of the contiguous pool, linked onto its
    // object's list. The object is a constant, or the one the variable's own handle names
    // now -- after the declaration initializers ran, which can have re-pointed it.
    if (events_gv != nullptr) {
        count_loop(static_cast<uint32_t>(event_subs.size()), "evsub", [&](llvm::Value* t) {
            llvm::Value* e = b_.CreateInBoundsGEP(event_ty, events_gv, t);
            const auto   field = [&](uint32_t f, llvm::Type* ty) { return b_.CreateLoad(ty, b_.CreateStructGEP(event_ty, e, f)); };
            llvm::Value* node = field(0, ptr_ty);
            llvm::Value* obj  = field(1, ptr_ty);
            llvm::Value* row  = field(2, i32);
            llvm::Value* by   = field(3, i32);
            auto* const  from_handle = block("evsub.handle");
            auto* const  have        = block("evsub.obj");
            auto* const  entry       = b_.GetInsertBlock();
            b_.CreateCondBr(b_.CreateICmpNE(by, b_.getInt32(0)), from_handle, have);
            b_.SetInsertPoint(from_handle);
            llvm::Value* named = event_obj_of(b_.CreateLoad(i64, obj));
            auto* const  named_bb = b_.GetInsertBlock();
            b_.CreateBr(have);
            b_.SetInsertPoint(have);
            auto* const o = b_.CreatePHI(ptr_ty, 2);
            o->addIncoming(obj, entry);
            o->addIncoming(named, named_bb);
            llvm::Value* kind = b_.CreateLoad(i32, row_field(row, 3));
            llvm::Value* wake = b_.CreateLoad(ptr_ty, row_field(row, 2));
            // A persistent subscription is never unlinked, so it needs no owner.
            emit_event_link(o, node, sub_handle(row, kind), wake, /*record_owner=*/false);
        });
    }
    b_.CreateRetVoid();

    b_.restoreIP(ip);
    b_.CreateCall(seed, { wheel });
}

} // namespace clotho::ir::backend
