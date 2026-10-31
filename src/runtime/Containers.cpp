// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Dynamic arrays, queues and associative arrays.

#include "RuntimeInternal.h"

using namespace rt_internal;

namespace {

// -- Container element lifecycle (rt_elem_ops) --
//
// Generic recursion over the element descriptor. POD short-circuits to raw
// byte moves, so the descriptor adds no cost to the plane-scalar fast paths.
// Internal to the runtime — the backend inlines the element ops at call sites
// and never emits a call to these, so they carry no C linkage.

void rt_elem_init(const rt_elem_ops* o, void* p) {
    if (o->kind == rt_elem_event) {
        // LRM Table 6-7: an event's default value is a NEW event, so every element
        // slot gets its own object. This is the one kind whose power-on image is
        // not a constant, which is why init cannot be a blind copy of `proto`.
        auto h = rt_event_new();
        std::memcpy(p, &h, sizeof h);
        return;
    }
    std::memcpy(p, o->proto, static_cast<size_t>(o->stride));
    if (o->kind == rt_elem_aggregate) {
        // The proto image covers every leaf whose power-on value is a constant; an
        // event leaf is the one that is not, so construct the leaves afterwards.
        // Re-initialising a string / container leaf here is a no-op copy of the same
        // bytes the proto already supplied.
        for (uint32_t i = 0; i < o->nleaves; ++i) {
            rt_elem_init(o->leaves[i].ops, static_cast<uint8_t*>(p) + o->leaves[i].offset);
        }
    }
}

void rt_elem_deinit(const rt_elem_ops* o, void* p) {
    switch (o->kind) {
        case rt_elem_pod:
            return;
        case rt_elem_event:
            // Nothing to release: event objects are never freed (LRM 15.5.5.2's
            // reclamation is GC, which Clotho does not have).
            return;
        case rt_elem_string:
            rt_string_deinit(static_cast<rt_string*>(p));
            return;
        case rt_elem_dynarr:
            rt_dynarr_delete(static_cast<rt_dynarr*>(p), o->inner);
            return;
        case rt_elem_queue:
            rt_queue_delete_all(static_cast<rt_queue*>(p), o->inner);
            return;
        case rt_elem_assoc:
            rt_assoc_reset(static_cast<rt_assoc*>(p), o->inner);
            return;
        case rt_elem_aggregate:
            for (uint32_t i = 0; i < o->nleaves; ++i) {
                rt_elem_deinit(o->leaves[i].ops,
                               static_cast<uint8_t*>(p) + o->leaves[i].offset);
            }
            return;
        default:
            return;
    }
}

void rt_elem_copy(const rt_elem_ops* o, void* dst, const void* src, const rt_alloc_site* site) {
    switch (o->kind) {
        case rt_elem_pod:
            std::memcpy(dst, src, static_cast<size_t>(o->stride));
            return;
        case rt_elem_event:
            // Copying an event copies the HANDLE, so both elements name one sync
            // object -- the merge LRM 15.5.5 specifies for `a = b`.
            std::memcpy(dst, src, static_cast<size_t>(o->stride));
            return;
        case rt_elem_string:
            rt_string_assign(static_cast<rt_string*>(dst), static_cast<const rt_string*>(src),
                             site);
            return;
        case rt_elem_dynarr:
            rt_dynarr_assign(static_cast<rt_dynarr*>(dst), static_cast<const rt_dynarr*>(src),
                             o->inner, site);
            return;
        case rt_elem_queue:
            rt_queue_assign(static_cast<rt_queue*>(dst), static_cast<const rt_queue*>(src),
                            o->inner, site);
            return;
        case rt_elem_assoc:
            rt_assoc_assign(static_cast<rt_assoc*>(dst), static_cast<const rt_assoc*>(src),
                            o->inner, site);
            return;
        case rt_elem_aggregate:
            // Deep-assign every heap leaf (dst's owned buffers are reused per
            // leaf), then raw-copy the POD gaps between the leaf slots. The gap
            // walk relies on `leaves` being offset-sorted (backend invariant).
            for (uint32_t i = 0; i < o->nleaves; ++i) {
                rt_elem_copy(o->leaves[i].ops,
                             static_cast<uint8_t*>(dst) + o->leaves[i].offset,
                             static_cast<const uint8_t*>(src) + o->leaves[i].offset, site);
            }
            {
                uint64_t pos = 0;
                for (uint32_t i = 0; i <= o->nleaves; ++i) {
                    const uint64_t end =
                        i < o->nleaves ? o->leaves[i].offset : o->stride;
                    if (end > pos) {
                        std::memcpy(static_cast<uint8_t*>(dst) + pos,
                                    static_cast<const uint8_t*>(src) + pos,
                                    static_cast<size_t>(end - pos));
                    }
                    if (i < o->nleaves) {
                        pos = o->leaves[i].offset + o->leaves[i].ops->stride;
                    }
                }
            }
            return;
        default:
            return;
    }
}

// Whether elements need per-element construction / destruction (anything but
// raw bytes).
bool elem_managed(const rt_elem_ops* o) {
    return o->kind != rt_elem_pod;
}

// Whether an element's power-on image is all zero bytes. True of every 2-state
// element; false of a 4-state one (its x image sets the may plane) and of a
// string (an empty inline string records its remaining capacity). A zero image
// is precisely what calloc returns, so a caller filling `count` of them can
// allocate zeroed instead and skip the fill -- which for a large array means
// the pages arrive on first touch rather than all at once.
bool proto_is_zero(const rt_elem_ops* o) {
    const auto* p = static_cast<const uint8_t*>(o->proto);
    for (uint64_t i = 0; i < o->stride; ++i) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

// -- Dynamic arrays (LRM 7.5) --

extern "C" void rt_dynarr_new(rt_dynarr* h, const rt_elem_ops* ops, int64_t n,
                              const rt_alloc_site* site) {
    if (n < 0) {
        rt_alloc_negative(n, site, rt_diag_now());
    }
    rt_dynarr_delete(h, ops);
    if (n == 0) {
        return;
    }
    const uint64_t count  = static_cast<uint64_t>(n);
    const uint64_t stride = ops->stride;
    // Scanning the prototype costs `stride` bytes against a fill of
    // `count * stride`, so it is worth it for every array but a singleton.
    if (!elem_managed(ops) && count > 1 && proto_is_zero(ops)) {
        h->data = rt_zalloc_elems_checked(count, stride, site, rt_diag_now());
        h->len  = count;
        return;
    }
    auto* buf = static_cast<uint8_t*>(rt_alloc_elems_checked(count, stride, site, rt_diag_now()));
    for (uint64_t i = 0; i < count; ++i) {
        // rt_elem_init, not a memcpy of the prototype: a managed element may need
        // per-slot construction (an event element binds its own object).
        rt_elem_init(ops, buf + i * stride);
    }
    h->data = buf;
    h->len  = count;
}

extern "C" void rt_dynarr_new_copy(rt_dynarr* h, const rt_elem_ops* ops, int64_t n,
                                   const rt_dynarr* src, const rt_alloc_site* site) {
    if (n < 0) {
        rt_alloc_negative(n, site, rt_diag_now());
    }
    // Build the new buffer before destroying `h`'s old one: a self-resize
    // (`a = new[n](a)`) aliases src == h, so the old elements must stay live
    // through the deep copy.
    const uint64_t count  = static_cast<uint64_t>(n);
    const uint64_t stride = ops->stride;
    const uint64_t copy   = src->len < count ? src->len : count;
    // A grow-resize of plain elements fills the tail with the prototype; when
    // that image is zero the allocator can supply it, which for a large grow is
    // the difference between touching every new page and touching none.
    const bool zeroed = !elem_managed(ops) && count > copy + 1 && proto_is_zero(ops);
    uint8_t* buf = static_cast<uint8_t*>(
        zeroed ? rt_zalloc_elems_checked(count, stride, site, rt_diag_now())
               : rt_alloc_elems_checked(count, stride, site, rt_diag_now()));
    if (!elem_managed(ops)) {
        if (copy != 0) {
            std::memcpy(buf, src->data, static_cast<size_t>(copy * stride));
        }
        if (!zeroed) {
            for (uint64_t i = copy; i < count; ++i) {
                std::memcpy(buf + i * stride, ops->proto, static_cast<size_t>(stride));
            }
        }
    } else {
        const auto* sdata = static_cast<const uint8_t*>(src->data);
        for (uint64_t i = 0; i < count; ++i) {
            rt_elem_init(ops, buf + i * stride);
            if (i < copy) {
                rt_elem_copy(ops, buf + i * stride, sdata + i * stride, site);
            }
        }
    }
    rt_dynarr_delete(h, ops);
    h->data = buf;
    h->len  = count;
}

extern "C" void rt_dynarr_delete(rt_dynarr* h, const rt_elem_ops* ops) {
    if (elem_managed(ops)) {
        auto* data = static_cast<uint8_t*>(h->data);
        for (uint64_t i = 0; i < h->len; ++i) {
            rt_elem_deinit(ops, data + i * ops->stride);
        }
    }
    std::free(h->data);
    h->data = nullptr;
    h->len  = 0;
    // Advanced, never zeroed, so an epoch is never reused within a run. new,
    // new_copy and assign each funnel through here exactly once, which is where
    // their single bump comes from.
    ++h->epoch;
}

extern "C" void rt_dynarr_assign(rt_dynarr* dst, const rt_dynarr* src, const rt_elem_ops* ops,
                                 const rt_alloc_site* site) {
    if (dst == src) {
        return;
    }
    rt_dynarr_delete(dst, ops);
    if (src->len == 0) {
        return;
    }
    const uint64_t stride = ops->stride;
    auto* buf = static_cast<uint8_t*>(
        rt_alloc_elems_checked(src->len, stride, site, rt_diag_now()));
    if (!elem_managed(ops)) {
        std::memcpy(buf, src->data, static_cast<size_t>(src->len * stride));
    } else {
        const auto* sdata = static_cast<const uint8_t*>(src->data);
        for (uint64_t i = 0; i < src->len; ++i) {
            rt_elem_init(ops, buf + i * stride);
            rt_elem_copy(ops, buf + i * stride, sdata + i * stride, site);
        }
    }
    dst->data = buf;
    dst->len  = src->len;
}

// Move `src`'s buffer into `dst` (ownership transfer): destroy `dst`'s current
// elements + buffer, take `src`'s, and leave `src` the empty array. Used by the
// nonblocking-assignment commit -- the RHS was deep-copied into `src` in the
// Active region, so the NBA region just hands that buffer to `dst` with no
// second copy. A no-op when dst == src.
extern "C" void rt_dynarr_move(rt_dynarr* dst, rt_dynarr* src, const rt_elem_ops* ops) {
    if (dst == src) {
        return;
    }
    // dst's epoch advances inside rt_dynarr_delete, which is the single bump
    // this move owes it; src's is bumped here because it is left empty.
    rt_dynarr_delete(dst, ops);
    dst->data = src->data;
    dst->len  = src->len;
    src->data = nullptr;
    src->len  = 0;
    ++src->epoch;
}
namespace {

// -- Array ordering methods (LRM 7.12.2) --
//
// Element-agnostic reverse and value-keyed sort over `stride`-byte slots, shared
// by the dynamic-array and queue entry points. reverse() moves whole slots and
// works for any element; sort()/rsort() key on a scalar element value, restricted
// by the frontend to a 2-state integral (width <= 64) or real element.

// Sort-key kind for the ordering comparator; mirrors the backend descriptor
// (ir/llvm/Containers.cpp dynarr_sort_desc). The 4-state kinds are separate rather than
// a flag on the first two so that a 2-state element never tests a plane it does
// not have: the comparator is chosen once per sort, outside the compare loop.
enum : uint32_t {
    sort_unsigned  = 0,
    sort_signed    = 1,
    sort_real      = 2,
    sort_unsigned4 = 3,
    sort_signed4   = 4
};

// Swap two `stride`-byte element slots through a caller-provided scratch slot.
void swap_slots(uint8_t* a, uint8_t* b, uint64_t stride, uint8_t* tmp) {
    std::memcpy(tmp, a, static_cast<size_t>(stride));
    std::memcpy(a, b, static_cast<size_t>(stride));
    std::memcpy(b, tmp, static_cast<size_t>(stride));
}

// Reverse `n` contiguous `stride`-byte slots in place.
void reverse_run(uint8_t* data, uint64_t n, uint64_t stride) {
    if (n < 2) {
        return;
    }
    std::vector<uint8_t> tmp(static_cast<size_t>(stride));
    for (uint64_t i = 0, j = n - 1; i < j; ++i, --j) {
        swap_slots(data + i * stride, data + j * stride, stride, tmp.data());
    }
}

// The low min(stride, 8) bytes of a slot as a raw 64-bit word (little-endian).
uint64_t slot_word(const uint8_t* p, uint64_t stride) {
    uint64_t v = 0;
    std::memcpy(&v, p, stride < 8 ? static_cast<size_t>(stride) : 8);
    return v;
}

// Ascending `a < b` over one element slot for the element's compare kind.
bool slot_less(const uint8_t* a, const uint8_t* b, uint64_t stride, uint32_t kind, uint32_t width) {
    if (kind == sort_real) {
        double da = 0.0;
        double db = 0.0;
        if (width == 32) {
            // A shortreal slot holds a float; reading it as a double compared the
            // element with its neighbour's bits.
            float fa = 0.0F;
            float fb = 0.0F;
            std::memcpy(&fa, a, sizeof(float));
            std::memcpy(&fb, b, sizeof(float));
            da = fa;
            db = fb;
        } else {
            std::memcpy(&da, a, sizeof(double));
            std::memcpy(&db, b, sizeof(double));
        }
        // `<` is false whenever an operand is NaN, so a NaN is equivalent to every
        // number and the relation is not a strict weak order: std::sort over it is
        // undefined and mis-orders the numbers around a NaN. NaN is placed after
        // every number, all NaNs equal (docs/LRM_NOTES.md, 7.12.2); rsort reverses
        // the ascending result, so it puts NaN first.
        return !std::isnan(da) && (std::isnan(db) || da < db);
    }
    uint64_t ua = slot_word(a, stride);
    uint64_t ub = slot_word(b, stride);
    if (width < 64) {
        const uint64_t mask = (uint64_t{ 1 } << width) - 1;
        ua &= mask;
        ub &= mask;
    }
    if (kind == sort_signed) {
        // Sign-extend the width-bit value so the compare is arithmetic.
        const uint64_t sbit = uint64_t{ 1 } << (width - 1);
        return static_cast<int64_t>((ua ^ sbit) - sbit) < static_cast<int64_t>((ub ^ sbit) - sbit);
    }
    return ua < ub;
}

// Whether `kind` orders 4-state slots (must/may planes) rather than flat values.
bool sort_kind_four(uint32_t kind) {
    return kind == sort_unsigned4 || kind == sort_signed4;
}

// One 4-state element slot decoded: the definite value (the must plane) and the
// mask of bits that are x or z. A 4-state slot is `{ i(2W) must|may, iW z }` with
// must in bits [0,W) and may in [W,2W) (ir/llvm/Backend.h), so `must ^ may` is
// the unknown mask -- the same decode rt_container_eq does, and the same bounded
// (2W+7)/8-byte read inside the mm field. The z plane is never read: Z is encoded
// as X in must/may with the z bit set alongside, and the ordering does not
// distinguish x from z.
struct FourSlot {
    uint64_t must;
    uint64_t unknown;
};
FourSlot slot_four(const uint8_t* p, uint32_t width) {
    const uint64_t    mask = width < 64 ? (uint64_t{ 1 } << width) - 1 : ~uint64_t{ 0 };
    unsigned __int128 f    = 0;
    std::memcpy(&f, p, static_cast<size_t>((2 * width + 7) / 8));
    const uint64_t must = static_cast<uint64_t>(f) & mask;
    return { must, must ^ (static_cast<uint64_t>(f >> width) & mask) };
}

// Ascending three-way compare of two 4-state slots under clotho's total order
// (docs/design/array-methods.md): a slot carrying ANY x or z bit is unknown, all
// unknowns are mutually equal, and an unknown precedes every defined value.
// Defined values compare numerically at the element's signedness. Returns < 0,
// 0 or > 0. LRM 11.4.4's own answer -- x -- is not an ordering at all, so it
// cannot be what a comparison sort consumes.
int slot_cmp4(const uint8_t* a, const uint8_t* b, uint32_t kind, uint32_t width) {
    const FourSlot fa = slot_four(a, width);
    const FourSlot fb = slot_four(b, width);
    const bool     ua = fa.unknown != 0;
    const bool     ub = fb.unknown != 0;
    if (ua || ub) {
        return ua == ub ? 0 : (ua ? -1 : 1);
    }
    if (kind == sort_signed4) {
        // Sign-extend the width-bit value so the compare is arithmetic.
        const uint64_t sbit = uint64_t{ 1 } << (width - 1);
        const int64_t  va   = static_cast<int64_t>((fa.must ^ sbit) - sbit);
        const int64_t  vb   = static_cast<int64_t>((fb.must ^ sbit) - sbit);
        return va < vb ? -1 : (vb < va ? 1 : 0);
    }
    return fa.must < fb.must ? -1 : (fb.must < fa.must ? 1 : 0);
}

// Sort `n` contiguous slots ascending; `rev` then flips to descending (rsort).
//
// This is the flat-value path only: the caller reached it because the element IS
// the sort key (no `with` clause, and a 2-state integral of width <= 64 or a
// real). Two invariants follow from that and hold ONLY here:
//
//   * Ties are unobservable -- equal keys mean byte-equal elements -- so an
//     unstable std::sort suffices (LRM 7.12.2 does not require stability).
//   * Ascending-then-reverse IS a descending sort, because reversing a tie group
//     of indistinguishable elements changes nothing.
//
// A `with` clause breaks both: two DIFFERENT elements can share a key, so tie
// order becomes observable and reversing an ascending sort would reverse each tie
// group. That form is therefore not lowered here at all -- it is a stable merge
// sort emitted as IR, which orders descending directly (lower_array_sort_iter,
// src/lower/Sort.cpp). A 4-STATE element breaks both the same way and has its own
// driver below; this function and its comparator are reached only by the two
// kinds above, and pay nothing for the existence of the other path.
void sort_run(uint8_t* data, uint64_t n, uint64_t stride, uint32_t kind, uint32_t width, uint32_t rev) {
    if (n < 2) {
        return;
    }
    std::vector<uint64_t> order(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](uint64_t x, uint64_t y) {
        return slot_less(data + x * stride, data + y * stride, stride, kind, width);
    });
    std::vector<uint8_t> tmp(static_cast<size_t>(n * stride));
    for (uint64_t i = 0; i < n; ++i) {
        std::memcpy(tmp.data() + i * stride, data + order[i] * stride, static_cast<size_t>(stride));
    }
    std::memcpy(data, tmp.data(), static_cast<size_t>(n * stride));
    if (rev != 0) {
        reverse_run(data, n, stride);
    }
}

// Sort `n` contiguous 4-state slots under clotho's ordering for unknowns
// (slot_cmp4): `rev` orders descending. Neither of sort_run's two invariants
// survives the unknowns, so this is a separate driver rather than a flag on it:
//
//   * Ties ARE observable -- `8'b1111_111x` and `8'b0000_000x` compare equal and
//     are not byte-equal -- so the order must be stable. Tie-breaking on the
//     original index is exactly that, and costs no storage: the index vector is
//     already here.
//   * Ascending-then-reverse is NOT a descending sort, because every unknown is
//     one tie group and a reverse would flip it. So `rev` inverts the comparator
//     instead, which keeps the tie-break ascending in both directions.
void sort_run4(uint8_t* data, uint64_t n, uint64_t stride, uint32_t kind, uint32_t width, uint32_t rev) {
    if (n < 2) {
        return;
    }
    std::vector<uint64_t> order(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    const bool desc = rev != 0;
    std::sort(order.begin(), order.end(), [&](uint64_t x, uint64_t y) {
        const int c = slot_cmp4(data + x * stride, data + y * stride, kind, width);
        if (c != 0) {
            return desc ? c > 0 : c < 0;
        }
        return x < y;
    });
    std::vector<uint8_t> tmp(static_cast<size_t>(n * stride));
    for (uint64_t i = 0; i < n; ++i) {
        std::memcpy(tmp.data() + i * stride, data + order[i] * stride, static_cast<size_t>(stride));
    }
    std::memcpy(data, tmp.data(), static_cast<size_t>(n * stride));
}

} // namespace

// The three ordering entry points take the element descriptor rather than a
// bare stride: they reindex the container, so each bumps the relocation epoch,
// and `ops` keeps every container entry point on one signature shape. The
// reverse_run / sort_run* / swap_slots helpers above stay on a bare stride --
// they move slots and know nothing about epochs. Fewer than two elements is a
// genuine no-op, so it reorders nothing and leaves the epoch alone.
extern "C" void rt_dynarr_reverse(rt_dynarr* h, const rt_elem_ops* ops) {
    if (h->len < 2) {
        return;
    }
    reverse_run(static_cast<uint8_t*>(h->data), h->len, ops->stride);
    ++h->epoch;
}

extern "C" void rt_dynarr_sort(rt_dynarr* h, const rt_elem_ops* ops, uint32_t kind, uint32_t width,
                               uint32_t rev) {
    if (h->len < 2) {
        return;
    }
    // Which driver is decided by the element's domain, once per call, next to the
    // length test that was already here -- the comparators themselves stay
    // domain-specific and test nothing.
    if (sort_kind_four(kind)) {
        sort_run4(static_cast<uint8_t*>(h->data), h->len, ops->stride, kind, width, rev);
    } else {
        sort_run(static_cast<uint8_t*>(h->data), h->len, ops->stride, kind, width, rev);
    }
    ++h->epoch;
}

extern "C" void rt_dynarr_shuffle(rt_dynarr* h, const rt_elem_ops* ops, uint64_t* rng) {
    if (h->len < 2) {
        return;
    }
    const uint64_t       stride = ops->stride;
    auto*                data   = static_cast<uint8_t*>(h->data);
    std::vector<uint8_t> tmp(static_cast<size_t>(stride));
    // Fisher-Yates. The LRM does not fix the permutation (18.13-class
    // tool-private), only that it draw from the process RNG; a modulo index
    // reduction is sufficient.
    for (uint64_t i = h->len - 1; i > 0; --i) {
        const uint64_t j = xoshiro_next(rng) % (i + 1);
        swap_slots(data + i * stride, data + j * stride, stride, tmp.data());
    }
    ++h->epoch;
}
namespace {

// -- Queues (LRM 7.10) --
//
// A queue is a growable ring buffer: `base` holds `cap` element slots, the `len`
// live elements at physical positions (head + i) mod cap, computed by
// rt_queue_slot. All indexing is via q_elem; growth linearises to head == 0.

// Address of logical element `i` (0-based) in the ring. Requires cap > 0 and
// i < cap.
uint8_t* q_elem(const rt_queue* h, uint64_t stride, uint64_t i) {
    return static_cast<uint8_t*>(h->base) + rt_queue_slot(h, i) * stride;
}

// Ensure capacity for at least `need` elements, relocating the live elements to
// a contiguous head == 0 layout when a larger buffer is allocated. Geometric
// growth keeps repeated push amortised O(1).
void q_reserve(rt_queue* h, uint64_t stride, uint64_t need, const rt_alloc_site* site) {
    if (need <= h->cap) {
        return;
    }
    uint64_t newcap = h->cap < 4 ? 4 : h->cap * 2;
    if (newcap < need) {
        newcap = need;
    }
    auto* buf = static_cast<uint8_t*>(rt_alloc_elems_checked(newcap, stride, site, rt_diag_now()));
    for (uint64_t i = 0; i < h->len; ++i) {
        std::memcpy(buf + i * stride, q_elem(h, stride, i), static_cast<size_t>(stride));
    }
    std::free(h->base);
    h->base = buf;
    h->head = 0;
    h->cap  = newcap;
}

} // namespace

// Drop elements past the bounded-queue limit, destroying managed ones
// (LRM 7.10.4: keep the first `bound + 1` elements; bound == 0 is unbounded).
//
// Truncation is always from the TAIL, so the two push directions lose different
// elements when a full bounded queue overflows:
//   - push_front puts the new element at logical 0, so what gets dropped is the
//     old tail -- the end opposite the push.
//   - push_back puts the new element at logical len, so what gets dropped is the
//     element just pushed, and the push has no visible effect.
// Both fall out of "keep the first bound + 1"; neither direction is special-
// cased below. LRM 7.10.4 does not pin the choice (docs/LRM_NOTES.md, note 6).
static void q_apply_bound_ops(rt_queue* h, const rt_elem_ops* ops, uint64_t bound) {
    if (bound == 0 || h->len <= bound + 1) {
        return;
    }
    if (ops->kind != rt_elem_pod) {
        for (uint64_t i = bound + 1; i < h->len; ++i) {
            rt_elem_deinit(ops, q_elem(h, ops->stride, i));
        }
    }
    h->len = bound + 1;
}

// Append one element at the tail WITHOUT touching the epoch. The bump is left to
// the caller so that appending a whole range costs one epoch step instead of one
// per element: rt_queue_push_back bumps once, rt_queue_append_range bumps once
// for the range it appended.
static void q_push_back_raw(rt_queue* h, const rt_elem_ops* ops, uint64_t bound, const void* elem,
                            const rt_alloc_site* site) {
    const uint64_t stride = ops->stride;
    q_reserve(h, stride, h->len + 1, site);
    void* slot = q_elem(h, stride, h->len);
    if (ops->kind == rt_elem_pod) {
        std::memcpy(slot, elem, static_cast<size_t>(stride));
    } else {
        rt_elem_init(ops, slot);
        rt_elem_copy(ops, slot, elem, site);
    }
    h->len += 1;
    q_apply_bound_ops(h, ops, bound);
}

extern "C" void rt_queue_push_back(rt_queue* h, const rt_elem_ops* ops, uint64_t bound,
                                   const void* elem, const rt_alloc_site* site) {
    q_push_back_raw(h, ops, bound, elem, site);
    // Bumped even when the ring did not have to grow: `len` changed, so q[$]
    // names a different element than it did before.
    ++h->epoch;
}

extern "C" void rt_queue_push_front(rt_queue* h, const rt_elem_ops* ops, uint64_t bound,
                                    const void* elem, const rt_alloc_site* site) {
    const uint64_t stride = ops->stride;
    q_reserve(h, stride, h->len + 1, site);
    h->head = h->head == 0 ? h->cap - 1 : h->head - 1;
    void* slot = q_elem(h, stride, 0);
    if (ops->kind == rt_elem_pod) {
        std::memcpy(slot, elem, static_cast<size_t>(stride));
    } else {
        rt_elem_init(ops, slot);
        rt_elem_copy(ops, slot, elem, site);
    }
    h->len += 1;
    q_apply_bound_ops(h, ops, bound);
    // Every live element's logical index shifted up by one, so this reindexes
    // the whole queue even when the ring did not move.
    ++h->epoch;
}

extern "C" void rt_queue_pop_back(rt_queue* h, const rt_elem_ops* ops, void* out) {
    if (h->len == 0) {
        return;
    }
    // Relocation: the element's bytes (and heap ownership) move to `out`; the
    // vacated slot is not destroyed.
    h->len -= 1;
    std::memcpy(out, q_elem(h, ops->stride, h->len), static_cast<size_t>(ops->stride));
    ++h->epoch;
}

extern "C" void rt_queue_pop_front(rt_queue* h, const rt_elem_ops* ops, void* out) {
    if (h->len == 0) {
        return;
    }
    std::memcpy(out, q_elem(h, ops->stride, 0), static_cast<size_t>(ops->stride));
    h->head = h->head + 1 == h->cap ? 0 : h->head + 1;
    h->len -= 1;
    ++h->epoch;
}

extern "C" void rt_queue_insert(rt_queue* h, const rt_elem_ops* ops, uint64_t bound, uint64_t i,
                                const void* elem, const rt_alloc_site* site) {
    if (i > h->len) {
        return;
    }
    const uint64_t stride = ops->stride;
    q_reserve(h, stride, h->len + 1, site);
    // Shift [i, len) up by one to open a slot at i (relocation, raw memcpy).
    for (uint64_t j = h->len; j > i; --j) {
        std::memcpy(q_elem(h, stride, j), q_elem(h, stride, j - 1), static_cast<size_t>(stride));
    }
    void* slot = q_elem(h, stride, i);
    if (ops->kind == rt_elem_pod) {
        std::memcpy(slot, elem, static_cast<size_t>(stride));
    } else {
        rt_elem_init(ops, slot);
        rt_elem_copy(ops, slot, elem, site);
    }
    h->len += 1;
    q_apply_bound_ops(h, ops, bound);
    ++h->epoch;
}

extern "C" void rt_queue_delete_idx(rt_queue* h, const rt_elem_ops* ops, uint64_t i) {
    if (i >= h->len) {
        return;
    }
    const uint64_t stride = ops->stride;
    if (ops->kind != rt_elem_pod) {
        rt_elem_deinit(ops, q_elem(h, stride, i));
    }
    // Shift (i, len) down by one over the removed slot (relocation).
    for (uint64_t j = i; j + 1 < h->len; ++j) {
        std::memcpy(q_elem(h, stride, j), q_elem(h, stride, j + 1), static_cast<size_t>(stride));
    }
    h->len -= 1;
    ++h->epoch;
}

extern "C" void rt_queue_delete_all(rt_queue* h, const rt_elem_ops* ops) {
    if (ops->kind != rt_elem_pod) {
        for (uint64_t i = 0; i < h->len; ++i) {
            rt_elem_deinit(ops, q_elem(h, ops->stride, i));
        }
    }
    std::free(h->base);
    h->base = nullptr;
    h->head = 0;
    h->len  = 0;
    h->cap  = 0;
    // Advanced, never zeroed, so an epoch is never reused within a run. assign,
    // assign_dynarr and move each call this exactly once, which is where their
    // single bump comes from.
    ++h->epoch;
}

extern "C" void rt_queue_assign(rt_queue* dst, const rt_queue* src, const rt_elem_ops* ops,
                                const rt_alloc_site* site) {
    if (dst == src) {
        return;
    }
    rt_queue_delete_all(dst, ops);
    if (src->len == 0) {
        return;
    }
    const uint64_t stride = ops->stride;
    auto* buf = static_cast<uint8_t*>(
        rt_alloc_elems_checked(src->len, stride, site, rt_diag_now()));
    if (!elem_managed(ops)) {
        for (uint64_t i = 0; i < src->len; ++i) {
            std::memcpy(buf + i * stride, q_elem(src, stride, i), static_cast<size_t>(stride));
        }
    } else {
        for (uint64_t i = 0; i < src->len; ++i) {
            rt_elem_init(ops, buf + i * stride);
            rt_elem_copy(ops, buf + i * stride, q_elem(src, stride, i), site);
        }
    }
    dst->base = buf;
    dst->head = 0;
    dst->len  = src->len;
    dst->cap  = src->len;
}

// Deep-copy a dynamic array into a queue (LRM 7.10 assignment compatibility):
// `q = d`. The source is an rt_dynarr (contiguous {data,len}), NOT an rt_queue,
// so it cannot go through rt_queue_assign, whose struct layout differs. An empty
// source clears the queue to size 0.
extern "C" void rt_queue_assign_dynarr(rt_queue* dst, const rt_dynarr* src,
                                       const rt_elem_ops* ops, const rt_alloc_site* site) {
    // No dst == src guard, unlike every sibling assign: the operands are an
    // rt_queue and an rt_dynarr: distinct handle types that can never name the
    // same object, so the comparison could only ever be false. A comment costs
    // less than a check that is dead by construction.
    rt_queue_delete_all(dst, ops);
    if (src->len == 0) {
        return;
    }
    const uint64_t stride = ops->stride;
    const auto*    sdata  = static_cast<const uint8_t*>(src->data);
    auto* buf = static_cast<uint8_t*>(
        rt_alloc_elems_checked(src->len, stride, site, rt_diag_now()));
    if (!elem_managed(ops)) {
        std::memcpy(buf, sdata, static_cast<size_t>(src->len * stride));
    } else {
        for (uint64_t i = 0; i < src->len; ++i) {
            rt_elem_init(ops, buf + i * stride);
            rt_elem_copy(ops, buf + i * stride, sdata + i * stride, site);
        }
    }
    dst->base = buf;
    dst->head = 0;
    dst->len  = src->len;
    dst->cap  = src->len;
}

// Deep-copy a queue into a dynamic array (LRM 7.5.1 resizeable assignment):
// `d = q`. The source is an rt_queue (ring-indexed {base,head,len,cap}), NOT
// an rt_dynarr, so it cannot go through rt_dynarr_assign, whose struct layout
// differs -- the mirror of rt_queue_assign_dynarr above. An empty source
// clears the array to size 0. Always copies element-by-element (even for an
// unmanaged/POD element): the source ring can wrap, so it has no single
// contiguous run to bulk memcpy the way rt_dynarr_assign's dynarr source does.
extern "C" void rt_dynarr_assign_queue(rt_dynarr* dst, const rt_queue* src,
                                       const rt_elem_ops* ops, const rt_alloc_site* site) {
    // No dst == src guard, unlike every sibling assign: the operands are an
    // rt_dynarr and an rt_queue: distinct handle types that can never name the
    // same object, so the comparison could only ever be false. A comment costs
    // less than a check that is dead by construction.
    rt_dynarr_delete(dst, ops);
    if (src->len == 0) {
        return;
    }
    const uint64_t stride = ops->stride;
    auto* buf = static_cast<uint8_t*>(
        rt_alloc_elems_checked(src->len, stride, site, rt_diag_now()));
    if (!elem_managed(ops)) {
        for (uint64_t i = 0; i < src->len; ++i) {
            std::memcpy(buf + i * stride, q_elem(src, stride, i), stride);
        }
    } else {
        for (uint64_t i = 0; i < src->len; ++i) {
            rt_elem_init(ops, buf + i * stride);
            rt_elem_copy(ops, buf + i * stride, q_elem(src, stride, i), site);
        }
    }
    dst->data = buf;
    dst->len  = src->len;
}

extern "C" void rt_queue_append_range(rt_queue* dst, const rt_queue* src,
                                      const rt_elem_ops* ops, uint64_t bound, int64_t lo,
                                      int64_t hi, const rt_alloc_site* site) {
    if (src->len == 0) {
        return;
    }
    const int64_t last = static_cast<int64_t>(src->len) - 1;
    if (lo < 0) {
        lo = 0;
    }
    if (hi > last) {
        hi = last;
    }
    // Each source element is deep-copied through the shared push body (which
    // deinit/copy-constructs a managed element). dst never aliases src, so
    // growing dst cannot relocate src's buffer mid-loop. The raw form is used so
    // the whole append advances the epoch once rather than once per element.
    for (int64_t i = lo; i <= hi; ++i) {
        q_push_back_raw(dst, ops, bound, q_elem(src, ops->stride, static_cast<uint64_t>(i)), site);
    }
    if (lo <= hi) {
        ++dst->epoch;
    }
}

// Render a queue / dynamic array as `%p` assignment-pattern text `'{e0, ...}`
// (LRM 21.2.1.6) into `out`. `kind`: 0 integral, 1 real, 2 string element. An
// integral element slot is `must | (may << width)` (an i(2*width) word) plus an
// optional z plane at `z_off`; a 2-state slot is the bare value word. Element
// width is <= 64 (the frontend guards wider). Integral leaves reuse render_arg
// under `%0d` (minimal decimal, the element's signedness honoured) so the leaf
// format matches a fixed-array `%p`: LRM 21.2.1.6 prints a singular element "as
// [it] would unformatted", and 21.2.1.1 makes the unformatted default decimal.
extern "C" void rt_container_format_p(rt_string* out, const void* handle, int32_t is_queue,
                                      int32_t kind, uint32_t width, uint32_t is_signed,
                                      uint32_t four_state, uint32_t has_z, uint32_t z_off,
                                      uint64_t stride, const rt_alloc_site* site) {
    uint64_t       count = 0;
    const uint8_t* base  = nullptr; // dynamic-array element base (linear)
    const rt_queue* q    = nullptr; // queue handle (ring indexing)
    if (is_queue != 0) {
        q     = static_cast<const rt_queue*>(handle);
        count = q->len;
    } else {
        const auto* a = static_cast<const rt_dynarr*>(handle);
        count         = a->len;
        base          = static_cast<const uint8_t*>(a->data);
    }

    clotho::FormatSpec spec{};
    spec.ok        = true;
    spec.conv      = 'd';
    spec.has_width = true; // width 0 => minimal digits (the %0d form)

    std::string s = "'{";
    for (uint64_t i = 0; i < count; ++i) {
        if (i != 0) {
            s += ", ";
        }
        const uint8_t* slot = q != nullptr ? q_elem(q, stride, i) : base + i * stride;
        if (kind == 2) {
            const auto* es = reinterpret_cast<const rt_string*>(slot);
            s += '"';
            s.append(rt_string_data(es), static_cast<size_t>(rt_string_len(es)));
            s += '"';
            continue;
        }
        if (kind == 1) {
            double dv = 0;
            std::memcpy(&dv, slot, sizeof(double));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", dv);
            s += buf;
            continue;
        }
        // Integral: extract the must / xz / z planes for render_arg.
        const uint64_t maskW = width >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << width) - 1);
        uint64_t       must = 0, xz = 0, z = 0;
        if (four_state != 0) {
            unsigned __int128 f0 = 0;
            std::memcpy(&f0, slot, static_cast<size_t>((2 * width + 7) / 8));
            must = static_cast<uint64_t>(f0) & maskW;
            const uint64_t may = static_cast<uint64_t>(f0 >> width) & maskW;
            xz                 = must ^ may;
            if (has_z != 0) {
                std::memcpy(&z, slot + z_off, static_cast<size_t>((width + 7) / 8));
                z &= maskW;
            }
        } else {
            std::memcpy(&must, slot, static_cast<size_t>((width + 7) / 8));
            must &= maskW;
        }
        const rt_arg a{ width, is_signed != 0 ? 1u : 0u, &must, &xz, &z };
        render_arg(s, a, spec);
    }
    s += '}';
    rt_string_assign_bytes(out, s.data(), static_cast<uint64_t>(s.size()), site);
}
namespace {

// The `n` bits (n <= 64) of a little-endian 64-bit plane starting at bit `lo`.
// A null plane (a 2-state value carries no xz / z) reads as all zero.
uint64_t plane_bits(const uint64_t* p, uint32_t lo, uint32_t n) {
    if (p == nullptr || n == 0) {
        return 0;
    }
    const uint32_t w0  = lo / 64;
    const uint32_t off = lo % 64;
    uint64_t       r   = p[w0] >> off;
    if (off != 0 && off + n > 64) {
        r |= p[w0 + 1] << (64 - off);
    }
    return n == 64 ? r : r & ((uint64_t{ 1 } << n) - 1);
}

// Copy the low `w` bits of `src` into `dst` (plane_words(w) words), zero-extended
// above `w`. render_arg requires that zero extension: it clips only the top word,
// so handing it the union's full-width plane under a narrower member width would
// leak the gap and tag bits into the rendered leaf.
void copy_low_bits(std::vector<uint64_t>& dst, const uint64_t* src, uint32_t w) {
    const uint32_t words = plane_words(w);
    dst.assign(words, 0);
    if (src == nullptr) {
        return;
    }
    for (uint32_t k = 0; k < words; ++k) {
        dst[k] = src[k];
    }
    if (w % 64 != 0) {
        dst[words - 1] &= (uint64_t{ 1 } << (w % 64)) - 1;
    }
}

} // namespace

