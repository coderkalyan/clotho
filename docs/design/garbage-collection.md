# Garbage collection (LRM 8.4)

Status: **P0-P4 landed, and the collector is unconditional. Both tracks
(evacuation, multi-mutator) remain the proposal (§12).** The heap layout of §3.1 and
§4 has since replaced the header word, line marks and bitmaps those phases describe.
What runs today: a mark-region heap
with an inlined bump-allocation fast path, the object model, precise global roots,
precise container roots, conservative pinned coroutine-frame roots, per-class trace
maps and finalizers, generational collection in place with an inlined write
barrier, and stop-the-world young or full mark-region collection at the scheduler's
region boundary. There is no way to run a design uncollected; §12a records why the
flag went and what that cost.

Landed evidence, all [in-tree]:

- 2029/2029 of the suite passes, including 43 cases in `tests/gc_test.cpp` (723
  assertions), which also pass under ASan+UBSan with no memory error.
- **Allocation is 2.0× faster than the leak-everything path**: 4M short-lived
  objects, 196 ms of simulate time uncollected against **96 ms** collected. The
  uncollected path called `rt_alloc` (64-byte-aligned `operator new`) per object;
  the collector bumps a
  cursor inline. Inlining that path (P2) was itself worth 1.5× over the out-of-line
  pool allocator.
- **Generational collection does what it exists for**: a 20,000-node stable
  population plus 200 transactions per cycle for 400 cycles, comparing young
  collections against forcing every collection full — 3.35 ms vs 11.13 ms at a
  128 KiB nursery (**3.3×**), 3.68 ms vs 21.81 ms at 64 KiB (**5.9×**). The point is
  the shape rather than the ratio: generational time is nearly flat as the
  collection count triples (22 → 50 young collections cost 3.35 → 3.68 ms) while
  all-full time scales with it. `live at last collection: 1 object` on a heap
  holding 20,000 live nodes is the same fact from the other side.
- The collector costs a design that does not allocate essentially nothing, measured
  rather than argued: median **0.0%** simulate delta over the 12 runnable cases of
  `tests/perf/cases` (min of 9, interleaved), corpus total −1.7%. That is what made
  the flag removable (§12a); while it existed, `--gc=off` emitted no GC artifact at
  all, so the same claim held by construction.
- Byte-identical stdout across `off` / `on` / `stress` on every case.
- A 10,201-object churn design under `--gc-stress`: 201 collections, live set
  settles at 202 objects / 12,928 bytes, 9,999 objects finalized during the run
  (their `string` buffers freed — the second-order leak in §0), heap 128 blocks
  with 127 free at exit.
- The conservative frame path is exercised, not assumed: an object held only by an
  `always` block's `automatic` local across a suspension, and one held only by a
  blocking task's local across `#3`, both survive 40 stress collections against a
  10,000-object filler, with 29 conservative pins recorded. `tests/gc_test.cpp`
  asserts `conservative_pins > 0` on that case so it cannot silently decay into a
  precise-root test (see §5.4 for why that trap is easy to fall into).
- LeakSanitizer, the house method for lifetime work, on the churn design:
  uncollected it reported **571,256 bytes leaked in 10,201 allocations** — the class
  objects and their `string` buffers, exactly §0's two leaks. Collected it reports
  **no leak at all**. On a design that also calls `$finish`, uncollected leaks 10,024
  allocations and `--gc-stress` leaks **4**: the collector removes 10,020 of them
  and the residue is the pre-existing `$finish`-abandons-live-frames leak
  (`cooperative-finish.md:24-28`), not a collector bug.
- ASan and UBSan report no memory error on any case under `--gc-stress`
  — no use-after-free, no overflow. That is the signal that matters most, because
  a missed root shows up as exactly that.
- **Idle overhead is proportional to what a design actually uses, with no cliff.**
  The first version of this work charged every design a call pair per process
  activation and a call per wheel advance: `examples/wait_bench.v` paid **+10.3%**
  simulate time with no class anywhere in it, and a 5M-edge design with **one**
  long-lived object and zero churn paid **+15.1%**. §6.4 is the mechanism that
  removed both, and what it is *not* is a special case: an intermediate fix
  disabled the collector for designs declaring no class, which fixed the RTL
  number and left the 15% cliff standing one `class` keyword away. That test is
  gone.

  Where it stands now, measured interleaved (9 alternating runs, min) because the
  deltas are close to this machine's noise: `wait_bench.v` **−2.3%**, and the
  sparse one-object design **+6.5%**. The second number is not explained by
  emitted code — its hot `always` blocks contain no GC instruction at all, and the
  only per-event addition is the inlined poll in `sim_drain`, which the class-free
  design gets identically and does not pay for. A control run isolates part of it:
  adding the unused class field alone, uncollected on both sides, moves the
  same benchmark **−2.4%**, so this family swings ±2-3% on layout. Honest summary:
  **within a few percent, at the edge of what this harness can resolve, with no
  per-event GC code in the hot path.** Settling it properly needs `perf.py` plus
  `cloud/run.sh` on quiet baremetal, which is the project's actual gate and has
  not been run.
- 12 of the 43 test cases exist because two implementation bugs got through review
  and were caught by measurement rather than by reading (§7.1). Both were in the
  generational phase, both were use-after-free, and neither was subtle in
  hindsight.

