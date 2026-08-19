# `process` class and process reflection (LRM 9.7)

Target: the built-in `process` class — `self`, `status`, `kill`, `await`, `suspend`,
`resume`, `srandom`, `get_randstate`, `set_randstate` — plus the two statements that
need the same runtime structure and are refused today for exactly that reason,
`disable fork` and `wait fork` (LRM 9.6.2/9.6.3, fork V3).

Status: **scoping. Nothing is implemented.** The whole surface is refused
type-level, by class name, at one site (`src/Lower.cpp:2178-2181`), so today a bare
`process p;` declaration fails before any method is reached.

This is an M4 feature, not a nice-to-have: a UVM testbench stores a `process`
handle in a class property, calls `process::self()` inside a forked thread and
`kill()` on it later, and `uvm_objection`-style code awaits completion. Nothing in
the tree is close — `docs/design/disable.md:189-196` states it plainly: *"No process
identity, no process table, and no handle array. A process is the raw `ptr` from
`llvm.coro.begin`."*

Two documents defer to this one and both understate it:

- `docs/design/disable.md` V4 (`:745-768`, now corrected there) said `kill()` is "V2's abort applied to a
  dynamic handle, `await()` a join barrier, `suspend()`/`resume()` a desensitise
  pair". The first is true only for a *single* activation of a process body; a
  thread is a chain of coroutine frames and a subtree of descendants, and neither
  is reachable from anything that exists.
- `docs/design/fork-join.md` V3 (`:459-484`) says `wait fork` / `disable fork` need
  "an enumerable live-descendant structure" that **no phase of either document
  commits to building**. This design builds it, once, and all three features fall
  out of it. That is the single most important architectural claim here: the
  process control block below is fork V3's missing structure, not a parallel one.

---

## 1. Semantics (LRM 9.7, 9.6.3, 18.14)

slang synthesizes the class into the `std` package
(`build/_deps/slang-src/source/ast/builtins/StdPackage.cpp:24-66`), `isAbstract` +
`isFinal`, with an `int`-based `EnumType` whose members are also injected as
transparent members of the class:

```systemverilog
class process;
  typedef enum { FINISHED, RUNNING, WAITING, SUSPENDED, KILLED } state;
  static function process self();
  function state status();
  function void kill();
  task await();
  function void suspend();
  function void resume();
  function void srandom(int seed);
  function string get_randstate();
  function void set_randstate(string state);
endclass
```

The rules that shape the design, in the order they constrain it:

- **A process object is not user-constructible.** `isAbstract` makes slang reject
  `new`, which clotho already reports (`cannot instantiate an abstract (virtual)
  class`, `src/Lower.cpp:22102`). Objects come into existence because a process
  exists, so their lifetime is tied to a thread, not to a `new`.
- **Identity is per *process*, not per object.** `self()` called twice in one
  thread must compare equal. Clotho's `==` on class handles is a bare `icmp` on the
  64-bit handle (`src/ir/InternPool.cpp:176-180`), so clotho must return *the same
  object*, memoized. Verilator instead returns a fresh object per call and
  overloads `operator==` to compare the underlying control block
  (`/usr/share/verilator/include/verilated_std.sv:155-160`,
  `verilated_types.h` comparison specialization) — not an option here, and not
  needed.
- **The five states.** FINISHED = terminated normally; RUNNING = currently
  running, *not* in a blocking statement; WAITING = waiting in a blocking
  statement; SUSPENDED = stopped awaiting a `resume`; KILLED = forcibly killed by
  `kill` **or by `disable`**. That last clause matters: 9.6.2's `disable` and 9.7's
  `kill` share a state, so the two mechanisms must share one status word.
- **`kill()` terminates the process *and all its descendants*** created with
  `fork` — and 9.6.3's `disable fork` wording is explicit that descendants include
  "descendants of children that already terminated". So the descendant relation
  must survive an intermediate thread's completion (§5.3).
- **`await()` blocks until the target completes.** It shall be an error to call it
  on the current process.
- **`suspend()`** suspends the caller's own execution or another process's; a
  self-suspend takes effect immediately and resumes at the statement after the
  call.
- **`resume()`** on a process suspended while blocked *re-sensitizes* it to the
  event expression / wait condition / remaining delay; **if the condition is now
  true or the delay has already transpired, the process is scheduled onto the
  Active (or Reactive) region so as to continue execution in the current time
  step.** This sentence, not the suspend side, is what forces a *pending-wake* bit
  (§7).
- **Random stability (18.14).** The RNG is per process; `srandom` reseeds it, and
  `get_randstate` / `set_randstate` round-trip it. The string format is
  implementation-defined, and passing a string not obtained from `get_randstate` is
  undefined.

`process` is `final`, so there is no derivation, no vtable slot question, and
`$cast` on it is trivial.

---

## 2. Current state — what exists, what does not

Facts, all established against the code, in the order the design needs them.

**Nothing names a process.** A handle is `llvm::Value* hdl_` from
`llvm.coro.begin` (`src/ir/LlvmBackend.cpp:3547-3548`); the seed loop keeps
handles in a *codegen-time* `std::vector<llvm::Value*>` and drops them after the
subscribe pass (`:9873`, `:9944-9957`). The only durable copies anywhere are a
wait-bucket entry `{ptr handle, ptr flag}` (`wait_elem_ty`, `:6038-6041`), field 2
of the frame-resident `awaiter` (`:4735-4741`), and the `parent` field of a task or
fork record — all region-tagged through `parked_handle` (`:9406-9430`).
`src/runtime/*` has no process struct at all.

**A fork child cannot be named after it is spawned.** `lower_fork_inst`
(`:20319-20449`) ramps each child, then either `emit_region_push`es it or
`coro_resume`s it inline, and the handle dies as a dead SSA value at the spawn site
(`:20382-20428`). Fork state is `{ i32 count, ptr parent }` (`fork_record_ty`,
`:20260-20271`) or the refcounted `{ count, parent, wake_at, refs }` for the
detaching kinds — a *count*, never a set. `is_fork_child` is a compile-time
`Function` flag (`src/ir/Module.h:543`).

