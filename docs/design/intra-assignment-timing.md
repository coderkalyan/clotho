# Intra-assignment timing (LRM 9.4.5)

`lhs <op> <timing> rhs`, where `<op>` is `=` or `<=` and `<timing>` is a delay
(`#d`), an event (`@e`), or a repeat-event (`repeat(N) @e`). The RHS is sampled
in the active region at the instant the statement executes; what differs is what
happens to the *process* and *when the store lands*.

## Class split

The gap splits by `<op>`, not by timing kind.

- **Class A — blocking (`=`)**: LANDED. A pure statement reorder — sample RHS
  now (SSA value), suspend on the existing `Delay`/`Wait` op, store the sampled
  value after. The sampled RHS lives across the suspend; CoroSplit spills it into
  the process frame. `lower_intra_timing` in `src/Lower.cpp`;
  `tests/intra_assign_test.cpp`.
- **Class B — nonblocking (`<=`)**: this document. The process does **not**
  block; the update is scheduled to land at `t + d` (or at the event) as a
  nonblocking update, and the statement returns immediately.

This document designs Class B, phased **B1 (`<= #d`)** then **B2 (`<= @e` /
`<= repeat(N) @e`)**. B1 is the common RTL idiom (`q <= #1 d` pipelines) and is
specified in full; B2 reuses B1's record machinery over the wait-queue instead
of the wheel and is sketched at the end.

**Status: B1 landed** (`StoreNbDelay` IR op, `rt_intra_*` runtime,
`emit_intra_nba_arm` / `emit_intra_commit` in `LlvmBackend.cpp`;
`tests/intra_assign_test.cpp`). Two facts learned in implementation, folded in
below: same-instant updates must commit in statement-execution order (a monotonic
`seq` stamp on each record — the wheel's per-tick fire order does not preserve it,
LRM 10.4.2), and a `#0` delay cannot arm the wheel (`deadline` must be `> now`) so
it pushes straight onto the ready list to commit in the current instant's NBA
region. B2 has since landed, as have bit/part-select, unpacked-array element, and
packed/unpacked struct field targets; the remaining loud-failing follow-ups are
listed under "IR and frontend" below.

Ordering refinement: a **compile-time** `#0` is const-folded in the frontend and
lowered as a plain `<=` (StoreNb), so it shares the one block-commit path and its
updates to a signal order with that signal's other NBAs by statement-execution
order (LRM 10.4.2). Only a genuinely **runtime** zero delay (`#(expr)` that
evaluates to 0) reaches the deferred path's arm-site zero branch; it commits with
the correct value and timing (current-instant NBA region), but because the
deferred commit runs after block-commit in `ncommit`, its order *relative to a
plain NBA to the same signal in the same instant* is fixed rather than
execution-order — a documented edge, now reachable for an array element as well
(`mem[i] <= a;` then `mem[i] <= #0 b;` with a runtime-computed zero delay).

## Semantics (the two facts that shape the design)

For `a <= #5 b;` executed at time `t`:

1. **Sample now, in the active region.** `b` (and any LHS index) is read at `t`.
   The process continues to the next statement with no suspend.
2. **Commit in the NBA region of `t+5`.** The update to `a` is a nonblocking
   update event scheduled for `t+5`; at `t+5` it lands in the NBA (nonblocking
   assignment update) region — *after* that instant's active region, exactly
   like a same-instant `<=`.

Two consequences the design must honour:

- **N outstanding at once.** A pipeline firing `a <= #5 b` every cycle leaves up
  to 5 updates in flight (more with a larger delay, unbounded with a dynamic
  one). Each carries its own sampled value → a queue of `(value, fire-time)`
  records, not a single shadow slot.
- **NBA-region commit is not optional.** Committing at wheel-fire time (the start
  of `t+5`, before the active region) is observably wrong. Counter-example:

  ```systemverilog
  always @(posedge clk) x = a;          // clk posedge at t+5
  initial begin a = 1; a <= #5 0; end   // a-update scheduled for t+5
  ```

  LRM ordering at `t+5`: active region runs `x = a` reading `a == 1` (old),
  *then* the NBA region applies `a <= 0`. So `x == 1`. If the deferred store
  committed `a = 0` when its wheel timer fired (the first thing at `t+5`, before
  the active region), `x = a` would read `0`. iverilog gives `1`; we must too.
  This is deterministic region ordering (§4.4.2), not a nondeterministic corner.

## Architecture: reuse the wheel and the NBA region, add a record subsystem

The whole point is that Clotho already has every mechanism this needs; Class B is
plumbing, not a new engine.

