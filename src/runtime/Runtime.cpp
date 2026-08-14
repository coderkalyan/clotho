#include "Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cinttypes>
#include <condition_variable>
#include <csetjmp>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <ankerl/unordered_dense.h>
#include <fmt/color.h>

#include "Format.h"
#include "Wheel.h"

namespace {

// The runtime's monomorphization: every fired timer is handed the single opaque
// context pointer the design threads through rt_wheel_tick.
using RuntimeWheel = clotho::Wheel<void*>;

// rt_timer is a layout-compatible mirror of clotho::Timer, so a node can cross
// the C ABI by pointer cast rather than a copy. Keep the two in lockstep.
static_assert(sizeof(rt_timer) == sizeof(clotho::Timer));
static_assert(alignof(rt_timer) == alignof(clotho::Timer));
static_assert(offsetof(rt_timer, next) == offsetof(clotho::Timer, next));
static_assert(offsetof(rt_timer, on_expire) == offsetof(clotho::Timer, on_expire));

// rt_wheel is opaque inline storage the caller owns; the wheel is constructed in
// place by rt_wheel_init. The storage must be big and aligned enough to hold it.
static_assert(sizeof(RuntimeWheel) <= sizeof(rt_wheel));
static_assert(alignof(RuntimeWheel) <= alignof(rt_wheel));

RuntimeWheel* unwrap(rt_wheel* w) {
    return reinterpret_cast<RuntimeWheel*>(w->storage);
}

const RuntimeWheel* unwrap(const rt_wheel* w) {
    return reinterpret_cast<const RuntimeWheel*>(w->storage);
}

// -- $timeformat / %t state --

// The single global %t format, set by $timeformat and seeded by
// rt_timeformat_init at startup. The simulator is single-threaded, so a plain
// global is sufficient (LRM §20.4.3).
struct TimeFormat {
    // Femtoseconds per design tick; converts a tick-valued %t argument to fs.
    uint64_t tick_fs = 1;
    // Display time-unit exponent (10^units seconds).
    int32_t units = 0;
    // Fractional digits printed after the decimal point.
    int32_t precision = 0;
    // Minimum field width of the number + suffix (space-padded on the left).
    int32_t min_width = 20;
    // String appended after the number (e.g. " ns").
    std::string suffix;
};

TimeFormat timeformat;

// Render a tick-valued time `ticks` per the current $timeformat, appending the
// unpadded number + suffix to `out` (LRM §20.4.3). The tick value scales to
// femtoseconds, then to the display unit (10^units s); since fs = 10^-15 s, a
// display unit of 10^units s is 10^(units+15) fs. The number is printed with
// `precision` fractional digits. Field justification (to $timeformat's
// min_width or an explicit %t width) is the caller's job.
void render_time(std::string& out, uint64_t ticks) {
    const double fs      = static_cast<double>(ticks) * static_cast<double>(timeformat.tick_fs);
    double       divisor = 1.0;
    for (int32_t k = 0; k < timeformat.units + 15; ++k) {
        divisor *= 10.0;
    }
    const double value = fs / divisor;

    const int32_t prec = timeformat.precision < 0 ? 0 : timeformat.precision;
    char          numbuf[64];
    std::snprintf(numbuf, sizeof(numbuf), "%.*f", prec, value);

    out += numbuf;
    out += timeformat.suffix;
}

// A runtime formatting invariant broke: the frontend validates every specifier
// before it reaches the runtime, so these paths are unreachable from lowered
// code. Abort loudly rather than emit silently-wrong output.
[[noreturn]] void format_fatal(const char* what) {
    std::fprintf(stderr, "clotho runtime: %s\n", what);
    std::abort();
}

// -- $display formatting --

// Low-`w`-bits mask (all ones for w >= 64).
uint64_t width_mask(uint32_t w) {
    return w >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << w) - 1);
}

// Number of 64-bit words a `width`-bit plane occupies (>= 1).
uint32_t plane_words(uint32_t width) {
    return width == 0 ? 1 : (width + 63) / 64;
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

// Forward declarations: the rt_string representation lives below with the
// other string internals; %s rendering needs these two.
uint64_t    str_len(const rt_string* s);
const char* str_data(const rt_string* s);

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
// `-` left-justifies with trailing spaces (an extension the LRM does not define).
void render_arg(std::string& out, const rt_arg& a, const clotho::FormatSpec& spec) {
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
                    field               = std::to_string(static_cast<int64_t>(sext));
                } else {
                    field = std::to_string(val);
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
                // the LRM table is explicit, so LRM wins.
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
        case 'T':
            // The frontend scales a %t argument to design ticks, so the low word
            // is the tick count; render_time converts it per the current
            // $timeformat, whose min width is the default field width for %t.
            render_time(field, a.val[0]);
            if (!spec.has_width && timeformat.min_width > 0) {
                target = static_cast<uint32_t>(timeformat.min_width);
            }
            break;
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
                cs += std::to_string(spec.width);
            }
            if (spec.has_prec) {
                cs += '.';
                cs += std::to_string(spec.prec);
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

// Format `fmt` with the argument descriptors, appending to `out`. Shared by every
// $write-family task ($display / $write / $fdisplay / $fwrite differ only in the
// destination and the folded-in newline, both handled by the caller; $sformat /
// $swrite in the stored destination). The frontend validates every specifier at
// lower time, so a malformed or unsupported one here is an invariant break.
void render(std::string& out, const char* fmt, uint32_t argc, const rt_arg* argv) {
    const std::string_view f(fmt);
    uint32_t               ai = 0;
    for (size_t pos = 0; pos < f.size();) {
        if (f[pos] != '%') {
            out += f[pos++];
            continue;
        }
        const clotho::FormatSpec spec = clotho::parse_format_spec(f.substr(pos + 1));
        if (!spec.ok) {
            format_fatal("malformed format specifier reached the formatter");
        }
        pos += 1 + spec.len;
        if (spec.conv == '%') {
            out += '%';
            continue;
        }
        if (ai >= argc) {
            format_fatal("format specifier without a matching argument");
        }
        const rt_arg& a = argv[ai++];
        if (spec.conv == 'u' || spec.conv == 'U' || spec.conv == 'z' || spec.conv == 'Z') {
            render_unformatted(out, a, spec.conv == 'z' || spec.conv == 'Z');
        } else {
            render_arg(out, a, spec);
        }
    }
}

// -- File I/O descriptor tables (LRM §21.3) --
//
// Two integer descriptor kinds share this state, distinguished by bit 31 (see
// Runtime.h). The tables are process-global and unsynchronized: one simulator
// per process, single-threaded. Adding thread safety later means a lock around
// these two containers plus stdio's own per-stream locking on the writes.

// Bit 31: set marks an FD, clear marks an MCD.
constexpr int32_t kFdBit = static_cast<int32_t>(0x80000000u);

// MCD channels indexed by bit position; [0] is the preopened standard output. A
// one-argument $fopen claims the lowest free channel in [1, 30].
std::array<std::FILE*, 31> mcd_channels{};

// FD streams indexed by (descriptor & ~kFdBit); [0]/[1]/[2] are stdin/stdout/
// stderr. A closed slot holds nullptr and is reused before the vector grows.
std::vector<std::FILE*> fd_streams;

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

// A filename / source ASCII argument, from either representation. A dynamic
// string (`forward` != 0) is already natural byte order and exact length; a
// packed-ASCII vector is little-endian (byte 0 = last char) and reverses via
// unpack_ascii. Mirrors the $readmem / $dumpfile split (rt_readmem / vcd_open).
std::string read_ascii_arg(const char* buf, uint32_t len, int32_t forward) {
    return forward != 0 ? std::string(buf, len) : unpack_ascii(buf, len);
}

// Resolve a single-stream FD to its FILE*, or nullptr if out of range / closed.
std::FILE* fd_stream(int32_t desc) {
    const uint32_t idx = static_cast<uint32_t>(desc & ~kFdBit);
    return idx < fd_streams.size() ? fd_streams[idx] : nullptr;
}

// Store `count` bytes right-justified into a packed vector's planes: byte 0 (LSB)
// holds the last byte, higher bytes precede it, the top bytes are cleared. When
// the bytes overflow the vector the low (rightmost) `capacity` are kept. must ==
// may (bytes are fully known) and the z plane is cleared. width % 8 == 0 (the
// backend guards it), so the must / may plane halves stay byte-aligned. A narrow
// 2-state slot holds only the iW value word, so exactly `capacity` bytes are
// written (a wider write would overrun into the adjacent state field). Shared by
// $fgets, $sformat / $swrite, and $ferror.
void store_string_planes(uint8_t* mm, uint8_t* z, uint32_t width, const uint8_t* bytes, uint32_t count,
                         bool narrow) {
    const uint32_t capacity = width / 8;
    std::memset(mm, 0, narrow ? capacity : static_cast<size_t>(capacity) * 2);
    if (z != nullptr) { // null when the target's slot has no z word (z elision)
        std::memset(z, 0, capacity);
    }
    const uint32_t keep = count < capacity ? count : capacity;
    for (uint32_t k = 0; k < keep; ++k) {
        const uint8_t byte = bytes[count - 1 - k]; // last byte read lands in mm[0]
        mm[k]              = byte;                 // must half (low `width` bits)
        if (!narrow) {
            mm[capacity + k] = byte; // may half (high `width` bits)
        }
    }
}

// Write a ≤64-bit value's planes into a packed vector: the must|may word (must in
// the low `width` bits, may above) at `mm`, plus the z word at `z`. Values are
// masked to `width`. A narrow 2-state slot stores only the iW must word (x/z bits
// are 0 there by construction, LRM 6.3.1). Shared by $readmem, $fread, and the
// $fscanf / $sscanf engine.
void write_word_planes(uint8_t* mm, uint8_t* z, uint32_t width, uint64_t must, uint64_t may, uint64_t zz,
                       bool narrow) {
    const uint64_t wmask = width >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << width) - 1);
    must &= wmask;
    if (narrow) {
        std::memcpy(mm, &must, (width + 7) / 8);
        return;
    }
    may &= wmask;
    zz &= wmask;
    const unsigned __int128 mmv = static_cast<unsigned __int128>(must) |
                                  (static_cast<unsigned __int128>(may) << width);
    std::memcpy(mm, &mmv, (2 * width + 7) / 8);
    if (z != nullptr) { // null when the target's slot has no z word (z elision)
        std::memcpy(z, &zz, (width + 7) / 8);
    }
}

// -- Wide (> 64-bit) plane assembly for $readmem / $fread --
//
// The <= 64-bit path above packs into one __int128; a wider element assembles
// its value in a little-endian word array (ceil(width/64) words) instead. These
// helpers are the multi-word analogues of the `(v << k) | d` accumulation and
// the `must | (may << width)` packing the scalar path does in machine words.
// (plane_words is defined above with the display formatting helpers.)

// Mask the top word of a `words`-word little-endian value to `width` bits.
void mask_top(uint64_t* a, uint32_t words, uint32_t width) {
    const uint32_t bits = width % 64;
    if (bits != 0) {
        a[words - 1] &= (uint64_t{ 1 } << bits) - 1;
    }
}

// Shift the `words`-word value left by `shift` bits, in place (high bits drop).
void wide_shl(uint64_t* a, uint32_t words, uint32_t shift) {
    if (shift == 0) {
        return;
    }
    const uint32_t ws = shift / 64;
    const uint32_t bs = shift % 64;
    for (int32_t k = static_cast<int32_t>(words) - 1; k >= 0; --k) {
        uint64_t v = 0;
        if (static_cast<uint32_t>(k) >= ws) {
            v = a[k - ws] << bs;
            if (bs != 0 && static_cast<uint32_t>(k) > ws) {
                v |= a[k - ws - 1] >> (64 - bs);
            }
        }
        a[k] = v;
    }
}

// OR `src` (`words` words) shifted left by `shift` bits into `dst`, which must
// have room for the shifted result. Used to lay `may` above `must` in the packed
// must|may vector.
void wide_or_shifted(uint64_t* dst, const uint64_t* src, uint32_t words, uint32_t shift) {
    const uint32_t ws = shift / 64;
    const uint32_t bs = shift % 64;
    for (uint32_t k = 0; k < words; ++k) {
        dst[ws + k] |= src[k] << bs;
        if (bs != 0) {
            dst[ws + k + 1] |= src[k] >> (64 - bs);
        }
    }
}

// a = a * mul + add, in place (high bits drop). `mul`/`add` are small (base and a
// single digit), so each partial product fits __int128. The multi-word analogue
// of the scalar `v = v * 10 + d` decimal accumulation.
void wide_mul_add(uint64_t* a, uint32_t words, uint64_t mul, uint64_t add) {
    uint64_t carry = add;
    for (uint32_t k = 0; k < words; ++k) {
        const unsigned __int128 p = static_cast<unsigned __int128>(a[k]) * mul + carry;
        a[k]                      = static_cast<uint64_t>(p);
        carry                     = static_cast<uint64_t>(p >> 64);
    }
}

// Two's-complement negate a `words`-word value in place (~a + 1). For one word
// this is the scalar `-v`; the target-width masking happens at write time.
void wide_negate(uint64_t* a, uint32_t words) {
    uint64_t carry = 1;
    for (uint32_t k = 0; k < words; ++k) {
        const uint64_t inv = ~a[k] + carry;
        carry              = (carry != 0 && inv == 0) ? 1 : 0;
        a[k]               = inv;
    }
}

// Wide sibling of write_word_planes: the value planes are little-endian word
// arrays (`words` = plane_words(width)). Masks each to `width`, then writes the
// packed must|may vector (must low, may above) and the z word, matching the
// scalar layout bit for bit.
void write_word_planes_wide(uint8_t* mm, uint8_t* z, uint32_t width, uint64_t* must, uint64_t* may, uint64_t* zz,
                            bool narrow) {
    const uint32_t words = plane_words(width);
    mask_top(must, words, width);
    if (narrow) {
        std::memcpy(mm, must, (width + 7) / 8);
        return;
    }
    mask_top(may, words, width);
    // packed holds 2*width bits; +1 word of slack for a shift straddling the top.
    std::vector<uint64_t> packed((2 * width + 63) / 64 + 1, 0);
    std::memcpy(packed.data(), must, words * sizeof(uint64_t));
    wide_or_shifted(packed.data(), may, words, width);
    std::memcpy(mm, packed.data(), (2 * width + 7) / 8);
    if (z != nullptr) { // null when the target's slot has no z word (z elision)
        mask_top(zz, words, width);
        std::memcpy(z, zz, (width + 7) / 8);
    }
}

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

namespace {

// -- SystemVerilog string representation --
//
// fbstring-style small-string optimization over the 32 opaque bytes of an
// rt_string (Runtime.h). Two forms, discriminated by the MSB of byte 31:
//
//   inline: bytes[0..29] hold up to 30 characters; byte[len] is a NUL
//           terminator; byte 31 holds 30 - len, so a full 30-char string stores 0
//           there and no separate flag byte is spent.
//   heap:   a StrHeap {ptr, len, cap} occupies bytes[0..23]; byte 31 is 0x80. The
//           buffer is always cap+1 bytes so byte[len] can hold the terminator.
//
// The length is explicit (O(1) str_len) and authoritative; a NUL terminator is
// ALSO maintained at byte[len] so str_data() is a valid C string (C interop /
// DPI). Embedded NULs are legal — only a C-string *view* truncates at the first;
// every clotho op uses the length. A heap buffer never shrinks back to inline —
// the capacity is kept for reuse so reassignment in a loop stops allocating.

constexpr uint64_t      str_inline_cap = rt_string_inline_cap; // 30 (see Runtime.h)
constexpr unsigned char str_heap_flag  = 0x80;

// The heap form's header, memcpy'd in and out of the handle bytes.
struct StrHeap {
    char*    ptr;
    uint64_t len;
    uint64_t cap;
};
static_assert(sizeof(StrHeap) == 24);
static_assert(sizeof(StrHeap) < sizeof(rt_string));
static_assert(alignof(StrHeap) <= alignof(rt_string));

bool str_is_heap(const rt_string* s) {
    return (s->storage[31] & str_heap_flag) != 0;
}

StrHeap str_heap(const rt_string* s) {
    StrHeap h;
    std::memcpy(&h, s->storage, sizeof(h));
    return h;
}

void str_set_heap(rt_string* s, const StrHeap& h) {
    std::memcpy(s->storage, &h, sizeof(h));
    s->storage[31] = str_heap_flag;
}

uint64_t str_len(const rt_string* s) {
    return str_is_heap(s) ? str_heap(s).len : str_inline_cap - s->storage[31];
}

const char* str_data(const rt_string* s) {
    return str_is_heap(s) ? str_heap(s).ptr : reinterpret_cast<const char*>(s->storage);
}

char* str_data(rt_string* s) {
    return str_is_heap(s) ? str_heap(s).ptr : reinterpret_cast<char*>(s->storage);
}

// Core write path: s = the n bytes at p. p may alias s's own buffer (memmove on
// the in-place paths; on the grow path the new buffer is filled before the old
// one is freed).
void str_assign(rt_string* s, const char* p, uint64_t n, const rt_alloc_site* site) {
    if (!str_is_heap(s)) {
        if (n <= str_inline_cap) {
            if (n != 0) {
                std::memmove(s->storage, p, n);
            }
            s->storage[n]  = 0; // NUL terminator (n <= 30, byte 31 stays the marker)
            s->storage[31] = static_cast<unsigned char>(str_inline_cap - n);
            return;
        }
        char* buf = static_cast<char*>(rt_alloc_checked(n + 1, site, rt_diag_now()));
        std::memcpy(buf, p, n);
        buf[n] = 0;
        str_set_heap(s, StrHeap{ buf, n, n });
        return;
    }
    StrHeap h = str_heap(s);
    if (n <= h.cap) {
        if (n != 0) {
            std::memmove(h.ptr, p, n);
        }
        h.ptr[n] = 0; // buffer is cap+1 bytes, so byte[n] is in range
        h.len    = n;
        str_set_heap(s, h);
        return;
    }
    // Geometric growth so repeated append-style reassignment amortizes.
    const uint64_t cap = h.cap * 2 > n ? h.cap * 2 : n;
    char*          buf = static_cast<char*>(rt_alloc_checked(cap + 1, site, rt_diag_now()));
    std::memcpy(buf, p, n);
    buf[n] = 0;
    std::free(h.ptr);
    str_set_heap(s, StrHeap{ buf, n, cap });
}

// Grow s's buffer to hold at least n bytes, preserving contents and length.
// The handle stays valid across the grow, so aliased readers of s (same handle
// pointer) keep seeing the current bytes.
void str_reserve(rt_string* s, uint64_t n, const rt_alloc_site* site) {
    if (!str_is_heap(s)) {
        if (n <= str_inline_cap) {
            return;
        }
        const uint64_t len = str_inline_cap - s->storage[31];
        char*          buf = static_cast<char*>(rt_alloc_checked(n + 1, site, rt_diag_now()));
        std::memcpy(buf, s->storage, len);
        buf[len] = 0; // preserve the NUL at byte[len] in the new buffer
        str_set_heap(s, StrHeap{ buf, len, n });
        return;
    }
    StrHeap h = str_heap(s);
    if (n <= h.cap) {
        return;
    }
    // Geometric growth so repeated append-style reassignment amortizes.
    const uint64_t cap = h.cap * 2 > n ? h.cap * 2 : n;
    char*          buf = static_cast<char*>(rt_alloc_checked(cap + 1, site, rt_diag_now()));
    std::memcpy(buf, h.ptr, h.len);
    buf[h.len] = 0;
    std::free(h.ptr);
    str_set_heap(s, StrHeap{ buf, h.len, cap });
}

// Set the length of s; capacity for n bytes must already be reserved (str_reserve
// / str_assign allocate cap+1, so byte[n] holds the NUL terminator).
void str_set_len(rt_string* s, uint64_t n) {
    if (str_is_heap(s)) {
        StrHeap h = str_heap(s);
        h.ptr[n]  = 0;
        h.len     = n;
        str_set_heap(s, h);
        return;
    }
    s->storage[n]  = 0;
    s->storage[31] = static_cast<unsigned char>(str_inline_cap - n);
}

char str_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

char str_ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Scratch buffer for building concat/repl results before the alias-safe final
// assign: stack storage for small results, heap beyond.
struct StrScratch {
    char  stack[256];
    char* buf;

