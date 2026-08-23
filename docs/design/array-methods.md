# Array manipulation methods (LRM 7.12)

Status of the built-in array methods on dynamic arrays and queues, and the
scoped-out plan for the remainder. All three families are landed: ordering
(7.12.2) through runtime helpers, locator (7.12.1) and reduction (7.12.3)
through a frontend iterator loop that also implements the shared `with` clause.

## The three families (LRM 7.12)

| Clause | Family | Methods | Returns | `with` clause |
|---|---|---|---|---|
| 7.12.1 | Locator | `find`, `find_index`, `find_first`, `find_first_index`, `find_last`, `find_last_index`, `min`, `max`, `unique`, `unique_index` | queue of elements / int indices | `find*` require it; `min`/`max`/`unique*` optional |
| 7.12.2 | Ordering | `reverse`, `sort`, `rsort`, `shuffle` | void (mutate in place) | `sort`/`rsort` optional; `reverse`/`shuffle` none |
| 7.12.3 | Reduction | `sum`, `product`, `and`, `or`, `xor` | scalar | optional |

All arrive from slang as a `CallExpression` with the receiver at
`arguments()[0]` and (for methods that take one) the `with` iterator in the
`SystemCallInfo` — `std::get<CallExpression::SystemCallInfo>(call.subroutine)
.getIteratorInfo()` returns `(iterExpr, iterVar)`.

## Landed: ordering methods (7.12.2)

`reverse` / `sort` / `rsort` / `shuffle` on dynamic arrays and queues. Runtime
helpers `rt_dynarr_{reverse,sort,shuffle}` and `rt_queue_{reverse,sort,shuffle}`
over the raw `stride`-byte element buffer (the queue variants operate on the
ring's logical order).

- **reverse / shuffle** are element-agnostic (whole-slot move), so they support
  any element type.
- **sort / rsort** split by whether the element is its own key. A **2-state
  integral (width ≤ 64) or real** element takes the runtime path: the backend
  derives the comparator descriptor `{kind: unsigned|signed|real, width}` from the
  element value type, and `rsort` sorts ascending then reverses. Both shortcuts
  are sound only because the element IS the key there — equal keys mean
  byte-equal elements, so tie order is unobservable and reversing a tie group
  changes nothing. A `with` clause, a **string** element or an element **wider
  than 64 bits** goes through the IR permutation sort instead (see below).
- **shuffle** is Fisher-Yates driven by the calling thread's RNG (xoshiro256++,
  LRM 18.13). The runtime replicates the backend's inlined `emit_xoshiro_next`
  byte-for-byte, so a shuffle leaves the `$urandom` stream consistent. A
  shuffling process is forced to a coroutine (the state lives in its frame); a
  shuffle in a subroutine uses the caller's, threaded through the call ABI.

Oracle: iverilog implements none of these ("sorry: not currently supported").
verilator matches Clotho byte-for-byte on the value cases. shuffle's permutation
is LRM-unspecified (18.13-class tool-private), so tests pin Clotho's own stream.

## Landed: the `with` iterator clause and the locator / reduction families

A `with` clause is an inline expression evaluated once per element with an
implicit iterator bound:

```systemverilog
q.find(x) with (x.age > 21)     // `x` is the element; `x.index` is its position
a.sum with (item)               // default iterator name `item`; `item.index`
```

slang resolves the iterator to a `ValueSymbol` (an `IteratorSymbol`) and the body
to an `Expression`; both arrive from
`std::get<CallExpression::SystemCallInfo>(call.subroutine).getIteratorInfo()`.
`item.index` arrives as a builtin method call named `index` whose receiver is
that symbol — slang has already resolved a user-chosen index-method name to it.

