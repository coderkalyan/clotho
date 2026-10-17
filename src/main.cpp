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
#include "dap/Dap.h"
#include "lsp/Lsp.h"
#ifdef CLOTHO_SYNTH
#include "legalize/Legalize.h"
#include "legalize/Materialize.h"
#endif

// Thin driver over libclotho / libclotho_ir: take SystemVerilog sources, run
// them through the lowering pipeline, then write the compiled artifact
// (`build`), JIT-compile and simulate it (`run`), or report only what the
// frontend found (`lint`). Every compiler decision lives in the libraries; this
// file is argument handling and reporting.
//
//   clotho build [options] <source>...             emit an artifact, no execution
//   clotho run   [options] <source>... [+arg...]   compile, then simulate
//   clotho lint  [options] <source>...             diagnostics only
//   clotho lsp                                     serve an editor over stdio
//   clotho dap                                     debug a design for an editor
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

enum class Command : uint8_t { Build, Run, Lint, Synth, Lsp, Dap };

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
    std::optional<std::string> coverage;
    std::optional<bool>        gc_stress;
    std::optional<bool>        gc_stats;
    std::optional<uint64_t>    gc_nursery_bytes;
    std::optional<uint64_t>    gc_heap_bytes_max;
    std::optional<uint32_t>    gc_full_every;
    std::optional<std::string> allow_system;
    std::optional<bool>        loose_timescale;
    std::optional<bool>        loose_stale_ref;
    std::vector<std::string>   dpi_libs;
    std::optional<bool>        perf_jitdump;
    std::optional<std::string> sidecar;
    std::optional<std::string> target;
    std::optional<std::string> hierarchy;
    std::optional<bool>        no_legalize;
    std::optional<bool>        full_case;
    std::optional<bool>        parallel_case;
};

// The `--sanitize` checks this build actually has. `xprov` is compiled out of a
// Release build (CMakeLists CLOTHO_XSAN): it is parsed and carried in
// SimulationConfig but no backend pass reads the bit yet, and a check that
// silently reports nothing is worse than one the binary says it does not know.
constexpr std::string_view kSanitizeList =
#ifdef CLOTHO_XSAN
  "bounds or xprov";
#else
  "bounds";
#endif

