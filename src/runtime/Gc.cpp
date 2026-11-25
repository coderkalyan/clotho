// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Garbage collection for SystemVerilog class objects (LRM 8.4): the non-moving,
// stop-the-world, generational mark-sweep collector of docs/design/architecture.md
// (Memory). Gc.h is the ABI contract and carries the layout; this file carries the
// mechanism.
//
// Four properties are worth stating up front, because nearly every decision below
// follows from one of them.
//
// ALL COLLECTOR STATE IS BESIDE THE HEAP. An object is its body: no header word, and
// the collector never writes into it. What it knows about an object is in the
// metadata byte of the object's first granule (Gc.h), found from the object's
// address by arithmetic, and the same bytes say where every object starts and ends
// and which granules are free.
//
// OLD IS MARKED. One bit is both. A young collection treats a marked object as live
// without tracing it, marks what it reaches, and sweeps only the blocks allocated
// from since the last collection: an unmarked object can be nowhere else, because
// every object outside them was marked by an earlier collection or freed by it. A
// full collection clears every mark first, so afterwards a mark means "reached by
// this collection" and an unmarked object anywhere is dead.
//
// THE SWEEP IS PER OBJECT AND TOUCHES NO OBJECT. It walks the metadata bytes of a
// block, finds each object's extent there, and zeroes the bytes of a dead one, which
// is the whole of freeing it. Only an object that owes a finalizer is read.
//
// PINNING IS INERT. RT_GC_META_PINNED is set on every conservatively-reached object
// and read only to carry the write-barrier obligation of a suspended activation
// forward (relog_conservative_roots), because nothing moves. unpin_all() is what
// makes "every pin is released" checkable.
//
// COLLECTION HAPPENS AT A SAFEPOINT, EXCEPT WHEN AN ALLOCATION LOOP NEVER REACHES ONE.
// The ordinary collection runs at a scheduler region boundary, the one place with no
// live JIT frame (design §6.1, F1): crossing the budget raises mutator.request and the
// next boundary collects. A loop that allocates and never suspends reaches none, so
// once kEmergencyFactor budgets have been spent without one, the allocation slow path
// collects from inside the allocation. That collection runs with JIT frames live, so it
// also scans the native stack and the registers conservatively (push_stack_roots);
// design §6.1, "Why collecting inside an allocation is sound", is the argument that this
// finds everything the running code still uses. With no stack extent recorded
// (rt_gc_run_enter) allocation never collects.
//
// THE SAFEPOINT IS A BYTE. Generated code polls `mutator.request` inline -- a
// load, a test and a not-taken branch -- and calls rt_gc_safepoint_slow() only
// when it is set (Gc.h:144-149, design §6.1-§6.2). So that byte is the whole of
// "a collection is due": charge_budget() raises it when the nursery budget runs
// out, buffers_pressure() when owned buffers have grown by a budget, and a
// stress-mode collection re-arms it at the end of every collection, because the
// inlined poll cannot see g.cfg.stress. A design that allocates nothing never sets
// it, and never pays more than the load and the branch.
#include "Gc.h"

#include "GcBuffers.h"
#include "Os.h"
#include "Runtime.h"
#include "RuntimeInternal.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

#include <ankerl/unordered_dense.h>

// An assertion that resolves a pointer through the heap's metadata (a bitmap scan or
// a large-object lookup) rather than testing a field. On the allocation, marking and
// barrier paths those lookups add 21% to the instructions of a collection-bound
// design, so they are extra asserts; field tests stay live in every build.
#ifdef CLOTHO_EXTRA_ASSERTS
#define assert_resolves(expr) assert(expr)
#else
#define assert_resolves(expr) ((void)0)
#endif

using rt_internal::g_owned;

namespace {

// -- Geometry -----------------------------------------------------------------

constexpr uint64_t kChunkBytes   = RT_GC_CHUNK_BYTES;
constexpr uint64_t kChunkMask    = kChunkBytes - 1;
constexpr uint32_t kGranuleShift = 4;

// A chunk's own record, its metadata and then its blocks. The metadata array has a
// byte for every granule of the chunk, the ones under the record and under the array
// itself included: they are never read, and leaving them in is what makes the index
// the plain offset.
constexpr uint32_t kGranulesPerBlock = RT_GC_BLOCK_BYTES / RT_GC_ALIGN;
constexpr uint32_t kFirstBlockOffset = RT_GC_CHUNK_PAYLOAD;
constexpr uint32_t kBlocksPerChunk =
  static_cast<uint32_t>((kChunkBytes - kFirstBlockOffset) / RT_GC_BLOCK_BYTES);

// One more flag than Gc.h publishes, because only the collector reads it: set by the
// pass that clears the marks before a FULL collection on every object that was marked,
// and cleared by that collection's sweep. It lets stats.promoted count an object once,
// when it is first marked, rather than at every full collection it survives.
constexpr uint8_t kMetaWasOld = 0x10;
constexpr uint8_t kMetaFlags =
  RT_GC_META_MARK | RT_GC_META_LOGGED | RT_GC_META_PINNED | RT_GC_META_FINALIZE | kMetaWasOld;

// The ancestor-display length the backend emits into class_desc_ty, from
// src/ir/Module.h:602 (`inline constexpr uint32_t kMaxClassDepth = 16`). The map
// pointer the backend prepends therefore sits one pointer plus this many i32
// display entries below the vptr. rt_gc_init cross-checks the result against the
// backend's own DataLayout computation, so a divergence fails loudly rather than
// reading garbage.
constexpr uint32_t kMaxClassDepth = 16;

// Objects above a block get a mapping of their own (Gc.h). Published there rather than
// here because the INLINED allocation fast path has to skip large objects, so the
// backend needs the same number.
constexpr uint64_t kLargeObjectBytes = RT_GC_LARGE_OBJECT_BYTES;

// Runaway-detection bounds, not design limits: every loop over caller-supplied
// data must be bounded by something (TigerStyle), and a corrupt frame
// registry should trip an assertion rather than spin forever. [[maybe_unused]]
// because a release build compiles the assertions that read them away.
[[maybe_unused]] constexpr uint64_t kFrameScanMax   = 1ull << 20;
[[maybe_unused]] constexpr uint32_t kRootHandlesMax = 1u << 28;
// A conservatively scanned region is a coroutine frame or a staging pool. Neither
// is remotely this large, so a bigger `bytes` means the registry node or the range
// registration is corrupt and the word-wise loop below would run off the end.
[[maybe_unused]] constexpr uint64_t kScanBytesMax = 1ull << 32;
// Runaway bounds for the container walk (§3.3), design limits neither. A corrupt
// element descriptor whose `inner` chain closed on itself would reinterpret element
// bytes as containers forever, and a container handle read out of bytes that are no
// longer the object's would name an arbitrary length. 2^28 containers is far past any
// real design, where each one costs a malloc'd payload, and 2^32 elements is 32 GiB
// of handles.
[[maybe_unused]] constexpr uint64_t kContainerWorkMax  = 1ull << 28;
[[maybe_unused]] constexpr uint64_t kContainerElemsMax = 1ull << 32;
// Runaway bound on how many live activations may ref into ONE heap-resident container
// (ContainerReg::refs), a design limit no more than the others: it takes one suspended
// activation holding a `ref` into the container per count, so 2^20 of them means the
// register / remove pairing has come apart rather than that a design is large.
[[maybe_unused]] constexpr uint32_t kContainerRefsMax = 1u << 20;

// Initial worklist capacity, in handles. A page-ish of marking stack covers a
// small testbench's object graph without a reallocation; deeper graphs grow it
// once and it is never shrunk, so steady state is allocation-free.
constexpr size_t kWorklistReserve = 4096;

// Floor for a nursery budget of zero, which would otherwise request a collection
// at nearly every allocation and silently duplicate --gc-stress.
constexpr uint64_t kNurseryBytesFloor = RT_GC_BLOCK_BYTES;

// Runaway bound for the remembered set, not a design limit -- the convention of
// kFrameScanMax above. A real set is thousands of entries, one per OLD object the
// barrier logged since the last collection, and the set is emptied by every
// collection. Reaching this means the barrier is logging without the collection that
// drains it ever running, so recording STOPS here and the next collection is forced
// full -- which traces everything and needs no remembered set at all, which is what
// makes dropping an entry sound. 2^20 entries is 8 MiB of vector.
constexpr size_t kRememberedMax = 1u << 20;

// The heap-growth rule that picks the kind when cfg.full_every is 0 (§7). A young
// collection cannot reclaim old garbage, so old garbage grows monotonically between
// full collections; capping the heap at kFullGrowthFactor times the live bytes the
// LAST FULL collection measured bounds floating garbage at a constant factor of the
// live set and amortises full-collection cost to O(live) per doubling -- the
// growth-factor argument, and the shape of BDW's GC_free_space_divisor.
//
// The floor is not a tuning knob but a correctness-of-policy fix: the first chunk is
// committed before anything can be live, so twice the live bytes of a design holding
// a handful of objects is less than the heap will ever be and every collection would
// be full, which is exactly the non-generational collector this phase replaces.
constexpr uint64_t kFullGrowthFactor = 2;
constexpr uint64_t kFullGrowthFloor  = kChunkBytes;

// How many nursery budgets of allocation, with no safepoint reached, make an
// allocation collect from inside itself (§6.1). The first budget only raises the
// request flag, which a region boundary answers, so a design that suspends every so
// often never reaches this. A design that does not suspend is bounded by this many
// budgets of garbage above its live set, and each emergency collection is amortised
// over that much allocation. It is larger than one on purpose: an emergency collection
// is the only one that runs with JIT frames live, so it is taken only when the
// alternative is the heap growing without bound, and a design whose live set is
// legitimately large and growing should not pay for it more often than the generational
// policy would collect anyway.
constexpr uint64_t kEmergencyFactor = 4;

// A native stack word below this is not a pointer into a heap buffer: a null, a small
// integer, a flag, a length. Keeping them out of the set the buffer frees consult
// (buffers_park) is what keeps that set the size of the stack's pointers.
constexpr uintptr_t kInFlightWordMin = 4096;

// Runaway bound on the stack an emergency collection will scan, not a design limit: a
// stack deeper than this is a design recursing without end, and a "stack" larger
// than this between the recorded base and the allocating frame means the allocation is
// on a stack the collector was not told about.
constexpr uint64_t kStackScanMax = 1ull << 30;

static_assert((kChunkBytes & kChunkMask) == 0, "a chunk size is a power of two");
static_assert((1u << kGranuleShift) == RT_GC_ALIGN, "the granule shift is log2(RT_GC_ALIGN)");
static_assert(RT_GC_ALIGN >= alignof(void*), "a handle word must be naturally aligned");
static_assert(RT_GC_BLOCK_BYTES % RT_GC_ALIGN == 0, "a block is whole granules");
static_assert(kGranulesPerBlock == 2048, "2048 metadata bytes per block");
static_assert(RT_GC_META_OFFSET + (kChunkBytes >> kGranuleShift) <= kFirstBlockOffset,
              "the chunk record and the metadata end before the first block");
static_assert(kFirstBlockOffset % RT_GC_BLOCK_BYTES == 0, "blocks sit on block boundaries");
static_assert(kBlocksPerChunk == 119, "119 blocks per 4 MiB chunk");
static_assert(RT_GC_META_OFFSET + (RT_GC_LARGE_PAYLOAD >> kGranuleShift) < RT_GC_LARGE_PAYLOAD,
              "a large object's metadata byte lies before the object");
static_assert(RT_GC_LARGE_PAYLOAD % RT_GC_ALIGN == 0, "a large object is granule-aligned");
static_assert((RT_GC_META_START & (kMetaFlags | RT_GC_META_BACK_MAX)) == 0,
              "START is no flag and no back distance");
static_assert(kLargeObjectBytes % RT_GC_ALIGN == 0, "the large threshold is granule-aligned");
static_assert(kLargeObjectBytes <= RT_GC_BLOCK_BYTES, "a block-resident object fits in a block");

// -- Heap structures ----------------------------------------------------------

// Block states. A block is in exactly one of the collector's lists (the free list, or
// the partial list of its run class), owned by a mutator, or full.
enum : uint8_t {
    kBlockFree    = 0,  // no object: every metadata byte is zero
    kBlockPartial = 1,  // some objects and some free granules
    kBlockFull    = 2,  // no free granule
    kBlockOwned   = 3,  // handed to a mutator; in neither list until it is swept
};

// A partial block's longest free run is 1 to 2047 granules -- 2048 would be a free
// block -- and its class is the power of two at or below it.
constexpr uint32_t kRunClasses = 11;
static_assert((1u << kRunClasses) == kGranulesPerBlock, "one class per power of two below a block");

uint32_t run_class(uint32_t run) {
    assert(run >= 1 && run < kGranulesPerBlock && "a partial block's longest run");
    return static_cast<uint32_t>(std::bit_width(run)) - 1;
}

// What the collector keeps per block, in the chunk's record.
struct BlockInfo {
    uint16_t cursor;  // first granule the allocator has not yet considered
    uint16_t used;    // granules holding an object, as of the last sweep
    // The longest free run, in granules, as of the last sweep: exact for a block in a
    // list, stale once a mutator has allocated from it. It decides which partial list
    // the block is in, so a request finds a block that fits without reading metadata.
    uint16_t max_run;
    uint8_t  state;  // one of the kBlock* states above
    // Allocated from since the last collection, so it is in g.young and a young
    // collection sweeps it. The only blocks that can hold an unmarked object.
    uint8_t  young;
};

// The record at the base of every mapping the heap owns: a chunk of blocks, or one
// large object. It fills the bytes before the metadata, so it must stay under
// RT_GC_META_OFFSET.
struct ChunkRecord {
    // 0 for a chunk of blocks, else the byte size of the one large object here.
    uint64_t  large_bytes;
    // Where the collector lists this mapping: an index into g.chunks or g.large.
    uint32_t  slot;
    uint32_t  blocks_free;
    BlockInfo block[kBlocksPerChunk];
};
static_assert(sizeof(ChunkRecord) <= RT_GC_META_OFFSET, "the chunk record ends before the metadata");
static_assert(std::is_trivially_copyable_v<ChunkRecord>, "a chunk record lives in raw memory");

// A block, as the lists name one: its chunk and its index there.
struct BlockRef {
    ChunkRecord* chunk;
    uint32_t     index;

    BlockInfo& info() const { return chunk->block[index]; }
    uint8_t*   base() const {
        return reinterpret_cast<uint8_t*>(chunk) + kFirstBlockOffset +
               static_cast<uint64_t>(index) * RT_GC_BLOCK_BYTES;
    }
    // The metadata of the block's first granule; kGranulesPerBlock bytes follow.
    uint8_t* meta() const {
        return reinterpret_cast<uint8_t*>(chunk) + RT_GC_META_OFFSET +
               ((kFirstBlockOffset + static_cast<uint64_t>(index) * RT_GC_BLOCK_BYTES) >> kGranuleShift);
    }
    bool operator==(const BlockRef&) const = default;
};

// A runtime-owned region scanned conservatively: the NBA and intra-assignment
// staging pools hold byte images that may contain a handle and are described by no
// emitted root table (Gc.h:202-205).
struct Range {
    const uint8_t* base;
    uint64_t       bytes;
};

// One NBA staging queue that can hold a handle (Gc.h, rt_gc_nba_words_add). `ops` is null
// for a queue whose records are scanned as words, else the descriptor of the container
// image at `offset` of every record, of container kind `kind`.
struct NbaQueue {
    uint32_t           idx;
    uint32_t           stride;
    uint32_t           offset;
    uint32_t           kind;
    const rt_elem_ops* ops;
};

// One pending handle-bearing container. `ops` is the descriptor of the CONTAINER
// type -- kind rt_elem_dynarr / rt_elem_queue / rt_elem_assoc -- so `ops->inner` is
// the element descriptor and `ops->keyshape` says where the handles are inside one
// normalized associative KEY span (Runtime.h, rt_key_*). That is what makes a registered
// root, a class-body leaf and a nested container element the same triple, and it keeps
// the key shape off the element descriptor, which the backend memoizes by element TYPE
// and would therefore share between an `int m[C]` and an `int d[]`. `kind` is the ABI's
// own copy of ops->kind, kept so the two can be asserted equal at both ends of the
// worklist.
struct PendingContainer {
    void*              handle;
    uint32_t           kind;
    const rt_elem_ops* ops;
};

// A dynamically-scoped container registration: a frame local, or an element of one,
// keyed by the address of its rt_dynarr / rt_queue / rt_assoc handle.
struct ContainerReg {
    uint32_t           kind;
    const rt_elem_ops* ops;
    // The heap object whose body or payload the registered container lives in, or null
    // when the container's storage is not in the heap at all -- which is every frame-local
    // registration, whose rt_dynarr / rt_queue / rt_assoc is a coroutine-frame alloca.
    //
    // Non-null for the one shape that needs it: a `ref` bound to an ELEMENT of a container
    // owned by a class property (`obj.q[3]`, or `obj.qq[3]` for a queue of queues). That
    // actual is a pointer INTO the outer container's malloc'd payload, which
    // rt_gc_find_object rejects as not-in-a-block, so the conservative frame scan cannot
    // resolve it to its owner the way it resolves a `ref` to a direct property. The
    // registration therefore stands in for the resolution the frame scan cannot do, and it
    // must do BOTH halves of it: reach the elements, and keep the owner alive
    // (push_container_roots). Without the second half a registration that outlives its
    // owner -- a task disabled mid-flight never reaches the removal -- would name a
    // finalized body and the next collection would read freed memory. With it, an unpaired
    // registration can only LEAK its owner, never dangle, which is the right side of that
    // trade to fail on.
    void*              owner;
    // How many live activations have registered THIS address. Always 1 for a frame-local
    // container, whose address is a per-activation alloca, so a second registration of one
    // is an init without its deinit and stays fatal. A heap-resident container's address is
    // a property of its OWNER, not of the activation, so two `ref` actuals into the same
    // container -- `t(obj.q[1], obj.q[2])`, or two forked processes each passing obj.q[3]
    // -- legitimately name the same key, and counting is the only correct answer: without
    // it the first removal would deregister a container a live activation still refs, and
    // in a release build the collector would then free what that activation pushes.
    //
    // Not a tolerated case but an EMITTED one, so the count cannot be optimised away later:
    // `t(obj.q[1], obj.q[2])` emits two adds and two removes on this one key in a single
    // call, and the backend deliberately does not fold them, because the two roots are
    // separate SSA values that merely happen to compute the same address and relying on CSE
    // to notice would be unsound exactly where it looked cleanest. So the COUNT is the
    // only thing that tells one live registration of a key from two.
    uint32_t           refs;
};

struct GcState {
    rt_gc_config cfg{};
    rt_gc_stats  stats{};
    int32_t  initialized = 0;
    // Per-reason collection counts for the shutdown stats line. Indexed by
    // RT_GC_REASON_*, which is why the array is exactly four long.
    uint64_t by_reason[4] = { 0, 0, 0, 0 };
    // The KIND of the collection now running (RT_GC_KIND_*): collection-scoped
    // state. A field rather than a parameter threaded through
    // mark_and_push -> trace_one -> drain, because the kind is tested in exactly ONE
    // place (mark_and_push) and every other marking function stays kind-agnostic:
    // one marking path parameterised by kind, never two copies (§7).
    uint32_t kind = RT_GC_KIND_FULL;
    // The remembered set: the OLD objects logged since the last collection, drained as
    // a root set at the start of the next one and cleared there. Per-mutator by
    // construction -- there is exactly one mutator, g_mutator below -- and it lives
    // here rather than in rt_gc_mutator for the reason hole_start does: no emitted
    // instruction touches it. The barrier's out-of-line half is a CALL, so the vector
    // needs no ABI-visible layout, and §6.2's multi-threaded milestone moves it into
    // the mutator beside frames_head when the mutator becomes per-thread.
    std::vector<void*> remembered;
    // Set when the remembered set hit kRememberedMax and stopped recording. The next
    // collection MUST then be full: a dropped entry is sound only because a full
    // collection traces everything, so this flag is the whole of that guarantee and is
    // cleared only by a full collection actually completing.
    int32_t force_full = 0;
    // stats.bytes_live as the last FULL collection measured it -- the whole live set,
    // which is the baseline the growth rule compares the heap against. A young
    // collection's bytes_live is only what it visited, so it must not update this.
    uint64_t live_after_full = 0;

    // -- Pressure and emergency collection (§6.1, §6.5) --
    //
    // Bytes charge_budget() has been handed since the last collection: every hole and every
    // large cell. rt_gc_mutator::budget_left is refilled each time it runs out, so it cannot
    // say how many budgets have been spent with no collection, and this can.
    uint64_t charged_since_gc = 0;
    // g_owned.live as the last collection, and the last FULL collection, left it. The first
    // is the baseline the growth of owned buffers is measured from; the second is what the
    // growth rule compares them against, as live_after_full is for the object heap.
    uint64_t owned_after_gc   = 0;
    uint64_t owned_after_full = 0;
    // The two levels of g_owned.live buffers_pressure() reacts to: one budget of growth
    // since the last collection, which requests one, and emergency_bytes of it, which
    // makes the next allocation take the slow path so it can run one.
    uint64_t owned_request_at   = UINT64_MAX;
    uint64_t owned_emergency_at = UINT64_MAX;
    // kEmergencyFactor budgets, saturated.
    uint64_t emergency_bytes = UINT64_MAX;
    // Where the native stack ends, for an emergency collection to scan up to. Null outside
    // rt_sim_run, and then allocation never collects.
    const uint8_t* stack_base = nullptr;
    // Where it starts, and non-zero only while an emergency collection is marking: the
    // address of a frame below every register the collector has spilled.
    uintptr_t stack_lo = 0;
    // Set for the length of rt_gc_alloc_held: the caller holds handles no collection
    // would find, so the allocation must not run one.
    int32_t held = 0;
    // Every word of the stack that is not a pointer into the object heap, sorted: the
    // candidates for a pointer into a malloc'd buffer the running code is still using.
    std::vector<uintptr_t> inflight;
    // A buffer an emergency collection's finalizer freed that one of those words lies in,
    // kept until a collection finds none does (buffers_park).
    struct Parked {
        void*    ptr;
        uint64_t bytes;
    };
    std::vector<Parked> parked;

    // Every mapping the heap owns, by the 4 MiB slot of the address space it covers:
    // one entry for a chunk of blocks, one per slot for a large object's mapping. This
    // is what says whether an address is in the heap at all; `heap_lo` / `heap_hi`
    // bound every mapping, so a word that is no heap address usually costs two
    // compares and no probe.
    ankerl::unordered_dense::map<uintptr_t, ChunkRecord*> table;
    uintptr_t heap_lo = UINTPTR_MAX;
    uintptr_t heap_hi = 0;
    // Each ChunkRecord::slot is its index in one of these, so removing a mapping is a
    // swap with the last one.
    std::vector<ChunkRecord*> chunks;  // chunks of blocks
    std::vector<ChunkRecord*> large;   // large objects, one mapping each
    // The mappings of dead large objects, kept for the next object of the same size,
    // and how many objects of that size were allocated since the last collection,
    // which is how many a full collection leaves (settle_spare_large). By object
    // size. A kept mapping is in neither `table` nor `large` and holds no object.
    struct LargeSize {
        std::vector<ChunkRecord*> spare;
        uint64_t                  taken = 0;
    };
    ankerl::unordered_dense::map<uint64_t, LargeSize> large_sizes;
    // The bytes of every large object's mapping, in use or kept.
    uint64_t large_mapped = 0;
    // Blocks an allocation can take: wholly free, and with some free granules, the
    // latter by the length of their longest free run -- partial_blocks[k] holds the
    // blocks whose longest run is 2^k to 2^(k+1)-1 granules, so every block in a list
    // at or above a request's class fits it and none is searched for. A block is in
    // at most one list, and in none while a mutator owns it or it is full. A young
    // collection updates the lists for the blocks it sweeps; a full one rebuilds them.
    std::vector<BlockRef> free_blocks;
    std::vector<BlockRef> partial_blocks[kRunClasses];
    // The blocks allocated from since the last collection (BlockInfo::young), and the
    // large objects allocated since then: everything a young collection sweeps.
    std::vector<BlockRef>     young;
    std::vector<ChunkRecord*> young_large;

