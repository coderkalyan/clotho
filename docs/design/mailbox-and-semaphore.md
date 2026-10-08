# `std::semaphore` and `std::mailbox` (LRM 15.3, 15.4)

Target: the two built-in synchronization classes slang synthesizes into the `std`
package — `semaphore` (`new`, `put`, `get`, `try_get`) and `mailbox #(T)` (`new`,
`num`, `put`, `try_put`, `get`, `try_get`, `peek`, `try_peek`).

Status: **landed, in the shape scoped below.** `semaphore` and `mailbox #(T)` are
implemented end to end (P0-P3 of §13), with 22 cases in `tests/sync_test.cpp`; the
typeless mailbox (§8, P4) is the one piece still refused, by its own message. The
two claims this document got wrong before it was implemented are marked **measured
correction** where they appear (§6.2, and the `require_head` split in §6.2's third
bullet); everything else was built as written.

This document is written after `process` (LRM 9.7) landed, and it deliberately
copies that document's shape. The conclusion, however, is the opposite one: where
`process` needed a new native structure, a new ABI argument, three new IR tags and
a design-wide gate, **these two classes need none of that**. Every part except the
park/wake primitive already exists and is tested:

- a **parameterized class** is already an ordinary nominal class, one `ir::Ref` per
  specialization (`src/Lower.cpp:2578-2585`; four green tests at
  `tests/class_test.cpp:910`, `:934`, `:951`, `:971`), so `mailbox #(T)` needs no
  type machinery whatsoever;
- an **unbounded queue of T** already carries every payload the LRM admits, with a
  runtime that is element-type agnostic (`rt_elem_ops`, `src/runtime/Runtime.h`
  `[gc]`:947-958) and a frontend admission gate that already enumerates the legal
  element set (`guard_container_element`, `src/Lower.cpp:2180-2220`);
- a **class-property container is already traced precisely** by the collector
  through its owning object's map, and a class handle inside a queue is
  supported, not refused (`class_elem_leaves` / `clotho.classgc.<Ref>`,
  `src/ir/LlvmBackend.cpp` `[gc]`:2580-2695; `tests/gc_test.cpp` `[gc]`:1022);
- an **intrusive waiter list rooted in a heap object**, with frame-resident nodes, a
  scheduled-flag dedupe and self-unlink on resume, is the landed named-event
  machinery (`emit_event_link` / `emit_event_unlink` / `emit_event_wake_list`,
  `src/ir/LlvmBackend.cpp:8691-8800`);
- a **bodyless built-in method** already has an interception seam in the frontend
  (`lower_process_method` reached from `lower_user_call`, `src/Lower.cpp:23212-23219`)
  and a synthetic class body already has one (`lower_process_class`, `:2679-2692`).

So the work is: two synthetic class bodies, one new frame-resident node shape with
three small emitters, two IR tags, and the frontend expansion of ten methods. No new
runtime symbol, no new pool, no ABI change, no wheel change, no `DesignUses` flag.

---

## 1. Semantics (LRM 15.3, 15.4)

slang synthesizes both classes in
`build-asan/_deps/slang-src/source/ast/builtins/StdPackage.cpp` — `semaphore` at
`:68-90` (an ordinary `ClassBuilder`) and `mailbox` at `:92-136` (an
`allocGenericClass` whose parameter `T` defaults to `c.getType(SyntaxKind::Untyped)`,
`:132-133`). Every method is a bodyless prototype carrying
`MethodFlags::BuiltIn`, and both `new`s additionally carry
`MethodFlags::Constructor`:

```systemverilog
class semaphore;
  function new(int keyCount = 0);
  function void put(int keyCount = 1);
  task get(int keyCount = 1);
  function int try_get(int keyCount = 1);
endclass

class mailbox #(type T = <untyped>);
  function new(int bound = 0);
  function int num();
  task put(T message);          function int try_put(T message);
  task get(ref T message);      function int try_get(ref T message);
  task peek(ref T message);     function int try_peek(ref T message);
endclass
```

The LRM sentences that shape the design, quoted from IEEE 1800-2017 §15.3-15.4,
in the order they constrain it:

- **Keys are unbounded above.** 15.3.1: *"The number of keys in the bucket can
  increase beyond `keyCount` when more keys are put into the semaphore than are
  removed."* So the count is not clamped to the constructor argument.
- **`put` hands off.** 15.3.2: *"If a process has been suspended waiting for a key,
  that process shall execute if enough keys have been returned."*
- **The semaphore queue is strictly FIFO.** 15.3.3: *"The semaphore waiting queue is
  first-in first-out (FIFO). This does not guarantee the order in which processes
  arrive at the queue, only that their arrival order shall be preserved by the
  semaphore."* Accellera's own interpretation of this sentence
  (`SemaphoreExamples.pdf`, "Interpretation #1") spells out the consequence: *"a
  blocked semaphore will block subsequent semaphore requests until the first
  semaphore's request is satisfied, even if there are enough keys to immediately
  satisfy the request from a later semaphore request."* This is head-of-line
  blocking, and it is the single most load-bearing rule in this document (§6.1).
  **Verilator 5.050 does not implement it** — measured, §14.
- **`try_get` is availability-only.** 15.3.4: *"If the specified number of keys is
  available, the method returns a positive integer... If the specified number of
  keys is not available, the method returns 0."* No mention of the queue.
- **A mailbox is a bounded or unbounded FIFO of messages.** 15.4: *"A process that
  attempts to place a message into a full mailbox shall be suspended until enough
  room becomes available in the mailbox queue. Unbounded mailboxes never suspend a
  thread in a send operation."*
- **A negative bound is illegal.** 15.4.1: *"The bound shall be positive. Negative
  bounds are illegal and can result in indeterminate behavior, but implementations
  can issue a warning."*
- **`num()` is a snapshot.** 15.4.2: *"valid only until the next `get()` or `put()`
  is executed on the mailbox."*
- **Messages are stored in strict FIFO order** (15.4.3, 15.4.4) and **the mailbox
  waiting queue is FIFO** in the same words as the semaphore's (15.4.5).
- **`peek` does not consume, and unblocks broadly.** 15.4.7: *"Calling the `peek()`
  method can also cause one message to unblock more than one process. As long as a
  message remains in the mailbox queue, any process blocked in either a `peek()` or
  `get()` operation shall become unblocked."*
- **Type checking is a run-time obligation of the typeless mailbox only.** 15.4.5:
  *"If the type of the message variable is not equivalent to the type of the message
  in the mailbox, a run-time error is generated. Nonparameterized mailboxes are
  typeless... a mailbox implementation shall maintain the message data type placed
  by `put()`. This is required in order to enable the run-time type checking."*
  15.4.6/15.4.8 make the non-blocking forms return **a negative integer** in that
  case. For a `mailbox #(T)` slang has already type-checked the actual, so neither
  the run-time error nor the negative return is reachable — §8.

Neither class is `abstract` or `final` (unlike `process`), so `new` is legal and
slang admits `class my_mbx extends mailbox #(int);`. Derivation is refused in the
first cut (§10, hazard 9).

---

## 2. Current state — what exists, what does not

Facts, all established against the code, in the order the design needs them.

**The refusal.** `lower_class_type` (`src/Lower.cpp:2544`) fires, immediately after
the cache probe, on any class whose parent scope is package `std`, with `process`
now diverted (`:2565-2571`):