Supersedes the two-option sketch in `classes.md` §7 and amends the class-body row
of `data-model.md` §8 (`data-model.md:418`, "never (no GC) — out of scope by
direction").

Measurements marked **[probe]** were run during scoping in standalone programs on
the development workstation (Linux 6.18, x86-64, Ryzen 5 7640U), not inside
clotho. Measurements marked **[in-tree]** come from this repository. Everything
else is a source citation or is marked as intuition.

Two constraints shape this document more than anything else, and both are about
the future rather than the present: **multi-threaded simulation is a planned
performance milestone**, so nothing here may assume one mutator; and **evacuation
stays on the table**, so the object model is built to move objects even though the
first shipping configuration will not.

---

## 0. Why this exists

`new` allocates and nothing frees. `ir/llvm/Inst.cpp, lower_inst` says so at the
site:

> Lifetime is leaked in V1 (LRM 8.4 objects are GC'd; a program cannot observe a
> free) — see docs/design/classes.md §7.

That was the right call for a base — LRM 8.4 gives no `delete` for a class handle
and a program cannot observe reclamation, so leaking is *observably correct*. It
stops being correct enough at M4 (`TODO.md`, "Run a real UVM testbench"),
because a UVM run allocates a transaction object per bus transfer and never
stops.

The leak is also second-order: because a class body is never freed, every
`string`, dynamic array, queue and associative array **inside** an object leaks
its own heap too (`emit_class_body_init`, `ir/llvm/Aggregates.cpp`, has no
matching `emit_agg_deinit` call anywhere).

**In scope.** Class bodies; the heap they own through managed properties; event
objects (`event-handles.md` §9); `std::weak_reference` (§7a).
`std::mailbox` and `std::semaphore` are ordinary traced class bodies
(`mailbox-and-semaphore.md` §4.4).

A `process` object (LRM 9.7) is scoped as an ordinary traced class body whose
properties are the thread's control block — status word, activation stack, `await`
waiters, RNG pointer and four tree links — with the raw pointers deliberately typed
`chandle` so they stay OUT of the handle-offset table, and with the top-level
control blocks in an `nproc`-sized precise root array. The collector is what gives
it a lifetime: a control block outlives its thread exactly as long as a user handle
references its `FINISHED` / `KILLED` status. See `docs/design/process-class.md` §4.

**Out of scope.** Everything that already has a working owner: coroutine frames,
frame-local strings and containers, managed heap buffers, assoc entries — the
four fully-specified rows of `data-model.md` §8. Those stay explicitly owned and
explicitly freed. A collector is not an excuse to delete a working ownership
discipline.

**Never in scope.** `chandle`. It is a foreign pointer from DPI code
(`lower/Types.cpp, lower_type`); clotho never allocates it and must never trace or
reclaim it.

---

## 1. Guiding decisions

- **Write the collector; take no dependency.** Four candidate libraries were
  evaluated against clotho's actual shape and all four fail on facts, not taste
  (§10). The nearest miss (Ravenbrook MPS) fails on MSYS2 CLANG64 and on owning
  `SIGSEGV` process-wide. clotho's JIT already knows every object's exact layout —
  the hardest thing any binding has to reconstruct — so the binding costs more and
  buys less than the collector.

- **Tracing, not reference counting.** Cycles are not an edge case in UVM, they
  are the architecture: `uvm_component` holds a parent handle and an associative
  array of children. Verilator ships intrusive refcounting (`VlClass` holds
  `std::atomic<size_t> m_counter`, `include/verilated_types.h`) and its own issue
  tracker records the consequence — verilator#4651: "reference counting is
  insufficient, and easily results in memory leaks … Assuming each and every
  component references its children and parent, currently no `uvm_component`s
  will ever be freed." Refcounting also costs a retain/release on *every* handle
  store, and under multiple mutators those become atomic RMWs on shared objects —
  the worst possible scaling shape.

- **Immix, not size-class pools.** Mark-region: 32 KiB blocks, bump allocation
  within a block, reclamation of free runs rather than cells
  (Blackburn & McKinley, PLDI 2008, which divides a block into 128 B lines; the
  heap as built reclaims at the 16-byte granule instead, §4.2).
  It is chosen because it is the one heap
  organisation that does not force the moving/non-moving decision at design time:
  the same heap runs strictly non-moving, generationally in place (StickyImmix),
  or with opportunistic evacuation, as a *policy* setting. It also gets
  copying-grade allocation throughput — a bump pointer — without copying
  anything. The paper's numbers: "improving total application performance by 7 to
  25% on average across 20 benchmarks", "3% more memory than mark-compact … 15%
  less than mark-sweep", 0.8% metadata overhead, 2.5% defragmentation headroom.

- **Evacuation is a policy, not an architecture.** The object model reserves a
  forwarding word and a pin bit from P0, and §8 lists the four prerequisites that
  must land before evacuation may be switched on. What is *not* deferred is the
  ability to switch it on.

- **Conservative roots do not force a non-moving heap.** Pinning reconciles them,
  and this is settled prior art rather than a gamble: Bartlett's mostly-copying
  collector (DEC WRL 88/2, 1988) leaves ambiguously-referenced pages in place and
  copies the rest; MPS's AMC pool is that algorithm productised; SBCL's gencgc
  pins at eighth-of-a-page granularity from raw register and stack words; CRuby's
  compacting collector pins conservatively-marked objects (`gc_mark_and_pin`);
  V8's Scavenger stays evacuating and quarantines pages holding conservatively
  pinned objects (`PinObjectsConservative` in `src/heap/scavenger.cc`). An earlier
  draft of this document claimed conservative frames implied non-moving. That was
  wrong.

- **Multi-mutator from P0, in the parts that are expensive to retrofit.** Three
  specific commitments: the allocation fast path reaches its state through *one
  indirection from a mutator context pointer*, never a link-time constant address;
  all collector metadata that mutators write is **byte-granular**, so parallel
  marking and card marking need no atomics; and the write barrier is an
  unconditional card store, which is race-free by construction. Everything else
  about MT — poll sites, thread-to-process assignment, the rendezvous
  implementation — belongs to the MT scheduler design and is deliberately not
  pre-empted here (§6.2).

- **Collect at scheduler region boundaries.** Today that means the drain loop's
  quiescent edges, where no JIT frame is live. Under MT it means the same place:
  SV's event regions (LRM 4.4-4.5) are globally ordered, so a parallel scheduler
  *must* rendezvous at region boundaries regardless of the collector. The GC
  piggybacks on synchronisation the semantics already require.

- **Precise heap and globals, conservative coroutine frames.** clotho emits the
  layout of the heap and the design state, so those are precise. Coroutine frame
  layout is chosen by LLVM's CoroSplit and is not ours to describe; a frame is
  scanned word-wise, every candidate validated against our own metadata, and hits
  are pinned.

- **No header word.** What the collector records about an object lives in a
  metadata byte beside the heap (§4.1), found from the object's address by
  arithmetic. So `class_header_words()` stays 1, every property GEP index stays
  `1 + declIndex`, `$cast`'s negative-offset display arithmetic
  (`ir/llvm/Inst.cpp, lower_inst`) is untouched, and an object is 16-byte aligned.

- **Finalize; do not re-own.** A collected object runs a per-class finalizer that
  is exactly the existing `emit_agg_deinit` walk. Managed buffers keep their
  current owner-frees discipline.

---

## 2. The facts that decide the design

**F1 — the scheduler has real quiescent points today, and will still have them
under MT.** One sim thread runs JIT'd `main` → `rt_sim_run` → `sim_drain`; the
whole event loop is emitted (`emit_active_run`, `ir/llvm/Scheduler.cpp`), and
at its ring-empty edges — `active.check` with `head == tail`, `wheel.check`, and
immediately after `rt_wheel_advance` returns — the native stack is exactly
`main → rt_sim_run → sim_drain` and every process is suspended in its frame. The
property that survives multi-threading is not "one thread" but "regions are
globally ordered": NBA commit, the Observed/Reactive split and delta-cycle
ordering are defined across the whole design, so region boundaries are rendezvous
points in any correct parallel scheduler.

**F2 — a suspended process has no stack.** Processes and blocking tasks are LLVM
switched-resume coroutines (`setPresplitCoroutine` at `ir/llvm/Subroutines.cpp, lower_group`);
the frame is `rt_alloc(coro.size)` and CoroSplit chooses
its layout. Two consequences, both measured:

- LLVM's `RewriteStatepointsForGC` is not merely incomplete here, it is
  **unsound**: the ramp stores the handle into the frame and the following
  statepoint carries no `gc-live` bundle at all (`NumLocations = 3`, zero
  relocation records); in the resume function the `gc.relocate` result is never
  written back to the frame slot **[probe]**.
- A link node declared as an `alloca` *inside* the coroutine **is** hoisted into
  the frame by CoroSplit with all uses rewritten to a stable frame-interior
  address **[probe]** — so an intrusive registry node living in the frame is a
  legitimate mechanism, and it is how a frame's base and size reach the collector.

**F3 — every class object already carries a type tag.** Field 0 is the descriptor
pointer, stored at `new` (`ir/llvm/Inst.cpp, lower_inst`); the descriptor
(`clotho.classdesc.<Ref>`) is a private constant with the ancestor
display at negative offsets from the vptr and the vtable at positive ones. Precise
tracing costs one load plus one indirect table read. `class_body_ty` already knows
exact byte offsets via `getStructLayout()`, and `collect_elem_leaves`
already emits an `{offset, descriptor}` table.

**F4 — handles are bare, aligned, untagged 64-bit words.** `ClassNew` yields
`ptrtoint` of the body (`:11697`); `HandleDeref` does `inttoptr` (`:11700-11714`);
`null` is all-zero; `==` is `icmp eq i64`. No tagging anywhere in the tree. A
naturally-aligned 64-bit store is atomic on every target in the matrix, so even
under MT the collector never observes a torn handle. Bodies are 64-byte aligned
today only because `rt_alloc` is the coroutine-frame allocator
(`runtime/Scheduler.cpp`, `frame_align{64}`) — 62% waste on a 24-byte transaction
object.

**F5 — the call ABI already carries a uniform extra pointer, and the reason is
documented.** `declare_function` (`ir/llvm/Subroutines.cpp`) threads the RNG
state unconditionally:

> The caller's xoshiro256++ state (LRM 18.13). Unconditional rather than gated on
> whether the callee draws: every entry of a vtable slot and every `$monitor` body
> is called through ONE FunctionType, which no per-callee bit can vary … One
> pointer register on a call that does not need it is the price of that
> uniformity.

That pointer is per-*SV-process* (the state lives in a frame slot, `:3263`), so it
is not itself the mutator context — but it settles the ABI question in principle:
a uniform context pointer is an accepted cost here, and the allocation fast path
must be written against one (§4.3).

**F6 — three mechanisms currently assume a stable address.** These are the
prerequisites for evacuation, and they are all fixable (§8):

| Mechanism | Where | What breaks if bodies move |
|---|---|---|
| Assoc keys on class handles | `lower/Types.cpp, lower_type`, `emit_key_bytes` `ir/llvm/AssocArrays.cpp`, `runtime/Containers.cpp, AssocKeyLess`, `AssocMap` | the handle's raw bits are big-endian-encoded into the `std::string` hash key *and* drive `first`/`next` ordering |
| `ref` into a class property | `ir/llvm/StateLayout.cpp, ref_actual_arg` | passed as a raw interior pointer with `clotho.ref.noepoch`, a compile-time-zero epoch that can never signal invalidation |
| `rt_ref_names` | `runtime/Reports.cpp` | address-keyed linear scan; safe today only because no entry aims at a class body |

---

## 3. Object model

### 3.1 An object and its metadata

```
        handle + 0   ┌──────────────────────────┐
                     │ vptr   : ptr             │  → clotho.classdesc.<Ref>.slots[0]
        handle + 8   │ property 0 …             │
                     └──────────────────────────┘
```

A handle is the address of the object's first byte, which is 16-byte aligned. The
object's size is its body rounded up to 16 bytes, and comes from the class's trace
map (§3.2). There is nothing before the body.

What the collector records about an object is in the metadata byte of its first
granule (§4.1): that an object starts there, and four flags.

- `MARK`. The only generation state: an object is old exactly when it is marked. A
  young collection marks what it reaches and leaves a marked object alone; a full
  collection clears every mark before it marks anything.
- `LOGGED`. The remembered set holds the object (§7).
- `PINNED`. A conservative root named the object in the collection now running. A
  bit plus a side list of the pinned objects, cleared when the collection ends.
- `FINALIZE`. The class has a finalizer, which the sweep runs when it frees the
  object.

**Identity numbers are not in the metadata either.** An object used as an associative-array
key or formatted with an integral conversion needs a name that is not its address:
key bytes are compared, hashed and sorted, output is diffed between runs, and
neither may depend on where an object sits. That name is the object's identity
number (`rt_gc_identity`, `runtime/Gc.h`): assigned by the first request for it,
counting from 1 in request order, never reused within a run, 0 for null. It is
kept in two hash maps beside the heap, address to number and number to address,
so an object that is never asked pays nothing and the object header has nothing
to carry. A key serializes the number; the collector and a key traversal resolve
it back (`rt_gc_identity_object`). A key holds its object, so a number in a live
key always resolves.

A collection forgets the number of every object it did not reach, once marking is
complete (`release_dead_identities`): the table names an object by address, and a
stale entry would give the dead object's number to the next object allocated
there. A young collection checks only the objects numbered since the last
collection, because every other numbered object is old; a full one reads the
table.

What the table costs is one hash probe wherever a handle and its number are
converted. Measured in the shipped configuration (Release, extra assertions off;
user-mode instructions, the least of three runs; `int by_obj[C]` with 256 or 4096
keys), against the same tree with the address in the key:

| operation | before | after | |
|---|---|---|---|
| `by_obj[h]` lookup | 334 | 378 | +44 per lookup (+13%) |
| one `foreach (by_obj[k])` step, reading `k.x` | 784 | 901 | +117 per step (+15%) |
| the collector visiting one handle key | | | +54 per key |

A lookup converts once; a traversal step converts the current key to bytes and the
next key back. The numbers are per operation of a loop that does nothing else.

**Objects do not move.** There is no forwarding address and nowhere to put one;
§8 records what evacuation would need.

### 3.2 The trace map

A second constant global per class, `clotho.classgc.<Ref>`:

```
{ u32 nhandles, u32 nleaves, ptr finalizer, u32 handle_offsets[], rt_elem_leaf leaves[] }
```

- `handle_offsets` — byte offsets of directly-embedded class handles, including
  those inside fixed unpacked arrays and structs, from
  `class_body_ty(ref)`'s `StructLayout`.
- `leaves` — reuses `rt_elem_leaf { u64 offset; const rt_elem_ops* ops; }`
  (`Runtime.h:883-886`), so tracing *through* a managed property and finalizing it
  are the same table.
- `finalizer` — the emitted `emit_agg_deinit` function for this class, or null.

The map pointer sits immediately before the ancestor display, at a design-wide
constant negative offset from the vptr (`-4*kMaxClassDepth - 8`): one load from
any handle, and no existing offset arithmetic moves. Reaching it from the
descriptor base instead would make the offset depend on the vtable slot count.

### 3.3 Containers and aggregates

The one genuine gap in the existing descriptor vocabulary. `elem_ops_for`
(`ir/llvm/Containers.cpp`) classifies a class-handle element as `rt_elem_pod`
(kind 0) with no leaf table, and `Runtime.h:871-873` says so outright — "A POD
element (plane scalar / real / packed vector / class handle) is pure bytes". A
container of handles therefore carries **zero** type information at runtime, so
nothing can trace through it.

Required: a handle-bearing element kind, handle offsets in the leaf table, and
`rt_dynarr_trace` / `rt_queue_trace` / `rt_assoc_trace` walkers. The queue walker
must respect ring wraparound (`base + ((head+i) % cap) * stride`); the assoc
walker must iterate the C++ `AssocMap`, because element payloads are one `SlotVec`
heap block per entry and `unordered_dense` relocates them on rehash
(`runtime/Containers.cpp`). Handle-valued assoc **keys** are strong references and
must be traced from their encoded bytes.

`rt_elem_ops` is mirrored bit-for-bit by `elem_ops_ty()`
(`ir/llvm/RuntimeAbi.cpp, elem_ops_ty`) under an explicit "must match the runtime struct
exactly" contract, so this is a lockstep two-file edit.

And one problem that is not a descriptor problem at all, which is the real reason
this is a phase rather than an afternoon. A container's payload is `malloc`'d, not
GC-heap memory: a frame-local `C q[$]` puts an `rt_queue` handle in the coroutine
frame, and the conservative frame scan finds only a pointer to a malloc'd ring —
which `rt_gc_find_object` correctly rejects, because it is not in a block. The
handles *inside* the ring are therefore invisible to every root path the collector
has. So P4 needs live handle-bearing containers to be **root providers in their own
right**: registered when they first acquire a handle element and deregistered at
deinit, walked precisely at every collection. The hook exists — containers already
have an element lifecycle through `rt_elem_ops` (`rt_elem_init` / `rt_elem_deinit`,
`runtime/Containers.cpp`) — and `rt_gc_range_add` already establishes the pattern
for a runtime-owned region the collector must scan. The alternative, making
container payloads GC-allocated, was rejected: it would drag strings, dynamic
arrays and queues out of the ownership discipline that `data-model.md` §8 already
gets right for every non-class kind.

**This was the sharpest limitation of the landed work, and it is now closed.** For one
phase `--gc` refused a class handle inside a container outright, because the
alternative is silently freeing an object whose only reference is a container element,
and per `PRINCIPLES.md` that has to be a located failure rather than a quiet
fallback. Worth recording why the refusal existed at all: an earlier draft of this
document asserted the shape was already loud-guarded in the frontend, citing `TODO.md`.
It was not — `C q[$]; q.push_back(a);` compiled and ran uncollected [in-tree],
so the collector was one guard away from freeing live objects in a shape users
already write.

P4 lifted it, which is what made a UVM component tree collectable: `uvm_component`
holds its children in an associative array of handles, so this was the gate on the
workload the collector exists for, not a nice-to-have.

### 3.3a Associative-array keys are strong references

A key is not stored as a value. It is normalized into bytes and used as the hash and
ordering key (a `std::string` in the runtime), so for a class-handle key **those bytes
are the only copy of the handle**. The entry is a strong reference: `m[obj] = 1` keeps
`obj` alive, and `m.first(k)` hands the handle back for dereference. The collector
therefore has to decode handles out of key bytes and mark them.

`rt_elem_ops` carries `nkeyshape` / `keyshape` for that: a **program**, not a flag and
not a table of offsets. A flag can only say "the key *is* a handle", which covers
`int m[Cell]` and nothing else. An offset table covers a key of fixed layout, where a
handle can sit at any offset and a struct can carry several, but not a key that holds
a string or a container, whose layout differs per entry:

```systemverilog
typedef struct { byte b; Cell c; shortint s; } off_t;  // handle at byte 1
typedef struct { string s; Cell c; }           var_t;  // handle behind the string
typedef Cell                                   row_t[$]; // a count, then handles
int m[off_t];
int n[var_t];
int q[row_t];
```

The shape is a sequence of nodes in serialization order (`Runtime.h`, `rt_key_*`):

| node | bytes it describes |
|---|---|
| `skip n` | `n` bytes that hold no handle |
| `handle` | 8 big-endian bytes, a class or event handle (unsigned, so no sign flip) |
| `string` | a `u32` length, then that many bytes |
| `seq w body` | a `u32` count, then the body per element (queue, dynamic array) |
| `rep n w body` | the body `n` times (fixed unpacked array) |
| `map kw vw key value` | a `u32` count, then per entry a `u32` key length, the nested array's own key bytes and the value |

A bare `int n[Cell]` is the one-word program `{ handle }`, not a separate path. A key
type that can reach no handle has no shape at all, and `nkeyshape != 0` is what the
write barrier asks too. Inside a shape every node is complete, handle or not, because
the walk has to get past the bytes. The one exception is a nested array's key: it
travels behind its own length, so it is passed over by that length when it holds no
handle, which is also the only way past a bare string key.

**The shape is serialized, not storage.** That is the one thing to get right here: a
`getStructLayout` shortcut would place the handle after the struct's padding and decode
the wrong bytes — passing every same-offset test and failing the moment a `byte` or a
`shortint` precedes the handle. The backend's `key_shape` (`ir/llvm/GcEmit.cpp`)
follows the serializers arm for arm: `agg_key_bytes` (which sizes a fixed span),
`emit_agg_key_bytes` and `emit_variable_agg_key_bytes` (which write one).

The collector's `walk_key` (`runtime/Gc.cpp`) interprets it with an explicit stack and
checks every read against the end of the key: the shape and the bytes come from two
places, and a disagreement ends the run instead of reading past the key.

Verified with the objects reachable *only* through key bytes, against a filler that
recycles and poisons freed lines, for every shape above and their nestings
(`tests/gc_test.cpp`).

### 3.4 chandle disambiguation

After lowering, a `chandle` and a class handle are both a 64-bit two-state plane
(`InternPool.cpp:177-180`, `lower/Types.cpp, lower_type`); the distinction survives only
in slang's AST. A "this plane slot is a GC handle" bit must be threaded from
`lower_class_type` into `FrameSlot` / `SignalSlot` / `rt_elem_ops`, or a precise
root table would trace foreign DPI pointers.

---

## 4. Heap

### 4.1 Chunks, blocks and metadata

- **4 MiB chunks, aligned to 4 MiB.** A chunk is one mapping: a record page, the
  metadata for the whole chunk, then 119 blocks of 32 KiB. Because a chunk is
  aligned, the chunk an address belongs to is the address with its low 22 bits
  cleared, and nothing is searched to find it.
- **One metadata byte per 16-byte granule**, at `chunk + 4096 + (offset >> 4)`:
  6.25% of the heap. The byte is 0 for a free granule, has its top bit set with
  the object's flags (§3.1) for the first granule of an object, and otherwise
  holds the distance back to the object's first granule, capped at 127 so that a
  long object is walked in hops. An interior pointer therefore resolves to its
  object in one load for an object up to 2 KiB and a few for a larger one.
- **A table of chunks** (a hash map keyed by `address >> 22`, behind a lowest
  and highest heap address) says whether an address is in the heap at all. Only
  the conservative scan and `rt_gc_find_object` consult it: generated code and
  the marker reach metadata from an object they already know to be one.
- **Marks are bytes, not bits.** Parallel marking therefore needs no CAS and no
  atomics for the mark itself: two threads racing to mark an object write the
  same value.
- **Alignment 16, not 64.** The 64-byte guarantee exists for coroutine frame plane
  spills (`runtime/Scheduler.cpp, rt_alloc`) and is waste for objects.
- **An object larger than a block** (32 KiB) has a mapping of its own, aligned as
  a chunk is and entered in the same table, with the object at a fixed offset so
  that the same arithmetic finds its metadata byte. When the object dies its
  mapping leaves the table and is kept for the next object of the same size
  (§4.2).

### 4.2 Reclamation

A sweep walks a block's metadata from object to object. A marked object stays; an
unmarked one has its finalizer run if it owes one, and then its metadata bytes are
zeroed. That is all freeing is: the allocator finds free space as runs of zero
metadata bytes, and uses any run the request fits in. No object is read except to
be finalized.

The sweep reads the metadata eight granules to a word. A block in which nothing is
marked, the usual end of a block of short-lived objects, is freed whole: its
finalizers are run if any object owes one, and its metadata is zeroed in one go.
Only a block that keeps something is walked object by object.

A young collection sweeps only the blocks allocated from since the last collection
and the large objects allocated since then, because nothing else can hold an
unmarked object. A full collection sweeps every block that is not wholly free. So
a young collection's sweep costs what was allocated, not the size of the heap.

The sweep leaves each block it visits wholly free, full, or partial with its longest
free run recorded. Partial blocks are listed by that run's length, one list per
power of two, so an allocation that needs a block takes one whose run is known to
fit without searching: a partial block first, a free block only when no partial one
is certain to fit. A block in the class just below the request may fit and is
passed over; it serves smaller requests.

A full collection leaves the heap as many free blocks as were allocated from since
the last collection, and never fewer than one allocation budget's worth: what the
next cycle will want again. A chunk with no object beyond that goes back to the
system. So a design allocating at a steady rate keeps its chunks, and one whose
live set has shrunk returns the difference at its next full collection.

Large objects follow the same rule. A dead one's mapping is kept, by object size,
and the next object of that size is put in it with nothing mapped; a full
collection returns, of each size, the mappings beyond the number of objects of
that size allocated since the last collection. A kept mapping still counts as
heap against `--gc-heap-max`, and the heap gives all of them back before it reports that the
ceiling or the system has no room. It is not in the old generation, so it does not
bring a full collection on.

### 4.3 The allocation fast path

Bump within the mutator's current block, refill at a hole boundary, call out when
the block is exhausted. Emitted inline; the size is a compile-time constant at
almost every `new` site because `getTypeAllocSize` is known at codegen.

```llvm
%mut  = <the mutator context pointer>                  ; NOT a link-time constant
%cur  = load ptr, ptr %mut.cursor
%next = getelementptr i8, ptr %cur, i64 <size>
%fits = icmp ule ptr %next, %limit                     ; %limit loaded alongside
br %fits, label %fast, label %refill
fast:
store ptr %next, ptr %mut.cursor
; metadata bytes, zeroed body, vptr store follow
```

The **mutator context pointer is the whole MT story of the allocator**. Under a
single mutator it is a constant that folds away to nothing; under N mutators it is
whatever the MT scheduler hands generated code — a new uniform call parameter
(F5's precedent, one register), a field seeded into the coroutine frame at resume,
or eventually native TLS if RTDyld's TLS support stops being a liability
(`Jit.cpp:137-148` uses `RTDyldObjectLinkingLayer`). The GC's requirement is only
that the address is *loaded*, so the choice can be made later without touching the
collector. Baking `@clotho.gc.cursor` as a fixed global would foreclose it, which
is the mistake this section exists to prevent.

Blocks are owned by one mutator at a time, so the fast path has no contention at
all; only block acquisition touches shared state, and that is one lock-free pop
per 32 KiB of allocation.

**This is faster than today.** `new` currently calls out to `rt_alloc` = 64-byte
aligned `operator new`. For scale, inlining a comparable fast path measured
33.1 → 94.9 Mops/s on bdwgc, and MPS's allocation-point protocol compiles to six
instructions **[probe]** — allocation throughput improves before any collection
benefit, on any collector, purely from inlining.

**OOM.** `rt_alloc` returns null on failure and the `ClassNew` site deliberately
does not check (`ir/llvm/Inst.cpp, lower_inst` elides null guards on a fresh
handle). The Immix allocator must preserve "never returns null" by taking the
existing located-fatal path (`alloc_bytes`, `runtime/Reports.cpp`) after a
collection fails to satisfy the request.

### 4.4 What the layout costs

Two measurements, each within one tree, in the shipped configuration: Release,
extra assertions off, default tier. Instructions are user-mode, per iteration, the
least of three runs, from two runs of different lengths; time is CPU time for the
longer run, the least of five, and includes about 0.1 s of compilation.

**The layout against the collector it replaced** (an 8-byte header word, 128-byte
line marks, object-start and finalizable bitmaps, large objects from 8 KiB in
malloc'd cells). When this was measured a dead large object's mapping was unmapped
at once, and `memset` in the simulator was a byte loop on both sides (below).

| per iteration | before | after | time |
|---|---|---|---|
| allocate and drop a 24-byte-body object | 121 | 110 | 474 to 427 ms |
| the same with a `string` property (a finalizer) | 306 | 237 | 665 to 547 ms |
| the same beside 200,000 live objects | 215 | 150 | 395 to 295 ms |
| replace a random element of two arrays of handles (20-byte and 100-byte bodies) | 457 | 395 | 2621 to 1262 ms |
| two handle stores into an old object | 32 | 42 | 313 to 307 ms |
| allocate and drop a 12 KiB object | 29.7k | 36.8k | 4269 to 4621 ms |
| allocate and drop a 64 KiB object | 157.0k | 156.4k | 5025 to 15004 ms |

What is behind the three that got worse:

- **The barrier** finds the owner's metadata byte by arithmetic on its address,
  where it used to load the word before the object: five instructions more per
  handle store into an object.
- **An object of 8 KiB to 32 KiB** is now in a block and allocated inline, where it
  had a malloc'd cell. Its metadata is 1/16 of its size to write and to clear, and
  what is left of a block after the last object that fits is unused until the next
  collection sweeps the block: a 12 KiB object leaves a quarter of each block, an
  object just over 16 KiB nearly half.
- **A mapping made and unmade per large object** cost about 50 microseconds: the
  system calls, and a page fault for each page the object touches. That is the
  last row, and why a dead object's mapping is now kept (§4.2).

**Kept mappings, and the C library's `memset`.** Two later changes, measured on a
later tree whose generated code had changed as well, so its first column is not the
table above's last. The simulator links libsva, which bundles Zig's compiler-rt,
and that defines `memcpy`, `memmove`, `memset`, `memcmp`, `bcmp` and `strlen` as
weak symbols; linked in, they replaced the C library's for the simulator and for
the code it generates, and its `memset` is a byte loop that the default tier calls
at every `new`. libsva now keeps those names to itself in an ELF archive.

| per iteration | before | kept mappings | and the C library's `memset` | time |
|---|---|---|---|---|
| allocate and drop a 24-byte-body object | 101 | 101 | 59 | 445, 437, 248 ms |
| the same with a `string` property | 228 | 228 | 140 | 565, 583, 362 ms |
| the same beside 200,000 live objects | 141 | 141 | 98 | 321, 306, 208 ms |
| replace a random element of two arrays of handles | 388 | 387 | 308 | 1372, 1408, 1366 ms |
| two handle stores into an old object | 37 | 37 | 37 | 293, 293, 293 ms |
| allocate and drop a 12 KiB object | 36.8k | 36.8k | 19.5k | 4823, 4996, 2489 ms |
| allocate and drop a 64 KiB object | 156.4k | 155.9k | 69.9k | 15943, 4870, 2697 ms |

Peak resident memory for the last row is 214, 226 and 221 MiB: the kept mappings
are those of one cycle.

---

## 5. The root set

| Roots | Mechanism | Precision |
|---|---|---|
| Design state: `@clotho.state`, `@clotho.inst.<hier>.state`, package variables, static class properties | Compile-time `clotho.gcroots` table per region of `{ ptr base, u32 nhandles, u32 bytes, u32 offsets[], u32 cards }`, emitted where `emit_main` already walks `slots_` (`ir/llvm/Scheduler.cpp`); statics from `declare_static_prop` (`lower/Types.cpp`). A young collection reads the cards stored to since the last one (§7) | precise |
| Live coroutine frames (processes, blocking tasks, fork children) | Intrusive `{ prev, next, base, size, ran, open }` node in the frame, linked at the ramp, unlinked at frame free. A young collection reads the frames that ran since the last collection and the open ones (§5.5) | conservative, validated, **pinning** |
| Managed containers reachable from the above | `rt_elem_ops` walkers (§3.3) | precise |
| Intra-assignment staging records carrying a class handle (`rt_intra_alloc_rooted`, `intra-assignment-timing.md`) | `rt_gc_range_add` from alloc to release, scanned as a conservative range | conservative, validated, pinning |
| NBA record queues (`rt_nba_arr_slot`: array element, virtual-interface member, aggregate leaf, whole container) | Registered by shape at start-up (`rt_gc_nba_words_add` / `rt_gc_nba_container_add`); the staged records are read at each collection | conservative words, or precise through the container's element descriptor |
| `$strobe` / `$monitor` pended bodies | No image of their own: the body is a zero-formal coroutine whose frame is registered (above), and it evaluates its arguments in the Postponed region on the native stack. `$monitor` keeps only the last rendered text. | conservative frame / stack |
| What the runtime holds of an event object: the NBA pending chain, a `->> #d e` timer, a `->> @(x) e` subscription, a dumped event | Enumerated at each collection (`rt_internal::event_gc_roots`, `src/runtime/Scheduler.cpp`) | precise |
| Wheel timers, wait buckets, the active ring, fork/disable records | Nothing to do — they hold *frame* pointers, and frames are enumerated above | — |
| VCD var tables, `rt_ref_names`, `chandle` | **Not roots.** VCD caches only pointers into `clotho.state`; `chandle` is foreign | — |

### 5.1 Frame registration

`build_frame_layout` (`ir/llvm/Subroutines.cpp`) already places all SV locals in
a single `frame_ty_` struct alloca positioned after `llvm.coro.begin`, which is
why the registry node can be an alloca too: CoroSplit hoists it into the frame and
rewrites uses to a stable frame-interior address **[probe]**. The node records the
frame base and `llvm.coro.size`, both already materialised for the
`rt_alloc` call. Under MT the registry becomes per-mutator lists spliced at the
rendezvous, so registration stays uncontended.

Link at the ramp, unlink at every path that frees the frame: a frame left in the
registry after being freed is a use-after-free inside the collector, so this was
settled by census rather than by reading prose. There are exactly **two** frame-free
sites, the process ramp's and the task ramp's `coro.free` blocks, and all eleven
`llvm.coro.destroy` sites route into the coroutine's destroy clone, which runs the
same cleanup. So the unlink is emitted once per ramp in `coro_cleanup_bb_`, which
dominates both. An earlier draft of this section claimed two further raw-destroy
paths that skip the epilogue, citing `disable.md:259-262`; those line numbers hold
`ref_actual_ptr` and `ref_needs_epoch` and no such path exists.

The registered length is `coro.size`, not `coro.size + coro.align`. The ramp
allocates `size + align` and `coro.begin` returns the *aligned* base, so scanning
`size + align` from there would read up to `align` bytes past the allocation on any
frame LLVM actually realigned.

Non-coroutine code needs nothing. Plain functions (`declare_function`) and comb
thunks cannot suspend, so at a region boundary none of their frames exist. No
handle temporaries are spilled and no shadow stack is pushed on any call — the
per-call cost of the whole root scheme is zero.

### 5.2 Why frames are conservative, and what that costs

A frame holds SV locals at offsets clotho chose *and* SSA values CoroSplit spilled
at offsets it chose. A handle temporary live across a suspension is in the second
category, so a precise map built from `frame_ty_` alone would miss it and free a
live object. The alternative — spill every handle temporary into `frame_ty_`,
mirroring `build_str_temps`, and prove the liveness analysis complete — is
strictly more code with a use-after-free as its failure mode.

Conservative here is much stronger than Boehm-style conservative, because every
candidate word is validated against our own metadata: it must land in a mapped
chunk and in a granule some object occupies. The same lookup resolves
an *interior* pointer to its object start, which is what makes
`ref`-into-a-class-property (F6) work with no epoch machinery at all.

Costs, both bounded and both measurable: scan time proportional to the bytes of the frames a
collection reads (every live frame in a full collection, the frames that ran in a young one,
§5.5), and retention of one object graph per stale frame slot
until that slot is overwritten — in a UVM sequence loop, the next iteration. §13
specifies a mode that scans both ways and reports the difference, so this number
is measured rather than asserted.

### 5.3 Pinning

Conservative frame hits are pinned for the duration of the collection. Objects
with an outstanding `ref` into them are pinned for the duration of the call: the
`ref` ABI already carries an optional second word for container epochs
(`:3495-3505`), and the same slot pattern carries the pin token for a class-rooted
`ref`. Immix honours pins natively — the paper: "When the application requests
pinning for an object, immix sets a pinned bit in the object header.
Defragmentation never moves pinned objects." Under a non-evacuating policy the pin
bit is set and never read, and the whole mechanism is dead code that costs a store.

### 5.4 How much is actually precise: an `initial` block's locals are not in a frame

A declaration inside an `initial` block is *static* in SystemVerilog unless it says
`automatic`, and clotho gives it storage in the design state buffer. So the handles
in the shape testbench code most often writes —

```systemverilog
initial begin
    Cell only = new;   // static storage: a PRECISE root
    ...
end
```

— are precise roots, not conservative frame words. Measured on a two-handle design:
the emitted `clotho.gcroots` table carries both, the collection reports both live,
and `conservative_pins` stays 0 [in-tree]. Only per-activation locals — an
`always` block's `automatic`, a task's or function's locals, a `fork` child's —
land in a coroutine frame and need the conservative path.

Two consequences. **Good:** false retention is confined to genuinely
per-activation state, which is a much smaller surface than "all process locals",
so §5.2's cost is smaller than the design assumed. **Dangerous:** it makes a
frame-root test trivially easy to write as a precise-root test that passes with
the registry never consulted. That happened twice while writing
`tests/gc_test.cpp` — once in the lead's own smoke design and once in the
committed suite — which is why the frame case now asserts
`conservative_pins > 0` and why the `always` + `automatic` shape is spelled out
in its comment.

### 5.5 Which frames a young collection reads

A frame's words change in two ways only: its own coroutine runs, or a callee it is parked in
stores through a pointer into it. So the registry is kept in two parts, divided by a node of the
collector's own, the **sentinel** (`g_frame_sentinel`, linked again by `reset_mutator`, never a
frame). A frame links at the head, so before the sentinel. A young collection reads the frames
before the sentinel and a full collection reads them all. A frame behind the sentinel was read by
a collection that left every object it names marked, so old, and is unchanged since.

Two bytes of the node keep the division:

- `ran` is 1 while the frame is before the sentinel. The link sets it. Where a coroutine resumes
  from a park (`emit_suspend`, which every park goes through) generated code loads it and, when it
  is clear, calls `rt_gc_frame_ran`, which sets it and moves the node to the head: a load, a
  compare and a not-taken branch for a frame that has run since the last collection.
- `open` is 1 while the coroutine is parked in a task call that handed its callee a pointer it can
  store a class handle through into this frame (`task_call_opens_frame`): a `ref` formal, or an
  output or inout formal the callee copies out through, whose type reaches a class handle and whose
  actual is not known to be in an object or in design state. A container formal is not one: its
  elements are in a payload the container roots cover. The call site sets it before the park and
  clears it after its own resume.

A collection at a safepoint (every reason but an emergency) **retires** what it read. No coroutine
is running and every object a read frame names is now marked, so it clears `ran` on each frame it
read before the sentinel and moves the sentinel to the head, which is constant work; the open
frames it found go back before it and keep `ran`. A collection inside an allocation
(`RT_GC_REASON_EMERGENCY`, which is also what `--gc-stress` runs at every allocation) reads the
same frames and retires none: the coroutine it interrupts has already passed its hook and goes on
storing into its frame.

**Why this is sound** is the list of everything that can write a word of a parked frame:

| Writer | Covered by |
|---|---|
| The frame's own coroutine, after a park | The hook runs before its first store, so the frame is before the sentinel |
| The frame's own first segment | The link sets `ran`. Only a safepoint's collection retires, and the scheduler polls the safepoint at the wheel advance, after every ring has drained, so every frame ramped since the last one has run its first segment |
| A callee storing through a `ref` formal bound to a place in the frame | `open`, set around the caller's park in that call |
| A callee copying an output or inout formal out into the frame | `open`, which the caller clears only after its own resume, so also between the copy-out and the caller's hook, where other processes run |
| A `ref` forwarded down a chain of task calls | The frame that holds the place is parked in the outermost call, open for the whole chain |
| A function storing through a `ref` or output formal | Not a coroutine: its caller is running, so before the sentinel |
| A fork child | Writes the capture boxes, which are heap objects (fork-join.md V4), not its parent's frame |
| A blocking mailbox or semaphore method | Expanded into the waiter's own coroutine (`SyncWait`), so it pops and stores in its own activation, after its hook; it is not a task call |
| The runtime's wake-ups: event, semaphore, mailbox, `wait fork`, `kill`, `disable` | Flags and list links, which are addresses of frames and resolve to no object. A coroutine they wake runs its hook |
| NBA targets, staging records | Design state and registered ranges, not frames |

The extra-assertion build checks it. Before every collection it asserts that each frame before
the sentinel has `ran` set and each frame behind it has it clear and is not open, and before a
young collection marks anything it asserts that no word of a frame behind the sentinel resolves
to an unmarked, young object (`verify_frame_registry`). A writer missing from the list therefore
fails an assertion at the next young collection instead of freeing an object that a frame holds.

**What it costs.** A young collection costs what ran. With N task frames parked on an event that
never fires and one task that churns, the last young collection reads one frame at N = 1,000,
100,000 and 1,000,000, and a churn step (1,000 allocations and the collection they cause) takes
about 124 thousand instructions at each N, where reading every frame took 174 thousand, 5.9 million
and 54 million (shipped build, 256 KiB nursery, user instructions). The hook is two instructions,
a compare of a byte of the frame and a not-taken branch, at each resume of a registered frame and
nothing for a frame that is not registered: 2.0 instructions on a resume path of 61 (`#0`) to 101
(timer) in a design of 50 frames that do nothing else. A registered frame pays two byte stores at
its link, and a task call that opens its caller's frame two more around the park.

**What it does not remove.** Frame-local containers (`g.containers`), registered ranges and NBA
queues are separate root sets that every collection reads, so a design with a queue local to each
of a million parked tasks still pays for them.

---

## 6. Safepoints

### 6.1 Today: single mutator

Collection runs at the drain-loop quiescent edges (F1) when the allocation budget
is exceeded — a nursery budget in bytes plus a heap-size threshold, both
benchmarked (§11), with `--gc-stress`
collecting at every safepoint. No polls are emitted in generated code.

**The unsuspended-allocation hole, and its answer.** A process may allocate without
bound between suspensions; `for (int i = 0; i < 1_000_000; i++) obj = new C();` is
legal and reaches no region boundary. Crossing the budget raises the request flag,
which a boundary would answer; when none comes, an **emergency collection** runs at
the allocation slow path (`rt_gc_alloc_slow`, once per hole, so the inlined fast path
is untouched) after `kEmergencyFactor` (4) budgets have been charged with no
collection in between. It runs *before* the allocation, so no half-built object
exists, and it is a young or full collection by the ordinary policy. Failing loudly
was rejected: the loop is legal SV. `rt_sim_run` records the address of one of its
locals as the stack base (`rt_gc_run_enter`); with none recorded -- static
initialization, a test calling the allocator -- allocation never collects.

One allocation never collects by design: an event object constructed as a
container ELEMENT (`rt_gc_alloc_held`, through `rt_event_new_elem`). Element
construction runs inside container operations, and the elements an operation has
built so far sit in a buffer its container does not name yet, where no collection
would find them. The budget is still charged, so the next ordinary allocation or
safepoint collects.

**`--gc-heap-max` is enforced here.** When the heap would have to grow past the
ceiling (in whole 4 MiB chunks, at least one), the allocation runs a full emergency
collection, retries, and ends the run with a located fatal (the `new` statement, via
the `rt_alloc_site` the backend now passes to `rt_gc_alloc*`) if the objects still do
not fit. The ceiling counts the object heap only, not owned buffers (§6.5).

**Why collecting inside an allocation is sound.** The collection runs with JIT frames
live, so beyond the registered roots it scans the native stack and the callee-saved
registers conservatively (`emergency_collect` spills them with `__builtin_unwind_init`
into a frame above the one whose address starts the scan; the scan runs to the base
`rt_sim_run` recorded). The argument:

1. *Handles.* A value generated code uses after the allocation call is live across a
   call, hence in a callee-saved register or a stack slot, hence found. Every word is
   validated by `rt_gc_find_object` like a frame word, interior pointers included, and
   pinned; the heap is non-moving, so a pin is retention and nothing else.
2. *The fresh object.* A store into a handle straight out of `new` carries the barrier
   like any other. An emergency collection may intervene (a property initializer or
   constructor argument that is itself a `new`, the binding of an event property in the
   body), and the fresh handle is live across it, so it is found and promoted, and the
   store that follows lands in an old object. The same holds of a store through a `ref`
   or into an `out` actual: the callee logs the owner from the address
   (`rt_gc_note_ref_store`), at each store, so a collection between the call and the
   store, in a task or in a function that allocates, changes nothing.
3. *Pointers into owned buffers.* A pointer into a malloc'd payload (a container
   element computed before the allocation) cannot be resolved to its owner. A finalizer
   run by an emergency collection therefore does not free a buffer that a word of the
   stack lies in (`buffers_park`): it is parked and re-tested at the next collection.
4. *Staged nonblocking assignments.* Staging and commit used to be separated by no
   collection (the only one followed the NBA commit), so nothing that holds a staged
   handle had to be a root, and `h <= new C` followed by enough allocation in the same
   time step freed the handle before it committed. Every place one can sit is now a root:
   - *The block shadow* of a scalar or struct-member target that can hold a handle: a
     conservatively scanned range (`emit_gc_root_tables`, `rt_gc_range_add`).
   - *The record queues* (`rt_nba_arr_slot`, one growable buffer per signal) of an array
     element, a member written through a virtual interface, an aggregate leaf reached
     through a runtime index, and a whole-container copy. The buffers move as they grow,
     so a record cannot be a range of its own; the queue is registered once at start-up
     (`emit_gc_nba_queues`, `rt_gc_nba_words_add` / `rt_gc_nba_container_add`) and each
     collection reads the staged records through `rt_nba_arr_base` / `rt_nba_arr_count`:
     a word-scanned record for a handle in the record, the element descriptor of the
     container image for a whole-container copy. An aggregate record pads its payload to
     a word boundary (`agg_rec_payload`) so the scan sees a handle leaf.
   - *The records of a delayed* (`<= #d`, `<= @e`) assignment: ranges from allocation to
     release (`rt_intra_alloc_rooted`).
   A class-property target is refused by the frontend. The record queues were claimed to
   be roots by this audit's first version and were not; `gc_test` stages through runtime
   indices and a virtual interface so that no stale copy on the stack can hide it.
5. *Not a root:* a buffer pointer held only in a suspended frame is the existing task
   case, unchanged.
6. ASan: the scan loop is `no_sanitize("address")` since it must read redzones.

`--gc-stress` extends to "every opportunity": with a stack base recorded, every
allocation empties the hole so the next `new` takes the slow path and collects. That
makes the existing stress runs a dense test of items 1-4.

**Audit of what assumed "a collection happens only after the drain".** Each place that
said so, and what now holds:

| assumption | resolution |
|---|---|
| NBA shadow needs no root (`StoreNb` barrier comment) | item 4: shadow registered as a range |
| array-element, aggregate-leaf, whole-container and virtual-interface NBA records are roots | they were not: item 4, the record queues are registered and scanned |
| fresh `new` needs no barrier (`HandleDeref`) | item 2: it has one, and so does the event binding of a class body |
| barrier then store, nothing between (`Store`) | still true: nothing between them allocates |
| call-site barrier is airtight for a function (`lower_call_common`) | item 2: there is no call-site barrier; the callee logs the owner from the address at each store (`rt_gc_note_ref_store`) |
| frame unlink with no safepoint after it | still true: the cleanup path allocates nothing |
| vptr stored before any collection sees the object | still true: nothing allocates between `rt_gc_alloc` and the vptr store |
| deferred (`<= #d`) targets are design state | unchanged: asserted, and the records are roots |
| class-property NBA | refused by the frontend, so no staging record names an object body |
| `$strobe` / `$monitor` argument images | there are none that outlive a collection (the first version claimed precise roots for them): arguments are evaluated in Postponed on the stack an emergency collection scans, and `gc_test` evaluates both across collections |
| the safepoint comment in `emit_active_run` | describes the region-boundary collection, which is unchanged; the emergency one is additional |
| `new` as a blocker in `Scans.cpp` | already treated as a safepoint, so no purity analysis relied on its absence |

### 6.5 Buffers owned by objects

A `string`, dynamic array, queue or associative array inside an object owns malloc'd
memory the collector frees only by finalizing the object, yet the budget counted only
the object. `GcBuffers.h` is the whole mechanism: `rt_alloc_checked` charges each
buffer, every free site credits the size it allocated (so the count is exact: it is
zero after a run that frees what it allocates, asserted in `gc_test`), and growth of
the count by a budget since the last collection raises the request flag, by
`kEmergencyFactor` budgets makes the next `new` take the slow path where the emergency
collection runs. The kind policy compares the old generation, the objects a collection
has marked and the buffers owned as the last collection left them, with its size at the
last full collection; otherwise a young collection never frees an old dead object's
strings. An associative
array's key strings and hash buckets use `std::allocator` and are not counted.

### 6.2 Under MT: a rendezvous the scheduler already needs

Region boundaries are global synchronisation points in any correct parallel
SV scheduler (F1), so the collector adds no synchronisation of its own for the
common case: the last mutator to arrive at the barrier runs the collection while
the others wait, which is work they were already going to wait for.

What MT genuinely adds is **bounded-latency stopping** for the emergency path: a
mutator deep in a non-allocating loop must be brought to a safepoint. That needs
poll sites, and the poll is one load of a per-mutator request byte plus a
predicted-not-taken branch, emitted at loop back-edges. The zero-branch
alternative is Julia's `mprotect`-ed safepoint page (`src/safepoint.c:59-66`),
where the poll is a bare load and the trap does the work — cheaper, but it puts a
`SIGSEGV`/vectored-exception handler in a process that also hosts the DAP debug
loop, which is one of the reasons MPS was rejected (§10). Which one lands is an MT
decision, not a GC decision; the collector's requirement is only that a mutator
can be stopped in bounded time and that stopping is initiated by a store to its
context.

Per-mutator state the design assumes: allocation cursor/limit, a card-buffer or
remembered-set cursor, the frame registry list head, and the request byte. All in
one cache line, reached through the §4.3 pointer.

### 6.3 Cooperative `$finish` is a prerequisite

`$finish` today longjmps out of live JIT'd coroutine frames, abandoning frames and
leaving runtime globals mid-update (`CI.md` closing section;
`cooperative-finish.md:24-28`), which the test harness works around with
subprocess isolation. A collector holding a frame registry across a run inherits
that corruption directly: abandoned frames are never unlinked.
`cooperative-finish.md`'s fix — branch to the coroutine's cleanup so it runs
`coro.end` and frees its own frame — is what makes the registry invariant hold,
and it is already called the highest-value follow-up in `CI.md`.

At `$finish`, sweep every block running finalizers, then release the blocks. Not
needed for correctness, but it is what lets the `ASAN_OPTIONS=detect_leaks=0`
overrides at `tests/data_model_test.cpp:701-705` and
`tests/dynamic_array_test.cpp:524,578` be deleted, which is the acceptance
evidence in §13.

### 6.4 Paying only for what a design uses

Collection is rare; the *machinery around it* runs on the scheduler's hot path, and
that is where the first version of this work went wrong. Two costs, both per-event
rather than per-object:

| cost | was | is |
|---|---|---|
| Safepoint | `call rt_gc_safepoint` per wheel advance | inlined poll: `cmpl $0, 28(%rax)` + a cold-weighted branch, no call |
| Frame registry | `rt_gc_frame_link` + `unlink` per process activation | emitted only for coroutines whose frame can hold a reference |

**The poll is a byte.** `rt_gc_mutator::request` is set by the allocator when the
nursery budget runs out, so a design that allocates nothing never sets it and the
branch is never taken. Generated code loads it at an offset the backend computes
from its own mirror of the struct and cross-checks through
`rt_gc_config::request_offset`, the same discipline as `map_vptr_offset`. Stress
mode cannot be a second test in generated code — the poll is all there is — so a
stress collection *re-arms* the byte, which is what keeps "collect at every
safepoint" meaning what it says.

**The registry is conditional on a sound predicate.** `frame_may_hold_gc_ref`
returns true if a coroutine's IR has any class-typed local, any class-typed
parameter (a method's implicit `this` arrives this way), **any `ref` formal at
all** — a `ref` bound to a class property is a raw interior pointer into an object
body with no class-typed value in sight — or any instruction that produces or
consumes a class-typed value. That last clause is required rather than decorative:
CoroSplit spills temporaries at offsets nobody chose, so a declared-local test
alone would miss a handle that only ever exists as an SSA value across a
suspension.

The exhaustiveness argument: a frame holds the coroutine's allocas plus the values
live across a suspend, so a frame word can only acquire a reference through a
declared local, a formal, a `ref` actual, or a spilled temporary — and every
class-typed value in a body is rooted in one of four producers (`ClassNew`, a read
of class-typed module storage, a call returning a handle, or a handle-typed
formal/local). Containers are not a fifth producer *because* §3.3 refuses them
under the collector; when P4 lifts that guard, the predicate gains a door and this
paragraph has to be re-argued.

Measured on the design this is for — 5M edges, 10M process activations, one
long-lived object, zero churn — exactly one of ten processes registers a frame (the
one containing the `new`), the two `always @(posedge clk)` blocks and the worker
register nothing, and the cost is +0.6%. `tests/gc_test.cpp` pins the predicate
with five shapes that each reach a handle through a different door while a filler
recycles and poisons freed lines, because a missed door is a use-after-free and the
cheap version of this predicate looks correct.

Still out of line, and P2's job: the allocation fast path itself, and the
link/unlink pair for the frames that do register (~7 inline instructions each,
against two calls today).

---

## 7. Generational collection

The workload is a large stable population (component tree, factory, config DB,
coverage, objections) plus violent young churn. Re-marking the stable set every
collection is the cost to avoid.

- **Sticky mark bits (StickyImmix).** In-place generational: young collections
  trace from roots plus the remembered set and stop at marked objects. The
  paper introduces this variant explicitly "to support pinning", which is exactly
  the property §5.3 needs. Julia's MMTk configuration defaults to `StickyImmix`
  and Ruby's to Immix with a non-moving nursery; mmtk-core's own default is
  `GenImmix`, i.e. a copying nursery over an Immix mature space — all three are
  reachable from this heap organisation without redesign.
- **Object remembering, not card marking.** This document specified an unconditional
  card store; the implementation is Julia-shaped object remembering, and the change
  was right. A handle store into a heap object tests the *owner's* metadata byte inline —
  `(meta & (MARK|LOGGED)) == MARK` — and calls `rt_gc_remember` only on the taken
  edge, which sets `LOGGED` so a hot object costs the call once per cycle rather
  than once per store. A few IR instructions, cold-weighted, the same idiom as the
  safepoint poll and the null-handle check.

  The reason to prefer it here is the one the card-marking argument got backwards. A
  container's payload is not a GC object, so a card keyed on the *store's address*
  marks a card that no collection scans — the mutated container needs its OWNER
  remembered, and the owner is exactly what the inline test already has in
  hand. `Runtime.h:874-877`'s licence to relocate elements by raw `memcpy` then
  needs no instrumenting either, for a better reason than a range mark: the owner is
  remembered, and tracing the owner walks the container wherever its elements have
  moved to.
- **Design state is logged by cards, and its containers by flags.** Every state region ends in one byte
  of dirty flag for each `RT_GC_CARD_BYTES` (512) of the region. A store of a class or event handle
  into state -- a scalar, an array element, a struct member, the staging of a nonblocking assignment,
  an aggregate copy -- tests the card byte of the place inline (a load, a compare and a not-taken
  branch; the byte is at a constant offset from the signal's storage, so shared code names nothing of
  an instance) and calls `rt_gc_note_state_store` when it is clear. That sets the byte and appends the
  card to a bounded log. A young collection reads the handle slots of the logged cards (the table's
  offsets ascend, so a card's slots are a binary search) and no others; a full collection reads every
  slot; every collection clears the cards it read and empties the log. Past `kLoggedCardsMax` cards,
  and after a table is registered (its initializer is no store's), the next collection reads every
  slot instead. This is sound because every slot a collection read names a marked, old object: a slot
  nothing has stored to since still does. Stores through a `ref` or an output formal reach the same
  entry from the callee (`rt_gc_note_ref_store` finds the region by address). A frame local needs no
  barrier: a young collection reads every frame that ran since the last one and every frame a callee can
  store into (§5.5).

  A container of design state (a queue, dynamic array or associative array whose elements or keys can
  hold a handle) is an entry of its region's root table and has flag bytes of its own after the cards,
  one for each word of every signal that holds one. A card would not do: what a container costs a
  collection is its whole payload, so a queue of a million handles must not be walked because a slot a
  few bytes from its handle was stored to. A mutation that adds a handle to the container -- a push or
  insert, an element store, a whole assignment or move, `new[n](old)`, `new[n]` of events, an
  associative entry (its key or its constructed value), at any depth inside it -- tests the flag byte
  of the word of the outermost container's handle it has the address of (the handle itself, or its
  epoch word for a place inside the payload) and calls `rt_gc_note_state_store`, which logs the cards
  and every container whose handle the range touches. A young collection walks the logged containers
  and no others, a full collection every one, with the same bounded log and fallback as the cards.
  Moving handles within a container (a sort, a ring growth) and removing them add none, so they log
  nothing. A `ref` bound into a container reaches the same entry through its witness chain, and a
  container formal is never bound to a place inside another container (the frontend passes a temp).

  The extra-assertion build checks both: `verify_cards` that a card byte, and a container's flag bytes
  together, are set exactly when the log holds them, and, before a young collection marks anything,
  that no slot outside the logged cards and no element or key of an unlogged container names an
  unmarked (young) object.
- **The mark is the generation.** A young collection never frees a marked object
  and sweeps only blocks allocated from since the last collection, which is what
  lets it skip the stable population without freeing it. Old garbage accumulates
  until a full collection clears the marks and sweeps everything.

### 7.1 Two use-after-free bugs the phase shipped with, and what they teach

Both were found by running the collector, not by reading it, and both would have
been silent data corruption in a Release build.

**Epoch parity is unsound the moment a collection may skip an object.** P1 marked
with a 1↔2 parity toggled per collection and never cleared anything, which is sound
precisely because every collection stamped every live object: a stale value was
always the other parity. Generational skipping broke that premise without anyone
noticing the premise existed. An `OLD` object goes unstamped for arbitrarily many
young collections, so its stale mark eventually matches the current epoch — and a
full collection then read a live object as already-marked, skipped marking its
lines, and unprotected them. A gdb trace of one block's line 0 across four
collections is what pinned it:

```
C1 FULL  epoch=2 | line0 mark=2 old=0   <- marked; protection derived
C2 YOUNG epoch=1 | line0 mark=2 old=1   <- OLD skipped; protected, mark re-published
C3 FULL  epoch=2 | line0 mark=1 old=1   <- stale 2 from C1 read as "already marked"
                                           -> lines never marked -> old=0 -> freed
C4 YOUNG         | the design-state root is now dangling
```

The fix is two mark bits, each stamped by every collection *of its own kind*, which
bounds each one's staleness to one such collection: the young parity for objects that
are not `OLD`, and `OMARK` against a parity that toggles only at full collections for
those that are. Lengthening the cycle to 3 or 4 values does not work — the staleness
is unbounded. This is why Julia's stock collector has four states rather than a
parity bit, which is obvious in retrospect and was not obvious in advance.

**A liveness predicate is not a dedup predicate.** The fix above introduced one
predicate, `is_marked_word = spared_by_age || is_stamped`, and routed every mark test
through it — including the *forced* mark that the remembered set uses. But
`spared_by_age` is true for an `OLD` object in a young collection, so the forced path
refused exactly the case it exists for: a remembered old object was never traced and
the young object it had just acquired was freed while reachable. Eight of the
generational test cases failed on it immediately, all with the filler's `0x5A5A5A5A`
in place of a value.

The distinction the code now makes explicitly: `is_marked_word` answers "may this
collection free the object", `is_stamped && !spared_by_age` answers "has this
collection already been here". They are different questions and only the second is a
dedup guard. One predicate for two questions was the actual mistake.

Neither mechanism exists any more. A mark is one bit that a full collection clears
before it marks, so there is no parity to go stale, and "old" and "marked" are the
same bit, so there is one question left to ask of it. The remembered set traces its
objects by a path of its own (`push_marked`) rather than through the test that
refuses a marked object.

---

## 7a. Weak references (LRM 8.30)

`std::weak_reference #(T)` is an ordinary class with a synthetic body
(`lower/BuiltinClasses.cpp`, `lower_weak_class`): one property of type `T`, the
referent. `new` stores it, `get()` loads it, `clear()` stores null, a shallow copy
copies it, and a user class may extend the class. One fact makes it weak, and it is
a fact about the class: the trace map leaves the referent's offset out of `handles`
and gives it as `rt_gc_map::weak_referent` (`ir::WeakClass`, `class_gc_map_global`).

The collector records each weak reference it marks. When marking is complete it
stores null into the referent of each recorded one whose referent is unmarked
(`clear_weak_referents`), before anything is finalized or swept. Every weak
reference to one referent is cleared in that one pass, which is the atomicity
8.30.1 asks for.

A young collection examines only the weak references it marked, which are the young
ones and the logged ones. That is enough: a weak reference's referent is written
when the weak reference is made and by `clear()`, and never otherwise, so a weak
reference that has survived a collection names an object that collection marked, or
null. An old weak reference therefore names an old object, which a young collection
does not free. `tests/gc_model_test.cpp` has weak objects in its random graphs and
holds every collection to this; its mutator also takes a referent back out of a
weak reference whose referent nothing else reaches and no collection has freed yet.

**Only a collection at the scheduler's safepoint clears.** A collection run from
inside an allocation (§6.1) is in the middle of some process's statement, at a
point the heap's size chose. It marks every referent instead
(`keep_weak_referents`), so a referent it would have found unreachable survives it
and waits for the next full collection at the safepoint. Every process is parked at
the safepoint, so a design sees a weak reference change only between time steps.

**Waiting on `get()`** (8.30.3, `wait (w.get() == null)`). A specialization that
some `wait` or `@` reads `get()` of carries a second field, the referent's change
notifier, the event object a waited-on property has (`ir::ChangeNotify`). `clear()`
is a store and fires it as any store to a waited-on property does. The collector
puts it on the chain `->>` pends events on (`rt_internal::event_pend`), and the
safepoint fires that chain as soon as the collection returns
(`emit_gc_safepoint`), on the cold side of its poll. The waiters woken are exactly
those waiting when the reference was cleared, and they run in the active region of
the time step that is beginning.

The scheduler edge at which the wheel is idle is not a safepoint
(`ir/llvm/Scheduler.cpp`). The run ends there, so a collection would free what
teardown is about to free, and a waiter it woke would have no time step to run in.

`get_id` (8.30.5) is the identity number of §3 (`rt_gc_identity`): 0 for null, the
object's own whatever the handle's type, and never handed out twice.

What a design sees of a weak reference depends on when collections run, which
`--gc-nursery`, `--gc-full-every` and `--gc-stress` change. `tests/weak_reference_test.cpp`
says what each cadence must do; a cadence that never collects never clears.

---

## 8. Evacuation: what it costs and what it buys

Immix's opportunistic defragmentation picks candidate blocks from hole-count
histograms before a collection, then in the single mark pass "if the object is
unmarked and movable, it evacuates the object, installs a forwarding pointer …
If an object is pinned by the application or is on a target page, immix simply
marks the object, leaving it in place", stopping when the target space is
exhausted. It is a policy knob over the same heap, with 2.5% headroom.

**Prerequisites, all independently useful:**

1. **Assoc keys must not hash addresses.** Done: a key encodes the object's
   identity number (§3.1), so `first` / `next` order is the order in which objects
   were first numbered, which is the same in every run — address order is not.
2. **`ref` into a class property.** Either pin for the call (§5.3, cheap, chosen)
   or pass base + offset and re-derive. The same treatment covers a container
   epoch pointer that aims into a class body.
3. **`rt_ref_names`** must never be extended to class-owned containers while it is
   address-keyed (`runtime/Reports.cpp`).
4. **`data-model.md` §8 and `:165-167`** must be amended in the same change:
   "class bodies never move and are never freed" becomes a statement about pinned
   and un-pinned objects.

**What it buys, honestly.** Two things that mark-region alone does not: compaction
(fragmentation across a long run with heterogeneous object sizes) and locality
(co-locating survivors). What it does *not* buy is allocation throughput — that
comes from bump allocation, which Immix already has without moving anything.

**The V8 comparison, taken seriously.** V8 keeps a copying young generation and,
where conservative roots intrude, pins rather than abandoning evacuation
(`PinObjectsConservative`, `scavenger_conservative_object_pinning`, currently
default-off/experimental for the JS heap, shipped conservative for Oilpan). Its
*non-moving* young generation, MinorMS, was built to remove the
conservative-scanning conflict and never launched — "Mark minor-ms as experimental
— There's no current plan in launching this GC" (V8 commit `51879489d8`,
2026-06-01). So the honest reading of V8 is that it supports the user's position:
a mature engine with this exact churn profile chose moving and made conservative
roots work around it.

The counter-hypothesis specific to SV — that UVM nursery survival rates are high
because a transaction lives from sequencer through driver and monitor into a
scoreboard, crossing many collections — is **mine, and unverified**. No V8
statement supports it (V8's own remark about large young generations concerns
MinorMS's page-iteration jank, not survival), and no public UVM allocation
measurement was found. It is therefore §11's first measurement, not an argument.

---

## 9. Lifetime semantics

**What keeps an object alive**, exhaustively: a handle in design state or a static
property; a handle anywhere in a live coroutine frame; a handle in a live element
of a traced container, including as an assoc key; a handle in a live class body; a
handle in a registered runtime staging image; a pending detached event trigger;
an outstanding `ref` or interior pointer. Nothing else.

**Reclamation is unobservable, so finalization is unconstrained.** SV has no
destructor and LRM 8.4 gives no `delete`, so there is no ordering requirement, no
resurrection question, and no finalizer-cycle problem — the failure that makes
bdwgc's `GC_register_finalizer` unusable for cyclic UVM graphs ("Cycles involving
one or more finalizable objects are never finalized", `docs/finalization.md`) does
not arise, because clotho's finalizer is not a user-visible hook. It runs at
sweep, must not allocate, and must not store a handle anywhere; both are
assertable.

**Process death.** A dead process's frame is freed and unlinked, so objects
referenced only by its locals die at the next collection — which is also why
`disable`'s raw-destroy paths must unlink (§5.1).

**Named event handles.** An event object is the second collected family: it has
metadata as any object has and a trace map of its own, which the runtime owns
because only the runtime allocates one (`event-handles.md` §3.2, §9). An event
handle is traced wherever a class handle is. Two things are particular to it:

1. A process suspended on `@(ev)` keeps `ev` alive through the waiter node in its
   frame, which records the object it is linked onto and is found by the
   conservative frame scan.
2. The runtime's own records of an event (a pending `->> e`, a `->> #d e` timer, a
   `->> @(x) e` subscription on a signal, a dumped event) hold a reference with no
   handle anywhere, and are roots until they are consumed.
3. What waits on an event object (the target of `->> @(c) e`, the value of
   `x <= @(c) v`) is reachable only while `c` is, and is dropped with it
   (`event-handles.md` §9).

**`std::weak_reference` (LRM 8.30)** is §7a.

**Unchanged**: `null` is all-zero; `==` is one `icmp`; the null-deref fatal path
(`emit_null_handle_check`) is untouched; `$cast` still reads field 0; the
subroutine ABI still passes a raw pointer; DPI cannot receive a class handle, so
there are no external roots.

---

## 10. Rejected alternatives

| Candidate | Fatal objection |
|---|---|
| **MMTk** | No Windows backend at all — zero `windows` matches in `src/`, none in `src/util/os/imp/mod.rs`. arm64 is Tier 3 ("no guarantee whether they will build or work"); `aarch64-apple-darwin` unlisted. 3 of 6 CI targets. No C API (`crate-type = ["rlib"]`), so a Rust binding crate implementing 7 traits is mandatory against a source API with a per-release migration guide. `mmtk_init()` reserves ~15 TiB of VA **[probe]**. Its *plans* are the right shape, which is why this design borrows their names and not their code. |
| **BDWGC** | Suspended coroutine frames are invisible to it: one stack region per registered thread, and a probe confirmed an object referenced only from a suspended fiber's stack was collected and its memory reused, 64/64 bytes clobbered **[probe]**. GC-heap frames make every suspended frame fully conservatively scanned and force `ALL_INTERIOR_POINTERS`; `GC_add_roots` hard-aborts past 2048 regions **[probe]**. 963/1000 handles held in a `std::vector` were collected while live **[probe]**. Precise marking and generational mode are mutually exclusive with the fast path (`docs/scale.md:105-107`, enforced at `thread_local_alloc.c:307`). Non-moving forever, so it forecloses §8. Mono evaluated it with this profile and shipped SGen. |
| **Ravenbrook MPS** | The strongest library evaluated: mostly-copying AMC, generational, incremental, BSD-2, one 294-line TU, six-instruction allocation **[probe]**, verified collecting and forwarding. Killed by three facts: `code/mpstd.h` has no clang/MinGW-w64 platform and hard-`#error`s under MSYS2 CLANG64; it takes process-global ownership of `SIGSEGV`, `SIGXCPU`, `SIGXFSZ` for hardware barriers, against clotho's DAP loop; and it has no model for roots in heap coroutine frames (one area root per live process, each a locked arena call, all rescanned per trace — O(live processes) in a workload defined by process churn). Dormant 21 months. |
| **LLVM statepoints** | Structurally cannot describe clotho's roots and silently miscompiles rather than failing (F2) **[probe]**. Experimental IR with no compatibility guarantee, non-integral-pointer datalayout constraints, one production user. The reusable piece is stack-map capture, ~5 lines on `RTDyldObjectLinkingLayer` — kept on the shelf. |
| **LLVM `gc.root` / ShadowStackGC** | Single global `llvm_gc_root_chain`, no per-task head, so it cannot be swapped per coroutine; documented "high overhead per function call". §5.1 reimplements the idea as a frame-resident node with no per-call push. |
| **Reference counting** | Cycles (§1). Removes the free-handle-store property everywhere including inside containers, needs the fifth `rt_elem_ops` lifecycle kind that class handles were admitted to avoid (`TODO.md`), and becomes atomic RMW traffic under MT. |
| **Per-process heaps (Erlang model)** | SV class handles are shared mutable references by construction — a driver holds the sequencer's item; the config DB holds handles every component reads. No ownership boundary to copy across. Relevant to note *because* MT is coming: the Erlang answer to parallel GC is not available here. |
| **Bump-allocate and reset per region** | Nothing in SV marks an object region-local; a handle escapes into a field, a container, a static, a mailbox. Viable later only where escape analysis proves a `new` cannot outlive its activation. |
| **sgcl, Pony ORCA, Mono SGen, ZGC/Shenandoah, `tgc`/`mkirchner-gc`** | Not embeddable, not separable from their host runtime, or toys. `sgcl` additionally maps stack addresses by `address % 8 MiB`, aliasing across multiple stacks. |

**What is being borrowed.** Immix's heap organisation and defragmentation policy
(Blackburn & McKinley, PLDI 2008); Bartlett's pinning-for-ambiguous-roots result
(DEC WRL 88/2, 1988) as productised in MPS's AMC, SBCL's gencgc and CRuby's
compactor; Julia's `gc-stock.c` for pool mechanics, page-level young/old
metadata and quick-versus-full sweeps; V8's pin-and-quarantine handling of
conservative roots under an evacuating young generation. All blueprints, no
dependencies. Scala Native's production Immix is the existence proof that
mark-region with no evacuation path at all is a shippable configuration.

---

## 11. Performance: what to measure

**The first number to get is nursery survival rate**, because it decides §8: bytes
surviving a young collection over bytes allocated since the previous one, per
benchmark shape. High survival favours in-place StickyImmix; low survival is the
case for a copying nursery (`GenImmix`-shaped) and makes evacuation worth its
prerequisites. Everything in §8 is written so that this can be answered with
measurements from P3 rather than argued now.

**Also to collect:** fragmentation (hole-count histograms, which Immix computes
anyway), mark time versus sweep time, false-retention delta from the §13
cross-check, allocation throughput, and pause distribution at region boundaries.

**Intuitions, labelled as such.** Per bus transfer a UVM environment allocates a
sequence item, one or two clones (`copy`/`clone` in the driver and monitor path), a
transaction record, and several strings (`$sformatf` in `uvm_info`,
`get_full_name`, config-DB keys). Against that, the component tree, factory,
config DB, coverage model and objections live for the whole run and are mutually
referencing. Generational is clearly right; *which* generational is empirical. No
public measurement of UVM allocation rate or live-set size was found.

**The perf gate must not be gamed.** `tests/perf/README.md` makes any `sim_ms`
regression a merge blocker, and picorv32, hazard3 and gate_rca allocate no class
objects at all — they must be exactly flat. The card-marking barrier is emitted
only at class-handle stores into heap objects; the allocation fast path replaces
an out-of-line call; no per-call, per-suspend or per-region cost is added. Under
single-mutator builds the mutator context pointer folds to a constant.

**The gate's blind spot must be closed first.** The harness records only
wall/compile/sim milliseconds. `FinishStats { peak_rss_kb, cpu_seconds }`
(`runtime/Scheduler.cpp`) is already computed for `$finish(2)` and every perf case
runs to `$finish`, so `perf.py` can gate a `peak_rss_kb` metric with no new
instrumentation.

**New shapes**, generated by extending `bench-sched/gen.py` (already parameterised
by process and cycle count) and promoted into `tests/perf/cases/` once worth
gating: `churn` (N allocated per cycle, M live — throughput and young-collection
cost); `tree` (large cyclic component graph plus churn on top — is the stable set
being re-marked?); `latency` (objects held across K cycles before release — the
survival-rate sweep, and the shape that decides §8); `strings` (`$sformatf` into
class properties — finalizers); `handles-in-containers` (queues and assoc arrays
of handles — §3.3 and card marking). Credible numbers come from `cloud/run.sh` on
quiet baremetal.

---

## 12. Phases

Each phase is shippable and leaves no half-state. Gaps are loud-guarded per
`PRINCIPLES.md`. P0-P4 are the collector; the two tracks after it are independent and
either may come first.

**P0 — object model and heap, no collection. (landed)** Immix blocks, lines, block
metadata, object-start bitmap; GC word at −8 with forwarding, pin and lazy-id
encodings defined; trace map and per-class finalizer emitted but unused; `chandle`
distinction threaded into layout metadata; `clotho.gcroots` emitted; frame registry
node linked and unlinked; allocation state addressed through a mutator context
pointer. The collector was still opt-in at this point (§12a).
*Acceptance, met:* byte-identical stdout on the whole suite (1980/1980); the off mode
emits no GC artifact at all, so `sim_ms` is flat by construction rather than by
measurement; registry invariants assert clean. Not yet measured: the peak-RSS fall
from 64→16-byte object alignment, because `perf.py` has no memory metric (§11).


**P1 — stop-the-world mark-region collection. (landed)** At the drain-loop region
boundaries; precise globals and heap, conservative validated pinned frames;
finalizers at sweep; lazy line sweep; teardown sweep at `$finish`. Requires
cooperative `$finish` (§6.3).
*Acceptance, met:* a churn design reaches a steady live set (202 objects held out
of 10,201 allocated) instead of growing linearly; the class suite passes under
both cadences; both frame-root shapes retain against a filler.
*Not met, deliberately deferred:* cooperative `$finish` has not landed, so a
`$finish` out of live frames still abandons them — with the collector that is
retention, not corruption, because an abandoned frame is never freed and so is
never a dangling registry entry. The `detect_leaks=0` overrides in
`tests/data_model_test.cpp` and `tests/dynamic_array_test.cpp` therefore stay until
`$finish` is cooperative; the class-object half of their justification is now
obsolete once objects are collected.

**P2 — inlined bump allocation. (landed)** The fast path is emitted inline against
the mutator context: bump, object-start bit, GC word, zeroed body, with a cold call
to `rt_gc_alloc_slow` only when the hole is exhausted. Frame link/unlink is inlined
pointer surgery too, since `frames_head` moved into the mutator. The finalizable set
became a per-block bitmap, because the allocation-time `std::vector` push was the one
thing an inlined path could not do — and restricting inlining to classes without
finalizers would have excluded almost every UVM object.
*Acceptance, met:* 4M objects at **96 ms against the uncollected path's 196 ms**, and 1.5×
over the out-of-line pool allocator it replaced.

**P3 — generational (StickyImmix). (landed)** Sticky mark bits, an inlined write
barrier over object remembering (§7), a remembered set, per-line age records, and
young-or-full collection kinds with `--gc-full-every=N` over a heap-growth default.
The default was left off here; §12a records what happened instead, which is that
the flag was removed outright once the measurements were in.
*Acceptance, met:* young-collection cost is independent of stable-set size — 20,000
live nodes, and the last young collection reports **1 live object**; 3.3× to 5.9×
against forcing every collection full, and nearly flat as the collection count
triples. Two use-after-free bugs found here and fixed; see §7.1.

**P4 — containers and aggregates of handles. (landed)** `rt_elem_ops` gained a
handle-bearing description and the container walkers; the blanket `--gc` refusal is
gone, and so is the narrow guard that briefly replaced it — an associative-array key
*containing* a class handle works too, because the key description became an offset
table rather than a flag bit (§3.3a). Container roots are registered by storage class
per
§3.3, and the class trace map now emits its leaf table for real, which is how a
container inside an object is traced.
*Acceptance, met:* a UVM-shaped tree — `Child children[string]`, each child holding a
parent handle — keeps exactly its 45 live components while 4,000 transactions churn
through the heap, and the dropped-subtree case reclaims. **A real UVM testbench can
now run collected**, which was this phase's whole point.

The rest of the family has since landed: `std::mailbox`, `semaphore` and `process`
as ordinary traced class bodies, event objects (`event-handles.md` §9), and
`std::weak_reference` (§7a).

**Track A — evacuation.** Gated on P3's fragmentation and survival numbers, not on
taste. Prerequisites in §8: pin-on-`ref`, `rt_ref_names` discipline, `data-model.md`
amendment (identity numbers in keys are done). Deliverable is a policy
switch (`--gc-defrag=off|opportunistic`) plus, if the survival numbers argue for
it, a copying nursery over the same Immix mature space.

**Track B — multi-mutator.** Co-designed with the MT scheduler milestone, not
before it. Parallel marking over work-stealing deques (byte-granular metadata
means no atomics); per-mutator block ownership, card buffers and registry lists;
rendezvous at the region barrier the scheduler will already have; poll sites for
bounded-latency stopping (§6.2). The P0 commitments — indirect allocation state,
byte metadata, unconditional card store — are the parts that would be expensive to
retrofit, which is why they are in P0 and not here.


## 12a. Why there is no uncollected mode

Every phase above shipped behind `--gc[=off|on|stress]`, default off, and the
backend carried a complete second code path for the off case: `rt_alloc` instead of
the bump allocator, a class descriptor with no trace-map pointer, a zeroed
`rt_elem_ops` GC tail, and emit-nothing gates on the root tables, the safepoint, the
frame registry and `rt_gc_init`. That flag is gone. The collector always runs; only
its cadence and its budgets are on the CLI (`--gc-stress`, `--gc-stats`,
`--gc-nursery`, `--gc-heap-max`, `--gc-full-every`).

**The evidence that made it removable**, all measured on this branch: median **0.0%**
simulate delta across the 12 runnable cases of `tests/perf/cases` (min of 9,
interleaved; corpus total −1.7%, worst case +2.3%); allocation-heavy designs roughly
**2× faster** collected, because the fast path replaces `operator new`; a UVM-shaped
run **28% faster** at **44% lower peak RSS**; and, since P4 and §3.3a, nothing
refused that was previously accepted. There was no workload left that the flag
protected.

**What it cost.** Two things, and neither should be discovered by surprise later:

- **The strongest test oracle.** "Byte-identical with the collector off" compared
  against a reference in which nothing can be freed early. §13.1 states what
  replaced it: a cadence at which the collector never runs, on the one code path.
- **The escape hatch.** A collector bug can no longer be worked around from the
  command line. That is a deliberate trade for one code path instead of two — a
  dual path that is exercised by every test is a real maintenance asset, and one
  that is exercised only by the flag's own test is a liability, which is what this
  one had become.

Removing it also deleted `rt_gc_enabled()` from the runtime ABI: its only caller was
the test that proved the off mode collected nothing.

---

## 13. Verification

**There is no differential oracle.** LRM 8.4 makes collection unobservable, so
iverilog cannot check it — per `PRINCIPLES.md`, the substitute strategy is stated, and
it is four things:

1. **Cadence identity.** Every design in `tests/gc_test.cpp` prints the same bytes
   at three cadences: never (a budget no run can spend, so no collection runs),
   ordinary (a collection when the budget is spent) and `--gc-stress` (one at every
   safepoint). A collector that changes observable behaviour is a bug by
   definition.

   The never cadence is the reference, because nothing in it can be freed early.
   It is what "byte-identical with the collector *off*" was before §12a, without a
   second code path: the same generated code and the same runtime, with the one
   number that paces collections out of reach. Without it the comparison showed
   only that two collection schedules agree, and a wrong answer both produce
   passed. Stress is the cadence that bites: a missing root is sensitive to *when*
   collection happens, and stress collects whenever it can.

   The comparison is on what a design prints, so it is as strong as the case.
   The rest of the suite runs at the ordinary cadence only.

   Why the reachability cases are built the way they are:
   each runs a filler process that recycles and poisons freed granules, so a
   wrongly-collected object reads back as `0x5A5A5A5A` instead of its own
   stale-but-intact bytes — a value check needing no reference run. Several also
   give the object under test a `string` property, so the same mistake becomes a
   heap-use-after-free under ASan rather than a text diff. Both devices measure
   against an absolute expectation rather than a differential one.
2. **LeakSanitizer with the overrides removed.** The house method for lifetime
   work (`fork-join.md:495-497`: "a missed release leaks and a double release is a
   double free, and neither shows up in a value check"). Concrete criterion:
   delete `ASAN_OPTIONS=detect_leaks=0` from `tests/data_model_test.cpp:701-705`
   and `tests/dynamic_array_test.cpp:524,578`.
3. **Bounded peak RSS** on the new shapes, gated in `perf.py` via `$finish(2)`.
4. **`sim_ms` flat** on the existing 15 cases through `cloud/run.sh`.

**Four debug modes carry most of the correctness load:**

- `--gc-stress` — collect at every region boundary, and at every allocation under
  `-DCLOTHO_SANITIZE=ON`. Turns a latent missing root into a deterministic
  failure. Under evacuation, also *move every movable object at every collection*,
  which is the standard way to shake out stale-pointer bugs.
- **Heap verify** — after a collection, walk every block through its metadata
  bytes and assert that they parse as free granules and whole objects, that each
  object's descriptor is a known `clotho.classdesc`, its extent matches its type,
  its `FINALIZE` flag matches its class, every handle field points at an
  object, and no unlogged old object holds a young one. Pair-assertion discipline
  (TigerStyle) applied to the heap. It walks the whole heap after every collection,
  2.3x slower on an allocation-heavy design, so it is built only with
  `CLOTHO_EXTRA_ASSERTS` (Debug and CI); shipped builds keep the per-object
  assertions and leave out the walk.
- **Root cross-check** — scan frames precisely *and* conservatively, diff the live
  sets. The conservative set must be a superset; anything live precisely but dead
  conservatively is a bug in validation, and the size of the difference is §5.2's
  false-retention number. This is also what would qualify a precise frame map if
  Track A ever wants one.
- **Pin audit** — assert that no pinned object moved, and that every pin is
  released (collection-scoped pins at collection end, `ref` pins at return).

**TigerStyle obligations**, called out because a collector is where they bite:
the mark phase is an explicit worklist, never recursion; the worklist has a
documented growth policy and an assertion; header size, alignment, line
and block size relationships are asserted as compile-time constants; and every
collector function carries its two assertions, with validate-on-allocate plus
validate-on-trace as the canonical pair.

---

## 14. Deferred, each loud-guarded

- **Nothing is refused that was accepted before the collector existed.** Every guard this work
  introduced has since been lifted: class handles as container elements and aggregate
  leaves (P4, §3.3), and an associative-array key containing a class handle (§3.3a).
- Event handles are traced like class handles (`types::is_object_handle`), in every
  place one can sit, a variable-length associative key included (§3.3a).
- Virtual interfaces (`src/lower/`) — unrelated to GC, but another
  handle-shaped family; whether it becomes GC-managed is that design's question.
- Compaction of the large-object space; user-visible pinning; `%p` of a handle
  (which under Track A prints an id, not an address).

## 15. Open questions

1. **Cooperative `$finish` first?** §6.3 argues it is a hard prerequisite. It is
   already the highest-value follow-up in `CI.md`, so sequencing it before P1
   costs nothing and removes the registry-corruption risk entirely.
2. **Is a hand-written collector accepted?** `PRINCIPLES.md` asks a new dependency for a
   strong justification; this asks for confirmation of *no* dependency on
   §10's evidence. The counter-proposal worth naming is MPS plus an MSYS2 CLANG64
   port and a signal-ownership negotiation with the DAP loop, which buys
   mostly-copying compaction sooner.
3. **How much MT scaffolding lands in P0?** This document commits to the three
   things that are expensive to retrofit and defers the rest to Track B. If the MT
   scheduler design is closer than that implies, per-mutator block ownership and
   card buffers should move into P0 as well.
4. **Does the MT design want native TLS?** If so, RTDyld's TLS support becomes a
   blocker and JITLink migration (`Jit.cpp:137-148`) joins the critical path. The
   allocator does not care, but the scheduler might.
5. **Precise frame roots, ever?** §5.2 chooses conservative-plus-pinning for
   soundness, and §8 shows evacuation does not require precision. The only reason
   to build precise frame maps is if the cross-check shows material false
   retention.
6. **Where does the emergency-collection hard cap sit**, and is crossing it
   reported (`--gc-stats`) or silent?
7. **Amend or supersede.** `classes.md` §7 now cross-references this document; the
   class-body row of `data-model.md` §8 and the `:165-167` "class bodies never
   move" sentence must be rewritten in whichever change first makes them false, and
   `TODO.md` M4 needs a tracked GC item — it has none.
