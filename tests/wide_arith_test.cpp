// Multiply, divide, modulo and power of values wider than 128 bits (docs/design/kernel.md,
// Values). Above `wide_arith_bits` the backend calls rt_wide_mul, rt_wide_udiv,
// rt_wide_sdiv, rt_wide_umod and rt_wide_smod instead of asking LLVM for the operation:
// LLVM lowers a wide divide to a bit-serial loop and a wide multiply to every partial
// product. `**` is one call, rt_pow, at every width above one bit.
//
// Two layers, each checked against an oracle that shares no code with the routines
// (llvm::APInt; the routines are the schoolbook product and Knuth's Algorithm D; for
// rt_pow also Table 11-4 over plain integers and slang's SVInt::pow):
//
//   - the routines themselves, over random and boundary operands at every width around
//     a word boundary, signed and unsigned, with guard words around every buffer;
//   - SV designs through the JIT, 2- and 4-state, with x operands and a zero divisor.
//     Their expected text is computed from APInt in the test; the semantics it encodes
//     (truncating signed division, the remainder taking the dividend's sign, MIN / -1
//     wrapping) were first confirmed against iverilog 13 on 200- and 300-bit values.

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/raw_ostream.h>
#include <slang/numeric/SVInt.h>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "runtime/Runtime.h"

using namespace clotho;
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

// -- Power --
//
// rt_pow takes two definite values and returns whether the result is x (a zero base to a
// negative exponent); the generated code decides x and z bits in an operand before it
// calls. Three oracles, none of which shares code with the routine:
//
//   - Table 11-4 over plain integers, for every operand pair of up to 4 bits;
//   - Table 11-4 over APInt, with the product by repeated multiplication for a small
//     exponent and by squaring in the test for a large one, over random and boundary
//     operands at every width around a word boundary;
//   - slang's SVInt::pow, which the constant evaluator folds `**` with, so that a
//     constant and the same value in a variable cannot disagree.

