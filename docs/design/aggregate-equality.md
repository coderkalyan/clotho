# Aggregate equality

**Status:** normative. Aggregate equality stays separate from scalar equality but is
implemented completely and recursively, with no refusal that exists only because a
combination was not wired.

`==`, `!=`, `===` and `!==` on an unpacked struct, fixed unpacked array, queue, dynamic
array or associative array are one IR node, `AggEq`, over two lvalues of one type. `!=`
and `!==` are a `Lnot` of it. The backend (`lower_agg_eq`) is the single comparator.

## One function over the type

`eq(mode, type, a, b)` has exactly one rule per type kind and recurses for every
aggregate and container kind, at any nesting. `mode` is `Logical` (`==`) or `Case` (`===`).

| type | rule |
| --- | --- |
| integral, packed struct / union, enum | Logical: LRM 11.4.5, a bit position known in both operands that differs gives 0; otherwise any x/z gives x; otherwise 1. Case: all of the value, x and z planes equal. |
| real, shortreal | IEEE-754 `==`; no x/z. `===` on a real operand is illegal (Table 11-1); inside an aggregate it compares like `==`. |
| string | content equality; no x/z. |
| class handle, chandle, event, virtual interface | identity; no x/z. |
| unpacked struct | AND-reduction of `eq` over the members. |
| fixed unpacked array | AND-reduction of `eq` over the elements. |
| queue, dynamic array | different sizes: 0, never x. Equal sizes: AND-reduction of `eq` over the elements. |
| associative array | see below. |

AND-reduction: any definite 0 gives 0, else any x gives x, else 1 (this is `Land`).
A member with a definite mismatch therefore wins over an unknown one.

## Result domain

The result is 4-state only for `Logical` over a type with a 4-state leaf
(`types::has_four_state_leaf`), the only case where x can arise. A `Case` result is
always a known 2-state bit, like scalar `===`.

## Operands

Any expression of aggregate type is an operand. One helper materializes an operand that
is not already an lvalue (an assignment pattern, a function result, a `?:`, a slice)
into a temp; every equality, and the ambiguous `?:` of LRM 11.4.11, goes through it.

## Associative arrays

Equal iff the same set of (key, value) pairs: sizes, then a lock-step walk in
sorted-key order, keys compared exactly, values by the recursive `eq` in the requested
mode. A different key set is a definite 0, like a queue size mismatch. The rule is the
same for a whole array, a member, an element and any nesting.

Open, pending the maintainer: which LRM clause defines associative-array equality (the
code cites "11.4.5 / 7.9"), and whether a key-set mismatch propagates x in logical
mode. Until then logical `==` over an associative array whose value has a 4-state leaf
is refused, at one site (`guard_agg_eq_members`); `===` and every other combination
work. The decision only changes the logical-mode x rule of the walk.
