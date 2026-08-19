# Constrained randomization, step 1: the unconstrained core (LRM 18)

Target: `rand` / `randc` class properties, `obj.randomize()`, `std::randomize(...)`,
`pre_randomize` / `post_randomize`, `rand_mode()`, `srandom()`, and the per-object
RNG of LRM 18.14.2 — everything that is reachable *without a constraint solver*.
Constraints themselves (`constraint` blocks, inline `with {}`, `dist`, `soft`,
`solve … before`, `unique {}`, `constraint_mode`) are refused loud by this step and
are the subject of step 2.

Status: **landed** (P0-P4; P5, the randstate codec, is not started). Three things
came out simpler than this document first specified, and the text below has been
corrected in place: the rand block is carried as ORDINARY class fields rather than a
backend layout change (§5), the three new opcodes therefore take LVALUE operands and
the backend needs no layout knowledge at all (§10), and the object-seeding draw is a
real `Urandom` instruction the frontend emits, so no analysis pass changed (§9).

This document owns the object layout, the dispatch mechanism, the per-field draw
rules and the random-stability model. `docs/design/classes.md` §8 defers
`rand`/`randomize` here; `docs/design/process-class.md` §7.4 owns the *thread*-side
`srandom` / `get_randstate` / `set_randstate` and states that the per-object RNG is a
separate, unblocked item — this is that item.

---

## 1. What step 1 is, and what it is not

Step 1 makes every randomization construct that has no constraint attached work
completely, and makes every construct that does have one fail loud and by name. The
split is deliberate: the solver is the part whose API is unknown, and it is the part
that touches the least code.

**In:** rand/randc declaration and layout; the built-in `randomize()` in all three
call forms; the argument-subset and `null` forms; `std::randomize` on locals;
`pre_randomize`/`post_randomize` including the LRM 18.6.2 chain; recursion into
`rand` class handles; `rand_mode()` in both the object-level and per-property forms,
both task and function spellings; per-object RNG seeded at `new` from the creating
thread; `obj.srandom()`.

**Out, and loud (§12):** any `constraint` block; any inline `with { … }`;
`constraint_mode()`; `dist`; `randc` wider than 8 bits; `rand` on a real, on an
associative array, or on more than 64 properties in one class.

**Not a phase here:** `process::self().srandom()` and the thread-side randstate trio
belong to `docs/design/process-class.md` §7.4. They share the two runtime codec
helpers with §14 P5 and nothing else.

---

## 2. Current state

### 2.1 What is refused

`src/Lower.cpp:2627-2630`, inside `lower_class_type`'s property-index pass, before
the property is given a field index:

```cpp
if (prop.randMode != slang::ast::RandMode::None) {
    // TODO: rand / randc properties and randomize() (Phase 3).
    fail("rand / randc class properties are not supported");
}
```

`obj.randomize()` and `std::randomize(...)` never arrive as user method calls: slang
rewrites any `MethodFlags::Randomize` subroutine into a system call
(`build-asan/_deps/slang-src/source/ast/expressions/CallExpression.cpp:107-121`), so
both reach the terminal `fail("unsupported system function '" + name + "'")` at
`src/Lower.cpp:17212`, or its statement-position twin at `src/Lower.cpp:15912`. Same
for the per-property `x.rand_mode(...)` and `c1.constraint_mode(...)`, which slang
also models as system methods (`NonConstFuncs.cpp:203-240`, registered `:666-669`).

None of these refusals has a test. `tests/class_test.cpp` contains no `rand`,
`randomize` or `constraint` case.

### 2.2 What is silently accepted

A `constraint` block is **ignored with no diagnostic**. `lower_class_type` enumerates
`membersOfType<ClassPropertySymbol>()` (`src/Lower.cpp:2621`, `:2637`) and
`membersOfType<SubroutineSymbol>()` (`:2719`); a `ConstraintBlockSymbol` matches
neither and there is no unknown-member backstop. Verified: `class C; int x;
constraint c1 { x > 0; } endclass` compiles and runs clean (exit 0). This violates
the project's rules today, independently of anything in this document.

Note that a constraint block on a class with no `rand` property is inert, so nothing
observable is wrong yet — but the guard has to exist before rand properties are
admitted, or step 1 would start honouring `rand` while dropping the constraints that
qualify it.

### 2.3 What crashes

The other built-ins slang synthesizes on every class — `srandom`, `get_randstate`,
`set_randstate`, and the *object-level* `rand_mode` / `constraint_mode`
(`ClassSymbols.cpp:234-252`) — carry `MethodFlags::BuiltIn` but **not**
`MethodFlags::Randomize`, so they are never rewritten into system calls. They route
through `lower_user_call` → `function_id` → `build_function`, which lowers
`getBody()`; slang returns an empty `StatementList` for a bodyless builtin
(`SubroutineSymbols.cpp:31-38`), and the resulting `ir::Function` has an argument-dir
list that does not match the spliced call. Measured on the ASan build at "Merge feat/virtual-interfaces: virtual interfaces (LRM 25.9)":

| design | result |
|---|---|
| `c.srandom(7);` | ASan heap-buffer-overflow, `call_actuals`, `src/ir/Analysis.cpp:537` |
| `c.rand_mode(0);` | same |
| `c.constraint_mode(0);` | same |
| `c.set_randstate("abc");` | same |
| `s = c.get_randstate();` | UBSan null load, `src/ir/LlvmBackend.cpp:20256`, then SIGSEGV |

