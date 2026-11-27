#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/Gc.h"
#include "runtime/Runtime.h"

// The collector against a model, at its C interface and with no design in between.
//
// The test builds an object graph by the calls generated code makes (rt_gc_alloc, a
// store followed by the write barrier's out-of-line half, a precise root table, a
// conservatively scanned frame) and keeps its own copy of that graph as plain
// indices. After every collection it computes what the copy says is reachable and
// holds the collector to it:
//
//   - every reachable object is intact: its descriptor, its stamp, its filler and
//     each handle field are what the model wrote, so nothing reachable was freed and
//     nothing handed its memory to another object;
//   - a finalizer ran at most once per object, only for an object the model can no
//     longer reach, and found the body intact;
//   - after a FULL collection the collector's live count and live bytes are exactly
//     the model's, every unreachable finalizable object has been finalized, and the
//     identity table holds exactly the numbered objects that are reachable;
//   - an address inside a reachable object resolves to that object, and an address
//     outside the heap to nothing.
//
// What it cannot see is what a design sees: generated code's inlined allocation and
// barrier paths, and container tracing. tests/gc_test.cpp covers those. What it
// adds is volume and shape: thousands of random mutations per run, object sizes
// from one grain to larger than a block, old and young objects pointing at each
// other in every direction, and an oracle that does not depend on how the heap is
// laid out -- which is what lets the same file check a collector whose metadata has
// been rearranged.
namespace {

constexpr uint32_t kFieldsMax = 4;
// An object's body, from the handle: the descriptor pointer, the model's index for
// the object, a stamp derived from it, the handle fields, then filler words.
constexpr uint32_t kIndexOffset  = 8;
constexpr uint32_t kStampOffset  = 16;
constexpr uint32_t kFieldsOffset = 24;

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

uint64_t load(const void* base, uint64_t offset) {
    uint64_t v;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof v);
    return v;
}

void store(void* base, uint64_t offset, uint64_t v) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &v, sizeof v);
}

// One finalizer run, as the callback saw it.
struct Finalized {
    uint64_t index;
    bool     intact;
};
std::vector<Finalized>* g_finalized = nullptr;

void on_finalize(void* body) {
    const uint64_t index = load(body, kIndexOffset);
    g_finalized->push_back({ index, load(body, kStampOffset) == mix(index) });
}

// A class as the collector sees one: a trace map, and a descriptor whose only content
// is the map pointer at the offset below the vptr where the collector looks for it.
//
// A weak shape is a weak reference class: its first filler word is the referent, which
// the map names and leaves out of the handles. A notified one keeps the handle of its
// notifier in its first handle field.
struct Shape {
    uint32_t nhandles;
    uint32_t filler_words;
    bool     finalizable;
    bool     weak     = false;
    bool     notified = false;

    rt_gc_map   map{};
    uint32_t    offsets[kFieldsMax]{};
    const void* vptr = nullptr;
    alignas(void*) uint8_t desc[128]{};

    uint64_t referent_offset() const {
        return kFieldsOffset + 8ull * nhandles;
    }

    uint64_t body_bytes() const { return kFieldsOffset + 8ull * nhandles + 8ull * filler_words; }

    void build() {
        for (uint32_t k = 0; k < nhandles; ++k) {
            offsets[k] = kFieldsOffset + 8 * k;
        }
        // The one place this file knows how an object's size follows from its body.
        map.size     = (body_bytes() + RT_GC_ALIGN - 1) / RT_GC_ALIGN * RT_GC_ALIGN;
        map.nhandles = nhandles;
        map.nleaves  = 0;
        map.handles  = nhandles != 0 ? offsets : nullptr;
        map.leaves   = nullptr;
        map.finalize = finalizable ? on_finalize : nullptr;
        REQUIRE((!weak || filler_words != 0));
        REQUIRE((!notified || (weak && nhandles != 0)));
        map.weak_referent            = weak ? static_cast<uint32_t>(referent_offset()) : 0;
        map.weak_notify              = notified ? kFieldsOffset : 0;
        const auto            below = static_cast<size_t>(-rt_gc_map_vptr_offset());
        const rt_gc_map* const m     = &map;
        REQUIRE(below + sizeof(void*) <= sizeof desc);
        std::memcpy(desc, &m, sizeof m);
        vptr = desc + below;
    }
};

// From one grain to larger than a block. Of the first ten the last three do not fit in
// a block and get a mapping of their own; the two before them are found from an
// interior address in more than one step back. The model allocates all of those but
// the last, and the two weak shapes after them. The last two are for the notifier's
// test: a weak reference with one, and an object the size of an event object.
Shape g_shapes[] = {
    { 0, 0, false },
    { 1, 1, false },
    { 2, 0, true },
    { 4, 3, false },
    { 3, 40, true },
    { 1, 300, false },
    { 2, 1500, true },
    { 2, 5000, false },
    { 1, 6000, true },
    { 0, 7000, false },
    { 1, 2, false, true },
    { 0, 1, true, true },
    { 1, 1, false, true, true },
    { 0, 4, false },
};
constexpr uint32_t kShapes        = sizeof g_shapes / sizeof g_shapes[0];
constexpr uint32_t kWeakShapes    = 10;
constexpr uint32_t kNotifiedShape = 12;
constexpr uint32_t kEventShape    = 13;

