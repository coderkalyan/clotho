// Multiply, divide and modulo of values wider than 128 bits (docs/design/kernel.md,
// Values): rt_wide_mul, rt_wide_udiv, rt_wide_sdiv, rt_wide_umod and rt_wide_smod, which
// the backend calls in place of LLVM's own lowering of the operation, a bit-serial loop
// for the divide and every partial product for the multiply.
//
// The routines are the schoolbook product and Knuth's Algorithm D; the oracle is
// llvm::APInt, which shares no code with them. They are called as the backend calls
// them, over random and boundary operands at every width around a word boundary, signed
// and unsigned, with guard words around every buffer.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringExtras.h>

#include "runtime/Runtime.h"

using llvm::APInt;

namespace {

// -- Operands --

// splitmix64, with a fixed seed so a failure reproduces.
struct Rng {
    uint64_t state;

    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z          = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    uint64_t below(uint64_t n) { return next() % n; }
};

// Words at the ends of their range. Algorithm D's estimate of a quotient digit is wrong
// (by one, or by more than a word holds) only for operands whose top words nearly
// agree, which random words never produce in a test's lifetime.
constexpr uint64_t kEdgeWords[] = {
    0,
    1,
    2,
    0x00000000ffffffffULL,
    0x7fffffffffffffffULL,
    0x8000000000000000ULL,
    0x8000000000000001ULL,
    0xffffffff00000000ULL,
    0xfffffffffffffffeULL,
    0xffffffffffffffffULL,
};

enum class Shape { random, edge, low_words, small_top };

// A `width`-bit operand of the given shape: random words, words from `kEdgeWords`, a
// random number of low words only (short divisors, short dividends), or random words
// whose top significant word is 1..3 (a long normalization shift).
APInt operand(Rng& rng, uint32_t width, Shape shape) {
    const uint32_t        words = (width + 63) / 64;
    std::vector<uint64_t> w(words);
    for (uint64_t& x : w) {
        x = shape == Shape::edge ? kEdgeWords[rng.below(std::size(kEdgeWords))] : rng.next();
    }
    if (shape == Shape::low_words) {
        const uint32_t keep = 1 + static_cast<uint32_t>(rng.below(words));
        std::fill(w.begin() + keep, w.end(), 0);
    }
    if (shape == Shape::small_top) {
        const uint32_t top = static_cast<uint32_t>(rng.below(words));
        std::fill(w.begin() + top + 1, w.end(), 0);
        w[top] = 1 + rng.below(3);
    }
    return APInt(width, w);
}

// Every ordered pair of these is checked at each width: the boundary values, and two
// operands of each shape.
std::vector<APInt> operands(Rng& rng, uint32_t width) {
    std::vector<APInt> v = {
        APInt(width, 0),
        APInt(width, 1),
        APInt(width, 2, /*isSigned=*/false, /*implicitTrunc=*/true), // 0 at width 1
        APInt::getAllOnes(width), // unsigned maximum, signed -1
        APInt::getSignedMinValue(width),
        APInt::getSignedMaxValue(width),
    };
    for (Shape s : { Shape::random, Shape::edge, Shape::low_words, Shape::small_top }) {
        v.push_back(operand(rng, width, s));
        v.push_back(operand(rng, width, s));
    }
    return v;
}

// -- The routines, called as the backend calls them --

using Routine = void (*)(uint64_t*, const uint64_t*, const uint64_t*, uint32_t);

constexpr size_t   kGuard     = 2;
constexpr uint64_t kGuardWord = 0xdeadbeefcafef00dULL;
constexpr uint64_t kPoison    = 0xa5a5a5a5a5a5a5a5ULL;

// The words of a value between guard words.
struct Buffer {
    std::vector<uint64_t> words;
    size_t                count;

    // A buffer for a result: every word poisoned, so one the routine fails to write shows.
    explicit Buffer(uint32_t width)
        : words(kGuard + (width + 63) / 64 + kGuard, kGuardWord), count((width + 63) / 64) {
        std::fill_n(words.begin() + kGuard, count, kPoison);
    }

    explicit Buffer(const APInt& v)
        : words(kGuard + (v.getBitWidth() + 63) / 64 + kGuard, kGuardWord),
          count((v.getBitWidth() + 63) / 64) {
        std::copy_n(v.getRawData(), count, words.begin() + kGuard);
    }

    uint64_t* data() { return words.data() + kGuard; }

    bool guards_intact() const {
        for (size_t k = 0; k < kGuard; ++k) {
            if (words[k] != kGuardWord || words[kGuard + count + k] != kGuardWord) {
                return false;
            }
        }
        return true;
    }