All five are legal SystemVerilog. Closing this is P0 (§14) and is worth landing on
its own regardless of the rest of this design.

### 2.4 What already exists and is reusable

- The RNG primitive: xoshiro256++ inlined by `emit_xoshiro_next`
  (`src/ir/LlvmBackend.cpp:21925-21956`), and the only runtime entry point,
  `void rt_urandom_seed(uint64_t* state, uint64_t seed)`
  (`src/runtime/Runtime.cpp:4970-4979`, exposed `src/ir/Jit.cpp:241`), a splitmix64
  expansion of one 64-bit selector into four state words. Both are reused verbatim.
- The precedent for a body seeded from one draw of its creating context: the fork
  child / deferred body at `src/ir/LlvmBackend.cpp:4920-4933`. The object seeding in
  §9 is byte-for-byte the same sequence.
- The precedent for a reserved vtable slot: `kCopySlot` (`"$copy"`,
  `src/Lower.cpp:23709`), reserved for every class at `src/Lower.cpp:2736-2739` and
  called at `:22891`.
- The precedent for per-class synthesized code the runtime calls back into:
  `clotho.classfini.<Ref>` (`src/ir/LlvmBackend.cpp:2186-2221`).

### 2.5 The constraint that shapes everything

**A C++ runtime helper cannot address a class property.** The only offset channel
from generated code to the runtime is `rt_gc_map::handles`, a `const uint32_t*` of
byte offsets used for tracing (`src/runtime/Gc.h:86-93`, built by
`class_handle_offsets`, `src/ir/LlvmBackend.cpp:2145-2168`); `leaves` is null in this
phase. Every field-touching operation in clotho is JIT-emitted. So `randomize()`
cannot be `rt_randomize(handle)`; it must be generated code that knows the layout.
That single fact decides §6 and §7.

---

## 3. Decision summary

| question | decision | why |
|---|---|---|
| where does randomize live | a synthesized IR function per class, `clotho.randomize.<Ref>` | §2.5: the runtime cannot address properties |
| how is it reached | a reserved vtable slot `"$randomize"`, alongside `kCopySlot` | LRM makes `randomize` virtual; the slot also solves the layout-offset problem (§5.3) |
| how are srandom / rand_mode reached | one more reserved slot `"$randops"`, `i64 (this, i32 op, i64 arg)` | same offset problem, one slot instead of four |
| where does the object RNG live | four `i64` words appended to the class body struct | GC header must stay at `handle[-1]` (`src/runtime/Gc.h:41-46`); the body is the only free space |
| when is it seeded | at `ClassNew`, one draw of the creating thread, splitmix64-expanded | LRM 18.14.2 |
| does `randomize` draw from the thread RNG | **no** | the object owns the stream; keeps `blocks_thunk` off (§9.3) |
| does `new` of a rand class draw from the thread RNG | **yes, exactly one draw** | LRM 18.14.2 hierarchical seeding; forces `rng_own_` (§9.2) |
| does `$urandom` in a class method change | **no**, it keeps the calling thread's stream | §16, and it keeps the pinned streams in `tests/sysfunc_test.cpp:288-296` |
| rand_mode state | one `i64` bitmask per object, bit *k* = rand ordinal *k* | dense, inheritance-flat, one `and` per field in the generated body |
| randc state | a 256-bit remaining-value bitmap per randc property | exact cyclic semantics with no permutation array |
| solver seam | one region of the generated body (§7, step 2) | everything else — hooks, recursion, modes, stability, refusals — is constraint-independent |

---

## 4. Semantics step 1 must reproduce

Oracle position: **iverilog 13 cannot run any of this** (no class randomization), so
it is not the oracle here. Verilator 5.050 implements a useful
subset and was used to score the cases below; anything it declines is pinned to the
LRM text. Each row was measured, not assumed.

| behaviour | measured | evidence |
|---|---|---|
| hook order is `pre` → (nested `pre`/`post`) → `post` | `outer.pre / inner.pre / inner.post y=4 / outer.post` | verilator 5.050 |
| a `rand` class handle is **recursed into**, never replaced | `same_handle=1` | verilator 5.050 |
| a **null** `rand` handle is skipped and `randomize()` still returns 1 | `null-handle: ok=1 … lh_null=1` | verilator 5.050 |
| a 4-state `rand logic [3:0]` gets 2-state values | `four=0011` | verilator 5.050 |
| non-`rand` properties and `static` properties are untouched | `plain=f`, `s=9` | verilator 5.050 |
| an enum draws only declared labels, uniformly | 300 draws: A=97 B=104 C=99, other=0 | verilator 5.050 |
| `randc bit[1:0]` is a permutation per cycle | `2 1 3 0` then `3 0 2 1` | verilator 5.050 |
| `x.rand_mode(0)` holds the value; the query form returns 0 | `a=a … rand_mode()=0` | verilator 5.050 |
| object-level `rand_mode(0)` holds every rand field | `b=5` | verilator 5.050 |
| `randomize(b)` randomizes only `b` | `a=1(held) b=9` | verilator 5.050 |
| inherited rand fields are randomized; `rand_mode` on an inherited field works through a derived handle | `b=c (held) d=9` | verilator 5.050 |
| a dynamic array's **size** is not randomized, its elements are | `dq.size=3`, elements moved | verilator 5.050 |
| `randomize(null)` assigns nothing and returns 1 | verilator declines (`Unsupported: 'randomize(null)' on class with rand container or class member`) | LRM 18.11 |

