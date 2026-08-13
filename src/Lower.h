#pragma once

// Lowering: SystemVerilog source -> clotho process IR. Parses and elaborates
// with slang, then walks the elaborated AST of each top module into an
// ir::Module (one Process per continuous assign / procedural block). Multiple
// top-level modules flatten into the one Module, each prefixed by its name.
//
// Unsupported constructs are reported through `diagnostics` rather than
// crashing; the rule is to never silently accept a construct Clotho does
// not implement.

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Diagnostic.h"
#include "SourceContext.h"
#include "ir/Module.h"
#include "ir/SimConfig.h"
#include "ir/Timing.h"

namespace clotho {

// Which semantics a run judges the design under.
//
// A different axis from the command. `lint` names the activity -- analyse and
// report, write nothing -- and this names what the analysis is measured against,
// so `clotho lint --policy=synth` is the combination that asks "would a
// synthesiser object to this?" without a synthesiser in the loop. Calling the
// severity set a "lint policy" would conflate the two and leave the interesting
// question (under which semantics?) unanswered.
//
// Only what genuinely differs by semantics belongs here. Clotho's own hard
// limits -- the promotions it makes because its signal model or symbol table
// cannot represent the alternative -- are NOT policy and must hold under every
// value; see apply_severity_policy for the split. A policy able to relax those
// would generate miscompiles, not options.
enum class DiagnosticPolicy : uint8_t {
    // IEEE 1800 simulation semantics. The default, and the only value `run` can
    // meaningfully use: anything it rejects is something Clotho cannot simulate.
    Sim,
};

struct LowerResult {
    // The lowered module, or null if lowering failed.
    std::unique_ptr<ir::Module> module;
    // Everything the frontend found, as structured items. Non-empty on success
    // too (warnings). Ordered as produced: slang's elaboration and analysis
    // diagnostics first, then any Clotho lowering diagnostic.
    std::vector<Diagnostic> diagnostics;
    // Keeps the slang SourceManager alive so a `SourceLoc` in `module` or in
    // `diagnostics` can still be resolved to a file position. Shared rather than
    // owned so a diagnostic list may outlive the result it came from. Never null.
    std::shared_ptr<const SourceContext> sources;
    // Whether a usable module was produced.
    bool ok = false;

    // Convenience for CLI output and test failure messages: the diagnostics
    // rendered against `sources`. Equivalent to `render(diagnostics,
    // sources.get())`.
    [[nodiscard]] std::string diagnostic_text() const {
        return render(diagnostics, sources.get());
    }
};

// The $system access tier is threaded through every entry point so the
// compile-time gate can refuse (or constant-check) a shell-exec task. Defaults
// to Off — $system is a hard error unless the caller opts in.

// Lower one SystemVerilog source file at `path`. If `timings` is non-null, the
// front-end and analysis phase durations are accumulated into it.
LowerResult lower_file(std::string_view path,
                       Timings*         timings      = nullptr,
                       ir::SystemAccess allow_system = ir::SystemAccess::Off);

// Lower several source files compiled together into one design. Paths are
// parsed in order and added to a single Compilation before elaboration. No
// preprocessor options; for command-line defines/include-dirs/library flags use
// `lower_sources`.
LowerResult lower_files(std::span<const std::string_view> paths,
                        Timings*                          timings      = nullptr,
                        ir::SystemAccess                  allow_system = ir::SystemAccess::Off);

// Lower a design described by slang command-line arguments (source files plus
// `-D`/`+define+`, `-I`/`+incdir+`, `-f` command files, `-y`/`-v` libraries,
// `--top`, `--timescale`, `--std`, ...). Builds a `driver::Driver`, parses the
// args with it, and lowers the result; every file is merged into one compilation
// unit (LRM 3.12, single-unit: `` `define `` macros and `$unit` declarations span
// files). `args` must exclude runtime plusargs, which slang would reject.
LowerResult lower_sources(std::span<const std::string_view> args,
                          Timings*                          timings      = nullptr,
                          ir::SystemAccess                  allow_system = ir::SystemAccess::Off);

// Lower a design from a Driver the caller built and already parsed with
// `parseCommandLine`. This is the entry the CLI uses: `clotho` registers its own
// options on the Driver's `CommandLine`, so one parser and one `--help` cover
// both tools' options and neither has to forward to the other. Takes ownership —
// the returned `sources` keeps the Driver (hence its SourceManager) alive so a
// `SourceLoc` stays resolvable.
LowerResult lower_driver(std::unique_ptr<slang::driver::Driver> driver,
                         Timings*                               timings      = nullptr,
                         ir::SystemAccess                       allow_system = ir::SystemAccess::Off);

// Lower a SystemVerilog source fragment held in memory. If `timings` is non-null,
// the front-end and analysis phase durations are accumulated into it.
LowerResult lower_text(std::string_view source,
                       Timings*         timings      = nullptr,
                       ir::SystemAccess allow_system = ir::SystemAccess::Off);

// Lower an editor's in-memory buffer, named `path` so its diagnostics can be
// attributed back to the file the client has open.
//
// Three things distinguish this from `lower_text`, and all three exist for the
// language server:
//
//   * The buffer is assigned to a SourceManager this call OWNS, not to slang's
//     process-wide static one. `lower_text` borrows the static manager, so every
//     keystroke would leak a buffer for the process lifetime and two concurrent
//     calls would race -- it is the one lowering path that is not reentrant.
//   * The buffer carries `path`, so a diagnostic resolves to the client's file
//     rather than to the literal name "source".
//   * Unknown modules are ignored during elaboration. An open buffer is
//     normally one file of a design whose other files are not present, and
//     without this a file that instantiates anything reports `unknown module`
//     per instance. NOT `CompilationFlags::LintMode`, which sounds like the
//     flag for this and is not: it marks every scope uninstantiated and so
//     deletes the whole unused-signal family. See lower_compilation's
//     `partial` argument for what a partial design does suppress, and why the
//     line falls where it does.
//
// Consequence: findings that need the instantiating context (port width
// mismatches across a boundary, parameter-dependent widths) do not appear, and
// parameters take their declared defaults -- so an answer from this entry point
// describes the module in isolation, not the module as the design instantiates
// it.
LowerResult lower_buffer(std::string_view path,
                         std::string_view text,
                         Timings*         timings      = nullptr,
                         ir::SystemAccess allow_system = ir::SystemAccess::Off);

// Decode a source extent for whoever needs one: the IR dump's headers, and the
// snippet a runtime diagnostic bakes into its report. Everything comes from the
// SourceManager lowering handed back, so the returned resolver borrows `result`
// and stays valid for exactly as long as it does.
ir::LocResolver loc_resolver(const LowerResult& result);

} // namespace clotho
