# Event handles (LRM 15.5.5)

Target: named events as **real objects with handles** — `a = b` merge, `a = null`,
identity `a == b` / `a == null`, events as subroutine arguments (`in`, `out`,
`inout`, `ref`), event class properties, event ports, and events inside every
aggregate (fixed unpacked array, unpacked struct, queue, dynamic array,
associative array value *and* key).

Status: **largely landed.** Supersedes the "Deferred: event handles" section of
`docs/design/named-events.md`, whose Options A/B are both withdrawn — see
[§2](#2-the-two-recorded-blockers-dissolve).

Built: the event object and its intrusive waiter list (§3, §6), the 64-bit handle
word (§3.1), merge / `null` / identity, event arguments in every direction, event
ports, `static` event class properties, the `->>` pending chain (§7.2), the single
`->> #d` thunk (§7.3), and the monomorphic fast path (§5).

Not built, each loud-failing with a TODO: events inside aggregates and containers,
per-instance event class properties, automatic-lifetime event locals — all of which
need an event object per element / per activation, which the compile-time slot array
cannot size (§9 names the runtime pool that would fix them together) — plus
`@(<event held in a call frame>)` (§6 note), `->> @(x) e` (§7.4) and `wait_order`
(§8).

Two deviations from the design as first written, both measured:

- The object array is **codegen-owned** (`clotho.events`), not a runtime slot table.
  Every object is statically bound today, so the table bought nothing and cost an
  indirection; §9's runtime pool is what the deferred aggregate cases need, and it
  can be added underneath the same handle word without changing anything above it.
  The `gen` half of the handle is therefore reserved, not yet consulted.
- Refcounting is **not** implemented, because nothing is freed yet: every object is
  immortal. It becomes necessary exactly when the runtime pool lands.

---

## 1. Decision summary

Three moves, in dependency order. Everything else follows.

| # | Move | Replaces |
|---|---|---|
| **A** | An event is a **64-bit plane word** — `{ u32 slot, u32 gen }` into a runtime-owned slot table. | `lower_type(EventType) -> logic_type`, a 1-bit placeholder (`Lower.cpp:2116-2122`) |
| **B** | An event's waiters are an **intrusive doubly-linked list of frame-resident nodes**, unbounded. | statically-sized per-`(signal,edge)` buckets `clotho.wait.<sig>.{sta,dyn}.*` for events only |
| **C** | The trigger is **one generated function** `clotho.event_fire(ptr obj, ptr wheel)`, called from generated code *and* from runtime callbacks. | per-event inlined `emit_event_fire` against named globals (`LlvmBackend.cpp:7685`) |

Move A buys every storage class and the whole call ABI for free (§4). Move B
removes the static-capacity blocker (§6). Move C keeps `clotho.active`
codegen-private, so the scheduler core is **not** touched (§2).

The efficiency claim is concentrated in one idea, §5: an event variable that is
never assigned, never `ref`/`out`-passed and never aliased is **monomorphic**;
its handle is a compile-time constant, its object address folds to a constant,
and its lowering is byte-for-byte what Tier 1 emits today. Essentially all RTL
and most testbench events are monomorphic, so generality is paid for only where
it is used.

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
// src/ir/LlvmBackend.cpp:7662-7679  (build_delay_event_thunks)
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
(`LlvmBackend.cpp:5309-5433`) enumerates waiters over the IR at compile time, and
a waiter on `evq[i]` or a `ref event` formal is not enumerable. So the design
does not use buckets for events. It uses an intrusive list, for which the
codebase already contains a complete worked implementation: the remote-`disable`
activation list.

```cpp
// src/ir/LlvmBackend.cpp:21510-21513
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
   (`LlvmBackend.cpp:8112-8114`). Event waiters are parked process/task
   activations already counted in `cap = bit_ceil(nprocs + fork_ring_slots())`
   (`:8001-8003`) and deduped by the scheduled flag, so the bound still holds —
   but the invariant becomes much harder to audit by hand. **This design requires
   `ring_guard_wanted()` to include a new `design_uses_event_handles_`.** Cheap
   insurance against the one failure mode the project forbids: a silent
   out-of-bounds store.
2. **VCD aims a raw pointer at `clotho.event_trig.<name>`** at `$dumpvars` time
   (§12). Moving the tick into an object is an ABI change for that registration.
3. **The wheel callback ABI is fixed at `void(rt_timer*, void* ctx)`** with `ctx`
   globally bound to the wheel by `rt_wheel_advance(wheel, wheel)`
   (`LlvmBackend.cpp:9297`). A per-trigger payload must therefore ride the timer
   node, not `ctx` (§7.3).

---

## 3. Object and handle representation

### 3.1 The handle word

```
handle : i64  =  { u32 slot ; u32 gen }        // slot in low half
null   : i64  =  0                              // slot 0 reserved, gen 0
```

A 64-bit 2-state plane word, identical in every mechanical respect to a class
handle. `Type::Kind::Event` interns as:

```cpp
// src/ir/InternPool.cpp — alongside Kind::Class at 175-182
case Type::Kind::Event: return { Domain::Two, 64, plane_bytes(64) };
```

and `types::storage_class` maps it to `StorageClass::Plane`
(`TypeModel.h:56-58`, whose doc comment at `:22-27` already reads "a class handle
or chandle (a 64-bit word with reference semantics)" — events join that
sentence).

Why `{slot, gen}` and not a bare pointer:

| Property | Bare `rt_event*` | `{slot, gen}` |
|---|---|---|
| `null` == power-on zero (LRM Table 7-1) | yes | yes |
| identity `==` is one `icmp` | yes | yes |
| rides `rt_elem_pod` in containers | yes | yes (with a refcounting variant, §9) |
| **use-after-free** | undefined behaviour | **located fatal** `rt_event_stale` |
| table locality | scattered `rt_alloc` blocks | one contiguous `std::vector<rt_event>` |

The generation half is what makes "strictly managed lifetimes" enforceable rather
than aspirational. Releasing a slot bumps its generation, so every stale handle
is *detected*. The precedent is exact: the fat `ref` ABI already carries
`{ptr, epoch, expect}` (`RefBinding`, `LlvmBackend.cpp:678-682`) to catch a
container reference invalidated by relocation, and `rt_null_handle`
(`Runtime.h:312`) is the located-fatal shape to copy.

### 3.2 The object

```c
// src/runtime/Runtime.h — new
typedef struct rt_event {
    uint64_t last_trig;   // sim tick of the last fire, or UINT64_MAX == never
    void*    waiters;     // intrusive list head (rt_event_node*), or NULL
    uint32_t gen;         // generation of the live occupant of this slot
    uint32_t refs;        // reference count; RT_EVENT_IMMORTAL pins a static event
    uint32_t pending;     // ->> nonblocking-trigger state (bitfield, §7.2)
    uint32_t next_pend;   // slot index of the next event on the NBA pending chain
} rt_event;               // 32 bytes, one cache line holds two
```

`last_trig` is field 0 deliberately: VCD registers `&obj->last_trig` and the
offset must be zero so an `rt_event*` and the tick pointer are the same address
(§12).

The slot table is runtime-owned and growable:

```c
static std::vector<rt_event> g_events;   // g_events[0] is the reserved null slot
static uint32_t              g_event_free;  // freelist head, 0 == empty
```

Growth is amortised `push_back`; a released slot is pushed on the freelist and
its `gen` incremented. Static events occupy slots `1..N` in a deterministic
elaboration order (§5), so their indices are compile-time constants.

### 3.3 Deref

`rt_event* obj = &g_events[slot]` after two checks:

- `slot < g_events.size()` — bounds
- `obj->gen == gen` — liveness

Both fold away for a monomorphic static handle (§5). For a dynamic handle they
are one compare each against a base/length pair the backend loads once per
region, exactly as the container element path does with `dyn_inrange`
(`LlvmBackend.cpp:11373-11441`). Failure calls
`rt_event_stale(const rt_null_site*, uint64_t now)` — a `[[noreturn]]` located
fatal modelled on `rt_null_handle` (`Runtime.cpp:3711`), reusing the existing
`rt_null_site` record and `null_site_global` emitter (`LlvmBackend.cpp:16321`).
A `null` handle triggered or waited on is **not** an error: LRM 15.5.5 makes
`-> null` a no-op and `@(null)` block forever, so the deref path tests slot 0
first and branches to the no-op.

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
| `event a, b; a = b;` | `Lower.cpp:17377` | none — plane store |
| `a = null;` | `Lower.cpp:17377` | peel the `ConversionExpression` slang inserts over the `NullLiteral` |
| `a == b`, `a == null`, `a !== b` | `Lower.cpp:18922-18929` | none — `icmp` via the generic scalar compare |
| `event arr[4];`, `arr[i]` | `Lower.cpp:1847-1854` | none — 8-byte plane element |
| `event q[$]`, `event d[]`, `event aa[string]` | `Lower.cpp:1828` (one predicate, three names) | `rt_elem_event` ops kind (§9) |
| `int aa[event]` (event as key) | the key arm at `Lower.cpp:1929` | none — word key |
| `struct { event e; }` | `Lower.cpp:1971-1977` | none — aggregate leaf slot |
| `class C; event e;` | `Lower.cpp:2238-2242` | none — body field, `agg_member` adds the header word |
| `class C; static event e;` | *no gate exists* — this is the misleading-error hole | route through `declare_static_prop` |
| `task t(event x)` (in) | *no gate exists* — fails downstream as `reference to unknown signal 'x'` | none — scalar `(i128 mm, i64 z)` arg |
| `task t(output/inout event x)` | *no gate exists* — **silently accepted and runs today** | none — `(&mm, &z)` arg |
| `task t(ref event x)` | `Lower.cpp:21391-21395` | none — `ref_actual_ptr` returns `lv.base` for a whole plane slot |
| `function event f()` | — | none — `fn_return_ty` `{ i128, i64 }` |

The `ref` case deserves emphasis, because the current refusal says "there is
nothing for a pointer to alias" — true of a valueless event, false of a handle
word. The fat-ref ABI comment already anticipates this:

> Every storage class goes through here — a plane slot, a managed handle, an
> aggregate image — so `ref` has one shape.
> — `src/ir/LlvmBackend.cpp:18034-18037`

`ref_needs_epoch` is false for a stable place, so a `ref event` costs one pointer
argument and no epoch words.

### 4.1 Ports

Event ports are **aliasing** ports, not copying ports. `input`/`output`/`inout`
all collapse the child's symbol onto the parent's `ir::Signal`, which is what
`build_inout_aliases` (`Lower.cpp:3852-3884`) already does for `inout`/`ref` and
what interface ports get from `canonical_value_symbol` (`Lower.cpp:102-126`):

```cpp
// Lower.cpp:3925-3931 — the existing collapse, reused verbatim for events
if (const auto ia = inout_alias_.find(&m); ia != inout_alias_.end()) {
    sigmap_[&vs] = ia->second;
    break;
}
```

This is both correct and free: LRM 15.5.5 says a port-connected event names the
*same* sync object, and aliasing gives that by construction. Extend
`build_inout_aliases`' direction filter to admit `In`/`Out` when the port type is
an event, keep its existing whole-signal requirement (`:3878-3882`), and delete
`Lower.cpp:3974-3978`. `wire_ports` then needs no event case at all, because an
aliased port is skipped there already (`:6395-6403`).

Consequence to state plainly: an event port creates **no** copy process, so
`-> e` inside the child and `@(e)` in the parent are the same object. That is the
LRM semantics, and it is why the plane-copy port model must not be used here.

---

## 5. The monomorphic fast path

Generality must not cost the common case. Define:

> An event variable is **monomorphic** iff its handle is provably a single
> compile-time-known object for the whole run: it is a static-lifetime
> declaration (module, package, `$unit`, generate block, `static` local, `static`
> class property), it is never the target of an assignment, it is never passed as
> `ref`/`out`/`inout`, and it is not an aggregate element or non-static class
> property.

A one-pass analysis over the AST computes this; it is the same shape as
`facts_.ref_arg` in the existing analysis. Every event in `tests/event_test.cpp`
and every event in ordinary RTL is monomorphic.

For a monomorphic event:

- its slot index is a compile-time constant, assigned during
  `collect_*_signals` in declaration order;
- `rt_event_bind_static(slot)` runs once in PreInit, setting
  `refs = RT_EVENT_IMMORTAL`;
- `&g_events[slot]` folds to `base + slot*32` off a single loaded table base,
  and the generation check is **omitted** (a static slot's `gen` never bumps);
- `-> e` is `store now, [obj+0]` then the list walk;
- `e.triggered` is `icmp eq (load [obj+0]), rt_wheel_now()` — bit-identical to
  today (`LlvmBackend.cpp:7702-7707`);
- **no refcount traffic is emitted at all** (§9).

So the delta for a design that never uses a handle form is: one indirection
through a table base instead of a named global, and the list walk of §6 instead
of a fixed-trip array scan. Both are measurable and both are small; if the table
base load shows up, it is loadable once per function into a register, since the
table only ever grows and never moves the *slots* (see §9 note on reallocation).

Non-monomorphic events pay the full path: load the handle word, split
`{slot, gen}`, bounds+generation check, deref. That is the price of
`evq[i]`/`obj.e`/`ref event`, and it is paid only there.

---

## 6. The waiter list

### 6.1 Node

```
%clotho.event_node = type { ptr next, ptr prev, ptr handle, ptr sched_flag, i32 flags }
```

Fields 2 and 3 are exactly `wait_elem_ty()`'s `{ ptr, ptr }`
(`LlvmBackend.cpp:5438-5441`) — the parked coroutine handle **with its region tag
bits intact** (`kReactiveHomeTag = 2`, `:8248`; `emit_wake_push` is what strips
them, `:8348-8357`) and the opaque `sched_flag` pointer, which is a constant GEP
into `clotho.proc_sched` for a process and a frame alloca for a task
(`:4052`) — store it raw and never interpret it.

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
same reason (`LlvmBackend.cpp:21378-21382`): the woken activation may unlink its
own node. Dynamic nodes are *not* bulk-cleared the way a dynamic bucket is
(`*lenptr = 0`, `:8508-8510`); each waiter unlinks itself on resume, which is
also what makes a multi-event `@(a or b)` wait correct without
`emit_dynamic_remove`'s compaction loop (`:7716-7784`).

Callable from generated code (`-> e`), from the NBA region (`->> e`), and from a
wheel thunk (`->> #d e`) — the three forms converge here, as they converge on
`emit_event_fire` today.

---

## 7. Trigger forms

### 7.1 `-> e` (blocking, LRM 15.5.2)

Resolve the target to an `rt_event*` (§8), then `call @clotho.event_fire(obj, wheel)`.
For a monomorphic target this inlines to today's code plus the list walk.

### 7.2 `->> e` (nonblocking, LRM 15.5.4)

Today the NBA region scans a compile-time set `nb_event_sigs_`
(`LlvmBackend.cpp:2462-2481`, consumed at `:8682-8697`), which a runtime-identified
event cannot join. Replace with an **intrusive pending chain** through
`rt_event::next_pend`:

- `->> e` sets `obj->pending` and, if it was clear, links the slot onto a runtime
  head `g_event_pending`.
- The NBA region walks the chain, calling `clotho.event_fire` per entry, clearing
  `pending`, and emptying the chain.
- `emit_nba_any_staged` (`:8586-8606`) gains `g_event_pending != 0`; `has_nba`
  (`:8871-8873`) gains "the design contains any `->>`".

This is strictly better than the status quo: the NBA region visits exactly the
events that fired, not every `->>` target in the design.

### 7.3 `->> #d e` (delayed detached, landed)

The timer node grows a payload and the per-event thunk collapses to one:

```c
typedef struct rt_event_timer { rt_timer base; uint64_t handle; } rt_event_timer;
```

`clotho.ev_delay(rt_timer* t, void* ctx)` reads `t->handle`, resolves it, calls
`clotho.event_fire(obj, ctx)` — `ctx` is the wheel, per
`rt_wheel_advance(wheel, wheel)` (`:9297`) — and `rt_free`s the node. This
replaces `build_delay_event_thunks`' one-function-per-event
(`:7662-7679`) with one function per design, and removes the `nb_delay_event_sigs_`
static set for the same reason as §7.2.

The follow-up already flagged in `named-events.md:202` (a per-event freelist for
hot `->> #d` loops) becomes a single design-wide freelist, since the node is now
type-uniform.

### 7.4 `->> @(x) e` and `->> repeat(n) @(x) e`

Now expressible, and the reason it was deferred is gone. `x` is an ordinary
*signal*, whose buckets are statically sized — so do **not** put the detached
subscription in `x`'s bucket. Use a runtime-owned growable list keyed by the
existing `trigger_id(sig, edge)` (`LlvmBackend.cpp:7395`), which is precisely the
shape of the already-shipping intra-assignment mechanism:

> `rt_intra_event_register(trig_id, rec)` / `rt_intra_event_fire(trig_id)`
> (`Runtime.cpp:2496-2516`) — a `std::vector<void*>` per trigger id, compacted in
> place.

Add `rt_event_detach_register(trig_id, handle, count)` and have
`service_wait_queue` call a generated `clotho.detached_fire(trig_id)` after the
static and dynamic bucket scans, for the signals that have any such
registration. `repeat(n)` is the record's countdown. No new mechanism, no
capacity risk, no scheduler-core change.

---

## 8. Resolving a target — one shared resolver

The current asymmetry is a two-line accident: `event_signal`
(`Lower.cpp:6877-6926`) accepts `NamedValue | HierarchicalValue`, while
`trigger_target` (`:7079-7087`) accepts only `NamedValue`. That is why `@(s.e)`
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
`lower_expr`/`lvalue_of` machinery, once `Lower.cpp:18922-18929` stops refusing
to produce an event value. `.triggered` needs no change beyond that:
`triggered_event` (`:7117-7129`) already pulls `arguments()[0]` generically for
every receiver shape.

`wait_order(a, b, c)` (LRM 15.5.6) becomes expressible for the first time: park a
node on all three objects, and on each wake compare the firing object against the
expected next; a mismatch takes the `ifFalse` arm. Note it also needs the
reactive-region walk to stop hard-rejecting it (`Lower.cpp:883-891`), so it is
scoped as a follow-on (§13, Phase 5), not part of the core.

---

## 9. Lifetime

Refcount, with the common case emitting nothing.

| Object origin | Lifetime rule |
|---|---|
| Static declaration (module / package / `$unit` / generate / `static` local / `static` class property) | bound in PreInit with `refs = RT_EVENT_IMMORTAL`; never released; **no refcount traffic emitted** because every reference is monomorphic |
| Automatic declaration (per subroutine call, per block entry) | `rt_event_alloc()` in the prologue, `rt_event_release()` in the epilogue |
| Aggregate element | created lazily on first write/subscribe; released by the container's element deinit |
| Non-static class property | created by `emit_class_property_inits`; released never, because class bodies leak by direction (`docs/design/classes.md:299-303`) — consistent, and documented as such |
| Parked waiter | holds a reference for the duration of the park: incref on link, decref on unlink |

The waiter reference is what makes this sound: an object with parked waiters
cannot reach zero, so `clotho.event_fire` can never run against a freed object.

Containers need one new element-ops kind. `rt_elem_ops` already carries a
per-kind copy/deinit protocol (`Runtime.h:889-906`, `rt_elem_pod = 0` being "raw
bytes; proto init, memcpy copy, no deinit"), and a class handle uses the POD kind
precisely because class objects never die. An event element takes a new
`rt_elem_event`: proto = all-zero (so `null` is the power-on default LRM Table 7-1
asks for, for free), copy = incref-then-word-copy, deinit = decref. `q2 = q1`
then shares objects and refcounts correctly.

Where refcount traffic is emitted at all, it is `add`/`sub` on
`obj->refs` plus a zero test on the decrement — no atomics; the simulator is
single-threaded through the scheduler.

Two notes, stated because they are the sharp edges:

- **`refs` reaching zero bumps `gen`.** Any handle word still holding the old
  `{slot, gen}` therefore fails its generation check and produces a located
  fatal instead of silently aliasing the slot's next occupant. This is the
  "strictly managed" guarantee, and it converts a whole class of refcount bugs
  from memory corruption into a diagnosable error.
- **`g_events` reallocation must not invalidate an `rt_event*` held across a
  suspend.** Either resolve handles freshly at every use (the design's default —
  a deref is two compares and an add) or make the table a deque of stable
  chunks. Prefer the former; do not cache `rt_event*` across a coroutine
  suspend.

Allocation goes through the **checked** family (`rt_alloc_checked`,
`Runtime.h:467`), not the raw `rt_alloc` that `ClassNew` uses unchecked
(`LlvmBackend.cpp:11688`), so OOM is a located fatal rather than a null deref.

---

## 10. Runtime ABI additions

All must be added to `expose()` in `src/ir/Jit.cpp:199-303` to be linkable, and
every struct the backend mirrors must keep field order in lockstep with the C
declaration — the convention `rt_null_site` follows
(`LlvmBackend.cpp:16311-16313`: "Field order and types mirror the C struct
exactly, so the two change together").

```c
// slot table
uint64_t rt_event_alloc(void);                 // -> {slot,gen}, refs = 1
void     rt_event_bind_static(uint32_t slot);  // PreInit; refs = RT_EVENT_IMMORTAL
void     rt_event_retain(uint64_t h);
void     rt_event_release(uint64_t h);
void*    rt_event_table_base(void);            // for constant-folded static deref
uint32_t rt_event_table_len(void);
[[noreturn]] void rt_event_stale(const rt_null_site* site, uint64_t now);

// ->> pending chain
uint32_t rt_event_pending_head(void);
void     rt_event_pending_link(uint64_t h);
void     rt_event_pending_clear(void);

// ->> @(x) detached subscriptions (mirrors rt_intra_event_*)
void     rt_event_detach_register(int32_t trig_id, uint64_t h, uint32_t count);
void*    rt_event_detach_base(int32_t trig_id);
uint32_t rt_event_detach_count(int32_t trig_id);
```

Deliberately **absent**: any `rt_event_trigger` / `rt_event_wait`. Triggering and
waiting stay in generated code (Move C). The runtime owns storage and
bookkeeping only — the same division `rt_intra_event_*` already draws.

Note the JITLink hazard: `native_cb_trampoline`
(`LlvmBackend.cpp:4373-4409`) exists because materialising the *address* of a
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
reads — `build_state_types` (`LlvmBackend.cpp:2064-2263`) never tested `is_event`
anyway.

The four event tags stay on the `blocks_thunk` list (`Analysis.cpp:124-127`) and
out of the comb-fusion whitelist (`:1426-1429`): a trigger is a scheduling side
effect and `.triggered` reads out-of-model state.

---

## 12. VCD

Today the VCD layer aims a raw pointer at the trigger tick and diffs it:

- `vcd_register` takes the event branch with `mm_bytes = 8`, `seen = true` and a
  `0xFF`-seeded shadow (`Runtime.cpp:6017-6025`);
- `VcdVar::mm` points at `clotho.event_trig.<name>` and "the ordinary shadow diff
  detects a trigger" (`Runtime.cpp:5416-5420`);
- the change emits the bare pulse `1<id>` (`Runtime.cpp:5687-5697`);
- the declaration is `$var event 1` (`:6039`).

This survives with one substitution: register `&g_events[slot].last_trig` instead
of the named global. That is why `last_trig` is field 0 of `rt_event` — the
object address *is* the tick address, so `rt_vcd_var`'s raw-pointer ABI
(`Runtime.h:715`) is unchanged and `rt_vcd_touch`'s dense `u32` index
(`:736`) is unchanged.

Two constraints follow, both acceptable:

1. Only **static** events are dumpable, so only immortal slots are registered and
   the pointer is stable for the run. Procedural locals are already skipped by
   VCD (`Lower.cpp:22933` skips `loc == SourceLoc{0}`), and a container element or
   class property is not a `$var` in the first place.
2. `$dumpvars` must still loud-fail on anything unrepresentable. An event handle
   is now a real 64-bit value, so the `is_event` check must be kept *ahead* of the
   generic width-based path, or an event would start dumping as a 64-bit vector.

---

## 13. Staging

Each phase is independently landable and independently testable. Phases 0-2 are
prerequisites; 3-5 are parallel.

**Phase 0 — close the current holes.** (landed)
Three real defects exist today, all in the "`is_event` set at only 2 of 18
signal-creating sites" family:

- `declare_local` (`Lower.cpp:4069-4085`) and `declare_static_local`
  (`:20779-20793`) never set `is_event`, so `event_trig_gv_` stays null and any
  procedural-scope event that is triggered or read **SIGSEGVs the compiler**
  (`clotho build` exits 139). Identical to the package-scope regression recorded
  at `tests/event_test.cpp:213-221`.
- `task automatic t(output event x)` **lints clean and runs**, silently accepted,
  violating the project's never-silently-accept rule.
- `static event` class properties escape the guard at `Lower.cpp:2238-2242`
  (static properties `continue` at `:2231-2236`) and die with the misleading
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
delete `Lower.cpp:3974-3978`.

**Phase 5 — the two remaining LRM forms.** (not started) `->> @(x) e` / `->> repeat(n) @(x) e`
via `rt_event_detach_*` (§7.4), and `wait_order` (§8) including the
reactive-region walk at `Lower.cpp:883-891`.

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

**A bare `rt_event*` as the handle.** Rejected: gives up use-after-free detection
and table locality for nothing. The generation half of the word is free — the
handle is 64 bits either way.

**Keeping the Tier-1 bucket path for monomorphic events and adding a list path
for handles.** Rejected: two event engines, which the old design doc correctly
identified as the trap. §5 gets the performance back through constant folding
instead, and §6.2 gets bucket-like locality by placing static nodes
contiguously.

**Refcount-free owner-based lifetimes.** Tempting, because the intrusive
frame-resident waiter list means an event object owns no external heap, so a
leaked event is as invisible as a leaked class body. Rejected because
`event a; begin automatic event b; a = b; end` then leaves `a` dangling with no
defined behaviour, and containers of events cannot express shared ownership at
all. Refcount is the only sound answer once merge exists — and §5 makes it free
for every event that does not use merge.

**A toggling 1-bit wire per event** (the model clocking blocks use,
`Lower.cpp:12632`). Already rejected in `named-events.md:104-107` and still
right: it leaks a phantom wire into VCD and domain analysis, and gives no
identity, merge, or null.
