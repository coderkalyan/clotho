// The `clotho` command line, driven as a process: usage errors, which commands
// accept which options, and the slang options the tool exposes (`-G`, `--libmap`).
//
// Exit status and stderr are the contract here, and only a real process shows
// them. A scope violation is reported before any source is read, so those cases
// pass a design that is never opened.

#include <doctest/doctest.h>

#include <algorithm>
#include <cerrno>
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
#include "ir/SimConfig.h"

#if !defined(_WIN32)

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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

    // Like run, from inside the scratch directory, for output written beside it.
    Result run_in(std::string_view args) const {
        const fs::path    out = path("stdout.txt");
        const fs::path    err = path("stderr.txt");
        const std::string cmd = std::format(
          "cd \"{}\" && \"{}\" {} > \"{}\" 2> \"{}\"", dir_.string(), CLOTHO_EXE, args, out.string(), err.string());
        const int status = std::system(cmd.c_str());
        Result    r;
        r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        r.out  = clotho::test::detail::read_file(out);
        r.err  = clotho::test::detail::read_file(err);
        return r;
    }

    // Like run, but stdout is a pseudo-terminal, so the process sees what it
    // sees when a person runs it in a shell, and `out` is what it wrote there
    // (with the terminal's CR LF line endings). stderr is still a file.
    Result run_on_terminal(std::string_view args) const {
        const int master = posix_openpt(O_RDWR | O_NOCTTY);
        REQUIRE(master >= 0);
        REQUIRE(grantpt(master) == 0);
        REQUIRE(unlockpt(master) == 0);
        const char* slave_name = ptsname(master);
        REQUIRE(slave_name != nullptr);
        const int slave = open(slave_name, O_RDWR | O_NOCTTY);
        REQUIRE(slave >= 0);

        const fs::path             err    = path("stderr-terminal.txt");
        const std::string          cmd    = std::format("\"{}\" {} 2> \"{}\"", CLOTHO_EXE, args, err.string());
        const char*                argv[] = { "sh", "-c", cmd.c_str(), nullptr };
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, slave);
        posix_spawn_file_actions_addclose(&actions, master);
        pid_t pid = 0;
        REQUIRE(posix_spawn(&pid, "/bin/sh", &actions, nullptr, const_cast<char* const*>(argv), environ) == 0);
        posix_spawn_file_actions_destroy(&actions);
        // With this process's copy closed, the master reads end-of-file (EIO on
        // Linux) when the child exits, after everything it wrote.
        close(slave);

        Result r;
        char   buffer[4096];
        for (;;) {
            const ssize_t n = read(master, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            r.out.append(buffer, static_cast<size_t>(n));
        }
        close(master);
        int status = 0;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

bool has_command(const Scratch& s, std::string_view command) {
    return s.run(std::format("{} --help", command)).code == 0;
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
            "--cpu",
            "-o",
            "-O",
            "--sanitize",
            "--coverage",
            "--gc-stats",
            "--dpi-lib",
            "--time",
            "--allow-system",
            "--loose-timescale",
            "-h",
            "--help-all",
            "--version" },
          { "--stop-time",
            "--loose-stale-ref",
            "--perf-jitdump",
            "--stat",
            "--gc-stress",
            "--gc-nursery",
            "--gc-heap-max",
            "--gc-full-every" } },
        { "run",
          "Usage: clotho run [options] <files>... [+plusarg...]\n",
          { "-O",
            "--sanitize",
            "--coverage",
            "--gc-stats",
            "--time",
            "--allow-system",
            "--loose-timescale",
            "--stop-time",
            "--dpi-lib",
            "-h",
            "--help-all",
            "--version" },
          { "--emit",
            "-o",
            "--stat",
            "--gc-stress",
            "--gc-nursery",
            "--gc-heap-max",
            "--gc-full-every",
            "--loose-stale-ref",
            "--perf-jitdump" } },
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
            "--stop-time",
            "--dpi-lib",
            "--loose-stale-ref",
            "--perf-jitdump" } },
    };
    for (const Case& c : cases) {
        CAPTURE(c.command);
        if (!has_command(s, c.command)) {
            continue;
        }
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
    CHECK_FALSE(contains(s.run("lint --help").out, "plusarg"));
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
    CHECK(contains(all.out, "[build|run] Library holding the C functions"));
    // Options for working on clotho are listed only here, under their own heading,
    // after Clotho's other options and before slang's.
    const size_t dev_at = all.out.find("Developer options (for working on clotho):\n");
    REQUIRE(dev_at != std::string::npos);
    const size_t source_at = all.out.find("Source options:\n", dev_at);
    REQUIRE(source_at != std::string::npos);
    const std::vector<std::string> developer = listed_options(all.out.substr(dev_at, source_at - dev_at));
    CHECK(developer == std::vector<std::string>{ "--stat",
                                                 "--gc-stress",
                                                 "--gc-nursery",
                                                 "--gc-heap-max",
                                                 "--gc-full-every",
                                                 "--loose-stale-ref",
                                                 "--perf-jitdump" });
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
    for (const std::string_view flag : { "--perf-jitdump", "--loose-stale-ref", "--stop-time=1ns" }) {
        CAPTURE(flag);
        const Result r = s.run(std::format("build {} \"{}\"", flag, design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "applies to 'run', not 'build'"));
    }
}

