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
    // Significant bit width of the value (0 for a string handle).
    uint32_t width;
    // Bit 0: the value is signed (drives %d sign-extension).
    // Bit 1: `val` carries an rt_string* (a %s string-object argument); the
    //        width/xz/z fields are meaningless.
    // Bit 2: the value is real — `val` is an IEEE-754 double bit pattern
    //        (shortreal is widened to double by the codegen) and xz / z are 0;
    //        it renders via the %e / %f / %g family (LRM 21.2, Table 21-2).
    uint32_t flags;
    // Value plane (definitely-1 bits) in the low `width` bits.
    uint64_t val;
    // Unknown mask: bit set => the value bit is X or Z.
    uint64_t xz;
    // High-impedance mask: bit set => the value bit is Z (implies the xz bit).
    uint64_t z;
} rt_arg;

// Format `fmt` with `argc` descriptors and write the result to descriptor
// `desc` — an MCD or FD (see the file-I/O block below); $display / $write pass
// the standard-output MCD (bit 0). Specifiers follow LRM §21.2.1 (%d %h/%x %b
// %o %c %s %t %u %z and %%, with widths, %0 minimal form, and `-` left-justify);
// each except %% consumes the next descriptor. A %t descriptor carries a
// tick-valued time (see $timeformat below). The frontend validates specifiers
// at lower time; an invalid one reaching the runtime aborts. The codegen folds
// any trailing newline into `fmt`.
void rt_fwrite(int32_t desc, const char* fmt, uint32_t argc, const rt_arg* argv);

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
//
// Before exiting it prints the exit banner per LRM Table 20-1, selected by
// `arg` (0/1/2): 0 prints nothing, 1 prints "<banner> called at <time>", 2 adds
// a resource-usage line. `banner` is the compile-time "<file>:<line>: <task>"
// prefix; `now` is the current simulation time in design ticks. The banner goes
// to stderr (a simulator diagnostic, not design output; the LRM fixes its
// content but not the stream).
[[noreturn]] void rt_finish(const char* banner, uint64_t now, int32_t arg);

// Report a priority-case violation (LRM 12.5.3): a `priority case` with no
// default reached at runtime with no matching item. Prints a warning banner to
// stderr and returns — non-fatal, the simulation continues. `banner` is the
// compile-time "<file>:<line>: " prefix; `now` is the current simulation time in
// design ticks. Fires once per dynamic execution that finds no match, matching
// iverilog. unique / unique0 overlap violations are a separate, unimplemented
// check (see docs/TODO.md).
void rt_case_violation(const char* banner, uint64_t now);

// Install (or clear, with null) the setjmp target rt_finish longjmps to, so a
// $finish ends only the simulation, not the embedding process (the test binary
// runs many simulations). The jump abandons live coroutine frames — acceptable,
// the simulation is over. `env` points at a std::jmp_buf.
void rt_finish_host(void* env);

// Drive one simulation: run `drain` (the event loop) under a $finish catch, then
// run `finals` (the `final` blocks, LRM 9.2.3). A $finish during the drain
// longjmps to this frame's own catch, so the finals still execute with the
// design state (owned by main, which called this) live. `state` and `wheel` are
// forwarded verbatim to both callbacks. Returns after the finals complete (or a
// final block's own $finish transfers to the outer host / exits).
void rt_sim_run(void* state, void* wheel, void (*drain)(void*, void*), void (*finals)(void*, void*));

// -- File I/O (LRM 1800-2023 §21.3) --
//
// A file descriptor is a 32-bit integer the design stores in its own variables.
// Two kinds are distinguished by bit 31:
//   * MCD (multi-channel descriptor), bit 31 clear: bits 0..30 form a channel
//     mask, one bit per open file, and a write goes to every set channel at
//     once. Bit 0 is the preopened standard output. Returned by the one-argument
//     $fopen(name), which opens for writing.
//   * FD (single file descriptor), bit 31 set: 0x8000_0000 | index of one
//     stream. Indices 0/1/2 are the preopened stdin/stdout/stderr. Returned by
//     the two-argument $fopen(name, mode); the read routines require this kind.
// $fopen returns 0 on failure (or when no MCD channel is free).

// $fopen(name): open `name` for writing and return a fresh MCD, or 0 on failure.
// `name_buf`/`name_len` carry the path as a packed-ASCII vector (as $readmem).
int32_t rt_fopen_mcd(const char* name_buf, uint32_t name_len);

// $fopen(name, mode): open `name` with C stdio mode string `mode` and return a
// fresh FD, or 0 on failure. Both strings are packed-ASCII vectors; `mode` is
// passed straight to fopen, so SV mode strings ("r", "w", "a", "r+b", ...) map
// directly.
int32_t rt_fopen_fd(const char* name_buf, uint32_t name_len, const char* mode_buf, uint32_t mode_len);

// $fclose(desc): close every stream the descriptor names (all set MCD channels,
// or the one FD). The preopened standard streams are never closed.
void rt_fclose(int32_t desc);

// The file read / positioning functions require an FD (a stream), not an MCD.
// Each maps directly onto its C stdio analog.