    std::vector<const rt_gc_roots*> roots;  // borrowed: the JIT'd module owns them
    std::vector<Range>              ranges;

    // The block the mutator's free run is in, and where that run began. The accounting
    // the inlined fast path cannot do: it bumps `cursor` and writes metadata bytes,
    // but the mutator struct the backend mirrors holds no counters and adding one
    // would put a second store on the hot path to serve a shutdown stats line. So
    // bytes_allocated and objects_allocated are DERIVED when the run is retired, from
    // the cursor and from the START bytes over [hole_start, cursor) -- see
    // hole_consumed(). `hole_block.chunk` is null when the mutator has no run.
    BlockRef hole_block{ nullptr, 0 };
    uint8_t* hole_start = nullptr;

    std::vector<void*> worklist;  // the mark stack; never recursion
    std::vector<void*> pinned;    // collection-scoped pins, cleared at the end
    // The weak references this collection has marked (rt_gc_map::weak_referent), for
    // the pass that clears those whose referent it did not reach, or, in a collection
    // that keeps every referent (`weak_kept`), the pass that marks them.
    std::vector<void*> weak_reached;
    bool               weak_kept = false;

    // Containers of class handles (§3.3, Gc.h:226-268). `container_tables` is
    // borrowed exactly as `roots` is: the JIT'd module owns the constant.
    // `containers` is the dynamically-scoped registry, keyed by handle address so a
    // removal is a hash erase rather than a scan of every live activation's locals.
    // `container_work` is the container half of the mark fixpoint.
    std::vector<const rt_gc_container_table*>         container_tables;
    ankerl::unordered_dense::map<void*, ContainerReg> containers;
    std::vector<PendingContainer>                     container_work;
    // The NBA staging queues whose records can hold a handle. Registered once at start-up
    // by the emitted code; the records themselves are read from the scheduler at each
    // collection, since the buffers grow and move.
    std::vector<NbaQueue> nba_queues;
    // Identity numbers (rt_gc_identity), kept beside the heap so that an object pays for
    // one only once something asks. The two maps are each other's inverse.
    // `identity_fresh` lists the objects numbered since the last collection: the only
    // numbered ones a young collection can find dead, so it need not read the maps.
    ankerl::unordered_dense::map<const void*, uint64_t> identity;
    ankerl::unordered_dense::map<uint64_t, void*>       identified;
    std::vector<void*>                                  identity_fresh;
    uint64_t                                            identity_last = 0;
    // Set only for the duration of rt_gc_verify(), which reuses the container
    // walkers with marking replaced by an assertion, so the ring and associative
    // arithmetic has exactly one implementation and cannot drift from itself.
    int32_t verifying = 0;
};

GcState g;

// The single mutator. Reached only through rt_gc_mutator_current(); see the
// comment there for why the indirection must not be folded away by hand.
rt_gc_mutator g_mutator{ .cursor      = nullptr,
                         .limit       = nullptr,
                         .budget_left = 0,
                         .reserved    = 0,
                         .request     = 0,
                         .frames_head = nullptr };

// -- Loud failure -------------------------------------------------------------

// The shape of alloc_fatal (runtime/Reports.cpp) without its fmt styling and
// without a source location, for the callers that have no statement to name: a test
// calling rt_gc_alloc directly, and the `process` object the runtime mints for
// `process::self()`, which has no `new` in the source. Everything else matches,
// including the exit path, because the failure mode is the same one.
[[noreturn]] void gc_fatal(const char* headline, const char* note) {
    assert(headline != nullptr);
    std::fflush(stdout);
    std::fprintf(stderr, "fatal: %s\n", headline);
    std::fprintf(stderr, "<unknown>\n");
    if (note != nullptr) {
        std::fprintf(stderr, "  note: %s\n", note);
    }
    std::fprintf(stderr, "  note: at time %" PRIu64 "\n", rt_diag_now());
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as alloc_fatal (runtime/Reports.cpp): there is no defined
    // state to hand back mid-instant, and a nonzero status distinguishes this from
    // a clean run.
    std::exit(1);
}

// The fatal for an allocation that names its statement when it has one. alloc_fatal
// prints one headline, the source line and the time, so the detail goes in the
// headline in parentheses.
[[noreturn]] void alloc_failed(const rt_alloc_site* site, const char* headline, const char* note) {
    if (site == nullptr) {
        gc_fatal(headline, note);
    }
    std::string line = headline;
    line += " (";
    line += note;
    line += ")";
    rt_internal::alloc_fatal(site, rt_diag_now(), line);
}

// rt_gc_alloc must never return null: the ClassNew site elides the null guard on a
// fresh handle (ir/llvm/Inst.cpp, lower_inst), so a null return would be a silent
// wrong-code bug rather than a diagnosable one (Gc.h:189-192, design §4.3).
[[noreturn]] void oom_fatal(uint64_t size, const rt_alloc_site* site) {
    char head[192];
    std::snprintf(head, sizeof(head),
                  "out of memory for this class object: the collector could not obtain %" PRIu64
                  " bytes",
                  size);
    char note[192];
    std::snprintf(note, sizeof(note),
                  "the class heap holds %" PRIu64 " blocks of %u bytes, %" PRIu64 " of them free",
                  g.stats.blocks_total, RT_GC_BLOCK_BYTES, g.stats.blocks_free);
    alloc_failed(site, head, note);
}

// --gc-heap-max, enforced (§6.1): the heap is at its ceiling, an emergency collection
// reclaimed everything the design can no longer reach, and this object still does not
// fit. The live figure is what that collection measured, so the user sees how much of the
// ceiling is theirs.
[[noreturn]] void ceiling_fatal(uint64_t size, const rt_alloc_site* site) {
    char head[224];
    std::snprintf(head, sizeof(head),
                  "the class heap would pass --gc-heap-max (%" PRIu64
                  " bytes) allocating this object of %" PRIu64 " bytes",
                  g.cfg.heap_bytes_max, size);
    char note[224];
    if (g.stack_base == nullptr) {
        std::snprintf(note, sizeof(note),
                      "reached before the simulation started, where nothing can be reclaimed");
    } else if (g.held != 0) {
        std::snprintf(note, sizeof(note),
                      "reached while a container element was being constructed, where the "
                      "collector cannot run");
    } else {
        std::snprintf(note, sizeof(note),
                      "%" PRIu64 " bytes of objects are live after an emergency collection, and "
                      "the heap grows in %" PRIu64 "-byte chunks",
                      g.stats.bytes_live, kChunkBytes);
    }
    alloc_failed(site, head, note);
}

// -- Metadata and descriptor access -------------------------------------------

// The metadata byte of the granule at `a`: the arithmetic of Gc.h, and the same
// arithmetic generated code emits. `a` must lie in a mapping the heap owns.
uint8_t* meta_at(uintptr_t a) {
    return reinterpret_cast<uint8_t*>(a & ~static_cast<uintptr_t>(kChunkMask)) + RT_GC_META_OFFSET +
           ((a & kChunkMask) >> kGranuleShift);
}

uint8_t* meta_at(const uint8_t* p) {
    return meta_at(reinterpret_cast<uintptr_t>(p));
}

// -- Scanning metadata --------------------------------------------------------
//
// The collector reads metadata eight granules at a time wherever it can: a word that
// is all zero is free space, and the tests below pick out, in one word, the bytes
// that are zero, that start an object, or that start a marked or finalizable one.
// They are the usual word-at-a-time forms and depend on the byte order.
static_assert(std::endian::native == std::endian::little, "the metadata scans read words little-endian");
static_assert(RT_GC_META_START == 0x80 && RT_GC_META_MARK == 0x01 && RT_GC_META_FINALIZE == 0x08,
              "the word tests below shift START onto MARK and onto FINALIZE");

constexpr uint64_t kLowBits  = 0x0101010101010101ull;
constexpr uint64_t kHighBits = 0x8080808080808080ull;

uint64_t load_word(const uint8_t* p) {
    uint64_t w;
    std::memcpy(&w, p, sizeof w);
    return w;
}

// The bytes of `w` that start a marked object, as bit 0 of each.
uint64_t marked_starts(uint64_t w) {
    return w & (w >> 7) & kLowBits;
}

// The bytes of `w` that start an object owing a finalizer, as bit 3 of each.
uint64_t finalizable_starts(uint64_t w) {
    return w & (w >> 4) & (kLowBits << 3);
}

// First granule at or after `i` that is not free, or kGranulesPerBlock.
uint32_t skip_free(const uint8_t* meta, uint32_t i) {
    for (; i + 8 <= kGranulesPerBlock; i += 8) {
        const uint64_t w = load_word(meta + i);
        if (w != 0) {
            return i + (static_cast<uint32_t>(std::countr_zero(w)) >> 3);
        }
    }
    while (i < kGranulesPerBlock && meta[i] == 0) {
        ++i;
    }
    return i;
}

// First granule at or after `i` that is free, or kGranulesPerBlock.
uint32_t skip_used(const uint8_t* meta, uint32_t i) {
    for (; i + 8 <= kGranulesPerBlock; i += 8) {
        const uint64_t w = load_word(meta + i);
        // The high bit of a byte comes out set where the byte is zero. The subtraction
        // borrows only out of a zero byte, so nothing below the first one is disturbed,
        // and the first is the one taken.
        const uint64_t zero = (w - kLowBits) & ~w & kHighBits;
        if (zero != 0) {
            return i + (static_cast<uint32_t>(std::countr_zero(zero)) >> 3);
        }
    }
    while (i < kGranulesPerBlock && meta[i] != 0) {
        ++i;
    }
    return i;
}

// First granule at or after `i` that is free or starts an object, or
// kGranulesPerBlock: where the object covering granule `i - 1` ends.
uint32_t skip_object(const uint8_t* meta, uint32_t i) {
    for (; i + 8 <= kGranulesPerBlock; i += 8) {
        const uint64_t w = load_word(meta + i);
        // As in skip_used, with the bytes that have START already set joining the zero ones.
        const uint64_t edge = ((w - kLowBits) | w) & kHighBits;
        if (edge != 0) {
            return i + (static_cast<uint32_t>(std::countr_zero(edge)) >> 3);
        }
    }
    while (i < kGranulesPerBlock && meta[i] != 0 && (meta[i] & RT_GC_META_START) == 0) {
        ++i;
    }
    return i;
}

// How many objects start in the `n` granules at `meta`.
uint64_t count_starts(const uint8_t* meta, uint64_t n) {
    uint64_t count = 0;
    uint64_t i     = 0;
    for (; i + 8 <= n; i += 8) {
        count += static_cast<uint64_t>(std::popcount(load_word(meta + i) & kHighBits));
    }
    for (; i < n; ++i) {
        count += meta[i] >> 7;
    }
    return count;
}

// Zero `n` metadata bytes. Most objects are a few granules, and for those a call
// costs more than the stores: two stores that may overlap cover up to 16 bytes.
void zero_run(uint8_t* p, uint32_t n) {
    assert(n >= 1 && "a run to zero is not empty");
    if (n >= 16) {
        std::memset(p, 0, n);
    } else if (n >= 8) {
        const uint64_t z = 0;
        std::memcpy(p, &z, sizeof z);
        std::memcpy(p + n - sizeof z, &z, sizeof z);
    } else if (n >= 4) {
        const uint32_t z = 0;
        std::memcpy(p, &z, sizeof z);
        std::memcpy(p + n - sizeof z, &z, sizeof z);
    } else if (n >= 2) {
        const uint16_t z = 0;
        std::memcpy(p, &z, sizeof z);
        std::memcpy(p + n - sizeof z, &z, sizeof z);
    } else {
        p[0] = 0;
    }
}

// An object's metadata byte: the byte of its first granule, which holds its flags.
uint8_t* meta_ptr(void* handle) {
    assert(handle != nullptr);
    assert(reinterpret_cast<uintptr_t>(handle) % RT_GC_ALIGN == 0 && "a handle is granule-aligned");
    uint8_t* m = meta_at(reinterpret_cast<uintptr_t>(handle));
    assert((*m & RT_GC_META_START) != 0 && "a handle names the first granule of an object");
    return m;
}

// The record of the mapping a handle lies in.
ChunkRecord* record_of(void* handle) {
    return reinterpret_cast<ChunkRecord*>(reinterpret_cast<uintptr_t>(handle) &
                                          ~static_cast<uintptr_t>(kChunkMask));
}

// -- The one mark test --------------------------------------------------------
//
// Whether the collection now running may free an object: not while MARK is set. In a
// young collection a set mark is an old object the collection left alone or a young
// one it reached; in a full collection, which cleared every mark before it started,
// it is an object the collection reached. Marking, both sweeps, the identity table,
// the event records and rt_gc_verify() all ask here, because a second opinion about
// liveness is how a live object gets freed.
bool is_marked_now(void* handle) {
    return (*meta_ptr(handle) & RT_GC_META_MARK) != 0;
}

// Whether the object is old, which is the same bit read outside a collection: it has
// been marked, so a young collection will not trace it and the write barrier must
// report a store into it.
[[maybe_unused]] bool is_old(void* handle) {
    return (*meta_ptr(handle) & RT_GC_META_MARK) != 0;
}

// Non-asserting descriptor read: returns null when the vptr has not been stored
// yet. Generated code stores it immediately after rt_gc_alloc returns
// (ir/llvm/Inst.cpp, lower_inst) and nothing allocates in between, so the collector never
// sees this -- but rt_gc_find_object is also the conservative-scan validator and
// must not dereference through a zero word to find that out.
const rt_gc_map* map_probe(void* handle) {
    assert(handle != nullptr);
    const uint8_t* vptr = *reinterpret_cast<const uint8_t* const*>(handle);
    if (vptr == nullptr) {
        return nullptr;
    }
    const uint8_t* slot = vptr + static_cast<ptrdiff_t>(rt_gc_map_vptr_offset());
    return *reinterpret_cast<const rt_gc_map* const*>(slot);
}

// Split assertions over one map (TigerStyle). Field tests only, so they stay live in
// every build and the marking path calls this freely.
void assert_map([[maybe_unused]] const rt_gc_map* map) {
    assert(map != nullptr && "every class emits a clotho.classgc constant");
    assert(map->size >= sizeof(void*) && "an object holds at least its vptr");
    assert(map->size % RT_GC_ALIGN == 0 && "map->size is rounded up to RT_GC_ALIGN");
    assert((map->nhandles == 0 || map->handles != nullptr) && "nhandles implies a table");
    assert((map->nleaves == 0) == (map->leaves == nullptr) && "nleaves implies a table");
    // A managed property is exactly what puts a leaf in this table AND what makes the
    // class emit clotho.classfini, so a leaf table without a finalizer means the two
    // emissions disagree.
    assert((map->nleaves == 0 || map->finalize != nullptr) && "managed leaves imply a finalizer");
    // Offset 0 is the vptr, so 0 can mean "not a weak reference" and "no notifier".
    assert((map->weak_referent == 0 ||
            (map->weak_referent % sizeof(void*) == 0 && map->weak_referent + sizeof(void*) <= map->size)) &&
           "a weak referent is an aligned word of the body");
    assert((map->weak_notify == 0 || (map->weak_referent != 0 && map->weak_notify % sizeof(void*) == 0 &&
                                      map->weak_notify + sizeof(void*) <= map->size)) &&
           "a weak notifier is an aligned word of a weak reference's body");
}

// The two loads generated code does (Gc.h:70-73): the vptr at handle[0], then the
// map pointer at a design-wide constant negative offset from it.
const rt_gc_map* map_of(void* handle) {
    const rt_gc_map* map = map_probe(handle);
    assert(map != nullptr && "a traced object has its vptr stored");
    assert_map(map);
    return map;
}

// -- Address lookup ----------------------------------------------------------

constexpr uint32_t kChunkShift = 22;
static_assert((1ull << kChunkShift) == kChunkBytes, "the chunk shift is log2 of its size");

// The mapping that covers `a`, or null: whether an address is in the heap at all. Two
// compares reject nearly every word that is not, and the rest cost one probe.
ChunkRecord* record_at(uintptr_t a) {
    if (a < g.heap_lo || a >= g.heap_hi) {
        return nullptr;
    }
    const auto it = g.table.find(a >> kChunkShift);
    return it == g.table.end() ? nullptr : it->second;
}

// The first granule of the object that the granule at `a` belongs to, by the metadata
// alone, or null when that granule is free. This is the half of rt_gc_find_object that
// also holds at allocation time, before generated code has stored the vptr.
uint8_t* object_start_of(uintptr_t a) {
    ChunkRecord* const c = record_at(a);
    if (c == nullptr) {
        return nullptr;  // outside the heap: a stack word, a string, a chandle
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(c);
    if (c->large_bytes != 0) {
        // One object per mapping, so its start is known and only its extent is tested.
        const uintptr_t start = base + RT_GC_LARGE_PAYLOAD;
        if (a < start || a - start >= c->large_bytes) {
            return nullptr;
        }
        return reinterpret_cast<uint8_t*>(start);
    }
    if ((a & kChunkMask) < kFirstBlockOffset) {
        return nullptr;  // the chunk's own record and metadata
    }
    const uint8_t* const meta = reinterpret_cast<const uint8_t*>(c) + RT_GC_META_OFFSET;
    uint64_t             gran = (a - base) >> kGranuleShift;
    uint8_t              b    = meta[gran];
    if (b == 0) {
        return nullptr;  // a free granule
    }
    // Each step moves back at least one granule inside the object, and an object lies
    // in one block, so the steps are bounded by a block's granules.
    [[maybe_unused]] uint32_t steps = 0;
    while ((b & RT_GC_META_START) == 0) {
        assert(b <= RT_GC_META_BACK_MAX && "a later granule holds a distance back");
        assert(++steps <= kGranulesPerBlock && "the way back to an object's start does not end");
        gran -= b;
        b = meta[gran];
        assert(b != 0 && "a later granule of an object leads back to its first");
    }
    return reinterpret_cast<uint8_t*>(base + (gran << kGranuleShift));
}

// -- Heap growth --------------------------------------------------------------

// Enter a mapping of `bytes` in the table, under every 4 MiB slot it covers.
void table_add(ChunkRecord* c, uint64_t bytes) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(c);
    const uintptr_t hi = lo + bytes;
    assert((lo & kChunkMask) == 0 && "a mapping is chunk-aligned");
    for (uintptr_t slot = lo >> kChunkShift; slot <= (hi - 1) >> kChunkShift; ++slot) {
        [[maybe_unused]] const bool fresh = g.table.emplace(slot, c).second;
        assert(fresh && "two mappings cover one slot of the address space");
    }
    g.heap_lo = lo < g.heap_lo ? lo : g.heap_lo;
    g.heap_hi = hi > g.heap_hi ? hi : g.heap_hi;
}

// The bounds are left as they are: they only have to contain every mapping.
void table_remove(ChunkRecord* c, uint64_t bytes) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(c);
    const uintptr_t hi = lo + bytes;
    for (uintptr_t slot = lo >> kChunkShift; slot <= (hi - 1) >> kChunkShift; ++slot) {
        [[maybe_unused]] const size_t gone = g.table.erase(slot);
        assert(gone == 1 && "a mapping being released was in the table");
    }
}

// Give a large object's mapping, in use or kept, back to the system.
void unmap_large(ChunkRecord* c) {
    const uint64_t bytes = RT_GC_LARGE_PAYLOAD + c->large_bytes;
    assert(g.large_mapped >= bytes && "a large mapping was counted when it was made");
    g.large_mapped -= bytes;
    clotho::rt::os_unmap(c, static_cast<size_t>(bytes));
}

// Give back every mapping kept for reuse. Returns the bytes that went.
uint64_t release_spare_large() {
    const uint64_t before = g.large_mapped;
    for (auto& entry : g.large_sizes) {
        for (ChunkRecord* c : entry.second.spare) {
            unmap_large(c);
        }
        entry.second.spare.clear();
    }
    return before - g.large_mapped;
}

// A fresh chunk-aligned mapping of `bytes`, or null. Memory the heap is keeping for no
// object is not a reason to report that the system has none, so the kept mappings go
// back before that is the answer.
void* map_region(uint64_t bytes) {
    void* raw = clotho::rt::os_map_aligned(static_cast<size_t>(bytes), static_cast<size_t>(kChunkBytes));
    if (raw == nullptr && release_spare_large() != 0) {
        raw = clotho::rt::os_map_aligned(static_cast<size_t>(bytes), static_cast<size_t>(kChunkBytes));
    }
    return raw;
}

// One chunk from the OS, or false. Aligned to its own size, which is what lets an
// address find its chunk and its metadata by arithmetic.
bool grow_heap() {
    void* raw = map_region(kChunkBytes);
    if (raw == nullptr) {
        return false;
    }
    assert((reinterpret_cast<uintptr_t>(raw) & kChunkMask) == 0 && "aligned allocation honoured");
    // A fresh mapping reads as zero, which is the initial state of all of it: every
    // granule free, every block kBlockFree with its cursor at 0 and not young.
    auto* const c  = static_cast<ChunkRecord*>(raw);
    c->slot        = static_cast<uint32_t>(g.chunks.size());
    c->blocks_free = kBlocksPerChunk;
    g.chunks.push_back(c);
    for (uint32_t i = 0; i < kBlocksPerChunk; ++i) {
        c->block[i].max_run = kGranulesPerBlock;
        g.free_blocks.push_back(BlockRef{ c, i });
    }
    table_add(c, kChunkBytes);
    g.stats.blocks_total += kBlocksPerChunk;
    g.stats.blocks_free += kBlocksPerChunk;
    return true;
}

// -- Allocation ---------------------------------------------------------------

// The invariant of the mutator's free run: it has none, or [cursor, limit) lies in
// the block g.hole_block names. Generated code bumps the cursor and writes the
// metadata of what it allocates with no bounds check of its own, so every writer of
// these fields ends by calling this.
void assert_hole_consistent([[maybe_unused]] const rt_gc_mutator* mut) {
#ifndef NDEBUG
    assert(mut != nullptr && "the mutator context exists");
    if (mut->cursor == nullptr) {
        assert(mut->limit == nullptr && "no free run, no limit");
        assert(g.hole_block.chunk == nullptr && "no free run, no block");
        return;
    }
    assert(g.hole_block.chunk != nullptr && "a free run is in a block");
    const uint8_t* const base = g.hole_block.base();
    assert(g.hole_block.info().state == kBlockOwned && "the mutator owns the block of its run");
    assert(g.hole_block.info().young != 0 && "a block being allocated from is young");
    assert(mut->cursor >= base && "the run starts inside the block");
    assert(mut->cursor <= mut->limit && "the run is well-formed");
    assert(mut->limit <= base + RT_GC_BLOCK_BYTES && "the run ends inside the block");
    assert(reinterpret_cast<uintptr_t>(mut->cursor) % RT_GC_ALIGN == 0 && "the cursor is aligned");
#endif
}

// What the current free run has handed out: the bytes the cursor advanced, and the
// objects that start in them. DERIVED rather than counted at allocation time because
// the inlined fast path bumps the cursor and writes metadata but cannot reach g.stats
// -- see GcState::hole_start. Side-effect free, so rt_gc_get_stats() can fold a run in
// use into a mid-run read without committing it.
void hole_consumed(const rt_gc_mutator* mut, uint64_t* bytes, uint64_t* objects) {
    assert(bytes != nullptr && objects != nullptr && "the two counters have destinations");
    assert_hole_consistent(mut);
    *bytes   = 0;
    *objects = 0;
    if (mut->cursor == nullptr) {
        assert(g.hole_start == nullptr && "no run, and yet a recorded start");
        return;
    }
    assert(g.hole_start != nullptr && "a run in use has a recorded start");
    assert(mut->cursor >= g.hole_start && "the bump cursor never walks backwards");
    *bytes = static_cast<uint64_t>(mut->cursor - g.hole_start);
    // Every granule of a free run was zero when the run was handed out, so each START
    // over [hole_start, cursor) is an object this run allocated.
    const uint64_t n = *bytes >> kGranuleShift;
    assert(n <= kGranulesPerBlock && "a run lies in one block");
    *objects = count_starts(meta_at(g.hole_start), n);
}

