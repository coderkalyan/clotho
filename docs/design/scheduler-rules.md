# Scheduler rules, speedups and event-bug postmortem

The event rules of IEEE 1800 that Clotho's backend relies on, the speedups that
implement or bypass each one, and every event-scheduling bug fixed since
2026-06, mapped onto both. Event bugs keep shipping from the backend's speedups,
and few were found by the suite itself (§4). This document says where checks
have to look.

Status: §3 is normative, like `kernel.md` and `architecture.md`: it is the rule
list that the checks of §1 steps 2 to 6 are written against, and code that
disagrees with a rule is a bug unless the rule changes first. **Not yet** marks
a rule the code does not follow today; delete the marker when it does. §2, §4
and §5 are descriptive, as of commit 26fade75.

Every rule's wording is **not yet checked against the LRM text**: no copy of the
LRM was available when this was written. Each rule cites the clause as code,
`docs/LRM_NOTES.md` or `docs/design/` already cite it. Tests are cited as file,
then `TEST_CASE` name.

Keeping this current:

- A new scheduling speedup gets a row in §2: where it is, its gate, its as-if
  argument. A new rule gets a row in §3 with its pinning tests.
- A fix of an event bug gets a row in §4 and updates its cell in §5.

## 1. Plan

Decided by the maintainer, recorded 2026-11-24:

- **Backend first.** The speedups in `src/ir/llvm/` and `src/runtime/` are the
  target. Frontend lowering is out of scope, so bugs in sensitivity inference
  and in the shape of event controls (B4, B6,
  B31) are out of reach of every step below.
- **Reference semantics are defined over Clotho IR**, not over SystemVerilog
  source.
- **The reference scheduler is written in Rust** and shares no code with the
  backend.
- **Order of work.** Each step catches:

| Step | What | Catches |
|---|---|---|
| 1 | Rule catalog and postmortem (this document) | Nothing by itself: it names the rule × speedup cells, so that reviews and tests aim at them. |
| 2 | Config differential: every test design run with each speedup gate off | A gated speedup that changes the output of an order-independent design (B15, B25, B43). It cannot see the ungated speedups (§2.2). |
| 3 | Scheduler trace and a checker of the rules in §3 | Violations visible in the trace of any design the suite runs, with no expected output: a resume of an activation that is not at its wait (B51), a double resume (B13, B28), NBA updates out of creation order (B38, B40), a missing wake (B7, B16). |
| 4 | Rust reference scheduler over Clotho IR, enumerating LRM-legal orders (legal-set oracle) | Outputs outside the legal set where no external oracle exists: iverilog has no `->>`, applies event-controlled updates in Active, and merges updates of some target shapes. It also finds tests whose expected output rests on an order 4.7 leaves open. |
| 5 | Event generator | Combinations no hand-written test has, weighted to the cells of §5: `disable` × inline fork or task, `$finish` × virtual call, force × net delay, virtual interface × twin instances. |
| 6 | Mutation testing of the speedups | Speedups whose breakage no test notices: a dropped flag hold, a skipped wake bracket, a reordered commit path. These are the empty cells of §5.2. |

Step 2 reuses what exists. Tests vary gates through `run_capture`:

- `thunk`, `fuse` and `regions`: in `comb_fusion_test.cpp`, `region_test.cpp` and
  `llvm_backend_test.cpp`.
- `dedup`, `split` and `z_elision`: once each, in `llvm_backend_test.cpp`.
- `comb_fuse` and `o3`: never set in a test (grep of `tests/`).

`corpus.md` (proposed, not started) plans a runner over every case.

## 2. Speedup inventory

A literal LRM 4.5 scheduler does the following:

- It keeps an event queue per region.
- Each update event schedules the evaluation of every process sensitive to it.
- A process registers its event control each time it reaches it.
- NBA updates are performed one by one, in creation order.

Each mechanism below changes how that is done. "None" in the gate column means
the speedup cannot be turned off for a differential run.

### 2.1 Mechanisms on main

