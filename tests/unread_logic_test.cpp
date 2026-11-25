#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// The processes whose results nothing reads are removed (docs/design/
// modular-compilation.md, "Unread logic"). Each case is logic that looks unread
// from inside its module but is read by something the removal must count, next
// to logic that is unread; the outputs follow from the LRM clause each cites and
// match iverilog 13, except the concurrent assertion, which iverilog 13 does not
// parse.

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

TEST_CASE("unread logic: a register chain read hierarchically stays beside one nothing reads") {
    // LRM 10.4.2, 23.6. Only l4 and c are read, both from the testbench; the x
    // chain is unread, and c reads only itself.
    CHECK(run_capture(R"(
        module pipe(input clk, input [7:0] d);
          logic [7:0] l1, l2, l3, l4;
          logic [7:0] x1, x2, x3, x4;
          logic [7:0] c;
          initial begin l1 = 0; l2 = 0; l3 = 0; l4 = 0; c = 0; end
          always @(posedge clk) l1 <= d;
          always @(posedge clk) l2 <= l1;
          always @(posedge clk) l3 <= l2;
          always @(posedge clk) l4 <= l3;
          always_ff @(posedge clk) x1 <= d + 8'd1;
          always_ff @(posedge clk) x2 <= x1;
          always_ff @(posedge clk) x3 <= x2;
          always_ff @(posedge clk) x4 <= x3;
          always @(posedge clk) c <= c + 8'd1;
        endmodule
        module tb;
          logic clk;
          logic [7:0] d;
          pipe p(.clk(clk), .d(d));
          initial begin clk = 0; forever #5 clk = ~clk; end
          initial begin
            d = 8'd10;
            #1;
            repeat (7) begin
              @(negedge clk);
              $display("l4=%0d", tb.p.l4);
              d = d + 8'd10;
            end
            $display("c=%0d", tb.p.c);
            $finish(0);
          end
        endmodule
    )") == "l4=0\nl4=0\nl4=0\nl4=10\nl4=20\nl4=30\nl4=40\nc=7\n");
}

TEST_CASE("unread logic: a signal read only inside a function body is read") {
    // LRM 10.3.2, 9.2.2.2.1: k2 is read only by f, called from a continuous
    // assignment; m only by g, called from always_comb.
    CHECK(run_capture(R"(
        module sub(input [7:0] trig, output [7:0] o, output logic [7:0] o2);
          logic [7:0] k;
          wire  [7:0] k2 = k + 8'd2;
          function automatic logic [7:0] f(input logic [7:0] a);
            return a + k2;
          endfunction
          assign o = f(trig);
          logic [7:0] m;
          function automatic logic [7:0] g();
            return m * 8'd3 + 8'd1;
          endfunction
          always_comb o2 = g();
          initial begin k = 8'd10; m = 8'd4; end
        endmodule
        module tb;
          logic [7:0] trig;
          wire  [7:0] o, o2;
          sub u(.trig(trig), .o(o), .o2(o2));
          initial begin
            #1 trig = 8'd1;
            #1 $display("o=%0d o2=%0d", o, o2);
            tb.u.m = 8'd5;
            tb.u.k = 8'd20;
            #1 $display("o2=%0d", o2);
            trig = 8'd3;
            #1 $display("o=%0d", o);
            $finish(0);
          end
        endmodule
    )") == "o=13 o2=13\no2=16\no=25\n");
}

TEST_CASE("unread logic: a signal read only by a concurrent assertion is read") {
    // LRM 16.14.1, 16.5.1: a and b are read only by the assertion, through the
    // values it samples. v changes at negedges, so each posedge samples stable
    // values, and only the posedge at 25 has a=1 and b=0.
    CHECK(run_capture(R"(
        module sub(input clk, input [3:0] v);
          wire a = (v == 4'd2) || (v == 4'd5);
          wire b = v[0];
          ap: assert property (@(posedge clk) a |-> b) else $display("assertion failed at %0d", $time);
        endmodule
        module tb;
          logic clk;
          logic [3:0] v;
          sub u(.clk(clk), .v(v));
          initial begin
            clk = 0; v = 0;
            repeat (6) begin #5 clk = 1; #5 clk = 0; v = v + 4'd1; end
          end
        endmodule
    )") == "assertion failed at 25\n");
}

TEST_CASE("unread logic: a signal read only from a sibling instance is read") {
    // LRM 23.6, 23.8: x is read only by consumer c, by a full hierarchical name
    // and by an upward reference.
    CHECK(run_capture(R"(
        module producer;
          logic [7:0] a;
          wire  [7:0] x = a + 8'd3;
          initial begin #1 a = 8'd4; #2 a = 8'd10; end
        endmodule
        module consumer;
          wire [7:0] seen = p.x * 8'd2;
          initial begin
            #2 $display("x=%0d seen=%0d", tb.p.x, seen);
            #2 $display("x=%0d seen=%0d", tb.p.x, seen);
          end
        endmodule
        module tb;
          producer p();
          consumer c();
        endmodule
    )") == "x=7 seen=14\nx=13 seen=26\n");
}

TEST_CASE("unread logic: a delay in a block that writes only unread signals still holds time") {
    // LRM 4.5, 9.2.3: the #100 is the last scheduled event, so the simulation,
    // and the final block's $time, end at 100 although nothing reads `dead`.
    CHECK(run_capture(R"(
        module tb;
          int dead;
          initial begin #100 dead = 1; end
          initial #3 $display("live at %0d", $time);
          final $display("end at %0d", $time);
        endmodule
    )") == "live at 3\nend at 100\n");
}
