// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Garbage collection for SystemVerilog class objects (LRM 8.4).
//
// Design: docs/design/garbage-collection.md. A non-moving mark-sweep heap of
// 4 MiB chunks cut into 32 KiB blocks, with one metadata byte beside the heap for
// every 16-byte granule and no header in the object. Traced precisely through the
// per-class map the backend emits beside each class descriptor, rooted precisely
// in the design state and conservatively in live coroutine frames.
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
// rt_alloc_site, from Runtime.h: where an allocation came from, for the located
// fatal. Null where the caller has no statement to name.
struct rt_alloc_site;

// -- Geometry -----------------------------------------------------------------
//
// A granule is the unit of everything: an object starts on one, its size is a
// whole number of them, and free space is found and reclaimed one at a time.
// 16 bytes, not the 64 rt_alloc gives, because rt_alloc's 64 exists for
// over-aligned coroutine frame plane spills (runtime/Scheduler.cpp, rt_alloc) and
// would waste most of a 24-byte transaction object.
//
// A chunk is what the heap grows and shrinks by. It is aligned to its own size,
// so the chunk an address belongs to is the address with its low bits cleared,
// and it carries the metadata of its own granules at a fixed offset, so that
// metadata is found from an object's address by arithmetic alone.
//
// A block is what a mutator allocates from: it owns one at a time and bumps
// through the free runs in it. No object crosses a block boundary.
#define RT_GC_ALIGN       16u
#define RT_GC_BLOCK_BYTES 32768u
#define RT_GC_CHUNK_BYTES (4u << 20)

// An object larger than this gets a mapping of its own instead of a place in a
// block. Published because the inlined allocation fast path must skip such an
// object, so the backend needs the same number.
#define RT_GC_LARGE_OBJECT_BYTES RT_GC_BLOCK_BYTES

// -- The metadata byte --------------------------------------------------------
//
// One byte per granule, at `chunk + RT_GC_META_OFFSET + granule index`, where the
// chunk is the address rounded down to RT_GC_CHUNK_BYTES and the index is the
// offset within it over RT_GC_ALIGN. Generated code computes exactly that at an
// allocation and at a write barrier, so both numbers are ABI.
//
//   0                    a free granule.
//   START | flags        the first granule of an object.
//   1 .. BACK_MAX        a later granule of an object: how many granules back its
//                        first one is, or BACK_MAX when it is at least that far,
//                        in which case the byte that far back says the rest.
//
// So an address inside an object leads to the object's start in a few steps
// without reading the object, a sweep finds an object's extent the same way, and
// a run of zero bytes is free space.
//
// The flags of an object:
//   MARK      reached by a collection, and so OLD: an object is old once a
//             collection has marked it, and this bit is the only record of
//             either. A young collection leaves a marked object alone; a full
//             collection clears every mark before it starts.
//   LOGGED    already in the remembered set, so the write barrier's inline test
//             skips a call it has already made.
//   PINNED    reached from a conservative root during the collection now running.
//   FINALIZE  the object's class has a finalizer that has not run.
//
// The chunk's first RT_GC_META_OFFSET bytes are its own record, the metadata
// follows, and objects start RT_GC_CHUNK_PAYLOAD in. A large object's mapping is
// laid out the same way with the object RT_GC_LARGE_PAYLOAD in, so its one
// metadata byte is found by the same arithmetic.
#define RT_GC_META_OFFSET   4096u
#define RT_GC_META_START    0x80u
#define RT_GC_META_MARK     0x01u
#define RT_GC_META_LOGGED   0x02u
#define RT_GC_META_PINNED   0x04u
#define RT_GC_META_FINALIZE 0x08u
#define RT_GC_META_BACK_MAX 127u
#define RT_GC_CHUNK_PAYLOAD (9u * RT_GC_BLOCK_BYTES)
#define RT_GC_LARGE_PAYLOAD 8192u

