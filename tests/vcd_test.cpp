#include <doctest/doctest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// A fresh dump path per run so cases (which ctest may run in parallel processes)
// never collide.
std::string unique_path() {
    static std::atomic<int> counter{ 0 };
    return "/tmp/clotho_vcd_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + ".vcd";
}

// JIT-run a design and return the VCD it wrote. The source must open its dump with
// $dumpfile("OUT.vcd"); the token is rewritten to a unique path. stdout ($display)
// is discarded so a design's prints do not pollute the test log.
std::string run_vcd(std::string source) {
    const std::string path = unique_path();
    const auto        tok  = source.find("OUT.vcd");
    REQUIRE(tok != std::string::npos);
    source.replace(tok, std::string_view("OUT.vcd").size(), path);

    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    std::FILE* null = std::fopen("/dev/null", "w");
    if (null != nullptr) {
        dup2(fileno(null), fileno(stdout));
    }

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    if (null != nullptr) {
        std::fclose(null);
    }

    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::remove(path.c_str());
    return ss.str();
}

// The value-change body: everything after $enddefinitions.
std::string body_of(const std::string& vcd) {
    const auto pos = vcd.find("$enddefinitions");
    REQUIRE(pos != std::string::npos);
    return vcd.substr(vcd.find('\n', pos) + 1);
}

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("vcd: scalar + vector value changes, with no-change suppression") {
    // A clock and a counter. Registration order (clk, count, next) fixes the
    // identifier codes !/"/#. $finish at #38 lands between edges, so the trace ends
    // cleanly at the #35 posedge with no same-time race at the boundary.
    const std::string src = R"(
        module top;
          reg clk = 0;
          reg [3:0] count = 0;
          wire [3:0] next = count + 1;
          always #5 clk = ~clk;
          always @(posedge clk) count <= next;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #38 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));

    // The full trace is deterministic. Note the compressed vectors (b100, not
    // b0100) and that #10/#20/#30 carry only the clock edge — count/next did not
    // change, so they are not re-emitted (LRM §21.7: one settled value per time).
    const std::string expected =
      "#0\n0!\nb0 \"\nb1 #\n"
      "#5\n1!\nb1 \"\nb10 #\n"
      "#10\n0!\n"
      "#15\n1!\nb10 \"\nb11 #\n"
      "#20\n0!\n"
      "#25\n1!\nb11 \"\nb100 #\n"
      "#30\n0!\n"
      "#35\n1!\nb100 \"\nb101 #\n";
    CHECK(body == expected);
}

TEST_CASE("vcd: header declares scopes, vars, and a timescale") {
    const std::string src = R"(
        `timescale 1ns/1ps
        module top;
          reg a = 0;
          reg [7:0] bus = 0;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #1 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    CHECK(contains(vcd, "$timescale\n\t1ns\n$end"));
    CHECK(contains(vcd, "$scope module top $end"));
    CHECK(contains(vcd, "$var reg 1 ! a $end"));
    CHECK(contains(vcd, "$var reg 8 \" bus $end"));
    CHECK(contains(vcd, "$upscope $end"));
    CHECK(contains(vcd, "$enddefinitions $end"));
}

TEST_CASE("vcd: x and z render per bit") {
    // r is an uninitialized reg (x); w is an undriven 4-state net (z, LRM 6.6).
    const std::string src = R"(
        module top;
          reg [1:0] r;
          wire [1:0] w;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #1 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // 2-bit all-x compresses to "x", all-z to "z".
    CHECK(contains(body, "bx "));
    CHECK(contains(body, "bz "));
}

TEST_CASE("vcd: nested instance hierarchy nests $scope/$upscope") {
    const std::string src = R"(
        module sub(input wire clk, output reg [2:0] q);
          initial q = 0;
          always @(posedge clk) q <= q + 1;
        endmodule
        module top;
          reg clk = 0;
          wire [2:0] q;
          sub u1(.clk(clk), .q(q));
          always #5 clk = ~clk;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #12 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    // top scope contains a nested u1 scope, each closed by its own $upscope.
    const auto top    = vcd.find("$scope module top $end");
    const auto u1     = vcd.find("$scope module u1 $end");
    const auto enddef = vcd.find("$enddefinitions");
    REQUIRE(top != std::string::npos);
    REQUIRE(u1 != std::string::npos);
    CHECK(top < u1);
    CHECK(u1 < enddef);
    CHECK(contains(vcd, "$var reg 3 "));   // u1.q, a 3-bit reg
}

TEST_CASE("vcd: $dumpoff writes an all-x checkpoint, $dumpon resumes") {
    const std::string src = R"(
        module top;
          reg [1:0] r;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 r = 2'b10;
            #5 $dumpoff;
            #5 $dumpon;
            #5 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // #0 x, #5 the assignment, #10 the dumpoff all-x checkpoint, #15 the dumpon
    // re-dump of the current value.
    const std::string expected = "#0\nbx !\n#5\nb10 !\n#10\nbx !\n#15\nb10 !\n";
    CHECK(body == expected);
}

TEST_CASE("vcd: $dumpvars(0, top) matches the bare whole-design form") {
    const std::string bare = R"(
        module top;
          reg [3:0] count = 0;
          always #5 count = count + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #18 $finish;
          end
        endmodule
    )";
    const std::string scoped = R"(
        module top;
          reg [3:0] count = 0;
          always #5 count = count + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars(0, top);
            #18 $finish;
          end
        endmodule
    )";
    CHECK(body_of(run_vcd(bare)) == body_of(run_vcd(scoped)));
}