**The abort primitive exists and is per-activation, per-static-target.** Remote
`disable` publishes a frame-resident node
`{ ptr next, ptr prev, ptr handle, ptr park_slot, ptr sched_flag, i32 abort }`
(`disable_node_ty`, `:22902-22911`) on a flat per-target list
`@clotho.disable.head[t]` (`:22877-22917`), linked on entry and unlinked on exit
(`:22921-22981`). `DisableRemote` compiles to an inline walk (`:22750-22817`) that
marks every node, skips the running self, and force-wakes **exactly once** — either
by claiming the per-arm heap `#delay` park record's `state` field (`:4969-4986`,
loser path in the wheel trampoline at `:4820-4841`) or, absent a live record, by
pushing only if the scheduled flag is clear. Each woken activation polls after
every parking op (`lower_region`, `:11855-11862`, gated `design_uses_disable_ &&
tag_suspends(t)`) and leaves through its **ordinary** structured exit — the block's
end block, or `task_epilogue_bb_` with copy-out skipped (`:4527-4545`). No `rt_*`
helper is involved, and nothing is ever destroyed from outside.

What that primitive cannot do, and this design must: terminate a *process* (slang
admits only a block or task symbol as a `disable` target), find an activation from a
*dynamic handle* (target ids are constant GEP indices), or reach a *subtree*.

**Termination cannot be done from outside.** `coro.destroy` on a queued handle is a
use-after-free and no ring supports removal (`disable.md` Hazard 3);
`coro_cleanup_bb_` is `coro.free` only (`:3588-3595`), so a raw destroy leaks every
frame-owned string, container and aggregate — the deinit walk
(`deinit_body_strings`, `:4198-4221`) runs solely at normal exits. The wheel cannot
cancel (`src/Wheel.h`: `now`/`empty`/`schedule`/`advance`/`tick`, nothing else), and
static wait buckets are never compacted (`:5001`, scan at `:9542-9581`); only
dynamic buckets can be compacted out (`emit_dynamic_remove`, `:8831-8873`).

**A thread already has one per-thread pointer threaded through the whole call
ABI.** The xoshiro256++ state is argument 1 of every function and task
(`declare_function:3846-3855`, `declare_task:3916-3925`), materialized as a frame
alloca in the ramp (`:3629-3634`), reseeded per fork child from one draw of the
parent's stream (`:4455-4461`). This is the precedent for process identity (§4).

**The class machinery and the collector are both in place.** An object is
`{ ptr vptr, ...properties }` (`class_body_ty`, `:1825-1841`) with a uniform
one-word header (`:1820-1823`); the handle is a raw pointer in a 64-bit two-state
plane, `null == 0`; allocation is `rt_gc_alloc` with a per-class trace map
`clotho.classgc.<Ref>` (`:2141-2168`) or `rt_alloc` under `--gc off`
(`:12960-12999`). The collector is a non-moving Immix-shaped mark-region collector
that scans suspended coroutine frames conservatively through an intrusive registry
(`rt_gc_frame_link`, `:10539-10544`; `src/runtime/Gc.h:107-124`) and collects only
at the drain's quiescent safepoint (`:10403-10419`). Precise roots come from
per-region tables built by filtering `slots_` to class-handle words (`:5775-5804`).
**A class handle inside a queue / dynamic array / associative array is still a
compile error under `--gc`** (`elem_ops_for`, `:1503-1516`).

**No randstate support of any kind** — a grep for `srandom|randstate` across
`src/` returns nothing, not even a refusal by name (`docs/TODO.md:1432-1433`).

---

## 3. Decision summary

| # | Move | Why |
|---|---|---|
| **A** | **Split the control block from the object.** A thread's control block (status word, activation stack, `await` waiters, RNG pointer, tree links) is **native, untraced** storage: a `[nproc x %clotho.pcb]` array in module state for statically created processes — no allocation at all — and a chunked runtime pool with a freelist for fork children, the only genuinely dynamic tier. The `process` **object** is a small GC class object whose single property is a `chandle` to a control block, created **lazily at the first `self()`** and memoized weakly in the block. | Processes are static, so their control blocks are static; only fork children are unbounded (§4.0). Allocation then scales with `self()` calls, not with threads spawned, and the tree/park state the scheduler walks never touches the collector. |
| **B** | **Identity travels the call ABI**, as argument 2 beside `rng`, gated on `DesignUses::process`; the argument is the control-block pointer, not a handle. | `rng` proves the pattern. A "current process" global would need a store at every resume site, and a missed site is a *silent* wrong identity (Verilator pays exactly this: `t_currentp`, `verilated_types.h:315`). |
| **C** | Each parking activation links a **frame-resident node onto its thread's PCB stack**, innermost first, reusing `disable_node_ty` verbatim. | Gives kill a claimable park for a thread parked *behind a task call or a fork* — the case remote `disable` refuses today (`hard_park`, `src/Lower.cpp:8463-8501`). |
| **D** | **`kill` = the landed `disable` abort, per thread instead of per target**: mark, force-wake once, poll after each park, leave through the ordinary epilogue. Nothing is destroyed from outside. | It is the only shape that unwinds a nested frame chain and releases frame-owned heap. §12 rejects the alternatives. |
| **E** | The tree **re-parents live grandchildren on a thread's completion**, so `first_child == null` means "no live descendants". | LRM 9.6.3 requires descendants of already-terminated children; the invariant then makes `wait fork` a one-word test rather than a walk. Verilator does *not* do this (`verilated_types.h:338-343` detaches in the destructor), which is a spec deviation to avoid inheriting. |
| **F** | `suspend`/`resume` are a **status word plus a pending-wake bit**, checked at the same poll point as the abort. No desensitisation, no queue surgery. | Nothing can be removed from the wheel or from a static bucket; the LRM's own resume wording (§1) is satisfied exactly by "the wake already happened, replay it". |
| **G** | One `DesignUses::process` gate; when clear, **zero bytes emitted**, verified by byte-identical `--emit llvm`. | The rule every scheduler feature in the tree has held (`disable.md` Hazard 17). |

---

## 4. The control block and the object

### 4.0 Where the dynamism actually is