extern "C" void rt_tagged_format_p(rt_string* out, const rt_arg* v, uint32_t width,
                                   uint32_t tag_bits, uint32_t count, const char* const* names,
                                   const uint32_t* name_lens, const uint32_t* widths,
                                   const uint32_t* signs, const rt_alloc_site* site) {
    clotho::FormatSpec spec{};
    spec.ok        = true;
    spec.conv      = 'd';
    spec.has_width = true; // width 0 => minimal digits (the %0d form)

    // A single-member tagged union reserves no tag bits, so its one member is
    // always the selected one.
    const uint64_t tag   = tag_bits == 0 ? 0 : plane_bits(v->val, width - tag_bits, tag_bits);
    const uint64_t tag_x = tag_bits == 0 ? 0 : plane_bits(v->xz, width - tag_bits, tag_bits);

    std::string s;
    if (tag_x != 0 || tag >= count) {
        // No member is valid, so there is no `tag:value` to print. Fall back to the
        // ordinary integral leaf over the whole flat value, which shows the x digits
        // for an uninitialized union rather than inventing a member name.
        std::vector<uint64_t> mv, mx, mz;
        copy_low_bits(mv, v->val, width);
        copy_low_bits(mx, v->xz, width);
        copy_low_bits(mz, v->z, width);
        const rt_arg a{ width, 0, mv.data(), mx.data(), mz.data() };
        render_arg(s, a, spec);
    } else {
        s.append(names[tag], static_cast<size_t>(name_lens[tag]));
        if (const uint32_t mw = widths[tag]; mw != 0) {
            // A void member (width 0) has no element to print, only its tag.
            std::vector<uint64_t> mv, mx, mz;
            copy_low_bits(mv, v->val, mw);
            copy_low_bits(mx, v->xz, mw);
            copy_low_bits(mz, v->z, mw);
            s += ':';
            const rt_arg a{ mw, signs[tag] != 0 ? 1u : 0u, mv.data(), mx.data(), mz.data() };
            render_arg(s, a, spec);
        }
    }
    rt_string_assign_bytes(out, s.data(), static_cast<uint64_t>(s.size()), site);
}

