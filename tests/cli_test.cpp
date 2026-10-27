// The `clotho` command line, driven as a process: usage errors, which commands
// accept which options, and the slang options the tool exposes (`-G`, `--libmap`).
//
// Exit status and stderr are the contract here, and only a real process shows
// them. A scope violation is reported before any source is read, so those cases
// pass a design that is never opened.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Subprocess.h"

#if !defined(_WIN32)

#include <sys/wait.h>

namespace cli_test {

namespace fs = std::filesystem;

struct Result {
    int         code = -1;
    std::string out;
    std::string err;
};

// A temp directory that holds the design files and the captured streams of one
// test, and is removed with it.
class Scratch {
public:
    Scratch()
      : dir_(fs::temp_directory_path() / clotho::test::detail::unique_stem()) {
        fs::create_directories(dir_);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    Scratch(const Scratch&)            = delete;
    Scratch& operator=(const Scratch&) = delete;

    const fs::path& dir() const {
        return dir_;
    }

    fs::path path(std::string_view name) const {
        return dir_ / name;
    }

    fs::path write(std::string_view name, std::string_view text) const {
        const fs::path p = path(name);
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        return p;
    }

    // Runs `clotho <args>` with `args` already shell-quoted.
    Result run(std::string_view args) const {
        const fs::path    out = path("stdout.txt");
        const fs::path    err = path("stderr.txt");
        const std::string cmd =
          std::format("\"{}\" {} > \"{}\" 2> \"{}\"", CLOTHO_EXE, args, out.string(), err.string());
        const int status = std::system(cmd.c_str());
        Result    r;
        r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        r.out  = clotho::test::detail::read_file(out);
        r.err  = clotho::test::detail::read_file(err);
        return r;
    }

private:
    fs::path dir_;
};

constexpr std::string_view kParamDesign = R"(
module top #(parameter int W = 4, parameter string S = "dflt");
    initial $display("W=%0d S=%s", W, S);
endmodule
)";

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// The first line of `--version` without its "clotho " prefix: the release and
// the commit, which is also what the servers report.
std::string version_string(const Scratch& s) {
    const Result r = s.run("--version");
    REQUIRE(r.code == 0);
    REQUIRE(r.out.starts_with("clotho "));
    return r.out.substr(7, r.out.find('\n') - 7);
}

// The first spelling of every option a help text lists. A row starts two columns
// in with its spellings; a description's continuation line is indented further.
std::vector<std::string> listed_options(const std::string& help) {
    std::vector<std::string> names;
    std::istringstream       in(help);
    for (std::string line; std::getline(in, line);) {
        if (line.size() > 2 && line.starts_with("  ") && (line[2] == '-' || line[2] == '+')) {
            names.push_back(line.substr(2, line.find_first_of(" ,", 2) - 2));
        }
    }
    return names;
}

bool lists(const std::string& help, std::string_view name) {
    const std::vector<std::string> names = listed_options(help);
    return std::ranges::find(names, name) != names.end();
}

// Terminal columns of the widest line: one per UTF-8 code point.
size_t widest_line(const std::string& text) {
    size_t widest = 0;
    size_t width  = 0;
    for (const char c : text) {
        if (c == '\n') {
            widest = std::max(widest, width);
            width  = 0;
        } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++width;
        }
    }
    return std::max(widest, width);
}

// The slang options a command's help lists beside Clotho's own.
const std::vector<std::string> kCommonSource = {
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
    "--error-limit",
    "--color-diagnostics",
};

// slang options a design can do without: only `--help-all` lists them.
const std::vector<std::string> kUncommonSource = {
    "--depfile-trim", "--max-case-analysis-steps", "--isystem", "--diag-json", "-v", "-W"
};

// The commands this build has. A command a build leaves out (`synth`, `lsp` and
// `dap` are opt-in) is an unknown command, which exits 2 whatever follows it.
std::vector<std::string_view> built_commands(const Scratch& s) {
    std::vector<std::string_view> commands;
    for (const std::string_view command : { "build", "run", "lint", "synth", "lsp", "dap" }) {
        if (s.run(std::format("{} --help", command)).code == 0) {
            commands.push_back(command);
        }
    }
    return commands;
}

} // namespace cli_test

using namespace cli_test;

