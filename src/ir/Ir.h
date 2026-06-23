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

// Extra payload for `cond`: the then / else regions (the condition is the op).
struct Regions {
    // Instructions executed when the condition is true.
    ExtraSlice then_;
    // Instructions executed when the condition is false.
    ExtraSlice else_;
};

static_assert(ExtraPayload<ExtraSlice>);
static_assert(ExtraPayload<Mux>);
static_assert(ExtraPayload<PartSelect>);
static_assert(ExtraPayload<Regions>);

struct Inst {
    enum class Tag : uint8_t {
        // -- Leaves --
        // Interned constant value (payload: ref).
        Constant,
        // Function / task parameter by position (payload: arg).
        Arg,
        // Reference to a module signal (payload: signal).
        Signal,

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
        // Return a value from a function / task (payload: unary).
        Ret,
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
        Repl,
        OpExtra,
        Extra,
    };

    union Payload {
        // Inactive / nullary placeholder.
        uint32_t none;
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
