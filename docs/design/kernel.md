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
- A 2-state value is a native W-bit integer. Its domain comes from its IR type,
  the single source of truth. **Not yet:** each value also carries its own flag.
- Domains, signedness and extension are decided by the language, through slang's
  legalized AST. The IR carries them explicitly (signed and unsigned opcodes, the
  extension kind); the backend does not re-derive them, and an operand pair that
  disagrees is an IR error caught by an assertion. **Not yet:** the backend
  promotes silently, reads signedness from the left operand and picks the
  extension by the target's sign.
- A comparison, reduction or logical operator over 2-state operands has a 2-state
  result and lowers to a native compare. **Not yet:** always 4-state.
- Result width, domain and signedness are recomputed in the backend from operand
  and declared types, not stored on instructions.
- Values of any width are LLVM integers of that width. If profiling shows compile
  time or code quality falling off, or a width exceeds LLVM's limit, a ceiling
  routes wider values to our own bigint. **Not yet:** no profiling, no ceiling.
- The constant pool keeps the frontend's encoding; the backend converts on read
  through one shared set of helpers.
- The encoding's invariants are checked by assertions in debug builds at stores
  and at call and runtime boundaries. **Not yet:** no checks.
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
  wraps). **Not yet:** a sanitizer that reports the zero divisor.
- `**` calls one support-library routine. **Not yet:** an inline loop.
- XOR and XNOR test for unknown bits and use one native XOR when all are known.
- `->` and `<->` are IR operations lowered from their own three-valued rules.
- Concatenation, replication and streaming reversal build independent
  constant-offset terms, with pattern forms (bit-reverse, byte-swap, multiply by
  a repeating constant). **Not yet:** a serial shift-and-OR chain.
- `$clog2` of an argument with x or z bits: decided after checking what other
  simulators do. **Not yet:** all-x, checked against Icarus only.
- Real math: the classic functions as LLVM intrinsics, the rest as C math calls.
- `?:`, `&&` and `||` lower to control flow in the frontend; the optimizer
  recovers selects. **Not yet:** control flow only for arms unsafe to speculate.

## Storage

- Each signal's slot has the best encoding known for it: a 2-state signal is one
  W-bit word, and a 4-state signal drops its z plane when the may-z analysis
  proves it z-free. **Not yet:** a 2-state signal written from a z-capable source
  keeps a z word.
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
  is the plain form, an out-of-range one is the default value or a no-op. **Not
  yet:** one sequence for both, left to constant folding.
- Constant selects touch only the 64-bit words they cover; part-select stores
  read and write only those words, plane by plane. **Not yet:** the whole slot.
- Indices reach the backend as 64-bit offsets, extended by their own signedness;
  wider indices are checked and treated as out of range. **Not yet:** packed
  offsets are 32-bit, with indices that do not fit saturated out of range.
- A dynamic select branches to a plain fast path when its index is known and in
  range; the mask path runs only otherwise. **Not yet:** a storage-wide mask on
  every access.
- A runtime helper serves only runtime-indexed selects of storage above a fixed
  threshold. It works a 64-bit word at a time and receives only the planes the
  destination needs. **Not yet:** constant indices also call it, it loops per bit,
  and all three planes are always passed.
- A fixed unpacked array's declared range becomes a 0-based position in the
  backend, next to the range check.
- An element access with an invalid index is guarded so only valid paths touch
  memory. The select itself is safe by construction; no consumer can bypass it.
  **Not yet:** each consumer applies a carried validity flag.
- Change detection after a blocking store compares the whole slot before and
  after; through the wide-select path it compares only the words the store
  touches. **Not yet:** the whole slot always.
- A queue element's physical slot is `head + i`, minus the capacity once it
  reaches it. **Not yet:** a 64-bit remainder.
- An associative-array select carries its key; the read resolves without
  creating an entry, the write creates it.

## Calls and the runtime boundary

- Scalar arguments and results pass by domain: a 2-state scalar is one native
  word, a 4-state scalar the full form. **Not yet:** always the full form.
- A runtime routine receives a pointer to the value's own storage and a
  compile-time constant shape descriptor; the runtime decodes the planes through
  shared helpers. **Not yet:** values are translated per call into a separate
  value/x/z encoding.

## Nets

- A multiply-driven net without strengths accumulates driven-0, driven-1 and x
  masks over all drivers and resolves once. **Not yet:** a pairwise fold.
- Drive strengths are per-driver compile-time constants, resolved by per-level
  mask arithmetic. An x/z driver with asymmetric strengths contributes a strength
  range (LRM 28.12.3). Strength is carried with the value only on nets connected
  to switch primitives. **Not yet:** no strength ranges, no switch strength.

## Sanitizers

- A sanitizer's code exists only when it is on: the access calls a checking
  routine that decides and reports. **Not yet:** the bounds verdict is computed on
  every select.
