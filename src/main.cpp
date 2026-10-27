#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include <slang/driver/Driver.h>
#include <slang/util/VersionInfo.h>

#include "Lower.h"
#include "dap/Dap.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/Printer.h"
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

// What `--version` prints after "clotho", and what the language and debug
// servers report to their clients: the release, then the commit it was built from.
constexpr std::string_view kVersion = CLOTHO_VERSION " (" CLOTHO_GIT_HASH ")";

enum class Command : uint8_t { Build, Run, Lint, Synth, Lsp, Dap };

// A set of commands, one bit per Command.
using Commands = uint32_t;

constexpr Commands bit(Command command) {
    return Commands{ 1 } << static_cast<uint32_t>(command);
}

// The commands that read source. An option about reading it (`--time`,
// `--allow-system`) is scoped to all of them, `lint` included.
constexpr Commands kSource = bit(Command::Build) | bit(Command::Run) | bit(Command::Lint)
#ifdef CLOTHO_SYNTH
                             | bit(Command::Synth)
#endif
  ;
constexpr Commands kAny = kSource | bit(Command::Lsp) | bit(Command::Dap);

// The options the artifact is compiled with and then run with, which `build` is
// told exactly as `run` is: the --gc-* group (the backend bakes an rt_gc_config
// into the emitted module and calls rt_gc_init from its main), `--coverage` (the
// coverage model the backend emits names its own destination, so the report file
// is a mode compiled in, not something `run` can add to an artifact built
// without it), and `-O`, `--stat` and `--sanitize`, which steer code generation.
// `lint` and `synth` never reach code generation, so they refuse all of these.
constexpr Commands kBuildRun = bit(Command::Build) | bit(Command::Run);

// `-o` names the artifact, and two commands produce one.
constexpr Commands kArtifact = bit(Command::Build)
#ifdef CLOTHO_SYNTH
                               | bit(Command::Synth)
#endif
  ;

// A command as the user types it.
struct CommandInfo {
    Command          command;
    std::string_view name;
    // One line, lowercase and unpunctuated: the command list in `--help`.
    std::string_view summary;
    // What follows the name on its usage line.
    std::string_view usage;
    // A paragraph for the command's own help, after its usage line.
    std::string_view notes = {};
};

// Runtime plusargs (LRM 21.6) are `run`'s alone. See is_slang_plusarg.
constexpr std::string_view kPlusargNote =
  "An argument starting with '+' that is not an option goes to the design itself, for $test$plusargs "
  "and $value$plusargs to read while it runs (LRM 21.6). Use '--' to stop that and pass everything "
  "after it through as written.";

// The commands this build has, in the order `--help` lists them. `synth` is
// compiled out of a Release build, and a usage message that offers a command the
// binary will then reject is worse than one that never mentions it.
constexpr CommandInfo kCommands[] = {
    { Command::Build,
      "build",
      "compile the design and write the generated code, without running it",
      "[options] <files>..." },
    { Command::Run, "run", "compile the design and simulate it", "[options] <files>... [+plusarg...]", kPlusargNote },
    { Command::Lint,
      "lint",
      "check the design and report problems, without compiling or running it",
      "[options] <files>..." },
#ifdef CLOTHO_SYNTH
    { Command::Synth, "synth", "write the design back out as synthesizable SystemVerilog", "[options] <files>..." },
#endif
    { Command::Lsp, "lsp", "serve the Language Server Protocol over stdin/stdout, for an editor", "" },
    { Command::Dap, "dap", "serve the Debug Adapter Protocol over stdin/stdout, to debug a design", "" },
};

const CommandInfo* find_command(std::string_view name) {
    for (const CommandInfo& c : kCommands) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

const CommandInfo& command_info(Command command) {
    for (const CommandInfo& c : kCommands) {
        if (c.command == command) {
            return c;
        }
    }
    // Every Command a user can select has a row, and the dispatch only selects those.
    std::println(stderr, "clotho: internal error: no command table row");
    std::abort();
}

std::string_view command_name(Command command) {
    return command_info(command).name;
}

// The names of the commands in `set`, in listing order, as a sentence lists
// them: 'a', then 'a' or 'b', then 'a', 'b' or 'c'.
std::string command_list(Commands set) {
    std::vector<std::string_view> names;
    for (const CommandInfo& c : kCommands) {
        if ((set & bit(c.command)) != 0) {
            names.push_back(c.name);
        }
    }
    std::string text;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            text += i + 1 == names.size() ? " or " : ", ";
        }
        text += std::format("'{}'", names[i]);
    }
    return text;
}

