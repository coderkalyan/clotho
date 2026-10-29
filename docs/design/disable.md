# `disable` statements (process control, LRM 9.6.2 / 9.6.3)

Target: `disable <hierarchical_block_identifier>;`, `disable
<hierarchical_task_identifier>;`, and `disable fork;`, plus the deferred- and
procedural-concurrent-assertion flush points those statements trigger (LRM
16.4.4, 16.14.6.4).

Status: **V1 + V2 landed. `disable` of a named block and of a task both work,
wherever the statement sits.** Three forms:

- **Lexical, in a process body** — a structured multi-level exit
  (`ir::Inst::Tag::NamedBlock` + `Disable`). A bare branch, no runtime at all, which
  is what keeps a design using only this form byte-identical to one with no
  `disable` in it.
- **Remote** (`ir::Inst::Tag::DisableRemote`) — a sibling block, a block in another
  process, a block in a task or a fork child, or a TASK. Every activation the target
  names is marked and force-woken, and each aborts itself at its own poll point.
  LRM 9.6.2 example 3 and example 4 are both this.
- **Lexical, in a body that can have several activations** — the two together: mark
  every activation on the target's list, then take the structured exit out of this
  one. That is the labelled-break idiom inside a task.

LRM 16.4.4's outermost-scope flush lands with them, lexically and remotely, as a
`DeferFlush` generation bump. Oracle-matched against iverilog 13.0 on 9.6.2 examples
1, 2, 3 (both readings), 4 and 5, plus the stale-timer, always-re-arm, two-instance,
two-activation, `#0`, fork-child and action-block cases.

Still refused, and now only ever for one reason plus fork: a target whose body can
park where the abort cannot claim the park — a TIME-CONSUMING task call, a fork, or
an `expect` — and `disable fork` / `wait fork`, which need the enumerable
live-descendant set fork owes and which no `disable` work can supply. The 9.7
`process` class is a separate feature behind the same std-package guard as
`semaphore` and `mailbox`.

**What V2 cost, against what this document long assumed.** The epilogue is why the
task target was cheap: LRM 9.6.2 says a disabled task's caller resumes at the
statement after the task-enabling statement, which is exactly what
`task_epilogue_bb_` already does. An aborted activation takes the ORDINARY epilogue
with copy-out skipped (9.6.2 leaves output/inout results unspecified) — the deinit
walk, the fork join-count decrement or the parent push, and the final suspend all
stay. No ABI change, no caller-side poll, no generation threaded through the call
boundary, no epoch, no handle array, no zombie stage, no `Wheel.h` change, no
runtime change.

What it needed instead: one activation list per target (a frame-resident node,
linked when an activation enters the target and unlinked when it leaves), a claim on
the `#delay` park record, and a poll after each park. The self-disable case is the
one activation no force-wake can reach — it is running, not parked — so the walk
skips it and the frontend follows the mark with an explicit abort.

Six predictions this document made before V1 were wrong, and the sections below
are corrected rather than left standing: the ring-overflow guard has LANDED (its
`design_forks()` gate is what needed widening, §Hazards 1); a `disable`-bearing
process is thunk-ELIGIBLE, so nothing forced it to a coroutine (§Hazards 12 — now
fixed by a payload-dependent test in `is_simple_always`); `@clotho.proc_handle`
does not exist in the tree at all; **`Symbol*` identity DOES select an instance**,
the opposite of what the pre-V2a scoping concluded — slang gives every elaborated
instance its own `StatementBlockSymbol`, verified by two instances of one module
getting `L0` and `L1`, so the whole instance-ordinal step was unnecessary;
16.14.6.4 is not the generation counter's problem in any form (§Assertion
interaction); and V3's stated gate is wrong — fork V2 as scoped keeps a refcount,
not an enumerable set, so no phase of either document commits to building what
`disable fork` needs.

What V2a actually cost, and what it did NOT need: no epoch counter, no handle
array, no zombie stage, no `Wheel.h` change, no runtime change. Three flat
per-target tables (occupant handle, its scheduled-flag slot, its published park
slot) plus a pending mark; a claim field on the `#delay` park record, which becomes
a fresh heap allocation per arm only in a design that has a remote `disable`; and a
poll after each suspend inside a targeted region. The epoch the original design
proposed was for "all activations of a task", which V2a does not implement — for a
single-activation process-body target a live-occupant slot is both cheaper and
more precise, because it answers "is the block currently executing?" directly.

What V1 actually cost, against what §Design predicted: two `Inst::Tag`s, a
`PayloadKind::Label`, a `BlockInfo` extra record, a `block_targets_` stack in the
backend beside `loop_targets_`, and a Collect-pass scan
(`scan_disable_targets`) that labels the blocks some `disable` names — a block
with no `disable` naming it is still flattened, so no existing design changes
shape. No runtime, no scheduler, no global, no depth arithmetic: the label made
the "region depth" the doc originally proposed unnecessary, which matters because
`foreach` emits one loop per dimension and `wait` synthesises a loop of its own,
so a depth would have had to count regions the frontend does not otherwise track.

`disable iff` (LRM 16.12.1) is an unrelated SVA production that shares only the
keyword; it is implemented (`src/Sva.cpp:552-569`) and out of scope here.

## Semantics (LRM 9.6.2, 9.6.3, 16.4.4)

The statement terminates the activity of a named block or a task. Execution
resumes at the statement following the block, or following the task-enabling
statement. The load-bearing rules, in the order they constrain the design:

- **Targets are static, not dynamic.** `disable` names a syntactic block or
  task; it "uses the static, syntactical information of the disabled block"
  (9.6.3), and so "shall end all processes executing a particular block,
  whether the processes were forked by the calling thread or not". `disable
  fork`, by contrast, is the dynamic form: it kills all *descendants* of the
  calling process, "not just immediate children", including descendants of
  children that already terminated.
- **All activations, not one.** "If a task is enabled more than once, then
  disabling such a task shall disable all activations of the task." Nested task
  enables disable downward along the chain. Automatic tasks behave as regular
  tasks "for all concurrent executions".
- **No live activation means no effect.** "If the block is not currently
  executing, the disable has no effect."
- **A loop-body block behaves as `continue`** (9.6.2 example 3); a block that
  encloses the loop behaves as `break` (example 5). SV has no labelled
  `break`/`continue`, so `disable` is the only multi-level structured exit in
  the language.
- **Functions cannot be disabled**, only named blocks inside them. slang
  enforces this before Clotho sees the statement (below). A `disable` inside a
  function that targets a block or task *calling* that function is explicitly
  undefined behaviour.
- **Three effects are unspecified** when a task is disabled: output/inout
  argument results, nonblocking assignments already scheduled but not
  executed, and procedural continuous assignments (`assign`/`force`).
- **Deferred assertions (16.4.4) are dropped, not executed.** Disabling a
  specific deferred assertion cancels its pending reports; disabling the
  *outermost* scope of a procedure flushes that procedure's whole deferred
  queue. "Disabling a task or a non-outermost scope of a procedure does not
  cause flushing." 16.14.6.4 states the same rule for the pending
  procedural-concurrent-assertion queue, with the added constraint that a
  matured attempt is immune.

The open question here — whether 16.4.4 wants eager execution or a stale drop —
is settled by 16.4.4's own wording: *cancelled* and *cleared*. Drop semantics,
the same shape as the existing generation counter.

## Current state — V1 landed

- **slang** binds the target for us. `DisableStatement::target` is an
  `ArbitrarySymbolExpression` and `DisableStatement::fromSyntax`
  (`build-asan/_deps/slang-src/source/ast/statements/MiscStatements.cpp:26-44`)
  hard-enforces `diag::InvalidDisableTarget` unless the resolved symbol is a
  `StatementBlockSymbol` or a `SubroutineSymbol` with
  `subroutineKind == Task`. Clotho needs no hierarchical-path parser: `disable
  blk;` and `disable top.u1.blk;` arrive as one node shape. Both use sites
  unwrap `.as<ArbitrarySymbolExpression>().symbol` with no null check
  (`src/lower/Statements.cpp, lower_disable`), relying on that enforcement.
  `DisableForkStatement` carries no fields.
- **Eight frontend refusal sites**, each with its own message. `lower_disable`,
  in the order it tests: a task target (`src/lower/Statements.cpp, lower_disable`); `fn_ctx_ !=
  nullptr`, which is one predicate covering a function body, a task
  body, a fork child, an assertion action block, and a `$strobe`/`$monitor`
  print thunk — there is no separate fork-child test, and `FnScope`
  additionally swaps `named_blocks_` empty, so the enclosing
  process's blocks are invisible from inside a body even if that predicate were
  relaxed; a target absent from the named-block stack; a found target
  with no label (unreachable today); and the 16.4.4 outermost-scope
  flush. One level up in `lower_stmt`: `disable fork` and
  `wait fork`. Independently, `ReactiveSafetyChecker` rejects both
  `Disable` and `DisableFork` in either copy of an assertion action block.
  `wait_order` is the one form in this family that still falls to the
  generic `unsupported statement`.
- **The backend has a ninth, and it is a throw, not a diagnostic.** `block_end`
  (`src/ir/llvm/Inst.cpp`) throws `BackendError{"internal: disable
  of a block that does not enclose it"}` when a `Disable`'s label is not on
  `block_targets_`. Any V2 form that reaches codegen with an unresolvable label
  is crash-class, not a refusal — so the remote form must resolve outside
  `block_targets_` entirely, not lean on that search.
- **The target set is design-wide and complete before any code is emitted.**
  `scan_disable_targets` (`src/lower/Assertions.cpp`) runs in `Pass::Collect`,
  which walks the whole instance tree before `Pass::Emit`,
  and neither `disable_targets_` nor `disable_flushes_`
  is ever cleared. A REMOTE target is therefore already knowable when
  its disabler is lowered — stronger than §Design assumed. The `Block` case
  does an unqualified lookup, with no same-process check, so a block
  targeted only from another process ALREADY emits its `NamedBlock` region
  today: the remote form's end block exists, only the branch to it is missing.
- **What the map does not record.** The value is a bare `uint32_t` label — no
  owner process, no instance scope, no `FunctionId`. The key is the
  `StatementBlockSymbol*`, which every elaborated instance of a module SHARES.
  So `Symbol*` identity does **not** resolve which instance's epoch to bump;
  §Design's claim that it does is wrong, and V2 needs an instance dimension on
  the value side. That is a value-shape change, not a new pass.
- **Two latent frontend bugs are held shut by the `fn_ctx_` refusal alone.**
  `scan_disable_targets` is only ever called for `ProceduralBlockSymbol`
  members (`:1265` is its sole call site), so a named block declared inside a
  task or function is never labelled even though the shared `Block` case would
  label it — relaxing `:6721` or `:6728` without widening the scan makes the
  "internal: … not labelled" path at `:6751` reachable. And
  `proc_root_block_` (`:6024`) is assigned per procedure and never restored, so
  a subroutine body built lazily at its first call site reads the enclosing
  procedure's outermost block at `:6753`.
- **`OpenBlock` is shared with `%m`.** `hier_name()` (`:15221-15232`) iterates
  the same stack outermost-first appending `sym->name` (LRM 21.2.1.5), so any
  V2 change to that frame must keep both the name and the order.
- **No process identity, no process table, and no handle array.** A process is
  the raw `ptr` from `llvm.coro.begin`; `src/runtime/*` has no process struct.
  `@clotho.proc_handle` does not exist anywhere in the tree — the only
  occurrence of that name is this document. Handles live as SSA values inside
  the seed loop (`src/ir/llvm/Scheduler.cpp, emit_active_run`) and are then dropped; the
  only durable copies anywhere are wait-bucket entries (`{handle, flag_ptr}`),
  field 2 of a frame-resident `awaiter`, and the `parent` field of a task or
  fork record — all three region-tagged through `parked_handle`.
  `fnmap_` gives the static `FunctionId` for a task target and says nothing
  about which calls are live.

## The machinery this must build on

Three existing idioms cover the whole design; nothing else in the tree is
close.

1. **Cooperative exit, never an unwind.** `$finish` sets one byte and returns;
   each frame leaves through its own epilogue (`emit_finish_exit`,
   `src/ir/llvm/SystemTasks.cpp, emit_finish_exit/emit_finish_check`, four shapes: process coroutine, thunk-
   elided process, task, function). A task branches to `task_epilogue_bb_`,
   which runs `emit_copyout`, then `deinit_body_strings`, then
   either decrements the fork join count or `emit_wake_push`es the caller's
   `parent`, then falls into the final suspend. That order is load-bearing: a
   child's heap is released BEFORE the parent can be resumed. There is no
   stack-unwind primitive: the former longjmp "cannot work on Windows (JIT'd
   frames carry no unwind data) and corrupted the heap elsewhere"
   (`src/runtime/Runtime.h:113-119`). A `disable` must be cooperative for the
   same reasons.

   **The constraint V2 inherits from this, and it is the sharpest one in the
   file:** the epilogue is reachable ONLY from structured returns —
   `emit_finish_exit:14583`, `lower_ret:16357`, and the structured fall-through
   at `:3663`. No suspend point branches to it (`emit_suspend:3898-3906`
   wires case 1 to `coro_cleanup_bb_` and case 0 to the resume block, nothing
   else). A force-woken activation therefore cannot be "unwound" by the
   disabler in any sense; it must re-enter at its own resume block and branch to
   the epilogue from there under an epoch test. The poll point is not a
   stylistic preference — it is the only edge that exists.
