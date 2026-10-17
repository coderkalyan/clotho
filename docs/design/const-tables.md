# Runtime-indexed constant tables

**Status:** landed (option B, below). Line references are "Merge feat/copyclass: fork join_any and join_none" (`main`),
i.e. the state of the tree before the change, file `src/Lower.cpp` unless
stated.

## Problem

A compile-time table indexed by a runtime value does not lower:

```systemverilog
module top;
  localparam int P[0:3] = '{1,2,3,4};
  logic [1:0] i;
  logic [31:0] x = P[i];   // error: reference to unknown signal 'P'
endmodule
```

The idiom — a ROM/LUT held in a `localparam` (or a package parameter, or a
module parameter overridden per instance) and read with a runtime index — is
common in real RTL. Clotho supports the *packed* form of the same shape
(`parameter [7:0] R; R[i]`, added for hazard3's `PMP_HARDWIRED[i]`, see
`tests/packed_range_test.cpp:366`) and the *variable* form (`const int P[0:3]`),
but not the unpacked-parameter form.

## Why it fails

Clotho reaches a value in exactly two ways: fold it to a pooled constant, or
address it through `sigmap_` (`signal_of`, :3676). Elaboration constants are
deliberately kept out of `sigmap_` — `collect_scope_signals` registers only
`Net` / `Variable` (:3547), `collect_global_scope_signals` only `Variable`
(:3468) — so a parameter has no storage to address. A runtime index also
defeats folding, because `expr.eval` returns `bad()`.

| # | hop | line | outcome |
|---|---|---|---|
| 1 | `lower_expr`, `ElementSelect` rvalue | :17609 | calls `try_lower_constant_select` |
| 2 | `try_lower_constant_select` | :3879 | `expr.eval` is `bad()` (runtime `i`) → `nullopt` |
| 3 | `select_base_lvalue` | :17612 → :3901 | guard at **:3903** requires `base.type…isIntegral()`; an unpacked array is not integral, so the spill at :3908 is skipped |
| 4 | fallthrough | :3926 | `return lower_lvalue(base, b);` |
| 5 | `lower_lvalue` → `value_lvalue` | :18942 → :19287 | no storage source matches → `b.signal(signal_of(vs), l)` |
| 6 | `signal_of` | :3676 | `sigmap_` miss |
| 7 | **fail** | :3688 | `reference to unknown signal 'P'` |

The packed sibling survives only because :3903 passes and the constant is
spilled into a fresh `__taskarg` temp (`make_arg_temp`, :20705) — per access,
not once.

## Blast radius

Measured with `./build/clotho run` at "Merge feat/copyclass: fork join_any and join_none", i.e. before this change; every
row a separate one-module source. Every failing row below works now.

| shape | today |
|---|---|
| `P[2]` constant index | folds (`try_lower_constant_select`) |
| `P[g]` genvar index | folds |
| `$size(P)` / `$bits(P)` | folds in slang |
| unpacked-struct parameter, whole read / member of a constant index | works |
| **`P[i]` runtime index** | `reference to unknown signal 'P'` |
| `q = P` whole-array copy | same |
| `P[i][j]` (multi-dim, and array-of-packed with an inner select) | same |
| `P[i].field` (array of packed structs) | same |
| `foreach (P[k]) … P[k] …` | loop nest lowers; body fails the same way |
| `f(P)` parameter array as a subroutine actual | same |
| `wire [31:0] x = P[i]` continuous assign | same |
| package `localparam int P[0:3]`, `P[i]` | same |
| `parameter int P[0:1]` overridden per instance, `P[i]` | same |
| class `parameter int P[0:1]`, `P[k]` in a method | same |
| `localparam real R[0:1]`, `R[i]` | same |
| `localparam string S[0:1]`, `S[i]` | same |
| `localparam string S = "abc"` (whole read) | `only integral and real constant parameters are supported` (:3450) |
| **`const int P[0:3] = '{…}; P[i]`** | works — module scope, package scope, and under repeated instantiation |