template<typename T>
bool is_given(const std::optional<T>& value) {
    return value.has_value();
}

template<typename T>
bool is_given(const std::vector<T>& value) {
    return !value.empty();
}

// "[build|run] ": the commands that take an option, as `--help-all` prints them in
// front of its description. Empty when every command that reads source takes it.
std::string scope_tag(Commands commands) {
    if ((commands & kSource) == kSource) {
        return {};
    }
    std::string tag = "[";
    for (const CommandInfo& c : kCommands) {
        if ((commands & bit(c.command)) != 0) {
            tag += tag.size() > 1 ? "|" : "";
            tag += c.name;
        }
    }
    return tag + "] ";
}

// The options Clotho registers on the Driver's CommandLine, each with the
// commands that take it. One registration feeds the parser, the check that an
// option is not used under a command that ignores it (check_scope), and which
// commands' help lists it, so the three cannot disagree.
class Registry {
public:
    struct Entry {
        // The first spelling, which is how diagnostics name the option.
        std::string           name;
        Commands              commands;
        std::function<bool()> given;
    };

    explicit Registry(slang::CommandLine& cl)
      : cl_(cl) {}

    // `names`, `value`, `desc` and what follows are slang's CommandLine::add.
    // The value's variable is also how the entry knows whether the command line
    // gave the option.
    template<typename T, typename... Rest>
    void add(Commands commands, std::string_view names, T& value, std::string_view desc, Rest&&... rest) {
        cl_.add(names, value, desc, std::forward<Rest>(rest)...);
        entries_.push_back(
          { std::string(names.substr(0, names.find(','))), commands, [&value] { return is_given(value); } });
    }

    std::span<const Entry> entries() const {
        return entries_;
    }

