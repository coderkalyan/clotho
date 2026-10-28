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
  the process frame. `lower_intra_timing` in `src/lower/Timing.cpp`;
  `tests/intra_assign_test.cpp`.
- **Class B — nonblocking (`<=`)**: this document. The process does **not**
  block; the update is scheduled to land at `t + d` (or at the event) as a
  nonblocking update, and the statement returns immediately.

This document designs Class B, phased **B1 (`<= #d`)** then **B2 (`<= @e` /
`<= repeat(N) @e`)**. B1 is the common RTL idiom (`q <= #1 d` pipelines) and is
specified in full; B2 reuses B1's record machinery over the wait-queue instead
of the wheel and is sketched at the end.

**Status: B1 landed** (`StoreNbDelay` IR op, `rt_intra_*` runtime,
`emit_intra_nba_arm` / `emit_intra_commit` in `ir/llvm/Stores.cpp`;
`tests/intra_assign_test.cpp`). Two facts learned in implementation, folded in
below: same-instant updates must commit in statement-execution order (a monotonic
`seq` stamp on each record — the wheel's per-tick fire order does not preserve it,
LRM 10.4.2), and a `#0` delay cannot arm the wheel (`deadline` must be `> now`) so
it pushes straight onto the ready list to commit in the current instant's NBA
region. B2 has since landed, as have bit/part-select, unpacked-array element, and
packed/unpacked struct field targets.

**Remainder phases R-a and R-b have also landed.** R-a: every decomposing target
(a concatenation, a streaming concatenation, an assignment-pattern lvalue, and a
whole unpacked struct or array) now arms one deferred record per leaf over one
sampled RHS and one evaluation of the delay / count, and a non-positive repeat
count assigns with no wait per 9.4.5. R-b: a nonblocking write through a virtual
interface handle commits into the instance the handle named, and a multi-signal
trigger `@(a or b)` puts the record on every listed trigger with real
first-fire-wins removal. R-c (`iff`) and R-d (the owned-handle record class, so
`s <= #5 t` and `q <= #2 r`) remain scoped and loud-failing, as do the blocking
form's own two gaps; all of them are in "The remainder" below.

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
  (`clotho.enqueue_timer`, `ir/llvm/Scheduler.cpp`). A deferred store reuses the same
  wheel-timer mechanism with a different fire callback.
- **The drain already runs the NBA region after every fired instant.** The drain
  loop is `active → inactive(#0) → nba → wheel_advance`, and after
  `wheel_advance` fires timers it branches back to `active.check`, so an instant
  brought to life purely by a fired timer still reaches `nba.check`
  (`emit_active_run`, `ir/llvm/Scheduler.cpp`). If the fire callback *stages* the
  update into the NBA structures rather than committing it, the existing
  `nba.check`/`nba.commit` commits it — in the NBA region, after active settles.
  This is exactly the ordering §4.4.2 requires, for free.
- **`emit_nba_service` already does change-detect + edge-aware waking.** The
  block-commit path uses it (`ir/llvm/Stores.cpp, emit_block_commit_body`); the deferred commit reuses
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

## IR and frontend (`src/Ir.h`, `src/lower/`)

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
- **Decomposing targets** (R-a): a concatenation, a streaming concatenation, an
  assignment-pattern lvalue and a whole unpacked struct / array are no longer
  refused. `lower_assign` lowers the timing once into an `IntraNbTiming` and threads
  it, as a nullable `const IntraNbTiming*` beside the existing store tag, through the
  same walkers plain NBA uses (`store_concat` / `store_stream_target` /
  `lower_pattern_assign` / `emit_agg_store` / `emit_agg_copy`); each leaf emits its
  own `StoreNbDelay` / `StoreNbEvent`. A plain pointer rather than a leaf-emitter
  callback, so every ordinary assignment pays a null test and not an indirect call.
