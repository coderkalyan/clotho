# Per-definition LLVM modules (module split)

Split the single design-wide `llvm::Module` into one module per elaborated
definition content plus a root module, enabling per-definition object caching
(ORC ObjectCache), incremental recompilation on a definition edit, and (later)
parallel codegen. Builds directly on the instance-anchor / instance-ctx /
body-sharing prep: a definition's code already never embeds whole-design layout,
and every per-instance base routes through one `@clotho.inst.<hier>.ctx` symbol.

## Partition

Unit = elaborated definition content. Key: `def_name` + state-type field
sequence + canonical process-body hash set (the twin-detector hash,
`--stat=dedup`), NOT raw `def_name` — one def name with two
parameterizations is two units; twin instances share one unit.

- `M_def(key)`: single-def fusion groups anchored in instances of the def,
  def-declared functions/tasks, per-def NBA commit functions. Exports its group
  / commit / function symbols by stable name.
- `M_root`: `@clotho.state` (+ power-on initializer), all `.ctx` definitions
  and startup fills, `main` / `sim_drain` / finals, active queue,
  `clotho.proc_sched`, NBA dispatch heads + meta + commit-fn table, wait-queue
  globals, VCD setup, DPI export thunks, package/`$unit` (`no_scope`)
  processes. Cross-instance fusion groups of <= 16 members OUTLINE: one plain
  function per member in the member's own unit + a root dispatcher (named
  like the merged thunk; owns the sched-flag timeline) calling them in rank
  order, so the member code is per-definition cacheable. Past the cap the
  merged body stays in root: a cross-module member call cannot inline, and a
  256-member comb group measured 3.7x sim outlined (combchain) while the
  real core populations (2-7 member clocked groups, picorv32) are
  wall-neutral. --no-outline for bisection.

## Context and types

One shared `LLVMContext` for all modules during construction (`Backend`/
`ModuleEnv`/`sym()` are unchanged from V1 — ALL emission still happens into
one context, so every type memo stays shared and no per-module type
re-derivation was needed there). Parallel codegen (landed) does NOT touch
that: instead, `Jit.cpp` migrates each already-built module into its own
fresh `LLVMContext` via a bitcode round-trip (`WriteBitcodeToFile` /
`parseBitcodeFile`) after construction, then runs `optimize_module` + ORC
codegen per module across a thread pool. This sidesteps the "DataLayout-derived
offset recomputation" concern entirely — bitcode reconstructs types
structurally (same field order/types), not by pointer, so GEP offsets
(already plain field-index vectors, context-free) are unaffected; nothing in
`Backend` needed to become context-aware.

Symbols are module-owned, unlike types: a `GlobalVariable*`/`Function*` used
from another module must become an external declaration. `Backend::sym()`
(ir/llvm/Backend.cpp) is that funnel: identity while the definition is local, a
memoized extern declaration otherwise (`ModuleEnv::decls`). Internal constants
with non-unique names (rt_elem_ops descriptors, string sink) duplicate per
module instead (`ModuleEnv::elem_ops` / `str_sink`).

Linkage flips Internal -> External for cross-unit symbols only. GEPs of an
external `@clotho.state` still fold to reloc+addend at codegen, so the
unique-definition constant-anchor shape survives. The
`@clotho.nba.shadow_base` opacity fix (Zen4 memory-renaming hazard) survives
trivially — loads through an extern global are more opaque, not less.

## ORC side

`addIRModule` per unit into the main JITDylib; `lookup("clotho_main")` pulls root and
its extern refs materialize each def module. Eager, sequential, same
semantics. Laziness is deliberately not built: the seed loop references every
group at startup, so the whole design is always reachable. ObjectCache keys on
(unit content key, SimConfig flags, clotho version, triple); a definition edit
changes other units' ctx *values* (root-filled), not their code, by
construction of the anchors work.

## Cache stability: fully relocatable def modules (approved direction)

Goal: def-module object bytes are a pure function of (definition content,
instance name list) — a body edit in def A never changes def B's bytes, only
root's symbol values. Everything resolves at JIT link, zero runtime cost.
Numbers shift on edits; hierarchy-derived *names* don't. No ObjectCache yet —
these phases only make the objects cacheable.

Two mechanism classes:

- **Address families → real data symbols + standard relocations.** Split the
  monolithic `@clotho.state` into per-instance-scope globals
  (`@clotho.inst.<hier>.state`, own signals only — children get their own
  globals; unscoped signals keep a root `@clotho.state`). Def code addresses
  `symbol + def-local offset`: same link-time-constant codegen shape as
  today's constant anchors (pico FastISel win preserved), def-stable bytes.
  Same treatment: per-scope sched arrays, per-instance NBA shadow globals,
  wait queues (already per-signal globals — need hier-stable names). The
  GlobalAlias-into-interior route stays dead (JITLink bounds validation).
