# Design: dynamic domain specialization (built, then removed)

Status: **REMOVED** (2026-07). Landed as opt-in `--domain-spec` (thunk groups
only, reader-side dispatch, both bodies compiled eagerly), then deleted along
with the flag, `SimulationConfig::domain_spec`, `compute_domain_spec`,
`assume_clean_` / `domain_fake`, the `CLOTHO_DOM_DEBUG` counters, and
tests/domain_spec_test.cpp.

Why it did not pan out: the only shape it won on was the microbenchmark it was
designed for (`xdomain`, 4.7x). A real core regressed (picorv32 +11% sim) and
compile time rose 26% because both bodies are compiled eagerly, so the honest
answer to "should I pass `--domain-spec`?" was always "no" — an opt-in flag
nobody should enable is not a feature, it is two divergent code paths to
maintain. The per-op 4-state tax is still worth attacking; the routes that do
not need a second body are static never-x inference
(never-x-inference.md) and 2-state storage (two-state.md).

The rest of this document is kept as the record: the mechanism, the
eligibility rules, and the measurements. It describes code that no longer
exists; `git log -- src/ir/LlvmBackend.cpp` has it.

## Problem

4-state RTL pays the per-op 4-state tax (x-detect branch + both-planes
traffic) even when a signal never carries x at runtime. Real designs change
*domain* rarely: x until reset, then clean for the rest of the run. Static
never-x inference (never-x-inference.md) cannot prove the common
uninitialized-register case; the xdomain ceiling bench measures the win at
5.6x sim / 12x per-op on op-dense clean-steady-state workloads.

## Design

Speculative specialization with **reader-side validation** (the guard re-checks
the read set at each dispatch; there is no write-side invalidation state):

- **Two bodies per eligible thunk group.** `lower_group` emits the generic
  body and a fast body lowered under `assume_clean_`: `type_of` / `from_ref`
  report every 4-state value type as 2-state, so all operators route through
  the native 2-state paths that the explicit-`bit` lowering
  (two-state.md) already provides. `set_value`'s domain-coercion invariant
  and `to_four` at storage sinks handle every boundary: loads project the
  must plane (exact for a clean word), stores write duplicated planes + z=0,
  so storage layout and outside readers are untouched. No IR cloning; the
  same IR is lowered twice with per-pass value/type tables.
- **Dispatch: reader-side guard.** At each activation the group loads every
  tracked 4-state read-set signal's packed word and tests it clean
  (`(mm ^ (mm >> W)) == 0` — z encodes as x in the planes, so one word covers
  both), short-circuiting to the generic body on the first dirty signal. All
  clean ⇒ fast body. `group_reads_[g]` is the (deduped) read set. Correct
  because the guard reads the committed live state at entry, and eligibility
  forbids x-generation mid-body, so a read set clean at entry stays clean
  through the body.
- **No write-side maintenance.** The guard loads CSE with the body's own loads
  of the same signals (early-cse: the guard block dominates both bodies), so
  the marginal guard cost is the x/z tests, not the loads — and a persistently
  dirty read set exits after ~one test. Nothing is paid at store / NBA-commit
  sites. This replaced a per-signal-tag + per-group-dirty-counter scheme whose
  per-write retest (a full-word cleanliness test at every blocking store, NBA
  commit, and runtime write-back) was the entire ungated maintenance residual
  — see the measurements below.

### Why reader-side (vs the writer-side tag/counter it replaced)

The first implementation maintained a per-signal tag (clean/dirty/pinned) and a
per-group dirty counter, updated at every committed write by retesting the
stored word, with a flap-pin megamorphic fallback. Measured on picorv32 (all
groups specialized), the write-side notes cost ~9% and decomposed as ~100% the
retest (the changed-word / ndeps gates were free) — an irreducible
per-tracked-write cost. Reader-side moves the same cleanliness test to
dispatch, where the loads are already paid by the body: the guard measures
**+0.9%** (vs the notes' ~9%), and it *improves* the profitable case (xdomain
4.1x → 4.7x, because the heavy per-write retest is gone). It also
short-circuits for non-converting groups (one test, not a full retest per
write) and deletes the tag / counter / dep globals, the two note functions, and
the four NBA-meta
fields (~390 lines).

## Eligibility (V1 allow-list, spec_eligible_body)

Fast-body soundness = "on an all-clean read set, 2-state and 4-state lowering
agree, and nothing can dirty the read set mid-activation." Rejected:

- x-generators: Div / Mod / Pow (clean inputs can yield x — the 2-state path
  folds it to 0).
- dynamic BitSelect / DynPartSelect (OOB or unknown index reads x in 4-state,
  0 in 2-state; TODO: admit provably in-range selects) and ElemSelect /
  array signals (no per-element tracking).
- Call (callee lowers generically and may write x into the read set
  mid-activation; the dispatch decision is entry-time). TaskCall / Delay /
  Wait are already excluded by the thunk predicate.
- runtime write-backs ($fgets / $*scanf / $fread / $readmem / $ferror).
- strings, and constants carrying x/z bits (casez/casex wildcard patterns —
  their must-projection would drop the wildcard).

