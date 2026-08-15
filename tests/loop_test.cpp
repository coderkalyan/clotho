#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// Read the whole of a temp file into a string (helper for the capture routines).
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

// JIT-run a module's initial blocks and return everything it wrote to stdout.
// Loop bodies report their progress with $display, so the captured text is the
// observable behavior.
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

    std::string out = slurp(tmp);
    std::fclose(tmp);

    return out;
}

// JIT-run a design that ends via $finish and return its stdout. The simulation
// gets a process of its own (Subprocess.h explains why $finish cannot share
// the runner's); lowering here as well is what reports a broken source.
// Use this, not run_capture, for any free-running process — e.g. a
// clock-generating `always` — that only stops at $finish.
std::string run_capture_finish(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    return test::run_design_subprocess(source);
}

// Does the IR of any process contain at least one instruction with the tag?
bool any_process_has_tag(const ir::Module& m, ir::Inst::Tag tag) {
    for (const ir::Process& p : m.processes()) {
        for (size_t i = 0; i < p.ir.inst_count(); ++i) {
            if (p.ir.tag(static_cast<ir::Index>(i)) == tag) {
                return true;
            }
        }
    }

    return false;
}

} // namespace

// IEEE 1800-2023 12.7.1: the three-step for-loop — init once, test, body, step,
// repeat. A declared loop variable is local to the loop.
TEST_CASE("for loop counts up over its declared loop variable") {
    constexpr std::string_view src = R"(
        module m;
            initial
                for (int i = 0; i < 3; i++)
                    $display("%0d", i);
        endmodule
    )";
    CHECK(run_capture(src) == "0\n1\n2\n");
}

TEST_CASE("for loop lowers to a Loop instruction") {
    const LowerResult r = lower_text(R"(
        module m;
            initial for (int i = 0; i < 3; i = i + 1) $display("%0d", i);
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Loop));
}

// The step may decrement; the body runs for each value down to the bound.
TEST_CASE("for loop counts down with predecrement") {
    constexpr std::string_view src = R"(
        module m;
            initial
                for (int i = 3; i > 0; --i)
                    $display("%0d", i);
        endmodule
    )";
    CHECK(run_capture(src) == "3\n2\n1\n");
}

// 12.7.1: comma-separated init may declare multiple vars, and a later
// initializer can read an earlier one.
TEST_CASE("for loop with multiple loop variables, later reads earlier") {
    constexpr std::string_view src = R"(
        module m;
            initial
                for (int i = 0, j = i + 10; i < 3; i++, j++)
                    $display("%0d %0d", i, j);
        endmodule
    )";
    CHECK(run_capture(src) == "0 10\n1 11\n2 12\n");
}

// The loop accumulates into a module signal; the final value is observable.
TEST_CASE("for loop accumulates into a signal") {
    constexpr std::string_view src = R"(
        module m;
            integer sum;
            initial begin
                sum = 0;
                for (int i = 1; i <= 4; i++)
                    sum = sum + i;
                $display("%0d", sum);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10\n");
}

// 12.7.1 step b) / 12.4: a condition that evaluates to X is treated as zero
// (false), so the loop body never runs.
TEST_CASE("for loop with an x condition runs zero times") {
    constexpr std::string_view src = R"(
        module m;
            logic cond;
            initial begin
                cond = 1'bx;
                for (int i = 0; cond; i++)
                    $display("ran");
                $display("done");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "done\n");
}

// while-loop over a signal counter (12.7.4).
TEST_CASE("while loop counts down a signal") {
    constexpr std::string_view src = R"(
        module m;
            integer n;
            initial begin
                n = 3;
                while (n > 0) begin
                    $display("%0d", n);
                    n = n - 1;
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3\n2\n1\n");
}

// A multi-bit loop condition (12.7) has the same truth value as an `if` (12.4):
// true iff any bit is 1. The condition is used bare (not compared), so it must
// be reduced to a 1-bit predicate before the loop test. Regression: a bare
// multi-bit condition previously reached the backend at native width and
// crashed on a width-mismatched compare.
TEST_CASE("while loop with a bare multi-bit condition") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] x;
            initial begin
                x = 8'd3;
                while (x) begin
                    $display("%0d", x);
                    x = x - 1;
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3\n2\n1\n");
}

// An all-x multi-bit condition has no definite 1 bit, so it is false and the
// loop runs zero times.
TEST_CASE("while loop with an all-x multi-bit condition runs zero times") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] x;
            initial begin
                x = 8'bxxxxxxxx;
                while (x)
                    $display("ran");
                $display("done");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "done\n");
}

