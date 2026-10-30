// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Value formatting for $display, $sformatf and friends.

#include "RuntimeInternal.h"

using namespace rt_internal;

namespace rt_internal {

TimeFormat timeformat;

// Render a tick-valued time `ticks` per the current $timeformat, appending the
// unpadded number + suffix to `out` (LRM §20.4.3). The tick value scales to
// femtoseconds, then to the display unit (10^units s); since fs = 10^-15 s, a
// display unit of 10^units s is 10^(units+15) fs. The number is printed with
// `precision` fractional digits. Field justification (to $timeformat's
// min_width or an explicit %t width) is the caller's job. `negative` makes the
// time minus `ticks`, written with a leading `-` that counts toward the field
// width, as a negative %d does.
void render_time(std::string& out, uint64_t ticks, bool negative) {
    const double fs      = static_cast<double>(ticks) * static_cast<double>(timeformat.tick_fs);
    double       divisor = 1.0;
    for (int32_t k = 0; k < timeformat.units + 15; ++k) {
        divisor *= 10.0;
    }
    const double value = negative ? -(fs / divisor) : fs / divisor;

    const int32_t prec = timeformat.precision < 0 ? 0 : timeformat.precision;
    char          numbuf[64];
    std::snprintf(numbuf, sizeof(numbuf), "%.*f", prec, value);

    out += numbuf;
    out += timeformat.suffix;
}

// A runtime formatting invariant broke: with a COMPILE-TIME format the frontend
// validates every specifier before it reaches the runtime, so these paths are
// unreachable from lowered code. Abort loudly rather than emit silently-wrong
// output. A format string built at run time is validated by dynfmt_fatal
// instead -- there is nothing broken about a design that writes "%q" into one.
[[noreturn]] void format_fatal(const std::string& what) {
    std::fprintf(stderr, "clotho runtime: %s\n", what.c_str());
    std::abort();
}

// The `bits`-wide bit group of little-endian word array `p` (`words` words) at
// bit offset `off`, read straight through the word boundary. `bits` <= 64.
uint64_t bit_group(const uint64_t* p, uint32_t words, uint32_t off, uint32_t bits) {
    const uint32_t wi = off / 64;
    const uint32_t sh = off % 64;
    uint64_t       g  = p[wi] >> sh;
    if (sh != 0 && wi + 1 < words) {
        g |= p[wi + 1] << (64 - sh);
    }
    const uint64_t m = bits >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << bits) - 1);
    return g & m;
}

// Render the plane arrays as ceil(width/bits) digits, most-significant first.
// `bits` is 1 (binary), 3 (octal) or 4 (hex). Unknown digits follow LRM
// §21.2.1.3 casing: lowercase x/z when every in-range bit of the group is x / z,
// uppercase X/Z when only some are (Z only when the group has no x bits). The
// top group clips to the operand width so its "every bit" test ignores
// out-of-range bits. `words` is plane_words(width).
void render_digits(std::string& out, const uint64_t* val, const uint64_t* xz, const uint64_t* z, uint32_t words,
                   uint32_t width, uint32_t bits) {
    if (width == 0) {
        width = 1;
    }
    static constexpr char kHex[]   = "0123456789abcdef";
    const uint32_t        digits   = (width + bits - 1) / bits;
    for (uint32_t d = digits; d-- > 0;) {
        const uint32_t off       = d * bits;
        const uint32_t grp_bits  = std::min(bits, width - off);
        const uint64_t gm        = (uint64_t{ 1 } << grp_bits) - 1;
        const uint64_t dxz       = bit_group(xz, words, off, bits) & gm;
        if (dxz == 0) {
            out += kHex[bit_group(val, words, off, bits) & gm];
            continue;
        }
        const uint64_t dz = bit_group(z, words, off, bits) & gm;
        const uint64_t dx = dxz & ~dz;
        if (dx != 0) {
            out += dx == gm ? 'x' : 'X';
        } else {
            out += dz == gm ? 'z' : 'Z';
        }
    }
}

