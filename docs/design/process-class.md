# `process` class and process reflection (LRM 9.7)

Target: the built-in `process` class — `self`, `status`, `kill`, `await`, `suspend`,
`resume`, `srandom`, `get_randstate`, `set_randstate` — plus the two statements that
need the same runtime structure and were refused for exactly that reason,
`disable fork` and `wait fork` (LRM 9.6.1/9.6.3, fork V3).

Status: **landed, all nine methods plus both fork statements.** Phases P0-P4 below
are done in four commits; §13 records what each cost against what it predicted. The
`std`-package refusal is now keyed by class name, so `semaphore` / `mailbox` /
`weak_reference` keep theirs (`src/Lower.cpp`, `lower_class_type`).

This is an M4 feature, not a nice-to-have: a UVM testbench stores a `process`
handle in a class property, calls `process::self()` inside a forked thread and
`kill()` on it later, and `uvm_objection`-style code awaits completion. Nothing in
the tree was close — `docs/design/disable.md:189-196` still states the starting
point: *"No process identity, no process table, and no handle array. A process is
the raw `ptr` from `llvm.coro.begin`."*

Two documents deferred to this one and both understated it:

- `docs/design/disable.md` V4 (`:745-768`, corrected there) said `kill()` is "V2's
  abort applied to a dynamic handle, `await()` a join barrier, `suspend()`/`resume()`
  a desensitise pair". The first is true only for a *single* activation of a process
  body; a thread is a chain of coroutine frames and a subtree of descendants. The
  third is wrong outright: nothing can be desensitised, because the wheel cannot
  cancel and a static wait bucket cannot be compacted (§7.3).
- `docs/design/fork-join.md` V3 (`:459-484`) said `wait fork` / `disable fork` need
  "an enumerable live-descendant structure" that **no phase of either document
  commits to building**. This design built it once, and all three features fell out
  of it. That is the load-bearing architectural claim here, and it held: the control
  block tree IS fork V3's missing structure, not a parallel one.

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
registered as the runtime pool's first chunk, one address space, one resolver. Both
pools are re-registered at run start by `rt_run_init`
(`src/runtime/Runtime.cpp:5899`), emitted at the top of the design's main.

### 4.1 The control block (native, untraced)

A backend `StructType` mirrored in `Runtime.h` only because the pool allocator
needs its size — the `disable_node_ty` / `rt_event` convention of keeping the two
declarations in lockstep.

```
%clotho.pcb = type {                    ; == rt_proc (Runtime.h), field for field
  i32  ctl,          ; state in bits 0-2 (the enum's own 0..4); flags in the high
                     ; bits: STATIC (8), ABORT (9), WAKE_PENDING (10),
                     ; SELF_PARKED (11), WAIT_FORK (12)
  i32  refs,         ; 0..2: one for a live thread, one for an existing object
  ptr  acts,         ; activation-node stack, innermost first (frame-resident nodes)
  ptr  waiters,      ; await / wait-fork waiter list head (frame-resident nodes)
  ptr  rng,          ; this thread's xoshiro state, or the module-wide state
  ptr  join_rec,     ; reserved: the heap fork record a killed child decrements
  ptr  obj,          ; WEAK memo: the process object naming this block, or null
  ptr  parent,       ; always a live thread or a static block (§5.3)
  ptr  first_child, ptr next_sib, ptr prev_sib,
  ptr  next_marked   ; scratch chain of the blocks one `kill` marked (§7.1)
}
```

Two changes from the design as first written. `STATIC` moved into the flag bits
because the static tier needed to be recognisable at run time (a reference count
reaching zero on a static block is not a free), and `next_marked` was added so the
subtree WALK can live in the runtime -- plain memory work -- while the force-wake it
feeds stays in generated code, which is what touches the scheduler rings. The
`join_rec` slot is still reserved: the abandon path (§11 hazard 2) turned out to need
the record in a FRAME slot rather than on the block, because the thread that abandons
a join is the one whose frame holds it.

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

