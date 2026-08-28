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

// -- $strobe (LRM 21.2.2) --
//
// $strobe defers both evaluating its arguments and printing them to the
// Postponed region of the current time step, just before simulation time
// advances — unlike $display, which does both immediately. The codegen ramps
// (but never resumes) a per-call-site coroutine whose body holds the format
// args' evaluation, and queues its handle here; the backend resumes each
// queued handle once, at the settled-instant point (emit_active_run), which is
// where the deferred evaluation actually happens.

// Queue one $strobe invocation's ramped coroutine handle to run this instant's
// Postponed region.
void rt_strobe_pend(void* handle);

// Pop and return the next queued handle in FIFO order (so same-instant
// $strobe calls print in call order), or null once the queue is empty. Called
// in a loop from the Postponed region.
void* rt_strobe_pop(void);

// -- $monitor (LRM 21.2.2) --
//
// A single persistent monitor. rt_monitor_set registers its body's ramp function
// pointer (replacing any prior one). Each settled instant the Postponed-region
// drain queries rt_monitor_active_ramp and, if non-null, ramps + resumes that
// body, which renders its arguments and calls rt_monitor_emit — which prints only
// when the rendered output changed since the last emit. $monitoron/$monitoroff map
// to rt_monitor_enable.
void  rt_monitor_set(void* ramp);
void  rt_monitor_enable(int32_t on);
void* rt_monitor_active_ramp(void);
void  rt_monitor_emit(int32_t desc, const char* fmt, uint32_t argc, const rt_arg* argv);

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
// is treated identically to $finish. Returns to its caller, which is a JIT'd
// body that then leaves through its own epilogue; the drain loop stops when it
// sees rt_finishing.
//
// Before exiting it prints the exit banner per LRM Table 20-1, selected by
// `arg` (0/1/2): 0 prints nothing, 1 prints "<banner> called at <time>", 2 adds
// a resource-usage line. `banner` is the compile-time "<file>:<line>: <task>"
// prefix; `now` is the current simulation time in design ticks. The banner goes
// to stderr (a simulator diagnostic, not design output; the LRM fixes its
// content but not the stream).
// Prints the banner, sets rt_finishing, and RETURNS: the JIT'd body that called
// it leaves through its own epilogue, so every frame down to the drain loop is
// released by its owner. It used to longjmp instead, which cannot work on
// Windows (longjmp there unwinds, and JIT'd frames carry no unwind data) and
// corrupted the heap elsewhere -- docs/design/cooperative-finish.md.
void rt_finish_coop(const char* banner, uint64_t now, int32_t arg);

// Non-zero once a $finish / $stop has been taken. The JIT loads this after
// resuming a process to leave the drain loop, the same way it loads
// rt_vcd_active to gate a touch.
extern unsigned char rt_finishing;

// Queue a unique/priority case violation report for the Observed region (LRM
// 12.4.2 / 12.5.3, 4.4.2.4). The report is not emitted at the point of
// execution (the Active region) but recorded and flushed by rt_observed_flush
// once the instant settles. `banner` is the compile-time "<file>:<line>: "
// prefix (static lifetime); `now` is the simulation time of the violation, in
// design ticks; `kind` selects the message: 0 = no case item matched (priority
// or unique, no default), 1 = more than one case item matched (unique /
// unique0 overlap).
void rt_observed_case_violation(const char* banner, uint64_t now, int32_t kind);

// Emit and clear every Observed-region report queued this instant. Called by
// the sim loop at the settled point (between the NBA-settle and Postponed
// regions) and by rt_finish (a $finish ends the step before that point). A
// no-op when nothing is queued.
void rt_observed_flush(void);


// Report a severity system task ($info / $warning / $error / $fatal, LRM 20.10)
// or an immediate-assertion default failure (LRM 16.3). Renders the $display-
// style message (`fmt` + `argc` `argv`, empty for a bare `$error;`) and writes
// "<LABEL>: <banner><message>\n" to stderr, then returns — non-fatal. `level` is
// the ir::Severity label (0 info, 1 warning, 2 error, 3 fatal); it selects the
// LABEL only. `banner` is the compile-time "<file>:<line>: " prefix. $fatal's
// termination is a separate rt_finish the caller emits after this report.
void rt_severity(int32_t level, const char* banner, const char* fmt, uint32_t argc, const rt_arg* argv);

// Queue a deferred immediate assertion report (LRM 16.4) for maturation in the
// Observed (`is_final` == 0) or Postponed (`is_final` != 0) region. The message
// is rendered now (arguments sampled at the Active-region execution point);
// `level` is the ir::Severity label, `banner` the "<file>:<line>: " prefix.
// `gen` is the owning process's generation-counter address and `gen_val` its
// value at queue time: at maturation the report fires only if `*gen` still
// equals `gen_val`, i.e. the process has not re-activated (flushed) since.
void rt_defer_report(int32_t level, uint64_t is_final, const void* gen, uint64_t gen_val,
                     const char* banner, const char* fmt, uint32_t argc, const rt_arg* argv);

// Mature and clear the queued observed / final deferred assertion reports.
// Called by the sim loop at the Observed / Postponed points, and by rt_finish.
void rt_defer_flush_observed(void);
void rt_defer_flush_final(void);

// What kind of indexing an instrumented access performs. Selects the wording of
// the report and nothing else; the check is the same shape in every case.
typedef enum rt_bounds_what {
    // Element index into an unpacked array of any kind (LRM 7.4.5).
    RT_BOUNDS_ELEMENT = 0,
    // Single-bit select of a packed vector (LRM 11.5.1).
    RT_BOUNDS_BIT = 1,
    // Indexed part-select `[i +: w]` / `[i -: w]` of a packed vector, which can
    // straddle the boundary and be only partially out of range (LRM 11.5.1).
    RT_BOUNDS_PART = 2,
    // Character index into a string (LRM 6.16.2 / 6.16.3).
    RT_BOUNDS_CHAR = 3,
    // Index argument of a queue method: insert / delete
    // (LRM 7.10.2.2 - 7.10.2.3).
    RT_BOUNDS_QUEUE_OP = 4,
    // pop_front / pop_back on an empty queue (LRM 7.10.2.4 - 7.10.2.5). No
    // index and no bound: the report is the fact itself.
    RT_BOUNDS_EMPTY_POP = 5,
    // Associative-array index that is or contains x or z (LRM 7.8.4 / 7.8.5):
    // invalid outright, not merely out of some range, so (like
    // RT_BOUNDS_EMPTY_POP) there is no bound to report -- x/z is the whole
    // finding.
    RT_BOUNDS_ASSOC_KEY = 6,
} rt_bounds_what;

// One instrumented access, emitted by the backend as a constant global beside
// the code that reports it. Everything here is compile-time; the index, the
// runtime extent, the direction and the time travel as arguments. Its address
// is stable for the run, so it also keys the per-site repeat cap.
//
// It carries TWO positions. The index expression is the primary one -- it is
// the thing that holds the wrong value, so it gets the location line and the
// caret. The indexed expression is secondary, underlined on the same snippet
// line when it shares it, because `q[i]` and `mem[i]` in one statement are told
// apart by which array is named, not by the index.
//
// Field order and types are the emitted LLVM struct's, so the two must change
// together: { ptr, ptr, ptr, i32 x10 }.
typedef struct rt_bounds_site {
    // "<file>:<line>:<col>" of the index expression, or "<unknown>".
    const char* where;
    // The source line the index expression sits on, verbatim and without its
    // newline, for the snippet under the message. Empty when the source is
    // unavailable, in which case no snippet is drawn.
    const char* line;
    // What was indexed: an array's hierarchical name, or "<local>" for a frame
    // local (the backend has no name for one).
    const char* name;
    // 1-based line and column of the index expression within `line`, and one
    // past its last column, so the carets span [column, end_column). 0 when
    // unknown; an end at or below the start means no recorded extent and draws
    // a single caret.
    int32_t line_no;
    int32_t column;
    int32_t end_column;
    // The same three for the expression being indexed. Underlined on the
    // snippet only when `acc_line_no` equals `line_no` -- an index split across
    // lines from the array it indexes gets the carets alone. All 0 for a site
    // with no index of its own (an empty-queue pop), where the two positions
    // are the same thing.
    int32_t acc_line_no;
    int32_t acc_column;
    int32_t acc_end_column;
    // An rt_bounds_what.
    int32_t what;
    // The declared bounds [left:right], as written. Meaningful only when the
    // report carries no runtime extent (see `extent` on rt_bounds_report): a
    // dynamic array, queue or string is bounded by its current size instead.
    int32_t left;
    int32_t right;
    // Selected width, for RT_BOUNDS_PART; 1 everywhere else.
    int32_t width;
} rt_bounds_site;

// Report an access whose index missed the bounds of what it indexed, found by
// `--sanitize=bounds`. Not an LRM event: the access still behaves exactly as
// the LRM says (7.4.5 / Table 7-1 for an array element, 11.5.1 for a packed
// select, 6.16.2-6.16.3 for a string character), and this only makes that
// silent outcome audible. Non-fatal — it prints and returns, so the design
// keeps running with unchanged semantics.
//
// `index` is the index as the design wrote it, in the indexed object's own
// coordinates and with its own signedness, so an index below the low bound
// reads as the negative it is. `extent` is the current size of a runtime-sized
// object (dynamic array, queue, string) and the bound is then [0, extent);
// pass -1 for a statically bounded one, whose bounds are in the site record.
// `flags` is a mask: bit 0 set = write, clear = read; bit 1 set = the index
// carried x/z bits, in which case `index` means nothing and is reported as x;
// bit 2 set = only part of a part-select fell outside. `now` is the simulation
// time in design ticks.
//
// Repeats are capped per site: a bad index inside an always block re-fires on
// every activation, and the first reports carry all the information the rest
// would bury.
void rt_bounds_report(const rt_bounds_site* site, int64_t index, int64_t extent, uint32_t flags,
                      uint64_t now);

// -- Runtime-offset bit / part select of a very wide vector (LRM 11.5.1) --