- **Guards (fail loud, per PRINCIPLES.md)**: a **string** target and a whole
  **container** target (both need an owned staged handle the record has no field
  for, R-d) — `lower/Assign.cpp, emit_agg_store`; an `iff` qualifier
  (R-c); a **conditional RHS** over a whole unpacked aggregate,
  an **aggregate with a container member**, an **indexed
  array-slice** target and a **string / container operand of a streaming
  target**, all narrowed out of the old blanket whole-aggregate refusal;
  a write to a **force target**, including one reached as a concatenation operand;
  and the **blocking** form's assignment-pattern lvalue,
  whole unpacked aggregate and string / array target.
  A runtime-offset select of a vector wider than `wide_dyn_select_bits`
  also fails loud in the backend (`ir/llvm/Stores.cpp, fill_intra_payload`): it addresses words
  through `rt_wide_ins` rather than a storage-width mask, which is the only encoding
  the record has room for. A queue / dynamic-array / associative-array **element**
  is not in this list: LRM 6.21 forbids a nonblocking assignment to one outright,
  timed or not (see "The remainder"). Each remaining guard is scoped there.

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

Deferred follow-ups are scoped in full in the next section.

## The remainder (scoped, not implemented)

`intra_nb_target_supported` / `intra_nb_target_path` (`lower/Assign.cpp`) admit
exactly the shapes the deferred record can encode as *one positioned plane pair plus
an optional write mask*: a whole integral or real scalar, a bit/part-select of an
integral base (constant or variable index), a fixed unpacked-array element, and a
packed or unpacked struct field. Everything else is refused, by one of six
differently-worded guards (`lower/Assign.cpp, lower_assign`),
and this section takes each remaining shape and says what the
record is actually missing.

The organising fact: **every remaining shape already has a plain-NBA lowering in
clotho**, and in each case that lowering is either (a) a *decomposition* into
stores the intra path already encodes, or (b) a *different record class*. So the
remainder is four pieces of work, not a dozen: reuse the decompositions, add one
`SignalSlot` flag, add one record class, and extend the trigger side twice.

### First: three shapes come off the list, because the LRM forbids them

**LRM 6.21** ("Scope and lifetime"), not 9.4.5 or 10.4.2:

> Automatic variables and **elements of dynamically sized array variables** shall not
> be written with nonblocking, continuous, or procedural continuous assignments.

So `q[i] <= x`, `d[i] <= x` and `aa[k] <= x` are *illegal*, timed or not — a queue,
a dynamic array and an associative array are all dynamically sized arrays
(`Type::isDynamicallySizedArray()` in slang covers `QueueType`,
`DynamicArrayType` and `AssociativeArrayType`, `Type.cpp:351-359`). slang enforces it
directly (`diag::NonblockingDynamicAssign`, `SelectExpressions.cpp:104-111`) and, as
measured, gets there first for every shape tried: a bare `q[0] <= v`, a struct field's
queue, an array-of-structs field's queue, and a `ref int r[$]` formal all report
slang's own diagnostic. clotho's queue-element guard (`lower/Assign.cpp, lower_assign`) is therefore
defence in depth, not a live path — slang's test is keyed on the value's
`getSymbolReference()`, so a shape whose reference is null would skip it.

Two corrections follow, and they are the reason this subsection is first:

- The M2 remainder entry's list of "target shapes that still have no lvalue
  encoding to defer" must **shed** the queue / dynamic-array / associative-array
  element: there is nothing to implement, and listing an LRM prohibition as a gap
  invites someone to close it.
- clotho's queue-element guard is a **legality refusal, not a TODO** — corrected in
  passing: it now says so and cites 6.21, instead of promising "a handle-aware NBA
  staging path". The neighbouring string-NBA comment attributed the prohibition to
  10.4.2; the text is in 6.21, also corrected.

A **class property** (`obj.p <= #d x`) also stays off this list, but for a different
reason: it is legal (6.21 bars only *continuous* assignments to a non-static
property) and plain NBA does not lower it either
(`src/lower/`). It is the class-property NBA gap, tracked on its own; the
intra path has nothing to reuse until that lands, and gets it for free afterwards.

