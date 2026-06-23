#pragma once

// Lowering: SystemVerilog source -> clotho process IR. Parses and elaborates
// with slang, then walks the elaborated AST of the top module into an
// ir::Module (one Process per continuous assign / procedural block).
//
// This is a proof-of-concept, not a full front-end: it covers a single top
// module of synthesizable Verilog-2005 (ports, nets/vars, continuous assigns,
// always blocks with if/else, the usual operators). Unsupported constructs are
// reported in `diagnostics` rather than crashing.

#include <memory>
#include <string>
#include <string_view>

#include "ir/Module.h"

namespace clotho {

struct LowerResult {
    // The lowered module, or null if lowering failed.
    std::unique_ptr<ir::Module> module;
    // Rendered slang diagnostics plus any lowering messages (may be non-empty
    // even on success, e.g. warnings).
    std::string diagnostics;
    // Whether a usable module was produced.
    bool ok = false;
};

// Lower a SystemVerilog source file at `path`.
LowerResult lower_file(std::string_view path);

// Lower a SystemVerilog source fragment held in memory.
LowerResult lower_text(std::string_view source);

} // namespace clotho