// Commit what the run handed out, then forget it. Reached wherever a run stops being
// the mutator's: when the next is handed out, at a collection, and at shutdown.
void retire_hole(rt_gc_mutator* mut) {
    uint64_t bytes   = 0;
    uint64_t objects = 0;
    hole_consumed(mut, &bytes, &objects);
    g.stats.bytes_allocated += bytes;
    g.stats.objects_allocated += objects;
    g.hole_start = nullptr;
    assert(g.stats.bytes_allocated >= bytes && "the byte counter wrapped");
    assert(g.stats.objects_allocated >= objects && "the object counter wrapped");
}

// Give up the run entirely: its block is about to be swept, or the collector is going
// away. The block stays kBlockOwned and young, so the next sweep reclassifies it.
void drop_hole(rt_gc_mutator* mut) {
    retire_hole(mut);
    mut->cursor  = nullptr;
    mut->limit   = nullptr;
    g.hole_block = BlockRef{ nullptr, 0 };
    assert_hole_consistent(mut);
    assert(g.hole_start == nullptr && "a dropped run leaves no start behind");
}

// Charge the nursery budget for bytes MADE AVAILABLE, once per hole handed out
// (Gc.h:247-253). Per hole rather than per object is what keeps the inlined fast path
// free of budget arithmetic -- it bumps and stores, and nothing else -- while leaving
// the accounting exact in the quantity a nursery was always about: a span of heap, not
// a count of objects.
void charge_budget(rt_gc_mutator* mut, uint64_t bytes) {
    assert(g.cfg.nursery_bytes > 0 && "rt_gc_init floors the nursery budget");
    assert(bytes > 0 && "a charge of nothing means a hole of nothing was published");
    // The running total, which budget_left cannot be: it is refilled below each time it
    // runs out, and emergency_due() has to know how many times that has happened with no
    // collection in between. Cannot wrap: it is zeroed by every collection and bounded by
    // the address space between them.
    g.charged_since_gc += bytes;
    assert(g.charged_since_gc >= bytes && "the charge total wrapped");
    // budget_left is unsigned, so "goes non-positive" is a compare rather than a
    // subtract that would wrap into an enormous remaining budget.
    if (mut->budget_left > bytes) {
        mut->budget_left -= bytes;
        return;
    }
    // The writer of the poll byte outside stress mode, with buffers_pressure(): it is
    // reached only from publish_hole() and alloc_large(). That is what makes the inlined
    // poll free on a design that allocates no object: reset_mutator() zeroes `request` at
    // rt_gc_init, these are the sole paths that would raise it, so outside --gc-stress
    // every safepoint for the whole run is a load, a test and a not-taken branch
    // (Gc.h:157-161). Raising the byte is ALL that happens here: whether the allocation
    // itself collects is decided by emergency_due() at the top of alloc_object, once
    // kEmergencyFactor budgets have gone by with no region boundary to answer this one
    // (design §6.1).
    mut->request     = 1;
    mut->budget_left = g.cfg.nursery_bytes;
    assert(mut->budget_left > 0 && "a reset budget is never immediately spent");
}

// First run of free granules at or after `from` that is at least `size` bytes long,
// as [lo, hi) with hi at the run's end. Pure: probing a block must not disturb its
// cursor, or a request too large for a run would lose that run for the smaller
// requests that follow. `longest` is the longest run seen when none fits.
bool find_hole(BlockRef b, uint64_t size, uint32_t from, uint32_t* lo_out, uint32_t* hi_out,
               uint32_t* longest) {
    assert(size > 0 && size % RT_GC_ALIGN == 0);
    assert(from <= kGranulesPerBlock);
    const uint8_t* const meta = b.meta();
    const uint64_t       need = size >> kGranuleShift;
    uint32_t             i    = from;
    *longest                  = 0;
    // Each turn moves past one free run, so the search is one pass over the block.
    while (i < kGranulesPerBlock) {
        i = skip_used(meta, i);
        if (i == kGranulesPerBlock) {
            break;
        }
        const uint32_t end = skip_free(meta, i);
        if (end - i >= need) {
            *lo_out = i;
            *hi_out = end;
            return true;
        }
        *longest = end - i > *longest ? end - i : *longest;
        i        = end;  // too small for this request; skip the whole run
    }
    return false;
}

// Hand a free run to the mutator: its two fields, the block's cursor, the young list
// and the budget charge. The ONLY place the mutator's cursor is set to a non-null
// value, so it is the only place the invariant can be established.
void publish_hole(rt_gc_mutator* mut, BlockRef b, uint32_t lo, uint32_t hi) {
    assert(lo < hi && hi <= kGranulesPerBlock && "a run handed out is in its block");
    // Whatever the outgoing run handed out, count it before the cursor that measures
    // it is overwritten.
    retire_hole(mut);
    BlockInfo& info = b.info();
    assert(info.state == kBlockOwned && "a run is handed out of a block the mutator owns");
    info.cursor = static_cast<uint16_t>(hi);
    if (info.young == 0) {
        // From here on the block may hold an object no collection has seen, which is
        // what a young collection sweeps for.
        info.young = 1;
        g.young.push_back(b);
    }
    g.hole_block = b;
    mut->cursor  = b.base() + (static_cast<uint64_t>(lo) << kGranuleShift);
    mut->limit   = b.base() + (static_cast<uint64_t>(hi) << kGranuleShift);
    g.hole_start = mut->cursor;
    assert_hole_consistent(mut);
    assert(mut->cursor < mut->limit && "a run handed out is non-empty");
    charge_budget(mut, static_cast<uint64_t>(mut->limit - mut->cursor));
}

// Another run in the block this mutator already owns.
bool refill_hole(rt_gc_mutator* mut, uint64_t size) {
    const BlockRef b = g.hole_block;
    if (b.chunk == nullptr) {
        return false;
    }
    uint32_t lo      = 0;
    uint32_t hi      = 0;
    uint32_t longest = 0;
    if (!find_hole(b, size, b.info().cursor, &lo, &hi, &longest)) {
        return false;
    }
    publish_hole(mut, b, lo, hi);
    return true;
}

// Hand the mutator the first run of `b` that fits, `b` having just come off a list
// that guarantees one.
void take_block(rt_gc_mutator* mut, BlockRef b, uint64_t size) {
    BlockInfo& info = b.info();
    uint32_t   lo   = 0;
    uint32_t   hi   = kGranulesPerBlock;
    // A free block is one run of the whole block, with nothing to search for.
    if (info.state != kBlockFree) {
        uint32_t                    longest = 0;
        [[maybe_unused]] const bool found   = find_hole(b, size, info.cursor, &lo, &hi, &longest);
        assert(found && "a block's list promised a run this long");
    }
    info.state = kBlockOwned;
    publish_hole(mut, b, lo, hi);
}

bool acquire_block(rt_gc_mutator* mut, uint64_t size) {
    const uint32_t need = static_cast<uint32_t>(size >> kGranuleShift);
    assert(need >= 1 && need <= kGranulesPerBlock && "a block-resident request");
    // The space between survivors before a clean block, so that clean blocks stay
    // clean for the objects that need a long run and for giving back to the system.
    // The first class whose every block fits is the one whose shortest run, 2^k, is
    // at least the request. A block in the class below may fit too and is passed
    // over: taking it would mean searching that list for one that does.
    for (uint32_t k = static_cast<uint32_t>(std::bit_width(need - 1)); k < kRunClasses; ++k) {
        std::vector<BlockRef>& list = g.partial_blocks[k];
        if (list.empty()) {
            continue;
        }
        const BlockRef b = list.back();
        list.pop_back();
        assert(b.info().state == kBlockPartial && b.info().max_run >= need && "a listed block fits its class");
        take_block(mut, b, size);
        return true;
    }
    if (g.free_blocks.empty()) {
        return false;
    }
    const BlockRef b = g.free_blocks.back();
    g.free_blocks.pop_back();
    assert(b.info().state == kBlockFree && "the free list holds free blocks");
    assert(b.chunk->blocks_free > 0 && g.stats.blocks_free > 0 && "a free block was counted");
    b.chunk->blocks_free -= 1;
    g.stats.blocks_free -= 1;
    take_block(mut, b, size);
    return true;
}

uint8_t* bump(rt_gc_mutator* mut, uint64_t size) {
    assert(mut->cursor != nullptr && "a bump needs a free run");
    assert(size <= static_cast<uint64_t>(mut->limit - mut->cursor) && "the request fits");
    uint8_t* start = mut->cursor;
    mut->cursor    = start + size;
    assert(mut->cursor <= mut->limit && "a bump stayed inside the run");
    return start;
}

// -- Collection from inside an allocation (design §6.1) -----------------------

uint32_t choose_kind();  // the collection policy, below
uint64_t heap_bytes();   // likewise

// Whether the heap may take `bytes` more without passing --gc-heap-max, 0 meaning no
// ceiling. The heap grows in whole chunks, so a ceiling smaller than one is read as one:
// a design has to be able to allocate at all.
bool heap_may_grow(uint64_t bytes) {
    const uint64_t max = g.cfg.heap_bytes_max;
    if (max == 0) {
        return true;
    }
    const uint64_t ceiling = max < kChunkBytes ? kChunkBytes : max;
    return heap_bytes() + bytes <= ceiling;
}

// The same, having given back the mappings the heap keeps for no object if that is
// what it takes: they count against the ceiling, and must not be what refuses a
// design an object.
bool make_room(uint64_t bytes) {
    return heap_may_grow(bytes) || (release_spare_large() != 0 && heap_may_grow(bytes));
}

// How far past its budget the design has allocated since the last collection: the bytes
// charge_budget() was handed, plus how far the buffers its strings and containers own
// have grown (GcBuffers.h).
uint64_t pressure_since_gc() {
    const uint64_t grown = g_owned.live > g.owned_after_gc ? g_owned.live - g.owned_after_gc : 0;
    const uint64_t total = g.charged_since_gc + grown;
    return total < g.charged_since_gc ? UINT64_MAX : total;
}

// Whether the allocation now starting collects before it allocates. Never without a
// recorded stack to scan; always under --gc-stress, which is "collect at every
// opportunity"; otherwise once kEmergencyFactor budgets of allocation have gone by with
// no collection, the region boundary that normally answers the request flag never having
// come.
bool emergency_due() {
    if (g.stack_base == nullptr || g.held != 0) {
        return false;
    }
    if (g.cfg.stress != 0) {
        return true;
    }
    return pressure_since_gc() >= g.emergency_bytes;
}

// Collect with the registers and the stack still holding what the running code uses. The
// two functions are one idea in two frames, and both are noinline for it:
//
//   emergency_collect spills every callee-saved register into its own frame. The
//   allocation was reached from JIT code through a plain call, so a handle that code
//   keeps across it is either in a callee-saved register (spilled here, or by a frame
//   between, which is on the stack either way) or already on the stack.
//
//   collect_below_registers is called from it, so its frame is lower than that spill, and
//   the address of that frame is where the scan starts: everything from there up to the
//   base rt_sim_run recorded is the design's native stack.
//
// The result is used after the call so that call cannot be a tail call, which would pop the
// frame that holds the registers before the scan reads it.
[[gnu::noinline]] bool collect_below_registers(uint32_t kind) {
    const uintptr_t lo   = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    const uintptr_t base = reinterpret_cast<uintptr_t>(g.stack_base);
    if (lo >= base || base - lo > kStackScanMax) {
        // Not a stack rt_sim_run is above: a foreign thread entering through a DPI export,
        // say. There is no sound extent to scan, so there is no sound collection; the
        // allocation proceeds as it did before emergency collection existed.
        return false;
    }
    assert(g.stack_lo == 0 && "an emergency collection is already running");
    g.stack_lo = lo;
    rt_gc_collect_kind(RT_GC_REASON_EMERGENCY, kind);
    g.stack_lo = 0;
    return true;
}

[[gnu::noinline]] bool emergency_collect(uint32_t kind) {
    assert(g.stack_base != nullptr && "an emergency collection needs a stack to scan");
    __builtin_unwind_init();
    const bool collected = collect_below_registers(kind);
    asm volatile("" : : "r"(collected) : "memory");
    return collected;
}

// The heap is at --gc-heap-max with nothing to reuse: reclaim everything the design can no
// longer reach. Whether that made room is the caller's to find out by trying again, so
// that the one definition of "room" is the allocator's own.
void collect_for_room() {
    if (g.stack_base != nullptr && g.held == 0) {
        emergency_collect(RT_GC_KIND_FULL);
    }
}

// Three escalating steps, each strictly more expensive than the last. This is the body
// rt_gc_alloc_slow() exists for: the current hole could not satisfy the request.
uint8_t* refill_and_bump(rt_gc_mutator* mut, uint64_t size, const rt_alloc_site* site) {
    assert(size <= kLargeObjectBytes);
    assert(size % RT_GC_ALIGN == 0);
    if (refill_hole(mut, size)) {
        return bump(mut, size);
    }
    if (acquire_block(mut, size)) {
        return bump(mut, size);
    }
    // No block to reuse, so the heap has to grow -- unless --gc-heap-max says it may not,
    // which is the one place the ceiling is enforced. A collection can free blocks, and the
    // design may have been holding garbage; if even that leaves no room, the design needs
    // more than it was given and the run ends here, at this statement.
    if (!make_room(kChunkBytes)) {
        collect_for_room();
        if (refill_hole(mut, size) || acquire_block(mut, size)) {
            return bump(mut, size);
        }
        // No block came free, but the collection may have made room for a chunk: what
        // it freed was large objects.
        if (!make_room(kChunkBytes)) {
            ceiling_fatal(size, site);
        }
    }
    if (grow_heap() && acquire_block(mut, size)) {
        return bump(mut, size);
    }
    oom_fatal(size, site);
}

uint8_t* alloc_small(rt_gc_mutator* mut, uint64_t size, const rt_alloc_site* site) {
    assert(size <= kLargeObjectBytes);
    assert(mut->cursor <= mut->limit && "the bump range is well-formed");
    // The test generated code inlines (design §4.3). A null cursor fails it because
    // drop_hole() nulls `limit` alongside it, so the empty-hole case costs no second
    // branch here and none there.
    if (mut->cursor != nullptr && size <= static_cast<uint64_t>(mut->limit - mut->cursor)) {
        return bump(mut, size);
    }
    return refill_and_bump(mut, size, site);
}

// Write the metadata of an object of `size` bytes starting at `start`: START and its
// flag on the first granule, the distance back on each later one (Gc.h). The inlined
// fast path stores the same bytes, so the runtime's own entry point goes through the
// same arithmetic and a mistake in it cannot hide in JIT'd code alone.
void set_object_meta(uint8_t* start, uint64_t size, bool finalizable) {
    uint8_t* const meta = meta_at(start);
    const uint64_t n    = size >> kGranuleShift;
    assert(n >= 1 && n <= kGranulesPerBlock && "a block-resident object is 1 to 2048 granules");
#ifdef CLOTHO_EXTRA_ASSERTS
    // Negative space: a granule that is not free here means the bump cursor or the
    // sweep is wrong, and two objects would share it.
    for (uint64_t k = 0; k < n; ++k) {
        assert(meta[k] == 0 && "a new object's granules were free");
    }
#endif
    meta[0] = static_cast<uint8_t>(RT_GC_META_START | (finalizable ? RT_GC_META_FINALIZE : 0u));
    for (uint64_t k = 1; k < n; ++k) {
        meta[k] = static_cast<uint8_t>(k < RT_GC_META_BACK_MAX ? k : RT_GC_META_BACK_MAX);
    }
}

// A kept mapping for an object of `size` bytes, or null: where a dead object of that
// size was. Taking one maps nothing, which is what makes allocating large objects at
// a steady rate cost what the objects cost and not a system call and a page fault per
// page for each.
ChunkRecord* take_spare_large(uint64_t size) {
    const auto it = g.large_sizes.find(size);
    if (it == g.large_sizes.end() || it->second.spare.empty()) {
        return nullptr;
    }
    ChunkRecord* const c = it->second.spare.back();
    it->second.spare.pop_back();
    assert(c->large_bytes == size && "a kept mapping is listed under its object's size");
    return c;
}

// A mapping of its own, laid out as a chunk is: the record, the metadata, the object
// at RT_GC_LARGE_PAYLOAD. Only the object's first granule has a metadata byte anyone
// reads, and it is where the arithmetic of Gc.h puts it, so marking, the barrier and
// the sweep treat the object as any other. The table says which addresses are inside.
//
// Counted and charged HERE, per object, because a large allocation hands out no run:
// the per-run accounting every block-resident object rides on would never see it, and
// a design allocating only large objects would then never exhaust its nursery budget.
uint8_t* alloc_large(rt_gc_mutator* mut, uint64_t size, const rt_alloc_site* site) {
    assert(size > kLargeObjectBytes);
    assert(size % RT_GC_ALIGN == 0);
    const uint64_t bytes = RT_GC_LARGE_PAYLOAD + size;
    ChunkRecord*   c     = take_spare_large(size);
    if (c == nullptr && !make_room(bytes)) {
        collect_for_room();
        // The collection may have left a mapping of this size, or room for a new one.
        c = take_spare_large(size);
        if (c == nullptr && !make_room(bytes)) {
            ceiling_fatal(size, site);
        }
    }
    if (c == nullptr) {
        void* raw = map_region(bytes);
        if (raw == nullptr) {
            oom_fatal(size, site);
        }
        assert((reinterpret_cast<uintptr_t>(raw) & kChunkMask) == 0 && "aligned allocation honoured");
        // A fresh mapping reads as zero, so the record starts empty.
        c              = static_cast<ChunkRecord*>(raw);
        c->large_bytes = size;
        g.large_mapped += bytes;
    }
    // Of the metadata, a large object uses the byte of its first granule, which
    // alloc_object assigns; a kept mapping's was zeroed when its object died.
    g.large_sizes[size].taken += 1;
    c->slot = static_cast<uint32_t>(g.large.size());
    g.large.push_back(c);
    g.young_large.push_back(c);
    table_add(c, bytes);
    g.stats.bytes_allocated += size;
    g.stats.objects_allocated += 1;
    charge_budget(mut, size);
    return reinterpret_cast<uint8_t*>(c) + RT_GC_LARGE_PAYLOAD;
}

// THE allocation body. rt_gc_alloc() and rt_gc_alloc_slow() are this function behind
// two different sets of entry assertions, so the initialisation sequence -- granules,
// metadata, zeroed body -- exists once and cannot drift between the runtime's entry
// point and the one the inlined fast path falls through to.
void* alloc_object(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map,
                   const rt_alloc_site* site) {
    assert(mut == &g_mutator && "one mutator today; the pointer is the MT hook");
    assert(g.initialized != 0 && "allocation before rt_gc_init");
    assert_map(map);
    assert(size == map->size && "the backend passes map->size");

    // The one place an allocation collects (§6.1), and before it allocates rather than
    // after: the object about to exist cannot be found half-built, and what the caller
    // holds is no different from what it holds across any call it makes.
    if (emergency_due()) {
        emergency_collect(choose_kind());
    }
    const bool     large = size > kLargeObjectBytes;
    const bool     fin   = map->finalize != nullptr;
    uint8_t* const start = large ? alloc_large(mut, size, site) : alloc_small(mut, size, site);
    assert(start != nullptr && "the slow paths take a fatal rather than fail");
    assert(reinterpret_cast<uintptr_t>(start) % RT_GC_ALIGN == 0 && "an object start is aligned");

    // No MARK: the object is young, so the next collection traces it if it reaches it
    // and frees it if it does not.
    if (large) {
        *meta_at(start) = static_cast<uint8_t>(RT_GC_META_START | (fin ? RT_GC_META_FINALIZE : 0u));
    } else {
        set_object_meta(start, size, fin);
        // Under --gc-stress every allocation is an opportunity to collect, but the inlined
        // fast path never reaches the runtime while the run has room. Emptying the run
        // sends the next `new` here, where emergency_due() answers yes -- and only when it
        // can, so a design allocating before the run starts does not burn a block each.
        if (g.cfg.stress != 0 && g.stack_base != nullptr) {
            mut->limit = mut->cursor;
        }
    }
    // 2-state leaves and handle properties power on 0 / null (LRM Table 7-1). These
    // bytes are recycled heap, so this is required rather than merely tidy.
    std::memset(start, 0, static_cast<size_t>(size));

    // Validate-on-allocate, the pair half of the assertion in mark_and_push(). It is the
    // metadata resolution rather than the full rt_gc_find_object because generated code
    // stores the vptr only after this returns (ir/llvm/Inst.cpp, lower_inst), so the map
    // half of the pair is asserted at trace time instead.
    assert_resolves(object_start_of(reinterpret_cast<uintptr_t>(start)) == start);
    assert_resolves(object_start_of(reinterpret_cast<uintptr_t>(start) + size - 1) == start);
    return start;
}

// -- Marking ------------------------------------------------------------------

void pin(void* handle) {
    uint8_t* const m = meta_ptr(handle);
    if ((*m & RT_GC_META_PINNED) != 0) {
        return;  // already pinned this collection; the side list holds it once
    }
    *m |= RT_GC_META_PINNED;
    g.pinned.push_back(handle);
    g.stats.conservative_pins += 1;
}

// Queue a marked object for tracing and count it as visited. Marking at PUSH time
// rather than at pop is what bounds the worklist: an object enters it at most once per
// collection, so its length can never exceed objects_allocated.
void push_marked(void* handle) {
    assert(is_marked_now(handle) && "only a marked object is traced");
    const rt_gc_map* map = map_of(handle);
    g.stats.objects_live += 1;
    g.stats.bytes_live += map->size;
    g.worklist.push_back(handle);
    assert(g.worklist.size() <= g.stats.objects_allocated && "the mark stack outgrew the heap");
    if (map->weak_referent != 0) {
        g.weak_reached.push_back(handle);
    }
}

// Mark `handle` and queue it; do nothing if it is marked already -- reached earlier in
// this collection, or OLD in a young one.
//
// Marking is promotion: these are sticky mark bits, a survivor is old in place with
// nothing copied (§7), and surviving ONE collection is what makes an object old.
// stats.promoted counts the first time only, which kMetaWasOld tells apart from a full
// collection marking again an object whose mark it cleared.
//
// Leaving an OLD object alone in a young collection is the whole of generational
// marking, and it is sound because of two facts about how an object becomes old. An
// object is promoted exactly when a collection MARKS it, and marking pushes it, so the
// collection that promoted it also TRACED it: every reference the object held at that
// instant named an object the same collection marked, so every un-mutated out-edge of an
// OLD object names another OLD object, which no young collection frees. The mutator is
// stopped for the whole collection (design §6.1), so no store can slip in between
// "traced" and "promoted". And every reference written into the object AFTER that point
// takes the write barrier's taken edge -- `(meta & (MARK|LOGGED)) == MARK` -- so the
// object is in the remembered set and the next collection traces it from there, which
// covers stores from generated code and, through relog_conservative_roots(), the raw
// interior stores a suspended activation performs after the barrier that armed it has
// already been consumed.
//
// A third store shape stores no handle into a heap object at all: a class handle written
// into a CONTAINER owned by an old object lands in the container's malloc'd payload. A
// class handle CAN be a container element, or a handle inside an associative KEY
// (ir/llvm/Containers.cpp, elem_handle_offsets), so it is covered in two halves rather
// than assumed away. The backend logs the OWNING object at every container write helper:
// push_back / push_front / insert, whole-container assign, `new[n](src)`, slice append,
// and the put_ptr that writes a key whose shape is non-empty (assoc_key_is_class_handle).
// And the runtime traces a remembered object through the ORDINARY path --
// push_remembered_roots puts it on the same worklist every other object uses, so
// trace_one walks its handle offsets AND pushes its container leaves for the element
// walkers (§3.3). That is not incidental: a bespoke field walk over map->handles would
// trace `old.f = fresh` and lose `old.q.push_back(fresh)`, freeing an object a live queue
// still points at.
//
// The suspended-activation window is the same for a container held by `ref`, with ONE
// exception. A `ref` to a container that is a direct class property is a pointer to the
// rt_queue / rt_dynarr / rt_assoc header inside the owning object's BODY, so the frame
// scan resolves it to that object (rt_gc_find_object resolves interior pointers) and
// relog_conservative_roots() carries the obligation forward exactly as for a `ref` to a
// handle property. A `ref` to a NESTED container element -- `obj.qq[3]` where qq is a
// queue of queues -- is a pointer into the outer container's malloc'd payload
// (ir/llvm/StateLayout.cpp, ref_actual_ptr), which rt_gc_find_object correctly rejects as
// outside the heap, so nothing re-logs the owner and a callee that suspends and only then
// pushes is outside both mechanisms. It is closed at the call site instead, by
// registering the actual as a dynamically-scoped container root for the call's duration
// (rt_gc_container_add / rt_gc_container_remove), which needs nothing from this file,
// because a container ROOT is scanned in full at every collection whatever its kind.
//
// Should that registration ever be missing, the shape is LOUD rather than silent:
// verify_old_edges() walks an un-remembered old object's container leaves and asserts
// every element is one this collection would spare (is_marked_now), so an un-logged
// young element fails an assertion at the collection that would have freed it instead of
// at the use-after-free later.
//
// By induction over collections, an OLD object outside the remembered set has no
// out-edge this collection could reach that is not already covered. Skipping it loses
// nothing, and skipping it is what makes a young collection cost the young survivors
// instead of the whole component tree (§7).
void mark_and_push(void* handle) {
    assert(handle != nullptr);
    assert(g.kind == RT_GC_KIND_YOUNG || g.kind == RT_GC_KIND_FULL);
    // Pair half of the validate-on-allocate assertion in alloc_object: every handle the
    // marker follows must resolve through the heap's own metadata.
    assert_resolves(rt_gc_find_object(handle) == handle && "traced a handle the heap does not own");
    uint8_t* const m = meta_ptr(handle);
    if ((*m & RT_GC_META_MARK) != 0) {
        return;
    }
    *m |= RT_GC_META_MARK;
    if ((*m & kMetaWasOld) == 0) {
        g.stats.promoted += 1;
    }
    push_marked(handle);
}

