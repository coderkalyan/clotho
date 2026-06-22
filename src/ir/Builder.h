#pragma once

// IR construction. `Builder` owns the growable build-time storage (instruction
// columns + extra side-table) and produces an immutable `Ir` via finish().
// Instructions are appended in program order; the builder is spent after
// finish().
//
// Scaffolding: one emit method per sample instruction. The full set is filled
// in in a later step.

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

    // Emit instructions, in program order. Each returns its result handle and
    // requires a source location, opaque to the IR.
    Index constant(Ref value, SourceLoc loc);
    Index add(Index l, Index r, SourceLoc loc);
    Index call(Index callee, std::span<const Index> args, SourceLoc loc);
    Index ret(Index value, SourceLoc loc);

    // Freeze the built state into an Ir. The builder is spent afterwards.
    Ir finish();

private:
    Index emit(const Inst& inst, SourceLoc loc) {
        const auto index = static_cast<uint32_t>(tags_.size());
        tags_.push_back(inst.tag);
        payloads_.push_back(inst.payload);
        locs_.push_back(loc);

        return static_cast<Index>(index);
    }

    // Append a struct to the extra buffer; returns its offset handle.
    template<ExtraPayload T>
    ExtraIndex addExtra(const T& value) {
        constexpr size_t n     = sizeof(T) / sizeof(uint32_t);
        const auto       words = std::bit_cast<std::array<uint32_t, n>>(value);
        const auto       start = static_cast<uint32_t>(extra_.size());
        extra_.insert(extra_.end(), words.begin(), words.end());

        return static_cast<ExtraIndex>(start);
    }

    // Append a run of instruction indices; returns the covering slice.
    ExtraSlice addSlice(std::span<const Index> indices) {
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
};

} // namespace clotho::ir