```cpp
if (const slang::ast::Scope* parent = clt.getParentScope();
    parent != nullptr && parent->asSymbol().kind == SymbolKind::Package && parent->asSymbol().name == "std") {
    if (clt.name == "process") { return lower_process_class(clt); }
    fail("the built-in '" + std::string(clt.name) + "' class is not supported");
}
```

`mailbox #(int)` carries the name `mailbox`, so a specialization produces the same
message (`tests/class_test.cpp:1234-1235`). Because the guard is type-level it fires
on a declaration, on a class property, and on a task formal (`:1239-1243`).

**Parameterized classes are done.** slang monomorphizes each `C#(args)` into a
distinct fully substituted `ClassType`; clotho lowers each as an independent nominal
class keyed by symbol pointer in `class_type_cache_`, with its own `fields_id`, own
`FunctionId`s and own statics (`src/Lower.cpp:2578-2585`). So `mailbox #(int)` and
`mailbox #(pkt)` are already two unrelated types with two class descriptors, and
`$cast` between them already fails (`tests/class_test.cpp:1739`).

**A queue already carries every payload the LRM's `singular` admits, and more.**
`rt_queue` is a 40-byte caller-owned handle `{ base, head, len, cap, epoch }`
(`src/runtime/Runtime.h` `[gc]`:1083-1097) embedded directly in design state, a frame
slot, or a class body; it owns one `malloc`'d ring, and every element operation goes
through one `const rt_elem_ops*` descriptor the backend materializes
(`elem_ops_for`, `src/ir/LlvmBackend.cpp:1464`). Seven element kinds exist (pod,
string, dynarr, queue, aggregate, assoc, event). `rt_queue_push_back` /
`rt_queue_pop_front` already relocate a managed element into and out of the ring, and
`q[0]` is inline ring math, not a call. The frontend gate that decides the legal
element set is `guard_container_element` (`src/Lower.cpp:2180-2220`), whose only
failure is `"<what> element type is not supported"`; it admits integral, real,
string, class handle, `chandle`, unpacked struct / fixed unpacked array,
nested queue / dynamic array / associative array, and event.

**Two facts about the queue bound that the mailbox must not inherit.** The bound is
a property of the *interned Queue type*, not of the push site (`QueueArg`,
`src/ir/Ir.h:350-360`), and an overflowing push on a bounded queue **silently drops
the element from the opposite end** (`Ir.h:1237-1239`; `q_apply_bound_ops`,
`src/runtime/Runtime.cpp` `[gc]`:1556-1565). A mailbox bound is a *blocking*
condition, so the FIFO must be an **unbounded** queue type with the bound enforced
in emitted code (§6.2).

**An intrusive waiter list on a heap object is landed machinery.** A named event's
object is `{ last_trig, waiters, pending, vcd, next_pend }`
(`src/runtime/Runtime.h:1405-1411`), its waiter node is the frame-resident
`event_node_ty` = `{ next, prev, handle, flag, owner }`
(`src/ir/LlvmBackend.cpp:6047`, field constants `:6062-6070`), and the three
emitters are `emit_event_wake_list` (`:8691`), `emit_event_link` (`:8738`) and
`emit_event_unlink` (`:8772`). The disciplines that matter, all already paid for:

- the node is an alloca placed right after `coro.begin` so CoroSplit spills it into
  the coroutine frame (`event_wait_node`, `:22848`);
- the wake walk reads `next` **before** waking, because a woken activation unlinks
  its own node as soon as it runs (`:8741` region);
- the wake is deduped by the parked thread's scheduled flag, so one thread is pushed
  at most once per instant;
- the waiter **always** unlinks itself on resume (`lower_wait`, `:5228-5235`), which
  is what makes a multi-event wait correct with no compaction pass;
- `kNodeOwner` exists so unlink is correct even if the variable naming the object was
  reassigned while the thread was parked.

**`process` supplies the abort and suspend contracts, and one of them is unpaid.**
The abort poll is emitted by `lower_region` after each suspending op
(`src/ir/LlvmBackend.cpp:11806-11816`), gated on `design_uses_disable_` /
`design_uses_process_` and on `tag_suspends(t)` (`:23022-23033`). There is **no
general "unlink me from whatever I am waiting on" hook**: cleanup is per-mechanism,
and correctness rests on the poll being emitted *after* the parking op's own
resume-side unlink. `tag_suspends` today lists only `Delay`, `Wait`, `TaskCall`,
`TaskCallVirtual`, `Fork`, `ExpectWait` — **`ProcWait` is missing**, which is a live
defect in the landed process work (§16).

**The collector already traces a container inside a class body.** `[gc]` A class
property container is deliberately *not* a container root; it is reached through the
owning object's `clotho.classgc.<Ref>` leaf table (`class_elem_leaves`,
`src/ir/LlvmBackend.cpp` `[gc]`:2580-2602, rationale `:2554-2579`), and
`clotho.classfini.<Ref>` releases it when the object dies (`:2620-2657`, with the
hard constraint that a finalizer runs during a collection and must not allocate).
`rt_elem_ops` carries `nhandles`/`handles` so the queue walker marks handle payloads
(`walk_queue`, ring-wraparound aware, `src/runtime/Gc.cpp` `[gc]`:1467-1487).
`tests/gc_test.cpp` `[gc]`:1022 already covers `class Holder; Cell q[$]; endclass`.

**What does not exist.** Nothing in `src/runtime/*` for either class, no FIFO
wait-queue node shape (the event node has no tail pointer and is push-front, LRM
4.7 leaving event wake order arbitrary), and no frontend expansion of a bodyless
built-in method that needs *control flow* — every arm of `lower_process_method`
(`src/Lower.cpp:23142-23204`) is a single op with a fixed operand array, and none
takes a `ref` formal.

---

## 3. Decision summary