    explicit StrScratch(uint64_t n, const rt_alloc_site* site) {
        buf = n <= sizeof(stack) ? stack
                                 : static_cast<char*>(rt_alloc_checked(n, site, rt_diag_now()));
    }
    ~StrScratch() {
        if (buf != stack) {
            std::free(buf);
        }
    }
    StrScratch(const StrScratch&)            = delete;
    StrScratch& operator=(const StrScratch&) = delete;
};

clotho::Timer& unwrap(rt_timer* t) {
    return *reinterpret_cast<clotho::Timer*>(t);
}

// One array-NBA signal's deferred-write buffer: a growable byte buffer of
// fixed-stride records (defined and filled by the backend) plus a live count.
// Reused across timesteps (count reset, capacity kept).
struct ArrQueue {
    std::vector<uint8_t> buf;
    uint32_t             count = 0;
};

std::vector<ArrQueue> arr_queues; // indexed by dense array-nba id
uint32_t              arr_total = 0; // records staged this pass, across all arrays

} // namespace

extern "C" void* rt_nba_arr_slot(uint32_t idx, uint32_t stride) {
    if (idx >= arr_queues.size()) {
        arr_queues.resize(idx + 1);
    }
    ArrQueue&    q    = arr_queues[idx];
    const size_t need = static_cast<size_t>(q.count + 1) * stride;
    if (q.buf.size() < need) {
        // Geometric growth; resize copies existing records, so slot pointers the
        // backend already wrote stay valid (they were filled before this grow).
        q.buf.resize(std::max<size_t>(need, q.buf.size() * 2));
    }
    void* p = q.buf.data() + static_cast<size_t>(q.count) * stride;
    q.count++;
    arr_total++;
    return p;
}

extern "C" void* rt_nba_arr_base(uint32_t idx) {
    // An array with no writes this pass may have no buffer yet; its commit loop
    // runs zero iterations, so a null base is never dereferenced.
    return idx < arr_queues.size() ? arr_queues[idx].buf.data() : nullptr;
}

extern "C" uint32_t rt_nba_arr_count(uint32_t idx) {
    return idx < arr_queues.size() ? arr_queues[idx].count : 0;
}

extern "C" uint32_t rt_nba_arr_any(void) {
    return arr_total;
}

extern "C" void rt_nba_arr_reset(void) {
    for (ArrQueue& q : arr_queues) {
        q.count = 0;
    }
    arr_total = 0;
}

// -- Container element lifecycle (rt_elem_ops) --
//
// Generic recursion over the element descriptor. POD short-circuits to raw
// byte moves, so the descriptor adds no cost to the plane-scalar fast paths.
// Internal to the runtime — the backend inlines the element ops at call sites
// and never emits a call to these, so they carry no C linkage.
namespace {

void rt_elem_init(const rt_elem_ops* o, void* p) {
    std::memcpy(p, o->proto, static_cast<size_t>(o->stride));
}

void rt_elem_deinit(const rt_elem_ops* o, void* p) {
    switch (o->kind) {
        case rt_elem_pod:
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
    if (count > 1 && proto_is_zero(ops)) {
        h->data = rt_zalloc_elems_checked(count, stride, site, rt_diag_now());
        h->len  = count;
        return;
    }
    auto* buf = static_cast<uint8_t*>(rt_alloc_elems_checked(count, stride, site, rt_diag_now()));
    for (uint64_t i = 0; i < count; ++i) {
        std::memcpy(buf + i * stride, ops->proto, static_cast<size_t>(stride));
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

// -- Array ordering methods (LRM 7.12.2) --
//
// Element-agnostic reverse and value-keyed sort over `stride`-byte slots, shared
// by the dynamic-array and queue entry points. reverse() moves whole slots and
// works for any element; sort()/rsort() key on a scalar element value, restricted
// by the frontend to a 2-state integral (width <= 64) or real element.

namespace {

// Sort-key kind for the ordering comparator; mirrors the backend descriptor.
enum : uint32_t { sort_unsigned = 0, sort_signed = 1, sort_real = 2 };

// One xoshiro256++ step against a process frame RNG state. Byte-for-byte the
// same recurrence the backend inlines at each $urandom site
// (`emit_xoshiro_next` in LlvmBackend.cpp), so a shuffle drawing from the frame
// state leaves the stream consistent for subsequent inlined $urandom draws.
uint64_t xoshiro_next(uint64_t* s) {
    const auto rotl = [](uint64_t x, unsigned k) { return (x << k) | (x >> (64 - k)); };
    const uint64_t result = rotl(s[0] + s[3], 23) + s[0];
    const uint64_t t      = s[1] << 17;
    uint64_t       n2     = s[2] ^ s[0];
    uint64_t       n3     = s[3] ^ s[1];
    const uint64_t n1     = s[1] ^ n2;
    const uint64_t n0     = s[0] ^ n3;
    n2 ^= t;
    n3    = rotl(n3, 45);
    s[0]  = n0;
    s[1]  = n1;
    s[2]  = n2;
    s[3]  = n3;
    return result;
}

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
        std::memcpy(&da, a, sizeof(double));
        std::memcpy(&db, b, sizeof(double));
        return da < db;
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

// Sort `n` contiguous slots ascending; `rev` then flips to descending (rsort).
// A scalar element equals its own sort key, so ties are unobservable and an
// unstable sort suffices (LRM 7.12.2 does not require stability).
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

} // namespace

// The three ordering entry points take the element descriptor rather than a
// bare stride: they reindex the container, so each bumps the relocation epoch,
// and `ops` keeps every container entry point on one signature shape. The
// reverse_run / sort_run / swap_slots helpers above stay on a bare stride --
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
    sort_run(static_cast<uint8_t*>(h->data), h->len, ops->stride, kind, width, rev);
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

// -- Queues (LRM 7.10) --
//
// A queue is a growable ring buffer: `base` holds `cap` element slots, the `len`
// live elements at physical positions (head + i) mod cap. All indexing is via
// q_elem; growth linearises to head == 0.

namespace {

// Address of logical element `i` (0-based) in the ring. Requires cap > 0.
uint8_t* q_elem(const rt_queue* h, uint64_t stride, uint64_t i) {
    const uint64_t phys = (h->head + i) % h->cap;
    return static_cast<uint8_t*>(h->base) + phys * stride;
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

namespace {

// One deferred nonblocking-intra-assignment site (a target signal). `pool` owns
// every record ever allocated for the site (stable addresses — the wheel holds
// pointers into them across time); `freelist` holds the currently-free ones;
// `ready` holds records that have fired and await the NBA-region commit. A record
// cycles pool-backed: freelist -> (armed on wheel) -> ready -> freelist.
struct IntraSite {
    std::vector<std::unique_ptr<unsigned char[]>> pool;
    std::vector<void*>                             freelist;
    std::vector<void*>                             ready;
};

std::vector<IntraSite> intra_sites; // indexed by dense intra-nba id
uint32_t               intra_total = 0; // ready records across all sites
uint32_t               intra_seq   = 0; // monotonic arm-order stamp

// Per inertial continuous-assignment site (LRM 10.3.3): the current deschedule
// epoch and the value currently scheduled to propagate. Indexed by the same dense
// intra-nba id as intra_sites. `pending` records whether a store is in flight (so a
// same-value change keeps its timer, LRM 10.3.3 step b).
struct InertialState {
    uint64_t epoch   = 0;
    bool     pending = false;
    uint64_t pmm     = 0; // pending value's packed mm plane
    uint64_t pz      = 0; // pending value's packed z plane
};
std::vector<InertialState> inertial_states; // indexed by dense intra-nba id

// Per trigger id (dense (trigger-signal, edge)): records armed on `x <= @e y`
// waiting for that edge. Growable — an unbounded number can arm between two edges.
std::vector<std::vector<void*>> intra_event_pending;

// Move a fired record onto its target's ready list, keeping the list in ascending
// seq so the commit applies same-instant updates last-executed-wins (LRM 10.4.2).
// Same-instant counts are small, so the linear insert is cheap.
void intra_ready_push(rt_intra_rec* rec) {
    std::vector<void*>& ready = intra_sites[rec->idx].ready;
    size_t              pos   = ready.size();
    while (pos > 0 && reinterpret_cast<rt_intra_rec*>(ready[pos - 1])->seq > rec->seq) {
        --pos;
    }
    ready.insert(ready.begin() + static_cast<std::ptrdiff_t>(pos), rec);
    intra_total++;
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
// cased below. LRM 7.10.4 does not pin the choice.
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
    h->head = (h->head + h->cap - 1) % h->cap;
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
    h->head = (h->head + 1) % h->cap;
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
    sort_run(buf.data(), n, stride, kind, width, rev);
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

// -- Associative arrays (LRM 7.8/7.9) --
//
// The map is keyed by the backend's normalized order-preserving key bytes
// (docs/design/dynamic-arrays.md §4a), so hashing / equality / ordering are all
// key-kind-agnostic byte operations. The value is the element slot bytes (which
// may carry managed sub-handles, serviced through the rt_elem_ops descriptor).
namespace {

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

extern "C" void* rt_intra_alloc(uint32_t idx, uint32_t stride) {
    if (idx >= intra_sites.size()) {
        intra_sites.resize(idx + 1);
    }
    IntraSite& site = intra_sites[idx];
    void*      p;
    if (!site.freelist.empty()) {
        p = site.freelist.back();
        site.freelist.pop_back();
    } else {
        site.pool.push_back(std::make_unique<unsigned char[]>(stride));
        p = site.pool.back().get();
    }
    // Stamp arm order so the commit applies same-instant updates last-executed-wins.
    reinterpret_cast<rt_intra_rec*>(p)->seq = intra_seq++;
    return p;
}

extern "C" void rt_intra_cb(rt_timer* timer, void* /*ctx*/) {
    // The record base is the timer (timer at offset 0), so recover it by cast.
    intra_ready_push(reinterpret_cast<rt_intra_rec*>(timer));
}

extern "C" uint64_t rt_inertial_decide(uint32_t idx, uint64_t new_mm, uint64_t new_z,
                                       uint64_t cur_mm, uint64_t cur_z) {
    if (idx >= inertial_states.size()) {
        inertial_states.resize(idx + 1);
    }
    InertialState& s = inertial_states[idx];
    // LRM 10.3.3 step b: if the new RHS equals the value already scheduled to
    // propagate, keep the existing timer running — do not reschedule.
    if (s.pending && new_mm == s.pmm && new_z == s.pz) {
        return 0;
    }
    // Any other change deschedules the pending store: bump the epoch so a stale
    // record still on the wheel is dropped when it fires (rt_inertial_cb).
    s.epoch++;
    // LRM 10.3.3 step c: the new RHS already equals the current net value, so
    // schedule nothing (and there is no longer a pending store).
    if (new_mm == cur_mm && new_z == cur_z) {
        s.pending = false;
        return 0;
    }
    // LRM 10.3.3 step d: arm a fresh store; the caller stamps this epoch.
    s.pending = true;
    s.pmm     = new_mm;
    s.pz      = new_z;
    return s.epoch;
}

extern "C" void rt_inertial_cb(rt_timer* timer, void* /*ctx*/) {
    auto* rec = reinterpret_cast<rt_intra_rec*>(timer);
    if (rec->idx < inertial_states.size() && rec->epoch == inertial_states[rec->idx].epoch) {
        intra_ready_push(rec); // still current -> commit this instant (pre-active for CA)
        return;
    }
    // Descheduled by a later RHS change: reclaim the record unfired.
    intra_sites[rec->idx].freelist.push_back(rec);
}

extern "C" void rt_intra_event_register(uint32_t trig_id, void* rec) {
    if (trig_id >= intra_event_pending.size()) {
        intra_event_pending.resize(trig_id + 1);
    }
    intra_event_pending[trig_id].push_back(rec);
}

extern "C" void rt_intra_event_fire(uint32_t trig_id) {
    if (trig_id >= intra_event_pending.size()) {
        return;
    }
    std::vector<void*>& pend = intra_event_pending[trig_id];
    size_t              w    = 0; // survivors compacted in place
    for (void* p : pend) {
        auto* rec = reinterpret_cast<rt_intra_rec*>(p);
        if (--rec->count == 0) {
            intra_ready_push(rec); // last awaited edge -> commit this instant
        } else {
            pend[w++] = p; // more edges to wait for
        }
    }
    pend.resize(w);
}

extern "C" void* rt_intra_ready_base(uint32_t idx) {
    return idx < intra_sites.size() ? intra_sites[idx].ready.data() : nullptr;
}

extern "C" uint32_t rt_intra_ready_count(uint32_t idx) {
    return idx < intra_sites.size() ? static_cast<uint32_t>(intra_sites[idx].ready.size()) : 0;
}

extern "C" uint32_t rt_intra_any(void) {
    return intra_total;
}

extern "C" void rt_intra_release(uint32_t idx) {
    if (idx >= intra_sites.size()) {
        return;
    }
    IntraSite& site = intra_sites[idx];
    intra_total -= static_cast<uint32_t>(site.ready.size());
    for (void* p : site.ready) {
        site.freelist.push_back(p);
    }
    site.ready.clear();
}

extern "C" void rt_intra_reset(void) {
    // A fresh simulation has nothing scheduled, so every pooled record is free —
    // including any this-process left armed on the wheel at a prior $finish.
    for (IntraSite& site : intra_sites) {
        site.freelist.clear();
        for (const auto& rec : site.pool) {
            site.freelist.push_back(rec.get());
        }
        site.ready.clear();
    }
    for (std::vector<void*>& pend : intra_event_pending) {
        pend.clear();
    }
    // A fresh simulation has no pending inertial store and starts at epoch 0.
    for (InertialState& s : inertial_states) {
        s = {};
    }
    intra_total = 0;
    intra_seq   = 0;
}

extern "C" void rt_wheel_init(rt_wheel* wheel) {
    new (wheel->storage) RuntimeWheel;
}

extern "C" void rt_wheel_deinit(rt_wheel* wheel) {
    unwrap(wheel)->~RuntimeWheel();
}

extern "C" uint64_t rt_wheel_now(const rt_wheel* wheel) {
    return unwrap(wheel)->now();
}

extern "C" int rt_wheel_pending(const rt_wheel* wheel) {
    return unwrap(wheel)->empty() ? 0 : 1;
}

extern "C" void rt_wheel_schedule(rt_wheel* wheel, rt_timer* timer, uint64_t deadline, rt_timer_fn on_expire) {
    // rt_timer* and clotho::Timer* share a representation, so reinterpreting the
    // callback's first parameter is sound — the same pointer-cast the wheel
    // already relies on for its type-erased dispatch.
    unwrap(wheel)->schedule(unwrap(timer), deadline, reinterpret_cast<RuntimeWheel::Callback>(on_expire));
}

extern "C" void rt_wheel_schedule_rel(rt_wheel* wheel, rt_timer* timer, uint64_t delay, rt_timer_fn on_expire) {
    // now + delay computed runtime-side so a #delay crosses the ABI once, not
    // twice (rt_wheel_now then rt_wheel_schedule). See rt_wheel_schedule for the
    // callback-cast rationale.
    RuntimeWheel& w = *unwrap(wheel);
    w.schedule(unwrap(timer), w.now() + delay, reinterpret_cast<RuntimeWheel::Callback>(on_expire));
}

extern "C" void rt_wheel_tick(rt_wheel* wheel, void* ctx) {
    unwrap(wheel)->tick(ctx);
}

extern "C" int rt_wheel_advance(rt_wheel* wheel, void* ctx) {
    return unwrap(wheel)->advance(ctx) ? 1 : 0;
}

namespace {
// Write already-rendered bytes to a descriptor: an FD's stream, or every set
// channel of an MCD (LRM §21.3.4).
void write_raw(int32_t desc, const std::string& out) {
    if ((desc & kFdBit) != 0) {
        if (std::FILE* f = fd_stream(desc)) {
            std::fwrite(out.data(), 1, out.size(), f);
        }
        return;
    }
    for (int b = 0; b < 31; ++b) {
        if ((desc & (int32_t{ 1 } << b)) != 0 && mcd_channels[b] != nullptr) {
            std::fwrite(out.data(), 1, out.size(), mcd_channels[b]);
        }
    }
}
} // namespace

extern "C" void rt_fwrite(int32_t desc, const char* fmt, uint32_t argc, const rt_arg* argv) {
    io_ensure_init();
    // Write raw: `out` may contain '%' or '{}' that printf/std::print would treat
    // as format syntax.
    std::string out;
    render(out, fmt, argc, argv);
    write_raw(desc, out);
}

namespace {
// The single active $monitor (LRM 21.2.2): its body's ramp function pointer, the
// enable flag, and the last rendered output for change detection. `primed` is
// false until the first emit (or after a re-arm), forcing that first print.
struct MonitorState {
    void*       ramp    = nullptr;
    bool        enabled = false;
    bool        has     = false;
    bool        primed  = false;
    std::string last;
};
MonitorState g_monitor;
} // namespace

extern "C" void rt_monitor_set(void* ramp) {
    g_monitor.ramp    = ramp;
    g_monitor.enabled = true;
    g_monitor.has     = true;
    g_monitor.primed  = false; // print at the first settled instant
    g_monitor.last.clear();
}

extern "C" void rt_monitor_enable(int32_t on) {
    if (!g_monitor.has) {
        return;
    }
    g_monitor.enabled = (on != 0);
    if (on != 0) {
        g_monitor.primed = false; // $monitoron reprints the current values
        g_monitor.last.clear();
    }
}

extern "C" void* rt_monitor_active_ramp() {
    return (g_monitor.has && g_monitor.enabled) ? g_monitor.ramp : nullptr;
}

extern "C" void rt_monitor_emit(int32_t desc, const char* fmt, uint32_t argc, const rt_arg* argv) {
    io_ensure_init();
    std::string out;
    render(out, fmt, argc, argv);
    if (g_monitor.primed && out == g_monitor.last) {
        return; // no argument changed this time step
    }
    g_monitor.primed = true;
    g_monitor.last   = std::move(out);
    write_raw(desc, g_monitor.last);
}

namespace {
// FIFO queue of ramped-but-not-yet-resumed $strobe coroutine handles for the
// current time step (LRM 21.2.2). One growable buffer reused across
// instants: rt_strobe_pop advances a read cursor and only clears (never
// deallocates) once fully drained, so steady-state $strobe use allocates once.
std::vector<void*> g_strobe_queue;
size_t             g_strobe_read = 0;
} // namespace

extern "C" void rt_strobe_pend(void* handle) {
    g_strobe_queue.push_back(handle);
}

extern "C" void* rt_strobe_pop() {
    if (g_strobe_read >= g_strobe_queue.size()) {
        g_strobe_queue.clear();
        g_strobe_read = 0;
        return nullptr;
    }
    return g_strobe_queue[g_strobe_read++];
}

extern "C" int32_t rt_fopen_mcd(const char* name_buf, uint32_t name_len, int32_t name_fwd) {
    io_ensure_init();
    const std::string path = read_ascii_arg(name_buf, name_len, name_fwd);
    // Claim the lowest free channel; bit 0 is the reserved standard output.
    for (int b = 1; b < 31; ++b) {
        if (mcd_channels[b] == nullptr) {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (f == nullptr) {
                return 0;
            }
            mcd_channels[b] = f;
            return int32_t{ 1 } << b;
        }
    }
    return 0; // all 30 channels in use
}

extern "C" int32_t rt_fopen_fd(const char* name_buf, uint32_t name_len, int32_t name_fwd,
                               const char* mode_buf, uint32_t mode_len, int32_t mode_fwd) {
    io_ensure_init();
    const std::string path = read_ascii_arg(name_buf, name_len, name_fwd);
    std::string mode = read_ascii_arg(mode_buf, mode_len, mode_fwd);
    // Always binary. SystemVerilog file I/O is byte-oriented -- $fwrite emits the
    // bytes the format produced -- so a host that rewrites \n as \r\n on the way
    // out (Windows text mode) would make a design's output platform-dependent,
    // and $fgetc/$fread would see bytes the file does not contain. The VCD writer
    // and $readmem already open binary; this makes $fopen agree. A no-op on
    // POSIX, where the two modes are identical.
    if (mode.find('b') == std::string::npos) {
        mode += 'b';
    }
    std::FILE* f = std::fopen(path.c_str(), mode.c_str());
    if (f == nullptr) {
        return 0;
    }
    // Reuse a slot emptied by $fclose before growing the table; indices 0..2 are
    // the reserved standard streams.
    for (uint32_t idx = 3; idx < fd_streams.size(); ++idx) {
        if (fd_streams[idx] == nullptr) {
            fd_streams[idx] = f;
            return kFdBit | static_cast<int32_t>(idx);
        }
    }
    fd_streams.push_back(f);
    return kFdBit | static_cast<int32_t>(fd_streams.size() - 1);
}

extern "C" void rt_fclose(int32_t desc) {
    io_ensure_init();
    if ((desc & kFdBit) != 0) {
        const uint32_t idx = static_cast<uint32_t>(desc & ~kFdBit);
        if (idx >= 3 && idx < fd_streams.size() && fd_streams[idx] != nullptr) {
            std::fclose(fd_streams[idx]);
            fd_streams[idx] = nullptr;
        }
        return;
    }
    // MCD: close every set channel except the preopened standard output (bit 0).
    for (int b = 1; b < 31; ++b) {
        if ((desc & (int32_t{ 1 } << b)) != 0 && mcd_channels[b] != nullptr) {
            std::fclose(mcd_channels[b]);
            mcd_channels[b] = nullptr;
        }
    }
}

// The $system access tier, set once by run_jit before the design runs
// (0 = off, 1 = const, 2 = full). Default off so a JIT that somehow reaches
// rt_system without run_jit having enabled it is refused, not run.
int g_system_access = 0;

extern "C" void rt_system_set_access(int32_t tier) {
    g_system_access = tier;
}

extern "C" int32_t rt_system(const char* cmd_buf, uint32_t cmd_len, int32_t cmd_fwd,
                             int32_t has_arg) {
    // Runtime backstop behind the compile-time --allow-system gate. The frontend
    // refuses to emit this call unless the operator opted in, so reaching here
    // with the tier off means the gate was bypassed — abort loudly rather than
    // run a shell command.
    if (g_system_access == 0) {
        format_fatal("$system reached with shell execution disabled (--allow-system)");
    }
    if (has_arg == 0) {
        // No-arg $system(): probe for a shell (system(NULL) -> nonzero if one
        // exists). LRM §20.17.1.
        return std::system(nullptr);
    }
    const std::string cmd = read_ascii_arg(cmd_buf, cmd_len, cmd_fwd);
    return std::system(cmd.c_str());
}

extern "C" int32_t rt_fgetc(int32_t fd) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    return f != nullptr ? std::fgetc(f) : -1;
}

extern "C" int32_t rt_ungetc(int32_t c, int32_t fd) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    return f != nullptr ? std::ungetc(c, f) : -1;
}

extern "C" int32_t rt_feof(int32_t fd) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    return (f == nullptr || std::feof(f) != 0) ? 1 : 0;
}

extern "C" int32_t rt_ftell(int32_t fd) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    return f != nullptr ? static_cast<int32_t>(std::ftell(f)) : -1;
}

extern "C" int32_t rt_fseek(int32_t fd, int32_t offset, int32_t operation) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    if (f == nullptr) {
        return -1;
    }
    int whence;
    switch (operation) {
        case 0:
            whence = SEEK_SET;
            break;
        case 1:
            whence = SEEK_CUR;
            break;
        case 2:
            whence = SEEK_END;
            break;
        default:
            return -1;
    }
    return std::fseek(f, offset, whence) == 0 ? 0 : -1;
}

extern "C" int32_t rt_rewind(int32_t fd) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    if (f == nullptr) {
        return -1;
    }
    std::rewind(f);
    return 0;
}

extern "C" int32_t rt_fgets(uint8_t* mm, uint8_t* z, uint32_t width, int32_t fd, uint32_t flags) {
    io_ensure_init();
    std::FILE*     f        = fd_stream(fd);
    const uint32_t capacity = width / 8;
    if (f == nullptr || capacity == 0) {
        return 0;
    }
    // Read up to `capacity` characters, stopping once a newline is stored (LRM
    // §21.3.4).
    std::vector<uint8_t> line;
    line.reserve(capacity);
    while (line.size() < capacity) {
        const int c = std::fgetc(f);
        if (c == EOF) {
            break;
        }
        line.push_back(static_cast<uint8_t>(c));
        if (c == '\n') {
            break;
        }
    }
    const uint32_t count = static_cast<uint32_t>(line.size());
    if (count == 0) {
        return 0;
    }
    store_string_planes(mm, z, width, line.data(), count, (flags & 1u) != 0);
    return static_cast<int32_t>(count);
}

extern "C" void rt_sformat(uint8_t* mm, uint8_t* z, uint32_t width, const char* fmt, uint32_t argc,
                           const rt_arg* argv, uint32_t flags) {
    // $sformat / $swrite: render like $display, then store the result into the
    // destination string variable (LRM §21.3.3). An oversized result keeps its low
    // (rightmost) bytes, matching a string-to-narrower-reg assignment.
    std::string out;
    render(out, fmt, argc, argv);
    store_string_planes(mm, z, width, reinterpret_cast<const uint8_t*>(out.data()),
                        static_cast<uint32_t>(out.size()), (flags & 1u) != 0);
}

extern "C" void rt_sformat_str(rt_string* dst, const char* fmt, uint32_t argc, const rt_arg* argv,
                               const rt_alloc_site* site) {
    std::string out;
    render(out, fmt, argc, argv);
    rt_string_assign_bytes(dst, out.data(), out.size(), site);
}

extern "C" int32_t rt_fgets_str(rt_string* dst, int32_t fd, const rt_alloc_site* site) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    if (f == nullptr) {
        return 0;
    }
    // No fixed width to cap at (unlike rt_fgets's packed-vector capacity): read
    // the whole line, stopping after a newline or at EOF.
    std::string line;
    for (;;) {
        const int c = std::fgetc(f);
        if (c == EOF) {
            break;
        }
        line.push_back(static_cast<char>(c));
        if (c == '\n') {
            break;
        }
    }
    if (line.empty()) {
        return 0;
    }
    rt_string_assign_bytes(dst, line.data(), line.size(), site);
    return static_cast<int32_t>(line.size());
}

extern "C" int32_t rt_ferror(int32_t fd, uint8_t* mm, uint8_t* z, uint32_t width, uint32_t flags) {
    // $ferror (LRM §21.3.6): return the stream's error code and write the matching
    // message into `str`. C stdio has no per-stream error code, only an indicator,
    // so errno is the closest portable source. A cleared stream reports 0.
    // TODO: the LRM's exact error-code values are implementation-defined; this
    // surfaces errno / strerror rather than a fixed table.
    io_ensure_init();
    std::FILE*  f    = fd_stream(fd);
    int         code = 0;
    const char* msg  = "";
    if (f == nullptr) {
        code = -1;
        msg  = "invalid file descriptor";
    } else if (std::ferror(f) != 0) {
        code = errno;
        msg  = std::strerror(errno);
    }
    store_string_planes(mm, z, width, reinterpret_cast<const uint8_t*>(msg),
                        static_cast<uint32_t>(std::strlen(msg)), (flags & 1u) != 0);
    return code;
}

extern "C" int32_t rt_ferror_str(int32_t fd, rt_string* dst, const rt_alloc_site* site) {
    io_ensure_init();
    std::FILE*  f    = fd_stream(fd);
    int         code = 0;
    const char* msg  = "";
    if (f == nullptr) {
        code = -1;
        msg  = "invalid file descriptor";
    } else if (std::ferror(f) != 0) {
        code = errno;
        msg  = std::strerror(errno);
    }
    rt_string_assign_bytes(dst, msg, std::strlen(msg), site);
    return code;
}

namespace {

// Defined below, next to $readmem which shares them.
int hex_digit(char c);
int bin_digit(char c);

// A character source for the scan engine: a file ($fscanf) or an in-memory buffer
// ($sscanf), with one character of pushback.
struct ScanSource {
    std::FILE*     f   = nullptr;
    const uint8_t* buf = nullptr;
    size_t         len = 0;
    size_t         pos = 0;

    int get() {
        if (f != nullptr) {
            return std::fgetc(f);
        }
        return pos < len ? buf[pos++] : EOF;
    }
    void unget(int c) {
        if (c == EOF) {
            return;
        }
        if (f != nullptr) {
            std::ungetc(c, f);
        } else if (pos > 0) {
            --pos;
        }
    }
};

bool scan_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Skip a run of input whitespace, pushing back the first non-space character.
void skip_ws(ScanSource& src) {
    int c;
    while ((c = src.get()) != EOF && scan_ws(c)) {
    }
    src.unget(c);
}

// The $fscanf / $sscanf engine (LRM §21.3.4). Walks `fmt`, matching literal text
// and whitespace against `src` and converting each %-directive into the next
// output descriptor. Supports d/o/b/h/x (with x/z digits on the power-of-two
// bases), c, and s, plus `*` suppression and an optional maximum field width.
// Returns the number of stored conversions, or -1 if the input is exhausted before
// the first conversion (matching $fscanf's EOF result). Unsupported conversions are
// rejected by the frontend, so `fmt` only carries the above.
int32_t scan_run(ScanSource& src, const char* fmt, uint32_t argc, const rt_scan_arg* args) {
    uint32_t ai        = 0;
    int32_t  count     = 0;
    bool     any_input = false;

    // Value planes accumulate into word arrays sized to the widest target: a
    // target wider than 64 bits assembles multi-word, a <= 64-bit target uses
    // word 0 and reduces to the scalar accumulation. write_word_planes masks each
    // conversion to its own arg's width.
    uint32_t words = 1;
    for (uint32_t k = 0; k < argc; ++k) {
        words = std::max(words, plane_words(args[k].width));
    }
    std::vector<uint64_t> must(words);
    std::vector<uint64_t> may(words);
    std::vector<uint64_t> z(words);

    for (const char* p = fmt; *p != '\0'; ++p) {
        if (scan_ws(static_cast<unsigned char>(*p))) {
            skip_ws(src);
            continue;
        }
        if (*p != '%') {
            const int c = src.get();
            if (c != static_cast<unsigned char>(*p)) {
                src.unget(c);
                break; // literal mismatch (or EOF) ends the scan
            }
            any_input = true;
            continue;
        }

        ++p;
        if (*p == '%') {
            const int c = src.get();
            if (c != '%') {
                src.unget(c);
                break;
            }
            continue;
        }

        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            ++p;
        }
        uint32_t maxw = 0;
        while (*p >= '0' && *p <= '9') {
            maxw = maxw * 10 + static_cast<uint32_t>(*p - '0');
            ++p;
        }
        const char conv = *p;
        if (conv == '\0') {
            break;
        }

        std::fill(must.begin(), must.end(), 0);
        std::fill(may.begin(), may.end(), 0);
        std::fill(z.begin(), z.end(), 0);
        bool ok = false;

        if (conv == 'c') {
            const int c = src.get();
            if (c != EOF) {
                any_input   = true;
                must[0] = may[0] = static_cast<uint8_t>(c);
                ok               = true;
            }
        } else if (conv == 's') {
            skip_ws(src);
            std::vector<uint8_t> tok;
            int                  c;
            while ((c = src.get()) != EOF && !scan_ws(c)) {
                if (maxw != 0 && tok.size() >= maxw) {
                    src.unget(c);
                    break;
                }
                tok.push_back(static_cast<uint8_t>(c));
            }
            if (scan_ws(c)) {
                src.unget(c);
            }
            if (!tok.empty()) {
                any_input = true;
                // Right-justify the token bytes (byte 0 = last char) into `must`,
                // capped at the widest target; write_word_planes masks to width.
                const size_t cap  = static_cast<size_t>(words) * 8;
                const size_t keep = tok.size() < cap ? tok.size() : cap;
                for (size_t k = 0; k < keep; ++k) {
                    must[k / 8] |= static_cast<uint64_t>(tok[tok.size() - 1 - k]) << (8 * (k % 8));
                }
                std::copy(must.begin(), must.end(), may.begin());
                ok = true;
            }
        } else {
            // Numeric: d (signed decimal), o / b / h / x (power-of-two bases, with
            // x/z digits marking unknown bits like $readmem).
            // NOTE: a token shorter than the target zero-extends its high bits (the
            // natural value read). Whether it x-extends when the token carries
            // unknowns is not pinned down by the LRM (§21.3.4), so the simpler
            // zero-extension is used.
            const int base = conv == 'd' ? 10 : conv == 'o' ? 8 : conv == 'b' ? 2 : 16;
            const int bits = base == 16 ? 4 : base == 8 ? 3 : base == 2 ? 1 : 0;
            skip_ws(src);
            bool     neg  = false;
            uint32_t used = 0;
            int      c    = src.get();
            if (base == 10 && (c == '+' || c == '-')) {
                neg = c == '-';
                c   = src.get();
            }
            const uint64_t dmask = bits != 0 ? static_cast<uint64_t>((1u << bits) - 1) : 0;
            while (c != EOF) {
                if (maxw != 0 && used >= maxw) {
                    break;
                }
                uint64_t dv = 0;
                uint64_t mv = 0;
                uint64_t zz = 0;
                if (base != 10 && (c == 'x' || c == 'X')) {
                    mv = dmask;
                } else if (base != 10 && (c == 'z' || c == 'Z' || c == '?')) {
                    mv = zz = dmask;
                } else {
                    int hv;
                    if (base == 10) {
                        hv = (c >= '0' && c <= '9') ? c - '0' : -1;
                    } else if (base == 8) {
                        hv = (c >= '0' && c <= '7') ? c - '0' : -1;
                    } else if (base == 2) {
                        hv = bin_digit(static_cast<char>(c));
                    } else {
                        hv = hex_digit(static_cast<char>(c));
                    }
                    if (hv < 0) {
                        break;
                    }
                    dv = static_cast<uint64_t>(hv);
                    mv = dv; // known digit: may == must
                }
                if (base == 10) {
                    wide_mul_add(must.data(), words, 10, dv);
                } else {
                    wide_shl(must.data(), words, bits);
                    must[0] |= dv;
                    wide_shl(may.data(), words, bits);
                    may[0] |= mv;
                    wide_shl(z.data(), words, bits);
                    z[0] |= zz;
                }
                ok        = true;
                any_input = true;
                ++used;
                c = src.get();
            }
            src.unget(c);
            if (ok && base == 10) {
                if (neg) {
                    wide_negate(must.data(), words);
                }
                std::copy(must.begin(), must.end(), may.begin()); // decimal: no x/z
            }
        }

        if (!ok) {
            break; // conversion failed; stop like C scanf
        }
        if (!suppress) {
            if (ai >= argc) {
                break; // more conversions than output args
            }
            const rt_scan_arg& a      = args[ai++];
            const bool         narrow = (a.flags & 1u) != 0;
            if (a.width > 64) {
                write_word_planes_wide(a.mm, a.z, a.width, must.data(), may.data(), z.data(), narrow);
            } else {
                write_word_planes(a.mm, a.z, a.width, must[0], may[0], z[0], narrow);
            }
            ++count;
        }
    }

    return (count == 0 && !any_input) ? -1 : count;
}

} // namespace

extern "C" int32_t rt_fscanf(int32_t fd, const char* fmt, uint32_t argc, const rt_scan_arg* args) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    if (f == nullptr) {
        return -1;
    }
    ScanSource src;
    src.f = f;
    return scan_run(src, fmt, argc, args);
}

