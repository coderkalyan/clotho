// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Wide value kernels: dynamic part-select read and write, and multiply, divide and modulo.

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

// The bits a select can address: its container [base, base+cwidth) intersected
// with the vector [0, width).
//
// The container is the whole slot for a direct select but the enclosing select's
// window for a nested one (`w[i][j]`), and when `i` is out of range that window
// lies outside the vector, or hangs over its end. Without the intersection a bit
// inside the container but outside the vector is read from, or written to,
// whatever sits next to the slot (a negative `i` made the offset negative and the
// word index a huge unsigned). LRM 11.5.1 makes such a bit out of range: x when
// read, untouched when written.
//
// `base` and the offsets are sign-extended i32 values (the backend carries a bit
// offset as an i32), so no sum below can overflow an i64. An empty intersection
// is [0, 0).
struct Span {
    int64_t lo;
    int64_t hi;
};

Span storage_span(uint32_t width, int64_t base, uint32_t cwidth) {
    assert(base >= INT32_MIN && base <= INT32_MAX && "a container base is an i32");
    const int64_t lo = std::max<int64_t>(base, 0);
    const int64_t hi = std::min<int64_t>(base + static_cast<int64_t>(cwidth), static_cast<int64_t>(width));
    if (hi <= lo) {
        return { 0, 0 };
    }
    assert(lo >= 0 && hi <= static_cast<int64_t>(width) && "the span lies inside the vector");
    return { lo, hi };
}

} // namespace