---

## 5. Object layout: the rand block

### 5.1 Shape

A class **owns a rand block** iff it, or any class in its base chain, declares at
least one `rand`/`randc` property. The block is three kinds of ORDINARY class field,
appended by `lower_class_type` (`src/Lower.cpp`) after every declared property:

| field | type | purpose |
|---|---|---|
| `$rand.rng` | unpacked array `bit [63:0] [4]` | the object's xoshiro256++ state (LRM 18.14.2) |
| `$rand.mode` | `bit [63:0]` | bit *k* = rand ordinal *k* is active (LRM 18.8) |
| `$rand.randc` | unpacked array `bit [63:0] [4]`, one per `randc` property | the 256-bit already-used-this-cycle bitmap (LRM 18.6) |

Carrying them as fields rather than as a backend-side extension of `class_body_ty`
is what keeps the whole backend out of this feature. A `member_select` reaches them
like any other property; `emit_class_body_copy` copies them like any other property
(§16); `gc_object_bytes` and `rt_gc_map::size` grow on their own. The 4-element
unpacked array is chosen so the storage is addressable as a `uint64_t[4]`, which is
the shape `rt_urandom_seed` and the inlined xoshiro step already take.

`class_header_words()` stays **1** and every property GEP stays `1 + declIndex`,
which is what `src/runtime/Gc.h:41-46` requires and what the `$cast` display
arithmetic (`4*depth - 64` off the vptr) depends on. Nothing above the properties
moves.

The block holds no pointers, so `class_handle_offsets`
(`src/ir/LlvmBackend.cpp:2145-2168`) yields the same handle list as before and the GC
traces the object unchanged.

Its field indices are decided and published to `class_rand_` BEFORE the second
layout pass, for the same reason the property indices are (`lower_class_type`'s two
passes): lowering a property's type can reach a method body that constructs an
object of this very class, and the seeding at that `new` needs the block it is about
to get.

### 5.2 Rand ordinals

The rand ordinal space is the subsequence of `class_prop_index_`
(`src/Lower.cpp:2619-2633`) whose `randMode != None`, in the same
deepest-base-first order the property indices already use. So a derived class's
ordinals extend the base's rather than renumbering them, and one flat `i64` mask
covers the whole chain. More than 64 rand properties in one chain is refused loud
(§12); a second mask word is a mechanical extension if a real design ever needs it.

### 5.3 The offset problem, and why the vtable solves it

Because the block sits *after* the properties, its offset differs between a base and
its derived class (the derived has more properties in front of it). Prefix layout —
the thing that makes an upcast a no-op — is preserved, but it means **the block must
never be addressed from a statically unknown type**.

The invariant that makes this safe: *every access to the rand block happens inside a
function synthesized for that exact class, selected by the vtable.* A base-typed
handle reaches the most-derived `clotho.randomize.<Ref>`, which was compiled against
the most-derived layout. There is no other door.

The alternative — hoisting the block into the header, or storing its offset in the
class descriptor — is rejected in §13.

---

## 6. Dispatch: two reserved vtable slots

`compute_vtable` (`src/Lower.cpp:2715-2792`) already reserves `kCopySlot` for every
class before user virtuals are numbered. Two more join it, unconditionally, so the
slot numbers are design-wide constants:

| slot | signature | body |
|---|---|---|
| `"$randomize"` | `int (ptr this, i64 mask)` | §7 |
| `"$randops"` | `longint (ptr this, longint op, longint arg)` | `srandom`, `rand_mode` set/query, and (P5) the randstate codec |

Every class gets both filled, including rand-less ones, whose bodies are trivial:
`$randomize` runs the hook chain and returns 1; `$randops` returns 0 for a
`rand_mode` query and does nothing otherwise. Reserving unconditionally costs 16
bytes per class descriptor and keeps a single slot number for the whole design,
exactly as `kCopySlot` does.

`op` is a small enum, not a general dispatch: `Srandom`, `RandModeAll`,
`RandModeOne` (`arg` = `ordinal << 1 | on`), `RandModeQuery` (`arg` = ordinal). The
per-property form `obj.x.rand_mode(0)` resolves the ordinal statically from the
property symbol and passes it as `arg` — the *ordinal* is static, only the block's
address is not.

`randomize` stays a genuine virtual, which the LRM requires. Note that
`is_user_virtual` (`src/Lower.cpp:2654-2656`) keeps slang's built-in `randomize`
symbol out of ordinary slot allocation; that stays, because the slot is now reserved
by name instead.

---

## 7. The generated body, and the solver seam

`clotho.randomize.<Ref>` is built in `src/Lower.cpp` with the ordinary `ir::Builder`,
like any other function, so it prints under `--emit ir`, is analyzed by
`src/ir/Analysis.cpp`, and calls user hooks through the normal call ABI. Skeleton:

```
int $randomize(this, mask):          // mask FILTERS the active set for THIS call
  0.  eff = mask & this.$rand.mode      (LRM 18.8)
  1.  call pre_randomize                 (LRM 18.6.2)
  2.  <SOLVE>                            ← the seam
  3.  call post_randomize
  4.  return 1
```

