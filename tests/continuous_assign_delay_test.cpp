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

// LRM 10.3.3: an inertial continuous-assignment delay rejects a pulse on the
// RHS narrower than the delay. `y = a|b` with `#5`: a 1->0 pulse of width 2 on
// the RHS never appears on y. Byte-identical to iverilog 13
// (t=1 y=x, t=13 y=0, t=16 y=0, t=18 y=1).
TEST_CASE("inertial CA delay: pulse rejection and no-restart on same-value change") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, y;
            assign #5 y = a | b;
            initial begin
                a = 0; b = 0;
                #1 $display("t=%0t y=%b", $time, y);
                a = 1; #2 a = 0;
                #10 $display("t=%0t y=%b", $time, y);
                a = 1; #2 b = 1;
                #1 $display("t=%0t y=%b", $time, y);
                #2 $display("t=%0t y=%b", $time, y);
                #5 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) ==
          "t=1 y=x\nt=13 y=0\nt=16 y=0\nt=18 y=1\n");
}

// Step b (LRM 10.3.3): a change to a different input that leaves the RHS value
// unchanged does NOT restart the timer. The `#2 b = 1` at t=15 leaves `a|b`==1
// (a already 1 since t=13), so y propagates at the original t=18, not t=20.
// (Covered above by the t=18 y=1 line; this case isolates a clean toggle
// sequence with delay 3.)
TEST_CASE("inertial CA delay: clean toggles wider than the delay propagate") {
    constexpr std::string_view src = R"(
        module m;
            logic a, y;
            assign #3 y = a;
            initial begin
                a = 0;
                #5 $display("t=%0t y=%b", $time, y);
                a = 1; #5 $display("t=%0t y=%b", $time, y);
                a = 0; #1 a = 1;
                #5 $display("t=%0t y=%b", $time, y);
                a = 0; #4 $display("t=%0t y=%b", $time, y);
                #5 $finish;
            end
        endmodule)";
    // t=3 y<=0, t=8 y<=1, glitch at t=10..11 (width 1 < 3) rejected, y=0<=t=19.
    CHECK(run_capture_finish(src) ==
          "t=5 y=0\nt=10 y=1\nt=16 y=1\nt=20 y=0\n");
}

// A wide 2-state net (<= 64 value bits) is supported: the packed value fits the
// i64 the inertial decide compares.
TEST_CASE("inertial CA delay: 64-bit 2-state net") {
    constexpr std::string_view src = R"(
        module m;
            bit [63:0] a, y;
            assign #2 y = a;
            initial begin
                a = 64'hDEAD_BEEF_0000_0001;
                #5 $display("y=%h", y);
                #1 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "y=deadbeef00000001\n");
}

// Guard: a rise/fall/turn-off delay triple `#(a,b,c)` on a net has no Clotho
// representation and must fail loud (LRM 10.3.3 vector-net transition delays).
TEST_CASE("inertial CA delay: multi-value delay is rejected") {
    const std::string err = lower_error(R"(
        module m; logic a; wire y; assign #(1,2,3) y = a; endmodule)");
    CHECK(err.find("rise/fall/turn-off") != std::string::npos);
}

// Guard: only a whole integral scalar target is lowered; a bit/part-select
// target fails loud.
TEST_CASE("inertial CA delay: select target is rejected") {
    const std::string err = lower_error(R"(
        module m; logic a; logic [3:0] y; assign #5 y[2] = a; endmodule)");
    CHECK(err.find("whole integral scalar") != std::string::npos);
}

// Guard: a concatenation target has no single record shape; fail loud.
TEST_CASE("inertial CA delay: concatenation target is rejected") {
    const std::string err = lower_error(R"(
        module m; logic a, b; logic [1:0] c; assign #5 {a,b} = c; endmodule)");
    CHECK(err.find("whole integral scalar") != std::string::npos);
}

// An 81-bit 4-state target (162 mm-plane bits) is supported: the backend's
// inertial decide compares via `icmp` on loaded values, not a packed-scalar
// runtime call, so it carries no width limit.
TEST_CASE("inertial CA delay: a net wider than 64 value bits is supported") {
    constexpr std::string_view src = R"(
        module m;
            logic [80:0] a, y;
            assign #5 y = a;
            initial begin
                a = 81'h1_DEAD_BEEF_0000_0001_1234;
                #6 $display("y=%h", y);
                #1 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "y=1deadbeef000000011234\n");
}

// LRM 10.3.3: on a multiply-driven net the delay belongs to the DRIVER, ahead of
// resolution. `assign #5 w = a` beside `assign w = b` delays only the first
// driver's contribution; the resolver re-folds when that driver's slot lands, so
// the undelayed driver still conflicts at its own instant (t=13 x). The delayed
// driver keeps inertial pulse rejection: the 2-wide 1->0->1 pulse at t=14 never
// reaches w. Byte-identical to iverilog 13.
TEST_CASE("inertial CA delay: delayed driver of a multiply-driven net") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            wire  w;
            assign #5 w = a;
            assign    w = b;
            initial begin
                a = 0; b = 1'bz;
                #1 $display("t=%0t w=%b", $time, w);  // driver's slot powers up x
                #5 $display("t=%0t w=%b", $time, w);  // 0 landed at t=5
                a = 1;
                #4 $display("t=%0t w=%b", $time, w);  // t=10, lands at t=11
                #2 $display("t=%0t w=%b", $time, w);  // t=12, resolved 1
                b = 0;
                #1 $display("t=%0t w=%b", $time, w);  // undelayed conflict -> x
                b = 1'bz;
                #1 $display("t=%0t w=%b", $time, w);  // back to the delayed 1
                a = 0; #2 a = 1;                      // 2-wide pulse < 5: rejected
                #8 $display("t=%0t w=%b", $time, w);
                #5 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 w=x\nt=6 w=0\nt=10 w=0\nt=12 w=1\nt=13 w=x\nt=14 w=1\nt=24 w=1\n");
}

// Guard: a delayed PARTIAL driver of a resolved net would fold as x (not z) in
// its undriven bits for the whole initial window, because its slot powers up
// all-x and the delay defers the first Z-background store.
TEST_CASE("inertial CA delay: partial driver of a resolved net is rejected") {
    const std::string err = lower_error(R"(
        module m; logic [1:0] a, b; wire [3:0] v;
        assign #5 v[1:0] = a; assign v[1:0] = b; endmodule)");
    CHECK(err.find("partial driver of a multiply-driven") != std::string::npos);
}

// A delayed driver's slot on a resolved net also carries no width limit — a
// 41-bit 4-state net packs to 82 mm-plane bits, past the old 64-bit cap.
TEST_CASE("inertial CA delay: a wide multiply-driven net is supported") {
    constexpr std::string_view src = R"(
        module m;
            logic [40:0] a, b;
            wire  [40:0] y;
            assign #5 y = a;
            assign    y = b;
            initial begin
                a = 41'h1_FFFF_FFFF; b = 41'bz;
                #1 $display("t=%0t y=%h", $time, y);
                #6 $display("t=%0t y=%h", $time, y);
                #1 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 y=xxxxxxxxxxx\nt=7 y=001ffffffff\n");
}
