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
- **sort / rsort** split by whether the element is its own key. An **integral
  (width ≤ 64) or real** element takes the runtime path: the backend derives the
  comparator descriptor `{kind: unsigned|signed|real|unsigned4|signed4, width}`
  from the element value type, and `rsort` sorts ascending then reverses — except
  for the 4-state kinds, which order descending directly (§3). Both shortcuts are
  sound for a 2-state or real element only because the element IS the key there —
  equal keys mean byte-equal elements, so tie order is unobservable and reversing
  a tie group changes nothing. A `with` clause, a **string** element or an element
  **wider than 64 bits** goes through the IR permutation sort instead (see below).
- **shuffle** is Fisher-Yates driven by the calling thread's RNG (xoshiro256++,
  LRM 18.13). The runtime replicates the backend's inlined `emit_xoshiro_next`
  byte-for-byte, so a shuffle leaves the `$urandom` stream consistent. A
  shuffling process is forced to a coroutine (the state lives in its frame); a
  shuffle in a subroutine uses the caller's, threaded through the call ABI.

Oracle: iverilog implements none of these ("sorry: not currently supported").
verilator matches Clotho byte-for-byte on the DEFINED value cases; on an unknown
it fabricates a value and sorts that (§3). shuffle's permutation is
LRM-unspecified (18.13-class tool-private), so tests pin Clotho's own stream.

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
  64 bits, `real` (the F* compare tags), `string`, packed struct. No element ever
  needs a `{kind, width}` descriptor here, because there is no C comparator to
  describe one to — which is why this family never had `sort`'s routing split.
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

Receivers: fixed-size unpacked array, dynamic array and queue. LRM 7.12 also
allows an associative array, which fails loudly (see "Remaining work").
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
quiescent edge (`ir/llvm/Scheduler.cpp, emit_active_run`). A 400-element sort whose key allocates a
string per evaluation reports one collection under `--gc-stress`, none of it
inside the sort.

### 2. Fixed-size unpacked-array receiver — landed