// Read (rt_wide_sel) or write (rt_wide_ins) the `n` bits at storage bit offset
// `off` of a `width`-bit slot, clamped to the container [base, base+cwidth) the
// select sits in. `four` selects the packed 4-state layout (must in bits [0,width),
// may in [width,2*width) of `mm`, z a separate `width`-bit field); `has_z` says
// whether the z field exists. `unknown` marks an x/z index, which 11.5.1 makes
// wholly out of range: the read is all-x and the write a no-op. Bits outside the
// container read x (2-state: 0) and are never written.
//
// The selection's own planes are little-endian word buffers of ceil(n/64) words,
// matching the in-memory layout of an i(n), so the JIT spills / reloads them
// directly. Storage planes are the same layout at `width` / 2*`width` bits.
//
// These index the containing 64-bit word rather than shifting the whole vector,
// which is what keeps a dynamic select on a 65 Kbit vector from taking O(width^2)
// JIT time (see the comment on the definitions).
void rt_wide_sel(const void* mm, const void* z, uint32_t width, int64_t base, uint32_t cwidth,
                 int64_t off, uint32_t n, uint32_t four, uint32_t has_z, uint32_t unknown,
                 uint64_t* out_must, uint64_t* out_may, uint64_t* out_z);
void rt_wide_ins(void* mm, void* z, uint32_t width, int64_t base, uint32_t cwidth, int64_t off,
                 uint32_t n, uint32_t four, uint32_t has_z, uint32_t unknown,
                 const uint64_t* must, const uint64_t* may, const uint64_t* z_bits);

// -- Null class-handle dereference (LRM 8.4) --

// The site of one guarded class-handle use — a property dereference or a virtual
// dispatch — emitted by the backend as a constant global beside the guard.
// What kind of handle was null, which decides the wording: a class handle is
// constructed with `new`, a virtual interface is bound to an elaborated instance,
// so the same report would misdirect for the other.
typedef enum rt_null_kind {
    RT_NULL_CLASS = 0,
    RT_NULL_IFACE = 1,
} rt_null_kind;

// Nothing about a null handle is dynamic except the simulation time, so the whole
// record is compile-time. Field order and types mirror the LLVM struct the
// backend builds: { ptr, ptr, i32 x4 }.
typedef struct rt_null_site {
    // "<file>:<line>:<col>" of the handle expression, or "<unknown>" when the
    // location cannot be resolved.
    const char* where;
    // The source line that expression starts on, verbatim and without its
    // newline. Empty when unavailable.
    const char* line;
    // 1-based line and column of the expression's first character; 0 when
    // unknown.
    int32_t line_no;
    int32_t column;
    // 1-based column one past its last character, so the caret spans
    // [column, end_column). Equals `column` when no extent was recorded.
    int32_t end_column;
    // An rt_null_kind.
    int32_t kind;
} rt_null_site;

// Reaching a non-static property, or dispatching a virtual method, through a
// class handle that holds no object. LRM 8.4 calls such an access illegal,
// leaves its result indeterminate, and lets an implementation issue an error;
// the only other outcome available here is a load from address zero, which
// faults with nothing attributing it to the design. So this prints a located
// diagnostic and ends the simulation with a nonzero status. Never returns.
[[noreturn]] void rt_null_handle(const rt_null_site* site, uint64_t now);

// -- A `ref` whose actual's container was restructured under it (LRM 13.5.2) --

// The site of one guarded `ref` dereference, emitted by the backend as a
// constant global beside the check. Everything except the two epoch values is
// compile-time. Field order and types mirror the LLVM struct the backend
// builds: { ptr, ptr, i32 x3, ptr, ptr }.
typedef struct rt_ref_site {
    // "<file>:<line>:<col>" of the dereference, or "<unknown>".
    const char* where;
    // The source line that dereference starts on, verbatim, no newline.
    const char* line;
    // 1-based line / column of the dereference, and one past its last column.
    int32_t line_no;
    int32_t column;
    int32_t end_column;
    // Declared name of the `ref` formal, for the diagnostic text. Never null.
    //
    // No container name: which container went stale is answered by the epoch
    // pointer the guard was checking, through rt_ref_names below, rather than
    // recorded per call site.
    const char* formal;
} rt_ref_site;

// One container's relocation epoch and the name it was declared under.
typedef struct rt_container_name {
    const void* epoch;
    const char* name;
} rt_container_name;

// Register the design's epoch-to-name table, once, before it runs. The backend
// emits one entry per container whose handle sits at a statically known address
// (a module-scope signal, or a local hoisted into design state), which is exactly
// the set whose name a report could ever state: a container inside a class body,
// or inside another container's buffer, has no fixed address to key on and is
// reported without a name rather than under a guessed one.
//
// Consulted only from a staleness report, so the lookup is a linear scan rather
// than an index that would have to be built during startup.
void rt_ref_names(const rt_container_name* table, uint64_t count);

// Dereferencing a `ref` formal whose actual was an element of a container that
// has since been restructured (a push, an insert, a delete, a resize, a whole
// -value assignment, a sort). The interior pointer the ref holds no longer names
// that element, and may point outside the buffer entirely, so LRM 13.5.2's
// "the formal refers to the actual" no longer holds.
//
// The LRM does not define this case. Rather than dereference the stale pointer,
// Clotho reports it: prints a located diagnostic naming the formal, the
// container and both epochs, and ends the simulation with a nonzero status.
// `--loose-stale-ref` downgrades it to a once-per-site warning and returns,
// which is the escape hatch for a design that relies on the stale-pointer
// behaviour.
//
// `epoch` is the pointer the guard was watching; the report resolves it to the
// container's declared name through the rt_ref_names table. Passing it costs
// nothing at the call that formed the ref — it is already live here, on a path
// taken once per run at most.
void rt_ref_stale(const rt_ref_site* site, const void* epoch, uint64_t expect, uint64_t actual,
                  uint64_t now);

// A ref's expected epoch ahead of its container's. Epochs are monotone and only
// ever advance, so this is unreachable without a codegen bug in the epoch
// plumbing itself. Aborts unconditionally — `--loose-stale-ref` does not apply,
// because the invariant being violated is Clotho's, not the design's.
[[noreturn]] void rt_ref_epoch_regress(const rt_ref_site* site, const void* epoch,
                                       uint64_t expect, uint64_t actual, uint64_t now);

// Select the stale-ref mode: nonzero downgrades rt_ref_stale to a once-per-site
// warning that returns, zero (the default) leaves it fatal. Owned by the CLI's
// --loose-stale-ref and set once before the design runs. Does not affect
// rt_ref_epoch_regress, which aborts in either mode.
void rt_ref_stale_warn(int32_t enable);

// -- Tagged-union member access inconsistent with the current tag (LRM 11.9) --

// The site of one guarded tagged-union member access, emitted by the backend as
// a constant global beside the guard. Everything except the tag the union
// actually held is compile-time. Field order and types mirror the LLVM struct
// the backend builds: { ptr, ptr, i32 x3, ptr, ptr, ptr, i32, i32 }.
typedef struct rt_tagged_site {
    // "<file>:<line>:<col>" of the member access, or "<unknown>".
    const char* where;
    // The source line that access starts on, verbatim and without its newline.
    // Empty when unavailable.
    const char* line;
    // 1-based line and column of the access's first character; 0 when unknown.
    int32_t line_no;
    int32_t column;
    // 1-based column one past its last character, so the caret spans
    // [column, end_column). Equals `column` when no extent was recorded.
    int32_t end_column;
    // Name of the member the design reached for.
    const char* member;
    // Every member name, indexed by tag value, so the report can name the tag the
    // union actually holds.
    const char* const* names;
    // Length of each entry of `names`, same order.
    const uint32_t* name_lens;
    // Number of entries in `names` / `name_lens`.
    uint32_t count;
    // 1 when the access was an assignment, 0 for a read.
    uint32_t is_write;
} rt_tagged_site;

// Reading or assigning a tagged-union member that the current tag does not
// select. LRM 11.9: "Members of tagged unions can be read or assigned using the
// usual dot notation. Such accesses are completely type-checked ... An attempt to
// read or assign a value whose type is inconsistent with the tag results in a
// run-time error." `tag` is the value the tag held; `tag_unknown` is 1 when it
// carried x/z (LRM 11.9 leaves an uninitialized tagged union's tag bits
// undefined), in which case `tag` is meaningless. Prints a located diagnostic and
// ends the simulation with a nonzero status. Never returns.
[[noreturn]] void rt_tagged_access(const rt_tagged_site* site, uint64_t tag, uint32_t tag_unknown,
                                   uint64_t now);

// -- Checked heap allocation --
//
// Every heap buffer a design's containers need (dynamic arrays, queues, strings)
// comes from rt_alloc_checked. Three failures used to be silent: a size computed
// by wrapping multiplication (an element count times a stride), a `new[n]` whose
// n arrived zero-extended so a negative count became a huge one, and a malloc
// that simply returned null. All three produced a small-or-null buffer the
// caller then wrote past. They are now diagnosed at the point of allocation.

// Where an allocation came from, emitted by the backend as a constant global
// beside the call it guards. Everything here is compile-time; only the size and
// the simulation time are dynamic. Field order and types mirror the LLVM struct
// the backend builds: { ptr, ptr, ptr, i32, i32, i32 }.
typedef struct rt_alloc_site {
    // "<file>:<line>:<col>" of the statement that allocates, or "<unknown>".
    const char* where;
    // The source line it sits on, verbatim and without its newline. Empty when
    // unavailable.
    const char* line;
    // What is being allocated, as the diagnostic's noun: "dynamic array",
    // "queue", "string", ... Never null.
    const char* what;
    // 1-based line and column of the statement; 0 when unknown.
    int32_t line_no;
    int32_t column;
    // 1-based column one past its last character, so the caret spans
    // [column, end_column). Equals `column` when no extent was recorded.
    int32_t end_column;
} rt_alloc_site;

// The runtime's one heap allocation. Returns `size` bytes, null for a zero-byte
// request (every caller treats null-and-empty as "no buffer"), and never returns
// on failure: an allocation the system refuses ends the simulation with a
// located fatal rather than handing back a null the caller would write through.
// `now` is the simulation time in design ticks, for the report only.
//
// There is deliberately no byte ceiling beyond what size_t can express: a design
// that legitimately wants a huge array should get it if the machine has it.
void* rt_alloc_checked(uint64_t size, const rt_alloc_site* site, uint64_t now);

// `count` elements of `stride` bytes as a byte total, formed with a checked
// multiply -- an element count that overflows size_t is reported as the overflow
// it is, instead of wrapping to a small number the caller then sizes a buffer
// from and writes `count` elements into. Never returns on overflow. Split out
// from rt_alloc_elems_checked for the caller that needs the total before it
// knows whether it will allocate at all (a short string builds in stack
// scratch).
uint64_t rt_alloc_elems_size(uint64_t count, uint64_t stride, const rt_alloc_site* site,
                             uint64_t now);