extern "C" int32_t rt_sscanf(const char* buf, uint32_t len, int32_t fwd, const char* fmt,
                             uint32_t argc, const rt_scan_arg* args) {
    // The source is a packed-ASCII vector (reversed) or a dynamic string (fwd,
    // natural order); reconstruct the string, then scan it.
    const std::string s = read_ascii_arg(buf, len, fwd);
    ScanSource        src;
    src.buf = reinterpret_cast<const uint8_t*>(s.data());
    src.len = s.size();
    return scan_run(src, fmt, argc, args);
}

extern "C" void rt_timeformat_init(uint64_t tick_fs, int32_t default_units) {
    timeformat.tick_fs   = tick_fs == 0 ? 1 : tick_fs;
    timeformat.units     = default_units;
    timeformat.precision = 0;
    timeformat.min_width = 20;
    timeformat.suffix.clear();
}

extern "C" void
rt_timeformat_set(int32_t units, int32_t precision, const char* suffix, uint32_t suffix_len, int32_t min_width) {
    timeformat.units     = units;
    timeformat.precision = precision;
    timeformat.min_width = min_width;
    timeformat.suffix.assign(suffix, suffix_len);
}

namespace {

// Where rt_finish returns control: the run_jit frame that started the
// simulation, or null when the design runs standalone.
// Append the simulation time `now` (in design ticks) as "<value> <unit>",
// reduced to the largest SI unit that divides it evenly (fs / ps / ns / us / ms
// / s). A unitless design (tick_fs == 0) prints the raw tick count. Used only by
// the $finish banner, so a plain best-effort rendering — not the %t format.
void append_finish_time(std::string& out, uint64_t now) {
    const uint64_t tick_fs = timeformat.tick_fs;
    if (tick_fs == 0) {
        out += std::to_string(now);
        return;
    }
    // Total femtoseconds; 128-bit to avoid overflow on a coarse precision.
    const unsigned __int128 fs = static_cast<unsigned __int128>(now) * tick_fs;
    static const struct {
        uint64_t    div;
        const char* unit;
    } table[] = { { 1000000000000000ull, "s" }, { 1000000000000ull, "ms" },
                  { 1000000000ull, "us" },       { 1000000ull, "ns" },
                  { 1000ull, "ps" },             { 1ull, "fs" } };
    // At now == 0 every unit divides; pick the design precision's unit so the
    // output reads "0 <precision-unit>" rather than "0 s".
    const unsigned __int128 pick = fs == 0 ? tick_fs : fs;
    for (const auto& e : table) {
        if (pick % e.div == 0) {
            out += std::to_string(static_cast<uint64_t>(fs / e.div));
            out += ' ';
            out += e.unit;
            return;
        }
    }
}

// Peak resident set (in kilobytes) and total process CPU time, for the
// $finish(2) resource line. Normalizing the unit here matters: getrusage
// reports ru_maxrss in kilobytes on Linux but in *bytes* on the BSDs (macOS
// included), so reading the field raw would overstate macOS by 1024x. Windows
// has no getrusage at all and goes through the Win32 equivalents.
struct FinishStats {
    uint64_t peak_rss_kb;
    double   cpu_seconds;
};

FinishStats finish_stats() {
#if defined(_WIN32)
    // FILETIME counts 100-nanosecond ticks; the creation/exit pair is unused.
    FILETIME creation = {}, exited = {}, kernel = {}, user = {};
    double   cpu = 0.0;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user)) {
        const auto ticks = [](const FILETIME& ft) {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        cpu = static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
    }
    PROCESS_MEMORY_COUNTERS pmc = {};
    uint64_t                rss_kb = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        rss_kb = static_cast<uint64_t>(pmc.PeakWorkingSetSize) / 1024;
    }
    return {rss_kb, cpu};
#else
    rusage ru = {};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                       static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
#if defined(__APPLE__)
    const uint64_t rss_kb = static_cast<uint64_t>(ru.ru_maxrss) / 1024;
#else
    const uint64_t rss_kb = static_cast<uint64_t>(ru.ru_maxrss);
#endif
    return {rss_kb, cpu};
#endif
}

} // namespace

unsigned char rt_finishing = 0;

namespace {

// Everything a $finish does before control leaves: the Observed drain, the
// banner, and closing the dump.
void finish_report(const char* banner, uint64_t now, int32_t arg) {
    rt_observed_flush();
    if (arg >= 1) {
        std::fflush(stdout);
        std::string line = banner;
        line += " called at ";
        append_finish_time(line, now);
        if (arg >= 2) {
            const FinishStats st = finish_stats();
            char              stats[128];
            std::snprintf(stats, sizeof(stats),
                          "\n  Memory: %" PRIu64 " KB  CPU time: %.3f seconds", st.peak_rss_kb,
                          st.cpu_seconds);
            line += stats;
        }
        line += '\n';
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }
    // Flush and close the VCD while the design's stack (state + wheel the engine
    // points at) is still live: the drain stops before its own close at
    // wheel-drain. A no-op when no dump is open.
    rt_vcd_close();
}

} // namespace

extern "C" void rt_finish_coop(const char* banner, uint64_t now, int32_t arg) {
    finish_report(banner, now, arg);
    rt_finishing = 1;
    // Returns. The caller is a JIT'd body, which leaves through its own
    // epilogue; each frame between here and the drain loop is released by its
    // owner, and the drain sees rt_finishing and stops.
}

extern "C" void rt_oscillation_abort(const rt_wheel* wheel) {
    // Read `now` from the wheel here rather than materializing it in the JIT'd
    // drain loop: this abort is a cold error path, and keeping rt_wheel_now out
    // of the generated IR preserves the delay path's single-ABI-crossing shape
    // (rt_wheel_schedule_rel only; see the drain-loop guard).
    const uint64_t now = rt_wheel_now(wheel);
    // A single time step activated more than the delta-cycle limit of processes
    // without settling: a zero-delay oscillation the scheduler cannot converge
    // (e.g. two `always @*` blocks writing a shared variable each is sensitive
    // to). clotho's per-store wake semantics do not coalesce transient writes the
    // way a net-change scheduler would, so such a design fails to converge.
    // Terminate loudly instead of hanging (a diagnostic, not a design value).
    std::fflush(stdout);
    std::string line = "FATAL: simulation did not converge at ";
    append_finish_time(line, now);
    line += " (zero-delay oscillation: delta-cycle activation limit exceeded)\n";
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    rt_vcd_close();
    rt_finishing = 1;
    // A non-converging design cannot be handed back to the host mid-instant --
    // there is no settled state to return -- so this one path still ends the
    // process. std::exit, not a longjmp: it is portable, and it keeps the
    // nonzero status that distinguishes a hang from a clean run.
    std::exit(1);
}

