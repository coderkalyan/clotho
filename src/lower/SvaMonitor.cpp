// Concurrent-assertion monitors: automaton stepping, attempt rings and sampled values as IR.

#include "lower/Lowerer.h"

namespace clotho::lower {

// A hidden monitor state signal, powering up to 0. Two-state throughout: a
// state mask is a set, never a design value, so it has no x/z to carry.
ir::Signal Lowerer::new_monitor_state(std::string name, ir::Ref type, ir::SourceLoc l) {
    ir::SignalInfo info;
    info.name = prefix_ + std::move(name);
    info.loc  = l;
    info.type = type;
    info.dir  = ir::SignalInfo::Direction::Internal;

    return register_signal(std::move(info));
}

// Store `value`, or hold, or reset. Three outcomes rather than two, because
// the monitor runs every time slot now that it lives in Observed: no clocking
// edge is not a cycle at all, so the state keeps what it holds; an edge with
// the assertion disabled is LRM 16.15's reset (emitted_step's early return);
// an edge with it enabled is the step. Selects rather than branches, so the
// step stays straight-line.
//
// `active` already has the edge folded into it, so whenever the outer select
// takes `value` the inner one is testing the enable alone.
void Lowerer::store_gated(ir::Signal    sig,
                          ir::Index     value,
                          ir::Index     active,
                          ir::Index     edge,
                          uint32_t      width,
                          ir::Builder&  b,
                          ir::SourceLoc l) {
    const ir::Index dst     = b.signal(sig, l);
    const ir::Index stepped = b.select(active, value, zero_mask(width, b, l), l);
    const ir::Index cur     = load_signal(sig, b, l);
    const ir::Index val     = b.select(edge, stepped, cur, l);
    b.binary(ir::Inst::Tag::Store, dst, val, l);
}

// This cycle's attempt id: one bit rotating through the ids, so an id is handed
// out again only long after its attempt finished -- attempt_ring_size sizes the
// gap. The signal powers up to 0, which is no attempt at all, so the first
// attempt takes bit 0.
ir::Index Lowerer::next_attempt_id(const MonitorState& st,
                                   uint32_t            ring,
                                   ir::Index           active,
                                   ir::Index           edge,
                                   ir::Builder&        b,
                                   ir::SourceLoc       l) {
    const ir::Index held   = load_signal(st.idnow, b, l);
    const ir::Index have   = mask_any(held, b, l);
    const ir::Index first  = slot_mask(0, ring, b, l);
    const ir::Index id_now = b.select(have, held, first, l);
    store_gated(st.idnow, rotl1(id_now, ring, b, l), active, edge, ring, b, l);

    return id_now;
}

// Carry the attempt ids along the same edges the trigger's mask just took: state
// `s` inherits every attempt sitting in a predecessor that was live last cycle,
// and a first-state also takes the attempt starting now. Two attempts meeting in
// one state is not a conflict -- the state simply holds both bits, and both go on
// matching. Gating by `next` drops the ones whose guard failed.
//
// A SET per state and not a COUNT per state, and that difference is the whole
// construction. Union is idempotent, so two forked threads of ONE attempt that
// reconverge collapse back to a single id, while two distinct attempts stay
// distinct. Counting threads instead would read two matches off a state that two
// paths of one attempt arrive in, and LRM 16.9's match is a (start, end) pair and
// not a path -- so a count of matches has to be a count of IDS, derived from
// identity. libsva's reference monitor carries the same rule.
//
// `fired` comes back as the union over the trigger's `last` states: whose
// antecedent completed a match this cycle, and so which attempts a seed owes an
// obligation to.
std::vector<ir::Index> Lowerer::advance_attempt_ids(const ir::AssertionMonitor& mon,
                                                    const MonitorState&         st,
                                                    uint32_t                    ring,
                                                    ir::Index                   cur_t,
                                                    ir::Index                   next,
                                                    ir::Index                   id_now,
                                                    ir::Index&                  fired,
                                                    ir::Builder&                b,
                                                    ir::SourceLoc               l) {
    const uint32_t         trig = mon.trigger.state_count;
    std::vector<ir::Index> tid_next(trig);

    fired = zero_mask(ring, b, l);
    for (uint32_t s = 0; s < trig; ++s) {
        ir::Index acc = zero_mask(ring, b, l);
        for (uint32_t p = 0; p < trig; ++p) {
            const std::span<const uint64_t> row = table_row(mon.trigger.follow, p, mon.trigger.words);
            if ((row[s / 64] & (uint64_t{ 1 } << (s % 64))) == 0) {
                continue;
            }
            const ir::Index live    = mask_bit(cur_t, p, trig, b, l);
            const ir::Index id_at   = load_signal(st.tid[p], b, l);
            const ir::Index none    = zero_mask(ring, b, l);
            const ir::Index carried = b.select(live, id_at, none, l);
            acc                     = b.binary(ir::Inst::Tag::Bor, acc, carried, l);
        }
        if ((mon.trigger.first[s / 64] & (uint64_t{ 1 } << (s % 64))) != 0) {
            acc = b.binary(ir::Inst::Tag::Bor, acc, id_now, l);
        }
        const ir::Index live = mask_bit(next, s, trig, b, l);
        const ir::Index none = zero_mask(ring, b, l);
        tid_next[s]          = b.select(live, acc, none, l);

        // Whose antecedent completed a match this cycle.
        if ((mon.trigger.last[s / 64] & (uint64_t{ 1 } << (s % 64))) != 0) {
            fired = b.binary(ir::Inst::Tag::Bor, fired, tid_next[s], l);
        }
    }

    return tid_next;
}

// `advance`, transposed. libsva walks the live states and ORs in each one's
// follow row; there is no run-time state set here, so this folds over every
// state instead and selects that state's constant row by its own liveness
// bit. A state with no successors contributes nothing and drops out at
// compile time. See concurrent-assertions.md 7.2.
ir::Index Lowerer::automaton_advance(const ir::AssertionAutomaton& au,
                                     ir::Index                     current,
                                     ir::Index                     satisfied,
                                     ir::Builder&                  b,
                                     ir::SourceLoc                 l) {
    const uint32_t  states = au.state_count;
    const ir::Index zero   = zero_mask(states, b, l);

    ir::Index next = zero;
    bool      any  = false;
    for (uint32_t s = 0; s < states; ++s) {
        const std::span<const uint64_t> row = table_row(au.follow, s, au.words);
        if (row_is_zero(row)) {
            continue;
        }
        const ir::Index live  = mask_bit(current, s, states, b, l);
        const ir::Index bits  = b.constant(mask_const(row, states), l);
        const ir::Index taken = b.select(live, bits, zero, l);
        next                  = any ? b.binary(ir::Inst::Tag::Bor, next, taken, l) : taken;
        any                   = true;
    }

    return b.binary(ir::Inst::Tag::Band, next, satisfied, l);
}

// `satisfy`: the states whose guard holds under this cycle's root valuation.
// Folds over guards rather than gathering per state -- that is what
// guard_states is for. A cube is a compile-time literal list, so a guard's
// contribution is a constant mask selected by a chain of ANDs over the root
// bits. The reserved unsatisfiable cube carries no surviving state, so its
// mask is zero and it folds away here along with every other unused guard.
//
// A complemented literal (ir::AssertionLiteral::negated) is the two-valued
// complement of the collapsed root bit, and Bxor against 1 is exactly that. It is
// NOT the user's `!`, and the difference matters under x.
//
// libsva's IR makes atoms two-valued a PRECONDITION: a three-valued front-end
// owes it negation normal form, with each polarity supplied as its own atom, so
// clotho's `!a` is an atom over `!a` whose Lnot runs before the four-to-two
// collapse (eval_atom) and never appears as a complemented literal here. What
// does appear is libsva's own structural negation -- the determinisation's cell
// splitter builds `a` and NOT-`a` cells to partition the valuation space
// (Glushkov.zig's guardWithout), and that partition is over the COLLAPSED
// decision "did the guard hold". Under x the guard did not hold, so the
// NOT-`a` cell is the one enabled, and complementing the collapsed bit is what
// puts the attempt in it.
//
// Evaluating `!a` three-valuedly here instead leaves BOTH cells unsatisfied under
// x -- a gap rather than a partition -- and the attempt dies with no successor.
// Measured: it turns `not a` over an x-valued `a` from a hold into a failure.
ir::Index Lowerer::automaton_satisfy(const ir::AssertionMonitor&   mon,
                                     const ir::AssertionAutomaton& au,
                                     std::span<const ir::Index>    roots,
                                     ir::Builder&                  b,
                                     ir::SourceLoc                 l) {
    const uint32_t  states = au.state_count;
    const ir::Index zero   = zero_mask(states, b, l);

    ir::Index sat = zero;
    bool      any = false;
    for (uint32_t g = 0; g < mon.cubes.size(); ++g) {
        const std::span<const uint64_t> row = table_row(au.guard_states, g, au.words);
        if (row_is_zero(row)) {
            continue;
        }
        const ir::Index          carriers = b.constant(mask_const(row, states), l);
        const ir::AssertionCube& cube     = mon.cubes[g];

        ir::Index holds{};
        bool      conjoined = false;
        for (uint32_t i = 0; i < cube.len; ++i) {
            const ir::AssertionLiteral& lit = mon.literals[cube.start + i];
            ir::Index                   bit = roots[lit.root];
            if (lit.negated) {
                bit = bit_not(bit, b, l);
            }
            holds     = conjoined ? b.binary(ir::Inst::Tag::Band, holds, bit, l) : bit;
            conjoined = true;
        }
        // The empty cube is `true`, so its carriers are satisfied outright.
        const ir::Index add = conjoined ? b.select(holds, carriers, zero, l) : carriers;
        sat                 = any ? b.binary(ir::Inst::Tag::Bor, sat, add, l) : add;
        any                 = true;
    }

    return sat;
}

// One ring slot's verdict. `held` is whether the slot carries an attempt at all
// and `states` its obligation state vector after this cycle's step.
//
// Holds is reaching a `last` state and Fails is running out of states, and the
// two are taken in that order because they are not symmetric: an attempt can
// reach `last` in the same cycle its other threads die, and LRM 16.12 says that
// is a match. Pending is then whatever the slot still holds that neither
// resolved -- which is why occupancy has to be tracked rather than derived from
// `states`: an attempt whose first letter does not hold has an empty vector
// immediately, and that is a Fails, not an absence.
Lowerer::AttemptVerdict Lowerer::ring_verdict(ir::Index     held,
                                              ir::Index     states,
                                              ir::Index     last,
                                              ir::Builder&  b,
                                              ir::SourceLoc l) {
    const ir::Index done  = mask_any(b.binary(ir::Inst::Tag::Band, states, last, l), b, l);
    const ir::Index empty = bit_not(mask_any(states, b, l), b, l);

    AttemptVerdict v;
    v.holds = b.binary(ir::Inst::Tag::Band, held, done, l);
    v.fails =
      b.binary(ir::Inst::Tag::Band, held, b.binary(ir::Inst::Tag::Band, bit_not(done, b, l), empty, l), l);

    // Either resolution frees the slot, so Pending is the rest of it.
    const ir::Index resolved = b.binary(ir::Inst::Tag::Bor, v.holds, v.fails, l);
    v.pending                = b.binary(ir::Inst::Tag::Band, held, bit_not(resolved, b, l), l);

    return v;
}

// Whether an attempt sitting in obligation state `s` can still be there next
// cycle, i.e. whether the state needs a counter at all.
//
// A state in `last` has MATCHED and one in `dead` has FAILED, and either way
// the attempt resolves in the cycle it arrives and is dropped -- exactly
// Subsets.run, which re-adds a stepped subset to the live set only when it is
// neither. So such a state never carries a count into the next cycle. The two
// masks are disjoint by construction: `dead` is the states with no path left to
// an exit, and a `last` state is an exit.
bool Lowerer::counted_carries(const ir::AssertionAutomaton& au, uint32_t s) {
    const uint64_t bit = uint64_t{ 1 } << (s % 64);

    return (au.last[s / 64] & bit) == 0 && (au.dead[s / 64] & bit) == 0;
}

bool Lowerer::row_has(const std::vector<uint64_t>& row, uint32_t s) {
    return (row[s / 64] & (uint64_t{ 1 } << (s % 64))) != 0;
}

// The same test against a row that lives at `base` inside a flattened table,
// which is how `follow` is stored: one row of `words` per source state.
bool Lowerer::row_has_at(const std::vector<uint64_t>& table, size_t base, uint32_t s) {
    return (table[base + s / 64] & (uint64_t{ 1 } << (s % 64))) != 0;
}

// emit_action_pends' counted twin: `count` action-block executions instead of
// one per compile-time verdict bit. Each iteration is its own execution, which
// is what LRM 16.14.1 asks for and the whole reason the count is carried.
//
// `filter` gates the pends and nothing else, for emit_action_pends' reason: an
// assertion whose reports are switched off is still running.
void Lowerer::emit_counted_pends(ir::Index         count,
                                 ir::FunctionId    body,
                                 ir::Index         active,
                                 const ir::Signal* filter,
                                 ir::Builder&      b,
                                 ir::SourceLoc     l) {
    ir::Index take = active;
    if (filter != nullptr) {
        take = b.binary(ir::Inst::Tag::Band, take, bit_not(load_signal(*filter, b, l), b, l), l);
    }
    const ir::Index gated = b.select(take, count, zero_mask(counter_bits, b, l), l);

    repeat_verdict(
      gated, [&] { b.reactive_pend(body, l); }, b, l);
}

// The sampled value functions of LRM 16.9.3. $past is the primitive; every
// other one is a ring entry plus a comparison.
bool Lowerer::is_sampled_value_fn(std::string_view name) {
    return name == "$past" || name == "$sampled" || name == "$rose" || name == "$fell" || name == "$stable" ||
           name == "$changed";
}

// LRM 20.9's bit vector system functions: pure combinational reductions
// over one bit-vector operand (plus, for `$countbits`, its control
// arguments -- count_control_class requires each of those to be a
// constant 0/1/x/z, so they carry no runtime state of their own to
// sample). Used by reject_unsampled_call to let these through a sampled
// assertion expression: lower_bit_query, which already lowers the
// non-constant case of each in a plain (non-assertion) expression, is
// nothing but lower_expr on the operand plus the reduction op, so it
// needs no sampling logic of its own -- see the comment on
// reject_unsampled_call.
bool Lowerer::is_pure_bitvec_sysfn(std::string_view name) {
    return name == "$isunknown" || name == "$countones" || name == "$countbits" || name == "$onehot" ||
           name == "$onehot0";
}

// entries[n - 1] of the ring `live` keeps for `gate`: the value its shadow held
// n advances of that ring ago. A null `gate` names the ungated ring, which is
// what every $past without a third argument -- and every value change function,
// none of which takes one -- reads.
//
// Keyed by the PAIR and not by the variable alone. LRM 16.9.3 reads
// $past(v, n, g) at "the kth strictly prior time step in which the event ev iff
// g occurred", so a gated ring advances on a subset of the ticks and is a
// second history of the same variable. Keyed by pointer, so two textually
// identical gates at two call sites get two rings: state spent, never an answer
// changed, because both shift on the same condition.
//
// An entry carries the variable's OWN type, full width and full 4-state, for
// the same reason the shadow does -- a collapsed bit could answer neither
// $past(bus, 2) nor a whole-value $stable. Like the shadow it is a variable
// and not a net, so nothing drives it but the shift and the seed below.
//
// Every entry is SEEDED with the variable's time-zero value, so a $past
// deeper than the number of ticks that have elapsed reports the value the
// variable started the simulation with, not its type default. The difference
// is observable: for `logic [3:0] v = 4'd7`, $past(v, 2) is 7 at the second
// tick, not 4'bxxxx. An uninitialised ring happens to agree only when the
// variable is itself uninitialised, because then the type default and the
// time-zero value are both X. A gated ring seeds identically: a tick the
// gate held back is a tick that never happened for it, so its shallow
// entries stay at time zero for longer.
ir::Signal Lowerer::sva_history_slot(ir::Signal live, const Expression* gate, uint32_t n, ir::Builder& b) {
    SvaHistory& hist = *sva_history_;

    size_t at = hist.rings.size();
    for (size_t i = 0; i < hist.rings.size(); i++) {
        if (hist.rings[i].live == live && hist.rings[i].gate == gate) {
            at = i;
            break;
        }
    }
    if (at == hist.rings.size()) {
        // Evaluated BEFORE the ring is appended, and `at` taken after: a gate
        // is an ordinary assertion boolean and may hold a $past of its own, so
        // evaluating one can append rings, and a half-built entry is exactly
        // what that growth would move.
        const ir::Index gate_value =
          gate != nullptr ? eval_atom(sva::Atom{ .expr = gate }, true, b) : ir::Index{};
        at = hist.rings.size();
        hist.rings.push_back(SvaHistory::Ring{ .live = live, .gate = gate, .gate_value = gate_value });
    }
    std::vector<ir::Signal>& entries = hist.rings[at].entries;

    while (entries.size() < n) {
        // Re-read the live info every round: register_signal can reallocate
        // the signal table, so a reference hoisted out of the loop would
        // dangle after the first entry.
        //
        // A gated ring is a second one over the same variable, so its name
        // carries which; an ungated ring keeps the name every monitor without
        // a gate already emits.
        ir::SignalInfo info;
        info.name = hist.base + "past." + module_.signal(live).name +
                    (gate != nullptr ? ".gate" + std::to_string(at) : std::string()) + "." +
                    std::to_string(entries.size());
        info.loc  = hist.loc;
        info.type = module_.signal(live).type;
        const ir::Signal entry = register_signal(std::move(info));
        entries.push_back(entry);
        sva_history_seeds_.emplace_back(live, entry);
    }

    return entries[n - 1];
}

// Store when `when` holds, hold otherwise. store_gated's sibling for state
// that must survive `disable iff` rather than be reset by it.
void Lowerer::store_when(ir::Signal sig, ir::Index value, ir::Index when, ir::Builder& b, ir::SourceLoc l) {
    const ir::Index dst = b.signal(sig, l);
    const ir::Index val = b.select(when, value, load_signal(sig, b, l), l);
    b.binary(ir::Inst::Tag::Store, dst, val, l);
}

// Advance every ring by one clock tick. Emitted at the very END of the monitor
// body, and that placement is the whole correctness argument:
//
//  * The roots above have ALREADY READ the rings, so every read this cycle saw
//    the PRE-shift contents -- $past(v) is the shadow from the previous tick,
//    not this one. Shifting before evaluation would make ring[0] this tick's
//    own shadow and leave every function here off by exactly one tick.
//  * Within the shift, entries are copied in DESCENDING order so each is read
//    before it is overwritten; stores in a process body are blocking, so an
//    ascending copy would smear ring[0] through the whole ring.
//
// Gated on `edge` alone and deliberately NOT on `active`: LRM 16.9.3 counts
// "the most recent strictly prior time step in which the clocking event
// occurred", which a `disable iff` does not stop -- the same reason the
// clkprev store is ungated. A time step with no clocking edge holds every
// entry unchanged, and THAT is what makes this history per tick rather than
// per time step.
//
// A ring carrying a $past gating expression advances on a SUBSET of those
// ticks, because 16.9.3 counts the prior steps in which `ev iff gate` occurred
// rather than the ones in which `ev` did. The gate therefore narrows the
// condition that ring's stores select on and touches nothing else: the read
// side is unchanged, and an entry the gate holds back keeps the value it
// already had. Folded into the select rather than branched around, so the step
// stays straight-line like the rest of it.
void Lowerer::emit_history_shift(const SvaHistory& hist, ir::Index edge, ir::Builder& b, ir::SourceLoc l) {
    for (const SvaHistory::Ring& ring : hist.rings) {
        const ir::Index tick =
          ring.gate != nullptr ? b.binary(ir::Inst::Tag::Band, edge, ring.gate_value, l) : edge;
        for (size_t k = ring.entries.size(); k-- > 1;) {
            store_when(ring.entries[k], load_signal(ring.entries[k - 1], b, l), tick, b, l);
        }
        store_when(ring.entries[0], load_signal(sva_shadow_of(ring.live), b, l), tick, b, l);
    }
}

// "Definitely 1" / "definitely 0" of a value, each as a definite bit. A
// 4-state -> 2-state resize is LRM 6.3.1's collapse (x/z drop to 0), so it
// answers "definitely 1"; SystemVerilog's `!` is true only when every bit is
// definitely 0, so the same collapse over Lnot answers "definitely 0".
ir::Index Lowerer::def_one(ir::Index v, ir::Builder& b, ir::SourceLoc l) {
    return b.convert(v, module_.pool.bit_type, false, l);
}

ir::Index Lowerer::def_zero(ir::Index v, ir::Builder& b, ir::SourceLoc l) {
    return b.convert(b.unary(ir::Inst::Tag::Lnot, v, l), module_.pool.bit_type, false, l);
}

// $rose / $fell (LRM 16.9.3): did the bit move UP (resp. DOWN) the 4-state
// edge order 0 < x/z < 1? Not a plain 0 -> 1 test -- x -> 1 IS a rise -- and
// this is the same lattice the backend applies to @(posedge) in
// emit_edge_facts (src/ir/llvm/Events.cpp), reused rather than reinvented so
// that an assertion edge and a procedural edge can never disagree.
//
// emit_edge_facts computes a rise as `!old_must && new_may && (!old_may ||
// new_must)` over the must/may planes. With must = "definitely 1" and !may =
// "definitely 0" that is exactly the form below, and the fall case is its
// mirror image with the two ends of the order swapped.
ir::Index Lowerer::edge_step(ir::Index old_v, ir::Index new_v, bool rising, ir::Builder& b, ir::SourceLoc l) {
    // `hi` is the end of the order being moved TOWARDS, `lo` the one moved
    // away from: a rise travels towards 1, a fall towards 0.
    const auto hi = [&](ir::Index v) { return rising ? def_one(v, b, l) : def_zero(v, b, l); };
    const auto lo = [&](ir::Index v) { return rising ? def_zero(v, b, l) : def_one(v, b, l); };

    // Moved iff the old value was not already at the far end, the new one is
    // not at the near end, and at least one of the two is definite -- that
    // last conjunct is what excludes x -> x, the only remaining pair whose
    // endpoints are indistinguishable.
    const ir::Index old_not_hi = bit_not(hi(old_v), b, l);
    const ir::Index new_not_lo = bit_not(lo(new_v), b, l);
    const ir::Index moved      = b.binary(ir::Inst::Tag::Band, old_not_hi, new_not_lo, l);
    const ir::Index old_lo     = lo(old_v);
    const ir::Index new_hi     = hi(new_v);
    const ir::Index definite   = b.binary(ir::Inst::Tag::Bor, old_lo, new_hi, l);
    return b.binary(ir::Inst::Tag::Band, moved, definite, l);
}

// One sampled value function (LRM 16.9.3). All six read SAMPLED values, so
// every one is built from the Preponed shadow and the per-tick history ring
// and none of them touches live state -- which is also what lets $sampled
// stand in a root that is evaluated live, as the refusal below explains.
ir::Index Lowerer::lower_sampled_value(std::string_view      name,
                                       const CallExpression& call,
                                       ir::Builder&          b,
                                       ir::SourceLoc         l) {
    if (sva_history_ == nullptr) {
        // LRM 16.9.3 also allows these in some procedural contexts, but
        // nothing samples there: with no clocking event there is no tick for
        // history to advance on, and no shadow to compare against.
        // TODO: sampled value functions outside a concurrent assertion, which
        // need a clock from `default clocking` (LRM 14.12) and a sampling
        // process keyed to it.
        fail(std::string(name) + " is only supported inside a concurrent assertion expression");
    }

    const auto args = call.arguments();

    // slang has already checked the argument counts and that a clocking
    // argument sits in the one position each function allows
    // (NonConstFuncs.cpp), so what is left here is refusing the shapes clotho
    // cannot honour yet.
    const bool   is_past   = name == "$past";
    const size_t clock_arg = is_past ? 3 : 1;
    if (args.size() > clock_arg && args[clock_arg]->kind == ExpressionKind::ClockingEvent) {
        // An explicit clocking argument gives the function its own tick
        // stream, independent of the assertion's: a second ring shifted by a
        // second edge, in a process that does not exist.
        // TODO: an explicit clocking argument on a sampled value function.
        fail("an explicit clocking argument on " + std::string(name) + " is not supported");
    }
    if (!sva_sampled_ && name != "$sampled") {
        // An unsampled root -- the `disable iff` condition (LRM 16.15) or an
        // asynchronous abort's (16.12.7) -- carries no clock into the functions
        // written inside it, and LRM 16.9.3's inference rules have no fallback
        // for that position: "if called in a disable condition or a clock
        // expression in an assertion, sequence, or property, it shall be
        // explicitly clocked". 16.12.14 says the same of an abort condition in
        // as many words: "when sampled value functions other than $sampled are
        // used in the abort condition, the clock argument shall be explicitly
        // specified". So the only legal spelling of the other five here is the
        // explicitly clocked one, which is the gap refused just above; an
        // unclocked one is illegal source. slang diagnoses that for a disable
        // condition itself (analysis/ClockInference.cpp, checkSampledValueFuncs)
        // and lets it through for an abort condition, so this is where the abort
        // half stops -- no TODO, because there is nothing legal left to support.
        //
        // $sampled uses no clocking event at all (16.9.3) and is expressly
        // blessed here: "the use of $sampled in a disable iff clause is
        // meaningful since the disable condition by default is not sampled".
        // It reads the shadow and nothing else, so it falls through unchanged.
        fail(std::string(name) + " must be given an explicit clocking argument in a `disable iff` or "
                                 "asynchronous `accept_on` / `reject_on` condition");
    }

    uint32_t          ticks = 1;
    const Expression* gate  = nullptr;
    if (is_past) {
        if (args.size() > 2 && args[2]->kind != ExpressionKind::EmptyArgument) {
            // LRM 16.9.3 reads $past(v, n, g) at "the kth strictly prior time
            // step in which the event ev iff g occurred", so the gate does not
            // pick a value out of the history -- it decides which ticks the
            // history counts at all. Carried down to the ring rather than
            // applied here, because it is the SHIFT that has to skip a tick.
            gate = args[2];
        }
        if (args.size() > 1 && args[1]->kind != ExpressionKind::EmptyArgument) {
            const std::optional<int32_t> n = try_const_int(*args[1], Fold::Required);
            if (!n.has_value() || *n < 1) {
                // The ring is sized at compile time, so the tick count has to
                // be one too. slang diagnoses a constant below 1 on its own;
                // the non-constant case it leaves alone, and this catches both.
                fail("the number of ticks on $past must be a positive constant");
            }
            ticks = static_cast<uint32_t>(*n);
        }
    }

    // The operand has to be a plain variable -- a NamedValueExpression for a
    // local name, or a HierarchicalValueExpression for a dotted path (LRM
    // 23.6, `dif.sig`, now reachable here because sampled_shadow_of no
    // longer requires the asserting scope to declare it); slang binds both
    // to the target ValueSymbol the same way, through
    // ValueExpressionBase::symbol. Sampling is keyed by variable -- LRM
    // 16.6 captures operands, not expressions -- so the shadow and the ring
    // are both per-signal, and an expression has nothing to hang history on.
    // The width guard is what makes the unwrap safe: only a width-preserving
    // wrapper leaves the shadow's value interchangeable with the call's.
    const Expression& operand = unwrap_conversions(*args[0]);
    const bool        plain =
      (operand.kind == ExpressionKind::NamedValue || operand.kind == ExpressionKind::HierarchicalValue) &&
      operand.type->getCanonicalType().getBitWidth() == args[0]->type->getCanonicalType().getBitWidth() &&
      operand.as<ValueExpressionBase>().symbol.kind != SymbolKind::Parameter &&
      sigmap_.contains(&operand.as<ValueExpressionBase>().symbol);
    if (!plain) {
        // TODO: a sampled value function over an expression, which needs a
        // history entry per EXPRESSION, evaluated at the tick it belongs to
        // rather than resolved back to a variable.
        fail(std::string(name) + " is only supported on a plain variable reference");
    }
    const ir::Signal live   = signal_of(operand.as<ValueExpressionBase>().symbol);
    const ir::Signal shadow = sva_shadow_of(live);

    // $sampled is the shadow itself: the value this variable had when the
    // current tick was sampled. No history, so deliberately no ring entry --
    // asking for one would deepen a ring that nothing shifts on its behalf.
    if (name == "$sampled") {
        return load_signal(shadow, b, l);
    }
    if (is_past) {
        return load_signal(sva_history_slot(live, gate, ticks, b), b, l);
    }

    // Every value change function compares this tick's sample against the
    // previous tick's, so they all need entry 0 of the ungated ring and
    // nothing deeper: LRM 16.9.3 gives a gating argument to $past alone.
    const ir::Index now  = load_signal(shadow, b, l);
    const ir::Index then = load_signal(sva_history_slot(live, nullptr, 1, b), b, l);

    if (name == "$stable" || name == "$changed") {
        // Case equality over the WHOLE value: LRM 16.9.3 asks whether the
        // sampled value changed, and an x bit that becomes a 1 is a change like
        // any other. `===` is the comparison that sees all four states and
        // still yields a definite bit.
        const ir::Index same = b.binary(ir::Inst::Tag::CaseEq, now, then, l);
        return name == "$stable" ? same : bit_not(same, b, l);
    }

    // $rose / $fell are LSB-only (LRM 16.9.3), so a multi-bit operand is
    // truncated to bit 0 -- as 4-state, because the edge order below needs to
    // tell x apart from 0 and 1.
    const ir::Index old_bit = b.convert(then, module_.pool.logic_type, false, l);
    const ir::Index new_bit = b.convert(now, module_.pool.logic_type, false, l);
    return edge_step(old_bit, new_bit, name == "$rose", b, l);
}

// Two families are exceptions, and neither is a grudging one, because
// neither has a body to trace: both bottom out in operand lowering that
// already reaches value_lvalue, which already redirects every variable to
// its Preponed shadow whenever sva_sampled_ is set -- set for the whole of
// this atom by eval_atom's SampledScope, so it is still set when either
// family's own operand gets lowered below.
//
// The sampled value functions are the first: they are DEFINED over sampled
// values, and lower_sampled_value builds each from the shadow and the
// history ring without reading live state at all. Their own operand must
// already be a plain variable reference -- rejected there when it is not --
// so declining to descend into one loses no call this would otherwise have
// caught.
//
// LRM 20.9's bit vector system functions are the second
// (is_pure_bitvec_sysfn: `$isunknown`, `$countones`, `$countbits`,
// `$onehot`, `$onehot0`): pure combinational reductions over one
// bit-vector operand and nothing else, lowered by lower_bit_query as
// nothing more than lower_expr on that operand plus the reduction op --
// the same lower_expr every plain (non-call) assertion atom already goes
// through to reach value_lvalue. Unlike the family above, LRM 20.9 leaves
// the operand an arbitrary expression rather than requiring a plain
// variable, so a call nested inside it (`$isunknown(f())`) is exactly as
// unsampleable as a bare one -- which is why this case descends into the
// call below instead of returning, the one difference from the family
// above.
//
// TODO: trace called bodies so their reads are sampled too, which is also
// what LRM 16.6's side-effect-freedom rule needs checking against
// (concurrent-assertions.md 4) -- still the gap for every remaining system
// function (e.g. `$time`, which names no operand to sample in the first
// place) and for a user function/task call, either of which may read state
// its own arguments never name.
//
// A sequence method reaches here too, and is called out by name: `.triggered`
// is a call syntactically, but "what it reads would not be sampled" describes
// nothing about it (concurrent-assertions.md 9.3).
void Lowerer::reject_unsampled_call(const Expression& expr) {
    struct Finder : ASTVisitor<Finder, VisitFlags::AllGood> {
        bool                  found  = false;
        const CallExpression* method = nullptr;

        void handle(const CallExpression& call) {
            if (call.isSystemCall() && is_sampled_value_fn(call.getSubroutineName())) {
                return;
            }
            if (call.isSystemCall() && is_pure_bitvec_sysfn(call.getSubroutineName())) {
                // Keep walking (unlike the case above) so a call nested in
                // THIS one's own operand -- not constrained to a plain
                // variable the way a sampled value function's is -- still
                // gets caught by a later visit of this same handler.
                visitDefault(call);
                return;
            }
            if (method == nullptr && !sequence_method(call).empty()) {
                method = &call;
            }
            found = true;
        }
    };
    Finder finder;
    expr.visit(finder);
    if (finder.method != nullptr) {
        // TODO: sequence methods (concurrent-assertions.md 9.3). Each needs
        // the named sequence stepped as an automaton of its own, whose match
        // this cycle becomes a root of the property reading it.
        fail("`." + std::string(sequence_method(*finder.method)) + "` on a named sequence is not supported");
    }
    if (finder.found) {
        fail("a function call in a concurrent assertion expression is not "
             "supported: what it reads would not be sampled");
    }
}

// One atom as a definite 1-bit value. LRM 16.6 evaluates an SVA boolean at
// full width and then interprets it exactly as an `if` condition, so this is
// lower_predicate plus the unknown-is-false collapse.
ir::Index Lowerer::eval_atom(const sva::Atom& atom, bool sampled, ir::Builder& b) {
    const ir::SourceLoc l = loc(*atom.expr);
    if (sampled) {
        reject_unsampled_call(*atom.expr);
        if (atom.match != nullptr) {
            reject_unsampled_call(*atom.match);
        }
    }

    ir::Index value{};
    {
        const SampledScope scope(*this, sampled);
        value = atom.match != nullptr ? eval_case_match(atom, b, l) : lower_predicate(*atom.expr, b);
    }
    if (atom.negated) {
        // Negation normal form puts `!` on the leaf, so the atom is the
        // negation of its own expression. Lnot of the reduced bit is exactly
        // SystemVerilog's `!`: true only when every bit is definitely 0, x
        // when the operand is unknown and no bit is definitely 1.
        value = b.unary(ir::Inst::Tag::Lnot, value, l);
    }

    // LRM 16.6: x and z are false. A 4-state -> 2-state resize is that
    // collapse (LRM 6.3.1 drops x/z to 0), and it also makes the bit definite,
    // which every select below needs -- an x condition would bitwise-merge its
    // arms rather than choose one.
    return b.convert(value, module_.pool.bit_type, false, l);
}

// `expr === match` for a `case` property item guard (LRM 16.12.17), which is
// the one atom shape carrying two expressions instead of one.
//
// CaseEq and not Eq because 16.12.17 defers to the 12.5 matching rules, which
// compare bit for bit and are x/z-sensitive -- the same comparison clotho's
// own `case` statement lowering picks for CaseStatementCondition::Normal.
//
// The widening is clotho's to do: slang binds the case expression and each
// item label as separate self-determined expressions and inserts no conversion
// between them (CaseAssertionExpr::fromSyntax), so a label narrower than the
// selector would otherwise compare short. The rule is common_case_type's --
// widest operand, 4-state if either is, signed only if both are.
ir::Index Lowerer::eval_case_match(const sva::Atom& atom, ir::Builder& b, ir::SourceLoc l) {
    const slang::ast::Type& sel   = *atom.expr->type;
    const slang::ast::Type& label = *atom.match->type;
    if (sel.isFloating() || label.isFloating()) {
        // As the case STATEMENT (lower_case_real): 12.5's bit-for-bit `===`
        // is defined only for integral operands, and a real has no x/z for
        // the 4-state rule to be about, so the match is IEEE-754 equality.
        // Both sides widen to `real` -- a shortreal widens exactly (6.12.2).
        const ir::Ref   rty = module_.pool.real_type;
        const ir::Index lhs = coerce(lower_expr(*atom.expr, b), sel, rty, b, l);
        const ir::Index rhs = coerce(lower_expr(*atom.match, b), label, rty, b, l);
        return b.binary(ir::Inst::Tag::FEq, lhs, rhs, l);
    }
    if (!sel.isIntegral() || !label.isIntegral()) {
        fail("a case property over a non-integral selector or item label is not supported");
    }
    const ir::Ref ty =
      make_int_type(std::max(sel.getBitWidth(), label.getBitWidth()), sel.isFourState() || label.isFourState());
    const ir::Index lhs = b.convert(lower_expr(*atom.expr, b), ty, sel.isSigned() && label.isSigned(), l);
    const ir::Index rhs = b.convert(lower_expr(*atom.match, b), ty, sel.isSigned() && label.isSigned(), l);
    return b.binary(ir::Inst::Tag::CaseEq, lhs, rhs, l);
}

// One boolean root as a definite 1-bit value, from the flat forest libsva
// handed back. Operands always point backwards, so a plain recursive walk
// terminates; a shared subterm costs a second evaluation, never a wrong one.
//
// Every root is DEFINITE: the four-to-two collapse happens at the atom
// (eval_atom), so And/Or here combine bits that are already 0 or 1 and a root's
// value is the two-valued answer to "did this boolean hold this cycle".
ir::Index Lowerer::eval_root(const sva::Result& lowered, uint32_t node, bool sampled, ir::Builder& b, ir::SourceLoc l) {
    using Op                          = sva::Result::RootNode::Op;
    const sva::Result::RootNode& item = lowered.nodes[node];
    switch (item.op) {
        case Op::Const:
            return item.lhs != 0 ? true_const(b, l) : zero_mask(1, b, l);
        case Op::Atom:
            return eval_atom(lowered.atoms[item.lhs], sampled, b);
        case Op::And: {
            const ir::Index lhs = eval_root(lowered, item.lhs, sampled, b, l);
            const ir::Index rhs = eval_root(lowered, item.rhs, sampled, b, l);
            return b.binary(ir::Inst::Tag::Band, lhs, rhs, l);
        }
        case Op::Or: {
            const ir::Index lhs = eval_root(lowered, item.lhs, sampled, b, l);
            const ir::Index rhs = eval_root(lowered, item.rhs, sampled, b, l);
            return b.binary(ir::Inst::Tag::Bor, lhs, rhs, l);
        }
        case Op::Not:
            // libsva's bnot, which clotho's own lowering never produces -- an SVA
            // boolean is three-valued, so the user's `!` is pushed to the leaves
            // and becomes an atom over the negated expression (Sva.cpp's
            // boolean(), concurrent-assertions.md 5.3). What a bnot CAN mean is
            // libsva's own structural negation, and that is the two-valued
            // complement of the definite bit above -- the same reading a
            // complemented guard literal has, for the same reason. See
            // automaton_satisfy.
            return bit_not(eval_root(lowered, item.lhs, sampled, b, l), b, l);
    }
    fail("internal: unknown assertion boolean node");
}

// One monitor: the hidden state it owns, plus the process that steps it once
// per clock edge. The order below is emitted_step's
// (../libsva/test/backend.c:147-201), which is differential-tested against
// libsva's own reference. Two things are expressed differently because a code
// generator has no run-time loop: a reset is a select on every store rather
// than an early return, and the free-slot search is unrolled.
// Returns the two ports an `expect` statement needs (see ExpectPorts), and an
// empty pair for every other directive: the monitor's hidden state is allocated
// in here, while the statement that drives it is emitted by the caller.
Lowerer::ExpectPorts Lowerer::emit_monitor(const sva::Result&          lowered,
                                           const ir::AssertionMonitor& mon,
                                           uint32_t                    id,
                                           const AssertionActions&     act,
                                           const MonitorControl*       ctl) {
    // Does this directive say anything? `restrict` is the one that does not,
    // and that is the whole of its simulation semantics: LRM 16.12 defines it
    // as a constraint on the input space a FORMAL tool may explore and gives
    // it no verdict, which is why slang rejects an action block on one. So it
    // is lowered exactly like an assert -- the property is bound, its roots
    // are sampled, and a shape libsva cannot handle is still refused -- and
    // then reports nothing, pends nothing and tallies nothing. This is the one
    // place the subsystem accepts a construct and deliberately does nothing
    // with it; everywhere else that would be the silent drop PRINCIPLES.md
    // forbids.
    bool verdicts = false;
    switch (mon.kind) {
        case ir::AssertionKind::Assert:
        case ir::AssertionKind::Assume:
        // An `expect` reports and acts exactly as an assert does (LRM 16.17
        // defines its action blocks by reference to 16.14, so an `else`-less
        // one still gets the default failure report). What differs is the
        // seeding and the blocking wait, both below -- not the verdict.
        case ir::AssertionKind::Expect:
            verdicts = true;
            break;
        case ir::AssertionKind::Cover:
        case ir::AssertionKind::Restrict:
            break;
    }
    // `depth` sizes the ring pool, so an empty one is a lowering bug -- but
    // only under the ring. A counted monitor has no pool, and nothing libsva
    // reports for `depth` there means anything.
    if (mon.obligation.state_count == 0 || (mon.has_trigger && mon.trigger.state_count == 0) ||
        mon.reset_root >= lowered.root_index.size() ||
        (mon.tracking == ir::AssertionTracking::Ring && mon.depth == 0)) {
        fail("internal: a concurrent assertion lowered to an empty automaton");
    }
    if (mon.tracking == ir::AssertionTracking::Counted) {
        for (const ir::AssertionAbort& abort : mon.aborts) {
            if (!abort.resolves_attempt) {
                // A branch-scoped abort -- a reject under a disjunction -- kills
                // its own disjunct by subtracting a mask from the attempt's state
                // vector, and a counted attempt has no vector: its single state
                // stands for a whole SUBSET of the operand's states, and no state
                // stands for that subset minus the mask. libsva refuses the
                // pairing (SVA_ERROR_UNSUPPORTED_ABORT_SCOPE), so this is the
                // backstop rather than a gap of clotho's own.
                fail("internal: a branch-scoped abort over a counted obligation");
            }
        }
    }

    const MonitorState  st = alloc_monitor_state(mon, id, ctl);
    const ir::SourceLoc l  = mon.span.start;
    ir::Builder         b(module_);

    // The action blocks, as zero-formal tasks the verdicts below pend into the
    // Reactive region. A cover has only a match action (LRM 16.12.6, and slang
    // rejects `cover ... else`); an assert / assume always has a fail action,
    // because with no `else` the default report is the implicit one. A restrict
    // has neither: no verdict, so nothing to hang a body on.
    std::optional<ir::FunctionId> pass_body;
    std::optional<ir::FunctionId> fail_body;
    if (act.pass != nullptr) {
        pass_body = build_action_body(*act.pass, "sva.pass");
    }
    if (verdicts) {
        fail_body = act.fail != nullptr ? build_action_body(*act.fail, "sva.fail") : build_default_fail_body(mon);
    }

    // The $past rings this monitor needs, filled in on demand while the roots
    // below are evaluated: a sampled value function reaches them through
    // sva_history_, and nothing else creates one.
    SvaHistory hist{ .base = "sva." + std::to_string(id) + ".", .loc = l };

    // Roots, one definite bit each. Every one reads the Preponed shadows of
    // the variables it names except the ones root_reads_live names: the
    // disable-iff condition, which LRM 16.15 deliberately does not sample
    // (concurrent-assertions.md 5.4), and an asynchronous abort's condition,
    // which is the same rule scoped to one property.
    std::vector<ir::Index> roots;
    roots.reserve(lowered.root_index.size());
    {
        const HistoryScope history(*this, &hist);
        for (uint32_t r = 0; r < lowered.root_index.size(); ++r) {
            roots.push_back(eval_root(lowered, lowered.root_index[r], !root_reads_live(mon, r), b, l));
        }
    }
    // The disable condition (LRM 16.15), and the clocking edge. The monitor
    // runs in the Observed region, which resumes every process it holds once
    // per time slot rather than subscribing them to net events (LlvmBackend's
    // emit_observed_run), so the edge is detected here instead of by a
    // sensitivity list: this slot's clock against the value it held at the
    // previous slot's Observed point. Taking it at the settled point is what
    // makes the monitor step once per cycle however many deltas the clock's
    // own driver needed to settle.
    const ir::Index clk_now = b.convert(load_signal(mon.clock, b, l), module_.pool.bit_type, false, l);
    const ir::Index clk_was = load_signal(st.clkprev, b, l);
    const ir::Index edge    = mon.edge == ir::Edge::Positive
                                ? b.binary(ir::Inst::Tag::Band, clk_now, bit_not(clk_was, b, l), l)
                                : b.binary(ir::Inst::Tag::Band, clk_was, bit_not(clk_now, b, l), l);
    // Ungated by the disable condition: clock history has to keep advancing
    // while the assertion is disabled, or re-enabling would see a stale edge.
    b.binary(ir::Inst::Tag::Store, b.signal(st.clkprev, l), clk_now, l);

    // $assertkill (LRM 20.12) abandons every attempt in flight, which is
    // precisely what LRM 16.15's reset already does -- so the request joins
    // the disable condition rather than clearing anything of its own, and
    // the two can never drift apart. It arrives as a latch because the task
    // runs in an arbitrary time slot while this is the only code that acts
    // on one: held until an edge, consumed by that edge, cleared by the same
    // one, so a request kills exactly one cycle. The clear is deliberately
    // ungated by anything but the edge, for the reason the abort latches
    // below are: a request must not survive the cycle it was served in.
    ir::Index disabled = roots[mon.reset_root];
    if (st.ctl != nullptr) {
        const ir::Index killed = load_signal(st.ctl->kill, b, l);
        disabled               = b.binary(ir::Inst::Tag::Bor, disabled, killed, l);
        const ir::Index dst    = b.signal(st.ctl->kill, l);
        const ir::Index val    = b.select(edge, zero_mask(1, b, l), killed, l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
    }
    const ir::Index active = b.binary(ir::Inst::Tag::Band, bit_not(disabled, b, l), edge, l);

    // Each abort's condition for this cycle. A `sync_` one is just its root:
    // the Preponed sample, read at the edge like every other guard. An
    // asynchronous one is read live and LRM 16.12.7 examines it at every
    // simulation time step, so it also counts if it was seen at any slot
    // since the last edge -- see MonitorState::abort_seen. The latch defers
    // an asynchronous report to the next tick: the edge is where this monitor
    // produces its verdicts, and the LRM does not fix which time step an
    // asynchronous abort reports in.
    std::vector<ir::Index> abort_cond(mon.aborts.size());
    for (uint32_t i = 0; i < mon.aborts.size(); ++i) {
        const ir::Index now = roots[mon.aborts[i].root];
        if (mon.aborts[i].sampled) {
            abort_cond[i] = now;
            continue;
        }
        abort_cond[i] = b.binary(ir::Inst::Tag::Bor, load_signal(st.abort_seen[i], b, l), now, l);
        // Consumed and cleared at every edge, enabled or not -- the window
        // belongs to the cycle, and a cycle the assertion is disabled for
        // discards it along with everything else. So the condition is tested
        // over (previous edge, this edge], with no gap and no double count.
        const ir::Index dst = b.signal(st.abort_seen[i], l);
        const ir::Index val = b.select(edge, zero_mask(1, b, l), abort_cond[i], l);
        b.binary(ir::Inst::Tag::Store, dst, val, l);
    }

    const uint32_t  obl   = mon.obligation.state_count;
    const ir::Index sat_o = automaton_satisfy(mon, mon.obligation, roots, b, l);

    // The counted discipline. Everything above this is shared -- the roots, the
    // clocking edge, the disable gate, the satisfied mask -- and everything
    // below it is the ring, which a counted monitor has none of.
    if (mon.tracking == ir::AssertionTracking::Counted) {
        emit_counted_step(mon,
                          st,
                          id,
                          act,
                          verdicts,
                          pass_body,
                          fail_body,
                          roots,
                          abort_cond,
                          sat_o,
                          active,
                          edge,
                          hist,
                          b,
                          l);
        return { st.expect_req, st.expect_busy };
    }

    // Per-attempt grouping: an attempt gets one verdict
    // (docs/LRM_NOTES.md, note 23). Width 0 switches it off and every `group`
    // branch below folds away, leaving the per-obligation path.
    const uint32_t ring  = attempt_ring_size(mon);
    const bool     group = ring != 0;

    // Whether any abort's scope reaches the ANTECEDENT rather than only the
    // obligation. Compile-time, and what decides whether the attempts still
    // matching the antecedent have to be gathered at all.
    bool covers_trigger = false;
    for (const ir::AssertionAbort& abort : mon.aborts) {
        covers_trigger = covers_trigger || abort.covers_trigger;
    }

    // This cycle's attempt id (next_attempt_id).
    ir::Index id_now = ir::kNoIndex;
    if (group) {
        id_now = next_attempt_id(st, ring, active, edge, b, l);
    }

    // The trigger. An attempt starts every cycle, so its first-state mask is
    // folded in unconditionally; `fires` is this cycle's antecedent match.
    //
    // `trig_next` and `tid_next` outlive the block: a failing obligation
    // resolves its whole attempt, which means killing that attempt's remaining
    // antecedent threads too, and that cannot be known until the verdicts.
    ir::Index              fires = zero_mask(1, b, l);
    ir::Index              trig_next     = ir::kNoIndex;
    ir::Index              fired_ids     = ir::kNoIndex;
    ir::Index              in_flight_ids = ir::kNoIndex;
    std::vector<ir::Index> tid_next;
    if (mon.has_trigger) {
        const uint32_t  trig  = mon.trigger.state_count;
        const ir::Index sat_t = automaton_satisfy(mon, mon.trigger, roots, b, l);
        const ir::Index cur_t = load_signal(st.trigger, b, l);
        ir::Index start =
          b.binary(ir::Inst::Tag::Band, b.constant(mask_const(mon.trigger.first, trig), l), sat_t, l);
        if (st.ctl != nullptr) {
            // $assertoff stops attempts STARTING and lets the ones already
            // executing finish (LRM 20.12). For an implication an attempt
            // starts when its ANTECEDENT does, so the gate belongs on the
            // first states injected here and nowhere downstream: `stepped`
            // carries threads already part-way through a match, and the seed
            // below is one of those threads reaching its obligation.
            const ir::Index off   = load_signal(st.ctl->off, b, l);
            const ir::Index zeros = zero_mask(trig, b, l);
            start                 = b.select(off, zeros, start, l);
        }
        const ir::Index stepped = automaton_advance(mon.trigger, cur_t, sat_t, b, l);
        const ir::Index next    = b.binary(ir::Inst::Tag::Bor, stepped, start, l);
        const ir::Index done =
          b.binary(ir::Inst::Tag::Band, next, b.constant(mask_const(mon.trigger.last, trig), l), l);
        fires     = mask_any(done, b, l);
        trig_next = next;

        if (group) {
            tid_next = advance_attempt_ids(mon, st, ring, cur_t, next, id_now, fired_ids, b, l);
            if (covers_trigger) {
                // Every attempt still MATCHING the antecedent, part-matched and
                // just-matched alike. This is the one place the union runs over ALL
                // trigger states rather than over the exits, and the abort below is
                // why (../libsva/test/backend.c:381-391).
                in_flight_ids = zero_mask(ring, b, l);
                for (const ir::Index in_state : tid_next) {
                    in_flight_ids = b.binary(ir::Inst::Tag::Bor, in_flight_ids, in_state, l);
                }
            }
        }
    }

    // An abort whose scope reaches the antecedent resolves at MATCH time --
    // the cycle the antecedent match ENDS, which for `|=>` is the cycle
    // BEFORE the obligation would be seeded. Reading the condition at seed
    // time instead would look at it one cycle late and lose the report
    // (diff_abort.sv's t=75 is exactly that cycle). So the match is consumed
    // here: it takes the abort's verdict and `fires` is forced low, which
    // suppresses the seed for both forms at once -- `|->` reads `fires`
    // directly and `|=>` reads it through `st.fired`.
    //
    // With a MULTI-CYCLE antecedent the match ending now is not the whole of what
    // the abort covers. LRM 16.12.7 resolves the attempt wherever it has got to, so
    // every attempt with a live antecedent thread is resolved -- one verdict each,
    // and part-matched ones count. That is what `in_flight_ids` gathers, and it is
    // why the scope below is a union over all trigger states instead of `fires`.
    // Both are the same expression when the antecedent matches in one cycle, where
    // the only attempt in flight IS the one that just matched.
    //
    // The attempt that has ALREADY been seeded is not this case: it is an
    // ordinary pending attempt, armed through the mask below because the
    // scope overlaps the obligation's first states. That is what discharges
    // `aa` at t=45. Grouped, an attempt that is BOTH -- a pending obligation and a
    // thread still matching, which only a multi-cycle antecedent can manage -- lands
    // in the id unions twice and is reported once, because that is what a set does.
    //
    // The resolved attempts' threads are killed, and it matters: an antecedent
    // completing later would raise an obligation on an attempt already resolved.
    // Grouped, dropping their ids from `tid_next` does it, and the `kept` fold
    // below then prunes the trigger vector itself.
    std::optional<ir::Index> trigger_matched;
    std::optional<ir::Index> trigger_failed;
    std::optional<ir::Index> discharged_ids;
    std::optional<ir::Index> rejected_ids;
    if (mon.has_trigger) {
        // What the abort can reach: every attempt in flight when the ids can tell
        // them apart, and the match ending now when one bit is the whole answer.
        const ir::Index scope = group && covers_trigger ? mask_any(in_flight_ids, b, l) : fires;

        // Backwards: outermost last in the list, and an outer abort beats an
        // inner verdict landing in the same cycle (LRM 16.12.7).
        ir::Index claimed = zero_mask(1, b, l);
        for (uint32_t i = static_cast<uint32_t>(mon.aborts.size()); i-- > 0;) {
            if (!mon.aborts[i].covers_trigger) {
                continue;
            }
            const ir::Index           hit       = b.binary(ir::Inst::Tag::Band, scope, abort_cond[i], l);
            const ir::Index           unclaimed = bit_not(claimed, b, l);
            const ir::Index           fire      = b.binary(ir::Inst::Tag::Band, hit, unclaimed, l);
            std::optional<ir::Index>& into =
              mon.aborts[i].effect == ir::AssertionAbortEffect::Accept ? trigger_matched : trigger_failed;
            into    = into.has_value() ? b.binary(ir::Inst::Tag::Bor, *into, fire, l) : fire;
            claimed = b.binary(ir::Inst::Tag::Bor, claimed, abort_cond[i], l);
        }
        if (trigger_matched.has_value() || trigger_failed.has_value()) {
            fires = b.binary(ir::Inst::Tag::Band, fires, bit_not(claimed, b, l), l);

            // Which attempts went with it. Every trigger-covering abort has the
            // SAME scope -- the whole antecedent -- so which one won is already
            // settled in the bits above and the id set they select is common.
            if (group) {
                const ir::Index none_ids = zero_mask(ring, b, l);
                if (trigger_matched.has_value()) {
                    discharged_ids = b.select(*trigger_matched, in_flight_ids, none_ids, l);
                }
                if (trigger_failed.has_value()) {
                    rejected_ids = b.select(*trigger_failed, in_flight_ids, none_ids, l);
                }
            }
        }
    }

    // Which slots each abort is armed for, taken from the live sets BEFORE
    // they advance -- see abort_arming_row for why that is the right instant.
    std::vector<ir::Index> arming(mon.aborts.size());
    for (uint32_t i = 0; i < mon.aborts.size(); ++i) {
        arming[i] = b.constant(mask_const(abort_arming_row(mon.obligation, mon.aborts[i]), obl), l);
    }
    std::vector<std::vector<ir::Index>> armed(mon.aborts.size(), std::vector<ir::Index>(mon.depth));

    // Advance every occupied slot. An unoccupied slot already holds 0 (a match
    // zeroes it, and a failure only fires once it is empty), so the guard is
    // belt-and-braces -- but it is what keeps that an invariant rather than an
    // assumption, and emitted_step has it.
    const ir::Index        occupied = load_signal(st.occupied, b, l);
    std::vector<ir::Index> pending(mon.depth);
    for (uint32_t k = 0; k < mon.depth; ++k) {
        const ir::Index current = load_signal(st.pending[k], b, l);
        const ir::Index busy    = mask_bit(occupied, k, mon.depth, b, l);
        for (uint32_t i = 0; i < mon.aborts.size(); ++i) {
            armed[i][k] = b.binary(
              ir::Inst::Tag::Band, busy, mask_any(b.binary(ir::Inst::Tag::Band, current, arming[i], l), b, l), l);
        }
        const ir::Index stepped = automaton_advance(mon.obligation, current, sat_o, b, l);
        pending[k]              = b.select(busy, stepped, current, l);
    }

    // Seed. `|->` seeds on the trigger's own last cycle, `|=>` on the cycle
    // after (so it reads last cycle's `fired`), and a bare sequence starts an
    // attempt every cycle.
    ir::Index seeding = true_const(b, l);
    if (mon.has_trigger) {
        seeding = mon.overlap ? fires : load_signal(st.fired, b, l);
    } else {
        seeding = bare_seed_gate(mon, st, b, l);
    }
    const ir::Index seed =
      b.binary(ir::Inst::Tag::Band, b.constant(mask_const(mon.obligation.first, obl), l), sat_o, l);

    // Which attempt owns the obligation seeded this cycle. `|->` seeds from
    // this cycle's match and `|=>` from last cycle's, so the id rides the same
    // one-cycle carry `fired` does.
    //
    // A trigger-covering abort cannot interfere: libsva admits one only over
    // an antecedent whose every match is a single cycle, which is exactly the
    // shape attempt_ring_size leaves ungrouped.
    ir::Index seed_ids = ir::kNoIndex;
    if (group) {
        seed_ids = mon.overlap ? fired_ids : load_signal(st.firedid, b, l);
    }
    std::vector<ir::Index> oid_next(group ? mon.depth : 0);
    for (uint32_t k = 0; group && k < mon.depth; ++k) {
        oid_next[k] = load_signal(st.oid[k], b, l);
    }

    // The first free slot, unrolled: depth is a compile-time constant, so there
    // is no run-time search to make. A slot is always free -- depth is the
    // obligation's longest match, so an attempt holds its slot for at most
    // depth cycles and the verdicts below free it in the last of them, leaving
    // at most depth - 1 alive when this runs.
    //
    // A freshly seeded attempt is armed for any abort whose scope overlaps the
    // states it starts in, so an abort firing in the cycle an attempt starts
    // applies to that attempt.
    std::vector<bool> arms_on_seed(mon.aborts.size());
    for (uint32_t i = 0; i < mon.aborts.size(); ++i) {
        arms_on_seed[i] = rows_intersect(mon.obligation.first, mon.aborts[i].mask);
    }
    ir::Index live      = occupied;
    ir::Index all_taken = true_const(b, l);
    for (uint32_t k = 0; k < mon.depth; ++k) {
        const ir::Index busy      = mask_bit(occupied, k, mon.depth, b, l);
        const ir::Index free_here = b.binary(ir::Inst::Tag::Band, all_taken, bit_not(busy, b, l), l);
        const ir::Index pick      = b.binary(ir::Inst::Tag::Band, seeding, free_here, l);
        const ir::Index slot      = slot_mask(k, mon.depth, b, l);
        const ir::Index zeros     = zero_mask(mon.depth, b, l);
        const ir::Index claim     = b.select(pick, slot, zeros, l);

        pending[k] = b.select(pick, seed, pending[k], l);
        if (group) {
            oid_next[k] = b.select(pick, seed_ids, oid_next[k], l);
        }
        live      = b.binary(ir::Inst::Tag::Bor, live, claim, l);
        all_taken = b.binary(ir::Inst::Tag::Band, all_taken, busy, l);
        for (uint32_t i = 0; i < mon.aborts.size(); ++i) {
            if (arms_on_seed[i]) {
                armed[i][k] = b.binary(ir::Inst::Tag::Bor, armed[i][k], pick, l);
            }
        }
    }

    // Aborts resolve an attempt early. After seeding, so one firing in the
    // cycle an attempt starts applies to that attempt, and before the verdicts
    // below, because resolving early is the whole point
    // (../libsva/test/backend.c:229-247). Three outcomes, and they are not the
    // monitor-wide reset's: a reject FAILS the attempt now, an accept
    // DISCHARGES it -- a pass, which an `else`-less assert reports nothing for
    // but a pass action block and a cover's tally both see -- and an abort
    // that does not resolve the whole attempt just clears its own states and
    // leaves the rest running, which is a reject under a disjunction killing
    // its own disjunct and no more. A reset does none of the three: it
    // abandons the attempt and leaves no verdict at all.
    //
    // `held` is threaded rather than re-read so that a slot an abort has taken
    // is out of play for every later abort and for the verdicts, the way
    // clearing `occupied` does it in the reference. Backwards over the list
    // again: outermost wins.
    std::vector<ir::Index>                held(mon.depth);
    std::vector<std::optional<ir::Index>> aborted_match(mon.depth);
    std::vector<std::optional<ir::Index>> aborted_fail(mon.depth);
    for (uint32_t k = 0; k < mon.depth; ++k) {
        held[k] = mask_bit(live, k, mon.depth, b, l);
    }
    for (uint32_t i = static_cast<uint32_t>(mon.aborts.size()); i-- > 0;) {
        const ir::AssertionAbort& abort = mon.aborts[i];
        // The states this abort does NOT name, for the partial clear.
        std::vector<uint64_t> spared(mon.obligation.words, 0);
        for (uint32_t s = 0; s < obl; ++s) {
            if ((abort.mask[s / 64] & (uint64_t{ 1 } << (s % 64))) == 0) {
                spared[s / 64] |= uint64_t{ 1 } << (s % 64);
            }
        }
        const ir::Index keep_states = b.constant(mask_const(spared, obl), l);

        for (uint32_t k = 0; k < mon.depth; ++k) {
            const ir::Index fire = b.binary(
              ir::Inst::Tag::Band, b.binary(ir::Inst::Tag::Band, abort_cond[i], armed[i][k], l), held[k], l);
            if (!abort.resolves_attempt) {
                pending[k] =
                  b.select(fire, b.binary(ir::Inst::Tag::Band, pending[k], keep_states, l), pending[k], l);
                continue;
            }
            std::optional<ir::Index>& into =
              abort.effect == ir::AssertionAbortEffect::Accept ? aborted_match[k] : aborted_fail[k];
            into       = into.has_value() ? b.binary(ir::Inst::Tag::Bor, *into, fire, l) : fire;
            pending[k] = b.select(fire, zero_mask(obl, b, l), pending[k], l);
            held[k]    = b.binary(ir::Inst::Tag::Band, held[k], bit_not(fire, b, l), l);
        }
    }

    // Verdicts. Seeding before taking them is not incidental: under `|->` a
    // one-cycle consequent has to be able to discharge in the very cycle it was
    // created (../libsva/test/backend.c:187-188).
    const ir::Index        last = b.constant(mask_const(mon.obligation.last, obl), l);
    std::vector<ir::Index> matched(mon.depth);
    std::vector<ir::Index> failed(mon.depth);
    ir::Index              remaining = zero_mask(mon.depth, b, l);
    for (uint32_t k = 0; k < mon.depth; ++k) {
        // The lattice, once, for this slot. `matched` and `failed` stay the
        // names the rest of the step and the reports use; they are its Holds and
        // Fails members, and Pending is what keeps the slot allocated.
        const AttemptVerdict v = ring_verdict(held[k], pending[k], last, b, l);

        matched[k] = v.holds;
        failed[k]  = v.fails;

        // Only a match has a vector left to clear, since a failure is what an
        // empty vector means.
        const ir::Index slot  = slot_mask(k, mon.depth, b, l);
        const ir::Index zeros = zero_mask(mon.depth, b, l);
        const ir::Index still = b.select(v.pending, slot, zeros, l);

        pending[k] = b.select(matched[k], zero_mask(obl, b, l), pending[k], l);
        remaining  = b.binary(ir::Inst::Tag::Bor, remaining, still, l);

        // An abort's verdict is the attempt's verdict, so it joins the slot's
        // own -- after `remaining`, which an aborted slot is already out of.
        if (aborted_match[k].has_value()) {
            matched[k] = b.binary(ir::Inst::Tag::Bor, matched[k], *aborted_match[k], l);
        }
        if (aborted_fail[k].has_value()) {
            failed[k] = b.binary(ir::Inst::Tag::Bor, failed[k], *aborted_fail[k], l);
        }
    }
    // A trigger-covering abort's verdict belongs to an attempt that never reached a
    // slot, so ungrouped it rides alongside the per-slot ones. Grouped it goes in as
    // an id SET instead (`discharged_ids` and `rejected_ids` below), because the
    // abort can have resolved several attempts at once and they each owe a verdict.
    if (!group) {
        if (trigger_matched.has_value()) {
            matched.push_back(*trigger_matched);
        }
        if (trigger_failed.has_value()) {
            failed.push_back(*trigger_failed);
        }
    }

    // `cover sequence` counts MATCHES, and the grouping below is about to fold
    // those into one verdict per attempt and drop the ones belonging to an
    // attempt that later failed. Its tally and its match action read this copy,
    // taken before any of that; `cover property` keeps reading `report_pass`
    // and is unchanged. Empty, and no copy made, for every other directive.
    //
    // A match is a (start, end) pair, so grouped this is one hit per (slot,
    // attempt) pair rather than one per slot: a slot may stand for several
    // attempts, whose antecedent matches ended in the same cycle, and those are
    // that many distinct matches -- same end, different starts. Deliberately NOT
    // the union over slots that the verdicts below take, because an attempt whose
    // antecedent matched twice really did match twice, and counting both is the
    // whole difference between this reading and `cover property`'s.
    std::vector<ir::Index> seq_matched;
    if (mon.cover_matches) {
        if (!group) {
            seq_matched = matched;
        } else {
            seq_matched.reserve(static_cast<size_t>(mon.depth) * ring);
            for (uint32_t k = 0; k < mon.depth; ++k) {
                for (uint32_t i = 0; i < ring; ++i) {
                    seq_matched.push_back(b.binary(
                      ir::Inst::Tag::Band, matched[k], mask_bit(oid_next[k], i, ring, b, l), l));
                }
            }
        }
    }

    // Per-attempt resolution (docs/LRM_NOTES.md, note 23). An attempt is ONE verdict over
    // every obligation its antecedent matches seeded: it fails at the first of
    // them to fail, and passes only once all of them have passed and it can
    // match no more. Ungrouped, each obligation is an attempt of its own and
    // these stay exactly `failed` and `matched`.
    std::vector<ir::Index> report_fail = failed;
    std::vector<ir::Index> report_pass = matched;
    if (group) {
        const uint32_t  trig = mon.trigger.state_count;
        const ir::Index none = zero_mask(ring, b, l);

        // The attempts a failing obligation resolves this cycle, plus the ones a
        // REJECTING abort over the antecedent resolved -- a reject is a failure, and
        // an attempt it took is as finished as one whose obligation failed.
        ir::Index failed_ids = none;
        for (uint32_t k = 0; k < mon.depth; ++k) {
            failed_ids = b.binary(ir::Inst::Tag::Bor, failed_ids, b.select(failed[k], oid_next[k], none, l), l);
        }
        if (rejected_ids.has_value()) {
            failed_ids = b.binary(ir::Inst::Tag::Bor, failed_ids, *rejected_ids, l);
        }
        const ir::Index survives = b.unary(ir::Inst::Tag::Bnot, failed_ids, l);

        // FINISHED is wider than survives, and the two must not be confused. An
        // ACCEPTING abort over the antecedent discharges its attempts: that is a
        // pass, so it must not suppress one the way a failure does -- but those
        // attempts are just as done, and their threads and slot shares have to go or
        // a later antecedent match would raise an obligation on an attempt already
        // resolved. So `finished` clears state and `survives` decides passes, and
        // they coincide for every monitor without an abort over its antecedent.
        const ir::Index finished =
          discharged_ids.has_value()
            ? b.unary(ir::Inst::Tag::Bnot, b.binary(ir::Inst::Tag::Bor, failed_ids, *discharged_ids, l), l)
            : survives;

        // A resolved attempt is finished. Its other obligations are abandoned --
        // a later one passing is not a pass of an attempt that already failed -- and
        // so are the antecedent threads that would have seeded more.
        //
        // Abandoning is per OWNER and not per slot, because a slot can have
        // several: attempts whose antecedent matches ended in the same cycle
        // raised the same obligation and share it. Dropping the failed attempts
        // out of the owner set is the whole of it, and the slot itself only goes
        // once the LAST owner has -- until then the obligation it holds is still
        // a surviving attempt's, and still owes that attempt a verdict. Freeing
        // the slot on the first owner to fail loses every co-owner silently.
        for (uint32_t k = 0; k < mon.depth; ++k) {
            const ir::Index survivors    = b.binary(ir::Inst::Tag::Band, oid_next[k], finished, l);
            const ir::Index owned        = mask_any(oid_next[k], b, l);
            const ir::Index no_survivors = bit_not(mask_any(survivors, b, l), b, l);
            const ir::Index doomed       = b.binary(ir::Inst::Tag::Band, owned, no_survivors, l);
            oid_next[k]                  = survivors;
            matched[k]                   = b.binary(ir::Inst::Tag::Band, matched[k], bit_not(doomed, b, l), l);
            pending[k]                   = b.select(doomed, zero_mask(obl, b, l), pending[k], l);
            remaining                    = b.select(
              doomed,
              b.binary(
                ir::Inst::Tag::Band, remaining, b.unary(ir::Inst::Tag::Bnot, slot_mask(k, mon.depth, b, l), l), l),
              remaining,
              l);
        }
        fired_ids      = b.binary(ir::Inst::Tag::Band, fired_ids, finished, l);
        ir::Index kept = zero_mask(trig, b, l);
        for (uint32_t s = 0; s < trig; ++s) {
            tid_next[s]             = b.binary(ir::Inst::Tag::Band, tid_next[s], finished, l);
            const ir::Index present = mask_any(tid_next[s], b, l);
            const ir::Index slot    = slot_mask(s, trig, b, l);
            const ir::Index zeros   = zero_mask(trig, b, l);
            kept                    = b.binary(ir::Inst::Tag::Bor, kept, b.select(present, slot, zeros, l), l);
        }
        trig_next = b.binary(ir::Inst::Tag::Band, trig_next, kept, l);

        // What can still give an attempt a verdict after this cycle: an
        // antecedent thread of it still running, an obligation of it still
        // pending, or a match of it that seeds one next cycle. A pass waits for
        // all three to be clear, which is what makes a twice-matching attempt
        // report at its LATER obligation instead of its first.
        ir::Index live_ids = none;
        for (const ir::Index in_state : tid_next) {
            live_ids = b.binary(ir::Inst::Tag::Bor, live_ids, in_state, l);
        }
        if (!mon.overlap) {
            live_ids = b.binary(ir::Inst::Tag::Bor, live_ids, fired_ids, l);
        }
        for (uint32_t k = 0; k < mon.depth; ++k) {
            live_ids = b.binary(ir::Inst::Tag::Bor,
                                live_ids,
                                b.select(mask_bit(remaining, k, mon.depth, b, l), oid_next[k], none, l),
                                l);
        }

        // One FAIL report per ATTEMPT that is owed one, which is not the same
        // as one per slot. Attempts whose antecedent matches end in the SAME
        // cycle share a single obligation -- the obligation they impose really
        // is one and the same -- but they do not share the verdict it produces,
        // and LRM 16.14.1 owes each attempt its own. Three attempts started
        // at 45, 55 and 65 whose shared obligation fails at 75 therefore
        // report `FAIL 75` each, not one failure for the slot they share.
        //
        // `failed_ids` is already exactly the set of attempts a failing
        // obligation resolves this cycle, so a report is one bit of it per id.
        // Being a SET it also collapses an attempt two of whose obligations
        // fail in this one cycle, which is what the per-slot walk needed a
        // claim chain to do by hand.
        report_fail.assign(ring, none);
        for (uint32_t i = 0; i < ring; ++i) {
            report_fail[i] = mask_bit(failed_ids, i, ring, b, l);
        }

        // A pass is the attempt's only once nothing of it is left running, so
        // it is carried in `passing` rather than reported where it matched, and
        // fires in the cycle the attempt goes quiet. That is what defers a
        // twice-matching attempt's pass to its LATER obligation -- and to a
        // cycle later still when the antecedent could have matched again and
        // did not.
        //
        // An attempt an ACCEPTING abort discharged joins them: a discharge is a pass
        // (an `else`-less assert reports nothing for it, a pass action and a cover's
        // tally both see it), and since `finished` has already emptied its threads
        // and its slot shares, nothing of it is left running and it settles in this
        // very cycle rather than waiting.
        ir::Index matched_ids = none;
        for (uint32_t k = 0; k < mon.depth; ++k) {
            matched_ids = b.binary(ir::Inst::Tag::Bor, matched_ids, b.select(matched[k], oid_next[k], none, l), l);
        }
        if (discharged_ids.has_value()) {
            matched_ids = b.binary(ir::Inst::Tag::Bor, matched_ids, *discharged_ids, l);
        }
        const ir::Index waiting =
          b.binary(ir::Inst::Tag::Band,
                   b.binary(ir::Inst::Tag::Bor, load_signal(st.passing, b, l), matched_ids, l),
                   survives,
                   l);
        const ir::Index settle =
          b.binary(ir::Inst::Tag::Band, waiting, b.unary(ir::Inst::Tag::Bnot, live_ids, l), l);

        // One verdict bit per attempt id now, not per slot: a settling pass may
        // belong to an attempt whose slot was freed cycles ago.
        report_pass.assign(ring, none);
        for (uint32_t i = 0; i < ring; ++i) {
            report_pass[i] = mask_bit(settle, i, ring, b, l);
        }

        for (uint32_t k = 0; k < mon.depth; ++k) {
            store_gated(st.oid[k], oid_next[k], active, edge, ring, b, l);
        }
        for (uint32_t s = 0; s < trig; ++s) {
            store_gated(st.tid[s], tid_next[s], active, edge, ring, b, l);
        }
        store_gated(st.firedid, fired_ids, active, edge, ring, b, l);
        store_gated(st.passing, b.binary(ir::Inst::Tag::Band, waiting, live_ids, l), active, edge, ring, b, l);
    }

    // The antecedent's own state, stored down here because a failing
    // obligation may just have killed the threads of the attempt it came from.
    if (mon.has_trigger) {
        store_gated(st.trigger, trig_next, active, edge, mon.trigger.state_count, b, l);
        store_gated(st.fired, group ? mask_any(fired_ids, b, l) : fires, active, edge, 1, b, l);
    }

    store_gated(st.occupied, remaining, active, edge, mon.depth, b, l);
    for (uint32_t k = 0; k < mon.depth; ++k) {
        store_gated(st.pending[k], pending[k], active, edge, obl, b, l);
    }

    // What a cover counts, and what its match action runs on. `cover property`
    // counts the ATTEMPTS that succeeded, so it reads the grouped verdict bits;
    // `cover sequence` counts MATCHES of the sequence, so it reads the per-slot
    // ones from before the grouping. The two coincide unless the antecedent can
    // match twice in one attempt, which is exactly the case grouping exists for.
    const std::vector<ir::Index>& hits = mon.cover_matches ? seq_matched : report_pass;

    if (mon.kind == ir::AssertionKind::Cover) {
        // A cover counts matches and never reports (LRM 16.12.6). More than one
        // attempt can complete in a cycle, so this is a sum and not a flag. Over
        // the verdict bits rather than the slots, because an accept discharging
        // a trigger-covering attempt is a match with no slot behind it.
        //
        // Per ATTEMPT, not per obligation, for `cover property`: a twice-matching
        // antecedent is one cover hit, and none at all when one of its obligations
        // failed (docs/LRM_NOTES.md, note 23).
        ir::Index count = load_signal(st.covered, b, l);
        for (const ir::Index bit : hits) {
            count = b.binary(ir::Inst::Tag::Add, count, b.convert(bit, mask_type(32), false, l), l);
        }
        store_gated(st.covered, count, active, edge, 32, b, l);
    } else if (verdicts) {
        emit_action_pends(report_fail,
                          *fail_body,
                          active,
                          /*unlikely=*/true,
                          st.ctl != nullptr ? &st.ctl->foff : nullptr,
                          b,
                          l);
    }
    if (pass_body.has_value()) {
        // A cover's action block runs on a match (LRM 16.12.6) and an assert's
        // pass action on a pass (LRM 16.14.6) -- the same verdict bit either
        // way, so the same pend. A restrict never gets here: slang rejects an
        // action block on one.
        emit_action_pends(hits,
                          *pass_body,
                          active,
                          /*unlikely=*/false,
                          st.ctl != nullptr ? &st.ctl->poff : nullptr,
                          b,
                          l);
    }
    if (mon.kind == ir::AssertionKind::Expect) {
        // After the pends, so an expect's own action block reaches the reactive
        // ring before the process whose statement it belongs to.
        emit_expect_step(st, id, seeding, active, any_verdict(report_pass, report_fail, b, l), b, l);
    }

    // Last in the body, after every root above has read the pre-shift rings.
    // See emit_history_shift: this ordering is what makes $past(v) the
    // previous tick's sample rather than this one's.
    emit_history_shift(hist, edge, b, l);

    module_.add_process(ir::Process{ .activation = ir::Activation::Observed,
                                     // Not a subscription: an Observed process is resumed once
                                     // per time slot, never woken by a net event. Kept as the
                                     // record of which event delimits this monitor's cycle --
                                     // it is what the edge detection above is derived from.
                                     .wait_events = { ir::WaitEvent{ .signal = mon.clock, .edge = mon.edge } },
                                     .self_start  = false,
                                     .span        = mon.span,
                                     .ir          = b.finish(),
                                     .scope       = cur_scope_id() });

    // A restrict is silent at the end of the trace for the same reason it is
    // silent during one: it has no verdict to drain. So is a COVER, and for the
    // same reason -- it counts matches and never reports (LRM 16.12.6), and
    // slang rejects an `else` on one, so the drain below would emit the DEFAULT
    // failure report for a directive that has no failure to report.
    //
    // Hence `verdicts` rather than the kind: it is exactly "this directive can
    // report a failure". Reachable for a cover only when the property is strong,
    // which is why it went unnoticed while the unbounded operators -- the ones
    // whose natural spelling is strong -- were still refused. Found on
    // tests/sva/diff_until.sv's `cover property (a s_until b)`, which printed
    // one "assertion failed" per pending attempt after the finish banner.
    // And `verdict_at_end_of_trace` rather than `strength == Strong` spelled
    // here: which resolved verdict a Pending attempt collapses to IS the
    // strength, and it is the one rule `not` inverts.
    //
    // AN EXPECT IS EXCLUDED, and this is a semantic choice rather than an
    // omission. An expect's pass and fail statements are statements of the
    // PROCESS it blocked, and LRM 16.17 can only schedule them "after processing
    // the Observed region in which the property completes its evaluation". When
    // the trace ends with the attempt still pending, no such region ever comes:
    // the process stays blocked, so neither its action block nor the statement
    // after the `expect` runs. That is derivation (a) of the three
    // tests/sva/diff_expect.sv sets out; (b) would run the fail action here on
    // the strength argument of LRM 16.12.2, and the reason it is not taken is
    // that emit_monitor_finish lowers the action INLINE into a Final process,
    // which would run an expect's action block in a process that never blocked
    // and never unblocks -- a verdict delivered to nobody.
    //
    // It also means an expect's action block needs no require_finish_inlinable:
    // it has no end-of-trace copy to constrain.
    if (ir::verdict_at_end_of_trace(mon.strength) == ir::AssertionVerdict::Fails && verdicts &&
        mon.kind != ir::AssertionKind::Expect) {
        emit_monitor_finish(mon, st, act);
    }

    return { st.expect_req, st.expect_busy };
}

// A counted monitor's whole per-cycle step, plus the process it lives in.
// Called from emit_monitor once the shared prologue has run, and it owns the
// tail from there: the ring below that call site has no counterpart here.
//
// This is Subsets.run (../libsva/test/merge.zig) with subsets replaced by the
// states of the determinised obligation. Straight-line, exactly as the ring
// path is: every gate is a select and every fold runs over the compile-time
// state set, because a code generator has no run-time state set to walk. The
// only loops emitted are the report ones (emit_counted_pends).
void Lowerer::emit_counted_step(const ir::AssertionMonitor& mon,
                                const MonitorState&         st,
                                // For expect_wake, which names the monitor whose waiters it
                                // drains. Unused by every other directive.
                                uint32_t                      id,
                                const AssertionActions&       act,
                                bool                          verdicts,
                                std::optional<ir::FunctionId> pass_body,
                                std::optional<ir::FunctionId> fail_body,
                                std::span<const ir::Index>    roots,
                                std::span<const ir::Index>    abort_cond,
                                ir::Index                     sat_o,
                                ir::Index                     active,
                                ir::Index                     edge,
                                SvaHistory&                   hist,
                                ir::Builder&                  b,
                                ir::SourceLoc                 l) {
    const uint32_t  obl  = mon.obligation.state_count;
    const ir::Index zero = zero_mask(counter_bits, b, l);

    // Per-attempt identity on the trigger (attempt_ring_size). Width 0 switches it
    // off: `fires` stays the 0/1 bit it was, the seed adds a literal 1, and not one
    // instruction of this appears. That is every counted monitor whose antecedent
    // matches in a single cycle -- `req |=> s_eventually ack` among them.
    const uint32_t ring = attempt_ring_size(mon);
    const bool     ids  = ring != 0;

    // Whether any abort's scope reaches the ANTECEDENT rather than only the
    // obligation. Compile-time, and what decides whether the attempts still matching
    // one have to be counted at all.
    bool covers_trigger = false;
    for (const ir::AssertionAbort& abort : mon.aborts) {
        covers_trigger = covers_trigger || abort.covers_trigger;
    }

    ir::Index id_now = ir::kNoIndex;
    if (ids) {
        id_now = next_attempt_id(st, ring, active, edge, b, l);
    }

    // The trigger, when there is one -- and a counted monitor may well have
    // one. The mix the two disciplines cannot survive is a trigger strictly
    // BELOW an unbounded operator, where the shift would have to land on the
    // antecedent; that is refused as SVA_ERROR_MODE_MIX before it reaches here.
    // A trigger ABOVE one is fine and is the point of the exercise
    // (`req |=> s_eventually ack`): a trigger is EXISTENTIAL, so one state
    // vector serves all of its attempts at once and it needs no pool to be
    // sized. It therefore composes with either obligation discipline unchanged,
    // and this is the ring path's trigger block with the per-attempt GROUPING
    // dropped -- grouping is what needs a bound, and attempt_ring_size says so.
    //
    // The IDS are not the grouping, and they are kept. Existential is the right
    // reading of "did the antecedent match"; it is the wrong reading of "how MANY
    // attempts matched", and that second number is the one a counted seed adds.
    //
    // `trig_next` and `tid_next` outlive the block: the stores are held back to sit
    // with the counter stores, so `st.fired` and `st.firedid` are READ for this
    // cycle's seed before they are written for the next one, exactly as the ring
    // path orders it.
    ir::Index              fires = zero_mask(1, b, l);
    ir::Index              fired_ids = ir::kNoIndex;
    ir::Index              trig_next = ir::kNoIndex;
    std::vector<ir::Index> tid_next;
    if (mon.has_trigger) {
        const uint32_t  trig  = mon.trigger.state_count;
        const ir::Index sat_t = automaton_satisfy(mon, mon.trigger, roots, b, l);
        const ir::Index cur_t = load_signal(st.trigger, b, l);

        ir::Index start =
          b.binary(ir::Inst::Tag::Band, b.constant(mask_const(mon.trigger.first, trig), l), sat_t, l);
        if (st.ctl != nullptr) {
            // $assertoff stops attempts STARTING and lets those already running
            // finish (LRM 20.12), and an implication's attempt starts when its
            // ANTECEDENT does -- so the gate belongs here and nowhere downstream.
            const ir::Index off   = load_signal(st.ctl->off, b, l);
            const ir::Index zeros = zero_mask(trig, b, l);
            start                 = b.select(off, zeros, start, l);
        }
        const ir::Index stepped = automaton_advance(mon.trigger, cur_t, sat_t, b, l);
        trig_next               = b.binary(ir::Inst::Tag::Bor, stepped, start, l);
        fires                   = mask_any(
          b.binary(ir::Inst::Tag::Band, trig_next, b.constant(mask_const(mon.trigger.last, trig), l), l), b, l);

        if (ids) {
            tid_next = advance_attempt_ids(mon, st, ring, cur_t, trig_next, id_now, fired_ids, b, l);
        }
    }

    // An abort whose scope reaches the antecedent resolves at MATCH time: the
    // cycle the antecedent match ENDS, which for `|=>` is the cycle BEFORE the
    // obligation would be seeded. So the match is consumed here and `fires` is
    // forced low, which suppresses the seed for both forms at once. Backwards
    // over the list, because it runs innermost first and an outer abort beats an
    // inner verdict landing in the same cycle (LRM 16.12.7).
    //
    // How many attempts that verdict belongs to is `in_flight`: every attempt with
    // a live antecedent thread, because LRM 16.12.7 resolves an attempt wherever it
    // has got to and a part-matched one is owed a verdict just as much as one whose
    // match ends now. Ungrouped by ids that is a single attempt and the unit the
    // arithmetic used to add; with ids it is the popcount of the union over ALL
    // trigger states, exits included (../libsva/test/backend.c:396-420).
    std::optional<ir::Index> trigger_matched;
    std::optional<ir::Index> trigger_failed;
    ir::Index                in_flight = ir::kNoIndex;
    if (mon.has_trigger) {
        // The scope an abort covering the antecedent reaches. Without ids the only
        // attempt in flight IS the match ending now, so `fires` is the whole of it
        // and the verdict is worth the same one unit the arithmetic below already
        // adds. With ids it is every attempt with a live antecedent thread, counted.
        ir::Index scope = fires;
        if (ids && covers_trigger) {
            ir::Index alive = zero_mask(ring, b, l);
            for (const ir::Index in_state : tid_next) {
                alive = b.binary(ir::Inst::Tag::Bor, alive, in_state, l);
            }
            in_flight = mask_popcount(alive, ring, b, l);
            scope     = mask_any(alive, b, l);
        }

        ir::Index claimed = zero_mask(1, b, l);
        for (uint32_t i = static_cast<uint32_t>(mon.aborts.size()); i-- > 0;) {
            if (!mon.aborts[i].covers_trigger) {
                continue;
            }
            const ir::Index           hit       = b.binary(ir::Inst::Tag::Band, scope, abort_cond[i], l);
            const ir::Index           unclaimed = bit_not(claimed, b, l);
            const ir::Index           fire      = b.binary(ir::Inst::Tag::Band, hit, unclaimed, l);
            std::optional<ir::Index>& into =
              mon.aborts[i].effect == ir::AssertionAbortEffect::Accept ? trigger_matched : trigger_failed;
            into    = into.has_value() ? b.binary(ir::Inst::Tag::Bor, *into, fire, l) : fire;
            claimed = b.binary(ir::Inst::Tag::Bor, claimed, abort_cond[i], l);
        }
        if (trigger_matched.has_value() || trigger_failed.has_value()) {
            fires = b.binary(ir::Inst::Tag::Band, fires, bit_not(claimed, b, l), l);

            // The resolved attempts' threads go with them, or an antecedent
            // completing later would raise an obligation on an attempt already
            // resolved. The ring path gets this from masking its id sets by
            // `finished`; a counted monitor has no such fold, so it clears outright.
            trig_next = b.select(claimed, zero_mask(mon.trigger.state_count, b, l), trig_next, l);
            if (ids) {
                for (uint32_t s = 0; s < mon.trigger.state_count; ++s) {
                    tid_next[s] = b.select(claimed, zero_mask(ring, b, l), tid_next[s], l);
                }
            }
        }
    }

    // How many attempts sit in each state. A state that cannot carry one has no
    // signal and contributes a compile-time zero.
    std::vector<ir::Index> cur(obl, zero);
    for (uint32_t s = 0; s < obl; ++s) {
        if (counted_carries(mon.obligation, s)) {
            cur[s] = load_signal(st.counts[s], b, l);
        }
    }

    // Aborts, against the counts as they stand and BEFORE the advance. The ring
    // applies them after, which it can afford because an attempt-resolving abort
    // discards the post-advance vector anyway. Here it cannot: two states can
    // share a successor -- everything that has just died shares the trap -- and
    // once two counts have merged there is no telling which of them was armed.
    //
    // Being armed is a question about the STATE and not about an attempt's
    // history: the reachable set is `follow(s)`, so one answer serves every
    // attempt counted there and retiring the whole count is exact. That is the
    // case a backend retiring "the" attempt would get wrong. The arming row is
    // compile-time, so an unarmed state costs nothing.
    //
    // Backwards over the list, so an outer abort beats an inner one firing in
    // the same cycle; the `claimed` chain is the branch-free form of the
    // reference's `break`.
    std::optional<ir::Index> aborted_match;
    std::optional<ir::Index> aborted_fail;
    std::vector<std::vector<uint64_t>> arming(mon.aborts.size());
    for (uint32_t i = 0; i < mon.aborts.size(); ++i) {
        arming[i] = abort_arming_row(mon.obligation, mon.aborts[i]);
    }
    for (uint32_t s = 0; s < obl; ++s) {
        if (!counted_carries(mon.obligation, s)) {
            continue;
        }
        ir::Index claimed;
        bool      has_claimed = false;
        for (uint32_t i = static_cast<uint32_t>(mon.aborts.size()); i-- > 0;) {
            if (!row_has(arming[i], s)) {
                continue;
            }
            const ir::Index fire =
              has_claimed ? b.binary(ir::Inst::Tag::Band, abort_cond[i], bit_not(claimed, b, l), l)
                          : abort_cond[i];
            const ir::Index retired = b.select(fire, cur[s], zero, l);

            std::optional<ir::Index>& into =
              mon.aborts[i].effect == ir::AssertionAbortEffect::Accept ? aborted_match : aborted_fail;
            into = into.has_value() ? b.binary(ir::Inst::Tag::Add, *into, retired, l) : retired;

            cur[s]      = b.select(fire, zero, cur[s], l);
            claimed     = has_claimed ? b.binary(ir::Inst::Tag::Bor, claimed, abort_cond[i], l) : abort_cond[i];
            has_claimed = true;
        }
    }

    // `advance`, transposed onto counts. A source's count moves along its
    // follow row, and the row is compile-time.
    //
    // A count must move along exactly ONE enabled transition. The obligation is
    // determinised, but a determinised state is a (subset, cell) pair and two
    // cells may share a valuation, so a single valuation CAN enable several
    // successors of one state -- libsva pins that, and pins that such co-enabled
    // siblings are bisimilar: same `last` bit, same follow row, same subset.
    // Indistinguishable, so which one is taken cannot matter; taking all of them
    // very much does, because the count would be duplicated into each. Summing
    // over enabled predecessors is therefore a real overcount bug, and one that
    // hides on any obligation whose cells happen not to overlap.
    //
    // So the tie is broken the way libsva's own reference monitor breaks it:
    // LOWEST STATE INDEX WINS. The successors of `s` are walked in ascending
    // order and the first satisfied one claims the count, which is the ring
    // path's unrolled free-slot search over a different set. Totality
    // guarantees the set is non-empty, so there is always exactly one winner.
    //
    // Several distinct sources may still converge on one destination, which is
    // Subsets.add merging two subsets' counts -- so a destination's inflow stays
    // a SUM, just a sum of at most one contribution per source. `any` records
    // which destinations anything reaches at all, so an unreachable state costs
    // no instructions rather than an add of zero.
    std::vector<ir::Index> next(obl, zero);
    std::vector<bool>      any(obl, false);
    for (uint32_t s = 0; s < obl; ++s) {
        if (!counted_carries(mon.obligation, s)) {
            continue;
        }
        ir::Index taken;
        bool      claimed = false;
        for (uint32_t d = 0; d < obl; ++d) {
            if (!row_has_at(mon.obligation.follow, static_cast<size_t>(s) * mon.obligation.words, d)) {
                continue;
            }
            const ir::Index sat_d = mask_bit(sat_o, d, obl, b, l);
            const ir::Index wins =
              claimed ? b.binary(ir::Inst::Tag::Band, sat_d, bit_not(taken, b, l), l) : sat_d;
            const ir::Index add = b.select(wins, cur[s], zero, l);

            next[d] = any[d] ? b.binary(ir::Inst::Tag::Add, next[d], add, l) : add;
            any[d]  = true;
            taken   = claimed ? b.binary(ir::Inst::Tag::Bor, taken, sat_d, l) : sat_d;
            claimed = true;
        }
    }

    // Seed, after the advance and before the verdicts, exactly where Subsets.run
    // seeds: `first & satisfied`, under the same lowest-index tie-break the advance
    // uses. As many attempts as matched, and not one -- see `seeded` below.
    //
    // The determinisation is TOTAL, so the entry set always has a satisfied
    // member and it may be a DEAD one. That is what turns a seed whose very
    // first letter does not hold into a failure in the cycle it starts, instead
    // of an attempt that silently never existed -- the reference's
    // `seeded == 0 -> fails += 1`, falling straight out of the same arithmetic.
    // It is the whole reason the construction has to be total.
    //
    // With an antecedent the seed is that antecedent reaching its obligation:
    // `|->` on the trigger's own last cycle, `|=>` on the cycle after, which is
    // why the latter reads last cycle's `fired`. Without one an attempt starts
    // every cycle, and then the seed IS the start and takes the $assertoff gate
    // the trigger's first states take above -- LRM 20.12 lets an attempt already
    // in flight run to its verdict.
    //
    // `seed_ids` is which attempts matched, riding the same one-cycle carry
    // `st.fired` does, and `seeding` is whether there were any.
    ir::Index seed_ids = ir::kNoIndex;
    if (ids) {
        seed_ids = mon.overlap ? fired_ids : load_signal(st.firedid, b, l);
    }
    ir::Index seeding;
    if (mon.has_trigger) {
        seeding = ids ? mask_any(seed_ids, b, l) : (mon.overlap ? fires : load_signal(st.fired, b, l));
    } else {
        seeding = bare_seed_gate(mon, st, b, l);
    }

    const ir::Index one = int_const(1, counter_bits, b, l);

    // How many attempts this cycle's seed stands for. LRM 16.12.1 starts an attempt
    // every tick, so several antecedent matches can END in one cycle -- each of
    // them raises an obligation of its own and each is owed a verdict of its own
    // (LRM 16.14.1). A counted state already carries "how many attempts are here";
    // what was missing is that the seed must put the right number IN. Adding 1 for
    // three attempts reports one failure where three are owed, which is the same
    // wrong answer the ring path gave from the other end.
    //
    // Without ids a cycle seeds at most one attempt -- no antecedent at all, or one
    // whose every match is a single cycle -- and this is the literal 1 the
    // arithmetic below had inline before.
    const ir::Index seeded = ids ? mask_popcount(seed_ids, ring, b, l) : one;

    // A seed's fate is settled BEFORE its count is added: afterwards it is
    // indistinguishable from whatever else landed in that state. It is armed by
    // `first` rather than by a follow row, and armed-ness is a compile-time
    // property of the abort here, so this whole block folds away for a monitor with
    // none. An abort firing in the cycle a seed lands retires the WHOLE seed --
    // every attempt sharing it shares that cycle too.
    for (uint32_t i = static_cast<uint32_t>(mon.aborts.size()); i-- > 0;) {
        if (!rows_intersect(mon.obligation.first, mon.aborts[i].mask)) {
            continue;
        }
        const ir::Index fire    = b.binary(ir::Inst::Tag::Band, seeding, abort_cond[i], l);
        const ir::Index retired = b.select(fire, seeded, zero, l);

        std::optional<ir::Index>& into =
          mon.aborts[i].effect == ir::AssertionAbortEffect::Accept ? aborted_match : aborted_fail;
        into = into.has_value() ? b.binary(ir::Inst::Tag::Add, *into, retired, l) : retired;

        // Resolved, so nothing is seeded -- and `seeding` going low is what makes
        // the outer abort win over an inner one without a second chain.
        seeding = b.binary(ir::Inst::Tag::Band, seeding, bit_not(abort_cond[i], b, l), l);
    }
    ir::Index       entered;
    bool            entry_claimed = false;
    for (uint32_t d = 0; d < obl; ++d) {
        if (!row_has(mon.obligation.first, d)) {
            continue;
        }
        const ir::Index sat_d = mask_bit(sat_o, d, obl, b, l);
        const ir::Index wins =
          entry_claimed ? b.binary(ir::Inst::Tag::Band, sat_d, bit_not(entered, b, l), l) : sat_d;
        const ir::Index starts = b.binary(ir::Inst::Tag::Band, seeding, wins, l);
        const ir::Index add    = b.select(starts, seeded, zero, l);

        next[d]       = any[d] ? b.binary(ir::Inst::Tag::Add, next[d], add, l) : add;
        any[d]        = true;
        entered       = entry_claimed ? b.binary(ir::Inst::Tag::Bor, entered, sat_d, l) : sat_d;
        entry_claimed = true;
    }

    // Verdicts. A count arriving in a `last` state is that many MATCHES and one
    // arriving in a `dead` state that many FAILURES -- counts, not flags.
    // Several attempts sharing a state all resolve in the cycle it resolves,
    // and collapsing them into one verdict would lose every attempt but the
    // first, which is precisely the loss this discipline exists to prevent.
    // Match before failure, and the two masks are disjoint anyway
    // (counted_carries).
    ir::Index matches  = zero;
    ir::Index failures = zero;
    for (uint32_t d = 0; d < obl; ++d) {
        if (!any[d]) {
            continue;
        }
        if (row_has(mon.obligation.last, d)) {
            matches = b.binary(ir::Inst::Tag::Add, matches, next[d], l);
        } else if (row_has(mon.obligation.dead, d)) {
            failures = b.binary(ir::Inst::Tag::Add, failures, next[d], l);
        }
    }

    // The aborts' verdicts join the automaton's. An accept DISCHARGES the
    // attempts it retired -- a pass, which an `else`-less assert reports nothing
    // for but a pass action and a cover's tally both see -- and a reject FAILS
    // them. A trigger-covering one resolves every attempt that was still matching
    // the antecedent, which is `in_flight` of them: one unit when the antecedent
    // matches in a single cycle, because then the only attempt in flight is the one
    // whose match just ended, and a real count when it does not.
    if (aborted_match.has_value()) {
        matches = b.binary(ir::Inst::Tag::Add, matches, *aborted_match, l);
    }
    if (aborted_fail.has_value()) {
        failures = b.binary(ir::Inst::Tag::Add, failures, *aborted_fail, l);
    }
    const ir::Index resolved_by_trigger_abort = ids ? in_flight : one;
    if (trigger_matched.has_value()) {
        matches =
          b.binary(ir::Inst::Tag::Add, matches, b.select(*trigger_matched, resolved_by_trigger_abort, zero, l), l);
    }
    if (trigger_failed.has_value()) {
        failures = b.binary(
          ir::Inst::Tag::Add, failures, b.select(*trigger_failed, resolved_by_trigger_abort, zero, l), l);
    }

    // Only the carrying states persist; a resolved attempt is gone, and its
    // state is one no counter was allocated for.
    for (uint32_t d = 0; d < obl; ++d) {
        if (counted_carries(mon.obligation, d)) {
            store_gated(st.counts[d], next[d], active, edge, counter_bits, b, l);
        }
    }

    // The antecedent's own state and its attempt ids, stored down here so that the
    // `st.fired` and `st.firedid` the seed above read are last cycle's and not this
    // one's.
    if (mon.has_trigger) {
        store_gated(st.trigger, trig_next, active, edge, mon.trigger.state_count, b, l);
        store_gated(st.fired, fires, active, edge, 1, b, l);
        if (ids) {
            for (uint32_t s = 0; s < mon.trigger.state_count; ++s) {
                store_gated(st.tid[s], tid_next[s], active, edge, ring, b, l);
            }
            store_gated(st.firedid, fired_ids, active, edge, ring, b, l);
        }
    }

    if (mon.kind == ir::AssertionKind::Cover) {
        // A cover counts matches and never reports (LRM 16.12.6). `cover
        // sequence` and `cover property` coincide here: they differ only where
        // an antecedent matches twice in ONE attempt, and telling those apart is
        // the grouping a counted monitor does not have -- so there is one
        // reading available and both spellings get it.
        const ir::Index covered = load_signal(st.covered, b, l);
        const ir::Index wide    = b.convert(matches, mask_type(32), false, l);
        const ir::Index count   = b.binary(ir::Inst::Tag::Add, covered, wide, l);
        store_gated(st.covered, count, active, edge, 32, b, l);
    } else if (verdicts) {
        emit_counted_pends(failures, *fail_body, active, st.ctl != nullptr ? &st.ctl->foff : nullptr, b, l);
    }
    if (pass_body.has_value()) {
        emit_counted_pends(matches, *pass_body, active, st.ctl != nullptr ? &st.ctl->poff : nullptr, b, l);
    }
    if (mon.kind == ir::AssertionKind::Expect) {
        // A pass and a failure are counts here rather than bits, so "did the one
        // attempt in flight resolve" is whether either count is nonzero. Only one
        // attempt can be in flight, so the count can only ever be 0 or 1 -- the
        // fold is what makes that independent of the discipline rather than an
        // assumption about it.
        const ir::Index any_match = mask_any(matches, b, l);
        const ir::Index any_fail  = mask_any(failures, b, l);
        const ir::Index verdict   = b.binary(ir::Inst::Tag::Bor, any_match, any_fail, l);
        emit_expect_step(st, id, seeding, active, verdict, b, l);
    }

    // Last in the body, for emit_monitor's reason: every root above has already
    // read the pre-shift rings.
    emit_history_shift(hist, edge, b, l);

    module_.add_process(ir::Process{ .activation  = ir::Activation::Observed,
                                     .wait_events = { ir::WaitEvent{ .signal = mon.clock, .edge = mon.edge } },
                                     .self_start  = false,
                                     .span        = mon.span,
                                     .ir          = b.finish(),
                                     .scope       = cur_scope_id() });

    // `verdicts` and not the kind, for emit_monitor's reason: a cover has no
    // failure to drain, and a counted cover is the shape that exposed it. An
    // expect is excluded for emit_monitor's reason too: its verdict belongs to a
    // process that can no longer be scheduled.
    if (ir::verdict_at_end_of_trace(mon.strength) == ir::AssertionVerdict::Fails && verdicts &&
        mon.kind != ir::AssertionKind::Expect) {
        emit_monitor_finish(mon, st, act);
    }
}

// End of trace, counted (LRM 16.12.2). Everything still sitting in a carrying
// state is an attempt the trace ended underneath: a weak one holds and reports
// nothing, a strong one owes a failure EACH. The residue is a run-time number
// for the same reason the per-cycle verdicts are, so it is a loop rather than
// the ring path's unrolled slot walk.
//
// This is what makes `s_eventually p` report only at the end of the trace: no
// cycle before it can tell the witness apart from one that has not arrived yet,
// so no count ever reaches a dead state and the whole backlog resolves here.
void Lowerer::emit_counted_finish(const ir::AssertionMonitor& mon,
                                  const MonitorState&         st,
                                  const AssertionActions&     act,
                                  ir::Builder&                b,
                                  ir::SourceLoc               l) {
    ir::Index residue = zero_mask(counter_bits, b, l);
    for (uint32_t s = 0; s < mon.obligation.state_count; ++s) {
        if (counted_carries(mon.obligation, s)) {
            residue = b.binary(ir::Inst::Tag::Add, residue, load_signal(st.counts[s], b, l), l);
        }
    }
    // An end-of-trace failure is a fail action like any other, so LRM 20.12's
    // FailOff suppresses it too.
    if (st.ctl != nullptr) {
        const ir::Index enabled = bit_not(load_signal(st.ctl->foff, b, l), b, l);
        const ir::Index zeros   = zero_mask(counter_bits, b, l);
        residue                 = b.select(enabled, residue, zeros, l);
    }

    // Inline rather than pended, for emit_monitor_finish's reason: Final is past
    // the last time slot's Reactive drain, so a pend would queue a body nothing
    // will ever resume.
    repeat_verdict(
      residue,
      [&] {
          if (act.fail != nullptr) {
              lower_stmt(*act.fail, b);
          } else {
              b.severity(static_cast<uint32_t>(ir::Severity::Error), mon.banner, "assertion failed", {}, l, 0);
          }
      },
      b,
      l);
}

// One pend per verdict bit: two attempts can resolve in the same cycle and
// each is its own action-block execution (LRM 16.14.1). The body is scheduled
// into the Reactive region rather than run here, because this monitor is an
// Observed process and LRM 4.4.2.5 puts an assertion's pass/fail code in
// Reactive -- which is also what orders a report after the same edge's Active
// region $display, rather than racing it.
//
// `filter` is the assertion's LRM 20.12 pass/fail action suppression bit, or
// null in a design that controls nothing. It gates the PEND and nothing
// else, which is the whole of what 20.12 asks for: the automaton has
// already stepped, every state store is upstream of this, and a cover's
// tally is counted before it -- so an assertion whose reports are switched
// off is still running, and still says so through everything but the
// report. Gating `active` instead would freeze the monitor, which is what
// $assertoff means and emphatically not what $assertfailoff does.
void Lowerer::emit_action_pends(const std::vector<ir::Index>& verdict,
                                ir::FunctionId                body,
                                ir::Index                     active,
                                bool                          unlikely,
                                const ir::Signal*             filter,
                                ir::Builder&                  b,
                                ir::SourceLoc                 l) {
    // Hoisted: one load however many verdict bits share the filter.
    const ir::Index allow = filter != nullptr ? bit_not(load_signal(*filter, b, l), b, l) : ir::Index{};
    for (const ir::Index bit : verdict) {
        ir::Index take = b.binary(ir::Inst::Tag::Band, active, bit, l);
        if (filter != nullptr) {
            take = b.binary(ir::Inst::Tag::Band, take, allow, l);
        }

        b.open_region();
        b.reactive_pend(body, l);
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(take, then_region, else_region, l, unlikely);
    }
}

// The gate on a bare sequence's seed, shared by both disciplines.
//
// With no antecedent an attempt starts every cycle, so the seed IS the start
// and takes the same $assertoff gate the trigger's first states take. With an
// antecedent it must NOT: seeding is then an attempt already in flight reaching
// its obligation, and LRM 20.12 lets that one run to its verdict.
//
// An `expect` narrows the same gate from the other side. LRM 16.17 starts "a
// single thread of evaluation for the given property on the subsequent clocking
// event", so its attempt begins because the STATEMENT executed, and the pending
// request is what says so. Deliberately the same gate rather than a second
// mechanism: "may an attempt start this cycle" already has an owner, and an
// expect is one more reason for the answer to be no.
//
// An expect's `off` bit can never be set, because LRM Table 20-6 makes an
// expect assertion_type 16 rather than the concurrent-assertion bit 1 and
// lower_assert_control refuses 16 outright. It is still read: the term costs one
// fold of a signal that is provably zero, and leaving it out would make this
// read as though the interaction had been forgotten rather than decided.
ir::Index Lowerer::bare_seed_gate(const ir::AssertionMonitor& mon,
                                  const MonitorState&         st,
                                  ir::Builder&                b,
                                  ir::SourceLoc               l) {
    ir::Index gate = st.ctl != nullptr ? bit_not(load_signal(st.ctl->off, b, l), b, l) : true_const(b, l);
    if (mon.kind == ir::AssertionKind::Expect) {
        gate = b.binary(ir::Inst::Tag::Band, gate, load_signal(st.expect_req, b, l), l);
    }

    return gate;
}

// The `expect`-only tail of one monitor step (LRM 16.17), shared by both
// disciplines. This is the whole of what makes an `expect` more than an assert
// over the same property: the request is consumed, the attempt in flight is
// tracked, and the cohort waiting on it is woken.
//
// `seeding` is the gate the seed above used, passed in rather than recomputed
// so that the request is cleared in exactly the cycle it was consumed. A
// request the disable condition or an $assertoff held back is still owed an
// attempt, and clearing it there would park its processes for good.
//
// `resolved` is one bit -- did the attempt in flight reach a verdict this cycle
// -- and one bit suffices BECAUSE only one attempt can be in flight: the
// statement refuses to join while `expect_busy` is set, so the waiter queue is
// always exactly the cohort of the next attempt to resolve. Nothing here needs
// attempt ids, and that is not luck about this monitor's shape: it is the
// invariant that refusal buys.
//
// Called after the action pends, which is what orders an `expect`'s own
// pass/fail block before the statements that follow the `expect` -- both land
// in the Reactive region of this slot and the ring drains in push order. That
// is the order LRM 16.17 describes and the order tests/sva/diff_expect.sv
// measures (EXP 2 before EXP 4).
void Lowerer::emit_expect_step(const MonitorState& st,
                               uint32_t            id,
                               ir::Index           seeding,
                               ir::Index           active,
                               ir::Index           verdict,
                               ir::Builder&        b,
                               ir::SourceLoc       l) {
    const ir::Index started = b.binary(ir::Inst::Tag::Band, active, seeding, l);
    // `active` and not the bare verdict bits, and this is load bearing rather
    // than defensive. The verdicts are COMBINATIONAL over state the step stores
    // only at an edge, and the monitor body runs in every time slot -- so
    // between two ticks they read this slot's satisfied mask against the last
    // tick's pending states and can perfectly well come out high. Every other
    // consumer is already gated: the state stores through store_gated, the
    // action pends through emit_action_pends' own `active` term. Ungated here,
    // the first slot after a seed woke the process a whole half-cycle early
    // (t=10 rather than t=15 on tests/sva/diff_expect.sv), which is the OFFSET
    // that fixture exists to measure.
    const ir::Index resolved = b.binary(ir::Inst::Tag::Band, active, verdict, l);
    const ir::Index req_lv   = b.signal(st.expect_req, l);
    const ir::Index zeros    = zero_mask(1, b, l);
    const ir::Index req_now  = load_signal(st.expect_req, b, l);
    const ir::Index req_next = b.select(started, zeros, req_now, l);
    b.binary(ir::Inst::Tag::Store, req_lv, req_next, l);

    // Set by the edge that seeds and cleared by the one that resolves, and the
    // two can be the SAME edge: a one-cycle property is seeded and matched in
    // one step, because the seed runs before the verdicts. Clearing last is what
    // makes that land on 0 instead of latching an attempt that is already done.
    //
    // Ungated by `active`, unlike the automaton's own state: both terms already
    // carry it, and a plain store keeps this readable as the two-line state
    // machine it is.
    const ir::Index busy_lv    = b.signal(st.expect_busy, l);
    const ir::Index busy_now   = b.binary(ir::Inst::Tag::Bor, load_signal(st.expect_busy, b, l), started, l);
    const ir::Index unresolved = bit_not(resolved, b, l);
    const ir::Index busy_next  = b.binary(ir::Inst::Tag::Band, busy_now, unresolved, l);
    b.binary(ir::Inst::Tag::Store, busy_lv, busy_next, l);

    b.expect_wake(resolved, id, l);
}

// Whether any of these verdict bits is set: one cycle's verdicts folded into
// the single "the attempt in flight resolved" bit emit_expect_step wants. A
// pass and a failure are both resolutions, which is why the two vectors fold
// together rather than being told apart here -- what to DO about a verdict is
// the action pends' business, and they have already read them.
ir::Index Lowerer::any_verdict(const std::vector<ir::Index>& pass,
                               const std::vector<ir::Index>& fail,
                               ir::Builder&                  b,
                               ir::SourceLoc                 l) {
    ir::Index any = zero_mask(1, b, l);
    for (const std::vector<ir::Index>* side : { &pass, &fail }) {
        for (const ir::Index bit : *side) {
            any = b.binary(ir::Inst::Tag::Bor, any, bit, l);
        }
    }

    return any;
}

// End of trace (LRM 16.12.2), libsva's emitted_finish. A weak attempt that
// never completed holds, so it reports nothing; a strong one is a failure. The
// state teardown emitted_finish also does is dropped: a Final process runs
// after every other process has settled and nothing reads the state again.
//
// The fail action runs here too, but lowered inline rather than pended: Final
// is past the last time slot's Reactive drain, so a pend would queue a body
// that nothing will ever resume -- exactly the silent drop PRINCIPLES.md forbids.
// Inlining costs the action a restriction the pended copy does not carry, and
// require_finish_inlinable is where that is charged: a Final process is
// resumed once and destroyed, so this copy may not suspend at all. Only the
// FAIL action is inlined, so only it is checked -- a pass action has no
// end-of-trace copy to constrain.
void Lowerer::emit_monitor_finish(const ir::AssertionMonitor& mon,
                                  const MonitorState&         st,
                                  const AssertionActions&     act) {
    const ir::SourceLoc l = mon.span.start;
    ir::Builder         b(module_);

    if (act.fail != nullptr) {
        require_finish_inlinable(*act.fail);
    }

    if (mon.tracking == ir::AssertionTracking::Counted) {
        emit_counted_finish(mon, st, act, b, l);
        module_.add_process(ir::Process{
          .activation = ir::Activation::Final, .span = mon.span, .ir = b.finish(), .scope = cur_scope_id() });
        return;
    }

    const ir::Index occupied = load_signal(st.occupied, b, l);
    // The end-of-trace failure is a fail action like any other, so LRM
    // 20.12's FailOff suppresses it too. Hoisted out of the slot loop: one
    // load serves every attempt still pending.
    const ir::Index fail_ok =
      st.ctl != nullptr ? bit_not(load_signal(st.ctl->foff, b, l), b, l) : ir::Index{};

    // One report per ATTEMPT here too (docs/LRM_NOTES.md, note 23) -- and per attempt is not
    // per slot in either direction. A strong attempt still pending at the end of
    // the trace owes ONE failure however many obligations of it are unresolved, and
    // a single slot may stand for SEVERAL attempts, because attempts whose
    // antecedent matches ended in the same cycle raised the same obligation. Both
    // directions fall out of one union: the ids of every still-occupied slot, one
    // report per id in it.
    //
    // Reachable by the same shape the per-cycle verdicts are: a ranged antecedent
    // that matched twice near the end for the first direction, and one whose
    // matches ended together for the second.
    //
    // Ungrouped there are no ids, and then an obligation IS an attempt and a slot
    // is its own report -- which is the emission this used to have for every
    // monitor.
    const uint32_t ring = attempt_ring_size(mon);
    ir::Index      pending_ids = ir::kNoIndex;
    if (ring != 0) {
        pending_ids = zero_mask(ring, b, l);
        for (uint32_t k = 0; k < mon.depth; ++k) {
            const ir::Index busy  = mask_bit(occupied, k, mon.depth, b, l);
            const ir::Index ids   = load_signal(st.oid[k], b, l);
            const ir::Index zeros = zero_mask(ring, b, l);
            pending_ids           = b.binary(ir::Inst::Tag::Bor, pending_ids, b.select(busy, ids, zeros, l), l);
        }
    }
    const uint32_t reports = ring != 0 ? ring : mon.depth;
    for (uint32_t i = 0; i < reports; ++i) {
        ir::Index held = ring != 0 ? mask_bit(pending_ids, i, ring, b, l)
                                   : mask_bit(occupied, i, mon.depth, b, l);
        if (st.ctl != nullptr) {
            held = b.binary(ir::Inst::Tag::Band, held, fail_ok, l);
        }

        b.open_region();
        if (act.fail != nullptr) {
            lower_stmt(*act.fail, b);
        } else {
            b.severity(static_cast<uint32_t>(ir::Severity::Error), mon.banner, "assertion failed", {}, l, 0);
        }
        const ir::ExtraSlice then_region = b.close_region();

        b.open_region();
        const ir::ExtraSlice else_region = b.close_region();

        b.cond(held, then_region, else_region, l, /*unlikely=*/true);
    }

    module_.add_process(ir::Process{
      .activation = ir::Activation::Final, .span = mon.span, .ir = b.finish(), .scope = cur_scope_id() });
}

} // namespace clotho::lower