namespace {

struct Power {
    APInt value;
    bool  is_x = false;
};

uint32_t pow_flags(bool base_signed, bool exp_signed) {
    return (base_signed ? uint32_t{ RT_POW_BASE_SIGNED } : 0u) | (exp_signed ? uint32_t{ RT_POW_EXP_SIGNED } : 0u);
}

// rt_pow as the backend calls it, with the buffer checks of `call`.
Power call_pow(const APInt& base, const APInt& exp, bool base_signed, bool exp_signed) {
    const uint32_t width = base.getBitWidth();
    Buffer         out(width);
    Buffer         base_buf(base);
    Buffer         exp_buf(exp);
    const uint32_t is_x =
      rt_pow(out.data(), base_buf.data(), exp_buf.data(), width, exp.getBitWidth(), pow_flags(base_signed, exp_signed));

    const uint64_t above = width % 64 == 0 ? 0 : ~uint64_t{ 0 } << (width % 64);
    const bool     clean = out.guards_intact() && base_buf.guards_intact() && exp_buf.guards_intact() &&
                       base_buf.value(width) == base && exp_buf.value(exp.getBitWidth()) == exp &&
                       (out.data()[out.count - 1] & above) == 0;
    CHECK_MESSAGE(clean, "a guard word, an operand or the padding above the width changed");
    return { out.value(width), is_x != 0 };
}

// The product of `exp` copies of `base`, wrapped at the base's width.
APInt power_by_multiplication(const APInt& base, uint64_t exp) {
    APInt r(base.getBitWidth(), 1);
    for (uint64_t k = 0; k < exp; ++k) {
        r *= base;
    }
    return r;
}

// The same by squaring, for an exponent too large to count.
APInt power_by_squaring(APInt base, APInt exp) {
    APInt r(base.getBitWidth(), 1);
    while (!exp.isZero()) {
        if (exp[0]) {
            r *= base;
        }
        base *= base;
        exp.lshrInPlace(1);
    }
    return r;
}

// Table 11-4, row by row. The exponent is negative when it is signed and its top bit is
// set; the base is -1 when it is signed and all ones (a signed 1-bit base of 1 is -1).
Power table_11_4(const APInt& base, const APInt& exp, bool base_signed, bool exp_signed) {
    const uint32_t width    = base.getBitWidth();
    const APInt    one(width, 1);
    const APInt    zero(width, 0);
    const bool     negative = exp_signed && exp.isNegative();
    const bool     odd      = exp[0];
    const bool     minus_one = base_signed && base.isAllOnes();

    if (exp.isZero()) {
        return { one, false };
    }
    if (minus_one) {
        return { odd ? APInt::getAllOnes(width) : one, false };
    }
    if (base.isZero()) {
        return { zero, negative };
    }
    if (base.isOne()) {
        return { one, false };
    }
    if (negative) {
        return { zero, false };
    }
    const APInt result = exp.getActiveBits() <= 6 ? power_by_multiplication(base, exp.getZExtValue())
                                                  : power_by_squaring(base, exp);
    return { result, false };
}

// slang's SVInt::pow on the same operands.
Power slang_power(const APInt& base, const APInt& exp, bool base_signed, bool exp_signed) {
    const auto literal_of = [](const APInt& v, bool is_signed) {
        return slang::SVInt::fromString(std::to_string(v.getBitWidth()) + (is_signed ? "'sh" : "'h") +
                                        llvm::toString(v, 16, false));
    };
    const slang::SVInt r = literal_of(base, base_signed).pow(literal_of(exp, exp_signed));
    if (r.hasUnknown()) {
        return { APInt(base.getBitWidth(), 0), true };
    }
    return { APInt(base.getBitWidth(), llvm::ArrayRef<uint64_t>(r.getRawPtr(), r.getNumWords())), false };
}

// A `width`-bit pattern as the integer it stands for.
__int128 integer_of(uint64_t bits, uint32_t width, bool is_signed) {
    if (is_signed && ((bits >> (width - 1)) & 1) != 0) {
        return static_cast<__int128>(bits) - (static_cast<__int128>(1) << width);
    }
    return bits;
}

// Table 11-4 on plain integers, for operands of at most 4 bits: the exact power, wrapped.
Power table_11_4_integers(uint64_t base_bits, uint32_t width, uint64_t exp_bits, uint32_t exp_width, bool base_signed,
                          bool exp_signed) {
    const __int128 b = integer_of(base_bits, width, base_signed);
    const __int128 e = integer_of(exp_bits, exp_width, exp_signed);
    __int128       r = 1;
    bool           is_x = false;
    if (e == 0) {
        r = 1;
    } else if (e > 0) {
        if (b == -1) {
            r = e % 2 == 1 ? -1 : 1;
        } else if (b == 0 || b == 1) {
            r = b;
        } else {
            for (__int128 k = 0; k < e; ++k) {
                r *= b; // 15 ** 15 fits.
            }
        }
    } else if (b == -1) {
        r = (-e) % 2 == 1 ? -1 : 1;
    } else if (b == 0) {
        r    = 0;
        is_x = true;
    } else {
        r = b == 1 ? 1 : 0;
    }
    const uint64_t mask = (uint64_t{ 1 } << width) - 1;
    return { APInt(width, static_cast<uint64_t>(r) & mask), is_x };
}

void check_power(const APInt& base, const APInt& exp, bool base_signed, bool exp_signed) {
    CAPTURE(base.getBitWidth());
    CAPTURE(exp.getBitWidth());
    CAPTURE(base_signed);
    CAPTURE(exp_signed);
    CAPTURE(llvm::toString(base, 16, false));
    CAPTURE(llvm::toString(exp, 16, false));
    const Power want   = table_11_4(base, exp, base_signed, exp_signed);
    const Power got    = call_pow(base, exp, base_signed, exp_signed);
    const Power folded = slang_power(base, exp, base_signed, exp_signed);
    CHECK(got.is_x == want.is_x);
    CHECK(got.value == want.value);
    CHECK(folded.is_x == want.is_x);
    CHECK(folded.value == want.value);
}

} // namespace