// Wording rule for everything a user reads: say what the option does for the
// design being simulated, not how Clotho implements it. An option that exists
// only for working on Clotho itself says so ("for working on clotho"), which is
// the one place implementation vocabulary belongs.
//
// Registered before Driver::addStandardArgs so these head the --help listing.
// A `[build]` / `[run]` tag in a description marks a command-specific option, and
// `[build|run]` one that two commands share (the --gc-* group, whose settings the
// artifact is built with and then run with); check_scope enforces it after parsing.
void register_options(slang::CommandLine& cl, Options& o) {
    cl.add("-h,--help", o.help, "Show this help and exit");
    cl.add("--version", o.version, "Show version information and exit");
    cl.add("--emit",
           o.emit,
           "[build] What to write: llvm (LLVM IR for the design, the default), ir (Clotho's own "
           "intermediate form), llvm-raw (LLVM IR before the coroutine passes; for working on clotho)",
           "<target>");
    cl.add("-o,--output",
           o.out,
           "[build] Write the output to this file instead of stdout",
           "<file>",
           slang::CommandLineFlags::FilePath);
    cl.add("--time", o.time, "Print how long each stage took: reading the source, compiling, simulating");
    cl.add("-O", o.o3, "Compile the design more thoroughly: slower to start up, faster to simulate");
    cl.add("--stat",
           o.stats,
           "Print a compiler statistic while building, for working on clotho: dedup, dedup-sets, "
           "comb-fuse, finish or parallel (repeatable, comma-separated)",
           "<name>",
           slang::CommandLineFlags::CommaList);
    cl.add("--sanitize",
           o.sanitizers,
           "Check the design for a class of bug while it runs, reporting when one happens: bounds "
           "(an array index outside the array)"
#ifdef CLOTHO_XSAN
           ", xprov (where an x value decided something, and what made it x)"
#endif
           ". Costs simulation time (repeatable, comma-separated)",
           "<check>",
           slang::CommandLineFlags::CommaList);
    cl.add("--coverage",
           o.coverage,
           "[build|run] Write the design's functional-coverage report here: which of the bins its "
           "covergroups declare the run reached, and which it never did (LRM 19). No report by "
           "default. The destination is compiled into the generated code, so a build and the run "
           "of what it built must be given the same file",
           "<file>",
           slang::CommandLineFlags::FilePath);
    cl.add("--gc-stress",
           o.gc_stress,
           "[build|run] Reclaim unused class objects at every opportunity instead of when the "
           "design has created enough of them. Slow by design: it turns an object Clotho failed "
           "to keep into an immediate wrong answer rather than a rare one, so it is for working "
           "on clotho itself. Changes the generated code, so a build and the run of what it "
           "built must be given the same setting");
    cl.add("--gc-stats",
           o.gc_stats,
           "[build|run] Print what reclaiming class objects did when the design exits: how many "
           "times it ran, how much it reclaimed, and how long the design was paused for it");
    cl.add("--gc-nursery",
           o.gc_nursery_bytes,
           "[build|run] How many bytes of class objects the design may create between one reclaim "
           "and the next (default 8388608). A plain number of bytes",
           "<bytes>");
    cl.add("--gc-heap-max",
           o.gc_heap_bytes_max,
           "[build|run] Size in bytes the design's class objects should stay under: reaching it "
           "asks for a reclaim at the next opportunity, and passing it anyway is reported rather "
           "than refused, since a design is free to create objects without ever pausing. 0, the "
           "default, sets no limit",
           "<bytes>");
    cl.add("--gc-full-every",
           o.gc_full_every,
           "[build|run] Reclaim the whole heap on every Nth reclaim instead of only the objects "
           "created since the last one. Only a whole-heap pass frees objects that have already "
           "survived a reclaim, so this trades cheap frequent passes against how long those "
           "linger. 0, the default, lets the runtime decide from how fast the heap is growing. "
           "<n>");
    cl.add("--allow-system",
           o.allow_system,
           "Let the design run shell commands through $system, refused by default: 'const' allows "
           "only commands written literally in the source, 'full' allows any command the design "
           "builds while it runs",
           "<tier>");
    cl.add("--loose-timescale",
           o.loose_timescale,
           "Accept a design where some parts declare a time scale and others do not, refused by "
           "default: the parts that do not take --timescale's value, or 1ns/1ns when it is not given");
    cl.add("--loose-stale-ref",
           o.loose_stale_ref,
           "[run] Report and keep simulating when a ref argument no longer refers to the variable it "
           "was bound to, which stops the run by default: growing or reordering an array or queue "
           "while a ref refers to an element of it leaves that ref on something else entirely, so "
           "nothing after the first one means anything. For naming every such ref in one run");
    cl.add("--dpi-lib",
           o.dpi_libs,
           "[run] Library holding the C functions this design imports through DPI-C. Give a .o or .a "
           "if C also calls back into the design; a .so is enough for imports alone",
           "<path>",
           slang::CommandLineFlags::FilePath);
#ifdef CLOTHO_SYNTH
    cl.add("--sidecar",
           o.sidecar,
           "[synth] Also write the JSON sidecar here: what the SystemVerilog cannot carry -- the "
           "original-to-emitted name map every constraint file depends on, and each signal's inferred "
           "storage, clock and reset",
           "<path>",
           slang::CommandLineFlags::FilePath);
    cl.add("--target",
           o.target,
           "[synth] Which language to write: '2005' for IEEE 1364-2005, 'sv' for IEEE 1800 (default)",
           "<lang>");
    cl.add("--hierarchy",
           o.hierarchy,
           "[synth] How much of the instance tree to keep: 'instances' writes one module per instance "
           "with names as written (default), 'flat' writes one module with paths mangled into names",
           "<mode>");
    cl.add("--no-legalize",
           o.no_legalize,
           "[synth] Translate the design as written instead of writing out the hardware it implies: no "
           "mux trees rebuilt, no implicit branch filled in");
    cl.add("--full-case",
           o.full_case,
           "[synth] Believe `full_case` and `unique`: a case the design says is complete stops inferring "
           "a latch. Reported wherever the claim is what removed the latch, since simulation will still "
           "hold the old value");
    cl.add("--parallel-case",
           o.parallel_case,
           "[synth] Believe `parallel_case` and `unique0`: the arms are treated as mutually exclusive");
#endif
    cl.add("--perf-jitdump",
           o.perf_jitdump,
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
  "  lsp     serve the Language Server Protocol over stdin/stdout, for an editor\n"
  "  dap     serve the Debug Adapter Protocol over stdin/stdout, to debug a design\n"
#ifdef CLOTHO_SYNTH
  "  synth   write the design back out as synthesizable SystemVerilog\n"
#endif
  "\n"
  "Clotho's own options come first below, then the source-reading options it shares\n"
#ifdef CLOTHO_SYNTH
  "with slang, the SystemVerilog front end it is built on. A [build], [run] or\n"
  "[synth] tag marks an option only that command accepts; [build|run] one that\n"
  "both of those accept.\n"
#else
  "with slang, the SystemVerilog front end it is built on. A [build] or [run] tag\n"
  "marks an option only that command accepts; [build|run] one that both accept.\n"
#endif
  "\n"
  "An argument starting with '+' that is not an option goes to the design itself,\n"
  "for $test$plusargs and $value$plusargs to read while it runs (LRM 21.6). Use\n"
  "'--' to stop that and pass everything after it through as written.";

// The commands this build actually has. `synth` is compiled out of a Release
// build, and a usage message that offers a command the binary will then reject is
// worse than one that never mentions it.
constexpr std::string_view kCommandList =
#ifdef CLOTHO_SYNTH
  "'build', 'run', 'lint', 'synth', 'lsp' or 'dap'";
#else
  "'build', 'run', 'lint', 'lsp' or 'dap'";
#endif

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
        case Command::Synth:
            return "synth";
        case Command::Lsp:
            return "lsp";
        case Command::Dap:
            return "dap";
    }
    return "";
}