- **The wheel already schedules and fires at `t+d`.** A `#delay` arms an
  intrusive `rt_timer` embedded in an awaiter and fires it during
  `rt_wheel_advance`; the fire callback pushes the awaiter onto the active queue
  (`clotho.enqueue_timer`, `LlvmBackend.cpp`). A deferred store reuses the same
  wheel-timer mechanism with a different fire callback.
- **The drain already runs the NBA region after every fired instant.** The drain
  loop is `active → inactive(#0) → nba → wheel_advance`, and after
  `wheel_advance` fires timers it branches back to `active.check`, so an instant
  brought to life purely by a fired timer still reaches `nba.check`
  (`emit_active_run`, `LlvmBackend.cpp:4272`). If the fire callback *stages* the
  update into the NBA structures rather than committing it, the existing
  `nba.check`/`nba.commit` commits it — in the NBA region, after active settles.
  This is exactly the ordering §4.4.2 requires, for free.
- **`emit_nba_service` already does change-detect + edge-aware waking.** The
  block-commit path uses it (`LlvmBackend.cpp:3473`); the deferred commit reuses
  it, so waking `a`'s `@(posedge a)` / level readers needs no new logic.

What is genuinely new: a per-target-signal **deferred-record subsystem** that
holds the sampled value from `t` until the timer fires at `t+d`, then hands it to
the NBA region. It is the `#delay`-triggered analogue of the existing
same-instant array-NBA record buffer (`rt_nba_arr_*` / `emit_array_commit`,
`docs/design/static-arr-nba.md`), and is deliberately modelled on it.

## Record lifecycle (zero steady-state allocation)

One record travels a four-state cycle; each transition is O(1) and allocates only
when a per-signal freelist must grow:

```
   freelist ──alloc──▶ (fill value+dst, arm wheel) ──▶ scheduled (linked in wheel slot)
      ▲                                                      │
      │                                                   fire @ t+d
      │                                                      ▼
      └──────── commit (store+wake, NBA region) ◀──────── ready (per-signal list)
```

- **freelist → scheduled** (at `t`, statement execution): pull a free record for
  the target signal's dense `idx`, fill the captured value planes + the resolved
  live destination pointer, then `rt_wheel_schedule(&rec->timer, t+d, rt_intra_cb)`.
  The `rt_timer` node is embedded in the record, so arming is a link, not an
  allocation.
- **scheduled → ready** (at `t+d`, `rt_wheel_advance` fires): `rt_intra_cb`
  recovers the record from the timer via `offsetof`, links it onto its signal's
  **ready list**, and bumps a global pending counter. It does **not** touch design
  state — commit is deferred to the NBA region.
- **ready → committed → freelist** (at `t+d`, `nba.commit`): the drain sees the
  pending counter nonzero at `nba.check`, and for each intra-NBA signal runs a
  monomorphized `emit_intra_commit(sig)` that walks the ready list, stores each
  record's planes to its live pointer, wakes readers via `emit_nba_service`, and
  returns the record to the freelist.

Steady state allocates nothing: the freelist high-water mark equals the peak
outstanding count, reached once and then recycled — the same discipline that
makes `rt_nba_arr_*` allocation-free after warm-up. The ready-list link reuses
the record's `timer.next` field (a timer not linked in the wheel is free to
reuse), so the ready list costs no extra pointer.

## Runtime ABI (`rt_intra_*`, mirroring `rt_nba_arr_*`)

The record layout is backend-defined and monomorphized on the target's
width / z-ness (the runtime never interprets the value bytes); the runtime owns
the freelist, the per-signal ready lists, and the pending counter.

```c
// Runtime-owned record header the backend places at offset 0 of every record.
// timer: wheel node (offset 0, so the record base is the rt_timer*). idx: target
// signal's dense intra-NBA id. seq: monotonic arm-order stamp (set by
// rt_intra_alloc) so same-instant commits apply last-executed-wins (LRM 10.4.2),
// which the wheel's per-tick fire order does not preserve. The backend's value
// tail (mm [+ z]) follows the header.
typedef struct rt_intra_rec { rt_timer timer; uint32_t idx; uint32_t seq; } rt_intra_rec;

// Pull a free record for site `idx` (stride = sizeof the monomorphized record).
// From the idx's freelist; grows geometrically, so steady state never allocates.
void* rt_intra_alloc(uint32_t idx, uint32_t stride);

// Wheel fire callback: recover the record, link onto idx's ready list, bump the
// global pending counter. Shared by every intra-NBA site.
void rt_intra_cb(rt_timer* timer, void* ctx);

// Ready-list head + count for site `idx`, read once in the NBA region to drive
// the backend-emitted commit loop (mirrors rt_nba_arr_base / _count).
void*    rt_intra_ready_head(uint32_t idx);
uint32_t rt_intra_ready_count(uint32_t idx);

// Total ready records across all sites; nba.check tests this to decide whether a
// deferred commit is needed (mirrors rt_nba_arr_any).
uint32_t rt_intra_any(void);

// Return site `idx`'s committed records to its freelist and clear its ready list.
// Called by the backend after emit_intra_commit(sig) (mirrors the per-pass reset).
void rt_intra_release(uint32_t idx);
```

