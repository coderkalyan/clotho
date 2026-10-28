# Unpacked struct backing

Status: **recursive aggregate model implemented** — arbitrary nesting of
unpacked structs and unpacked arrays (array-of-struct, struct-with-array-member,
nested structs, N-D arrays of struct, struct members of any type incl.
string/real). Reads, blocking writes, member access, whole-object copy /
equality / `'{...}` patterns, static-path nonblocking assignment, and
string-free by-value function *and task* arguments (`input` / `output` /
`inout`) / returns all work.
Guarded (loud): whole-object copy / equality over a member that is an unpacked
array, unpacked union members, a string-containing aggregate passed / returned
by value, a `ref` aggregate formal, a struct-bearing array slice as a whole
actual, and a whole aggregate in a general rvalue context ($display %p, concat,
streaming).

## The recursive model

A signal whose type contains an unpacked struct at any depth is routed through a
recursive aggregate path (the perf-critical scalar and flat-memory paths are
untouched):

- **Storage** (`agg_storage_ty`): the LLVM storage type is a direct structural
  mirror of the IR type — a nested `StructType` per unpacked struct, one
  `ArrayType` per array dimension (not flattened), a leaf slot per scalar /
  string / real. A 4-state leaf carries a z plane conservatively (no per-leaf
  may-z analysis), so z assigned to a member is representable.
- **Addressing** (`LValue::agg_path`): a `MemberSelect` pushes a constant field
  index, an `ElemSelect` pushes a runtime element index, uniformly — arbitrary
  nesting composes into one GEP (`lv_field_ptr`). A struct-field-only path is
  *static* (compile-time address); crossing an array index makes it runtime.
- **Power-on** (`emit_agg_init`): a recursive fill — array dims become runtime
  loops, struct fields straight-line — storing X into 4-state leaves and
  initing string leaves; 2-state / real leaves are covered by the state global's
  zeroinitializer.
- **Whole-object** (`emit_agg_store` / `emit_agg_copy` / `agg_eq`): a blocking
  copy of a **string-free** aggregate is a single `llvm.memcpy` of the storage
  (the `AggCopy` IR op) — the byte image is the value, arrays included. A
  string leaf owns a heap buffer, so a string-containing aggregate copies
  per-leaf (string stores deep-copy; arrays unroll). `'{...}` patterns decompose
  per field; equality reduces per leaf, x-propagating. A nonblocking whole-object
  copy always decomposes per-leaf (it defers); a nonblocking copy over an array
  member is guarded (element-wise deferral is a follow-up).

## Nonblocking assignment

A leaf reached by a **static path** (struct fields only) has a compile-time
address, so it commits through the block-commit path; a leaf reached through a
runtime array index has no compile-time address and takes the deferred record
path (both detailed below).

- Each **static leaf** of an aggregate NBA target (`SignalSlot::static_leaves`,
  enumerated by `collect_static_leaves`, recursion stopping at array dimensions)
  owns a **shadow slot** (`AggLeaf::sh_mm_field` / `sh_z_field`).
- **Staging** (`x.a.b <= v`): the whole leaf value is stored into its shadow; the
  block is registered with a **static-leaf-set mask** (`BlockSig::mask` = one bit
  per static leaf, matched to the leaf by its constant field path).
- **Whole-object** `x <= y` decomposes in the frontend to per-leaf `StoreNb`
  (`emit_agg_store` with the `StoreNb` tag).
- **Commit** (`emit_agg_commit`): copy each set leaf's shadow → live (mm and z),
  wake level (Any-edge) readers via `service_any_waiters` if any leaf changed.
  Reuses block-commit registration / epoch / threading.
- **Waiter granularity is aggregate-level** — matches read/sensitivity
  granularity, so the aggregate's own wait buckets are Any (level) only.
  Member-level edge/level sensitivity (`@(posedge x.f)`, `@(x.f)`) is instead
  served by `event_signal` synthesizing a temp net continuously assigned from the
  member and subscribing to *that* net (one delta of latency); the aggregate
  never grows a per-member bucket. Integral leaves only — a non-integral member
  (real / nested aggregate) has no flat net to extract and is rejected loud.