So the workaround available today is to demote the `localparam` to a `const`
variable, which is impossible only for an overridable module parameter or a
class parameter.

`lower_constant` (:3440) rejects any non-integral, non-real `ConstantValue`
(:3450), so the whole-array and class-`obj.P` (:17665) shapes fail there rather
than at `signal_of`.

## What already exists

- **Per-access packed spill** (:3901-3915). Elaboration-constant base, integral
  only, re-materialized at every select site.
- **The `const` variable path**, which already does everything this feature
  needs: signal registered in Collect (:3547), initializer emitted into the
  run-once `PreInit` process (`lower_var_initializer` :11287,
  `finalize_preinit` :11315), element addressing and LRM 7.4.6 out-of-range
  behavior for free.
- **Backend read-only globals**: `agg_proto_global`
  (`src/ir/LlvmBackend.cpp:1257`), `elem_ops_global` (:1290, memoized by element
  type), `vtable_global`, the enum name/value tables — all
  `isConstant=true` `InternalLinkage` globals. None of them holds user design
  data today.
- **Element addressing is storage-agnostic**: the `ElemSelect` lvalue case
  (`src/ir/LlvmBackend.cpp:10338`) consumes only `LValue::base` / `elem_idx` /
  `array_ty`, never `storage_ptr` directly, so a table addressed out of a
  constant global reuses the whole bounds/normalization path unchanged.

## Constraints

1. **Sensitivity.** `SensitivityCollector` (:458) documents the invariant
   "symbols not in `sigmap` (parameters, genvars, other constants) carry no
   sensitivity and are skipped". Putting a parameter in `sigmap_` silently adds
   it to every inferred sensitivity list that reads it. Harmless at runtime
   (`PreInit` stores service no wait queue) but it inflates wait queues and
   breaks the documented rule; `record_read` must skip const-table signals.
2. **VCD.** `vcd_index_signal` (`src/ir/LlvmBackend.cpp:16146`) dumps every
   named signal of a scope. A materialized table would appear as a design
   variable. This is already a live bug for the packed spill — dumping the
   existing hazard3-shaped case emits `$var reg 8 # __taskarg0 $end`. One
   `hidden` flag on `SignalInfo` fixes both.
3. **Per-instance values.** slang gives each instance distinct `ValueSymbol`s
   (`lower_instance`, :1044), and `emit_state_global`
   (`src/ir/LlvmBackend.cpp:2388`) creates one state global per anchor with no
   dedup, so naive signal storage duplicates the table bytes and its
   initialization once per instance.
4. **Layout is a backend decision.** `lower_constant` has no aggregate branch;
   `InternPool::put_value` takes a flat byte blob, while the actual element slot
   layout (may-z narrowing, 2-state vs 4-state planes) is computed in
   `build_state_types` (`src/ir/LlvmBackend.cpp:1797`). A baked image must
   therefore be assembled backend-side from a canonical pooled blob, not
   pre-laid-out in the frontend.
5. **Signal creation order.** The `Pass` model (:1038) says only Collect
   creates signals, but `make_arg_temp` already mints signals during Emit and
   appends them to the open scope. Lazy, demand-driven materialization during
   Emit is therefore viable; it must be deterministic (memoized per
   `ParameterSymbol*`) so signal order stays stable.
6. **Out-of-range reads.** The existing element-select path already implements
   LRM 7.4.6 (measured: `int` element → `0`, 4-state element → `x`); a
   materialized table inherits it with no new work.
7. **Immutability.** slang rejects any write to a parameter, so read-only
   storage needs no runtime enforcement beyond a backend assert.

## Cost measurements

Existing element-wise `PreInit` seeding, measured as total `clotho run` wall
time on the equivalent `const int P[0:N-1] = '{…}` form (an `initial` that reads
one element; the sum-loop variant is within noise of these numbers):

