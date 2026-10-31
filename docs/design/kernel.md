# Kernel decisions (tier 1)

The maintainer's decisions for the tier-1 kernel (`docs/PRINCIPLES.md`, review
tiers): how values are represented and operated on, how state is laid out, and
how selects and stores work. They are normative. Code that disagrees is a bug
unless the decision here is changed first. Where the LRM fixes the observable
behavior it decides, and this list only covers what is left to us.

**Not yet** marks a decision the code does not follow today; delete the marker
when it does.

## Values

- A 4-state bit is two planes, definitely-1 and possibly-1 (x = 0/1), plus a z
  plane. AND, OR and NOT need no masking.
- In SSA the two planes are one 2W-bit integer; the z plane is a separate W-bit
  value.
- A 2-state value is a native W-bit integer, and its domain comes from its IR
  type alone: one source of truth, no per-value flag to keep in agreement. A
  2-state variable's storage, and the call ABI, use the native form too, so no
  boundary ever holds a 2-state value in the packed form. The code generator
  keeps no record of a value's encoding: whoever reads a plane passes the domain
  of the type it came from.
- An IR integer type is a width and a domain; it has no sign. Domains, signedness
  and extension are decided by the language, through slang's legalized AST, and the
  IR carries them explicitly: signed and unsigned opcodes (`SDiv`/`UDiv`,
  `SLt`/`ULt`, `Sra`/`Srl`, `SItoF`/`UItoF`), explicit flags on the instructions
  that read a sign (`Pow`, inside ranges, format arguments), and the extension in
  the opcode of a conversion (`ZExt`, `SExt`, `Trunc`, `ToFour`, `ToTwo`). A
  same-width sign change emits nothing. The backend does not re-derive any of it,
  and an operand pair that disagrees in width or domain is an IR error: the
  verifier (`src/ir/Verify.h`) asserts it after lowering and before code
  generation, and the backend asserts the same invariants instead of repairing
  them. Operands the language leaves self-determined (logical operators, shift
  amounts, the exponent of `**`) keep their own domain. A container's ordering,
  which is not an operand, is part of its type (`Type::elem_signed`,
  `Field::is_signed`).
- A comparison, reduction or logical operator over 2-state operands has a 2-state
  result and lowers to a native compare; a case-equality is always 2-state.
- A value's width and domain come from one function (`ir::value_types`) shared by
  the builder, the verifier and the backend, not stored on instructions.
- Values of any width are LLVM integers of that width. Multiply, divide and modulo
  above 128 bits call runtime word routines (`rt_wide_*`) in place of the native
  operation, which LLVM lowers to a bit-serial loop (divide) or a quadratic
  expansion (multiply); no other operator has a ceiling. A value whose integer would
  pass LLVM's limit (2^23 bits; 2W for a 4-state value of width W) is refused with a
  located error at the declaration or expression that introduces the width.
- The constant pool keeps the frontend's encoding; the backend converts on read
  through one shared set of helpers.
- The encoding's invariants (must & ~may == 0, z only on unknown bits) are checked
  by assertions in debug builds (`CLOTHO_EXTRA_ASSERTS`) wherever a value is stored
  to a signal or frame, staged for a nonblocking update, passed or returned, handed
  to a runtime routine (format operands, wide selects, DPI arguments) or taken back
  from one (`emit_encoding_check`); a violation is a located internal error
  (`rt_encoding_violation`). Nothing is generated otherwise. Bits above a value's
  width cannot exist: it is an LLVM integer of exactly that width.
- Reals are native floating point everywhere: SSA, storage, arguments and
  results. **Not yet:** stored and passed as integer bit patterns.
- Handles (class, event, chandle, virtual interface) are a pointer plus a
  generation. **Not yet:** a 64-bit integer.

## Operators

- Width-1 binary operators read a constant truth table, indexed by both
  operands' plane bits.
- Add, subtract and multiply test for any unknown bit and branch to a cold all-x
  block; the hot path is the native operation.
- 4-state divide and modulo: a native hot path; unknown bits, a zero divisor and
  MIN / -1 are guarded off it, combined into as few branches as profiling
  supports.
- 2-state divide and modulo are straight-line (zero divisor gives 0, MIN / -1
  wraps); `--sanitize=divzero` adds a cold located report of a zero divisor and
  nothing otherwise.
