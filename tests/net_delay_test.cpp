#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a design that ends via $finish and return its stdout. Mirrors
// continuous_assign_delay_test.cpp's helper of the same name.
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

// A net delay (LRM 28.16, `wire #5 w;`, no net_decl_assignment) is NOT a
// per-driver delay like `assign #d` -- it delays "the time it takes from any
// driver on the net changing value to the time when the net value is updated
// and propagated further", applied once at the resolved net regardless of
// driver count. iverilog 13 does not implement net delays at all ("sorry: Net
// delays not supported"), so this is pinned by hand-tracing the LRM 10.3.3
// inertial decide steps (a/b/c/d) against Clotho's own already-verified
// `store_inertial_delay` primitive, the same one `assign #d` uses.
//
// Trace: a=0 at t=0 stages the (forced) driver slot to 0 immediately (the
// driver itself carries no delay); the resolver wakes, sees the net's
// power-up value (x, LRM 6.6/6.7 default before any store — an inertially
// delayed target powers up x, not z, same as a delayed `assign #d` target),
// and arms a delayed store of 0 for t=0+5=5. At t=1 (still before that store
// lands) a=1 fires: the slot changes to 1, the resolver deschedules the
// pending 0 (LRM 10.3.3 step a/b) and arms 1 for t=1+5=6, so the t=5 store
// never lands. w reads x at t=1 and 1 from t=6 on.
TEST_CASE("net delay: single driver update lands 5 units after it changes (LRM 28.16)") {
    constexpr std::string_view src = R"(
        module m;
            logic a;
            wire #5 w;
            assign w = a;
            initial begin
                a = 0;
                #1 $display("t=%0t w=%b", $time, w);
                a = 1;
                #6 $display("t=%0t w=%b", $time, w);
                #5 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 w=x\nt=7 w=1\n");
}

// LRM 9.4.1: a delay that evaluates to an unknown value "shall be interpreted as
// zero delay". 28.16 does not repeat it for a net delay, which takes the same
// delay_value (see scaled_delay), so any x or z bit zeroes it: `wire #D w` with
// `D = 4'b1x01` waited 9 and now behaves as the same net with no delay, whose
// default is zero (28.16). iverilog 13 implements no net delay, so the expected
// output is that of the undelayed net (net 0 at t=1, then 1 at t=2).
TEST_CASE("net delay: a delay with an x or z bit is a zero delay") {
    constexpr std::string_view src = R"(
        module m;
            localparam logic [3:0] D = 4'b1x01;
            logic a;
            wire #D w;
            assign w = a;
            initial begin
                a = 0;
                #1 $display("t=%0t w=%b", $time, w);
                a = 1;
                #1 $display("t=%0t w=%b", $time, w);
                #30 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 w=0\nt=2 w=1\n");
}

// A driver's own delay and the net's declared delay are independent stages
// and add: LRM 10.3.3's "shall not be added to the delay of other drivers"
// applies only to a delay embedded IN the net declaration's own assignment
// (the next test), not to a separate `assign #d` beside a `wire #d` net.
// `assign #3 w = a` stages the slot 3 units after a driver-visible change;
// the net's own #5 then delays the resolver's fold-into-net store another 5,
// landing at t(slot change)+5.
TEST_CASE("net delay: stacks with a driver's own continuous-assign delay") {
    constexpr std::string_view src = R"(
        module m;
            logic a;
            wire #5 w;
            assign #3 w = a;
            initial begin
                a = 0;
                #1 $display("t=%0t w=%b", $time, w);
                a = 1;
                repeat (8) #1 $display("t=%0t w=%b", $time, w);
                #5 $finish;
            end
        endmodule)";
    // a=0 at t=0 arms the slot for t=3 (value 0); a=1 at t=1 (before that
    // lands) deschedules it and arms the slot for t=1+3=4 (value 1) instead,
    // so the slot only ever visibly changes once, at t=4. The resolver then
    // arms the net's own #5 from that change, landing at t=4+5=9.
    CHECK(run_capture_finish(src) ==
          "t=1 w=x\nt=2 w=x\nt=3 w=x\nt=4 w=x\nt=5 w=x\nt=6 w=x\nt=7 w=x\nt=8 w=x\nt=9 w=1\n");
}

// A delay embedded in the net-declaration's OWN assignment (`wire #5 w =
// expr;`) is that one driver's continuous-assign delay (LRM 10.3.3's closing
// paragraph), not a net delay -- byte-identical to the same design written
// with a separately-declared wire and a bare `assign #d`.
TEST_CASE("net delay: a net declaration assignment delay is that driver's own delay, not a net delay") {
    constexpr std::string_view declared   = R"(
        module m;
            logic a;
            wire #5 w = a;
            initial begin
                a = 0;
                #1 $display("t=%0t w=%b", $time, w);
                a = 1;
                #7 $display("t=%0t w=%b", $time, w);
                #5 $finish;
            end
        endmodule)";
    constexpr std::string_view equivalent = R"(
        module m;
            logic a;
            wire w;
            assign #5 w = a;
            initial begin
                a = 0;
                #1 $display("t=%0t w=%b", $time, w);
                a = 1;
                #7 $display("t=%0t w=%b", $time, w);
                #5 $finish;
            end
        endmodule)";
    const std::string          out        = run_capture_finish(declared);
    CHECK(out == "t=1 w=x\nt=8 w=1\n");
    CHECK(out == run_capture_finish(equivalent));
}

