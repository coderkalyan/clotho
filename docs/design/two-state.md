# Design: native 2-state (`bit`) lowering

Status: **landed** (explicit `bit` / sized-int path). This is the as-built
design. Reconciliation (the sections below predate the code and read as
proposal — they are accurate as the design that shipped):

- **Done**: native single-`iW` value (its domain is its IR type's), narrow `{iW}` storage for
  signals / scalar NBA targets / frame locals / plain memories / array-NBA /
  `$readmem` / file-io ABI, native operators (add/sub/mul, bitwise, shifts,
  div/mod, mux, reductions, selects, conversions), LRM 6.3.1 x/z→0 conversion,
  sized-int signedness, and the call ABI (a 2-state formal or result is one `iW`,
  an out / inout formal a pointer to an `iW` slot). iverilog-verified.
  `bit[7:0] a+b` → one `add i8`.
- **Residual (perf follow-up, not correctness)**: `**` uses the 4-state inline
  loop; comparisons/reductions/logicals emit a 4-state 1-bit result that
  instcombine folds to native.
- **Extending the win to 4-state `logic` RTL** (which this path does not touch)
  is never-X inference — scoped in `never-x-inference.md`.

## Problem

`bit` is nominally 2-state (`InternPool` `Domain::Two`) but the backend lowers it
identically to `logic` minus the z plane: `slot_type` emits `{ i(2W) }` (the
packed must|may word, `ir/llvm/StateLayout.cpp, slot_type`) and every operator runs the branchy
4-state path (`lower_arith` etc.). Two costs:

1. **Storage**: a `bit [W]` slot is `i(2W)`, double the needed width. The high W
   bits (`may`) are always equal to the low W bits (`must`) — pure redundancy.
2. **Ops**: `add` of two `bit` values still emits the X-detect branch +
   duplicate-into-both-planes dance, when it could be one native `add iW`.

Correctness gap: assigning `x`/`z` to a `bit` today leaves the `may` bit set (the
store just drops the z word), so a `bit` can carry X in its `may` plane —
violating LRM 6.3.1 (x/z → 0 on assignment to a 2-state target).

## Representation

A 2-state value is a single `iW` LLVM value. One `Plane` carries both encodings,
and which one it holds comes from the value's `ValueType.four_state` (derived
per-instruction from its operands, `ir/Typing.h, ValueType`), never from the plane:

| domain | `Plane.mm` | `Plane.z` |
|--------|-----------|-----------|
| 4-state | `i(2W)`: `[0,W)` must, `[W,2W)` may | `iW` or null (z-elision) |
| 2-state | `iW`: the value | always null |

