# Scheduler trace and rule checker

Status: phases 1 and 2 landed (2026-11-25). Phase 1: `spawn`, `park`, `wake`, `dispatch`,
`resume`, `end` and `slot`, the control table and the R5 checks. Phase 2: `region`, `nba_create`
(`nba_stage` and `nba_release` for timed updates), `nba_pass`, `pend`, `trigger`, `slot_end` and
`finish`, and the checks of R12, R20 and R24; `change` and its rules are phases 3-4, all in
`src/runtime/TraceCheck.cpp`. Measured (Debug, extra asserts, `ctest -j2`, load ~7): 226 s and
230.6 s with the checker, 221 s with `CLOTHO_NO_TRACE_CHECK=1` (+2%, inside the 15% budget); no
violation in 5429 in-process runs, every staging path created at least once. With the fixes
reverted locally, the regression tests of B42 (R12.2), B38 (R12.1), B41 (R20.2), B21 (R20.1) and
B37 (R24.2) fail with the rule named; B40 is covered by a hand-built sequence only. The Release
build without extra asserts was not rebuilt for phase 2. `cli_test` ("-G overrides a type
parameter") fails independent of the trace. Reintroducing B51 is reported as R5.1 at t=3.
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
| `change` | x; bit 0 old and new (0, 1, x, z); `changed`; the facts the code computed; path | inside the store brackets: `Events.cpp:service_wait_queue`, `service_change_notify`, `service_wide_window`, `service_any_waiters`; `Containers.cpp:str_chg_after`, `notify_container_mutation` | 3 |

Fork, join, `disable` and `kill` need no kinds of their own: a fork is a `spawn` per child
and the parent's join park (`join_none` has none); a child's completion is `wake(parent,
child)` from the epilogue of `lower_task`; `disable` and `kill` are `wake(a, force)` from
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
| S15 | an inline `dispatch`, and a wake of the parent or caller while it runs | R5 (1) |
| S17 | any event after `finish` | R24 (2) |
| S18, S19 | `change` with the code's facts; a waiter owed a wake that gets no `change` or no `wake` | R6, R7 (3) |
| S21 | continuous processes are activations like any other | R5 (1), R15 (4) |
| S22, S23, S24 | one `nba_create` per NBA, and what `nba_pass` leaves in the target | R12 (2) |
| S25 | `pend` order against `trigger` order and region | R20 (2) |
| S26 | an inertial `change` at `wheel.posttick` | R16 (4) |
| S27 | a wake by `event` against the object in L; `trigger` | R5 (1), R6 (3) |
| S28 | the time of a `timer` wake against the park's deadline | R2 (4) |

Gated: S6 is `dispatch` plus a park at the entry; S7 and S8 give a group one list; S9 needs
member-run events (phase 4); S11 makes every identity per instance; S20 shows as R7 on a
slot with no z word.

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
| **R6**, phase 3 | **R6a:** after `change(x)` or `trigger(o)`, every `Parked(C)` with an entry the change satisfies (matched on the checker's own bit-0 ranks, not the code's facts) gets a `wake` before the next `dispatch`, `park` or `region`. **R6b:** at each `slot`, a still `Parked(C)` whose entry (x, e) is satisfied by x's change from its bytes at the park to its bytes now is a violation. R6b reads design memory, so it sees stores no bracket reports, and signals the code skipped (S19). | a change that reverts within the slot on an unbracketed path; content behind a handle (B16: a container mutation, with an empty list from the frontend); the `$readmem` and `ref` gaps (`scheduler-rules.md` §5.2), known entries until fixed | B7, B8, B25 (R6b); B12's wake half |
| **R7**, phase 3 | Each fact in `change(x)` equals Table 9-2 on the old and new bit 0, ranking 0 < x = z < 1: `pos` when the rank rises, `neg` when it falls, only 0 and 1 for a 2-state slot. `changed` equals "a plane or the z word differs". | a skipped fact (S19: R6b) | B5 |
| **Later**, phases 4–5 | R14 (needs the override stores marked in the IR: B10, B26, B33, B35, B47); R15 and R17 at slot 0 (B2, B24); R2; R11 and R16; R22; R23 with block occupancy (B27, B29); R25 (B39); S9 member runs (B15); R8 and R9 once `nba-glitch` merges (B48–B50); R26 | | |

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
| 3 | `change`, park hashes, the slot poll; R6, R7 | Reverted fixes for B5, B7, B25 are caught. The `$readmem` and `ref` gaps are `bug` entries. The suite stays silent. The nightly's time is measured. |
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
