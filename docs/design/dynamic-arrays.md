# Variable-size arrays: dynamic arrays, queues, associative arrays

Design for SystemVerilog variable-size aggregate types (LRM 7.5 dynamic
arrays, 7.10 queues, 7.8/7.9 associative arrays).

**Status: dynamic arrays, queues, and associative arrays landed; managed
elements landed.** Dynamic arrays are complete LRM 7.5 (new/size/delete/index/
foreach, locals, in/out/inout/ref args, return); queues are complete LRM 7.10
core (push/pop both ends, insert/delete(i)/delete(), size, `q[i]`/`q[$]`, bounded
`[$:N]`, foreach, deep-copy, Table 7-1 defaults, reverse/sort/rsort/shuffle
(7.12.2), the locator and reduction families with their `with` iterator clause
(7.12.1/7.12.3/7.12.4), and the same in/out/inout/ref args + return ABI).
Associative arrays
(LRM 7.8/7.9) are complete for integral/enum/packed/string/`[*]`-wildcard keys
(index read/write, num/size/exists/delete(k)/delete/first/last/next/prev,
ascending-key foreach, deep-copy assign, `'{key:value, default:v}` literals
(7.9.11), in/out/inout/ref args) with an integral/real value element — see §4a for
the key model. `$size` lowers to `.size()` (LRM 20.7). Neither iverilog nor
verilator can serve as a reference for assoc — iverilog cannot parse an
associative-array declaration, and verilator violates the read-no-create rule —
so the behaviour is pinned against LRM 7.8/7.9 by
`tests/associative_array_test.cpp`. Value elements that are themselves managed
(string / nested container) are the one guarded assoc gap.

**The POD-only element cut is superseded.** The `rt_elem_ops` element-descriptor
ABI (feat-compose) threads a per-element-type stride + deinit/copy function
pointer through every runtime call, so dynarr/queue elements may now be managed
types (`string`, nested dynarr/queue, class handle) -- not just POD. `chandle`
(LRM 6.14) landed alongside a class handle -- both are a bare 64-bit word with
no lifecycle, admitted by the same `guard_container_element` gate; the two
differ only in whether the GC traces the word (a chandle is a foreign DPI
pointer clotho never allocated and must not trace, docs/design/
garbage-collection.md §0), which falls out of `chandle` lowering to a plain
`Kind::Int` rather than `Kind::Class` and needs no dedicated code. Any new
container design (associative arrays below) must be built on `rt_elem_ops` from
the start, not the original POD-first memcpy path.

The rest of this doc is the original scope-out; §4a and the phasing in §11 track
what landed vs deferred. Remaining per-collection gaps (each loud-fails): NBA to
a dynarr/queue/assoc target, slicing `q[a:b]`, `'{...}`/concat init for a dynarr
or queue *member* of an aggregate, a `with` clause on `sort`/`rsort`, the 7.12
manipulation methods on an associative array or a fixed-size unpacked array,
multidimensional, an aggregate-valued assoc `'{...}` literal, and a managed
(string / nested-container) assoc value element.

Before it landed, all three were one loud-fail in `lower_type`:

```cpp
if (ct.isUnpackedArray()) {
    // TODO: dynamic arrays, queues, associative arrays ...
    fail("dynamic / queue / associative arrays are not supported");
}
```

No IR type, no runtime storage, no method dispatch exists. slang's
`DynamicArrayType` / `QueueType` / `AssociativeArrayType` are unreferenced in
the tree; only `FixedSizeUnpackedArrayType` is lowered.

## 1. Guiding decisions

- **Value types.** Assignment (`a = b`) and subroutine pass-by-value are deep
  copies (LRM 7.5/7.9/7.10) — the same discipline `rt_string` already uses.
- **Reuse quality C++ where it earns its keep, hand-roll where the stdlib
  container is poor.** `std::deque` (queue) and `std::map` (assoc) are the weak
  ones and are avoided. See §4.
- **Element model reuses the existing plane machinery** — no new value
  representation (§3).
- **Phased scope cuts, each behind a loud-fail guard + TODO** (never silently
  accept): POD elements first, blocking-assign first. See §2.