**Step 2 is the only region a constraint solver replaces.** In step 1 it is
straight-line code: for each rand ordinal *k*, in ordinal order,

```
if (eff >> k) & 1:
    <field rule k>            (§8)
```

The test is not folded away: a class cannot in general be proven untouched by
`rand_mode`, and the argument form varies the mask per call site. It is one
`and` plus one branch per field against a value already in a register.

When a solver lands, step 2 becomes: partition the active rand set into constrained
and unconstrained, hand the constrained partition plus the object body pointer, the
active mask and the object's RNG pointer to the solver, and keep the step-1 draw loop
for the unconstrained remainder. Three properties of step 1 are what make that a
drop-in:

- **The hook chain, the recursion and the mode mask are outside the seam.** They are
  constraint-independent by the LRM and are implemented once, here.
- **The draw order is defined** (ordinal order, one field rule per field, §8), so a
  solver that leaves an unconstrained field to the step-1 path reproduces the same
  values.
- **The return value is already a value**, not a constant folded by the caller, so
  turning `return 1` into `return solver_status` changes one instruction.

`mask` is a FILTER, not the active set: the body intersects it with the object's own
`$rand.mode`. That is what lets a caller who does not know the callee's dynamic type
— the recursion into a `rand` handle, and every plain `obj.randomize()` — pass
all-ones and still honour modes only the callee can address. `randomize(x, y)`
(LRM 18.11) passes the subset constant, `randomize(null)` passes zero. One generated
body serves all three forms, and a solver gets the same knob.

The hooks are NOT a chain: `find_rand_hook` walks the class chain most-derived-first
and calls the first USER declaration it finds (slang synthesizes an empty built-in on
every class, which `MethodFlags::BuiltIn` filters out). That is the LRM's behaviour
in both directions — where a class declares no override the walk lands on the nearest
base's declaration, which is exactly what the LRM's default chaining implementation
would have reached, and where a class does override, reaching its base is the
override's own job via `super.pre_randomize()`. An explicit call to a base's
synthesized default lowers to nothing (`lower_builtin_class_method`).

Recursion into a `rand` class handle (LRM 18.4) is a null test plus a virtual call
through `"$randomize"` on the handle — which is why a `rand` handle to a base type
correctly randomizes the derived object it holds.

---

## 8. Per-field draw rules

Every rule consumes draws from the **object's** RNG via one primitive, `ObjRandom`
(§10), which yields the high 32 bits of one xoshiro256++ step exactly as
`lower_urandom` does (`src/ir/LlvmBackend.cpp:21958-21962`).

| field type | rule |
|---|---|
| integral, width W ≤ 32 | one draw, masked to W. Signedness is irrelevant to the bits. |
| integral, width W > 32 | `ceil(W/32)` draws, filling 32-bit chunks from the LSB up, the last chunk masked |
| 4-state | the same 2-state value; the z/x planes are cleared. Never x, never z (LRM 18.4) |
| packed struct / packed array | one integral of the total packed width |
| enum | one draw, Lemire-mapped to `[0, n)` over the *distinct declared values*, then an n-way select chain. Uniform over labels, not over the base type's range |
| fixed unpacked array | each element in index order, recursively by these rules |
| class handle | not assigned. If non-null, virtual `"$randomize"` on it; if null, skipped |
| `randc`, width W ≤ 8 | §8.1 |
| non-`rand`, `static`, or `rand_mode(0)` | untouched |

The enum select chain is `emit_rand_enum`. A module-level constant table indexed by
the mapped draw would also work (`docs/design/const-tables.md`), but a member count
is small and the chain keeps one mechanism instead of two.

A `rand` **dynamic array, queue, associative array or unpacked struct** is refused
loud rather than element-walked: the first three need a runtime loop over a size
only known at run time, which the generated body has no reason to grow before a
solver needs one anyway (§12).

### 8.1 randc

`randc` is cyclic: over `2^W` successive calls every value appears exactly once, in a
random order, then a fresh permutation starts (measured: `2 1 3 0 | 3 0 2 1`).

Implementation: a 256-bit `randc_seen` bitmap per randc property. A draw picks
uniformly among the *clear* bits — `popcount` the four words to get the remaining
count `m`, Lemire-map one draw to `[0, m)`, select the *i*-th clear bit, set it. When
`m` reaches 0 the bitmap is cleared and the cycle restarts. That is an exact random
permutation with no permutation array and no rejection loop.

This is the one field rule that is a runtime call rather than inline IR:
`uint32_t rt_randc_next(uint64_t* seen, uint32_t nvals, uint64_t* rng)`. It is
callable from C++ *because the backend passes pointers, not offsets* — the same
reason `rt_dynarr_shuffle(rt_dynarr*, const rt_elem_ops*, uint64_t* rng)`
(`src/runtime/Runtime.h:1019`) works today.

The 8-bit cap is deliberate: 256 values is one 32-byte bitmap per property. LRM 18.6
permits an implementation limit and requires at least 8 bits. Wider `randc` is
refused loud (§12), in the same spirit as the existing
`randcase with a weight wider than 32 bits is not supported`.

---

## 9. Random stability (LRM 18.14.2)

### 9.1 Seeding

At the `new` of a class that owns a rand block, and only there, `lower_new_class`
emits two instructions before the property initializers run:

```
obj_rand_seed(this.$rand.rng, zext(urandom()))   // ONE draw of the creating thread
store this.$rand.mode = ~0                        // every rand property starts enabled
```