struct Model {
    static constexpr uint32_t kRoots = 24;
    static constexpr uint32_t kFrame = 24;
    static constexpr int32_t  kNone  = -1;

    struct Obj {
        void*    handle;
        uint32_t shape;
        int32_t  field[kFieldsMax];
        bool     finalized;
        uint64_t number;  // its identity number, 0 until asked
        int32_t  weak;    // a weak shape's referent
    };

    std::vector<Obj>       objs;
    std::vector<Finalized> finalized;
    size_t                 finalized_seen = 0;
    uint64_t               last_number    = 0;

    // The precise roots: a table whose slots the collector reads when a store to them is logged
    // (rt_gc_note_state_store) or the collection is full. The word after them holds the card byte.
    uint64_t    root_words[kRoots + 1]{};
    int32_t     root_obj[kRoots];
    uint32_t    root_offsets[kRoots];
    rt_gc_roots root_table{};
    // The conservative roots: a frame scanned word by word. A word is a handle, an
    // address inside an object, or something that is not a heap address at all.
    uint64_t    frame_words[kFrame]{};
    int32_t     frame_obj[kFrame];
    rt_gc_frame frame_node{};

    void*    pend_head = nullptr;
    uint64_t rng;
    uint64_t collections_checked = 0;
    uint64_t full_checked        = 0;
    uint64_t weak_cleared        = 0;
    uint64_t weak_retaken        = 0;

    std::vector<uint8_t>  reach;
    std::vector<int32_t>  reach_list;
    std::vector<int32_t>  work;

    explicit Model(uint64_t seed, int32_t full_every) : rng(mix(seed)) {
        for (Shape& s : g_shapes) {
            s.build();
        }
        g_finalized = &finalized;
        rt_gc_config cfg{};
        cfg.nursery_bytes   = RT_GC_BLOCK_BYTES;
        cfg.map_vptr_offset = rt_gc_map_vptr_offset();
        cfg.request_offset  = static_cast<int32_t>(rt_gc_mutator_request_offset());
        cfg.mutator_bytes   = static_cast<int32_t>(rt_gc_mutator_bytes());
        cfg.full_every      = full_every;
        rt_gc_init(&cfg);
        // What generated code calls next: it resets the runtime's own records of
        // event objects, which name objects of whatever run came before.
        rt_run_init(&pend_head);
        for (uint32_t k = 0; k < kRoots; ++k) {
            root_obj[k]     = kNone;
            root_offsets[k] = 8 * k;
        }
        for (uint32_t k = 0; k < kFrame; ++k) {
            frame_obj[k] = kNone;
        }
        root_table.base     = root_words;
        root_table.nhandles = kRoots;
        root_table.cards    = 8 * kRoots;
        root_table.bytes    = 8 * kRoots + 1;
        root_table.offsets  = root_offsets;
        rt_gc_roots_add(&root_table);
        rt_gc_frame_link(&frame_node, frame_words, sizeof frame_words);
    }

    ~Model() {
        rt_gc_frame_unlink(&frame_node);
        rt_gc_shutdown();
        g_finalized = nullptr;
    }

    uint64_t next() {
        rng = mix(rng);
        return rng;
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }

    // What the model says a collection must keep, from both root sets.
    void compute_reach() {
        reach.assign(objs.size(), 0);
        reach_list.clear();
        work.clear();
        const auto push = [&](int32_t i) {
            if (i != kNone && reach[static_cast<size_t>(i)] == 0) {
                reach[static_cast<size_t>(i)] = 1;
                reach_list.push_back(i);
                work.push_back(i);
            }
        };
        for (const int32_t i : root_obj) {
            push(i);
        }
        for (const int32_t i : frame_obj) {
            push(i);
        }
        while (!work.empty()) {
            const Obj& o = objs[static_cast<size_t>(work.back())];
            work.pop_back();
            for (uint32_t k = 0; k < g_shapes[o.shape].nhandles; ++k) {
                push(o.field[k]);
            }
        }
    }

    int32_t pick_reachable() {
        compute_reach();
        return reach_list.empty() ? kNone : reach_list[below(static_cast<uint32_t>(reach_list.size()))];
    }

    int32_t pick_reachable_weak() {
        compute_reach();
        work.clear();
        for (const int32_t i : reach_list) {
            if (g_shapes[objs[static_cast<size_t>(i)].shape].weak) {
                work.push_back(i);
            }
        }
        return work.empty() ? kNone : work[below(static_cast<uint32_t>(work.size()))];
    }

    void* handle_of(int32_t i) const { return i == kNone ? nullptr : objs[static_cast<size_t>(i)].handle; }

