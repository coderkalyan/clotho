# Clock-wave dispatch (Direction C)

Static, deterministic dispatch of a clock domain's processes, bypassing the
per-edge wait-bucket scan and the per-process active-queue round-trip. A
stepping stone toward a verilator-style static schedule for the synchronous
subset (Direction B), retaining the event-driven engine everywhere else.

Status: design only. Read the honest-accounting section before investing —
static fusion already captured most of what C would, so C's *standalone* win is
modest and design-dependent; its real role is the on-ramp to B.

---

## 1. Current dispatch (what C replaces)

Everything runs in one JIT'd function, `sim_drain` (`emit_active_run`). A
blocking store to a signal `s` with subscribers emits `service_wait_queue(s)`,
which computes edge facts (pos/neg/any) and, per matching edge, scans the static
bucket `clotho.wait.<s>.<edge>` (`emit_static_scan_rt`): for each `{handle,
flag}` entry with `flag == 0`, push `handle` to the active FIFO and set
`flag = 1`. The drain pops each handle and dispatches (thunk call / coro resume);
each process clears its own flag at body end (re-arm).

So a clock edge waking G subscribers costs: one G-entry bucket scan, up to G
ring pushes + G flag sets, then G pops + G dispatches. The flag provides
**per-process dedup** — a process woken by two of its signals in the same
instant is enqueued once.

Static fusion (`compute_fusion_groups`) already merged every suspension-free
process with an *identical* canonical sensitivity into one thunk, so G is the
count of **distinct-sensitivity groups** on the edge, not the raw process count.
On picorv32 that is ~50 groups on the clock nets (was hundreds of blocks).

---

## 2. Honest accounting — where the scheduling cost actually is

Post-thunk, post-fusion, post-`rt_wheel_advance`, per clock edge the residual
scheduling cost is the G-group scan + queue traffic above. On picorv32 (~50
groups, 1M edges) that is ~50 scheduling ops/cycle against a core doing
thousands of instructions/cycle — **order ~1%**. Two consequences:

- **C cannot beat fusion where fusion already applies.** A single-clock domain
  of identical-sensitivity flops fuses to *one* group; its bucket has one entry;
  the scan/queue is already trivial. C only has slack to remove when a domain
  carries **many distinct-sensitivity groups** (async-reset variants, mixed
  enables, fusion-cap overflow at >256 members) or in the distinct-sensitivity
  **comb-fanout** case (§5).
- **The dominant remaining sim cost is body execution, not scheduling** — the
  4-state X-guards and `-O0`/FastISel codegen. 2-state lowering and the `-O`
  tier attack that and are almost certainly higher value than C in isolation. C
  is worth building as **B's first phase**, not as a standalone perf play.

Measure the scheduling share on the target design (perf, split `sim_drain` self
vs body self) before committing.

---

## 3. Design: the clock-domain wave

### 3.1 Domain and eligibility

A **clock domain** `D(c, e)` is the set of processes triggered by edge `e` of
clock net `c`. A group is **wave-eligible** in `D(c, e)` iff:

- it is a thunk (suspension-free — `is_simple_always`; no coroutine/`#delay`/
  dynamic wait in the domain, else the wave cannot be a straight call list), and
- it is **edge-triggered** (fires at most once per instant — the property that
  makes dedup cheap; level-sensitive comb is Phase 2, §5), and
- for v1, its sensitivity is **exactly `{(c, e)}`** (single-source) OR the design
  is proven single-clock so no other wake source coincides (see dedup, §3.4).

Ineligible groups (async-reset multi-edge, delayed, level) stay on the current
scan+queue path; the domain runs hybrid.

### 3.2 The wave function

For each eligible domain, emit one synthetic thunk:

```
void wave_<c>_<e>(state, wheel):
    call group_0(state, wheel)   // members in sub_order_ (topo) order
    call group_1(state, wheel)
    ...
    ret
```

Members are the domain's fused groups, called in the existing scheduled-rank
order (`sub_order_` / the bucket's subscription order — already producer-before-
consumer). This is **fusion at the dispatch layer**: fusion merges identical-
sensitivity *bodies* into one function; the wave *calls* the distinct groups
back-to-back from one function. It reuses every group thunk unchanged.

### 3.3 Deferred dispatch via a single token (timing correctness)

The wave must **not** run inline at the clock write. In the event-driven model a
blocking store's woken processes are *queued* — they run after the writer
finishes its remaining statements. Running the wave inline at `c`'s store site
would run the flops before the writer's later statements, diverging if a flop
reads a signal the writer sets after the clock edge.

