#include "Runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <sys/resource.h>

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

// Render `val` as ceil(width/bits) digits, most-significant first. `bits` is 1
// (binary), 3 (octal) or 4 (hex). Unknown digits follow LRM §21.2.1.3 casing:
// lowercase x/z when every in-range bit of the group is x / z, uppercase X/Z
// when only some are (Z only when the group has no x bits). The top group
// clips to the operand width so its "every bit" test ignores out-of-range bits.
void render_digits(std::string& out, uint64_t val, uint64_t xz, uint64_t z, uint32_t width, uint32_t bits) {
    if (width == 0) {
        width = 1;
    }
    static constexpr char kHex[]   = "0123456789abcdef";
    const uint64_t        dig_mask = (uint64_t{ 1 } << bits) - 1;
    const uint64_t        all_mask = width_mask(width);
    const uint32_t        digits   = (width + bits - 1) / bits;
    for (uint32_t d = digits; d-- > 0;) {
        const uint32_t shift = d * bits;
        const uint64_t gm    = dig_mask & (all_mask >> shift);
        const uint64_t dxz   = (xz >> shift) & gm;
        if (dxz == 0) {
            out += kHex[(val >> shift) & gm];
            continue;
        }
        const uint64_t dz = (z >> shift) & gm;
        const uint64_t dx = dxz & ~dz;
        if (dx != 0) {
            out += dx == gm ? 'x' : 'X';
        } else {
            out += dz == gm ? 'z' : 'Z';
        }
    }
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
    const uint32_t w    = a.width ? a.width : 1;
    const uint64_t mask = width_mask(w);
    const uint64_t val  = a.val & mask;
    const uint64_t xz   = a.xz & mask;
    const uint64_t z    = a.z & mask;

    std::string field;
    // Field width to justify to; an explicit specifier width overrides the
    // per-conversion default filled in below. Width 0 means minimal.
    uint32_t target   = spec.has_width ? spec.width : 0;
    bool     zero_pad = false;

    switch (spec.conv) {
        case 'd':
        case 'D':
            if (!spec.has_width) {
                // Automatic size: the largest possible value of a `w`-bit
                // operand; a signed operand's widest text is its most negative
                // value, sign included (LRM §21.2.1.2).
                target = (a.flags & 1u) != 0 ? 1 + dec_digits(uint64_t{ 1 } << (w - 1))
                                             : dec_digits(mask);
            }
            if (xz != 0) {
                // LRM §21.2.1.3: whole-field collapse, lowercase only when
                // every bit is x (resp. z), uppercase X when x bits mix with
                // anything, uppercase Z when z bits mix with known bits.
                const uint64_t x = xz & ~z;
                if (x != 0) {
                    field += x == mask ? 'x' : 'X';
                } else {
                    field += z == mask ? 'z' : 'Z';
                }
            } else if ((a.flags & 1u) != 0) {
                const uint64_t sext = (w < 64 && ((val >> (w - 1)) & 1u)) ? (val | ~mask) : val;
                field = std::to_string(static_cast<int64_t>(sext));
            } else {
                field = std::to_string(val);
            }
            break;
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
            render_digits(field, val, xz, z, w, bits);
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
            field += static_cast<char>(val & 0xff);
            break;
        case 't':
        case 'T':
            // The frontend scales a %t argument to design ticks, so `val` is the
            // tick count; render_time converts it per the current $timeformat,
            // whose min width is the default field width for %t.
            render_time(field, val);
            if (!spec.has_width && timeformat.min_width > 0) {
                target = static_cast<uint32_t>(timeformat.min_width);
            }
            break;
        case 's':
        case 'S':
            if ((a.flags & 2u) != 0) {
                // A string object: `val` carries the rt_string handle pointer.
                const auto* s = reinterpret_cast<const rt_string*>(static_cast<uintptr_t>(a.val));
                field.append(str_data(s), str_len(s));
                break;
            }
            // A packed value: ASCII chars most-significant first. Leading 0x00
            // bytes are never printed (LRM §21.2.1.7 — iverilog deviates and
            // prints a space per leading NUL; the LRM text and the §21.2.1.1
            // example are explicit, so LRM wins). An embedded/trailing 0x00
            // prints as a space (LRM silent; matches iverilog).
            {
                uint32_t k = (w + 7) / 8;
                while (k > 0 && ((val >> (8 * (k - 1))) & 0xff) == 0) {
                    --k;
                }
                while (k-- > 0) {
                    const auto c = static_cast<char>((val >> (8 * k)) & 0xff);
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
            std::memcpy(&v, &a.val, sizeof v);
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
    const uint32_t w    = a.width ? a.width : 1;
    const uint64_t mask = width_mask(w);
    const uint64_t xz   = a.xz & mask;
    const uint64_t aval = ((a.val & ~a.xz) | (xz & ~a.z)) & mask;
    const uint32_t words = (w + 31) / 32;
    for (uint32_t k = 0; k < words; ++k) {
        const auto append_word = [&out](uint64_t plane, uint32_t word) {
            const auto u = static_cast<uint32_t>(plane >> (32 * word));
            out.append(reinterpret_cast<const char*>(&u), sizeof(u));
        };
        append_word(four_state ? aval : (a.val & ~a.xz & mask), k);
        if (four_state) {
            append_word(xz, k);
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

// Number of 64-bit words a `width`-bit value occupies (>= 1).
uint32_t plane_words(uint32_t width) {
    return width == 0 ? 1 : (width + 63) / 64;
}

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

// -- SystemVerilog string representation --
//
// fbstring-style small-string optimization over the 32 opaque bytes of an
// rt_string (Runtime.h). Two forms, discriminated by the MSB of byte 31:
//
//   inline: bytes[0..30] hold the characters; byte 31 holds 31 - len, so a full
//           31-char string stores 0 there and no separate flag byte is spent.
//   heap:   a StrHeap {ptr, len, cap} occupies bytes[0..23]; byte 31 is 0x80.
//
// Strings are raw byte sequences with an explicit length; no NUL terminator is
// stored or relied upon. A heap buffer never shrinks back to inline — the
// capacity is kept for reuse so reassignment in a loop stops allocating.

constexpr uint64_t      str_inline_cap = 31;
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
void str_assign(rt_string* s, const char* p, uint64_t n) {
    if (!str_is_heap(s)) {
        if (n <= str_inline_cap) {
            if (n != 0) {
                std::memmove(s->storage, p, n);
            }
            s->storage[31] = static_cast<unsigned char>(str_inline_cap - n);
            return;
        }
        char* buf = static_cast<char*>(std::malloc(n));
        std::memcpy(buf, p, n);
        str_set_heap(s, StrHeap{ buf, n, n });
        return;
    }
    StrHeap h = str_heap(s);
    if (n <= h.cap) {
        if (n != 0) {
            std::memmove(h.ptr, p, n);
        }
        h.len = n;
        str_set_heap(s, h);
        return;
    }
    // Geometric growth so repeated append-style reassignment amortizes.
    const uint64_t cap = h.cap * 2 > n ? h.cap * 2 : n;
    char*          buf = static_cast<char*>(std::malloc(cap));
    std::memcpy(buf, p, n);
    std::free(h.ptr);
    str_set_heap(s, StrHeap{ buf, n, cap });
}

// Grow s's buffer to hold at least n bytes, preserving contents and length.
// The handle stays valid across the grow, so aliased readers of s (same handle
// pointer) keep seeing the current bytes.
void str_reserve(rt_string* s, uint64_t n) {
    if (!str_is_heap(s)) {
        if (n <= str_inline_cap) {
            return;
        }
        const uint64_t len = str_inline_cap - s->storage[31];
        char*          buf = static_cast<char*>(std::malloc(n));
        std::memcpy(buf, s->storage, len);
        str_set_heap(s, StrHeap{ buf, len, n });
        return;
    }
    StrHeap h = str_heap(s);
    if (n <= h.cap) {
        return;
    }
    // Geometric growth so repeated append-style reassignment amortizes.
    const uint64_t cap = h.cap * 2 > n ? h.cap * 2 : n;
    char*          buf = static_cast<char*>(std::malloc(cap));
    std::memcpy(buf, h.ptr, h.len);
    std::free(h.ptr);
    str_set_heap(s, StrHeap{ buf, h.len, cap });
}

// Set the length of s; capacity for n bytes must already be reserved.
void str_set_len(rt_string* s, uint64_t n) {
    if (str_is_heap(s)) {
        StrHeap h = str_heap(s);
        h.len     = n;
        str_set_heap(s, h);
        return;
    }
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

    explicit StrScratch(uint64_t n) {
        buf = n <= sizeof(stack) ? stack : static_cast<char*>(std::malloc(n));
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

extern "C" void rt_wheel_tick(rt_wheel* wheel, void* ctx) {
    unwrap(wheel)->tick(ctx);
}

extern "C" int rt_wheel_advance(rt_wheel* wheel, void* ctx) {
    return unwrap(wheel)->advance(ctx) ? 1 : 0;
}

extern "C" void rt_fwrite(int32_t desc, const char* fmt, uint32_t argc, const rt_arg* argv) {
    io_ensure_init();
    // Write raw: `out` may contain '%' or '{}' that printf/std::print would treat
    // as format syntax.
    std::string out;
    render(out, fmt, argc, argv);
    if ((desc & kFdBit) != 0) {
        if (std::FILE* f = fd_stream(desc)) {
            std::fwrite(out.data(), 1, out.size(), f);
        }
        return;
    }
    // MCD: emit to every set channel (LRM §21.3.4 multichannel descriptor).
    for (int b = 0; b < 31; ++b) {
        if ((desc & (int32_t{ 1 } << b)) != 0 && mcd_channels[b] != nullptr) {
            std::fwrite(out.data(), 1, out.size(), mcd_channels[b]);
        }
    }
}

extern "C" int32_t rt_fopen_mcd(const char* name_buf, uint32_t name_len) {
    io_ensure_init();
    const std::string path = unpack_ascii(name_buf, name_len);
    // Claim the lowest free channel; bit 0 is the reserved standard output.
    for (int b = 1; b < 31; ++b) {
        if (mcd_channels[b] == nullptr) {
            std::FILE* f = std::fopen(path.c_str(), "w");
            if (f == nullptr) {
                return 0;
            }
            mcd_channels[b] = f;
            return int32_t{ 1 } << b;
        }
    }
    return 0; // all 30 channels in use
}

extern "C" int32_t
rt_fopen_fd(const char* name_buf, uint32_t name_len, const char* mode_buf, uint32_t mode_len) {
    io_ensure_init();
    const std::string path = unpack_ascii(name_buf, name_len);
    const std::string mode = unpack_ascii(mode_buf, mode_len);
    std::FILE*        f    = std::fopen(path.c_str(), mode.c_str());
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

extern "C" void rt_sformat_str(rt_string* dst, const char* fmt, uint32_t argc, const rt_arg* argv) {
    std::string out;
    render(out, fmt, argc, argv);
    rt_string_assign_bytes(dst, out.data(), out.size());
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

extern "C" int32_t
rt_sscanf(const char* buf, uint32_t len, const char* fmt, uint32_t argc, const rt_scan_arg* args) {
    // The source is a packed-ASCII vector; reconstruct the string, then scan it.
    const std::string s = unpack_ascii(buf, len);
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
std::jmp_buf* finish_env = nullptr;

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

} // namespace

extern "C" void rt_finish_host(void* env) {
    finish_env = static_cast<std::jmp_buf*>(env);
}

extern "C" void rt_finish(const char* banner, uint64_t now, int32_t arg) {
    if (arg >= 1) {
        std::fflush(stdout);
        std::string line = banner;
        line += " called at ";
        append_finish_time(line, now);
        if (arg >= 2) {
            rusage ru;
            getrusage(RUSAGE_SELF, &ru);
            const double cpu = static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                               static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
            char stats[128];
            std::snprintf(stats, sizeof(stats), "\n  Memory: %ld KB  CPU time: %.3f seconds",
                          ru.ru_maxrss, cpu);
            line += stats;
        }
        line += '\n';
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }
    // Jump back to the host that started the run when one is installed — the
    // codegen relies on rt_finish being noreturn, so returning normally is not an
    // option. Otherwise exit; std::exit runs atexit handlers and flushes C stdio,
    // so any buffered $display output is committed before the process leaves.
    if (finish_env != nullptr) {
        std::longjmp(*finish_env, 1);
    }
    std::exit(0);
}

extern "C" void rt_case_violation(const char* banner, uint64_t now, int32_t kind) {
    std::fflush(stdout);
    std::string line = banner;
    line += kind == 0 ? "no condition is true in the unique/priority if/case at time "
                      : "if/case statement is not unique (overlapping items) at time ";
    append_finish_time(line, now);
    line += '\n';
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

extern "C" void rt_sim_run(void* state, void* wheel, void (*drain)(void*, void*),
                           void (*finals)(void*, void*)) {
    // Run the simulation, then the `final` blocks (LRM 9.2.3), so the finals see
    // the settled design state on BOTH exit paths. The natural drain returns from
    // `drain`; a $finish inside it longjmps out of the abandoned coroutine frames
    // back to the local catch below — main's state/wheel allocas stay live because
    // main sits beneath this frame. Either way, control reaches the finals.
    std::jmp_buf  local;
    std::jmp_buf* prev = finish_env; // outer host catch (run_jit), restored below
    if (setjmp(local) == 0) {
        finish_env = &local;
        drain(state, wheel);
    }
    // Restore the outer catch before the finals so a $finish in a final block
    // exits cleanly (to the host / process exit) rather than re-entering here.
    finish_env = prev;
    finals(state, wheel);
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

} // namespace

extern "C" int32_t rt_random(void) {
    // rtl_dist_uniform's INT32_MIN/INT32_MAX branch: renormalize the uniform
    // draw across the full 32-bit span, then floor toward negative infinity.
    double r = (rng_uniform(g_random_seed) + 2147483648.0) / 4294967295.0;
    r        = r * 4294967296.0 - 2147483648.0;
    const long i = r >= 0 ? static_cast<long>(r) : static_cast<long>(r - 1);
    return static_cast<int32_t>(i);
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
    s->storage[31] = str_inline_cap; // Inline form, length 0.
}

extern "C" void rt_string_deinit(rt_string* s) {
    if (str_is_heap(s)) {
        std::free(str_heap(s).ptr);
    }
    s->storage[31] = str_inline_cap;
}

extern "C" void rt_string_assign(rt_string* dst, const rt_string* src) {
    if (dst == src) {
        return;
    }
    str_assign(dst, str_data(src), str_len(src));
}

extern "C" void rt_string_assign_bytes(rt_string* dst, const char* p, uint64_t n) {
    str_assign(dst, p, n);
}

extern "C" uint64_t rt_string_len(const rt_string* s) {
    return str_len(s);
}

extern "C" const char* rt_string_data(const rt_string* s) {
    return str_data(s);
}

extern "C" void rt_string_concat(rt_string* dst, const rt_string* const* parts, uint32_t n) {
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
        str_reserve(dst, total);
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
    StrScratch scratch(total);
    uint64_t   off = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint64_t len = str_len(parts[k]);
        std::memcpy(scratch.buf + off, str_data(parts[k]), len);
        off += len;
    }
    str_assign(dst, scratch.buf, total);
}

extern "C" void rt_string_repl(rt_string* dst, const rt_string* src, uint64_t count) {
    const uint64_t len   = str_len(src);
    const uint64_t total = len * count;
    StrScratch     scratch(total);
    for (uint64_t k = 0; k < count; ++k) {
        std::memcpy(scratch.buf + k * len, str_data(src), len);
    }
    str_assign(dst, scratch.buf, total);
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

extern "C" void rt_string_substr(rt_string* dst, const rt_string* src, int32_t i, int32_t j) {
    const uint64_t len = str_len(src);
    if (i < 0 || j < i || static_cast<uint64_t>(j) >= len) {
        str_assign(dst, nullptr, 0);
        return;
    }
    str_assign(dst, str_data(src) + i, static_cast<uint64_t>(j) - static_cast<uint64_t>(i) + 1);
}

extern "C" void rt_enum_name(rt_string* dst, uint64_t val, uint32_t count, const uint64_t* values,
                             const char* const* names, const uint32_t* lens) {
    for (uint32_t k = 0; k < count; ++k) {
        if (values[k] == val) {
            str_assign(dst, names[k], lens[k]);
            return;
        }
    }
    str_assign(dst, nullptr, 0); // No member matched: empty string (LRM 6.19.2).
}

extern "C" void rt_string_toupper(rt_string* dst, const rt_string* src) {
    rt_string_assign(dst, src); // No-op when dst == src, then transform in place.
    char*          p = str_data(dst);
    const uint64_t n = str_len(dst);
    for (uint64_t k = 0; k < n; ++k) {
        p[k] = str_ascii_upper(p[k]);
    }
}

extern "C" void rt_string_tolower(rt_string* dst, const rt_string* src) {
    rt_string_assign(dst, src);
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
void str_toa_base(rt_string* dst, int32_t i, int base) {
    char       buf[36]; // sign + up to 32 binary digits
    const auto res = std::to_chars(buf, buf + sizeof(buf), i, base);
    str_assign(dst, buf, static_cast<uint64_t>(res.ptr - buf));
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

extern "C" void rt_string_itoa(rt_string* dst, int32_t i) {
    str_toa_base(dst, i, 10);
}
extern "C" void rt_string_hextoa(rt_string* dst, int32_t i) {
    str_toa_base(dst, i, 16);
}
extern "C" void rt_string_octtoa(rt_string* dst, int32_t i) {
    str_toa_base(dst, i, 8);
}
extern "C" void rt_string_bintoa(rt_string* dst, int32_t i) {
    str_toa_base(dst, i, 2);
}

extern "C" void rt_string_realtoa(rt_string* dst, double r) {
    char       buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), r);
    str_assign(dst, buf, static_cast<uint64_t>(res.ptr - buf));
}

extern "C" void rt_string_from_packed(rt_string* dst, const uint8_t* val, uint32_t width_bits) {
    const uint32_t nbytes = (width_bits + 7) / 8;
    StrScratch     scratch(nbytes);
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
    str_assign(dst, scratch.buf, out_len);
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

extern "C" void* rt_alloc(uint64_t size) {
    // Coroutine frames may be over-aligned (16-byte plane spills); glibc
    // malloc guarantees only 16, so align generously. aligned_alloc requires
    // the size to be a multiple of the alignment.
    return std::aligned_alloc(64, (size + 63) / 64 * 64);
}

extern "C" void rt_free(void* p) {
    std::free(p);
}
