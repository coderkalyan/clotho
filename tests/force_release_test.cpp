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

// JIT-run a design that ends via $finish and return its stdout (fork + temp
// file, mirroring continuous_assign_delay_test.cpp).
std::string run_capture_finish(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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
    return r.ok ? std::string{} : r.diagnostics;
}

} // namespace

// LRM 10.6.2: a force overrides procedural assignment; on release a variable
// holds its forced value until the next procedural write. Byte-identical to
// iverilog 13 (t=1 a=10, t=2 a=99, t=3 a=99 (write suppressed), t=4 a=99 (held
// through release), t=5 a=7).
TEST_CASE("force/release: override, suppression, and release-hold") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial begin
                a = 8'd10;
                #1 $display("t=%0t a=%0d", $time, a);
                force a = 8'd99;
                #1 $display("t=%0t a=%0d", $time, a);
                a = 8'd5;
                #1 $display("t=%0t a=%0d", $time, a);
                release a;
                #1 $display("t=%0t a=%0d", $time, a);
                a = 8'd7;
                #1 $display("t=%0t a=%0d", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) ==
          "t=1 a=10\nt=2 a=99\nt=3 a=99\nt=4 a=99\nt=5 a=7\n");
}

// A force RHS is a continuous relationship (LRM 10.6.2): `force a = b` tracks
// every later change of b. Matches iverilog 13.
TEST_CASE("force: RHS is continuously tracked") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                b = 8'd20; a = 8'd1;
                #1 force a = b;
                #1 $display("a=%0d", a);
                b = 8'd50;
                #1 $display("a=%0d", a);
                b = 8'd200;
                #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=20\na=50\na=200\n");
}

TEST_CASE("force: re-force is last-wins, old RHS link broken") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                b = 8'd20; a = 8'd1;
                #1 force a = b;
                #1 $display("a=%0d", a);
                force a = 8'd77;
                b = 8'd99;
                #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=20\na=77\n");
}

// A force overrides a nonblocking (clocked) driver; on release the NBA driver
// resumes. Matches iverilog 13.
TEST_CASE("force: suppresses an NBA clocked driver, resumes on release") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] q = 0;
            logic clk = 0;
            always #1 clk = ~clk;
            always @(posedge clk) q <= q + 1;
            initial begin
                #3 force q = 8'hAA;
                #10 $display("q=%0d", q);
                release q;
                #4 $display("q=%0d", q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "q=170\nq=172\n");
}

// Two independent forced variables do not interfere.
TEST_CASE("force: independent targets") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                a = 1; b = 2;
                force a = 8'd10;
                force b = 8'd20;
                #1 $display("a=%0d b=%0d", a, b);
                release a;
                a = 8'd3;
                #1 $display("a=%0d b=%0d", a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=10 b=20\na=3 b=20\n");
}

// release of a variable never forced is a legal no-op (LRM 10.6.2).
TEST_CASE("release: never-forced variable is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial begin
                a = 8'd5;
                release a;
                #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=5\n");
}

TEST_CASE("assign/force/release/deassign: full priority ladder") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                a = 8'd1; b = 8'd10;
                #1 a = 8'd2;    $display("a=%0d", a);
                assign a = b;
                #1 $display("a=%0d", a);
                a = 8'd99;
                #1 $display("a=%0d", a);
                b = 8'd20; #1 $display("a=%0d", a);
                force a = 8'd77;
                b = 8'd30; #1 $display("a=%0d", a);
                release a;
                #1 $display("a=%0d", a);
                deassign a;
                #1 $display("a=%0d", a);
                a = 8'd5; #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) ==
          "a=2\na=10\na=10\na=20\na=77\na=30\na=30\na=5\n");
}

// A procedural continuous `assign` is a continuous relationship and is last-wins:
// a second `assign` supersedes the first. Matches iverilog 13.
TEST_CASE("assign: continuous tracking and last-wins") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b, c;
            initial begin
                b = 8'd10; c = 8'd50;
                assign a = b;
                #1 $display("a=%0d", a);
                b = 8'd11; #1 $display("a=%0d", a);
                assign a = c;
                #1 $display("a=%0d", a);
                b = 8'd12; #1 $display("a=%0d", a);
                c = 8'd55; #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=10\na=11\na=50\na=50\na=55\n");
}

// -- Loud guards (Phase 1 boundaries) --

TEST_CASE("force on a net is rejected (Phase 1.5)") {
    constexpr std::string_view src = R"(
        module m; wire w; logic x;
            initial begin force w = x; end
        endmodule)";
    CHECK(lower_error(src).find("force is only supported on a whole integral variable") !=
          std::string::npos);
}

TEST_CASE("select-write to a forced variable is rejected (Phase 2)") {
    constexpr std::string_view src = R"(
        module m; logic [7:0] a;
            initial begin force a = 8'd1; a[0] = 1'b0; end
        endmodule)";
    CHECK(lower_error(src).find("select of a force-target variable") != std::string::npos);
}

TEST_CASE("release: reestablishes a continuous assignment on the variable") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b, d;
            assign d = a + b;
            initial begin
                a = 1; b = 2;
                #1 $display("d=%0d", d);
                force d = 8'd100;
                #1 a = 10;
                #1 $display("d=%0d", d);
                release d;
                #1 $display("d=%0d", d);
                b = 20;
                #1 $display("d=%0d", d);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "d=3\nd=100\nd=12\nd=30\n");
}

TEST_CASE("force on an input port variable is rejected") {
    constexpr std::string_view src = R"(
        module sub(input logic [7:0] p); endmodule
        module m;
            logic [7:0] x;
            sub s(.p(x));
            initial begin force s.p = 8'd1; end
        endmodule)";
    CHECK(lower_error(src).find("input or inout port variable") != std::string::npos);
}
