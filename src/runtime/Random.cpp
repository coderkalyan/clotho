// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// $random, $urandom, the distribution functions and process random state.

#include "RuntimeInternal.h"

using namespace rt_internal;

// LRM 18.14.4 leaves `set_randstate` with a foreign string undefined. Clotho reports
// it: silently seeding an arbitrary state would make a run irreproducible in a way
// nothing else in the design could explain.
extern "C" void rt_proc_randstate_bad(const rt_null_site* site, uint64_t now) {
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    std::string when;
    append_finish_time(when, now);

    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " process::set_randstate() with a string it did not produce");
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column), '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " at {} the string was not 64 hex digits naming a generator state",
               when);
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " LRM 18.14.4 only defines a string obtained from get_randstate on "
                             "this implementation");
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    std::exit(1);
}
namespace {

// ---- Random state of a thread (LRM 9.7, 18.14) ----

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

} // namespace

extern "C" void rt_proc_randstate_get(rt_string* dst, const void* rng, const rt_alloc_site* site) {
    const auto* w = static_cast<const uint64_t*>(rng);
    char        buf[64];
    for (int k = 0; k < 4; ++k) {
        const uint64_t v = w[k];
        for (int d = 0; d < 16; ++d) {
            buf[(k * 16) + d] = kHexDigits[(v >> ((15 - d) * 4)) & 0xF];
        }
    }
    str_assign(dst, buf, sizeof(buf), site);
}

extern "C" void rt_proc_randstate_set(void* rng, const rt_string* s, const rt_null_site* site,
                                      uint64_t now) {
    const char*    p = str_data(const_cast<rt_string*>(s));
    const uint64_t n = str_len(const_cast<rt_string*>(s));
    uint64_t       w[4]{};
    bool           ok = n == 64;
    for (uint64_t k = 0; ok && k < 64; ++k) {
        const int v = hex_value(p[k]);
        if (v < 0) {
            ok = false;
            break;
        }
        w[k / 16] = (w[k / 16] << 4) | static_cast<uint64_t>(v);
    }
    // All-zero is not a legal xoshiro256++ state: it is the generator's fixed point,
    // so every subsequent draw would be zero. get_randstate never produces it (the
    // seeding expansion cannot yield four zero words), so it can only come from a
    // string that was not one of ours.
    if (ok && (w[0] | w[1] | w[2] | w[3]) == 0) {
        ok = false;
    }
    if (!ok) {
        rt_proc_randstate_bad(site, now);
    }
    auto* dst = static_cast<uint64_t*>(rng);
    for (int k = 0; k < 4; ++k) {
        dst[k] = w[k];
    }
}
namespace {

// One draw of the Annex N `uniform(seed, INT32_MIN, INT32_MAX)`: a real in
// [-2^31, 2^31).
double rng_uniform(uint32_t& seed) {
    constexpr double d = 0.00000011920928955078125; // 2^-23
    constexpr double a = -2147483648.0;
    constexpr double b = 2147483647.0;
    if (seed == 0) {
        seed = 259341593u;
    }
    seed = 69069u * seed + 1u;
    const uint32_t stemp = (seed >> 9) | 0x3f800000u;
    double         c     = static_cast<double>(std::bit_cast<float>(stemp));
    c                    = c + c * d;
    return (b - a) * (c - 1.0) + a;
}

// One $random draw off `seed`: advance the generator (mutating `seed`) and take
// rtl_dist_uniform's INT32_MIN/INT32_MAX branch — renormalize the uniform across
// the full 32-bit span, then floor toward negative infinity.
int32_t rng_draw(uint32_t& seed) {
    double r = (rng_uniform(seed) + 2147483648.0) / 4294967295.0;
    r        = r * 4294967296.0 - 2147483648.0;
    const long i = r >= 0 ? static_cast<long>(r) : static_cast<long>(r - 1);
    return static_cast<int32_t>(i);
}

} // namespace