    APInt value(uint32_t width) { return APInt(width, llvm::ArrayRef<uint64_t>(data(), count)); }
};

// `routine` over `a` and `b`, into a result buffer poisoned beforehand so that a word the
// routine fails to write shows. Also checks what no comparison of values would: the
// guard words around all three buffers, the operands left alone, and nothing set above
// the width in the result.
APInt call(Routine routine, const APInt& a, const APInt& b) {
    const uint32_t width = a.getBitWidth();
    Buffer         out(width);
    Buffer         a_buf(a);
    Buffer         b_buf(b);
    routine(out.data(), a_buf.data(), b_buf.data(), width);

    const uint64_t above = width % 64 == 0 ? 0 : ~uint64_t{ 0 } << (width % 64);
    const bool     clean = out.guards_intact() && a_buf.guards_intact() && b_buf.guards_intact() &&
                       a_buf.value(width) == a && b_buf.value(width) == b &&
                       (out.data()[out.count - 1] & above) == 0;
    CHECK_MESSAGE(clean, "a guard word, an operand or the padding above the width changed");
    return out.value(width);
}

// All five routines on one pair, against APInt. A zero divisor gives 0 (the 2-state
// rule); APInt itself refuses to divide by it.
void check_pair(const APInt& a, const APInt& b) {
    const uint32_t width = a.getBitWidth();
    CAPTURE(width);
    CAPTURE(llvm::toString(a, 16, false));
    CAPTURE(llvm::toString(b, 16, false));
    const APInt zero(width, 0);
    const bool  div0 = b.isZero();

    CHECK(call(rt_wide_mul, a, b) == a * b);
    CHECK(call(rt_wide_udiv, a, b) == (div0 ? zero : a.udiv(b)));
    CHECK(call(rt_wide_umod, a, b) == (div0 ? zero : a.urem(b)));
    CHECK(call(rt_wide_sdiv, a, b) == (div0 ? zero : a.sdiv(b)));
    CHECK(call(rt_wide_smod, a, b) == (div0 ? zero : a.srem(b)));
}

// The same without the buffer checks, for the exhaustive sweep: how many of the four
// divide and modulo results differ from APInt's.
int divide_mismatches(const APInt& a, const APInt& b) {
    const auto run = [&](Routine routine) {
        std::vector<uint64_t> out((a.getBitWidth() + 63) / 64, kPoison);
        routine(out.data(), a.getRawData(), b.getRawData(), a.getBitWidth());
        return APInt(a.getBitWidth(), out);
    };
    return (run(rt_wide_udiv) != a.udiv(b)) + (run(rt_wide_umod) != a.urem(b)) +
           (run(rt_wide_sdiv) != a.sdiv(b)) + (run(rt_wide_smod) != a.srem(b));
}

// The widths worth a run: every one up to just past the backend's threshold, both sides
// of each word boundary after it, some arbitrary ones in between, and two much wider.
std::vector<uint32_t> widths() {
    std::vector<uint32_t> w;
    for (uint32_t k = 1; k <= 260; ++k) {
        w.push_back(k);
    }
    for (uint32_t words = 5; words <= 64; ++words) {
        for (int32_t d = -1; d <= 1; ++d) {
            w.push_back(static_cast<uint32_t>(static_cast<int32_t>(64 * words) + d));
        }
    }
    Rng rng{ 7 };
    for (int k = 0; k < 40; ++k) {
        w.push_back(261 + static_cast<uint32_t>(rng.below(4096 - 261)));
    }
    w.push_back(16384);
    w.push_back(16385);
    return w;
}

} // namespace

TEST_CASE("the wide arithmetic routines match APInt at every width around a word boundary") {
    Rng rng{ 1 };
    for (const uint32_t width : widths()) {
        const std::vector<APInt> ops = operands(rng, width);
        for (const APInt& a : ops) {
            for (const APInt& b : ops) {
                check_pair(a, b);
            }
        }
    }
}

TEST_CASE("the wide divide matches APInt on operands built to reach every branch of Algorithm D") {
    // A quotient digit estimate that is one too big (the add-back), one that starts
    // above 2^64 (the top words equal), and divisors whose top word is 1 or has its top
    // bit set (a normalization shift of 63 and of 0) are each too rare for random
    // operands to reach. These words are the alphabet where they are common, so every
    // dividend of up to 4 words over every divisor of up to 3 words is checked, at a
    // partial top word and at a whole one.
    constexpr uint64_t alphabet[] = {
        0,
        1,
        0x7fffffffffffffffULL,
        0x8000000000000000ULL,
        0xfffffffffffffffeULL,
        0xffffffffffffffffULL,
    };
    constexpr uint32_t symbols = std::size(alphabet);
    constexpr uint32_t dividends = symbols * symbols * symbols * symbols;
    constexpr uint32_t divisors  = symbols * symbols * symbols;
    for (const uint32_t width : { 251u, 256u }) {
        const auto build = [&](uint32_t len, uint32_t index) {
            std::vector<uint64_t> w((width + 63) / 64, 0);
            for (uint32_t k = 0; k < len; ++k) {
                w[k] = alphabet[index % symbols];
                index /= symbols;
            }
            return APInt(width, w);
        };
        int mismatches = 0;
        for (uint32_t i = 0; i < dividends; ++i) {
            for (uint32_t j = 1; j < divisors; ++j) {
                mismatches += divide_mismatches(build(4, i), build(3, j));
            }
        }
        CAPTURE(width);
        CHECK(mismatches == 0);
    }
}