// Logical equality (`==`) of two queues / dynamic arrays (LRM 11.2.2, 11.4.5).
// Returns 0 (definitely unequal), 1 (definitely equal), 2 (ambiguous -> 1'bx).
// Different sizes are a definite inequality (0, never x). Same size folds a
// per-element 4-state compare: any definite element mismatch -> 0; else any
// unknown bit -> x; else 1. Element params match rt_container_format_p; integral
// width <= 64 (the frontend guards wider). `!=` is the caller's Lnot of this.
extern "C" uint8_t rt_container_eq(const void* pa, const void* pb, int32_t is_queue, int32_t kind,
                                   uint32_t width, uint32_t /*is_signed*/, uint32_t four_state,
                                   uint32_t has_z, uint32_t z_off, uint64_t stride) {
    uint64_t        na = 0, nb = 0;
    const uint8_t*  ba = nullptr;
    const uint8_t*  bb = nullptr;
    const rt_queue* qa = nullptr;
    const rt_queue* qb = nullptr;
    if (is_queue != 0) {
        qa = static_cast<const rt_queue*>(pa);
        qb = static_cast<const rt_queue*>(pb);
        na = qa->len;
        nb = qb->len;
    } else {
        na = static_cast<const rt_dynarr*>(pa)->len;
        nb = static_cast<const rt_dynarr*>(pb)->len;
        ba = static_cast<const uint8_t*>(static_cast<const rt_dynarr*>(pa)->data);
        bb = static_cast<const uint8_t*>(static_cast<const rt_dynarr*>(pb)->data);
    }
    if (na != nb) {
        return 0;
    }
    const uint64_t maskW = width >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << width) - 1);
    bool           any_x = false;
    for (uint64_t i = 0; i < na; ++i) {
        const uint8_t* sa = qa != nullptr ? q_elem(qa, stride, i) : ba + i * stride;
        const uint8_t* sb = qb != nullptr ? q_elem(qb, stride, i) : bb + i * stride;
        if (kind == 2) {
            const auto* xa = reinterpret_cast<const rt_string*>(sa);
            const auto* xb = reinterpret_cast<const rt_string*>(sb);
            const uint64_t la = rt_string_len(xa);
            if (la != rt_string_len(xb) ||
                std::memcmp(rt_string_data(xa), rt_string_data(xb), static_cast<size_t>(la)) != 0) {
                return 0;
            }
            continue;
        }
        if (kind == 1) {
            double da = 0, db = 0;
            std::memcpy(&da, sa, sizeof(double));
            std::memcpy(&db, sb, sizeof(double));
            if (da != db) {
                return 0;
            }
            continue;
        }
        // Integral 4-state compare of the two element slots.
        uint64_t must_a = 0, unk_a = 0, must_b = 0, unk_b = 0;
        if (four_state != 0) {
            unsigned __int128 fa = 0, fb = 0;
            std::memcpy(&fa, sa, static_cast<size_t>((2 * width + 7) / 8));
            std::memcpy(&fb, sb, static_cast<size_t>((2 * width + 7) / 8));
            must_a = static_cast<uint64_t>(fa) & maskW;
            must_b = static_cast<uint64_t>(fb) & maskW;
            unk_a  = (must_a ^ (static_cast<uint64_t>(fa >> width) & maskW));
            unk_b  = (must_b ^ (static_cast<uint64_t>(fb >> width) & maskW));
            (void)has_z;
            (void)z_off;
        } else {
            std::memcpy(&must_a, sa, static_cast<size_t>((width + 7) / 8));
            std::memcpy(&must_b, sb, static_cast<size_t>((width + 7) / 8));
            must_a &= maskW;
            must_b &= maskW;
        }
        // A definite mismatch on any bit known in both operands settles it (0).
        if (((must_a ^ must_b) & ~unk_a & ~unk_b) != 0) {
            return 0;
        }
        if ((unk_a | unk_b) != 0) {
            any_x = true;
        }
    }
    return any_x ? 2 : 1;
}