    // Put a reference to `i` (or null) somewhere the collector will find it.
    void attach(int32_t i) {
        const uint32_t where = below(4);
        if (where == 0) {
            const uint32_t k = below(kFrame);
            frame_obj[k]     = i;
            // Half of the frame's references are addresses inside the object.
            uint64_t word = reinterpret_cast<uint64_t>(handle_of(i));
            if (i != kNone && below(2) == 0) {
                word += 8ull * below(static_cast<uint32_t>(g_shapes[objs[static_cast<size_t>(i)].shape].body_bytes() / 8));
            }
            frame_words[k] = word;
            return;
        }
        if (where == 1) {
            const int32_t owner = pick_reachable();
            if (owner != kNone && owner != i && g_shapes[objs[static_cast<size_t>(owner)].shape].nhandles != 0) {
                store_field(owner, below(g_shapes[objs[static_cast<size_t>(owner)].shape].nhandles), i);
                return;
            }
        }
        const uint32_t k = below(kRoots);
        root_obj[k]      = i;
        root_words[k]    = reinterpret_cast<uint64_t>(handle_of(i));
        rt_gc_note_state_store(&root_words[k], sizeof(uint64_t));
    }

    // A store of a handle into an object, as generated code does one: the store, and
    // the barrier's out-of-line half, which tests the owner's age itself.
    void store_field(int32_t owner, uint32_t k, int32_t value) {
        Obj& o     = objs[static_cast<size_t>(owner)];
        o.field[k] = value;
        store(o.handle, kFieldsOffset + 8ull * k, reinterpret_cast<uint64_t>(handle_of(value)));
        rt_gc_remember(o.handle);
    }

    void allocate() {
        // Small shapes mostly; a large object now and then, and a weak reference.
        const uint32_t pick  = below(16);
        const uint32_t shape = pick == 0 ? 6 + below(3) : pick <= 2 ? kWeakShapes + below(2) : below(6);
        Shape&         s     = g_shapes[shape];
        void* const    h     = rt_gc_alloc(rt_gc_mutator_current(), s.map.size, &s.map, nullptr);
        REQUIRE(h != nullptr);
        const auto index = static_cast<uint64_t>(objs.size());
        for (uint64_t off = 0; off < s.body_bytes(); off += 8) {
            REQUIRE(load(h, off) == 0);  // a fresh body is zero
        }
        store(h, 0, reinterpret_cast<uint64_t>(s.vptr));
        store(h, kIndexOffset, index);
        store(h, kStampOffset, mix(index));
        for (uint32_t w = s.weak ? 1 : 0; w < s.filler_words; ++w) {
            store(h, kFieldsOffset + 8ull * s.nhandles + 8ull * w, mix(index * 0x100 + w));
        }
        Obj o{ h, shape, { kNone, kNone, kNone, kNone }, false, 0, kNone };
        if (s.weak) {
            // Made of a referent, or of null (LRM 8.30.2). The store is to an object
            // no collection has seen, as the constructor's is.
            o.weak = below(4) == 0 ? kNone : pick_reachable();
            store(h, s.referent_offset(), reinterpret_cast<uint64_t>(handle_of(o.weak)));
        }
        objs.push_back(o);
        attach(static_cast<int32_t>(index));
    }

    void step() {
        const uint32_t op = below(100);
        if (op < 40) {
            allocate();
        } else if (op < 60) {
            // Point a reachable object at another, or at nothing.
            const int32_t owner = pick_reachable();
            if (owner != kNone && g_shapes[objs[static_cast<size_t>(owner)].shape].nhandles != 0) {
                const int32_t value = below(4) == 0 ? kNone : reach_list[below(static_cast<uint32_t>(reach_list.size()))];
                store_field(owner, below(g_shapes[objs[static_cast<size_t>(owner)].shape].nhandles), value);
            }
        } else if (op < 82) {
            // Drop a root. A dropped frame word becomes something that is no heap
            // address: a small integer, or the address of a local.
            if (below(2) == 0) {
                const uint32_t k = below(kRoots);
                root_obj[k]      = kNone;
                root_words[k]    = 0;
            } else {
                const uint32_t k = below(kFrame);
                frame_obj[k]     = kNone;
                frame_words[k]   = below(2) == 0 ? below(4096) : reinterpret_cast<uint64_t>(&op);
            }
        } else if (op < 88) {
            attach(pick_reachable());
        } else if (op < 92) {
            // Through a weak reference: clear it, or take its referent and hold it.
            // The referent may be one nothing else reaches that no collection has
            // freed yet, and from here on it is reachable again.
            const int32_t i = pick_reachable_weak();
            if (i != kNone) {
                Obj&           o   = objs[static_cast<size_t>(i)];
                const uint64_t off = g_shapes[o.shape].referent_offset();
                sync_weak(o);
                REQUIRE(load(o.handle, off) == reinterpret_cast<uint64_t>(handle_of(o.weak)));
                if (below(3) == 0) {
                    o.weak = kNone;
                    store(o.handle, off, 0);
                    rt_gc_remember(o.handle);
                } else if (o.weak != kNone) {
                    weak_retaken += reach[static_cast<size_t>(o.weak)] == 0 ? 1 : 0;
                    attach(o.weak);
                }
            }
        } else {
            // Ask for an identity number. A new one is larger than every earlier one.
            const int32_t i = pick_reachable();
            if (i != kNone) {
                Obj&           o = objs[static_cast<size_t>(i)];
                const uint64_t n = rt_gc_identity(o.handle);
                if (o.number == 0) {
                    REQUIRE(n > last_number);
                    last_number = n;
                    o.number    = n;
                }
                REQUIRE(n == o.number);
                REQUIRE(rt_gc_identity_object(n) == o.handle);
            }
        }
    }