The plane records nothing about its own encoding, so there is no second piece of
state to keep in agreement with the type. The accessors
(`must_of`/`may_of`/`z_of`/`width_of`/`any_unknown`/`to_four`) and the operators
take the domain as an argument, read off the operand's IR type at the call site
(`type_of(i).four_state`, or the storage's `four_state` for a load); the `Index`
overloads (`must_of(i)`, `any_unknown(i)`) read it themselves. `xz_mask`, `packed`
and the single-bit truth tables assume `mm` is `i(2W)` and are only reached with
4-state planes. Dispatch happens in `lower_inst`: each operator is called with the
domains of its operands and routes to either the 4-state lowering or the native
2-state one, so the two representations do not leak into each other.

### Same-domain operand invariant

slang unifies the domain of context-determined operands: in `a + b`, if either is
4-state it inserts a `ConversionExpression` making both 4-state, and the result is
4-state (LRM 11.8.1 / §11.6). So a binary op's two operands always share a domain
by the time Clotho lowers it. Self-determined operands (shift amount) only
contribute a value; convert to the value bits and ignore. `ir::verify` asserts the
invariant (`operand-domain`), and the operators take it as one `four` argument.

## Storage / slot

`slot_type(width, four_state=false, has_z=false)` field becomes `int_ty(width)`
(was `int_ty(2*width)`):

```llvm
%bit.8  = type { i8 }    ; was { i16 }
%logic.8 = type { i16, i8 }
%logic.nz.8 = type { i16 }
```

Load / store / default-init / array-init / NBA-shadow for a 2-state slot read and
write the raw `iW`. Defaults already special-case 2-state to zero — the constant
just narrows from `zero(2*width)` to `zero(width)`.

## Conversions (`ZExt`, `SExt`, `Trunc`, `ToFour`, `ToTwo`)

An IR integer type is a width and a domain; it has no sign. A conversion is one
instruction of the five below, each carrying the target type (width + domain), and
the frontend picks it. `Builder::convert(v, to, sign_extend)` emits the sequence for
an integral value (domain down, then width, then domain up) and nothing for a value
already of that width and domain. Whether a widening fills with the top bit or with
zeros is the language's call, never the value's: LRM 11.8.3 takes the *source's*
signedness for an assignment or a cast, LRM 11.8.2 the *propagated type's* for an
operand, so `coerce` passes whichever applies and a same-width sign change emits
nothing.

### `ToTwo`: 4-state → 2-state (LRM 6.3.1: x/z → 0)

The 2-state value is exactly the `must` plane: `must` is 0 for X (0,1) and for Z
(encoded as X), 1 for a defined 1, 0 for a defined 0. One `trunc`:

```llvm
; logic[8] %mm (i16)  ->  bit[8] %v (i8)
%v = trunc i16 %mm to i8        ; low 8 = must plane; x/z already 0 there
```

### `ToFour`: 2-state → 4-state

must = may = value, z = 0. The value is duplicated:

```llvm
; bit[8] %v (i8)  ->  logic[8]  (i16 mm, i8 z)
%lo = zext i8 %v to i16
%hi = shl i16 %lo, 8
%mm = or i16 %lo, %hi           ; must|may
; z  = 0 : i8
```

### `ZExt`, `SExt`, `Trunc`: width, one domain

2-state: plain `zext` / `sext` / `trunc` of the `iW` (native, replaces the packed
`cast_plane`). 4-state: the same on each plane, `SExt` replicating the top bit of
the must, may and z planes alike.

## Operators (both operands 2-state, W=8 shown)

All native; no X branch, no LUT, no duplicate-into-planes.

```llvm
; Add / Sub / Mul
%r = add i8 %a, %b
%r = sub i8 %a, %b
%r = mul i8 %a, %b

; Bitwise AND / OR / XOR / XNOR / NOT
%r = and i8 %a, %b
%r = or  i8 %a, %b
%r = xor i8 %a, %b
%r = xor i8 (xor i8 %a, %b), -1      ; xnor
%r = xor i8 %a, -1                    ; ~a

; Shifts (amount %s). >= width already handled: LLVM shift by >= width is poison,
; so mask/guard as the 4-state path does, but on native iW.
%r = shl  i8 %a, %s
%r = lshr i8 %a, %s                   ; unsigned >>
%r = ashr i8 %a, %s                   ; signed  >>>

; Comparisons -> i1, never X for 2-state
%r = icmp eq  i8 %a, %b
%r = icmp ult i8 %a, %b               ; unsigned <   (slt/ult by sign)

; Reductions -> i1
%r = icmp ne  i8 %a, 0                ; |a  (RedOr)
%r = icmp eq  i8 %a, -1               ; &a  (RedAnd)
%p = call i8 @llvm.ctpop.i8(i8 %a)    ; ^a  (RedXor = parity)
%r = trunc i8 %p to i1

; case-equality === / !==  : identical to ==/!= for 2-state (no x/z to match)
; wildcard ==? / !=? with a 2-state pattern: also == (no x/z wildcard bits)
```

### Div / Mod

LRM 11.4.3: divide/modulo by zero → X; in 2-state X collapses to 0. Signed
`INT_MIN / -1` overflow is LLVM UB, guard as the 4-state path does. Both fold to a
guarded native op:

```llvm
%dz  = icmp eq i8 %b, 0
%bsf = select i1 %dz, i8 1, i8 %b     ; avoid UB divisor
%q   = udiv i8 %a, %bsf                ; sdiv for signed (+ INT_MIN/-1 guard)
%r   = select i1 %dz, i8 0, i8 %q     ; zero divisor -> 0
```

Div-by-zero-in-2-state = 0 to be confirmed against iverilog before commit.

### Pow (`**`, unsigned)

Exponentiation-by-squaring on native `iW`, mirroring the existing 4-state inline
loop but without plane bookkeeping.

### Mux (`?:`), logical `&& || !`

Condition reduced to i1 (`icmp ne v,0`). `select i1 %c, i8 %a, i8 %b`. Logicals:
reduce each operand to i1, native `and`/`or`/`xor`.

## Truthiness (predicate)

`lower_predicate`'s `RedOr` on a 2-state value:

```llvm
%t = icmp ne i8 %v, 0                 ; i1, no X path
```

## Edge sensitivity

`service_wait_queue` compares old vs new `mm`. For a 2-state signal `mm` is `iW`;
LSB trunc + `~old&new` (pos), `old&~new` (neg), `old!=new` (any) work natively. No
X in the plane means no spurious ambiguous edges — strictly simpler than 4-state.

## Sized aliases

`byte`/`shortint`/`int`/`longint` (2-state) and `integer`/`time` (4-state) are
canonicalized by slang; `lower_type` → `make_int_type(getBitWidth, isFourState,
isSigned)` already picks `bit_type`/`logic_type` and signedness correctly. No IR
change — verify with tests (widths 8/16/32/64, signedness, `integer` stays
4-state, `int` is 2-state).

## Loud-fail guards

Any 2-state construct not yet covered must loud-fail per PRINCIPLES.md. Audit sites:
mixed-domain operands if the invariant is ever violated (assert→fail), 2-state in
constructs still 4-state-only. Enumerate during implementation.

## Test plan

- Operator equivalence: `bit` vs `logic` results agree on all-defined inputs
  across widths (1/8/32/64), signed and unsigned.
- Conversion: `x`/`z` assigned to `bit` → 0 (LRM 6.3.1); `bit` → `logic` round
  trips; width-changing conversions.
- Truthiness, `posedge`/`negedge` on a `bit` clock.
- Aliases: `int`/`byte`/`shortint`/`longint`/`integer` widths + signedness +
  domain, oracle iverilog.
- Storage: IR check that a `bit [W]` slot is `{ iW }`.
