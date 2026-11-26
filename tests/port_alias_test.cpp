#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

// An input or output port the child definition allows to be bound has no storage
// of its own; the parent's signal, or a parent-owned temporary, is the storage
// (docs/design/modular-compilation.md, "The boundary"). Copy aliases then reach
// through such ports. These cases pin what a design observes across port
// connections of every kind. The expected outputs follow the LRM clauses each case
// cites and are cross-checked against iverilog 13 except where noted.

namespace {

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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

} // namespace

TEST_CASE("port alias: variable input and output ports on parent variables") {
    // LRM 23.3.3, 23.3.3.2, 23.2.2.3: a variable port connection is an implied
    // continuous assignment. Values are read on both sides of the port and through
    // tb.u.in and u.out. Prints come one unit after each change, when a
    // continuous-assignment port and a merged variable hold the same settled
    // values.
    const std::string out = run_capture(R"(
        module child(input var logic [7:0] in, output logic [7:0] out);
          always @(in) out = in + 8'd1;
        endmodule

        module tb;
          logic [7:0] a;
          logic [7:0] b;
          child u(.in(a), .out(b));
          initial begin
            #1 a = 8'h10;
            #1 $display("t=%0t a=%h b=%h u.in=%h u.out=%h", $time, a, b, tb.u.in, u.out);
            a = 8'hff;
            #1 $display("t=%0t a=%h b=%h u.in=%h u.out=%h", $time, a, b, tb.u.in, u.out);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 a=10 b=11 u.in=10 u.out=11\n"
                 "t=3 a=ff b=00 u.in=ff u.out=00\n");
}

TEST_CASE("port alias: net input and output ports on parent nets carry z") {
    // LRM 23.3.3, 23.3.3.3, 23.3.3.7, 6.6.1: nets of the same type may be merged.
    // Every net has one driver, so a merged net and a continuous-assignment port
    // give the same values.
    const std::string out = run_capture(R"(
        module child(input wire [7:0] in, output wire [7:0] out);
          assign out = ~in;
        endmodule

        module tb;
          logic [7:0] drv;
          wire [7:0] a, b;
          assign a = drv;
          child u(.in(a), .out(b));
          initial begin
            #1 drv = 8'h0f;
            #1 $display("t=%0t a=%h b=%h u.in=%h u.out=%h", $time, a, b, tb.u.in, u.out);
            drv = 8'bzzzz0101;
            #1 $display("t=%0t a=%h b=%h u.in=%h u.out=%h", $time, a, b, tb.u.in, u.out);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 a=0f b=f0 u.in=0f u.out=f0\n"
                 "t=3 a=z5 b=xa u.in=z5 u.out=xa\n");
}

TEST_CASE("port alias: a parent variable on a net input port and a net output port on a parent variable") {
    // LRM 23.3.3, 23.3.3.2, 23.3.3.3: an output net port may connect to a variable
    // through an implied continuous assignment. Single drivers everywhere.
    const std::string out = run_capture(R"(
        module child(input wire [7:0] in, output wire [7:0] out);
          assign out = in + 8'd3;
        endmodule

        module tb;
          logic [7:0] v;
          logic [7:0] r;
          child u(.in(v), .out(r));
          initial begin
            #1 v = 8'h20;
            #1 $display("t=%0t v=%h r=%h u.in=%h u.out=%h", $time, v, r, tb.u.in, u.out);
            v = 8'bz;
            #1 $display("t=%0t v=%h r=%h u.in=%h u.out=%h", $time, v, r, tb.u.in, u.out);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 v=20 r=23 u.in=20 u.out=23\n"
                 "t=3 v=zz r=xx u.in=zz u.out=xx\n");
}

TEST_CASE("port alias: a parent net on a variable input port keeps z") {
    // LRM 23.2.2.3, 23.3.3.2: the 4-state input variable stores z unchanged, and
    // the variable output drives the parent net. The expectation is the LRM
    // reading; iverilog coerces the input port to inout because a net is connected
    // to it, and reads x (DISAGREEMENTS.md).
    const std::string out = run_capture(R"(
        module child(input var logic [7:0] in, output logic [7:0] out);
          always @(in) out = {in[3:0], in[7:4]};
        endmodule

        module tb;
          logic [7:0] d;
          wire [7:0] w;
          wire [7:0] o;
          assign w = d;
          child u(.in(w), .out(o));
          initial begin
            #1 d = 8'h12;
            #1 $display("t=%0t w=%h o=%h u.in=%h u.out=%h", $time, w, o, tb.u.in, u.out);
            d = 8'bz;
            #1 $display("t=%0t w=%h o=%h u.in=%h u.out=%h", $time, w, o, tb.u.in, u.out);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 w=12 o=21 u.in=12 u.out=21\n"
                 "t=3 w=zz o=zz u.in=zz u.out=zz\n");
}

TEST_CASE("port alias: one clock fanned out to eight instances counts its edges") {
    // LRM 23.3.3, 9.4.2, 4.7: the counters ignore time 0, where a port net may or
    // may not make a z/x->0 transition. d changes only while clk is low and q is
    // assigned with an NBA.
    const std::string out = run_capture(R"(
        module regm(input clk, input [7:0] d, output logic [7:0] q);
          int npos = 0, nneg = 0;
          always @(posedge clk) if ($time > 0) begin npos++; q <= d; end
          always @(negedge clk) if ($time > 0) nneg++;
        endmodule

        module tb;
          logic clk = 1'b0;
          logic [7:0] d;
          wire [7:0] q [8];
          regm u0(.clk(clk), .d(d), .q(q[0]));
          regm u1(.clk(clk), .d(d), .q(q[1]));
          regm u2(.clk(clk), .d(d), .q(q[2]));
          regm u3(.clk(clk), .d(d), .q(q[3]));
          regm u4(.clk(clk), .d(d), .q(q[4]));
          regm u5(.clk(clk), .d(d), .q(q[5]));
          regm u6(.clk(clk), .d(d), .q(q[6]));
          regm u7(.clk(clk), .d(d), .q(q[7]));
          initial begin
            d = 8'h00;
            repeat (5) begin
              #5 clk = 1'b1;
              #5 clk = 1'b0;
              d = d + 8'h01;
            end
            #1;
            $display("u0 pos=%0d neg=%0d q=%h parent_q=%h", u0.npos, u0.nneg, u0.q, q[0]);
            $display("u1 pos=%0d neg=%0d q=%h parent_q=%h", u1.npos, u1.nneg, u1.q, q[1]);
            $display("u2 pos=%0d neg=%0d q=%h parent_q=%h", u2.npos, u2.nneg, u2.q, q[2]);
            $display("u3 pos=%0d neg=%0d q=%h parent_q=%h", u3.npos, u3.nneg, u3.q, q[3]);
            $display("u4 pos=%0d neg=%0d q=%h parent_q=%h", u4.npos, u4.nneg, u4.q, q[4]);
            $display("u5 pos=%0d neg=%0d q=%h parent_q=%h", u5.npos, u5.nneg, u5.q, q[5]);
            $display("u6 pos=%0d neg=%0d q=%h parent_q=%h", u6.npos, u6.nneg, u6.q, q[6]);
            $display("u7 pos=%0d neg=%0d q=%h parent_q=%h", u7.npos, u7.nneg, u7.q, q[7]);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "u0 pos=5 neg=5 q=04 parent_q=04\n"
                 "u1 pos=5 neg=5 q=04 parent_q=04\n"
                 "u2 pos=5 neg=5 q=04 parent_q=04\n"
                 "u3 pos=5 neg=5 q=04 parent_q=04\n"
                 "u4 pos=5 neg=5 q=04 parent_q=04\n"
                 "u5 pos=5 neg=5 q=04 parent_q=04\n"
                 "u6 pos=5 neg=5 q=04 parent_q=04\n"
                 "u7 pos=5 neg=5 q=04 parent_q=04\n");
}