    const Entry* find(std::string_view name) const {
        for (const Entry& e : entries_) {
            if (e.name == name) {
                return &e;
            }
        }
        return nullptr;
    }

private:
    slang::CommandLine& cl_;
    std::vector<Entry>  entries_;
};

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
    std::optional<bool>        help_all;
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
// The first argument of each registration is the commands that take the option;
// check_scope refuses it under any other, and the description needs no tag of
// its own for that.
void register_options(Registry& r, Options& o) {
    r.add(bit(Command::Build),
          "--emit",
          o.emit,
          "What to write: llvm (LLVM IR for the design, the default), ir (Clotho's own "
          "intermediate form), llvm-raw (LLVM IR before the coroutine passes; for working on clotho)",
          "<target>");
    r.add(kArtifact,
          "-o,--output",
          o.out,
          "Write the output to this file instead of stdout",
          "<file>",
          slang::CommandLineFlags::FilePath);
    r.add(kSource, "--time", o.time, "Print how long each stage took: reading the source, compiling, simulating");
    r.add(kBuildRun, "-O", o.o3, "Compile the design more thoroughly: slower to start up, faster to simulate");
    r.add(kBuildRun,
          "--stat",
          o.stats,
          "Print a compiler statistic while building, for working on clotho: dedup, "
          "dedup-sets, comb-fuse, finish or parallel (repeatable, comma-separated)",
          "<name>",
          slang::CommandLineFlags::CommaList);
    r.add(kBuildRun,
          "--sanitize",
          o.sanitizers,
          "Check the design for a class of bug while it runs, reporting when one "
          "happens: bounds (an array index outside the array)"
#ifdef CLOTHO_XSAN
          ", xprov (where an x value decided something, and what made it x)"
#endif
          ". Costs simulation time (repeatable, comma-separated)",
          "<check>",
          slang::CommandLineFlags::CommaList);
    r.add(kBuildRun,
          "--coverage",
          o.coverage,
          "Write the design's functional-coverage report here: which of the bins its "
          "covergroups declare the run reached, and which it never did (LRM 19). No report by "
          "default. The destination is compiled into the generated code, so a build and the run "
          "of what it built must be given the same file",
          "<file>",
          slang::CommandLineFlags::FilePath);
    r.add(kBuildRun,
          "--gc-stress",
          o.gc_stress,
          "Reclaim unused class objects at every opportunity instead of when the "
          "design has created enough of them. Slow by design: it turns an object Clotho failed "
          "to keep into an immediate wrong answer rather than a rare one, so it is for working "
          "on clotho itself. Changes the generated code, so a build and the run of what it "
          "built must be given the same setting");
    r.add(kBuildRun,
          "--gc-stats",
          o.gc_stats,
          "Print what reclaiming class objects did when the design exits: how many "
          "times it ran, how much it reclaimed, and how long the design was paused for it");
    r.add(kBuildRun,
          "--gc-nursery",
          o.gc_nursery_bytes,
          "How many bytes of class objects the design may create between one reclaim "
          "and the next (default 8388608). A plain number of bytes",
          "<bytes>");
    r.add(kBuildRun,
          "--gc-heap-max",
          o.gc_heap_bytes_max,
          "Size in bytes the design's class objects should stay under: reaching it "
          "asks for a reclaim at the next opportunity, and passing it anyway is reported rather "
          "than refused, since a design is free to create objects without ever pausing. 0, the "
          "default, sets no limit",
          "<bytes>");
    r.add(kBuildRun,
          "--gc-full-every",
          o.gc_full_every,
          "Reclaim the whole heap on every Nth reclaim instead of only the objects "
          "created since the last one. Only a whole-heap pass frees objects that have already "
          "survived a reclaim, so this trades cheap frequent passes against how long those "
          "linger. 0, the default, lets the runtime decide from how fast the heap is growing",
          "<n>");
    r.add(kSource,
          "--allow-system",
          o.allow_system,
          "Let the design run shell commands through $system, refused by default: 'const' allows "
          "only commands written literally in the source, 'full' allows any command the design "
          "builds while it runs",
          "<tier>");
    r.add(kSource,
          "--loose-timescale",
          o.loose_timescale,
          "Accept a design where some parts declare a time scale and others do not, refused by "
          "default: the parts that do not take --timescale's value, or 1ns/1ns when it is not given");
    r.add(bit(Command::Run),
          "--loose-stale-ref",
          o.loose_stale_ref,
          "Report and keep simulating when a ref argument no longer refers to the variable it "
          "was bound to, which stops the run by default: growing or reordering an array or queue "
          "while a ref refers to an element of it leaves that ref on something else entirely, so "
          "nothing after the first one means anything. For naming every such ref in one run");
    r.add(bit(Command::Run),
          "--dpi-lib",
          o.dpi_libs,
          "Library holding the C functions this design imports through DPI-C. Give a .o or .a "
          "if C also calls back into the design; a .so is enough for imports alone",
          "<path>",
          slang::CommandLineFlags::FilePath);
#ifdef CLOTHO_SYNTH
    r.add(bit(Command::Synth),
          "--sidecar",
          o.sidecar,
          "Also write the JSON sidecar here: what the SystemVerilog cannot carry -- the "
          "original-to-emitted name map every constraint file depends on, and each signal's inferred "
          "storage, clock and reset",
          "<path>",
          slang::CommandLineFlags::FilePath);
    r.add(bit(Command::Synth),
          "--target",
          o.target,
          "Which language to write: '2005' for IEEE 1364-2005, 'sv' for IEEE 1800 (default)",
          "<lang>");
    r.add(bit(Command::Synth),
          "--hierarchy",
          o.hierarchy,
          "How much of the instance tree to keep: 'instances' writes one module per instance "
          "with names as written (default), 'flat' writes one module with paths mangled into names",
          "<mode>");
    r.add(bit(Command::Synth),
          "--no-legalize",
          o.no_legalize,
          "Translate the design as written instead of writing out the hardware it implies: no "
          "mux trees rebuilt, no implicit branch filled in");
    r.add(bit(Command::Synth),
          "--full-case",
          o.full_case,
          "Believe `full_case` and `unique`: a case the design says is complete stops inferring "
          "a latch. Reported wherever the claim is what removed the latch, since simulation will still "
          "hold the old value");
    r.add(bit(Command::Synth),
          "--parallel-case",
          o.parallel_case,
          "Believe `parallel_case` and `unique0`: the arms are treated as mutually exclusive");
#endif
    r.add(bit(Command::Run),
          "--perf-jitdump",
          o.perf_jitdump,
          "Let `perf` see names for the design's compiled code (writes a jitdump under "
          "$JITDUMPDIR); for profiling clotho");
    r.add(kAny, "-h,--help", o.help, "Show this help and exit");
    r.add(kAny, "--help-all", o.help_all, "Show every option, slang's included, and exit");
    r.add(kAny, "--version", o.version, "Show version information and exit");
}

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