// $fgetc(fd): read one byte, or -1 (EOF) on end of file / error.
int32_t rt_fgetc(int32_t fd);

// $ungetc(c, fd): push `c` back so the next read returns it; -> c, or -1.
int32_t rt_ungetc(int32_t c, int32_t fd);

// $feof(fd): nonzero once the stream has hit end of file.
int32_t rt_feof(int32_t fd);

// $ftell(fd): current byte offset from the start, or -1 on error.
int32_t rt_ftell(int32_t fd);

// $fseek(fd, offset, operation): reposition the stream. `operation` 0/1/2 selects
// SEEK_SET / SEEK_CUR / SEEK_END. -> 0 on success, -1 on failure.
int32_t rt_fseek(int32_t fd, int32_t offset, int32_t operation);

// $rewind(fd): seek to the start; -> 0.
int32_t rt_rewind(int32_t fd);

// $fgets(str, fd): read a line into the packed vector `str`, whose must|may plane
// is at `mm` (2*`width` bits) and z plane at `z` (`width` bits). Reads up to
// `width`/8 characters, stopping after a newline or at EOF. The characters are
// stored right-justified (byte 0 = last character), the high bytes and the z
// plane are cleared. -> the character count, or 0 on EOF / error. `width` must be
// a multiple of 8. `flags` bit0: `mm` is a narrow 2-state slot holding the bare
// iW value — only `width` bits are written and `z` is null.
int32_t rt_fgets(uint8_t* mm, uint8_t* z, uint32_t width, int32_t fd, uint32_t flags);

// $sformat / $swrite (LRM §21.3.3): format `fmt` with `argc` descriptors (as
// rt_fwrite) and store the result into the destination string variable, whose
// must|may plane is at `mm` (2*`width` bits) and z plane at `z` (`width` bits).
// Stored right-justified; an oversized result keeps its low bytes. `width` must be
// a multiple of 8. `flags` as rt_fgets.
void rt_sformat(uint8_t* mm, uint8_t* z, uint32_t width, const char* fmt, uint32_t argc, const rt_arg* argv,
                uint32_t flags);

// $sformatf / $sformat into a string-object destination: format as rt_fwrite
// and assign the result to the rt_string at `dst` (declared below).
struct rt_string;
void rt_sformat_str(struct rt_string* dst, const char* fmt, uint32_t argc, const rt_arg* argv);

// $ferror(fd, str) (LRM §21.3.6): write the descriptor's most recent error message
// into the string variable at `mm` / `z` (`width` bits) and return its error code
// (0 when the stream is clear). `flags` as rt_fgets.
int32_t rt_ferror(int32_t fd, uint8_t* mm, uint8_t* z, uint32_t width, uint32_t flags);

// One output destination of a $fscanf / $sscanf conversion: the must|may plane
// pointer (2*`width` bits), the z plane pointer (`width` bits), and the width. The
// conversion char in the format decides how the parsed value is stored here.
// `flags` bit0: `mm` is a narrow 2-state slot holding the bare iW value — only
// `width` bits are written (x/z parsed digits drop to 0 per LRM 6.3.1) and `z`
// is null.
typedef struct rt_scan_arg {
    uint8_t* mm;
    uint8_t* z;
    uint32_t width;
    uint32_t flags;
} rt_scan_arg;

// $fscanf(fd, fmt, args) (LRM §21.3.4): parse the descriptor's input per `fmt`,
// writing each conversion into the matching `args` descriptor. Supports d/o/b/h/x
// (with x/z digits), c, s, `*` suppression and a field width. -> the number of
// stored conversions, or -1 if the input is exhausted before the first.
int32_t rt_fscanf(int32_t fd, const char* fmt, uint32_t argc, const rt_scan_arg* args);

// $sscanf(str, fmt, args): as rt_fscanf, but the source is the packed-ASCII vector
// at `buf` (`len` bytes).
int32_t rt_sscanf(const char* buf, uint32_t len, const char* fmt, uint32_t argc, const rt_scan_arg* args);

// $fread(target, fd[, start[, count]]) (LRM §21.3.4): read raw bytes from `fd` into
// a memory (or a scalar, `count` == 1), MSB-first per element. The destination
// geometry matches $readmem: element i's planes are at `mm_base` / `z_base` +
// i*`stride`, `width` bits each, over `count` elements with declared bounds
// `decl_left` / `decl_right`. `start` / `max_count` (valid per `flags` bit0 / bit1)
// bound the address range; `flags` bit2 marks a narrow 2-state destination (bare
// iW elements, null `z_base`). Returns the number of bytes read. `width` must be
// <= 64.
int32_t rt_fread(int32_t fd, uint8_t* mm_base, uint8_t* z_base, uint32_t stride, uint32_t count,
                 uint32_t width, uint32_t four_state, int64_t decl_left, int64_t decl_right, int64_t start,
                 int64_t max_count, uint32_t flags);

// $fflush(desc): flush the output buffers of descriptor `desc` (an MCD or FD).
// The no-argument $fflush passes 0, which flushes every open stream (LRM
// §21.3.6).
void rt_fflush(int32_t desc);