extern "C" int32_t rt_random(void) {
    return rng_draw(g_random_seed);
}
namespace {

// -- Distribution routines (LRM §20.15.2) ----------------------------------------
//
// Implement the algorithms IEEE Std 1800-2023 Annex N defines, so the streams
// are identical to any conforming tool for a given seed. The float/double steps
// round, so (as with rng_uniform above) they follow the Annex's arithmetic step
// for step and are never reassociated — no fast-math. `dist_uniform_real` is the
// general two-argument `uniform(seed, start, end)`; rng_uniform above is its
// full-int32-span specialisation kept separate so the verified unseeded
// $random stream is untouched.

// Annex N `uniform(seed, start, end)`: advance the seed's 32-bit LCG once, use
// the high 23 bits of the new state as the mantissa of a float in [1, 2), and
// scale that into [start, end). A degenerate range draws over [0, 2^31 - 1].
// The state is a uint32_t, so the LCG wraps without signed overflow, and the
// float is assembled with std::bit_cast, as in rng_uniform.
double dist_uniform_real(int32_t* seed, int32_t start, int32_t end) {
    constexpr double epsilon = 0x1p-23;
    const bool       ordered = start < end;
    const double     low     = ordered ? static_cast<double>(start) : 0.0;
    const double     high    = ordered ? static_cast<double>(end) : 2147483647.0;

    uint32_t state = std::bit_cast<uint32_t>(*seed);
    if (state == 0) {
        state = 259341593u;
    }
    state = 69069u * state + 1u;
    *seed = std::bit_cast<int32_t>(state);

    double unit = static_cast<double>(std::bit_cast<float>((state >> 9) | 0x3f800000u));
    unit        = unit + unit * epsilon;
    return (high - low) * (unit - 1.0) + low;
}

// Annex N's conversion of a real draw to an integer: truncate toward zero, and
// one lower for any negative value, integral or not.
int32_t annex_floor(double r) {
    return r >= 0 ? static_cast<int32_t>(r) : static_cast<int32_t>(r - 1);
}

// $dist_uniform: an integer in [start, end] from one real draw. The draw's span
// is widened by one at an end that is not already the int32 limit, so both
// endpoints are reachable, and the result is clamped back into the range.
int32_t rtl_dist_uniform(int32_t* seed, int32_t start, int32_t end) {
    if (start >= end) {
        return start;
    }
    if (end != INT32_MAX) {
        const int32_t upper = end + 1;
        const int32_t value = annex_floor(dist_uniform_real(seed, start, upper));
        return std::clamp(value, start, upper - 1);
    }
    if (start != INT32_MIN) {
        const int32_t lower = start - 1;
        const int32_t value = annex_floor(dist_uniform_real(seed, lower, end) + 1.0);
        return std::clamp(value, lower + 1, end);
    }
    const double unit = (dist_uniform_real(seed, start, end) + 2147483648.0) / 4294967295.0;
    return annex_floor(unit * 4294967296.0 - 2147483648.0);
}

// $dist_normal: the polar method. Draw points in the square until one lands
// strictly inside the unit circle, away from the origin, then scale it.
double dist_normal_real(int32_t* seed, int32_t mean, int32_t deviation) {
    double x, y, radius;
    do {
        x      = dist_uniform_real(seed, -1, 1);
        y      = dist_uniform_real(seed, -1, 1);
        radius = x * x + y * y;
    } while (radius >= 1.0 || radius == 0.0);
    const double gauss = x * std::sqrt(-2.0 * std::log(radius) / radius);
    return gauss * static_cast<double>(deviation) + static_cast<double>(mean);
}

// $dist_exponential: inverse transform of one draw in [0, 1).
double dist_exponential_real(int32_t* seed, int32_t mean) {
    const double u = dist_uniform_real(seed, 0, 1);
    return u == 0.0 ? 0.0 : -std::log(u) * mean;
}

// $dist_poisson: count the draws whose running product stays above e^-mean.
int32_t dist_poisson_int(int32_t* seed, int32_t mean) {
    const double limit   = std::exp(-static_cast<double>(mean));
    double       product = dist_uniform_real(seed, 0, 1);
    int32_t      count   = 0;
    while (limit < product) {
        ++count;
        product = dist_uniform_real(seed, 0, 1) * product;
    }
    return count;
}

// $dist_chi_square: an odd degree contributes one squared normal draw, and each
// further pair of degrees one doubled exponential draw.
double dist_chi_square_real(int32_t* seed, int32_t degrees) {
    double sum = 0.0;
    if (degrees % 2 != 0) {
        const double z = dist_normal_real(seed, 0, 1);
        sum            = z * z;
    }
    for (int32_t pair = 2; pair <= degrees; pair += 2) {
        sum = sum + 2 * dist_exponential_real(seed, 1);
    }
    return sum;
}

// $dist_t: a normal draw over the root of a chi-square draw per degree.
double dist_t_real(int32_t* seed, int32_t degrees) {
    const double chi   = dist_chi_square_real(seed, degrees);
    const double scale = std::sqrt(chi / static_cast<double>(degrees));
    return dist_normal_real(seed, 0, 1) / scale;
}

// $dist_erlang: the log of a product of k uniform draws, scaled by mean / k.
double dist_erlang_real(int32_t* seed, int32_t k, int32_t mean) {
    double product = 1.0;
    for (int32_t i = 1; i <= k; ++i) {
        product = product * dist_uniform_real(seed, 0, 1);
    }
    return -static_cast<double>(mean) * std::log(product) / static_cast<double>(k);
}

// Round-half-away-from-zero to int32, matching the reference rtl_dist_* wrappers.
int32_t dist_round(double r) {
    if (r >= 0) {
        return static_cast<int32_t>(r + 0.5);
    }
    return -static_cast<int32_t>(-r + 0.5);
}

// Pack the updated seed (high 32) with the signed integer draw (low 32).
uint64_t dist_pack(int32_t seed, int32_t value) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) |
           static_cast<uint32_t>(value);
}

} // namespace