TEST_CASE("cli: --help lists the commands and the options most designs need") {
    const Scratch s;
    const Result  r = s.run("--help");
    CHECK(r.code == 0);
    CHECK(contains(r.out, "Usage: clotho <command> [options] <files>..."));
    for (const std::string_view command : built_commands(s)) {
        CAPTURE(command);
        CHECK(contains(r.out, std::format("\n  {} ", command)));
    }
    for (const std::string& name : kCommonSource) {
        CAPTURE(name);
        CHECK(lists(r.out, name));
    }
    for (const std::string_view name :
         { "--time", "--allow-system", "--loose-timescale", "-h", "--help-all", "--version" }) {
        CAPTURE(name);
        CHECK(lists(r.out, name));
    }
    // Command-specific options are one `clotho <command> --help` away, and the
    // rest of slang's `--help-all`.
    for (const std::string_view name : { "--emit", "-o", "-O", "--dpi-lib", "--gc-nursery", "--stat" }) {
        CAPTURE(name);
        CHECK_FALSE(lists(r.out, name));
    }
    for (const std::string& name : kUncommonSource) {
        CAPTURE(name);
        CHECK_FALSE(lists(r.out, name));
    }
    CHECK(contains(r.out, "clotho --help-all"));
}

TEST_CASE("cli: <command> --help lists only the options that command takes") {
    const Scratch s;
    struct Case {
        std::string_view              command;
        std::string_view              usage;
        std::vector<std::string_view> takes;
        std::vector<std::string_view> refuses;
    };
    const Case cases[] = {
        { "build",
          "Usage: clotho build [options] <files>...\n",
          { "--emit",
            "-o",
            "-O",
            "--stat",
            "--sanitize",
            "--coverage",
            "--gc-stress",
            "--gc-stats",
            "--gc-nursery",
            "--gc-heap-max",
            "--gc-full-every",
            "--time",
            "--allow-system",
            "--loose-timescale",
            "-h",
            "--help-all",
            "--version" },
          { "--dpi-lib", "--loose-stale-ref", "--perf-jitdump" } },
        { "run",
          "Usage: clotho run [options] <files>... [+plusarg...]\n",
          { "-O",
            "--stat",
            "--sanitize",
            "--coverage",
            "--gc-stress",
            "--gc-stats",
            "--gc-nursery",
            "--gc-heap-max",
            "--gc-full-every",
            "--time",
            "--allow-system",
            "--loose-timescale",
            "--loose-stale-ref",
            "--dpi-lib",
            "--perf-jitdump",
            "-h",
            "--help-all",
            "--version" },
          { "--emit", "-o" } },
        { "lint",
          "Usage: clotho lint [options] <files>...\n",
          { "--time", "--allow-system", "--loose-timescale", "-h", "--help-all", "--version" },
          { "--emit",
            "-o",
            "-O",
            "--stat",
            "--sanitize",
            "--coverage",
            "--gc-stress",
            "--gc-stats",
            "--gc-nursery",
            "--gc-heap-max",
            "--gc-full-every",
            "--dpi-lib",
            "--loose-stale-ref",
            "--perf-jitdump" } },
    };
    for (const Case& c : cases) {
        CAPTURE(c.command);
        const Result r = s.run(std::format("{} --help", c.command));
        CHECK(r.code == 0);
        CHECK(contains(r.out, c.usage));
        for (const std::string_view name : c.takes) {
            CAPTURE(name);
            CHECK(lists(r.out, name));
        }
        for (const std::string_view name : c.refuses) {
            CAPTURE(name);
            CHECK_FALSE(lists(r.out, name));
        }
        for (const std::string& name : kCommonSource) {
            CAPTURE(name);
            CHECK(lists(r.out, name));
        }
        for (const std::string& name : kUncommonSource) {
            CAPTURE(name);
            CHECK_FALSE(lists(r.out, name));
        }
        CHECK(contains(r.out, "clotho --help-all"));
    }
    // Only `run` takes plusargs, so only its usage line and help say so.
    CHECK(contains(s.run("run --help").out, "LRM 21.6"));
    CHECK_FALSE(contains(s.run("build --help").out, "plusarg"));
}

TEST_CASE("cli: the servers' help lists no source options") {
    const Scratch s;
    for (const std::string_view command : built_commands(s)) {
        if (command != "lsp" && command != "dap") {
            continue;
        }
        CAPTURE(command);
        const Result r = s.run(std::format("{} --help", command));
        CHECK(r.code == 0);
        CHECK(contains(r.out, std::format("Usage: clotho {}\n", command)));
        CHECK(listed_options(r.out) == std::vector<std::string>{ "-h", "--help-all", "--version" });
    }
}