// A net delay applies to the RESOLVED value of a multiply-driven net (after
// WireRes), not to either driver individually: an undelayed driver's own
// conflict is visible only once the net delay's inertial store for it lands.
TEST_CASE("net delay: applies to the resolved value of a multiply-driven net") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            wire #4 w;
            assign w = a;
            assign w = b;
            initial begin
                a = 0; b = 1'bz;
                #1  $display("t=%0t w=%b", $time, w);
                a = 1;
                #10 $display("t=%0t w=%b", $time, w);
                b = 0;
                #1  $display("t=%0t w=%b", $time, w);
                #10 $display("t=%0t w=%b", $time, w);
                #10 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 w=x\nt=11 w=1\nt=12 w=1\nt=22 w=x\n");
}

// A net delay on a wand net folds its drivers by the net kind's OWN resolver
// (WandRes: bitwise AND), not the generic WireRes an earlier implementation
// mistake would have forced regardless of net type.
TEST_CASE("net delay: a wand net still resolves by wired-AND, not WireRes") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            wand #3 w;
            assign w = a;
            assign w = b;
            initial begin
                a = 1; b = 1;
                #1 $display("t=%0t w=%b", $time, w);
                #5 $display("t=%0t w=%b", $time, w);
                b = 0;
                #1 $display("t=%0t w=%b", $time, w);
                #5 $display("t=%0t w=%b", $time, w);
                #5 $finish;
            end
        endmodule)";
    // 1 & 1 == 1, delayed to t=3; 1 & 0 == 0, delayed to t=9. A WireRes fold
    // would show x instead of 0 once b drops to 0 (mismatched values).
    CHECK(run_capture_finish(src) == "t=1 w=x\nt=6 w=1\nt=7 w=1\nt=12 w=0\n");
}

// A net delay's width is no longer capped at 64 value bits (the backend's
// inertial decide compares via `icmp` on loaded values, not a packed-scalar
// runtime call), including a width whose 4-state mm plane is not a multiple
// of 8 bits -- exercising the exact padding-representation hazard a naive
// byte-memcmp fix would have reintroduced.
TEST_CASE("net delay: width is not capped, including a non-byte-aligned 4-state width") {
    constexpr std::string_view src = R"(
        module m;
            logic [4:0] a;
            wire  [4:0] w;
            wire  [4:0] #2 y;
            assign w = a;
            assign y = w;
            initial begin
                a = 5'b10110;
                #1 $display("t=%0t y=%b", $time, y);
                #3 $display("t=%0t y=%b", $time, y);
                a = 5'b00011; #1 a = 5'b10110; // 1-wide pulse < delay 2: rejected
                #5 $display("t=%0t y=%b", $time, y);
                #1 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 y=xxxxx\nt=4 y=10110\nt=10 y=10110\n");
}

// A 128-bit net delay: the mm plane (256 bits) is far past the old 64-bit
// cap. Cross-checks the exact value, not just that it lowers.
TEST_CASE("net delay: a 128-bit net delay carries the exact value") {
    constexpr std::string_view src = R"(
        module m;
            logic [127:0] a;
            wire  [127:0] w;
            wire  [127:0] #2 y;
            assign w = a;
            assign y = w;
            initial begin
                a = 128'hDEAD_BEEF_0000_0001_1234_5678_9ABC_DEF0;
                #5 $display("y=%h", y);
                #1 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "y=deadbeef00000001123456789abcdef0\n");
}

// Guard: a rise/fall/turn-off delay triple on a plain net delay has no
// Clotho representation (same gap as `assign #(a,b,c)`), LRM 28.16.
TEST_CASE("net delay: multi-value delay is rejected") {
    const std::string err = lower_error(R"(
        module m; logic a; wire #(1,2,3) w; assign w = a; endmodule)");
    CHECK(err.find("rise/fall/turn-off") != std::string::npos);
}

// Guard: same gap for a delay embedded in a net declaration assignment.
TEST_CASE("net delay: multi-value net declaration assignment delay is rejected") {
    const std::string err = lower_error(R"(
        module m; logic a; wire #(1,2,3) w = a; endmodule)");
    CHECK(err.find("rise/fall/turn-off") != std::string::npos);
}

// Guard: a net delay forces the net onto the per-driver-slot/resolver path,
// which only folds a flat packed plane -- an unpacked-aggregate net has no
// such fold (WireRes is packed-only), so it is refused rather than silently
// mishandled.
TEST_CASE("net delay: an unpacked-aggregate net is rejected") {
    const std::string err = lower_error(R"(
        module m;
            logic a, b;
            wire struct {logic x; logic y;} #5 w;
            assign w = '{a,b};
        endmodule)");
    CHECK(err.find("unpacked-aggregate") != std::string::npos);
}

// Guard: same gap for a delay embedded in an aggregate net's own declaration
// assignment.
TEST_CASE("net delay: a delay on an aggregate net declaration assignment is rejected") {
    const std::string err = lower_error(R"(
        module m;
            logic a, b;
            wire struct {logic x; logic y;} #5 w = '{a,b};
        endmodule)");
    CHECK(err.find("aggregate net declaration assignment") != std::string::npos);
}

// No delay: a plain net declaration and a plain net declaration assignment
// still lower (regression coverage for the guard split -- a net with no
// delay at all must never trip either delay branch).
TEST_CASE("net delay: no delay on a net declaration still lowers") {
    const LowerResult rp = lower_text(R"(
        module m(input a);
            wire w = a;
        endmodule
    )");
    CHECK_MESSAGE(rp.ok, rp.diagnostic_text());

    const LowerResult rq = lower_text(R"(
        module m(input a);
            wire w;
            assign w = a;
        endmodule
    )");
    CHECK_MESSAGE(rq.ok, rq.diagnostic_text());
}
