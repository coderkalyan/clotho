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

// A real count is legal (12.7.2 requires only a numeric expression) and converts
// to an integer by rounding half away from zero (LRM 6.12.1), so 2.5 runs the
// body three times where 2.4 runs it twice; a negative one rounds to a negative
// and runs it none, which is 12.7.2's negative-is-zero rule. The runtime-valued
// pair at the end is the same conversion outside a folded literal. Every count
// pinned against iverilog 13.
TEST_CASE("repeat loop with a real count rounds half away from zero") {
    CHECK(run_capture(R"(
        module m;
            int  c;
            real r;
            initial begin
                c = 0; repeat (2.7) c = c + 1; $display("%0d", c);
                c = 0; repeat (2.4) c = c + 1; $display("%0d", c);
                c = 0; repeat (2.5) c = c + 1; $display("%0d", c);
                c = 0; repeat (3.5) c = c + 1; $display("%0d", c);
                c = 0; repeat (-0.6) c = c + 1; $display("%0d", c);
                r = 1.5;  c = 0; repeat (r) c = c + 1; $display("%0d", c);
                r = -2.7; c = 0; repeat (r) c = c + 1; $display("%0d", c);
            end
        endmodule
    )") == "3\n2\n3\n4\n0\n2\n0\n");
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
// deferred reports — "cancelled", "cleared", never reported (docs/LRM_NOTES.md, note 22). The
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

// LRM 9.6.2: disabling a named block ends "all activities enabled within the named
// block", and a named `fork` is a block whose activity is its children. `disable F`
// used to walk an empty list, so it was a silent no-op from inside the fork and from
// another process alike, and the join waited out every child. A child is now an
// activation of each targeted block it was forked inside, linked when it is spawned,
// so a child that is only queued (join_none, join_any) is ended too. Oracle: iverilog
// 13 on every design here.
TEST_CASE("disable of a named fork ends its children") {
    SUBCASE("from one of its own branches, join") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        #50 $display("A alive %0t", $time);
                        #5 disable F;
                    join
                    $display("after F %0t", $time);
                end
            endmodule
        )") == "after F 5\n");
    }
    SUBCASE("from another process, join") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        #50 $display("A alive %0t", $time);
                        #60 $display("B alive %0t", $time);
                    join
                    $display("after F %0t", $time);
                end
                initial #5 disable t.F;
            endmodule
        )") == "after F 5\n");
    }
    SUBCASE("join_any: the children still running are ended") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        #50 $display("A alive %0t", $time);
                        #5 $display("B done %0t", $time);
                    join_any
                    $display("after F %0t", $time);
                    #5 disable F;
                    #100 $display("end %0t", $time);
                end
            endmodule
        )") == "B done 5\nafter F 5\nend 110\n");
    }
    SUBCASE("join_any, disabled from another process while the parent has moved on") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        #50 $display("A alive %0t", $time);
                        #5 $display("B done %0t", $time);
                    join_any
                    $display("after F %0t", $time);
                    #200 $display("end %0t", $time);
                end
                initial #10 disable t.F;
            endmodule
        )") == "B done 5\nafter F 5\nend 205\n");
    }
    SUBCASE("join_none, from the parent and from another process") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        #50 $display("A alive %0t", $time);
                    join_none
                    $display("after F %0t", $time);
                    #5 disable F;
                    #100 $display("end %0t", $time);
                end
            endmodule
        )") == "after F 0\nend 105\n");
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        #50 $display("A alive %0t", $time);
                    join_none
                    $display("after F %0t", $time);
                    #100 $display("end %0t", $time);
                end
                initial #5 disable t.F;
            endmodule
        )") == "after F 0\nend 100\n");
    }
    SUBCASE("a child that has not started yet never runs") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        $display("child ran %0t", $time);
                    join_none
                    disable F;
                    #10 $display("end %0t", $time);
                end
            endmodule
        )") == "end 10\n");
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        begin #5 $display("child ran %0t", $time); end
                    join_none
                    #1 disable F;
                    #10 $display("end %0t", $time);
                end
            endmodule
        )") == "end 11\n");
    }
    SUBCASE("a child parked on an event is not woken by it afterwards") {
        CHECK(run_capture(R"(
            module t;
                event e;
                initial begin
                    fork : F
                        @e $display("A woke");
                        #5 $display("B done %0t", $time);
                    join_any
                    disable F;
                    #1 -> e;
                    #10 $display("end %0t", $time);
                end
            endmodule
        )") == "B done 5\nend 16\n");
    }
    SUBCASE("a block that is entered again is disabled again") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    for (int i = 0; i < 2; i++) begin
                        fork : F
                            #50 $display("A alive i=%0d %0t", i, $time);
                            #5 disable F;
                        join
                        $display("after F i=%0d %0t", i, $time);
                    end
                    #100 $display("end %0t", $time);
                end
            endmodule
        )") == "after F i=0 5\nafter F i=1 10\nend 110\n");
    }
    SUBCASE("two instances: each disable reaches its own instance's children") {
        CHECK(run_capture(R"(
            module sub #(parameter D = 5);
                initial begin
                    fork : F
                        #50 $display("%m alive");
                        #D disable F;
                    join
                    $display("%m after %0t", $time);
                end
            endmodule
            module t; sub #(5) a(); sub #(7) b(); endmodule
        )") == "t.a after 5\nt.b after 7\n");
    }
    SUBCASE("a fork nested in a child of the disabled fork") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    fork : F
                        begin
                            fork
                                #50 $display("G alive %0t", $time);
                            join
                            $display("child after inner %0t", $time);
                        end
                        #5 disable F;
                    join
                    $display("after F %0t", $time);
                end
            endmodule
        )") == "after F 5\n");
    }
}