// A real loop condition is true iff nonzero (reals carry no x/z). Regression:
// a bare real condition previously routed a real plane through the integer
// must-plane extraction and failed in the backend.
TEST_CASE("while loop with a real condition") {
    constexpr std::string_view src = R"(
        module m;
            real r;
            initial begin
                r = 3.0;
                while (r) begin
                    $display("%0g", r);
                    r = r - 1.0;
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3\n2\n1\n");
}

// A #delay inside a loop body suspends the coroutine; resume must land back in
// the body and the back-edge must still close the loop across the suspension.
TEST_CASE("delay inside a loop body suspends and resumes across iterations") {
    constexpr std::string_view src = R"(
        module m;
            initial
                for (int i = 0; i < 3; i++) begin
                    $display("%0d", i);
                    #1;
                end
        endmodule
    )";
    CHECK(run_capture(src) == "0\n1\n2\n");
}

// forever lowers to a Loop (a constant-true condition). Not executed: with no
// $finish / break support it would never drain the wheel.
TEST_CASE("forever loop lowers to a Loop instruction") {
    const LowerResult r = lower_text(R"(
        module m;
            integer n;
            initial forever begin n = n + 1; #1; end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Loop));
}

// repeat (N) runs the body exactly N times (12.7.2).
TEST_CASE("repeat loop runs N times") {
    CHECK(run_capture(R"(
        module m;
            integer i;
            initial begin
                for (i = 0; i < 3; i = i + 1) ; // touch a control var
                repeat (3) $display("x");
            end
        endmodule
    )") == "x\nx\nx\n");
}

// Lowers to the generic Loop instruction.
TEST_CASE("repeat loop lowers to a Loop") {
    const LowerResult r = lower_text(R"(
        module m;
            initial repeat (3) $display("x");
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Loop));
}

// A zero, negative, or X/Z count yields zero iterations (12.7.2).
TEST_CASE("repeat loop with non-positive or unknown count runs zero times") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                repeat (0) $display("zero");
                $display("after-zero");
            end
        endmodule
    )") == "after-zero\n");

    CHECK(run_capture(R"(
        module m;
            integer n;
            initial begin
                n = -1;
                repeat (n) $display("neg");
                $display("after-neg");
            end
        endmodule
    )") == "after-neg\n");

    // An uninitialized 4-state integer is all-X, so the count is unknown.
    CHECK(run_capture(R"(
        module m;
            integer n;
            initial begin
                repeat (n) $display("x");
                $display("after-x");
            end
        endmodule
    )") == "after-x\n");
}

// The count is sampled once on entry; mutating its source inside the body does
// not change the iteration total (12.7.2).
TEST_CASE("repeat loop count is evaluated once") {
    CHECK(run_capture(R"(
        module m;
            integer n;
            initial begin
                n = 3;
                repeat (n) begin
                    $display("i");
                    n = n + 10; // must not extend the loop
                end
            end
        endmodule
    )") == "i\ni\ni\n");
}

// Side effects in the body observe an independent loop counter each pass.
TEST_CASE("repeat loop body side effects") {
    CHECK(run_capture(R"(
        module m;
            integer k;
            initial begin
                k = 0;
                repeat (4) begin
                    k = k + 1;
                    $display("%0d", k);
                end
            end
        endmodule
    )") == "1\n2\n3\n4\n");
}

// Nested repeat multiplies the iteration counts.
TEST_CASE("repeat loop nesting") {
    CHECK(run_capture(R"(
        module m;
            initial
                repeat (2)
                    repeat (3) $display("y");
        endmodule
    )") == "y\ny\ny\ny\ny\ny\n");
}

// A sensitivity-less `always` is an implicit forever loop around its body (LRM
// 9.2.2): `always #5 clk = ~clk;` is a free-running clock. Each toggle drives a
// posedge-sensitive process that counts edges and stops the run.
TEST_CASE("always with a delay is a free-running clock") {
    CHECK(run_capture_finish(R"(
        module m;
            reg clk;
            integer count;
            initial begin clk = 0; count = 0; end
            always #5 clk = ~clk;
            always @(posedge clk) begin
                count = count + 1;
                $display("posedge %0d", count);
                if (count == 3) $finish;
            end
        endmodule
    )") == "posedge 1\nposedge 2\nposedge 3\n");
}

// The implicit forever loop re-enters the body, so a multi-statement block with
// several internal delays cycles indefinitely until $finish.
TEST_CASE("always block with multiple internal delays cycles") {
    CHECK(run_capture_finish(R"(
        module m;
            reg a;
            integer n;
            initial begin a = 0; n = 0; end
            always begin
                #1 a = 1;
                #1 a = 0;
                n = n + 1;
                $display("cycle %0d", n);
                if (n == 3) $finish;
            end
        endmodule
    )") == "cycle 1\ncycle 2\ncycle 3\n");
}

// A sensitivity-less `always` lowers to an implicit forever Loop, not a
// run-once body.
TEST_CASE("sensitivity-less always lowers to a Loop") {
    const LowerResult r = lower_text(R"(
        module m;
            reg clk;
            always #5 clk = ~clk;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Loop));
}

// -- do-while / break / continue (12.7). Oracle: iverilog. --

// do-while runs the body before the first predicate test, so it executes at
// least once even when the condition is false on entry (12.7.1).
TEST_CASE("do-while runs the body once when the condition is false on entry") {
    CHECK(run_capture(R"(
        module m;
            integer i, n;
            initial begin
                i = 5; n = 0;
                do begin n = n + 1; i = i + 1; end while (i < 5);
                $display("%0d", n);
            end
        endmodule
    )") == "1\n");
}

TEST_CASE("do-while iterates while the condition holds") {
    CHECK(run_capture(R"(
        module m;
            integer i;
            initial begin
                i = 0;
                do begin $display("%0d", i); i = i + 1; end while (i < 3);
            end
        endmodule
    )") == "0\n1\n2\n");
}

TEST_CASE("do-while lowers to a Loop instruction") {
    const LowerResult r = lower_text(R"(
        module m;
            integer i;
            initial begin i = 0; do i = i + 1; while (i < 3); end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Loop));
}

// break leaves the innermost loop immediately.
TEST_CASE("break exits the enclosing loop") {
    CHECK(run_capture(R"(
        module m;
            integer i, sum;
            initial begin
                sum = 0;
                for (i = 0; i < 10; i = i + 1) begin
                    if (i == 4) break;
                    sum = sum + i;
                end
                $display("%0d %0d", sum, i);
            end
        endmodule
    )") == "6 4\n");
}

TEST_CASE("break in a while(1) terminates the loop") {
    CHECK(run_capture(R"(
        module m;
            integer i, sum;
            initial begin
                i = 0; sum = 0;
                while (1) begin
                    if (i >= 5) break;
                    sum = sum + i; i = i + 1;
                end
                $display("%0d", sum);
            end
        endmodule
    )") == "10\n");
}

// continue skips the rest of the body; the for-loop step still runs (12.7).
TEST_CASE("continue skips the rest of the body but runs the for step") {
    CHECK(run_capture(R"(
        module m;
            integer i, sum;
            initial begin
                sum = 0;
                for (i = 0; i < 6; i = i + 1) begin
                    if (i % 2 == 0) continue;
                    sum = sum + i;
                end
                $display("%0d", sum);
            end
        endmodule
    )") == "9\n");
}

// continue in a do-while re-evaluates the condition rather than skipping it.
TEST_CASE("continue in a do-while re-tests the condition") {
    CHECK(run_capture(R"(
        module m;
            integer i, sum;
            initial begin
                i = 0; sum = 0;
                do begin
                    i = i + 1;
                    if (i == 2) continue;
                    sum = sum + i;
                end while (i < 4);
                $display("%0d %0d", sum, i);
            end
        endmodule
    )") == "8 4\n");
}

// An inner break only exits the inner loop; the outer loop keeps iterating.
TEST_CASE("break inside a nested loop exits only the inner loop") {
    CHECK(run_capture(R"(
        module m;
            integer i, j, sum;
            initial begin
                sum = 0;
                for (i = 0; i < 3; i = i + 1) begin
                    for (j = 0; j < 3; j = j + 1) begin
                        if (j == 1) break;
                        sum = sum + 1;
                    end
                end
                $display("%0d", sum);
            end
        endmodule
    )") == "3\n");
}

// break / continue nested inside an if inside the loop body still resolve to
// the enclosing loop's exit / step blocks.
TEST_CASE("break and continue under a nested if") {
    CHECK(run_capture(R"(
        module m;
            integer i, sum;
            initial begin
                sum = 0;
                for (i = 0; i < 8; i = i + 1) begin
                    if (i == 3) begin
                        continue;
                    end
                    if (i == 6) begin
                        break;
                    end
                    sum = sum + i;
                end
                $display("%0d", sum);
            end
        endmodule
    )") == "12\n");
}

// slang rejects break/continue outside any loop at elaboration, so Clotho needs
// no guard of its own — the design never lowers.
TEST_CASE("break outside a loop is rejected") {
    const LowerResult r = lower_text(R"(
        module m;
            initial begin break; end
        endmodule
    )");
    CHECK_FALSE(r.ok);
}

// -- disable of a named block (LRM 9.6.2). A `disable` naming a block that
// lexically encloses it is a structured multi-level exit — the only one the
// language has, since SV has no labelled break — so it belongs beside
// break/continue. Oracle: iverilog. Everything else about `disable` (a task
// target, a remote target, `disable fork`) is refused; see task_test.cpp and
// fork_test.cpp, and docs/design/disable.md for the phasing. --

// LRM 9.6.2 examples 1 and 2: the block ends where it ends, and execution
// resumes at the statement AFTER it — which is a point in the middle of the
// enclosing body, not any loop's exit.
TEST_CASE("disable of the enclosing block resumes after it") {
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial begin
                x = 0;
                begin : b
                    x = 1;
                    disable b;
                    x = 99;
                end
                x = x + 1;
                $display("x=%0d", x);
            end
        endmodule
    )") == "x=2\n");
}

// LRM 9.6.2 example 3: a loop-body block is the `continue` equivalent — the step
// still runs, so the loop advances rather than restarting the same iteration.
TEST_CASE("disable of a loop-body block acts like continue") {
    CHECK(run_capture(R"(
        module m;
            integer i, hits;
            initial begin
                hits = 0;
                for (i = 0; i < 5; i = i + 1) begin : body
                    if (i % 2 == 0) disable body;
                    hits = hits + 1;
                    $display("kept %0d", i);
                end
                $display("hits=%0d i=%0d", hits, i);
            end
        endmodule
    )") == "kept 1\nkept 3\nhits=2 i=5\n");
}

// LRM 9.6.2 example 5: a block ENCLOSING the loop is the `break` equivalent, and
// it crosses as many regions as it needs to — here two loops and an `if`. The
// second process proves only the one activation ended.
TEST_CASE("disable of an enclosing block leaves every loop it names") {
    CHECK(run_capture(R"(
        module m;
            integer i, j;
            initial begin : top
                for (i = 0; i < 3; i = i + 1) begin
                    for (j = 0; j < 3; j = j + 1) begin
                        if (i == 1 && j == 1) begin
                            $display("bail i=%0d j=%0d", i, j);
                            disable top;
                        end
                        $display("i=%0d j=%0d", i, j);
                    end
                end
                $display("unreached");
            end
            initial #1 $display("second process still runs");
        endmodule
    )") == "i=0 j=0\ni=0 j=1\ni=0 j=2\ni=1 j=0\nbail i=1 j=1\nsecond process still runs\n");
}

// The exit unwinds a `case` arm and a `while` at once: the backend has to reach
// the block's end block from inside regions that each have their own merge.
TEST_CASE("disable leaves a case arm and a while loop together") {
    constexpr std::string_view expect = "zero\ntail i=0 seen=1\nbail from while at i=1\ndflt 2\n"
                                        "tail i=2 seen=2\ndflt 3\ntail i=3 seen=3\ndone seen=3\n";
    CHECK(run_capture(R"(
        module m;
            integer i, seen;
            initial begin
                seen = 0;
                for (i = 0; i < 4; i = i + 1) begin : outer
                    case (i)
                        0: $display("zero");
                        1: begin
                             while (1) begin
                                 $display("bail from while at i=%0d", i);
                                 disable outer;
                             end
                           end
                        default: $display("dflt %0d", i);
                    endcase
                    seen = seen + 1;
                    $display("tail i=%0d seen=%0d", i, seen);
                end
                $display("done seen=%0d", seen);
            end
        endmodule
    )") == expect);
}

// The target is the whole process body, so leaving it ends the process — the
// same thing running off the end does.
TEST_CASE("disable of the process body ends the process") {
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial begin : whole
                x = 1;
                if (x == 1) disable whole;
                x = 99;
            end
            initial #1 $display("x=%0d", x);
        endmodule
    )") == "x=1\n");
}

// LRM 9.6.2 example 7's mechanism: disabling the body of an `always` ends THIS
// activation only, and the loop re-arms — so the disable fires again every other
// pass rather than killing the process.
TEST_CASE("a disabled always body re-arms") {
    constexpr std::string_view expect = "t=5 tail n=1 log=1\nt=10 disabled at n=2\nt=15 tail n=3 log=2\n"
                                        "t=20 disabled at n=4\nt=25 tail n=5 log=3\nn=5 log=3\n";
    CHECK(run_capture_finish(R"(
        module m;
            integer n, log;
            initial begin n = 0; log = 0; end
            always begin : blk
                #5 n = n + 1;
                if (n % 2 == 0) begin
                    $display("t=%0t disabled at n=%0d", $time, n);
                    disable blk;
                end
                log = log + 1;
                $display("t=%0t tail n=%0d log=%0d", $time, n, log);
            end
            initial #26 begin $display("n=%0d log=%0d", n, log); $finish; end
        endmodule
    )") == expect);
}

// A targeted block becomes an ir::NamedBlock region, which is where %m reads its
// name from now. Both halves of the name survive the change.
TEST_CASE("disable leaves %m naming the enclosing blocks") {
    CHECK(run_capture(R"(
        module m;
            initial begin : outer
                begin : inner
                    $display("%m");
                    disable inner;
                end
                $display("%m after");
            end
        endmodule
    )") == "m.outer.inner\nm.outer after\n");
}

// The lexical form is the only one lowered, and it is lowered as structure: a
// labelled region plus a terminator, no flag and no runtime call.
TEST_CASE("a lexical disable lowers to a named_block region and a disable") {
    const LowerResult r = lower_text(R"(
        module m;
            integer x;
            initial begin : b
                x = 1;
                disable b;
            end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::NamedBlock));
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Disable));
}

// The REMOTE form: a `disable` naming a block it is not inside. Both shapes lower —
// a sibling block in the same process, and LRM example 3's cross-process form —
// and the sibling one is also the LRM's "if the block is not currently executing,
// the disable has no effect", because a process is never inside two of its own
// sibling blocks at once. Oracle: iverilog.
TEST_CASE("disable of a sibling block is a no-op, its target not being live") {
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial begin
                x = 0;
                begin : first
                    x = 1;
                end
                begin : second
                    disable first;
                end
                $display("x=%0d", x);
            end
        endmodule
    )") == "x=1\n");
}

// LRM example 3's shape: the target is parked mid-body in ANOTHER process. The
// disable ends that activation at the instant it runs — the always block re-arms
// and its next completion is a full period after the disable, not after the
// #5 it was parked on. iverilog agrees step for step (the only difference is the
// order of two processes woken at the same instant, which the LRM leaves open).
TEST_CASE("disable of a block in another process ends that activation now") {
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial x = 0;
            always begin : always1
                #5 x = x + 1;
                $display("t=%0t x=%0d", $time, x);
            end
            initial begin
                #7 disable m.always1;
                $display("disabled at %0t", $time);
                #19 $finish;
            end
        endmodule
    )") == "t=5 x=1\ndisabled at 7\nt=12 x=2\nt=17 x=3\nt=22 x=4\n");
}

// The abort has to abandon the park it walked away from. Here the target is parked
// on a #50 when the disable lands at t=5; that timer still fires at t=50, by which
// point the process is parked on a later delay of its own. If the stale timer were
// allowed to wake it, the second delay would end early — so this pins the timing of
// everything AFTER the abort, not just the abort.
TEST_CASE("a disabled activation is not resumed by the timer it abandoned") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                begin : blk
                    #50 $display("never");
                end
                $display("after blk at %0t", $time);
                #20 $display("second at %0t", $time);
                #100 $display("third at %0t", $time);
            end
            initial #5 disable blk;
        endmodule
    )") == "after blk at 5\nsecond at 25\nthird at 125\n");
}

// Every elaborated instance gets its own StatementBlockSymbol, so a target id names
// one block IN one instance and `disable u1.blk` cannot reach u2's copy.
TEST_CASE("a remote disable reaches one instance's block, not the definition's") {
    CHECK(run_capture(R"(
        module sub;
            initial begin
                begin : blk
                    #100 $display("%m never");
                end
                $display("%m after at %0t", $time);
            end
        endmodule
        module m;
            sub u1();
            sub u2();
            initial #5 disable u1.blk;
        endmodule
    )") == "m.u1 after at 5\nm.u2.blk never\nm.u2 after at 100\n");
}

// LRM 16.4.4: disabling the outermost scope of a procedure DROPS its pending
// deferred reports — "cancelled", "cleared", never reported. The
// exit carries the same generation bump an activation start does, so the queued
// report never matures. No oracle: iverilog does not implement deferred
// assertions.
TEST_CASE("disable of a procedure's outermost block drops its deferred reports") {
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial begin : whole
                x = 0;
                assert #0 (x == 1) else $error("bad");
                disable whole;
            end
            initial #1 $display("still alive");
        endmodule
    )") == "still alive\n");

    // And the other direction, which is a different rule rather than a weaker case
    // of the same one: a block nested deeper is NOT the outermost scope, so 16.4.4
    // does not flush and the report still matures.
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial begin
                x = 0;
                begin : inner
                    assert #0 (x == 1) else $info("deferred fired");
                    disable inner;
                    $display("never");
                end
                $display("after inner");
            end
        endmodule
    )") == "after inner\n");
}

// The same rule reached from ANOTHER process, which is the case that needs the bump
// to land on the target's generation counter rather than the disabler's. The
// `disable` has to run in the same time slot as the assert and after it, or the
// report has already matured and there is nothing pending to drop -- so the
// disabler yields with `#0` (the inactive region runs after Active, before
// Observed).
TEST_CASE("a remote disable of an outermost block drops that process's reports") {
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial begin : whole
                x = 0;
                assert #0 (x == 1) else $error("should be dropped");
                #10 $display("never");
            end
            initial begin
                #0 disable whole;
                $display("disabled at %0t", $time);
            end
        endmodule
    )") == "disabled at 0\n");

    // Without the disable the same report matures normally, which is what makes the
    // case above evidence of a drop rather than of a report that never existed.
    CHECK(run_capture(R"(
        module m;
            integer x;
            initial begin : whole
                x = 0;
                assert #0 (x == 1) else $info("fired");
                #10 $display("reached %0t", $time);
            end
        endmodule
    )") == "reached 10\n");
}

// The remote form does not care where the DISABLER runs — it marks an occupant and
// pushes a handle — so a task may carry one. Only the TARGET has to be a
// single-occupant block in a process body. Oracle: iverilog.
TEST_CASE("a disable inside a task can end a block in a process") {
    CHECK(run_capture(R"(
        module m;
            task killer;
                #5 disable blk;
                $display("killed at %0t", $time);
            endtask
            initial begin
                begin : blk
                    #100 $display("never");
                end
                $display("after blk at %0t", $time);
            end
            initial killer();
        endmodule
    )") == "killed at 5\nafter blk at 5\n");
}

// A task call that consumes no time runs to completion inside the call, so the
// process is never parked behind it and there is nothing to double-wake. Refusing
// every task call was over-broad; only a time-consuming one is a park.
TEST_CASE("a target that calls a zero-time task is still a legal remote target") {
    CHECK(run_capture(R"(
        module m;
            task quiet(input integer v);
                $display("helper %0d at %0t", v, $time);
            endtask
            initial begin
                begin : blk
                    quiet(1);
                    #100 $display("never");
                end
                $display("after blk at %0t", $time);
            end
            initial #5 disable blk;
        endmodule
    )") == "helper 1 at 0\nafter blk at 5\n");
}

// A block in a body that can have SEVERAL activations at once. A bare branch would
// end only the one running it; LRM 9.6.2 ends them all, so the block keeps a list
// of the activations currently inside it, exactly as a task target does. Two
// activations of one task, both inside the block, ended by one `disable`. Oracle:
// iverilog.
TEST_CASE("disable of a block in a task ends every activation inside it") {
    CHECK(run_capture(R"(
        module m;
            task worker;
                begin : loop
                    #10 $display("tick at %0t", $time);
                    #100 $display("never");
                end
                $display("done at %0t", $time);
            endtask
            initial worker;
            initial #2 worker;
            initial #20 disable m.worker.loop;
        endmodule
    )") == "tick at 10\ntick at 12\ndone at 20\ndone at 20\n");
}

// The same block used as a labelled break INSIDE the task, which is the common
// idiom and the one V1's lexical form could not have in a multi-activation body.
// It now marks every activation and then takes the ordinary structured exit out of
// its own.
TEST_CASE("a disable of an enclosing block inside a task exits it") {
    CHECK(run_capture(R"(
        module m;
            task t(input integer n);
                begin : blk
                    if (n > 1) disable blk;
                    $display("n=%0d not skipped", n);
                end
                $display("t done");
            endtask
            initial begin t(1); t(2); end
        endmodule
    )") == "n=1 not skipped\nt done\nt done\n");
}

// A fork child is a second activation of the enclosing body, and its blocks are on
// the same footing: reachable from another process, and usable as a labelled exit
// from inside the child. Either way the `join` completes when the child ends.
TEST_CASE("a block inside a fork child can be disabled from either side") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                fork
                    begin : child
                        #10 $display("never at %0t", $time);
                    end
                join
                $display("join done at %0t", $time);
            end
            initial #5 disable child;
        endmodule
    )") == "join done at 5\n");

    CHECK(run_capture(R"(
        module m;
            initial begin
                fork
                    begin : child
                        #1 disable child;
                        $display("never");
                    end
                join
                $display("join done at %0t", $time);
            end
        endmodule
    )") == "join done at 1\n");
}

// What is still refused, and it is now only ever a park the abort cannot claim.
TEST_CASE("a target that parks on a time-consuming call is still refused") {
    const LowerResult task_park = lower_text(R"(
        module m;
            task t; #10; endtask
            initial begin
                begin : blk
                    t();
                end
            end
            initial #5 disable blk;
        endmodule
    )");
    CHECK_FALSE(task_park.ok);
    CHECK(task_park.diagnostic_text().find("cannot claim that park") != std::string::npos);
}

// An assertion action block is an ordinary body to the remote form: it marks the
// target's activations and pushes their handles, which needs no coroutine of its
// own and no region correction. The failing attempt at the first posedge ends the
// victim block there.
TEST_CASE("an assertion action block can disable a block") {
    CHECK(run_capture(R"(
        module m;
            reg clk = 0, a = 0;
            always #5 clk = ~clk;
            initial begin
                begin : victim
                    #1000 $display("victim never");
                end
                $display("victim ended at %0t", $time);
            end
            assert property (@(posedge clk) a) else disable victim;
            initial #40 $finish;
        endmodule
    )") == "victim ended at 5\n");
}

// A `#0` inside a target parks in the inactive batch, which has no claim field. It
// needs none: the batch's own promotion later in the same instant is the wake, and
// the park marks itself scheduled so a `disable` arriving meanwhile stands down
// instead of pushing a second copy of the same handle. Two `#0`s and a `#0`
// disabler is the shape that double-woke before the mark, so it is the one pinned.
// Oracle: iverilog.
TEST_CASE("a #0 inside a disabled block is woken exactly once") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                begin : blk
                    #0 $display("z1 at %0t", $time);
                    #0 $display("z2 at %0t", $time);
                    #50 $display("never");
                end
                $display("after blk at %0t", $time);
                #1 $display("done at %0t", $time);
            end
            initial begin
                #0 disable blk;
                $display("disabled at %0t", $time);
            end
        endmodule
    )") == "z1 at 0\ndisabled at 0\nafter blk at 0\ndone at 1\n");
}

// -- foreach (12.7.3). Oracle: iverilog. --

// Single-dimension foreach over a fixed-size unpacked array, ascending
// declared range: the loop variable is implicitly `int` and reads back
// exactly the elements written through it.
TEST_CASE("foreach writes and reads back a 1-D unpacked array, ascending range") {
    CHECK(run_capture(R"(
        module m;
            integer mem [0:3];
            initial begin
                foreach (mem[i]) mem[i] = i * 10;
                $display("%0d %0d %0d %0d", mem[0], mem[1], mem[2], mem[3]);
            end
        endmodule
    )") == "0 10 20 30\n");
}

// A descending declared range ([3:0]) iterates in declared left-to-right
// order, i.e. counting down (matches the LRM's own `B[5:1]` example, which
// iterates 5 down to 1).
TEST_CASE("foreach over a descending declared range counts down") {
    CHECK(run_capture(R"(
        module m;
            integer arr [3:0];
            initial foreach (arr[i]) $display("%0d", i);
        endmodule
    )") == "3\n2\n1\n0\n");
}

// Multiple loop variables nest, innermost varying fastest (outer loops
// correspond to the lower-cardinality / leftmost dimensions).
TEST_CASE("foreach over a 2-D array nests with the inner dimension fastest") {
    CHECK(run_capture(R"(
        module m;
            integer a [0:1][0:2];
            initial foreach (a[i, j]) $display("%0d %0d", i, j);
        endmodule
    )") == "0 0\n0 1\n0 2\n1 0\n1 1\n1 2\n");
}

// foreach lowers to nested Loop instructions, one per iterated dimension.
TEST_CASE("foreach lowers to a Loop instruction") {
    const LowerResult r = lower_text(R"(
        module m;
            integer a [0:1][0:2];
            initial foreach (a[i, j]) $display("%0d %0d", i, j);
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Loop));
}

// A skipped (omitted) loop variable means no iteration over that dimension at
// all -- not a hidden loop over its full extent. Regression: an earlier
// misreading of 12.7.3 treated a skipped dimension as "iterate but don't
// bind," which would multiply the count by the skipped dimension's size (24
// instead of 8 here: dim2's size-3 must NOT be a factor).
TEST_CASE("foreach with a skipped dimension does not iterate over it") {
    CHECK(run_capture(R"(
        module m;
            integer a [0:1][0:2][0:3];
            integer count;
            initial begin
                count = 0;
                foreach (a[i, , k]) count = count + 1;
                $display("%0d", count);
            end
        endmodule
    )") == "8\n");
}

// continue in a nested foreach applies to the innermost dimension only.
TEST_CASE("continue in a nested foreach skips only the innermost iteration") {
    CHECK(run_capture(R"(
        module m;
            integer a [0:1][0:2];
            integer count;
            initial begin
                count = 0;
                foreach (a[i, j]) begin
                    if (j == 1) continue;
                    count = count + 1;
                end
                $display("%0d", count);
            end
        endmodule
    )") == "4\n");
}

// break in a nested foreach exits only the innermost loop; the outer
// dimension keeps iterating.
TEST_CASE("break in a nested foreach exits only the innermost loop") {
    CHECK(run_capture(R"(
        module m;
            integer a [0:1][0:2];
            integer count;
            initial begin
                count = 0;
                foreach (a[i, j]) begin
                    if (j == 1) break;
                    count = count + 1;
                end
                $display("%0d", count);
            end
        endmodule
    )") == "2\n");
}

// A string is treated as a dynamic array of bytes indexed 0..N-1 (12.7.3); the
// bound is the runtime length, not a compile-time range.
TEST_CASE("foreach over a string iterates its bytes") {
    CHECK(run_capture(R"(
        module m;
            string s;
            initial begin
                s = "abc";
                foreach (s[i]) $display("%0d %0d", i, s[i]);
            end
        endmodule
    )") == "0 97\n1 98\n2 99\n");
}

// Dynamic arrays now lower (LRM 7.5); foreach iterates 0..size-1 over the
// runtime length. Queues and associative arrays are still unmodeled and must
// fail loudly rather than silently mis-iterate (see below).
TEST_CASE("foreach over a dynamic array iterates the runtime size") {
    CHECK(run_capture(R"(
        module m;
            integer dyn [];
            initial begin
                dyn = new[3];
                foreach (dyn[i]) dyn[i] = i + 5;
                foreach (dyn[i]) $display("%0d %0d", i, dyn[i]);
            end
        endmodule
    )") == "0 5\n1 6\n2 7\n");
}

TEST_CASE("foreach over a queue iterates the runtime size") {
    CHECK(run_capture(R"(
        module m;
            integer q [$];
            initial begin
                q.push_back(5); q.push_back(6); q.push_back(7);
                foreach (q[i]) $display("%0d %0d", i, q[i]);
            end
        endmodule
    )") == "0 5\n1 6\n2 7\n");
}

// foreach over an associative array binds the loop variable to each key in
// ascending order (LRM 7.9.3); see associative_array_test.cpp for the runtime
// behavior. Here we only check it lowers cleanly.
TEST_CASE("foreach over an associative array lowers") {
    const LowerResult r = lower_text(R"(
        module m;
            integer assoc [int];
            int i;
            initial foreach (assoc[i]) $display("%0d", i);
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

// A loop's hidden counter and bound are per-ACTIVATION cells, not module state.
// Inside a subroutine they are frame Locals, so a recursive call gets its own:
// three levels of `repeat (3)` run 3 + 9 + 27 == 39 body iterations. Backed by a
// shared module signal, the inner call's counter clobbered the outer's and the
// whole nest collapsed to 5. Cross-checked against Icarus Verilog.
TEST_CASE("a repeat counter is per-activation, so recursion nests correctly") {
    const std::string out = run_capture(R"(
        module m;
            int total;
            task automatic count(int depth);
                repeat (3) begin
                    total = total + 1;
                    if (depth > 0) count(depth - 1);
                end
            endtask
            initial begin total = 0; count(2); $display("%0d", total); end
        endmodule
    )");
    CHECK(out == "39\n");
}

// The same task suspended in two concurrent activations. Each `repeat` count
// has to survive its own activation's suspends without the other's writes
// reaching it -- one shared cell made the shorter loop's exit end both.
TEST_CASE("concurrent activations of one task keep separate repeat counters") {
    const std::string out = run_capture_finish(R"(
        module m;
            int a, b;
            task automatic spin(input int n, output int got);
                int k;
                k = 0;
                repeat (n) begin #1; k = k + 1; end
                got = k;
            endtask
            initial begin
                fork spin(3, a); spin(5, b); join
                $display("%0d %0d", a, b);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "3 5\n");
}

// foreach snapshots its bound in the same kind of cell, so it nests too.
TEST_CASE("a foreach bound is per-activation, so recursion nests correctly") {
    const std::string out = run_capture(R"(
        module m;
            int total;
            task automatic walk(input int depth);
                int arr[];
                arr = new[3];
                foreach (arr[i]) begin
                    total = total + 1;
                    if (depth > 0) walk(depth - 1);
                end
            endtask
            initial begin total = 0; walk(2); $display("%0d", total); end
        endmodule
    )");
    CHECK(out == "39\n");
}

// `lhs = repeat (n) @e rhs` (LRM 9.4.5) counts trigger occurrences in a cell of
// its own, so two activations waiting on the same edge resume at their own
// counts rather than whichever finished first.
TEST_CASE("an intra-assignment repeat count is per-activation") {
    const std::string out = run_capture_finish(R"(
        module m;
            reg clk = 0;
            int x, y, v;
            always #1 clk = ~clk;
            task automatic grab(input int n, output int t);
                v = repeat (n) @(posedge clk) 7;
                t = $time;
            endtask
            initial begin
                fork grab(1, x); grab(3, y); join
                $display("%0d %0d", x, y);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "1 5\n");
}

// A process has no frame, so its loop cells stay module signals and the emitted
// IR is unchanged: the loop below is the only one in the design whose counter
// needs storage, and it still resolves through a Signal, not a Local.
TEST_CASE("a process-level repeat still uses module-signal storage") {
    const LowerResult r = lower_text(R"(
        module m;
            int s;
            initial repeat (4) s = s + 1;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(any_process_has_tag(*r.module, ir::Inst::Tag::Signal));
    CHECK_FALSE(any_process_has_tag(*r.module, ir::Inst::Tag::Local));
}
