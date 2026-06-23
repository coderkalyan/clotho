#pragma once

// Process IR construction. `Builder` owns the growable build-time storage
// (instruction columns + extra side-table) and produces an immutable `Ir` via
// finish(). The builder is spent after finish().
//
// Structured control flow: instructions accumulate into the current region.
// open_region()/close_region() bracket a nested body (e.g. a cond branch);
// close_region() returns the region's slice, to be handed to cond(). The
// outermost (root) region is sealed by finish().
//
// One emit method per instruction shape. Variants sharing a payload
// (arithmetic, bitwise, ...) go through the generic unary()/binary().

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

#include "InternPool.h"
#include "Ir.h"

namespace clotho::ir {

class Builder {
public:
    explicit Builder(InternPool& pool)
      : pool_(pool) {}

    // -- Leaves --
    Index constant(Ref value, SourceLoc loc);
    Index arg(uint32_t position, Ref type, SourceLoc loc);
    Index signal(Signal sig, SourceLoc loc);

    // -- Generic ops sharing a payload shape --
    // tag must be a unary-payload op (load, bnot, lnot, red_*, ret).
    Index unary(Inst::Tag tag, Index op, SourceLoc loc);
    // tag must be a binary-payload op (arithmetic, bitwise, shift, compare,
    // logical and/or, store, store_nb, bit_select).
    Index binary(Inst::Tag tag, Index l, Index r, SourceLoc loc);

    // -- Irregular shapes --
    Index resize(Index op, Ref type, SourceLoc loc);
    Index repl(Index value, uint32_t count, SourceLoc loc);
    Index mux(Index cond, Index a, Index b, SourceLoc loc);
    Index concat(std::span<const Index> parts, SourceLoc loc);
    Index part_select(Index base, uint32_t msb, uint32_t lsb, SourceLoc loc);
    Index cond(Index condition, ExtraSlice then_region, ExtraSlice else_region, SourceLoc loc);

    // -- Regions --
    // Begin a nested region; subsequent emits accumulate into it until closed.
    void open_region();
    // End the current region and return its instruction slice.
    ExtraSlice close_region();

    // Freeze the built state into an Ir, sealing the root region. The builder is
    // spent afterwards.
    Ir finish();

private:
    Index emit(const Inst& inst, SourceLoc loc) {
        const auto index = static_cast<uint32_t>(tags_.size());
        tags_.push_back(inst.tag);
        payloads_.push_back(inst.payload);
        locs_.push_back(loc);
        scratch_.push_back(static_cast<Index>(index));

        return static_cast<Index>(index);
    }

    // Append a struct to the extra buffer; returns its offset handle.
    template<ExtraPayload T>
    ExtraIndex add_extra(const T& value) {
        constexpr size_t n     = sizeof(T) / sizeof(uint32_t);
        const auto       words = std::bit_cast<std::array<uint32_t, n>>(value);
        const auto       start = static_cast<uint32_t>(extra_.size());
        extra_.insert(extra_.end(), words.begin(), words.end());

        return static_cast<ExtraIndex>(start);
    }

    // Append a run of instruction indices to the extra buffer; returns the slice.
    ExtraSlice add_slice(std::span<const Index> indices) {
        const auto start = static_cast<uint32_t>(extra_.size());
        for (Index i : indices) {
            extra_.push_back(static_cast<uint32_t>(i));
        }
        const auto end = static_cast<uint32_t>(extra_.size());

        return { static_cast<ExtraIndex>(start), static_cast<ExtraIndex>(end) };
    }

    // Intern pool backing constant / type references.
    InternPool& pool_;
    // Growable SoA instruction columns under construction.
    std::vector<Inst::Tag>     tags_;
    std::vector<Inst::Payload> payloads_;
    std::vector<SourceLoc>     locs_;
    // Flat u32 side-table under construction.
    std::vector<uint32_t> extra_;
    // Instruction indices of all open regions, innermost segment last.
    std::vector<Index> scratch_;
    // Start offset into scratch_ of each open nested region.
    std::vector<size_t> region_starts_;
};

} // namespace clotho::ir
