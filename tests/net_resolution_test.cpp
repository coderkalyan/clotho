// Multi-driver net resolution (LRM 6.6.1, wire/tri, no drive strengths): the
// lattice join z ⊑ {0,1} ⊑ x. Whole-net overlapping drivers each stage into a
// private slot and a WireRes resolver folds them into the net. Every expected
// value below was cross-checked against iverilog 13 (a full net-resolution
// oracle) on the same source.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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

// Partial drivers of a multiply-driven net whose region is a multi-dimensional
// packed element select (`t[1]`, a whole W-bit sub-vector) or a constant-base
// indexed part-select (`a[base +: w]` / `a[base -: w]`) — net_region_of /
// packed_bit_range resolve both to a static bit band. Cross-checked vs iverilog.
TEST_CASE("multi-dim element and indexed part-select partial drivers resolve") {
    const std::string out = run_capture(R"(
        module top;
          logic ea, eb;
          logic [7:0] va, vb;
          wire [2:0][7:0] t;
          assign t    = eb ? {vb,vb,vb} : 24'bz;  // whole-net driver
          assign t[1] = ea ? va : 8'bz;           // multi-bit element select (k>0)
          wire [3:0] a;
          assign a[0+:2] = ea ? va[1:0] : 2'bz;   // indexed part-select (+:)
          assign a[3-:2] = eb ? vb[1:0] : 2'bz;   // indexed part-select (-:)
          initial begin
            va = 8'hAA; vb = 8'h55;
            ea=0; eb=1; #1; $display("%h %b", t, a);
            ea=1; eb=0; #1; $display("%h %b", t, a);
            ea=1; eb=1; #1; $display("%h %b", t, a);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "555555 01zz\nzzaazz zz10\n55xx55 0110\n");
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

// The Z background of a partial driver and the tri0/tri1 pull are pooled
// 4-state constants, and a pooled constant's planes are `ir::plane_bytes(w)`
// apart (1, 2, 4, 8 bytes, then whole 64-bit words), not `(w + 7) / 8`. The two
// differ for w = 17..24, 33..56 and every w above 64 that is not a multiple of
// 64, where a misplaced xz plane turned the high bits of the constant into a
// definite 0 (or the pull value). Each net below has a Z or pull segment of such
// a width. Oracle: iverilog 13 prints these lines for this source.
TEST_CASE("Z and pull constants of every plane stride keep their z bits") {
    const std::string out = run_capture(R"(
        module top;
          wire [31:0] w;
          assign w      = 32'hffffffff;
          assign w[20]  = 1'b0;          // only bit 20 conflicts: 1 vs 0
          tri1 [19:0] p;                 // driverless: all pulled up
          tri0 [39:0] q;                 // driverless: all pulled down
          tri1 [23:0] c;
          assign c[3:0] = 4'b0;          // [23:4] pulled up
          wire [17:0] d;
          assign d[0]   = 1'b1;
          assign d[0]   = 1'b1;          // two drivers on bit 0, the rest z
          wire [71:0] e;
          assign e      = 72'bz;
          assign e[40]  = 1'b1;          // z segments of 40 and 31 bits
          wand [23:0] a;
          assign a      = 24'hffffff;
          assign a[3:0] = 4'h0;          // 1 & 0 in [3:0], 1 & z above
          initial begin
            #1;
            $display("%b", w);
            $display("%b", p);
            $display("%b", q);
            $display("%b", c);
            $display("%b", d);
            $display("%b", e);
            $display("%h", a);
          end
        endmodule
    )");
    CHECK(out == "11111111111x11111111111111111111\n"
                 "11111111111111111111\n"
                 "0000000000000000000000000000000000000000\n"
                 "111111111111111111110000\n"
                 "zzzzzzzzzzzzzzzzz1\n"
                 "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz1zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n"
                 "fffff0\n");
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

// A cmos is two drivers, not one (LRM 28.9 defines it as an nmos plus a pmos
// sharing one output), so on a net that already carries a third driver each half
// gets its own slot and all three fold through the wire resolver — exactly as if
// the two halves had been written out longhand. This is also the check that the
// Classify pass records both halves: with only one recorded driver the net would
// never resolve and the halves would stomp each other. Oracle: iverilog 13.
TEST_CASE("a cmos switch shares a resolved net with another driver") {
    const std::string out = run_capture(R"(
        module top;
          logic d, nc, pc, a, en;
          wire  w;
          cmos   c0(w, d, nc, pc);
          bufif1 b0(w, a, en);
          initial begin
            d=0; nc=0; pc=1; a=1; en=0; #1; $display("%b", w); // cmos off, bufif off -> z
            pc=0;                       #1; $display("%b", w); // pmos half passes 0
            en=1;                       #1; $display("%b", w); // 0 vs 1 -> x
            pc=1;                       #1; $display("%b", w); // cmos off, bufif drives 1
            nc=1; pc=0; d=1;            #1; $display("%b", w); // all three agree on 1
            $finish;
          end
        endmodule
    )");
    CHECK(out == "z\n0\nx\n1\n1\n");
}

// -- Drive strengths (LRM 6.6.1, 28.11) --
//
// A net with any non-default drive strength resolves through the strength fold
// (StrengthRes) instead of WireRes: per bit the strongest side wins, equal top
// strength on both sides is X, no driver is Z. At all-Strong it degenerates to
// WireRes. (iverilog does not model strengths.)

TEST_CASE("strong beats weak, either polarity") {
    const std::string out = run_capture(R"(
        module top;
          logic a, b;
          wire  w;
          assign (strong0, strong1) w = a;
          assign (weak0,   weak1)   w = b;
          initial begin
            a=1'b1; b=1'b0; #1 $display("%b", w); // strong 1 vs weak 0 -> 1
            a=1'b0; b=1'b1; #1 $display("%b", w); // strong 0 vs weak 1 -> 0
            a=1'bz; b=1'b1; #1 $display("%b", w); // z vs weak 1 -> 1
            a=1'bz; b=1'bz; #1 $display("%b", w); // nothing -> z
            $finish;
          end
        endmodule
    )");
    CHECK(out == "1\n0\n1\nz\n");
}

TEST_CASE("strength ladder: supply > strong > pull > weak") {
    const std::string out = run_capture(R"(
        module top;
          logic a, b;
          wire  wp, ws;
          assign (pull0,   pull1)   wp = a;   // pull 1
          assign (strong0, strong1) wp = b;   // strong 0 -> strong wins
          assign (supply0, supply1) ws = a;   // supply 1
          assign (strong0, strong1) ws = b;   // strong 0 -> supply wins
          initial begin
            a=1'b1; b=1'b0; #1 $display("%b %b", wp, ws); // 0 1
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0 1\n");
}

TEST_CASE("equal strength opposite drivers conflict to x") {
    const std::string out = run_capture(R"(
        module top;
          logic a, b;
          wire  ws, ww;
          assign (strong0, strong1) ws = a;
          assign (strong0, strong1) ws = b;
          assign (weak0,   weak1)   ww = a;
          assign (weak0,   weak1)   ww = b;
          initial begin
            a=1'b1; b=1'b0; #1 $display("%b %b", ws, ww); // x x
            $finish;
          end
        endmodule
    )");
    CHECK(out == "x x\n");
}

TEST_CASE("per-bit strength resolution over a vector") {
    const std::string out = run_capture(R"(
        module top;
          logic [3:0] a, b;
          wire  [3:0] w;
          assign (strong0, strong1) w = a;
          assign (weak0,   weak1)   w = b;
          initial begin
            a=4'b1100; b=4'b1010; #1 $display("%b", w); // strong per bit -> 1100
            $finish;
          end
        endmodule
    )");
    CHECK(out == "1100\n");
}

// Open-drain (drive 0 strong, float 1) and open-source (float 0, drive 1 strong)
// use an asymmetric (highz on one side) strength with definite values — the
// common asymmetric use.
TEST_CASE("open-drain and open-source asymmetric strengths") {
    const std::string out = run_capture(R"(
        module top;
          logic a, b;
          wire  od, os;
          assign (strong0, highz1) od = a;   // open drain
          assign (strong0, highz1) od = b;
          assign (highz0, strong1) os = a;   // open source
          assign (weak0,  weak1)   os = b;
          initial begin
            a=1'b0; b=1'b1; #1 $display("%b", od); // a drives 0, b floats -> 0
            a=1'b1; b=1'b1; #1 $display("%b", od); // both float -> z
            a=1'b1; b=1'b0; #1 $display("%b", os); // a strong 1 vs b weak 0 -> 1
            a=1'b0; b=1'b0; #1 $display("%b", os); // a floats, b weak 0 -> 0
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0\nz\n1\n0\n");
}

// A tri0/tri1 pull is a Pull-strength driver of 0/1, so it competes in the fold:
// a Weak (< Pull) driver loses to it, a Strong (> Pull) driver beats it. This is
// the corner a plain post-fold pull (fills Z only) gets wrong.
TEST_CASE("tri0/tri1 pull competes on strength with explicit drivers") {
    const std::string out = run_capture(R"(
        module top;
          logic a;
          tri0 t0w, t0s;
          tri1 t1w, t1s;
          assign (weak0,   weak1)   t0w = a;
          assign (strong0, strong1) t0s = a;
          assign (weak0,   weak1)   t1w = a;
          assign (strong0, strong1) t1s = a;
          initial begin
            a=1'b1; #1 $display("%b%b%b%b", t0w, t0s, t1w, t1s); // 0 1 1 1
            a=1'b0; #1 $display("%b%b%b%b", t0w, t0s, t1w, t1s); // 0 0 1 0
            a=1'bz; #1 $display("%b%b%b%b", t0w, t0s, t1w, t1s); // 0 0 1 1
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0111\n0010\n0011\n");
}

// pullup / pulldown (LRM 28.5): a lone permanent Pull-strength driver of 1 / 0.
// A stronger (strong/supply) driver wins; when every driver is off (z) the net
// holds the pull value.
TEST_CASE("pullup and pulldown compete on strength") {
    const std::string out = run_capture(R"(
        module top;
          logic en, d;
          wire wu, wd;
          pullup   (wu);
          pulldown (wd);
          assign wu = en ? d : 1'bz;
          assign wd = en ? d : 1'bz;
          initial begin
            en=0;      #1 $display("%b%b", wu, wd); // off: pull -> 1 0
            en=1; d=0; #1 $display("%b%b", wu, wd); // strong 0 beats pullup, agrees pulldown -> 0 0
            en=1; d=1; #1 $display("%b%b", wu, wd); // agrees pullup, strong 1 beats pulldown -> 1 1
            $finish;
          end
        endmodule
    )");
    CHECK(out == "10\n00\n11\n");
}

// supply0 / supply1 nets (LRM 6.6.5): a permanent Supply-strength (level 4)
// driver of 0 / 1 that beats even a Strong driver.
TEST_CASE("supply0/supply1 nets hold their value and beat strong drivers") {
    const std::string out = run_capture(R"(
        module top;
          logic d;
          supply0 gnd;
          supply1 vdd;
          wire w;
          assign (strong0, strong1) vdd = d;   // strong0 loses to supply1 -> 1
          assign (supply0, supply1) w = 1'b0;  // supply0 vs...
          assign (strong0, strong1) w = 1'b1;  // ...strong1 -> supply wins -> 0
          initial begin
            d = 1'b0; #1 $display("%b%b%b", gnd, vdd, w); // 0 1 0
            $finish;
          end
        endmodule
    )");
    CHECK(out == "010\n");
}

TEST_CASE("drive strengths on a wand net fail loudly") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b);
            wand w;
            assign (weak0, weak1) w = a;
            assign (weak0, weak1) w = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("drive strengths on a wand/wor net") != std::string::npos);
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
    CHECK(r.diagnostic_text().find("unpacked-array or unpacked-struct multiply-driven net") != std::string::npos);
}

// A partially-driven net (continuous assigns to disjoint constant sub-ranges —
// each bit has a single driver, so the net is NOT multiply-driven / resolved)
// powers its UNDRIVEN bits to z, not x: the partial-undriven mask refines the
// whole-signal undriven analysis per bit. Covers a 1-D vector, a multi-dim
// packed element, and a nested `v[i][bit]` select. Cross-checked vs iverilog.
TEST_CASE("undriven bits of a partially-driven net power on z, not x") {
    const std::string out = run_capture(R"(
        module top;
          wire [7:0] w;
          assign w[1:0] = 2'h3;
          assign w[5:4] = 2'h2;          // bits 3:2, 7:6 undriven -> z
          wire [1:0][7:0] v;
          assign v[0][0] = 1'b1;
          assign v[0][3] = 1'b1;         // v[0] other bits, v[1] whole -> z
          initial begin
            #1; $display("%b %b %b", w, v[0], v[1]);
          end
        endmodule
    )");
    CHECK(out == "zz10zz11 zzzz1zz1 zzzzzzzz\n");
}

// The undriven bits of a partially driven net read as z, whatever reads them.
// The may-z analysis has to know the net can hold z, or a reader proven never-z
// drops its z word and stores x: a continuous, delayed, concatenating,
// replicating, sign-casting, blocking, nonblocking or resolving reader is checked
// below (ivtest br_gh99m..w, pr2837451). Oracle: iverilog 13 prints these lines
// for this source.
TEST_CASE("readers of a partially-driven net keep its undriven bits at z") {
    const std::string out = run_capture(R"(
        module top;
          wire [7:0] a;
          assign a[3:0] = 4'd2;          // a[7:4] undriven -> z
          wire [7:0]  cont = a;
          wire [7:0]  dly;
          assign #1 dly = a;
          wire [9:0]  cat = {1'b0, a, 1'b1};
          wire [15:0] rep = {2{a}};
          wire [9:0]  sgn = $signed(a);
          logic [7:0] blk, nba;
          wire [1:0]  c;
          assign c[1] = 1'b1;            // c[0] undriven -> z
          wire e;
          assign e = c[0];               // z resolved with 0 -> 0
          assign e = 1'b0;
          initial begin
            #2;
            blk = a;
            nba <= a;
            #1;
            $display("%b %b %b", a, cont, dly);
            $display("%b %b %b", cat, rep, sgn);
            $display("%b %b", blk, nba);
            $display("%b %b", c, e);
          end
        endmodule
    )");
    CHECK(out == "zzzz0010 zzzz0010 zzzz0010\n"
                 "0zzzz00101 zzzz0010zzzz0010 zzzzzz0010\n"
                 "zzzz0010 zzzz0010\n"
                 "1z 0\n");
}

// -- 2-state driver regions --
//
// A driver slot is cut from the NET's type, so it is always 4-state and always
// holds the Z background a partial driver needs: LRM 6.7.1 restricts a built-in
// net type's data type to a 4-state integral type (slang rejects `wire bit [7:0]
// w;` outright), and a user-defined nettype — the only way to declare a 2-state
// net, LRM 6.6.7 b — is refused at signal collection. What a design CAN write is
// a 2-state MEMBER of a 4-state packed net, whose own 2-state view converts each
// driver's value (LRM 6.11.2: x and z become 0) before it reaches the resolver.
//
// Line 3 is the discriminating one: the `lo` driver's x converts to 0 and then
// AGREES with `hi`'s 0, so bit 0 resolves 0 — an x reaching the resolver would
// make it x, as line 2 and line 4 show for a genuine 0-vs-1 conflict.
TEST_CASE("a 2-state member of a 4-state packed net converts before resolving") {
    const std::string out = run_capture(R"(
        module top;
          logic a, b, c, d;
          wire union packed { bit [3:0] lo; logic [3:0] hi; } u;
          assign u.lo[0] = a & b;
          assign u.hi[0] = c & d;
          initial begin
            a=1; b=1; c=1; d=1; #1; $display("%b", u);
            b=0;                #1; $display("%b", u);
            a=1'bx; b=1; d=0;   #1; $display("%b", u);
            d=1;                #1; $display("%b", u);
            $finish;
          end
        endmodule
    )");
    // u[3:1] undriven -> z. bit 0: 1 vs 1 -> 1; 0 vs 1 -> x; 0 vs 0 -> 0; 0 vs 1 -> x.
    CHECK(out == "zzz1\nzzzx\nzzz0\nzzzx\n");
}

// The 2-state analogue of a partially driven net is a 2-state VARIABLE, which LRM
// 10.3.2 allows one continuous assign to drive: disjoint constant sub-ranges each
// have a single driver, so nothing is multiply driven and no slot or resolver is
// involved. Oracle: iverilog 13 prints `v=a5` for this source.
TEST_CASE("disjoint partial continuous assigns to a 2-state variable") {
    const std::string out = run_capture(R"(
        module top;
          logic [3:0] x, y;
          bit [7:0] v;
          assign v[3:0] = x;
          assign v[7:4] = y;
          initial begin
            x=4'h5; y=4'hA; #1; $display("v=%h", v);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "v=a5\n");
}