// A command-specific option used under another command is an error, not a
// silently ignored no-op (PRINCIPLES.md: never silently accept).
bool check_scope(const Options& o, Command command, int& status) {
    // `-o` names the artifact, and two commands produce one, so ownership is a set
    // rather than a single command. The --gc-* group is a set for a different
    // reason: its settings are compiled in (the backend bakes an rt_gc_config into
    // the emitted module and calls rt_gc_init from its main), so `build` is told
    // them exactly as `run` is, and `lint`, which emits no code, refuses them.
    // `--coverage` is in that set for the same reason: the coverage model the
    // backend emits names its own destination, so the report file is a mode
    // compiled in, not something `run` can add to an artifact built without it.
    struct Check {
        bool             given;
        std::string_view name;
        Command          owner;
        Command          also = Command::Build;
    };
    const Check checks[] = {
        { o.emit.has_value(), "--emit", Command::Build },
        { o.out.has_value(), "-o", Command::Build, Command::Synth },
        { o.gc_stress.has_value(), "--gc-stress", Command::Build, Command::Run },
        { o.gc_stats.has_value(), "--gc-stats", Command::Build, Command::Run },
        { o.gc_nursery_bytes.has_value(), "--gc-nursery", Command::Build, Command::Run },
        { o.gc_heap_bytes_max.has_value(), "--gc-heap-max", Command::Build, Command::Run },
        { o.gc_full_every.has_value(), "--gc-full-every", Command::Build, Command::Run },
        { o.coverage.has_value(), "--coverage", Command::Build, Command::Run },
        { !o.dpi_libs.empty(), "--dpi-lib", Command::Run },
        { o.perf_jitdump.has_value(), "--perf-jitdump", Command::Run },
        { o.loose_stale_ref.has_value(), "--loose-stale-ref", Command::Run },
        { o.sidecar.has_value(), "--sidecar", Command::Synth, Command::Synth },
        { o.target.has_value(), "--target", Command::Synth, Command::Synth },
        { o.hierarchy.has_value(), "--hierarchy", Command::Synth, Command::Synth },
        { o.no_legalize.has_value(), "--no-legalize", Command::Synth, Command::Synth },
        { o.full_case.has_value(), "--full-case", Command::Synth, Command::Synth },
        { o.parallel_case.has_value(), "--parallel-case", Command::Synth, Command::Synth },
    };
    for (const Check& c : checks) {
        if (c.given && c.owner != command && c.also != command) {
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


// `build`: write the requested artifact. `-o -` (the default) means stdout.
// Write `text` to `out`, or to stdout for "-".
bool write_text(std::string_view out, const std::string& text) {
    if (out == "-") {
        std::cout << text;
        return true;
    }
    std::ofstream os{ std::string(out) };
    if (!os) {
        std::println(stderr, "clotho: cannot open '{}'", out);
        return false;
    }
    os << text;
    return true;
}

bool write_artifact(const clotho::LowerResult& result,
                    Emit                       emit,
                    std::string_view           out,
                    const SimulationConfig&    config,
                    clotho::Timings*           timings) {
    const bool to_stdout = out == "-";
    // Locations are resolvable because lowering hands back the SourceManager. An
    // IR dump you cannot map to source is much less useful when debugging a
    // lowering problem, and a sanitizer report with no file:line is barely a
    // report at all.
    const auto resolve = clotho::loc_resolver(result);
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

    const auto print_help    = [&] { std::print("{}", cmd_line.getHelpText(kOverview)); };
    const auto print_version = [] {
        std::println("clotho {}", CLOTHO_GIT_HASH);
        std::println("LLVM {}", CLOTHO_LLVM_VERSION);
        std::println("slang {}", slang::VersionInfo::getVersionString());
        std::println("Copyright (C) 2026 Kalyan Sriram");
        std::println("License GPL-3.0-only <https://www.gnu.org/licenses/gpl-3.0.html>.");
        std::println("The runtime linked into simulations is MPL-2.0.");
        std::println("This is free software: you are free to change and redistribute it.");
        std::println("There is NO WARRANTY, to the extent permitted by law.");
    };

    if (argc < 2) {
        return usage_error(std::format("no command; expected {}", kCommandList));
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
        } else if (first == "lsp") {
            command = Command::Lsp;
        } else if (first == "dap") {
            command = Command::Dap;
#ifdef CLOTHO_SYNTH
        } else if (first == "synth") {
            command = Command::Synth;
#endif
        } else if (first == "-h" || first == "--help") {
            // Help and version stand alone, with no command.
            print_help();
            return 0;
        } else if (first == "--version") {
            print_version();
            return 0;
        } else {
            return usage_error(std::format("unknown command '{}'; expected {}", first, kCommandList));
        }
    }

    // Neither server takes a design on the command line. For `lsp` an editor
    // supplies the file set and this build has no workspace configuration to
    // express it with; for `dap` the design arrives in the launch request's
    // `args`, which are this command line's own `run` arguments passed through
    // to the design process verbatim (docs/design/dap.md 4.4). Accepting
    // -D / -y / -f here and ignoring them would be exactly the silent
    // acceptance PRINCIPLES.md forbids, so they are refused until they mean
    // something.
    if (command == Command::Lsp || command == Command::Dap) {
        for (int i = 2; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "-h" || arg == "--help") {
                print_help();
                return 0;
            }
            if (arg == "--version") {
                print_version();
                return 0;
            }
            return usage_error(
              std::format("'{}' takes no arguments, got '{}'", command_name(command), arg));
        }
        if (command == Command::Dap) {
            clotho::dap::serve(CLOTHO_GIT_HASH, argv[0]);
        }
        clotho::lsp::serve(CLOTHO_GIT_HASH);
    }

    // Peel runtime plusargs and hand everything else to the Driver verbatim. argv
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
                return usage_error(std::format("plusarg '{}' applies to 'run', not '{}'", arg, command_name(command)));
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
    // backend pass reads that bit yet, so it instruments nothing. That is
    // PRINCIPLES.md's partial-feature exception, and it holds only where the operator
    // cannot mistake a silent check for a clean design -- so a Debug build offers
    // it (CLOTHO_XSAN) and the shipped ones reject the name outright. Both the
    // gate and this TODO disappear when the instrumentation lands. `bounds` is
    // live (SimConfig.h names what it does and does not cover yet).
    for (const std::string& name : options.sanitizers) {
        if (name == "bounds") {
            config.sanitizers |= static_cast<uint32_t>(Sanitizer::Bounds);
#ifdef CLOTHO_XSAN
        } else if (name == "xprov") {
            config.sanitizers |= static_cast<uint32_t>(Sanitizer::XProv);
#endif
        } else {
            return usage_error(
              std::format("unknown --sanitize check '{}'; expected {}", name, kSanitizeList));
        }
    }
    // Where the functional-coverage report goes (LRM 19). Compiled in rather
    // than looked up at startup, for the reason check_scope records: the emitted
    // coverage model carries its own destination.
    config.coverage_report = options.coverage.value_or(config.coverage_report);
    // Class objects are always collected (LRM 8.4 makes reclamation unobservable,
    // and docs/design/garbage-collection.md §12 records the measurements that made
    // an uncollected mode not worth keeping). What is left on the CLI only tunes
    // the collector, and every one of these is compiled into the artifact, so a
    // `build` and the `run` of what it built must be given the same values.
    config.gc_stress = options.gc_stress == true;
    config.gc_stats = options.gc_stats == true;
    // A zero nursery would make a collection due at every safepoint, which is
    // --gc-stress spelled obscurely; and 0 means "no limit" for --gc-heap-max, so
    // two adjacent byte options would read the same value opposite ways.
    if (options.gc_nursery_bytes.has_value() && *options.gc_nursery_bytes == 0) {
        return usage_error("--gc-nursery=0 would collect at every opportunity; write --gc-stress");
    }
    config.gc_nursery_bytes  = options.gc_nursery_bytes.value_or(config.gc_nursery_bytes);
    config.gc_heap_bytes_max = options.gc_heap_bytes_max.value_or(config.gc_heap_bytes_max);
    config.gc_full_every     = options.gc_full_every.value_or(config.gc_full_every);
    if (options.allow_system.has_value()) {
        // $system runs a shell command: arbitrary code execution, so the tier is an
        // explicit operator grant, never a default (LRM 20.17.1).
        const std::string& tier = *options.allow_system;
        if (tier == "const") {
            config.allow_system = SystemAccess::Const;
        } else if (tier == "full") {
            config.allow_system = SystemAccess::Full;
        } else {
            return usage_error(
              std::format("unknown --allow-system tier '{}'; write --allow-system=const or --allow-system=full", tier));
        }
        std::println(stderr,
                     "clotho: warning: this design may run shell commands ($system enabled with "
                     "--allow-system={})",
                     tier);
    }
    // A design element with no time scale, in a design where another element has
    // one, is an error: its `#5` would otherwise be five of whatever the design's
    // finest unit happened to be, so the delay a file means depends on which other
    // files are compiled with it (LRM 3.14.2.2). Loosening that has to supply the
    // missing unit rather than just mute the complaint, so this flag gives the
    // un-annotated elements a real default -- --timescale's value if the operator
    // named one, else 1ns/1ns, which is what slang assumes.
    // With every element carrying a time scale, the mixed-design check has nothing
    // left to report.
    if (options.loose_timescale == true) {
        if (!driver->options.timeScale.has_value()) {
            driver->options.timeScale = "1ns/1ns";
        }
        std::println(stderr,
                     "clotho: warning: parts of this design with no time scale of their own are "
                     "being read as {} (--loose-timescale)",
                     *driver->options.timeScale);
    }
    // The other two loosenings above announce themselves at startup, because
    // nothing else would ever say the design was read permissively. This one
    // needs no banner: it changes only what happens when a ref does go stale,
    // and that case reports itself, with a location, every time it happens
    // (rt_ref_stale). A run that never has one is unaffected, so a banner would
    // only be noise. The mode is armed on the runtime at simulation start
    // (run_jit in src/ir/Jit.cpp).
    config.loose_stale_ref = options.loose_stale_ref == true;

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

#ifdef CLOTHO_SYNTH
    if (command == Command::Synth) {
        clotho::legalize::SynthConfig cfg;
        if (options.target.has_value()) {
            if (*options.target == "2005") {
                cfg.target = clotho::legalize::Target::V2005;
            } else if (*options.target != "sv") {
                return usage_error(std::format("unknown --target '{}'; expected 2005 or sv", *options.target));
            }
        }
        if (options.hierarchy.has_value()) {
            if (*options.hierarchy == "flat") {
                cfg.hierarchy = clotho::legalize::Hierarchy::Flat;
            } else if (*options.hierarchy != "instances") {
                return usage_error(
                  std::format("unknown --hierarchy '{}'; expected flat or instances", *options.hierarchy));
            }
        }

        // Without legalization this is a straight translation of what the design
        // says. With it, each written signal is rebuilt from its guarded list and
        // the branches the source left implicit are written out.
        const bool                      legalize = options.no_legalize != true;
        clotho::legalize::Result        res;
        const clotho::legalize::Result* resp = nullptr;
        if (legalize) {
            res  = clotho::legalize::run(*result.module,
                                         nullptr,
                                         { .honour_full_case     = options.full_case == true,
                                           .honour_parallel_case = options.parallel_case == true,
                                           .keep_guards          = true });
            resp = &res;
        }
        if (!write_text(out, clotho::legalize::materialize(*result.module, resp, cfg))) {
            return 1;
        }
        if (options.sidecar.has_value() &&
            !write_text(*options.sidecar, clotho::legalize::sidecar(*result.module, resp, cfg))) {
            return 1;
        }
        if (want_time) {
            report_timings(timings, command);
        }
        return 0;
    }
#endif

    if (command == Command::Run) {
        const auto resolve = clotho::loc_resolver(result);
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