    void check_object(int32_t i) {
        const Obj&   o = objs[static_cast<size_t>(i)];
        const Shape& s = g_shapes[o.shape];
        REQUIRE(!o.finalized);
        REQUIRE(load(o.handle, 0) == reinterpret_cast<uint64_t>(s.vptr));
        REQUIRE(load(o.handle, kIndexOffset) == static_cast<uint64_t>(i));
        REQUIRE(load(o.handle, kStampOffset) == mix(static_cast<uint64_t>(i)));
        for (uint32_t k = 0; k < s.nhandles; ++k) {
            REQUIRE(load(o.handle, kFieldsOffset + 8ull * k) == reinterpret_cast<uint64_t>(handle_of(o.field[k])));
        }
        for (uint32_t w = s.weak ? 1 : 0; w < s.filler_words; ++w) {
            REQUIRE(load(o.handle, kFieldsOffset + 8ull * s.nhandles + 8ull * w) ==
                    mix(static_cast<uint64_t>(i) * 0x100 + w));
        }
        // The handle and the last word of the body both resolve to the object.
        REQUIRE(rt_gc_find_object(o.handle) == o.handle);
        REQUIRE(rt_gc_find_object(static_cast<uint8_t*>(o.handle) + s.body_bytes() - 8) == o.handle);
        if (o.number != 0) {
            REQUIRE(rt_gc_identity(o.handle) == o.number);
        }
    }

    // Collect, and hold the collector to the model.
    void collect(int32_t kind) {
        compute_reach();
        for (const int32_t i : reach_list) {
            if (g_shapes[objs[static_cast<size_t>(i)].shape].weak) {
                sync_weak(objs[static_cast<size_t>(i)]);
            }
        }
        rt_gc_stats before{};
        rt_gc_get_stats(&before);
        if (kind < 0) {
            rt_gc_collect(RT_GC_REASON_EXPLICIT);
        } else {
            rt_gc_collect_kind(RT_GC_REASON_EXPLICIT, static_cast<uint32_t>(kind));
        }
        rt_gc_stats st{};
        rt_gc_get_stats(&st);
        REQUIRE(st.collections == before.collections + 1);
        const bool full = st.full_collections != before.full_collections;

        compute_reach();
        for (const int32_t i : reach_list) {
            check_object(i);
        }
        for (; finalized_seen < finalized.size(); ++finalized_seen) {
            const Finalized f = finalized[finalized_seen];
            REQUIRE(f.index < objs.size());
            Obj& o = objs[f.index];
            REQUIRE(f.intact);
            REQUIRE(g_shapes[o.shape].finalizable);
            REQUIRE(!o.finalized);
            REQUIRE(reach[f.index] == 0);
            o.finalized = true;
        }
        REQUIRE(st.finalized == finalized.size());
        check_weak(full, st.weak_cleared - before.weak_cleared);
        REQUIRE(rt_gc_find_object(&st) == nullptr);
        collections_checked += 1;
        if (!full) {
            return;
        }
        // A full collection has an opinion about every object, so its counts are the
        // model's and nothing unreachable is left waiting.
        uint64_t bytes    = 0;
        uint64_t numbered = 0;
        for (const int32_t i : reach_list) {
            bytes += g_shapes[objs[static_cast<size_t>(i)].shape].map.size;
            numbered += objs[static_cast<size_t>(i)].number != 0 ? 1 : 0;
        }
        REQUIRE(st.objects_live == reach_list.size());
        REQUIRE(st.bytes_live == bytes);
        REQUIRE(st.identities == numbered);
        for (size_t i = 0; i < objs.size(); ++i) {
            if (reach[i] == 0 && g_shapes[objs[i].shape].finalizable) {
                REQUIRE(objs[i].finalized);
            }
        }
        full_checked += 1;
    }

    // A weak reference the model had lost sight of may have been cleared meanwhile: a
    // young collection keeps an unreachable object that an old, logged one names, and
    // clears it as it clears any other. Its referent was freed then and is not seen
    // again. Called with `reach` current, before the model reads through `o`.
    void sync_weak(Obj& o) {
        if (o.weak != kNone && load(o.handle, g_shapes[o.shape].referent_offset()) == 0) {
            REQUIRE(reach[static_cast<size_t>(o.weak)] == 0);
            o.weak = kNone;
        }
    }

