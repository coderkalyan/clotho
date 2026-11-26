# Scheduler trace and rule checker

Status: phases 1 to 3 landed (2026-11-25); phase 4 partly landed (2026-11-26): R15.1, R17.1 (time zero), R1.1, R1.2, R2.2, R2.3, R4.1, R4.2 (regions, `#0` batch, timers, run-once bodies), R25.1, R25.2 (program regions, continuous before program), record identity of join, call, sync, await and expect parks (R5.1, B43), and R23.1 to R23.5 (block occupancy, disable reach, inherited blocks, abort obeyed, force wake). Reintroduced B2, B24, B39 and a dropped Observed re-settle are caught (R15.1, R17.1, R25.2, R1.2); B29, B27, B43 and the R2, R4, R25.1, R23.2 and R23.5 checks are covered only by hand-built event sequences. The R23 x S1 kill gap was caught by R23.4 and fixed. Measured with phase 4 (Debug, extra asserts, `ctest -j2`, 117 tests, min of 2): 232 s with the checker, 227 s with `CLOTHO_NO_TRACE_CHECK=1` (+2%); no violation; a Release build has no `rt_trace` symbol; the runtime's libc++ uses resolve in an executable (checked with clang). Still open for phase 5: R22, R11, R16, S9 member runs, R14, and R8/R9 once `nba-glitch` merges. Phase 1: `spawn`, `park`, `wake`, `dispatch`,
`resume`, `end` and `slot`, the control table and the R5 checks. Phase 2: `region`, `nba_create`
(`nba_stage` and `nba_release` for timed updates), `nba_pass`, `pend`, `trigger`, `slot_end` and
`finish`, and the checks of R12, R20 and R24. Phase 3: `change`, the shape of each listed storage,
and the checks of R6 (R6.1 a wake owed, R6.2 a wake with no write, R6.3 a change under a parked
waiter that was never woken) and R7 (R7.1 an edge fact, R7.2 `changed`); the other rules are phases 4-5,
all in `src/runtime/TraceCheck.cpp`. Measured (Debug, extra asserts, `ctest -j2`, load ~5, one run
each): 234 s with the checker and 218 s with `CLOTHO_NO_TRACE_CHECK=1` (+7%, inside the 15%
budget); phase 2 measured +2%. No violation in the suite. With the fixes reverted locally, the
tests of B5 (Table 9-2 on x and z) fail with R7.1, those of B7 (file-I/O write-backs) with R6.3, and
that of B25 (a thunk group lowered after a PreInit store) with R6.3 (twin mode, which sees it at
build time, switched off). Phase 2's reverts (B42, B38, B41, B21, B37) are as phase 2 measured them. Reverting `signal_has_waiters`
to ignore virtual-interface waiters (a B8-shaped gap) was not caught, and no interface test fails
with it either. The `$readmem` and `ref`-after-suspend gaps of `scheduler-rules.md` §5.2 were closed
before phase 3 (bf0fcdad; 09511fbe and df83e80f), so neither is a known entry; their tests run
clean through the checker. All 115 ctest tests pass, `cli_test` included, on a build that fetched its own slang.
Step 3 of the plan in `scheduler-rules.md` §1. R, S and B ids are that document's §3, §2 and §4,
and its caveat applies: no rule is checked against the LRM text, and clauses are cited only as
the code and `docs/LRM_NOTES.md` cite them.

The generated code and the runtime report each scheduling event to a checker in the
runtime, which tests the rules of §3 as predicates over those events while the run goes
on. In a build with `CLOTHO_EXTRA_ASSERTS`, a violation is found on any design that is
simulated, with no expected output and no external oracle. It is a Clotho bug, or a rule
that needs rewording, never a property of the design.

Probe, 2026-11-25: the main checkout's `build/clotho` (built 2026-11-24 23:44) still
prints `3 woke n=1` for the B51 design of `scheduler-rules.md` §4. `clotho run --time`
splits its 72 ms into read 18.7, analyze 12.3, compile 40.7 and simulate 0.63.

## 1. Decisions

| Question | Proposal |
|---|---|
| Checker | C++ in the runtime, online: `src/runtime/TraceCheck.cpp`, events declared in `src/runtime/Trace.h` (§5). |
| Builds | `CLOTHO_EXTRA_ASSERTS` only; a shipped build emits no call and links no checker (§4.1). |
| Phase 1 rules | R5: a wake reaches only an activation parked at a control, by an event that control lists; each wake gets one dispatch; an ended activation gets nothing (§3). |
| Cost | About 50 ns per activation in phase 1, under 2× on scheduler-bound simulation; the suite is compile-bound, projected +3–8% (§4.2). All [INFERENCE] until phase 1 measures it. |
| Failure | The test that ran the design throws, unless `tests/trace-check-known.txt` lists it. `clotho run` prints a marker the sv-tests summary classifies (§6). |