// Move `src`'s buffer into `dst` (ownership transfer): destroy `dst`'s current
// elements + buffer, take `src`'s, and leave `src` the empty queue. Used by the
// nonblocking-assignment commit — the RHS was deep-copied into `src` in the
// Active region, so the NBA region just hands that buffer to `dst` with no
// second copy. A no-op when dst == src.
extern "C" void rt_queue_move(rt_queue* dst, rt_queue* src, const rt_elem_ops* ops) {
    if (dst == src) {
        return;
    }
    // dst's epoch advances inside rt_queue_delete_all, which is the single bump
    // this move owes it; src's is bumped here because it is left empty.
    rt_queue_delete_all(dst, ops);
    dst->base = src->base;
    dst->head = src->head;
    dst->len  = src->len;
    dst->cap  = src->cap;
    src->base = nullptr;
    src->head = 0;
    src->len  = 0;
    src->cap  = 0;
    ++src->epoch;
}

extern "C" void rt_queue_reverse(rt_queue* h, const rt_elem_ops* ops) {
    if (h->len < 2) {
        return;
    }
    const uint64_t       stride = ops->stride;
    std::vector<uint8_t> tmp(static_cast<size_t>(stride));
    for (uint64_t i = 0, j = h->len - 1; i < j; ++i, --j) {
        swap_slots(q_elem(h, stride, i), q_elem(h, stride, j), stride, tmp.data());
    }
    ++h->epoch;
}

