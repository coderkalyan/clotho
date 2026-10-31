# Per-definition LLVM modules (module split)

Split the single design-wide `llvm::Module` into one module per elaborated
definition content plus a root module, enabling per-definition object caching
(ORC ObjectCache), incremental recompilation on a definition edit, and (later)
parallel codegen. Builds directly on the instance-anchor / instance-ctx /
body-sharing prep: a definition's code already never embeds whole-design layout,
and every per-instance base routes through one `@clotho.inst.<hier>.ctx` symbol.

This is a partition of the backend's output, drawn after every whole-design
decision; `modular-compilation.md` replaces it with units the frontend produces
and the backend compiles independently.

## Partition

Unit = a `ir::Design` unit (one specialization), keyed by `InstanceScope::unit`;
a scope built without a unit is a unit of its own. Its module is named by the
unit key (`InstanceScope::unit_key`, from flatten): the definition name, plus a
hash of the unit's body when several units share the name.

- `M_unit`: the groups of the unit's instances, lowered once per family of
  instances that lower alike and run with the instance's record; the functions
  and tasks of the unit, once per family; the entry of a schedule group of one
  row (it takes the record).
- `M_root`: everything else -- instance storage, records, wait queues, the
  trampoline each instance's one-row group hands the entry its record through,
  dispatchers of schedule groups spanning instances, NBA block commit functions
  (not yet in the writer's unit), `clotho_main` / `sim_drain` / finals, the seed
  loop, DPI export thunks, package/`$unit` processes, and the lazily created
  singletons (rings, `clotho.enqueue_timer`, `clotho.ev_delay`, trampolines),
  which are defined in root whichever unit asked first.

What the scheduler holds is the entry of a SCHEDULE GROUP: the rows that wait on
the very same (signal, edge) set, whichever instances and units they belong to,
share one subscription and one scheduled flag. The entry owns the flag timeline,
re-arms it, and calls each row with its record in rank order. One row's entry is
unit code beside a body the inliner may fold in; several rows span units, so the
dispatcher is root glue and its row calls stay calls (`NoInline`). A coroutine
group is one function over one frame, so it is its own schedule group.

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

## Cache stability

A unit module's text is a function of the unit and the facts its context
imposes on it (modular-compilation.md, "Code generation"): its symbols lead
with its unit key, its code reaches its instance through the record it is handed
and other units through bindings, and what it names in root is a link symbol
with a stable name (`clotho.nba_block_enqueue`, `clotho.active*`, `clotho.rng`,
`clotho.wheel`, `clotho.enqueue_timer`), declared without a size. The
modular-compilation gate test (tests/modular_gate_test.cpp) checks that an edit
to one definition changes only that unit's module. `clotho build --emit
llvm-units -o <dir>` writes one file per module for looking at it.

Integer ids of design-wide spaces (VCD variables, NBA blocks, triggers, buffers)
are record fields (base plus unit-local ordinal), not immediates or symbols.
Measured history of the earlier symbol-relocation experiments (absolute-symbol
ids, per-scope state globals) is in git history.

## Known landmines

- A unit's group is lowered once and shared by the unit's instances
  (Backend::run, "code once"); the instances' groups live in one def module, so
  the shared function is env-local. A lowering that names something of the
  instance it ran for (`taint`) is not shared.
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
  (more of `M_root`'s machinery moved into the units the way the rows of
  schedule groups already are), not more parallel-compile machinery.
