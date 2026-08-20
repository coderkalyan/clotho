# Garbage collection (LRM 8.4)

Status: **P0-P4 landed behind a non-default `--gc`. Both tracks (evacuation,
multi-mutator) remain the proposal (§12).** What runs today: the Immix-shaped heap
with an inlined bump-allocation fast path, the object model, precise global roots,
precise container roots, conservative pinned coroutine-frame roots, per-class trace
maps and finalizers, generational collection in place with an inlined write
barrier, and stop-the-world young or full mark-region collection at the scheduler's
region boundary. `--gc=off` is the default and emits no collector at all.

Landed evidence, all [in-tree]:

- 2029/2029 of the suite passes, including 43 cases in `tests/gc_test.cpp` (723
  assertions), which also pass under ASan+UBSan with no memory error.
- **Allocation is 2.0× faster than the leak-everything path**: 4M short-lived
  objects, `--gc=off` 196 ms of simulate time against `--gc=on` **96 ms**. `--gc=off`
  calls `rt_alloc` (64-byte-aligned `operator new`) per object; `--gc=on` bumps a
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
- `--gc=off` emits zero GC artifacts: the class descriptor keeps its original
  `{ [16 x i32], [N x ptr] }` shape and no `rt_gc_*` symbol appears in the module,
  so the `sim_ms` gate is met by construction rather than by measurement.
- Byte-identical stdout across `off` / `on` / `stress` on every case.
- A 10,201-object churn design under `--gc=stress`: 201 collections, live set
  settles at 202 objects / 12,928 bytes, 9,999 objects finalized during the run
  (their `string` buffers freed — the second-order leak in §0), heap 128 blocks
  with 127 free at exit.
- The conservative frame path is exercised, not assumed: an object held only by an
  `always` block's `automatic` local across a suspension, and one held only by a
  blocking task's local across `#3`, both survive 40 stress collections against a
  10,000-object filler, with 29 conservative pins recorded. `tests/gc_test.cpp`
  asserts `conservative_pins > 0` on that case so it cannot silently decay into a
  precise-root test (see §5.4 for why that trap is easy to fall into).
- LeakSanitizer, the house method for lifetime work, on the churn design: `--gc=off`
  reports **571,256 bytes leaked in 10,201 allocations** — the class objects and
  their `string` buffers, exactly §0's two leaks. `--gc=on` reports **no leak at
  all**. On a design that also calls `$finish`, `--gc=off` leaks 10,024
  allocations and `--gc=stress` leaks **4**: the collector removes 10,020 of them
  and the residue is the pre-existing `$finish`-abandons-live-frames leak
  (`cooperative-finish.md:24-28`), not a collector bug.
- ASan and UBSan report no memory error on any case under `--gc=stress`
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
  adding the unused class field alone, with `--gc=off` on both sides, moves the
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

`new` allocates and nothing frees. `LlvmBackend.cpp:11681-11698` says so at the
site:

> Lifetime is leaked in V1 (LRM 8.4 objects are GC'd; a program cannot observe a
> free) — see docs/design/classes.md §7.

That was the right call for a base — LRM 8.4 gives no `delete` for a class handle
and a program cannot observe reclamation, so leaking is *observably correct*. It
stops being correct enough at M4 (`TODO.md:20`, "Run a real UVM testbench"),
because a UVM run allocates a transaction object per bus transfer and never
stops.

The leak is also second-order: because a class body is never freed, every
`string`, dynamic array, queue and associative array **inside** an object leaks
its own heap too (`emit_class_body_init`, `LlvmBackend.cpp:1847-1861`, has no
matching `emit_agg_deinit` call anywhere).

**In scope.** Class bodies; the heap they own through managed properties; the
`rt_event` sync objects that named-event handles will need
(`named-events.md:208-271`); `std::mailbox` / `std::semaphore` /
`std::weak_reference` when they land (`TODO.md:1439-1457`).

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
(`Lower.cpp:2123-2128`); clotho never allocates it and must never trace or
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

- **Immix, not size-class pools.** Mark-region: 32 KiB blocks divided into 128 B
  lines, bump allocation within a block, line-granularity reclamation
  (Blackburn & McKinley, PLDI 2008). It is chosen because it is the one heap
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