// `count` elements of `stride` bytes: rt_alloc_elems_size then rt_alloc_checked.
void* rt_alloc_elems_checked(uint64_t count, uint64_t stride, const rt_alloc_site* site,
                             uint64_t now);

// `count` ZEROED elements of `stride` bytes: the same checked multiply and the
// same located fatal as rt_alloc_elems_checked, but calloc rather than malloc.
// For a large request the allocator serves calloc from a fresh anonymous
// mapping, whose pages are already zero and materialize on first touch -- so a
// design that asks for a big array of zero-initialized elements pays for the
// pages it writes rather than for all of them. Writing the zeros eagerly is
// what turns an oversized request the kernel happened to grant into an
// out-of-memory kill of the whole process instead of a diagnosed refusal.
void* rt_zalloc_elems_checked(uint64_t count, uint64_t stride, const rt_alloc_site* site,
                              uint64_t now);

// `new[n]` with a negative n. LRM 7.5.1 gives a dynamic array a size of type
// int, and no meaning to a negative one; clotho refuses rather than reinterpret
// the bits as a 4-billion-element request. Never returns.
//
// The count crosses the ABI as int64_t, so a design writing a 64-bit UNSIGNED
// count of 2^63 or more lands here too and is reported as the negative those
// bits spell. Both are refused, only the wording differs, and the smallest such
// request is eight exabytes of elements -- not worth a second ABI bit to tell
// apart. Every narrower unsigned count zero-extends and is unambiguous.
[[noreturn]] void rt_alloc_negative(int64_t n, const rt_alloc_site* site, uint64_t now);

// Simulation time for a cold diagnostic path, in design ticks. Reads the wheel
// rt_sim_run is currently driving, so an allocation failure can report a time
// without every allocating entry point carrying one across the ABI. Zero outside
// a run (a constant-folding or teardown allocation has no meaningful time).
uint64_t rt_diag_now(void);

// Drive one simulation: run `drain` (the event loop), then `finals` (the
// `final` blocks, LRM 9.2.3). `drain` returns normally whether it ran out of
// work or a $finish stopped it, so the finals run on both paths.
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
// `name_buf`/`name_len` carry the path; `name_fwd` != 0 marks natural byte order
// (a dynamic string), 0 a reversed packed-ASCII vector (as $readmem / $dumpfile).
int32_t rt_fopen_mcd(const char* name_buf, uint32_t name_len, int32_t name_fwd);

// $fopen(name, mode): open `name` with C stdio mode string `mode` and return a
// fresh FD, or 0 on failure. Each of name / mode may be a packed-ASCII vector
// (fwd 0) or a dynamic string (fwd != 0, natural order); `mode` is passed
// straight to fopen, so SV mode strings ("r", "w", "a", "r+b", ...) map directly.
int32_t rt_fopen_fd(const char* name_buf, uint32_t name_len, int32_t name_fwd, const char* mode_buf,
                    uint32_t mode_len, int32_t mode_fwd);

// $fclose(desc): close every stream the descriptor names (all set MCD channels,
// or the one FD). The preopened standard streams are never closed.
void rt_fclose(int32_t desc);

// Set the $system access tier (0 = off, 1 = const, 2 = full) before the design
// runs. A host-side call, not exposed to the JIT. Governs the rt_system
// backstop below.
void rt_system_set_access(int32_t tier);

// $system(cmd): run `cmd` as a shell command via the C system(), returning its
// raw int (a POSIX wait-status) (LRM §20.17.1). `has_arg` == 0 is the no-arg
// $system(), which calls system(NULL) (returns nonzero if a shell exists) and
// ignores `cmd`. `cmd` may be a packed-ASCII vector (fwd 0) or a dynamic string
// (fwd != 0), like $fopen. Aborts loudly if the access tier is off — a backstop
// behind the compile-time --allow-system gate, which normally prevents this
// call from being emitted at all.
int32_t rt_system(const char* cmd_buf, uint32_t cmd_len, int32_t cmd_fwd, int32_t has_arg);

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

// What a format string that only exists at RUN TIME needs from its call site,
// and a compile-time one does not. LRM §21.3.3 exempts $sformat / $sformatf from
// the literal-format rule ("The only exception is $sformat, whose second
// argument can be an expression of string data type"), so their format text may
// first exist as an rt_string. With a literal format the frontend resolves
// everything call-site-dependent at lower time -- Lower.cpp's
// append_format_literal splices %m / %l, scale_to_ticks pre-scales a %t operand,
// build_format picks each argument's conversion -- and none of it reaches here.
// A runtime format leaves all of it to the renderer, which needs these facts.
//
// Null selects the compile-time path: `fmt` then carries nothing the frontend
// left behind, so the renderer takes no extra branch and the descriptors are
// already conversion-matched.
typedef struct rt_fmt_dyn {
    // Design ticks per the calling module's time unit: the multiplier
    // Lower.cpp's scale_to_ticks would have applied to a %t operand (LRM
    // §20.4.3). 1 when the two coincide.
    uint64_t time_mul;
    // The $sformat / $sformatf statement, so a report about the format's own
    // contents can point at the call rather than at the renderer.
    const rt_alloc_site* site;
} rt_fmt_dyn;

// $sformat / $swrite (LRM §21.3.3): format `fmt` with `argc` descriptors (as
// rt_fwrite) and store the result into the destination string variable, whose
// must|may plane is at `mm` (2*`width` bits) and z plane at `z` (`width` bits).
// Stored right-justified; an oversized result keeps its low bytes. `width` must be
// a multiple of 8. `flags` as rt_fgets. `dyn` non-null when `fmt` came from a
// runtime string expression.
void rt_sformat(uint8_t* mm, uint8_t* z, uint32_t width, const char* fmt, uint32_t argc, const rt_arg* argv,
                uint32_t flags, const rt_fmt_dyn* dyn);

// $sformatf / $sformat into a string-object destination: format as rt_fwrite
// and assign the result to the rt_string at `dst` (declared below). `site`
// locates the format call for an allocation-failure report. `dyn` as rt_sformat.
struct rt_string;
void rt_sformat_str(struct rt_string* dst, const char* fmt, uint32_t argc, const rt_arg* argv,
                    const rt_alloc_site* site, const rt_fmt_dyn* dyn);

// $fgets(str, fd) into a `string` destination: no fixed width, so the whole
// line is read (stopping after a newline or at EOF) and assigned to `dst`.
// -> the character count, or 0 on EOF / error, in which case `dst` is left
// unchanged (matches rt_fgets's packed-vector behavior on a 0-byte read).
// `site` locates the read for an allocation-failure report.
int32_t rt_fgets_str(struct rt_string* dst, int32_t fd, const rt_alloc_site* site);

// $ferror(fd, str) (LRM §21.3.6): write the descriptor's most recent error message
// into the string variable at `mm` / `z` (`width` bits) and return its error code
// (0 when the stream is clear). `flags` as rt_fgets.
int32_t rt_ferror(int32_t fd, uint8_t* mm, uint8_t* z, uint32_t width, uint32_t flags);

// $ferror(fd, str) into a `string` destination: same as rt_ferror, but always
// assigns `dst` (even the empty message when the stream is clear). `site`
// locates the call for an allocation-failure report.
int32_t rt_ferror_str(int32_t fd, struct rt_string* dst, const rt_alloc_site* site);

// One output destination of a $fscanf / $sscanf conversion: the must|may plane
// pointer (2*`width` bits), the z plane pointer (`width` bits), and the width. The
// conversion char in the format decides how the parsed value is stored here.
// `flags` bit0: `mm` is a narrow 2-state slot holding the bare iW value — only
// `width` bits are written (x/z parsed digits drop to 0 per LRM 6.3.1) and `z`
// is null.
// `flags` bit1: the destination is a `string` object. `str` / `site` are set and
// the plane fields are null / 0 — the scanned %s token is assigned to the handle
// verbatim (LRM 21.3.4.3 sanctions only %s into a string; the frontend refuses
// every other conversion, so the engine never pairs one with this variant).
typedef struct rt_scan_arg {
    uint8_t* mm;
    uint8_t* z;
    uint32_t width;
    uint32_t flags;
    struct rt_string*           str;
    const struct rt_alloc_site* site;
} rt_scan_arg;

// $fscanf(fd, fmt, args) (LRM §21.3.4): parse the descriptor's input per `fmt`,
// writing each conversion into the matching `args` descriptor. Supports d/o/b/h/x
// (with x/z digits), c, s, `*` suppression and a field width. -> the number of
// stored conversions, or -1 if the input is exhausted before the first.
int32_t rt_fscanf(int32_t fd, const char* fmt, uint32_t argc, const rt_scan_arg* args);

// $sscanf(str, fmt, args): as rt_fscanf, but the source is at `buf` (`len` bytes):
// `fwd` != 0 for a dynamic string (natural order), 0 for a packed-ASCII vector.
int32_t rt_sscanf(const char* buf, uint32_t len, int32_t fwd, const char* fmt, uint32_t argc,
                  const rt_scan_arg* args);

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
// with must == may and no z plane), bit 3 real (the value word is an IEEE-754 bit
// pattern -- `$var real`, dumped as `r<decimal>`; width 64 double / 32 shortreal),
// bit 4 event (`$var event 1`, dumped as the bare pulse `1<id>` at each trigger;
// `mm` points at the event's 64-bit last-trigger tick rather than a value slot,
// and `z` is null).
#define RT_VCD_FLAG_FOUR_STATE 1u
#define RT_VCD_FLAG_NET        2u
#define RT_VCD_FLAG_NARROW     4u
#define RT_VCD_FLAG_REAL       8u
#define RT_VCD_FLAG_EVENT      16u

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
// (any width; wider than 64 bits travels as multiple words), `flags` per
// RT_VCD_FLAG_*, `mm` its packed must|may plane pointer, `z` its z plane pointer
// (null for a never-z / narrow slot). Writes the $var line and adds the variable
// to the scan table.
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

