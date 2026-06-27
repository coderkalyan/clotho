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
#include <string_view>
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
    // for / while / forever. `cond_region` recomputes `pred` each iteration;
    // `step_region` is the per-iteration update (empty for while / forever).
    Index loop(ExtraSlice cond_region, Index pred, ExtraSlice body_region, ExtraSlice step_region, SourceLoc loc);

    // -- Timing --
    // Event wait `@(a or posedge b ...)`: suspend until ANY listed (signal, edge)
    // fires. Each WaitEdge carries an ir::Edge value as u32, keeping the builder
    // free of the Module-level enum. The list must be non-empty.
    Index wait(std::span<const WaitEdge> events, SourceLoc loc);

    // -- System tasks --
    // $display / $write: a literal format string, a newline flag (true for
    // $display), and the already-lowered value args its specifiers consume.
    Index display(std::string_view fmt, bool newline, std::span<const Index> args, SourceLoc loc);
    // $finish / $stop: end the simulation and exit. No operands.
    Index finish(SourceLoc loc);
    // $timeformat: set the %t format. `units`, `precision`, and `min_width` are
    // already-lowered value operands; `suffix` is a literal string appended after
    // the formatted number.
    Index timeformat(Index units, Index precision, std::string_view suffix, Index min_width, SourceLoc loc);

    // -- System functions --
    // $time / $stime: read the simulation clock. Yields a 64-bit unsigned value
    // rounded down by `scale` to the calling module's time unit (1 == unitless).
    Index sim_time(uint64_t scale, SourceLoc loc);
    // $random: a 32-bit signed PRNG draw. No operands.
    Index random(SourceLoc loc);

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

    // Append raw bytes to the extra buffer, packed little-endian 4 chars per
    // word; returns the offset of the first word. Decode with Ir::extra_bytes.
    ExtraIndex add_bytes(std::string_view s) {
        const auto start = static_cast<uint32_t>(extra_.size());
        for (size_t i = 0; i < s.size(); i += 4) {
            uint32_t word = 0;
            for (size_t k = 0; k < 4 && i + k < s.size(); ++k) {
                word |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + k])) << (8 * k);
            }
            extra_.push_back(word);
        }

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
