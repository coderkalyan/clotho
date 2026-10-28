# Static-index array NBA → block-commit

**Status:** designed, deferred (not a current bottleneck). Scope decision made:
*all-or-nothing per array* (see below). No code landed; this doc is the
implementation spec.

## Problem

A non-blocking assignment to an unpacked-array element (`mem[k] <= rhs`) always
stages into a per-array runtime *record buffer* (`rt_nba_arr_slot`), even when
the element index is a compile-time constant. Each such write costs a runtime
`rt_nba_arr_slot(idx, stride)` call, a store of the element's live pointer +
positioned value into the record, and, at the NBA region, a per-record replay
loop (`emit_array_commit`).

A whole-element write at a *statically known* index is a pseudo-scalar: its live
address is a compile-time-constant GEP (`array_base + k*stride`). Such writes
should ride the scalar **block-commit** path (shadow slot + one block
registration + straight-line commit), dropping the runtime slot call, the
per-write pointer store, and the record loop.

This is a known unexploited win, not a current bottleneck — the record buffer
is a capacity-reused `std::vector` per array (zero steady-state allocation) —
so this is a per-write constant-factor win, not an allocation win.

## Current architecture (extension points)

All references `src/ir/llvm/` unless noted.

**Shadow model is keyed per-signal.** Each scalar NBA-target signal gets a dense
`nba_id` (`nba_classify`) and a shadow slot in the `clotho.nba.shadow`
struct — `{ i(2W) mm, iW z?, iW dirty-mask? }`, appended to the root state
(`build_state_types`). Shadow is addressed by `shadow_ptr(field)`
(through the `@clotho.nba.shadow_base` global — a deliberate defense
against a Zen4 memory-renaming hazard). Live storage is a constant GEP via
`storage_ptr`.

**Block-commit** (`nba_block = !s.is_array`). A block = a straight-line
segment delimited by `cur_seg_epoch_` (bumped at region entry/exit and at
Delay/Wait/TaskCall). The first NBA of a segment calls the enqueue stub once
(`block_register`); each target is gathered into `block_commits_[id].sigs`
as a `BlockSig{sig, mask, dynamic}`. At the NBA region the pending blocks
musttail-chain their commit fns; `emit_block_commit_body` copies each
target's shadow → live (`storage_ptr`, no element index) and services its
per-signal wait queue via `emit_nba_service` + `nba_meta` (indexed by
`s.nba_id`).

**Array record path.** Array targets are collected in `array_nba_sigs_`
(`nba_classify_ir`) and never marked `nba`. `emit_array_nba`
stores `(live element ptr, positioned value, optional mask)` into a runtime
record. `emit_array_commit` replays records in push order into live
element slots, then — if the array took any write — wakes its readers via
`service_any_waiters(sig)`.

**The never-`nba` rule has a second consumer.** An intra-assignment nonblocking
write to an array element (`mem[i] <= #5 v`, LRM 9.4.5) uses its own deferred
records (`intra_nba_sigs_`, `docs/design/intra-assignment-timing.md`) and must obey
the same rule: `classify_intra_target` skips `nba` for an array target, and
`emit_intra_commit` wakes it through `service_any_waiters` plus a whole-range VCD
touch rather than the `nba_meta` comparison. Marking an array `nba` would index
`nba_meta` by an `nba_id` it never received — an out-of-bounds read with no compile
error. The cross-region ordering caveat below applies to that path too.

**Array waiter semantics (key simplifier).** Memory elements carry only
**Any-edge (level)** sensitivity — slang rejects `@(posedge mem[i])` — so
`service_any_waiters(array_sig)` wakes the array's Any-edge static + dynamic
queues **unconditionally** (no edge/value detection). It is **idempotent**: the
per-process scheduled-flag dedup makes a redundant wake a no-op.

**Constant-index detection.** When slang const-folds the selector,
`ElemSelect.index` (`Ir.h`) is an `Inst::Tag::Constant` node
(`src/lower/`). Two detection points:
- IR level (in `nba_classify_ir`): `ir.tag(ElemSelect.index) == Constant`,
  parallel to how `arr_masked` is set.
- Backend LLVM level (in staging): `lv.elem_idx` is an `llvm::ConstantInt`
  exactly when the index const-folds (all of `cast`/`Sub`/`Mul`/`Add` fold);
  `elem_idx == nullptr` means element 0. Idiom already used
  (`is_zero_const`).

## Design