## 2. Trace

### 2.1 Identities

| Term | Meaning |
|---|---|
| Activation `a` | What a ring holds, with the home tag bits that `Scheduler.cpp:parked_handle` adds stripped: a coroutine handle, or the address of a thunk group's wake pair with bit 0 set (`RootTables.cpp:emit_seed`). There is one per wake word, so a fused or schedule group (S7, S8, S10) is one. States: `Spawned`, `Parked(C)`, `Queued`, `Running`, `Ended`. A thunk is `Parked` at its leading control between runs. |
| Control `C` | A dense id per suspend site, from a root table built like the seed's (kind, process, source location). Every coroutine park goes through `Scheduler.cpp:emit_suspend` (10 call sites), which would take the control as an argument, so a park site cannot compile without one. |
| List `L(C)` | What the control waits on: `(signal, edge)` pairs from the IR (`ir_->wait_edges` in `Events.cpp:lower_wait`; `wait_events` in `Subroutines.cpp:lower_group`), as a constant table per control, with the resolved object or instance for an entry named by handle. Never read back from buckets or waiter lists: those are the speedups under test (S1, S27). |
| Signal `x` | The address of its storage in its own instance (`StateLayout.cpp:storage_ptr`, through the binding or virtual-interface handle of the writer or waiter), so every write path and waiter kind agree. An event is its object. |
| Cause `w` | What pushed: `static`, `dynamic`, `vif`, `notifier` (each with x and a fact `pos`, `neg` or `changed`); `event` (object); `timer`; `inactive` (`#0`); `child` (fork record); `callee`; `force` (`disable`, `kill`); `sync`; `await`; `proc` (`process::resume`); `expect`; `pend`; `seed`. |

### 2.2 Events

Each event is a call into the runtime with scalar arguments. "Ph." is the phase of §7
that first emits it.