- **One extra header word, at offset −8.** Below the descriptor pointer, so
  `class_header_words()` stays 1, every property GEP index stays `1 + declIndex`,
  and `$cast`'s negative-offset display arithmetic (`LlvmBackend.cpp:11722-11745`)
  is untouched. This is `jl_taggedvalue_t`'s placement for the same reason.

- **Finalize; do not re-own.** A collected object runs a per-class finalizer that
  is exactly the existing `emit_agg_deinit` walk. Managed buffers keep their
  current owner-frees discipline.

---

## 2. The facts that decide the design

**F1 — the scheduler has real quiescent points today, and will still have them
under MT.** One sim thread runs JIT'd `main` → `rt_sim_run` → `sim_drain`; the
whole event loop is emitted (`emit_active_run`, `LlvmBackend.cpp:8698-9340`), and
at its ring-empty edges — `active.check` with `head == tail`, `wheel.check`, and
immediately after `rt_wheel_advance` returns — the native stack is exactly
`main → rt_sim_run → sim_drain` and every process is suspended in its frame. The
property that survives multi-threading is not "one thread" but "regions are
globally ordered": NBA commit, the Observed/Reactive split and delta-cycle
ordering are defined across the whole design, so region boundaries are rendezvous
points in any correct parallel scheduler.

**F2 — a suspended process has no stack.** Processes and blocking tasks are LLVM
switched-resume coroutines (`setPresplitCoroutine` at `LlvmBackend.cpp:3131`,
`:3597`); the frame is `rt_alloc(coro.size)` (`:3168-3180`) and CoroSplit chooses
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
pointer, stored at `new` (`LlvmBackend.cpp:11696`); the descriptor
(`clotho.classdesc.<Ref>`, `:1765-1802`) is a private constant with the ancestor
display at negative offsets from the vptr and the vtable at positive ones. Precise
tracing costs one load plus one indirect table read. `class_body_ty` already knows
exact byte offsets via `getStructLayout()`, and `collect_elem_leaves`
(`:1419-1424`) already emits an `{offset, descriptor}` table.

**F4 — handles are bare, aligned, untagged 64-bit words.** `ClassNew` yields
`ptrtoint` of the body (`:11697`); `HandleDeref` does `inttoptr` (`:11700-11714`);
`null` is all-zero; `==` is `icmp eq i64`. No tagging anywhere in the tree. A
naturally-aligned 64-bit store is atomic on every target in the matrix, so even
under MT the collector never observes a torn handle. Bodies are 64-byte aligned
today only because `rt_alloc` is the coroutine-frame allocator
(`Runtime.cpp:5364-5376`, `frame_align{64}`) — 62% waste on a 24-byte transaction
object.

**F5 — the call ABI already carries a uniform extra pointer, and the reason is
documented.** `declare_function` (`LlvmBackend.cpp:3478-3491`) threads the RNG
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
| Assoc keys on class handles | `Lower.cpp:1929-1931`, `emit_key_bytes` `LlvmBackend.cpp:13766-13795`, `Runtime.cpp:2058-2065,2124` | the handle's raw bits are big-endian-encoded into the `std::string` hash key *and* drive `first`/`next` ordering |
| `ref` into a class property | `LlvmBackend.cpp:16368-16383`, `:3495-3505` | passed as a raw interior pointer with `clotho.ref.noepoch`, a compile-time-zero epoch that can never signal invalidation |
| `rt_ref_names` | `Runtime.cpp:3791-3809` | address-keyed linear scan; safe today only because no entry aims at a class body |

---

## 3. Object model

### 3.1 The GC word

```
        handle - 8   ┌──────────────────────────┐
                     │ gcword : u64             │  state | pin | payload
        handle + 0   ├──────────────────────────┤
                     │ vptr   : ptr             │  → clotho.classdesc.<Ref>.slots[0]
        handle + 8   │ property 0 …             │
                     └──────────────────────────┘
```

Low bits: mark/age state (`CLEAN` / `MARKED` / `OLD` / `OLD_MARKED`), `PINNED`,
`HAS_ID`, `FORWARDED`. The 56-bit payload is one of three things, never two at
once:

