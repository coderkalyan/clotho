#pragma once

// Process IR — models a single SystemVerilog process or process-like construct
// (always block, function, task). Frozen and immutable once built.
//
// Control flow is structured: there are no basic blocks. Region-bearing
// instructions (e.g. cond) hold nested instruction sequences as index ranges in
// the extra side-table. The top-level body is the IR's root region. All
// instructions live in one flat columnar store; the region tree gives ordering.
//
// Design:
//   * Index handles, not pointers. Every cross-reference is a 4-byte
//     strongly-typed enum index, so the stores stay relocatable and compact.
//   * Structure-of-arrays instruction store. The instruction is split into
//     parallel tag / payload / source-location columns (held directly by Ir)
//     so a pass that only reads tags never pulls payloads into cache.
//   * Compact untagged payload union (<=8 bytes). The discriminant lives in
//     the separate tag column and selects the active member via payload_kind().
//   * Flat u32 "extra" side-table for variable-length / overflow operands and
//     region bodies. Structs whose every field is a 4-byte handle are overlaid
//     onto the buffer with std::bit_cast.
//
// `Ir` is the immutable, finished product. `Builder` (Builder.h) owns the
// growable build-time state and hands back an `Ir` from finish().

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "Array.h"
#include "InternPool.h"

namespace clotho::ir {

// SSA-style handle to an instruction's result / the instruction itself.
enum class Index : uint32_t {};
// Offset into the flat u32 extra side-table.
enum class ExtraIndex : uint32_t {};
// Opaque, frontend-defined per-instruction source location (no reserved values).
enum class SourceLoc : uint64_t {};
// Opaque handle to a module signal (net / variable / port). Resolved by the
// enclosing module; the process IR never interprets it.
enum class Signal : uint32_t {};
// Handle to a function in the enclosing module's function table; the callee of a
// `call`. Resolved by the module, like Signal.
enum class FunctionId : uint32_t {};

// Which real math system function a `real_math` invokes (LRM §20.8.2, Table
// 20-4). Each takes real operands and returns a real; the LRM requires each to
// match the equivalent C standard math library function. Pow / Atan2 / Hypot
// take two operands, all others one. Kept in lockstep with the backend
// dispatch in LlvmBackend.
enum class RealMathOp : uint32_t {
    // $ln(x) -> C log(x).
    Ln,
    Log10,
    Exp,
    Sqrt,
    // $pow(x, y) -> C pow; also the lowering of the real ** operator (LRM
    // 11.4.3 leaves 0**negative and negative**non-integral unspecified — C pow
    // behavior is used).
    Pow,
    Floor,
    Ceil,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    // $atan2(y, x).
    Atan2,
    Hypot,
    Sinh,
    Cosh,
    Tanh,
    Asinh,
    Acosh,
    Atanh,
};

// Which probabilistic distribution function a `dist_call` selects (LRM
// §20.15.2). Kept in lockstep with the rt_dist_* runtime entries and the
// backend dispatch. Uniform doubles as the seeded $random(seed) form
// (§20.15.1), which is rtl_dist_uniform over the full int32 span.
enum class DistOp : uint32_t {
    Uniform,
    Normal,
    Exponential,
    Poisson,
    ChiSquare,
    StudentT,
    Erlang,
};

// A struct may be overlaid on the extra buffer iff it is trivially copyable, a
// whole number of u32 words wide, and padding-free (every field a 4-byte
// handle/enum — flags are u32, never bool). Padding would bit_cast
// indeterminate bytes into the extra buffer, making the IR bytes
// non-deterministic across identical runs (a hazard for hashing and future
// serialization); has_unique_object_representations rejects it at compile time.
template<class T>
concept ExtraPayload = std::is_trivially_copyable_v<T> && (sizeof(T) % sizeof(uint32_t) == 0) &&
                       std::has_unique_object_representations_v<T>;

// Half-open range [start, end) of u32 words in the extra buffer. Doubles as a
// region: a run of instruction indices in nested-execution order.
struct ExtraSlice {
    // First word covered by the slice.
    ExtraIndex start;
    // One past the last word covered by the slice.
    ExtraIndex end;
};

// Extra payload for `select`: the two selected values (the condition is the op).
struct Select {
    // Value chosen when the condition is true.
    Index a;
    // Value chosen when the condition is false.
    Index b;
};

// Extra payload for `store_nb_delay`: the sampled RHS value and the delay in
// ticks. The whole-signal lvalue is the enclosing op_extra `op`.
struct StoreNbDelayInfo {
    // Value stored after the delay (sampled in the current instant).
    Index value;
    // Delay in scaled ticks before the store lands.
    Index delay;
};

// Extra payload for `store_nb_event`: the sampled RHS value, the repeat count
// (constant 1 for a plain `@e`), and the single (trigger signal, edge) the store
// waits on. The whole-signal lvalue is the enclosing op_extra `op`.
struct StoreNbEventInfo {
    // Value stored when the trigger fires (sampled in the current instant).
    Index    value;
    // Number of trigger occurrences to wait for (sampled now); 1 for plain `@e`.
    Index    count;
    // Trigger signal whose edge fires the store.
    Signal   trig;
    // Edge to trigger on (ir::Edge value).
    uint32_t edge;
};

// Extra payload for `part_select`: a constant [msb:lsb] slice of the base.
struct PartSelect {
    // Most-significant selected bit.
    uint32_t msb;
    // Least-significant selected bit.
    uint32_t lsb;
};

// Extra payload for `dyn_part_select`: a runtime-offset, constant-width slice of
// the base (a SystemVerilog indexed part-select `base[off +: width]` /
// `base[off -: width]`). `offset` is the value of the low selected bit (the
// frontend already folds the `-:` form to its low bit); `width` is the constant
// number of selected bits.
struct DynPartSelect {
    // Value of the least-significant selected bit position.
    Index offset;
    // Number of selected bits (constant).
    uint32_t width;
};

// Extra payload for `elem_select`: a runtime element index into an unpacked
// array (the base is the op). `index` is the selector value (declared-range
// coordinate, normalized against the array's bounds in the backend);
// `array_type` is the interned UnpackedArray type being indexed, giving the
// element stride and the declared [left:right] bounds.
struct ElemSelect {
    // Element index value, in declared-range coordinates.
    Index index;
    // The UnpackedArray type being indexed.
    Ref array_type;
};

// Extra payload for `arr_slice`: a constant-bounds slice of a fixed unpacked
// array (LRM 7.4.5, `busB = busA[7:6]`). The op is the base array lvalue; the
// slice is position-0-anchored (a downstream elem_select's index normalizes
// against `slice_type`'s bounds, added to the base pre-advance), so it reads /
// writes as an ordinary array of `slice_type`. `first_pos` is the 0-based flat
// position of the slice's first element within the base array's storage.
struct ArrSlice {
    // The slice's own UnpackedArray type (gives element stride + declared bounds).
    Ref slice_type;
    // 0-based storage position of the slice's first element in the base array.
    uint32_t first_pos;
};

// Extra payload for `agg_copy`: a whole-object blocking copy of one aggregate
// lvalue into another (both of `type`). The op is the destination lvalue; `src`
// the source lvalue. Lowered to a single llvm.memcpy of the aggregate storage —
// valid only when the aggregate has no string (heap-owning) leaf, which the
// frontend checks before emitting it.
struct AggCopyInfo {
    // Source aggregate lvalue.
    Index src;
    // The aggregate type (gives the storage size to copy).
    Ref type;
};

// Extra payload for `inside_set`: the `inside` operator's membership test of
// the op against a set (LRM 11.4.13), OR-reduced across members with LRM
// 11.4.5 X-propagation. The OR is order-insensitive, so members are grouped
// by kind: `values` is a run of singular member values, each tested with
// wildcard equality (only the member's own x/z bits are wildcards); `ranges`
// is a run of back-to-back [lo, hi] operand pairs tested as
// `sel >= lo && sel <= hi`. All operands are pre-resized to the selector type.
struct InsideSetInfo {
    // Singular member values (wildcard-equality tests).
    ExtraSlice values;
    // [lo, hi] range operand pairs, two indices per range.
    ExtraSlice ranges;
};

// Extra payload for `agg_eq`: whole-aggregate equality of two unpacked
// aggregate lvalues of `type` (LRM 11.2.2). The op is the left lvalue.
struct AggEqInfo {
    // Right-hand aggregate lvalue.
    Index rhs;
    // The aggregate type (gives the member layout to compare).
    Ref type;
};

// Extra payload for `member_select`: a member of an unpacked struct (the base is
// the op). Unlike a packed struct member (a constant part-select over a flat
// bit-vector), an unpacked struct is an aggregate whose members have their own
// storage, so a member is addressed by its declaration-order field index into
// the struct's backing StructType, not a bit offset. `field_index` is the
// member's position in declaration order; `struct_type` is the interned
// Kind::Struct (is_packed=false) type, giving the member layout.
struct MemberSelect {
    // Declaration-order field index of the selected member.
    uint32_t field_index;
    // The unpacked Kind::Struct type being accessed.
    Ref struct_type;
};

// Extra payload for `dyn_array_new` / `dyn_array_copy`: the tag's single
// non-handle operand (element count for New, source handle for Copy) plus the
// interned DynArray type (element stride / slot), independent of the target
// signal — so the op works for a module signal or a frame local. The handle
// lvalue is the instruction's op.
struct DynArrayArg {
    // Element count (new) or source handle lvalue (copy).
    Index arg;
    // The interned DynArray type of the target.
    Ref array_type;
};

// Extra payload for `dyn_array_new_init`: arr = new[size](src) (LRM 7.5.1) —
// reallocate to `size` elements, then copy min(size, src.len) elements from
// the source handle lvalue. The handle lvalue is the op.
struct DynArrayNewInitInfo {
    // Element count operand.
    Index size;
    // Source dynamic-array handle lvalue.
    Index src;
    // The interned DynArray type of the target.
    Ref array_type;
};

// Extra payload for the one-operand queue tags (QueuePushBack / QueuePushFront
// / QueueDeleteIdx / QueueCopy): the tag's single non-handle operand plus the
// interned Queue type of the target (element stride / slot / bound),
// independent of the target signal — so the op works for a module signal or a
// frame local. The handle lvalue is the instruction's op.
struct QueueArg {
    // Element value (push), position (delete-idx), or source handle (copy).
    Index arg;
    // The interned Queue type of the target.
    Ref queue_type;
};

// Extra payload for `queue_insert`: q.insert(idx, e) inserts the element value
// `val` before position `idx` (LRM 7.10.2.4). The handle lvalue is the op.
struct QueueInsertInfo {
    // Position operand.
    Index idx;
    // Element value operand.
    Index val;
    // The interned Queue type of the target.
    Ref queue_type;
};

// Extra payload for `queue_append_range`: append `src`'s live elements at
// logical indices [lo, hi] (inclusive, i64 values; a full-queue append passes
// lo = 0 and hi = a to-end sentinel) onto the dst handle, materializing a queue
// slice `q[a:b]` (LRM 7.10.4) or a queue-typed concatenation operand. Both the
// dst (op) and src are handle lvalues; the dst never aliases src.
struct QueueSliceInfo {
    // Source queue handle lvalue.
    Index src;
    // Low / high logical index operands (i64 values).
    Index lo;
    Index hi;
    // The interned Queue type of the target.
    Ref queue_type;
};

// Extra payload for the one-operand associative-array tags (AssocSelect,
// AssocExists, AssocDeleteKey, AssocCopy, AssocFirst/Last/Next/Prev). `arg` is
// the key value (select/exists/delete-key), the source handle lvalue (copy), or
// the key-variable lvalue (first/last/next/prev). The handle lvalue is the op.
struct AssocArg {
    // Key value / source handle / key-variable lvalue, per the tag.
    Index arg;
    // The interned Assoc type of the target (element = value, fields_id = key,
    // right = wildcard flag).
    Ref assoc_type;
};

// Extra payload for `read_mem`: a $readmemh / $readmemb load of a file into an
// unpacked-array memory. The filename value is the instruction's `op`: either a
// packed-ASCII vector (the backend spills its must-bits to a byte buffer) or a
// string value (the backend reads the rt_string handle's bytes at the call site).
// `target` is the destination memory signal. `start` / `finish` are optional
// address-bound operands (valid per `has_start` / `has_finish`). `hex` selects hex
// vs binary digits.
struct ReadMemInfo {
    // Destination memory signal.
    Signal target;
    // Optional first / last address operands (declared-range coordinates).
    Index start;
    Index finish;
    // 1 for $readmemh, 0 for $readmemb.
    uint32_t hex;
    // Whether the start / finish operands are present.
    uint32_t has_start;
    uint32_t has_finish;
};

// Extra payload for `call`: the callee plus its already-lowered argument values,
// in formal-parameter order (each resized to the parameter type by the frontend).
struct CallInfo {
    // Function being called, resolved against the module's function table.
    FunctionId func;
    // The argument value operands, in order.
    ExtraSlice args;
};

// Extra payload for `task_call`: the callee task plus one operand per formal, in
// declaration order. An input formal's operand is a value (resized to the
// parameter type); an output / inout formal's operand is an lvalue leaf
// (Signal / Local) naming a caller temp the backend passes by pointer and the
// frontend copies to/from the actual around the call. Directions come from the
// callee's Function::arg_dirs.
struct TaskCallInfo {
    // Task being called, resolved against the module's function table.
    FunctionId func;
    // One operand per formal, in order (value for input, lvalue for output/inout).
    ExtraSlice args;
};

// Join discipline of a fork block (LRM 9.3.2). V1 implements Join only.
enum class ForkKind : uint32_t {
    Join,     // parent blocks until all children complete
    JoinAny,  // parent blocks until the first child completes (V2)
    JoinNone, // parent does not block (V2)
};

// Extra payload for `Fork` (LRM 9.3.2). Each child statement of the fork was
// lowered into its own argument-less fork-child task (Function::is_fork_child);
// `children` holds their FunctionIds as raw u32s (one per parallel process).
struct ForkInfo {
    // FunctionIds of the child bodies, stored as u32 (FunctionId is a u32 enum).
    ExtraSlice children;
    ForkKind   kind;
};

// Extra payload for `cond`: the then / else regions (the condition is the op).
struct Regions {
    // Instructions executed when the condition is true.
    ExtraSlice then_;
    // Instructions executed when the condition is false.
    ExtraSlice else_;
    // Hint (1/0) that the condition is rarely true; biases backend branch
    // weights toward the else path (e.g. a violation check).
    uint32_t unlikely = 0;
};

// Extra payload for `select_lazy`: a value-producing conditional (the predicate
// is the op). Unlike `cond` (statement-only) each branch designates a result
// value; the backend merges them with a PHI. Used for short-circuit &&/||,
// where the then-branch evaluates the right operand and the else-branch yields
// the dominating constant.
struct SelectLazy {
    // Instructions evaluated when the predicate is a definite 1.
    ExtraSlice then_;
    // Value produced by the then-branch.
    Index then_val;
    // Instructions evaluated when the predicate is 0.
    ExtraSlice else_;
    // Value produced by the else-branch.
    Index else_val;
};

// Extra payload for `loop`. `cond` holds the instructions that recompute the
// predicate each iteration; `pred` is the 1-bit predicate index defined within
// `cond` (a constant 1 when the source condition is omitted). `body` is the
// loop body; `step` is the per-iteration update (empty for while / forever).
struct LoopInfo {
    // Instructions recomputing the predicate at the top of each iteration.
    ExtraSlice cond;
    // The 1-bit predicate value, defined inside `cond`.
    Index pred;
    // Loop body, executed while the predicate is a definite 1.
    ExtraSlice body;
    // Per-iteration update, executed after the body (empty for while / forever).
    ExtraSlice step;
    // do-while (1/0): run the body once before the first predicate test. The
    // backend branches entry -> body instead of entry -> header (LRM 12.7.1).
    uint32_t bottom_test;
};

// Extra payload for one arm of a `switch`: the constant case-value keys that
// select this arm, and the arm's statement body. `keys` is a run of u64 case
// values (2 words each; decode with Ir::region); several keys share one body
// for a grouped case item (`2, 3: stmt`). `body` is the region executed on a
// match.
struct SwitchArm {
    // Constant case-value keys selecting this arm (u64 each).
    ExtraSlice keys;
    // Statement region run on a match.
    ExtraSlice body;
};

// Extra payload for `switch` (payload: op_extra -> SwitchInfo; the op is the
// selector value). A constant, deduped, wildcard-free `case` lowered to a
// multiway branch: the backend packs the selector's planes into an integer key
// ({may^must, must}, must on the low bits) so a clean value keys on its bare
// bits (dense -> jump table) and any x/z selector lands outside the clean key
// range and falls to `default_`. `arms` is a run of SwitchArm records.
struct SwitchInfo {
    // Run of SwitchArm records (each sizeof(SwitchArm)/4 words).
    ExtraSlice arms;
    // Default region, run when no key matches (empty when the case had none).
    ExtraSlice default_;
};

// Extra payload for `display` / `fdisplay`: a $display / $write / $fdisplay /
// $fwrite system task. The format-string bytes live packed little-endian
// (4 chars per u32 word) at `fmt` for `fmt_len` bytes; `args` is the slice of
// value operands the format specifiers consume, in order; `newline` is 1 for
// the $*display forms (append '\n') and 0 for the $*write forms. The
// destination is the tag's: Display writes the standard-output MCD, FDisplay
// carries the descriptor as its primary operand.
struct DisplayInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // 1 => append a trailing newline ($display / $fdisplay); 0 => none.
    uint32_t newline;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
};

