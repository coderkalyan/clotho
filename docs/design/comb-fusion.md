# Comb-process fusion (static scheduling for combinational logic)

Target: collapse per-wave scheduler round-trips on combinational cascades
(`combchain`: 512-stage assign chain, ~70 ns per activation) by fusing
dependent comb processes into one thunk that runs bodies in topological order
— a conservative, best-effort version of Verilator's static scheduling on top
of V1 identical-sensitivity fusion and topo subscription order.

Status: phase 0 (eligibility analysis + coverage probe + semantic pins) and
phase 1 (grouping + lowering, `--no-comb-fuse` to disable) landed. Measured
(dev box, 2026-07-08, `--time` sim split, min of 5): combchain 552 -> 203 ms
(2.7x) with llvm compile 412 -> 242 ms; picorv32 within noise both ways
(VERBOSE trace byte-identical); thunk_comb untouched by construction
(independent cones stay singleton).

## Why this is LRM-legal (the whole derisk argument)

A comb-fused group subscribes to the union of member sensitivities and runs
every member, producer-before-consumer, per activation. Two hazards:

**Spurious execution.** A member whose own trigger did not fire still runs.
LRM 9.4.2 says it must not. The run is unobservable — as-if — when:

- the body is a **pure deterministic function of its signal reads** (no
  $display/file I/O/task/$random/$time/string heap ops; whitelisted IR tags
  only), and
- its **read set ⊆ sensitivity** (else the spurious run samples a signal the
  unfused schedule never samples at that point — e.g. `always @(a) c = b;`),
  and
- every write is **change-detected**: `service_wait_queue` and the NBA
  service guard the wake scan on `old != new`
  (`emit_edge_facts`/`service_wait_queue`, LlvmBackend.cpp). Unchanged reads
  ⇒ recomputed values identical to the member's last run ⇒ no-change stores
  ⇒ no wakes ⇒ nothing downstream can tell.

Determinism also covers termination: the member's reads are unchanged from
its last (terminating) run, so a pure body cannot newly diverge or trap.
Read-modify-write part-selects are safe by the same argument — the rewritten
bits carry the same values, so the whole word compares equal.

Change detection for **string and unpacked-array stores is not verified**, so
those writes are excluded (`UnsupportedWrite`) until audited.

**Collapsed delta waves / suppressed glitches.** Unfused, unequal-depth
reconvergence can evaluate a join twice per instant (glitch); fused topo
order evaluates it once with final inputs. LRM 4.7 leaves same-instant
active-event ordering to the simulator ("events being executed and removed
from the active event region set in any order"), and deferring the join's
first evaluation past both updates is a reachable legal schedule — so the
single-evaluation outcome is one of the allowed nondeterministic behaviors,
not a deviation (the LRM does not require one legal schedule over another).
External observers of internal signals keep
their own subscriptions; internal stores service them normally, so they wake
exactly once per real value change either way (pinned by test).

**Excluded populations** (stay on the event-driven path, semantics
untouched): SCC members (comb loops keep re-wake fixpoint iteration),
edge-sensitive processes (V1 fusion territory), impure bodies, narrow
explicit sensitivities, NBA-writing bodies, function calls (purity closure
not implemented), string/array writers. Multi-driver nets cannot appear in a
group: clotho rejects them loudly at lowering.

## Phase 0: what landed

- `comb_fuse_verdict` (LlvmBackend.cpp, next to `is_simple_always`):
  per-process eligibility with reason codes. Purity is a **whitelist** over
  IR tags, so any future tag defaults to ineligible. Read set is collected
  from `Load` roots and verified against the sensitivity list mechanically —
  the frontend's inference is not trusted (explicit lists and inference gaps
  fail the check on their own).
- Coverage probe under `--stat=comb-fuse` (verdict histogram + how many
  eligible processes V1 left as singletons — the phase-1 opportunity).
- `tests/comb_fusion_test.cpp`: five semantic pins, each oracle-verified
  byte-identical against iverilog 13 (2026-07-08) and asserted across
  default / no-thunk / no-fuse lowerings: diamond settle values, external
  observer wake-per-real-change, $display activation count, stale narrow
  sensitivity, comb-loop fixpoint.

Probe results (2026-07-08):

