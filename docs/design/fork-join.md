# Fork/join (concurrent process spawning, LRM 9.3.2)

Target: `fork … join` / `join_any` / `join_none`, plus `disable fork` /
`wait fork`, as concurrent sub-processes spawned inside an `initial`/`always`
block or task. This is the last core testbench primitive missing after
cross-hierarchy refs, `wait(expr)`, severity tasks, and immediate assertions.

Status: **V1–V5 landed, plus V4b.** `fork … join` / `join_any` / `join_none`, in a
process body, a task body and (for `join_none`) a function body; children capture the
enclosing frame's automatics as GC-box handles for every join kind, and a fork's own
`block_item_declaration`s are initialized in the parent before any child is spawned.
Phased: V1 = `fork … join`; V2 = `join_any` / `join_none` (§V2 scope);
V3 = `wait fork` / `disable fork`; V4 = captured-automatic lifetime, as GC boxes;
V4b = spawn-time initialization of a fork's own declarations (§V4b scope);
V5 = `fork … join_none` inside a function (§V5 scope).

V1 shipped with two statically-sized structures that a fork child reaches sized as
if it did not exist, and with no overflow guard, so an ordinary `fork … join`
corrupted memory. Both are fixed; §V2.0 records what they were. The guards' gate is
no longer fork-only either: `ring_guard_wanted()` is `design_forks() ||
design_uses_disable_`, because a remote `disable` force-wakes handles the bound was
not reasoned about (`docs/design/disable.md`).

## Semantics (LRM 9.3.2)

A `fork … join*` block runs each of its top-level child statements as a
separate concurrent process. The three join kinds differ only in when the
parent proceeds:

- **`join`** — parent blocks until **all** children complete.
- **`join_any`** — parent blocks until the **first** child completes; the
  remaining children keep running.
- **`join_none`** — parent does **not** block; children are scheduled but the
  parent continues in the same time step (children run when the parent next
  blocks or ends its region).

Related: `disable fork` terminates all active children (and their
descendants) of the current process; `wait fork` blocks until all of them
complete. Automatic variables declared in (or captured by) a child have
per-spawn lifetime — the classic `for (int i…) fork … use i … join_none`
capture requires each spawn to see its own `i`.

## Current state

- **slang**: a fork is a `BlockStatement` with `blockKind ∈ {JoinAll, JoinAny,
  JoinNone}` (`Statement.h`); `body` is the statement list of children.
  `DisableForkStatement` / `WaitForkStatement` are their own statement kinds.
- **Clotho**: all three join kinds lower in a process body and in a task body (an
  assertion action block is lowered as one). `disable` / `disable fork` at the
  `Disable` / `DisableFork` cases and `wait fork` are still refused, by name.
  slang itself rejects `join` / `join_any` inside a *function*
  (`diag::TimingInFuncNotAllowed`) but allows `join_none` there, clearing
  `ASTFlags::Function` inside the block so the children may hold anything a task
  may, so `lower_fork`'s own non-task check used to refuse it. LRM 13.4.4 requires
  that case to work, and it now lowers: §V5.

## The process model this builds on

(From the scheduler map — `src/ir/LlvmBackend.cpp`.)

A process/task is an LLVM switched-resume coroutine. "Create a process" and
"wake a process" are the same op (`coro.resume`): the ramp builds the frame,
parks at the initial suspend, and returns the handle. The scheduler drains a
FIFO **active ring** of ready handles (`emit_active_run` / `sim_drain`); a
suspended coroutine leaves the ring and lives in the timing wheel (for
`#delay`) or a signal wait-queue (for `@event`), rejoining the ring when its
timer fires or a matching store services its queue.

Two existing mechanisms are the load-bearing analogs:

1. **Tasks = per-call coroutine + continuation record.** A task `rt_alloc`s a
   fresh frame per call (unlike processes, ramped once at startup) and carries
   a `{ptr parent}` record. On completion its epilogue loads `parent` and, if
   non-null, `emit_active_push(parent)` — i.e. resumes the caller by
   enqueueing its handle. A blocking task call stores the caller's own handle
   into the record, then suspends; the task drives itself and wakes the caller
   on completion (`lower_task`, `lower_task_call_inst`).
2. **Seed = ramp + enqueue.** Spawning a coroutine at runtime is exactly the
   two steps the startup seed loop already does per process.

A forked child is therefore **a task whose continuation is a join barrier**
instead of the direct caller. Its only formals are the enclosing frame locals the
fork captured by reference (see §E), so a child of a fork in a *process* body is
argument-less.

## Blocker #1 and its derisk: the active-ring bound

The active (and inactive `#0`) ring is a fixed-size FIFO sized to
`bit_ceil(nproc)` — one live slot per process — resting on the documented
invariant *"without fork, each process is a single call-stack with at most one
ready coroutine"* (`ensure_active_queue`). Fork breaks this: children are
ready coroutines the static process count does not account for. A silent ring
overrun is memory corruption.

**`fork … join` is statically boundable; `join_any` / `join_none` are not.**