**No new IR node was needed, and no runtime helper.** `lower_array_iter_method`
emits one ordinary loop (`b.loop` over three regions, the shape
`lower_foreach_runtime_dim` already used) whose body stores element `k` into the
iterator variable and lowers the clause against it. The iterator variable binds
through the same generic `declare_storage` / `value_lvalue` pair foreach uses for
its loop variable, so it is a frame Local inside a subroutine and a module Signal
in a process, and every hidden cell (index, snapshotted bound, accumulator,
min/max best-and-have, unique's key and dup flags) is a `TempCell`.

Consequences of going through the ordinary expression path rather than a runtime
comparator, which is the reason for choosing it:

- **Any element type works**: 4-state (x propagates per LRM 11.4.x), wider than
  64 bits, `real` (the F* compare tags), `string`, packed struct. None of
  `sort`'s element restrictions apply, because there is no C comparator that
  needs a `{kind, width}` descriptor.
- **The clause can be anything Clotho lowers**, including a member access, a cast
  or a nested method call.
- `item.index` resolves through a binding stack keyed on the iterator symbol, so
  a nested clause and `unique`'s inner dedup scan each read their own index.

Per-family shape:

- **find / find_index** push every match; **find_first\*** push and `break`;
  **find_last\*** clear the result queue before pushing, so the last match
  survives. The predicate goes through `lower_predicate`, so an x result is false.
- **min / max** keep a `best` key plus a `have` flag and hold the winning element
  in the result queue: a tie keeps the earlier element, and an unknown-bearing
  compare is not a definite 1, so an unknown never wins.
- **unique / unique_index** scan the earlier elements for an equal key, so the
  first of each duplicate group survives. Integral keys compare with `===`
  (bit-exact, matching slang's own constant-evaluation dedup); real and string
  keys have no x to distinguish. The scan re-evaluates the clause instead of
  caching keys in a second heap queue — 7.12 already makes a clause with side
  effects unpredictable.
- **Reductions** fold into an accumulator at `*call.type`'s width, so a `byte`
  sum wraps and a `with (int'(item))` clause widens it, exactly as 7.12.3's
  examples require. An **empty** array answers 0 for all five rather than the
  fold identity: 7.12.3 does not say what an empty receiver yields, and one
  uniform answer beats splitting `product` and `and` off with identities of
  their own.

Result queues are built the `materialize_queue_rvalue` way: an `make_arg_temp`
handle slot, cleared with `queue_delete`, then `queue_push` per hit. A
locator call therefore reaches consumers through `lower_lvalue`'s Call case, which
routes container methods to this path (a builtin method is a system call, which
`lower_user_call` refuses).

Receivers: dynamic array and queue. LRM 7.12 also allows a fixed-size unpacked
array and an associative array; both fail loudly (see "Remaining work").

iverilog implements none of LRM 7.12; verilator 5.050 agrees with Clotho apart
from a struct-member-select bug of its own and the empty-array reduction. Three
points have no single answer to match in any case: the empty-array reduction and
unknown placement in min/max are open in the LRM, and 7.12.1 explicitly leaves
the order of `unique` results unrelated to the array's order. Clotho's answers
are the ones stated above — 0 from an empty reduction, an unknown never wins a
min/max, `unique` keeps the first of each duplicate group in array order.

## Remaining work

### 1. `with` clause on sort / rsort — landed

`a.sort with (item.field)` sorts by the mapped key. Lowered by
`lower_array_sort_iter` as a **stable bottom-up merge sort over an index
permutation**, with one gather applying the permutation at the end — the
O(n log n) moves are on 32-bit indices and exactly n elements are copied. The
same entry point carries a bare `sort` of a **string** or **wide** element, since
7.12.2's bare form is `with (item)`: a string-element sort needs no new
`slot_less` case, and no multi-word runtime compare was written.

Why IR rather than a runtime comparator taking a generated `less` thunk: the key
expression can read enclosing frame locals, `this` and the call-ABI RNG pointer,
and clotho has no closure mechanism to hand a standalone thunk any of them
(`rt_elem_ops`, the nearest precedent, carries only layout data — no function
pointers). Going through the ordinary expression path is also what makes every
key type work with no per-type descriptor, exactly as for the locators.

Three semantics this fixes, none of which the clause states:

- **The key is evaluated per comparison**, not precomputed into a buffer. A
  side-effecting clause therefore observes two calls per comparison; a test pins
  the count. Precomputing was the alternative — fewer evaluations, but O(n) key
  storage and a divergence from `unique`'s re-evaluating dedup scan.
- **Ordering is stable**, because a clause makes ties observable: two different
  elements can share a key. `rsort` orders descending directly rather than
  ascending-then-reverse, which would reverse each tie GROUP.
- **A clause that mutates the receiver is bounded**: the element count is
  snapshotted at entry (the `foreach` rule), so the sort orders the elements that
  were there and pushed ones stay past them. Nothing dangles — no raw pointer is
  held across the clause, since every access re-resolves through `elem_select`.

On the GC: a container payload is `malloc`'d rather than GC-heap (`Gc.h:333`), so
no collector relocates it; allocation only sets `mutator->request`
(`Gc.cpp:1855`) and the sole poll site in emitted code is the drain loop's
quiescent edge (`LlvmBackend.cpp:11850`). A 400-element sort whose key allocates a
string per evaluation reports one collection under `--gc-stress`, none of it
inside the sort.

### 2. Fixed-size unpacked-array and associative-array receivers

LRM 7.12 applies to any unpacked array, and the locator/index families have
defined associative-array behaviour (index results are key-typed; first/last
follow `first()`/`last()`). Both fail loudly today.

- **Fixed-size**: the loop shape carries over, but the declared range does not.
  `_index` results and `item.index` must yield declared indices, not 0-based
  offsets, so the loop needs the dimension's range and a bias.
- **Associative**: no dense index exists; the walk is the
  `assoc_traverse(First/Next)` chain `lower_foreach_assoc_dim` uses, and the
  index-family result queue is keyed by the index type. `[*]`-wildcard keys are
  illegal for the index family (7.12.1).

### 3. 4-state `sort` / `rsort` ordering — deferred, LRM-unspecified

Guarded loud (4-state element OR 4-state `with` key → fail), and the message says
why rather than naming a width limit. Deferred deliberately: **LRM 7.12.2 leaves
x/z placement in `sort` unspecified.** A 4-state `<` yields x rather than a total
order, so any order Clotho picked would become a de-facto commitment that later
code depends on — a loud refusal is recoverable, a silently wrong order is not.

Note this is now the ONLY element restriction on ordering, and it is a semantic
one, not a comparator one: the IR path could compare 4-state values today. What is
missing is a defined answer, not a mechanism.

If revisited, an **unsigned** total order is implementable:
- 4-state slot = `{ i(2W) must|may, iW z }`, invariant must ⇒ may, Z encoded as
  must=0 / may=1 / z=1 (see the LlvmBackend header comment).
- `has_unknown = (may & ~must) != 0` (any X or Z bit).
- Comparator: `has_unknown` primary (unknown sorts first), then a per-bit rank
  key with rank `z=0, x=1, 0=2, 1=3` compared MSB-first.

Signed 4-state ordering stays unresolved pending an LRM clarification.

### 4. Wide (> 64-bit) `sort` / `rsort` — landed

Not by teaching the runtime comparator a multi-word compare, but by routing a wide
element through the IR permutation sort above (7.12.2's bare form is
`with (item)`, so it needs nothing else). `slot_less` still loads one word and is
still only reached by elements that fit it. Signed and unsigned both work — the
key's own type carries the signedness, so the emitted `Lt` / `Gt` picks the
arithmetic, which is the same reason the locator family never needed a descriptor.

## Priority

The receiver gaps (2) are the remaining scope, fixed-size first since its loop is
the one already written — and it is what the clause's `struct {...} c [512]`
example actually needs, so the two struct-key examples are pinned on a queue
receiver instead. 4-state sort (3) can stay guarded indefinitely without silent
miscompiles.
