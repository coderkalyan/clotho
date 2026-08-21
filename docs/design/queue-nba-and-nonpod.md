# Queue follow-ups: NBA to a queue, and non-POD queue elements

**Status: both landed.** §1 (whole-queue NBA) landed on `feat/queue-ops`; §2
(non-POD elements) landed on `feat/queue-nonpod`, for dynamic arrays at the same
time — they share `DynElem` and `rt_elem_ops`, so the element capability is one
gate. What follows is kept as the design record; the parts that turned out to be
wrong when the work was done are marked, because both were load-bearing
assumptions.

---

## 1. NBA to a queue / queue element

`q <= rhs` and `q[i] <= x` (LRM 7.10, 10.4.2). Whole-queue `q <= rhs` is
**landed** (see below); `q[i] <= x` (element) is still loud-guarded.

### Whole-queue `q <= rhs` — LANDED

A dedicated `QueueCopyNb` op: the frontend materializes the RHS (var / pattern /
concat / slice — reuses `materialize_queue_rvalue`), then in the Active region
`emit_queue_whole_nba` reserves a `{ ptr dest_handle, rt_queue staged }` record
in a per-queue `rt_nba_arr_*` buffer (indexed after the array + aggregate
buffers) and deep-copies the RHS into `staged`. The NBA region
(`emit_queue_whole_commit`) `rt_queue_move`s each staged buffer into its live
handle (ownership transfer, no second copy; frees the target's prior elements,
so managed string/nested elements are lifecycle-correct) and wakes Any-edge
readers. `dest_handle` is captured in Active because the *signal slot* never
moves (only its heap buffer does, which the move handles). `nba_classify` scans
for `QueueCopyNb`; the record buffer is reset each NBA pass. Verified against
verilator (2-state, clocked register pattern) and the whole test suite; string
elements move-commit without leak. Same machinery will extend to dynamic arrays
(`arr <= ...`).

### Element `q[i] <= x` — ILLEGAL SystemVerilog, not a gap

