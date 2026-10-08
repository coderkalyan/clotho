# Named events (LRM 15.5)

Target: SystemVerilog named events — `event e;`, `-> e`, `@(e)`,
`wait(e.triggered)`, `always @(e)`. TODO.md M2 (Icarus parity) item; all forms
loud-fail today.

Status: **Landed** — Tier 1 (`event e;`, `-> e`, `@(e)`, `always @(e)`,
`wait(e.triggered)`, `e.triggered`) plus the Tier-2 slice `->>` (NBA-region),
`->> #d e` (detached timer), and general `wait(<level-expr>)`.

The **event handle** semantics (assignment / merge / `null` / `==` / aggregates /
arguments / class properties / ports) and the **event-controlled trigger**
`->> @(x) e` are designed in **`docs/design/event-handles.md`**, which supersedes
the deferral sections at the end of this file. That design reworks the model
below — an event becomes a 64-bit handle into a runtime slot table with an
intrusive waiter list — so treat everything from "Event `Signal` identity"
onward as a description of the *current* implementation, not the target.

## The enabling insight

Clotho's wait/wake engine is entirely JIT-emitted codegen — there are **no**
runtime functions for subscribe/suspend/wake. The C runtime (`src/runtime/`)
owns only the timing wheel (for `#delay`) and `rt_sim_run`. Signal
subscription, coroutine suspend, and wake-on-change are LLVM coroutines plus
module-global arrays, all keyed by a `Signal` handle:

- Per-signal wait buckets `clotho.wait.<sig>.{sta,dyn}.{pos,neg,any}`
  (`build_wait_queues`, `LlvmBackend.cpp`), entry `{ ptr handle, ptr sched_flag }`.
- Subscribe: `emit_static_subscribe` (always @), `emit_dynamic_register`
  (intra-body @).
- Wake: a blocking store calls `service_wait_queue(sig, old, new, …)` →
  `emit_edge_facts` classifies the transition → per-bucket
  `service_static_queue` / `service_dynamic_queue` push coroutine handles onto
  the FIFO active queue.

Wait-queue storage is independent of value storage but shares the `Signal`
handle. So a named event is modeled as a **synthetic signal that owns wait
buckets but carries no data value**, and its operations reuse the existing
engine:

| Event op | Lowering |
|---|---|
| `-> e` | `service_event(e)` = `service_static_queue(e, Any)` + `service_dynamic_queue(e, Any)`, called **unconditionally** — a trigger is an Any-edge pulse with no value transition, so it bypasses `emit_edge_facts` |
| `@(e)` / `@(e or …)` | existing `lower_wait` with `WaitEdge{ e, Any }` — no new backend code |
| `always @(e)` | existing static subscription |
| `wait(e.triggered)` | per-event trigger-timestamp compare + level-recheck suspend loop |
| `->> e` | Tier 2 (NBA-region / wheel path) |

## `.triggered` without a timestep-boundary reset

`e.triggered` is true from the trigger through the end of the current time step
(LRM 15.5.3: the triggered state persists throughout the time step). Store one
`u64 last_trigger_time` per event:

- `-> e` sets `last_trigger_time = rt_wheel_now()`.
- `e.triggered` lowers to `last_trigger_time == now`.

Sim time is constant across delta cycles, so this is automatically true across
same-instant deltas and false once time advances — **no reset pass or
timestep-boundary hook is needed**. The timestamp initializes to a sentinel
(`UINT64_MAX`) meaning "never triggered" (`now` never equals it at t=0 unless a
trigger set it this instant). This per-event `u64` is the only genuinely new
simulation state.

The classic race is handled correctly: `-> e` in one process followed by
`@(e)` in another that had not yet suspended misses the trigger (bare `@`
catches only the *next* trigger), whereas `wait(e.triggered)` sees
`last_trigger_time == now` and falls through immediately.

## slang AST surface (v8, confirmed)

- `event e;` — `VariableSymbol` of `EventType` (`SymbolKind::EventType`).
- `-> e` / `->> e` — `EventTriggerStatement{ const Expression& target; const
  TimingControl* timing; bool isNonBlocking; }` (`StatementKind::EventTrigger`).
- `wait(cond)` — `WaitStatement{ const Expression& cond; const Statement& stmt; }`
  (`StatementKind::Wait`).
- `e.triggered` — a system **method** on `EventType`
  (`NonConstantFunction`, `KnownSystemName::Triggered`, `isMethod`, returns
  `bit`, 0 args), so it surfaces as a `CallExpression` detected by its known
  system name with the event as the method's operand.