### R1 — the decomposing shapes: concat, streaming concat, pattern, whole aggregate — LANDED

Four refusals (assignment-pattern lvalue, whole unpacked struct/array, and the
`default: return false` in `intra_nb_target_path` for a concatenation and a
streaming concatenation) shared one answer: plain NBA already decomposes each into
*N* ordinary `StoreNb`s over shapes the deferred record encodes today.

| target | plain-NBA decomposition | site |
|---|---|---|
| `{a,b} <= x` | `store_concat` → one `StoreNb` per leaf, each slicing the one RHS sample | `lower/Assign.cpp, lower_assign` |
| `{>>{a,b}} <= x` | `store_stream_target` | `lower/Containers.cpp` |
| `'{a,b} <= x` | `lower_pattern_assign` | `lower/Assign.cpp` |
| `s <= t` (whole unpacked struct) | `emit_struct_store` → `emit_agg_store`, per field | `lower/Assign.cpp` |
| `mem <= other` (whole unpacked array) | `emit_agg_copy`, per element | `lower/Assign.cpp` |

The intra version of each is the same walk emitting `StoreNbDelay` / `StoreNbEvent`
per leaf instead of `StoreNb`. `store_concat` was already tag-parameterised, but a
tag is not enough here: the deferred ops carry an `Extra` payload.

**As built**, the walkers take a nullable `const IntraNbTiming*` *beside* the
`Inst::Tag`, not the leaf-emitter callback this section originally proposed. The
POD carries the already-lowered delay / count / trigger list; a null pointer is the
ordinary path and every non-intra caller passes it by default. The reason is cost:
these walkers lower every ordinary assignment in the design, so the feature has to
be one null test per leaf rather than an indirect call on all of them. One shared
`store_leaf` does the dispatch.

Three semantics the walk must get right, all of them already decided:

- **One RHS evaluation** (9.4.5: "the right-hand expression shall be evaluated
  before the delay"), sliced N ways — which is exactly what `store_concat` already
  does for plain NBA, and its single-evaluation comment at `lower/Assign.cpp, store_concat` is the
  same guarantee.
- **One delay / count evaluation**, shared by all N records. For the delay form
  that means N wheel timers armed on one deadline; they fire in wheel order but
  commit in `seq` order, which the seq-sorted ready push already guarantees. For the
  event form all N records register on the same `trig_id` with the same count, so
  they mature on the same edge.
- **Leaf order = plain-NBA order.** Consecutive `seq` stamps in leaf order make a
  self-overlapping target (`{a, a[0]} <= #5 x`) land exactly as the plain-NBA form
  does, so there is one answer to explain, not two.

Cost: N records for an N-leaf target, which is N times a plain NBA's per-leaf cost —
the same shape, and the same objection. `mem <= #5 other` for a 1024-element array
arms 1024 timers. That is *not* a reason to widen the record: the aggregate record's
byte payload is sized to `agg_max_leaf_bytes` (`ir/llvm/Stores.cpp`), and sizing
it to a whole object would put a 1024-element image in every record of that
signal's freelist. The right fix is the **single-memcpy deferred record with a
heap-side payload** already listed as a plain-NBA perf follow-up
(`src/lower/`); it serves both paths and belongs to neither
exclusively. Decompose first, correctly; share that optimisation later.

### R2 — one `SignalSlot` flag: the pointer-carrying scalar record — LANDED

`vif.sig <= #5 x` was refused for an exactly-stated reason: plain `vif.sig <= x` works because its record carries the resolved instance
(`arr_rec_ctx`), while `emit_intra_commit` resolves a non-array destination by
hoisting the link-time-constant `storage_ptr(s)` out of the record loop
(`ir/llvm/Stores.cpp, emit_intra_commit`) — so a deferred update through a handle would land in
the *representative* instance. Silently the wrong interface, hence the guard.

The mechanism to fix it already exists in the same function. When `s.is_array`, the
record carries `mm_ptr` / `z_ptr` and the commit loads them **per record**
(`intra_rec_mm_ptr` / `intra_rec_z_ptr`, `ir/llvm/Stores.cpp`). So: a per-`SignalSlot` flag — `intra_ptr`, set exactly the way
`intra_masked` is — that forces the pointer-carrying tail and the per-record load for
a *scalar* target too, with `fill_intra_payload` storing the pointer the vif context
resolved to at arm time.

This is not merely convenient; it is the semantics LRM 10.4.2 requires:

> If `variable_lvalue` requires an evaluation, such as an index expression, class
> handle, or virtual interface reference, it shall be evaluated at the same time as
> the expression on the right-hand side.

Capturing the resolved destination at arm time *is* "evaluated at the same time as
the RHS". The array-element path already obeys this clause; `intra_ptr` extends the
same obedience to the handle-resolved scalar.

**Correction, found in implementation.** The wake does NOT follow the array
precedent, and this section's original claim that it should ("level-only
`service_any_waiters`, since a vif member has no `nba_id`") is wrong. An array
element has no edge semantics — slang rejects `@(posedge mem[i])` — which is
exactly why `service_any_waiters` serves only the Any bucket. A vif member is a
scalar and `@(posedge intf.sig)` is legal, so a level-only wake would never wake
its edge readers. It follows the PLAIN-NBA vif record instead: the record also
carries the resolved instance context, and the commit wakes **per record** under a
`CtxOverride` built from it (`service_wait_queue` + `vcd_touch_signal`), because
two records of one signal may have landed in different instances. `collect_vif_nba`
also had to learn to scan `StoreNbDelay` / `StoreNbEvent`, so an intra-only write
marks the member.

### R3 — one new record class: the owned handle (string, whole container)

Two refusals share a shape: `s <= #5 t` for a `string`
(`intra_nb_target_supported`'s first test rejects any non-integral, non-floating
type) and `q <= #2 r` for a whole queue / dynamic array / associative array (no
deferred analogue of `HandleCopyNb` exists).

Plain NBA handles both by **staging an owned deep copy and moving it in the NBA
region**: `emit_string_whole_commit` (`ir/llvm/Stores.cpp`) for a string
place, `HandleCopyNb` → `emit_handle_whole_commit` for a container.
Both stage through the shared `rt_nba_arr_*` buffer pool, keyed per signal — which is
sufficient for a plain NBA, because a signal can have only one *pending* same-instant
value (last executed wins), and is exactly why the intra path cannot reuse it: an
intra-assignment site can have **N in flight**, each with its own staged copy.

So the record grows a fourth tail class beside the narrow-plane, masked-plane and
aggregate-byte-image ones: `{ ptr owned }`. Arm deep-copies the sampled RHS into a
fresh owned handle stored in the record; commit moves it into the live destination
and wakes level-only (a handle has no flat value to change-detect, same as the
aggregate case). The IR side is a `HandleCopyNb` variant carrying
`StoreNbDelayInfo` / `StoreNbEventInfo`, mirroring how `StoreInertialDelay` reuses
`StoreNbDelayInfo` (`Ir.h:1362-1367`).

**The new work is the lifecycle, not the copy.** Every existing record class is
plain old data: a record that never commits can be dropped. An owned-handle record
cannot — dropping it leaks. Three drop paths must free it: a descheduled inertial
record (the `epoch` stamp path), a `$finish` with records still armed, and
`rt_run_init`'s pool reset (`Runtime.cpp:6437`, which already returns the
intra-assignment queues to power-on). That obligation is what makes this the
largest single item in the remainder, and it is also what closes the standing
`q <= #2 r` entry.

### R4 — trigger side: a multi-signal trigger `@(a or b)` — LANDED

