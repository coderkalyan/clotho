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

// The string a numeric conversion into a `string` stores: the converted value cast
// to string (LRM 6.16), sized like an unsized literal (LRM 5.7.1: at least 32 bits,
// signed for a decimal, wider when its digits need it; docs/LRM_NOTES.md). `must`
// is the value plane over `words` words, already negated for a negative decimal,
// with a free top bit above the magnitude; unknown digits are 0 in it. A decimal
// is as wide as its two's complement needs, a based value as its digits (`digit_width`
// bits). The cast zero-fills to whole bytes, writes them most significant first and
// drops every 0 byte ("\0" never belongs to a string).
std::string numeric_to_string(const uint64_t* must, uint32_t words, bool decimal, uint32_t digit_width) {
    uint32_t width = digit_width;
    if (decimal) {
        const bool negative = (must[words - 1] >> 63) != 0;
        uint32_t   bits     = 0;
        for (uint32_t k = words; k-- > 0 && bits == 0;) {
            const uint64_t w = negative ? ~must[k] : must[k];
            if (w != 0) {
                bits = k * 64 + 64 - static_cast<uint32_t>(__builtin_clzll(w));
            }
        }
        width = bits + 1;
    }
    width                  = std::max<uint32_t>(width, 32);
    const uint32_t nbytes = (width + 7) / 8;
    std::string    out;
    for (uint32_t j = nbytes; j-- > 0;) {
        auto byte = static_cast<uint8_t>(must[j / 8] >> (8 * (j % 8)));
        if (j == nbytes - 1 && width % 8 != 0) {
            byte = static_cast<uint8_t>(byte & ((1u << (width % 8)) - 1));
        }
        if (byte != 0) {
            out.push_back(static_cast<char>(byte));
        }
    }
    return out;
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

extern "C" void rt_monitor_set(void* ramp, void* record) {
    g_monitor.ramp    = ramp;
    g_monitor.record  = record;
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

extern "C" void* rt_monitor_active_record() {
    return g_monitor.record;
}

namespace {

// rt_arg flags bit 3: a bare $time / $stime / $realtime call.
constexpr uint32_t kArgTime = 8;

// The bytes that identify the values of every argument except the time calls, in
// argument order: what $monitor compares from one time step to the next. A value
// is its planes (a real its bit pattern, a string its characters), so two values
// that print alike but differ, such as a %c of a vector whose high bits changed,
// differ here too.
std::string monitor_key_image(uint32_t argc, const rt_arg* argv) {
    std::string key;
    for (uint32_t k = 0; k < argc; ++k) {
        const rt_arg& a = argv[k];
        if ((a.flags & kArgTime) != 0) {
            continue;
        }
        if ((a.flags & 2u) != 0) {
            const auto*    s   = reinterpret_cast<const rt_string*>(a.val);
            const uint64_t len = str_len(s);
            key.append(reinterpret_cast<const char*>(&len), sizeof len);
            key.append(str_data(s), len);
        } else if ((a.flags & 4u) != 0) {
            key.append(reinterpret_cast<const char*>(a.val), sizeof(uint64_t));
        } else {
            const size_t bytes = size_t{ plane_words(a.width) } * sizeof(uint64_t);
            key.append(reinterpret_cast<const char*>(a.val), bytes);
            key.append(reinterpret_cast<const char*>(a.xz), bytes);
            key.append(reinterpret_cast<const char*>(a.z), bytes);
        }
    }
    return key;
}

} // namespace

// $monitor's print (LRM 21.2.2): a monitor prints when an argument other than
// $time, $stime or $realtime changes value, and once when it is armed or
// re-enabled (`primed`). Change is a change of value, not of rendered text.
extern "C" void rt_monitor_emit(int32_t desc, const char* fmt, uint32_t argc, const rt_arg* argv) {
    io_ensure_init();
    std::string key = monitor_key_image(argc, argv);
    if (g_monitor.primed && key == g_monitor.last) {
        return;
    }
    g_monitor.primed = true;
    g_monitor.last   = std::move(key);
    std::string out;
    render(out, fmt, argc, argv);
    write_raw(desc, out);
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
        // A numeric conversion into a `string` destination converts the value to a
        // string (numeric_to_string); its digits may outgrow the planes, which are
        // sized to the packed targets, so they grow as the digits arrive.
        const bool str_dest   = !suppress && ai < argc && (args[ai].flags & 2u) != 0;
        uint32_t   digit_bits = 0;
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
                if (str_dest && (must[words - 1] >> 60) != 0) {
                    // Keeps a free top bit above the magnitude for the sign.
                    ++words;
                    must.resize(words);
                    may.resize(words);
                    z.resize(words);
                }
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
            digit_bits = used * static_cast<uint32_t>(bits);
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
                // A `string` object destination (LRM 21.3.4.3): %s assigns the token
                // verbatim, with no width cap, right-justification or padding; any
                // other conversion stores its value cast to string.
                assert(a.str != nullptr && "a string destination carries its handle");
                if (conv == 's') {
                    assert(!tok.empty() && "an empty %s token is an input failure, not a store");
                    rt_string_assign_bytes(a.str, reinterpret_cast<const char*>(tok.data()), tok.size(), a.site);
                } else {
                    const std::string s = numeric_to_string(must.data(), words, conv == 'd', digit_bits);
                    rt_string_assign_bytes(a.str, s.data(), s.size(), a.site);
                }
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

// Destination geometry for a bulk memory load, shared by $readmem and $fread: the
// `count` leaves of the words that exist, each `width` bits in a slot of `stride`
// bytes whose must|may word is at the slot's start (`mm_base` + its offset) and whose
// z word is at `z_base` + the same offset. A 2-state memory (four_state=false) has no
// z plane and stores x/z as 0. A scalar target is just count == 1.
struct MemDest {
    uint8_t* mm_base;
    uint8_t* z_base;
    uint32_t stride;
    int64_t  count;
    uint32_t width;
    bool     four_state;
    // A narrow 2-state memory stores the bare iW value per element (no packed
    // must|may word, no z).
    bool narrow = false;
    // The words of a queue sit in its ring (`cap` != 0): the leaves come `leaves` to a
    // word, and the word w places after the first that exists is at ring slot
    // `head` + w, less `cap` when that reaches it, counted from the plane bases.
    // Otherwise the leaves are contiguous from the bases.
    uint64_t leaves = 1;
    uint64_t head   = 0;
    uint64_t cap    = 0;
};

// Byte offset from the plane bases of leaf `idx`, counted from the first leaf that
// exists. One subtraction wraps the ring, as in rt_queue_slot: a word that exists is
// one of the queue's `len` <= `cap` elements and `head` < `cap`.
size_t leaf_offset(const MemDest& d, int64_t idx) {
    const auto i = static_cast<uint64_t>(idx);
    if (d.cap == 0) {
        return i * d.stride;
    }
    uint64_t slot = d.head + i / d.leaves;
    if (slot >= d.cap) {
        slot -= d.cap;
    }
    return (slot * d.leaves + i % d.leaves) * d.stride;
}

// Write one element's value planes at storage index `idx` (bounds-checked). A null
// z_base (a never-z / z-elided memory) yields a null z pointer — not an offset from
// null — so write_word_planes skips the z write.
void mem_write(const MemDest& d, int64_t idx, uint64_t must, uint64_t may, uint64_t z) {
    if (idx < 0 || idx >= d.count) {
        return;
    }
    if (!d.four_state) {
        may = must;
        z   = 0;
    }
    const size_t off = leaf_offset(d, idx);
    uint8_t*     zp  = d.z_base != nullptr ? d.z_base + off : nullptr;
    write_word_planes(d.mm_base + off, zp, d.width, must, may, z, d.narrow);
}

// Wide (> 64-bit element) sibling of mem_write. The planes are little-endian word
// arrays (plane_words(d.width) words); write_word_planes_wide masks them in place.
void mem_write_wide(const MemDest& d, int64_t idx, uint64_t* must, uint64_t* may, uint64_t* z) {
    if (idx < 0 || idx >= d.count) {
        return;
    }
    const uint32_t words = plane_words(d.width);
    if (!d.four_state) {
        std::memcpy(may, must, words * sizeof(uint64_t));
        std::memset(z, 0, words * sizeof(uint64_t));
    }
    const size_t off = leaf_offset(d, idx);
    uint8_t*     zp  = d.z_base != nullptr ? d.z_base + off : nullptr;
    write_word_planes_wide(d.mm_base + off, zp, d.width, must, may, z, d.narrow);
}

// Map a declared-range address to a 0-based storage index: an ascending range
// counts up from decl_left, a descending range counts down from it.
int64_t norm_addr(int64_t decl_left, int64_t decl_right, int64_t addr) {
    return decl_left <= decl_right ? addr - decl_left : decl_left - addr;
}

// The leaves of one word of a memory's highest dimension, in the order a $readmem
// file and a $fread byte stream give them (LRM 21.4.3): row-major over the lower
// dimensions, each from its lowest address to its highest. `inner` holds those
// dimensions' declared (left, right) pairs, outermost first. Storage is row-major
// from each dimension's left bound, so a descending dimension is walked backward. A
// memory of one dimension has none, and one leaf per word.
class RowOrder {
public:
    RowOrder(const int64_t* inner, uint32_t dims) {
        int64_t stride = 1;
        for (uint32_t k = dims; k-- > 0;) {
            const int64_t left  = inner[2 * size_t{ k }];
            const int64_t right = inner[2 * size_t{ k } + 1];
            const int64_t size  = (left <= right ? right - left : left - right) + 1;
            const bool    down  = left > right;
            dims_.push_back(Dim{ size, down ? -stride : stride });
            if (down) {
                first_ += (size - 1) * stride;
            }
            stride *= size;
        }
        leaves_ = stride;
        digit_.assign(dims_.size(), 0);
        offset_ = first_;
    }

    // Leaves in one word.
    int64_t leaves() const {
        return leaves_;
    }

    // Storage offset of the current leaf from the first storage slot of its word.
    int64_t offset() const {
        return offset_;
    }

    // Step to the next leaf of the word. False after the last one, where the order is
    // back at the first leaf.
    bool next() {
        for (size_t j = 0; j < dims_.size(); ++j) {
            offset_ += dims_[j].step;
            if (++digit_[j] < dims_[j].size) {
                return true;
            }
            digit_[j] = 0;
            offset_ -= dims_[j].step * dims_[j].size;
        }
        return false;
    }

    // Back to the first leaf, abandoning a word that was not completed.
    void restart() {
        std::fill(digit_.begin(), digit_.end(), 0);
        offset_ = first_;
    }

private:
    struct Dim {
        int64_t size;
        // Storage offset of one address step up: negative for a descending dimension.
        int64_t step;
    };
    // Innermost first, the order the odometer carries in.
    std::vector<Dim>     dims_;
    std::vector<int64_t> digit_;
    int64_t              leaves_ = 1;
    // Offset of the leaf at every dimension's lowest address.
    int64_t first_  = 0;
    int64_t offset_ = 0;
};

// A bulk load fills a destination that the file loops below (load_words, read_words) see only as
// the range of its addresses, which of them have storage, and where the numbers go. Two are:
// the words of an unpacked array, and an associative array, whose element for an address is
// created by the first number loaded into it (LRM 21.4.1).
//
// `lo` / `hi` bound the addresses and `left` / `right` name that range in a report; `first` is
// where a load that names no start begins, `bounded` says `hi` ends a range that a file is meant
// to fill, and `words` is the number of addresses `lo` through `hi` hold. `open` selects the
// word at an address as the one `put` fills (false: it has no storage), `put` stores the planes
// of one of its leaves, and `changed` counts what the load altered.

// Store the planes of leaf `idx` of `d`.
void mem_put(const MemDest& d, int64_t idx, uint64_t* must, uint64_t* may, uint64_t* z) {
    if (d.width > 64) {
        mem_write_wide(d, idx, must, may, z);
    } else {
        mem_write(d, idx, must[0], may[0], z[0]);
    }
}

// The words of an unpacked array, or of the part of it that has storage.
class ArrayWords {
public:
    // `track` has `put` compare a leaf's slot before and after, for `changed`.
    ArrayWords(const MemDest& dest, int64_t decl_left, int64_t decl_right, int64_t lo_word, int64_t word_count,
               bool track)
      : dest_(dest)
      , left_(decl_left)
      , right_(decl_right)
      , lo_word_(lo_word)
      , word_count_(word_count)
      , before_(track ? dest.stride : 0) {}

    int64_t left() const {
        return left_;
    }
    int64_t right() const {
        return right_;
    }
    int64_t lo() const {
        return std::min(left_, right_);
    }
    int64_t hi() const {
        return std::max(left_, right_);
    }
    int64_t words() const {
        return hi() - lo() + 1;
    }
    // The default start is the lowest address of the memory (LRM 21.4).
    int64_t first() const {
        return lo();
    }
    bool bounded() const {
        return true;
    }

    bool open(int64_t addr) {
        row_ = norm_addr(left_, right_, addr);
        return row_ >= lo_word_ && row_ < lo_word_ + word_count_;
    }

    void put(int64_t leaf, uint64_t* must, uint64_t* may, uint64_t* z) {
        const int64_t idx = (row_ - lo_word_) * static_cast<int64_t>(dest_.leaves) + leaf;
        if (before_.empty()) {
            mem_put(dest_, idx, must, may, z);
            return;
        }
        // A leaf's slot holds both of its planes, so its `stride` bytes before and after
        // the write tell whether the value changed.
        uint8_t* const slot = dest_.mm_base + leaf_offset(dest_, idx);
        std::memcpy(before_.data(), slot, dest_.stride);
        mem_put(dest_, idx, must, may, z);
        if (std::memcmp(before_.data(), slot, dest_.stride) != 0) {
            ++changed_;
        }
    }

    uint32_t changed() const {
        return changed_;
    }

private:
    MemDest              dest_;
    int64_t              left_;
    int64_t              right_;
    int64_t              lo_word_;
    int64_t              word_count_;
    std::vector<uint8_t> before_;
    int64_t              row_     = 0;
    uint32_t             changed_ = 0;
};

// An associative array of integral index (LRM 21.4.1): every address of the index type names an
// element, which a load creates when it is not there. An element is `leaves` slots of `stride`
// bytes, the z plane `z_off` bytes into a slot (MemDest is the same layout for one element).
// An address is the index value, a 64-bit number: a signed index covers the numbers that fit
// its width and an unsigned one those from 0, so the addresses of a wider index stop at the
// 64-bit limits.
class AssocWords {
public:
    AssocWords(rt_assoc* h, const rt_elem_ops* ops, const rt_alloc_site* alloc, uint32_t key_width, bool key_signed,
               uint32_t stride, uint32_t width, uint32_t z_off, bool four_state, bool narrow, int64_t leaves,
               bool track)
      : h_(h)
      , ops_(ops)
      , alloc_(alloc)
      , key_width_(key_width)
      , key_signed_(key_signed)
      , z_off_(z_off)
      , dest_{ nullptr, nullptr, stride, leaves, width, four_state, narrow, static_cast<uint64_t>(leaves), 0, 0 }
      , key_((key_width + 7) / 8)
      , before_(track ? stride : 0)
      , num_before_(rt_assoc_num(h)) {}

    int64_t lo() const {
        if (!key_signed_) {
            return 0;
        }
        return key_width_ >= 64 ? INT64_MIN : -static_cast<int64_t>(uint64_t{ 1 } << (key_width_ - 1));
    }
    int64_t hi() const {
        if (key_signed_) {
            return key_width_ >= 64 ? INT64_MAX : static_cast<int64_t>((uint64_t{ 1 } << (key_width_ - 1)) - 1);
        }
        return key_width_ >= 63 ? INT64_MAX : static_cast<int64_t>((uint64_t{ 1 } << key_width_) - 1);
    }
    int64_t left() const {
        return lo();
    }
    int64_t right() const {
        return hi();
    }
    int64_t words() const {
        const uint64_t n = static_cast<uint64_t>(hi()) - static_cast<uint64_t>(lo()) + 1;
        return n == 0 || n > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n);
    }
    // The LRM names no default start for an array with no lowest address (note 63 of
    // docs/LRM_NOTES.md): a load that names none counts up from 0.
    int64_t first() const {
        return 0;
    }
    // The highest address is the largest index, not the end of a range to fill.
    bool bounded() const {
        return false;
    }

    // Creates the element at `addr`, if the index type has that address.
    bool open(int64_t addr) {
        if (addr < lo() || addr > hi()) {
            return false;
        }
        make_key(addr);
        auto* entry = static_cast<uint8_t*>(
          rt_assoc_put_ptr(h_, ops_, key_.data(), static_cast<uint32_t>(key_.size()), alloc_));
        dest_.mm_base = entry;
        dest_.z_base  = dest_.four_state ? entry + z_off_ : nullptr;
        return true;
    }

    void put(int64_t leaf, uint64_t* must, uint64_t* may, uint64_t* z) {
        if (before_.empty()) {
            mem_put(dest_, leaf, must, may, z);
            return;
        }
        uint8_t* const slot = dest_.mm_base + leaf_offset(dest_, leaf);
        std::memcpy(before_.data(), slot, dest_.stride);
        mem_put(dest_, leaf, must, may, z);
        if (std::memcmp(before_.data(), slot, dest_.stride) != 0) {
            ++changed_;
        }
    }

    // An element the load created counts as a change, whatever it was loaded with.
    uint32_t changed() const {
        return changed_ + static_cast<uint32_t>(rt_assoc_num(h_) - num_before_);
    }

private:
    // The key bytes of an index of value `addr`, as the backend normalizes one
    // (emit_key_bytes): its width in bits, big-endian, with the sign bit flipped for a signed
    // index so that the byte order is the numeric order.
    void make_key(int64_t addr) {
        const size_t n = key_.size();
        std::fill(key_.begin(), key_.end(), uint8_t{ 0 });
        const auto bits = static_cast<uint64_t>(addr);
        for (size_t k = 0; k < n && k < 8; ++k) {
            key_[n - 1 - k] = static_cast<uint8_t>(bits >> (8 * k));
        }
        if (key_signed_ && addr < 0) {
            for (size_t k = 8; k < n; ++k) {
                key_[n - 1 - k] = 0xff;
            }
        }
        if (key_width_ % 8 != 0) {
            key_[0] &= static_cast<uint8_t>((1u << (key_width_ % 8)) - 1);
        }
        if (key_signed_) {
            key_[0] ^= static_cast<uint8_t>(1u << ((key_width_ - 1) % 8));
        }
    }

    rt_assoc*            h_;
    const rt_elem_ops*   ops_;
    const rt_alloc_site* alloc_;
    uint32_t             key_width_;
    bool                 key_signed_;
    uint32_t             z_off_;
    MemDest              dest_;
    std::vector<uint8_t> key_;
    std::vector<uint8_t> before_;
    uint64_t             num_before_;
    uint32_t             changed_ = 0;
};

// What a $readmem names, beside the destination it fills.
struct LoadArgs {
    bool                hex;
    bool                has_start;
    bool                has_finish;
    int64_t             start;
    int64_t             finish;
    uint32_t            width;
    const std::string&  task;
    const std::string&  path;
    const rt_null_site* site;
};

// The bytes of the file at `path`; false, with a report, when it cannot be opened.
bool read_text_file(const std::string& path, std::vector<char>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "$readmem: cannot open file '%s'\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::rewind(f);
    out.resize(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (!out.empty()) {
        out.resize(std::fread(out.data(), 1, out.size(), f));
    }
    std::fclose(f);
    return true;
}

// The numbers of a $readmemh / $readmemb file, loaded into `sink` (LRM 21.4). `order` walks the
// leaves of one of its words. The load can end early, at an address of the file outside the
// range the task names (an error) or at a number past its end (a warning); what it loaded stays.
template<typename Sink>
void load_words(Sink& sink, RowOrder& order, const std::vector<char>& buf, const LoadArgs& a) {
    const std::string& task = a.task;
    const std::string& path = a.path;

    // Iteration runs in declared-address space, not storage-index space (LRM
    // §21.4). The default start is the *lowest* address and loading proceeds upward
    // toward the highest, regardless of the range direction; a start-only form also
    // goes upward; a start>finish pair decrements. The direction persists across any
    // @addr directive in the file.
    const int64_t lo_addr = sink.lo();
    const int64_t hi_addr = sink.hi();
    int64_t       addr    = a.has_start ? a.start : sink.first();
    const int64_t dir     = (a.has_finish && a.finish < a.start) ? -1 : 1;
    // Loading stops at the finish address (if given) else the highest address; data
    // beyond that bound is ignored with a warning (LRM §21.4).
    const int64_t end = a.has_finish ? a.finish : hi_addr;
    // The range the task names, which an `@` address of the file has to lie in (LRM 21.4). A
    // start alone loads upward to the highest address, so one above it names no address.
    const int64_t range_lo = a.has_finish ? std::min(a.start, a.finish) : a.has_start ? a.start : lo_addr;
    const int64_t range_hi = a.has_finish ? std::max(a.start, a.finish) : hi_addr;
    // Its ends in the order of travel, as the reports name it.
    const int64_t range_a = a.has_start ? a.start : lo_addr;

    const int      bits   = a.hex ? 4 : 1;
    const uint64_t nibble = a.hex ? 0xFu : 0x1u;

    // Value planes accumulate into word arrays; a <= 64-bit element uses one word
    // and reduces to the scalar (v << bits) | d path, a wider element assembles
    // multi-word. Allocated once and reused per token.
    const uint32_t        words = plane_words(a.width);
    std::vector<uint64_t> must(words);
    std::vector<uint64_t> may(words);
    std::vector<uint64_t> z(words);

    // The numbers the range the task names holds, one for each leaf of each of its words. A
    // file of another count is warned of, unless it gives addresses of its own (LRM 21.4). An
    // array with no end to fill, an associative array loaded from a start alone, has none.
    const int64_t  leaves = order.leaves();
    const __int128 range_words =
      dir > 0 ? static_cast<__int128>(end) - addr + 1 : static_cast<__int128>(addr) - end + 1;
    const __int128 expected = (range_words > 0 ? range_words : 0) * leaves;
    const bool     counted  = a.has_finish || sink.bounded();

    // The word the numbers are filling, once its first has arrived. `row_ok` says it
    // names storage: an address outside the memory is reported once for its word, and
    // the numbers that fall in it are dropped.
    bool in_row = false;
    bool row_ok = false;
    // The numbers of the file seen, whether it gives an address of its own, and whether an
    // error or the end of the range cut it short.
    int64_t numbers = 0;
    bool    saw_at  = false;
    bool    cut     = false;

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
            uint64_t at_bits = 0;
            for (int d; i < n && (d = hex_digit(buf[i])) >= 0; ++i) {
                at_bits = at_bits * 16 + static_cast<uint64_t>(d);
            }
            saw_at = true;
            // The address has to lie in the range the task names (LRM 21.4); one outside it
            // ends the load, and what was loaded before it stays.
            const auto at = static_cast<int64_t>(at_bits);
            if (at < range_lo || at > range_hi) {
                mem_load_report(a.site, true,
                                fmt::format("{}: address {} in '{}' is outside the address range [{}:{}] of the load",
                                            task, at, path, range_a, end),
                                "the load stopped there, and the words before it are loaded");
                cut = true;
                break;
            }
            // An address names a whole word of the highest dimension, so a word that
            // was only partly filled keeps its other leaves (LRM 21.4.3).
            addr   = at;
            in_row = false;
            order.restart();
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
                const int hv = a.hex ? hex_digit(d) : bin_digit(d);
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
        ++numbers;
        if ((dir > 0 && addr > end) || (dir < 0 && addr < end)) {
            // Past the finish / highest address: the remaining file data is dropped.
            mem_load_report(a.site, false,
                            fmt::format("{}: '{}' holds more words than the range [{}:{}]", task, path, range_a, end),
                            "the extra words are ignored");
            cut = true;
            break;
        }
        if (!in_row) {
            row_ok = sink.open(addr);
            in_row = true;
            if (!row_ok) {
                mem_load_report(a.site, false,
                                fmt::format("{}: address {} in '{}' names no element of the array", task, addr, path),
                                "the words at it are dropped");
            }
        }
        if (row_ok) {
            sink.put(order.offset(), must.data(), may.data(), z.data());
        }
        if (!order.next()) {
            in_row = false;
            addr += dir;
        }
    }
    if (!cut && !saw_at && counted && numbers < expected) {
        mem_load_report(a.site, false,
                        fmt::format("{}: '{}' holds fewer words than the range [{}:{}]", task, path, range_a, end),
                        "the other words keep their value");
    }
}

// The bytes of `f`, loaded into `sink` as $fread does (LRM 21.3.4.4): from `start` (default
// the first address), at most `max_count` words and never past the highest address. Returns the
// number of bytes read.
template<typename Sink>
int32_t read_words(Sink&               sink,
                   RowOrder&           order,
                   std::FILE*          f,
                   bool                has_start,
                   bool                has_count,
                   int64_t             start,
                   int64_t             max_count,
                   uint32_t            width,
                   const rt_null_site* site,
                   uint64_t            now) {
    const uint32_t elem_bytes = (width + 7) / 8;
    const int64_t  hi         = sink.hi();
    int64_t        addr       = has_start ? start : sink.first();
    int64_t        remaining  = has_count ? max_count : sink.words();
    if (addr < sink.lo() || addr > hi) {
        // The file position stays where it was: no byte is consumed for an address
        // that names no element.
        rt_fread_range_report(site, addr, sink.left(), sink.right(), now);
        return 0;
    }

    // Bytes accumulate into a word array; a <= 64-bit element uses one word and
    // reduces to the scalar (val << 8) | byte path.
    const uint32_t        words = plane_words(width);
    std::vector<uint64_t> val(words);
    std::vector<uint64_t> may(words);
    std::vector<uint64_t> z(words, 0);

    int32_t bytes_read = 0;
    bool    eof        = false;
    while (!eof && addr <= hi && remaining > 0) {
        // The word is selected when its first byte has arrived, so that an array that is
        // not there does not get an element for the end of the file.
        bool opened = false;
        bool row_ok = false;
        do {
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
                eof = true; // EOF before this leaf
                break;
            }
            // MSB-first: a short final leaf left-shifts so the bytes read occupy the
            // high end and the missing low bytes stay 0.
            wide_shl(val.data(), words, 8u * (elem_bytes - nb));
            if (!opened) {
                row_ok = sink.open(addr);
                opened = true;
            }
            if (row_ok) {
                // File bytes are all known (must == may, no z); the 2-state override of
                // the destination handles a narrow memory.
                std::copy(val.begin(), val.end(), may.begin());
                std::fill(z.begin(), z.end(), 0);
                sink.put(order.offset(), val.data(), may.data(), z.data());
            }
            if (nb < elem_bytes) {
                eof = true; // hit EOF mid-leaf
                break;
            }
        } while (order.next());
        ++addr;
        --remaining;
    }
    return bytes_read;
}

} // namespace