TEST_CASE("cli: build refuses the options its output cannot carry") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    SUBCASE("an executable has no stdout form") {
        const Result r = s.run(std::format("build -o - \"{}\"", design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "an executable cannot be written to stdout"));
    }
    SUBCASE("only an executable links DPI libraries in") {
        const Result r = s.run(std::format("build --emit obj --dpi-lib x.o -o - \"{}\"", design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "--dpi-lib applies to an executable"));
    }
    SUBCASE("only machine code has a processor") {
        const Result r = s.run(std::format("build --emit llvm --cpu native -o - \"{}\"", design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "--cpu applies to an executable or object"));
    }
    SUBCASE("a processor LLVM does not know") {
        const Result r = s.run(std::format("build --cpu not-a-cpu -o x \"{}\"", design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "unknown --cpu 'not-a-cpu'"));
    }
}

TEST_CASE("cli: build and run accept the code-generation options") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    SUBCASE("build") {
        const Result r = s.run(std::format(
          "build -O --sanitize=bounds --stat=dedup -o \"{}\" \"{}\"", s.path("sim").string(), design.string()));
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

TEST_CASE("cli: --stat=unit-code counts the functions emitted for the groups of a unit") {
    // Three instances of one block wait on the clock of one interface: their
    // groups are rows of one schedule group sharing one subscription, and each
    // of the unit's groups (that one and the instance's own PreInit) is one
    // function that all three instances run with their own record.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        interface bus_if; logic clk = 0; endinterface
        module leaf(bus_if b);
            reg [7:0] n = 0;
            always @(posedge b.clk) n <= n + 8'd1;
        endmodule
        module top;
            bus_if bus();
            leaf u0(bus);
            leaf u1(bus);
            leaf u2(bus);
            initial #1 bus.clk = 1;
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "2 groups of multi-instance units over 6 instances' groups, 2 functions emitted (3 of the groups are coroutines"));
    CHECK(contains(r.err, "1 schedule groups run several groups (3 rows)"));
    // Where a build verifies shared code (CLOTHO_EXTRA_ASSERTS), the line says what
    // it lowered again to compare: u1 and u2 each run both groups of the unit.
    if (clotho::ir::SimulationConfig{}.verify_shared_code) {
        CHECK(contains(r.err, "4 shared groups lowered again for their own instance and compared with the code they share"));
    }
}

TEST_CASE("cli: --stat=unit-code shows the groups of twin instances that use events shared") {
    // Each instance of leaf declares events, triggers them (`->`, `->>`), waits
    // on them (`@e`, `always @(e)`) and takes event ports bound to different
    // events of the parent: the unit's four groups (three processes and the
    // instance's own PreInit) are one function each, run by both instances.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module leaf(input event go, output event done);
            event e, f;
            int n = 0;
            always @(e) n++;
            initial forever begin @(f); n++; end
            initial forever begin @(go); -> e; ->> f; #1 -> done; end
        endmodule
        module top;
            event g0, g1, d0, d1;
            leaf u0(g0, d0);
            leaf u1(g1, d1);
            initial begin #1 -> g0; #1 -> g1; end
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "4 groups of multi-instance units over 8 instances' groups, 4 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code counts the functions and tasks twin instances share") {
    // Two interface instances and two modules driving them, one body each: the
    // interface's task and function are two functions however many instances
    // there are, and so is the driver's code, which calls them into the
    // record of the interface instance its port names.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        interface bus_if;
            int last;
            task automatic put(input int v);
                static int n = 100;
                n++;
                last = v + n;
            endtask
            function int get();
                return last;
            endfunction
        endinterface
        module drv (bus_if b, input int k);
            initial begin
                #1; #(k);
                b.put(10 * k);
                b.put(20 * k);
                $display("drv %0d last %0d get %0d", k, b.last, b.get());
            end
        endmodule
        module top;
            bus_if b0 ();
            bus_if b1 ();
            drv d0 (b0, 1);
            drv d1 (b1, 2);
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "2 groups of multi-instance units over 4 instances' groups, 2 functions emitted"));
    CHECK(contains(r.err, "2 subroutines of multi-instance units over 4 instances' subroutines, 2 functions emitted "
                          "(2 of the subroutines are tasks)"));
    CHECK(!contains(r.err, "lowered per instance"));
    if (clotho::ir::SimulationConfig{}.verify_shared_code) {
        CHECK(contains(r.err, "2 shared subroutines lowered again for their own instance and compared with the code "
                              "they share"));
    }
}