TEST_CASE("port alias: an input chain over three levels with mixed port kinds") {
    // LRM 23.3.3, 23.3.3.2, 23.3.3.3, 23.2.2.3: a variable port, an explicit wire
    // port and a default net port carry z down the chain. Single drivers.
    const std::string out = run_capture(R"(
        module c3(input logic [7:0] i);
          wire [7:0] y;
          assign y = i * 8'd2;
        endmodule
        module b3(input wire [7:0] i);
          c3 uc(.i(i));
        endmodule
        module a3(input var logic [7:0] i);
          b3 ub(.i(i));
        endmodule

        module tb;
          logic [7:0] x;
          a3 u(.i(x));
          task show;
            $display("t=%0t x=%h a.i=%h b.i=%h c.i=%h c.y=%h", $time, x, u.i, u.ub.i, u.ub.uc.i, u.ub.uc.y);
          endtask
          initial begin
            #1 x = 8'h05;
            #1 show;
            x = 8'h80;
            #1 show;
            x = 8'bz;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 x=05 a.i=05 b.i=05 c.i=05 c.y=0a\n"
                 "t=3 x=80 a.i=80 b.i=80 c.i=80 c.y=00\n"
                 "t=4 x=zz a.i=zz b.i=zz c.i=zz c.y=xx\n");
}

TEST_CASE("port alias: a three-level chain down through inputs and back up through outputs") {
    // LRM 23.3.3, 23.3.3.2, 23.3.3.3: each level uses a different port kind, and a
    // variable output driven by a child output is an implied continuous
    // assignment. Single drivers.
    const std::string out = run_capture(R"(
        module c7(input [7:0] k, output logic [7:0] o);
          always @(k) o = k ^ 8'h5a;
        endmodule
        module b7(input [7:0] k, output wire [7:0] o);
          c7 uc(.k(k), .o(o));
        endmodule
        module a7(input var logic [7:0] k, output logic [7:0] o);
          b7 ub(.k(k), .o(o));
        endmodule

        module tb;
          logic [7:0] k;
          wire [7:0] r;
          a7 u(.k(k), .o(r));
          task show;
            $display("t=%0t k=%h r=%h a.o=%h b.o=%h c.o=%h c.k=%h", $time, k, r, u.o, u.ub.o, u.ub.uc.o, u.ub.uc.k);
          endtask
          initial begin
            #1 k = 8'h00;
            #1 show;
            k = 8'hff;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 k=00 r=5a a.o=5a b.o=5a c.o=5a c.k=00\n"
                 "t=3 k=ff r=a5 a.o=a5 b.o=a5 c.o=a5 c.k=ff\n");
}

TEST_CASE("port alias: passthrough children chained between siblings and through a wrapper") {
    // LRM 23.3.3, 23.3.3.2: `assign o = i;` children feed one another, and one
    // passthrough has variable ports. The expectation is the LRM reading; iverilog
    // coerces the net-connected input variable port to inout and reads x
    // (DISAGREEMENTS.md, as in c04).
    const std::string out = run_capture(R"(
        module pass(input [7:0] i, output [7:0] o);
          assign o = i;
        endmodule
        module passv(input var logic [7:0] i, output logic [7:0] o);
          assign o = i;
        endmodule
        module inc(input [7:0] i, output [7:0] o);
          assign o = i + 8'd1;
        endmodule
        module wrap(input [7:0] i, output [7:0] o);
          pass p(.i(i), .o(o));
        endmodule
        module wrap2(input [7:0] i, output [7:0] o);
          wrap u(.i(i), .o(o));
        endmodule

        module tb;
          logic [7:0] s;
          wire [7:0] w1, w2, w3, w4;
          pass  p1(.i(s),  .o(w1));
          passv p2(.i(w1), .o(w2));
          inc   p3(.i(w2), .o(w3));
          wrap2 q (.i(w3), .o(w4));
          task show;
            $display("t=%0t s=%h w1=%h w2=%h w3=%h w4=%h q.i=%h deep=%h", $time, s, w1, w2, w3, w4, q.i, q.u.p.o);
          endtask
          initial begin
            #1 s = 8'h10;
            #1 show;
            s = 8'hff;
            #1 show;
            s = 8'bz;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 s=10 w1=10 w2=10 w3=11 w4=11 q.i=11 deep=11\n"
                 "t=3 s=ff w1=ff w2=ff w3=00 w4=00 q.i=00 deep=00\n"
                 "t=4 s=zz w1=zz w2=zz w3=xx w4=xx q.i=xx deep=xx\n");
}