| ID | Mechanism | Where | Gate | As-if argument (and what it relies on) |
|---|---|---|---|---|
| S1 | **Static subscription.** When an `always` has a leading event control, the frontend hoists it as the process's list. While no park of the body arms the wake word (`TagInfo::arms_wake`: an event control or `wait`, `expect`, a sync queue, `wait fork`, `await`), the seed subscribes the process once into per-(signal, edge) static buckets, and its body runs body-first in a forever loop. A body that has such a park waits for the list at the top of its loop instead, as a dynamic waiter, and the seed starts it to register at its first run (B51). | `RootTables.cpp:emit_seed`; `Events.cpp:plan_static_subscribe`, `build_wait_queues`, `lower_wait_edges`; `Groups.cpp:subscribed_statically`, `waits_at_head`; `Subroutines.cpp:lower_group`; `Analysis.cpp:tag_info`; `lower/Statements.cpp:lower_procedural_block` | none | A bucket entry is the event control, evaluated once for the whole run. It must be inert whenever the process is not waiting at that control, and S2 makes it inert only while the word is held set, from the wake to the re-arm store of the loop's own park. A park that arms the word makes the entry live there and it would resume the process at that park, so a body that has one is not subscribed. The cost is a registration per activation, on processes that are coroutines already. |
| S2 | **Wake word as one-wake token.** A waker pushes only if the word is 0, and sets it. The word stays set from the wake to the re-arm store before the next park, the loop's own or an intra-body one's (`emit_arm_wake_word`). Parks that a second waker can reach hold it too: `#0`, join, inline fork or task, and `#delay` under S16. Ring and bucket capacities are sized from this one-entry-per-process bound. | `Events.cpp:emit_static_scan_rt`, `emit_dynamic_scan_rt`, `emit_event_wake_list`, `lower_wait`, `emit_arm_wake_word`, `wait_counts`; `Subroutines.cpp:lower_group`; `Groups.cpp:emit_sched_entries`; `Processes.cpp:emit_node_force_wake` | none | A queued or running process is not at an event control, so a further event has nothing to wake (9.4.2). The bound holds only while every park is accounted for (B11), and while no static entry is live at a park that arms the word (S1). |
| S3 | **Topological subscription order and FIFO ring.** Bucket entries are in topological order of the blocking-write wake graph and are popped FIFO, so producers run before consumers. | `Analyses.cpp:compute_subscription_order`; `RootTables.cpp:emit_seed`; `Scheduler.cpp:emit_active_push`, `emit_active_run` | none | 4.7 leaves active order open, so the order affects performance only. |
| S4 | **Table-driven seed and time-zero runs.** One row per schedule group, in creation order. `self_start` groups (continuous assigns, port copies, always_comb, event nets) are queued with their word preset to 1 plus every trigger bit. Buckets and persistent event nodes are filled before the drain. An always that waits at its head (S1) is queued ahead of every other group and registers at its first run, so it listens before any other body runs, as a static subscription does. PreInit stores are silent. | `RootTables.cpp:emit_seed` (`clotho.seed.rows`, `.slots`, `.late`, `.events`) | none | Time-zero evaluation (10.3, 4.9.1, 9.2.2.2). The preset word stops a time-zero store from queuing a second entry (B3). Nothing scans before the drain, so fill order does not matter. |
| S5 | **Deferred always_comb start.** A schedule group with an always_comb or always_latch member is queued after every other body. A continuous assign fused with one starts with it. | `RootTables.cpp:emit_seed` (`starts_late`); `Module.h` `Process::starts_after_procedures` | none | 9.2.2.2. 4.9.1 and 4.7 leave a continuous assign's time-zero order open (LRM_NOTES 44). |
| S6 | **Thunk.** A suspension-free `always` becomes a plain function, called through a tagged pointer. | `Groups.cpp:compute_fusion_groups` (`thunkable`); `Subroutines.cpp:lower_group`; `Scheduler.cpp:emit_active_run` | `thunk` | Same wake-word timeline and FIFO order as a coroutine (SimConfig.h). |
| S7 | **V1 fusion.** Processes with identical canonical sensitivity run as one body. | `Groups.cpp:compute_fusion_groups`; `Analysis.cpp:is_simple_always` | `fuse` | Each member's wake set equals the group's; the member order is one 4.7 order; no member blocking-writes its own sensitivity. |
| S8 | **Comb fusion.** Dependent pure comb processes run producer-before-consumer on the union sensitivity. | `Groups.cpp:compute_fusion_groups`; `Analysis.cpp:comb_fuse_verdict` | `comb_fuse` (needs `fuse`) | A spurious member run is unobservable: the body is pure, its reads are within its sensitivity, its writes are change-detected. A collapsed delta wave is a legal 4.7 schedule (comb-fusion.md). |
| S9 | **Member guards.** The wake word carries trigger bits, so each member of a heterogeneous group runs only if one of its own events fired. | `Groups.cpp:compute_trigger_masks`, `emit_trigger_failsafe`; `Subroutines.cpp:lower_group`; `Events.cpp:emit_static_scan_rt` | `regions` | Guards only remove runs that the union subscription added. A wake that cannot name its trigger sets every bit (regions.md §3.3, §3.4). |
| S10 | **Schedule groups.** Thunk groups of several instances that wait on one (signal, edge) set share one subscription, wake word and dispatcher. | `Groups.cpp:build_sched_groups`, `emit_sched_entries` | none (exists only with `thunk` on) | No row writes a signal of the shared set, so no row wakes another. Rows run in scheduled-rank order (comment at `build_sched_groups`; comb-fusion.md, "Scope"). |
| S11 | **Code shared across instances.** A unit's groups and subroutines are lowered once and run with each instance's record, which carries wake words, buckets, masks and ports. One module per unit. | `Backend.cpp:run`; `Families.cpp:lower_subroutines`; `RootTables.cpp:fill_instance_records`; `Dedup.cpp:verify_shared_group` | `dedup`, `split` | The shared code is each instance's own lowering up to symbols. A taint list keeps instance facts out. Checked by `verify_shared_code` and twin mode (VERIFICATION.md). |
| S12 | **Run-once regions.** Preponed, Observed and Re-NBA processes are in no queue. Preponed runs at the top of a slot, Observed at each settle, Re-NBA in its region. | `RootTables.cpp:emit_run_once`; `Scheduler.cpp:emit_active_run` | none | These bodies are timing-free. A second Observed run in a slot finds no clocking event and does nothing (comment at `observed.settled`); the same is assumed for Re-NBA drives [INFERENCE]. |
| S13 | **Region elision.** A region's code exists only if some construct can fill it: `has_nba` (including `->>`), `has_inactive`, the reactive set, Observed, `$finish`, VCD, `$strobe`, `$monitor`. | `Scheduler.cpp:emit_active_run`; `Stores.cpp:emit_nba_any_staged` | none | An empty region is a no-op. Relies on each predicate covering every source (B20, B18). |
| S14 | **Reactive continuous ring.** A program's port copies, assigns and resolvers drain before program code. | `Scheduler.cpp:emit_active_run`, `ensure_reactive_cont_queue`; `RootTables.cpp:emit_seed` | none | 4.7 leaves the order inside Reactive open (LRM_NOTES 50). |
| S15 | **Inline runs.** `join` and `join_any` children run at the spawn, on the parent's stack. A task call resumes its callee inline. | `Processes.cpp:lower_fork_inst`; `Subroutines.cpp:lower_task_call_inst` | none | The parent is about to block on the children (9.3.2, per the comment in `lower_fork_inst`), and a task call is synchronous. The parent is running, so its word must read "queued" (B28, B32). |
| S16 | **Claim gating.** Park claims, the flag hold at a `#delay` park and the ring-overflow checks are emitted only in a design that uses `disable` or process control. | `Scheduler.cpp:park_claim_wanted`, `ring_guard_wanted`; `Events.cpp:emit_delay_timer` | none | Without a second waker, the one wake a park expects is the only one. |
| S17 | **Cooperative `$finish`.** The finishing flag is tested on each drain's queue-empty edge, and after may-finish call sites only. NBAs already staged are left uncommitted. | `Analyses.cpp:compute_may_finish`; `Scheduler.cpp:emit_active_run`; `runtime/Scheduler.cpp:rt_finish_coop` | none | 20.2. A call that cannot reach `$finish` needs no test (cooperative-finish.md). |
| S18 | **Store-site wake service.** Every write path brackets its store inline: snapshot, edge facts, bucket scans, trigger fire, class-property notifiers. There is no update-event queue. PreInit and Preponed stores are silent. | `Events.cpp:service_snapshot`, `service_after_write`, `service_wait_queue`, `service_change_notify`, `emit_edge_facts`; `Stores.cpp:emit_block_commit_body` | none | The update happens at the store, so its wake can too, and Table 9-2 is computed there. Relies on every write path being bracketed (B7, B16, B12). |
| S19 | **Wake-service skipping.** No snapshot or scan for a signal that no waiter kind watches. Only waited edges get a bucket; the z snapshot is taken only for level waiters; a wide store snapshots only the words it touches. | `Events.cpp:signal_has_waiters`, `signal_has_bucket`, `signal_has_level_waiters`, `wide_window_snapshot` | none | Nothing can observe the skipped work. Relies on the waiter predicate covering every waiter kind (B8). |
| S20 | **z elision.** A signal that may-z proves never z loses its z word. | `StateLayout.cpp:build_state_types`; `Analysis.cpp` may-z | `z_elision` | x↔z changes cannot occur on such a signal. |
| S21 | **Continuous assigns and nets as processes.** A continuous assign, port copy, gate-input temp net, net resolver and expression event net is each a process woken by its operands. | `lower/Nets.cpp:add_continuous_process`, `continuous_events`, `emit_wire_resolvers`, `release_net`; `lower/Timing.cpp:synth_event_net` | none (frontend shape) | The net's update is a later Active event of the same slot (4.7). Force and release must bypass it (B10, B26, B33). |
| S22 | **NBA block commit.** A scalar target keeps one staged value per NBA pass. Each straight-line block has one enqueue and one commit function, and the commit wakes once, on old != final. | `Stores.cpp:lower_store_nb`, `block_register`, `emit_block_commit_body`, `finalize_block_nba` | none | Holds only if no waiter observes an intermediate update. False for edge waiters (B48; the fix is unmerged). |
| S23 | **Fixed order of NBA staging paths.** Commit order is: block chain; array, aggregate, container and string record queues; intra ready lists; then `->>`. | `Stores.cpp:emit_nba_commit`, `emit_intra_nba_ordered` | none | A signal takes one path, or its plain NBAs are also copied onto its ready list (B38). Zero delays take the plain path (B9, B22). |
| S24 | **Timed-update records.** `<= #d` and `<= @e` updates are records on per-site ready lists, batched by release moment. The commit applies a site's records and wakes once. | `Stores.cpp:emit_intra_nba_arm`, `emit_intra_event_arm`, `emit_intra_commit`; `Events.cpp:emit_release_moment`; `runtime/Scheduler.cpp:intra_ready_push`, `rt_intra_moment`, `rt_intra_now`, `rt_intra_release` | none | The commit order is the order the updates were created (LRM_NOTES 51). One wake per site has the same hazard as S22 (B49). |
| S25 | **`->>` pend chain.** Nonblocking triggers queue on one chain, drained in the NBA region in execution order. A `->> #d` expiry pends onto that chain. | `Events.cpp:emit_event_pend`, `emit_event_pend_drain`, `event_delay_thunk` | none | 15.5.1. |
| S26 | **Inertial commit at the top of the slot.** A continuous assign's delayed update, fired by the wheel, commits before the slot's Active drain. An epoch cancels superseded updates. | `Stores.cpp:emit_inertial_arm`; `Scheduler.cpp:emit_active_run` (`wheel.posttick`); `runtime/Scheduler.cpp:rt_inertial_decide` | none | 10.3.3: the update is an Active-region event (LRM_NOTES 9). |
| S27 | **Event objects.** Waiter lists are push-front. `always @(e)` nodes are linked once, by the seed. `.triggered` is `stamp == now`. A monomorphic event's object is a constant. | `Events.cpp:emit_event_link`, `emit_event_wake_list`, `lower_event_triggered`, `analyze_event_monomorphism` | none | 4.7 leaves wake order open; 15.5.3 makes `triggered` last for the slot. |
| S28 | **Timing wheel.** Timers due in one tick fire in list order, last armed first (679e3ce2), all before that slot's Active drain. | `runtime/Wheel.h:advance`; `runtime/Scheduler.cpp:rt_wheel_advance` | none | 4.7. |
| S29 | **Published report generation.** The running thread's deferred-report generation counter is published through one global: a process group stores it at each activation and after each park, a task reads it in its ramp and republishes it after its parks, a fork child takes a pooled cell. Bodies that cannot reach a unique/priority report or a waiting task in such a design touch none of it. | `SystemTasks.cpp:gen_global`, `body_gen`; `Scheduler.cpp:emit_suspend`; `Subroutines.cpp:lower_group`, `lower_task`; `Processes.cpp:lower_fork_inst`; `Analysis.cpp` (`fn_gen`, `gen_counter`); `runtime/Reports.cpp:rt_gen_cell_alloc` | none | A body runs in its caller's thread, so what it reports and flushes belongs to that thread (16.4.2). Relies on every park republishing (`emit_suspend`) and on a runtime-ramped body publishing null. |

