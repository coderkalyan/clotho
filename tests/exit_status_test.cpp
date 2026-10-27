// How a run ends, as the process that started it sees it: the exit status after a
// `$fatal` (LRM 20.10), and what Ctrl-C does to a running simulation.
//
// The in-process cases read the status run_jit returns, which is what the driver
// hands to exit(). The process cases run `clotho run` for real, because a signal
// and an exit status are only observable from outside the process they are about.

#include <doctest/doctest.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// A DPI-C import the design below calls to interrupt itself. raise() runs the
// handler in the calling thread before it returns, so the interrupt lands at a
// known point of the run instead of at whatever instant a second process picked.
extern "C" void exit_status_test_raise_sigint(void) {
    std::raise(SIGINT);
}

namespace {

struct Run {
    std::string out;
    std::string err;
    int         status = -1;
};

std::string drain(std::FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string s;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    return s;
}

Run run(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* out_tmp = std::tmpfile();
    std::FILE* err_tmp = std::tmpfile();
    REQUIRE(out_tmp != nullptr);
    REQUIRE(err_tmp != nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(out_tmp), fileno(stdout));
    dup2(fileno(err_tmp), fileno(stderr));

    Run result;
    result.status = ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    result.out = drain(out_tmp);
    result.err = drain(err_tmp);
    std::fclose(out_tmp);
    std::fclose(err_tmp);
    return result;
}

// A design whose only statement is `body`, in a module with a final block that
// proves whether the finals ran.
std::string with_body(std::string_view body) {
    return "module m;\n"
           "  initial begin\n    " +
           std::string(body) +
           "\n  end\n"
           "  final $display(\"final ran\");\n"
           "endmodule\n";
}

} // namespace

// -- $fatal: LRM 20.10 "terminates the simulation with an error code" --

TEST_CASE("exit status: $fatal ends the run with status 1, whatever its finish number") {
    for (const char* const call : { "$fatal;", "$fatal(0, \"x\");", "$fatal(1, \"x\");", "$fatal(2, \"x\");" }) {
        CAPTURE(call);
        CHECK(run(with_body(call)).status == 1);
    }
}

TEST_CASE("exit status: a $fatal still runs the final blocks, as iverilog does") {
    const Run r = run(with_body("$fatal(1, \"stop\");"));
    CHECK(r.out == "final ran\n");
    CHECK(r.status == 1);
}

TEST_CASE("exit status: only $fatal changes it") {
    for (const char* const body : {
           "$finish;",
           "$finish(0);",
           "$finish(1);",
           "$finish(2);",
           "$stop;",
           "$info(\"i\");",
           "$warning(\"w\");",
           "$error(\"e\");",
           "assert (0);",
           "assert (0) else $error(\"custom\");",
           "$display(\"nothing at all\");",
         }) {
        CAPTURE(body);
        CHECK(run(with_body(body)).status == 0);
    }
}

TEST_CASE("exit status: an earlier $fatal's status does not leak into the next run") {
    CHECK(run(with_body("$fatal;")).status == 1);
    CHECK(run(with_body("$display(\"ok\");")).status == 0);
}

// The simulator's own run-time fatals are reported and ended through the same
// severity + finish pair `$fatal` is, so they carry its error code too.
TEST_CASE("exit status: a failed $cast task, a run-time fatal of the design, ends with status 1") {
    const Run r = run("class A; endclass\n"
                      "class B extends A; endclass\n"
                      "class C extends A; endclass\n"
                      "module m;\n"
                      "  A a; B b; C c;\n"
                      "  initial begin\n"
                      "    c = new;\n"
                      "    a = c;\n"
                      "    $cast(b, a);\n"
                      "    $display(\"unreachable\");\n"
                      "  end\n"
                      "  final $display(\"final ran\");\n"
                      "endmodule\n");
    CHECK(r.err.find("FATAL: source:9: m at 0 s: $cast: the source is not assignment compatible") !=
          std::string::npos);
    CHECK(r.out == "final ran\n");
    CHECK(r.status == 1);
}

// -- SIGINT, in this process --

TEST_CASE("sigint: an interrupt ends the run at the next time step, with no finals and status 130") {
    const Run r = run("`timescale 1ns/1ns\n"
                      "module m;\n"
                      "  import \"DPI-C\" function void exit_status_test_raise_sigint();\n"
                      "  int n = 0;\n"
                      "  always #1 n++;\n"
                      "  initial #5 exit_status_test_raise_sigint();\n"
                      "  final $display(\"final ran, n=%0d\", n);\n"
                      "endmodule\n");
    // The interrupt arrives while the step at 5 ns is running. That step finishes,
    // so the time it is reported at is the step's own.
    CHECK(r.err == "clotho: interrupted at 5 ns\n");
    CHECK(r.out.empty());
    CHECK(r.status == 130);
}