TEST_CASE("port alias: expression connections to input ports are sized by the port") {
    // LRM 23.3.3, 23.3.3.8, 10.7, 11.8.1: a port expression is evaluated as in an
    // assignment, so the port width is the context. The expectation is the LRM
    // reading; iverilog evaluates `a + 4'd1` at 4 bits before padding to the 5-bit
    // port and drops the carry (DISAGREEMENTS.md).
    const std::string out = run_capture(R"(
        module shown #(parameter int W = 8) (input [W-1:0] in);
        endmodule
        module showv #(parameter int W = 8) (input var logic [W-1:0] in);
        endmodule

        module tb;
          logic [3:0] a, b;
          logic [7:0] c;
          shown #(4) u_and (.in(a & b));
          showv #(5) u_add5(.in(a + 4'd1));
          shown #(4) u_add4(.in(a + 4'd1));
          shown #(8) u_cat (.in({a, b}));
          showv #(4) u_ps  (.in(c[3:0]));
          shown #(4) u_k   (.in(4'd5));
          task show;
            $display("t=%0t and=%h add5=%h add4=%h cat=%h ps=%h k=%h", $time,
                     u_and.in, u_add5.in, u_add4.in, u_cat.in, u_ps.in, u_k.in);
          endtask
          initial begin
            #1 a = 4'hf; b = 4'h3; c = 8'ha5;
            #1 show;
            a = 4'h6; b = 4'hc; c = 8'h3c;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 and=3 add5=10 add4=0 cat=f3 ps=5 k=5\n"
                 "t=3 and=4 add5=07 add4=7 cat=6c ps=c k=5\n");
}

TEST_CASE("port alias: outputs to a part-select and to concatenations of nets and variables") {
    // LRM 23.3.3.2, 23.3.3.3, 23.3.3: the other half of the part-selected net has
    // its own assign. Every bit has exactly one driver.
    const std::string out = run_capture(R"(
        module gen4(input [3:0] k, output [3:0] o);
          assign o = k;
        endmodule
        module gen8(input [7:0] k, output [7:0] o);
          assign o = k;
        endmodule
        module genv8(input [7:0] k, output logic [7:0] o);
          assign o = k;
        endmodule

        module tb;
          logic [3:0] k;
          logic [7:0] kk;
          wire [7:0] w;
          wire [2:0] x;
          wire [4:0] y;
          logic [3:0] vx, vy;
          assign w[7:4] = 4'ha;
          gen4  u1(.k(k),  .o(w[3:0]));
          gen8  u2(.k(kk), .o({x, y}));
          genv8 u3(.k(kk), .o({vx, vy}));
          task show;
            $display("t=%0t w=%h x=%h y=%h vx=%h vy=%h u1.o=%h u2.o=%h u3.o=%h", $time, w, x, y, vx, vy, u1.o, u2.o, u3.o);
          endtask
          initial begin
            #1 k = 4'h3; kk = 8'hb9;
            #1 show;
            k = 4'bz; kk = 8'h47;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 w=a3 x=5 y=19 vx=b vy=9 u1.o=3 u2.o=b9 u3.o=b9\n"
                 "t=3 w=az x=2 y=07 vx=4 vy=7 u1.o=z u2.o=47 u3.o=47\n");
}

TEST_CASE("port alias: width mismatches extend or truncate by the source") {
    // LRM 23.3.3, 23.3.3.8, 10.7, 11.8.1-11.8.2: one side is always a variable, so
    // the port must be a continuous assignment and 10.7 extension applies,
    // including an x sign bit.
    const std::string out = run_capture(R"(
        module wm(input [7:0] p8a, input [7:0] p8b, input [7:0] p8c, input [7:0] p8d,
                  input [3:0] p4,
                  output logic [7:0] o8, output logic [3:0] o4);
          assign o8 = 8'hc3;
          assign o4 = 4'h9;
        endmodule

        module tb;
          logic [3:0] n4;
          logic signed [3:0] s4, sx;
          logic [3:0] ux;
          logic [7:0] big8;
          wire [3:0] nar;
          wire [7:0] wid;
          wm u(.p8a(n4), .p8b(s4), .p8c(sx), .p8d(ux), .p4(big8), .o8(nar), .o4(wid));
          initial begin
            #1 n4 = 4'ha; s4 = 4'b1010; sx = 4'bx010; ux = 4'bx010; big8 = 8'ha5;
            #1 $display("p8a=%h p8b=%h p8c=%b p8d=%b p4=%h", u.p8a, u.p8b, u.p8c, u.p8d, u.p4);
            $display("nar=%h wid=%h u.o8=%h u.o4=%h", nar, wid, u.o8, u.o4);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "p8a=0a p8b=fa p8c=xxxxx010 p8d=0000x010 p4=5\n"
                 "nar=3 wid=09 u.o8=c3 u.o4=9\n");
}

TEST_CASE("port alias: two-state variable ports read x and z as 0") {
    // LRM 23.3.3, 23.3.3.2, 6.22.3, 6.11.2: assigning a 4-state value to a 2-state
    // variable turns x and z into 0. The 2-state ports use `var`, since a 2-state
    // net is not allowed (23.2.2.3).
    const std::string out = run_capture(R"(
        module two(input var bit [3:0] i2, input var int ii, output bit [3:0] o2);
          assign o2 = i2 + 4'd1;
        endmodule

        module tb;
          logic [3:0] v;
          logic [31:0] lv;
          wire [3:0] w;
          two u(.i2(v), .ii(lv), .o2(w));
          task show;
            $display("t=%0t v=%b u.i2=%b u.ii=%0d w=%b", $time, v, u.i2, u.ii, w);
          endtask
          initial begin
            #1 v = 4'b1x0z; lv = {28'bx, 4'b0101};
            #1 show;
            v = 4'bzzzz; lv = 32'bz;
            #1 show;
            v = 4'b0110; lv = 32'd77;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 v=1x0z u.i2=1000 u.ii=5 w=1001\n"
                 "t=3 v=zzzz u.i2=0000 u.ii=0 w=0001\n"
                 "t=4 v=0110 u.i2=0110 u.ii=77 w=0111\n");
}

TEST_CASE("port alias: signedness does not cross a port; extension follows the source") {
    // LRM 23.3.3.8, 10.7, 11.8.1-11.8.2: a port expression is treated as an
    // assignment, so the bit pattern is kept and a wider port is sign-extended
    // only from a signed source.
    const std::string out = run_capture(R"(
        module sg(input var logic [7:0] u, input var logic signed [7:0] sp,
                  input var logic signed [7:0] w1, input var logic signed [7:0] w2,
                  input var logic [7:0] w3,
                  output logic signed [7:0] so);
          wire neg = sp < 0;
          assign so = sp >>> 1;
        endmodule

        module tb;
          logic signed [7:0] s;
          logic [7:0] uu;
          logic signed [3:0] s4;
          logic [3:0] u4;
          logic [7:0] pu;
          sg x(.u(s), .sp(uu), .w1(s4), .w2(u4), .w3(s4), .so(pu));
          initial begin
            #1 s = -8'sd3; uu = 8'd250; s4 = -4'sd2; u4 = 4'he;
            #1 $display("s=%0d x.u=%0d uu=%0d x.sp=%0d x.neg=%b x.so=%0d pu=%0d", s, x.u, uu, x.sp, x.neg, x.so, pu);
            $display("x.w1=%0d x.w2=%0d x.w3=%0d", x.w1, x.w2, x.w3);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "s=-3 x.u=253 uu=250 x.sp=-6 x.neg=1 x.so=-3 pu=253\n"
                 "x.w1=-2 x.w2=14 x.w3=254\n");
}