TEST_CASE("rt_pow matches Table 11-4 for every pair of operands of up to 4 bits") {
    int pairs = 0;
    for (uint32_t width = 1; width <= 4; ++width) {
        for (uint32_t exp_width = 1; exp_width <= 4; ++exp_width) {
            for (const bool base_signed : { false, true }) {
                for (const bool exp_signed : { false, true }) {
                    for (uint64_t b = 0; b < (uint64_t{ 1 } << width); ++b) {
                        for (uint64_t e = 0; e < (uint64_t{ 1 } << exp_width); ++e) {
                            const Power want = table_11_4_integers(b, width, e, exp_width, base_signed, exp_signed);
                            const Power got  = call_pow(APInt(width, b), APInt(exp_width, e), base_signed, exp_signed);
                            CAPTURE(width);
                            CAPTURE(exp_width);
                            CAPTURE(base_signed);
                            CAPTURE(exp_signed);
                            CAPTURE(b);
                            CAPTURE(e);
                            CHECK(got.is_x == want.is_x);
                            CHECK(got.value == want.value);
                            ++pairs;
                        }
                    }
                }
            }
        }
    }
    // Both signs of each operand, over 2 + 4 + 8 + 16 patterns of each width.
    CHECK(pairs == 4 * 30 * 30);
}

TEST_CASE("rt_pow matches Table 11-4 over APInt and slang at every width around a word boundary") {
    Rng                         rng{ 11 };
    const std::vector<uint32_t> base_widths = { 1,   2,   3,   4,   5,   7,   8,   9,   16,  31,  32,  33,  63,  64,  65,
                                                66,  100, 127, 128, 129, 130, 191, 192, 193, 255, 256, 257, 321, 512, 1000 };
    const std::vector<uint32_t> exp_widths  = { 1, 2, 3, 6, 8, 31, 32, 33, 64, 65, 129 };
    for (const uint32_t width : base_widths) {
        const std::vector<APInt> bases = operands(rng, width);
        for (const uint32_t exp_width : exp_widths) {
            std::vector<APInt> exps = operands(rng, exp_width);
            // Exponents a loop can count as well as the ones it cannot.
            for (const uint64_t small : { 3u, 5u, 17u, 40u }) {
                exps.push_back(APInt(exp_width, small, /*isSigned=*/false, /*implicitTrunc=*/true));
            }
            for (const APInt& base : bases) {
                for (const APInt& exp : exps) {
                    for (const bool base_signed : { false, true }) {
                        for (const bool exp_signed : { false, true }) {
                            check_power(base, exp, base_signed, exp_signed);
                        }
                    }
                }
            }
        }
    }
}

// -- SV designs through the JIT --

namespace {

LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    return r;
}

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_ok(source);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[4096];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

std::string emit_ir(std::string_view source) {
    const LowerResult        r = lower_ok(source);
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*r.module, os));
    return text;
}

// What `%h` prints for a value: lower case, padded to ceil(width / 4) digits.
std::string hex_digits(const APInt& v) {
    std::string s = llvm::toString(v, 16, false);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return std::string((v.getBitWidth() + 3) / 4 - s.size(), '0') + s;
}

std::string literal(const APInt& v) {
    return std::to_string(v.getBitWidth()) + "'h" + hex_digits(v);
}

std::string all_x(uint32_t width) {
    return std::string((width + 3) / 4, 'x');
}

// One operand pair of an SV design. `x_a` / `x_b` make an operand all-x (4-state only).
struct Pair {
    APInt a;
    APInt b;
    bool  x_a = false;
    bool  x_b = false;
};

