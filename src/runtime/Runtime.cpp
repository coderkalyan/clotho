#include "Runtime.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

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

// Render a tick-valued time `ticks` per the current $timeformat, appending to
// `out` (LRM §20.4.3). The tick value scales to femtoseconds, then to the
// display unit (10^units s); since fs = 10^-15 s, a display unit of 10^units s
// is 10^(units+15) fs. The number is printed with `precision` fractional digits,
// the suffix is appended, and the whole field is right-justified to min_width.
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

    std::string field = numbuf;
    field += timeformat.suffix;
    if (timeformat.min_width > 0 && field.size() < static_cast<size_t>(timeformat.min_width)) {
        out.append(static_cast<size_t>(timeformat.min_width) - field.size(), ' ');
    }
    out += field;
}

// -- $display formatting --

// Low-`w`-bits mask (all ones for w >= 64).
uint64_t width_mask(uint32_t w) {
    return w >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << w) - 1);
}

// Render `val` as ceil(width/bits) digits, most-significant first. A digit prints
// as 'z' if all of its unknown bits are high-impedance, 'x' if any are X, else
// its hex/octal/binary value. `bits` is 1 (binary), 3 (octal) or 4 (hex).
void render_digits(std::string& out, uint64_t val, uint64_t xz, uint64_t z, uint32_t width, uint32_t bits) {
    if (width == 0) {
        width = 1;
    }
    static constexpr char kHex[]  = "0123456789abcdef";
    const uint64_t        dig_mask = (uint64_t{ 1 } << bits) - 1;
    const uint32_t        digits   = (width + bits - 1) / bits;
    for (uint32_t d = digits; d-- > 0;) {
        const uint32_t shift = d * bits;
        const uint64_t dxz   = (xz >> shift) & dig_mask;
        if (dxz != 0) {
            const uint64_t dz = (z >> shift) & dig_mask;
            out += (dxz & ~dz) ? 'x' : 'z';
        } else {
            out += kHex[(val >> shift) & dig_mask];
        }
    }
}

// Render one descriptor under conversion `conv`, appending to `out`.
void render_arg(std::string& out, const rt_arg& a, char conv) {
    const uint32_t w    = a.width ? a.width : 1;
    const uint64_t mask = width_mask(w);
    const uint64_t val  = a.val & mask;
    const uint64_t xz   = a.xz & mask;
    const uint64_t z    = a.z & mask;

    switch (conv) {
        case 'd':
        case 'D':
            if (xz != 0) { // any unknown bit collapses the whole field
                out += (xz & ~z) ? 'x' : 'z';
            } else if ((a.flags & 1u) != 0) {
                const uint64_t sext = (w < 64 && ((val >> (w - 1)) & 1u)) ? (val | ~mask) : val;
                out += std::to_string(static_cast<int64_t>(sext));
            } else {
                out += std::to_string(val);
            }
            return;
        case 'h':
        case 'H':
        case 'x':
        case 'X':
            render_digits(out, val, xz, z, w, 4);
            return;
        case 'o':
        case 'O':
            render_digits(out, val, xz, z, w, 3);
            return;
        case 'b':
        case 'B':
            render_digits(out, val, xz, z, w, 1);
            return;
        case 'c':
        case 'C':
            out += static_cast<char>(val & 0xff);
            return;
        case 't':
        case 'T':
            // The frontend scales a %t argument to design ticks, so `val` is the
            // tick count; render_time converts it per the current $timeformat.
            render_time(out, val);
            return;
        default:
            // TODO: %s / %m / %f are consumed but emit nothing, so the field
            // silently vanishes. LRM expects string / hierarchical-path / real
            // output respectively.
            return;
    }
}