    // Every weak reference the model reaches, against its referent. A referent that
    // is reachable is still there. One that is not is gone after a full collection;
    // a young collection frees it only if it is young, which the model does not
    // track, so either answer is taken, but the same answer from every weak reference
    // to it (LRM 8.30.1), and an object that was kept is whole. A full collection
    // clears these and no others; a young one may also clear weak references it kept
    // that the model does not reach.
    void check_weak(bool full, uint64_t cleared_by_collector) {
        std::vector<int8_t> kept(objs.size(), -1);
        uint64_t            cleared = 0;
        for (const int32_t i : reach_list) {
            Obj&         o = objs[static_cast<size_t>(i)];
            const Shape& s = g_shapes[o.shape];
            if (!s.weak) {
                continue;
            }
            const uint64_t actual = load(o.handle, s.referent_offset());
            if (o.weak == kNone) {
                REQUIRE(actual == 0);
                continue;
            }
            const auto referent = static_cast<size_t>(o.weak);
            if (reach[referent] != 0) {
                REQUIRE(actual == reinterpret_cast<uint64_t>(handle_of(o.weak)));
                continue;
            }
            const int8_t now = actual != 0 ? 1 : 0;
            REQUIRE((kept[referent] == -1 || kept[referent] == now));
            kept[referent] = now;
            if (now == 0) {
                o.weak = kNone;
                cleared += 1;
                continue;
            }
            REQUIRE(!full);
            REQUIRE(actual == reinterpret_cast<uint64_t>(handle_of(o.weak)));
            check_object(o.weak);
        }
        REQUIRE((full ? cleared_by_collector == cleared : cleared_by_collector >= cleared));
        weak_cleared += cleared;
    }

    void run(uint32_t steps) {
        for (uint32_t n = 0; n < steps; ++n) {
            step();
            if (below(40) == 0) {
                // By policy, young or full.
                collect(static_cast<int32_t>(below(3)) - 1);
            }
        }
        // Everything goes, and a full collection must then find nothing.
        for (uint32_t k = 0; k < kRoots; ++k) {
            root_obj[k]   = kNone;
            root_words[k] = 0;
        }
        for (uint32_t k = 0; k < kFrame; ++k) {
            frame_obj[k]   = kNone;
            frame_words[k] = 0;
        }
        collect(RT_GC_KIND_FULL);
        rt_gc_stats st{};
        rt_gc_get_stats(&st);
        REQUIRE(st.objects_live == 0);
        REQUIRE(st.bytes_live == 0);
        REQUIRE(st.objects_allocated == objs.size());
    }
};

// One full collection per `full_every` collections, where 0 lets the collector choose
// and 1 makes every collection full.
void run_model(int32_t full_every) {
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        CAPTURE(seed);
        Model m(seed, full_every);
        m.run(4000);
        CHECK(m.collections_checked > 50u);
        CHECK(m.full_checked > 0u);
        CHECK(m.objs.size() > 1000u);
        CHECK(m.finalized.size() > 100u);
        CHECK(m.weak_cleared > 10u);
        CHECK(m.weak_retaken >= 5u);
    }
}

// A heap with one table of precise roots and nothing else, for claims about where
// objects are put and what the heap gives back. Nothing here polls, so a collection
// runs only where a test asks for one.
struct Heap {
    std::vector<uint64_t> words;
    std::vector<uint32_t> offsets;
    rt_gc_roots           table{};
    void*                 pend_head = nullptr;

    // The slots, and after them the card bytes (a word more than the cards need).
    explicit Heap(uint32_t slots, uint64_t heap_bytes_max = 0)
      : words(slots + (((8ull * slots - 1) >> RT_GC_CARD_SHIFT) + 1 + 7) / 8, 0)
      , offsets(slots) {
        for (Shape& s : g_shapes) {
            s.build();
        }
        rt_gc_config cfg{};
        cfg.nursery_bytes   = RT_GC_BLOCK_BYTES;
        cfg.heap_bytes_max  = heap_bytes_max;
        cfg.map_vptr_offset = rt_gc_map_vptr_offset();
        cfg.request_offset  = static_cast<int32_t>(rt_gc_mutator_request_offset());
        cfg.mutator_bytes   = static_cast<int32_t>(rt_gc_mutator_bytes());
        rt_gc_init(&cfg);
        rt_run_init(&pend_head);
        for (uint32_t k = 0; k < slots; ++k) {
            offsets[k] = 8 * k;
        }
        table.base     = words.data();
        table.nhandles = slots;
        table.cards    = 8 * slots;
        table.bytes    = 8 * slots + static_cast<uint32_t>(((8ull * slots - 1) >> RT_GC_CARD_SHIFT) + 1);
        table.offsets  = offsets.data();
        rt_gc_roots_add(&table);
    }

    ~Heap() { rt_gc_shutdown(); }

    // A fresh object of a shape without a finalizer, stamped with `stamp` and held by
    // root `slot`.
    void* make(uint32_t slot, uint32_t shape, uint64_t stamp) {
        Shape& s = g_shapes[shape];
        REQUIRE(!s.finalizable);
        void* const h = rt_gc_alloc(rt_gc_mutator_current(), s.map.size, &s.map, nullptr);
        REQUIRE(h != nullptr);
        REQUIRE(load(h, kStampOffset) == 0);  // a fresh body is zero, whatever was here before
        store(h, 0, reinterpret_cast<uint64_t>(s.vptr));
        store(h, kStampOffset, stamp);
        words[slot] = reinterpret_cast<uint64_t>(h);
        rt_gc_note_state_store(&words[slot], sizeof(uint64_t));
        return h;
    }

    rt_gc_stats collect(uint32_t kind) {
        rt_gc_collect_kind(RT_GC_REASON_EXPLICIT, kind);
        return stats();
    }