// The sibling construct: a `begin` block with a fork in it. Its children are activity
// within it, so a lexical disable has to end them as well as leave the block, and a
// disable from one of those children, or from another process while the parent is
// parked at the join, ends the whole block.
TEST_CASE("disable of a begin block ends the children forked inside it") {
    SUBCASE("lexically, from the parent") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    begin : B
                        fork
                            #50 $display("child alive %0t", $time);
                        join_none
                        #5 disable B;
                        $display("not reached");
                    end
                    $display("after B %0t", $time);
                    #100 $display("end %0t", $time);
                end
            endmodule
        )") == "after B 5\nend 105\n");
    }
    SUBCASE("from a forked child, the parent parked at the join") {
        CHECK(run_capture(R"(
            module t;
                initial begin
                    begin : B
                        fork
                            #5 disable B;
                            #50 $display("A alive %0t", $time);
                        join
                        $display("not reached %0t", $time);
                    end
                    $display("after B %0t", $time);
                end
            endmodule
        )") == "after B 5\n");
    }
    SUBCASE("from another process, the parent parked at the join") {
        CHECK(run_capture(R"(
            module t;
                initial begin : P
                    begin : B
                        #1;
                        fork
                            #50 $display("A alive %0t", $time);
                        join
                        $display("not reached %0t", $time);
                    end
                    $display("after B %0t", $time);
                end
                initial #5 disable t.P.B;
            endmodule
        )") == "after B 5\n");
    }
}

// What is still refused is a park the abort cannot claim, or children the block does
// not hold.
TEST_CASE("disable of a block with forked children that park in a call is refused") {
    const LowerResult call_park = lower_text(R"(
        module t;
            task automatic tk(input int n); #n; endtask
            initial begin
                fork : F
                    tk(50);
                    #5 disable F;
                join
            end
        endmodule
    )");
    CHECK_FALSE(call_park.ok);
    CHECK(call_park.diagnostic_text().find("cannot claim that park") != std::string::npos);

    const LowerResult via_call = lower_text(R"(
        module t;
            task automatic spawn; fork #50 $display("x"); join_none endtask
            initial begin : B
                spawn();
                #5;
            end
            initial #1 disable B;
        endmodule
    )");
    CHECK_FALSE(via_call.ok);
    CHECK(via_call.diagnostic_text().find("calls a subroutine that forks") != std::string::npos);
}

// A `join` / `join_any` child runs INLINE at the spawn, with the parent on the stack and
// not yet parked. If that child disables a block the parent is an occupant of, the
// force-wake used to push the parent's handle while it was running, and the drain
// resumed it a second time (a crash, or a hang). The parent now holds its scheduled
// flag across the inline run, so the wake stands down, only the mark lands, and the
// parent leaves the block at the poll after the fork. Oracle: iverilog 13 on each.
TEST_CASE("a fork child that disables its parent's block before it suspends") {
    SUBCASE("join, the parent in a begin block") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin
                begin : B
                  fork
                    disable B;
                    #5 $display("FAILED: sibling alive");
                  join
                  $display("FAILED: not reached");
                end
                $display("after B %0t", $time);
              end
              initial #10 $display("end %0t", $time);
            endmodule
        )") == "after B 0\nend 10\n");
    }
    SUBCASE("join_any") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin
                begin : B
                  fork
                    #5 $display("FAILED: sibling alive");
                    disable B;
                  join_any
                  $display("FAILED: not reached");
                end
                $display("after B %0t", $time);
              end
              initial #10 $display("end %0t", $time);
            endmodule
        )") == "after B 0\nend 10\n");
    }
    SUBCASE("the parent is a task activation") {
        CHECK(run_capture_finish(R"(
            module t;
              task automatic run();
                begin : B
                  fork
                    disable B;
                    #5 $display("FAILED: sibling alive");
                  join
                  $display("FAILED: not reached");
                end
                $display("after B %0t", $time);
              endtask
              initial run();
              initial #10 $display("end %0t", $time);
            endmodule
        )") == "after B 0\nend 10\n");
    }
    SUBCASE("the block is the procedure's outermost block") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin : P
                fork
                  disable P;
                join
                $display("FAILED: not reached");
              end
              initial #10 $display("end %0t", $time);
            endmodule
        )") == "end 10\n");
    }
    SUBCASE("a grandchild, run inline inside a child that is itself running inline") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin
                fork : F
                  begin
                    fork
                      disable F;
                    join
                    $display("FAILED: child continued");
                  end
                  #5 $display("FAILED: sibling alive");
                join
                $display("after F %0t", $time);
              end
              initial #10 $display("end %0t", $time);
            endmodule
        )") == "after F 0\nend 10\n");
    }
    SUBCASE("the parent had waited before it forked") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin
                #1;
                begin : B
                  fork
                    disable B;
                    #5 $display("FAILED: sibling alive");
                  join
                  $display("FAILED: not reached");
                end
                $display("after B %0t", $time);
              end
              initial #10 $display("end %0t", $time);
            endmodule
        )") == "after B 1\nend 10\n");
    }
}