// Extra payload for `finish`: a $finish / $stop. `arg` is the diagnostic level
// (0/1/2; LRM Table 20-1: 0 prints nothing, 1 time+location, 2 adds resource
// stats). `banner`/`banner_len` are the packed bytes of the compile-time banner
// prefix "<file>:<line>: <task>" that the runtime completes with the simulation
// time when `arg` >= 1; the task name is baked in so $finish and $stop differ.
struct FinishInfo {
    // First extra word of the packed banner-prefix bytes.
    ExtraIndex banner;
    // Banner-prefix length in bytes.
    uint32_t banner_len;
    // Diagnostic level (0/1/2), defaulting to 1 when the source omits it.
    uint32_t arg;
};

// Extra payload for `case_viol`: a unique/priority violation report (LRM
// 12.4.2 / 12.5.3). `banner` / `banner_len` are the packed bytes of the
// compile-time "<file>:<line>: " prefix; `kind` selects the message (0: no
// item matched, 1: overlapping items).
struct CaseViolInfo {
    // First extra word of the packed banner-prefix bytes.
    ExtraIndex banner;
    // Banner-prefix length in bytes.
    uint32_t banner_len;
    // Violation-message selector: 0 no-match, 1 overlap.
    uint32_t kind;
};

// Extra payload for `strobe_pend`: the deferred-print task to run at the end
// of the current time step (see Inst::Tag::StrobePend).
struct StrobePendInfo {
    // The synthesized zero-formal task holding the format args + Display.
    FunctionId func;
};

// Severity level of a `severity` instruction. Selects the diagnostic label the
// runtime prints (LRM 20.10); Fatal is `$fatal` (the frontend follows the report
// with a Finish carrying the finish number, so the report itself never
// terminates).
enum class Severity : uint32_t {
    Info,
    Warning,
    Error,
    Fatal,
};

// Extra payload for `severity`: an $info / $warning / $error / $fatal report
// (LRM 20.10) or an immediate-assertion default failure (LRM 16.3). `banner` /
// `banner_len` are the packed bytes of the compile-time "<file>:<line>: " prefix
// the runtime completes with the message. `fmt` / `fmt_len` are the packed
// $display-style format bytes; `args` the value operands its specifiers consume
// (both empty for a bare `$error;` or a default assertion failure). `level` is
// the ir::Severity label.
struct SeverityInfo {
    // First extra word of the packed banner-prefix bytes.
    ExtraIndex banner;
    // Banner-prefix length in bytes.
    uint32_t banner_len;
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // ir::Severity label.
    uint32_t level;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
};

// Extra payload for `fseek`: $fseek(fd, offset, operation) — the descriptor is
// the instruction's op; these are the remaining two value operands.
struct FseekInfo {
    // Byte-offset operand.
    Index offset;
    // Seek-origin operand (0/1/2 per C fseek whence).
    Index operation;
};

