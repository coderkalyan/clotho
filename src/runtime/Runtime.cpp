#include "Runtime.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

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

clotho::Timer& unwrap(rt_timer* t) {
    return *reinterpret_cast<clotho::Timer*>(t);
}

} // namespace

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

extern "C" void rt_display(const char* fmt, uint32_t argc, const rt_arg* argv) {
    std::string out;
    uint32_t    ai = 0;
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
    // Write raw: `out` may contain '%' or '{}' that printf/std::print would treat
    // as format syntax.
    std::fwrite(out.data(), 1, out.size(), stdout);
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

extern "C" void* rt_alloc(uint64_t size) {
    return std::malloc(size);
}

extern "C" void rt_free(void* p) {
    std::free(p);
}
