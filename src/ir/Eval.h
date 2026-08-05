#pragma once

// Partial evaluation of an expression cone with one signal pinned to a constant.
//
// The question this answers: "if `rst` were 1, what would this predicate be?"
// That is the whole of the reset-classification rule in
// docs/design/process-legalization.md §4A.5 -- pin the trigger asserted, fold, and
// hand the residue to the ordinary completeness test. It is also what a case
// selector needs to decide coverage, and what `Builder` folding (S2) would need.
//
// Three properties define it:
//
//  * **Bit-precise partial knowledge.** A value carries a `mask` of known bits, so
//    `1'b1 | unknown` folds to 1 even though the operand was not known. That is not
//    a nicety: `posedge a or posedge b` with `if (a | b)` needs exactly it.
//
//  * **Unknown is always sound.** Every unhandled tag, every over-wide value and
//    every x/z bit degrades to unknown, and every consumer reads unknown as "could
//    not prove". So the `default:` arm below is correct rather than a gap -- unlike
//    the exhaustive tables in Ir.h, where a missing case would fabricate an answer.
//
//  * **It stops at signal loads.** A predicate routed through a function call, a
//    task, or any stateful read is unknown; there is no inlining. That bounds what
//    reset shapes can be classified, and it is a deliberate ceiling: the honest
//    "cannot prove" is better than a partial interprocedural evaluator that is
//    right most of the time.

#include <cstdint>
#include <span>
#include <vector>

#include "Ir.h"
#include "Module.h"

namespace clotho::ir {

// A partially-known packed value.
//
// `mask` marks bits whose value is known; `bits` holds them. Bits outside `mask`
// are meaningless. Capped at 64 bits -- a wider value is reported fully unknown,
// which is sound and keeps this allocation-free.
struct EvalValue {
    uint64_t bits  = 0;
    uint64_t mask  = 0;
    uint32_t width = 0;

    [[nodiscard]] static constexpr uint64_t width_mask(uint32_t w) {
        return w >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << w) - 1);
    }
    [[nodiscard]] static constexpr EvalValue unknown(uint32_t w) {
        return { 0, 0, w };
    }
    [[nodiscard]] static constexpr EvalValue known(uint64_t v, uint32_t w) {
        return { v & width_mask(w), width_mask(w), w };
    }
    [[nodiscard]] constexpr bool fully_known() const {
        return width > 0 && width <= 64 && mask == width_mask(width);
    }
};

// The LRM's three-way truth of a value (IEEE 1800-2023 §11.4.7): defined as true,
// defined as false, or an ambiguous truth value. Deciding which is exactly the
// reduction-OR question of §1.1, so `truth` implements that rule and not the
// weaker "no bit may be x" reading two published sources give.
enum class Tri : uint8_t { False, True, Unknown };

// One signal held at a value for the duration of an evaluation.
struct Pin {
    Signal   signal;
    uint64_t bits;
};

class Eval {
public:
    // Pin `pinned` to `pinned_bits` for the whole evaluation. Pass
    // `Signal(~0u)` to pin nothing, which is still useful: it folds the constant
    // sub-expressions of a cone.
    Eval(const Module& module, const Ir& ir, Signal pinned, uint64_t pinned_bits);

    // Several pins at once. Needed to recover the SYNCHRONOUS body of a process
    // with more than one asynchronous reset: that body is what runs with every
    // async trigger deasserted, and one pin cannot express two.
    Eval(const Module& module, const Ir& ir, std::span<const Pin> pins);

    // Value of an instruction's result. Memoized; repeated queries are free.
    [[nodiscard]] EvalValue value(Index i);

    // Truth of an instruction's result, per §11.4.7 / Table 11-17: any definitely-1
    // bit makes it True regardless of unknown bits; all bits known and zero makes
    // it False; anything else is an ambiguous truth value.
    [[nodiscard]] Tri truth(Index i);

private:
    EvalValue compute(Index i);
    EvalValue constant_value(Ref ref) const;

    const Module&          module_;
    const Ir&              ir_;
    std::vector<Pin>       pins_;
    std::vector<EvalValue> memo_;
    std::vector<uint8_t>   done_;
};

// Convenience: truth of `i` with `pinned` held at `bits`. For one-shot queries;
// build an `Eval` directly when asking about several instructions in one cone.
[[nodiscard]] Tri truth_with_pin(const Module&, const Ir&, Index i, Signal pinned, uint64_t bits);

} // namespace clotho::ir
