# Event handles (LRM 15.5.5)

Target: named events as **real objects with handles** — `a = b` merge, `a = null`,
identity `a == b` / `a == null`, events as subroutine arguments (`in`, `out`,
`inout`, `ref`), event class properties, event ports, and events inside every
aggregate (fixed unpacked array, unpacked struct, queue, dynamic array,
associative array value *and* key).

Status: **landed.** Supersedes the "Deferred: event handles" section of
`docs/design/named-events.md`, whose Options A/B are both withdrawn — see
[§2](#2-the-two-recorded-blockers-dissolve).

Every form in LRM 15.5 is implemented: handles (merge / `null` / identity / the
Boolean test), events in unpacked arrays, unpacked structs, queues, dynamic arrays
and associative arrays, per-instance and `static` class properties,
automatic-lifetime locals, subroutine arguments in all four directions, module
ports, `@(<any event expression>)`, `->> e`, `->> #d e`, `->> @(x) e` /
`->> repeat(n) @(x) e`, `wait_order`, and event operations inside a function.

Where the code differs from the design as first written. §3, §5, §9, §10 and
§12 describe the code; the rest reads as written.

- **A handle is the object's address and the object is collected.** No slot
  table, no generation and no reference count: `docs/design/kernel.md` makes a
  handle one pointer, and `docs/design/architecture.md` makes every built-in
  handle kind a collected object. LRM 15.5.5.2's reclamation is the collector's
  (§3, §9).
- **One tier.** A declared event's object is allocated when the design starts,
  like every other event's, and code reads its handle from the variable. The
  static block and the monomorphic fast path are gone (§5).
- **The wheel is a module global.** Cheaper than threading `ptr wheel` through
  every function signature (the RNG precedent), and it lifts the restriction that
  event operations could not appear in a function. Measured a 16% *win* on a pure
  `#delay` loop.

Measured against `main` when the list landed, on 3M triggers: **−19%** with three
`always @(e)` waiters, **−16%** on a pure `#delay` loop, **+15%** with two one-shot
`@(e)` waiters in a loop — the price of a doubly-linked node's link/unlink over
the old bucket's append plus bulk clear, which is what buys the unbounded waiter
set and handle semantics.

## 1. Decision summary

Three moves, in dependency order. Everything else follows.

| # | Move | Replaces |
|---|---|---|
| **A** | An event is a **64-bit plane word**: a handle to its object. | `lower_type(EventType) -> logic_type`, a 1-bit placeholder (`lower/Types.cpp, lower_type`) |
| **B** | An event's waiters are an **intrusive doubly-linked list of frame-resident nodes**, unbounded. | statically-sized per-`(signal,edge)` buckets `clotho.wait.<sig>.{sta,dyn}.*` for events only |
| **C** | The trigger is **one generated function** `clotho.event_fire(ptr obj, ptr wheel)`, called from generated code *and* from runtime callbacks. | per-event inlined `emit_event_fire` against named globals (`ir/llvm/Events.cpp`) |

Move A buys every storage class and the whole call ABI for free (§4). Move B
removes the static-capacity blocker (§6). Move C keeps `clotho.active`
codegen-private, so the scheduler core is **not** touched (§2).

The efficiency claim was first concentrated in §5's monomorphic fast path. It is
now that a handle is the object's address: every use is one load of the variable
and a field access, with no table and no call.

---

## 2. The two recorded blockers dissolve

`docs/design/named-events.md:241-252` records two reasons handles were deferred.
Both are wrong about the current code.

### Blocker 1 — "waking a waiter pushes onto `clotho.active`, which a runtime `rt_event_trigger` can't reach, so handles force exposing the active-queue push to the runtime"

True premise, wrong conclusion. The runtime never needs to push, because
**generated code can already be called from the runtime** and a JIT-emitted
function can touch `clotho.active` freely.

The precedent is not hypothetical — it is the delayed-event thunk that already
ships:

```cpp
// src/ir/llvm/  (build_delay_event_thunks)
auto* fn = llvm::Function::Create(fnty, llvm::Function::InternalLinkage,
                                  "clotho.ev_delay." + mod_.signal(sig).name, env_->llmod);
b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
emit_event_fire(sig, fn->getArg(1));         // ctx (arg 1) is the wheel
b_.CreateCall(free_fn(), { fn->getArg(0) }); // free the detached timer node
```

The wheel calls this through `rt_timer_fn = void(*)(rt_timer*, void* ctx)`
(`Runtime.h:1401`, dispatched `Wheel.h:193`), and its body runs
`emit_event_fire` → `service_static_queue`/`service_dynamic_queue` →
`emit_wake_push` → `clotho.active[tail++]`. A runtime-initiated event wake that
reaches the codegen-owned active queue **is already in production.** Move C only
generalises it from one-thunk-per-event to one-thunk-per-design taking the object
as an argument.

### Blocker 2 — "dynamic wait-queue capacities are computed statically, so an unbounded number of live registrations would overflow"

True, and fatal for the *bucket* representation — `wait_counts()`
(`ir/llvm/Events.cpp, wait_counts`) enumerates waiters over the IR at compile time, and
a waiter on `evq[i]` or a `ref event` formal is not enumerable. So the design
does not use buckets for events. It uses an intrusive list, for which the
codebase already contains a complete worked implementation: the remote-`disable`
activation list.

```cpp
// src/ir/llvm/Processes.cpp, disable_node_ty
llvm::StructType* disable_node_ty() {
    auto* ptr = llvm::PointerType::getUnqual(ctx_);
    return llvm::StructType::get(ctx_, { ptr, ptr, ptr, ptr, ptr, b_.getInt32Ty() });
}
```

That node is **frame-resident** (zero allocation), linked in the prologue
(`emit_disable_link`, `:21523`), unlinked in the epilogue (`emit_disable_unlink`,
`:21550`, including the no-`prev` head fixup at `:21569`), and walked by
generated code that wakes each entry through `emit_wake_push` (`:21408`) after
testing its scheduled flag (`:21404-21405`) — the same flag protocol as a wait
bucket. It is unbounded by construction. §6 reuses it verbatim.

### What is *actually* left, and is designed for

Three real constraints survive, all local:

1. **The ring-overflow guard is compiled out** unless
   `ring_guard_wanted() = design_forks() || design_uses_disable_`
   (`ir/llvm/Scheduler.cpp, ring_guard_wanted`). Event waiters are parked process/task
   activations already counted in `cap = bit_ceil(nprocs + fork_ring_slots())`
   and deduped by the scheduled flag, so the bound still holds —
   but the invariant becomes much harder to audit by hand. **This design requires
   `ring_guard_wanted()` to include a new `design_uses_event_handles_`.** Cheap
   insurance against the one failure mode PRINCIPLES.md forbids: a silent
   out-of-bounds store.
2. **VCD aims a raw pointer at `clotho.event_trig.<name>`** at `$dumpvars` time
   (§12). Moving the tick into an object is an ABI change for that registration.
3. **The wheel callback ABI is fixed at `void(rt_timer*, void* ctx)`** with `ctx`
   globally bound to the wheel by `rt_wheel_advance(wheel, wheel)`
   (`ir/llvm/Scheduler.cpp, emit_active_run`). A per-trigger payload must therefore ride the timer
   node, not `ctx` (§7.3).

---

## 3. Object and handle representation

### 3.1 The handle word

```
handle : i64  =  the address of the event object
null   : i64  =  0
```

A 64-bit 2-state plane word, identical in every mechanical respect to a class
handle. `Type::Kind::Event` interns as `{ Domain::Two, 64 }` beside
`Kind::Class` (`src/ir/InternPool.cpp`), and `types::storage_class` maps it to
`StorageClass::Plane`. `types::is_object_handle` (`src/ir/TypeModel.h`) is the one
predicate that says "the collector traces this word", true of a class handle and
an event handle alike.

The handle was first `{ u32 slot, u32 gen }` into a runtime table, the
generation being there to detect a stale handle once slots were released. With
a collector nothing a handle names is freed while the handle exists, so there is
nothing to detect.

### 3.2 The object

```c
// src/runtime/Runtime.h
typedef struct rt_event {
    const void* vptr;      // names the collector's trace map, as a class object's does
    uint64_t    last_trig; // sim tick of the last fire, or UINT64_MAX == never
    void*       waiters;   // intrusive list head (event node), or NULL
    void*       next_pend; // next object on the NBA pending chain (§7.2)
    int32_t     vcd;       // dump-var index, or -1 (§12)
    uint32_t    trig;      // id as a `<= @e` / `->> @(e)` control, 0 until it is one
    uint8_t     pending;   // a `->> e` awaiting the NBA region
} rt_event;
```

An event object is a collected object (`docs/design/garbage-collection.md`) that
only the runtime allocates: `rt_event_new`, and `rt_event_new_elem` for a
container element, which never collects because element construction runs inside
container operations whose buffers the collector cannot see until they are
installed. The runtime owns the descriptor the `vptr` word names
(`src/runtime/Scheduler.cpp`), and the trace map lists no handle.

### 3.3 Deref

`inttoptr`. A null handle must not be dereferenced, and two forms deal with it:

- A trigger (`-> e`, `->> e`) branches on null and does nothing, which is LRM
  15.5.5's no-op.
- A wait and `.triggered` select the design's **null object**
  (`clotho.event_null`) in place of a null handle. Nothing triggers it, so a wait
  on it blocks forever and its tick stays the never-triggered sentinel, without a
  branch. It is a global of the design, not a collected object, and no handle
  holds its address.

---

## 4. What Move A buys for free

Because the handle is a `StorageClass::Plane` 64-bit word, every gate in the
storage model that refuses an event today is satisfied by *deleting the gate*,
not by writing new machinery. This is measurable against the class handle, which
already occupies every one of these cells (`tests/data_model_test.cpp` walks the
`elements × containers × modes` matrix and the class-handle row is excluded from
exactly one cell).

| Form | Gate to delete | New code needed |
|---|---|---|
| `event a, b; a = b;` | `lower/Assign.cpp, lower_assign` | none — plane store |
| `a = null;` | `lower/Assign.cpp, lower_assign` | peel the `ConversionExpression` slang inserts over the `NullLiteral` |
| `a == b`, `a == null`, `a !== b` | `lower/Expr.cpp, lower_expr` | none — `icmp` via the generic scalar compare |
| `event arr[4];`, `arr[i]` | `lower/Types.cpp, lower_type` | none — 8-byte plane element |
| `event q[$]`, `event d[]`, `event aa[string]` | `lower/Types.cpp, guard_container_element` (one predicate, three names) | `rt_elem_event` ops kind: constructed per slot, traced like a class handle |
| `int aa[event]` (event as key) | the key arm at `lower/Types.cpp, lower_type` | none — word key |
| `struct { event e; }` | `lower/Types.cpp, lower_type` | none — aggregate leaf slot |
| `class C; event e;` | `lower/Lowerer.h` | none — body field, `agg_member` adds the header word |
| `class C; static event e;` | *no gate exists* — this is the misleading-error hole | route through `declare_static_prop` |
| `task t(event x)` (in) | *no gate exists* — fails downstream as `reference to unknown signal 'x'` | none — scalar `(i128 mm, i64 z)` arg |
| `task t(output/inout event x)` | *no gate exists* — **silently accepted and runs today** | none — `(&mm, &z)` arg |
| `task t(ref event x)` | `src/lower/` | none — `ref_actual_ptr` returns `lv.base` for a whole plane slot |
| `function event f()` | — | none — `fn_return_ty` `{ i128, i64 }` |

The `ref` case deserves emphasis, because the current refusal says "there is
nothing for a pointer to alias" — true of a valueless event, false of a handle
word. The fat-ref ABI comment already anticipates this:

> Every storage class goes through here — a plane slot, a managed handle, an
> aggregate image — so `ref` has one shape.
> — `src/ir/llvm/Subroutines.cpp, lower_call_common`

A `ref` to a stable place passes a null witness chain, so a `ref event` costs its
pointer and one null word.

### 4.1 Ports

Event ports are **aliasing** ports, not copying ports. `input`/`output`/`inout`
all collapse the child's symbol onto the parent's `ir::Signal`, which is what
`build_inout_aliases` (`lower/Types.cpp`) already does for `inout`/`ref` and
what interface ports get from `canonical_value_symbol` (`lower/Scans.cpp`):

```cpp
// lower/Types.cpp, collect_scope_signals — the existing collapse, reused verbatim for events
if (const auto ia = inout_alias_.find(&m); ia != inout_alias_.end()) {
    sigmap_[&vs] = ia->second;
    break;
}
```

This is both correct and free: LRM 15.5.5 says a port-connected event names the
*same* sync object, and aliasing gives that by construction. Extend
`build_inout_aliases`' direction filter to admit `In`/`Out` when the port type is
an event, keep its existing whole-signal requirement, and delete
`lower/Hierarchy.cpp`. `wire_ports` then needs no event case at all, because an
aliased port is skipped there already.

Consequence to state plainly: an event port creates **no** copy process, so
`-> e` inside the child and `@(e)` in the parent are the same object. That is the
LRM semantics, and it is why the plane-copy port model must not be used here.

---

## 5. The monomorphic fast path (removed)

As first built, a scalar declared event had a compile-time slot in a static
block of objects, and an event variable that no store reached was
**monomorphic**: its object address folded to a constant and its handle was never
loaded. A whole-design analysis (`analyze_event_monomorphism`) decided it.

It is gone, for two reasons. Unit code is shared by a unit's instances
(docs/design/modular-compilation.md), so only an event of no instance (a
package's) could still fold; every other use already loaded the handle. And an
object in a static block is not a collected object, so the collector would need
a second kind of handle. A declared event's object is now allocated by
`init_state` when the design starts, before any PreInit process runs, and every
use loads the handle from the variable and casts it.

---

## 6. The waiter list

### 6.1 Node

```
%clotho.event_node = type { ptr next, ptr prev, ptr handle, ptr sched_flag, i32 flags }
```

Fields 2 and 3 are exactly `wait_elem_ty()`'s `{ ptr, ptr }`
(`ir/llvm/Events.cpp, wait_elem_ty`) — the parked coroutine handle **with its region tag
bits intact** (`kReactiveHomeTag = 2`; `emit_wake_push` is what strips
them) and the opaque `sched_flag` pointer, which is a constant GEP
into `clotho.proc_sched` for a process and a frame alloca for a task
— store it raw and never interpret it.

### 6.2 Two node storage classes, one list

| Subscriber | Node lives in | Linked | Unlinked |
|---|---|---|---|
| `always @(e)`, top-level sensitivity | a contiguous `clotho.event_nodes` array in module state, one slot per static subscription (countable by the existing `wait_counts()` walk over `group_events_`) | once, in the elaboration seed loop, in `sub_order_` | never |
| intra-body `@(e)`, `wait(...)`, `wait_order` | the waiting coroutine's **own frame** | on suspend | on resume, or on `disable` |

Both are the same node type on the same list, so there is **one** scan mechanism —
the two-engine outcome the old design doc warned against is avoided. Putting the
static nodes in a contiguous array is a pure layout win: the permanent prefix of
every event's list is sequential in memory and prefetches, even though traversal
is a pointer chase.

Static-node capacity is exactly countable, so no new sizing risk. Dynamic nodes
are frame-resident, so they are **unbounded for free** — which is the whole point.

### 6.3 Fire

One function per design:

```
define internal void @clotho.event_fire(ptr %obj, ptr %wheel) {
  ; store rt_wheel_now(%wheel) into %obj->last_trig
  ; vcd_touch if this object is dumped (§12)
  ; n = %obj->waiters
  ; while (n) { next = n->next
  ;             if (*n->sched_flag == 0) { emit_wake_push(n->handle); *n->sched_flag = 1 }
  ;             n = next }
}
```

`next` is read before the wake, exactly as `lower_disable_list` does and for the
same reason (`ir/llvm/Processes.cpp, lower_disable_list`): the woken activation may unlink its
own node. Dynamic nodes are *not* bulk-cleared the way a dynamic bucket is
(`*lenptr = 0`); each waiter unlinks itself on resume, which is
also what makes a multi-event `@(a or b)` wait correct without
`emit_dynamic_remove`'s compaction loop.

Callable from generated code (`-> e`), from the NBA region (`->> e`), and from a
wheel thunk (`->> #d e`) — the three forms converge here, as they converge on
`emit_event_fire` today.

---

## 7. Trigger forms

### 7.1 `-> e` (blocking, LRM 15.5.2)

Resolve the target to an `rt_event*` (§8), then `call @clotho.event_fire(obj, wheel)`.
For a monomorphic target this inlines to today's code plus the list walk.

### 7.2 `->> e` (nonblocking, LRM 15.5.1)

Today the NBA region scans a compile-time set `nb_event_sigs_`
(`ir/llvm/Stores.cpp, nba_classify_ir`), which a runtime-identified
event cannot join. Replace with an **intrusive pending chain** through
`rt_event::next_pend`:

- `->> e` sets `obj->pending` and, if it was clear, links the slot onto a runtime
  head `g_event_pending`.
- The NBA region reverses the chain (it links the newest first) and walks it oldest first,
  calling `clotho.event_fire` per entry, clearing `pending`, and emptying the chain. Events
  trigger in the order their `->>` executed (LRM 4.6 b)).
- `emit_nba_any_staged` (`:8586-8606`) gains `g_event_pending != 0`; `has_nba`
  (`:8871-8873`) gains "the design contains any `->>`".

This is strictly better than the status quo: the NBA region visits exactly the
events that fired, not every `->>` target in the design.

### 7.3 `->> #d e` (delayed detached, landed)

The timer node carries the handle and the per-event thunk collapses to one. The
runtime owns the node, because it is what keeps the event alive until the delay
expires:

```c
void*    rt_event_timer_new(uint64_t handle);   // a wheel node holding `handle`
uint64_t rt_event_timer_take(void* timer);      // the handle back; frees the node
```

`clotho.ev_delay(rt_timer* t, void* ctx)` takes the handle back and pends that
event for the NBA region. This replaces `build_delay_event_thunks`'
one-function-per-event with one function per design, and removes the
`nb_delay_event_sigs_` static set for the same reason as §7.2.

### 7.4 `->> @(x) e` and `->> repeat(n) @(x) e`

Now expressible, and the reason it was deferred is gone. `x` is an ordinary
*signal*, whose buckets are statically sized — so do **not** put the detached
subscription in `x`'s bucket. Use a runtime-owned growable list keyed by the
existing `trigger_id(sig, edge)` (`ir/llvm/Events.cpp`), which is precisely the
shape of the already-shipping intra-assignment mechanism:

> `rt_intra_event_register(trig_id, rec)` / `rt_intra_event_fire(trig_id)`
> (`runtime/Scheduler.cpp`) — a `std::vector<void*>` per trigger id, compacted in
> place.

Add `rt_event_detach_register(trig_id, handle, count)` and have
`service_wait_queue` call a generated `clotho.detached_fire(trig_id)` after the
static and dynamic bucket scans, for the signals that have any such
registration. `repeat(n)` is the record's countdown. No new mechanism, no
capacity risk, no scheduler-core change.

---

## 8. Resolving a target — one shared resolver

The current asymmetry is a two-line accident: `event_signal`
(`lower/Timing.cpp, event_signal`) accepts `NamedValue | HierarchicalValue`, while
`trigger_target` accepts only `NamedValue`. That is why `@(s.e)`
works today and `-> s.e` does not. Both end in the same `signal_of`.

Replace both with **one** `EventRef resolve_event(const Expression&)` returning
either a monomorphic slot constant or an `ir::Index` producing the handle word,
handling the four kinds slang actually emits as an `EventTriggerStatement::target`
or a `.triggered` receiver:

| Expression kind | Source form |
|---|---|
| `NamedValue` | `e`, `C::e` (static class property) |
| `HierarchicalValue` | `u.e`, `i.e` (interface port), `i.e` (modport), `vi.e` (virtual interface) |
| `ElementSelect` | `evq[i]`, `evq[0]` |
| `MemberAccess` | `s.e` (struct field), `obj.e` (class property) |

plus arbitrary nesting (`obj.arr[i]`, `s.inner.e`). For every non-`NamedValue`
kind this is an ordinary lvalue load of a plane word — the existing
`lower_expr`/`lvalue_of` machinery, once `lower/Expr.cpp` stops refusing
to produce an event value. `.triggered` needs no change beyond that:
`triggered_event` already pulls `arguments()[0]` generically for
every receiver shape.

`wait_order(a, b, c)` (LRM 15.5.6) becomes expressible for the first time: park a
node on all three objects, and on each wake compare the firing object against the
expected next; a mismatch takes the `ifFalse` arm. Note it also needs the
reactive-region walk to stop hard-rejecting it (`lower/Lowerer.h, ReactiveSafetyChecker`), so it is
scoped as a follow-on (§13, Phase 5), not part of the core.

---

## 9. Lifetime

An event object is reclaimed by the collector once nothing names it. What names
it, exhaustively:

| Holder | How the collector finds it |
|---|---|
| An event variable in design state, a class property, a container element or key, a frame local | The tables and scans that find a class handle there: `types::is_object_handle` puts an event word in the same root tables, trace maps and element descriptors |
| A parked waiter | Its frame-resident node records the object it is linked onto, and frames are scanned conservatively. This is what keeps the object while `@(e)` is parked and `e` is reassigned, and the unlink on resume or `disable` stores into the object |
| A pending `->> e` | The NBA pending chain, walked from its head |
| A `->> #d e` timer | The runtime's list of unexpired timers |
| A `->> @(x) e` subscription on a signal | The runtime's subscription records |
| A `->> @(c) e` subscription on an event `c` | The records on `c`, while `c` itself is reachable |
| A dump | The object a dumped variable named at `$dumpvars` is kept for the run (§12) |

The pending chain, the timers, the subscriptions on a signal and the dump are the
runtime's own records, enumerated by `rt_internal::event_gc_roots`
(`src/runtime/Scheduler.cpp`) at every collection.

What waits on an event object is different: only that event can release it, so
it is reachable exactly while the event is. This covers the event a
`->> @(c) e` would trigger and the value a `x <= @(c) v` staged (when the
assignment waits on events alone; one that also waits on a signal edge is a
root, since a signal can always change). The collector marks from the roots,
then asks `rt_internal::event_gc_waiting` for what waits on each control it
reached, and repeats until a pass marks nothing: reaching `e` can reach what
waits on `e` in turn. A staged value that names its own control therefore does
not keep itself. Once marking is complete, `event_gc_release_dead` drops what
waits on a control that was not reached, frees its records and returns its
trigger id for the next event that becomes a control.

A persistent `always @(e)` node does not name its object. If the variable is
reassigned and nothing else names the old object, the object is reclaimed with
the node still linked; nothing can trigger a reclaimed object, and a persistent
node is never unlinked, so the node is never read again.

A store of an event handle into a class object runs the write barrier, as a
store of a class handle does. `->> e` writes the chain link into an event object
without one, because every object on the chain is a root.

---

## 10. Runtime ABI additions

All must be added to `expose()` in `src/ir/Jit.cpp:199-303` to be linkable, and
every struct the backend mirrors must keep field order in lockstep with the C
declaration — the convention `rt_null_site` follows
(`ir/llvm/RuntimeAbi.cpp, null_site_ty`: "Field order and types mirror the C struct
exactly, so the two change together").

```c
// objects
uint64_t rt_event_new(void);                   // a fresh object's handle
uint64_t rt_event_new_elem(void);              // the same for a container element; never collects
uint32_t rt_event_trig_id(void* obj);          // RT_TRIG_EVENT | the object's id, assigned on first use

// ->> #d timers
void*    rt_event_timer_new(uint64_t handle);
uint64_t rt_event_timer_take(void* timer);

// ->> @(x) detached subscriptions (mirrors rt_intra_event_*)
void     rt_event_detach_register(uint32_t trig_id, uint64_t handle, uint32_t count);
uint32_t rt_event_detach_fire(uint32_t trig_id);
uint64_t rt_event_detach_ready(uint32_t i);
```

`rt_run_init` takes the address of the pending-chain head, a global of the
design, so the collector can walk the chain.

Deliberately **absent**: any `rt_event_trigger` / `rt_event_wait`. Triggering and
waiting stay in generated code (Move C). The runtime owns storage and
bookkeeping only — the same division `rt_intra_event_*` already draws.

Note the JITLink hazard: `native_cb_trampoline`
(`ir/llvm/Scheduler.cpp, awaiter/intra_cb_trampoline`) exists because materialising the *address* of a
runtime symbol is mis-relocated on AArch64 COFF. Any function pointer stored into
a timer or detached-subscription record must therefore be a **JIT-local**
function, never an `extern "C"` runtime symbol.

---

## 11. IR changes

| Op | Today | After |
|---|---|---|
| `EventTrigger` | payload `Signal` (`Ir.h:1538`) | payload `Index` — the resolved handle value |
| `EventTriggerNb` | payload `Signal` (`:1549`) | payload `Index` |
| `EventTriggerNbDelayed` | `op_extra` (`:1555`) | `op_extra`: op = ticks, extra = handle `Index` |
| `EventTriggered` | payload `Signal` (`:1543`) | payload `Index` |
| `Wait` | `WaitInfo` of `WaitEdge{signal, edge}` (`:1532`, `:950`) | `WaitEdge` gains an event variant carrying a handle `Index` |

`SignalInfo::is_event` (`Module.h:57`) survives only to mark that a signal's
64-bit slot holds an event handle, for VCD (§12) and diagnostics. It stops
gating storage allocation, so an event no longer burns a value plane it never
reads — `build_state_types` (`ir/llvm/StateLayout.cpp`) never tested `is_event`
anyway.

The four event tags stay on the `blocks_thunk` list (`Analysis.cpp:124-127`) and
out of the comb-fusion whitelist (`:1426-1429`): a trigger is a scheduling side
effect and `.triggered` reads out-of-model state.

---

## 12. VCD

The VCD layer watches an event's trigger tick and diffs it:

- `vcd_register` takes the event branch with `mm_bytes = 8`, `seen = true` and a
  `0xFF`-seeded shadow (`runtime/Vcd.cpp`);
- the change emits the bare pulse `1<id>` (`runtime/Vcd.cpp, vcd_emit`);
- the declaration is `$var event 1` (`vcd_register`).

The registration row carries the address of the event *variable*. At
`$dumpvars` the runtime reads the handle, points the dump variable at that
object's `last_trig`, stores the dump index in the object (so a trigger reached
through any handle marks the variable dirty) and keeps the object for the run.

Consequences:

1. A dumped variable follows the object it named at `$dumpvars`, not later
   assignments to the variable. Of two dumped variables naming one object, the
   first registered pulses.
2. Only declared events are dumpable. Procedural locals are already skipped by
   VCD (`lower/Lower.cpp, check_vcd_scope` skips `loc == SourceLoc{0}`), and a
   container element or class property is not a `$var` in the first place.
3. `$dumpvars` must still loud-fail on anything unrepresentable. An event handle
   is a real 64-bit value, so the `is_event` check must be kept *ahead* of the
   generic width-based path, or an event would start dumping as a 64-bit vector.

---

## 13. Staging

Each phase is independently landable and independently testable. Phases 0-2 are
prerequisites; 3-5 are parallel.

**Phase 0 — close the current holes.** (landed)
Three real defects exist today, all in the "`is_event` set at only 2 of 18
signal-creating sites" family:

- `declare_local` (`lower/Types.cpp`) and `declare_static_local`
  never set `is_event`, so `event_trig_gv_` stays null and any
  procedural-scope event that is triggered or read **SIGSEGVs the compiler**
  (`clotho build` exits 139). Identical to the package-scope regression recorded
  at `tests/event_test.cpp:213-221`.
- `task automatic t(output event x)` **lints clean and runs**, silently accepted,
  violating PRINCIPLES.md.
- `static event` class properties escape the guard at `lower/Classes.cpp, lower_class_type`
  (static properties `continue` there) and die with the misleading
  `reference to unknown signal 'e'`.

Also give `wait_order` a named message instead of the generic
`default: fail("unsupported statement")` (`:7316-7317`), and open up
`trigger_target` to `HierarchicalValue` — a two-line fix that makes `-> u.e` and
`-> i.e` work and removes the wait/trigger asymmetry, with no handle machinery at
all.

**Phase 1 — the word.** (landed, together with Phase 2) `Type::Kind::Event`, 64-bit plane layout, the runtime slot
table, `rt_event_bind_static`, monomorphic analysis, the constant-folded static
deref. Retarget the existing `EventTrigger`/`EventTriggered` lowering onto the
object while keeping the *bucket* waiter representation. Nothing user-visible
changes; the whole existing `tests/event_test.cpp` must pass unaltered. This is
the risky-but-invisible step and it must land alone.

**Phase 2 — the list.** (landed) Replace the per-event bucket with the intrusive node
list, static nodes in a contiguous module-state array, dynamic nodes in frames.
Delete the event arm of `wait_counts()`. Add `design_uses_event_handles_` to
`ring_guard_wanted()`. Again no user-visible change; the test suite is the
oracle.

**Phase 3 — handles.** (landed, except the aggregate cases) Delete the value/assignment gates, add the shared
`resolve_event` resolver, add refcounting and `rt_elem_event`. This is where
`a = b`, `a = null`, `a == b`, arrays, queues, dynamic and associative arrays,
struct members, class properties, and all four argument directions light up — as
gate deletions, per §4.

**Phase 4 — ports.** (landed) Extend `build_inout_aliases` to `In`/`Out` event ports;
delete `lower/Types.cpp, collect_scope_signals`.

**Phase 5 — the two remaining LRM forms.** (not started) `->> @(x) e` / `->> repeat(n) @(x) e`
via `rt_event_detach_*` (§7.4), and `wait_order` (§8) including the
reactive-region walk at `lower/Lowerer.h, ReactiveSafetyChecker`.

---

## 14. Test plan

Extend `tests/event_test.cpp` in its existing style (`run_capture` for behaviour,
`reject` for loud-fails).

- **Merge/identity:** `a = b` makes `-> a` wake `@(b)`; `a == b` after merge;
  `a == null` before any binding; `a = null` makes `-> a` a no-op and `@(a)`
  block forever; three-way aliasing.
- **Lifetime:** an automatic event in a task called 10 000 times allocates and
  releases without growth; a released handle held past scope exit produces the
  located `rt_event_stale` fatal rather than corrupting; a parked waiter keeps its
  object alive across the owner's scope exit.
- **Aggregates:** the `tests/data_model_test.cpp` `elements × containers × modes`
  matrix gains an event row and must pass every cell the class-handle row passes;
  `-> evq[i]` with a runtime index; `foreach` triggering each element;
  `q2 = q1` sharing objects.
- **Arguments:** `in`/`out`/`inout`/`ref` event formals; a task triggering the
  caller's event; `ref` retargeting the caller's handle variable (mirroring
  `tests/data_model_test.cpp:1199`).
- **Class:** an event property per object — two objects' events are distinct;
  `static event` shared across instances.
- **Ports:** `input`/`output`/`inout` event ports share the object across the
  boundary; interface and modport events trigger from either side.
- **Regression:** every existing Tier-1/2 case unchanged, and the Phase-0 crash
  cases (`begin event e; -> e; end`, task-local, `static` local) as behavioural
  tests, not `reject` tests, since they will then work.
- **Oracle:** iverilog covers `event`, `->`, `@(e)`, `.triggered`, and rejects
  event arrays outright ("sorry: event arrays are not supported"), so merge,
  identity, `->>`, arrays and handle lifetime are pinned to the LRM, as Tier 2
  already is.

---

## 15. Rejected alternatives

**Option A of the old doc — full runtime sync objects with the active-queue push
exposed to the runtime.** Rejected: unnecessary. §2 shows generated code is
already reachable from the runtime via the wheel thunk, so the push stays
codegen-private and the scheduler core is untouched.

**Option B of the old doc — bounded static pool + runtime index, no class
support.** Rejected: it pays the same waiter-list cost for a strict subset, and
the "bounded" pool cannot express automatic-lifetime events created in a loop.
The growable slot table is simpler *and* more general.

**`{ slot, gen }` into a runtime slot table as the handle.** What was first
built, for stale-handle detection under a reference-counted release that never
landed. Replaced by the object's address once events became collected objects:
a collected object is not freed while a handle names it, so the generation had
nothing to detect, and the table was a second lifetime discipline beside the
collector's.

**Keeping the Tier-1 bucket path for monomorphic events and adding a list path
for handles.** Rejected: two event engines, which the old design doc correctly
identified as the trap. §5 gets the performance back through constant folding
instead, and §6.2 gets bucket-like locality by placing static nodes
contiguously.

**Reference counting.** The first design's answer to
`event a; begin automatic event b; a = b; end`, where an owner-based lifetime
leaves `a` dangling. Never built; the collector answers the same case without
retain and release traffic on every handle copy and without a container element
kind that counts.

**A toggling 1-bit wire per event** (the model clocking blocks use,
`src/lower/`). Already rejected in `named-events.md:104-107` and still
right: it leaks a phantom wire into VCD and domain analysis, and gives no
identity, merge, or null.