- **nothing**, for the overwhelming majority of objects;
- **an object id**, assigned lazily the first time an object is used as an
  associative-array key or formatted (§8.1) — so the id costs nothing for objects
  that are never keyed;
- **a forwarding address**, during an evacuating collection. No conflict with the
  id: the id is copied with the object, and the original's word is dead after the
  collection.

The vptr must *not* be overwritten by forwarding, because object size is derived
from the type and is needed while sweeping. That is precisely why the GC word
exists as a separate word rather than as stolen vptr bits, and it is a second
reason not to steal them (the first: a mask on every virtual dispatch and every
`$cast`).

`PINNED` is a bit, not a count, plus a side list of pinned objects for the current
collection — pins from conservative roots are collection-scoped and cleared at the
end. Pins with lifetime (an outstanding `ref`, §5.3) use a small count in the
payload's spare bits, asserted non-saturating.

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
(`LlvmBackend.cpp:1428-1487`) classifies a class-handle element as `rt_elem_pod`
(kind 0) with no leaf table, and `Runtime.h:871-873` says so outright — "A POD
element (plane scalar / real / packed vector / class handle) is pure bytes". A
container of handles therefore carries **zero** type information at runtime, so
nothing can trace through it.

Required: a handle-bearing element kind, handle offsets in the leaf table, and
`rt_dynarr_trace` / `rt_queue_trace` / `rt_assoc_trace` walkers. The queue walker
must respect ring wraparound (`base + ((head+i) % cap) * stride`); the assoc
walker must iterate the C++ `AssocMap`, because element payloads are one `SlotVec`
heap block per entry and `unordered_dense` relocates them on rehash
(`Runtime.cpp:2093-2110`). Handle-valued assoc **keys** are strong references and
must be traced from their encoded bytes.

`rt_elem_ops` is mirrored bit-for-bit by `elem_ops_ty()`
(`LlvmBackend.cpp:1297-1305`) under an explicit "must match the runtime struct
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
`Runtime.cpp:1049-1110`) — and `rt_gc_range_add` already establishes the pattern
for a runtime-owned region the collector must scan. The alternative, making
container payloads GC-allocated, was rejected: it would drag strings, dynamic
arrays and queues out of the ownership discipline that `data-model.md` §8 already
gets right for every non-class kind.

**This was the sharpest limitation of the landed work, and it is now closed.** For one
phase `--gc` refused a class handle inside a container outright, because the
alternative is silently freeing an object whose only reference is a container element,
and that has to be a located failure rather than a quiet
fallback. Worth recording why the refusal existed at all: an earlier draft of this
document asserted the shape was already loud-guarded in the frontend, citing `TODO.md`.
It was not — `C q[$]; q.push_back(a);` compiles and runs under `--gc=off` [in-tree],
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

`rt_elem_ops` carries `nkeyhandles` / `keyhandles` for that — an offset **list**, not a
flag. A flag can only say "the key *is* a handle", which covers `int m[Cell]` and
nothing else. An aggregate key is serialized leaf by leaf, depth first, in declaration
order, with `ceil(W/8)` bytes per leaf and no padding (`assoc-keys.md` §2), so a handle
can sit at any offset and a struct can carry several:

```systemverilog
typedef struct { byte b; Cell c; shortint s; } off_t;  // handle at byte 1
typedef struct { Cell a; int i; Cell b; }      two_t;  // handles at 0 and 12
int m[off_t];
int n[two_t];
```

Both work; a bare `int n[Cell]` is the one-entry case of the same table, `{ 0 }`, not a
separate path. Each offset names 8 big-endian bytes, and a handle is unsigned, so there
is no sign flip to undo.

**The offsets are serialized, not storage.** That is the one thing to get right here: a
`getStructLayout` shortcut would place the handle after the struct's padding and decode
the wrong bytes — passing every same-offset test and failing the moment a `byte` or a
`shortint` precedes the handle. The backend's `agg_key_handle_offsets` is deliberately
the third walk in the same three-arm shape as `agg_key_bytes` (which sizes the span) and
`emit_agg_key_bytes` (which writes it), which is what makes the offsets provably the
ones the serializer produces.

