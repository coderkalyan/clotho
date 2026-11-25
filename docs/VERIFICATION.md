# Verification

How Clotho establishes that it is correct. `docs/CI.md` describes what CI runs.

## Priorities

In strict order:

1. **Never silently miscompile.** A crash or a refusal is better than a wrong
   answer.
2. **Never crash.** Every input is simulated or rejected with a diagnostic. An
   internal error is a bug even when the input is invalid.
3. **Complete sections, not common subsets.** What is implemented is
   implemented completely, rare cases and interactions included; a feature not
   implemented at all is refused and listed in `docs/TODO.md`. Refusing a case
   of an implemented feature is a gap, not a finish, and needs the
   maintainer's approval. Sections are completed in milestone order.

## Oracles

A test is evidence only if its expected result did not come from Clotho. Each
oracle below is first class and none is sufficient alone:

- **LRM-derived.** Expected output written from the LRM by an author
  independent of the implementation (see "Writing tests").
- **External simulators.** Icarus Verilog for event semantics; Verilator only
  for race-free designs. They cover a shrinking share of what Clotho
  implements and are sometimes wrong. A disagreement with the LRM goes to a
  human and is decided by the LRM.
- **Self-consistency.** A design produces identical output under every
  semantics-preserving configuration: optimization levels, optional passes,
  `clotho run` and `clotho build`.
- **Metamorphic relations.** Semantics-preserving rewrites of a design, such as
  commuting operands, inlining a function, flattening hierarchy, or replacing
  a packed struct with its vector, do not change its output. These reach
  constructs no external simulator supports.
- **Snapshot.** Clotho's own earlier output. A regression guard, never evidence
  of correctness.

Every SystemVerilog-level test records which oracle its expected output came
from. Comparisons cover every signal where possible, per time step through
VCD, not only what a test prints.

## Detectors

- **Assertions.** Invariants are asserted, and `assert()` is live in every
  build, shipped ones included. Checks too slow to ship, such as whole-structure
  verifiers, are behind `CLOTHO_EXTRA_ASSERTS`, which Debug and CI turn on.