// Format `fmt` with the argument descriptors, appending to `out`. Shared by every
// $write-family task ($display / $write / $fdisplay / $fwrite differ only in the
// destination and the folded-in newline, both handled by the caller). Text is
// emitted verbatim; a `%` begins a specifier whose flags / width / precision are
// skipped (the field is auto-sized) and whose conversion char selects render_arg.
void render(std::string& out, const char* fmt, uint32_t argc, const rt_arg* argv) {
    uint32_t ai = 0;
    for (const char* p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        ++p;
        if (*p == '\0') {
            out += '%';
            break;
        }
        if (*p == '%') {
            out += '%';
            continue;
        }
        // Skip flags and an optional width/precision; the field is auto-sized.
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0') {
            ++p;
        }
        while ((*p >= '0' && *p <= '9') || *p == '.') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        if (ai < argc) {
            render_arg(out, argv[ai++], *p);
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
// backend guards it), so the must / may plane halves stay byte-aligned. Shared by
// $fgets, $sformat / $swrite, and $ferror.
void store_string_planes(uint8_t* mm, uint8_t* z, uint32_t width, const uint8_t* bytes, uint32_t count) {
    const uint32_t capacity = width / 8;
    std::memset(mm, 0, static_cast<size_t>(capacity) * 2);
    std::memset(z, 0, capacity);
    const uint32_t keep = count < capacity ? count : capacity;
    for (uint32_t k = 0; k < keep; ++k) {
        const uint8_t byte = bytes[count - 1 - k]; // last byte read lands in mm[0]
        mm[k]              = byte;                  // must half (low `width` bits)
        mm[capacity + k]   = byte;                  // may half (high `width` bits)
    }
}

// Write a ≤64-bit value's planes into a packed vector: the must|may word (must in
// the low `width` bits, may above) at `mm`, plus the z word at `z`. Values are
// masked to `width`. Shared by $readmem and the $fscanf / $sscanf engine.
void write_word_planes(uint8_t* mm, uint8_t* z, uint32_t width, uint64_t must, uint64_t may, uint64_t zz) {
    const uint64_t wmask = width >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << width) - 1);
    must &= wmask;
    may &= wmask;
    zz &= wmask;
    const unsigned __int128 mmv = static_cast<unsigned __int128>(must) |
                                  (static_cast<unsigned __int128>(may) << width);
    std::memcpy(mm, &mmv, (2 * width + 7) / 8);
    std::memcpy(z, &zz, (width + 7) / 8);
}

clotho::Timer& unwrap(rt_timer* t) {
    return *reinterpret_cast<clotho::Timer*>(t);
}

// One deferred unpacked-array element write: a bytewise masked merge of `mm` /
// `z` value bytes into the live element's must|may and z words. Self-describing
// (absolute destination pointers captured at push time) so the commit pass is
// width-agnostic.
struct NbaArrWrite {
    uint32_t             sig;
    void*                mm_ptr;
    void*                z_ptr;
    std::vector<uint8_t> mm_val;
    std::vector<uint8_t> mm_mask;
    std::vector<uint8_t> z_val;
    std::vector<uint8_t> z_mask;
};

// The single deferred-write queue. Cleared (kept allocated) each NBA pass.
std::vector<NbaArrWrite> nba_arr_queue;

// Bytewise dest = (dest & ~mask) | (val & mask).
void masked_merge(void* dest, const std::vector<uint8_t>& val, const std::vector<uint8_t>& mask) {
    auto* d = static_cast<uint8_t*>(dest);
    for (size_t k = 0; k < val.size(); ++k) {
        d[k] = static_cast<uint8_t>((d[k] & ~mask[k]) | (val[k] & mask[k]));
    }
}

} // namespace

extern "C" void rt_nba_arr_push(uint32_t sig, void* mm_ptr, const void* mm_val, const void* mm_mask,
                                uint32_t mm_bytes, void* z_ptr, const void* z_val, const void* z_mask,
                                uint32_t z_bytes) {
    const auto* mv = static_cast<const uint8_t*>(mm_val);
    const auto* mk = static_cast<const uint8_t*>(mm_mask);
    const auto* zv = static_cast<const uint8_t*>(z_val);
    const auto* zk = static_cast<const uint8_t*>(z_mask);
    nba_arr_queue.push_back(NbaArrWrite{ .sig     = sig,
                                         .mm_ptr  = mm_ptr,
                                         .z_ptr   = z_ptr,
                                         .mm_val  = { mv, mv + mm_bytes },
                                         .mm_mask = { mk, mk + mm_bytes },
                                         .z_val   = { zv, zv + z_bytes },
                                         .z_mask  = { zk, zk + z_bytes } });
}

