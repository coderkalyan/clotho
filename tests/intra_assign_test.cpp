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

// IEEE 1800-2023 9.4.5: for `lhs = #d rhs` the RHS is evaluated in the active
// region, the process blocks d time units, then the sampled value is stored. A
// later change to the RHS operand during the delay must not affect the result.
// Verified against iverilog 13.
TEST_CASE("blocking intra-assignment delay samples the RHS before the delay") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                b = 8'd10;
                a = #12 b;
                $display("a=%0d t=%0t", a, $time);
                $finish;
            end
            initial begin
                #3 b = 8'd20;
                #4 b = 8'd30;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=10 t=12\n");
}

// A non-constant sampled value must survive the suspend: CoroSplit spills the
// pre-delay SSA value into the process frame and reloads it after the delay. A
// free-running counter makes the sample non-foldable; sampled at t=3 (cnt=1) it
// must print 101 even though cnt has advanced by the store at t=8. Deliberately
// off the counter's update phase (#2 vs #3) to avoid a same-time race. iverilog
// prints 101.
TEST_CASE("blocking intra-assignment delay preserves a non-constant sample across the suspend") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, cnt = 0;
            always #2 cnt = cnt + 1;
            initial begin
                #3;
                a = #5 (cnt + 8'd100);
                $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=101\n");
}

// The LHS (including any index) is evaluated at assignment time, after the
// suspend — LRM 9.4.5's equivalent form assigns `lhs = tmp` post-delay. The
// bit index changes during the delay, so vec[3] is written at t=5. iverilog
// prints 8.
TEST_CASE("blocking intra-assignment delay resolves the LHS after the suspend") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] vec = 0;
            logic [2:0] idx = 0;
            initial begin
                vec[idx] = #5 1'b1;
                $display("vec=%0d", vec);
                $finish;
            end
            initial #2 idx = 3;
        endmodule)";
    CHECK(run_capture_finish(src) == "vec=8\n");
}

// `lhs = @(posedge clk) rhs`: sample now, block until the edge, store. The RHS
// operand changes before the edge but the stored value is the pre-wait sample.
// iverilog prints 77 at the first posedge (t=5).
TEST_CASE("blocking intra-assignment event samples the RHS before the wait") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] c, d;
            logic clk = 0;
            always #5 clk = ~clk;
            initial begin
                d = 8'd77;
                c = @(posedge clk) d;
                $display("c=%0d t=%0t", c, $time);
                $finish;
            end
            initial #2 d = 8'd99;
        endmodule)";
    CHECK(run_capture_finish(src) == "c=77 t=5\n");
}

// `lhs = repeat(N) @(posedge clk) rhs`: sample now, block for N edges, store.
// Posedges at 5/15/25 -> store at t=25 with the t=0 sample. iverilog prints 1.
TEST_CASE("blocking intra-assignment repeat-event waits N edges then stores the sample") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] e, f;
            logic clk = 0;
            always #5 clk = ~clk;
            initial begin
                f = 8'd1;
                e = repeat(3) @(posedge clk) f;
                $display("e=%0d t=%0t", e, $time);
                $finish;
            end
            initial #6 f = 8'd2;
        endmodule)";
    CHECK(run_capture_finish(src) == "e=1 t=25\n");
}

// A real repeat count rounds half away from zero (LRM 6.12.1), so 2.4 waits two
// edges (t=15) and 2.5 waits three (t=25) -- the two differ by one edge, which is
// what pins the rounding rather than a truncation. A count that rounds negative is
// 9.4.5's "the assignment occurs as if there is no repeat construct": no wait at
// all, so the store lands at t=0. Each expectation is the integral count it rounds
// to, run against iverilog 13 (which cannot compile the real form itself).
TEST_CASE("blocking intra-assignment repeat-event rounds a real count") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b, c;
            logic clk = 0;
            always #5 clk = ~clk;
            initial begin a = repeat(2.4)  @(posedge clk) 8'd7; $display("a=%0d t=%0t", a, $time); end
            initial begin b = repeat(2.5)  @(posedge clk) 8'd9; $display("b=%0d t=%0t", b, $time); end
            initial begin c = repeat(-0.6) @(posedge clk) 8'd5; $display("c=%0d t=%0t", c, $time); end
            initial #40 $finish;
        endmodule)";
    CHECK(run_capture_finish(src) == "c=5 t=0\na=7 t=15\nb=9 t=25\n");
}

// -- Nonblocking intra-assignment delay (`x <= #d y`, LRM 9.4.5) --
//
// The process does not block: the RHS is sampled now and the update is committed
// in the NBA region of `now + d` (docs/design/intra-assignment-timing.md). All
// cases below are pinned against iverilog 13.

// Region ordering: an active-region read at `t+d` sees the OLD value; the deferred
// update lands afterward, in the NBA region. Committing at wheel-fire time (before
// the active region) would print 0. iverilog prints 1.
TEST_CASE("nonblocking intra-delay commits in the NBA region, not at wheel-fire") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, x;
            initial begin
                a = 1;
                a <= #5 0;
                #5 x = a;
                $display("x=%0d", x);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "x=1\n");
}

// The RHS is sampled at the statement's instant, not when the update lands: a
// source change during the delay does not affect the stored value. iverilog
// prints 11 (10 sampled at t=0, +1), not 100 (99 + 1 at t=12).
TEST_CASE("nonblocking intra-delay samples the RHS now") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] b;
            initial begin b = 8'd10; b <= #12 (b + 1); end
            initial #3 b = 8'd99;
            initial begin #20; $display("b=%0d", b); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "b=11\n");
}

