#pragma once

// Core compiler IR — data-oriented scaffolding.
//
// One Ir instance is a single function / process body: a flat, append-only
// list of instructions, frozen and immutable once built. There are no basic
// blocks at this layer.
//
// This is a skeleton: the instruction set and payload shapes below are a small
// representative sample, enough to exercise every storage mechanism. The real
// instruction set is filled in in a later step.
//
// Design:
//   * Index handles, not pointers. Every cross-reference is a 4-byte
//     strongly-typed enum index, so the stores stay relocatable and compact.
//   * Structure-of-arrays instruction store. The instruction is split into
//     parallel tag / payload / source-location columns (held directly by Ir)
//     so a pass that only reads tags never pulls payloads into cache.
//   * Compact untagged payload union (<=8 bytes). The discriminant lives in
//     the separate tag column and selects the active member via payloadKind().
//   * Flat u32 "extra" side-table for variable-length / overflow operands
//     (e.g. call args). Structs whose every field is a 4-byte handle are
//     overlaid onto the buffer with std::bit_cast.
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

// A struct may be overlaid on the extra buffer iff it is trivially copyable
// and a whole number of u32 words wide. Because every field is a 4-byte
// handle/enum, such structs carry no padding and round-trip via bit_cast.
template<class T>
concept ExtraPayload = std::is_trivially_copyable_v<T> && (sizeof(T) % sizeof(uint32_t) == 0);

// Half-open range [start, end) of u32 words in the extra buffer.
struct ExtraSlice {
    // First word covered by the slice.
    ExtraIndex start;
    // One past the last word covered by the slice.
    ExtraIndex end;
};

static_assert(ExtraPayload<ExtraSlice>);

struct Inst {
    // Representative sample of instruction kinds, one per payload shape.
    enum class Tag : uint8_t {
        // Interned constant value (payload: ref).
        constant,
        // Sample fixed-arity binary op (payload: binary).
        add,
        // Variadic call; op = callee, extra = arg slice (payload: unary_extra).
        call,
        // Terminator returning a value (payload: unary).
        ret,
    };

    // Which union member a tag activates. Drives payloadKind() / operands().
    enum class PayloadKind : uint8_t {
        // No active member.
        none,
        // ref.
        ref,
        // unary.
        unary,
        // binary.
        binary,
        // unary_extra.
        unary_extra,
    };

    union Payload {
        // Inactive / nullary placeholder.
        uint32_t none;
        // Interned constant (constant).
        Ref ref;
        // Single operand (ret).
        Index unary;
        // Two operands (add).
        struct {
            // Left operand.
            Index l;
            // Right operand.
            Index r;
        } binary;
        // Callee plus a slice of arguments in the extra buffer (call).
        struct {
            // Primary operand (e.g. the callee).
            Index op;
            // Handle to the operand slice / extra struct.
            ExtraIndex extra;
        } unary_extra;
    };

    // Discriminant selecting the active payload member.
    Tag tag;
    // Per-tag operand data.
    Payload payload;
};

static_assert(sizeof(Inst::Payload) <= 8, "payload must stay compact");
static_assert(std::is_trivially_copyable_v<Inst::Payload>);

// Maps an instruction tag to the active payload union member.
Inst::PayloadKind payloadKind(Inst::Tag tag);

// Immutable, finished IR: a flat instruction list held as a structure of
// arrays — parallel tag / payload / location columns, all indexed by Index.
// Built once from the builder's vectors, then read-only: no spare capacity, no
// append. Constructed by Builder::finish().
class Ir {
public:
    // Intern pool backing this IR's constant / type handles.
    const InternPool& pool;

    // Instructions are indexed 0 .. instCount()-1 in program order.
    size_t instCount() const {
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

    // Decode a struct previously written with Builder::addExtra.
    template<ExtraPayload T>
    T extraData(ExtraIndex at) const {
        constexpr size_t        n    = sizeof(T) / sizeof(uint32_t);
        const auto              base = static_cast<uint32_t>(at);
        std::array<uint32_t, n> words{};
        for (size_t i = 0; i < n; ++i) {
            words[i] = extra_[base + i];
        }

        return std::bit_cast<T>(words);
    }

    // A slice reinterpreted as a run of instruction indices.
    std::span<const Index> indexSlice(ExtraSlice s) const {
        const auto a = static_cast<uint32_t>(s.start);
        const auto b = static_cast<uint32_t>(s.end);

        return { reinterpret_cast<const Index*>(extra_.data() + a), b - a };
    }

    // Fill `scratch` with the SSA operands of an instruction and return a view
    // of them. Fixed-arity ops use the scratch buffer; variadic ops (call)
    // return a view straight into the extra buffer.
    std::span<const Index> operands(Index inst, std::array<Index, 2>& scratch) const;

private:
    friend class Builder;

    // Constructed atomically by Builder::finish().
    Ir(const InternPool&          pool,
       std::vector<Inst::Tag>     tags,
       std::vector<Inst::Payload> payloads,
       std::vector<SourceLoc>     locs,
       std::vector<uint32_t>      extra)
      : pool(pool)
      , tags_(std::move(tags))
      , payloads_(std::move(payloads))
      , locs_(std::move(locs))
      , extra_(std::move(extra)) {}

    // Instruction tags, in program order.
    Array<Inst::Tag> tags_;
    // Per-instruction payloads, parallel to tags_.
    Array<Inst::Payload> payloads_;
    // Per-instruction opaque source locations, parallel to tags_.
    Array<SourceLoc> locs_;
    // Flat u32 side-table for variable-length operands.
    Array<uint32_t> extra_;
};

} // namespace clotho::ir