Verified with the objects reachable *only* through key bytes, against a filler that
recycles and poisons freed lines: a handle after a `byte`, and two handles in one key,
both survive 39 stress collections and decode back correctly through `m.first()`.

### 3.4 chandle disambiguation

After lowering, a `chandle` and a class handle are both a 64-bit two-state plane
(`InternPool.cpp:177-180`, `Lower.cpp:2123-2128`); the distinction survives only
in slang's AST. A "this plane slot is a GC handle" bit must be threaded from
`lower_class_type` into `FrameSlot` / `SignalSlot` / `rt_elem_ops`, or a precise
root table would trace foreign DPI pointers.

---

## 4. Heap

### 4.1 Blocks, lines, and metadata

- **32 KiB blocks, 128 B lines**, the paper's experimentally-selected sizes. Block
  metadata (line mark bytes, hole count, a `has_young` flag) is off-heap and
  block-aligned.
- **Line marks are bytes, not bits**, and so are object mark states (in the GC
  word). Parallel marking therefore needs no CAS and no atomics: two threads
  racing to mark the same line or object write the same value. This is the single
  cheapest MT accommodation available and it costs 0.8% of the heap.
- **An object-start bitmap**, one bit per 8 bytes (1.6%), written only by the
  owning mutator and block-aligned so no two mutators share a bitmap word. It
  serves four purposes: validating conservative candidate words, resolving
  interior pointers to object starts, heap parsability for the verify mode, and
  the sweep cursor.
- **Alignment 16, not 64.** The 64-byte guarantee exists for coroutine frame plane
  spills (`Runtime.cpp:5364-5371`) and is waste for objects. Frames keep
  `rt_alloc`; objects get the Immix allocator.
- **Large objects** (larger than a line, or larger than some fraction of a block)
  go to a separate large-object space with the same GC word, never evacuated.

### 4.2 Reclamation

Line-granularity: a collection marks lines, and reclamation threads free *holes*
(runs of unmarked lines) rather than individual cells, which is what makes sweep
cost independent of the number of dead objects. Blocks with no marked line are
recycled whole. Sweeping is lazy — a mutator sweeps a block when it acquires it.

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
; object-start bit, gcword, vptr stores follow
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
does not check (`LlvmBackend.cpp:11706-11712` elides null guards on a fresh
handle). The Immix allocator must preserve "never returns null" by taking the
existing located-fatal path (`alloc_bytes`, `Runtime.cpp:4104-4128`) after a
collection fails to satisfy the request.

---

## 5. The root set

| Roots | Mechanism | Precision |
|---|---|---|
| Design state: `@clotho.state`, `@clotho.inst.<hier>.state`, package variables, static class properties | Compile-time `clotho.gcroots` table of `{ ptr base, u32 nhandles, u32 offsets[] }`, emitted where `emit_main` already walks `slots_` (`LlvmBackend.cpp:5279-5291`); statics from `declare_static_prop` (`Lower.cpp:2229-2234`) | precise |
| Live coroutine frames (processes, blocking tasks, fork children) | Intrusive `{ prev, next, u64 size }` node in the frame, linked at the ramp, unlinked at frame free | conservative, validated, **pinning** |
| Managed containers reachable from the above | `rt_elem_ops` walkers (§3.3) | precise |
| NBA / intra staging images (`rt_nba_arr_slot`, `rt_intra_alloc`), `$strobe` / `$monitor` argument images, deferred-report queues | Registered with their element descriptor | precise |
| Detached `->> #d e` timer node (`LlvmBackend.cpp:12646`), later `rt_event` waiter lists | Explicit root registration at creation | precise |
| Wheel timers, wait buckets, the active ring, fork/disable records | Nothing to do — they hold *frame* pointers, and frames are enumerated above | — |
| VCD var tables, `rt_ref_names`, `chandle` | **Not roots.** VCD caches only pointers into `clotho.state`; `chandle` is foreign | — |

### 5.1 Frame registration

