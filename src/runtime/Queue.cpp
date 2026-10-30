// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The queue engine's element operations (LRM 7.10.2): push, pop, insert, delete
// and range append.
//
// Built twice. The runtime compiles this file like every other source. CMakeLists
// also compiles it to LLVM bitcode with CLOTHO_RT_BITCODE defined, and the backend
// links the functions a module calls into that module (ir/llvm/RuntimeBitcode.cpp),
// where the element descriptor is a constant the optimizer folds through. The two
// builds differ only in how a managed element is copied and destroyed: the bitcode
// build calls the descriptor's generated kernels (rt_elem_ops::copy / destroy, which
// the backend fills in for every descriptor it hands to these functions when it
// links the bitcode), and the runtime build interprets the descriptor like the rest
// of the runtime. Growing the ring is a runtime call either way (rt_queue_grow): it
// allocates, and it is the cold half of a push.
//
// The bitcode build includes only Runtime.h and the C library, so the bitcode names
// no C++ library symbol and links the same against the JIT host's runtime and an
// executable's libc++ runtime.

#include "Runtime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if !defined(CLOTHO_RT_BITCODE)
#include "RuntimeInternal.h"

using namespace rt_internal;
#endif

namespace {

// Address of logical element `i` (0-based) in the ring. Requires cap > 0 and
// i < cap.
uint8_t* q_elem(const rt_queue* h, uint64_t stride, uint64_t i) {
    return static_cast<uint8_t*>(h->base) + rt_queue_slot(h, i) * stride;
}

// Copy-construct the element at `src` into the uninitialized slot `dst`.
void q_copy_elem(const rt_elem_ops* ops, void* dst, const void* src, const rt_alloc_site* site) {
    if (ops->kind == rt_elem_pod) {
        std::memcpy(dst, src, static_cast<size_t>(ops->stride));
        return;
    }
#if defined(CLOTHO_RT_BITCODE)
    ops->copy(dst, src, site);
#else
    rt_elem_init(ops, dst);
    rt_elem_copy(ops, dst, src, site);
#endif
}

// Release whatever the element in `slot` owns.
void q_destroy_elem(const rt_elem_ops* ops, void* slot) {
    if (ops->kind == rt_elem_pod) {
        return;
    }
#if defined(CLOTHO_RT_BITCODE)
    ops->destroy(slot);
#else
    rt_elem_deinit(ops, slot);
#endif
}

// Ensure capacity for at least `need` elements. Only the test is here; growing is
// rt_queue_grow's.
void q_reserve(rt_queue* h, uint64_t stride, uint64_t need, const rt_alloc_site* site) {
    if (need > h->cap) [[unlikely]] {
        rt_queue_grow(h, stride, need, site);
    }
}

// Drop elements past the bounded-queue limit, destroying managed ones (LRM 7.10.4:
// keep the first `bound + 1` elements). Out of line so the unbounded push the
// optimizer specializes stays small.
//
// Truncation is always from the TAIL, so the two push directions lose different
// elements when a full bounded queue overflows:
//   - push_front puts the new element at logical 0, so what gets dropped is the
//     old tail -- the end opposite the push.
//   - push_back puts the new element at logical len, so what gets dropped is the
//     element just pushed, and the push has no visible effect.
// Both fall out of "keep the first bound + 1"; neither direction is special-
// cased below. LRM 7.10.4 does not pin the choice (docs/LRM_NOTES.md, note 6).
[[gnu::noinline]] void q_truncate(rt_queue* h, const rt_elem_ops* ops, uint64_t bound) {
    for (uint64_t i = bound + 1; i < h->len; ++i) {
        q_destroy_elem(ops, q_elem(h, ops->stride, i));
    }
    h->len = bound + 1;
}

// bound == 0 is unbounded.
void q_apply_bound_ops(rt_queue* h, const rt_elem_ops* ops, uint64_t bound) {
    if (bound == 0 || h->len <= bound + 1) {
        return;
    }
    q_truncate(h, ops, bound);
}

// Append one element at the tail WITHOUT touching the epoch. The bump is left to
// the caller so that appending a whole range costs one epoch step instead of one
// per element: rt_queue_push_back bumps once, rt_queue_append_range bumps once
// for the range it appended.
void q_push_back_raw(rt_queue* h, const rt_elem_ops* ops, uint64_t bound, const void* elem,
                     const rt_alloc_site* site) {
    const uint64_t stride = ops->stride;
    q_reserve(h, stride, h->len + 1, site);
    q_copy_elem(ops, q_elem(h, stride, h->len), elem, site);
    h->len += 1;
    q_apply_bound_ops(h, ops, bound);
}

} // namespace

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
    q_copy_elem(ops, q_elem(h, stride, 0), elem, site);
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
    q_copy_elem(ops, q_elem(h, stride, i), elem, site);
    h->len += 1;
    q_apply_bound_ops(h, ops, bound);
    ++h->epoch;
}

extern "C" void rt_queue_delete_idx(rt_queue* h, const rt_elem_ops* ops, uint64_t i) {
    if (i >= h->len) {
        return;
    }
    const uint64_t stride = ops->stride;
    q_destroy_elem(ops, q_elem(h, stride, i));
    // Shift (i, len) down by one over the removed slot (relocation).
    for (uint64_t j = i; j + 1 < h->len; ++j) {
        std::memcpy(q_elem(h, stride, j), q_elem(h, stride, j + 1), static_cast<size_t>(stride));
    }
    h->len -= 1;
    ++h->epoch;
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
    // copy-constructs a managed element). dst never aliases src, so growing dst
    // cannot relocate src's buffer mid-loop. The raw form is used so the whole
    // append advances the epoch once rather than once per element.
    for (int64_t i = lo; i <= hi; ++i) {
        q_push_back_raw(dst, ops, bound, q_elem(src, ops->stride, static_cast<uint64_t>(i)), site);
    }
    if (lo <= hi) {
        ++dst->epoch;
    }
}

#if !defined(CLOTHO_RT_BITCODE)
// Geometric growth keeps repeated push amortised O(1). The new buffer holds the
// live elements contiguously from head == 0.
extern "C" void rt_queue_grow(rt_queue* h, uint64_t stride, uint64_t need, const rt_alloc_site* site) {
    assert(need > h->cap && "a ring only grows when it is too small");
    assert(stride > 0 && "an element slot is at least one byte");
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
#endif