// The four kinds of variable a design declares: `bit` and `logic`, unsigned and signed.
struct Kind {
    const char* type;
    const char* prefix;
    bool        four;
    bool        is_signed;
};

constexpr Kind kKinds[] = {
    { "bit", "u2", false, false },
    { "bit signed", "s2", false, true },
    { "logic", "u4", true, false },
    { "logic signed", "s4", true, true },
};

bool skipped(const Kind& k, const Pair& p) {
    return !k.four && (p.x_a || p.x_b);
}

// A design that prints a * b, a / b and a % b, as `%h`, for each pair and each kind of
// variable, twice: as the operands of $display, where a division by zero reads x even for
// 2-state operands (LRM 11.4.3; the expression is not yet converted to a 2-state type),
// and assigned to variables of the operands' own type, where a 2-state result is 0. The
// two contexts lower to different IR for 2-state operands. Values are assigned at run
// time and read through variables, so the frontend cannot fold the operation and the
// routine always runs.
std::string design(uint32_t width, const std::vector<Pair>& pairs) {
    const std::string range = " [" + std::to_string(width - 1) + ":0] ";
    std::string       s     = "module m;\n";
    for (const Kind& k : kKinds) {
        s += std::string("  ") + k.type + range + k.prefix + "a, " + k.prefix + "b, " + k.prefix + "m, " +
             k.prefix + "d, " + k.prefix + "r;\n";
    }
    s += "  initial begin\n";
    for (const Kind& k : kKinds) {
        const std::string a = std::string(k.prefix) + "a";
        const std::string b = std::string(k.prefix) + "b";
        const std::string m = std::string(k.prefix) + "m";
        const std::string d = std::string(k.prefix) + "d";
        const std::string r = std::string(k.prefix) + "r";
        for (const Pair& p : pairs) {
            if (skipped(k, p)) {
                continue;
            }
            s += "    " + a + " = " + (p.x_a ? "'x" : literal(p.a)) + "; " + b + " = " +
                 (p.x_b ? "'x" : literal(p.b)) + ";\n";
            s += "    $display(\"%h %h %h\", " + a + " * " + b + ", " + a + " / " + b + ", " + a + " % " +
                 b + ");\n";
            s += "    " + m + " = " + a + " * " + b + "; " + d + " = " + a + " / " + b + "; " + r + " = " +
                 a + " % " + b + ";\n";
            s += "    $display(\"%h %h %h\", " + m + ", " + d + ", " + r + ");\n";
        }
    }
    return s + "  end\nendmodule\n";
}

// What `design` prints for the same pairs, one line each, with the kind and pair it is for.
struct Line {
    std::string label;
    std::string text;
};

// `v` as `%h` prints it, or all x.
std::string shown(bool is_x, const APInt& v) {
    return is_x ? all_x(v.getBitWidth()) : hex_digits(v);
}

std::vector<Line> expected(uint32_t width, const std::vector<Pair>& pairs) {
    std::vector<Line> lines;
    for (const Kind& k : kKinds) {
        for (size_t index = 0; index < pairs.size(); ++index) {
            const Pair& p = pairs[index];
            if (skipped(k, p)) {
                continue;
            }
            const bool x_operand = p.x_a || p.x_b;
            const bool div0      = !x_operand && p.b.isZero();
            APInt      quot(width, 0);
            APInt      rem(width, 0);
            if (!x_operand && !div0) {
                quot = k.is_signed ? p.a.sdiv(p.b) : p.a.udiv(p.b);
                rem  = k.is_signed ? p.a.srem(p.b) : p.a.urem(p.b);
            }
            const std::string label = std::string(k.type) + ", pair " + std::to_string(index);
            const std::string mul   = shown(x_operand, p.a * p.b);
            // As $display operands: x for an x operand or a zero divisor, in any type.
            const bool as_expr = x_operand || div0;
            lines.push_back({ label + ", expression",
                              mul + " " + shown(as_expr, quot) + " " + shown(as_expr, rem) });
            // Assigned back: a 2-state target turns the x of a zero divisor into 0.
            const bool as_assigned = x_operand || (div0 && k.four);
            lines.push_back({ label + ", assignment",
                              mul + " " + shown(as_assigned, quot) + " " + shown(as_assigned, rem) });
        }
    }
    return lines;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    for (size_t from = 0; from < text.size();) {
        const size_t end = text.find('\n', from);
        lines.push_back(text.substr(from, end - from));
        from = end == std::string::npos ? text.size() : end + 1;
    }
    return lines;
}