`build_frame_layout` (`LlvmBackend.cpp:3714-3786`) already places all SV locals in
a single `frame_ty_` struct alloca positioned after `llvm.coro.begin`, which is
why the registry node can be an alloca too: CoroSplit hoists it into the frame and
rewrites uses to a stable frame-interior address **[probe]**. The node records the
frame base and `llvm.coro.size`, both already materialised at `:3168-3180` for the
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
candidate word is validated against our own metadata: it must land in a live
block, be 16-aligned, and have its object-start bit set. The same lookup resolves
an *interior* pointer to its object start, which is what makes
`ref`-into-a-class-property (F6) work with no epoch machinery at all.

Costs, both bounded and both measurable: scan time proportional to live-frame
bytes once per collection, and retention of one object graph per stale frame slot
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

---

## 6. Safepoints

### 6.1 Today: single mutator

Collection runs at the drain-loop quiescent edges (F1) when the allocation budget
is exceeded — a nursery budget in bytes plus a heap-size threshold, both
benchmarked (§11), with `--gc=off` reproducing today's behaviour and `--gc=stress`
collecting at every safepoint. No polls are emitted in generated code.

**The unsuspended-allocation hole.** A process may allocate without bound between
suspensions; `for (int i = 0; i < 1_000_000; i++) obj = new C();` is legal and
reaches no region boundary. The allocator's block-acquisition path sets a request
flag and continues; the heap grows to the next boundary. If a hard cap is crossed
first, an **emergency collection** runs at the allocation site, with the sim
thread's native stack (base recorded at `rt_sim_run` entry; current extent plus
callee-saved registers captured with `setjmp`) scanned conservatively in addition
to the registry, and everything it finds pinned. Failing loudly instead was
rejected: the loop is legal SV.

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
  trace from roots plus the remembered set and stop at `OLD_MARKED` objects. The
  paper introduces this variant explicitly "to support pinning", which is exactly
  the property §5.3 needs. Julia's MMTk configuration defaults to `StickyImmix`
  and Ruby's to Immix with a non-moving nursery; mmtk-core's own default is
  `GenImmix`, i.e. a copying nursery over an Immix mature space — all three are
  reachable from this heap organisation without redesign.
- **Object remembering, not card marking.** This document specified an unconditional
  card store; the implementation is Julia-shaped object remembering, and the change
  was right. A handle store into a heap object tests the *owner's* header inline —
  `(gcword & (OLD|LOGGED)) == OLD` — and calls `rt_gc_remember` only on the taken
  edge, which sets `LOGGED` so a hot object costs the call once per cycle rather
  than once per store. Five IR instructions, cold-weighted, the same idiom as the
  safepoint poll and the null-handle check.

  The reason to prefer it here is the one the card-marking argument got backwards. A
  container's payload is not a GC object, so a card keyed on the *store's address*
  marks a card that no collection scans — the mutated container needs its OWNER
  remembered, and the owner is exactly what the object-header test already has in
  hand. `Runtime.h:874-877`'s licence to relocate elements by raw `memcpy` then
  needs no instrumenting either, for a better reason than a range mark: the owner is
  remembered, and tracing the owner walks the container wherever its elements have
  moved to.
- **No barrier for design state, statics or frame locals.** All scanned in full at
  every collection. In the implementation this is structural rather than
  enumerated: the barrier is emitted from an lvalue field set only by `HandleDeref`,
  so an lvalue rooted in a signal or a frame slot cannot carry an owner to remember.
- **Per-line age records** (`line_old`) let reclamation protect a line holding an old
  survivor, re-derived at every full collection. That is what makes a young
  collection able to skip the stable population without freeing it.

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

---

## 8. Evacuation: what it costs and what it buys

Immix's opportunistic defragmentation picks candidate blocks from hole-count
histograms before a collection, then in the single mark pass "if the object is
unmarked and movable, it evacuates the object, installs a forwarding pointer …
If an object is pinned by the application or is on a target page, immix simply
marks the object, leaving it in place", stopping when the target space is
exhausted. It is a policy knob over the same heap, with 2.5% headroom.

**Prerequisites, all independently useful:**

1. **Assoc keys must not hash addresses.** Encode the lazily-assigned object id
   (§3.1) at `emit_key_bytes` instead of the handle bits. Side benefit: `first` /
   `next` ordering becomes allocation order, which is deterministic across runs —
   address order is not.
