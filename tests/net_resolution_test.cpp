// Multi-driver net resolution (LRM 6.6.1, wire/tri, no drive strengths): the
// lattice join z ⊑ {0,1} ⊑ x. Whole-net overlapping drivers each stage into a
// private slot and a WireRes resolver folds them into the net. Every expected
// value below was cross-checked against iverilog 13 (a full net-resolution
// oracle) on the same source.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);
    return r;
}

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_ok(source);

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

} // namespace

// -- Runtime resolution values (oracle: iverilog 13) --

TEST_CASE("tri-state bus: z identity, agreement, conflict") {
    const std::string out = run_capture(R"(
        module top;
          logic a_en, b_en;
          logic [3:0] a_val, b_val;
          wire  [3:0] bus;
          assign bus = a_en ? a_val : 4'bz;
          assign bus = b_en ? b_val : 4'bz;
          initial begin
            a_en=0; b_en=0; a_val=4'h5; b_val=4'hA; #1; $display("%b", bus);
            a_en=1; #1;                                  $display("%b", bus);
            a_en=0; b_en=1; #1;                          $display("%b", bus);
            a_en=1; b_en=1; #1;                          $display("%b", bus);
            a_val=4'hC; b_val=4'h8; #1;                  $display("%b", bus);
            $finish;
          end
        endmodule
    )");
    // none=zzzz, a=0101, b=1010, conflict 5vsA=xxxx, C(1100)vs8(1000)=1x00.
    CHECK(out == "zzzz\n0101\n1010\nxxxx\n1x00\n");
}

