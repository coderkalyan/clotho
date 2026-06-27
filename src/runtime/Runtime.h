#pragma once

// Clotho simulation runtime: native support routines linked into the JIT
// environment and called from JIT'd designs. Deliberately has no dependency on
// LLVM or the rest of the compiler — it is plain native code the design calls
// into. The interface is C so the codegen can reference it by a stable symbol.

#include <cstdint>

extern "C" {

// -- $display / $write --
//
// One value argument of a display task. The codegen packs each operand's 4-state
// planes into the low 64 bits (wider values are truncated): `val` is the bit
// pattern, `xz` marks unknown bits (X or Z), and `z` marks the high-impedance
// subset (z is a subset of xz). Only the low `width` bits are significant.
typedef struct rt_arg {
    // Significant bit width of the value.
    uint32_t width;
    // Bit 0: the value is signed (drives %d sign-extension).
    uint32_t flags;
    // Value plane (definitely-1 bits) in the low `width` bits.
    uint64_t val;
    // Unknown mask: bit set => the value bit is X or Z.
    uint64_t xz;
    // High-impedance mask: bit set => the value bit is Z (implies the xz bit).
    uint64_t z;
} rt_arg;

// Format `fmt` with `argc` descriptors and write the result to stdout. Supports
// the common $display specifiers (%d %h/%x %b %o %c %t, and %%); each consumes
// the next descriptor. A %t descriptor carries a tick-valued time (see
// $timeformat below). The codegen folds any trailing newline into `fmt`.
void rt_display(const char* fmt, uint32_t argc, const rt_arg* argv);

// -- $timeformat / %t --
//
// The %t format specifier renders a time argument (passed in design ticks, i.e.
// the value scaled to the design's finest time unit) according to the format set
// by $timeformat (LRM §20.4.3). The runtime holds one global timeformat state.

// Seed the %t formatter at startup: `tick_fs` is the femtoseconds per design
// tick (so a tick argument converts to real time), and `default_units` is the
// smallest design time precision exponent. Installs the LRM default format
// (precision 0, empty suffix, field width 20).
void rt_timeformat_init(uint64_t tick_fs, int32_t default_units);

// $timeformat(units, precision, suffix, min_width): set the %t format. `units`
// and `precision` are 10^n-second exponents; `suffix` (length `suffix_len`) is
// appended after the number; `min_width` right-justifies the number + suffix.
void rt_timeformat_set(int32_t units, int32_t precision, const char* suffix, uint32_t suffix_len, int32_t min_width);

// -- Simulation control --
//
// $finish / $stop: end the simulation and exit the process. LRM 1800-2023 §20.2
// defines $stop as suspending simulation and $finish as exiting to the OS; it
// mandates no interactive mode. Clotho has no mechanism to resume a suspended
// run, so a suspend that can never resume is observationally identical to exit,
// and $stop is treated identically to $finish. Does not return.
// TODO: the optional diagnostic-level argument (0/1/2) that controls exit-message
// verbosity is accepted at the frontend but ignored here.
[[noreturn]] void rt_finish(void);

// -- $random --
//
// $random (LRM 1800-2023 §20.15.1): return the next value of the simulator's
// global random stream as a signed 32-bit integer. Clotho draws from a
// file-scope std::mt19937 seeded with a fixed constant, so a run is fully
// reproducible (re-running the same design yields the same stream). The
// generator is process-global, matching the single-simulator-per-process model.
// TODO: the seeded form $random(seed) threads per-seed PRNG state through an
// inout argument; it is rejected at the frontend and has no runtime entry yet.
int32_t rt_random(void);

// $power / ** operator (LRM 1800-2023 §11.4.3), unsigned 2-state path. Returns
// base**exp computed modulo 2^64 via exponentiation by squaring. The caller
// truncates to the result width W; since 2^W divides 2^64 for W <= 64, the
// modular result is exact at that width. Caller handles X/Z propagation and the
// signed/wide forms; this entry assumes both operands fit in 64 bits and are
// X/Z-free.
uint64_t rt_pow_u64(uint64_t base, uint64_t exp);

// -- Coroutine frame allocation --
//
// Each process lowers to an LLVM coroutine; its frame is allocated through these
// hooks (wired into the coro.alloc / coro.free pattern) so the sim owns frame
// memory rather than libc. Today they are thin malloc/free wrappers; a later swap
// to a pool/arena is transparent to codegen. The optimizer elides the allocation
// entirely for processes whose frame can live on the caller's stack.
void* rt_alloc(uint64_t size);
void  rt_free(void* p);

// -- Timing wheel --
//
// The wheel (src/Wheel.h) is the design's event scheduler, surfaced here behind
// a C ABI. Tick values are an abstract uint64 the wheel never interprets; the
// design owns the mapping from simulation time to ticks. The fired-timer
// context is a single opaque pointer the design threads through tick().

// Opaque inline storage for a wheel, owned by the caller. The design declares an
// rt_wheel (typically on the stack), hands it to rt_wheel_init, and tears it down
// with rt_wheel_deinit — no heap allocation. The byte block is sized and aligned to
// hold the concrete clotho::Wheel (both checked in Runtime.cpp); the design never
// inspects it.
typedef struct rt_wheel {
    alignas(8) unsigned char storage[2560];
} rt_wheel;

// Intrusive timer node the design embeds inside an owning awaiter and recovers
// via offsetof. Layout mirrors clotho::Timer (asserted in Runtime.cpp); the
// design never reads these fields, it only hands the node back to the wheel.
typedef struct rt_timer {
    // Intrusive slot-list link. Unspecified unless linked into a slot.
    struct rt_timer* next;
    // Type-erased expiry callback. Set by rt_wheel_schedule, re-typed on fire.
    void (*on_expire)();
} rt_timer;

// Expiry callback: invoked as on_expire(timer, ctx) when `timer` fires, where
// `ctx` is the pointer passed to the rt_wheel_tick that fired it.
typedef void (*rt_timer_fn)(rt_timer* timer, void* ctx);

// Construct a wheel in caller-owned storage, starting at tick zero. Pair with
// rt_wheel_deinit.
void rt_wheel_init(rt_wheel* wheel);

// Tear down a wheel constructed by rt_wheel_init, releasing its overflow heap. The
// caller still owns the rt_wheel storage and any timer nodes linked into it.
void rt_wheel_deinit(rt_wheel* wheel);

// Current tick. Starts at zero, advances only through rt_wheel_tick.
uint64_t rt_wheel_now(const rt_wheel* wheel);

// Non-zero iff the wheel has any timer scheduled. The design's sim loop ticks
// the wheel until this reports empty.
int rt_wheel_pending(const rt_wheel* wheel);

// Link `timer` to fire at absolute tick `deadline` (must be > rt_wheel_now),
// calling `on_expire(timer, ctx)` when it fires.
void rt_wheel_schedule(rt_wheel* wheel, rt_timer* timer, uint64_t deadline, rt_timer_fn on_expire);

// Jump to the next non-empty tick and fire its timers in list order, passing
// `ctx` to each callback. A no-op on an idle wheel. A node is unlinked before
// its callback runs, so a callback may re-schedule it.
void rt_wheel_tick(rt_wheel* wheel, void* ctx);

} // extern "C"
