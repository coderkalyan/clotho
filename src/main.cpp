#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include <slang/driver/Driver.h>
#include <slang/util/VersionInfo.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/Printer.h"

// Thin driver over libclotho / libclotho_ir: take SystemVerilog sources, run
// them through the lowering pipeline, then write the compiled artifact
// (`build`), JIT-compile and simulate it (`run`), or report only what the
// frontend found (`lint`). Every compiler decision lives in the libraries; this
// file is argument handling and reporting.
//
//   clotho build [options] <source>...             emit an artifact, no execution
//   clotho run   [options] <source>... [+arg...]   compile, then simulate
//   clotho lint  [options] <source>...             diagnostics only
//
// One parser, one namespace, one --help: Clotho's options are registered on the
// slang Driver's own CommandLine (register_options below, before
// addStandardArgs so they head the help text), and the Driver parses the whole
// line. So `-D`, `-I`, `-y`, `-v`, `-f`, `--top`, `-G`, `--timescale`, `--std`,
// `-W`, `--error-limit` and `-j` are not "forwarded" to anything — they are
// simply options this tool has, listed by --help alongside --emit and -o, and a
// typo in either set gets slang's "did you mean" instead of silent passthrough.
//
// Two things argv still needs before slang sees it: the leading command word,
// and runtime plusargs (LRM 21.6, `run` only, read by $test$plusargs /
// $value$plusargs), which slang would reject as unknown `+` options. A `+arg`
// that is not one of the `+` forms slang registers is taken as a plusarg; `--`
// ends plusarg interpretation and passes everything after it to slang as given.
//
// Sources always share one compilation unit (LRM 3.12): `` `define `` macros
// and `$unit` declarations span files, parsed in the order given.
//
// Exit status: 0 success; 1 compile / emission / JIT failure (or, for `lint`, a
// diagnostic that reached Error); 2 usage error; under `run`, otherwise the
// simulated design's own status.