| Event | Fields | Emitted at | Ph. |
|---|---|---|---|
| `spawn` | a, process index (`Processes.cpp:proc_index_value`), parent | after each ramp: `RootTables.cpp:emit_seed`, `run_once_fn`; `Processes.cpp:lower_fork_inst`; `Subroutines.cpp:lower_task_call_inst`; `SystemTasks.cpp:lower_strobe_pend`, `lower_reactive_pend`; `Dpi.cpp:emit_dpi_export`; the `$monitor` ramp in `Scheduler.cpp:emit_active_run` | 1 |
| `park` | a, C, L, a delay's deadline | before the first store a waker can read (the re-arm, a registration, a record), at the 10 `emit_suspend` calls: `Events.cpp:lower_delay`, `emit_wait_suspend` (from `lower_wait` and the always loop of `Subroutines.cpp:lower_group`); `Processes.cpp:lower_fork_inst`, `emit_proc_park_while_suspended`, `lower_wait_fork`, `lower_proc_wait`, `lower_sync_wait`; `Subroutines.cpp:lower_task_call_inst`; `SystemTasks.cpp:lower_expect_wait`. Also each row `emit_seed` subscribes without queuing (a queued row parks at its loop top after its first run) and the re-arm of a thunk (`Groups.cpp:emit_sched_entries`, `emit_unit_entry`) | 1 |
| `wake` | a, w, ring | at each push: the 12 callers of `Scheduler.cpp:emit_wake_push` (the scans `Events.cpp:emit_static_scan_rt`, `emit_dynamic_scan_rt`, `emit_event_wake_list`; both timer paths of `build_enqueue_trampoline`; `Processes.cpp:emit_node_force_wake`, `emit_proc_wake_waiters`, `emit_proc_resume`, `emit_sync_wake`; the join and caller wakes of `Subroutines.cpp:lower_task`); `emit_region_push`; the seed's pushes; `SystemTasks.cpp:lower_expect_wake`, `lower_reactive_pend`. A `#0` wakes at its batch promotion (`inactive.copy.body`, `re_inactive.copy.body` in `emit_active_run`); `emit_zero_delay_push` emits nothing | 1 |
| `dispatch` | a, how (ring, inline, run-once); returns 0 to refuse | before every resume or thunk call: both drains, `$strobe` and `$monitor` in `emit_active_run`; `RootTables.cpp:run_once_fn`; the inline resumes of `lower_fork_inst`, `lower_task_call_inst` and `Dpi.cpp:emit_dpi_export` | 1 |
| `resume` | a, C | the resume block that `emit_suspend` creates | 1 |
| `end` | a | the final suspend: `coro.final` in `lower_group` and `lower_task` | 1 |
| `slot` | time | `runtime/Scheduler.cpp:rt_wheel_advance` (and the seed, for time 0) | 1 |
| `region` | the region entered | blocks of `emit_active_run`, on the edges that enter them and not on each activation's trip round the drain loop: `active.check`, `inactive.promote`, Observed, `reactive.check`, `re_inactive.promote`, Postponed and `wheel.posttick`; a region with no code emits none. `rt_sim_run` marks the start of the finals | 2 |
| `nba_create` | the target's planes (storage addresses), the bits and mask as stored, `gated`, the staging path | when a statement creates an update: `Stores.cpp:lower_store_nb`, `emit_array_nba`, `emit_agg_nba` (one per statement, whichever way it stages); a zero-delay `<=` in `emit_intra_nba_arm` (not from `emit_intra_nba_ordered`, whose copy restates an update already created) | 2 |
| `nba_stage`, `nba_release` | the same planes, keyed by the record; the release moment | a timed update is created when its record is released: `emit_intra_nba_arm` and `emit_intra_event_arm` report its planes when they arm it, and `runtime/Scheduler.cpp:rt_intra_cb` and `rt_intra_event_fire` create it (not `rt_inertial_cb`: S26 is Active, not NBA). The arm order is the checker's own count of the stages | 2 |
| `nba_pass` | begin, end, region | entry to and exit from `Stores.cpp:emit_nba_commit`, the NBA region's pass and the Re-NBA region's | 2 |
| `pend`, `trigger` | event object; `->` or `->>` | `Events.cpp:emit_event_pend`, where the object links onto the chain (the expiry thunk of `->> #d` and a detached `->> @(x)` come through it); `emit_event_fire`, where every trigger converges: `->` reports as `->`, every form of `->>` as `->>` | 2 |
| `slot_end` | | `runtime/Scheduler.cpp:rt_wheel_advance`, on entry: every region is empty | 2 |
| `finish` | | `runtime/Scheduler.cpp:rt_finish_coop`, which does not know which activation finishes | 2 |
| `change` | the written storage `x` (null where no waiter names it); bit 0 of the must and may planes before and after; whether a plane and the z word differ; the facts the code computed (`pos`, `neg`, `changed`); which of these the bracket knows (S19 skips what no waiter consumes); the path | before the wakes of each bracket: `Events.cpp:service_wait_queue` (blocking stores and the block, array and intra commits), `service_wide_window`, `service_change_notify` (class property, `x` null), the `ref` thunk (`emit_ref_wake_thunk`, and the callee's own bits with `x` null), `service_any_waiters` and `Containers.cpp:service_signal_container_mutation` (the level paths: nothing of the bits is known). A string's compare and a class-property container's notifier are not reported | 3 |
| `fork`, `child_done`, `join` | the forking activation (null in a function), the join record, the join kind and the number of children; the completing child and its record; the parent and the record of the join it is past | `Processes.cpp:lower_fork_inst`, once the record is initialized and before the first child is ramped; `Subroutines.cpp:lower_task`, the fork-child epilogue, before the count falls and the parent's wake; `lower_fork_inst` again at the join point, whether the parent parked for it or not (none for `join_none`) | 4 |
| `members`, `member` | the guarded group's activation, each member's own list (storage, edge, member) from the IR, the number of members, and whether no two events of the group share a trigger bit; the activation and the member that starts | `RootTables.cpp:emit_seed` and `Trace.cpp:emit_trace_seed_row`, once per guarded group; `Subroutines.cpp:lower_group`, at the start of every member of a guarded group (thunk row and coroutine), after its guard. An event object is not listed | 4 |
| `inertial_decide`, `inertial_arm`, `inertial_fire`, `inertial_applied` | decide: the site (the dense id of the target), whether the value equals the pending record's and the target's current one, and the epoch of the new record the runtime asked for (zero: none); arm: the site, the record, its delay and its epoch; fire: the record and whether the runtime takes it for the latest of its site; applied: the record | decide and fire: `runtime/Scheduler.cpp:rt_inertial_decide` and `rt_inertial_cb` (a zero delay calls the second at once, not through the wheel); arm: `Stores.cpp:emit_inertial_arm`, once the record is filled and before it is armed; applied: `rt_intra_release`, for each record a commit released (every intra-assignment site comes through it: the checker ignores the records that are not inertial) | 4 |

A fork is a `fork` event, a `spawn` per child (tied to the record by its creator's latest `fork`) and the
parent's join park (`join_none` has none); each child's completion is a `child_done` and, at the one the join
is due at, `wake(parent, child)` from the epilogue of `lower_task`; the parent's pass of the join is a `join`;
`disable` and `kill` are `wake(a, force)` from
`emit_node_force_wake`, reached from `Processes.cpp:lower_disable_list` and
`lower_proc_kill`. Block entry and exit, which R23 needs, come in phase 4.

### 2.3 What each speedup shows

All 22 ungated speedups of `scheduler-rules.md` §2.2 appear.

| S | Visible in the trace as | Checked by (phase) |
|---|---|---|
| S1 | a `static` wake of an activation parked at a control that does not list it | R5 (1) |
| S2, S16 | a second wake of one park (event and timer, or event and force); a wake of a `Queued` or `Running` activation | R5 (1) |
| S3 | the order of `dispatch` in a region | nothing: 4.7 leaves it open |
| S4, S5 | `spawn`, `wake(seed)` and the first `dispatch` of each group in slot 0; always_comb groups after the rest | R15, R17 (4) |
| S10 | one activation per schedule group | R5 (1) |
| S12, S13, S14 | `region` events; a region with no code emits none; reactive dispatch order | R20 (2); R1, R4, R25 (4) |
| S15 | an inline `dispatch`, and a wake of the parent or caller while it runs; the start of a fork's children | R5 (1), R22 (4) |
| S17 | any event after `finish` | R24 (2) |
| S18, S19 | `change` with the code's facts; a waiter owed a wake that gets no `change` or no `wake` | R6, R7 (3) |
| S21 | continuous processes are activations like any other | R5 (1), R15 (4) |
| S22, S23, S24 | one `nba_create` per NBA, and what `nba_pass` leaves in the target | R12 (2) |
| S25 | `pend` order against `trigger` order and region | R20 (2) |
| S26 | the `inertial_*` events: a record's arm, its fire at its deadline and its application at the top of the slot, before the Active region | R16 (4) |
| S27 | a wake by `event` against the object in L; `trigger` | R5 (1), R6 (3) |
| S28 | the time of a `timer` wake against the park's deadline | R2 (4) |

Gated: S6 is `dispatch` plus a park at the entry; S7 and S8 give a group one list; S9 shows as
`member` events against each member's own list (R5.5, R6.4: phase 4); S11 makes every identity per
instance; S20 shows as R7 on a slot with no z word.

## 3. Rules

Ordered by the number of §4 rows each would have caught. The B-ids are read from each
fix's commit message, not from a run [INFERENCE]; B51 was probed.

### R5, phase 1

| Check | Predicate | Would have caught |
|---|---|---|
| R5.1 wake | `wake(a, w)` needs `Parked(C)` with `w` matching `L(C)`; or `w = force` and `Parked`; or a push of a `Spawned` activation (`seed`, region push, `pend`). It sets `Queued`. A wake of a `Queued`, `Running` or `Ended` activation is a violation. | B51 (list); B3, B13, B44 (queued twice); B28, B32 (parent or caller running inline) |
| R5.2 dispatch | `dispatch(a)` from a ring needs `Queued`; inline or run-once needs `Spawned`. It sets `Running`; a refused dispatch skips the resume. | B13's second resume of a freed frame |
| R5.3 resume | `resume(a, C)` needs `Running` and C the last park. An unreported park site fails here. | the instrumentation's own gaps |
| R5.4 end | An `Ended` activation gets no event until `spawn` reuses its address. | B13 |

A signal cause `(x, fact)` matches entry `(x, e)` when `fact` implies `e`: `pos` gives
posedge, edge and any-change; `neg` gives negedge, edge and any-change; `changed` gives
any-change. `event` matches the object; `timer` a delay park whose deadline is now;
`inactive` a `#0`. `child`, `callee`, `sync`, `await`, `expect` and `proc` match a park of
their own kind in phase 1, and its record or queue in phase 4 (B43).

R5.1 to R5.4 also check the one-wake half of R23, which `scheduler-rules.md` §4 files under
B13, B28, B32 and B44. **Cannot see:** a wake that never comes (R6); a wrong list in the IR
(frontend: B4, B6); the order of simultaneous wakes (4.7); members inside a fused group
(S8, S9); a stale wake of a freed frame that a new activation has reused while parked on
the same event.

### The rest

| Rule | Predicate | Cannot see | Would have caught |
|---|---|---|---|
| **R12**, phase 2 | For each NBA pass take U: the updates created and not yet applied when it began, ordered by creation: a plain NBA when it executes, a timed one when released, one release moment's records in arm order. For each plane x written by U that takes no gated NBA (`StoreNbGated`, a forceable variable: R14's) and whose bits the trace knows, x at `nba_pass` end must equal x at begin with U's masked stores applied in that order, followed by a prefix of the updates released during the pass: the commit of a record's own site may or may not have reached what the pass's own wake released (B42), so the checker takes the longest prefix the bytes allow, an update that changed nothing counting as applied, and the rest wait for the next pass. Nothing the Active set created is left unapplied at the entry of Observed, Reactive or Re-Inactive, and nothing at all at the entry of Postponed and at `slot_end`; a pass begins from the Active region (the Re-NBA pass from Reactive), and no statement creates an update inside one. | values inside a pass (R8: B48, B49); strings and containers; updates a `$finish` leaves staged; planes the backend does not spill (a runtime-offset select of a very wide vector, a plane over 1024 bits); the orders LRM_NOTES 51 leaves open (Clotho's reading is enforced); a wrong value or mask at creation (only order is checked); which occurrences made a release moment (the scheduler's count, until phase 3's `change`) | B9, B22, B38, B40, B42; B12's NBA half if the virtual-interface staging reports the bound instance |
| **R20**, phase 2 | A `->>` trigger happens only inside an NBA or Re-NBA pass, and each trigger is of the oldest event pended and not yet triggered: a pass triggers in `pend` order, and an event is on the chain once. At `slot_end` and at the entry of Postponed nothing pended is untriggered (the Active set's at Observed and Reactive too). A `->>` form that fires its event itself, outside a pass, reports as a `->>` trigger and breaks the first clause. | whom the trigger wakes (R5, R6) | B21, B41 (B20 already aborts on the wheel's assertion) |
| **R24**, phase 2 | After `finish`, until `rt_sim_run` starts the finals: no `nba_create` or `nba_stage`, `pend` or `trigger`; a `wake` only by the completion that makes a frame leave (`child`, `callee`, `await`); a `dispatch` only from a ring and only of an activation such a wake queued after the finish, which resumes at its call or join and leaves through its own epilogue. An activation queued before the finish is never dispatched (`lower_finish` empties the ring), and none starts inline or runs once. | code that runs on and only prints, up to its next store, wake or trigger (a `park` is the exit's own: a thunk group re-arms after its body returns) | B37; B19 and B34 wherever the code that ran on stored, staged, triggered or woke |
| **R6**, phase 3 | **R6.1:** after `change(x)` or `trigger(o)`, every `Parked(C)` with an entry the write satisfies (the checker's own bit-0 ranks where the bracket gave them, the code's facts where it gave only those: the `ref` thunk) gets a `wake` before the next `dispatch`, `park`, `region` or `slot_end`. A level write the code does not classify is a change to every `any` entry. **R6.2:** a `wake` by a store (`static`, `dynamic`, `vif`) needs a `change` of the same `x` since the last dispatch, park or region that satisfies its fact: a write that changed nothing wakes nobody. **R6.3:** at each `slot_end`, a still `Parked(C)` whose entry (x, e) is satisfied by x's change from its bytes at the park to its bytes now is a violation. R6.3 reads design memory, so it sees stores no bracket reports and signals the code skipped (S19). It reads scalars, vectors and memories whose storage is at most 2048 bytes (`Backend::trace_shape`); a larger memory, an event, a string, a container and an aggregate are listed and not read. Ranks of bit 0 are read from the first element. | a change that reverts within the slot on an unbracketed path; content behind a handle (B16: a container mutation, with an empty list from the frontend); a string's compare; a spurious wake on a level path (an array commit wakes when a record applied, not when a value changed); a class-property notifier (the waiter names the notifier object, not the property) | B7, B25 (R6.3); B8's shape if the store is unbracketed; B12's wake half (R6.1) |
| **R7**, phase 3 | Each fact in `change(x)` equals Table 9-2 on the old and new bit 0, ranking 0 < x = z < 1: `pos` when the rank rises, `neg` when it falls, only 0 and 1 for a 2-state slot (R7.1). `changed` equals "a plane or the z word differs" (R7.2); where the bracket did not read the z word (no level waiter consumes it) a plane that differs must make it true, and no more is asked. | a fact the code did not compute (S19: R6.3) | B5 |
| **R22**, phase 4 | **R22.1:** a child of a `join_none` fork is never dispatched inline, nor is a sibling of a `join_any` once one child has completed; each starts from a ring, which runs only after the parent has blocked or ended. **R22.2:** a child completes once, into the join record of the fork that created it (the latest `fork` of its creator when its `spawn` came). **R22.3:** the `child` wake of a parent parked at a join comes at the completion the join is due at, the last of a `join` and the first of a `join_any`; and that completion, with the parent parked at the record, is followed by the wake before the next `dispatch`, `park`, `region` or `slot_end`. **R22.4:** a parent passes a join, whether it parked for it or not, only once every child of a `join`, or one of a `join_any`, has completed and every child the fork announced was created, and only the join of a fork it made. Nothing is asked after a `$finish`, whose exit leaves frames before their children. | whether a `join_none` child starts when the parent blocks and not later (the ring pops it in some region: R1); the children of a fork in a function, which has no activation and no join (R22.2 only) | no §4 row: B11 is ring sizing, which the ring guard reports. Reintroduced locally, as mutations: a `join_any` that does not wait (R22.4), `join_none` children run at the fork (R22.1), a due child that does not wake its parent (R22.3); `fork_test` flags each |
| **R5.5, R6.4**, phase 4 (S9) | A guarded group is one activation that runs each member behind a guard on that member's own list. **R6.4:** a write that satisfies an entry of a member's own list owes that member a run in the activation that the write wakes or finds running, and every owed member has run by the time the activation parks again at its static control. A member that has run in the activation is owed none: a fused group runs its producers first, so a later write finds it past. **R5.5:** a member runs only when a write that satisfies its own list was seen in the activation, or a wake that names no trigger (the seed, `disable` or `kill`, an event) set every bit; where two events of the group share a trigger bit (`exact` false: `Groups.cpp:compute_trigger_masks` colours collide, which costs precision and nothing else) a sibling's event may run it too. Nothing is asked after a `$finish`. | which of the events that share a bit ran a member; a write no bracket reported (R6.3); the order of the members | B15 (the guard tested the word as it was at the group's entry, so a consumer fed from inside its own group was skipped): reintroduced locally, `region_test` and `comb_fusion_test` fail with R6.4 in 8 runs |
| **R16**, phase 4 (S26) | **R16.1:** after a store evaluates a value at an inertial site, the runtime keeps the pending update when the value equals the one it holds (nothing is armed), cancels it for any other value, and arms a new one unless the value is the target's current one (LRM 10.3.3 steps b to d, as `rt_inertial_decide` cites them). **R16.2:** the generated code arms the record that decision asked for, with its epoch. **R16.3:** the wheel fires a record at the tick its delay ends (a zero delay: at once) and loses none. **R16.4:** the runtime applies a record exactly when nothing superseded it: one a later store cancelled is dropped, one nothing cancelled is applied. **R16.5:** an update the wheel fired is applied by the commit at the top of its slot, before the Active region begins; a zero delay's lands in an NBA pass of its slot (the reading `emit_inertial_arm` cites: the rule text of `scheduler-rules.md`, an Active-region update, is not asked of it); none is left unapplied at the end of the slot. Nothing is asked after a `$finish`. | the values: what an update writes, and whether the compares that gave `same_pending` and `same_cur` were right (the lowering's `icmp`); a record the runtime still takes for pending after it fired (`rt_inertial_decide` keeps `pending` set), which is tolerated; force and release of a delayed net (R14) | no §4 row maps here: B35 and B36 are force × net delay (R14). Reintroduced locally, as mutations: a callback that applies a superseded record (R16.4), no commit of the wheel's records at the top of the slot (R16.5), a same-value store that restarts the timer (R16.1); `continuous_assign_delay_test`, `net_delay_test` and `gate_test` fail with each |
| **Later**, phases 4–5 | R14 (needs the override stores marked in the IR: B10, B26, B33, B35, B47); R15 and R17 at slot 0 (B2, B24); R2; R11; R23 with block occupancy (B27, B29); R25 (B39); R8 and R9 once `nba-glitch` merges (B48–B50); R26 | | |

Out of reach of every phase: frontend lists (B4, B6, B31); B1, a comment; B11, ring sizing,
which the ring guard reports; B14, the sync handoff; B17, a conversion before the park
records its deadline; B18, a lifetime bug that frame poisoning and ASan catch; B36 except
through R14; B45 and B46, R26's text.

## 4. Cost and gating

### 4.1 Gate

- **Shipped builds.** The `Backend::emit_trace_*` helpers sit under `#ifdef
  CLOTHO_EXTRA_ASSERTS` and emit nothing otherwise, as `Sanitizers.cpp:emit_encoding_check`
  does. `RuntimeAbi.cpp` declares no `rt_trace_*` function, `Jit.cpp` exposes none, and
  `TraceCheck.cpp` and the hooks in `runtime/Scheduler.cpp` (`rt_wheel_advance`,
  `rt_intra_cb`, `rt_intra_event_fire`, `rt_finish_coop`) sit in the same `#ifdef`. The code
  a shipped build generates is unchanged. A test shaped like `encoding_check_test.cpp`
  checks the emitted IR for `rt_trace_` in both kinds of build.
- **Extra-asserts builds.** `SimulationConfig::trace_check` is on by default, as
  `verify_shared_code` is (`SimConfig.h`); when off the backend emits nothing.
  `CLOTHO_NO_TRACE_CHECK=1` turns it off, to time a run without it, as `CLOTHO_NO_TWIN` does.

### 4.2 Cost

Phase 1 has about 55 emission sites, counted from the callers above: 10 `emit_suspend`
calls plus the seed and thunk re-arms; 20 push sites (12 `emit_wake_push` callers and 8
other pushes); 10 dispatches; 8 spawns; 2 ends. The bucket scans are inlined into every
store bracket (S18), so `wake` call sites grow with a design's store sites times their
buckets. All figures are estimates.

| Item | Estimate | Basis |
|---|---|---|
| Call from generated code | ~2 ns | ~1.8 ns per by-value call at tier 0 (`dpi-c.md`) |
| Checker work per event | 5–15 ns | one lookup by handle and a state store; a park also copies L, ~5 ns per entry |
| Phase 1, per activation (park, wake, dispatch, resume) | ~50 ns | ~70 ns per activation on `combchain` (`comb-fusion.md`), so under 2× on scheduler-bound runs |
| Phases 2–3 | +20–40 ns per serviced store and per NBA; per slot ~10 ns plus a hash of each waited signal's bytes | 2–3× on scheduler-bound runs; the hash is the cost to watch for waited memories (§8, item 2) |
| Compile, per site | ~4 IR lines, ~0.14 M host instructions, ~50 µs | ~34k host instructions per IR line at tier 0 (`dynamic-arrays.md`) |
| B51 design | ~20 sites: +1 ms on a 40.7 ms compile; ~15 activations add under 10 µs to the 0.63 ms simulate | the probe above |
| Suite | +3–8% | the suite is LLVM-bound: an unoptimized frontend moves it 1.1× (`CMakeLists.txt`) |
| Memory | ~64 bytes per live activation plus its L; one hash per waited signal (phase 3) | |

Phase 1 measures the suite with and without `CLOTHO_NO_TRACE_CHECK=1` and states the
result in its commit.

### 4.3 Buffer

The checker is online and writes no trace file. Each call updates its state and appends a
32-byte record to a 256-entry ring, which is printed only with a violation: the
activation's last events and the slot's last events. Online, because:

- R6b reads design memory at every slot; a file would carry every waited value at every slot.
- `dispatch` refuses a double resume before it happens: B13 resumed a freed frame.
- A long UVM run would write gigabytes, and the suite simulates thousands of designs.
- The checker stays one file: a state map, the list store, the predicates and the report.

## 5. Placement and language

**Recommendation: C++ in the runtime, online.** It reads design memory, which is the only
way to see a store no bracket reports (B7, B25), and it stops at the first violation, before
its consequences, with no trace files, no parser and no second toolchain in a suite run. A
Rust tool over a trace file would share its event reader with the step-4 reference, but it
could not see unbracketed stores, every run would write its whole trace, and cargo would
join the CI suite job. The price of C++ is that the step-4 reference cannot call it.

To keep the checker independent of the code it checks:

- `TraceCheck.cpp` includes only `Trace.h` and the standard library. The one kernel fact it
  shares is how bit 0 decodes from the must, may and z planes (`kernel.md`).
- Lists come from IR facts and causes from what fired, never from buckets or ready lists.
- Table 9-2 and creation order are written from `scheduler-rules.md` §3, not shared with
  `Events.cpp:emit_edge_facts` or `runtime/Scheduler.cpp:intra_ready_push`.

Records are fixed-size and versioned, so a later `CLOTHO_TRACE_DUMP=<file>` could hand the
step-4 reference the order Clotho chose in a run, to test against its legal set.

## 6. Integration

A violation ends the run as an interrupt does: the checker sets a halt that
`rt_wheel_advance` reads (as it reads the stop time), a refused `dispatch` skips its resume,
the finals are skipped, and `rt_run` returns. A host-side hook in `src/ir/Jit.h`, like
`set_design_check` in `Lower.h`, then receives the report after the JIT returns (never
inside it, which has no unwind tables).

| Runner | Checker | On a violation |
|---|---|---|
| `clotho_test`, in a build with extra asserts (Debug; CI's linux-x64 and sanitize jobs) | always on, installed by `tests/test_main.cpp` as twin mode is, unless `CLOTHO_NO_TRACE_CHECK=1` | the hook throws unless the running test case is listed, so the test that ran the design fails, naming the rule, time, region, process, control (file:line), cause and last events |
| A design in a child process (`tests/Subprocess.h:spawn_design`) | on, in the test binary | `spawn_design` sets `CLOTHO_TRACE_REPORT=<temp file>`, reads it back and throws in the parent |
| Config matrix (`CLOTHO_CONFIG_MATRIX=1`) | on in every variant | a violation that one variant shows differs from the default's output and is reported under that variant's name |
| `clotho run`, executables of `clotho build` | on in extra-asserts builds | stderr gets a line `clotho: scheduler trace violation:` and the report; the exit status is its own |
| sv-tests nightly (the linux-x64 binary, which has extra asserts) | as `clotho run` | `sv-tests-summary.py` gets a `trace` class, read from the marker in the log before pass and rejected; it fails the job unless `sv-tests-known.txt` lists `trace <test> <reason>` |

Always on in extra-asserts builds, not opt-in: the cost is per design, while the config
matrix's is a process per variant per design (`docs/CI.md`), and CI's linux-x64 job should run
it on every push, as it runs twin mode.

The known list is `tests/trace-check-known.txt`, in the format of
`tests/config-matrix-known.txt`, with the rule first in the reason. `parse_known`
(`tests/ConfigMatrix.h`) would move to a shared header and take the categories as a parameter.

```
# <category> "<doctest test case name>" <rule> <reason, to end of line>
bug "always parks at an intra-body event control" R5.1 B51: the leading control's static entries stay live
```

- `bug`: a Clotho bug with a row in `scheduler-rules.md` §4 or an entry in `docs/TODO.md`.
- `rule`: the LRM allows what the predicate refuses; the entry names the §3 rule to reword.
- An entry covers only its own rule: a violation of another rule still fails the test.
- A listed test that runs without violating its rule fails the run, so the list does not
  outlive its cause.
- After its summary the suite prints `trace check: N runs, E events, P parks, W wakes, V
  violations (K known)` and a line with the NBA updates created by each staging path, the NBA
  passes, the pends, the triggers and the finishes, so a checker that stopped emitting shows as
  zero counts.

## 7. Phases

| Phase | Lands | Acceptance |
|---|---|---|
| 1 | `Trace.h`, `TraceCheck.cpp`, the gate; `spawn`, `park`, `wake`, `dispatch`, `resume`, `end`, `slot`; the control table; R5 with lists checked for signal, event and delay controls and by kind for the rest; the suite hook, child report, known list, CLI marker and sv-tests class | below |
| 2 | `region`, `nba_create`, `nba_pass`, `pend`, `trigger`, `finish`; R12, R20, R24 | With the fix for B9, B22, B38, B40, B41, B42 or B37 reverted in a scratch tree, that bug's regression test fails with the rule named. Creation counts are nonzero for every staging path over the suite. The suite stays silent. |
| 3 | `change`, park hashes, the slot poll; R6, R7 | Reverted fixes for B5, B7, B25 are caught. The `$readmem` and `ref` gaps are `bug` entries if they still exist. The suite stays silent. The nightly's time is measured. |
| 4 | R15, R17, R2, R11, R16, R22, R23 with block entry and exit, R25, S9 member runs; per-record identity for sync, join, `expect` and `await` parks; R14 if the IR marks override stores | Reverted fixes for B2, B15, B23, B24, B27, B29, B39, B43 are caught. The suite stays silent. |
| 5 | R8, R9 once `nba-glitch` merges; the dump for step 4 if wanted | Reverted fixes for B48 to B50 are caught. |

Phase 1 acceptance:

1. **B51 is caught.** On main today, `clotho run` of the §4 design in an extra-asserts
   build ends at t=3 with R5.1: the always at line 3, parked at `@(posedge done)`, woken by
   a `static` posedge of `clk`. With the B51 fix the run prints `4 woke n=1` and reports
   nothing. A checker test also feeds the predicates the B51 event sequence directly, and
   the double-wake sequences of B3, B13, B28 and B44, so the detector stays proven after
   the bug is fixed.
2. **The suite is silent.** `clotho_test` in a `CLOTHO_EXTRA_ASSERTS` build passes with the
   checker on and an empty known list. The summary line shows one run per design simulated
   and nonzero parks and wakes. Every violation found before landing is triaged: a Clotho
   bug gets a §4 row and a `bug` entry, a checker error is fixed. CI's linux-x64 job then
   keeps it silent on every push.
3. **Shipped builds are untouched.** The emitted-IR test passes in both kinds of build, and
   a build without extra asserts has no `rt_trace_` symbol.
4. **Cost is stated.** The commit body gives the suite's time with and without
   `CLOTHO_NO_TRACE_CHECK=1`.

## 8. Decisions (2026-11-25)

1. **Merged events.** Decided in code, not open: a persistent `always @(e)` node (S27)
   exists only when no event variable of the design can be retargeted
   (`lower/Lower.cpp`, the `events_merge` scan); otherwise the list stays in the body and
   each pass re-reads the handle (LRM 15.5.5 as cited there: a re-armed wait uses the
   current object, a parked one stays put). R5 resolves L the same way, so they agree.
2. **Cost.** The checker stays on in extra-asserts builds while it costs the suite at most
   15%; past that it turns opt-in like the config matrix.
3. **`clotho run`.** On by default in extra-asserts builds, so a Debug run can end with a
   violation. Shipped builds never have it: `CLOTHO_EXTRA_ASSERTS` is off outside Debug
   unless asked for (`CMakeLists.txt`), and without it no checker code is compiled.
4. **sv-tests past the harness limit.** Undecided until phase 3 adds R6b; measure first.
5. **R12.** Enforces Clotho's reading where LRM_NOTES 51 leaves the order open; updates
   staged at a `$finish` stay unchecked.
6. **Bugs the checker finds.** Fixed before the phase lands, unless the fix is large; then
   a `bug` entry in `tests/trace-check-known.txt` and a `scheduler-rules.md` §4 row.
7. **Step 4 input.** The reference reads Clotho IR only, never this trace: a reference
   that consumes the implementation's own trace inherits its blind spots. Step 4 compares
   observable output (and later its own trace) against Clotho's.