// -- Handle-bearing containers (design §3.3, Gc.h:226-268) --------------------
//
// A container's payload is malloc'd, not GC-heap memory, so neither root path finds
// the handles inside it: a precise root table describes state-buffer words, and a
// conservative frame scan sees only a pointer to a ring that rt_gc_find_object
// correctly rejects because it is not in a block. A handle-bearing container is
// therefore traced FROM its handle, precisely, through its element descriptor.

bool is_container_kind(uint32_t kind) {
    return kind == rt_elem_dynarr || kind == rt_elem_queue || kind == rt_elem_assoc;
}

// Split assertions over one element descriptor, in the shape assert_map() uses for a
// trace map. Field tests only, so the walkers call it freely.
void assert_elem_ops([[maybe_unused]] const rt_elem_ops* ops) {
    assert(ops != nullptr && "every traced element has a descriptor");
    assert(ops->stride > 0 && "an element slot is at least one byte");
    assert((ops->nhandles == 0 || ops->handles != nullptr) && "nhandles implies a table");
    assert((ops->nleaves == 0 || ops->leaves != nullptr) && "nleaves implies a leaf table");
    assert((!is_container_kind(ops->kind) || ops->inner != nullptr) &&
           "a container descriptor names its element descriptor");
    // The key shape and its length are one fact stated twice (Runtime.h, rt_key_*), and
    // `nkeyshape != 0` is the SINGLE source of truth for "this array's keys carry a
    // handle" -- the same question the backend's barrier answers off the same program
    // (ir/llvm/AssocArrays.cpp, assoc_key_shape), so the barrier and the tracer cannot disagree.
    assert((ops->nkeyshape == 0) == (ops->keyshape == nullptr) &&
           "nkeyshape implies a key shape, and a key shape implies a length");
    assert((ops->nkeyshape == 0 || ops->kind == rt_elem_assoc) &&
           "only an associative array has key bytes to decode");
}

void push_container(void* handle, uint32_t kind, const rt_elem_ops* ops) {
    assert(handle != nullptr && "a container walk needs the container handle");
    assert(is_container_kind(kind) && "only a dynarr / queue / assoc is walked");
    assert_elem_ops(ops);
    assert(ops->kind == kind && "the kind and the container descriptor disagree");
    g.container_work.push_back(PendingContainer{ handle, kind, ops });
    assert(g.container_work.size() <= kContainerWorkMax && "the container walk outgrew its bound");
}

// One class-handle word read out of a container element image, or out of an
// associative array's key bytes.
//
// Resolution is asserted here as it is for a field in trace_one: every container this
// can reach is state-resident, registered live, or inside an object no collection has
// freed, so a non-resolving element means the descriptor is wrong rather than that the
// bytes are stale. It stays a skip in release because a misclassified element -- a chandle
// lowers to the same 64-bit plane (design §3.4) -- must not be traced either way.
void mark_container_handle(void* child) {
    if (child == nullptr) {
        return;  // a null element is the power-on default (LRM Table 7-1)
    }
    void* resolved = rt_gc_find_object(child);
    assert(resolved == child && "a container element is not an exact class handle");
    if (resolved == nullptr) {
        return;
    }
    if (g.verifying != 0) {
        // §13 for containers: the mark phase has already run, so every handle inside a
        // live container is one this collection would spare -- marked, or OLD in a young
        // collection, which is the same statement about liveness reached without a mark
        // (§7). One predicate for both, asserted, never re-marked.
        assert(is_marked_now(resolved) &&
               "a live container holds a handle this collection would not spare");
        return;
    }
    mark_and_push(resolved);
}

// One element image: the class handles inside it, then whatever it owns that has to
// be walked as a container in its own right.
//
// `handles` covers every handle reachable without leaving the image
// (Runtime.h:913-916). For an aggregate that includes the handles of any nested
// aggregate, because collect_elem_leaves (ir/llvm/Containers.cpp) flattens the
// leaf table through nested aggregates and emits only terminal leaves -- so no leaf
// is itself an aggregate and one pass over `leaves` is complete.
void visit_elem_image(uint8_t* image, const rt_elem_ops* ops) {
    assert(image != nullptr && "an element image has an address");
    assert_elem_ops(ops);
    for (uint32_t i = 0; i < ops->nhandles; ++i) {
        const uint32_t off = ops->handles[i];
        assert(off % sizeof(void*) == 0 && "a handle word is naturally aligned");
        assert(off + sizeof(void*) <= ops->stride && "a handle word is inside the image");
        mark_container_handle(*reinterpret_cast<void**>(image + off));
    }
    if (is_container_kind(ops->kind)) {
        // The image IS the nested container's handle (Runtime.h:902-905), so the
        // handles inside it are found by walking it and are never listed here.
        assert(ops->nhandles == 0 && "a nested container element lists no inline handle");
        push_container(image, ops->kind, ops);
        return;
    }
    if (ops->kind != rt_elem_aggregate) {
        return;  // pod, string, event: nothing further inside owns a class handle
    }
    for (uint32_t i = 0; i < ops->nleaves; ++i) {
        const rt_elem_leaf& leaf = ops->leaves[i];
        assert(leaf.ops != nullptr && "an aggregate leaf has a descriptor");
        assert(leaf.ops->kind != rt_elem_aggregate && "the leaf table is flattened");
        if (!is_container_kind(leaf.ops->kind)) {
            continue;  // a string or event leaf holds no class handle: skipped on kind
        }
        assert(leaf.offset + leaf.ops->stride <= ops->stride && "a leaf is inside the image");
        push_container(image + leaf.offset, leaf.ops->kind, leaf.ops);
    }
}

// One dynamic array: `len` element images at data + i * stride (Runtime.h:985-991).
void walk_dynarr(const rt_dynarr* h, const rt_elem_ops* cops) {
    assert(h != nullptr && "a dynamic-array walk needs its handle");
    assert(cops != nullptr && "a dynamic-array walk needs a container descriptor");
    assert(cops->kind == rt_elem_dynarr && "a dynarr walk needs a dynarr descriptor");
    assert((h->len == 0 || h->data != nullptr) && "a non-empty array owns its buffer");
    assert(h->len <= kContainerElemsMax && "an implausible dynamic-array length");
    // The element stride comes from `inner`, never from the container descriptor,
    // whose own stride is the 24-byte rt_dynarr handle slot.
    const rt_elem_ops* eops = cops->inner;
    assert_elem_ops(eops);
    uint8_t* data = static_cast<uint8_t*>(h->data);
    for (uint64_t i = 0; i < h->len; ++i) {
        visit_elem_image(data + i * eops->stride, eops);
    }
}

// One queue, in RING order: element i sits at base + rt_queue_slot(h, i) * stride,
// i.e. (head + i) mod cap (Runtime.h:1048-1056). A walk that ignored the
// wraparound would silently miss exactly the elements a push_front wrapped past
// the end of the buffer, and a reachable handle the collector misses is a
// use-after-free, not a lost element.
void walk_queue(const rt_queue* h, const rt_elem_ops* cops) {
    assert(h != nullptr && "a queue walk needs its handle");
    assert(cops != nullptr && "a queue walk needs a container descriptor");
    assert(cops->kind == rt_elem_queue && "a queue walk needs a queue descriptor");
    assert(h->len <= h->cap && "a queue holds no more than its ring");
    assert((h->cap == 0 || h->base != nullptr) && "a non-empty ring owns its buffer");
    assert((h->cap == 0 || h->head < h->cap) && "the head is a ring slot index");
    assert(h->cap <= kContainerElemsMax && "an implausible queue capacity");
    if (h->len == 0 || h->cap == 0) {
        return;  // empty; also keeps a zero capacity out of the ring arithmetic
    }
    const rt_elem_ops* eops = cops->inner;
    assert_elem_ops(eops);
    uint8_t* base = static_cast<uint8_t*>(h->base);
    for (uint64_t i = 0; i < h->len; ++i) {
        const uint64_t slot = rt_queue_slot(h, i);
        visit_elem_image(base + slot * eops->stride, eops);
    }
}

// One normalized key span being walked. Every read is checked against the end of the
// span and ends the run if it would pass it: the shape and the bytes come from two
// places (the backend's descriptor, the serializer's output), and a disagreement must
// not become a read of whatever follows the key.
struct KeyCursor {
    const uint8_t* at;
    const uint8_t* end;
};

void key_need(const KeyCursor& c, uint64_t bytes) {
    assert(c.at <= c.end && "a key cursor stays inside its span");
    if (static_cast<uint64_t>(c.end - c.at) < bytes) {
        gc_fatal("an associative array key is shorter than its shape says",
                 "the collector's key shape and the key serializer disagree");
    }
}

// A big-endian count or length, as emit_key_bytes wrote it.
uint32_t key_u32(KeyCursor& c) {
    key_need(c, sizeof(uint32_t));
    uint32_t v = 0;
    for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
        v = (v << 8) | c.at[i];
    }
    c.at += sizeof(uint32_t);
    return v;
}

// The handle in the next 8 key bytes.
//
// The ONE place the key byte order is undone. emit_key_bytes writes byte 0 as the most
// significant and applies no sign flip, a handle being unsigned
// (ir/llvm/AssocArrays.cpp, docs/design/assoc-keys.md §2), so the word is reassembled
// MSB-first. Byte-wise also because nothing about a key handle is aligned: the span is
// serialized leaf by leaf with no padding. The word is the object's identity number,
// not its address (rt_gc_identity): a key holds its object, so the number of a key in a
// live container names a live object, and one that names none is a fatal there.
void* key_handle(KeyCursor& c) {
    key_need(c, sizeof(uint64_t));
    uint64_t id = 0;
    for (uint32_t i = 0; i < sizeof(uint64_t); ++i) {
        id = (id << 8) | c.at[i];
    }
    c.at += sizeof(uint64_t);
    return rt_gc_identity_object(id);
}

// One level of a key walk in progress.
//   Run:  the words [pc, end) once. A nested key also has to end exactly at `stop`.
//   Loop: `body` .. `body + words`, `left` more times.
//   Map:  `left` more entries, each a length, a key and a value.
struct KeyFrame {
    enum class Kind : uint8_t { Run, Loop, Map };
    Kind            kind;
    uint32_t        left;
    const uint32_t* pc;
    const uint32_t* end;
    const uint8_t*  stop;
    const uint32_t* key;
    uint32_t        key_words;
};

// Nesting of containers and arrays inside one key type. A key type is finite, and no
// design nests one this deep; the bound makes a corrupt shape a fatal instead of a
// walk off the stack (TigerStyle).
constexpr uint32_t kKeyDepthMax = 64;

// Mark every handle in one normalized key span, by walking it as its shape says
// (Runtime.h, rt_key_*). An explicit stack and never recursion: a key may be a queue
// of structs of associative arrays, to any depth the type system allows.
void walk_key(const uint32_t* shape, uint32_t words, const uint8_t* key, uint32_t klen) {
    assert(shape != nullptr && words > 0 && "a key walk needs a shape");
    assert(key != nullptr && "a key walk needs the key bytes");
    KeyCursor c{ key, key + klen };
    KeyFrame  stack[kKeyDepthMax];
    uint32_t  depth = 0;
    const auto push  = [&](const KeyFrame& f) {
        if (depth == kKeyDepthMax) {
            gc_fatal("an associative array key nests deeper than the collector walks",
                     "the key shape is corrupt, or the key type nests more than 64 levels");
        }
        stack[depth++] = f;
    };
    const auto run = [&](const uint32_t* pc, const uint32_t* end, const uint8_t* stop) {
        push(KeyFrame{ KeyFrame::Kind::Run, 0, pc, end, stop, nullptr, 0 });
    };
    // An element or an entry is at least one byte, so a count beyond the bytes that are
    // left is a wrong shape. This is also what bounds every loop below.
    const auto counted = [&]() {
        const uint32_t n = key_u32(c);
        key_need(c, n);
        return n;
    };
    run(shape, shape + words, c.end);
    while (depth > 0) {
        KeyFrame& f = stack[depth - 1];
        if (f.kind == KeyFrame::Kind::Loop) {
            if (f.left == 0) {
                --depth;
                continue;
            }
            f.left -= 1;
            run(f.pc, f.end, nullptr);
            continue;
        }
        if (f.kind == KeyFrame::Kind::Map) {
            if (f.left == 0) {
                --depth;
                continue;
            }
            f.left -= 1;
            const uint32_t        key_bytes = counted();
            const uint8_t* const  key_end   = c.at + key_bytes;
            const KeyFrame        entry     = f;  // `push` may not be handed a reference into the stack
            run(entry.pc, entry.end, nullptr);
            if (entry.key_words == 0) {
                c.at = key_end;
            } else {
                run(entry.key, entry.key + entry.key_words, key_end);
            }
            continue;
        }
        if (f.pc == f.end) {
            if (f.stop != nullptr && c.at != f.stop) {
                gc_fatal("an associative array key does not end where its shape does",
                         "the collector's key shape and the key serializer disagree");
            }
            --depth;
            continue;
        }
        const uint32_t* const pc = f.pc;
        switch (pc[0]) {
            case rt_key_skip:
                key_need(c, pc[1]);
                c.at += pc[1];
                f.pc = pc + 2;
                break;
            case rt_key_handle:
                // Resolution of the decoded handle is asserted by mark_container_handle,
                // which is the pair half of this decode: a key handle read out of the
                // wrong bytes names no object, and one the collector fails to read at all
                // is a use-after-free that surfaces on the next `m.first(k)`.
                mark_container_handle(key_handle(c));
                f.pc = pc + 1;
                break;
            case rt_key_string:
                c.at += counted();
                f.pc = pc + 1;
                break;
            case rt_key_seq: {
                const uint32_t count = counted();
                f.pc                 = pc + 2 + pc[1];
                push(KeyFrame{ KeyFrame::Kind::Loop, count, pc + 2, pc + 2 + pc[1], nullptr, nullptr, 0 });
                break;
            }
            case rt_key_rep:
                f.pc = pc + 3 + pc[2];
                push(KeyFrame{ KeyFrame::Kind::Loop, pc[1], pc + 3, pc + 3 + pc[2], nullptr, nullptr, 0 });
                break;
            case rt_key_map: {
                const uint32_t        count = counted();
                const uint32_t* const kprog = pc + 3;
                const uint32_t* const vprog = kprog + pc[1];
                f.pc                        = vprog + pc[2];
                push(KeyFrame{ KeyFrame::Kind::Map, count, vprog, vprog + pc[2], nullptr, kprog, pc[1] });
                break;
            }
            default:
                gc_fatal("an associative array key shape holds an unknown word",
                         "the backend and the runtime disagree about rt_key_*");
        }
    }
    assert(c.at == c.end && "the outermost run checks that the shape consumed the key");
}

// The context the associative walk carries through the C callback: the descriptor of
// the ARRAY, so the callback can read both `inner` (the value element) and
// `keyshape` (where the handles are inside one normalized key span).
struct AssocWalk {
    const rt_elem_ops* cops;
};

// One live element image of an associative array, plus its normalized key bytes. A
// null `key` is the pattern default (LRM 7.9.11), a live element image with no key:
// reading any absent index yields it, so a handle inside it is a strong reference
// (design §9, "a handle in a live element of a traced container").
//
// A handle inside a KEY is a strong reference for the same reason and one the collector
// has to decode: the key bytes are the only copy of the handle, so `m[obj] = 1` keeps
// `obj` alive and `m.first(k)` hands it back for dereference
// (docs/design/garbage-collection.md §3.3a). A bare `int m[C]` is the one-word case of
// the same walk and not a path of its own (ir/llvm/AssocArrays.cpp, assoc_key_shape).
void assoc_visit_image(void* elem, const uint8_t* key, uint32_t klen, void* ctx) {
    assert(elem != nullptr && "a visited element image has an address");
    assert(ctx != nullptr && "the associative walk carries its descriptor");
    const rt_elem_ops* cops = static_cast<const AssocWalk*>(ctx)->cops;
    visit_elem_image(static_cast<uint8_t*>(elem), cops->inner);
    if (cops->nkeyshape == 0 || key == nullptr) {
        return;
    }
    assert(cops->kind == rt_elem_assoc && "only an associative array is keyed");
    walk_key(cops->keyshape, cops->nkeyshape, key, klen);
}

// One associative array. rt_assoc_visit is the only way in: the map is an
// unordered_dense of per-entry SlotVec blocks whose payloads relocate on rehash, so
// no external code can iterate it by address (Runtime.h:934-941).
void walk_assoc(const rt_assoc* h, const rt_elem_ops* cops) {
    assert(h != nullptr && "an associative walk needs its handle");
    assert(cops != nullptr && "an associative walk needs a container descriptor");
    assert(cops->kind == rt_elem_assoc && "an assoc walk needs an assoc descriptor");
    assert_elem_ops(cops->inner);
    AssocWalk ctx{ cops };
    rt_assoc_visit(h, &assoc_visit_image, &ctx);
}

// Drain the container worklist: an explicit worklist and never recursion
// (TigerStyle), because container nesting is arbitrary -- `C q[$][]` is a
// queue of dynamic arrays of handles -- and recursing through `inner` would trade a
// collector for a stack overflow on a shape the type system permits.
void drain_containers() {
    [[maybe_unused]] uint64_t pops = 0;
    while (!g.container_work.empty()) {
        const PendingContainer p = g.container_work.back();
        g.container_work.pop_back();
        ++pops;
        assert(pops <= kContainerWorkMax && "the container walk exceeded its bound");
        assert(p.kind == p.ops->kind && "the worklist kind and descriptor disagree");
        switch (p.kind) {
            case rt_elem_dynarr:
                walk_dynarr(static_cast<const rt_dynarr*>(p.handle), p.ops);
                break;
            case rt_elem_queue:
                walk_queue(static_cast<const rt_queue*>(p.handle), p.ops);
                break;
            case rt_elem_assoc:
                walk_assoc(static_cast<const rt_assoc*>(p.handle), p.ops);
                break;
            default:
                assert(false && "only a dynarr / queue / assoc reaches the worklist");
                break;
        }
    }
}

// Container roots. Which containers are roots is decided by where the container's own
// storage lives (Gc.h:234-244): design state and static class properties are roots
// through a registered table, a frame local is a root while its activation lives
// through rt_gc_container_add, and a class property is NOT a root -- it is reached
// through its owning object's leaf table in trace_one, so a container inside a dead
// object keeps nothing alive.
void push_container_roots() {
    for (const rt_gc_container_table* t : g.container_tables) {
        assert(t != nullptr && "a registered container table exists");
        assert(t->count < kRootHandlesMax && "an implausible container table size");
        if (t->count == 0) {
            continue;
        }
        assert(t->base != nullptr && "a non-empty container table has a base");
        assert(t->entries != nullptr && "a non-empty container table has entries");
        uint8_t* base = static_cast<uint8_t*>(t->base);
        for (uint32_t i = 0; i < t->count; ++i) {
            const rt_gc_container& e = t->entries[i];
            push_container(base + e.offset, e.kind, e.ops);
        }
        g.stats.container_roots += t->count;
    }
    // Dynamically-scoped containers: the registration IS the liveness claim, so there
    // is nothing further to validate here. A broken init / deinit pairing is caught
    // where it happens, by rt_gc_container_remove's assertion, rather than as a leak
    // or a use-after-free one collection later.
    for (const auto& kv : g.containers) {
        // The owner first, so the container this walk is about to read cannot be a payload
        // the finalizer of a dead object frees. A young collection legitimately declines to
        // trace an OLD owner (mark_and_push), and that is not a gap: no young collection
        // frees an old object, so it is intact either way.
        if (kv.second.owner != nullptr) {
            mark_and_push(kv.second.owner);
        }
        push_container(kv.first, kv.second.kind, kv.second.ops);
    }
    g.stats.container_roots += static_cast<uint64_t>(g.containers.size());
}

// Trace one marked object's handle fields.
void trace_one(void* handle) {
    const rt_gc_map* map  = map_of(handle);
    uint8_t*         body = static_cast<uint8_t*>(handle);
    for (uint32_t i = 0; i < map->nhandles; ++i) {
        const uint32_t off = map->handles[i];
        // Offsets are from the handle and name a whole aligned 64-bit word inside
        // the body (Gc.h:75-78); handles are bare, aligned, untagged words (F4).
        assert(off % sizeof(void*) == 0 && "a handle field is naturally aligned");
        assert(off + sizeof(void*) <= map->size && "field inside the body");
        void* child = *reinterpret_cast<void**>(body + off);
        if (child == nullptr) {
            continue;  // a null handle is the power-on default (LRM Table 7-1)
        }
        // A marked object's fields name objects, always: what a collection marks it also
        // traces, so nothing a marked object points at is freed while it is, and the
        // sweep erases a dead object whole. Even an object reached only from a stale
        // conservative word is one no collection has freed, with fields to match.
        assert_resolves(rt_gc_find_object(child) == child && "a handle field names an object");
        mark_and_push(child);
    }
    // The leaf table: this class's managed properties, by byte offset from the handle
    // (Gc.h:75-85). Only a container leaf can hold a class handle, and it is NOT a
    // root -- it is reached from here, which is what makes a container inside a dead
    // object keep nothing alive (Gc.h:236-244).
    for (uint32_t i = 0; i < map->nleaves; ++i) {
        const rt_elem_leaf& leaf = map->leaves[i];
        assert(leaf.ops != nullptr && "a class leaf has an element descriptor");
        assert(leaf.ops->kind != rt_elem_aggregate && "the leaf table is flattened");
        if (!is_container_kind(leaf.ops->kind)) {
            // A string or event property holds no class handle. Rejected on the kind
            // alone, before any dereference: such a leaf has nhandles 0 and a null
            // `inner`, so there is nothing here to walk.
            continue;
        }
        assert(leaf.offset + leaf.ops->stride <= map->size &&
               "a leaf is inside the body");
        push_container(body + leaf.offset, leaf.ops->kind, leaf.ops);
    }
}

// The object mark loop. An explicit worklist, never recursion (TigerStyle): a
// testbench object graph is arbitrarily deep -- a UVM component tree plus a
// sequence-item chain is thousands of links -- so recursion would trade a
// collector for a stack overflow.
void drain_objects() {
    [[maybe_unused]] uint64_t pops = 0;
    while (!g.worklist.empty()) {
        void* h = g.worklist.back();
        g.worklist.pop_back();
        ++pops;
        // Each object is marked at push time, so it enters the worklist at most
        // once and total pops cannot exceed the objects ever allocated. A runaway
        // here means a mark bit is not sticking (TigerStyle).
        assert(pops <= g.stats.objects_allocated && "the mark loop exceeded its bound");
        trace_one(h);
    }
    assert(g.worklist.empty() && "the object mark loop left work behind");
}

