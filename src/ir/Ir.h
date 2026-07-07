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

// Which file-I/O system function a `file_value_op` invokes (LRM §21.3). Each
// returns a 32-bit integer and takes its operands (a file descriptor, plus a
// character or offset) as plain value args. Kept in lockstep with the runtime
// dispatch in LlvmBackend.
enum class FileOp : uint32_t {
    // $fgetc(fd) -> next byte, or -1 at EOF.
    Fgetc,
    // $ungetc(c, fd) -> c, or -1 on failure.
    Ungetc,
    // $feof(fd) -> nonzero once EOF has been seen.
    Feof,
    // $ftell(fd) -> current byte offset, or -1.
    Ftell,
    // $fseek(fd, offset, operation) -> 0 on success, -1 on failure.
    Fseek,
    // $rewind(fd) -> 0 (seek to start).
    Rewind,
};

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

// A struct may be overlaid on the extra buffer iff it is trivially copyable
// and a whole number of u32 words wide. Because every field is a 4-byte
// handle/enum, such structs carry no padding and round-trip via bit_cast.
template<class T>
concept ExtraPayload = std::is_trivially_copyable_v<T> && (sizeof(T) % sizeof(uint32_t) == 0);

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
    // True for $readmemh, false for $readmemb.
    bool hex;
    // Whether the start / finish operands are present.
    bool has_start;
    bool has_finish;
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

// Extra payload for `cond`: the then / else regions (the condition is the op).
struct Regions {
    // Instructions executed when the condition is true.
    ExtraSlice then_;
    // Instructions executed when the condition is false.
    ExtraSlice else_;
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
    // do-while: run the body once before the first predicate test. The backend
    // branches entry -> body instead of entry -> header (LRM 12.7.1).
    bool bottom_test;
};

// Extra payload for `display`: a $display / $write / $fdisplay / $fwrite system
// task. The format-string bytes live packed little-endian (4 chars per u32 word)
// at `fmt` for `fmt_len` bytes; `args` is the slice of value operands the format
// specifiers consume, in order; `newline` is 1 for the $*display forms (append
// '\n') and 0 for the $*write forms. `fd` is the destination descriptor operand
// for the $f* forms; when `has_fd` is 0 ($display / $write) the backend writes to
// the standard-output MCD and `fd` is unused.
struct DisplayInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // 1 => append a trailing newline ($display / $fdisplay); 0 => none.
    uint32_t newline;
    // Destination descriptor operand ($fdisplay / $fwrite); valid iff has_fd.
    Index fd;
    // 1 => fd is a real descriptor operand; 0 => write to standard output.
    uint32_t has_fd;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
};

// Extra payload for `fopen`: the $fopen(name, mode) form. `mode` is the C stdio
// mode-string operand (a packed-ASCII vector, spilled by the backend) and is
// valid iff `has_mode`; the name value is the instruction's `op`. With a mode a
// single FD is opened, without one a fresh MCD is opened for writing.
struct FopenInfo {
    // C stdio mode-string operand; valid iff has_mode.
    Index mode;
    // 1 => two-argument $fopen (FD); 0 => one-argument $fopen (MCD).
    uint32_t has_mode;
};

// Extra payload for `file_value_op`: a file-I/O system function returning a
// 32-bit integer. `op` is the ir::FileOp selecting the routine; `args` are its
// value operands in call order (a descriptor, plus a character / offset).
struct FileOpInfo {
    // ir::FileOp value.
    uint32_t op;
    // Value operands in call order.
    ExtraSlice args;
};

// Extra payload for `real_math`: a real math system function (LRM §20.8.2).
// `op` is the ir::RealMathOp selecting the routine; `args` are its real value
// operands in call order (one for most, two for Pow / Atan2 / Hypot).
struct RealMathInfo {
    // ir::RealMathOp value.
    uint32_t op;
    // Real value operands in call order.
    ExtraSlice args;
};