## 2. Scope and phased cuts

First cut restrictions, each keeping a guard and a documented TODO:

- ~~**POD elements only.**~~ **Superseded — managed elements landed.** The
  original v1 collapsed copy/deinit to `memcpy` + `free` and loud-failed
  `string`/class-handle/nested elements. The `rt_elem_ops` descriptor ABI now
  carries a per-element deinit/copy function pointer, so managed elements work
  for dynarr/queue. Associative arrays inherit this and are **not** POD-first
  (§6 signatures take `rt_elem_ops`).
- **Blocking assign only.** NBA to these types loud-fails (matches the existing
  "NBA to string" gap). Whole-signal NBA staging keys on a fixed shadow slot
  and cannot represent a heap handle; deferring is consistent with strings.
- **Associative keys — every key kind** (revised): integral of any width,
  `[*]` wildcard, enum, packed struct/array, `string`, **class handle** (LRM
  7.8.3) and **unpacked struct / fixed array** (LRM 7.8.5). All unified under the
  normalized byte-span key model (§4a); the only remaining exclusion is a string
  or dynamically-sized member *inside* a struct key (variable-length span).
- **Streaming operators — granule-aligned only** (LRM 11.4.14, added with
  pack/unpack into containers). A queue, dynamic array or string on either side of
  `{>>{...}}` has no lower-time width, so the stream stays a list of chunks
  (`StreamChunk` in `Lower.cpp`) and the destination consumes it granule by
  granule: one element per element width, one byte per byte. Everything whose
  chunk boundaries do not line up with the destination's granule is refused with
  the offending width in the message and needs a bit-level buffer instead: a
  6-bit-element queue into a string, a 12-bit value into a byte queue, `{<<S{...}}`
  over a dynamically sized stream, a dynamically sized source in an unpack. Only
  the dynamic-array destination needs the element count up front (it is allocated,
  not pushed onto), and `stream_granule_count` computes it — with a runtime
  accumulation loop when the count depends on string lengths.

## 3. Element value model

An element is exactly the backend's existing per-element slot (`slot_type()` in
`src/ir/LlvmBackend.cpp`):

- `{ i(2W) }` — 4-state, never-z (must in low W bits, may/unknown in high W),
- `{ i(2W), iW }` — 4-state with z plane,
- `{ iW }` — narrow 2-state,
- 8-byte `real`.

**Element stride = `sizeof(slot_ty)`.** Wide (>64-bit) elements fall out for
free (the slot is a wide `iW`). The runtime containers are byte/stride-generic
— exactly like the NBA `ArrQueue` record buffer, which already stores
backend-defined records in a `std::vector<uint8_t>`
(`src/runtime/Runtime.cpp:848-897`). The JIT monomorphizes element load/store
by reusing `lv_field_ptr`, with the base pointer taken from the handle instead
of a state GEP (§7).

## 4. Container choices

| Type | Backing | New dep |
|---|---|---|
| Dynamic array | raw `malloc` buffer, no container | none |
| Queue | hand-rolled growable ring over a raw buffer | none |
| Associative | `ankerl::unordered_dense::map` + lazy sorted-key index | none (already vendored) |

### Dynamic array — raw buffer

`new[n]` sizes once, `len == cap` always, storage is contiguous. `std::vector`'s
amortized growth buys nothing; a bare `{ data, len }` is simplest and gives
inline GEP element access. Resize (`new[m](src)`) is malloc-new + copy-old.

### Queue — hand-rolled ring, not `std::deque`

A queue needs amortized O(1) at *both* ends plus indexed access `q[i]` and
slicing. `std::deque` is the poor stdlib container here and, being segmented,
would force every `q[i]` and every iteration step through a runtime call. A
growable ring buffer over a raw `malloc`'d region — `{ base, head, len, cap }` —
gives O(1) push/pop at both ends and keeps storage contiguous, so `q[i]` stays
an inline address computation `base + ((head + i) % cap) * stride`. It is ~30
lines and beats `std::deque` on both axes; not meaningfully "reinventing
`std::vector`."