// $dumpoff / $dumpon / $dumpall / $dumpflush (LRM §21.7.1.3-6). off/on/all only
// set a deferred flag serviced by the next rt_vcd_flush, so rt_vcd_flush stays the
// sole writer and the change lands at the correct instant; dumpflush drains the
// output buffer to disk immediately.
void rt_vcd_off(void);
void rt_vcd_on(void);
void rt_vcd_all(void);
void rt_vcd_dumpflush(void);

// $dumplimit (LRM §21.7.1.5): stop dumping once the file reaches `bytes` bytes.
// 0 means unlimited. May be called before or after $dumpvars. The writer checks
// the count at instant boundaries, so the file never ends mid-instant: it holds
// the last whole instant that fit plus a `$comment` recording the cutoff, and
// dumping stays off for the rest of the run ($dumpon / $dumpall do not resume).
void rt_vcd_limit(uint64_t bytes);

// Final flush of the in-progress instant + output buffer, then close the file.
// Called on normal simulation end and when a $finish stops the drain.
void rt_vcd_close(void);

// -- $random --
//
// $random (LRM 1800-2023 §20.15.1): return the next value of the simulator's
// global random stream as a signed 32-bit integer. Clotho implements the
// IEEE Std 1800-2023 Annex N generator (rtl_dist_uniform over the full int32 span)
// from an unseeded global stream starting at seed 0, so the stream is
// deterministic and identical from run to run. The generator is process-global,
// matching the single-simulator-per-process model.
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
// RNG is a per-thread xoshiro256++ generator whose 256-bit state lives in the
// coroutine frame of the process (or spawned child) that owns the thread; a
// function or task draws from its caller's, reached through the call ABI. The
// draw and the $urandom_range mapping are inlined into the JIT (no runtime call
// per draw). The only runtime entry is seeding a state from a 64-bit selector
// (splitmix64 expansion): the process index at startup, one draw of the parent's
// stream for a spawned child, or the user seed for $urandom(seed).
// A modern integer PRNG (not the $random LCG) is chosen for quality and speed
// per the project's "unspecified -> pick performance and simplicity"; the stream is
// therefore intentionally not iverilog-identical.
void rt_urandom_seed(uint64_t* state, uint64_t seed);

// -- randc cyclic randomization (LRM §18.6) --
//
// One cyclic draw for a `randc` property. `seen` is that property's 256-bit
// "already used this cycle" bitmap (4 words, word 0 holding values 0-63) and
// `nvals` is its value count 1 << width, a power of two in [2, 256] (the
// frontend refuses a randc wider than 8 bits). Picks uniformly among the values
// in [0, nvals) whose bit is still clear, sets that bit and returns the value;
// when no bit is clear the permutation has been exhausted, so the bitmap is
// cleared first and a fresh cycle begins -- LRM §18.6's guarantee that no value
// repeats until every one has been drawn.
//
// Draws exactly once, from `rng`: the object's own xoshiro256++ state (LRM
// §18.14.2), not the randomizing process's stream, which is what keeps an
// object's randc sequence stable regardless of which thread randomizes it.
// Unlike the inlined $urandom draw this is a real call -- selecting the i-th
// clear bit is a popcount-and-scan over four words, too much to inline at every
// randc site.
uint32_t rt_randc_next(uint64_t* seen, uint32_t nvals, uint64_t* rng);

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

// $value$plusargs(user_string, var) with a `string`-typed var (LRM §21.6). Only
// the clause's "%s -- string (no conversion)" form reaches here: the remainder
// is the value, so there is no width to pad or truncate to and no illegal
// character to reject. On a match the remainder is assigned to `dst` (empty
// when the plusarg has none, which is the clause's "empty string value") and
// the function returns nonzero; on no match `dst` is left untouched and it
// returns 0. `site` locates the assignment for an allocation-failure report.
int32_t rt_value_plusargs_str(const char* prefix, uint32_t prefix_len, struct rt_string* dst,
                              const rt_alloc_site* site);

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

// -- Container element lifecycle descriptors --
//
// A dynamic array / queue element is described by a constant rt_elem_ops the
// backend materializes per element type. A POD element (plane scalar / real /
// packed vector / class handle) is pure bytes: init is a proto memcpy, copy a
// memcpy, deinit a no-op. A managed element owns heap somewhere (a string, a
// nested container, or - future - an aggregate with such leaves) and recurses
// through the descriptor. Every element type is trivially RELOCATABLE (no
// self-pointers), so the containers move elements by raw memcpy (ring growth,
// insert / delete shifts, pop-out) even when managed; only DUPLICATION and
// destruction dispatch through the descriptor.
typedef struct rt_elem_ops rt_elem_ops;

// One heap-owning leaf of an aggregate element: byte offset of its slot inside
// the element image, and its own descriptor.
typedef struct rt_elem_leaf {
    uint64_t           offset;
    const rt_elem_ops* ops;
} rt_elem_leaf;

// Element kinds, by lifecycle shape.
enum {
    rt_elem_pod       = 0, // raw bytes; proto init, memcpy copy, no deinit
    rt_elem_string    = 1, // one rt_string handle
    rt_elem_dynarr    = 2, // one rt_dynarr handle; `inner` describes ITS element
    rt_elem_queue     = 3, // one rt_queue handle; `inner` describes ITS element
    rt_elem_aggregate = 4, // fixed byte image with `nleaves` heap leaves
    rt_elem_assoc     = 5, // one rt_assoc handle; `inner` describes ITS value element
    rt_elem_event     = 6, // one event handle; init binds a NEW object (Table 6-7)
};

// The four trailing fields are the GC's (docs/design/garbage-collection.md §3.3).
// They are APPENDED, so every existing field keeps its offset and the backend's
// mirror (`elem_ops_ty`, LlvmBackend.cpp) changes in lockstep by growing.
//
// `nhandles` / `handles` describe the class-handle words inside ONE element image,
// by byte offset: a bare class-handle element is `nhandles == 1` at offset 0, an
// aggregate element lists the handles reachable without leaving its image, and a
// nested container element lists none (its handles are found by recursing through
// `inner`).
//
// `nkeyhandles` / `keyhandles` do the same for an associative array's normalized
// KEY bytes, and exist because a key is a strong reference the collector has to
// decode: `m[obj] = 1` keeps `obj` alive and `m.first(k)` hands the handle back, so
// the key bytes are the only copy of it. An offset LIST rather than a flag, because
// an aggregate key is serialized leaf by leaf with no padding (assoc-keys.md §2), so
// a handle can sit at any offset and a struct can carry several. A bare class-handle
// key is the one-entry case, `nkeyhandles == 1` at offset 0. Each listed offset
// names 8 big-endian bytes -- a handle is unsigned, so there is no sign flip to
// undo (emit_key_bytes).
struct rt_elem_ops {
    uint32_t            kind;
    uint32_t            nleaves; // rt_elem_aggregate only
    uint64_t            stride;  // element slot bytes
    const void*         proto;   // power-on byte image, `stride` bytes
    const rt_elem_ops*  inner;   // rt_elem_dynarr / rt_elem_queue / rt_elem_assoc only
    const rt_elem_leaf* leaves;  // rt_elem_aggregate only
    uint32_t            nhandles;
    uint32_t            nkeyhandles; // rt_elem_assoc only
    const uint32_t*     handles;
    const uint32_t*     keyhandles;  // rt_elem_assoc only
};

// Visit every live element image of an associative array, in unspecified order,
// passing the normalized key bytes alongside. Exported for exactly one caller --
// the collector's precise walk -- because the map itself (an unordered_dense of
// per-entry SlotVec blocks, Runtime.cpp:2093-2110) is internal to Runtime.cpp and
// its payloads relocate on rehash, so no external code can iterate it by address.
void rt_assoc_visit(const struct rt_assoc* h,
                    void (*visit)(void* elem, const uint8_t* key, uint32_t klen, void* ctx),
                    void* ctx);

// -- The relocation epoch --
//
// Every container handle carries a monotone `epoch`, bumped by each operation
// that can change the mapping from a logical index to a byte address: a
// reallocation, a reindexing insert / delete, a length change, or a whole-value
// replacement. Writing an existing element does NOT bump it.
//
// Its purpose is `ref` safety (docs/design/data-model.md §3). A `ref` whose
// actual is a container element is a fat pointer carrying `&handle->epoch` plus
// the epoch observed when the ref was formed; every dereference in the callee
// re-reads the field and compares. A mismatch means the container was
// restructured while the ref was live, so the interior pointer is dangling —
// reported as a located fatal instead of corrupting memory.
//
// The epoch belongs to the handle rather than a side table so that it survives
// assign / move correctly: the destination's epoch ADVANCES, and is never
// copied from the source. It is deliberately not reset by a delete, so an epoch
// is never reused within a run.
//
// A stable (non-relocatable) place — a module signal, a frame local, a fixed
// unpacked array element, a class property — has no epoch to check, so a ref
// formed through one points at a compile-time zero word and expects 0. That
// keeps the callee's check unconditional rather than paying a null test, means
// such a ref can never be reported stale, and lets the optimizer fold the check
// away entirely when it can see the caller. That zero word is emitted by the
// backend (`clotho.ref.noepoch`) rather than exported from here.

// NOTE: the element lifecycle helpers (init / deinit / copy over an
// rt_elem_ops descriptor) are internal to Runtime.cpp — the backend inlines
// the element ops at call sites and never calls them, so they have internal
// linkage and no declaration here.

// -- SystemVerilog dynamic arrays --
//
// The dynamic array type (LRM §7.5): a runtime-sized vector of a fixed-width
// element. The value lives in a 24-byte caller-owned handle embedded in design
// state (same ownership model as rt_string): `data` points at one heap buffer of
// `len` elements, each `stride` bytes (the backend's per-element slot size),
// malloc-owned by the handle. Copies are eager deep copies; handles never share
// storage. A handle powers on zeroed — { null, 0, 0 } is the empty array — and is
// released with rt_dynarr_delete. The per-element record layout is defined by the
// backend; the runtime owns only the raw byte buffer.
typedef struct rt_dynarr {
    void*    data;
    uint64_t len;
    // Relocation epoch: bumped by new / new_copy / delete / assign / move and by
    // reverse / sort / shuffle (which reindex without reallocating).
    uint64_t epoch;
} rt_dynarr;

// arr = new[n]: (re)allocate `h` to `n` elements, constructing each from the
// descriptor's proto. Destroys the prior elements and frees the buffer. `n` is
// signed because LRM §7.5.1 gives the size expression a signed type; a negative
// `n` is a fatal rather than the huge unsigned request its bits would name.
// `site` is where the `new[]` sits in the source, for the failure report.
void rt_dynarr_new(rt_dynarr* h, const rt_elem_ops* ops, int64_t n, const rt_alloc_site* site);