// Decimal string of the unsigned magnitude in little-endian word array `w`
// (`words` words, zero-extended above the value width so no masking is needed).
// Repeated divmod by 1e9 over 32-bit limbs; "0" for a zero value.
std::string wide_udec(const uint64_t* w, uint32_t words) {
    std::vector<uint32_t> limb(2 * words);
    for (uint32_t i = 0; i < words; ++i) {
        limb[2 * i]     = static_cast<uint32_t>(w[i]);
        limb[2 * i + 1] = static_cast<uint32_t>(w[i] >> 32);
    }
    while (limb.size() > 1 && limb.back() == 0) {
        limb.pop_back();
    }
    std::string s;
    for (;;) {
        bool zero = true;
        for (const uint32_t l : limb) {
            if (l != 0) {
                zero = false;
                break;
            }
        }
        if (zero) {
            break;
        }
        uint64_t rem = 0;
        for (uint32_t i = static_cast<uint32_t>(limb.size()); i-- > 0;) {
            const uint64_t cur = (rem << 32) | limb[i];
            limb[i]            = static_cast<uint32_t>(cur / 1000000000ull);
            rem                = cur % 1000000000ull;
        }
        // Nine LSD-first digits of this chunk; the final (most-significant) chunk
        // is trimmed of leading zeros after the loop.
        for (int k = 0; k < 9; ++k) {
            s += static_cast<char>('0' + rem % 10);
            rem /= 10;
        }
    }
    if (s.empty()) {
        return "0";
    }
    while (s.size() > 1 && s.back() == '0') {
        s.pop_back();
    }
    std::reverse(s.begin(), s.end());
    return s;
}

// Automatic %d field width for a width > 64 operand: the decimal length of the
// widest value it can hold. Unsigned -> 2^width - 1; signed -> the most negative
// value's magnitude 2^(width-1) plus the sign character (LRM §21.2.1.2).
uint32_t wide_dec_maxlen(uint32_t width, bool is_signed) {
    const uint32_t        words = plane_words(width);
    std::vector<uint64_t> ext(words, 0);
    if (is_signed) {
        ext[(width - 1) / 64] = uint64_t{ 1 } << ((width - 1) % 64);
        return 1 + static_cast<uint32_t>(wide_udec(ext.data(), words).size());
    }
    for (uint32_t k = 0; k < words; ++k) {
        ext[k] = ~uint64_t{ 0 };
    }
    ext[words - 1] &= (width % 64 == 0) ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << (width % 64)) - 1);
    return static_cast<uint32_t>(wide_udec(ext.data(), words).size());
}

// Decimal digit count of `v` (1 for 0).
uint32_t dec_digits(uint64_t v) {
    uint32_t n = 1;
    while (v >= 10) {
        v /= 10;
        ++n;
    }
    return n;
}