| # | Move | Why |
|---|---|---|
| **A** | **Both objects are ordinary GC class objects with a synthetic body.** `semaphore` = `{ i64 keys, chandle q_head, chandle q_tail }`; `mailbox #(T)` = `{ i64 bound, T fifo[$], chandle get_head/get_tail, chandle put_head/put_tail, chandle peek_head/peek_tail }`. Registered by hand in `lower_class_type`, exactly as `lower_process_class` does. | Every property type is already supported, so allocation, `null`, `==`, `HandleDeref`, the trace map, the finalizer, `--gc off` leak behaviour and `$cast` all come free. §12 rejects the runtime-pool alternative on a specific, measured ground: a runtime-owned object holding an `rt_queue` is **invisible to the collector** today. |
| **B** | **The payload FIFO is an unbounded queue property**, and every non-blocking mailbox method expands in the frontend into the queue tags that already exist (`QueueSize`, `QueuePushBack`, `QueuePopFront`, `ElemSelect`). The mailbox bound is tested in emitted IR. | The whole payload problem — strings, handles, aggregates, nested containers, relocation, deep copy, deinit, GC tracing — is already solved once, in the container path. A bounded *queue type* would silently drop a message instead of blocking (§2). |
| **C** | **One new frame-resident node shape** `%clotho.sync_node = { next, prev, handle, flag, q_head, q_tail }`, with three emitters modelled line-for-line on the event ones, differing in exactly two ways: **enqueue is at the tail** (FIFO), and the node **stays linked across a spurious wake**. | LRM 15.3.3/15.4.5 require arrival order to be preserved; re-linking at the tail on every unsatisfied retest would not preserve it. The node carries both queue-header pointers so unlink can fix the tail without assuming body layout. |
| **D** | **Two IR tags**, appended at the end of the enum: `SyncWait` (suspending) and `SyncCtl` (statement: `Leave`, `WakeHead`, `WakeAll`). The retest condition is ordinary frontend IR; the park/link/unlink/abort discipline is backend-owned. | The backend cannot evaluate a frontend condition and the frontend cannot reach the abort exit; splitting there is the only seam that keeps both correct. Appending, not inserting, is required: a mid-enum insert renumbers later tags and breaks `tests/sva_test.cpp`'s `monitor_shape` digests (`disable.md:803-807`). |
| **E** | **Head-of-line admission.** A consuming op (`semaphore::get`, `mailbox::get`, `mailbox::put` on a bounded mailbox) takes its fast path only when its wait queue is empty; a woken waiter proceeds only as the head. `peek` is exempt — it consumes nothing, so it proceeds whenever `num() > 0`. | This is 15.3.3 read with Accellera's Interpretation #1, and the `peek` exemption is what makes 15.4.7 ("any process blocked in either a `peek()` or `get()` shall become unblocked") and 15.4.5 (FIFO) both true at once. |
| **F** | **`SyncWait` joins `tag_suspends`, and `SyncWait`/`SyncCtl` join `blocks_thunk`.** The op unlinks its node on the abort-pending resume path and then lets the existing poll take the exit. | Without the `tag_suspends` entry a thread blocked in `mailbox::get()` is *unkillable* — the exact hole `ProcWait` has today (§16). Everything else about kill / `disable` / `suspend` / `resume` interop then falls out of machinery that is already tested. |
| **G** | **No `DesignUses` flag, no gate widening.** All emitted code sits at op sites and in the two synthetic bodies, so a design that names neither class emits zero extra bytes structurally — the way events are already free (`event_slot_count_ == 1` ⇒ no pool init). `ring_guard_wanted()` and `park_claim_wanted()` are unchanged. | A wake pushes at most one entry per thread (scheduled-flag dedupe) and every thread is counted in `bit_ceil(nproc + fork_ring_slots)`, so the ring bound argument from `disable.md:682-685` holds unchanged. Nothing here force-wakes a `#delay` park, so no claim word is needed. |
| **H** | **The typeless mailbox (LRM 15.4.9) stays refused**, by its own message, and is a separate phase with its own design (§8). `mailbox #(T)` lands first. | It needs a dynamically typed element — a per-slot descriptor and a run-time type identity — which is a new `rt_elem` kind and a new notion of type equality. Nothing else in this document depends on it. |

---

## 4. The objects

### 4.1 `semaphore`

```
fields (built by hand in lower_semaphore_class, mirroring lower_process_class:2679)
  0  keys    : i64, 2-state      ; key count; i64 because 15.3.1 lets it grow
  1  q_head  : i64, 2-state      ; chandle-shaped: head of the FIFO wait queue
  2  q_tail  : i64, 2-state      ; chandle-shaped: tail of the same queue
```

`keys` is 64-bit even though `keyCount` is an `int`: 15.3.1 explicitly allows the
count to exceed the constructor argument, and a 32-bit accumulator would wrap in a
long UVM run of unbalanced `put`s. The exposed arithmetic stays `int`-typed at the
call sites, so `try_get` still returns a 32-bit `int`.

Both queue words are `chandle`-shaped 2-state i64, not class handles, so
`collect_class_handles` skips them and the collector never sees a coroutine frame
pointer (the property `process` bought the same way, `process-class.md:242-247`).

### 4.2 `mailbox #(T)`

```
fields (one body per specialization, keyed by ClassType pointer as usual)
  0  bound      : i64, 2-state   ; 0 == unbounded (LRM 15.4.1)
  1  fifo       : Queue<T>       ; UNBOUNDED queue type — the message FIFO
  2  get_head   : i64            ; consuming waiters (get)
  3  get_tail   : i64
  4  put_head   : i64            ; space waiters (put on a bounded mailbox)
  5  put_tail   : i64
  6  peek_head  : i64            ; non-consuming waiters (peek)
  7  peek_tail  : i64
```

Three queues rather than one list with a kind tag on the node: it keeps the node
shape uniform, keeps every wake walk branch-free, and makes the `peek` exemption
(decision E) a property of *which list you are on* rather than a run-time test.
A mailbox is one object per testbench channel, not one per transaction, so six
words of header are not a cost worth optimizing.

`T` is admitted by `guard_container_element(T, ..., "mailbox")` — reusing the gate,
and its diagnostic, verbatim. That admits a superset of LRM 15.4.3's `singular`
(an unpacked struct or fixed unpacked array is admitted, and LRM's `singular`
excludes it). Deliberate: the queue path supports it, refusing it would invent a
restriction the LRM does not impose, and it is recorded in §15.

### 4.3 Construction

`new` is a bodyless `MethodFlags::Constructor`, so it cannot go through
`emit_ctor_call` (`src/Lower.cpp:22927`), which would call `function_id(*ctor)` →
`build_function` → `lower_stmt` on a prototype and die on an out-of-range formal
index — the documented failure mode at `:2548-2564`. `lower_new_class`
(`:22902`) therefore gains one intercept before that call:

1. `ClassNew` allocates and power-on-inits the body — all-zero is exactly the right
   image (`keys = 0`, empty queue handle, null queue heads);
2. store the constructor argument: `keys` from `keyCount`, or `bound` from `bound`.
   slang materializes a defaulted actual, so `new` and `new(0)` are the same lowering;
   the arm asserts arity rather than assuming it, because `ExpressionKind::EmptyArgument`
   placeholders do occur elsewhere (`src/Lower.cpp:11099`, `:16770-16772`);
3. `m = new;` with no parentheses has a null `constructorCall()`; the arm uses the
   LRM default (0) rather than reading `arguments()`;
4. a **negative** `bound` (15.4.1) or a negative `keyCount` is a **located fatal**,
   modelled on `rt_null_handle`. The LRM makes a negative bound "illegal... can
   result in indeterminate behavior" and permits a warning; clotho never silently
   accepts, so it reports. A *non-constant* negative value needs a run-time test, so
   the check is emitted, not folded — one compare on a path that runs once per object.

### 4.4 Lifetime, `--gc`, and `--gc off`

An object's body holds one managed property, the FIFO, so `[gc]`:

- `clotho.classgc.<mailbox#(T)>` gets a leaf entry for `fifo` from
  `class_elem_leaves`, and the collector walks the ring precisely through the
  element descriptor — a `mailbox #(pkt)` keeps its queued objects alive with **no
  new GC code**;
- `clotho.classfini.<mailbox#(T)>` is non-null (the field has a heap leaf) and
  releases the ring via the ordinary `emit_agg_deinit` walk. It allocates nothing,
  which is the finalizer's hard constraint;
- `semaphore` has no managed property, so its finalizer is null and its trace map
  has zero handle offsets — tracing one costs a map lookup.