TEST_CASE("cli: --help-all lists every option, tagged with the commands that take it") {
    const Scratch s;
    const Result  all = s.run("--help-all");
    CHECK(all.code == 0);
    for (const std::string& name : kCommonSource) {
        CAPTURE(name);
        CHECK(lists(all.out, name));
    }
    for (const std::string& name : kUncommonSource) {
        CAPTURE(name);
        CHECK(lists(all.out, name));
    }
    // The Verilog-style spellings are listed with the option they alias.
    CHECK(contains(all.out, "-I, --include-directory, +incdir <dir-pattern>[,...]"));
    CHECK(contains(all.out, "[build] What to write"));
    CHECK(contains(all.out, "[build|run] Compile the design more thoroughly"));
    CHECK(contains(all.out, "[run] Library holding the C functions"));
    // Whatever a command's own help lists, `--help-all` lists too, and
    // `--help-all` after a command is the same list.
    const std::vector<std::string> everything = listed_options(all.out);
    for (const std::string_view command : built_commands(s)) {
        CAPTURE(command);
        for (const std::string& name : listed_options(s.run(std::format("{} --help", command)).out)) {
            CAPTURE(name);
            CHECK(std::ranges::find(everything, name) != everything.end());
        }
        CHECK(s.run(std::format("{} --help-all", command)).out == all.out);
    }
}

TEST_CASE("cli: help is laid out in 100 columns") {
    const Scratch            s;
    std::vector<std::string> invocations = { "--help", "--help-all" };
    for (const std::string_view command : built_commands(s)) {
        invocations.push_back(std::format("{} --help", command));
    }
    for (const std::string& args : invocations) {
        CAPTURE(args);
        const Result r = s.run(args);
        CHECK(r.code == 0);
        CHECK(widest_line(r.out) <= 100);
    }
}

TEST_CASE("cli: --version names the release and the commit it was built from") {
    const Scratch s;
    const Result  r = s.run("--version");
    CHECK(r.code == 0);
    const std::string first = r.out.substr(0, r.out.find('\n'));
    // `clotho <major>.<minor>.<patch>[-<pre-release>] (<git hash>)`; a tree
    // without git reports the hash as "unknown".
    const std::regex format(R"(clotho \d+\.\d+\.\d+(-[0-9A-Za-z.]+)? \(([0-9a-f]+|unknown)\))");
    CHECK_MESSAGE(std::regex_match(first, format), first);
    CHECK(contains(r.out, "\nLLVM "));
    CHECK(contains(r.out, "\nslang "));
}

TEST_CASE("cli: lsp reports the version --version prints") {
    const Scratch s;
    // `lsp` exists only in a build with CLOTHO_LSP.
    if (s.run("lsp --help").code != 0) {
        MESSAGE("this build has no lsp command");
        return;
    }
    const std::string version = version_string(s);
    std::string       input;
    for (const std::string_view body : { R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
                                         R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})",
                                         R"({"jsonrpc":"2.0","method":"exit"})" }) {
        input += std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
    }
    const fs::path in = s.write("session.lsp", input);
    const Result   r  = s.run(std::format("lsp < \"{}\"", in.string()));
    CHECK(r.code == 0);
    CHECK_MESSAGE(contains(r.out, std::format("\"version\":\"{}\"", version)), r.out);
}

TEST_CASE("cli: lint refuses every option that steers code generation") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    struct Case {
        std::string_view flag;
        std::string_view name;
    };
    const Case cases[] = {
        { "-O", "-O" },
        { "--stat=dedup", "--stat" },
        { "--sanitize=bounds", "--sanitize" },
        { "--gc-stats", "--gc-stats" },
        { "--gc-full-every=3", "--gc-full-every" },
        { "--coverage=cov.txt", "--coverage" },
    };
    for (const Case& c : cases) {
        CAPTURE(c.flag);
        const Result r = s.run(std::format("lint {} \"{}\"", c.flag, design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, std::format("{} applies to 'build' or 'run', not 'lint'", c.name)));
    }
}

