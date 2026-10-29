// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Wide 4-state value kernels: dynamic part-select read and write.

#include "RuntimeInternal.h"

using namespace rt_internal;

namespace {

// -- Runtime-offset bit / part select of a very wide vector (LRM 11.5.1) --
//
// The inline form of a dynamic select shifts the whole vector: `(v >> off) & mask`
// on an i(2W) SSA value. LLVM legalizes a variable shift on such a value into
// O(W/64) selects per word, so JIT time grows as O(W^2) -- 21 s for a 16 Kbit
// vector and over half an hour for the 65537-bit one in ivtest pr722. These two
// helpers index the containing 64-bit word instead, so the cost is O(selected
// bits) with no dependence on the vector's width at all. The backend routes a
// dynamic select here once the storage passes `wide_dyn_select_bits`.
//
// The storage layout is the one the backend's slot types define: a 4-state slot's
// `mm` field holds must in bits [0, width) and may in bits [width, 2*width), and
// its z plane (when not elided) is a separate `width`-bit field. A 2-state
// (narrow) slot is just `width` value bits in `mm`. Bit i of a field lives in word
// i/64 at bit i%64, little-endian, which is what the LLVM in-memory layout of an
// iN gives on every target clotho runs on.

// Bit `i` of a little-endian word array.
uint64_t plane_bit(const uint64_t* p, uint64_t i) {
    return (p[i >> 6] >> (i & 63)) & 1;
}

void plane_bit_set(uint64_t* p, uint64_t i, uint64_t v) {
    const uint64_t m = uint64_t{ 1 } << (i & 63);
    if (v != 0) {
        p[i >> 6] |= m;
    } else {
        p[i >> 6] &= ~m;
    }
}

} // namespace

// Read the `n` bits at storage bit offset `off` of a `width`-bit slot, clamped to
// the container [base, base+cwidth) the select sits in. `unknown` marks an x/z
// index, which LRM 11.5.1 makes wholly out of range. Bits outside the container
// read x (must 0, may 1) for a 4-state slot and 0 for a 2-state one.
//
// The three out-params are little-endian word buffers of ceil(n/64) words each --
// the same layout as the value planes, so the backend loads each one straight back
// as an i(n).
extern "C" void rt_wide_sel(const void* mmp, const void* zpp, uint32_t width, int64_t base,
                            uint32_t cwidth, int64_t off, uint32_t n, uint32_t four,
                            uint32_t has_z, uint32_t unknown, uint64_t* out_must,
                            uint64_t* out_may, uint64_t* out_z) {
    const auto*    mm    = static_cast<const uint64_t*>(mmp);
    const auto*    zp    = static_cast<const uint64_t*>(zpp);
    const uint32_t words = (n + 63) / 64;
    std::memset(out_must, 0, words * sizeof(uint64_t));
    std::memset(out_may, 0, words * sizeof(uint64_t));
    std::memset(out_z, 0, words * sizeof(uint64_t));
    for (uint32_t k = 0; k < n; ++k) {
        const int64_t i = off + static_cast<int64_t>(k);
        if (unknown != 0 || i < base || i >= base + static_cast<int64_t>(cwidth)) {
            if (four != 0) {
                plane_bit_set(out_may, k, 1); // x: must 0, may 1
            }
            continue;
        }
        const auto u = static_cast<uint64_t>(i);
        plane_bit_set(out_must, k, plane_bit(mm, u));
        if (four != 0) {
            plane_bit_set(out_may, k, plane_bit(mm, static_cast<uint64_t>(width) + u));
            if (has_z != 0) {
                plane_bit_set(out_z, k, plane_bit(zp, u));
            }
        }
    }
}

// Write the `n` bits of `must` / `may` / `z` at storage bit offset `off`, skipping
// every bit outside the container (LRM 11.5.1: an out-of-range write "shall only
// affect the bits that are in range"; a wholly out-of-range or x/z index writes
// nothing). Params match rt_wide_sel, value planes included.
extern "C" void rt_wide_ins(void* mmp, void* zpp, uint32_t width, int64_t base, uint32_t cwidth,
                            int64_t off, uint32_t n, uint32_t four, uint32_t has_z,
                            uint32_t unknown, const uint64_t* must, const uint64_t* may,
                            const uint64_t* z) {
    if (unknown != 0) {
        return;
    }
    auto* mm = static_cast<uint64_t*>(mmp);
    auto* zp = static_cast<uint64_t*>(zpp);
    for (uint32_t k = 0; k < n; ++k) {
        const int64_t i = off + static_cast<int64_t>(k);
        if (i < base || i >= base + static_cast<int64_t>(cwidth)) {
            continue;
        }
        const auto u = static_cast<uint64_t>(i);
        plane_bit_set(mm, u, plane_bit(must, k));
        if (four != 0) {
            plane_bit_set(mm, static_cast<uint64_t>(width) + u, plane_bit(may, k));
            if (has_z != 0) {
                plane_bit_set(zp, u, plane_bit(z, k));
            }
        }
    }
}
