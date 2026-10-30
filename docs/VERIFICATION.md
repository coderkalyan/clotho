# Verification

How Clotho establishes that it is correct. `docs/CI.md` describes what CI runs.

## Priorities

In strict order:

1. **Never silently miscompile.** A crash or a refusal is better than a wrong
   answer.
2. **Never crash.** Every input is simulated or rejected with a diagnostic. An
   internal error is a bug even when the input is invalid.
3. **Complete sections, not common subsets.** What is implemented is
   implemented completely; what is not is refused and listed in `docs/TODO.md`.
   Sections are completed in milestone order.

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
  optimization. An IR verifier (`src/ir/Verify.h`) runs after lowering as an extra assert.
  Until the frontend's output is clean under it, it is opt-in and report-only
  (`clotho build --verify-ir`).
- **Internal errors.** A crash or failed assertion reports itself as a bug in
  Clotho and says where to report it, never as a bare abort.
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
- what remains is refused and listed in `docs/TODO.md`.

## Trust levels

| Level | Bar |
|---|---|
| Developer | Used by someone who can troubleshoot the simulator. |
| Shared | Tested builds keep assertions; sanitizers clean; sv-tests at zero crashes; no known miscompile. |
| Teaching | Differential fuzzing and the oracle-labeled corpus in place; the synthesizable RTL sections complete; reference cores match Icarus Verilog by VCD on Linux, macOS and Windows. |
| Small commercial | The same bar across the M3 and M4 milestones. |