The record is armed on the wheel with `rt_intra_cb` as `on_expire`, so no new
wheel code is needed — the wheel already invokes `on_expire(timer, ctx)` on fire.

## Backend codegen

Two additions, both modelled on existing code:

1. **`emit_intra_commit(Signal sig)`** — a near-copy of `emit_array_commit`: loop
   `rt_intra_ready_count(idx)` records from `rt_intra_ready_base(idx)` and apply
   each one, then `rt_intra_release(idx)`. Where the destination comes from depends
   on the target shape, and so does the wake:
   - **whole scalar, or a bit/part-select of one** (a packed struct field included):
     the address is a link-time constant, so `storage_ptr(s)` is hoisted out of the
     loop; a select merges through the record's mask. Wakes with **edge semantics**
     via `emit_nba_service` (change-detect + posedge/negedge), comparing the value
     sampled before the loop with the one after it.
   - **unpacked-array element**: the destination pointer is loaded from the record
     inside the loop. Wakes with the array path's unconditional Any-edge
     `service_any_waiters` and touches the whole element range for VCD — an array
     signal is deliberately never an `nba` target, so it has no `nba_id` and the
     `nba_meta` table must not be indexed by it (`docs/design/static-arr-nba.md`).
   - **aggregate leaf** (`emit_intra_agg_commit`): the record carries the leaf slot
     pointer plus a byte image, replayed by `memcpy` (or a byte-wise masked
     read-modify-write for a partial leaf write) exactly as
     `emit_agg_array_commit` does. Wakes level-only: an aggregate has no flat value
     to change-detect.

2. **`nba.check` / `nba.commit` integration** (`emit_active_run`): extend the
   `has_nba` predicate and the `any` test at `ncheck` with
   `rt_intra_any() != 0`, and add the per-signal `emit_intra_commit` calls to the
   `ncommit` block next to the existing block-commit and array-commit calls. No
   new region — the deferred commit rides the region that already exists.

The arm site (at statement execution) is emitted where the IR op lowers:
`rt_intra_alloc(idx, stride)` → `fill_intra_payload` → `rt_wheel_schedule`. The
payload is the plain-NBA lvalue encoding for the shape: the value **positioned**
at the target's bit offset, a write mask when the target is partial, and — for an
array element or aggregate leaf only — the live destination pointer the select
resolved to. A whole scalar or a select of one carries **no pointer**; its
destination is re-derived at commit from `storage_ptr`. (Earlier revisions of this
document claimed every record stored "the resolved live destination pointer";
that was never true, and is now true only for the two pointer-carrying shapes.)
An out-of-range variable index arms nothing at all, so no record can hold a
pointer past the end of its array (LRM 7.4.5 / 11.5.1).

## IR and frontend (`src/Ir.h`, `src/Lower.cpp`)

- **IR**: a new op `IntraNbDelay(dst, value, delay_ticks)` — `dst` resolves to a
  whole target `Signal`, `value` is the sampled RHS, `delay_ticks` the scaled
  delay. (A three-operand op; carry `delay_ticks` in an `Extra` slot like `Wait`
  carries its edge list, or as the second operand with `dst` in the payload —
  match whichever encoding `Ir.h` makes cheapest.)
- **`lower_assign`**: on `a.isNonBlocking() && timingControl != nullptr`, sample the
  RHS and emit `StoreNbDelay` / `StoreNbEvent` against the ordinary lowered lvalue.
  The RHS is sampled at `t` in the active region (no suspend), so no CoroSplit
  interaction — unlike Class A. `intra_nb_target_supported` admits exactly the
  shapes the deferred record can encode: a whole integral or real scalar, a
  bit/part-select of an integral base (constant or variable index), an
  unpacked-array element, and a packed or unpacked struct field.