A leaf reached through a **runtime array index** (`arr[i].f <= v`,
`x.mem[i] <= v`, `arr[i] <= s` decomposed per member) has no compile-time
address, so it takes the **deferred path** (`emit_agg_nba` / `emit_agg_array_commit`):
each write stages a `(dest_slot_pointer, nbytes, payload)` record into a
per-aggregate runtime buffer (the aggregate analogue of the array queue, sharing
`rt_nba_arr_*`, indexed after the array buffers); the NBA region replays them by
`memcpy` in push order (later write to a leaf wins). The payload is the new
value written in the leaf's own slot byte layout, so any leaf width / domain —
including a 4-state leaf's mm+z planes — replays uniformly with no per-leaf
monomorphization. A signal can take both paths at once (static field NBA via the
shadow, array-indexed NBA via the queue).

A **partial** leaf write (`x.f[3:0] <= v`, `arr[i].f[2] <= v` — static or
runtime) also defers, with an extra `mask` region in the record (positioned like
the value). When any deferred site of an aggregate is partial (`agg_masked`) the
commit does a **byte-wise masked read-modify-write** — `dest[j] = (dest[j] & ~m)
| (val[j] & m)` — instead of a memcpy; width-agnostic (a mask byte of 0 is a
no-op, so unselected bits and padding are untouched), the aggregate analogue of
`arr_masked`. A whole-leaf write to a masked aggregate uses an all-ones mask.

Covered by `tests/struct_test.cpp`: member / nested-member NBA one-cycle latency,
whole-struct NBA sampling (`y <= x` trails a clock), runtime-indexed register
file of structs, whole struct-array-element NBA, runtime-index into a struct
array member, mixed static+runtime NBA on one signal, 4-state X/Z through the
deferred memcpy, and static / runtime-index / mixed **partial** member NBA with x
preserved in the unwritten bits of a 4-state leaf.

Access reuses the scalar leaf path once a select chain resolves a leaf, so
member arithmetic, continuous-assign reads, and inferred sensitivity work
unchanged. A packed-struct member (`x.p.field`) is a part-select over the leaf
slot and composes. iverilog 13 does not support unpacked structs, so these cases
are pinned by `tests/struct_test.cpp`.

## By-value ABI (function and task arguments, and returns)

An aggregate has no flat value, so it passes by **pointer** and copies at the
boundary for value semantics (LRM 13.5.1). Scoped to **string-free** aggregates;
a string leaf owns a heap buffer, so a by-value copy would need a per-leaf deep
copy (guarded, follow-up).

- **Frame storage** (`FrameSlot::is_agg`): a function local / formal of aggregate
  type is a nested `agg_storage_ty` field in the call frame (like a signal's
  storage), not a flat slot. `local_lvalue` returns an aggregate lvalue over it;
  `init_frame` powers it on with `emit_agg_init`.
- **Call ABI** (`declare_function` / `declare_task`): each aggregate parameter is
  one pointer param (the caller's object) in every by-value direction; an
  aggregate return is a `void` return plus a trailing pointer out-param.
  `param_indices` counts one LLVM slot per aggregate formal (not the scalar
  `(mm, z)` pair), for a task as well as a function.
- **Marshalling**: the callee copies each aggregate `input` / `inout` formal into
  its frame at prologue (`emit_agg_copyin`, an `emit_agg_storage_copy` memcpy) so
  a mutation of the formal cannot reach the caller, and copies an `output` /
  `inout` formal's frame Local back through the same pointer at return
  (`emit_copyout`). `lower_ret` of an aggregate memcpies the returned object's
  storage into the out-param. The caller (`lower_call_inst`) allocates a temp for
  an aggregate return, passes its pointer, and hands the temp back as the Call's
  lvalue — the **safe-temp** discipline (callee writes the temp, then the
  assignment copies temp → target via `AggCopy`), which avoids aliasing when the
  target also feeds the call.
- **Frontend** (`lower_user_call` / `lower_task_call` / `lower_return` /
  `emit_agg_store`): an aggregate actual passes its lvalue (`lower_lvalue`, not a
  loaded value) — its own storage for a struct-bearing type, a canonical
  `make_canonical_agg_temp` for a flat (possibly may-z-narrowed) array, which
  `emit_call_copyouts` copies back into the actual after an `output` / `inout`
  call; an aggregate `return` yields the returned object's lvalue; an assignment
  whose RHS is an aggregate-returning call copies the Call's temp into the target.
- **Tasks** are the same ABI over the coroutine shape: the pointer is passed to
  the ramp, the copy-in runs in the body's start block (the caller's first resume,
  i.e. the instant of the call), and the copy-out rides the completion epilogue —
  so a task that consumes time copies its `output` / `inout` aggregates back when
  it *returns*, not when it was called (LRM 13.5.1). The caller's storage is a
  frame Local or a signal, so the passed pointer stays valid across the task's
  suspends.