// Extra payload for `fgets`: $fgets(str, fd) (LRM §21.3.4) reads a line from the
// descriptor `fd` into the packed-vector lvalue `str` (the instruction's `op`),
// returning the character count. `fd` is a value operand.
struct FgetsInfo {
    // File-descriptor value operand.
    Index fd;
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

// Extra payload for `ferror`: $ferror(fd, str) (LRM §21.3.6) writes the
// descriptor's error message into the string lvalue (the instruction's `op`) and
// returns the error code. `fd` is a value operand.
struct FerrorInfo {
    // File-descriptor value operand.
    Index fd;
};

// Extra payload for `scanf`: $fscanf / $sscanf (LRM §21.3.4) parse `source` per the
// packed format-string bytes at `fmt` (`fmt_len` bytes) into the output lvalues
// `args`, returning the conversion count. When `is_string` is 1 ($sscanf) `source`
// is a packed-ASCII string value the backend spills; otherwise ($fscanf) it is a
// file-descriptor value.
struct ScanfInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // Source operand: an FD value ($fscanf) or a packed string value ($sscanf).
    Index source;
    // 1 => $sscanf (source is a packed string); 0 => $fscanf (source is an FD).
    uint32_t is_string;
    // Output lvalue operands, one per conversion, in order.
    ExtraSlice args;
};

// Extra payload for `fread_word`: $fread(reg, fd) (LRM §21.3.4) reads raw bytes into
// the packed-vector lvalue `reg` (the instruction's `op`). `fd` is a value operand.
struct FreadWordInfo {
    // File-descriptor value operand.
    Index fd;
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

static_assert(ExtraPayload<ExtraSlice>);
static_assert(ExtraPayload<CallInfo>);
static_assert(ExtraPayload<TaskCallInfo>);
static_assert(ExtraPayload<Select>);
static_assert(ExtraPayload<PartSelect>);
static_assert(ExtraPayload<DynPartSelect>);
static_assert(ExtraPayload<ElemSelect>);
static_assert(ExtraPayload<ReadMemInfo>);
static_assert(ExtraPayload<Regions>);
static_assert(ExtraPayload<LoopInfo>);
static_assert(ExtraPayload<DisplayInfo>);
static_assert(ExtraPayload<FopenInfo>);
static_assert(ExtraPayload<FileOpInfo>);
static_assert(ExtraPayload<RealMathInfo>);
static_assert(ExtraPayload<FgetsInfo>);
static_assert(ExtraPayload<SformatInfo>);
static_assert(ExtraPayload<SformatfInfo>);
static_assert(ExtraPayload<FerrorInfo>);
static_assert(ExtraPayload<ScanfInfo>);
static_assert(ExtraPayload<FreadWordInfo>);
static_assert(ExtraPayload<FreadMemInfo>);
static_assert(ExtraPayload<TimeFormatInfo>);
static_assert(ExtraPayload<WaitEdge>);
static_assert(ExtraPayload<WaitInfo>);
static_assert(ExtraPayload<StrPutcInfo>);
static_assert(ExtraPayload<StrSubstrInfo>);

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

        // -- State access --
        // Read the current value of an lvalue (payload: unary).
        Load,
        // Blocking assignment, lhs = rhs (payload: binary).
        Store,
        // Nonblocking assignment, lhs <= rhs (payload: binary).
        StoreNb,

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

        // -- Reductions, e.g. &x (payload: unary) --
        RedAnd,
        RedOr,
        RedXor,

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
        // Real math system function $sqrt / $sin / ... (payload: extra ->
        // RealMathInfo). LRM §20.8.2: each matches the C math library function.
        RealMath,

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
        // Select). The op is a definite 2-state predicate; the then-branch
        // (predicate 1) and else-branch each yield a value, merged by the
        // backend. Used for &&/|| so the right operand is not evaluated when the
        // left already decides the result (LRM 11.3.5, 11.4.7). Lowers to
        // then/else/cont basic blocks with a PHI, like a lazy Select.
        SelectLazy,
        // Concatenation {a, b, ...} (payload: extra -> index slice).
        Concat,
        // Replication {count{value}} (payload: repl).
        Repl,

        // -- Width --
        // Resize (zero/sign extend or truncate) to a target type (payload: op_type).
        Resize,

