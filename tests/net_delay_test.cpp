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

// -- force and release on a net with a net delay (LRM 10.6.2, 10.3.3, 28.16) --
//
// 28.16 delays "the time it takes from any driver on the net changing value to the
// time when the net value is updated", and 10.6.2 has a force "override all drivers
// of the net ... until a release", after which "the net shall immediately be
// assigned the value determined by the drivers of the net". So the net delay is a
// stage between the drivers and the net value, a force sits on the net value past it
// and takes effect at once (4.9.2: a force is an active update event), a driver
// change that is on its way lands under the force and does not disturb it, and a
// release shows what the drivers have delivered. That is Clotho's reading of clauses
// that do not mention each other (docs/LRM_NOTES.md, note 48); a release while a driver
// change is still on its way is the case the clauses leave open, and the test that
// stops the run covers it. iverilog 13 implements no net delay, so the expected output
// is the LRM's; on a net with ONE driver the net delay is that driver's delay, and
// iverilog 13 prints exactly the lines of the first test for the driver-delay form
// `wire #5 w = a` (tests/force_release_test.cpp).

// a=1 at 10 is due at 15. The force taken at 12 holds through that landing and the
// release at 20 shows the 1. The delayed fold was stored into the net itself, so it
// landed over the force (w=1 at 15, still 1 at 18).
TEST_CASE("net delay: a driver change on its way lands under an active force") {
    constexpr std::string_view src = R"(
        module m;
            logic a;
            wire #5 w;
            assign w = a;
            always @(w) $display("%0t chg w=%b", $time, w);
            initial begin
                a = 0;
                #10 a = 1;
                #2 force w = 1'bz;
                #6 $strobe("%0t s w=%b", $time, w);
                #2 release w;
                $strobe("%0t s w=%b", $time, w);
                #10 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "5 chg w=0\n12 chg w=z\n18 s w=z\n20 chg w=1\n20 s w=1\n");
}

// "Immediately" (10.6.2): the statement after a release already sees the drivers'
// value, for a net with nothing on its way, whatever drives it. The release made no
// store at all when the driver's value was the one last armed, so the net kept the
// forced value for good (w=1 where the driver says 0).
TEST_CASE("net delay: a release returns the net to its drivers at once") {
    constexpr std::string_view single = R"(
        module m;
            logic a;
            wire #5 w;
            assign w = a;
            always @(w) $display("%0t chg w=%b", $time, w);
            initial begin
                a = 0;
                #10 force w = 1'b1;
                #5 release w;
                $strobe("%0t s w=%b", $time, w);
                #10 $strobe("%0t s w=%b", $time, w);
                #1 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(single) == "5 chg w=0\n10 chg w=1\n15 chg w=0\n15 s w=0\n25 s w=0\n");

    // The same for a wired-AND net of two drivers, a net driven by a gate and an
    // undriven tri1 net whose pull is its only driver: all resolve through the fold
    // the net delay follows.
    constexpr std::string_view kinds = R"(
        module m;
            logic a, b;
            wand #4 wa;
            assign wa = a;
            assign wa = b;
            wire #3 wg;
            and g (wg, a, b);
            tri1 #4 wt;
            initial begin
                a = 1; b = 1;
                #10 force wa = 1'b0;
                force wg = 1'b0;
                force wt = 1'b0;
                $display("forced %b%b%b", wa, wg, wt);
                #5 release wa;
                release wg;
                release wt;
                $display("released %b%b%b", wa, wg, wt);
                #10 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(kinds) == "forced 000\nreleased 111\n");
}

// A forced bit or part holds against the landing and each releases on its own: the
// landing at 15 changes only the free bit, and every release shows the delivered
// value of its bits (the net is 1111 once both are free).
TEST_CASE("net delay: a forced bit and part hold against a landing and release separately") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            wire [3:0] #5 w;
            assign w = a;
            always @(w) $display("%0t chg w=%b", $time, w);
            initial begin
                a = 4'b0000;
                #10 a = 4'b1111;
                #2 force w[1] = 1'b0;
                force w[3:2] = 2'b0z;
                #6 $strobe("%0t s w=%b", $time, w);
                #2 release w[1];
                $strobe("%0t s w=%b", $time, w);
                #2 release w[3:2];
                $strobe("%0t s w=%b", $time, w);
                #10 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "5 chg w=0000\n12 chg w=0z00\n15 chg w=0z01\n18 s w=0z01\n"
                                     "20 chg w=0z11\n20 s w=0z11\n22 chg w=1111\n22 s w=1111\n");
}

// A driver's own delay and the net's stack (see "stacks with a driver's own
// continuous-assign delay"): a=1 at 10 reaches the slot at 13 and the net at 18, under
// the force taken at 11, and the release at 25 shows it.
TEST_CASE("net delay: a driver's own delay and the net delay both pass under a force") {
    constexpr std::string_view src = R"(
        module m;
            logic a;
            wire #5 w;
            assign #3 w = a;
            always @(w) $display("%0t chg w=%b", $time, w);
            initial begin
                a = 0;
                #10 a = 1;
                #1 force w = 1'bz;
                #9 $strobe("%0t s w=%b", $time, w);
                #5 release w;
                $strobe("%0t s w=%b", $time, w);
                #10 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "8 chg w=0\n11 chg w=z\n20 s w=z\n25 chg w=1\n25 s w=1\n");
}