- **Guards (fail loud)**: a concatenation and a whole unpacked
  struct / array target (the sampled RHS would have to be spread across one record
  per decomposed piece); a string, queue, dynamic-array or associative-array
  element (plain NBA has no lvalue encoding for these either, so there is nothing
  to reuse); a multi-signal trigger `@(a or b)` or an `iff` qualifier; and a real
  target on the event form. A runtime-offset select of a vector wider than
  `wide_dyn_select_bits` also fails loud in the backend: it addresses words through
  `rt_wide_ins` rather than a storage-width mask, which is the only encoding the
  record has room for.

## Efficiency

- **No new scheduler and no new region.** Reuses the wheel for timing and the
  existing NBA-region drain for commit; the marginal control-flow cost at
  `nba.check` is one added `rt_intra_any()` test, paid only when the design has
  intra-NBA sites.
- **Zero steady-state allocation.** Per-signal freelist with geometric growth;
  records recycle through the four-state cycle. Matches the `rt_nba_arr_*`
  guarantee.
- **O(1) per transition.** Arm = fill + one intrusive wheel link. Fire = one
  intrusive ready-list link + a counter bump. Commit = one store + a
  change-detected conditional wake, then a freelist push. Per-update cost is a
  normal NBA commit plus one wheel round-trip (inherent to the delay).
- **The ready-list link is free** (reuses `timer.next`); the record carries only
  its value planes, live pointer, and dense `idx` beyond the timer node.
- **Constant-delay common case** (`q <= #1 d`) needs no special path — the
  general arm is already a fill + a link. A future refinement could pool by
  `(idx, delay)` to skip the deadline computation, but the win is marginal.

## B2 — `<= @e` and `<= repeat(N) @e` (landed)

Same record subsystem and NBA-region commit as B1, triggered off a **signal
edge** instead of the wheel (`StoreNbEvent` IR op; `rt_intra_event_*`).

- **The record header gains a `count`** (event countdown) alongside `idx`/`seq`.
  `<= @e` sets count 1, `<= repeat(N) @e` sets count N (sampled now).
- **Per (trigger-signal, edge) `trig_id`** with a growable pending list — records
  between two edges are unbounded (a loop can arm many before one fires), so a
  growable list, not the fixed-capacity dynamic wait queue.
- **Arm** (`emit_intra_event_arm`): fill the record (value planes + count) and
  `rt_intra_event_register(trig_id, rec)`. No wheel, no suspend.
- **Fire** (`rt_intra_event_fire`): on the trigger edge, decrement each pending
  record; at zero, move it to its target's ready list (the same seq-sorted push as
  the wheel path), else keep it pending for the next edge. The NBA region then
  commits via the unchanged `emit_intra_commit`.

The one real difficulty was the **two wake paths**: a signal edge fires from a
blocking store (`service_wait_queue`) *and* an NBA commit (`emit_nba_service`) —
`@(posedge clk)` with `clk <= ~clk` is common — so the record-fire hook
(`fire_event_trigger`) sits in both, each already a per-`(sig, edge)` bucket loop
guarded by the edge fact. And a trigger signal that nothing else is sensitive to
still needs edge detection, so `signal_has_waiters` (and `has_bucket`) treat any
`trig_id >= 0` (sig, edge) as a waiter. Everything is gated on `trigger_id_`
being non-empty, so a design without `<= @e` gets identical codegen.

Deferred follow-ups: a **multi-signal trigger** `@(a or b)` (record on several
`trig_id` lists, first-fire-wins removal via a `done` flag), an `iff` qualifier on
the trigger, a real target on the event form, and the concat / whole-aggregate /
string / container-element targets listed under "IR and frontend" — all loud-fail.

## Test plan

Pinned against iverilog (a clean oracle here — 9.4.5 is fully specified, no LRM
ambiguity):

- Basic delayed pipeline: `q <= #2 d` sampled each cycle, N in flight; check the
  N-deep delay line.
- The region-ordering counter-example above (`x = a` at the same instant a
  deferred `a <= #d` fires) — the test that a wheel-fire-time commit would fail.
- Value sampled at `t`, not `t+d` (RHS operand changes during the delay).
- Multiple deferred updates to the same signal at the same fire instant
  (last-writer-wins, no tearing).
- Zero and dynamic delays; `<= #0` routes through the current-instant NBA path.
- Event / repeat: basic `<= @(posedge clk)`, `repeat(N)`, an NBA-driven trigger
  (`clk <= ~clk`), multiple arms between edges (unbounded pending), same-edge
  ordering.
- Guards fail loud: multi-signal trigger, element/part-select/string targets.