TEST_CASE("port alias: unconnected ports read their defaults") {
    // LRM 23.3.3.2, 23.3.3.3, 23.3.2.2, 6.8 (Table 6-7): an unconnected net input
    // reads z, a 4-state variable input x and a 2-state variable input 0, for
    // `.in()`, omitted named ports and empty positional slots alike.
    const std::string out = run_capture(R"(
        module um(input wire [3:0] ni, input var logic [3:0] vi, input var bit [3:0] bi, output logic [3:0] o);
          assign o = 4'h7;
        endmodule

        module tb;
          logic [3:0] n, v, b;
          wire [3:0] r, r2, r3;
          um ua(.ni(n), .vi(v), .bi(b), .o(r));
          um ub(.ni(), .vi(), .bi(), .o());
          um uc(.o(r2));
          um ud(, , , r3);
          initial begin
            #1 n = 4'h1; v = 4'h2; b = 4'h3;
            #1 $display("a: ni=%h vi=%h bi=%h o=%h r=%h", ua.ni, ua.vi, ua.bi, ua.o, r);
            $display("b: ni=%h vi=%h bi=%h o=%h", ub.ni, ub.vi, ub.bi, ub.o);
            $display("c: ni=%h vi=%h bi=%h o=%h r2=%h", uc.ni, uc.vi, uc.bi, uc.o, r2);
            $display("d: ni=%h vi=%h bi=%h o=%h r3=%h", ud.ni, ud.vi, ud.bi, ud.o, r3);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "a: ni=1 vi=2 bi=3 o=7 r=7\n"
                 "b: ni=z vi=x bi=0 o=7\n"
                 "c: ni=z vi=x bi=0 o=7 r2=7\n"
                 "d: ni=z vi=x bi=0 o=7 r3=7\n");
}

TEST_CASE("port alias: a wire driven by an assign and a child variable output resolves") {
    // LRM 23.3.3.2, 6.6.1 (Table 6-2): an output port on a net is one more driver,
    // and the child's own variable u.o keeps what the child drives. The
    // expectation is the LRM reading; iverilog treats the wire as a single-driver
    // uwire and refuses to compile (DISAGREEMENTS.md).
    const std::string out = run_capture(R"(
        module drv(input var logic d, output logic o);
          assign o = d;
        endmodule

        module tb;
          logic c, dv;
          wire w;
          assign w = c;
          drv u(.d(dv), .o(w));
          initial begin
            #1 c = 0;    dv = 0;    #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            c = 0;       dv = 1;    #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            c = 1;       dv = 1;    #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            c = 1'bz;    dv = 1;    #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            c = 1'bz;    dv = 0;    #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            c = 1'bz;    dv = 1'bz; #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            c = 1'bx;    dv = 0;    #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            c = 1;       dv = 1'bz; #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "c=0 d=0 w=0 u.o=0\n"
                 "c=0 d=1 w=x u.o=1\n"
                 "c=1 d=1 w=1 u.o=1\n"
                 "c=z d=1 w=1 u.o=1\n"
                 "c=z d=0 w=0 u.o=0\n"
                 "c=z d=z w=z u.o=z\n"
                 "c=x d=0 w=x u.o=0\n"
                 "c=1 d=z w=1 u.o=z\n");
}

TEST_CASE("port alias: a wire driven by a net output and a variable output of two children") {
    // LRM 23.3.3.2, 23.3.3.3, 6.6.1: the resolved net and the variable child's own
    // value are printed. ua.o is not, since 23.3.3.7 lets it show either the
    // resolved or ua's own value.
    const std::string out = run_capture(R"(
        module tsw(input en, input d, output wire o);
          assign o = en ? d : 1'bz;
        endmodule
        module tsv(input en, input d, output logic o);
          always_comb o = en ? d : 1'bz;
        endmodule

        module tb;
          logic ea, da, eb, db;
          wire w;
          tsw ua(.en(ea), .d(da), .o(w));
          tsv ub(.en(eb), .d(db), .o(w));
          initial begin
            #1 ea = 1; da = 0; eb = 0; db = 0; #1 $display("w=%b ub.o=%b", w, ub.o);
            ea = 0; da = 0; eb = 1; db = 1;    #1 $display("w=%b ub.o=%b", w, ub.o);
            ea = 1; da = 1; eb = 1; db = 0;    #1 $display("w=%b ub.o=%b", w, ub.o);
            ea = 0; da = 0; eb = 0; db = 0;    #1 $display("w=%b ub.o=%b", w, ub.o);
            ea = 1; da = 1; eb = 1; db = 1;    #1 $display("w=%b ub.o=%b", w, ub.o);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "w=0 ub.o=z\n"
                 "w=1 ub.o=1\n"
                 "w=x ub.o=0\n"
                 "w=z ub.o=z\n"
                 "w=1 ub.o=1\n");
}

TEST_CASE("port alias: a wand on the outputs of two children") {
    // LRM 23.3.3.7 (Table 23-1), 6.6.3 (Table 6-3), 23.3.3.2: the wand resolution
    // is the same whether ub's net is merged or a continuous assignment. ub.o is
    // not printed. The expectation is the LRM reading; iverilog refuses to
    // compile, as in c15 (DISAGREEMENTS.md).
    const std::string out = run_capture(R"(
        module wdv(input var logic d, output logic o);
          assign o = d;
        endmodule
        module wdn(input d, output wire o);
          assign o = d;
        endmodule

        module tb;
          logic da, db;
          wand w;
          wdv ua(.d(da), .o(w));
          wdn ub(.d(db), .o(w));
          initial begin
            #1 da = 1;    db = 1;    #1 $display("da=%b db=%b w=%b ua.o=%b", da, db, w, ua.o);
            da = 1;       db = 0;    #1 $display("da=%b db=%b w=%b ua.o=%b", da, db, w, ua.o);
            da = 0;       db = 1;    #1 $display("da=%b db=%b w=%b ua.o=%b", da, db, w, ua.o);
            da = 1'bz;    db = 1;    #1 $display("da=%b db=%b w=%b ua.o=%b", da, db, w, ua.o);
            da = 1'bx;    db = 1;    #1 $display("da=%b db=%b w=%b ua.o=%b", da, db, w, ua.o);
            da = 1'bz;    db = 1'bz; #1 $display("da=%b db=%b w=%b ua.o=%b", da, db, w, ua.o);
            da = 0;       db = 1'bx; #1 $display("da=%b db=%b w=%b ua.o=%b", da, db, w, ua.o);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "da=1 db=1 w=1 ua.o=1\n"
                 "da=1 db=0 w=0 ua.o=1\n"
                 "da=0 db=1 w=0 ua.o=0\n"
                 "da=z db=1 w=1 ua.o=z\n"
                 "da=x db=1 w=x ua.o=x\n"
                 "da=z db=z w=z ua.o=z\n"
                 "da=0 db=x w=0 ua.o=0\n");
}