// Render one descriptor under the parsed specifier, appending to `out`.
// Justification follows LRM §21.2.1.2: no explicit width sizes the field to the
// largest possible value (decimal space-padded, other radices zero-padded, %t
// to $timeformat's min width); width 0 is the minimal form; an explicit width
// pads on the left (spaces for d/s/c/t, zeros for h/o/b) and never truncates.
// `-` left-justifies with trailing spaces, an extension (docs/LRM_NOTES.md, note 30).
// `dyn` non-null when the format came from a runtime string, in which case an
// undefined conversion is a design error and not a broken invariant.
void render_arg(std::string& out, const rt_arg& a, const clotho::FormatSpec& spec,
                const rt_fmt_dyn* dyn) {
    const uint32_t w     = a.width ? a.width : 1;
    const uint32_t words = plane_words(w);
    // Top-word mask; every plane is zero-extended above `w`, so only the top
    // word needs clipping for the whole-field x/z collapse and magnitude negate.
    const uint64_t topm = (w % 64 == 0) ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << (w % 64)) - 1);

    std::string field;
    // Field width to justify to; an explicit specifier width overrides the
    // per-conversion default filled in below. Width 0 means minimal.
    uint32_t target   = spec.has_width ? spec.width : 0;
    bool     zero_pad = false;

    switch (spec.conv) {
        case 'd':
        case 'D': {
            // Whole-field x/z collapse test across every plane word (LRM
            // §21.2.1.3): lowercase only when every bit is x (resp. z),
            // uppercase X when x bits mix with anything, uppercase Z when z bits
            // mix with known bits.
            bool any_unknown = false, any_x = false, all_x = true, all_z = true;
            for (uint32_t k = 0; k < words; ++k) {
                const uint64_t full = k + 1 == words ? topm : ~uint64_t{ 0 };
                const uint64_t xzk  = a.xz[k] & full;
                const uint64_t zk   = a.z[k] & full;
                const uint64_t xk   = xzk & ~zk;
                any_unknown |= xzk != 0;
                any_x |= xk != 0;
                all_x &= xk == full;
                all_z &= zk == full;
            }
            if (!spec.has_width) {
                // Automatic size: the widest text a `w`-bit operand can take (a
                // signed operand's most negative value, sign included; LRM
                // §21.2.1.2).
                if (w <= 64) {
                    target = (a.flags & 1u) != 0 ? 1 + dec_digits(uint64_t{ 1 } << (w - 1))
                                                 : dec_digits(width_mask(w));
                } else {
                    target = wide_dec_maxlen(w, (a.flags & 1u) != 0);
                }
            }
            if (any_unknown) {
                if (any_x) {
                    field += all_x ? 'x' : 'X';
                } else {
                    field += all_z ? 'z' : 'Z';
                }
            } else if (w <= 64) {
                const uint64_t mask = width_mask(w);
                const uint64_t val  = a.val[0] & mask;
                if ((a.flags & 1u) != 0) {
                    const uint64_t sext = (w < 64 && ((val >> (w - 1)) & 1u)) ? (val | ~mask) : val;
                    field               = fmt::to_string(static_cast<int64_t>(sext));
                } else {
                    field = fmt::to_string(val);
                }
            } else if ((a.flags & 1u) != 0 && ((a.val[(w - 1) / 64] >> ((w - 1) % 64)) & 1u) != 0) {
                // Negative signed: print the sign and the two's-complement
                // magnitude (2^w - value), computed in place across the words.
                std::vector<uint64_t> mag(words);
                uint64_t              carry = 1;
                for (uint32_t k = 0; k < words; ++k) {
                    const uint64_t inv = ~a.val[k] + carry;
                    carry              = (carry != 0 && inv == 0) ? 1 : 0;
                    mag[k]             = inv;
                }
                mag[words - 1] &= topm;
                field = "-" + wide_udec(mag.data(), words);
            } else {
                field = wide_udec(a.val, words);
            }
            break;
        }
        case 'h':
        case 'H':
        case 'x':
        case 'X':
        case 'o':
        case 'O':
        case 'b':
        case 'B': {
            const char     lc   = static_cast<char>(spec.conv | 0x20);
            const uint32_t bits = lc == 'h' || lc == 'x' ? 4 : lc == 'o' ? 3 : 1;
            render_digits(field, a.val, a.xz, a.z, words, w, bits);
            if (spec.has_width) {
                // An explicit width replaces the automatic sizing: reduce to the
                // minimal digits, then zero-pad back out to the field width
                // (LRM §21.2.1.2 examples: %3h of 32'h5 is "005"; %0h is "a").
                // NOTE: iverilog deviates here (prints the full natural width);
                // the LRM table is explicit, so LRM wins (docs/LRM_NOTES.md, note 31).
                size_t k = 0;
                while (k + 1 < field.size() && field[k] == '0') {
                    ++k;
                }
                field.erase(0, k);
            }
            zero_pad = true;
            break;
        }
        case 'c':
        case 'C':
            field += static_cast<char>(a.val[0] & 0xff);
            break;
        case 't':
        case 'T': {
            // The frontend scales a %t argument to design ticks (scale_to_ticks), so
            // the low word is the tick count and the descriptor's signedness says
            // whether it is negative. render_time converts it per the current
            // $timeformat, whose min width is the default field width for %t.
            const bool negative = (a.flags & 1u) != 0 && static_cast<int64_t>(a.val[0]) < 0;
            render_time(field, negative ? uint64_t{ 0 } - a.val[0] : a.val[0], negative);
            if (!spec.has_width && timeformat.min_width > 0) {
                target = static_cast<uint32_t>(timeformat.min_width);
            }
            break;
        }
        case 's':
        case 'S':
            if ((a.flags & 2u) != 0) {
                // A string object: `val` is the rt_string handle pointer itself.
                const auto* s = reinterpret_cast<const rt_string*>(a.val);
                field.append(str_data(s), str_len(s));
                break;
            }
            // A packed value: ASCII chars most-significant first. Leading 0x00
            // bytes are never printed (LRM §21.2.1.7 — iverilog deviates and
            // prints a space per leading NUL; the LRM text and the §21.2.1.1
            // example are explicit, so LRM wins). An embedded/trailing 0x00
            // prints as a space (LRM silent; matches iverilog).
            {
                const auto byte = [&a](uint32_t j) {
                    return static_cast<uint8_t>(a.val[j / 8] >> (8 * (j % 8)));
                };
                uint32_t k = (w + 7) / 8;
                while (k > 0 && byte(k - 1) == 0) {
                    --k;
                }
                while (k-- > 0) {
                    const auto c = static_cast<char>(byte(k));
                    field += c == '\0' ? ' ' : c;
                }
            }
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            // Real conversion: rebuild the C specifier and let snprintf format
            // the whole field — LRM 21.2 Table 21-2 grants the real specifiers
            // full C formatting (width / precision / justification), so the
            // generic justify pass below sees an already-padded field. `val`
            // carries an IEEE-754 double bit pattern (rt_arg flags bit 2).
            std::string cs = "%";
            if (spec.left_justify) {
                cs += '-';
            }
            if (spec.alt) {
                cs += '#';
            }
            if (spec.zero_pad) {
                cs += '0';
            }
            if (spec.has_width) {
                cs += fmt::to_string(spec.width);
            }
            if (spec.has_prec) {
                cs += '.';
                cs += fmt::to_string(spec.prec);
            }
            cs += spec.conv;
            double v = 0.0;
            std::memcpy(&v, a.val, sizeof v);
            const int n = std::snprintf(nullptr, 0, cs.c_str(), v);
            if (n > 0) {
                field.resize(static_cast<size_t>(n) + 1);
                std::snprintf(field.data(), static_cast<size_t>(n) + 1, cs.c_str(), v);
                field.resize(static_cast<size_t>(n));
            }
            break;
        }
        default:
            if (dyn != nullptr) {
                dynfmt_fatal(dyn, spec_name(spec.conv) + " is not a defined conversion (LRM §21.2.1.1)");
            }
            format_fatal("unsupported conversion reached the formatter");
    }

    if (field.size() >= target) {
        out += field;
        return;
    }
    const size_t fill = target - field.size();
    if (spec.left_justify) {
        out += field;
        out.append(fill, ' ');
    } else {
        out.append(fill, zero_pad ? '0' : ' ');
        out += field;
    }
}