// The process does not block: it continues past the `<=` immediately. Here the
// same process schedules a delayed update and then keeps running in zero time.
TEST_CASE("nonblocking intra-delay does not block the process") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial begin
                a = 8'd1;
                a <= #5 8'd9;
                $display("after=%0d t=%0t", a, $time);   // a still 1, time still 0
                #10 $display("later=%0d", a);            // now 9
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "after=1 t=0\nlater=9\n");
}

// Two updates to the same signal firing at the same instant commit in
// statement-execution order — last executed wins (LRM 10.4.2), not wheel-fire
// order. iverilog prints 2.
TEST_CASE("nonblocking intra-delay same-instant updates apply last-executed-wins") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] mm;
            initial begin
                mm = 0;
                mm <= #5 8'd1;   // executed first  -> fires at t=5
                #2;
                mm <= #3 8'd2;   // executed second -> also fires at t=5
            end
            initial begin #10; $display("m=%0d", mm); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "m=2\n");
}

// A zero delay cannot arm the wheel; the update lands in the current instant's
// NBA region. iverilog leaves b == 2 by the end.
TEST_CASE("nonblocking intra-delay with #0 commits in the current NBA region") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] b;
            initial begin b = 8'd1; b <= #0 8'd2; end
            initial begin #5; $display("b=%0d", b); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "b=2\n");
}

// A compile-time `#0` lowers as a plain NBA, so a `#0` update and a plain NBA to
// the same signal in the same instant order by statement-execution order — last
// written wins (LRM 10.4.2), regardless of which carries the `#0`. iverilog: the
// plain `a <= 2` executes last, so a == 2.
TEST_CASE("nonblocking intra-delay #0 orders with plain NBAs by execution order") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial begin a = 0; a <= #0 1; a <= 2; end
            initial begin #1; $display("a=%0d", a); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=2\n");
}