The single-trigger assumption was baked in at four levels: `StoreNbEventInfo`
carried one `(Signal trig, uint32_t edge)` pair; the frontend checked
`events.size() != 1`; `emit_intra_event_arm` called `rt_intra_event_register`
exactly once; and `rt_intra_rec` had no way to be found in a *second* trigger's
pending vector.

What it needs: an `ExtraSlice` of `(trig, edge)` pairs in the payload, a `trig_id`
allocated per listed event, one `rt_intra_event_register` call per event for the
*same* record, and **first-fire-wins removal**.

Removal has to be real, not lazy. The tempting version — mark the record matured and
let other lists skip it — is unsafe: a committed record returns to its freelist, so a
stale pointer left on another trigger's list can later match a *reused* record.
Instead give the record tail a `[intra_trigs × i32]` array of the `trig_id`s it
registered on, where `intra_trigs` is the maximum event-list length over the sites
targeting that signal — the same per-`SignalSlot` monomorphisation that `is_array`,
`is_agg` and `intra_masked` already drive. On maturation, `rt_intra_event_fire`
removes the record from each other listed id by pointer identity, which the existing
in-place list compaction already does the work of. The scan is paid only on the
maturing edge and only by multi-trigger sites; single-trigger codegen is unchanged
(`intra_trigs <= 1` adds no field), which was checked by diffing `clotho build
--emit llvm` for a single-trigger design across the change.

**As built**, two details this sketch did not have. The trigger list is an
`ExtraSlice` of packed two-word `IntraTrigger` records (the `WaitInfo` encoding),
packed ONCE per statement so an N-leaf decomposing target interns one list, not N.
And the record's id array is filled from `trigger_id_value`, not from compile-time
constants: a dense trigger id is a per-INSTANCE link-time constant
(`abs_id_const`) while twin instances of a definition share the emitted code, so a
constant global would name one instance's triggers from both. That is also why the
array has to live in the record rather than beside it — a first attempt at a
private constant global was wrong for exactly this reason, and there is a
twin-instance test for it. The runtime learns the list through
`rt_intra_event_register_group(ids, n, rec)`, where `ids` points into the record's
own (stable) tail.

### R5 — trigger side: an `iff` qualifier

Refused at `lower/Assign.cpp, lower_assign` — "the deferred-record path has no guard hook".

LRM 9.4.2.3 makes `iff` part of the *event*, not a gate on the action: "The event
expression only triggers if the expression after the `iff` is true." So the guard
must be evaluated **at fire time**, and for `repeat(N) @(e iff c)` only *enabled*
occurrences count down. That is why the plain-event mechanism cannot transfer:
`emit_guarded_body` (`lower/Timing.cpp`) wraps the already-lowered body in
`if (C)`, which is observably identical for an `always @(a iff en)` block (no
count to consume) but *wrong* for a countdown — a disabled edge would still
decrement. A deferred record has no body to wrap in the first place.

Design: emit one `i1 <site>.guard(ptr state)` function per `iff` site, store its
address in the record (one pointer field, gated on any site for that signal carrying
an `iff`), and have `rt_intra_event_fire` call it before decrementing `count`.

The reason this is cheap rather than a capture problem: **slang forbids an automatic
variable in the timing control of a nonblocking assignment**
(`diag::AutoFromNonBlockingTiming`, raised under `ASTFlags::NonBlockingTimingControl`,
`MiscExpressions.cpp:70-88`). The guard therefore reads only static and design state
by construction, so it needs nothing from the arming process's coroutine frame and
can be a plain function of `state`. Without that upstream rule the guard would have
to mix arm-time captures with fire-time signal reads, which the LRM does not
contemplate; with it, there is one honest evaluation point.

### R6 — two leftovers that are not target shapes

- **A runtime-offset select of a very wide vector** (`lv.wide_dyn`, storage wider
  than `wide_dyn_select_bits`) throws in `fill_intra_payload`
  (`ir/llvm/Stores.cpp, fill_intra_payload`). It addresses words through `rt_wide_ins` instead
  of a storage-width mask, so its value and mask do not fit the record's
  storage-width fields. Same answer as the whole-aggregate case: a heap-side
  payload. Group it with that shared follow-up rather than growing the record.