A **non-lvalue actual on an `input` formal** is materialized (LRM 13.5: an
input actual "can be any expression"): a call result passes the caller-side
out-param temp that is the Call's lvalue, everything else (`'{...}` pattern,
aggregate `?:`, concat literal) is built into a fresh `make_canonical_agg_temp`
by `emit_agg_store` (`agg_input_actual`, the call-site twin of
`agg_return_operand`). Guarded (loud, follow-ups): a **string-containing**
aggregate by value, a `ref` aggregate formal (needs the alias ABI, not the
storage-pointer one), and a struct-bearing array slice as a whole actual (the
backend has no aggregate-element slice). Covered by `tests/struct_test.cpp` and
`tests/function_test.cpp`: function arg value semantics under mutation, struct
return, nested struct arg+return, array-of-struct arg, struct-with-array return,
`output` / `inout` function args, task `input` / `output` / `inout` struct and
whole-array args, copy-out after a delay, nesting / recursion / fork concurrency,
a class method task, struct-with-array, array-of-struct, and 2-D and slice
actuals.

## Module ports and net continuous copies

A whole unpacked struct / aggregate crosses a **module port** (input or output)
and drives an aggregate **net declaration assignment** (`wire p_t n = x;`) as a
whole-object continuous copy — the same `AggCopy` (string-free memcpy) /
per-leaf (string) path as a bare `assign a = b`, wrapped in the self-starting
continuous process every continuous construct lowers to (`wire_ports` /
`lower_net_initializer` in `lower/Nets.cpp`, via `emit_port_agg_copy`). Port types
are equivalent (LRM 23.3.3), so both sides share the recursive storage layout
and the memcpy is byte-valid.

The delicate part is **waking downstream readers**. A scalar continuous copy
services its target's wait queue on a value change; the whole-object memcpy did
not, so a chain (`assign mid = x; assign y = mid;`, or an input port feeding an
`assign` feeding an output port) settled only its first hop. `AggCopy` now
memcmp's old-dest vs new-source *before* the copy and, on a change, services the
destination signal's Any-edge waiters (`service_any_waiters`) — gated on change
so a feedback delta wave still settles, and skipped in the run-once PreInit
process and for non-signal targets (a call temp / frame local). This also fixes
`y = f()` for an aggregate-returning call (the caller safe-temp copy now wakes
`y`'s readers). Covered by `tests/struct_test.cpp`: input+output struct ports,
nested struct through a port, the chained continuous copy, and the net
declaration assignment.

Guarded (loud, follow-ups): inout / ref aggregate ports (bidirectional, need net
resolution), and an aggregate port driven by a `'{...}` pattern / call (needs a
materialized temp, like an aggregate call actual).

## Background: why packed reuse does not carry over

A packed struct is a single bit-vector value (LRM 7.2.1): `x.field` is a
constant part-select at the field's declared bit offset, so it rides the
existing PartSelect lvalue/rvalue path with zero backend changes. That is the
whole packed implementation.

An unpacked struct (LRM 7.2, no `packed` keyword) has **no bit-vector
representation**. Members keep their own storage and may be types that are not
plane-packable at all — `real` (IEEE-754 word), `string` (rt_string handle),
nested unpacked array/struct. There is no flat offset to part-select, so
member access must address a *sub-object*, not a bit range. This is the
`llvm::StructType` aggregate the user directed: one LLVM struct field per
member, member access = struct GEP.

## What already exists (IR layer — done)

- `ir::Type::Kind::Struct` with `is_packed=false`, declaration-order field
  table in the pool (`fields_id` / `intern_struct_fields` / `struct_fields`).
  Built for both packed and unpacked in `lower_type`; only the unpacked
  `fail()` gates it.
- `layout()` returns the width-0 sentinel `{Two,0,0}` for unpacked struct
  (mirrors String) — "backend builds its own layout" is already the documented
  contract.