extern "C" void rt_queue_sort(rt_queue* h, const rt_elem_ops* ops, uint32_t kind, uint32_t width,
                              uint32_t rev) {
    if (h->len < 2) {
        return;
    }
    const uint64_t stride = ops->stride;
    const uint64_t n      = h->len;
    // Gather the ring's logical order into a contiguous scratch, sort there, then
    // scatter back into the same ring positions (head unchanged).
    std::vector<uint8_t> buf(static_cast<size_t>(n * stride));
    for (uint64_t i = 0; i < n; ++i) {
        std::memcpy(buf.data() + i * stride, q_elem(h, stride, i), static_cast<size_t>(stride));
    }
    if (sort_kind_four(kind)) {
        sort_run4(buf.data(), n, stride, kind, width, rev);
    } else {
        sort_run(buf.data(), n, stride, kind, width, rev);
    }
    for (uint64_t i = 0; i < n; ++i) {
        std::memcpy(q_elem(h, stride, i), buf.data() + i * stride, static_cast<size_t>(stride));
    }
    ++h->epoch;
}

extern "C" void rt_queue_shuffle(rt_queue* h, const rt_elem_ops* ops, uint64_t* rng) {
    if (h->len < 2) {
        return;
    }
    const uint64_t       stride = ops->stride;
    std::vector<uint8_t> tmp(static_cast<size_t>(stride));
    for (uint64_t i = h->len - 1; i > 0; --i) {
        const uint64_t j = xoshiro_next(rng) % (i + 1);
        swap_slots(q_elem(h, stride, i), q_elem(h, stride, j), stride, tmp.data());
    }
    ++h->epoch;
}
namespace {

// -- Associative-array key scratch buffer (docs/design/assoc-keys.md P1) --
// See the rationale at the declaration (Runtime.h).
std::vector<uint8_t> g_keybuf;

} // namespace

