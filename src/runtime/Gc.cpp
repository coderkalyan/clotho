// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Garbage collection for SystemVerilog class objects (LRM 8.4): the Immix-shaped,
// non-moving, stop-the-world mark-region collector specified in
// docs/design/garbage-collection.md §3-§6. Gc.h is the ABI contract and already
// carries most of the rationale; this file carries the mechanism.
//
// Four properties are worth stating up front, because nearly every decision below
// follows from one of them.
//
// EPOCH PARITY, NOT CLEARING, AND TWO OF THEM. `mark_epoch` is 1 or 2 and toggles at
// every collection; `full_epoch` is 0 or RT_GC_FLAG_OMARK and toggles only at a FULL
// one. A line is marked iff line_mark[i] == mark_epoch; an object is marked iff
// is_marked_now() says so, which is the only mark test in this file. Nothing is ever
// cleared, so a collection costs no preparatory pass over the heap.
//
// A parity is sound only while every collection stamps every object that parity is a
// claim about, and that is why there are two. Lines: reclaim_blocks() zeroes the marks
// of the lines it frees and re-publishes the marks of the lines it keeps, so after every
// reclamation each line reads mark_epoch or 0, and neither equals the next epoch.
// Objects: a young collection deliberately SKIPS an OLD one (mark_and_push), so an old
// object's young parity can be arbitrarily stale -- and a stale value matches any
// fixed-length cycle, which is the soundness bug Gc.h:101-109 records. So an OLD
// object's liveness rides on OMARK against full_epoch, which every FULL collection
// stamps on every object it marks; a stale OMARK is therefore the PREVIOUS full parity
// and reads as unmarked. Lengthening either cycle would not have helped: staleness
// under skipping is unbounded, not two or three collections long.
//
// NO PER-OBJECT SWEEP. Reclamation walks 256 line-mark bytes per 32 KiB block and
// never looks at an object. That is the property that makes Immix's collection
// cost independent of the number of dead objects (Blackburn & McKinley, PLDI 2008
// §3.4; design §4.2), and it is also why a dead object's bytes can outlive it
// inside a line that some live object still occupies. trace_one() validates every
// field it follows for exactly that reason, in release as well as debug.
//
// PINNING IS INERT. RT_GC_FLAG_PINNED is set on every conservatively-reached
// object and never read, because nothing moves in this phase. It exists so
// evacuation stays a policy change (design §8) and so the §13 pin audit has
// something to audit; unpin_all() is what makes "every pin is released" checkable.
//
// COLLECTION HAPPENS ONLY AT A SAFEPOINT. rt_gc_alloc() never collects, however
// far over budget it goes: it runs inside a live JIT frame whose layout is
// CoroSplit's, and the only place with no live JIT frame is a scheduler region
// boundary (design §6.1, F1). Crossing the budget raises mutator.request and the
// heap grows to the next boundary -- the documented unsuspended-allocation hole.
//
// THE SAFEPOINT IS A BYTE. Generated code polls `mutator.request` inline -- a
// load, a test and a not-taken branch -- and calls rt_gc_safepoint_slow() only
// when it is set (Gc.h:144-149, design §6.1-§6.2). So that byte is the whole of
// "a collection is due": charge_budget() raises it when the nursery budget runs
// out, and a stress-mode collection re-arms it at the end of every collection,
// because the inlined poll cannot see g.cfg.stress. A design that allocates
// nothing never sets it, and never pays more than the load and the branch.
#include "Gc.h"

#include "Runtime.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace {

// -- Geometry -----------------------------------------------------------------

// 4 MiB per OS request: 128 blocks, so the aligned-new call is cold (one per
// 4 MiB of allocation) while still being small enough that a design allocating a
// handful of objects does not reserve a fortune.
constexpr uint64_t kChunkBytes     = 4ull << 20;
constexpr uint32_t kBlocksPerChunk = static_cast<uint32_t>(kChunkBytes / RT_GC_BLOCK_BYTES);

// One metadata bit per RT_GC_ALIGN bytes, in each of the two per-block bitmaps.
// Design §4.1 says one bit per 8 bytes; 16 is the right granularity here because
// RT_GC_ALIGN is the object alignment, so every object start is a multiple of it
// and a finer bitmap could only ever hold zeroes in its odd bits.
//
// The object-start bitmap and the finalizable bitmap share this geometry exactly --
// the same bit index for the same address (Gc.h:169-174) -- so they share the
// constant too. Generated code computes that index as `((addr - block_base) >> 4)`
// and can use it for both only because the two shapes are one shape.
constexpr uint32_t kGrainsPerBlock = RT_GC_BLOCK_BYTES / RT_GC_ALIGN;
constexpr uint32_t kBitmapBytes    = kGrainsPerBlock / 8;
constexpr uint32_t kGrainsPerLine  = RT_GC_LINE_BYTES / RT_GC_ALIGN;

// The ancestor-display length the backend emits into class_desc_ty, from
// src/ir/Module.h:602 (`inline constexpr uint32_t kMaxClassDepth = 16`). The map
// pointer the backend prepends therefore sits one pointer plus this many i32
// display entries below the vptr. rt_gc_init cross-checks the result against the
// backend's own DataLayout computation, so a divergence fails loudly rather than
// reading garbage.
constexpr uint32_t kMaxClassDepth = 16;

// Objects above a quarter block go to the large-object list. The threshold and its
// rationale are Gc.h:42-52: it is published there rather than here because the
// INLINED allocation fast path has to skip large objects, so the backend needs the
// same number and a second definition of it would be a silent divergence.
constexpr uint64_t kLargeObjectBytes = RT_GC_LARGE_OBJECT_BYTES;

// mutator.block_index when the mutator owns no block. Not 0, which is a valid id.
constexpr uint32_t kNoBlock = 0xffffffffu;

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

static_assert(RT_GC_BLOCK_BYTES % RT_GC_LINE_BYTES == 0, "a block is whole lines");
static_assert(RT_GC_LINES_PER_BLOCK == 256, "256 line marks per block");
static_assert(RT_GC_LINE_BYTES % RT_GC_ALIGN == 0, "a line is whole object grains");
static_assert(kGrainsPerBlock == 2048, "2048 metadata bits per block, per bitmap");
static_assert(kBitmapBytes == 256, "256 bitmap bytes per block, per bitmap");
// The most load-bearing geometry fact in this file: 128 / 16 == 8 means one line
// maps to exactly one BYTE of each bitmap, so reclaiming a line clears its object
// starts and its finalizable bits with one byte store each, and both arrays are
// indexed by line number.
static_assert(kGrainsPerLine == 8, "one bitmap byte per line, in each bitmap");
static_assert(kBitmapBytes == RT_GC_LINES_PER_BLOCK, "the bitmaps are indexed by line");
static_assert(kChunkBytes % RT_GC_BLOCK_BYTES == 0, "a chunk is whole blocks");
static_assert(kBlocksPerChunk == 128, "128 blocks per 4 MiB chunk");
static_assert(RT_GC_HEADER_BYTES == sizeof(uint64_t), "the header is one gcword");
static_assert(RT_GC_HEADER_BYTES < RT_GC_ALIGN, "the header cannot spill an object grain");
static_assert(RT_GC_ALIGN >= alignof(void*), "a handle word must be naturally aligned");
static_assert((RT_GC_ALIGN & (RT_GC_ALIGN - 1)) == 0, "RT_GC_ALIGN is a power of two");
static_assert((RT_GC_BLOCK_BYTES & (RT_GC_BLOCK_BYTES - 1)) == 0, "block size is a power of two");
static_assert(RT_GC_MARK_MASK == 0x3ull, "two mark bits: epochs 1 and 2 plus never-marked");
static_assert((RT_GC_MARK_MASK & RT_GC_FLAG_PINNED) == 0, "the pin bit is not a mark bit");
static_assert((RT_GC_MARK_MASK & RT_GC_FLAG_OMARK) == 0, "OMARK is not a young mark bit");
static_assert(RT_GC_FLAG_OMARK < (1ull << RT_GC_PAYLOAD_SHIFT), "OMARK is below the payload");
static_assert(kLargeObjectBytes % RT_GC_ALIGN == 0, "the large threshold is grain-aligned");
static_assert(kLargeObjectBytes < RT_GC_BLOCK_BYTES, "a block-resident object fits in a block");

// -- Heap structures ----------------------------------------------------------

// Per-block metadata, held in a side array owned by the chunk and NEVER inside the
// block payload: the payload is entirely objects, so an object's address alone
// gives its block and a mutator writing metadata never dirties a line it is also
// allocating into. Both mark arrays are bytes, not bits, so parallel marking will
// need no CAS -- two threads racing to mark the same line write the same value
// (design §4.1, the cheapest MT accommodation available).
struct BlockMeta {
    uint8_t line_mark[RT_GC_LINES_PER_BLOCK];
    // "This line holds an OLD survivor": re-derived at every FULL collection, sticky
    // across young ones. Why it has to exist: reclamation frees a line whose
    // line_mark misses the current epoch, and a young collection never marks an
    // un-remembered OLD object, so without this record the first young collection
    // after a promotion hands out the lines of every old object in the heap
    // (Gc.h:88-91). reclaim_blocks() holds the exact predicate.
    //
    // A byte array beside line_mark rather than an epoch stamp inside it, though the
    // stamp is there for the taking: after a reclamation every line reads {epoch, 0},
    // so at the NEXT reclamation the previous epoch's value would itself mean "not
    // free last time", which is nearly this predicate. Rejected because marking
    // overwrites line_mark -- the protection state would be unobservable from the end
    // of the mark phase onwards, and hazard 1, the one that must never regress, could
    // then not be asserted after a collection at all. 256 B per 32 KiB block: 0.78%,
    // the same as line_mark, and the two are walked together in one pass.
    uint8_t line_old[RT_GC_LINES_PER_BLOCK];
    uint8_t start_bits[kBitmapBytes];
    // The finalizable bitmap: the same geometry as start_bits, written by the same
    // inlined store sequence (Gc.h:169-174), bit `(addr - block_base) >> 4` set iff
    // an object starts at `addr` AND its class emitted a finalizer. It replaced a
    // std::vector<void*> of handles, which no inlined fast path can push onto -- and
    // "inline only the classes without a finalizer" would have inlined almost
    // nothing, because a UVM-shaped object nearly always has a `string` property
    // whose buffer only the finalizer frees (design §0's second-order leak).
    uint8_t  fin_bits[kBitmapBytes];
    uint16_t hole_cursor;   // first line not yet handed out, for find_hole
    uint16_t lines_marked;  // marked lines at the last reclamation
    uint8_t  flags;         // one of the kBlock* states below
    // Whether fin_bits may hold a set bit, so the finalizer sweep can skip a block
    // whole instead of scanning 256 bitmap bytes to find nothing. Conservative in one
    // direction only: publish_hole() sets it for every block it hands a hole out of,
    // because the inlined fast path holds `fin_bitmap` and not this byte and so
    // cannot set it itself, and reclaim_blocks() makes it exact again from the bitmap
    // it already walks. sweep_finalizable() asserts the negative space -- a clear
    // flag means a clear bitmap -- which turns "the mutator set a bit in a block the
    // sweep skipped" from a leaked finalizer into a failed assertion.
    uint8_t has_finalizable;
};

// Block states. A block is in exactly one of the collector's two free lists
// (kBlockFree, kBlockRecycled), owned by a mutator, or full.
enum : uint8_t {
    kBlockFree     = 0,  // no marked line: available whole, and its bitmap is clear
    kBlockRecycled = 1,  // some marked lines and some holes
    kBlockFull     = 2,  // no free line
    kBlockOwned    = 3,  // handed to a mutator; in neither list until the next reclaim
};

// The two views of a block. `blocks` is append-only so a block id is stable, which
// matters because `chunks` is kept sorted by base and an insertion in the middle
// would renumber a flat chunk-relative id under a live mutator.
struct Block {
    uint8_t*   base;
    BlockMeta* meta;
};

struct Chunk {
    uint8_t*   base;         // RT_GC_BLOCK_BYTES-aligned, kChunkBytes long
    BlockMeta* meta;         // kBlocksPerChunk entries, owned
    uint32_t   first_block;  // stable Block id of the block at `base`
};

// A large object owns its cell. `bytes` is kept because ::operator delete with an
// alignment wants the request it matches, and because reclamation reads the extent
// without touching the (possibly already finalized) object.
struct LargeCell {
    uint8_t* start;
    uint64_t bytes;
};

// A runtime-owned region scanned conservatively: the NBA and intra-assignment
// staging pools hold byte images that may contain a handle and are described by no
// emitted root table (Gc.h:202-205).
struct Range {
    const uint8_t* base;
    uint64_t       bytes;
};

// One pending handle-bearing container. `ops` is the descriptor of the CONTAINER
// type -- kind rt_elem_dynarr / rt_elem_queue / rt_elem_assoc -- so `ops->inner` is
// the element descriptor and `ops->keyhandles` names the class handles inside one
// normalized associative KEY span (Runtime.h:919-927). That is what makes a registered
// root, a class-body leaf and a nested container element the same triple, and it keeps
// the key table off the element descriptor, which the backend memoizes by element TYPE
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
    // 1 or 2, toggled at EVERY collection: the young mark, and the only one that means
    // anything for an object no collection has promoted yet.
    uint64_t mark_epoch  = 1;
    // 0 or RT_GC_FLAG_OMARK, toggled ONLY by a full collection, and spelled as the bit
    // itself so the test and the stamp are one mask each. This is what makes an OLD
    // object's liveness readable at all: mark_epoch is stamped by collections that skip
    // old objects and so goes unboundedly stale on them, while every FULL collection
    // stamps this one on every object it marks (is_marked_now, and the note at the top).
    uint64_t full_epoch  = 0;
    int32_t  initialized = 0;
    // Per-reason collection counts for the shutdown stats line. Indexed by
    // RT_GC_REASON_*, which is why the array is exactly three long.
    uint64_t by_reason[3] = { 0, 0, 0 };
    // The KIND of the collection now running (RT_GC_KIND_*): collection-scoped state,
    // exactly as mark_epoch is. A field rather than a parameter threaded through
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

    std::vector<Chunk>     chunks;           // sorted by base; binary-searched
    std::vector<Block>     blocks;           // append-only; index == stable block id
    std::vector<uint32_t>  free_blocks;      // rebuilt per reclamation, so duplicate-free
    std::vector<uint32_t>  recycled_blocks;  // likewise
    std::vector<LargeCell> large;            // sorted by start; binary-searched

    std::vector<const rt_gc_roots*> roots;  // borrowed: the JIT'd module owns them
    std::vector<Range>              ranges;

    // Where the mutator's current hole began. The accounting the inlined fast path
    // cannot do: it bumps `cursor` and sets a start bit, but the mutator struct the
    // backend mirrors holds no counters (Gc.h:163-182) and adding one would put a
    // second store on the hot path to serve a shutdown stats line. So
    // bytes_allocated and objects_allocated are DERIVED at hole retirement, from the
    // cursor and from the start bits over [hole_start, cursor) -- see
    // hole_consumed(). Runtime-side rather than in the mutator for the mirror image
    // of the reason frames_head moved the other way: this is the collector's
    // bookkeeping and no emitted instruction touches it.
    uint8_t* hole_start = nullptr;

    std::vector<void*> worklist;  // the mark stack; never recursion
    std::vector<void*> pinned;    // collection-scoped pins, cleared at the end

    // Containers of class handles (§3.3, Gc.h:226-268). `container_tables` is
    // borrowed exactly as `roots` is: the JIT'd module owns the constant.
    // `containers` is the dynamically-scoped registry, keyed by handle address so a
    // removal is a hash erase rather than a scan of every live activation's locals.
    // `container_work` is the container half of the mark fixpoint.
    std::vector<const rt_gc_container_table*>         container_tables;
    ankerl::unordered_dense::map<void*, ContainerReg> containers;
    std::vector<PendingContainer>                     container_work;
    // Set only for the duration of rt_gc_verify(), which reuses the container
    // walkers with marking replaced by an assertion, so the ring and associative
    // arithmetic has exactly one implementation and cannot drift from itself.
    int32_t verifying = 0;
};