extern "C" void rt_ring_overflow(uint32_t ring, uint32_t capacity) {
    // The scheduler's Active / Reactive rings are sized statically from the
    // design (ensure_active_queue): one slot per process plus one per
    // concurrently-live `fork` child. Reaching this means that count was too
    // small -- an unbounded shape the static bound saturates on (recursion into a
    // forking task), or a bug in the bound itself. The alternative to reporting
    // is queue[tail & mask] overwriting a live coroutine handle, which corrupts
    // the scheduler and surfaces as a hang or a wrong answer with no diagnostic.
    const uint64_t now = rt_diag_now();
    std::fflush(stdout);
    std::string line = "FATAL: the ";
    line += ring == 0 ? "active" : "reactive";
    line += " scheduler ring overflowed at ";
    append_finish_time(line, now);
    line += fmt::format(" (capacity {} coroutine handles)\n", capacity);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_oscillation_abort: there is no settled state to hand
    // back mid-instant, so this path ends the process with a nonzero status.
    std::exit(1);
}

// ---- Observed region (LRM 4.4.2.4) deferred reports ----
//
// A report produced while the Active/NBA regions settle an instant is not
// emitted at its point of execution; it is queued here and flushed in the
// Observed region once the instant settles (rt_observed_flush, called from the
// sim loop between the NBA-settle and Postponed regions). This moves
// unique/priority violation reporting to the region LRM 12.4.2 / 12.5.3 specify
// (previously emitted immediately in the Active region).
//
// Step 1 carries only the record — no maturation/flush-on-re-execution yet, so
// a statement that violates transiently on multiple settle passes still queues
// one report per pass. Deferred immediate assertions (LRM 16.4) will add the
// per-instance maturation that gives glitch immunity, reusing this queue.
namespace {
struct ObservedCaseViol {
    const char* banner; // compile-time "<file>:<line>: " prefix, static lifetime
    uint64_t    now;    // instant the violation occurred (== Observed flush time)
    int32_t     kind;   // 0 = no match, 1 = overlap
};
std::vector<ObservedCaseViol> g_observed_case;
} // namespace

extern "C" void rt_observed_case_violation(const char* banner, uint64_t now, int32_t kind) {
    g_observed_case.push_back({ banner, now, kind });
}

extern "C" void rt_observed_flush() {
    if (g_observed_case.empty()) {
        return;
    }
    std::fflush(stdout);
    for (const ObservedCaseViol& r : g_observed_case) {
        std::string line = r.banner;
        line += r.kind == 0 ? "no condition is true in the unique/priority if/case at time "
                            : "if/case statement is not unique (overlapping items) at time ";
        append_finish_time(line, r.now);
        line += '\n';
        std::fputs(line.c_str(), stderr);
    }
    std::fflush(stderr);
    g_observed_case.clear();
}

extern "C" void rt_severity(int32_t level, const char* banner, const char* fmt, uint32_t argc,
                            const rt_arg* argv) {
    // $info / $warning / $error / $fatal (LRM 20.10) or an immediate-assertion
    // default failure (LRM 16.3). A simulator diagnostic, not design output, so
    // it goes to stderr like the $finish / case-violation banners; flush stdout
    // first so buffered $display text is committed before the report interleaves.
    // The exact layout is not LRM-mandated; "<LABEL>: <file>:
    // <line>: <message>" tracks iverilog's first report line.
    static const char* const labels[] = { "INFO", "WARNING", "ERROR", "FATAL" };
    const char* const        label    = labels[level < 4 ? level : 2];
    io_ensure_init();
    std::fflush(stdout);
    std::string line = label;
    line += ": ";
    line += banner;
    render(line, fmt, argc, argv);
    line += '\n';
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

// ---- --sanitize=bounds reports ----

namespace {

// Reports emitted per access site before the site goes quiet. An out-of-range
// index in an always block re-fires on every activation, and an unbounded
// stream of identical lines buries both the first report and the design's own
// output. Not configurable yet; a --sanitize option for it can come when
// something needs it.
constexpr uint32_t bounds_report_cap = 8;

// Reports already emitted, keyed by access site: the emitted "<file>:<line>:
// <col>" string's address (one per instrumented access, stable for the run)
// with the read/write direction folded in, so a read and a write on the same
// line are counted apart. Scaled rather than bit-packed because a string
// constant carries no alignment guarantee.
ankerl::unordered_dense::map<uint64_t, uint32_t> g_bounds_hits;

// Whether a sanitizer report may use colour: a terminal that is not `dumb`,
// with NO_COLOR unset (https://no-color.org). Escape codes in a redirected log
// or a test's captured stderr are corruption, not decoration. Resolved once;
// the answer cannot change while the process runs.
bool diag_color() {
    static const bool on = []
    {
#if defined(_WIN32)
        const bool tty = _isatty(_fileno(stderr)) != 0;
#else
        const bool tty = isatty(fileno(stderr)) != 0;
#endif
        const char* term = std::getenv("TERM");
        return tty && std::getenv("NO_COLOR") == nullptr &&
               (term == nullptr || std::strcmp(term, "dumb") != 0);
    }();
    return on;
}

// `s` when colour is on, plain otherwise. fmt emits no escape at all for a
// default-constructed style, so one call site serves both.
fmt::text_style styled(fmt::text_style s) {
    return diag_color() ? s : fmt::text_style{};
}

// One run on a diagnostic's marker row: `width` copies of `ch` starting under
// 1-based source column `column`. A mark at column 0 (nothing to point at) is
// dropped by print_snippet.
struct SnippetMark {
    int32_t column;
    size_t  width;
    char    ch;
};

// The source line a marker row will be measured against. A tab is one column to
// slang, so it must occupy one column here too or the caret drifts. Rendering it
// as a single space keeps the arithmetic exact at the cost of the source's own
// indentation width.
std::string snippet_text(const char* src) {
    std::string text = src;
    std::replace(text.begin(), text.end(), '\t', ' ');
    return text;
}

// The "<n> | <source>" row and, under it, the marker row carrying `marks`. The
// row is built as one padded string so two marks cannot fight over spacing;
// later marks overwrite earlier ones where they overlap. The text row puts a
// space after the bar, so source column N is offset N in the marker row.
void print_snippet(const std::string& text, int32_t line_no,
                   std::initializer_list<SnippetMark> marks, const fmt::text_style& loc,
                   const fmt::text_style& caret) {
    const std::string number = std::to_string(line_no);
    const std::string gutter(number.size(), ' ');
    fmt::print(stderr, loc, "{} |", number);
    fmt::print(stderr, " {}", text);
    fmt::print(stderr, "\n");
    std::string row;
    for (const SnippetMark& m : marks) {
        if (m.column <= 0 || static_cast<size_t>(m.column) > text.size() + 1) {
            continue;
        }
        const size_t at = static_cast<size_t>(m.column);
        const size_t n  = std::min(m.width, text.size() + 1 - at);
        if (row.size() < at + n) {
            row.resize(at + n, ' ');
        }
        row.replace(at, n, n, m.ch);
    }
    if (row.empty()) {
        return;
    }
    fmt::print(stderr, loc, "{} |", gutter);
    fmt::print(stderr, caret, "{}", row);
    fmt::print(stderr, "\n");
}

// Width of a recorded [column, end_column) source extent; 1 when the frontend
// recorded no extent, which draws a bare caret.
size_t mark_width(int32_t column, int32_t end_column) {
    return end_column > column ? static_cast<size_t>(end_column - column) : 1;
}

} // namespace

extern "C" void rt_bounds_report(const rt_bounds_site* site, int64_t index, int64_t extent,
                                 uint32_t flags, uint64_t now) {
    const bool     write   = (flags & 1u) != 0;
    const bool     unknown = (flags & 2u) != 0;
    const bool     partial = (flags & 4u) != 0;
    const uint64_t key     = reinterpret_cast<uint64_t>(site) * 2 + (write ? 1 : 0);
    uint32_t&      hits    = g_bounds_hits[key];
    if (hits >= bounds_report_cap) {
        return;
    }
    ++hits;
    // A simulator diagnostic, not design output, so it goes to stderr like the
    // severity and case-violation reports; flush stdout first so buffered
    // $display text is committed before the warning interleaves.
    io_ensure_init();
    std::fflush(stdout);

    // Laid out like a modern compiler diagnostic: what went wrong, where, the
    // offending line with a caret under the access, then the context that fits
    // in none of those. The array's name and the simulation time are notes
    // rather than headline because the snippet already shows the access.
    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::yellow));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::yellow));

    std::string when;
    append_finish_time(when, now);
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break
    // leaves the style live across it.
    fmt::print(stderr, label, "warning:");
    // What was indexed, and how it was bounded. A runtime-sized object reports
    // the size it actually had; a declared one reports the range as written.
    // An x/z index is invalid by LRM 7.4.5 / Table 7-1 with no numeric value to
    // report, so it prints as what it is rather than as its must-plane bits.
    const char* noun = "index out of bounds";
    switch (site->what) {
        case RT_BOUNDS_BIT: noun = "bit select out of bounds"; break;
        case RT_BOUNDS_PART: noun = partial ? "part select partly out of bounds"
                                            : "part select out of bounds"; break;
        case RT_BOUNDS_CHAR: noun = "string index out of bounds"; break;
        case RT_BOUNDS_QUEUE_OP: noun = "queue index out of bounds"; break;
        case RT_BOUNDS_EMPTY_POP: noun = "pop from an empty queue"; break;
        default: break;
    }
    if (site->what == RT_BOUNDS_EMPTY_POP) {
        // No index and no bound to name: the headline is the whole finding.
        fmt::print(stderr, head, " {}", noun);
    } else {
        const std::string idx   = unknown ? std::string("x") : std::to_string(index);
        const std::string subj  = site->what == RT_BOUNDS_PART
                                    ? fmt::format("index {} +: {}", idx, site->width)
                                    : fmt::format("index {}", idx);
        const char*       unit  = site->what == RT_BOUNDS_CHAR ? "length"
                                : (site->what == RT_BOUNDS_BIT || site->what == RT_BOUNDS_PART)
                                  ? "width"
                                  : "size";
        const std::string bound = extent >= 0 ? fmt::format("{} {}", unit, extent)
                                              : fmt::format("range [{}:{}]", site->left, site->right);
        fmt::print(stderr, head, " {}: {}, {}", noun, subj, bound);
    }
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        // The marker row: `^^^` under the index expression, `---` under the
        // array it indexes when both landed on this line.
        SnippetMark       acc{ 0, 0, '-' };
        if (site->acc_line_no == site->line_no) {
            size_t n = mark_width(site->acc_column, site->acc_end_column);
            if (n == 1) {
                // No extent recorded for the indexed expression. Fall back to
                // its name, and only where the source really spells it at that
                // column -- a hierarchical `name` is not what the statement
                // wrote, and the leaf may be renamed by a `` `define ``.
                const char*       dot  = std::strrchr(site->name, '.');
                const std::string leaf = dot != nullptr ? dot + 1 : site->name;
                const size_t      at   = static_cast<size_t>(site->acc_column) - 1;
                if (site->acc_column > 0 && at + leaf.size() <= text.size() &&
                    text.compare(at, leaf.size(), leaf) == 0) {
                    n = leaf.size();
                }
            }
            acc = SnippetMark{ site->acc_column, n, '-' };
        }
        print_snippet(text, site->line_no,
                      { acc,
                        SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " {} '{}' at {}", write ? "write to" : "read of", site->name, when);
    fmt::print(stderr, "\n");
    if (hits == bounds_report_cap) {
        fmt::print(stderr, tag, "  note:");
        fmt::print(stderr, body, " further reports from this access are suppressed");
        fmt::print(stderr, "\n");
    }
    std::fflush(stderr);
}

// ---- Null class-handle dereference (LRM 8.4) ----

extern "C" void rt_null_handle(const rt_null_site* site, uint64_t now) {
    // Reached only from the guard the backend plants ahead of every class-handle
    // dereference and every virtual dispatch. LRM 8.4 leaves the result of an
    // access through a null handle indeterminate and permits an implementation
    // error; the untrapped alternative is an inttoptr to address zero and a
    // segfault carrying nothing that points back at the design. Laid out like the
    // bounds report -- what went wrong, where, the offending line under a caret,
    // then the notes -- except that this one is fatal.
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
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " dereference of a null class handle");
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " the handle holds no object at {} -- it was never assigned one, or "
                             "was assigned null",
               when);
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " construct the object with `new` before reaching a member "
                             "through it (LRM 8.4)");
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_oscillation_abort: there is no defined state to hand
    // back to the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}

// ---- Stale `ref` through a restructured container (LRM 13.5.2) ----

namespace {

// Whether a stale `ref` dereference warns and continues instead of ending the
// run (--loose-stale-ref). Process-wide like g_system_access: the host sets it
// once before the design runs, and the simulation thread only reads it.
int g_ref_stale_warn = 0;

// Dereference sites that have already warned. A stale deref inside an always
// block re-fires on every activation, and an unbounded stream of identical
// lines buries both the first report and the design's own output. Keyed on the
// rt_ref_site global's address: the backend emits one per guarded dereference
// and it is stable for the run. Only ever reached under --loose-stale-ref.
ankerl::unordered_dense::set<const void*> g_ref_stale_seen;

} // namespace

extern "C" void rt_ref_stale_warn(int32_t enable) {
    g_ref_stale_warn = enable;
}

extern "C" void rt_ref_stale(const rt_ref_site* site, uint64_t expect, uint64_t actual,
                             uint64_t now) {
    // Reached only from the guard the backend plants ahead of every dereference
    // through a `ref` formal, and only in the `actual > expect` direction -- the
    // guard routes the impossible one to rt_ref_epoch_regress instead. The
    // container was restructured while the ref was live, so the interior pointer
    // no longer names the actual and LRM 13.5.2's "the formal refers to the
    // actual" has stopped holding. Same layout as the null-handle fatal, except
    // that --loose-stale-ref downgrades this one to a once-per-site warning.
    const bool warn = g_ref_stale_warn != 0;
    if (warn && !g_ref_stale_seen.insert(site).second) {
        return;
    }
    io_ensure_init();
    std::fflush(stdout);

    const fmt::terminal_color accent =
        warn ? fmt::terminal_color::yellow : fmt::terminal_color::red;
    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(accent));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(accent));

    std::string when;
    append_finish_time(when, now);
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    fmt::print(stderr, label, "{}", warn ? "warning:" : "fatal:");
    fmt::print(stderr, head,
               " ref '{}' is stale: '{}' was restructured (epoch {}, ref formed at epoch {})",
               site->formal, site->container, actual, expect);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " a push, insert, delete, resize, reorder or whole-value assignment "
                             "to '{}' by {} moved the element '{}' was bound to",
               site->container, when, site->formal);
    fmt::print(stderr, "\n");
    if (warn) {
        fmt::print(stderr, tag, "  note:");
        fmt::print(stderr, body, " --loose-stale-ref: proceeding through the stale pointer anyway, "
                                 "and further reports from this dereference are suppressed");
        fmt::print(stderr, "\n");
        std::fflush(stderr);
        return;
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " do not restructure '{}' while the ref is live, or pass the element "
                             "by value instead (LRM 13.5.2)",
               site->container);
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_null_handle: there is no defined state to hand back to
    // the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}

extern "C" void rt_ref_epoch_regress(const rt_ref_site* site, uint64_t expect, uint64_t actual,
                                     uint64_t now) {
    // The guard reached this because the container's epoch is BELOW the one the
    // ref recorded. Every bump is a ++ and no path resets the field, so a
    // container's epoch can only ever climb: no design can produce this. It means
    // the epoch plumbing itself is wrong -- a missing bump, a handle whose epoch
    // was copied rather than advanced, or a wrong value captured when the ref was
    // formed. --loose-stale-ref deliberately does not apply, because the invariant
    // being violated is Clotho's and not the design's.
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
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " internal error: the relocation epoch of '{}' ran backwards",
               site->container);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " '{}' is at epoch {} but ref '{}' was formed at epoch {}, at {}",
               site->container, actual, site->formal, expect, when);
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " this is a bug in Clotho's epoch plumbing, not an error in the "
                             "design -- please report it with the source line above "
                             "(docs/design/data-model.md section 3)");
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_null_handle: there is no defined state to hand back to
    // the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}

extern "C" void rt_tagged_access(const rt_tagged_site* site, uint64_t tag, uint32_t tag_unknown,
                                 uint64_t now) {
    // Reached only from the guard the backend plants ahead of every tagged-union
    // member read and member assignment. LRM 11.9 makes such an access "completely
    // type-checked" and says an inconsistent one "results in a run-time error";
    // the untrapped alternative is reinterpreting one member's bits as another's,
    // exactly what LRM 7.3.2 says a tagged union exists to prevent. Same layout as
    // the null-handle fatal.
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag_s = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    // The tag names itself only when it is defined and in range; an x/z tag (an
    // uninitialized tagged union, LRM 11.9) or a value past the member table
    // names no member at all.
    std::string held;
    if (tag_unknown != 0) {
        held = "undefined (x)";
    } else if (tag < site->count) {
        held = fmt::format("`{}`", std::string_view(site->names[tag], site->name_lens[tag]));
    } else {
        held = fmt::format("no member (tag {})", tag);
    }

    std::string when;
    append_finish_time(when, now);
    const char* const verb = site->is_write != 0 ? "assignment to" : "read of";
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset that lands past the line break leaves
    // the style live across it.
    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " {} tagged-union member `{}`, which the current tag does not select",
               verb, site->member);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", site->where);
    fmt::print(stderr, "\n");
    if (site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag_s, "  note:");
    fmt::print(stderr, body, " the tag selects {} at {}", held, when);
    fmt::print(stderr, "\n");
    fmt::print(stderr, tag_s, "  note:");
    fmt::print(stderr, body, " a tagged union's tag and value change only together, through a whole-value "
                             "`tagged` expression -- assign `tagged {} (...)` before reaching this member "
                             "(LRM 11.9)",
               site->member);
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_null_handle: there is no defined state to hand back to
    // the host mid-instant, and std::exit keeps the nonzero status that
    // distinguishes this from a clean run.
    std::exit(1);
}

// ---- Checked heap allocation ----

namespace {

// The wheel rt_sim_run is currently driving, so a cold diagnostic can report a
// simulation time without every allocating entry point carrying one across the
// ABI. Written once per run on the simulation thread and read only from a
// failure path.
const rt_wheel* g_diag_wheel = nullptr;

// The noun a headline names the failing object by. Callers say "this <noun>"
// rather than "a <noun>": one phrasing serves "queue" and "associative array"
// alike, and it pairs with the caret already pointing at the statement.
const char* site_noun(const rt_alloc_site* site) {
    return site != nullptr && site->what != nullptr ? site->what : "value";
}

// Shared body of every heap-failure fatal: the compiler-diagnostic block
// (headline, location, source line under a caret) plus the notes, then exit.
// The caller composes the whole headline, so each failure leads with what is
// wrong with the size rather than with the machinery that noticed.
[[noreturn]] void alloc_fatal(const rt_alloc_site* site, uint64_t now, const std::string& headline) {
    io_ensure_init();
    std::fflush(stdout);

    const fmt::text_style label = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));
    const fmt::text_style head  = styled(fmt::emphasis::bold);
    const fmt::text_style loc   = styled(fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style tag   = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::cyan));
    const fmt::text_style body  = styled(fmt::emphasis::faint);
    const fmt::text_style caret = styled(fmt::emphasis::bold | fmt::fg(fmt::terminal_color::red));

    const char* where = site != nullptr && site->where != nullptr ? site->where : "<unknown>";

    std::string when;
    append_finish_time(when, now);
    // Each styled run stops short of the newline: fmt closes a style after
    // everything it formatted, and a reset past the line break leaks the style.
    fmt::print(stderr, label, "fatal:");
    fmt::print(stderr, head, " {}", headline);
    fmt::print(stderr, "\n");
    fmt::print(stderr, loc, "{}", where);
    fmt::print(stderr, "\n");
    if (site != nullptr && site->line != nullptr && site->line[0] != '\0') {
        const std::string text = snippet_text(site->line);
        print_snippet(text, site->line_no,
                      { SnippetMark{ site->column, mark_width(site->column, site->end_column),
                                     '^' } },
                      loc, caret);
    }
    fmt::print(stderr, tag, "  note:");
    fmt::print(stderr, body, " at {}", when);
    fmt::print(stderr, "\n");
    std::fflush(stderr);

    rt_vcd_close();
    rt_finishing = 1;
    // Same reasoning as rt_oscillation_abort: no defined state to hand back, and
    // a nonzero status distinguishes this from a clean run.
    std::exit(1);
}

