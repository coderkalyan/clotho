#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/Printer.h"

// Thin driver over libclotho / libclotho_ir: take SystemVerilog sources, run
// them through the lowering pipeline, then either write the compiled artifact
// (`build`) or JIT-compile and simulate it (`run`). Every compiler decision
// lives in the libraries; this file is argument handling and reporting.
//
//   clotho build [options] <source>...             emit an artifact, no execution
//   clotho run   [options] <source>... [+arg...]   compile, then simulate
//
// Argv is split three ways: Clotho's own options (kOptions below), runtime
// plusargs (LRM 21.6, `run` only, read by $test$plusargs / $value$plusargs),
// and everything else — source files plus slang's preprocessor / library /
// elaboration flags (`-D`, `-I`, `-y`, `-v`, `-f`, `--top`, `-G`, `--std`,
// `--timescale`, ...) — forwarded verbatim to slang's Driver (`--help-slang`
// lists them). slang rejects an unregistered `+arg`, so plusargs are peeled off
// here; the `+` forms slang itself owns (`+define+`, `+incdir+`, `+libext+`)
// are forwarded, and `--slang-arg` forces through anything else.
//
// Sources always share one compilation unit (LRM 3.12): `` `define `` macros
// and `$unit` declarations span files, parsed in the order given.
//
// Exit status: 0 success; 1 compile / emission / JIT failure; 2 usage error;
// under `run`, otherwise the simulated design's own status.