// The same holds when the delay is zero only at RUN time (LRM 10.4.2: a nonblocking
// assignment with a zero delay is scheduled like one without, in execution order;
// 4.9.4: the update is an NBA event in the current time step). It used to commit
// through the deferred record, after every plain NBA of the instant, so `a` ended
// at 1. An x or z delay is a zero delay too (LRM 9.4.1), and so is a real that
// rounds to zero ticks. The `g` line has the zero-delay store LAST: it wins. A
// nonzero runtime delay (`h`) still commits later, after the plain `h <= 2`.
// iverilog prints the same.
TEST_CASE("nonblocking intra-delay that is zero at run time orders with plain NBAs") {
    constexpr std::string_view src = R"(
        `timescale 1ns/1ns
        module m;
            logic [7:0] a, c, d, g, h;
            int n = 0, n5 = 5;
            logic [3:0] dx = 4'b1x01;
            initial begin
                a = 0; a <= #(n) 1; a <= 2;
                c = 0; c <= #(dx) 1; c <= 2;
                d = 0; d <= #(0.4) 1; d <= 2;
                g = 0; g <= 2; g <= #(n) 1;
                h = 0; h <= #(n5) 1; h <= 2;
                #1 $display("a=%0d c=%0d d=%0d g=%0d h=%0d", a, c, d, g, h);
                #10 $display("h=%0d", h);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=2 c=2 d=2 g=1 h=2\nh=1\n");
}

// A target that decomposes into several leaves arms one record per leaf against
// ONE evaluation of the delay (LRM 9.4.5), so a zero delay routes every leaf to the
// plain `<=` path. iverilog prints p=3 q=4.
TEST_CASE("nonblocking intra-delay that is zero at run time on a concatenation") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] p, q;
            int n = 0;
            initial begin
                p = 0; q = 0;
                {p, q} <= #(n) 16'h0102;
                {p, q} <= 16'h0304;
                #1 $display("p=%0d q=%0d", p, q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "p=3 q=4\n");
}

// A dynamic (runtime-valued) delay is supported. iverilog: a == 9 at the end.
TEST_CASE("nonblocking intra-delay accepts a dynamic delay") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            int n = 4;
            initial begin a = 8'd5; a <= #(n + 1) 8'd9; end
            initial begin #20; $display("a=%0d", a); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=9\n");
}

// A pipeline: q sampled each posedge and committed a delay later, so several
// updates are in flight at once. iverilog prints q=6 d=8.
TEST_CASE("nonblocking intra-delay pipelines multiple in-flight updates") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] d, q;
            logic       clk = 0;
            int         cyc = 0;
            always #5 clk = ~clk;
            initial begin d = 0; q = 0; end
            always @(posedge clk) begin
                d   <= d + 1;
                q   <= #12 d;
                cyc <= cyc + 1;
                if (cyc == 8) begin
                    $display("q=%0d d=%0d", q, d);
                    $finish;
                end
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "q=6 d=8\n");
}

// A 4-state value with x bits propagates through the deferred store unchanged.
TEST_CASE("nonblocking intra-delay carries a 4-state x value") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] xr, xs;
            initial begin xs = 4'bxx01; xr <= #3 xs; end
            initial begin #10; $display("xr=%b", xr); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "xr=xx01\n");
}

// -- Nonblocking intra-assignment event / repeat (`x <= @e y`, LRM 9.4.5) --
//
// The RHS is sampled now; the store lands after N occurrences of the trigger
// edge, committed in that edge instant's NBA region. All pinned against iverilog.

// Plain `<= @(posedge clk)`: sample d now, store to q at the next posedge.
TEST_CASE("nonblocking intra-delay event stores at the next matching edge") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] d, q;
            logic       clk = 0;
            always #5 clk = ~clk;
            initial begin d = 0; q = 0; end
            always @(posedge clk) d <= d + 1;
            always @(posedge clk) q <= @(posedge clk) d;
            initial begin #48; $display("d=%0d q=%0d", d, q); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "d=5 q=3\n");
}

// `repeat(N) @e` stores after N occurrences of the edge. iverilog: r == 42.
TEST_CASE("nonblocking intra-delay repeat-event stores after N edges") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] r;
            logic       clk = 0;
            always #5 clk = ~clk;
            initial begin r = 0; #1 r <= repeat(2) @(posedge clk) 8'd42; end
            initial begin #48; $display("r=%0d", r); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "r=42\n");
}

// The nonblocking form rounds its real count the same way (LRM 6.12.1): 1.6 rounds
// to 2, so the record commits at the SECOND posedge after the arm at t=1. The pair
// of samples straddles that edge -- r is still 0 at t=14 and 42 at t=16 -- which a
// truncated count of 1 would have committed at t=5 instead. Times pinned against
// iverilog 13 with the integral count 1.6 rounds to.
TEST_CASE("nonblocking intra-delay repeat-event rounds a real count") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] r;
            logic       clk = 0;
            always #5 clk = ~clk;
            initial begin r = 0; #1 r <= repeat(1.6) @(posedge clk) 8'd42; end
            initial begin #14 $display("t=%0t r=%0d", $time, r); #2 $display("t=%0t r=%0d", $time, r); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=14 r=0\nt=16 r=42\n");
}

// The trigger edge may come from an NBA commit (`clk <= ~clk`), not just a
// blocking store — the record-fire hook is in both wake paths. iverilog: d=8 q=6.
TEST_CASE("nonblocking intra-delay event fires on an NBA-driven trigger edge") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] d, q;
            logic       clk = 0, go = 0;
            always #5 go  = ~go;
            always @(go) clk <= ~clk;
            initial begin d = 0; q = 0; end
            always @(posedge clk) d <= d + 1;
            always @(posedge clk) q <= @(posedge clk) d;
            initial begin #80; $display("d=%0d q=%0d", d, q); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "d=8 q=6\n");
}

// Multiple arms before the first edge all sample now and all fire at that edge;
// last executed wins (LRM 10.4.2). A source change after sampling is ignored.
// iverilog: a == 9 (7 sampled at t=0, +2 from the last arm).
TEST_CASE("nonblocking intra-delay event samples now and orders same-edge arms") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, s;
            logic       clk = 0;
            always #5 clk = ~clk;
            initial begin
                s = 8'd7;
                a <= @(posedge clk) s;
                a <= @(posedge clk) s + 1;
                a <= @(posedge clk) s + 2;
            end
            initial #1 s = 8'd99;
            initial begin #8; $display("a=%0d", a); $finish; end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=9\n");
}

// A multi-signal trigger `@(a or b)`: the whole list is the event, so the FIRST
// edge to bring the countdown to zero matures the store. The record then has to be
// removed from the other trigger's list -- asserted here by firing that other
// trigger afterwards and observing no second commit, which is the case a "mark it
// matured and skip it" implementation gets wrong once the record is recycled.
// Verified against iverilog 13.
TEST_CASE("nonblocking intra-assignment multi-signal trigger: first edge wins") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            logic [7:0] x, y;
            initial begin
                a = 0; b = 0; x = 0; y = 8'h33;
                #1 x <= @(posedge a or posedge b) y;
                #1 a = 1;
                #1 $display("t=%0t x=%h", $time, x);
                y = 8'h77; x = 0;
                #1 b = 1;
                #1 $display("t=%0t x=%h", $time, x);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=3 x=33\nt=5 x=00\n");
}

// `repeat(N) @(a or b)` counts an occurrence of EITHER trigger: the countdown
// lives in the one record, which both lists hold. Verified against iverilog 13.
TEST_CASE("nonblocking intra-assignment repeat over a multi-signal trigger") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            logic [7:0] x, y;
            initial begin
                a = 0; b = 0; x = 0; y = 8'h44;
                #1 x <= repeat(3) @(posedge a or posedge b) y;
                #1 a = 1; #1 a = 0;
                #1 b = 1; #1 b = 0;
                #1 $display("t=%0t x=%h", $time, x);
                #1 a = 1;
                #1 $display("t=%0t x=%h", $time, x);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 x=00\nt=8 x=44\n");
}

// A decomposing target under a multi-signal trigger: every leaf record registers
// on the same list pair and matures together, and first-fire-wins removal has to
// hold for each of them independently. Verified against iverilog 13.
TEST_CASE("nonblocking intra-assignment concat target with a multi-signal trigger") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            logic [3:0] p, q;
            logic [7:0] x;
            initial begin
                a = 0; b = 0; p = 0; q = 0; x = 8'h5c;
                #1 {p, q} <= @(posedge a or posedge b) x;
                x = 8'hff;
                #1 b = 1;
                #1 $display("t=%0t p=%h q=%h", $time, p, q);
                p = 0; q = 0;
                #1 a = 1;
                #1 $display("t=%0t p=%h q=%h", $time, p, q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=3 p=5 q=c\nt=5 p=0 q=0\n");
}

// A multi-signal trigger inside a module instantiated TWICE. A dense trigger id
// is a per-instance link-time constant (abs_id_const) while twin instances share
// the emitted code, so the id list the record registers on -- and is removed from
// -- has to be built per instance. It lives in the record's own tail for exactly
// this reason; a compile-time constant global would bake u0's ids into u1. Each
// instance must respond only to its own triggers. Verified against iverilog 13.
TEST_CASE("nonblocking intra-assignment multi-signal trigger in twin instances") {
    constexpr std::string_view src = R"(
        module sub(input logic a, input logic b, output logic [7:0] o);
            initial begin
                o = 0;
                #1 o <= @(posedge a or posedge b) 8'h9e;
            end
        endmodule
        module m;
            logic a0 = 0, b0 = 0, a1 = 0, b1 = 0;
            logic [7:0] o0, o1;
            sub u0(a0, b0, o0);
            sub u1(a1, b1, o1);
            initial begin
                #2 a0 = 1;
                #1 $display("t=%0t o0=%h o1=%h", $time, o0, o1);
                #1 b1 = 1;
                #1 $display("t=%0t o0=%h o1=%h", $time, o0, o1);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=3 o0=9e o1=00\nt=5 o0=9e o1=9e\n");
}

// A whole real / shortreal target rides the same narrow deferred record as an
// integral scalar (the IEEE pattern via intra_pack_mm). Delay form only; the
// event form on a real is still a documented gap. Checked vs iverilog.
TEST_CASE("nonblocking intra-delay to a real target") {
    constexpr std::string_view src = R"(
        module m;
            real r, q;
            initial begin
                r <= #2 1.5;
                q <= #1 3.25;
                #3 $display("r=%g q=%g", r, q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "r=1.5 q=3.25\n");
}

// A bit-select target commits through the deferred record's write mask: the rest
// of the vector is untouched, and the write lands at t=5 (not before). Verified
// against iverilog 13.
TEST_CASE("nonblocking intra-delay to a constant bit-select target") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a = 8'h00;
            initial begin
                a[3] <= #5 1'b1;
                #4 $display("t=%0t a=%b", $time, a);
                #2 $display("t=%0t a=%b", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=4 a=00000000\nt=6 a=00001000\n");
}

// A constant part-select target: only the selected nibble is replaced, so the
// upper nibble of a5 survives as af. Verified against iverilog 13.
TEST_CASE("nonblocking intra-delay to a constant part-select target") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a = 8'hA5;
            initial begin
                a[3:0] <= #5 4'hF;
                #4 $display("t=%0t a=%h", $time, a);
                #2 $display("t=%0t a=%h", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=4 a=a5\nt=6 a=af\n");
}

// A string / whole-array target with intra-assignment timing is not yet lowered
// (the string/array store paths need reordering across the suspend) and must
// fail loudly.
TEST_CASE("intra-assignment timing on a string target is rejected") {
    constexpr std::string_view src = R"(
        module m;
            string s, t;
            initial s = #5 t;
        endmodule)";
    CHECK(lower_error(src).find("string or array target") != std::string::npos);
}

// LRM 9.4.5 evaluates the LHS index when the statement executes and defers only
// the update, so changing `i` right after the arm must not move the target bit:
// bit 5 is set even though i is 0 by the time the write lands. Verified against
// iverilog 13.
TEST_CASE("nonblocking intra-delay to a variable-index bit-select samples the index at arm time") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a = 8'h00;
            int i;
            initial begin
                i = 5;
                a[i] <= #5 1'b1;
                i = 0;
                #6 $display("t=%0t a=%b i=%0d", $time, a, i);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 a=00100000 i=0\n");
}

// An indexed (`+:`) part-select target, index likewise sampled at arm time: the
// nibble lands at [7:4] for i=4, not at [15:12] for the later i=12. Verified
// against iverilog 13.
TEST_CASE("nonblocking intra-delay to an indexed part-select target") {
    constexpr std::string_view src = R"(
        module m;
            logic [15:0] a = 16'h0000;
            int i;
            initial begin
                i = 4;
                a[i +: 4] <= #5 4'hD;
                i = 12;
                #6 $display("t=%0t a=%h", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 a=00d0\n");
}

// An unpacked-array element target: the record carries the element pointer the
// index resolved to at arm time, so mem[2] takes the write even though i has
// since changed, and its neighbours are untouched. Verified against iverilog 13.
TEST_CASE("nonblocking intra-delay to a variable-index unpacked-array element") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] mem [0:3];
            int i;
            initial begin
                mem[0] = 8'h11; mem[1] = 8'h22; mem[2] = 8'h33; mem[3] = 8'h44;
                i = 2;
                mem[i] <= #5 8'hAB;
                i = 0;
                #6 $display("t=%0t m0=%h m1=%h m2=%h m3=%h", $time, mem[0], mem[1], mem[2], mem[3]);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 m0=11 m1=22 m2=ab m3=44\n");
}

// LRM 7.4.5: a write through an out-of-range index performs no operation. The arm
// must stage nothing at all -- a record would otherwise carry a destination
// pointer past the end of the array that the NBA region dereferences
// unconditionally. Verified against iverilog 13.
TEST_CASE("nonblocking intra-delay to an out-of-range array element is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] mem [0:3];
            int i;
            initial begin
                mem[0] = 8'h11; mem[1] = 8'h22; mem[2] = 8'h33; mem[3] = 8'h44;
                i = 9;
                mem[i] <= #5 8'hAB;
                #6 $display("t=%0t m0=%h m1=%h m2=%h m3=%h", $time, mem[0], mem[1], mem[2], mem[3]);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 m0=11 m1=22 m2=33 m3=44\n");
}

// A packed struct field is a part-select of a flat integral signal, so it rides
// the masked scalar path: writing .lo leaves .hi alone (a5 -> a3). Verified
// against iverilog 13.
TEST_CASE("nonblocking intra-delay to a packed struct field") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct packed { logic [3:0] hi; logic [3:0] lo; } pk_t;
            pk_t p;
            initial begin
                p = 8'hA5;
                p.lo <= #5 4'h3;
                #4 $display("t=%0t p=%h", $time, p);
                #2 $display("t=%0t p=%h hi=%h lo=%h", $time, p, p.hi, p.lo);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=4 p=a5\nt=6 p=a3 hi=a lo=3\n");
}

// An unpacked struct field is an aggregate leaf: the record carries the leaf slot
// pointer and replays it by memcpy, leaving the sibling field alone. iverilog 13
// cannot be the oracle here ("sorry: Unpacked structs not supported"), so the
// expectation is LRM 9.4.5 directly, matching the packed case above which
// iverilog does confirm.
TEST_CASE("nonblocking intra-delay to an unpacked struct field") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { logic [7:0] a; logic [7:0] b; } st_t;
            st_t s;
            initial begin
                s.a = 8'h11; s.b = 8'h22;
                s.b <= #5 8'hEE;
                #4 $display("t=%0t a=%h b=%h", $time, s.a, s.b);
                #2 $display("t=%0t a=%h b=%h", $time, s.a, s.b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=4 a=11 b=22\nt=6 a=11 b=ee\n");
}

// The event form (`<= @e`) on a part-select target: same masked record, fired by
// the trigger edge instead of the wheel. Verified against iverilog 13.
TEST_CASE("nonblocking intra-delay event to a part-select target") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a = 8'h00;
            logic clk = 0;
            initial begin
                a[7:4] <= @(posedge clk) 4'h9;
                #5 clk = 1;
                #1 $display("t=%0t a=%h", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 a=90\n");
}

// Two records on one signal firing in the same instant must compose: each masked
// read-modify-write merges only its own bit, so both survive (0x42). A commit
// that stored whole words would keep just the last one. Verified against
// iverilog 13.
TEST_CASE("two nonblocking intra-delays to different bits of one signal both land") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a = 8'h00;
            initial begin
                a[1] <= #5 1'b1;
                a[6] <= #5 1'b1;
                #6 $display("t=%0t a=%b", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 a=01000010\n");
}

// The z plane is masked independently of the must|may word, so an x/z payload in
// a part-select must land bit-exact without disturbing the bits outside the
// select. Verified against iverilog 13.
TEST_CASE("nonblocking intra-delay to a part-select carries the x and z planes") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a = 8'h00;
            initial begin
                a[3:0] <= #5 4'bz1x0;
                #6 $display("t=%0t a=%b", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 a=0000z1x0\n");
}

// A within-element part-select under a variable element index exercises the mask
// and the record-carried destination pointer together: only mem[1]'s low nibble
// clears. Verified against iverilog 13.
TEST_CASE("nonblocking intra-delay to a part-select of a variable-index array element") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] mem [0:2];
            int i;
            initial begin
                mem[0] = 8'hFF; mem[1] = 8'hFF; mem[2] = 8'hFF;
                i = 1;
                mem[i][3:0] <= #5 4'h0;
                i = 0;
                #6 $display("t=%0t m0=%h m1=%h m2=%h", $time, mem[0], mem[1], mem[2]);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 m0=ff m1=f0 m2=ff\n");
}

// A string target has no plain-NBA lvalue encoding for the deferred record to
// reuse, so the nonblocking form must still fail loudly (the blocking form is
// caught by the separate string/array guard above).
TEST_CASE("nonblocking intra-delay to a string target is rejected") {
    constexpr std::string_view src = R"(
        module m;
            string s, t;
            initial s <= #5 t;
        endmodule)";
    CHECK(lower_error(src).find("unpacked-array element, or a struct field") != std::string::npos);
}

// A real target on the EVENT form. The delay form already worked, and both arms
// fill the same deferred record through fill_intra_payload, whose first act is
// real_to_planes -- so the frontend guard that named an "event arm/fire path"
// real gap described a drift that never existed. Values verified against
// iverilog 13: the store lands at the posedge (t=10), not when the statement ran.
TEST_CASE("nonblocking intra-assignment event timing to a real target") {
    constexpr std::string_view src = R"(
        module m;
            real r;
            logic clk = 0;
            initial begin
                r = 1.5;
                #1 r <= @(posedge clk) 2.75;
                #5 $display("t=%0t r=%0.2f", $time, r);
                #5 clk = 1;
                #1 $display("t=%0t r=%0.2f", $time, r);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=6 r=1.50\nt=12 r=2.75\n");
}

// The repeat-event form on a real target: the countdown rides the same record,
// so the store lands on the SECOND posedge (t=15), matching iverilog 13.
TEST_CASE("nonblocking intra-assignment repeat-event timing to a real target") {
    constexpr std::string_view src = R"(
        module m;
            real r;
            logic clk = 0;
            initial forever #5 clk = ~clk;
            initial begin
                r = 1.0;
                r <= repeat (2) @(posedge clk) 3.25;
                #7  $display("t=%0t r=%0.2f", $time, r);
                #10 $display("t=%0t r=%0.2f", $time, r);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=7 r=1.00\nt=17 r=3.25\n");
}

// -- Decomposing targets (docs/design/intra-assignment-timing.md, R1) ----------
//
// A target that names N places lowers to N deferred records over ONE sampled RHS
// and ONE evaluation of the delay / count (LRM 9.4.5: "the right-hand expression
// shall be evaluated before the delay"). Every case below mutates the RHS during
// the delay, so a second evaluation would show.

// `{a,b} <= #5 x`: the concatenation walk plain NBA already uses, emitting a
// deferred record per operand. Byte-checked against iverilog 13.
TEST_CASE("nonblocking intra-delay to a concatenation target") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            logic [7:0] x;
            initial begin
                a = 4'h0; b = 4'h0; x = 8'hab;
                #1 {a, b} <= #5 x;
                x = 8'hcd;
                #4 $display("t=%0t a=%h b=%h", $time, a, b);
                #2 $display("t=%0t a=%h b=%h", $time, a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=5 a=0 b=0\nt=7 a=a b=b\n");
}

