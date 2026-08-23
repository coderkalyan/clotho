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
// Every wait test drives a free-running stimulus process that only stops at
// $finish, so this (not a plain drain) is the right harness.
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

// IEEE 1800-2023 9.4.3: a `wait` whose condition is already true does not
// suspend — it falls through in the same time step. Verified against iverilog 13
// (which prints the second line at t=0).
TEST_CASE("wait on an already-true condition does not suspend") {
    constexpr std::string_view src = R"(
        module m;
            logic g = 1;
            initial begin
                $display("before %0t", $time);
                wait(g);
                $display("after %0t", $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "before 0\nafter 0\n");
}

// LRM 9.4.3: a false condition suspends until it becomes true, then the body
// runs. The process must resume at the exact time the last read changed it true.
TEST_CASE("wait suspends until the condition rises") {
    constexpr std::string_view src = R"(
        module m;
            logic g = 0;
            initial begin
                wait(g);
                $display("woke %0t", $time);
                $finish;
            end
            initial #25 g = 1;
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 25\n");
}

// LRM 9.4.3 truth is the `if` truth (12.4): X is false. A wait on an X value
// must keep blocking until a definite 1 — it must NOT fall through on X. This is
// the case a naive `while (!cond)` lowering gets wrong (RedOr of a 4-state cond
// is X, not a must-plane 0). iverilog resumes only at the 1.
TEST_CASE("wait treats X as false and blocks until a definite 1") {
    constexpr std::string_view src = R"(
        module m;
            logic g;
            initial begin
                wait(g);
                $display("woke %0t g=%b", $time, g);
                $finish;
            end
            initial begin
                #10 g = 1'bx;
                #10 g = 1;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 20 g=1\n");
}

// A multi-signal condition suspends on an any-change wait of every read, and
// re-evaluates the whole expression on each wake.
TEST_CASE("wait on a compound condition wakes when the whole expression is true") {
    constexpr std::string_view src = R"(
        module m;
            logic a = 0, b = 0;
            initial begin
                wait(a && b);
                $display("both %0t", $time);
                $finish;
            end
            initial begin
                #10 a = 1;   // a&&b still false
                #10 b = 1;   // now true
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "both 20\n");
}

// A wait inside a loop must re-arm every iteration — a producer/consumer
// handshake. Verified byte-identical to iverilog 13.
TEST_CASE("wait re-arms across loop iterations (handshake)") {
    constexpr std::string_view src = R"(
        module m;
            logic req = 0;
            int i;
            initial begin
                for (i = 0; i < 3; i++) begin
                    wait(req);
                    $display("got %0d %0t", i, $time);
                    wait(!req);
                end
                $finish;
            end
            initial forever begin
                #10 req = 1;
                #10 req = 0;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "got 0 10\ngot 1 30\ngot 2 50\n");
}

// LRM 9.4.3 with a constant-false condition: the process blocks permanently
// (iverilog warns "will block permanently"). Simulation continues — other
// processes run and $finish still fires. The unreachable body must never print.
TEST_CASE("wait on a constant-false condition parks the process forever") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                $display("before");
                wait(0);
                $display("UNREACHABLE");
            end
            initial #100 begin
                $display("watchdog %0t", $time);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "before\nwatchdog 100\n");
}

// A string condition would need a string-aware wait queue (string stores skip
// wait-queue servicing), so it must fail loudly, not silently never re-trigger.
TEST_CASE("wait on a string condition fails loudly") {
    constexpr std::string_view src = R"(
        module m;
            string s = "";
            initial begin
                wait(s);
                $display("x");
            end
        endmodule
    )";
    const std::string err = lower_error(src);
    CHECK(err.find("string") != std::string::npos);
}

// -- wait on a class property (LRM 9.4.3) ------------------------------------
//
// This is the shape that failed WORST before change notifiers landed. A `wait`
// lowers to `forever { if (cond) break; @(read set of cond); }`, and the read
// set is built from SIGNALS. A class property is heap storage inside an object
// and is not a signal, so the read set came back empty, the `@()` had no
// sensitivity, and the process parked on something nothing could ever trigger:
// a permanent hang with no diagnostic at any stage. Verilator 5.050 prints
// `woke at 1`; iverilog 13 at least fails loudly on the same root cause
// ("Empty NexusSet from wait expression").

// The bare form: `wait(p)` on the enclosing `this`.
TEST_CASE("wait on a bare class property resumes when it changes") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                bit on;
                task w(); wait(on); $display("woke %0t", $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w(); join_none
                #1 c.on = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 1\n");
}

// Through a handle, from outside the class: the uvm_phase_hopper.svh:498 shape
// (`wait (phase.m_premature_end)`). The object expression is lowered INSIDE the
// loop body, so it is re-read on every pass.
TEST_CASE("wait on a class property through a handle resumes when it changes") {
    constexpr std::string_view src = R"(
        module m;
            class C; bit on; endclass
            C c;
            initial begin
                c = new;
                fork begin wait(c.on); $display("woke %0t", $time); end join_none
                #1 c.on = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 1\n");
}

// A condition mixing a class property with an ordinary module signal takes its
// sensitivity from both, and re-tests the whole expression on each wake. Only
// the second change makes it true, so the resume time is what proves both
// wake sources are live. (Verilator 5.050 aborts on this design with "Null
// pointer dereferenced" -- it resolves the handle too early, the same class of
// bug clotho had for `@(c.on)`.)
TEST_CASE("wait on a class property and a module signal wakes from either") {
    constexpr std::string_view src = R"(
        module m;
            class C; bit on; endclass
            C c;
            logic g = 0;
            initial begin
                c = new;
                fork begin wait(c.on && g); $display("mix %0t", $time); end join_none
                #1 c.on = 1;
                #1 g = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "mix 2\n");
}

// Properties of TWO different objects in one condition: two notifiers, two
// object handles, one wait.
TEST_CASE("wait on properties of two objects wakes when both are set") {
    constexpr std::string_view src = R"(
        module m;
            class C; bit on; endclass
            C x, y;
            initial begin
                x = new; y = new;
                fork begin wait(x.on && y.on); $display("two %0t", $time); end join_none
                #1 x.on = 1;
                #1 y.on = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "two 2\n");
}

// The object need not be a variable: an associative-array element keyed by a
// class handle works, which is the uvm_objection.svh shape inside a `wait`.
TEST_CASE("wait on a property of an assoc-array element resumes when it changes") {
    constexpr std::string_view src = R"(
        module m;
            class K; int id; endclass
            class E; bit raised; endclass
            E m_events[K];
            K k;
            initial begin
                k = new;
                m_events[k] = new;
                fork begin wait(m_events[k].raised); $display("woke %0t", $time); end join_none
                #1 m_events[k].raised = 1;
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture_finish(src) == "woke 1\n");
}

// A property whose type carries no old -> new transition to classify gets no
// notifier, so there is nothing to park on. It must be REFUSED: lowering it
// anyway is what produced the silent permanent park this whole feature exists to
// remove. `wait(0)` is unaffected -- it reads no property at all and keeps the
// documented LRM 9.4.3 behaviour (the case above).
TEST_CASE("wait on a class property with no change notifier fails loudly") {
    const std::string err = lower_error(R"(
        module m;
            class C;
                string s;
                task w(); wait(s == "go"); $display("x"); endtask
            endclass
            C c;
            initial begin c = new; c.w(); end
        endmodule
    )");
    CHECK(err.find("no change notifier") != std::string::npos);
}
