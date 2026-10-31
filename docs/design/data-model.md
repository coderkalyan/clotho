# The unified data model: storage, places, refs, and lifetimes

**Status:** design of record for this pass. Derived from first principles, then
reconciled with what already exists, so most of it is a *consolidation* rather
than a rewrite.

This document is the single answer to "what is a value, where does it live, what
is a `ref`, and how does every combination of element type x aggregate kind x
passing mode work". It supersedes the per-feature scope-outs in
`dynamic-arrays.md` §2/§4a, `queue-nba-and-nonpod.md`, and `ref-args.md` §163
("Restrictions"), which are now historical.

## 0. Why this exists

Clotho supports every SV data type in isolation. The failures in the conformance
surface were never "type X is missing"; they were always a *product* term — a
queue whose element is an associative array, a `ref` whose actual is a queue
element, a `string` member of a struct crossing a module port. The cause is
structural: the same three questions ("how is it stored", "how is it addressed",
"how is it passed") were each answered independently, per type, in three
different vocabularies, at ~40 separate `fail()` sites.

Three concrete symptoms, all measured (see §9 for the evidence):

1. **Two type vocabularies.** `src/lower/` had 9 `ir::Ref` predicates and
   `src/ir/llvm/` had 11, of which 5 pairs were textual clones
   (`is_agg_abi`/`is_agg_abi_ref`, `contains_struct`/`contains_struct_ref`,
   `is_string_type`/`is_string_ref`, `is_leaf_type`/`is_leaf_ref`,
   `is_handle_type`/`is_handle_ref`). Nothing in `clotho_ir_core` exported a
   single type predicate, so drift was undetectable at compile time.
2. **Five disagreeing address resolvers.** `lv_field_ptr`, `lv_handle_ptr`,
   `agg_slot_ptr`, `lv_string_ptr` and `flat_elem_ptr` each re-decided whether
   `array_ty`/`elem_idx` participate. `lv_handle_ptr` ignored `elem_idx`
   entirely, so a nested container reached element 0 of its parent's buffer.
3. **No lifetime story for interior pointers.** A `ref` to a queue element is a
   raw pointer into a ring buffer that `push_back` may reallocate. Nothing
   detected it.

## 1. Storage classes

Every SV type maps onto exactly one of four storage classes. This is the *only*
type classification the rest of the model consults; it is total over
`ir::Type::Kind`, and it lives in one place (`src/ir/TypeModel.h`).

| Class | `ir::Type::Kind` | Storage | Copy | Destroy |
|---|---|---|---|---|
| **Plane** | `Logic`, `Bit`, `PackedArray`, `Struct{is_packed}` (incl. packed & unpacked unions, tagged), `Real`, `ShortReal`, `Class` (the handle), chandle, enum | `{ i(2W) }`, `{ i(2W), iW }`, or narrow `{ iW }`; `real` = one 8-byte plane | plane store | — |
| **Managed** | `String`, `DynArray`, `Queue`, `Assoc` | fixed-size POD handle embedded in the parent, owning <=1 heap allocation | deep copy (`assign`) or ownership transfer (`move`) | `deinit` |
| **Aggregate** | `Struct{!is_packed}`, `UnpackedArray` | recursive image: one field / element slot per member, addressed by GEP path | memcpy iff no managed leaf, else per-leaf | per-managed-leaf |
| **Void** | `Void` | — | — | — |

Three consequences worth stating, because each was previously re-derived ad hoc:

- **A packed anything is a Plane.** A packed struct, a packed union and an
  unpacked union are all one flat bit vector; member access is a `PartSelect`,
  never a GEP. This is why they need no aggregate machinery at all.
- **A class handle is a Plane**, not Managed. It is a 64-bit word with reference
  semantics: copy *is* the pointer copy, `==` is identity. The object *body* is
  an Aggregate, reached only through `HandleDeref`.
- **Managed is closed under nesting.** A managed leaf inside an Aggregate, or a
  managed element inside a Managed container, is described by the same
  `rt_elem_ops` descriptor recursively. There is no depth limit and no special
  case per depth.

### 1.1 `is_trivially_copyable` vs relocatable

Kept distinct, because the runtime relies on the difference:

