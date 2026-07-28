#pragma once

// Out-of-process execution of a design, for tests whose design only ends at
// $finish.
//
// Such a design must not be simulated in the test runner's own process. $finish
// longjmps out of the middle of the simulation (ir::run_jit installs the
// jmp_buf), abandoning live coroutine frames and leaving the runtime's globals
// part-way through whatever they were doing. Enough of those in one process and
// the heap is corrupted: the suite segfaults inside the allocator during a later,
// unrelated LLVM compile. So the simulation gets a process of its own, which
// $finish is then free to leave in any state.
//
// The mechanism is a re-execution of the test binary in the hidden mode
// test_main.cpp implements: the parent writes the source to a temp file, spawns
// itself against it with stdout redirected, and reads the result back. Not
// fork(): forking a process that has already built a JIT and its thread pool,
// then continuing to run code in the child without an exec, is unsupported on
// Darwin and unavailable on Windows.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace clotho::test {

// The argument that puts the test binary into single-design mode. Followed by
// the path of the source file to simulate.
inline constexpr std::string_view run_design_flag = "--clotho-run-design";

// Absolute path of the running test binary, captured by main() so a child can
// be spawned from any working directory.
inline std::string self_exe;

namespace detail {

// Temp-file stem unique to this process (so concurrent test runs cannot collide)
// and to each call within it.
inline std::string unique_stem() {
    static const uint64_t     salt = std::random_device{}();
    static std::atomic<uint64_t> seq{0};
    return "clotho-test-" + std::to_string(salt) + "-" + std::to_string(seq++);
}

inline std::string read_file(const std::filesystem::path& p) {
    std::ifstream     in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace detail

// Simulate `source` in a child process and return everything it wrote to stdout.
// stderr is left attached to the parent's, so diagnostics ($finish banners,
// severity reports) stay visible in the test log and out of the compared text.
inline std::string run_design_subprocess(std::string_view source) {
    namespace fs = std::filesystem;

    const std::string stem = detail::unique_stem();
    const fs::path    dir  = fs::temp_directory_path();
    const fs::path    src  = dir / (stem + ".sv");
    const fs::path    out  = dir / (stem + ".out");
    {
        std::ofstream f(src, std::ios::binary);
        f.write(source.data(), static_cast<std::streamsize>(source.size()));
    }

    const auto quote = [](const std::string& s) { return "\"" + s + "\""; };
    std::string cmd = quote(self_exe) + " " + std::string(run_design_flag) + " " +
                      quote(src.string()) + " > " + quote(out.string());
#if defined(_WIN32)
    // cmd.exe strips the outer pair of quotes from a command that both starts
    // and ends with one, so a command built entirely of quoted paths needs a
    // sacrificial pair around the whole thing.
    cmd = "\"" + cmd + "\"";
#endif

    std::fflush(stdout);
    const int rc = std::system(cmd.c_str());
    static_cast<void>(rc); // The design's own output is the assertion, not its status.

    std::string text = detail::read_file(out);
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(out, ec);
    return text;
}

} // namespace clotho::test