extern "C" uint32_t rt_keybuf_begin(void) {
    return static_cast<uint32_t>(g_keybuf.size());
}

extern "C" void rt_keybuf_append(const void* bytes, uint32_t len) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    g_keybuf.insert(g_keybuf.end(), p, p + len);
}

extern "C" void rt_keybuf_span(uint32_t mark, const uint8_t** out_ptr, uint32_t* out_len) {
    *out_ptr = g_keybuf.data() + mark;
    *out_len = static_cast<uint32_t>(g_keybuf.size() - mark);
}

extern "C" void rt_keybuf_end(uint32_t mark) {
    g_keybuf.resize(mark);
}

// Whole-associative-array equality (LRM 11.4.5 / 7.9, P4a): 1 if two spans are
// byte-identical, else 0. The backend builds each side's span with the P3
// assoc-key canonical serializer (count || sorted (key, value) pairs), so
// byte identity here is exactly map-content equality.
extern "C" uint8_t rt_key_span_eq(const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen) {
    return (alen == blen && std::memcmp(a, b, alen) == 0) ? 1 : 0;
}
namespace {

// -- Associative arrays (LRM 7.8/7.9) --
//
// The map is keyed by the backend's normalized order-preserving key bytes
// (docs/design/dynamic-arrays.md §4a), so hashing / equality / ordering are all
// key-kind-agnostic byte operations. The value is the element slot bytes (which
// may carry managed sub-handles, serviced through the rt_elem_ops descriptor).

// Unsigned lexicographic order (shorter-is-less on a prefix) — the order the
// backend's normalization is built to make equal to each key kind's LRM order.
struct AssocKeyLess {
    static bool less(std::string_view a, std::string_view b) {
        const size_t n = a.size() < b.size() ? a.size() : b.size();
        const int    c = std::memcmp(a.data(), b.data(), n);
        if (c != 0) {
            return c < 0;
        }
        return a.size() < b.size();
    }
    // The typed overloads (both pointer, plus the heterogeneous span forms for
    // lower/upper_bound) all reduce to the same span compare.
    bool operator()(const std::string* a, const std::string* b) const { return less(*a, *b); }
    bool operator()(const std::string* a, std::string_view b) const { return less(*a, b); }
    bool operator()(std::string_view a, const std::string* b) const { return less(a, *b); }
};

// The alloc site of the associative-array operation currently in flight. A
// std::vector reaches its allocator with no room for an extra argument, so the
// slot allocator below reads the site from here rather than carrying it. Single
// writer -- the simulation thread -- which is the assumption g_diag_wheel makes
// too.
const rt_alloc_site* g_assoc_site = nullptr;

// Scoped set/restore of g_assoc_site. Restoring rather than clearing is what
// keeps the innermost site reporting when an element copy recurses into a
// nested associative array.
struct AssocSiteScope {
    const rt_alloc_site* prev;