`ptr proc` is argument 2 of a function and argument 3 of a task -- right after the
fixed prefix in each, so `state` / `rng` / `wheel` keep their indices and only
`param_indices`' formal base moves -- present only when `DesignUses::process` is set. Every call site
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
| RUNNING → WAITING | `emit_suspend` (`:4861-4903`), immediately before the `coro.suspend` |
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
5. **A thread marked before it has ever run must not run its head segment.** Found
   landing LRM 24.3 (`program-blocks.md` §5.1), and it applies to `kill`,
   `disable fork` and `$exit` alike. `pcb->acts` is linked in the BODY, after the
   initial suspend, so a thread that has been ramped and queued but not yet resumed
   has no activation for the mark walk to force-wake: it is marked, correctly
   skipped by the walk, and then resumed by the drain anyway. Every other abort poll
   sits AFTER a suspending op, so such a thread would run everything up to its first
   park — printing, storing, even forking threads of its own — after having been
   terminated.

   Fixed by testing the mark once on body entry. **Two body kinds need it, and they
   are the whole set:** a fork child (`lower_task`), reached by `kill` /
   `disable fork` on a just-spawned `join_none` child; and a **program `initial`**
   (`lower_group`), reached by `$exit`, which names every `initial` of its instance
   by that procedure's STATIC control block — including one the seed loop has queued
   but the reactive drain has not yet resumed. Nothing can name a design process
   before it starts, so no other body kind is exposed. A program `initial` caught
   this way leaves through `program.end`, which counts it down: 24.3 asks only that
   a procedure have "reached its end", and one terminated by `$exit` has. Invariant
   4's "runs its own poll once" now holds for a thread that had not started as well
   as for one that had.

   **Why those two are the whole set**, by enumeration of every way a mark can reach
   a thread — re-check this if you add a `lower_proc_kill` caller:

   | caller | target | can it name an unstarted thread? |
   |---|---|---|
   | `p.kill()` | a runtime `process` handle | **no** — the only producer of one is `process::self()`, so the thread has executed inside itself; `process` is abstract and cannot be constructed |
   | `disable fork` | `pcb_`, `with_self=false` | **yes**, a just-spawned `join_none` child — the `lower_task` entry test |
   | `$exit` (24.7) | each `initial`'s STATIC slot, `with_self=true` | **yes**, a seeded-but-unresumed `initial` — the `lower_group` entry test |
   | `$exit` / 24.3 | the program instance's termination root | not a thread at all |

   `proc_pcb_slot()` has exactly three callers — its own definition, the `pcb_`
   assignment in `lower_group`, and `$exit` — so `$exit` is the only thing in the
   tree that names a thread by static slot, which is what makes the third row the
   only static-slot exposure.

   Stated generally, because the table is the special case: `pcb->acts` is linked in
   `start_bb`, i.e. only on a thread's FIRST RESUME, so **any** future way of naming
   a thread before that reopens this — not merely a new `lower_proc_kill` caller.
   Naming by handle cannot, since a `process` handle only exists once the ramp has
   returned; naming by static slot can, which is what `$exit` does.

6. **A force-wake must not push a thread that is RUNNING.** The walk skips only the
   CALLING thread (`p == pcb_`), and the runtime cannot tell running from queued, so
   the scheduled flag is what stands between a force-wake and a second queue entry
   for a thread that is neither parked nor the caller. That invariant held for an
   always loop (flag held set across the body) and for any woken thread (the waker
   sets it), but **not** for a program `initial` in its FIRST activation: the seed
   loop enqueued it without marking it scheduled. `$exit` from a `fork ... join`
   child reaches exactly that state — `join` resumes a child inline, so the parent is
   on the stack, its park record null, its flag clear — and the push would queue a
   handle whose frame the resumer is about to destroy. Fixed by marking a Reactive
   process scheduled as the seed loop enqueues it, which is what the `self_start` arm
   beside it already did for the analogous double-push. "Set while running, clear
   while parked" now holds for every body kind.

Two operations were added to this machinery by clause 24 and belong here rather
than in `program-blocks.md`, since both are general:

- **`rt_proc_adopt(to, from)`** moves `from`'s live children under `to`. Needed
  because `rt_proc_complete` re-parents an ending thread's children onto its own
  parent, which is **null** for a statically created process — so a program with two
  `initial`s would drop the first one's children out of the tree entirely before the
  second ended. A program instance therefore owns a static block that is a tree node
  and never a thread (`clotho.program_root`), and every program body hands its
  children to it on the way out. Nothing else in the tree needs a non-thread node,
  which is why this is a separate entry point rather than a change to
  `rt_proc_complete` — that function is on the path of every thread completion in
  every design.
- **`lower_proc_kill(..., branch_on_self)`**, defaulted true. False for exactly one
  caller, `$exit`, which kills several roots in a row: the existing `with_self`
  self-branch fires immediately after the matching root's walk, which would leave the
  walks after it unreachable. `$exit` marks every root first, then ends the calling
  thread with one unconditional branch.

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

**Two later sources turn the machinery on without the source naming `process`**, both
from LRM clause 24, because a program's termination rules ARE this subtree abort and
its tree is these control blocks (`docs/design/program-blocks.md` §5):

- `Inst::Tag::ProgramExit` (`$exit`, 24.7) sets the fact directly. It has to: 24.7
  terminates a program's `initial` procedures themselves, so the threads it marks are
  the statically created ones, and a program with no `fork` anywhere still needs their
  blocks.
