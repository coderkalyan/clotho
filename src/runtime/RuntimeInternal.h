// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Internal to the runtime: helpers and state shared by more than one
// src/runtime/ file. Only those files include this.

#pragma once

#include "Runtime.h"

#include "Coverage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cinttypes>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <queue>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// First, in a group of its own: psapi.h uses its types.
#include <windows.h>

#include <io.h>
#include <locale.h>
#include <psapi.h>
#else
#include <locale.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#endif

#include <ankerl/unordered_dense.h>
#include <fmt/color.h>

#include "Format.h"
#include "Os.h"
#include "Wheel.h"

namespace rt_internal {

// The runtime's monomorphization: every fired timer is handed back as the C-ABI
// rt_timer its callback was declared with, plus the single opaque context pointer
// the design threads through rt_wheel_tick.
using RuntimeWheel = clotho::Wheel<rt_timer, void*>;

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

inline RuntimeWheel* unwrap(rt_wheel* w) {
    return reinterpret_cast<RuntimeWheel*>(w->storage);
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

// NOTE: Globals with a non-trivial destructor are declared `extern` here and
// defined in one .cpp. As `inline` variables, clang guards their initialization
// with __cxa_guard_acquire, which an executable's runtime does not link (no C++
// library; docs/design/executables.md).
extern TimeFormat timeformat;

void render_time(std::string& out, uint64_t ticks, bool negative);

[[noreturn]] void format_fatal(const std::string& what);

// A format string that only existed at RUN TIME asked the renderer for something
// it cannot do. Unlike format_fatal this is a design error, not a broken
// invariant: LRM §21.3.3 lets $sformat / $sformatf take a format built at run
// time, and nothing then checks its contents until here, so every rejection
// lower/SystemTasks.cpp's check_specifier makes at compile time has to be made again with the
// text in hand. Reported in the ordinary located-fatal shape (headline, source
// caret, simulation time) and defined with that block below.
[[noreturn]] void dynfmt_fatal(const rt_fmt_dyn* dyn, const std::string& what);

// A specifier rendered back as source text, so a dynfmt_fatal headline can name
// the conversion the format asked for.
inline std::string spec_name(char conv) {
    return std::string("%") + conv;
}

// -- $display formatting --

// Low-`w`-bits mask (all ones for w >= 64).
inline uint64_t width_mask(uint32_t w) {
    return w >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << w) - 1);
}

// Number of 64-bit words a `width`-bit plane occupies (>= 1).
inline uint32_t plane_words(uint32_t width) {
    return width == 0 ? 1 : (width + 63) / 64;
}

uint64_t bit_group(const uint64_t* p, uint32_t words, uint32_t off, uint32_t bits);

void render_digits(std::string& out, const uint64_t* val, const uint64_t* xz, const uint64_t* z, uint32_t words,
                   uint32_t width, uint32_t bits);

std::string wide_udec(const uint64_t* w, uint32_t words);

uint32_t wide_dec_maxlen(uint32_t width, bool is_signed);

// Forward declarations: the rt_string representation lives below with the
// other string internals; %s rendering needs these two.
inline uint64_t    str_len(const rt_string* s);
inline const char* str_data(const rt_string* s);

uint32_t dec_digits(uint64_t v);

void render_arg(std::string& out, const rt_arg& a, const clotho::FormatSpec& spec,
                const rt_fmt_dyn* dyn = nullptr);

void render_unformatted(std::string& out, const rt_arg& a, bool four_state);

// Backing storage for a descriptor coerce_dyn_arg had to synthesize. The rt_arg
// planes are pointers, so the words they name have to outlive the render call --
// the caller owns one of these per specifier.
struct CoercedArg {
    rt_arg   arg{};
    uint64_t val  = 0;
    // Every synthesized value is two-state; one shared zero word serves both the
    // xz and z planes, which render_arg dereferences unconditionally for %d.
    uint64_t zero = 0;
};

int64_t sat_round_i64(double v);

int64_t integral_time_ticks(const rt_arg& a, uint64_t mul);

int64_t real_time_ticks(double v, uint64_t mul);

const rt_arg& coerce_dyn_arg(const rt_arg& a, char conv, CoercedArg& scratch, const rt_fmt_dyn* dyn);

void render_p_scalar(std::string& out, const rt_arg& a);

void render(std::string& out, const char* fmt, uint32_t argc, const rt_arg* argv,
            const rt_fmt_dyn* dyn = nullptr);

// MCD channels indexed by bit position; [0] is the preopened standard output. A
// one-argument $fopen claims the lowest free channel in [1, 30].
inline std::array<std::FILE*, 31> mcd_channels{};

// FD streams indexed by (descriptor & ~kFdBit); [0]/[1]/[2] are stdin/stdout/
// stderr. A closed slot holds nullptr and is reused before the vector grows.
extern std::vector<std::FILE*> fd_streams;

void io_ensure_init();

std::string unpack_ascii(const char* buf, uint32_t len);

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

inline bool str_is_heap(const rt_string* s) {
    return (s->storage[31] & str_heap_flag) != 0;
}

inline StrHeap str_heap(const rt_string* s) {
    StrHeap h;
    std::memcpy(&h, s->storage, sizeof(h));
    return h;
}

inline void str_set_heap(rt_string* s, const StrHeap& h) {
    std::memcpy(s->storage, &h, sizeof(h));
    s->storage[31] = str_heap_flag;
}

inline uint64_t str_len(const rt_string* s) {
    return str_is_heap(s) ? str_heap(s).len : str_inline_cap - s->storage[31];
}

inline const char* str_data(const rt_string* s) {
    return str_is_heap(s) ? str_heap(s).ptr : reinterpret_cast<const char*>(s->storage);
}