All three families on `int q[5]`, `int q[3:7]` and the descending `int q[7:3]`.
7.12 applies to "any unpacked array" and 7.12.2 spells it out ("fixed or
dynamically sized"), so the refusal that stood at the shared container-method
dispatch was clotho's own gap. It cost Accellera UVM `uvm_printer.svh:1498` —
`int q[5]; int qq[$]; qq = q.max; m = qq[0];` — the only one of uvm-core's 41
array-method call sites not on a queue.

**The one thing a fixed receiver changes is that its indices are its DECLARED
range**, not `0 .. n-1`. That is threaded as a `DeclIndexMap`
(`{first, step, count}`) carried beside the receiver: iteration position `p` holds
the element at declared index `first + step*p`. A queue or dynamic array is
`{0, +1}`, for which every expression the map builds collapses to the position
itself — so this is ONE path with the old receivers as its degenerate case rather
than a fork, and the queue paths emit the instructions they emitted before. The
walk stays 0-based; the map is applied only where an index is REPORTED or an
element is ADDRESSED:

- `find_index` / `find_first_index` / `find_last_index` / `unique_index` push
  `decl_of(k)`.
- `item.index` (7.12.4) reads it through the `IterBinding` that the clause
  already resolved by iterator-symbol identity, so a nested clause and `unique`'s
  dedup scan each map their own index.
- `elem_select` takes it directly, because an `ir::ElemSelect` index into a fixed
  array is ALREADY a declared-range coordinate the backend normalizes against
  `at.left` in either direction. One computation feeds all three, which is what
  keeps a reported index and the element it names from disagreeing.

**Range direction is the part a 0-based test cannot catch.** 7.12.1 defines the
first element as the one "closest to the leftmost indexed element", so the walk
starts at the declared LEFT bound whichever side of the range it is on: position 0
is index 3 for `int b[3:7]` and index 7 for `int c[7:3]`, where `step` is -1.
Ordering permutes in declared-address order for the same reason, so `sort` puts
the smallest element at the left bound and `c.sort()` reads ascending from index 7
down to 3. Negative bounds (`int a[-1:1]`) are still refused, by the pre-existing
`lower_type` guard.

**Ordering reuses the dynamic-array implementations instead of growing a second
copy of any of them.** `lower_fixed_array_order` mirrors the elements into a
temporary dynamic array in declared-address order, reorders THERE, and copies
back. A fixed array has no handle, so it cannot be handed to `rt_dynarr_*`
directly; a mirror is what makes the whole existing stack apply unchanged.

- `reverse` and bare `sort` / `rsort` keep the runtime fast path, including the
  4-state comparator kinds — so §3's total order for unknowns holds on a fixed
  receiver with nothing added (`'{8'd3, 8'hxx, 8'd1, 8'd2}` sorts to
  `xx 1 2 3` and rsorts to `3 2 1 xx`).
- `shuffle` is the same `rt_dynarr_shuffle` call on a mirror of the same length,
  so it draws the SAME RNG stream: a six-element fixed shuffle produces the
  permutation the dynamic-array test already pins and leaves `$urandom` at the
  same value. An IR Fisher-Yates would have diverged here, since `urandom_range`
  maps a draw by multiply-shift where the runtime takes a modulo.
- A keyed `sort` goes through `lower_array_sort_iter` unchanged, with the
  receiver's map installed in the iterator binding so `item.index` still reports
  declared indices while `ord` holds mirror positions.

Reuse is pinned by RESULT rather than by assertion: stability and the
per-comparison clause count come out to the same `12 13 15 10 11 14` and
`calls=8` that the queue cases pin.

**One real bug the mirror exposed, fixed here.** An aggregate element crosses the
`rt_*` ABI as a pointer to a CANONICAL aggregate image, but a fixed array of
scalar-element sub-arrays stores FLAT leaf slots that may be independently
z-widened — a different image size. Handing that pointer to `queue_push` made the
runtime copy the wrong byte count, and `int m[2][3]; mq = m.max with (item.sum);`
segfaulted. Result pushes now stage such an element through a canonical temp
(`canonical_elem_ptr`, the rule `lower_container_element_value` already applies to
a pushed actual), and the mirror copy decomposes per leaf rather than taking the
whole-image `agg_copy` memcpy. A queue receiver is unaffected — its elements
already live in a canonical image — and emits what it emitted before.

**Multidimensional receivers work through the clause's own nested form.** 7.12.3's
worked example `logic [7:0] m [2][2]; m.sum with (item.sum with (item))` gives
50, because the ELEMENTS of `m` are its sub-arrays and the inner clause reduces
one of them per iteration — a bare `m.sum` is NOT the flattened leaf sum. Index
results name the outer dimension in declared coordinates (`int n[2:3][3]` answers
3), `min` / `max` return a queue OF sub-arrays, and `reverse` / `shuffle` reorder
sub-arrays because they move whole slots and need no comparison. A BARE `m.sum` or
`m.max` is refused in slang's binder with 7.12.3's and 7.12.1's own words
("unpacked arrays of integral values" / "of comparable values"), which is the
right place for a type rule; no arithmetic or relational operator is defined for a
sub-array. `item.index(2)` for the second dimension is still refused by clotho.

Refused loudly, each on the clause: a bare reduction of a non-integral element
(multidimensional, `string`, `real`) and a bare `min` / `max` / `sort` of a
non-comparable one, both by slang ahead of lowering; an aggregate element with no
`with` clause, by clotho, now citing 7.12.1 and 7.12.3 together; a
virtual-interface element, which the ordering mirror still refuses
(`guard_container_element` -- a `chandle` element now lowers, see
docs/design/dynamic-arrays.md); and `item.index(d)` for `d > 1`.

### 2a. Associative-array receiver

LRM 7.12 allows it, and the locator/index families have defined behaviour (index
results are key-typed; first/last follow `first()`/`last()`). It fails loudly
today: no dense index exists, so the walk is the `assoc_traverse(First/Next)`
chain `lower_foreach_assoc_dim` uses, and the index-family result queue is keyed
by the index type. `[*]`-wildcard keys are illegal for the index family (7.12.1).

### 3. 4-state `sort` / `rsort` ordering — landed

**Clotho's ordering of unknowns, normatively.** For `sort` / `rsort` on a 4-state
element or a 4-state `with` key:

```
unknown  <  defined,     unknown_i  ==  unknown_j   for all i, j
```

- **Unknown** means the compared value — the element, or the KEY for a keyed sort
  — carries **at least one** x or z bit. Not all bits: `8'b1111_111x` and
  `8'b0000_000x` are both unknown.
- All unknowns are **mutually equal**. x is not distinguished from z, and two
  unknowns are not ranked by the bits they do know: the two values above compare
  equal to each other.
- An unknown sorts **ahead of** every defined value in `sort`, and **behind**
  every defined one in `rsort` — the mirror, ordered directly, never
  ascending-then-reverse (see below).
- Defined values sort **numerically**, signed or unsigned per the operand type,
  exactly as 2-state ones do.
- The order is **stable**, so mutually-equal unknowns keep their input order and
  the whole result is deterministic, not merely legal.

**Why a defined order rather than a refusal.** The naive reading of the LRM is
that this is unspecified and therefore refusable: 11.4.4 says "If either operand
of a relational operator contains an unknown (x) or high-impedance (z) value,
then the result shall be 1'bx", so `<` answers x and the values look
incomparable. But *incomparable* is not what x gives a sort. A comparison sort
needs a **strict weak ordering**; x collapsed to false yields a relation that is
neither transitive nor consistent between `less(a,b)` and `less(b,a)`, which is an
**inconsistent comparator** — undefined behaviour in the algorithm, not an
unspecified result in the program. The order above IS a strict weak ordering:
irreflexive (nothing precedes itself), transitive, and with transitive
incomparability (the unknowns form one equivalence class, the defined values order
numerically). So it is safe to sort with, and reproducible.

**What refusing cost.** Accellera UVM `uvm_root.svh:827` runs
`m_time_settings.sort() with ( item.offset )`, and that property is declared
`time offset` (`uvm_cmdline_report.svh:179`). `time` is a 64-bit FOUR-STATE type,
so the refusal fired on a sort whose keys are ordinary simulation times with no
unknown in them — the guard could not tell "might be unknown" from "is unknown",
because a type is all it had. The three other keyed sorts in that file
(`sorted_list.sort() with ( item.get_full_name() )`) have string keys and always
worked.

**Where it is implemented.** Two comparators, because there are two paths:

- **The IR keyed comparator** (`sort_right_wins`, `src/lower/Sort.cpp`) — a `with`
  clause, a string element, or an element wider than 64 bits. A key is unknown iff
  its unknown plane is nonzero, which is exactly the `IsUnknown` node ($isunknown,
  LRM 20.9) and works at any width. Ascending, the right candidate wins iff
  `!unknown(left) && (unknown(right) || right < left)`; descending is the mirror,
  `!unknown(right) && (unknown(left) || right > left)`. The `<` / `>` in there is
  11.4.4's own compare, x and all: it is only ever the deciding operand when both
  keys are defined, and the bitwise combination is monotone (`0 & x == 0`,
  `1 | x == 1`), so no x reaches the result. **A 2-state key emits exactly the
  compare it emitted before** — the domain is decided at lower time
  (`SortIter::key_four`), not by a runtime test.
- **The runtime scalar comparator** (`slot_cmp4` / `sort_run4`,
  `src/runtime/Runtime.cpp`) — the bare form on a 4-state integral of width ≤ 64,
  which is where `time` lands. The 4-state slot is `{ i(2W) must|may, iW z }` with
  must in bits [0,W) and may in [W,2W), so `must ^ may` is the unknown mask and
  `must` is the value — the same decode `rt_container_eq` already does, and the z
  plane is never read (Z is encoded as X in must/may, and the ordering does not
  distinguish them anyway). **The 2-state fast path pays nothing**: the 4-state
  kinds are separate comparator kinds (`sort_unsigned4` / `sort_signed4`) driven by
  a separate function, `slot_less` and `sort_run` are untouched, and the choice is
  one test in `rt_dynarr_sort` / `rt_queue_sort` beside the `len < 2` test that was
  already there.

`sort_run4` is also **stable** and orders descending **directly**, and both are
load-bearing rather than tidiness: `sort_run`'s two shortcuts (an unstable sort,
and `rsort` as ascending-then-reverse) rest on equal keys meaning byte-equal
elements, which unknowns break — `8'b1111_111x` ties with `8'b0000_000x` and is
not byte-equal to it. All unknowns being one tie group is exactly what an
ascending-then-reverse `rsort` would flip. Stability comes from tie-breaking on
the original index, which the index vector `sort_run` already allocates makes free.

