# Regions (guarded static dispatch)

Fuse processes into one function and compile their sensitivity lists into
guards inside it, so that a synchronous design's instant becomes a straight
line of `if (trigger) body;` instead of a round trip through the wait buckets
and the active ring per process. A generalisation of comb fusion
(comb-fusion.md) that admits edge-sensitive members, and the mechanism
clock-wave.md wanted without teaching the backend what a clock is.

Status: **P1 + P2 landed** (`SimulationConfig::regions`, default on); P3 and P4
not started. §2 is the P0 measurement that justified it and §4 records what
each phase is; §9 is the as-built account of P1 + P2, including the two things
the design above got wrong. Read §2 before investing further: it revises
clock-wave.md's estimate of the scheduling share upward by 30-60x, and it moves
the biggest single bucket from dispatch to NBA staging.

---

## 1. What this replaces

Per activation, today (map in clock-wave.md §1; the code is `emit_active_run`,
ir/llvm/Scheduler.cpp, emit_active_run):

- the writer's blocking store computes edge facts inline
  (`service_snapshot`:9765, `emit_edge_facts`:9824) and scans the static
  bucket for the signal and edge (`emit_static_scan_rt`:11085): per entry a
  handle load, a flag-pointer load, a flag load, a compare, and on a wake a
  ring push plus a flag store;
- the drain pops the handle, tests bit 0, and calls or resumes (the
  `active.thunk` block of `emit_active_run`);
- the body runs **every** member of its fusion group unconditionally (the
  per-member loops in `lower_group`:4463, thunk and coroutine paths alike) and
  clears the shared sched flag on the way out (one `store i32 0, sched_ptr_`
  per emission path, commented `re-arm: ready to be woken`).

A group has no idea *why* it woke. The sched flag is one `i32`; nothing
records which bucket fired. That is the gap this design closes, and closing it
is what lets an edge-sensitive process join a group at all —
`comb_fuse_verdict` rejects any process with a non-`Any` edge
(Analysis.cpp:1692, verdict `CombFuse::EdgeSensitive`, Analysis.h:130)
precisely because comb fusion runs its members unguarded.

## 2. Honest accounting — measured, not estimated

clock-wave.md §2 put the residual scheduling share at "order ~1%" from a
per-group op count, and asked for a `sim_drain`-self vs body-self profile
before anything was built (§7). That profile, plus an isolated cost model, is
P0.

Measured 2026-08-23, Release + LLVM 22, AMD Ryzen 5 7640U, min of 3. Reproduce
the cost model with `bench-sched/cost_model.py`; each family is a matched A/B
pair whose arms differ only in the mechanism under test, and the figure is the
slope of retired instructions against the parameter, so compile time and
startup cancel exactly.

### 2.1 Cost per site

| mechanism | instructions | flat over |
|---|---|---|
| process activation round trip — bucket entry, push, flag set, pop, tag test, indirect call, flag clear, prologue, and the operand reloads a fused body would forward | **65** | n = 8..64 (72.2 / 67.8 / 65.8 / 64.9) |
| blocking store to a signal with waiters — pre-store load, `emit_edge_facts`, per-bucket branch, one-entry scan | **33** | n = 2..64 (33.41 / 32.97 / 33.03 / 33.02 / 33.00) |
| NBA, per straight-line block per activation — `nba_block_enqueue`, chain, commit call, musttail, `nba.check` | **~38** | fit over n = 2..32 |
| NBA, per signal — shadow store, commit load/store/compare (**plus the 33 above** when the target has waiters) | **~3.5** | same fit |
| the arithmetic being decorated — one 32-bit 4-state `xor` and its store | **15** | n = 2..64 |

The service figure is the marginal cost with the sched flag already set, i.e.
the common case for the second and later stores of an activation. So an
activation that performs one 32-bit `xor` costs 65 + 33 + 15 = 113
instructions, of which **15 are the design**.

### 2.2 picorv32, 1M-cycle TIMEOUT workload

Total, by differencing a full `run` against `build --emit=llvm -o /dev/null`
over the same sources, which removes the frontend and LLVM exactly:

- **20,155 retired instructions per simulated clock cycle** (23.429e9 −
  3.274e9, /1e6).
