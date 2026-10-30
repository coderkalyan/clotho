# Principles

These principles guide design decisions in Clotho. They change as the project
does.

- The IEEE 1800 LRM defines correct behavior. When Clotho's behavior is in
  question, the LRM text decides.
- Compliance is not traded for performance. A faster, non-compliant mode must
  be opt-in.
- A construct Clotho cannot simulate correctly is rejected with an error at its
  location. Clotho does not accept code it cannot simulate correctly.
- Every known gap is listed in `docs/TODO.md` until it is closed.
- Where the LRM allows more than one behavior, Clotho picks the fastest, then
  the simplest.
- Ambiguous clauses and deliberate interpretations are documented at the code
  that implements them, and in the feature's design document if it has one,
  citing the LRM text.
- Open-source simulators such as Icarus Verilog and Verilator are a
  cross-check. Event-driven simulators are useful oracles; cycle-based
  simulators are not oracles for event semantics. A disagreement with the LRM
  is resolved in favor of the LRM.
- Designs are compiled to native code. Performance is planned during design and
  measured after implementation.
- A feature costs nothing for a design that does not use it.
- Priorities, in order: correctness, safety, performance, ease of change.
- Invariants are checked with assertions. A violated invariant stops the
  program and reports where.
- Loops, queues and worklists have explicit bounds, and exceeding one is an
  error. Deep data structures are traversed with worklists, not recursion.
- Tests check observable behavior at the highest practical level. Every fixed
  bug gets a regression test.
- New dependencies need a strong justification.

## Methodology

Clotho follows
[TigerStyle](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md)
as a general methodology, with two exceptions: dynamic allocation is allowed
where needed, and naming follows the conventions of each language.

### Review tiers

| Tier | Scope | Review |
|---|---|---|
| 1 Kernel | Value representation and its operations; memory layout of state, frames and aggregates; stores; the GC and sanitizer fast paths in generated code and the runtime. | Every line, by the maintainer, against the decisions in `docs/design/kernel.md`. |
| 2 Architecture | The IR; analyses and transforms; containers, strings and built-in classes; the GC collector; sanitizer and coverage infrastructure; the LLVM pipeline. | The design matches the maintainer's intent, recorded in `docs/design/architecture.md`. |
| 3 Frontend | Lowering from slang, system tasks, VCD, DPI, the CLI, LSP, DAP and diagnostics. | Tests, fuzzing and comparison against the LRM. |
| Rewrite | Scheduling, concurrent assertions, constrained randomization. | None; these are being rewritten. |

## Design

Clotho is an event-driven simulator that follows the LRM's scheduling
semantics. A SystemVerilog frontend elaborates the design, Clotho lowers it to
its own intermediate representation and compiles it to native code, and a
runtime provides the scheduler and other simulation services. `docs/design/`
describes each part.