// Render a byte count the way a person reads one: exact, plus a rounded binary
// magnitude once it stops being legible on its own.
std::string bytes_text(uint64_t n) {
    static const char* const unit[] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    if (n < 1024) {
        return fmt::format("{} bytes", n);
    }
    double      v = static_cast<double>(n);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < sizeof(unit) / sizeof(unit[0])) {
        v /= 1024.0;
        ++u;
    }
    return fmt::format("{} bytes ({:.1f} {})", n, v, unit[u]);
}

} // namespace

extern "C" uint64_t rt_diag_now(void) {
    return g_diag_wheel != nullptr ? rt_wheel_now(g_diag_wheel) : 0;
}

namespace {

// The body behind both public entry points. `zeroed` picks calloc, whose large
// requests come back as untouched zero pages; the diagnostics are identical
// because the failure modes are.
void* alloc_bytes(uint64_t size, const rt_alloc_site* site, uint64_t now, bool zeroed) {
    if (size == 0) {
        // Every caller treats a null buffer with a zero length as the empty
        // container, and malloc(0) may hand back a pointer that must still be
        // freed. Normalizing here keeps the "null means empty" invariant.
        return nullptr;
    }
    // size_t is the only ceiling: a 32-bit host cannot address a >4 GiB buffer
    // however much the design asks for, and saying so beats a truncating cast.
    if (size > static_cast<uint64_t>(SIZE_MAX)) {
        alloc_fatal(site, now,
                    fmt::format("size for this {} exceeds what this machine can address: {}",
                                site_noun(site), bytes_text(size)));
    }
    const size_t bytes = static_cast<size_t>(size);
    void*        p     = zeroed ? std::calloc(bytes, 1) : std::malloc(bytes);
    if (p == nullptr) {
        alloc_fatal(site, now,
                    fmt::format("out of memory for this {}: the system refused {}",
                                site_noun(site), bytes_text(size)));
    }
    return p;
}

} // namespace

extern "C" void* rt_alloc_checked(uint64_t size, const rt_alloc_site* site, uint64_t now) {
    return alloc_bytes(size, site, now, /*zeroed=*/false);
}

extern "C" uint64_t rt_alloc_elems_size(uint64_t count, uint64_t stride, const rt_alloc_site* site,
                                        uint64_t now) {
    uint64_t size = 0;
    if (__builtin_mul_overflow(count, stride, &size)) {
        alloc_fatal(site, now,
                    fmt::format("size for this {} overflows: {} elements of {} bytes exceeds the "
                                "largest object this machine can address",
                                site_noun(site), count, stride));
    }
    return size;
}

extern "C" void* rt_alloc_elems_checked(uint64_t count, uint64_t stride, const rt_alloc_site* site,
                                        uint64_t now) {
    return rt_alloc_checked(rt_alloc_elems_size(count, stride, site, now), site, now);
}

extern "C" void* rt_zalloc_elems_checked(uint64_t count, uint64_t stride,
                                         const rt_alloc_site* site, uint64_t now) {
    return alloc_bytes(rt_alloc_elems_size(count, stride, site, now), site, now,
                       /*zeroed=*/true);
}

extern "C" void rt_alloc_negative(int64_t n, const rt_alloc_site* site, uint64_t now) {
    alloc_fatal(site, now,
                fmt::format("invalid negative size for this {}: {}", site_noun(site), n));
}

// ---- Deferred immediate assertion reports (LRM 16.4) ----
//
// A deferred assertion's action (a single severity system task, or the default
// $error) is rendered here when the assertion executes in the Active region —
// its arguments are sampled at that instant (LRM 16.4.1) — but its emission is
// delayed: an observed (`#0`) report matures + fires in the Observed region, a
// final report in the Postponed region. Each queued report is tagged with a
// per-process generation counter address and the counter's value at queue time.
// The owning process bumps its generation at every activation (a flush point,
// LRM 16.4.2), so a report left over from a superseded activation has a stale
// generation and is dropped at maturation instead of firing — this is the
// glitch suppression the construct exists for. All same-mode reports leave the
// queue at their maturation region regardless (fired if current, dropped if
// stale), so the queue never carries a report across time steps.
namespace {
struct DeferReport {
    std::string     line;     // fully rendered "<LABEL>: <banner><message>\n"
    const uint64_t* gen;      // owning process's generation counter
    uint64_t        gen_val;  // generation at queue time
    bool            is_final; // final (Postponed) vs observed (#0, Observed)
};
std::vector<DeferReport> g_defer;
} // namespace

extern "C" void rt_defer_report(int32_t level, uint64_t is_final, const void* gen, uint64_t gen_val,
                                const char* banner, const char* fmt, uint32_t argc,
                                const rt_arg* argv) {
    static const char* const labels[] = { "INFO", "WARNING", "ERROR", "FATAL" };
    io_ensure_init();
    std::string line = labels[level >= 0 && level < 4 ? level : 2];
    line += ": ";
    line += banner;
    render(line, fmt, argc, argv);
    line += '\n';
    g_defer.push_back(
      { std::move(line), static_cast<const uint64_t*>(gen), gen_val, is_final != 0 });
}

// Mature the queued reports of one mode: fire those whose process generation is
// still current (not superseded by a later activation) and drop the rest; every
// same-mode report leaves the queue. `final_pass` selects final vs observed.
static void defer_mature(bool final_pass) {
    if (g_defer.empty()) {
        return;
    }
    std::fflush(stdout);
    std::vector<DeferReport> keep;
    bool                     wrote = false;
    for (DeferReport& r : g_defer) {
        if (r.is_final != final_pass) {
            keep.push_back(std::move(r));
            continue;
        }
        if (r.gen == nullptr || *r.gen == r.gen_val) {
            std::fputs(r.line.c_str(), stderr);
            wrote = true;
        }
    }
    g_defer.swap(keep);
    if (wrote) {
        std::fflush(stderr);
    }
}

extern "C" void rt_defer_flush_observed() {
    defer_mature(/*final_pass=*/false);
}

extern "C" void rt_defer_flush_final() {
    defer_mature(/*final_pass=*/true);
}

// Return the process-global simulation state to its power-on condition. Called
// at the end of every run, so the next simulation in this process starts clean.
//
// This matters because several of these outlive the JIT session that created
// their contents: g_monitor.ramp is a function pointer INTO the JIT'd code, and
// g_strobe_queue holds coroutine handles. run_jit destroys the LLJIT when it
// returns, so anything left here dangles, and the next simulation's Postponed
// drain would call or resume it. Only a host that runs more than one simulation
// per process can observe that -- the CLI runs exactly one -- but clotho_test
// runs it a thousand times over.
namespace {
void sim_teardown() {
    g_monitor = MonitorState{};
    g_strobe_queue.clear();
    g_strobe_read = 0;
    g_observed_case.clear();
    g_defer.clear();
    rt_nba_arr_reset();
    rt_intra_reset();
}
} // namespace

extern "C" void rt_sim_run(void* state, void* wheel, void (*drain)(void*, void*),
                           void (*finals)(void*, void*)) {
    // Run the simulation, then the `final` blocks (LRM 9.2.3), so the finals see
    // the settled design state on BOTH exit paths. `drain` now returns normally
    // whether it ran out of work or a $finish stopped it, so there is one path.
    rt_finishing = 0; // a prior run in this process may have set it
    // Publish the wheel so a cold diagnostic (an allocation failure) can report
    // the simulation time without every allocating call carrying one.
    g_diag_wheel = static_cast<const rt_wheel*>(wheel);
    drain(state, wheel);
    finals(state, wheel);
    g_diag_wheel = nullptr;
    sim_teardown();
}

extern "C" void rt_fflush(int32_t desc) {
    if (desc == 0) {
        // No-argument $fflush: passing nullptr flushes all C stdio buffers.
        std::fflush(nullptr);
        return;
    }
    io_ensure_init();
    if ((desc & kFdBit) != 0) {
        if (std::FILE* f = fd_stream(desc)) {
            std::fflush(f);
        }
        return;
    }
    for (int b = 0; b < 31; ++b) {
        if ((desc & (int32_t{ 1 } << b)) != 0 && mcd_channels[b] != nullptr) {
            std::fflush(mcd_channels[b]);
        }
    }
}

namespace {

// Hex / binary digit value, or -1 if `c` is not a digit of that base.
int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
int bin_digit(char c) {
    return (c == '0' || c == '1') ? c - '0' : -1;
}

} // namespace

namespace {

// Destination geometry for a bulk memory load, shared by $readmem and $fread:
// element i's planes live at mm_base / z_base + i*stride, each `width` bits (<=64,
// enforced by the backend). A 2-state memory (four_state=false) has no z plane and
// stores x/z as 0. A scalar target is just count == 1.
struct MemDest {
    uint8_t* mm_base;
    uint8_t* z_base;
    uint32_t stride;
    uint32_t count;
    uint32_t width;
    bool     four_state;
    // A narrow 2-state memory stores the bare iW value per element (no packed
    // must|may word, no z).
    bool narrow = false;
};

// Write one element's value planes at storage index `idx` (bounds-checked). A null
// z_base (a never-z / z-elided memory) yields a null z pointer — not an offset from
// null — so write_word_planes skips the z write.
void mem_write(const MemDest& d, int64_t idx, uint64_t must, uint64_t may, uint64_t z) {
    if (idx < 0 || idx >= static_cast<int64_t>(d.count)) {
        return;
    }
    if (!d.four_state) {
        may = must;
        z   = 0;
    }
    const size_t off = static_cast<size_t>(idx) * d.stride;
    uint8_t*     zp  = d.z_base != nullptr ? d.z_base + off : nullptr;
    write_word_planes(d.mm_base + off, zp, d.width, must, may, z, d.narrow);
}

// Wide (> 64-bit element) sibling of mem_write. The planes are little-endian word
// arrays (plane_words(d.width) words); write_word_planes_wide masks them in place.
void mem_write_wide(const MemDest& d, int64_t idx, uint64_t* must, uint64_t* may, uint64_t* z) {
    if (idx < 0 || idx >= static_cast<int64_t>(d.count)) {
        return;
    }
    const uint32_t words = plane_words(d.width);
    if (!d.four_state) {
        std::memcpy(may, must, words * sizeof(uint64_t));
        std::memset(z, 0, words * sizeof(uint64_t));
    }
    const size_t off = static_cast<size_t>(idx) * d.stride;
    uint8_t*     zp  = d.z_base != nullptr ? d.z_base + off : nullptr;
    write_word_planes_wide(d.mm_base + off, zp, d.width, must, may, z, d.narrow);
}

// Map a declared-range address to a 0-based storage index: an ascending range
// counts up from decl_left, a descending range counts down from it.
int64_t norm_addr(int64_t decl_left, int64_t decl_right, int64_t addr) {
    return decl_left <= decl_right ? addr - decl_left : decl_left - addr;
}

} // namespace

