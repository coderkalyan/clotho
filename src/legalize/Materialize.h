#pragma once

// Materialize: render a legalized design back as SystemVerilog with every
// implicit part written out.
//
// This is the "fill in the mux tree" view. A guarded-assignment list is already
// a complete description of one signal's next-state logic; materializing it
// spells that description as source, so an always-block can be read as the
// hardware it implies rather than as the statements that imply it.
//
// `full_case` / `parallel_case` are NOT honoured here: the pragma data is M7 and
// does not exist yet, and a style option that silently did nothing would be worse
// than its absence.
//
// The base slot is the interesting part. A guarded list has an implicit value
// for "no guard matched", and what belongs there is exactly the classification:
//
//   Comb       the base is UNREACHABLE, so it renders as 'x. Writing it is what
//              keeps the signal combinational -- a reader (and a re-lowering)
//              can see completeness without re-deriving it.
//   Latch/Flop the base is the PREVIOUS VALUE, and SystemVerilog already spells
//              that by omission. Rendering `q = q` instead would create a
//              self-reference that is not in the design, so the fill-in for a
//              holding signal is deliberately nothing.
//
// That asymmetry is why this is not a naive "add the missing else" pass, and it
// is why the round-trip test in tests/legalize_test.cpp is the acceptance: the
// rendered source must re-lower to the SAME classification.
//
// A separate header and translation unit from Legalize.h because this is a
// consumer of the result, the way Printer is a consumer of Ir -- nothing in the
// analysis depends on it.

#include <string>

#include "Legalize.h"
#include "ir/Module.h"

namespace clotho::legalize {

struct MaterializeOptions {
    // Module name to emit. Empty renders `m`.
    std::string_view name;
};

// Render `result` as SystemVerilog. `result` MUST have been produced with
// Options::keep_guards, since the guards are the mux tree.
//
// A process the analysis rejected, or one using an expression this renderer does
// not cover, is emitted as a comment naming the reason rather than as source
// that would silently mean something else.
[[nodiscard]] std::string materialize(const ir::Module& module, const Result& result, MaterializeOptions options = {});

} // namespace clotho::legalize