### Associative — ankerl hash + lazy sorted index, not `std::map` / `flat_map`

Ordered `.first` / `.next` / `.prev` / `.last` traversal is mandated (LRM
7.9.3, ascending key). The two obvious ordered stdlib containers are poor fits:

- `std::map` — red-black, one heap node per element, cache-hostile.
- `std::flat_map` (C++23) — sorted parallel vectors: native order and
  cache-friendly, but **O(n) insert/erase**. SV assoc arrays are usually built
  incrementally (fill a scoreboard/coverage map key-by-key), so a build-up is
  O(N²). Also has a `<flat_map>` toolchain-availability risk on the static
  build.

Instead: `ankerl::unordered_dense::map` (already a project dependency, flat and
top-tier) keyed by the **normalized key bytes** (§4a), giving **O(1)** `a[k]`
access — the assoc hot path. Ordered traversal is served by a **lazily-built
sorted key array**, rebuilt on the first `first`/`next` after any mutation and
cached until the next insert/delete. Iteration cost is a single sort per
mutation-batch, amortized across the whole `first`→`next`* walk. Pointer
stability is not required: element `get_ptr` is always immediately followed by
the load/store, never held across a mutation.

Revisit `flat_map` only if a fill-once-iterate-many workload ever dominates.

## 4a. Key model — normalized order-preserving byte span (all key kinds)

**Status: implemented.** All key kinds below land in v1. There is no external
reference for assoc — iverilog 13 cannot parse an associative-array
declaration, and verilator violates LRM 7.8 by creating an entry on a read of an
absent key.

**One uniform key representation folds every LRM 7.8 index type — integral of
any width, `[*]` wildcard, enum, packed struct/array, `string` — into a single
runtime path.** A key is a byte span `{ const uint8_t* ptr, uint32_t len }`
holding a **normalized** form with two properties: (1) equal keys ⇒ equal bytes
(so the hash map dedups correctly) and (2) unsigned lexicographic byte order ==
the LRM ascending traversal order for that key type. Given both, the runtime is
**key-kind-agnostic**: the map hashes the raw span, equality is `memcmp`, and the
sorted index is a plain lexicographic sort (`memcmp`, shorter-is-less on a
prefix) — no per-kind branching anywhere in the container.

All key-kind knowledge collapses into two tiny **JIT-monomorphized converters**
per key type (the `rt_elem_ops` philosophy applied to keys — runtime generic,
backend specializes):

- **normalize** (user value → span): emitted inline before every keyed call.
- **denormalize** (span → user value): emitted after `first`/`next`/`prev`/`last`
  to write the ordered key into the `foreach` loop variable.

Normalization per kind:

| Key type | normalize | order property |
|---|---|---|
| unsigned integral (fixed width `W`) / enum | big-endian bytes, `ceil(W/8)` wide | fixed len ⇒ `memcmp` == numeric |
| signed integral | big-endian, **flip the MSB** (offset-binary) | two's-complement → numeric order |
| packed struct / array | its `$bits` treated as unsigned integral | falls out of the integral row |
| `[*]` wildcard | zero-extend to a **fixed 64-bit** unsigned big-endian form | fixed len ⇒ `memcmp` == numeric, and every numerically-equal index of any width zero-extends to the same 8 bytes — `8'd5` and `32'd5` are the *same* key (LRM 7.8.1 width-agnostic wildcard equality). A wildcard index wider than 64 bits truncates (documented limitation; typed keys of any width are exact). |
| `string` | raw char bytes, no transform (variable len) | `memcmp` + length == SV string compare |
| **class handle** (LRM 7.8.3) | the handle word as a 64-bit unsigned big-endian key | identity: equal handles ⇒ equal bytes, `null` is the all-zero key. Order is handle order, which 7.8.3 explicitly leaves "deterministic but arbitrary" — stable for a whole run, since objects are never moved or freed |
| **unpacked struct / fixed array** (LRM 7.8.5) | each leaf's integral row, concatenated in declaration order (depth first) | fixed len ⇒ `memcmp` == lexicographic by member, each member in its own signedness. 7.8.5 leaves the order free where `<` is undefined ("can vary … shall remain the same within the same simulation run"); this one is total and deterministic |