// $readmemh / $readmemb (LRM 1800-2023 §21.4): load a hex/binary text file into an
// unpacked-array memory. `path_buf`/`path_len` carry the filename as a packed-ASCII
// vector, or as bytes in natural order when flags bit4 is set (a string-object
// filename); `mm_base`/`z_base` point at element 0's must|may and z words in the
// state buffer, strided by `stride` (`z_base` is null when the memory's slot stores
// no z word); `count`/`width` describe the memory; `decl_left`/`decl_right` are its
// declared address bounds (for `@addr` and start/finish mapping). `flags` bit0=hex,
// bit1=has_start, bit2=has_finish, bit3=four_state, bit4=path_forward.
void rt_readmem(const char* path_buf, uint32_t path_len, uint8_t* mm_base, uint8_t* z_base, uint32_t stride,
                uint32_t count, uint32_t width, int64_t decl_left, int64_t decl_right, int64_t start,
                int64_t finish, uint32_t flags);

int32_t rt_random(void);

// -- Deferred nonblocking writes to unpacked-array elements --
//
// A synchronous memory write `mem[i] <= rhs` cannot use the per-signal NBA shadow
// (whole-signal, statically keyed), so each element write is deferred to the NBA
// region. The runtime only owns per-array growable record buffers; the record
// layout (destination pointer(s) + value [+ mask]) is defined and filled by the
// backend, monomorphized on the element's width / z-ness. Each array-NBA signal
// has a dense `idx`. The buffers are reused (count reset, capacity kept) across
// timesteps, so steady-state pushes allocate nothing at any element width.

// Reserve one `stride`-byte record slot in array `idx`'s buffer and return its
// address for the backend to fill. The buffer grows geometrically; the returned
// pointer is valid until the next slot call on the same idx.
void* rt_nba_arr_slot(uint32_t idx, uint32_t stride);

// Base pointer and record count of array `idx`'s buffer, read once in the NBA
// region to drive the backend-emitted commit loop.
void* rt_nba_arr_base(uint32_t idx);
uint32_t rt_nba_arr_count(uint32_t idx);

// Total records staged across all arrays this pass; the NBA region tests this to
// decide whether an array commit is needed.
uint32_t rt_nba_arr_any(void);

// Reset every array's record count to zero (buffers keep their capacity). Called
// after the commit + wake pass.
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

// Pointer to the string's bytes (not NUL-terminated; length via rt_string_len).
// Valid until the next mutation of `s`.
const char* rt_string_data(const rt_string* s);

// dst = parts[0] .. parts[n-1] concatenated. dst may appear among the parts
// (a = {a, b}); when its first occurrence is parts[0] the call appends in
// place with amortized growth, so repeated a = {a, chunk} is linear overall.
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

// dst = the source name of the enum member whose value equals `val`, or "" if
// none matches (LRM §6.19.2 e.name()). `values` / `names` / `lens` are the
// member table (parallel arrays of `count` rows) emitted per enum type.
void rt_enum_name(rt_string* dst, uint64_t val, uint32_t count, const uint64_t* values,
                  const char* const* names, const uint32_t* lens);

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

// s.atoi() / s.atohex() / s.atooct() / s.atobin() (LRM §6.16.9): scan leading
// digits and '_' separators from the start of s in the given base, stopping
// at the first other character or end of string; 0 if no digit matched. No
// leading whitespace or sign handling — the LRM excludes them explicitly.
// Overflow truncates silently (wraps in unsigned 32-bit arithmetic).
int32_t rt_string_atoi(const rt_string* s);
int32_t rt_string_atohex(const rt_string* s);
int32_t rt_string_atooct(const rt_string* s);
int32_t rt_string_atobin(const rt_string* s);

// s.atoreal() (LRM §6.16.10): parses a real-literal-like prefix, stopping at
// the first non-conforming character; 0.0 if no digits matched. Unlike the
// atoi family, leading whitespace and an optional '+' are skipped first
// (verified against iverilog).
double rt_string_atoreal(const rt_string* s);

// dst = sign + magnitude of `i` in the given base, lowercase digits (LRM
// §6.16.11-6.16.14; inverse of atoi/atohex/atooct/atobin). `i`'s x/z bits are
// read as 0 by the caller before this call, the same must-plane convention as
// StrFromPacked.
void rt_string_itoa(rt_string* dst, int32_t i);
void rt_string_hextoa(rt_string* dst, int32_t i);
void rt_string_octtoa(rt_string* dst, int32_t i);
void rt_string_bintoa(rt_string* dst, int32_t i);

// dst = ASCII representation of r at full round-trip precision (LRM
// §6.16.15; inverse of atoreal).
void rt_string_realtoa(rt_string* dst, double r);

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

// rt_wheel_pending + rt_wheel_tick folded into one call: advance and fire if any
// timer is scheduled and return non-zero, else return zero. Lets the sim loop
// cross the ABI boundary once per timestep instead of twice and skips the
// separate occupancy scan (advance already reports emptiness).
int rt_wheel_advance(rt_wheel* wheel, void* ctx);

} // extern "C"