// $readmemh / $readmemb (LRM §21.4). Loads a hex/binary text file into an unpacked
// array. The destination planes live in the JIT state buffer: element i's must|may
// word is at mm_base + i*stride, its z word at z_base + i*stride (the backend passes
// the two region bases, already offset to element 0). Each parsed token is assembled
// into u64 must / may / z accumulators (element width <= 64, enforced by the backend)
// and written through mem_write — no per-element allocation.
extern "C" void rt_readmem(const char* path_buf, uint32_t path_len, uint8_t* mm_base, uint8_t* z_base,
                           uint32_t stride, uint32_t count, uint32_t width, int64_t decl_left,
                           int64_t decl_right, int64_t start, int64_t finish, uint32_t flags) {
    const bool hex          = (flags & 1u) != 0;
    const bool has_start    = (flags & 2u) != 0;
    const bool has_finish   = (flags & 4u) != 0;
    const bool four_state   = (flags & 8u) != 0;
    const bool path_forward = (flags & 16u) != 0;
    // A narrow 2-state memory stores a single iW element (no packed must|may word).
    const bool narrow       = (flags & 32u) != 0;

    // A string-object filename (path_forward) arrives as its bytes in natural
    // order; otherwise it is a right-justified packed-ASCII vector (byte 0 = last
    // character, high bytes zero padding), the same layout $fopen receives.
    const std::string path = path_forward ? std::string(path_buf, path_len) : unpack_ascii(path_buf, path_len);

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "$readmem: cannot open file '%s'\n", path.c_str());
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::rewind(f);
    std::vector<char> buf(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (!buf.empty()) {
        buf.resize(std::fread(buf.data(), 1, buf.size(), f));
    }
    std::fclose(f);

    // Iteration runs in declared-address space, not storage-index space (LRM
    // §21.4). The default start is the *lowest* address and loading proceeds upward
    // toward the highest, regardless of the range direction; a start-only form also
    // goes upward; a start>finish pair decrements. The direction persists across any
    // @addr directive in the file. norm() maps each address to its storage slot.
    const int64_t lo_addr = decl_left < decl_right ? decl_left : decl_right;
    const int64_t hi_addr = decl_left < decl_right ? decl_right : decl_left;
    int64_t       addr    = has_start ? start : lo_addr;
    const int64_t dir     = (has_finish && finish < start) ? -1 : 1;
    // Loading stops at the finish address (if given) else the highest address; data
    // beyond that bound is ignored with a warning (LRM §21.4).
    const int64_t end = has_finish ? finish : hi_addr;

    const int      bits   = hex ? 4 : 1;
    const uint64_t nibble = hex ? 0xFu : 0x1u;

    // Value planes accumulate into word arrays; a <= 64-bit element uses one word
    // and reduces to the scalar (v << bits) | d path, a wider element assembles
    // multi-word. Allocated once and reused per token.
    const uint32_t        words = plane_words(width);
    const bool            wide  = width > 64;
    std::vector<uint64_t> must(words);
    std::vector<uint64_t> may(words);
    std::vector<uint64_t> z(words);

    const MemDest dest{ mm_base, z_base, stride, count, width, four_state, narrow };
    const auto    write_elem = [&](int64_t idx) {
        if (idx < 0 || idx >= static_cast<int64_t>(count)) {
            std::fprintf(stderr, "$readmem: address out of range for '%s'\n", path.c_str());
            return;
        }
        if (wide) {
            mem_write_wide(dest, idx, must.data(), may.data(), z.data());
        } else {
            mem_write(dest, idx, must[0], may[0], z[0]);
        }
    };

    const size_t n = buf.size();
    size_t       i = 0;
    while (i < n) {
        const char c = buf[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && buf[i + 1] == '/') {
            i += 2;
            while (i < n && buf[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && buf[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(buf[i] == '*' && buf[i + 1] == '/')) {
                ++i;
            }
            i += 2;
            continue;
        }
        if (c == '@') {
            // Address directive @hexaddr (always hex, regardless of readmemh/b).
            ++i;
            uint64_t a = 0;
            for (int d; i < n && (d = hex_digit(buf[i])) >= 0; ++i) {
                a = a * 16 + static_cast<uint64_t>(d);
            }
            addr = static_cast<int64_t>(a);
            continue;
        }
        // A data token: accumulate digits MSB-first; x/z (and ?) mark unknown bits.
        // wide_shl a[0] |= d reduces to (a << bits) | d for a single word.
        std::fill(must.begin(), must.end(), 0);
        std::fill(may.begin(), may.end(), 0);
        std::fill(z.begin(), z.end(), 0);
        bool any = false;
        while (i < n) {
            const char d = buf[i];
            uint64_t   dv;
            uint64_t   mv;
            uint64_t   zv;
            if (d == '_') {
                ++i;
                continue;
            }
            if (d == 'x' || d == 'X') {
                dv = 0;
                mv = nibble;
                zv = 0;
            } else if (d == 'z' || d == 'Z' || d == '?') {
                dv = 0;
                mv = nibble;
                zv = nibble;
            } else {
                const int hv = hex ? hex_digit(d) : bin_digit(d);
                if (hv < 0) {
                    break;
                }
                dv = static_cast<uint64_t>(hv);
                mv = dv;
                zv = 0;
            }
            wide_shl(must.data(), words, bits);
            must[0] |= dv;
            wide_shl(may.data(), words, bits);
            may[0] |= mv;
            wide_shl(z.data(), words, bits);
            z[0] |= zv;
            any = true;
            ++i;
        }
        if (!any) {
            ++i; // an unrecognized character; skip it and continue
            continue;
        }
        if ((dir > 0 && addr > end) || (dir < 0 && addr < end)) {
            // Past the finish / highest address: the remaining file data is dropped.
            std::fprintf(stderr, "$readmem: more data than the memory range for '%s'\n", path.c_str());
            break;
        }
        write_elem(norm_addr(decl_left, decl_right, addr));
        addr += dir;
    }
}

extern "C" int32_t rt_fread(int32_t fd, uint8_t* mm_base, uint8_t* z_base, uint32_t stride, uint32_t count,
                            uint32_t width, uint32_t four_state, int64_t decl_left, int64_t decl_right,
                            int64_t start, int64_t max_count, uint32_t flags) {
    // $fread (LRM §21.3.4): read raw bytes from `fd` into a memory (or a scalar,
    // count == 1), MSB-first per element. Fills from the `start` declared address
    // (default the lowest) upward, at most `max_count` elements. Shares the memory
    // destination substrate with $readmem; only the byte source differs. Returns the
    // number of bytes read.
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    if (f == nullptr) {
        return 0;
    }
    const bool     has_start  = (flags & 1u) != 0;
    const bool     has_count  = (flags & 2u) != 0;
    const bool     narrow     = (flags & 4u) != 0;
    const MemDest  dest{ mm_base, z_base, stride, count, width, four_state != 0, narrow };
    const uint32_t elem_bytes = (width + 7) / 8;

    const int64_t lo        = decl_left < decl_right ? decl_left : decl_right;
    const int64_t hi        = decl_left < decl_right ? decl_right : decl_left;
    int64_t       addr      = has_start ? start : lo;
    int64_t       remaining = has_count ? max_count : static_cast<int64_t>(count);

    // Bytes accumulate into a word array; a <= 64-bit element uses one word and
    // reduces to the scalar (val << 8) | byte path.
    const uint32_t        words = plane_words(width);
    const bool            wide  = width > 64;
    std::vector<uint64_t> val(words);
    std::vector<uint64_t> may(words);
    std::vector<uint64_t> z(words, 0);

    int32_t bytes_read = 0;
    while (addr <= hi && remaining > 0) {
        std::fill(val.begin(), val.end(), 0);
        uint32_t nb = 0;
        while (nb < elem_bytes) {
            const int c = std::fgetc(f);
            if (c == EOF) {
                break;
            }
            wide_shl(val.data(), words, 8);
            val[0] |= static_cast<uint8_t>(c);
            ++nb;
            ++bytes_read;
        }
        if (nb == 0) {
            break; // EOF before this element
        }
        // MSB-first: a short final element left-shifts so the bytes read occupy the
        // high end and the missing low bytes stay 0.
        wide_shl(val.data(), words, 8u * (elem_bytes - nb));
        const int64_t idx = norm_addr(decl_left, decl_right, addr);
        if (wide) {
            // File bytes are all known (must == may, no z); mem_write_wide's
            // 2-state override handles a narrow memory.
            std::copy(val.begin(), val.end(), may.begin());
            std::fill(z.begin(), z.end(), 0);
            mem_write_wide(dest, idx, val.data(), may.data(), z.data());
        } else {
            mem_write(dest, idx, val[0], val[0], 0);
        }
        ++addr;
        --remaining;
        if (nb < elem_bytes) {
            break; // hit EOF mid-element
        }
    }
    return bytes_read;
}

// The `$random` generator, implementing the algorithm IEEE Std 1800-2023 Annex N
// defines. `$random` is rtl_dist_uniform(seed, INT32_MIN, INT32_MAX): a 32-bit
// LCG (seed = 69069*seed + 1) whose high bits build an IEEE single in [1, 2),
// scaled to a full-range signed 32-bit result. Only the low 32 bits of the LCG
// state feed the float trick and they evolve independently of any higher bits,
// so a plain uint32_t state reproduces the Annex's stream exactly.
//
// The float/double steps follow the Annex's arithmetic step for step: each one
// rounds, and algebraically folding them diverges from the defined stream, so
// they must not be reassociated (no fast-math).
namespace {

uint32_t g_random_seed = 0;

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

// -- Distribution routines (LRM §20.15.2) ----------------------------------------
//
// Implement the algorithms IEEE Std 1800-2023 Annex N defines, so the streams
// are identical to any conforming tool for a given seed. The float/double steps
// round, so (as with rng_uniform above) they follow the Annex's arithmetic step
// for step and are never reassociated — no fast-math. `dist_uniform_real` is the
// general two-argument `uniform(seed, start, end)`; rng_uniform above is its
// full-int32-span specialisation kept separate so the verified unseeded
// $random stream is untouched.
namespace {

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

namespace {

// The command-line plusarg list (LRM §21.6), threaded from the host before
// the design runs. Process-global, like g_random_seed above — matches the
// single-simulator-per-process model.
std::vector<std::string> g_plusargs;

// LRM §21.6: "If the prefix of one of the supplied plusargs matches all
// characters in the provided string" — `query` must be a prefix of the
// plusarg, and plusargs are searched in the order given.
const std::string* find_plusarg(std::string_view query) {
    for (const std::string& pa : g_plusargs) {
        if (pa.size() >= query.size() && std::string_view(pa).substr(0, query.size()) == query) {
            return &pa;
        }
    }
    return nullptr;
}

} // namespace

extern "C" void rt_plusargs_init(const char* const* args, const uint32_t* lens, uint32_t n) {
    g_plusargs.clear();
    g_plusargs.reserve(n);
    for (uint32_t k = 0; k < n; ++k) {
        g_plusargs.emplace_back(args[k], lens[k]);
    }
}

extern "C" int32_t rt_test_plusargs(const char* s, uint32_t len) {
    return find_plusarg(std::string_view(s, len)) != nullptr ? 1 : 0;
}

namespace {

// Convert a $value$plusargs remainder string into must/may bits per `conv`
// (z is always 0 — LRM §21.6 writes an all-unknown result as 'bx, never a z
// bit). Unlike $sscanf, which stops at the first bad character, every
// character in `rem` must be legal or the whole result is 'bx.
// Parse the plusarg remainder into the `words`-word planes (must == may; plusarg
// values carry no x/z except the whole-'bx illegal result). A single word reduces
// to the scalar `val * base + digit` accumulation.
void value_plusargs_numeric(std::string_view rem, char conv, uint64_t* must, uint64_t* may, uint32_t words) {
    std::fill(must, must + words, 0);
    std::fill(may, may + words, 0);
    if (rem.empty()) {
        return; // LRM: no remaining string -> store zero.
    }
    const int base = conv == 'd' ? 10 : conv == 'o' ? 8 : conv == 'b' ? 2 : 16;
    size_t    i    = 0;
    bool      neg  = false;
    if (base == 10 && (rem[0] == '-' || rem[0] == '+')) {
        neg = rem[0] == '-';
        i   = 1;
    }
    // Illegal input -> whole result 'bx (VPI: must=0, may=all-1, z=0). Masked to
    // the target width at write time.
    const auto set_x = [&] {
        std::fill(must, must + words, 0);
        std::fill(may, may + words, ~uint64_t{ 0 });
    };
    if (i >= rem.size()) {
        set_x(); // bare sign, no digits
        return;
    }
    for (; i < rem.size(); ++i) {
        int        hv;
        const char c = rem[i];
        if (base == 10) {
            hv = (c >= '0' && c <= '9') ? c - '0' : -1;
        } else if (base == 8) {
            hv = (c >= '0' && c <= '7') ? c - '0' : -1;
        } else if (base == 2) {
            hv = (c == '0' || c == '1') ? c - '0' : -1;
        } else {
            hv = hex_digit(c);
        }
        if (hv < 0) {
            set_x();
            return;
        }
        wide_mul_add(must, words, static_cast<uint64_t>(base), static_cast<uint64_t>(hv));
    }
    if (neg) {
        wide_negate(must, words);
    }
    std::copy(must, must + words, may);
}

} // namespace

extern "C" int32_t
rt_value_plusargs(const char* prefix, uint32_t prefix_len, uint32_t conv, const rt_scan_arg* arg) {
    const std::string* pa = find_plusarg(std::string_view(prefix, prefix_len));
    if (pa == nullptr) {
        return 0;
    }
    const std::string_view rem(pa->data() + prefix_len, pa->size() - prefix_len);
    const char              c = static_cast<char>(conv);

    const uint32_t        words = plane_words(arg->width);
    std::vector<uint64_t> must(words, 0);
    std::vector<uint64_t> may(words, 0);
    std::vector<uint64_t> z(words, 0);
    if (c == 's') {
        // Right-justify the remainder bytes (byte 0 = last char) into `must`, like
        // $sscanf's %s, capped at the target width; write_word_planes masks it.
        const size_t cap  = static_cast<size_t>(words) * 8;
        const size_t keep = rem.size() < cap ? rem.size() : cap;
        for (size_t k = 0; k < keep; ++k) {
            must[k / 8] |= static_cast<uint64_t>(static_cast<uint8_t>(rem[rem.size() - 1 - k])) << (8 * (k % 8));
        }
        std::copy(must.begin(), must.end(), may.begin());
    } else {
        value_plusargs_numeric(rem, c, must.data(), may.data(), words);
    }

    const bool narrow = (arg->flags & 1u) != 0;
    if (arg->width > 64) {
        write_word_planes_wide(arg->mm, arg->z, arg->width, must.data(), may.data(), z.data(), narrow);
    } else {
        write_word_planes(arg->mm, arg->z, arg->width, must[0], may[0], 0, narrow);
    }
    return 1;
}

extern "C" void rt_string_init(rt_string* s) {
    s->storage[0]  = 0;              // NUL terminator for the empty string
    s->storage[31] = str_inline_cap; // Inline form, length 0.
}

extern "C" void rt_string_deinit(rt_string* s) {
    if (str_is_heap(s)) {
        std::free(str_heap(s).ptr);
    }
    s->storage[0]  = 0;
    s->storage[31] = str_inline_cap;
}

extern "C" void rt_string_assign(rt_string* dst, const rt_string* src, const rt_alloc_site* site) {
    if (dst == src) {
        return;
    }
    str_assign(dst, str_data(src), str_len(src), site);
}

extern "C" void rt_string_move(rt_string* dst, rt_string* src) {
    if (dst == src) {
        return;
    }
    // Ownership transfer, not a copy: release dst's own buffer, then take src's
    // handle image whole. The 32 bytes encode either form -- inline text or a
    // heap {ptr,len,cap} -- so one memcpy moves the value and, with it, ownership
    // of any buffer. src is re-initialized rather than left aliasing that buffer,
    // so its own later deinit stays correct and the moved value is freed once.
    // No epoch: a string handle has no interior pointer for a ref to hold.
    rt_string_deinit(dst);
    std::memcpy(dst->storage, src->storage, sizeof(src->storage));
    rt_string_init(src);
}

extern "C" void rt_string_assign_bytes(rt_string* dst, const char* p, uint64_t n, const rt_alloc_site* site) {
    str_assign(dst, p, n, site);
}

extern "C" uint64_t rt_string_len(const rt_string* s) {
    return str_len(s);
}

extern "C" const char* rt_string_data(const rt_string* s) {
    return str_data(s);
}

extern "C" void rt_string_concat(rt_string* dst, const rt_string* const* parts, uint32_t n, const rt_alloc_site* site) {
    // Aliasing exists only as identical handle pointers: distinct handles never
    // share a buffer (every assignment deep-copies). Scan for dst among the
    // parts; anything but a prefix occurrence falls back to a scratch build.
    uint64_t total  = 0;
    uint32_t dst_at = n;
    for (uint32_t k = 0; k < n; ++k) {
        total += str_len(parts[k]);
        if (dst_at == n && parts[k] == dst) {
            dst_at = k;
        }
    }
    if (dst_at == n || dst_at == 0) {
        // In-place: dst's current bytes are either unused (dst is not a part)
        // or already the result's prefix (dst is part 0, the s = {s, ...}
        // append). Writes land at offsets >= that prefix, so later parts equal
        // to dst still read their original bytes ({s, s} self-append); dst's
        // stored length is only updated after the copies, so their str_len is
        // the pre-call value too.
        const uint64_t skip = dst_at == 0 ? str_len(dst) : 0;
        str_reserve(dst, total, site);
        char*    out = str_data(dst);
        uint64_t off = skip;
        for (uint32_t k = dst_at == 0 ? 1 : 0; k < n; ++k) {
            const uint64_t len = str_len(parts[k]);
            std::memcpy(out + off, str_data(parts[k]), len);
            off += len;
        }
        str_set_len(dst, total);
        return;
    }
    // dst appears at a later position (s = {c, s}): its bytes sit mid-result,
    // so prefix writes would clobber them. Build in scratch.
    StrScratch scratch(total, site);
    uint64_t   off = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint64_t len = str_len(parts[k]);
        std::memcpy(scratch.buf + off, str_data(parts[k]), len);
        off += len;
    }
    str_assign(dst, scratch.buf, total, site);
}

extern "C" void rt_string_repl(rt_string* dst, const rt_string* src, uint64_t count, const rt_alloc_site* site) {
    const uint64_t len = str_len(src);
    // `count` is a design-controlled replication factor ({N{s}}), so the byte
    // total is the one product here that a program can push past 2^64. A wrapping
    // one would size the scratch buffer from the wrapped value and then write
    // `count` copies into it.
    const uint64_t total = rt_alloc_elems_size(count, len, site, rt_diag_now());
    StrScratch     scratch(total, site);
    for (uint64_t k = 0; k < count; ++k) {
        std::memcpy(scratch.buf + k * len, str_data(src), len);
    }
    str_assign(dst, scratch.buf, total, site);
}

extern "C" int32_t rt_string_cmp(const rt_string* a, const rt_string* b) {
    const uint64_t la = str_len(a);
    const uint64_t lb = str_len(b);
    const uint64_t n  = la < lb ? la : lb;
    const int      c  = n != 0 ? std::memcmp(str_data(a), str_data(b), n) : 0;
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

extern "C" int32_t rt_string_icmp(const rt_string* a, const rt_string* b) {
    const uint64_t la = str_len(a);
    const uint64_t lb = str_len(b);
    const uint64_t n  = la < lb ? la : lb;
    const char*    pa = str_data(a);
    const char*    pb = str_data(b);
    for (uint64_t k = 0; k < n; ++k) {
        const char ca = str_ascii_lower(pa[k]);
        const char cb = str_ascii_lower(pb[k]);
        if (ca != cb) {
            return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
        }
    }
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

extern "C" void rt_string_substr(rt_string* dst, const rt_string* src, int32_t i, int32_t j,
                                 const rt_alloc_site* site) {
    const uint64_t len = str_len(src);
    if (i < 0 || j < i || static_cast<uint64_t>(j) >= len) {
        str_assign(dst, nullptr, 0, site);
        return;
    }
    str_assign(dst, str_data(src) + i, static_cast<uint64_t>(j) - static_cast<uint64_t>(i) + 1, site);
}

extern "C" void rt_enum_name(rt_string* dst, const uint8_t* val, uint32_t vbytes, uint32_t count,
                             const uint8_t* values, const char* const* names, const uint32_t* lens,
                             uint32_t width, uint32_t fmt_fallback, uint32_t is_signed,
                             const rt_alloc_site* site) {
    for (uint32_t k = 0; k < count; ++k) {
        if (std::memcmp(val, values + static_cast<size_t>(k) * vbytes, vbytes) == 0) {
            str_assign(dst, names[k], lens[k], site);
            return;
        }
    }
    if (fmt_fallback != 0) {
        // LRM 21.2.1.6: "Otherwise the value shall print according to the base type
        // of the enumeration" — under %p that is the ordinary integral leaf, i.e. the
        // base type printed as it would be unformatted: decimal (21.2.1.1), signed
        // when the base type is. Only the must plane reaches here, so an x/z-valued
        // enum prints its must bits.
        clotho::FormatSpec spec{};
        spec.ok        = true;
        spec.conv      = 'd';
        spec.has_width = true; // width 0 => minimal digits (the %0d form)

        // render_arg reads all three planes, so the unknown planes are materialized
        // as zeros rather than left null.
        const uint32_t        words = plane_words(width != 0 ? width : 1);
        std::vector<uint64_t> mv(words, 0);
        const std::vector<uint64_t> zero(words, 0);
        std::memcpy(mv.data(), val, std::min<size_t>(vbytes, words * sizeof(uint64_t)));
        std::string  s;
        const rt_arg a{ width, is_signed != 0 ? 1u : 0u, mv.data(), zero.data(), zero.data() };
        render_arg(s, a, spec);
        str_assign(dst, s.data(), static_cast<uint64_t>(s.size()), site);
        return;
    }
    str_assign(dst, nullptr, 0, site); // No member matched: empty string (LRM 6.19.2).
}

extern "C" void rt_string_toupper(rt_string* dst, const rt_string* src, const rt_alloc_site* site) {
    rt_string_assign(dst, src, site); // No-op when dst == src, then transform in place.
    char*          p = str_data(dst);
    const uint64_t n = str_len(dst);
    for (uint64_t k = 0; k < n; ++k) {
        p[k] = str_ascii_upper(p[k]);
    }
}

extern "C" void rt_string_tolower(rt_string* dst, const rt_string* src, const rt_alloc_site* site) {
    rt_string_assign(dst, src, site);
    char*          p = str_data(dst);
    const uint64_t n = str_len(dst);
    for (uint64_t k = 0; k < n; ++k) {
        p[k] = str_ascii_lower(p[k]);
    }
}

extern "C" void rt_string_putc(rt_string* s, int32_t i, uint32_t c) {
    if (i < 0 || static_cast<uint64_t>(i) >= str_len(s) || (c & 0xffu) == 0) {
        return;
    }
    str_data(s)[i] = static_cast<char>(c & 0xffu);
}

extern "C" uint32_t rt_string_getc(const rt_string* s, int32_t i) {
    if (i < 0 || static_cast<uint64_t>(i) >= str_len(s)) {
        return 0;
    }
    return static_cast<unsigned char>(str_data(s)[i]);
}

namespace {

// Shared digit-scan for the atoi family (LRM §6.16.9): consume leading digits
// (and '_' separators) of `base`, stopping at the first other character or
// end of string. No leading whitespace or sign handling — the clause
// explicitly excludes them ("does not parse the full syntax for integer
// literals"). Verified against iverilog. Underscore
// interleaving rules out std::from_chars here; overflow wraps silently via
// plain unsigned 32-bit accumulation, matching the clause's own truncation
// note.
int32_t str_atoi_base(const rt_string* s, uint32_t base) {
    const char*    p     = str_data(s);
    const uint64_t n     = str_len(s);
    uint32_t       acc   = 0;
    bool           found = false;
    for (uint64_t k = 0; k < n; ++k) {
        const char c = p[k];
        if (c == '_') {
            continue;
        }
        uint32_t v;
        if (c >= '0' && c <= '9') {
            v = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'z') {
            v = static_cast<uint32_t>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'Z') {
            v = static_cast<uint32_t>(c - 'A') + 10;
        } else {
            break;
        }
        if (v >= base) {
            break;
        }
        acc   = acc * base + v;
        found = true;
    }
    return found ? static_cast<int32_t>(acc) : 0;
}

// dst = sign + magnitude of `i` in `base`, lowercase digits (LRM §6.16.11-
// 6.16.14). std::to_chars natively formats a negative signed integer as '-'
// followed by the magnitude in any base, matching iverilog's convention for
// hextoa/octtoa/bintoa — not a two's-complement bit-pattern
// dump.
void str_toa_base(rt_string* dst, int32_t i, int base, const rt_alloc_site* site) {
    char       buf[36]; // sign + up to 32 binary digits
    const auto res = std::to_chars(buf, buf + sizeof(buf), i, base);
    str_assign(dst, buf, static_cast<uint64_t>(res.ptr - buf), site);
}

} // namespace

extern "C" int32_t rt_string_atoi(const rt_string* s) {
    return str_atoi_base(s, 10);
}
extern "C" int32_t rt_string_atohex(const rt_string* s) {
    return str_atoi_base(s, 16);
}
extern "C" int32_t rt_string_atooct(const rt_string* s) {
    return str_atoi_base(s, 8);
}
extern "C" int32_t rt_string_atobin(const rt_string* s) {
    return str_atoi_base(s, 2);
}

extern "C" double rt_string_atoreal(const rt_string* s) {
    const char* p   = str_data(s);
    const char* end = p + str_len(s);
    // Unlike the atoi family, atoreal accepts leading whitespace and a '+'
    // sign (verified against iverilog); std::from_chars accepts neither, so
    // both are skipped by hand before the real-literal parse.
    while (p < end && scan_ws(static_cast<unsigned char>(*p))) {
        ++p;
    }
    if (p < end && *p == '+') {
        ++p;
    }
    double v = 0.0; // Unmodified by from_chars on failure: "0.0 if no digits".
    std::from_chars(p, end, v);
    return v;
}

extern "C" void rt_string_itoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 10, site);
}
extern "C" void rt_string_hextoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 16, site);
}
extern "C" void rt_string_octtoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 8, site);
}
extern "C" void rt_string_bintoa(rt_string* dst, int32_t i, const rt_alloc_site* site) {
    str_toa_base(dst, i, 2, site);
}

extern "C" void rt_string_realtoa(rt_string* dst, double r, const rt_alloc_site* site) {
    char       buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), r);
    str_assign(dst, buf, static_cast<uint64_t>(res.ptr - buf), site);
}

extern "C" void rt_string_from_packed(rt_string* dst, const uint8_t* val, uint32_t width_bits,
                                      const rt_alloc_site* site) {
    const uint32_t nbytes = (width_bits + 7) / 8;
    StrScratch     scratch(nbytes, site);
    uint64_t       out_len = 0;
    for (uint32_t k = nbytes; k-- > 0;) {
        uint8_t c = val[k];
        if (k == nbytes - 1 && width_bits % 8 != 0) {
            c &= static_cast<uint8_t>((1u << (width_bits % 8)) - 1);
        }
        if (c != 0) {
            scratch.buf[out_len++] = static_cast<char>(c);
        }
    }
    str_assign(dst, scratch.buf, out_len, site);
}

extern "C" void rt_string_to_packed(const rt_string* s, uint8_t* out, uint32_t width_bits) {
    // Buffer sizing mirrors ir::plane_bytes (InternPool.h) — the backend allocas
    // exactly one val plane and loads an iW from it.
    uint32_t buf_bytes = 0;
    if (width_bits <= 8) {
        buf_bytes = 1;
    } else if (width_bits <= 16) {
        buf_bytes = 2;
    } else if (width_bits <= 32) {
        buf_bytes = 4;
    } else if (width_bits <= 64) {
        buf_bytes = 8;
    } else {
        buf_bytes = ((width_bits + 63) / 64) * 8;
    }
    std::memset(out, 0, buf_bytes);

    const uint32_t nbytes = (width_bits + 7) / 8;
    const uint64_t len    = str_len(s);
    const char*    p      = str_data(s);
    const uint32_t take   = len < nbytes ? static_cast<uint32_t>(len) : nbytes;
    for (uint32_t k = 0; k < take; ++k) {
        out[k] = static_cast<uint8_t>(p[len - 1 - k]);
    }
    if (width_bits % 8 != 0) {
        out[nbytes - 1] &= static_cast<uint8_t>((1u << (width_bits % 8)) - 1);
    }
}

// Coroutine frames may be over-aligned (16-byte plane spills) and malloc
// guarantees only 16, so allocate with an explicit alignment. Aligned operator
// new rather than C11 aligned_alloc: the latter is absent from the MSVC runtime
// (which offers only the non-interchangeable _aligned_malloc/_aligned_free
// pair). The nothrow form matters — this is called from JIT'd code across a C
// ABI with no unwind tables, so a thrown bad_alloc could not propagate.
namespace {
constexpr std::align_val_t frame_align{64};
}

extern "C" void* rt_alloc(uint64_t size) {
    return ::operator new(static_cast<size_t>(size), frame_align, std::nothrow);
}

extern "C" void rt_free(void* p) {
    ::operator delete(p, frame_align);
}