- **Trivially copyable** = no managed leaf at any depth ⇒ a whole-object copy may
  be one `memcpy`. Gates `AggCopy` and the by-value ABI.
- **Trivially relocatable** = no self-pointers ⇒ a *move* may be one `memcpy`.
  **Every** Clotho value type is relocatable, managed ones included, which is
  what licenses the containers' bulk `memcpy` shifts (ring growth, insert/delete)
  even for string elements. This invariant is now asserted at descriptor-build
  time rather than only stated in a comment.

## 2. Places (the lvalue model)

An lvalue is a **place**: `(root, path)`, resolving to one byte pointer plus the
storage class of the selected sub-object.

**Roots** — where the bytes ultimately live, and their lifetime:

| Root | Lifetime | Relocatable? |
|---|---|---|
| `SignalStorage` (module state) | whole run (static) | no |
| `FrameSlot` (subroutine local / formal) | one activation | no |
| `RefPtr` (a `ref` formal) | the callee's activation, *borrowed* | inherits the actual's |
| `ClassBody` (`HandleDeref`) | until freed — never, today | no |
| `HeapElem` (dynarr / queue element) | until the container is mutated | **yes** |
| `AssocSlot` (assoc element) | until that entry is erased | **yes** |

**Path** — a sequence of steps, each of which composes onto any root:

`field i` (struct/class member) | `elem i` (array/dynarr/queue element) |
`key k` (assoc element) | `bit`/`part`/`dyn-part select` (Plane only) |
`slice [a:b]` (fixed unpacked array only).

The load-bearing rule, and the one that was previously violated: **the resolver
is one function.** `object_ptr(place)` returns the byte pointer for any
(root, path); the Plane / Managed / Aggregate accessors are layered on top of it
and never re-derive addressing. `lv_handle_ptr`, `lv_string_ptr`, `agg_slot_ptr`
and `flat_elem_ptr` become thin wrappers over it, which is what fixes the
dropped-`elem_idx` family of bugs.

Two roots stay *deferred* rather than eager, and for one reason: an assoc element
address depends on the access direction (`get_ptr` must not create an entry,
`put_ptr` must). So `AssocSelect` records `(handle, key)` and resolves at the
`Load`/`Store`. Every other root resolves eagerly at the select. Container
element addresses *are* known at select time, so `q[i].f.g` needs no deferral —
it is an ordinary Aggregate path anchored at a heap pointer.

## 3. `ref` — a pointer, epoch-checked only where storage can move

`ref` (LRM 13.5.2) binds the formal to the actual's storage: no copy in, no copy
out, writes visible live across a `#delay`. That forces a pointer, and a pointer
into a relocatable root is a dangling-pointer hazard the previous design avoided
only by refusing every interesting actual.

**A `ref` formal is one pointer, plus two epoch words when — and only when — some
call site binds it inside a container's heap buffer:**

```
{ ptr   slot     // the place's byte pointer: plane slot, handle, or agg image
, ptr   epoch    // &container.epoch  -- heap-rooted formals only
, i64   expect } // the epoch observed when the ref was formed   -- ditto
```

Nothing about the *report* is passed. Both facts a staleness diagnostic needs are
recoverable without the caller's help: the formal's name is the callee's own, and
the container's name comes from the epoch pointer (§3.3). So the site record lives
in the callee, one per dereference, and costs nothing per call.

- **Plane** ref → `slot` is the plane slot, whose shape is the canonical
  type-derived one (§3.2), matching the actual by construction.
- **Managed** ref → `slot` is the handle. `ref string`, `ref int q[$]`,
  `ref int a[]`, `ref int m[string]` all work, and all alias rather than copy.
- **Aggregate** ref → `slot` is the image pointer. Identical to the existing
  by-pointer aggregate ABI, minus the copy-in/copy-out.

### 3.0 Which formals are fat, and why the epoch cannot be derived

The natural question is why `epoch` is passed at all, when the callee knows the
formal's type: can it not GEP to the epoch field from `slot`? The answer is the
reason the whole scheme works, so it is worth stating exactly. Two cases:

- **`ref int q[$]` bound to `q`.** `slot` *is* the `rt_queue` POD, and `epoch` is
  a field of it — `slot + offsetof(epoch)`, derivable. But the POD is design state
  and never moves, so there is nothing to check.