- **A non-positive repeat count** — LANDED. 9.4.5: "If the repeat count literal, or signed
  variable holding the repeat count, is less than or equal to 0 at the time of
  evaluation, the assignment occurs as if there is no repeat construct." The blocking
  form gets this from its signed `Lt` bound (`lower_intra_timing`,
  `lower/Timing.cpp, lower_intra_timing`: a negative bound waits zero times, so the store happens
  immediately). The nonblocking record's `count` is `uint32_t`, so today a
  non-positive count waits 2**32 edges. Fix: a signed test at the arm site that
  routes `count <= 0` to the current instant's NBA region — the branch
  `emit_intra_nba_arm` already has for a runtime zero delay
  (`ir/llvm/Stores.cpp, emit_intra_nba_arm`), which is the nonblocking spelling of "the store
  happens immediately". Local, and it makes the two forms agree.

  **As built**, the decision is made in the FRONTEND, not at the arm site: fold it
  when the count is a constant, otherwise emit `count < 1` and route the true arm to
  a plain `StoreNb`. Two corrections to the sketch above, both measured against
  iverilog 13. The test is `count < 1` **in the count's own signedness**, which is
  `<= 0` for a signed count and `== 0` for an unsigned one — so an unsigned count
  does need a test, because zero is zero in both signednesses and clamps either way,
  while an unsigned variable holding -3 is 4294967293 and correctly waits (the
  clause's own example). And the bug was never only about NEGATIVE counts: a count
  of exactly 0 underflowed the same `uint32_t` countdown. An x/z count narrows
  through `CaseEq`, so the rule is "clamp only when the count is KNOWN
  non-positive"; 9.4.5 says nothing about an unknown count. For a decomposing
  target the test sits at each leaf, re-testing one already-evaluated SSA value,
  rather than wrapping the walk — wrapping it would re-lower every leaf's RHS in
  both arms and break single evaluation.

### The blocking form's own remainder (Class A)

Two gaps, both distinct from everything above:

- `s = #5 t` for a string and `mem = #5 other` for a whole array are refused at
  `src/lower/`, and the recorded reason is the real one: the string and
  array store paths evaluate the LHS *before* the RHS, so hoisting the RHS sample
  across the suspend reorders two evaluations that 9.4.5 pins in the other order.
  The fix is a lowering discipline (evaluate and spill the LHS address *and* the RHS
  before the suspend), not a record.
- An assignment-pattern lvalue is refused for both forms at `lower/Assign.cpp, lower_assign`.

### Phases

- **R-a** — LANDED. R1 (the four decomposing shapes) and R6's repeat-count clamp.
  Pure frontend, no record change, closed the largest number of refusals for the
  least new mechanism. It also narrowed what remains: the old blanket
  whole-aggregate refusal is now four specific ones (a conditional RHS, an
  aggregate with a container member, an indexed array slice, and a
  string/container operand of a streaming target).
- **R-b** — LANDED. R2 (`intra_ptr`, the vif member) and R4 (multi-signal
  trigger). One `SignalSlot` flag and one record-tail array, both extensions of
  tails that already existed. R2 turned out to need the instance CONTEXT as well as
  the pointer (see the correction under R2), and R4's id array had to live in the
  record rather than in a constant global (see R4). Landing R2 also surfaced and
  fixed a latent runtime bug: `rt_intra_alloc` recycled a pooled record without
  checking its stride, so a design whose record is wider than a previous
  in-process design's for the same dense site id overflowed it.
- **R-c** — R5 (`iff`), which needs the per-site guard function and a fire-time call.
- **R-d** — R3 (the owned-handle record class), largest because of the free
  discipline. Closes `q <= #2 r` and `s <= #5 t` together.
- **Shared, unscheduled** — the single-memcpy deferred record with a heap-side
  payload, which subsumes the whole-aggregate cost of R1 and the `wide_dyn` refusal
  of R6, and is a plain-NBA optimisation first.

### Refusal surface after all of it

Permanent, by the LRM: a queue / dynamic-array / associative-array **element**
(6.21), and an automatic variable (10.4.2). Tracked elsewhere: a class property
(the class-property NBA gap). Everything else in the remainder above is closable.

After R-a and R-b, what still fails loud, with where:

| shape | guard |
|---|---|
| a `string` target (R-d) | `lower/Assign.cpp, emit_agg_store` |
| a whole queue / dynamic array (R-d) | `lower/Assign.cpp, emit_agg_store` |
| an aggregate with a container member (R-d) | `lower/Assign.cpp, emit_agg_store` |
| an `iff` qualifier (R-c) | `src/lower/` |
| a conditional RHS over a whole aggregate | `lower/Assign.cpp, emit_agg_store` |
| an indexed array-slice target | `lower/Assign.cpp, lower_assign` |
| a string / container operand of a streaming target | `src/lower/` |
| a write to a force target (whole, or a concat operand) | `lower/Assign.cpp, lower_assign` |
| a runtime-offset select of a very wide vector | `src/ir/llvm/` |
| BLOCKING: an assignment-pattern lvalue | `src/lower/` |
| BLOCKING: a whole unpacked struct / array | `src/lower/` |
| BLOCKING: a string or array target | `lower/Assign.cpp, lower_assign` |
| a class property (tracked as the class-property NBA gap) | `lower/Assign.cpp, lower_assign` |

### Oracle

iverilog 13 stays the oracle for every shape it can express, as it is for the landed
work — 9.4.5 is fully specified and the nonblocking region ordering is 4.4.2's, not
a nondeterministic corner. Two of the items are not expressible against it and are
derived from the clause text instead: the `iff` countdown semantics of
`repeat(N) @(e iff c)` (9.4.2.3 plus 9.4.5, and the wrong reading — gating the
action — is the one a body-wrapping implementation would produce), and the
vif-member target, whose whole point is which instance the update lands in.

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

Per-phase additions for the remainder above:

- **R1**: `{a,b} <= #5 x` against iverilog, including the self-overlapping
  `{a, a[0]} <= #5 x` (must match the plain-NBA answer); a whole unpacked struct and
  a whole array, both with the RHS mutated during the delay (one sample, N leaves);
  a streaming-concat target; an assignment-pattern target. Event form of each, so the
  shared-count path is exercised, not just the shared-deadline one.
- **R2**: two interface instances, a vif handle pointing at the second, and
  `vif.sig <= #5 x` — the first instance must be untouched. This is the test the
  guard exists to make unnecessary; it is not expressible against iverilog (no vif
  DPI-free oracle for which instance was written), so it is a clotho-only assertion
  derived from 10.4.2's evaluation rule.
- **R3**: `s <= #5 t` with `t` reassigned during the delay (the staged copy must be
  the sample, not the later value); `q <= #2 r` with `r` resized during the delay;
  N in flight to the same string signal; and the three drop paths free the staged
  handle (a descheduled inertial record, `$finish` with records armed, a second
  simulation in one process after `rt_run_init`) — the last checked under the
  existing leak assertions rather than by output.
- **R4**: `x <= @(posedge a or posedge b) y` — first edge wins, the record is gone
  from the other list (assert by firing the *other* trigger afterwards and observing
  no second commit); `repeat(3) @(posedge a or posedge b)` counting either edge.
- **R5**: `x <= @(posedge clk iff en) y` (a disabled edge must not commit) and
  `x <= repeat(2) @(posedge clk iff en) y` — the countdown must consume only enabled
  edges, which is the case a body-wrapping implementation gets wrong.
- **R6**: `x <= repeat(N) @e y` with `N` a negative signed variable and with a real
  `-1.4`; both must land in the current instant's NBA region, matching the blocking
  form's immediate store.