    static rt_gc_stats stats() {
        rt_gc_stats st{};
        rt_gc_get_stats(&st);
        return st;
    }
};

// Every other one of a run of small objects dies, leaving free space only in pieces
// the size of one object, each between two survivors. `kind` is the collection that
// finds them dead: a young one when the objects have never been collected, a full
// one when they are old.
void check_granule_reuse(uint32_t kind) {
    constexpr uint32_t kCount = 4096;
    constexpr uint32_t kSmall = 0;
    Heap               heap(kCount);
    std::vector<void*> first(kCount);
    for (uint32_t k = 0; k < kCount; ++k) {
        first[k] = heap.make(k, kSmall, k);
    }
    if (kind == RT_GC_KIND_FULL) {
        heap.collect(RT_GC_KIND_FULL);
    }
    std::vector<void*> freed;
    for (uint32_t k = 1; k < kCount; k += 2) {
        heap.words[k] = 0;
        freed.push_back(first[k]);
    }
    const rt_gc_stats after = heap.collect(kind);
    CHECK(after.objects_live == kCount / 2);
    for (uint32_t k = 0; k < kCount; ++k) {
        REQUIRE((rt_gc_find_object(first[k]) == first[k]) == (k % 2 == 0));
    }
    // As many again: each lands where a dead one was, so the heap takes no new block.
    std::vector<void*> second;
    for (uint32_t k = 1; k < kCount; k += 2) {
        second.push_back(heap.make(k, kSmall, kCount + k));
    }
    std::sort(freed.begin(), freed.end());
    std::sort(second.begin(), second.end());
    CHECK(second == freed);
    const rt_gc_stats reused = Heap::stats();
    CHECK(reused.blocks_total == after.blocks_total);
    CHECK(reused.blocks_free == after.blocks_free);
    for (uint32_t k = 0; k < kCount; ++k) {
        void* const h = reinterpret_cast<void*>(heap.words[k]);
        REQUIRE(rt_gc_find_object(h) == h);
        REQUIRE(load(h, kStampOffset) == (k % 2 == 0 ? k : kCount + k));
    }
}

} // namespace

TEST_CASE("gc model: the collector keeps exactly what a model of the heap says") {
    SUBCASE("the collector chooses young or full") { run_model(0); }
    SUBCASE("every collection is full") { run_model(1); }
    SUBCASE("one collection in four is full") { run_model(4); }
}

TEST_CASE("gc heap: a freed object's granules are the next allocation's") {
    SUBCASE("freed by a young collection") { check_granule_reuse(RT_GC_KIND_YOUNG); }
    SUBCASE("freed by a full collection") { check_granule_reuse(RT_GC_KIND_FULL); }
}

// More cards stored to than the log holds: the next young collection reads every slot instead, so a
// store the log dropped is not lost, and every card is clear after it.
TEST_CASE("gc heap: a young collection after the card log overflows reads every slot") {
    constexpr uint32_t kSlots = 512; // eight cards of 64 slots
    constexpr uint32_t kSmall = 0;
    Heap               heap(kSlots);
    heap.collect(RT_GC_KIND_FULL);
    rt_gc_set_logged_cards_max(3);
    std::vector<void*> made;
    for (uint32_t card = 0; card < 8; ++card) {
        made.push_back(heap.make(card * 64, kSmall, card));
    }
    const rt_gc_stats overflowed = heap.collect(RT_GC_KIND_YOUNG);
    CHECK(overflowed.state_slots_read == kSlots);
    CHECK(overflowed.objects_live == 8);
    for (uint32_t card = 0; card < 8; ++card) {
        REQUIRE(rt_gc_find_object(made[card]) == made[card]);
        CHECK(load(made[card], kStampOffset) == card);
    }
    CHECK(heap.collect(RT_GC_KIND_YOUNG).state_slots_read == 0);
}

TEST_CASE("gc heap: an object larger than a block has a mapping of its own") {
    constexpr uint32_t kLarge = 7;
    Heap               heap(2);
    const uint64_t     size = g_shapes[kLarge].map.size;
    REQUIRE(size > RT_GC_BLOCK_BYTES);
    const rt_gc_stats empty = Heap::stats();
    uint8_t* const    h     = static_cast<uint8_t*>(heap.make(0, kLarge, 11));
    const rt_gc_stats held  = Heap::stats();
    CHECK(held.blocks_total == empty.blocks_total);
    CHECK(held.heap_bytes >= empty.heap_bytes + size);
    CHECK(reinterpret_cast<uintptr_t>(h) % RT_GC_ALIGN == 0);
    // Every byte of it resolves to it, and the bytes around it to nothing.
    CHECK(rt_gc_find_object(h) == h);
    CHECK(rt_gc_find_object(h + size / 2) == h);
    CHECK(rt_gc_find_object(h + size - 1) == h);
    CHECK(rt_gc_find_object(h + size) == nullptr);
    CHECK(rt_gc_find_object(h - 1) == nullptr);

    heap.collect(RT_GC_KIND_YOUNG);
    heap.collect(RT_GC_KIND_FULL);
    CHECK(rt_gc_find_object(h) == h);
    CHECK(load(h, kStampOffset) == 11);
    // Old and unreachable: a young collection leaves it, a full one frees it and,
    // nothing of its size having been allocated since the last collection, its
    // mapping goes back.
    heap.words[0] = 0;
    CHECK(heap.collect(RT_GC_KIND_YOUNG).heap_bytes == held.heap_bytes);
    CHECK(rt_gc_find_object(h) == h);
    CHECK(heap.collect(RT_GC_KIND_FULL).heap_bytes == empty.heap_bytes);
    CHECK(rt_gc_find_object(h) == nullptr);
    // Young and unreachable: a young collection is enough to free the object.
    uint8_t* const y = static_cast<uint8_t*>(heap.make(1, kLarge, 12));
    heap.words[1]    = 0;
    heap.collect(RT_GC_KIND_YOUNG);
    CHECK(rt_gc_find_object(y) == nullptr);
    CHECK(rt_gc_find_object(y + size - 1) == nullptr);
}