- `@(e)` — an ordinary `SignalEventControl` whose `expr` is a `NamedValue`
  referencing the event variable; flows through the existing
  `collect_wait_events` / `event_signal` path once the event has a `Signal`.

## Event `Signal` identity — implementation

An event is registered as an ordinary `Signal` carrying an `is_event` flag on
`SignalInfo` (`src/ir/Module.h`); its `type` is a 1-bit `logic` placeholder so
it flows through signal allocation, but its value planes are never read or
written. Two pieces of state hang off the handle:

- **Wait queues** — the existing per-`(signal, edge)` `clotho.wait.<sig>.…`
  globals (`build_wait_queues`), used only through the `Any` bucket.
- **Trigger timestamp** — a per-event `clotho.event_trig.<name>` i64
  (`event_trig_gv_`, indexed by handle, null for non-events), initialized to
  `UINT64_MAX`. `-> e` (`EventTrigger` IR op) stores `rt_wheel_now()` here and
  services the `Any` static + dynamic queues **unconditionally** (no
  `emit_edge_facts` — a trigger has no value transition). `e.triggered`
  (`EventTriggered` IR op) is `event_trig == rt_wheel_now()`, a definite 2-state
  bit.

Because `-> e` never emits a value store, `service_wait_queue` / `emit_edge_facts`
are never invoked for an event, and no phantom value change reaches any other
pass. Event-touching processes are excluded from thunk elision / comb fusion
(`is_simple_always` returns false on `EventTrigger` / `EventTriggered`), since a
trigger's wake is a scheduling side effect and `.triggered` reads out-of-model
state.

Considered and rejected: a "fake 1-bit wire" whose `-> e` toggles a real value
plane (always-changes → Any fires). It rides even more existing machinery but
leaks a phantom wire into VCD / z-elision / domain analysis, and `.triggered`
still needs the timestamp — so the toggle buys nothing.

### Post-rebase note (VCD)

This branch was cut from an `origin/main` predating the VCD merge, so no VCD
guard was needed here. On rebase onto a main with VCD, a whole-design
`$dumpvars` must **loud-fail** on an event signal (an event has no dumpable
value) — add the `is_event` check alongside the existing string-signal /
multidimensional-array VCD guards.

## Frontend guards replaced (all `fail(...)` today)

| Construct | Location today |
|---|---|
| `event e;` | `lower_type` `fail("unsupported non-integral type")`, `Lower.cpp:797` (TODO already names event) |
| `-> e`, `wait(...)` | `lower_stmt` `default: fail("unsupported statement")`, `Lower.cpp:2088` |
| `e.triggered` | never reached (type rejected) |

## Tier 1 work items

1. **Type + storage.** Accept `EventType` in `lower_type`; allocate a synthetic
   event `Signal` with an `is_event` slot carrying the `u64` timestamp; register
   in the sigmap so `@(e)` and `-> e` resolve the event variable to its handle.
2. **`EventTrigger` statement** (blocking `->` only). New `lower_stmt` case; new
   IR op `EventTrigger(signal)`; backend sets the timestamp then calls
   `service_event`.