(The fixed-64 wildcard form replaced an earlier variable-length "strip leading
zeros + length prefix" sketch — plain lexicographic order is not numeric for
variable-length keys, so a fixed width is both correct and simpler. Typed
integral keys keep their own fixed `ceil(W/8)` width, exact at any bit width.)

Because normalization is order-preserving, `first`/`next`/`prev`/`last` return
normalized spans that sort correctly with zero container awareness; the JIT
denormalizes on readout (un-flip + reassemble for integral; build an `rt_string`
for a string key).

**Map key storage.** The runtime is `ankerl::unordered_dense::map<std::string,
std::vector<uint8_t>>` keyed by the normalized bytes; the value vector is the
element slot (`ops->stride` bytes), which may carry managed sub-handles serviced
through `rt_elem_ops`. `std::string`'s SSO holds a short key (≤~15 B) inline, so
the common short-key build-up — an integral or short-string scoreboard filled
key-by-key — takes no per-entry key malloc. Keys and element values are owned by
the map: `delete` frees them, `assign` deep-copies keys *and* elements
(per-value via `rt_elem_copy`). Ordered traversal is a lazily-rebuilt
`vector<const std::string*>` sorted by an explicit unsigned-`memcmp` comparator
(not `std::string::operator<`), invalidated on any insert/delete.

**Fixed vs variable width** is the only thing the JIT-side converter needs to
know, and it is a compile-time constant of the declared index type — so the two
converters are branch-free straight-line code, not a runtime dispatch. `next`/
`prev` take the current key span and binary-search the sorted index
(`upper_bound` / `lower_bound`), so they need not be a present key (LRM 7.9.4).

**Why a struct key is serialized, never memcpy'd.** `agg_storage_ty` builds a
*non-packed* LLVM struct, so the storage image has ABI padding holes, and a frame
aggregate is a bare `alloca` whose padding is never written (`emit_agg_init` touches
leaves only). Two structs with equal members can therefore differ byte for byte —
fatal for both the hash and the ordering. Walking leaves also gets the per-leaf
signedness right (`{int hi; byte lo}` orders by `hi` *signed* first) and is where the
x/z check falls out. `agg_key_bytes` is a pure type walk, so the length stays a
compile-time constant and the runtime comparator's shorter-is-less tiebreak never
fires within one array.

