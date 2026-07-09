// Packed ranges other than [N-1:0]: reversed ([0:7]), non-zero aligned
// ([15:8]), and negative ([-1:-8]) declared ranges, across constant and
// runtime bit selects, part selects, indexed part selects, and their lvalue
// forms. Every expected string was cross-checked against Icarus Verilog
// (iverilog v13) as the oracle.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and return everything it wrote to stdout.
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

// Lower and return the rendered diagnostics; ok must be false (a rejected design).
std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

TEST_CASE("non-[N-1:0] packed ranges: reads agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            logic [0:7]   big;
            logic [15:8]  mid;
            logic [-1:-8] neg;
            integer i;
            initial begin
                big = 8'hA5; mid = 8'hA5; neg = 8'hA5;
                $display("cbit %b%b %b%b %b%b", big[0], big[7], mid[15], mid[8], neg[-1], neg[-8]);
                for (i = 0; i < 8; i = i + 1) $display("dyn %0d %b %b", i, big[i], mid[i+8]);
                for (i = -8; i <= -1; i = i + 1) $display("dynneg %0d %b", i, neg[i]);
                $display("rng %h%h %h%h %h%h", big[0:3], big[4:7], mid[15:12], mid[11:8], neg[-1:-4], neg[-5:-8]);
                $display("ips %h %h %h", big[2 +: 4], mid[13 -: 4], neg[-6 +: 4]);
                i = 2; $display("dips %h %h", big[i +: 4], mid[i+8 +: 4]);
                i = -5; $display("dipsneg %h", neg[i +: 4]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "cbit 11 11 11\n"
          "dyn 0 1 1\ndyn 1 0 0\ndyn 2 1 1\ndyn 3 0 0\n"
          "dyn 4 0 0\ndyn 5 1 1\ndyn 6 0 0\ndyn 7 1 1\n"
          "dynneg -8 1\ndynneg -7 0\ndynneg -6 1\ndynneg -5 0\n"
          "dynneg -4 0\ndynneg -3 1\ndynneg -2 0\ndynneg -1 1\n"
          "rng a5 a5 a5\n"
          "ips 9 9 9\n"
          "dips 9 9\n"
          "dipsneg 4\n");
}

TEST_CASE("non-[N-1:0] packed ranges: writes agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            logic [0:7]   big;
            logic [15:8]  mid;
            logic [-1:-8] neg;
            integer i;
            initial begin
                big = 8'h00; mid = 8'h00; neg = 8'h00;
                big[1] = 1'b1; mid[14] = 1'b1; neg[-2] = 1'b1;
                $display("wbit %h %h %h", big, mid, neg);
                big[2:5] = 4'hC; mid[13:10] = 4'hC; neg[-3:-6] = 4'hC;
                $display("wrng %h %h %h", big, mid, neg);
                i = 2;
                big[i +: 3] = 3'b101; mid[i+8 +: 3] = 3'b101;
                $display("wips %h %h", big, mid);
                i = -5; neg[i +: 3] = 3'b111;
                $display("wipsneg %h", neg);
                i = 5; big[i -: 3] = 3'b011;
                $display("wipsd %h", big);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "wbit 40 40 40\n"
          "wrng 70 70 70\n"
          "wips 68 74\n"
          "wipsneg 78\n"
          "wipsd 6c\n");
}

// Narrow indices exercise the offset widening: an unsigned index must
// zero-extend (7 stays 7) and a signed index must sign-extend (-3 stays -3)
// before the range-normalization subtraction.
TEST_CASE("narrow unsigned and signed runtime indices agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            logic [0:7]        big;
            logic [-1:-8]      neg;
            logic [2:0]        j;
            logic signed [3:0] sj;
            initial begin
                big = 8'hA5; neg = 8'h5A;
                j = 3'd6;   $display("nu %b", big[j]);
                j = 3'd7;   $display("nu2 %b", big[j]);
                sj = -4'sd3; $display("ns %b", neg[sj]);
                sj = -4'sd8; $display("ns2 %b", neg[sj]);
                sj = -4'sd6; $display("nips %h", neg[sj +: 3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "nu 0\nnu2 1\nns 0\nns2 0\nnips 6\n");
}