### The NBA target unit becomes `(signal, optional const leaf-index)`

Introduce an **element-NBA-target registry** parallel to the per-signal
`nba_id`: `ElemNbaTarget { Signal sig; uint32_t index; uint32_t width; bool
has_z; bool narrow; uint32_t sh_mm_field, sh_z_field; }`, plus a map
`(sig, index) → target id` for dedup and staging lookup. Each distinct
constant-index whole-element target gets its own shadow slot, appended to the
`clotho.nba.shadow` field list after the per-signal slots (`build_state_types`).

### All-or-nothing per array (the scope decision)

An array qualifies for element-block-commit **iff every NBA write to it is a
constant-index whole-element write** — no dynamic index, no within-element
bit/part-select. Otherwise the array stays entirely on the record path
(unchanged).

**Why.** NBA is last-write-wins in program order. The block-commit region and
the array-record region commit in different phases, so mixing a constant-index
block-commit write (`mem[3] <= a`) and a dynamic-index record write
(`mem[i] <= b`, `i == 3` at runtime) on the same array could invert the winner
across regions. Requiring the whole array to be static-only removes the record
buffer for it entirely, so no cross-region ordering exists.

Within a qualifying array, correctness holds:
- Distinct constant elements → independent shadow slots.
- Repeated writes to the same element (same block, or two blocks) → the element
  shadow holds the last *staged* value (active-region order), and commits are
  idempotent copies — identical to scalar block-commit semantics.

### Staging

In the `StoreNb` dispatch, before the `is_array` → `emit_array_nba`
branch: if the array is qualifying and `lv.elem_idx` is constant (or null),
look up the element target id, store `rhs` into its shadow slot, and
`block_register` it (extended `BlockSig` carrying the constant element offset /
target id). Otherwise fall through to `emit_array_nba`.

### Commit

Extend `BlockSig` with an optional element target id (or an "is-element" flag +
constant element index). In `emit_block_commit_body`:
- Scalar `BlockSig`: unchanged.
- Element `BlockSig`: live address is a constant element GEP
  (`array_base + index*stride`, the `lv_field_ptr` array form), shadow is
  the element target's `sh_mm_field`/`sh_z_field`. Copy shadow → live
  (whole-element, always the unmasked path — partial writes disqualified the
  array). Then wake the array once via `service_any_waiters(array_sig)`.

**Waiter servicing is idempotent**, so no cross-block dedup is needed: any block
that committed an element of `mem` calls `service_any_waiters(mem)`; redundant
calls are no-ops via the scheduled flag. (Optionally dedup within a block to
save a call, but not required for correctness.)

### Gate

A `static_arr_nba` gate in `SimulationConfig` disables the routing (all array
NBAs fall back to the record path), mirroring `fuse` / `thunk` for differential tests.

## Extension points (touch list)

1. **Classification** — `nba_classify_ir`: per array, detect
   disqualifiers (`lvalue_has_dynamic_select` / `lvalue_has_subselect`, existing)
   and, if clean, collect distinct constant indices (IR `ElemSelect.index ==
   Constant`). Mark the array qualifying and build the element-target registry.
2. **`nba_block` exclusion** — `build_nba_state`: unchanged for the
   array signal itself; element targets are a separate registry, not
   `slot.nba`.
3. **Shadow layout** — `build_state_types`: append element-target shadow
   slots after the per-signal ones.
4. **Staging** — `StoreNb` dispatch + a new element-staging helper.
5. **Commit** — `BlockSig` struct, `block_register`, and
   `emit_block_commit_body`: handle element targets (constant element
   GEP + `service_any_waiters`).

## Verification

- Full test suite byte-identical.
- New iverilog-checked integration tests: qualifying array (const-index
  whole-element writes, single and multiple elements, two processes writing the
  same element, same-element twice in a block); disqualified arrays (dynamic
  index, within-element partial) confirmed unchanged; a mixed design (one array
  qualifies, another on the record path).
- `--no-static-arr-nba` A/B byte-identical (the routing is an as-if
  optimization).
- picorv32 to TIMEOUT byte-identical (its register file is dynamic-index, so it
  should *not* qualify — a good negative check).

## Effort / risk

Real refactor of the perf-critical NBA core (a Zen 4 mis-speculation
hazard). Generalizes the NBA target unit from per-signal to
per-`(signal, const-index)`. Modest, non-bottleneck win. Deferred until it is
worth the risk budget.