The draw is the ordinary `Urandom` op, which is what makes this cost no analysis
change at all (§9.2). `rt_urandom_seed`'s splitmix64 expands the 32-bit selector into
the four state words, exactly as it does for a process ramp.

**Exactly one draw per `new`**, whether or not the object is ever randomized. If the
draw were conditional on later use, adding a `randomize()` call would shift every
subsequent seed in the creating thread — the precise instability LRM 18.14 exists to
prevent.

### 9.2 Consequence for the RNG-ownership analysis: none

`ir_draws_rng` (`src/ir/LlvmBackend.cpp:7238-7263`) is the seed set that decides which
bodies materialize their own frame state and which processes stay off the thunk path.
Because the seeding draw at §9.1 is a real `Urandom` instruction in the constructing
body, that machinery already does the right thing untouched: a process that
constructs a randomizable object is, by that instruction, a process that draws — it
gets its own frame state and leaves the comb-thunk path, and a fork child that
constructs one is already `rng_own_`.

An earlier draft of this design had `ClassNew` join the seed set payload-dependently,
with a matching arm in `is_simple_always`. Emitting the draw in the frontend deletes
both. This is the single largest simplification the implementation found.

### 9.3 What does *not* join

`ObjRandom`, `ObjRandSeed` and `RandcNext` are **not** in `ir_draws_rng` and are
**not** `blocks_thunk`. Their state is heap-resident, in the object, so a thunk-lowered
body does not restart any stream — unlike `$urandom`, whose state is frame-resident,
which is exactly why that one blocks thunks (`src/ir/Analysis.cpp:130-140`).
This is a real structural win over the `$urandom` path and should be stated in the
tag's doc comment so nobody "fixes" it later.

`pre_randomize`/`post_randomize` are ordinary user functions reached by ordinary
`Call`s, so if one of them draws `$urandom` the existing callee closure
(`src/ir/LlvmBackend.cpp:7294-7327`, including the virtual-slot union at `:7279-7290`)
already charges the draw to the caller. Nothing to add.

---

## 10. IR, printing, legalization

Three new tags, appended at the **end** of `Inst::Tag` per the rule at
`src/ir/Ir.h:1946-1950` (a tag's ordinal feeds the digests in `tests/sva_test.cpp`):

| tag | payload | value | lowering |
|---|---|---|---|
| `ObjRandom` | `unary` — the `$rand.rng` field **lvalue** | `{32,false,false}` | `agg_slot_ptr`, `emit_xoshiro_next`, high 32 bits |
| `ObjRandSeed` | `binary` — `l` = the state lvalue, `r` = a 64-bit seed | none | `agg_slot_ptr`, `call rt_urandom_seed` |
| `RandcNext` | `op_extra` — `op` = the bitmap lvalue, extra `RandcInfo{Index rng; uint32_t nvals;}` | `{32,false,false}` | two `agg_slot_ptr`s, `call rt_randc_next` |

**All three take lvalue operands, not an object handle plus a field number.** That is
what leaves the backend with no knowledge of the rand block: the frontend has already
done `member_select(handle_deref(this), field)`, and the backend only needs the
storage pointer `agg_slot_ptr` gives it for any aggregate lvalue.

Classification, per the six `-Werror=switch` sites and the two constexpr totality
`static_assert`s:

1. `payload_kind` — `src/ir/Ir.h`: `Unary`, `Binary`, `OpExtra`.
2. `tag_info` — `src/ir/Analysis.cpp:11-262`: all three are `pure=false`,
   `blocks_thunk=false` (§9.3), `synthesizable=false`.
3. `ValueSweep::sweep` — `src/ir/Analysis.cpp:897-1512`: the valueless / always-known
   arm; the two value-producing tags are 2-state with `val_x = 0`.
4. `tag_name` — `src/ir/Printer.cpp:104-660`, plus the `OpExtra` render for
   `RandcNext` at `:882-1020` (that one has a silent `default:`).
5. `compute_type` — `src/ir/LlvmBackend.cpp:12638-13045`.
6. `lower_inst` — `src/ir/LlvmBackend.cpp:13301-15046`.
7. `Ir::operands` — `src/ir/Ir.cpp:143-296`: `RandcNext` needs an `if` in the
   `OpExtra` arm or its handle operand is invisible to every def-use walk. This one
   fails **silently**.
8. `extra_operand_shape` / `extra_operand_tail` — only if `RandcNext` were `Extra`
   rather than `OpExtra`; as specified, it is not.

Legalization needs no edit: the `default:` at `src/legalize/Legalize.cpp:636-661`
rejects anything whose `tag_info(...).synthesizable` is false, which is right — a
randomizing class is not hardware.

Dedup: none of the three embeds a `Signal` or `FunctionId`, so the instance-specific
blocker list at `src/ir/LlvmBackend.cpp:7515-7519` is unaffected.

Nothing here suspends. `pre_randomize`/`post_randomize` are LRM `function`s and lower
as atomic `Call`s, so the generated body stays a plain function and never needs the
task ABI. If a future solver is ever allowed to consume time, its instruction has to
be treated exactly like `TaskCall` — statement-only, coroutine-only, epoch-bracketed,
`blocks_thunk`, and listed in both reactive backstops
(`src/ir/LlvmBackend.cpp:5960-6013`, `:6022-6050`). Step 1 does not go there.

One new runtime symbol, `rt_randc_next`, declared in `src/runtime/Runtime.h`, defined
`extern "C"` in `src/runtime/Runtime.cpp`, registered with one `expose(...)` line in
`src/ir/Jit.cpp:183-300`.

---

## 11. `std::randomize`

Scope randomize has no object, so it draws from the **calling thread's** RNG — i.e.
it lowers to the existing `Urandom` tag and inherits `ir_draws_rng` membership,
frame-state ownership and thunk-blocking for free. Confirmed against the LRM by
elimination: 18.14.2 gives an RNG to objects, and there is no object here.

Argument shape is the one slang trap: `ScopeRandomizeFunction::bindArgument`
(`MiscSystemFuncs.cpp:193-196`) wraps every argument in an `AssignmentExpression`
whose RHS is a throwaway placeholder, so the real target is
`arg->as<AssignmentExpression>().left()`. That is also the discriminator between
scope randomize and class randomize, which otherwise share
`getSubroutineName() == "randomize"` and carry no class pointer:

- any argument of `ExpressionKind::Assignment` → scope randomize;
- `arguments()[0]` of class-handle type → class randomize through an explicit handle;
- no arguments → the enclosing scope decides (bare `randomize()` inside a class method
  resolves to the class member), and the implicit `this` comes from
  `resolve_method_this` (`src/Lower.cpp:23204-23230`).

Field rules are §8's, minus the class-handle and randc rows (a local cannot be
`randc`). Return value 1.