extern "C" uint64_t rt_dist_uniform(int32_t seed, int32_t start, int32_t end) {
    const int32_t v = rtl_dist_uniform(&seed, start, end);
    return dist_pack(seed, v);
}

extern "C" uint64_t rt_dist_normal(int32_t seed, int32_t mean, int32_t deviation) {
    const int32_t v = dist_round(dist_normal_real(&seed, mean, deviation));
    return dist_pack(seed, v);
}

extern "C" uint64_t rt_dist_exponential(int32_t seed, int32_t mean) {
    // Reference guards: a non-positive parameter yields 0 (LRM §20.15.2). The
    // seed is still consumed by the guard's own path only when the routine runs;
    // matching the reference, a guarded call leaves the seed untouched.
    if (mean <= 0) {
        return dist_pack(seed, 0);
    }
    const int32_t v = dist_round(dist_exponential_real(&seed, mean));
    return dist_pack(seed, v);
}

extern "C" uint64_t rt_dist_poisson(int32_t seed, int32_t mean) {
    if (mean <= 0) {
        return dist_pack(seed, 0);
    }
    const int32_t v = dist_poisson_int(&seed, mean);
    return dist_pack(seed, v);
}

extern "C" uint64_t rt_dist_chi_square(int32_t seed, int32_t df) {
    if (df <= 0) {
        return dist_pack(seed, 0);
    }
    const int32_t v = dist_round(dist_chi_square_real(&seed, df));
    return dist_pack(seed, v);
}

extern "C" uint64_t rt_dist_t(int32_t seed, int32_t df) {
    if (df <= 0) {
        return dist_pack(seed, 0);
    }
    const int32_t v = dist_round(dist_t_real(&seed, df));
    return dist_pack(seed, v);
}

extern "C" uint64_t rt_dist_erlang(int32_t seed, int32_t k, int32_t mean) {
    if (k <= 0) {
        return dist_pack(seed, 0);
    }
    const int32_t v = dist_round(dist_erlang_real(&seed, k, mean));
    return dist_pack(seed, v);
}