GcState g;

// The single mutator. Reached only through rt_gc_mutator_current(); see the
// comment there for why the indirection must not be folded away by hand.
//
// `block_index` starts at kNoBlock rather than at the zero a default initializer
// would give it: zero is a VALID block id, so a zero-initialized mutator claims to
// own block 0 while holding a null cursor, and the empty-hole invariant
// (assert_hole_consistent) is false before rt_gc_init has run. rt_gc_init itself
// reaches drop_hole through release_heap, so it is the first thing to observe it.
rt_gc_mutator g_mutator{ .cursor       = nullptr,
                         .limit        = nullptr,
                         .budget_left  = 0,
                         .block_index  = kNoBlock,
                         .request      = 0,
                         .block_base   = nullptr,
                         .start_bitmap = nullptr,
                         .fin_bitmap   = nullptr,
                         .frames_head  = nullptr };

// -- Loud failure -------------------------------------------------------------

// The shape of alloc_fatal (Runtime.cpp:4116-4155) without its fmt styling and
// without a source location: rt_gc_alloc carries no rt_alloc_site, so there is no
// statement to name and no caret to draw. Everything else matches, including the
// exit path, because the failure mode is the same one.
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
    // Same reasoning as alloc_fatal (Runtime.cpp:4152-4154): there is no defined
    // state to hand back mid-instant, and a nonzero status distinguishes this from
    // a clean run.
    std::exit(1);
}

// rt_gc_alloc must never return null: the ClassNew site elides the null guard on a
// fresh handle (LlvmBackend.cpp:11706-11712), so a null return would be a silent
// wrong-code bug rather than a diagnosable one (Gc.h:189-192, design §4.3).
[[noreturn]] void oom_fatal(uint64_t size) {
    char head[192];
    std::snprintf(head, sizeof(head),
                  "out of memory for this object: the collector could not obtain %" PRIu64
                  " bytes",
                  size);
    char note[192];
    std::snprintf(note, sizeof(note),
                  "the class heap holds %" PRIu64 " blocks of %u bytes, %" PRIu64 " of them free",
                  g.stats.blocks_total, RT_GC_BLOCK_BYTES, g.stats.blocks_free);
    gc_fatal(head, note);
}

// -- Header and descriptor access ---------------------------------------------

// A handle points at the vptr; the gcword is the word below it (Gc.h:41-46).
uint64_t* gcword_ptr(void* handle) {
    assert(handle != nullptr);
    assert(reinterpret_cast<uintptr_t>(handle) % RT_GC_ALIGN == RT_GC_HEADER_BYTES);
    return reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(handle) - RT_GC_HEADER_BYTES);
}

uint8_t* object_start(void* handle) {
    assert(handle != nullptr);
    assert(reinterpret_cast<uintptr_t>(handle) % RT_GC_ALIGN == RT_GC_HEADER_BYTES);
    return static_cast<uint8_t*>(handle) - RT_GC_HEADER_BYTES;
}

void* handle_of(uint8_t* start) {
    assert(start != nullptr);
    assert(reinterpret_cast<uintptr_t>(start) % RT_GC_ALIGN == 0);
    return start + RT_GC_HEADER_BYTES;
}

// -- The one mark test --------------------------------------------------------
//
// Whether THIS collection has marked an object, in exactly one implementation, because a
// second opinion about liveness is how a live object gets freed (Gc.h:101-109). Marking,
// promotion, both finalizer sweeps, the large-object sweep and rt_gc_verify() all read
// it here.
//
// Decomposed into its two terms rather than written as one three-armed test, because the
// consumers genuinely want different halves of it:
//
//   spared_by_age -- an OLD object in a YOUNG collection. Such a collection never looks
//                    at it, so it has no opinion: the object is live-and-opaque, its
//                    lines are held by line_old and the sweeps must spare it.
//   is_stamped    -- a mark some collection actually wrote, read against the parity of
//                    the collections that write it. The young parity for an object no
//                    collection has promoted -- 0 or the current epoch, so at most one
//                    collection stale -- and OMARK for an OLD one, written by every FULL
//                    collection over every object it marks, so a stale value is the
//                    previous full parity and reads as unmarked. That last part is also
//                    what makes old garbage collectable at all.
//
// is_marked_now() is the disjunction, and it is what every FREE-or-FINALIZE decision
// asks. is_stamped() is the stronger, kind-independent fact -- "a real mark reached this
// object" -- and it is what rt_gc_verify()'s heap walk asks, because a young
// collection's having no opinion is not evidence that an object's extent is intact.
bool spared_by_age(uint64_t gcword) {
    assert(g.kind == RT_GC_KIND_YOUNG || g.kind == RT_GC_KIND_FULL);
    return (gcword & RT_GC_FLAG_OLD) != 0 && g.kind == RT_GC_KIND_YOUNG;
}

bool is_stamped(uint64_t gcword) {
    assert(g.mark_epoch == 1 || g.mark_epoch == 2);
    assert(g.full_epoch == 0 || g.full_epoch == RT_GC_FLAG_OMARK);
    if ((gcword & RT_GC_FLAG_OLD) != 0) {
        return (gcword & RT_GC_FLAG_OMARK) == g.full_epoch;
    }
    return (gcword & RT_GC_MARK_MASK) == g.mark_epoch;
}

bool is_marked_word(uint64_t gcword) {
    return spared_by_age(gcword) || is_stamped(gcword);
}

bool is_marked_now(void* handle) {
    return is_marked_word(*gcword_ptr(handle));
}

// The stamp is_marked_now() reads, so the two cannot drift. BOTH parities go down
// unconditionally, and writing OMARK in a young collection is not a mistake: full_epoch
// then holds the LAST full collection's parity, which is exactly the value the NEXT full
// one reads as unmarked. That is what makes promotion by a young collection safe. An
// object promoted between two full collections must not look already-marked to the full
// collection that has to trace it, and a never-written OMARK bit would look exactly that
// on every second full collection.
uint64_t stamp_marked(uint64_t gcword) {
    constexpr uint64_t both = RT_GC_MARK_MASK | RT_GC_FLAG_OMARK;
    const uint64_t     out  = (gcword & ~both) | g.mark_epoch | g.full_epoch;
    assert(is_stamped(out) && "the stamp does not satisfy the mark test");
    assert((out & ~both) == (gcword & ~both) && "the stamp disturbed another flag");
    return out;
}

// Whether the object has been promoted: it survived a collection, so a YOUNG
// collection treats it as live without tracing into it and its lines are held by
// line_old rather than by a mark (§7). Read only by assertions.
[[maybe_unused]] bool is_old(void* handle) {
    assert(handle != nullptr);
    return (*gcword_ptr(handle) & RT_GC_FLAG_OLD) != 0;
}

// Non-asserting descriptor read: returns null when the vptr has not been stored
// yet. Generated code stores it immediately after rt_gc_alloc returns
// (LlvmBackend.cpp:11696) and no safepoint can intervene, so the collector never
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