A process is static: `mod_.processes()` is a compile-time span
(`src/ir/Module.h:707-709`) and the process index already keys `proc_sched`
(`sched_flag_ptr`, `LlvmBackend.cpp:889-895`). So a statically created process needs
**no allocation whatsoever** — its control block is one element of a module-state
array at a constant address, exactly like its scheduled flag. An earlier draft of
this document allocated one per thread uniformly; that was wrong, and the three
places dynamism is real are worth naming precisely, because each of them
independently rules out a purely static scheme:

1. **Fork children are unbounded.** `for (...) fork ... join_none` spawns one thread
   per iteration with no static bound — which is why the ring bound *saturates*
   at `kMaxForkRingSlots` for a non-`Join` spawn edge rather than computing a number
   (`body_activations`, `:9040-9072`; `fork-join.md` §V2.3). A per-thread control
   block therefore cannot come from a statically sized table.
2. **A finished process stays observable.** `status()` on a retained handle must
   still read `FINISHED` / `KILLED` after the thread's storage is gone, so the block
   cannot live in the coroutine frame (freed by `coro.destroy` at completion), and a
   child's block cannot simply be recycled for the next child — a stale handle would
   silently name a different thread.
3. **Reclamation must be deterministic and unbounded-safe.** A UVM run spawns and
   kills per transaction; finished blocks have to go back on a freelist rather than
   accumulate.

Hence two tiers for the block and a third, lazy tier for the object. This is the
shape `event-handles.md` already ships for `rt_event`: a codegen-owned static block
registered as the runtime pool's first chunk, one address space, one resolver
(`rt_event_pool_init`, `src/runtime/Runtime.cpp:5474-5479`, called from
`:5729`).

### 4.1 The control block (native, untraced)

A backend `StructType` mirrored in `Runtime.h` only because the pool allocator
needs its size — the `disable_node_ty` / `rt_event` convention of keeping the two
declarations in lockstep.

```
%clotho.pcb = type {
  i32  ctl,          ; state in bits 0-2 (the enum's own 0..4); flags above:
                     ; ABORT (3), WAKE_PENDING (4), SELF_PARKED (5), WAIT_FORK (6)
  i32  refs,         ; 0..2: one for a live thread, one for an existing object
  ptr  acts,         ; activation-node stack, innermost first (frame-resident nodes)
  ptr  waiters,      ; await waiter list head (frame-resident nodes)
  ptr  rng,          ; this thread's xoshiro state, or the module-wide state
  ptr  join_rec,     ; the heap fork record this thread is a child of, or null
  ptr  obj,          ; WEAK memo: the process object naming this block, or null
  ptr  parent,       ; always a live thread or a static block (§5.3)
  ptr  first_child, ptr next_sib, ptr prev_sib
}
```

State and flags share one word so the poll of §6 is one load and one masked
compare. Nothing here is a class handle, so **the collector never sees the tree,
the park state or the memo**: `kill`, `disable fork` and `wait fork` walk native
memory with plain GEPs, and `Gc.cpp`'s "a precise handle field must resolve"
assertion (`:796`) and `push_precise_roots`' fatal on a non-heap root (`:845-848`)
are simply never in play. That is the property the earlier draft had to buy with
uniform GC allocation, and it comes free from the split.

**Tier 1 — static processes.** `@clotho.proc_pcb : [nproc x %clotho.pcb]` in module
state, indexed by process index. Zero allocation, a constant address, and
`self()` in a process body folds to a constant GEP. Initialized in the ramp
(`ctl := RUNNING`, `refs := 1`, the rest zero).

**Tier 2 — fork children.** `rt_proc_alloc()` / `rt_proc_release()` over a chunked
pool with a freelist: chunks never move, so a block address survives pool growth
while a thread is parked on it (`rt_event`'s stated reason,
`src/runtime/Runtime.h:1385-1387`). A child's ramp takes a block; the release rule
is §4.2. Chunk-allocated rather than `rt_alloc`-per-child so a `join_none` loop is a
freelist pop, not a malloc.

### 4.2 The object, and who owns what

The `process` class Ref is registered with a **one-property synthetic body** —
`chandle pcb` — rather than by walking slang members (it has none):
`lower_class_type` gains one narrow special case for `std::process`. Everything
downstream then works unchanged: `class_body_ty` (`:1825-1841`), the `agg_member`
GEP chokepoint (`:2300-2301`), `HandleDeref`'s null check (`:13005-13020`), the
ordinary class handle in the 64-bit two-state plane, `null == 0`, `==` as an
`icmp`. Its GC trace map carries **zero handle offsets** (a `chandle` is excluded
from `collect_class_handles`, which keys on `Type::Kind::Class`), so tracing a
`process` object costs one map lookup and no field walk.

Ownership is a two-count state machine, not general refcounting:

| holder | takes a `refs` count | drops it |
|---|---|---|
| the live thread | at ramp | at completion (§5.3) |
| the object | at lazy creation (§5.1) | in the object's finalizer |

`refs == 0` releases a tier-2 block to the freelist; a tier-1 block is never
released. So a block outlives its thread exactly as long as some object names it,
which is exactly how long the LRM lets a program read `FINISHED`.

`pcb->obj` is a **weak** slot: it is not a root, so an object whose handle the
program has dropped is collected normally, and the object's finalizer
(`clotho.classfini.process`, the existing per-class finalizer mechanism,
`:2082-2131`) clears `pcb->obj` and drops the count. A later `self()` then makes a
fresh object — unobservable, because the program no longer holds the old one. This
asymmetry (object → block strong, block → object weak) is what keeps the pair
acyclic; making the memo strong would leak every named thread's object for the run.

No generation counter is needed anywhere, which is the one place this differs from
`rt_event`'s `{slot, gen}`: a block cannot be recycled while an object names it,
because that object holds a count. The stale-handle class of bug the generation
half exists to catch cannot arise.

