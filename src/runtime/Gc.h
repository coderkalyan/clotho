// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Garbage collection for SystemVerilog class objects (LRM 8.4).
//
// Design: docs/design/garbage-collection.md. An Immix-shaped mark-region heap
// (32 KiB blocks of 128 B lines), traced precisely through the per-class map the
// backend emits beside each class descriptor, rooted precisely in the design
// state and conservatively in live coroutine frames. Non-moving in this phase;
// the header reserves the forwarding and pin encodings so evacuation stays a
// policy change rather than a redesign.
//
// Everything here is part of the JIT-callable C ABI, so the struct layouts below
// are mirrored bit-for-bit by the backend (ir/llvm/RuntimeAbi.cpp, `gc_map_ty` /
// `gc_roots_ty` / `gc_frame_ty`) exactly as rt_elem_ops is. Any change is a
// lockstep two-file edit; rt_gc_init cross-checks the one offset the backend
// computes independently.
#ifndef CLOTHO_RUNTIME_GC_H
#define CLOTHO_RUNTIME_GC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// rt_elem_leaf, from Runtime.h. Declared incomplete here so this header does not
// depend on the whole runtime ABI; the GC only ever passes these to the
// element-lifecycle walkers, which live in runtime/Containers.cpp.
struct rt_elem_leaf;
struct rt_elem_ops;

// -- Geometry -----------------------------------------------------------------
//
// Immix's experimentally-selected sizes (Blackburn & McKinley, PLDI 2008 §5): a
// 128-byte line and a 32 KiB block. RT_GC_ALIGN is the object alignment: 16, not
// the 64 rt_alloc gives, because rt_alloc's 64 exists for over-aligned coroutine
// frame plane spills (runtime/Scheduler.cpp, rt_alloc) and would waste 62% of a 24-byte
// transaction object.
#define RT_GC_BLOCK_BYTES     32768u
#define RT_GC_LINE_BYTES      128u
#define RT_GC_LINES_PER_BLOCK (RT_GC_BLOCK_BYTES / RT_GC_LINE_BYTES)
#define RT_GC_ALIGN           16u

// Objects strictly above this go to the large-object space instead of a block.
// Published rather than file-static in Gc.cpp because the INLINED allocation fast
// path must skip them, so the backend needs the same number: three reasons for the
// value, in decreasing weight -- a block-resident object this big leaves at most
// three usable quarter-block holes behind and so fragments a block far out of
// proportion to the space it uses; four of them exhaust a block, so the per-block
// acquisition cost the block structure exists to amortise would be paid every
// fourth allocation; and it keeps every block-resident object strictly inside one
// block, which is the precondition all the line-mark and start-bitmap index
// arithmetic relies on.
#define RT_GC_LARGE_OBJECT_BYTES (RT_GC_BLOCK_BYTES / 4u)

// The GC word sits BELOW the descriptor pointer: a handle points at the vptr, and
// the header is at handle[-1]. That keeps class_header_words() == 1, every
// property GEP index at `1 + declIndex`, and the `$cast` display arithmetic
// (`4*depth - 64` off the vptr) untouched -- the same reason jl_taggedvalue_t
// sits below a Julia object.
#define RT_GC_HEADER_BYTES 8u

// -- The GC word --------------------------------------------------------------
//
// bits 0-1  mark state, compared against the collector's current epoch value
//           (1 or 2, toggled per collection) so a collection never has to clear
//           marks. 0 is "never marked".
// bit  2    PINNED    -- a conservative root reached this object; never move it.
// bit  3    FINALIZE  -- this object is on the finalizable list.
// bit  4    HAS_ID    -- the payload holds a lazily-assigned object id.
// bit  5    FORWARDED -- the payload holds a forwarding address (evacuation only;
//                        never set while the collector is non-moving).
// bits 8-63 payload   -- object id or forwarding address, never both: the id
//                        travels with the copy and the original's word is dead
//                        after the collection that forwarded it.
#define RT_GC_MARK_MASK      0x3ull
#define RT_GC_FLAG_PINNED    (1ull << 2)
#define RT_GC_FLAG_FINALIZE  (1ull << 3)
#define RT_GC_FLAG_HAS_ID    (1ull << 4)
#define RT_GC_FLAG_FORWARDED (1ull << 5)