2. **Epoch filtering instead of removal.** `InertialState::epoch`
   (`src/runtime/Scheduler.cpp`) + `rt_intra_rec::epoch`
   (`src/runtime/Runtime.h:1330-1336`) deschedule a pending record by bumping a
   counter (`runtime/Scheduler.cpp, rt_inertial_decide`) and dropping the
   record when its timer fires and the stamp no longer matches
   (`rt_inertial_cb`). `@clotho.defer.gen.<g>` +
   `rt_defer_report(gen, gen_val)` (`src/ir/llvm/SystemTasks.cpp, defer_report_fn`;
   `src/runtime/Scheduler.cpp, rt_defer_report`, comparison in `defer_mature`)
   does the same for deferred reports and *is already* the LRM 16.4.2 flush
   point. Neither structure is ever unlinked.

   The two are different shapes, and V2 wants the second: `defer.gen` is a flat
   per-group `i64` global with no anchor, whereas the per-instance anchored
   array is `build_sched_flags` (`:793-829`) — dense def-local ordinals,
   per-anchor `clotho.inst.<hier>.sched` arrays, a root array for unowned /
   PreInit / package processes, and a constant-GEP accessor `sched_flag_ptr`
   (`:832-838`) valid in any block. A `build_disable_epochs` /
   `disable_epoch_ptr` pair is a near-literal transcription of that with `i32`
   → `i64` and "process index" → "target index".
3. **A destroy edge already exists at every suspend.** `emit_suspend`
   (`src/ir/llvm/`) switches `coro.suspend` with case 1
   wired to `coro_cleanup_bb_`. It is unreachable in practice today.

What is missing, exactly: **the wheel cannot cancel** (`Wheel.h`'s entire public
surface is `now`/`empty`/`schedule`/`advance`/`tick`; slot lists are singly
linked with no back-pointer, `Timer` does not even store its own deadline any
more — the heap entry carries it — and the overflow queue is a
`std::priority_queue` with no erase), **the rings cannot remove** (fixed-size
FIFOs, no removal, bulk `head := tail` only on `$finish`), **static wait buckets
cannot compact** (there is no length global at all: capacity is a compile-time
constant and the scan is fixed-trip, a padded slot is recognised by a null
*flag* pointer, `emit_static_scan_rt:7648-7652`, and a non-padded bucket carries
no null check whatsoever), and **`coro_cleanup_bb_` does not release frame-owned
heap** (`:2862-2870` and `:3613-3621` are `coro.free` only, so a raw destroy
leaks every frame-local string, dynarray, queue, assoc, and aggregate with a
heap leaf — `deinit_body_strings:3418-3443`, which also walks nested aggregates
via `emit_agg_deinit`, runs only at the normal exits).

## Design

### Targeting: one activation list per target — LANDED

Every disable *target* gets a dense id: the `StatementBlockSymbol`s and task
`SubroutineSymbol`s some `disable` names, all known before any code is emitted
(`scan_disable_targets`, `Pass::Collect`). The id comes from the Module, not the
Lowerer, because a multi-top design gives every top its own Lowerer sharing one
Module.

**An id already names one target in ONE instance, and this is the fact the design
turned on.** slang gives every elaborated instance its own symbols, so
`disable_targets_`, keyed on the pointer, is already per-instance — two instances of
one module get `L0` and `L1`, which `--emit ir` shows directly. The earlier claim
that `Symbol*` identity does not distinguish instances was wrong, and load-bearing:
it is why the design reached for the per-instance anchored-array shape `proc_sched`
uses. Nothing anchored is needed.

One flat array indexed by target id does it: `@clotho.disable.head[t]`, the head of
the list of activations currently inside that block, or currently running that task.
An activation links a frame-resident node when it enters the target and unlinks it
when it leaves:

    { ptr next, ptr prev, ptr handle, ptr park_slot, ptr sched_flag, i32 abort }

Doubly linked so the unlink is constant-time, frame-resident so it is per activation,
and published rather than looked up so the disabler needs no process table.

The list, not a single occupant slot, is what LRM 9.6.2 actually asks for: "if a task
is enabled more than once, then disabling such a task shall disable all activations
of the task". A block in a process body simply never has more than one node on it —
which is why the first cut got away with a slot, and why blocks in tasks and fork
children did not.

A thunk-elided group links nothing: it has no handle, and having no suspend it cannot
be inside the target while any other process runs — so the list stays empty and the
`disable` is the no-op the LRM already prescribes. A group holding either half of the
remote form is nevertheless forced to a real coroutine (`is_simple_always`), because
the disabler's baked target id is per-instance and only a thunk group is ever shared
across instances.

### Aborting: force-wake exactly once, then a structured exit — LANDED

Marking is not enough: LRM 9.6.2 requires the block's activity to end *at the
instant the `disable` runs*, and the statement after the block to execute then. So
the occupant has to be woken, not merely marked.

**The whole difficulty is waking it exactly once, and this document did not have
it.** The source the occupant is parked on is still armed and will fire later; the
wheel cannot cancel. A second wake is not a harmless duplicate — it resumes the
coroutine at whatever suspend point it has reached by then, so a `#100` two
statements later ends early, or a completed `initial`'s freed frame is resumed.
That is a correctness bug the epoch/zombie design never addressed, because the
zombie stage answers frame *lifetime* for an activation that is ending, and a
process body does not end — it carries on after the block.

What landed instead is a claim, on the park rather than on the process:

- **Parked on a `#delay`.** The park record carries a `state` claim, and — only in
  a design that has a remote `disable` — is a fresh heap allocation per arm, so a
  stale record and a live one cannot be the same memory. (One reused per-frame
  record defeats every filter: the next arm overwrites the field the stale timer is
  about to read.) The disabler reads the occupant's published park slot, claims the
  record, and pushes. The wheel's trampoline pushes only if it finds the claim
  clear, so the timer that fires afterwards frees the record and wakes nobody.
