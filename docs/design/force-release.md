# Procedural continuous assignment: force/release and assign/deassign (LRM 10.6)

Status: landed for whole-scalar variables (force/assign/release/deassign, writes of any
shape gated) and for nets (whole, plus constant bit/part-select force). `force` of a
select or concatenation of a *variable* stays guarded loud.

## Semantics (LRM 10.6)

Four statements, two priority layers over a signal's ordinary drivers:

- `force lhs = rhs;` (10.6.2) — highest priority. Overrides *every* other driver
  (procedural, continuous, gate, resolved-net) on `lhs` until released. `rhs` is a
  **continuous** relationship: re-evaluated whenever it changes. Legal on both nets
  and variables. May be conditional / impure (`if (c) force x = $random`), so which
  force is active is a runtime fact.
- `release lhs;` — ends the force.
  - Net: reverts **immediately** to the resolved continuous-driver value.
  - Variable: **holds** the forced value until the next procedural write, *unless* a
    procedural continuous `assign` is active on it, in which case it reverts to that
    assign's value.
- `assign lhs = rhs;` (10.6.1) — procedural continuous assignment. Variables only.
  Overrides ordinary procedural writes, is itself overridden by `force`. Continuous
  relationship like `force`.
- `deassign lhs;` — ends the `assign`. Value **holds**.

Priority ladder: **force > assign > ordinary (procedural / continuous-net) driver**.

slang statement kinds: `ProceduralAssignStatement{.assignment, .isForce}` (force vs
assign), `ProceduralDeassignStatement{.lvalue, .isRelease}` (release vs deassign).

## Why not the Verilator/Antmicro model verbatim

Antmicro's Verilator work (blog 2026-05) keeps a per-forced-signal `VlForceVec` — a
runtime sorted set of forced ranges, each pointing at RHS shadow storage — overlaid
onto the signal at settle time. Correct core insight: force state must be **runtime**,
not compile-time-resolved (their old static shadow-signal scheme could not do
conditional / impure-RHS force). We adopt that insight but not the structure:

- The range-vec is a consult-*on-write/settle* structure built for Verilator's static
  per-cycle schedule. Clotho is event-driven; continuous drivers are already
  *processes*, and multi-driver nets already resolve through a generated *resolver
  process*. Force fits that machinery, not a bespoke overlay vector.
- For the whole-signal common case a flat per-signal override state + a generated
  continuous process is cheaper and simpler than a range set. The range set only earns
  its keep for **partial (bit/part-select) force with runtime bounds / overlap** —
  deferred to Phase 2.
- Clotho's whole-design JIT lets us scope *all* overhead to the statically-known
  **forceable set** (any signal that is a force/assign target anywhere). A design with
  no force/assign pays exactly zero — no state, no gate, no branch.

## Model

### Forceable set

A pre-pass collects every signal that is a whole-signal `force`/`assign` target. Only
these carry override state and gated writers. Everything else lowers unchanged.

### Per-forceable-signal state (hidden signals)

- `force_site` (2-state i32, 0 = none) — id of the currently-active force site.
- `assign_site` (2-state i32, 0 = none) — id of the currently-active `assign` site;
  allocated only for variables that are an `assign` target.

Site ids disambiguate re-force / re-assign (LRM last-wins): each textual force/assign
site targeting a signal gets a unique per-signal id; a stale site's re-eval process
sees `force_site != its id` (or `assign_site != its id`) and stops writing. No
`assign_val` is stored — `release` reestablishes an active `assign` by re-waking that
`assign`'s own re-eval process (below), which recomputes the RHS.

### Lowering (variables)

The apply/re-eval writes to `v` are plain `Store`s emitted directly by the force/assign
lowering, so they bypass the ordinary-writer gate (they are the override). The backend
already services `v`'s wait queue on any `Store`, so a forced/assigned change wakes
readers with no extra machinery.

Per force site `k` on variable `v`:
- statement `force v = rhs`:  `force_site = k; Store v = rhs`
- generated Always process, sensitive to rhs reads:
  `if (force_site == k) Store v = rhs`

Per `assign` site `k` on `v` (priority below force):
- statement `assign v = rhs`: `assign_site = k; if (force_site == 0) Store v = rhs`
- generated Always process, sensitive to rhs reads **and `force_site`**:
  `if (assign_site == k && force_site == 0) Store v = rhs`
  (the `force_site` sensitivity is what makes `release` reestablish the assign).

- `release v`: `force_site = 0`. Storing `force_site` services its wait queue, waking
  the `assign` re-eval process (if any) and any `force_site`-sensitive continuous
  driver, which reverts `v` to the active `assign` / continuous-assign value; with
  neither, nothing re-wakes and `v` holds (LRM 10.6.2).
- `deassign v`: `assign_site = 0` (hold; the assign re-eval sees `assign_site != k`).

A variable driven by a module-level continuous `assign` (`assign v = expr;` at module
scope, not the procedural statement) is handled the same way as the `assign`-statement
layer's reestablish: `lower_continuous_assign` appends `force_site` to that driver's
sensitivity (`with_force_wake`) and the driver is gated through the ordinary path, so
`release` re-wakes it and reverts. Input/inout-port variables (driven by an external
port-copy process that is not `force_site`-aware) are guarded loud.

Ordinary procedural writers to `v` (blocking `Store`, nonblocking `StoreNb`) are gated
`if (force_site == 0 && assign_site == 0) <store + wait-service>`. This gives
release-hold for free: while forced/assigned the write is dropped; after release the
gate opens and the next procedural write lands, with the forced value persisting until
then.