// Unformatted output (LRM §21.2.1.1 %u / %z): the operand's 32-bit words, LSB
// word first, native endian. %u transfers the 2-value projection (x/z bits read
// as zero); %z transfers vpi aval/bval pairs per word (x: a=1 b=1, z: a=0 b=1).
void render_unformatted(std::string& out, const rt_arg& a, bool four_state) {
    const uint32_t w     = a.width ? a.width : 1;
    const uint32_t lanes = (w + 31) / 32;
    // The 32-bit lane `k` of a little-endian plane word array; above-width bits
    // are already zero (the codegen zero-extends every plane).
    const auto lane = [](const uint64_t* p, uint32_t k) {
        return static_cast<uint32_t>(p[k / 2] >> (32 * (k % 2)));
    };
    const auto append_lane = [&out](uint32_t u) { out.append(reinterpret_cast<const char*>(&u), sizeof(u)); };
    for (uint32_t k = 0; k < lanes; ++k) {
        const uint32_t val = lane(a.val, k);
        const uint32_t xz  = lane(a.xz, k);
        const uint32_t z   = lane(a.z, k);
        // %u: 2-value projection (x/z read as 0). %z: vpi aval then bval, where
        // aval is the known-1 and x bits and bval marks every unknown bit.
        append_lane(four_state ? ((val & ~xz) | (xz & ~z)) : (val & ~xz));
        if (four_state) {
            append_lane(xz);
        }
    }
}