TEST_CASE("three tri-state drivers fold left through WireRes") {
    const std::string out = run_capture(R"(
        module top;
          logic [1:0] sel;
          logic [7:0] d0, d1, d2;
          wire  [7:0] bus;
          assign bus = (sel==0) ? d0 : 8'bz;
          assign bus = (sel==1) ? d1 : 8'bz;
          assign bus = (sel==2) ? d2 : 8'bz;
          initial begin
            d0=8'h11; d1=8'h22; d2=8'h44;
            sel=0; #1; $display("%h", bus);
            sel=1; #1; $display("%h", bus);
            sel=2; #1; $display("%h", bus);
            sel=3; #1; $display("%h", bus);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "11\n22\n44\nzz\n");
}

TEST_CASE("two definite drivers with no z conflict to x, agree to the value") {
    const std::string out = run_capture(R"(
        module top;
          logic p, q;
          wire  cw;
          assign cw = p;
          assign cw = q;
          initial begin
            p=0; q=0; #1; $display("%b", cw);
            p=1; q=1; #1; $display("%b", cw);
            p=1; q=0; #1; $display("%b", cw);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0\n1\nx\n");
}

TEST_CASE("two submodule outputs resolve on one parent bus") {
    const std::string out = run_capture(R"(
        module drv(input en, input [3:0] d, output [3:0] o);
          assign o = en ? d : 4'bz;
        endmodule
        module top;
          logic e0, e1;
          logic [3:0] d0, d1;
          wire  [3:0] bus;
          drv u0(.en(e0), .d(d0), .o(bus));
          drv u1(.en(e1), .d(d1), .o(bus));
          initial begin
            d0=4'h3; d1=4'hC;
            e0=0; e1=0; #1; $display("%b", bus);
            e0=1; e1=0; #1; $display("%b", bus);
            e0=0; e1=1; #1; $display("%b", bus);
            e0=1; e1=1; #1; $display("%b", bus);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "zzzz\n0011\n1100\nxxxx\n");
}

TEST_CASE("a single-driver net keeps the plain-store path") {
    // The common case: one driver, no resolver, plain store propagates.
    const std::string out = run_capture(R"(
        module top;
          logic a;
          wire  w;
          assign w = a;
          initial begin a=1; #1; $display("%b", w); $finish; end
        endmodule
    )");
    CHECK(out == "1\n");
}

TEST_CASE("wand resolves wired-AND, wor resolves wired-OR") {
    const std::string out = run_capture(R"(
        module top;
          logic e0, e1;
          logic v0, v1;
          wand wa;
          wor  wo;
          assign wa = e0 ? v0 : 1'bz;
          assign wa = e1 ? v1 : 1'bz;
          assign wo = e0 ? v0 : 1'bz;
          assign wo = e1 ? v1 : 1'bz;
          initial begin
            e0=0; e1=0; v0=0; v1=0; #1; $display("%b %b", wa, wo); // z z
            e0=1; v0=1;             #1; $display("%b %b", wa, wo); // 1 1
            v0=0;                   #1; $display("%b %b", wa, wo); // 0 0
            e1=1; v0=0; v1=1;       #1; $display("%b %b", wa, wo); // 0 1
            v0=1; v1=1;             #1; $display("%b %b", wa, wo); // 1 1
            v0=1; v1=1'bx;          #1; $display("%b %b", wa, wo); // x 1
            v0=0;                   #1; $display("%b %b", wa, wo); // 0 x
            $finish;
          end
        endmodule
    )");
    // wand: 0 dominates, x otherwise, z identity. wor: 1 dominates.
    CHECK(out == "z z\n1 1\n0 0\n0 1\n1 1\nx 1\n0 x\n");
}

TEST_CASE("triand/trior synonyms fold, single-driver wand takes the plain path") {
    const std::string out = run_capture(R"(
        module top;
          logic [3:0] a, b, c;
          triand [3:0] ta;
          trior  [3:0] to;
          wand  sd;
          assign ta = a; assign ta = b; assign ta = c;
          assign to = a; assign to = b; assign to = c;
          assign sd = a[0];
          initial begin
            a=4'b1100; b=4'b1010; c=4'b1111; #1; $display("%b %b %b", ta, to, sd);
            a=4'b0011;                        #1; $display("%b %b %b", ta, to, sd);
            $finish;
          end
        endmodule
    )");
    // ta = a&b&c, to = a|b|c, sd = a[0] (one driver, no fold).
    CHECK(out == "1000 1111 0\n0010 1111 1\n");
}

TEST_CASE("overlapping partial-select drivers resolve per bit") {
    // Driver A drives bits [5:0], driver B drives [7:2] -> [5:2] overlap; the
    // undriven bits of each slot carry Z (the identity) so a bit resolves only
    // across the drivers that reach it.
    const std::string out = run_capture(R"(
        module top;
          logic [7:0] va, vb;
          logic ea, eb;
          wire  [7:0] bus;
          assign bus[5:0] = ea ? va[5:0] : 6'bz;
          assign bus[7:2] = eb ? vb[7:2] : 6'bz;
          initial begin
            va = 8'hFF; vb = 8'h00;
            ea=0; eb=0; #1; $display("%b", bus); // zzzzzzzz
            ea=1; eb=0; #1; $display("%b", bus); // A: [5:0]=1, [7:6]=z
            ea=0; eb=1; #1; $display("%b", bus); // B: [7:2]=0, [1:0]=z
            ea=1; eb=1; #1; $display("%b", bus); // [7:6]=0, [5:2]=1vs0=x, [1:0]=1
            $finish;
          end
        endmodule
    )");
    CHECK(out == "zzzzzzzz\nzz111111\n000000zz\n00xxxx11\n");
}

TEST_CASE("a whole-net driver and a partial-select driver resolve together") {
    const std::string out = run_capture(R"(
        module top;
          logic [7:0] vw, va;
          logic ew, ea;
          wire  [7:0] mix;
          assign mix      = ew ? vw : 8'bz;
          assign mix[3:0] = ea ? va[3:0] : 4'bz;
          initial begin
            vw = 8'hA5; va = 8'hFF;
            ew=1; ea=0; #1; $display("%b", mix); // whole = A5
            ew=1; ea=1; #1; $display("%b", mix); // [7:4]=1010, [3:0]=0101 vs 1111 -> x1x1
            $finish;
          end
        endmodule
    )");
    CHECK(out == "10100101\n1010x1x1\n");
}

TEST_CASE("wand/wor fold partial drivers per bit") {
    const std::string out = run_capture(R"(
        module top;
          logic vc, vb;
          wand [3:0] wa;
          wor  [3:0] wo;
          assign wa = {vc,vc,vc,vc}; assign wa[0] = vb; // bit0: vc wand vb
          assign wo = {vb,vb,vb,vb}; assign wo[0] = vc; // bit0: vb wor  vc
          initial begin
            vc=1; vb=0; #1; $display("%b %b", wa, wo); // wa: 1110, wo: 0001
            $finish;
          end
        endmodule
    )");
    CHECK(out == "1110 0001\n");
}

TEST_CASE("tri0 pulls down, tri1 pulls up, drivers override the pull") {
    const std::string out = run_capture(R"(
        module top;
          logic ea, eb, va, vb;
          tri0 t0;
          tri1 t1;
          assign t0 = ea ? va : 1'bz;
          assign t0 = eb ? vb : 1'bz;
          assign t1 = ea ? va : 1'bz;
          assign t1 = eb ? vb : 1'bz;
          initial begin
            ea=0; eb=0; va=1; vb=0; #1; $display("%b %b", t0, t1); // no driver -> 0 1
            ea=1;                   #1; $display("%b %b", t0, t1); // va=1     -> 1 1
            va=0;                   #1; $display("%b %b", t0, t1); // va=0     -> 0 0
            ea=1; eb=1; va=0; vb=1; #1; $display("%b %b", t0, t1); // 0 vs 1  -> x x
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0 1\n1 1\n0 0\nx x\n");
}

TEST_CASE("a driverless tri0/tri1 net holds its pull value") {
    const std::string out = run_capture(R"(
        module top;
          tri0 [3:0] a;
          tri1 [3:0] b;
          initial begin #1; $display("%b %b", a, b); $finish; end
        endmodule
    )");
    CHECK(out == "0000 1111\n");
}

TEST_CASE("a partial driver on a tri0 vector pulls the undriven bits") {
    const std::string out = run_capture(R"(
        module top;
          logic en;
          tri0 [3:0] bus;
          assign bus[1:0] = en ? 2'b11 : 2'bz;
          initial begin
            en=0; #1; $display("%b", bus); // all pulled: 0000
            en=1; #1; $display("%b", bus); // [1:0]=11 driven, [3:2] pulled: 0011
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0000\n0011\n");
}

// A gate primitive driving a multiply-driven net stages into a per-driver slot
// and folds through the wire resolver, like a continuous assign (ivtest
// many_drivers). A buf treats a z input as x (LRM Table 28-3), so an all-z input
// blends to x, not z. Oracle: iverilog 13.
TEST_CASE("gate primitives resolve as drivers of a multiply-driven net") {
    const std::string out = run_capture(R"(
        module top;
          reg [2:0] in;
          wire      out;
          buf (out, in[0]);
          buf (out, in[1]);
          buf (out, in[2]);
          initial begin
            in = 3'b000; #1; $display("%b", out); // all agree 0
            in = 3'b111; #1; $display("%b", out); // all agree 1
            in = 3'b010; #1; $display("%b", out); // 0 vs 1 conflict -> x
            in = 3'bzzz; #1; $display("%b", out); // buf(z)=x, all x -> x
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0\n1\nx\nx\n");
}

// -- Loud-failure guards for the shapes V2 does not resolve --

TEST_CASE("an unpacked-array-element driver of a resolved net fails loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [7:0] a, input [7:0] b);
            wire [7:0] mem [0:3];
            assign mem[0] = a;
            assign mem[0] = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("unpacked-array or unpacked-struct multiply-driven net") != std::string::npos);
}