TEST_CASE("cli: synth refuses every option that steers code generation") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    // `synth` exists only in a Debug build (CLOTHO_SYNTH); a build without it
    // rejects the command word, which is its own, earlier, usage error.
    const Result probe = s.run(std::format("synth \"{}\"", design.string()));
    if (contains(probe.err, "unknown command 'synth'")) {
        MESSAGE("this build has no synth command");
        return;
    }
    for (const std::string_view flag : { "-O", "--stat=dedup", "--sanitize=bounds" }) {
        CAPTURE(flag);
        const Result r = s.run(std::format("synth {} \"{}\"", flag, design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "applies to 'build' or 'run', not 'synth'"));
    }
}

TEST_CASE("cli: build refuses the options that only mean something while simulating") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    for (const std::string_view flag : { "--dpi-lib=x.o", "--perf-jitdump", "--loose-stale-ref" }) {
        CAPTURE(flag);
        const Result r = s.run(std::format("build {} \"{}\"", flag, design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "applies to 'run', not 'build'"));
    }
}

TEST_CASE("cli: build and run accept the code-generation options") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    SUBCASE("build") {
        const Result r = s.run(std::format("build -O --sanitize=bounds --stat=dedup \"{}\"", design.string()));
        CHECK(r.code == 0);
    }
    SUBCASE("build --emit=ir stops before code generation and takes them without effect") {
        const Result r = s.run(std::format("build --emit=ir -O --sanitize=bounds \"{}\"", design.string()));
        CHECK(r.code == 0);
    }
    SUBCASE("run") {
        const Result r = s.run(std::format("run -O --sanitize=bounds --gc-full-every=3 \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(r.out == "W=4 S=dflt\n");
    }
}

TEST_CASE("cli: -G overrides a parameter of the top-level module") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    SUBCASE("an integer") {
        const Result r = s.run(std::format("run -GW=7 \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(r.out == "W=7 S=dflt\n");
    }
    SUBCASE("a string, with the separate-argument spelling") {
        const Result r = s.run(std::format("run -G W=9 '-GS=\"hi\"' \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(r.out == "W=9 S=hi\n");
    }
    SUBCASE("an override with no value is a command-line error") {
        const Result r = s.run(std::format("run -GW \"{}\"", design.string()));
        CHECK(r.code == 1);
        CHECK(contains(r.err, "is not a valid form of parameter override"));
    }
}

TEST_CASE("cli: --libmap assigns sources to libraries and -L orders the lookup") {
    const Scratch s;
    s.write("la/leaf.sv", "module leaf; initial $display(\"leaf from la\"); endmodule\n");
    s.write("lb/leaf.sv", "module leaf; initial $display(\"leaf from lb\"); endmodule\n");
    const fs::path top = s.write("t.sv", "module t; leaf u(); endmodule\n");
    const fs::path map =
      s.write("lib.map", std::format("library la {0}/la/*.sv;\nlibrary lb {0}/lb/*.sv;\n", s.dir().string()));
    SUBCASE("the first library listed wins by default") {
        const Result r = s.run(std::format("run --libmap \"{}\" \"{}\"", map.string(), top.string()));
        CHECK(r.code == 0);
        CHECK(r.out == "leaf from la\n");
    }
    SUBCASE("-L puts another library first") {
        const Result r = s.run(std::format("run --libmap \"{}\" -L lb \"{}\"", map.string(), top.string()));
        CHECK(r.code == 0);
        CHECK(r.out == "leaf from lb\n");
    }
}

TEST_CASE("cli: every message names the commands this build has and no others") {
    const Scratch                       s;
    const std::vector<std::string_view> built = built_commands(s);
    const Result                        none  = s.run("");
    CHECK(none.code == 2);
    const Result unknown = s.run("frobnicate");
    CHECK(unknown.code == 2);
    const Result help = s.run("--help");
    // `synth`, `lsp` and `dap` are opt-in; a build without one rejects it as an
    // unknown command and must not mention it anywhere.
    for (const std::string_view command : { "build", "run", "lint", "synth", "lsp", "dap" }) {
        CAPTURE(command);
        const bool has = std::ranges::find(built, command) != built.end();
        CHECK(contains(none.err, std::format("'{}'", command)) == has);
        CHECK(contains(unknown.err, std::format("'{}'", command)) == has);
        CHECK(contains(help.out, std::format("\n  {} ", command)) == has);
        if (!has) {
            const Result r = s.run(std::format("{} --help", command));
            CHECK(r.code == 2);
            CHECK(contains(r.err, std::format("unknown command '{}'", command)));
        }
    }
}
#endif // !defined(_WIN32)