// Round half away from zero into a signed 64-bit, the pair the backend's
// lower_ftoi emits for Inst::Tag::FtoI: LRM 6.12.1's real-to-integer conversion,
// the integer then cut to 64 bits (10.7), so a finite magnitude of 2**63 or more
// keeps its low 64 bits. NaN and infinity have no integer value and saturate, as
// in the backend.
int64_t sat_round_i64(double v) {
    if (std::isnan(v)) {
        return 0;
    }
    const double r = std::round(v);
    if (std::isinf(r)) {
        return r < 0 ? INT64_MIN : INT64_MAX;
    }
    if (r > -9223372036854775808.0 && r < 9223372036854775808.0) {
        return static_cast<int64_t>(r);
    }
    // fmod is exact, and the magnitude's remainder below 2**64 is an integer.
    const auto low = static_cast<uint64_t>(std::fmod(std::fabs(r), 18446744073709551616.0));
    return static_cast<int64_t>(r < 0 ? uint64_t{ 0 } - low : low);
}

// The tick count of an integral %t operand under a RUNTIME format: what
// scale_to_ticks puts in the IR for a constant one. The operand's value times the
// unit multiplier `mul` (at least 1), saturating at +-INT64_MAX ticks instead of
// wrapping, the rule a real operand follows (docs/LRM_NOTES.md, note 40).
//
// The sign is the operand's own, read from its top bit at its own width when it is
// signed. The magnitude is taken across every word, so a wide operand is not cut
// to its low one: -2**100 is far beyond the ceiling, not 0.
int64_t integral_time_ticks(const rt_arg& a, uint64_t mul) {
    assert(mul != 0);
    const uint32_t w        = a.width != 0 ? a.width : 1;
    const uint32_t words    = plane_words(w);
    const uint32_t top      = w - 1;
    const bool     negative = (a.flags & 1u) != 0 && ((a.val[top / 64] >> (top % 64)) & 1u) != 0;

    // The magnitude: the planes as they are, or their two's-complement negation
    // over `w` bits (invert, add one, clip the top word to the width).
    uint64_t low   = 0;
    bool     wide  = false;
    uint64_t carry = 1;
    for (uint32_t k = 0; k < words; ++k) {
        uint64_t word = a.val[k];
        if (negative) {
            word  = ~word + carry;
            carry = carry != 0 && word == 0 ? 1 : 0;
            if (k + 1 == words) {
                word &= width_mask(w - 64 * k);
            }
        }
        if (k == 0) {
            low = word;
        } else if (word != 0) {
            wide = true;
        }
    }
    const uint64_t limit = static_cast<uint64_t>(INT64_MAX) / mul;
    if (wide || low > limit) {
        return negative ? -INT64_MAX : INT64_MAX;
    }
    const auto ticks = static_cast<int64_t>(low * mul);
    return negative ? -ticks : ticks;
}