        // -- Control flow --
        // If / else; condition + then/else regions (payload: op_extra -> Regions).
        Cond,
        // Return from a function (payload: unary, the return value; ignored for a
        // void function). A real terminator: instructions after it in the same
        // region are unreachable. Early `return` works without any flag.
        Ret,
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
        // string'(packed) -> string: bytes MSB-first with 0x00 bytes dropped
        // (payload: unary — the packed value).
        StrFromPacked,
        // Packed cast of a string: characters pack MSB-first, right-justified to
        // the target type's width (payload: op_type — op is the string, type the
        // target packed type).
        StrToPacked,

        // -- System tasks --
        // $display / $write / $fdisplay / $fwrite: format string + variadic args,
        // with an optional destination descriptor (payload: display -> DisplayInfo).
        Display,
        // $fopen: open a file, -> 32-bit descriptor value (payload: op_extra ->
        // FopenInfo; op is the filename value). LRM §21.3. One-argument form yields
        // an MCD, two-argument form yields an FD. Lowers to rt_fopen_mcd/_fd.
        Fopen,
        // $fclose: close a file descriptor (payload: unary, the descriptor value).
        // LRM §21.3.2. Lowers to an rt_fclose call.
        Fclose,
        // $fgetc / $ungetc / $feof / $ftell / $fseek / $rewind: file read /
        // positioning system functions, each -> a 32-bit integer (payload: extra ->
        // FileOpInfo, whose `op` picks the routine and `args` are its value
        // operands). LRM §21.3. Lowers to the matching rt_* call.
        FileValueOp,
        // $fgets: read a line from a descriptor into a packed-vector lvalue, ->
        // character count (payload: op_extra -> FgetsInfo; op is the destination
        // lvalue, extra holds the descriptor value). LRM §21.3.4. Lowers to rt_fgets.
        Fgets,
        // $sformat / $swrite: format value args and store the result into a string
        // lvalue (payload: extra -> SformatInfo). LRM §21.3.3. Lowers to rt_sformat.
        Sformat,
        // $sformatf: format value args, -> new string value (payload: extra ->
        // SformatfInfo). LRM §21.3.3. Lowers to rt_sformat_str into the
        // instruction's string temp.
        Sformatf,
        // $ferror: write a descriptor's error message into a string lvalue, ->
        // error code (payload: op_extra -> FerrorInfo; op is the destination
        // lvalue). LRM §21.3.6. Lowers to an rt_ferror call.
        Ferror,
        // $fscanf / $sscanf: parse a source into output lvalues per a format, ->
        // conversion count (payload: extra -> ScanfInfo). LRM §21.3.4. Lowers to an
        // rt_fscanf / rt_sscanf call.
        Scanf,
        // $fread into a single packed-vector lvalue (payload: op_extra ->
        // FreadWordInfo; op is the destination lvalue), -> bytes read. LRM §21.3.4.
        // Lowers to an rt_fread call.
        FreadWord,
        // $fread into an unpacked-array memory (payload: op_extra -> FreadMemInfo;
        // op is the fd value), -> bytes read. LRM §21.3.4. Lowers to an rt_fread call.
        FreadMem,
        // $timeformat: set the %t format (units / precision / suffix / min width)
        // for all later %t specifiers (payload: timeformat -> TimeFormatInfo).
        // LRM §20.4.3. Lowers to an rt_timeformat_set call.
        TimeFormat,
        // $finish / $stop: end the simulation and exit (payload: none). LRM §20.2
        // has $stop suspend and $finish exit; with no resume mechanism a suspend
        // is observationally an exit, so both are treated identically.
        Finish,
        // $fflush: flush a descriptor's output buffers (payload: unary, the
        // descriptor value; the no-argument form passes a constant 0, which the
        // runtime treats as "flush all"). LRM §21.3.6. Lowers to an rt_fflush call.
        Fflush,
        // $readmemh / $readmemb: load a hex/binary file into an unpacked-array
        // memory (payload: op_extra -> ReadMemInfo; op is the filename value). LRM
        // §21.4. Lowers to an rt_readmem call.
        ReadMem,

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
        // x/z). The seeded form `$random(seed)` is rejected by the frontend, so
        // this tag never carries a seed operand.
        Random,
        // $clog2: ceil(log2(operand)) with the operand treated as unsigned and
        // $clog2(0) == 0, -> a 32-bit signed `integer` (payload: unary). LRM
        // §20.8.1. The operand's own width bounds the count; any x/z bit in it
        // makes the whole result X (iverilog oracle — the LRM is silent). Lowers
        // to a count-leading-zeros: `operand == 0 ? 0 : W - clz(operand - 1)`.
        Clog2,
    };

    // Which union member a tag activates. Drives payload_kind() / operands().
    enum class PayloadKind : uint8_t {
        None,
        Ref,
        Signal,
        Unary,
        Binary,
        OpType,
        Arg,
        Local,
        Repl,
        OpExtra,
        Extra,
        Call,
        TaskCall,
        Display,
        TimeFormat,
        Wait,
        FileOp,
        Sformat,
        Sformatf,
        Scanf,
        RealMath,
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
        // Primary operand plus a handle to an extra struct (select, part_select, cond).
        struct {
            // Condition / base operand.
            Index op;
            // Handle to the op's extra payload.
            ExtraIndex extra;
        } op_extra;
        // Handle to a variable-length operand slice (concat).
        ExtraIndex extra;
        // Handle to a DisplayInfo extra record (display).
        ExtraIndex display;
        // Handle to a TimeFormatInfo extra record (timeformat).
        ExtraIndex timeformat;
        // Handle to a WaitInfo extra record (wait): the list of (signal, edge)
        // events to suspend on. The process wakes when ANY of them fires. No SSA
        // operands.
        ExtraIndex wait;
    };

    // Discriminant selecting the active payload member.
    Tag tag;
    // Per-tag operand data.
    Payload payload;
};