// -- $urandom seeding (LRM §18.13) --------------------------------------------
//
// splitmix64 expansion of a 64-bit selector into the 256-bit xoshiro256++ state,
// the seeding scheme recommended by the xoshiro authors. Called once per process
// (selector = process index) and on $urandom(seed) reseed (selector = user seed).
extern "C" void rt_urandom_seed(uint64_t* state, uint64_t seed) {
    uint64_t x = seed;
    for (int i = 0; i < 4; ++i) {
        uint64_t z = (x += 0x9e3779b97f4a7c15ull);
        z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z          = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        state[i]   = z ^ (z >> 31);
    }
}

// -- randc cyclic randomization (LRM §18.6) -----------------------------------
//
// One cyclic draw against a randc property's 256-bit remaining-value bitmap. The
// bitmap is fixed at four words because the frontend caps a randc at 8 bits, so
// `nvals` never needs a fifth; the cap is what keeps the exact cyclic semantics
// affordable as a bitmap instead of a shuffled permutation array.
extern "C" uint32_t rt_randc_next(uint64_t* seen, uint32_t nvals, uint64_t* rng) {
    assert(seen != nullptr && "randc needs a seen bitmap");
    assert(rng != nullptr && "randc needs an object RNG");
    assert(nvals >= 2 && "randc has at least two values");
    assert(nvals <= 256 && "randc is capped at 8 bits");
    assert(std::has_single_bit(nvals) && "randc value count is a power of two");

    // The bits each word actually owns: full below `nvals`, empty above it. A
    // power-of-two `nvals` is either a multiple of 64 or smaller than one word,
    // so the shift below is never the UB `1 << 64`.
    uint64_t live[4];
    for (uint32_t w = 0; w < 4; ++w) {
        const uint32_t lo = 64 * w;
        live[w] = nvals <= lo ? 0ull : (nvals - lo >= 64 ? ~0ull : (1ull << (nvals - lo)) - 1);
    }

    uint32_t remaining = 0;
    for (uint32_t w = 0; w < 4; ++w) {
        remaining += static_cast<uint32_t>(std::popcount(~seen[w] & live[w]));
    }
    if (remaining == 0) {
        // Every value has been handed out, so this draw opens a fresh permutation
        // (LRM §18.6). Clearing here rather than after the last draw of a cycle
        // keeps the bitmap a pure record of what this cycle consumed.
        for (uint32_t w = 0; w < 4; ++w) {
            seen[w] = 0;
        }
        remaining = nvals;
    }
    assert(remaining >= 1 && "randc has a candidate after the cycle check");
    assert(remaining <= nvals && "randc candidate count cannot exceed the value count");

    // Lemire multiply-shift of the draw's high 32 bits onto [0, remaining) --
    // the same mapping the JIT inlines for $urandom_range (the UrandomRange arm
    // of ir/llvm/Inst.cpp's lower_inst), so a bounded index is produced one way
    // across all of clotho. Exactly one draw per call, whether or not the cycle
    // restarted above.
    const uint64_t draw32 = xoshiro_next(rng) >> 32;
    auto           pick   = static_cast<uint32_t>((draw32 * remaining) >> 32);
    assert(pick < remaining && "randc pick is within the candidate count");

    // `pick` counts clear bits, so walk the words spending it on each word's
    // clear count, then take the pick-th clear bit of the word it lands in.
    for (uint32_t w = 0; w < 4; ++w) {
        const uint64_t free_bits = ~seen[w] & live[w];
        const auto     count     = static_cast<uint32_t>(std::popcount(free_bits));
        if (pick >= count) {
            pick -= count;
            continue;
        }
        // Drop the `pick` lowest clear bits; the lowest one left is the choice.
        // Bounded by 63 iterations of one ANDN, which a per-randomize draw can
        // afford far more cheaply than maintaining a permutation array.
        uint64_t rest = free_bits;
        for (uint32_t k = 0; k < pick; ++k) {
            rest &= rest - 1;
        }
        assert(rest != 0 && "randc word had the pick-th clear bit");
        const auto bit = static_cast<uint32_t>(std::countr_zero(rest));
        seen[w] |= 1ull << bit;
        return 64 * w + bit;
    }

    // Unreachable: `remaining` counted the clear bits the loop just spent.
    assert(false && "randc found no clear bit");
    return 0;
}