TEST_CASE("port alias: forcing a child output variable reaches the parent signals") {
    // LRM 10.6.2, 23.3.3.2: the parent variable and net follow the forced child
    // variable. After release the child variable keeps the forced value until its
    // next procedural assignment.
    const std::string out = run_capture(R"(
        module c(input var logic [3:0] d, output logic [3:0] o);
          always @(d) o = d;
        endmodule

        module tb;
          logic [3:0] dv;
          logic [3:0] pv;
          wire [3:0] pw;
          c u (.d(dv), .o(pv));
          c u2(.d(dv), .o(pw));
          task show;
            $display("t=%0t dv=%h u.o=%h pv=%h u2.o=%h pw=%h", $time, dv, u.o, pv, u2.o, pw);
          endtask

          initial begin
            #1 dv = 4'h1;
            #1 show;
            force u.o = 4'ha; force u2.o = 4'ha;
            #1 show;
            dv = 4'h2;
            #1 show;
            release u.o; release u2.o;
            #1 show;
            dv = 4'h3;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 dv=1 u.o=1 pv=1 u2.o=1 pw=1\n"
                 "t=3 dv=1 u.o=a pv=a u2.o=a pw=a\n"
                 "t=4 dv=2 u.o=a pv=a u2.o=a pw=a\n"
                 "t=5 dv=2 u.o=a pv=a u2.o=a pw=a\n"
                 "t=6 dv=3 u.o=3 pv=3 u2.o=3 pw=3\n");
}

TEST_CASE("port alias: forcing the parent of an output port leaves the child's variable alone") {
    // LRM 10.6.2, 23.3.3.2: the child's output variable is the source of the port
    // assignment, and 23.3.3.7 merges only nets, so forcing the parent signal does
    // not change u.o. On release the parent variable is re-evaluated from the
    // port. The expectation is the LRM reading; iverilog merges the child variable
    // with the parent signal and shows the forced value in u.o (DISAGREEMENTS.md).
    const std::string out = run_capture(R"(
        module c(input var logic [3:0] d, output logic [3:0] o);
          always @(d) o = d;
        endmodule

        module tb;
          logic [3:0] dv;
          logic [3:0] pv;
          wire [3:0] pw;
          c u (.d(dv), .o(pv));
          c u2(.d(dv), .o(pw));
          task show;
            $display("t=%0t dv=%h u.o=%h pv=%h u2.o=%h pw=%h", $time, dv, u.o, pv, u2.o, pw);
          endtask

          initial begin
            #1 dv = 4'h1;
            #1 show;
            force pv = 4'h5; force pw = 4'h6;
            #1 show;
            dv = 4'h2;
            #1 show;
            release pv; release pw;
            #1 show;
            dv = 4'h3;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 dv=1 u.o=1 pv=1 u2.o=1 pw=1\n"
                 "t=3 dv=1 u.o=1 pv=5 u2.o=1 pw=6\n"
                 "t=4 dv=2 u.o=2 pv=5 u2.o=2 pw=6\n"
                 "t=5 dv=2 u.o=2 pv=2 u2.o=2 pw=2\n"
                 "t=6 dv=3 u.o=3 pv=3 u2.o=3 pw=3\n");
}

