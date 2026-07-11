#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    std::rewind(tmp);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
}

} // namespace

// fork … join (LRM 9.3.2), V1. Values cross-checked against Icarus Verilog 13.
// Child execution order among same-instant children is LRM-unspecified; Clotho
// runs them in source order, so the deterministic checks below assume that.

TEST_CASE("fork: zero-time children all run before the join completes") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              $display("A");
              $display("B");
            join
            $display("done");
          end
        endmodule
    )");
    CHECK(out == "A\nB\ndone\n");
}

TEST_CASE("fork: parent blocks until the last child finishes") {
    // B finishes at 5, A at 10; join resumes the parent at 10.
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              begin #10 $display("A@%0t", $time); end
              begin #5  $display("B@%0t", $time); end
            join
            $display("done@%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "B@5\nA@10\ndone@10\n");
}

TEST_CASE("fork: event-controlled children write shared parent variables") {
    const std::string out = run_capture(R"(
        module t;
          logic [7:0] a = 0, b = 0;
          logic clk = 0;
          always #5 clk = ~clk;
          initial begin
            fork
              begin @(posedge clk) a = 8'd11; end
              begin @(posedge clk) b = 8'd22; end
            join
            $display("a=%0d b=%0d t=%0t", a, b, $time);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "a=11 b=22 t=5\n");
}

TEST_CASE("fork: join serializes reuse across loop iterations") {
    // Each iteration blocks at the join, so children never overlap: the bounded
    // static ring holds (docs/design/fork-join.md).
    const std::string out = run_capture(R"(
        module t;
          logic [7:0] a = 0, b = 0;
          initial begin
            for (int i = 0; i < 3; i = i + 1) begin
              fork
                a = a + 8'd1;
                b = b + 8'd2;
              join
            end
            $display("a=%0d b=%0d", a, b);
          end
        endmodule
    )");
    CHECK(out == "a=3 b=6\n");
}

TEST_CASE("fork: nonblocking assigns from children commit") {
    const std::string out = run_capture(R"(
        module t;
          logic clk = 0; logic [7:0] q1 = 0, q2 = 0;
          always #5 clk = ~clk;
          initial begin
            fork
              repeat (2) @(posedge clk) q1 <= q1 + 8'd1;
              repeat (2) @(posedge clk) q2 <= q2 + 8'd2;
            join
            #1 $display("q1=%0d q2=%0d", q1, q2);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "q1=2 q2=4\n");
}

TEST_CASE("fork: single-child and empty forks") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork $display("solo"); join
            fork join
            $display("after");
          end
        endmodule
    )");
    CHECK(out == "solo\nafter\n");
}

// Unsupported variants fail loudly (never silently serialized).
TEST_CASE("fork: join_any / join_none / fork-in-subroutine rejected") {
    check_rejected(R"(
        module t; initial fork #5 $display("x"); #6 $display("y"); join_any endmodule
    )",
                   "join_any");
    check_rejected(R"(
        module t; initial fork #5 $display("x"); #6 $display("y"); join_none endmodule
    )",
                   "join_none");
    check_rejected(R"(
        module t;
          task run; fork $display("x"); $display("y"); join endtask
          initial run;
        endmodule
    )",
                   "fork..join inside a function or task");
}