// The same for a real %t operand: rounded half away from zero after the multiply
// (scale_to_ticks scales in the real domain first, so a fraction of a unit
// survives), saturating at +-INT64_MAX. Infinity saturates and NaN is 0, as in the
// frontend. The bound is INT64_MAX as a double, 2**63, the constant
// saturating_ticks tests against.
int64_t real_time_ticks(double v, uint64_t mul) {
    const double     scaled = v * static_cast<double>(mul);
    constexpr double bound  = 9223372036854775808.0;
    if (scaled >= bound) {
        return INT64_MAX;
    }
    if (scaled <= -bound) {
        return -INT64_MAX;
    }
    return sat_round_i64(scaled);
}

// Reconcile one descriptor with the conversion a RUNTIME format asked for.
//
// With a compile-time format lower/SystemTasks.cpp's build_format has the specifier in hand
// and fixes the mismatch in the IR: it emits ItoF for an integral under %e/%f/%g,
// FtoI for a real under %d/%h/%o/%b, scale_to_ticks for %t, and fails the
// compile for the combinations that have no defined answer. A runtime format
// reaches the renderer with the argument already marshalled, so the same
// reconciliation has to happen here -- and the same combinations have to fail,
// loudly and by name, rather than reinterpret one type's bits as another's.
//
// Returns `a` itself when the descriptor already matches; otherwise a descriptor
// backed by `scratch`.
const rt_arg& coerce_dyn_arg(const rt_arg& a, char conv, CoercedArg& scratch, const rt_fmt_dyn* dyn) {
    const char lc        = static_cast<char>(conv | 0x20);
    const bool real_conv = lc == 'e' || lc == 'f' || lc == 'g';
    const bool int_conv  = lc == 'd' || lc == 'h' || lc == 'x' || lc == 'o' || lc == 'b';
    const bool is_real   = (a.flags & 4u) != 0;
    const bool is_str    = (a.flags & 2u) != 0;

    // A synthesized two-state 64-bit descriptor over `scratch.val`.
    const auto synth = [&scratch](bool is_signed) -> const rt_arg& {
        scratch.arg = rt_arg{ 64, is_signed ? 1u : 0u, &scratch.val, &scratch.zero, &scratch.zero };
        return scratch.arg;
    };

    if (is_str) {
        // %s renders the characters, %p the same in quotes (render_p_scalar).
        // Every other conversion would need the packed reinterpretation of those
        // characters, which build_format refuses at compile time for exactly this
        // reason ("a string argument to $display/$write requires %s").
        // TODO: %d / %h / ... of a string object, on both paths at once.
        if (lc != 's' && lc != 'p') {
            dynfmt_fatal(dyn, spec_name(conv) + " of a string argument is not supported: a string "
                                                "renders under %s, or %p in quotes (LRM §21.2.1.1)");
        }
        return a;
    }

    if (is_real) {
        if (real_conv || lc == 'p') {
            return a; // the real specifiers, and %p which renders a real as %g
        }
        if (lc == 't') {
            // One step, not two: scale_to_ticks multiplies in the real domain and
            // rounds afterwards, so rounding first would quantize to whole time
            // units before the multiply and report the wrong tick. Signed, as a
            // real is: a negative one is a negative time.
            scratch.val = static_cast<uint64_t>(real_time_ticks(std::bit_cast<double>(a.val[0]), dyn->time_mul));
            return synth(/*is_signed=*/true);
        }
        if (int_conv) {
            // Table 21-1 gives these a common integral operand, reached from a
            // real by LRM §21.2.1.2 -> §6.12.1. Signed 64-bit, so a negative
            // prints as two's complement under %h (build_format's own choice).
            scratch.val = static_cast<uint64_t>(sat_round_i64(std::bit_cast<double>(a.val[0])));
            return synth(/*is_signed=*/true);
        }
        // TODO: %c of a real (the rounded integer truncated to a character code)
        // and %u / %z of a real (its raw bits, LRM §21.2.1.3). build_format
        // refuses both on the compile-time path; keep the two paths agreeing.
        dynfmt_fatal(dyn, spec_name(conv) + " with a real argument is not supported");
    }

    // An integral operand.
    if (real_conv) {
        // LRM §6.12.1 integral -> real, so one representation reaches the
        // formatter. Mirrors build_format's ItoF, which reads the must plane and
        // therefore takes x/z bits as 0.
        if (a.width > 64) {
            // TODO: an operand wider than 64 bits under %e / %f / %g from a
            // runtime format. The compile-time path handles it (ItoF lowers to
            // SIToFP on the full iW), but accumulating the planes here would
            // round at every word boundary instead of once, so it would be
            // quietly off rather than merely imprecise.
            dynfmt_fatal(dyn, spec_name(conv) + " of an operand wider than 64 bits is not supported "
                                                "from a runtime format string");
        }
        const uint64_t bits = a.val[0];
        double         d    = 0.0;
        if ((a.flags & 1u) != 0 && a.width < 64 && ((bits >> (a.width - 1)) & 1u) != 0) {
            d = static_cast<double>(static_cast<int64_t>(bits | ~width_mask(a.width)));
        } else if ((a.flags & 1u) != 0 && a.width == 64) {
            d = static_cast<double>(static_cast<int64_t>(bits));
        } else {
            d = static_cast<double>(bits);
        }
        scratch.val = std::bit_cast<uint64_t>(d);
        scratch.arg = rt_arg{ 64, 0b101u, &scratch.val, nullptr, nullptr };
        return scratch.arg;
    }
    if (lc == 't') {
        // scale_to_ticks's count: the operand's value times the unit multiplier,
        // saturating, signed as the operand is (docs/LRM_NOTES.md, note 40).
        scratch.val = static_cast<uint64_t>(integral_time_ticks(a, dyn->time_mul));
        return synth(/*is_signed=*/true);
    }
    return a;
}