- **`ref int` bound to `q[0]`.** `slot` is an interior pointer into the malloc'd
  ring, one indirection *past* the handle; `epoch` is in the handle. Two different
  allocations, no fixed offset — and needed.

So the epoch is derivable exactly when it is unnecessary, and necessary exactly
when it is not derivable. That is forced, not incidental: a staleness witness must
live outside the object that can move. Put the epoch in the ring buffer and a
stale `slot` would read its witness out of freed memory — the pointer whose
validity is in question would be the one used to check it.

**Nesting therefore needs a CHAIN, not a witness.** For `a[k][i]` the inner queue's
handle — and with it the epoch word — lives *inside the map's payload*, so that
witness is itself destructible: `a.delete(k)` frees it. One pair per container level
travels with the ref, outermost first, and the callee checks them in that order,
short-circuiting on the first mismatch. Level n's witness is read only once level
n-1 is proven current, which is exactly what makes reading it safe. A call site that
binds shallower than the formal's declared depth pads with the `clotho.ref.noepoch`
zero word.

A single field forced the opposite: each element select OVERWROTE the witness, so a
two-level place kept the inner one — the only one that can die — and a restructuring
of the outer container went unreported while the check read freed memory.

Which formals need it, and how deep, is therefore a property of the ACTUALS, and
`Analysis::call_actuals` already visits every one. `heap_depth()` walks the select
chain of each `ref` actual and COUNTS the relocatable levels: each `ElemSelect` over
a dynamic array or queue, and each `AssocSelect`, at any depth (a further member or
bit step off such a place stays at the same depth, so the walk continues to the
root). A ref to a variable, a fixed-array element, a struct member, a class
property or a whole container handle counts zero and stays one plain pointer.
`DesignFacts::ref_epoch_formals` holds the answer keyed `(fid << 32) | formal`,
mapping to that depth, maxed over call sites.

Two obligations, both discharged in `analyze()`:

- **Conservative on absence.** A formal narrows only when every call site was seen
  and none was heap-rooted; `ref_needs_epoch` also treats an unknown function as
  fat. The fact is per FORMAL, never per call site, so a formal reached by both
  shapes stays fat everywhere and the variable actual passes the `noepoch`
  sentinel rather than a shorter argument list.
- **Virtual dispatch agrees.** A `CallVirtual` reaches whichever override occupies
  the vtable slot, so the answer is unioned across each slot index: one override
  bound to a queue element widens every sibling in that slot. Were this wrong, an
  indirect call would write arguments the callee never declared — memory
  corruption, not a visible failure, which is why
  `llvm_backend_test.cpp` asserts the emitted parameter lists of both bodies
  directly and in both directions.

**Epoch checking**, for the formals that carry it. Every deref loads `*epoch` and
compares it to `expect`:

- equal → proceed. One load, one compare, one not-taken branch.
- `*epoch > expect` → the container was restructured under a live ref. Located
  fatal `rt_ref_stale`, naming the ref formal, the actual, and both epochs.
  `--loose-stale-ref` downgrades it to a one-shot warning and continues.
- `*epoch < expect` → impossible without a codegen bug (epochs are monotone).
  Assert trip: `rt_ref_epoch_regress` aborts unconditionally, even under
  `--loose-stale-ref`.

A fat formal whose actual at *this* site is stable (the mixed case above) points
`epoch` at `clotho.ref.noepoch`, an internal constant zero global, and passes
`expect == 0`, so it is never reported stale.

**Cost.** For the common shapes — a variable, a struct member, a fixed-array
element, a class property, a whole handle — a `ref` is exactly one pointer and the
dereference is a plain load: no epoch, no compare, no branch, nothing for the
optimizer to have to see through. Only a container-element ref pays, and there it
buys the difference between a located diagnostic and silent corruption.

This replaces an earlier claim in this document that the check "folds away" for a
stable actual. It does not fold: the argument is an opaque `ptr` parameter nothing
specializes, and a task's coroutine split spills it. Rather than pay it everywhere
and hope, the shape is decided per formal up front. What the folding argument does
still buy, for the fat formals, is that `rt_ref_stale` is declared
`inaccessiblememonly`, so GVN collapses repeated dereferences of one formal within
a block to a single load and compare instead of one per access.