extern "C" uint32_t rt_nba_arr_pending(void) {
    return static_cast<uint32_t>(nba_arr_queue.size());
}

extern "C" void rt_nba_arr_commit(void) {
    for (const NbaArrWrite& w : nba_arr_queue) {
        masked_merge(w.mm_ptr, w.mm_val, w.mm_mask);
        masked_merge(w.z_ptr, w.z_val, w.z_mask);
    }
}

extern "C" int rt_nba_arr_dirty(uint32_t sig) {
    for (const NbaArrWrite& w : nba_arr_queue) {
        if (w.sig == sig) {
            return 1;
        }
    }
    return 0;
}

extern "C" void rt_nba_arr_reset(void) {
    nba_arr_queue.clear();
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
    // rt_timer* and clotho::Timer& share a representation, so reinterpreting the
    // callback's first parameter is sound — the same pointer-cast the wheel
    // already relies on for its type-erased dispatch.
    unwrap(wheel)->schedule(unwrap(timer), deadline, reinterpret_cast<RuntimeWheel::Callback>(on_expire));
}

extern "C" void rt_wheel_tick(rt_wheel* wheel, void* ctx) {
    unwrap(wheel)->tick(ctx);
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

extern "C" int32_t rt_fgets(uint8_t* mm, uint8_t* z, uint32_t width, int32_t fd) {
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
    store_string_planes(mm, z, width, line.data(), count);
    return static_cast<int32_t>(count);
}

extern "C" void
rt_sformat(uint8_t* mm, uint8_t* z, uint32_t width, const char* fmt, uint32_t argc, const rt_arg* argv) {
    // $sformat / $swrite: render like $display, then store the result into the
    // destination string variable (LRM §21.3.3). An oversized result keeps its low
    // (rightmost) bytes, matching a string-to-narrower-reg assignment.
    std::string out;
    render(out, fmt, argc, argv);
    store_string_planes(mm, z, width, reinterpret_cast<const uint8_t*>(out.data()),
                        static_cast<uint32_t>(out.size()));
}

extern "C" int32_t rt_ferror(int32_t fd, uint8_t* mm, uint8_t* z, uint32_t width) {
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
                        static_cast<uint32_t>(std::strlen(msg)));
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

        uint64_t must = 0;
        uint64_t may  = 0;
        uint64_t z    = 0;
        bool     ok   = false;

        if (conv == 'c') {
            const int c = src.get();
            if (c != EOF) {
                any_input = true;
                must = may = static_cast<uint8_t>(c);
                ok         = true;
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
                // Right-justify the token bytes into `must` (byte 0 = last char),
                // capped at 8 bytes; wider %s targets are guarded by the backend.
                const size_t keep = tok.size() < 8 ? tok.size() : 8;
                for (size_t k = 0; k < keep; ++k) {
                    must |= static_cast<uint64_t>(tok[tok.size() - 1 - k]) << (8 * k);
                }
                may = must;
                ok  = true;
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
                    must = must * 10 + dv;
                } else {
                    must = (must << bits) | dv;
                    may  = (may << bits) | mv;
                    z    = (z << bits) | zz;
                }
                ok        = true;
                any_input = true;
                ++used;
                c = src.get();
            }
            src.unget(c);
            if (ok && base == 10) {
                if (neg) {
                    must = static_cast<uint64_t>(-static_cast<int64_t>(must));
                }
                may = must; // decimal values carry no x/z
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
            write_word_planes(a.mm, a.z, a.width, must, may, z);
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

extern "C" void rt_finish(void) {
    // $finish / $stop: end the simulation and return control to the OS. std::exit
    // runs atexit handlers and flushes C stdio, so any buffered $display output is
    // committed before the process leaves.
    std::exit(0);
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
};

// Write one element's value planes at storage index `idx` (bounds-checked).
void mem_write(const MemDest& d, int64_t idx, uint64_t must, uint64_t may, uint64_t z) {
    if (idx < 0 || idx >= static_cast<int64_t>(d.count)) {
        return;
    }
    if (!d.four_state) {
        may = must;
        z   = 0;
    }
    write_word_planes(d.mm_base + static_cast<size_t>(idx) * d.stride,
                      d.z_base + static_cast<size_t>(idx) * d.stride, d.width, must, may, z);
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
    const bool hex        = (flags & 1u) != 0;
    const bool has_start  = (flags & 2u) != 0;
    const bool has_finish = (flags & 4u) != 0;
    const bool four_state = (flags & 8u) != 0;

    // The filename value is a right-justified packed-ASCII vector (byte 0 = last
    // character, high bytes zero padding), the same layout $fopen receives.
    const std::string path = unpack_ascii(path_buf, path_len);

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

    const MemDest dest{ mm_base, z_base, stride, count, width, four_state };
    const auto    write_elem = [&](int64_t idx, uint64_t must, uint64_t may, uint64_t z) {
        if (idx < 0 || idx >= static_cast<int64_t>(count)) {
            std::fprintf(stderr, "$readmem: address out of range for '%s'\n", path.c_str());
            return;
        }
        mem_write(dest, idx, must, may, z);
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
        uint64_t must = 0;
        uint64_t may  = 0;
        uint64_t z    = 0;
        bool     any  = false;
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
            must = (must << bits) | dv;
            may  = (may << bits) | mv;
            z    = (z << bits) | zv;
            any  = true;
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
        write_elem(norm_addr(decl_left, decl_right, addr), must, may, z);
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
    const MemDest  dest{ mm_base, z_base, stride, count, width, four_state != 0 };
    const uint32_t elem_bytes = (width + 7) / 8;

    const int64_t lo        = decl_left < decl_right ? decl_left : decl_right;
    const int64_t hi        = decl_left < decl_right ? decl_right : decl_left;
    int64_t       addr      = has_start ? start : lo;
    int64_t       remaining = has_count ? max_count : static_cast<int64_t>(count);

    int32_t bytes_read = 0;
    while (addr <= hi && remaining > 0) {
        uint64_t val = 0;
        uint32_t nb  = 0;
        while (nb < elem_bytes) {
            const int c = std::fgetc(f);
            if (c == EOF) {
                break;
            }
            val = (val << 8) | static_cast<uint8_t>(c);
            ++nb;
            ++bytes_read;
        }
        if (nb == 0) {
            break; // EOF before this element
        }
        // MSB-first: a short final element left-shifts so the bytes read occupy the
        // high end and the missing low bytes stay 0.
        val <<= 8u * (elem_bytes - nb);
        mem_write(dest, norm_addr(decl_left, decl_right, addr), val, val, 0);
        ++addr;
        --remaining;
        if (nb < elem_bytes) {
            break; // hit EOF mid-element
        }
    }
    return bytes_read;
}

extern "C" int32_t rt_random(void) {
    // $random's global stream. The seed is a fixed constant so runs reproduce
    // bit-for-bit; the LRM leaves the stream's contents implementation-defined,
    // requiring only stability for a given seed. 0x1979 is an arbitrary nonzero
    // constant. The generator is process-global (one simulator per process).
    static std::mt19937 gen{ 0x1979u };
    // A full-range int32 draw: reinterpret the 32-bit unsigned output as signed
    // so negative values occur, matching $random's signed return type.
    return static_cast<int32_t>(gen());
}

extern "C" void* rt_alloc(uint64_t size) {
    return std::malloc(size);
}

extern "C" void rt_free(void* p) {
    std::free(p);
}