- **Integer-id families → loads of root-defined value globals.** VCD var
  index, NBA block id, trigger id, array/intra buffer ids are immediates,
  not addresses. A unit-module use is a 4-byte load of
  `@clotho.idval.<family>.<hier>[+ordinal]`, an i32 the ROOT module defines
  with the value baked (root always recompiles, so no cache-stability cost;
  the unit carries only the hierarchy-derived symbol name). The
  absolute-symbol relocation scheme was tried first and REVERTED: a
  truncating-ptrtoint constant in operand position makes SelectionDAG (the
  -O0 fallback for blocks FastISel misses) keep the symbol address live
  function-wide and spill it — measured +3KB codegen on the extracted pico
  core thunk vs the literal floor, with the instruction form no better; the
  load form lands within 1.7KB of the floor with no spill cascades (llc A/B:
  72248 / 72169 / 70992 / 69298 B). Root-side code keeps plain immediates.

Shadow opacity (Zen4 memory-renaming hazard): per-instance shadow globals are
accessed through per-instance root-filled base-pointer globals
(`@clotho.inst.<hier>.shadow_base`) — one load, exactly today's cost, opacity
preserved, def-stable symbol reference. Direct symbol addressing of shadow is
a later experiment gated on the wideff resync-counter signature.

Shared twin bodies keep the ctx-parameter indirection (one body cannot be
relocated two ways); unshared bodies use direct per-instance symbols. This
replaces the old absolute-vs-ctx `def_relative_for` split with
shared-vs-unshared.

Unavoidable costs, accepted: root always recompiles (whole-design tables,
seed loop, symbol values); hierarchy renames/restructures invalidate
dependent def modules; per-instance stubs regenerate; symbolic immediates
materialize as relocated movs.