### 3.1 The relocation epoch

`rt_dynarr`, `rt_queue` and `rt_assoc` each gain a `uint64_t epoch`, bumped by
**every** operation that can change the index-to-address mapping — not merely
those that reallocate:

| Container | Bumps epoch |
|---|---|
| `rt_dynarr` | `new`, `new_copy`, `delete`, `assign`, `move`, `reverse`, `sort`, `shuffle` |
| `rt_queue` | `push_back`, `push_front`, `pop_back`, `pop_front`, `insert`, `delete_idx`, `delete_all`, `assign`, `assign_dynarr`, `move`, `append_range`, `reverse`, `sort`, `shuffle` |
| `rt_assoc` | entry-creating `put_ptr`, `default_ptr` first use, `delete_key`, `delete_all`, `reset`, `assign`, `move` |

Writing an existing element (`q[i] = v`) does **not** bump: it changes no
address and no index. A `push_back` that happens not to reallocate *does* bump —
it changes `len`, hence the meaning of `q[$]`, and cheap monotonicity beats a
subtle "did it actually move" test.

The epoch is part of the handle rather than a side table so that it survives
`assign`/`move` correctly (the destination's epoch advances; it is never copied
from the source) and needs no extra indirection to check.

### 3.2 Slot shapes agree by type, never by analysis

A `ref` aliases storage rather than copying through it, so the two sides' layouts
must be *identical*, not merely both readable. The invariant that makes that hold
without either side knowing the other:

> A ref actual's storage, and a ref formal's slot, both take the CANONICAL
> type-derived shape — `has_z = four_state`, and the native `{ iW }` when 2-state — which is
> exactly the rule an aggregate leaf already followed.

Getting this wrong is silent memory corruption, and it did go wrong twice while
this pass was being built, in opposite directions:

1. A ref formal was force-packed `{i(2W), iW}` while a canonical 2-state aggregate
   leaf is narrow `{iW}`, so a `ref` to a fixed-array element had the callee write
   8 bytes through a 4-byte element and overrun into the next signal in the state
   struct. One activation looked fine (it landed in padding); two concurrent
   activations of the same task made it visible.
2. Forcing a ref-actual array onto the recursive aggregate MIRROR fixed that, but
   left a ref-actual array and an ordinary array *of the same type* with different
   storage, which a whole-array copy between the two cannot express — it crashed
   codegen on `v = tmp; fn(v)`.

The resolution is to canonicalize the ELEMENT SHAPE only and leave the storage
model alone: flat `[N x slot]` over canonical element slots is byte-identical to
the canonical mirror (row-major, same leaf type), so the callee's nested GEP and
the caller's flat GEP compute the same address, and same-typed arrays keep the
same storage whether or not one of them is a ref actual.

A pointer-ABI actual takes this shape whatever the may-z analysis proved
(`ptr_arg`): a *user* signal is one only by being a ref actual, and the out /
inout actual of a call is always a compiler-materialized temp (`make_arg_temp`),
which a callee writes through a pointer to its value word, and its z word when
4-state.

The cost, stated plainly: a 4-state signal that is a ref actual keeps its z word
even where the may-z analysis could have elided it, and every ordinary reader of
that signal pays the wider access. That is inherent to aliasing it.

A whole-object copy therefore has to work with either model on either side, and
picks its shape from the TYPE rather than from what the two places happen to be:
anything struct- or container-bearing is in the mirror on both sides by
construction, so its own storage type is the memcpy shape; a plain scalar-leaf
array compares the two element slots and either memcpies, moves word by word when
they differ only in the z word's presence, or converts element by element through
the ordinary load/store pair when one side is narrow and the other packed. That
last case is reached whenever a class property or struct member (always canonical)
meets a module-scope array signal (may-z-analysed), and used to be a backend
refusal.

### 3.3 Where the report comes from

An earlier shape passed a fourth word, `const rt_ref_site*`, built at the call —
on the reasoning that only the caller knows which container the actual came out
of. Reading the diagnostic back disproves the premise. It needs exactly two names:

- **the formal** (`'bump_slow argument 1'`) — the callee's own, derived from its
  name and the parameter position. Never caller knowledge.
