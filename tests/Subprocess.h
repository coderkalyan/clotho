#pragma once

// Out-of-process execution of a design, for tests whose design is expected to
// end the process or whose exit status is the assertion.
//
// A runtime fatal ends the simulation with std::exit, so a test of one cannot run
// the design in the test runner's own process. Such a design gets a process of its
// own, whose exit status and stderr the test then reads back.
//
// TODO: some callers spawn only because their design ends in $finish. That was
// required while $finish longjmped out of live coroutine frames; it now returns
// cooperatively (docs/design/cooperative-finish.md), so those designs can run
// in-process like any other.
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

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace clotho::test {

// The argument that puts the test binary into single-design mode. Followed by
// the path of the source file to simulate.
inline constexpr std::string_view run_design_flag = "--clotho-run-design";

// Absolute path of the running test binary, captured by main() so a child can
// be spawned from any working directory.
inline std::string self_exe;

// Everything one design run produced. `status` is the raw std::system() return
// value; read an exit code out of it with design_exit_code.
struct DesignRun {
    std::string out;
    std::string err;
    int         status = -1;
};

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

// Spawn the test binary in single-design mode against `source`, always
// redirecting stdout to a temp file and stderr too when `capture_err`. `env` is
// prepended to the command as `NAME=value ` assignments for the child only.
inline DesignRun spawn_design(std::string_view source, bool capture_err, std::string_view env) {
    namespace fs = std::filesystem;

    const std::string stem = unique_stem();
    const fs::path    dir  = fs::temp_directory_path();
    const fs::path    src  = dir / (stem + ".sv");
    const fs::path    out  = dir / (stem + ".out");
    const fs::path    err  = dir / (stem + ".err");
    {
        std::ofstream f(src, std::ios::binary);
        f.write(source.data(), static_cast<std::streamsize>(source.size()));
    }

    const auto quote = [](const std::string& s) { return "\"" + s + "\""; };
    std::string cmd = std::string(env) + quote(self_exe) + " " + std::string(run_design_flag) +
                      " " + quote(src.string()) + " > " + quote(out.string());
    if (capture_err) {
        cmd += " 2> " + quote(err.string());
    }
#if defined(_WIN32)
    // cmd.exe strips the outer pair of quotes from a command that both starts
    // and ends with one, so a command built entirely of quoted paths needs a
    // sacrificial pair around the whole thing.
    cmd = "\"" + cmd + "\"";
#endif

    std::fflush(stdout);
    DesignRun run;
    run.status = std::system(cmd.c_str());
    // The design's own output is the assertion, not its exit status — a design
    // that ends in $finish(1) is expected to exit nonzero. But a spawn that
    // never ran (no shell, or a fork that failed under memory pressure) yields
    // empty output and would otherwise surface as an unhelpful text diff.
    if (run.status == -1 || run.status == 127) {
        std::fprintf(stderr, "run_design_subprocess: could not spawn '%s' (rc %d)\n",
                     cmd.c_str(), run.status);
    }

    run.out = read_file(out);
    std::error_code ec;
    fs::remove(src, ec);
    fs::remove(out, ec);
    if (capture_err) {
        run.err = read_file(err);
        fs::remove(err, ec);
    }
    return run;
}

} // namespace detail

// Simulate `source` in a child process and return everything it wrote to stdout.
// stderr is left attached to the parent's, so diagnostics ($finish banners,
// severity reports) stay visible in the test log and out of the compared text.
inline std::string run_design_subprocess(std::string_view source) {
    return detail::spawn_design(source, /*capture_err=*/false, /*env=*/{}).out;
}

// Simulate `source` in a child process, capturing both streams and its exit
// status. For a run whose payload IS the diagnostic — a fatal that ends the
// process rather than returning through the sim loop.
//
// `env` is a shell `NAME=value ` prefix applied to the child alone. The one use
// is a test of an allocation the system refuses: under AddressSanitizer the
// allocator aborts on an oversized request instead of returning null, so the
// runtime's own null check never runs unless the child asks for
// `allocator_may_return_null=1`.
inline DesignRun run_design_captured(std::string_view source, std::string_view env = {}) {
    return detail::spawn_design(source, /*capture_err=*/true, env);
}

// The child's exit code, or -1 when it did not exit normally. std::system
// returns a wait status on POSIX and the exit code itself on Windows.
inline int design_exit_code(const DesignRun& run) {
#if defined(_WIN32)
    return run.status;
#else
    return WIFEXITED(run.status) ? WEXITSTATUS(run.status) : -1;
#endif
}

} // namespace clotho::test