// arr = new[n](src): like rt_dynarr_new, but the low min(n, src->len) elements
// are first deep-copied from `src`; any remaining take the proto (LRM §7.5.1).
// `src` may alias `h` (a self-resize). `n` is signed and a negative one is a
// fatal, and `site` locates the allocation, both as for rt_dynarr_new.
void rt_dynarr_new_copy(rt_dynarr* h, const rt_elem_ops* ops, int64_t n, const rt_dynarr* src,
                        const rt_alloc_site* site);

// arr.delete(): destroy every element, free the buffer, reset to the empty
// array. Idempotent.
void rt_dynarr_delete(rt_dynarr* h, const rt_elem_ops* ops);

// dst = src: deep copy `src`'s elements into `dst` (value semantics, LRM
// §7.5). A no-op when dst == src. `site` is the assignment's source location,
// for the report if the copy cannot be allocated.
void rt_dynarr_assign(rt_dynarr* dst, const rt_dynarr* src, const rt_elem_ops* ops,
                      const rt_alloc_site* site);

// Move `src`'s buffer into `dst` (ownership transfer): destroy `dst`'s elements
// and buffer, take `src`'s, leave `src` empty. Both epochs advance. The
// nonblocking-assignment commit path, where the RHS was already deep-copied
// into `src` in the Active region. A no-op when dst == src.
void rt_dynarr_move(rt_dynarr* dst, rt_dynarr* src, const rt_elem_ops* ops);

// -- Array ordering methods (LRM 7.12.2) --
//
// `kind` selects the sort comparator over an element's `width`-bit value:
// 0 = unsigned integral, 1 = signed integral, 2 = IEEE real (double),
// 3 = unsigned 4-state integral, 4 = signed 4-state integral. The frontend
// restricts sort()/rsort() to an integral (width <= 64) or real element;
// reverse() is element-agnostic (whole-slot move) and takes no kind.
//
// The 4-state kinds read the slot's must/may planes and order under clotho's
// total order for unknowns (docs/design/array-methods.md): an element with any
// x or z bit precedes every defined element ascending, all unknowns are mutually
// equal, and the sort is stable so their input order survives. `rev` orders
// descending directly there rather than reversing an ascending sort, which would
// flip that one tie group.
//
// All three take the element descriptor rather than a bare stride: they reindex
// the container, so they bump the relocation epoch, and taking `ops` keeps every
// container entry point on one signature shape.

// arr.reverse(): reverse the element order in place.
void rt_dynarr_reverse(rt_dynarr* h, const rt_elem_ops* ops);

// arr.sort() (rev == 0) / arr.rsort() (rev == 1): order elements ascending /
// descending by value.
void rt_dynarr_sort(rt_dynarr* h, const rt_elem_ops* ops, uint32_t kind, uint32_t width,
                    uint32_t rev);

// arr.shuffle() (LRM 7.12.2): randomize element order in place via `rng`, the
// process's 256-bit xoshiro256++ frame state (LRM 18.13); advances that state.
void rt_dynarr_shuffle(rt_dynarr* h, const rt_elem_ops* ops, uint64_t* rng);

// -- SystemVerilog queues --
//
// The queue type (LRM §7.10): a runtime-sized double-ended vector of a
// fixed-width element. The value lives in a 40-byte caller-owned handle embedded
// in design state (same ownership model as rt_string / rt_dynarr): `base` points
// at one heap ring buffer of `cap` elements, each `stride` bytes; the `len` live
// elements occupy physical slots (head + i) mod cap for i in [0, len). Copies are
// eager deep copies; handles never share storage. A handle powers on zeroed —
// { null, 0, 0, 0, 0 } is the empty queue — and is released with
// rt_queue_delete_all. The per-element record layout is defined by the backend;
// the runtime owns only the raw byte buffer.
typedef struct rt_queue {
    void*    base;
    uint64_t head;
    uint64_t len;
    uint64_t cap;
    // Relocation epoch: bumped by every push / pop / insert / delete / assign /
    // move / append_range and by reverse / sort / shuffle. A ring grow relocates
    // the buffer and an insert or delete reindexes it, and even a push that fits
    // changes `len` — hence the meaning of `q[$]` — so every mutation bumps.
    uint64_t epoch;
} rt_queue;

// Bounded-queue max size argument for the push helpers: `bound` is the `[$:N]`
// maximum index (max size N+1); 0 means unbounded (LRM §7.10.4).
//
// An overflowing push keeps the first `bound + 1` elements, i.e. it always drops
// from the TAIL. The two directions therefore lose different elements, and the
// difference is deliberate rather than incidental:
//   - push_front lands the new element at the head, so the dropped one is the
//     old tail -- the end opposite the push.
//   - push_back lands the new element at the tail, so the dropped one is the
//     element just pushed: pushing onto a full bounded queue has no effect.
// The same rule governs insert(). LRM 7.10.4 does not pin which end an
// overflowing push sacrifices.
//
// q.push_back(e) / q.push_front(e): append `elem` (a `stride`-byte element slot)
// to the tail / head, growing the ring as needed. `site` is the push's source
// location, for the report if the ring cannot be grown.
void rt_queue_push_back(rt_queue* h, const rt_elem_ops* ops, uint64_t bound, const void* elem,
                        const rt_alloc_site* site);
void rt_queue_push_front(rt_queue* h, const rt_elem_ops* ops, uint64_t bound, const void* elem,
                         const rt_alloc_site* site);

// q.pop_back() / q.pop_front(): remove the tail / head element and RELOCATE it
// into `out` (a raw element slot whose prior contents must be unconstructed or
// already destroyed - ownership of the element's heap moves to `out`). On an
// empty queue `out` is left untouched (the caller pre-fills the default).
void rt_queue_pop_back(rt_queue* h, const rt_elem_ops* ops, void* out);
void rt_queue_pop_front(rt_queue* h, const rt_elem_ops* ops, void* out);

// q.insert(i, e): insert `elem` before position `i` (0 <= i <= len); out-of-range
// `i` is ignored. Honours the bound like a push. `site` locates the insert for
// an allocation-failure report.
void rt_queue_insert(rt_queue* h, const rt_elem_ops* ops, uint64_t bound, uint64_t i,
                     const void* elem, const rt_alloc_site* site);

// q.delete(i): remove the element at position `i` (0 <= i < len); out-of-range
// `i` is ignored.
void rt_queue_delete_idx(rt_queue* h, const rt_elem_ops* ops, uint64_t i);

// q.delete(): free the buffer and reset to the empty queue. Idempotent.
void rt_queue_delete_all(rt_queue* h, const rt_elem_ops* ops);

// dst = src: deep copy `src`'s elements into `dst`, linearised so dst->head
// == 0 (value semantics, LRM §7.10). A no-op when dst == src. `site` is the
// assignment's source location, for the report if the copy cannot be allocated.
void rt_queue_assign(rt_queue* dst, const rt_queue* src, const rt_elem_ops* ops,
                     const rt_alloc_site* site);
void rt_queue_assign_dynarr(rt_queue* dst, const rt_dynarr* src, const rt_elem_ops* ops,
                            const rt_alloc_site* site);

// dst = src: deep copy a QUEUE's elements into a dynamic array (LRM 7.5.1's
// resizeable assignment). `src` is ring-indexed (rt_queue), `dst` linear
// (rt_dynarr); the mirror of rt_queue_assign_dynarr above. `site` is the
// assignment's source location, for the report if the copy cannot be
// allocated.
void rt_dynarr_assign_queue(rt_dynarr* dst, const rt_queue* src, const rt_elem_ops* ops,
                            const rt_alloc_site* site);

// Move `src`'s buffer into `dst` (ownership transfer, LRM 7.10): destroy `dst`'s
// elements + buffer, take `src`'s, leave `src` empty. The nonblocking-assignment
// commit path (RHS deep-copied into `src` in the Active region). No-op if equal.
void rt_queue_move(rt_queue* dst, rt_queue* src, const rt_elem_ops* ops);

// Append `src`'s live elements at logical indices [lo, hi] (inclusive) onto
// `dst`, respecting `bound`. Materializes a queue slice `q[a:b]` (LRM §7.10.4)
// and the queue-typed operands of a concatenation `{q, e}` (a full-queue append
// passes lo = 0, hi = INT64_MAX; the range is runtime-clamped to [0, len-1], and
// lo > hi appends nothing). `dst` MUST NOT alias `src` — callers build into a
// fresh temp handle, so a self-referential `q = {q, e}` is safe. `site` locates
// the slice or concatenation for an allocation-failure report.
void rt_queue_append_range(rt_queue* dst, const rt_queue* src, const rt_elem_ops* ops,
                           uint64_t bound, int64_t lo, int64_t hi, const rt_alloc_site* site);

// Render a queue (is_queue != 0) or dynamic array as `%p` text `'{e0, ...}`
// (LRM 21.2.1.6) into `out`. `kind`: 0 integral, 1 real, 2 string element. An
// integral slot is `must | (may << width)` plus an optional z plane at `z_off`
// (a 2-state slot is the bare value); element `width` <= 64. The frontend
// guards non-POD / wider elements. `site` locates the format for an
// allocation-failure report.
void rt_container_format_p(rt_string* out, const void* handle, int32_t is_queue, int32_t kind,
                           uint32_t width, uint32_t is_signed, uint32_t four_state, uint32_t has_z,
                           uint32_t z_off, uint64_t stride, const rt_alloc_site* site);

// Render a tagged union as `%p` assignment-pattern text (LRM 21.2.1.6: "In the
// case of a tagged union, it shall print "tag:value" along with the currently
// valid element") into `out`. `v` carries the whole flat {tag, gap, payload}
// value's planes; `width` is its total width and `tag_bits` its high discriminant
// bits. `names` / `name_lens` / `widths` / `signs` are the per-member table in tag
// order, where a width of 0 marks a `void` member, printed as its tag with no
// `:value`, and `signs` selects a signed decimal payload. A tag that is x/z or
// names no member has no valid element to print, so the whole flat value renders
// as one decimal leaf instead.
void rt_tagged_format_p(rt_string* out, const rt_arg* v, uint32_t width, uint32_t tag_bits,
                        uint32_t count, const char* const* names, const uint32_t* name_lens,
                        const uint32_t* widths, const uint32_t* signs, const rt_alloc_site* site);