The ring holds only **ready** handles. A `join` parent blocks until all
children finish, and a *blocked parent is not in the ring* — its handle sits
in the join record (like a task's `parent` slot), pushed back only when the
join completes. So blocked parents cost zero ring slots, and children never
accumulate:

- **Loop**: `for (i<N) begin fork a; b; join end` — the parent blocks each
  iteration, so `a` and `b` complete before iteration `i+1` spawns. Peak live
  children = the fork's static width **2, independent of N**.
- **Nesting**: `fork t(); join` where `t` forks again — each level blocks at its
  own join and leaves the ring, so depth consumes `rt_alloc` frames and join
  records. It still consumes ring slots for the children of every level that is
  live at once, which is why the bound below multiplies a fork site's width by
  the activations of the body containing it rather than counting bodies.
- **Recursion**: a forking task that reaches itself has no static bound at all
  (each level is a fresh activation), so the bound saturates and the runtime
  check below is what catches it.

Safe static cap, as landed (`ensure_active_queue` / `fork_ring_slots`):

```
cap   = bit_ceil( nproc + Σ over fork-child bodies c of mult(c) )
mult(process) = 1
mult(f)       = Σ over call / spawn edges (b → f) of mult(b)
```

where a *spawn edge* is a `Fork` naming child `f` and a *call edge* is any
`Call` / `TaskCall` of `f`, and the sum runs over **edges**, not over distinct
sources. That last detail is the whole point: `Σ fork widths` alone counted child
*bodies*, which is only sound when each fork site has one live activation. A task
body is lowered once and shared by every call site, so `fork t(); t(); join`
gives `t` two live activations and a width-2 fork inside `t` needs **4** slots,
not 2. Relaxed to a fixpoint over the static graph at lower time; a DAG converges
in at most `function_count()` rounds.

Three shapes have no finite static bound and saturate to a ceiling
(`kMaxForkRingSlots`, one pointer per slot) instead of pretending to a number:
recursion into a forking task (a cycle in the graph), a virtual task call — the
bound `collect_callees` already documents — and a `join_any` / `join_none` spawn
edge, whose children accumulate with no static limit at all.

That last one is why the doc's original plan for V2 was dropped. It proposed a
**growable ring** (realloc on overflow), but the capacity reaches the emitted IR only
as `i32` immediates (the array extent of `queue_arr_ty_`, the mask at five index
sites, `cap` at two guard sites), so growth turns every push and pop into a
base-pointer load plus a mask load — a cost paid by every `fork … join` design on the
wake path, to buy a case only the detaching kinds can reach. Saturating the bound and
leaning on the guards (§V2.3) leaves every existing push and pop untouched instead.

**Defense in depth (landed):** in a design that forks, `emit_active_push`,
`emit_reactive_push` and `emit_wake_push` each check the bound before storing —
`tail − head >= cap` calls `rt_ring_overflow`, which prints a located FATAL naming
the buffer and ends the simulation. So a mis-computed bound or a saturated ceiling
can never silently overrun.

The same check now covers every other statically-sized scheduler buffer a fork
child can push to, through `emit_len_full_check` — the variant for a buffer whose
index *is* its live count: the `#0` batch (`emit_inactive_push`), the reactive `#0`
batch (`emit_re_inactive_push`) and the one-shot wait bucket
(`emit_dynamic_register`). All three were unguarded bump stores, and §V2.0 records
what that cost. The per-`expect` waiter array is deliberately left alone: an
`expect` inside a task is refused outright (`lower_expect_wait`), so a fork child
cannot reach it and one process parks at most one waiter.

A design with no fork child does **not** emit the check. Only the fork term of
`cap` is an over-approximation; without it the bound is the
one-live-slot-per-process invariant the scheduler has always held, so the check
could not fire — and it is not free. Measured on one host (Release, hyperfine min
of 5) when it was emitted unconditionally: combchain sim +42%, wheel sim +13%,
picorv32 sim +8.5% and LLVM codegen +12.5%, gate_rca codegen +29%. A compare, a
branch and a cold block on every push, in the wake path every process takes.

## V2 scope: `join_any` / `join_none`

The user-visible mechanic is small and lives in two functions. What it costs is
entirely a question of which statically-sized structure it makes unbounded, and
of one lifetime invariant that `join` held for free.

### V2.0 Prerequisites — two V1 corruptions (**landed**)

Both were reproducible with plain `fork … join`; neither was a `join_any` /
`join_none` problem, and both are on the path `join_none` takes.

**(a) Dynamic wait bucket was sized from `nproc`, ignoring fork width.**
`wait_counts` added, for each `(signal, edge)` that *any* task waits on, exactly
`chains = mod_.processes().size()` dynamic slots, resting on the same "without
fork, each process is a single call-stack" invariant the ring bound had already
abandoned. A fork child that waits is an extra chain it never counted, and
`emit_dynamic_register` had no bounds check. §A of this doc claimed a "Σ-style
bound parallel to the ring" was landed here; it was not.

```systemverilog
module top;                       // 3 processes -> [3 x {ptr,ptr}] bucket
  bit clk = 0; int hits = 0;
  always #5 clk = ~clk;
  initial begin
    fork                          // N children, all on (clk, posedge)
      begin @(posedge clk); hits++; end   // ... x N
    join
    $display("hits=%0d", hits);
  end
endmodule
```

`N <= 3` printed `hits=N`; `N = 4` SIGSEGVed, resuming a garbage handle
(`0x7bca00000004`) out of the slot past `@clotho.wait.clk.dyn.pos`.

Fixed by giving the bucket the activation bound the ring already had. The `mult()`
fixpoint is now `body_activations()`, shared by `fork_ring_slots` and
`wait_counts`, and it also reports which bodies are **reachable from a fork child**
— exactly the frames the chain term does not cover. `wait_counts` adds
`Σ mult(f)` over those bodies, per `(signal, edge)`, *on top of* `chains`: additive
rather than a replacement, so a design without a fork keeps its previous capacities
and cannot regress. The bucket for the case above goes from `[3 x …]` to
`[3 + N x …]`. Compile-time only — no run-time cost — plus the guard below.

**(b) The `#0` batch was sized from the saturating ring cap and unguarded.**
`fork_ring_slots` saturates at `kMaxForkRingSlots = 1024`, so a 3000-wide fork gets
`cap = bit_ceil(nproc + 1024) = 2048`. The active ring's guard fired correctly, but
a child doing `#0` goes to `emit_inactive_push`, which was an unmasked, unguarded
bump store — so the run emitted 3001 perfectly good `rt_ring_overflow` guard sites
and still died silently.

Fixed with `emit_len_full_check` on the two `#0` batches and the wait bucket, and
`rt_ring_overflow` now names the buffer that overflowed rather than assuming it is
one of the two rings. That case now reports
`FATAL: the inactive (#0) batch overflowed at 0 s (capacity 2048 entries)` and
exits 1.

**Cost.** Both guards sit behind the existing `design_forks()` gate, so a design
with no fork emits neither — verified byte-identical: the emitted LLVM IR for
picorv32 (52 133 lines) is unchanged. A forking design pays one compare and branch
per `#0` and per dynamic wait registration, on top of the ring check it already
paid.

### V2.1 The core mechanic (**landed**)

`ForkInfo::kind` already existed and was already written by `Builder::fork`, but
nothing read it — `lower_fork_inst` hardcoded join-all. `lower_fork` now maps
`blockKind` onto it and the backend branches on it in two places:
`lower_fork_inst` and the fork-child epilogue.

At run time the three kinds differ in exactly one record field. `wake_at` is the
`count` value at which the parent is due to be resumed — 0 for `join` (the last
child), `W-1` for `join_any` (the first), never reached for `join_none` (which
stores no parent at all). Since `count` only ever decreases, `count == wake_at` is
true at most once, so no claim flag is needed to stop a second child re-pushing a
parent that has moved on.

They also differ in **when a child starts**, which is the part an implementation
can get wrong invisibly:

- **`join`** keeps the original spawn loop: ramp each child and `coro.resume` it
  inline. Legal, because the parent is about to block on all of them anyway, and it
  keeps `join` children off the rings entirely.
- **`join_none`** ramps each child and pushes it onto the ring for the spawning
  body's own region (`emit_region_push`) without resuming it. LRM 9.3.2 has the
  parent continue while children are merely scheduled, so running one at the fork
  would reverse observable order — iverilog agrees, printing `parent` first.
- **`join_any`** runs children only *until one has completed*, then queues the rest
  (`count == W` is "nobody has finished yet"). Every child is still created. Running
  them all and then testing the count would make `join_any` behave exactly like
  `join` for children that never block: `fork n=n+1; n=n+1; join_any` must leave `n`
  at 1, not 2.

### V2.2 Lifetime: whose frame owns the join record (**landed**)

`join`'s "the parent outlives every child" was load-bearing in three places, and
the other two kinds invalidate all three.

1. The record was **one alloca per coroutine, reused across fork sites**, sound only
   because a `join` completes before the next fork runs. A `join_any` /`join_none`
   parent reaches a second site with the first site's children still live, and the
   second site's init would stomp the first's live count.
2. The record lived **in the parent's frame**. A `join_none` parent can return and
   have its frame destroyed while children still decrement.
3. Captures are **live pointers into the parent's frame** (the ref ABI).

So ownership now depends on the kind, and `Function::fork_kind` carries it to the
child's epilogue:

- **`join`** — unchanged. A frame alloca, reused across the coroutine's join sites,
  no refcount, the short `{ i32 count, ptr parent }` record, and the epilogue tests
  against a constant 0. Emitted LLVM IR for a `fork … join` design is
  **byte-identical** to before V2.
- **`join_any` / `join_none`** — one `rt_alloc` record per fork *site*, typed
  `{ i32 count, ptr parent, i32 wake_at, i32 refs }` and reference-counted by the
  parent plus one per child. Whoever drops the last reference frees it, which
  handles both "the parent returned first" and "every child finished
  synchronously inside the spawn loop, but the parent still has to read `count`".
- **Captures** were refused by name for the detaching kinds, since nothing kept the
  frame alive. Lifted in V4, and not by keeping the ref ABI for `join`: a capture is
  a GC box handle for EVERY kind, so this bullet's lifetime argument no longer has a
  case to make. A fork in a *process* body still has no captures at all (parent
  variables are module signals), which is why the change is free for the common
  testbench idiom.

### V2.3 Unbounded live children (**landed**)

A loop around a `join_any` / `join_none` accumulates children with no static limit,
and they occupy more than the ring: the `#0` batches and the dynamic wait buckets
too (a spawned child doing `@(posedge clk)` is the common case). Rather than make
three different structures growable — the wait bucket is *scanned*, so a spill list
there means reworking the service scan — the activation bound itself reports the
unboundedness, and every buffer sizes from that one answer.

`body_activations` gains a third saturating shape alongside recursion and virtual
calls: a spawn edge from a non-`Join` fork contributes `kMaxForkRingSlots` rather
than its parent's multiplicity, so those children (and everything they call) saturate
the ceiling. The rings, the batches and the wait buckets then all reserve for it,
and the V2.0 guards turn anything past the ceiling into a located FATAL. 1024
concurrent detached children are supported; the 1025th reports rather than corrupts.

This costs memory in a design that uses the detaching kinds (one pointer per ring
slot, one `{ptr,ptr}` per wait slot) and **nothing anywhere else** — the term only
appears when a non-`Join` fork does.

A genuinely unbounded version is still possible and is worth doing if a real design
hits the ceiling: keep the static array as the fast path and turn the *already
emitted* cold block of `emit_ring_full_check` into a spill onto a runtime-owned
growable buffer (`$strobe`'s Postponed queue, `Runtime.cpp:2479-2497`, is the working
precedent), refilling from it at the drain's empty edge the way the inactive→active
promote already copies. The push fast path — static array, constant base, constant
mask — would not change at all, so no existing construct would pay. The wait bucket
is the hard half of that work, not the ring.

### V2.4 Smaller edges (**landed**)

- `suspends("a fork")` classified any non-sequential block as suspending, which
  over-refused `join_none` in a strong assertion's action block. `join_none` does not
  park the parent, so it is exempt; its children DO go through a ring, but each
  carries its own home region, which is what the parked-handle tag is for.
- Every refusal in the fork path now names the kind it saw (`fork..join_none child
  that reads …`) instead of always saying `fork..join`.
- The **fork-in-a-function** guard is now reachable for the first time: slang rejects
  `join` / `join_any` in a function via `diag::TimingInFuncNotAllowed` but lets
  `join_none` through, so `lower_fork`'s own `is_task` check is the only thing
  catching it. Kept, and refused by name — a known gap rather than a mis-lowering.
- A `join_any` / `join_none` parent can end with children still live. A child parked
  on the wheel keeps the wheel non-empty and still runs (verified: a parent that ends
  at time 0 still gets its `#5` child). A child parked on a wait queue nothing will
  ever service leaves a suspended frame at end of simulation — same as a task's frame
  after `$finish`. Accepted.
- `Inst::Tag::Fork` stays in `blocks_thunk` for `join_none` too: it spawns coroutines
  and mutates scheduler state, so the process still cannot be a thunk and must not
  fuse.
- **Random stability** (LRM 18.14.1): a child owns its xoshiro256++ state rather
  than sharing the parent's pointer, seeded in its ramp from one draw of the
  parent's stream. Sharing was not an option even ignoring 18.14.1 — the state is
  an alloca in the parent's coroutine frame, and under `join_any` / `join_none` a
  child can outlive it. The state is materialized only for a child that can reach
  a draw, so an ordinary fork costs nothing and does not perturb the parent's
  stream (`build_rng_fns`).

**Closed since V2.** Both holes were call-edge discovery re-derived per consumer, so
both are fixed at the source: `ir::inst_callees` / `ir::collect_callees`
(`src/ir/Analysis.h`) are now the single authority on where a call edge lives in the
IR, and `Structural::walk`, the backend's reactive/program reachability walks and
`scan_finish` all read them. `BodyFacts::calls` therefore carries `Call`,
`CallVirtual`, `TaskCall`, `TaskCallVirtual` and every `ForkInfo::children` entry, and
a `$finish` in a child now marks the forking body may-finish.

Two bounds survive and are documented at the declaration. Virtual dispatch
contributes its STATICALLY resolved target only (`VirtualCallInfo::func`), so an
override reached through a handle is missed and the may-finish figure stays a lower
bound. And for `join_any` / `join_none` a child outlives the fork, so a write of its
may land in a later instant than the edge claims — the edge is conservative rather
than exact, which is the safe direction for a wake graph that is
over-approximation-tolerant by construction.

### V2.5 What V2 does not do

- **`wait fork` / `disable fork`** (V3) — both still refused by name. They need a
  per-process live-child *set*, which the count-and-wake_at record does not carry;
  `disable fork` additionally needs the general abort primitive
  (`docs/design/disable.md`).
- **Per-spawn automatic lifetime** (V4) — LANDED, and as a LIFETIME fix rather than
  per-spawn copies: `for (int i…) fork … use i … join_none` captures and reads the
  shared final `i`, which is the strict reading of LRM 9.3.2 (see the V4 bullet's
  three-way measurement). The clause's explicit opt-in to a per-iteration copy —
  `fork automatic int k = i; … join_none`, a declaration of the FORK's own scope —
  landed separately: §V4b.
- **Truly unbounded** live children — the ceiling in V2.3 is 1024, reported loudly.
- **A `fork` inside a function** — LANDED for `join_none` (§"V5 scope" below), which
  is the only kind LRM 13.4.4 allows there. A child capturing the function's own
  formals landed with V4.

## V5 scope: `fork … join_none` inside a function

Status: **LANDED**, without captures. `join` / `join_any` in a function are illegal
SystemVerilog and stay that way: LRM 13.4.4's "Functions shall execute with no
delay" is what slang enforces in `BlockStatement::fromSyntax`
(`diag::TimingInFuncNotAllowed` for `JoinAll` / `JoinAny`, slang
`source/ast/Statement.cpp:694-698`). So V5 is `join_none` only, and `lower_fork`'s
guard is kind-specific rather than gone — a non-`JoinNone` fork on a non-task
`fn_ctx_` is still a defensive fail. A child that reads the function's own formals
or locals is still refused by name and lifts with V4 (§V5.5).

### V5.0 What the LRM asks for (13.4.4)

> Statements that do not block shall be allowed inside a function; specifically,
> nonblocking assignments, event triggers, clocking drives, and fork-join_none
> constructs shall be allowed inside a function. Calling a function that tries to
> schedule an event that cannot become active until after that function returns
> shall be allowed provided that the thread calling the function is created by an
> initial procedure, always procedure, or fork block from one of those procedures
> and in a context in which a side effect is allowed. Implementations shall issue
> an error either at compile time or run time when these provisions have not been
> met. […] Within a function, a fork-join_none construct may contain any statements
> that are legal within a task.

Three consequences, in increasing order of work:

1. The child body is unrestricted — `#delay`, `@`, `wait`, a task enable. slang
   implements exactly that by clearing `ASTFlags::Function | ASTFlags::Final`
   inside a `join_none` block (`Statement.cpp:699-703`, "all statements are allowed
   in those"), so the children arrive fully formed and the body needs no frontend
   work at all.
2. Table 9-1's "the spawned processes do not start executing until the parent
   thread executes a blocking statement or terminates" is about the THREAD, not the
   function activation. The function returns at once; the children must run when the
   CALLING PROCESS next blocks or ends — which is what pushing them onto the
   caller's own region ring already does.
3. The CALLER becomes part of the legality question and the LRM demands a
   diagnostic. Its own example is the illegal case — `bit y = watch_for_zero(stack);`
   at module scope, i.e. a static-variable initializer, with the same verdict for a
   continuous assignment. That is V5's real work (§V5.2).

### V5.1 The spawn needed almost nothing a function lacks

The refusal in `lower_fork` was not ABI-driven, and its own comment said so
("`is_task` is the real discriminator"). The `join_none` path of `lower_fork_inst`
reads four values out of the enclosing body, and a plain function has all four:
`state_` (arg 0), `rng_slot_` (arg 1), `wheel_` — a function takes no wheel
parameter but binds `wheel_` to the `clotho.wheel` global, which `run()` creates
before any body for exactly this reason — and, in a design with process control,
`proc_arg()`, which `lower_function` takes as its own `proc` parameter
(`declare_function` pushes it, `lower_function` binds it). Everything else is a
global, a compile-time constant, or the heap record `rt_alloc`ed per fork site. The
target region is the compile-time `reactive_home()` bit `lower_function` already
computes, folded inside `emit_region_push`; there is no runtime "current region"
value in Clotho to thread. And `join_none` provably carries no captures:
`lower_fork`'s `kind != Join` guard runs before the capture-type guard, so
`ForkInfo::args` is empty on every surviving `JoinNone` path and the child ramp argv
degenerates to `{ state, rng, wheel, [proc], rec }`.

Every coroutine-dependent line of the join path is on the far side of the `continue`
the `none` case takes: `fork_record_slot()` (a `coro_begin_`-anchored alloca),
`parked_handle(b_, hdl_)` and `emit_suspend`. A `join_none` parent never enters the
record — `parent` stays null, so a child epilogue's `wake` is always false — and the
record's refcount (`W+1`, released once by the parent and once per child) is what
already licensed a parent that walks away from live children. A function activation
that returns is that same lifetime, not a new one.

**The one line that did need changing came from V3.** With process control in the
design the heap record is also published into `fork_live_slot()`, a
`coro_begin_`-anchored frame alloca, so that an aborted parent parked at the join can
detach itself (`emit_fork_record_abandon`). That publish was on the `join_none` path
and is the only thing that would have null-dereferenced `coro_begin_` in a function.
It is also dead weight there: between the `rt_alloc` and the parent's release there
is no park, hence no abort poll, hence no window for an abort to land in, and the
record's `parent` is never non-null. Both the publish and its matching null-store are
now gated on `!none`, which unblocks the function case and removes two stores from
every `join_none` in a process-control design. `fork … join` still publishes — its
emitted IR is unchanged.

Two consequences of V3 that needed no work, and are the reason V5 is worth having:

- The child's PCB comes from the pool with the SPAWNING body's block as parent
  (`lower_task`'s `f.is_fork_child ? rt_proc_alloc(caller) : caller`). In a function
  that block is the CALLING THREAD's, so a child spawned inside a function is a child
  of the calling process — exactly LRM 9.6.3's descendant relation. `wait fork` and
  `disable fork` in the caller therefore reach it with no extra plumbing, which is
  covered by a test, and `process::self()` inside the function is the caller, as LRM
  13.4 requires.
- A function activation is not on its thread's activation stack
  (`emit_pcb_acts_link` needs `hdl_`). Harmless: a function cannot park, so an abort
  aimed at the thread is noticed at the caller's next poll.

`Ir.h`'s invariant is narrowed accordingly: `Inst::Tag::Fork` is coroutine-only for
`ForkKind::Join` / `JoinAny`, and legal in a plain function body for `JoinNone`.

### V5.2 The real work: which callers may spawn

13.4.4 makes the legal set "a thread created by an initial procedure, an always
procedure, or a fork of one, in a context where a side effect is allowed", and
requires an error otherwise. Clotho's entry contexts, and the verdict for each:

**Legal.** `Activation::Initial`; a user `always` / `always_ff` / `always_comb` /
`always_latch`; any task or fork child reached from those; `Activation::Reactive` (a
`program` block's `initial`, LRM 24.3); and a concurrent assertion's action block,
which already supported a direct `join_none` (§V2.4) and reaches the reactive ring
through the same `emit_region_push`.

**Refused, each by name.** `Activation::PreInit` — static-variable initializers, the
LRM's own illegal example. Continuous assignments, gates and port copies — not a
procedure. `Preponed` / `Observed` / `ReNba` — compiler-synthesized sampling,
property evaluation and clocking drives; none is a procedure and all are resume-once.
`Activation::Final` — a `final` block is not an initial/always procedure, slang
treats it like a function for timing (`ASTFlags::Final`), and the drain has already
ended, so a child pushed onto the active ring could never run. A `$strobe` /
`$monitor` deferred body — its arguments are evaluated in Postponed, where LRM
4.4.2.9 forbids a value change. A DPI export — native C drove the call, so there is
no thread at all, which the backend already says in so many words at `ProcOp::Self`
("No calling thread: a DPI export thunk or the $monitor ramp"); a `join_none` there
would hand `rt_proc_alloc` a null parent.

**All of those were silently accepted before this landed**, which is why the check is
the deliverable and not a formality. Measured while scoping, with the frontend guard
relaxed and nothing else changed: `int y = spawn();` ran its child at time 1;
`assign z = spawn(a);` spawned one child per re-evaluation; and
`final begin void'(spawn()); end` printed `n=0` with the fork record and the child
frame leaked (LeakSanitizer, 96 + 24 bytes).

**Mechanism: `check_spawn_roots` in the frontend** (`src/Lower.cpp`, beside
`check_vcd_scope`), run once after every top has lowered. `build_spawn_graph` marks
every body that holds a `Fork` and relaxes that over `Call` / `TaskCall` /
`CallVirtual` / `TaskCallVirtual` edges to a fixpoint — tasks included, because a
`final` block may enable a task and a `join_none` inside one has the same problem.
Fork children are deliberately not followed: the forking body is already marked, and
a child's legality follows its parent's. A design with no fork pays one scan of the
function table and stops. Roots are then every `Process` the LRM does not allow to
spawn, plus the two non-process entries: `StrobePend` / `MonitorSet` target bodies
and `Module::dpi_exports`. `ReactivePend` is deliberately NOT a root — an action
block is reactive code of the assertion's own thread and may spawn.

**One bit of provenance was missing.** `Activation::Always` covers a user `always`
AND a continuous assignment — `add_continuous_process` is what `assign`,
net-declaration assignments, gates and port copies all go through — so the activation
alone cannot separate the legal case from the illegal one. `Process::spawn_root` now
carries it, set only where `lower_procedural_block` builds a process and false for
`Activation::Final` and for every synthesized process. One byte per process, at
compile time.

The diagnostic is the frontend's because that is the only place a located error is
possible: `Process::span`, `Function::span` and the `Fork` instruction's own
`SourceLoc` are all available, while `BackendError` carries a message and no
location. It points at the fork site and attaches the calling context as a note:

```
t.sv:5:7: error: a function that spawns a background process (fork..join_none, LRM
13.4.4) is called from a `final` block, which is not a thread of an initial or
always procedure: the spawned process could not become active [illegal-construct]
  t.sv:9:3: note: called from here [illegal-construct]
```

`DiagnosticOrigin::Rejected` with code `illegal-construct`, not
`unsupported-construct`: the source is illegal SystemVerilog that slang does not
catch, rather than something Clotho has not implemented.

### V5.3 What the analyses needed — one closure

Almost everything already covers a `Fork` that lives in a function body, because it
either walks the whole function table or keys on `Function::is_fork_child` rather
than on where the `Fork` sits. Verified against code, so it is not re-audited:
`body_activations` (walks every function's IR, and takes the saturating
`kUnboundedSource` edge from `ForkInfo::kind` alone, never from the containing body),
`fork_ring_slots` / `ensure_active_queue`, `wait_counts`'s `from_fork` term (its
relaxation crosses `Call` edges), `nba_classify` (scans every function body, for the
NBA-in-a-function case 13.4.4 also allows), `design_forks` / `ring_guard_wanted`
(per-`Function` flag), `build_rng_fns` (`Call` and `Fork` edges both),
`collect_callees` — which is what gives a child of a reactive body its own reactive
classification — and `legalize/Legalize.cpp`, which refuses `is_fork_child` outright
and maps `Fork` to `Finding::Fork` in its default arm.

The one hole was `reject_resume_once`: it scanned `p.ir` only, so an Observed /
Re-NBA process reaching a fork through a call would have been resumed once and
silently truncated. It now closes over calls (`reaches_fork`, a memoized walk over
`collect_callees`), and its `Inst::Tag::Fork` case is gone as unreachable. Those
designs are refused by §V5.2's root rule with a source location; this is the backend's
independent backstop, kept in agreement deliberately, exactly as
`reject_unsupported_reactive` documents.

**Thunk elision needed nothing, contrary to the obvious worry.** `is_simple_always`
is syntactic on the process's own IR, `Fork` is `blocks_thunk` but `Call` is not, so
`always_comb y = bump(a);` where `bump` forks IS thunk-lowered — confirmed in the
emitted IR (`clotho.inst.top.p0.thunk`) — and it works: children run at the right
times with the right values, one per evaluation. A thunk has `state` and `wheel`
parameters and the rings are globals, so the push is mechanically fine, and a
thunk-lowered process ending is exactly the "parent thread terminates" of Table 9-1.
The one thing a thunk cannot provide is per-process RNG state, and that is already
handled: `rng_procs_` follows `Call` and `Fork` edges, so a process whose forked child
can draw is already off the thunk path. Since `always_comb` calling a spawning
function is LEGAL per 13.4.4, that is the right answer — no exclusion, no refusal.

### V5.4 Region: already handled, including the shared-body hazard

Region is a compile-time property of the lowered body (`reactive_home()`) plus a tag
on parked handles, and a function already gets the compile-time answer from
`build_reactive_fns` / `build_program_fns`. The interesting case — a function reached
both as reactive code and from ordinary code, which is lowered once and can record
only one home region — is already refused: `reject_unsupported_reactive` runs over
every reactive-reachable FUNCTION and its `Fork` arm calls
`reject_shared_reactive_suspend`. A function is likelier than a task to be shared
that way, so that guard stops being theoretical with V5; it needs no change.

### V5.5 What V5 alone does not buy: the LRM's own example

```systemverilog
function automatic bit watch_for_zero( IntClass p );
  fork forever @p.a begin if ( p.a == 0 ) $display("Unexpected zero"); end join_none
  return ( p.a == 0 );
endfunction
```

The child reads `p`, an automatic FORMAL, which V5 alone could not carry: the capture
guards fired with
`fork..join_none child that reads the enclosing function's 'p' is not supported`.
V4 lifted exactly that — `p` is boxed and the child holds the box — so the clause's own
example now works. The paragraph below is kept because its reasoning is what chose the
box over a copy. (The message used to say "the enclosing task's" whatever the body was;
it now names the kind, because in a function the captures that matter are its own
formals.) A function's interesting state IS its formals, which is why V5 on its own
admitted only children touching module signals, static locals and static class
properties: a monitor spawned by a helper function, not the LRM's idiom.

That was an argument for finishing V4, not for widening the ref ABI. This paragraph
used to say the right model for a detached child is "copy-in by value at spawn"; see
the corrected V4 bullet in the phased plan for why that is wrong in general — LRM
9.3.2 makes an enclosing automatic ONE variable, and iverilog agrees where Verilator
does not. What the child needs is the same variable with a longer life: a GC-owned
box, whose POINTER is copied in by value. That is what removes the lifetime problem
`ArgDir::Ref` has, and it is the only model available to a function, whose frame is
gone the moment the call returns. `watch_for_zero` reads `p` and never writes it, so
it cannot tell the two apart — but a child that WRITES a captured local can, which is
why the box and not the copy is the answer.

Adjacent friction cleared since: `$time` / `$stime` / `$realtime` inside a function
is supported. The refusal's stated reason ("no wheel pointer in its ABI") was
always stale — `lower_function` binds `wheel_` to the `clotho.wheel` global, so a
function reads the clock exactly as a task does — and the timescale objection
(LRM 20.3.1 scales `$time` to the invoking module's unit, while one shared body
has a single `time_mul_`) is settled by LRM 3.14.2.3: a read inside a subroutine
is in the units of the scope that DECLARES it, which is what `function_id` already
applies to subroutine delays and to `$time` in a task. Cross-checked against
iverilog, which scales a package function's `$time` by the package. So `%0t` may
be printed in the spawning function as well as in the child. `$strobe` /
`$monitor` in a function keep the second, still-real reason (no `coro_begin_` to
hang the ramp's record slot off).

### V5.6 Cost

Nothing at run time for a design that does not fork inside a function: the spawn
graph, the root check and `Process::spawn_root` are compile-time, and the `!none`
gate on the `fork_live_slot` publish removes two stores rather than adding any. No
new capacity term either — a non-`Join` spawn edge already saturates the activation
bound (§V2.3), so the rings, `#0` batches and wait buckets size the same whether the
fork sits in a process, a task or a function. Emitted IR for a `fork … join` design
is unchanged, including its `fork_live` publish under process control.

### V5.7 Verification

**iverilog is not an oracle for this construct.** Two independent deviations,
measured on iverilog 13 while scoping this:

- it rejects a `#5` inside a `join_none` child in a function outright
  (`error: functions cannot have delay statements`), which 13.4.4's "any statements
  that are legal within a task" allows and slang accepts;
- for delay-free children it prints both children BEFORE the statement following the
  fork, i.e. it runs them at the fork, contradicting Table 9-1 — so its function path
  disagrees with its own process path, which prints the parent first.

It also still asserts (`vthread.cc:3793`, `of_JOIN_DETACH`) on a detaching fork in an
automatic task, which is what already pins three of the 22 V2 cases.

**Verilator 5.050** (`--binary --timing`) matches the LRM on both shapes.
Corroboration only; the LRM text is the pin, per the project's rule on Verilator.

In `tests/fork_test.cpp`: parent-first ordering with a `#5` child in a function
(`fn done n=0 / after call n=0@0 / mid n=1@1 / end n=11@11`); a spawning function
called from a task; one called from `always_comb`, which spawns per evaluation and
stays thunked; eight calls in a loop giving eight live children on one delay; `wait
fork` and `disable fork` in the CALLER reaching a child spawned inside a function;
the capture refusal naming the function; and all five illegal roots reporting
(static initializer, continuous assignment, `final`, `$strobe` body, DPI export).

Not covered: a spawning function reached from an Observed / Re-NBA process, which
needs an SVA build (`-DCLOTHO_SVA=ON`) — the frontend refuses it by the root rule
and `reject_resume_once`'s new closure is the backstop, both by construction rather
than by test.

### V5.8 Adjacent gaps this widens but does not own (**closed**)

`BodyFacts::calls` was fed only by `TaskCall` / `TaskCallVirtual` — the `Call` arms
pushed nothing and there was no `Fork` arm — so `compute_subscription_order`'s
closure missed every signal a fork child, or any plain function, blocking-writes, and
`wake_adj_` / `scc_nontrivial_` / `sub_order_` were computed on an incomplete graph
that `compute_fusion_groups` then read. Likewise `scan_finish` omitted `Fork`. Both
now route through `ir::collect_callees`; see the "Closed since V2" note under §V2.4.

**This was a latent-correctness fix, not a live wrong-value bug, and the reason is
worth writing down because it is recorded nowhere else.** No settled-value divergence
is reproducible from either hole. For the missing edge to produce a wrong VALUE it
would have to defeat the held-sched-flag argument, which requires a combinational
cycle *inside* one fused group. `compute_fusion_groups` blocks that independently,
by two facts that compose:

- Candidacy (`LlvmBackend.cpp:8989-9000`): a body holding `Call` / `CallVirtual` /
  `TaskCall` / `TaskCallVirtual` / `Fork` is never `CombFuse::Eligible` (it scores
  `HasCall` or `ImpureOp`), so the SOURCE of a hidden edge is never a group member.
- The interior guard (`LlvmBackend.cpp:9001-9038`): a candidate merges only when its
  ENTIRE sensitivity set is written by another candidate. Group members are therefore
  triggered only by candidates, whose writes are all direct `Store`s and hence all
  visible in the graph.

Together those force the in-group candidate chain to be acyclic and fully visible: a
hidden edge can only ever close a cycle OUTSIDE a group, where the sched flag is not
held and ordinary re-wake-on-change still reaches the fixpoint. Confirmed
empirically — a fork child in a loop, a value function in a loop, a task in a loop
(control) and a fork child feeding a comb chain all produce byte-identical output
under `{fuse=true}` and `{fuse=false}`, both before and after the fix. What the fix
DOES change is the fusion decision: on a shape crafted to keep the members interior
while the back edge hides in a function, the loop stops fusing, which is what
`tests/comb_fusion_test.cpp` now pins. The `scan_finish` hole is latent for the
separate reason the §V2.4 note gives — `$finish` is still a longjmp.

**Residual bound, and the sentence a future reader needs: the interior guard is what
makes this latent.** Anything that relaxes it — admitting a member whose sensitivity
is written by a non-candidate, or admitting a callable body as a fusion candidate
once purity closure over callees lands (the `CombFuse::HasCall` verdict exists
precisely as a placeholder for that) — makes the hidden edge live, and a wrong
settled value becomes reachable. Do not relax it without the complete call graph this
change installs.

## V4b scope: spawn-time initialization of a fork's own declarations (LRM 9.3.2)

Status: **LANDED.** This is the other half of V4, whose refusal the V4 material below
recorded. V4 gave a captured automatic the right lifetime and the right sharing; what it
did not do was materialize the declarations the fork itself introduces.

### V4b.0 Two positions, not one

The clause's sentence is scoped precisely, and the scope is the whole design:

> Variables declared in the block_item_declaration of a fork-join block shall be
> initialized to their initialization value expression whenever execution enters their
> scope and before any processes are spawned.

Its own example uses BOTH positions and states both answers:

```systemverilog
initial
  for (int j = 1; j <= 3; ++j)
    fork
      automatic int k = j;   // local copy, k, for each value of j
      #k $write("%0d", k);
      begin
        automatic int m = j; // the value of m is 4
        ...
      end
    join_none
```

- **(a) fork-scope** — `k` is a block_item_declaration of the FORK. Initialized in the
  parent, once per execution of the fork statement, before any child is spawned, so each
  iteration's children see their own copy: `1 2 3`.
- **(b) child-body-local** — `m` is a block_item_declaration of the nested
  `begin … end`, which is a DIFFERENT block. Its scope is entered when the child body
  runs, after the loop has finished, so it reads the shared final `j`: 4. The clause
  states that answer in a comment of its own.

The old `SpawnInitScan` refusal fired for both, and its message — "LRM 9.3.2 initializes
it at the spawn, and the body runs later, so it would read a later value" — described
only (a). For (b) it was refusing the answer the clause asks for: clotho's ordinary late
lowering of a declaration statement is already right there, and needed nothing except
for the guard to stop firing.

`fork : guard` is the same construct. A label changes neither `blockKind` nor the body
shape, so keying the split on `is_fork_block` keeps the labelled form — what
`uvm_objection.svh:818` uses — on one path with the bare one.

### V4b.1 The mechanism: per ENTRY, not per activation

Lowering a fork's declarations in the parent is the whole user-visible change, and once
the variable is parent-side storage a child referencing it is an ORDINARY CAPTURE. The
existing V4 machinery carried that unchanged: `build_fork_child` already turns a capture
into an `In` formal holding a box handle and registers it in the child's own `boxed` map,
so there is no new child-side mechanism, no new IR node and no backend change. What the
declarations needed was a home for the handle and a different allocation POINT.

The storage has to be fresh per **entry to the fork's scope**. That is the one property
a per-activation box does not have, and getting it wrong looks like a working feature:

| storage for the fork's declaration | the clause's example prints |
|---|---|
| per entry (correct) | `123` |
| per activation — one box, rewritten each iteration | `333` |
| treated as a child-body-local | `444` |

What buys the right one is that **lowering happens once while the fork EXECUTES many
times.** So the box's home is allocated once, at lower time, and the `ClassNew` that
fills it is emitted at the fork statement:

- in a subroutine the home is a frame Local and the entry goes in `FnCtx::boxed` — the
  same representation `box_one_capture` uses, which is why `value_lvalue`,
  `ForkCaptureDetector` and a nested fork's capture of the same variable all needed
  nothing;
- in a PROCESS body there is no frame, so the home is a design-state signal
  (`proc_fork_boxes_`). An `initial` block's storage is design state, and design state is
  a PRECISE GC root (garbage-collection.md §5.4), so the handle is rooted there for the
  same reason a frame slot roots one conservatively.

The handle is stored into its home immediately, before any initializer can allocate, so
the box is reachable from a root for its whole life rather than sitting in an SSA value
the collector does not scan.

`install_capture_boxes` correspondingly stops boxing a fork-scope declaration per
activation (`BoxCandidateScan::fork_scope_decls`); with both homes live the two would
race, and `value_lvalue`'s precedence would silently pick the per-activation one.

**Cost per fork EXECUTION:** one `ClassNew` — a bump allocation of a one-field object —
plus one store of the handle, per declared variable, plus the initializer's own
evaluation and store. Nothing per access beyond the load-and-deref a capture already
paid, and nothing whatever for a fork that declares nothing: `decls` is empty and no
instruction is emitted, so a design without the construct is unaffected.

### V4b.2 The ref-formal carve-out

The clause's second sentence makes a by-reference formal legal in exactly one place:

> Within a fork-join_any or fork-join_none block, it shall be illegal to refer to formal
> arguments passed by reference other than in the initialization value expressions of
> variables declared in a block_item_declaration of the fork, unless the argument is
> declared `ref static`.

clotho refused capturing an `output` / `inout` formal outright ("capturing an output or
inout formal would lose the writes made through it"), which refused this legal position
along with the illegal ones. It is implementable for the same reason it is legal: the
initializer is evaluated in the PARENT at the fork, so there is no child-side access to
the formal and no copy-out to lose.

It needed no new check, only a narrower scan. `BoxCandidateScan` no longer collects the
references inside a fork-scope declaration's initializer, so such a formal never becomes
a box candidate and the refusal never triggers. The refusal for a child BODY reading one
is untouched, and is now the precise complement of the carve-out.

### V4b.3 What the `join` claim turned out to be

A `kind != Join` gate used to exempt `join` from the refusal, on the recorded grounds
that a `join` child is ramped and `coro.resume`d inline at the fork, so its declaration
initializers already run at the spawn. Measured rather than trusted, that claim does not
hold, and the gate is gone:

- what made `join` look correct was the per-activation capture box, which exists only in
  a subroutine body. The declaration statement was lowered as child #0, ran first
  because declarations precede statements, and wrote through a box its siblings could
  read;
- in a PROCESS body there is no such box, and `fork automatic int k = 5; … join` failed
  outright with `reference to unknown signal 'k'` — the declaration's variable lived in
  child #0's own frame, where no sibling could see it;
- under `join_any` the same shape is worse than a diagnostic. The declaration child
  completes immediately, which SATISFIES the join before any real child has run.

All three follow from the same root cause: a flat walk of the fork body made every
top-level statement a child, so a declaration became a process.

### V4b.4 What is refused

- A **static-lifetime** declaration in a fork's block_item_declaration. LRM 6.21
  initializes static storage once before time zero; 9.3.2 asks for initialization on
  every entry to the fork's scope. The two readings disagree, nothing here implements
  either, so it fails loudly and names the conflict.
- A child **body** reading an `output` / `inout` formal — unchanged.

### V4b.5 Verification

Verilator 5.050 `--timing` is the oracle for (a) and agrees on every case measured: the
clause example's `123`, both UVM shapes including the labelled one, all six initializer
sources, the once-per-execution side effect, the sharing cases, the multiple-declaration
forward reference, and all three join kinds with their timestamps.

It is **not** an oracle for (b): it prints `1 2 3` for the nested `m`, contradicting the
clause's own `// the value of m is 4`, so it conflates the two positions that
`tests/fork_test.cpp` now pins apart.

**iverilog 13 cannot compile the construct at all** — "sorry: Overriding the default
variable lifetime is not yet supported" for any `automatic` declaration in a fork — so
the oracle that covered V1–V3 is unavailable here. Its `vvp` additionally aborts on the
bare-capture loop shape (`vthread.cc:3793`, `of_JOIN_DETACH`).

The bare-capture reading is unchanged, and is now pinned side by side with the other two
positions in one test so the distinction cannot decay: `444|123|444`. Worth recording
precisely, since the V4 table below measured only the process-body shape: Verilator's
`1 2 3` deviation on a bare capture is process-body-specific — in a TASK body it prints
`444` and agrees with clotho.

## Hard parts (V1)

Beyond the ring bound, ranked by risk:

### A. Fork-child bodies must plumb into every static backend analysis

"Children aren't in `mod_.processes()`" is pervasive. Each analysis that
iterates the static process/task set needs the child bodies:

- **`nba_classify`** — an NBA (`q <= x`) in a child stages into the NBA
  region; miss the child body and the write silently never commits.
- **`build_wait_queues` sizing** — a child's `@(edge)` wait registers on a
  dynamic queue at runtime; bucket capacities are counted statically and must
  reserve for child subscriptions (a Σ-style bound parallel to the ring). Landed
  late, as a bug fix rather than with V1: `wait_counts` bounded task waiters by the
  process count until §V2.0(a).
- **`is_simple_always` (thunk elision)** — a process containing a fork
  suspends at the join, so it must be excluded from the thunk path (like
  `Delay`/`Wait`/`TaskCall`).
- **fusion** — forking processes must not fuse.

### B. Per-child coroutine extraction

Each child statement lowers to its own `void` coroutine with a per-call
`rt_alloc` frame — a task with no formals beyond its captures (reuse the
`lower_task` skeleton with a modified epilogue). The child body is lowered **in
the parent's scope context** (sigmap / prefix / time-mul) so shared static locals
and hierarchical names resolve to the parent's signals.

Inside a task there is no such fallback: the enclosing formals and automatics
live only in that frame. `lower_fork` therefore collects the enclosing-frame
symbols the fork's children touch (`ForkCaptureDetector`) and passes each to
**every** child as an `ArgDir::Ref` formal — the landed live-pointer ABI
(`docs/design/ref-args.md`), whose lifetime rule holds here for the same reason:
a `join` parent outlives all its children. One shared list rather than one per
child, so an unused capture costs a dead pointer parameter and every child of a
fork has the same ABI. The captures ride the IR as `ForkInfo::args`, which is
what makes them ordinary SSA operands to the def-use / x-z walks.

### C. Join-record sequencing + synchronous-completion edge

Init `live_count = W` **before** spawning any child. Each child's epilogue
decrements; the one reaching 0 pushes the parent (generalizes the task's
single `parent` push). Edges: `W == 0` (empty fork → don't suspend); all children
completing within the same drain before the parent's suspend commits (reuse the
task synchronous-vs-blocked idiom — if `count == 0` after spawning, continue
inline; else store handle + suspend).

This section used to prescribe "push children to the ring (not inline-resume)".
The landed code does the opposite: the spawn loop ramps each child and
`coro.resume`s it inline, then destroys it if `coro.done`. For `join` that is a
legal order and it keeps children off the ring entirely. For `join_none` it is
observably wrong (LRM 9.3.2 — the parent continues first), so `join_none` restores
the ring push; see §V2.1.

### D. Sensitivity attribution

The parent process's read-set / sensitivity visitor must **not** absorb the
fork children's reads — children are independent processes with their own
waits. Descending into the fork body would wrongly extend the parent's
`always @*` sensitivity and skew comb/latch detection. Exclude child bodies
from the parent's sensitivity, as called-subroutine bodies are handled.

### E. Loud guards (never silently accept)

- fork in a **function** — only `join_none` can reach `lower_fork` there, and its
  non-task `fn_ctx_` check refuses a non-`JoinNone` kind by name. LRM 13.4.4 permits
  `join_none` there and it lowers (§V5). `join` / `join_any` in a function, and any
  fork in **`always_comb` / `always_latch`**, are illegal SV and slang rejects them
  itself (`diag::TimingInFuncNotAllowed`, `diag::ForkJoinAlwaysComb`).
- a captured local the ref ABI cannot carry (unpacked array / struct / string /
  real / queue / class handle) — **gone with the ref ABI (V4).** A capture is a GC
  box handle, so the repertoire is the declared type.
- a child reading a class property of the enclosing method through the implicit
  `this` — **gone with the ref ABI (V4).** `this` is a capturable automatic like any
  other; the box carries a handle.
- a capture under `join_any` / `join_none` — **gone with the ref ABI (V4).** The box
  does not need the enclosing frame to outlive anything.
- a captured `output` / `inout` formal in a child BODY — rejected by name (V4): its
  copy-out reads the formal's own frame slot, which does not see writes made through
  the box. Reading one in the initializer of a fork's own declaration is LEGAL and
  supported, which is the clause's own carve-out (§V4b.2).
- a STATIC declaration in a fork's block_item_declaration — rejected by name (§V4b.4):
  6.21 initializes static storage once before time zero, 9.3.2 on every entry to the
  fork's scope, and nothing implements either reading. An AUTOMATIC one is supported
  for every join kind (§V4b).
- `disable` / `wait fork` / `disable fork` — reject (V3).
- child-body constructs needing the unwind machinery — reject.

Mechanical / low-risk: the join record + parent frame live across the join
suspend (alloca after `coro.begin`, spilled into the frame — reuse the task
record/awaiter pattern); `$finish` mid-fork leaks suspended child frames
(accept, matches tasks); child-local VCD placement (minor).

## Phased plan

- **V1 — `fork … join`, static child set, no automatic capture.** Static ring
  bound + overflow guard (derisk #1). Per-child argument-less task coroutines
  spawned at the fork, parent suspends on a join record (count = W), last
  child wakes the parent. Backend-analysis plumbing (A), sensitivity exclusion
  (D), loud guards (E). Static locals shared (documented limitation, same as
  `declare_local` today).
- **V2 — `join_any` / `join_none`** (landed). Scoped and recorded in full at
  §"V2 scope" above: the join kinds differ at run time in one record field
  (`wake_at`) and in when a child starts; `join` keeps its frame-resident record and
  byte-identical codegen while the detaching kinds get a per-site, reference-counted
  heap record; the activation bound saturates for an unbounded spawn edge so the
  rings, `#0` batches and wait buckets all size from one answer; and two V1
  corruptions were fixed first.
- **V3 — `wait fork` / `disable fork`. LANDED, AND NOT HERE:
  `docs/design/process-class.md` §8.** `wait fork` = a barrier over the whole live
  descendant set; `disable fork` = an abort per descendant.

  **Neither was unblocked by V2 as scoped, and the dependency is worth keeping
  because both documents once stated it wrongly.** V2's per-fork-site heap join record
  is a COUNT used as a refcount; a "live-child set" that can be enumerated does not
  exist in it, every child handle is dropped as a dead SSA value at the spawn site, and
  `is_fork_child` is a compile-time `Function` flag rather than a runtime relation. So
  V3 owed an enumerable live-descendant structure — and that structure turned out to be
  the LRM 9.7 `process` control block's tree, so both statements shipped with the
  `process` class instead of here. Two consequences for this document:

  - **Every fork in a design with process control now takes the HEAP refcounted
    record**, including `join` (`design_uses_process_` in `lower_fork_inst` and in the
    child epilogue). A `join` record is a frame slot, which is sound only while the
    parent outlives its children; `kill` breaks exactly that, and an aborting parent
    additionally nulls the record's `parent` before releasing its reference so a
    surviving child wakes nobody. §V2.2's byte-identical `join` codegen therefore holds
    only while process control is unused — which is the gate, and is verified.
  - **The `ArgDir::Ref` capture refusal for the detaching kinds had two reasons, not
    one**: a `join_none` parent can return, and a killed parent's frame goes away under
    live children. V4 retired both by retiring the ref — and the second reason is why
    it had to retire it for `join` too, not just for the detaching kinds.

- **V4 — captured-automatic LIFETIME, not per-spawn copies. LANDED in full;
  spawn-time initialization of a fork's own declarations landed after it, §V4b.**
  This bullet used to
  read "give fork-captured automatics real per-spawn copies (frame locals), fixing
  `for … fork … i … join_none`". **That is the wrong target**, and §V2.5's own
  parenthetical already said so ("rather than reading a shared final `i`"): under
  LRM 9.3.2 a reference to an automatic of an ENCLOSING scope is a reference to that
  one variable, which is the entire reason 9.3.2's example has to write
  `automatic int k = j;` to get a copy. Copying implicitly would delete the
  distinction the clause is built around.

  Measured on the clause's own idiom,
  `for (int j = 1; j <= 3; ++j) fork $write("%0d ", j); join_none`:

  | | result |
  |---|---|
  | iverilog 13 | `4 4 4` — shared, the strict reading |
  | Verilator 5.050 | `1 2 3` — per-spawn copies, a deviation |
  | clotho today | `4 4 4` |

  So clotho is already right here, and per-spawn copying would have regressed it.
  It is right for an incidental reason worth keeping in view: a process-scope local
  goes through `declare_local`, which gives static signal-like storage (documented
  there), so the three children genuinely share one `j`. The refusal only bites where
  the local really is frame-resident — a task or function body.

  **The actual gap is lifetime.** A detached child holding a reference into its
  parent's frame outlives that frame: a `join_none` parent can return, and V3's
  `kill` can take the parent out from under live children. Two things follow:

  - An automatic declared INSIDE the fork block (9.3.2's `automatic int k = j;`) was
    filed here as "a child frame local by construction", needing nothing new. That is
    wrong on both counts, and §V4b is the correction: the declaration belongs to the
    FORK's scope rather than to any one child, so it is parent-side storage that every
    child of that entry SHARES — and it needs fresh storage per ENTRY to the fork,
    which neither a child frame nor a per-activation box provides.
  - A captured ENCLOSING automatic needs storage that outlives the parent activation
    while staying one variable. The mechanism already exists: box it and let the GC
    own it. Live coroutine frames are conservative, pinning GC roots linked at the
    ramp (garbage-collection.md §5.1), so a box reachable from the child's frame
    stays alive with no new lifetime machinery, no refcount and no frame-ownership
    surgery. The child receives the box POINTER by value, which also retires the
    second objection standing in the way today — nothing needs the fat `ArgDir::Ref`,
    so the "only packed integral and dynamic-array locals can be captured" restriction
    goes with it, and 13.4.4's `watch_for_zero` (a captured class-handle formal) works
    for the same reason as an `int`.

  **The box, per variable.** One box per captured variable, not one per activation:
  per-variable keeps the trace precise and does not hold an unrelated local alive for
  as long as any child of that activation runs, which for a `join_none` in a loop is
  the whole loop's worth of captures.

  A box is a **synthetic one-field class**, the shape `lower_process_class` already
  uses for the PCB handle. That buys the whole mechanism with no new IR: `ClassNew`
  allocates it through the GC fast path, the trace map falls out of the field table
  (`class_gc_map_global`), reference semantics are what a class handle already has, and
  the place is `HandleDeref` + `MemberSelect 0`. Nothing about it is keyed on a slang
  type: `Module::set_class_desc` takes a bare `Ref`, and a fresh `type_id` is
  `ClassIds::next++` without registering in the symbol map — the box is never `$cast`
  to, never a base, and has no methods, so its slot table is empty.

  **The wrinkle, and it decides the data structure.** `build_task` / `build_function`
  keep the invariant *formal k == frame Local k*, which the backend prologue and
  copy-out rely on. So a boxed FORMAL cannot simply become the box: the formal keeps
  Local k as the ABI's copy-in target, and the box is a second slot. `FnCtx` therefore
  needs `boxed: sym -> {box_local, box_class}` ALONGSIDE `localmap`, consulted first in
  `value_lvalue`, with a prologue store copying Local k into the box once. The formal
  slot is then write-only.

  That leaves one case to refuse rather than get wrong: an `output` / `inout` formal
  that is also captured. Its copy-out reads Local k, which no longer sees writes made
  through the box, so the box would have to be copied back before every return.
  Guarded loud until it has a reason to exist.

  **Which locals box** is decided before the frame is laid out, because a boxed local
  is boxed for every access in the activation, including ones lowered before the
  `fork` is reached. A pre-pass over the body collects every `ValueSymbol` referenced
  by a boxing fork's children; `fn_local` boxes a symbol in that set. The set is an
  over-approximation on purpose — a module signal or a static in it is simply never a
  frame local, so it never boxes — which keeps the pre-pass from having to resolve
  storage class before storage exists.

  **Which forks box: all of them.** This paragraph used to prescribe
  `kind != Join || design_uses_process_`, the same predicate the join record uses, and
  that was wrong twice over.

  It is not implementable where the decision has to be made. `design_uses_process_` is
  `facts_.uses.process`, computed by an IR scan (`Analysis.cpp`, on `ProcQuery` /
  `ProcCtl` / `ProcWait`) after the whole module is lowered; the boxing decision has to
  be made per subroutine BEFORE its frame is laid out. The frontend has no whole-design
  fact to consult and no pre-pass that could produce one (`check_spawn_roots`, the only
  design-wide frontend question, also runs after lowering).

  It is also the wrong shape. The storage a capture needs is a property of the CAPTURE,
  not of the join kind, and keying it on the kind is what produced two unrelated
  refusals that both read as fork limitations:

  - the fat `ArgDir::Ref` could carry only a packed integral or a dynamic array, so
    `fork … join` refused a string / real / unpacked / queue / class-handle capture by
    name — nothing to do with `join`;
  - `join`'s "the parent outlives every child" is false under `kill`
    (process-class.md §11 hazard 3), so even `join` did not really guarantee the
    lifetime the pointer needed.

  So the rule is one storage rule for every kind: a fork capture is a GC box handle.
  `BoxCandidateScan` takes `JoinAll` alongside `JoinAny` / `JoinNone`, and the kind
  disappears from the capture path entirely — `lower_fork` no longer has a
  `box_captures` flag, `build_fork_child` no longer takes one, and `lower_fork_inst`
  stops consulting `arg_dirs` because there is nothing but `In` left to find.

  **What it costs, and where.** One `ClassNew` per captured variable per activation,
  plus a load-and-deref per access instead of the ref ABI's load-and-epoch-check —
  which is not obviously worse, since a `ref` read emits `emit_ref_epoch_check` and a
  box read emits a null check on a handle that is never null. It is paid only by a body
  that forks over its own frame variables. A fork in a PROCESS body has no captures at
  all (a process-scope local is signal-like storage via `declare_local`), which is the
  common testbench idiom, so `ForkInfo::args` stays empty and no box is created.

  No kind-conditional survives. A `kind != Join` gate on the spawn-time-initialization
  refusal used to sit here, on the grounds that a `join` child is ramped and
  `coro.resume`d inline at the fork so its declaration initializers already run at the
  spawn. §V4b.3 measured that claim: it held only in a subroutine body, and only by
  accident of the per-activation capture box, while the same fork in a PROCESS body
  failed outright with `reference to unknown signal 'k'`.

  **`this` is an ordinary capture.** A bare property reference inside a method was
  refused for every kind ("child that reads a class property of the enclosing method"),
  on the stated grounds that the ref ABI carries a packed-slot pointer and not a
  handle. That reason dies with the ref, and the construct needs no special case at
  all: `value_lvalue` re-roots a bare property at the method's `this` automatic, and
  that automatic is a real slang `ValueSymbol` (`SubroutineSymbol::thisVar`, a
  `VariableSymbol` with `Automatic` lifetime) already living in `localmap` as Local 0.
  So `ForkCaptureDetector::check` maps a non-static `ClassPropertySymbol` onto
  `thisVar` and captures THAT; an explicit `this.p` already arrived that way. Three
  details make it work:

  - `thisVar` is in neither `getArguments()` nor the body's declarations, so
    `install_capture_boxes` boxes it explicitly, driven by a flag `ValueRefScan` sets
    when it sees a bare non-static property (the implied `this` reference is not
    written anywhere in the AST). `copy_in` is right for the same reason it is right
    for a formal — `splice_this` already gave it Local 0 and the prologue fills it.
  - The four sites that loaded `this` out of Local 0 now go through one
    `this_handle(b, l)`, which consults `boxed` first. Without that, a boxed `this`
    would be read as if the box slot were the handle.
  - `build_fork_child` sets `has_this` / `this_var` / `this_class` from a captured
    `this`, so a bare property inside the CHILD re-roots the same way. `this_class`
    stays the real class type; only the handle comes from the box.

  `this` is `const` (slang marks it so), so a by-value `In` formal would have been
  semantically identical and one indirection cheaper. Boxing it anyway keeps one path
  through the capture machinery rather than two.

  **What landed.** Every child's captures are boxed and shared, so LRM 9.3.2's
  reading holds in a task and a function as it already did in a process: the children
  of `for (int j…) fork … join_none` read the shared final `j` (`4 4 4`), a child's
  WRITE to a capture is visible to the parent, a captured class-handle formal survives
  the function returning (13.4.4's `watch_for_zero`), and a string capture works -- the
  `ArgDir::Ref` type restriction is gone with the ref itself. Both old refusals are
  lifted, for every join kind, and so is the implicit-`this` refusal. Backend:
  `lower_fork_inst` marshals a capture as an ordinary `In` scalar (must|may and z);
  `ref_actual_arg`, `push_ref_epochs` and the `gc_owner` assertion are gone from the
  fork path, `frame_out_targets`' `Fork` arm no longer reasons about a ref actual, and
  the x/z sweep's `Fork` arm treats an arg as an `In` scalar — the shared variable's
  own x/z lives in the box's single field and travels through the ordinary
  class-member Store cases instead.

  Two things this needed that were not in the plan:

  - A capture reference inside a child's DECLARATION INITIALIZER was invisible to
    `ForkCaptureDetector`, because an initializer hangs off the symbol rather than the
    statement. That made `fork begin automatic int k = j; … end join_none` fail as an
    unknown signal instead of capturing -- a pre-existing hole, now fixed in both
    walks.
  - **Spawn-time initialization was refused; it is now implemented — §V4b.** The
    refusal named the wrong construct as well as being a refusal: it fired for a
    declaration of a child's own `begin … end` too, which the clause initializes late
    on purpose ("the value of m is 4").

  Still cross-cutting with the broader automatic-lifetime gap (`declare_local`): the
  boxing decision is per captured variable, so it is a strictly smaller change than
  giving every automatic true per-activation lifetime.
- **V5 — `fork … join_none` inside a function (LRM 13.4.4). LANDED:** §"V5 scope"
  above. The spawn path needed almost nothing a function lacks — one `!none` gate on
  V3's `fork_live_slot` publish, which `join_none` never needed anyway. The work was
  13.4.4's caller-legality rule (`check_spawn_roots` plus `Process::spawn_root`,
  since a static initializer, a continuous assignment, a `final` block, a `$strobe`
  body and a DPI export all accepted a spawn silently before) and closing
  `reject_resume_once` over calls. Captures landed with V4.

## Verification

Value and ordering cases against iverilog 13, which implements all three kinds.
Two differential matrices were run outside the suite and their interesting cases
lifted into `tests/fork_test.cpp`:

- 29 `fork … join` shapes for the V2.0 bounds — widths 1..64 all waiting on one
  edge, `#0` batches, mixed wait/`#0`/NBA children, several live activations of one
  forking task body, recursion, loop reuse. Every shape with more than three
  concurrent waiters on one edge segfaulted before V2.0(a).
- 22 `join_any` / `join_none` shapes — ordering, a parent that ends while children
  are live, loop-spawned children (2/10/100), unbounded children on one wait bucket
  (2/8/40), two fork sites in one body, all-synchronous children, `join_any` followed
  by another fork, in-task and nested, NBA from a detached child, `#0` children.

All 51 also run clean under ASan + UBSan + LeakSanitizer, which is what actually
covers the reference-counted heap record: a missed release leaks and a double
release is a double free, and neither shows up in a value check.

Three of the 22 cannot be scored against iverilog at all — its `vvp` fails an
internal assertion (`vthread.cc:3793`, `of_JOIN_DETACH`) on a detaching fork inside
an automatic task. Those are pinned from the LRM and flagged as such in the test
comment.

No-regression gate: the full doctest suite, plus **byte-identical emitted LLVM IR**
for a design that does not fork (picorv32, 52 133 lines). The record type is still
split in two — the short `{ count, parent }` record keeps `join`'s join-record codegen
bit-for-bit unchanged when process control is unused. The stronger claim that used to
sit here, byte-identical IR for a design using only `fork … join`, no longer holds and
should not be restored: V4 made a `join` capture a box handle rather than a frame
pointer, so a `fork … join` that CAPTURES emits different (and correct) code. A
`fork … join` with no captures — which is every fork in a process body — is unchanged.

Not covered: the overflow FATAL itself has no unit test. There is no cap knob in
`SimulationConfig`, so reaching it needs a design whose static bound saturates AND
whose live count exceeds the ceiling — §V2.0(b)'s 3000-wide `#0` fork does it in
about 11 s of compile time, too slow for the suite, and a runtime FATAL would have to
go through `tests/Subprocess.h`'s `run_design_captured` rather than the in-process
`run_capture`.