// Marking to a fixpoint over BOTH worklists, because they feed each other: a
// container element marks an object, and a marked object's leaf table names more
// containers. Draining either one once is therefore not enough.
void drain() {
    [[maybe_unused]] uint64_t rounds = 0;
    while (!g.worklist.empty() || !g.container_work.empty()) {
        ++rounds;
        // Each round pops at least one item, and each list's pops are bounded above
        // (by objects_allocated and by kContainerWorkMax), so their sum bounds the
        // rounds as well.
        assert(rounds <= g.stats.objects_allocated + kContainerWorkMax &&
               "the mark fixpoint exceeded its bound");
        drain_objects();
        drain_containers();
    }
    assert(g.container_work.empty() && "the mark fixpoint left a container behind");
    assert(g.stats.objects_live <= g.stats.objects_allocated && "live exceeds allocated");
}

// Precise roots: the design state buffer, per-instance state, static class
// properties. The backend emits each table's handle slots by byte offset, so this
// is one load per slot and needs no validation for correctness -- but a slot the
// backend misclassified (a `chandle` lowers to the same 64-bit plane, design §3.4)
// would have us trace a foreign DPI pointer, so a non-resolving precise root is
// fatal rather than skipped. There is no staleness path for a root slot: design
// state is never recycled heap.
void push_precise_roots() {
    for (const rt_gc_roots* t : g.roots) {
        assert(t != nullptr);
        assert(t->nhandles < kRootHandlesMax && "an implausible root table size");
        if (t->nhandles == 0) {
            continue;
        }
        assert(t->base != nullptr && "a non-empty root table has a base");
        assert(t->offsets != nullptr && "a non-empty root table has offsets");
        uint8_t* base = static_cast<uint8_t*>(t->base);
        for (uint32_t i = 0; i < t->nhandles; ++i) {
            void* h = *reinterpret_cast<void**>(base + t->offsets[i]);
            if (h == nullptr) {
                continue;
            }
            if (rt_gc_find_object(h) != h) {
                // Report the word, not just the shape: the two ways to get here are a
                // root table naming a slot that is not a class handle (so the value is
                // whatever the design stored there) and a live root naming an object the
                // heap has already reclaimed (so the value is a plausible-looking heap
                // address). The number is what distinguishes them.
                char note[160];
                std::snprintf(note, sizeof note,
                              "root %u of the table at %p reads %p, which the heap does not own "
                              "(resolved to %p)",
                              i, static_cast<const void*>(t), h, rt_gc_find_object(h));
                gc_fatal("a precise GC root does not name a class object", note);
            }
            mark_and_push(h);
        }
    }
}

// Conservative scan of a byte range. Every candidate is validated against our own
// metadata -- it must land in a live block, be RT_GC_ALIGN-aligned once walked
// back, and have its object-start bit set -- which makes this much stronger than
// Boehm-style conservatism (design §5.2). Hits are pinned; the pin is inert today
// and exists so evacuation stays a policy change (§5.3).
void scan_conservative(const uint8_t* base, uint64_t bytes) {
    assert(base != nullptr || bytes == 0);
    assert(bytes <= kScanBytesMax && "an implausible conservative range length");
    if (bytes < sizeof(void*)) {
        return;
    }
    // Word-wise at pointer alignment: a handle is a naturally-aligned 64-bit word
    // (F4), so an unaligned candidate cannot be one.
    constexpr uintptr_t step = sizeof(void*);
    const uintptr_t     raw  = reinterpret_cast<uintptr_t>(base);
    const uintptr_t     lo   = (raw + step - 1) & ~(step - 1);
    const uintptr_t     hi   = raw + bytes;
    for (uintptr_t p = lo; p + step <= hi; p += step) {
        void* cand = *reinterpret_cast<void* const*>(p);
        if (cand == nullptr) {
            continue;
        }
        void* h = rt_gc_find_object(cand);
        if (h == nullptr) {
            continue;  // an integer, a string handle, a chandle, a frame pointer
        }
        pin(h);
        mark_and_push(h);
    }
}

// A suspended process has no stack: its live handles are in a CoroSplit-laid-out
// heap frame reached through the intrusive registry node (F2, Gc.h:107-118).
void push_frame_roots() {
    [[maybe_unused]] uint64_t seen = 0;
    for (rt_gc_frame* n = g_mutator.frames_head; n != nullptr; n = n->next) {
        ++seen;
        assert(seen <= kFrameScanMax && "the frame registry is cyclic or corrupt");
        assert(n->base != nullptr || n->bytes == 0);
        assert(n->next == nullptr || n->next->prev == n);
        scan_conservative(static_cast<const uint8_t*>(n->base), n->bytes);
    }
}

void push_range_roots() {
    for (const Range& r : g.ranges) {
        assert(r.base != nullptr && "a registered range has a base");
        assert(r.bytes > 0 && "a registered range is non-empty");
        scan_conservative(r.base, r.bytes);
    }
}

// The records staged and not yet committed. Only the first `count` records are live: the
// rest are what earlier passes left, which a collection must not retain. A record is
// filled before any allocation that could collect (nothing between rt_nba_arr_slot and
// the last field store allocates a collected object), so no live record is half-written.
void push_nba_roots() {
    for (const NbaQueue& q : g.nba_queues) {
        const uint32_t count = rt_nba_arr_count(q.idx);
        if (count == 0) {
            continue;
        }
        uint8_t* base = static_cast<uint8_t*>(rt_nba_arr_base(q.idx));
        assert(base != nullptr && "a queue holding records has a buffer");
        for (uint32_t k = 0; k < count; ++k) {
            uint8_t* rec = base + static_cast<uint64_t>(k) * q.stride;
            if (q.ops != nullptr) {
                push_container(rec + q.offset, q.kind, q.ops);
            } else if (g.verifying == 0) {
                // A conservative hit is marked and pinned; verification only re-checks
                // what a precise walk reaches, as for a range or a frame.
                scan_conservative(rec, q.stride);
            }
        }
    }
}

// The event objects the runtime's own records name (RuntimeInternal.h, event_gc_roots).
// Precise, and held to mark_container_handle's standard for the same reason a container
// element is: each is a handle the runtime stored, so one that does not resolve is a bug,
// and under verification each must already be marked.
void push_event_roots() {
    rt_internal::event_gc_roots(mark_container_handle);
}

// What waits on a reachable event object (RuntimeInternal.h, event_gc_waiting). The
// event a subscription names is held to the standard of push_event_roots. A waiting
// record is native memory with no map, so it is scanned as a range is; verification
// only re-checks what a precise walk reaches, as for a range or a frame.
void push_event_waiting() {
    void (*const skip)(const uint8_t*, uint64_t) = [](const uint8_t*, uint64_t) {};
    rt_internal::event_gc_waiting(is_marked_now, mark_container_handle,
                                  g.verifying != 0 ? skip : scan_conservative);
}

// A weak reference's referent, by the weak reference's map.
void** weak_slot(void* weak) {
    const rt_gc_map* map = map_of(weak);
    assert(map->weak_referent != 0 && "only a weak reference has a referent");
    return reinterpret_cast<void**>(static_cast<uint8_t*>(weak) + map->weak_referent);
}

// Mark the referent of every weak reference reached so far, and what those reach. A
// collection run from inside an allocation does this instead of clearing: it runs in
// the middle of some process's statement, at a point the heap's size chose, and a
// process that runs later in the same time step could start waiting on a weak
// reference between its clearing and the wake that reports it. So weak references
// are cleared only at the scheduler's safepoint, where every process is parked.
// Tracing can reach more weak references, which join the list behind `k`.
void drain();
void keep_weak_referents() {
    for (size_t k = 0; k < g.weak_reached.size(); ++k) {
        void* const referent = *weak_slot(g.weak_reached[k]);
        if (referent != nullptr) {
            mark_and_push(referent);
            drain();
        }
    }
    g.weak_reached.clear();
}

// Clear the weak references this collection reached whose referent it did not
// (LRM 8.30.1). Called once marking is complete and before anything is freed, so
// every referent is still an object and is_marked_now() is its verdict.
//
// The weak references this collection reached are all of those that can need it. A
// full collection reaches every live one. A young one leaves an OLD weak reference
// unread, and that one's referent is old or null: the referent was an object before
// the weak reference was made, the word is written only then and by clear(), and the
// collection that made the weak reference old either marked the referent, which made
// it old too, or cleared the word. No young collection frees an old object.
//
// A wait that reads get() parks on the event whose handle is at `weak_notify`. That
// event is a field of the weak reference, so it was marked with it. It is put on the
// chain `->>` pends events on, and the scheduler fires that chain as soon as the
// collection returns to it, before any process runs (ir/llvm/GcEmit.cpp,
// emit_gc_safepoint): the collector has no wheel to stamp a trigger time from.
void clear_weak_referents() {
    assert(!g.weak_kept && "a collection that keeps referents clears none");
    for (void* weak : g.weak_reached) {
        const rt_gc_map* map      = map_of(weak);
        uint8_t* const   body     = static_cast<uint8_t*>(weak);
        void** const     slot     = weak_slot(weak);
        void* const      referent = *slot;
        if (referent == nullptr) {
            continue; // made of null, cleared by the design, or cleared before
        }
        assert_resolves(rt_gc_find_object(referent) == referent && "a weak referent names an object");
        if (is_marked_now(referent)) {
            continue;
        }
        *slot = nullptr;
        g.stats.weak_cleared += 1;
        if (map->weak_notify != 0) {
            void* const event = *reinterpret_cast<void**>(body + map->weak_notify);
            assert(event != nullptr && is_marked_now(event) && "a weak reference's notifier lives with it");
            rt_internal::event_pend(event);
        }
    }
    g.weak_reached.clear();
}

// Forget the number of every numbered object this collection did not reach. Called once
// marking is complete. A young collection can only have found dead the objects that were
// numbered since the last collection, since every other numbered object is old; a full
// one reads the whole table. A number is never handed out twice, so nothing else is
// owed: what named the object by its number named the object, and is gone with it.
void release_dead_identities() {
    assert(g.identity.size() == g.identified.size() && "the two identity maps are each other's inverse");
    const auto forget = [](void* handle) {
        const auto it = g.identity.find(handle);
        assert(it != g.identity.end() && "a numbered object is in the table");
        g.identified.erase(it->second);
        g.identity.erase(it);
    };
    if (g.kind == RT_GC_KIND_YOUNG) {
        for (void* handle : g.identity_fresh) {
            if (!is_marked_now(handle)) {
                forget(handle);
            }
        }
    } else {
        // Collected first: erasing from the map while walking it would skip entries.
        g.identity_fresh.clear();
        for (const auto& [handle, id] : g.identity) {
            if (!is_marked_now(const_cast<void*>(handle))) {
                g.identity_fresh.push_back(const_cast<void*>(handle));
            }
        }
        for (void* handle : g.identity_fresh) {
            forget(handle);
        }
    }
    g.identity_fresh.clear();
    g.stats.identities = g.identity.size();
}

// -- The native stack, for a collection run from inside an allocation (§6.1) ---
//
// Why this is sound is the argument in design §6.1; this is the mechanism. The stack and
// the registers are roots exactly as a suspended coroutine's frame is -- every word is
// validated against the heap's own metadata, and a hit is pinned, which in a non-moving
// heap is retention and nothing else -- so nothing here is new to the collector except
// where the bytes come from.

// One stack word. A handle, or a pointer into an object, is a root. Anything else that could
// be an address is kept for buffers_park: it may point into a malloc'd buffer an object
// owns, which the heap cannot resolve to its owner and a finalizer must not free from under
// the code holding it.
void consider_stack_word(uintptr_t word) {
    if (word < kInFlightWordMin) {
        return;
    }
    void* h = rt_gc_find_object(reinterpret_cast<const void*>(word));
    if (h != nullptr) {
        pin(h);
        mark_and_push(h);
        return;
    }
    g.inflight.push_back(word);
}

// The machine stack from `lo` up to `hi`, word by word. NOT instrumented: the runtime's
// own frames between here and rt_sim_run carry AddressSanitizer redzones, which a
// conservative scan has to read like any other word, so the loads must not be checked.
// Everything done with a word goes through an ordinary function above.
__attribute__((no_sanitize("address"))) void scan_stack_words(uintptr_t lo, uintptr_t hi) {
    constexpr uintptr_t step = sizeof(uintptr_t);
    lo                       = (lo + step - 1) & ~(step - 1);
    for (uintptr_t p = lo; p + step <= hi; p += step) {
        consider_stack_word(*reinterpret_cast<const uintptr_t*>(p));
    }
}

// Whether a word of the stack lies in [p, p + bytes], one past the end included: a loop's
// pointer rests there between its last element and the exit test.
bool inflight_covers(const void* p, uint64_t bytes) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(p);
    const auto      it = std::lower_bound(g.inflight.begin(), g.inflight.end(), lo);
    return it != g.inflight.end() && *it <= lo + bytes;
}

// Every collection clears the set; only an emergency one fills it.
void push_stack_roots() {
    g.inflight.clear();
    if (g.stack_lo == 0) {
        return;
    }
    scan_stack_words(g.stack_lo, reinterpret_cast<uintptr_t>(g.stack_base));
    // A comparator of our own rather than the default: the executable's runtime links no
    // libc++ library, and the default-ordered sort of integers is an extern template there.
    std::sort(g.inflight.begin(), g.inflight.end(), [](uintptr_t a, uintptr_t b) { return a < b; });
    g.inflight.erase(std::unique(g.inflight.begin(), g.inflight.end()), g.inflight.end());
}

// Free what an earlier emergency collection kept, except what a word of this collection's
// stack still lies inside. After an ordinary collection the set is empty, so this frees
// everything, which is right: no JIT frame exists at a region boundary to hold a pointer.
void release_parked() {
    size_t keep = 0;
    for (const GcState::Parked& p : g.parked) {
        if (inflight_covers(p.ptr, p.bytes)) {
            g.parked[keep++] = p;
        } else {
            std::free(p.ptr);
        }
    }
    g.parked.resize(keep);
}

// -- The remembered set (§7) --------------------------------------------------
//
// The barrier's record of the old-to-young edges a young collection could not
// otherwise see. Two producers -- rt_gc_remember(), the out-of-line half of the
// inlined barrier, and relog_conservative_roots() below -- and one consumer,
// push_remembered_roots(), which every collection runs exactly once.

// Set LOGGED and record. Idempotent through the LOGGED bit, so a hot object costs one
// push per collection CYCLE rather than one per store, and bounded by kRememberedMax:
// past that recording STOPS and the next collection is forced full. Dropping an entry
// is sound only because of that -- a full collection traces everything and consults no
// remembered set -- so the drop and the flag are written together here and can never
// come apart.
void remember_object(void* handle) {
    assert(handle != nullptr && "the barrier logs an object, not null");
    uint8_t* const m = meta_ptr(handle);
    assert((*m & RT_GC_META_MARK) != 0 && "only an OLD object belongs in the remembered set");
    if ((*m & RT_GC_META_LOGGED) != 0) {
        return;  // already recorded this cycle; the set holds it exactly once
    }
    if (g.remembered.size() >= kRememberedMax) {
        assert(g.remembered.size() == kRememberedMax && "the remembered set ran past its bound");
        // LOGGED deliberately NOT set: the barrier stays armed for this object, so it is
        // recorded again the moment a collection has drained the set. `request` may be
        // cleared again by rearm_after_collection() if this runs inside a collection;
        // force_full is the durable half and the only one correctness rests on.
        g.force_full      = 1;
        g_mutator.request = 1;
        return;
    }
    *m |= RT_GC_META_LOGGED;
    g.remembered.push_back(handle);
}

// The remembered set as a root set. LOGGED is cleared on every entry whatever the kind,
// and that is not bookkeeping: a stuck LOGGED bit disarms the barrier FOREVER for that
// object, because every later store takes the not-taken edge of
// `(gcword & (OLD|LOGGED)) == OLD`. So a full collection, which needs no remembered set
// at all, still walks it to clear the bits before it drops the contents (Gc.h:98-108).
void push_remembered_roots() {
    assert(g.kind == RT_GC_KIND_YOUNG || g.kind == RT_GC_KIND_FULL);
    assert(g.remembered.size() <= kRememberedMax && "the remembered set ran past its bound");
    for (void* h : g.remembered) {
        uint8_t* const m = meta_ptr(h);
        assert((*m & RT_GC_META_LOGGED) != 0 && "a remembered entry lost its LOGGED bit");
        // OLD, which a full collection has by now turned into "was old" by clearing the marks.
        assert((*m & (RT_GC_META_MARK | kMetaWasOld)) != 0 && "a remembered entry is an OLD object");
        *m = static_cast<uint8_t>(*m & ~RT_GC_META_LOGGED);
        if (g.kind == RT_GC_KIND_FULL) {
            // Deliberately NOT marked. A full collection reaches every live object from
            // the real roots, so marking from here instead would let a stale entry keep
            // an unreachable old object -- and reclaiming exactly those is what a full
            // collection is for.
            continue;
        }
        // Traced though it is marked, because it has been written to since the collection
        // that marked it: this is the one place a young collection reads an OLD object.
        // The set holds an object once (LOGGED), and mark_and_push() refuses a marked
        // object, so nothing else queues it a second time.
        push_marked(h);
    }
    g.remembered.clear();
    assert(g.remembered.empty() && "the drain must leave the set empty");
}

// Re-enter every conservatively-reached object into the remembered set, for the NEXT
// collection. This is not belt-and-braces: it closes a hole the inlined barrier cannot.
//
// The barrier is emitted at the store, except for a store through a raw interior pointer
// whose owner is not statically known -- a `ref` formal bound to a class property, or an
// output/inout formal whose actual is one -- where it is emitted at the CALL SITE, the
// last place the owner handle is in hand (ir/llvm/Subroutines.cpp, lower_call_common). A non-blocking call cannot span a safepoint, so for a
// function the call-site entry is still in the set when the callee's store lands. A
// blocking TASK can suspend across a collection, which consumes that entry; the store
// then lands on an OLD object whose LOGGED bit is clear with nothing left to log it, and
// the next young collection would not trace it and would reclaim the young object it
// stored while it is still reachable.
//
// What closes that is liveness. To perform the store after resuming, the activation must
// hold the pointer -- the interior pointer, or the owner handle for a copy-back -- across
// the suspend that PRECEDES the store, so CoroSplit keeps it in the coroutine frame and
// the frame scan resolves it (rt_gc_find_object resolves interior pointers,
// Gc.h:433-438). Every collection with such a store still to come therefore sees the
// object conservatively, and re-logging it there means the first collection after the
// store traces it. g.pinned is exactly that set, already deduplicated by the PINNED bit.
//
// Tracing the object at THIS collection instead would not close the hole: the pointer
// need not still be live at the next safepoint after the store, so the object may never
// be reached conservatively again. Only carrying the obligation forward works.
void relog_conservative_roots() {
    assert(g.worklist.empty() && "the re-log runs after the mark phase");
    // The drain emptied the set at the start of this collection and nothing between
    // then and here logs anything: a finalizer must not store a class handle, and
    // sweep() asserts the neighbouring halves of that same claim.
    assert(g.remembered.empty() && "something logged an object mid-collection");
    for (void* h : g.pinned) {
        // Marked (hence promoted) or already OLD -- either way remember_object's
        // precondition holds, which is what this asserts on its behalf.
        assert(is_old(h) && "a conservatively-reached object is OLD after the mark phase");
        remember_object(h);
    }
    // Never counted into stats.remembered: that counter means "objects the WRITE BARRIER
    // logged" (Gc.h:408-411), which is what lets a test prove the barrier fired at all,
    // and it would prove nothing if the collector padded it.
    assert(g.remembered.size() <= kRememberedMax && "the re-log ran past the bound");
}

void unpin_all() {
    for (void* h : g.pinned) {
        uint8_t* const m = meta_ptr(h);
        // §13's pin audit: every collection-scoped pin must still be held here, and
        // after this loop no object anywhere carries one -- which is the negative
        // space rt_gc_verify() then asserts across the whole heap.
        assert((*m & RT_GC_META_PINNED) != 0 && "a collection-scoped pin was cleared early");
        // Marked, or OLD and therefore live without being marked: a young collection does
        // not mark an un-remembered OLD object, so a conservative hit on one pins it and
        // stops there (mark_and_push). One predicate covers both, which is the §13 claim
        // it always was -- a pin names a live object.
        assert(is_marked_now(h) && "a pinned object is a live object");
        *m = static_cast<uint8_t>(*m & ~RT_GC_META_PINNED);
    }
    g.pinned.clear();
}

// -- Sweeping -----------------------------------------------------------------
//
// What frees memory. A sweep walks a block's metadata from object to object: a marked
// object stays, and an unmarked one has its finalizer run if it owes one and then its
// metadata bytes zeroed, which is all that freeing is -- the allocator finds free space
// as runs of zero bytes. No object is read except to finalize it.
//
// A finalizer runs while its object's body is intact, and a dead neighbour's body is
// intact too whichever of them the sweep reaches first: zeroing metadata disturbs no
// object, and nothing is allocated over a freed granule until the collection is over.

// Before a FULL collection marks anything: clear every mark, leaving on each object
// that had one the note that it was old (kMetaWasOld). After this a mark means "reached
// by this collection". A free block has no object and is skipped whole.
void clear_marks() {
    for (ChunkRecord* c : g.chunks) {
        for (uint32_t i = 0; i < kBlocksPerChunk; ++i) {
            if (c->block[i].state == kBlockFree) {
                continue;
            }
            uint8_t* const meta = BlockRef{ c, i }.meta();
            for (uint32_t k = 0; k < kGranulesPerBlock; k += 8) {
                uint64_t       w = load_word(meta + k);
                const uint64_t t = marked_starts(w);
                if (t != 0) {
                    // MARK off and kMetaWasOld on, in each byte `t` names.
                    w = (w & ~t) | (t << 4);
                    std::memcpy(meta + k, &w, sizeof w);
                }
            }
        }
    }
    for (ChunkRecord* c : g.large) {
        uint8_t* const m = meta_at(reinterpret_cast<uint8_t*>(c) + RT_GC_LARGE_PAYLOAD);
        if ((*m & RT_GC_META_MARK) != 0) {
            *m = static_cast<uint8_t>((*m & ~RT_GC_META_MARK) | kMetaWasOld);
        }
    }
}

// Run the finalizer of a dead object that owes one. The class of an object the sweep
// reaches is known: generated code stores the vptr right after the allocation returns
// and nothing collects in between.
void finalize_object(void* handle) {
    const rt_gc_map* map = map_of(handle);
    assert(map->finalize != nullptr && "FINALIZE was set from a non-null finalize");
    map->finalize(handle);
}

// Run the finalizers a block owes, every object in it being dead.
uint64_t finalize_block(BlockRef b) {
    const uint8_t* const meta = b.meta();
    uint8_t* const       base = b.base();
    uint64_t             ran  = 0;
    for (uint32_t i = 0; i < kGranulesPerBlock; i += 8) {
        // One bit per object, cleared lowest first, so at most eight turns a word.
        for (uint64_t f = finalizable_starts(load_word(meta + i)); f != 0; f &= f - 1) {
            const uint64_t granule = i + (static_cast<uint32_t>(std::countr_zero(f)) >> 3);
            finalize_object(base + (granule << kGranuleShift));
            ++ran;
        }
    }
    return ran;
}

