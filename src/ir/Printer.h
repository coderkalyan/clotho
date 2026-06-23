#pragma once

// Debug printer for the process IR. Renders the instruction stream in a flat,
// textual form inspired by femtoc's FIR renderer: one `%index = op(...)` line
// per instruction, with region-bearing ops (cond) opening an indented brace
// block for their nested bodies. Intended for tests and human inspection only;
// the format is not stable and not parsed back.

#include <ostream>
#include <string>

#include "Ir.h"
#include "Module.h"

namespace clotho::ir {

// Render the whole IR (root region, descending into nested regions) to `os`.
void print(const Ir& ir, std::ostream& os);

// Render the whole IR to a string.
std::string toString(const Ir& ir);

// Render a module: its name, signal table (so the body's `s<n>` references are
// decodable), then each process with a header and its lowered body.
void print_module(const Module& m, std::ostream& os);

// Render a module to a string.
std::string toString(const Module& m);

} // namespace clotho::ir
