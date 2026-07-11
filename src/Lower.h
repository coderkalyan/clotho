#pragma once

// Lowering: SystemVerilog source -> clotho process IR. Parses and elaborates
// with slang, then walks the elaborated AST of each top module into an
// ir::Module (one Process per continuous assign / procedural block). Multiple
// top-level modules flatten into the one Module, each prefixed by its name.
//
// This is a proof-of-concept, not a full front-end: it covers the top
// module(s) of synthesizable Verilog-2005 (ports, nets/vars, continuous assigns,
// always blocks with if/else, the usual operators). Unsupported constructs are
// reported in `diagnostics` rather than crashing.

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ir/Module.h"
#include "ir/Timing.h"

namespace clotho {

// Preprocessor inputs supplied on the command line, threaded into slang's
// PreprocessorOptions before parsing (LRM 22.5 `define`, 22.4 `include`).
struct FrontendOptions {
    // Macro predefinitions, each "NAME" or "NAME=VALUE" (as `+define+`/`-D`).
    std::vector<std::string> defines;
    // Directories searched for `` `include `` files (as `+incdir+`/`-I`).
    std::vector<std::string> include_dirs;
};

struct LowerResult {
    // The lowered module, or null if lowering failed.
    std::unique_ptr<ir::Module> module;
    // Rendered slang diagnostics plus any lowering messages (may be non-empty
    // even on success, e.g. warnings).
    std::string diagnostics;
    // Whether a usable module was produced.
    bool ok = false;
};

// Lower one SystemVerilog source file at `path`. If `timings` is non-null, the
// front-end and analysis phase durations are accumulated into it.
LowerResult lower_file(std::string_view path, Timings* timings = nullptr);

// Lower several source files compiled together into one design (LRM 3.12: all
// files share one compilation unit / global name space). Paths are parsed in
// order and added to a single Compilation before elaboration.
LowerResult lower_files(std::span<const std::string_view> paths,
                        const FrontendOptions& fe = {}, Timings* timings = nullptr);

// Lower a SystemVerilog source fragment held in memory. If `timings` is non-null,
// the front-end and analysis phase durations are accumulated into it.
LowerResult lower_text(std::string_view source, Timings* timings = nullptr);

} // namespace clotho