// The `%p` text of a SINGULAR value (LRM §21.2.1.6's closing paragraph: an
// element of an aggregate expression prints "as [it] would [print] unformatted").
// Reachable only from a runtime format: with a compile-time one the frontend
// splices %p away before the renderer sees it, so there is nothing for the
// specifier switch to do.
//
// MUST agree with lower/SystemTasks.cpp's expand_p_scalar, which for the same three cases
// splices exactly `%0d` (minimal decimal at the operand's own signedness), `%g`
// for a real, and `"%s"` for a string. A constant format and a runtime format
// that hold the same text have to render the same value identically; these are
// the only two places that decide it, so they drift or they match.
void render_p_scalar(std::string& out, const rt_arg& a) {
    clotho::FormatSpec spec{};
    spec.ok = true;
    if ((a.flags & 4u) != 0) {
        spec.conv = 'g';
        render_arg(out, a, spec);
        return;
    }
    if ((a.flags & 2u) != 0) {
        spec.conv = 's';
        out += '"';
        render_arg(out, a, spec);
        out += '"';
        return;
    }
    // `%0d`: has_width with width 0 is the LRM §21.2.1.2 minimal form. Leaving
    // has_width false would auto-size the field and pad the number.
    spec.conv      = 'd';
    spec.has_width = true;
    render_arg(out, a, spec);
}