// The write barrier's out-of-line half. Generated code tests
// `(meta & (MARK|LOGGED)) == MARK` on the owner's metadata byte inline at every
// store of a class handle into a heap object and calls this only on the taken
// edge, so reaching here means an old object has just acquired a reference that a
// young collection would otherwise never look at. It makes the same test itself,
// so a caller without the inline half may call it for any object. Idempotent: it
// sets LOGGED, so a hot object costs the call once per collection cycle rather
// than once per store.
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
// `size` is the body's alloc size rounded up to RT_GC_ALIGN: an object is its body
// and nothing else. Offsets in `handles` and `leaves` are from the handle, which
// is the object's first byte, so they are exactly the byte offsets
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
// `cursor` / `limit` are the bump range inside the mutator's current free run.
// Generated code bumps the cursor inline and reaches the runtime only when the run
// cannot hold the object.
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
    uint32_t             reserved;
    uint32_t             request;
    // The coroutine frame registry head, here so the link/unlink pair can be
    // inlined, and so it is per-mutator when simulation goes multi-threaded (§6.2).
    struct rt_gc_frame*  frames_head;
} rt_gc_mutator;

// -- Configuration and lifecycle ---------------------------------------------

// `nursery_bytes`  bytes allocated between collections before one is requested.
// `heap_bytes_max` ceiling on the class heap in bytes, 0 for none. An allocation
//                  that would take the heap past it first runs an emergency
//                  collection, and ends the run with a located fatal if the
//                  objects still do not fit (docs/design/garbage-collection.md
//                  §6.1). The heap grows in whole chunks, so the ceiling is the
//                  largest number of them that fits, and never fewer than one.
// `stress`         collect at every safepoint, and at every allocation once the
//                  simulation is running.
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
    // garbage (§7).
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

// Allocate an object of `size` bytes (map->size) and return its handle, which is
// its first byte. Zeroes the body (2-state leaves and handle properties power on
// 0 / null, LRM Table 7-1), writes the object's metadata bytes, and records it as
// finalizable when map->finalize is non-null. Never returns null: an
// unsatisfiable request takes the runtime's located-fatal path, matching
// rt_alloc's contract at the ClassNew site (ir/llvm/Inst.cpp elides the null
// guard on a fresh handle). `site` names the statement for that fatal and is null
// where there is none.
//
// Generated code INLINES the bump and the metadata stores and reaches the runtime
// only when the current free run cannot hold the object, so in a collected run
// this entry point serves large objects, the runtime's own callers and the tests.
//
// This is also where a collection can happen in the middle of the design: when
// the budget has been exceeded by a large factor with no safepoint reached, the
// allocation collects before it allocates (§6.1, "The unsuspended-allocation
// hole"). Everything the running code still needs is therefore found from the
// native stack, the registers and the registered roots, so a caller must hold a
// handle it is going to use in a register, on the stack or in a root across
// this call -- which generated code does by construction, since a value used
// after a call has to be somewhere.
void* rt_gc_alloc(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map,
                  const struct rt_alloc_site* site);

// rt_gc_alloc for a runtime caller that holds handles where no collection would
// find them (a container buffer being built): it never collects, at the price
// that the nursery budget and --gc-heap-max are only enforced by the next
// ordinary allocation or safepoint. An object that does not fit under the
// ceiling without a collection ends the run.
void* rt_gc_alloc_held(uint64_t size, const rt_gc_map* map);

// The out-of-line half of the inlined fast path: the current free run cannot hold
// the object, so find another (the next run in this block, then a block with
// room, then a new chunk) and allocate. Refreshing `cursor` / `limit` is this
// function's job, and the nursery budget is charged HERE, per run handed out,
// rather than per object -- which is what keeps the fast path free of budget
// arithmetic while leaving the accounting exact in bytes made available.
void* rt_gc_alloc_slow(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map,
                       const struct rt_alloc_site* site);

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