// $readmemh / $readmemb (LRM §21.4). Loads a hex/binary text file into an unpacked
// array. The destination planes live in the JIT state buffer: leaf i's must|may word
// is at mm_base + i*stride, its z word at z_base + i*stride (the backend passes the
// two region bases, already offset to leaf 0). Each parsed token is assembled into
// u64 must / may / z accumulators (element width <= 64, enforced by the backend) and
// written through mem_write — no per-element allocation. Returns the number of
// leaves whose value the load changed, so the caller wakes the memory's waiters only
// when it did.
//
// A memory of several unpacked dimensions is a run of words of its highest dimension,
// each a row of leaves (LRM 21.4.3): `decl_left` / `decl_right` are that dimension's
// declared bounds, `inner` / `inner_dims` the (left, right) pairs of the others. The
// file's numbers fill a word's leaves row-major, each dimension from its lowest
// address to its highest, and start, finish and @ addresses name whole words.
//
// Only the words at positions [lo_word, lo_word + word_count) of that dimension,
// counted from `decl_left`, have storage, and `mm_base` / `z_base` point at the first
// of them. That is all of them for a memory, and for a slice a part of it can hang off
// the array or the whole of it be invalid (LRM 7.4.5): a word with no storage is a
// word the file's numbers are dropped from, and a load that names no word does
// nothing, not even open the file.
//
// The addresses of the file lie in the range the task names (LRM 21.4): `start` through
// `finish`, `start` through the highest address of the array when only `start` is given,
// else all of the array. An `@` address outside it is an error that ends the load, and the
// words before it stay loaded. For a slice (`flags` bit6) `start` and `finish` themselves
// have to lie in the slice's range, or nothing is loaded.
//
// The words of a queue sit in its ring rather than one after another: a nonzero
// `ring_cap` makes `mm_base` / `z_base` point at the ring's slot 0 and the first word
// that exists lie at slot `ring_head`, the next at the slot after it, and so on
// around the ring (rt_queue_slot).
extern "C" uint32_t rt_readmem(const char* path_buf, uint32_t path_len, uint8_t* mm_base, uint8_t* z_base,
                               uint32_t stride, uint32_t width, const int64_t* inner, uint32_t inner_dims,
                               int64_t decl_left, int64_t decl_right, int64_t lo_word, int64_t word_count,
                               int64_t ring_head, int64_t ring_cap, int64_t start, int64_t finish,
                               uint32_t flags, const rt_null_site* site) {
    const bool hex          = (flags & 1u) != 0;
    const bool has_start    = (flags & 2u) != 0;
    const bool has_finish   = (flags & 4u) != 0;
    const bool four_state   = (flags & 8u) != 0;
    const bool path_forward = (flags & 16u) != 0;
    // A narrow 2-state memory stores a single iW element (no packed must|may word).
    const bool narrow       = (flags & 32u) != 0;
    const bool slice        = (flags & 64u) != 0;
    if (word_count <= 0) {
        return 0;
    }
    const std::string task = hex ? "$readmemh" : "$readmemb";

    // LRM 21.4: with slice syntax, start and finish "shall fall within the bounds of the
    // slice's range". An address outside it names nothing to load, so nothing is loaded. A whole
    // memory takes the same rule by analogy (docs/LRM_NOTES.md note 66; iverilog 13 agrees).
    {
        const int64_t slice_lo = std::min(decl_left, decl_right);
        const int64_t slice_hi = std::max(decl_left, decl_right);
        const auto    outside  = [&](bool given, int64_t at, const char* which) {
            if (!given || (at >= slice_lo && at <= slice_hi)) {
                return false;
            }
            mem_load_report(site, true,
                            fmt::format("{}: {} address {} is outside the {} range [{}:{}]", task, which, at,
                                        slice ? "slice" : "memory", decl_left, decl_right),
                            "nothing was loaded");
            return true;
        };
        if (outside(has_start, start, "start") || outside(has_finish, finish, "finish")) {
            return 0;
        }
    }

    // A string-object filename (path_forward) arrives as its bytes in natural
    // order; otherwise it is a right-justified packed-ASCII vector (byte 0 = last
    // character, high bytes zero padding), the same layout $fopen receives.
    const std::string path = path_forward ? std::string(path_buf, path_len) : unpack_ascii(path_buf, path_len);
    std::vector<char> buf;
    if (!read_text_file(path, buf)) {
        return 0;
    }

    // The highest dimension is `word_count` words that exist, of `order.leaves()` leaves
    // each, and the leaves of the word at position p start at storage leaf
    // (p - lo_word) * leaves.
    RowOrder      order(inner, inner_dims);
    const int64_t leaves = order.leaves();
    const MemDest dest{ mm_base,
                        z_base,
                        stride,
                        word_count * leaves,
                        width,
                        four_state,
                        narrow,
                        static_cast<uint64_t>(leaves),
                        static_cast<uint64_t>(ring_head),
                        static_cast<uint64_t>(ring_cap) };
    ArrayWords    sink(dest, decl_left, decl_right, lo_word, word_count, /*track=*/true);
    load_words(sink, order, buf, LoadArgs{ hex, has_start, has_finish, start, finish, width, task, path, site });
    return sink.changed();
}