static_assert(sizeof(Inst::Payload) <= 8, "payload must stay compact");
static_assert(std::is_trivially_copyable_v<Inst::Payload>);

// Maps an instruction tag to the active payload union member.
Inst::PayloadKind payload_kind(Inst::Tag tag);

// Immutable, finished process IR: a structure of arrays (parallel tag / payload
// / location columns indexed by Index) plus a flat extra side-table. The body
// is the root region. Constructed by Builder::finish().
class Ir {
public:
    // Intern pool backing this IR's constant / type handles.
    const InternPool& pool;

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

    // Decode the (signal, edge) events of a `wait` instruction's WaitInfo. The
    // events are packed WaitEdge records (each two words wide) in the extra
    // buffer; this returns them in order.
    std::vector<WaitEdge> wait_edges(ExtraIndex info) const {
        const WaitInfo        wi    = extra_data<WaitInfo>(info);
        const auto            start = static_cast<uint32_t>(wi.events.start);
        const auto            end   = static_cast<uint32_t>(wi.events.end);
        constexpr uint32_t    stride = sizeof(WaitEdge) / sizeof(uint32_t);
        std::vector<WaitEdge> out;
        for (uint32_t w = start; w < end; w += stride) {
            out.push_back(extra_data<WaitEdge>(static_cast<ExtraIndex>(w)));
        }

        return out;
    }

    // Fill `scratch` with the SSA operands of an instruction and return a view
    // of them. Fixed-arity ops use the scratch buffer; variadic ops (concat)
    // return a view straight into the extra buffer. Region bodies are not
    // operands — walk them via region().
    std::span<const Index> operands(Index inst, std::array<Index, 3>& scratch) const;

private:
    friend class Builder;

    // A slice reinterpreted as a run of instruction indices.
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
      : pool(pool)
      , tags_(std::move(tags))
      , payloads_(std::move(payloads))
      , locs_(std::move(locs))
      , extra_(std::move(extra))
      , root_(root) {}

    // Instruction tags.
    Array<Inst::Tag> tags_;
    // Per-instruction payloads, parallel to tags_.
    Array<Inst::Payload> payloads_;
    // Per-instruction opaque source locations, parallel to tags_.
    Array<SourceLoc> locs_;
    // Flat u32 side-table for region bodies, operand slices, and extra structs.
    Array<uint32_t> extra_;
    // Top-level region: the process body.
    ExtraSlice root_;
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
