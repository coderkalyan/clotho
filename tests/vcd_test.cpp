#include <doctest/doctest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
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

TEST_CASE("vcd: unpacked-array memory dumps one $var per element") {
    const std::string src = R"(
        module top;
          reg [7:0] mem [0:3];
          integer i;
          initial begin
            for (i = 0; i < 4; i = i + 1) mem[i] = 0;
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 mem[2] = 8'hAB;
            #5 $finish;
          end
        endmodule
    )";
    const std::string vcd  = run_vcd(src);
    const std::string body = body_of(vcd);
    // Each element is its own $var named mem[<addr>] over the declared [0:3] range.
    CHECK(contains(vcd, "mem[0] $end"));
    CHECK(contains(vcd, "mem[1] $end"));
    CHECK(contains(vcd, "mem[2] $end"));
    CHECK(contains(vcd, "mem[3] $end"));
    // Only mem[2] changes at #5, to 0xAB; the other elements are not re-emitted.
    CHECK(contains(body, "#5\nb10101011 "));
}

TEST_CASE("vcd: nonblocking array-element writes are dumped per element") {
    // mem[idx] <= ... each cycle: the touched element must re-emit on its write
    // cycle only (array writes are instrumented, not always-scanned).
    const std::string src = R"(
        module top;
          reg clk = 0;
          reg [7:0] mem [0:3];
          reg [1:0] idx = 0;
          integer i;
          initial for (i = 0; i < 4; i = i + 1) mem[i] = 0;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            mem[idx] <= mem[idx] + 8'h11;
            idx <= idx + 1;
          end
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #45 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    // mem[0] is written at the first posedge (#5) -> 0x11; mem[1] at #15, etc.
    CHECK(contains(body, "#5\n"));
    CHECK(contains(body, "b10001 "));   // 0x11 on the written element
    // Count 0x11 (b10001) emissions: one per element as each is written once.
    size_t n = 0;
    for (size_t p = body.find("b10001 "); p != std::string::npos; p = body.find("b10001 ", p + 1)) {
        ++n;
    }
    CHECK(n == 4);   // mem[0..3] each written once over the run
}

TEST_CASE("vcd: descending array range names elements by declared address") {
    const std::string src = R"(
        module top;
          reg [3:0] m [3:1];
          initial begin
            m[3] = 0; m[2] = 0; m[1] = 0;
            $dumpfile("OUT.vcd");
            $dumpvars;
            #1 $finish;
          end
        endmodule
    )";
    const std::string vcd = run_vcd(src);
    CHECK(contains(vcd, "m[3] $end"));
    CHECK(contains(vcd, "m[2] $end"));
    CHECK(contains(vcd, "m[1] $end"));
}

TEST_CASE("vcd: signals wider than 64 bits dump as full multi-word vectors") {
    const std::string src = R"(
        module top;
          reg [95:0] w = 0;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 w = 96'hDEAD_BEEF_1234_5678_9ABC_DEF0;
            #5 $finish;
          end
        endmodule
    )";
    const std::string vcd  = run_vcd(src);
    const std::string body = body_of(vcd);
    CHECK(contains(vcd, "$var reg 96 "));   // declared at full width in the header
    // 0xDEADBEEF123456789ABCDEF0 as 96 binary bits, MSB first, no leading-zero loss
    // (the MSB is 1, so the full width prints).
    CHECK(contains(body, "b110111101010110110111110111011110001001000110100010101100111100"
                         "010011010101111001101111011110000 "));
}

TEST_CASE("vcd: wide four-state value renders x then the settled bits") {
    const std::string src = R"(
        module top;
          reg [70:0] w;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #5 w = 71'h5A;
            #5 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));
    CHECK(contains(body, "#0\nbx "));         // uninitialized 71-bit reg is all-x
    CHECK(contains(body, "#5\nb1011010 "));   // 0x5A, leading zeros dropped
}

TEST_CASE("vcd: high-activity trace stays ordered through the writer thread") {
    // Enough instants to exceed the writer's handoff threshold many times over, so
    // the background writer processes multiple buffers. A race would corrupt the
    // record order; assert the time markers come out strictly increasing and the
    // final value is correct.
    const std::string src = R"(
        module top;
          reg [15:0] a = 0;
          always #1 a = a + 1;
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #2000 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));

    long prev  = -1;
    long times = 0;
    for (size_t i = 0; i < body.size();) {
        if (body[i] == '#') {
            const long t = std::strtol(body.c_str() + i + 1, nullptr, 10);
            CHECK(t > prev);   // strictly increasing: no reordering across the thread
            prev = t;
            ++times;
        }
        i = body.find('\n', i);
        if (i == std::string::npos) {
            break;
        }
        ++i;
    }
    // One instant per tick 0..2000: the always block fires at t=2000 (a -> 2000)
    // before $finish, and that final value is dumped at #2000 — matches iverilog 13
    // (both emit 2001 markers ending b11111010000). $finish does not suppress the
    // current instant's postponed-region dump.
    CHECK(times == 2001);
    CHECK(contains(body, "b11111001111 !"));    // a == 1999 at the penultimate instant
    CHECK(contains(body, "#2000\nb11111010000 !")); // a == 2000 dumped at the finish instant
}

TEST_CASE("vcd: dirty set re-emits only signals that actually change") {
    // `moving` toggles every posedge (an NBA target); `still` is written the same
    // value every posedge, and `idle` is never written. All three appear at #0
    // (the initial dump); afterwards only `moving` should re-appear -- the NBA
    // commit touches `still` too, but the flush diff suppresses the no-change.
    const std::string src = R"(
        module top;
          reg clk = 0;
          reg [3:0] moving = 0;
          reg [3:0] still  = 7;
          reg [3:0] idle   = 3;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            moving <= moving + 1;
            still  <= 7;
          end
          initial begin
            $dumpfile("OUT.vcd");
            $dumpvars;
            #28 $finish;
          end
        endmodule
    )";
    const std::string body = body_of(run_vcd(src));

    // The initial dump at #0 carries all three; count later re-emissions.
    const auto count = [&](std::string_view id) {
        size_t n = 0;
        for (size_t p = body.find(id); p != std::string::npos; p = body.find(id, p + 1)) {
            ++n;
        }
        return n;
    };
    // Identify each var's id from the header is overkill; instead check that only
    // one 4-bit value line pattern recurs. `moving` reaches 1,2 over #28; `still`
    // and `idle` never change after #0. Concretely: exactly one #0 block, and after
    // it only `moving`'s edges produce vector lines.
    CHECK(count("#0") == 1);
    // moving = 1 at first posedge (#5), 2 at #15, 3 at #25 -> b1, b10, b11 appear.
    CHECK(contains(body, "b1 "));
    CHECK(contains(body, "b10 "));
    CHECK(contains(body, "b11 "));
    // `still` holds 7 (b111) and `idle` holds 3 (b11) only at #0; b111 must not
    // recur (still never changes value).
    CHECK(count("b111 ") == 1);
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
