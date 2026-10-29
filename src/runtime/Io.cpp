// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// File and console I/O, $monitor and $strobe, $readmem, plusargs, $system and DPI helpers.

#include "RuntimeInternal.h"

using namespace rt_internal;

namespace rt_internal {

std::vector<std::FILE*>  fd_streams;
MonitorState             g_monitor;
std::vector<void*>       g_strobe_queue;
std::vector<std::string> g_plusargs;

} // namespace rt_internal

namespace {

// -- File I/O descriptor tables (LRM §21.3) --
//
// Two integer descriptor kinds share this state, distinguished by bit 31 (see
// Runtime.h). The tables are process-global and unsynchronized: one simulator
// per process, single-threaded. Adding thread safety later means a lock around
// these two containers plus stdio's own per-stream locking on the writes.

// Bit 31: set marks an FD, clear marks an MCD.
constexpr int32_t kFdBit = static_cast<int32_t>(0x80000000u);

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

// The $system access tier, set by rt_run before the design runs (0 = off,
// 1 = const, 2 = full). Default off so a design that somehow reaches rt_system
// without rt_run having enabled it is refused, not run.
int g_system_access = 0;

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
                           const rt_arg* argv, uint32_t flags, const rt_fmt_dyn* dyn) {
    // $sformat / $swrite: render like $display, then store the result into the
    // destination string variable (LRM §21.3.3). An oversized result keeps its low
    // (rightmost) bytes, matching a string-to-narrower-reg assignment.
    std::string out;
    render(out, fmt, argc, argv, dyn);
    store_string_planes(mm, z, width, reinterpret_cast<const uint8_t*>(out.data()),
                        static_cast<uint32_t>(out.size()), (flags & 1u) != 0);
}

extern "C" void rt_sformat_str(rt_string* dst, const char* fmt, uint32_t argc, const rt_arg* argv,
                               const rt_alloc_site* site, const rt_fmt_dyn* dyn) {
    std::string out;
    render(out, fmt, argc, argv, dyn);
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
    uint32_t ai    = 0;
    int32_t  count = 0;
    // The clause's two failure kinds (LRM 21.3.4.3): the scan returns EOF (-1)
    // only when input ENDS before the first matching failure or conversion.
    // `input_failed` marks a directive that hit EOF before matching anything;
    // `converted` marks a completed conversion, including a %*-suppressed one
    // (a matched literal is neither). A conflicting non-EOF character is a
    // matching failure and yields the count, so $sscanf("abc", "%d") is 0,
    // while $sscanf("", "%d") is -1.
    bool input_failed = false;
    bool converted    = false;

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
                if (c == EOF) {
                    input_failed = true;
                }
                src.unget(c);
                break; // literal mismatch (or EOF) ends the scan
            }
            continue;
        }

        ++p;
        if (*p == '%') {
            const int c = src.get();
            if (c != '%') {
                if (c == EOF) {
                    input_failed = true;
                }
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
        // The %s token bytes, kept verbatim past the plane packing: a string-object
        // destination stores these, not the planes.
        std::vector<uint8_t> tok;

        if (conv == 'c') {
            const int c = src.get();
            if (c == EOF) {
                input_failed = true;
            } else {
                converted        = true;
                must[0] = may[0] = static_cast<uint8_t>(c);
                ok               = true;
            }
        } else if (conv == 's') {
            skip_ws(src);
            int c;
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
            if (tok.empty()) {
                // After the whitespace skip a non-EOF character joins the token,
                // so an empty token IS input ending at this directive.
                input_failed = true;
            } else {
                converted = true;
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
                converted = true;
                ++used;
                c = src.get();
            }
            src.unget(c);
            if (!ok && c == EOF) {
                input_failed = true;
            }
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
            const rt_scan_arg& a = args[ai++];
            if ((a.flags & 2u) != 0) {
                // A `string` object destination: the token IS the value, assigned
                // verbatim with no width cap, right-justification or padding (LRM
                // 21.3.4.3 sanctions only %s here; the frontend refuses the rest).
                assert(conv == 's' && "a string destination pairs only with %s");
                assert(a.str != nullptr && "a string destination carries its handle");
                assert(!tok.empty() && "an empty %s token is an input failure, not a store");
                rt_string_assign_bytes(a.str, reinterpret_cast<const char*>(tok.data()),
                                       tok.size(), a.site);
            } else {
                const bool narrow = (a.flags & 1u) != 0;
                if (a.width > 64) {
                    write_word_planes_wide(a.mm, a.z, a.width, must.data(), may.data(), z.data(), narrow);
                } else {
                    write_word_planes(a.mm, a.z, a.width, must[0], may[0], z[0], narrow);
                }
            }
            ++count;
        }
    }

    // LRM 21.3.4.3: EOF (-1) only when input ends before the first matching
    // failure or conversion; a matching failure returns the count (possibly 0).
    return (count == 0 && !converted && input_failed) ? -1 : count;
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
namespace {

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

extern "C" int32_t rt_value_plusargs_str(const char* prefix, uint32_t prefix_len, rt_string* dst,
                                         const rt_alloc_site* site) {
    const std::string* pa = find_plusarg(std::string_view(prefix, prefix_len));
    if (pa == nullptr) {
        return 0; // LRM §21.6: no match leaves the variable alone and returns 0.
    }
    // "%s -- string (no conversion)": the remainder IS the value, verbatim. No
    // right-justification and no width cap, unlike the packed-vector form above --
    // a `string` grows to whatever the command line supplied. An empty remainder
    // stores the empty string, which is the clause's stated result.
    assert(pa->size() >= prefix_len);
    rt_string_assign_bytes(dst, pa->data() + prefix_len, pa->size() - prefix_len, site);
    return 1;
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