// The first `n` characters of a possibly very long line, for a failure message.
std::string abbreviated(const std::string& s, size_t n = 90) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

// The pairs every width gets: random, a one-word divisor, a divisor larger than the
// dividend, a divisor whose top word is tiny, equal operands, MIN / -1 and MIN / 1, a
// zero divisor, all-ones, and (4-state) an x in either operand.
std::vector<Pair> pairs_for(uint32_t width) {
    Rng        rng{ width };
    const APInt big_a = operand(rng, width, Shape::random);
    const APInt big_b = operand(rng, width, Shape::random);
    const APInt min   = APInt::getSignedMinValue(width);
    return {
        { big_a, big_b },
        { big_a, operand(rng, width, Shape::low_words) },
        { operand(rng, width, Shape::low_words), big_b },
        { big_a, operand(rng, width, Shape::small_top) },
        { big_a, big_a },
        { min, APInt::getAllOnes(width) },
        { min, APInt(width, 1) },
        { big_a, APInt(width, 0) },
        { APInt::getAllOnes(width), APInt::getAllOnes(width) },
        { big_a, big_b, /*x_a=*/true },
        { big_a, big_b, /*x_a=*/false, /*x_b=*/true },
    };
}

// Run `design` for these pairs and compare each line it prints with `expected`.
void check_design(uint32_t width, const std::vector<Pair>& pairs) {
    CAPTURE(width);
    const std::vector<Line>        want = expected(width, pairs);
    const std::vector<std::string> got  = split_lines(run_capture(design(width, pairs)));
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        CHECK_MESSAGE(got[i] == want[i].text, want[i].label, ": got ", abbreviated(got[i]),
                      ", want ", abbreviated(want[i].text));
    }
}

} // namespace

TEST_CASE("wide multiply, divide and modulo through the JIT match APInt, 2- and 4-state") {
    // Just past the backend's threshold, a whole number of words, a partial top word, and
    // a size of many words. Each is compiled with 88 divide sites of its own, which is
    // what the test costs; the routines themselves are covered much harder above.
    for (const uint32_t width : { 129u, 192u, 200u, 256u, 1000u }) {
        check_design(width, pairs_for(width));
    }
}

TEST_CASE("a 16384-bit multiply, divide and modulo compile and run") {
    // Before the routines, LLVM compiled one 16384-bit multiply in 65-84 s of CPU and one
    // modulo in 140-150 s (a divide in 4-7 s), per site, so these 24 sites took tens of
    // minutes and ran the divide bit by bit; now each is a call and the test is a few
    // seconds. One pair of random operands, in all four kinds and both contexts.
    check_design(16384, { pairs_for(16384)[0] });
}