TEST_CASE("port alias: a child reads its own initialized output") {
    // LRM 23.2.2.2, 6.8, 23.3.3.2: `output logic [3:0] o = 4'd3` holds 3 before
    // any process starts; the output is connected to a net, left unconnected and
    // connected to a variable. clk starts at 0, so time 0 has no posedge.
    const std::string out = run_capture(R"(
        module ctr(input clk, output logic [3:0] o = 4'd3);
          always @(posedge clk) o <= o + 4'd1;
        endmodule

        module tb;
          logic clk = 1'b0;
          wire [3:0] w;
          logic [3:0] vr;
          ctr u (.clk(clk), .o(w));
          ctr u2(.clk(clk), .o());
          ctr u3(.clk(clk), .o(vr));
          task show;
            $display("t=%0t w=%h vr=%h u.o=%h u2.o=%h u3.o=%h", $time, w, vr, u.o, u2.o, u3.o);
          endtask
          initial begin
            #1 show;
            repeat (3) begin #1 clk = 1'b1; #1 clk = 1'b0; end
            #1 show;
            repeat (11) begin #1 clk = 1'b1; #1 clk = 1'b0; end
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=1 w=3 vr=3 u.o=3 u2.o=3 u3.o=3\n"
                 "t=8 w=6 vr=6 u.o=6 u2.o=6 u3.o=6\n"
                 "t=31 w=1 vr=1 u.o=1 u2.o=1 u3.o=1\n");
}

TEST_CASE("port alias: an NBA pipeline across instance boundaries") {
    // LRM 4.7, 4.9.4, 23.3.3: each stage samples the previous stage's value from
    // before the edge, whether the port updates the parent in the NBA region
    // (merged) or in a later active pass (continuous assignment). din changes and
    // prints happen while clk is low.
    const std::string out = run_capture(R"(
        module stage(input clk, input [7:0] d, output logic [7:0] q);
          always @(posedge clk) q <= d;
        endmodule

        module tb;
          logic clk = 1'b0;
          logic [7:0] din;
          wire [7:0] q1, q2;
          logic [7:0] r;
          stage s1(.clk(clk), .d(din), .q(q1));
          stage s2(.clk(clk), .d(q1), .q(q2));
          always @(posedge clk) r <= q2;
          task show;
            $display("t=%0t q1=%h q2=%h r=%h s2.d=%h s2.q=%h", $time, q1, q2, r, s2.d, s2.q);
          endtask
          initial begin
            #1 din = 8'h11;
            #1 clk = 1'b1; #1 clk = 1'b0; show; din = 8'h22;
            #1 clk = 1'b1; #1 clk = 1'b0; show; din = 8'h33;
            #1 clk = 1'b1; #1 clk = 1'b0; show; din = 8'h44;
            #1 clk = 1'b1; #1 clk = 1'b0; show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=3 q1=11 q2=xx r=xx s2.d=11 s2.q=xx\n"
                 "t=5 q1=22 q2=11 r=xx s2.d=22 s2.q=11\n"
                 "t=7 q1=33 q2=22 r=11 s2.d=33 s2.q=22\n"
                 "t=9 q1=44 q2=33 r=22 s2.d=44 s2.q=33\n");
}

TEST_CASE("port alias: edge and any-change sensitivity on ports") {
    // LRM 9.4.2 (Table 9-2), 9.4.1, 10.3, 23.3.3: assigning the same value again
    // produces no event, and x/z edges count per Table 9-2. Counters ignore time
    // 0.
    const std::string out = run_capture(R"(
        module ch(input in, input var logic d, output logic out);
          int npos = 0, nany = 0;
          always @(posedge in) if ($time > 0) npos++;
          always @(in) if ($time > 0) nany++;
          assign out = d;
        endmodule
        module chv(input var logic in);
          int npos = 0, nany = 0;
          always @(posedge in) if ($time > 0) npos++;
          always @(in) if ($time > 0) nany++;
        endmodule

        module tb;
          logic a, dd;
          wire u_out;
          int nneg = 0, nchg = 0;
          ch  u (.in(a), .d(dd), .out(u_out));
          chv uv(.in(a));
          always @(negedge u_out) if ($time > 0) nneg++;
          always @(u_out) if ($time > 0) nchg++;
          initial begin
            #1 a = 0;    dd = 1;     // a x->0 any        ; dd x->1 any
            #1 a = 1;    dd = 0;     // a 0->1 pos,any    ; dd 1->0 neg,any
            #1 a = 1;    dd = 0;     // same / same
            #1 a = 1;    dd = 1;     // same              ; dd 0->1 any
            #1 a = 0;    dd = 1;     // a 1->0 any        ; same
            #1 a = 0;    dd = 1'bz;  // same              ; dd 1->z neg,any
            #1 a = 1;    dd = 0;     // a 0->1 pos,any    ; dd z->0 neg,any
            #1 a = 1'bz; dd = 1'bx;  // a 1->z any        ; dd 0->x any (posedge)
            #1 a = 1;    dd = 0;     // a z->1 pos,any    ; dd x->0 neg,any
            #1 a = 1'bx; dd = 0;     // a 1->x any        ; same
            #1 a = 1;                // a x->1 pos,any
            #1 $display("u.npos=%0d u.nany=%0d uv.npos=%0d uv.nany=%0d nneg=%0d nchg=%0d",
                        u.npos, u.nany, uv.npos, uv.nany, nneg, nchg);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "u.npos=4 u.nany=8 uv.npos=4 uv.nany=8 nneg=4 nchg=7\n");
}