namespace {

using clotho::ir::Sanitizer;
using clotho::ir::SimulationConfig;
using clotho::ir::Stat;
using clotho::ir::SystemAccess;

enum class Command : uint8_t { Build, Run, Lint };

// What `build` writes.
enum class Emit : uint8_t {
    Ir,      // Clotho process IR
    Llvm,    // LLVM IR after the coroutine-lowering pipeline (the default)
    LlvmRaw, // LLVM IR straight from the backend, llvm.coro.* intact
};

// Clotho's own options, as slang CommandLine targets: unset optional means "not
// given", which is what the command-scope checks below key on.
struct Options {
    std::optional<bool>        help;
    std::optional<bool>        version;
    std::optional<std::string> emit;
    std::optional<std::string> out;
    std::optional<bool>        time;
    std::optional<bool>        o3;
    std::vector<std::string>   stats;
    std::vector<std::string>   sanitizers;
    std::optional<std::string> allow_system;
    std::vector<std::string>   dpi_libs;
    std::optional<bool>        perf_jitdump;
};

// Wording rule for everything a user reads: say what the option does for the
// design being simulated, not how Clotho implements it. An option that exists
// only for working on Clotho itself says so ("for working on clotho"), which is
// the one place implementation vocabulary belongs.
//
// Registered before Driver::addStandardArgs so these head the --help listing.
// A `[build]` / `[run]` tag in a description marks a command-specific option;
// check_scope enforces it after parsing.
void register_options(slang::CommandLine& cl, Options& o) {
    cl.add("-h,--help", o.help, "Show this help and exit");
    cl.add("--version", o.version, "Show version information and exit");
    cl.add("--emit", o.emit,
           "[build] What to write: llvm (LLVM IR for the design, the default), ir (Clotho's own "
           "intermediate form), llvm-raw (LLVM IR before the coroutine passes; for working on clotho)",
           "<target>");
    cl.add("-o,--output", o.out, "[build] Write the output to this file instead of stdout", "<file>",
           slang::CommandLineFlags::FilePath);
    cl.add("--time", o.time, "Print how long each stage took: reading the source, compiling, simulating");
    cl.add("-O", o.o3, "Compile the design more thoroughly: slower to start up, faster to simulate");
    cl.add("--stat", o.stats,
           "Print a compiler statistic while building, for working on clotho: dedup, dedup-sets, "
           "comb-fuse, finish or parallel (repeatable, comma-separated)",
           "<name>", slang::CommandLineFlags::CommaList);
    cl.add("--sanitize", o.sanitizers,
           "Check the design for a class of bug while it runs, reporting when one happens: bounds "
           "(an array index outside the array), xprov (where an x value decided something, and what "
           "made it x). Costs simulation time (repeatable, comma-separated)",
           "<check>", slang::CommandLineFlags::CommaList);
    cl.add("--allow-system", o.allow_system,
           "Let the design run shell commands through $system, refused by default: 'const' allows "
           "only commands written literally in the source, 'full' allows any command the design "
           "builds while it runs",
           "<tier>");
    cl.add("--dpi-lib", o.dpi_libs,
           "[run] Library holding the C functions this design imports through DPI-C. Give a .o or .a "
           "if C also calls back into the design; a .so is enough for imports alone",
           "<path>", slang::CommandLineFlags::FilePath);
    cl.add("--perf-jitdump", o.perf_jitdump,
           "[run] Let `perf` see names for the design's compiled code (writes a jitdump under "
           "$JITDUMPDIR); for profiling clotho");
}

constexpr std::string_view kOverview =
  "clotho — a SystemVerilog simulator\n"
  "\n"
  "commands:\n"
  "  build   compile the design and write the generated code, without running it\n"
  "  run     compile the design and simulate it\n"
  "  lint    check the design and report problems, without compiling or running it\n"
  "\n"
  "Clotho's own options come first below, then the source-reading options it shares\n"
  "with slang, the SystemVerilog front end it is built on. A [build] or [run] tag\n"
  "marks an option only that command accepts.\n"
  "\n"
  "An argument starting with '+' that is not an option goes to the design itself,\n"
  "for $test$plusargs and $value$plusargs to read while it runs (LRM 21.6). Use\n"
  "'--' to stop that and pass everything after it through as written.";

int usage_error(std::string_view message) {
    std::println(stderr, "clotho: {}", message);
    std::println(stderr, "try 'clotho --help'");
    return 2;
}

// A `+` token slang owns: it registers `+define`, `+incdir` and `+libext`
// (Driver::addStandardArgs). Anything else starting with `+` is a runtime
// plusarg — `--` is the escape for a `+` command slang gained some other way
// (`--cmd-rename`).
bool is_slang_plusarg(std::string_view arg) {
    return arg.starts_with("+define+") || arg.starts_with("+incdir+") || arg.starts_with("+libext+");
}

std::string_view command_name(Command command) {
    switch (command) {
        case Command::Build:
            return "build";
        case Command::Run:
            return "run";
        case Command::Lint:
            return "lint";
    }
    return "";
}

// A command-specific option used under another command is an error, not a
// silently ignored no-op (never silently accept).
bool check_scope(const Options& o, Command command, int& status) {
    struct Check {
        bool             given;
        std::string_view name;
        Command          owner;
    };
    const Check checks[] = {
        { o.emit.has_value(), "--emit", Command::Build },
        { o.out.has_value(), "-o", Command::Build },
        { !o.dpi_libs.empty(), "--dpi-lib", Command::Run },
        { o.perf_jitdump.has_value(), "--perf-jitdump", Command::Run },
    };
    for (const Check& c : checks) {
        if (c.given && c.owner != command) {
            status = usage_error(
              std::format("{} applies to '{}', not '{}'", c.name, command_name(c.owner), command_name(command)));
            return false;
        }
    }
    return true;
}

// Phase timings go to stderr so stdout stays exactly the design's own output.
// Labels name what the stage does for the user's design, not the component that
// does it: "read source" is slang parse + elaborate, "analyze" is the lowering to
// Clotho's own form, "compile" is LLVM code generation. tests/perf/perf.py
// scrapes these, so a rename lands there too.
void report_timings(const clotho::Timings& t, Command command) {
    std::println(stderr, "timings (ms):");
    std::println(stderr, "  {:<12}{:>12.3f}", "read source", t.frontend_ms);
    std::println(stderr, "  {:<12}{:>12.3f}", "analyze", t.analysis_ms);
    if (t.llvm_ms > 0.0) {
        std::println(stderr, "  {:<12}{:>12.3f}", "compile", t.llvm_ms);
    }
    if (command == Command::Run) {
        std::println(stderr, "  {:<12}{:>12.3f}", "simulate", t.sim_ms);
    }
    const double total = t.frontend_ms + t.analysis_ms + t.llvm_ms + t.sim_ms;
    std::println(stderr, "  {:<12}{:>12.3f}", "total", total);
}

// Decodes a source extent for whoever needs one: the IR dump's headers, and the
// snippet a sanitizer bakes into its report. Everything comes from the
// SourceManager lowering hands back, so it stays valid for as long as `result`
// does.
clotho::ir::LocResolver loc_resolver(const clotho::LowerResult& result) {
    return [&result](clotho::ir::SourceSpan span) {
        const clotho::SourcePosition pos = result.sources->resolve(span.start);
        // The underline needs an end on the SAME line to have a width; a span
        // that runs onto the next line degrades to a bare caret.
        const clotho::SourcePosition end = result.sources->resolve(span.end);
        return clotho::ir::SourceText{
            .where      = result.sources->render(span.start),
            .line       = result.sources->line_text(span.start),
            .line_no    = pos.line,
            .column     = pos.column,
            .end_column = end.valid && end.line == pos.line ? end.column : pos.column,
        };
    };
}

// `build`: write the requested artifact. `-o -` (the default) means stdout.
bool write_artifact(const clotho::LowerResult& result, Emit emit, std::string_view out,
                    const SimulationConfig& config, clotho::Timings* timings) {
    const bool to_stdout = out == "-";
    // Locations are resolvable because lowering hands back the SourceManager. An
    // IR dump you cannot map to source is much less useful when debugging a
    // lowering problem, and a sanitizer report with no file:line is barely a
    // report at all.
    const auto resolve = loc_resolver(result);
    if (emit == Emit::Ir) {
        if (to_stdout) {
            clotho::ir::print_module(*result.module, std::cout, resolve);
            return true;
        }
        std::ofstream os{ std::string(out) };
        if (!os) {
            std::println(stderr, "clotho: cannot open '{}'", out);
            return false;
        }
        clotho::ir::print_module(*result.module, os, resolve);
        return true;
    }
    if (to_stdout) {
        return clotho::ir::emit_llvm(*result.module, llvm::outs(), config, timings, resolve);
    }
    std::error_code      ec;
    llvm::raw_fd_ostream os(out, ec);
    if (ec) {
        std::println(stderr, "clotho: cannot open '{}': {}", out, ec.message());
        return false;
    }
    return clotho::ir::emit_llvm(*result.module, os, config, timings, resolve);
}

} // namespace