// An NBA staging queue (rt_nba_arr_*, Runtime.h) whose records may hold a class handle
// between staging and the NBA commit. The queue is a growable byte buffer of fixed-stride
// records the backend lays out, so its address moves and each record cannot be a range of
// its own; the collector instead reads rt_nba_arr_base / rt_nba_arr_count at every
// collection and scans the records staged so far.
//
// `words`: the records hold the handle as a naturally aligned word (an array element, a
// member written through a virtual interface, an aggregate leaf), so each whole record is
// scanned conservatively. `container`: each record holds a whole-container copy at byte
// `offset`, whose handles sit in malloc'd storage and are found by the element
// descriptor, exactly as for a container local to a frame.
void rt_gc_nba_words_add(uint32_t idx, uint32_t stride);
void rt_gc_nba_container_add(uint32_t idx, uint32_t stride, uint32_t offset, uint32_t kind,
                             const struct rt_elem_ops* ops);

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
#define RT_GC_REASON_BUDGET    0u
#define RT_GC_REASON_STRESS    1u
#define RT_GC_REASON_EXPLICIT  2u
// A collection run from inside an allocation (§6.1), not at a region boundary.
#define RT_GC_REASON_EMERGENCY 3u

// Collection KIND, orthogonal to the reason above. A young collection marks from
// the roots plus the remembered set, treats an OLD object as live without tracing
// into it, and sweeps only the blocks allocated from since the last collection; a
// full collection clears every mark, traces everything and sweeps everything. Old
// garbage therefore accumulates until the next full collection, which is the
// generational trade and is why `full_every` exists.
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

// Stop-the-world mark and sweep. Marks from the precise global tables, the
// conservative frame registry and the conservative runtime ranges; then sweeps,
// which runs the finalizer of each unreachable object and frees its granules.
// The mark phase is an explicit bounded worklist -- never recursion (TigerStyle)
// -- because a testbench object graph is arbitrarily deep.
void rt_gc_collect(uint32_t reason);

// Bracket the span in which generated code runs under the scheduler: rt_sim_run
// passes the address of one of its own locals, which is above every frame the
// design's code can have on the native stack, and null on the way out. An
// emergency collection scans the stack from where it is called up to this
// address, so with none recorded -- static initialization before the run, a test
// calling the allocator directly -- allocation never collects and the heap grows
// to the next safepoint, as it did before emergency collection existed.
void rt_gc_run_enter(const void* stack_base);
void rt_gc_run_leave(void);

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
    // Collections run from inside an allocation (RT_GC_REASON_EMERGENCY); they are
    // also counted in `collections`.
    uint64_t emergency_collections;
    // Bytes of string and container buffers the design holds right now, as
    // rt_internal::g_owned (GcBuffers.h) counts them. Zero after a run that frees
    // everything it allocates, which every design ending by running out of events
    // does, so a test can assert it to find a free site that does not report.
    uint64_t owned_bytes;
    // Objects that held an identity number (rt_gc_identity) once the last
    // collection had forgotten the numbers of those it did not reach.
    uint64_t identities;
    // Bytes the heap holds from the system right now: its chunks and the mappings of
    // large objects, those of dead ones kept for reuse included. It falls when a
    // full collection returns empty chunks or kept mappings.
    uint64_t heap_bytes;
} rt_gc_stats;

void rt_gc_get_stats(rt_gc_stats* out);

// Resolve a candidate word to the handle of the object containing it, or null.
// The validation predicate behind conservative scanning: the address must lie in
// a chunk of the heap or in a large object's mapping, and its granule must belong
// to an object. Also resolves interior pointers, which is what makes a `ref` into
// a class property (a raw interior pointer that passes no witness chain,
// ir/llvm/StateLayout.cpp, ref_actual_arg) safe with no epoch machinery.
void* rt_gc_find_object(const void* addr);

// The identity number of the object at `handle`: assigned by the first call for
// that object, counting from 1 in the order of those calls, and never given to
// another object in the same run. 0 for null. It is what stands for the object
// wherever its address must not: in the bytes of an associative-array key, which
// decide the array's order, and in formatted output.
uint64_t rt_gc_identity(void* handle);

// The object an identity number names; null for 0. A number whose object has been
// reclaimed names nothing, and asking for it ends the run.
void* rt_gc_identity_object(uint64_t id);

// Walk the heap asserting every live object's descriptor, size and handle fields
// are self-consistent. The verify mode from
// docs/design/garbage-collection.md §13; a no-op unless built with
// CLOTHO_EXTRA_ASSERTS.
void rt_gc_verify(void);

#ifdef __cplusplus
}
#endif

#endif // CLOTHO_RUNTIME_GC_H