// A command-specific option used under another command is an error, not a
// silently ignored no-op (PRINCIPLES.md: never silently accept).
bool check_scope(const Registry& registry, Command command, int& status) {
    for (const Registry::Entry& e : registry.entries()) {
        if (e.given() && (e.commands & bit(command)) == 0) {
            status = usage_error(
              std::format("{} applies to {}, not '{}'", e.name, command_list(e.commands), command_name(command)));
            return false;
        }
    }
    return true;
}

// Help is laid out for this many columns. An option's spelling gets at most
// kHelpKeyMax of them; a longer one puts its description on the line below.
constexpr size_t kHelpWidth  = 100;
constexpr size_t kHelpIndent = 2;
constexpr size_t kHelpGap    = 2;
constexpr size_t kHelpKeyMax = 32;

// The slang options a command's help lists beside Clotho's own: the ones most
// designs need, in the order they are listed. `--help-all` lists the rest. This
// only names options; slang's registration stays the one record of what each
// takes and says.
constexpr std::string_view kCommonSourceOptions[] = {
    "-f",
    "-F",
    "-I",
    "-D",
    "-U",
    "-y",
    "-Y",
    "--libmap",
    "-L",
    "--top",
    "-G",
    "--timescale",
    "--std",
    "-j",
    "-W",
    "--error-limit",
    "--color-diagnostics",
};

// An option as slang's help text prints it.
struct HelpOption {
    std::vector<std::string> names; // "-I", "--include-directory", "+incdir"
    std::string              value; // "<dir-pattern>[,...]"; empty for a flag
    std::string              desc;

    bool has_name(std::string_view name) const {
        return std::ranges::find(names, name) != names.end();
    }
};

// One line of a listing: an option's spellings, then what it does.
struct HelpRow {
    std::string key;
    std::string desc;
};

struct HelpSection {
    std::string_view     title;
    std::vector<HelpRow> rows;
};

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    return text;
}

// CommandLine prints its options but cannot enumerate them, so they are read back
// out of getHelpText: one entry per option, `  <names> <value>`, at least two
// spaces, then the description, with continuation lines indented to the same
// column. Option names and value placeholders never hold two spaces in a row.
std::vector<HelpOption> read_help_options(std::string_view text) {
    std::vector<HelpOption> options;
    bool                    listing = false;
    for (size_t pos = 0; pos < text.size();) {
        const size_t           end  = std::min(text.find('\n', pos), text.size());
        const std::string_view line = text.substr(pos, end - pos);
        pos                         = end + 1;
        if (!listing) {
            listing = line == "OPTIONS:";
        } else if (line.size() > 2 && line.starts_with("  ") && line[2] != ' ') {
            const std::string_view body  = line.substr(2);
            const size_t           gap   = body.find("  ");
            const std::string_view key   = trim(body.substr(0, gap));
            const size_t           space = key.find(' ');
            const std::string_view names = key.substr(0, space);
            HelpOption             option;
            for (size_t at = 0; at <= names.size();) {
                const size_t comma = std::min(names.find(',', at), names.size());
                option.names.emplace_back(names.substr(at, comma - at));
                at = comma + 1;
            }
            option.value = space == std::string_view::npos ? "" : trim(key.substr(space));
            option.desc  = gap == std::string_view::npos ? "" : trim(body.substr(gap));
            options.push_back(std::move(option));
        } else if (!trim(line).empty() && !options.empty()) {
            std::string& desc = options.back().desc;
            desc += desc.empty() ? "" : " ";
            desc += trim(line);
        }
    }
    return options;
}

