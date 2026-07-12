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

#include "ir/Module.h"
#include "ir/Timing.h"

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

// Lower one SystemVerilog source file at `path`. If `timings` is non-null, the
// front-end and analysis phase durations are accumulated into it.
LowerResult lower_file(std::string_view path, Timings* timings = nullptr);

// Lower several source files compiled together into one design. Paths are
// parsed in order and added to a single Compilation before elaboration. No
// preprocessor options; for command-line defines/include-dirs/library flags use
// `lower_sources`.
LowerResult lower_files(std::span<const std::string_view> paths, Timings* timings = nullptr);

// Lower a design described by slang command-line arguments (source files plus
// `-D`/`+define+`, `-I`/`+incdir+`, `-f` command files, `-y`/`-v` libraries,
// `--top`, `--timescale`, `--std`, ...). Args are handed to slang's
// `driver::Driver`; every file is merged into one compilation unit (LRM 3.12,
// single-unit: `` `define `` macros and `$unit` declarations span files). This
// is the entry the CLI uses; `args` must exclude Clotho's own options and
// runtime plusargs.
LowerResult lower_sources(std::span<const std::string_view> args, Timings* timings = nullptr);

// Lower a SystemVerilog source fragment held in memory. If `timings` is non-null,
// the front-end and analysis phase durations are accumulated into it.
LowerResult lower_text(std::string_view source, Timings* timings = nullptr);

} // namespace clotho
