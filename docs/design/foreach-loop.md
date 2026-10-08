# Design: `foreach` loop

Status: **landed** (fixed-size unpacked/packed array dimensions + `string`).
This is the as-built design; the "Lowering plan" section below predates the
code and reads as proposal, but matches what shipped. Reconciliation:

- **Done**: nested `Loop` lowering for any number of fixed-range dimensions
  (ascending or descending declared ranges), skipped dimensions (correctly
  contribute zero nesting, not a size-1 loop), `break`/`continue` (fall out of
  the existing per-dim `Loop` targets, innermost only, matching 12.7's
  semantics), and `string` foreach (runtime length as the bound, snapshotted
  once on entry like a repeat-loop count). iverilog-verified for every case
  except `string` (iverilog's own string support is too limited to execute the
  oracle case at all -- internal `$low()`/`$high()` errors -- consistent with
  iverilog's other string gaps; the LRM's own text is unambiguous here, so this
  needed no deviation record).
- **Not done** (loud-fail, `Lower.cpp` `lower_foreach_dim`): dynamic arrays,
  queues, associative arrays -- moot today since declaring one of these
  anywhere already fails at the array's own type-lowering (`Lower.cpp:665`),
  before a `foreach` referencing it is ever reached. The guard exists for
  defense-in-depth (and to document intent) if partial support for those
  array kinds lands first.
- **Turned out simpler than scoped**: no `ElemSelect` synthesis is needed in
  `lower_foreach` at all. The loop only has to bind each dimension's
  `IteratorSymbol` to the right value each iteration; the body's own indexing
  expressions (`arr[i][j]`) already lower through the pre-existing, generic
  ElemSelect/BitSelect paths, unmodified by this feature. This also means
  packed multi-dimensional foreach (e.g. `bit[3:0][2:1] x; foreach(x[i,j])`)
  works today wherever the underlying packed indexing already does, with no
  foreach-specific code for it -- slang hands over a per-dimension `range` for
  packed dims exactly the same way it does for unpacked ones.

## LRM semantics (§12.7.3, p.331-332)

- `foreach (arr[i, j, ...])` iterates every dimension of `arr`: fixed/packed
  unpacked arrays, dynamic arrays, queues, associative arrays (wildcard-index
  assoc arrays are explicitly barred), and `string` (byte-indexed dynamic
  array).
- Dimension order: leftmost loop var = outermost (lowest-cardinality)
  dimension, rightmost = innermost, matching declaration order. Vars may be
  skipped (`foreach(B[q,,s])`) to not iterate that dimension; trailing commas
  may be omitted.
- Loop vars are **implicitly declared**, type = array's index type (`int` for
  fixed/dynamic/queue, key type for associative), and are **read-only** in an
  implicit `begin...end` scope — body cannot assign to them.
- Bounds are runtime, not required constant: "similar to a repeat-loop that
  uses the array bounds." Works with dynamic/queue/assoc sizes computed at
  execution.