// Extra payload for `real_math_binary`: the second real operand plus the
// ir::RealMathOp selecting the routine (Pow / Atan2 / Hypot); the first
// operand is the instruction's op.
struct RealMathBinaryInfo {
    // Second real operand (exponent / x / second leg).
    Index rhs;
    // ir::RealMathOp value.
    uint32_t fn;
};

// Extra payload for `dist_unary`: a one-parameter distribution function
// ($dist_exponential / $dist_poisson / $dist_chi_square / $dist_t, LRM
// §20.15.2). The seed word is the instruction's op.
struct DistUnaryInfo {
    // The distribution parameter (mean / degrees of freedom).
    Index p1;
    // ir::DistOp value.
    uint32_t fn;
};

// Extra payload for `dist_binary`: a two-parameter distribution function
// ($dist_uniform / $dist_normal / $dist_erlang, LRM §20.15.2) or seeded
// $random (§20.15.1, Uniform over the full int32 span). The seed word is the
// instruction's op.
struct DistBinaryInfo {
    // First distribution parameter.
    Index p1;
    // Second distribution parameter.
    Index p2;
    // ir::DistOp value.
    uint32_t fn;
};

// Extra payload for `sformat`: $sformat / $swrite (LRM §21.3.3) format the value
// args per the packed format-string bytes at `fmt` (`fmt_len` bytes) and store the
// result into the destination string lvalue `dst`. `args` is the value-operand
// slice the specifiers consume.
struct SformatInfo {
    // Destination string lvalue.
    Index dst;
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
};

// Extra payload for `sformatf`: $sformatf (LRM §21.3.3) formats the value args
// per the packed format-string bytes at `fmt` (`fmt_len` bytes) and yields a
// new string value (the instruction's string temp).
struct SformatfInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
};

// Extra payload for `fscanf` / `sscanf` (LRM §21.3.4): parse `source` per the
// packed format-string bytes at `fmt` (`fmt_len` bytes) into the output lvalues
// `args`, returning the conversion count. The tag says what `source` is: an FD
// value (Fscanf) or a packed-ASCII string value the backend spills (Sscanf).
struct ScanfInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // Source operand: an FD value (Fscanf) or a packed string value (Sscanf).
    Index source;
    // Output lvalue operands, one per conversion, in order.
    ExtraSlice args;
};

// Extra payload for `fread_mem`: $fread(mem, fd[, start[, count]]) reads raw bytes
// into the unpacked-array memory `mem`. The fd value is the instruction's `op`;
// `start` / `count` are optional address / element-count operands (valid per has_*).
struct FreadMemInfo {
    // Destination memory signal.
    Signal mem;
    // Optional first-address and max-element-count operands.
    Index start;
    Index count;
    // Whether the start / count operands are present.
    uint32_t has_start;
    uint32_t has_count;
};

// Extra payload for `test_plusargs`: $test$plusargs(str) (LRM §21.6). `str` is
// the packed query-string bytes (a compile-time-literal argument; a runtime
// string operand is a documented gap, see Lower.cpp).
struct TestPlusArgsInfo {
    // First extra word of the packed query-string bytes.
    ExtraIndex str;
    // Query-string length in bytes.
    uint32_t str_len;
};

// Extra payload for `value_plusargs`: $value$plusargs(user_string, var) (LRM
// §21.6). `prefix` is the plusarg_string portion of user_string (everything
// before the trailing `%conv`); `conv` is the lowercase format character
// (d/o/b/h/s — e/f/g are a documented gap, real conversion needs the same
// ASCII->real scan $fscanf/$sscanf's own %f/%e/%g gap needs). The
// instruction's `op` (in the enclosing op_extra payload) is the output lvalue.
struct ValuePlusArgsInfo {
    // First extra word of the packed plusarg_string prefix bytes.
    ExtraIndex prefix;
    // Prefix length in bytes.
    uint32_t prefix_len;
    // Lowercase format conversion character ('d'/'o'/'b'/'h'/'s').
    uint32_t conv;
};

// Extra payload for `timeformat`: a $timeformat system task. `units`,
// `precision`, and `min_width` are value operands (general runtime rvalues); the
// suffix string is appended after the formatted number and lives packed
// little-endian (4 chars per u32 word) at `suffix` for `suffix_len` bytes.
struct TimeFormatInfo {
    // Time-unit exponent operand (10^units seconds).
    Index units;
    // Fractional-digit count operand.
    Index precision;
    // Minimum field width operand (right-justifies the number + suffix).
    Index min_width;
    // First extra word of the packed suffix bytes.
    ExtraIndex suffix;
    // Suffix length in bytes.
    uint32_t suffix_len;
};

// Extra payload for `str_putc`: s[i] = c / s.putc(i, c). The string lvalue is
// the op; these are the index and character value operands.
struct StrPutcInfo {
    // Character position operand.
    Index index;
    // Character value operand (low 8 bits used).
    Index ch;
};

// Extra payload for `str_substr`: s.substr(i, j). The source string is the op;
// these are the inclusive start / end position operands.
struct StrSubstrInfo {
    // First selected position operand.
    Index i;
    // Last selected position operand (inclusive).
    Index j;
};

// One row of an enum's name table: a member value (low 64 bits) and its source
// name, packed little-endian at `name_at` for `name_len` bytes.
struct EnumNameEntry {
    // First extra word of the member value's must-plane bytes ((width+7)/8 bytes,
    // little-endian). Stored as bytes rather than a word so a base wider than 64
    // bits round-trips.
    ExtraIndex val_at;
    // Member value length in bytes ((width+7)/8), identical across all entries.
    uint32_t val_len;
    // First extra word of the packed name bytes.
    ExtraIndex name_at;
    // Name length in bytes.
    uint32_t name_len;
};

// Extra payload for `enum_name`: the enum's member table, a run of back-to-back
// EnumNameEntry records the runtime scans for a value match.
struct EnumNameInfo {
    // The member rows, as back-to-back EnumNameEntry records.
    ExtraSlice entries;
};

// Which 4-state bit values a CountBits op tallies (LRM 20.9 $countbits control
// bits). Bits are OR-combined into CountBitsInfo::mask; $countones is CountOne
// alone, $isunknown is CountX | CountZ.
enum CountBitsClass : uint32_t {
    CountZero = 1u << 0,
    CountOne  = 1u << 1,
    CountX    = 1u << 2,
    CountZ    = 1u << 3,
};

// Extra payload for `count_bits`: the operand is the op; this selects which
// per-bit 4-state values are counted (a CountBitsClass OR-mask).
struct CountBitsInfo {
    // OR of CountBitsClass bits.
    uint32_t mask;
};

// Extra payload for `wait`: one (signal, edge) event of an intra-body event
// control. A `@(a or posedge b)` wait carries several of these in a row.
struct WaitEdge {
    // Signal whose change can wake the process.
    Signal signal;
    // Edge to wait for (ir::Edge value).
    uint32_t edge;
};

// Extra payload for `wait`: the list of events the process suspends on. The
// process wakes when ANY listed (signal, edge) fires. `events` is a run of
// packed WaitEdge records (each two words wide) in the extra buffer.
struct WaitInfo {
    // The (signal, edge) events, as back-to-back WaitEdge records.
    ExtraSlice events;
};

// Extra payload for `event_trigger_nb_delayed` (`->> #d e`): the target event
// signal. The delay (in wheel ticks) is the instruction's `op` operand.
struct EventDelayInfo {
    // Event whose synthetic signal owns the wait queue to service on expiry.
    Signal event;
};

static_assert(ExtraPayload<ExtraSlice>);
static_assert(ExtraPayload<CallInfo>);
static_assert(ExtraPayload<TaskCallInfo>);
static_assert(ExtraPayload<ForkInfo>);
static_assert(ExtraPayload<Select>);
static_assert(ExtraPayload<PartSelect>);
static_assert(ExtraPayload<DynPartSelect>);
static_assert(ExtraPayload<ElemSelect>);
static_assert(ExtraPayload<ArrSlice>);
static_assert(ExtraPayload<MemberSelect>);
static_assert(ExtraPayload<AggCopyInfo>);
static_assert(ExtraPayload<AggEqInfo>);
static_assert(ExtraPayload<InsideSetInfo>);
static_assert(ExtraPayload<DynArrayArg>);
static_assert(ExtraPayload<DynArrayNewInitInfo>);
static_assert(ExtraPayload<QueueArg>);
static_assert(ExtraPayload<QueueInsertInfo>);
static_assert(ExtraPayload<QueueSliceInfo>);
static_assert(ExtraPayload<AssocArg>);
static_assert(ExtraPayload<ReadMemInfo>);
static_assert(ExtraPayload<Regions>);
static_assert(ExtraPayload<LoopInfo>);
static_assert(ExtraPayload<SwitchArm>);
static_assert(ExtraPayload<SwitchInfo>);
static_assert(ExtraPayload<DisplayInfo>);
static_assert(ExtraPayload<FinishInfo>);
static_assert(ExtraPayload<CaseViolInfo>);
static_assert(ExtraPayload<StrobePendInfo>);
static_assert(ExtraPayload<SeverityInfo>);
static_assert(ExtraPayload<FseekInfo>);
static_assert(ExtraPayload<RealMathBinaryInfo>);
static_assert(ExtraPayload<DistUnaryInfo>);
static_assert(ExtraPayload<DistBinaryInfo>);
static_assert(ExtraPayload<SformatInfo>);
static_assert(ExtraPayload<SformatfInfo>);
static_assert(ExtraPayload<ScanfInfo>);
static_assert(ExtraPayload<FreadMemInfo>);
static_assert(ExtraPayload<TimeFormatInfo>);
static_assert(ExtraPayload<WaitEdge>);
static_assert(ExtraPayload<WaitInfo>);
static_assert(ExtraPayload<StrPutcInfo>);
static_assert(ExtraPayload<StrSubstrInfo>);
static_assert(ExtraPayload<EnumNameEntry>);
static_assert(ExtraPayload<EnumNameInfo>);
static_assert(ExtraPayload<CountBitsInfo>);
static_assert(ExtraPayload<TestPlusArgsInfo>);
static_assert(ExtraPayload<ValuePlusArgsInfo>);