**`--gc off`**: the finalizer never runs, so a `process` object leaks and its
block's count never drops — identity stays stable and nothing is recycled. That is
the same trade every class body already makes (`LlvmBackend.cpp:12965-12968`: "With
--gc off the lifetime is leaked, which LRM 8.4 makes observably correct"), and it is
bounded by the number of distinct threads that called `self()`.

One rule the emitted code must obey: do not cache a control-block pointer across a
suspend if it came from a *pool* chunk — chunks are stable, but a released and
re-taken block is a different thread. Reach it from the ABI argument or from the
object at each use, as the `rt_event` deref rule already requires
(`event-handles.md:525-529`).

### 4.3 Which threads get a PCB

A PCB is per **thread**, and a thread in clotho is a process body or a fork child —
not a task activation, since a task runs in its caller's thread and
`process::self()` inside a task must name the caller.

In a design where `DesignUses::process` is clear, no PCB exists. When it is set,
every `ir::Process` and every `is_fork_child` body gets one. Narrowing that to a
statically reachable subset is possible (a closure over spawn/call edges from
bodies that mention a process op, in the shape of `body_activations`,
`:8978-9072`) but is deliberately **not** in the first cut: `kill` is transitive
over descendants, so the closure has to include everything reachable from any
`self()` site, which in a UVM-shaped design is everything. One GC bump-allocation
plus four stores per thread spawn is the honest price.

Consequences that must be handled rather than discovered:

- **A thunk-elided process has no frame and no handle** (`:9901-9904`), so a group
  whose body can reach a process op must be forced to a real coroutine. The
  mechanism exists: a payload-dependent test in `is_simple_always`
  (`src/ir/Analysis.cpp:1503-1560`, precedent at `:1521-1535` for
  `NamedBlock(remote)` / `DisableRemote`). A *killable* process is by definition one
  whose handle escaped through `self()`, so the payload test is sufficient — there
  is no need to deoptimise every process in the design.
- **Thunk groups are the only bodies shared across instances**
  (`share_twin_groups`, `:7280-7325`), so forcing a coroutine also removes the
  cross-instance sharing question for PCB-bearing bodies.
- **Non-Active processes get PCBs too** (PreInit, Preponed, Observed, ReNba,
  Final, Reactive — `src/ir/Module.h:100-172`). They are processes; `self()` inside
  one is legal and rare. Cost is `nproc` objects at startup.
- **Bodies with no calling thread pass null**: DPI export thunks, the `$monitor`
  ramp in `main`, VCD callbacks — exactly the set that already falls back to
  `rng_global_` (`:3440-3443`, `:20078`, `:10391`). `process::self()` with a null
  PCB is a located fatal (`process::self() outside a process`), modelled on
  `rt_null_handle`.
- **A pended assertion action block** inherits the monitor process's PCB. It is
  lowered as a task body and pended onto the reactive ring with the pending
  thread's `rng` already (`:17091`), so the PCB rides the same argv. The
  alternative — a PCB per attempt — is rejected: an attempt is not a process the
  LRM lets you name, and 16.14.6.4's flush is not modelled anyway
  (`disable.md:473-503`).

---

## 5. Identity, the activation stack, and the tree

### 5.1 `self()`

`ptr proc` becomes argument 2 of `declare_function` / `declare_task`, immediately
after `ptr rng`, present only when `DesignUses::process` is set. Every call site
that already pushes `rng_slot_` pushes `pcb_` beside it (function call `:19399`,
task call `:20490`, fork spawn `:20360`, `$strobe` pend `:17071`, reactive pend
`:17091`). `self()` in a process body can also fold to a constant load from
`@clotho.proc_pcb[k]`.

The object *is* the PCB, so `self()` is a load, not an allocation, and
`self() == self()` is an `icmp` on the same pointer. No memo slot, no per-call
`new` — which is the one place this design is simpler than Verilator's.

### 5.2 The activation stack

Every activation that can park links a `disable_node_ty` node (§2) onto
`pcb->acts`, head-first, and unlinks it at its epilogue: the process body frame at
ramp time, each task frame after copy-in (mirroring `:4514-4516`), each fork child
in its own PCB's stack (a child is its own thread, so its stack starts empty).
Functions never park and link nothing.

`acts` head is therefore the frame that is actually parked, and it carries the
three things an abort needs: the region-tagged handle (field 2), the frame slot
naming the live heap park record (field 3), and the scheduled-flag pointer
(field 4). This is what generalises `disable`'s per-target list to "whatever this
thread is currently parked on", and it is why `kill` has no `hard_park` refusal to
inherit: a thread parked behind a time-consuming task call is parked at its
innermost frame, which is on the stack.

Reusing `disable_node_ty` rather than inventing a second node type is deliberate;
a node can only be on one list, so an activation inside a remotely-named block
allocas two nodes, which is two pointers of frame, not a new mechanism.

### 5.3 The tree, and the re-parenting rule

A fork child's ramp links itself into its parent's child list
(`parent`/`first_child`/`next_sib`/`prev_sib`). On a thread's completion, in this
order:

1. `ctl.state := KILLED` if `ABORT` was set, else `FINISHED`;
2. **splice every live child into the parent's child list** (re-parent), fixing
   each child's `parent`;
3. unlink self from the parent's list;
4. wake the `waiters` list (§6.2);
5. null `acts` — the frames are going away.

Step 2 is what makes 9.6.3's "descendants of children that already terminated"
true, and it buys the invariant that carries `wait fork`:

> **A thread has no live descendants iff `first_child == null`.**

An `always` process never reaches this path (its loop re-arms), so its state stays
RUNNING/WAITING for the whole run, which is correct: only `kill`, `disable`, or
`$finish` end it.

---

## 6. The status word and the poll

### 6.1 Transitions

| transition | where |
|---|---|
| → RUNNING at creation | ramp, with the PCB store |
| RUNNING → WAITING | `emit_suspend` / `emit_suspend_raw` (`:4861-4903`), immediately before the `coro.suspend` |
| WAITING → RUNNING | the resume block of the same suspend |
| → FINISHED / KILLED | the completion sequence (§5.3) |
| → SUSPENDED | `suspend()` |
| SUSPENDED → RUNNING | `resume()` |

Two stores per park, in a gated design only. `status()` is `load ctl` masked to
three bits; `.name()` on the result goes through the existing enum-name path
(`lower_enum_method`, `src/Lower.cpp:15687`) because slang's synthesized
`EnumType` is an ordinary enum to clotho (`lower_type` has no enum arm — it
lowers as a 32-bit signed 2-state int, `:2145-2151`).

The LRM has no READY state, so a thread that is queued on a ring but not yet
resumed reads **RUNNING** ("not in a blocking statement"). Recorded as an
LRM-ambiguity decision; it is also unobservable before the thread's first run,
since nothing else can hold its handle yet.

### 6.2 The poll

The existing hook already sits in exactly the right place —
`lower_region:11855-11862`, after any op that can park, because the parking op's
own resume bookkeeping (a `wait` compacting itself out of its buckets) must run
first. It grows one arm:

```
if ((design_uses_disable_ || design_uses_process_) && tag_suspends(t)) {
    emit_disable_poll();        // per-block marks (unchanged)
    emit_task_abort_poll();     // per-activation mark (unchanged)
    emit_process_poll();        // new: ABORT | SUSPENDED on this thread's PCB
}
```

`emit_process_poll` is one load of `ctl`, one masked test, and two cold arms:

- **ABORT** → the thread's abort exit: `coro_final_bb_` for a process body (the
  cooperative-`$finish` shape, `:3581-3586`, `:3717-3725` — `deinit_body_strings`
  then the final suspend, and the resumer's `coro.done` destroys the frame exactly
  once), `task_epilogue_bb_` for a task or fork-child frame, which already skips
  copy-out under an abort (`:4527-4545`) and already runs the deinit walk before
  waking the parent (`:4546-4590`). Every frame in the chain aborts in turn: the
  innermost leaves and pushes its caller, the caller polls after its own
  `TaskCall` — which `tag_suspends` already classifies (`:22858-22870`) — and
  leaves too.