- **Verifiers.** The LLVM verifier runs on every module before and after
  optimization. An IR verifier (`src/ir/Verify.h`) asserts the typing rules of
  `docs/design/kernel.md` on every lowered design and again before code generation
  (so on the backend tests' hand-built IR too), in every build type that keeps
  `assert()`. The backend asserts what the rules state; it repairs none of it.
- **Internal errors.** A crash or failed assertion reports itself as a bug in
  Clotho and says where to report it, never as a bare abort.
- **Shared code.** A unit's groups are lowered once and run by every instance
  whose imports are alike. With `CLOTHO_EXTRA_ASSERTS` the backend also lowers
  each group for each instance that shares it and fails the build, naming the
  unit, the group, the instances and the first differing line, if that is not
  the code shared (`SimulationConfig::verify_shared_code`,
  docs/design/modular-compilation.md). The check compares only units a design
  instantiates twice, so a bake the taint list misses stays unseen by every design
  that has one instance of the unit (an `expect` queue named by monitor number
  shipped that way). **Twin mode** closes that: in a build with the check, every
  design the suite lowers is lowered a second time with each of its top-level
  modules instantiated twice (a synthetic wrapper module, ports open), output
  discarded, and `ir::check_shared_code` runs the check on that design. A
  mismatch throws out of the lowering entry point (`set_twin_check`, Lower.h), so
  the test that compiled the design fails and names what the instances do not
  lower alike. Nothing is generated or optimized, so it costs a second frontend
  and backend lowering per design and no codegen. A design whose twin form cannot
  be built (no top, a hierarchical name that reaches a top by its module name, a
  twin that does not elaborate) is skipped and counted by reason; the suite prints
  the counts after its summary. `CLOTHO_NO_TWIN=1` turns it off to time the suite
  without it, `CLOTHO_TWIN_VERBOSE=1` prints each skip's diagnostic. A new test of a
  construct whose lowering names something of its instance is covered without a
  two-instance design of its own; a fix still gets one, since the twin check is a
  build-time check and a Release build has neither it nor the suite's twins.
  Shared code that reaches something of one instance by no record or binding
  (a design-wide id, a dump-var index, another instance's record) taints the
  lowering instead of baking it, and a verifying re-lowering must leave root as it
  found it: a global it makes there is reported as a difference.
- **Config matrix.** Self-consistency, run over the suite. With
  `CLOTHO_CONFIG_MATRIX=1`, every design a test lowers (`lower_text`, `lower_files`,
  `lower_sources`; hook: `set_design_check`, Lower.h) is run in child processes of
  the test binary under the default `SimulationConfig` and under each
  semantics-preserving backend gate flipped on its own (`fuse`, `comb_fuse`, `thunk`,
  `regions`, `dedup`, `split`, `z_elision`, `o3`, `gc_stress`), each bounded by a
  stop time and a wall-clock limit. Exit status, stdout and stderr must equal the
  default run's; otherwise the lowering throws, so the test that lowered the design
  fails and names the variant, the first differing line and a command that replays
  it. A design that reaches outside the process (files, `$system`, DPI-C, plusargs,
  VCD) or whose default run does not repeat is skipped and counted by reason. A
  difference is legitimate only where the LRM leaves the order open (4.7): such
  test cases, and known bugs, are listed by name in
  `tests/config-matrix-known.txt` as `race` or `bug`, and an entry whose test no
  longer differs fails the run. `CLOTHO_CONFIG_MATRIX_TIMEOUT` (seconds per child)
  and `CLOTHO_CONFIG_MATRIX_LOG` (one line per design) tune it. It costs a process
  per variant per design, so it is not in CI.
- **Scheduler trace check.** A build with `CLOTHO_EXTRA_ASSERTS` makes the generated
  code and the scheduler report each spawn, park, wake, dispatch, resume, end, region,
  NBA update and pass, `->>` pend and trigger, and `$finish` to `src/runtime/TraceCheck.cpp`,
  which tests the rules of docs/design/scheduler-rules.md online: R5 (a wake needs a
  parked activation waiting for its cause, a dispatch a queued one, a resume the control
  it parked at, nothing touches an ended one), R12 (each NBA pass leaves its targets as
  the updates give in creation order, and none is left over), R20 (a `->>` triggers inside
  an NBA pass, in the order they were pended), R24 (past a `$finish` only the exit that
  follows it runs), R6 (every write a parked waiter lists wakes it, no write that changed
  nothing wakes anyone, and a parked waiter's value read from design memory has not changed
  at the end of a slot without a wake) and R7 (the edge and change facts of each write are
  Table 9-2's). It is on by default in such a build, for the suite and for
  `clotho run`; `CLOTHO_NO_TRACE_CHECK=1` turns it off. A violation ends the run (status
  99, `clotho: scheduler trace violation:` and a report naming the rule, the time, the
  target or activation, and the cause) and fails the test that ran the design, unless
  `tests/trace-check-known.txt` lists it as `bug` or `rule`; an entry whose test no
  longer violates fails the run. The sv-tests summary classifies the marker as `trace`.
  A shipped build emits no call and links no checker (`trace_check_test.cpp`).
- **Sanitizers.** ASan and UBSan over the full suite and test corpus.
- **sv-tests.** Outcomes are classified as pass, refused, crash or fail; the
  crash count is zero. A pass there means only that nothing failed, not that
  values are right.
- **Fuzzing.** Differential: generated race-free designs, weighted toward
  width, signedness and 4-state, compared against the oracles above. Crash:
  mutated corpus inputs, each of which must simulate or be refused.
- **Coverage.** Line and branch coverage maps lowering and backend paths that
  no test reaches. It directs tests and generators; it is not a target.
- **Reduction.** Every fuzzer or crash finding is reduced (sv-bugpoint, then
  cvise) before it is debugged. The reduced case becomes the regression test.

## Writing tests

- Tests are SystemVerilog designs checked by observable output. C++ tests of
  internals are only for invariants with no observable effect.
- Expected values come first, from an independent author: an agent that reads
  only the LRM, with no knowledge of Clotho or the implementation plan. It
  writes the cases and their expected output before implementation starts,
  working out edge cases and covering the whole clause. The implementer does
  not edit them; a disagreement goes to a human and is decided by the LRM.
- Composition is tested by construction. Where components must compose, test
  the property that makes them compose, such as one type-classification
  predicate, one width and signedness contract per operator, or one storage
  layout that every access path agrees on. Generated matrices of combinations
  are for where no such property can be stated; they cost build and run time.
- Refusals are tested: a construct listed in `docs/TODO.md` is rejected with a
  diagnostic that names it.

## Fixing bugs

- The fix comes with a test that fails without it, reduced to the bug.
- Look for the same defect at sibling sites, and state the result in the
  commit body.
- Fix commits carry `Found-by:` (how it was found) and `Bug-class:` trailers.
  Classes: `ordering` (lazy or re-entrant lowering, partial state on an error
  path), `sibling` (one of several sites), `fail-open` (a predicate that
  defaults to accepting), `classification` (duplicated type predicates that
  drifted), `lifetime`, `4-state` (including width and signedness),
  `platform`.

## Review

Human review concentrates on intent and on where expected values came from.
Agent review also checks that:

- predicates fail closed: allowlists, exhaustive switches, no `default:` in a
  semantic classification;
- lookups into lazily built tables go through an accessor that builds the
  entry or asserts it exists;
- error paths leave no partial state;
- interactions with neighboring features are tested;
- type classifications are not duplicated;
- each new test names its oracle, and its expected values were not written by
  the implementer.

## Sections

A section, an LRM clause or a coherent part of one, is complete when:

- independently authored tests cover it;
- the differential fuzzer covering it has run clean for a fixed budget since
  its last fix;
- coverage shows no unreached lowering path for it;
- nothing in it is refused: a refusal left in the section is an open gap listed
  in `docs/TODO.md`, and the section is not complete until it closes.

## Trust levels

| Level | Bar |
|---|---|
| Developer | Used by someone who can troubleshoot the simulator. |
| Shared | Tested builds keep assertions; sanitizers clean; sv-tests at zero crashes; no known miscompile. |
| Teaching | Differential fuzzing and the oracle-labeled corpus in place; the synthesizable RTL sections complete; reference cores match Icarus Verilog by VCD on Linux, macOS and Windows. |
| Small commercial | The same bar across the M3 and M4 milestones. |