struct Inst {
    enum class Tag : uint8_t {
        // -- Leaves --
        // Interned constant value (payload: ref).
        Constant,
        // Function / task parameter by position (payload: arg). Read-only; a
        // formal is copied into a Local at entry so the body can mutate it.
        Arg,
        // Reference to a module signal (payload: signal).
        Signal,
        // Reference to a function-frame local slot by index (payload: local). An
        // lvalue leaf like Signal, but backed by per-call stack storage (automatic
        // lifetime) rather than the persistent state buffer. Holds formals, the
        // return variable, and body-local declarations of a function.
        Local,

        // -- Lvalue derivation --
        // Single-bit select base[index] (payload: binary).
        BitSelect,
        // Constant part-select base[msb:lsb] (payload: op_extra -> PartSelect).
        PartSelect,
        // Runtime-offset, constant-width part-select base[off +: w] / base[off -: w]
        // (payload: op_extra -> DynPartSelect). The offset is a runtime value; the
        // backend rides the same shift/mask load/store path as a dynamic BitSelect.
        DynPartSelect,
        // Unpacked-array element select base[index] (payload: op_extra ->
        // ElemSelect). Distinct from BitSelect: the stride is the element's
        // storage size, not one bit, and the index is normalized against the
        // array's declared bounds.
        ElemSelect,
        // Constant-bounds slice of a fixed unpacked array base[hi:lo] (payload:
        // op_extra -> ArrSlice). Yields a position-0-anchored sub-array lvalue;
        // an lvalue-only op, consumed by the per-element aggregate copy / eq /
        // store paths, never lowered as a value.
        ArrSlice,
        // Unpacked-struct member select base.field (payload: op_extra ->
        // MemberSelect). The base is an unpacked struct aggregate; the member is
        // addressed by declaration-order field index into the backing StructType,
        // not a bit offset (a packed struct member is a PartSelect instead).
        MemberSelect,
        // Class object `new` (LRM 8.7): allocate the heap object body, power-on
        // init its properties, and yield the object handle as a 2-state 64-bit
        // value (payload: ref -> the Kind::Class type). The constructor call is
        // emitted separately by the frontend (a normal call with the handle spliced
        // as `this`). Reference semantics: the handle is a bare pointer.
        ClassNew,
        // Dereference a class object handle to an aggregate lvalue rooted at the
        // object body (payload: op_type -> op is the handle value, type is the
        // Kind::Class). Produces an is_agg lvalue whose base is the body pointer, so
        // a following MemberSelect addresses a property. The one op that turns a
        // reference handle into a body lvalue (contrast a value struct, addressed
        // inline).
        HandleDeref,

        // -- State access --
        // Read the current value of an lvalue (payload: unary).
        Load,
        // Blocking assignment, lhs = rhs (payload: binary).
        Store,
        // Nonblocking assignment, lhs <= rhs (payload: binary).
        StoreNb,
        // Nonblocking intra-assignment delay, lhs <= #d rhs (LRM 9.4.5): sample
        // rhs now (this instant), schedule the store of the sampled value for
        // `now + d` without blocking the process (payload: op_extra ->
        // StoreNbDelayInfo; the op is the whole-signal lvalue).
        StoreNbDelay,
        // Nonblocking intra-assignment event, lhs <= @e rhs / lhs <= repeat(N) @e
        // rhs (LRM 9.4.5): sample rhs (and N) now, store the sampled value after N
        // occurrences of the trigger edge, without blocking the process (payload:
        // op_extra -> StoreNbEventInfo; the op is the whole-signal lvalue).
        StoreNbEvent,
        // Whole-object blocking copy of one aggregate into another (payload:
        // op_extra -> AggCopyInfo). Lowered to llvm.memcpy of the storage; the
        // frontend emits it only for a string-free aggregate.
        AggCopy,
        // 1-bit 4-state equality of two whole unpacked aggregates (payload:
        // op_extra -> AggEqInfo; op is the left lvalue). The backend expands
        // the member walk, AND-reducing per-leaf `==` results so LRM 11.4.5
        // X-propagation is carried exactly; `!=` is a Lnot
        // of this.
        AggEq,

        // -- Dynamic-array operations on a handle lvalue (LRM 7.5) --
        // Each lowers to its rt_dynarr_* runtime call; element access rides
        // the ordinary ElemSelect path over the handle's heap buffer, not
        // these tags.
        // arr = new[size]: (re)allocate to `size` elements, each the element's
        // power-on value (LRM 7.5.1; payload: op_extra -> DynArrayArg, whose
        // arg is the element count; op is the handle lvalue).
        DynArrayNew,
        // arr = new[size](src): like DynArrayNew, then copy min(size, src.len)
        // elements from the source handle (payload: op_extra ->
        // DynArrayNewInitInfo; op is the handle lvalue).
        DynArrayNewInit,
        // arr.size() -> 32-bit int (payload: op_type — op is the handle
        // lvalue, type the interned DynArray type).
        DynArraySize,
        // arr.delete(): free the storage, size -> 0 (payload: op_type).
        DynArrayDelete,
        // arr = other: deep copy of another dynamic array's storage (value
        // semantics; payload: op_extra -> DynArrayArg, whose arg is the source
        // handle lvalue; op is the destination handle lvalue).
        DynArrayCopy,

        // -- Queue operations on a handle lvalue (LRM 7.10.2) --
        // Each lowers to its rt_queue_* runtime call; indexed element access
        // q[i] rides the ElemSelect queue path (ring math over the handle's
        // buffer), not these tags.
        // q.size() -> 32-bit int (payload: op_type — op is the handle lvalue,
        // type the interned Queue type).
        QueueSize,
        // q.push_back(e) / q.push_front(e): append the element value (payload:
        // op_extra -> QueueArg; op is the handle lvalue). On a bounded queue an
        // overflowing element is dropped from the opposite end.
        QueuePushBack,
        QueuePushFront,
        // q.pop_back() / q.pop_front(): remove and return the tail / head
        // element — the element default on an empty queue (payload: op_type,
        // like QueueSize).
        QueuePopBack,
        QueuePopFront,
        // q.insert(idx, e) (payload: op_extra -> QueueInsertInfo; op is the
        // handle lvalue).
        QueueInsert,
        // q.delete(idx): remove the element at idx (payload: op_extra ->
        // QueueArg, whose arg is the position; op is the handle lvalue).
        QueueDeleteIdx,
        // q.delete(): remove all elements, size -> 0 (payload: op_type).
        QueueDeleteAll,
        // q = other: deep copy of another queue's storage (value semantics;
        // payload: op_extra -> QueueArg, whose arg is the source handle
        // lvalue; op is the destination handle lvalue).
        QueueCopy,
        // Nonblocking `dst <= src` deep copy (payload: op_extra -> QueueArg, arg
        // is the source handle; op is the dst handle lvalue). Stages a deep copy
        // of src in the Active region; the NBA region moves it into dst (LRM
        // 7.10 / 10.4.2).
        QueueCopyNb,
        // Append src[lo:hi] onto the dst handle (payload: op_extra ->
        // QueueSliceInfo; op is the dst handle lvalue), materializing a queue
        // slice or a queue-typed concatenation operand (LRM 7.10.4).
        QueueAppendRange,

        // -- Array ordering methods on a handle lvalue (LRM 7.12.2) --
        // Each lowers to its rt_{dynarr,queue}_* runtime call; all carry only
        // the handle lvalue + interned type (payload: op_type, like
        // DynArrayDelete). Reverse / Shuffle are element-agnostic; Sort / Rsort
        // are value-keyed (2-state integral width<=64 or real, guarded in the
        // frontend). Shuffle draws from the process frame RNG (forces a
        // coroutine); the backend threads the frame RNG slot at lowering.
        DynArrayReverse,
        DynArraySort,
        DynArrayRsort,
        DynArrayShuffle,
        QueueReverse,
        QueueSort,
        QueueRsort,
        QueueShuffle,

        // -- Associative-array operations on a handle lvalue (LRM 7.8/7.9) --
        // Each lowers to its rt_assoc_* runtime call; keys cross the ABI as
        // normalized order-preserving byte spans, built by a backend converter
        // keyed on the interned Assoc type (element = value, fields_id = key,
        // right = wildcard flag). See docs/design/dynamic-arrays.md §4a/§6.
        // a[k] element select producing an lvalue (payload: op_extra ->
        // AssocArg, whose arg is the key value/lvalue; op is the handle lvalue).
        // A Load of it calls rt_assoc_get_ptr (absent -> element default, no
        // create); a Store calls rt_assoc_put_ptr (create + default-fill). The
        // get/put dispatch is deferred to lower_load/lower_store.
        AssocSelect,
        // a.num() / a.size() -> 32-bit int entry count (payload: op_type).
        AssocNum,
        // a.exists(k) -> 32-bit int 0/1 (payload: op_extra -> AssocArg, arg is
        // the key value; op is the handle lvalue).
        AssocExists,
        // a.delete(k): remove one key if present (payload: op_extra ->
        // AssocArg, arg is the key value; op is the handle lvalue).
        AssocDeleteKey,
        // a.delete(): remove all entries, num -> 0 (payload: op_type).
        AssocDeleteAll,
        // a = other: deep copy of another assoc's entries (value semantics;
        // payload: op_extra -> AssocArg, arg is the source handle lvalue).
        AssocCopy,
        // a.first(k)/last(k)/next(k)/prev(k): ordered traversal (LRM 7.9.3).
        // Return 32-bit int 1 if a key was written, 0 at the end. `arg` is the
        // key-variable LVALUE: first/last write the min/max key into it; next/
        // prev read its current value and write the successor/predecessor. The
        // backend normalizes the current key and denormalizes the result key.
        AssocFirst,
        AssocLast,
        AssocNext,
        AssocPrev,

        // -- Arithmetic (payload: binary) --
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        Pow,

        // -- Bitwise (payload: binary, except Bnot) --
        Band,
        Bor,
        Bxor,
        // Bitwise xnor ^~ / ~^ (payload: binary).
        Bxnor,
        // Bitwise invert ~ (payload: unary).
        Bnot,