Fix: keep the deferral, but enqueue **one wave token** instead of G handles. The
wave function has the thunk signature `void(state, wheel)`, so its tagged pointer
rides the existing active FIFO and the existing tag-dispatch calls it — no new
dispatch machinery. `service_wait_queue(c)` for an eligible domain emits, per
matching edge, a single `push wave_token` (guarded by the domain's own flag)
instead of the G-entry scan. The drain pops the token, calls `wave_<c>_<e>`,
which runs the groups directly. Members no longer subscribe individually; the
domain's bucket holds only the wave.

Net per edge: **1 push + 1 pop + 1 flag**, then G direct calls — vs G pushes +
G pops + G flags + a G-entry scan. NBA staging inside the members, the NBA
commit, and any comb readers the members wake (pushed to the FIFO by their own
blocking writes, drained after the wave) are all unchanged and correctly ordered
— the wave members all run before anything they wake, exactly as the FIFO would
have ordered them.

### 3.4 Dedup correctness

The FIFO's per-process granularity is what dedups a process reachable from two
signals in one instant. Collapsing to a wave token loses that granularity, so
the wave is only trivially correct when its members **cannot be woken by anything
else in the same instant**:

- **Single-clock, single-source domain:** clean, no flag needed. Each member is
  edge-triggered on `(c, e)` only; one edge → one run.
- **Async-reset / multi-source overlap** (`@(posedge clk or posedge rst)`): a
  flop is in two domains. If both edges fire in the same instant, two wave tokens
  would run it twice, where the FIFO+flag runs it once. Options, in order of
  preference: (a) keep such groups on the scan+queue path (hybrid, v1 default);
  (b) set the member flag at *edge* time and have the wave skip already-flagged
  members, clearing at instant-end (works because edge-triggered members do not
  re-fire within an instant) — restores exact dedup at the cost of a per-member
  flag pass; (c) accept the double-evaluation as a documented deviation *only*
  if shown harmless (NBA idempotent) and gated — not recommended.

v1 takes (a): wave the single-source majority, leave async-reset flops on the
queue. Extend to (b) once measured to matter.

---

## 4. Interaction with existing passes

- `compute_subscription_order` (Tarjan SCC + longest-path levels) supplies the
  member order directly — the wave is emitted in `sub_order_`.
- `compute_fusion_groups` is unchanged and runs first; the wave's members are its
  output groups. C is strictly downstream of fusion.
- The tag-dispatch and active FIFO are reused as-is (the wave is a tagged thunk).
- A `wave` field in `SimulationConfig` gates it for differential tests, mirroring `fuse` / `thunk`.

New compile-time work: group eligible domains by `(c, e)`, emit one wave thunk
each, and rewrite the eligible buckets' service sites to push the token. Modest,
well-contained; no runtime/ABI change.

---

## 5. Phase 2 — comb settle in the schedule (toward B / verilator)

The larger prize is folding the **combinational settle** into the same static
schedule: after the clocked wave stages NBA and the NBA commits, run the level-
sensitive comb cone in one topo pass instead of bucket-scan waves. That closes
most of the remaining event-vs-cycle gap, but takes on the hard parts C avoids:

- **Re-fire within an instant.** Comb is level-sensitive and legally re-evaluates
  when an input changes again in a later settle wave, so the "one run per instant"
  dedup of §3.4 does not hold — the schedule must iterate the cone to a fixpoint,
  and only acyclic cones settle in one pass.
- **Observer safety.** A single topo pass yields final values but skips
  intermediate glitch states. Clotho is event-accurate: if anything observes a
  comb signal mid-settle (`$strobe`/`$monitor` are postponed and safe, but a
  `$display` in a comb block, or a cross-cone read the event order would expose),
  the collapsed pass diverges. Phase 2 needs a "no intermediate observer"
  analysis per cone, with fallback to the queue when it fails.
- **Combinational loops** need bounded fixpoint iteration inside the schedule.

This is where B's effort and risk live; §3 (clocked-only) is deliberately the
observation-safe subset (NBA deferral means no intermediate state to observe).

---

## 6. Endgame

A hybrid engine: a verilator-style static schedule for the statically-
analyzable, observation-safe synchronous+comb subset (built C → B), with the
event-driven FIFO + wheel retained for delays, dynamic sensitivity, mid-settle
observers, and non-synthesizable constructs. Target ~3–5× of verilator on
synchronous RTL while staying fully event-accurate and LRM-compliant elsewhere.

## 7. Recommendation

1. **Profile first.** Split `sim_drain` self-time (scan + queue) from body
   self-time on a representative design. If scheduling is ~1% (likely, given
   fusion), C standalone is not worth it ahead of 2-state / `-O`.
2. If scheduling is material (many distinct clocked sensitivities, or the
   distinct-sensitivity comb-fanout case), build **§3 v1** (single-source clocked
   waves, async-reset on the queue) behind `--no-wave`; validate byte-identical
   vs `--no-wave` on the corpus + picorv32; measure `regs`/picorv32.
3. Treat C as **B's phase 1**. The comb-settle extension (§5) is the real
   scheduling ceiling and the point where this competes with verilator; gate its
   observer-safety analysis carefully.
