# Fork/join (concurrent process spawning, LRM 9.3.2)

Target: `fork … join` / `join_any` / `join_none`, plus `disable fork` /
`wait fork`, as concurrent sub-processes spawned inside an `initial`/`always`
block or task. This is the last core testbench primitive missing after
cross-hierarchy refs, `wait(expr)`, severity tasks, and immediate assertions.

Status: **V1 + V2 landed.** `fork … join` / `join_any` / `join_none`, in a process
body and in a task body (children capture the enclosing frame's packed integral /
dynamic-array locals by reference under `join`; the detaching kinds refuse a capture,
see §V2.2). Phased: V1 = `fork … join`; V2 = `join_any` / `join_none` (§V2 scope);
V3 = `wait fork` / `disable fork`; V4 = true per-spawn automatic-variable lifetime;
V5 = `fork … join_none` inside a function (§V5 scope; landed, no captures).

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
- **Captures** are refused by name for the detaching kinds, since nothing keeps the
  frame alive. A fork in a *process* body has no captures at all (parent variables
  are module signals), which is the common testbench idiom, so this costs little. It
  lifts with V4's per-spawn copies.

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

Still open, and unchanged by V2: `Structural::walk` has no `Fork` case, so children
are absent from `BodyFacts::calls`; and `scan_finish` omits `Fork`, so a `$finish` in
a child does not mark the parent may-finish. Harmless while `$finish` is a longjmp, a
live hazard for the cooperative-finish migration.

### V2.5 What V2 does not do

- **`wait fork` / `disable fork`** (V3) — both still refused by name. They need a
  per-process live-child *set*, which the count-and-wake_at record does not carry;
  `disable fork` additionally needs the general abort primitive
  (`docs/design/disable.md`).
- **Per-spawn automatic lifetime** (V4) — the classic
  `for (int i…) fork … use i … join_none` still cannot capture, and is refused by
  name rather than reading a shared final `i`.
- **Truly unbounded** live children — the ceiling in V2.3 is 1024, reported loudly.
- **A `fork` inside a function** — LANDED for `join_none` (§"V5 scope" below), which
  is the only kind LRM 13.4.4 allows there. A child still cannot capture the
  function's own formals, which is V4's job.

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

The child reads `p`, an automatic FORMAL, so the capture guards fire:
`fork..join_none child that reads the enclosing function's 'p' is not supported`.
(The message used to say "the enclosing task's" whatever the body was; it now names
the kind, because in a function the captures that matter are its own formals.) A
function's interesting state IS its formals, so V5 admits only children touching
module signals, static locals and static class properties: a monitor spawned by a
helper function, not the LRM's idiom.

That is an argument for finishing V4, not for widening the ref ABI. For a detached
child the right model is **copy-in by value at spawn** (per-spawn automatic lifetime,
V4): a class handle, an `int` or a string copied into the child's own frame removes
the lifetime problem `ArgDir::Ref` has, and for a function it is the only model
available, since the frame is gone the moment the call returns.

Adjacent friction NOT cleared here: `$time` / `$realtime` inside a function is still
refused, and its stated reason ("no wheel pointer in its ABI") is stale —
`lower_function` binds `wheel_` to the `clotho.wheel` global and `has_wheel()` is
unconditionally true. It is left alone because lifting it is a timescale question,
not an ABI one: LRM 20.3.1 scales `$time` to the time unit of the module that
invoked the call, and a function body is lowered ONCE with its own definition's
`time_mul_`, so a package function called from two modules with different
timescales has no single right answer to bake in. Refusing beats guessing, and it
is a separate piece of work. Practical consequence for this feature: print `%0t`
in the CHILD (a task body, where `$time` works) rather than in the spawning
function. `$strobe` / `$monitor` in a function keep a second, still-real reason (no
`coro_begin_` to hang the ramp's record slot off).

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

### V5.8 Adjacent gaps this widens but does not own

`BodyFacts::calls` is fed only by `TaskCall` / `TaskCallVirtual` — the `Call` arms
push nothing and there is no `Fork` arm — so `compute_subscription_order`'s closure
misses signals a fork child, or any plain function, blocking-writes, and
`wake_adj_` / `scc_nontrivial_` / `sub_order_` are computed on an incomplete graph
that `compute_fusion_groups` then reads. Pre-existing (a fork in a task has it too)
and unchanged in kind by V5, which only widens its reach. Likewise `scan_finish`
omits `Fork`, so a `$finish` in a child does not mark the parent may-finish —
harmless while `$finish` is a longjmp, a live hazard for the cooperative-finish
migration.

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
  real / queue / class handle) — rejected by name, and lifts when the ref ABI
  does.
- a child reading a class property of the enclosing method through the implicit
  `this` — rejected: the ref ABI carries a packed-slot pointer, not a handle.
- `disable` / `wait fork` / `disable fork` — reject (V3).
- child-body constructs needing the unwind machinery — reject.
- a capture under `join_any` / `join_none` — reject (§V2.2): the ref ABI needs the
  enclosing frame to outlive every child, which only `join` guarantees.

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
  - **The `ArgDir::Ref` capture refusal for the detaching kinds must STAY until V4**,
    now for two reasons rather than one: a `join_none` parent can return, and a killed
    parent's frame goes away under live children.

- **V4 — true per-spawn automatic lifetime.** Give fork-captured automatics
  real per-spawn copies (frame locals), fixing `for … fork … i … join_none`.
  Cross-cutting with the broader automatic-lifetime gap (`declare_local`).
- **V5 — `fork … join_none` inside a function (LRM 13.4.4). LANDED:** §"V5 scope"
  above. The spawn path needed almost nothing a function lacks — one `!none` gate on
  V3's `fork_live_slot` publish, which `join_none` never needed anyway. The work was
  13.4.4's caller-legality rule (`check_spawn_roots` plus `Process::spawn_root`,
  since a static initializer, a continuous assignment, a `final` block, a `$strobe`
  body and a DPI export all accepted a spawn silently before) and closing
  `reject_resume_once` over calls. Captures stay refused until V4.

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
for a design that does not fork (picorv32, 52 133 lines) and for one that uses only
`fork … join`. That is a stronger check than a timing comparison and is why the
record type is split in two — the short `{ count, parent }` record keeps `join`
codegen bit-for-bit unchanged.

Not covered: the overflow FATAL itself has no unit test. There is no cap knob in
`SimulationConfig`, so reaching it needs a design whose static bound saturates AND
whose live count exceeds the ceiling — §V2.0(b)'s 3000-wide `#0` fork does it in
about 11 s of compile time, too slow for the suite, and a runtime FATAL would have to
go through `tests/Subprocess.h`'s `run_design_captured` rather than the in-process
`run_capture`.