// Format `fmt` with the argument descriptors, appending to `out`. Shared by every
// $write-family task ($display / $write / $fdisplay / $fwrite differ only in the
// destination and the folded-in newline, both handled by the caller; $sformat /
// $swrite in the stored destination).
//
// `dyn` null means the format was known at compile time: the frontend validated
// every specifier, matched every descriptor to its conversion and spliced away
// %m / %l / %p, so a malformed or unsupported one here is an invariant break and
// no reconciliation is needed. Non-null means the format is a runtime string
// (LRM §21.3.3's $sformat / $sformatf exception) and none of that happened, so
// every check is made here against the design instead.
void render(std::string& out, const char* fmt, uint32_t argc, const rt_arg* argv,
            const rt_fmt_dyn* dyn) {
    const std::string_view f(fmt);
    uint32_t               ai = 0;
    for (size_t pos = 0; pos < f.size();) {
        if (f[pos] != '%') {
            out += f[pos++];
            continue;
        }
        const clotho::FormatSpec spec = clotho::parse_format_spec(f.substr(pos + 1));
        if (!spec.ok) {
            if (dyn != nullptr) {
                dynfmt_fatal(dyn, "a runtime format string ends in an incomplete conversion "
                                  "specification (LRM §21.2.1.1)");
            }
            format_fatal("malformed format specifier reached the formatter");
        }
        pos += 1 + spec.len;
        if (spec.conv == '%') {
            out += '%';
            continue;
        }
        if (dyn != nullptr && (spec.conv == 'm' || spec.conv == 'M' || spec.conv == 'l' ||
                               spec.conv == 'L')) {
            // %m / %l consume no argument and name the INVOKING SCOPE, which only
            // the frontend knows: append_format_literal splices the text in at
            // lower time and the runtime never learns either name.
            //
            // TODO: %m / %l in a runtime format string. The hierarchical name and
            // the library.cell of the call site would have to ride the IR record
            // (two more byte runs on SformatInfo / SformatfInfo) and reach the
            // renderer through rt_fmt_dyn.
            dynfmt_fatal(dyn, spec_name(spec.conv) + " is not supported in a runtime format string: "
                                                     "it names the invoking scope, which only the "
                                                     "compiler knows");
        }
        if (ai >= argc) {
            if (dyn != nullptr) {
                dynfmt_fatal(dyn, "a runtime format string has more conversion specifications than "
                                  "arguments: " + spec_name(spec.conv) + " has none left to consume "
                                  "(LRM §21.2.1.1)");
            }
            format_fatal("format specifier without a matching argument");
        }
        const rt_arg& raw = argv[ai++];
        CoercedArg    scratch;
        const rt_arg& a = dyn != nullptr ? coerce_dyn_arg(raw, spec.conv, scratch, dyn) : raw;
        if (spec.conv == 'u' || spec.conv == 'U' || spec.conv == 'z' || spec.conv == 'Z') {
            render_unformatted(out, a, spec.conv == 'z' || spec.conv == 'Z');
            continue;
        }
        if (spec.conv == 'p' || spec.conv == 'P') {
            // Only a runtime format reaches here; see render_p_scalar. An
            // aggregate operand cannot: src/lower/ refuses one outright against a
            // runtime format, because rt_arg has no image of it.
            render_p_scalar(out, a);
            continue;
        }
        if (dyn != nullptr && (spec.conv == 'v' || spec.conv == 'V')) {
            // TODO: %v (per-bit net strength) has no runtime model to read at all
            // -- Clotho resolves drive strengths away at the resolver. Rejected on
            // the compile-time path by check_specifier for the same reason.
            dynfmt_fatal(dyn, "%v (net signal strength) is not supported");
        }
        render_arg(out, a, spec, dyn);
    }
}

// Preopen the standard streams on first file-I/O use, so a design that never
// touches file I/O pays nothing. mcd_channels[0] doubles as the init flag.
void io_ensure_init() {
    if (mcd_channels[0] != nullptr) {
        return;
    }
    mcd_channels[0] = stdout;
    fd_streams      = { stdin, stdout, stderr };
}

// Reconstruct a string from a right-justified packed-ASCII vector: byte 0 is the
// last character and the high bytes are zero padding (the layout $readmem and
// $fopen both receive from the backend).
std::string unpack_ascii(const char* buf, uint32_t len) {
    std::string s;
    uint32_t    hi = len;
    while (hi > 0 && buf[hi - 1] == 0) {
        --hi;
    }
    for (uint32_t k = hi; k-- > 0;) {
        s.push_back(static_cast<char>(buf[k]));
    }
    return s;
}

} // namespace rt_internal
