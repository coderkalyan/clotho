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

// Extra payload for `mux`: the two selected values (the condition is the op).
struct Mux {
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

// Extra payload for `call`: the callee plus its already-lowered argument values,
// in formal-parameter order (each resized to the parameter type by the frontend).
struct CallInfo {
    // Function being called, resolved against the module's function table.
    FunctionId func;
    // The argument value operands, in order.
    ExtraSlice args;
};

// Extra payload for `cond`: the then / else regions (the condition is the op).
struct Regions {
    // Instructions executed when the condition is true.
    ExtraSlice then_;
    // Instructions executed when the condition is false.
    ExtraSlice else_;
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
};

// Extra payload for `display`: a $display / $write system task. The format-string
// bytes live packed little-endian (4 chars per u32 word) at `fmt` for `fmt_len`
// bytes; `args` is the slice of value operands the format specifiers consume, in
// order; `newline` is 1 for $display (append '\n') and 0 for $write.
struct DisplayInfo {
    // First extra word of the packed format-string bytes.
    ExtraIndex fmt;
    // Format-string length in bytes.
    uint32_t fmt_len;
    // 1 => append a trailing newline ($display); 0 => none ($write).
    uint32_t newline;
    // Value operands consumed by the format specifiers.
    ExtraSlice args;
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
static_assert(ExtraPayload<Mux>);
static_assert(ExtraPayload<PartSelect>);
static_assert(ExtraPayload<Regions>);
static_assert(ExtraPayload<LoopInfo>);
static_assert(ExtraPayload<DisplayInfo>);
static_assert(ExtraPayload<TimeFormatInfo>);
static_assert(ExtraPayload<WaitEdge>);
static_assert(ExtraPayload<WaitInfo>);

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

        // -- Logical, -> 1-bit --
        // Logical and (payload: binary).
        Land,
        // Logical or (payload: binary).
        Lor,
        // Logical not (payload: unary).
        Lnot,

        // -- Selection / structural --
        // Ternary cond ? a : b (payload: op_extra -> Mux).
        Mux,
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
        // Loop with a per-iteration condition; `for` / `while` / `forever` all
        // lower to this (payload: extra -> LoopInfo). The `for` init runs in the
        // enclosing region before this inst; `while` / `forever` have empty step.
        // Lowers to header / body / step / exit blocks with a back-edge.
        Loop,
        // Time delay `#n`: suspend the process for `n` ticks (payload: unary, the
        // tick count). Lowers to a timer scheduled on the wheel plus a coroutine
        // suspend; the wheel resumes the process when the delay elapses.
        Delay,
        // Event wait `@(edge signal)` / `@(a or posedge b)`: suspend until ANY
        // listed (signal, edge) fires (payload: wait -> WaitInfo). Lowers to
        // registering the process on each event's wait queue plus a coroutine
        // suspend; a matching blocking assignment to one of the signals wakes it.
        Wait,

        // -- System tasks --
        // $display / $write: format string + variadic args (payload: display).
        Display,
        // $timeformat: set the %t format (units / precision / suffix / min width)
        // for all later %t specifiers (payload: timeformat -> TimeFormatInfo).
        // LRM §20.4.3. Lowers to an rt_timeformat_set call.
        TimeFormat,
        // $finish / $stop: end the simulation and exit (payload: none). LRM §20.2
        // has $stop suspend and $finish exit; with no resume mechanism a suspend
        // is observationally an exit, so both are treated identically.
        Finish,

        // -- System functions --
        // $time / $stime: read the simulation clock, -> 64-bit unsigned (payload:
        // scale). Lowers to a `rt_wheel_now` call; the value is always known (no
        // x/z). The wheel counts in the design's smallest time unit; the backend
        // rounds the tick count down by `scale` (the calling module's unit / the
        // smallest unit) to the module time unit (LRM §20.3.1). `scale` is 1 when
        // unitless or the module owns the smallest unit. $stime truncates the
        // result to 32 bits via a Resize.
        SimTime,
        // $random: V2005 signed-32 PRNG, -> 32-bit signed two-state (payload:
        // none). Lowers to an `rt_random` call; the value is always known (no
        // x/z). The seeded form `$random(seed)` is rejected by the frontend, so
        // this tag never carries a seed operand.
        Random,
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
        Display,
        TimeFormat,
        Wait,
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
        // Primary operand plus a handle to an extra struct (mux, part_select, cond).
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

} // namespace clotho::ir