inline char* str_data(rt_string* s) {
    return str_is_heap(s) ? str_heap(s).ptr : reinterpret_cast<char*>(s->storage);
}

// Core write path: s = the n bytes at p. p may alias s's own buffer (memmove on
// the in-place paths; on the grow path the new buffer is filled before the old
// one is freed).
inline void str_assign(rt_string* s, const char* p, uint64_t n, const rt_alloc_site* site) {
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

// One xoshiro256++ step against a thread's RNG state. Byte-for-byte the
// same recurrence the backend inlines at each $urandom site
// (`emit_xoshiro_next` in ir/llvm/Ops.cpp), so a shuffle drawing from that
// state leaves the stream consistent for subsequent inlined $urandom draws.
inline uint64_t xoshiro_next(uint64_t* s) {
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
extern MonitorState g_monitor;

// FIFO queue of ramped-but-not-yet-resumed $strobe coroutine handles for the
// current time step (LRM 21.2.2). One growable buffer reused across
// instants: rt_strobe_pop advances a read cursor and only clears (never
// deallocates) once fully drained, so steady-state $strobe use allocates once.
extern std::vector<void*> g_strobe_queue;
inline size_t             g_strobe_read = 0;

inline bool scan_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

void append_finish_time(std::string& out, uint64_t now);

// ---- Observed region (LRM 4.4.2.4) deferred reports ----
//
// A report produced while the Active/NBA regions settle an instant is not
// emitted at its point of execution; it is queued here and flushed in the
// Observed region once the instant settles (rt_observed_flush, called from the
// sim loop between the NBA-settle and Postponed regions). unique/priority
// violation reports use it because LRM 12.4.2 / 12.5.3 place them in that
// region (docs/LRM_NOTES.md, note 18).
//
// The queue has no flush when a statement executes again in the same instant,
// so a statement that violates transiently on several settle passes queues one
// report per pass.
struct ObservedCaseViol {
    const char* banner; // compile-time "<file>:<line>: " prefix, static lifetime
    uint64_t    now;    // instant the violation occurred (== Observed flush time)
    int32_t     kind;   // 0 = no match, 1 = overlap
};
extern std::vector<ObservedCaseViol> g_observed_case;

// Set by a fatal severity report: rt_sim_run folds it into the exit status.
inline bool g_fatal_seen = false;

std::string severity_line(int32_t level, const char* banner, const char* fmt, uint32_t argc, const rt_arg* argv);

// Reports already emitted, keyed by access site: the emitted "<file>:<line>:
// <col>" string's address (one per instrumented access, stable for the run)
// with the read/write direction folded in, so a read and a write on the same
// line are counted apart. Scaled rather than bit-packed because a string
// constant carries no alignment guarantee.
extern ankerl::unordered_dense::map<uint64_t, uint32_t> g_bounds_hits;

// Whether a sanitizer report may use colour: a terminal that is not `dumb`,
// with NO_COLOR unset (https://no-color.org). Escape codes in a redirected log
// or a test's captured stderr are corruption, not decoration. Resolved once;
// the answer cannot change while the process runs. Cached in an atomic rather
// than a function-local static, whose thread-safe initialization would need the
// C++ ABI library: two threads racing the first call compute the same answer.
inline std::atomic<int8_t> g_diag_color{ -1 };

bool diag_color();

// `s` when colour is on, plain otherwise. fmt emits no escape at all for a
// default-constructed style, so one call site serves both.
inline fmt::text_style styled(fmt::text_style s) {
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

std::string snippet_text(const char* src);

void print_snippet(const std::string& text, int32_t line_no,
                   std::initializer_list<SnippetMark> marks, const fmt::text_style& loc,
                   const fmt::text_style& caret);

// Width of a recorded [column, end_column) source extent; 1 when the frontend
// recorded no extent, which draws a bare caret.
inline size_t mark_width(int32_t column, int32_t end_column) {
    return end_column > column ? static_cast<size_t>(end_column - column) : 1;
}

// ---- Stale `ref` through a restructured container (LRM 13.5.2) ----

// Whether a stale `ref` dereference warns and continues instead of ending the
// run (--loose-stale-ref). Process-wide like g_system_access: the host sets it
// once before the design runs, and the simulation thread only reads it.
inline int g_ref_stale_warn = 0;

// Dereference sites that have already warned. A stale deref inside an always
// block re-fires on every activation, and an unbounded stream of identical
// lines buries both the first report and the design's own output. Keyed on the
// rt_ref_site global's address: the backend emits one per guarded dereference
// and it is stable for the run. Only ever reached under --loose-stale-ref.
extern ankerl::unordered_dense::set<const void*> g_ref_stale_seen;

// ---- Checked heap allocation ----

// The wheel rt_sim_run is currently driving, so a cold diagnostic can report a
// simulation time without every allocating entry point carrying one across the
// ABI. Written once per run on the simulation thread and read only from a
// failure path.
inline const rt_wheel* g_diag_wheel = nullptr;

[[noreturn]] void alloc_fatal(const rt_alloc_site* site, uint64_t now, const std::string& headline);

void dynfmt_fatal(const rt_fmt_dyn* dyn, const std::string& what);

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

// The unseeded global $random stream. Starts at 0, which the algorithm remaps
// to 259341593 on the first draw, so an unseeded run draws the same
// deterministic stream every time.
inline uint32_t g_random_seed = 0;

// The command-line plusarg list (LRM §21.6), threaded from the host before
// the design runs. Process-global, like g_random_seed above — matches the
// single-simulator-per-process model.
extern std::vector<std::string> g_plusargs;

size_t shortest_double(char* out, double d);

} // namespace rt_internal

// Defined in Io.cpp, set by rt_run in Scheduler.cpp.
extern int g_system_access;