// Greedy word wrap. A word longer than `width` gets a line of its own.
std::vector<std::string> wrap(std::string_view text, size_t width) {
    std::vector<std::string> lines;
    std::string              line;
    for (size_t pos = 0; pos < text.size();) {
        const size_t           end  = std::min(text.find(' ', pos), text.size());
        const std::string_view word = text.substr(pos, end - pos);
        pos                         = end + 1;
        if (word.empty()) {
            continue;
        }
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            lines.push_back(std::move(line));
            line.clear();
        }
        line += line.empty() ? "" : " ";
        line += word;
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

void append_paragraph(std::string& out, std::string_view text) {
    for (const std::string& line : wrap(text, kHelpWidth)) {
        out += line;
        out += '\n';
    }
    out += '\n';
}

// Lists the sections' rows with one description column, as wide as the widest
// spelling that fits in kHelpKeyMax.
void append_sections(std::string& out, const std::vector<HelpSection>& sections) {
    size_t widest = 0;
    for (const HelpSection& section : sections) {
        for (const HelpRow& row : section.rows) {
            if (row.key.size() <= kHelpKeyMax) {
                widest = std::max(widest, row.key.size());
            }
        }
    }
    const size_t desc_col   = kHelpIndent + widest + kHelpGap;
    const size_t desc_width = kHelpWidth - desc_col;
    for (const HelpSection& section : sections) {
        if (section.rows.empty()) {
            continue;
        }
        out += std::format("{}:\n", section.title);
        for (const HelpRow& row : section.rows) {
            const std::vector<std::string> lines = wrap(row.desc, desc_width);
            std::string                    head(kHelpIndent, ' ');
            head += row.key;
            size_t next = 0;
            if (row.key.size() <= widest && !lines.empty()) {
                head.resize(desc_col, ' ');
                head += lines[next++];
            }
            out += head;
            out += '\n';
            for (; next < lines.size(); ++next) {
                out += std::string(desc_col, ' ') + lines[next] + '\n';
            }
        }
        out += '\n';
    }
}

// An option's spellings and value placeholder as one listing key. The `+` forms
// (`+incdir`, `+define`, `+libext`) are Verilog-tool spellings of the same
// options, listed only when everything is.
std::string help_key(const HelpOption& option, bool with_plus_forms) {
    std::string key;
    for (const std::string& name : option.names) {
        if (with_plus_forms || !name.starts_with('+')) {
            key += key.empty() ? "" : ", ";
            key += name;
        }
    }
    if (!option.value.empty()) {
        key += ' ';
        key += option.value;
    }
    return key;
}

// The help for one command, for the command list and the options most designs
// need (`command` unset), or for every option (`all`).
std::string render_help(const Registry&           registry,
                        const slang::CommandLine& cmd_line,
                        std::optional<Command>    command,
                        bool                      all) {
    // `--help-all` after a command lists everything too.
    const bool scoped = command.has_value() && !all;

    std::string out;
    if (scoped) {
        const CommandInfo& info = command_info(*command);
        out += std::format("clotho {} — {}\n\nUsage: clotho {}", info.name, info.summary, info.name);
        out += info.usage.empty() ? "" : " ";
        out += info.usage;
        out += "\n\n";
        if (!info.notes.empty()) {
            append_paragraph(out, info.notes);
        }
    } else {
        out += "clotho — a SystemVerilog simulator\n\nUsage: clotho <command> [options] <files>...\n\n";
        HelpSection commands{ "Commands", {} };
        for (const CommandInfo& c : kCommands) {
            commands.rows.push_back({ std::string(c.name), std::string(c.summary) });
        }
        append_sections(out, { commands });
        if (all) {
            append_paragraph(out,
                             "A tag such as [build|run] names the commands that take an option; an option "
                             "with no tag is taken by every command that reads source.");
        }
    }

    const std::vector<HelpOption> options = read_help_options(cmd_line.getHelpText({}));
    HelpSection                   own{ "Options", {} };
    HelpSection                   source{ "Source options", {} };
    for (const HelpOption& option : options) {
        const Registry::Entry* entry = registry.find(option.names.front());
        if (entry != nullptr) {
            const bool listed =
              all || (scoped ? (entry->commands & bit(*command)) != 0 : (entry->commands & kSource) == kSource);
            if (listed) {
                own.rows.push_back({ help_key(option, all), (all ? scope_tag(entry->commands) : "") + option.desc });
            }
        } else if (all) {
            source.rows.push_back({ help_key(option, true), option.desc });
        }
    }
    const bool reads_source = !scoped || (bit(*command) & kSource) != 0;
    if (!all && reads_source) {
        for (const std::string_view name : kCommonSourceOptions) {
            const auto found = std::ranges::find_if(options, [&](const HelpOption& o) { return o.has_name(name); });
            if (found != options.end()) {
                source.rows.push_back({ help_key(*found, false), found->desc });
            }
        }
    }
    append_sections(out, { own, source });

    if (all) {
        append_paragraph(out, kPlusargNote);
    } else if (scoped && reads_source) {
        append_paragraph(out, "Run 'clotho --help-all' for every option, including the rest of the source options.");
    } else if (!scoped) {
        append_paragraph(out,
                         "Run 'clotho <command> --help' for the options of one command, and 'clotho --help-all' for "
                         "every option.");
    }
    return out;
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

#ifdef CLOTHO_SYNTH
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
#endif

// `build`: write the requested artifact. `-o -` (the default) means stdout.
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
    Registry            registry(cmd_line);
    register_options(registry, options);
    driver->addStandardArgs();
    cmd_line.setProgramName("clotho <command>");

    const auto print_help = [&](std::optional<Command> command, bool all = false) {
        std::print("{}", render_help(registry, cmd_line, command, all));
    };
    const auto print_version = [] {
        std::println("clotho {}", kVersion);
        std::println("LLVM {}", CLOTHO_LLVM_VERSION);
        std::println("slang {}", slang::VersionInfo::getVersionString());
        std::println("Copyright (C) 2026 Kalyan Sriram");
        std::println("License GPL-3.0-only <https://www.gnu.org/licenses/gpl-3.0.html>.");
        std::println("The runtime linked into simulations is MPL-2.0.");
        std::println("This is free software: you are free to change and redistribute it.");
        std::println("There is NO WARRANTY, to the extent permitted by law.");
    };

    if (argc < 2) {
        return usage_error(std::format("no command; expected {}", command_list(kAny)));
    }
    Command command = Command::Run;
    {
        const std::string_view first = argv[1];
        if (const CommandInfo* info = find_command(first)) {
            command = info->command;
        } else if (first == "-h" || first == "--help" || first == "--help-all") {
            // Help and version stand alone, with no command.
            print_help(std::nullopt, first == "--help-all");
            return 0;
        } else if (first == "--version") {
            print_version();
            return 0;
        } else {
            return usage_error(std::format("unknown command '{}'; expected {}", first, command_list(kAny)));
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
            if (arg == "-h" || arg == "--help" || arg == "--help-all") {
                print_help(command, arg == "--help-all");
                return 0;
            }
            if (arg == "--version") {
                print_version();
                return 0;
            }
            return usage_error(std::format("'{}' takes no arguments, got '{}'", command_name(command), arg));
        }
        if (command == Command::Dap) {
            clotho::dap::serve(kVersion, argv[0]);
        }
        clotho::lsp::serve(kVersion);
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
    if (options.help_all == true || options.help == true) {
        print_help(command, options.help_all == true);
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
    if (!check_scope(registry, command, status)) {
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
            return usage_error(std::format("unknown --sanitize check '{}'; expected {}", name, kSanitizeList));
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
    config.gc_stats  = options.gc_stats == true;
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