3. **`Wait` statement.** New `lower_stmt` case. For `wait(e.triggered)`: emit
   `last_trig == now ? fall-through : { register e/Any, suspend, recheck }`.
   General `wait(expr)` over arbitrary signals (TODO.md) is the same
   mechanism (subscribe to the cond's read set, loop-recheck) and is scoped as a
   fast-follow, not required for events.
4. **`.triggered` expression.** Detect the `Triggered` system method, lower to
   the timestamp compare.
5. **Wait-queue accounting.** Event waiters count into
   `wait_counts` / `build_wait_queues` automatically once they are real
   `WaitEvent{ e, Any }`; verify capacities.
6. **Guards + tests.** Loud-fail the Tier-2 set below; `tests/event_test.cpp`
   cross-checked against iverilog.

### Loud-fail boundary (Tier 1)

Event assignment/merge `e2 = e1`, `null`, `->>` nonblocking + delayed trigger,
events as ports / task-func args, events inside arrays / structs / queues,
`wait_order`, `wait fork`. (`iff` on a single event control is now supported —
lowered as a body/`@`-loop guard in `collect_wait_events` / `emit_guarded_body`;
only per-event `iff` in a multi-event list stays guarded.)

### Scheduler-analysis interaction to verify

`-> e` is a *writer* of `e` in the blocking-write wake graph. It must slot into
topo-subscription order and the fusion / thunk self-wake predicates like any
other blocking write: a process that triggers an event in its own sensitivity
(`always @(e) begin … -> e; end`) must not be fused into a self-re-waking
group. The existing self-write exclusion should cover it; this is a test point,
not expected new logic.

## Tier 2 — `->>` nonblocking trigger + general `wait` (landed)

**`->> e` (nonblocking, no delay), LRM 15.5.4.** A distinct IR op
`EventTriggerNb` marks the event pending (an i8 `clotho.event_pending.<name>`)
instead of waking now. The NBA region — after committing this instant's
nonblocking updates — scans the events that are `->>` targets
(`nb_event_sigs_`, collected in `nba_classify_ir` like `array_nba_sigs_`), and
for each pending one stamps the trigger time and services its Any queue
(`emit_event_fire`, shared with `-> e` but taking the wheel explicitly since the
drain's `wheel_` member is stale). `has_nba` and the NBA "anything staged" test
gain the pending-event check, so the region runs even in a design with no NBA
assignments. Consequence, verified: a `-> e` waiter wakes in the Active region
and reads the pre-commit value; a `->> e` waiter wakes in the NBA region and
reads the post-commit value.

**General `wait(<level-expr>)`, LRM 9.4.3.** `lower_wait_stmt` now lowers any
condition (not just `.triggered`) as `forever { if (expr) break; @(read-set of
expr); }` — the condition's read set comes from `collect_expr_reads`, and the
`if` truth rule makes an x/z condition keep waiting. This subsumes
`wait(e.triggered)` (read set = the event; `-> e` wakes it; the re-tested
`.triggered` is true). Verified vs iverilog for non-event conditions.

Neither `->>` nor `.triggered` is implemented by iverilog, so both are pinned to
the LRM.

## Tier 2 — delayed nonblocking trigger `->> #d e` (landed)

The fixed-delay form schedules a **detached wheel timer** — no hosting
coroutine. `EventTriggerNbDelayed` (op = delay in ticks, extra = the event)
lowers in a process body to: `deadline = now + d; node = rt_alloc(16);
rt_wheel_schedule(wheel, node, deadline, ev_delay_thunk_<e>)`, then falls
through (nonblocking). One `ev_delay.<e>(rt_timer* t, void* ctx)` thunk is
generated per delay-targeted event (`build_delay_event_thunks`, after
`build_wait_queues` so the event globals exist): it stamps the trigger time and
services the event's Any queue via the shared `emit_event_fire(e, ctx)`, then
`rt_free(t)`. The thunk reads the current time from `ctx`, which is the wheel —
so the drain's `rt_wheel_advance(wheel, null)` became `(wheel, wheel)` (the
coroutine trampolines ignore ctx; only this thunk uses it). The timer node is a
bare 16-byte `rt_timer` (`{next, on_expire}`), heap-allocated per trigger and
freed on expiry; a per-event freelist pool is the obvious follow-up if a hot
`->> #d` loop shows allocator churn. Verified: the issuing process does not
block, the event fires `d` ticks later, runtime (non-constant) delays work, and
1000 concurrent detached timers run and free cleanly. iverilog implements no
`->>`, so pinned to the LRM.

## Superseded: event handles, `->> @(x) e`, `wait_order`, ports

These were deferred here with a captured design (Options A/B) built on the
premise that handles require exposing the active-queue push to the C runtime and
growable wait queues — a scheduler-core change. **That premise is wrong and both
options are withdrawn.** See `docs/design/event-handles.md`:

- Generated code is already reachable *from* the runtime through the wheel's
  `rt_timer_fn` callback, and `clotho.ev_delay.<e>` already performs a
  runtime-initiated event wake that touches `clotho.active`. The active queue
  stays codegen-private.
- Statically sized wait buckets are replaced for events by an intrusive
  doubly-linked list of frame-resident nodes — the mechanism remote `disable`
  already uses (`disable_node_ty`, `emit_disable_link`/`_unlink`) — which is
  unbounded by construction.

`wait fork` remains genuinely deferred: it needs the live-child set a `fork` does
not retain, which is unrelated to events.


## Oracle

iverilog supports named events, `->`, `@(e)`, and `.triggered` — the Tier-1
oracle. iverilog's support for `->>` and event handles (Tier 2) is incomplete,
so those are pinned to the LRM instead.