The gate keys off the write's **root**, not its shape, since the site state lives on the
root signal — so a whole-variable write, a bit/part/element-select or packed-struct
member of it, a read-modify-write (`x += 1`, `x++`), an operand of a concatenation
lvalue, and a task/function output copy-back all take the same gate (`store_or_gate`).
LRM 10.6.2 suppresses the *update* only: the RHS, the lvalue's selectors and a
read-modify-write's read all still run, so their side effects stand and `x++` still
yields the forced value. A select's store is narrowing over storage that already holds
the forced value, so dropping it leaves that value exactly in place.

The override writes bypass the gate simply by being emitted directly (not through the
gated `lower_assign` path), so no dedicated `OverrideStore` op is needed — a plain
`Store` the backend already services suffices. State-signal writes (`force_site = k`,
`assign_site = k`) are plain stores to non-forceable hidden signals, so they are never
gated.

### Where the gate lives (frontend, not backend)

The implementation is **frontend-only** — no new IR ops, no backend changes. The gate
is applied where the store is emitted (`store_or_gate`, reached from `lower_assign`'s
`gated_store`, the `++`/`--` path, the concatenation scatter and the call copy-out):
when the written lvalue's root signal is forceable, that store alone is wrapped in the
`force_site==0 && assign_site==0` conditional (a `cond` region). This reuses clotho's
existing conditional-region and continuous-process machinery; the earlier idea of a
backend `Store`/`StoreNb` choke was unnecessary once the gate could sit at the frontend
store sites. Intra-assignment timing on a write to a force target (`x = #5 e`) is
guarded loud: the update commits after the delay, so the gate would have to ride the
deferred record rather than the sampling site.

## Phasing

- **Phase 1** (this branch): whole-scalar **variable** force/release. Includes
  release-reestablish of a variable driven by a module-level `assign` — the driver's
  continuous process is made `force_site`-sensitive and gated `if (force_site === 0)`,
  so `release` (force_site → 0) re-wakes it and it reverts the variable to the driver's
  current value (LRM 10.6.2). Input/inout-port targets (external port-copy driver, not
  force_site-aware) and non-integral targets are guarded loud. Writes of any shape
  rooted at the forced variable are gated (see above); `force` of a select/concat of a
  variable stays guarded loud.
- **Phase 1b** (this branch): procedural continuous `assign` / `deassign` (LRM
  10.6.1) on the same whole-scalar variables. A second override layer keyed by an
  `assign_site` state signal, priority **force > assign > ordinary procedural**:
  ordinary writes gate on `force_site==0 && assign_site==0`, the `assign` re-eval
  process gates on `force_site==0` (so force overrides it) and is also
  force_site-sensitive so `release` reestablishes it (LRM 10.6.2 revert-to-assign);
  `deassign` clears `assign_site` and the value holds. Both `force` and `assign` are
  last-wins via their per-site ids. The full ladder is byte-identical to iverilog 13
  (`tests/force_release_test.cpp`).
- **Phase 1.5** (landed): whole **net** force/release. A forced net always resolves
  through `emit_wire_resolvers` (forceable nets are added to `resolved_nets_` and to the
  resolver set even when single-driver or undriven). The resolver injects
  `select(force_site === 0, <driver fold>, force_val)` as its final value and adds
  `force_site` / `force_val` to its sensitivity, so a force apply and a `release` both
  re-run it — `release` reverts to the driver-resolved value immediately (or z for an
  undriven net). Unlike a variable, a net has no procedural writers to gate and no
  hold-on-release, so net force is *purely* the resolver override: `force n = rhs` sets
  `force_site`/`force_val` and arms a re-eval process tracking rhs into `force_val`;
  `release n` clears `force_site`. `assign`/`deassign` do not apply to nets. Uniform
  over single-driver, gate-driven, multi-driver/resolved, and undriven nets.
- **Phase 2** (landed for nets): constant **bit/part-select force of a net**. Each
  `force` statement on a net (whole or a constant select) registers a `NetForceSite
  {active, val, [lo,hi]}` — `val` is exactly range-wide. The net resolver stores the
  driver fold to the net, then for each site emits `if (active) net[hi:lo] = val` (a
  positioned store to the net *signal*, an lvalue — a bitwise mask/or would corrupt z).
  A released site (`active == 0`) is skipped, so re-running the resolver reverts those
  bits to the fold; `release net[hi:lo]` clears every site whose range is covered; a release that overlaps a
  site without covering it is a fatal run-time error while that site is active, since a
  site is released as a unit.
  Non-overlapping sites are exact; simultaneously-active overlapping partial forces are
  order-dependent (rare, LRM-unspecified). Byte-identical to iverilog.
  - **Not** the sorted-range `VlForceVec`: constant selects give compile-time-known
    ranges, so a fixed per-site `{active, val}` pair with a positioned store is simpler
    and exact — a range vector is only needed for runtime-variable bounds, which
    §10.6.2 does not permit (it requires *constant* selects).
  - Bit/part-select force of a **variable** is forbidden by §10.6.2 ("shall not be a
    bit-select or a part-select of a variable") — rejected loud — even though iverilog
    accepts it. An open decision (docs/TODO.md).

## Oracle

iverilog 13 supports force/release and assign/deassign on variables and nets — the
primary oracle. Release-hold-vs-revert-to-assign (LRM 10.6.2) is the subtle corner
to pin.