2. **`ref` into a class property.** Either pin for the call (§5.3, cheap, chosen)
   or pass base + offset and re-derive. The same treatment covers a container
   epoch pointer that aims into a class body.
3. **`rt_ref_names`** must never be extended to class-owned containers while it is
   address-keyed (`Runtime.cpp:3791-3809`).
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

**Named event handles.** `named-events.md:208-271` turns an event into a heap sync
object reached through a handle; it becomes the second GC-managed family and
should use this document's GC word and trace map. Two requirements follow:

1. A process suspended on `@(ev)` must keep `ev` alive. Wait buckets store
   coroutine handles, not event handles, so the event handle must be reachable
   from the waiting frame — which it is if it is a frame local, and Option A must
   not drop it as a dead SSA value at the subscribe site (the mistake
   `fork-join.md:466-470` records for child process handles).
2. A pending `->> #d e` timer node holds an event reference with no handle anywhere
   (`LlvmBackend.cpp:12646`) and must be an explicit root until it fires.

**`std::weak_reference` (LRM 8.30)** becomes implementable: a weak table cleared
after marking, before sweeping. `TODO.md:1403-1404` already notes it presupposes a
collector.

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
| **Reference counting** | Cycles (§1). Removes the free-handle-store property everywhere including inside containers, needs the fifth `rt_elem_ops` lifecycle kind that class handles were admitted to avoid (`TODO.md:816-818`), and becomes atomic RMW traffic under MT. |
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
(`Runtime.cpp:3273-3311`) is already computed for `$finish(2)` and every perf case
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

Each phase is shippable and leaves no half-state. Gaps are loud-guarded.
P0-P4 are the collector; the two tracks after it are independent and
either may come first.

**P0 — object model and heap, no collection. (landed)** Immix blocks, lines, block
metadata, object-start bitmap; GC word at −8 with forwarding, pin and lazy-id
encodings defined; trace map and per-class finalizer emitted but unused; `chandle`
distinction threaded into layout metadata; `clotho.gcroots` emitted; frame registry
node linked and unlinked; allocation state addressed through a mutator context
pointer. Default `--gc=off` behaves exactly as today.
*Acceptance, met:* byte-identical stdout on the whole suite (1980/1980); `--gc=off`
emits no GC artifact at all, so `sim_ms` is flat by construction rather than by
measurement; registry invariants assert clean. Not yet measured: the peak-RSS fall
from 64→16-byte object alignment, because `perf.py` has no memory metric (§11).


**P1 — stop-the-world mark-region collection. (landed)** At the drain-loop region
boundaries; precise globals and heap, conservative validated pinned frames;
finalizers at sweep; lazy line sweep; teardown sweep at `$finish`. Requires
cooperative `$finish` (§6.3).
*Acceptance, met:* a churn design reaches a steady live set (202 objects held out
of 10,201 allocated) instead of growing linearly; the class suite passes under
`--gc=on` and `--gc=stress`; both frame-root shapes retain against a filler.
*Not met, deliberately deferred:* cooperative `$finish` has not landed, so a
`$finish` out of live frames still abandons them — with the collector that is
retention, not corruption, because an abandoned frame is never freed and so is
never a dangling registry entry. The `detect_leaks=0` overrides in
`tests/data_model_test.cpp` and `tests/dynamic_array_test.cpp` therefore stay until
`$finish` is cooperative; the class-object half of their justification is now
obsolete under `--gc=on`.

**P2 — inlined bump allocation. (landed)** The fast path is emitted inline against
the mutator context: bump, object-start bit, GC word, zeroed body, with a cold call
to `rt_gc_alloc_slow` only when the hole is exhausted. Frame link/unlink is inlined
pointer surgery too, since `frames_head` moved into the mutator. The finalizable set
became a per-block bitmap, because the allocation-time `std::vector` push was the one
thing an inlined path could not do — and restricting inlining to classes without
finalizers would have excluded almost every UVM object.
*Acceptance, met:* 4M objects at **96 ms against `--gc=off`'s 196 ms**, and 1.5×
over the out-of-line pool allocator it replaced.

**P3 — generational (StickyImmix). (landed)** Sticky mark bits, an inlined write
barrier over object remembering (§7), a remembered set, per-line age records, and
young-or-full collection kinds with `--gc-full-every=N` over a heap-growth default.
The default stays `--gc=off`: flipping it is a user-facing decision that should
follow a real UVM run, not precede it.
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