// The force's own right-hand side is a continuous relationship (10.6.2) that the net
// delay does not touch: the net follows b at 12 and 16 while a=1 at 14 is on its way
// (due at 19, under the force), and the release at 20 shows it.
TEST_CASE("net delay: a force follows its right-hand side at once, past the net delay") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            wire #5 w;
            assign w = a;
            always @(w) $display("%0t chg w=%b", $time, w);
            initial begin
                a = 0; b = 0;
                #10 force w = b;
                #2 b = 1;
                #2 a = 1;
                #2 b = 0;
                #4 release w;
                #10 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "5 chg w=0\n12 chg w=1\n16 chg w=0\n20 chg w=1\n");
}

// Each instance of a module that holds a delayed, forced net has its own delayed fold:
// s1 is forced and released from inside at 10 and 11, s0 from the top by a hierarchical
// name at 10 and 12, and each release shows its own driver's value (s0 follows x=1, s1
// follows y=0).
TEST_CASE("net delay: every instance of a delayed forced net keeps its own state") {
    constexpr std::string_view src = R"(
        module sub #(parameter bit F = 0) (input logic a);
            wire #3 w;
            assign w = a;
            initial if (F) begin
                #10 force w = 1'bz;
                #1 release w;
                $display("%m inside w=%b", w);
            end
        endmodule
        module top;
            logic x = 1, y = 0;
            sub #(0) s0 (.a(x));
            sub #(1) s1 (.a(y));
            initial begin
                #10 force s0.w = 1'bz;
                #2 release s0.w;
                $display("outside s0.w=%b s1.w=%b", s0.w, s1.w);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "top.s1 inside w=0\noutside s0.w=1 s1.w=0\n");
}

// A release only meets the update on its way for the bits it frees: bit 0 has nothing
// pending at 12, so the release is exact although bits 3:2 are due at 15.
TEST_CASE("net delay: a release is exact for bits that have nothing on their way") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            wire [3:0] #5 w;
            assign w = a;
            initial begin
                a = 4'b0000;
                #6 force w[0] = 1'b1;
                #4 a = 4'b1100;
                #2 release w[0];
                $display("t=%0t w=%b", $time, w);
                #4 $display("t=%0t w=%b", $time, w);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=12 w=0000\nt=16 w=1100\n");
}

// OPEN LRM QUESTION, so refused: a release at 12 while the driver change of 10 is due at
// 15. 10.6.2 says the net "shall immediately be assigned the value determined by the
// drivers of the net", and the drivers say 1 since 10; 10.3.3 says a value change "shall be
// delayed for ten time units before it takes effect" (here five), so the net says 0 until
// 15. The clauses do not say which wins, and the stuck forced value this run used to show
// at 12 is neither. The run stops at the release instead.
TEST_CASE("net delay: a release while a driver change is on its way stops the run") {
    const test::DesignRun run = test::run_design_captured(R"(
        module m;
            logic a;
            wire #5 w;
            assign w = a;
            always @(w) $display("%0t chg w=%b", $time, w);
            initial begin
                a = 0;
                #7 force w = 1'b1;
                #3 a = 1;
                #2 release w;
                $display("not reached %b", w);
                $finish;
            end
        endmodule)");
    CHECK(run.out == "5 chg w=0\n7 chg w=1\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("release of a net while its net delay holds a driver change is not supported") !=
          std::string::npos);
}

// A rise/fall delay pair is refused for a net that is forced as for any other (the
// triple form is the test above); the force does not give it a meaning.
TEST_CASE("net delay: a two-value delay on a forced net is rejected") {
    const std::string err = lower_error(R"(
        module m;
            logic a;
            wire #(3,4) w;
            assign w = a;
            initial begin force w = 1'b1; release w; end
        endmodule)");
    CHECK(err.find("rise/fall/turn-off") != std::string::npos);
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

// The same for a plain unpacked ARRAY of nets, driven or not: a driverless
// `tri1 [3:0] #5 t [0:1]` took the delayed resolver store, which wrote element 0
// only, so t[1] read x where LRM 6.6.5 gives the pull (1111). A driven one already
// failed at its driver; the declaration is what is refused now.
TEST_CASE("net delay: an unpacked array of nets is rejected") {
    for (const std::string_view decl :
         { "tri1 [3:0] #5 t [0:1];", "wire [3:0] #3 w [0:1];", "wire [3:0] #3 w [0:1][0:1];", "wand #2 w [0:2];" }) {
        const std::string err = lower_error("module m;\n " + std::string(decl) + "\nendmodule");
        CHECK_MESSAGE(err.find("unpacked-array or unpacked-aggregate net") != std::string::npos, err, " for ", decl);
    }
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