TEST_CASE("sigint: the previous SIGINT handler is back once the run ends") {
#if defined(_WIN32)
    using Handler = void (*)(int);
    const Handler before = std::signal(SIGINT, SIG_DFL);
    std::signal(SIGINT, before);
    run(with_body("$display(\"x\");"));
    const Handler after = std::signal(SIGINT, SIG_DFL);
    std::signal(SIGINT, after);
    CHECK(after == before);
#else
    // The test runner's own SIGINT handling (doctest installs one while a case
    // runs) is what must survive a simulation, not the default.
    struct sigaction before = {};
    struct sigaction after  = {};
    REQUIRE(sigaction(SIGINT, nullptr, &before) == 0);
    run(with_body("$display(\"x\");"));
    REQUIRE(sigaction(SIGINT, nullptr, &after) == 0);
    CHECK(after.sa_handler == before.sa_handler);
    CHECK(after.sa_flags == before.sa_flags);
#endif
}

TEST_CASE("sigint: an interrupt before the run does not stop the next one") {
    // The flag belongs to one run, so a design that did not get to see it cannot
    // leave it to end the next. (Only observable with a handler installed, which
    // is what the first run of the pair arranges.)
    const Run first = run("`timescale 1ns/1ns\n"
                          "module m;\n"
                          "  import \"DPI-C\" function void exit_status_test_raise_sigint();\n"
                          "  initial begin #3 exit_status_test_raise_sigint(); $finish; end\n"
                          "endmodule\n");
    // $finish ends the run in the same step, before the boundary where the flag
    // would be looked at.
    CHECK(first.status == 0);
    const Run second = run("`timescale 1ns/1ns\n"
                           "module m;\n"
                           "  initial begin #3 $display(\"three\"); #3 $display(\"six\"); end\n"
                           "endmodule\n");
    CHECK(second.out == "three\nsix\n");
    CHECK(second.status == 0);
}

// -- Ctrl-C on a real process --

#if !defined(_WIN32)

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include "Subprocess.h"

extern char** environ;

namespace {

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

// A temp directory holding one test's design, dump and captured streams.
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

    fs::path path(std::string_view name) const {
        return dir_ / name;
    }

    fs::path write(std::string_view name, std::string_view text) const {
        const fs::path p = path(name);
        std::ofstream  f(p, std::ios::binary);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        return p;
    }

private:
    fs::path dir_;
};

std::string read(const fs::path& p) {
    return clotho::test::detail::read_file(p);
}

// True once `done` holds, false if it did not within `limit`. A test that waits on
// a process polls: the process owes it no notification.
template<typename Pred>
bool poll_until(Pred done, seconds limit) {
    const auto deadline = steady_clock::now() + limit;
    while (!done()) {
        if (steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(2));
    }
    return true;
}

