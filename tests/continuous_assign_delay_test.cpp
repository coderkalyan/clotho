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

// Guard: a 4-state net wider than 32 bits packs to more than 64 mm-plane bits,
// which the i64 decide compare cannot hold; fail loud.
TEST_CASE("inertial CA delay: net wider than 64 value bits is rejected") {
    const std::string err = lower_error(R"(
        module m; logic [40:0] a, y; assign #5 y = a; endmodule)");
    CHECK(err.find("wider than 64 value bits") != std::string::npos);
}