---

## 12. The refusal surface

Each gets its own message and its own test, per `docs/design/process-class.md` §14's
"the refusal messages, each by its own name".

| construct | message | site |
|---|---|---|
| any `constraint` block in a class | `constraint blocks are not supported` | `lower_class_type`, new member walk |
| inline `with { … }` on either randomize | `inline constraints on randomize() are not supported` | the randomize lowering, via `RandomizeCallInfo::inlineConstraints` |
| `constraint_mode()`, both forms | `constraint_mode is not supported` | system-method path and `$randops` |
| `randc` wider than 8 bits | `randc wider than 8 bits is not supported` | `lower_class_type` |
| `rand real` / `shortreal` (LRM 2023) | `rand real properties are not supported` | `lower_class_type` |
| `rand` dynamic array / queue / assoc array / unpacked struct | `rand properties of this type are not supported` | `guard_rand_type` |
| more than 64 rand properties in one chain | `more than 64 rand / randc properties in one class is not supported` | `admit_rand_prop` |
| `randomize(x)` naming a **non-rand** property (LRM 18.11 makes it rand for that call) | `randomize() argument naming a non-rand property is not supported` | `lower_randomize_call` |
| `rand_mode` on a subelement of a rand property | `rand_mode is supported on a whole rand property only` | `lower_rand_mode_call` |
| `std::randomize` of a class handle | `std::randomize of a class handle is not supported` | `lower_scope_randomize` |
| randomizing a class whose layout is still in progress (a property-type cycle) | `randomization of a class that is still being laid out is not supported` | `rand_slot_of` |
| `get_randstate` / `set_randstate` (until P5) | `get_randstate / set_randstate are not supported` | the BuiltIn whitelist below |
| any other unimplemented `MethodFlags::BuiltIn` class method | `unsupported built-in class method '<name>'` | `lower_user_call` |

Enumerating constraint blocks needs slang's own pattern, not `membersOfType`:
inherited members are wrapped in `TransparentMemberSymbol` and `membersOfType`'s
iterator does not unwrap (`Scope.h:189-193`), so the walk is the one at
`ConstraintAnalysis.h:308-322` — iterate `scope.members()`, unwrap
`TransparentMember`, test for `SymbolKind::ConstraintBlock`. Because clotho already
walks the base chain for properties, checking each class in the chain is equivalent
and simpler.

`dist` needs no guard of its own: `x dist {…}` binds as an `ExpressionConstraint`
whose expression is a `DistExpression`, and a constraint expression is only reachable
from inside a constraint block or a `with` clause, both of which are already refused.
Recording that here so its absence does not read as an oversight.

The last row is the P0 crash fix (§2.3), and it is a **whitelist**: `lower_user_call`
refuses any `BuiltIn` subroutine it does not explicitly implement, rather than
lowering slang's empty body. New slang versions adding built-ins then fail loud
instead of silently no-oping.

`srandom` on a class with **no** rand property is a genuine no-op, not a refusal:
such a class has no rand block, and nothing in the language can observe the RNG it
would have seeded. `get_randstate` on it is refused rather than inventing a state to
print (§16).

---

## 13. Rejected alternatives

**`randomize()` as a runtime helper, `rt_randomize(handle, ...)`.** Rejected: the
runtime has no way to reach a property (§2.5). It would need a second per-class offset
table beside `rt_gc_map`, invented and maintained solely for this, plus a type tag per
field — a full reflection layer to avoid emitting a hundred bytes of straight-line IR.

**The rand block in the object header, above the properties.** Rejected: it changes
`class_header_words()` from 1 to 6, which moves every property GEP
(`src/ir/LlvmBackend.cpp:2390`, and the sites at `:2208`, `:2285`), the `$cast`
display arithmetic, and the GC's assumption at `src/runtime/Gc.h:41-46` — and it taxes
every class in the design, including the overwhelming majority with no rand property.