The rest of the family — `rt_event` sync objects, `std::mailbox` / `semaphore` /
`process`, `std::weak_reference` — is blocked on those features existing at all,
not on the collector. Each should adopt this document's header and trace map when
it lands.

**Track A — evacuation.** Gated on P3's fragmentation and survival numbers, not on
taste. Prerequisites in §8: lazy object ids at `emit_key_bytes`, pin-on-`ref`,
`rt_ref_names` discipline, `data-model.md` amendment. Deliverable is a policy
switch (`--gc-defrag=off|opportunistic`) plus, if the survival numbers argue for
it, a copying nursery over the same Immix mature space.

**Track B — multi-mutator.** Co-designed with the MT scheduler milestone, not
before it. Parallel marking over work-stealing deques (byte-granular metadata
means no atomics); per-mutator block ownership, card buffers and registry lists;
rendezvous at the region barrier the scheduler will already have; poll sites for
bounded-latency stopping (§6.2). The P0 commitments — indirect allocation state,
byte metadata, unconditional card store — are the parts that would be expensive to
retrofit, which is why they are in P0 and not here.

---

## 13. Verification

**There is no differential oracle.** LRM 8.4 makes collection unobservable, so
iverilog cannot check it — the substitute strategy is stated, and
it is four things:

1. **Stdout identity.** Every existing test byte-identical under `--gc=off`,
   `--gc=on`, `--gc=stress`, and later `--gc-defrag=opportunistic`. A collector
   that changes observable behaviour is a bug by definition; a *moving* collector
   that changes observable behaviour is the bug this catches.
2. **LeakSanitizer with the overrides removed.** The house method for lifetime
   work (`fork-join.md:495-497`: "a missed release leaks and a double release is a
   double free, and neither shows up in a value check"). Concrete criterion:
   delete `ASAN_OPTIONS=detect_leaks=0` from `tests/data_model_test.cpp:701-705`
   and `tests/dynamic_array_test.cpp:524,578`.
3. **Bounded peak RSS** on the new shapes, gated in `perf.py` via `$finish(2)`.
4. **`sim_ms` flat** on the existing 15 cases through `cloud/run.sh`.

**Four debug modes carry most of the correctness load:**

- `--gc=stress` — collect at every region boundary, and at every allocation under
  `-DCLOTHO_SANITIZE=ON`. Turns a latent missing root into a deterministic
  failure. Under evacuation, also *move every movable object at every collection*,
  which is the standard way to shake out stale-pointer bugs.
- **Heap verify** — after marking, walk every block via the object-start bitmap
  and assert each live object's descriptor is a known `clotho.classdesc`, its size
  matches its type, its line marks are consistent, and every handle field points at
  an allocated object start. Pair-assertion discipline
  (TigerStyle) applied to the heap.
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

- **Nothing is refused under `--gc` that `--gc=off` accepts.** Every guard this work
  introduced has since been lifted: class handles as container elements and aggregate
  leaves (P4, §3.3), and an associative-array key containing a class handle (§3.3a).
- Event handles, event arrays, events as class properties — already guarded
  (`Lower.cpp:2241`); guard stays until P4, and `named-events.md` Option A should
  adopt this document's header and trace map rather than inventing a second.
- `std::mailbox`, `std::semaphore`, `process`, `std::weak_reference` — already loud
  (`Lower.cpp:2160-2174`).
- Virtual interfaces (`Lower.cpp:2138`) — unrelated to GC, but another
  handle-shaped family; whether it becomes GC-managed is that design's question.
- Compaction of the large-object space; user-visible pinning; `%p` of a handle
  (which under Track A prints an id, not an address).

## 15. Open questions

1. **Cooperative `$finish` first?** §6.3 argues it is a hard prerequisite. It is
   already the highest-value follow-up in `CI.md`, so sequencing it before P1
   costs nothing and removes the registry-corruption risk entirely.
2. **Is a hand-written collector accepted?** The project requires explicit
   confirmation for a dependency; this asks for confirmation of *no* dependency on
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