// -- Generational bits (docs/design/garbage-collection.md §7) ------------------
//
// Sticky mark bits, not a copied nursery: an object that survives a collection is
// promoted in place by setting RT_GC_FLAG_OLD, and a young collection then treats
// it as live-and-opaque. LOGGED says the object is already in the remembered set,
// so the inlined write barrier can skip a cold call it has already made.
//
// Three places must respect OLD or they turn a young collection into a
// use-after-free, and each is a separate assertion in Gc.cpp:
//   * reclamation must not free a line that holds an OLD object, which is what the
//     per-line `line_old` record exists for;
//   * the finalizer sweep must not finalize an unmarked OLD object -- in a young
//     collection unmarked does not mean dead;
//   * the large-object sweep must not reclaim an unmarked OLD cell, same reason.
// bit  6    OLD    -- survived a collection; a young collection treats it as
//                     live-and-opaque and its lines are held by `line_old`.
// bit  7    LOGGED -- already in the remembered set, so the inlined barrier can
//                     skip a cold call it has already made.
// bit  8    OMARK  -- reached by the current FULL collection, compared against a
//                     parity that toggles only at full collections.
//
// TWO mark bits, not one, and the reason is a bug this design already had once:
// epoch parity is sound only while EVERY collection stamps EVERY live object. A
// young collection deliberately skips OLD objects, so an old object's stored parity
// can be arbitrarily stale -- and a stale value matches any fixed-length epoch
// cycle, which made a FULL collection read a live object as already-marked, skip
// marking its lines, and then unprotect them. Each bit is now stamped by every
// collection of its own kind, which bounds each one's staleness to one such
// collection. Julia's four states (CLEAN / MARKED / OLD / OLD_MARKED) exist for
// exactly this reason.
#define RT_GC_FLAG_OLD    (1ull << 6)
#define RT_GC_FLAG_LOGGED (1ull << 7)
#define RT_GC_FLAG_OMARK  (1ull << 8)
#define RT_GC_PAYLOAD_SHIFT  9u

// The write barrier's out-of-line half. Generated code tests
// `(gcword & (OLD|LOGGED)) == OLD` inline at every store of a class handle into a
// heap object and calls this only on the taken edge, so reaching here means an old
// object has just acquired a reference that a young collection would otherwise
// never look at. Idempotent: it sets LOGGED, so a hot object costs the call once
// per collection cycle rather than once per store.
//
// Design state, statics and frame locals need NO barrier -- they are scanned in
// full at every collection, young or not -- so this is emitted only for a store
// whose target is inside a heap object (§7).
void rt_gc_remember(void* obj);

// -- Per-class trace map ------------------------------------------------------
//
// One constant per class, emitted as `clotho.classgc.<Ref>` and reached from any
// handle in two loads: the vptr at handle[0], then this pointer at a design-wide
// constant negative offset from it (rt_gc_map_vptr_offset(), asserted against the
// backend's own computation at rt_gc_init).
//
// `size` covers the header: RT_GC_HEADER_BYTES + the body's alloc size, rounded
// up to RT_GC_ALIGN. Offsets in `handles` and `leaves` are from the HANDLE (the
// body base), not from the header, so they are exactly the byte offsets
// getStructLayout() reports for class_body_ty.
//
// `leaves` describes the class's managed properties (string / dynarr / queue /
// assoc) and is walked for TWO purposes, both required since P4 lifted the guard
// that used to keep class handles out of containers:
//
//   * TRACING. A container property's payload is `malloc`'d, so its elements are
//     reachable only by walking this table into the container and through its
//     element descriptor. This is why a class-property container is NOT a
//     container root -- see the root/reachable table below -- and it is why a
//     REMEMBERED old object must be traced through its leaves as well as its
//     inline handle fields. Tracing only the inline fields frees a live element of
//     `old.q` on the next young collection.
//   * FINALIZATION. The emitted `finalize` frees the same buffers when the object
//     dies.
//
// A `string` leaf holds no handle and is skipped by the tracing half.
typedef struct rt_gc_map {
    uint64_t                    size;
    uint32_t                    nhandles;
    uint32_t                    nleaves;
    const uint32_t*             handles;
    const struct rt_elem_leaf*  leaves;
    void                      (*finalize)(void* body);
} rt_gc_map;

// -- Precise global roots -----------------------------------------------------
//
// One constant per storage region (the design state buffer, each per-instance
// state buffer, each static class property block): the class-handle slots inside
// it, by byte offset. Emitted by the backend and registered once at startup, so
// the table's lifetime is the JIT'd module's and the runtime only borrows it.
typedef struct rt_gc_roots {
    void*           base;
    uint32_t        nhandles;
    uint32_t        reserved;
    const uint32_t* offsets;
} rt_gc_roots;