TEST_CASE("port alias: one module under whole, expression, unconnected, constant and part-select connections") {
    // LRM 23.3.3, 23.3.3.3, 23.3.3.8, 10.7: the narrower part-select of a parent
    // variable is zero-extended and the unconnected net input reads z. Single
    // drivers.
    const std::string out = run_capture(R"(
        module m(input [7:0] i, output [7:0] o);
          assign o = i + 8'd1;
        endmodule

        module tb;
          logic [7:0] s;
          wire [7:0] ow, oe, op;
          logic [7:0] ok;
          m uw(.i(s), .o(ow));
          m ue(.i(s ^ 8'hf0), .o(oe));
          m un(.i(), .o());
          m uk(.i(8'd7), .o(ok));
          m up(.i(s[3:0]), .o(op));
          task show;
            $display("t=%0t s=%h ow=%h oe=%h un.i=%h un.o=%h ok=%h up.i=%h op=%h", $time, s, ow, oe, un.i, un.o, ok, up.i, op);
          endtask
          initial begin
            #1 s = 8'h12;
            #1 show;
            s = 8'hff;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 s=12 ow=13 oe=e3 un.i=zz un.o=xx ok=08 up.i=02 op=03\n"
                 "t=3 s=ff ow=00 oe=10 un.i=zz un.o=xx ok=08 up.i=0f op=10\n");
}

TEST_CASE("port alias: hierarchical writes to a parent variable reach the input ports") {
    // LRM 23.6, 23.3.3, 23.3.3.2: another module writes tb.x with blocking and
    // nonblocking assignments; the net-port and variable-port children each see
    // every value. Writes at t=1,3,5, prints at t=2,4,6, counters ignore time 0.
    const std::string out = run_capture(R"(
        module rd(input [7:0] in);
          logic [7:0] last;
          int n = 0;
          always @(in) if ($time > 0) begin last = in; n++; end
        endmodule
        module rdv(input var logic [7:0] in);
          logic [7:0] last;
          int n = 0;
          always @(in) if ($time > 0) begin last = in; n++; end
        endmodule
        module writer;
          initial begin
            #1 tb.x = 8'h05;
            #2 tb.x = 8'ha0;
            #2 tb.x <= 8'h77;
          end
        endmodule

        module tb;
          logic [7:0] x;
          rd  u (.in(x));
          rdv uv(.in(x));
          writer w();
          task show;
            $display("t=%0t x=%h u.in=%h uv.in=%h u.last=%h uv.last=%h u.n=%0d uv.n=%0d", $time, x, u.in, uv.in, u.last, uv.last, u.n, uv.n);
          endtask
          initial begin
            #2 show;
            #2 show;
            #2 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 x=05 u.in=05 uv.in=05 u.last=05 uv.last=05 u.n=1 uv.n=1\n"
                 "t=4 x=a0 u.in=a0 uv.in=a0 u.last=a0 uv.last=a0 u.n=2 uv.n=2\n"
                 "t=6 x=77 u.in=77 uv.in=77 u.last=77 uv.last=77 u.n=3 uv.n=3\n");
}

TEST_CASE("port alias: an output wired to the parent's own output port over two levels") {
    // LRM 23.3.3, 23.3.3.2, 23.3.3.3: leaf.o (variable) -> mid.o (net) -> top2.o
    // (variable) -> tb wire, and mid reads its own output port. Single drivers.
    const std::string out = run_capture(R"(
        module leaf(input [7:0] i, output logic [7:0] o);
          always @(i) o = i + 8'd1;
        endmodule
        module mid(input [7:0] i, output [7:0] o);
          wire [7:0] dbl;
          leaf l(.i(i), .o(o));
          assign dbl = o << 1;
        endmodule
        module top2(input [7:0] i, output logic [7:0] o);
          mid m(.i(i), .o(o));
        endmodule

        module tb;
          logic [7:0] x;
          wire [7:0] r;
          logic [7:0] rv;
          top2 u (.i(x), .o(r));
          mid  u2(.i(x), .o(rv));
          task show;
            $display("t=%0t x=%h r=%h u.o=%h u.m.o=%h u.m.l.o=%h u.m.dbl=%h rv=%h u2.o=%h u2.dbl=%h",
                     $time, x, r, u.o, u.m.o, u.m.l.o, u.m.dbl, rv, u2.o, u2.dbl);
          endtask
          initial begin
            #1 x = 8'h10;
            #1 show;
            x = 8'hff;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 x=10 r=11 u.o=11 u.m.o=11 u.m.l.o=11 u.m.dbl=22 rv=11 u2.o=11 u2.dbl=22\n"
                 "t=3 x=ff r=00 u.o=00 u.m.o=00 u.m.l.o=00 u.m.dbl=00 rv=00 u2.o=00 u2.dbl=00\n");
}

TEST_CASE("port alias: generate loops connect ports to array elements and bit-selects") {
    // LRM 27.4, 23.3.3, 23.3.3.2, 23.3.3.3: `.in(bus[i])` of an unpacked variable
    // array, `.out(res[i])` of an unpacked net array, and single-bit ports on
    // bit-selects of packed vectors. Single drivers.
    const std::string out = run_capture(R"(
        module cellm #(parameter int K = 0) (input [7:0] in, output logic [7:0] out);
          always @(in) out = in + K;
        endmodule
        module inv(input in, output out);
          assign out = ~in;
        endmodule

        module tb;
          logic [7:0] bus [4];
          wire  [7:0] res [4];
          logic [3:0] pk;
          wire  [3:0] pko;
          for (genvar i = 0; i < 4; i++) begin : g
            cellm #(.K(i)) c(.in(bus[i]), .out(res[i]));
            inv n(.in(pk[i]), .out(pko[i]));
          end
          task show;
            $display("t=%0t res=%h %h %h %h g2.in=%h g1.out=%h pko=%b g0.n.out=%b", $time,
                     res[0], res[1], res[2], res[3], g[2].c.in, g[1].c.out, pko, g[0].n.out);
          endtask
          initial begin
            #1 bus[0] = 8'h10; bus[1] = 8'h20; bus[2] = 8'h30; bus[3] = 8'h40; pk = 4'b1010;
            #1 show;
            bus[1] = 8'hff; pk[0] = 1'b1;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 res=10 21 32 43 g2.in=30 g1.out=21 pko=0101 g0.n.out=1\n"
                 "t=3 res=10 00 32 43 g2.in=30 g1.out=00 pko=0100 g0.n.out=0\n");
}

TEST_CASE("port alias: one signal on an input and an output of the same instance counts") {
    // LRM 23.3.3, 23.3.3.2, 23.2.2.2, 4.9.4: a parent net and a parent variable
    // each loop through one instance; the variable is written only by the output
    // port. NBA update, clk starts at 0, prints while clk is low.
    const std::string out = run_capture(R"(
        module acc(input clk, input [3:0] i, output logic [3:0] o = 4'd0);
          always @(posedge clk) o <= i + 4'd1;
        endmodule
        module accv(input clk, input var logic [3:0] i, output logic [3:0] o = 4'd8);
          always @(posedge clk) o <= i + 4'd1;
        endmodule

        module tb;
          logic clk = 1'b0;
          wire [3:0] w;
          logic [3:0] v;
          acc  u (.clk(clk), .i(w), .o(w));
          accv uv(.clk(clk), .i(v), .o(v));
          task show;
            $display("t=%0t w=%h u.i=%h u.o=%h v=%h uv.i=%h uv.o=%h", $time, w, u.i, u.o, v, uv.i, uv.o);
          endtask
          initial begin
            #1 show;
            repeat (5) begin #1 clk = 1'b1; #1 clk = 1'b0; end
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=1 w=0 u.i=0 u.o=0 v=8 uv.i=8 uv.o=8\n"
                 "t=12 w=5 u.i=5 u.o=5 v=d uv.i=d uv.o=d\n");
}

TEST_CASE("port alias: one signal on two inputs and an output fed back into a third") {
    // LRM 23.3.3, 23.3.3.2, 23.3.3.3: s = a + b + c is recomputed on every input
    // change, so after the time step settles it is the same for any evaluation
    // order.
    const std::string out = run_capture(R"(
        module m3(input [7:0] a, input var logic [7:0] b, input [7:0] c,
                  output logic [7:0] s, output [7:0] t);
          assign t = a + 8'd1;
          always @(a, b, c) s = a + b + c;
        endmodule

        module tb;
          logic [7:0] x;
          wire [7:0] tt, ss;
          m3 u(.a(x), .b(x), .c(tt), .s(ss), .t(tt));
          task show;
            $display("t=%0t x=%h u.a=%h u.b=%h u.c=%h tt=%h ss=%h u.s=%h", $time, x, u.a, u.b, u.c, tt, ss, u.s);
          endtask
          initial begin
            #1 x = 8'h10;
            #1 show;
            x = 8'hff;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 x=10 u.a=10 u.b=10 u.c=11 tt=11 ss=31 u.s=31\n"
                 "t=3 x=ff u.a=ff u.b=ff u.c=00 tt=00 ss=fe u.s=fe\n");
}

TEST_CASE("port alias: positional, named, implicit named and wildcard connections") {
    // LRM 23.3.2.1-23.3.2.4: `.*` takes an explicit named override for y. Single
    // drivers.
    const std::string out = run_capture(R"(
        module sub(input [7:0] a, input [7:0] b, output [7:0] y);
          assign y = a - b;
        endmodule

        module tb;
          logic [7:0] a, b;
          wire [7:0] y1, y2, y3, y4;
          sub u0(a, b, y1);
          sub u1(.a(b), .b(a), .y(y2));
          sub u2(.a, .b, .y(y3));
          sub u3(.*, .y(y4));
          task show;
            $display("t=%0t y1=%h y2=%h y3=%h y4=%h u1.a=%h u3.b=%h", $time, y1, y2, y3, y4, u1.a, u3.b);
          endtask
          initial begin
            #1 a = 8'h03; b = 8'h04;
            #1 show;
            a = 8'h80; b = 8'h01;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 y1=ff y2=01 y3=ff y4=ff u1.a=04 u3.b=04\n"
                 "t=3 y1=7f y2=81 y3=7f y4=7f u1.a=01 u3.b=01\n");
}

TEST_CASE("port alias: passthrough children with net input ports") {
    // LRM 23.3.3, 23.3.3.2, 23.3.3.3: c08's chain and wrapper with every input
    // port a net; p2 keeps a variable output port written by a continuous
    // assignment. Single drivers.
    const std::string out = run_capture(R"(
        module pass(input [7:0] i, output [7:0] o);
          assign o = i;
        endmodule
        module passl(input logic [7:0] i, output logic [7:0] o);
          assign o = i;
        endmodule
        module inc(input [7:0] i, output [7:0] o);
          assign o = i + 8'd1;
        endmodule
        module wrap(input [7:0] i, output [7:0] o);
          pass p(.i(i), .o(o));
        endmodule
        module wrap2(input [7:0] i, output [7:0] o);
          wrap u(.i(i), .o(o));
        endmodule

        module tb;
          logic [7:0] s;
          wire [7:0] w1, w2, w3, w4;
          pass  p1(.i(s),  .o(w1));
          passl p2(.i(w1), .o(w2));
          inc   p3(.i(w2), .o(w3));
          wrap2 q (.i(w3), .o(w4));
          task show;
            $display("t=%0t s=%h w1=%h w2=%h w3=%h w4=%h q.i=%h deep=%h", $time, s, w1, w2, w3, w4, q.i, q.u.p.o);
          endtask
          initial begin
            #1 s = 8'h10;
            #1 show;
            s = 8'hff;
            #1 show;
            s = 8'bz;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 s=10 w1=10 w2=10 w3=11 w4=11 q.i=11 deep=11\n"
                 "t=3 s=ff w1=ff w2=ff w3=00 w4=00 q.i=00 deep=00\n"
                 "t=4 s=zz w1=zz w2=zz w3=xx w4=xx q.i=xx deep=xx\n");
}

TEST_CASE("port alias: procedural child outputs into a resolved wire and a wand") {
    // LRM 23.3.3.2, 6.6.1, 6.6.3: c15's and c17's tables with always_comb-driven
    // output variables. The children's own variables are printed, not the resolved
    // net.
    const std::string out = run_capture(R"(
        module drvp(input var logic d, output logic o);
          always_comb o = d;
        endmodule

        module tb;
          logic c, dv, da, db;
          wire w;
          wand wa;
          assign w = c;
          drvp u (.d(dv), .o(w));
          drvp ua(.d(da), .o(wa));
          drvp ub(.d(db), .o(wa));
          task s1(input logic cc, input logic dd);
            c = cc; dv = dd;
            #1 $display("c=%b d=%b w=%b u.o=%b", c, dv, w, u.o);
          endtask
          task s2(input logic aa, input logic bb);
            da = aa; db = bb;
            #1 $display("da=%b db=%b wa=%b ua.o=%b ub.o=%b", da, db, wa, ua.o, ub.o);
          endtask
          initial begin
            #1;
            s1(0, 0); s1(0, 1); s1(1, 1); s1(1'bz, 1); s1(1'bz, 0); s1(1'bz, 1'bz); s1(1'bx, 0); s1(1, 1'bz);
            s2(1, 1); s2(1, 0); s2(0, 1); s2(1'bz, 1); s2(1'bx, 1); s2(1'bz, 1'bz); s2(0, 1'bx);
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "c=0 d=0 w=0 u.o=0\n"
                 "c=0 d=1 w=x u.o=1\n"
                 "c=1 d=1 w=1 u.o=1\n"
                 "c=z d=1 w=1 u.o=1\n"
                 "c=z d=0 w=0 u.o=0\n"
                 "c=z d=z w=z u.o=z\n"
                 "c=x d=0 w=x u.o=0\n"
                 "c=1 d=z w=1 u.o=z\n"
                 "da=1 db=1 wa=1 ua.o=1 ub.o=1\n"
                 "da=1 db=0 wa=0 ua.o=1 ub.o=0\n"
                 "da=0 db=1 wa=0 ua.o=0 ub.o=1\n"
                 "da=z db=1 wa=1 ua.o=z ub.o=1\n"
                 "da=x db=1 wa=x ua.o=x ub.o=1\n"
                 "da=z db=z wa=z ua.o=z ub.o=z\n"
                 "da=0 db=x wa=0 ua.o=0 ub.o=x\n");
}

TEST_CASE("port alias: an inout used in both directions with tri-state drivers") {
    // LRM 23.3.3, 23.3.3.3, 6.6.1: child drives, parent drives, contention and no
    // driver. Only strong drivers, so a tran connection and a merged net give the
    // same value on both sides.
    const std::string out = run_capture(R"(
        module dev(input [7:0] din, input oe, output logic [7:0] dout, inout wire [7:0] io);
          assign io = oe ? din : 8'bz;
          assign dout = io;
        endmodule

        module tb;
          logic [7:0] pd, cd;
          logic poe, coe;
          wire [7:0] bus;
          logic [7:0] seen;
          assign bus = poe ? pd : 8'bz;
          dev u(.din(cd), .oe(coe), .dout(seen), .io(bus));
          task show;
            $display("t=%0t bus=%b u.io=%b seen=%b", $time, bus, u.io, seen);
          endtask
          initial begin
            #1 coe = 1; cd = 8'h3c; poe = 0; pd = 8'h00;
            #1 show;
            coe = 0; poe = 1; pd = 8'ha5;
            #1 show;
            coe = 1; poe = 1; pd = 8'h0f;
            #1 show;
            coe = 0; poe = 0;
            #1 show;
            $finish(0);
          end
        endmodule
    )");
    CHECK(out == "t=2 bus=00111100 u.io=00111100 seen=00111100\n"
                 "t=3 bus=10100101 u.io=10100101 seen=10100101\n"
                 "t=4 bus=00xx11xx u.io=00xx11xx seen=00xx11xx\n"
                 "t=5 bus=zzzzzzzz u.io=zzzzzzzz seen=zzzzzzzz\n");
}

TEST_CASE("port alias: a store-only process feeding an output through a copy alias stays") {
    // `seen` becomes a name of the parent's `s0` (the copy alias through the bound
    // output port), and the process storing into it must survive unread-logic
    // removal because another instance reads that storage.
    const std::string out = run_capture(R"(
        module leaf(input logic clk, output logic ev_seen);
          event e;
          logic seen = 0;
          always @(posedge clk) -> e;
          always @(e) seen = 1;
          assign ev_seen = seen;
        endmodule
        module tb;
          logic clk = 0;
          logic s0;
          leaf l0(.clk, .ev_seen(s0));
          always #5 clk = ~clk;
          initial begin #30 $display("%b", s0); $finish(0); end
        endmodule
    )");
    CHECK(out == "1\n");
}