- Resizing the array mid-iteration is explicitly undefined behavior (LRM
  doesn't require detection).
- `continue` inside a multi-dim foreach applies to the innermost dimension
  only, not the whole nest.

## Current Clotho array support (blocks most of the above)

Only one array kind is modeled at all: fixed-size unpacked arrays
(`ir::Type::Kind::UnpackedArray`, declared `left`/`right` range,
`Lower.cpp:654-660`). Every other unpacked-array kind loud-fails today at
type-lowering (`Lower.cpp:662-665`: "dynamic / queue / associative arrays are
not supported"). No runtime representation exists for dynamic arrays, queues,
or associative arrays (`src/runtime`). Packed multidim arrays are also an open
TODO item (`docs/TODO.md` M3 "Data types").

Consequence: a correct, non-stubbed `foreach` can only be built, in the near
term, over **fixed-size unpacked arrays** (single or nested dims) and `string`
(SSO layout already exists) -- everything else must loud-fail per PRINCIPLES.md,
not silently degrade.

## slang AST shape

`ForeachLoopStatement` (`ast/statements/LoopStatements.h:160-218`):

```cpp
struct LoopDim {
  std::optional<ConstantRange> range;      // nullopt => dynamically sized dim
  const IteratorSymbol* loopVar;           // nullptr => skipped dimension
};
const Expression& arrayRef;
std::span<const LoopDim> loopDims;
const Statement& body;
```

`IteratorSymbol` is a real `VariableSymbol` (carries `arrayType`,
`indexMethodName`), so it should thread through the existing
`declare_local`/lvalue path (`Lower.cpp:939`) like `ForLoopStatement.loopVars`
does today (`Lower.cpp:1701-1703`) — not the synthetic-temp path `repeat` uses.

## Lowering plan (fixed-size unpacked arrays only, first cut)

- Add `case StatementKind::ForeachLoop:` to `lower_stmt`'s dispatch
  (`Lower.cpp:1545-1559`, current `default: fail("unsupported statement")` at
  1575) calling new `lower_foreach`.
- Guard first: if `arrayRef`'s type is anything but `UnpackedArray` with a
  known constant range (i.e. `LoopDim.range` has a value for every non-skipped
  dim, and the element type chain bottoms out in a supported type), `fail(...)`
  with a `// TODO:` naming the missing array kind (dynamic/queue/assoc/packed
  multidim), per PRINCIPLES.md's loud-fail-stub rule. This is the bulk of the
  correctness surface — must not let an assoc/dynamic array silently iterate
  zero times or crash.
- For each non-skipped dim, synthesize a hidden index counter (repeat-loop
  style `make_temp_cell` — a per-activation frame Local inside a subroutine,
  a module signal in a process) ranging over `[left, right]` per
  `ConstantRange` (respecting descending ranges, already handled for plain
  array decls); bind the real `IteratorSymbol` via `declare_local` to the
  current counter value each iteration (cast to `int`, per LRM's implicit
  `int` loop-var type) — read-only is enforced by simply never emitting a
  store target for it (frontend never generates an lvalue use since slang
  itself rejects assignment to a foreach loop var during elaboration).
  Skipped dims get no counter and no binding, only affect nesting depth.
- Nest dims outermost-to-innermost per source order (leftmost var =
  outermost), reusing the generic `b.loop(cond, pred, body, step, ...)`
  primitive per dim — i.e. an N-dim foreach lowers to N nested `Loop` IR
  regions, innermost containing the user's body.
- Element access inside the body reuses `ir::ElemSelect`
  (`Ir.h:148-158`, used at `Lower.cpp:3858`/`4305`) keyed on the live counters.
- `continue` semantics: verify it already targets the correct (innermost)
  loop region under the existing nested-`Loop` lowering — should fall out for
  free since each dim is its own IR loop, but confirm against LRM's "innermost
  only" text and an iverilog oracle check before landing.

## Explicitly out of scope / follow-ups

- Dynamic arrays, queues, associative arrays, packed multidim arrays: blocked
  on their own IR/runtime representations landing first (separate TODO items).
  `foreach` over these should stay a loud stub until then.
- `foreach` inside constraint blocks (`docs/TODO.md`) — randomization
  (M4), unrelated lowering path.
- Zero-size dim (empty array) — LRM doesn't spell out the zero-iteration case
  explicitly in 12.7.3 but it falls out naturally from the repeat-loop-style
  bound (`left..right` empty range => 0 iterations); worth an iverilog oracle
  check anyway per PRINCIPLES.md before trusting the reading.

## Verification plan

- iverilog oracle cases: single-dim, multi-dim (2-3 levels), skipped
  dimension, descending range (`[7:0]` vs `[0:7]`), `continue`/`break` inside
  nested foreach, string foreach.
- Loud-fail tests: dynamic array / queue / associative array / wildcard-assoc
  `foreach` all reject at compile with a clear diagnostic, not silently.

## Nested runtime dimensions (amended)

The claim above that no `ElemSelect` synthesis is needed holds only while every
runtime-sized dimension belongs to the array the `foreach` names. It does not for a
NESTED runtime dimension: `foreach (a[k, i])` over an associative array of queues
measures and iterates `a[k]`, an entry slang gives no AST node for.

`lower_foreach_dim` therefore rebuilds each dimension's container itself
(`foreach_base_lv`): the array the `foreach` names, plus one select per
already-iterated dimension keyed by that dimension's live loop variable, re-emitted
per region because a loop's cond / body / step are three regions. Dimensions are
peeled through the interned IR type rather than slang's predicates, since slang
answers `isDynamicallySizedArray()` for an associative array too — peeling by
predicate measured a map with `dyn_array_size`.

Before this, every runtime dimension measured `f.arrayRef`, so dimension 2 of an
assoc-of-queue re-walked the outer map with the inner loop variable bound to a key:
wrong trip count, wrong index, no diagnostic.

Still refused: a skipped outer dimension over a nested runtime dimension
(`foreach (a[, i])`), which has no entry to descend into.