// -- Coroutine frame registry -------------------------------------------------
//
// A suspended SV process has no stack: it is an LLVM switched-resume coroutine
// whose live handles are in a heap frame with a CoroSplit-chosen layout
// (ir/llvm/Subroutines.cpp, lower_group). The node is an alloca inside the coroutine, which
// CoroSplit hoists into that frame, so linking it at the ramp makes the frame's
// base and size reachable while the process is suspended. The frame is then
// scanned word-wise and every hit validated and pinned.
//
// EVERY path that frees a frame must unlink: a node left linked after its frame
// is freed is a use-after-free inside the collector. There are exactly two such
// paths, the process ramp's and the task ramp's `coro.free` blocks, and every
// llvm.coro.destroy routes through the coroutine's destroy clone into the same
// cleanup, so the unlink is emitted once per ramp in `coro_cleanup_bb_` -- which
// dominates both. (An earlier draft of docs/design/garbage-collection.md claimed
// two further raw-destroy paths that skip the epilogue. A census of every
// free_fn() and coro_destroy call site found they do not exist.)
typedef struct rt_gc_frame {
    struct rt_gc_frame* prev;
    struct rt_gc_frame* next;
    void*               base;
    uint64_t            bytes;
} rt_gc_frame;

// -- Mutator context ---------------------------------------------------------
//
// The allocation state, reached through a pointer rather than a link-time
// constant address. That indirection is the whole point: multi-threaded
// simulation is a planned milestone, and a baked `@clotho.gc.cursor` would
// foreclose it. Under one mutator rt_gc_mutator_current() returns the same
// pointer forever and the indirection folds away.
//
// `cursor` / `limit` are the bump range inside the mutator's current block hole.
// The inlined allocation fast path (a later phase) loads them at these offsets;
// today the allocation slow path owns them and generated code only calls
// rt_gc_alloc.
//
// `request` is polled INLINE by generated code at every safepoint -- a load, a
// test and a not-taken branch, no call. Measured before that: one out-of-line
// rt_gc_safepoint per wheel advance cost real simulate time on designs that
// allocate nothing at all. The allocator sets `request` when the nursery budget
// runs out, and a stress-mode collection re-arms it, so the inline poll is the
// only check generated code ever needs to emit.
typedef struct rt_gc_mutator {
    uint8_t*             cursor;
    uint8_t*             limit;
    uint64_t             budget_left;
    uint32_t             block_index;
    uint32_t             request;
    // Cached so the INLINED allocation fast path needs no address arithmetic
    // beyond this struct: the object-start bit of an address is
    // `((addr - block_base) >> 4)` in `start_bitmap`, and the finalizable bit is
    // the same index in `fin_bitmap`. The slow path refreshes all three whenever
    // it hands out a new hole, so a fast-path allocation can never index the wrong
    // block's metadata.
    uint8_t*             block_base;
    uint8_t*             start_bitmap;
    uint8_t*             fin_bitmap;
    // The coroutine frame registry head, moved here from a file-static so the
    // link/unlink pair can be inlined too, and so it becomes per-mutator for free
    // when simulation goes multi-threaded (§6.2).
    struct rt_gc_frame*  frames_head;
} rt_gc_mutator;

// -- Configuration and lifecycle ---------------------------------------------

// `nursery_bytes`  bytes allocated between collections before one is requested.
// `heap_bytes_max` soft ceiling reported through the stats line; the heap grows
//                  past it between safepoints exactly as an uncollected heap
//                  does today (see docs/design/garbage-collection.md §6.1 on the
//                  unsuspended-allocation hole).
// `stress`         collect at every safepoint.
// `stats`          print a collection summary at shutdown.
// `map_vptr_offset` the backend's own computation of the constant negative offset
//                  from a vptr to its rt_gc_map pointer. rt_gc_init asserts it
//                  equals rt_gc_map_vptr_offset(), so a descriptor-layout change
//                  fails loudly instead of reading garbage.
// `request_offset`  the backend's own computation of offsetof(rt_gc_mutator,
//                  request), cross-checked the same way, because generated code
//                  loads that byte directly to poll for a due collection.
// `mutator_bytes`  the backend's sizeof(rt_gc_mutator). Generated code loads
//                  mutator fields at offsets it computes from its own mirror, so a
//                  size mismatch means the mirror drifted and every one of those
//                  offsets is suspect.
typedef struct rt_gc_config {
    uint64_t nursery_bytes;
    uint64_t heap_bytes_max;
    int32_t  stress;
    int32_t  stats;
    int32_t  map_vptr_offset;
    int32_t  request_offset;
    int32_t  mutator_bytes;
    // One full collection per `full_every` collections; 0 lets the runtime choose
    // by heap growth. A full collection is the only thing that reclaims old
    // garbage or re-derives which lines hold old survivors (§7).
    int32_t  full_every;
    int32_t  reserved;
} rt_gc_config;

