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

// Short mnemonic for an instruction tag (snake_case). Exposed so a test can pin an
// emitted tag SEQUENCE without pinning the enum's ordinals, which move whenever a
// tag is inserted rather than appended.
const char* tag_name(Inst::Tag tag);

// Render a module: its name, signal table (so the body's `s<n>` references are
// decodable), then each process with a header and its lowered body. When
// `resolve` is set, signal, process and function headers carry their source
// location.
void print_module(const Module& m, std::ostream& os, const LocResolver& resolve = {});

// Render a module to a string.
std::string toString(const Module& m);

} // namespace clotho::ir