// A SELF-OVERLAPPING concatenation, `{a, a[0]} <= #5 x`: two records land on one
// signal, and they must apply in leaf order so the answer matches the plain `<=`
// form exactly -- there is one answer to explain, not two. The records take
// consecutive seq stamps in walk order, which is what makes the whole-`a` write
// (later in the walk) win over the `a[0]` write. iverilog 13 gives 5 for both
// forms; so must clotho.
TEST_CASE("nonblocking intra-delay to a self-overlapping concatenation matches plain NBA") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            logic [7:0] x;
            initial begin
                a = 4'h0; x = 8'hab;
                #1 {a, a[0]} <= #5 x;
                #6 $display("intra a=%h", a);
                a = 4'h0;
                #1 {a, a[0]} <= x;
                #1 $display("plain a=%h", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "intra a=5\nplain a=5\n");
}

// A whole unpacked STRUCT target: one sample of the source struct, one delay,
// one record per field. iverilog rejects unpacked structs outright, so the oracle
// is the hand-decomposed per-field form (`sp <= #5 rp; sq <= #5 rq;`), which is
// what the clause makes this equivalent to; that form prints the same two lines
// under iverilog 13.
TEST_CASE("nonblocking intra-delay to a whole unpacked struct target") {
    constexpr std::string_view src = R"(
        typedef struct { logic [3:0] p; logic [3:0] q; } s_t;
        module m;
            s_t s, r;
            initial begin
                s = '{4'h0, 4'h0};
                r = '{4'h1, 4'h2};
                #1 s <= #5 r;
                r = '{4'h9, 4'h9};
                #4 $display("t=%0t p=%h q=%h", $time, s.p, s.q);
                #2 $display("t=%0t p=%h q=%h", $time, s.p, s.q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=5 p=0 q=0\nt=7 p=1 q=2\n");
}