void rt_gc_init(const rt_gc_config* cfg);

// Run every remaining finalizer and release the heap. Not needed for
// correctness -- the process is exiting -- but it is what makes LeakSanitizer
// meaningful for class objects, which is the acceptance evidence for this work.
void rt_gc_shutdown(void);

// The constant negative offset from a vptr to the rt_gc_map pointer that sits
// immediately below the ancestor display.
int32_t rt_gc_map_vptr_offset(void);

// -- Allocation ---------------------------------------------------------------

rt_gc_mutator* rt_gc_mutator_current(void);

// Allocate an object of `size` bytes INCLUDING the header (i.e. map->size) and
// return the HANDLE -- the body base, one word above the header. Zeroes the body
// (2-state leaves and handle properties power on 0 / null, LRM Table 7-1), writes
// the GC word, and records the object as finalizable when map->finalize is
// non-null. Never returns null: an unsatisfiable request takes the runtime's
// located-fatal path, matching rt_alloc's contract at the ClassNew site
// (ir/llvm/Inst.cpp elides the null guard on a fresh handle).
//
// Generated code INLINES the bump-and-mark sequence against the mutator fields
// above and reaches the runtime only when the current hole cannot satisfy the
// request, so in a collected run this entry point serves large objects, the
// runtime's own callers and the tests.
void* rt_gc_alloc(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map);

// The out-of-line half of the inlined fast path: the current hole is exhausted,
// so refill (next hole, then a recycled or fresh block, then a new chunk) and
// allocate. Refreshing `cursor` / `limit` / `block_base` / `start_bitmap` /
// `fin_bitmap` is this function's job, and the nursery budget is charged HERE, per
// hole handed out, rather than per object -- which is what keeps the fast path free
// of budget arithmetic while leaving the accounting exact in bytes made available.
void* rt_gc_alloc_slow(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map);

// sizeof(rt_gc_mutator), for the layout cross-check. Generated code loads mutator
// fields at offsets it computes from its own mirror of the struct, so a size
// mismatch means the mirror has drifted and every one of those offsets is suspect.
uint32_t rt_gc_mutator_bytes(void);

// -- Roots --------------------------------------------------------------------

void rt_gc_roots_add(const rt_gc_roots* table);

void rt_gc_frame_link(rt_gc_frame* node, void* base, uint64_t bytes);
void rt_gc_frame_unlink(rt_gc_frame* node);

// A runtime-owned region scanned conservatively at every collection. Used for
// the nonblocking-assignment and intra-assignment staging pools, which hold byte
// images of values that may contain a class handle and are not described by any
// emitted root table.
void rt_gc_range_add(const void* base, uint64_t bytes);
void rt_gc_range_remove(const void* base);

// -- Containers of class handles (docs/design/garbage-collection.md §3.3) ------
//
// A container's payload is `malloc`'d, not GC-heap memory, so neither root path
// finds the handles inside it: a precise root table describes state-buffer words,
// and a conservative frame scan sees only a pointer to a ring the collector
// rejects because it is not in a block. A handle-bearing container therefore has
// to be traced from its handle, precisely, through its element descriptor.
//
// Which containers are ROOTS, and which are merely reachable, is the distinction
// that matters and is decided by where the container's own storage lives:
//
//   design state / static     -> a root. Emitted as a static table, registered
//                                once at startup; the storage never moves.
//   frame local               -> a root while the activation lives. Registered
//                                beside the existing element init and removed
//                                beside the existing deinit.
//   class property            -> NOT a root. Traced through the owning object's
//                                rt_gc_map leaves, so a container inside a dead
//                                object keeps nothing alive.

// One state-resident container: byte offset within the table's base region, its
// element kind (rt_elem_dynarr / rt_elem_queue / rt_elem_assoc) and its element
// descriptor.
typedef struct rt_gc_container {
    uint64_t           offset;
    uint32_t           kind;
    uint32_t           reserved;
    const struct rt_elem_ops* ops;
} rt_gc_container;

typedef struct rt_gc_container_table {
    void*                    base;
    uint32_t                 count;
    uint32_t                 reserved;
    const rt_gc_container* entries;
} rt_gc_container_table;

void rt_gc_containers_add(const rt_gc_container_table* table);

// A dynamically-scoped handle-bearing container: a frame local, or an element of
// one. `handle` is the rt_dynarr / rt_queue / rt_assoc itself.
void rt_gc_container_add(void* handle, uint32_t kind, const struct rt_elem_ops* ops);
void rt_gc_container_remove(void* handle);