        // Strong wire/tri multi-driver resolution of two driver values (LRM
        // 6.6.1, no drive strengths), the lattice join z ⊑ {0,1} ⊑ x: a
        // high-Z operand contributes nothing (identity), two operands that
        // agree keep the value, a 0-vs-1 (or any x) conflict is x, all-z is z.
        // Associative + commutative, so K drivers fold left through this.
        // Result is always 4-state at the operand width (payload: binary).
        // Emitted only by the multi-driver-net resolver (Lower.cpp
        // emit_wire_resolvers); single-driver nets never use it.
        WireRes,
        // Wired-AND / wired-OR multi-driver resolution (LRM 6.6.2 / 6.6.3) for
        // wand/triand and wor/trior nets. Same slot machinery and z-identity as
        // WireRes, only a different per-bit reducer: wand forces 0 on any 0
        // driver, wor forces 1 on any 1 driver, else x on any x, else z. Both
        // associative + commutative, so K drivers fold left (payload: binary).
        WandRes,
        WorRes,
        // Resistive pull applied after a tri0/tri1 net's drivers are resolved
        // (LRM 6.6.4): a bit still Z (all drivers off) is pulled to the immediate
        // (0 for tri0, 1 for tri1); a definitely-driven or X bit is unchanged.
        // A post-fold unary transform, not a per-pair rule — the pull only fills
        // Z once every driver has folded. Result never carries Z (payload:
        // op_imm — op is the resolved value, imm the pull value).
        NetPull,

        // -- Reductions, e.g. &x (payload: unary) --
        RedAnd,
        RedOr,
        RedXor,

        // Index of the lowest (Ctz) / highest (Clz) definite-1 bit of the
        // operand's must plane, or the operand width when it has no definite-1
        // bit (payload: unary). x/z bits are not definite 1, so they never
        // count. Result is a definite (2-state) 32-bit count. Used to lower the
        // one-hot `case (1'b1)` reverse-case idiom to a jump table: the winning
        // arm is the lowest (ascending source order) or highest (descending) set
        // bit index. See Lower.cpp build_onehot_plan.
        Ctz,
        Clz,

        // -- Shifts (payload: binary) --
        // Shift left logical.
        Sll,
        // Shift right logical.
        Srl,
        // Shift right arithmetic.
        Sra,

        // -- Comparisons, -> 1-bit (payload: binary) --
        Eq,
        Ne,
        Lt,
        Le,
        Gt,
        Ge,

        // -- Case-equality, -> definite 1-bit (payload: binary) --
        // Unlike Eq, these never yield X: they compare the 4-state planes
        // directly and return a known 0/1, matching SV case-statement semantics.
        // CaseEq is `===` (every bit, incl. x/z, must match exactly); CasezEq /
        // CasexEq treat z (resp. x and z) bits in either operand as don't-care.
        CaseEq,
        CasezEq,
        CasexEq,

        // Wildcard equality `==?` (payload: binary). x/z bits in the RIGHT operand
        // are wildcards; unlike the case-equality tags this is X-propagating — a
        // left-operand x/z on a non-wildcard bit yields X.
        WildEq,
        // The `inside` operator as an expression (payload: op_extra ->
        // InsideSetInfo; op is the selector), -> 1-bit 4-state. The backend
        // expands the member tests and OR-reduces them; the statement twin
        // (`case ... inside`) has its own switch machinery in the frontend.
        InsideSet,

        // -- Floating point (real / shortreal; LRM 6.12) --
        // Operands and result share one float type: the frontend inserts all
        // int/real and real/shortreal conversions (LRM 11.8.2), so mixed
        // operands never reach these tags. Reals carry no x/z.
        // Arithmetic (payload: binary).
        FAdd,
        FSub,
        FMul,
        FDiv,
        // Power ** on reals (payload: binary). C pow semantics for the corners
        // LRM 11.4.3 leaves unspecified.
        FPow,
        // Negation -x (payload: unary). A real fneg, not Sub-from-zero: 0.0 - 0.0
        // is +0.0 but -(0.0) must be -0.0.
        FNeg,
        // Comparisons, -> definite 1-bit, never X (payload: binary). FNe is
        // an unordered compare so NaN != NaN yields 1, matching C.
        FEq,
        FNe,
        FLt,
        FLe,
        FGt,
        FGe,
        // Conversions (payload: op_type = operand + target type).
        // Integral -> float. Source signedness comes from the operand type;
        // x/z bits read as 0 (LRM 6.12.1).
        ItoF,
        // Float -> integral, rounding half away from zero (LRM 6.12.1) — the
        // assignment / cast conversion.
        FtoI,
        // Float -> integral, truncating toward zero — $rtoi (LRM 20.5).
        FtoITrunc,
        // Bit-pattern reinterpret between a float type and the same-width
        // integral type: $realtobits / $bitstoreal / $shortrealtobits /
        // $bitstoshortreal (LRM 20.5). Direction from operand vs target type.
        FBitcast,
        // real <-> shortreal value conversion (fpext / fptrunc, LRM 6.12.1).
        FResize,
        // One-operand real math system function — $ln / $sqrt / $sin / ...
        // (payload: op_imm — op is the real operand, imm the ir::RealMathOp).
        // LRM §20.8.2: each matches the C math library function.
        RealMathUnary,
        // Two-operand real math system function — $pow / $atan2 / $hypot
        // (payload: op_extra -> RealMathBinaryInfo; op is the first operand).
        RealMathBinary,

        // -- Logical, -> 1-bit --
        // Logical and (payload: binary).
        Land,
        // Logical or (payload: binary).
        Lor,
        // Logical not (payload: unary).
        Lnot,

        // -- Selection / structural --
        // Ternary cond ? a : b (payload: op_extra -> Select).
        Select,
        // Value-producing short-circuit conditional (payload: op_extra ->
        // SelectLazy). The op is a definite 2-state predicate; the then-branch
        // (predicate 1) and else-branch each yield a value, merged by the
        // backend. Used for &&/|| so the right operand is not evaluated when the
        // left already decides the result (LRM 11.3.5, 11.4.7). Lowers to
        // then/else/cont basic blocks with a PHI, like a lazy Select.
        SelectLazy,
        // Concatenation {a, b, ...} (payload: extra -> index slice).
        Concat,
        // Replication {count{value}} (payload: repl).
        Repl,
        // Streaming-concat block reversal {<<S{x}} (LRM 11.4.14.2): reverse
        // the order of the operand's S-bit blocks, sliced from the LSB (a
        // final partial block keeps its own width; block 0 lands at the MSB).
        // A pure per-plane bit permutation of the operand's width (payload:
        // op_imm — op is the built stream value, imm the slice size S).
        StreamReverse,

        // -- Width --
        // Resize (zero/sign extend or truncate) to a target type (payload: op_type).
        Resize,

        // -- Control flow --
        // If / else; condition + then/else regions (payload: op_extra -> Regions).
        Cond,
        // Multiway branch for a constant, deduped, wildcard-free `case` (payload:
        // op_extra -> SwitchInfo; the op is the selector value). Lowers to an
        // llvm switch over a packed key; unmatched keys (incl. any x/z selector)
        // run the default region. See SwitchInfo.
        Switch,
        // Return a value from a non-void function (payload: unary, the return
        // value). A real terminator: instructions after it in the same region
        // are unreachable. Early `return` works without any flag.
        Ret,
        // Return from a void function, task, or fork-child body (payload: none).
        // A terminator like Ret; tasks branch to their copy-out epilogue instead
        // of a real `ret`.
        RetVoid,
        // Call a function (payload: extra -> CallInfo). The result is this
        // instruction's value (absent for a void function called as a statement).
        Call,
        // Enable a task (payload: extra -> TaskCallInfo). A statement, never a
        // value: it creates the task coroutine, drives it, and blocks the calling
        // coroutine until the task completes (LRM 13.3). Only legal inside a
        // coroutine (a process or another task), never a plain function.
        TaskCall,
        // Loop with a per-iteration condition; `for` / `while` / `forever` all
        // lower to this (payload: extra -> LoopInfo). The `for` init runs in the
        // enclosing region before this inst; `while` / `forever` have empty step.
        // Lowers to header / body / step / exit blocks with a back-edge.
        Loop,
        // Loop control: `break` exits the innermost enclosing Loop (branches to
        // its exit block); `continue` skips to the innermost Loop's step block
        // (payload: none). Both are terminators — instructions after them in the
        // same region are unreachable. slang rejects either outside a loop.
        Break,
        Continue,
        // Time delay `#n`: suspend the process for `n` ticks (payload: unary, the
        // tick count). Lowers to a timer scheduled on the wheel plus a coroutine
        // suspend; the wheel resumes the process when the delay elapses.
        Delay,
        // Event wait `@(edge signal)` / `@(a or posedge b)`: suspend until ANY
        // listed (signal, edge) fires (payload: wait -> WaitInfo). Lowers to
        // registering the process on each event's wait queue plus a coroutine
        // suspend; a matching blocking assignment to one of the signals wakes it.
        Wait,
        // Blocking trigger of a named event `-> e` (payload: signal, the event's
        // synthetic signal). Records the trigger time (for `.triggered`) and
        // unconditionally services the event signal's Any-edge wait queues,
        // waking every `@(e)` / `always @(e)` waiter in the current instant
        // (LRM 15.5.2). An event has no value, so there is no edge computation.
        EventTrigger,
        // The `e.triggered` event property (payload: signal), -> a definite 1-bit
        // value: 1 iff the event was triggered in the current time step, else 0
        // (LRM 15.5.3). Lowers to a compare of the recorded trigger time against
        // the current sim time.
        EventTriggered,
        // Nonblocking trigger of a named event `->> e` (payload: signal). Unlike
        // EventTrigger, the wake is deferred to the NBA region of the current time
        // step (LRM 15.5.4): it marks the event pending; the NBA region then
        // stamps the trigger time and services the waiters. The triggering process
        // does not block.
        EventTriggerNb,
        // Delayed nonblocking trigger `->> #d e` (payload: op_extra -> op is the
        // delay in wheel ticks, extra -> EventDelayInfo names the event). Schedules
        // a detached wheel timer that, on expiry, stamps the trigger time and
        // services the event's waiters (LRM 15.5.4). The triggering process does
        // not block. The event-controlled form (`->> @(x) e`) is not lowered here.
        EventTriggerNbDelayed,
        // Fork block `fork … join` (payload: extra -> ForkInfo). A statement that
        // spawns each child body as its own coroutine (an argument-less fork-child
        // task), then — for `join` — suspends the parent until all children
        // complete. Only legal inside a coroutine (process or task). LRM 9.3.2.
        Fork,