int main(int argc, char** argv) {
    // Build the option set first: Clotho's, then slang's, on one CommandLine.
    auto                driver = std::make_unique<slang::driver::Driver>();
    Options             options;
    slang::CommandLine& cmd_line = driver->cmdLine;
    register_options(cmd_line, options);
    driver->addStandardArgs();
    cmd_line.setProgramName("clotho <command>");

    const auto print_help = [&] { std::print("{}", cmd_line.getHelpText(kOverview)); };
    const auto print_version = [] {
        std::println("clotho {}", CLOTHO_GIT_HASH);
        std::println("LLVM {}", CLOTHO_LLVM_VERSION);
        std::println("slang {}", slang::VersionInfo::getVersionString());
    };

    if (argc < 2) {
        return usage_error("no command; expected 'build', 'run' or 'lint'");
    }
    Command command = Command::Run;
    {
        const std::string_view first = argv[1];
        if (first == "build") {
            command = Command::Build;
        } else if (first == "run") {
            command = Command::Run;
        } else if (first == "lint") {
            command = Command::Lint;
        } else if (first == "-h" || first == "--help") {
            // Help and version stand alone, with no command.
            print_help();
            return 0;
        } else if (first == "--version") {
            print_version();
            return 0;
        } else {
            return usage_error(std::format("unknown command '{}'; expected 'build', 'run' or 'lint'", first));
        }
    }

    // Peel runtime plusargs; everything else goes to the Driver verbatim. argv
    // entries are already null-terminated, so they can be handed over directly.
    std::vector<const char*>      driver_argv{ "clotho" };
    std::vector<std::string_view> plusargs;
    bool                          verbatim = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (!verbatim && arg == "--") {
            verbatim = true;
            continue;
        }
        if (!verbatim && arg.starts_with("+") && !is_slang_plusarg(arg)) {
            if (command != Command::Run) {
                return usage_error(
                  std::format("plusarg '{}' applies to 'run', not '{}'", arg, command_name(command)));
            }
            plusargs.emplace_back(arg.substr(1));
            continue;
        }
        driver_argv.push_back(argv[i]);
    }

    // The Driver reports unknown options, bad values and "did you mean" hints.
    if (!driver->parseCommandLine(static_cast<int>(driver_argv.size()), driver_argv.data())) {
        return 2;
    }
    if (options.help == true) {
        print_help();
        return 0;
    }
    if (options.version == true) {
        print_version();
        return 0;
    }
    if (driver_argv.size() == 1) {
        return usage_error(std::format("'{}' needs at least one source file", command_name(command)));
    }

    int status = 0;
    if (!check_scope(options, command, status)) {
        return status;
    }

    Emit emit = Emit::Llvm;
    if (options.emit.has_value()) {
        const std::string& target = *options.emit;
        if (target == "llvm") {
            emit = Emit::Llvm;
        } else if (target == "llvm-raw") {
            emit = Emit::LlvmRaw;
        } else if (target == "ir") {
            emit = Emit::Ir;
        } else {
            return usage_error(std::format("unknown --emit target '{}'; expected llvm, llvm-raw or ir", target));
        }
    }

    SimulationConfig config;
    config.o3 = options.o3 == true;
    // llvm-raw is the pre-pipeline dump; every other path needs runnable shape.
    config.lower_coro   = emit != Emit::LlvmRaw;
    config.perf_jitdump = options.perf_jitdump == true;
    config.dpi_libs     = options.dpi_libs;
    // slang's -j/--threads sizes its parser; the same count sizes the LLVM
    // compile pool, so the tool has one thread knob rather than two.
    config.compile_jobs = driver->options.numThreads.value_or(0);
    for (const std::string& name : options.stats) {
        if (name == "dedup") {
            config.stats |= static_cast<uint32_t>(Stat::Dedup);
        } else if (name == "dedup-sets") {
            config.stats |= static_cast<uint32_t>(Stat::DedupSets);
        } else if (name == "comb-fuse") {
            config.stats |= static_cast<uint32_t>(Stat::CombFuse);
        } else if (name == "finish") {
            config.stats |= static_cast<uint32_t>(Stat::Finish);
        } else if (name == "parallel") {
            config.stats |= static_cast<uint32_t>(Stat::Parallel);
        } else {
            return usage_error(std::format("unknown --stat '{}'; expected dedup, dedup-sets, comb-fuse, "
                                           "finish or parallel",
                                           name));
        }
    }
    // TODO(xsan): `xprov` is parsed and carried in SimulationConfig, but no
    // backend pass reads that bit yet, so `--sanitize=xprov` is accepted and
    // instruments nothing. Unguarded on purpose while xsan is built out
    // incrementally (the project's partial-feature exception); the guard
    // disappears when the instrumentation lands, not the TODO. `bounds` is
    // live (SimConfig.h names what it does and does not cover yet).
    for (const std::string& name : options.sanitizers) {
        if (name == "bounds") {
            config.sanitizers |= static_cast<uint32_t>(Sanitizer::Bounds);
        } else if (name == "xprov") {
            config.sanitizers |= static_cast<uint32_t>(Sanitizer::XProv);
        } else {
            return usage_error(std::format("unknown --sanitize check '{}'; expected bounds or xprov", name));
        }
    }
    if (options.allow_system.has_value()) {
        // $system runs a shell command: arbitrary code execution, so the tier is an
        // explicit operator grant, never a default (LRM 20.17.1).
        const std::string& tier = *options.allow_system;
        if (tier == "const") {
            config.allow_system = SystemAccess::Const;
        } else if (tier == "full") {
            config.allow_system = SystemAccess::Full;
        } else {
            return usage_error(std::format(
              "unknown --allow-system tier '{}'; write --allow-system=const or --allow-system=full", tier));
        }
        std::println(stderr, "clotho: warning: this design may run shell commands ($system enabled with "
                             "--allow-system={})",
                     tier);
    }

    const bool             want_time = options.time == true;
    clotho::Timings        timings;
    clotho::Timings* const tp = want_time ? &timings : nullptr;

    const clotho::LowerResult result = clotho::lower_driver(std::move(driver), tp, config.allow_system);
    if (!result.diagnostics.empty()) {
        std::print(stderr, "{}", result.diagnostic_text());
    }

    // `lint`: the diagnostics ARE the artifact, so stop here.
    //
    // Note what this does NOT claim to be. `build` already elaborates, lowers,
    // analyses and reports exactly the same diagnostics before it writes anything
    // (docs/design/cli.md), so lint is not a different pipeline -- it is that
    // pipeline with no artifact and its own exit-status rule. Its real reason to
    // exist separately is to be the home of the third severity policy
    // (synthesis-and-lsp-scoping.md §3.4), which does not exist yet: today lint
    // and build report identically and only their outputs differ.
    //
    // Deliberately ahead of the `ok` gate below. A design with a member that
    // failed to lower is exactly what this is for, since an editor buffer is
    // broken most of the time and every other finding in the file is still
    // wanted. Exit status keys on whether anything reached Error, so warnings
    // alone leave it clean.
    if (command == Command::Lint) {
        if (want_time) {
            report_timings(timings, command);
        }
        return clotho::has_error(result.diagnostics) ? 1 : 0;
    }

    // An IR dump is analysis, not execution, so it tolerates a poisoned module:
    // when a lowering guard fires, seeing what DID lower is the whole point. The
    // diagnostics are already on stderr and the exit status still reports the
    // failure, so a partial dump cannot be mistaken for a whole design.
    const std::string_view out = options.out.has_value() ? std::string_view(*options.out) : std::string_view("-");
    if (command == Command::Build && emit == Emit::Ir && result.module != nullptr) {
        const bool ok = write_artifact(result, emit, out, config, tp);
        if (want_time) {
            report_timings(timings, command);
        }
        return ok && result.ok ? 0 : 1;
    }
    if (!result.ok) {
        return 1;
    }

    if (command == Command::Run) {
        const auto resolve = loc_resolver(result);
        const int  rc      = clotho::ir::run_jit(*result.module, tp, config, plusargs, resolve);
        // run_jit returns a negative sentinel on setup failure; the design's own
        // status passes through unchanged.
        status = rc < 0 ? 1 : rc;
    } else {
        status = write_artifact(result, emit, out, config, tp) ? 0 : 1;
    }
    if (want_time) {
        report_timings(timings, command);
    }
    return status;
}
