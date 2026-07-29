#pragma once

// Debug printer for the process IR. Renders the instruction stream in a flat,
// textual form inspired by femtoc's FIR renderer: one `%index = op(...)` line
// per instruction, with region-bearing ops (cond) opening an indented brace
// block for their nested bodies. Intended for tests and human inspection only;
// the format is not stable and not parsed back.

#include <functional>
#include <ostream>
#include <string>

#include "Ir.h"
#include "Module.h"

namespace clotho::ir {

// Optional hook turning a SourceLoc into display text.
//
// The IR core cannot decode a SourceLoc itself: the (buffer, offset) mapping lives
// in the slang-aware frontend, and keeping this library free of slang is the point
// of the clotho_ir_core / clotho split. So a caller that wants located output
// passes a resolver (in practice `SourceContext::render`). Default-constructed
// means "print no locations", which is what tests want — golden IR text must not
// depend on file paths.
using LocResolver = std::function<std::string(SourceLoc)>;

// Render the whole IR (root region, descending into nested regions) to `os`.
void print(const Ir& ir, std::ostream& os);

// Render the whole IR to a string.
std::string toString(const Ir& ir);

// Render a module: its name, signal table (so the body's `s<n>` references are
// decodable), then each process with a header and its lowered body. When
// `resolve` is set, signal, process and function headers carry their source
// location.
void print_module(const Module& m, std::ostream& os, const LocResolver& resolve = {});

// Render a module to a string.
std::string toString(const Module& m);

} // namespace clotho::ir