// -- Value-change dump (VCD, LRM §21.7; design in docs/VCD.md) --
//
// The sim thread is the sole producer: at each settled instant rt_vcd_flush reads
// the planes of the variables the dirty set marked, diffs them against a
// per-variable shadow, and appends backend-neutral records to a buffer. A writer
// thread formats whole buffers into VCD text and owns the file. Storage (var
// table, shadows, record buffers, the 1 MiB output buffer) is allocated once at
// rt_vcd_start; the steady state performs no heap allocation. docs/VCD.md is the
// design of record; what is still open there (multithreaded formatting, io_uring,
// crash-handler durability, FST / viewer sinks) all sits behind this same
// producer/record/sink split.

namespace {

// 64-bit words needed to hold `width` bits.
inline uint32_t vcd_nwords(uint32_t width) {
    return (width + 63) / 64;
}

// One dumped scalar/vector variable: its live plane pointers into design state,
// its VCD identifier, and the last value emitted (shadow) for change suppression.
// Planes are multi-word to support arbitrary width; a value is `nwords` 64-bit
// words per plane (little-endian, low word first).
struct VcdVar {
    const uint8_t* mm = nullptr;   // packed must|may plane (i(2W)), or the iW word when narrow
    const uint8_t* z  = nullptr;   // z plane (iW), or null for a never-z / narrow variable
    uint32_t       width       = 0;
    uint32_t       nwords      = 0;   // words per plane
    uint32_t       mm_bytes    = 0;   // significant bytes of the mm slot field (for the raw diff)
    uint32_t       z_bytes     = 0;   // significant bytes of the z field (0 for never-z / narrow)
    bool           four_state  = false;
    bool           narrow      = false;   // 2-state `{ iW }` slot: mm is a bare iW value word
    // real / shortreal: the value word is an IEEE-754 bit pattern, dumped as
    // `r<decimal>` rather than as bits (LRM §21.7.2.1 vector_value_change).
    bool           is_real     = false;
    // named event (LRM 15.5): no value, only occurrences. `mm` points at the
    // event's 64-bit last-trigger tick, so the ordinary shadow diff detects a
    // trigger, and the emitted change is the bare pulse `1<id>`.
    bool           is_event    = false;
    bool           seen        = false;   // shadow valid (a value has been dumped)
    // Shadow of the last dumped *raw* slot: mm_bytes of the mm field, then z_bytes
    // of the z field. The diff is a memcmp of these against the live slot; the
    // planes are assembled (vcd_read_planes) only when they differ.
    std::vector<uint8_t> shadow;
    // VCD identifier code, and the precomputed line suffix: idcode+"\n" for a
    // scalar, " "+idcode+"\n" for a vector (appended after the value in one copy).
    std::string idcode;
    std::string suffix;
};

// The producer -> sink record stream is a byte buffer of self-describing records
// (docs/VCD.md §4), so a value of any width travels inline and the sink stays
// stateless. A record is a tag byte followed by:
//   tag 1 (time):   u64 tick
//   tag 0 (change): u32 idx, u32 nwords, then nwords u64 each of must, may, z
// Keeping the planes in the record (not a shadow reference) is what lets the sink
// run on another thread or format an alternate backend (FST, viewer).

// The single global VCD engine (the simulator is single-threaded, one dump per
// run). Reset by rt_vcd_close so the next simulation in the same process starts
// clean.
struct VcdEngine {
    std::FILE*            file    = nullptr;
    char*                 iobuf   = nullptr;   // owned output buffer (rendered VCD bytes)
    size_t                out_len = 0;         // bytes pending in iobuf
    rt_wheel*             wheel   = nullptr;    // cached for the close-time flush
    std::string           path;
    std::vector<VcdVar>   vars;
    std::vector<uint8_t>  recbuf;    // producer -> sink record byte stream for pending instants
    std::vector<uint64_t> scratch;   // scan buffer: 3*max_nwords words (must | may | z)
    // Activity-driven dirty set: `epoch[i]` is set by rt_vcd_touch when var i is
    // written this instant, including per-element for an unpacked array. A dense
    // byte array parallel to `vars`, so the flush's per-instant index scan reads
    // one byte per variable and never the fat VcdVar struct.
    std::vector<uint8_t> epoch;
    // Bytes already handed to the FILE. With the pending iobuf they are the file's
    // size, which the writer tests against the $dumplimit cap (vcd_limit_reached).
    uint64_t written = 0;
    uint32_t max_nwords = 1;    // widest variable, in words (sizes scratch / buffers)
    uint32_t next_id    = 0;    // next VCD identifier index (assigned per registration)
    bool started        = false;   // header finished ($enddefinitions written)
    bool active         = false;   // dumping (cleared by $dumpoff)
    bool force_all      = false;   // next flush emits every var ($dumpall / $dumpon / initial)
    bool pending_off    = false;   // next flush emits an all-x checkpoint, then deactivates

    static constexpr size_t iobuf_size = 1u << 20;   // 1 MiB
};

VcdEngine g_vcd;

// All VCD text is rendered into g_vcd.iobuf and written in bulk. This avoids the
// per-character locked stdio (std::fputc takes the FILE lock on every call), which
// dominated the formatter's cost. The buffer is written by whichever thread is
// appending (the sim thread for the header before the writer spawns, the writer
// thread for the body afterwards) -- never concurrently.
void vcd_flush_out() {
    if (g_vcd.out_len != 0) {
        std::fwrite(g_vcd.iobuf, 1, g_vcd.out_len, g_vcd.file);
        g_vcd.written += g_vcd.out_len;
        g_vcd.out_len = 0;
    }
}

inline void vcd_put(char c) {
    if (g_vcd.out_len >= VcdEngine::iobuf_size) {
        vcd_flush_out();
    }
    g_vcd.iobuf[g_vcd.out_len++] = c;
}

inline void vcd_puts(std::string_view s) {
    if (g_vcd.out_len + s.size() > VcdEngine::iobuf_size) {
        vcd_flush_out();
        if (s.size() > VcdEngine::iobuf_size) {   // a single string larger than the buffer
            std::fwrite(s.data(), 1, s.size(), g_vcd.file);
            g_vcd.written += s.size();
            return;
        }
    }
    std::memcpy(g_vcd.iobuf + g_vcd.out_len, s.data(), s.size());
    g_vcd.out_len += s.size();
}

void vcd_put_dec(uint64_t v) {
    char t[20];
    int  n = 0;
    if (v == 0) {
        vcd_put('0');
        return;
    }
    while (v != 0) {
        t[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    while (n != 0) {
        vcd_put(t[--n]);
    }
}

// Dense VCD identifier from a variable index: base-94 over the printable range
// 33..126. Uniqueness is all VCD requires; the exact spelling is unspecified.
std::string vcd_idcode(uint32_t n) {
    std::string s;
    do {
        s.push_back(static_cast<char>(33 + n % 94));
        n = n / 94;
    } while (n-- != 0);
    return s;
}

// Decompose femtoseconds-per-tick into a VCD `<magnitude><unit>` timescale. The
// design precision is a power of ten, so the magnitude lands on 1 / 10 / 100.
void vcd_write_timescale(uint64_t tick_fs) {
    struct Unit {
        uint64_t    fs;
        const char* suffix;
    };
    static constexpr Unit units[] = { { 1000000000000000ull, "s" }, { 1000000000000ull, "ms" },
                                      { 1000000000ull, "us" },       { 1000000ull, "ns" },
                                      { 1000ull, "ps" },             { 1ull, "fs" } };
    if (tick_fs == 0) {
        tick_fs = 1;
    }
    for (const Unit& u : units) {
        if (tick_fs % u.fs == 0) {
            vcd_puts("$timescale\n\t");
            vcd_put_dec(tick_fs / u.fs);
            vcd_puts(u.suffix);
            vcd_puts("\n$end\n");
            return;
        }
    }
    vcd_puts("$timescale\n\t");
    vcd_put_dec(tick_fs);
    vcd_puts("fs\n$end\n");
}

// Mask keeping the significant bits of a plane's top word (all bits when the width
// is a multiple of 64).
inline uint64_t vcd_topmask(uint32_t width) {
    const uint32_t r = width % 64;
    return r == 0 ? ~0ull : ((1ull << r) - 1);
}

// dst[0..dstw) = (src[0..srcw) as a bignum) >> shift. Words are little-endian.
void vcd_shr(const uint64_t* src, uint32_t srcw, uint32_t shift, uint64_t* dst, uint32_t dstw) {
    const uint32_t ws = shift / 64;
    const uint32_t bs = shift % 64;
    for (uint32_t k = 0; k < dstw; ++k) {
        const uint64_t lo  = ws + k < srcw ? src[ws + k] : 0;
        uint64_t       val = lo >> bs;
        if (bs != 0) {
            const uint64_t hi = ws + k + 1 < srcw ? src[ws + k + 1] : 0;
            val |= hi << (64 - bs);
        }
        dst[k] = val;
    }
}

// Read a variable's live planes into `out`, laid out must | may | z, each
// `v.nwords` little-endian 64-bit words. The must|may plane is the packed i(2W)
// slot field (bits [0,W) must, [W,2W) may); a narrow slot is a bare iW value.
void vcd_read_planes(const VcdVar& v, uint64_t* out) {
    if (v.nwords == 1) {
        // Fast path for the common width <= 64: three word stores, no memset / no
        // bignum shift. The 2W-bit slot is <= 128 bits, read once via __int128.
        const uint64_t tm = vcd_topmask(v.width);
        if (v.narrow) {
            uint64_t val = 0;
            std::memcpy(&val, v.mm, (v.width + 7) / 8);
            val    = val & tm;
            out[0] = val;   // must
            out[1] = val;   // may == must
            out[2] = 0;     // z
            return;
        }
        unsigned __int128 mm = 0;
        std::memcpy(&mm, v.mm, (2 * v.width + 7) / 8);
        out[0] = static_cast<uint64_t>(mm) & tm;             // must (low W)
        out[1] = static_cast<uint64_t>(mm >> v.width) & tm;  // may  (bits [W,2W))
        uint64_t zz = 0;
        if (v.z != nullptr) {
            std::memcpy(&zz, v.z, (v.width + 7) / 8);
            zz &= tm;
        }
        out[2] = zz;
        return;
    }
    const uint32_t w    = v.width;
    const uint32_t nw   = v.nwords;
    uint64_t*      must = out;
    uint64_t*      may  = out + nw;
    uint64_t*      z    = out + 2 * nw;
    const uint64_t tm   = vcd_topmask(w);
    std::memset(out, 0, static_cast<size_t>(3) * nw * 8);
    if (v.narrow) {
        // A 2-state `{ iW }` slot: the single value word is fully known (may==must).
        std::memcpy(must, v.mm, (w + 7) / 8);
        must[nw - 1] &= tm;
        std::memcpy(may, must, static_cast<size_t>(nw) * 8);
        return;   // z stays 0
    }
    // Copy the 2W-bit must|may field into a zeroed scratch, then split it: must is
    // the low W bits, may is the field shifted right by W.
    const uint32_t rw  = vcd_nwords(2 * w);
    uint64_t*      raw = g_vcd.scratch.data() + 3 * g_vcd.max_nwords;   // scratch tail, reserved
    std::memset(raw, 0, static_cast<size_t>(rw) * 8);
    std::memcpy(raw, v.mm, (2 * w + 7) / 8);
    std::memcpy(must, raw, static_cast<size_t>(nw) * 8);
    must[nw - 1] &= tm;
    vcd_shr(raw, rw, w, may, nw);
    may[nw - 1] &= tm;
    if (v.z != nullptr) {
        std::memcpy(z, v.z, (w + 7) / 8);
        z[nw - 1] &= tm;
    }
}

// Emit a real's value change: `r<decimal> <id>` (LRM §21.7.2.1). The slot holds
// the IEEE-754 bit pattern in its value word -- 64 bits for `real`, 32 for
// `shortreal`, which widens to double so the file carries one representation.
// The decimal is the shortest round-tripping form (std::to_chars), so a reader's
// strtod recovers the exact bits; iverilog's %.16g can lose the last ulp.
// A real has no x/z, so the only way `may` can differ from `must` is the all-x
// $dumpoff checkpoint (rt_vcd_flush), which prints as NaN -- iverilog's spelling,
// and the one value no real can otherwise round-trip to.
void vcd_emit_real(const VcdVar& v, const uint64_t* must, const uint64_t* may) {
    char   buf[40];
    size_t len = 0;
    if (must[0] != may[0]) {
        std::memcpy(buf, "NaN", 3);
        len = 3;
    } else {
        const double d = v.width == 32
                           ? static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(must[0])))
                           : std::bit_cast<double>(must[0]);
        len = static_cast<size_t>(std::to_chars(buf, buf + sizeof(buf), d).ptr - buf);
    }
    vcd_put('r');
    vcd_puts(std::string_view(buf, len));
    vcd_puts(v.suffix);
}

// Emit one variable's value-change line for the given planes (each `v.nwords`
// little-endian words). Scalars print the value char then the id; vectors print
// `b<bits> <id>` with the LRM §21.7.2.1 asymmetric leading-bit compression
// (leftmost printed digit sets the reader's left-extension: 0->0, x->x, z->z, but
// 1->0, so a 1-MSB cannot be compressed). A real prints `r<decimal> <id>`, and a
// named event the bare pulse `1<id>`.
void vcd_emit(const VcdVar& v, const uint64_t* must, const uint64_t* may, const uint64_t* z) {
    const auto bitc = [&](uint32_t i) -> char {
        const uint64_t m = 1ull << (i & 63);
        const uint32_t w = i >> 6;
        if ((z[w] & m) != 0) {
            return 'z';
        }
        if ((may[w] & m) == 0) {
            return '0';
        }
        return (must[w] & m) != 0 ? '1' : 'x';
    };
    if (v.is_real) {
        vcd_emit_real(v, must, may);
        return;
    }
    if (v.is_event) {
        // An event occurrence has no value and no complement: VCD writes `1<id>`
        // at the instant it triggers and nothing at any other instant. Repeated
        // triggers within one instant collapse into this single pulse, which is
        // all the format can express (LRM §21.7.2.1 declares the `event` var
        // type; iverilog writes the same line).
        vcd_put('1');
        vcd_puts(v.suffix);
        return;
    }
    // One capacity check for the whole line: 'b' + up to width digits + suffix. Then
    // write through a local pointer, no per-character bounds check.
    const size_t need = static_cast<size_t>(v.width) + v.suffix.size() + 2;
    if (g_vcd.out_len + need > VcdEngine::iobuf_size) {
        vcd_flush_out();
    }
    char* wp = g_vcd.iobuf + g_vcd.out_len;
    if (v.width == 1) {
        *wp++ = bitc(0);
    } else {
        *wp++          = 'b';
        const char msb = bitc(v.width - 1);
        if (msb == '0') {
            int hi = -1;
            for (int i = static_cast<int>(v.width) - 1; i >= 0; --i) {
                if (bitc(static_cast<uint32_t>(i)) != '0') {
                    hi = i;
                    break;
                }
            }
            if (hi < 0) {
                *wp++ = '0';
            } else {
                for (int i = hi; i >= 0; --i) {
                    *wp++ = bitc(static_cast<uint32_t>(i));
                }
            }
        } else if (msb == '1') {
            for (int i = static_cast<int>(v.width) - 1; i >= 0; --i) {
                *wp++ = bitc(static_cast<uint32_t>(i));
            }
        } else {
            // x / z MSB: one marker char forces the reader's left-extension, then the
            // bits below the leading run (every bit above `hi` equals the marker).
            *wp++  = msb;
            int hi = -1;
            for (int i = static_cast<int>(v.width) - 2; i >= 0; --i) {
                if (bitc(static_cast<uint32_t>(i)) != msb) {
                    hi = i;
                    break;
                }
            }
            for (int i = hi; i >= 0; --i) {
                *wp++ = bitc(static_cast<uint32_t>(i));
            }
        }
    }
    // Precomputed suffix: scalar idcode+"\n", vector " "+idcode+"\n".
    std::memcpy(wp, v.suffix.data(), v.suffix.size());
    wp += v.suffix.size();
    g_vcd.out_len = static_cast<size_t>(wp - g_vcd.iobuf);
}

// -- Producer / sink seam --
//
// The producer (rt_vcd_flush, on the sim thread) diffs against the shadow and
// appends records; the sink (vcd_drain) turns records into VCD text. Splitting the
// two here is what lets a later step move vcd_drain onto a writer thread, or swap
// in an FST / viewer sink, without touching the producer.

// Grow recbuf by `n` and return a pointer to the fresh region. recbuf is reserved
// to a whole handoff batch at start, so resize never reallocates -- far cheaper
// than std::vector::insert's range machinery, which dominated the producer's cost
// in profiling.
inline uint8_t* vcd_rec_grow(size_t n) {
    const size_t o = g_vcd.recbuf.size();
    g_vcd.recbuf.resize(o + n);
    return g_vcd.recbuf.data() + o;
}

void vcd_rec_time(uint64_t tick) {
    uint8_t* d = vcd_rec_grow(1 + sizeof(tick));
    d[0]       = 1;
    std::memcpy(d + 1, &tick, sizeof(tick));
}

// Append a value-change record: idx, nwords, then the must | may | z planes
// (3*nwords words, contiguous at `planes`).
void vcd_rec_change(uint32_t idx, const uint64_t* planes, uint32_t nwords) {
    const size_t pbytes = static_cast<size_t>(3) * nwords * 8;
    uint8_t*     d      = vcd_rec_grow(9 + pbytes);
    d[0]                = 0;
    std::memcpy(d + 1, &idx, 4);
    std::memcpy(d + 5, &nwords, 4);
    std::memcpy(d + 9, planes, pbytes);
}

// Diff variable `i` against its shadow; if changed (or `force`), append a change
// record and update the shadow. Returns true when a record was appended.
bool vcd_scan_var(uint32_t i, bool force) {
    VcdVar&  v  = g_vcd.vars[i];
    uint8_t* sh = v.shadow.data();
    // Cheap diff on the raw slot bytes (no plane assembly). Assemble the planes
    // only when the value actually changed.
    if (!force && v.seen && std::memcmp(v.mm, sh, v.mm_bytes) == 0
        && (v.z_bytes == 0 || std::memcmp(v.z, sh + v.mm_bytes, v.z_bytes) == 0)) {
        return false;
    }
    uint64_t* scan = g_vcd.scratch.data();
    vcd_read_planes(v, scan);
    vcd_rec_change(i, scan, v.nwords);
    std::memcpy(sh, v.mm, v.mm_bytes);
    if (v.z_bytes != 0) {
        std::memcpy(sh + v.mm_bytes, v.z, v.z_bytes);
    }
    v.seen = true;
    return true;
}

// Writer-thread scratch for the aligned plane copy (only the writer touches it).
std::vector<uint64_t> vcd_writer_scratch;

// -- $dumplimit (LRM §21.7.1.5) --
//
// The cap and the "cap reached" edge are the only VCD state both threads see: the
// sim thread sets the cap at the $dumplimit call and reads the edge at the next
// flush; the writer, which is the only thread that knows how many bytes the text
// actually takes, tests the cap and publishes the edge. Both are reset at close.
std::atomic<uint64_t> vcd_limit{ 0 };
std::atomic<bool>     vcd_limit_hit{ false };

// Writer-side test, run at instant boundaries only, so the dump never ends
// mid-instant: the file keeps the last whole instant that fit, then the comment
// the LRM asks for. Everything after it is dropped. The console notice goes out
// here, the one place the edge is taken -- a small dump reaches its cap only when
// the writer drains at close, too late for the sim thread to report it.
bool vcd_limit_reached() {
    const uint64_t cap = vcd_limit.load(std::memory_order_relaxed);
    if (cap == 0 || g_vcd.written + g_vcd.out_len < cap) {
        return false;
    }
    vcd_puts("$comment Dump file limit (");
    vcd_put_dec(cap);
    vcd_puts(" bytes) reached. $end\n");
    vcd_limit_hit.store(true, std::memory_order_release);
    std::fprintf(stderr, "$dumplimit: dump file limit (%" PRIu64 " bytes) reached; dumping stopped\n", cap);
    return true;
}

// Format one record byte buffer to the output file. Reads only the record bytes
// and the read-only var table (no diff / shadow state), so it runs on the writer
// thread while the sim thread fills the next buffer.
void vcd_write_records(const std::vector<uint8_t>& buf) {
    size_t p = 0;
    while (p < buf.size()) {
        const uint8_t tag = buf[p++];
        if (tag == 1) {
            if (vcd_limit_hit.load(std::memory_order_relaxed) || vcd_limit_reached()) {
                return;   // capped: this instant and everything after it is dropped
            }
            uint64_t tick = 0;
            std::memcpy(&tick, &buf[p], sizeof(tick));
            p += sizeof(tick);
            vcd_put('#');
            vcd_put_dec(tick);
            vcd_put('\n');
        } else {
            uint32_t idx = 0, nw = 0;
            std::memcpy(&idx, &buf[p], sizeof(idx));
            p += sizeof(idx);
            std::memcpy(&nw, &buf[p], sizeof(nw));
            p += sizeof(nw);
            // Copy the (unaligned) planes into the aligned scratch (reserved to
            // 3*max_nwords at start, so no resize) before formatting.
            std::memcpy(vcd_writer_scratch.data(), &buf[p], static_cast<size_t>(3) * nw * 8);
            p += static_cast<size_t>(3) * nw * 8;
            const uint64_t* planes = vcd_writer_scratch.data();
            vcd_emit(g_vcd.vars[idx], planes, planes + nw, planes + 2 * nw);
        }
    }
}

// -- Writer thread --
//
// The sim thread fills g_vcd.recbuf and, at instant boundaries, hands a full
// buffer to the writer thread through a bounded pool: the sim thread never blocks
// on formatting or the disk unless the writer falls a whole pool behind
// (backpressure). Buffers are recycled, so steady state does not allocate. The
// var table and file are the only shared state: the table is read-only after
// rt_vcd_start, and after the thread spawns the file is written by the writer
// alone (the header is written earlier, on the sim thread, before the spawn).

constexpr size_t kVcdBuffers      = 8;             // record buffers in the pool
constexpr size_t kVcdHandoffBytes = 64u * 1024u;   // hand off once a buffer holds this many bytes

std::thread                        vcd_thread;
std::mutex                         vcd_mu;
std::condition_variable            vcd_cv_ready;   // writer waits for filled buffers
std::condition_variable            vcd_cv_free;    // sim waits for a recycled buffer
std::condition_variable            vcd_cv_idle;    // $dumpflush waits for the writer to catch up
std::queue<std::vector<uint8_t>>   vcd_ready;      // filled buffers awaiting write
std::vector<std::vector<uint8_t>>  vcd_pool;       // recycled empty buffers
size_t                             vcd_inflight = 0;  // buffers handed off but not yet written
bool                               vcd_stop     = false;

void vcd_writer_loop() {
    for (;;) {
        std::vector<uint8_t> buf;
        {
            std::unique_lock<std::mutex> lk(vcd_mu);
            vcd_cv_ready.wait(lk, [] { return !vcd_ready.empty() || vcd_stop; });
            if (vcd_ready.empty()) {
                return;   // stop requested and nothing left to write
            }
            buf = std::move(vcd_ready.front());
            vcd_ready.pop();
        }
        vcd_write_records(buf);   // format + write outside the lock
        {
            std::unique_lock<std::mutex> lk(vcd_mu);
            buf.clear();
            vcd_pool.push_back(std::move(buf));
            --vcd_inflight;
            vcd_cv_free.notify_one();
            if (vcd_ready.empty() && vcd_inflight == 0) {
                vcd_cv_idle.notify_all();
            }
        }
    }
}

// Hand the current record buffer to the writer and take a recycled one. Blocks
// only if every pooled buffer is in flight (backpressure). A no-op on an empty
// buffer.
void vcd_handoff() {
    if (g_vcd.recbuf.empty()) {
        return;
    }
    std::unique_lock<std::mutex> lk(vcd_mu);
    vcd_cv_free.wait(lk, [] { return !vcd_pool.empty(); });
    std::vector<uint8_t> next = std::move(vcd_pool.back());
    vcd_pool.pop_back();
    next.clear();
    vcd_ready.push(std::move(g_vcd.recbuf));
    ++vcd_inflight;
    g_vcd.recbuf = std::move(next);
    vcd_cv_ready.notify_one();
}

// Sim-thread side of the $dumplimit cap: has the writer reported the file full?
// It switches dumping off for the rest of the run -- a later $dumpon / $dumpall /
// $dumpvars does not restart it, since the file already carries its cutoff
// comment -- and drops the records still queued behind the cutoff.
bool vcd_capped() {
    if (!vcd_limit_hit.load(std::memory_order_acquire)) {
        return false;
    }
    g_vcd.active      = false;
    rt_vcd_active     = 0;
    g_vcd.pending_off = false;
    g_vcd.force_all   = false;
    g_vcd.recbuf.clear();
    return true;
}

} // namespace