- **Parked on an event, or already queued.** There is no live record, and the
  scheduled flag is the design's existing one-wake-per-process token: push only if
  it is clear, exactly as `emit_static_scan_rt` does.

The scheduled flag cannot serve as the token in the first case, and finding out why
cost a real bug: mid-body that flag is deliberately SET, so the process's own
sensitivity cannot re-wake it. Clearing it at a `#delay` park — which is what an
early version of this did, to make the dedup uniform — let an `always @(posedge
clk)` body be re-triggered by its own clock while parked, so `always @(posedge clk)
begin : b #100 …; end` completed every 20 ticks instead of never.

The woken occupant then polls (`emit_disable_poll`, emitted in `lower_region` after
any instruction that can park, and only while some enclosing block is remotely
named) and, if the mark is set, branches to that block's end — the same structured
exit the lexical form uses, so the end block's own bookkeeping clears the mark and
the occupant slot. Nothing is destroyed from outside, nothing unwinds, and a process
body's frame is not reclaimed because the activation does not end.

Two things the poll depends on for its placement. It runs *after* the parking
instruction's own resume bookkeeping, which is why the hook is in `lower_region`
and not in `emit_suspend`: a `wait` has to compact itself out of its buckets first.
And in a design with a remote `disable` a single-signal `wait` compacts itself out
too, which it otherwise need not — a force-wake neither fires nor clears the bucket
it was parked in, so the entry would otherwise wake it later at an unrelated
suspend point.

The parks V2a does NOT support are refused in the frontend by walking the target
block's body (`hard_park`): a `#0` or a runtime-valued delay (the inactive batch has
no per-entry claim), a task call or a fork (the process is parked behind another
activation's completion, which would resume it a second time), and an `expect`.

Consequences worth stating plainly:

- **No removal primitive is needed anywhere.** A static bucket entry belongs to a
  process that re-arms, so it must *not* be removed. A dynamic entry is compacted
  out by the woken activation itself, which is machinery that already existed for
  the multi-signal case.
- **A disabled process body never frees its frame.** Terminating the current
  activation of `always begin : monostable … end` (9.6.2 example 7) means
  resuming after the block, which is the end of the always body, which re-arms
  the loop. Only task activations and fork children have a frame to reclaim, which
  is why V2a — process-body targets only — needs no frame lifetime story at all.
- **No handle array was needed.** `@clotho.proc_handle` does not exist; the name
  appeared only in this document. The occupant publishes its own parked handle on
  region entry, which is strictly less machinery than a seed-loop global: one store
  per region entry, in a region that only a targeted block has, and no enumeration
  anywhere. V2b's per-activation list is still owed for task targets.
- **A pending wheel record does NOT force a zombie stage** — the conclusion this
  document reached, and the reason it was wrong is worth keeping. The wheel does
  point into the frame and cannot be cancelled, so a *frame being freed* under a
  live timer would be a use-after-free; but a disabled process body's frame is not
  freed (bullet 2), so the only real hazard is the stale *wake*, which the claim on
  the record settles. The record is heap-allocated per arm in a disable-using
  design, so it also stops pointing into the frame at all. `coro.destroy` remains
  not an alternative: it IS used in-tree (`emit_finals_run`, `emit_preponed_run`,
  `emit_observed_run`, and the active drain) but only ever on a handle that has run
  to completion or is gated on `coro.done`. A real `WheelImpl::cancel`
  (doubly-linked slots plus lazy heap invalidation) would let the per-arm heap
  record go away; it is an optimisation now, not a prerequisite.
- **Detached event timers were already heap-allocated.** A `->> #d e` timer is not
  frame-resident and is freed by its own expiry thunk (`build_delay_event_thunks`),
  which is the precedent the per-arm park record follows.

### IR and lowering shape

Two forms, distinguished at lower time from the resolved `Symbol*`:

- **Lexical form — LANDED.** The target block lexically encloses the `disable`
  inside the same lowering unit, with no intervening call boundary, *and* the
  enclosing unit is single-activation (`fn_ctx_ == nullptr`, a process body — not
  a task, not a fork child, so no second activation can be executing the same
  block). This is a pure structured exit, and it needs a place to branch TO,
  which a flattened block does not have: the end of the block sits in the middle
  of the enclosing region (LRM example 2), not at any loop's exit. So a targeted
  block becomes `Inst::Tag::NamedBlock` — one region plus a label — and
  `Inst::Tag::Disable` carries that label, resolved in the backend the way
  `Break`/`Continue` are but against `block_targets_`, a (label, end-block) stack
  beside `loop_targets_`. A label, not the region depth this doc first proposed:
  depth would have to count emitted regions, and `foreach` emits one loop per
  dimension while `wait (expr)` synthesises a `forever` of its own, so the
  frontend would have had to track a number it otherwise never needs. No epoch,
  no wake, no runtime. Covers LRM examples 1, 2, 3-as-`continue` and 5.
- **Remote form — LANDED for a process-body target.** A sibling block, or a block
  in another process. `Inst::Tag::DisableRemote` carries the target id and is NOT a
  terminator: the disabling process carries on. `ir::BlockInfo` grew a `remote`
  flag, so only a block some `disable` names from outside pays for the occupant
  bookkeeping — a purely lexical target emits exactly what V1 emitted.
  `Inst::Tag::DeferFlush` carries 16.4.4's bump.

  Three facts made this cheap, all established against the code first:
  **(a)** nothing enforced that `Disable` is a terminator. `Ir.h` claimed it, but
  that was a backend convention: the frontend does not truncate the region after
  `b.disable(...)`, there is no IR verifier for terminator position, and the only
  enforcement is `lower_region`'s early break on `getTerminator()`. So a
  fall-through form violated no invariant; only the comment needed correcting.
  **(b)** the target set is complete before Emit and a remotely-targeted block
  ALREADY emitted its `NamedBlock` region, because the `Block` case looks the
  symbol up unqualified — the end block existed, only the branch to it was missing.
  **(c)** `PayloadKind::Label` is one `uint32_t`, and since the id is already
  per-instance (§Targeting) that is all the remote form needs: no move to
  `K::Extra`, no instance selector.

  Still refused here: a task target and the self-task case (LRM example 4), which
  must end EVERY activation and so needs V2b's frame list rather than one occupant
  slot; a target inside a fork child, for the same reason; and a target whose body
  parks in a way the abort cannot cancel.

`named_blocks_` is now `Symbol*`-keyed (a `{block symbol, label}` frame), and it
is cleared in `recover()` as well as swapped at each unit boundary — a throw
inside a named block used to leave a stale entry, which would now mis-classify a
later `disable` as lexical. The IR stays region-structured — no basic blocks, no
computed branch — preserving the invariant
`docs/design/synthesis-and-lsp-scoping.md:39-49` rests on. Both new tags are
classified NOT synthesizable: sv2v's jump ordinal could encode a multi-level
exit, but the legalizer's guard narrowing models only the two bare jumps
(`docs/design/process-legalization.md:693-725`), so claiming otherwise would
silently under-guard the stores after a `disable`. A process holding one declines
with `// process N: not synthesizable`, which is the existing path.

### Assertion interaction (16.4.4 / 16.14.6.4)

Drop semantics — but the two LRM subsections need two *different* mechanisms,
which this document previously conflated into one bump.

**16.4.4, the deferred report queue: a one-line bump, and the granularity is
already right.** At a disable of the *outermost* scope of a procedure, bump that
group's `@clotho.defer.gen.<g>` (`defer_gen_ptr`,
`emit_defer_gen_bump`) alongside the epoch bump. That drops exactly the
disabled procedure's pending reports and nothing else, for a reason worth
recording because it is not obvious: a process holding a deferred `Severity`
fails `is_simple_always` (`src/ir/Analysis.cpp:1425-1432` returns false on
`defer != 0`), identical-sensitivity fusion admits only `simple_always`
processes (`ir/llvm/Analyses.cpp, compute_fusion_groups`), and cross-instance body sharing is
thunk-only. So a group with a deferred assertion always has
exactly one member and is never shared across instances: the counter is already
per-process-per-instance. No new tagging is needed for this bullet.

