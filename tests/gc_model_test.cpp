#include <doctest/doctest.h>

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
struct Shape {
    uint32_t nhandles;
    uint32_t filler_words;
    bool     finalizable;

    rt_gc_map   map{};
    uint32_t    offsets[kFieldsMax]{};
    const void* vptr = nullptr;
    alignas(void*) uint8_t desc[128]{};

    uint64_t body_bytes() const { return kFieldsOffset + 8ull * nhandles + 8ull * filler_words; }

    void build() {
        for (uint32_t k = 0; k < nhandles; ++k) {
            offsets[k] = kFieldsOffset + 8 * k;
        }
        // The one place this file knows how an object is laid out beside its body.
        map.size     = (RT_GC_HEADER_BYTES + body_bytes() + RT_GC_ALIGN - 1) / RT_GC_ALIGN * RT_GC_ALIGN;
        map.nhandles = nhandles;
        map.nleaves  = 0;
        map.handles  = nhandles != 0 ? offsets : nullptr;
        map.leaves   = nullptr;
        map.finalize = finalizable ? on_finalize : nullptr;
        const auto            below = static_cast<size_t>(-rt_gc_map_vptr_offset());
        const rt_gc_map* const m     = &map;
        REQUIRE(below + sizeof(void*) <= sizeof desc);
        std::memcpy(desc, &m, sizeof m);
        vptr = desc + below;
    }
};

// From one grain to larger than a block. The last two are above the large-object
// threshold, and the last does not fit in a block at all.
Shape g_shapes[] = {
    { 0, 0, false },   { 1, 1, false },   { 2, 0, true },     { 4, 3, false },
    { 3, 40, true },   { 1, 300, false }, { 2, 1500, true },  { 0, 5000, false },
};
constexpr uint32_t kShapes = sizeof g_shapes / sizeof g_shapes[0];

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
    };

    std::vector<Obj>       objs;
    std::vector<Finalized> finalized;
    size_t                 finalized_seen = 0;
    uint64_t               last_number    = 0;

    // The precise roots: a table the collector reads in full at every collection.
    uint64_t    root_words[kRoots]{};
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
        // Small shapes mostly; a large object now and then.
        const uint32_t shape = below(16) == 0 ? 6 + below(2) : below(6);
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
        for (uint32_t w = 0; w < s.filler_words; ++w) {
            store(h, kFieldsOffset + 8ull * s.nhandles + 8ull * w, mix(index * 0x100 + w));
        }
        Obj o{ h, shape, { kNone, kNone, kNone, kNone }, false, 0 };
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
        } else if (op < 92) {
            attach(pick_reachable());
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
        for (uint32_t w = 0; w < s.filler_words; ++w) {
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
    }
}

} // namespace

TEST_CASE("gc model: the collector keeps exactly what a model of the heap says") {
    SUBCASE("the collector chooses young or full") { run_model(0); }
    SUBCASE("every collection is full") { run_model(1); }
    SUBCASE("one collection in four is full") { run_model(4); }
}