// Sweep one block and bring its record up to date. `spare_marked` is false only at
// teardown, which frees everything. Returns the finalizers run.
uint64_t sweep_block(BlockRef b, bool spare_marked) {
    uint8_t* const meta = b.meta();
    uint8_t* const base = b.base();
    BlockInfo&     info = b.info();
    // First, eight granules at a time: is there an object to keep, and a finalizer to run?
    uint64_t marked = 0;
    uint64_t owed   = 0;
    for (uint32_t i = 0; i < kGranulesPerBlock; i += 8) {
        const uint64_t w = load_word(meta + i);
        marked |= marked_starts(w);
        owed |= finalizable_starts(w);
    }
    if (!spare_marked || marked == 0) {
        // Nothing stays, which is the usual end of a block of short-lived objects: the
        // whole block is freed at once and no object in it is looked at, but for the
        // ones that owe a finalizer.
        const uint64_t ran = owed != 0 ? finalize_block(b) : 0;
        std::memset(meta, 0, kGranulesPerBlock);
        info.cursor  = 0;
        info.used    = 0;
        info.max_run = kGranulesPerBlock;
        info.young   = 0;
        info.state   = kBlockFree;
        return ran;
    }
    uint64_t ran     = 0;
    uint32_t used    = 0;
    uint32_t run     = 0;  // the free run ending at `i`
    uint32_t longest = 0;
    uint32_t i       = 0;
    // `i` only ever advances, so one pass over the block's metadata bounds the walk.
    while (i < kGranulesPerBlock) {
        const uint8_t m = meta[i];
        if (m == 0) {
            const uint32_t next = skip_free(meta, i);
            run += next - i;
            i = next;
            continue;
        }
        assert((m & RT_GC_META_START) != 0 && "the sweep steps from object to object");
        const uint32_t end = skip_object(meta, i + 1);
        if ((m & RT_GC_META_MARK) != 0) {
            meta[i] = static_cast<uint8_t>(m & ~kMetaWasOld);
            used += end - i;
            longest = run > longest ? run : longest;
            run     = 0;
        } else {
            assert((m & RT_GC_META_PINNED) == 0 && "a pinned object would be marked");
            assert((m & RT_GC_META_LOGGED) == 0 && "a logged object is old, so marked");
            if ((m & RT_GC_META_FINALIZE) != 0) {
                finalize_object(base + (static_cast<uint64_t>(i) << kGranuleShift));
                ++ran;
            }
            zero_run(meta + i, end - i);
            run += end - i;
        }
        i = end;
    }
    longest      = run > longest ? run : longest;
    info.cursor  = 0;
    info.used    = static_cast<uint16_t>(used);
    info.max_run = static_cast<uint16_t>(longest);
    info.young   = 0;
    info.state   = used == kGranulesPerBlock ? kBlockFull : kBlockPartial;
    assert(used != 0 && "a block with a marked object is not free");
    return ran;
}

// Put a swept block on the list its state calls for, and count it if it is free.
void list_block(BlockRef b) {
    const uint8_t state = b.info().state;
    if (state == kBlockFree) {
        g.free_blocks.push_back(b);
        b.chunk->blocks_free += 1;
        g.stats.blocks_free += 1;
    } else if (state == kBlockPartial) {
        g.partial_blocks[run_class(b.info().max_run)].push_back(b);
    }
}

// Free a dead large object: its finalizer, then its place in the table and in g.large,
// whose last entry takes its slot, so that no address resolves to it. Its mapping is
// kept for the next object of its size.
uint64_t retire_large(ChunkRecord* c) {
    uint8_t* const handle = reinterpret_cast<uint8_t*>(c) + RT_GC_LARGE_PAYLOAD;
    uint8_t* const meta   = meta_at(handle);
    uint64_t       ran    = 0;
    if ((*meta & RT_GC_META_FINALIZE) != 0) {
        finalize_object(handle);
        ran = 1;
    }
    *meta = 0;
    table_remove(c, RT_GC_LARGE_PAYLOAD + c->large_bytes);
    assert(c->slot < g.large.size() && g.large[c->slot] == c && "a large object is in its slot");
    g.large[c->slot]       = g.large.back();
    g.large[c->slot]->slot = c->slot;
    g.large.pop_back();
    const auto it = g.large_sizes.find(c->large_bytes);
    assert(it != g.large_sizes.end() && "a large object's size was entered when it was allocated");
    it->second.spare.push_back(c);
    return ran;
}

// A large object the collection keeps: nothing to do but drop the full collection's note.
bool keep_large(ChunkRecord* c, bool spare_marked) {
    uint8_t* const m = meta_at(reinterpret_cast<uint8_t*>(c) + RT_GC_LARGE_PAYLOAD);
    assert((*m & RT_GC_META_START) != 0 && "a mapped large object has its metadata byte");
    if (!spare_marked || (*m & RT_GC_META_MARK) == 0) {
        return false;
    }
    *m = static_cast<uint8_t>(*m & ~kMetaWasOld);
    return true;
}

// What a collection leaves of the kept mappings. A young one leaves them all, as it
// leaves every chunk. A full one keeps, of each size, as many as objects of that size
// were allocated since the last collection -- what the next cycle will want again --
// and gives the rest back, so a design that has stopped allocating large objects
// holds none. Either way the count starts again.
void settle_spare_large(bool full) {
    for (auto& entry : g.large_sizes) {
        GcState::LargeSize& kept = entry.second;
        while (full && kept.spare.size() > kept.taken) {
            unmap_large(kept.spare.back());
            kept.spare.pop_back();
        }
        kept.taken = 0;
    }
}

// How many free blocks a full collection leaves the heap: as many as were allocated
// from since the last collection, which the next cycle will want again, and never
// fewer than one allocation budget. Without the first a design that allocates at a
// steady rate would return and re-request the same chunks at every full collection.
uint64_t block_reserve() {
    // Rounded up without the sum, which wraps for a budget no run can spend.
    const uint64_t nursery = g.cfg.nursery_bytes;
    const uint64_t budget  = nursery / RT_GC_BLOCK_BYTES + (nursery % RT_GC_BLOCK_BYTES != 0 ? 1u : 0u);
    return g.young.size() > budget ? g.young.size() : budget;
}

// Give a chunk with no object in it back to the system. The last entry of g.chunks
// takes its slot; the caller rebuilds the block lists.
void release_chunk(ChunkRecord* c) {
    assert(c->blocks_free == kBlocksPerChunk && "only an empty chunk is released");
    table_remove(c, kChunkBytes);
    assert(c->slot < g.chunks.size() && g.chunks[c->slot] == c && "a chunk is in its slot");
    g.chunks[c->slot]       = g.chunks.back();
    g.chunks[c->slot]->slot = c->slot;
    g.chunks.pop_back();
    g.stats.blocks_total -= kBlocksPerChunk;
    g.stats.blocks_free -= kBlocksPerChunk;
    clotho::rt::os_unmap(c, static_cast<size_t>(kChunkBytes));
}

// A YOUNG collection's sweep: the blocks allocated from since the last collection and
// the large objects allocated since then. Nothing else can hold an unmarked object, so
// nothing else is read, and the cost is what was allocated.
uint64_t sweep_young() {
    uint64_t ran = 0;
    for (const BlockRef b : g.young) {
        assert(b.info().young != 0 && b.info().state == kBlockOwned &&
               "a young block is one a mutator took and no sweep has seen");
        ran += sweep_block(b, true);
        list_block(b);
    }
    for (ChunkRecord* c : g.young_large) {
        if (!keep_large(c, true)) {
            ran += retire_large(c);
        }
    }
    return ran;
}

// A FULL collection's sweep, and the teardown sweep when `spare_marked` is false: every
// block that is not free, every large object. The block lists are rebuilt, and chunks
// left with no object beyond the reserve go back to the system.
uint64_t sweep_all(bool spare_marked) {
    uint64_t ran = 0;
    g.free_blocks.clear();
    for (std::vector<BlockRef>& list : g.partial_blocks) {
        list.clear();
    }
    g.stats.blocks_free = 0;
    for (ChunkRecord* c : g.chunks) {
        c->blocks_free = 0;
        for (uint32_t i = 0; i < kBlocksPerChunk; ++i) {
            const BlockRef b{ c, i };
            if (c->block[i].state != kBlockFree) {
                ran += sweep_block(b, spare_marked);
            }
            c->blocks_free += c->block[i].state == kBlockFree ? 1u : 0u;
        }
        g.stats.blocks_free += c->blocks_free;
    }
    // The free blocks of chunks that still hold an object count toward the reserve,
    // and whole chunks are kept only until it is met. Released from the back, so the
    // swap that fills a slot never moves an unvisited chunk into a visited one.
    const uint64_t want = block_reserve();
    uint64_t       have = 0;
    for (const ChunkRecord* c : g.chunks) {
        have += c->blocks_free != kBlocksPerChunk ? c->blocks_free : 0u;
    }
    for (size_t n = g.chunks.size(); n > 0; --n) {
        ChunkRecord* const c = g.chunks[n - 1];
        if (c->blocks_free != kBlocksPerChunk) {
            continue;
        }
        if (have < want) {
            have += kBlocksPerChunk;
            continue;
        }
        release_chunk(c);
    }
    for (ChunkRecord* c : g.chunks) {
        for (uint32_t i = 0; i < kBlocksPerChunk; ++i) {
            const uint8_t state = c->block[i].state;
            if (state == kBlockFree) {
                g.free_blocks.push_back(BlockRef{ c, i });
            } else if (state == kBlockPartial) {
                g.partial_blocks[run_class(c->block[i].max_run)].push_back(BlockRef{ c, i });
            }
        }
    }
    // Backwards for the same reason: retire_large fills the slot from the end.
    for (size_t n = g.large.size(); n > 0; --n) {
        ChunkRecord* const c = g.large[n - 1];
        if (!keep_large(c, spare_marked)) {
            ran += retire_large(c);
        }
    }
    return ran;
}

// The sweep of the collection now running. A finalizer must not allocate and must not
// store a handle anywhere: the first is checked here, in the two halves that cover both
// allocation paths, and the second would show up as an unmarked object some marked one
// points at, which rt_gc_verify() catches.
void sweep() {
    [[maybe_unused]] const uint64_t allocated_before = g.stats.objects_allocated;
    assert(g_mutator.cursor == nullptr && "rt_gc_collect retires the run before marking");
    assert(g.worklist.empty() && "a sweep runs outside the mark loop");
    g.stats.finalized += g.kind == RT_GC_KIND_FULL ? sweep_all(true) : sweep_young();
    settle_spare_large(g.kind == RT_GC_KIND_FULL);
    g.young.clear();
    g.young_large.clear();
    assert(g_mutator.cursor == nullptr && "a finalizer allocated");
    assert(g.stats.objects_allocated == allocated_before && "a finalizer allocated a large one");
    assert(g.worklist.empty() && "a finalizer marked something");
    assert(g.stats.blocks_free <= g.stats.blocks_total && "more free blocks than blocks");
}

// Every remaining finalizer at teardown, live or not. Not needed for correctness
// -- the process is exiting -- but it is what makes LeakSanitizer meaningful for
// class objects, which is this work's acceptance evidence (§13).
//
// Deliberately NOT counted into stats.finalized: that counter means "finalized by a
// collection", so a garbage cycle dropped mid-run bumps it and a cycle that
// survives to exit does not. The teardown count is returned for the stats line
// instead, which keeps the two distinguishable without widening rt_gc_stats.
uint64_t finalize_all_remaining() {
    const uint64_t ran = sweep_all(false);
    g.young.clear();
    g.young_large.clear();
    assert(g.large.empty() && "teardown frees every large object");
    return ran;
}

// -- Teardown -----------------------------------------------------------------

void release_heap() {
    // Before a single block goes away: the run names one of them, and the retirement
    // inside drop_hole() reads that block's metadata to count what it handed out.
    drop_hole(&g_mutator);
    for (ChunkRecord* c : g.large) {
        unmap_large(c);
    }
    g.large.clear();
    release_spare_large();
    g.large_sizes.clear();
    assert(g.large_mapped == 0 && "every large mapping went back");
    for (ChunkRecord* c : g.chunks) {
        clotho::rt::os_unmap(c, static_cast<size_t>(kChunkBytes));
    }
    g.chunks.clear();
    g.table.clear();
    g.heap_lo = UINTPTR_MAX;
    g.heap_hi = 0;
    g.free_blocks.clear();
    for (std::vector<BlockRef>& list : g.partial_blocks) {
        list.clear();
    }
    g.young.clear();
    g.young_large.clear();
    g.roots.clear();
    g.ranges.clear();
    g.nba_queues.clear();
    g.container_tables.clear();
    g.containers.clear();
    g.container_work.clear();
    g.worklist.clear();
    g.pinned.clear();
    g.weak_reached.clear();
    g.identity.clear();
    g.identified.clear();
    g.identity_fresh.clear();
    g.identity_last = 0;
    // Every entry names an object in a block that has just been freed, so the set must
    // not outlive the heap. The LOGGED bits go with the objects; there is nothing left
    // to re-arm and rt_gc_init re-establishes the rest of the generational state.
    g.remembered.clear();
    // The buffers an emergency collection kept for the code that was using them: that code
    // is gone with the run, so nothing can name them now.
    for (const GcState::Parked& p : g.parked) {
        std::free(p.ptr);
    }
    g.parked.clear();
    g.inflight.clear();
}

void reset_mutator() {
    drop_hole(&g_mutator);
    g_mutator.budget_left = g.cfg.nursery_bytes;
    g_mutator.request     = 0;
    // The coroutine frame registry lives in the mutator now (Gc.h:178-181), so
    // resetting the mutator is what empties it. Nothing is freed: every node is an
    // alloca inside a coroutine frame the JIT'd module owns.
    g_mutator.frames_head = nullptr;
    assert(g_mutator.cursor == nullptr && "a reset mutator owns no hole");
    assert(g_mutator.frames_head == nullptr && "a reset mutator has an empty registry");
}

// Saturating, because the budgets are configuration and the byte counts are whatever the
// design made them; neither is bounded by anything the collector controls.
uint64_t sat_add(uint64_t a, uint64_t b) {
    const uint64_t sum = a + b;
    return sum < a ? UINT64_MAX : sum;
}

// Measure the growth of owned buffers from here: what the collection that just ended left
// is the baseline, one budget above it is where the next is requested, and
// emergency_bytes above it is where the allocator is told to take the slow path (§6.5).
void arm_owned_trigger() {
    g.owned_after_gc     = g_owned.live;
    g.owned_request_at   = sat_add(g_owned.live, g.cfg.nursery_bytes);
    g.owned_emergency_at = sat_add(g_owned.live, g.emergency_bytes);
    g_owned.trigger      = g.owned_request_at;
}

// What a collection leaves behind for the next one. Separate from reset_mutator()
// because it deliberately does NOT touch the hole -- rt_gc_collect dropped that before
// marking -- and deliberately does not clear the frame registry, whose nodes are still
// live frames.
void rearm_after_collection() {
    // Re-armed under stress, cleared otherwise: the inlined poll tests this byte and
    // nothing else, so leaving it set is the whole of "collect at every safepoint". It
    // cannot livelock: rt_gc_safepoint_slow() collects once and returns, and the next
    // poll is only reached when the scheduler advances to another region boundary, so
    // re-arming buys exactly one collection per safepoint -- which is what --gc-stress
    // meant before the poll was inlined.
    g_mutator.request     = (g.cfg.stress != 0) ? 1u : 0u;
    g_mutator.budget_left = g.cfg.nursery_bytes;
    g.charged_since_gc    = 0;
    arm_owned_trigger();
    assert(g_mutator.cursor == nullptr && "a collection drops the hole before marking");
    assert(g_mutator.budget_left > 0 && "rt_gc_init floors the nursery budget");
}

// -- Collection policy (§7) ---------------------------------------------------

// Committed heap bytes: whole chunks plus the mappings of large objects, those kept
// for reuse included. Large objects belong in it because a young collection does not
// reclaim an old one either, so a design whose garbage is all large objects would
// otherwise never take a full collection.
uint64_t heap_bytes() {
    const uint64_t chunks = g.chunks.size() * kChunkBytes;
    const uint64_t bytes  = chunks + g.large_mapped;
    assert(bytes >= chunks && "the heap total wrapped");
    return bytes;
}

// Which kind of collection to run. One place, and a SCHEDULE rather than a heuristic
// wherever the caller asked for one:
//
//   1. The first collection is always full: there is no live baseline for the growth
//      rule yet. It costs exactly what a young collection costs, because nothing is OLD.
//   2. A remembered-set overflow forces the next one full. That is the whole reason
//      dropping an entry is sound (remember_object), so it outranks every preference.
//   3. cfg.full_every is a user instruction and wins over everything below: every Nth
//      collection is full, counting from the first. 1 means "always full", which is
//      exactly the non-generational collector this phase generalises.
//   4. --gc-stress with no explicit full_every behaves as full_every 2, so the young and
//      full paths ALTERNATE and a stress run exercises both at every safepoint. Stress is
//      the strongest correctness mode and a mode that only ever ran one of the two paths
//      would be the weaker for it.
//   5. Otherwise heap growth: full once the heap has grown past kFullGrowthFactor times
//      the live bytes the last FULL collection measured, floored at kFullGrowthFloor. A
//      young collection cannot reclaim old garbage, so this is the only thing bounding
//      floating garbage, and it bounds it at a constant factor of the live set. The same
//      rule is applied to the buffers strings and containers own, which a young collection
//      cannot free for an old dead object either: a design whose objects are small and
//      whose strings are large would otherwise never take a full collection, however
//      much of its memory was dead (§6.5). Reaching --gc-heap-max forces one too: it is
//      the one situation a young collection can do nothing about at all.
uint32_t choose_kind() {
    assert(g.initialized != 0 && "the kind is chosen inside an initialized collector");
    assert(g.cfg.full_every >= 0 && "a negative full_every is a configuration bug");
    if (g.stats.full_collections == 0 || g.force_full != 0) {
        return RT_GC_KIND_FULL;
    }
    int32_t every = g.cfg.full_every;
    if (every <= 0 && g.cfg.stress != 0) {
        every = 2;
    }
    if (every > 0) {
        const uint64_t n = static_cast<uint64_t>(every);
        return (g.stats.collections % n == 0) ? RT_GC_KIND_FULL : RT_GC_KIND_YOUNG;
    }
    const uint64_t heap = heap_bytes();
    if (g.cfg.heap_bytes_max != 0 && !heap_may_grow(kChunkBytes)) {
        return RT_GC_KIND_FULL;
    }
    assert(g.live_after_full < (1ull << 62) && "an implausible live-bytes baseline");
    uint64_t target = g.live_after_full * kFullGrowthFactor;
    if (target < kFullGrowthFloor) {
        target = kFullGrowthFloor;
    }
    if (heap > target) {
        return RT_GC_KIND_FULL;
    }
    assert(g.owned_after_full < (1ull << 62) && "an implausible owned-bytes baseline");
    uint64_t owned_target = g.owned_after_full * kFullGrowthFactor;
    if (owned_target < kFullGrowthFloor) {
        owned_target = kFullGrowthFloor;
    }
    return (g_owned.live > owned_target) ? RT_GC_KIND_FULL : RT_GC_KIND_YOUNG;
}

// Every root set, then the mark fixpoint. Extracted from rt_gc_collect_kind so both
// halves fit on a screen (TigerStyle), and it is one idea in its own right: what
// a collection treats as reachable before it has looked at the heap at all.
void mark_from_roots() {
    assert(g.worklist.empty() && "the mark phase starts with an empty worklist");
    assert(g.pinned.empty() && "the previous collection left a pin behind");
    // The remembered set FIRST. The order among the root sets is free for the reason it
    // always was -- marking is idempotent -- but it must precede drain(), so that a young
    // collection traces the old objects that acquired a young reference. It is also where
    // every LOGGED bit is cleared, whatever the kind, which is what re-arms the barrier
    // for the cycle about to start.
    push_remembered_roots();
    // Precise before conservative. Not required for correctness either, but it means an
    // object reachable both ways is marked precisely and only pinned afterwards, so
    // conservative_pins counts the objects whose retention conservatism is actually
    // responsible for.
    push_precise_roots();
    push_container_roots();
    push_frame_roots();
    push_range_roots();
    push_nba_roots();
    push_event_roots();
    // Empty and a no-op outside an emergency collection, where it is the one root set that
    // is not a registered structure: the design's own native stack (§6.1).
    push_stack_roots();
    drain();
    // Reaching an event can make what waits on it reachable, and that can reach another
    // event; keeping a weak reference's referent can reach either. Each pass that goes
    // round again marked an object no pass had, so this ends.
    for (;;) {
        if (g.weak_kept) {
            keep_weak_referents();
        }
        push_event_waiting();
        if (g.worklist.empty() && g.container_work.empty()) {
            break;
        }
        drain();
    }
    assert((!g.weak_kept || g.weak_reached.empty()) && "every referent of a kept weak reference is marked");
}

// The counters, and the invariants that are claims about the collection as a whole.
void account_collection(uint32_t reason, uint32_t kind) {
    assert(reason <= RT_GC_REASON_EMERGENCY && "an unknown collection reason");
    assert(kind == RT_GC_KIND_YOUNG || kind == RT_GC_KIND_FULL);
    g.stats.collections += 1;
    g.by_reason[reason] += 1;
    if (kind == RT_GC_KIND_FULL) {
        g.stats.full_collections += 1;
        // Only a full collection may write either of these: it is the only one that has
        // measured the WHOLE live set.
        g.live_after_full = g.stats.bytes_live;
        // The owned-buffer half of the same baseline. Read at the end of the collection,
        // after the finalizers have freed what the dead objects owned, so it is what the
        // live set holds.
        g.owned_after_full = g_owned.live;
        g.force_full      = 0;
    } else {
        g.stats.young_collections += 1;
    }
    assert(g.stats.young_collections + g.stats.full_collections == g.stats.collections &&
           "every collection is either young or full");
    assert((g.cfg.stress == 0 || g_mutator.request != 0) &&
           "under stress a collection leaves the poll armed for the next safepoint");
    assert(g.stats.objects_live <= g.stats.objects_allocated && "live exceeds ever-allocated");
    assert(g.stats.bytes_live <= g.stats.bytes_allocated && "live exceeds ever-allocated");
}

// The three numbers the backend computes independently from its own mirror of these
// layouts, checked before anything else and in RELEASE as well as debug. Generated code
// dereferences all three directly, so a drifted mirror reads or writes the wrong field
// -- a silent wrong-code bug rather than a diagnosable one, which is exactly what a
// located fatal at startup is for.
void check_backend_mirror(const rt_gc_config* cfg) {
    assert(cfg != nullptr && "the cross-check needs a configuration");
    char note[192];
    // (1) The trace-map offset. A descriptor-layout change must fail here rather than
    // mark garbage (Gc.h:80-86).
    if (cfg->map_vptr_offset != rt_gc_map_vptr_offset()) {
        std::snprintf(note, sizeof(note),
                      "the backend computed %d; this runtime computes %d from kMaxClassDepth=%u",
                      cfg->map_vptr_offset, rt_gc_map_vptr_offset(), kMaxClassDepth);
        gc_fatal("the GC trace-map offset disagrees with the class descriptor layout", note);
    }
    // (2) The poll byte generated code loads at every safepoint. Polling the wrong field
    // is silent, and its neighbour, the top half of budget_left, is nearly always
    // nonzero -- so the failure mode is "collect at every safepoint" rather than a crash.
    if (cfg->request_offset != static_cast<int32_t>(rt_gc_mutator_request_offset())) {
        std::snprintf(note, sizeof(note),
                      "the backend computed %d; this runtime computes %u from "
                      "offsetof(rt_gc_mutator, request)",
                      cfg->request_offset, rt_gc_mutator_request_offset());
        gc_fatal("the GC safepoint-poll offset disagrees with the mutator layout", note);
    }
    // (3) The size of the whole struct, which is what says the other offsets can be
    // trusted: generated code no longer loads one byte from the mutator but indexes five
    // fields of it for the inlined allocation fast path (Gc.h:200-203, 255-258). Every
    // field offset is pinned by static_assert beside rt_gc_mutator_bytes(); a size
    // mismatch means the mirror drifted and every one of those offsets is suspect, so
    // this is the check that turns a silently mis-indexed bitmap into a startup failure.
    if (cfg->mutator_bytes != static_cast<int32_t>(rt_gc_mutator_bytes())) {
        std::snprintf(note, sizeof(note),
                      "the backend computed %d; this runtime computes %u from "
                      "sizeof(rt_gc_mutator)",
                      cfg->mutator_bytes, rt_gc_mutator_bytes());
        gc_fatal("the GC mutator size disagrees with the backend's mirror of it", note);
    }
}