extern "C" void rt_vcd_open(const char* path_buf, uint32_t path_len, uint32_t forward) {
    g_vcd.path = forward != 0 ? std::string(path_buf, path_len) : unpack_ascii(path_buf, path_len);
}

extern "C" void rt_vcd_begin(void* wheel, uint64_t tick_fs, int32_t units) {
    (void)units;
    if (g_vcd.file != nullptr) {
        return;   // a repeat $dumpvars: header already written (rt_vcd_start re-arms)
    }
    if (g_vcd.path.empty()) {
        g_vcd.path = "dump.vcd";   // LRM default when $dumpfile is omitted
    }
    g_vcd.file = std::fopen(g_vcd.path.c_str(), "wb");
    if (g_vcd.file == nullptr) {
        std::fprintf(stderr, "$dumpvars: cannot open '%s'\n", g_vcd.path.c_str());
        return;
    }
    g_vcd.iobuf   = static_cast<char*>(std::malloc(VcdEngine::iobuf_size));
    g_vcd.out_len = 0;
    // We buffer output ourselves in iobuf; make the FILE unbuffered so a bulk
    // fwrite of the rendered bytes goes straight to write() with no second copy.
    std::setvbuf(g_vcd.file, nullptr, _IONBF, 0);
    g_vcd.wheel = static_cast<rt_wheel*>(wheel);

    vcd_puts("$date\n\tClotho simulation\n$end\n");
    vcd_puts("$version\n\tClotho VCD dump\n$end\n");
    vcd_write_timescale(tick_fs);
}

extern "C" void rt_vcd_scope(const char* name, uint32_t name_len) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    vcd_puts("$scope module ");
    vcd_puts(std::string_view(name, name_len));
    vcd_puts(" $end\n");
}

extern "C" void rt_vcd_upscope(void) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    vcd_puts("$upscope $end\n");
}

// Write one $var line and add the variable to the scan table. Shared by the
// scalar (rt_vcd_var) and per-element array (rt_vcd_array) registration; every
// variable it adds is instrumented with rt_vcd_touch at its write sites, so the
// flush needs no per-variable scan policy.
static void vcd_register(std::string_view ref, uint32_t width, uint32_t flags, const uint8_t* mm,
                         const uint8_t* z) {
    VcdVar v;
    v.mm         = mm;
    v.z          = z;
    v.width      = width;
    v.nwords     = vcd_nwords(width);
    v.four_state = (flags & RT_VCD_FLAG_FOUR_STATE) != 0;
    v.narrow     = (flags & RT_VCD_FLAG_NARROW) != 0;
    v.is_real    = (flags & RT_VCD_FLAG_REAL) != 0;
    v.is_event   = (flags & RT_VCD_FLAG_EVENT) != 0;
    v.idcode     = vcd_idcode(g_vcd.next_id++);
    v.mm_bytes   = v.narrow ? (width + 7) / 8 : (2 * width + 7) / 8;
    v.z_bytes    = z != nullptr ? (width + 7) / 8 : 0;
    if (v.is_event) {
        // `mm` is the event's 64-bit last-trigger tick, not a value slot: diff all
        // eight bytes, and seed the shadow with the same never-triggered sentinel
        // the design state holds, so a trigger at tick 0 still reads as a change.
        // `seen` starts true for the same reason -- there is no power-on value to
        // dump, only occurrences.
        v.mm_bytes = sizeof(uint64_t);
        v.z_bytes  = 0;
        v.seen     = true;
        v.shadow.assign(v.mm_bytes, 0xFF);
    } else {
        v.shadow.assign(static_cast<size_t>(v.mm_bytes) + v.z_bytes, 0);
    }
    v.suffix     = width == 1 ? v.idcode + "\n" : " " + v.idcode + "\n";
    if (v.nwords > g_vcd.max_nwords) {
        g_vcd.max_nwords = v.nwords;
    }
    const char* vtype = v.is_real                          ? "real"
                        : v.is_event                       ? "event"
                        : (flags & RT_VCD_FLAG_NET) != 0   ? "wire"
                                                           : "reg";
    vcd_puts("$var ");
    vcd_puts(vtype);
    vcd_put(' ');
    vcd_put_dec(v.is_real ? 64 : width);
    vcd_put(' ');
    vcd_puts(v.idcode);
    vcd_put(' ');
    vcd_puts(ref);
    vcd_puts(" $end\n");
    g_vcd.vars.push_back(std::move(v));
}

// Mirror of g_vcd.active exposed as a plain external byte (global scope so the
// symbol matches the JIT's external reference) so the JIT can inline the "is VCD
// dumping active?" check and skip the rt_vcd_touch call entirely when it is not.
extern "C" unsigned char rt_vcd_active = 0;

extern "C" void rt_vcd_var(const char* name, uint32_t name_len, uint32_t width, uint32_t flags,
                           const uint8_t* mm, const uint8_t* z) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    vcd_register(std::string_view(name, name_len), width, flags, mm, z);
}

extern "C" void rt_vcd_touch(uint32_t idx) {
    // idx >= epoch.size() covers a not-dumped NBA target passed as (uint32_t)-1.
    // Setting the flag is idempotent, so no dedup / list bookkeeping is needed.
    if (g_vcd.active && idx < g_vcd.epoch.size()) {
        g_vcd.epoch[idx] = 1;
    }
}

extern "C" void rt_vcd_touch_range(uint32_t base, uint32_t count) {
    if (!g_vcd.active) {
        return;
    }
    uint32_t end = base + count;
    if (end > g_vcd.epoch.size()) {
        end = static_cast<uint32_t>(g_vcd.epoch.size());
    }
    for (uint32_t i = base; i < end; ++i) {
        g_vcd.epoch[i] = 1;
    }
}

extern "C" void rt_vcd_array(const char* name, uint32_t name_len, uint32_t count, uint32_t width, uint32_t flags,
                             const uint8_t* mm_base, const uint8_t* z_base, uint32_t stride, int64_t left,
                             int64_t right) {
    if (g_vcd.file == nullptr || g_vcd.started) {
        return;
    }
    const std::string_view leaf(name, name_len);
    for (uint32_t k = 0; k < count; ++k) {
        // Storage index k maps to declared address left+k (ascending range) or
        // left-k (descending), matching the element-select normalization.
        const int64_t     addr = left <= right ? left + static_cast<int64_t>(k) : left - static_cast<int64_t>(k);
        const std::string ref  = std::string(leaf) + "[" + std::to_string(addr) + "]";
        const uint8_t*    mm   = mm_base + static_cast<size_t>(k) * stride;
        const uint8_t*    z    = z_base != nullptr ? z_base + static_cast<size_t>(k) * stride : nullptr;
        vcd_register(ref, width, flags, mm, z);
    }
}

extern "C" void rt_vcd_start(void) {
    if (g_vcd.file == nullptr) {
        return;
    }
    if (!g_vcd.started) {
        vcd_puts("$enddefinitions $end\n");
        // The scan scratch holds one variable's 3*nwords planes plus, in its tail,
        // the raw 2W-bit must|may read buffer (up to 2*max_nwords+1 words).
        g_vcd.scratch.assign(static_cast<size_t>(5) * g_vcd.max_nwords + 2, 0);
        // Dirty set: one epoch flag per var, set by rt_vcd_touch and scanned +
        // cleared in index order at flush.
        g_vcd.epoch.assign(g_vcd.vars.size(), 0);
        vcd_writer_scratch.reserve(static_cast<size_t>(3) * g_vcd.max_nwords);
        // Size every record buffer to hold a full handoff batch plus one more whole
        // instant, so the producer never allocates and never splits an instant
        // across buffers. A worst-case instant is a full-dump change for every
        // variable plus one time marker. Spawn the writer only after the header is
        // fully written on this (sim) thread, so file access never overlaps.
        size_t instant_bytes = 1 + sizeof(uint64_t);   // one time marker
        for (const VcdVar& v : g_vcd.vars) {
            instant_bytes += 1 + 2 * sizeof(uint32_t) + static_cast<size_t>(3) * v.nwords * 8;
        }
        const size_t cap = kVcdHandoffBytes + instant_bytes;
        g_vcd.recbuf.reserve(cap);
        vcd_pool.clear();
        for (size_t k = 0; k < kVcdBuffers; ++k) {
            std::vector<uint8_t> b;
            b.reserve(cap);
            vcd_pool.push_back(std::move(b));
        }
        vcd_stop      = false;
        vcd_inflight  = 0;
        vcd_thread    = std::thread(vcd_writer_loop);
        g_vcd.started = true;
    }
    if (vcd_capped()) {
        return;   // the cap was already reached: $dumpvars does not restart the dump
    }
    g_vcd.active    = true;
    rt_vcd_active   = 1;
    g_vcd.force_all = true;   // the first flush emits every variable's initial value
}

extern "C" void rt_vcd_flush(uint64_t tick) {
    if (g_vcd.file == nullptr || vcd_capped()) {
        return;
    }
    uint64_t* const scan  = g_vcd.scratch.data();
    const uint32_t  nvars = static_cast<uint32_t>(g_vcd.vars.size());
    if (g_vcd.pending_off) {
        // $dumpoff checkpoint: mark every variable all-x at this instant. An event
        // is skipped -- it has no value to make unknown, and `1<id>` here would
        // read as a trigger that never happened.
        const size_t cmark = g_vcd.recbuf.size();
        vcd_rec_time(tick);
        bool checkpointed = false;
        for (uint32_t i = 0; i < nvars; ++i) {
            VcdVar& v = g_vcd.vars[i];
            if (v.is_event) {
                g_vcd.epoch[i] = 0;
                continue;
            }
            const uint32_t nw = v.nwords;
            std::memset(scan, 0, static_cast<size_t>(3) * nw * 8);   // must = 0, z = 0
            for (uint32_t k = 0; k < nw; ++k) {
                scan[nw + k] = ~0ull;   // may = all ones (x)
            }
            scan[2 * nw - 1] &= vcd_topmask(v.width);
            vcd_rec_change(i, scan, nw);
            checkpointed = true;
            // The shadow (raw slot) is left stale: $dumpon re-arms a full dump
            // (force_all), which re-emits every variable and rewrites the shadow.
            v.seen         = true;
            g_vcd.epoch[i] = 0;
        }
        if (!checkpointed) {
            // Nothing to make unknown (a design whose only dumped variables are
            // events): drop the marker rather than write a bare `#tick`.
            g_vcd.recbuf.resize(cmark);
        }
        g_vcd.pending_off = false;
        g_vcd.active      = false;
        rt_vcd_active     = 0;
        vcd_handoff();
        return;
    }
    if (!g_vcd.active) {
        return;
    }
    // Produce a time marker followed by the changed variables; drop the marker if
    // nothing changed (no-change instants emit nothing). Scan in index order,
    // reading + diffing only the variables the dirty set marked. No per-instant
    // sort: index order is byte-identical to a full scan for free, and the epoch
    // check skips the expensive read/diff for the rest.
    const size_t mark = g_vcd.recbuf.size();
    vcd_rec_time(tick);
    bool any = false;
    if (g_vcd.force_all) {
        for (uint32_t i = 0; i < nvars; ++i) {
            const bool touched = g_vcd.epoch[i] != 0;
            g_vcd.epoch[i]     = 0;
            VcdVar& v          = g_vcd.vars[i];
            if (v.is_event) {
                // A forced dump ($dumpvars / $dumpall / $dumpon) states current
                // values, and an event has none -- it is emitted only when it
                // actually triggers. So pulse only if it triggered this instant,
                // and otherwise resync the shadow, which keeps a trigger that
                // happened while dumping was off from surfacing as a pulse here.
                if (touched) {
                    any |= vcd_scan_var(i, false);
                } else {
                    std::memcpy(v.shadow.data(), v.mm, v.mm_bytes);
                }
                continue;
            }
            any |= vcd_scan_var(i, true);
        }
    } else {
        // Hot loop over one dense byte array (no fat-struct access): scan a var
        // iff it was written this instant.
        uint8_t* ep = g_vcd.epoch.data();
        for (uint32_t i = 0; i < nvars; ++i) {
            if (ep[i] != 0) {
                ep[i] = 0;
                any |= vcd_scan_var(i, false);
            }
        }
    }
    if (!any) {
        g_vcd.recbuf.resize(mark);
    }
    g_vcd.force_all = false;
    // Hand the batch to the writer once it is large enough; small instants
    // accumulate to amortize the handoff. The remainder flushes at $dumpflush /
    // close.
    if (g_vcd.recbuf.size() >= kVcdHandoffBytes) {
        vcd_handoff();
    }
}

extern "C" void rt_vcd_off(void) {
    if (g_vcd.active) {
        g_vcd.pending_off = true;
    }
}

extern "C" void rt_vcd_on(void) {
    if (g_vcd.started && !vcd_capped()) {
        g_vcd.active    = true;
        rt_vcd_active   = 1;
        g_vcd.force_all = true;
    }
}

extern "C" void rt_vcd_all(void) {
    if (g_vcd.active) {
        g_vcd.force_all = true;
    }
}

extern "C" void rt_vcd_limit(uint64_t bytes) {
    // Legal before or after $dumpvars; the writer picks the new cap up at its next
    // instant boundary. A cap already passed is honoured there, not here, so the
    // cutoff comment is always written by the thread that owns the file.
    vcd_limit.store(bytes, std::memory_order_relaxed);
}

extern "C" void rt_vcd_dumpflush(void) {
    if (g_vcd.file == nullptr) {
        return;
    }
    // Hand off any buffered records and wait until the writer has drained
    // everything, then push the file's own buffer to the OS. Waiting until the
    // writer is idle means no write is in flight, so the fflush cannot race it.
    vcd_handoff();
    std::unique_lock<std::mutex> lk(vcd_mu);
    vcd_cv_idle.wait(lk, [] { return vcd_ready.empty() && vcd_inflight == 0; });
    vcd_flush_out();   // writer is idle -> no concurrent append into iobuf
    std::fflush(g_vcd.file);
}

// Stop the writer thread and reset all threading state. Safe to call when no
// thread was ever spawned (a $dumpvars that never ran).
static void vcd_shutdown_writer() {
    if (vcd_thread.joinable()) {
        {
            std::unique_lock<std::mutex> lk(vcd_mu);
            vcd_stop = true;
            vcd_cv_ready.notify_all();
        }
        vcd_thread.join();
    }
    while (!vcd_ready.empty()) {
        vcd_ready.pop();
    }
    vcd_pool.clear();
    vcd_inflight = 0;
    vcd_stop     = false;
    // The cap outlives the engine struct (both threads read it), so it resets here
    // with the rest of the cross-thread state.
    vcd_limit.store(0, std::memory_order_relaxed);
    vcd_limit_hit.store(false, std::memory_order_relaxed);
}

extern "C" void rt_vcd_close(void) {
    if (g_vcd.file == nullptr) {
        vcd_shutdown_writer();
        g_vcd = VcdEngine{};
        return;
    }
    if (g_vcd.active || g_vcd.pending_off) {
        const uint64_t now = g_vcd.wheel != nullptr ? unwrap(g_vcd.wheel)->now() : 0;
        rt_vcd_flush(now);
    }
    vcd_handoff();            // push the final partial buffer
    vcd_shutdown_writer();    // drain + join the writer before touching the file
    vcd_flush_out();          // writer joined -> flush the last rendered bytes
    std::fflush(g_vcd.file);
    std::fclose(g_vcd.file);
    std::free(g_vcd.iobuf);
    g_vcd = VcdEngine{};      // reset for the next simulation in this process
}

// -- DPI-C packed-vector marshalling (see Runtime.h) --

void rt_dpi_pack_logicvec(const uint32_t* must, const uint32_t* may, const uint32_t* z,
                          uint32_t nchunks, uint32_t* out) {
    for (uint32_t k = 0; k < nchunks; ++k) {
        out[2 * k]     = may[k] & ~z[k];   // aval: value-1 or X, but not Z
        out[2 * k + 1] = must[k] ^ may[k]; // bval: any unknown (X or Z)
    }
}

void rt_dpi_unpack_logicvec(const uint32_t* in, uint32_t nchunks, uint32_t* must, uint32_t* may,
                            uint32_t* z) {
    for (uint32_t k = 0; k < nchunks; ++k) {
        const uint32_t aval = in[2 * k];
        const uint32_t bval = in[2 * k + 1];
        must[k] = aval & ~bval;
        may[k]  = aval | bval;
        z[k]    = bval & ~aval;
    }
}

void rt_dpi_str_out(rt_string* dst, const char* p, const rt_alloc_site* site) {
    rt_string_assign_bytes(dst, p, p != nullptr ? std::strlen(p) : 0, site);
}