namespace {

using clotho::ir::SimulationConfig;
using clotho::ir::Stat;
using clotho::ir::SystemAccess;

enum class Command : uint8_t { Build, Run, Check };

// What `build` writes.
enum class Emit : uint8_t {
    Ir,      // Clotho process IR
    Llvm,    // LLVM IR after the coroutine-lowering pipeline (the default)
    LlvmRaw, // LLVM IR straight from the backend, llvm.coro.* intact
};

enum class Opt : uint8_t {
    Emit,
    Out,
    Time,
    O3,
    Stat,
    AllowSystem,
    DpiLib,
    Jobs,
    PerfJitdump,
    SlangArg,
    Help,
    Version,
    HelpSlang,
};

// Which commands accept an option. Anything::Any is also accepted before a
// command is given (`clotho --help`).
enum Scope : uint8_t {
    kBuild = 1u << 0,
    kRun   = 1u << 1,
    kCheck = 1u << 2,
    kAny   = kBuild | kRun | kCheck,
};

struct OptSpec {
    Opt              id;
    std::string_view name;
    std::string_view alias;   // "" when there is none
    std::string_view operand; // "" for a flag; else the operand's display name
    uint8_t          scope;
    std::string_view help;
};

// The single source of truth for the option surface: parsing resolves a token
// through this table, and --help is printed from it, so a flag cannot exist
// without being documented (or be documented without existing).
constexpr OptSpec kOptions[] = {
    { Opt::Emit, "--emit", "", "<target>", kBuild, "artifact to write: llvm (default), llvm-raw, ir" },
    { Opt::Out, "-o", "--output", "<file>", kBuild, "write the artifact to <file> ('-' is stdout, the default)" },
    { Opt::Time, "--time", "", "", kAny, "report per-phase wall-clock timings to stderr" },
    { Opt::O3, "-O", "", "", kAny, "optimize hard: LLVM default<O3> plus aggressive machine codegen" },
    { Opt::Stat,
      "--stat",
      "",
      "<list>",
      kAny,
      "report backend statistics: dedup, dedup-sets, comb-fuse, finish, parallel" },
    { Opt::AllowSystem,
      "--allow-system",
      "",
      "<tier>",
      kAny,
      "permit the $system shell task: const (constant commands only) or full" },
    { Opt::DpiLib,
      "--dpi-lib",
      "",
      "<path>",
      kRun,
      "DPI-C library: .o/.a is JIT-linked (import and export), .so is dlopen'd (import); repeatable" },
    { Opt::Jobs, "--jobs", "-j", "<n>", kAny, "worker threads for parsing and compilation (0 = host concurrency)" },
    { Opt::PerfJitdump,
      "--perf-jitdump",
      "",
      "",
      kRun,
      "write a perf jitdump ($JITDUMPDIR) so JIT'd code symbolizes in perf" },
    { Opt::SlangArg,
      "--slang-arg",
      "",
      "<text>",
      kAny,
      "forward <text> to slang verbatim (for +args clotho would eat)" },
    { Opt::Help, "--help", "-h", "", kAny, "show this help" },
    { Opt::Version, "--version", "", "", kAny, "show clotho, LLVM and slang versions" },
    { Opt::HelpSlang, "--help-slang", "", "", kAny, "show slang's options, all of which clotho forwards" },
};

const OptSpec* find_option(std::string_view arg) {
    for (const OptSpec& spec : kOptions) {
        // `--name`, `--name=value`, and the alias in both forms.
        for (const std::string_view name : { spec.name, spec.alias }) {
            if (name.empty()) {
                continue;
            }
            if (arg == name || (arg.starts_with(name) && arg.size() > name.size() && arg[name.size()] == '=')) {
                return &spec;
            }
        }
    }
    return nullptr;
}

void print_help() {
    std::println("clotho — SystemVerilog simulator (JIT)");
    std::println("");
    std::println("usage: clotho <command> [options] <source>... [+plusarg...]");
    std::println("");
    std::println("commands:");
    std::println("  build   compile the design and write the requested artifact");
    std::println("  run     compile the design and simulate it");
    std::println("  check   analyse the design and report diagnostics, writing nothing");
    const auto group = [](std::string_view title, uint8_t scope, bool exact) {
        std::println("");
        std::println("{}", title);
        for (const OptSpec& spec : kOptions) {
            if (exact ? spec.scope != scope : spec.scope != kAny) {
                continue;
            }
            std::string flag(spec.name);
            if (!spec.alias.empty()) {
                flag += ", ";
                flag += spec.alias;
            }
            if (!spec.operand.empty()) {
                flag += ' ';
                flag += spec.operand;
            }
            std::println("  {:<24}{}", flag, spec.help);
        }
    };
    group("options (all commands):", kAny, false);
    group("options (build only):", kBuild, true);
    group("options (run only):", kRun, true);
    std::println("");
    std::println("Every other option goes to slang's driver (-D, -I, -y, -v, -f, --top, -G,");
    std::println("--timescale, --std, -W, --error-limit, ...); see --help-slang. Sources always");
    std::println("share one compilation unit (LRM 3.12).");
}

void print_version() {
    std::println("clotho {}", CLOTHO_GIT_HASH);
    std::println("LLVM {}", CLOTHO_LLVM_VERSION);
    std::println("slang {}", clotho::slang_version());
}

int usage_error(std::string_view message) {
    std::println(stderr, "clotho: {}", message);
    std::println(stderr, "try 'clotho --help'");
    return 2;
}

// A `+` token slang owns (it registers `+define`, `+incdir` and `+libext`);
// anything else starting with `+` is a runtime plusarg.
bool is_slang_plusarg(std::string_view arg) {
    return arg.starts_with("+define+") || arg.starts_with("+incdir+") || arg.starts_with("+libext+");
}

struct Args {
    Command                       command = Command::Run;
    Emit                          emit    = Emit::Llvm;
    std::string_view              out     = "-";
    std::vector<std::string_view> slang;    // source files + slang driver options
    std::vector<std::string_view> plusargs; // LRM 21.6, `+` stripped
    bool                          time = false;
    SimulationConfig              config;
};

// Phase timings go to stderr so stdout stays exactly the design's own output.
void report_timings(const clotho::Timings& t, Command command) {
    std::println(stderr, "timings (ms):");
    std::println(stderr, "  {:<16}{:>12.3f}", "frontend (slang)", t.frontend_ms);
    std::println(stderr, "  {:<16}{:>12.3f}", "analysis (ir)", t.analysis_ms);
    if (t.llvm_ms > 0.0) {
        std::println(stderr, "  {:<16}{:>12.3f}", "llvm compile", t.llvm_ms);
    }
    if (command == Command::Run) {
        std::println(stderr, "  {:<16}{:>12.3f}", "simulation", t.sim_ms);
    }
    const double total = t.frontend_ms + t.analysis_ms + t.llvm_ms + t.sim_ms;
    std::println(stderr, "  {:<16}{:>12.3f}", "total", total);
}

// `build`: write the requested artifact. `-o -` (the default) means stdout.
bool write_artifact(const clotho::LowerResult& result, const Args& args, clotho::Timings* timings) {
    const bool to_stdout = args.out == "-";
    if (args.emit == Emit::Ir) {
        // Locations are resolvable because lowering hands back the SourceManager;
        // an IR dump you cannot map to source is much less useful when debugging
        // a lowering problem.
        const auto resolve = [&](clotho::ir::SourceLoc l) { return result.sources->render(l); };
        if (to_stdout) {
            clotho::ir::print_module(*result.module, std::cout, resolve);
            return true;
        }
        std::ofstream os(std::string(args.out));
        if (!os) {
            std::println(stderr, "clotho: cannot open '{}'", args.out);
            return false;
        }
        clotho::ir::print_module(*result.module, os, resolve);
        return true;
    }
    if (to_stdout) {
        return clotho::ir::emit_llvm(*result.module, llvm::outs(), args.config, timings);
    }
    std::error_code      ec;
    llvm::raw_fd_ostream os(args.out, ec);
    if (ec) {
        std::println(stderr, "clotho: cannot open '{}': {}", args.out, ec.message());
        return false;
    }
    return clotho::ir::emit_llvm(*result.module, os, args.config, timings);
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (argc < 2) {
        return usage_error("no command; expected 'build', 'run' or 'check'");
    }

    // A global option may stand alone, without a command: `clotho --help`.
    {
        const std::string_view first = argv[1];
        if (first == "build") {
            args.command = Command::Build;
        } else if (first == "run") {
            args.command = Command::Run;
        } else if (first == "check") {
            args.command = Command::Check;
        } else if (first == "-h" || first == "--help") {
            print_help();
            return 0;
        } else if (first == "--version") {
            print_version();
            return 0;
        } else if (first == "--help-slang") {
            std::print("{}", clotho::slang_option_help());
            return 0;
        } else {
            return usage_error(std::format("unknown command '{}'; expected 'build', 'run' or 'check'", first));
        }
    }
    const std::string_view command_name = argv[1];
    const uint8_t scope = args.command == Command::Build ? kBuild : args.command == Command::Run ? kRun : kCheck;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg.starts_with("+") && !is_slang_plusarg(arg)) {
            if (args.command != Command::Run) {
                return usage_error(std::format("plusarg '{}' applies to 'run', not '{}'", arg, command_name));
            }
            args.plusargs.emplace_back(arg.substr(1));
            continue;
        }