// A whole unpacked ARRAY target: one record per element, all sharing the sample
// and the deadline. Same oracle argument as the struct case -- iverilog rejects a
// whole-array assignment even without timing, and its per-element form
// (`mem[0] <= #5 other[0]; ...`) prints exactly these two lines.
TEST_CASE("nonblocking intra-delay to a whole unpacked array target") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] mem [0:2];
            logic [7:0] other [0:2];
            initial begin
                mem[0] = 0; mem[1] = 0; mem[2] = 0;
                other[0] = 8'd10; other[1] = 8'd11; other[2] = 8'd12;
                #1 mem <= #5 other;
                other[0] = 8'd99; other[1] = 8'd99; other[2] = 8'd99;
                #4 $display("t=%0t %0d %0d %0d", $time, mem[0], mem[1], mem[2]);
                #2 $display("t=%0t %0d %0d %0d", $time, mem[0], mem[1], mem[2]);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=5 0 0 0\nt=7 10 11 12\n");
}

// A streaming-concatenation target, `{>>{a,b}} <= #5 x`. `>>` packs left to
// right, so `a` takes the high nibble. iverilog has no streaming-concatenation
// support, so this is derived from LRM 11.4.14.3 plus the concat case above --
// the same unpack walk, only the bit apportionment differs.
TEST_CASE("nonblocking intra-delay to a streaming-concatenation target") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            logic [7:0] x;
            initial begin
                a = 4'h0; b = 4'h0; x = 8'hab;
                #1 {>>{a, b}} <= #5 x;
                x = 8'hcd;
                #4 $display("t=%0t a=%h b=%h", $time, a, b);
                #2 $display("t=%0t a=%h b=%h", $time, a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=5 a=0 b=0\nt=7 a=a b=b\n");
}