**An invalid (x/z) index** (LRM 7.8.4 for integral, 7.8.5 for a struct: "an index
expression that is or contains x or z … is invalid") is gated, not normalized: the
converter returns an `i1` alongside the span, false when any bit (or any leaf) is
unknown, which rides the same `dyn_inrange` predicate a dynamic-array element uses.
7.8.6 then falls out — a read yields the element default with no entry created, a
write is dropped (and `put_ptr` is branched around, so `num` never sees the key),
`exists` answers 0 and `delete` is a no-op. `next`/`prev` are unaffected: their key
is a *probe* (7.9.4 allows an absent one) and the clause defines no invalid-probe
behaviour, so it normalizes as its must bits. Before this, an x key normalized to
its must plane, which silently aliased `a['x]` onto `a[0]`.

Deferred: a **string or dynamically-sized member inside a struct key** (LRM 7.8.5
allows it, but the span would be variable-length, so the fixed-width leaf layout —
and the shorter-is-less order — does not model it); guarded loud. **String /
nested-container VALUE elements** are guarded loud (the a[k] read/write would have
to route a handle, not a plane) — keys of every kind are supported regardless.

## 5. Handle ABI (follows `rt_string`)

Each type gets a fixed-size, **caller-owned, inline-in-state** handle blob,
mirroring the 32-byte `rt_string` (`src/runtime/Runtime.h:386-388`). Each has
its own backend `slot_ty` and a power-on constant = empty handle (built as an
LLVM constant like `slot_const` does for strings, so no per-signal init store
for scalars; arrays of handles get a runtime init loop like string arrays).

- **Dynamic array**: `{ uint8_t* data; uint64_t len; }` (16 B). `len` inline ⇒
  `$size` / `foreach` bound needs no runtime call. Element access inline.
- **Queue**: `{ uint8_t* base; uint64_t head; uint64_t len; uint64_t cap; }`
  (32 B). `q[i]` inline via ring math; push/pop/insert/delete cross to the
  runtime (may realloc/rebalance and write all four fields back).
- **Associative**: `{ void* impl; }` (opaque pointer to the ankerl map + its
  cached sorted index). Every op is a runtime call.

Lifecycle mirrors strings (`emit_string_slot_op`, `deinit_body_strings`,
teardown loop at `LlvmBackend.cpp:2029-2036`): `init` (empty, no alloc),
`deinit` (free buffer/map), `assign` (deep copy), and copy-in/out for
subroutine args. Body temporaries and function-frame locals are bracketed by
init/deinit at body exits exactly as string temps are. State teardown deinits
every handle on both the natural-drain and `$finish` paths.

## 6. Runtime surface

New `rt_*` functions, C-ABI, every handle passed by pointer (like `rt_string`):

```
Dynamic array:
  rt_dynarr_new(h, stride, n)              // (re)alloc; X/0-fill per element
  rt_dynarr_new_copy(h, stride, n, src)    // new[n](src): grow/truncate copy
  rt_dynarr_delete(h)                       // free -> size 0
  rt_dynarr_assign(dst, src, stride)        // deep copy
  // size(), a[i] read/write: inline, no call

Queue:
  rt_queue_push_back / push_front(h, stride, const void* elem)
  rt_queue_pop_back  / pop_front (h, stride, void* out)
  rt_queue_insert(h, stride, i, const void* elem)
  rt_queue_delete_idx(h, stride, i) / rt_queue_delete_all(h)
  rt_queue_assign(dst, src, stride)
  // size(), q[i]: inline ring math

Associative (any key kind — key passed as a normalized byte span, §4a):
  rt_assoc_get_ptr(h, ops, kptr, klen)  // -> element ptr; absent -> the '{default:} slot, else null
  rt_assoc_put_ptr(h, ops, kptr, klen)  // -> element ptr, creating (element-default-filled) entry
  rt_assoc_default_ptr(h, ops)          // -> the '{default:} slot, creating it (LRM 7.9.11)
  rt_assoc_exists(h, kptr, klen)   rt_assoc_delete_key(h, ops, kptr, klen)
  rt_assoc_delete_all(h, ops)      // a.delete(): entries only, the default survives
  rt_assoc_reset(h, ops)           // whole-value replacement + teardown: entries AND default
  rt_assoc_num(h)
  rt_assoc_first/last(h, kptr_out, klen_out)     // ordered ends; 0 if empty. span into stable map storage
  rt_assoc_next/prev(h, kptr_inout, klen_inout)  // successor/predecessor of the given key; 0 at end
  rt_assoc_assign(dst, src, ops)                 // deep copy: keys (map-owned bytes) + elements (ops->copy) + default
```

- **`ops`** is the `const rt_elem_ops*` descriptor (element stride + deinit/copy
  fn pointers) already used by `rt_dynarr_*` / `rt_queue_*` — managed *element*
  types work with no assoc-specific lifecycle code.
- **Keys are normalized spans** (§4a); the runtime never learns the key kind.
  `next`/`prev` binary-search the current key in the cached sorted index
  (O(log n)/step, unique keys ⇒ exact position) and return the neighbour's span,
  robust to the caller holding no opaque iterator.
- The returned key/element spans point into stable map storage, valid until the
  next mutation — `get_ptr`/`first`/`next` are always immediately consumed by a
  load/store/denormalize, never held across an insert or delete.
- The runtime owns the map + cached sorted index + the key bytes; entry storage
  and z-ness are described by `ops`, matching the dynarr/queue split.
- **The `'{default: v}` value** (LRM 7.9.11) is a per-map slot beside `entries`,
  not an entry: `num`/`exists`/`first`/`next` never see it, `a.delete()` keeps it
  (hence the delete_all / reset split above), a whole-array copy carries it, and a
  newly created entry is filled from the ELEMENT default rather than from it
  (§8). It is what `get_ptr` returns on a miss, which is why the
  backend's absent-key path needs no change: a hit on the default slot is an
  ordinary in-range element read, including for a `string` element, whose
  empty-string sink redirect would otherwise fire.

## 7. Frontend / backend lowering

- **IR + type pool** (`src/ir/Ir.h`): `DynArray` / `Queue` `Type::Kind` landed;
  add `Assoc` carrying an `element` type ref plus a `key` ref. Replaces the
  `fail("associative arrays are not supported")` that used to sit at the
  `isUnpackedArray()` guard in `src/Lower.cpp` (that guard is gone: all four
  unpacked-array kinds are matched by type above it).
- **`SignalSlot`**: `is_dynarr` / `is_queue` / `is_assoc` flags; `slot_ty` = the
  handle blob; power-on constant = empty handle.
- **`LValue` extension**: today `LValue.base` is always a state GEP
  (`LlvmBackend.cpp:254-290`). Add a "base pointer loaded from a handle field"
  form. `ElemSelect` on a dynarr/queue loads `data`/`base` from the handle and
  computes the element address inline (reusing `lv_field_ptr` for the plane
  load/store); on an assoc it calls `rt_assoc_get_ptr` (read) or
  `rt_assoc_put_ptr` (write).
- **Method dispatch**: map slang's method-call node to the `rt_*` calls
  (`new[]` is a `NewArrayExpression`). *Confirm at impl* whether slang models
  `.size`/`.push_back`/`.exists`/… as `SystemSubroutine` calls or a builtin
  array-method node — currently unreferenced in-tree.
- **`foreach`**: dynarr/queue bound from inline `len` landed; for assoc replace
  the `fail("foreach over an associative array dimension is not supported")`
  guard with an ordered `first`/`next` walk binding the loop var to the key.
- **Query funcs**: `$size` / `$dimensions` / `$left` / `$right` / `$high` /
  `$low` (TODO.md M3 array-query item), plus `.size` / assoc `.num` / `.exists`.

## 8. Semantics

- **Dynamic array / queue OOB**: read out of range → element default (`'x` for
  4-state, 0 for 2-state) + warning; write out of range → no-op / warning per
  LRM 7.5.1, 7.10. Pop from an empty queue → default + warning.
- **Associative**: read of an absent key → the array's `'{default: v}` value when
  one was set (LRM 7.9.11, no warning), else the element default and **no
  creation** (`rt_assoc_get_ptr` returns null → materialize the default in a
  temp); write creates the entry (`rt_assoc_put_ptr`).
- **Associative literal corners the LRM leaves open** — decided here, and
  decided together, so that the `'{default: v}` slot behaves as part of the
  array's value rather than as an entry: the default survives `a.delete()`; it
  travels with a whole-array copy, and copying an array that has none clears the
  target's; a created entry is filled from the ELEMENT default, not the pattern
  default, so a partial write to an absent key (`d[5][0] = 1'b0` over
  `'{default: 8'hAA}`) reads back `8'bxxxxxxx0`; and `'{k:v}` with no `default:`
  clears a previously set one, because the literal is the array's whole new
  value.
- **Copy**: assignment and pass-by-value are deep copies; for POD elements this
  is `memcpy` of the buffer (dynarr/queue) or an entry-wise copy (assoc).
- **Queue bound** `q[$:N]`: enforce max length on push (LRM 7.10.4 — excess push
  is dropped with a warning). Confirm exact drop-end vs error against iverilog.

## 9. Lifecycle (managed elements landed)

For POD elements, `deinit` = `free`, `assign` = buffer/entry `memcpy`. **Managed
elements** (string, class handle, nested dyn/queue/assoc) require the runtime to
per-element deinit on delete/resize and deep-copy on assign — an element
descriptor (stride + deinit/copy function pointers) threaded through every
runtime function. This landed for dynarr/queue as `rt_elem_ops` (feat-compose);
the POD path is the descriptor's trivial-copy fast case. Associative arrays
reuse the same descriptor (§6), so managed-element assoc needs no new lifecycle
machinery — only the assoc container itself.

## 10. NBA — landed for a queue, deferred for the rest

A whole-queue `q <= rhs` landed (`QueueCopyNb`; see queue-nba-and-nonpod.md §1): the
Active region deep-copies the RHS into a staged handle in a per-queue runtime buffer
and the NBA region `rt_queue_move`s it into the live handle, which is
managed-element safe. A dynamic-array or associative-array target still loud-fails,
for the reason this section gave: the scalar block-commit shadow (`LlvmBackend.cpp`
shadow region) stages into a fixed state slot and cannot hold a heap handle, and the
array-element NBA path (`rt_nba_arr_*`) is keyed to fixed unpacked arrays. Both take
the queue's shape when they land. An NBA to an ELEMENT of any of the three is
illegal SystemVerilog (LRM 10.4.2), not a gap.

## 11. Phasing

1. **Dynamic arrays, POD, blocking** — type/pool/slot/handle, `new[]` / `size` /
   `delete` / index / `foreach` / assign, `rt_dynarr_*`. Smallest slice that
   exercises the whole handle + element machinery end to end.
2. **Queues, POD, blocking** — ring runtime, push/pop/insert/delete/index/slice,
   `q[$:N]` bound.
3. **Associative — all key kinds, from the start (LANDED)** — ankerl
   map keyed by normalized byte spans (§4a: integral/enum/packed/`[*]`/string,
   plus class-handle and unpacked-struct keys),
   lazy sorted index, index read/write + num/size/exists/delete(k)/delete/first/
   last/next/prev, ascending-key `foreach`, deep-copy assign, and the shared
   handle in/out/inout/ref arg ABI, on the existing `rt_elem_ops` element
   descriptor. No POD-first and no integral-only cut. Value element restricted to
   integral/real (a managed value element is the one guarded gap).
4. **Landed since v1**: managed (string/handle/nested) elements for dynarr/queue
   (`rt_elem_ops`); queue reverse/sort/rsort/shuffle.
   **Still deferred, each its own guarded TODO** — NBA to these types;
   a string / dynamically-sized member inside a struct assoc key; managed
   (string/nested) assoc *value* elements;
   whole-array aggregate `'{...}` assignment; reduction/locator methods
   (`.sum`/`.find`/…); queue slice-assignment corners.

## 12. Testing

Dynamic arrays and queues use iverilog 13 as the reference. **Associative arrays
have none** — iverilog 13 cannot parse an associative-array declaration
(`int a[int]` → "Type names are not valid expressions here"), and verilator
violates LRM 7.8 by *creating* an entry on a read of an absent key (its
`num()`/`foreach` then include the phantom key) — so they are pinned against LRM
7.8/7.9 directly. `tests/associative_array_test.cpp` cross-checks element
values, absent-key no-create, `foreach`/first/next order (integral, signed,
string, `[*]`-wildcard cross-width), absent-key element default by domain,
deep-copy independence, and input-argument copy-in.

**Streaming into or out of a container has no iverilog oracle either** — iverilog
13 answers `sorry: Streaming concatenation not supported`. verilator covers the
fixed shapes (unpack into an array or unpacked struct, `<<` block reversal, and
the queue-of-strings join) and agrees on all of them; everything it refuses
(`{>>{q}} = v`, a string destination, a string source into a byte queue) is pinned
against LRM 11.4.14 plus slang's own constant evaluator, which
`tests/streaming_test.cpp` exercises directly by folding the same expression into
a `localparam` and comparing it with the runtime result.

## 13. Confirmed at implementation

- slang delivers the assoc built-in methods as a `CallExpression` with
  `isSystemCall()` true and the bare method name (no `$`); the receiver is
  `arguments()[0]`. `first`/`last`/`next`/`prev` take the key variable by ref,
  which slang may wrap in an `AssignmentExpression` (unwrap to the lvalue).
- Queue-bound overflow drop semantics vs iverilog (LRM 7.10.4).
- `foreach` over an assoc binds the loop var to each key (LRM 7.9.4): the backend
  denormalizes each `first`/`next` result span into the key-typed loop var.