// LRM 11.5.1: a dynamic bit/part select out of the vector bounds reads x and
// writes nothing; a partially out-of-range select applies the rule bit-by-bit;
// an x/z index is wholly out of range. All lines cross-checked against iverilog.
TEST_CASE("out-of-bounds dynamic selects read x and write nothing (LRM 11.5.1)") {
    constexpr std::string_view src = R"(
        module t;
            logic [7:0] v;
            logic [3:0] p;
            logic       b;
            logic [3:0] xi;
            integer     i;
            initial begin
                // Reads: fully / partially out of range yield x.
                v = 8'hA5;
                i = 9;  b = v[i];      $display("%b", b);        // x
                i = -1; b = v[i];      $display("%b", b);        // x
                i = 6;  p = v[i +: 4]; $display("%b", p);        // xx10 (v[7:6]=10)
                i = 20; p = v[i +: 4]; $display("%b", p);        // xxxx
                i = -2; p = v[i +: 4]; $display("%b", p);        // 01xx (v[1:0]=01)
                // Writes: out-of-range bits are not modified.
                v = 8'hFF; i = 9;  v[i]      = 1'b0; $display("%h", v);  // ff
                v = 8'h00; i = 6;  v[i +: 4] = 4'hF; $display("%h", v);  // c0
                v = 8'h33; i = 20; v[i +: 4] = 4'hF; $display("%h", v);  // 33
                // An x/z index is wholly out of range: read x, write no-op.
                xi = 4'bxx01;
                v = 8'hA5; p = v[xi +: 4];         $display("%b", p);    // xxxx
                v = 8'h5A; v[xi +: 4] = 4'hF;      $display("%h", v);    // 5a
            end
        endmodule
    )";
    CHECK(run_capture(src) == "x\nx\nxx10\nxxxx\n01xx\nff\nc0\n33\nxxxx\n5a\n");
}

// The out-of-bounds rule also applies to nonblocking writes: the deferred merge
// must mask off the out-of-range bits. Cross-checked against iverilog.
TEST_CASE("out-of-bounds nonblocking writes affect only in-range bits") {
    constexpr std::string_view src = R"(
        module t;
            logic [7:0] v;
            integer     i;
            initial begin
                v = 8'hFF; i = 9;  v[i]      <= 1'b0;  #1 $display("%h", v);  // ff
                v = 8'h00; i = 6;  v[i +: 4] <= 4'hF;  #1 $display("%h", v);  // c0
                v = 8'h33; i = 20; v[i +: 4] <= 4'hF;  #1 $display("%h", v);  // 33
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ff\nc0\n33\n");
}

// Wide (>64-bit) constants built during lowering (repeat counters, ++ step,
// unary-minus zero) used to read out of bounds in InternPool::put_int and
// produce garbage planes. Cross-checked against iverilog.
TEST_CASE("wide-typed lowering constants: repeat, ++, unary minus") {
    constexpr std::string_view src = R"(
        module t;
            logic [95:0]  w;
            logic [127:0] v;
            integer n;
            initial begin
                w = 96'h3;
                n = 0;
                repeat (w) n = n + 1;
                $display("rep %0d", n);
                for (v = 128'hFFFF_FFFF_FFFF_FFFF_FFFF; v < 128'hFFFF_FFFF_FFFF_FFFF_FFFF + 3; v++)
                    $display("v %h %h", v[127:64], v[63:0]);
                v = 128'h1;
                v = -v;
                $display("neg %h %h", v[127:64], v[63:0]);
                w = -96'h5;
                $display("neg2 %h %h", w[95:64], w[63:0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "rep 3\n"
          "v 000000000000ffff ffffffffffffffff\n"
          "v 0000000000010000 0000000000000000\n"
          "v 0000000000010000 0000000000000001\n"
          "neg ffffffffffffffff ffffffffffffffff\n"
          "neg2 ffffffff fffffffffffffffb\n");
}

TEST_CASE("element select on a multi-dim packed array is rejected") {
    constexpr std::string_view src = R"(
        module t;
            logic [3:0][7:0] x;
            initial $display("%h", x[1]);
        endmodule
    )";
    CHECK(reject(src).find("multi-dimensional packed array") != std::string::npos);
}

TEST_CASE("fork..join is rejected, not serialized") {
    constexpr std::string_view src = R"(
        module t;
            initial fork #10 $display("A"); #5 $display("B"); join
        endmodule
    )";
    CHECK(reject(src).find("fork..join blocks are not supported") != std::string::npos);
}

TEST_CASE("iff qualifier on an event control is rejected, not dropped") {
    constexpr std::string_view src = R"(
        module t;
            logic clk, en;
            always @(posedge clk iff en) $display("tick");
            initial begin en = 0; clk = 0; #1 clk = 1; #1 $finish; end
        endmodule
    )";
    CHECK(reject(src).find("iff qualifier") != std::string::npos);
}

TEST_CASE("behavior-bearing members are rejected, not dropped") {
    constexpr std::string_view specify = R"(
        module m(input a, output b);
            assign b = a;
            specify (a => b) = 5; endspecify
        endmodule
        module t;
            reg a; wire b;
            m u(a, b);
        endmodule
    )";
    CHECK(reject(specify).find("SpecifyBlock") != std::string::npos);
}