// An assignment-pattern lvalue over a packed-struct source (LRM 10.9): positional,
// one record per member. iverilog does not parse a pattern lvalue, so this is
// derived from 10.9 -- element order equals concatenation order, so the expected
// split is the concat case's.
TEST_CASE("nonblocking intra-delay to an assignment-pattern lvalue") {
    constexpr std::string_view src = R"(
        typedef struct packed { logic [3:0] p; logic [3:0] q; } p_t;
        module m;
            logic [3:0] a, b;
            p_t         v;
            initial begin
                a = 4'h0; b = 4'h0; v = 8'hab;
                #1 '{a, b} <= #5 v;
                v = 8'hcd;
                #4 $display("t=%0t a=%h b=%h", $time, a, b);
                #2 $display("t=%0t a=%h b=%h", $time, a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=5 a=0 b=0\nt=7 a=a b=b\n");
}

// The EVENT form of a decomposing target, so the shared-COUNT path is exercised
// and not only the shared-deadline one: all N records register on the trigger with
// the same countdown and mature on the same edge. Byte-checked against iverilog 13.
TEST_CASE("nonblocking intra-event to a concatenation target") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            logic [3:0] a, b;
            logic [7:0] x;
            initial forever #5 clk = ~clk;
            initial begin
                a = 4'h0; b = 4'h0; x = 8'hab;
                #1 {a, b} <= @(posedge clk) x;
                x = 8'hcd;
                #3 $display("t=%0t a=%h b=%h", $time, a, b);
                #4 $display("t=%0t a=%h b=%h", $time, a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=4 a=0 b=0\nt=8 a=a b=b\n");
}