// Read the `n` bits at storage bit offset `off` of a `width`-bit slot, clamped to
// the container [base, base+cwidth) the select sits in and to the vector itself.
// `unknown` marks an x/z index, which LRM 11.5.1 makes wholly out of range. Bits
// outside read x (must 0, may 1) for a 4-state slot and 0 for a 2-state one.
//
// The three out-params are little-endian word buffers of ceil(n/64) words each --
// the same layout as the value planes, so the backend loads each one straight back
// as an i(n).
extern "C" void rt_wide_sel(const void* mmp, const void* zpp, uint32_t width, int64_t base,
                            uint32_t cwidth, int64_t off, uint32_t n, uint32_t four,
                            uint32_t has_z, uint32_t unknown, uint64_t* out_must,
                            uint64_t* out_may, uint64_t* out_z) {
    assert(off >= INT32_MIN && off <= INT32_MAX && "a select offset is an i32");
    const auto*    mm    = static_cast<const uint64_t*>(mmp);
    const auto*    zp    = static_cast<const uint64_t*>(zpp);
    const uint32_t words = (n + 63) / 64;
    const Span     span  = storage_span(width, base, cwidth);
    std::memset(out_must, 0, words * sizeof(uint64_t));
    std::memset(out_may, 0, words * sizeof(uint64_t));
    std::memset(out_z, 0, words * sizeof(uint64_t));
    for (uint32_t k = 0; k < n; ++k) {
        const int64_t i = off + static_cast<int64_t>(k);
        if (unknown != 0 || i < span.lo || i >= span.hi) {
            if (four != 0) {
                plane_bit_set(out_may, k, 1); // x: must 0, may 1
            }
            continue;
        }
        assert(i >= 0 && i < static_cast<int64_t>(width) && "an in-span bit is inside the vector");
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
// every bit outside the container and the vector (LRM 11.5.1: an out-of-range
// write "shall only affect the bits that are in range"; a wholly out-of-range or
// x/z index writes nothing). Params match rt_wide_sel, value planes included.
extern "C" void rt_wide_ins(void* mmp, void* zpp, uint32_t width, int64_t base, uint32_t cwidth,
                            int64_t off, uint32_t n, uint32_t four, uint32_t has_z,
                            uint32_t unknown, const uint64_t* must, const uint64_t* may,
                            const uint64_t* z) {
    assert(off >= INT32_MIN && off <= INT32_MAX && "a select offset is an i32");
    if (unknown != 0) {
        return;
    }
    auto*      mm   = static_cast<uint64_t*>(mmp);
    auto*      zp   = static_cast<uint64_t*>(zpp);
    const Span span = storage_span(width, base, cwidth);
    for (uint32_t k = 0; k < n; ++k) {
        const int64_t i = off + static_cast<int64_t>(k);
        if (i < span.lo || i >= span.hi) {
            continue;
        }
        assert(i >= 0 && i < static_cast<int64_t>(width) && "an in-span bit is inside the vector");
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

// -- Multiply, divide and modulo of very wide integers --
//
// LLVM expands `mul iW` into every partial product, so its object code and compile time
// grow as W^2 (12x per doubling past 4 Kbit), and it lowers `udiv iW` above 128 bits to a
// bit-serial loop (3.5 us at 256 bits, 3.3 ms at 4096). The backend calls these routines
// instead once an operand passes `wide_arith_bits`: schoolbook multiply and Knuth's
// division, both O(n^2) in the word count n, which cost the same wherever they are called.
//
// An operand or a result is a little-endian array of ceil(width / 64) words, the in-memory
// layout of an iW, with the bits above `width` zero. Every result is what the LLVM
// operation at that width gives, wrapped modulo 2^width, plus the 2-state divide rules of
// docs/design/kernel.md: a zero divisor gives 0, and the signed MIN / -1 wraps (quotient
// MIN, remainder 0). A 4-state divide handles x and a zero divisor in generated code and
// never passes one here.

namespace {

using u128 = unsigned __int128;

// The bits of the top word that belong to a `width`-bit value.
uint64_t top_mask(uint32_t width) {
    assert(width > 0);
    return width_mask(width - 64 * (plane_words(width) - 1));
}

// Whether nothing is set above `width`, which every operand and result must hold.
bool is_canonical(const uint64_t* v, uint32_t width) {
    return (v[plane_words(width) - 1] & ~top_mask(width)) == 0;
}

// The length of `v` without its leading zero words; 0 for a zero value.
uint32_t significant_words(const uint64_t* v, uint32_t words) {
    uint32_t n = words;
    while (n > 0 && v[n - 1] == 0) {
        --n;
    }
    return n;
}

// v = -v modulo 2^width, in place.
void negate(uint64_t* v, uint32_t width) {
    const uint32_t words = plane_words(width);
    uint64_t       carry = 1;
    for (uint32_t i = 0; i < words; ++i) {
        const uint64_t inverted = ~v[i];
        v[i]                    = inverted + carry;
        carry                   = (carry != 0 && inverted == ~uint64_t{ 0 }) ? 1 : 0;
    }
    v[words - 1] &= top_mask(width);
}

// out = a * b modulo 2^(64 * words). Each row of partial products stops at the top word:
// what would land above it is never formed.
void multiply_words(uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t words) {
    std::memset(out, 0, words * sizeof(uint64_t));
    for (uint32_t i = 0; i < words; ++i) {
        uint64_t carry = 0;
        for (uint32_t j = 0; i + j < words; ++j) {
            // At most (2^64 - 1)^2 + 2 (2^64 - 1) = 2^128 - 1: no overflow.
            const u128 p = static_cast<u128>(a[i]) * b[j] + out[i + j] + carry;
            out[i + j]   = static_cast<uint64_t>(p);
            carry        = static_cast<uint64_t>(p >> 64);
        }
    }
}

// The high word of the 128-bit value hi:lo shifted left by `s` in [0, 63]: the word of
// a left-shifted array that takes bits from two neighbours. A word shift of 64 - s would
// be undefined at s == 0, so the shift is done on the pair.
uint64_t shift_left_high(uint64_t hi, uint64_t lo, uint32_t s) {
    assert(s < 64);
    return static_cast<uint64_t>((((static_cast<u128>(hi) << 64) | lo) << s) >> 64);
}

// The low word of the 128-bit value hi:lo shifted right by `s` in [0, 63].
uint64_t shift_right_low(uint64_t hi, uint64_t lo, uint32_t s) {
    assert(s < 64);
    return static_cast<uint64_t>(((static_cast<u128>(hi) << 64) | lo) >> s);
}

// The `len`-word `a` divided by the single word `d`, the remainder carried down a word at
// a time. Each step's quotient is below 2^64 because the carried remainder is below `d`.
void divide_by_word(uint64_t* quot, uint64_t* rem, const uint64_t* a, uint32_t len, uint64_t d) {
    assert(d != 0);
    uint64_t carried = 0;
    for (uint32_t i = len; i > 0; --i) {
        const u128     cur = (static_cast<u128>(carried) << 64) | a[i - 1];
        const uint64_t q   = static_cast<uint64_t>(cur / d);
        if (quot != nullptr) {
            quot[i - 1] = q;
        }
        carried = static_cast<uint64_t>(cur) - q * d; // below `d`, so exact modulo 2^64
    }
    if (rem != nullptr) {
        rem[0] = carried;
    }
}

// The `len`-word `a` divided by the `n`-word `b` (n >= 2, len >= n, b's top word nonzero):
// Knuth, TAOCP vol. 2, 4.3.1, Algorithm D, in base 2^64. `quot` and `rem` are zeroed
// outside the words the result occupies.
//
// Both operands are first shifted left until the divisor's top bit is set, which is what
// makes the quotient digit estimate from the top words wrong by at most 2. `work` holds
// the shifted divisor `vn` (n words) and the shifted dividend `un` (len + 1 words), whose
// low n words end up holding the shifted remainder.
void divide_by_words(uint64_t* quot, uint64_t* rem, const uint64_t* a, uint32_t len,
                     const uint64_t* b, uint32_t n) {
    assert(n >= 2 && len >= n);
    assert(b[n - 1] != 0);
    const uint32_t        s = static_cast<uint32_t>(std::countl_zero(b[n - 1]));
    std::vector<uint64_t> work(n + len + 1);
    uint64_t*             vn = work.data();
    uint64_t*             un = work.data() + n;

    vn[0] = b[0] << s;
    for (uint32_t i = 1; i < n; ++i) {
        vn[i] = shift_left_high(b[i], b[i - 1], s);
    }
    un[0]   = a[0] << s;
    un[len] = shift_left_high(0, a[len - 1], s);
    for (uint32_t i = 1; i < len; ++i) {
        un[i] = shift_left_high(a[i], a[i - 1], s);
    }

    // One quotient digit per position of the divisor under the dividend, from the top.
    for (uint32_t j = len - n + 1; j > 0; --j) {
        uint64_t* const window = un + (j - 1); // n + 1 words: the part being divided
        assert(window[n] <= vn[n - 1] && "the part being divided is below divisor * 2^64");
        // The estimate from the top two words, corrected with the third. It starts at
        // most 2^64 + 1, so it stays wide until the correction has brought it below 2^64.
        const u128 top  = (static_cast<u128>(window[n]) << 64) | window[n - 1];
        u128       qhat = top / vn[n - 1];
        u128       rhat = top - qhat * vn[n - 1];
        while ((qhat >> 64) != 0 || qhat * vn[n - 2] > ((rhat << 64) | window[n - 2])) {
            --qhat;
            rhat += vn[n - 1];
            if ((rhat >> 64) != 0) {
                break; // the test below could no longer fail
            }
        }
        assert((qhat >> 64) == 0);
        uint64_t digit = static_cast<uint64_t>(qhat);

        // window -= digit * vn; the result is negative when the estimate was one too big.
        uint64_t borrow = 0;
        uint64_t carry  = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const u128 p = static_cast<u128>(digit) * vn[i] + carry;
            const u128 t = static_cast<u128>(window[i]) - borrow - static_cast<uint64_t>(p);
            carry        = static_cast<uint64_t>(p >> 64);
            window[i]    = static_cast<uint64_t>(t);
            borrow       = static_cast<uint64_t>(t >> 64) != 0 ? 1 : 0;
        }
        const u128 t = static_cast<u128>(window[n]) - borrow - carry;
        window[n]    = static_cast<uint64_t>(t);
        if ((t >> 64) != 0) {
            // Add the divisor back once; its carry out cancels the borrow in window[n].
            --digit;
            uint64_t add_carry = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const u128 sum = static_cast<u128>(window[i]) + vn[i] + add_carry;
                window[i]      = static_cast<uint64_t>(sum);
                add_carry      = static_cast<uint64_t>(sum >> 64);
            }
            window[n] += add_carry;
        }
        assert(window[n] == 0 && "the remainder is below the divisor");
        if (quot != nullptr) {
            quot[j - 1] = digit;
        }
    }

    if (rem != nullptr) {
        for (uint32_t i = 0; i < n; ++i) {
            rem[i] = shift_right_low(un[i + 1], un[i], s);
        }
    }
}

enum class Division { unsigned_quotient, unsigned_remainder, signed_quotient, signed_remainder };

// `out` = the quotient or remainder of `a` / `b`, at `width` bits, per the rules in the
// comment above. A signed division is an unsigned one of the magnitudes: the quotient is
// negative when the signs differ, the remainder has the dividend's sign, and MIN / -1
// falls out as 2^(width - 1) / 1 = 2^(width - 1), which is MIN again once wrapped.
void divide(uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t width,
            Division kind) {
    const uint32_t words = plane_words(width);
    const bool     is_signed =
      kind == Division::signed_quotient || kind == Division::signed_remainder;
    const bool want_quotient =
      kind == Division::unsigned_quotient || kind == Division::signed_quotient;

    std::memset(out, 0, words * sizeof(uint64_t));
    if (significant_words(b, words) == 0) {
        return; // x converts to 0 in a 2-state result
    }

    // The magnitudes of a signed pair, in scratch (a, b are the caller's).
    std::vector<uint64_t> magnitude(is_signed ? 2 * words : 0);
    const uint64_t*       dividend = a;
    const uint64_t*       divisor  = b;
    bool                  a_negative = false;
    bool                  b_negative = false;
    if (is_signed) {
        const uint32_t sign_word = (width - 1) / 64;
        const uint32_t sign_bit  = (width - 1) % 64;
        a_negative               = ((a[sign_word] >> sign_bit) & 1) != 0;
        b_negative               = ((b[sign_word] >> sign_bit) & 1) != 0;
        std::memcpy(magnitude.data(), a, words * sizeof(uint64_t));
        std::memcpy(magnitude.data() + words, b, words * sizeof(uint64_t));
        if (a_negative) {
            negate(magnitude.data(), width);
        }
        if (b_negative) {
            negate(magnitude.data() + words, width);
        }
        dividend = magnitude.data();
        divisor  = magnitude.data() + words;
    }

    const uint32_t dividend_len = significant_words(dividend, words);
    const uint32_t divisor_len  = significant_words(divisor, words);
    assert(divisor_len > 0 && "a nonzero divisor has a nonzero magnitude");
    uint64_t* const quot = want_quotient ? out : nullptr;
    uint64_t* const rem  = want_quotient ? nullptr : out;
    if (dividend_len < divisor_len) {
        // The divisor is the larger: quotient 0, remainder the dividend.
        if (rem != nullptr) {
            std::memcpy(rem, dividend, dividend_len * sizeof(uint64_t));
        }
    } else if (divisor_len == 1) {
        divide_by_word(quot, rem, dividend, dividend_len, divisor[0]);
    } else {
        divide_by_words(quot, rem, dividend, dividend_len, divisor, divisor_len);
    }

    if (is_signed && (want_quotient ? a_negative != b_negative : a_negative)) {
        negate(out, width);
    }
}

// The preconditions the five entry points share: distinct buffers (an operand may be
// both of them) and canonical operands.
void check_operands(const uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t width) {
    assert(width > 0);
    assert(out != nullptr && a != nullptr && b != nullptr);
    assert(out != a && out != b && "the result does not overlap an operand");
    assert(is_canonical(a, width) && is_canonical(b, width));
}

} // namespace

// out = a * b modulo 2^width. The low `width` bits of a product are the same whether the
// operands are read as unsigned or as two's complement, so there is one routine, as there
// is one LLVM `mul`.
extern "C" void rt_wide_mul(uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t width) {
    check_operands(out, a, b, width);
    const uint32_t words = plane_words(width);
    multiply_words(out, a, b, words);
    out[words - 1] &= top_mask(width);
    assert(is_canonical(out, width));
}

// out = a / b, or a % b, of unsigned (`udiv`, `umod`) or two's complement (`sdiv`, `smod`)
// operands; the quotient is truncated toward zero and the remainder has the dividend's
// sign.
extern "C" void rt_wide_udiv(uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t width) {
    check_operands(out, a, b, width);
    divide(out, a, b, width, Division::unsigned_quotient);
    assert(is_canonical(out, width));
}

extern "C" void rt_wide_umod(uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t width) {
    check_operands(out, a, b, width);
    divide(out, a, b, width, Division::unsigned_remainder);
    assert(is_canonical(out, width));
}

extern "C" void rt_wide_sdiv(uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t width) {
    check_operands(out, a, b, width);
    divide(out, a, b, width, Division::signed_quotient);
    assert(is_canonical(out, width));
}

extern "C" void rt_wide_smod(uint64_t* out, const uint64_t* a, const uint64_t* b, uint32_t width) {
    check_operands(out, a, b, width);
    divide(out, a, b, width, Division::signed_remainder);
    assert(is_canonical(out, width));
}