- **195.6 indirect dispatches per simulated clock cycle** (214.345e6 −
  18.743e6). `ex_ret_ind_brch_instr` is exactly **1.000** per process
  activation — calibrated on the `dispatch` family, where adding 8, 16 and 32
  processes moved the counter by exactly 8, 16 and 32. It also counts one per
  NBA commit function, per coroutine resume and per cross-definition member
  call, so 195.6 is an upper bound on process activations.

Profile, jitdump-resolved, as a share of the 47.2% of samples attributable to
simulation (the rest is the in-process JIT compile):

| bucket | % of sim | instr/cycle |
|---|---|---|
| design bodies (106 symbols) | 67.1 | 13,522 |
| NBA plumbing (`nba_block_enqueue` + 124 sampled `commit_blk*`) | 22.7 | 4,579 |
| `sim_drain` (ring pop, tag test, indirect call, region blocks) | 10.0 | 2,024 |
| wheel / timer | 0.15 | 30 |

**Cross-check.** The drain's 2,024 instr/cycle over 195.6 dispatches is 10.4
instructions of drain loop per dispatch, which is what the pop / tag test /
indirect call sequence should cost. Independently, the measured 65-instruction
round trip minus the ~10 that live in `sim_drain` says ~55 instructions per
activation are emitted *into the bodies* — the bucket scan at the writer, the
push, the flag set, the flag clear at the callee, the prologue, the reloads.
The two derivations agree to 0.5%, which is the evidence that the 67.1%
"design bodies" bucket is mostly not arithmetic.

**Verdict.** Non-arithmetic work is bounded below by 32.7% (drain + NBA
plumbing, neither of which contains any design logic) and above by roughly 63%
(195.6 × 65 / 20,155, an over-estimate because some of those dispatches are
NBA commit calls at ~38 rather than activations). Either end of that range is
30-60x the 1% clock-wave.md assumed, for one reason: that estimate counted
per-group bucket ops and omitted the per-activation round trip, the NBA block
chain, and the commit-and-service functions. The wheel, which both that
document and a first reading of a Debug profile flag as a suspect, is
**0.15%** — it is not a problem and should not be touched.

Design shape, same build: 272 `always` processes, 226 comb-eligible, 118 still
unfused singletons, 17 comb groups holding 62 members (largest 14), 42 fused
groups in total, 447 wait buckets, 592 NBA commit functions, 1,191 service
sites, 88,315 lines of emitted IR. (comb-fusion.md's 2026-07-08 table said 180
unfused singletons; coverage has improved, the conclusion has not.)

### 2.3 The floor, on a design with no logic in it

An 8-bit counter — `assign wrap`, one `always @*`, one `always @(posedge
clk)`, plus a clock generator and an `initial` — measures **960 instructions
and 14 indirect dispatches per simulated clock period**. Its arithmetic is
about 20 instructions. 14 × 65 = 910.

The frontend produces **eight** `always` processes for that 30-line design:
three carry the RTL (`assign wrap`, the comb block, the flop), **four are port
copies**, and one is the clock generator. `--stat=comb-fuse` fuses exactly one
pair: `assign wrap` with the `u.q`→`tb.q` port copy. Neither the flop
(excluded: `edge=1`) nor the comb block (demoted by the depth-1 interior guard
in `compute_fusion_groups`) joins anything. This is the picorv32-at-noise
result of comb-fusion.md:12 reproduced in 30 lines, and it is why the useful
unit is not "dependent comb processes" but "the processes that run in one
instant".

## 3. Design

### 3.1 Trigger provenance: at the store, not at region entry

The tempting design is to sample each watched signal's previous value at
region entry and re-derive `posedge` there — `emit_edge_facts` is pure IR
emission on an old/new SSA pair and would work anywhere, and the frontend
already does exactly this for SVA monitor clocks (`lower/Lowerer.cpp, alloc_monitor_state`, sample
against `clkprev` at the previous Observed point).

**It is wrong.** A writer that stores `clk = 1; clk = 0;` inside one instant
raises a real posedge event: the first store classifies pos and queues the
flop, the second classifies neg and finds no bucket, and the flop then runs. A
region-entry sample sees `prev = 0, now = 0` and skips it. That is a
divergence on legal input, not an as-if transform, so region-entry sampling is
rejected.