`o3` (the LLVM pipeline), `gc_stress` (collection cadence) and
`verify_shared_code` (a check) are configuration axes for step 2, but do not
change scheduling.

### 2.2 Ungated

23 of the 29 mechanisms cannot be turned off: S1, S2, S3, S4, S5, S10, S12, S13, S14, S15, S16, S17, S18, S19, S21, S22, S23, S24, S25, S26, S27, S28, S29. The
other 6 have a gate: S6, S7, S8, S9, S11, S20. A bug in an ungated speedup is
invisible to step 2. Of the 51 rows of §4, 42 involve an
ungated speedup, 2 only gated ones, and 7 none (code outside
this list: frontend lowering, the sync queues, process control). Only 3 rows (B15, B25, B43) would have
changed output with a gate off [INFERENCE: read from each fix, not run].

### 2.3 Unmerged

| ID | Mechanism | Where | Gate |
|---|---|---|---|
| U1 | Self-only store elision: a store whose only static waiter is the storing, suspension-free, unguarded group skips its wake service. | branches `self-wake` and `rank-order` at 1df9abbc | `self_wake_elision` |
| U2 | A variable named by one group lives in an alloca for the activation. | same | `privatize` |
| U3 | Ready bitmap dispatched by rank. | uncommitted in the `rank-order` worktree (`docs/design/rank-order.md`) | `rank_order` |

`nba-glitch` adds two ungated fix mechanisms: per-update spill records for
observed scalars (B48), and in-place calls of event nets from
operand scans (B50).

## 3. Rule catalog

Wording is not yet checked against the LRM text.