**The rand-block offset in the class descriptor.** Rejected, though it is the closest
call: it works, and it costs one extra load per access. But it also grows
`%clotho.classdesc` for every class and adds a third variant to a struct whose shape
already varies with `--gc` (`src/ir/LlvmBackend.cpp:1943-1949`). The vtable slot gets
the same polymorphism for free from machinery that already exists, and `randomize`
has to be virtual anyway.

**Four vtable slots instead of `"$randops"` with an opcode.** Rejected: `srandom`,
`rand_mode` and the randstate codec are rare operations; four slots per class buys one
saved compare on a path nobody profiles.

**Lazy object-RNG seeding at the first `randomize()`.** Rejected: it draws from the
*randomizing* thread instead of the *creating* thread, so two objects created in a
fixed order but randomized in the other order would swap streams. Directly contrary
to LRM 18.14.2.

**A whole-program pre-pass to find which classes are targeted by `srandom`, so
rand-less classes can still get a block.** Rejected for step 1: `lower_class_type` is
on-demand and memoized, so layout is fixed before bodies are lowered, and a pre-pass
means a second traversal of every class body before the first one is lowered. The
syntactic rule (has a rand property) is stable and explainable; §12 documents the one
observable consequence.

**A permutation array for `randc`.** Rejected: the remaining-set bitmap is 32 bytes
for the whole 8-bit range, needs no shuffle at cycle start, and yields the same
distribution.

---

## 14. Phased plan

Each phase was independently landable and independently testable. All of P0-P4
landed together, as one branch; the phase boundaries below are kept because they are
the order the work was done and the order a reviewer can follow it.

The invariant "byte-identical for a design that declares no rand property" holds for
a design with **no class**, and was verified (`--emit llvm` compares equal). It does
NOT hold for a class-bearing design: every class now carries two more vtable slots
and two synthesized bodies (§6), which is 16 bytes of descriptor plus two small
functions per class whether or not it randomizes. Measured on a one-class design,
`--emit llvm` grew from 15509 to 16511 bytes. Collapsing the trivial bodies — a
rand-less, hook-less class's `$randomize` is "return 1" and its `$randops` is
"return 0" — onto one shared pair per module is the obvious follow-up if a
UVM-scale class count makes it matter.

- **P0 — close the crash and the silent accept.** (landed) The `BuiltIn` whitelist
  guard in `lower_user_call`, the constraint-block guard, the inline-`with` guard.
  *Acceptance:* the five designs in §2.3 emit `unsupported-construct` diagnostics
  instead of crashing; `constraint c { x > 0; }` no longer compiles silently.

- **P1 — layout, dispatch, object RNG, integral fields.** (landed) The rand block,
  the two reserved slots, the `new`-site seeding, `ObjRandom`, `ObjRandSeed`,
  `srandom`, and a generated `$randomize` covering integral, packed and enum fields.
  *Acceptance:* met — see §15.

- **P2 — hooks and aggregates.** (landed, except the dynamically-sized containers,
  which are refused loud per §12) `pre_randomize`/`post_randomize`, recursion into
  `rand` class handles with the null skip, fixed unpacked arrays element-wise.

- **P3 — the active set.** (landed) `rand_mode()` in all four spellings, the mask as
  the second parameter of `$randomize`, `randomize(x, y)`, `randomize(null)`.

- **P4 — `randc`.** (landed) The bitmap, `RandcNext`, `rt_randc_next`, the 8-bit
  guard.

- **P5 — optional: the randstate codec.** `get_randstate` / `set_randstate` on an
  object: two `rt_*` helpers formatting and parsing four state words as 64 hex digits
  into a `clotho.str`, on the out-parameter convention
  (`rt_string_itoa`, `src/ir/LlvmBackend.cpp:11720`), rejecting a foreign string with
  a located fatal. Shared verbatim with `docs/design/process-class.md` §7.4, whichever
  lands first.

Constraints, `constraint_mode`, `dist`, `soft`, `solve … before` and `unique` are
**not** phases here. They keep their §12 guards until step 2.

---

## 15. Verification

New file `tests/randomize_test.cpp`, added to the explicit TU list at
`CMakeLists.txt:516-591`, in the existing style: `run_capture` for behaviour,
`lower_error` for loud fails (`tests/randcase_test.cpp:16-54` is the closest model),
`tests/Subprocess.h` for anything ending at `$finish`.

Determinism without a `--seed` flag — there is none, and this design does not add one.
Three instruments, all yielding an exact expected stdout:

1. **Pin the stream.** Object seeding is deterministic (process index → thread state →
   one draw per `new`), so a small design's draws are run-stable and can be pinned
   literally, as `tests/sysfunc_test.cpp:288-296` pins `$urandom`.
2. **`srandom` reproducibility.** Seed two objects identically, assert their sequences
   are equal; seed differently, assert they differ.
3. **In-design statistical bounds**, `$display`ing booleans, for the distribution
   claims (enum uniformity, per-field independence), as `randcase_test.cpp:56-79`
   does over 8000 draws.

What landed, in `tests/randomize_test.cpp` (22 cases, all passing under the ASan +
UBSan build):

- **Value rules:** assignment with non-rand untouched; a 4-state property never x or
  z over 200 draws; an enum uniform over its labels and never outside them over 900
  draws; a 96-bit property with every 32-bit chunk moving; a 4-element unpacked array
  whose elements differ.