LRM 10.4.2 forbids a nonblocking assignment to an *element* of a
dynamically-sized array (queue or dynamic array). slang enforces this and
rejects `q[i] <= x` / `d[i] <= x` before Clotho ever sees them
("nonblocking assignments to elements of dynamically sized arrays are not
allowed"). This is precisely how the LRM sidesteps the sample-vs-commit realloc
hazard analysed below: by disallowing the construct entirely. So there is
**nothing to implement** — Clotho never needs a queue-element NBA path. The
analysis below is retained only as the rationale for why the construct is
illegal (and would be unsafe if it weren't).

### Why the scalar path does not work

A whole-signal scalar NBA stages the RHS into the signal's *shadow* slot (a
statically-keyed, plane-shaped mirror at the end of root state) and the NBA
region copies shadow -> live (`nba_block` block-commit). A queue's value is a
heap `rt_queue` handle, not a plane slot, so there is no shadow to stage into.

### Precedent to reuse: the array-element deferred-record path

`mem[i] <= rhs` already can't use the per-signal shadow, so it defers through a
per-array growable record buffer (`rt_nba_arr_slot` reserves a `{dest_ptr,
value}` record in the Active region; a backend-emitted commit loop in the NBA
region applies them; `rt_nba_arr_any` gates the pass; `rt_nba_arr_reset` clears
counts, keeping capacity). This is exactly the shape a queue NBA needs.

### Design

**`q[i] <= x` (element NBA).** Reuse the deferred-record machinery, but key the
record by **(handle*, logical index, sampled value)**, resolving the element
address at *commit* time via `q_elem(handle, index)` — NOT by sampling the
element pointer in the Active region. A queue can be `push`/`pop`'d between the
Active sample and the NBA commit within the same time step, relocating its
buffer; a sampled pointer would dangle. At commit, if `index >= len` (the queue
shrank), skip the write (a defensible reading of an out-of-range deferred
target; document it as a deviation — iverilog crashes on queues, so there is
nothing to check it against). New record type + a queue-element commit loop;
`nba_classify_ir` must learn the queue-element StoreNb shape (root through
`ElemSelect` over a queue).

**`q <= q2` (whole-queue NBA).** Sample `q2` in the Active region by
**deep-copying it into a per-record owned buffer** (value semantics, LRM 7.10),
then in the NBA region `rt_queue_assign(dst_handle, staged_copy)` and free the
staged copy. Record holds `{dst_handle*, rt_queue staged}`. A `'{...}` / concat
/ slice RHS first materializes into the staged copy (reuse
`materialize_queue_rvalue`, then move into the record). New record type + commit.

**Bit/part-select element NBA** (`q[i][3:0] <= x`) keeps the per-element
read-modify-write shape the array path already documents.

### Effort / risk

Moderate. Bounded to the NBA region (`nba_classify` + a new commit loop +
runtime record types); the wheel / scheduler / block-commit are untouched.
Main correctness risk is the sample-vs-commit aliasing (addressed by
index-at-commit for elements, deep-copy-at-sample for whole handles). ~1-2 days,
one branch. Same machinery extends to dynamic arrays (`arr <= ...`,
`arr[i] <= ...`) and associative arrays for free.

---

## 2. Non-POD queue elements (queue-of-struct / nested container) — LANDED

`struct {...} q[$]`, `int q2[$][$]`, `C q[$]`, and the same for a dynamic array.
The runtime *lifecycle* was already done — `rt_elem_ops` (init/copy/deinit)
recurses nested containers and string leaves (`feat/compose`), and `rt_queue_*`
already drove it — so the work was frontend/backend addressing, as predicted.

### What the two "missing pieces" turned out to be

1. **Element-select lvalue into a heap-rooted aggregate.** The prediction that
   `dynarr_elem` cannot classify a recursable aggregate was **already stale when
   written**: the associative-array value-element work had added `is_agg` /
   `agg_ty` to it, and `AssocSelect` was already producing a heap-anchored
   aggregate `LValue` (deferred base, resolved at Load / Store). So this reduced to
   one branch in the queue / dynamic-array `ElemSelect` arms —
   `dyn_agg_elem_lvalue` — and it is SIMPLER than the assoc one: a container
   element's slot address is known at select time, so the base is concrete instead
   of deferred. Everything downstream (`agg_member` / `agg_elem` descent,
   `AggCopy`'s `dyn_inrange` gating, `dyn_elem_load`'s per-leaf out-of-range
   default, `AggEq`) then worked unchanged, which is exactly the payoff the entry
   predicted.

2. **Whole-element aggregate rvalue staging.** Correct as written, and cheaper than
   expected: `rt_queue_*` already took `elem` / `out` as `void*`, so nothing about
   the ABI changed — push / insert hand over `agg_slot_ptr` of the source (the
   runtime's `rt_elem_copy` never retains it) and the frontend guarantees an lvalue
   operand, staging a `'{...}` actual into a canonical aggregate temp the way an
   aggregate associative-array key already did.

### What the entry did not anticipate

- **A managed leaf needs the kind-4 descriptor.** `rt_elem_aggregate` (kind 4) was
  fully implemented in the runtime and *never emitted*: `elem_ops_global` hardcoded
  `nleaves = 0` / `leaves = null`, so every aggregate element was described as kind
  0 (POD memcpy). Sound only while the frontend refused a managed leaf. Emitting an
  offset-sorted `{offset, ops}` table (the order is a runtime contract:
  `rt_elem_copy` raw-copies the POD gaps *between* leaves) is what admits
  `struct { int a; string s; } q[$]`.
- **A pop transfers ownership.** `rt_queue_pop_*` RELOCATES the element into the
  caller's buffer, so for an aggregate or nested-container element that buffer owns
  heap storage. It is therefore body-lifetime, not per-site scratch: the pop site
  releases the previous contents (a pop in a loop) and every body exit releases the
  last — the same shape the string-element temp already had, for the same reason.
- **An out-of-range nested container element needs a sink**, not just a gate: the
  access is a method call, not a value read.
- **`str_slot_ty` was under-aligned.** `{ [32 x i8] }` (align 1) mirroring an
  `alignas(8)` `rt_string` meant a string leaf inside any aggregate — a plain struct
  signal included — was accessed misaligned. Pre-existing UB, found because element
  images made it common.

### Still guarded

An associative-array element (needs a new `rt_elem_assoc` descriptor kind), an event
element (per-element trigger state, LRM 15.5), and — on the associative-array side —
an aggregate VALUE element with a managed leaf: the kind-4 table it needs now exists,
but a leaf string / handle access reaches `agg_slot_ptr` with the assoc base still
deferred, which the backend refuses. Resolving that means choosing get_ptr vs put_ptr
per leaf access, which is the one thing a queue element never has to do.

## 3. NBA to a `string` — LANDED

A string is Managed like a queue, so it has the same problem and takes the same
shape: the per-signal NBA shadow is a plane-shaped integer mirror, and an
`rt_string` is a 32-byte owning handle. Committing it by memcpy out of a byte
image would leave two owners of one heap buffer and free it twice.

What it does NOT reuse is `HandleCopyNb`. That op's payload is a source handle
*lvalue* (`QueueArg`), consumed through `lvalue_of` + `lv_handle_ptr`; a string
rvalue is often a body temp with no LValue at all, so the accessor does not
resolve. The ordinary `StoreNb` already carries everything needed — a string
lvalue and a string-valued operand (`str_of` is total over string instructions) —
so the string path is a branch in `StoreNb`, not a new IR tag.

**The record.** A fourth range of the shared `rt_nba_arr_*` index space (after
array, aggregate and container), `{ ptr dest, rt_string staged }`:

- **Active region** (`emit_string_whole_nba`): reserve the record, store the
  destination handle's pointer, **`rt_string_init` the staged handle**, then
  `rt_string_assign` the RHS into it. The staged handle owns its value from here.
- **NBA region** (`emit_string_whole_commit`): `rt_string_move(dest, staged)` per
  record, in push order. The move releases what the destination held, so a target
  written twice in one instant keeps the last write and frees the superseded buffer
  exactly once; it also re-inits the staged handle, so the record's retained
  capacity is a valid empty string for the next instant.

**Two deliberate differences from the container recipe.**

1. **`init`, not `memset`.** Every container handle's empty image is all-zero; an
   empty `rt_string` is not (byte 31 holds the inline capacity). A memset here
   would make `rt_string_assign` read a bogus capacity out of the record.
2. **No wake, no VCD touch.** A string is refused from sensitivity lists and event
   controls in the frontend, so no process can be waiting on one, and a string has
   no `$var` form. The container commit's `service_any_waiters` / `rt_vcd_touch`
   have no counterpart.

**Leaves come free.** The record carries the destination *pointer*, not a signal
identity, so a string member of an unpacked struct and a string element of a fixed
unpacked array use the same range keyed on their ROOT signal — which is also what
closes a whole-aggregate NBA carrying a string member (`emit_agg_copy` decomposes
it into one per-leaf `StoreNb`). Classification is by lvalue shape
(`lvalue_is_string_place`), tested ahead of the array and aggregate branches so
those never claim a string store; a signal may appear in both ranges (a struct with
a string member and an integral array member takes both, at distinct indices).

### Still guarded

`s[i] <= c` — one character. That is a masked write into an existing value, not the
replacement of a whole handle, so it needs a record carrying an index and a byte
rather than a staged string. `s <= #5 t` also stays refused: intra-assignment
timing rides the separate `rt_intra_*` pool, whose record is a fixed-width packed
value. An NBA to a string class property is refused for the reason every class
property is (staging is signal-keyed; an object body has no shadow).