        // -- String ops (operands are string values unless noted) --
        // s.len() -> 32-bit signed (payload: unary).
        StrLen,
        // s[i] / s.getc(i) -> 8-bit 2-state; 0 when i is out of range (payload:
        // binary — l is the string, r the index).
        StrGetc,
        // s[i] = c / s.putc(i, c): mutate a string lvalue in place; a no-op when
        // i is out of range or c's low byte is 0 (payload: op_extra ->
        // StrPutcInfo; the op is the string lvalue, like Store's lhs).
        StrPutc,
        // a.compare(b) / a.icompare(b) -> 32-bit signed, strcmp order (payload:
        // binary).
        StrCmp,
        StrICmp,
        // s.toupper() / s.tolower() -> new string value (payload: unary).
        StrUpper,
        StrLower,
        // s.substr(i, j) -> new string value; "" when the range is invalid
        // (payload: op_extra -> StrSubstrInfo; the op is the source string).
        StrSubstr,
        // e.name() -> new string value: the source name of the enum member whose
        // value equals the operand, or "" if none matches (LRM 6.19.2). Payload:
        // op_extra -> EnumNameInfo (the member table); op is the enum value.
        EnumName,
        // %p of a queue / dynamic array -> new string value: the runtime renders
        // '{e0, e1, ...} over the runtime element count (LRM 21.2.1.6). Payload:
        // op_type — op is the handle lvalue, type the interned Queue / DynArray
        // type (element format descriptor).
        ContainerFmtP,
        // string'(packed) -> string: bytes MSB-first with 0x00 bytes dropped
        // (payload: unary — the packed value).
        StrFromPacked,
        // Packed cast of a string: characters pack MSB-first, right-justified to
        // the target type's width (payload: op_type — op is the string, type the
        // target packed type).
        StrToPacked,
        // s.atoi() / s.atohex() / s.atooct() / s.atobin() -> 32-bit signed
        // `integer` (LRM 6.16.9: the declared type is 4-state, though the
        // conversion itself never yields x — it scans leading digits, and '_'
        // separators, of the given base and stops at the first other character
        // or end of string; 0 if none matched. No leading whitespace or sign
        // handling, and overflow truncates silently (payload: unary — the
        // source string).
        StrAtoi,
        StrAtohex,
        StrAtooct,
        StrAtobin,
        // s.atoreal() -> real (LRM 6.16.10): parses a real-literal-like prefix,
        // stopping at the first non-conforming character; 0.0 if none matched.
        // Unlike the atoi family, leading whitespace and an optional '+' are
        // consumed first (payload: unary).
        StrAtoreal,
        // s.itoa(i) / s.hextoa(i) / s.octtoa(i) / s.bintoa(i): format `i` (a
        // 4-state `integer`; x/z bits read as 0, the same must-plane
        // simplification as StrFromPacked) into the string lvalue as sign +
        // magnitude in the given base, lowercase digits (LRM 6.16.11-6.16.14;
        // payload: binary — l is the string lvalue, like Store's lhs, r is the
        // integer value).
        StrItoa,
        StrHextoa,
        StrOctoa,
        StrBintoa,
        // s.realtoa(r): format `r` into the string lvalue at full round-trip
        // precision (LRM 6.16.15; payload: binary — l the string lvalue, r the
        // real value).
        StrRealtoa,

        // -- System tasks --
        // $display / $write: format string + variadic args to standard output
        // (payload: display -> DisplayInfo).
        Display,
        // $fdisplay / $fwrite: like Display, but writes the MCD / FD given by
        // the descriptor operand (payload: fdisplay — op_extra whose op is the
        // descriptor and extra a DisplayInfo).
        FDisplay,
        // $strobe / $strobeb / $strobeo / $strobeh (LRM 21.2.2): register the
        // compiler-synthesized zero-formal task `func` (payload: extra ->
        // StrobePendInfo) to run once in the Postponed region of the current
        // time step, just before simulation time advances. `func`'s body holds
        // the format args' evaluation (a Display), not this instruction — the
        // whole point of $strobe is that evaluation itself is deferred to the
        // end of the time step, not merely the print, so nothing here is
        // re-evaluated later. A process containing this tag is forced to a
        // coroutine (see is_simple_always) so its call site can ramp `func`'s
        // coroutine frame through the task-call record ABI.
        StrobePend,
        // Two-argument $fopen(name, mode): open a single file descriptor with
        // the C stdio mode string, -> 32-bit FD value (payload: binary — l is
        // the filename value, r the mode string; both packed-ASCII vectors the
        // backend spills). LRM §21.3. Lowers to rt_fopen_fd.
        FopenFd,
        // One-argument $fopen(name): open a fresh multichannel descriptor for
        // writing, -> 32-bit MCD value (payload: unary, the filename value).
        // LRM §21.3. Lowers to rt_fopen_mcd.
        FopenMcd,
        // $fclose: close a file descriptor (payload: unary, the descriptor value).
        // LRM §21.3.2. Lowers to an rt_fclose call.
        Fclose,
        // -- File read / positioning system functions (LRM §21.3) --
        // Each -> a 32-bit integer, lowered to its rt_* call.
        // $fgetc(fd) -> next byte, or -1 at EOF (payload: unary, the fd).
        Fgetc,
        // $ungetc(c, fd) -> c, or -1 on failure (payload: binary — l is the
        // character, r the fd, in call order).
        Ungetc,
        // $feof(fd) -> nonzero once EOF has been seen (payload: unary).
        Feof,
        // $ftell(fd) -> current byte offset, or -1 (payload: unary).
        Ftell,
        // $fseek(fd, offset, operation) -> 0 on success, -1 on failure
        // (payload: op_extra -> FseekInfo; op is the fd).
        Fseek,
        // $rewind(fd) -> 0, seek to start (payload: unary).
        Rewind,
        // $fgets: read a line from a descriptor into a packed-vector lvalue, ->
        // character count (payload: binary — l is the destination lvalue, r the
        // descriptor value). LRM §21.3.4. Lowers to rt_fgets.
        Fgets,
        // $sformat / $swrite: format value args and store the result into a string
        // lvalue (payload: extra -> SformatInfo). LRM §21.3.3. Lowers to rt_sformat.
        Sformat,
        // $sformatf: format value args, -> new string value (payload: extra ->
        // SformatfInfo). LRM §21.3.3. Lowers to rt_sformat_str into the
        // instruction's string temp.
        Sformatf,
        // $ferror: write a descriptor's error message into a string lvalue, ->
        // error code (payload: binary — l is the destination lvalue, r the
        // descriptor value). LRM §21.3.6. Lowers to an rt_ferror call.
        Ferror,
        // $fscanf: parse an FD source into output lvalues per a format, ->
        // conversion count (payload: extra -> ScanfInfo, whose source is the FD
        // value). LRM §21.3.4. Lowers to an rt_fscanf call.
        Fscanf,
        // $sscanf: like Fscanf, but the source is a packed-ASCII string value
        // the backend spills (payload: extra -> ScanfInfo). Lowers to rt_sscanf.
        Sscanf,
        // $fread into a single packed-vector lvalue (payload: binary — l is the
        // destination lvalue, r the descriptor value), -> bytes read. LRM
        // §21.3.4. Lowers to an rt_fread call.
        FreadWord,
        // $fread into an unpacked-array memory (payload: op_extra -> FreadMemInfo;
        // op is the fd value), -> bytes read. LRM §21.3.4. Lowers to an rt_fread call.
        FreadMem,
        // $timeformat: set the %t format (units / precision / suffix / min width)
        // for all later %t specifiers (payload: timeformat -> TimeFormatInfo).
        // LRM §20.4.3. Lowers to an rt_timeformat_set call.
        TimeFormat,
        // $finish / $stop: end the simulation and exit (payload: extra ->
        // FinishInfo, carrying the diagnostic level and the compile-time banner
        // prefix). LRM §20.2 has $stop suspend and $finish exit; with no resume
        // mechanism a suspend is observationally an exit, so both are treated
        // identically apart from the banner's task name.
        Finish,
        // unique/priority violation report (payload: extra -> CaseViolInfo).
        // LRM 12.5.3 / 12.4.2. Non-fatal — lowers to an rt_case_violation call
        // that warns and returns.
        CaseViol,
        // Severity system task report ($info / $warning / $error / $fatal, LRM
        // 20.10) or the default failure of an immediate assertion (LRM 16.3),
        // both non-terminating (payload: extra -> SeverityInfo, carrying the
        // severity level, compile-time banner prefix, and $display-style format
        // + value args). Lowers to an rt_severity call that renders the message
        // and writes the labelled diagnostic to stderr, then returns. $fatal's
        // termination is a separate Finish the frontend emits after this report.
        Severity,
        // $fflush: flush a descriptor's output buffers (payload: unary, the
        // descriptor value; the no-argument form passes a constant 0, which the
        // runtime treats as "flush all"). LRM §21.3.6. Lowers to an rt_fflush call.
        Fflush,
        // $readmemh / $readmemb: load a hex/binary file into an unpacked-array
        // memory (payload: op_extra -> ReadMemInfo; op is the filename value). LRM
        // §21.4. Lowers to an rt_readmem call.
        ReadMem,

        // -- Value-change dump (VCD, LRM §21.7; see docs/VCD.md) --
        // $dumpfile("f.vcd"): set the output path (payload: unary, the filename
        // value). Lowers to an rt_vcd_open call.
        DumpFile,
        // $dumpvars: begin dumping (payload: none). Phase 0 supports only the
        // whole-design form ($dumpvars / $dumpvars(0)); a scope-subset selector is
        // rejected at the frontend. The backend enumerates every dumpable signal,
        // emits the descriptor table, and lowers this to an rt_vcd_start call.
        DumpVars,
        // $dumpoff / $dumpon / $dumpall / $dumpflush (payload: none). Suspend (with
        // an all-x checkpoint) / resume / force-dump-now / flush-to-disk. Each
        // lowers to the matching rt_vcd_* control call.
        DumpOff,
        DumpOn,
        DumpAll,
        DumpFlush,