State-split measured cost (2026-07-13, S1-S3 landed): tier-0 sim neutral
across the corpus (hazard3 instructions identical, L1d misses −18% after
dropping the 64-byte scope-global over-alignment, wall within noise; wideff
Zen4 resync gate clean at ~106K vs the hazard's ~101M). picorv32 `-O` pays
~+2% cycles (wall +4-5% on a noisy box): L1i misses +30% with instructions,
L1d, branches, and resyncs flat — a code-layout effect of multi-symbol
addressing under the O3 pipeline, not a data-path cost. Accepted: `-O` is
opt-in, and the -O tier rework (the FastISel ceiling) is the right place
to revisit code layout.

## Phasing

Approved scope (2026-07-13): through integer-id relocs; no ObjectCache yet.
State split deliberately lands *before* the module split — it is the delicate
piece, verified in isolation on the single-module pipeline; the module split
then externalizes real symbols instead of interior GEPs.

1. **ModuleEnv + sym() funnel** (landed, "backend: route cross-unit symbol operands through ModuleEnv::sym"): all cached symbol members
   used as IR operands route through `sym()`; single env; byte-identical.
   Misses are caught by the per-module verifier once emission splits.
2. **State split** (single module still):
   a. per-scope state globals (own signals only; shadow leaves the root
      struct and becomes its own global(s)); slot paths become scope-relative
   b. per-instance shadow globals behind per-instance base-pointer globals
   c. per-scope sched arrays
   Gate each step: identical sim output on the perf corpus + full suite;
   wideff resync signature; pico -O spot check (constant-anchor shape must
   survive).
3. **Split emission**: one env per unit + root, `lower_group` looped by owning
   unit (single-scope groups → their def's unit; cross-instance groups,
   functions/tasks, commit fns, all machinery → root, V1), per-module
   `lower_coroutines` + verify, one `addIRModule` per unit, `--no-split`
   escape hatch. Gate: `--no-split` byte-identical to pre-split; split mode
   sim-output identical; compile time neutral or better.
4. **Symbol hygiene**: def-local function naming (`@<defkey>.p<local>` +
   per-instance stubs), hier-stable wait-queue / event / ctx symbol names.
5. **Integer-id relocs**: absolute-value symbols, upfront `absoluteSymbols()`
   registration in Jit.cpp; def bodies emit relocated immediates; drop
   ctx-loads that existed only for id renumbering (shared bodies keep ctx).
6. **Parallel codegen** (landed, feat/orc-parallel-compile): bitcode round-trip
   per module into its own `LLVMContext` + thread pool +
   `LLJITBuilder::setNumCompileThreads`, not a `Backend`-level
   per-module-context rewrite (see "Context and types" above).
7. Later, out of scope now: ObjectCache; direct-symbol shadow addressing
   experiment.

## Status (2026-07-14)

Landed on refactor/module-env: sym() funnel ("backend: route cross-unit symbol operands through ModuleEnv::sym"), state split S1-S3
("backend: split design state into per-scope globals", "backend: split NBA shadow into per-instance globals", "backend: split scheduled flags into per-scope arrays"), split emission ("backend: split emission into root + per-definition modules"), def-local naming
("backend: def-local, instance-qualified names for unit-module processes"), integer-id absolute symbols ("backend: integer design-numbering ids as absolute-symbol relocations"). Suite 991/991 with split
default-on; corpus sim outputs identical in both modes; --no-split
byte-identical to the pre-split build; compile time neutral.

OPEN — split-mode sim regression: combchain +29% cycles (sim-phase branch
misses 5.7M -> 24.8M), pico +5.7% sim, wideff +14% sim. DIAGNOSIS (revised
2026-07-14 — NOT placement): identical IR does not give identical machine
code. objdump of the jitted wideff hot thunk shows FastISel emits different
code depending on symbol linkage: with the state global promoted internal ->
external dso_local (sym()'s cross-unit promotion), the address is
materialized once and stack-spilled/reloaded on always-executed paths, where
the internal form re-movabs'd per use (split thunk: 778 vs 1040 movabs, 524
fewer static instructions, yet more executed per activation). Measured
sim-phase deltas: wideff +7.5% instructions (+416/cycle, mispredicts
near-flat); pico +6.7% instructions; combchain instead shows taken-branch
mispredicts 2.7M -> 13.4M (indirect flat — dispatch/stub theories dead),
plausibly downstream of the same codegen shift moving branch layout in its
giant root thunks. Supporting facts: post-pipeline IR byte-identical modulo
absid immediates; addIRModule order reversal is a no-op perturbation
(materialization order is dependency-driven from lookup("clotho_main")); miss
counts run-stable under ASLR; --perf-jitdump inflates branch-misses ~7x
(never attribute misses under it); compile-phase counters clean everywhere.

FIXED ("backend: mark sym() cross-unit declarations dso_local"): sym() created its cross-unit declarations without
dso_local — a non-dso_local external is addressed conservatively (address
stack-spilled/reloaded on hot paths). Marking the declarations dso_local
(everything links into one JIT dylib) restored the direct form: wideff
instruction bloat gone (below no-split), combchain flipped to a WIN (cycles
+29% -> -7.5% vs no-split, taken-mispredicts 13.4M -> 3.5M), pico compile
-4%. Follow-up: absid addend folded into the symbol
reference as a byte GEP — one relocated movabs per id, no add.

RESIDUAL: pico split sim +4% (compile -4%; net wall ~+1%). Attribution
bottoms out in sampling artifacts: sim_drain's IR is byte-identical modulo
unit-fn renames (3205 lines both, 170 rename-pair diff) yet gets +18%
instruction attribution — return-skid from callees; the hot core thunk's
own attribution is equal between modes. The idval load form eliminated the
absid spill cascades (hot thunk codegen now within 1.7KB of the literal
floor). Remaining candidates if this is ever worth more: IBS (skid-free)
attribution, per-activation instruction microbench of one unit thunk in
isolation. Split default stays ON in the branch; final default decision at
merge weighs pico's +4% sim / -4% compile against combchain's -7.5% win
and the caching payoff.

Also fixed in flight: a zero-valued integer id defined its @clotho.absid.*
symbol at address null, which the RTDyld object layer (--perf-jitdump path)
reports as unresolved and aborts — values are registered +1 with the -1
folded into the relocation addend ("backend: bias absolute-id symbol values by +1").

Split stays default-on within the branch for soak; the default must be
re-decided on measurement before any merge to main.

## Known landmines

- `share_twin_groups` compares/rewrites symbol operands by pointer identity —
  runs within one def module in phase 2 (its groups are same-def by
  construction), keep it env-local.
- `wait_base` / seed loop / NBA meta initializer assume queue globals resolve
  as constants — in split form these are root-defined, def-declared; the meta
  table itself stays root-side.
- Null-by-construction symbol members (an absent inactive queue): `sym()` must
  never see null — keep the null checks *before* the sym call.
- Module count explosion on unique-def designs (hazard3-class): merge
  singleton defs below a group-count threshold into root if per-module fixed
  cost shows up.
- Root's unbounded size is now also a *compile-parallelism* ceiling, not just
  an object-cache-granularity one: measured on rocket-chip (255 modules), root
  alone is 30 MB of bitcode (16x the next-largest module) and its own
  single-threaded optimize+codegen time is the entire parallel section's
  wall-clock floor, capping the win at ~1.5x on 12 cores despite 260
  independent modules. Raising this ceiling needs root itself split further
  (more of `M_root`'s machinery outlined the way small cross-instance groups
  already are), not more parallel-compile machinery.