extern "C" int32_t rt_fread(int32_t fd, uint8_t* mm_base, uint8_t* z_base, uint32_t stride, uint32_t width,
                            uint32_t four_state, const int64_t* inner, uint32_t inner_dims, int64_t decl_left,
                            int64_t decl_right, int64_t lo_word, int64_t word_count, int64_t ring_head,
                            int64_t ring_cap, int64_t start, int64_t max_count, uint32_t flags,
                            const rt_null_site* site, uint64_t now) {
    // $fread (LRM §21.3.4): read raw bytes from `fd` into a memory (or a scalar, a
    // memory of one word), MSB-first per leaf. Fills from the `start` declared address
    // (default the lowest) toward the highest, at most `max_count` words and never
    // past the highest address. An address names a word of the memory's highest
    // dimension, a row of leaves for a memory of several, filled row-major (LRM
    // 21.4.3 gives that order, and `count` is in the same words). Shares the memory
    // destination substrate with $readmem, including which words have storage (see
    // rt_readmem): the bytes of a word with none are read and dropped, and a target
    // that names no word reads nothing. Only the byte source differs. Returns the
    // number of bytes read. A `start` outside the declared range reads nothing,
    // returns 0 and warns (docs/LRM_NOTES.md 55); `site` / `now` locate that warning.
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    if (f == nullptr || word_count <= 0) {
        return 0;
    }
    const bool has_start = (flags & 1u) != 0;
    const bool has_count = (flags & 2u) != 0;
    const bool narrow    = (flags & 4u) != 0;

    RowOrder      order(inner, inner_dims);
    const int64_t leaves = order.leaves();
    const MemDest dest{ mm_base,
                        z_base,
                        stride,
                        word_count * leaves,
                        width,
                        four_state != 0,
                        narrow,
                        static_cast<uint64_t>(leaves),
                        static_cast<uint64_t>(ring_head),
                        static_cast<uint64_t>(ring_cap) };
    ArrayWords    sink(dest, decl_left, decl_right, lo_word, word_count, /*track=*/false);
    return read_words(sink, order, f, has_start, has_count, start, max_count, width, site, now);
}