| ID | Rule | Clause as cited | Speedups | Pinned by |
|---|---|---|---|---|
| R1 | A time slot runs Preponed, Active, Inactive, NBA, Observed, Reactive, Re-Inactive, Re-NBA and Postponed. A region that gets work sends control back to the first non-empty one, and time advances only when every region is empty. | 4.4, 4.4.2.1 to 4.4.2.8 (as the code numbers them), 4.5 | S12, S13, S14, S17, S26 | `clocking_test.cpp` "a @(cb) waiter runs after all Active work of the time slot"; `program_test.cpp` "program initial runs in the Reactive region, after all Active work" |
| R2 | A delay control resumes the process at t+d, never earlier. `#0` resumes in the Inactive region of the same slot, after Active empties and before NBA, and every such process becomes active together. A delay with an x or z bit is zero. | 4.4.2.3, 9.4.1; LRM_NOTES 39 | S13, S28 | `gate_test.cpp` "gates settle before a #0 resumption observes them"; `event_test.cpp` "->> #0 e triggers in the current slot's NBA region"; `llvm_backend_test.cpp` "a delay with an x or z bit is a zero delay" |
| R3 | Events in one region run in any order, and processes woken by one event resume in any order. Statements of one begin-end still run in order. | 4.7 | S3, S6, S7, S8, S9, S10, S14, S15, S27, S28 | `comb_fusion_test.cpp` "comb fusion: diamond reconvergence settles to final values"; `region_test.cpp` "regions: unequal-depth reconvergence settles to final values" |
| R4 | Preponed samples before any process of the slot, and Observed runs after Active and NBA settle. `$strobe`, `$monitor`, VCD and final deferred reports read settled values in Postponed. | 4.4.2.1, 4.4.2.5, 16.4, 21.2.2 | S12, S13 | `case_test.cpp` "priority case: Observed-region report is stamped with the settled instant"; `severity_test.cpp` "deferred final assertion: report matures in the Postponed region"; `strobe_test.cpp` "$monitor prints once per change" |
| R5 | A process waits only from the moment it reaches an event control. It resumes once however many listed events occur, and only on a listed event. | 9.4.2 | S1, S2, S4, S6, S7, S8, S9, S10, S11, S15, S16 | `llvm_backend_test.cpp` "bare initial @(event) fires once when the edge arrives"; `region_test.cpp` "regions: the wake word keeps its queued/not-queued discipline"; `comb_fusion_test.cpp` "comb fusion: narrow explicit sensitivity keeps stale reads"; `event_test.cpp` "an always parked at an intra-body @ is not woken by its own leading control" and the cases after it |
| R6 | Every change of a variable or net, by any write path, wakes its waiters. A write that changes nothing wakes nobody. A write through a `ref` formal wakes at the write (the call passes a wake thunk). | 9.4.2, 13.5.2 | S6, S9, S10, S11, S18, S19, S21, S22, S23 | `llvm_backend_test.cpp` "$sscanf / $sformat / $fgets / scalar $fread wake waiting processes", "a fused group lowered after a declaration initializer still wakes its readers"; `comb_fusion_test.cpp` "comb fusion: external observer wakes once per real change"; `case_test.cpp` "ref write: ..." |
| R7 | Only bit 0 makes an edge. 0→x, 0→z, 0→1, x→1 and z→1 are posedges, the mirrors are negedges, and `edge` is either. x↔z changes the value but is no edge. | 9.4.2, Table 9-2 | S18, S20, S21 | `llvm_backend_test.cpp` "posedge/negedge classify x/z transitions per LRM Table 9-2", "the edge keyword fires on posedge or negedge, not x<->z", "NBA commits classify x/z edges and z-only changes the same way" |
| R8 | Each update in a slot is its own event, on its own transition, whether it comes from a blocking store or an NBA. A change that reverts within the slot is still a posedge and a negedge, and `repeat(n) @` counts each occurrence. **Not yet** for NBAs (fix on `nba-glitch`, §4). | 4.6 b), 9.4.2, 9.4.5 | S8, S9, S22, S23, S24 | `region_test.cpp` "regions: a signal stored twice in one instant keeps its trigger" (blocking only) |
| R9 | `@(expr)` over a select, member or operator waits for a change of the expression's value, re-evaluated at each update of an operand. **Not yet:** it is evaluated once after the operands settle (fix on `nba-glitch`, §4). | 9.4; 15.5.3 as `nba-update-order.md` on `nba-glitch` cites it (§6) | S21 | `event_test.cpp` "event control on struct members (edge and level)" (no glitch pin on main) |
| R10 | `wait(c)` does not suspend if `c` is already true; otherwise it resumes once `c` becomes true. x is false. | 9.4.3 | S2, S18, S19 | `wait_test.cpp` "wait on an already-true condition does not suspend", "wait treats X as false and blocks until a definite 1" |
| R11 | An intra-assignment control samples the right-hand side and selects when the statement executes. A `<= #d` or `<= @e` update is created when the delay elapses or the event occurs, and `repeat(n)` counts occurrences. | 9.4.5, 4.9.4 | S24, S27 | `intra_assign_test.cpp` "blocking intra-assignment repeat-event waits N edges then stores the sample", "nonblocking intra-delay repeat-event stores after N edges" |
| R12 | An NBA samples its RHS when it executes. NBA updates are performed in creation order: a plain NBA's update is created when it executes, a timed one's when it is released. A zero delay is a plain NBA. | 4.6 b), 10.4.2, 4.9.4; LRM_NOTES 51 (a plain NBA between an arm and its occurrence is undecided) | S22, S23, S24, S25 | `intra_assign_test.cpp` "nonblocking intra-delay #0 orders with plain NBAs by execution order", "a plain NBA executed after the trigger commits after an event-controlled update", "a plain NBA between two releases commits between their updates" |
| R13 | NBA commits wake processes into the Active region of the same slot, and the slot ends only when every region is empty. | 4.5 | S13, S22 | `gate_test.cpp` "an NBA commit pushes gate fanout in the same instant"; `llvm_backend_test.cpp` "combinational read port wakes after a nonblocking memory write"; `struct_test.cpp` "combinational reader re-evaluates after a struct-member NBA commit" |
| R14 | A force or procedural assign overrides from its statement on. An NBA is checked against it when it commits. Releasing a net assigns the driven value at once, and a driver change under a force gives readers no event. | 10.6.1, 10.6.2, 4.9.2; LRM_NOTES 49 | S18, S21, S22, S26 | `force_release_test.cpp` "force: suppresses an NBA staged before the force", "release of a net takes the driven value at once" |
| R15 | A continuous assign is evaluated at time zero and again when an RHS operand changes (operands only, not reads inside a called function). Port connections and gate-input expressions are implicit continuous assigns. | 10.3, 10.3.2, 4.9.1, 4.9.6, 23.3.3 | S1, S4, S8, S21 | `llvm_backend_test.cpp` "declaration initializers propagate to continuous consumers at time zero"; `function_test.cpp` "continuous assign is not sensitive to signals read inside a function" |
| R16 | A delayed continuous assign or net delay is inertial: the update lands in the Active region at t+d, and a newer change cancels a pending one. | 10.3.3, 28.16; LRM_NOTES 9 | S21, S26 | `continuous_assign_delay_test.cpp` "inertial CA delay: pulse rejection and no-restart on same-value change"; `net_delay_test.cpp` "net delay: a release returns the net to its drivers at once" |
| R17 | always_comb and always_latch run once at time zero, after every initial and always has started. They are sensitive to their reads, including reads in called functions, minus their own writes. `@*` sees only function arguments and keeps read+written signals. | 9.2.2.2, 9.2.2.2.1, 9.2.2.2.2, 9.2.2.3, 9.4.2.2; LRM_NOTES 44 | S1, S4, S5, S8, S10 | `comb_fusion_test.cpp` "always_comb and always_latch run once at time zero after every initial has started"; `function_test.cpp` "always_comb sensitivity includes signals read inside a function"; `llvm_backend_test.cpp` "always @* re-triggers on a read+written intermediate" |
| R18 | Every initial and always starts at time zero. An always's leading event control is a wait, not a run, an `always` with no event control loops, and a leading control of an initial blocks. | 9.2.1, 9.2.2, 9.4 | S1, S4, S6 | `llvm_backend_test.cpp` "bare initial @(event) blocks; it does not run at time zero" |
| R19 | `-> e` wakes the processes waiting at that moment. `e.triggered` is true from the trigger to the end of the slot. | 15.5.1, 15.5.3 | S24, S27 | `event_test.cpp` "always @(e) re-runs on every trigger", "e.triggered persists within the triggering step only", "wait(e.triggered) catches a same-step trigger" |
| R20 | `->> e` triggers in the NBA region, and `->> #d e` in the NBA region of the expiry slot. Several triggers fire in execution order. | 15.5.1, 4.6 b) | S13, S25 | `event_test.cpp` "->> #d e wakes its waiter in the NBA region of the expiry instant"; `intra_assign_test.cpp` "nonblocking event triggers occur in the order they executed" |
| R21 | Assigning an event merges the two objects. A control that lists two names of one object counts one occurrence. | 15.5.5, 15.5.5.1 | S24, S27 | `event_test.cpp` "event assignment merges the two handles onto one object"; `intra_assign_test.cpp` "a control that lists two names of one event counts it once" |
| R22 | `join` waits for every child and `join_any` for the first. `join_none` children start only when the parent blocks or ends. | 9.3.2 | S2, S15 | `fork_test.cpp` "fork: join_none does not run its children at the fork", "fork: join_any does not run the siblings it is not waiting for" |
| R23 | `disable` and `kill` end an activity once, at the moment they run, and end the children forked inside the target. An ended activation is never resumed by the wake it was parked on, and an aborted waiter hands its wake to the next. | 9.6.1, 9.6.2, 9.6.3, 9.7; 15.3, 15.4 (queues) | S2, S15, S16 | `loop_test.cpp` "a disable that runs after its target's timer expired wakes the target once", "a fork child that disables its parent's block before it suspends"; `process_test.cpp` "process: kill a parent parked at a join"; `sync_test.cpp` "semaphore: killing the queue head hands off to the next waiter" |
| R24 | After `$finish`, nothing more of the finishing thread runs, and neither does any process it woke. `final` blocks run. | 20.2, 9.2.3 | S11, S17 | `llvm_backend_test.cpp` "$finish inside a function leaves nothing queued to run after it"; `class_test.cpp` "a $finish reached through a virtual call stops every caller" |
| R25 | Program code and program continuous assigns run in Reactive. Clotho settles port copies before program code reads them. | 24.3.1, 24.3.2, 4.7; LRM_NOTES 50 | S3, S4, S14 | `program_test.cpp` "a program initial reads an input port settled at time zero", "a port copy woken behind a queued program thread still settles before it reads" |
| R26 | A pending deferred report, or unique/priority violation report, of a thread flushes when the thread starts an activation, resumes from an event control or wait (inside a task it called too), or when its outermost scope is disabled. A fork child is a thread of its own. `$monitor` prints when an argument other than a time function changes. | 12.4.2.1, 12.5.3.1, 16.4.2, 16.4.4, 21.2.2; LRM_NOTES 18, 22, 54 | S11, S12, S13, S29 | `severity_test.cpp` "deferred assertion: resuming from an event control or a wait statement flushes the report", "deferred assertion: a wait inside a called task flushes the calling process's report"; `case_test.cpp` "unique / priority report: ..."; `strobe_test.cpp` "$monitor ignores a changing $time argument" |