**Reals and strings are unaffected, with no check added.** Neither can carry x or
z — a real is an IEEE-754 double and a string is a handle — so `key_four` is false
for both by construction and they take the paths they always did.

**Signed 4-state is resolved too**, not left open: unknown-ness is decided
entirely by the unknown plane, so signedness only ever affects the ordering of
*defined* values, where it is 11.4.4's ordinary arithmetic compare.

**Oracles.** Verilator 5.050 accepts these sorts but **fabricates a value**: it
maps each unknown bit to 0 and sorts the result as that value. Measured —
`'{8'd3, 8'hxx, 8'd1}` sorts to `'{'h0, 'h1, 'h3}`, and `'{8'b0000_0000,
8'b1111_111x, 8'b0000_000x}` sorts to `'{'b0000_0000, 'b0000_0000, 'b1111_1110}`,
i.e. a partial unknown is ordered by its defined bits and lands next to the value
it is not. Clotho gives an unknown an explicit position instead of inventing one
for it. iverilog 13 does not implement `sort()` at all: "sorry: 'sort()' array
sorting method is not currently supported".

### 4. Wide (> 64-bit) `sort` / `rsort` — landed

Not by teaching the runtime comparator a multi-word compare, but by routing a wide
element through the IR permutation sort above (7.12.2's bare form is
`with (item)`, so it needs nothing else). `slot_less` / `slot_cmp4` still load one
word each and are still only reached by elements that fit them. Signed and
unsigned both work — the key's own type carries the signedness, so the emitted
`Lt` / `Gt` picks the arithmetic, which is the same reason the locator family
never needed a descriptor. A wide **4-state** element needed nothing further than
§3's ordering: the unknown test is one reduction of the whole unknown plane, at
any width.

## Priority

The associative receiver (2a) is the remaining scope. Ordering has no remaining
element, key or receiver restriction bar that one; the clause's
`struct {...} c [512]` example now runs on the fixed receiver it was written for,
rather than being pinned on a queue.
