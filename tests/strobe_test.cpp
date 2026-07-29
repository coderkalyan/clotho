#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

// $strobe / $strobeb / $strobeo / $strobeh per LRM §21.2.2: unlike $display,
// both evaluating the arguments and printing them are deferred to the
// Postponed region of the current time step, just before simulation time
// advances. Expectations are cross-checked against Icarus Verilog 13, which
// (like Clotho) rejects an automatic-lifetime argument at compile time.

namespace {

// JIT-run a module and capture stdout (mirrors display_test.cpp).
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

// Assert that `source` is rejected with a diagnostic containing `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

} // namespace

// Evaluation is deferred to the end of the time step, not just the print: two
// $strobe calls earlier in the same step both print the value `x` settles to
// by the end of that step (iverilog: "call1 x=4" / "call2 x=4").
TEST_CASE("$strobe defers evaluation, not just printing, to end of time step") {
    CHECK(run_capture(R"(
        module m;
            int x;
            initial begin
                x = 1;
                $strobe("call1 x=%0d", x);
                x = 2;
                $strobe("call2 x=%0d", x);
                x = 4;
                #1 $finish;
            end
        endmodule
    )") == "call1 x=4\n"
           "call2 x=4\n");
}

// Each invocation queues its own Postponed-region print — a $strobe inside a
// loop prints once per invocation, not once per call site (iverilog: three
// identical lines).
TEST_CASE("repeated invocations of one $strobe call site each print") {
    CHECK(run_capture(R"(
        module m;
            int x;
            initial begin
                x = 3;
                repeat (3) $strobe("loop x=%0d", x);
                #1 $finish;
            end
        endmodule
    )") == "loop x=3\n"
           "loop x=3\n"
           "loop x=3\n");
}

// A later time step's $strobe reflects only that step's final value — the
// deferred print does not leak across steps (iverilog: "99").
TEST_CASE("$strobe across separate time steps") {
    CHECK(run_capture(R"(
        module m;
            int x;
            initial begin
                x = 1;
                $strobe("t0 x=%0d", x);
                #1;
                x = 99;
                $strobe("t1 x=%0d", x);
                #1 $finish;
            end
        endmodule
    )") == "t0 x=1\n"
           "t1 x=99\n");
}

// The LRM §21.2.2 example, verbatim in shape: $strobe of $time and a signal at
// every negedge, run to a fixed point count.
TEST_CASE("$time inside $strobe (LRM 21.2.2 example)") {
    CHECK(run_capture(R"(
        module m;
            reg clock;
            int data;
            initial clock = 0;
            always #5 clock = ~clock;
            initial begin
                data = 0;
                #1 data = 8'hAB;
                $strobe("At time %0d, data is %h", $time, data);
                #20 $finish;
            end
        endmodule
    )") == "At time 1, data is 000000ab\n");
}

// Radix task variants ($strobeb / $strobeo / $strobeh) set the bare-argument
// default conversion, same as the $display family (LRM §21.2.1.1).
TEST_CASE("$strobeb / $strobeo / $strobeh bare-argument radix") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $strobeb(5'b101x0);
                $strobeo(9'o567);
                $strobeh(12'hEF);
                #0;
            end
        endmodule
    )") == "101x0\n"
           "567\n"
           "0ef\n");
}

// $strobe inside a clocked always block: forced to a coroutine (is_simple_always
// excludes StrobePend), so the deferred body still ramps correctly from a
// thunk-ineligible process.
TEST_CASE("$strobe inside always @(posedge clk)") {
    CHECK(run_capture(R"(
        module m;
            reg clk;
            int n;
            initial clk = 0;
            always #5 clk = ~clk;
            initial n = 0;
            always @(posedge clk) begin
                n = n + 1;
                $strobe("edge n=%0d", n);
            end
            initial #12 $finish;
        endmodule
    )") == "edge n=1\n");
}

// $strobe inside a task: the ramp happens from the task's own coroutine frame
// (state_/wheel_/coro_begin_ all belong to the task, not its caller).
TEST_CASE("$strobe inside a task") {
    CHECK(run_capture(R"(
        module m;
            int v;
            task automatic t;
                begin
                    $strobe("in task v=%0d", v);
                end
            endtask
            initial begin
                v = 7;
                t();
                #1 $finish;
            end
        endmodule
    )") == "in task v=7\n");
}

// $strobe is rejected inside a plain (non-task) function: no wheel in the ABI,
// mirroring the existing $time / $realtime restriction.
TEST_CASE("$strobe inside a plain function is rejected") {
    check_rejected(R"(
        module m;
            int g;
            function automatic int f();
                $strobe("g=%0d", g);
                return 0;
            endfunction
            initial begin
                g = 9;
                $display("%0d", f());
            end
        endmodule
    )",
                   "$strobe inside a function is not supported");
}

// An automatic-lifetime argument cannot be re-evaluated by the deferred body
// (its storage does not outlive the call): rejected at compile time. slang's
// own binder catches this (the same "traceable argument" rule it applies to
// $monitor / $dumpvars), matching iverilog's identical rejection.
TEST_CASE("$strobe of an automatic local is rejected") {
    check_rejected(R"(
        module m;
            task automatic t;
                int local_v;
                begin
                    local_v = 42;
                    $strobe("local_v=%0d", local_v);
                end
            endtask
            initial begin
                t();
                #0 $finish;
            end
        endmodule
    )",
                   "cannot be traced");
}

TEST_CASE("$monitor prints once per change") {
    CHECK(run_capture(R"(
        module m;
          integer a;
          initial begin
            a = 0;
            $monitor("a=%0d", a);
            #1 a = 1;
            #1 a = 2;
            #1 a = 2;   // no change -> no print
            #1 a = 5;
            #1 $finish;
          end
        endmodule
    )") == "a=0\na=1\na=2\na=5\n");
}

// $monitoron/$monitoroff enable/disable (reon reprints current); a new $monitor
// replaces the previous.
TEST_CASE("$monitor on/off and replacement") {
    CHECK(run_capture(R"(
        module m;
          integer a, b;
          initial begin
            a = 0; b = 0;
            $monitor("mon1 a=%0d", a);
            #1 a = 1;
            #1 $monitoroff;
            #1 a = 2;
            #1 $monitoron;
            #1 a = 3;
            #1 $monitor("mon2 b=%0d", b);
            #1 b = 7;
            #1 a = 9;
            #1 $finish;
          end
        endmodule
    )") == "mon1 a=0\nmon1 a=1\nmon1 a=2\nmon1 a=3\nmon2 b=0\nmon2 b=7\n");
}

// $monitor inside a function has no wheel in the ABI; guard loud (like $strobe).
TEST_CASE("$monitor in a function is rejected") {
    check_rejected(R"(
        module m;
          function void f(); $monitor("x"); endfunction
          initial f();
        endmodule
    )",
                   "$monitor inside a function is not supported");
}
