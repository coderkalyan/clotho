// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What the collector's budget knows about the heap buffers that strings and
// containers own (docs/design/garbage-collection.md §6.5).
//
// A collected object's `string`, dynamic array, queue and associative-array
// properties own malloc'd buffers the collector never allocated, and the
// collector frees them only when it finalizes the object. The budget counts
// holes of the object heap, so an object owning a megabyte string costs it 48
// bytes while it pins a megabyte. Every such buffer comes from rt_alloc_checked
// and goes back through one of a handful of sites in the runtime; those sites
// report here, and nothing else in the runtime knows the collector exists.
//
// This is the whole mechanism, kept in one header so that a heap that owns its
// buffers can replace it by deleting this file and the calls to it.
//
// Bytes are charged when a buffer is allocated, including when it grows, and
// credited when it is freed, so `live` is the bytes the design holds right now.
// The charge sites pass the size they allocated and the free sites pass the size
// they allocated it with, which is why a `free` here takes a byte count.

#pragma once

#include <cstdint>
#include <cstdlib>

namespace rt_internal {

struct OwnedBuffers {
    // Bytes charged and not yet credited.
    uint64_t live;
    // The value of `live` at which buffers_pressure() must be told. UINT64_MAX
    // when there is no collector to tell, so a charge outside a run costs one
    // compare and never leaves this header.
    uint64_t trigger;
    // Set while an emergency collection runs its finalizers: a free then asks
    // buffers_park() whether the running code can still be using the buffer.
    bool parking;
};

extern OwnedBuffers g_owned;

// Gc.cpp. Called when a charge crosses `trigger`: tells the collector the budget
// is spent, and past a larger threshold arranges for an emergency collection.
void buffers_pressure();

// Gc.cpp. Whether the code now running holds a pointer into [p, p + bytes), in
// which case the buffer is kept until a collection that finds it unreferenced.
bool buffers_park(void* p, uint64_t bytes);

inline void buffer_charge(uint64_t bytes) {
    g_owned.live += bytes;
    if (g_owned.live >= g_owned.trigger) [[unlikely]] {
        buffers_pressure();
    }
}

// `p` was allocated as `bytes` bytes by rt_alloc_checked (or is null, with
// `bytes` zero). The credit is clamped rather than asserted: a run's own
// buffers all pair up, but a string a previous run left in a runtime global and
// this one frees was charged to a counter rt_gc_init has since reset.
inline void buffer_free(void* p, uint64_t bytes) {
    g_owned.live -= bytes < g_owned.live ? bytes : g_owned.live;
    if (g_owned.parking) [[unlikely]] {
        if (buffers_park(p, bytes)) {
            return;
        }
    }
    std::free(p);
}

} // namespace rt_internal