So the type identity and field metadata are plumbed. Everything below is
**backend storage + frontend access/whole-object lowering**; the IR type layer
needs no change beyond possibly a new access op (see §Frontend).

## The backend obstacle: SignalSlot is single-leaf

`SignalSlot` (ir/llvm/Backend.h) models one signal as **one homogeneous leaf
slot** `{ i(2W), iW }`, optionally arrayed to `[N x slot]` for a memory. Every
consumer — `storage_ty`, `state_initializer`/`const_for_path`, `init_state`,
the NBA shadow region, wait queues, `ElemSelect` stride math
(`array_leaf_count`) — assumes that single leaf type. An unpacked struct is a
*heterogeneous tree* of leaves, which this model cannot express. This is the
core work, and it is shared with two open unpacked-array gaps (whole-array
assignment, unpacked arrays as function locals).

## Recommended approach: recursive slot tree (Option B)

Give an unpacked-struct signal an `llvm::StructType` `storage_ty` built by
recursing the field table: field k's storage is its member type's own storage
(scalar `{i(2W),iW}` slot, `[N x slot]` array, `clotho.string` handle, real
word, or a nested struct's `StructType`). Member access GEPs to field k and
yields an LValue over that member's sub-storage — the heterogeneous analog of
`ElemSelect`'s runtime homogeneous index.

Generalize `SignalSlot` from a flat leaf into a **slot subtree**: a member is
either a leaf slot (as today) or a nested aggregate carrying its own child
slots. The GEP-path machinery already recurses `StructType` in
`const_for_path` and `path_offset`, so state-init and NBA-meta offsets extend
naturally once the storage tree exists; `init_state`'s array/string special
cases must be driven per-leaf over the tree instead of per top-level signal.

### Frontend (src/lower/)

1. `lower_type`: drop the unpacked `fail`; build `Kind::Struct is_packed=false`
   (field table already built for both paths).
2. New access op — **`MemberSelect(base, field_index)`** (IR) — the backend
   lowers to a struct GEP. `lower_member_lvalue` branches: packed → `part_select`
   (today); unpacked → `MemberSelect`. `narrow_to_declared` is packed-only and
   drops out for unpacked (each member already carries its declared type).
3. Whole-object ops expand **field-wise** (an aggregate `memcpy` is wrong —
   string members need deep copy, NBA members need per-leaf shadow staging):
   struct `a = b`, `a == b`/`!=`, by-value arg/return, port copy of a whole
   struct, and `'{...}` assignment patterns (already a separate TODO for packed
   too). Recurse to leaves and emit the existing scalar/string/array op per
   leaf.
4. `@(structvar)` sensitivity expands to every leaf signal.

### Staging

- **Stage 1 — scalar-member unpacked structs.** Every member plane-packable
  (integral / packed struct). Storage is a `StructType` of `{i(2W),iW}` slots;
  member access GEP; whole-object + `'{...}` field-wise; NBA per member. Guard
  string/real/nested-unpacked/array-of-struct members loudly. Unblocks the
  common `struct { logic ...; } ` bundle with bounded backend surface.
- **Stage 2 — heterogeneous + arrays.** string/real/nested-unpacked members and
  **array of unpacked struct** (`s arr[0:3]`, `[N x structAgg]`). Needs the full
  recursive shadow tree and breaks `ElemSelect`'s single-slot stride assumption
  — the largest piece.

## Alternative considered and rejected: flatten to per-leaf signals (Option A)

Register one synthetic Signal per leaf member; member access resolves to that
leaf signal. Zero backend changes — reuses all NBA/wait/string/real/array
machinery. **Rejected** because a runtime-indexed array of unpacked structs
cannot pick a static leaf signal, so it can never support `s arr[i].field`, and
because the user directed struct-type (aggregate) backing. Kept only as a note:
if Stage 2's array support proves too costly, Option A is a fallback for the
non-array case.

## Risk / effort

- Type layer: none (done).
- Backend slot-tree generalization: **high** — touches storage build,
  state-init, NBA shadow, and the array stride model; shared with unpacked-array
  gaps.
- Frontend access + whole-object: medium, mostly mechanical field-wise
  recursion.
- Guard everything Stage 1 excludes loudly (PRINCIPLES.md): never silently accept a
  member type or an array-of-struct the backing does not yet lower.