// One summary to stderr, never stdout: §13's first acceptance criterion is
// byte-identical stdout under every --gc-* setting.
void print_stats(uint64_t swept_at_teardown) {
    std::fflush(stdout);
    std::fprintf(stderr,
                 "gc: %" PRIu64 " collections (%" PRIu64 " budget, %" PRIu64 " stress, %" PRIu64
                 " explicit, %" PRIu64 " emergency)\n",
                 g.stats.collections, g.by_reason[RT_GC_REASON_BUDGET],
                 g.by_reason[RT_GC_REASON_STRESS], g.by_reason[RT_GC_REASON_EXPLICIT],
                 g.by_reason[RT_GC_REASON_EMERGENCY]);
    std::fprintf(stderr, "gc: allocated %" PRIu64 " objects / %" PRIu64 " bytes\n",
                 g.stats.objects_allocated, g.stats.bytes_allocated);
    std::fprintf(stderr, "gc: live at last collection %" PRIu64 " objects / %" PRIu64 " bytes\n",
                 g.stats.objects_live, g.stats.bytes_live);
    std::fprintf(stderr,
                 "gc: heap %" PRIu64 " blocks of %u bytes, %" PRIu64 " free; ceiling %" PRIu64
                 " bytes\n",
                 g.stats.blocks_total, RT_GC_BLOCK_BYTES, g.stats.blocks_free,
                 g.cfg.heap_bytes_max);
    std::fprintf(stderr, "gc: %" PRIu64 " bytes of string and container buffers still held\n",
                 g_owned.live);
    std::fprintf(stderr, "gc: finalized %" PRIu64 " by collection, %" PRIu64 " at teardown\n",
                 g.stats.finalized, swept_at_teardown);
    std::fprintf(stderr, "gc: %" PRIu64 " conservative pins over the run\n",
                 g.stats.conservative_pins);
    std::fprintf(stderr, "gc: %" PRIu64 " container roots at the last collection\n",
                 g.stats.container_roots);
    std::fprintf(stderr,
                 "gc: %" PRIu64 " young, %" PRIu64 " full; %" PRIu64 " objects promoted, %" PRIu64
                 " logged by the write barrier\n",
                 g.stats.young_collections, g.stats.full_collections, g.stats.promoted,
                 g.stats.remembered);
    std::fprintf(stderr, "gc: %" PRIu64 " weak references cleared\n", g.stats.weak_cleared);
    std::fflush(stderr);
}

// -- Verification -------------------------------------------------------------
//
// Everything below is CLOTHO_EXTRA_ASSERTS only: §13's heap-verify mode, which is
// pair-assertion discipline (TigerStyle) applied to the whole heap rather than to one
// function. A whole-heap walk per collection is too slow to ship.
#ifdef CLOTHO_EXTRA_ASSERTS

// What one rt_gc_verify() pass accumulates. A struct rather than a growing list of
// out-parameters because every one of these serves a claim that is heap-WIDE -- the live
// accounting the mark phase computed, the size of the remembered set, the block lists --
// and so cannot be checked object by object.
struct VerifyTally {
    uint64_t marked         = 0;
    uint64_t marked_bytes   = 0;
    uint64_t logged         = 0;
    uint64_t blocks_free    = 0;
    uint64_t blocks_young   = 0;
    uint64_t blocks_partial = 0;
};

// One object, found through the metadata. `granules` is the extent the metadata gives
// it, or 0 for a large object, whose extent is its mapping's.
//
// Every object in the heap is intact, marked or not: the sweep erases a dead object
// whole and nothing else frees one. So every claim here is made of every object, and
// only the last depends on its age.
void verify_object(void* h, uint64_t granules, VerifyTally* t) {
    assert(t != nullptr && "the verification tally is heap-wide");
    const uint8_t    m   = *meta_ptr(h);
    const rt_gc_map* map = map_probe(h);
    assert(map != nullptr && "an object in the heap has its vptr stored");
    assert_map(map);
    assert(map->size == (granules != 0 ? granules << kGranuleShift : record_of(h)->large_bytes) &&
           "an object's extent in the metadata is its class's size");
    assert((m & RT_GC_META_PINNED) == 0 && "a pin outlived its collection");
    assert((m & kMetaWasOld) == 0 && "a full collection's note outlived it");
    // The direction that matters is the missing flag: an object whose class emitted a
    // finalizer MUST carry it, or its string and container buffers are never freed.
    // Generated code writes this byte, so this is the only check of what it wrote.
    assert(((m & RT_GC_META_FINALIZE) != 0) == (map->finalize != nullptr) &&
           "FINALIZE disagrees with the class descriptor");
    assert(rt_gc_find_object(h) == h && "an object does not resolve to itself");
    assert(rt_gc_find_object(static_cast<uint8_t*>(h) + map->size - 1) == h &&
           "an object's last byte does not resolve to it");

    const bool marked = (m & RT_GC_META_MARK) != 0;
    const bool logged = (m & RT_GC_META_LOGGED) != 0;
    if (logged) {
        // Counted, not tested against the set: the remembered set holds exactly the LOGGED
        // objects, and verify_totals() compares the two totals once. Totals rather than
        // membership keeps this O(heap) instead of O(heap x set), and it proves the set is
        // duplicate-free as a side effect, since a doubled entry carries one bit and fills
        // two slots.
        assert(marked && "a LOGGED object that is not old");
        t->logged += 1;
    }
    if (marked) {
        t->marked += 1;
        t->marked_bytes += map->size;
    }
    // A weak reference's referent is an object or null, whether or not either is
    // marked: the collection that frees a referent clears every weak reference to it
    // first (clear_weak_referents).
    if (map->weak_referent != 0) {
        void* const referent = *reinterpret_cast<void**>(static_cast<uint8_t*>(h) + map->weak_referent);
        assert((referent == nullptr || rt_gc_find_object(referent) == referent) &&
               "a weak reference names an object that has been freed");
    }
    // The soundness precondition for NOT tracing an un-remembered OLD object, checked
    // against the heap rather than argued in a comment. mark_and_push() leaves such an
    // object alone on the claim that every reference it holds names another old object;
    // a young reference here would be exactly the edge a young collection cannot see. An
    // object whose remembered-set entry is pending is exempt: a store has landed in it
    // that no collection has traced yet.
    const bool settled = marked && !logged;
    uint8_t*   body    = static_cast<uint8_t*>(h);
    for (uint32_t i = 0; i < map->nhandles; ++i) {
        void* child = *reinterpret_cast<void**>(body + map->handles[i]);
        if (child == nullptr) {
            continue;
        }
        assert(rt_gc_find_object(child) == child && "an object holds a handle that names no object");
        assert((!settled || is_marked_now(child)) &&
               "an un-remembered OLD object holds a young reference the barrier missed");
    }
    // The same claim for its container properties, which hold handles exactly as fields
    // do (`old.q.push_back(fresh)` is an old-to-young edge as `old.f = fresh` is). Pushed
    // here and drained once by rt_gc_verify(), through the walkers the mark phase uses,
    // which under g.verifying assert each element marked instead of marking it.
    if (settled) {
        for (uint32_t i = 0; i < map->nleaves; ++i) {
            const rt_elem_leaf& leaf = map->leaves[i];
            assert(leaf.ops != nullptr && "a class leaf has an element descriptor");
            if (is_container_kind(leaf.ops->kind)) {
                push_container(body + leaf.offset, leaf.ops->kind, leaf.ops);
            }
        }
    }
}

// One block's metadata, which GENERATED code writes at every inlined allocation: the
// runtime's own assertions cover only its own entry point, and this is the other half
// of that pair. The metadata must parse as free granules and whole objects, each
// object's later granules holding the distance back that Gc.h says they hold.
void verify_block(BlockRef b, VerifyTally* t) {
    const uint8_t* const meta = b.meta();
    const BlockInfo&     info = b.info();
    uint32_t             used    = 0;
    uint32_t             run     = 0;
    uint32_t             longest = 0;
    uint32_t             i       = 0;
    while (i < kGranulesPerBlock) {
        const uint8_t m = meta[i];
        if (m == 0) {
            ++i;
            ++run;
            longest = run > longest ? run : longest;
            continue;
        }
        run = 0;
        assert((m & RT_GC_META_START) != 0 && "a later granule of an object with no first");
        uint32_t n = 1;
        while (i + n < kGranulesPerBlock && meta[i + n] != 0 && (meta[i + n] & RT_GC_META_START) == 0) {
            assert(meta[i + n] == (n < RT_GC_META_BACK_MAX ? n : RT_GC_META_BACK_MAX) &&
                   "a later granule holds the wrong distance back");
            ++n;
        }
        verify_object(b.base() + (static_cast<uint64_t>(i) << kGranuleShift), n, t);
        used += n;
        i += n;
    }
    assert((info.state != kBlockFree || used == 0) && "a free block holds an object");
    assert((info.state != kBlockFree || info.young == 0) && "a free block is young");
    assert((info.young == 0 || info.state == kBlockOwned) && "a young block no mutator took");
    // What puts a block in its list: stale for a block a mutator has allocated from,
    // which is in no list.
    assert((info.state != kBlockPartial || info.max_run == longest) && "a listed block's longest run");
    assert((info.state != kBlockFull || used == kGranulesPerBlock) && "a full block has a free granule");
    t->blocks_partial += info.state == kBlockPartial ? 1u : 0u;
    t->blocks_free += info.state == kBlockFree ? 1u : 0u;
    t->blocks_young += info.young != 0 ? 1u : 0u;
}

void verify_large(VerifyTally* t) {
    uint64_t mapped = 0;
    for (size_t i = 0; i < g.large.size(); ++i) {
        ChunkRecord* const c = g.large[i];
        assert(c->slot == i && "a large object is listed in its slot");
        assert(c->large_bytes > kLargeObjectBytes && "a small object in the large list");
        uint8_t* const h = reinterpret_cast<uint8_t*>(c) + RT_GC_LARGE_PAYLOAD;
        assert(record_at(reinterpret_cast<uintptr_t>(h) + c->large_bytes - 1) == c &&
               "the table does not cover a large object's last byte");
        verify_object(h, 0, t);
        mapped += RT_GC_LARGE_PAYLOAD + c->large_bytes;
    }
    // A kept mapping holds no object and no address resolves into it.
    for (const auto& entry : g.large_sizes) {
        for (const ChunkRecord* c : entry.second.spare) {
            const uint8_t* const h = reinterpret_cast<const uint8_t*>(c) + RT_GC_LARGE_PAYLOAD;
            assert(c->large_bytes == entry.first && "a kept mapping is listed under its object's size");
            assert(record_at(reinterpret_cast<uintptr_t>(h)) == nullptr && "a kept mapping is in the table");
            assert(*meta_at(h) == 0 && "a kept mapping holds an object");
            mapped += RT_GC_LARGE_PAYLOAD + c->large_bytes;
        }
    }
    assert(mapped == g.large_mapped && "the bytes of large mappings disagree with the count");
}

// §13 from the ROOT side, and the invariant no heap walk in this file can state: a
// freed object's metadata is zero, so it vanishes from every walk above and the only
// remaining witness is the root that still names it.
//
// True BETWEEN collections as well as at the end of one -- which is what lets it live in
// rt_gc_verify(), a function tests call at arbitrary times: design state is never
// recycled and nothing is freed outside a collection, so a root that stops resolving
// names a live object some collection reclaimed.
void verify_roots() {
    for (const rt_gc_roots* t : g.roots) {
        assert(t != nullptr);
        assert(t->nhandles < kRootHandlesMax && "an implausible root table size");
        if (t->nhandles == 0) {
            continue;
        }
        assert(t->base != nullptr && t->offsets != nullptr && "a non-empty table is complete");
        uint8_t* base = static_cast<uint8_t*>(t->base);
        for (uint32_t i = 0; i < t->nhandles; ++i) {
            void* h = *reinterpret_cast<void**>(base + t->offsets[i]);
            assert((h == nullptr || rt_gc_find_object(h) == h) &&
                   "a precise root names a reclaimed object");
        }
    }
    // The same claim for a container root's OWNER, which is the shape whose failure runs
    // a finalizer over a live object's string and container buffers rather than merely
    // dangling one handle.
    for (const auto& kv : g.containers) {
        void* owner = kv.second.owner;
        assert((owner == nullptr || rt_gc_find_object(owner) == owner) &&
               "a container owner was reclaimed");
    }
}

// The heap-wide totals, split from rt_gc_verify() for length (TigerStyle). Every
// claim here is about a counter the walk produced rather than about any one object.
void verify_totals(const VerifyTally& tally) {
    // The pair half of the mark phase's own accounting: the marked objects the walk
    // finds are the objects marking counted. Exact after a FULL collection, which
    // cleared every mark and so marked precisely what it counted, and until the next
    // collection, since nothing between two collections sets or clears a mark. One-sided
    // after a young one, which leaves the old objects it never visited marked and
    // uncounted.
    const bool exact = (g.kind == RT_GC_KIND_FULL);
    assert((exact ? tally.marked == g.stats.objects_live : tally.marked >= g.stats.objects_live) &&
           "the heap walk disagrees on live objects");
    assert((exact ? tally.marked_bytes == g.stats.bytes_live
                  : tally.marked_bytes >= g.stats.bytes_live) &&
           "the heap walk disagrees on live bytes");
    // The remembered set holds EXACTLY the objects carrying LOGGED, and both directions
    // matter: an object logged but not recorded is an old-to-young edge the next young
    // collection will not trace, and a recorded object whose bit was cleared is a barrier
    // that can never fire for it again. After a collection both sides hold precisely what
    // relog_conservative_roots() re-armed; between collections they grow together.
    assert(tally.logged == g.remembered.size() && "a LOGGED object outside the remembered set");
    for (void* h : g.remembered) {
        assert((*meta_ptr(h) & RT_GC_META_LOGGED) != 0 && "a remembered entry lost its bit");
        assert(is_old(h) && "a remembered entry is an OLD object");
    }
    // The generational accounting: the two kinds partition the collections, and every
    // old object was promoted by some collection.
    assert(g.stats.young_collections + g.stats.full_collections == g.stats.collections &&
           "every collection is either young or full");
    assert(g.stats.promoted <= g.stats.objects_allocated && "more promotions than objects");
    assert(tally.marked <= g.stats.promoted && "an OLD object no collection ever promoted");
    // The block accounting: the count, the free list and the walk agree on the free
    // blocks, and the young list holds exactly the blocks flagged young.
    assert(g.stats.blocks_total == g.chunks.size() * kBlocksPerChunk && "a chunk is 119 blocks");
    assert(tally.blocks_free == g.stats.blocks_free && "the free-block count disagrees with the walk");
    assert(g.free_blocks.size() == g.stats.blocks_free && "the free list disagrees with the count");
    assert(tally.blocks_young == g.young.size() && "the young list disagrees with the flags");
    uint64_t partial = 0;
    for (uint32_t k = 0; k < kRunClasses; ++k) {
        for (const BlockRef b : g.partial_blocks[k]) {
            assert(b.info().state == kBlockPartial && run_class(b.info().max_run) == k &&
                   "a block is in the partial list of its longest run");
        }
        partial += g.partial_blocks[k].size();
    }
    assert(partial == tally.blocks_partial && "the partial lists disagree with the walk");
    assert(g.table.size() >= g.chunks.size() + g.large.size() && "a mapping is missing from the table");
    for (size_t i = 0; i < g.chunks.size(); ++i) {
        assert(g.chunks[i]->slot == i && g.chunks[i]->large_bytes == 0 && "a chunk is listed in its slot");
    }
}

#endif // CLOTHO_EXTRA_ASSERTS

}  // namespace

// -- Owned buffers (GcBuffers.h, design §6.5) ---------------------------------

namespace rt_internal {

// Constant-initialized, with no collector to tell: a charge before rt_gc_init, or after
// rt_gc_shutdown, is one add and one compare against a trigger it cannot reach.
OwnedBuffers g_owned = { 0, UINT64_MAX, false };

// A charge crossed the trigger. The first crossing, one budget of growth past the last
// collection, only requests one -- the next region boundary answers it, as it answers an
// exhausted nursery. A second, emergency_bytes past it, means no boundary came: the
// design is allocating buffers in a loop that does not suspend, and the collection that
// frees them runs from the next allocation of an object, which is where their owners are
// made. Emptying the hole is how the inlined fast path is sent there; it is safe because
// the fast path has no call in it, so no runtime function runs between its loads of the
// hole and its store.
void buffers_pressure() {
    if (g.initialized == 0) {
        g_owned.trigger = UINT64_MAX;
        return;
    }
    g_mutator.request = 1;
    if (g_owned.live < g.owned_emergency_at) {
        g_owned.trigger = g.owned_emergency_at;
        return;
    }
    g_owned.trigger = UINT64_MAX;
    if (g.stack_base != nullptr && g_mutator.cursor != nullptr) {
        g_mutator.limit = g_mutator.cursor;
    }
}

// A finalizer of an emergency collection is freeing [p, p + bytes). If a word of the
// stack lies in it, code that is still running holds a pointer into the buffer -- a
// container element it computed before this allocation, say -- and the owner is dead only
// as far as the heap can tell, since nothing can resolve such a pointer to the object that
// owns it. The buffer is kept until a collection finds no word in it. The owner's body is
// reclaimed regardless; the running code never reads it, or it would hold a pointer into
// the body, which the stack scan would have found.
bool buffers_park(void* p, uint64_t bytes) {
    if (!inflight_covers(p, bytes)) {
        return false;
    }
    g.parked.push_back(GcState::Parked{ p, bytes });
    return true;
}

} // namespace rt_internal
// -- Configuration and lifecycle ---------------------------------------------

extern "C" int32_t rt_gc_map_vptr_offset(void) {
    // The backend prepends a `ptr` field to class_desc_ty, so the map pointer sits
    // immediately below the ancestor display: one pointer plus kMaxClassDepth i32
    // display entries below the vptr. -(8 + 4*16) = -72 on a 64-bit host, which is
    // the value rt_gc_init cross-checks against the backend's own computation.
    return -static_cast<int32_t>(sizeof(void*) + 4 * kMaxClassDepth);
}

extern "C" void rt_gc_init(const rt_gc_config* cfg) {
    assert(cfg != nullptr && "rt_gc_init needs a configuration");
    check_backend_mirror(cfg);
    assert(g.initialized == 0 && "rt_gc_init without a matching rt_gc_shutdown");
    // Defensive rather than necessary: rt_gc_shutdown already released everything
    // and the assertion above says so. Releasing again means a double init in a
    // release build leaks nothing.
    release_heap();

    g.cfg = *cfg;
    assert(g.cfg.nursery_bytes > 0 && "a zero nursery budget is a configuration bug");
    if (g.cfg.nursery_bytes < kNurseryBytesFloor) {
        // A tiny budget would request a collection at nearly every allocation and
        // silently duplicate --gc-stress; the floor keeps the two distinguishable.
        g.cfg.nursery_bytes = kNurseryBytesFloor;
    }
    // Zeroed here, and NEVER by rt_gc_shutdown: the test harness runs designs
    // in-process and reads the counters after a run returns, so teardown must leave
    // a usable oracle behind while a fresh run starts from zero.
    g.stats = rt_gc_stats{};
    // The generational state a fresh run starts from (§7): nothing is old, nothing is
    // logged, and the first collection is full -- which choose_kind() reads off
    // stats.full_collections, zeroed just above, rather than from a flag of its own.
    g.kind            = RT_GC_KIND_FULL;
    g.force_full      = 0;
    g.live_after_full = 0;
    // The owned-buffer side of the same fresh start (§6.5), and the emergency threshold the
    // budget implies. Zeroing the count is what makes the clamp in buffer_free matter: a
    // string a previous run left in a runtime global is freed by this one against it.
    g.emergency_bytes  = g.cfg.nursery_bytes > UINT64_MAX / kEmergencyFactor
                           ? UINT64_MAX
                           : g.cfg.nursery_bytes * kEmergencyFactor;
    g.charged_since_gc = 0;
    g.owned_after_full = 0;
    g.stack_base       = nullptr;
    g.stack_lo         = 0;
    g_owned.live       = 0;
    g_owned.parking    = false;
    arm_owned_trigger();
    g.remembered.clear();
    for (uint64_t& n : g.by_reason) {
        n = 0;
    }
    g.worklist.reserve(kWorklistReserve);
    reset_mutator();
    if (g.cfg.stress != 0) {
        // Stress means "collect at every safepoint", and the inlined poll cannot see
        // g.cfg.stress -- it only tests this byte. So arm it here and re-arm it at the
        // end of every collection (rt_gc_collect below asserts that invariant). Nothing
        // else is needed to make stress collect at every safepoint reached; collecting at
        // every allocation too is alloc_object's, once a run has recorded its stack.
        g_mutator.request = 1;
    }
    g.initialized = 1;
    assert(g.chunks.empty() && "a fresh collector owns no chunk");
    assert(g.stats.objects_allocated == 0 && "a fresh collector has allocated nothing");
}

extern "C" void rt_gc_shutdown(void) {
    if (g.initialized == 0) {
        return;  // idempotent: the harness may tear down a run that never started
    }
    // Retire the hole before the sweep, for the same reason rt_gc_collect does: the
    // stats line and the teardown sweep must both see a complete count, and the sweep
    // is about to start clearing the start bits the count is derived from.
    drop_hole(&g_mutator);
    const uint64_t swept = finalize_all_remaining();
    if (g.cfg.stats != 0) {
        print_stats(swept);
    }
    release_heap();
    reset_mutator();
    g.initialized  = 0;
    g.stack_base   = nullptr;
    g_owned.trigger = UINT64_MAX;
    assert(g.chunks.empty() && "shutdown released every chunk");
    assert(g.large.empty() && "shutdown released every large cell");
}

// -- Allocation ---------------------------------------------------------------

extern "C" rt_gc_mutator* rt_gc_mutator_current(void) {
    // One mutator today, reached through a pointer. The indirection is the whole MT
    // story of the allocator (Gc.h:126-136, design §4.3): a baked
    // @clotho.gc.cursor would foreclose multi-threaded simulation, so this must not
    // be "simplified" to a direct global reference at the call sites. Under one
    // mutator it returns the same pointer forever and the load folds away.
    assert(g.initialized != 0 && "rt_gc_mutator_current before rt_gc_init");
    assert(g_mutator.cursor <= g_mutator.limit && "the bump range is well-formed");
    return &g_mutator;
}

extern "C" void* rt_gc_alloc(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map,
                             const rt_alloc_site* site) {
    // The whole allocation sequence, out of line. Generated code inlines the bump and
    // the two bitmap stores and reaches this only for a large object, so in a collected
    // run this entry point serves large objects, the runtime's own callers and the tests
    // (Gc.h:236-245).
    return alloc_object(mut, size, map, site);
}

extern "C" void* rt_gc_alloc_held(uint64_t size, const rt_gc_map* map) {
    assert(g.held == 0 && "a held allocation does not nest");
    g.held             = 1;
    void* const handle = alloc_object(&g_mutator, size, map, nullptr);
    g.held             = 0;
    return handle;
}

extern "C" void* rt_gc_alloc_slow(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map,
                                  const rt_alloc_site* site) {
    // rt_gc_alloc's body behind two extra assertions, deliberately: one function means
    // the runtime's entry point and the inlined fast path's fall-through cannot drift
    // into allocating two different kinds of object.
    //
    // (1) This is the NOT-TAKEN edge of the inlined bump test (design §4.3), so the
    // current hole genuinely could not satisfy the request. If it could have, generated
    // code's mirror of `cursor` / `limit` has drifted from these fields.
    assert((size > kLargeObjectBytes || mut->cursor == nullptr ||
            size > static_cast<uint64_t>(mut->limit - mut->cursor)) &&
           "the inlined fast path fell through on a request that fit");
    // (2) And the five cached fields still describe one block, with `cursor` and
    // `limit` both inside it -- so every [cursor, cursor + size) the fast path handed
    // out before falling through here was inside that one block, and no fast-path
    // allocation straddled two.
    assert_hole_consistent(mut);
    return alloc_object(mut, size, map, site);
}

extern "C" void rt_gc_run_enter(const void* stack_base) {
    assert(stack_base != nullptr && "a run records the stack it runs on");
    g.stack_base = static_cast<const uint8_t*>(stack_base);
}

extern "C" void rt_gc_run_leave(void) {
    g.stack_base = nullptr;
}

// -- Roots --------------------------------------------------------------------

extern "C" void rt_gc_roots_add(const rt_gc_roots* table) {
    assert(table != nullptr && "a root table registration needs a table");
    assert(table->nhandles < kRootHandlesMax && "an implausible root table size");
    assert((table->nhandles == 0 || table->base != nullptr) && "a non-empty table has a base");
    assert((table->nhandles == 0 || table->offsets != nullptr) && "a non-empty table has offsets");
    // Negative space: registering a table twice is harmless to the scan but means
    // the emitted startup sequence ran twice, which is not harmless anywhere else.
    // The table's lifetime is the JIT'd module's; the runtime only borrows it.
    assert(std::find(g.roots.begin(), g.roots.end(), table) == g.roots.end() &&
           "this root table is already registered");
    g.roots.push_back(table);
}