Anything not on the allow-list keeps the group generic — new IR tags are safe
by default. The tracked read set is the group's 4-state packed scalar Load
roots; a group with an empty set gains nothing and is skipped.

## No gate — opt-in instead

Specializing a group is only a *win* if its fast body is enough faster than
the generic body to pay for the per-activation boundary conversions the fast
body adds (a `trunc` extracting the must plane on each load, a `to_four`
duplicating planes on each store). For an **op-thin** body those conversions
can exceed the op savings — instcombine already folds the generic 4-state path
to near-2-state quality via z-elision and demanded bits — so the fast body can
run *slower* than generic (measured +10% on picorv32; see below). The root
cause is that the feature specializes *ops* but not *storage*, so the fast
body reads/writes 4-state storage and pays conversions it shouldn't.

An earlier revision hid this behind a static profitability gate (specialize
only groups with >= 64 compute ops and >= 8x their store count). That is
architectural debt: a benchmark-tuned heuristic gating two divergent code
paths, with discontinuous behavior at the threshold and no self-correction.
It was removed. The feature is now **fully opt-in and ungated**: off by
default (zero machinery emitted — the design compiles exactly as without it),
and when `--domain-spec` is passed every eligible group is specialized. The
op-dense-vs-op-thin trade-off is the caller's to make by opting in, not a
magic number in the backend. The principled way to make specialization a
universal win — and re-enable it by default — is to specialize storage too
(2-state storage for the tracked signals), removing the boundary conversions;
see Follow-ups.

## Measured (Release, LLVM 22, 2026-07-09; paired A/B min of 6)

| case | off (default) | `--domain-spec` | |
|---|---|---|---|
| xdomain sim | 620 ms | **131 ms** (4.7x) | 2-state floor 112 ms — 96% of the ceiling captured (reader-side, up from 4.1x / 92% writer-side) |
| xdomain llvm compile | 604 ms | ~760 ms (+26%) | both bodies eager |
| picorv32 sim | 1719 ms | +11% | op-thin datapath groups: fast-body boundary conversions exceed op savings — why the feature is opt-in |

- xdomain is the target shape (op-dense over transiently-x reset-then-run
  signals a static analysis cannot prove clean): a 4.7x win when opted in.
- picorv32 is the anti-shape (op-thin eligible groups; the op-dense blocks read
  deliberate `'bx` don't-cares and are ineligible): +11% when opted in, so you
  wouldn't. Off by default it is exactly nospec.
- Reader-side dispatch itself is near-free: with the fast body forced never to
  run, the guard over all 196 picorv32 groups costs +0.9%. The +10% is the
  fast body, not the guard.
- Output byte-identical off vs `--domain-spec` on picorv32, xdomain, and the
  bench-sched shapes (thunk_comb / regs / bigcase); domain transitions pinned
  against iverilog in tests/domain_spec_test.cpp (x round trip, partial-store
  promotion, NBA-commit transitions, producer-store promotion of a downstream
  group, cycle-by-cycle oscillation).

## Follow-ups

- **Specialize storage, not just ops — the path back to default-on.** The fast
  body is only slower than generic because it reads/writes 4-state storage and
  pays boundary conversions (`trunc` the must plane on each load, `to_four` it
  back on each store). Give the tracked signals 2-state storage and the fast
  body is unconditionally cheaper, so specialization is never a loss and the
  feature can be on by default with no gate. Two routes: **static never-x
  inference** (never-x-inference.md — proven-clean signals get `iW` storage,
  zero runtime discriminant, no gate, but can't cover transiently-x reset
  signals like xdomain's); and **dynamic 2-state storage** (a per-signal clean
  bit + must-only fast store + reconstruct-on-read for generic readers —
  covers transiently-x but is a dual-representation scheme, higher debt). A
  two-narrow-store cheapening of the current packed store was prototyped and
  measured a regression (one wide store beats two narrow), so the win is in
  storage width/representation, not store packing.
- Lazy fast-body compilation (ORC) to reclaim the eager compile-time cost
  (both bodies are compiled up front today).
- Coverage: div with provably nonzero divisor; per-element array tracking;
  coroutine processes (needs re-check at resume points); 4-state-return Call
  with a clean-return proof; loop-variable index range analysis (in-range
  dynamic selects landed for width- and constant-bounded indices, which
  covers the shift/localparam idioms; picorv32's integer for-loop indices
  still reject).
- picorv32 diagnosis (2026-07): the reader-side guard over all 196 eligible
  groups costs +0.9% (dispatch is near-free), but the op-thin fast bodies run
  ~10% slower than generic (boundary conversions > op savings). X is not the
  blocker — the op-dense datapath blocks are ineligible by construction:
  picorv32 assigns `'bx` don't-cares as defaults (25 sites: alu_out,
  decoded_rs, reg_sh, ...), so their bodies traffic in x every activation,
  leaving only op-thin candidates. picorv32 is the anti-shape for this feature.