// Logical `==` of two queues / dynamic arrays (LRM 11.4.5): 0 unequal, 1 equal,
// 2 ambiguous (1'bx). Params mirror rt_container_format_p; `!=` is the caller's
// negation. Different sizes are a definite inequality.
uint8_t rt_container_eq(const void* a, const void* b, int32_t is_queue, int32_t kind, uint32_t width,
                        uint32_t is_signed, uint32_t four_state, uint32_t has_z, uint32_t z_off,
                        uint64_t stride);

// q.reverse() / q.sort() / q.rsort() (LRM 7.12.2). `kind` / `width` / `rev` match
// the rt_dynarr_* ordering entry points; operates on the ring's logical order.
void rt_queue_reverse(rt_queue* h, const rt_elem_ops* ops);
void rt_queue_sort(rt_queue* h, const rt_elem_ops* ops, uint32_t kind, uint32_t width,
                   uint32_t rev);
void rt_queue_shuffle(rt_queue* h, const rt_elem_ops* ops, uint64_t* rng);

// -- Associative-array key scratch buffer (docs/design/assoc-keys.md P1) --
//
// A FIXED key (every leaf integral / handle-shaped) normalizes into a
// compile-time-sized entry-block alloca with no runtime call, unchanged from
// before this existed. A VARIABLE key -- one containing a string / queue /
// dynamic-array / associative-array leaf -- has no such bound, so the backend
// builds it here instead: a growable buffer with mark/pop scoping, since key
// construction nests (an assoc-bearing key iterates a map whose own entries
// are themselves normalized spans). Process-global, like every other piece of
// clotho's single-threaded simulation state.

// Returns a mark: the buffer's current length, to `end` back to later.
uint32_t rt_keybuf_begin(void);

// Appends `len` bytes to the buffer's tail.
void rt_keybuf_append(const void* bytes, uint32_t len);

// The bytes appended since `mark`: a span into the buffer's storage, valid
// until the next rt_keybuf_append (which may reallocate) or a `rt_keybuf_end`
// that pops back over it.
void rt_keybuf_span(uint32_t mark, const uint8_t** out_ptr, uint32_t* out_len);

// Pops the buffer back to `mark`, discarding everything appended since. Does
// not free the underlying storage, so a span already read via rt_keybuf_span
// stays valid in memory (though logically popped) until the next append.
void rt_keybuf_end(uint32_t mark);

// Whole-associative-array equality (LRM 11.4.5 / 7.9, P4a): 1 if two spans are
// byte-identical, else 0. The backend builds each side's span with the P3
// assoc-key canonical serializer (count || sorted (key, value) pairs), so
// byte identity here is exactly map-content equality.
uint8_t rt_key_span_eq(const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen);

// -- SystemVerilog associative arrays --
//
// The associative-array type (LRM §7.8/7.9): a runtime-sized key->value map. The
// value lives in a 16-byte caller-owned handle embedded in design state (same
// ownership model as the other containers): `impl` is null for the empty map,
// else points at a heap AssocMap (an ankerl map of normalized-key-bytes ->
// element slot bytes, plus a lazily-rebuilt sorted key index for ordered
// traversal). Keys reach the runtime as normalized order-preserving byte spans
// built by the backend (docs/design/dynamic-arrays.md §4a) — the runtime is
// key-kind-agnostic: it hashes the span, compares with memcmp, and sorts
// lexicographically. Copies are eager deep copies; handles never share storage.
typedef struct rt_assoc {
    void* impl;
    // Relocation epoch: bumped by an entry-creating put / default_ptr and by
    // delete_key / delete_all / reset / assign / move. An element slot's address
    // is stable while its entry lives (the value buffer is separately owned), so
    // a plain overwrite of an existing key does not bump.
    uint64_t epoch;
} rt_assoc;

// a[key] read: pointer to the element slot for `key`; when the key is absent,
// the pattern default slot if one was set (LRM 7.9.11), else null (LRM 7.8: a
// read of an absent key does NOT create it — the backend materializes the
// element default). The pointer is valid until the next mutation of `h`.
void* rt_assoc_get_ptr(rt_assoc* h, const uint8_t* kptr, uint32_t klen);

// a[key] write: pointer to the element slot for `key`, creating (and proto-
// initializing) the entry if absent (LRM 7.8). Valid until the next mutation.
// `site` locates the write for an allocation-failure report. A created entry
// starts from the element default, NOT from the pattern default (see
// docs/design/dynamic-arrays.md §8).
void* rt_assoc_put_ptr(rt_assoc* h, const rt_elem_ops* ops, const uint8_t* kptr, uint32_t klen,
                       const rt_alloc_site* site);

// Writable pointer to the array's pattern default slot (LRM 7.9.11's
// `'{default: v}`), creating and proto-initializing it on first use. The default
// is per-array state, not an entry: it is invisible to num/exists/first/next and
// survives a.delete(). Valid until the next mutation of `h`.
void* rt_assoc_default_ptr(rt_assoc* h, const rt_elem_ops* ops, const rt_alloc_site* site);

// a.exists(key) -> 0/1 (LRM 7.9.1).
int rt_assoc_exists(rt_assoc* h, const uint8_t* kptr, uint32_t klen);

// a.delete(key): remove one entry if present (LRM 7.9.2). Idempotent.
void rt_assoc_delete_key(rt_assoc* h, const rt_elem_ops* ops, const uint8_t* kptr, uint32_t klen);

// a.delete(): destroy every entry, num -> 0. Keeps the pattern default, which
// LRM 7.9.11 makes independent of the array's contents. Idempotent.
void rt_assoc_delete_all(rt_assoc* h, const rt_elem_ops* ops);

// Destroy every entry AND the pattern default, resetting `h` to the empty map.
// Whole-array assignment (which replaces the default too) and teardown.
void rt_assoc_reset(rt_assoc* h, const rt_elem_ops* ops);

// a.num() / a.size(): entry count (LRM 7.9.5).
uint64_t rt_assoc_num(rt_assoc* h);

// a.first(k) / a.last(k) (LRM 7.9.3): write the min / max key's normalized span
// to *out_ptr / *out_len, return 1; return 0 for an empty map. The span points
// into the map's key storage — valid until the next mutation.
int rt_assoc_first(rt_assoc* h, const uint8_t** out_ptr, uint32_t* out_len);
int rt_assoc_last(rt_assoc* h, const uint8_t** out_ptr, uint32_t* out_len);

// a.next(k) / a.prev(k) (LRM 7.9.4): given the current key span, write the
// smallest key strictly greater (next) / largest strictly less (prev) to
// *out_ptr / *out_len, return 1; return 0 past the end. `cur` need not be a
// present key. The span points into the map's key storage.
int rt_assoc_next(rt_assoc* h, const uint8_t* cur_ptr, uint32_t cur_len, const uint8_t** out_ptr,
                  uint32_t* out_len);
int rt_assoc_prev(rt_assoc* h, const uint8_t* cur_ptr, uint32_t cur_len, const uint8_t** out_ptr,
                  uint32_t* out_len);

// dst = src: deep copy `src`'s entries into `dst` (value semantics, LRM §7.9).
// A no-op when dst == src. `site` locates the assignment for an
// allocation-failure report.
void rt_assoc_assign(rt_assoc* dst, const rt_assoc* src, const rt_elem_ops* ops, const rt_alloc_site* site);

// Move `src`'s map into `dst` (ownership transfer): destroy `dst`'s entries and
// pattern default, take `src`'s map, leave `src` empty. Both epochs advance. The
// nonblocking-assignment commit path. A no-op when dst == src.
void rt_assoc_move(rt_assoc* dst, rt_assoc* src, const rt_elem_ops* ops);

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

// dst = src (deep copy). dst == src is a no-op. `site` locates the assignment
// for an allocation-failure report.
void rt_string_assign(rt_string* dst, const rt_string* src, const rt_alloc_site* site);

// Move `src` into `dst` (ownership transfer): release `dst`'s buffer, take
// `src`'s handle image, leave `src` a valid empty string. The
// nonblocking-assignment commit path, where the RHS was already deep-copied
// into `src` in the Active region. A no-op when dst == src.
//
// A string handle has no epoch: it is not indexable by a stable identity, so no
// interior pointer outlives an operation on it (a character `ref` is not
// expressible in SV).
void rt_string_move(rt_string* dst, rt_string* src);

// dst = the `n` bytes at `p`. `p` is a raw byte sequence, not NUL-terminated;
// it may alias dst's own buffer. `site` locates the assignment for an
// allocation-failure report.
void rt_string_assign_bytes(rt_string* dst, const char* p, uint64_t n, const rt_alloc_site* site);

// Length in bytes ($.len()). The empty string has length 0.
uint64_t rt_string_len(const rt_string* s);

// Pointer to the string's bytes, NUL-terminated at byte[len] so it is a valid C
// string (length via rt_string_len; embedded NULs make the C view stop early).
// Valid until the next mutation of `s`.
const char* rt_string_data(const rt_string* s);

// dst = parts[0] .. parts[n-1] concatenated. dst may appear among the parts
// (a = {a, b}); when its first occurrence is parts[0] the call appends in
// place with amortized growth, so repeated a = {a, chunk} is linear overall.
// `site` locates the concatenation for an allocation-failure report.
void rt_string_concat(rt_string* dst, const rt_string* const* parts, uint32_t n, const rt_alloc_site* site);

// dst = src repeated `count` times ({count{src}}); count == 0 yields "". The
// count crosses the ABI SIGNED (like rt_dynarr_new's): the multiplier may be a
// RUNTIME value under the LRM 11.4.12.1 string deviation (Lower.cpp's
// Replication case), and a negative one must be seen as the negative the design
// wrote, not zero-extended into a huge request. Negative is refused with
// rt_string_repl_invalid. `site` locates the replication for the reports.
void rt_string_repl(rt_string* dst, const rt_string* src, int64_t count, const rt_alloc_site* site);

// A runtime replication multiplier holding an illegal value: negative
// (unknown == 0, `count` holds it) or x/z (unknown != 0, `count` is
// meaningless). LRM 11.4.12.1 requires a non-negative, non-x, non-z
// multiplier; zero is its one blessed edge case, so these two have no defined
// result and end the run with the located-fatal block (headline, source caret,
// simulation time). The x/z caller is JIT code (only it sees the planes); the
// negative caller is rt_string_repl itself.
[[noreturn]] void rt_string_repl_invalid(int64_t count, uint32_t unknown, const rt_alloc_site* site,
                                         uint64_t now);