| entries | with initializer | same array, no initializer |
|---|---|---|
| 16 | 0.02 s | — |
| 1024 | 0.05 s | — |
| 4096 | 0.13 s | — |
| 16384 | 0.50 s | 0.02 s |
| 65536 | 2.10 s | 0.03 s |

≈32 µs of JIT compile time per element, i.e. one IR store plus its LLVM
instructions per entry. Acceptable for a 4-entry LUT, not for a 64K-entry ROM,
and paid again per instance.

## Options

Common frontend contract for all three: materialize the parameter as a signal
carrying a pooled constant image.

- **A — element-wise `PreInit` seeding.** Reuse `emit_struct_store`. Smallest
  diff (no IR/backend change at all), but keeps the 32 µs/element compile cost,
  per-instance duplication of both data and init code, and is throwaway once B
  lands.
- **B — baked static initializer.** `SignalInfo` gains `const_data` (a pooled
  blob `Ref`); the backend builds the state field's initializer from it in
  `slot_const` (`src/ir/LlvmBackend.cpp:9335`) and skips the X/0 runtime fill in
  `emit_array_init` (:9406). Zero init instructions, zero runtime cost, data
  still duplicated per instance in `.data`.
- **C — `.rodata`-backed storage.** The signal gets no state field;
  `storage_ptr` (`src/ir/LlvmBackend.cpp:2297`) returns an `isConstant=true`
  global memoized by the pooled blob `Ref`, so every instance sharing a
  parameterization shares one copy. Element addressing is unchanged (see
  above). Needs a `storage_ptr` / `build_state_types` branch and a
  no-store assert.

**Chosen: B.** It is the smallest change that has the right cost profile
(nothing executes at time zero) and it shares its whole frontend contract with
C, so C stays available as a backend-local follow-up for many-instance designs
with large tables. A was rejected: strictly more compile time, strictly more
duplication, and its emission code would be thrown away.

## What landed

Two materialization modes, chosen per type by whether the value has a static
image. `const_table_signal` (`src/Lower.cpp`) is the single entry: it registers
one signal per parameter symbol, memoized in `sigmap_`, and either bakes or
seeds it.

**Baked** — everything with a plane image: integral, real, enum, packed struct,
class/chandle handles (`null`), fixed unpacked arrays of those at any nesting,
and unpacked structs. `append_const_image` / `append_svint_bytes` flatten the
slang `ConstantValue` leaf-major — an array is its elements back to back (what
`InternPool::compute_layout` sizes an `UnpackedArray` value at), an unpacked
struct is its fields in declaration order (that type has no pooled layout of its
own, so the image is only ever read back through the same walk) — and the image
is interned into `SignalInfo::const_data`. Nothing executes at any point: the
backend's `slot_const` returns the image as the state field's static
initializer, decoding it into the signal's own slot layout (`const_data_init` →
`const_slot` for flat storage, `const_agg` walking the IR type and the storage
type in lockstep for the aggregate mirror, since `agg_storage_ty` mints a fresh
`StructType` per call). `init_state` skips these signals so the X-fill cannot
erase them, and `build_state_types` ORs `const_image_has_z` into `has_z` — the
may-z analysis proves a table never-z from an empty seed set, but a parameter may
itself hold z.

**Seeded** — anything holding a heap-backed leaf (`has_heap_leaf`): string,
queue, dynamic array, associative array, and aggregates containing them. Only
the runtime can construct those handles, so `emit_const_seed` writes the value
once into the run-once `PreInit` process: `Store` per leaf (the backend routes a
string destination to `rt_string_assign`), `DynArrayNew` + element stores,
`QueueDeleteAll` + `QueuePushBack` per entry, `AssocReset` + an optional default
store + one store per entry. It walks the elaborated `ConstantValue`, not the
parameter's initializer expression: a defparam / config / `--G` override installs
a value with *no* initializer (slang `ParameterBuilder::createParam` early-returns
after `setValue`), and an instance override's expression is bound in the
instantiating scope rather than the one being lowered. Seeds are emitted at first
reference, so a declaration initializer that reads a table (`string t = S[1];`)
still sees it: the seed instructions land in `preinit_` before that statement's
own store.

