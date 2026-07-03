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
// $finish / $stop: end the simulation. LRM 1800-2023 §20.2 defines $stop as
// suspending simulation and $finish as exiting to the OS; it mandates no
// interactive mode. Clotho has no mechanism to resume a suspended run, so a
// suspend that can never resume is observationally identical to exit, and $stop
// is treated identically to $finish. Never returns to the caller: control
// transfers to the host installed by rt_finish_host (run_jit), or the process
// exits when no host is installed.
// TODO: the optional diagnostic-level argument (0/1/2) that controls exit-message
// verbosity is accepted at the frontend but ignored here.
[[noreturn]] void rt_finish(void);

// Install (or clear, with null) the setjmp target rt_finish longjmps to, so a
// $finish ends only the simulation, not the embedding process (the test binary
// runs many simulations). The jump abandons live coroutine frames — acceptable,
// the simulation is over. `env` points at a std::jmp_buf.
void rt_finish_host(void* env);

// $fflush (no-arg form, LRM 1800-2023 §21.3.6): flush all open output buffers so
// queued $write output is emitted immediately.
void rt_fflush(void);

// $readmemh / $readmemb (LRM 1800-2023 §21.4): load a hex/binary text file into an
// unpacked-array memory. `path_buf`/`path_len` carry the filename as a packed-ASCII
// vector; `mm_base`/`z_base` point at element 0's must|may and z words in the state
// buffer, strided by `stride` (`z_base` is null when the memory's slot stores no z
// word); `count`/`width` describe the memory; `decl_left`/`decl_right` are its
// declared address bounds (for `@addr` and start/finish mapping). `flags` bit0=hex,
// bit1=has_start, bit2=has_finish, bit3=four_state.
void rt_readmem(const char* path_buf, uint32_t path_len, uint8_t* mm_base, uint8_t* z_base, uint32_t stride,
                uint32_t count, uint32_t width, int64_t decl_left, int64_t decl_right, int64_t start,
                int64_t finish, uint32_t flags);

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

// -- Deferred nonblocking writes to unpacked-array elements --
//
// A synchronous memory write `mem[i] <= rhs` cannot use the per-signal NBA
// shadow (whole-signal, statically keyed). Each element write is appended to
// this deferred queue in the active region and replayed in the NBA region. A
// record is a bytewise masked merge into the live element slot, so the queue is
// width-agnostic; the value / mask byte blobs are copied into runtime-owned
// storage at push time. One global queue serves the whole design.

// Append one deferred element write. `sig` identifies the array signal (for
// post-commit wake servicing). `mm_ptr` / `z_ptr` point at the live element's
// must|may and z words; `*_val` are the bits to write and `*_mask` selects which
// bits (1 => take from val), each `*_bytes` long. A target whose slot stores no
// z word passes `z_ptr` null with `z_bytes` 0.
void rt_nba_arr_push(uint32_t sig, void* mm_ptr, const void* mm_val, const void* mm_mask, uint32_t mm_bytes,
                     void* z_ptr, const void* z_val, const void* z_mask, uint32_t z_bytes);

// Number of queued element writes; the NBA region tests this to decide whether a
// commit pass is needed.
uint32_t rt_nba_arr_pending(void);

// Replay every queued write into its destination (bytewise (dest & ~mask) | (val
// & mask)), in push order so a later write to the same element wins. Records
// stay queued (for rt_nba_arr_dirty) until rt_nba_arr_reset.
void rt_nba_arr_commit(void);

// Non-zero iff any currently-queued write targets `sig` (queried after commit to
// decide whether to wake that array's readers).
int rt_nba_arr_dirty(uint32_t sig);

// Drop all queued writes (called after a commit + wake pass).
void rt_nba_arr_reset(void);

// -- SystemVerilog string objects --
//
// The dynamic `string` type (LRM §6.16). A string value lives in a fixed
// 32-byte caller-owned handle embedded in design state (same ownership model as
// rt_wheel): small strings are stored inline in the handle, larger ones spill
// to a heap buffer the handle owns. Copies are eager deep copies; handles never
// share storage. A handle must be constructed with rt_string_init before any
// other call and released with rt_string_deinit. Every operation that writes a
// destination handle requires it already constructed and reuses its existing
// capacity, so steady-state reassignment in a loop does not allocate.
typedef struct rt_string {
    alignas(8) unsigned char storage[32];
} rt_string;

// Construct the empty string "" in caller-owned storage. Never allocates.
void rt_string_init(rt_string* s);

// Release any heap buffer owned by `s`. The caller still owns the handle
// storage; `s` is left as a valid empty string, so deinit is idempotent.
void rt_string_deinit(rt_string* s);

// dst = src (deep copy). dst == src is a no-op.
void rt_string_assign(rt_string* dst, const rt_string* src);

// dst = the `n` bytes at `p`. `p` is a raw byte sequence, not NUL-terminated;
// it may alias dst's own buffer.
void rt_string_assign_bytes(rt_string* dst, const char* p, uint64_t n);

// Length in bytes ($.len()). The empty string has length 0.
uint64_t rt_string_len(const rt_string* s);

// dst = parts[0] .. parts[n-1] concatenated. dst may appear among the parts
// (a = {a, b}).
void rt_string_concat(rt_string* dst, const rt_string* const* parts, uint32_t n);

// dst = src repeated `count` times ({count{src}}); count == 0 yields "".
void rt_string_repl(rt_string* dst, const rt_string* src, uint64_t count);

// Lexicographic comparison in strcmp order over (bytes, then length): returns
// < 0, 0, or > 0. The LRM (§6.16.9) specifies only the sign.
int32_t rt_string_cmp(const rt_string* a, const rt_string* b);

// rt_string_cmp over ASCII-lowercased operands ($.icompare()).
int32_t rt_string_icmp(const rt_string* a, const rt_string* b);

// dst = src.substr(i, j): the bytes at positions i..j inclusive, or "" when the
// range is invalid (i < 0, j < i, or j >= len; LRM §6.16.8).
void rt_string_substr(rt_string* dst, const rt_string* src, int32_t i, int32_t j);

// dst = src with ASCII letters upper/lowercased. dst may alias src.
void rt_string_toupper(rt_string* dst, const rt_string* src);
void rt_string_tolower(rt_string* dst, const rt_string* src);

// s[i] = c ($.putc()): a no-op when i is out of range or (c & 0xff) == 0 — a
// string never contains a 0 byte (LRM §6.16.2).
void rt_string_putc(rt_string* s, int32_t i, uint32_t c);

// s[i] ($.getc()): the byte at i, or 0 when i is out of range (LRM §6.16.3).
uint32_t rt_string_getc(const rt_string* s, int32_t i);

// string'(value): `val` is the little-endian val plane of a packed value,
// plane_bytes(width_bits) long. Bytes are taken most-significant first and
// every 0x00 byte is dropped (LRM §6.16: the integral value converts as a
// string literal, and literal conversion removes \0 characters). X/Z bits are
// read as 0 — the caller passes the val plane only.
void rt_string_from_packed(rt_string* dst, const uint8_t* val, uint32_t width_bits);

// Packed-integral cast of a string: writes the little-endian val plane of the
// result into `out`, which is plane_bytes(width_bits) long. Characters pack
// most-significant first, right-justified: truncated on the left when the
// string is longer than the width, zero-filled on the left when shorter
// (LRM §6.16).
void rt_string_to_packed(const rt_string* s, uint8_t* out, uint32_t width_bits);

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