// Lexicographic comparison in strcmp order over (bytes, then length): returns
// < 0, 0, or > 0. The LRM (§6.16.9) specifies only the sign.
int32_t rt_string_cmp(const rt_string* a, const rt_string* b);

// rt_string_cmp over ASCII-lowercased operands ($.icompare()).
int32_t rt_string_icmp(const rt_string* a, const rt_string* b);

// dst = src.substr(i, j): the bytes at positions i..j inclusive, or "" when the
// range is invalid (i < 0, j < i, or j >= len; LRM §6.16.8). `site` locates the
// substr call for an allocation-failure report.
void rt_string_substr(rt_string* dst, const rt_string* src, int32_t i, int32_t j, const rt_alloc_site* site);

// dst = src with ASCII letters upper/lowercased. dst may alias src. `site`
// locates the call for an allocation-failure report.
void rt_string_toupper(rt_string* dst, const rt_string* src, const rt_alloc_site* site);
void rt_string_tolower(rt_string* dst, const rt_string* src, const rt_alloc_site* site);

// dst = the source name of the enum member whose value equals `val`, or "" if
// none matches (LRM §6.19.2 e.name()). `val` is the operand's must-plane bytes
// (`vbytes` = (width+7)/8) and `values` is the `count` member values packed
// contiguously (`vbytes` each); `names` / `lens` are the parallel name table.
// Byte-compared, so a base wider than 64 bits works. With `fmt_fallback` set, a
// value matching no member instead renders as a decimal leaf of `width` bits,
// signed when `is_signed` — LRM §21.2.1.6's rule for `%p` of an enum ("according
// to the base type"), against §6.19.2's "" for `.name()`.
// `site` locates the name() call for an allocation-failure report.
void rt_enum_name(rt_string* dst, const uint8_t* val, uint32_t vbytes, uint32_t count, const uint8_t* values,
                  const char* const* names, const uint32_t* lens, uint32_t width, uint32_t fmt_fallback,
                  uint32_t is_signed, const rt_alloc_site* site);

// s[i] = c ($.putc()): a no-op when i is out of range or (c & 0xff) == 0 — a
// string never contains a 0 byte (LRM §6.16.2).
void rt_string_putc(rt_string* s, int32_t i, uint32_t c);

// s[i] ($.getc()): the byte at i, or 0 when i is out of range (LRM §6.16.3).
uint32_t rt_string_getc(const rt_string* s, int32_t i);

// string'(value): `val` is the little-endian val plane of a packed value,
// plane_bytes(width_bits) long. Bytes are taken most-significant first and
// every 0x00 byte is dropped (LRM §6.16: the integral value converts as a
// string literal, and literal conversion removes \0 characters). X/Z bits are
// read as 0 — the caller passes the val plane only. `site` locates the cast for
// an allocation-failure report.
void rt_string_from_packed(rt_string* dst, const uint8_t* val, uint32_t width_bits, const rt_alloc_site* site);

// Append `val`'s bytes to `dst`, most significant byte first: one operand of a
// streaming concatenation whose destination is a string (LRM §11.4.14). Byte
// handling is rt_string_from_packed's, dropped 0x00 chunk included (LRM §6.16):
// a stream into a string converts through that same rule, so this and a
// constant-folded `localparam string P = {>>{...}}` agree. X/Z bits read as 0
// (the caller passes the val plane only).
void rt_string_append_packed(rt_string* dst, const uint8_t* val, uint32_t width_bits, const rt_alloc_site* site);

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
// StrFromPacked. `site` locates the conversion for an allocation-failure report.
void rt_string_itoa(rt_string* dst, int32_t i, const rt_alloc_site* site);
void rt_string_hextoa(rt_string* dst, int32_t i, const rt_alloc_site* site);
void rt_string_octtoa(rt_string* dst, int32_t i, const rt_alloc_site* site);
void rt_string_bintoa(rt_string* dst, int32_t i, const rt_alloc_site* site);

// dst = ASCII representation of r at full round-trip precision (LRM
// §6.16.15; inverse of atoreal). `site` locates the conversion for an
// allocation-failure report.
void rt_string_realtoa(rt_string* dst, double r, const rt_alloc_site* site);

// -- Coroutine frame allocation --
//
// Each process lowers to an LLVM coroutine; its frame is allocated through these
// hooks (wired into the coro.alloc / coro.free pattern) so the sim owns frame
// memory rather than libc. Today they are thin malloc/free wrappers; a later swap
// to a pool/arena is transparent to codegen. The optimizer elides the allocation
// entirely for processes whose frame can live on the caller's stack.
void* rt_alloc(uint64_t size);
void  rt_free(void* p);

// -- Named event objects (LRM 15.5) --
//
// An event variable holds a 64-bit handle `{ u32 slot, u32 gen }` naming an event
// OBJECT. LRM Table 6-7 gives an event's default initial value as "New event", so
// EVERY event-typed storage location is bound to a fresh object when that storage
// comes into existence -- a declared variable, an unpacked-array element, a struct
// member, a class property, an automatic local, a container element.
//
// The objects a scalar declared event needs are laid out by the backend in one
// static array, so the common case folds to a constant address. Everything else is
// allocated here, from a CHUNKED pool: chunks never move, so an object pointer
// stays valid across growth even while a process is parked on its waiter list.
// The backend registers its static block as the pool's first chunk at startup, so
// one slot space covers both tiers and rt_event_ptr resolves any handle.
//
// Objects are never freed. LRM 15.5.5.2 reclaims a synchronization object once no
// variable references it, which is garbage collection; Clotho has none, and class
// object bodies leak by the same direction (docs/design/classes.md §7).

// Mirror of the backend's `%clotho.event`. Field order and types must match it
// exactly, like rt_null_site -- the two change together.
typedef struct rt_event {
    uint64_t last_trig; // tick of the last fire; UINT64_MAX == never triggered
    void*    waiters;   // intrusive waiter-list head
    uint8_t  pending;   // a `->> e` awaiting the NBA region
    int32_t  vcd;       // dump-var index, or -1
    void*    next_pend; // next object on the NBA pending chain
} rt_event;

// -- Run start (the process-global reset seam) --
//
// Every process-global growable pool back to power-on. The backend emits one call
// at the very top of the design's main, before anything can allocate, so a host
// that runs more than one design per process starts each run from the state the
// first one saw: ir::run_jit (src/ir/Jit.cpp) builds a fresh LLJIT per design and
// clotho_test drives thousands of them through a single process. Without it the
// leftovers are not merely untidy -- a detached `->> @(x) e` record keyed by a
// dense trigger id the next design reuses hands out a handle minted by the dead
// module, an unclosed $fopen keeps its channel bit until $fopen runs out of
// channels, and the unseeded $random stream is only deterministic for the first
// design in the process.
//
// This is rt_gc_init's rule (Gc.cpp:2984-2987) applied to the rest of the
// runtime: reset at the START of a run, never at shutdown, so a finished run
// leaves a readable oracle behind for the harness that just ran it.
//
// The parameters are the event pool's first chunk -- the backend's static object
// block, re-registered on every run because it is a JIT global that dies with the
// previous LLJIT. `count` includes the reserved null object at slot 0. The seam
// also owns the process control-block pool and the detached-subscription table;
// rt_run_init's own definition lists what it resets and what it deliberately does
// not.
void rt_run_init(void* event_static_block, uint32_t event_static_count);

// Detached one-shot event subscriptions (`->> @(x) e`, LRM 15.5.1), keyed by the
// dense (signal, edge) trigger id. Growable: a `->> @(x) e` in a loop makes one
// registration per pass, which no static wait bucket can bound. `count` is the
// `repeat (n)` countdown; the edge decrements and only a record reaching zero
// fires. rt_event_detach_fire moves the matured handles to a ready list the caller
// reads back with rt_event_detach_ready.
void     rt_event_detach_register(uint32_t trig_id, uint64_t handle, uint32_t count);
uint32_t rt_event_detach_fire(uint32_t trig_id);
uint64_t rt_event_detach_ready(uint32_t i);

// Allocate a fresh event object; returns its handle `{ slot, gen }`.
uint64_t rt_event_new(void);

// The object a handle's slot names, in either tier.
void* rt_event_ptr(uint32_t slot);

// -- Process control blocks (the `process` class, LRM 9.7) --
//
// One block per THREAD -- a process body or a fork child, never a task activation
// (a task runs in its caller's thread, so `process::self()` inside one names the
// caller). It carries the status word LRM 9.7's `status()` reports, the `await`
// waiter list, and the descendant tree `kill` / `disable fork` / `wait fork` walk.
//
// Two storage tiers, because only one of them is dynamic: a statically created
// process gets a slot in the backend's `clotho.proc_pcb` array (there are exactly
// `nproc` of them, known at compile time), while a fork child -- unbounded, since a
// `join_none` in a loop spawns one per pass -- takes a block from the chunked pool
// here. Chunks never move, so a block pointer stays valid across pool growth while
// a thread is parked on it, exactly as the event pool guarantees.
//
// A block outlives its thread while a `process` object still names it, because
// LRM 9.7 lets a program read `FINISHED` / `KILLED` after the fact. That is the
// whole reason the block is not simply frame-resident.
// See docs/design/process-class.md §4.

// Mirror of the backend's `%clotho.pcb`. Field order and types must match it
// exactly, like rt_event -- the two change together.
typedef struct rt_proc {
    // LRM 9.7 `state` in bits 0-2, flags above (RT_PROC_* below).
    uint32_t ctl;
    // 1 while the thread is live, plus 1 while a `process` object names this block.
    // A pooled block returns to the freelist when it reaches 0.
    uint32_t refs;
    // Reserved: the activation-node stack whose head is the frame this thread is
    // parked in, which is what `kill` claims. Null until that phase lands.
    void* acts;
    // `await` waiter list head: frame-resident nodes in the awaiting threads.
    void* waiters;
    // Reserved: this thread's xoshiro256++ state (srandom / get_randstate).
    void* rng;
    // Reserved: the fork record this thread decrements when killed.
    void* join_rec;
    // Weak memo: the `process` object naming this block, or null. Weak so a handle
    // the program has dropped can be collected; the object's finalizer clears it.
    void* obj;
    // Descendant tree. `parent` always names a live thread or a static block: a
    // completing thread re-parents its live children before unlinking itself.
    struct rt_proc* parent;
    struct rt_proc* first_child;
    struct rt_proc* next_sib;
    struct rt_proc* prev_sib;
    // Scratch chain of the blocks one `kill` marked, so the walk that marks a
    // subtree (here, plain memory work) and the force-wake that follows it
    // (generated code, because it touches the scheduler rings) can be separate.
    struct rt_proc* next_marked;
} rt_proc;