- **the container** (`'q'`) — recoverable from the `epoch` pointer, which the
  guard is already holding. The backend emits one `{ &epoch, name }` row per
  container signal into `clotho.ref.names` and registers it once, before any
  process runs; `rt_ref_stale` scans it. Linear, because it runs at most once in a
  run, immediately before printing.

So the site record moved into the callee, one per *dereference* — and the fourth
word went away entirely. The table is emitted only for a design that has a fat ref
formal at all.

Two consequences, both deliberate:

- **The caret moved** from the call that formed the ref to the access that
  faulted. The formal's name in the headline is what ties it back to the binding.
  This is the one thing the old shape did better; it cost a word at every call to
  do it, and the note already states the fix in terms of the container.
- **A container with no fixed address is described, not named.** A queue that is a
  class property lives in a heap object body, and one nested in another container
  lives in a moving buffer; neither can be a table key, so the report says "its
  container". That is not a regression: the old shape passed the *actual's root
  signal* name, which was empty for a class property and, for `outer[0][0]`,
  was `outer` — the outer container, while the epoch being watched belongs to the
  inner queue. It named a place that had not moved. Describing beats guessing.

## 4. One element-descriptor ABI

`rt_elem_ops` is the single lifecycle vocabulary for anything stored *inside* a
container or as an aggregate leaf. It gains one kind and loses its exceptions:

- `rt_elem_assoc = 5` — the missing kind. Previously an `Assoc` element was
  described as `rt_elem_queue`, which would have read `base/head/len/cap` out of
  an 8-byte slot and freed garbage the moment the frontend gate lifted.
- Kind 4 `rt_elem_aggregate` emits a real, offset-sorted `leaves[]` table.
- **Every** container op takes `const rt_elem_ops*`. The ordering ops
  (`reverse`/`sort`/`shuffle`) and the `%p` / `==` renderers previously took a
  bare `stride` plus, in the renderers' case, a *second* three-value element
  taxonomy that had to be hand-synced with the descriptor's.
- All four managed kinds get the same four verbs: `init`, `deinit`, `assign`,
  `move`. `move` previously existed only for `rt_queue`, which is why whole-
  container NBA existed only for queues.

## 5. Associative keys: one order-preserving encoder

A key is a byte span with two properties: equal keys ⇒ equal bytes, and unsigned
lexicographic byte order == the LRM traversal order. Fixed-width leaves keep
today's encoding (big-endian, MSB flipped for signed). The new part is
**variable-length leaves**, which unlocks struct keys containing a `string` or a
container:

- a variable-length leaf is emitted as its bytes with `0x00` escaped to
  `0x00 0x01`, terminated by `0x00 0x00`.

That is order-preserving for the concatenation of a tuple of byte strings, which
is exactly what a struct key is, and it removes the "shorter-is-less on a
prefix" tiebreak's ambiguity. LRM 7.8.5 leaves the order free wherever `<` is
undefined for the member type, so any total deterministic order conforms; this
one is total, deterministic, and stable for a run.

## 6. Passing modes

One table, derived rather than enumerated. `in` is a deep copy into the callee's
frame; `out` a deep copy back; `inout` both; `ref` the pointer of §3. The `ref`
column is the *storage* it aliases; whether the formal also carries the three
epoch words is orthogonal to the storage class and decided per formal from its
actuals (§3.0) — a `ref int` is fat when bound to `q[i]` and narrow when bound to
a variable, though both are Plane.

| Storage class | in | out / inout | ref | return | module port |
|---|---|---|---|---|---|
| Plane | value (2 params) | 2 ptrs (&mm, &z) | ptr to plane slot | `{i(2W), iW}` | plane copy process |
| Managed | 1 ptr + deep copy in | 1 ptr + deep copy back | ptr to handle (alias) | out-param + deep copy | deep-copy process |
| Aggregate, trivially copyable | 1 ptr + memcpy | 1 ptr + memcpy back | ptr to image (alias) | out-param + memcpy | memcpy process |
| Aggregate, managed leaf | 1 ptr + per-leaf copy | 1 ptr + per-leaf copy back | ptr to image (alias) | out-param + per-leaf | per-leaf process |