## 4. Postmortem

Scope:

- Every event-scheduling fix in `git log --since=2026-06-01` on main. That is
  the whole history: the first commit is 2026-06-22.
- `main..nba-glitch` (base bbe3b9a1). Its three fixes are reported in their commit
  messages against that base; main HEAD was not probed for them [INFERENCE that
  they are still open there]. iverilog 13 agrees with the reading for a scalar:
  `a <= 1; a <= 0;` from 0 gave one posedge and one negedge (probed).
- `force-nba-commit`, which is merged, so its commits are B47.
- `self-wake` and `rank-order`, which both point at 1df9abbc. Their two commits
  add speedups (U1, U2), not fixes.

Excluded, with examples:

- Features and lifted refusals (71d7c373, 310dea62, ca3c46a4).
- Crashes and value bugs outside scheduling (9c59953c, f5a710af, d1c89159,
  0033a309, 09625763) and GC rooting of NBA records (bbf846a8, 73a1931e, 60fbc91e).
- Performance changes (3831da15, 1af2a431, b1c7d4bd, 5802ee74, 8e2d830f) and a
  verifier failure at compile time (bfe250e2).

"Found by" quotes the `Found-by:` trailer where there is one (trailers start on
2026-10-28), or the commit message, marked (msg). "Why tests missed it" is
inferred from the fix's test and the suite of the time, except where marked
(msg). A row groups the commits of one defect or series. Rows are ordered by
their earliest commit, then the unmerged ones, then the open one.

