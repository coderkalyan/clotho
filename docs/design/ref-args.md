# `ref` arguments — live-pointer formals

**Status:** implemented. `ref` / `const ref` on **scalar & packed vector** types,
in **functions and tasks**. The actual may be a **whole variable** or a **whole
element of an unpacked array** (`arr[i]`, const or dynamic index, nested) — both
are one complete `{i(2W),iW}` slot, so the backend passes the slot pointer
(`ref_actual_ptr`: `&array[idx]` for an element, else the scalar slot). Deferred:
array/struct/string/real ref *type*, packed bit/part-select actual (slang rejects
it outright), unpacked-struct-member actual (needs struct support), function
`output`/`inout`. Design settled: **live-pointer** (not copy-in/out).

## Implementation notes (as landed)

Two decisions differ from / refine the sketch below; both for correctness.

**Uniform-packed one-pointer ABI (not narrow-by-type).** A ref formal is passed
as one pointer to the actual's `{ i(2W), iW }` slot, and both the ref formal's
frame slot and any ref-actual signal / frame-local are *forced packed* (`has_z`,
non-narrow) — even for a 2-state type. Rationale: a narrow-by-type formal (`{iW}`
for 2-state) silently corrupts the may plane when the *actual* is independently
forced packed by another constraint — e.g. a 2-state signal that is also a task
out/inout target keeps `{i2W,iW}` storage; a narrow-formal store of `iW` into its
field 0 overwrites the must half and leaves the may half stale, so other packed
readers see spurious x. Uniform-packed sidesteps this and is consistent with the
existing subroutine ABI, which already passes every formal packed (the narrow
form is a storage optimization, not a call-ABI one). Cost: a 2-state ref actual's
storage carries the redundant may/z halves; instcombine folds the traffic.
Forcing is via `ref_actual_targets()` (signals) and the extended
`frame_out_targets()` (locals), OR'd into `has_z` / non-narrow exactly like the
task out/inout keep-z marking. z-elision is made ref-aware (`ZElision.cpp`):
a ref formal reads the actual's z on entry and writes its z back live, i.e. it is
analyzed like a task inout.

**Sensitivity waking is serviced at the call site.** A blocking write through a
ref is a write to the actual, so it must service the actual signal's wait queue.
Servicing is per-signal static (inlined LLVM keyed on the signal's wait globals),
which the callee — holding only an opaque pointer — cannot do. Instead the call
site snapshots each ref-actual signal before the call and services its queue
after (`service_snapshot` / `service_after_write` in `lower_call_inst` /
`lower_task_call_inst`). Correct for functions (atomic, zero-time) and for a task
that writes the ref in a single time step. **Limitation:** a task that writes the
same ref signal across a `#delay` (multiple time steps) wakes level/edge waiters
only once, at task completion, with the net transition — not per intermediate
write. A generic runtime wake keyed by signal id would let the callee service
per-write; documented TODO.

## Oracle

iverilog rejects ref ports outright ("sorry: Reference ports not supported yet"),
so it is **not** an oracle here. `ref` semantics therefore rest on LRM 13.5.2
directly, exercised over the event-driven cases that distinguish live storage
from copy-in/copy-out: function ref mutation, two-ref aliasing, const ref + x
propagation, task ref observed live across a `#delay`, and a ref write waking an
`always @(sig)` (function and single-write task). The committed coverage is
listed under "Verification" below.

## Motivation & current state

`ref` on any subroutine is rejected (`Lower.cpp:5543`); functions reject
`output`/`inout`/`ref` (`:5548`). Tasks support `output`/`inout` via
copy-in/copy-out through plane pointers.

`ref` (LRM 13.5.2) binds the formal to the *actual's storage* — the callee reads
and writes it live, with no private copy. This is required, not optional, for a
**task**: a `ref` must observe writes that happen across a `#delay` mid-body,
which a copy-in/copy-out snapshot misses. For a **function** (zero-time, atomic)
copy-in/out is observationally equivalent except under argument aliasing, where
live-pointer is the correct one. Both therefore unify on live-pointer.

## Mechanism — the indirect formal

Today a formal becomes a frame `Local` with its own storage: a function copies
`Arg → Local` at entry (`Lower.cpp:5651`); a task copies in/out per direction
(`emit_task_copyin`/`emit_task_copyout`). A `ref` formal instead **is** the
passed pointer: it is passed as `(ptr mm, ptr z)` aimed at the *actual's* live
storage, and every load/store of the formal dereferences those pointers. No
frame slot, no copy-in, no copy-out.

The ABI *shape* matches task out/inout — `(ptr mm, ptr z)` — but with two
differences: (1) the pointer targets the actual's real storage, not a caller
temp; (2) the callee accesses it live instead of copy-in/out. And it applies to
functions too.

## Exact edits (touch list)

1. **`src/ir/Module.h`** — add `ArgDir::Ref` to the enum (`:130`). Update the
   "only tasks carry Out/Inout" comment. A `const ref` lowers to the same
   `Ref` dir (writeback legality is enforced by slang; the backend passes the
   pointer either way).

2. **`Lower.cpp` `function_id` guards (`:5539-5556`)** — allow `Ref` when the
   formal type is a packed scalar/vector (integral, `width == store width`);
   keep the loud-fail for array / struct / string / real ref (this chunk).
   Keep the function `output`/`inout` rejection (separate chunk). Keep string
   out/inout task rejection.

3. **`Lower.cpp` `build_function` (`:5645-5652`) and `build_task`
   (`:5604-5608`)** — populate `arg_dirs` for **both** (functions currently
   leave it empty). For a `Ref` formal in `build_function`, **skip** the
   `Store(Local(k), Arg(pos))` prologue copy — the local is indirect. Still
   allocate the Local (so formal k == Local k holds; the backend gives it no
   frame slot).

4. **`Lower.cpp` call sites** — `lower_user_call` (function) and
   `lower_task_call` (`:5643`+): for a `Ref` actual, pass the actual's storage
   **pointers** (`lv_mm_ptr`/`lv_z_ptr` of the actual lvalue), like a task
   out/inout temp but pointing at the actual itself. The actual must be an
   lvalue (slang enforces). This chunk: require a whole-variable scalar/vector
   lvalue (whole slot); guard bit/part-select / array-element ref actuals.

5. **`LlvmBackend.cpp` `declare_function` (`:1251`) / `declare_task`
   (`:1294`)** — a `Ref` formal contributes `(ptr mm, ptr z)` params (like task
   out/inout). `param_indices` (`:1329`) already strides 2 for non-string; a
   ref formal is 2 params, so it composes.

6. **`LlvmBackend.cpp` frame setup (`:1358-1420`) + Local addressing** — a
   `Ref` local gets **no** frame-struct field; instead record its `(mm, z)` arg
   pointers (a `frame_ref_ptr_[localidx] = {mm_param, z_param}` map, filled in
   the callee prologue). The Local lvalue resolution (`Tag::Local`, ~`:2422`)
   for a ref local returns those pointers as `lv.base`/mm-ptr/z-ptr instead of a
   frame GEP. `frame_out_targets` stays packed for ref locals (they are accessed
   through the packed-word pointer ABI, never narrow).

7. **`LlvmBackend.cpp` call sites `lower_call_inst` / `lower_task_call_inst`** —
   for a `Ref` arg, push the actual lvalue's `lv_mm_ptr`/`lv_z_ptr` (the actual's
   own storage), not `(pl.mm, pl.z)` and not a caller temp.