// The registry head lives in the mutator now (Gc.h:178-181), which is what let the
// backend inline this pair: seven instructions against a call, per activation of every
// coroutine whose frame can hold a reference (design §6.4). These two entry points are
// the out-of-line equivalents, kept for the runtime's own callers and the tests, and
// they reach the head through rt_gc_mutator_current() for the same reason the fast path
// loads a mutator pointer rather than a link-time constant: under N mutators the
// registry is per-mutator, and a baked global address would foreclose that.
extern "C" void rt_gc_frame_link(rt_gc_frame* node, void* base, uint64_t bytes) {
    assert(node != nullptr && "a frame registration needs a node");
    assert(base != nullptr && "a live frame has a base");
    assert(bytes >= sizeof(void*) && "a coroutine frame is at least a word");
    rt_gc_mutator* const mut = rt_gc_mutator_current();
    // The node is an alloca inside the coroutine that CoroSplit hoists into the
    // frame (F2), so its contents before this call are undefined and there is
    // nothing to assert about them; everything after this point is asserted.
    node->base  = base;
    node->bytes = bytes;
    node->prev  = nullptr;
    node->next  = mut->frames_head;
    if (mut->frames_head != nullptr) {
        mut->frames_head->prev = node;
    }
    mut->frames_head = node;
    assert(mut->frames_head == node && "the new node heads the registry");
    assert((node->next == nullptr || node->next->prev == node) && "the links are symmetric");
}

extern "C" void rt_gc_frame_unlink(rt_gc_frame* node) {
    assert(node != nullptr && "a frame unlink needs a node");
    rt_gc_mutator* const mut = rt_gc_mutator_current();
    // An O(1) membership check that is complete for a doubly-linked list: a node
    // that is not linked either has a stale prev whose next is not us, or has no
    // prev and is not the head. EVERY path that frees a frame must reach here,
    // including the raw-destroy paths that skip the normal epilogue (Gc.h:128-130):
    // a node left linked after its frame is freed is a use-after-free inside the
    // collector, and this is the pair assertion that catches it.
    if (node->prev != nullptr) {
        assert(node->prev->next == node && "this node is not linked where it claims");
    } else {
        assert(mut->frames_head == node && "this node is not in the registry");
    }
    if (node->next != nullptr) {
        assert(node->next->prev == node && "this node is not linked where it claims");
        node->next->prev = node->prev;
    }
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        mut->frames_head = node->next;
    }
    node->prev = nullptr;
    node->next = nullptr;
}

extern "C" void rt_gc_range_add(const void* base, uint64_t bytes) {
    assert(base != nullptr && "a conservative range needs a base");
    assert(bytes > 0 && "an empty conservative range is a registration bug");
    const uint8_t* p = static_cast<const uint8_t*>(base);
    assert(std::find_if(g.ranges.begin(), g.ranges.end(),
                        [p](const Range& r) { return r.base == p; }) == g.ranges.end() &&
           "this range is already registered");
    g.ranges.push_back(Range{ p, bytes });
}

extern "C" void rt_gc_range_remove(const void* base) {
    assert(base != nullptr && "a range removal needs a base");
    const uint8_t* p   = static_cast<const uint8_t*>(base);
    const auto     pos = std::find_if(g.ranges.begin(), g.ranges.end(),
                                      [p](const Range& r) { return r.base == p; });
    // Negative space: removing a range that was never added means a staging pool is
    // registered under one address and released under another, which would leave a
    // freed buffer in the root set.
    assert(pos != g.ranges.end() && "this range was never registered");
    if (pos != g.ranges.end()) {
        *pos = g.ranges.back();
        g.ranges.pop_back();
    }
}

// The queues are the scheduler's (Scheduler.cpp, arr_queues); only their shape is
// registered here. A duplicate index is a backend bug: two shapes for one buffer.
extern "C" void rt_gc_nba_words_add(uint32_t idx, uint32_t stride) {
    assert(stride >= sizeof(void*) && "a record is at least one word");
    assert(std::none_of(g.nba_queues.begin(), g.nba_queues.end(),
                        [idx](const NbaQueue& q) { return q.idx == idx; }) &&
           "this NBA queue is already registered");
    g.nba_queues.push_back(NbaQueue{ idx, stride, 0, 0, nullptr });
}

extern "C" void rt_gc_nba_container_add(uint32_t idx, uint32_t stride, uint32_t offset,
                                        uint32_t kind, const rt_elem_ops* ops) {
    assert(is_container_kind(kind) && "only a dynarr / queue / assoc is staged whole");
    assert_elem_ops(ops);
    assert(ops->kind == kind && "the kind and the container descriptor disagree");
    assert(offset < stride && "the staged container lies inside its record");
    assert(std::none_of(g.nba_queues.begin(), g.nba_queues.end(),
                        [idx](const NbaQueue& q) { return q.idx == idx; }) &&
           "this NBA queue is already registered");
    g.nba_queues.push_back(NbaQueue{ idx, stride, offset, kind, ops });
}

// -- Containers of class handles ----------------------------------------------

extern "C" void rt_gc_containers_add(const rt_gc_container_table* table) {
    assert(table != nullptr && "a container table registration needs a table");
    assert(table->count < kRootHandlesMax && "an implausible container table size");
    assert((table->count == 0 || table->base != nullptr) && "a non-empty table has a base");
    assert((table->count == 0 || table->entries != nullptr) && "a non-empty table has entries");
#ifndef NDEBUG
    for (uint32_t i = 0; i < table->count; ++i) {
        assert(is_container_kind(table->entries[i].kind) && "an entry names a container");
        assert(table->entries[i].ops != nullptr && "an entry carries a descriptor");
        assert(table->entries[i].kind == table->entries[i].ops->kind &&
               "an entry's kind and descriptor disagree");
    }
#endif
    // Borrowed, never copied: the table is a constant in the JIT'd module and outlives
    // the run, exactly as a root table does (Gc.h:99-100, 246-248).
    g.container_tables.push_back(table);
}

extern "C" void rt_gc_container_add(void* handle, uint32_t kind, const rt_elem_ops* ops) {
    assert(handle != nullptr && "a container registration needs the container handle");
    assert(is_container_kind(kind) && "only a dynarr / queue / assoc is registered");
    assert_elem_ops(ops);
    assert(ops->kind == kind && "the kind and the container descriptor disagree");
    // Whose heap object this container's storage belongs to, if any. Resolved ONCE here
    // rather than at every collection: it costs a binary search per registration, which is
    // per activation or per element init, and it turns the walk in push_container_roots
    // into two loads. Null for a frame-local container, which is every registration the
    // landed phases make -- so this is additive and changes none of them.
    void* const owner = rt_gc_find_object(handle);
    const auto it = g.containers.find(handle);
    if (it != g.containers.end()) {
        // Negative space, and the one direction that is still fatal: one storage ADDRESS is
        // one frame-local container, because that address is a per-activation alloca, so a
        // second registration of one means an emitted init ran without its deinit. A
        // heap-resident container is different in kind -- its address belongs to the owner,
        // so two live activations reffing into it name the same key by construction -- and
        // there the answer is to count, not to complain.
        assert(owner != nullptr && "this frame-local container is already registered");
        assert(it->second.owner == owner && "two registrations disagree on the owner");
        assert(it->second.kind == kind && "two registrations disagree on the kind");
        assert(it->second.ops == ops && "two registrations disagree on the descriptor");
        it->second.refs += 1;
        assert(it->second.refs <= kContainerRefsMax && "a container registration ran away");
        return;
    }
    [[maybe_unused]] const bool inserted =
      g.containers.try_emplace(handle, ContainerReg{ kind, ops, owner, 1 }).second;
    assert(inserted && "the find above said this container is not registered");
    assert_resolves((owner == nullptr || rt_gc_find_object(owner) == owner) &&
                    "a resolved container owner is a class object");
}

extern "C" void rt_gc_container_remove(void* handle) {
    assert(handle != nullptr && "a container removal needs the container handle");
    // O(1) average, and the reason this registry is a hash map where g.ranges is a
    // vector: a frame-local container is registered and removed once per ACTIVATION,
    // not once per run, so a linear scan would be on the call path.
    const auto it = g.containers.find(handle);
    // Negative space in both directions, both silent otherwise. Absent means this container
    // was never registered, so the emitted init / deinit pair is broken: either a live
    // container is not a root (a use-after-free waiting to happen) or a dead one stays
    // one (a leak of its whole element subtree). A double removal arrives here as the
    // removal that finds nothing, and the same assertion catches it.
    assert(it != g.containers.end() && "this container was never registered");
    if (it == g.containers.end()) {
        return;
    }
    assert(it->second.refs > 0 && "a registration with no holder is still in the registry");
    if (it->second.refs > 1) {
        // Another live activation still refs this heap-resident container; it stops being a
        // root only when the last one lets go.
        it->second.refs -= 1;
        return;
    }
    g.containers.erase(it);
}

// -- Collection ---------------------------------------------------------------

// The mutator layout, pinned field by field. Generated code indexes it at constant
// offsets the backend computes from its own mirror: the poll loads `request`, and the
// inlined allocation fast path loads and stores `cursor`, `limit` and `budget_left`. A
// silent reordering here would point one of those at another field, so every offset is
// pinned and so is the total size. Written as "the previous field's offset plus its
// size" rather than as literals: that spells out both the order AND the absence of
// padding, and holds on any host whose DataLayout the backend agrees with.
//
// rt_gc_init cross-checks two of these numbers against the backend's own computation:
// `request` for the poll, and sizeof for the mirror as a whole.
static_assert(offsetof(rt_gc_mutator, cursor) == 0, "the mutator starts with the bump cursor");
static_assert(offsetof(rt_gc_mutator, limit) ==
                offsetof(rt_gc_mutator, cursor) + sizeof(uint8_t*),
              "limit follows cursor");
static_assert(offsetof(rt_gc_mutator, budget_left) ==
                offsetof(rt_gc_mutator, limit) + sizeof(uint8_t*),
              "budget_left follows the bump range");
static_assert(offsetof(rt_gc_mutator, reserved) ==
                offsetof(rt_gc_mutator, budget_left) + sizeof(uint64_t),
              "reserved follows budget_left");
static_assert(offsetof(rt_gc_mutator, request) ==
                offsetof(rt_gc_mutator, reserved) + sizeof(uint32_t),
              "request sits immediately after reserved");
static_assert(offsetof(rt_gc_mutator, frames_head) ==
                offsetof(rt_gc_mutator, request) + sizeof(uint32_t),
              "the two u32s pack, so frames_head needs no padding after the poll byte");
static_assert(sizeof(rt_gc_mutator) ==
                offsetof(rt_gc_mutator, frames_head) + sizeof(struct rt_gc_frame*),
              "frames_head is the last field: the mirror knows every one of them");
// The numbers those expressions come to on the LP64 host the backend targets, spelled
// out so a reader can check the mirror by eye against ir/llvm/RuntimeAbi.cpp's gc_mutator_ty.
static_assert(offsetof(rt_gc_mutator, cursor) == 0 && offsetof(rt_gc_mutator, limit) == 8 &&
                offsetof(rt_gc_mutator, budget_left) == 16 &&
                offsetof(rt_gc_mutator, reserved) == 24 &&
                offsetof(rt_gc_mutator, request) == 28 &&
                offsetof(rt_gc_mutator, frames_head) == 32 && sizeof(rt_gc_mutator) == 40,
              "the LP64 mutator layout the backend mirrors: 0/8/16/24/28/32, 40 bytes");

extern "C" uint32_t rt_gc_mutator_request_offset(void) {
    // 28 on a 64-bit host: two pointers, one u64 budget, one reserved u32.
    return static_cast<uint32_t>(offsetof(rt_gc_mutator, request));
}

extern "C" uint32_t rt_gc_mutator_bytes(void) {
    // 64 on a 64-bit host. rt_gc_init compares this against rt_gc_config::mutator_bytes,
    // which is the backend's sizeof of its own mirror (Gc.h:200-203).
    return static_cast<uint32_t>(sizeof(rt_gc_mutator));
}

extern "C" void rt_gc_safepoint_slow(void) {
    // Reaching here means a poll -- the one generated code inlines, or the one in
    // rt_gc_safepoint below -- saw `request` set, so a collection is due and this one
    // is unconditional (Gc.h:235-240). Called at a scheduler region boundary, the
    // only place with no live JIT frame (design §6.1, F1).
    assert(g_mutator.request != 0 && "the slow half is the taken branch of the poll");
    assert(g.initialized != 0 && "the poll fired with no collector; request is 0 when off");
    // Reason accounting, unchanged from the single-function safepoint: under stress
    // every collection is a stress collection, because stress is what armed the byte
    // and re-arms it; otherwise the only writer is charge_budget().
    const uint32_t reason = (g.cfg.stress != 0) ? RT_GC_REASON_STRESS : RT_GC_REASON_BUDGET;
    rt_gc_collect(reason);
}

extern "C" void rt_gc_safepoint(void) {
    // The poll, out of line: one load, one test, one not-taken branch. Kept for the
    // runtime's own call sites and tests/gc_test.cpp (Gc.h:242-245) while generated
    // code inlines the same two instructions.
    //
    // No `initialized` guard, and that is not an oversight: rt_gc_init and
    // rt_gc_shutdown both route through reset_mutator(), which clears the byte, so
    // `request` is 0 whenever there is no collector to hand the work to.
    assert(g.initialized != 0 || g_mutator.request == 0);
    if (g_mutator.request == 0) {
        return;
    }
    rt_gc_safepoint_slow();
}

extern "C" void rt_gc_collect_kind(uint32_t reason, uint32_t kind) {
    assert(reason <= RT_GC_REASON_EMERGENCY && "an unknown collection reason");
    assert(kind == RT_GC_KIND_YOUNG || kind == RT_GC_KIND_FULL);
    assert(g.initialized != 0 && "rt_gc_collect_kind before rt_gc_init");
    if (g.initialized == 0) {
        return;
    }
    // An overflowed remembered set dropped entries, and that is sound only because the
    // NEXT collection traces everything. So a young collection is upgraded here rather
    // than trusted, whoever asked for it and however explicitly: the caller chooses a
    // kind, and correctness is not one of the things a caller gets to choose.
    if (g.force_full != 0) {
        kind = RT_GC_KIND_FULL;
    }
    // The mutator's bump range points into a block the sweep is about to go through, so
    // it goes FIRST. Not merely tidiness: drop_hole() retires the run, and retirement
    // counts the objects it handed out from the metadata bytes the inlined fast path
    // wrote, which the sweep zeroes for every object it frees.
    drop_hole(&g_mutator);

    // Published before the first mark, because the kind is what mark_and_push() tests,
    // and it stays set through the sweep, which tests it too. One marking path,
    // parameterised here.
    g.kind      = kind;
    g.weak_kept = reason == RT_GC_REASON_EMERGENCY;
    if (kind == RT_GC_KIND_FULL) {
        clear_marks();
    }

    g.stats.objects_live    = 0;
    g.stats.bytes_live      = 0;
    // Per collection, not cumulative: Gc.h:322-326 defines this as the containers
    // consulted in the LAST collection, which is what lets a test distinguish "the
    // container-root path kept this queue's elements alive" from "a conservative frame
    // word happened to pin the same object".
    g.stats.container_roots = 0;
    g.worklist.clear();
    g.container_work.clear();
    if (g.worklist.capacity() < kWorklistReserve) {
        g.worklist.reserve(kWorklistReserve);
    }
    assert(g.pinned.empty() && "the previous collection left a pin behind");
    assert(g.weak_reached.empty() && "the previous collection left a weak reference unexamined");

    mark_from_roots();
    // The marks are complete and nothing is reclaimed yet, which is the one moment to
    // ask whether an object was reached: of a weak reference's referent, and of the
    // event a record of the runtime's waits on.
    if (!g.weak_kept) {
        clear_weak_referents();
    }
    rt_internal::event_gc_release_dead(is_marked_now);
    release_dead_identities();

    // The stack is read, so the words a running activation may be using a buffer through
    // are known: first let go of what an earlier collection kept for them that none of
    // them covers now, then keep what they cover from the finalizers (§6.1). Outside an
    // emergency collection the set is empty and nothing is kept.
    release_parked();
    g_owned.parking = !g.inflight.empty();

    // The sweep runs the finalizers of the objects it frees, which free their buffers.
    sweep();
    g_owned.parking = false;
    // Before unpin_all(), which is what clears the pins this reads. See the function:
    // it carries the barrier obligation for a store a suspended activation has not
    // performed yet, and it is the reason the conservative root set is scanned at all in
    // a young collection.
    relog_conservative_roots();
    unpin_all();

    rearm_after_collection();

    account_collection(reason, kind);
    rt_gc_verify();
}

extern "C" void rt_gc_collect(uint32_t reason) {
    assert(reason <= RT_GC_REASON_EMERGENCY && "an unknown collection reason");
    assert(g.initialized != 0 && "rt_gc_collect before rt_gc_init");
    if (g.initialized == 0) {
        return;
    }
    // The only difference between the two entry points: this one applies the policy, and
    // rt_gc_collect_kind is what the policy resolves to. Tests and --gc-full-every reach
    // the kinds directly; the safepoint reaches them through here.
    rt_gc_collect_kind(reason, choose_kind());
}

// The out-of-line half of the inlined write barrier. Generated code has already tested
// `(meta & (MARK|LOGGED)) == MARK` on the object's metadata byte and taken the cold edge, so reaching
// here means an OLD object has just acquired a reference that a young collection would
// otherwise never look at.
//
// A young object needs no barrier and is filtered here rather than trusted away: it is
// traced in full by every collection, young or not, so logging it would cost a push and
// buy nothing. That also makes the entry point safe for the runtime's own stores, which
// do not have the inlined test in front of them.
extern "C" void rt_gc_remember(void* obj) {
    if (g.initialized == 0) {
        return;  // No live collector (a test that never called rt_gc_init): nothing to remember.
    }
    assert(obj != nullptr && "the barrier fires on a store INTO an object");
    assert_resolves(rt_gc_find_object(obj) == obj && "the barrier logged something not a class object");
    if (!is_old(obj)) {
        return;
    }
    const size_t before = g.remembered.size();
    remember_object(obj);
    // Counted here and NOWHERE else, which is what Gc.h:408-411 promises: the number of
    // objects the WRITE BARRIER logged over the run. The collector's own re-logging
    // (relog_conservative_roots) deliberately does not touch it, so a test can still use
    // this counter to prove the barrier fired rather than a root having found everything.
    if (g.remembered.size() != before) {
        g.stats.remembered += 1;
    }
}

// -- Introspection ------------------------------------------------------------

extern "C" void rt_gc_get_stats(rt_gc_stats* out) {
    assert(out != nullptr && "rt_gc_get_stats needs a destination");
    assert(g.stats.blocks_free <= g.stats.blocks_total && "the block accounting is consistent");
    *out = g.stats;
    // Not kept in g.stats: the first is by_reason's fourth entry and the second is
    // g_owned, which the runtime's own free sites move. Read here, at the end of the run
    // for a test, which is when owned_bytes means "what the design leaked".
    out->emergency_collections = g.by_reason[RT_GC_REASON_EMERGENCY];
    out->owned_bytes           = g_owned.live;
    out->heap_bytes            = heap_bytes();
    // The bytes and objects the current hole has handed out but not yet retired. Folded
    // in here rather than committed, because a read must not mutate the counters:
    // retirement happens at the next hole publication, at every collection and at
    // shutdown, and this keeps a mid-run read exact between those points. It is needed
    // at all because the inlined fast path bumps the cursor without touching g.stats
    // (hole_consumed).
    uint64_t bytes   = 0;
    uint64_t objects = 0;
    hole_consumed(&g_mutator, &bytes, &objects);
    out->bytes_allocated += bytes;
    out->objects_allocated += objects;
    assert(out->bytes_allocated >= g.stats.bytes_allocated && "the in-flight fold wrapped");
    assert(out->objects_allocated >= g.stats.objects_allocated && "the in-flight fold wrapped");
}

extern "C" void* rt_gc_find_object(const void* addr) {
    // Both the conservative-scanning validator and the interior-pointer resolver that
    // makes a `ref` into a class property safe. Each rejection is its own return.
    if (addr == nullptr) {
        return nullptr;  // (1) a null handle is the power-on default, not an object
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    // (2) Outside every chunk and every large mapping: a stack word, a frame pointer, a
    // string buffer, an integer that happens to look like an address, a chandle from
    // DPI code -- which the collector must never trace or reclaim. (3) Inside the heap
    // but in no object: a free granule, a chunk's own record and metadata, or the pages
    // around a large object.
    uint8_t* const start = object_start_of(a);
    if (start == nullptr) {
        return nullptr;
    }
    assert(reinterpret_cast<uintptr_t>(start) % RT_GC_ALIGN == 0 && "an object starts on a granule");
    assert(a >= reinterpret_cast<uintptr_t>(start) && "the walk back passed the address");
    const rt_gc_map* map = map_probe(start);
    if (map == nullptr) {
        // (4) The vptr is not stored yet. Only reachable between rt_gc_alloc
        // returning and the store at ir/llvm/Inst.cpp, lower_inst, where nothing
        // allocates -- but a conservative candidate must not be believed on the
        // strength of its metadata alone, so this is a rejection and not an assert.
        return nullptr;
    }
    assert_map(map);
    assert(a < reinterpret_cast<uintptr_t>(start) + map->size &&
           "an object's metadata covers exactly its class's size");
    return start;
}

extern "C" uint64_t rt_gc_identity(void* handle) {
    if (handle == nullptr) {
        return 0;
    }
    assert_resolves((g.initialized == 0 || rt_gc_find_object(handle) == handle) && "only an object has an identity");
    const auto [it, fresh] = g.identity.try_emplace(handle, g.identity_last + 1);
    if (fresh) {
        g.identity_last += 1;
        g.identified.emplace(g.identity_last, handle);
        g.identity_fresh.push_back(handle);
    }
    return it->second;
}

extern "C" void* rt_gc_identity_object(uint64_t id) {
    if (id == 0) {
        return nullptr;
    }
    const auto it = g.identified.find(id);
    if (it == g.identified.end()) {
        char note[96];
        std::snprintf(note, sizeof note, "identity number %llu", static_cast<unsigned long long>(id));
        gc_fatal("an identity number names no object", note);
    }
    return it->second;
}

extern "C" void rt_gc_verify(void) {
#ifdef CLOTHO_EXTRA_ASSERTS
    if (g.initialized == 0) {
        return;
    }
    assert(g.pinned.empty() && "verify runs outside a collection's pin scope");
    assert(g.worklist.empty() && "verify runs outside the mark loop");
    assert(g.container_work.empty() && "verify runs outside the container walk");
    // Before the heap walk, because a root is the only witness left once a collection has
    // freed a live object: reclaiming it clears the very start bit the walk below iterates
    // over, so the walk cannot see the failure the two parities exist to prevent.
    verify_roots();
    // The container half of §13, through the same walkers with marking replaced by an
    // assertion (mark_container_handle): every non-null handle inside a live container --
    // an element handle, or one decoded out of an associative array's KEY bytes by
    // assoc_visit_image -- must resolve and must already be marked. The roots are pushed
    // here and the class-body leaves by verify_object(), so one drain below covers both.
    const uint64_t container_roots_at_mark = g.stats.container_roots;
    g.verifying                            = 1;
    push_container_roots();
    push_nba_roots();
    push_event_roots();
    push_event_waiting();
    VerifyTally tally;
    for (ChunkRecord* c : g.chunks) {
        for (uint32_t i = 0; i < kBlocksPerChunk; ++i) {
            verify_block(BlockRef{ c, i }, &tally);
        }
    }
    verify_large(&tally);
    drain_containers();
    g.verifying = 0;
    // container_roots describes the COLLECTION, not this verification pass, so the
    // counter the mark phase computed is restored rather than left doubled.
    g.stats.container_roots = container_roots_at_mark;
    assert(g.container_work.empty() && "the verification walk left a container behind");
    verify_totals(tally);
#endif
}