- **SUSPENDED** → set `WAKE_PENDING`, then suspend again without registering on
  any source. §7.

`tag_suspends` needs `ProcWait` added; `ProcCtl` does not park.

---

## 7. `kill`, `suspend`, `resume`, `await`

### 7.1 `kill`

```
kill(p):
  if p.state in {FINISHED, KILLED}: return          // LRM: no effect
  walk p's subtree (first_child / next_sib, backtracking via parent):
      for each thread q with state not in {FINISHED, KILLED}:
          q.ctl |= ABORT
          if q is not the running thread: force_wake(q)
  if p is the running thread: branch to this frame's abort exit
```

`force_wake(q)` is `lower_disable_list`'s inner block sequence (`:22750-22817`)
with the target read from `q->acts` instead of a per-target list node: claim the
park record's `state` if one is live, else push iff the scheduled flag is clear,
then set the flag. Deleting the "am I my own node" test in favour of an explicit
self branch mirrors `DisableAbort` (`src/Lower.cpp:7669-7671`,
`LlvmBackend.cpp:13907-13912`).

The subtree walk is an explicit pointer-chasing DFS using `parent` to backtrack —
O(1) extra space, no recursion in emitted code, and safe against the walk's own
mutations because marking never unlinks (a marked thread unlinks itself later, at
its own epilogue).

Four things must happen that a naive "mark and wake" gets wrong:

1. **The join count must be released.** A killed fork child that never reaches its
   epilogue would leave its parent's `join` blocked forever. Two candidate points:
   eagerly at kill (Verilator's `forkSyncOnKill`, `verilated_timing.cpp:298-307`)
   or at the abort epilogue. **Take the epilogue**: it is the path the abort
   already goes through, it keeps the ordering that makes a child's heap die before
   the parent resumes (`disable.md` Hazard 11), and it needs no `join_rec` bookkeeping
   at kill time. `join_rec` (PCB field 4) is then only for the *inverse* case:
2. **A killed fork *parent* must not be woken by its surviving children.** Under
   `join` the record is a frame alloca reused across sites (`fork_record_slot`,
   `:20277-20283`) and dies with the frame, so a child decrementing it afterwards
   is a use-after-free (`disable.md` Hazard 10). Therefore **in a process-control
   design every fork uses the heap, refcounted record**, exactly as
   `awaiter()` becomes a per-arm heap record under `design_uses_disable_`
   (`:4747-4761`), and the aborting parent nulls `record->parent` and releases its
   reference in its abort epilogue. This is a real, gated codegen change to
   `lower_fork_inst`, not a detail.
3. **A killed thread must never be woken again.** Its abandoned wheel timer is
   already neutralised by the claim; its **static** wait-bucket entries are never
   compacted (`:5001`) and point at `@clotho.proc_sched[k]`, so the abort exit
   leaves that flag **set to 1 permanently** — Hazard 5 of `disable.md` (a flag left
   at 1 makes an activation permanently unwakeable) is here a *feature*, and it is
   what makes destroying the frame safe. Its **dynamic** entries are compacted out
   by the woken activation itself, which is why single-signal `wait` compaction must
   be gated on `design_uses_process_` as well as `design_uses_disable_` (`:5116`).
4. **Nothing is destroyed from outside.** The force-wake guarantees the killed
   thread runs its own poll once, so there is no need for a lazy-reap path (§12).

### 7.2 `await`

An intrusive waiter list on the PCB, node frame-resident in the *awaiting*
thread's frame — the event object's design, verbatim
(`event_node_ty` `{prev, next, handle, flag, owner}`, `:6078-6081`;
`emit_event_link:8734-8763`; `emit_event_wake_list:8690-8728`, which reads `next`
before waking because the woken activation unlinks its own node). Factor that walk
into one shared `emit_wake_list(head)` used by events, `await` and `wait fork`;
three users is exactly the threshold the style rules set for factoring.

```
await(p):
  if p == self: located fatal ("process::await() on the current process")
  while p.state not in {FINISHED, KILLED}:
      link node onto p.waiters; suspend; unlink
```

The loop, rather than a single park, is what makes a spurious wake harmless.
`await` is a task (slang: `SubroutineKind::Task`), so it parks; `ProcWait` joins
`tag_suspends` and `blocks_thunk`, and `emit_suspend`'s existing refusal for a
suspension inside a task called from a `program` block (`:4883-4903`) applies
unchanged.

### 7.3 `suspend` / `resume`

```
suspend(p):
  if p.state in {FINISHED, KILLED}: return
  p.ctl.state := SUSPENDED
  if p == self: p.ctl |= SELF_PARKED; suspend with no registration

resume(p):
  if p.state != SUSPENDED: return                  // no-op; LRM silent
  p.ctl.state := RUNNING
  if p.ctl & (WAKE_PENDING | SELF_PARKED): clear both; force_wake(p)
```

The subtlety is entirely on the *target* side, and it is handled by the poll, not
by touching any queue:

- Target parked on a `#delay`, event or `wait`: it stays parked. Its source fires
  normally, it is resumed, and its poll sees SUSPENDED → sets `WAKE_PENDING` and
  parks again with no registration. `resume()` then pushes it, and it continues
  **in the current time step** — which is precisely the LRM sentence quoted in §1.
  If the source has *not* fired by the time `resume()` runs, the registration is
  still live and untouched, which is the "re-sensitize" half of the same sentence,
  for free.
- Target queued on a ring at the moment of `suspend()`: the drain resumes it and
  it lands at its resume block, whose first instructions are the poll. No user
  statement runs, so "the effect is immediate" holds observably.
- Target running: impossible. The simulator is single-threaded through the
  scheduler, so the caller is the only running thread.
- A killed *and* suspended thread: KILLED wins — the poll tests ABORT first.

`force_wake` pushes through `emit_wake_push` on the stored **region-tagged** handle
(`:9450-9484`), so a resumed program-block or `expect`-migrated thread returns to
the Reactive ring, not the Active one. That is why the node stores
`parked_handle(...)` and never a bare pointer.

### 7.4 `srandom` / `get_randstate` / `set_randstate`

`pcb->rng` points at the thread's xoshiro256++ state. `srandom(seed)` is the
existing `rt_urandom_seed` splitmix64 expansion
(`src/runtime/Runtime.cpp:4952-4958`), already used by `$urandom(seed)`.
`get_randstate` / `set_randstate` need two new runtime helpers that format and
parse the four state words (64 hex digits) into a `clotho.str`, exposed through
`expose()` (`src/ir/Jit.cpp:199-303`). A string that did not come from
`get_randstate` is a located fatal, not a silent ignore — the LRM makes it
undefined and clotho never silently accepts.

One gated cost: `srandom` may be called on *another* thread's handle, so a
targetable thread must own its state rather than falling back to the module-wide
`@clotho.rng` (`:6893-6898`). Statically, the target is unknowable, so a design
that uses any of these three methods forces `rng_own_` for every PCB-bearing body
(`build_rng_fns`, `:6765-6887`, whose `ir_draws_rng` seed set gains the three
tags). Per-object RNG for class `randomize` (`TODO.md:1432-1433`) is a separate
item and is not blocked by this.

---

## 8. `disable fork` and `wait fork` (fork V3, discharged)

Both become small, and neither needs anything beyond §4-§7:

- **`disable fork`** = `kill` over the current thread's descendants, self excluded:
  the same subtree walk, starting at `pcb->first_child`. Re-parenting (§5.3) is what
  makes it reach a grandchild whose parent already finished, which is what 9.6.3
  demands and what Verilator's model misses.
- **`wait fork`** = park until `first_child == null`: set `WAIT_FORK`, link a node
  onto the thread's own `waiters` list, suspend; a completing thread's step 4 (§5.3)
  additionally checks its parent's `WAIT_FORK` and wakes it when the parent's
  `first_child` has gone null. One pointer test, because of the invariant.

The two frontend refusals (`src/Lower.cpp:7614-7623`) and the independent
`ReactiveSafetyChecker` rejections (`:889-895`, `:926-930`) are then narrowed to
the cases that remain refused, if any.

This also **lifts, rather than works around, two of `disable`'s refusals**: with the
activation stack, a `disable` whose target body can park behind a time-consuming
task call or a fork has a claimable park (`hard_park`, `:8463-8501`). Doing that is
a separate, optional phase (P5), not a prerequisite — but the design should not
leave two mechanisms for "find this thread's park" in the tree, so `disable`'s
per-target node and the PCB stack should converge on the same node the first time
someone touches both.

---

## 9. Gating and cost

`DesignUses::process` (`src/ir/Analysis.h:164-182`) is set by any `ProcCtl` /
`ProcWait` op, in processes **or** functions (like `disable`, unlike `defer`). With
it clear, nothing below is emitted and `--emit llvm` must be byte-identical.

With it set, the design pays:

| site | cost |
|---|---|
| thread spawn | one `rt_gc_alloc` + PCB init + tree link |
| every parking activation | node link/unlink (two frame pointers, four stores) |
| every park | two `ctl` stores (WAITING/RUNNING) |
| after every park | one load + masked branch (shared block with the disable polls) |
| every call and task call | one extra pointer argument |
| every fork | the heap refcounted record, for all three join kinds |
| every `#delay` | the per-arm heap park record with a claim (already paid under `disable`) |
| every single-signal `wait` | self-compaction out of its bucket (already paid under `disable`) |
| every ring push | the overflow guard, via `ring_guard_wanted()` (`:9227-9229`) gaining the new flag |
| thunk-eligible groups reaching a process op | forced to a real coroutine |
| `randstate` designs | per-thread RNG ownership for every PCB-bearing body |

No sizing change is needed. A force-wake admits at most one queue entry per thread,
deduped by the scheduled flag, and every thread is either a process counted in
`nproc` or a fork child counted (saturating) in `fork_ring_slots` — the same
argument `disable.md:682-685` makes, and it holds for the same reason.

No `Wheel.h` change, no ring removal, no static-bucket compaction, no epoch, and
no handle array beyond the `nproc`-sized root table.

---

## 10. IR, printing, legalization

Two new tags, **appended at the end of the enum** — inserting mid-enum renumbers
every later tag and breaks `tests/sva_test.cpp`'s `monitor_shape` digests without
moving a single instruction (`disable.md:803-807`):

- **`ProcCtl`**, with a small op enum in the payload: `Self`, `Status`, `Kill`,
  `Suspend`, `Resume`, `Srandom`, `RandstateGet`, `RandstateSet`, `DisableFork`.
  Some produce a value (`Self`, `Status`, `RandstateGet`), some do not — payload-
  dependent, as `NamedBlock`'s `remote` already is.
- **`ProcWait`**, for `await` and `wait fork`: suspends, blocks the thunk path,
  joins `tag_suspends`.

Both need a `Printer` arm, an `Analysis` arm (`blocks_thunk`, and the
`is_simple_always` payload test), and a **not synthesizable** classification for
the same reason `Disable`/`DisableRemote` have one
(`docs/design/process-legalization.md:693-725` models only the two bare jumps, so
claiming otherwise would silently under-guard).

Frontend interception: a `CallExpression` whose subroutine carries
`MethodFlags::BuiltIn` and whose parent scope is the `std::process` class routes to
a `lower_process_method` before the ordinary user-call path
(`lower_user_call:22332`, `lower_task_call:22393`). `is_user_virtual` already
excludes `MethodFlags::BuiltIn` from dispatch (`:2280-2282`), so no vtable slot is
minted for these. The type-level guard at `:2178-2181` is narrowed by class name —
`semaphore`, `mailbox` and `weak_reference` keep failing loudly and keep their
TODO.

---

## 11. Hazards

Ranked by how much damage a naive implementation does.

1. **A killed process whose frame is destroyed while a static bucket entry still
   names it.** The entry is never removed, the scan pushes whatever it holds, and
   the handle is freed memory. The mitigation is the permanently-set scheduled flag
   (§7.1.3); get it wrong and the failure is a silent resume of a freed frame.
2. **The `join` fork record is frame-resident and reused across sites**
   (`:20277-20283`). Killing a fork parent, or killing a child whose parent later
   dies, is a use-after-free until every fork in a process-control design uses the
   heap refcounted record. Four distinct failure shapes are enumerated at
   `disable.md:598-608`.
3. **A `join_any`/`join_none` child's `ArgDir::Ref` captures point into the
   parent's frame.** Killing the parent leaves the child holding dangling formals.
   Today captures are refused outright for the detaching kinds
   (`src/Lower.cpp:7790-7795`), which is what makes this latent rather than live —
   so **that refusal must stay until fork V4's per-spawn copies land**, and a
   `join` parent that is killed with live children must be treated as the same
   hazard (its children are killed too, but they abort *later*, after the frame is
   gone). The conservative rule: a killed thread's children are marked before the
   thread takes its own exit, and a child's abort path must not touch a capture
   before its own poll.
4. **Double wake.** A force-wake does not cancel the park it interrupts
   (`disable.md` Hazard 18). Every wake must go through the claim/flag protocol; a
   second resume lands at whatever suspend point the thread has since reached.
5. **`coro_cleanup_bb_` still leaks frame-owned heap** (`:3588-3595`). The abort
   path must reuse the epilogue / final-suspend shapes, never the cleanup block.
6. **A task's scheduled flag is a frame alloca** (`:4440`), so it is meaningless
   once the frame dies and unreachable from the killer. Only the activation itself
   may clear it — another reason the abort is self-driven.
7. **The GC must not see `acts` / `rng` / `join_rec` as object references.** The
   `chandle` typing of §4.1 is what guarantees that; a future refactor that retypes
   them to a class handle would hand the collector a coroutine frame pointer.
8. **A PCB pointer cached across a suspend** breaks the day evacuation is enabled.
   Same rule as `rt_event*` (`event-handles.md:525-529`).
9. **`process` in a container inherits a hard refusal.** `process q[$]` /
   `process p[]` hits `elem_ops_for`'s class-handle-in-container error under `--gc`
   (`:1503-1516`). This is a **real UVM-shaped blocker** and it is owned by
   `garbage-collection.md` §3.3, not by this design. It must be stated in the
   refusal message, because "process is supported" plus "this queue is not" is
   otherwise a confusing pair.
10. **`Structural::walk` has no `Fork` case and `scan_finish` omits `Fork`**
    (`fork-join.md:344-347`). A process-control design leans harder on both; check
    before relying on `BodyFacts::calls` for the PCB-reachability closure if §4.3's
    narrowing is ever implemented.
11. **Oscillation counter.** Each kill adds up to one activation per marked
    descendant in the same instant, and the drain's limit check counts them
    (`clotho.osc_count`, `:10025-10028`, reactive twin `:10204-10207`); bounded by
    the subtree size rather than by a loop, but worth a test at the limit.
12. **`$finish` interaction.** Threads parked at `$finish` are never resumed and
    their PCBs read WAITING forever; nothing observes it. Conversely, the PCB tree
    is exactly the enumerable live-thread set that
    `docs/design/cooperative-finish.md:106-112` phase 3 (teardown) lacks — a future
    win, but only for gated designs, so teardown cannot depend on it.

---

## 12. Rejected alternatives

- **A `{slot, gen}` handle into a runtime-owned PCB table**, the event-object model
  (`event-handles.md` §3). It buys stale-handle detection and needs no collector,
  but it needs its own refcount discipline (retain/release on every handle copy,
  a container element ops kind, a finalizer) for an object the collector can
  already manage, and `garbage-collection.md:43-44` already lists `process` as in
  scope for the collector. Rejected as a second lifetime discipline.
- **Refcounted PCBs.** Same objection, plus `garbage-collection.md:68-77`'s
  argument against refcounting in a UVM-shaped heap.
- **Lazy reap: mark KILLED and simply never resume the thread again**, destroying
  the frame whenever its handle next surfaces. This is Verilator's model
  (`verilated_timing.h:139-152` — resume only if the process is not killed). It is
  genuinely simpler, and it is wrong here for two independent reasons: a thread is a
  *chain* of coroutine frames, and destroying the innermost leaks every outer frame
  because nothing will ever resume them; and the destroy path
  (`coro_cleanup_bb_`) does not run the deinit walk, so every frame-local string
  and container leaks. The cooperative abort walks the chain and releases the heap,
  using code that already exists.
- **`coro.destroy` from the killer.** Use-after-free on a queued handle, no ring
  supports removal, and it skips the deinit walk. `disable.md` Hazard 3.
- **A global "current process" pointer** instead of the ABI argument (Verilator's
  `t_currentp`). Cheaper per call, but it needs a store at every resume site in the
  drain, the region runners, the fork spawn loop and the pend paths, and a missed
  site is a silently wrong identity — the one failure class the ABI approach cannot
  have. Also worth noting: the ABI already carries a per-thread pointer for the
  RNG, so this adds a register, not a mechanism.
- **A per-target epoch counter** for kill, as `disable`'s pre-V2 design proposed.
  V2b already established that a list of activations is strictly more useful than a
  counter saying one of them is stale (`disable.md:722-725`); the same holds per
  thread.

---

## 13. Phased plan

Each phase is independently landable and independently testable, and every phase
keeps the ungated IR byte-identical.

- **P0 — the structure, no user-visible feature.** Narrow the std-package guard by
  class name; register the synthetic `process` class body and its trace map;
  `DesignUses::process` + the ABI argument + `@clotho.proc_pcb` root table; PCB
  creation in both ramps; the activation-node stack; the status word transitions;
  the poll arm; the forced-coroutine test; the gate widenings
  (`ring_guard_wanted`, the park claim, single-signal `wait` compaction, heap fork
  records). `process p;` and `p == null` work; every method still refuses by name.
  Acceptance: byte-identical `--emit llvm` on picorv32, every `examples/` design
  and `tests/perf`, with the feature unused.
- **P1 — `self()`, `status()`, `await()`.** Adds the waiter list and the shared
  `emit_wake_list`. No termination, so no abort paths yet. This is the phase a
  UVM-shaped smoke test can first exercise.
- **P2 — `kill()` and `disable fork`.** The subtree walk, the abort exits, the
  join-record ownership change, the killed-thread wake discipline, self-kill.
  The largest phase and the one carrying hazards 1-6.
- **P3 — `suspend()` / `resume()` and `wait fork`.** `WAKE_PENDING`, the
  `first_child == null` barrier.
- **P4 — the randstate trio.** RNG ownership forcing, the string codec, two runtime
  entry points.
- **P5 — optional cleanups, once P2 is proven.** Lift `disable`'s `hard_park`
  refusals through the activation stack; converge `disable`'s per-target node and
  the PCB stack; narrow §4.3's PCB set with a reachability closure if profiling
  says the allocation matters.

`process` in a queue / dynamic array / associative array is **not** a phase here:
it is `garbage-collection.md` §3.3's container trace work, and it gates
`process q[$]` regardless of anything in this document.

---

## 14. Verification

**There is no oracle, and this needs a decision before P1 lands.** Probed on
this workstation:

- **iverilog 13.0 cannot parse the class at all** — `process p;` is a syntax
  error (`Invalid module instantiation`), as is `process::self()`. So the
  primary oracle the project prescribes is unavailable for every case in this
  document.
- **Verilator 5.050 implements a useful subset** and is a partial oracle:
  `self()`, `status()`, `await()`, `kill()` and object identity all work
  (`verilated_std.sv:141-200`), and `status()` returned `RUNNING` for `self` and
  `WAITING` for a child parked on a `#100` in a probe run here. But
  **`suspend()` is an explicit `$error("std::process::suspend() not supported")`**
  (`verilated_std.sv:181-183`), `resume()` merely sets RUNNING, and `kill()` does
  **not** kill descendants of an already-finished child (`verilated_types.h:338-343`
  detaches rather than re-parents). So it cannot score §7.3 at all and cannot score
  §5.3's re-parenting rule.

That needed an explicit choice, and it is **settled: score the
Verilator-supported subset against Verilator 5.050, and pin everything it does not
implement from the LRM text**, with each pinned case naming the sentence it
encodes — the approach `fork-join.md:508-511` already takes for the three cases
`vvp` cannot run. Concretely:

| case group | oracle |
|---|---|
| identity, `status()` over RUNNING / WAITING / FINISHED, `await()` before and after completion, `kill()` of a live thread and of a dead one | Verilator 5.050 |
| `suspend()` / `resume()` in every shape (§7.3) | LRM 9.7 only — Verilator `$error`s |
| `kill` / `disable fork` reaching a grandchild whose parent already finished (§5.3) | LRM 9.6.3 only — Verilator detaches instead of re-parenting, so scoring against it would encode its deviation |
| `wait fork`, the RUNNING-when-queued reading, the `await`-on-self fatal, `set_randstate` on a foreign string | LRM only (§15 records each decision) |

A Verilator-scored case must state which of the two it is in the test comment, so a
later Verilator release changing behaviour cannot silently reclassify a pinned case.

Regardless of oracle, the suite owes:

- **Ordering and value cases per method**, in a new `tests/process_test.cpp` added
  to the explicit TU list (`CMakeLists.txt:515-584`): identity, all five statuses
  including a killed and a finished thread read *after* completion, `await` before
  and after completion, `await`-on-self as a fatal, kill of a parked / queued /
  suspended / already-dead thread, kill of a subtree two levels deep with the
  middle thread already finished, self-kill, `kill` inside a task called by the
  target thread, suspend-then-resume across a transpired delay and across an
  untriggered event, `disable fork` / `wait fork` shapes lifted from
  `fork-join.md`'s owed list (LRM 9.6.2 example 6 among them).
- **The three GC modes.** `tests/gc_test.cpp`'s `check_all_gc_modes` harness
  (`:80-84`) is the right vehicle: a PCB is a class object with tree links, so
  Off/On/Stress must all pass, and a stress run is what catches a PCB collected
  under a live thread.
- **ASan + UBSan + LeakSanitizer**, which is what actually covers the fork-record
  ownership change: a missed release leaks and a double release is a double free,
  and neither shows up in a value check (`fork-join.md:504-506`).
- **Byte-identical `--emit llvm`** for every design that does not use the feature,
  at every phase — the gate `disable.md:646-654` established, verified by hash
  rather than argued.
- **The refusal messages**, each by its own name: `semaphore`, `mailbox`,
  `weak_reference` still refusing after the guard is narrowed; `process` in a
  container; `self()` with no calling thread.

---

## 15. LRM decisions this design records

- A thread queued on a ring but not yet resumed reads **RUNNING**; the LRM has no
  READY state (§6.1).
- `resume()` on a process that is not SUSPENDED is a **no-op**; the LRM does not
  say.
- `set_randstate` with a string not produced by `get_randstate` is a **located
  fatal**; the LRM makes it undefined and clotho never silently accepts.
- A killed thread's already-armed nonblocking assignments and intra-assignment
  records **still commit**; the LRM leaves the analogous `disable` cases
  unspecified (`disable.md` Hazard 14) and this takes the free behaviour.
- Output/inout copy-out of an aborted task activation is **skipped**, reusing
  9.6.2's unspecified-results licence and the landed abort path.
- A pended assertion action block **inherits the monitor process's PCB** rather
  than getting one of its own (§4.3).
- `KILLED` is shared by `kill` and by `disable`, per 9.7's own definition of the
  state.