Keep the classification exactly where it is and change only what it writes.

### 3.2 The trigger word

Per group, one word (`clotho.proc_trig`, parallel to `clotho.proc_sched`,
ir/llvm/StateLayout.cpp, build_sched_flags), one bit per entry of `group_events_[g]`
(ir/llvm/Backend.h, OpenNamedBlock). At every site that today scans a bucket and pushes a
handle, `or` the entry's mask into the word alongside the push. The group body
loads the word at entry, guards each member on `(trig & member_mask) != 0`,
and zeroes the word at the end — the same timeline as the sched flag, which is
held across the body so that an intra-group store cannot re-enqueue the group
(the `re-arm: ready to be woken` store in `lower_group`); a trigger bit raised
mid-body is dropped for the same reason and by the same argument
(comb-fusion.md:115: the dropped re-wake is work the topo order already did).

Members whose sensitivity is identical share a mask, so a whole clocked
sub-region collapses to one load, one `and` and one branch — the effect
clock-wave.md §3 wanted from a per-clock wave function, obtained without any
notion of a clock, by mask sharing rather than by hoping GVN finds it.

Cost: two instructions at the wake site, in exchange for deleting a per-entry
handle load, flag-pointer load, flag load and compare from the scan.

### 3.3 Fail-safe polarity

Any wake path that cannot name a trigger — the `self_start` seed (the seed
loop in `emit_active_run`), a dynamic wait, a virtual-interface waiter list,
an event trigger — stores all-ones. An unclassified wake therefore runs every
member, which is exactly today's behaviour. Same polarity as `TagInfo::pure`
(Analysis.h:58): the default is the conservative answer, and a newly added
wake path is safe until someone classifies it.

### 3.4 Why this is cheap to land: guards are monotone

A fused group today runs every member on any union event. A guarded member
runs on a subset of that. So the transform **cannot introduce an execution**;
it can only remove one, and with §3.3 it cannot remove one either unless the
trigger bit says the member's own event did not fire. The failure mode is not
"spurious run" — which is the hazard comb fusion had to argue away, and which
required the purity whitelist, the read-set containment check and the
change-detection argument (comb-fusion.md §"Why this is LRM-legal") — but
"missing run", closed by construction.

That inverts the risk profile of the whole feature relative to comb fusion,
and it retires five of the eight `CombFuse` verdicts as fusion blockers, since
`EdgeSensitive`, `ImpureOp`, `HasCall`, `UnsupportedWrite` and
`ReadOutsideSens` all exist only because a fused member may run when its own
trigger did not (Analysis.h:123-146). Proof that they are guard artifacts and
not purity requirements: identical-sensitivity fusion imposes none of them
(ir/llvm/Analyses.cpp, compute_fusion_groups gates on `simple_always` alone) because its members'
wake set equals the union by construction.

`NotSimpleAlways` stays — a member must still be a suspension-free
straight-line body to sit inside one function. The `is_simple_always`
rejection of a blocking write to a signal in the process's own sensitivity
also stays: the guard tests wake-time facts, and the held sched flag still
swallows a mid-body self-trigger. The SCC rejection in `compute_fusion_groups`
stays: a combinational loop needs the re-wake fixpoint.

### 3.5 Why the synchronous subset collapses, without a clock

Two properties, both already computed:

1. `compute_subscription_order` (ir/llvm/Analyses.cpp) condenses the
   blocking-write wake graph and marks nontrivial SCCs (`scc_nontrivial_`,
   ir/llvm/Backend.h, OpenNamedBlock), so an acyclic component settles in one pass in rank
   order.
2. NBA cuts the sequential feedback edges — a flop's effect appears at the
   commit, not in the pass.

A region whose blocking wake graph is acyclic and whose only feedback is via
NBA therefore converges in one pass, and the instant becomes `region();
nba_commit(); rt_wheel_advance();`. "Cycle-accurate design" is not a shape to
detect; it is the case where an acyclicity proof the backend already computes
succeeds. Where it fails, the region body keeps a `do { … } while (changed)`
fixpoint over the same members.

## 4. Phasing

