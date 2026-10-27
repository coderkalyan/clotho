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
#include <regex>
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

// The first line of `--version` without its "clotho " prefix: the release and
// the commit, which is also what the servers report.
std::string version_string(const Scratch& s) {
    const Result r = s.run("--version");
    REQUIRE(r.code == 0);
    REQUIRE(r.out.starts_with("clotho "));
    return r.out.substr(7, r.out.find('\n') - 7);
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
    const Scratch     s;
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

#endif // !defined(_WIN32)