TEST_CASE("an operation above 128 bits calls the runtime and one at or below it stays native") {
    // The threshold is a codegen choice with no observable difference, so it is read off
    // the IR: 128 bits is two registers of native arithmetic, 129 is the first width that
    // goes through rt_wide_*. The operands come from a plusarg so nothing folds.
    const auto source = [](std::string_view width) {
        std::string s = R"(
            module m;
              logic [WIDTH-1:0] a, b, c, d, e;
              bit   [WIDTH-1:0] ta, tb, tc, td;
              int n;
              initial begin
                if (!$value$plusargs("N=%d", n)) n = 5;
                a = n; b = n + 1; ta = n; tb = n + 1;
                c = a * b; d = a / b; e = a % b;
                tc = ta * tb; td = ta / tb;
                $display("%h %h %h %h %h", c, d, e, tc, td);
              end
            endmodule
        )";
        for (size_t p = s.find("WIDTH"); p != std::string::npos; p = s.find("WIDTH", p)) {
            s.replace(p, 5, width);
            p += width.size();
        }
        return s;
    };
    const std::string narrow = emit_ir(source("128"));
    const std::string wide   = emit_ir(source("129"));
    CHECK(narrow.find("rt_wide_") == std::string::npos);
    CHECK(narrow.find("mul i128") != std::string::npos);
    CHECK(wide.find("rt_wide_mul") != std::string::npos);
    CHECK(wide.find("rt_wide_udiv") != std::string::npos);
    CHECK(wide.find("rt_wide_umod") != std::string::npos);
    CHECK(wide.find("mul i129") == std::string::npos);
}

TEST_CASE("a 200-bit power through the JIT matches repeated multiplication in both domains") {
    // The values of a power above the backend's multiply threshold, a 2-state one and a
    // 4-state one, and the all-x of an x base. Oracle: APInt, by repeated multiplication.
    constexpr uint32_t width = 200;
    Rng                rng{ 99 };
    const APInt        base = operand(rng, width, Shape::random);
    const auto         pow  = [&](uint32_t e) {
        APInt r(width, 1);
        for (uint32_t k = 0; k < e; ++k) {
            r *= base;
        }
        return hex_digits(r);
    };
    const std::string out = run_capture(
      "module m;\n"
      "  logic [199:0] la, lr;\n"
      "  bit   [199:0] ba, br;\n"
      "  int e;\n"
      "  initial begin\n"
      "    la = " + literal(base) + "; ba = " + literal(base) + ";\n"
      "    e = 0;  lr = la ** e; br = ba ** e; $display(\"%h %h\", lr, br);\n"
      "    e = 1;  lr = la ** e; br = ba ** e; $display(\"%h %h\", lr, br);\n"
      "    e = 5;  lr = la ** e; br = ba ** e; $display(\"%h %h\", lr, br);\n"
      "    e = 17; lr = la ** e; br = ba ** e; $display(\"%h %h\", lr, br);\n"
      "    la = 'x; lr = la ** 3; $display(\"%h\", lr);\n"
      "  end\n"
      "endmodule\n");
    std::string want;
    for (const uint32_t e : { 0u, 1u, 5u, 17u }) {
        want += pow(e) + " " + pow(e) + "\n";
    }
    CHECK(out == want + all_x(width) + "\n");
}

