// The `clotho` command line, driven as a process: usage errors, which commands
// accept which options, and the slang options the tool exposes (`-G`, `--libmap`).
//
// Exit status and stderr are the contract here, and only a real process shows
// them. A scope violation is reported before any source is read, so those cases
// pass a design that is never opened.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

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
    Scratch() : dir_(fs::temp_directory_path() / clotho::test::detail::unique_stem()) {
        fs::create_directories(dir_);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    Scratch(const Scratch&)            = delete;
    Scratch& operator=(const Scratch&) = delete;

    const fs::path& dir() const { return dir_; }

    fs::path path(std::string_view name) const { return dir_ / name; }

    fs::path write(std::string_view name, std::string_view text) const {
        const fs::path p = path(name);
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        return p;
    }

    // Runs `clotho <args>` with `args` already shell-quoted.
    Result run(std::string_view args) const {
        const fs::path out = path("stdout.txt");
        const fs::path err = path("stderr.txt");
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

} // namespace cli_test

using namespace cli_test;

TEST_CASE("cli: --help shows the placeholder of every valued gc option") {
    const Scratch s;
    const Result  r = s.run("--help");
    CHECK(r.code == 0);
    CHECK(contains(r.out, "--gc-nursery <bytes>"));
    CHECK(contains(r.out, "--gc-heap-max <bytes>"));
    CHECK(contains(r.out, "--gc-full-every <n>"));
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
    const fs::path map = s.write(
      "lib.map", std::format("library la {0}/la/*.sv;\nlibrary lb {0}/lb/*.sv;\n", s.dir().string()));
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

#endif // !defined(_WIN32)