// A `clotho run` that has been started and not yet waited for. Whatever a test
// leaves running is killed with it, so a failed check cannot leak a simulator
// that never stops.
class Child {
public:
    Child(const fs::path& design, const fs::path& out, const fs::path& err, bool inherit_sigint_ignored = false) {
        posix_spawn_file_actions_t files;
        posix_spawn_file_actions_init(&files);
        posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_addopen(&files, STDERR_FILENO, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        // A test runner that was itself started with SIGINT ignored (a background
        // job of a non-interactive shell) would hand that to the child through
        // exec, and there would be nothing to interrupt. Say what the child gets.
        sigset_t defaults;
        sigemptyset(&defaults);
        if (!inherit_sigint_ignored) {
            sigaddset(&defaults, SIGINT);
        }
        sigset_t unblocked;
        sigemptyset(&unblocked);
        posix_spawnattr_setsigdefault(&attr, &defaults);
        posix_spawnattr_setsigmask(&attr, &unblocked);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

        std::string        exe    = CLOTHO_EXE;
        std::string        run    = "run";
        std::string        source = design.string();
        std::vector<char*> argv   = { exe.data(), run.data(), source.data(), nullptr };

        // The child inherits the ignored disposition from this process at spawn,
        // so for the "ignored on entry" case it is set here and put back after.
        struct sigaction ignore = {};
        struct sigaction saved  = {};
        ignore.sa_handler       = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        if (inherit_sigint_ignored) {
            sigaction(SIGINT, &ignore, &saved);
        }
        const int rc = posix_spawn(&pid_, exe.c_str(), &files, &attr, argv.data(), environ);
        if (inherit_sigint_ignored) {
            sigaction(SIGINT, &saved, nullptr);
        }
        posix_spawn_file_actions_destroy(&files);
        posix_spawnattr_destroy(&attr);
        REQUIRE_MESSAGE(rc == 0, "could not spawn clotho");
    }

    ~Child() {
        if (!reaped_) {
            kill(pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
        }
    }
    Child(const Child&)            = delete;
    Child& operator=(const Child&) = delete;

    void signal(int sig) const {
        kill(pid_, sig);
    }

    // Whether the process is still running. Reaps it if it is not.
    bool running() {
        if (reaped_) {
            return false;
        }
        int         status = 0;
        const pid_t r      = waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            reaped_ = true;
            status_ = status;
            return false;
        }
        return true;
    }

    // The wait status once the process has ended, or nothing if it did not end
    // within `limit` (it is killed, so the test fails instead of hanging).
    std::optional<int> wait(seconds limit) {
        if (!poll_until([&] { return !running(); }, limit)) {
            return std::nullopt;
        }
        return status_;
    }

private:
    pid_t pid_    = -1;
    bool  reaped_ = false;
    int   status_ = 0;
};

// A design that never ends by itself and dumps a VCD. The clock toggles at every
// step, so every step leaves a marker in the dump.
std::string free_running(const fs::path& vcd) {
    return std::format("`timescale 1ns/1ns\n"
                       "module top;\n"
                       "  logic clk = 0;\n"
                       "  integer n = 0;\n"
                       "  always #5 begin clk = ~clk; n = n + 1; end\n"
                       "  initial begin $dumpfile(\"{}\"); $dumpvars(0, top); end\n"
                       "  final $display(\"final block ran, n=%0d\", n);\n"
                       "endmodule\n",
                       vcd.generic_string());
}

// "clotho: interrupted at <n> <unit>" -> nanoseconds. The design has a 1 ns grid,
// so the unit is never finer than ns.
std::optional<uint64_t> interrupted_at_ns(const std::string& err) {
    static const std::regex pattern(R"(clotho: interrupted at ([0-9]+) (ns|us|ms|s)\n)");
    std::smatch             m;
    if (!std::regex_search(err, m, pattern)) {
        return std::nullopt;
    }
    const uint64_t value = std::stoull(m[1]);
    const std::string unit = m[2];
    const uint64_t scale   = unit == "ns" ? 1 : unit == "us" ? 1'000 : unit == "ms" ? 1'000'000 : 1'000'000'000;
    return value * scale;
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

// Whether `line` is a complete line of a VCD's value-change section. A write cut
// off partway through a line fails here: a scalar change with no identifier, a
// vector change with no space and identifier after its bits.
bool is_complete_vcd_line(std::string_view line) {
    if (line.empty()) {
        return false;
    }
    switch (line[0]) {
        case '$':
            return true;
        case '#':
            return all_digits(line.substr(1));
        case 'b':
        case 'r': {
            const size_t space = line.find(' ');
            return space != std::string_view::npos && space > 1 && space + 1 < line.size();
        }
        default:
            return line.size() >= 2 && std::string_view("01xzXZ").find(line[0]) != std::string_view::npos;
    }
}

} // namespace

TEST_CASE("sigint: Ctrl-C ends a free-running design at a time step: no finals, status 130, a whole dump") {
    const Scratch  s;
    const fs::path vcd    = s.path("wave.vcd");
    const fs::path design = s.write("design.sv", free_running(vcd));
    Child          child(design, s.path("stdout.txt"), s.path("stderr.txt"));

    // The dump growing is the sign the simulation is running: $dumpvars executes
    // inside it, after the handler is in place.
    REQUIRE(poll_until([&] { return fs::exists(vcd) && fs::file_size(vcd) > 2048; }, seconds(120)));
    child.signal(SIGINT);
    const std::optional<int> status = child.wait(seconds(120));
    REQUIRE_MESSAGE(status.has_value(), "the run did not stop after SIGINT");
    REQUIRE(WIFEXITED(*status));
    CHECK(WEXITSTATUS(*status) == 130);

    const std::string err = read(s.path("stderr.txt"));
    CAPTURE(err);
    const std::optional<uint64_t> stopped_at = interrupted_at_ns(err);
    REQUIRE(stopped_at.has_value());
    // The finals are skipped: Ctrl-C asks the run to end, not to wrap up.
    CHECK(read(s.path("stdout.txt")).find("final block ran") == std::string::npos);

    // The dump is closed and ends at a complete time step: every line of it is a
    // whole line, and the last step it records is the one the run stopped at.
    const std::string text = read(vcd);
    REQUIRE(text.ends_with("\n"));
    const size_t defs = text.find("$enddefinitions");
    REQUIRE(defs != std::string::npos);
    std::istringstream body(text.substr(text.find('\n', defs) + 1));
    std::string        line;
    std::string        last_marker;
    std::string        malformed;
    bool               changed_after_marker = false;
    while (std::getline(body, line)) {
        if (!is_complete_vcd_line(line)) {
            malformed = line;
            break;
        }
        if (line[0] == '#') {
            last_marker          = line;
            changed_after_marker = false;
        } else if (line[0] != '$') {
            changed_after_marker = true;
        }
    }
    CHECK_MESSAGE(malformed.empty(), "a cut-off line in the dump: '" << malformed << "'");
    REQUIRE(!last_marker.empty());
    CHECK(changed_after_marker);
    CHECK(last_marker == "#" + std::to_string(*stopped_at));
}

TEST_CASE("sigint: a second Ctrl-C ends a run that never reaches a time step") {
    const Scratch  s;
    // One process spinning with no timing control never lets the simulation reach
    // the boundary where a first Ctrl-C is looked at.
    const fs::path design = s.write("design.sv", "module top;\n"
                                                 "  integer n = 0;\n"
                                                 "  initial begin\n"
                                                 "    $display(\"spinning\");\n"
                                                 "    $fflush;\n"
                                                 "    forever n = n + 1;\n"
                                                 "  end\n"
                                                 "  final $display(\"final block ran\");\n"
                                                 "endmodule\n");
    Child child(design, s.path("stdout.txt"), s.path("stderr.txt"));
    REQUIRE(poll_until([&] { return read(s.path("stdout.txt")).find("spinning") != std::string::npos; }, seconds(120)));

    child.signal(SIGINT);
    // The first one is only noted. Nothing here can act on it, so the process
    // is still running a good while later.
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(child.running());

    child.signal(SIGINT);
    const std::optional<int> status = child.wait(seconds(60));
    REQUIRE_MESSAGE(status.has_value(), "the second SIGINT did not end the run");
    REQUIRE(WIFSIGNALED(*status));
    CHECK(WTERMSIG(*status) == SIGINT);
    CHECK(read(s.path("stdout.txt")).find("final block ran") == std::string::npos);
}

TEST_CASE("sigint: a run that inherited SIGINT ignored keeps ignoring it") {
    const Scratch  s;
    const fs::path design = s.write("design.sv", free_running(s.path("wave.vcd")));
    Child          child(design, s.path("stdout.txt"), s.path("stderr.txt"), /*inherit_sigint_ignored=*/true);

    REQUIRE(poll_until([&] { return fs::exists(s.path("wave.vcd")) && fs::file_size(s.path("wave.vcd")) > 2048; },
                       seconds(120)));
    child.signal(SIGINT);
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(child.running());
    child.signal(SIGKILL);
    const std::optional<int> status = child.wait(seconds(60));
    REQUIRE(status.has_value());
    CHECK(WIFSIGNALED(*status));
    CHECK(read(s.path("stderr.txt")).find("interrupted") == std::string::npos);
}

// -- $fatal, through the driver --

namespace {

struct Cli {
    int         code = -1;
    std::string out;
    std::string err;
};

Cli run_cli(const Scratch& s, std::string_view source) {
    const fs::path design = s.write("design.sv", source);
    const fs::path out    = s.path("cli.out");
    const fs::path err    = s.path("cli.err");
    const std::string cmd =
      std::format("\"{}\" run \"{}\" > \"{}\" 2> \"{}\"", CLOTHO_EXE, design.string(), out.string(), err.string());
    const int status = std::system(cmd.c_str());
    return Cli{ WIFEXITED(status) ? WEXITSTATUS(status) : -1, read(out), read(err) };
}

} // namespace

TEST_CASE("exit status: `clotho run` exits 1 after a $fatal and 0 after $finish(2) and $error") {
    const Scratch s;
    const Cli     fatal = run_cli(s, with_body("$fatal(1, \"giving up\");"));
    CHECK(fatal.code == 1);
    CHECK(fatal.out == "final ran\n");
    CHECK(fatal.err.find("FATAL: ") != std::string::npos);

    CHECK(run_cli(s, with_body("$finish(2);")).code == 0);
    CHECK(run_cli(s, with_body("$error(\"e\");")).code == 0);
}

#endif // !defined(_WIN32)