// What a power's operands are, independently of each other: the exponent is
// self-determined, so it keeps its own domain and its own sign (LRM 11.4.3).
namespace {

struct PowKind {
    const char* type;
    bool        four;
    bool        is_signed;
};

constexpr PowKind kPowKinds[] = {
    { "bit", false, false },
    { "bit signed", false, true },
    { "logic", true, false },
    { "logic signed", true, true },
};

struct PowPair {
    APInt base;
    APInt exp;
    bool  x_base = false;
    bool  x_exp  = false;
};

// For each pair and each combination of kinds: assign the operands at run time and print
// the power assigned to a variable of the base's own kind. A 2-state result turns the x of
// `0 ** negative` into 0 (LRM 6.3.1); a 4-state one keeps it.
std::string power_design(uint32_t width, uint32_t exp_width, const std::vector<PowPair>& pairs) {
    std::string s = "module m;\n";
    for (size_t k = 0; k < std::size(kPowKinds); ++k) {
        s += std::string("  ") + kPowKinds[k].type + " [" + std::to_string(width - 1) + ":0] a" + std::to_string(k) + ", r" +
             std::to_string(k) + ";\n";
        s += std::string("  ") + kPowKinds[k].type + " [" + std::to_string(exp_width - 1) + ":0] e" + std::to_string(k) + ";\n";
    }
    s += "  initial begin\n";
    for (size_t k = 0; k < std::size(kPowKinds); ++k) {
        for (size_t j = 0; j < std::size(kPowKinds); ++j) {
            for (const PowPair& p : pairs) {
                if ((p.x_base && !kPowKinds[k].four) || (p.x_exp && !kPowKinds[j].four)) {
                    continue;
                }
                const std::string a = "a" + std::to_string(k);
                const std::string e = "e" + std::to_string(j);
                const std::string r = "r" + std::to_string(k);
                s += "    " + a + " = " + (p.x_base ? "'x" : literal(p.base)) + "; " + e + " = " + (p.x_exp ? "'x" : literal(p.exp)) +
                     "; " + r + " = " + a + " ** " + e + "; $display(\"%h\", " + r + ");\n";
            }
        }
    }
    return s + "  end\nendmodule\n";
}

std::vector<std::string> power_expected(uint32_t width, const std::vector<PowPair>& pairs) {
    std::vector<std::string> lines;
    for (size_t k = 0; k < std::size(kPowKinds); ++k) {
        for (size_t j = 0; j < std::size(kPowKinds); ++j) {
            for (const PowPair& p : pairs) {
                if ((p.x_base && !kPowKinds[k].four) || (p.x_exp && !kPowKinds[j].four)) {
                    continue;
                }
                const Power want = table_11_4(p.base, p.exp, kPowKinds[k].is_signed, kPowKinds[j].is_signed);
                const bool  is_x = p.x_base || p.x_exp || want.is_x;
                lines.push_back(is_x && kPowKinds[k].four ? all_x(width) : hex_digits(is_x ? APInt(width, 0) : want.value));
            }
        }
    }
    return lines;
}

} // namespace

TEST_CASE("a power through the JIT matches Table 11-4 for every domain and sign of its operands") {
    // A multiword and a one-word base, with an exponent that fits a word and one that
    // does not. The exponents are the Table 11-4 rows: zero, positive, and the patterns
    // that are negative to a signed exponent and huge to an unsigned one.
    for (const auto [width, exp_width] : { std::pair{ 8u, 1u }, std::pair{ 65u, 1u }, std::pair{ 8u, 5u }, std::pair{ 8u, 70u },
                                           std::pair{ 65u, 5u }, std::pair{ 200u, 70u } }) {
        Rng                  rng{ width * 1000 + exp_width };
        std::vector<PowPair> pairs;
        for (const APInt& base : { APInt(width, 0), APInt(width, 1), APInt::getAllOnes(width), APInt(width, 2),
                                   operand(rng, width, Shape::random) }) {
            for (const APInt& exp : { APInt(exp_width, 0), APInt(exp_width, 1), APInt(exp_width, 3, false, true),
                                      APInt(exp_width, 4, false, true), APInt::getAllOnes(exp_width),
                                      APInt::getAllOnes(exp_width) - 1, APInt::getSignedMinValue(exp_width) }) {
                pairs.push_back({ base, exp });
            }
        }
        pairs.push_back({ APInt(width, 3), APInt(exp_width, 2, false, true), /*x_base=*/true });
        pairs.push_back({ APInt(width, 3), APInt(exp_width, 2, false, true), /*x_base=*/false, /*x_exp=*/true });
        pairs.push_back({ APInt(width, 0), APInt(exp_width, 0), /*x_base=*/true });

        CAPTURE(width);
        CAPTURE(exp_width);
        const std::vector<std::string> want = power_expected(width, pairs);
        const std::vector<std::string> got  = split_lines(run_capture(power_design(width, exp_width, pairs)));
        REQUIRE(got.size() == want.size());
        for (size_t i = 0; i < want.size(); ++i) {
            CHECK_MESSAGE(got[i] == want[i], "line ", i, ": got ", abbreviated(got[i]), ", want ", abbreviated(want[i]));
        }
    }
}