// $readmemh / $readmemb into an associative array of integral index (LRM 21.4.1): loading an
// address creates the element of that index when it is not there. `handle` / `ops` are the array
// and its descriptor; `stride`, `width`, `inner` / `inner_dims` and the flags describe one
// element as rt_readmem describes a memory's leaves, the z plane `z_off` bytes into a slot, and
// a number is loaded into the leaf of the element at its address, row-major (LRM 21.4.3).
// `key_width` is the index's width. `flags` bit0=hex, bit1=has_start, bit2=has_finish,
// bit3=four_state, bit4=path_forward, bit5=narrow, bit7=signed index, bit8=names no array (a
// target that is invalid at run time: nothing is loaded, as for a word count of 0 in rt_readmem).
// A load that names no start begins at address 0 (docs/LRM_NOTES.md 63); the other rules
// of rt_readmem hold, the addresses being those of the index type. `alloc` locates the
// allocation of an element and `site` the load's reports. Returns the number of leaves the load
// changed, an element it created counting as one.
extern "C" uint32_t rt_readmem_assoc(const char* path_buf, uint32_t path_len, rt_assoc* handle,
                                     const rt_elem_ops* ops, uint32_t key_width, uint32_t stride, uint32_t width,
                                     uint32_t z_off, const int64_t* inner, uint32_t inner_dims, int64_t start,
                                     int64_t finish, uint32_t flags, const rt_alloc_site* alloc,
                                     const rt_null_site* site) {
    const bool hex          = (flags & 1u) != 0;
    const bool has_start    = (flags & 2u) != 0;
    const bool has_finish   = (flags & 4u) != 0;
    const bool four_state   = (flags & 8u) != 0;
    const bool path_forward = (flags & 16u) != 0;
    const bool narrow       = (flags & 32u) != 0;
    const bool key_signed   = (flags & 128u) != 0;
    if ((flags & 256u) != 0) {
        return 0;
    }
    const std::string task = hex ? "$readmemh" : "$readmemb";
    const std::string path = path_forward ? std::string(path_buf, path_len) : unpack_ascii(path_buf, path_len);
    std::vector<char> buf;
    if (!read_text_file(path, buf)) {
        return 0;
    }

    RowOrder   order(inner, inner_dims);
    AssocWords sink(handle, ops, alloc, key_width, key_signed, stride, width, z_off, four_state, narrow,
                    order.leaves(), /*track=*/true);
    load_words(sink, order, buf, LoadArgs{ hex, has_start, has_finish, start, finish, width, task, path, site });
    return sink.changed();
}