        // -- System functions --
        // $time / $stime: read the simulation clock, -> 64-bit unsigned (payload:
        // scale). Lowers to a `rt_wheel_now` call; the value is always known (no
        // x/z). The wheel counts in the design's smallest time unit; the backend
        // rounds the tick count down by `scale` (the calling module's unit / the
        // smallest unit) to the module time unit (LRM §20.3.1). `scale` is 1 when
        // unitless or the module owns the smallest unit. $stime truncates the
        // result to 32 bits via a Resize.
        SimTime,
        // $random: IEEE Annex N reference PRNG, -> 32-bit signed two-state (payload:
        // none). Lowers to an `rt_random` call; the value is always known (no
        // x/z). The unseeded (process-global) form only; the seeded form
        // `$random(seed)` lowers to DistBinary(Uniform, ...) instead.
        Random,
        // A probabilistic distribution function (LRM §20.15.2), split by
        // parameter count; DistBinary also covers the seeded $random(seed)
        // (§20.15.1, Uniform over the full int32 span). Payload: op_extra —
        // op is the seed word, extra a DistUnaryInfo / DistBinaryInfo carrying
        // the parameter(s) and the ir::DistOp. Lowers to the matching
        // rt_dist_* call, returning an i64 with the updated seed packed in
        // [32,64) and the signed integer draw in [0,32); the frontend slices
        // the seed back to the inout lvalue and takes the draw. The result is
        // always known (no x/z).
        DistUnary,
        DistBinary,
        // $urandom (LRM §18.13.1): a per-process xoshiro256++ draw, -> a 32-bit
        // unsigned two-state `int`. No operand (payload: none); reads and
        // advances the process's coroutine-frame RNG state, inlined by the
        // backend. A process containing this tag is forced to a coroutine (it
        // needs the frame) — see is_simple_always.
        Urandom,
        // $urandom(seed) (LRM §18.13.1): reseed the process RNG from `seed`
        // (payload: unary) then draw, -> a 32-bit unsigned `int`.
        UrandomSeeded,
        // $urandom_range(maxval, minval) (LRM §18.13.2): an inlined Lemire
        // multiply-shift of one process RNG draw into [min, max] (payload:
        // binary l=maxval, r=minval; the backend swaps when min > max), -> a
        // 32-bit unsigned `int`.
        UrandomRange,
        // $clog2: ceil(log2(operand)) with the operand treated as unsigned and
        // $clog2(0) == 0, -> a 32-bit signed `integer` (payload: unary). LRM
        // §20.8.1. The operand's own width bounds the count; any x/z bit in it
        // makes the whole result X (iverilog oracle — the LRM is silent). Lowers
        // to a count-leading-zeros: `operand == 0 ? 0 : W - clz(operand - 1)`.
        Clog2,
        // $countbits / $countones / $onehot / $onehot0 / $isunknown building
        // block (payload: op_extra -> CountBitsInfo; the op is the counted
        // value). Tallies the operand's bits whose 4-state value is in the
        // info mask (LRM 20.9), -> a definite 2-state 32-bit `int`. $countones
        // masks CountOne; $isunknown is `CountBits(CountX|CountZ) != 0`; the
        // one-hot forms have their own tags below. A constant operand folds in
        // slang and never reaches this tag.
        CountBits,
        // $onehot / $onehot0: exactly-one / at-most-one definite-1 bit in the
        // operand (LRM 20.9), -> a definite 1-bit result (payload: unary).
        // x/z bits are not definite 1, so they never count.
        OneHot,
        OneHot0,
        // $isunknown: nonzero iff any operand bit is x/z (LRM 20.9), -> a
        // definite 1-bit result (payload: unary).
        IsUnknown,
        // $test$plusargs(str) (payload: extra -> TestPlusArgsInfo), -> a 1-bit
        // definite boolean. LRM §21.6: nonzero iff some command-line plusarg's
        // prefix matches every character of `str`. Lowers to an
        // rt_test_plusargs call.
        TestPlusArgs,
        // $value$plusargs(user_string, var) (payload: op_extra ->
        // ValuePlusArgsInfo; op is the output lvalue), -> a 1-bit definite
        // boolean. LRM §21.6: on a matching plusarg, converts its remainder per
        // the user_string's format character into the lvalue and returns
        // nonzero; otherwise leaves the lvalue untouched and returns 0. Lowers
        // to an rt_value_plusargs call.
        ValuePlusArgs,
    };

    // Which union member a tag activates. Drives payload_kind() / operands().
    enum class PayloadKind : uint8_t {
        None,
        // The u64 scale member (sim_time). No SSA operands.
        Scale,
        Ref,
        Signal,
        Unary,
        Binary,
        OpType,
        Arg,
        Local,
        Repl,
        // The op_imm member: one operand plus a 32-bit immediate.
        OpImm,
        OpExtra,
        Extra,
        Call,
        TaskCall,
        Display,
        // The op_extra member with a DisplayInfo record: op is the descriptor.
        FDisplay,
        TimeFormat,
        Wait,
        Sformat,
        Sformatf,
        Scanf,
        Severity,
    };

    union Payload {
        // Inactive / nullary placeholder.
        uint32_t none;
        // Time-unit multiplier for sim_time: $time returns the wheel tick count
        // rounded down by this to the calling module's time unit. 1 == unitless.
        uint64_t scale;
        // Interned constant value (constant).
        Ref ref;
        // Module signal handle (signal).
        Signal signal;
        // Single operand (load, bnot, lnot, red_*, ret).
        Index unary;
        // Two operands (arithmetic, bitwise, shift, compare, store, bit_select).
        struct {
            // Left operand.
            Index l;
            // Right operand.
            Index r;
        } binary;
        // Operand plus a target type (resize).
        struct {
            // Value being converted.
            Index op;
            // Target type.
            Ref type;
        } op_type;
        // Parameter position plus its type (arg).
        struct {
            // Zero-based argument position.
            uint32_t position;
            // Parameter type.
            Ref type;
        } arg;
        // Function-frame local slot index plus its type (local).
        struct {
            // Index into the function's local table / frame layout.
            uint32_t index;
            // Storage type of the local.
            Ref type;
        } local;
        // Replicated value plus a constant count (repl).
        struct {
            // Value being replicated.
            Index value;
            // Replication count.
            uint32_t count;
        } repl;
        // One operand plus a 32-bit immediate (real_math_unary's RealMathOp,
        // stream_reverse's slice size).
        struct {
            // The operand value.
            Index op;
            // Tag-defined immediate.
            uint32_t imm;
        } op_imm;
        // Primary operand plus a handle to an extra struct (select, part_select, cond).
        struct {
            // Condition / base operand.
            Index op;
            // Handle to the op's extra payload.
            ExtraIndex extra;
        } op_extra;
        // Handle to a variable-length operand slice (concat) or to a tag's
        // extra record with no primary operand (loop, call, display, wait,
        // timeformat, ...). The record type is selected by the tag's
        // PayloadKind.
        ExtraIndex extra;
    };

    // Discriminant selecting the active payload member.
    Tag tag;
    // Per-tag operand data.
    Payload payload;
};

static_assert(sizeof(Inst::Payload) <= 8, "payload must stay compact");
static_assert(std::is_trivially_copyable_v<Inst::Payload>);