Do **not** call `rt_defer_flush_observed`/`_final`. The reason is not the one
stated here before: those are not merely a drop. `defer_mature`
(`src/runtime/Scheduler.cpp`) FIRES every same-mode report whose
generation is still current and removes every same-mode entry from the queue, so
calling one from a `disable` in the Active region would emit other processes'
reports early, out of their maturation region, as well as clearing them.

**16.14.6.4, procedural concurrent assertions: not a queue, and the generation
counter cannot reach it.** There is no pending-attempt queue anywhere. A
concurrent assertion — procedural or module-item — becomes its own
`Activation::Observed` process (`src/lower/SvaMonitor.cpp, emit_monitor`), and its in-flight
attempts are hidden 2-state signals of that monitor (`MonitorState`),
not records. That monitor is a different `ir::Process` with a
different activation kind, so it is not in the procedure's fusion group and
`defer_gen_ptr` cannot address it: bumping the generation has literally zero
effect on an attempt. Two further facts shape the real fix:

- **Maturation immunity is free.** An attempt matures by pending its action
  block onto the reactive ring (`lower_reactive_pend:14408-14414`), a fixed FIFO
  of raw handles with no owner tag and no per-entry removal. A matured attempt
  cannot be found again, let alone withdrawn — which is exactly what 16.14.6.4
  asks for.
- **The abandonment primitive already exists, and it is not an epoch.**
  `MonitorControl::kill` (`src/lower/Lowerer.h`) is `$assertkill`'s request
  latch, held until the monitor's own clocking edge, where it joins `disable iff`
  in the reset term. The owner linkage exists too: `AssertSite::owner` is the
  enclosing `ProceduralBlockSymbol` and `AssertSite::label` the innermost named
  block between them, maintained by `scan_assert_control` for
  `$assertcontrol` targeting. So a 16.14.6.4 flush is shaped like "raise `kill`
  on every monitor whose `owner` is the disabled procedure", not like a
  generation bump.
- **And it is blocked upstream anyway.** A procedural `assert property` in
  clotho does not start attempts because the procedure executed the statement:
  outside `expect`, the seed gate is only "`$assertoff` is not set"
  (`bare_seed_gate:10856-10866`), so the monitor free-runs like a module-item
  one. 16.14.6.4's premise — that pending attempts belong to the procedure's
  execution — is not modelled, which is a concurrent-assertion gap, not a
  `disable` gap.