| design | eligible / always procs | unfused singletons | excluded |
|---|---|---|---|
| combchain n=512 | 1537/1539 | 1537 | 1 delay-loop, 1 edge |
| thunk_comb n=256 | 256/259 | 256 | 1 delay-loop, 2 edge |
| picorv32 + tb | 226/272 (83%) | 180 | 23 not-simple, 23 edge |

Readings: combchain's opportunity is 3x the naive stage count (each stage =
assign + two port copies). thunk_comb's 256 eligibles are **independent** —
grouping must be by dependency component, or union sensitivity would run all
256 bodies per single-signal trigger. 83% on picorv32 confirms the predicate
is not overfit to the target benchmark.

## Phase 1: grouping + lowering (landed)

- Group = connected component of the writer→reader wake graph restricted to
  eligible singleton processes (`wake_adj_` retained from
  `compute_subscription_order`, union-find, equal `self_start` **and equal
  `reactive_home`** required); components > 256 chunk into rank-contiguous
  segments (a leftover chunk of one stays a singleton).
- Home-homogeneous because a group is one function pushed onto one ring: a
  program's continuous assign is reactive-home (LRM 24.3.1,
  docs/design/program-blocks.md §3) and fusing it with an Active-home process
  would run half the group in the wrong region. The V1 identical-sensitivity
  bucket key carries `reactive_home` for the same reason. A reactive-home group
  is also kept off the thunk path: the thunk tag is pointer bit 0 and the
  reactive-home tag is bit 1, and the scheduler's reactive test requires bit 0
  clear, so a pointer carrying both reads as neither.
- Members ordered by existing `sub_order_` rank (DAG by construction —
  nontrivial SCCs excluded via `scc_nontrivial_`).
- **Interior guard**: only candidates whose entire sensitivity is
  candidate-written may merge; the rest stay singletons (still contributing
  component edges and wakes). Without it, a member with a private external
  trigger drags the whole group awake on every private change: the `star`
  generator (one shared candidate-produced signal + 256 consumers with hot
  private inputs) measured **10.9x slower than unfused** (16 -> 175 ms).
  With the guard the consumers demote and star matches unfused timing.
  Interior members wake only via changes propagating through the component
  — exactly the wave the fused topo order replaces.
- Subscription = canonical union of member sensitivities, NOT narrowed:
  the sched flag is held across the fused body (same discipline as V1 /
  thunks), so an intra-group store finds the flag set and cannot re-enqueue
  the group — zero redundant activations without a writer census. The
  dropped re-wake is exactly the work the topo order already did (the
  downstream member runs later in the same activation with final values).
- Lowers via the existing thunk path (or the coroutine group path under
  `--no-thunk` — same flag timeline, both verified).
- Accepted cost: a stimulus touching one branch of a wide component re-runs
  the untouched branch (pure, no-change, wasted compute). picorv32 measured
  at noise; per-member dirty bits only if a real design regresses.
- **Known residual degradation shape**: the interior guard is depth-1. A
  star whose private inputs are laundered through one candidate layer
  (`assign y_i = x_i; assign c_i = s ^ y_i;`) makes the consumers interior
  again and re-enables the over-evaluation (bounded by the 256 cap). The
  honest fix is per-member dirty bits (phase 2); until then
  `SimulationConfig::comb_fuse` is the differential gate and `--stat=comb-fuse` shows
  the formed groups.
- Both of the above are **fixed** by regions.md P2 (landed): each member is
  guarded on its own sensitivity, tested against the trigger bits the wake site
  ORs into the process's wake word, so a stimulus touching one branch of a
  component no longer re-runs the rest. That is the per-member dirty bit this
  entry asked for, and it makes the interior guard a cost model rather than a
  hard reject whenever that is worth measuring. Measured on a laundered star:
  1.29x at 16 consumers, 2.02x at 64, 3.32x at 256, output byte-identical.
  The same mechanism retires five of the eight `CombFuse` verdicts as fusion
  blockers (`EdgeSensitive`, `ImpureOp`, `HasCall`, `UnsupportedWrite`,
  `ReadOutsideSens`), which exist only because a fused member may run when its
  own trigger did not; regions.md P3 is where that gets taken up.

Verified: fused vs `--no-comb-fuse` byte-identical on combchain (20k
cycles), picorv32 default and VERBOSE traces, tri networks across 4
declaration orders, regs/chain/bigcase/onehot/wheel/timers generators, both
thunk and coroutine lowerings; phase-0 iverilog pins pass under the fused
default config.
