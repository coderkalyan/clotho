#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a design that ends via $finish and return its stdout. The simulation
// gets a process of its own (Subprocess.h explains why $finish cannot share
// the runner's); lowering here as well is what reports a broken source.
std::string run_capture_finish(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    return test::run_design_subprocess(source);
}

std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    return r.ok ? std::string{} : r.diagnostic_text();
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

TEST_CASE("force: net override of a gate driver, continuous, revert on release") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, c;
            wire e;
            and (e, a, b, c);
            initial begin
                a = 1; b = 0; c = 1;
                #1 $display("e=%b", e);
                force e = (a | b | c);
                #1 $display("e=%b", e);
                release e;
                #1 $display("e=%b", e);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "e=0\ne=1\ne=0\n");
}

TEST_CASE("force: net tracks RHS, reverts to assign driver on release") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] src, drv;
            wire [7:0] y;
            assign y = drv;
            initial begin
                drv = 8'd1; src = 8'd50;
                #1 force y = src;
                #1 $display("y=%0d", y);
                src = 8'd77; #1 $display("y=%0d", y);
                release y; #1 $display("y=%0d", y);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "y=50\ny=77\ny=1\n");
}

TEST_CASE("force: undriven net reverts to z on release") {
    constexpr std::string_view src = R"(
        module m;
            wire [3:0] u;
            initial begin
                #1 $display("u=%b", u);
                force u = 4'd5;
                #1 $display("u=%0d", u);
                release u;
                #1 $display("u=%b", u);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "u=zzzz\nu=5\nu=zzzz\n");
}

TEST_CASE("force: resolved net override reverts to resolution on release") {
    constexpr std::string_view src = R"(
        module m;
            logic da, db;
            wire w;
            assign w = da;
            assign w = db;
            initial begin
                da = 0; db = 0; #1 $display("w=%b", w);
                force w = 1'b1; #1 $display("w=%b", w);
                da = 1; db = 0; #1 $display("w=%b", w);
                release w; #1 $display("w=%b", w);
                db = 1; #1 $display("w=%b", w);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "w=0\nw=1\nw=1\nw=x\nw=1\n");
}

// A net force must be visible to a same-instant read (`force n = v; if (n===v)`),
// not deferred one delta to the resolver. Icarus ivtest force1/force2 shape.
// Matches iverilog 13.
TEST_CASE("force: net force is visible in the same time step") {
    constexpr std::string_view src = R"(
        module m;
            reg [3:0] drv = 4'b0101;
            wire [3:0] val = drv;
            initial begin
                force val = 4'b1010;
                if (val !== 4'b1010) $display("FAIL a");
                force val = 4'bzzzz;
                if (val !== 4'bzzzz) $display("FAIL b");
                $display("v=%b", val);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "v=zzzz\n");
}

// Constant bit/part-select force of a net (LRM 10.6.2): only the selected bits are
// overridden; unforced bits keep the driver (z preserved), and a range `release`
// reverts just those bits. Icarus ivtest force_release_wire_pv shape, byte-identical
// to iverilog 13.
TEST_CASE("force: net bit/part-select overlay, z-preserving, range release") {
    constexpr std::string_view src = R"(
        module m;
            reg [3:0] in = 4'b0;
            wire [3:0] bus = in;
            initial begin
                #1 force bus[0] = 1'b1;    #1 $display("%b", bus);
                #1 force bus[3:2] = 2'b11; #1 $display("%b", bus);
                in[1] = 1'bz;              #1 $display("%b", bus);
                release bus[0];            #1 $display("%b", bus);
                release bus[3:2];          #1 $display("%b", bus);
                force bus[2:1] = 2'bx1;    #1 $display("%b", bus);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "0001\n1101\n11z1\n11z0\n00z0\n0x10\n");
}

// force / assign target an OUTPUT-port variable driven internally (an input/inout
// port is the guarded case). sv-tests 10.6.1 / 10.6.2 shape.
TEST_CASE("force: output-port variable is forceable") {
    constexpr std::string_view src = R"(
        module m(output logic [7:0] q);
            initial begin
                q = 8'd1;
                force q = 8'd42;
                #1 $display("q=%0d", q);
                release q;
                #1 q = 8'd7;
                $display("q=%0d", q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "q=42\nq=7\n");
}

// force/release and assign/deassign on a real variable (LRM 10.6). The machinery
// is type-generic; the value store is a plain real store. Icarus ivtest
// real_force_rel / real_assign_deassign shape. Matches iverilog 13.
TEST_CASE("force/assign: real variable") {
    constexpr std::string_view src = R"(
        module m;
            real f;
            reg d;
            always @(d) assign f = 1.5;
            initial begin
                f = 0.0;
                #1 $display("f=%g", f);
                f = 2.0;
                #1 $display("f=%g", f);
                d = 0;                      // assign f = 1.5 activates
                #1 f = 9.0;                 // suppressed by assign
                #1 $display("f=%g", f);
                force f = 7.25;
                #1 f = 9.0;                 // suppressed by force
                #1 $display("f=%g", f);
                release f;                  // reverts to assign (1.5)
                #1 $display("f=%g", f);
                deassign f;                 // holds
                #1 f = 3.0;
                #1 $display("f=%g", f);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "f=0\nf=2\nf=1.5\nf=7.25\nf=1.5\nf=3\n");
}

// -- Loud guards (Phase 1 boundaries) --

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
