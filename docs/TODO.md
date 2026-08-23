# TODO — Road to full IEEE 1800-2023

This is a TODO **spec**, not a changelog or design doc. Treat it as such: when a
line item is partially implemented, split it so the unimplemented remainder
stays tracked. Clotho must never silently accept a construct it does not
correctly lower; every unimplemented gap below is guarded by a loud
`fail`, or must grow one when its surrounding feature lands.

Most of those guards are in the frontend, and a few are necessarily in the
backend: whether a destination is a `string` handle, a part-select or a whole
slot is not known until storage is assigned. Both kinds now report identically —
`file:line:col: error: <message> [<code>]`, the shape `render` (src/Diagnostic.cpp)
gives a frontend item. A backend refusal used to print `llvm backend error:
<prose>` with no location and no code, so there was nothing to jump to and
nothing to filter on, and it named an internal pipeline stage at a user whose
language feature had been refused. `lower_region` (src/ir/LlvmBackend.cpp)
attaches the offending instruction's source extent to the `BackendError` on its
way out, which covers every refusal raised while lowering an instruction, and
`report_backend_error` renders it: `unsupported-construct` for a legal construct
the backend does not lower, `internal-error` for a broken invariant (the guards
whose message opens with `internal:`). Still outstanding: the refusals raised
OUTSIDE instruction lowering have no instruction to point at and stay unlocated
— the per-process reactive / `program`-region pre-passes, DPI signature and
declaration emission, class-descriptor and id-global emission, and VCD variable
emission. Each names the process or symbol it is about in its message instead,
so they are legible but not navigable.

Items are organized by the five delivery milestones, then grouped by logical
similarity. Within each group, unimplemented items come first for visibility,
then completed items as one-liners. Lower milestones unblock higher ones.

- **M1 — Verilog-2005 RTL.** Run typical synthesizable V2005 (picorv32, small
  custom designs) end to end.
- **M2 — Icarus Verilog parity.** Everything iverilog handles: full V2005 plus
  the SV subset and tooling (VCD, tasks, gates, UDPs, real, file I/O).
- **M3 — Common SystemVerilog.** The SV that icarus/verilator/yosys accept:
  user types, packages, interfaces, dynamic data, basic classes, immediate
  assertions.
- **M4 — Industrial SV + UVM.** Run a real UVM testbench (suggest OpenTitan's
  Ibex UVM env, or the smaller CV32E40P / a minimal UVM agent as a stepping
  stone): full OOP, randomization, concurrent assertions, coverage, DPI.
- **M5 — Full LRM.** No exceptions or compromises: complete system
  tasks/functions, VPI/PLI, specify/timing, nettypes, config, checkers.

M1–M3 make Clotho usable in the open-source community; M4 brings it to
professional parity; M5 makes it complete.

---

## M1 — Verilog-2005 RTL

### Operators & expressions

- [x] **Division / modulo / power** (`/`, `%`, `**`) — all widths, signed+unsigned
  (`**` signed base/exponent corners per Table 11-4).
- [x] **Signed arithmetic** — sign extension, signed compare/divide/modulo/shift,
  mixed-sign rule (§11.8.1), `$signed`/`$unsigned`, shift-amount >= width.