TEST_CASE("gc heap: a dead large object's mapping is kept for the next of its size") {
    constexpr uint32_t kLarge = 7;
    constexpr uint32_t kFirst = 16;
    constexpr uint32_t kNext  = 4;
    Heap               heap(kFirst);
    const rt_gc_stats  empty = Heap::stats();
    std::vector<void*> first(kFirst);
    for (uint32_t k = 0; k < kFirst; ++k) {
        first[k] = heap.make(k, kLarge, 1 + k);
    }
    const rt_gc_stats held    = Heap::stats();
    const uint64_t    mapping = (held.heap_bytes - empty.heap_bytes) / kFirst;
    REQUIRE(mapping > g_shapes[kLarge].map.size);
    // A young collection frees the objects and keeps their mappings.
    std::fill(heap.words.begin(), heap.words.end(), 0);
    const rt_gc_stats freed = heap.collect(RT_GC_KIND_YOUNG);
    CHECK(freed.objects_live == 0);
    CHECK(freed.heap_bytes == held.heap_bytes);
    for (uint32_t k = 0; k < kFirst; ++k) {
        REQUIRE(rt_gc_find_object(first[k]) == nullptr);
    }
    // The next objects of that size are put where dead ones were, and map nothing.
    std::sort(first.begin(), first.end());
    std::vector<void*> next(kNext);
    for (uint32_t k = 0; k < kNext; ++k) {
        next[k] = heap.make(k, kLarge, 100 + k);
        CHECK(std::binary_search(first.begin(), first.end(), next[k]));
        CHECK(rt_gc_find_object(next[k]) == next[k]);
    }
    CHECK(Heap::stats().heap_bytes == held.heap_bytes);
    // A full collection keeps as many as were allocated since the last collection,
    // which is what the next cycle will want, and returns the rest.
    std::fill(heap.words.begin(), heap.words.end(), 0);
    CHECK(heap.collect(RT_GC_KIND_FULL).heap_bytes == empty.heap_bytes + kNext * mapping);
    // After a cycle that allocated none, that is none.
    CHECK(heap.collect(RT_GC_KIND_FULL).heap_bytes == empty.heap_bytes);
    for (uint32_t k = 0; k < kNext; ++k) {
        CHECK(rt_gc_find_object(next[k]) == nullptr);
    }
}

TEST_CASE("gc heap: a kept mapping is given up before the ceiling refuses an object") {
    constexpr uint32_t kSmall  = 0;
    constexpr uint32_t kLarge  = 7;
    constexpr uint32_t kLarger = 9;
    // What the heap holds once it has a chunk, measured rather than assumed.
    uint64_t base = 0;
    {
        Heap probe(1);
        probe.make(0, kSmall, 1);
        base = Heap::stats().heap_bytes;
    }
    REQUIRE(base >= RT_GC_CHUNK_BYTES);
    // A ceiling with room for either large object beside that chunk, and not for both.
    const uint64_t larger = RT_GC_LARGE_PAYLOAD + g_shapes[kLarger].map.size;
    REQUIRE(g_shapes[kLarger].map.size > g_shapes[kLarge].map.size);
    Heap heap(3, base + larger);
    heap.make(0, kSmall, 1);
    heap.make(1, kLarge, 2);
    heap.words[1] = 0;
    // The dead object's mapping is kept, and still counts against the ceiling.
    CHECK(heap.collect(RT_GC_KIND_YOUNG).heap_bytes > base);
    // An object of another size cannot use it and does not fit beside it. Nothing here
    // can collect, so the heap makes room by giving the kept mapping back.
    void* const big = heap.make(2, kLarger, 3);
    CHECK(rt_gc_find_object(big) == big);
    CHECK(Heap::stats().heap_bytes == base + larger);
}