TEST_CASE("cli: --stat=unit-code shows twins calling into one instance sharing their groups") {
    // The driver's body names the monitor instance's task and function: a call
    // into another instance is its record, held in the driver's own, so the
    // callee being a function of one instance does not keep the twin drivers
    // apart.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module mon;
            int log_n;
            task automatic log(input int who);
                log_n += who;
                #1;
            endtask
            function int total();
                return log_n;
            endfunction
        endmodule
        module drv (input int id);
            initial begin
                #1;
                top.m.log(id);
                $display("drv %0d sees %0d", id, top.m.total());
            end
        endmodule
        module top;
            mon m ();
            drv d0 (.id(1));
            drv d1 (.id(2));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "1 groups of multi-instance units over 2 instances' groups, 1 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows the fold of a forceable net shared by twin instances") {
    // A forceable net folds its drivers in a function of its instance, which
    // twin instances used to keep their groups apart by.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module m (input logic [3:0] a, input logic [3:0] b, input logic [3:0] f);
            wire [3:0] w;
            assign w = a;
            assign w = b;
            initial begin
                #1 force w = f;
                #1 release w;
            end
        endmodule
        module top;
            m x (.a(4'd3), .b(4'bzzzz), .f(4'd9));
            m y (.a(4'd5), .b(4'd6), .f(4'd2));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "1 subroutines of multi-instance units over 2 instances' subroutines, 1 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows subroutines that read their instance's name shared") {
    // `hello` reads its instance's name (`%m`) from the instance's record, so it
    // is one function for both instances, and so is the code that calls it.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module leaf (input int k);
            int n;
            task automatic hello();
                $display("hello from %m %0d", n);
                n++;
            endtask
            function int twice(int x);
                return 2 * x + k;
            endfunction
            initial begin
                #1;
                hello();
                $display("%0d", twice(3));
            end
        endmodule
        module top;
            leaf u0 (.k(1));
            leaf u1 (.k(2));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "2 subroutines of multi-instance units over 4 instances' subroutines, 2 functions emitted "
                          "(2 of the subroutines are tasks)"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows the groups of twin instances with deferred assertions shared") {
    // The generation counter that drops a pending report is a field of the
    // instance's record, so the unit's two coroutine groups are one function each.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module chk(input int k);
            int x;
            initial begin : whole
                #1;
                x = 0;
                assert #0 (x == 1) else $error("k=%0d", k);
                if (k == 2) disable whole;
            end
            always @(k) assert #0 (x == 7) else $warning("w k=%0d", k);
        endmodule
        module top;
            chk c1(.k(1)); chk c2(.k(2)); chk c3(.k(3));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "2 groups of multi-instance units over 6 instances' groups, 2 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows twin instances with process control and random streams shared") {
    // A body names its process's control block and seeds its random stream from
    // the process index its instance's record holds, so twins share the group.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module m(input int k, output int unsigned a);
            process p;
            initial begin
                p = process::self();
                #k;
                a = $urandom;
                if (k == 1) p.kill();
            end
        endmodule
        module top;
            int unsigned x0, x1;
            m m0(.k(1), .a(x0));
            m m1(.k(2), .a(x1));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "1 groups of multi-instance units over 2 instances' groups, 1 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows twin program instances and their $exit task shared") {
    // The program instance an `initial` or a task ends is read from the instance's
    // record, so two instances of one program are one function each.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module top;
            p p0(.d(5));
            p p1(.d(20));
        endmodule
        program p(input int d);
            task automatic bail();
                $exit;
            endtask
            initial begin
                #d;
                bail();
            end
        endprogram
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "1 subroutines of multi-instance units over 2 instances' subroutines, 1 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows twin instances with remote disables and covergroups shared") {
    // The flat id of a disable target or a covergroup type the instance declares is
    // a field of its record, so the code that names one is the same in every instance.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module m(input int k);
            bit [1:0] v = 0;
            covergroup cg;
                cp: coverpoint v { bins b[] = {0,1,2,3}; }
            endgroup
            cg a = new;
            initial begin : P
                #10 a.sample();
            end
            initial begin #5 if (k == 1) disable m.P; end
            task automatic tk();
                #20 $display("%m task done");
            endtask
            initial begin
                fork tk(); join_none
                #6;
                if (k == 2) disable m.tk;
            end
        endmodule
        module top;
            m m0(.k(1));
            m m1(.k(2));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "4 groups of multi-instance units over 8 instances' groups, 4 functions emitted"));
    CHECK(contains(r.err, "3 subroutines of multi-instance units over 6 instances' subroutines, 3 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows twin instances that run $dumpvars sharing their code") {
    // `$dumpvars` registers the whole design from one function of root, so the body
    // that calls it names no instance's signals, events included.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module leaf(input int k);
            event e, f;
            initial begin
                #1;
                f = e;
                $dumpfile("OUT.vcd");
                if (k == 1) $dumpvars;
                #k -> e;
                #1 -> f;
            end
        endmodule
        module top;
            leaf u0(.k(1));
            leaf u1(.k(4));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "1 groups of multi-instance units over 2 instances' groups, 1 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows twin instances with mutually recursive functions sharing them") {
    // `even` and `odd` call each other: the cycle is one family, lowered once and
    // run by both instances with their own records.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module m(input int k);
            function automatic int even(int n);
                if (n == 0) return 1;
                return odd(n - 1);
            endfunction
            function automatic int odd(int n);
                if (n == 0) return 0;
                return even(n - 1);
            endfunction
            initial begin
                #1;
                $display("%m %0d %0d", even(k), odd(k));
            end
        endmodule
        module top;
            m a(.k(4));
            m b(.k(7));
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "2 subroutines of multi-instance units over 4 instances' subroutines, 2 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
}

TEST_CASE("cli: --stat=unit-code shows twins that stage nonblocking writes into other instances sharing") {
    // Each driver writes the interface instance its port names: the staged blocks of
    // the second instance name its own interface's signal, though the code is one.
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        interface bus;
            logic [7:0] d;
        endinterface
        module drv(bus b, input int k);
            initial begin
                #k;
                b.d <= 8'd10 + k[7:0];
            end
        endmodule
        module top;
            bus b0();
            bus b1();
            drv u0(b0, 1);
            drv u1(b1, 2);
        endmodule
    )");
    const Result r = s.run(std::format("build --emit=llvm --stat=unit-code -o /dev/null \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(contains(r.err, "1 groups of multi-instance units over 2 instances' groups, 1 functions emitted"));
    CHECK(!contains(r.err, "lowered per instance"));
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

// A legal construct Clotho refuses (a wait on a fork-captured automatic event),
// reported at 5:5 as an `unsupported-construct` error and by nothing else.
constexpr std::string_view kRefusedDesign = R"(module top;
  task automatic t;
    event e;
    fork begin #1 -> e; end join_none
    wait (e.triggered);
  endtask
  initial t();
endmodule
)";

constexpr std::string_view kEsc = "\x1b[";

TEST_CASE("cli: a refusal shows its source line and a caret, like a slang error") {
    const Scratch  s;
    const fs::path design = s.write("refused.sv", kRefusedDesign);
    const Result   r      = s.run(std::format("run \"{}\"", design.string()));
    CHECK(r.code == 1);
    CHECK(contains(r.err, "refused.sv:5:5: error: wait on the fork-captured automatic 'e'"));
    CHECK(contains(r.err, "[unsupported-construct]\n    wait (e.triggered);\n    ^"));

    SUBCASE("--diag-source=false leaves the headline alone") {
        const Result q = s.run(std::format("run --diag-source=false \"{}\"", design.string()));
        CHECK(q.code == 1);
        CHECK(contains(q.err, "refused.sv:5:5: error: "));
        CHECK_FALSE(contains(q.err, "wait (e.triggered);"));
    }
    SUBCASE("--diag-column=false drops the column from the location") {
        const Result q = s.run(std::format("run --diag-column=false \"{}\"", design.string()));
        CHECK(q.code == 1);
        CHECK(contains(q.err, "refused.sv:5: error: "));
        CHECK(contains(q.err, "wait (e.triggered);"));
    }
}

TEST_CASE("cli: design diagnostics are coloured only when asked for, on a pipe") {
    const Scratch  s;
    const fs::path refused = s.write("refused.sv", kRefusedDesign);
    const fs::path unused  = s.write("unused.sv", "module top;\n  int unused_var;\nendmodule\n");

    SUBCASE("a slang warning") {
        const Result plain = s.run(std::format("lint \"{}\"", unused.string()));
        CHECK(plain.code == 0);
        CHECK(contains(plain.err, "unused variable 'unused_var'"));
        CHECK_FALSE(contains(plain.err, kEsc));

        const Result colour = s.run(std::format("lint --color-diagnostics \"{}\"", unused.string()));
        CHECK(colour.code == 0);
        CHECK(contains(colour.err, "unused variable 'unused_var'"));
        CHECK(contains(colour.err, kEsc));
    }
    SUBCASE("a refusal, which Clotho raises itself") {
        const Result plain = s.run(std::format("lint \"{}\"", refused.string()));
        CHECK(plain.code == 1);
        CHECK_FALSE(contains(plain.err, kEsc));

        const Result colour = s.run(std::format("lint --color-diagnostics \"{}\"", refused.string()));
        CHECK(colour.code == 1);
        CHECK(contains(colour.err, kEsc));
        // The text under the colour is the plain text, nothing added or lost.
        CHECK(contains(colour.err, "wait on the fork-captured"));
        CHECK(contains(colour.err, "wait (e.triggered);"));
    }
    SUBCASE("--color-diagnostics=false is not colour either") {
        const Result off = s.run(std::format("lint --color-diagnostics=false \"{}\"", unused.string()));
        CHECK_FALSE(contains(off.err, kEsc));
    }
}

TEST_CASE("cli: a missing source file is reported once") {
    const Scratch s;
    const Result  r = s.run("run missing.sv");
    CHECK(r.code == 1);
    CHECK(contains(r.err, "error: 'missing.sv': No such file or directory"));
    CHECK_FALSE(contains(r.err, "bad-command-line"));
    CHECK_FALSE(contains(r.err, "command-line or source-loading error"));

    SUBCASE("lint keeps the exit status too") {
        const Result l = s.run("lint missing.sv");
        CHECK(l.code == 1);
        CHECK_FALSE(contains(l.err, "bad-command-line"));
    }
}

TEST_CASE("cli: an invalid --top is not followed by a missing-top warning") {
    const Scratch  s;
    const fs::path design = s.write("two.sv", "module a; endmodule\nmodule b; endmodule\n");
    const Result   r      = s.run(std::format("run \"{}\" --top c", design.string()));
    CHECK(r.code == 1);
    CHECK(contains(r.err, "error: 'c' is not a valid top-level module"));
    CHECK_FALSE(contains(r.err, "missing-top"));
    CHECK_FALSE(contains(r.err, "no top-level modules found"));
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

TEST_CASE("cli: an unknown command suggests the command it is closest to") {
    const Scratch s;
    struct Case {
        std::string_view typed;
        std::string_view suggested;
    };
    const Case cases[] = {
        { "rnu", "run" },  { "rn", "run" },  { "biuld", "build" },  { "bild", "build" },
        { "lnt", "lint" }, { "Run", "run" }, { "buildd", "build" },
    };
    for (const Case& c : cases) {
        CAPTURE(c.typed);
        const Result r = s.run(c.typed);
        CHECK(r.code == 2);
        CHECK(contains(r.err, std::format("unknown command '{}'; did you mean '{}'?", c.typed, c.suggested)));
        CHECK(contains(r.err, "try 'clotho --help'"));
    }
    SUBCASE("a word near no command gets the list instead") {
        const Result r = s.run("frobnicate");
        CHECK(r.code == 2);
        CHECK(contains(r.err, "unknown command 'frobnicate'; expected 'build', 'run'"));
        CHECK_FALSE(contains(r.err, "did you mean"));
    }
    SUBCASE("a command this build lacks is never suggested") {
        // `lsp` and `dap` are a keystroke from each other and from `lint` or `run`
        // only if the build has them.
        const std::vector<std::string_view> built = built_commands(s);
        for (const std::string_view command : { "synth", "lsp", "dap" }) {
            CAPTURE(command);
            if (std::ranges::find(built, command) == built.end()) {
                CHECK_FALSE(contains(s.run("synt").err, std::format("'{}'", command)));
                CHECK_FALSE(contains(s.run("lps").err, std::format("did you mean '{}'", command)));
                CHECK_FALSE(contains(s.run("dpa").err, std::format("did you mean '{}'", command)));
            }
        }
    }
}

TEST_CASE("cli: a source file given where the command goes suggests run") {
    const Scratch  s;
    const fs::path file = s.write("design.txt", kParamDesign);
    SUBCASE("a file that exists, whatever it is called") {
        const Result r = s.run(std::format("\"{}\"", file.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, std::format("unknown command '{0}'; did you mean 'clotho run {0}'?", file.string())));
    }
    SUBCASE("a name that looks like source, existing or not") {
        for (const std::string_view name : { "missing.sv", "missing.v", "missing.svh", "missing.vh" }) {
            CAPTURE(name);
            const Result r = s.run(name);
            CHECK(r.code == 2);
            CHECK(contains(r.err, std::format("unknown command '{0}'; did you mean 'clotho run {0}'?", name)));
        }
    }
    SUBCASE("a file and the options after it are not run") {
        const Result r = s.run(std::format("\"{}\" --time", file.string()));
        CHECK(r.code == 2);
        CHECK(r.out.empty());
    }
}

TEST_CASE("cli: build writes an executable named after the top module by default") {
    const Scratch s;
    s.write("design.sv", kParamDesign);
    const Result built = s.run_in("build design.sv");
    CHECK(built.code == 0);
    CHECK(built.out.empty());
    REQUIRE(fs::exists(s.path("top")));
    const Result simulated = s.run_in("build -o sim design.sv");
    CHECK(simulated.code == 0);
    const std::string cmd = std::format("\"{}\" > \"{}\"", s.path("sim").string(), s.path("sim.txt").string());
    CHECK(std::system(cmd.c_str()) == 0);
    CHECK(clotho::test::detail::read_file(s.path("sim.txt")) == "W=4 S=dflt\n");
}

TEST_CASE("cli: build will not write text or an object to a terminal by default") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    SUBCASE("without -o it is a usage error that says how to get the output") {
        for (const std::string_view emit : { "obj", "llvm", "llvm-raw", "ir" }) {
            CAPTURE(emit);
            const Result r = s.run_on_terminal(std::format("build --emit={} \"{}\"", emit, design.string()));
            CHECK(r.code == 2);
            CHECK(r.out.empty());
            CHECK(contains(r.err, "'build' writes this to stdout by default, and stdout is a terminal"));
            CHECK(contains(r.err, "-o <file>"));
            CHECK(contains(r.err, "-o -"));
        }
    }
    SUBCASE("-o - asks for the terminal by name") {
        const Result r = s.run_on_terminal(std::format("build --emit=llvm -o - \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(contains(r.out, "ModuleID"));
    }
    SUBCASE("-o <file> writes the file and leaves the terminal alone") {
        const fs::path out = s.path("design.ll");
        const Result   r =
          s.run_on_terminal(std::format("build --emit=llvm -o \"{}\" \"{}\"", out.string(), design.string()));
        CHECK(r.code == 0);
        CHECK(r.out.empty());
        CHECK(contains(clotho::test::detail::read_file(out), "ModuleID"));
    }
    SUBCASE("only build is refused: run and lint still use the terminal") {
        const Result run = s.run_on_terminal(std::format("run \"{}\"", design.string()));
        CHECK(run.code == 0);
        CHECK(contains(run.out, "W=4 S=dflt"));
        CHECK(s.run_on_terminal(std::format("lint \"{}\"", design.string())).code == 0);
    }
}

TEST_CASE("cli: build still writes text to a pipe or a file without -o") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    SUBCASE("a pipe") {
        const Result r = s.run(std::format("build --emit=llvm \"{}\" | cat", design.string()));
        CHECK(r.code == 0);
        CHECK(contains(r.out, "ModuleID"));
    }
    SUBCASE("a file") {
        const Result r = s.run(std::format("build --emit=llvm \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(contains(r.out, "ModuleID"));
    }
    SUBCASE("an explicit -o - into a file") {
        const Result r = s.run(std::format("build --emit=llvm -o - \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(contains(r.out, "ModuleID"));
    }
}

TEST_CASE("cli: build --help explains where its output goes") {
    const Scratch s;
    Result r = s.run("build --help");
    // The note is wrapped; compare it as one line.
    std::ranges::replace(r.out, '\n', ' ');
    CHECK(contains(r.out, "named after the design's first top-level module"));
    CHECK(contains(r.out, "unless that is a terminal; pass -o - to print it there anyway"));
}

TEST_CASE("cli: --path-prefix-map changes the source paths a simulation prints") {
    const Scratch s;
    s.write("rtl/t.sv", "module top;\n  initial begin\n    $error(\"boom\");\n    $finish;\n  end\nendmodule\n");
    const std::string dir = s.path("rtl").string();
    SUBCASE("run, and the longest matching directory wins") {
        const Result r = s.run(std::format("run --path-prefix-map \"{}=wide\" --path-prefix-map \"{}=narrow\" \"{}\"",
                                           s.dir().string(),
                                           dir,
                                           s.path("rtl/t.sv").string()));
        CHECK(r.code == 0);
        CHECK(contains(r.err, "ERROR: narrow/t.sv:3: top at 0 s: boom"));
        CHECK(contains(r.err, "narrow/t.sv:4: $finish called at 0 s"));
    }
    SUBCASE("an executable keeps the paths build was told to show") {
        CHECK(s.run_in(std::format("build --path-prefix-map \"{}=proj\" -o sim rtl/t.sv", s.dir().string())).code == 0);
        const std::string cmd =
          std::format("cd / && \"{}\" 2> \"{}\"", s.path("sim").string(), s.path("e.txt").string());
        CHECK(std::system(cmd.c_str()) == 0);
        CHECK(contains(clotho::test::detail::read_file(s.path("e.txt")), "ERROR: proj/rtl/t.sv:3: top at 0 s: boom"));
    }
    SUBCASE("a value without = is a usage error") {
        const Result r = s.run(std::format("run --path-prefix-map nothing \"{}\"", s.path("rtl/t.sv").string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, "invalid --path-prefix-map 'nothing'"));
    }
}

TEST_CASE("cli: --stop-time takes an integer and a unit") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    SUBCASE("every unit, in either spelling of a valued option") {
        for (const std::string_view time : { "1fs", "1ps", "100ns", "5us", "7ms", "2s", "18446s" }) {
            CAPTURE(time);
            const Result r = s.run(std::format("run --stop-time={} \"{}\"", time, design.string()));
            CHECK(r.code == 0);
            CHECK(r.out == "W=4 S=dflt\n");
        }
        const Result r = s.run(std::format("run --stop-time 100ns \"{}\"", design.string()));
        CHECK(r.code == 0);
    }
    SUBCASE("a malformed time is a usage error before any source is read") {
        struct Case {
            std::string_view time;
            std::string_view message;
        };
        const Case cases[] = {
            { "100", "invalid --stop-time '100': the unit is missing; write e.g. 100ns" },
            { "100xs", "unknown unit 'xs' (units: fs, ps, ns, us, ms or s)" },
            { "100NS", "unknown unit 'NS'" },
            { "1.5ns", "unknown unit '.5ns'" },
            { "'100 ns'", "unknown unit ' ns'" },
            { "ns", "write an integer and a unit, e.g. 100ns or 5us" },
            { "-5ns", "write an integer and a unit" },
            { "+5ns", "write an integer and a unit" },
            { "0ns", "invalid --stop-time '0ns': must be greater than zero" },
            { "0s", "must be greater than zero" },
            { "18447s", "too large; the longest time is about 18446 s" },
            { "18446744073709551ns", "too large" },
            { "99999999999999999999999ns", "too large" },
        };
        for (const Case& c : cases) {
            CAPTURE(c.time);
            // A design that does not exist: the error must come first.
            const Result r = s.run(std::format("run --stop-time={} missing.sv", c.time));
            CHECK(r.code == 2);
            CHECK(contains(r.err, c.message));
            CHECK(r.out.empty());
        }
    }
}

TEST_CASE("cli: --stop-time is refused where nothing simulates") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", kParamDesign);
    for (const std::string_view command : built_commands(s)) {
        if (command == "run" || command == "lsp" || command == "dap") {
            continue;
        }
        CAPTURE(command);
        const Result r = s.run(std::format("{} --stop-time=100ns \"{}\"", command, design.string()));
        CHECK(r.code == 2);
        CHECK(contains(r.err, std::format("--stop-time applies to 'run', not '{}'", command)));
    }
}

// How many specializations `--stat=units` reports as still lowering differently in
// their instances (docs/design/modular-compilation.md, step 2c), or -1 when the
// report is missing.
long split_specializations(const std::string& report) {
    const std::string tail = " specializations still split across instances";
    const size_t      end  = report.find(tail);
    if (end == std::string::npos) {
        return -1;
    }
    const size_t start = report.rfind(' ', end - 1) + 1;
    return std::strtol(report.c_str() + start, nullptr, 10);
}

// The instances of one module differ only in where they sit, so they are one unit:
// the hidden storage a process body needs (a repeat's bound, a task call's output
// argument) is numbered within its instance, never across the design.
TEST_CASE("cli: instances of one module share a unit whatever hidden temporaries their bodies need") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module leaf;
          int n = 0;
          task automatic bump(output int o);
            o = n + 1;
          endtask
          initial begin
            int r;
            repeat (3) n++;
            bump(r);
            $display("%m n=%0d r=%0d", n, r);
          end
        endmodule
        module top;
          leaf u1();
          leaf u2();
        endmodule
    )");
    const Result   r      = s.run(std::format("run --stat=units \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(r.out == "top.u1 n=3 r=4\ntop.u2 n=3 r=4\n");
    CHECK(split_specializations(r.err) == 0);
}

// An instance's name is data the body reads, not text in it: %m prints the path, a
// report names its instance by it, and the instances of a module that does both,
// with an assertion of each kind in it, still lower alike.
TEST_CASE("cli: instances of one module share a unit whatever names they print or report") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        module leaf(input logic clk);
          logic a = 0, b = 0;
          initial begin
            $display("%m");
            $error("plain");
            assert (a) else $error("checked");
          end
          ap: assert property (@(posedge clk) a |=> b);
        endmodule
        module top;
          logic clk = 0;
          leaf u1(clk);
          leaf u2(clk);
        endmodule
    )");
    const Result   r      = s.run(std::format("run --stat=units \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(r.out == "top.u1\ntop.u2\n");
    CHECK(contains(r.err, "top.u1 at 0 s: plain"));
    CHECK(contains(r.err, "top.u2 at 0 s: plain"));
    CHECK(contains(r.err, "top.u1 at 0 s: checked"));
    CHECK(contains(r.err, "top.u2 at 0 s: checked"));
    CHECK(split_specializations(r.err) == 0);
}

// A covergroup is built per instance, but its type says nothing of the instance:
// the prefix the report names the instance by is read when the group is built, so
// the instances of a module that builds one still share a unit.
TEST_CASE("cli: instances of one module share a unit when each builds a covergroup") {
    const Scratch  s;
    const fs::path report = s.path("coverage.txt");
    const auto     run    = [&](std::string_view module_body) {
        const fs::path design = s.write("design.sv", std::format(R"(
            module leaf;
              bit [1:0] v = 0;
              covergroup cg; coverpoint v; endgroup
              {}
            endmodule
            module top;
              leaf u1();
              leaf u2();
            endmodule
        )",
                                                                 module_body));
        return s.run(std::format("run --stat=units --coverage \"{}\" \"{}\"", report.string(), design.string()));
    };
    const auto named = [&](std::string_view instance) {
        return contains(clotho::test::detail::read_file(report), std::format("instance {} ", instance));
    };
    SUBCASE("built by a procedure") {
        const Result r = run("cg c; initial begin c = new; c.sample(); end");
        CHECK(r.code == 0);
        CHECK(split_specializations(r.err) == 0);
        CHECK(named("top.u1.cg"));
        CHECK(named("top.u2.cg"));
    }
    SUBCASE("built by a declaration initializer") {
        const Result r = run("cg c = new; initial c.sample();");
        CHECK(r.code == 0);
        CHECK(split_specializations(r.err) == 0);
        CHECK(named("top.u1.cg"));
        CHECK(named("top.u2.cg"));
    }
}

// A declaration initializer is part of its own instance, so a covergroup it builds
// (with a coverage event, whose process lives in that instance too) and the
// instance's %m, assertion banner and covergroup name all read that instance's own
// path, with the instances still sharing one unit. Expected output is main's.
TEST_CASE("cli: per-instance initializers keep %m, assertion banners and covergroup names per instance") {
    const Scratch  s;
    const fs::path report = s.path("coverage.txt");
    const fs::path design = s.write("design.sv", R"(
        module leaf(input bit clk);
          bit [1:0] v = 0;
          covergroup cg @(posedge clk); coverpoint v; endgroup
          cg c = new;
          always @(negedge clk) v <= v + 1;
          a: assert property (@(posedge clk) v != 2'd3) else $error("v hit 3");
          initial $display("%m up");
        endmodule
        module top;
          bit clk = 0;
          leaf u1(clk);
          leaf u2(clk);
          initial begin repeat (10) #1 clk = ~clk; $finish; end
        endmodule
    )");
    const Result   r = s.run(
      std::format("run --stat=units --coverage \"{}\" \"{}\"", report.string(), design.string()));
    CHECK(r.code == 0);
    CHECK(split_specializations(r.err) == 0);
    CHECK(contains(r.out, "top.u1 up\n"));
    CHECK(contains(r.out, "top.u2 up\n"));
    CHECK(contains(r.err, "ERROR: "));
    CHECK(contains(r.err, ": top.u1.a at 7 s: v hit 3\n"));
    CHECK(contains(r.err, ": top.u2.a at 7 s: v hit 3\n"));
    const std::string cov = clotho::test::detail::read_file(report);
    CHECK(contains(cov, "instance top.u1.cg  coverage 100.00%"));
    CHECK(contains(cov, "instance top.u2.cg  coverage 100.00%"));
}

// Declaration initializers run in the order the elaboration walks the hierarchy
// (LRM 6.8 leaves it open; this is the order designs have always seen): a parent's
// initializers declared before a child instance first, the child's next, the
// parent's later ones after, whichever instance an initializer reads. Each
// instance's initializers are part of that instance, so the two `mid` instances
// still share one unit.
TEST_CASE("cli: declaration initializers run in elaboration order across instances") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(
        package p;
          int n = 0;
          function int next(string who); $display("init %s n=%0d", who, n); n++; return n; endfunction
        endpackage
        module leaf; int id = p::next($sformatf("%m")); endmodule
        module mid; int x = p::next("mid.x"); leaf l1(); int y = p::next("mid.y"); leaf l2(); endmodule
        module top;
          int t0 = p::next("top.t0"); mid m1(); int t1 = p::next("top.t1"); mid m2();
          int early = m1.l1.id + 100;
          int late  = m2.l2.id + 100;
          initial $display("early=%0d late=%0d", early, late);
        endmodule
    )");
    const Result   r = s.run(std::format("run --stat=units \"{}\"", design.string()));
    CHECK(r.code == 0);
    CHECK(split_specializations(r.err) == 0);
    CHECK(r.out == "init top.t0 n=0\ninit mid.x n=1\ninit top.m1.l1 n=2\ninit mid.y n=3\ninit top.m1.l2 n=4\n"
                   "init top.t1 n=5\ninit mid.x n=6\ninit top.m2.l1 n=7\ninit mid.y n=8\ninit top.m2.l2 n=9\n"
                   "early=103 late=110\n");
}

// A covergroup type a package declares belongs to no instance: every instance that
// builds it refers to the package's type, so the instances of a module that does
// still share one unit, and the report names each by the instance that built it.
TEST_CASE("cli: instances of one module share a unit when each builds a package's covergroup") {
    const Scratch  s;
    const fs::path report = s.path("coverage.txt");
    const fs::path design = s.write("design.sv", R"(
        package p;
          bit [1:0] pv = 0;
          covergroup pcg; coverpoint pv; endgroup
        endpackage
        module leaf(input bit [1:0] hit);
          import p::*;
          pcg lc = new;
          initial begin #1 pv = hit; lc.sample(); end
        endmodule
        module top;
          leaf u1(2'd1);
          leaf u2(2'd2);
        endmodule
    )");
    const Result   r = s.run(std::format("run --stat=units --coverage \"{}\" \"{}\"", report.string(), design.string()));
    CHECK(r.code == 0);
    CHECK(split_specializations(r.err) == 0);
    const std::string cov = clotho::test::detail::read_file(report);
    CHECK(contains(cov, "instance top.u1.pcg "));
    CHECK(contains(cov, "instance top.u2.pcg_1 "));
}

TEST_CASE("cli: --sanitize=divzero reports a 2-state divide by zero and leaves the result alone") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(module m;
  int a, b, q, r;
  integer ia, ib;
  initial begin
    a = 7; b = 0;
    q = a / b;
    r = a % b;
    $display("%0d %0d", q, r);
    ia = 7; ib = 0;
    $display("%0d", ia / ib);
    b = 2;
    $display("%0d", a / b);
  end
endmodule
)");
    SUBCASE("on: a located report for each of the two operations, the same output") {
        const Result r = s.run(std::format("run --sanitize=divzero \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(r.out == "0 0\nx\n3\n");
        CHECK(contains(r.err, "warning: division by zero in a 2-state operation"));
        CHECK(contains(r.err, "design.sv:6:"));
        CHECK(contains(r.err, "design.sv:7:"));
        // The 4-state divide shows its x in the result; it is not reported.
        CHECK_FALSE(contains(r.err, "design.sv:10:"));
        CHECK_FALSE(contains(r.err, "design.sv:12:"));
    }
    SUBCASE("off: the same output and no report") {
        const Result r = s.run(std::format("run \"{}\"", design.string()));
        CHECK(r.code == 0);
        CHECK(r.out == "0 0\nx\n3\n");
        CHECK_FALSE(contains(r.err, "division by zero"));
    }
    SUBCASE("off: no code for it; on: a call to the report") {
        const Result off = s.run(std::format("build --emit=llvm -o - \"{}\"", design.string()));
        const Result on  = s.run(std::format("build --emit=llvm --sanitize=divzero -o - \"{}\"", design.string()));
        CHECK(off.code == 0);
        CHECK(on.code == 0);
        CHECK_FALSE(contains(off.out, "rt_divzero_report"));
        CHECK(contains(on.out, "rt_divzero_report"));
    }
}

// --gc-heap-max is a ceiling the run enforces: objects the design still holds after
// an emergency collection stop it at the allocation that does not fit, and the report
// names that statement. The same design without the ceiling completes.
TEST_CASE("cli: --gc-heap-max stops a design whose live objects do not fit, at the allocation") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", R"(module top;
  class C; int a; int b; endclass
  C q[$];
  initial begin
    for (int i = 0; i < 1000000; i++) begin
      automatic C x = new();
      q.push_back(x);
    end
    $display("done %0d", q.size());
  end
endmodule
)");
    const Result   capped = s.run(std::format("run --gc-heap-max=8388608 \"{}\"", design.string()));
    CHECK(capped.code == 1);
    CHECK(capped.out.empty());
    CHECK(contains(capped.err, "fatal: the class heap would pass --gc-heap-max (8388608 bytes)"));
    CHECK(contains(capped.err, "design.sv:6"));
    const Result free_run = s.run(std::format("run \"{}\"", design.string()));
    CHECK(free_run.code == 0);
    CHECK(free_run.out == "done 1000000\n");
}
#endif // !defined(_WIN32)