- 24.3's implicit descendant termination sets it in the backend rather than the
  analysis, at the one site that already folds design-wide questions into the flag:
  `design_uses_process_ = facts_.uses.process || (has_program_initial() && design_forks())`
  (`src/ir/LlvmBackend.cpp`, `run()`). Narrowed by `design_forks()` deliberately —
  with no fork in the design a program `initial` has no descendant thread, so the
  walk is provably empty and a program-only design keeps paying nothing.

So "no `process` in the source" no longer implies "no PCBs"; "no `process`, no
`$exit`, and no `fork` alongside a program" does. A design with no program at all is
unaffected either way.

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
3. **CLOSED by fork V4. A fork child's captures used to point into the parent's
   frame** (`ArgDir::Ref`), so killing the parent left the child holding dangling
   formals. This was latent only because captures were refused outright for the
   detaching kinds — and the note here used to say that refusal "must stay until fork
   V4's per-spawn copies land", plus that a killed `join` parent is the same hazard
   (its children are killed too, but they abort *later*, after the frame is gone).
   That second half is what mattered: the `join` case was REACHABLE, with an ordinary
   `logic [7:0]` capture, because `join` was allowed to capture. A fork capture is now
   a GC box handle for every join kind (`docs/design/fork-join.md` V4) — the child's
   frame holds a traced handle, a live coroutine frame is a conservative GC root, and
   the box outlives the parent activation with no refcount — so there is no pointer
   into a parent frame left to dangle and no refusal propping the hazard up. What
   remains is the ordinary rule, unchanged: a killed thread's children are marked
   before the thread takes its own exit.
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

## 13. Phased plan — as built

Each phase was independently landable and independently testable, and every one kept
the ungated IR byte-identical (verified per commit on
`examples/{nba,alu,wait,gates_bench}`). P0 and P1 landed together, because a
structure with no reachable method could not be smoke-tested; P5 is still open.

- **P0 — the structure, no user-visible feature. LANDED with P1.** Narrow the std-package guard by
  class name; register the synthetic `process` class body and its trace map;
  `DesignUses::process` + the ABI argument + `@clotho.proc_pcb` root table; PCB
  creation in both ramps; the activation-node stack; the status word transitions;
  the poll arm; the forced-coroutine test; the gate widenings
  (`ring_guard_wanted`, the park claim, single-signal `wait` compaction, heap fork
  records). `process p;` and `p == null` work; every method still refuses by name.
  Acceptance: byte-identical `--emit llvm` on picorv32, every `examples/` design
  and `tests/perf`, with the feature unused.
- **P1 — `self()`, `status()`, `await()`. LANDED.** The waiter list is the event
  object's node type (`event_node_ty`) rather than a new one, and the wake walk was
  written fresh rather than factored out of `emit_event_wake_list`: the third user
  (`wait fork`) arrived one phase later and shares the process one, so the two lists
  still have two walks. Factoring them is a follow-up, not a debt with a bug in it.
- **P2 — `kill()` and `disable fork`. LANDED**, and it did carry hazards 1-6. Two
  things the plan did not say: the force-wake protocol was FACTORED out of
  `lower_disable_list` into `emit_node_force_wake` rather than duplicated (both sides
  publish the same `disable_node_ty`, so the claim-or-flag chain is now written once);
  and KILLED has to be published where `kill` RUNS, not where the killed thread
  notices, or a caller that kills a thread and reads its status sees WAITING.
- **P3 — `suspend()` / `resume()` and `wait fork`. LANDED.** The one thing the plan
  missed cost two bugs before it was found: the RUNNING <-> WAITING pair maintained at
  every park has to be a CONDITIONAL transition. An unconditional store at a park
  erased a SUSPENDED another thread had just set -- its `resume` then found nothing
  suspended and the thread never came back -- and would equally erase a KILLED. It is
  now one load, one compare, one select, one store (`emit_proc_state(from, to)`).
- **P4 — the randstate trio. LANDED.** RNG ownership forcing behaves as designed
  (`DesignUses::process_randstate` -> every thread owns its state). The string codec is
  64 lowercase hex digits, and `set_randstate` rejects both a malformed string and the
  all-zero state -- xoshiro's fixed point, which `get_randstate` can never produce.
- **P5 — still open, and now unblocked.** Lift `disable`'s `hard_park` refusals
  through the activation stack (a thread parked behind a task call or a fork is
  reachable now -- `kill` proves it); converge `disable`'s per-target node with the
  activation stack; factor the three waiter-list walks; narrow §4.3's control-block set
  with a reachability closure if profiling ever says the pooled allocation matters.

Two deliberate refusals survive their own implementation, both for the same identity
reason: `disable fork` and `wait fork` inside an ASSERTION ACTION BLOCK. An action
block is pended as a task body and runs on the monitor process's control block, so the
descendants it would end or wait for are the monitor's, not the assertion's.

`process` in a queue / dynamic array / associative array is **not** a phase here:
it is `garbage-collection.md` §3.3's container trace work, and it gates
`process q[$]` regardless of anything in this document.