| ID | Commit(s) | Rule | Speedup | Found by | Why tests missed it |
|---|---|---|---|---|---|
| B1 | 7a618080, 96cf3c9b | R18 | — (none in §2) | not recorded | A comment claimed a guard that did not exist (msg). |
| B2 | 9dd0a794 | R15, R17 | S1, S4 | picorv32 bring-up (msg) | Every continuous consumer in the suite had a runtime-driven source; PreInit stores are silent. [INFERENCE] |
| B3 | 1e7d0605, 3116dff7 | R5 | S2, S4 | not recorded | Needs duplicate queue entries to overflow the one-slot-per-process ring (msg). |
| B4 | de2141ad, 7c7ee3f1, cfbb24cf | R15, R17 | — (none in §2) | not recorded | Inference was tested on always_comb shapes; no task-body write, read+written `@*` intermediate or function call in an assign. [INFERENCE] |
| B5 | af33ff03 | R7 | S18, S20 | not recorded | Edge tests used the values 0 and 1 only. [INFERENCE] |
| B6 | c9c95865 | R18 | S1 | not recorded | No test of `initial @(e)`. [INFERENCE] |
| B7 | 806f0a89 | R6 | S18 | not recorded | The file-I/O tests printed their targets; none waited on one. [INFERENCE] |
| B8 | 3c8401ee | R6 | S19 | not recorded | Gate outputs always had process waiters and no z-initialized wire was tested. [INFERENCE] The gate worklist is gone (4b5b456c). |
| B9 | 3577359f | R12 | S23 | not recorded | No `<= #0` was mixed with a plain NBA to one target. [INFERENCE] |
| B10 | 5348d700 | R14 | S21 | sv-tests ivtest force1, force2 (msg) | No read in the slot of the force. [INFERENCE] |
| B11 | ed1754b8 | R22 | S2 | not recorded | Forks of three children worked; four overflowed a wait bucket (msg). |
| B12 | ecc424a0, 03bc7ae3, 0f955e53 | R6, R12 | S18, S22, S23 | the LRM 25.9 example (ecc424a0); not recorded for the other two | A virtual-interface member was staged through one representative instance, and classification and dispatch disagreed about one store (msg). |
| B13 | 10d59c93 | R23 | S2, S16 | flaky process tests, 12 of 20 runs (msg) | The test existed. The double resume read a freed frame, so it failed only when that word read zero (msg). |
| B14 | 345f36d6 | R23 | — (none in §2) | stress test of the sync parks (msg) | No test killed a queue head while one `put` served several waiters. [INFERENCE] |
| B15 | 86041952 | R6 | S9 | four existing tests, before merge (regions.md §9.2) | Caught: chains where a consumer sits downstream of a producer in its own group. |
| B16 | 9092f46a | R6, R10 | S18 | real UVM designs (msg) | A container mutation is not a store, and a lazily minted static-property signal gave an empty sensitivity list, so the wait depended on method declaration order (msg). |
| B17 | 1c28b18b, 26cd52aa, f357fb2f, 11b7cf30, 2ce653c2 | R2 | S28 | review of the previous commit's effect on delay conversions | Delays were tested at small known values: no x bit, negative, real or past-the-end delay. [INFERENCE] |
| B18 | a1246a91 | R4 | S13 | sv-tests nightly (regress-vlg_pr3527694_iv) | Needs a process that ends in the step of its `$strobe`; it crashed only when the freed frame was reused, 'always at -j4' (msg). |
| B19 | 976a87a6 | R24 | S17 | sv-tests nightly (uvm_id_register) | `$finish` was never combined with a virtual call. [INFERENCE] |
| B20 | 1f0c6086 | R2, R20 | S13, S25 | timing review of the nonblocking event trigger | iverilog has no `->>`, so no oracle-derived test existed (msg). |
| B21 | 0231d347 | R20 | S25 | not recorded (1c28b18b notes it) | The waiter woke in Active and read before an NBA of the slot had committed (msg); no test compared the region. |
| B22 | 45825ba6 | R12 | S23, S24 | not recorded (1c28b18b notes it) | Zero delays were literals, which take the plain path (msg). |
| B23 | 935bd9c6 | R11, R19 | S24, S27 | sv-tests ivtest nb_ec_concat, nb_ec_multi_ev, pv_wr_vec*_nb_ec | `<= @e` was tested on signal edges only (msg: posedge, `#d` and blocking `= @e` were unaffected). |
| B24 | 1eea5be9, 4401c371 | R17 | S4, S5, S8 | sv-tests ivtest always_comb_*; review of fix-procs (4401c371) | No always_comb sat above the initial it depends on (msg). The first fix split fused cones: fuse_bench 2.4 s to 4.6 s (msg). |
| B25 | 26a316b2, 6c2158b5 | R6 | S6, S10, S11 | a failing xhier test on the modular branch (msg) | Needs a cross-instance thunk group lowered after an earlier top's PreInit; single-top designs lowered PreInit last (msg). |
| B26 | 885653d5 | R14 | S21 | sv-tests ivtest pr1832097a | Force tests read after a delay. [INFERENCE] |
| B27 | 74d84909 | R23 | — (none in §2) | sv-tests ivtest disable_fork, pr718 | `disable` of a named fork was a silent no-op (msg). |
| B28 | bdbeb531 | R23 | S2, S15 | review of fix-procs (d_sync, d_sync_ja, ...) | `disable` was never combined with an inline fork child. [INFERENCE] |
| B29 | 1aacf148 | R23 | S2, S16 | review of fix-procs (d_dangle*, d_kill, d_nest_lex) | Every exit that skips a block's end (break, continue, return, abort, `$finish`) left the node linked (msg); no test combined one with a remote disable target. [INFERENCE] |
| B30 | ee320db0 | R21 | S24, S27 | review of fix-procs (ev_alias_decl, ev_alias_stale) | iverilog implements neither merge form (msg). |
| B31 | 52088981, b6165666, e61257c2, 6c801471, ff44b777 | R23, R26 | — (none in §2) | reviews of fix-procs and fix-disable-deferred | Fail-open allowlists: no test per park form, recursion order or virtual override (msg). |
| B32 | aaf53cb4 | R23 | S2, S15 | sibling of review finding P1#1 (s_taskcall) | Same gap for an inline task call; it crashed on main (msg). |
| B33 | afa51ab6 | R7, R14 | S18, S21 | sibling check of a reviewer's report on force/release with a net delay | The resolver made two serviced stores per resolution (msg); no test had an edge reader of a forced net. [INFERENCE] |
| B34 | fa800e77 | R24 | S17 | reproduction sweep of the likely-bug list (ReproT2, A6) | may-finish was tested on acyclic call graphs. [INFERENCE] |
| B35 | 9d433608, fa883cd6, 7836427d | R14 | S21, S26 | review of the force/net-delay series; sibling check | Each writer path (port copy, gate output, delayed assign) needed its own force gate (msg); no test forced a variable such a path drives. [INFERENCE] |
| B36 | 71ac25b7, e92ee320 | R14, R16 | S21, S26 | reviewer's report, checked against the LRM and iverilog 13; a maintainer decision (e92ee320) | Force × net delay was untested, and iverilog has no net delay (msg). |
| B37 | 4e6f60ae | R24 | S11, S17 | the shared-code check (msg) | The result depended on lowering order: a `$finish` in a function lowered before any push to the ring (msg). |
| B38 | fa16b929 | R12 | S23 | the docs/TODO.md known gap, reproduced against iverilog 13 | A recorded gap. iverilog applies event-controlled updates in Active, so it is no oracle (msg). |
| B39 | 6b412f5c | R25 | S3, S4 | sv-tests ivtest program3 / review | Program inputs never changed at time zero or in the slot of a design write. [INFERENCE] |
| B40 | 84d5b497 | R12 | S24 | review of fa16b929 (flip.sv) | iverilog differs and Verilator orders by source position (msg); no test released two updates of one signal around an unrelated write. [INFERENCE] |
| B41 | 472ddda0 | R12, R20 | S25 | review of 84d5b497 | iverilog has no `->>` (msg); no test fired two in one slot. [INFERENCE] |
| B42 | dd0fd197 | R11, R12 | S24 | review of 84d5b497 | A record released by its own site's commit was never tried. [INFERENCE] |
| B43 | 99cd87ef, 04b85c0f | R5, R26 | S11 | the shared-code check (99cd87ef, msg); review of the previous commit (04b85c0f, msg) | The check compares only units a design instantiates twice, and every `expect` test had one instance (VERIFICATION.md). |
| B44 | 679e3ce2 | R23 | S2, S16 | review of fix-procs | Needs a body that had never waited on an event and is parked only on `#delay` (msg). |
| B45 | 6b6ec151 | R26 | — (none in §2) | review of fix-procs | The flush point after a mid-body event control or `wait` did not exist, and no available simulator implements deferred assertions (msg). |
| B46 | eb815175 | R26 | — (none in §2) | code review of a documented TODO | No `$time` argument and no change that renders alike in earlier tests (the fix's tests). |
| B47 | 26fade75, b736565a | R14 | S22 | not recorded | No test staged an NBA before a force, or released one before the NBA region (the fix's tests). |
| B48 | 8a89563d (`nba-glitch`) | R8, R12 | S22 | not recorded | iverilog also merges updates of some target shapes (msg); earlier tests checked final values. [INFERENCE] |
| B49 | b0b268aa (`nba-glitch`) | R8, R12 | S23, S24 | not recorded | Plain and timed NBAs to one waited-on scalar were never mixed with an observer. [INFERENCE] |
| B50 | c76469be (`nba-glitch`) | R9 | S21 | not recorded | iverilog evaluates once per pass (msg), so it was no oracle for a select that changes and changes back. |
| B51 | 2bff14f1 | R5 | S1, S2 | the scheduler-rules postmortem (this document): code reading, then a probe | The one test with an always that parks mid-body (`gc_test.cpp`) waits on its own sensitivity's signal and edge, which hides it. |

**B51.** The prebuilt main binary (`build/clotho`, built 2026-11-24 18:14)
printed `3 woke n=1` for the design below; iverilog 13 printed `4 woke n=1`, which
is what 9.4.2 asks for. `wait fork` in the same place matched iverilog.

```systemverilog
module t;
  reg clk = 0, done = 0; integer n = 0;
  always @(posedge clk) begin @(posedge done); n = n + 1; $display("%0t woke n=%0d", $time, n); end
  initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #5 $finish; end
endmodule
```

Clotho printed one line, at t=3, and none at t=4. Traced: the process was in `clk`'s
static `pos` bucket (S1) and, parked at `@(posedge done)`, in `done`'s dynamic `pos`
bucket, both entries naming one wake word. `lower_wait` arms the word to 0 for the
dynamic entry, so the second posedge of `clk` found it armed through the static entry
and resumed the process at that wait. The entry the wait left in the `done` bucket (a
single-signal wait is not compacted without `disable` or process control) then resumed
the next activation at its loop top at t=4, which ran the body again from its first
statement: with a `$display` as the first statement the output was `1 enter`, `3 woke
n=1`, `4 enter`. Where `done` never fires, the stale entry made the next activation
register twice in a bucket sized for one, and the store past its end ended a 40-cycle
run in "simulation did not converge". An `expect` in that place was resumed at its
park too. `wait fork`, `await` and the mailbox and semaphore queues test their
condition again after a wake, which is why they printed the right text.

Fixed in 2bff14f1: a body with a park that arms the wake word (`TagInfo::arms_wake`)
is not subscribed. It waits for its list at the top of its loop, as `forever begin
@(list) body end` says (S1).

Patterns:

- **How the bugs were found.** Of 51 rows, 16 by review (including sibling
  checks and reviewers' reports), 8 by sv-tests, 1 by a reproduction sweep of
  review findings, 4 by the suite itself (a flake, a stress test, a failing branch test,
  a catch before merge), 2 by the shared-code check, 3 by a real design or the
  LRM's own example, 1 from a documented gap, 15 record nothing, and 1 by this review.
- **Ungated speedups.** 42 of 51 rows involve one (§2.2).
- **A wake service per write path.** The wake bracket is emitted separately
  for each write path (blocking store, block commit, record-queue commits, intra
  commit, file I/O, container mutation, resolver, force). Bugs where one path
  lacked what another had: B7, B8, B12, B16,
  B33.
- **One fact decided in two places.** B12 (classification and dispatch
  disagreed), B25 (a flag reset on one lowering path only),
  B37 (the ring was made only if an earlier body made it).
- **Two features at once.** `disable` × inline fork or task (B28,
  B32), `$finish` × virtual call (B19), force × net delay
  (B36), virtual interface × twin instances (B12). Each feature
  alone was tested.
- **Same-slot update order has no usable external oracle** (B38,
  B40, B41, B48): iverilog applies
  event-controlled updates in Active, merges updates of some target shapes and
  lacks `->>`; Verilator orders by source position. Expected values there come
  from the LRM.

## 5. Rule × speedup cells

### 5.1 Cells that had bugs

A cell lists every row of §4 that has one of its rules and one of its speedups.
Rows involving no speedup of §2: B1, B4, B14, B27, B31, B45, B46.

| Cell | Rules × speedups | Bugs | Gate | Tests on main |
|---|---|---|---|---|
| NBA creation order × staging paths and ready lists | R12 × S23, S24 | B9, B12, B22, B38, B40, B42, B49 | none | `intra_assign_test.cpp` (many); `nba-glitch` adds `nba_update_order_test.cpp` |
| disable/kill × one-wake token, inline runs, claim gating | R23 × S2, S15, S16 | B13, B28, B29, B32, B44 | none | `loop_test.cpp`, `process_test.cpp`, `sync_test.cpp` |
| force × continuous processes, inertial commit, NBA commit | R14 × S21, S22, S26 | B10, B26, B33, B35, B36, B47 | none | `force_release_test.cpp`, `net_delay_test.cpp` |
| `$finish` × cooperative finish | R24 × S11, S17 | B19, B34, B37 | none (S11: `dedup`) | `function_test.cpp`, `class_test.cpp`, `llvm_backend_test.cpp` |
| `->>` × pend chain and region elision | R20 × S13, S25 | B20, B21, B41 | none | `event_test.cpp`, `intra_assign_test.cpp` |
| per-update events × block commit, ready lists, event nets | R8, R9 × S21, S22, S23, S24 | B48, B49, B50 | none | none on main (`nba-glitch` only) |
| `always` and `initial` start × static subscription | R18 × S1, S4, S6 | B6 | S6: `thunk` | `llvm_backend_test.cpp` |
| time zero × seed, late start, comb fusion | R15, R17 × S1, S4, S5, S8 | B2, B24 | S8: `comb_fuse` | `llvm_backend_test.cpp`, `comb_fusion_test.cpp` |
| change wakes × store-site service and skipping | R6, R10 × S18, S19 | B7, B8, B12, B16 | none | `llvm_backend_test.cpp`; open gaps in §5.2 |
| thunk and shared code × wakes and `expect` | R5, R6 × S6, S10, S11 | B25, B43 | `thunk`, `dedup` | `llvm_backend_test.cpp`, `sva_test.cpp` |
| member guards × change wakes | R6 × S9 | B15 | `regions` | `region_test.cpp` |
| event control × static subscription and the token | R5 × S1, S2 | B3, B51 | none | `event_test.cpp` |
| timed records and event objects | R11, R21 × S24, S27 | B23, B30, B42 | none | `intra_assign_test.cpp`, `event_test.cpp` |
| edges × store service and z elision | R7 × S18, S20 | B5, B33 | S20: `z_elision` | `llvm_backend_test.cpp` |
| program ring × seed order | R25 × S3, S4 | B39 | none | `program_test.cpp` |
| fork × ring and bucket sizing | R22 × S2 | B11 | none | `fork_test.cpp` |
| delay conversion and `#0` × wheel, Inactive region | R2 × S13, S28 | B17, B20 | none | `llvm_backend_test.cpp`, `event_test.cpp`, `gate_test.cpp` |
| Postponed pends × region elision | R4 × S13 | B18 | none | `strobe_test.cpp` |

### 5.2 Cells with no test

| Cell | What nothing checks |
|---|---|
| R8 × S22, R9 × S21 | Two NBAs to an observed scalar in one slot, and a select that changes and changes back. The fixes and tests are on `nba-glitch`. |
| R6 × S19 | Waiter kinds crossed with write paths. Waiter kinds: static, dynamic, vif, `<= @` trigger, class-property notifier, event net. Write paths: blocking, block commit, record queues, intra commit, file I/O, DPI output, `ref`, vif, force/release, resolver. [INFERENCE: no such matrix found in `tests/`] |
| R1, R13, R20 × S13 | The region-use predicates (`has_nba`, `design_uses_nb_events_`, `has_inactive`, the reactive set, `has_renba`), except through feature tests. There is no gate. |
| R2 × S13 | `#0` resuming in Inactive, before NBA, all together: pinned only through `->> #0` and gate tests [INFERENCE: grep of `tests/`]. |
| R17 × S5, S10 | The late start of an always_comb group whose schedule group has rows from several instances [INFERENCE]. |
| R24 × S17 | NBAs staged in the slot of a `$finish` stay uncommitted [INFERENCE: no test found]. |
| R23 × S1 | `p.kill()` of an always parked at its static leading control resumes its body once and does not stop it: the loop's park has no abort poll (probed on 32ee5c3e). An always that waits at its head polls. |
| R1 × S12 | Observed reached again from the reactive set, for an assertion clocked by a program signal: refused (`docs/TODO.md`). |
| Gates | `comb_fuse` and `o3` are never set in a test (`fuse = false` also turns comb fusion off); `dedup`, `split` and `z_elision` in one case each. |

## 6. Not settled without the LRM

- The wording of every rule in §3.
- Region clause numbers in comments disagree. Observed is cited as 4.4.2.5
  (`Scheduler.cpp:emit_active_run` at the property-evaluation run, and
  `Analyses.cpp`) and as 4.4.2.4 (same function, at the report flush), while
  `event_test.cpp` and 1f0c6086 cite 4.4.2.4 for NBA.
- Clause 15.5.3: `named-events.md` and tests cite it for `triggered` (R19), and
  `nba-update-order.md` on `nba-glitch` cites it for re-evaluating an
  expression event on an operand change (R9).
- Whether 4.5 lets every NBA update of a pass run before any woken process. The
  per-update wakes of `nba-update-order.md` (on `nba-glitch`) rest on that
  reading.
- When an expression event control is re-evaluated (9.4): at each operand update
  (`nba-glitch`), or when the waiter runs (iverilog).
- The cases LRM_NOTES 51 leaves undecided: a plain NBA between an arm and its
  occurrence, and updates released in the opposite order to their arms.
- Whether a `$finish` may leave NBAs staged in its slot uncommitted (20.2; S17).
- Whether running `join` children inline at the spawn is within 9.3.2.
  `lower_fork_inst` asserts that it is.
- Two `always @*` blocks that share a loop variable oscillate under per-store
  wakes and abort at the oscillation limit (3a58c8f7), where iverilog converges.
  Whether the LRM requires convergence is not decided here.
- B51 is decided against 9.4.2 as cited and against iverilog 13; the
  clause text was not read.