// -- Collection ---------------------------------------------------------------

// Reasons, for the stats line.
#define RT_GC_REASON_BUDGET   0u
#define RT_GC_REASON_STRESS   1u
#define RT_GC_REASON_EXPLICIT 2u

// Collection KIND, orthogonal to the reason above. A young collection marks from
// the roots plus the remembered set and treats an OLD object as live without
// tracing into it; a full collection traces everything and re-derives which lines
// hold old survivors. Old garbage therefore accumulates until the next full
// collection, which is the generational trade and is why `full_every` exists.
#define RT_GC_KIND_YOUNG 0u
#define RT_GC_KIND_FULL  1u

// Collect now, of the given kind, regardless of the budget. `rt_gc_collect` keeps
// its meaning and picks the kind by policy.
void rt_gc_collect_kind(uint32_t reason, uint32_t kind);

// The offset of rt_gc_mutator::request, for the poll generated code inlines.
uint32_t rt_gc_mutator_request_offset(void);

// The out-of-line half of a safepoint. Generated code polls
// `mutator->request` inline and calls this ONLY when it is set, so reaching here
// means a collection is due: it collects unconditionally. Called at the
// scheduler's region boundaries, where no JIT frame is live and every process is
// suspended in its frame.
void rt_gc_safepoint_slow(void);

// Poll and collect if due. What generated code emitted before the poll was
// inlined, and what the runtime's own call sites and the tests still use.
// Equivalent to `if (rt_gc_mutator_current()->request) rt_gc_safepoint_slow();`.
void rt_gc_safepoint(void);

// Stop-the-world mark-region collection. Marks from the precise global tables,
// the conservative frame registry and the conservative runtime ranges; runs the
// finalizers of unreachable objects; then reclaims free lines and recycles empty
// blocks. The mark phase is an explicit bounded worklist -- never recursion
// (TigerStyle) -- because a testbench object graph is arbitrarily deep.
void rt_gc_collect(uint32_t reason);

// -- Introspection (tests, --gc-stats, the debug modes) -----------------------
//
// Two guarantees the tests depend on, so they are contract and not implementation
// detail (tests/gc_test.cpp reads these counters in-process after a run):
//
// 1. rt_gc_init zeroes the whole struct and rt_gc_shutdown never touches it, so
//    the counters after a run describe exactly that run, and objects_live /
//    bytes_live hold what the LAST collection computed.
// 2. `finalized` counts finalizers run by a COLLECTION only. The shutdown sweep
//    reports separately and stays out of this struct -- otherwise "reclaimed
//    mid-run" and "cleaned up at exit" would be indistinguishable, which is the
//    entire oracle for cycle collection.

typedef struct rt_gc_stats {
    uint64_t collections;
    // Generational accounting (§7). `young` + `full` == `collections`;
    // `remembered` is the objects the write barrier logged over the run, which is
    // what tells a test the barrier fired at all rather than the young collection
    // having found everything through a root.
    uint64_t young_collections;
    uint64_t full_collections;
    uint64_t remembered;
    uint64_t promoted;
    uint64_t bytes_allocated;
    uint64_t objects_allocated;
    uint64_t objects_live;
    uint64_t bytes_live;
    uint64_t blocks_total;
    uint64_t blocks_free;
    uint64_t finalized;
    uint64_t conservative_pins;
    // Handle-bearing containers consulted as roots in the last collection (§3.3):
    // a test asserting a frame-local queue kept its elements alive checks this to
    // prove the container-root path ran rather than a conservative frame word
    // having happened to pin the same object.
    uint64_t container_roots;
} rt_gc_stats;

void rt_gc_get_stats(rt_gc_stats* out);

// Resolve a candidate word to the handle of the object containing it, or null.
// The validation predicate behind conservative scanning: the address must land in
// a live block, be RT_GC_ALIGN-aligned once walked back, and have its
// object-start bit set. Also resolves interior pointers, which is what makes a
// `ref` into a class property (a raw interior pointer carrying
// clotho.ref.noepoch, ir/llvm/StateLayout.cpp, ref_actual_arg) safe with no epoch machinery.
void* rt_gc_find_object(const void* addr);

// Walk the heap asserting every live object's descriptor, size and handle fields
// are self-consistent. The verify mode from
// docs/design/garbage-collection.md §13; a no-op unless assertions are live.
void rt_gc_verify(void);

#ifdef __cplusplus
}
#endif

#endif // CLOTHO_RUNTIME_GC_H