TEST_CASE("gc heap: empty chunks go back to the system after a full collection") {
    constexpr uint32_t kMedium = 5;
    constexpr uint32_t kCount  = 7000;
    Heap               heap(2);
    void* const        kept = heap.make(0, kMedium, 7);
    for (uint32_t n = 0; n < kCount; ++n) {
        heap.make(1, kMedium, n);
    }
    const rt_gc_stats grown = Heap::stats();
    REQUIRE(grown.heap_bytes >= 4ull * RT_GC_CHUNK_BYTES);
    // A young collection frees the objects and keeps the chunks.
    const rt_gc_stats young = heap.collect(RT_GC_KIND_YOUNG);
    CHECK(young.objects_live == 2);
    CHECK(young.blocks_free > grown.blocks_free);
    CHECK(young.heap_bytes == grown.heap_bytes);
    // A full one, with nothing allocated since, keeps only the chunks its two objects
    // are in.
    const rt_gc_stats full = heap.collect(RT_GC_KIND_FULL);
    CHECK(full.objects_live == 2);
    CHECK(full.heap_bytes <= 3ull * RT_GC_CHUNK_BYTES);
    CHECK(full.blocks_free < young.blocks_free);
    CHECK(rt_gc_find_object(kept) == kept);
    CHECK(load(kept, kStampOffset) == 7);
    // The heap grows again when asked, and a full collection right after the burst
    // keeps the chunks the burst used, which the next burst will want.
    for (uint32_t n = 0; n < kCount; ++n) {
        heap.make(1, kMedium, n);
    }
    const rt_gc_stats regrown = Heap::stats();
    CHECK(regrown.heap_bytes >= grown.heap_bytes);
    CHECK(heap.collect(RT_GC_KIND_FULL).heap_bytes == regrown.heap_bytes);
    CHECK(heap.collect(RT_GC_KIND_FULL).heap_bytes <= 3ull * RT_GC_CHUNK_BYTES);
}

// A weak reference with a notifier: the collector pends the notifier when it clears
// the referent and at no other time. The chain is the one `->>` pends events on,
// which the NBA region drains; here the test does.
TEST_CASE("gc: the collector pends the notifier of a weak reference it clears") {
    constexpr uint32_t kSmall = 0;
    Heap               heap(4);
    const Shape&       shape    = g_shapes[kNotifiedShape];
    void* const        referent = heap.make(0, kSmall, 1);
    void* const        notifier = heap.make(1, kEventShape, 0);
    void* const        weak     = heap.make(2, kNotifiedShape, 3);
    store(weak, shape.referent_offset(), reinterpret_cast<uint64_t>(referent));
    store(weak, kFieldsOffset, reinterpret_cast<uint64_t>(notifier));
    // The weak reference alone holds its notifier, as a class object holds a
    // property's.
    heap.words[1]       = 0;
    auto* const event   = static_cast<rt_event*>(notifier);
    const auto  drained = [&] {
        REQUIRE(heap.pend_head == notifier);
        REQUIRE(event->pending == 1);
        REQUIRE(event->next_pend == nullptr);
        heap.pend_head = nullptr;
        event->pending = 0;
    };

    // Held: nothing is cleared and nothing is pended.
    CHECK(heap.collect(RT_GC_KIND_YOUNG).weak_cleared == 0);
    CHECK(heap.collect(RT_GC_KIND_FULL).weak_cleared == 0);
    CHECK(load(weak, shape.referent_offset()) == reinterpret_cast<uint64_t>(referent));
    CHECK(heap.pend_head == nullptr);

    // Dropped, and old by now: a young collection does not free it, so the weak
    // reference still names it.
    heap.words[0] = 0;
    CHECK(heap.collect(RT_GC_KIND_YOUNG).weak_cleared == 0);
    CHECK(load(weak, shape.referent_offset()) == reinterpret_cast<uint64_t>(referent));
    CHECK(rt_gc_find_object(referent) == referent);
    CHECK(heap.pend_head == nullptr);

    // Nor does a full collection run from inside an allocation, which keeps every
    // referent: weak references are cleared at the scheduler's safepoint only.
    rt_gc_collect_kind(RT_GC_REASON_EMERGENCY, RT_GC_KIND_FULL);
    CHECK(Heap::stats().weak_cleared == 0);
    CHECK(load(weak, shape.referent_offset()) == reinterpret_cast<uint64_t>(referent));
    CHECK(rt_gc_find_object(referent) == referent);
    CHECK(load(referent, kStampOffset) == 1);
    CHECK(heap.pend_head == nullptr);

    // A full collection at the safepoint finds it unreachable.
    CHECK(heap.collect(RT_GC_KIND_FULL).weak_cleared == 1);
    CHECK(load(weak, shape.referent_offset()) == 0);
    CHECK(rt_gc_find_object(referent) == nullptr);
    drained();
    CHECK(heap.collect(RT_GC_KIND_FULL).weak_cleared == 1);
    CHECK(heap.pend_head == nullptr);

    // A young weak reference to a young referent that nothing holds: the young
    // collection clears it. Its notifier is the first one's.
    void* const second    = heap.make(3, kNotifiedShape, 4);
    void* const referent2 = heap.make(0, kSmall, 5);
    store(second, shape.referent_offset(), reinterpret_cast<uint64_t>(referent2));
    store(second, kFieldsOffset, reinterpret_cast<uint64_t>(notifier));
    heap.words[0] = 0;
    CHECK(heap.collect(RT_GC_KIND_YOUNG).weak_cleared == 2);
    CHECK(load(second, shape.referent_offset()) == 0);
    CHECK(rt_gc_find_object(referent2) == nullptr);
    drained();
}