V1 carries neither, so it refuses, and only where the rules actually bite:
`outermost_named_block` (`:5994-6009`) peels single-statement lists and the
timing control to find the block that IS the procedure's outermost scope, and
`scan_disable_targets` records whether that procedure holds a deferred or
procedural concurrent assertion (`:7353-7362`). The predicate is deliberately
over-approximate — every target of a flushing process is inserted into
`disable_flushes_` (`:7371-7374`) and the narrowing to "outermost" happens at the
use site (`:6753`) — and it is sound only while `disable` inside a subroutine
stays refused, because the scan does not follow calls. Disabling a block nested
deeper never flushes (16.4.4's own wording), so it stays legal and the report
still matures — which `tests/loop_test.cpp:787-819` pins in both directions.

One case needs machinery that does not exist at all: a `disable` naming one
deferred assertion LABEL (16.4.4's first bullet). Two deferred assertions in one
process share one counter, so a bump cancels both, and `DeferReport`
(`src/runtime/Scheduler.cpp`) carries `{line, gen, gen_val, is_final}` —
no statement identity whatsoever, with `line` already rendered at queue time.
A statement label on an `assert #0` is also never pushed onto `named_blocks_`
(only `StatementKind::Block` with a `blockSymbol` is, `:6570`), so such a
`disable` does not even reach a 16.4.4-aware path today: it hits the generic
non-enclosing-target message. This needs both a target-recognition path and a
per-report tag, and is out of scope for V2.

`ReactiveSafetyChecker` keeps rejecting `disable` inside assertion action blocks,
but not for a region reason: a `disable` suspends nothing, so no wake is
involved, and the checker explicitly retired the "a reactive body is resumed
once" reason. What stands in the way is this document's
own V1 restriction — a `disable` is a lexical exit from an enclosing block of a
*process* body, and an action block is lowered as a task body (and, for a strong
property, again inside a `final` process), so neither copy has the block to
leave. Note the backend's independent backstop `reject_unsupported_reactive`
(`src/ir/llvm/Analyses.cpp`) has NO `disable` arm — the tag never reaches
reactive code because the frontend refuses first, so if V2 lifts the frontend
refusal the backstop silently accepts.

## Hazards

Ranked by how much damage a naive implementation does.

1. **Ring overflow is guarded, but only for a design that forks.**
   `emit_active_push` (`ir/llvm/Scheduler.cpp`) calls `emit_ring_full_check`,
   which calls `rt_ring_overflow` — `FATAL: the <name> overflowed
   at <t> (capacity N entries)` then `exit(1)`, `runtime/Scheduler.cpp` — when
   `tail − head >= cap`, and `emit_len_full_check` does the same
   for the length-counted buffers. Six push sites over five named buffers are
   covered: active ring, reactive ring, the runtime-selected ring in
   `emit_wake_push`, the inactive `#0` batch, the reactive inactive batch, and
   the dynamic wait bucket. Both guards used to open with `if (!design_forks())
   return;`, because the check cost ~40% of sim time on a wake-heavy design
   (combchain) and ~25% of codegen time — so a design that
   force-woke but never forked emitted NO overflow checking at all. **RESOLVED by
   V2a:** the gate is now `ring_guard_wanted()` = `design_forks() ||
   design_uses_disable_`. Sizing is unchanged —
   `bit_ceil(max(nproc + fork_ring_slots(), 1))` — and needs no Σ term for the
   remote form, because an occupant is a process already counted in `nproc` and the
   wake dedup admits at most one queue entry per process.
2. **One capacity, four buffers.** `queue_arr_ty_` and `active_mask_` are shared
   by the active ring, the inactive `#0` batch, the reactive ring and the
   re-inactive batch (`:7238`, `ensure_inactive_queue:7260-7268`,
   `ensure_re_inactive_queue:7379-7392`). Any V2 capacity increase pays for all
   four.
3. **`coro.destroy` on a queued handle is a use-after-free**, and no ring
   supports removal. The design's answer is that nothing is ever destroyed from
   outside; if that discipline slips, corruption is silent.
4. **Frame-owned heap leaks on any raw destroy.** `coro_cleanup_bb_` does only
   `coro.free` (`:2862-2870`, `:3613-3621`); the deinit walk (`:3418-3443`,
   including `emit_agg_deinit` for aggregates with heap leaves) exists solely at
   normal exits. The unwind path must reuse the epilogue, not the cleanup block.
5. **The scheduled-flag protocol is a trap.** Abandon an activation with its
   `@clotho.proc_sched` slot left at 1 and it is permanently unwakeable: both
   scans skip a non-zero flag (`emit_static_scan_rt:7657`,
   `emit_dynamic_scan_rt:7699`) and nothing anywhere bulk-resets the array — the
   `$finish` paths reset only ring head/tail. The unwind must clear it exactly as
   the always-loop tail does (`:2923`).
6. **A task's scheduled flag is not in that array.** It is a frame alloca hoisted
   after `coro.begin` (`:3604`, `sched_anchor_ = no_anchor`), so a per-activation
   flag is unreachable from the disabler and meaningless once the frame is freed.
   Only the activation itself can clear it — another reason the unwind must be
   self-driven.
7. **Static wait buckets have no length and are never compacted.** Capacity is a
   compile-time constant, the scan is fixed-trip, and a padded slot is detected
   by a null *flag* pointer (`emit_static_scan_rt:7648-7652`); a non-padded
   bucket has no null check at all. Zeroing a handle without the flag pushes null
   onto the active ring.
8. **`emit_dynamic_remove` matches the region-tagged handle**
   (`parked_handle(b_, hdl_)`, `:7004`), not the bare pointer. An abort-side
   removal built from a bare handle silently never matches.
9. **The dynamic bucket's bound already has an acknowledged hole.**
   `wait_counts` (`:4711-4803`) sizes per intra-body wait plus `nproc` per
   task-waited `(signal, edge)` plus an activation term for fork-reachable
   bodies; `:4052-4057` records that a pended assertion action block can hold N
   slots at once, which no static count bounds and which is unguarded in a
   non-forking design. V2's zombie re-parks land in the same buffer class.
10. **The fork record aliases the parent frame and is reused across fork sites**
    (`fork_record_ty` is `{ i32 count, ptr parent }`,
    `fork_record_slot` memoizes one alloca per coroutine), legal only
    because `join` serialises them. Four distinct failures follow if a child
    outlives its join: site collision on the reused record (parent woken early,
    or count skipping 0 so the parent is never woken at all), use-after-free once
    the parent's frame is released, dangling `ArgDir::Ref` capture formals that
    point into the parent frame (`lower/Statements.cpp, lower_fork` states the lifetime rule
    this relies on), and a double push if two children ever observe `cnt == 0`.
    Note the third failure is a **V2** hazard, not only V3: aborting a *parent*
    with live children invalidates every child's formals.
11. **The fork-child epilogue order is load-bearing.** `deinit_body_strings`
    (`:3667`) runs strictly before the join-count decrement (`:3676-3679`), so a
    child's heap dies before the parent can resume. An abort path that resumes
    the parent earlier reintroduces that race.
12. **Thunk-elided processes have no frame and no handle** (a tagged function
    pointer, `:7992-7995`, discriminated in the drain at `:8129-8141`). A
    targeted group must be forced to a real coroutine — and **no existing path
    does that for a `disable`**: `Disable` and `NamedBlock` are classified
    `blocks_thunk == false` (`Analysis.cpp:188-189` → `none` at `:294`, with
    `:180-181` saying explicitly that landing there leaves an op thunk-eligible),
    so today a V1 `disable` in a suspension-free `always @(...)` still lowers to
    a thunk. The deferred-severity precedent this document pointed at
    (`is_simple_always` returning false on `defer != 0`, `Analysis.cpp:1425-1432`)
    does not generalise. **RESOLVED by V2a**, not by adding either tag to
    `blocks_thunk` (which would deoptimise every V1 user) but by a
    payload-dependent test in `is_simple_always`: a `NamedBlock` with `remote != 0`,
    or any `DisableRemote`, disqualifies the group. The disabler side is forced for
    a second reason — its target id is per-instance, and only a thunk group is ever
    shared across instances.
13. **Per-activation temp cells are safe; the capture ABI is not.**
    `repeat`/`foreach` counters are per-activation frame Locals whenever
    `fn_ctx_ != nullptr` (`make_temp_cell`, `lower/Lowerer.cpp`;
    `docs/TODO.md`), and a fork child always has an `FnCtx`
    (`build_fork_child`), so aborting one activation cannot corrupt a
    sibling's counters. The asymmetry that does bite is Hazard 10's third
    failure: a child's formals point into the *parent's* frame.
14. **Detached records still commit.** `rt_intra_rec` and `nba_arr` records are
    runtime-owned; an `x <= #5 y` armed before the disable will commit after the
    activation dies. LRM leaves this unspecified — take the free behaviour and
    record it, do not pay to suppress it.
15. **Oscillation counter.** Force-wake adds activations per instant
    (`osc_count`, `:7455-7470`); bounded by the number of targeted activations,
    not by a loop, but worth a test at the limit.
16. **A zombie re-suspend inherits an existing refusal.** `emit_suspend`
    (`:3920-3939`) already throws `BackendError` for a suspension inside a task
    called from a `program` block. The zombie tail must not widen that path
    silently.
17. **Zero-cost-when-unused is mandatory. RESOLVED by V2a:** `DesignUses::disable`,
    set only by `DisableRemote` (so V1's lexical form still costs nothing), copied
    to `design_uses_disable_` beside its five siblings, and unguarded by
    `for_process` because V2b targets tasks. Every site added by V2a sits behind it:
    the occupant tables, the force-wake, the poll, the per-arm park record, the
    single-signal wait removal, and the widened overflow-guard gate. Verified rather
    than argued: `--emit llvm` is byte-identical to the pre-disable binary on picorv32
    (4.6 MB, same MD5), on all 15 designs under `examples/`, on the four
    self-contained `tests/perf` cases, and on a design using V1's lexical `disable`.
18. **A force-wake does not cancel the park it interrupts, and that is the hazard
    this document did not have.** The wheel timer, the `#0` batch entry and the wait
    bucket entry all survive the wake, and a second resume lands at whatever suspend
    point the process has reached by then — an early `#delay`, or a resume of a
    completed `initial`'s freed frame. V2a settles it for the two parks it supports
    (a claim on the per-arm `#delay` record; unconditional self-removal for a wait)
    and REFUSES the rest. The trap for V2b: a per-frame record cannot carry the
    claim, because the next arm overwrites the field the stale waker is about to
    read — the record has to be per-park.

## Phased plan

- **V1 — lexical, single-activation target. LANDED.** `Symbol*`-keyed
  named-block stack; `NamedBlock` + `Disable` tags with a label (not a region
  depth — see §IR and lowering shape); `block_targets_` exit stack in the
  backend; a Collect-pass scan labelling the targeted blocks. No runtime, no
  scheduler change, no new global. Everything else fails loud with a
  target-specific message: a task target (including the self-task LRM example 4),
  a remote target, a `disable` in a task / function / fork child, the 16.4.4
  outermost-scope flush, `disable fork`, and `wait fork` — which now names itself
  instead of falling into `lower_stmt`'s generic `default:`.
- **V2a — remote target in a process body. LANDED.** What it took, in the order it
  landed, against what the plan above predicted:
  1. **`DesignUses::disable`**, unguarded by `for_process`, set only by
     `DisableRemote` — so a design using V1's lexical form alone still sets nothing
     and pays nothing. This is the gate everything else sits behind, and it is what
     makes picorv32's emitted LLVM byte-identical (verified: same MD5 over 4.6 MB).
  2. **The overflow-guard gate widened** to `design_forks() || design_uses_disable_`
     (`ring_guard_wanted`). No Σ term was needed after all: an occupant is a process
     already counted in `nproc`, and the wake dedup admits at most one queue entry
     per process, so the existing bound holds.
  3. **Frontend classification.** `scan_disable_targets` now keeps its own
     enclosing-block stack, so it can tell a lexical use from a remote one, and
     records four things per target: the id (allocated from the Module, so a
     multi-top design cannot collide), whether it is named remotely, whether it is
     its own procedure's outermost scope, whether it sits in a fork child, and
     whether its body parks in a way the abort cannot cancel. Target ids stayed a
     bare `uint32_t` — no owner, no instance ordinal — because the id is already
     per-instance.
  4. **`DisableRemote` + `DeferFlush` tags**, `BlockInfo::remote`, and a
     `Printer`/`Analysis` arm each. `Disable` kept `PayloadKind::Label`.
  5. **Force the coroutine** for a group holding either half of the remote form, via
     a payload-dependent test in `is_simple_always` — the deferred-severity
     precedent did not generalise (Hazard 12).
  6. **The occupant tables and the poll** (§Targeting, §Aborting). No epoch, no
     handle array, no zombie tail, no `Wheel.h` change.
  7. **The park claim.** The one thing the plan above did not see: force-waking a
     parked activation leaves its original wake source armed, and a second resume
     lands at whatever suspend point the process has reached by then. The `#delay`
     record became a per-arm heap allocation with a claim field, and a single-signal
     `wait` now compacts itself out of its bucket. Both are gated.
  8. **16.4.4's outermost-scope bump**, as predicted: one `emit_defer_gen_bump()`,
     no new tagging, because the generation counter is already per-process
     per-instance (§Assertion interaction).

  Two steps the plan listed were NOT needed and are dropped rather than carried:
  widening `scan_disable_targets` to subroutine bodies and giving
  `proc_root_block_` a save/restore. Both are still latent, still held shut by the
  `fn_ctx_` refusal, and belong to V2b, which is the phase that lifts it.
- **V2b — the multi-activation target. LANDED.** A task target, blocks in tasks and
  fork children, and the lexical form in a body that can have several activations.
  The activation list replaced the occupant slot for every target kind rather than
  being added beside it, which is a net simplification: three per-target tables and
  the pending mark collapsed into one head pointer. The `#0` case came free once the
  batch park marked itself scheduled, and 16.4.4's remote flush needed only that the
  generation counter be addressable by group.

  The epoch this document proposed for "all activations" never appeared. A list of
  the activations is strictly more useful than a counter that says one of them is
  stale: it also answers "is anything executing the target?" — LRM 9.6.2's own
  precondition — and it is what lets each activation carry its own park to claim.

- **V3 — `disable fork`. LANDED with the `process` class**
  (`docs/design/process-class.md` §8). The walk applies V2's abort per descendant, over
  the control-block tree — which is the enumerable live-descendant structure this
  document correctly said neither it nor `fork-join.md` committed to building. Fork V2's
  record is still a count; the tree is elsewhere, and `disable fork` reads it.
- **V4 — `process` class (LRM 9.7). LANDED; see `docs/design/process-class.md`.**
  Three sentences of this document's sketch were wrong and are worth keeping corrected,
  because each named a real difficulty it had mis-sized:

  `kill()` is not "the abort applied to a dynamic handle". A thread is a CHAIN of
  coroutine frames plus a SUBTREE of descendants: each frame aborts at its own poll and
  wakes its caller on the way out, and the subtree is marked before any of them is
  woken. What V2's abort did supply, and unchanged, is the protocol —
  `emit_node_force_wake` is now SHARED by `lower_disable_list` and by `kill`, since both
  publish the same `disable_node_ty` and both need "force-wake exactly once": claim the
  per-arm `#delay` record, else the scheduled-flag token.

  `suspend()`/`resume()` are not "a desensitise pair". Nothing can be desensitised —
  the wheel cannot cancel and a static bucket cannot compact — so a suspended thread
  stays parked, records the wake it receives, and `resume` replays it.

  The type-level refusal was at `src/lower/Classes.cpp`'s `lower_class_type`, and is now keyed
  by class name: `semaphore` / `mailbox` / `weak_reference` keep it.

  Two things this document owes now. The per-target activation node here and the
  per-thread activation stack there are two ways to find a thread's park, and they
  should converge. And the `hard_park` refusals above are liftable: `kill` demonstrably
  unwinds a thread parked behind a time-consuming task call or a fork, through that
  stack, which is exactly what those refusals said could not be claimed.

## Verification

iverilog supports `disable` (Verilog-2001), so all seven LRM 9.6.2 examples are
directly oracle-checkable against it. Ownership follows the existing harness
split (each TU carries its own file-local `run_capture`/`check_rejected`; a new
TU must be added to the explicit list at `CMakeLists.txt:515-584`).

Landed with V1, all cross-checked against `iverilog -g2012` before being pinned:

- `tests/loop_test.cpp` — the positive set, beside break/continue because a
  lexical `disable` IS the language's only labelled exit: examples 1 and 2
  (resume after the block), example 3 as `continue` (a loop-body block, with the
  `for` step still running), example 5 as a multi-level `break` (two loops and an
  `if` at once, plus a second process proving only one activation ended), an exit
  out of a `case` arm and a `while` together, disabling the whole process body,
  the `always` re-arm that makes example 7's mechanism work, `%m` still naming
  the enclosing blocks, the IR shape (`NamedBlock` + `Disable` emitted), and —
  replacing the two refusals V1 pinned there — the V2a behaviour below.
- `tests/task_test.cpp` — the task target refused from inside the task
  (example 4) and from another process, and a block target inside a task or a
  fork child.
- `tests/fork_test.cpp` — `disable fork` and `wait fork`, each by its own name.
- `tests/recovery_test.cpp`, `tests/sourcemap_test.cpp` — these used a lexical
  `disable` as their canonical unsupported construct and now use `disable fork`;
  V1 made the old exerciser legal.

Landed with V2a, in `tests/loop_test.cpp`, each cross-checked against
`iverilog -g2012` before being pinned (except the 16.4.4 case, which has no local
oracle — iverilog does not implement deferred assertions):

- **A sibling-block target is a no-op**, which is also LRM 9.6.2's "if the block is
  not currently executing, the disable has no effect": a process is never inside two
  of its own sibling blocks at once.
- **A cross-process target ends that activation now** (example 3's shape): an
  `always begin : always1 #5 … end` disabled at t=7 while parked on the `#5` armed
  at t=5 re-arms instead, so its next completion is t=12 rather than t=10. This is
  the case that pins the force-wake and the poll together.
- **A disabled activation is not resumed by the timer it abandoned** — the target is
  parked on a `#50` at the disable, and the two delays it executes AFTERWARDS have to
  land at their own times, not at the stale timer's. This is the regression test for
  Hazard 18, and it fails loudly (early wake) without the park claim.
- **Two instances of one module, disabled independently** — `disable u1.blk` must not
  touch `u2`'s copy. Passes because the target id is per-instance.
- **The 16.4.4 outermost-scope flush drops the report**, plus the nested-block
  counterpart that must still lower AND still report, which is a different rule
  rather than a weaker case of the same one.
- **The three remaining remote refusals**, each by its own message: a park the abort
  cannot cancel (a task call, and a `#0`), and a target inside a fork child.

Also landed: `tests/sva_test.cpp`'s `monitor_shape` digest now hashes tag NAMES
rather than tag ordinals. Inserting `DisableRemote` mid-enum renumbered every later
tag and broke both digests without moving a single emitted instruction — the
instruction counts (125, 117, 18) were unchanged, which is the claim those cases
actually make. `ir::tag_name` is exposed in `Printer.h` for it.

Landed with V2b, in `tests/task_test.cpp` and `tests/loop_test.cpp`:

- **A task target**, with its caller resumed after the task-enabling statement; the
  **self-disable** of 9.6.2 example 4; **two concurrent activations** ended by one
  statement; and a task that is **not running** (no effect).
- **A block inside a task**, with every activation inside it ended, and the same
  block used as a **labelled break from inside** the task.
- **A block inside a fork child**, disabled from another process and from inside the
  child, with the `join` completing when the child ends.
- **A `#0` inside a target**, in the two-`#0`s-plus-`#0`-disabler shape that
  double-woke before the batch park marked itself scheduled.
- **A remote 16.4.4 flush**, with the un-disabled counterpart proving the report
  would otherwise have matured.
- **An assertion action block as the disabler.**

Owed by whoever takes the rest:

- `tests/fork_test.cpp` — example 6 (`@reset disable event_expr` inside a `fork`),
  which needs `join_any`/`join_none` before it means anything.
- **A fork-free design under a shrunk ring cap.** V2 widened the guard's gate so
  such a design now emits the check at all, but there is still no cap knob in
  `SimulationConfig` to shrink, so the test `docs/design/fork-join.md` also owes
  remains unwritten.
- The parks still refused, once a target can end a nested activation too.

Regression gate, run at every step: the full suite (1781 cases) green, and `--emit llvm`
byte-identical to the pre-V2a binary on picorv32, every design under `examples/`,
the self-contained `tests/perf` cases, and a design using V1's lexical `disable`.

The three unspecified 9.6.2 effects (output/inout results, already-scheduled
NBAs, procedural continuous assignments), the refusal to model the undefined
`disable`-from-within-a-function case, and the 16.4.4 drop reading above are the
LRM-interpretation decisions this design rests on.