- **Hooks and recursion:** the exact `outer.pre / inner.pre / inner.post / outer.post`
  string; handle identity preserved with the pointee moving; a null handle skipped
  with `randomize()` still 1; a base-typed handle randomizing the derived object's
  own properties.
- **Modes:** `rand_mode(0)` then `(1)` on one property with the query form checked in
  both states; object-level off/on; an inherited property through a derived handle;
  `randomize(b)` moving only `b`; `randomize(null)` moving nothing and returning 1.
- **randc:** eight consecutive 4-value cycles each an exact permutation; an 8-value
  randc observed in a non-sorted order.
- **Stability:** two objects independent over 50 draws; `srandom` making two objects
  agree and then diverge; two sibling fork children's objects differing; the stream
  reproducible run to run; a shallow copy continuing the source's stream (§16).
- **Refusals:** constraint block, inline `with`, `constraint_mode`, `randc` > 8 bits,
  `rand` dynamic array, and both randstate methods — each asserted by its own
  message; plus `srandom` on a rand-less class being inert rather than a crash.

Suite result: **2007 of 2013 cases pass** (the 22 new cases included, ASan + UBSan
build). Every one of the six failures is preceded by `JIT session error: …
relocation target … is out of range of Delta32 fixup` — an in-process JIT
section-placement artifact, not a semantic failure. Evidence that it is not this
change:

- `wide_select_test.cpp:277` fails **identically, with the same relocation**, on the
  pre-change binary.
- The other five (`fork_test.cpp:506/587/691`, `struct_test.cpp:1575`,
  `wide_select_test.cpp:297`) pass when their files are run on their own, and the set
  drifts between full-suite runs.

What this change does contribute is pressure: two more functions and two more
descriptor slots per class, across ~2000 JIT'd designs in one process, make the
placement failure likelier. Collapsing the trivial bodies (§14) would take most of
that back, and giving the JIT a small-code-model memory manager would remove the
failure class entirely; neither is this branch's job.

Still owed, and not yet written:

- **GC:** `check_all_gc_modes` (`tests/gc_test.cpp:80-84`) over a randomizing design.
  Objects grew, so `gc_object_bytes` and `rt_gc_map::size` changed; the appended
  fields hold no handles, so the trace map should be unaffected, but that is reasoned
  rather than measured. `--gc` is a non-default mode and the GC work itself is
  unlanded on main, so this belongs with whichever lands second.
- **Thunk polarity:** an assertion in the shape of `tests/analysis_test.cpp:53-79`
  that a randomizing `always` block leaves the comb-thunk path (it does, through the
  `Urandom` at the `new` site, §9.2) while a body that only calls `randomize()` on an
  object it did not create does not.

Each test comment names the LRM clause and, where §4 scored it, records
"verilator 5.050 agrees".

---

## 16. LRM decisions this design records

- **`$urandom` inside a class method keeps using the calling thread's RNG**, not the
  object's. LRM 18.14.2 gives an object "an independent RNG for all randomization
  methods within the object", which can be read as covering `$urandom` in a method;
  18.13 says `$urandom` is thread-stable. The reading taken here is the narrow one —
  the object RNG governs `randomize()` and `srandom()` only — because it
  costs nothing (the thread pointer is already ABI
  argument 1), and leaves the pinned streams in `tests/sysfunc_test.cpp:288-296`
  untouched. Revisit only with a concrete counter-case.
- **Exactly one draw of the creating thread per `new` of a rand-bearing class**, and
  none for a rand-less class. The LRM mandates the seeding but not its cost model;
  making the draw unconditional within the class is what keeps seeds stable when a
  `randomize()` call is added later.
- **`randc` is capped at 8 bits.** LRM 18.6 permits an implementation limit and
  requires at least 8.
- **A null `rand` class handle is skipped and `randomize()` still returns 1.** The
  LRM does not make it a failure; verilator 5.050 agrees.
- **An unconstrained rand field is uniform over its full declared range**, and an
  unconstrained rand enum is uniform over its **distinct declared values**, not over
  the base type's range. Measured against verilator 5.050.
- **A 4-state rand field never receives x or z.**
- **A dynamic array's or queue's size is never randomized** without a constraint —
  moot in step 1, which refuses a `rand` dynamic array outright (§12), and recorded
  because it is the rule the element walk will be built against.
- **A shallow copy carries the randomization state.** `new src` copies the object
  body member-wise, and the rand block is part of the body, so the copy inherits the
  source's RNG state and its rand_mode mask and continues the same stream. LRM 8.12
  step 2 names randomization state as part of what a copy carries; nothing else in
  the LRM says what the copy's generator should be, and re-seeding it would have to
  draw from the copying thread, which 18.14.2 reserves for `new`.
- **The hooks are not chained by the caller.** `randomize()` calls the most-derived
  USER declaration of `pre_randomize` / `post_randomize`; reaching a base's is the
  override's own job (`super.pre_randomize()`), which is what the LRM's default
  implementation does and what an override replaces.
- **The draw order is declaration order, one field rule per field**, and this is a
  contract, not an accident: it is what lets a later solver leave unconstrained fields
  bit-identical.
- **`srandom` on a class with no rand property is a no-op**, since nothing can observe
  the RNG it would seed; `get_randstate` on such a class is refused rather than
  fabricating a state string. The LRM assumes every object has an RNG and says nothing
  about observing one that cannot be reached.