---

## 14. Verification

**There is no oracle.** Probed on this workstation, and this is what the test
file's per-case oracle notes record:

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

**Landed:** `tests/process_test.cpp` (in the explicit TU list), 27 cases over the
whole surface -- identity within and across threads, all five statuses including a
killed and a finished thread read *after* completion, `.name()` on the status enum,
`await` before / after completion / from inside a task / with two awaiters / on a
killed target, `kill` of a parked thread, of a subtree, of a thread parked inside a
time-consuming task call, of a parent parked at a `join` with a live child, of self
and of an already-dead thread, `disable fork`, suspend-with-transpired-delay,
self-suspend-and-resume, both no-op shapes, `wait fork` over a grandchild and with
nothing forked, the randstate round-trip, cross-thread `srandom`, two threads seeded
alike, a handle in a class property, and the two surviving refusals.

Each case names its oracle in a comment: Verilator-scored ones say so, LRM-pinned ones
name the sentence they encode.

**ASan + UBSan + LeakSanitizer (RelWithDebInfo, `-DCLOTHO_SANITIZE=ON`), on the
process and fork suites: no errors.** That is the check that actually covers the fork
record's ownership change, the abort exits, the subtree walk and three intrusive
lists -- a missed release leaks, a double release is a double free, and a stale wake
is a use-after-free, none of which a value check would show.

The leaks LeakSanitizer does report are accounted for exactly, which is the point of
citing them rather than the summary line:

- `tests/process_test.cpp`, 35 cases: **6096 bytes in 30 allocations** = ONE
  5632-byte control-block pool chunk plus one 16-byte `process` object per design
  that leaks one. Measured under `-DCLOTHO_SANITIZE=ON`.
  The pool term is now O(1) in the process rather than O(designs). `g_proc_chunks`
  was never cleared and its chunks never freed, so before `rt_run_init` every
  design's chunks accumulated for the life of the `clotho_test` process; now each
  run starts by freeing the previous run's (§4.1's pool has no static first chunk,
  so every chunk is the pool's own). What LeakSanitizer still sees is the FINAL
  run's chunk, which by construction no later run exists to free — a reset seam
  bounds the pool, it does not tidy up after the last design, and that is the
  intended trade (`rt_gc_init` makes the same one).
  `tests/fork_test.cpp`, 39 cases, reports **384 bytes in 6 allocations and no pool
  chunk at all**, which is also the check that the V4 capture boxes are collected:
  a per-capture leak would show up there first.
  Class bodies still leak on this branch by direction (`LlvmBackend.cpp`,
  `ClassNew`: "with --gc off the lifetime is leaked, which LRM 8.4 makes observably
  correct"), and a `process` object is a class body.
- **The freelist demonstrably works.** 2000 spawn / `disable fork` cycles whose
  children never call `self()` leak *16 bytes in 1 allocation* -- one chunk's worth of
  blocks, recycled 2000 times. The same 2000 cycles where each child DOES call
  `self()` leak one block per child, because the object that names it leaks and a
  named block cannot be recycled. That is the `--gc off` cost of §4.2 stated as a
  measurement rather than a claim, and it is what the collector removes.

**Still owed:**

- **The three GC modes.** `tests/gc_test.cpp`'s `check_all_gc_modes` harness is the
  right vehicle once the collector is in the tree -- it is not on this branch. A
  `process` object is an ordinary class body, so Off/On/Stress must all pass, and a
  stress run is what would catch one collected while its thread is live. The control
  block itself is native and untraced, so the collector cannot reach the tree at all
  (§4.1), which is what makes this a check rather than a risk. The object's finalizer
  clearing `pcb->obj` (§4.2) is the one piece of the design that only exists on paper
  until then -- with no collector, the memo is never cleared, which is why the leak
  above is per named thread rather than per live one.
- **A UVM-shaped integration case**, once `process q[$]` is expressible (it inherits
  the container class-handle refusal, §11 hazard 9).
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
- **KILLED is published where `kill` runs**, not where the killed thread notices. 9.7
  defines the state as "process was forcibly killed", and a caller that kills a thread
  and then reads its status must not see WAITING. The unwind is driven separately, by
  the abort flag.
- **`await` treats KILLED as a completion**, so an awaiting thread is released by a
  kill rather than hanging on a thread that will never finish. 9.7 says await waits for
  "the completion of another process" and gives KILLED as a terminal state.
- **A suspended thread that is killed is KILLED**, not resumed: the poll tests the
  abort flag before the suspension state.
- **`set_randstate` rejects the all-zero state** as well as a malformed string. It is
  xoshiro256++'s fixed point, so accepting it would make every later draw zero;
  `get_randstate` cannot produce it, so it can only come from a foreign string, which
  18.14.4 leaves undefined anyway.