// A whole unpacked struct on the repeat-event form: one count evaluation shared by
// every field's record. Oracle is again the hand-decomposed per-field form, which
// iverilog 13 runs and which prints these two lines.
TEST_CASE("nonblocking intra-repeat-event to a whole unpacked struct target") {
    constexpr std::string_view src = R"(
        typedef struct { logic [3:0] p; logic [3:0] q; } s_t;
        module m;
            logic clk = 0;
            s_t   s, r;
            initial forever #5 clk = ~clk;
            initial begin
                s = '{4'h0, 4'h0};
                r = '{4'h3, 4'h4};
                #1 s <= repeat(2) @(posedge clk) r;
                r = '{4'h9, 4'h9};
                #10 $display("t=%0t p=%h q=%h", $time, s.p, s.q);
                #10 $display("t=%0t p=%h q=%h", $time, s.p, s.q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=11 p=0 q=0\nt=21 p=3 q=4\n");
}

// A whole unpacked array on the event form.
TEST_CASE("nonblocking intra-event to a whole unpacked array target") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            logic [7:0] mem [0:2];
            logic [7:0] other [0:2];
            initial forever #5 clk = ~clk;
            initial begin
                mem[0] = 0; mem[1] = 0; mem[2] = 0;
                other[0] = 8'd10; other[1] = 8'd11; other[2] = 8'd12;
                #1 mem <= @(posedge clk) other;
                other[0] = 8'd99; other[1] = 8'd99; other[2] = 8'd99;
                #3 $display("t=%0t %0d %0d %0d", $time, mem[0], mem[1], mem[2]);
                #4 $display("t=%0t %0d %0d %0d", $time, mem[0], mem[1], mem[2]);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=4 0 0 0\nt=8 10 11 12\n");
}

// A compile-time `#0` on a decomposing target is a plain `<=`, so it lands in the
// arming instant's NBA region -- the null-timing walk, not N deferred records.
TEST_CASE("nonblocking intra-delay #0 to a concatenation target commits in the current instant") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            logic [7:0] x;
            initial begin
                a = 4'h0; b = 4'h0; x = 8'hab;
                #1 {a, b} <= #0 x;
                $display("t=%0t active a=%h b=%h", $time, a, b);
                #1 $display("t=%0t next   a=%h b=%h", $time, a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 active a=0 b=0\nt=2 next   a=a b=b\n");
}

// -- The repeat-count clamp (LRM 9.4.5, R6) -----------------------------------
//
// "If the repeat count literal, or signed variable holding the repeat count, is
// less than or equal to 0 at the time of evaluation, the assignment occurs as if
// there is no repeat construct", and the clause's example fixes what that means:
// "repeat (-3) @ (event_expression) // will not execute event_expression". So the
// store happens with no wait -- the current instant's NBA region -- which is what
// the blocking form's signed bound already does. All byte-checked against
// iverilog 13.

TEST_CASE("nonblocking intra-repeat with a negative signed count stores in the current instant") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            logic [7:0] x, w, y;
            integer     n;
            initial forever #5 clk = ~clk;
            initial begin
                x = 0; w = 0; y = 8'h55; n = -1;
                #1;
                x <= repeat(n) @(posedge clk) y;
                w <= y;
                $display("t=%0t active x=%h w=%h", $time, x, w);
                #1 $display("t=%0t next   x=%h w=%h", $time, x, w);
                $finish;
            end
        endmodule)";
    // Identical to the plain `w <= y` beside it: same region, same instant.
    CHECK(run_capture_finish(src) == "t=1 active x=00 w=00\nt=2 next   x=55 w=55\n");
}

TEST_CASE("nonblocking intra-repeat with a negative real count stores in the current instant") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            logic [7:0] x, y;
            real        r;
            initial forever #5 clk = ~clk;
            initial begin
                x = 0; y = 8'h55; r = -1.4;
                #1 x <= repeat(r) @(posedge clk) y;
                #1 $display("t=%0t x=%h", $time, x);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=2 x=55\n");
}

