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

// A multi-signal trigger `@(a or b)` needs the record on several trigger lists
// with first-fire-wins removal (a documented follow-up) and must fail loudly.
TEST_CASE("nonblocking intra-assignment multi-signal event trigger is rejected") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            logic       c = 0, d = 0;
            initial a <= @(posedge c or posedge d) b;
        endmodule)";
    CHECK(lower_error(src).find("multi-signal event trigger") != std::string::npos);
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
