#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// Read the whole of a temp file into a string.
std::string slurp(std::FILE* f) {
    std::rewind(f);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    return out;
}

// JIT-run a design that ends via $finish and return its stdout. $finish calls
// std::exit, so the simulation runs in a forked child writing to a temp file
// (mirrors wait_test.cpp).
std::string run_capture_finish(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        dup2(fileno(tmp), fileno(stdout));
        ir::run_jit(*r.module);
        std::fflush(stdout);
        _exit(0);
    }

    int wstatus = 0;
    REQUIRE(waitpid(pid, &wstatus, 0) == pid);

    std::string out = slurp(tmp);
    std::fclose(tmp);
    return out;
}

std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    return r.ok ? std::string{} : r.diagnostics;
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

// The event / repeat forms need wait-queue-triggered records (a documented
// follow-up) and must fail loudly, never silently drop the timing.
TEST_CASE("nonblocking intra-assignment event timing is rejected") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            logic       c = 0;
            initial a <= @(posedge c) b;
        endmodule)";
    CHECK(lower_error(src).find("event/repeat timing is not supported") != std::string::npos);
}

// A non-whole-scalar target (bit/part-select, array element) needs the masked /
// array record path (a documented follow-up) and must fail loudly.
TEST_CASE("nonblocking intra-delay to a part-select target is rejected") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial a[3:0] <= #5 4'd1;
        endmodule)";
    CHECK(lower_error(src).find("whole integral scalar target") != std::string::npos);
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