// A targeted block keeps this activation's node on its occupant list until its END
// block unlinks it. Every exit that skips that block (a `disable` of an outer block, a
// `break` / `continue` out of a loop it is in, a `return`, and every abort, which goes
// straight to the epilogue) left the node linked. The frame was then freed or entered
// again, and the next walk of the list followed a dangling pointer (a crash), went round
// a cycle (a hang), or woke the process that had since moved on. Each exit now unlinks
// the nodes it passes, innermost first. Oracle: iverilog 13, except where noted.
TEST_CASE("an exit that skips a block's end takes the activation off its list") {
    SUBCASE("a fork child ended while parked inside its own block, by a sibling") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin
                for (int i = 0; i < 3; i++) begin : B
                  fork
                    begin : C
                      #10;
                      if (i != 0) disable C;
                      $display("i=%0d C body after #10 at %0t", i, $time);
                      #10;
                    end
                    #5 if (i == 0) disable B;
                  join
                  $display("after iteration %0d at %0t", i, $time);
                end
                $display("done at %0t", $time);
              end
            endmodule
        )") == "after iteration 1 at 15\nafter iteration 2 at 25\ndone at 25\n");
    }
    SUBCASE("a fork child that ends its own enclosing block from inside a nested one") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin
                for (int i = 0; i < 3; i++) begin : B
                  fork
                    begin : C
                      #5;
                      if (i == 0) disable B;
                      if (i != 0) disable C;
                      #10;
                    end
                  join
                  $display("after iteration %0d at %0t", i, $time);
                end
                $display("done at %0t", $time);
              end
            endmodule
        )") == "after iteration 1 at 10\nafter iteration 2 at 15\ndone at 15\n");
    }
    SUBCASE("`disable fork` of a child parked inside its own block") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin
                for (int i = 0; i < 3; i++) begin
                  fork
                    begin : C
                      #10;
                      if (i != 0) disable C;
                      #10;
                    end
                  join_none
                  #5 if (i == 0) disable fork;
                  wait fork;
                  $display("after iteration %0d at %0t", i, $time);
                end
                $display("done at %0t", $time);
              end
            endmodule
        )") == "after iteration 0 at 5\nafter iteration 1 at 15\nafter iteration 2 at 25\ndone at 25\n");
    }
    SUBCASE("a structured exit to an outer block from inside an inner remotely-named one") {
        CHECK(run_capture_finish(R"(
            module t;
              int i;
              initial begin : P
                for (i = 0; i < 3; i++) begin : B
                  begin : C
                    #5;
                    if (i == 0) disable B;
                    #10;
                  end
                end
                $display("done at %0t", $time);
              end
              initial begin
                #12 disable t.P.B.C;
                $display("remote disable ran at %0t", $time);
              end
            endmodule
        )") == "remote disable ran at 12\ndone at 27\n");
    }
    SUBCASE("a remote disable of an outer block while the process is parked in an inner one") {
        CHECK(run_capture_finish(R"(
            module t;
              initial begin : P
                begin : OUT
                  begin : IN
                    #10;
                  end
                  #10 $display("FAILED: OUT continued");
                end
                $display("after OUT %0t", $time);
                #100 $display("end %0t", $time);
              end
              initial begin #5 disable t.P.OUT; #2 disable t.P.OUT.IN; end
            endmodule
        )") == "after OUT 5\nend 105\n");
    }
    SUBCASE("a `return` from a task's block") {
        CHECK(run_capture_finish(R"(
            module t;
              task automatic tk(input int i);
                begin : B
                  if (i == 0) return;
                  if (i == 2) begin #5 disable B; end
                  #10 $display("FAILED: tk %0d survived at %0t", i, $time);
                end
              endtask
              initial begin
                tk(0);
                fork tk(1); join_none
                #1;
                tk(2);
                #10 $display("end %0t", $time);
              end
            endmodule
        )") == "end 16\n");
    }
    SUBCASE("a `continue` out of a block that a later disable names") {
        CHECK(run_capture_finish(R"(
            module t;
              int i;
              initial begin : P
                for (i = 0; i < 3; i++) begin : L
                  begin : B
                    #5;
                    if (i == 0) continue;
                    #10;
                  end
                end
                $display("after loop %0t", $time);
                #100 $display("end %0t", $time);
              end
              initial #12 disable t.P.L.B;
            endmodule
        )") == "after loop 27\nend 127\n");
    }
    // LRM 12.8 ends the loop at the `break`, and 9.6.2 makes the later `disable` of a
    // block that is not executing a no-op. iverilog 13 prints a second "after loop 42"
    // for this design, which contradicts both clauses, so the expected value is the LRM's.
    SUBCASE("a `break` out of a block that a later disable names") {
        CHECK(run_capture_finish(R"(
            module t;
              int i;
              initial begin : P
                for (i = 0; i < 3; i++) begin : L
                  begin : B
                    #5;
                    if (i == 0) break;
                    #10;
                  end
                end
                $display("after loop %0t", $time);
                #100 $display("end %0t", $time);
              end
              initial #12 disable t.P.L.B;
            endmodule
        )") == "after loop 5\nend 105\n");
    }
}

// LRM 16.4.4: disabling the outermost scope of a procedure drops its pending deferred
// reports. A fork child that does it (an activation of that block, with no region of its
// own to leave) skipped the flush, and the report matured after the procedure had ended.
// A deferred assertion's action is a severity task, whose message goes to stderr. No
// oracle: iverilog does not implement deferred assertions.
TEST_CASE("a fork child's disable of the outermost block drops its deferred reports") {
    const test::DesignRun dropped = test::run_design_captured(R"(
        module t;
          bit a = 0;
          initial begin : P
            #5;
            assert #0 (a) else $error("pending report of P not flushed");
            fork
              #0 disable P;
            join
            $display("FAILED: not reached");
          end
          initial #10 $display("end %0t", $time);
        endmodule
    )");
    CHECK(dropped.out == "end 10\n");
    CHECK(dropped.err.find("not flushed") == std::string::npos);

    // Without the disable the same report matures, so the case above is a drop and not
    // a report that never existed.
    const test::DesignRun matured = test::run_design_captured(R"(
        module t;
          bit a = 0;
          initial begin : P
            #5;
            assert #0 (a) else $error("pending report of P matured");
            fork
              #0 ;
            join
            #1 $display("P done %0t", $time);
          end
          initial #10 $display("end %0t", $time);
        endmodule
    )");
    CHECK(matured.out == "P done 6\nend 10\n");
    CHECK(matured.err.find("matured") != std::string::npos);
}

// The children of a fork a block holds are ended with it only when the fork is lexically
// inside the block. One reached through a virtual call (any override in the design may
// be the one that runs) or a randsequence production (lowered to a function of its own)
// is not, so a disable of such a block is refused rather than left to run on.
TEST_CASE("disable of a block that may fork through a virtual call or a randsequence is refused") {
    const std::string classes = R"(
        class Base; virtual task start(); endtask endclass
        class Spawner extends Base;
          virtual task start();
            fork #50 $display("grandchild"); join_none
          endtask
        endclass
        class Quiet extends Base;
          virtual task start(); #1; endtask
        endclass
    )";
    const LowerResult virt    = lower_text(classes + R"(
        module t;
          Base h;
          Spawner s = new;
          initial begin
            h = s;
            begin : B
              h.start();
              #10;
            end
          end
          initial #5 disable t.B;
        endmodule
    )");
    CHECK_FALSE(virt.ok);
    CHECK(virt.diagnostic_text().find("calls a subroutine that forks") != std::string::npos);

    const LowerResult rs = lower_text(R"(
        module t;
          initial begin : P
            begin : B
              randsequence (main)
                main : { fork #50 $display("production child"); join_none };
              endsequence
              #10;
            end
          end
          initial #5 disable t.P.B;
        endmodule
    )");
    CHECK_FALSE(rs.ok);
    CHECK(rs.diagnostic_text().find("calls a subroutine that forks") != std::string::npos);

    // A virtual call is not refused when no virtual method of the design forks.
    const LowerResult quiet = lower_text(R"(
        class Base; virtual task start(); endtask endclass
        class Quiet extends Base;
          virtual task start(); #1; endtask
        endclass
        module t;
          Base h;
          Quiet q = new;
          initial begin
            h = q;
            begin : B
              h.start();
              #10;
            end
          end
          initial #5 disable t.B;
        endmodule
    )");
    CHECK_MESSAGE(quiet.ok, quiet.diagnostic_text());
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

// -- Block-local lifetime (LRM 6.21) -------------------------------------------
//
// A declaration in a procedural block defaults to STATIC lifetime: one instance,
// initialized once before time zero. An `automatic` one has the lifetime of the
// BLOCK and is initialized on every entry to it -- with its initializer if it has
// one, else with the type's default (LRM 6.8, Table 6-7).
//
// Neither reset happened: an automatic declaration with no initializer kept the
// previous entry's value, and a static one re-ran its initializer on every entry, so
// both lifetimes behaved the same way and neither matched the LRM.

TEST_CASE("the LRM 6.21 lifetime example prints what the LRM says it prints") {
    // Verbatim from LRM 6.21 (module top_legal), including its two expected lines.
    CHECK(run_capture(R"(
        module m;
            initial begin
                for (int i = 0; i < 3; i++) begin
                    automatic int loop3 = 0;   // executes every loop
                    for (int j = 0; j < 3; j++) begin
                        loop3++;
                        $write("%0d ", loop3);
                    end
                end
                $display("");
                for (int i = 0; i < 3; i++) begin
                    static int loop2 = 0;      // executes once at time zero
                    for (int j = 0; j < 3; j++) begin
                        loop2++;
                        $write("%0d ", loop2);
                    end
                end
                $display("");
            end
        endmodule
    )") == "1 2 3 1 2 3 1 2 3 \n"
           "1 2 3 4 5 6 7 8 9 \n");
}

TEST_CASE("an automatic block local with no initializer starts at its type default") {
    // Table 6-7: x for a 4-state integral, 0 for 2-state, 0.0 for real, "" for a
    // string, null for a class handle, empty for a container, per leaf for an
    // aggregate. Each line reads the variable BEFORE assigning it, so a value from
    // the previous entry would show.
    CHECK(run_capture(R"(
        typedef struct {
            int         a;
            logic [3:0] f;
        } t;
        class C;
            int v;
        endclass
        module m;
            initial begin
                for (int k = 0; k < 2; k++) begin
                    automatic logic [3:0] y;
                    automatic int         n;
                    automatic real        r;
                    automatic string      s;
                    automatic C           h;
                    automatic t           st;
                    automatic int         arr[2];
                    automatic int         q[$];
                    automatic int         d[];
                    automatic int         a[string];
                    $display("k%0d y=%0h n=%0d r=%0g s=[%s] null=%0b st=%0d,%0h arr=%0d q=%0d d=%0d a=%0d",
                             k, y, n, r, s, h == null, st.a, st.f, arr[1], q.size(), d.size(), a.num());
                    y   = 4'h5;
                    n   = 7;
                    r   = 1.5;
                    s   = "x";
                    h   = new;
                    st  = '{a: 1, f: 4'h2};
                    arr = '{9, 9};
                    q.push_back(1);
                    d = new[3];
                    a["k"] = 1;
                end
            end
        endmodule
    )") == "k0 y=x n=0 r=0 s=[] null=1 st=0,x arr=0 q=0 d=0 a=0\n"
           "k1 y=x n=0 r=0 s=[] null=1 st=0,x arr=0 q=0 d=0 a=0\n");
}

TEST_CASE("a static block local keeps its value across entries, a container included") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                for (int k = 0; k < 3; k++) begin
                    static int q[$];
                    static int n = 100;
                    q.push_back(k);
                    n++;
                    $display("k%0d n=%0d size=%0d last=%0d", k, n, q.size(), q[$]);
                end
            end
        endmodule
    )") == "k0 n=101 size=1 last=0\n"
           "k1 n=102 size=2 last=1\n"
           "k2 n=103 size=3 last=2\n");
}

TEST_CASE("a block re-entered within one call resets its automatic locals") {
    // The frame is per CALL, so a block inside it needs its own reset -- a queue
    // local in a loop body would otherwise accumulate across iterations of one call.
    CHECK(run_capture(R"(
        module m;
            function automatic int rows(int n);
                int total = 0;
                for (int k = 0; k < n; k++) begin
                    automatic int q[$];
                    automatic int seen;
                    for (int j = 0; j <= k; j++) q.push_back(j);
                    seen  = q.size();
                    total = total + seen;
                    $display("row %0d size=%0d", k, seen);
                end
                return total;
            endfunction
            initial $display("total=%0d", rows(3));
        endmodule
    )") == "row 0 size=1\n"
           "row 1 size=2\n"
           "row 2 size=3\n"
           "total=6\n");
}

TEST_CASE("a static local in an automatic subroutine still initializes once") {
    CHECK(run_capture(R"(
        module m;
            function automatic int tick();
                static int n = 0;
                automatic int fresh = 0;
                n++;
                fresh++;
                return n * 10 + fresh;
            endfunction
            initial $display("%0d %0d %0d", tick(), tick(), tick());
        endmodule
    )") == "11 21 31\n");
}

// -- foreach over a NESTED runtime-sized dimension (LRM 12.7.3) --
//
// Each dimension iterates its own container: dimension 2 of an assoc of queues is
// the entry the outer dimension is sitting on, so its bound is that entry's size and
// its index is 0-based. Deriving every dimension's bound from the whole array (what
// the lowering used to do) re-walked the OUTER container, which bound the inner index
// to a key and gave every entry the same trip count.
//
// No oracle: iverilog does not compile a `foreach` over an associative array of
// queues.
TEST_CASE("foreach over an associative array of queues iterates each entry's own size") {
    CHECK(run_capture(R"(
        module m;
            int all[string][$];
            initial begin
                all["a"].push_back(10);
                all["a"].push_back(20);
                all["b"].push_back(30);
                foreach (all[k, i]) $display("%s %0d %0d", k, i, all[k][i]);
            end
        endmodule
    )") == "a 0 10\na 1 20\nb 0 30\n");
}

TEST_CASE("foreach over nested containers: assoc of assoc, queue of queue, assoc of dynarr") {
    CHECK(run_capture(R"(
        module m;
            int aa[string][string];
            int qq[$][$];
            int ad[string][];
            initial begin
                aa["a"]["x"] = 1;
                aa["a"]["y"] = 2;
                aa["b"]["z"] = 3;
                foreach (aa[i, j]) $display("aa %s %s %0d", i, j, aa[i][j]);
                begin
                    int inner[$];
                    inner.push_back(7);
                    inner.push_back(8);
                    qq.push_back(inner);
                    inner.delete();
                    inner.push_back(9);
                    qq.push_back(inner);
                end
                foreach (qq[i, j]) $display("qq %0d %0d %0d", i, j, qq[i][j]);
                ad["k"] = new[2];
                ad["k"][1] = 5;
                foreach (ad[i, j]) $display("ad %s %0d %0d", i, j, ad[i][j]);
            end
        endmodule
    )") == "aa a x 1\naa a y 2\naa b z 3\n"
           "qq 0 0 7\nqq 0 1 8\nqq 1 0 9\n"
           "ad k 0 0\nad k 1 5\n");
}

// The shape that motivated the fix (uvm_resource_pool.svh): iterate an associative
// array of queues and read each element. Both halves — the per-entry bound and the
// select through the entry — are needed for this to run at all.
TEST_CASE("foreach over an assoc of queues, reading each element") {
    CHECK(run_capture(R"(
        module m;
            int all[string][$];
            int q[$];
            initial begin
                all["a"].push_back(1);
                all["a"].push_back(2);
                all["b"].push_back(3);
                foreach (all[aa_iter, q_iter]) q.push_front(all[aa_iter][q_iter]);
                $display("%0d %0d %0d size=%0d", q[0], q[1], q[2], q.size());
            end
        endmodule
    )") == "3 2 1 size=3\n");
}
