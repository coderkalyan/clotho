// `clotho build` against `clotho run`: an executable must simulate exactly as
// the JIT does. For every example and SVA fixture, at both optimization tiers,
// the executable's stdout and exit status equal run's, and run's stderr is what
// build printed while compiling followed by what the executable printed.
//
// Every process runs from this test's working directory: both print source
// locations relative to the directory the design was compiled in.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "Subprocess.h"

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

struct Output {
    int         code = -1;
    std::string out;
    std::string err;
};

// Run `command` (already quoted) with its streams captured in `dir`.
Output run(const std::string& command, const fs::path& dir) {
    const fs::path    out  = dir / "stdout.txt";
    const fs::path    err  = dir / "stderr.txt";
    const std::string full = std::format("{} > \"{}\" 2> \"{}\"", command, out.string(), err.string());
#if defined(_WIN32)
    // cmd.exe strips one pair of quotes around the whole line.
    const int status = std::system(("\"" + full + "\"").c_str());
    const int code   = status;
#else
    const int status = std::system(full.c_str());
    const int code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return { code, clotho::test::detail::read_file(out), clotho::test::detail::read_file(err) };
}

std::vector<fs::path> corpus() {
    const fs::path        tests = CLOTHO_TEST_DIR;
    std::vector<fs::path> designs;
    for (const fs::path& dir : { tests / ".." / "examples", tests / "sva" }) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            const fs::path&   path = entry.path();
            const std::string name = path.filename().string();
            // The benchmarks run for seconds each and exercise nothing the rest do not.
            if ((path.extension() == ".v" || path.extension() == ".sv") && name.find("bench") == std::string::npos) {
                designs.push_back(path);
            }
        }
    }
    std::ranges::sort(designs);
    return designs;
}

void check_tier(std::string_view flags) {
    const fs::path dir = fs::temp_directory_path() / clotho::test::detail::unique_stem();
    fs::create_directories(dir);
#if defined(_WIN32)
    const fs::path exe = dir / "design.exe";
#else
    const fs::path exe = dir / "design";
#endif
    const std::vector<fs::path> designs = corpus();
    REQUIRE(designs.size() > 40);
    for (const fs::path& design : designs) {
        CAPTURE(design.string());
        const Output simulated = run(std::format("\"{}\" run {} \"{}\"", CLOTHO_EXE, flags, design.string()), dir);
        const Output built =
          run(std::format("\"{}\" build {} -o \"{}\" \"{}\"", CLOTHO_EXE, flags, exe.string(), design.string()), dir);
        REQUIRE(built.code == 0);
        CHECK(built.out.empty());
        const Output executed = run(std::format("\"{}\"", exe.string()), dir);
        CHECK(executed.code == simulated.code);
        CHECK(executed.out == simulated.out);
        CHECK(built.err + executed.err == simulated.err);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

TEST_CASE("executable: simulates as run does") {
    check_tier("");
}

TEST_CASE("executable: simulates as run does, optimized and bounds-checked") {
    check_tier("-O --sanitize=bounds");
}
