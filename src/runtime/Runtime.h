#pragma once

// Clotho simulation runtime: native support routines linked into the JIT
// environment and called from JIT'd designs. Deliberately has no dependency on
// LLVM or the rest of the compiler — it is plain native code the design calls
// into. The interface is C so the codegen can reference it by a stable symbol.

#include <cstdint>

extern "C" {

// -- $display / $write --
//
// One value argument of a display task. Each plane is a little-endian word array
// of ceil(width/64) words (LSW first); the codegen zero-extends the bits above
// `width`, so a plane never carries garbage past the operand width. This carries
// operands of any width, not only <= 64 bits.
typedef struct rt_arg {
    // Significant bit width of the value (0 for a string handle).
    uint32_t width;
    // Bit 0: the value is signed (drives %d sign-extension).
    // Bit 1: `val` carries an rt_string* directly (a %s string-object argument);
    //        the width/xz/z fields are meaningless (val is not a plane pointer).
    // Bit 2: the value is real — `val` points to one word holding an IEEE-754
    //        double bit pattern (shortreal is widened to double by the codegen)
    //        and xz / z are null; it renders via the %e / %f / %g family (LRM
    //        21.2, Table 21-2).
    uint32_t flags;
    // Value plane (definitely-1 bits), or an rt_string* / double-bits word per
    // the flags above.
    const uint64_t* val;
    // Unknown mask: bit set => the value bit is X or Z. Null for string / real.
    const uint64_t* xz;
    // High-impedance mask: bit set => the value bit is Z (implies the xz bit).
    // Null for string / real.
    const uint64_t* z;
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

// Report a unique/priority case (LRM 12.5.3) or if (LRM 12.4.2) violation.
// Prints a warning banner
// to stderr and returns — non-fatal, the simulation continues. `banner` is the
// compile-time "<file>:<line>: " prefix; `now` is the current simulation time in
// design ticks. `kind` selects the message: 0 = no case item matched (priority
// or unique, no default), 1 = more than one case item matched (unique /
// unique0 overlap). Fires once per dynamic execution that detects the violation.
void rt_case_violation(const char* banner, uint64_t now, int32_t kind);

// Report a severity system task ($info / $warning / $error / $fatal, LRM 20.10)
// or an immediate-assertion default failure (LRM 16.3). Renders the $display-
// style message (`fmt` + `argc` `argv`, empty for a bare `$error;`) and writes
// "<LABEL>: <banner><message>\n" to stderr, then returns — non-fatal. `level` is
// the ir::Severity label (0 info, 1 warning, 2 error, 3 fatal); it selects the
// LABEL only. `banner` is the compile-time "<file>:<line>: " prefix. $fatal's
// termination is a separate rt_finish the caller emits after this report.
void rt_severity(int32_t level, const char* banner, const char* fmt, uint32_t argc, const rt_arg* argv);

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

// -- Value-change dump (VCD, LRM 1800-2023 §21.7; design in docs/VCD.md) --
//
// Registration is call-based: at the $dumpvars site the backend walks the design
// scope tree, calling rt_vcd_begin, then rt_vcd_scope / rt_vcd_var / rt_vcd_upscope
// in tree order, then rt_vcd_start. Each rt_vcd_var passes live pointers to the
// signal's planes in the design state buffer (stable for the run), so the runtime
// needs no offset math or state base. See docs/VCD.md.

// rt_vcd_var flags: bit 0 four-state (has x/z), bit 1 net (`wire` vs `reg` var),
// bit 2 narrow (a 2-state `{ iW }` slot: read the single iW word as the value,
// with must == may and no z plane).
#define RT_VCD_FLAG_FOUR_STATE 1u
#define RT_VCD_FLAG_NET        2u
#define RT_VCD_FLAG_NARROW     4u

// $dumpfile: record the output path. When `forward` is 0 the path is a right-
// justified packed-ASCII vector (byte 0 = last character), as the backend spills a
// packed value; when 1 it is the natural-order bytes of a string object. The file
// is opened at rt_vcd_begin.
void rt_vcd_open(const char* path_buf, uint32_t path_len, uint32_t forward);

// Begin the header: open the file and write $date / $version / $timescale. `wheel`
// is cached for the close-time flush; `tick_fs` / `units` set the timescale.
void rt_vcd_begin(void* wheel, uint64_t tick_fs, int32_t units);

// $scope module <name> / $upscope: open / close a hierarchy level in the header.
void rt_vcd_scope(const char* name, uint32_t name_len);
void rt_vcd_upscope(void);

// Register one dumped variable: `name` its leaf reference, `width` its bit width
// (1..64 in phase 0), `flags` per RT_VCD_FLAG_*, `mm` its packed must|may plane
// pointer, `z` its z plane pointer (null for a never-z / narrow slot). Writes the
// $var line and adds the variable to the scan table.
void rt_vcd_var(const char* name, uint32_t name_len, uint32_t width, uint32_t flags, const uint8_t* mm,
                const uint8_t* z);

// Register a whole unpacked-array (memory) as one $var per element, named
// `<name>[<addr>]` over the declared address range [left:right]. Element k's
// planes sit at `mm_base + k*stride` (and `z_base + k*stride`, or z_base null for a
// never-z / narrow memory). `width`/`flags` describe one element. Registering the
// array in a single call keeps the emitted design independent of the array size.
void rt_vcd_array(const char* name, uint32_t name_len, uint32_t count, uint32_t width, uint32_t flags,
                  const uint8_t* mm_base, const uint8_t* z_base, uint32_t stride, int64_t left, int64_t right);

// Finish the header ($enddefinitions) and arm the initial full dump.
void rt_vcd_start(void);

// Mark variable `idx` changed this instant (activity-driven dirty set). Called
// from the design at each write to a dumped scalar signal (blocking store and NBA
// commit); the next rt_vcd_flush visits only the marked variables. A no-op when
// dumping is not active.
// Non-zero while VCD dumping is active. The JIT loads this to skip the
// rt_vcd_touch call when nothing is being dumped (see LlvmBackend touch gate).
extern unsigned char rt_vcd_active;
void rt_vcd_touch(uint32_t idx);

// Mark the `count` variables [base, base+count) changed -- used for a whole
// unpacked-array (memory) write, where `base` is the array's first element var.
// A blocking element write touches the single element; an NBA array commit
// touches the whole array (the runtime queue does not carry per-element indices).
void rt_vcd_touch_range(uint32_t base, uint32_t count);

// Postponed-region sample at a settled instant, called from the sim loop before
// the wheel advances (`tick` is rt_wheel_now). Emits changed variables under one
// `#tick` marker; a no-change instant emits nothing. A no-op when dumping is not
// active. Also services a pending $dumpoff / $dumpall / $dumpon at this instant.
void rt_vcd_flush(uint64_t tick);

// $dumpoff / $dumpon / $dumpall / $dumpflush (LRM §21.7.4-7). off/on/all only set
// a deferred flag serviced by the next rt_vcd_flush, so rt_vcd_flush stays the
// sole writer and the change lands at the correct instant; dumpflush drains the
// output buffer to disk immediately.
void rt_vcd_off(void);
void rt_vcd_on(void);
void rt_vcd_all(void);
void rt_vcd_dumpflush(void);

// Final flush of the in-progress instant + output buffer, then close the file.
// Called on normal simulation end and on the $finish longjmp path.
void rt_vcd_close(void);

int32_t rt_random(void);

// -- Probabilistic distribution functions (LRM §20.15.2) and $random(seed) --
//
// Implementations of the distribution algorithms IEEE Std 1800-2023 (formerly
// IEEE 1364) defines in Annex N. The LRM mandates the exact algorithm so every
// conforming tool produces the same stream from the same seed (and these match
// iverilog's output byte-for-byte). Each takes the caller's inout `seed` (the
// whole PRNG state, one int32) plus the distribution parameters, and returns the
// updated seed packed in the high 32 bits with the signed integer draw in the
// low 32 — the JIT slices the two halves, writing the seed back to the lvalue.
//
// $random(seed) is exactly rt_dist_uniform(seed, INT32_MIN, INT32_MAX) and is
// lowered through this same path (the unseeded $random above keeps its own
// process-global stream).
uint64_t rt_dist_uniform(int32_t seed, int32_t start, int32_t end);
uint64_t rt_dist_normal(int32_t seed, int32_t mean, int32_t deviation);
uint64_t rt_dist_exponential(int32_t seed, int32_t mean);
uint64_t rt_dist_poisson(int32_t seed, int32_t mean);
uint64_t rt_dist_chi_square(int32_t seed, int32_t df);
uint64_t rt_dist_t(int32_t seed, int32_t df);
uint64_t rt_dist_erlang(int32_t seed, int32_t k, int32_t mean);

// -- $urandom / $urandom_range (LRM §18.13) --
//
// Unlike $random/$dist_* the LRM leaves the $urandom algorithm unspecified,
// requiring only that it be deterministic and per-process ("thread") stable. Its
// RNG is a per-process xoshiro256++ generator whose 256-bit state lives in the
// process's coroutine frame; the draw and the $urandom_range mapping are inlined
// into the JIT (no runtime call per draw). The only runtime entry is the
// one-per-process seeding of that state from a 64-bit selector (splitmix64
// expansion): the process index at startup, or the user seed for $urandom(seed).
// A modern integer PRNG (not the $random LCG) is chosen for quality and speed
// per the project's "unspecified -> pick performance and simplicity"; the stream is
// therefore intentionally not iverilog-identical.
void rt_urandom_seed(uint64_t* state, uint64_t seed);

// -- Plusargs (LRM §21.6) --
//
// Command-line arguments prefixed with `+` (everything after the leading `+`
// dropped), in the order given on Clotho's own command line. Populated once
// by the host before the design runs; the backing storage must outlive the
// run. Not thread-safe (matches the single-simulator-per-process model
// elsewhere in the runtime, e.g. $random / the file table).
void rt_plusargs_init(const char* const* args, const uint32_t* lens, uint32_t n);

// $test$plusargs(str) (LRM §21.6): search the plusargs in order; -> nonzero
// iff some plusarg's prefix matches every character of `s` (`len` bytes).
int32_t rt_test_plusargs(const char* s, uint32_t len);

// $value$plusargs(user_string, var) (LRM §21.6): `prefix`/`prefix_len` are
// the plusarg_string portion of user_string (a compile-time literal); `conv`
// is the format character ('d'/'o'/'b'/'h'/'s'). Searches the plusargs in
// order for one whose prefix matches `prefix`; on a match, converts its
// remainder per `conv` into `arg` (zero-padded / truncated to `arg->width`,
// a negative value always truncates, an illegal character writes 'bx) and
// returns nonzero. On no match, `arg` is left untouched and the function
// returns 0.
int32_t rt_value_plusargs(const char* prefix, uint32_t prefix_len, uint32_t conv, const rt_scan_arg* arg);

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

// -- SystemVerilog dynamic arrays --
//
// The dynamic array type (LRM §7.5): a runtime-sized vector of a fixed-width
// element. The value lives in a 16-byte caller-owned handle embedded in design
// state (same ownership model as rt_string): `data` points at one heap buffer of
// `len` elements, each `stride` bytes (the backend's per-element slot size),
// malloc-owned by the handle. Copies are eager deep copies; handles never share
// storage. A handle powers on zeroed — { null, 0 } is the empty array — and is
// released with rt_dynarr_delete. The per-element record layout is defined by the
// backend; the runtime owns only the raw byte buffer.
typedef struct rt_dynarr {
    void*    data;
    uint64_t len;
} rt_dynarr;

// arr = new[n]: (re)allocate `h` to `n` elements of `stride` bytes, filling each
// with the `stride`-byte power-on prototype `proto` (the element default: X for
// 4-state, 0 for 2-state). Frees any prior buffer.
void rt_dynarr_new(rt_dynarr* h, uint64_t stride, uint64_t n, const void* proto);

// arr = new[n](src): like rt_dynarr_new, but the low min(n, src->len) elements
// are first copied from `src`; any remaining elements take `proto` (LRM §7.5.1).
// `src` may alias `h` (a self-resize).
void rt_dynarr_new_copy(rt_dynarr* h, uint64_t stride, uint64_t n, const void* proto, const rt_dynarr* src);

// arr.delete(): free the buffer and reset to the empty array. Idempotent.
void rt_dynarr_delete(rt_dynarr* h);

// dst = src: deep copy `src`'s `stride`-byte elements into `dst` (value
// semantics, LRM §7.5). A no-op when dst == src.
void rt_dynarr_assign(rt_dynarr* dst, const rt_dynarr* src, uint64_t stride);

// -- Array ordering methods (LRM 7.12.2) --
//
// `kind` selects the sort comparator over an element's `width`-bit value:
// 0 = unsigned integral, 1 = signed integral, 2 = IEEE real (double). The
// frontend restricts sort()/rsort() to a 2-state integral (width <= 64) or real
// element; reverse() is element-agnostic (whole-slot move) and takes no kind.

// arr.reverse(): reverse the element order in place.
void rt_dynarr_reverse(rt_dynarr* h, uint64_t stride);

// arr.sort() (rev == 0) / arr.rsort() (rev == 1): order elements ascending /
// descending by value.
void rt_dynarr_sort(rt_dynarr* h, uint64_t stride, uint32_t kind, uint32_t width, uint32_t rev);

// -- SystemVerilog queues --
//
// The queue type (LRM §7.10): a runtime-sized double-ended vector of a
// fixed-width element. The value lives in a 32-byte caller-owned handle embedded
// in design state (same ownership model as rt_string / rt_dynarr): `base` points
// at one heap ring buffer of `cap` elements, each `stride` bytes; the `len` live
// elements occupy physical slots (head + i) mod cap for i in [0, len). Copies are
// eager deep copies; handles never share storage. A handle powers on zeroed —
// { null, 0, 0, 0 } is the empty queue — and is released with rt_queue_delete_all.
// The per-element record layout is defined by the backend; the runtime owns only
// the raw byte buffer.
typedef struct rt_queue {
    void*    base;
    uint64_t head;
    uint64_t len;
    uint64_t cap;
} rt_queue;

// Bounded-queue max size argument for the push helpers: `bound` is the `[$:N]`
// maximum index (max size N+1); 0 means unbounded (LRM §7.10.4). An overflowing
// push drops the element at the opposite end.
//
// q.push_back(e) / q.push_front(e): append `elem` (a `stride`-byte element slot)
// to the tail / head, growing the ring as needed.
void rt_queue_push_back(rt_queue* h, uint64_t stride, uint64_t bound, const void* elem);
void rt_queue_push_front(rt_queue* h, uint64_t stride, uint64_t bound, const void* elem);

// q.pop_back() / q.pop_front(): remove the tail / head element and copy it into
// `out` (a `stride`-byte slot). On an empty queue `out` is left untouched (the
// caller pre-fills it with the element default).
void rt_queue_pop_back(rt_queue* h, uint64_t stride, void* out);
void rt_queue_pop_front(rt_queue* h, uint64_t stride, void* out);

// q.insert(i, e): insert `elem` before position `i` (0 <= i <= len); out-of-range
// `i` is ignored. Honours the bound like a push.
void rt_queue_insert(rt_queue* h, uint64_t stride, uint64_t bound, uint64_t i, const void* elem);

// q.delete(i): remove the element at position `i` (0 <= i < len); out-of-range
// `i` is ignored.
void rt_queue_delete_idx(rt_queue* h, uint64_t stride, uint64_t i);

// q.delete(): free the buffer and reset to the empty queue. Idempotent.
void rt_queue_delete_all(rt_queue* h);

// dst = src: deep copy `src`'s `stride`-byte elements into `dst`, linearised so
// dst->head == 0 (value semantics, LRM §7.10). A no-op when dst == src.
void rt_queue_assign(rt_queue* dst, const rt_queue* src, uint64_t stride);

// q.reverse() / q.sort() / q.rsort() (LRM 7.12.2). `kind` / `width` / `rev` match
// the rt_dynarr_* ordering entry points; operates on the ring's logical order.
void rt_queue_reverse(rt_queue* h, uint64_t stride);
void rt_queue_sort(rt_queue* h, uint64_t stride, uint32_t kind, uint32_t width, uint32_t rev);

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

// Max inline (SSO) string length. One byte shorter than the 31 free bytes so
// byte[len] can always hold the NUL terminator (rt_string_data is a valid C
// string). Shared with the backend, which materializes the power-on empty-string
// constant — the two MUST agree or that constant corrupts. See the SSO block in
// Runtime.cpp.
constexpr uint64_t rt_string_inline_cap = 30;

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

// Pointer to the string's bytes, NUL-terminated at byte[len] so it is a valid C
// string (length via rt_string_len; embedded NULs make the C view stop early).
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
// none matches (LRM §6.19.2 e.name()). `val` is the operand's must-plane bytes
// (`vbytes` = (width+7)/8) and `values` is the `count` member values packed
// contiguously (`vbytes` each); `names` / `lens` are the parallel name table.
// Byte-compared, so a base wider than 64 bits works.
void rt_enum_name(rt_string* dst, const uint8_t* val, uint32_t vbytes, uint32_t count, const uint8_t* values,
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
    alignas(8) unsigned char storage[9216];
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

// Like rt_wheel_schedule but the deadline is `rt_wheel_now + delay`, computed
// runtime-side so a #delay crosses the C ABI once instead of twice. `delay`
// must be > 0 (a #0 never reaches the wheel — it parks in the inactive queue).
void rt_wheel_schedule_rel(rt_wheel* wheel, rt_timer* timer, uint64_t delay, rt_timer_fn on_expire);

// Jump to the next non-empty tick and fire its timers in list order, passing
// `ctx` to each callback. A no-op on an idle wheel. A node is unlinked before
// its callback runs, so a callback may re-schedule it.
void rt_wheel_tick(rt_wheel* wheel, void* ctx);

// rt_wheel_pending + rt_wheel_tick folded into one call: advance and fire if any
// timer is scheduled and return non-zero, else return zero. Lets the sim loop
// cross the ABI boundary once per timestep instead of twice and skips the
// separate occupancy scan (advance already reports emptiness).
int rt_wheel_advance(rt_wheel* wheel, void* ctx);

// -- Deferred nonblocking intra-assignment stores (LRM 9.4.5) --
//
// `lhs <= #d rhs` samples the RHS now and schedules the LHS update for `now + d`
// without blocking the process, so any number of updates can be in flight at
// once (a pipeline `q <= #1 d` firing each cycle). Each is a fixed record the
// backend fills (its value planes, monomorphized per target width/z-ness) and
// arms on the timing wheel; on fire the record is queued for the NBA region of
// that instant, where a backend-emitted commit stores it and wakes readers —
// same region ordering as a same-instant `<=`. Records are keyed by a dense
// per-target-signal `idx`.
//
// Unlike the array-NBA buffers, a record must keep a STABLE address from arm
// (the wheel holds a pointer to its embedded timer) until it fires, so records
// are individually owned and recycled through a per-idx freelist rather than
// packed in a reallocating buffer. Steady state allocates nothing.

// Runtime-owned record header the backend places at offset 0 of every record;
// the backend's value tail follows. `timer` is the wheel node (offset 0, so the
// record base doubles as the rt_timer*, used by the `#d` delay path); `idx`
// routes the record to its target site; `seq` is a monotonic arm-order stamp (set
// by rt_intra_alloc) so same-instant commits apply in statement-execution order
// (LRM 10.4.2, last-executed wins), which fire order does not preserve; `count`
// is the event countdown for the `@e` / `repeat(N) @e` path (unused by `#d`).
typedef struct rt_intra_rec {
    rt_timer timer;
    uint32_t idx;
    uint32_t seq;
    uint32_t count;
} rt_intra_rec;

// Reserve a `stride`-byte record for site `idx` (from its freelist; allocates
// only to grow the pool). The backend fills the value tail, sets `idx`, and arms
// the record on the wheel with rt_intra_cb as the expiry callback.
void* rt_intra_alloc(uint32_t idx, uint32_t stride);

// Wheel expiry callback: recover the record, link it onto its site's ready list,
// and bump the global pending count. Shared by every deferred-store site; does
// not touch design state (commit is the NBA region's job).
void rt_intra_cb(rt_timer* timer, void* ctx);

// Base (a `void**` array of record pointers) and length of site `idx`'s ready
// list, read once in the NBA region to drive the backend-emitted commit loop.
void*    rt_intra_ready_base(uint32_t idx);
uint32_t rt_intra_ready_count(uint32_t idx);

// Total ready records across all sites; nba.check tests this to decide whether a
// deferred commit is needed.
uint32_t rt_intra_any(void);

// Return site `idx`'s committed records to its freelist and clear its ready list
// (called by the backend after the site's commit loop).
void rt_intra_release(uint32_t idx);

// Reclaim every record (including any left scheduled by a $finish mid-flight)
// into its freelist and clear all ready lists. Called once at simulation start.
void rt_intra_reset(void);

// -- Event-triggered deferred stores (`x <= @e y` / `x <= repeat(N) @e y`) --
//
// Same records and NBA-region commit as the `#d` path, but fired by a signal edge
// instead of the wheel. A `<= @e` site registers its record on a dense trigger id
// `trig_id` (a distinct (trigger-signal, edge) pair); when that edge fires (from a
// blocking store or an NBA commit), the backend calls rt_intra_event_fire, which
// counts the record down and, at zero, moves it to its target's ready list for the
// NBA region. `count` is N for `repeat(N) @e`, 1 for a plain `@e`.

// Register `rec` (its idx/seq/count already filled) to fire on trigger `trig_id`.
void rt_intra_event_register(uint32_t trig_id, void* rec);

// Fire trigger `trig_id`: decrement each pending record's count; at zero, move it
// to its target's ready list (bumping the NBA-pending total), else keep it pending
// for the next edge. Called from both wake paths when the trigger edge matches.
void rt_intra_event_fire(uint32_t trig_id);

// -- DPI-C packed-vector marshalling (LRM 35.5.6) --
//
// Convert Clotho's must/may/z planes (each chunked to uint32) to/from the DPI
// canonical svLogicVecVal — an array of {aval, bval} 32-bit chunks. Per bit:
// aval = value-or-X, bval = unknown-or-Z, so
//   aval = may & ~z ,  bval = must ^ may
// and inversely must = aval & ~bval, may = aval | bval, z = bval & ~aval.
// `nchunks` = ceil(width/32); `out`/`in` is the flat svLogicVecVal (2*nchunks
// uint32). The 2-state form (svBitVecVal) needs no helper — the value plane
// chunked is already the canonical layout.
void rt_dpi_pack_logicvec(const uint32_t* must, const uint32_t* may, const uint32_t* z,
                          uint32_t nchunks, uint32_t* out);
void rt_dpi_unpack_logicvec(const uint32_t* in, uint32_t nchunks, uint32_t* must, uint32_t* may,
                            uint32_t* z);

// Copy a NUL-terminated C string into an SV string handle — the copy-out for a
// DPI import that returns `string` (LRM 35.5.6). The C retains ownership of `p`;
// a null `p` yields the empty string. (An SV string argument passed to C needs no
// helper — rt_string_data() is already a valid C `const char*`.)
void rt_dpi_str_out(rt_string* dst, const char* p);

// Register the single DPI scope (LRM 35.5.3) for the svScope API — the design's
// hierarchical name. Called once from main when the design uses DPI; also resets
// the scope's user-data (so a fresh simulation in the same process starts clean).
// Implemented in Svdpi.cpp alongside svGetScope / svSetScope / svGetUserData.
void rt_svdpi_scope_init(const char* name, uint32_t name_len);

// -- DPI open arrays (LRM 35.5.6.2) --
//
// Clotho's backing for a 1-D svOpenArrayHandle: the JIT builds one per call and
// the svdpi accessors (Svdpi.cpp) read it. `data` points at the element buffer —
// the design array itself for a zero-copy 2-state (named-scalar) element, or a
// marshaled contiguous svBitVecVal/svLogicVecVal buffer for a packed element.
typedef struct rt_svopen {
    void*    data;
    int32_t  left;
    int32_t  right;
    uint32_t nelem;
    uint32_t elem_bytes; // C stride per element (data buffer)
    uint32_t elem_width; // element bit width
    uint32_t is_logic;   // 4-state element (svLogicVecVal) vs 2-state (svBitVecVal)
} rt_svopen;

// The element pack/unpack marshal is emitted inline as LLVM IR at the DPI call site
// (emit_dpi_oa_pack_ir / emit_dpi_oa_unpack_ir), operating on the native iW/i2W
// design slot at any width — there is no C-runtime marshal. A named-scalar 2-state
// element needs no marshal at all: its narrow `[N x iW]` storage is already the C
// array (zero-copy), and the descriptor points straight at it.

} // extern "C"