// The clause's own asymmetry: "if a is assigned -3, it will execute the
// event_expression if a is declared as an unsigned variable, but not if a is
// signed". An unsigned 8-bit variable holding -3 is 253, so the store waits 253
// posedges (t=2525 at this clock) and is nowhere in sight at t=104.
TEST_CASE("nonblocking intra-repeat with an unsigned count holding -3 still waits") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            logic [7:0] x, y, u;
            initial forever #5 clk = ~clk;
            initial begin
                x = 0; y = 8'h55; u = -3;
                #1 x <= repeat(u) @(posedge clk) y;
                #1   $display("t=%0t x=%h", $time, x);
                #102 $display("t=%0t x=%h", $time, x);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=2 x=00\nt=104 x=00\n");
}

// Zero is zero in both signednesses, so it clamps either way -- the test is
// `count < 1` in the count's OWN signedness, which for an unsigned count
// degenerates to `count == 0`. Without the clamp the uint32 countdown would
// underflow and wait 2**32 edges. iverilog 13 stores all three immediately.
TEST_CASE("nonblocking intra-repeat with a zero count stores in the current instant") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            logic [7:0] x, w, z, y;
            logic [7:0] uz;
            integer     sz;
            initial forever #5 clk = ~clk;
            initial begin
                x = 0; w = 0; z = 0; y = 8'h66; uz = 0; sz = 0;
                #1;
                x <= repeat(0)  @(posedge clk) y;
                w <= repeat(uz) @(posedge clk) y;
                z <= repeat(sz) @(posedge clk) y;
                #1 $display("t=%0t lit=%h unsigned=%h signed=%h", $time, x, w, z);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=2 lit=66 unsigned=66 signed=66\n");
}

// The clamp reaches every leaf of a decomposing target: the count is evaluated
// once and each leaf re-tests that one value, so all N land in the same instant.
TEST_CASE("nonblocking intra-repeat clamp applies to every leaf of a concatenation target") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            logic [3:0] a, b;
            logic [7:0] x;
            integer     n;
            initial forever #5 clk = ~clk;
            initial begin
                a = 0; b = 0; x = 8'hab; n = -2;
                #1 {a, b} <= repeat(n) @(posedge clk) x;
                #1 $display("t=%0t a=%h b=%h", $time, a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=2 a=a b=b\n");
}

// -- Shapes that must STILL fail loudly ---------------------------------------

// A whole queue / dynamic array is a single owned handle, not N leaves: the
// deferred record has no owned-handle tail class to stage a deep copy into, and an
// intra site can have N copies in flight where a plain NBA has one
// (docs/design/intra-assignment-timing.md, R3).
TEST_CASE("intra-assignment timing on a whole queue target is rejected") {
    constexpr std::string_view src = R"(
        module m;
            int q[$], r[$];
            initial q <= #2 r;
        endmodule)";
    CHECK(!lower_error(src).empty());
}

TEST_CASE("intra-assignment timing on a whole dynamic-array target is rejected") {
    constexpr std::string_view src = R"(
        module m;
            int d[], e[];
            initial d <= #2 e;
        endmodule)";
    CHECK(!lower_error(src).empty());
}

// LRM 9.4.2.3 makes `iff` part of the EVENT, so the guard has to be evaluated at
// fire time and only enabled occurrences may count down; the deferred record has
// no guard hook (R5).
TEST_CASE("iff on a nonblocking intra-assignment event is rejected") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            logic       clk = 0, en = 1;
            initial a <= @(posedge clk iff en) b;
        endmodule)";
    CHECK(lower_error(src).find("iff qualifier on a nonblocking intra-assignment event") !=
          std::string::npos);
}

// The BLOCKING form of a pattern lvalue still has to reorder the sampled RHS
// across a real suspend, which the nonblocking form never does.
TEST_CASE("blocking intra-assignment timing on an assignment-pattern lvalue is rejected") {
    constexpr std::string_view src = R"(
        typedef struct packed { logic [3:0] p; logic [3:0] q; } p_t;
        module m;
            logic [3:0] a, b;
            p_t         v;
            initial '{a, b} = #5 v;
        endmodule)";
    CHECK(lower_error(src).find("intra-assignment timing control on an assignment-pattern") !=
          std::string::npos);
}

// The BLOCKING form of a whole unpacked aggregate: the aggregate store path
// evaluates the LHS before the RHS, so hoisting the sample across the suspend
// reorders two evaluations 9.4.5 pins in the other order.
TEST_CASE("blocking intra-assignment timing on a whole unpacked struct is rejected") {
    constexpr std::string_view src = R"(
        typedef struct { logic [3:0] p; logic [3:0] q; } s_t;
        module m;
            s_t s, r;
            initial s = #5 r;
        endmodule)";
    CHECK(lower_error(src).find("whole unpacked struct or array target") != std::string::npos);
}

TEST_CASE("blocking intra-assignment timing on a whole array target is rejected") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] mem [0:2], other [0:2];
            initial mem = #5 other;
        endmodule)";
    CHECK(lower_error(src).find("string or array target") != std::string::npos);
}

// A class property has no per-object shadow for any NBA path, timed or not.
TEST_CASE("intra-assignment timing on a class property is rejected") {
    constexpr std::string_view src = R"(
        class c; int p; endclass
        module m;
            c o = new;
            initial o.p <= #5 3;
        endmodule)";
    CHECK(lower_error(src).find("class property") != std::string::npos);
}