- [x] **Multi-bit `if` / `?:` / loop condition** — reduced to 1-bit truth (§12.4, §12.7).
- [x] **Lazy `?:` arm evaluation** (LRM 11.4.11) — a definite condition evaluates
  only its arm; an ambiguous one evaluates both and blends them bitwise (Table 11-20),
  which is a run-time branch inside `lower_lazy_conditional`, not a compile-time
  choice. Which arms may instead take the branchless packed select is decided by
  `is_speculation_safe` (`Lower.cpp`'s `SpeculationScan`), a WHITELIST over
  `ExpressionKind`.
  It used to be a blacklist of three node kinds — a call, an embedded assignment, an
  increment / decrement — and that polarity fails OPEN: every construct clotho lowers
  to a trapping runtime check silently became a wrong answer at this one site, and
  five such traps exist that are none of those three. `(h == null) ? -1 : h.x` fataled
  on a null handle deref from the arm the condition had ruled out; so did a null
  virtual-interface deref, a read of a non-active tagged-union member (LRM 11.9), a
  read of a stale `ref` formal (LRM 13.5.2), and `new src`'s virtual dispatch through
  a possibly-null source. A whitelist fails CLOSED: an unlisted kind merely takes the
  branching path, which is correct for every expression by construction, so being
  conservative costs one `SelectLazy` and being wrong costs a fatal on legal code.
  Deliberately still speculatable, because each has an LRM-defined value and no trap:
  an out-of-range index into a fixed / dynamic array or queue, a missing
  associative-array key, integer divide / modulo by zero (all-x), real divide by zero,
  and a string index out of range.
- [x] **Aggregate `?:`** (LRM 11.4.11 over an unpacked struct / fixed unpacked
  array) — `o = c ? x : y;`, `st d = c ? x : y;`, `return c ? x : y;`,
  `(c ? x : y).m`, `$display("%p", c ? x : y)`, `(c ? x : y) == z`. The clause's
  last legality rule is what routes an aggregate here — "For all other cases, the
  type of the first expression and second expression shall be equivalent (see
  6.22.2)" — so both arms and the result share one layout and the only question is
  which arm reaches the destination. Motivated by Accellera UVM's
  `uvm_factory.svh:2318`,
  `m_uvm_factory_type_pair_t match_type_pair = match_original_type ? override.orig : override.ovrd;`
  over a `{ uvm_object_wrapper, string }` struct, which fataled on
  "unsupported unpacked struct rvalue".
  DESTINATION-DIRECTED, not staged: `emit_agg_conditional` opens two `Cond`
  regions and recurses into `emit_agg_store` with the SAME destination lvalue, so
  the selected arm is built straight into the target and no temp exists on either
  definite path. A variable arm is one `AggCopy` memcpy, a `'{...}` arm is built
  in place, a call arm writes through its own out-param, and a nested chain
  (`c1 ? x : c2 ? y : z`) falls out of the recursion for free. `return` needed no
  case of its own for the same reason — `agg_return_operand` already falls
  through to `emit_agg_store`, whose destination is the return variable.
  Arm evaluation is CONDITIONAL, never speculative, and `is_speculation_safe` is
  not consulted at all: the whitelist exists to preserve the branchless packed
  select, and an aggregate has none — a copy is a sequence of stores whichever arm
  wins — so there is nothing to trade against the clause's "without evaluating".
  A call arm, a trapping arm and a pure arm all take the same branch, which is
  strictly stronger than the integral path's guarantee.
  A 2-STATE CONDITION EMITS NO AMBIGUITY MACHINERY. `conditions_may_be_ambiguous`
  proves the ambiguous rule unreachable from the condition's type, mirroring
  `lower_predicate`, which is what fixes the predicate's representation: a string
  condition reduces `StrLen` and a real condition compares against 0.0, both
  definite; everything else reduces the operand's own bits, which can be x exactly
  when those bits have an unknown plane to carry one, and a 2-state type has none
  by construction. So a `bit` condition emits one `Cond` over two `AggCopy`s and
  nothing else — verified with `clotho build --emit ir`: 46 IR lines, zero
  `agg_eq`, zero `case_eq`, against 84 lines with one `agg_eq` and three `case_eq`
  for the same design on a `logic` condition. That is the case UVM takes, and it
  also WIDENS what compiles: with no 11.4.5 compare emitted, a 2-state condition
  works over a struct with an associative-array member, which the compare itself
  cannot handle. A `matches` condition counts as possibly ambiguous even though
  `lower_pattern` builds its result from definite `CaseEq` tests — same
  fails-CLOSED polarity `is_speculation_safe` uses, since being conservative costs
  one dead compare and being wrong silently drops the merge.
  THE AMBIGUOUS PATH follows the clause in its own order: "then both the first
  expression and the second expression shall be evaluated, and compared for
  logical equivalence as described in 11.4.5. If that comparison is true (1), the
  operator shall return either the first or second expression." Both arms are
  staged into `make_canonical_agg_temp` temps rather than one being built into the
  destination first, because the destination may ALIAS an arm (`v = c ? y : v`)
  and `emit_agg_store` is handed only the destination's `ir::Index`, never the
  expression, so there is nothing to test aliasing against the way
  `array_copy_source` can. It costs nothing where it matters — this is the cold
  path and neither definite branch allocates. The compare is `AggEq` narrowed with
  `CaseEq` against 1, because 11.4.5 propagates X and the clause says TRUE (1).
  Then the two shapes split, and the split is the whole point:
  - An unpacked STRUCT takes "For all other data types, the value specified in
    Table 7-1 for the resulting type as defined previously shall be returned" —
    the WHOLE type default, not a member-wise merge. `struct {int a; byte b;}`
    with differing arms is `'{a:0, b:0}`.
  - A fixed unpacked ARRAY takes "their results shall be combined element by
    element. If the elements match, the element shall be returned. If they do not
    match, then the value specified in Table 7-1 for that element's type shall be
    returned." So `'{1,2,3}` against `'{1,9,3}` is `'{1, 0, 3}`.
    Read LITERALLY and one level deep: a mismatching element that is itself an
    array or struct takes Table 7-1 for the ELEMENT TYPE, not a recursive merge,
    because the clause names the table rather than reapplying itself. No
    whole-array compare precedes the walk — `AggEq` over an array is the
    AND-reduce of exactly these per-element compares, so it would be dead weight.
  Neither Verilator nor iverilog is an oracle for the ambiguous path, and clotho is deliberately stricter
  than both. Verilator 5.050 is NON-CONFORMANT here: for
  `c = 1'bx` it returns the FALSE arm, `'{a:3, b:4}`, where the clause requires
  `'{a:0, b:0}` (its definite branches do agree). iverilog 13.0 cannot compile an
  unpacked struct at all — "sorry: Unpacked structs not supported." So the
  ambiguous expectations in `struct_test.cpp` come from the clause text, and are
  labelled as such rather than cross-checked.
- [x] **Table 7-1 defaults, separately from Table 6-7** — `emit_table7_1_default`
  is its own recursion and deliberately NOT a call into `emit_default_init`, which
  implements Table 6-7 (declaration defaults). Three rows disagree, so reusing it
  would have been silently wrong:
  1. EVENT. `emit_default_init` binds a FRESH event (`EventNew`) because Table 6-7
     says "New event" and two concurrent activations of one task must not share a
     synchronization object. Table 7-1 says `null`, which for clotho's event
     handle is the all-zero word (`InternPool` `Type::Kind::Event`: "`null` is the
     all-zero handle, which resolves to the reserved null object at slot 0"). The
     Table 7-1 path stores zero.
  2. UNPACKED UNION. Table 7-1 returns "Value specified in this table for the type
     of the FIRST member of the union". Refused loud rather than approximated, and
     the guard has to key on the SLANG type: clotho interns an unpacked union with
     `is_packed` SET (`lower_type` lowers it "identically to a packed union", one
     flat plane sized to the widest member), so the IR `is_union` flag does not
     distinguish one and a first attempt at this guard was dead code. The
     singular-leaf fill it fell through to is observably wrong, which is what makes
     the refusal necessary rather than pedantic: the blob's domain is the union of
     ALL members (`four |= mt.isFourState()`), while the table wants the first
     member's. For `union { int i; logic [7:0] w; }` inside a struct, an ambiguous
     condition filled all-x and `v.u.w` read back `xx`, where the first member is
     2-state and the table therefore requires `'0`. Writing only the first member's
     low bits instead would leave the blob's high bits undefined, and LRM 7.3 fixes
     no bit-level overlay for the unpacked case to say what they should become.
  3. MEMBER INITIALIZERS. `emit_default_init` has no notion of them; Table 7-1
     keeps a member's declared initial assignment in preference to its type
     default — "unless the member has an initial assignment as part of its
     declaration (see 7.2.2), in which case the member's value shall be as given
     by its initial assignment". Applied inline, one store per member, rather than
     as a second overlay pass over a default that would be thrown away.
     `struct {int a; int b = 7; string s = "keep";}` defaults to
     `'{a:0, b:7, s:"keep"}`.
  Every other row checked and agreeing: 4-state integral `'x`, 2-state `'0`, enum
  via its base type (an enum interns as its base, so the integral branch already
  answers at the base's domain), real/shortreal `0.0`, string `""`, class /
  interface-class / chandle / virtual-interface `null` (all-zero, which is what the
  2-state branch writes), variable-size unpacked array (dynamic / queue /
  associative) empty, fixed-size unpacked array per element.
- [ ] **`?:` over a queue / dynamic array / associative array** (LRM 11.4.11) —
  refused loud with its own message naming the clause. The two definite branches
  are reachable (they are the existing handle copy inside a `Cond`), but the
  ambiguous half is not: it has to stage BOTH arms in temps of the container's own
  type to compare them, and `make_canonical_agg_temp` builds aggregate storage,
  not a container handle. Refused whole rather than only for a 4-state condition,
  so the supported set does not depend on the condition's domain. For an
  ASSOCIATIVE operand the clause is explicit that the element-wise merge does not
  apply — "For aggregate array data types, EXCEPT associative arrays" — so the
  mismatch result would be the whole-type Table 7-1 default, an empty array.
  A struct with a queue or associative MEMBER is a different matter and already
  works under a 2-state condition (no compare is emitted); under a 4-state one an
  associative member is refused by `guard_agg_eq_members`, which owns that gap.
- [ ] **`?:` over an aggregate whose result type interns differently from the
  assignment target's** — refused loud. 11.4.11 requires the two ARMS be
  equivalent (6.22.2) to each other, not that either match the target, so slang
  can hand over a distinct-but-equivalent type; every member / element walk
  addresses the target's interned type, so a merely equivalent layout needs a
  converting copy rather than the direct one.
- [x] **`AggEq` miscompared a `real` leaf** — found while adding the aggregate `?:`,
  whose ambiguous path reaches the same node, but it PREDATED that and reproduced on
  `main`: `x == y` over `struct { real r; string s; }` SEGFAULTED with no diagnostic
  at all (exit 139). `LlvmBackend.cpp`'s `lower_agg_eq` converted a real leaf to a
  real plane in its `vt.is_real` tail and then compared it with
  `lower_compare(Inst::Tag::Eq, ...)` — the INTEGER compare — which read the
  `double`'s bit pattern as an integer image and walked unknown planes that a real
  plane does not carry. The leaf compare now picks `FEq` through
  `lower_real_compare`, the same helper the scalar `==` reaches; reals carry no x/z,
  so the definite 1-bit result needs none of `lower_compare`'s unknown-plane
  machinery. The stopgap frontend refusal in `guard_agg_eq_members` is gone and its
  test is now a behaviour test, cross-checked against Verilator on a real+string
  struct, a real+int struct, and `!=`. The ELEMENT-wise path was never affected: the
  aggregate `?:`'s per-element merge already picked `FEq` for a real element, so
  `real p[0:1]` merged to `'{1.5, 0}` correctly throughout.
- [ ] **Eager predicate evaluation outside `?:`** — the same deficiency, in three
  places that do not consult `is_speculation_safe` at all because they evaluate
  predicates up front unconditionally: `&&&` conjuncts in a `cond_predicate`
  (`lower_conditions` ANDs every `cond.expr`, so `if (h != null &&& h.x)` fatals
  where LRM 12.4 reads as short-circuiting), every `case` flavour's item-label
  predicates (built before dispatch, while LRM 12.5 specifies in-order comparison),
  and `unique` / `unique0` / `priority` `if` arm conditions (defensible under 12.4.2's
  any-order licence for overlap detection, but it still fatals). `&&` / `||` / `->`
  are already correctly lazy via `lower_short_circuit`; `<->` is eagerly evaluated by
  definition. Fixing these is a structural change to each lowering (a `SelectLazy`
  chain plus pattern-variable binding order), not a reuse of the predicate.

### Subroutines

- [x] **Output / inout string formals** (function and task, LRM 13.5) — a string is
  Managed, so it now takes the Managed ABI of data-model.md §6 like a queue or a
  dynamic array: ONE `rt_string*` per formal in every direction (the caller temp),
  `rt_string_assign` copy-in for `inout`, and `rt_string_move` out of the frame slot
  in the callee's epilogue — free, since copy-out is always immediately followed by
  `deinit_body_strings`. The frontend's post-call copy re-resolves the actual, so
  any place a string store can name is a legal actual (queue / dynarr / assoc
  element, struct member, fixed-array element, class property, hierarchical ref),
  and a task's copy-out rides its completion epilogue (visible when the caller
  resumes, not when the body writes). A `disable`d activation skips copy-out, so the
  actual keeps the temp's content — the same policy as a scalar out/inout arg (LRM
  9.6.2 leaves it undefined; iverilog copies the pre-abort value instead). Fixed in
  passing: `formal_param_count` and `declare_function` disagreed on a function's
  out/inout string formal (1 param vs 2), which would have mis-indexed every later
  formal. Still refused: a mixed string / packed actual (slang rejects it first —
  neither direction is assignment compatible), and a string-element ARRAY by value.
- [x] **DPI-C import function** (LRM 35) — `import "DPI-C" [pure] function` over
  the scalar-mapped types (byte/shortint/int/longint/real/shortreal/bit/logic;
  by-value, direct native `call`, ~1.8ns/call), packed vectors (bit[N]→
  svBitVecVal, logic[N]→svLogicVecVal via rt_dpi_pack/unpack_logicvec), and
  `chandle` (opaque i64↔void*), and `string` (input arg via rt_string_data
  zero-copy — needs the rt_string NUL invariant — and return via rt_dpi_str_out);
  `input`/`output`/`inout` args, scalar/chandle/string return, `pure`+`context`.
  **export "DPI-C"**: C calls into SV — a C-ABI thunk marshals args and calls the
  lowered SV function, which addresses its own instance's state absolutely, so an
  export binds at lowering time and needs no scope lookup; functions,
  `input`/`output`/`inout` scalar/chandle/real/vec args, `input` string,
  scalar/chandle/real/void return, any number of tops. Loader: `--dpi-lib` takes a
  `.o`/`.a` (JIT-linked via addObjectFile — both directions) or a `.so` (dlopen,
  import only; refused when the design exports). Export direction is checked
  against verilator (not a 4-state oracle — it drops x/z on logic vecs). See
  docs/design/dpi-c.md. **svdpi.h runtime** (Annex H): packed-vector
  bit/part-select accessors (svGet/PutBitsel/Partsel Bit/Logic, svDpiVersion) +
  per-top scope table (svGetScope/svSetScope/svGetNameFromScope/
  svGetScopeFromName/svPut+GetUserData), and 1-D open-array accessors
  (svSize/svLeft/svRight/svLow/svHigh/svIncrement/svGetArrElemPtr1/
  svGet+PutBit|LogicArrElem1VecVal) in src/runtime/Svdpi.cpp, header
  src/runtime/svdpi.h, exposed as JIT absolute symbols. **1-D open arrays**
  (LRM 35.5.6.2, all directions, any element width): named-scalar element zero-copy
  (narrow [N×iW] storage is the C array; output/inout write it in place), packed
  bit/logic element marshal via inline LLVM IR at any width (emit_dpi_oa_pack/
  unpack_ir — constant-trip loop over the native iW/i2W design slot, N is a compile-
  time constant; LLVM legalizes must=trunc / may=lshr-i2W-W / reconstruct=shl-i2W-W,
  no hand funnel shift, z field gated on the slot's real has_z); output/inout unpack
  back after the call; svOpenArrayHandle = rt_svopen descriptor built per call. No C
  marshal — one mechanism at all widths (an earlier ≤64-bit autovectorized C fast
  path was dropped: thin edge only at -O0, standing two-impl hazard). Actuals: fixed
  unpacked array + dynamic array (LRM 7.5; named-scalar element zero-copy off the
  runtime {data,len} handle — verilator rejects dynarray open-array actuals; an
  empty array is handled robustly rather than fatally). Remaining DPI gaps are
  tracked as `[ ]` items under M4 "Foreign interface".
- [ ] **Class-method call in a function**.
- [ ] **Nonblocking assignment in a function**.
- [x] **Procedural-block variable lifetime** (LRM 6.21) — a declaration in a
  procedural block defaults to STATIC lifetime (one instance, initializer evaluated
  once before time zero, LRM 10.5), and an `automatic` one has the lifetime of the
  BLOCK: it is initialized on every entry, with its initializer if it has one, else
  with the type's default (LRM 6.8, Table 6-7). Neither reset happened before, in a
  process or inside a subroutine, so both lifetimes behaved identically and neither
  matched the LRM: an automatic declaration with no initializer kept the previous
  entry's value (a container local in a loop body accumulated across iterations),
  and a static one re-ran its initializer on every entry. `is_static_local` no
  longer requires a subroutine (the LIFETIME decides when the initializer runs, and
  a process local's storage is likewise persistent), and `emit_default_init` writes
  the Table 6-7 default at the declaration's own position: x / 0 per integral leaf,
  0.0 real, `""` string, `null` handle, an emptied container, per member / element
  for an aggregate. LRM 6.21's worked example now prints its documented
  `1 2 3 1 2 3 1 2 3` / `1 2 3 4 5 6 7 8 9`. A block re-entered WITHIN one call is
  covered too: the frame is per call, so `init_frame` alone was not enough.
  lower_var_decl still clears fn_ctx_ around the PreInit lowering, so a static
  initializer that reaches for a frame local fails loud rather than mis-resolving.
  An `event` local is skipped: its power-on value is its own handle, baked into the
  state initializer, and re-running a default init would unbind it. An
  automatic-lifetime event local is refused instead (Table 6-7's "new event" per
  activation would need a fresh object).
  * Perf follow-up: the aggregate default is unrolled per leaf, and tier-0 does not
    fold it (measured: 279 stores for `automatic byte mem[256]`). A bulk reset needs
    an op that memcpys the type's proto image, valid only for a trivially-copyable
    aggregate — a managed leaf has to keep the per-leaf path, which frees the
    previous entry's string / container instead of leaking it.
- [x] **`$time` / `$stime` / `$realtime` in a function** — the wheel is a module
  global (`clotho.wheel`), not a body parameter, so a plain function reads the
  clock exactly as a task does; the guard that refused it was stale. The value
  scales to the unit of the scope that DECLARES the function (LRM 3.14.2.3, the
  same rule as a subroutine delay), which is what makes one shared body correct
  for callers of different timescales. `$strobe` / `$monitor` stay refused there:
  their deferred body rides the caller's task-call record slot, which only a
  coroutine has.
- [ ] **NBA to a task frame local / output-inout formal**.
- [x] **Functions** — combinational callables (not inlined); `automatic`/`static`
  locals, recursion, early `return`, callee-descending sensitivity, NBA to a
  module signal / `static` local / array element (LRM 13.4.4 — same shadow-staging
  path as a task; frame-local NBA guarded).
- [x] **ref / const ref args** (13.5.2) — a pointer to the actual's storage,
  uniform for every storage class, so the formal may be any type at all (an
  `event` formal aliases its 64-bit handle word like a class handle) and
  the actual any whole place: a container element, an assoc element, a struct
  member, a class property, nested to any depth. A ref into a container's heap
  storage additionally carries that container's relocation epoch and is re-checked
  on every dereference, so a push or resize under a live ref is a located fatal
  rather than a dangling write (`--loose-stale-ref` downgrades it). Every other
  shape aliases storage that cannot move, and narrows to one plain pointer with no
  check — decided per formal from its actuals, unioned across each vtable slot.
  Nothing for the report rides on the call: the formal's name is the callee's, and
  the container's comes from the epoch pointer. See docs/design/data-model.md §3.
- [x] **Function output / inout args** (13.5) — pointer copy-in/copy-out ABI,
  coexists with the return value.
- [x] **Tasks** (13.3) — time-consuming coroutines with per-call frames;
  input/output/inout args, `automatic`/`static` locals, `#delay`/`@event`/`$time`/
  recursion, NBA to module signal / `static` local / array element.

### Memories & arrays

- [x] **Nonblocking whole-array copy** of an unpacked array (per-element
  deferred; single-memcpy record is a perf follow-up).
- [x] **Nonblocking `'{...}` pattern** to an unpacked array.
- [x] **Whole unpacked-array `==` / `!=`** (per-leaf AggEq, X-propagating).
- [x] **Fixed unpacked-array slice** `busB = busA[7:6]` (LRM 7.4.5): copy /
  equality / lvalue / NBA target, either direction. Flat-storage elements
  only; an aggregate-element slice and a whole by-value slice read (concat /
  `%p`) stay guarded.
- [x] **Indexed part-select slice** `a[i +: n]` / `a[i -: n]` of an unpacked array
  (LRM 7.4.5, constant width + runtime base): copy source / target, NBA either
  side, equality operand, and `input` / `output` / `inout` subroutine argument,
  over both endiannesses, a struct's array member, and a multidimensional array's
  outer dimension. Expanded at lower time into one ordinary element access per
  position (`emit_indexed_slice_copy`), which is what gives it LRM 7.4.5's
  per-position invalid-index rule for free: a position past the bound reads the
  element default and drops its write, and an x/z base invalidates every
  position. The base is evaluated once. A read is
  staged in a canonical temp. A string / handle-element slice works too (the copy
  is per element, each leaf a deep copy); the remaining gap the constant-bounds
  form shares is a general by-value read.
- [x] **Unpacked arrays of `string`** — a fixed array whose leaves own heap is now
  a first-class value: subroutine local, by-value argument in every direction,
  by-value return, whole-array assignment and slice. The copy that made it work is
  `emit_agg_deep_copy`, the per-leaf sibling of `emit_agg_init`/`emit_agg_deinit`:
  every string leaf is `rt_string_assign`'d and every container leaf deep-copied,
  with a memcpy short-circuit for any subtree that owns no heap. It replaces the
  raw memcpy at the three ABI copies (`emit_agg_copyin`, `emit_copyout`'s aggregate
  arm, the aggregate `Ret`), which would otherwise have duplicated ownership of one
  allocation and freed it twice. A managed-leaf RESULT also gets a body-lifetime,
  proto-initialized, GC-registered return buffer (`build_agg_elem_temps`) instead
  of a bare per-call `alloca`, since the callee assigns into its leaves and the
  heap it hands back has to be released at a body exit.
- [x] **Unpacked array as a function / task local, by-value argument, or return**
  (LRM 13.4/13.5): canonical aggregate frame storage; value semantics via a
  canonical-temp copy-in (the flat-vs-canonical z-elision layout hazard).
  `input` / `output` / `inout` all work for both functions and tasks; a task's
  copy-out rides its completion epilogue, so it lands when the task returns.
  Managed-leaf arrays included; `ref` aggregate formals stay guarded.
  The RESULT copy-out carried the mirrored hazard and crashed on it. It resolved
  the returned object with `agg_slot_ptr` unconditionally -- the RECURSIVE
  addressing mode, which reads `LValue::root_ty` -- and that field is filled only
  for a place in the canonical mirror. The two predicates part company exactly
  there: `is_agg_abi` is true for ANY fixed unpacked array, since it names the
  by-pointer ABI role, while `needs_agg_storage` / `contains_struct` are false for
  a plain scalar-leaf array, which stays FLAT `[N x slot]`. Nothing forces a
  returned object into the mirror, and a subroutine variable declared without
  `automatic` is static (13.4.2), so `return a;` returns a module SIGNAL: flat,
  `root_ty` null, GEP off null -- the compiler segfaulted on
  `function arr_t f(); arr_t a; ... return a;` with no diagnostic at all. Missed
  because every test here returned an `automatic` local, which
  `build_frame_layout` always puts in the mirror. `emit_agg_ret_copy` now resolves
  the source through `agg_image_ptr`, the one whole-object image resolver, and
  picks the copy from the two ELEMENT slots instead of assuming they agree --
  because they routinely do not. A flat slot is may-z analysed per signal while an
  aggregate leaf's is fixed by its type, so a never-z `logic [7:0] a[0:1]` is
  `[2 x { i16 }]` against the canonical `[2 x { i16, i8 }]`, and a 2-state array
  the analysis cannot clear is `[2 x { i64, i32 }]` against `[2 x { i32 }]`. The
  memcpy the crash was hiding would have over-read the signal by four bytes in the
  first and taken one element's low half for the whole array in the second, so
  equal slots memcpy and unequal ones convert per element through
  `emit_array_zdiff_copy`. A string-leaf array needs no conversion (a managed slot
  has no planes to narrow) and its flat `[N x rt_string]` is the mirror's bytes
  exactly, so the per-leaf deep copy addresses it unchanged.
- [ ] **Sliced memory target** for `$readmem*` / `$fread`.
- [x] **Unpacked arrays / memories** — element-addressed; decl, read,
  blocking/nonblocking write, nested bit/part-select, multidimensional,
  descending ranges, X power-on, async-read sensitivity.
- [x] **`$readmemh` / `$readmemb`** (§21.4).
- [x] **`$readmem*` / `$fread` element width > 64 bits**.

### Elaboration & structure

- [x] **Module variable declaration initializer** (LRM 6.8) — applied via a
  run-once `PreInit` process before any Initial/Always.
- [x] **Runtime-indexed constant table** (LRM 6.20 + 11.5.1) — a parameter /
  localparam with an aggregate value read at a non-constant index (`localparam
  int P[0:3] = '{...}; x = P[i]`), the constant lookup-table idiom. A parameter
  has no storage and a runtime index defeats the fold, so the value is
  materialized as an unscoped read-only signal, in one of two ways. A value with
  a plane image (integral / real / enum / packed struct / handle leaves, fixed
  unpacked arrays of them, and unpacked structs) is interned whole
  (`SignalInfo::const_data`) and baked into the state initializer: no elaboration
  loop, no time-zero fill, every read an ordinary indexed load. A value holding a
  heap-backed leaf (string, queue, dynamic / associative array) has no static
  form, so it is seeded once in the `PreInit` process instead, from the
  elaborated value rather than the initializer expression (a defparam / config
  override has no initializer). Covers module / package / class parameters,
  per-instance overrides, multidimensional and 4-state tables (x/z round-trip),
  whole-array copy, by-value actuals and `foreach`; scalar `string` and `null`
  parameters fold too. Refused loudly: a wildcard-index associative array, and a
  queue / associative array of aggregate elements. See
  docs/design/const-tables.md.
- [x] **Net declaration assignment** (`wire w = a & b;`, LRM 6.7.1).
- [x] **`generate`** — `for`/`if`/`case` and nesting.
- [x] **`defparam`** — parameter override by hierarchical path; cross-top paths
  unsupported.
- [x] **Instance arrays** (`foo u[N:M] (...)`, LRM 23.3.6) — multidimensional,
  bit-sliced port connections.
- [x] **Multiple top-level modules** — each flattened into the one design,
  shared timeline; cross-top hierarchical refs unsupported (see M2 Hierarchy reach).

### Procedural control

- [x] **`disable`** of a named block and of a task (LRM 9.6.2) — all three forms
  lower and simulate. LEXICAL in a process body: the block becomes an
  `ir::NamedBlock` region and `ir::Disable` branches to its end, a structured
  multi-level exit (SV has no labelled `break`), with no runtime at all. REMOTE
  (`ir::DisableRemote`): a sibling block, a block in another process, a block in a
  task or fork child, or a TASK — every activation the target names is marked and
  force-woken, and each aborts itself at its own poll point. LEXICAL in a body with
  several activations: both, since 9.6.2 ends them all. LRM 16.4.4's outermost-scope
  flush lands too, lexically and remotely, as an `ir::DeferFlush` generation bump.

  A disabled task's caller resumes after the task-enabling statement, which is what
  the existing epilogue already does, so an aborted activation takes that epilogue
  with copy-out skipped (9.6.2 leaves output/inout results unspecified). The
  machinery is one activation list per target: a frame-resident doubly-linked node,
  linked on entry and unlinked on exit, plus a claim on the `#delay` park record so
  the wheel and a `disable` cannot both wake the same park. No epoch, no process
  table, no `Wheel.h` change, no runtime change.

  Matches iverilog on 9.6.2 examples 1, 2, 3 (both readings), 4 and 5, plus: a target
  parked on a delay whose LATER delays must still land on time, an `always` body that
  re-arms, two instances disabled independently, two activations of one task ended by
  one statement, `#0` inside a target, a fork child from either side, and an
  assertion action block as the disabler. A design that uses only the lexical form
  pays nothing — picorv32's emitted LLVM is byte-identical.

  Still refused: a target whose body can park where the abort cannot claim the park
  (a TIME-CONSUMING task call, a fork, an `expect`). 9.6.2 disables downward along
  the enable chain, so the nested-call case needs the callee's activation ended too,
  which is a second list walk.
- [x] **`repeat (N)` loop** — negative or X/Z count runs zero times. The hidden
  counter and bound are per-*activation* cells (`make_temp_cell`): a frame Local
  inside anything with a frame (task, function, fork child, action block), a
  module Signal only in a process, which has none and is single-activation, so
  process IR is unchanged. Module-signal storage made every activation share one
  cell, so a recursive `repeat (3)` two levels deep ran 5 body iterations instead
  of 39 and two forked calls to one task ended on whichever count expired first.
  `foreach`'s snapshotted bound and its associative-array step flag share the
  mechanism, as does `lhs = repeat (n) @e rhs` (LRM 9.4.5).
  A **real count** is legal (12.7.2 wants only a numeric expression) and rounds
  half away from zero into a signed 64-bit count (LRM 6.12.1, `coerce`'s `FtoI`),
  so `repeat (2.5)` runs 3 times, `repeat (2.4)` 2 and `repeat (-0.6)` none; 64
  bits because that is where `FtoI` saturates instead of wrapping.
- [x] **`final` block** (LRM 9.2.3) — source order, zero-time.
- [x] **Looping `always` with timing control** (`always #5 clk = ~clk;`).

### System tasks

- [x] **`$random`** — unseeded V2005 signed-32 PRNG.
- [x] **Seeded `$random(seed)`** (LRM §20.15.1) — reads the inout seed lvalue,
  advances the reference stream, writes the updated seed back.
- [x] **`%0d` / width / left-justify format flags** (§21.2.1.2).
- [x] **`$display` / `$write` operands wider than 64 bits** — all radices multi-word.
- [x] **`%m`** — hierarchical scope name.
- [x] **`%u` / `%z`** — unformatted 2-/4-value output.

---

## M2 — Icarus Verilog parity

### Time-consuming procedural code

- [ ] **Intra-assignment timing remainder** (LRM 9.4.5) — a multi-signal trigger
  `@(a or b)`, an `iff` qualifier on the trigger, and the target shapes that still
  have no lvalue encoding to defer: a concatenation, a whole unpacked struct or
  array, a string, and a queue / dynamic-array / associative-array element (none of
  which plain NBA lowers either). (Blocking, nonblocking `#d`, and nonblocking
  `@e`/`repeat(N) @e` all done below, now including bit/part-select, unpacked-array
  element, packed/unpacked struct field, and real targets.)
- [x] **`iff` event-control qualifier** (`@(posedge clk iff en)`, LRM 9.4.2.3).
  Single-event only, lowered as a body/`@`-loop guard (`if (cond)`). Per-event
  `iff` in a multi-event list (`@(posedge a iff x or ...)`) still rejected loud —
  needs waking-source identification the shared-wake engine lacks.
- [x] **Event handles and every event storage class** (LRM 15.5) — `event` is a
  reference type: a 2-state 64-bit handle `{ u32 slot, u32 gen }` naming an event
  OBJECT, laid out and passed exactly like a class handle. `a = b` merges onto one
  sync object, `a = null` unbinds (`-> null` is a no-op, `null.triggered` is 0,
  `@(null)` blocks -- 15.5.5.2 leaves it undefined), `==`/`!=`/`===`/`!==` are
  identity and `if (e)` is the Boolean test (15.5.5.3).

  LRM Table 6-7 makes an event's default initial value a **New event**, so every
  event-typed storage location binds its own object when that storage comes into
  existence: a declared variable, an **unpacked-array element** (any dimension), an
  **unpacked-struct member**, a **class property** (per instance and `static`), an
  **automatic-lifetime local** (a fresh object per activation), and a **queue /
  dynamic-array / associative-array element**. A scalar declared event takes a
  compile-time slot in a static block; everything else draws from a chunked runtime
  pool whose chunks never move, so an object address survives growth while a
  process is parked on its waiter list.

  Waiters are an intrusive doubly-linked list on the object -- persistent nodes in a
  contiguous array, one-shot nodes frame-resident -- so the number of simultaneous
  waiters is unbounded. `@(<any event expression>)` works: a `WaitEdge` carries an
  optional handle operand, and a sensitivity list that would need one is left in the
  body, where it re-reads the handle each pass (a static subscription is issued from
  the seed loop and cannot re-resolve). Events pass as **subroutine arguments** in
  every direction and across **module ports** (aliasing, not copying, in both
  directions -- `inout event` is illegal SV).

  `->> e` defers to the NBA region through a runtime pending chain; `->> #d e`
  schedules one detached wheel timer carrying the handle; **`->> @(x) e` /
  `->> repeat(n) @(x) e`** register a detached one-shot subscription on the edge,
  growable because a loop makes one per pass. **`wait_order`** (15.5.4) desugars to
  one park per stage, each waiting on the events still ahead of it, with the
  else-arm on an out-of-order trigger and a run-time error when it is absent.

  Event operations work **inside a function**: the timing wheel is a module global,
  so a body with no wheel parameter still reads the clock. A monomorphism analysis
  keeps an event that is never assigned folded to a constant object address, so a
  design that does not merge pays nothing.

  Objects are never freed: LRM 15.5.5.2's reclamation is garbage collection, which
  Clotho does not have (class bodies leak by the same direction).
  Design: docs/design/event-handles.md, docs/design/named-events.md.
- [x] **`join_any` / `join_none`** (LRM 9.3.2, V2) — the join kinds differ at run
  time in one join-record field (`wake_at`: 0 for `join`, W-1 for `join_any`, never
  reached for `join_none`) and in when a child starts. `join` keeps resuming children
  inline at the fork; the detaching kinds must not run a child the parent is not
  waiting for, so `join_none` queues every child and `join_any` runs them only until
  one completes. Their record is heap-allocated per fork SITE and reference-counted,
  because the parent no longer outlives its children; `join`'s stays a frame slot and
  its codegen is byte-identical. Unbounded live children saturate the activation
  bound, which sizes the rings, the `#0` batches and the wait buckets alike.
  A capture of an enclosing task frame was refused for the detaching kinds (the ref
  ABI needed the frame to outlive the child); lifted by V4, which made a capture a GC
  box handle for every kind.
  Design: docs/design/fork-join.md §"V2 scope".
- [x] **`fork … join_none` inside a function** (LRM 13.4.4) — the only kind the LRM
  allows there ("fork-join_none constructs shall be allowed inside a function", with
  children holding anything a task may); `join` / `join_any` in a function are
  illegal SV and slang rejects them itself, so `lower_fork`'s non-task check is now
  kind-specific. The spawn path needed almost nothing a function lacks — `state`,
  `rng` and (with process control) the caller's `proc` block are parameters, the
  wheel is the `clotho.wheel` global, the record is heap and refcounted, the region
  is a compile-time bit, and a detaching kind carries no captures. One line did need
  gating: V3 published the heap record into the `coro_begin_`-anchored
  `fork_live_slot`, which `join_none` never needs (it cannot park, so no abort can
  intervene) — now `!none`, which also removes two stores from every detached fork
  in a process-control design. The real work was 13.4.4's second paragraph: the
  calling thread must belong to an `initial` / `always` procedure, so
  `check_spawn_roots` (a spawn-reachability fixpoint over the call graph) plus
  `Process::spawn_root` report a static-variable initializer, a continuous
  assignment, a `final` block, a Preponed / Observed / Re-NBA process, a `$strobe` /
  `$monitor` body and a DPI export, each with the fork site and a "called from here"
  note. All five accepted the spawn silently before, and the `final` one leaked the
  record and the child frame. `reject_resume_once` now closes over calls as the
  backend backstop. Children spawned in a function belong to the CALLING thread, so
  `wait fork` / `disable fork` in the caller reach them (LRM 9.6.3). A child still
  can now read the function's own formals and locals — exactly what the LRM's own
  `watch_for_zero` example does — since they ride GC boxes rather than frame pointers
  (see "Captured-automatic lifetime"). (`$time`
  in a function was refused when V5 landed; it is supported now — see "`$time` /
  `$stime` / `$realtime` in a function" under Subroutines.) Design:
  docs/design/fork-join.md §"V5 scope".
- [x] **`fork`-child capture of a local the ref ABI cannot carry** — no fork capture
  goes through the `ref` ABI any more, for any join kind, so the type restriction is
  gone with it: the capture rides a per-variable GC box and only the box HANDLE is
  passed (see "Captured-automatic lifetime" below). A string, a real, an unpacked
  struct, a queue, a class handle and a formal all capture, under `join` as much as
  under the detaching kinds.
  The generalization is the point. Keying the capture ABI on the JOIN KIND produced
  two refusals that both read as fork limitations and were neither about forking: the
  fat ref could only carry a packed integral or a dynamic array (a type limit), and
  `join`'s "the parent outlives every child" is false under `kill`
  (process-class.md §11 hazard 3 — reachable with a plain `logic [7:0]` capture, so
  `join`'s by-ref path was not sound either). One storage rule for all three kinds
  retires both. It also cannot be written any other way: the boxing decision is made
  per subroutine before its frame is laid out, and `design_uses_process_` — which the
  design doc used to prescribe as half the predicate — is an IR fact computed after
  the whole module has lowered.
  Cost: one `ClassNew` per captured variable per activation, paid only by a body that
  forks over its own frame variables. A fork in a PROCESS body has no captures at all
  (a process-scope local is signal-like storage), so the common testbench idiom is
  unaffected and emits the same code.
- [x] **`fork`-child capture of a class property through the implicit `this`** — a
  bare property reference inside a method needs the method's `this`, not the property,
  and `this` is an ordinary automatic of the frame: slang's
  `SubroutineSymbol::thisVar` is a real `VariableSymbol` already living in `localmap`
  as Local 0. So `ForkCaptureDetector` maps a non-static `ClassPropertySymbol` onto
  `thisVar` and captures that; an explicit `this.p` already arrived that way. The
  refusal existed only because the ref ABI carried a packed-slot pointer rather than a
  handle, so it died with the ref. Three supporting pieces: `install_capture_boxes`
  boxes `thisVar` explicitly (it is in neither `getArguments()` nor the body's
  declarations, and the implied `this` reference is not written anywhere in the AST, so
  `ValueRefScan` flags it); the four sites that loaded `this` out of Local 0 now share
  one `this_handle()` that consults the box first; and `build_fork_child` installs
  `has_this` / `this_var` / `this_class` from a captured `this` so a bare property
  inside the CHILD re-roots the same way.
- [x] **`wait fork` / `disable fork`** — landed, but not here: the live-descendant SET
  V2's count-and-`wake_at` record could not carry is the `process` control-block tree
  (LRM 9.7), so both statements came with it. `disable fork` is `kill` over the
  caller's subtree; `wait fork` is `first_child == null`, exact because a completing
  thread re-parents its live children. Design: docs/design/process-class.md §8.
- [x] **Captured-automatic lifetime** (LRM 9.3.2, fork-join.md V4) — this entry used
  to ask for "per-spawn automatic-variable lifetime", i.e. copies. **That was the wrong
  target**: 9.3.2 makes a reference to an enclosing scope's automatic a reference to
  THAT variable, which is why the clause has to write `automatic int k = j;` to get a
  copy. Measured on the clause's own idiom: iverilog `4 4 4` (shared), Verilator
  `1 2 3` (copies, a deviation), and slang warns by name (-Wfork-loop-var, "all loop
  iterations will see the same stale value"). Copying would have regressed a case
  clotho already got right in a process, where a local has static signal-like storage.
  The real gap was LIFETIME, and it is closed: a captured automatic lives in a
  per-variable GC box (a synthetic one-field class, so ClassNew allocates it, the trace
  map falls out of the field table, and the place is HandleDeref + MemberSelect 0), and
  the child receives the box handle by value. Live coroutine frames are already
  conservative pinning GC roots, so nothing refcounts and no frame ownership moves.
  Landed: shared reads in a task/function (`4 4 4`), a child's WRITE seen by the
  parent, a captured class-handle formal outliving the call (13.4.4's
  `watch_for_zero`), a string capture, `join_any` with several children. Also fixed on
  the way: a capture referenced from a child's DECLARATION INITIALIZER was invisible to
  the capture walk (an initializer hangs off the symbol, not the statement), so 9.3.2's
  own idiom failed as an unknown signal instead of capturing.
- [x] **Spawn-time initialization of a fork's own declarations** (LRM 9.3.2,
  fork-join.md §V4b) — the other half of V4, and the entry above listed it as the
  remainder. The clause: variables declared in the `block_item_declaration` of a
  fork-join block "shall be initialized to their initialization value expression
  whenever execution enters their scope and **before any processes are spawned**". So
  they are lowered IN THE PARENT at the fork, in declaration order, before any child is
  built or spawned — which also makes a later initializer able to read an earlier
  declaration (`automatic int a = j; automatic int b = a + 1;`). Once the variable is
  parent-side storage a child referencing it is an ORDINARY CAPTURE, and V4's box
  machinery carried that unchanged: no new IR, no backend change, no child-side
  mechanism.
  **TWO POSITIONS, and the old refusal conflated them.** `SpawnInitScan` fired for a
  declaration of the FORK's scope and for one of a child's own `begin … end` alike,
  while its message described only the first. The clause's own example settles the
  second: `fork automatic int k = j; … begin automatic int m = j; … end join_none` —
  `k` is "a local copy, k, for each value of j" (1, 2, 3) while `m` is annotated "the
  value of m is 4", because the nested block is a DIFFERENT block whose scope is
  entered when the child body runs. So the fork-scope form needed implementing and the
  body-local form needed the guard to stop firing: clotho's ordinary late lowering was
  already right there. Both are now pinned side by side with the bare capture —
  `444|123|444` — so the three cannot decay into each other.
  **PER ENTRY, NOT PER ACTIVATION.** This is the part that looks like a working feature
  when it is wrong. The box must be fresh per EXECUTION of the fork statement; reusing
  the per-activation box `install_capture_boxes` hands out makes the clause's example
  print `333` (one box, rewritten by each iteration, every child reading the last
  value) instead of `123`. What buys it is that lowering happens once while the fork
  executes many times: the home holding the handle is allocated once at lower time — a
  frame Local in a subroutine, a design-state signal in a process body, which has no
  frame and whose storage is a precise GC root (garbage-collection.md §5.4) — and the
  `ClassNew` that fills it is emitted AT the fork. Cost per fork execution: one
  `ClassNew` plus one handle store per declared variable, plus the initializer itself.
  A fork that declares nothing emits nothing.
  **THE REF-FORMAL CARVE-OUT.** 9.3.2 makes it illegal to refer to a formal passed by
  reference inside a `join_any` / `join_none` block "other than in the initialization
  value expressions of variables declared in a `block_item_declaration` of the fork".
  clotho refused capturing an `output` / `inout` formal outright, which refused this
  legal position too. Legal and implementable for the same reason: the initializer is
  evaluated in the PARENT, so no child-side access and no lost copy-out. Implemented by
  narrowing the scan rather than adding a check — `BoxCandidateScan` no longer collects
  references inside a fork-scope initializer — so a child BODY reading such a formal is
  still refused, now as the exact complement.
  **The `join` exemption was wrong.** The old guard exempted `join`, on the grounds that
  a `join` child is ramped and resumed inline at the fork so its initializers already
  run at the spawn. Measured: that held only in a subroutine body, and only by accident
  of the per-activation box; `fork automatic int k = 5; … join` in a PROCESS body failed
  outright with `reference to unknown signal 'k'`, and under `join_any` the declaration
  — lowered as a child process — completes immediately and satisfies the join before any
  real child runs. All three are the same root cause: a flat walk made every top-level
  statement of the fork body a child, so a declaration became a process.
  **THE OTHER LIFETIME, and this entry first recorded it as a refusal by mistake.** A
  STATIC declaration in a fork's `block_item_declaration` was rejected on the grounds
  that 6.21 and 9.3.2 disagree. They do not, and 6.21 says so outright: the `static`
  keyword "shall be required when an initialization value is specified … to indicate the
  user's intent of executing that initialization only once at the beginning of
  simulation". `static` is exactly the modifier that opts OUT of per-entry
  initialization, so 9.3.2's "whenever execution enters their scope" is the rule
  `automatic` selects — which is why the clause's own example spells it
  `automatic int k = j` — and its "before any processes are spawned" is a rule about
  WHERE, satisfied for either lifetime by lowering in the parent at the fork. The
  refusal also fired for a declaration with NO initializer, where neither clause is even
  engaged, and took a working construct with it: `initial fork : c reg x; join` plus a
  hierarchical `a.c.x` passed before that commit and failed after. Restored to what it
  was — ordinary static storage in the block's own scope (`lower_var_decl`, so a module
  signal in a process body and a persistent signal in a subroutine, initializer in
  PreInit) — which needs no box and is no capture, since shared storage already outlives
  every child and a named fork stays a hierarchical scope.
  **Oracles.** Verilator 5.050 `--timing` agrees on every fork-scope case measured —
  the clause example, both UVM shapes including the labelled `fork : guard`, all six
  initializer sources, the once-per-execution side effect, sharing, and all three join
  kinds with timestamps — but is NOT an oracle for the body-local form: it prints
  `1 2 3` for the nested `m`, contradicting the clause's own comment. **iverilog 13
  cannot compile the `automatic` form at all** ("sorry: Overriding the default variable
  lifetime is not yet supported"), so the oracle that covered V1–V3 is unavailable
  there — but it does compile the STATIC form, and it and Verilator both print
  `PASSED x=1` for the hierarchical `a.c.x` case and `101 102 103` / `201 202 203` for
  the once-only initializer in a looping module fork and a task fork. Verilator alone
  covers the mixed fork, and agrees. Unchanged: the bare captured loop variable stays
  `4 4 4`. Verilator's `1 2 3` deviation there is process-body-specific — in a task
  body it prints `444` too.
- [ ] **1024 concurrent detached children** is the ceiling: an unbounded spawn edge
  saturates the activation bound, and past it the scheduler-buffer guards report a
  located FATAL. Lifting it means a spill on the already-emitted overflow edge; the
  dynamic wait bucket, which is scanned, is the hard half (fork-join.md §V2.3).
- [x] **Tasks** — time-consuming (see Subroutines).
- [x] **Intra-assignment timing** (LRM 9.4.5) — blocking (`x = #5 y;`,
  `x = @(edge) y;`, `x = repeat(N) @(edge) y;`), nonblocking `#d` (deferred wheel
  record, last-executed-wins per LRM 10.4.2), nonblocking `@e` / `repeat(N) @e`
  (deferred record on the trigger edge). A **real** target works on every form,
  including the event ones: `emit_intra_event_arm` and `emit_intra_nba_arm` both
  fill the deferred record through `fill_intra_payload`, whose first act is
  `real_to_planes`, so the record carries the IEEE-754 bit pattern in its narrow
  slot — the event form's claimed gap was stale and is deleted (two cases in
  tests/intra_assign_test.cpp, values from iverilog 13). The repeat count may be
  **real** in either form and rounds per LRM 6.12.1 (`repeat (1.6) @e` waits two
  edges). A count <= 0 assigns as if there were no repeat in the blocking form; the
  nonblocking form still waits, its record countdown being unsigned — a TODO in
  `lower_assign`. Design: docs/design/intra-assignment-timing.md.
- [x] **`wait (expr)` statement** (LRM 9.4.3) — level-sensitive suspend; subsumes
  `wait(e.triggered)`.
- [x] **Event control and `wait` on a class PROPERTY** (LRM 9.4.2 / 9.4.3) — `@p`
  on the enclosing `this`, `@(obj.p)` through any handle expression, and
  `wait(<cond reading properties>)`, for integral instance properties, non-edge and
  edge. This unblocks Accellera UVM: `uvm_event_base::wait_off`
  (`base/uvm_event.svh:120`) parks on `@on`, where `on` is the `protected bit`
  property declared at :54, and nothing in a UVM run gets past it.
  All three shapes failed before, and the third failed SILENTLY, which is the part
  that mattered most. `lower_wait_stmt` lowers `wait(c)` to
  `forever { if (c) break; @(read set of c); }` and the read set is built from
  SIGNALS; a property is heap storage inside an object, so the set came back empty
  and the process suspended on an event list nothing could ever fire — a permanent
  park with no diagnostic at lowering, at elaboration or at run time. The `@` forms
  were at least loud: `@p` reported "reference to unknown signal 'on'", and
  `@(c.p)` resolved the object while the process was being BUILT and aborted with
  "dereference of a null class handle, at 0 s" because `c = new` had not run yet.
  **Mechanism** (docs/design/classes.md, "Waiting on a property"): a property some
  `@` / `wait` in the design waits on carries hidden `event`-typed sibling
  properties — its change notifiers, one per waited edge — and a blocking store to
  the property classifies its own old -> new transition through the *same*
  `emit_edge_facts` a signal store uses, then wakes the notifiers whose edge that
  transition produced. Waiting is then literally `@(obj.<hidden event>)`, an
  existing tested path, so there is no new IR op, no new runtime state and no new
  wait engine. Because the classification is at the write, a fire always means a
  real change of the kind that waiter asked for: `p = p` fires nothing at all, and
  the waiter needs no snapshot and no re-comparison on resume.
  **Granularity: per property per edge, not per object.** A per-object notifier
  would also have been correct (9.4.2 forces a value comparison somewhere), but it
  is strictly worse here for one concrete reason: `uvm_event` writes `num_waiters`,
  `m_event`, `trigger_data` and `on` through the same object, so every unrelated
  write would wake every `@on` waiter and each would have to re-compare. Per
  property costs one hidden field per waited property and gives exact wakes.
  Per-EDGE on top of that is what makes edges exact rather than approximate: with
  one shared notifier a same-instant `0->1->0` pulse would wake a `posedge` waiter
  that then re-reads 0 and misses the edge, whereas classifying at the write cannot
  miss it. Only the edges the design uses are laid out, so `@p` alone costs one
  field and `@(posedge p)` alone costs one, never three.
  **Zero cost for a class nothing waits on**, by construction and asserted:
  a design-wide pre-pass (`WaitPropScan`, modelled on `EventMergeScan`) collects the
  (property, edge) pairs before any class is laid out — it must, since
  `lower_class_type` runs on demand from the first reference anywhere and can
  precede the `@` that needs a notifier. An unnamed class gets no field, no store
  barrier and no descriptor change; `Module::change_notify`'s empty-table early-out
  means even the compile-time question folds away. `tests/class_test.cpp` pins the
  field lists on both sides of that line.
  **Write paths**, enumerated from the code rather than guessed. The notifier rides
  `service_snapshot` / `service_after_write`, which is already documented as THE
  bracket around every blocking write path, so all ten sites are covered by
  construction and a new one cannot forget: `Store`, the dyn/queue element store,
  `$fgets`, `$sformat`/`$swrite`, `$ferror`, `$fscanf`/`$sscanf`, `$value$plusargs`,
  `$fread`, and the `ref` actual bracket at a function call and at a task call.
  That last pair is what makes a `ref` formal work at all, since a `ref` is a raw
  aliased pointer the callee writes through and never copies back. Also covered,
  through `Store`: a base-class method writing an inherited property (which is why
  a notifier is laid out immediately AFTER its property — appending after all
  properties would give the base and the derived class different indices and break
  prefix layout), `randomize()` writeback, and `output`/`inout` copy-out. Refused
  loudly, not silently: a non-integral property in either construct (no old -> new
  transition to classify), and `wait` over a condition whose only class-property
  reads are non-notifiable — that one used to be the silent park. NBA to a class
  property was already refused (TODO.md:1242), so that path cannot exist.
  **One inherited imprecision, uniform with signals**: a `ref` bound to a property
  and written by a task AFTER an intervening suspend wakes at the task's RETURN
  rather than at the write, because the bracket is at the call site and the callee
  writes through a bare pointer. Measured identical for a `ref` to an ordinary
  signal, so it is a property of clotho's `ref` ABI, not of this feature.
  **Oracle**: Verilator 5.050 agrees on every shape it can run (`woke at 1` for all
  three headline cases, and byte-identical on the mock `uvm_event`); it *aborts*
  with "Null pointer dereferenced" on `wait(c.on && g)`, resolving the handle too
  early — the bug clotho just fixed. iverilog 13 is no oracle: it rejects all three
  ("cannot synthesize expression: @.<0>", and for the `wait` form "Empty NexusSet
  from wait expression", which is the very same root cause named out loud).
  Table 9-2 edge behaviour is pinned to clotho's existing signal path, asserted
  equal to it in the same test; Verilator diverges there for signals too (it does
  not treat `x -> 0` as a negedge).
  Covered in tests/event_test.cpp, tests/wait_test.cpp and tests/class_test.cpp,
  including the four distinct UVM call shapes: `@on`, `@(obj.p)`,
  `wait(obj.p)`, and `@(m_events[obj].raised)` on an object held in an associative
  array keyed by a class handle (uvm_objection.svh:1078) — the last works with
  nothing added, because the notifier handle is loaded from whatever object the
  expression evaluates to at the moment the statement runs.
- [x] **`wait` on a CONTAINER class property** (LRM 9.4.3) — `wait(q.size() > 0)`,
  `wait(q[0] == 3)`, `wait(a.num() > 0)` and the like on a queue (7.10), dynamic
  array (7.5) or associative array (7.8) instance property, including a nested
  container (`q[0].size()`) and a mix of a container and an integral property in
  one condition. This unblocks the next UVM site after `uvm_event_base::wait_off`:
  `uvm_queue#(T)::wait_until_not_empty` (`base/uvm_queue.svh:218`) is
  `wait(queue.size() > 0)` over the `protected T queue[$]` at :58, and it failed
  with "wait on a class property of a type with no change notifier is not
  supported".
  **Why it is not just a wider type test.** The entry above rests on the STORE
  having an old and a new word to compare. A container has neither: its value is
  its contents, and `q.size()` is computed from the handle, not stored anywhere a
  write could snapshot. So a container property's single any-change notifier fires
  on MUTATION, with no comparison at all.
  **The `wait` / `@` asymmetry is what makes that sound, and it is the whole
  argument.** 9.4.3 `wait` is level-sensitive: it re-evaluates and parks again, so
  a fire that leaves the condition false costs one re-test and is unobservable,
  while a fire that never happens is a permanent park. Over-firing is free;
  under-firing is a silent hang. 9.4.2 `@` is the opposite: "a non-edge implicit
  event shall be detected on any CHANGE in the value of the expression", so a
  spurious fire there is an observable wrong wake — and mutation genuinely is not
  change, since `q.push_back(x); q.pop_back();` is two mutations and leaves
  `q.size()` where it started. **`@` over a container-dependent expression is
  therefore REFUSED LOUDLY** ("an event control on a container class property … is
  not supported; only `wait` can park on one") rather than silently given the
  mutation notifier. Doing it properly needs the expression's value snapshotted in
  the WAITER's frame and compared on each wake — a different mechanism, since the
  write side has nothing to classify; that is the `TODO:` on the refusal. A
  consequence worth naming: because only `wait` can reach a container notifier,
  only `Edge::Any` is ever requested, so a container property carries exactly one
  hidden field and never `.pos` / `.neg`.
  **The mutation surface, and why the list is closed** — two facts, not a survey.
  First, a SystemVerilog container is a VALUE TYPE: there is no handle to one, so
  the only reachable mutations are through an lvalue rooted at the property or
  through a `ref` formal bound to such an lvalue. Second, every mutation needs a
  writable pointer to the container's rt handle slot, and `lv_handle_ptr` is the
  only thing that produces one — every runtime mutator takes it as its first
  argument. Its second parameter is now a three-valued `HandleUse`
  (`Read` / `Mutate` / `Place`) **with no default**, so all 47 call sites had to be
  classified and one added later cannot compile without answering. Covered:
  queue `push_back` / `push_front` / `pop_back` / `pop_front` / `insert` /
  `delete(i)` / `delete()` / `sort` / `rsort` / `reverse` / `shuffle` / whole
  assignment (from a queue and from a dynamic array) / append-range; dynamic array
  `new[]` / `new[](src)` / whole assignment / `delete()` / the four ordering
  methods; associative array `delete(key)` / `delete()` / whole assignment /
  `'{...}` reset. An ELEMENT write is the one shape that does not touch the
  handle, so it is `Place` and fires from the direction the frontend already
  threads — `ir::ElemSelect::for_write` for `q[i] = v` / `d[i] = v`, and
  `assoc_elem_lvalue(for_write)` for `a[k] = v` and the `'{default: v}` slot; the
  assoc case must fire even when the value written equals the element default,
  because creating the entry changes `a.num()`. A `ref` / `output` / `inout`
  container formal is `Place` too and rides the existing `service_snapshot` /
  `service_after_write` bracket, which now carries a `container` flag: a `ref` is a
  raw aliased pointer the callee mutates and never copies back, and a task callee
  may suspend first, so the notify has to be at the call's RETURN. One fire per
  call instead of one per mutation inside it, which a level wait absorbs.
  A base-class method mutating an inherited container works through the same
  member select at the base-relative index, and `$copy` fires the DESTINATION's own
  notifier because the copy emits the same container ops (notifier rows are still
  skipped, for the aliasing reason above).
  A `Mutate` fire is emitted immediately BEFORE the runtime call it precedes, which
  is sound because `emit_event_wake_list` only pushes onto the ready list and a
  woken process cannot run before this one suspends; the only sites where a suspend
  can intervene are the `ref` / `inout` ones, which is exactly why those use the
  call bracket instead.
  **Also refused loudly**: a FIXED unpacked-array property (its extent never
  changes, so it has no mutation op — every write is an element store, and
  `agg_member` deliberately clears the notifier when descending into an interior
  aggregate, so it needs a separate change), and, unchanged, string / real / class
  handle / unpacked struct properties. NBA to a class property was already refused.
  **Zero cost** for a container nothing waits on, by construction and asserted:
  the pre-pass admits a container property only from a `wait` condition, and
  `class_test.cpp` pins the same class — one that churns its queue through
  `push_back`, `push_front`, an element write and `delete` — on both sides of the
  line, `{q, n}` versus `{q, $chg.q, n}`, plus prefix layout with the notifier in
  place (`B{q, $chg.q, pad}` / `D{q, $chg.q, pad, extra}`).
  Design: docs/design/classes.md, "Waiting on a container property". Covered in
  tests/wait_test.cpp (the UVM `uvm_queue` shape with a producer and a parked
  consumer, one case per mutation op, all three container kinds, a `ref` mutation,
  a base-class mutation, a mixed container + integral condition, containers of two
  objects, a nested container, and the negative space — a push-then-pop and an
  unrelated-key delete must leave the waiter parked) and tests/class_test.cpp.
- [x] **Named events** (LRM 15.5) — `event e;`, `-> e`, `->> e`, `->> #d e`,
  `@(e)` / `always @(e)`, `wait(e.triggered)`, at module, package, generate,
  procedural-block and `static`-local scope. An event is an OBJECT (trigger tick +
  intrusive waiter list + `->>` pending state) in the `clotho.events` array; its
  waiters are a doubly-linked list of frame- or array-resident nodes, so the number
  of simultaneous waiters is unbounded. Design: docs/design/named-events.md, and
  docs/design/event-handles.md for the handle layer.
- [x] **`fork` / `join`** (LRM 9.3.2, V1) — each child a coroutine; the parent
  joins all. Also inside a **task** body (and an assertion action block, which is
  lowered as one): a child takes the enclosing frame's packed integral /
  dynamic-array locals as `ref` formals, and the active-ring bound counts task
  activations rather than child bodies. The ring-overflow fatal the design doc
  promised is landed too (`rt_ring_overflow`).
  Two bounds that a fork child reaches but that were still sized as if it did not
  exist are fixed: the dynamic wait bucket counted `mod_.processes().size()`
  waiters, so a width-4 fork on one edge stored past the end of the bucket, and the
  `#0` batches were unguarded bump stores, so exceeding the saturated ring cap died
  with no diagnostic. The activation fixpoint is now `body_activations()`, shared by
  the ring and the bucket, and `emit_len_full_check` guards every bump-pushed
  buffer — both behind the existing `design_forks()` gate, so a design without a
  fork emits byte-identical code.
  Design: docs/design/fork-join.md.

### Continuous / structural timing

- [x] **Continuous-assign delay** (`assign #N lhs = rhs;`) — inertial delay on
  the driven net (LRM 10.3.3), pulse rejection via an epoch-cancelled detached
  record on the timing wheel, committed in the active region ahead of the drain
  (`emit_inertial_arm` / `rt_inertial_decide`). A delay on a whole driver of a
  multiply-driven (resolved) net rides that driver's own slot store, ahead of
  resolution (`store_driver_slot`). Gaps (guarded loud): rise/fall/turn-off delay
  triples `#(a,b,c)`, a select / concat / aggregate target, a net wider than 64
  value bits, and a delay on a PARTIAL driver of a resolved net (its slot powers
  up all-x, so the undriven bits would fold as x instead of z until the first
  store lands).
- [x] **Single-value gate delay** (`and #2 (...)`, LRM 28.16) — inertial, reusing
  the continuous-assign delay record (`emit_gate_as_process` -> `store_inertial_delay`).
  A gate driving a multiply-driven (resolved) net delays its own slot, and
  `cmos #d` / `rcmos #d` delay each of the two halves LRM 28.9 decomposes them
  into (identical waveforms to `nmos #d` + `pmos #d` under iverilog 13). Gaps
  (guarded loud): rise/fall/turn-off triples `#(a,b,c)` (now also rejected on
  cmos/rcmos), a bit-addressed output on a single-driver net, and a delay on a
  partial driver of a resolved net.
- [ ] **Net delays** (`wire #3 w;`) — delays every driver of the net (added to each
  driver's own delay, LRM 10.3.3); distinct from a continuous/gate assign delay.

### Procedural continuous assignment & force

- [x] **`force` / `release`** on a whole-scalar **variable** (integral or real; LRM 10.6.2,
  Phase 1): per-target hidden `force_site` state, gated ordinary writers, continuous
  re-eval process, last-force-wins. Includes **release-reestablish** of a variable
  driven by a module-level `assign` (the driver is made force_site-sensitive so
  release re-evaluates it). Input/inout-port targets guarded loud (port-copy driver
  is not force_site-aware). The gate keys off the write's ROOT, not its shape, so
  every ordinary write rooted at the target is suppressed: whole-variable,
  bit/part/element-select, packed-struct member, read-modify-write (`x += 1`,
  `x++`), a forced operand of a concatenation lvalue, and a task/function output
  copy-back — all byte-identical to iverilog 13. See
  docs/design/force-release.md.
- [x] **`assign` / `deassign`** (procedural continuous assignment to a variable,
  LRM 10.6.1, Phase 1b): second override layer below force (force > assign >
  ordinary procedural), `assign_site` state + gated re-eval process; `release`
  reverts to an active `assign` (the assign's re-eval process is force_site-aware);
  `deassign` holds; both last-wins. Full ladder byte-identical to iverilog.
- [x] **`force` / `release`** on **nets** (LRM 10.6.2, Phase 1.5): resolver-level
  override — a forced net always resolves through `emit_wire_resolvers`, which injects
  `force_site != 0 ? force_val : <driver fold>` and reverts on `release`. Uniform over
  single-driver, gate-driven, multi-driver/resolved, and undriven (reverts to z) nets;
  continuous RHS tracking, which iverilog does not do (it evaluates the
  net-force RHS once — its own documented limitation).
- [x] **`force` / `release`** on a constant **bit/part-select of a net** (LRM 10.6.2,
  Phase 2): per-force-site `{active, range-wide val, [lo,hi]}`; the net resolver stores
  the driver fold then positionally overlays each active site's bits (z-preserving),
  reverting on release. Non-overlapping sites exact; byte-identical to iverilog.
  Bit/part-select of a **variable** is LRM-illegal (§10.6.2) yet accepted by
  iverilog — rejected loud pending a decision (see below).
- [x] **Shape-agnostic write gate** for force/assign targets (LRM 10.6.2 suppresses the
  *update*, not the statement): one `store_or_gate` helper wraps the store alone, so the
  RHS, the lvalue's selectors and a read-modify-write's read still run (`x++` under force
  yields the forced value and leaves it standing). The write predicate is
  `pca_info_of_lvalue` — `force_info_of(lvalue_root(lhs))` — replacing the old
  force-STATEMENT predicate. Covers selects, packed-struct members, compound
  assignment, `++`/`--`, per-operand gating of a concatenation lvalue, and the
  output-argument copy-back — the last three were silently overriding the force before.
  Intra-assignment timing on a write to a force target (`x = #5 e`) is guarded loud: the
  update commits after the delay, so the gate must ride the deferred record.
- [ ] **`force` / `release`** on a **concatenation** of nets/selects (LRM 10.6.2), and
  the LRM-illegal-but-iverilog-accepted variable bit/part-select force.

### Real numbers

- [x] **`case` on a real selector / label** (LRM 12.5) — 12.5 words matching as a
  bit-for-bit `===` and Table 11-1 bars `===` on a real, so the standard defines the
  comparison only for integral operands; a real carries no x/z, which is the whole
  content of the 4-state rule, so it degenerates to IEEE-754 equality (the same call
  clotho already makes for strings, 6.16.9). A real selector or a real item label
  routes the whole statement to `lower_case_real`, which matches with `FEq` after
  widening both sides to `real` — an integral operand converts first (11.8.1), so
  `case (2.0) 2:` hits; `-0.0` matches `0.0`; a NaN selector matches nothing and
  takes `default`. It keeps 12.5's lazy first-match chain, so a later label's side
  effects never run once an earlier item matched; keeps the `unique` / `unique0` /
  `priority` 12.5.3 overlap and no-match violation checks, built from the same
  per-item predicates; and compares a `case ... inside` range member with `FGe` /
  `FLe` (11.4.13 membership is relational, which Table 11-1 does define for reals).
  The `(* full_case *)` / `(* parallel_case *)` analysis is skipped — it enumerates
  the selector's integral value space. The same matching rule applies to the `case`
  PROPERTY (16.12.16, `eval_case_match`). `casez` / `casex` over a real stay
  rejected, by slang's binder rather than by clotho: it refuses a non-integral
  selector for either keyword, reading 12.5.1's do-not-care positions as bit
  positions. iverilog accepts that spelling and treats it as a plain case, so
  this is a deliberate deviation. Still guarded loud on the real path exactly as
  on the integral one: `case ... inside` tolerance ranges (`+/-`, `+%-`) and `$`
  range bounds.
- [x] **Fractional (real) delays** (LRM 3.14 / 22.7) — a real `#delay` (procedural,
  intra-assign, continuous, gate) rounds to its module's time precision then scales
  to wheel ticks (`FtoI` half-away, LRM 6.12.1). The wheel tick is the design-wide
  GCD of every module's *unit* plus the *precision* of every scope that actually
  contains a real delay -- including inside a subroutine body
  (`scope_has_own_real_delay` / `design_tick_fs` / `global_scope_tick_fs` in
  `src/Lower.cpp`), so an integer-only design keeps the coarse minimum-unit tick and
  its generated IR is byte-identical to before — zero cost. A constant `#1.5` folds
  to a single tick constant; a runtime `#(rvar)` costs one round + one integer
  multiply. Follow-ups (only real-delay designs, and only these shapes, pay): (a)
  many *concurrent* fractional timers whose fine-tick periods all exceed the wheel's
  1024-slot window spill to the O(log n) overflow heap (~2.5x on a 64-timer
  microbench; a single/sparse clock is free — the wheel jumps straight to the lone
  overflow deadline). A hierarchical wheel (below) removes this; a cheaper partial
  win is to set the tick to the GCD of a module's *constant* real-delay values
  instead of its full precision when the module has no runtime real delay. (b)
  negative / NaN / inf real delays are not clamped (parity with the integer path).
- [ ] **A `$unit`-scope subroutine / class has no time unit** — a `timescale`
  directive applies to design elements (LRM 3.14.2.2), so slang leaves
  `CompilationUnitSymbol::timeScale` unset unless `$unit` carries an explicit
  `timeunit` / `timeprecision`, and `apply_time_scale` then falls back to unitless
  (multiplier 1). A function, task or class method declared directly in `$unit`
  therefore reads `$time` in *wheel ticks* and writes `#5` as five ticks — a unit
  that moves with unrelated parts of the design (adding a `#0.5` anywhere shifts
  the tick from 1 ns to 1 ps and with it every `$unit` reading). Measured on
  `` `timescale 1ns/1ps `` + a `$unit` function: clotho reports 3000 where the
  module reports 3; iverilog reports 0, i.e. its own 1 s default. The fix is a
  fixed tool-default timescale for a scope slang hands back no timescale for
  (and folding that unit into the wheel tick, like `global_scope_tick_fs` already
  does for package / `$unit` scopes that DO declare one). Declaring the class or
  subroutine in a package or module — the usual case — is unaffected.
- [x] **Subroutine delays and `$time` use the declaring scope's timescale**
  (LRM 3.14.2.3) — a `#5` or a `$time` read inside a task/function is in the time
  units of the scope that *declares* it, not the one that calls it. Clotho lowers
  each subroutine body once and shares it across call sites, so the caller's
  multiplier left at most one caller correct: a `1ns` package task called from a
  `1us` module delayed 5 us, silently, 1000x off. `function_id` now sets
  `time_mul_` / `prec_over_tick_` from `sub.getParentScope()->getTimeScale()`
  around the body build (`apply_time_scale`), which makes memoization sound —
  the body's scaling depends only on the declaration. Two supporting fixes: the
  wheel tick folds in the unit (and real-delay precision) of any package /
  `$unit` scope that declares a subroutine (`global_scope_tick_fs`), since those
  sit outside the instance tree and a coarser tick cannot represent their delays
  at all; and `scope_has_own_real_delay` now reads subroutine and class-method
  bodies, so a real delay inside a task rounds to its own precision (`#1.5` in a
  `1ns/1ps` task was rounding to 2 ns instead of 1500 ps). Cross-checked against
  iverilog; see tests/task_test.cpp.
- [x] **`real` / `shortreal` types** — storage, arithmetic, conversions, literals,
  parameters, comparisons, `?:`, sensitivity / NBA / continuous / ports / unpacked
  arrays, display formats.
- [x] **`$realtime`** — including inside a plain function, in the units of the
  scope that declares it.
- [x] **Real `$display` formats** (`%f`, `%e`, `%g`) and the 21 real math system
  functions (LRM 20.8.2). A real under `%d`/`%h`/`%x`/`%o`/`%b` also converts to
  its rounded integer (LRM 21.2.1.2 / Table 21-1, rounding per 6.12.1) and formats
  in the requested base. Still guarded loud: `%c` and `%u`/`%z` of a real.

### Observability & debug

- [ ] **VCD sparse / scope-subset selection** (whole-design dump only today).
- [ ] **VCD of a string signal** — blocked on the format, not the engine: IEEE
  1800-2023 21.7.2 defines no `string` var type, and iverilog and Verilator both
  omit string signals from the dump entirely. Belongs in FST, which carries
  variable-length values natively.
- [ ] **VCD of a multidimensional array**.
- [x] **VCD of a signal with no `$var` form is refused, loudly** — a $dumpvars over
  a design declaring a string, a container (queue / dynamic array / associative
  array), an unpacked struct or a class handle used to be silently wrong: the queue
  got `$var reg 1 ! q $end` aliased to its RING POINTER (never changing), the class
  handle dumped a heap address, and the unpacked struct SEGV'd in codegen on its
  null leaf slot type. `check_vcd_scope` now refuses the dump at lowering, naming
  the offending declaration, and the backend keeps a matching throw as the second
  line of defence (a compiler-synthesized signal is skipped instead — it is not
  design state). Walks exactly the set the backend registers, so a package-scope
  variable, which is never dumped, does not trigger it. The follow-up that removes
  the most common cause is dumping an unpacked struct / array as one `$var` per
  leaf.
- [x] **VCD of a named event** — `$var event 1`, and the bare pulse `1<id>` at
  each instant the event triggers (all three forms: `-> e`, `->> e`, `->> #d e`,
  hooked at the single `emit_event_fire` stamp point). The runtime diffs the
  event's last-trigger tick, so repeated triggers in one instant collapse to one
  pulse; a forced dump and the `$dumpoff` checkpoint never invent one, which
  iverilog does at time 0, `$dumpall` and `$dumpon`.
- [x] **`$monitor` / `$monitoron` / `$monitoroff`** (+ radix `$monitorb/o/h`,
  LRM 21.2.2) — one persistent monitor (a new `$monitor` replaces the previous),
  reprinting at the Postponed region of every time step only when an argument
  changed. A synthesized zero-formal body (`build_deferred_body`) renders the arg
  list and calls `rt_monitor_emit`, which prints only when the rendered output
  differs from the last (render-compare change-detection — no per-signal wiring).
  The body's ramp function pointer is registered via `MonitorSet`/`rt_monitor_set`;
  the drain (`emit_active_run`, after the `$strobe` loop, gated by
  `design_uses_monitor_`) indirectly ramps + resumes it each settled instant.
  `$monitoron`/`$monitoroff` toggle (reon reprints). Rejected inside a plain
  function (no record slot for the deferred body), like `$strobe`. Caveat:
  change-detection is on the rendered output, so `$time`/`%t` in the arg list
  reprints every step.
- [ ] **`$timeformat` dynamic-string suffix & zero-arg reset form** (only a
  string-literal suffix accepted).
- [x] **VCD dump** (`$dumpfile`, `$dumpvars`, `$dumpon`/`$dumpoff`, `$dumpall`,
  `$dumpflush`, `$dumplimit`) — whole-design dump, background writer thread,
  activity-driven dirty set, memories, reals, events (docs/VCD.md).
- [x] **`$timeformat` + `%t`**.
- [x] **`$strobe`** (LRM 21.2.2) — `$strobe`/`$strobeb`/`$strobeo`/`$strobeh`;
  a synthesized zero-formal task holds the deferred arg evaluation, ramped (not
  resumed) at the call site and drained once per settled instant in the
  Postponed region, just before the wheel advances. Rejected inside a plain
  function (its deferred body rides the caller's task-call record slot, which only
  a coroutine has) and of an automatic-lifetime argument (its storage cannot
  outlive the call — slang's own binder catches this, matching iverilog).
- [x] **`--emit ir` of a self-referential class type terminates** — the IR type
  printer walked a type structurally with no cycle guard, so a MODULE-SCOPE signal
  whose class type reaches itself made `render_type` recurse until the stack ran
  out: 2.3 MB of `s0: class{nm:string, kids:class{nm:string, kids:class{...`,
  truncated mid-identifier, then SIGSEGV (exit 139) with a mutually-recursive frame
  pair inside `std::__ostream_insert`. A local inside an `initial` did not trigger
  it — only the signal table renders a signal's type — and `clotho run` on the same
  file was always fine, because nothing but the printer expands a class body. It
  fired on ALL of UVM (`uvm_printer.svh:419` declares
  `local uvm_printer_element m_children[$]`, a queue of the enclosing class), which
  cost more than a dump: `--emit ir` is THE tool for diagnosing a backend problem,
  so the one design whose backend problems most need inspecting was the one design
  that could not be inspected. `render_type` now carries the stack of classes whose
  bodies it is inside and renders a re-entered one as `class^N` instead of expanding
  it again — a de Bruijn index counting outward from the class being expanded, so
  `class^1` is the innermost one. Positional rather than named because the pool
  carries no source name for a class: it identifies one by its `Ref` alone (the
  backend's own `clotho.classdesc.<ref>`), and a bare `Ref` in the dump would name
  nothing a reader could resolve. `^` appears in no other type rendering, so an
  elided cycle is never mistaken for a leaf. ONE guard covers every cycle shape,
  because `Kind::Class` is the only cyclic node in the type graph — a value struct
  cannot contain itself (`reserve_struct_fields` exists for classes and nothing
  else), so every cycle must pass through a handle: direct self-reference, mutual
  A<->B, three-hop A->B->C->A, and a cycle through a queue / dynamic-array /
  associative-array element or a struct member all stop at the same test. The
  struct-member form is not reachable from source today — slang refuses any struct
  that transitively reaches itself through a class handle ("'wrap' recursively
  depends on its own definition") — so it is pinned against a pool-built type
  instead, since the type graph models it and the printer must survive it either
  way. Ordinary output is byte-identical by construction — the guard emits only on
  the re-entry branch — and tests/ir_test.cpp pins a non-recursive class's
  rendering so it stays that way. Audited every other structural type walk for the
  same hazard and found none: the backend's `agg_storage_ty` / `class_body_ty` bottom
  out at the class HANDLE slot rather than descending into the body,
  `reaches_class_handle` is already an explicit worklist that never expands a Class node,
  `InternPool::compute_layout` answers Class with the 64-bit handle layout without
  reading its property table, `unsynthesizable_type` and `guard_agg_key_leaves`
  stop at the same boundary, and `%p` deliberately prints a handle's class name at
  every depth for exactly this reason (Lower.cpp:19419-19440). `--stat` emits
  counts and timings only, and no LSP / DAP path renders an IR type today.
- [ ] **`--emit ir` expands a class DAG once per PATH, not once per class** — a
  separate unboundedness in the same walk, pre-existing and untouched by the cycle
  guard above: `render_type` prints a class body structurally at every occurrence,
  so a class reachable by k distinct paths is expanded k times and the dump grows
  exponentially in the class count. Measured on a strictly ACYCLIC chain where
  class i holds handles to i+1, i+2 and i+3 (no cycle, so the guard never fires --
  the 10.7 MB dump contains zero `^`, which is what proves it is the old output
  byte for byte): 10 classes 7 KB, 14 classes 82 KB, 18 classes 933 KB, 22 classes
  10.7 MB, i.e. ~1.8x per class added. UVM has hundreds of densely
  cross-referencing classes, so `--emit ir` on it stays impractical for a reason
  that has nothing to do with the cycle: it terminates, but not at a size anyone
  can read. NOT fixed here because the obvious fix conflicts with a harder
  requirement — eliding a class already expanded ANYWHERE in the current rendering
  (rather than only one still open) makes the walk linear in the class count, but
  it also rewrites ordinary non-recursive output, where a class referenced by two
  properties legitimately prints twice today. The fix that costs no churn is to
  stop inlining class bodies at all and print a class by NAME, which needs a
  class-name side table on `Module`: `Lower` has the name (it prints one for `%p`
  via `class_p_render`) but drops it, and the pool identifies a class by its `Ref`
  alone. Do that when the IR dump is next touched.

### File and string I/O

- [ ] **Non-byte-multiple / part-select `$fgets` target**.
- [ ] **Scanf target width** — targets must be whole-storage and <= 64 bits.
- [x] **Streaming file output** — `$fstrobe` / `$fmonitor` (+ radix). The first
  argument is the destination descriptor; the synthesized deferred body targets
  that fd. `$fstrobe` reuses the `$strobe` Postponed drain; `$fmonitor` reuses the
  `$monitor` per-instant run and change-detection (`build_deferred_body` handles
  both the fd and monitor variants).
- [x] **`$value$plusargs` into a `string` object** (§21.6) — the `%s` form, which
  is "string (no conversion)": the plusarg remainder IS the value, so
  `rt_value_plusargs_str` assigns it to the `rt_string` handle verbatim with no
  width cap, no right-justification and no padding, unlike the packed-vector
  path's `rt_value_plusargs`. Same pattern as `$fgets` / `$ferror` into a string
  (`rt_fgets_str` / `rt_ferror_str`), and the same LRM answers as the integral
  destinations: a match returns nonzero, no match returns 0 and leaves the
  variable untouched, an empty remainder stores the empty string ("either a zero
  or an empty string value"). Load-bearing for UVM: `uvm_root.svh:551`
  (`+UVM_TESTNAME`, into `run_test`'s own `string test_name=""` formal) and
  `uvm_cmdline_report.svh:66` (`+UVM_VERBOSITY`, into a function local) are the
  only two `$value$plusargs` sites in the library, and UVM cannot select a test
  without the first.

  The specifier matrix as it now stands, per §21.6's list (uppercase and leading-0
  forms are accepted spellings of each, folded by `tolower` in the frontend):

  | conv | packed / integral | `string` | `real` |
  |------|-------------------|----------|--------|
  | `%d` | yes | refused | refused |
  | `%o` | yes | refused | refused |
  | `%h`, `%x` | yes | refused | refused |
  | `%b` | yes | refused | refused |
  | `%s` | yes (width-capped, right-justified) | **yes** | refused |
  | `%e`, `%f`, `%g` | refused | refused | refused |

  A part-select destination (`v[7:0]`) is refused for every conversion; a whole
  aggregate or class handle slang itself rejects as an argument type. Truncation
  to a narrower destination and the all-`'bx` illegal-character result are the
  clause's own rules and are unchanged.
- [ ] **`$value$plusargs` `%e`/`%f`/`%g` (real) conversion**, and with it any
  `real` destination at all. Refused in the frontend for every conversion now: an
  integral conversion into a real used to be accepted and write the parsed bit
  pattern into the IEEE-754 mantissa, so `+V=12` with `%d` read back as
  `0.000000` with no diagnostic. Needs the same ASCII->real scan
  `$fscanf`/`$sscanf`'s own `%f`/`%e`/`%g` needs, plus a real-slot write-back
  (`rt_value_plusargs`'s `rt_scan_arg` is must/may planes).
- [ ] **`$value$plusargs` numeric conversion into a `string` object** — `%d` and
  friends with a `string` destination. §21.6 stores the CONVERTED value, so this
  is 6.16's integral-to-string assignment of it (a one-byte string holding a
  character code), which is a separate question from the `%s` "no conversion"
  form above and is what real code uses. Refused in the backend, where the
  destination's storage kind is known.
- [ ] **`$fscanf` / `$sscanf` write-back into a `string` object**. NOT the change
  `$value$plusargs` above was: that path has one destination and got its own
  runtime entry point, while the scan engine interleaves several targets through
  one shared `rt_scan_arg` array, so the descriptor needs a string-handle variant
  (plus its alloc site), the engine's store step a branch on it, and `%5s` into
  an unbounded string its own answer. Real code reaches it: `uvm_root.svh:916` is
  `$sscanf(timeout, "%d,%s", timeout_int, override_spec)` with a `string`
  `override_spec`.
- [ ] **`$value$plusargs` runtime (non-literal) `str` argument**. Shared with
  `$test$plusargs`, whose query string is under the same restriction; §21.6's own
  `ieee1364_example` exercises it through a `pstring` variable. Both refuse in
  the frontend today. One feature, not two: the prefix has to be materialized and
  handed to the runtime as bytes at the call rather than baked into a global.
- [~] **Persist the Driver / SourceManager across runs** for buffer + parse
  caching (SM/loader/trees are public so a cache layer can sit above them).
  Within-process retention has landed: `lower_*` returns a `SourceContext`
  (`src/SourceContext.h`) owning the Driver / SourceManager / Compilation, so an
  `ir::SourceLoc` stays resolvable after lowering returns. Reuse ACROSS runs
  (parse caching) is still outstanding and needs the content-addressed cache.
- [x] **Run-start reset of process-global runtime state** — `rt_run_init`
  (`src/runtime/Runtime.h`, `src/runtime/Runtime.cpp`), emitted unconditionally at
  the top of the design's main beside `rt_gc_init` and subsuming the old
  `rt_event_pool_init`. A host that runs more than one design per process — which
  is every `clotho_test` process, since `ir::run_jit` builds a fresh LLJIT per
  design — used to carry state across designs: an unclosed `$fopen` kept its MCD
  channel until the 30-wide space ran out, the unseeded `$random` stream was
  deterministic only for the first design, a detached `->> @(x) e` record survived
  `$finish` keyed by a `trig_id` the next design reused, and the PCB and event
  pools grew without bound. `rt_run_init` returns the PCB pool, the event pool,
  the detach table, the NBA-array and intra-assignment queues, the file tables,
  the per-site diagnostic suppression maps and the `$random` seed to power-on.
  Deliberately out of scope: GC state (`rt_gc_init` owns it and preserves
  `g.stats`), VCD, and host-armed configuration set before the module runs.
  `sim_teardown` stays as it is — it drops what would dangle into dead JIT code
  the moment a run ends, but it cannot be the seam, because it runs before the
  emitted signal deinit and `rt_gc_shutdown` and is skipped entirely by the fatal
  `std::exit` paths. Still owed: the wait queues, the observed/deferred queues and
  the string pools (docs/design/cooperative-finish.md §Why).
- [ ] **`param-overrides` (`-G`), config/library-map (`--libmap`)** not yet surfaced.
- [x] **File output** (§21.3) — `$fopen` (MCD + FD), `$fclose`, `$fdisplay`/`$fwrite`,
  `$fflush`.
- [x] **File input / positioning** (§21.3.4–5) — `$fgetc`, `$ungetc`, `$fgets`,
  `$feof`, `$ftell`, `$fseek`, `$rewind`. `$fgets` into a `string` object
  reads the whole line with no fixed-width cap (`rt_fgets_str`); a
  packed-vector destination keeps the width-capped right-justified path.
- [x] **String formatting** (§21.3.3, §21.3.6) — `$sformat`, `$swrite`,
  `$ferror`. `$ferror` into a `string` object writes the message via
  `rt_ferror_str`, same error-code semantics as the packed-vector path.
- [x] **A `$sformat` / `$sformatf` format that is not a string literal**
  (§21.3.3). The clause makes these two the one exception in the family: "The
  format arguments to these tasks must be string literals, i.e., they cannot be
  expressions of string data type. The only exception is `$sformat`, whose second
  argument can be an expression of string data type." Clotho used to refuse every
  non-literal format with a TODO claiming a runtime format "needs dynamic string
  support, which Clotho lacks". **That premise was stale**: `rt_sformat` /
  `rt_sformat_str` have always parsed the format at run time, and `rt_arg` has
  always been a tagged descriptor carrying width, signedness and the string / real
  flags. Only the frontend was baking the format into a constant. Two paths now:

  **A CONSTANT format folds and costs nothing.** The blocker that motivated this
  is Accellera UVM's `uvm_phase.svh:1963`, whose jump-failure message is a
  four-piece concatenation of string literals — not a literal, but a compile-time
  constant, as are a `localparam string` and a stringified macro.
  `const_format_text` evaluates the format expression through slang's
  `ASTContext` / `EvalContext` and hands the text to the EXISTING compile-time
  path (`build_format`'s new `lead_text`), so specifier validation, `%m` / `%l`
  splicing and `%p` expansion all come for free. A concatenation has type
  `bit[N]` (LRM 5.9), so the constant arrives as an integral whose bytes are the
  text; `ConstantValue::convertToStr()` is slang's own LRM 6.16 conversion (8-bit
  chunks most significant first, a zero chunk dropped, a short leading chunk when
  `N` is not a multiple of 8) and is the same rule `rt_string_from_packed` applies
  at run time, so a folded format and a computed one that hold the same bits
  agree. Verified free: `clotho build --emit ir` for `$sformat(s, "a=%0d b=%0d",
  1, 2)` and for the equivalent concatenation are IDENTICAL modulo the file name
  in the source locations, and both emit the same `rt_sformat_str` call with the
  baked format global and a null dynamic context.

  **A genuinely RUNTIME format pays only for itself.** The format's `rt_string`
  hands the renderer its own NUL-terminated character buffer (`rt_string_data`,
  the guarantee the DPI `string` argument path already relies on) — no copy, read
  only within the call, never retained; an empty string is a pointer to a NUL and
  renders nothing. Text past an embedded NUL is unreachable, since a `const char*`
  format cannot express one. Each value argument is marshalled raw into an
  `rt_arg`, because which specifier it will meet is not knowable at compile time,
  and the renderer then does everything `build_format` used to do in the IR: the
  LRM 6.12.1 integral↔real reconciliation, the `%t` tick scaling, and `%p`. The
  one call-site fact it cannot derive is the calling module's ticks-per-time-unit,
  which rides a new `rt_fmt_dyn` (null on the constant path, so that path takes no
  extra branch). `%p` of a singular value gained a runtime case
  (`render_p_scalar`), which mirrors `expand_p_scalar` deliberately and says so on
  both sides: minimal decimal at the operand's own signedness, `%g` for a real,
  quotes around a string. The IR carries the format as a real SSA operand
  (`SformatInfo` / `SformatfInfo::fmt_value`, reported through
  `ExtraOperandTail`), so def-use, liveness and DCE see it.

  **Refused loudly, never approximated.** An aggregate or container argument
  against a runtime format is a COMPILE error naming the argument position and the
  type kind ("$sformat argument 4 is an unpacked struct"): `rt_arg` has no image of
  an aggregate, and with a format unknown until run time the frontend cannot tell
  whether the argument will meet a `%p` and so cannot expand one. At run time,
  every rejection `check_specifier` makes at compile time is made again with the
  text in hand — an undefined conversion, `%v`, more specifiers than arguments, a
  string under a numeric conversion, `%c` / `%u` / `%z` of a real — each a located
  fatal naming the specifier, with a source caret and the simulation time, never
  wrong text.

  **`$display` / `$write` are deliberately unchanged and clause-backed.** Their
  format must be a literal, so `$display({"a=%0d ", "b=%0d"}, 1, 2)` correctly
  prints the concatenation as one integral value and `$display(f, 7)` prints `f`
  under `%s` followed by a bare `7`. Verilator 5.050 agrees byte for byte;
  iverilog is lenient that treats both as formats, and is wrong on the
  clause. Pinned by a regression test in `display_test.cpp` so nothing added to
  the shared `build_format` can leak `$sformat`'s exception into the family.
  iverilog 13 and Verilator 5.050 do agree with each other, and with Clotho, on
  every `$sformat` shape above.
- [ ] **`%m` / `%l` in a runtime `$sformat` format** — they name the invoking
  scope, which only the compiler knows; `append_format_literal` splices the text
  in for a constant format and the renderer never learns either name. Would need
  the hierarchical name and the `library.cell` to ride the IR record (two more
  byte runs on `SformatInfo` / `SformatfInfo`) and reach the renderer through
  `rt_fmt_dyn`.
- [ ] **An aggregate or container argument to a runtime `$sformat` format** —
  refused today (above). Clotho already has runtime container descriptors for
  `%p` of a queue / dynamic array (`ir::Inst::Tag::ContainerFmtP` →
  `rt_container_fmt_p`), so a future version could pass a type descriptor
  alongside each `rt_arg` and let the renderer walk the value once it turns out
  to meet a `%p`.
- [ ] **An operand wider than 64 bits under `%e` / `%f` / `%g` from a runtime
  format** — the constant path handles it (`ItoF` lowers to `SIToFP` on the full
  `iW`); accumulating the planes in the renderer would round at every word
  boundary instead of once, so it refuses rather than being quietly off.
- [ ] **An empty argument (adjacent commas) against a runtime `$sformat` format**
  — an empty argument contributes format TEXT (LRM 21.2.1), and there is no format
  text at compile time to contribute to. Would need a "renders as a space,
  consumes no specifier" marker in `rt_arg`.
- [x] **Format scanning** (§21.3.4) — `$fscanf`, `$sscanf`.
- [x] **Binary read** (§21.3.4) — `$fread` into a scalar or unpacked-array memory.
- [x] **Command-line plusargs** (§21.6) — `$test$plusargs` / `$value$plusargs`,
  the latter into a packed vector or (for `%s`) a `string` object.
- [x] **slang `driver::Driver` frontend** — `-D`/`+define+`, `-I`/`+incdir+`, `-f`
  command files, `-y`/`-v` libraries, `--top`, `--timescale`, `--std`; single-unit
  compile. `--loose-timescale` accepts a design mixing elements that declare a
  time scale with elements that do not (LRM 3.14.2.2), giving the un-annotated
  ones `--timescale`'s value or `1ns/1ns`; refused by default, since a missing
  unit otherwise makes a `#5` mean whatever the design's finest unit happens to
  be.

### Structural primitives

- [x] **Tristate gates** `bufif0`/`bufif1`/`notif0`/`notif1` (LRM 28.6, Table
  28-5) — lowered as continuous-assign drivers that emit Z when disabled (X when
  the control is x/z; the weak L/H strengths collapse to X). Ride
  the existing driver-resolution path, so multiple tri-state drivers on one net
  resolve. `gate_tristate_value` in `Lower.cpp`.
- [x] **Gate delays** — single `#d` inertial delay landed (see Continuous /
  structural timing above). Remaining: rise/fall `#(r,f)` triples and the third
  (turn-off) delay of the tri-state gates (LRM 28.6).
- [x] **Drive strengths on gates** — the gate output's `(strength0, strength1)`
  threads to its driver slot and competes in the strength fold (see Drive
  strengths below); no charge / strength-reduction-through-gates.
- [x] **Gate outputs on 2-state targets** — the coercion policy is LRM 6.11.2's
  automatic 4-state -> 2-state conversion (x and z become 0), applied to the gate's
  1-bit fold by `gate_output_value` in `Lower.cpp` so a gate matches the equivalent
  continuous assign, whose conversion slang inserts on the RHS. Reaches a `bit`
  variable (LRM 10.3.2 allows one primitive output to drive a variable) and a
  2-state member of a 4-state packed net; a 2-state NET does not exist (LRM 6.7.1,
  enforced by slang's `isValidForNet`), so the question never had a net form. A
  SINGLE gate driving a packed-net member is still refused loud (`unsupported gate
  output terminal`: `gate_select_base` wants a `NamedValue` base, so `s.lo[0]` has
  no static (signal, bit) terminal) — only the resolved, multi-driver spelling of
  that shape works.
- [ ] **Concat / part-select gate output lvalues**.
- [ ] **Bit-select gate outputs** (one bit of a packed wire) — pay a whole-word
  read-modify-write and drop out of thunk/fusion eligibility; per-bit
  packed-signal fusion is the follow-up.
- [x] **MOS switches** — `nmos`/`pmos`/`rnmos`/`rpmos` (LRM 28.7, Table 28-6) and
  `cmos`/`rcmos` (LRM 28.9). Lowered on the tri-state gate path with a pass-device
  reading of a floating data input: a z data bit propagates as z instead of
  reading as x, so the whole DATA=z row is z. A `cmos` becomes the two drivers
  LRM 28.9 defines it as and folds through net resolution. No strength reduction
  through the r-variants, so `rnmos` ≡ `nmos` (LRM 28.14).
  `mos_switch_of` / `lower_cmos_switch` in `Lower.cpp`.
- [ ] **Bidirectional pass switches** — `tran`/`rtran` and `tranif0`/`tranif1`/
  `rtranif0`/`rtranif1` (LRM 28.8). Conduction is a net merge, not a driver: the
  unconditional pair could alias both terminals onto one signal, but the
  conditional ones need each side to see the other's resolved value excluding its
  own contribution, which the one-pass driver-slot fold cannot express.
- [ ] **UDPs** — combinational and sequential user-defined primitives.
- [x] **Combinational gate primitives** — `and`/`or`/`nand`/`nor`/`xor`/`xnor`/
  `buf`/`not`: n-input/output forms, instance arrays, x/z truth tables; lowered to
  self-starting continuous-assign comb processes.

### Net types, strengths, resolution

NOTE: anything here that can drive z (tri nets, tristate primitives, z-defaulted
unconnected ports) must seed the z-elision analysis (`compute_may_z`,
`ir/ZElision.cpp`) when implemented.

- [~] **Resolved / strength net kinds** — `wand`/`triand` (wired-AND),
  `wor`/`trior` (wired-OR), `tri0`/`tri1` (resistive Pull to 0/1), and
  `supply0`/`supply1` (Supply-strength constant 0/1 that beats a Strong driver;
  a Supply pull always folds as a strength driver, `net_pull_strength`) modeled
  (multi-driver resolution below). Remaining: `trireg` (strength / charge).
- [~] **Multiple-driver resolution** — default resolution (LRM 6.6) done for
  drivers of a **packed** net sharing one instance scope: whole-net drivers stage
  their value, constant packed bit/part-select drivers stage it with a Z
  background so each bit resolves only across its own drivers; a resolver process
  folds every driver's slot (`build_driver_slot_value` / `emit_wire_resolvers` in
  Lower.cpp). Reducers: `wire`/`tri` (`WireRes`, X on conflict), `wand`/`triand`
  (`WandRes`, wired-AND), `wor`/`trior` (`WorRes`, wired-OR) — all z ⊑ {0,1} ⊑ x
  lattice folds; `tri0`/`tri1` fold as wire, then `NetPull` fills undriven bits
  with 0/1 (LRM 6.6.4), always resolved (a driverless net still pulls). A slot is
  cut from the net's own type, hence always 4-state and always able to hold the Z
  background (LRM 6.7.1 admits no 2-state net; a 2-state MEMBER of a 4-state packed
  net converts per LRM 6.11.2 and resolves normally, so what used to be a guard is
  now the invariant `internal: resolved net storage is not 4-state`). Gate-primitive
  outputs stage into slots like any other driver. Remaining, all still loud-failing:
  unpacked-array / unpacked-struct nets (per-element slots + per-element resolver),
  concatenation-operand drivers (split into per-net stores), and cross-scope drivers
  (e.g. a shared interface net — the slots would span scopes). Non-overlapping
  constant bit/part-select drivers stay on the single-driver fast path.
- [~] **Drive strengths** — `(strong0, weak1)` etc. on continuous assigns, net
  declarations, and gates lower and resolve. Each driver slot carries a
  compile-time packed `(strength0, strength1)`; a net with any non-default
  strength folds through `StrengthRes` (level-by-level per-side-max, `Lower.cpp`
  `emit_wire_resolvers` / backend `lower_strength_res`) instead of `WireRes`.
  A `tri0`/`tri1` pull is injected as a synthetic Pull-strength driver so a
  Weak driver correctly loses to it. Symmetric strengths, open-drain /
  open-source (highz on one side), and pull competition all resolve exactly.
  `pullup`/`pulldown` (LRM 28.5) lower as a lone permanent Pull-strength driver
  slot of 1 / 0 (`lower_pull_source`), forcing the net resolved so a stronger
  driver wins and an all-off net holds the pull value.
  Remaining, all deferred: **strength ranges** (an x/z value on an
  *asymmetric*-strength driver spans two strengths — resolved to a definite
  value where the range model would yield x), drive strengths on
  `wand`/`wor` nets (loud-failing), `pullup`/`pulldown` on a bit/part-select
  target, strength reduction through gates, and `trireg` charge.
- [x] **Single-driver, z-defaulting net kinds** — `wire`, `tri`, `uwire`
  (single-driver `uwire`, LRM 6.6.7, complete).

### Hierarchy reach

- [ ] **Cross-top hierarchical refs** (`top2.sig` from `top1`) — each top has its
  own Lowerer / `sigmap_`.
- [ ] **Path into an instance nested in a generate block** (`g.u.sig`) or an
  instance-array element (`arr[i].sig`) — reported as a slang elaboration error
  before lowering. (Signals declared directly in a generate-loop block `g[i].sig`
  work.)
- [~] **`inout` / `ref` ports** (LRM 23.3.3) — shared-storage collapse: the child
  port's internal net/variable aliases the parent connection's signal
  (`build_inout_aliases`, no copy process), so both sides read/write one net and
  net resolution folds any drivers. Whole-signal connection with a single-scope
  effective driver works (either direction). Deferred, loud: a bit-select /
  part-select / concat connection (needs sub-signal aliasing the whole-signal
  collapse cannot express), and a bidirectional bus with drivers in two instance
  scopes (the general cross-scope multi-driver limitation below, not
  inout-specific).
- [x] **Cross-hierarchy references** (`top.u1.sig`, LRM 23.6) — reads and writes
  any direction, bit/part-select, `@(edge)`, blocking and nonblocking assign.
- [x] **Unconnected ports** — unconnected input reads `z` (LRM 6.6).

### Timing checks (basic)

- [ ] **`specify` blocks** — module path delays (`(a => b) = 3;`) and the common
  timing checks `$setup`, `$hold`, `$setuphold`, `$width`, `$period`. (Full set
  and SDF annotation are M5.)

---

## M3 — Common SystemVerilog

### Data types

- [ ] **2-state call-ABI micro-opt** — the function/task ABI still passes a 2-state
  formal as the packed (mm, z) pair (constant-0 z). Size opt, not a correctness gap.
- [x] **NBA to a string** (`s <= t`, LRM 10.4.2 / 4.9.4) — a fourth range of the
  shared `rt_nba_arr_*` record space holding `{ ptr dest, rt_string staged }`: the
  Active region `rt_string_init`s the staged handle and `rt_string_assign`s the RHS
  into it (sample-now with owned storage), the NBA region `rt_string_move`s it into
  the destination (release-and-take, so a target written twice keeps the last write
  and frees the superseded buffer once). Reuses `StoreNb` — no new IR tag — because
  `HandleCopyNb`'s payload is a source handle LVALUE and a string rvalue is often a
  body temp with none. Since the record carries the destination POINTER, a string
  member of an unpacked struct and an element of a fixed unpacked array come free,
  and with them a whole-aggregate NBA carrying a string member (decomposed per
  leaf). No wake and no VCD touch: a string is refused from sensitivity lists and
  has no `$var` form. Two traps documented in
  docs/design/queue-nba-and-nonpod.md §3: an empty `rt_string` is NOT all-zero (so
  `init`, never `memset`), and the classify branch must precede the array /
  aggregate ones. Still refused: `s[i] <= c` (a masked CHARACTER record, a
  different shape), `s <= #5 t` (the separate intra-assignment pool), and a string
  class property (signal-keyed staging). Fixed in passing: an UNQUALIFIED class
  property NBA inside a method (`p <= v`) reached the backend as an unaddressable
  root and failed with no source location, for every type — now a located refusal
  in `check_subroutine_nba_target`.
- [ ] **NBA to a class property** (`c.x <= v`) — legal SV, refused loudly. Every
  NBA staging buffer (the scalar shadow slot, the aggregate image, the container
  handle record) is indexed by `Signal`; an object body is a heap block with no
  signal, so a property reaches none of them. Needs staging keyed by destination
  ADDRESS rather than by signal, with last-write-wins resolved by comparing
  addresses at commit. See docs/design/classes.md §8.
- [ ] **Intra-assignment delay on a container NBA** (`q <= #2 r`) — refused loudly.
  The intra-assignment path stages an integral or real value, an unpacked-array
  element or a struct field; a container handle needs the HandleCopyNb record to
  survive to the delayed commit. See docs/design/intra-assignment-timing.md.
- [ ] **Non-constant string replication count**.
- [ ] **String ports** — including a string reached through a struct member or a
  container element, which the old array-only walk let through to the plane-copy
  path (now guarded).
- [ ] **String event control / sensitivity**.
- [x] **`case` over strings** (LRM 12.5 / 6.16.9) — **an LRM ambiguity resolved
  deliberately, not a sanctioned construct.** 12.5 words matching as a bit-for-bit
  `===` over operands zero-extended to the widest, and Table 11-1 bars `===` on a
  string exactly as it does on a real, so the standard defines the comparison only
  for integral operands; 6.16.9 defines `==` over string content and no `===`. But
  12.5 puts no type restriction on the case expression and nothing prohibits the
  spelling, so it is read as content equality — the only well-defined reading, and
  the one `===` degenerates to on an operand with no x/z, which is the same call the
  real path makes. The zero-extension half is dropped: there is no width to extend
  to, and padding one side would contradict 6.16.9. A literal's NUL bytes are
  already dropped where it becomes a string (6.16), so `"\000A"` and `"A"` are one
  value and match.
  A string selector routes the statement to `lower_case_string`, which matches with
  the existing string `Eq`. It keeps 12.5's lazy first-match chain (a later label's
  side effects never run once an earlier item matched), keeps the `unique` /
  `unique0` / `priority` 12.5.3 overlap and no-match checks off the same per-item
  predicates, and handles multi-label items and `case ... inside` over a label SET.
  `(* full_case *)` / `(* parallel_case *)` are skipped — they enumerate the
  selector's integral value space. Guarded loud: a `case ... inside` string RANGE
  bound (11.4.13 over 6.16.9's relational operators — implementable, unreached).
  `casez` / `casex` over a string and a mixed string/integral label list are both
  refused by slang's binder before clotho sees them; the clotho-side guards are
  kept and labelled unreachable.
  **No oracle**: iverilog 13 cannot compile a string `case` at all (it aborts
  in `draw_eval_vec4`), so this is pinned to the LRM text rather than to a
  simulator. Verilator 5.050 accepts it and agrees on content equality, which is
  corroboration, not authority. Motivated by UVM: `uvm_string_to_verbosity` /
  `uvm_string_to_action` dispatch `+UVM_VERBOSITY=` this way, nine such statements
  in the reachable part of uvm-core.
- [x] **`?:` over strings** — the arms are borrowed `rt_string` handles, so the
  eager path selects a pointer (`lower_string_mux`) and the lazy path merges the
  two arms with a pointer PHI; no string temp and no deep copy. An ambiguous
  condition yields the common value when the arms are equal and `""` otherwise,
  matching what `real` does with 0.0 (LRM 11.4.11 defines the bitwise merge for
  integral results only). See docs/design/x-semantics.md §1.4.
- [x] **Strings inside structs / queues / dynamic arrays** — recursive aggregate
  string leaves (init/deinit/copy walks) and rt_elem_ops string elements. A
  string as an associative-array VALUE element is now supported (a[k] routes the
  string handle through the OOB sink); string KEYS are supported.
- [ ] **enum non-constant `next`/`prev` step**.
- [x] **Nonblocking whole-object copy / equality over an unpacked-array member**
  — NBA of a struct with an array member decomposes per leaf (deferred queue);
  `==` / `!=` recurse into array members (per-element AggEq).
- [x] **Aggregate task arguments, and `output`/`inout` function arguments** —
  one storage pointer per aggregate formal in every by-value direction
  (`declare_task` mirrors `declare_function`); the callee copies in at its
  prologue (a task: at the body's start block) and copies an `output` / `inout`
  formal back at return (a task: in the completion epilogue, after any delay).
  Covered for nesting, recursion, fork concurrency, a class method task, and
  struct-with-array / array-of-struct / 2-D / slice actuals.
- [x] **`ref` aggregate formal** — subsumed by the uniform ref above; an aggregate
  ref aliases its image pointer with no copy either way.
- [ ] **Non-lvalue aggregate actual** (`'{...}` pattern / call result) — needs a
  materialized caller temp; guarded loud today for functions and tasks alike.
- [ ] **String-containing aggregate by value** through a function.
- [x] **Non-lvalue aggregate RETURN, and an aggregate call result in a read
  context** (LRM 13.4) — `return g();`, `return '{1,2};`,
  `return q.pop_back();`, plus `$display("%p", g())`, `g().a` and `ga()[1]`, for
  an unpacked struct and a fixed unpacked array alike. Two separate refusals sat
  behind this. The return path guarded its operand with `is_struct_lvalue_shape`
  (NamedValue / HierarchicalValue / MemberAccess / ElementSelect) and failed with
  "aggregate return of a non-lvalue expression is not supported"; the read
  contexts hit `lower_lvalue`'s `case Call:`, which handled only a
  HANDLE-returning call and fell through to "unsupported assignment target
  kind=Call" — an assignment-target message for a pure read. The root cause was
  the same in both: an aggregate call result HAS anchored storage (the callee
  writes through the caller-side out-param temp the call allocates, and the
  call's value IS that temp's lvalue), but nothing said so, so every consumer
  that structurally needs an lvalue refused it. Teaching the `Call` arm that one
  fact fixes the read contexts and the returned-call form together, since
  `lower_return` routes through `lower_lvalue`.
  The non-call rvalues take 13.4 literally instead: the clause defines
  `return expr` as assigning `expr` to the function's implicit return variable
  and returning it, so a `'{...}` pattern is built by the ordinary
  `emit_agg_store` path directly INTO that variable, whose frame storage already
  existed for the bare-`return;` path. **Deliberately not** staged through
  `array_rvalue_lvalue` or a struct-flavoured sibling of it: both allocate a
  fresh `make_canonical_agg_temp`, and `return` already owns its destination, so
  reusing the layer below both (`emit_agg_store`) costs one fewer frame local and
  no extra copy. `return g();` therefore performs exactly two memcpys, one per
  ABI boundary crossed — the callee's copy-out into its out-param, then the
  caller's copy of that temp into its own — with no temp→temp staging, and
  `return '{...}` allocates no temp at all (verified in the emitted LLVM IR).
  Deciding it this way is also what keeps the LRM's assignment-equivalence
  honest: `return` accepts exactly what an assignment accepts, so when the
  aggregate `?:` landed (Operators & expressions above) `return c ? x : y;` began
  working through the assignment path with no case of its own in
  `agg_return_operand` — the operand falls through to `emit_agg_store`, which
  branches and builds the selected arm straight into the return variable.
  The `%p` case had a standing hazard — a `%p` operand was once lowered
  twice, running a call operand twice (see String formatting below) — so the
  call is lowered once for the whole leaf walk and a counter test pins the
  operand at exactly one evaluation. Motivated by Accellera UVM's
  `uvm_queue.svh:189`,
  `virtual function T pop_back(); return queue.pop_back(); endfunction`, which is
  covered end to end with a struct `T` that owns a string.
  Remaining gaps:
  - [x] **Aggregate ternary rvalue** (`o = c ? x : y;`, and so
    `return c ? x : y;`) — done; see **Aggregate `?:`** under Operators &
    expressions.
  - [ ] **Return of an unpacked-array slice** (`return x[0:1];`) — refused loud
    with its own message. Not a return-path limit: inside a subroutine both the
    slice source and the return variable are canonical aggregate frame storage,
    and the backend has no aggregate-element slice, so a plain `o = x[0:1];`
    between two frame locals already fails the same way. Lowering it here would
    only move the failure into the backend.
- [x] **Whole aggregate in a general rvalue context** — `$display %p`
  (LRM 21.2.1.6, lower-time `'{...}` expansion; a tagged union and a
  queue / dynamic array render at runtime instead) and streaming-concat operand
  (LRM 11.4.14, leaf bit-serialization). A whole-array by-value read outside
  these contexts routes through the function-ABI copy-in, not a flat load.
- [ ] **inout / ref aggregate PORTS** (the subroutine side is done). A port copy is
  a continuous assignment, so it needs change detection over aggregate storage.
- [ ] **Aggregate port driven by a `'{...}` pattern / call**.
- [x] **Event control on a member / select / expression** (`@(posedge x.f)`,
  `@(arr[i])`, part-selects). `event_signal` synthesizes a temp net continuously
  assigned from the expression and subscribes to it (bit 0 = the expression's
  LSB, exact for edge; whole word for level). One delta of latency vs. a direct
  signal. Integral operands only; real / aggregate operands rejected loud.
- [x] **Unions** (LRM 7.3) — packed and unpacked. Both lower to a flat plane
  value (`Kind::Struct`, is_packed, is_union): a packed union is the widest
  member's width (all equal, 7.3.1); an unpacked union is sized to the widest
  member, every member overlaying the LSB (the natural, verilator-matching
  overlay). Member access reuses the packed part-select path
  with no backend changes; whole-union assignment, NBA, VCD, and change detection
  all ride the normal plane-signal path. Unpacked-union members are restricted to
  integral types (real / string / unpacked aggregate / handle members gated loud —
  no flat-bit overlay).
- [x] **Tagged unions** (LRM 7.3.2) — both packed and unpacked lower to the same
  flat `{tag, payload}` bit-vector: the tag in the high `tagBits` bits (packed:
  slang's `tagBits`/width; unpacked: synthesized `ceil(log2 n)`), the member
  payload overlaying the LSB. The `tagged Member(v)` setter
  (`TaggedUnionExpression`) is the only thing that moves the tag; it concatenates
  tag / gap / payload, and the gap between a narrow payload and the tag takes the
  power-on default (x for 4-state, 0 for 2-state), matching slang const-eval so
  runtime and folded-parameter forms agree. **Every member access — read and
  write — is tag-checked at run time** (LRM 11.9: an attempt to read or assign a
  value inconsistent with the tag "results in a run-time error"), emitted from the
  single `lower_member_lvalue` funnel as a `TaggedCheck` node — one 4-state
  compare plus a branch weighted toward the fall-through, into the
  `rt_tagged_access` located fatal. A member write requires the tag to *already*
  select that member and never retags (LRM 7.3.2: "The tag and value can only be
  updated together consistently using a statically type-checked tagged union
  expression"); an x/z tag selects no member and reports. The check is elided for a
  plain union and for a single-member tagged union, and is never reached from a
  pattern-matched arm. `%p` prints `'{tag:value}` with the payload at the active
  member's own width and signedness (LRM 21.2.1.6, runtime `TaggedFmtP` /
  `rt_tagged_format_p`);
  `%b` stays the raw N-bit value, gap bits included. Member read, whole-union
  assign / `==` / VCD ride the flat path. Members restricted to integral (+ void).
  Pattern matching (`case matches` / `if matches` / `?: matches`, LRM 12.6) is
  implemented — see Pattern matching under M5 Language corners. iverilog /
  verilator have no tagged-union support, so neither can cross-check this
  shape; the 11.9 run-time check is enforced because 11.9 requires
  it. See docs/design/tagged-unions.md.
  Remaining gaps:
  - [ ] Non-integral member of an **unpacked** tagged union (`real` / `string` /
    unpacked aggregate / class payload) — the Phase 3 gap, gated loud.
  - [ ] `force`/`release`, DPI, and queue / dynamic-array / associative-array
    element positions loud-fail for the **unpacked** tagged-union form only (all
    key on slang's `isIntegral()`); the packed form works in each.
  - [ ] `%p` of a queue / dynamic array of tagged unions — rejected loud; the
    runtime container descriptor carries no member-name table.
  - [ ] VCD dumps a tagged union as one flat anonymous `$var`. LRM 21.7 has no
    tagged-union notion so this is defensible, but no variant name is recoverable
    from the dump.
  - [ ] Latent: an all-`void` unpacked tagged union computes
    `max_width + tag_bits - 1` with both terms zero, underflowing. Currently
    masked — the width lands at 1 and VCD shows `$var reg 1` — so there is no
    observable misbehaviour. Recorded as latent, not as a bug.
- [x] **Unpacked-array full slicing / whole-array assignment** beyond the memcpy
  path (ArrSlice IR node for constant bounds, position-wise element expansion for
  an indexed part-select; per-element copy/eq/store; see Memories & arrays).
- [x] **Nonblocking `'{...}` to an unpacked array**.
- [x] **Associative-array `'{...}` literals** (LRM 7.9.11) — `'{key:value, …}`
  and the `default:` value a read of an absent key yields, at a declaration or as
  an assignment, on a module variable / procedural local / frame storage. The
  default is per-array runtime state (an `AssocMap` slot, not an entry): it is
  invisible to `num`/`exists`/`first`/`next`, survives `a.delete()`, travels with a
  whole-array copy, and does NOT seed a created entry. Before
  this the initializer was **silently dropped** (a zero-width store into the
  handle slot), which is what made it a bug rather than a gap. Guarded loud: a
  nonblocking literal, and an aggregate value element (needs a per-leaf store per
  entry). Dynamic-array / queue `'{...}` literals already worked.
- [x] **Non-POD dynamic-array / queue elements** (LRM 7.5 / 7.10) — every element
  kind but an associative array or an event: integral of any width, real, `string`,
  **class handle**, **unpacked struct / union or fixed unpacked array** (with
  string / container leaves at any depth), and a **nested queue / dynamic array**.
  Both containers share one gate (`guard_container_element`) because they share
  DynElem and the `rt_elem_ops` descriptor.
  * A class-handle element is a plain 64-bit POD word: objects are never freed
    (classes.md §7), so it needs no lifecycle, `null` is the all-zero Table 7-1
    default, and `==` is the identity compare a word compare gives.
  * An aggregate element slot holds the recursive storage image, and
    `dyn_agg_elem_lvalue` hands back a HEAP-ANCHORED aggregate lvalue — base
    advanced to the physical slot, `dyn_inrange` carried — so `q[i].f`, `q[i][j]`,
    whole-element copies, `foreach` and equality all reuse the ordinary aggregate
    machinery with no container-specific code. Whole elements cross the
    `rt_queue_*` ABI by pointer (no ABI change: `elem` / `out` were already
    `void*`); a `'{...}` actual stages into a canonical aggregate temp.
  * A managed leaf (string / nested container) inside an aggregate element rides a
    new **kind-4 `rt_elem_ops` leaf table**: the backend emits an offset-sorted
    `{offset, ops}` array beside the descriptor, and the runtime deep-copies each
    leaf and raw-copies the POD gaps between them (that walk is why the order is a
    contract). The runtime side already existed and was dead code.
  * A nested container element is a handle slot chaining through
    `rt_elem_ops::inner`. Out of range it redirects to a per-element-type sink
    handle, emptied at every access, so `q[99].size()` is 0 and `q[99].push_back(x)`
    is dropped (LRM 7.4.5) instead of addressing the heap past the live length —
    the same shape as the string-element sink.
  * A pop RELOCATES the element into a body-lifetime buffer, so the buffer owns
    whatever heap storage it carries: the pop site releases the previous contents
    (a pop in a loop) and every body exit releases the last (`build_agg_elem_temps`
    + `deinit_body_strings`). Verified leak-free and double-free-free under
    ASan/LSan.
  * Fixed en route: `str_slot_ty` was `{ [32 x i8] }` (align 1) while `rt_string`
    is `alignas(8)`, so a string leaf inside ANY aggregate (a plain struct signal
    included) was accessed misaligned — UB that UBSan reported 13 times on a
    two-member struct. The mirror now leads with an i64.
  An associative-array element works too, through the `rt_elem_assoc` (kind 5)
  descriptor. Still guarded loud: an event element, which needs one event OBJECT per
  element; the object slot array is sized at compile time from the declared events
  (docs/design/event-handles.md §9).
- [x] **NBA to a dynamic array** — and to an associative array. `HandleCopyNb`
  stages a deep copy in the Active region and MOVES it into the live handle in the
  NBA region, generalizing the queue's path to all three container kinds. A
  literal / `new[n]` right-hand side is still refused (it builds in place rather
  than deep-copying a handle the commit can move).
- [x] **Array ordering methods** (LRM 7.12.2) — `reverse` / `sort` / `rsort` /
  `shuffle` on a dynamic array or queue (`rt_dynarr_*` / `rt_queue_*` helpers),
  and on a fixed-size unpacked array through a mirror of one (see the fixed-size
  entry below).
  `reverse` / `shuffle` are element-agnostic; `sort` / `rsort` are value-keyed,
  taking an integral (width <= 64) or real element. `shuffle` draws
  from the calling thread's RNG (which forces its process to a coroutine, and
  reaches a subroutine body through the call ABI). This is the FLAT-VALUE path
  only — the element is its own key, which is what makes an unstable `std::sort`
  and an ascending-then-reverse `rsort` correct for a 2-state or real element. A
  `with` clause, a string element and a wide element go through the IR permutation
  sort instead (next entry but one). A **4-state** element takes its own
  comparator kinds (`sort_unsigned4` / `sort_signed4` -> `slot_cmp4` /
  `sort_run4`), which decode the slot's must/may planes and order under the total
  order for unknowns (see the 4-state entry below): that path is stable and orders
  descending directly, because neither flat-value shortcut survives unknowns —
  `8'b1111_111x` ties with `8'b0000_000x` without being byte-equal to it. Keeping
  them separate KINDS rather than a flag is what keeps the 2-state comparator free
  of a plane test: `slot_less` and `sort_run` are untouched, and the choice is one
  test in `rt_dynarr_sort` / `rt_queue_sort` beside the `len < 2` test.
- [x] **Array locator + reduction methods** (LRM 7.12.1 / 7.12.3) and the `with`
  iterator clause (7.12 / 7.12.4) — `find` / `find_index` / `find_first` /
  `find_first_index` / `find_last` / `find_last_index` / `min` / `max` /
  `unique` / `unique_index`, and `sum` / `product` / `and` / `or` / `xor`, on a
  dynamic array or queue. Lowered as one IR loop over the container, not a
  runtime helper: the clause is arbitrary IR, and going through the ordinary
  expression path makes 4-state, wide, real, string and struct elements work with
  no per-type comparator (so none of `sort`'s element restrictions apply here).
  A locator fills a fresh result queue; a reduction folds at the result type's
  width, so a `byte` sum wraps (7.12.3). `item.index` resolves through a binding
  stack, so a nested clause and `unique`'s dedup scan both keep their own index.
  Empty-array reductions answer 0 rather than the fold identity and 4-state
  `min`/`max` skip unknowns. An unpacked struct / array element works through
  the same loop: the iterator variable binds by
  aggregate copy and a locator's result queue receives whole elements, so
  `q.find(x) with (x.f > 2)` / `q.sum(x) with (x.f)` are ordinary. Guarded loud: an
  associative-array receiver (7.12 allows it; it needs key traversal), and an
  aggregate element with NO `with` clause — the bare form makes the element itself
  the key (7.12.1), and an unpacked aggregate has no ordering or fold. A
  fixed-size receiver is no longer among these (see the fixed-size entry below).
- [x] **`with` clause on `sort` / `rsort`** (LRM 7.12.2) — and, from the same
  mechanism, `sort` of a **string** and a **wide (> 64-bit)** element. The
  motivating shape is Accellera UVM `uvm_phase_hopper.svh:763`,
  `succ_q.sort with ( item.get_full_name() )`: a queue of class handles ordered by
  a string a method call returns, so the key is neither the element nor a field of
  it. The renameable iterator the prototype allows
  (`function void ordering_method ( array_type iterator = item )`) works, which is
  what the clause's own second example needs: `c.sort( x ) with ( {x.blue, x.green} )`.
  Keys: integral of any width, real, string, enum (integral, so it orders by base
  value) — the types 7.12.2's "the relational operators shall be defined for the
  type of the expression" admits. Anything else is refused by name.
  * **Lowered as IR, not a runtime comparator with a callback.** A keyed sort is a
    stable bottom-up merge sort over an index permutation emitted by
    `lower_array_sort_iter`, with one gather applying the permutation at the end —
    so the O(n log n) moves are on 32-bit indices and exactly n elements are
    copied. This is the locator family's reasoning (7.12.1) applied to ordering:
    the key is arbitrary IR, so the ordinary expression path makes string, wide,
    real, enum, struct-field and concatenation keys work with no C comparator and
    no `{kind, width}` element descriptor. The considered alternative was a
    generated `less` thunk handed to the runtime as a function pointer; it was
    rejected because the key expression can read enclosing frame locals, `this`
    and the call-ABI RNG pointer, none of which a standalone thunk can reach —
    clotho has no closure mechanism, and `rt_elem_ops` (which the idea was
    modelled on) carries no function pointers at all, only layout data.
  * **The key is evaluated per COMPARISON, not precomputed.** 7.12.2 fixes no
    evaluation count, so a side-effecting clause observes two calls per
    comparison; a test pins the count so switching to precomputed keys later is a
    deliberate, visible change. Precomputing n keys into a buffer was the
    alternative: fewer evaluations, but O(n) key storage, a second heap container
    for a string key, and a divergence from `unique`'s dedup scan, which likewise
    re-evaluates rather than caching.
  * **Ordering is stable, because a `with` clause makes ties observable.** With no
    clause the element IS the key, so equal keys mean byte-equal elements and tie
    order cannot be seen — which is why the flat path's `std::sort` was always
    fine. A clause breaks that: two DIFFERENT elements can share a key. The
    trap this creates is `rsort`: the flat path implements it as ascending then
    `reverse_run`, and that is NOT a descending sort once ties are observable,
    because it reverses each tie GROUP too. The keyed path therefore orders
    descending directly (compare with the operands swapped) and keeps equal keys
    in original order; a test asserts stability for both directions, the `rsort`
    one specifically to catch a reintroduced ascending-then-reverse.
  * **The GC-during-callback hazard does not exist on this design, and would not
    have bitten even on the other one.** Three independent facts: a container's
    payload is `malloc`'d, not GC-heap memory (`Gc.h:333`), so no collector — even
    a future evacuating one — relocates it; allocation never collects inline, the
    slow path only sets `mutator->request` (`Gc.cpp:1855`, `:3011`); and emitted
    code has exactly ONE poll site, the drain loop's quiescent edge after
    `rt_wheel_advance` ("THE safepoint", `LlvmBackend.cpp:11850`), which a sort
    running inside a process body is by construction not at. Measured, not
    assumed: a 400-element sort whose key allocates a fresh string per evaluation
    reports `gc: 1 collections` under `--gc-stress`, i.e. none during the sort.
    On this design the question is moot anyway — the runtime is never inside the
    sort, and every element access re-resolves through `elem_select`.
  * **A key expression that MUTATES the receiver is defined, not refused.** 7.12.2
    is silent. The element count is snapshotted at entry, the same rule `foreach`
    uses for its bound, so the sort orders exactly the elements that were there
    and anything the clause pushes stays past them; nothing dangles because no raw
    pointer is held across the clause. Refusing would have needed an epoch check
    per comparison to catch a case that cannot corrupt anything.
  * The plain scalar path is untouched: a bare `sort` / `rsort` on an integral of
    width <= 64 (either domain) or a real still emits `queue_sort` /
    `dyn_array_sort` and reaches `std::sort` with no call into generated code
    (verified on `--emit ir` / `--emit llvm`: an `int`, a `logic [7:0]` and a
    `logic [95:0]` queue emit two runtime sort calls — `kind 1 width 32` and
    `kind 3 width 8` — and exactly one merge-sort loop, the wide one; the keyed
    and string cases likewise emit loops and no runtime sort call).
  * Fixed en route: a string key, or any key that lowers to a PLACE inside the
    iterator variable rather than a value (a member select of an aggregate
    element), compared the left element against itself and reported every pair a
    tie — binding the iterator for the second key overwrote the storage the first
    key was still aiming at. The right key is now stored into a cell of the key
    type before the left is evaluated, which is why `min`/`max` and `unique` hold
    their keys in cells too.
  **4-state elements and keys are ordered, not refused** (the refusal this entry
  used to record is gone). LRM 11.4.4 says a relational compare "shall be 1'bx"
  when either operand carries an unknown, and the earlier reading was that 7.12.2
  therefore leaves x/z placement unspecified and refusable. That reading
  understates the problem and overstates the cost: x is not an unspecified RESULT
  to a comparison sort, it is an **inconsistent comparator** — collapse it to
  false and the relation is neither transitive nor consistent between `less(a,b)`
  and `less(b,a)`, which is UB in the algorithm. So clotho defines a strict weak
  ordering instead of refusing:

  ```
  unknown  <  defined,     unknown_i  ==  unknown_j   for all i, j
  ```

  Unknown means ANY x or z bit, so `8'b1111_111x` and `8'b0000_000x` are both
  unknown and equal to each other; x is not distinguished from z; defined values
  order numerically at the operand's signedness; and the order is stable, which is
  what makes the position of the mutually-equal unknowns deterministic rather than
  merely legal. `rsort` is the mirror (unknowns last), ordered directly — the one
  tie group containing every unknown is exactly what an ascending-then-reverse
  would flip. Signed 4-state is resolved by the same rule, since the unknown plane
  alone decides unknown-ness and signedness only ever orders DEFINED values. Reals
  and strings carry no x/z, so they are unaffected and got no new check. The keyed
  path tests `IsUnknown` (`$isunknown`, LRM 20.9) on each key, which is one
  reduction of the unknown plane and so needed nothing extra for a wide (> 64-bit)
  4-state element; the bare scalar path decodes the slot's must/may planes
  (`slot_cmp4`). The motivating shape is Accellera UVM `uvm_root.svh:827`,
  `m_time_settings.sort() with ( item.offset )` over `time offset`
  (`uvm_cmdline_report.svh:179`): `time` is 64-bit 4-STATE, so the refusal fired
  on a sort of ordinary simulation times with no unknown in them.
  Oracles: **verilator 5.050 fabricates a value** — it maps an unknown bit to 0
  and sorts the result as that value. Measured: `'{8'd3, 8'hxx, 8'd1}` sorts to
  `'{'h0, 'h1, 'h3}`, and `'{8'b0000_0000, 8'b1111_111x, 8'b0000_000x}` to
  `'{'b0000_0000, 'b0000_0000, 'b1111_1110}`, i.e. a partial unknown is ordered by
  the bits it does know. Clotho gives an unknown an explicit position instead of
  inventing a value for it. **iverilog 13 does not implement `sort()` at all**:
  "sorry: 'sort()' array sorting method is not currently supported"; it cannot
  even parse `s.sort()` on a string array, let alone a `with` clause.
- [x] **Fixed-size unpacked-array receiver** (LRM 7.12) — all three families on
  `int q[5]`, `int q[3:7]` and the descending `int q[7:3]`. 7.12 applies to "any
  unpacked array" and 7.12.2 spells it out ("fixed or dynamically sized"), so the
  refusal that stood at the shared container-method dispatch was clotho's own gap,
  not the clause's. The motivating shape is Accellera UVM
  `uvm_printer.svh:1498`, `int q[5]; int qq[$]; qq = q.max; m = qq[0];` — one of
  41 array-method call sites in uvm-core, and the only one not on a queue.
  * **What a fixed receiver actually changes is one thing: its indices are its
    DECLARED range, not 0 .. n-1.** That is threaded as a `DeclIndexMap`
    (`{first, step, count}`) carried beside the receiver: iteration position `p`
    holds the element at declared index `first + step*p`. A queue or dynamic array
    is `{0, +1}`, for which every expression the map builds collapses to the
    position itself — so this is ONE path with the old receivers as its
    degenerate case, not a fork, and the queue paths emit the instructions they
    emitted before. The walk itself stays 0-based; the map is applied only where
    an index is REPORTED or an element is ADDRESSED.
  * **Three consumers, one map.** `find_index` / `find_first_index` /
    `find_last_index` / `unique_index` push `decl_of(k)`; `item.index` (7.12.4)
    reads it through the `IterBinding` the clause already resolved by symbol
    identity; and `elem_select` takes it directly, because an `ir::ElemSelect`
    index into a fixed array is ALREADY a declared-range coordinate that the
    backend normalizes (`at.left`, either direction). Computing it once is what
    keeps a reported index and the element it names from disagreeing.
  * **Direction is the part a 0-based test cannot catch.** 7.12.1 defines the
    first element as the one "closest to the leftmost indexed element", so the
    walk starts at the declared LEFT bound whichever side it is on: position 0 is
    index 3 for `int b[3:7]` and index 7 for `int c[7:3]`, and `step` is -1 for
    the latter. Ordering likewise permutes in declared-address order, so `sort`
    puts the smallest element at the left bound and `c.sort()` reads ascending
    from index 7 down to 3. Both ranges are pinned by tests; negative bounds
    (`int a[-1:1]`) are still refused by the existing `lower_type` guard.
  * **Ordering reuses the dynamic-array implementations rather than growing a
    second copy.** The elements are mirrored into a temporary dynamic array in
    declared-address order, reordered THERE, and copied back
    (`lower_fixed_array_order`). So `reverse` / bare `sort` / `rsort` keep the
    runtime fast path including the 4-state comparators (`sort_run4`, so the total
    order for unknowns applies unchanged), `shuffle` is the same
    `rt_dynarr_shuffle` call and therefore draws the SAME RNG stream — a
    six-element fixed shuffle produces the permutation the dynamic-array test
    already pins and leaves `$urandom` at the same point — and a keyed `sort` goes
    through `lower_array_sort_iter` unchanged, with the receiver's map installed
    so `item.index` still reports declared indices. Reuse is pinned by RESULT:
    stability and the per-comparison clause count come out to the same
    `12 13 15 10 11 14` / `calls=8` the queue cases pin.
  * **One real bug the mirror exposed, fixed here.** An aggregate element crosses
    the `rt_*` ABI as a pointer to a CANONICAL aggregate image, but a fixed array
    of scalar-element sub-arrays stores FLAT leaf slots that may be independently
    z-widened — a different image size. Handing that pointer to `queue_push` made
    the runtime copy the wrong byte count and segfaulted on
    `int m[2][3]; mq = m.max with (item.sum);`. Result pushes now stage such an
    element through a canonical temp (`canonical_elem_ptr`, the rule
    `lower_container_element_value` already applies to a pushed actual), and the
    mirror copy decomposes per leaf instead of taking the whole-image memcpy
    shortcut. A queue receiver is unaffected: its elements already live in a
    canonical image, and it emits what it emitted before.
  * **Multidimensional receivers work through the clause's own nested form.**
    7.12.3's worked example `logic [7:0] m [2][2]; m.sum with (item.sum with
    (item))` gives 50, because the ELEMENTS of `m` are its sub-arrays and the
    inner clause reduces one of them per iteration — not a flattened leaf sum.
    Index results name the outer dimension in declared coordinates
    (`int n[2:3][3]` answers 3), `min`/`max` return a queue OF sub-arrays, and
    `reverse` / `shuffle` reorder sub-arrays because they move whole slots. A
    BARE `m.sum` or `m.max` is refused — with 7.12.3's and 7.12.1's own words, by
    slang's binder ("unpacked arrays of integral values" / "of comparable
    values"), which is the right place for a type rule — since no arithmetic or
    relational operator is defined for a sub-array. `item.index(2)` for the
    second dimension is still refused by clotho.
  * **Refused loudly, each on the clause:** a bare reduction of a
    non-integral element (multidimensional, `string`, `real`) and a bare
    `min`/`max`/`sort` of a non-comparable one, both by slang ahead of lowering
    (7.12.3 "any unpacked array of integral values"; 7.12.1 / 7.12.2 "the
    relational operators shall be defined for"); an aggregate element with no
    `with` clause, by clotho, now citing both clauses; an element a dynamic array
    cannot hold (a `chandle`), which only the ordering mirror needs
    (`guard_container_element`); and `item.index(d)` for `d > 1`.
  * Remaining receiver gap: the **associative** array, whose index family is
    key-typed and whose walk is the `assoc_traverse` chain.
- [ ] **`map`** (LRM 7.12.5, 1800-2023 only) — the one array manipulation method
  slang binds that clotho does not lower, so `q.map(x) with (x * 2)` hits the
  container-method guard (`--std 1800-2023`; older versions reject it in the
  binder). It is the locator loop with the CLAUSE VALUE collected instead of the
  element, so it reuses the `with`-iterator infra; the result keeps the receiver's
  container kind with the clause's element type (queue -> queue, dynamic ->
  dynamic, assoc -> same key type, fixed -> same range).
- [ ] **Multidimensional dynamic arrays**.
- [x] **`%p` of a container with a non-scalar or wide element** (LRM 21.2.1.6) —
  the runtime formatter renders an element from a flat descriptor (one word, no
  member names), which covers a single-word integral, a real and a string. An
  aggregate, a nested container, an element wider than one word and a tagged union
  now render through a frontend loop instead: one `$sformatf` per element built by
  the ORDINARY aggregate expansion, concatenated over the live element count. So
  member names, nested arrays, string leaves and tag names all print exactly as
  they do outside a container (`'{'{a:1, s:"hi"}, ...}`).
- [x] **NBA to a whole queue** (`q <= rhs`, LRM 10.4.2) — `QueueCopyNb` stages a
  deep copy of the RHS in the Active region (per-queue `rt_nba_arr` buffer), then
  `rt_queue_move`s it into the live handle in the NBA region (managed-element
  safe). Pattern/concat/slice RHS reuse the materialize path.
- [n/a] **NBA to a queue element** (`q[i] <= x`) — **illegal SystemVerilog**, not
  a gap. LRM 10.4.2 forbids a nonblocking assignment to an element of a
  dynamically-sized array (queue / dynamic array); slang rejects it before Clotho
  ("nonblocking assignments to elements of dynamically sized arrays are not
  allowed"), which sidesteps the sample-vs-commit realloc hazard by disallowing
  the construct. Nothing to implement.
- [x] **Queue slice `q[a:b]`** (LRM 7.10.4) — materialized element-wise into a
  temp handle via `QueueAppendRange` (`rt_queue_append_range`), usable in any
  rvalue position; bounds runtime-clamped. Dynamic-array slice still guarded.
- [x] **Non-variable queue assignment** (concat `{q,e}` / pattern `'{...}`) —
  `materialize_queue_rvalue` builds a temp handle (alias-safe for `q = {q,e}`),
  then a deep copy into the target; splices whole-queue operands, pushes scalars.
  A queue that IS an element or a member of something (`q = qq[i]`, `q = s.inner`)
  needs no temp: it already has a handle lvalue.
- [x] **Queue / dynamic-array equality** (`==` / `!=`, LRM 11.2.2/11.4.5) — direct
  and as an aggregate member, reached from the AggEq leaf walk: size mismatch is a
  definite 0, else per-element 4-state compare with x-propagation. Fixed a prior
  silent miscompile (handle compared as a value). A single-word integral / real /
  string element goes through `rt_container_eq`; an aggregate, a nested container
  or an element wider than one word walks element-wise in the backend
  (`emit_container_eq_loop`), recursing through the same aggregate compare — which
  is also what gives a queue of queues (and a struct holding a queue) a compare.
- [ ] **Associative arrays** — `[*]`/typed keys, `.exists`, `.first`/`.next`,
  `.num`, `.delete`.
- [ ] **Bit-stream casting** (`{<<{}}` / `{>>{}}` cast operands, LRM 6.24.3).
- [ ] **Casts to/from enum / struct / union**. `$cast` is tracked under M4. Open
  question, low confidence that it is a real violation: `$cast` into a **packed
  tagged union** is a plain bit cast and is *not* tag-checked, so it can install a
  tag/payload pairing no `tagged` expression could build — which LRM 11.9's
  "completely type-checked" accesses then trip over. Packed-to-packed bit casts
  are legal, so this may be correct as it stands; needs a reading. Note the entry
  previously claimed union casts were unsupported, which under-claimed what
  actually happens.
- [x] **2-state and sized integer types** — `bit`/`byte`/`shortint`/`int`/`longint`
  as genuine 2-state; 4-state<->2-state conversion (LRM 6.3.1).
- [x] **`string` type** (LRM 6.16) — SSO storage, assignment, concat, comparisons,
  `s[i]` read/write, the method set, string<->packed casts, `%s`, by-value
  args/returns, fixed unpacked arrays, string conditions.
- [x] **`enum`** — base type, member folding, methods (LRM 6.19:
  `first`/`last`/`num`/`next`/`prev`/`name`).
- [x] **`struct`** — packed member select/assignment; unpacked struct / arbitrary
  aggregate nesting (recursive storage mirror, composed-GEP path); read,
  blocking/nonblocking write, whole-object copy / `==` `!=`, by-value through
  functions, across module ports, aggregate net declaration assignment. Design:
  docs/design/unpacked-struct.md.
- [x] **Assignment patterns `'{...}`** (LRM 10.9) — positional, keyed
  (member / array-index / `default`), and replicated forms over packed structs /
  vectors / multi-dim packed arrays, unpacked structs, and fixed unpacked arrays.
- [x] **Assignment-pattern lvalue destructuring** (`'{a,b,c} = rhs` / typed
  `U'{a,b,c} = rhs`, LRM 10.9) — positional only, over a packed value (bit-slice,
  shares `store_concat`), an unpacked struct, or a fixed unpacked array
  (structural per-element walk); nested pattern/concat members recurse. Element
  count / keyed-or-replicated-as-lvalue / compound-assignment are guarded loud
  (slang itself rejects most of these at binding time already). Queue / dynamic
  / associative-array right-hand side, and a string / handle-typed member, are
  guarded loud (not supported); iverilog and verilator both reject the syntax
  outright.
- [x] **`typedef`** — alias resolution, canonicalized at `lower_type`.
- [x] **Packed multidimensional arrays** — element / range / nested bit & part
  select, read + lvalue, constant + dynamic indices, arbitrary nesting (LRM
  7.4.5/7.4.6). Array query functions `$size` / `$dimensions` /
  `$unpacked_dimensions` / `$left` / `$right` / `$low` / `$high` / `$increment`
  (LRM 20.7).
- [x] **Dynamic arrays** — `[]`, `new[n]` / `new[n](src)`, `.size()`, `.delete()`,
  element r/w, whole-array deep-copy, `foreach`, full subroutine ABI.
- [x] **Checked container allocation** — every heap buffer a design's containers
  need routes through `rt_alloc_checked` / `rt_alloc_elems_size` (Runtime.h).
  Three silent failures are now located fatals instead of a corrupt heap: a
  `new[n]` count that arrived zero-extended (so `new[-1]` asked for 4294967295
  elements — the count is `int64_t` at the ABI now, the backend sign-extends a
  signed count and zero-extends an unsigned one, and the runtime rejects `n < 0`),
  an element-count-times-stride product formed by wrapping multiplication
  (`__builtin_mul_overflow` now), and a `malloc` that simply returned null. Each
  reports through an `rt_alloc_site` constant the backend emits beside the call,
  so the diagnostic carries the file position, source line and caret rather than
  naming the runtime. The only ceiling is `size_t`. Covers dynamic arrays,
  queues, strings (including the `{N{s}}` replication product) and associative
  arrays; coroutine frames (`rt_alloc`) keep their own path.
- [x] **Queues** — `.size()`, push/pop/insert/delete, `q[i]` / `q[$]`, `foreach`,
  whole-queue deep-copy, bounded `q[$:N]`, full subroutine ABI.
- [x] **Associative arrays** (LRM 7.8/7.9) — all key kinds (integral of any
  width / enum / packed / `string` / `[*]`-wildcard / **class handle** (7.8.3) /
  **unpacked struct or fixed array** (7.8.5), unified as normalized
  order-preserving byte spans, see docs/design/dynamic-arrays.md §4a), index
  r/w, `.num`/`.size`/`.exists`/`.delete(k)`/`.delete`/`.first`/`.last`/`.next`/
  `.prev`, ascending-key `foreach`, whole-array deep-copy, `'{key:value,
  default:v}` literals (7.9.11), full subroutine ABI.
  Value element: every kind a queue / dynamic-array element takes (one shared
  rule, `guard_container_element`) — integral / real / `string` / class handle /
  unpacked struct or array including one with string or container leaves /
  a nested queue, dynamic array or associative array (`rt_elem_assoc`). Member,
  element and whole-value r/w, equality, absent-key default. `a[k]` resolves its
  entry by DIRECTION: a mutating method creates it, a query reads through get_ptr
  and falls back to the empty-container default, so `a["absent"].size()` is 0 and
  leaves `num()` alone (LRM 7.8.6).
  An element select THROUGH a container-valued entry (`a[k][i]`) works, to any
  nesting depth, in both directions and for a mutating method's receiver: the access
  direction is threaded down the select chain (`ir::ElemSelect::for_write`) rather
  than deferred to the consumer, so a read resolves get_ptr and creates nothing (LRM
  7.8.6) while a write and a mutating receiver resolve put_ptr (7.8 / 7.10). A `ref`
  bound through one is checked at every level (docs/design/data-model.md §3.1).
  Still guarded loud, for a stated reason (§7): a `string` or dynamically-sized
  member inside a struct KEY needs
  the order-preserving escaped encoding (the normalized span would otherwise be
  variable-length). Whole-array NBA works (`HandleCopyNb`); a literal RHS under
  NBA does not. An x/z index is invalid per 7.8.4 / 7.8.5: the read yields the
  element default, the write is dropped without creating an entry, `exists`
  answers 0 and `delete` is a no-op. iverilog cannot parse associative arrays at
  all, and verilator creates an entry on an absent read, violating LRM 7.8, so
  neither models this behaviour.
- [x] **Casting (scalar/packed)** — static / size / sign / const cast,
  string<->packed, real<->int. A widening cast extends by the SOURCE's
  signedness: LRM 6.24.1 defines a cast as "the value that a variable of the
  casting type would hold after being assigned the expression", which is LRM
  11.8.3's "sign extension if, and only if, the type of the right-hand side is
  signed". This was wrong until `fix/int-cast-signedness`: the cast lowered to a
  single `Resize`, whose extension follows its TARGET type (LRM 11.8.2's
  propagated-operand rule), so `int'(1'b1)` answered -1. Measured extent over a
  396-case matrix (source signedness x cast target x widths 1/2/7/8/31/32/33/64,
  in tests/llvm_backend_test.cpp): 61 wrong, every one of them an UNSIGNED source
  widened into a SIGNED target — `bit`, `logic`, `byte unsigned`, a packed struct
  and an enum alike, at every width, so it was never a width-1 story.
  `signed'` / `unsigned'` / `$signed` / `$unsigned` (LRM 20.5) never widen and a
  size cast `N'()` passes signedness through, so all four were already right;
  implicit assignment conversion was right only because slang already splits it
  into widen-then-retag; call arguments and return values, which the frontend
  coerces itself with nothing to split them, carried the bug. The constant-folded
  path (`localparam int P = int'(1'b1)`) is slang's own evaluator and was already
  correct. iverilog 13 and Verilator 5.050 agree with each other on all 396
  cases. The fix is in `Lower::coerce`, which now takes an `Extend` selecting
  between the two LRM rules and, for the 11.8.3 one, emits the widening `Resize`
  under the source's sign before the retag; a slang `ConversionKind::Propagated`
  node still gets 11.8.2, which genuinely differs in both directions
  (`logic [7:0] u; logic signed [3:0] s = -1;` gives `u + s` == 16 but
  `u = s` == 255).
- [x] **Parameterized types / type parameters** (LRM 6.20.3).

### Operators & expressions

- [ ] **Concatenation compound-assignment lvalue** (`{a,b} += x`).
- [ ] **String-element compound-assignment lvalue** (`s[i] += c`).
- [ ] **`inside` string / real left operand**.
- [ ] **`inside` unpacked-array set member** (`sel inside {arr}`).
- [ ] **`inside` tolerance ranges** (`[c +/- d]`).
- [ ] **`inside` unbounded `$` range bounds**.
- [x] **Part-select / dynamic bit-select OOB bounds check** (LRM 11.5.1: OOB read
  -> X (2-state: 0), write -> no-op, per bit for a partially out-of-range access;
  an x/z index is wholly out of range). Clamped against the container's declared
  range in both directions, negative indices included; `--sanitize=bounds` reports
  the same sites. A **runtime-offset** select of a vector wider than 4096 bits goes
  through the runtime word-indexed path (`rt_wide_sel` / `rt_wide_ins`) rather than
  a whole-value variable shift, which cost O(width^2) JIT time and made a 65 Kbit
  vector (ivtest pr722) appear to hang; blocking and nonblocking, any selection
  width. See tests/wide_select_test.cpp.
- [ ] **Dynamic part-select** (`sig[msb_expr:lsb_expr]`) — variable bounds
  (const-bounds only today).
- [ ] **Streaming operators, what still needs a bit-level buffer** — `with`
  clause; an operand or destination whose width does not divide into whole
  destination granules (`bit [5:0] q[$]` into a string, a 12-bit value into a byte
  queue); `{<<S{...}}` over a dynamically sized stream (which block a bit lands in
  depends on the total width, so the whole stream has to exist first); a
  dynamically sized SOURCE in an unpack (`{>>{q}} = s`); a dynamically sized
  stream into a FIXED packed target (`int v = {>>{q}}`, where LRM 11.4.14.3's
  overflow answer is still open — verilator's is not derivable from the text).
  Each is refused loudly with the width in the message.
- [x] **Logical implication / equivalence** (`->`, `<->`) in expressions
  (LRM 11.4.7) — dedicated `Limplies`/`Lequiv` IR ops with direct 3-valued
  truth tables. `->` short-circuits (skips lowering the right operand
  entirely when the left is definitely false, LRM 11.3.5); `<->` always
  lowers both operands exactly once (LRM 11.3.5 excludes it from the
  short-circuit list). iverilog does not implement `->`'s short-circuit but
  otherwise agrees on truth-table values.
- [x] **Increment / decrement** (`++`, `--`, pre and post, LRM 11.4.2).
- [x] **Compound assignments** (`+= -= *= /= %= &= |= ^= <<= >>= <<<= >>>=`,
  LRM 11.4.1) — single-evaluation LHS.
- [x] **Assignment as an rvalue** (`y = (x += 1)`, LRM 11.3.1).
- [x] **Case-equality operators** (`===`, `!==`).
- [x] **Wildcard equality** (`==?`, `!=?`) — wildcards only the right operand's x/z.
- [x] **Short-circuit `&&` / `||`** — value-producing `SelectLazy` (LRM 11.3.5, 11.4.7).
- [x] **`inside` operator** (LRM 11.4.13) — integral scalar / packed operands
  (members compare with `==?`, so a member's x/z bits are do-not-cares and the
  left operand's are not) and REAL operands, which 11.4.13 compares with `==`
  because they are nonintegral: `FEq` per value member and `FGe` / `FLe` for a
  range, folded with the same OR reduction. An UNPACKED-ARRAY member is traversed
  elementwise ("descending into the array until reaching a singular value"): a
  fixed-size member unrolls and recurses through nested dimensions, a queue or
  dynamic array walks at runtime (11.4.13's own example is a queue member).
  iverilog 13 cannot compile `inside` at all. Still guarded loud: a string left
  operand, an associative-array member (its traversal is the 7.9.4 first/next
  key walk, not an index loop), a queue / dynamic-array member whose elements
  are themselves non-singular, tolerance ranges and `$` bounds.
- [x] **Indexed part-select** (`sig[base +: width]`, `sig[base -: width]`).
- [x] **Streaming operators** (`{<<{...}}`, `{>>{...}}`, LRM 11.4.14) — pack and
  unpack. A fixed-size stream is one packed value (reversal included); a stream
  with a queue / dynamic-array / string operand keeps its operands apart as chunks
  and the destination consumes them granule by granule: bytes for a string
  (`StrAppendPacked` / whole-string concat, which makes `s = {>>{string_q}}` the
  join UVM's `m_uvm_string_queue_join` wants, one runtime call per element),
  elements for a queue (pushed) or dynamic array (counted, then sized and stored).
  Unpack takes the source's leftmost bits, LRM 11.4.14.3's greedy rule giving the
  first dynamically sized destination everything the fixed ones leave; a fixed
  unpacked array / struct destination walks leaves in declared order
  (`unpack_stream_agg`, the inverse of `serialize_agg_lvalue`). A packed operand
  reaching a string converts through LRM 6.16's 8-bit-chunk rule, dropped 0x00
  chunk included, so a folded `localparam` and the runtime path agree. Oracle:
  verilator for every shape it supports (fixed unpack, `<<` reversal, unpacked
  struct, string join), slang's constant evaluator otherwise.

### Procedural control

- [ ] **`case … inside` tolerance ranges** (`[a +/- b]`, `[a +%- b]`).
- [ ] **`case … inside` unbounded range bounds** (`[$:a]`, `[a:$]`).
- [~] **`unique`/`unique0`/`priority` Observed-region reporting** — reports now
  emit in the Observed region (LRM 4.4.2.4) via the deferred report queue
  (`rt_observed_flush`), not the Active region. Still missing: per-statement
  *maturation* (flush-on-re-execution) for glitch immunity per LRM 12.4.2, which
  lands with deferred immediate assertions (they share the queue).
- [ ] **`&&&` / `matches` predicates** in `if`/`?:`, and `case … matches` pattern
  matching.
- [ ] **`always_ff` non-sequential-behavior warning** (LRM 9.2.2.4) — the block
  executes correctly; slang emits no diagnostic when the body is not sequential.
- [x] **`foreach`** (12.7.3) — fixed-size arrays + `string`; dynamic arrays and
  queues iterate the runtime size; associative arrays iterate keys in ascending
  order via first/next (a `[*]`-wildcard index has no key type, so foreach over
  it is illegal per LRM and rejected by slang). A NESTED runtime dimension
  (`foreach (a[k, i])` over an assoc of queues, a queue of queues, an assoc of
  assocs) measures and iterates the entry the outer dimension is sitting on,
  rebuilt per region from the outer loop variables (`foreach_base_lv`); dimensions
  are peeled through the interned IR type, since slang answers
  `isDynamicallySizedArray()` for a map as well. Before that every runtime
  dimension measured the whole array, so the inner dimension re-walked the OUTER
  container with the inner loop variable bound to a key — wrong trip count, wrong
  index, and silent. Refused loud: a skipped outer dimension over a nested runtime
  one (`foreach (a[, i])`), which names no entry to descend into.
- [x] **`do … while`**.
- [x] **`break` / `continue`**.
- [x] **`case … inside`** (12.5.4 / 11.4.13) — singular labels via wildcard
  equality, `[lo:hi]` ranges via inclusive membership; jump-tables a run of
  clean-constant singular labels.
- [x] **`unique` / `unique0` / `priority` on `case` and `if`** — no-match and
  overlap violation reports (LRM 12.5.3 / 12.4.2).
- [x] **`final`** blocks (see M1 Procedural control).
- [x] **`void`-cast calls, function/task default args, named arg binding** — slang
  fills defaults / reorders named args into the positional list.

### Hierarchy & namespaces

- [ ] **Interfaces & modports, remaining** — bidirectional/multi-driver interface
  nets (a net continuously driven from both sides fails loud, shared with `inout`;
  single-driver works), modport `export` methods, and structural modport
  expression ports (`.name(sig[hi:lo])` / concat; the whole-signal rename form
  `.name(sig)` works). Virtual interfaces (LRM 25.9) are under M4, below.
- [x] **Interfaces & modports** (LRM 25) — interface instantiation, plain and
  modport-restricted interface ports, generic interface ports (`interface p`),
  modport views/directions, whole-signal modport expression (rename) ports,
  signals/tasks/functions in interfaces, parameterized interfaces, interface
  arrays, and interface ports forwarded through submodules. Storage is shared
  (no copy process): every reference — plain-port, modport, rename, or
  hierarchical — canonicalizes to the interface instance's own `ValueSymbol`
  (`canonical_value_symbol` in `src/Lower.cpp`). iverilog has no interface
  support. Design: docs/design/interfaces.md.
- [ ] **`bind`** — bind a module/interface into another scope.
- [ ] **Nested / extern module declarations**.
- [ ] **`export pkg::*;` pure pass-through** — a name the exporting package never
  itself references fails to resolve (slang lookup limitation). The
  internally-referenced form works.
- [ ] **Package-scope classes / covergroups / checkers / nettypes** — hit their
  own (pre-existing) unsupported-construct guard if referenced.
- [x] **Packages** — `package`/`endpackage`; `import` (explicit `p::name` and
  wildcard `p::*`), bare `pkg::name` resolution, `$unit` scope, package/`$unit`-scope
  variables (one shared signal). Partial `export` re-export and package-importing-
  package verified vs iverilog.

### Classes (basic OOP)

- [x] **Class declaration & objects** (Phase 1) — properties (scalar / real /
  string / nested struct-array / self-referential handle), methods (functions +
  tasks, `this`, recursion), constructor `new` + default property initializers,
  field read/write, handle assignment (reference semantics), `null`, `==`/`!=`
  identity, pass-by-value through the subroutine ABI. A class handle is a 2-state
  64-bit value (like `chandle`); the object body reuses the unpacked-aggregate
  storage. Lifetime: leaked by default (LRM 8.4 objects are GC'd — unobservable);
  collected under `--gc`, see docs/design/garbage-collection.md and the M4 item
  below. See docs/design/classes.md; verified vs iverilog in tests/class_test.cpp.
  Deferred within Phase 1, guarded loud: nonblocking assignment to a class
  property. (`$display("%p")` of a handle was listed here too and is not:
  a class handle prints its class name and the other four reference types print
  theirs — String formatting, under UVM enablers.) (Class handles as queue /
  dynamic-array / assoc-array elements were listed here as guarded and are not:
  all three work. They are refused only under `--gc`, which cannot yet trace
  through a container — garbage-collection.md §3.3.) A property initializer may
  now CALL a method of its
  own class (LRM 8.7 runs initializers as part of construction, so `this` is in
  scope; `ctor_this_` carries the object under construction from the `new` site and
  is consulted ahead of the enclosing frame's `this`, which the old code handed to
  the callee whenever the `new` sat inside another object's method — a silent
  miscompile, not just a refusal). A bare instance-method call with neither an
  object under construction nor a frame `this` is still guarded loud. A property
  initializer that READS a sibling property resolves the same way, through
  `value_lvalue`'s own `ctor_this_` arm: it used to die as an unknown signal when
  the `new` sat outside a method, and silently read the ENCLOSING object's
  same-named property when it sat inside one.
- [x] **Shallow copy** (`dst = new src`, LRM 8.12) — allocates a second object and
  copies every non-static property member-wise, running neither the constructor nor
  the declaration initializers (8.12 step 1). What each property does follows from
  its type, through the same `emit_agg_copy` an aggregate assignment uses: a string
  / queue / dynamic array / associative array property deep-copies, a class-handle
  property copies as a handle, so the copy and the source share what they point at
  (8.12: "objects, however, are not copied, only their handles"). The object
  allocated is of the SOURCE's class, not the expression's static type: every
  class carries a synthesized copy thunk in a reserved descriptor slot
  (`kCopySlot`), so `new base_handle` dispatches to the concrete class and allocates
  its size (iverilog 13 asserts on the polymorphic case, so it cannot cover
  this). A null source is the usual located fatal. Randomization state (8.12 step 2)
  is moot: `rand`/`randc` properties and covergroups are refused upstream.
- [x] **Null-handle dereference is a located fatal** (LRM 8.4) — accessing a
  non-static property, or dispatching a virtual method, through a handle that
  holds no object is illegal with an indeterminate result the LRM lets an
  implementation error on. A handle is raw pointer bits, so the untrapped outcome
  was `inttoptr 0` and an unattributed segfault. Every `HandleDeref` and every
  vtable load now branches on a compare against zero (`emit_null_handle_check`,
  weighted 1:2^20 into a `cold noreturn` block ending in `unreachable`) into
  `rt_null_handle`, which prints a compiler-style diagnostic — file position,
  source line, caret under the handle expression — and exits nonzero. The
  frontend records the object expression's span on the deref so `obj.prop`
  underlines `obj`; the deref rooted directly at `ClassNew` (a `new`'s own
  property default-initializer stores) skips the guard. A non-virtual method
  called on a null handle is caught at the first property its body touches; one
  that touches none runs, which the LRM's "indeterminate" permits.
- [x] **Forward typedef classes** (`typedef class C;`, LRM 6.18 / 8.27) — the
  class-resolution half needed no work: `lower_class_type`'s intern-first
  cache is keyed on the `ClassType` symbol pointer, which slang resolves a
  forward declaration and its later full definition to identically, so mutual
  cross-class handle references (2-way and N-way cycles) fall out for free.
  The one real gap was `lower_members`, which walked module/generate-block
  members through an exhaustive switch that had no case for
  `SymbolKind::ForwardingTypedef` (the forward declaration itself, as
  opposed to the class it names) and loud-failed on it — the declaration is
  legal only inside a module/generate scope's member list, so every forward
  typedef anywhere but the top compilation-unit scope hit this. Fixed by
  adding it to `lower_members`' inert-member case list (it carries no
  type/value/statement content, same as `TypeAlias`). Covered (iverilog has no
  class support): 2-way and 3-way mutual handle references (fields, method
  params, method calls through the handle, null comparison), a forward decl
  after its definition, duplicate forward decls in one scope, and the
  LRM-mandated error for a forward decl that never resolves. See
  tests/class_test.cpp.
- [x] **Single inheritance** (`extends`, `super`, non-virtual override, LRM 8.13)
  — Phase 2. Prefix layout: base properties lay first in the object body
  (`element` records the base Ref, `lower_class_type` copies the base's flattened
  field table as the prefix), so an inherited property keeps its base-relative
  field index and an upcast is the same pointer. A non-virtual method binds
  statically (slang resolves the SubroutineSymbol by the handle's static type;
  the per-symbol method table needs no change). Base construction (LRM 8.15) runs
  at each constructor's entry from `getBaseConstructorCall()` — implicit no-arg,
  `extends B(args)` (class-scope args), or an explicit `super.new(args)` (the body
  statement is a no-op marker); `emit_base_construction` / `emit_ctor_call` recurse
  through a constructor-less intermediate class. Whole-chain property defaults apply
  base-first at allocation. `super.method()` reuses the ordinary method-call path.
  A **virtual** method is guarded loud (Phase 3 needs a vtable); the built-in
  `randomize` virtual is skipped via `MethodFlags::BuiltIn` (iverilog cannot
  resolve inherited methods from a derived method body). See
  docs/design/classes.md and tests/class_test.cpp.
- [x] **Access control & statics** (Phase 2) — `static` properties are one slot
  shared across all instances (and inherited by derived classes), persistent
  module-signal storage keyed by the property symbol (`declare_static_prop`), not
  part of the per-object body; initialized once at time zero in the PreInit
  process (LRM 8.9.2). Reachable as `obj.p`, `C::p`, or a bare `p` in a method
  (all routed to the shared signal in `value_lvalue` / `lower_member_lvalue`).
  `static` methods reuse the free-subroutine path (no `this`). `local` /
  `protected` / `const` carry no runtime obligation (slang enforces at
  elaboration). An aggregate / container initializer (unpacked struct or array,
  queue, dynamic array, associative array) routes through `emit_struct_store` into
  the shared slot, the same element-wise path a module-level declaration
  initializer takes; a `string` initializer stores whole, bypassing `coerce`
  (whose `Resize` rejects a string value). Lowered with `fn_ctx_` / `ctor_this_`
  cleared, since LRM 8.11 scopes `this` to non-static methods only. Still missing:
  a static property of aggregate type with FIELD-level defaults and no initializer
  of its own reads the power-on default instead (`declare_static_prop` returns early
  on a null initializer) — silent, not guarded.
  A `static` method is now callable through an OBJECT HANDLE (`p.f(x)`), not only
  as `C::f(x)` or bare inside the class — LRM 8.9 grants a static member that
  freedom explicitly, and its own example reads `p.fileID` through a handle that
  was never assigned, so a null receiver is legal and dereferences nothing.
  `resolve_method_this` returned the receiver for any non-null `thisClass()`
  without asking whether the callee was static, and slang gives a static method no
  `thisVar`, so `build_function`/`splice_this` had built no receiver formal: the
  spliced handle shifted every actual one formal to the LEFT. The IR shape that
  turned that into a crash is a static method whose first formal is `ref` —
  `lower_call_common` read `arg_dirs[0] == Ref` for the handle operand and asked
  `lvalue_of` for a place, but a handle is a VALUE, so `lvals_` held a DISENGAGED
  `std::optional<LValue>`; dereferencing it read the all-zero buffer as an LValue
  and faulted on the first null member (`bit_off`, in `ref_actual_ptr` →
  `is_zero_const` → `dyn_cast<ConstantInt>(nullptr)`) with NO diagnostic at all,
  exit 139. Found compiling `uvm_component::apply_config_settings`, whose
  `rp.sort_by_precedence(rq)` calls `static function void (ref rsrc_q_t)`; the
  11-line repro needs no UVM. The gate is slang's `MethodFlags::Static`, not
  `thisVar == nullptr`: a PURE VIRTUAL also has a null `thisVar` in slang's
  degenerate representation and must keep taking a receiver. The receiver
  expression is still LOWERED for its side effects and then dropped, matching what
  the static-PROPERTY path already does with its prefix (`make_pool().f()` bumps
  its counter exactly once). Sibling audit: the same misalignment hit every static
  shape reached through a handle, not just `ref` — a `void` method with a plain
  argument, a zero-argument function (which indexed `param_types[0]` on an EMPTY
  vector), and a static task all produced LLVM's own unlocated `Incorrect number of
  arguments passed to called function!`; all four now agree with the `C::f` form.
  `super.f()` and bare in-method calls were already correct (both reach the
  `thisVar == nullptr && !is_user_virtual` arm), and the two capture detectors that
  mirror this condition (`ForkCaptureDetector` and the implicit-`this` visitor) test
  `thisVar` directly, so they already excluded static callees. Two backend guards
  close the class of defect rather than this one instance: `lvalue_of` refuses a
  disengaged entry (the single chokepoint every place consumer funnels through), and
  `lower_call_common` / `lower_task_call_inst` refuse an actual list whose length
  does not match `param_types` before any of the seven per-formal indexing sites
  runs. Both are located `BackendError`s, so a future marshalling slip is a
  `file:line:col` refusal instead of a silent segfault.
- [ ] **Cross-module class type identity** — the class type cache is per-Lowerer,
  so a `$unit`/package-scope class referenced by two modules interns two distinct
  (nominal) Refs; V1 exercises classes only within one module. Route
  `lower_class_type` through `globals_` like subroutines when this matters.

### Assertions (immediate)

- [ ] **Immediate `cover` count + report** (LRM 16.3 / 16.5) — the observable
  pass-action-on-true behavior is done; the per-point coverage count and
  end-of-sim report belong with a dedicated coverage engine.
- [~] **Deferred immediate assertions** (`assert #0`, `assert final`, LRM 16.4) —
  Phase 1 done: observed + final, per-process generation flush (glitch
  suppression), args sampled at execution, Observed/Postponed
  maturation (`rt_defer_report` / `rt_defer_flush_*`).
  Multi-instance is supported (each instance flattens to its own coroutine +
  generation global). Remaining (each guarded loud): user task / void-function
  action blocks (generic deferred-call ABI), `$display`/`$write`/`$fatal`
  actions, deferred assertions inside a function/task (generation threaded
  through the call ABI), and `disable`-based flush points (16.4.4). The
  observed/final distinction now has the Reactive region it was waiting on
  (`@clotho.reactive`), but a deferred report still matures through the deferred
  queue instead of scheduling an action block into Reactive, so the LRM's
  Reactive-region double-report is still not reproduced.
- [x] **Immediate assertions** — `assert` / `assume` / `cover` (LRM 16.3) with
  pass/fail action blocks; branch selection verified vs iverilog.
- [x] **Severity system tasks** — `$error`, `$fatal`, `$warning`, `$info` (LRM 20.10).
- [x] **`$signed` / `$unsigned` / `$clog2`**.
- [x] **Utility system functions** — `$bits`, `$countones`, `$countbits`,
  `$onehot`/`$onehot0`, `$isunknown`, `$typename`, `$isunbounded` (LRM 20.6.1).
  Sub-gaps:
  - [ ] `$countbits` with a **non-constant** control bit.
  - [x] `$typename` of an enum / struct / union / class / container type — every
    kind, from slang's own `TypePrinter` (which implements the clause in full)
    rather than a second hand-written formatter. The old refusal reasoned that
    Clotho must not name a type it does not lower; that expired once those types
    landed, and the hand-rolled output was byte-identical to slang's for every kind
    it covered. An anonymous type gets the clause's implementation-generated unique
    name, which is load-bearing: `$typename` strings match only for equivalent types.
  - [ ] `$isunbounded` of a **hierarchical** reference (slang refuses to
    constant-evaluate it, and Clotho has no cross-instance constant context).

---

## M4 — Industrial SystemVerilog + UVM

### Full object orientation

- [~] **Garbage collection** (LRM 8.4) — class objects are always collected; no
  design leaks them any more. Design and phasing:
  docs/design/garbage-collection.md. Landed: an Immix-shaped mark-region heap
  (32 KiB blocks / 128 B lines, `src/runtime/Gc.cpp`), a GC word below the
  descriptor pointer at handle[-1], a per-class trace map and finalizer emitted
  beside each class descriptor, precise `clotho.gcroots` tables over design state
  and statics, conservative pinned coroutine-frame roots through an intrusive
  registry node CoroSplit hoists into the frame, and stop-the-world collection at
  the drain loop's quiescent edge. Non-moving; the header reserves the forwarding
  and pin encodings so evacuation stays a policy change. 48 cases in
  tests/gc_test.cpp. P2 landed: the bump-allocation fast path and the
  frame registry are emitted inline, and allocation is 2x faster than the leaking
  path. P3 landed: generational in place, sticky mark bits, an inlined write
  barrier over object remembering, per-line age records, young/full kinds and
  `--gc-full-every`; 3.3-5.9x over forcing every collection full. P4 landed: class
  handles inside dynamic arrays, queues, assoc arrays and aggregates are described
  and rooted, so a UVM-shaped component tree runs collected. Remaining: event
  handles / `std::mailbox` / `semaphore` / `process` / `std::weak_reference` as
  those features land; then evacuation and multi-mutator as independent,
  measurement-gated tracks. Nothing is refused that was accepted before the
  collector existed: the assoc key-handle table (Runtime.h `nkeyhandles`) closed
  the last gap, so a key containing a class handle traces too. The collector is
  unconditional -- `--gc` is gone and only its cadence and budgets are on the CLI
  (`--gc-stress`, `--gc-stats`, `--gc-nursery`, `--gc-heap-max`,
  `--gc-full-every`); docs/design/garbage-collection.md 12a records why.

- [x] **Polymorphism** — `virtual` **functions** with dynamic dispatch (LRM 8.20)
  landed. Every class carries a descriptor pointer at object-body field 0
  (`class_body_ty`); each class registers a per-concrete-type dispatch
  table (`Module::set_class_desc`) whose slots are keyed by method NAME per hierarchy
  (base virtuals 0..k-1, an override — implicit or of a pure prototype — reuses the
  base slot of the same name, a new virtual appends, `compute_vtable` in Lower.cpp).
  A virtual call through a handle lowers to the new `CallVirtual` IR op (loads the
  callee from the object's vtable, `lower_virtual_call_inst`); a `super.method()`
  binds statically (detected syntactically). **Abstract classes** (`virtual class`,
  LRM 8.21) with concrete virtual methods lower normally; a direct `new` is
  rejected. **Pure virtual** methods (LRM 8.21) dispatch on the concrete type
  through an abstract base handle, with and without arguments, and via an internal
  call — the name-keyed slot matches the concrete override (which slang does not
  link to the prototype via `getOverride()`), and an abstract class's un-overridden
  slot is filled by a stub whose signature is rebuilt from the `MethodPrototypeSymbol`
  (slang's associated subroutine is degenerate: null `thisVar` / `returnValVar`,
  empty `getArguments()`); the stub is never executed. A virtual method returning a
  **non-scalar** (string / struct / queue / dynamic array) dispatches like any
  virtual call — the call-temp scanners recognize `CallVirtual`. Covered:
  override, inherited body, internal call, `super`, derived-introduced slot,
  abstract + pure with/without args, implicit override, multi-level abstract.
  A **virtual task** dispatches on the concrete type over the task (coroutine) ABI
  — the vtable slot holds the task ramp pointer, `TaskCallVirtual` loads it and
  drives the coroutine exactly like a direct `TaskCall`; time-consuming (`#delay`)
  virtual tasks and non-virtual tasks calling virtual ones internally work. The
  `CallVirtual`/`TaskCallVirtual` signature source is `function_id(sub)` (concrete)
  or the prototype stub (pure), NOT the receiver's vtable — so it is available even
  while the receiver class's own vtable is mid-computation (a virtual method whose
  body virtual-calls a sibling). A **pure virtual task** dispatches over the task
  (coroutine) ABI too: slang types a task prototype's return as `void`, so the stub's
  existing void body is already right and the only task-specific part is the `is_task`
  flag on its `Function` record, which is what selects the
  `(state, rng, wheel, <formals>, record)` signature a `TaskCallVirtual` drives the
  override's ramp through. A **pure virtual with a non-scalar return** reuses
  `build_function`'s fall-through return shape, so the stub's signature is the one
  every override was built with: an unpacked struct / fixed array and a container
  (queue, dynamic array, assoc) `Ret` their frame lvalue for the backend's trailing
  return out-param, while a string handle and a class handle `Ret` a loaded value. The
  stub stays never-executed (an abstract class is never `new`'d, so its vtable is
  never materialized). Covered: a pure virtual task with an argument,
  time-consuming and internally enabled; string, unpacked-struct and queue
  returns.
  An **`extern virtual`** method (LRM 8.24) dispatches too: the class member is the
  prototype and the body sits outside the class, so it is not a member
  `SubroutineSymbol` and a table filled only from members left the slot empty —
  reported as an internal error, since the un-overridden slot then found no pure
  prototype to stub. `compute_vtable` now also walks each class's
  `MethodPrototypeSymbol`s and fills the slot from `getSubroutine()`, the
  out-of-block implementation; a prototype with none is pure and still gets the
  stub. Covered: an abstract root whose extern body is the slot's implementation, a
  derived class overriding one extern with another and inheriting the other, a leaf
  overriding an inherited extern with an inline body, functions and tasks.
  A pure virtual returning a by-value aggregate that owns heap (a string member)
  works too, now that the return ABI's copy is per-leaf.
- [x] **Parameterized classes** (`class C #(...)`, LRM 8.25) — type and value
  parameters, param-sized members, methods over the type parameter, inheritance
  from a specialization (`extends base#(5)`), per-specialization static members,
  and a value parameter read via an instance (`obj.param`, LRM 8.5) or scope
  resolution (`C#(N)::param`). slang monomorphizes each `C#(args)` into a distinct,
  fully parameter-substituted `ClassType` (`genericClass` points to the generic
  definition), so each specialization lowers as an independent nominal class —
  distinct members, own-FunctionId methods, own static signals, keyed per
  `ClassType`. Lowering needed only the guard lifted plus folding an `obj.param`
  member to the specialization constant. (Cross-module
  specialization identity inherits the existing per-Lowerer class-cache limitation
  below.)
- [x] **Interface classes** (LRM 8.26) — `interface class` as a type and a handle
  type, `implements` (one or many, on a class or a `virtual class`), `extends`
  between interface classes including diamonds, dispatch through an interface-class
  handle (functions and time-consuming tasks), an implementation supplied by a base
  class (8.26.2), partial implementation in a `virtual class` (8.26.7), parameterized
  interface classes, `::` typedef / parameter access (8.26.3), `$cast` in every
  direction that involves one (8.26.5), and `randomize()` through an interface handle
  (8.26.9). An interface class interns as an ordinary nominal `Kind::Class` Ref with
  an EMPTY field table — no object, no display id, no vtable, no copy or
  randomization body. Interface method NAMES get one design-wide index (name keying
  IS 8.26.6.1's "one implementation satisfies every same-named prototype"), and each
  implementing class's descriptor grows an interface-method table indexed by it plus a
  bitmap of the interface ids it implements, both at design-wide constant negative
  offsets BELOW the trace-map pointer — so dispatch is one vptr load plus one
  constant-offset load (identical to an ordinary virtual call), the implements test is
  one load and one AND, and the GC's map offset and its startup cross-check are
  untouched. Both regions are emitted only in a design that declares an interface
  class: `--emit llvm` is byte-identical otherwise (measured). The interface closure
  unions `getImplementedInterfaces()` over the whole base chain, because slang's
  flattened list stops at the class carrying the `implements` clause — the bug
  Verilator 5.050 ships on 8.26.2's own example. slang's `DynamicCastConst` warning
  is dropped for an interface operand: its "always fails" condition is wrong on
  8.26.5's own legal `$cast(get_ref, put_ref)`. **Refused, loud:** `rand_mode`
  through an interface handle (8.26.9 makes it illegal, slang accepts it). 19 cases
  in `tests/class_test.cpp` plus a GC case in `tests/gc_test.cpp`. See
  docs/design/interface-classes.md.
- [~] **Class scope & nesting** — out-of-block method bodies (`function C::f();`,
  LRM 8.24) work, in a module or a package scope, for functions, tasks, `static`
  methods, `new`, and `virtual` (see Polymorphism above). Remaining: nested classes
  and `::` on parameterized specializations.
- [x] **`$cast`** (LRM 6.24.2 / 8.16) — all destinations. A numeric `$cast`
  converts and always succeeds (returns 1); an enum `$cast` succeeds only if the
  source is a legal member value (runtime `===` membership over `enum_members`);
  a class-handle `$cast` is the dynamic down-cast. Every class carries a
  descriptor pointer at object-body field 0 whose **ancestor display** (Cohen's
  display, `display[depth] == id`, one load and one compare, ids unique but
  unordered so nothing needs a whole-design numbering pass) answers the test. Only
  LRM 8.16 case 2 reaches run time: a cast to the source's own class or an
  ancestor of it, and a literal `null` source, always succeed; two unrelated class
  types always fail — a fold that holds only for a pair of ORDINARY classes, since
  an interface class on either side lets a common implementer relate them
  (LRM 8.26.5). A source that is null at run time succeeds and assigns null
  (8.16 is ambiguous; the reading is recorded at `lower_cast` and in
  docs/design/classes.md). An **interface-class destination** tests the object's
  implemented-interface bitmap instead of the display (`ClassImplements`); an
  interface-class SOURCE needs nothing new, since the object behind the handle is
  always a real class (docs/design/interface-classes.md §6). The function form
  returns the 1/0; the task form reports a run-time error on failure, for an enum
  destination as well. All of it dispatches through `lower_cast`. Deferred: a
  `chandle` destination (guarded loud — a chandle carries no type identity).

### Constrained randomization

- [ ] **Constraint blocks** — relational, `inside`, implication `->`, `if/else`,
  `foreach`, `solve … before`, `dist`, `soft`, uniqueness `unique{}`. Guarded loud,
  each by its own message: a `constraint` block in a class, an inline `with { … }`
  on either `randomize` form, and `constraint_mode`. The generated `$randomize` body
  is shaped around the seam a solver drops into
  (docs/design/constrained-randomization.md §7).
- [x] **`rand` / `randc` and `randomize()`, unconstrained** (LRM 18.3/18.4/18.6/18.8/
  18.11/18.12/18.14.2) — a class carrying a `rand` property grows a rand block of
  ordinary fields (its own xoshiro256++ state, a 64-bit rand_mode mask, a 256-bit
  cycle bitmap per `randc`), and two reserved vtable slots alongside `kCopySlot` hold
  per-class synthesized bodies: `$randomize(this, mask)` and
  `$randops(this, op, arg)`. The mask is a filter the body intersects with the
  object's own mode word, so one body serves `randomize()`, `randomize(x, y)`,
  `randomize(null)` and the recursion into a `rand` handle. Draws come from the
  OBJECT's generator, seeded at `new` from one `$urandom` draw of the creating thread
  (LRM 18.14.2) — which is also why no analysis pass changed: the seeding draw is an
  ordinary `Urandom` instruction, so a process that constructs a randomizable object
  already owns its frame state and leaves the thunk path. An unconstrained field is
  uniform over its full range, an enum over its distinct labels, a 4-state field
  never x or z; a `rand` handle is recursed into, never replaced, and a null one is
  skipped. `pre_randomize` / `post_randomize`, per-property and object-level
  `rand_mode`, `srandom`, `std::randomize` (which draws from the calling thread — no
  object) and `randc` all land with it. Three IR tags — `ObjRandom`, `ObjRandSeed`,
  `RandcNext` — all taking lvalue operands so the backend needs no layout knowledge,
  plus one runtime helper, `rt_randc_next`. Guarded loud: `randc` wider than 8 bits
  (LRM 18.6 allows the cap), `rand real`, `rand` dynamic arrays / queues /
  associative arrays / unpacked structs, more than 64 rand properties in one chain,
  `randomize(x)` naming a non-rand property, and `rand_mode` on a subelement. Design:
  docs/design/constrained-randomization.md.
- [x] **`$urandom` / `$urandom_range`** (LRM 18.13) — per-process xoshiro256++
  RNG in the coroutine frame (thread-stable by process index), inlined draw +
  Lemire range map. Stream intentionally not iverilog-identical (LRM
  §18.13 leaves the algorithm unspecified). Legal anywhere: a function or task
  runs in its caller's thread, so the state pointer is an implicit parameter of
  the call ABI (`declare_function`) and a draw inside a subroutine continues the
  calling process's stream.
- [x] **`randcase`** (LRM 18.16) — weighted random branch selection: each weight
  evaluated once, summed as unsigned at the widest weight's precision, one
  `$urandom_range` draw picking the first branch whose cumulative weight passes
  it. A zero-weight branch never runs and an all-zero sum runs none (the LRM's
  warning is optional). The fixed-precision sum wraps per the literal LRM
  reading, where verilator widens instead. Guarded loud: a
  weight wider than 32 bits (needs the LRM's several-draw stitching).
- [x] **Random stability across threads** (LRM 18.14.1) — a body that can outlive
  the site that ramped it owns its generator instead of sharing the pointer: a
  fork child, and the deferred `$strobe` / `$monitor` / action bodies. Each is
  seeded at its ramp from one draw of the ramping thread's state, so a child's
  stream is reproducible and independent of its siblings'. A DPI export thunk and
  the `$monitor` ramp in `main` have no calling thread at all and use a
  module-wide state seeded at startup.
- [x] **`srandom` / `get_randstate` / `set_randstate`** (LRM 9.7, 18.14) — the
  `process` methods: the thread's generator state rides its control block, so all three
  reach a thread through a handle. `srandom` is `$urandom(seed)`'s splitmix64
  expansion; the state renders as 64 hex digits and round-trips. A design that uses any
  of the three gives every thread its OWN generator (the target of `p.srandom()` is not
  statically known), gated so a `$urandom`-only design keeps the ownership
  `build_rng_fns` computes. A foreign string is a located fatal, LRM 18.14.4 leaving it
  undefined.
- [ ] **Random stability & seeding, remaining** — the OBJECT-side randstate pair
  (`obj.get_randstate()` / `obj.set_randstate()`), which shares the two `clotho.str`
  helpers the `process` trio above now provides and stays guarded loud until it uses
  them. The per-object RNG for class `randomize` and `obj.srandom()` landed with
  randomization; the thread-side trio landed with `process`.

### Concurrency & synchronization

- [x] **`wait fork`, `disable fork`** (LRM 9.6.1 / 9.6.3) — both landed with the
  `process` control blocks, which is where the live-descendant set they needed came
  from: `disable fork` is `kill` over the caller's subtree with the caller spared, and
  `wait fork` is the one-pointer test `first_child == null`, exact because a completing
  thread re-parents its live children (so a grandchild of a finished child still
  counts, as 9.6.3 requires). Both stay refused inside an assertion action block,
  which runs on the MONITOR's thread rather than one of its own.
  Design: docs/design/process-class.md §8.
- [x] **`std::semaphore` / `std::mailbox #(T)`** (LRM 15.3 / 15.4) — both are
  ordinary class objects whose synthetic body carries the resource and its wait
  queues: a signed 64-bit key count for `semaphore` (15.3.1 lets the count grow past
  the constructor's), and for `mailbox` an UNBOUNDED queue of T plus three queues of
  waiters (receive, send, peek). The bound is enforced in emitted IR, NOT by the
  queue type, whose overflow silently drops the far end instead of blocking. So the
  whole payload surface — int, string, class handle, 96-bit vector, unpacked struct
  with a string leaf, nested queue — comes from the container element descriptor with
  no mailbox-specific code, as does GC tracing of a handle payload.

  New surface is two IR tags: `SyncWait` (parks on a queue, -> 1 when it resumed as
  that queue's head) and `SyncCtl` (`Leave` / `WakeHead` / `WakeAll`); everything
  else expands in the frontend into ordinary IR over the body. No runtime symbol, no
  pool, no ABI change, no `DesignUses` gate. The waiter node is frame-resident and
  STAYS linked across an unsatisfied retest — that is what preserves LRM 15.3.3 /
  15.4.5 arrival order, and it is why the abort test lives inside `SyncWait`: a retry
  loop never reaches the end of its instruction, the same shape that made a killed
  `await` unkillable. A thread killed while blocked in `get` / `put` / `peek`
  terminates, and a suspended one does not consume.

  Head-of-line admission per 15.3.3 and Accellera's Interpretation #1: a blocked head
  blocks a later smaller request, where Verilator 5.050 serves the later one and then
  deadlocks the head — so those cases are pinned from the LRM text. `peek` is exempt
  (it consumes nothing) and a `put` wakes every peeker before the receive head, which
  is the only order that makes 15.4.7's broad unblock observable. `try_*` stay
  availability-only (15.3.4, 15.4.4/6/8).

  Guarded loud, each with a TODO: the **typeless mailbox** (15.4.9's `dynamic_type`,
  legal SV that slang type-checks) — it needs a dynamically typed element plus a
  run-time type identity for 15.4.5's check and 15.4.6's negative return;
  `mailbox #(event)` (an element's construction mints an event object the copy then
  overwrites, and event objects are never freed); deriving from either built-in
  class; and a **negative bound**, which 15.4.1 calls illegal and which reports at
  run time since the bound need not be constant. 22 cases in `tests/sync_test.cpp`.
  `std::weak_reference` (LRM 8.30) keeps the std-package guard alone.
  Design: docs/design/mailbox-and-semaphore.md.
- [x] **`process` class** (LRM 9.7) — all nine methods: `self`, `status`, `kill`,
  `await`, `suspend`, `resume`, `srandom`, `get_randstate`, `set_randstate`. A
  thread's control block is native, untraced storage in two tiers -- a static slot per
  process (no allocation, a constant address, the starting state in the initializer)
  and a pooled record per fork child, the only unbounded tier -- and a `process`
  handle is an ordinary class handle onto it, created lazily at the first `self()` and
  memoized so identity is a pointer compare. Identity travels the call ABI beside
  `rng`, because a task or function is not a thread.

  `kill` is the landed `disable` abort per thread rather than per static target: mark
  the subtree, force-wake each exactly once through the shared claim-or-flag protocol,
  and let every activation leave through its ordinary exit -- so a thread parked behind
  a time-consuming task call unwinds its whole frame chain, which is the case remote
  `disable` still refuses. `suspend`/`resume` desensitise nothing (the wheel cannot
  cancel, a static bucket cannot compact): a suspended thread replays the wake it
  already received, which is what makes `resume` continue it in the current time step.

  Everything is behind `DesignUses::process`, verified byte-identical `--emit llvm` on
  every design that does not name it. `std::weak_reference` keeps the std-package
  guard, now keyed by class name. 27 cases in `tests/process_test.cpp`, clean under
  ASan/UBSan/LSan. Design: docs/design/process-class.md.
- [~] **Clocking blocks** (LRM 14) — `clocking`/`endclocking` elaborates in a
  module, an interface (so a modport-scoped block comes free) or a program, and
  each block becomes one `ir::Activation::Observed` process that samples every
  input clockvar and then toggles the block's event, in that order (LRM 14.13).
  Putting it in Observed is what makes `@(cb)`, `@(cb.sig)` and
  `@(posedge cb.sig)` ordinary waits: an Observed store wakes an Active process,
  and the drain re-settles, so a waiter runs in the first Active region after
  Observed exactly as LRM 14.10 says. A clockvar is its own storage registered
  under the `ClockVarSymbol` in `sigmap_`, NOT an alias of the sampled signal, so
  expression lowering is untouched. `input #1step` (the LRM 14.4 default) reads the
  Preponed shadow the assertion sampler already maintains — one copy per variable,
  shared (`sampled_shadow_of`) — and an explicit `input #0` reads live in Observed.
  Synchronous drives (LRM 14.16) evaluate their RHS immediately into per-clockvar
  pending state and are applied by a per-block `ir::Activation::ReNba` process
  (new region, `emit_renba_run`), which is where 14.16 puts the value change and
  the only point late enough to see a drive issued by a `@(cb)` waiter or by a
  program's Reactive code. `##n` / `##0` (LRM 14.11) wait on the default
  clocking's event, with an integral count only — 14.11 wants "a non-negative
  integer value" and slang's `requireIntegral` rejects a real one, so lowering
  keeps it as an invariant (`internal: a real cycle delay count reached lowering`)
  rather than a gap, unlike a repeat count, which 12.7.2 admits as real.
  `default clocking` / `global clocking` / `$global_clock`
  supply an assertion's leading clocking event (LRM 16.16(a), 14.14). See
  `tests/clocking_test.cpp` and docs/design/clocking-blocks.md.

  Refused loudly, each with a TODO: any **input skew** other than `#1step`/`#0`
  and any **output skew** other than `#0` (a nonzero skew samples or drives at a
  time the edge does not name, so it needs a per-clockvar history keyed by time,
  and an edge-based skew is a second event per block); a **net-typed output**
  (LRM 14.16 wants a `'z`-initialized `(strong1, strong0)` driver joined to the
  net's resolution, not a direct write); `cb.sig <= ##n expr` (one pending slot
  per clockvar cannot hold two drives maturing in different cycles, which 14.16.2
  requires — it needs a ring indexed by cycle offset); a **bit-select or slice**
  drive target (the pending value is one whole-signal store); a **clocking item
  bound to an expression** rather than a signal (LRM 14.5 slices and
  concatenations); a non-posedge/negedge or multi-signal **clocking event**; an
  **assertion item declared inside a block** (LRM 14.3 with 16.16(b)'s rules); and
  a **sampled value function in procedural code** (LRM 16.9.3), which needs
  history rings keyed to the default clocking rather than to a monitor.
- [~] **Program blocks** — `program`/`endprogram` elaborate and simulate. A
  program instance goes through the ordinary instance path (own scope,
  variables, functions, non-suspending tasks), and its `initial` procedures run
  in the Reactive region per LRM 24.3 (`ir::Activation::Reactive`, seeded onto
  `@clotho.reactive`). A program initial is a real process, so it may suspend on
  `@`, `wait` or a nonzero `#delay` and resumes back into Reactive — each
  suspend wakes in Active, re-queues onto the reactive ring and suspends again
  (`emit_suspend`), because a waker does not know its sleeper's region. The LRM
  24.6 implicit `$finish` (simulation ends when the last program initial
  completes) is implemented as a countdown of live program initials. The region
  order against an Active-region cascade is covered by
  `tests/sva/diff_program_ref.sv` and `tests/program_test.cpp`.

  Before this, `program` was accepted and silently lowered as a **module** —
  Active-region initials, no implicit finish — a silent miscompile that
  predated the region set. Remaining gaps, all guarded loud: program **ports**
  (24.3 clocking/skew semantics unimplemented), **`$exit`** (24.5, needs program
  termination), **`#0`** inside a program (would resume through Inactive rather
  than Re-Inactive, 4.4.2.7), and a **suspending task** called from a program
  (the re-entry would push the task's handle, not the caller's). `always` in a
  program, nested programs and anonymous-program members are rejected by slang.

### Concurrent assertions (SVA)

- [~] **Sequences & properties** — `assert`/`assume`/`cover property` compile,
  simulate and report: `@(posedge|negedge clk)` written explicitly, inferred
  from the enclosing procedure (LRM 16.14.6), or taken from a clocking block —
  `default clocking` (LRM 16.16(a)), `global clocking` / `@($global_clock)`
  (14.14), or an `@(cb)` that names one (16.14.6(c)(1)) — plus `disable iff`,
  `|->`/`|=>`, `##n`/`##[m:n]`, `strong()`/`weak()`, and arbitrary boolean guards
  (lowered through negation normal form, so `!` never reaches libsva). Sampled
  values come from a Preponed region (`ir::Activation::Preponed`) that shadows
  each assertion variable; the automaton step is emitted as ordinary IR, so there
  is no new opcode. The region mapping is now the one LRM 4.4.2 specifies: the
  monitor runs in Observed (`ir::Activation::Observed`) and its pass/fail action
  block is scheduled into Reactive (`Inst::Tag::ReactivePend`), never run inline.
  Action blocks work. The SEQUENCE layer (LRM 16.9) is COMPLETE: `##n`/`##[m:n]`,
  `or`, `intersect`, `and`, `throughout`, `within`, `first_match`, repetition
  `[*]`/`[=]`/`[->]`, and named `sequence`/`property`/`let` declarations all
  lower, exercised cycle-for-cycle by the fixtures under `tests/sva/`.
  The PROPERTY layer (LRM 16.12) is now PARTLY done:
  implication, a bare sequence property and `strong`/`weak` around one, plus the
  whole BOUNDED group — `nexttime`, `nexttime[n]`, `always [m:n]`,
  `eventually [m:n]`, each with its `s_` form, `#-#`/`#=#`, property-level
  `and`/`or`, and the `if`/`else` and `case` forms (desugared to a disjunction of
  followed-bys, which is the shape that makes an untaken branch contribute
  nothing instead of holding vacuously) — plus the ABORT group: `accept_on`,
  `reject_on`, `sync_accept_on` and `sync_reject_on`, including the abort ABOVE an
  implication, which is the shape every real use writes. Covered by
  `tests/sva/diff_{nexttime,alwaysrange,eventuallyrange,followedby,propif,abort}.sv`,
  plus `diff_strongprop.sv` and `diff_propcase.sv`.
  An abort is scoped to a subtree rather than the whole monitor, so
  `ir::AssertionMonitor` carries an abort table (root, accept-vs-reject,
  sampled-vs-live, resolves-attempt, covers-trigger, state mask) and the emitted
  step has arms for it. Three things worth knowing: an accept DISCHARGES an
  attempt where `disable iff` abandons it with no verdict; the plain forms read
  live values like `disable iff` and carry a one-bit latch so a pulse strictly
  BETWEEN two ticks is still seen; and an abort covering a trigger resolves at
  MATCH time, the cycle the antecedent match ends, which for `|=>` is one cycle
  before the obligation would be seeded. Where two aborts fire on one attempt the
  outermost wins, so the abort list is scanned backwards.
  Remaining (each guarded loud): UNBOUNDED `always`/`eventually` and the `until`
  family (phase 3 — the obligation has no bounded length, so the number of
  attempts pending at once is unbounded); `not`, and with it `implies` and `iff`
  (phase 4 — a complement needs the operand determinised and swaps the weak/strong
  disposition); an abort covering an implication whose ANTECEDENT takes more than
  one cycle (several attempts can be part-way through it when the condition fires,
  each owing its own verdict, and the trigger is their union rather than a
  per-attempt ring, so the count cannot be recovered — one-cycle antecedents are
  exact and lower);
  `disable iff` inside a named property, local variables and sequence match
  items, recursive properties, a repetition with a zero lower bound (it admits
  the empty sequence, which libsva bans), goto/non-consecutive repetition over a
  compound sequence, multiclock, hierarchical or non-packed sampled variables,
  and an action block containing a nonzero delay / event control. A plain nonblocking
  assignment in an action block now works, committing in Re-NBA (LRM 4.4.2.8).
  An `and` in a consequent is refused where an `intersect` is accepted, because
  `and` runs to the later of its two ends and so needs both operands bounded;
  the diagnostic names that. A property operator whose operand raises an
  obligation of its own is refused too — a monitor has one trigger and nowhere to
  put a second — except over a `nexttime[n]` or an `always [m:n]`, where libsva
  moves the shift onto the antecedent instead. See
  docs/design/concurrent-assertions.md and docs/design/sva-property-layer.md.
- [x] **Sampled value functions** — `$rose`, `$fell`, `$stable`, `$changed`,
  `$past`, `$sampled`, including `$past(v, n)` for `n > 1`. `$past` is the
  primitive: a per-monitor history ring shifted once per CLOCK TICK at the end of
  the monitor body, as deep as the largest constant `n` asks for; the others
  desugar onto it. Ring entries are seeded with the variable's time-zero value,
  NOT the type default: before enough ticks have elapsed the only value that has
  actually been sampled is the one at time zero, and seeding the type default
  would manufacture a change that never occurred. Covered by
  `tests/sva/diff_past.sv` and `tests/sva/diff_valuechange.sv`. Still refused,
  each guarded loud: a gating expression or an explicit clocking argument on
  `$past`, an explicit clocking argument on a value change function, a
  non-constant tick count, any of the six
  over an expression rather than a plain variable, and any of them outside an
  assertion expression. In a `disable iff` or asynchronous `accept_on` /
  `reject_on` condition `$sampled` now lowers — LRM 16.9.3 blesses it there and it
  resolves to the Preponed shadow alone, touching no history ring and no
  per-attempt state — while the other five stay refused, because 16.9.3 infers no
  clocking event in that position and 16.12.14 requires an explicit clock argument
  for them, which is the gap above; slang diagnoses the unclocked
  disable-condition spelling itself. See docs/design/concurrent-assertions.md §4.1.
- [~] **Property layer phase 3** — DONE. Unbounded `always p`, `s_eventually p`
  and all four `until` forms lower and simulate, tracked by a new COUNTED mode:
  the obligation is determinised totally and each of its states carries a
  `uint64_t` count of the attempts sitting there, so a verdict says how many
  attempts it belongs to. `libsva/test/merge.zig` (`zig build merge`) is why it
  is shaped that way and not as the single merged vector the design doc first
  proposed: over every trace of 6 and 8 cycles a merged vector is unsound (a
  two-cycle counterexample on a bounded `a ##1 b`), `first_match`'s
  determinisation does not rescue it because it is partial and a dead attempt has
  no state to be seen in, and only the counted total construction is exact on
  cycle sets, counts and end-of-trace pendency at once. `tests/sva/diff_until.sv`,
  `diff_alwaysinf.sv` and `diff_seventually.sv` carry hand-derived expectations
  that clotho now reproduces exactly, including `diff_seventually.sv`'s six cover
  hits where a merged vector would have scored two. The goldens in those files
  are still `TO BE FILLED`, so the expectations there remain hand-derived.

  Two refusals remain from this work, both loud and both narrower than the old
  unbounded refusal they replace. A property mixing disciplines —
  `always (a |=> b)` and friends — is `SVA_ERROR_MODE_MIX`, by decision: tracking
  mode is monitor-level. And the operand of an unbounded `always`, or the LEFT
  operand of an `until`, must match in a single cycle, because both positions are
  universal over cycles and "p holds at every cycle of the window" is a statement
  about overlapping attempts of p that one counter cannot carry. The fix is a
  second subset construction over the determinised operand, where a dead
  sub-attempt becomes a visible state; cost is exponential in the operand and
  capped like every other blow-up. Multi-cycle operands in the EXISTENTIAL
  positions — `s_eventually p`, an `until`'s right operand — work today.
- [~] **Property layer phase 4** — `not` is DONE; `implies` and `iff` are
  structurally limited. The complement is the total determinisation with two
  masks exchanged: `last` becomes the dead (empty-subset) states, and the states
  whose subset meets the operand's exits get a `never` guard, so nothing in
  `Monitor` or in a backend knows a `not` was involved and `sva_report` needs no
  new field. A complement does NOT force counted mode — the operand-matched
  states are deleted, so the ordinary emptiness reading is restored and the ring
  holds it. `not pending == pending` is what cost real work: ordinary
  co-reachability pruning would resolve a pending verdict at the cycle the
  outcome became inevitable, so `prune` gained an exemption mask, pinned by
  `not (a ##1 (b or !b))` and by `not not p` agreeing with `p`.
  `tests/sva/diff_not.sv` reproduces an independently hand-derived LRM
  expectation exactly: 28 reports across seven ticks, four of them at end of
  trace, and seven cover hits.

  What is limited, and it is not conservatism: `implies` reduces to
  `(not p) or q`, `not` inverts the strength of the operand it complements, and a
  monitor carries ONE end-of-trace disposition — so `implies` lowers only when
  its operand strengths DIFFER, and `iff` never lowers, since each direction
  forces its operands opposite and the outer conjunction always sees a mixed
  pair. Weak-implies-weak, which is the default spelling, is therefore refused.
  Not conservative: with both operands weak, a trace where `q` definitively
  failed while `p` is pending ends with the implication FAILING, and a trace
  where both are pending ends with it HOLDING — one automaton, one strength bit,
  two dispositions.
- [ ] **Retire `MIXED_STRENGTH`** — the lift that makes `implies` and `iff`
  usable, and the one thing standing between phase 4 and complete LRM 16.12. A
  per-state end-of-trace disposition, OR-combined over the live vector and folded
  conjunctively at product-construction time, replaces the single strength bit.
  It changes what `sva_monitor_strength` means, so it is an ABI decision and not
  a refactor. Three separate refusals point at it.
- [ ] **Determinise the trigger** — what would restore an unbounded antecedent
  (`b[=2] |=> c`, `(a ##[1:$] b) |-> c`), refused since the trigger began
  counting attempts. The bound is not about counter width: the
  trigger is a nondeterministic Glushkov automaton, so one attempt is a SET of
  live states, "how many attempts sit in this state" is not well formed, and
  identity is needed because two threads of one attempt can reach exits together
  where LRM 16.9 has ONE match — set union is idempotent, addition is not.
  Determinised, an attempt is a single state and a plain count is exact with no
  bound at all. Reuses `Glushkov.determinise` but a trigger must keep running
  past a match, needs its own discipline flag and tables in the C ABI, and wants
  the subset blow-up capped under `state_limit` like `first_match`.
- [ ] **`expect`** — blocked behind giving an assertion action block a real
  coroutine so it can suspend and resume in the Reactive region. That one
  scheduler change is what lifted most of the action-block whitelist (a delay, an
  event control, `wait` and a `fork` are all allowed now); a user task call is the
  entry still refused, since the body is not traced for what it may suspend on.
  Not an SVA problem; see concurrent-assertions.md §9.3's whitelist.
- [ ] **`.matched`** — needs multiclock (LRM 16.13.6). `.triggered` on a plain
  same-clock sequence does not, and is the near-term half.
- [x] **Assertion control** — `$asserton`/`$assertoff`/`$assertkill`, the four
  pass/fail action tasks, `$assertnonvacuouson`/`$assertvacuousoff` and the
  constant-argument subset of `$assertcontrol` (LRM 20.12), over concurrent
  assertions. Per-assertion state in negative logic, addressed by scope or by
  label with `$dumpvars` `levels` counting; `off` gates the seed alone so an
  attempt in flight still reports, `kill` rides the `disable iff` reset, and the
  action filters gate the pend without touching the automaton. Still open:
  non-constant arguments (refused, and they need a run-time dispatch over every
  assertion in scope), control over immediate/deferred/`expect` assertions
  (refused when the design has one, since they lower inline with no state to
  gate), and `unique`/`priority` violation control.

**Deliberately deferred, recorded so it is not lost.** Each is a separate
project with its own algorithm, not tail cleanup on the property layer:

- [ ] **Multiclock sequences and properties** (LRM 16.13) — libsva's monitor has
  exactly one clock and the automaton model is "one clock edge is one cycle", so
  this needs a per-letter clock attribute, a rewrite of `advance`/`seed`/`satisfy`
  and of clotho's emitted step, plus a clock-domain partition in the frontend.
  Gates `.matched`, an explicit clocking argument on a sampled value function,
  and `@$global_clock` beside another clock. Roughly 1.5k lines. Its
  `default`/`global clocking` half is no longer blocked: clocking blocks landed
  (LRM 14), and a single `default clocking` / `global clocking` / `@(cb)` clock
  already resolves to an ordinary single-signal event, so what is left here really
  is the per-letter clock attribute and the automaton rewrite.
- [ ] **Local variables and sequence match items** — an attempt is a bitset of
  automaton states and a local variable is per-thread data; the merge that makes
  the bitset work is exactly what destroys the ability to carry a valuation.
  Needs a register automaton (determinise-with-data) in libsva plus per-slot
  storage in clotho's monitor state. The deepest item on the list.
- [ ] **Recursive properties** (LRM 16.12.4) — a fixpoint over the property
  graph, a different construction from the Glushkov lowering, not a tag. Tracked
  again under M5 language corners.

### Functional coverage

- [ ] **`covergroup`** — `coverpoint`, `bins` (explicit, ranges, arrays),
  transition bins, `wildcard`, `illegal_bins`/`ignore_bins`, `cross`.
- [ ] **Coverage methods & options** — `sample()`, `get_coverage()`,
  `get_inst_coverage()`, per-instance options, `option`/`type_option`.

### Foreign interface

- [ ] **DPI import `output`/`inout` string** — the C writes an unbounded
  NUL-terminated `char*` back with no size contract, so there is no safe fixed
  copy-out buffer (frontend `fail`).
- [ ] **DPI export `output`/`inout` string + string return** — returned-`char*`
  lifetime / the same unbounded copy-out (frontend `fail`).
- [ ] **DPI export open-array args** — needs open-array formals in the general SV
  function ABI (a runtime-bounded array parameter), not just the call-site marshal.
- [ ] **DPI open-array: packed-element dynamic-array actual + multi-dimensional** —
  a packed dynarray element needs a runtime-sized marshal scratch (backend guard);
  multi-dim needs `svGetArrElemPtr2/3` + a recursive descriptor. (A queue actual is
  blocked upstream in slang as a type mismatch, not implementable here.)
- [ ] **One c_identifier exported from several scopes** — `svSetScope` picking which
  instance a shared C name reaches. Blocked upstream: slang's `checkDPIMethods` keeps
  one cId->subroutine map and drops the duplicates before `getDPIExports()` is
  visible, so Clotho cannot even see the second scope. Needs walking the instances by
  hand, a `DpiExport` carrying a (scope, FunctionId) list, and a runtime scope-index
  switch in the thunk.
- [ ] **`svScope` handles for non-top instances** — a scope is registered per top, so
  `svGetScopeFromName` cannot name a nested instance. Exports themselves bind
  absolutely at lowering time, so this is only about the scope API's reach.
- [ ] **DPI import/export tasks** — a task can consume time; the native boundary is
  coroutine-shaped (import) / wheel-from-C (export). Functions only today.
- [ ] **`svGetCallerInfo` + the DPI disable API** — not provided; C calling one hits
  an unresolved symbol.
- [x] **DPI-C** — import (scalar/vec/chandle/string, in/out/inout, pure/context, 1-D
  open arrays in/out/inout at any element width, dynamic-array zero-copy actuals) and
  export V1 (C→SV, functions, scalar/chandle/real/vec/string args) landed, with the
  svdpi.h accessors (bit/part-select, one scope per top, open-array). See the M1
  Subroutines entry and docs/design/dpi-c.md.

### UVM enablers

- [~] **Virtual interfaces** (LRM 25.9) — a handle is the 64-bit address of one
  elaborated interface instance's runtime context record (the per-instance
  descriptor module dedup already builds: state base, NBA shadow base, sched base,
  dump-var base, wait-bucket bases). A member reached through it is the ordinary
  signal lvalue with that base substituted — the representative's def-local field
  index, the handle's instance — which is what a reused definition's own body
  already emits for itself. slang resolves `vif.sig` against a cached placeholder
  instance body, so the member symbol is matched to a real instance by
  `InstanceBodySymbol::hasSameType` (`VifDefs`), and that representative supplies
  every def-local constant. Declaration, `null`, `==` / `!=`, assignment from an
  instance / another handle / `null`, modport-qualified and parameterized handles,
  handles as class properties, constructor and task arguments and array elements,
  member reads and blocking writes with correct wake and VCD on the bound instance,
  interface events triggered through a handle, and `@(edge vif.sig)` /
  `wait(vif.x)` — which park a frame-resident node on an intrusive waiter list in
  the definition's state, the event-object list reused, so there is no bucket
  identity or capacity to know at compile time. Signals of an interface a handle
  can name keep the canonical type-derived shape, the `ref`-actual rule.
  **Nonblocking writes** (`vif.d <= x`) move the *member* — every writer of it,
  handle or direct — off the per-signal block shadow, which two handles naming twin
  instances would collide in, onto the deferred-record path, whose record carries
  the destination pointer resolved at statement-execution time plus the instance
  context the replay wakes against. A **clocking block** through a handle (25.9.1 /
  25.9.2) reads that instance's sampled storage and drives that instance's
  `drive_pend` pair, so its own Re-NBA process performs the drive and 14.16.2's
  last-write-wins falls out per instance. An **interface subroutine** through a
  handle is lowered once for the definition with the handle as formal 0
  (`Function::takes_iface_ctx`), installed body-wide by the backend, so one body
  serves every instance; a bare sibling call passes the context on. That body-wide
  install is why `CtxOverride` now carries an anchor and `def_relative_for` gates on
  it — a hierarchical reference out of the interface has a different `ctx_ty`. An
  event control on an **expression** over members is desugared into the sample /
  park / re-evaluate shape `wait` uses, never a continuous net (which would freeze
  onto whichever instance was bound at time zero); an `edge` / `iff` qualifier on
  such an expression stays guarded loud. A member of an interface the design never
  instantiates is an unconditional run-time fatal with a
  `virtual-interface-never-bound` warning up front, not a refusal. A member in a
  **generate block** of the interface resolves too — a generate block is not a
  separate instance, and the representative is found by ordinal path rather than by
  name. An `event` member is waited on through its own object, the mechanism its
  trigger fires. An NBA inside a subroutine reached through a handle takes the
  record path like any other handle write. The LRM 25.9 `SBusTransactor` example
  runs verbatim. Remaining, guarded loud:
  - [ ] A member of an interface **instantiated inside** the interface a handle
    names (`vif.i.d`) — a separate instance with a separate context record; needs
    the record to carry its children's. A handle taken directly to the inner
    instance works.
  - [ ] `virtual` handles in a **queue / dynamic / associative array** — the
    generic container-of-handles gap below, shared with class handles.

  Design: docs/design/interfaces.md.
- [~] **String formatting** — `%p` (LRM 21.2.1.6). The four structural `shall`
  cases the clause names beyond unpacked arrays/structs now print correctly
  instead of falling through to a bare decimal: a tagged union as `'{tag:value}`
  (`'{Valid:42}`, void member `'{Invalid}`), a plain union as its first declared
  member (`'{a:-85}`), a packed struct with named elements (`'{a:-85, b:42}`),
  and an enum as its enumeration name with a base-type fallback (`GREEN`, else
  `3`) — nested leaves included, via the slang `Type` threaded alongside the
  lowered `ir::Type` through the `%p` expansion. Leaves print in **decimal** at
  their own signedness, the clause's "as they would unformatted" (21.2.1.1's
  decimal default) — they used to be `'h<hex>`, copied from verilator, which is a
  `shall` violation. A real leaf prints `%g`, a string (variable or literal)
  quoted; both used to segfault or print the packed literal's decimal value.
  Also fixed: `%%p` in a format string
  corrupted the output (the splice did not skip `%%` escapes, so it substituted
  inside the literal and left the real specifier for the runtime formatter to abort
  on), and a `%p` operand was lowered twice (a call operand ran twice). Remaining:
  - [x] **`%p` of an aggregate CALL result** (`$display("%p", g())`) — the operand
    is expanded at lower time and needs an lvalue to walk, so a call used to be
    refused with an assignment-target message. The call's value is the caller-side
    out-param temp the callee wrote through, which is that lvalue; the call is
    lowered once for the whole leaf walk, so the double-lowering bug above is not
    reintroduced, pinned by a counter test. See the aggregate-return entry under
    the subroutine ABI.
  - [x] **`%p` of a class handle** (LRM 21.2.1.6) — **another implementation-defined
    form decided deliberately.** The clause describes `%p` over aggregate VALUES and
    a handle is not one; it says nothing about what a handle prints beyond `null` for
    a null one. Decision: a null handle prints `null`, any other prints its CLASS
    NAME, at **every depth** — a direct operand, a struct member, a container
    element. The uniformity is the point: it is cycle-safe by construction, and UVM's
    object graphs are cyclic (`uvm_component` holds `m_parent` and `m_children`), so
    expanding properties would not terminate without a depth cap the LRM does not
    define. Verilator 5.050 is itself inconsistent — it expands a direct operand
    (`'{x:'h5, i:t::Inner}`) but prints the type name for a handle inside a container
    (`'{t::C}`) — so there is no reference behaviour to match, only a choice to
    record. iverilog is no oracle either: it cannot compile a queue of class handles
    at all. The name is unqualified (a specialization carries its own arguments,
    `uvm_queue#(uvm_callback)`); Verilator writes `pkg::C`. Rendered by
    `handle_p_render` as a select over two string constants, so nothing dereferences
    the handle. A class-handle container ELEMENT routes the container through the
    loop render (`build_container_p_string`) rather than the flat runtime descriptor,
    which would have printed the pointer word. Needed by uvm_queue's
    `convert2string`, which is `%p` over a queue of handles.
    Follow-up: expand the properties of a DIRECT operand, the one depth where a
    cycle cannot be reached in one step, leaving nested handles as names.
  - [x] **`%p` of a chandle / event / virtual-interface handle** (LRM 21.2.1.6) — was
    **a refusal of a `shall`**, which is the part that makes this different from the
    class-handle entry above. The clause names five reference types in one sentence:
    "a chandle, class handle, interface class handle, event, or virtual interface
    shall print its value in an implementation-dependent format, except that a null
    handle value shall print the word **null**". So only the non-null form was ever
    open; printing nothing at all was never an option, and every
    `uvm_config_db#(virtual X)` in every UVM testbench hit it — `uvm_resource#(T)`'s
    `m_value_as_string` is `$sformatf("%p", val)` (uvm_resource.svh:93) and `T` is the
    virtual interface.
    Decision, extending the class-handle rule rather than inventing a second one: a
    null handle prints `null`, any other prints **the name of what it points at**, at
    every depth. A chandle prints `chandle` and an event prints `event` — LRM 6.14 and
    15.5 give those types no other name, the type IS the name. A virtual interface
    prints the interface **definition's** name (`virtual some_if` → `some_if`), taken
    from the placeholder body `vif_named_body` already resolves members against, so
    `%p` and `vif.sig` agree about which interface it is.
    Two more informative renderings were weighed and rejected. The interface
    INSTANCE's hierarchical path is the most useful thing a virtual interface could
    print, and it is what the class-name choice would look like if that choice were
    about identity — but it is not: the class arm prints a TYPE, so a path would make
    one of the five reference types disagree with the other four. It is also not cheap
    here: the handle is the address of an instance's context record
    (docs/design/interfaces.md) and there is no runtime address → path table, so a
    path would take a compare chain over every elaborated instance of the definition,
    emitted at every `%p`. A raw pointer value is rejected outright — informative but
    unstable across runs, i.e. useless in a golden file, which is the same reason the
    bug below was a bug.
    Fixed with it, and the reason the container depth was not merely a refusal one
    level down: `%p` of a container whose element is an EVENT printed the handle WORD
    (`'{1, 2}` for two events — a slot index, neither `null` nor a name, and not
    stable). The flat runtime element descriptor describes one integral / real /
    string, and an event handle is a 64-bit integral, so it took that path silently.
    `loop_render`'s class-handle test is now a handle test over all four kinds
    (`elem_is_handle`), and it needs BOTH halves: a class handle is decidable from the
    IR type, while a chandle is a plain unsigned 64-bit int there and only the AST
    tells it apart. An event queue / dynamic array / associative array is the one
    handle container the element type system admits today, so it is where that is
    pinned; a chandle or virtual-interface element is still refused by
    `guard_container_element`, which is the container-of-handles gap below and NOT a
    `%p` deficiency — and for a chandle 6.14's closed list of legal uses names an
    associative array but no queue or dynamic array, so the queue form is doubtful
    SystemVerilog in the first place.
    The RUNTIME-format path (`lower_dynamic_format`) keeps refusing a handle argument
    and the refusal is accurate as written: `rt_arg` carries one integral / real /
    string, and with the format unknown the frontend cannot tell which specifier the
    argument will meet. Pre-rendering the name as a string was considered and is
    unsound — `$display("%d", ch)` is accepted and prints the handle word, so a handle
    genuinely may meet a numeric conversion, and a pre-rendered name would be silently
    wrong text there.
  - [x] **`%0p`** (LRM 21.2.1.6) — a **separate specifier**, not a width applied to
    `%p`: the clause gives it its own sentence, "shall print aggregate expressions
    ... in a shorter, implementation specific form". slang draws the same line
    ("Pattern args allow the zero-pad specifier" — `%p` forbids every other width and
    left-justify, so those never reach Clotho and its own check is a backstop). The
    `0` reads as it does everywhere else in 21.2.1.2 — drop the `%d`-width padding on
    the integral leaves — and Clotho's `%p` already renders every leaf minimally,
    scalars included, so the shorter form **coincides** with the plain one. Recorded
    because the equality is a decision: if `%p` ever adopted padded leaves, `%0p`
    would have to keep the minimal ones. Verilator makes the two identical as well;
    iverilog cannot `%p` an aggregate at all (`vpiMemory`), so it is no oracle. The
    splice carries the spec's real length, since the token is 3 characters here and a
    fixed 2 left a stray `p`. Needed by uvm_get_to_lock_dap and friends.
  - [x] `%p` of an associative array (LRM 21.2.1.6) — renders
    `'{key:value, ...}` over the live entries in ascending key order. The one
    deficiency behind this was that `%p` had no ordered walk for a KEYED container:
    the escape hatch a container with a non-flat element already used
    (`build_container_p_string`) is driven by an index space, and a keyed table has
    none. `build_assoc_p_string` is the same loop keyed by
    `assoc_traverse(First/Next)` + `assoc_select`, i.e. the shape
    `lower_foreach_assoc_dim` already uses for `foreach`, so the value renders through
    the ORDINARY aggregate expansion and a struct / nested container / class-handle /
    wide value works for free. Keys print by the same leaf rules, so an enum key
    prints its name and a string key prints quoted. Ordering is asserted only for
    integral and string keys (LRM 7.8.5 leaves it implementation-defined otherwise).
    A direct operand takes the same path — `Assoc` is now in `expand_p_arg`'s
    container predicate, where its absence used to send `$display("%p", a)` down the
    singular path to die as an unrenderable handle.
    Retired with it: `guard_container_p_element`, whose four `%p` refusals (tagged
    union, class handle, element wider than 64 bits, nested aggregate) had all become
    unreachable when the loop render landed — every condition it tested forces
    `loop_render` — and whose one live caller was `guard_agg_eq_members`, i.e. an
    aggregate `==` refusal wearing `%p` wording.
- [ ] **Containers of handles** — queues/dynamic/associative arrays of class
  objects, and class objects holding them.

---

## M5 — Full LRM (IEEE 1800-2023)

### Net & primitive completeness

- [ ] **User-defined nettypes** (`nettype`) and resolution functions,
  `interconnect` nets.
- [ ] **Charge storage** — `trireg` with capacitance strengths, charge decay.
- [ ] **Sequential UDPs** — full edge-sensitive tables including `*`/`r`/`f` notation.

### Timing & back-annotation

- [ ] **Full `specify`** — edge-sensitive paths, state-dependent paths, conditional
  paths, `ifnone`, polarity, full timing-check set (`$recovery`, `$removal`,
  `$recrem`, `$skew`, `$timeskew`, `$fullskew`, `$nochange`, `$setuphold` with
  notifiers).
- [ ] **SDF annotation** (`$sdf_annotate`) and pulse-style controls.
- [ ] **Hierarchical / fractional timeunit & precision** — full `timeunit`/
  `timeprecision` per scope and precision-correct fractional-delay rounding.
- [x] **`$printtimescale` with a scope argument** (LRM §20.4 / §21.3.1.2) — the
  optional argument names an instance anywhere in the design (upward, downward or
  sideways), `$unit`, or `$root`; the target's timescale and hierarchical path are
  compile-time constants, so the task still folds to a literal `$display` with no
  IR or runtime support. A scope with no timescale now reports Clotho's unitless
  base (`1s / 1s`, byte-identical to iverilog) instead of failing.

### Configuration & checkers

- [ ] **Library map / `config`** — `library`/`config`/`design`/`cell`/`instance`
  binding, `liblist`/`use`.
- [ ] **Checkers** (`checker`/`endchecker`) — assertions, free variables,
  instantiation.
- [ ] **`randsequence`** — grammar-based stimulus generation.

### Language corners

- [x] **Pattern matching** (LRM 12.6) — Phase 2 of tagged unions
  (docs/design/tagged-unions.md). `case (x) matches` (`StatementKind::PatternCase`
  → first-match if/else chain), `if (x matches pat)`, and `x matches pat ? a : b`,
  all with `&&&` filters, over all five pattern kinds (Wildcard / Variable /
  Constant / Tagged / Structure). `lower_pattern` returns a 1-bit predicate and
  binds `PatternVarSymbol`s (declare_storage + store, the foreach-iterator
  mechanism); sub-values are extracted from the flat subject by shift/resize.
  The `?:` form single-evaluates its condition (11.4.11) across the branchless
  and lazy select paths. unique/priority/unique0 pattern case reports the same
  no-match / overlap violations as a plain case (LRM 12.5.3 wording).
  casez/casex `matches` (LRM 12.6.1) wildcard the z/x label bits over
  tag + member compares (pattern_cmp threads CaseEq/CasezEq/CasexEq through
  lower_pattern) — which is what 12.6.1 requires rather than a literal label
  compare. `unique` / `unique0 if (expr matches pattern)` works
  too: it was the third `matches` guard site (the Phase 2 plan named only two, so
  it survived undocumented and kept rejecting with `pattern matching in conditions
  is not supported`, a `&&&` filter failing one line earlier as an unsupported
  multi-condition `if`). Legal SV — LRM 12.4.2 attaches `unique_priority` to
  `if (cond_predicate)` and 12.6.2 makes a pattern a `cond_predicate`; `priority if
  (... matches ...)` already worked. `lower_unique_if` now collects each arm's whole
  condition list and lowers it through `lower_conditions`, the helper the plain and
  priority chains already use, evaluating every arm's predicate once up front —
  which 12.4.2 permits for unique/unique0, since overlap detection has to continue
  past the first true condition. Binding a non-selected arm's pattern variables is
  harmless: each is a pure sub-value extraction, read only inside the arm its own
  predicate guards. Same discipline as `lower_pattern_case_checked`. Follow-ups
  (gated loud): pattern case / structure pattern over an unpacked **aggregate**
  subject — an unpacked *tagged union* is not caught by that guard and does
  work, since it lowers to a flat integral — and Phase 3 non-integral
  unpacked-tagged member payloads.
- [ ] **`let` declarations** (full, including in sequences/checkers).
- [ ] **Recursive properties**. `global clocking` and `$global_clock` landed with
  clocking blocks (LRM 14.14); what remains of them is `@$global_clock` beside
  another clock, which is the multiclock item above.
- [ ] **Full constraint language** — array reduction methods in constraints,
  functions in constraints, `disable soft`, iterative constraints, constraint
  inheritance.
- [ ] **Coverage corners** — real coverpoints, type coverage, cross with `binsof`
  expressions, coverage merge/database semantics.
- [ ] **Attributes** (`(* ... *)`) — parse-through today via slang; expose where
  semantically meaningful.

### Complete system task / function library

- [ ] **Remaining query functions** — `$bits` on all types.
- [ ] **EVCD** — `$dumpports` family.
- [ ] **`$writememh` / `$writememb`**, address-ranged `$readmem*`.
- [ ] **PLA modeling tasks** (`$async$and$array`, `$sync$nand$plane`, …).
- [ ] **Stochastic / queue tasks** (`$q_initialize`, `$q_add`, `$q_remove`,
  `$q_full`, `$q_exam`).
- [x] **Probabilistic distribution functions** (`$dist_uniform`,
  `$dist_normal`, `$dist_exponential`, `$dist_poisson`, `$dist_chi_square`,
  `$dist_t`, `$dist_erlang`) and seeded `$random(seed)` — the IEEE reference
  algorithm (byte-identical to iverilog), inout-seed
  writeback shared with `$random(seed)`.
- [x] **`$system`** (LRM §20.17.1) — shell command execution via C `system()`,
  task and function forms. Refused by default; gated behind an explicit
  `--allow-system=const|full` operator opt-in with a compile-time gate plus a
  runtime backstop (a security-motivated deviation).
- [ ] **`$random` family completeness** and command/severity tasks
  (`$exit` semantics, `$stop` levels at `Lower.cpp:1021`).
- [x] **Conversion functions** — `$itor`, `$rtoi`, `$bitstoreal`, `$realtobits`,
  `$shortrealtobits`, `$bitstoshortreal`.

### PLI / VPI

- [ ] **VPI** — object-model access (`vpi_handle`/`vpi_get`/`vpi_get_value`), value
  change and simulation-phase callbacks, `vpi_put_value`.
- [ ] **Assertion & coverage APIs** (VPI extensions).
- [ ] **Compiler directives** — complete `` `define `` (with args),
  `` `ifdef``/`` `ifndef``/`` `elsif``, `` `include``, `` `line``, `` `resetall``,
  `` `default_nettype``, `` `celldefine``, `` `pragma`` (slang preprocesses;
  ensure each is honored semantically).
