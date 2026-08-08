#pragma once

// Materialize: render a legalized design back as SystemVerilog with every
// implicit part written out.
//
// This is the "fill in the mux tree" view. A guarded-assignment list is already
// a complete description of one signal's next-state logic; materializing it
// spells that description as source, so an always-block can be read as the
// hardware it implies rather than as the statements that imply it.
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
#include <vector>

#include "Legalize.h"
#include "SynthConfig.h"
#include "ir/Module.h"

namespace clotho::legalize {

// The names the writer actually emitted, indexed by handle.
//
// Produced by the same call the writer uses, which is the only way the sidecar's
// `original -> emitted` map can be true rather than a second guess at it.
struct EmittedNames {
    std::vector<std::string> sig;   // by ir::Signal
    std::vector<std::string> fn;    // by ir::FunctionId
    std::vector<std::string> mod;   // by scope id; empty when Hierarchy::Flat
    std::vector<std::string> scope; // by scope id: the full instance path
};

// Render `module` as SystemVerilog.
//
// `result` is OPTIONAL. Given one (produced with Options::keep_guards, since the
// guards are the mux tree), each written signal is rendered from its guarded list
// and the implicit arms are filled in. Given nullptr, this is a 1:1 structural
// translation: the region tree is printed as statements, an absent `else` stays
// absent, and nothing is inferred. The second path is the simpler of the two --
// there is no tree to rebuild, because the region tree already is one.
//
// A process the analysis rejected, or one using an expression this renderer does
// not cover, is emitted as a comment naming the reason rather than as source
// that would silently mean something else.
// The JSON sidecar: everything the emitted SystemVerilog cannot carry.
//
// Two things downstream needs and the text cannot hold. The NAME MAP, because
// normalization renames and every SDC constraint, physical script and area report
// keys on the original hierarchical path -- an emitter that renames without a map
// is unusable in a real flow (7.3). And the CLASSIFICATION, because "this signal is
// a flop on posedge clk with an async reset" is the analysis's product, not a
// property of the source text.
//
// `result` may be null; the classification section is then omitted rather than
// guessed. Names come from the same computation the writer uses, so the map is true
// by construction rather than by agreement.
[[nodiscard]] std::string sidecar(const ir::Module& module, const Result* result = nullptr, SynthConfig cfg = {});

[[nodiscard]] std::string materialize(const ir::Module& module, const Result* result = nullptr, SynthConfig cfg = {});

} // namespace clotho::legalize