// $fread into an associative array of integral index, as rt_readmem_assoc loads one: from the
// element at `start` (default 0), at most `max_count` of them, each the bytes of its leaves
// row-major. `flags` bit0=has_start, bit1=has_count, bit2=narrow, bit3=four_state, bit4=signed
// index, bit5=names no array. Returns the number of bytes read; a `start` the index type does
// not have reads nothing and warns, as for a memory.
extern "C" int32_t rt_fread_assoc(int32_t fd, rt_assoc* handle, const rt_elem_ops* ops, uint32_t key_width,
                                  uint32_t stride, uint32_t width, uint32_t z_off, const int64_t* inner,
                                  uint32_t inner_dims, int64_t start, int64_t max_count, uint32_t flags,
                                  const rt_alloc_site* alloc, const rt_null_site* site) {
    io_ensure_init();
    std::FILE* f = fd_stream(fd);
    if (f == nullptr || (flags & 32u) != 0) {
        return 0;
    }
    const bool has_start  = (flags & 1u) != 0;
    const bool has_count  = (flags & 2u) != 0;
    const bool narrow     = (flags & 4u) != 0;
    const bool four_state = (flags & 8u) != 0;
    const bool key_signed = (flags & 16u) != 0;

    RowOrder   order(inner, inner_dims);
    AssocWords sink(handle, ops, alloc, key_width, key_signed, stride, width, z_off, four_state, narrow,
                    order.leaves(), /*track=*/false);
    return read_words(sink, order, f, has_start, has_count, start, max_count, width, site, rt_diag_now());
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

extern "C" int32_t rt_value_plusargs_str(const char* prefix, uint32_t prefix_len, uint32_t conv, rt_string* dst,
                                         const rt_alloc_site* site) {
    const std::string* pa = find_plusarg(std::string_view(prefix, prefix_len));
    if (pa == nullptr) {
        return 0; // LRM §21.6: no match leaves the variable alone and returns 0.
    }
    assert(pa->size() >= prefix_len);
    const std::string_view rem(pa->data() + prefix_len, pa->size() - prefix_len);
    const char             c = static_cast<char>(conv);
    if (c == 's') {
        // "%s -- string (no conversion)": the remainder IS the value, verbatim. No
        // right-justification and no width cap, unlike the packed-vector form above --
        // a `string` grows to whatever the command line supplied. An empty remainder
        // stores the empty string, which is the clause's stated result.
        rt_string_assign_bytes(dst, rem.data(), rem.size(), site);
        return 1;
    }
    // A numeric conversion stores the converted value cast to string
    // (numeric_to_string); the planes hold every digit, with a word to spare.
    const uint32_t        digit_bits = (c == 'o' ? 3u : c == 'b' ? 1u : 4u);
    const auto            words      = static_cast<uint32_t>(rem.size() * digit_bits / 64) + 2;
    std::vector<uint64_t> must(words);
    std::vector<uint64_t> may(words);
    value_plusargs_numeric(rem, c, must.data(), may.data(), words);
    const std::string s =
      numeric_to_string(must.data(), words, c == 'd', static_cast<uint32_t>(rem.size()) * digit_bits);
    rt_string_assign_bytes(dst, s.data(), s.size(), site);
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