- `**` is one call to a support-library routine (Table 11-4 and the wrap at the
  result's width); the generated code decides x and z bits in an operand first, and
  only when one can have them. One unsigned bit stays a truth table.
- XOR and XNOR test for unknown bits and use one native XOR when all are known.
- `->` and `<->` are IR operations lowered from their own three-valued rules.
- Concatenation, replication and streaming reversal build independent
  constant-offset terms, with pattern forms (bit-reverse, byte-swap, sign
  extension, multiply by a repeating constant). **Not yet:** concatenation,
  which is still a serial shift-and-OR chain: independent terms measured
  +6.6% instructions on tests/perf timers (a 32-operand 4-state concat).
- `$clog2` of an argument with an x or z bit is all x, in constants and at run time:
  the LRM is silent (20.8.1) and Icarus agrees (docs/LRM_NOTES.md, note 53). An x
  where a known value is required, a width or a range bound, is slang's located
  error. The constant case is `patches/slang-clog2-unknown.patch`.
- Real math: the classic functions as LLVM intrinsics, the rest as C math calls.
- `?:`, `&&` and `||` lower to control flow in the frontend; the optimizer
  recovers selects. **Not yet:** control flow only for arms unsafe to speculate.

## Storage

- Each signal's slot has the best encoding known for it: a 2-state signal is one
  W-bit word, and a 4-state signal drops its z plane when the may-z analysis
  proves it z-free.
- Storage that aliases has one layout. A signal is narrowed only when every use,
  by-reference aliases included, allows it. **Not yet:** any signal passed by
  reference takes its type's default layout.
- Aggregate members, container elements and subroutine locals are narrowed per
  object by analysis. **Not yet:** by type, with full-form exceptions.
- Slot types are named LLVM structs, one per distinct shape.
- Power-on values: scalars in constant initializers; arrays zeroed and filled by
  a startup loop.
- A runtime-indexed unpacked parameter is an ordinary unpacked array, stored
  read-only. **Not yet:** writable state, one copy per instance.

## Selects and stores

- Constant indices are specialized in the backend: an in-range constant select
  is the plain form, an out-of-range one is the default value or a no-op.
- Constant selects touch only the 64-bit words they cover; part-select stores
  read and write only those words, plane by plane.
- Indices reach the backend as signed numbers of at least 64 bits, which the
  frontend extends by each index's own signedness; wider indices are checked and
  treated as out of range.
- A dynamic select branches to a plain fast path when its index is known and in
  range; the mask path runs only otherwise. On storage of 64 bits or less too: the
  branch costs 20-60% fewer instructions than the mask (an 8-bit 4-state read 150 ->
  48, a 64-bit one 105 -> 47), and with an index that is invalid half the time at
  random it costs 3% more cycles, against 23% fewer for one always in range.
- A runtime helper serves only runtime-indexed selects of storage above 256 bits,
  where it costs less than the inline shift (it is flat at 117-141 instructions for a
  bit select; the inline write costs more from 384 bits, the read from 1 Kbit, and the inline
  form's compile time grows quadratically). It works a 64-bit word at a time
  and receives only the planes the destination needs.
- A fixed unpacked array's declared range becomes a 0-based position in the
  backend, next to the range check.
- An element access with an invalid index is guarded so only valid paths touch
  memory. The select itself is safe by construction; no consumer can bypass it:
  a place whose select may be invalid is a `Guarded`, not an `LValue`, with its
  storage pointer private, so no raw primitive accepts it and the guards
  (`when_valid`, `read_or`, `pointer_or`) are the only way to an address.
- Change detection after a blocking store compares the whole slot before and
  after; through the wide-select path it compares only the words the store
  touches.
- A queue element's physical slot is `head + i`, minus the capacity once it
  reaches it.
- An associative-array select carries its key; the read resolves without
  creating an entry, the write creates it.

## Calls and the runtime boundary

- Scalar arguments and results pass by domain: a 2-state scalar is one native
  word, a 4-state scalar the full form.
- A runtime routine receives a pointer to the value's own storage and a
  compile-time constant shape descriptor; the runtime decodes the planes through
  shared helpers. **Not yet:** values are translated per call into a separate
  value/x/z encoding.

## Nets

- A multiply-driven net without strengths accumulates driven-0, driven-1 and x
  masks over all drivers and resolves once.
- Drive strengths are per-driver compile-time constants, resolved by per-level
  mask arithmetic. An x/z driver with asymmetric strengths contributes a strength
  range (LRM 28.12.3). Strength is carried with the value only on nets connected
  to switch primitives. **Not yet:** no switch strength.

## Sanitizers

- A sanitizer's code exists only when it is on: the access calls a checking
  routine that decides and reports. **Not yet:** the bounds verdict is computed on
  every select.