8. **`emit_task_copyin` / `emit_task_copyout` (`:1632`/`:1655`)** — a `Ref`
   formal is skipped entirely (no copy either way).

## Backend ABI (refined — makes the edits mechanical)

**A ref formal is passed as ONE pointer** to the actual's `{i(2W) mm, iW z}`
slot struct (not the two `(mm,z)` pointers task out/inout uses). The callee's
`local_lvalue` for that formal returns `base = passed pointer`, and
`lv_mm_ptr`/`lv_z_ptr` GEP it exactly like any other slot — so loads/stores are
live with zero special-casing in the op paths. `param_indices` strides **1** for
a ref formal (like a string-in), 2 otherwise.

**Indirect local:** keep the ref formal's (dead) frame field for index==field
alignment; in the callee prologue store the passed arg pointer into a side map
`ref_ptr_[localidx]`. `local_lvalue` (`:5760`): `if (ref_ptr_[index]) base =
ref_ptr_[index]` else the frame GEP. Nothing else in the load/store paths
changes.

**Layout matching (the only subtlety), resolved:** LRM 13.5.2 requires actual
type ≡ formal type, so narrow-ness matches by construction —
- 2-state ref: actual & formal are both narrow `{iW}`; the ref formal is **not**
  an out_target (keep its natural narrow slot). No z.
- 4-state ref: actual & formal are both `{i2W, iW}`. Frame-local actuals already
  keep z; only a **signal** actual can be z-elided, so mark 4-state ref-actual
  signals to keep z (reuse the existing out/inout "keep-z" signal marking —
  `SignalSlot.has_z`), forcing `{i2W, iW}` to match the formal.

**Call site** (`lower_call_inst`/`lower_task_call_inst`): for a ref arg push the
actual lvalue's `base` (its slot pointer) — a signal's `storage_ptr`, a frame
local's frame GEP. One value, not a plane pair.

**Pointer validity across a task suspend:** ref actuals are module signals or
caller frame locals; both outlive the callee, so the raw slot pointer stays
valid across `#delay`.

## Restrictions (this chunk) — loud-fail, TODO

- Array / unpacked / struct / string / real `ref` type.
- A `ref` actual that is not a whole-variable lvalue (bit/part-select,
  array-element) — passing a sub-slot pointer needs offset plumbing.
- Function `output`/`inout` (unchanged rejection; separate chunk).

## Correctness

- **Task ref** sees live mid-`#delay` writes: the pointer targets the actual's
  real storage, and the callee loads/stores it on every access; a suspend does
  not snapshot.
- **Function ref** handles aliasing: two `ref` actuals aliasing the same
  variable, or a `ref` aliasing a global the body reads/writes, resolve
  correctly because there is a single live storage location.
- The pointer stays valid across a task suspend: ref actuals are module signals
  or caller frame locals, both stable for the callee's lifetime.

## Verification

- iverilog-checked integration tests: `ref` scalar mutated by a function and
  reflected in the caller; `ref` mutated by a task across a `#delay` (the case
  copy-in/out would get wrong); `const ref` read; aliasing (two refs to one
  var); a ref actual that is itself a module signal vs a caller local.
- Full suite byte-identical; the guarded cases (array/string/partial ref)
  confirmed still loud-fail.
- Gate: none needed (a rejected construct becoming supported; no perf-sensitive
  default path changes).