    explicit AssocSiteScope(const rt_alloc_site* s) : prev(g_assoc_site) { g_assoc_site = s; }
    ~AssocSiteScope() { g_assoc_site = prev; }
    AssocSiteScope(const AssocSiteScope&)            = delete;
    AssocSiteScope& operator=(const AssocSiteScope&) = delete;
};

// std::vector allocator that routes an element-slot buffer through the runtime's
// checked allocator, so an associative array's element heap fails the way every
// other container's does -- a located fatal naming the statement -- instead of
// raising std::bad_alloc inside an entry point reached across a C ABI that has
// no unwind tables to carry it out. Stateless (the site comes from
// g_assoc_site), so slot vectors stay swappable and the map may relocate them.
template <class T>
struct CheckedAlloc {
    using value_type = T;

    CheckedAlloc() = default;
    template <class U>
    constexpr CheckedAlloc(const CheckedAlloc<U>& /*other*/) noexcept {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(rt_alloc_elems_checked(n, sizeof(T), g_assoc_site, rt_diag_now()));
    }
    void deallocate(T* p, std::size_t /*n*/) noexcept { std::free(p); }

    template <class U>
    bool operator==(const CheckedAlloc<U>& /*other*/) const noexcept {
        return true;
    }
};

// One element's slot bytes: `stride` of them, owning whatever managed handles
// the element descriptor places inside.
using SlotVec = std::vector<uint8_t, CheckedAlloc<uint8_t>>;

struct AssocMap {
    ankerl::unordered_dense::map<std::string, SlotVec> entries;
    std::vector<const std::string*>                    sorted;
    bool                                               sorted_valid = false;
    // The `'{default: v}` value (LRM 7.9.11), returned by a read of an absent
    // key. Per-array state rather than an entry, so it is invisible to
    // num/exists/first/next and survives a.delete().
    SlotVec default_slot;
    bool    has_default = false;

    void invalidate() { sorted_valid = false; }

    const std::vector<const std::string*>& sorted_keys() {
        if (!sorted_valid) {
            sorted.clear();
            sorted.reserve(entries.size());
            for (const auto& kv : entries) {
                sorted.push_back(&kv.first);
            }
            std::sort(sorted.begin(), sorted.end(), AssocKeyLess{});
            sorted_valid = true;
        }
        return sorted;
    }
};

AssocMap* assoc_impl(rt_assoc* h) { return static_cast<AssocMap*>(h->impl); }

// Allocate and construct one AssocMap through the checked allocator. A bare
// `new` raises std::bad_alloc, which cannot propagate out of a runtime entry
// point reached across a C ABI with no unwind tables; this reports the same
// located fatal a dynamic array's failed allocation does. The map's interior --
// ankerl's own arrays and the key strings -- still uses std::allocator: routing
// those would mean changing the key type and threading a site through ankerl's
// allocator parameter, a larger change than the remaining exposure justifies.
AssocMap* assoc_new(const rt_alloc_site* site) {
    void* raw = rt_alloc_checked(sizeof(AssocMap), site, rt_diag_now());
    return new (raw) AssocMap();
}

// Deinit every element value through the descriptor, then drop the map.
void assoc_free(AssocMap* m, const rt_elem_ops* ops) {
    if (m == nullptr) {
        return;
    }
    if (ops->kind != rt_elem_pod) {
        for (auto& kv : m->entries) {
            rt_elem_deinit(ops, kv.second.data());
        }
        if (m->has_default) {
            rt_elem_deinit(ops, m->default_slot.data());
        }
    }
    // Matches assoc_new: the storage came from the checked allocator, so it is
    // destroyed by hand and returned to free() rather than to operator delete.
    m->~AssocMap();
    std::free(m);
}

// Drop every entry but keep the map alive, so the pattern default survives.
void assoc_clear_entries(AssocMap* m, const rt_elem_ops* ops) {
    if (ops->kind != rt_elem_pod) {
        for (auto& kv : m->entries) {
            rt_elem_deinit(ops, kv.second.data());
        }
    }
    m->entries.clear();
    m->invalidate();
}

void assoc_set_span(const std::string* k, const uint8_t** out_ptr, uint32_t* out_len) {
    *out_ptr = reinterpret_cast<const uint8_t*>(k->data());
    *out_len = static_cast<uint32_t>(k->size());
}

} // namespace

extern "C" void* rt_assoc_get_ptr(rt_assoc* h, const uint8_t* kptr, uint32_t klen) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr) {
        return nullptr;
    }
    const auto it = m->entries.find(
      std::string(reinterpret_cast<const char*>(kptr), static_cast<size_t>(klen)));
    if (it != m->entries.end()) {
        return it->second.data();
    }
    // Absent key: the pattern default when one was set (LRM 7.9.11 -- and no
    // warning), else null so the backend materializes the Table 7-1 element
    // default. Neither creates an entry.
    return m->has_default ? m->default_slot.data() : nullptr;
}

extern "C" void* rt_assoc_put_ptr(rt_assoc* h, const rt_elem_ops* ops, const uint8_t* kptr,
                                  uint32_t klen, const rt_alloc_site* site) {
    const AssocSiteScope scope(site);
    AssocMap*            m = assoc_impl(h);
    if (m == nullptr) {
        m       = assoc_new(site);
        h->impl = m;
    }
    std::string key(reinterpret_cast<const char*>(kptr), static_cast<size_t>(klen));
    const auto  it = m->entries.find(key);
    if (it != m->entries.end()) {
        // The entry already exists, so nothing about the array's shape changes:
        // a plain overwrite of a present key is the one write that leaves every
        // ref valid, and it does not touch the epoch.
        return it->second.data();
    }
    auto& slot = m->entries[std::move(key)];
    slot.resize(static_cast<size_t>(ops->stride));
    rt_elem_init(ops, slot.data());
    m->invalidate();
    // Creating an entry changes num(), the first/next traversal order and which
    // keys resolve. Slot buffers are separately owned and do not move, so the
    // bump is conservative -- but a ref's guard checks the container, not one
    // slot (docs/design/data-model.md section 3.1).
    ++h->epoch;
    return slot.data();
}

extern "C" void* rt_assoc_default_ptr(rt_assoc* h, const rt_elem_ops* ops,
                                      const rt_alloc_site* site) {
    const AssocSiteScope scope(site);
    AssocMap*            m = assoc_impl(h);
    if (m == nullptr) {
        m       = assoc_new(site);
        h->impl = m;
    }
    if (!m->has_default) {
        m->default_slot.resize(static_cast<size_t>(ops->stride));
        rt_elem_init(ops, m->default_slot.data());
        m->has_default = true;
        // Creating the default changes what a read of an absent key resolves to.
        // Writing through the returned pointer afterwards does not, so only the
        // call that creates the slot bumps.
        ++h->epoch;
    }
    return m->default_slot.data();
}

