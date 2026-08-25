# Concurrent assertions (LRM 16) — frontend scoping

How clotho ingests SystemVerilog concurrent assertions, lowers them through
`libsva` to automata, and emits those automata as native code.

Status: **working end to end for the subset in §10; the region set is
complete.** Clotho compiles, simulates and reports `assert`/`assume`/`cover
property` over an explicit clock or one inferred from the enclosing procedure
(LRM 16.14.6), on the fixtures in `tests/sva/`. Everything outside that subset
still fails loud. `libsva` is complete and committed.

Landed: the CMake integration (§8 — `zig build` from an `add_custom_command`,
clotho's first external build tool); `ir::AssertionMonitor` in `clotho_ir_core`;
`src/Sva.{h,cpp}` as the sole libsva boundary; negation-normal-form booleans
(§5.3); `ir::Activation::Preponed` with sampling into per-variable shadows (§4);
the monitor as a clocked process whose automaton step is **ordinary clotho IR**
(§7); and the reactive region set (§3) — `ir::Activation::Observed` for property
evaluation, and `Inst::Tag::ReactivePend` scheduling action blocks onto
`@clotho.reactive`, with `@clotho.re_inactive` for a `#0` in reactive code.

The region set is complete. **Re-NBA** (LRM 4.4.2.8) commits a nonblocking
assignment made from reactive code, at the same instant; see §3.5.

The split is fixed: clotho owns the SystemVerilog — parsing, clocking, sampling,
4-state collapse, diagnostics, code generation. `libsva` owns the temporal
algebra — sequence and property operators, automaton construction, the per-cycle
protocol. `libsva` is SystemVerilog-agnostic and never sees a width, a clock
edge, or an X.

---

## 1. What libsva provides, exactly

`libsva` is a **build-time dependency of the compiler, not a runtime dependency
of the simulation.** No `libsva` code runs in a simulated process; it is called
during lowering and its output is baked into the emitted module as constants
(§7). Its C API is `../libsva/include/sva.h`, linked against `libsva.a`.

**The IR (built by clotho).** 18 node tags in three layers
(`../libsva/src/Ir.zig:234-350`):

| Layer | Tags |
|---|---|
| Boolean | `bool_const`, `atom`, `bnot`, `band`, `bor` |
| Sequence | `seq_boolean`, `seq_delay`, `seq_or`, `seq_and`, `seq_intersect`, `seq_throughout`, `seq_repeat_consec`, `seq_repeat_goto`, `seq_repeat_nonconsec` |
| Property | `prop_seq_weak`, `prop_seq_strong`, `prop_implies_overlap`, `prop_implies_nonoverlap` |

That set is exact: no `first_match`, and no `nexttime`/`always`/`until` family.
`within` is absent from it too but is not a gap — it is sugar, which
`sva_seq_within` builds out of `seq_delay` and `seq_intersect` so that nothing
downstream has to learn a new tag (§9.2). What clotho cannot express, clotho
rejects (§9). Nothing in the tree is optional — every edge is present, a leading
delay is `1'b1 ##[lo:hi] x`, and an assertion that never resets carries a
constant false where the reset condition goes.

**An atom** is an opaque dense `uint32_t` clotho assigns. `libsva` never
evaluates one, never samples, never knows a width or a 4-state value. Atoms are
*already-sampled booleans presented once per tick*. Everything about sampling is
on clotho's side of that sentence.

**A monitor** (`sva_lower`) is one *obligation* automaton plus, for an
implication, a *trigger* automaton, over a shared alphabet:

| Accessor | Meaning |
|---|---|
| `sva_monitor_roots` / `_root_count` | Boolean *expression* nodes, one bit each, evaluated once per cycle |
| `sva_bool_read` | Walks a root back to `band`/`bor`/`bnot`/`atom`/`bool_const` |
| `sva_monitor_guard` | Interned cube; literals are `root << 1 \| negated`. Guard `0` is empty and always holds, guard `1` is the reserved unsatisfiable cube |
| `sva_automaton_guards` | One guard index per state — the automaton is *homogeneous*, so a guard belongs to a state, not an edge |
| `sva_automaton_guard_states` | Per guard, the mask of states carrying it. This is what lets `satisfy` fold over guards instead of gathering per state |
| `sva_automaton_follow` / `_first` / `_last` | Bit-set rows over states |
| `sva_monitor_depth` | Obligation attempts pending at once = state vectors to allocate |
| `sva_monitor_strength` | What a pending attempt does when the trace ends |
| `sva_monitor_reset` | Root index of the reset condition |

**The per-cycle protocol** is documented at `../libsva/src/Monitor.zig:27-47` and
implemented as an executable reference at `:201-260`. Clotho's codegen reproduces
it; it never calls it. `../libsva/test/backend.c` is a compiling backend built
against the same tables and differential-tested against that reference — it is
the worked example §7 follows.

**Failures** split two ways:

- **Legal input libsva cannot lower yet** returns a status naming the offending
  node (`SVA_ERROR_UNBOUNDED_OBLIGATION`, `_UNSUPPORTED_CONSEQUENT`,
  `_STATE_LIMIT`, `_DEPTH_LIMIT`) — a property of the user's source, so clotho
  maps the node back to a `SourceSpan` and reports a diagnostic (§7).
- **A malformed tree aborts.** Wrong operand layer, `lo > hi`, `lo == 0` on a
  repetition, goto over a compound sequence. That is a bug in clotho's lowering
  with no user source to blame, so clotho must never construct one — every such
  shape is rejected in the frontend *before* the builder is called (§9.1).

---

## 2. What clotho has today

Nothing. Verified by grep over `src/`: `ConcurrentAssertion`, `AssertionExpr`,
`AssertionInstance`, `ClockingEvent`, `SequenceSymbol`, `PropertySymbol`,
`ClockingBlockSymbol`, `CheckerSymbol`, `isFromAssertion`, `$rose`, `$fell`,
`$stable`, `$changed`, `$past`, `$sampled`, `$assertoff`, `libsva`, `sva_` —
**none appear.**

Every SVA construct dies in a generic `default:` arm:

| Construct | slang node | Site | Message |
|---|---|---|---|
| `assert`/`assume`/`cover`/`restrict`/`expect property` | `ConcurrentAssertionStatement` | `src/Lower.cpp:5761` | `unsupported statement` |
| `sequence` / `property` / `let` / `clocking` / `checker` decl | `SymbolKind::{Sequence,Property,LetDecl,ClockingBlock,Checker}` | `src/Lower.cpp:963` | `unsupported module member kind=…` |
| checker instance array | — | `src/Lower.cpp:1033` | `unsupported instance array element kind=…` |
| `$rose`/`$fell`/`$stable`/`$changed`/`$past`/`$sampled` | `CallExpression` | `src/Lower.cpp:9040` | `unsupported system function '$rose'` |
| `$assertoff` family | `CallExpression` | `src/Lower.cpp:7887` | `unsupported system task '$assertoff'` |
| named sequence/property *use* | `ExpressionKind::AssertionInstance` | `src/Lower.cpp:11025` | `unsupported expression kind=AssertionInstance` |
| `@(posedge clk)` as an expression | `ExpressionKind::ClockingEvent` | `src/Lower.cpp:11025` | `unsupported expression kind=ClockingEvent` |

Two corrections to earlier notes, because they change where the work goes:

1. **`src/Lower.cpp:5761` is the `default:` arm of `lower_stmt`, not a
   `case StatementKind::ConcurrentAssertion`.** That case label does not exist.
2. **A module-item `assert property` never reaches the module-member switch.**
   slang wraps `ConcurrentAssertionMemberSyntax` in a
   `ProceduralBlockSymbol(Always, isFromAssertion = true)`
   (`build/_deps/slang-src/source/ast/symbols/BlockSymbols.cpp:433-437`), so it
   arrives via `lower_procedural_block` (`src/Lower.cpp:5182`) → `lower_forever`
   (`:6351`) → `lower_stmt` (`:5632`) → `:5761`. **No new module-member case is
   needed**; the whole structural seam is `lower_procedural_block` +
   `lower_stmt`. Clotho never reads `isFromAssertion`.

`src/Lower.cpp:5963` (`only immediate assert / assume / cover are supported`) is
on `ImmediateAssertionStatement` and is **unreachable from any concurrent form**.
Its TODO at `:5961-5962` must be split accordingly.

### 2.1 What slang precomputes, free

From `build/_deps/slang-src/include/slang/ast/expressions/AssertionExpr.h`:

| Facility | Line | Use |
|---|---|---|
| `SequenceRange{min, optional<max>}` | `:90` | `max` unset *is* libsva's unbounded bound |
| `NondegeneracyCheckResult` | `:155` | `AdmitsEmpty`/`AcceptsOnlyEmpty`/`AdmitsNoMatch` + **`noMatchRange`**, the exact span of the offender |
| `checkNondegeneracy()` | `:184` | Diagnoses the degenerate sequences libsva bans, with a location |
| `computeSequenceLength()` | `:187` | min/max ticks |
| `canSucceedVacuously()` | `:191` | Vacuity, for `cover` reporting |
| `isEquivalentTo()` | `:135` | Structural equality over assertion trees |
| `BinaryAssertionExpr::opRange` | `:486` | Per-operator span for a targeted diagnostic |

`AnalyzedProcedure::getInferredClock()`
(`build/_deps/slang-src/include/slang/analysis/AnalyzedProcedure.h:103`)
implements LRM 16.14.6 clock inference, and clotho now reads it. There was no
ordering problem to solve: the `AnalysisManager` that produces the dataflow
diagnostics already runs in `lower_compilation` *before* the AST walk. What was
missing was a channel from it to the walk, and a way to make its answer apply to
the instance being lowered.

The channel is an `AnalyzedAssertion` listener registered before `analyze()`,
filling an `InferredClockMap`. Collecting during the pass beats querying after
it: `AnalysisManager::getAnalyzedAssertions` is keyed by containing symbol, which
the lowering would have to rediscover.

**The table is keyed and valued in syntax, not AST, and that is the load-bearing
part.** slang analyses one instance body and skips every body that is an exact
duplicate of it (`InstanceSymbol::getCanonicalBody`), while clotho lowers each
instance's own body — distinct `Statement` and `ValueSymbol` objects per
instance. An AST-keyed table misses every duplicate, and its AST values name the
analysed instance's `clk` rather than this one's. Syntax is shared by
construction, so the *verdict* — which event control slang picked — transfers,
and `lower_concurrent_assertion` re-resolves it against `proc_timing_`, the event
control of the procedure it is lowering right now. Two consequences:

- The `AnalysisManager` still dies with the diagnostics block. Nothing in the
  table outlives it, so there is no lifetime coupling and no memory held open —
  which matters, because `ClockInference::expand` can synthesise a
  `TimingControl` into the analysis arena and an AST-valued table would have had
  to pin the whole manager to stay valid.
- The resolution searches one level into an event list, so
  `always @(posedge clk or negedge rst_n)` — where 16.14.6 picks `posedge clk`
  because the body reads `rst_n` — works. A verdict that resolves to *nothing* in
  the local procedure (a `default clocking` block, a `$inferred_clock` expansion,
  an event control buried in the body rather than leading it) is refused with its
  own message rather than approximated. So is a source location whose two
  elaborations disagreed.

`tests/sva/diff_inferclk.sv` is the fixture for this seam: three unclocked
properties, in a posedge procedure, a negedge procedure on the same clock, and a
reset flop. The reports separate in time only if the inferred *edge* is honoured
and not just the signal, and the reset flop's report must coincide with the
posedge one — down to which source line each failure is attributed to.

`AnalyzedAssertion::getSemanticLeadingClock()` is the other half of this seam and
is still unread: it resolves the leading clock of a multi-clocked property, which
clotho refuses (§9.3).

`AssertionInstanceExpression::body`
(`build/_deps/slang-src/include/slang/ast/expressions/MiscExpressions.h:235`) is
the **already-expanded** body with actuals substituted, so named `sequence`/
`property` declarations with arguments cost a pointer dereference rather than a
substitution pass.

---

## 3. Scheduling regions

This is the largest part of the work, and it is not optional: a concurrent
assertion is *defined* in terms of regions clotho does not have.

### 3.1 What the LRM requires

LRM 4.4.2 orders the simulation regions of a time slot: **Preponed, Active,
Inactive, NBA, Observed, Reactive, Re-Inactive, Re-NBA, Postponed.** Three
clauses are load-bearing here, quoted:

- **4.4.2.1 Preponed** — "*Sampling in the Preponed region is equivalent to
  sampling in the previous Postponed region.*"
- **4.4.2.5 Observed** — "*The Observed region is for evaluation of property
  expressions when they are triggered. During property evaluation, pass/fail code
  shall be scheduled in the Reactive region of the current time slot.*"
- **4.4.2.6 Reactive** — "*The code specified by blocking assignments in
  checkers, program blocks and the code in action blocks of concurrent assertions
  are scheduled in the Reactive region.*"

So the mapping is fixed and three-way: **sample in Preponed, evaluate the
property in Observed, run the action block in Reactive.** Re-Inactive and Re-NBA
are the reactive-set duals of Inactive and NBA, for `#0` and `<=` inside reactive
code.

### 3.2 What clotho has

There is **no region type at all.** Regions are LLVM basic blocks inside one
JIT'd function `sim_drain`, built by `emit_active_run`
(`src/ir/LlvmBackend.cpp`). The reactive set is complete:

| LRM region | clotho | Where |
|---|---|---|
| Preponed | `Activation::Preponed`, run once per time step | `emit_preponed_run` |
| Active | ring FIFO `@clotho.active`, drained | `active.check` |
| Inactive | batch promote into the empty Active ring | `inactive.check` |
| NBA | block-commit chain | `nba.check` |
| Observed | `Activation::Observed`, runs process bodies | `emit_observed_run` |
| Reactive | ring FIFO `@clotho.reactive`, drained | `reactive.check` |
| Re-Inactive | batch promote into the empty Reactive ring | `re_inactive.check` |
| Re-NBA | commit staged updates, re-drain Reactive | `re_nba.check` |
| Postponed | flush + VCD + `$strobe` + `$monitor` | after `reactive.done` |

Every part of the reactive set is gated on the design actually using it, so a
design with no assertions and no reactive code emits none of it — verified by
diffing `--emit llvm` and `--emit llvm-raw` against the pre-change compiler and
getting byte-identical output.

### 3.3 What to build

**Sampling goes in the existing Postponed region, not a new Preponed one.** LRM
4.4.2.1 declares the two equivalent, and clotho's Postponed already exists at the
settled point and is already where VCD samples (`rt_vcd_flush` is documented as a
"Postponed-region sample", `src/runtime/Runtime.h:481-485`). That removes one new
region from the scope entirely. Two consequences to handle:

- Time zero has no previous Postponed, so `emit_main` takes one initial sample
  after `init_state()` (`src/ir/LlvmBackend.cpp:3952`). This also supplies the
  *default sampled value* LRM 16.5.1 requires for value-change functions called
  at or before the first clocking event.
- LRM 4.4.2.9 forbids value changes in Postponed. Clotho's shadows are
  compiler-internal hidden signals that generate no events (the `PreInit` store
  discipline, `src/ir/Module.h:92-95`), so no *design* net changes. Worth
  recording as a deliberate reading rather than passing over in silence.

The alternative hook, if a real Preponed region is ever wanted, is
`sim_drain`'s `wheel.inertial` block (`src/ir/LlvmBackend.cpp:7014-7019`): it runs
after `rt_wheel_advance` fires timers but before any body executes, which is
exactly the Preponed position. It is strictly more work for the same result.

**Observed becomes a real region that runs bodies.** Monitor evaluation is a
process that must execute there, not a flush. This was the one genuinely new
capability, and it is now `ir::Activation::Observed`, run by `emit_observed_run`
at the settled point.

**Reactive, Re-Inactive and Re-NBA are new**, and structurally they are a second
copy of the Active/Inactive/NBA triad that `emit_active_run` already builds. This
document proposed parameterizing that drain over a region set rather than writing
it twice. **As built it is a second copy, deliberately.** The two drains agree in
shape but not in detail — the reactive ring has no self_start seeding and no NBA
phase, and its `$finish` and oscillation handling join the active ones rather than
duplicating them — so a region parameter would have had to be threaded through a
dozen branch points to share roughly thirty lines. Re-NBA is the exception: it
shares the Active NBA region's commit through `emit_nba_commit`, because the
commit body is straight-line and position-independent, with no branch points to
thread.

**This fixes gaps clotho already tracks**, which is the argument for doing it
properly rather than approximating:

- `docs/TODO.md:750` — deferred immediate assertions: "The observed/final
  distinction also needs a Reactive region to fully realize."
- `docs/TODO.md:853` — `program`/`endprogram` and `$exit` need the reactive set.
- LRM 16.17 `expect` needs it too.

**Immediate assertions are already correct and stay put.** LRM 16.3 immediate
assertions are procedural: the action block runs inline in whatever region the
process is executing. Deferred immediate (LRM 16.4) reports mature in Observed
(`#0`) or Postponed (`final`), which is what `rt_defer_report` /
`rt_observed_flush` / `rt_defer_flush_final` already do
(`src/ir/LlvmBackend.cpp:6929-6939`). What changes for them is only that the
Reactive region's existence lets clotho close the `docs/TODO.md:750` gap.

### 3.4 Where a monitor runs

A monitor is stepped once per clock tick. The trigger for that tick is an
ordinary edge on the clock signal, which clotho already detects at the clock
writer's store site (`emit_edge_facts`, `src/ir/LlvmBackend.cpp:6019`), including
the 4-state edge lattice `0 < x/z < 1` at `:6031-6041` — so `@(posedge clk)` from
X to 1 is an edge, per LRM, for free.

The monitor therefore is an `ir::Process` whose sensitivity is
`{{clk, Edge::Positive}}`, reusing static wait buckets, subscription order,
fusion, sched-flag wake dedup, and per-instance state addressing with no new
mechanism — but with `activation` selecting the **Observed** region rather than
Active. That is a new `ir::Activation` value (`src/ir/Module.h:92`);
`-Werror=switch` (`CMakeLists.txt:34`) then forces every activation switch to
acknowledge it instead of silently skipping it.

Because the monitor reads *sampled* values rather than live signals, its position
relative to other processes cannot affect its verdict. That is the whole point of
sampling, and it is why getting §4 right is what makes the rest safe.

### 3.5 Two deviations, now closed

Both were consequences of the reactive region set not existing. Both are fixed;
the record is kept because the reasoning is what justified shipping without them.

**Evaluation ran in Active, not Observed.** LRM 4.4.2.5 puts property evaluation
in Observed; clotho's monitor was an ordinary `Always` process on the clock edge.
That was sound *because of* §4 — the monitor reads only sampled shadows, so its
verdict could not depend on where in the instant it ran — and it would have become
observable only once reactive code could perturb a monitor's inputs.

**Now:** the monitor is `ir::Activation::Observed`, run by `emit_observed_run` at
the settled point. It is *not* subscribed to its clock and no event wakes it: it
runs once per settled point and detects its own clocking event by comparing the
sampled clock against a one-slot history. That is deliberate, not a shortcut. A
static subscription fires from a live store in the Active region, so subscribing
would trigger on the **live** clock edge, and LRM 16.5 requires the clocking event
to be evaluated on **sampled** values. Running unconditionally and testing the
sampled edge in the body is what keeps triggering in the sampled domain. Nothing
is lost by ramping a fresh coroutine each time: the automaton state, attempt slots
and clock history are per-instance design signals, so the frame holds nothing that
must survive the instant.

Because an Observed body writes ordinary design signals, it can wake an Active
process. The drain therefore re-settles the active set before Reactive whenever
the Observed run leaves the active queue non-empty, which is what the LRM's
time-slot loop does; re-running the bodies on the way back is harmless, since the
sampled values they trigger on cannot change within the step.

**Reports flushed in Observed, not Reactive.** LRM 4.4.2.6 puts action blocks in
Reactive; clotho emitted the default failure report through the deferred queue
(`defer = 1`), so it matured in Observed — one region early — and a user-written
action block was refused outright.

**Now:** the report is the implicit fail action (LRM 16.14.1) and takes the same
route as a written one — a synthesized zero-formal task scheduled by
`Inst::Tag::ReactivePend` onto `@clotho.reactive` and run by the reactive drain,
never inline. User-written action blocks work. Observable output ordering is
unchanged: the `tests/sva/` fixtures still report the same cycles they did
before.

**Re-NBA is now built too** (LRM 4.4.2.8). The NBA commit machinery was welded
into `emit_active_run`'s `nba.commit` block; it is now `emit_nba_commit`, which
emits at the builder's insert point and leaves the block unterminated so the
caller supplies the branch, with `emit_nba_any_staged` as the matching predicate.
`nba.commit` calls it and loops back to the active drain; `re_nba.commit` calls
it and loops back to the reactive one. The extraction was verified
behaviour-preserving by diffing `--emit llvm-raw` before and after over designs
covering every commit path — block-commit scalars, array and aggregate replays, a
queue whole-assign, an intra-assignment record and a `->> e` fire — and getting
byte-identical IR.

Sharing one set of staging structures between the two commits is safe because the
commit *consumes* them: the block chain head is nulled before the chain runs,
`rt_nba_arr_reset` empties the array / aggregate / queue records, each intra site
returns its records to its pool, and every `->> e` flag is cleared as it fires. So
the predicate re-arms, and the Re-NBA commit sees only what reactive code staged
after the NBA commit — it can neither re-apply an active-region update nor drop a
reactive one. `tests/sva/diff_renba_double.sv` pins that: a counter incremented by
an NBA in Active and an array element written by an NBA in Reactive both advance
exactly once per instant.

A Re-NBA commit services wait queues onto the **active** ring, so the reactive
drain re-settles the active set before falling through to Postponed, the outer
loop of LRM 4.4.2. Without it a process woken by a reactive NBA would not run
until the wheel had advanced — a full time step late, which the fixture catches.
A `#0` in reactive code is supported by Re-Inactive, as before.

---

## 4. Sampling: shadow the variables, evaluate in Observed

**What is sampled is a variable, not an expression.** LRM 16.6 makes the
separation explicit while describing a corner of it:

> Elements of dynamic arrays, queues, and associative arrays that are **sampled
> for assertion expression evaluation** may get removed from the array or the
> array may get resized **before the assertion expression is evaluated**.

Sampling captures operands; evaluation of the expression — comparisons, boolean
structure, everything — happens later, in Observed (§3). So the design is:

- **Shadow** — one per *variable* referenced by any assertion, of the same type:
  full width, full 4-state. Written in Postponed (§3.3) by a plain **copy**, with
  no expression evaluation at all. Shared by every assertion mentioning it.
- **Evaluation** — the monitor process, in Observed, evaluates each root from
  shadows. Every operator lives here.

An earlier revision of this document had this wrong: it evaluated whole boolean
expressions in the sampling region and stored a bit per expression. That is
*equivalent* only when the expression is a pure function of sampled variables, and
it is the wrong model — it makes the sampling region do work the LRM puts in
Observed, needs a purity argument clotho cannot yet make, and stores per-assertion
state where per-variable state is shared.

The shadow is a third instance of a pattern clotho already has twice: the NBA
shadow (`Anchor::shadow_ty`/`shadow_gv`, `src/ir/LlvmBackend.cpp:566-568`) and
the VCD shadow (`VcdVar::shadow`, `src/runtime/Runtime.cpp:4350-4354`). The
shadow set is exactly "variables referenced by some assertion expression",
computed in the frontend, and assertion-expression lowering resolves a signal
through a `Signal -> Signal` shadow map rather than reading the live slot.

Two consequences worth naming:

- A dynamic array, queue, or associative-array element in an assertion expression
  must have its *element value* captured, per the clause above, not a reference
  into a container that may be resized. Day one rejects them (§9.3).
- A function call in an assertion expression reads state that is not in the
  shadow set unless clotho traces into the body. Day one rejects those too, which
  is required anyway for the LRM's side-effect-freedom rule.

### 4.1 Sampled-value functions

**Landed.** All six of LRM 16.9.3 — `$past`, `$sampled`, `$rose`, `$fell`,
`$stable`, `$changed` — lower, with fixtures under `tests/sva/` exercising them
(`diff_past.sv`, `diff_valuechange.sv`).

History is per *clock tick*, not per time step. LRM 16.9.3, verbatim: a value
change function compares "*the sampled value of the expression with the sampled
value of the expression from the most recent strictly prior time step in which the
clocking event occurred*."

That single sentence is what splits the implementation in two, because the
Preponed shadow of §4 is rewritten every *time step*:

- **Preponed, per time step** — writes `sva.shadow.<v>`, unchanged by this work.
- **The monitor, per clock tick** — shifts a **history ring** per variable.

`$past` is the primitive and the rest desugar onto it:

| Function | Lowering |
|---|---|
| `$sampled(v)` | the shadow itself; no ring entry |
| `$past(v)` | `$past(v, 1)` |
| `$past(v, n)` | `ring[n - 1]` |
| `$stable(v)` | `shadow === ring[0]`, whole value, all four states |
| `$changed(v)` | the complement of that |
| `$rose(v)` / `$fell(v)` | LSB moved up / down the order `0 < x/z < 1` |

**Ring depth** is `max n` over every `$past(v, n)` on that variable, so it is a
compile-time constant — a non-constant `n` is refused, and entries are created on
demand, which makes the ring's own length the depth. `$sampled` creates none.
Each entry carries the variable's own type, full width and full 4-state, for the
same reason the shadow does.

**The ring is per monitor, not per variable**, which is correctness rather than
bookkeeping: `$past(v)` under `@(posedge a)` and under `@(posedge b)` are
different values, and one shared ring could answer only one of them. It also puts
the reads and the shift in a single straight-line process body, which is what
makes the next point checkable by inspection.

**Shift order.** The shift is emitted at the very *end* of the monitor body, after
the roots have read the ring, and copies **descending** (`ring[k] = ring[k-1]`
down to `k = 1`, then `ring[0] = shadow`). Both halves are load-bearing: shifting
before evaluation would make `ring[0]` this tick's own shadow and put every
function here off by one tick, and copying ascending would smear `ring[0]` through
every entry above it, because stores in a process body are blocking. It is gated
on the clocking **edge alone and not on `disable iff`** — 16.9.3 counts ticks on
which the clocking event occurred, which a disable does not stop, the same reason
the monitor's own `clkprev` store is ungated.

`$rose`/`$fell` reuse the backend's 4-state edge lattice from `emit_edge_facts`
rather than inventing one, so an assertion edge and a procedural `@(posedge)` can
never disagree: a rise is a strict increase in `0 < x/z < 1`, which makes `x -> 1`
a rise and `x -> x` no edge at all.

#### The default sampled value

A tick with no predecessor at the requested depth reads the **variable's
time-zero value**, *not* the default of its type. The two only differ when the
variable has a declaration initializer:

```systemverilog
logic [3:0] v = 4'd7;    // time-zero value 7; type default x
initial #2 v = 4'd3;     // so the first tick's sample (3) differs from both
```

LRM 16.5.1 requires a default sampled value for a function called at or before
the first clocking event, and §3.3's initial Preponed sample is what supplies
it. The ring takes that same value rather than a second, differently-defined
one, so it is pre-filled at every depth with the time-zero sample: at the first
tick `$past(v)`, `$past(v,2)` and `$past(v,3)` are all `0111`, and at the second
they are `0011`, `0111`, `0111`. An uninitialised `logic` reads `x` under the
same rule, since its time-zero value is `x` — which is why leaving the ring at
its power-up state *looks* right and is wrong for any initialized variable.

So each entry is seeded by appending a copy to the **PreInit** process, after the
declaration initializers and therefore before `emit_main`'s initial Preponed
sample (§3.3) — the ordering that makes the seed equal to what that first sample
will capture.

#### Refused, loudly

`$past` with a gating expression or an explicit clocking argument; an explicit
clocking argument on any of the four value change functions; a non-constant or
sub-1 tick count; any of the six applied to an expression rather than a plain
variable; and any of them outside an assertion expression. In a `disable iff` or
asynchronous abort condition only **`$sampled`** lowers, which is exactly what the
LRM allows there: 16.9.3 can infer no clocking event in that position ("it shall
be explicitly clocked") and 16.12.14 repeats the rule for an abort condition
("when sampled value functions other than `$sampled` are used in the abort
condition, the clock argument shall be explicitly specified"), so the other five
are legal there only with an explicit clocking argument — the gap above. slang
diagnoses the unclocked disable-condition half itself
(`analysis/ClockInference.cpp`); the abort half clotho refuses. Each has a unit
test.

### 4.2 Cross-scope sampling

**Landed.** A shadow used to be forced into the scope lowering the assertion:
`sampled_shadow_of` refused a live signal that was not itself in
`scope_stack_.back()->signals`, so a hierarchical reference through a
directly-instantiated interface (`dif.sig`, one hop from the asserting
scope, or any deeper instance path) or a package / `$unit` variable was
refused outright — "a concurrent assertion may only sample variables of the
instance that declares it". This was the single highest-impact SVA gap
against real UVM-flavoured SystemVerilog: 16 of 16 sv-tests chapter-16
concurrent-assertion fixtures share exactly this shape, sampling through a
directly-instantiated interface handed to the DUT and to the checker as a
plain port connection.

**The fix moves where the shadow's storage is registered, not how it is
read.** A shadow is per-instance state and belongs beside the variable it
shadows — the scope that DECLARES `live`, which need not be the scope
lowering the assertion. That was always the intended fix (the guard's own
`TODO` said so); what had not been established was whether reading a
relocated shadow from a different asserting scope needed new addressing
machinery. It does not, for a structural reason worth stating precisely:

- **Signal handles are flat and globally unique** across the whole design
  (`ir::InstanceScope`'s own doc comment, `src/ir/Module.h:75-78`). The scope
  tree exists so the backend can group signals into per-instance state, not
  to make a signal's *identity* scope-relative.
- **Every `InstanceScope` occurrence gets its own LLVM global**
  (`Anchor::state_gv`, `@clotho.inst.<hier>.state`,
  `src/ir/LlvmBackend.cpp:710-713`, built by `emit_state_global`). A `Load`/
  `Store` of a signal addresses it through ITS OWN declaring scope's anchor,
  looked up from the signal's own recorded home
  (`build_state_types`/`scope_type`), never from "the scope of the process
  currently executing". `Process::scope` exists for a different reason
  entirely — it is the key the state-type-deduplication work uses to decide
  whether two structurally-identical processes across sibling instances can
  share one compiled function body, addressed through a runtime anchor
  parameter instead of a baked-in global (`Anchor::indirect`,
  `src/ir/LlvmBackend.cpp:3833-3841`) — and an ordinary hierarchical read
  already exercises exactly this path when the reading process is deduped,
  since `dif.sig` read from a deduped `always` block already had to work.

So the ONLY thing standing in the way was a frontend bookkeeping choice: the
shadow's `ir::SignalInfo` was always registered while `scope_stack_` pointed
at the asserting instance, attaching it to the wrong `InstanceScope::signals`
list. The fix:

1. `register_signal` now also fills `signal_scope_`, its own inverse (Signal
   id -> declaring `InstanceScope::id`), alongside the forward attachment it
   already did.
2. `sampled_shadow_of` looks `live` up in `signal_scope_`. Found: the
   declaring scope is `find_scope_node(top_scope_, id)` — the SAME lookup
   `alloc_assert_control` already used to attach a different piece of
   per-assertion state (LRM 20.12 control masks) to an arbitrary scope, safe
   for the same reason: Collect has already finished and frozen the scope
   tree by the time either pass runs (`alloc_assert_control`'s own comment;
   `sampled_shadow_of` runs later still, during Emit). Not found: `live` was
   registered with `scope_stack_` empty, which only the standalone globals
   Lowerer does — a package / `$unit` variable.
3. The shadow is registered by temporarily pushing the declaring scope (or,
   for the package / `$unit` case, calling `module_.add_signal` directly, so
   it stays unscoped exactly as `live` itself is). A null declaring scope
   therefore does not need a synthetic home: `build_state_types` already
   treats a signal claimed by no `InstanceScope` as a flat root-level field,
   which is exactly where a package / `$unit` live signal already lives.

No change was needed to how a shadow is READ. Atom lowering, `$sampled`, and
the `$past` ring all resolve a shadow through `sva_shadow_of` and then emit an
ordinary `Load`, exactly as before; the relocation is invisible to them
because addressing was never about the reader's scope to begin with.

**Both invariants from §4's design hold, and neither needed new machinery to
keep holding:**

- **One shadow per live signal, shared across every asserting scope.** The
  `sva_shadows_` memoization at the top of `sampled_shadow_of` was always
  keyed on `live` alone, never on the asserting scope — it simply could not
  be exercised across scopes before, because a second asserting scope could
  never get past the guard to ask a second time. Pinned by a fixture with two
  different instances of one module (so this also exercises the shadow
  surviving state-type deduplication, since both instances have structurally
  identical monitors), both sampling one interface's `sig`: `shadow_count`
  reads 1, not 2.
- **The Preponed copy still runs exactly once per timestep.** No declaring
  scope needed a Preponed process of its own to attach a copy to, because
  there is only ever ONE — `finalize_preponed` builds a single,
  per-top process tagged `Activation::Preponed` with `scope = Process::no_scope`
  (never a specific instance), iterating `sva_shadow_pairs_` and emitting one
  `Store(shadow, Load(live))` per PAIR, in creation order — which the
  `sva_shadows_` memoization already keeps to one entry per live signal. The
  interface-instance and package/`$unit` cases needed no new Preponed
  process because the existing one was already unscoped and already
  addressed arbitrary signals directly, never relative to "the asserting
  scope". The same two-instance fixture above counts exactly one `Store` in
  the whole module's one Preponed process.

**Depth is not narrowed.** `find_scope_node` walks the whole scope tree from
`top_scope_`, not one level from the asserting scope, so a second hierarchy
hop (`assert` in `A` sampling a signal inside `A.b.c`, where `b` instantiates
`c`) costs nothing extra — verified with exactly that fixture.

**One more, narrower gap sat behind this one.** `$past`/`$rose`/`$fell`/
`$stable`/`$changed`'s own "plain variable" operand check (§4.1) tested only
`operand.kind == ExpressionKind::NamedValue`, never
`ExpressionKind::HierarchicalValue` — the kind slang gives a dotted path like
`dif.sig`, distinct from a bare name, everywhere else in `Lower.cpp` checked
alongside `NamedValue` wherever a hierarchical reference is legal. Sampled-
value functions had never needed the distinction before, because
`sampled_shadow_of` refused any hierarchical `live` upstream regardless.
Widened to accept both kinds, cast through the common `ValueExpressionBase`
rather than `NamedValueExpression` specifically, matching the pattern every
other hierarchical-reference site already uses.

**`sampled_shadow_of`'s other caller is unaffected.** A clocking block
input's `#1step` skew copy (LRM 14.4, §14.4 note in the function's own
comment) shares the same relocation fix by construction — it is the same
function — but no cross-scope clockvar-input fixture exists in
`tests/clocking_test.cpp` and none was added here, so that caller's
supported scope is unchanged pending its own coverage rather than assumed
correct.

**Still refused, loudly, with its own message and its own `TODO`:** a
sampled variable without a packed scalar value — a string, an event, an
unpacked aggregate, or a dynamic array / queue / associative array — LRM
16.6's "sample the element value" clause, unrelated to scope and untouched by
this work.

Verified against Verilator 5.050 for the interface-hierarchical case's report
timing, and against sv-tests: all 16 chapter-16 fixtures of the refused shape
now compile and simulate; every other chapter-16 refusal left is an unrelated
pre-existing gap (LRM 16.10 local variables in a sequence / property, LRM
16.13 multiclock sequences, LRM 16.4 a deferred assertion inside a function /
task). Tests: `tests/sva_test.cpp`.

---

## 5. Booleans, X/Z, and atoms

### 5.1 A boolean is reduced, not bit-blasted

An SVA boolean is an arbitrary SystemVerilog expression. Clotho evaluates it at
**full width** with ordinary `lower_expr` over shadows, then reduces the result to
one bit — nothing is bit-blasted, and no per-bit atom is ever created. LRM 16.6:

> The outcome of the evaluation of an expression is Boolean and is interpreted the
> same way as an expression is interpreted in the condition of a procedural `if`
> statement. In other words, if the expression evaluates to x, z, or 0, then it is
> interpreted as being false. Otherwise, it is true.

"Interpreted the same way as … a procedural `if`" is a direct instruction to reuse
the existing path, and clotho's is exactly right: `lower_predicate`
(`src/Lower.cpp:5814`) emits `ir::Inst::Tag::RedOr` over the lowered expression,
and the backend's `Cond` tests `must_of(v) != 0`
(`src/ir/LlvmBackend.cpp:16353-16355`, commented "must=1 holds only for a definite
1 … so X/Z fall through to else automatically"). `must(RedOr(v))` is "some bit of
`v` is definitely 1" — confirmed against iverilog: `if (4'b001x)` is taken,
because a definite 1 bit makes the value nonzero regardless of the X.

So an SVA boolean leaf costs one `RedOr` on top of the expression itself, and SVA
booleans reuse `lower_predicate` verbatim.

### 5.2 X/Z makes booleans non-complementary

Because unknown collapses to false, SVA booleans are **not complemented**: under
X both `a` and `!a` are false, so `a || !a` does not hold. Verified with iverilog,
which models X — Verilator does not, and reads a signal initialised to `1'bx` as
`0` (§11):

| expression, `a = 1'bx` | value | boolean verdict |
|---|---|---|
| `a` | `x` | false |
| `!a` | `x` | **false** |
| `a \|\| !a` | `x` | **false** — not a tautology |
| `a && 1'b0` | `0` | false |
| `a \|\| 1'b1` | `1` | true |

Write `c(v)` for the collapse. Which SV operators commute with `c` decides which
may be handed to libsva as structure:

- **`&&` and `||` do.** `c(a && b) = c(a) & c(b)` and `c(a || b) = c(a) | c(b)`:
  an unknown operand can never turn a known outcome around, which is what the last
  two table rows show.
- **`!` does not.** `c(!a) = 1` iff `a` is definitely **zero**, whereas
  `~c(a) = 1` iff `a` is *not* definitely one — which includes X.

### 5.3 Negation normal form, and no special cases

An earlier revision handled this by checking the operand's domain and emitting
libsva's `bnot` only for 2-state terms. That is operator special-casing, and it is
the wrong shape: it makes correctness depend on a domain query, leaves a trap for
the 4-state path, and buys only cube quality.

The uniform answer is **negation normal form**. Clotho rewrites the boolean
expression so negation appears only on leaves, then maps it structurally:

| SV | libsva |
|---|---|
| `a && b` | `band` |
| `a \|\| b` | `bor` |
| `!(a && b)` | `bor` of the negated operands (De Morgan) |
| `!(a \|\| b)` | `band` of the negated operands |
| `!!a` | `a` |
| anything else, in the polarity it appears | `atom` |

A negated leaf is not a special form: it is the atom of the **SV-negated
expression**, lowered by the same `RedOr` + `must` path as any other leaf. So
`!(count > 5)` is one atom whose expression is `!(count > 5)`. There is no plane
reasoning, no domain query, and **clotho never emits `bnot` at all**.

This is sound because De Morgan and double negation both hold in SystemVerilog's
3-valued logic, and the collapse agrees on both sides — verified exhaustively over
`{0,1,x,z}²` with iverilog. It generalizes to every operator, not just `!`: only
`&&`, `||` and `!` are boolean *structure*; every other node, including bitwise
`&`/`|`/`~` and every comparison, is a leaf. Treating a 1-bit `a & b` as a leaf
rather than a conjunction is less factored but never wrong, so correctness does
not depend on classifying operators cleverly.

libsva now documents the matching precondition — that `bNot` and negated cube
literals assume two-valued atoms, and that a 3-valued frontend must use NNF
(`../libsva/src/Ir.zig:22-38`, `../libsva/include/sva.h`, `../libsva/README.md`).
That closes the trap rather than leaving the next frontend to find it.

**The cost, stated.** Two polarities of one term become unrelated atoms, which
costs two measurable things (§5.6 explains what a literal is and how these were
measured):

- **One extra root per negated term.** With a real `bNot`, `a ##1 !a` produces two
  guards over a *single* root — the backend evaluates `a` once per cycle and the
  guards differ only by the literal's `negated` bit. Under NNF they are two atoms,
  so two roots and one more bit evaluated every cycle.
- **Lost contradiction pruning.** `../libsva/src/Glushkov.zig:485-492` returns
  `never` for a cube carrying one root with both polarities, making the state
  unreachable; `(a && !a) ##1 b` prunes to nothing. Under NNF the same shape keeps
  both states and three roots. A dead product state therefore survives in shapes
  like `(a ##1 b) intersect (!a ##1 c)`, whose cycle-1 guard can never hold.

Both are size and throughput, never a verdict.

The way to recover it, if state counts ever justify it, is the one libsva's docs
name: teach the guard layer that a *pair* of atoms is mutually exclusive. `c(e)`
and `c(!e)` are never both true (though both may be false, which is exactly the
3-valued part), so that is a purely logical statement about the alphabet with no
mention of widths or unknown values — it keeps the agnostic boundary intact. It is
deliberately not in scope here: correct and uniform first, measured later (§13).

### 5.4 Where sampling does not apply

`disable iff` is the one place the LRM deliberately does *not* sample (LRM 16.15).
Its condition is drawn from an unsampled channel, so clotho evaluates the reset
root from **live** signals rather than shadows — the one expression in an
assertion that does. This is why `sva_monitor_reset` is a root index rather than
being folded into the guards: the two have different evaluation sources.

`$sampled` is the one read in such a root that *is* sampled, and LRM 16.9.3 says
so in as many words: "the use of `$sampled` in a `disable iff` clause is
meaningful since the disable condition by default is not sampled". It resolves to
the shadow like any sampled operand and to nothing else — no history ring, no
per-attempt state — so it needs none of the live root's machinery and costs it
nothing.

A clock edge out of X is a real edge, because clotho's existing edge lattice
`0 < x/z < 1` already says so (§3.4). Nothing to add.

### 5.5 Atom identity

Identity affects size, never correctness. There is no expression-identity or CSE
map in clotho: `sigmap_` (`src/Lower.cpp:13536`) is `ValueSymbol* -> ir::Signal`
only, and nothing keys on an `Expression*`. An atom is keyed by **(canonical
expression, polarity)**:

- A plain reference to a `ValueSymbol` keys on `canonical_value_symbol(sym)`
  (`src/Lower.cpp:95`) plus polarity, reusing the canonicalization `signal_of`
  already performs. That covers `req`, `ack`, `valid` — the overwhelming majority,
  and it is the case where "atoms are just signals" is literally true.
- Anything else keys on `const Expression*` pointer identity plus polarity, the
  idiom `stmt_force_site_` already uses (`src/Lower.cpp:3969`). Two textually
  identical but distinct `Expression` nodes yield two atoms; the cost is a wider
  valuation and a coarser cube.

### 5.6 Why the boolean vocabulary stops here

The alternative to NNF-plus-atoms is to extend libsva's boolean layer — `xor`,
`eq`, `lt`, `ite`, bit-select, arithmetic — so guards model more of SystemVerilog
directly. The criterion for rejecting that is not taste, and it is not "keep the
library small": **a tag is earned by changing the automaton construction, not by
being an operator.**

#### What a cube is, and why its size is the measurement

A guard is not stored as an expression tree. It is a **cube**: a flat list of
**literals** conjoined, where a literal is `(root index, negated)` and a *root* is
a boolean expression the backend evaluates to one bit per cycle. At runtime,

```
holds = AND over literals of ( root_bit[literal.root] XOR literal.negated )
```

So `a && b` could be stored either as one literal whose root is the whole `a && b`
node — the backend evaluating one bit — or split into two literals over two roots.
Both are correct; libsva splits. For `a || b` it has **no choice**: a cube is a
conjunction, and a disjunction cannot be written as a list of conjoined literals,
so the whole `||` node becomes one root.

The literal count therefore measures **how far inside an expression the guard
layer looked.** Two or more literals means it looked inside and the pieces are
separately visible; one literal means it did not, and the expression is a black
box evaluated as a unit.

#### What looking inside buys

Two things, both requiring the literals to be separate — and both measurable:

```
sharing: are two states' guards recognised as the same condition?
  (a&&b) ##1 (b&&a)   guards 2,2 (2,2 literals); SHARED;      2 roots/cycle
  (a||b) ##1 (b||a)   guards 2,3 (1,1 literals); not shared;  2 roots/cycle
  a ##1 a             guards 2,2 (1,1 literals); SHARED;      1 root/cycle

pruning: is an impossible guard detected?
  (a&&!a) ##1 b       BOTH STATES PRUNED
  NNF of the same     guards 2,3 (2,1 literals); not shared;  3 roots/cycle
```

- **Sharing.** `intern` sorts the literals, so `a&&b` and `b&&a` normalise to one
  list and one guard index, evaluated once. `a||b` and `b||a` are the same
  condition and got *different* guard indices, because an opaque root is compared
  by identity rather than structure.
- **Pruning.** `intern` scans the sorted literals for one root carried with both
  polarities and returns `never`, making the state unreachable
  (`../libsva/src/Glushkov.zig:485-492`). With a single literal there is nothing to
  compare.

#### What the guard layer actually absorbs

Probing `Glushkov.cube` (`../libsva/src/Glushkov.zig:438-455`) with each shape:

| guard | cube produced |
|---|---|
| `a && b` | 2 literals — `atom`, `atom` |
| `a && b && c` | 3 literals |
| `a && (b \|\| c)` | 2 literals — `atom`, **`or` as one whole root** |
| `a \|\| b` | **1 literal — the entire `or` node is the root** |
| `!(a && b)` | **1 literal — the entire `and` node, negated** |
| `!(a \|\| b)` | 2 literals — `!atom`, `!atom` (De Morgan) |
| `a && !a` | state pruned, guard unsatisfiable |

So the guard layer absorbs **one** thing: conjunction in positive position, plus
its De Morgan dual. A positive `bor` and a negated `band` are *already* opaque
leaves — `bor` earns its tag only for the negated case.

That settles the question. `count == 3` is not a conjunction, so a hypothetical
`eq` tag could not be split either: it would land in exactly the slot positive
`bor` already occupies — one literal, one root compared by identity, evaluated by
the backend. Which is bit-for-bit what an atom produces. No gain in cube quality,
in interning, in contradiction detection, or in the emitted LLVM, which is
identical either way because clotho evaluates the root expression regardless of
who holds the tree.

And it would cost four things:

1. **Interning would get worse, not better.** This is the counter-argument's
   natural home — "libsva hash-conses, clotho has no expression CSE" — and it runs
   backwards. `rootOf` (`../libsva/src/Glushkov.zig:420-431`) keys an `atom` on its
   *handle*, i.e. by value, and states plainly that "every other root is compared
   by identity. Structural equality of arbitrary boolean trees is the `Ir`'s
   business, not the lowering's." So `count == 3` as an **atom** gets the sharing
   path that works; as an `eq` node it would get the one that does not.
2. **It duplicates clotho's expression IR, badly.** `ir::Ir` is 211 opcodes over
   4-state plane pairs with widths and signedness. A libsva subset would carry none
   of that and would have to pick a semantics for `==` on unknown or differently
   sized operands — a decision it has no information to make.
3. **It re-breaks a decision already taken.** The original review of this IR
   deleted `Signal.ty` and `bxor` on exactly this ground: a signal with a bit width
   leaks SystemVerilog, and `bxor` was expressible from what remained. `eq`/`lt`
   reintroduce the same leak in a stronger form.
4. **Richer contradiction detection needs a decision procedure, not syntax.**
   `x & !x` is detectable because it is propositional. `count == 3 && count == 4`
   needs theory reasoning. Putting that in the guard layer is putting a solver
   there — which is precisely what libsva's *other* planned backend is for, and it
   belongs there, not in the automaton builder.

There is also a boundary argument that is load-bearing rather than aesthetic.
libsva is meant to serve other front-ends — PSL, VHDL, a software trace monitor —
and each has its own expression language and its own unknown-value semantics
(SystemVerilog's 4-state, VHDL's 9-value, a trace monitor's 2-state). An opaque
atom is the only leaf all of them can agree on. An `eq` node would have to fix one
meaning and be wrong for someone.

**Where the same criterion says to extend.** The criterion is not a preference for
a small vocabulary, and it points the other way one layer up. None of these can be
an atom, because every one of them changes the automaton:

| operator | why it is a construction, not a leaf | where it ended up |
|---|---|---|
| `throughout` | a guard conjunction on every transition of the operand | tag, `seq_throughout` |
| `and` | a different product from `intersect` — operands may end at different cycles | tag, `seq_and` |
| `within` | containment, which is a padded `intersect` | sugar, `sva_seq_within` |
| `first_match` | shortest-match, needing determinization and set difference | still a gap (§9.2) |
| `until` / `nexttime` / `always` family | new property-layer constructions over pending obligations | still a gap (§9.2) |

The first three have since landed in libsva, and the split came out where the
criterion said it would: the two that are genuinely new products became tags, and
the one that is a rewrite of tags the IR already had became sugar. The line is:
**atoms for what the automaton does not branch on, tags for what it does.**
Boolean operators mostly do not; sequence and property operators always do.

The one boolean-layer extension the criterion admits is the mutually-exclusive
atom pair (§5.3), because it changes what the automaton is — it prunes a state the
table above shows NNF keeps. That is why it is scoped rather than dismissed, and
why it is gated on measurement rather than on argument (§13).

---

## 6. Where the monitor lives in the IR

`docs/design/synthesis-and-lsp-scoping.md:14` states the invariant:
"**Architectural invariant (non-negotiable): no IR rewriting.** Two lowering
passes, AST→IR and IR→backend." The same document (`:216-219`) explicitly blesses
*ephemeral, purpose-built, build-consume-discard* IRs.

The libsva IR is exactly that: built in the frontend, consumed by `sva_lower`,
discarded before the frontend returns. It is not a second persistent IR and does
not rewrite `ir::Ir`. **libsva therefore links only into the frontend**
(`clotho`, `CMakeLists.txt:221`), never into `clotho_ir` or `clotho_ir_core`.

What crosses into `ir::Module` is plain data with no libsva dependency, so
`clotho_ir_core`'s LLVM-free and slang-free status is untouched
(`CMakeLists.txt:193-202`, whose only link dep is header-only `unordered_dense`):

```cpp
// clotho_ir_core. POD tables read by the backend; no libsva, no LLVM, no slang.
struct AssertionMonitor {
    uint32_t   id;             // index into Module's assertion table
    Kind       kind;           // Assert | Assume | Cover
    uint32_t   depth;          // obligation state vectors
    Strength   strength;
    bool       has_trigger, overlap;
    uint32_t   reset_root;
    std::vector<Index>    roots;      // clotho IR expressions, one bit each
    std::vector<uint32_t> literals;   // root << 1 | negated
    std::vector<Guard>    guards;     // {start, len} into literals
    Automaton  trigger, obligation;   // guard_of, follow, first, last, guard_states
    SourceSpan span;
};
```

The `roots` bridge is `sva_bool_read`: the frontend walks each root back,
translating `atom(k)` to the clotho `ir::Index` it assigned atom `k`, and
`band`/`bor`/`bnot`/`bool_const` to the corresponding clotho logical ops.

Monitor runtime state is a set of **hidden 2-state signals** per assertion
instance, not a new storage mechanism. Hidden compiler-internal signals are an
existing idiom (`force_site`, `src/Lower.cpp:3931-3941`), inheriting storage
layout, per-instance addressing and zero-init:

| Signal | Type | Purpose |
|---|---|---|
| `sva.<n>.trig` | `bit [T-1:0]` | Trigger state mask (union of all attempts) |
| `sva.<n>.pend<k>` | `bit [O-1:0]` | Obligation attempt `k`, `k < depth` |
| `sva.<n>.occ` | `bit [depth-1:0]` | Which slots hold a live attempt |
| `sva.<n>.tid<s>` | `bit [A-1:0]` | Attempts whose antecedent threads sit in trigger state `s`, one bit each (§13). Only where the antecedent can match twice in one attempt and its longest match is bounded; `A` = that length + `depth` + 1 |
| `sva.<n>.oid<k>` | `bit [A-1:0]` | The attempt that seeded obligation slot `k` |
| `sva.<n>.fid` | `bit [A-1:0]` | The attempt of a `\|=>` match, carried to the cycle it seeds in |
| `sva.<n>.idn` | `bit [A-1:0]` | Rotating one-hot id the attempt starting this cycle takes |
| `sva.<n>.pass` | `bit [A-1:0]` | Attempts all of whose obligations have passed, waiting to go quiet |
| `sva.<n>.fired` | `bit` | Trigger matched last cycle, for `\|=>` |
| `sva.<n>.cnt` | `int` | Match count, `cover` only |
| `sva.shadow.<sig>` | same as `<sig>` | Preponed shadow, one per sampled variable, shared by every assertion (§4) |
| `sva.<n>.past.<sig>.<k>` | same as `<sig>` | History ring entry, the shadow from `k + 1` ticks ago; per monitor, only as deep as `$past` asks for (§4.1) |

---

## 7. Code generation

### 7.1 A worked example

`assert property (@(posedge clk) req |=> ack ##[1:2] done);` lowered by
`sva_lower`, tables dumped through the public API:

```
depth = 3   strength = weak   overlap = |=>
roots (4), reset = root0:
  root0 = const 0      root1 = atom done    root2 = atom ack    root3 = atom req
guards: 5 interned  (0 = always, 1 = never, 2 = ack, 3 = done, 4 = req)

trigger: 1 state
  first = 1     last = 1
  state 0  guard 4 = req     follow = .

obligation: 3 states
  first = 1..   last = .1.
  state 0  guard 2 = ack     follow = .11
  state 1  guard 3 = done    follow = ...
  state 2  guard 0 = true    follow = .1.
```

State 0 is the `ack` letter; state 1 is `done`; state 2 is the filler that spends
the second cycle of `##[1:2]`. `depth = 3` is the longest match — `ack`, filler,
`done` — so three attempts can be in flight.

### 7.2 The transition mechanism

Clotho has **no word-array bitset arithmetic**: every value is a single
arbitrary-precision LLVM integer `iW` (`struct Plane`,
`src/ir/LlvmBackend.cpp:111`). That is an advantage. An `N`-state mask is one
`iN`, LLVM legalizes it, libsva's `words` stride becomes irrelevant, and every
`follow` row, `first`/`last` mask and `guard_states` row becomes an `iN`
**immediate constant** — so the absence of a generic constant-table emitter does
not matter. Here the obligation is `i3` and the trigger is `i1`.

`Monitor.State` advances by iterating the set bits of the current vector. **A code
generator cannot do that** — it has no loop over an unknown set. It transposes:
instead of iterating live states and unioning their follow rows, fold over *every*
state and select that state's constant row by its own liveness bit.

```
// advance: next = (U follow[s] for s in current) & satisfied
next = 0
next |= sext(current >> 0) & 0b110      // FOLLOW_0, constant
next |= sext(current >> 1) & 0b000      // FOLLOW_1 = 0 — folds away entirely
next |= sext(current >> 2) & 0b010      // FOLLOW_2
next &= satisfied
```

`sext(x)` is `sext i1 to iN`, spreading one bit to a full mask so a constant can
be selected without a branch. Each state costs a shift, a sign-extend and two
ANDs, and states with an empty follow row cost nothing after folding.

`satisfy` transposes the same way, folding over **guards** rather than gathering
per state — which is what `sva_automaton_guard_states` exists for:

```
// satisfy: which states' guards hold this cycle
sat = 0
sat |= ~0                      & 0b100   // guard 0 (always) -> filler state 2
sat |= sext(root2)             & 0b001   // guard 2 (ack)    -> state 0
sat |= sext(root1)             & 0b010   // guard 3 (done)   -> state 1
// guard 1 (never) carries no state: guard_states is 0, folds away
// guard 4 (req) carries no obligation state: folds away
```

A cube with several literals becomes a chain of `(roots >> r) ^ negated` ANDs
before the select. In the common case where each state has a distinct single
literal, `satisfy` degenerates to *assembling a bitfield from the root bits*,
which is what the constants above show.

The rest of the cycle is then:

```
seed:     next |= FIRST & sat            // 0b001 & sat
matched:  (next & LAST) != 0             // next & 0b010
failed:   next == 0
```

and the attempt ring, with `depth = 3` a compile-time constant, unrolls over
three slots. `freeSlot` is `cttz(~occ)` on an `i3`; the verdict pass is three
predicated updates. There is no branch in the whole step except the reset test
and the calls that report.

### 7.3 One trap, found by testing

**`occ` is not redundant with `pend != 0`.** It is tempting to drop it and derive
occupancy from emptiness, since a discharged or failed attempt is zeroed either
way. That is wrong: an attempt seeded into a cycle whose first letter does not
hold has an **empty state vector immediately**, and that is exactly what a
failure is — it is what `a |=> b` reports when `b` is low. Deriving occupancy
from emptiness makes that indistinguishable from an absent attempt.

This is not hypothetical. The variant was implemented and it **lost every
failure on every shape** — `reference f=1` against `emitted f=0`, uniformly.
`../libsva/test/backend.c` now pins it as a named test.

### 7.4 Validation

The scheme above is not a proposal; it is implemented and differential-tested in
`../libsva/test/backend.c`, which reads the tables through the public C API,
transposes as described, and is checked against `Monitor.State` cycle by cycle
over 400 random traces × 24 cycles for each of eight property shapes —
implication with a delay range, with and without a reset, overlapping
implication, a bare strong sequence, consecutive repetition, goto repetition in
an antecedent, a multi-literal cube with `or`, and an `intersect` product — in
all four optimize modes. All agree.

That de-risks Phase 1: the remaining work on clotho's side is emitting this shape
in LLVM, not discovering it.

### 7.5 Loop or unroll

The backend can emit either — counted loops with PHIs appear in ~15 sites — and
the standing default is unrolled. The evidence is real but narrower than it
looks: an NBA block-commit A/B measured unrolled 383 vs counted-loop 380 ms on
`wideff` N=256, a wash, and the counted loop was adopted on that basis; a later
re-measurement found unrolled at 59 vs 81 ms and the default was set back to
threaded + unrolled. Neither is a mask-folding workload, so this argues for
starting unrolled, not that unrolled wins here (§13).

Clotho does **not** emit branch-free code generally — every 4-state op emits an
x-detect fast/slow split — and a branchless 4-state lowering is an open
follow-up. The monitor step sidesteps that by collapsing to 2-state at the
sampling boundary, where the LRM already requires it.

### 7.6 Diagnostics

A libsva lowering failure names an IR node. The frontend keeps a side table from
`sva_node` to `slang::SourceRange` while building, so
`SVA_ERROR_UNBOUNDED_OBLIGATION` points at the offending sub-expression rather
than the whole assertion. Raw material exists on both sides: every
`AssertionExpr` carries `syntax->sourceRange()`
(`build/_deps/slang-src/include/slang/ast/expressions/AssertionExpr.h:123`),
`BinaryAssertionExpr` adds `opRange` (`:486`), `SequenceConcatExpr::Element` adds
`delayRange` (`:365`). Reporting uses `report(Severity, code, msg, span)`
(`src/Lower.cpp:1091`) with `DiagnosticOrigin::Rejected`, not `fail()`, because
these are diagnosable properties of legal source. `cur_range_` is already stamped
with the statement's span at `lower_stmt`'s head (`:5633`), so `fail()` inside
assertion lowering needs no new plumbing.

Failure *reports* reuse `rt_severity` (`src/runtime/Runtime.cpp:2953`) and the
compile-time `banner_at` prefix (`src/Lower.cpp:1197`), scheduled into Reactive
per §3.

---

## 8. Build integration

libsva is a Zig static library plus a C header. Clotho's build has **never
invoked an external build tool**: the single 347-line `CMakeLists.txt` contains
zero `add_custom_command`, `add_custom_target`, `add_subdirectory`,
`find_program`, or `ExternalProject_Add`. Every dependency arrives via
`FetchContent` (slang, doctest, `unordered_dense`) or one
`find_package(LLVM REQUIRED CONFIG)` pinned to major 22. A `zig build` invocation
would be the first external-tool step, with no in-repo pattern to copy.

That cost lands on CI: one matrix job over six platforms
(`.github/workflows/ci.yml`) installing LLVM 22 from apt.llvm.org / Homebrew /
MSYS2, with no Zig toolchain. libsva's own CI already uses `mlugg/setup-zig@v2`
with Zig `0.16.0`, which is cross-platform, so the step is known-feasible.

Mitigating facts:

- `project(clotho C CXX)` (`CMakeLists.txt:2`) already enables C.
- libsva installs both artifacts (`zig-out/lib/libsva.a`,
  `zig-out/include/sva.h`) via `installHeader` + `installArtifact`
  (`../libsva/build.zig:28-29`).
- Linkage is frontend-only (§6), so exactly one target gains a dependency.
- No libsva symbol reaches `Jit.cpp`'s 195 `expose()` registrations
  (`src/ir/Jit.cpp:178-406`) — nothing new becomes visible to JIT'd code.
- libsva is built `.pic = true` (`../libsva/build.zig`). This was **not** true
  until it was found by linking the archive into a default `gcc` build: a non-PIC
  archive cannot link into a PIE executable, which is the default on most
  toolchains. Fixed upstream rather than worked around with `-no-pie`.

**Open decision.** Submodule + `add_custom_command` running `zig build`
(recommended — one source of truth, matches how libsva is developed),
`FetchContent`, or prebuilt archives per platform. Needs a call before Phase 1.

---

## 9. What is rejected, and how

Every construct below is guarded by a loud frontend `fail()` with a `TODO:`
naming the gap.

### 9.1 Must be rejected before the libsva builder is called

These would trip a libsva assertion and abort the compiler, so the frontend is
the only thing between a user's legal SystemVerilog and a crash.

| Shape | Why |
|---|---|
| `[*0:n]`, `[=0:n]`, `[->0]` | `seqRepeat` asserts `lo >= 1` |
| `[->n]` / `[=n]` over a non-boolean | `seqRepeat` asserts a `seq_boolean` operand for goto/nonconsecutive |
| Any empty-admitting sequence | libsva bans empty matches; slang's `NondegeneracyStatus::AdmitsEmpty` diagnoses it with a span |
| Reversed range `##[3:1]` | `seqDelay` asserts `lo <= hi` |
| Any use of libsva's `bnot` | Unsound over a 3-valued atom (§5.2). Not a libsva assertion — a *wrong answer*, which is worse. Clotho emits NNF and never calls it |

Reportable rather than fatal, but still must be reported: a nested implication in
a consequent (`SVA_ERROR_UNSUPPORTED_CONSEQUENT`), and an unbounded obligation
(`SVA_ERROR_UNBOUNDED_OBLIGATION`) — which is what `##[1:$]` or **any goto or
non-consecutive repetition** in a consequent produces. Such a repetition is legal
only in an antecedent, where the trigger is existential and needs no bound.

### 9.2 No libsva tag exists

> **This subsection lags the property layer and is not the authoritative status.**
> `docs/design/sva-property-layer.md` §8 is. Phases 1 and 2 landed most of the
> list below — the bounded group through `sva_prop_shift` / `sva_prop_junction` /
> `sva_prop_followed_by`, and the abort group through `sva_prop_abort` — and only
> the entries named here as remaining are still untagged.

Still untagged: `until`/`s_until`/`until_with`/`s_until_with`, unbounded
`always`/`s_always` and `eventually`/`s_eventually`, `implies` and `iff`.

Every entry left on that list is a PROPERTY-layer operator (LRM 16.12). The
SEQUENCE layer (LRM 16.9) no longer has one at all: `##`, `[*]`, `[->]`, `[=]`,
`and`, `intersect`, `or`, `first_match`, `throughout` and `within` all lower.

`and`, `throughout`, `within` and `first_match` were on this list and have left
it: libsva gained `seq_and`, `seq_throughout` and `seq_first_match` as tags and
`within` as sugar, and clotho lowers them in the `Binary` and `FirstMatch` arms
of `Lowering::sequence`. Each has a fixture under `tests/sva/` exercising it —
`diff_and.sv`, `diff_throughout.sv`, `diff_within.sv`, `diff_firstmatch.sv`.

`first_match` carried a warning for whoever removed it from this list, because
the obvious fixture for it proves nothing. The warning is kept, because it is
what makes `diff_firstmatch.sv` the shape it is and it applies again to anyone
who edits that fixture. In a CONSEQUENT, or as a top-level bare sequence
property, `first_match` is a no-op on the verdict: a sequence property holds the
moment its sequence completes a match, and the earliest match is exactly the one
`first_match` keeps, so a correct lowering and a no-op report the identical
cycle. Only two positions are observable — an ANTECEDENT, where each match
raises its own obligation and suppressing the later match suppresses an
obligation, and COMPOSED INSIDE a sequence (`first_match(s) ##1 c`), where
dropping the later inner match turns a match into no match at all. That was
measured on a libsva monitor rather than argued, and it means a fixture in
consequent position would pass whether or not the operator was lowered at all.

`diff_firstmatch.sv` therefore uses the ANTECEDENT form, on a trace that drives
BOTH matches of `a ##[1:2] b` from a single start so the suppression has
something to suppress. Its teeth are measured rather than asserted: the
committed fixture reports 4 failures (95, 145, 235, 285 ns), and the identical
design with `first_match` stripped reports 7 — the same four plus 45, 195 and
295 ns, which are exactly the three obligations the suppression removes. One
window of that trace drives only the SECOND match (no `b` at the first offset),
which is what separates a real lowering from one that merely truncated
`##[1:2]` to `##1`: there the later match IS the first match and has to survive.

libsva lowers `first_match` by DETERMINISING its operand rather than by
combining automata, because whether a match survives depends on whether a
shorter one covers the same cycles. That makes it the one sequence operator
whose state count can be exponential in its operand's, so `SVA_ERROR_STATE_LIMIT`
is a reachable outcome of the BUILDER and not just of `sva_lower`;
`Lowering::sequence` gives it its own message there instead of folding it into
the out-of-memory one every neighbouring call site uses. The unit test pins the
determinisation directly — the trigger automaton of a `first_match` antecedent
has more states than the bare operand's, which a lowering that silently dropped
the operator could not produce.

`throughout` used to carry a note here, that `exp throughout seq` ≡
`(exp[*0:$]) intersect seq` in the LRM but `[*0:…]` admits the empty sequence,
which libsva bans — so it could not be desugared. The resolution: because every
libsva sequence matches at least one cycle, `(exp[*1:$]) intersect seq` is the
same language, and libsva builds it by conjoining `exp` into every state of the
operand's automaton rather than constructing anything at all. Its left operand
is therefore a BOOLEAN node — the only sequence-layer operand in the IR that is
not a sequence. clotho unwraps slang's `SimpleAssertionExpr` back to its
`Expression` and lowers it through `boolean()`; anything else reaching that
parameter aborts the compiler instead of being reported, so the shape is checked
first (§9.1).

`and` and `intersect` differ in one way that reaches the user. An obligation is
refused unless it has a bounded longest match, and an `and` runs to the LATER of
its two ends, so BOTH operands must be bounded; an intersection's ends coincide,
so either bounded operand bounds the pair. `req |=> (ack ##[1:$] done) and rdy`
is therefore refused where the identical shape under `intersect` is accepted.
The refusal spells that asymmetry out, because "no bounded longest match" on its
own does not point at it.

### 9.3 Out of scope for day one, for clotho reasons

Genuinely multi-clock properties — two structurally different clocking events in
one assertion, or an `EventListControl` as the clock (LRM 16.13); a single event
declared inside a sequence or property is not one of these, see below; local
variables and match items (`SequenceWithMatchExpr::matchItems`,
`FirstMatchAssertionExpr::matchItems`, `AssertionInstanceExpression::localVars`);
`clocking` blocks and `default`/`global clocking`; `checker`/`endchecker` and
checker instances; `expect` (LRM 16.17, needs the reactive set plus a blocking
wait); `.triggered`/`.matched`; assertion control (LRM 20.12) over anything but
a concurrent assertion — an immediate, deferred or `expect` assertion lowers
inline with no per-assertion state a control task could write, and a
`unique`/`priority` violation check is not an assertion at all, so a task
selecting any of them is refused rather than quietly leaving them running — and
assertion control with a non-constant argument, which decides how much code to
emit and where and so would need a run-time dispatch over every assertion in
scope; a gating
expression or an explicit clocking argument on a sampled value function, and any
of them over an expression rather than a plain variable (§4.1); dynamic-array,
queue and associative-array elements in an assertion
expression (LRM 16.6 requires the element value be captured, §4); function calls
in an assertion expression (their reads are not in the shadow set, §4); recursive
properties (LRM 16.12.4); a `disable iff` at the top of a named `property`
declaration, which would have to be hoisted back out through the instantiation to
reach `sva::Request`; `$global_clock`; end-of-simulation coverage reporting for
`cover property` (already deferred at `docs/TODO.md:738-740`).

Named `sequence` / `property` declarations and their instantiation work
(`tests/sva/diff_namedseq.sv`). slang hands back an already-expanded
`AssertionInstanceExpression::body`, so an instance is a recursion into that body
at the layer the instance's *type* names — `SequenceType` recurses through
`Lowering::sequence`, `PropertyType` through `Lowering::property`, and getting
that backwards trips libsva's operand-layer assertion rather than producing a
diagnostic. A declaration that is never instantiated compiles to nothing: it is
skipped in the module-member walk, and `collect_scope_signals` never descends
into it, so it registers no storage either.

`let` came with them for free. slang expands a let call straight to the
substituted `Expression` rather than wrapping it in an
`AssertionInstanceExpression` (`MiscExpressions.cpp:1023-1025`), so a let inside
an assertion is an ordinary boolean by the time the walker sees it, and one
outside an assertion is an ordinary expression. It was only ever blocked by that
same module-member walk.

Inferred clocking (LRM 16.14.6) works (§2.1), with one shape still refused: a
clock slang infers that is not one of the events the enclosing procedure waits
on — a `default clocking` block, a `$inferred_clock` expansion, or an event
control buried in the body rather than leading it. Clotho has no instance-local
`TimingControl` to resolve such a verdict against, so it refuses rather than
substituting whichever event control is at hand.

A clocking event declared inside a named `sequence` or `property` rather than on
the assertion (LRM 16.13.1, clock flow) works too, and it is the shape most of
chapter 16 is written in:

```systemverilog
sequence seq;
    @(posedge clk) a ##1 b;
endsequence
assert property (seq);          // no clock at the top
```

`Lowerer::body_clock` walks the property for one, and the resolution order is
explicit clock at the top, then body clock, then 16.14.6 inference, then the
existing loud failure. The hoisted event stays where the user wrote it, so
`sva::Request::clock` carries it into the walk and `Lowering::is_assertion_clock`
skips exactly that `ClockingAssertionExpr`; any other one is still the multiclock
refusal, compared by `TimingControl::isEquivalentTo` so that the same event
written twice is one clock. `tests/sva/diff_seqclock.sv` is the differential
fixture, on a negedge with `$display` on both edges so a dropped edge cannot be
attributed to the right cycle.

Hoisting is sound because slang has already validated clock flow before lowering
runs. slang threads an outer clock down the tree and errors (`AssertionNoClock`)
at any position that needs one and has none — it does not flow a clock left to
right across `##`, nor from an antecedent into a consequent. So on a design slang
accepted, every clock-needing position sits under some event; if the tree holds
exactly one distinct event, every position is under it, and making it the
assertion's clock changes nothing. If the tree holds two, the walk meets the
second and refuses. The walk in `body_clock` therefore descends only where
`sva::lower` descends: an event lifted out of a subtree the lowering never walks
would clock the assertion with nothing left to check it against.

Action blocks now run in the Reactive region (§3.5) rather than being refused
wholesale: the default failure report, severity tasks and `$display` all work
(`tests/sva/diff_action_ref.sv`), and a bare `#0` round-trips through Re-Inactive
and resumes in Reactive at the same instant (`tests/sva/diff_action_delay.sv`).

What an action block may contain is a **whitelist**, `ReactiveSafetyChecker`
(`src/Lower.cpp`), so anything not on it is refused. The walk runs once per COPY
of the action block, because there are two and they can run different things:

- `Lowerer::require_reactive_safe` checks the copy the verdict pends into
  Reactive as its own coroutine. It **may suspend**. A parked coroutine records
  the region it parked from and every wake path pushes the handle onto the ring
  that handle names, so an event control or a `wait` in an action block comes back
  in Reactive rather than Active — LRM 4.4.2.3, the same discipline a `program`
  block runs on.
- `Lowerer::require_finish_inlinable` checks the end-of-trace copy of the FAIL
  action, which only a **strong** property has: LRM 16.12.2 makes an attempt the
  trace ended underneath a failure, and Final is past the last time slot's
  Reactive drain, so `emit_monitor_finish` lowers the action straight into a Final
  process instead of pending a body nothing would resume. A Final process is
  resumed exactly once and then destroyed, so that copy **may not suspend at
  all** — not even on the `#0` the pended copy is free to use. Before this check
  existed that `#0` was accepted and its tail silently dropped at the end of the
  trace.

What the pended copy still rejects, and why — the list is what it currently
refuses, not an exhaustive account of the whitelist's complement:

- A **nonblocking assignment** is allowed: Re-NBA (LRM 4.4.2.8) commits it at the
  same instant (§3.5, `tests/sva/diff_renba.sv`). A *delayed* or
  *event-controlled* one is refused, by the intra-assignment item below.
- An **event control**, a **`wait`** and a **`#delay` of any amount** are allowed:
  each parks the body as a handle, so the wake knows to come back into Reactive.
  `#0`, `#5` and a run-time `#d` are one case rather than three; they used to be
  told apart by constant-folding the amount, because only `#0` had a region to come
  back through. `tests/sva/diff_action_resume.sv` is the differential fixture for
  the subscription and timer wakes, and derives the expected line order from the
  LRM ahead of any run.
- Both **event triggers** are allowed. Neither suspends the writer, and a waiter
  woken by either runs in this same time slot, because the drain re-settles the
  Active set after reactive work. `->` fires inside the drain and `->>` only once
  Re-NBA commits it, so their waiters run in that order.
- An **intra-assignment delay** (`x <= #1 y`, `x <= @e y`, `x = #1 y`): not a wake
  matter at all, since none of these suspends the process that issues them. The
  update lands in a LATER time slot, where `emit_intra_commit` runs it in that
  slot's ordinary NBA region rather than its Re-NBA one, and 4.4.2.8 scopes Re-NBA
  to updates issued from the reactive region set without saying which slot's region
  a delayed one belongs to. With nothing in the LRM to settle it, clotho refuses
  rather than guessing. The backend refuses the same three tags independently.
- A **`fork … join`** is allowed in the pended copy. It was never a region matter —
  the join record parks the parent as a handle whose wake carries its home region,
  and a child is resumed inline at the spawn — and it is no longer a frame matter
  either: an action block is lowered as a task body, and `lower_fork` now lowers a
  fork inside one, passing the enclosing body's frame locals to each child as
  `ref` formals. Refused in the *inlined* copy (the fail action of a strong
  property) like every other suspension, since that copy has no second resume.
  `fork … join_any` / `join_none` stay refused everywhere.
- A **`wait fork`** or **`wait_order`**: clotho lowers neither anywhere.
- A **`disable`** / **`disable fork`**: also not a region matter, since a
  `disable` suspends nothing. clotho lowers it as a lexical exit from an enclosing
  block of a *process* body, and neither copy of an action block is one.
- A **deferred immediate assertion** (`assert #0` inside an action block): it
  matures in Observed, which reactive code has already passed — a region order no
  wake can reverse.
- A **`forever` loop**: it never returns. The old reason (a pended body is resumed
  once) no longer holds, since a body may now be resumed as often as it suspends;
  what remains is that the drain never regains control and the activation's frame
  is never released, one per verdict that pends it.
- A call to a user **task**: clotho does not trace the body, so it cannot rule out
  a construct there that would be refused if it were written in the action block —
  a `forever`, an intra-assignment delay, a deferred immediate assertion. A
  suspension is no longer among them, so this is no longer "the call might consume
  time". Nor can it tell whether ordinary code calls the same task, which the
  backend refuses separately: a body records one home region and is lowered once.
  A user *function* is fine — LRM 13.4.4 forbids timing controls in one, so it
  cannot suspend and is safe to share between reactive and ordinary callers.

Separately, the deferred-immediate-assertion action forms at
`docs/TODO.md:741-751` are a different context and are not covered by this
whitelist.

---

## 10. Sequencing

Every phase ships something a user can run.

**Phase 1 — the region set.** Preponed-equivalent sampling in Postponed, Observed
as a region that runs bodies, and Reactive/Re-Inactive/Re-NBA as the reactive
triad. Ships standalone value with no SVA at all: it closes `docs/TODO.md:750`
(deferred immediate observed/final) and unblocks `program` blocks (`:853`). This
is first because §3 is the only part the rest cannot be made correct without.

**Phase 2 — one assertion, end to end.**
`assert property (@(posedge clk) a |=> b);` with `a`, `b` 1-bit 2-state signals.
No `disable iff`, no action block, module-item form. Carries the build
integration (§8), the `AssertionMonitor` POD, the `sva_bool_read` root bridge,
the transposed step (§7), and the Reactive-region report.

**Phase 3 — the boolean and property layers.** Any expression as an atom with the
§5.2 `&&`/`||`/`!` rule; `disable iff` (unsampled, §5.2); `|->`; bare sequence
properties; `strong()`/`weak()`; `assume`; `cover` with a match counter; clock
inherited from an enclosing `always @(posedge clk)` via `getInferredClock()`;
in-procedure form; severity-task action blocks.

**Phase 4 — the sequence layer.** `##n`, `##[m:n]`, `##[m:$]`, `or`, `intersect`,
`[*m:n]`, `[->n]`, `[=n]`. Mostly rejection-removal plus `SequenceRange`
translation; libsva already does the work.

**Phase 5 — sampling and reuse. Landed.**
`$rose`/`$fell`/`$stable`/`$changed`/`$sampled`/`$past(x, n)` (§4.1), and named
`sequence`/`property` declarations with arguments — nearly free via
`AssertionInstanceExpression::body`.

`docs/TODO.md:855-865` must be split as each phase lands, per that file's rule at
`:3-8`. The Postponed-sampling choice (§3.3) and anything else deviating from
the LRM still needs recording.

---

## 11. Verification

The fixtures live under `tests/sva/`, starting with `impl_basic.sv` for the
plain `a |=> b` implication; the operator-specific ones are named in the
sections that introduce them.

Neither iverilog nor Verilator serves as a general reference for this feature:

- **iverilog** has no SVA support at all.
- **Verilator 5.050** handles `assert property` with `--assert` and gives the
  correct failure cycle for `req |=> ack`. But it is **2-state**: a signal
  initialised to `1'bx` reads as `0`, so it cannot validate any of §5.2. That is
  a stronger objection than the project's general distrust — structural
  incapacity for a core part of this feature, not just lower trust.

Verification therefore splits four ways, and only the last is left without a
reference:

1. **Temporal semantics** — whether `req |=> ack` *means* the right thing — is
   already verified inside libsva by differential testing (sequence semantics by
   direct recursion over the IR, checked against the automaton over every trace
   of a small alphabet from every start cycle) and by mutation testing. Clotho
   should not re-test it.
2. **Codegen fidelity** — whether clotho's emitted LLVM reproduces the protocol —
   has an exact, deterministic, in-process reference: the same differential
   harness as `../libsva/test/backend.c`, driving clotho's JIT'd monitor and
   `sva_run_step` over one root-bit trace and comparing per cycle. This is the
   highest-value test in the feature and needs no simulator.
3. **X/Z and boolean collapse** — §5.2's table is testable against **iverilog**,
   because it is plain SystemVerilog expression semantics rather than SVA. The
   same trick covers sampling: an explicit `always @(posedge clk) prev <= x;`
   model reproduces `$rose`/`$past` without any SVA, and iverilog runs it.
4. **Region ordering** — the observable consequences of Observed-then-Reactive.
   Partly testable with iverilog via `program` blocks and `$strobe` ordering;
   genuinely SVA-specific orderings are where an external reference is missing.

Recommendation: treat (1)–(3) as the standing verification story, use Verilator as
a corroborating cross-check on 2-state designs only, and record the residual gap
in (4) rather than pretending it is covered.

The C++ unit-test wiring follows the existing convention: one doctest executable
over `tests/*.cpp`, SV embedded as C++ raw string literals, no `.sv` fixtures.
Rejection tests use `LowerResult::diagnostic_text()` + `CHECK_FALSE(r.ok)` and a
substring match (`tests/fork_test.cpp:44-46`).

---

## 12. Decisions needed before Phase 1

1. **libsva delivery.** Submodule + `zig build` custom command (recommended),
   `FetchContent`, or prebuilt archives (§8).
2. **Sampling region.** Postponed, using LRM 4.4.2.1's stated equivalence
   (recommended — reuses an existing region), or a real Preponed region at
   `wheel.inertial` (§3.3).
3. **Phase 1 scope.** The region set is a scheduler change with no SVA in it. It
   is the right first phase because it ships value alone and nothing else can be
   correct without it — but it is also the phase most likely to disturb existing
   timing behaviour, so it wants its own review.

---

## 13. What this document does not establish

- Whether the unrolled step beats a counted loop for large automata. The cited
  A/B evidence is NBA commit bodies, not mask folding (§7.5), and no SVA
  benchmark exists. The standing rule — never land a performance claim without a
  benchmark — applies.
- The state-count and depth limits worth setting. libsva defaults to 4096 states
  and depth 256; both are reportable, and what a real testbench needs is
  unmeasured.
- The cost of the reactive region set on designs with no assertions. The drain
  triad is emitted conditionally today (`has_inactive`, `has_nba`), so the
  intended answer is "nothing", but that is a claim to measure.
- Whether sampling every term every time step needs gating. A time step with no
  assertion clock edge still samples. Gating is an obvious optimization and is
  unquantified.
- The cost of negation normal form on state counts. NNF makes two polarities of
  one term unrelated atoms, so libsva can no longer prune a cube holding both
  (§5.3). The fix — mutually-exclusive atom pairs in libsva — is designed and
  scoped but its value is unmeasured, and no SVA workload exists to measure it on.
- Multi-clock support (LRM 16.13), the largest gap in §9 and the one most likely
  to be needed by a real UVM testbench (`docs/TODO.md:20-22`, M4).
- Whether two attempts whose antecedent matches end in the SAME cycle should
  each seed an obligation. clotho seeds at most one per cycle; the LRM does not
  say whether two antecedent matches completing together raise one obligation or
  two, and the question is open.

Settled since this document was written:

- An attempt whose antecedent matches more than once has ONE verdict, not one
  per obligation: it fails at the first obligation to fail and passes only once
  all of them have and it can match no more, with `cover` counting it under the
  same rule. That rules out both report coalescing and
  abandon-at-first-failure. clotho implements it with a one-hot attempt id per
  antecedent thread (§6), which is bounded only where the antecedent's longest
  match is; an unbounded antecedent is the residue.