| | scope | effort | risk |
|---|---|---|---|
| P0 | this document; cost model (`bench-sched/cost_model.py`); picorv32 attribution | done | — |
| P1 | trigger bits in the wake word (§9.1) | **landed** | the def-relative GEP stride was the one silent-corruption hazard |
| P2 | member guards for existing groups | **landed** | monotone per §3.4; the intra-group edge of §9.2 was the one real bug |
| P3 | admit `CombFuse::EdgeSensitive` members (relax the `CombFuse::Eligible` filter, ir/llvm/Analyses.cpp, compute_fusion_groups); leave the other four verdicts alone | 3-5 d | med — union sensitivity now spans clock and level events, so activation count rises while body count falls; that trade is the cost model to watch |
| P4 | scalar NBA as an in-region `select` | 2-4 w | high — see below |

P2 pays for itself independently of regions: it discharges the two follow-ups
comb fusion already owes. It deletes the documented over-evaluation ("a
stimulus touching one branch of a wide component re-runs the untouched
branch", comb-fusion.md:119) and it *is* the per-member dirty bits that
document names as the honest fix for the depth-1 interior-guard residual
(comb-fusion.md:122), which lets the interior guard relax from a hard reject
to a cost model and fixes the `star` 10.9x pathology (comb-fusion.md:107)
properly instead of avoiding it.

**P4 is a separate feature.** The mechanics are local — staging has two entry
points (`lower_store_nb`:18401, `register_block_store`:18546), commit is per
`BlockSig`, and `nba_meta` needs no renumbering — but the qualification proof
is wide. A scalar NBA target may be observed through the shadow/commit path by
static and dynamic wait queues, virtual-interface waiters, `<= @e` triggers,
intra-assignment record pools, VCD, `$strobe`/`$monitor`, Preponed sampling
and Observed properties, force/release writer processes, Re-NBA reactive
writers, and cross-instance readers. Most are compile-time checkable today;
two are not:

- a **whole-design read closure** per signal. The wake graph covers wake
  edges, not passive reads (Analysis.h:171-185).
- **once-per-instant activation** of the group, or an equivalence argument.
  NBA commit is defined at drain quiescence and re-enters the drain (the
  `nba.commit` block of `emit_active_run`), so an inlined commit is visible to
  a second same-instant activation.

And one decision, not a discovery: a cooperative `$finish` drains the queue
and leaves staged NBAs **uncommitted** (the `rt_finishing` test in the active
drain), whereas an inlined commit has already applied. That is a
byte-identical differential failure on any design that finishes mid-instant.

A slice of P4 is free at P1 cost: the commit function's own wake scan
(`emit_nba_service`:11184) becomes a trigger-bit set like every other wake
site, which takes a bite out of the 22.7% without touching the shadow.

## 5. Interaction with existing passes

- `compute_subscription_order` (:8049) is unchanged and supplies member order
  and the acyclicity fact.
- `compute_fusion_groups` (:9072) keeps all four phases. P3 relaxes the
  phase-2 candidate filter (:9139); phase 3's canonical union already keys on
  `(signal, edge)` (the `canonical` lambda, :9085) so it needs no change to
  carry edges.
- Group bodies stay in `lower_group` (:4463) with member bodies inline. A
  guard is a `CondBr` diamond around the per-member `lower_region` call; the
  emission model is insert-point-relative and already tolerates mid-body
  control flow and multiple returns (`emit_finish_exit`:20149). On the
  coroutine path, per-member frame temps are built in the ramp (the
  `member_strs` snapshots in `lower_group`) — guard the body, never the temp
  construction.
- `share_twin_groups` (:8810) hashes group bodies for cross-instance dedup, so
  guard code and masks must be per-definition constants, not per-instance.
- The tagged-thunk handle and the active ring are reused as-is: a region is a
  thunk, so the hybrid with coroutines, delays and dynamic sensitivity needs
  no new dispatch machinery.

## 6. What stays on the event-driven path

Suspension-bearing processes, delays, dynamic sensitivity, forks,
combinational loops (SCCs), multi-driver nets, and any wake a store site
cannot classify. Regions thin the store-site path; they do not remove it.
`fire_event_trigger`, virtual-interface waiter lists and dynamic queues are
per-store effects that cannot move.

Bucket *elision* — dropping a signal's static bucket entirely when every
subscriber is inside one group — is decidable from existing data (invert
`group_events_`, then require `wait_counts().dyn` empty for the signal,
`vif_wait_` clear, no `trigger_id`, not an event), but the dynamic-wait
capacity term is a blunt per-design over-approximation: one task doing
`@(posedge clk)` anywhere adds capacity on `(clk, pos)` and vetoes elision for
that clock everywhere (the task-wait term of `wait_counts`). Treat elision as
a later, separate question.

## 7. Verification

Gate: one default-true `SimulationConfig` field, config-only, never on the CLI
— the contract at SimConfig.h:113 and the shape prescribed for `never_x`
(never-x-inference.md:135).

`tests/region_test.cpp`, structured like `comb_fusion_test.cpp` (`run_capture`
and `check_all_lowerings` across default / no-thunk / no-fuse), every expected
string oracle-verified against iverilog. The pins that matter:

1. **A clock that glitches inside one instant (`clk = 1; clk = 0;`) still
   fires a fused flop.** Write this one first: it is the pin that rejects
   §3.1's region-entry sampling, and it must fail against that design and pass
   against the trigger word.
2. An async-reset flop whose two triggers fire in the same instant runs once.
3. `$display` in a guarded member has an exact per-trigger activation count —
   which should now hold for a *fused* impure member, unlike today.
4. A member woken through a dynamic `@sig` in a task still runs (exercises the
   all-ones fallback of §3.3).
5. `self_start` seeds run every member.
6. Structural pins counting emitted symbols, via the `emit()` / `count_substr`
   helpers.

There is **no whole-corpus byte-identical differential runner** in the tree;
only per-test A/B and `bench-diff.py`, which compares timings across refs, not
output. P1-P3 can stand on per-case pins plus picorv32 byte-identity. P4
cannot — budget the corpus runner as a P4 prerequisite. The seam is that every
test TU already funnels through a config-taking `run_capture`; what is missing
is a shared registry of (source, expected) pairs to iterate.

## 8. Recommendation

1. **P1 + P2 as one landed change, then P3.** Done; §9 is the account.
2. Re-measure `-O` after P3. The `+2% cycles / +4-5% wall` verdict on picorv32
   (module-split.md:120-128) is a measurement of 106 tiny functions over a
   global state struct, not a verdict on O3; §2.1's 65-instruction round trip
   includes the operand reloads a fused body forwards.
3. Hold P4 until P3's measured dispatch count is in hand. Its prize is the
   largest single bucket, its cost is a new whole-design analysis plus a
   semantic decision on `$finish`, and it should not ride in on a mechanical
   change.
4. Do not build: region-entry prior-value sampling (§3.1), a clock-detection
   pass, or a profitability threshold gating two divergent bodies —
   domain-spec.md is the record of how that ends.

## 9. As built (P1 + P2)

Two things in §3 were wrong, and one encoding in §3.2 was replaced. This
section is the record; the sections above are the design as it now stands.

### 9.1 The trigger bits ride in the sched flag, not a parallel word

§3.2 proposed a `clotho.proc_trig` array beside `clotho.proc_sched`, with the
wait-bucket entry carrying `(trig_ptr, mask)`. That would have widened
`wait_elem_ty`, which that type's own comment refuses on the grounds that one
entry exists per subscription per edge across the whole design, so widening it
spends memory in every design including the ones with no guarded group at all.

What landed instead: the scheduled flag widens from `i32` to `i64` and becomes
the wake word. Bit 0 is the queued flag, unchanged in meaning and still the
only thing compared against zero; bits 1..63 are trigger bits. The bit index of
a `(signal, edge)` is a **design-global colour**, so the mask is a compile-time
constant at the scan site and no entry field is needed. Consequences:

- Two words became one. `wait_elem_ty` is untouched, and so are `wait_counts`,
  the bucket padding accounting, and the def-relative `wait_base` indirection --
  all of which §4 listed as P1's risk and none of which P1 ended up touching.
- The colour is global, so a `(signal, edge)` in two guarded groups uses one bit
  in each group's own word: no cross-group interference. Only a collision
  *within* one group costs anything, and it costs precision, never correctness.
- The one real hazard was `materialize_group_ctx`, which indexes a deduped
  instance's flag array with the element type as a GEP **stride**. Left at
  `i32` against an `i64` array it addresses byte offset `4k` — a silent wrong
  answer confined to deduped instances. `sched_word_ty()` exists so that type
  has one home.
- Cost at the wake site is now zero on the common path: the wake arm already
  stored a constant, so it stores `1|mask` instead of `1`. The only new work is
  the already-queued arm (§9.2).

### 9.2 The wake word must be re-read per member

§3.2 said to read the word once at group entry, reasoning that bits raised
during the body belong to the next activation, as the held sched flag already
implies. That is wrong, and it is wrong in the direction that loses executions.

A comb group exists to run a component in producer-before-consumer order. When
an earlier member stores to a group-internal signal, the scan finds the group
already queued and takes the arm that ORs the downstream member's bit in — and
that bit is what tells the consumer to run **in this same pass**. Snapshotting
at entry sees only the bits the group was woken with, so every consumer fed from
inside the group is stranded at its previous value. comb-fusion.md's aside that
"the downstream member runs later in the same activation with final values" is
load-bearing once guards exist, not an aside.

Caught by four existing tests (the comb-fusion observer pin, a net-resolution
case, a `real` continuous-assign case, and an `llvm_backend` case) — all of
which are chains where a consumer sits downstream of a producer in its own
group. Fixed by re-reading the word per member; `tests/region_test.cpp`'s first
case is the dedicated pin.

This is also why the already-queued arm of `emit_static_scan_rt` must OR the
bits rather than skip: it is not a rare second-wake path, it is the normal
intra-group edge.

### 9.3 Colour exhaustion shares bits instead of giving up

The first cut left a group unguarded when its event count exceeded the 63
colours. That put a cliff in the middle of the win: a 62-wide fan-out component
got guards and a 64-wide one got none (measured 1.94x and 1.00x). Exhaustion now
rotates through the colour space, so `n` events over 63 bits means about `n/63`
members per bit — precision degrades with width instead of vanishing. The same
64-wide component went to 2.02x, and 256-wide to 3.32x.

### 9.4 Measured (2026-08-23, Release, LLVM 22, min of 3)

Retired instructions per simulated clock period, gate on vs off, same tree:

| case | off | on | |
|---|---|---|---|
| fan-out component, 16 wide, one hot branch | 1132 | 880 | **1.29x** |
| same, 64 wide | 2195 | 1087 | **2.02x** |
| same, 256 wide | 6407 | 1932 | **3.32x** |
| picorv32, 1M-cycle TIMEOUT | 20166 | 20262 | **+0.48%** |

picorv32 pays and does not collect: its 17 comb groups hold 62 members, none of
whose masks is vacuous, but the members of a group there mostly fire together,
so the guards are 62 tests that almost always pass. +0.48% is inside the perf
harness's own margin (`max(2%, 2x spread)`; measured spread on the dev box is
4%), and it buys the fan-out column. If a real design ever regresses beyond the
margin, the honest lever is the interior guard becoming a cost model (§4), not a
profitability threshold on the guards themselves.

Verified: byte-identical gate on vs off across `examples/`, the region and
comb-fusion pins, and picorv32 at 1M cycles; full suite 2350 cases green;
`tests/region_test.cpp` asserts every case across default / `regions` off /
`thunk` off / both off / `fuse` off, oracle-verified against iverilog 13.

### 9.5 What P3 inherits

- The guard machinery is member-granular already, so admitting an
  edge-sensitive member is a change to the `compute_fusion_groups` candidate
  filter plus whatever the cost model says about the widened union sensitivity.
  No new wake-word work.
- The glitch pin belongs to P3, not here. A signal that returns to its starting
  value inside one instant is unobservable for a pure comb member — which is
  part of why guarding one is safe — but a posedge member must fire for an edge
  the schedule classified even though the level is back where it started. That
  is the case that rules out ever deriving triggers from a region-entry sample,
  and `tests/region_test.cpp` says so where the pin will go.
- `trig_collisions_` counts within-group colour collisions and is not reported
  anywhere yet. If P3's wider unions make collisions common, that counter is
  the thing to put behind `--stat`.