Under `--gc off`, both leak, bounded by the number of `new`s, exactly as every class
body already does (`LlvmBackend.cpp:12965-12968`'s recorded trade). The FIFO's ring
leaks with the body, which is a real per-transaction leak for a design that allocates
mailboxes in a loop; it is the same trade, and it is stated in the TODO entry.

**A parked thread keeps its object alive.** A waiter's frame holds the handle it
called through, and a suspended frame is registered and scanned conservatively
(`rt_gc_frame_link`, `Gc.h` `[gc]`:325-326), so an object cannot be collected out
from under its own wait queue.

---

## 5. The wait queue

### 5.1 The node

```
%clotho.sync_node = type {
  ptr next, ptr prev,   ; intrusive, doubly linked, FIFO (head = earliest arrival)
  ptr handle,           ; the parked coroutine handle, REGION TAG INTACT (parked_handle)
  ptr flag,             ; the parked thread's scheduled-flag address; never interpreted
  ptr q_head, ptr q_tail ; the two header words this node is linked into
}
```

Six words, versus the event node's five (`LlvmBackend.cpp:6047`). `handle` and
`flag` are the same two words as `wait_elem_ty`, for the same reasons. `q_head` /
`q_tail` replace `kNodeOwner`: they serve the identical purpose (unlink correctly
after the *variable* naming the object was reassigned mid-park) and additionally let
unlink repair the tail pointer without assuming the two header fields are adjacent
in the body layout.

Storage: one alloca per wait site index, placed immediately after `coro.begin` so
CoroSplit spills it into the frame — `event_wait_node`'s mechanism (`:22848-22860`),
shared by index because a coroutine is suspended at one wait at a time.

### 5.2 The three emitters

- `emit_sync_enqueue(node, q_head_ptr, q_tail_ptr, handle, flag)` — **push back**:
  store `handle`/`flag`/`q_head`/`q_tail` into the node, `next = null`,
  `prev = *q_tail`, then either `(*q_tail)->next = node` or `*q_head = node`, then
  `*q_tail = node`. Idempotent guard: if `node->q_head != null` the node is already
  linked and enqueue is a no-op — this is what preserves arrival order across a
  spurious wake (decision C).
- `emit_sync_unlink(node)` — the event unlink plus the tail repair: patch
  `prev->next` / `*q_head`, patch `next->prev` / `*q_tail`, then null `next`, `prev`
  **and `q_head`**, so a second unlink is a harmless no-op and the enqueue guard
  reads "unlinked" again.
- `emit_sync_wake(q_head_ptr, all)` — walk from the head; per node read `next`
  **before** waking, load `handle` and `flag`, and push only if `*flag == 0`, then
  set it — `emit_event_wake_list` verbatim (`:8691-8735`), except that `all == false`
  stops after the first node. Nothing is unlinked by the waker.

Nothing here is new in kind. The one genuinely new rule is the enqueue guard, and
it exists because FIFO is a spec requirement here where LRM 4.7 leaves event wake
order arbitrary.

### 5.3 Why order lives in the list and not in a ticket

The obvious alternative — a `serving` / `next_ticket` pair on the object, the
Verilator shape — is rejected, and the reason is not aesthetics: **a thread that
draws a ticket and is then killed never advances `serving`, so every later waiter
stalls forever.** Abort cleanup would have to run on the abort exit path, which the
frontend cannot reach and which has no per-frame cleanup hook. The intrusive list
has the opposite property: unlink happens on the aborting thread's own resume path,
before the poll branches out, so an aborted waiter removes itself and the queue
heals. §12 records the full comparison.

---

## 6. The operations

Every arm below is emitted by the frontend against the synthetic body: a
`HandleDeref` on the receiver (which supplies the LRM 8.4 null check via
`emit_null_handle_check`) plus `member_select(base, field_index, body_ref, loc)`
(`src/ir/Builder.h:111`) — index-addressed, so no slang `ClassPropertySymbol` is
needed for a synthetic field. Expression-position arms (`num`, the three `try_*`)
need branches, which expression lowering already emits for the short-circuit
operators; task-position arms (`put`, `get`, `peek`) are statements and may suspend.

### 6.1 `semaphore`

```
put(n):            keys += n
                   sync_ctl WakeHead(q_head)                  ; 15.3.2 hand-off

try_get(n):        if (keys >= n) { keys -= n; return 1 }      ; 15.3.4, queue ignored
                   return 0

get(n):            if (q_head == null && keys >= n) goto take ; fast path: no bypass
             loop:  sync_wait(q_head, q_tail)                 ; link-if-unlinked, park
                    if (!(node_is_head && keys >= n)) goto loop
                    sync_ctl Leave(q_head)                    ; unlink + WakeHead
             take:  keys -= n
```

`node_is_head` is `*q_head == node`, which the frontend cannot spell (the node is
backend-private), so `SyncWait` **produces** it: the op returns 1 when it resumed as
the head of its queue, 0 otherwise, and the frontend `and`s it with the resource
test. That keeps the condition in the frontend and the node in the backend.

`Leave` = unlink, then `WakeHead` on the same queue: `put(3)` satisfying two
one-key waiters must wake the second after the first consumes, and the departing
thread is the only one that knows the queue changed.

A `put` of fewer keys than the head needs still wakes the head, which retests and
re-parks — one wasted activation per `put`, deduped by the scheduled flag and
bounded by the oscillation counter like any other wake (`kOscillationLimit`,
`LlvmBackend.cpp:10925` region). Waking only a satisfiable head would need the
`need` value in the node; that is a pure optimization, deferred with a note.

### 6.2 `mailbox #(T)`

```
num():             queue_size(fifo)                              ; existing tag, inline

try_put(m):        if (bound != 0 && size >= bound) return 0      ; 15.4.4
                   queue_push_back(fifo, m)                      ; unbounded queue type
                   sync_ctl WakeHead(get_head); WakeAll(peek_head)
                   return 1

try_get(d):        if (size == 0) return 0                        ; 15.4.6
                   d = queue_pop_front(fifo)
                   sync_ctl WakeHead(put_head)
                   return 1

try_peek(d):       if (size == 0) return 0                        ; 15.4.8
                   d = fifo[0]                                    ; ElemSelect, ring math
                   return 1

put(m):            if (put_head == null && (bound == 0 || size < bound)) goto store
             loop:  sync_wait(put_head, put_tail)
                    if (!(head && (bound == 0 || size < bound))) goto loop
                    sync_ctl Leave(put_head)
            store:  queue_push_back(fifo, m)
                    sync_ctl WakeHead(get_head); WakeAll(peek_head)

get(d):            if (get_head == null && size > 0) goto take
             loop:  sync_wait(get_head, get_tail)
                    if (!(head && size > 0)) goto loop
                    sync_ctl Leave(get_head)
             take:  d = queue_pop_front(fifo)
                    sync_ctl WakeHead(put_head)

peek(d):           if (size > 0) goto copy                        ; NO queue test (15.4.7)
             loop:  sync_wait(peek_head, peek_tail)
                    if (size == 0) goto loop                      ; head-ness irrelevant
                    sync_ctl Leave(peek_head)                     ; unlink only
             copy:  d = fifo[0]
```

Three details that are decisions, not mechanics:

- **The destination is an ordinary assignment target, not a `ref` formal.** The
  message crosses as the element-shaped value the existing pop path produces (a pop
  *relocates* the element, so for an aggregate or nested container that storage is
  body-lifetime, `queue-nba-and-nonpod.md:134-137`), and the frontend then emits
  the copy verb the element type calls for — a plane or string stores, a nested
  container deep-copies through its own runtime verb, an aggregate goes leaf-wise.
  This sidesteps the ref ABI entirely. **Measured correction to an earlier draft of
  this section:** it does NOT buy a bit-select destination, because slang rejects a
  bit or part select as a `ref` actual itself ("invalid expression for pass by
  reference"), before lowering ever sees it. LRM 15.4.5's "valid left-hand
  expression" is therefore narrower in practice than it reads.
- **`try_get` / `try_peek` assign only on success**, so the store is emitted on the
  success edge and the status is the value of the expression.
- **`peek` unlinks but wakes nobody**: it changed no state. A `put` wakes the peek
  queue **before** the receive queue's head, so a peeker observes the message the
  receiver is about to consume — the only order under which 15.4.7's "as long as a
  message remains ... any process blocked in either a `peek()` or `get()` operation
  shall become unblocked" is observable at all. A peek waiter is also exempt from
  the head-of-line test its `get` counterpart takes (`require_head`), since it
  consumes nothing and so can starve nobody.

### 6.3 What `SyncWait` does, precisely

```
sync_wait(q_head_lv, q_tail_lv) -> i1 "resumed as head"
  emit_sync_enqueue(node, &q_head, &q_tail, parked_handle(hdl_), sched_ptr_)  ; no-op if linked
  store 0 -> *sched_ptr_                     ; ARM; mandatory, or never woken again
  emit_suspend("sync.resume")                ; coro.save / coro.suspend / switch
  ; --- resumed ---
  if (abort_pending()) { emit_sync_unlink(node); }   ; folds away when neither
                                                    ; disable nor process is used
  result = (*q_head == node)
```

Then `lower_region` emits, immediately after this instruction and before the
frontend's loop branch, the polls it already emits for every suspending tag
(`:11806-11816`): the `disable` polls, and — because `SyncWait` is in
`tag_suspends` — `emit_proc_abort_poll`, whose tail is
`emit_proc_park_while_suspended` (`:23390-23391`).

That ordering is what buys three separate correctness properties for free:

1. **Killable.** A killed waiter is force-woken through its PCB activation node
   (`emit_node_force_wake`, `:22904-22937`), unlinks itself on the abort-pending
   path above, and the poll branches to `task_epilogue_bb_` / `coro_final_bb_`. No
   waiter node is ever touched from outside, and nothing is destroyed from outside.
2. **Suspendable.** A `SUSPENDED` waiter parks in the suspension loop *at the poll*,
   i.e. **before** the frontend's retest, so it cannot consume a key or a message
   while suspended. This is the one contract a naive implementation gets wrong by
   putting the retest first.
3. **Re-sensitized for free.** Nothing is desensitized on suspend: the node stays
   linked, so `resume()` needs no re-arming — which is exactly the invariant
   `process-class.md` §7.3 requires of any new blocking primitive.

`abort_pending()` is a small new helper: OR of the PCB's `kProcAbort` bit (when
`design_uses_process_`) and the open `disable` nodes' abort words (when
`design_uses_disable_`). With neither feature in the design it folds to `false` and
the unlink is not emitted.