The `net` mode is a Plane-only concept: a net's value is resolved from competing
drivers, which requires a bit-level strength fold. A net of Managed or Aggregate
type has no such fold, so it stays refused — but now by *one* rule stated here,
rather than by five scattered guards. A `var` of any class is unrestricted.

## 7. What this eliminates

The gap class the pass targets is exactly "element type x aggregate kind x
passing mode". These guards are gone, not reworded:

- assoc value element: nested containers, class handles, and aggregates with
  managed leaves. One shared rule now admits exactly what a queue or
  dynamic-array element admits (`guard_container_element`).
- assoc as a dynarr / queue element (`rt_elem_assoc`).
- `ref`: every type, and every whole-place actual shape — container element,
  assoc element, struct member, class property, nested combinations.
- NBA: whole dynamic array and whole associative array, on the queue's staging
  path generalized to `HandleCopyNb`.
- module ports whose type contains a `string` reached through a struct member or
  container element, and a connected container port: both previously fell through
  to the plane-copy path. The first was unguarded; the second reached the backend
  as a plane load of a handle, so it failed with no source location. Both are now
  located diagnostics — refusals, not features, but honest ones.

Deliberately **not** closed, each with a reason rather than a TODO:

- **event** inside any aggregate or container, and `ref` of an event. An event has
  no value; it owns wait queues keyed by a static per-event timestamp. Per-element
  trigger state is a scheduler change, not a data-model one (LRM 15.5).
- **net** of Managed or Aggregate type (§6).
- **`real` in an assoc key** — illegal per LRM 7.8.5, not a gap.
- **assoc key with a `string` or container member.** The order-preserving escaped
  encoding sketched for it (escape `0x00`, terminate `0x00 0x00`) is correct and
  would work; it was cut for scope, and the fixed-width key path is untouched.
- **`s[i] <= c` — a nonblocking write of one string CHARACTER.** Whole-string NBA
  and a string leaf of an aggregate now stage an owned `rt_string` in their own
  record range (assign at sample, `rt_string_move` at commit — the Managed analogue
  of `HandleCopyNb`, see queue-nba-and-nonpod.md §3). A single character is a
  masked write into an existing value, which that record shape does not express.
- ~~**`a[k][i]` where the assoc value is a container.**~~ **Closed.** The direction
  is threaded down the select chain instead of deferred: `ir::ElemSelect` and
  `ir::AssocArg` carry the access's own `for_write`, so an inner select resolves the
  entry with get_ptr for a read (creating nothing, LRM 7.8.6) and put_ptr for a
  write (7.8), and a mutating method's receiver resolves as a write (7.10). Only a
  dyn/queue value needs the eager resolution — its element address loads
  base/head/len/cap, so it is not a function of the base alone; an AGGREGATE value
  still composes deferred through the GEP path. Nests to any depth, and a
  `ref` through it is checked at every level (§3.1).

## 8. Lifetime summary

| Object | Created | Destroyed | Interior pointers |
|---|---|---|---|
| Module signal storage | state global, power-on init | `$finish` / drain teardown | stable for the run |
| Frame slot | `init_frame` at activation | `deinit_body_strings` at every exit | stable for the activation |
| Managed heap buffer | first mutation | owner's `deinit`, or `assign`/`move` over it | **epoch-guarded** (§3.1) |
| Assoc entry | entry-creating `put_ptr` | `delete_key` / `delete_all` / `reset` / owner `deinit` | **epoch-guarded** |
| Class body | `ClassNew` | never (no GC) — out of scope by direction | stable, therefore safe |

The one honest gap: class bodies leak, so a class holding a `string` or a
container leaks its heap too. That is the documented no-GC position and is
explicitly excluded from this pass.

## 9. Verification

- The full cartesian product of {inner element type} x {aggregate / container
  kind} x {passing mode} is generated as tests, each case either producing a
  checked value or loud-failing with a reason named in §7.
- Epoch checking is verified positively (a `ref` to `q[i]` observed live across a
  `#delay`) and negatively (a `push_back` across that `#delay` producing the
  located `rt_ref_stale` fatal, and the same case continuing under
  `--loose-stale-ref`).
- Cross-checked against verilator for the container forms, and against iverilog
  wherever it parses. iverilog rejects `ref` ports, so the `ref` cases stand on
  the LRM and on the event-driven cases listed in `ref-args.md` §47.