        const OptSpec* spec = arg.starts_with("-") ? find_option(arg) : nullptr;
        if (spec == nullptr) {
            args.slang.push_back(arg); // source file or slang driver option
            continue;
        }
        if ((spec->scope & scope) == 0) {
            const std::string_view owner = spec->scope == kBuild ? "build" : spec->scope == kRun ? "run" : "check";
            return usage_error(std::format("{} applies to '{}', not '{}'", spec->name, owner, command_name));
        }

        // Operand: `--name=value`, or the next argv entry.
        std::string_view value;
        if (!spec->operand.empty()) {
            const size_t eq = arg.find('=');
            if (eq != std::string_view::npos) {
                value = arg.substr(eq + 1);
            } else if (i + 1 < argc) {
                value = argv[++i];
            }
            if (value.empty()) {
                return usage_error(std::format("{} needs an operand: {} {}", spec->name, spec->name, spec->operand));
            }
        }

        switch (spec->id) {
            case Opt::Emit:
                if (value == "llvm") {
                    args.emit = Emit::Llvm;
                } else if (value == "llvm-raw") {
                    args.emit = Emit::LlvmRaw;
                } else if (value == "ir") {
                    args.emit = Emit::Ir;
                } else {
                    return usage_error(std::format("unknown --emit target '{}'; expected llvm, llvm-raw or ir", value));
                }
                break;
            case Opt::Out:
                args.out = value;
                break;
            case Opt::Time:
                args.time = true;
                break;
            case Opt::O3:
                args.config.o3 = true;
                break;
            case Opt::Stat:
                // Comma-separated list, so one --stat covers a whole session.
                for (size_t pos = 0; pos <= value.size();) {
                    const size_t           end  = std::min(value.find(',', pos), value.size());
                    const std::string_view name = value.substr(pos, end - pos);
                    pos                         = end + 1;
                    if (name == "dedup") {
                        args.config.stats |= static_cast<uint32_t>(Stat::Dedup);
                    } else if (name == "dedup-sets") {
                        args.config.stats |= static_cast<uint32_t>(Stat::DedupSets);
                    } else if (name == "comb-fuse") {
                        args.config.stats |= static_cast<uint32_t>(Stat::CombFuse);
                    } else if (name == "finish") {
                        args.config.stats |= static_cast<uint32_t>(Stat::Finish);
                    } else if (name == "parallel") {
                        args.config.stats |= static_cast<uint32_t>(Stat::Parallel);
                    } else {
                        return usage_error(std::format("unknown --stat '{}'; expected dedup, dedup-sets, "
                                                       "comb-fuse, finish or parallel",
                                                       name));
                    }
                }
                break;
            case Opt::AllowSystem:
                // $system runs a shell command: arbitrary code execution, so the
                // tier is an explicit operator grant, never a default (LRM 20.17.1).
                if (value == "const") {
                    args.config.allow_system = SystemAccess::Const;
                } else if (value == "full") {
                    args.config.allow_system = SystemAccess::Full;
                } else {
                    return usage_error(std::format(
                      "unknown --allow-system tier '{}'; write --allow-system=const or --allow-system=full", value));
                }
                std::println(stderr, "clotho: warning: $system shell execution enabled (--allow-system={})", value);
                break;
            case Opt::DpiLib:
                args.config.dpi_libs.emplace_back(value);
                break;
            case Opt::Jobs: {
                unsigned   jobs = 0;
                const auto end  = value.data() + value.size();
                const auto res  = std::from_chars(value.data(), end, jobs);
                if (res.ec != std::errc{} || res.ptr != end) {
                    return usage_error(std::format("--jobs needs a thread count, got '{}'", value));
                }
                // One knob for the whole tool: Clotho's compile pool, and slang's
                // parser through its own -j (a later flag wins there, so an
                // explicit slang -j still overrides this).
                args.config.compile_jobs = jobs;
                args.slang.emplace_back("-j");
                args.slang.push_back(value);
                break;
            }
            case Opt::PerfJitdump:
                args.config.perf_jitdump = true;
                break;
            case Opt::SlangArg:
                args.slang.push_back(value);
                break;
            case Opt::Help:
                print_help();
                return 0;
            case Opt::Version:
                print_version();
                return 0;
            case Opt::HelpSlang:
                std::print("{}", clotho::slang_option_help());
                return 0;
        }
    }

    if (args.slang.empty()) {
        return usage_error("no source files");
    }
    // llvm-raw is the pre-pipeline dump; every other path needs runnable shape.
    args.config.lower_coro = args.emit != Emit::LlvmRaw;

    clotho::Timings        timings;
    clotho::Timings* const tp = args.time ? &timings : nullptr;

    const clotho::LowerResult result = clotho::lower_sources(args.slang, tp, args.config.allow_system);
    if (!result.diagnostics.empty()) {
        std::print(stderr, "{}", result.diagnostic_text());
    }

    // `check`: the diagnostics ARE the artifact. Deliberately ahead of the `ok`
    // gate below -- a design with a member that failed to lower is exactly what
    // this command is for, since an editor buffer is broken most of the time and
    // every other finding in the file is still wanted. Exit status keys on
    // whether anything reached Error, so warnings alone leave it clean and this
    // works as a CLI linter.
    //
    // Analysis-derived findings beyond the frontend's own (X-dependence, the rest
    // of the mismatch classes) surface here as they land; the case-pragma checks
    // already do. Nothing runs DesignFacts yet because nothing consumes it for
    // diagnostics.
    if (args.command == Command::Check) {
        if (args.time) {
            report_timings(timings, args.command);
        }
        return clotho::has_error(result.diagnostics) ? 1 : 0;
    }

    // An IR dump is analysis, not execution, so it tolerates a poisoned module:
    // when a lowering guard fires, seeing what DID lower is the whole point. The
    // diagnostics are already on stderr and the exit status still reports the
    // failure, so a partial dump cannot be mistaken for a whole design.
    if (args.command == Command::Build && args.emit == Emit::Ir && result.module != nullptr) {
        const bool ok = write_artifact(result, args, tp);
        if (args.time) {
            report_timings(timings, args.command);
        }
        return ok && result.ok ? 0 : 1;
    }
    if (!result.ok) {
        return 1;
    }

    int status = 0;
    if (args.command == Command::Run) {
        const int rc = clotho::ir::run_jit(*result.module, tp, args.config, args.plusargs);
        // run_jit returns a negative sentinel on setup failure; the design's own
        // status passes through unchanged.
        status = rc < 0 ? 1 : rc;
    } else {
        status = write_artifact(result, args, tp) ? 0 : 1;
    }
    if (args.time) {
        report_timings(timings, args.command);
    }
    return status;
}