**The abort path also hands off**, which the first implementation missed: an
aborting waiter may be the queue's HEAD, and the resource it was about to take is
still there. Measured — `put(2)` racing a `kill` of the head served exactly one of
four waiters, and the two behind the killed one stranded forever. So the abort
branch is `unlink` + `WakeHead`, the same pair `Leave` emits, and it is pinned by
`semaphore: killing the queue head hands off to the next waiter`.

---

## 7. Interaction with everything else that can park

| interaction | outcome | why |
|---|---|---|
| `process::kill` / `disable fork` on a blocked waiter | terminates correctly | §6.3(1); requires only the `tag_suspends` entry |
| `process::suspend` / `resume` on a blocked waiter | suspends without consuming; resume replays | §6.3(2,3) |
| remote `disable` of a block containing a blocked `get` | leaves through the block's structured exit | same poll, same unlink |
| `process::status()` of a blocked waiter | `WAITING` | `emit_suspend` maintains RUNNING↔WAITING (`:4958-4966`) |
| `wait fork` over children blocked on a mailbox | unaffected | the PCB tree is orthogonal |
| `$finish` while waiters are parked | waiters stay linked and are never resumed | rings are emptied, nothing is freed, no collection follows (`lower_finish`) |
| a blocked `get` inside an assertion action block | **refused in the first cut**, loud | an action block runs on the monitor's thread and the reactive-safety checker already refuses `wait fork` / `disable fork` there on identity grounds (`src/Lower.cpp:889-897`, `:928-938`), with a backend backstop at `reject_unsupported_reactive`. The `try_*` forms stay legal. Lifting it is the same work as `expect`'s. |
| a blocked `get` in a task called from a `program` block | works, and blocks in the REACTIVE region set | LRM 24.3.1 / 24.5: the task inherits the calling thread's home, so its park is reactive-home (`build_shared_home_fns`, `docs/design/program-blocks.md` §6.1). `emit_suspend`'s old refusal for this shape is gone. |
| deadlock (all threads blocked) | the drain quiesces and simulation ends | no event is left to run; no deadlock detection is in scope |

---

## 8. The typeless mailbox (LRM 15.4.9) — a separate phase

Bare `mailbox m;` is not a parse error and not a slang error: slang specializes the
generic class with `T` = `UntypedType` (`StdPackage.cpp:132-133`,
`slang/ast/types/AllTypes.h:479-486`), and `m.put(42); m.try_get(s);` type-checks.
Verified: clotho's std guard is the *only* diagnostic on
`module top; mailbox m; initial begin int a; m.put(42); ... end endmodule`
(`the built-in 'mailbox' class is not supported`). Verilator, by contrast, refuses it
outright (`Class parameter type without default value is never given value`), because
its `verilated_std.sv` mailbox is a plain `#(type T)`.

So this is legal SV that clotho must eventually support, and it needs two things
nothing in the tree has:

1. **A dynamically typed element.** `rt_elem_ops` is monomorphic per container:
   `stride`, `proto` and the leaf table are compile-time constants. A typeless FIFO
   needs a new kind — `rt_elem_dyn` — whose slot is
   `{ u32 type_id, u32 pad, const rt_elem_ops* ops, void* image }` and whose
   init / copy / deinit dispatch through the **slot's own** `ops`. GC tracing follows
   the same pointer, so a handle inside a boxed message stays precise.
2. **A run-time type identity with LRM 6.22.2 equivalence semantics.** The natural
   candidate is the interned `ir::Ref`: clotho's pool is structural for non-class
   types and nominal for classes, so `Ref` equality already answers "identical
   domain, signedness and size" for integral types and "same class" for handles. It
   is *more permissive* than 6.22.2 for unpacked structs (structural interning vs
   the LRM's same-declaration rule); the direction of the error and the fix (compare
   a canonical declared-type id instead) must be recorded at the site.

Only then do the two LRM behaviours that a `mailbox #(T)` can never reach become
reachable: `try_get` / `try_peek` returning **a negative integer** on a mismatch
(15.4.6, 15.4.8) and blocking `get` / `peek` raising **a run-time error** (15.4.5,
15.4.7) — a located fatal here, since clotho never silently accepts.

Until then the refusal is by its own message, not the generic std one:
`"the typeless std::mailbox (LRM 15.4.9) is not supported; use mailbox #(T)"`, with
a TODO naming this section.

---

## 9. Gating and cost

There is no design-wide machinery, so there is nothing to gate: a design that names
neither class emits nothing new, structurally, the way a design with no events emits
no event pool. `--emit llvm` must still be verified byte-identical at every phase,
because that is the standard the tree holds (`disable.md:646`,
`process-class.md:185`).

With the feature used, the cost is:

| site | cost |
|---|---|
| `new semaphore` / `new mailbox` | one `rt_gc_alloc` (or `rt_alloc` under `--gc off`), a memset, one or two stores, one bound-sign compare |
| `sem.put(n)` | null check, add, one wake walk step (load + flag test + ring push) |
| `sem.try_get(n)` | null check, load, compare, subtract |
| `sem.get(n)` fast path | null check, two loads, two compares, subtract — **no call, no alloca use** |
| `sem.get(n)` blocked | 6-word frame node, link, arm, suspend, unlink, one retest per wake |
| `mbx.num()` | inline `queue_size` (no call) |
| `mbx.try_put` / `try_get` | the existing `rt_queue_push_back` / `rt_queue_pop_front` call plus one bound or length compare, plus one/two wake walks |
| `mbx.try_peek` | inline ring math (`fifo[0]`) |
| any body containing any of these | forced to a real coroutine (`blocks_thunk`), which also removes it from `share_twin_groups` |
| `--gc` on | one leaf-table entry per mailbox class; the collector walks a mailbox's ring like any other queue |

No ring resizing, no wheel change, no static tables, no ABI change, no new runtime
symbol, no `Wheel.h` change, no compaction pass.

---

## 10. IR, printing, legalization, frontend interception

Two tags, **appended at the end of the enum** (`src/ir/Ir.h`, after the `Proc*`
family):

- **`SyncWait`** — suspending, produces an i1 ("resumed as head"). Payload
  `SyncWaitInfo { ExtraSlice args; Ref type; }` with the queue-header lvalues in
  `args` — `ExtraSlice` **first**, matching `ProcCtlInfo` (`Ir.h:513-521`), because
  `Printer`'s generic `PayloadKind::Extra` path reads the leading slice and would
  otherwise print garbage.
- **`SyncCtl`** — statement, with a `SyncOp : uint32_t { Leave, WakeHead, WakeAll }`
  selector (u32-wide for the unique-object-representation requirement `ProcOp`
  documents at `Ir.h:493-494`).

The checklist a new suspending op must satisfy, taken from what the `Proc*` family
actually touched:

| # | site | requirement |
|---|---|---|
| 1 | `Inst::Tag` enum (`Ir.h:2026-2040` region) | append only |
| 2 | payload struct + `payload_kind()` (`Ir.h:2381-2386`) | `K::Extra` |
| 3 | `ExtraOperandShape` + its switch (`Ir.h:2480-2504`) | new shape |
| 4 | `Ir::operands()` (`Ir.cpp:83-86`) | slice walk |
| 5 | `Builder` emitters + operand-order contract (`Builder.h:369-373`) | `sync_wait`, `sync_ctl` |
| 6 | `Printer` tag names (`Printer.cpp:410-414`) | plus an explicit payload arm if the selector should print |
| 7 | `Analysis::tag_info` (`Analysis.cpp:163-166` group) | `blocks_thunk = true`, not pure, **not synthesizable** — `process-legalization.md:693-725` models only the two bare jumps, so claiming otherwise would under-guard |
| 8 | `ValueSweep::sweep` | `SyncWait`'s i1 is definite 2-state |
| 9 | `compute_type` | i1 for `SyncWait`, statement shape for `SyncCtl` |
| 10 | `lower_inst` dispatch | `lower_sync_wait` / `lower_sync_ctl` |
| 11 | **`tag_suspends`** (`LlvmBackend.cpp:23022-23033`) | **`SyncWait` MUST be added** — see §16 |
| 12 | reactive legality | frontend checker + `reject_unsupported_reactive` backstop, kept in agreement |

No `DesignUses` arm and no `Structural::walk` change: nothing is emitted design-wide.

**Frontend interception** mirrors `process` exactly:

- `lower_class_type`'s std arm gains two more diversions:
  `clt.name == "semaphore"` → `lower_semaphore_class`, `clt.name == "mailbox"` →
  `lower_mailbox_class` (which refuses an `UntypedType` parameter per §8, and
  refuses a *derived* class — `class my_mbx extends mailbox#(int)` — because the
  synthetic body has no vtable story; loud, with a TODO). `weak_reference` keeps
  the generic message.
- `is_sync_method(sub)` beside `is_process_method` (`src/Lower.cpp:2697-2710`):
  `MethodFlags::BuiltIn`, owner is a `ClassType` named `semaphore` or `mailbox`,
  owner's parent is package `std`. Keyed on symbols, not on a remembered `Ref`,
  because a method call can be lowered before any variable of the type is declared.
- `lower_sync_method(call, sub, b)` reached from `lower_user_call`
  (`:23212-23219`), beside the process arm, before receiver resolution and argument
  marshaling; a name it does not know is
  `fail("<class>::<name>() is not supported")` — a whitelist, so a future slang
  addition fails loud rather than no-oping.
- `lower_new_class` (`:22902`) gains the constructor intercept of §4.3.
- No vtable slot is minted for any of these methods: `is_user_virtual` excludes
  `MethodFlags::BuiltIn` (`:2712-2714`), so dispatch is impossible by construction
  and interception at the call site is the only correct route.

---

## 11. Hazards

Ranked by how much damage a naive implementation does.

1. **`SyncWait` omitted from `tag_suspends`.** A thread blocked in `get()` becomes
   unkillable: it is force-woken, retests, and re-parks forever. This is not
   hypothetical — it is the live state of `ProcWait` today (§16). It is also silent:
   every value test passes, and only a kill-a-blocked-waiter test catches it.
2. **Re-enqueueing on a spurious wake.** Without the enqueue guard (§5.2) an
   unsatisfied waiter goes to the back of the queue on every wake, which converts
   LRM 15.3.3's FIFO into starvation-prone LIFO under load. Also silent: it shows up
   only in an ordering test with three or more waiters.
3. **The bound passed to `rt_queue_push_back`.** A bounded *queue type* drops the
   opposite end silently (`Ir.h:1237-1239`), so a mailbox whose FIFO is a bounded
   queue **loses messages** instead of blocking. The FIFO must be an unbounded queue
   type and the bound tested in emitted IR.
4. **The retest running before the suspension check.** If the frontend's condition
   is tested before `lower_region`'s poll, a SUSPENDED thread consumes a key or a
   message while suspended (§6.3(2)). The op must be a distinct instruction from the
   retest, so the poll interposes.
5. **`mailbox #(event)` mints and abandons an object per `put`.** `rt_queue_push_back`
   calls `rt_elem_init` before `rt_elem_copy`, and for `rt_elem_event` init is *not*
   a proto copy — it calls `rt_event_new()` (LRM Table 6-7's "a new event",
   `Runtime.cpp` `[gc]`:1048-1056) — then copy overwrites the handle
   (`:1105-1109`). Since event objects are never freed
   (`Runtime.h:1399-1404`), every `put` leaks one. Either refuse
   `mailbox #(event)` loudly in the first cut, or add an init-skipping push path;
   refusing is the first cut's answer, with the TODO naming this hazard.
6. **A moving collector would dangle the queue headers.** The node stores `q_head` /
   `q_tail` as raw interior pointers into the object body. Evacuation would move the
   body. Same class as `process-class.md` hazard 8 (a PCB pointer cached across a
   suspend); the fix on that day is to reach the header from the handle at each use.
7. **`num()` used as a control variable.** 15.4.2 says the value is valid only until
   the next `get`/`put`. Nothing to implement, but a test that samples `num()` around
   a blocking op is pinning unspecified behaviour and must not be written.