// LRM 9.7 `process::state`, the values slang's synthesized enum assigns.
enum {
    RT_PROC_FINISHED  = 0,
    RT_PROC_RUNNING   = 1,
    RT_PROC_WAITING   = 2,
    RT_PROC_SUSPENDED = 3,
    RT_PROC_KILLED    = 4,
    // Mask of the state field within `ctl`.
    RT_PROC_STATE_MASK = 7,
    // Set on a block that lives in the backend's static array, so it is never
    // returned to the pool freelist however its reference count moves.
    RT_PROC_STATIC = 1u << 8,
    // `kill` (or `disable fork`) has been requested on this thread: it aborts at its
    // next poll point, which is right after whatever park it is sitting in.
    RT_PROC_ABORT = 1u << 9,
    // The thread was woken while SUSPENDED, so the wake it was waiting for has
    // already happened: `resume` must schedule it rather than leave it parked (LRM
    // 9.7 -- "if the wait condition is now true or the original delay has transpired,
    // the process is scheduled ... in the current time step").
    RT_PROC_WAKE_PENDING = 1u << 10,
    // The thread suspended ITSELF, so there is no source that could wake it at all
    // and only `resume` can.
    RT_PROC_SELF_PARKED = 1u << 11,
    // The thread is parked in `wait fork`: it is woken when its last live descendant
    // completes.
    RT_PROC_WAIT_FORK = 1u << 12,
};

// A pooled block for a fork child, linked in as a child of `parent` (which may be
// null for a thread with no parent). Starts RUNNING with one reference.
void* rt_proc_alloc(void* parent);

// Mark `pcb`'s whole live subtree for abort (LRM 9.7 `kill` ends the process "and
// all its descendants", 9.6.3 says the same for `disable fork`) and return them as a
// chain through `next_marked`. `with_self` is 0 for `disable fork`, which kills the
// descendants but not the caller. A thread that has already completed is skipped.
// Generated code then force-wakes each one exactly once.
void* rt_proc_mark_kill(void* pcb, uint32_t with_self);

// End `pcb`'s thread: set FINISHED (or KILLED when an abort was marked), re-parent its live
// children onto its own parent -- LRM 9.6.3 counts descendants of children that
// already terminated -- and unlink it from the tree. Returns the `await` waiter
// list head, which generated code walks (the wake pushes onto a codegen-owned
// ring, so the runtime cannot do it), and clears it.
//
// `fork_waiters_out` receives a SECOND list to wake: the parent's, when the parent
// is parked in `wait fork` and this completion emptied its subtree. The barrier is
// exactly `first_child == NULL` thanks to the re-parenting above, and it has to be
// tested here -- after the unlink, while the parent is still reachable.
void* rt_proc_complete(void* pcb, void** fork_waiters_out);

// Move `from`'s live children under `to`, leaving `from` childless. LRM 24.3's
// per-program termination: a program `initial` that reaches its end while a
// SIBLING `initial` of the same program is still running must keep its
// descendants reachable, because the clause terminates them only once EVERY
// `initial` of that program has ended. rt_proc_complete would re-parent them onto
// the ending thread's own parent, which is null for a statically created process,
// dropping them out of the tree for good; so a program body hands them to its
// program instance's termination root first -- a static block that is a tree node
// and never a thread.
void rt_proc_adopt(void* to, void* from);

// Drop the reference the live thread held. Frees a pooled block whose last
// reference this was.
void rt_proc_thread_done(void* pcb);

// Take / drop the reference a `process` object holds on the block it names.
void rt_proc_obj_retain(void* pcb);
void rt_proc_obj_release(void* pcb);

// LRM 9.7 makes `p.await()` on the current process an error; the check is dynamic
// (the target is a handle value), so this is its located fatal.
[[noreturn]] void rt_proc_await_self(const rt_null_site* site, uint64_t now);

// Random stability (LRM 9.7, 18.14): a thread's generator state as a string, and back.
// The format is implementation-defined and only has to round-trip through
// set_randstate on the same implementation -- 64 lowercase hex digits, the four
// xoshiro256++ words most-significant word first.
void rt_proc_randstate_get(rt_string* dst, const void* rng, const rt_alloc_site* site);
// LRM 18.14.4 leaves a string that did not come from get_randstate undefined; clotho
// reports it rather than silently seeding something arbitrary.
void rt_proc_randstate_set(void* rng, const rt_string* s, const rt_null_site* site, uint64_t now);
// The report for such a string.
[[noreturn]] void rt_proc_randstate_bad(const rt_null_site* site, uint64_t now);

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

// Zero-delay oscillation guard: a single time instant activated more than the
// delta-cycle limit of processes without the active queue draining. Prints a
// FATAL diagnostic and ends the simulation (never returns). Reads `now` from the
// wheel itself, so the JIT'd drain loop passes only the wheel pointer.
[[noreturn]] void rt_oscillation_abort(const rt_wheel* wheel);

// Scheduler queue overrun: more entries were live at one instant than the
// statically sized buffer holds, so the next push would overwrite a live one.
// Prints a FATAL diagnostic naming the buffer and ends the simulation (never
// returns). `ring` selects the buffer -- 0 Active ring, 1 Reactive ring (LRM
// 4.4.2), 2 inactive `#0` batch, 3 reactive inactive batch, 4 event wait queue --
// and must stay in step with the kRing* constants in src/ir/LlvmBackend.cpp;
// `capacity` is that buffer's slot count. Only a miscomputed capacity can reach
// this -- see the sizing arguments in ensure_active_queue and wait_counts.
[[noreturn]] void rt_ring_overflow(uint32_t ring, uint32_t capacity);

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
    // Deschedule epoch for an inertial continuous-assignment delay (LRM 10.3.3):
    // the record fires only if its stamp still matches the site's current epoch,
    // else it was cancelled by a later RHS change. Unused by transport (`#d` NBA)
    // records.
    uint64_t epoch;
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

// -- Inertial continuous-assignment delay (`assign #d lhs = rhs`, LRM 10.3.3) --
//
// On each RHS change the backend loads the site's persistent pending-value
// buffer and the target net's current value, compares both against the fresh
// RHS with its own `icmp eq` (arbitrary width, no size limit — the runtime
// never interprets value bytes, only bookkeeps epoch/pending state), and asks
// rt_inertial_decide whether (and with what epoch) to arm a detached store.
// Inertial (pulse-rejecting) semantics: a change that supersedes a still-
// pending store cancels it (epoch bump), so a pulse narrower than the delay
// never propagates.

// Return site `idx`'s persistent pending-value buffer, allocating it to
// `nbytes` on the first call. A site's width is fixed after elaboration, so
// every later call for the same `idx` passes the same `nbytes` and returns the
// same pointer — one allocation per site, none after warm-up. The backend
// stores the freshly-armed value's planes here (see rt_inertial_decide) and
// loads them back next time to build `same_as_pending`.
void* rt_inertial_buf(uint32_t idx, uint32_t nbytes);

// Decide the inertial action for site `idx`. `same_as_pending` is the
// backend's own 4-state-exact (`===`) compare of the fresh RHS against the
// buffer rt_inertial_buf returned (mm && z planes, nonzero when equal); it is
// only consulted when a store is already pending, so a stale/uninitialized
// buffer before the first arm is never read here. `same_as_cur` is the same
// compare against the target net's current value. Both are plain 0/1 flags,
// not C++ `bool`, to keep the JIT call's LLVM parameter types an exact match
// for the native ABI. Returns 0 to arm nothing (either a store to the same
// value is already pending — keep its timer — or the new value already
// equals the net), else the non-zero epoch the caller must stamp on the armed
// record; the caller is then responsible for storing the new planes into the
// rt_inertial_buf buffer before arming.
uint64_t rt_inertial_decide(uint32_t idx, uint32_t same_as_pending, uint32_t same_as_cur);

// Wheel expiry callback for an inertial record: commit it (rt_intra ready list) only
// if its stamped epoch still matches the site's current epoch, else it was cancelled
// by a later RHS change — return it to the freelist unfired.
void rt_inertial_cb(rt_timer* timer, void* ctx);

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

// Register `rec` on EVERY trigger in `ids` (`n` of them) -- a multi-signal trigger
// `@(a or b)`, where the whole list is the event and the first edge to bring the
// countdown to zero matures the store. `ids` must outlive the record's arming; the
// backend points it at a private constant global, since the id list is fixed per
// statement. On maturation the fire path uses it to remove the record from the
// triggers that did not fire, by pointer identity -- a committed record returns to
// its site freelist, so leaving a stale pointer on another list would let a later,
// REUSED record mature early.
//
// `ids` points into `rec`'s own tail, which is stable from arm to commit; the
// backend cannot use a constant global because a trigger id is a per-instance
// link-time constant and twin instances share the emitted code.
void rt_intra_event_register_group(const uint32_t* ids, uint32_t n, void* rec);

// Fire trigger `trig_id`: decrement each pending record's count; at zero, move it
// to its target's ready list (bumping the NBA-pending total) and drop it from its
// other listed triggers, else keep it pending for the next edge. Called from both
// wake paths when the trigger edge matches.
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
// helper — rt_string_data() is already a valid C `const char*`.) `site` locates
// the DPI return for an allocation-failure report.
void rt_dpi_str_out(rt_string* dst, const char* p, const rt_alloc_site* site);

// Register one DPI scope (LRM 35.5.3) for the svScope API — a top-level instance's
// hierarchical name, at its position `index` in the design's top order. Called from
// main, once per top, when the design uses DPI. `index == 0` clears the table first
// (so a fresh simulation in the same process starts with no stale user data) and the
// scope registered there becomes the default svGetScope returns.
// Implemented in Svdpi.cpp alongside svGetScope / svSetScope / svGetUserData.
void rt_svdpi_scope_register(const char* name, uint32_t name_len, uint32_t index);

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