**Shared.** `value_lvalue` routes a `Parameter` / `Specparam` root to
`const_table_signal` instead of `signal_of` — the single hook every failing shape
shared, so element and range selects, `lower_lvalue` recursion, member access,
whole-array copy and by-value actuals all reach it. `lower_constant` grew
`isString` / `isNullHandle` branches, which also makes a scalar `localparam
string S = "abc"` and a `null` class-handle parameter fold. Tables are registered
**unscoped** rather than into the referencing instance's `InstanceScope`: that
keeps per-scope signal order — and so the backend's state layout — exactly as the
Collect pass built it, which is what makes lazy materialization during Emit safe,
and it keeps the table out of the VCD hierarchy walk for free, with no `hidden`
flag and no dump-path change. `SensitivityCollector::record_read` skips
parameters outright: a parameter is not a variable (9.2.2.2, 9.4.2.2), whether or
not it was forced into storage. The `__taskarg` VCD leak noted above is a
separate pre-existing bug and is untouched.

**Result.** `clotho run` wall time on a `for` loop summing an N-entry baked
table:

| entries | materialized parameter | `const` variable (PreInit stores) |
|---|---|---|
| 4096 | 0.13 s | 0.49 s |
| 65536 | 0.26 s | 5.51 s |

A seeded table costs what the `const` variable column costs, which is why the
split is by capability rather than convenience.

Tests are `tests/const_table_test.cpp` (registered at `CMakeLists.txt:537`):
value / x-z round-trip / element sub-select / multidimensional / real / LRM 7.4.5
out-of-range / whole-array copy / `foreach` / by-value actual / constant-index
fold; per-instance overrides and a package table; a class parameter table;
unpacked-struct tables including a nested member array and 4-state members;
string / queue / dynamic-array / associative-array tables (short and heap
strings, `default:`, string and integer keys, a struct with a string member);
scalar string and `null` parameters; a VCD-exclusion check; and the two
rejections. Sanitizer run (`-DCLOTHO_SANITIZE=ON`) on the heap-backed shapes
reports nothing the equivalent `const` variable design does not.

**Still out of scope.** An associative array with a wildcard index (`int A[*]`) —
no declared key type to intern the key constant against, while the runtime keys
on the byte span. A queue / associative array whose element is itself an
aggregate or container — one entry op carries a value, not an lvalue to build
into. Whole-array copy, by-value pass and NBA of a *string* array, which fail the
same way for variables (`docs/TODO.md` "Unpacked arrays of `string`"). A whole
unpacked-array parameter read in a general rvalue context, and `P.sum()` and
friends, both unchanged by this work. Option C (one shared `.rodata` copy instead
of one per elaborated parameter) and folding the per-access packed spill (:3901)
into the same mechanism remain available follow-ups.

## Oracles

iverilog v13 cannot serve as the oracle here: it rejects the construct outright
("sorry: unpacked array parameters are not supported yet"). Use the `const`
variable equivalent under iverilog for value and out-of-range semantics.
Verilator 5.050 accepts the parameter form but wraps out-of-range indices
(`P[4]` on a `[0:3]` table prints element 0), so it is not an oracle for LRM
7.4.6.

## Risks

- Materializing during Emit appends signals after Collect built the scope. Safe
  as implemented because a table is unscoped, so no `InstanceScope` order moves;
  a future change that scopes them reintroduces the hazard.
- The pooled image must round-trip 4-state element planes exactly; a mismatch
  with the backend slot layout would silently corrupt table contents. Covered by
  the x/z case in `tests/const_table_test.cpp`, cross-checked against iverilog.
- One table per elaborated parameter symbol: N instances of a module with the
  same override each carry their own copy of the bytes. Option C is the fix if a
  design ever makes that matter.