// Split assertions over one map (TigerStyle). Compiles to nothing under
// NDEBUG, so it is called freely from the marking path.
void assert_map([[maybe_unused]] const rt_gc_map* map) {
    assert(map != nullptr && "every class emits a clotho.classgc constant");
    assert(map->size >= RT_GC_HEADER_BYTES + sizeof(void*) && "header plus vptr at minimum");
    assert(map->size % RT_GC_ALIGN == 0 && "map->size is rounded up to RT_GC_ALIGN");
    assert((map->nhandles == 0 || map->handles != nullptr) && "nhandles implies a table");
    assert((map->nleaves == 0) == (map->leaves == nullptr) && "nleaves implies a table");
    // A managed property is exactly what puts a leaf in this table AND what makes the
    // class emit clotho.classfini, so a leaf table without a finalizer means the two
    // emissions disagree. Tracing a container leaf depends on it: finalization is
    // where sweep_finalizable() drops a dead object's start bit, and that is what
    // guarantees the leaf bytes of any object the collector can still reach are
    // still that object's own.
    assert((map->nleaves == 0 || map->finalize != nullptr) && "managed leaves imply a finalizer");
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

// The chunk containing `a`, or null. A binary search over the base-sorted chunk
// vector: O(log chunks), and deliberately so. This runs only on the
// conservative-scanning and interior-pointer paths -- never on allocation, which
// bumps a cursor, and never on precise marking, which already holds the object
// start. A 256 MiB heap is 64 chunks, so six halvings.
const Chunk* chunk_of(uintptr_t a) {
    size_t                    lo    = 0;
    size_t                    hi    = g.chunks.size();
    [[maybe_unused]] uint32_t steps = 0;
    while (lo < hi) {
        ++steps;
        assert(steps <= 64 && "a binary search over a vector cannot exceed 64 halvings");
        const size_t mid = lo + (hi - lo) / 2;
        if (reinterpret_cast<uintptr_t>(g.chunks[mid].base) <= a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return nullptr;  // below every chunk
    }
    const Chunk& c = g.chunks[lo - 1];
    if (a - reinterpret_cast<uintptr_t>(c.base) >= kChunkBytes) {
        return nullptr;  // in the gap above the last chunk at or below `a`
    }
    return &c;
}

struct BlockHit {
    uint8_t*   base;
    BlockMeta* meta;
    uint32_t   block_id;
};

bool block_of(uintptr_t a, BlockHit* out) {
    assert(out != nullptr);
    const Chunk* c = chunk_of(a);
    if (c == nullptr) {
        return false;
    }
    const uint64_t off = a - reinterpret_cast<uintptr_t>(c->base);
    const uint32_t i   = static_cast<uint32_t>(off / RT_GC_BLOCK_BYTES);
    assert(i < kBlocksPerChunk && "chunk_of already bounded the offset");
    out->base     = c->base + static_cast<uint64_t>(i) * RT_GC_BLOCK_BYTES;
    out->meta     = &c->meta[i];
    out->block_id = c->first_block + i;
    return true;
}

// The start of the large cell containing `a`, or null. Same binary search shape as
// chunk_of over the start-sorted cell vector; one object per cell, so the cell's
// start IS the object's start.
uint8_t* large_start_of(uintptr_t a) {
    size_t                    lo    = 0;
    size_t                    hi    = g.large.size();
    [[maybe_unused]] uint32_t steps = 0;
    while (lo < hi) {
        ++steps;
        assert(steps <= 64 && "a binary search over a vector cannot exceed 64 halvings");
        const size_t mid = lo + (hi - lo) / 2;
        if (reinterpret_cast<uintptr_t>(g.large[mid].start) <= a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return nullptr;
    }
    const LargeCell& cell = g.large[lo - 1];
    if (a - reinterpret_cast<uintptr_t>(cell.start) >= cell.bytes) {
        return nullptr;
    }
    return cell.start;
}

// The start of the block-resident object containing `a`, or null. Factored out of
// rt_gc_find_object because it is the half that also holds at allocation time,
// before generated code has stored the vptr.
uint8_t* block_start_of(uintptr_t a) {
    BlockHit hit{};
    if (!block_of(a, &hit)) {
        return nullptr;  // outside every chunk: a stack word, a string, a chandle
    }
    if (hit.meta->flags == kBlockFree) {
        return nullptr;  // a free block holds no object, and its bitmap is clear
    }
    // Walk the candidate down to its object grain. An object start is
    // RT_GC_ALIGN-aligned, a handle is that plus RT_GC_HEADER_BYTES, and an
    // interior pointer from a `ref` can name any byte in between.
    const uintptr_t base  = reinterpret_cast<uintptr_t>(hit.base);
    const uint32_t  grain = static_cast<uint32_t>((a - base) / RT_GC_ALIGN);
    assert(grain < kGrainsPerBlock && "block_of bounded the offset");
    // Nearest set start bit at or below `grain`, scanned a bitmap BYTE at a time:
    // one test covers a whole line (kGrainsPerLine == 8), so an empty run of lines
    // costs one load per 128 bytes rather than one per 16. Bounded by
    // kBitmapBytes iterations, since byte_i only ever decreases to 0.
    uint32_t byte_i = grain / 8;
    uint8_t  mask   = static_cast<uint8_t>((1u << ((grain % 8) + 1)) - 1);
    while (true) {
        const unsigned bits = hit.meta->start_bits[byte_i] & mask;
        if (bits != 0) {
            const uint32_t bit = static_cast<uint32_t>(std::bit_width(bits)) - 1;
            return hit.base + (static_cast<uint64_t>(byte_i) * 8 + bit) * RT_GC_ALIGN;
        }
        if (byte_i == 0) {
            return nullptr;  // nothing starts at or below: `a` precedes every object
        }
        --byte_i;
        mask = 0xffu;
    }
}

// Bitmap-only resolution, block or large. Used by the allocation-time half of the
// validate-on-allocate / validate-on-trace pair, which cannot go through
// rt_gc_find_object because that needs the map and hence the vptr.
[[maybe_unused]] uint8_t* object_start_of(uintptr_t a) {
    uint8_t* start = block_start_of(a);
    return start != nullptr ? start : large_start_of(a);
}

// -- Line marks ---------------------------------------------------------------
//
// The two BITMAPS are no longer written here. Generated code sets an object's start
// bit and its finalizable bit inline, indexing the mutator's cached `start_bitmap` /
// `fin_bitmap` with no lookup (design §4.3), so the runtime's own writer is
// set_object_bits() below -- which indexes the same cached fields the same way, so
// that the arithmetic has one implementation rather than one per caller. Clearing a
// single object's bits happens only in sweep_finalizable(), which already holds the
// BlockMeta and the bit index and so needs no lookup either.

// Mark every line the object's [start, start + size) range overlaps. Marking all
// of them, not just the first, is what lets reclaim_blocks() free a line without
// consulting a single object: a live object's whole extent is in marked lines.
void mark_lines(void* handle, uint64_t size) {
    assert(size >= RT_GC_HEADER_BYTES + sizeof(void*));
    uint8_t* start = object_start(handle);
    BlockHit hit{};
    if (!block_of(reinterpret_cast<uintptr_t>(start), &hit)) {
        // A large object owns its cell and has no line marks; its liveness is the
        // gcword alone and reclaim_large() sweeps it per object.
        assert(large_start_of(reinterpret_cast<uintptr_t>(start)) == start &&
               "an object is either block-resident or a large cell");
        return;
    }
    const uint64_t off = static_cast<uint64_t>(start - hit.base);
    const uint32_t lo  = static_cast<uint32_t>(off / RT_GC_LINE_BYTES);
    const uint32_t hi  = static_cast<uint32_t>((off + size - 1) / RT_GC_LINE_BYTES);
    assert(hi < RT_GC_LINES_PER_BLOCK && "an object never spans out of its block");
    for (uint32_t i = lo; i <= hi; ++i) {
        hit.meta->line_mark[i] = static_cast<uint8_t>(g.mark_epoch);
    }
}

// -- Heap growth --------------------------------------------------------------

void chunk_insert_sorted(const Chunk& c) {
    const auto pos = std::lower_bound(g.chunks.begin(), g.chunks.end(), c.base,
                                      [](const Chunk& e, const uint8_t* b) { return e.base < b; });
    // Negative space: each chunk is one aligned-new request, so chunks are disjoint
    // by construction and an overlap would corrupt every lookup above.
    if (pos != g.chunks.begin()) {
        assert((pos - 1)->base + kChunkBytes <= c.base && "chunks overlap");
    }
    if (pos != g.chunks.end()) {
        assert(c.base + kChunkBytes <= pos->base && "chunks overlap");
    }
    g.chunks.insert(pos, c);
}

// One 4 MiB chunk from the OS, or false. The aligned-`operator new` route rather
// than aligned_alloc for the reason rt_alloc documents at Runtime.cpp:5506-5512:
// aligned_alloc is absent from the MSVC runtime. RT_GC_BLOCK_BYTES alignment is
// what makes every block base 32 KiB-aligned, which the index arithmetic in
// block_of() and the grain arithmetic in block_start_of() both require.
bool grow_heap() {
    void* raw = ::operator new(static_cast<size_t>(kChunkBytes),
                               std::align_val_t{ RT_GC_BLOCK_BYTES }, std::nothrow);
    if (raw == nullptr) {
        return false;
    }
    assert(reinterpret_cast<uintptr_t>(raw) % RT_GC_BLOCK_BYTES == 0 && "aligned new honoured");
    BlockMeta* meta = new (std::nothrow) BlockMeta[kBlocksPerChunk];
    if (meta == nullptr) {
        ::operator delete(raw, std::align_val_t{ RT_GC_BLOCK_BYTES });
        return false;
    }
    // Zero is the correct initial state for all of it: kBlockFree, hole_cursor 0, no
    // marked line under either epoch, both bitmaps empty, and has_finalizable clear.
    std::memset(meta, 0, sizeof(BlockMeta) * kBlocksPerChunk);

    Chunk c{ static_cast<uint8_t*>(raw), meta, static_cast<uint32_t>(g.blocks.size()) };
    for (uint32_t i = 0; i < kBlocksPerChunk; ++i) {
        g.blocks.push_back(
          Block{ c.base + static_cast<uint64_t>(i) * RT_GC_BLOCK_BYTES, &meta[i] });
        g.free_blocks.push_back(c.first_block + i);
    }
    chunk_insert_sorted(c);
    g.stats.blocks_total += kBlocksPerChunk;
    g.stats.blocks_free += kBlocksPerChunk;
    assert(g.blocks.size() == g.stats.blocks_total && "block ids are dense and stable");
    return true;
}

// -- Allocation ---------------------------------------------------------------

// The five-field hole invariant the INLINED fast path depends on (Gc.h:168-177):
// either the mutator owns no hole and all five fields are empty, or it owns one and
// all five describe THE SAME block. Generated code indexes `start_bitmap` and
// `fin_bitmap` by `(addr - block_base) >> 4` with no lookup and no bounds check, so a
// `block_base` from one block beside a bitmap from another would silently corrupt a
// third block's metadata. Every writer of these fields ends by calling this.
void assert_hole_consistent([[maybe_unused]] const rt_gc_mutator* mut) {
#ifndef NDEBUG
    assert(mut != nullptr && "the mutator context exists");
    if (mut->cursor == nullptr) {
        assert(mut->limit == nullptr && "an empty hole has no limit");
        assert(mut->block_base == nullptr && "an empty hole names no block");
        assert(mut->start_bitmap == nullptr && "an empty hole names no start bitmap");
        assert(mut->fin_bitmap == nullptr && "an empty hole names no finalizable bitmap");
        assert(mut->block_index == kNoBlock && "an empty hole owns no block id");
        return;
    }
    assert(mut->block_index < g.blocks.size() && "a live hole names a real block");
    const Block& blk = g.blocks[mut->block_index];
    assert(mut->block_base == blk.base && "the cached base is this block's");
    assert(mut->start_bitmap == blk.meta->start_bits && "the cached start bitmap is wrong");
    assert(mut->fin_bitmap == blk.meta->fin_bits && "the cached fin bitmap is not this block's");
    assert(mut->cursor >= mut->block_base && "the bump range starts inside the block");
    assert(mut->cursor <= mut->limit && "the bump range is well-formed");
    assert(mut->limit <= mut->block_base + RT_GC_BLOCK_BYTES && "the bump range ends in it");
    assert(reinterpret_cast<uintptr_t>(mut->cursor) % RT_GC_ALIGN == 0 && "the cursor is aligned");
#endif
}

// What the current hole has handed out: the bytes the cursor advanced, and the objects
// whose start bits stand over that range. DERIVED rather than counted at allocation
// time because the inlined fast path bumps the cursor and sets the bit but cannot
// reach g.stats -- see GcState::hole_start. Side-effect free, so rt_gc_get_stats() can
// fold an in-flight hole into a mid-run read without committing it.
void hole_consumed(const rt_gc_mutator* mut, uint64_t* bytes, uint64_t* objects) {
    assert(bytes != nullptr && objects != nullptr && "the two counters have destinations");
    assert_hole_consistent(mut);
    *bytes   = 0;
    *objects = 0;
    if (mut->cursor == nullptr) {
        assert(g.hole_start == nullptr && "no hole, and yet a recorded hole start");
        return;
    }
    assert(g.hole_start != nullptr && "a live hole has a recorded start");
    assert(mut->cursor >= g.hole_start && "the bump cursor never walks backwards");
    *bytes = static_cast<uint64_t>(mut->cursor - g.hole_start);
    // Popcount the start bits over [hole_start, cursor). Exact, because every line of
    // a hole had its start byte zeroed by the reclamation that created the hole
    // (reclaim_blocks clears the bitmap of every line it frees) or by grow_heap's
    // memset, and hole_cursor stops a line being handed out twice -- so every set bit
    // in that range was set by an allocation from THIS hole. Bounded by kBitmapBytes
    // iterations, because both grain numbers are inside one block.
    const uint32_t lo = static_cast<uint32_t>((g.hole_start - mut->block_base) / RT_GC_ALIGN);
    const uint32_t hi = static_cast<uint32_t>((mut->cursor - mut->block_base) / RT_GC_ALIGN);
    assert(lo % 8 == 0 && "a hole starts on a line, and a line is eight grains");
    assert(hi <= kGrainsPerBlock && "the cursor is inside the cached block");
    for (uint32_t grain = lo; grain < hi; grain += 8) {
        unsigned bits = mut->start_bitmap[grain / 8];
        if (hi - grain < 8) {
            bits &= (1u << (hi - grain)) - 1u;  // the cursor's partial byte
        }
        *objects += static_cast<uint64_t>(std::popcount(bits));
    }
}

// Commit what the hole handed out, then forget it. Reached wherever a hole stops being
// the mutator's: at the next publication, at a collection, and at shutdown.
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

// Give up the hole entirely: its lines are about to be re-partitioned, or the collector
// is going away. Clears all five fields together, which is the only way the invariant
// above can hold of an empty hole.
void drop_hole(rt_gc_mutator* mut) {
    retire_hole(mut);
    mut->cursor       = nullptr;
    mut->limit        = nullptr;
    mut->block_index  = kNoBlock;
    mut->block_base   = nullptr;
    mut->start_bitmap = nullptr;
    mut->fin_bitmap   = nullptr;
    assert_hole_consistent(mut);
    assert(g.hole_start == nullptr && "a dropped hole leaves no start behind");
}

// Charge the nursery budget for bytes MADE AVAILABLE, once per hole handed out
// (Gc.h:247-253). Per hole rather than per object is what keeps the inlined fast path
// free of budget arithmetic -- it bumps and stores, and nothing else -- while leaving
// the accounting exact in the quantity a nursery was always about: a span of heap, not
// a count of objects.
void charge_budget(rt_gc_mutator* mut, uint64_t bytes) {
    assert(g.cfg.nursery_bytes > 0 && "rt_gc_init floors the nursery budget");
    assert(bytes > 0 && "a charge of nothing means a hole of nothing was published");
    // budget_left is unsigned, so "goes non-positive" is a compare rather than a
    // subtract that would wrap into an enormous remaining budget.
    if (mut->budget_left > bytes) {
        mut->budget_left -= bytes;
        return;
    }
    // The ONLY writer of the poll byte outside stress mode, and it is reached only from
    // publish_hole() and alloc_large(). That is what makes the inlined poll free on a
    // design that allocates no object: reset_mutator() zeroes `request` at rt_gc_init,
    // these are the sole paths that would raise it, so outside --gc-stress every safepoint for
    // the whole run is a load, a test and a not-taken branch (Gc.h:157-161). Raising the
    // byte is ALL that happens: allocation never collects, however far over budget it
    // goes, because it runs inside a live JIT frame and only a scheduler region boundary
    // has none (design §6.1).
    mut->request     = 1;
    mut->budget_left = g.cfg.nursery_bytes;
    assert(mut->budget_left > 0 && "a reset budget is never immediately spent");
}

// First run of free lines at or after `from` that is at least `size` bytes long.
// Pure: probing a block must not disturb its hole cursor, or a request too large
// for a hole would permanently lose that hole for the smaller requests that follow.
bool find_hole(const BlockMeta* meta, uint64_t size, uint32_t from, uint32_t* lo_out,
               uint32_t* hi_out) {
    assert(meta != nullptr);
    assert(size > 0);
    assert(from <= RT_GC_LINES_PER_BLOCK);
    const uint8_t epoch = static_cast<uint8_t>(g.mark_epoch);
    uint32_t      i     = from;
    // Both loops advance `i` / `end` strictly, so the whole search is bounded by
    // RT_GC_LINES_PER_BLOCK line tests.
    while (i < RT_GC_LINES_PER_BLOCK) {
        if (meta->line_mark[i] == epoch) {
            ++i;
            continue;
        }
        uint32_t end = i + 1;
        while (end < RT_GC_LINES_PER_BLOCK && meta->line_mark[end] != epoch) {
            ++end;
        }
        assert(end > i && "a hole is at least one line");
        if (static_cast<uint64_t>(end - i) * RT_GC_LINE_BYTES >= size) {
            *lo_out = i;
            *hi_out = end;
            return true;
        }
        i = end;  // too small for this request; skip the whole hole
    }
    return false;
}

// Hand a hole to the mutator: the five cached fields, the block's hole cursor, the
// budget charge and the conservative has_finalizable flag. The ONLY place any of the
// five is set to a non-null value, so it is the only place the invariant can be
// established -- hence one assertion here rather than one per caller.
void publish_hole(rt_gc_mutator* mut, uint32_t block_id, uint32_t lo, uint32_t hi) {
    assert(block_id < g.blocks.size() && "a published hole is in a real block");
    assert(lo < hi && "a published hole is at least one line");
    assert(hi <= RT_GC_LINES_PER_BLOCK && "a hole ends inside its block");
    // Whatever the outgoing hole handed out, count it before the cursor that measures
    // it is overwritten.
    retire_hole(mut);

    const Block& blk      = g.blocks[block_id];
    blk.meta->hole_cursor = static_cast<uint16_t>(hi);
    // A block the mutator can bump into may gain a finalizable object at any moment and
    // the inlined fast path cannot record that: it holds `fin_bitmap`, not the BlockMeta
    // this byte lives in. Publication is therefore the last point at which the runtime
    // can flag the block, and it flags it unconditionally. reclaim_blocks() makes the
    // flag exact again, and sweep_finalizable() asserts the negative space.
    blk.meta->has_finalizable = 1;

    mut->block_index  = block_id;
    mut->block_base   = blk.base;
    mut->start_bitmap = blk.meta->start_bits;
    mut->fin_bitmap   = blk.meta->fin_bits;
    mut->cursor       = blk.base + static_cast<uint64_t>(lo) * RT_GC_LINE_BYTES;
    mut->limit        = blk.base + static_cast<uint64_t>(hi) * RT_GC_LINE_BYTES;
    g.hole_start      = mut->cursor;

    // A hole starts on a line boundary and a line is a whole number of grains, so every
    // bump from here lands on an RT_GC_ALIGN boundary; assert_hole_consistent says so.
    assert_hole_consistent(mut);
    assert(mut->cursor < mut->limit && "a published hole is non-empty");
    charge_budget(mut, static_cast<uint64_t>(mut->limit - mut->cursor));
}

// Another hole in the block this mutator already owns.
bool refill_hole(rt_gc_mutator* mut, uint64_t size) {
    if (mut->block_index == kNoBlock) {
        return false;
    }
    assert(mut->block_index < g.blocks.size());
    const BlockMeta* meta = g.blocks[mut->block_index].meta;
    uint32_t         lo   = 0;
    uint32_t         hi   = 0;
    if (!find_hole(meta, size, meta->hole_cursor, &lo, &hi)) {
        return false;
    }
    publish_hole(mut, mut->block_index, lo, hi);
    return true;
}

// Take a block off `list` that has a big enough hole. Swap-erase, because neither
// list is ordered: a block's position in it carries no meaning.
bool take_from(std::vector<uint32_t>& list, rt_gc_mutator* mut, uint64_t size) {
    for (size_t n = list.size(); n > 0; --n) {
        const size_t     idx  = n - 1;
        const uint32_t   b    = list[idx];
        const BlockMeta* meta = g.blocks[b].meta;
        uint32_t         lo   = 0;
        uint32_t         hi   = 0;
        if (!find_hole(meta, size, meta->hole_cursor, &lo, &hi)) {
            continue;
        }
        list[idx] = list.back();
        list.pop_back();
        g.blocks[b].meta->flags = kBlockOwned;
        publish_hole(mut, b, lo, hi);
        return true;
    }
    return false;
}

bool acquire_block(rt_gc_mutator* mut, uint64_t size) {
    // Recycled before free, which is Immix's policy (PLDI 2008 §3.3): consume the
    // fragmented space between survivors before breaking open a clean block.
    if (take_from(g.recycled_blocks, mut, size)) {
        return true;
    }
    [[maybe_unused]] const bool had_free = !g.free_blocks.empty();
    if (take_from(g.free_blocks, mut, size)) {
        assert(g.stats.blocks_free > 0 && "a free block was accounted for");
        g.stats.blocks_free -= 1;
        return true;
    }
    // A free block has 256 unmarked lines and size <= kLargeObjectBytes < a whole
    // block, so a free block refusing the request means the line marks are corrupt.
    assert(!had_free && "a wholly free block failed a block-resident request");
    return false;
}

uint8_t* bump(rt_gc_mutator* mut, uint64_t size) {
    assert(mut->cursor != nullptr && "a bump needs a published hole");
    assert(size <= static_cast<uint64_t>(mut->limit - mut->cursor) && "the request fits");
    uint8_t* start = mut->cursor;
    mut->cursor    = start + size;
    // The one field of the five the fast path also writes, and the reason it may: a bump
    // within a published hole cannot leave the block the other four name, so no
    // fast-path allocation can straddle two blocks.
    assert(mut->cursor <= mut->limit && "a bump stayed inside the hole");
    return start;
}

// Three escalating steps, each strictly more expensive than the last. This is the body
// rt_gc_alloc_slow() exists for: the current hole could not satisfy the request.
uint8_t* refill_and_bump(rt_gc_mutator* mut, uint64_t size) {
    assert(size <= kLargeObjectBytes);
    assert(size % RT_GC_ALIGN == 0);
    if (refill_hole(mut, size)) {
        return bump(mut, size);
    }
    if (acquire_block(mut, size)) {
        return bump(mut, size);
    }
    // heap_bytes_max is reported, never enforced: `for (int i = 0; i < 1000000;
    // i++) obj = new C();` reaches no region boundary and is legal SV, and failing
    // it loudly was explicitly rejected (design §6.1, Gc.h:160-163).
    if (grow_heap() && acquire_block(mut, size)) {
        return bump(mut, size);
    }
    oom_fatal(size);
}

uint8_t* alloc_small(rt_gc_mutator* mut, uint64_t size) {
    assert(size <= kLargeObjectBytes);
    assert(mut->cursor <= mut->limit && "the bump range is well-formed");
    // The test generated code inlines (design §4.3). A null cursor fails it because
    // drop_hole() nulls `limit` alongside it, so the empty-hole case costs no second
    // branch here and none there.
    if (mut->cursor != nullptr && size <= static_cast<uint64_t>(mut->limit - mut->cursor)) {
        return bump(mut, size);
    }
    return refill_and_bump(mut, size);
}

// Set one block-resident object's metadata bits, indexing the mutator's cached block
// metadata exactly as the inlined fast path does (Gc.h:168-177) rather than looking the
// block up: the runtime's own entry point must exercise the same arithmetic generated
// code emits, or a mistake in it would only ever show up in a JIT'd run.
void set_object_bits(const rt_gc_mutator* mut, uint8_t* start, bool finalizable) {
    assert_hole_consistent(mut);
    assert(start >= mut->block_base && "the object is inside the cached block");
    const uint64_t off = static_cast<uint64_t>(start - mut->block_base);
    assert(off < RT_GC_BLOCK_BYTES && "the object starts inside the cached block");
    const uint32_t grain = static_cast<uint32_t>(off / RT_GC_ALIGN);
    const uint8_t  bit   = static_cast<uint8_t>(1u << (grain % 8));
    // Negative space: two objects at one address means the bump cursor or the
    // reclamation bitmap clear is wrong, and every later lookup would silently resolve
    // to whichever of the two the backward scan happened to reach. rt_gc_verify() runs
    // the same check over the bits GENERATED code sets, where no assertion can.
    assert((mut->start_bitmap[grain / 8] & bit) == 0 && "an object already starts here");
    mut->start_bitmap[grain / 8] |= bit;
    if (!finalizable) {
        assert((mut->fin_bitmap[grain / 8] & bit) == 0 && "a stale finalizable bit");
        return;
    }
    assert(g.blocks[mut->block_index].meta->has_finalizable != 0 &&
           "publish_hole flags every block a finalizable object can land in");
    mut->fin_bitmap[grain / 8] |= bit;
}

void large_insert_sorted(const LargeCell& cell) {
    const auto pos =
      std::lower_bound(g.large.begin(), g.large.end(), cell.start,
                       [](const LargeCell& e, const uint8_t* s) { return e.start < s; });
    if (pos != g.large.begin()) {
        assert((pos - 1)->start + (pos - 1)->bytes <= cell.start && "large cells overlap");
    }
    if (pos != g.large.end()) {
        assert(cell.start + cell.bytes <= pos->start && "large cells overlap");
    }
    g.large.insert(pos, cell);
}

// A cell of its own, RT_GC_ALIGN-aligned so the header and handle arithmetic is
// byte-identical to a block-resident object. No line marks, no start bit and no
// finalizable bit: these are never recycled into blocks, sweep_finalizable() finds them
// through g.large and the FINALIZE flag in their GC word, and reclaim_large() sweeps
// them per object -- all affordable precisely because they are rare.
//
// Counted and charged HERE, per object, because a large allocation publishes no hole:
// the per-hole accounting every block-resident object rides on would never see it, and
// a design allocating only large objects would then never exhaust its nursery budget.
uint8_t* alloc_large(rt_gc_mutator* mut, uint64_t size) {
    assert(size > kLargeObjectBytes);
    assert(size % RT_GC_ALIGN == 0);
    void* raw =
      ::operator new(static_cast<size_t>(size), std::align_val_t{ RT_GC_ALIGN }, std::nothrow);
    if (raw == nullptr) {
        oom_fatal(size);
    }
    assert(reinterpret_cast<uintptr_t>(raw) % RT_GC_ALIGN == 0 && "aligned new honoured");
    large_insert_sorted(LargeCell{ static_cast<uint8_t*>(raw), size });
    g.stats.bytes_allocated += size;
    g.stats.objects_allocated += 1;
    charge_budget(mut, size);
    return static_cast<uint8_t*>(raw);
}

// THE allocation body. rt_gc_alloc() and rt_gc_alloc_slow() are this function behind
// two different sets of entry assertions, so the initialisation sequence -- cell, GC
// word, start bit, finalizable bit, zeroed body -- exists once and cannot drift between
// the runtime's entry point and the one the inlined fast path falls through to.
void* alloc_object(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map) {
    assert(mut == &g_mutator && "one mutator today; the pointer is the MT hook");
    assert(g.initialized != 0 && "allocation before rt_gc_init");
    assert_map(map);
    assert(size == map->size && "the backend passes map->size (Gc.h:233-245)");

    const bool     large = size > kLargeObjectBytes;
    const bool     fin   = map->finalize != nullptr;
    uint8_t* const start = large ? alloc_large(mut, size) : alloc_small(mut, size);
    assert(start != nullptr && "the slow paths take a fatal rather than fail");
    assert(reinterpret_cast<uintptr_t>(start) % RT_GC_ALIGN == 0 && "an object start is aligned");

    // Mark state 0 is "never marked" (Gc.h:63-65), so the next collection traces this
    // object whichever epoch is current then. FINALIZE mirrors the finalizable bit set
    // below: the bit is how sweep_finalizable() FINDS a block-resident object, the flag
    // is how it finds a large one, and it asserts the two agree on everything it reaches.
    *reinterpret_cast<uint64_t*>(start) = fin ? RT_GC_FLAG_FINALIZE : 0ull;
    if (!large) {
        set_object_bits(mut, start, fin);
    }
    void* const handle = handle_of(start);
    // 2-state leaves and handle properties power on 0 / null (LRM Table 7-1). These
    // bytes are recycled heap, so this is required rather than merely tidy.
    std::memset(handle, 0, static_cast<size_t>(size - RT_GC_HEADER_BYTES));

    // Validate-on-allocate, the pair half of the assertion in mark_and_push(). It is the
    // bitmap resolution rather than the full rt_gc_find_object because generated code
    // stores the vptr only after this returns (LlvmBackend.cpp:11696), so the map half of
    // the pair is asserted at trace time instead.
    assert(object_start_of(reinterpret_cast<uintptr_t>(handle)) == start);
    assert(object_start_of(reinterpret_cast<uintptr_t>(start)) == start);
    return handle;
}

// -- Marking ------------------------------------------------------------------

void pin(void* handle) {
    uint64_t* w = gcword_ptr(handle);
    if ((*w & RT_GC_FLAG_PINNED) != 0) {
        return;  // already pinned this collection; the side list holds it once
    }
    *w |= RT_GC_FLAG_PINNED;
    g.pinned.push_back(handle);
    g.stats.conservative_pins += 1;
}

// Promote and enqueue `handle` unconditionally: the mark, the line marks, the live
// accounting and the OLD bit -- which is the whole of promotion, because these are
// sticky mark bits and a survivor is promoted IN PLACE with nothing copied (§7).
// Surviving ONE collection is what makes an object old, and stats.promoted counts the
// 0 -> 1 transition only, so an object that survives ten collections is counted once.
//
// Marking at PUSH time rather than at pop is what bounds the worklist: an object
// enters it at most once, so its length can never exceed objects_allocated.
//
// Split from mark_and_push() below because the remembered-set drain has to mark an
// OLD object that mark_and_push() refuses. The kind is therefore tested in exactly one
// place and this function, like the rest of the mark phase, is kind-agnostic.
void mark_and_push_forced(void* handle) {
    assert(handle != nullptr);
    // Pair half of the validate-on-allocate assertion in rt_gc_alloc: every handle
    // the marker follows must resolve through the heap's own metadata.
    assert(rt_gc_find_object(handle) == handle && "traced a handle the heap does not own");
    uint64_t* w = gcword_ptr(handle);
    // Dedup on "already stamped by THIS collection", which is is_marked_word() minus its
    // spared_by_age() term -- and dropping that term is the whole point of the forced
    // path. is_marked_word() answers "may this collection free the object", and for an OLD
    // object in a young collection that is a flat no; using it here made the forced path
    // refuse exactly the case it exists for, so a remembered old object was never traced
    // and the young reference it had just acquired was freed while reachable.
    //
    // is_stamped() alone is not the guard either: for an OLD object in a young collection
    // it compares OMARK against the LAST full collection's parity, which a live old object
    // matches, so it would refuse the same case. Excluding the spared case is what makes
    // the test mean "this collection has already been here".
    //
    // No dedup is lost by that exclusion: the only force-marks in a young collection come
    // from the remembered set, and rt_gc_remember() sets LOGGED before pushing, so an
    // object appears there at most once per cycle.
    if (is_stamped(*w) && !spared_by_age(*w)) {
        return;
    }
    assert((*w & RT_GC_FLAG_FORWARDED) == 0 && "the collector is non-moving in this phase");
    // Promotion first, then ONE stamp for whatever the object now is. stamp_marked()
    // writes both parities unconditionally, so an object this collection has just
    // promoted carries the OMARK a full collection reads. Without that, the next full
    // collection would read a live object as unmarked, free its lines and clear the start
    // bit that makes it resolvable -- precisely the failure Gc.h:101-109 records.
    if ((*w & RT_GC_FLAG_OLD) == 0) {
        *w |= RT_GC_FLAG_OLD;
        g.stats.promoted += 1;
    }
    *w = stamp_marked(*w);
    assert(is_stamped(*w) && "the object this collection just marked reads unmarked");
    assert((*w & RT_GC_FLAG_OLD) != 0 && "every object a collection marks is promoted");

    const rt_gc_map* map = map_of(handle);
    g.stats.objects_live += 1;
    g.stats.bytes_live += map->size;
    mark_lines(handle, map->size);
    g.worklist.push_back(handle);
    assert(g.worklist.size() <= g.stats.objects_allocated && "the mark stack outgrew the heap");
}

// Mark `handle` and queue it; do nothing if it is already marked this cycle, or if this
// is a YOUNG collection and the object is already OLD.
//
// That second case is the whole of generational marking, and it is sound because of two
// facts about how an object becomes OLD. An object is promoted exactly when a
// collection MARKS it, and marking pushes it, so the collection that promoted it also
// TRACED it: every reference the object held at that instant named an object the same
// collection marked and therefore promoted, so every un-mutated out-edge of an OLD
// object names another OLD object, whose lines line_old protects. The mutator is
// stopped for the whole collection (design §6.1), so no store can slip in between
// "traced" and "promoted". And every reference written into the object AFTER that
// point takes the write barrier's taken edge -- `(gcword & (OLD|LOGGED)) == OLD` --
// so the object is in the remembered set and the next collection traces it from there,
// which covers stores from generated code (Gc.h:98-108) and, through
// relog_conservative_roots(), the raw interior stores a suspended activation performs
// after the barrier that armed it has already been consumed.
//
// A third store shape stores no handle into a heap object at all: a class handle written
// into a CONTAINER owned by an old object lands in the container's malloc'd payload. Since
// P4 that shape is real -- a class handle CAN be a container element, or a handle inside
// an associative KEY, bare or one leaf of an aggregate key (LlvmBackend.cpp:2612-2615, the
// elem_ops_for door at :11693-11703) -- so it is covered in two halves rather than assumed
// away. The backend logs the OWNING object at every container write helper: push_back /
// push_front / insert, whole-container assign, `new[n](src)`, slice append, and the put_ptr
// that writes a key whose table is non-empty (assoc_key_is_class_handle, :1853-1857). And
// the runtime traces a remembered object through the ORDINARY path -- push_remembered_roots
// marks it onto the same worklist every other object uses, so trace_one walks its handle
// offsets AND pushes its container leaves for the element walkers (§3.3). That is not
// incidental: a bespoke field walk over map->handles would trace `old.f = fresh` and lose
// `old.q.push_back(fresh)`, freeing an object a live queue still points at.
//
// The suspended-activation window is the same for a container held by `ref`, with ONE
// exception. A `ref` to a container that is a direct class property is a pointer to the
// rt_queue / rt_dynarr / rt_assoc header inside the owning object's BODY, so the frame scan
// resolves it to that object (rt_gc_find_object resolves interior pointers) and
// relog_conservative_roots() carries the obligation forward exactly as for a `ref` to a
// handle property. A `ref` to a NESTED container element -- `obj.qq[3]` where qq is a
// queue of queues -- is a pointer into the outer container's malloc'd payload
// (LlvmBackend.cpp:16421), which rt_gc_find_object correctly rejects as not-in-a-block, so
// nothing re-logs the owner and a callee that suspends and only then pushes is outside both
// mechanisms. The collector cannot close that from here: a payload-interior pointer is
// indistinguishable from an integer, and re-logging the owner needs the call site's
// knowledge of who owns the element. It is closed at the call site instead, by registering
// the actual as a dynamically-scoped container root for the call's duration
// (rt_gc_container_add / rt_gc_container_remove, Gc.h:347-350) -- which needs nothing from
// this file, because a container ROOT is scanned in full at every collection whatever its
// kind and so needs no barrier by construction (Gc.h:105-107).
//
// Should that registration ever be missing, the shape is LOUD rather than silent:
// verify_old_edges() walks an un-remembered old object's container leaves and asserts every
// element is one this collection would spare (is_marked_now), so an un-logged young element
// fails an assertion at the collection that would have freed it instead of at the
// use-after-free later.
//
// By induction over collections, an OLD object outside the remembered set has no
// out-edge this collection could reach that is not already covered. Skipping it loses
// nothing, and skipping it is what makes a young collection cost the young survivors
// instead of the whole component tree (§7) -- the entire point of this phase.
void mark_and_push(void* handle) {
    assert(handle != nullptr);
    assert(g.kind == RT_GC_KIND_YOUNG || g.kind == RT_GC_KIND_FULL);
    if (spared_by_age(*gcword_ptr(handle))) {
        return;  // live-and-opaque: its lines are held by line_old, not by a mark
    }
    mark_and_push_forced(handle);
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
// trace map. Compiles to nothing under NDEBUG, so the walkers call it freely.
void assert_elem_ops([[maybe_unused]] const rt_elem_ops* ops) {
    assert(ops != nullptr && "every traced element has a descriptor");
    assert(ops->stride > 0 && "an element slot is at least one byte");
    assert((ops->nhandles == 0 || ops->handles != nullptr) && "nhandles implies a table");
    assert((ops->nleaves == 0 || ops->leaves != nullptr) && "nleaves implies a leaf table");
    assert((!is_container_kind(ops->kind) || ops->inner != nullptr) &&
           "a container descriptor names its element descriptor");
    // The key table and its count are one fact stated twice (Runtime.h:919-927), and
    // `nkeyhandles != 0` is the SINGLE source of truth for "this array's keys carry a
    // handle" -- the same question the backend's barrier answers off the same table
    // (LlvmBackend.cpp:1849-1857), so the barrier and the tracer cannot disagree.
    assert((ops->nkeyhandles == 0) == (ops->keyhandles == nullptr) &&
           "nkeyhandles implies a key table, and a key table implies a count");
    assert((ops->nkeyhandles == 0 || ops->kind == rt_elem_assoc) &&
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
// Resolution is asserted here, without trace_one's `conservative ||` escape, and the
// reason is sweep_finalizable(): every container this can reach is state-resident,
// registered live, or inside an object whose body bytes are still its own, so a
// non-resolving element means the descriptor is wrong rather than that the bytes are
// stale. It stays a skip in release because a misclassified element -- a chandle
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
// aggregate, because collect_elem_leaves (LlvmBackend.cpp:1542-1567) flattens the
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

// One queue, in RING order: element i sits at base + ((head + i) % cap) * stride
// (Runtime.h:1048-1056). A walk that ignored the wraparound would silently miss
// exactly the elements a push_front wrapped past the end of the buffer, and a
// reachable handle the collector misses is a use-after-free, not a lost element.
void walk_queue(const rt_queue* h, const rt_elem_ops* cops) {
    assert(h != nullptr && "a queue walk needs its handle");
    assert(cops != nullptr && "a queue walk needs a container descriptor");
    assert(cops->kind == rt_elem_queue && "a queue walk needs a queue descriptor");
    assert(h->len <= h->cap && "a queue holds no more than its ring");
    assert((h->cap == 0 || h->base != nullptr) && "a non-empty ring owns its buffer");
    assert((h->cap == 0 || h->head < h->cap) && "the head is a ring slot index");
    assert(h->cap <= kContainerElemsMax && "an implausible queue capacity");
    if (h->len == 0 || h->cap == 0) {
        return;  // empty, and also what keeps the modulo below off a zero capacity
    }
    const rt_elem_ops* eops = cops->inner;
    assert_elem_ops(eops);
    uint8_t* base = static_cast<uint8_t*>(h->base);
    for (uint64_t i = 0; i < h->len; ++i) {
        const uint64_t slot = (h->head + i) % h->cap;
        visit_elem_image(base + slot * eops->stride, eops);
    }
}

// The class handle encoded in the 8 key bytes at `key + off`.
//
// The ONE place the key byte order is undone. emit_key_bytes writes byte 0 as the most
// significant and applies no sign flip, a class handle being unsigned
// (LlvmBackend.cpp:1805-1810, docs/design/assoc-keys.md:85), so the word is reassembled
// MSB-first -- a memcpy would read it byte-reversed on this host and trace an address
// that is not an object.
//
// Byte-wise rather than one 64-bit load, and that is not a style choice: a key offset is
// a SERIALIZED offset, not a storage one. An aggregate key is concatenated leaf by leaf
// with no padding (assoc-keys.md:88), so `struct { int i; Cell c; }` puts its handle at
// offset 4 and nothing about a key handle is 8-aligned. Hence no alignment assertion
// here, unlike visit_elem_image's storage words.
void* decode_key_handle(const uint8_t* key, [[maybe_unused]] uint32_t klen, uint32_t off) {
    assert(key != nullptr && "a decoded key has bytes");
    assert(off + sizeof(uint64_t) <= klen && "a key handle lies inside the normalized span");
    uint64_t bits = 0;
    for (uint32_t i = 0; i < sizeof(uint64_t); ++i) {
        bits = (bits << 8) | key[off + i];
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(bits));
}

// The context the associative walk carries through the C callback: the descriptor of
// the ARRAY, so the callback can read both `inner` (the value element) and
// `keyhandles` (where the class handles are inside one normalized key span).
struct AssocWalk {
    const rt_elem_ops* cops;
};

// One live element image of an associative array, plus its normalized key bytes. A
// null `key` is the pattern default (LRM 7.9.11), a live element image with no key:
// reading any absent index yields it, so a handle inside it is a strong reference
// (design §9, "a handle in a live element of a traced container").
//
// A handle-valued KEY is a strong reference for the same reason and one the collector
// has to decode: the key bytes are the only copy of the handle, so `m[obj] = 1` keeps
// `obj` alive and `m.first(k)` hands it back for dereference
// (docs/design/garbage-collection.md:365). Which bytes those are comes from an offset
// TABLE rather than a flag, because an aggregate key is serialized leaf by leaf and a
// struct can carry several handles at offsets no flag could name (Runtime.h:919-927). A
// bare `int m[C]` is the ONE-ENTRY case of this same loop, `{ 0 }`, and not a path of
// its own (LlvmBackend.cpp:1817-1821).
void assoc_visit_image(void* elem, const uint8_t* key, uint32_t klen, void* ctx) {
    assert(elem != nullptr && "a visited element image has an address");
    assert(ctx != nullptr && "the associative walk carries its descriptor");
    const rt_elem_ops* cops = static_cast<const AssocWalk*>(ctx)->cops;
    visit_elem_image(static_cast<uint8_t*>(elem), cops->inner);
    if (cops->nkeyhandles == 0 || key == nullptr) {
        return;
    }
    assert(cops->kind == rt_elem_assoc && "only an associative array is keyed");
    assert(klen > 0 && "a present key has bytes");
    [[maybe_unused]] uint32_t prev = 0;
    for (uint32_t i = 0; i < cops->nkeyhandles; ++i) {
        const uint32_t off = cops->keyhandles[i];
        // Strictly ascending and disjoint -- the runtime half of the emitter's
        // `is_sorted` (LlvmBackend.cpp:1838): the offsets come from a walk of the span in
        // serialization order in which every class leaf consumes exactly 8 bytes
        // (:1805-1810), so no two can overlap. Alignment is deliberately NOT asserted
        // (decode_key_handle). This is also what bounds the loop: with the in-span check
        // in the decode, nkeyhandles <= klen / 8 follows.
        assert((i == 0 || off >= prev + sizeof(uint64_t)) &&
               "the key handle offsets are ascending and disjoint");
        prev = off;
        // Resolution of the decoded handle is asserted by mark_container_handle, which is
        // the pair half of this decode: a key handle read out of the wrong bytes names no
        // object, and one the collector fails to read at all is a use-after-free that
        // surfaces on the next `m.first(k)`.
        mark_container_handle(decode_key_handle(key, klen, off));
    }
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
        // mark an OLD owner (mark_and_push), and that is not a gap: line_old keeps its lines
        // out of every hole and the finalizer sweep spares it, so it is intact either way.
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
    // Whether this object was reached conservatively decides how much may be
    // demanded of its fields; see the long comment below.
    [[maybe_unused]] const bool conservative = (*gcword_ptr(handle) & RT_GC_FLAG_PINNED) != 0;
    for (uint32_t i = 0; i < map->nhandles; ++i) {
        const uint32_t off = map->handles[i];
        // Offsets are from the handle and name a whole aligned 64-bit word inside
        // the body (Gc.h:75-78); handles are bare, aligned, untagged words (F4).
        assert(off % sizeof(void*) == 0 && "a handle field is naturally aligned");
        assert(off + sizeof(void*) <= map->size - RT_GC_HEADER_BYTES && "field inside the body");
        void* child = *reinterpret_cast<void**>(body + off);
        if (child == nullptr) {
            continue;  // a null handle is the power-on default (LRM Table 7-1)
        }
        // This is a real check, not an assertion, and the reason is the
        // line-granular reclamation described at the top of this file. A dead
        // object whose start bit sits in a line some live object also occupies
        // keeps that bit, while its later lines can be reclaimed and handed out
        // again -- so its field bytes may by now be another object's `int`. Such an
        // object is only ever reached from a stale conservative word (§5.2's false
        // retention), which is why the assertion below is exactly as strong as it
        // can be: anything reached precisely is genuinely live, and a precise
        // object's fields must resolve.
        void* resolved = rt_gc_find_object(child);
        assert((conservative || resolved == child) && "a precise handle field must resolve");
        if (resolved == nullptr) {
            continue;
        }
        mark_and_push(resolved);
    }
    // The leaf table: this class's managed properties, by byte offset from the handle
    // (Gc.h:75-85). Only a container leaf can hold a class handle, and it is NOT a
    // root -- it is reached from here, which is what makes a container inside a dead
    // object keep nothing alive (Gc.h:236-244).
    //
    // Safe even for a conservatively-reached object: the one shape that could hand us
    // an rt_dynarr built from foreign bytes is a finalized object whose start bit
    // outlived its body, and sweep_finalizable() removes exactly that shape.
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
        assert(leaf.offset + leaf.ops->stride <= map->size - RT_GC_HEADER_BYTES &&
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
    uint64_t* w = gcword_ptr(handle);
    assert((*w & RT_GC_FLAG_OLD) != 0 && "only an OLD object belongs in the remembered set");
    if ((*w & RT_GC_FLAG_LOGGED) != 0) {
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
    *w |= RT_GC_FLAG_LOGGED;
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
        uint64_t* w = gcword_ptr(h);
        assert((*w & RT_GC_FLAG_LOGGED) != 0 && "a remembered entry lost its LOGGED bit");
        assert((*w & RT_GC_FLAG_OLD) != 0 && "a remembered entry is an OLD object");
        *w &= ~RT_GC_FLAG_LOGGED;
        if (g.kind == RT_GC_KIND_FULL) {
            // Deliberately NOT marked. A full collection reaches every live object from
            // the real roots, so marking from here instead would let a stale entry keep
            // an unreachable old object -- and reclaiming exactly those is what a full
            // collection is for.
            continue;
        }
        // Forced, because mark_and_push() refuses an OLD object in a young collection and
        // this is the one place that refusal must not apply. Marked rather than merely
        // traced: the mark deduplicates against the worklist, counts the object in the
        // live accounting rt_gc_verify()'s heap walk cross-checks, and stamps its lines.
        mark_and_push_forced(h);
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
// last place the owner handle is in hand (LlvmBackend.cpp:21145-21149, :22240-22244,
// :5706-5708, :21170-21174). A non-blocking call cannot span a safepoint, so for a
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
    // run_finalizers() asserts the neighbouring halves of that same claim.
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
        uint64_t* w = gcword_ptr(h);
        // §13's pin audit: every collection-scoped pin must still be held here, and
        // after this loop no object anywhere carries one -- which is the negative
        // space rt_gc_verify() then asserts across the whole heap.
        assert((*w & RT_GC_FLAG_PINNED) != 0 && "a collection-scoped pin was cleared early");
        // Marked, or OLD and therefore live without being marked: a young collection does
        // not mark an un-remembered OLD object, so a conservative hit on one pins it and
        // stops there (mark_and_push). One predicate covers both, which is the §13 claim
        // it always was -- a pin names a live object.
        assert(is_marked_now(h) && "a pinned object is a live object");
        *w &= ~RT_GC_FLAG_PINNED;
    }
    g.pinned.clear();
}

// -- Finalization and reclamation --------------------------------------------

// One block's finalizable objects. `spare_live` says whether this sweep spares what the
// collection calls live: a collection sweep passes true and asks is_marked_now(), which is
// also what spares an OLD object in a young collection (HAZARD 2), while the teardown
// sweep passes false and spares nothing.
//
// Clearing the start bit is what closes the one hole P4 would otherwise open.
// reclaim_blocks() clears the start bits of the lines it FREES, but a dead object whose
// FIRST line is shared with a survivor keeps its bit while its later lines are handed
// out again -- so a stale conservative word still resolves to it while its body bytes
// have become another object's. A handle field survives that, because trace_one
// revalidates every field through rt_gc_find_object; a container leaf does not, because
// an rt_dynarr / rt_assoc built from arbitrary bytes names a malloc'd payload no
// metadata can reject.
uint64_t sweep_block_finalizable(const Block& blk, bool spare_live) {
    BlockMeta* m   = blk.meta;
    uint64_t   ran = 0;
    for (uint32_t i = 0; i < kBitmapBytes; ++i) {
        unsigned bits = m->fin_bits[i];
        while (bits != 0) {
            const uint32_t bit  = static_cast<uint32_t>(std::countr_zero(bits));
            const uint8_t  mask = static_cast<uint8_t>(1u << bit);
            bits &= bits - 1;  // strictly fewer bits each turn: at most eight per byte
            // The pair of set_object_bits' two stores: a finalizable bit stands over an
            // object start, and that object's class emitted a finalizer.
            assert((m->start_bits[i] & mask) != 0 && "a finalizable bit with no object");
            void* const h = handle_of(blk.base + (static_cast<uint64_t>(i) * 8 + bit) *
                                                   RT_GC_ALIGN);
            uint64_t* const w = gcword_ptr(h);
            assert((*w & RT_GC_FLAG_FINALIZE) != 0 && "a finalizable bit without the flag");
            // HAZARD 2 (Gc.h:91-92), and now a property of the one mark test rather than a
            // second flag beside it. is_marked_now() spares an OLD object in a YOUNG
            // collection because such a collection never visited it: unmarked does not mean
            // dead there. Without that, the first young collection after a promotion runs
            // the finalizer of every old finalizable object in the heap -- freeing the
            // string, dynarr, queue and assoc buffers of LIVE objects and clearing the
            // start bit that makes them resolvable -- so it is a use-after-free in the
            // mutator, not merely a lost object. Only a FULL collection may conclude that
            // an unmarked OLD object is dead, and for a full collection that verdict is
            // exact: OMARK against full_epoch, not a young parity it never stamped on it.
            //
            // Sparing it keeps its start bit alive for longer than the pre-generational
            // collector ever did -- across every young collection until a full one -- and
            // that is safe because line_old keeps every line it occupies out of every hole,
            // so its LEAF bytes stay its own. That is exactly the guarantee assert_map's
            // "managed leaves imply a finalizer" pairing exists to provide, preserved by a
            // different mechanism rather than lost.
            if (spare_live && is_marked_word(*w)) {
                continue;  // reachable, or old and this collection had no opinion
            }
            const rt_gc_map* map = map_of(h);
            assert(map->finalize != nullptr && "the finalizable bit was set from a finalize");
            // Clear the flag before the call so a double finalization is assertable
            // rather than a double free of the managed buffers. Clearing the bit below is
            // what actually prevents it; this makes the bug loud if it ever stops working.
            *w &= ~RT_GC_FLAG_FINALIZE;
            map->finalize(h);
            // Unreachable, and its body bytes are about to become reusable, so it must
            // stop resolving from here on -- and it must stop being finalizable, which is
            // what keeps this sweep and the teardown sweep from both claiming it. No
            // lookup for either: the byte index and the mask are already in hand.
            m->fin_bits[i] = static_cast<uint8_t>(m->fin_bits[i] & ~static_cast<unsigned>(mask));
            m->start_bits[i] =
              static_cast<uint8_t>(m->start_bits[i] & ~static_cast<unsigned>(mask));
            ++ran;
        }
    }
    return ran;
}

// The large-object half. They need no list of their own: g.large is already a per-object
// list that reclaim_large() walks every collection, and the FINALIZE flag in the GC word
// already says which of them owe a finalizer. Large cells carry no start or finalizable
// bit (alloc_large), so nothing is cleared but the flag.
uint64_t sweep_large_finalizable(bool spare_live) {
    uint64_t ran = 0;
    for (const LargeCell& cell : g.large) {
        void* const     h = handle_of(cell.start);
        uint64_t* const w = gcword_ptr(h);
        if ((*w & RT_GC_FLAG_FINALIZE) == 0) {
            continue;  // no finalizer, or already run
        }
        if (spare_live && is_marked_word(*w)) {
            continue;  // HAZARD 2 again, for the large-object half; see the block sweep
        }
        const rt_gc_map* map = map_of(h);
        assert(map->finalize != nullptr && "FINALIZE was set from a non-null finalize");
        *w &= ~RT_GC_FLAG_FINALIZE;
        map->finalize(h);
        ++ran;
    }
    assert(ran <= g.large.size() && "one finalizer per cell at most");
    return ran;
}

// Every finalizable object the sweep claims, block bitmaps then large cells. This
// replaced a std::vector<void*> of handles: the vector could not survive P2 at all,
// because an inlined allocation fast path cannot push onto it and gating inlining on
// "no finalizer" would have gated out nearly every class (BlockMeta::fin_bits).
//
// The cost moved from O(finalizable objects), each a pointer chase into a cold GC word,
// to O(bytes of bitmap in flagged blocks) -- 256 bytes, four cache lines, per 32 KiB
// block that can hold one -- plus the same GC-word read for each object actually found.
uint64_t sweep_finalizable(bool spare_live) {
    assert(g.worklist.empty() && "a finalizer sweep runs outside the mark loop");
    // HAZARD 2's pair assertion is deliberately gone from here: there is no second flag
    // left that could disagree with the collection kind, because is_marked_now() reads the
    // kind itself. What remains to state is the predicate's precondition -- it means
    // nothing outside a collection of a known kind.
    assert(g.kind == RT_GC_KIND_YOUNG || g.kind == RT_GC_KIND_FULL);
    uint64_t ran = 0;
    for (uint32_t b = 0; b < g.blocks.size(); ++b) {
        const Block& blk = g.blocks[b];
        if (blk.meta->has_finalizable == 0) {
            // The negative space of publish_hole's conservative flag, and the only thing
            // standing between a bit GENERATED code set in a block the runtime never
            // flagged and a finalizer that silently never runs.
#ifndef NDEBUG
            for (uint32_t i = 0; i < kBitmapBytes; ++i) {
                assert(blk.meta->fin_bits[i] == 0 && "a finalizable object in a skipped block");
            }
#endif
            continue;
        }
        ran += sweep_block_finalizable(blk, spare_live);
    }
    return ran + sweep_large_finalizable(spare_live);
}

// Finalizers run BEFORE any line is reclaimed: an unmarked finalizable object's
// finalizer frees that object's managed properties -- its string, dynamic array,
// queue and associative-array buffers -- and must run while those bytes are still
// intact (design §1, "finalize; do not re-own").
void run_finalizers() {
    [[maybe_unused]] const uint64_t allocated_before = g.stats.objects_allocated;
    assert(g_mutator.cursor == nullptr && "rt_gc_collect retires the hole before marking");
    g.stats.finalized += sweep_finalizable(true);
    // A finalizer must not allocate and must not store a handle anywhere. The first is
    // directly checkable, in two halves that between them cover both allocation paths: a
    // block-resident allocation would have had to publish a hole, because rt_gc_collect
    // dropped the mutator's, and a large one is counted where it happens. The second
    // would show up as a resurrected object surviving the reclamation that follows, which
    // rt_gc_verify() catches.
    assert(g_mutator.cursor == nullptr && "a finalizer allocated");
    assert(g.stats.objects_allocated == allocated_before && "a finalizer allocated a large one");
    assert(g.worklist.empty() && "a finalizer marked something");
}

// Every remaining finalizer at teardown, live or not. Not needed for correctness
// -- the process is exiting -- but it is what makes LeakSanitizer meaningful for
// class objects, which is this work's acceptance evidence (Gc.h:216-218, §13).
//
// Deliberately NOT counted into stats.finalized: that counter means "finalized by a
// collection", so a garbage cycle dropped mid-run bumps it and a cycle that
// survives to exit does not. The teardown count is returned for the stats line
// instead, which keeps the two distinguishable without widening rt_gc_stats.
uint64_t finalize_all_remaining() {
    // Sparing nothing: every finalizable object still carrying its bit or its flag is
    // claimed here, live or not, because the process is exiting.
    const uint64_t ran = sweep_finalizable(false);
    assert(sweep_finalizable(false) == 0 && "teardown owes no further finalizer");
    return ran;
}

// What one block's line walk reports back: the lines it KEPT, whether any finalizable bit
// survived it, and the two protection counts HAZARD 1's heap-wide claim is made from.
struct LineSweep {
    uint32_t kept       = 0;
    unsigned any_fin    = 0;
    uint64_t old_before = 0;
    uint64_t old_after  = 0;
};

// One block's 256 line-mark bytes, and the whole of the generational reclamation
// predicate. Split out of reclaim_blocks() only for length (TigerStyle).
void reclaim_block_lines(BlockMeta* m, bool full, LineSweep* out) {
    assert(m != nullptr && "a block being reclaimed has metadata");
    assert(out != nullptr && "the line walk reports through its out-parameter");
    const uint8_t epoch = static_cast<uint8_t>(g.mark_epoch);
    for (uint32_t i = 0; i < RT_GC_LINES_PER_BLOCK; ++i) {
        // HAZARD 1 (Gc.h:89-91), and THE PREDICATE this phase turns on: a line is free iff
        // this collection did not mark it AND -- in a young collection -- it does not
        // already hold an old survivor.
        //
        // line_old is re-DERIVED here rather than maintained by the marker, because every
        // object a collection marks is promoted (mark_and_push_forced), so "a line holding
        // an old survivor" is exactly "a line this collection marked", unioned for a young
        // collection with what was recorded before. A full collection ASSIGNS instead of
        // unioning, and that assignment is the only thing that ever lets protection lapse
        // -- which is how old garbage comes back at all, and why it accumulates until a
        // full collection runs (§7).
        //
        // The full ASSIGNMENT is sound only because is_marked_now() is exact for an OLD
        // object too, and this is the line where the mark-staleness bug did its damage: a
        // live old object read as already-marked never reached mark_lines(), so its line
        // missed the epoch here, and the assignment then unprotected it, freed it and
        // cleared its start bit. OMARK is what makes "did this collection mark it"
        // answerable for an object young collections skip; verify_block() checks the
        // per-object half of the same claim.
        const bool marked_now = (m->line_mark[i] == epoch);
        const bool was_old    = (m->line_old[i] != 0);
        const bool old_here   = full ? marked_now : (marked_now || was_old);
        out->old_before += was_old ? 1 : 0;
        out->old_after += old_here ? 1 : 0;
        m->line_old[i] = static_cast<uint8_t>(old_here ? 1 : 0);
        if (old_here) {
            out->kept += 1;
            // Re-publish the mark for a line kept only by line_old, and this is not
            // bookkeeping either: find_hole() looks for runs of lines whose line_mark
            // misses the epoch, so a protected line still reading the PREVIOUS epoch would
            // be handed straight back to the allocator and bump-allocated over -- the same
            // use-after-free by another route. Re-publishing keeps every consumer of
            // line_mark right with no knowledge of ages at all: find_hole, lines_marked,
            // the block state, and the {epoch, 0} parity invariant at the top of this file.
            m->line_mark[i] = epoch;
        } else {
            // Zero the mark AND the line's one byte of each bitmap. The mark, because epoch
            // parity needs every line to read {epoch, 0} when the next collection toggles.
            // The start bitmap, because a stale start bit in a reusable hole would let
            // block_start_of() resolve a candidate to an object whose bytes have since been
            // handed out again. The finalizable bitmap, because a stale finalizable bit in a
            // reusable hole would run a finalizer over another object's body -- freeing a
            // `string` buffer that is now live.
            m->line_mark[i]  = 0;
            m->start_bits[i] = 0;
            m->fin_bits[i]   = 0;
        }
        // After the clear, so this sees exactly the bits that survive: the flag
        // publish_hole() had to set conservatively becomes EXACT here, for free, because
        // this walk already has the byte in a register. That is what lets the next sweep
        // skip every block holding no finalizable survivor -- including every wholly free
        // block, which is most of a freshly grown 4 MiB chunk.
        out->any_fin |= m->fin_bits[i];
        // HAZARD 1's negative space: protection and the line mark now agree exactly, in
        // both directions. That is what lets everything downstream -- allocation, and
        // rt_gc_verify()'s per-object check -- state the invariant in terms of whichever of
        // the two it already has in hand.
        assert((m->line_old[i] != 0) == (m->line_mark[i] == epoch) &&
               "a protected line disagrees with its mark");
    }
}

// Line-granular reclamation. No object is examined: this is 256 byte comparisons
// per 32 KiB block, and it is the whole reason collection cost does not scale with
// the number of dead objects. Both free lists are rebuilt from scratch, which is
// also what keeps them duplicate-free without a per-block "in a list" flag -- and
// is why the sweep is eager rather than the lazy per-acquisition sweep design §4.2
// sketches: the full walk is needed for the lists and for blocks_free anyway, and
// it touches one metadata byte per 128 heap bytes.
void reclaim_blocks() {
    g.free_blocks.clear();
    g.recycled_blocks.clear();
    const bool full  = (g.kind == RT_GC_KIND_FULL);
    uint64_t   freed = 0;
    // HAZARD 1 as a heap-wide claim, and the only formulation of it that is both true and
    // load-bearing: a YOUNG collection may only ADD protection, never remove it. Per
    // object it cannot be said -- a FULL collection legitimately frees the later lines of
    // an old object that has died while its start bit survives in a line some survivor
    // shares, the §5.2 shape verify_bitmaps describes -- but per heap it is exact, and it
    // fails the instant reclamation frees a line that held an old survivor.
    //
    // In terms of the predicate: a young collection has NO opinion about an OLD object --
    // is_marked_now() spares it by age -- so it may not act on one, and unioning is
    // exactly "does not act". Only a full collection, whose verdict on an old object is
    // OMARK against full_epoch and therefore exact, may let protection lapse. The
    // assertion below is one-sided for that reason rather than by omission.
    [[maybe_unused]] uint64_t old_before = 0;
    [[maybe_unused]] uint64_t old_after  = 0;
    for (uint32_t b = 0; b < g.blocks.size(); ++b) {
        BlockMeta* m = g.blocks[b].meta;
        LineSweep  sweep;
        reclaim_block_lines(m, full, &sweep);
        old_before += sweep.old_before;
        old_after += sweep.old_after;
        m->has_finalizable = static_cast<uint8_t>(sweep.any_fin != 0 ? 1 : 0);
        m->lines_marked    = static_cast<uint16_t>(sweep.kept);
        m->hole_cursor     = 0;
        assert((m->has_finalizable != 0 || sweep.any_fin == 0) &&
               "a survivor's finalizer was dropped");
        if (sweep.kept == 0) {
            m->flags = kBlockFree;
            g.free_blocks.push_back(b);
            ++freed;
        } else if (sweep.kept < RT_GC_LINES_PER_BLOCK) {
            m->flags = kBlockRecycled;
            g.recycled_blocks.push_back(b);
        } else {
            m->flags       = kBlockFull;
            m->hole_cursor = RT_GC_LINES_PER_BLOCK;
        }
    }
    g.stats.blocks_free = freed;
    assert(g.stats.blocks_free <= g.stats.blocks_total && "more free blocks than blocks");
    assert(g.free_blocks.size() == freed && "the free list agrees with the count");
    assert((full || old_after >= old_before) &&
           "a young collection freed a line that held an old survivor");
}

// Large cells are the one place a per-object sweep is unavoidable, and the one
// place it is cheap: there is no line to share, so an unmarked cell is dead.
void reclaim_large() {
    size_t keep = 0;
    for (size_t i = 0; i < g.large.size(); ++i) {
        const LargeCell cell = g.large[i];
        void*           h    = handle_of(cell.start);
        // HAZARD 3 (Gc.h:92-93). "Unmarked is dead" is exactly what is_marked_now()
        // decides, and the reason this is one call rather than a mark test plus an age
        // escape beside it: in a YOUNG collection an un-remembered OLD cell is one the mark
        // phase declined to visit and the predicate spares it by age, while in a FULL one
        // it is spared only if that collection actually marked it -- which is how a dead
        // large old object is ever freed. Freeing a live one returns a live object's
        // storage to the allocator immediately: with one large object per transaction,
        // the collector would corrupt the first design that promoted one, on the very next
        // collection.
        if (is_marked_now(h)) {
            g.large[keep++] = cell;  // compaction preserves the base ordering
            continue;
        }
        // run_finalizers() already ran this object's finalizer, while its managed
        // buffers were still intact, and cleared the flag on the way.
        assert((*gcword_ptr(h) & RT_GC_FLAG_FINALIZE) == 0 && "a dead cell owes a finalizer");
        assert(!spared_by_age(*gcword_ptr(h)) && "a young collection reclaimed an OLD cell");
        assert((*gcword_ptr(h) & RT_GC_FLAG_PINNED) == 0 && "a pinned object would be marked");
        ::operator delete(cell.start, std::align_val_t{ RT_GC_ALIGN });
    }
    g.large.resize(keep);
}

// -- Teardown -----------------------------------------------------------------

void release_heap() {
    // Before a single block goes away: the hole names one of them, and the retirement
    // inside drop_hole() reads that block's start bitmap to count what it handed out.
    drop_hole(&g_mutator);
    for (const LargeCell& cell : g.large) {
        ::operator delete(cell.start, std::align_val_t{ RT_GC_ALIGN });
    }
    g.large.clear();
    for (const Chunk& c : g.chunks) {
        assert(c.base != nullptr && "a registered chunk has a payload");
        assert(c.meta != nullptr && "a registered chunk has a metadata array");
        delete[] c.meta;
        ::operator delete(c.base, std::align_val_t{ RT_GC_BLOCK_BYTES });
    }
    g.chunks.clear();
    g.blocks.clear();
    g.free_blocks.clear();
    g.recycled_blocks.clear();
    g.roots.clear();
    g.ranges.clear();
    g.container_tables.clear();
    g.containers.clear();
    g.container_work.clear();
    g.worklist.clear();
    g.pinned.clear();
    // Every entry names an object in a block that has just been freed, so the set must
    // not outlive the heap. The LOGGED bits go with the objects; there is nothing left
    // to re-arm and rt_gc_init re-establishes the rest of the generational state.
    g.remembered.clear();
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
    assert(g_mutator.cursor == nullptr && "a collection drops the hole before marking");
    assert(g_mutator.budget_left > 0 && "rt_gc_init floors the nursery budget");
}

// -- Collection policy (§7) ---------------------------------------------------

// Committed heap bytes: whole blocks plus large cells. Large cells belong in it because
// a young collection does not reclaim an unmarked OLD cell either (reclaim_large), so a
// design whose garbage is all large objects would otherwise never take a full one.
uint64_t heap_bytes() {
    const uint64_t blocks = g.stats.blocks_total * RT_GC_BLOCK_BYTES;
    uint64_t       bytes  = blocks;
    for (const LargeCell& cell : g.large) {
        assert(cell.bytes > kLargeObjectBytes && "a small object in the large list");
        bytes += cell.bytes;
    }
    assert(bytes >= blocks && "the heap total wrapped");
    return bytes;
}

// Which kind of collection to run. One place, and a SCHEDULE rather than a heuristic
// wherever the caller asked for one:
//
//   1. The first collection is always full. line_old has never been derived, so no young
//      collection may precede one, and there is no live baseline for the growth rule. It
//      costs exactly what a young collection costs, because nothing is OLD yet.
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
//      floating garbage, and it bounds it at a constant factor of the live set. Crossing
//      the soft ceiling forces one too: it is the one situation a young collection can do
//      nothing about at all.
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
    if (g.cfg.heap_bytes_max != 0 && heap >= g.cfg.heap_bytes_max) {
        return RT_GC_KIND_FULL;
    }
    assert(g.live_after_full < (1ull << 62) && "an implausible live-bytes baseline");
    uint64_t target = g.live_after_full * kFullGrowthFactor;
    if (target < kFullGrowthFloor) {
        target = kFullGrowthFloor;
    }
    return (heap > target) ? RT_GC_KIND_FULL : RT_GC_KIND_YOUNG;
}

// The collection-scoped parities, both moved in one place so that "which collection stamps
// which mark" is a single decision rather than two statements that can drift apart. Split
// from rt_gc_collect_kind for length (TigerStyle).
void advance_epochs(uint32_t kind) {
    assert(kind == RT_GC_KIND_YOUNG || kind == RT_GC_KIND_FULL);
    assert(g.mark_epoch == 1 || g.mark_epoch == 2);
    // Two values suffice for an object no collection has promoted, by the same argument
    // that covers lines: a finalizable object still carrying its bit was either marked by
    // the immediately preceding collection (so it holds the OTHER epoch) or has never been
    // marked (so it holds 0), because a dead one is finalized and its bit cleared by the
    // very next collection after it dies. It does NOT extend to an OLD object, which no
    // young collection stamps, and that is what full_epoch is for.
    g.mark_epoch = 3 - g.mark_epoch;
    assert(g.mark_epoch == 1 || g.mark_epoch == 2);
    // The other parity, and the ONLY place it moves. A full collection stamps every live
    // object, so toggling it here is what makes a stale OMARK necessarily the PREVIOUS full
    // parity -- and so readable as unmarked -- however many young collections skipped the
    // object in between. A young collection must NOT toggle it: it does not stamp the old
    // objects it skips, so moving the parity under them would read the whole old heap as
    // unmarked and free it.
    [[maybe_unused]] const uint64_t full_before = g.full_epoch;
    if (kind == RT_GC_KIND_FULL) {
        g.full_epoch ^= RT_GC_FLAG_OMARK;
    }
    assert((g.full_epoch != full_before) == (kind == RT_GC_KIND_FULL) &&
           "the full parity moved in a young collection, or stood still in a full one");
    assert((g.full_epoch == 0 || g.full_epoch == RT_GC_FLAG_OMARK) && "a two-valued parity");
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
    drain();
}

// The counters, and the invariants that are claims about the collection as a whole.
void account_collection(uint32_t reason, uint32_t kind) {
    assert(reason <= RT_GC_REASON_EXPLICIT && "an unknown collection reason");
    assert(kind == RT_GC_KIND_YOUNG || kind == RT_GC_KIND_FULL);
    g.stats.collections += 1;
    g.by_reason[reason] += 1;
    if (kind == RT_GC_KIND_FULL) {
        g.stats.full_collections += 1;
        // Only a full collection may write either of these: it is the only one that has
        // measured the WHOLE live set, and the only one that re-derived line_old from that
        // measurement rather than inheriting it.
        g.live_after_full = g.stats.bytes_live;
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
    // is silent, and both neighbours are usually nonzero -- block_index whenever the
    // mutator owns a block, and the top half of budget_left nearly always -- so the
    // failure mode is "collect at every safepoint" rather than a crash.
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
                 " explicit)\n",
                 g.stats.collections, g.by_reason[RT_GC_REASON_BUDGET],
                 g.by_reason[RT_GC_REASON_STRESS], g.by_reason[RT_GC_REASON_EXPLICIT]);
    std::fprintf(stderr, "gc: allocated %" PRIu64 " objects / %" PRIu64 " bytes\n",
                 g.stats.objects_allocated, g.stats.bytes_allocated);
    std::fprintf(stderr, "gc: live at last collection %" PRIu64 " objects / %" PRIu64 " bytes\n",
                 g.stats.objects_live, g.stats.bytes_live);
    std::fprintf(stderr,
                 "gc: heap %" PRIu64 " blocks of %u bytes, %" PRIu64 " free; soft ceiling %" PRIu64
                 " bytes\n",
                 g.stats.blocks_total, RT_GC_BLOCK_BYTES, g.stats.blocks_free,
                 g.cfg.heap_bytes_max);
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
    std::fflush(stderr);
}

// -- Verification -------------------------------------------------------------
//
// Everything below is debug-only: §13's heap-verify mode, which is pair-assertion
// discipline (TigerStyle) applied to the whole heap rather than to one
// function.
#ifndef NDEBUG

// What one rt_gc_verify() pass accumulates. A struct rather than a growing list of
// out-parameters because every one of these serves a claim that is heap-WIDE -- the live
// accounting the mark phase computed, and the size of the remembered set -- and so cannot
// be checked object by object.
struct VerifyTally {
    uint64_t objects = 0;
    uint64_t bytes   = 0;
    uint64_t logged  = 0;
    uint64_t old     = 0;
};

// Whether every line the object occupies still records an old survivor. A large cell has
// no lines and is trivially intact: reclaim_large() keeps or frees the whole cell
// (HAZARD 3), so a large object's bytes are never PARTLY reclaimed.
bool object_lines_protected(void* h, uint64_t size) {
    assert(h != nullptr);
    assert(size >= RT_GC_HEADER_BYTES + sizeof(void*));
    uint8_t* start = object_start(h);
    BlockHit hit{};
    if (!block_of(reinterpret_cast<uintptr_t>(start), &hit)) {
        return true;
    }
    const uint64_t off = static_cast<uint64_t>(start - hit.base);
    const uint32_t lo  = static_cast<uint32_t>(off / RT_GC_LINE_BYTES);
    const uint32_t hi  = static_cast<uint32_t>((off + size - 1) / RT_GC_LINE_BYTES);
    assert(hi < RT_GC_LINES_PER_BLOCK && "an object never spans out of its block");
    for (uint32_t i = lo; i <= hi; ++i) {
        if (hit.meta->line_old[i] == 0) {
            return false;
        }
    }
    return true;
}

// The soundness precondition for NOT tracing an un-remembered OLD object, checked against
// the heap rather than argued in a comment. mark_and_push() skips such an object on the
// claim that every reference it holds already names an object it would spare; this is the
// one place that claim is observable, and a young reference here would be exactly the edge
// a young collection cannot see -- the use-after-free this phase exists to avoid.
//
// BOTH halves of the object are checked, the inline handle fields and the container leaves,
// because since P4 a class handle can be a container element (LlvmBackend.cpp:2612-2615) and
// so `old.q.push_back(fresh)` is an old-to-young edge exactly as `old.f = fresh` is. The
// leaves go through push_container, which under g.verifying asserts is_marked_now on every
// element instead of marking it (mark_container_handle), and rt_gc_verify drains them once.
//
// Reading those leaf bytes is legitimate for the same reason the live path's is: this object
// is intact. Its caller has established that every line it occupies is still protected, and
// an object with managed leaves always has a finalizer (assert_map), so if it were dead
// beyond a full collection it would have been finalized and lost its start bit rather than
// reaching this function at all.
void verify_old_edges(void* h, const rt_gc_map* map) {
    assert(is_old(h) && "these are the edges of an OLD object");
    assert(spared_by_age(*gcword_ptr(h)) && "only a young collection leaves an OLD one unread");
    uint8_t* body = static_cast<uint8_t*>(h);
    for (uint32_t i = 0; i < map->nhandles; ++i) {
        void* child = *reinterpret_cast<void**>(body + map->handles[i]);
        if (child == nullptr) {
            continue;
        }
        assert(rt_gc_find_object(child) == child && "an OLD object holds an unresolvable handle");
        assert(is_marked_now(child) &&
               "an un-remembered OLD object holds a young reference the barrier missed");
    }
    for (uint32_t i = 0; i < map->nleaves; ++i) {
        const rt_elem_leaf& leaf = map->leaves[i];
        assert(leaf.ops != nullptr && "a class leaf has an element descriptor");
        if (is_container_kind(leaf.ops->kind)) {
            push_container(body + leaf.offset, leaf.ops->kind, leaf.ops);
        }
    }
}

// One object found through a start bitmap. The live/dead split is not fussiness: a
// dead object's bytes stay intact while it shares a line with a survivor, but its
// later lines may already be reclaimed and its handle fields overwritten, so only a
// live object's fields and line marks are claims about the heap.
void verify_object(uint8_t* start, uint64_t extent_max, VerifyTally* t) {
    assert(t != nullptr && "the verification tally is heap-wide");
    void*            h   = handle_of(start);
    const rt_gc_map* map = map_probe(h);
    assert(map != nullptr && "an object with a start bit has its vptr stored");
    assert_map(map);
    assert(map->size <= extent_max && "an object runs past its block or cell");
    assert(rt_gc_find_object(h) == h && "an object does not resolve to itself");
    assert(rt_gc_find_object(start) == h && "the header does not resolve to the handle");

    const uint64_t w = *gcword_ptr(h);
    assert((w & RT_GC_FLAG_FORWARDED) == 0 && "a forwarding word in a non-moving collector");
    assert((w & RT_GC_FLAG_PINNED) == 0 && "a pin outlived its collection");
    if ((w & RT_GC_FLAG_LOGGED) != 0) {
        // Counted, not tested against the set: the remembered set holds exactly the LOGGED
        // objects, and rt_gc_verify() compares the two totals once. Totals rather than
        // membership keeps this O(heap) instead of O(heap x set), and it proves the set is
        // duplicate-free as a side effect, since a doubled entry carries one bit and fills
        // two slots.
        assert((w & RT_GC_FLAG_OLD) != 0 && "a LOGGED object that was never promoted");
        t->logged += 1;
    }
    if ((w & RT_GC_FLAG_OLD) != 0) {
        t->old += 1;
    }
    if (!is_marked_word(w)) {
        // Dead but intact, or allocated since the last collection: a young object this
        // collection did not mark, or -- only in a FULL collection, the one whose verdict
        // on an old object is exact -- an OLD object it did not reach, which is genuine old
        // garbage. Either way its later lines may already belong to another object, so its
        // fields are a claim about nothing.
        return;
    }
    if (spared_by_age(w)) {
        // Spared by AGE rather than by a mark: a YOUNG collection may never have looked
        // inside this object, so it is the one case whose out-edges nothing else checks.
        if ((w & RT_GC_FLAG_LOGGED) != 0) {
            // Its remembered-set entry is still pending, and pending means a store has
            // landed that no collection has traced yet, so its fields are a claim about
            // nothing either.
            return;
        }
        if (!object_lines_protected(h, map->size)) {
            // Old garbage a FULL collection already partly freed, keeping its start bit in
            // a line a survivor shares, and now spared by age alone in a young one. Part of
            // its body belongs to a newer object, so its fields must not be read at all.
            return;
        }
        verify_old_edges(h, map);
        return;
    }
    uint8_t* body = static_cast<uint8_t*>(h);
    for (uint32_t i = 0; i < map->nhandles; ++i) {
        void* child = *reinterpret_cast<void**>(body + map->handles[i]);
        if (child == nullptr) {
            continue;
        }
        assert(rt_gc_find_object(child) == child && "a live object holds an unresolvable handle");
        assert(is_marked_now(child) && "a live object holds a handle it would not spare");
    }
    // A live object's container properties are checked exactly as its fields are, and
    // through the same walkers: pushed here, drained once by rt_gc_verify().
    for (uint32_t i = 0; i < map->nleaves; ++i) {
        const rt_elem_leaf& leaf = map->leaves[i];
        assert(leaf.ops != nullptr && "a class leaf has an element descriptor");
        if (is_container_kind(leaf.ops->kind)) {
            push_container(body + leaf.offset, leaf.ops->kind, leaf.ops);
        }
    }
}

// The two bitmaps, which GENERATED code now writes (design §4.3). set_object_bits'
// assertions cover only the runtime's own entry point; this is the other half of that
// pair, and it is the only thing standing between a mis-emitted bit and either an
// object the collector cannot see or a `string` buffer nobody frees.
void verify_bitmaps(const Block& blk) {
    const BlockMeta* m          = blk.meta;
    unsigned         any_fin    = 0;
    // First byte past the last LIVE object seen. Live-only, and that is not a weakening:
    // mark_lines() marks every line a live object's extent touches, and a hole is a run
    // of UNMARKED lines, so no allocation can land inside a live object and two live
    // objects can never overlap. A dead object's extent may well be overlapped -- if its
    // first line is shared with a survivor it keeps its start bit while its later lines
    // are freed and handed out again, which is the exact shape rt_gc_find_object's
    // "past the extent of the object that owns the nearest start bit" case exists for.
    uint64_t live_covered_to = 0;
    for (uint32_t i = 0; i < kBitmapBytes; ++i) {
        any_fin |= m->fin_bits[i];
        if (m->flags == kBlockFree) {
            assert(m->start_bits[i] == 0 && "a free block has an object start");
            assert(m->fin_bits[i] == 0 && "a free block owes a finalizer");
        }
        // A finalizable bit with no object start under it is a finalizer with nothing to
        // run on: a wrong bit index from the fast path, or a reclaimed line that cleared
        // one bitmap and not the other.
        assert((m->fin_bits[i] & ~static_cast<unsigned>(m->start_bits[i])) == 0 &&
               "a finalizable bit with no object start");
        unsigned bits = m->start_bits[i];
        while (bits != 0) {
            const uint32_t bit = static_cast<uint32_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const uint64_t   off = (static_cast<uint64_t>(i) * 8 + bit) * RT_GC_ALIGN;
            void* const      h   = handle_of(blk.base + off);
            const rt_gc_map* map = map_probe(h);
            assert(map != nullptr && "an object with a start bit has its descriptor");
            assert_map(map);
            assert(off + map->size <= RT_GC_BLOCK_BYTES && "an object runs past its block");
            // Heap parsability for the live heap, and the check set_object_bits' "an
            // object already starts here" cannot make for a store the runtime never
            // performs: no live object's extent holds another live object's start.
            //
            // is_stamped(), NOT is_marked_now(): the question here is whether a real mark
            // reached this object, because a young collection's sparing an OLD one by age
            // says nothing about whether its extent is still its own. An old object a FULL
            // collection killed keeps its start bit in a shared line while its later lines
            // are handed out again, and reading that as live would make the newer object
            // now starting inside it look like an overlap.
            if (is_stamped(*gcword_ptr(h))) {
                assert(off >= live_covered_to && "two live objects overlap in one block");
                live_covered_to = off + map->size;
            }
            // The biconditional, and the direction that matters is the missing bit: an
            // object whose class emitted a finalizer MUST carry one, or its string and
            // container buffers are never freed (design §0's second-order leak). By the
            // time verify runs, every finalizable object with a start bit is a survivor,
            // because sweep_finalizable() cleared both bits of every one it claimed.
            const bool fin = (m->fin_bits[i] & (1u << bit)) != 0;
            assert(fin == (map->finalize != nullptr) &&
                   "the finalizable bit disagrees with the class descriptor");
            assert(fin == ((*gcword_ptr(h) & RT_GC_FLAG_FINALIZE) != 0) &&
                   "the finalizable bit disagrees with the GC word");
        }
    }
    // publish_hole() sets this conservatively and reclaim_blocks() makes it exact, so it
    // may be set with no bit under it -- but never the other way round, which is what
    // makes sweep_finalizable() skipping a block safe.
    assert((any_fin == 0 || m->has_finalizable != 0) &&
           "a finalizable object in a block the sweep would skip");
}

void verify_block(const Block& blk, VerifyTally* t) {
    const BlockMeta* m     = blk.meta;
    const uint8_t    epoch = static_cast<uint8_t>(g.mark_epoch);
    for (uint32_t i = 0; i < RT_GC_LINES_PER_BLOCK; ++i) {
        // Epoch parity is sound only because reclamation zeroes freed lines; this
        // is that invariant, checked over every line in the heap.
        assert((m->line_mark[i] == epoch || m->line_mark[i] == 0) && "a stale line epoch");
        // HAZARD 1 from the far side: reclamation leaves the age record and the line mark
        // in exact agreement, because a line kept only because it holds an old survivor is
        // re-published as marked so that find_hole() cannot hand it out (reclaim_blocks).
        // Nothing between two collections disturbs either -- the allocator reads line
        // marks and never writes them -- so this holds wherever verify can run.
        assert((m->line_old[i] != 0) == (m->line_mark[i] == epoch) &&
               "a line's age record disagrees with its mark");
        if (m->flags == kBlockFree) {
            assert(m->line_mark[i] == 0 && "a free block has a marked line");
            assert(m->line_old[i] == 0 && "a free block holds an old survivor");
        }
    }
    for (uint32_t i = 0; i < kBitmapBytes; ++i) {
        unsigned bits = m->start_bits[i];
        while (bits != 0) {
            const uint32_t bit   = static_cast<uint32_t>(std::countr_zero(bits));
            const uint64_t off   = (static_cast<uint64_t>(i) * 8 + bit) * RT_GC_ALIGN;
            uint8_t*       start = blk.base + off;
            verify_object(start, RT_GC_BLOCK_BYTES - off, t);
            void*          h    = handle_of(start);
            // is_stamped(), not is_marked_now(): "a real mark reached this object", which is
            // what makes the extent claims below true. For a young collection that is the
            // young parity of a fresh survivor and OMARK for an old one -- both bounded, so
            // neither can be a stale value left by some earlier collection.
            const bool     live = is_stamped(*gcword_ptr(h));
            const uint32_t l0   = static_cast<uint32_t>(off / RT_GC_LINE_BYTES);
            if (live) {
                const uint64_t size = map_of(h)->size;
                t->objects += 1;
                t->bytes += size;
                const uint32_t hi = static_cast<uint32_t>((off + size - 1) / RT_GC_LINE_BYTES);
                for (uint32_t l = l0; l <= hi; ++l) {
                    assert(m->line_mark[l] == epoch && "a live object sits in a reclaimed line");
                    assert(m->line_old[l] != 0 && "a live object's line lost its protection");
                }
            } else if (is_old(h)) {
                // HAZARD 1 per object, in the one form true of EVERY old object: its start
                // line is protected. A freed line has its start bits cleared, so a surviving
                // start bit means the line was kept, and reclaim_blocks keeps a line only by
                // recording it in line_old -- so if a young collection ever freed a line
                // holding an old survivor, the bit and the record part company right here.
                // The whole extent is checked where it is knowable: for a live object just
                // above, and for an un-remembered old one in verify_object, which needs the
                // same fact before it may read that object's fields at all.
                assert(m->line_old[l0] != 0 && "an OLD object's start line lost its protection");
            }
            bits &= bits - 1;
        }
    }
}

void verify_large(VerifyTally* t) {
    for (size_t i = 0; i < g.large.size(); ++i) {
        const LargeCell& cell = g.large[i];
        if (i > 0) {
            assert(g.large[i - 1].start < cell.start && "the large list is base-sorted");
        }
        assert(cell.bytes > kLargeObjectBytes && "a small object in the large list");
        verify_object(cell.start, cell.bytes, t);
        void* h = handle_of(cell.start);
        if (is_stamped(*gcword_ptr(h))) {
            t->objects += 1;
            t->bytes += map_of(h)->size;
        }
    }
}

// §13 from the ROOT side, and the invariant no heap walk in this file can state:
// reclaiming a live object clears its object-start bit, so it vanishes from every bitmap
// walked above and the only remaining witness is the root that still names it. That is
// why the mark-staleness bug (Gc.h:101-109) surfaced as a located fatal in the mutator one
// collection later rather than as an assertion in the collection that caused it.
//
// Two claims, both true BETWEEN collections as well as at the end of one -- which is what
// lets them live in rt_gc_verify(), a function tests call at arbitrary times, rather than
// in the collection driver:
//   * a root names an object the heap still owns. Design state is never recycled and
//     nothing is freed outside a collection, so a root that stops resolving names a live
//     object some collection reclaimed.
//   * a root that has EVER survived a collection is one this collection would spare. Every
//     collection marks or spares every root, and marking promotes, so an OLD root must
//     satisfy the predicate. A root allocated since the last collection legitimately holds
//     no mark yet, which is exactly what the OLD guard concedes.
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
            if (h == nullptr) {
                continue;
            }
            assert(rt_gc_find_object(h) == h && "a precise root names a reclaimed object");
            assert((!is_old(h) || is_marked_now(h)) &&
                   "a precise root names an OLD object this collection would not spare");
        }
    }
    // The same pair of claims for a container root's OWNER, which is the shape whose
    // failure runs a finalizer over a live object's string and container buffers rather
    // than merely dangling one handle.
    for (const auto& kv : g.containers) {
        void* owner = kv.second.owner;
        if (owner == nullptr) {
            continue;
        }
        assert(rt_gc_find_object(owner) == owner && "a container owner was reclaimed");
        assert((!is_old(owner) || is_marked_now(owner)) &&
               "a container root's owner is an OLD object this collection would not spare");
    }
}

// The heap-wide totals, split from rt_gc_verify() for length (TigerStyle). Every
// claim here is about a counter the walk produced rather than about any one object.
void verify_totals(const VerifyTally& tally) {
    // The two parities against an independent witness. mark_epoch toggles at EVERY
    // collection and full_epoch only at a full one, so each is a function of its own
    // counter -- the whole two-mark design stated as arithmetic. It fails if a young
    // collection ever moves the full parity under the old objects it skips.
    assert(g.mark_epoch == ((g.stats.collections & 1) != 0 ? 2u : 1u) &&
           "the mark parity disagrees with the collection count");
    assert(g.full_epoch == ((g.stats.full_collections & 1) != 0 ? RT_GC_FLAG_OMARK : 0ull) &&
           "the full parity disagrees with the full-collection count");
    // The pair half of the mark phase's own accounting: walking the heap through the
    // object-start bitmaps must find the objects marking counted, or either the bitmap or
    // the mark bookkeeping is wrong.
    //
    // Exact for a FULL collection and one-sided for a young one, and that asymmetry is the
    // truth rather than a weakening. The walk counts every object is_stamped() finds: for a
    // full collection precisely the set it marked, hence equality; for a young one that set
    // PLUS the old survivors of the last full collection, which this collection never
    // visited and so never counted. The old equality held only while a stale young parity
    // happened not to collide with the current epoch on those survivors, and that collision
    // is exactly what the second parity removes.
    const bool exact = (g.kind == RT_GC_KIND_FULL);
    assert((exact ? tally.objects == g.stats.objects_live
                  : tally.objects >= g.stats.objects_live) &&
           "the heap walk disagrees on live objects");
    assert((exact ? tally.bytes == g.stats.bytes_live : tally.bytes >= g.stats.bytes_live) &&
           "the heap walk disagrees on live bytes");
    // The remembered set holds EXACTLY the objects carrying LOGGED, and both directions
    // matter: an object logged but not recorded is an old-to-young edge the next young
    // collection will not trace, and a recorded object whose bit was cleared is a barrier
    // that can never fire for it again. After a collection both sides hold precisely what
    // relog_conservative_roots() re-armed; between collections they grow together.
    assert(tally.logged == g.remembered.size() && "a LOGGED object outside the remembered set");
    for (void* h : g.remembered) {
        assert((*gcword_ptr(h) & RT_GC_FLAG_LOGGED) != 0 && "a remembered entry lost its bit");
        assert(is_old(h) && "a remembered entry is an OLD object");
    }
    // The generational accounting: the two kinds partition the collections, promotion is
    // monotone in the objects that ever existed, and nothing carries OLD that no
    // collection ever promoted.
    assert(g.stats.young_collections + g.stats.full_collections == g.stats.collections &&
           "every collection is either young or full");
    assert(g.stats.promoted <= g.stats.objects_allocated && "more promotions than objects");
    assert(tally.old <= g.stats.promoted && "an OLD object no collection ever promoted");
    for (size_t i = 1; i < g.chunks.size(); ++i) {
        assert(g.chunks[i - 1].base < g.chunks[i].base && "the chunk list is base-sorted");
    }
    assert(g.blocks.size() == g.stats.blocks_total && "block ids are dense and stable");
}

#endif  // NDEBUG

}  // namespace

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
    g.stats      = rt_gc_stats{};
    g.mark_epoch = 1;
    // Either value would do -- nothing is OLD in a fresh run, so no OMARK is read before
    // the first full collection stamps one -- but a fixed start is what makes the parity a
    // function of stats.full_collections, which rt_gc_verify() checks.
    g.full_epoch = 0;
    // The generational state a fresh run starts from (§7): nothing is old, nothing is
    // logged, and the first collection is full -- which choose_kind() reads off
    // stats.full_collections, zeroed just above, rather than from a flag of its own.
    g.kind            = RT_GC_KIND_FULL;
    g.force_full      = 0;
    g.live_after_full = 0;
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
        // else is needed to make stress collect at every safepoint reached, and it stays
        // per-safepoint rather than per-allocation because rt_gc_alloc never collects.
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
    g.initialized = 0;
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

extern "C" void* rt_gc_alloc(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map) {
    // The whole allocation sequence, out of line. Generated code inlines the bump and
    // the two bitmap stores and reaches this only for a large object, so in a collected
    // run this entry point serves large objects, the runtime's own callers and the tests
    // (Gc.h:236-245).
    return alloc_object(mut, size, map);
}

extern "C" void* rt_gc_alloc_slow(rt_gc_mutator* mut, uint64_t size, const rt_gc_map* map) {
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
    return alloc_object(mut, size, map);
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
    assert((owner == nullptr || rt_gc_find_object(owner) == owner) &&
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

// The mutator layout, pinned field by field. Generated code no longer loads only the
// poll byte from this struct: the whole inlined allocation fast path indexes it --
// `cursor`, `limit`, `block_base`, `start_bitmap`, `fin_bitmap` -- at constant offsets
// the backend computes from its own mirror (Gc.h:10-14, design §4.3). A silent
// reordering here would point a bitmap store at another field, so every offset is
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
static_assert(offsetof(rt_gc_mutator, block_index) ==
                offsetof(rt_gc_mutator, budget_left) + sizeof(uint64_t),
              "block_index follows budget_left");
static_assert(offsetof(rt_gc_mutator, request) ==
                offsetof(rt_gc_mutator, block_index) + sizeof(uint32_t),
              "request sits immediately after block_index");
static_assert(offsetof(rt_gc_mutator, block_base) ==
                offsetof(rt_gc_mutator, request) + sizeof(uint32_t),
              "the two u32s pack, so block_base needs no padding after the poll byte");
static_assert(offsetof(rt_gc_mutator, start_bitmap) ==
                offsetof(rt_gc_mutator, block_base) + sizeof(uint8_t*),
              "start_bitmap follows block_base");
static_assert(offsetof(rt_gc_mutator, fin_bitmap) ==
                offsetof(rt_gc_mutator, start_bitmap) + sizeof(uint8_t*),
              "fin_bitmap follows start_bitmap: the two bitmaps are adjacent");
static_assert(offsetof(rt_gc_mutator, frames_head) ==
                offsetof(rt_gc_mutator, fin_bitmap) + sizeof(uint8_t*),
              "frames_head follows the cached block metadata");
static_assert(sizeof(rt_gc_mutator) ==
                offsetof(rt_gc_mutator, frames_head) + sizeof(struct rt_gc_frame*),
              "frames_head is the last field: the mirror knows every one of them");
// The numbers those expressions come to on the LP64 host the backend targets, spelled
// out so a reader can check the mirror by eye against LlvmBackend.cpp's gc_mutator_ty
// without evaluating nine offsetof chains.
static_assert(offsetof(rt_gc_mutator, cursor) == 0 && offsetof(rt_gc_mutator, limit) == 8 &&
                offsetof(rt_gc_mutator, budget_left) == 16 &&
                offsetof(rt_gc_mutator, block_index) == 24 &&
                offsetof(rt_gc_mutator, request) == 28 &&
                offsetof(rt_gc_mutator, block_base) == 32 &&
                offsetof(rt_gc_mutator, start_bitmap) == 40 &&
                offsetof(rt_gc_mutator, fin_bitmap) == 48 &&
                offsetof(rt_gc_mutator, frames_head) == 56 && sizeof(rt_gc_mutator) == 64,
              "the LP64 mutator layout the backend mirrors: 0/8/16/24/28/32/40/48/56, 64 bytes");

extern "C" uint32_t rt_gc_mutator_request_offset(void) {
    // 28 on a 64-bit host: two pointers, one u64 budget, one u32 block id.
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
    assert(reason <= RT_GC_REASON_EXPLICIT && "an unknown collection reason");
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
    // The mutator's bump range points into a hole whose lines are about to be
    // re-partitioned, so it goes FIRST -- before the epoch toggle, before marking, and
    // in particular before run_finalizers(). Not merely tidiness: drop_hole() retires
    // the hole, and retirement counts the objects it handed out by popcounting the start
    // bits the inlined fast path set, which the sweep is about to start clearing for
    // every object it finalizes. Retiring after the sweep would silently lose one from
    // objects_allocated per finalized object that came out of the live hole.
    drop_hole(&g_mutator);

    // Both collection-scoped parities, in the one function that owns them.
    advance_epochs(kind);
    // Published before the first mark, because the kind is what mark_and_push() tests,
    // and it stays set through the sweeps and reclamation, which test it too. One
    // marking path, parameterised here.
    g.kind = kind;

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

    mark_from_roots();

    // Finalizers before reclamation: the buffers they free are inside object bodies
    // whose lines are about to be handed out again.
    run_finalizers();
    reclaim_blocks();
    reclaim_large();
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
    assert(reason <= RT_GC_REASON_EXPLICIT && "an unknown collection reason");
    assert(g.initialized != 0 && "rt_gc_collect before rt_gc_init");
    if (g.initialized == 0) {
        return;
    }
    // The only difference between the two entry points: this one applies the policy, and
    // rt_gc_collect_kind is what the policy resolves to. Tests and --gc-full-every reach
    // the kinds directly; the safepoint reaches them through here.
    rt_gc_collect_kind(reason, choose_kind());
}

// The out-of-line half of the inlined write barrier (Gc.h:98-108). Generated code has
// already tested `(gcword & (OLD|LOGGED)) == OLD` and taken the cold edge, so reaching
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
    assert(rt_gc_find_object(obj) == obj && "the barrier logged something not a class object");
    if ((*gcword_ptr(obj) & RT_GC_FLAG_OLD) == 0) {
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
    // Five rejection cases, each its own return below, because this one function is
    // both the conservative-scanning validator and the interior-pointer resolver
    // that makes a `ref` into a class property safe with no epoch machinery
    // (Gc.h:244-249).
    if (addr == nullptr) {
        return nullptr;  // (1) a null handle is the power-on default, not an object
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    // (2) Outside every chunk and every large cell: a stack word, a frame pointer, a
    // string buffer, an integer that happens to look like an address, a chandle from
    // DPI code -- which the collector must never trace or reclaim (design §0).
    // block_start_of() also rejects (3) an address in a wholly free block, whose
    // bitmap reclamation cleared, and (4) an address below the first object start in
    // its block, which is a hole at the head of a recycled block.
    uint8_t* start = block_start_of(a);
    if (start == nullptr) {
        start = large_start_of(a);
        if (start == nullptr) {
            return nullptr;
        }
    }
    assert(reinterpret_cast<uintptr_t>(start) % RT_GC_ALIGN == 0 && "a start bit is grain-aligned");
    assert(a >= reinterpret_cast<uintptr_t>(start) && "the scan walked backwards");
    void*            h   = handle_of(start);
    const rt_gc_map* map = map_probe(h);
    if (map == nullptr) {
        // (5a) The vptr is not stored yet. Only reachable between rt_gc_alloc
        // returning and the store at LlvmBackend.cpp:11696, where no safepoint can
        // intervene -- but a conservative candidate must not be believed on the
        // strength of a start bit alone, so this is a rejection and not an assert.
        return nullptr;
    }
    assert_map(map);
    if (a >= reinterpret_cast<uintptr_t>(start) + map->size) {
        // (5b) Past the extent of the object that owns the nearest start bit: the
        // free tail of a hole, or padding between the last object in a line run and
        // the next marked line. The nearest preceding start bit exists, so the
        // bitmap alone cannot reject this -- only the size from the map can.
        return nullptr;
    }
    return h;
}

extern "C" void rt_gc_verify(void) {
#ifndef NDEBUG
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
    VerifyTally tally;
    for (const Block& blk : g.blocks) {
        // Two claims about every block, kept apart because they are different claims: the
        // line marks and the live accounting, then the two BITMAPS generated code writes.
        verify_block(blk, &tally);
        verify_bitmaps(blk);
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