8. **Oscillation budget.** Each `put` wakes at least one thread, and a `try_get`
   spin or a put/get ping-pong at one instant is bounded only by
   `kOscillationLimit` (10M activations) before `rt_oscillation_abort`. Worth one
   test at the limit, as `disable` and `process` both took.
9. **Derivation from a synthetic body.** slang admits
   `class c extends mailbox #(int);`. The synthetic body has no user methods to
   inherit and the built-ins take no vtable slot, so an override cannot work.
   Refused loudly, with a TODO.
10. **The three test files that use `semaphore` as a fixture** for "a construct that
    fails to lower" break the moment it lowers: `tests/class_test.cpp:1216-1244`,
    `tests/recovery_test.cpp:61,65,69,100,168`, `tests/sourcemap_test.cpp:99,120`.
    The last two need a *still-refused* construct instead — `std::weak_reference` is
    the natural replacement, and it stays refused after this work.

---

## 12. Rejected alternatives

- **A runtime-owned object with a `{slot, gen}` handle** (the `rt_event` model, and
  the shape `Lower.cpp:2554-2556`'s comment originally imagined). Rejected on a
  specific, measured ground: a runtime-owned object that itself holds an `rt_queue`
  is **invisible to the collector**. The one existing instance of that shape — the
  whole-container NBA staging record in `rt_nba_arr_slot`'s pool — is not scanned,
  because `rt_gc_range_add` (`Gc.h` `[gc]`:328-333), written for exactly this case,
  has **zero callers repo-wide** and is not JIT-exposed. Building the mailbox that
  way means either wiring that path for the first time or emitting
  `rt_gc_container_add` / `_remove` around a runtime object's lifetime — new GC
  surface, to replace a class body the collector already handles. It also needs its
  own refcount discipline for an object the collector can already manage, which
  `garbage-collection.md:68-77` argues against on its own terms.
- **A ticket pair (`serving`, `next_ticket`) for FIFO order**, i.e. Verilator's
  shape done strictly. Cheap and elegant until a waiter is killed: `serving` never
  advances and every later waiter stalls permanently, with no cleanup hook on the
  abort exit to fix it. The intrusive list self-heals because unlink happens on the
  aborting thread's own resume path. Rejected on that single ground.
- **An `event`-typed property as the wait queue**, making the blocking loops lower
  into `@(mbx.evt)` / `-> mbx.evt` with *zero* new IR tags — genuinely attractive,
  since events in class properties and `@(obj.e)` both landed. Rejected because the
  event list is push-front with arbitrary wake order (LRM 4.7), and every waiter
  unlinks on resume, so there is nowhere to put arrival order; recovering it needs a
  ticket, which the previous point rules out. It also leaks one `rt_event` per
  object (never freed) and would need `ClassNew`'s power-on init of an event property
  verified. Worth revisiting only if the FIFO requirement is ever relaxed, which the
  LRM does not permit.
- **Head-of-line ordering implemented in the waker** (walk the queue, wake only a
  satisfiable node) rather than wake-head-and-retest. Needs the `keyCount` in the
  node and a longer walk; it is a strict optimization of the same semantics, so it
  is deferred, not adopted, and noted at the site.
- **Lowering the two classes from SystemVerilog source**, Verilator's approach
  (`verilated_std.sv`): a shipped `.sv` package compiled with the design. Rejected —
  it needs `wait (expr)` on a class property to work, and it does not: a class
  property is not in `sigmap_`, so a store to it services no wait queue, and
  `wait (m_queue.size() > 0)` parks forever (`LValue::is_signal`,
  `LlvmBackend.cpp:340-343`: "its store services wait queues"; the sensitivity
  collector's `record_read` bails on a symbol absent from `sigmap`,
  `Lower.cpp:481-489`).
  Verilator gets away with it because its `--timing` model re-evaluates `wait`
  conditions differently. Fixing that to ship an `.sv` implementation would be a
  strictly larger change than this document, in a hotter path.
- **A dedicated `DesignUses::sync` flag.** Nothing is emitted design-wide, so the
  flag would gate nothing; adding one would be cargo-culting `disable`'s gate.

---

## 13. Phased plan, and the split question

Each phase is independently landable and independently testable, and every phase
keeps the ungated IR byte-identical for designs that do not use the feature.

- **P0 — the substrate.** Narrow the std guard by name; the two synthetic bodies;
  the constructor intercept; `%clotho.sync_node` and the three emitters; the two IR
  tags with all twelve checklist entries including `tag_suspends`; `abort_pending()`.
  Nothing user-visible works yet except a declaration and `== null`. Acceptance:
  byte-identical `--emit llvm` on picorv32, `examples/` and `tests/perf`.
- **P1 — `semaphore`, complete.** `new`, `put`, `try_get`, `get`, including
  head-of-line admission and the `Leave` hand-off. The smallest end-to-end vertical
  slice that exercises the whole substrate, and the phase that pins the FIFO
  semantics against the LRM.
- **P2 — `mailbox #(T)`, non-blocking half.** `new`, `num`, `try_put`, `try_get`,
  `try_peek`, plus the payload matrix (int, string, class handle, unpacked struct,
  nested queue) and the `mailbox #(event)` refusal. No waiting, so no new substrate
  risk; parallelizable with P1 once P0 lands.
- **P3 — `mailbox #(T)`, blocking half.** `put`, `get`, `peek`, the three wait
  queues, the `peek` exemption, and the process/`disable` interop tests.
- **P4 — the typeless mailbox** (§8): `rt_elem_dyn`, the run-time type identity, the
  negative return and the mismatch fatal. Separate project; the refusal from P0
  stays until it lands.
- **P5 — optional.** Wake only a satisfiable head (§12); lift the action-block
  refusal alongside `expect`'s.

**Should this be one task or two?** One branch, phased as above. The two classes
share P0 entirely — the guard, the node shape, the emitters, both IR tags, the
constructor intercept — so splitting them into parallel branches duplicates the
substrate and collides in the same three files (`Lower.cpp`'s std arm, `Ir.h`'s tag
enum, `LlvmBackend.cpp`'s `tag_suspends`). Once P0 is in, P1 and P2 are genuinely
independent and can run in parallel; P3 depends on P1 having proved the wait loop.
P4 is a separate task with its own design and no dependency on P1-P3 beyond the
class body.

---

## 14. Verification

**The oracle situation is better than `process`'s, and it was measured** (`iverilog 13.0`, `Verilator 5.050`):

- **iverilog 13.0 cannot parse either class.** `mailbox #(int) m;` and
  `semaphore s;` are both `syntax error ... Invalid module instantiation`. Same
  outcome as `process`: the primary oracle PRINCIPLES.md prescribes is unavailable.
- **Verilator 5.050 implements both**, from `verilated_std.sv:36-131`, and scores
  correctly on: a typed mailbox with a bound (blocking `put` resumed in the same time
  step after a `get` freed room — measured `put 0/put 1 @0, got 0 @10, put 2 @10,
  ...`), `peek` blocking on an empty mailbox then seeing the message at the `put`
  instant, a class handle through a mailbox (identity preserved), and every
  `semaphore` availability case (`try_get(4)` of 3 keys → 0, `try_get(3)` → 1,
  default `put()`/`get()` → 1).
- **Verilator does not implement LRM 15.3.3's FIFO.** Its `get` fast path is
  `if (m_keyCount >= keyCount && m_nextKeyCount > m_keyCount)`
  (`verilated_std.sv:117-121`), which deliberately lets a later, smaller request
  take keys the queued head cannot yet use. Measured: with `s = new(1)`, thread A
  `get(2)` at time 0 and thread B `get(1)` at time 1, **B is served first and A then
  deadlocks** even after a `put(1)` at time 5 (`[1] B got 1`, `[5] put 1`, no
  `A got 2`, `done` never printed). Under this design A is served first. So the FIFO
  cases must be pinned from the LRM text (15.3.3 plus Accellera's Interpretation #1),
  and any test that scores them **must say so in its comment**, so a later Verilator
  release cannot silently reclassify a pinned case. Same rule
  `process-class.md:852-853` and `fork-join.md:508-511` already impose.
- **`sv-tests` has two runnable cases**, `tests/chapter-15/15.4--mailbox-blocking.sv`
  and `15.4--mailbox-non-blocking.sv`, both `mailbox #(string)` with `put`/`peek`/
  `num`/`get` — free regression fixtures for P2/P3.

| case group | oracle |
|---|---|
| every `semaphore` availability case, `num()`, `try_*` returns, typed payload round-trips, bounded `put` blocking and resumption, `peek` on empty then filled, handle identity through a mailbox | Verilator 5.050 |
| FIFO / head-of-line: three waiters served in arrival order; a later small request not bypassing a blocked larger one; `put(3)` waking two one-key waiters in order | **LRM 15.3.3 + Accellera Interpretation #1 only** — Verilator deviates |
| `peek` unblocking behind a blocked `get` (15.4.7 vs 15.4.5) | LRM only (§15 records the reading) |
| negative `bound` fatal, typeless-mailbox refusal, `mailbox #(event)` refusal, derivation refusal | clotho-specific; LRM 15.4.1 for the first |
| kill / `disable fork` / `suspend` / `resume` of a blocked waiter | LRM 9.7 + `process-class.md` only |

Regardless of oracle, the suite owes:

- **A new `tests/sync_test.cpp`** added to the explicit TU list (`CMakeLists.txt`),
  with the `run_capture` + `fails` harness `tests/class_test.cpp:20-56` and
  `tests/process_test.cpp:15-46` use, plus `tests/Subprocess.h`'s
  `run_capture_finish` for the fatal cases (the negative-bound fatal and a
  deadlock-at-`$finish` design) — `process_test.cpp` has no fatal test precisely
  because it lacks that include.
- **Ordering tests with three waiters minimum.** Two waiters cannot distinguish FIFO
  from LIFO; hazard 2 is invisible below three.
- **The process interop matrix**: kill a thread blocked in `sem.get` / `mbx.get` /
  `mbx.put` / `mbx.peek`; kill it while it is the queue head and while it is not;
  suspend then resume a blocked waiter across a `put`; `disable fork` over a subtree
  with two blocked waiters. Each of these is a hazard-1 or hazard-4 detector.
- **The three GC modes** through `tests/gc_test.cpp` `[gc]`'s `check_all_gc_modes`
  harness, over a `mailbox #(handle)` whose messages are the only reference to their
  objects, and over a mailbox reachable only from a parked waiter's frame.
- **ASan + UBSan + LSan**, which is what actually covers the pop-relocation
  ownership rule (a missed deinit leaks, a double deinit is a double free, and
  neither shows in a value check).
- **Byte-identical `--emit llvm`** at every phase for every design that does not name
  either class.
- **The refusal messages, each by its own name**: `weak_reference` still refused
  after the guard narrows; the typeless mailbox by its own message; the
  `mailbox #(event)` and derivation refusals; a non-admissible `T` reporting through
  `guard_container_element`'s existing text.

---

## 15. LRM decisions this design records

- **`try_get` / `try_put` / `try_peek` ignore the wait queue.** 15.3.4 and 15.4.4/6/8
  define them purely on availability, and they never join the waiting queue, so
  15.3.3's FIFO clause — which is about that queue — does not constrain them. A
  `try_get` spinner can therefore starve a blocked `get`; the LRM permits it, and
  Verilator agrees.
- **A blocked head blocks later arrivals** even when the resource would satisfy them
  (15.3.3 read with Accellera Interpretation #1). Verilator does the opposite; the
  deviation is deliberate and measured (§14).
- **`peek` is exempt from the queue discipline**: it consumes nothing, so it proceeds
  whenever a message is present, regardless of blocked getters ahead of it. This is
  the only reading under which 15.4.7's broad-unblock sentence and 15.4.5's FIFO
  sentence are both satisfied.
- **A negative `bound` (or `keyCount`) is a located fatal**, where 15.4.1 says
  "illegal... can result in indeterminate behavior" and permits a warning. Clotho
  never silently accepts.
- **The key count is 64-bit**, per 15.3.1's "can increase beyond `keyCount`"; the
  `int`-typed surface is unchanged.
- **`mailbox #(T)` admits a non-`singular` `T`** (an unpacked struct or fixed
  unpacked array), which 15.4.3's `singular` formal excludes. The queue path already
  carries it; refusing it would invent a restriction, and admitting it cannot change
  the meaning of a legal program.
- **A suspended waiter does not consume**, and a suspended *head* stalls its whole
  queue. LRM 9.7 says a suspended process does not execute and 15.3.3 says order is
  preserved; together they say exactly this.
- **A killed waiter leaves the queue and the queue heals.** The LRM says nothing
  about a killed process's pending `get`; 9.6.2's unspecified-results licence covers
  it, and the alternative (a stalled queue) would be observably worse.
- **Wake order within one instant is queue order**, because the wake walk pushes in
  list order onto a FIFO ring. Not required by the LRM, but it is what makes the
  FIFO guarantee observable rather than merely internal.

---

## 16. Two defects this scoping found

Both are in landed or nearly-landed work, both are one-line-ish, and both are
recorded here because this design's correctness leans on them.

1. **`ProcWait` is missing from `tag_suspends`**
   (`src/ir/LlvmBackend.cpp:23022-23033`), although `process-class.md:479` and
   `:556-557` both state it must be there. Consequence: a thread killed while parked
   in `p.await()` or `wait fork` is force-woken, unlinks its waiter node, retests,
   and **re-parks** — neither retry loop tests `kProcAbort`, and
   `emit_suspend_raw` carries only the RUNNING↔WAITING maintenance. No test covers
   it: `tests/process_test.cpp:415` kills the *target* of an `await`, never the
   awaiter. Fix belongs to the process work; P0 must not "fix" it by accident and
   leave it untested.
2. **`process-class.md:165-166` and `:729-733` are stale.** They state that "a class
   handle inside a queue / dynamic array / associative array is still a compile error
   under `--gc` (`elem_ops_for`, `:1503-1516`)", and derive from it that `process
   q[$]` is refused. `elem_ops_for` contains no refusal in either worktree (read raw
   end-to-end at `:1464` here and at `[gc]`:1905 in the main worktree); the element
   admission gate is `guard_container_element` (`src/Lower.cpp:2180-2220`), which
   admits a class handle unconditionally with no `gc_on()` test, and the collector
   traces container payloads precisely (`rt_elem_ops.nhandles`, `walk_queue`). So
   `process q[$]` is admitted today, and this document's `mailbox #(handle)` claim
   rests on the same, working path. `docs/design/garbage-collection.md` §3.3's first
   sentence is stale in the same way (it describes the pre-landing state of that
   work).