extern "C" int rt_assoc_exists(rt_assoc* h, const uint8_t* kptr, uint32_t klen) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr) {
        return 0;
    }
    return m->entries.contains(
             std::string(reinterpret_cast<const char*>(kptr), static_cast<size_t>(klen)))
             ? 1
             : 0;
}

extern "C" void rt_assoc_delete_key(rt_assoc* h, const rt_elem_ops* ops, const uint8_t* kptr,
                                    uint32_t klen) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr) {
        return;
    }
    const auto it = m->entries.find(
      std::string(reinterpret_cast<const char*>(kptr), static_cast<size_t>(klen)));
    if (it == m->entries.end()) {
        return;
    }
    if (ops->kind != rt_elem_pod) {
        rt_elem_deinit(ops, it->second.data());
    }
    m->entries.erase(it);
    m->invalidate();
    // Reached only when an entry really was erased -- an absent key returned
    // above without touching the epoch.
    ++h->epoch;
}

extern "C" void rt_assoc_delete_all(rt_assoc* h, const rt_elem_ops* ops) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr) {
        // Already empty: nothing is destroyed, so the epoch stays put.
        return;
    }
    ++h->epoch;
    if (m->has_default) {
        // The pattern default outlives the entries (LRM 7.9.11 keeps it
        // independent of the array's contents), so the map has to stay.
        assoc_clear_entries(m, ops);
        return;
    }
    assoc_free(m, ops);
    h->impl = nullptr;
}

extern "C" void rt_assoc_reset(rt_assoc* h, const rt_elem_ops* ops) {
    assoc_free(assoc_impl(h), ops);
    h->impl = nullptr;
    // Advanced, never zeroed, so an epoch is never reused within a run: a ref
    // formed against the discarded contents stays distinguishable from one
    // formed against whatever replaces them. assign and move each call this
    // exactly once, which is where their single bump comes from.
    ++h->epoch;
}

extern "C" uint64_t rt_assoc_num(rt_assoc* h) {
    AssocMap* m = assoc_impl(h);
    return m == nullptr ? 0 : static_cast<uint64_t>(m->entries.size());
}

// The collector's precise walk over an associative array (Runtime.h:934-941). The
// only export of the map's interior, and deliberately the narrowest one that works:
// the entries live in an unordered_dense whose per-entry SlotVec payloads relocate
// on rehash, so an address handed out now is not an address later and no external
// code can iterate the map by address.
//
// Read-only by contract. The visitor marks; it must not insert, erase or resize,
// which would invalidate the iterator this loop holds -- so the entry count is
// captured before the walk and asserted unchanged after it.
extern "C" void rt_assoc_visit(const rt_assoc* h,
                               void (*visit)(void* elem, const uint8_t* key, uint32_t klen,
                                             void* ctx),
                               void* ctx) {
    assert(h != nullptr && "a visit needs an initialized associative-array handle");
    assert(visit != nullptr && "a visit needs a visitor");
    AssocMap* m = assoc_impl(const_cast<rt_assoc*>(h));
    if (m == nullptr) {
        // A null impl IS the empty array -- no map is allocated until the first
        // entry-creating put or default_ptr, and every reader spells that out the
        // same way (rt_assoc_num, :2351-2354; rt_assoc_get_ptr, :2228-2232).
        return;
    }
    [[maybe_unused]] const size_t entries_before = m->entries.size();
    for (auto& kv : m->entries) {
        assert(kv.second.data() != nullptr && "a live entry owns its element slot");
        visit(kv.second.data(), reinterpret_cast<const uint8_t*>(kv.first.data()),
              static_cast<uint32_t>(kv.first.size()), ctx);
    }
    // The pattern default (LRM 7.9.11) is a live element image with no key: a read of
    // ANY absent index yields it, so a class handle inside it is a strong reference
    // just as an entry's is (docs/design/garbage-collection.md §9). A null `key` is
    // the unambiguous marker for it -- std::string::data() never returns null, not
    // even for the empty string a[""] legitimately produces, so no real entry can
    // present one.
    if (m->has_default) {
        assert(m->default_slot.data() != nullptr && "a present default owns its slot");
        visit(m->default_slot.data(), nullptr, 0, ctx);
    }
    assert(m->entries.size() == entries_before && "a visitor mutated the map");
}

extern "C" int rt_assoc_first(rt_assoc* h, const uint8_t** out_ptr, uint32_t* out_len) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr || m->entries.empty()) {
        return 0;
    }
    assoc_set_span(m->sorted_keys().front(), out_ptr, out_len);
    return 1;
}

extern "C" int rt_assoc_last(rt_assoc* h, const uint8_t** out_ptr, uint32_t* out_len) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr || m->entries.empty()) {
        return 0;
    }
    assoc_set_span(m->sorted_keys().back(), out_ptr, out_len);
    return 1;
}

extern "C" int rt_assoc_next(rt_assoc* h, const uint8_t* cur_ptr, uint32_t cur_len,
                             const uint8_t** out_ptr, uint32_t* out_len) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr || m->entries.empty()) {
        return 0;
    }
    const std::string_view cur(reinterpret_cast<const char*>(cur_ptr),
                               static_cast<size_t>(cur_len));
    const auto&            keys = m->sorted_keys();
    // First key strictly greater than `cur` (upper_bound), robust to cur absent.
    const auto it = std::upper_bound(keys.begin(), keys.end(), cur, AssocKeyLess{});
    if (it == keys.end()) {
        return 0;
    }
    assoc_set_span(*it, out_ptr, out_len);
    return 1;
}

extern "C" int rt_assoc_prev(rt_assoc* h, const uint8_t* cur_ptr, uint32_t cur_len,
                             const uint8_t** out_ptr, uint32_t* out_len) {
    AssocMap* m = assoc_impl(h);
    if (m == nullptr || m->entries.empty()) {
        return 0;
    }
    const std::string_view cur(reinterpret_cast<const char*>(cur_ptr),
                               static_cast<size_t>(cur_len));
    const auto&            keys = m->sorted_keys();
    // Largest key strictly less than `cur`: lower_bound, then step back.
    const auto it = std::lower_bound(keys.begin(), keys.end(), cur, AssocKeyLess{});
    if (it == keys.begin()) {
        return 0;
    }
    assoc_set_span(*(it - 1), out_ptr, out_len);
    return 1;
}

extern "C" void rt_assoc_assign(rt_assoc* dst, const rt_assoc* src, const rt_elem_ops* ops, const rt_alloc_site* site) {
    if (dst == src) {
        return;
    }
    const AssocSiteScope scope(site);
    // The pattern default is part of the value, so the copy replaces dst's
    // default with src's -- reset, do not delete_all, so that assigning an
    // array with no default clears the target's. That reset is also where dst's
    // single epoch bump for this assignment comes from.
    rt_assoc_reset(dst, ops);
    AssocMap* sm = assoc_impl(const_cast<rt_assoc*>(src));
    if (sm == nullptr || (sm->entries.empty() && !sm->has_default)) {
        return;
    }
    auto* dm  = assoc_new(site);
    dst->impl = dm;
    dm->entries.reserve(sm->entries.size());
    const bool managed = ops->kind != rt_elem_pod;
    const auto copy_in = [&](SlotVec& slot, const uint8_t* from) {
        slot.resize(static_cast<size_t>(ops->stride));
        if (managed) {
            rt_elem_init(ops, slot.data());
            rt_elem_copy(ops, slot.data(), from, site);
        } else {
            std::memcpy(slot.data(), from, static_cast<size_t>(ops->stride));
        }
    };
    for (const auto& kv : sm->entries) {
        copy_in(dm->entries[kv.first], kv.second.data());
    }
    if (sm->has_default) {
        copy_in(dm->default_slot, sm->default_slot.data());
        dm->has_default = true;
    }
}

// Move `src`'s map into `dst` (ownership transfer): destroy `dst`'s entries and
// pattern default, take `src`'s map, and leave `src` the empty array. Used by
// the nonblocking-assignment commit -- the RHS was deep-copied into `src` in the
// Active region, so the NBA region just hands that map over with no second copy.
// A no-op when dst == src.
extern "C" void rt_assoc_move(rt_assoc* dst, rt_assoc* src, const rt_elem_ops* ops) {
    if (dst == src) {
        return;
    }
    // dst's epoch advances inside rt_assoc_reset, which is the single bump this
    // move owes it; src's is bumped here because it is left empty.
    rt_assoc_reset(dst, ops);
    dst->impl = src->impl;
    src->impl = nullptr;
    ++src->epoch;
}