// Maps an instruction tag to the active payload union member. Constexpr in the
// header so per-instruction dispatch (printer, operand walks, passes) inlines
// to a dense-switch lookup instead of an out-of-line call.
constexpr Inst::PayloadKind payload_kind(Inst::Tag tag) {
    using T = Inst::Tag;
    using K = Inst::PayloadKind;
    switch (tag) {
        case T::Constant:
            return K::Ref;
        case T::Signal:
        case T::EventTrigger:
        case T::EventTriggered:
        case T::EventTriggerNb:
            return K::Signal;
        case T::Arg:
            return K::Arg;
        case T::Local:
            return K::Local;

        case T::Load:
        case T::Bnot:
        case T::RedAnd:
        case T::RedOr:
        case T::RedXor:
        case T::Lnot:
        case T::FNeg:
        case T::Ret:
        case T::Delay:
        case T::Fclose:
        case T::FopenMcd:
        case T::Fgetc:
        case T::Feof:
        case T::Ftell:
        case T::Rewind:
        case T::Fflush:
        case T::DumpFile:
        case T::StrLen:
        case T::StrUpper:
        case T::StrLower:
        case T::StrFromPacked:
        case T::StrAtoi:
        case T::StrAtohex:
        case T::StrAtooct:
        case T::StrAtobin:
        case T::StrAtoreal:
        case T::Clog2:
        case T::Ctz:
        case T::Clz:
        case T::OneHot:
        case T::OneHot0:
        case T::IsUnknown:
        case T::UrandomSeeded:
            return K::Unary;

        case T::BitSelect:
        case T::Store:
        case T::StoreNb:
        case T::Add:
        case T::Sub:
        case T::Mul:
        case T::Div:
        case T::Mod:
        case T::Pow:
        case T::Band:
        case T::Bor:
        case T::Bxor:
        case T::Bxnor:
        case T::WireRes:
        case T::WandRes:
        case T::WorRes:
        case T::Sll:
        case T::Srl:
        case T::Sra:
        case T::Eq:
        case T::Ne:
        case T::Lt:
        case T::Le:
        case T::Gt:
        case T::Ge:
        case T::CaseEq:
        case T::CasezEq:
        case T::CasexEq:
        case T::WildEq:
        case T::Land:
        case T::Lor:
        case T::StrGetc:
        case T::StrCmp:
        case T::StrICmp:
        case T::StrItoa:
        case T::StrHextoa:
        case T::StrOctoa:
        case T::StrBintoa:
        case T::StrRealtoa:
        case T::FAdd:
        case T::FSub:
        case T::FMul:
        case T::FDiv:
        case T::FPow:
        case T::FEq:
        case T::FNe:
        case T::FLt:
        case T::FLe:
        case T::FGt:
        case T::FGe:
        case T::UrandomRange:
        // The write-back file reads: l is the destination lvalue (like Store's
        // lhs), r the descriptor value.
        case T::Fgets:
        case T::Ferror:
        case T::FreadWord:
        // Two-argument $fopen: l is the filename, r the mode string.
        case T::FopenFd:
        // $ungetc: l is the character, r the descriptor.
        case T::Ungetc:
            return K::Binary;

        case T::Resize:
        case T::StrToPacked:
        case T::ItoF:
        case T::FtoI:
        case T::FtoITrunc:
        case T::FBitcast:
        case T::FResize:
        case T::HandleDeref:
        // Handle-lvalue queue / dynamic-array ops with no extra operand: op is
        // the handle, type the interned Queue / DynArray type.
        case T::QueueSize:
        case T::QueuePopBack:
        case T::QueuePopFront:
        case T::QueueDeleteAll:
        case T::ContainerFmtP:
        case T::DynArraySize:
        case T::DynArrayDelete:
        // Array ordering methods (LRM 7.12.2): op is the handle, type the
        // interned DynArray / Queue type; no extra operand.
        case T::DynArrayReverse:
        case T::DynArraySort:
        case T::DynArrayRsort:
        case T::DynArrayShuffle:
        case T::QueueReverse:
        case T::QueueSort:
        case T::QueueRsort:
        case T::QueueShuffle:
        // Associative-array ops with no extra operand: op is the handle, type
        // the interned Assoc type.
        case T::AssocNum:
        case T::AssocDeleteAll:
            return K::OpType;
        case T::ClassNew:
            return K::Ref;
        case T::Repl:
            return K::Repl;
        case T::Concat:
        case T::Loop:
            return K::Extra;
        case T::Call:
            return K::Call;
        case T::TaskCall:
            return K::TaskCall;
        case T::Display:
            return K::Display;
        case T::FDisplay:
            return K::FDisplay;
        case T::Sformat:
            return K::Sformat;
        case T::Sformatf:
            return K::Sformatf;
        case T::Fscanf:
        case T::Sscanf:
            return K::Scanf;
        case T::RealMathUnary:
        case T::StreamReverse:
        case T::NetPull:
            return K::OpImm;
        case T::TimeFormat:
            return K::TimeFormat;
        case T::Break:
        case T::Continue:
        case T::RetVoid:
        case T::DumpVars:
        case T::DumpOff:
        case T::DumpOn:
        case T::DumpAll:
        case T::DumpFlush:
        case T::Random:
        case T::Urandom:
            return K::None;
        case T::SimTime:
            return K::Scale;
        case T::Finish:
        case T::CaseViol:
        case T::TestPlusArgs:
        case T::StrobePend:
            return K::Extra;
        case T::Fork:
            // Children are FunctionIds (not SSA operands), walked from ForkInfo;
            // the op has no top-level operands, so it rides the Extra path.
            return K::Extra;
        case T::Severity:
            return K::Severity;
        case T::Wait:
            return K::Wait;

        case T::StoreNbDelay:
        case T::StoreNbEvent:
        case T::Select:
        case T::SelectLazy:
        case T::PartSelect:
        case T::DynPartSelect:
        case T::ElemSelect:
        case T::ArrSlice:
        case T::MemberSelect:
        case T::AggCopy:
        case T::AggEq:
        case T::DynArrayNew:
        case T::DynArrayNewInit:
        case T::DynArrayCopy:
        case T::QueuePushBack:
        case T::QueuePushFront:
        case T::QueueInsert:
        case T::QueueDeleteIdx:
        case T::QueueCopy:
        case T::QueueCopyNb:
        case T::QueueAppendRange:
        case T::AssocSelect:
        case T::AssocExists:
        case T::AssocDeleteKey:
        case T::AssocCopy:
        case T::AssocFirst:
        case T::AssocLast:
        case T::AssocNext:
        case T::AssocPrev:
        case T::Cond:
        case T::Switch:
        case T::Fseek:
        case T::RealMathBinary:
        case T::DistUnary:
        case T::DistBinary:
        case T::InsideSet:
        case T::FreadMem:
        case T::ReadMem:
        case T::StrPutc:
        case T::StrSubstr:
        case T::EnumName:
        case T::CountBits:
        case T::ValuePlusArgs:
        case T::EventTriggerNbDelayed:
            return K::OpExtra;
    }

    return K::None;
}

// Immutable, finished process IR: a structure of arrays (parallel tag / payload
// / location columns indexed by Index) plus a flat extra side-table. The body
// is the root region. Constructed by Builder::finish(); default construction
// yields an inert empty body (no pool), so an Ir-holding record (Function,
// Process) can be slotted before its body is built and move-assigned later.
class Ir {
public:
    Ir() = default;

    // Intern pool backing this IR's constant / type handles. Valid only on a
    // built Ir (Builder::finish()), never on a default-constructed one.
    const InternPool& pool() const {
        return *pool_;
    }

    // Total number of instructions across all regions.
    size_t inst_count() const {
        return tags_.size();
    }

    Inst::Tag tag(Index i) const {
        return tags_[static_cast<uint32_t>(i)];
    }

    const Inst::Payload& payload(Index i) const {
        return payloads_[static_cast<uint32_t>(i)];
    }

    // Opaque source location of an instruction.
    SourceLoc loc(Index i) const {
        return locs_[static_cast<uint32_t>(i)];
    }

    // The top-level instruction sequence of the process body.
    std::span<const Index> root() const {
        return index_slice(root_);
    }

    // The whole extra side-table, raw. For content hashing / serialization
    // (regions, operand runs, packed strings, records — byte-stable since the
    // padding-free ExtraPayload guarantee); typed access stays extra_data().
    std::span<const uint32_t> extra_raw() const {
        return { extra_.data(), extra_.size() };
    }

    // The instructions of a region (e.g. a cond's then/else branch), in order.
    std::span<const Index> region(ExtraSlice r) const {
        return index_slice(r);
    }

    // Decode a struct previously written with Builder::add_extra.
    template<ExtraPayload T>
    T extra_data(ExtraIndex at) const {
        constexpr size_t        n    = sizeof(T) / sizeof(uint32_t);
        const auto              base = static_cast<uint32_t>(at);
        std::array<uint32_t, n> words{};
        for (size_t i = 0; i < n; ++i) {
            words[i] = extra_[base + i];
        }

        return std::bit_cast<T>(words);
    }

    // Decode `len` raw bytes previously packed at `at` by Builder (little-endian,
    // 4 chars per word). Used for $display format strings.
    std::string extra_bytes(ExtraIndex at, uint32_t len) const {
        std::string s(len, '\0');
        const auto  base = static_cast<uint32_t>(at);
        for (uint32_t i = 0; i < len; ++i) {
            s[i] = static_cast<char>((extra_[base + i / 4] >> (8 * (i % 4))) & 0xff);
        }

        return s;
    }

    // The (signal, edge) events of a `wait` instruction's WaitInfo, in order,
    // viewed directly over the extra buffer: WaitEdge is two u32-sized fields
    // (ExtraPayload), so the packed records overlay the words exactly. Valid
    // while this Ir lives, like every other extra view.
    std::span<const WaitEdge> wait_edges(ExtraIndex info) const {
        const WaitInfo     wi     = extra_data<WaitInfo>(info);
        const auto         start  = static_cast<uint32_t>(wi.events.start);
        constexpr uint32_t stride = sizeof(WaitEdge) / sizeof(uint32_t);
        const uint32_t     count  = (static_cast<uint32_t>(wi.events.end) - start) / stride;

        return { reinterpret_cast<const WaitEdge*>(extra_.data() + start), count };
    }

    // Fill `scratch` with the SSA operands of an instruction and return a view
    // of them. Fixed-arity ops use the scratch buffer; variadic ops (concat)
    // return a view straight into the extra buffer. Region bodies are not
    // operands — walk them via region().
    //
    // NOTE: not exhaustive for the variadic system tasks — a tag whose args are
    // an extra slice can also carry a scalar operand outside it, which one
    // span cannot express: FDisplay omits its fd, Fscanf/Sscanf their source,
    // InsideSet its member operands. A pass needing every use of those tags
    // must read their info records directly.
    std::span<const Index> operands(Index inst, std::array<Index, 4>& scratch) const;

private:
    friend class Builder;

    // A slice reinterpreted as a run of instruction indices.
    //
    // NOTE: viewing u32 extra words as enum-class handles (here and in
    // wait_edges) is formally an aliasing violation — an enum is not
    // aliasing-compatible with its underlying type ([basic.lval]). Accepted
    // deliberately: the representation is identical, every compiler this
    // builds on reads it as intended, and the alternative (per-element
    // copies) costs the zero-copy view this layout exists for.
    std::span<const Index> index_slice(ExtraSlice s) const {
        const auto a = static_cast<uint32_t>(s.start);
        const auto b = static_cast<uint32_t>(s.end);

        return { reinterpret_cast<const Index*>(extra_.data() + a), b - a };
    }

    // Constructed atomically by Builder::finish().
    Ir(const InternPool&          pool,
       std::vector<Inst::Tag>     tags,
       std::vector<Inst::Payload> payloads,
       std::vector<SourceLoc>     locs,
       std::vector<uint32_t>      extra,
       ExtraSlice                 root)
      : pool_(&pool)
      , tags_(std::move(tags))
      , payloads_(std::move(payloads))
      , locs_(std::move(locs))
      , extra_(std::move(extra))
      , root_(root) {}

    // Intern pool backing this IR's handles; null only when default-constructed.
    const InternPool* pool_ = nullptr;
    // Instruction tags.
    Array<Inst::Tag> tags_;
    // Per-instruction payloads, parallel to tags_.
    Array<Inst::Payload> payloads_;
    // Per-instruction opaque source locations, parallel to tags_.
    Array<SourceLoc> locs_;
    // Flat u32 side-table for region bodies, operand slices, and extra structs.
    Array<uint32_t> extra_;
    // Top-level region: the process body (empty when default-constructed).
    ExtraSlice root_{};
};

// Result of walking an lvalue's select chain to its root storage leaf.
struct LvalueRoot {
    // The root leaf (a Signal / Local / Arg — whatever the chain bottoms out
    // on; callers switch on its tag).
    Index root;
    // False when any select was traversed: the lvalue names a bit / part /
    // element of the root, not the whole storage.
    bool whole;
};

// Walk an lvalue select chain (bit / part / element selects) to its root leaf.
// The single walker every lvalue-classifying pass shares (NBA targeting, task
// copy-out targets, may-z storage resolution), so they cannot drift apart when
// a select form is added.
LvalueRoot lvalue_root(const Ir& ir, Index lv);

} // namespace clotho::ir
