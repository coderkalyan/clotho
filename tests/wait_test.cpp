#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// Read the whole of a temp file into a string.
std::string slurp(std::FILE* f) {
    std::rewind(f);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    return out;
}

// JIT-run a design that ends via $finish and return its stdout. $finish calls
// std::exit, so the simulation runs in a forked child writing to a temp file.
// Every wait test drives a free-running stimulus process that only stops at
// $finish, so this (not a plain drain) is the right harness.
std::string run_capture_finish(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        dup2(fileno(tmp), fileno(stdout));
        ir::run_jit(*r.module);
        std::fflush(stdout);
        _exit(0);
    }

    int wstatus = 0;
    REQUIRE(waitpid(pid, &wstatus, 0) == pid);

    std::string out = slurp(tmp);
    std::fclose(tmp);
    return out;
}

std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    return r.ok ? std::string{} : r.diagnostic_text();
}

} // namespace

TEST_CASE("wait on an already-true condition does not suspend") {
    constexpr std::string_view src = R"(
        module m;
            logic g = 1;
            initial begin
                $display("before %0t", $time);
                wait(g);
                $display("after %0t", $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "before 0\nafter 0\n");
}

// LRM 9.4.3: a false condition suspends until it becomes true, then the body
// runs. The process must resume at the exact time the last read changed it true.
TEST_CASE("wait suspends until the condition rises") {
    constexpr std::string_view src = R"(
        module m;
            logic g = 0;
            initial begin
                wait(g);
                $display("woke %0t", $time);
                $finish;
            end
            initial #25 g = 1;
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 25\n");
}

TEST_CASE("wait treats X as false and blocks until a definite 1") {
    constexpr std::string_view src = R"(
        module m;
            logic g;
            initial begin
                wait(g);
                $display("woke %0t g=%b", $time, g);
                $finish;
            end
            initial begin
                #10 g = 1'bx;
                #10 g = 1;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 20 g=1\n");
}

// A multi-signal condition suspends on an any-change wait of every read, and
// re-evaluates the whole expression on each wake.
TEST_CASE("wait on a compound condition wakes when the whole expression is true") {
    constexpr std::string_view src = R"(
        module m;
            logic a = 0, b = 0;
            initial begin
                wait(a && b);
                $display("both %0t", $time);
                $finish;
            end
            initial begin
                #10 a = 1;   // a&&b still false
                #10 b = 1;   // now true
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "both 20\n");
}

// A wait inside a loop must re-arm every iteration — a producer/consumer
// handshake. Verified byte-identical to iverilog 13.
TEST_CASE("wait re-arms across loop iterations (handshake)") {
    constexpr std::string_view src = R"(
        module m;
            logic req = 0;
            int i;
            initial begin
                for (i = 0; i < 3; i++) begin
                    wait(req);
                    $display("got %0d %0t", i, $time);
                    wait(!req);
                end
                $finish;
            end
            initial forever begin
                #10 req = 1;
                #10 req = 0;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "got 0 10\ngot 1 30\ngot 2 50\n");
}

TEST_CASE("wait on a constant-false condition parks the process forever") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                $display("before");
                wait(0);
                $display("UNREACHABLE");
            end
            initial #100 begin
                $display("watchdog %0t", $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "before\nwatchdog 100\n");
}

// A string condition would need a string-aware wait queue (string stores skip
// wait-queue servicing), so it must fail loudly, not silently never re-trigger.
TEST_CASE("wait on a string condition fails loudly") {
    constexpr std::string_view src = R"(
        module m;
            string s = "";
            initial begin
                wait(s);
                $display("x");
            end
        endmodule
    )";
    const std::string err = lower_error(src);
    CHECK(err.find("string") != std::string::npos);
}
