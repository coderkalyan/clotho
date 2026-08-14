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

// A block that does not enclose the `disable` has to abort an activation other
// than this one, which needs the scheduler work V2 owes. Both shapes refuse: a
// sibling block in the same process, and LRM example 3's cross-process form.
TEST_CASE("disable of a block that does not enclose it is rejected") {
    const LowerResult sibling = lower_text(R"(
        module m;
            integer x;
            initial begin
                begin : first
                    x = 1;
                end
                begin : second
                    disable first;
                end
            end
        endmodule
    )");
    CHECK_FALSE(sibling.ok);
    CHECK(sibling.diagnostic_text().find("does not enclose it") != std::string::npos);

    const LowerResult remote = lower_text(R"(
        module m;
            integer x;
            always begin : always1
                #5 x = x + 1;
            end
            initial #7 disable m.always1;
        endmodule
    )");
    CHECK_FALSE(remote.ok);
    CHECK(remote.diagnostic_text().find("does not enclose it") != std::string::npos);
}

// LRM 16.4.4: disabling the outermost scope of a procedure DROPS its pending
// deferred reports, which this exit does not carry yet. A block nested deeper
// never flushes, so it stays legal and the report still matures.
TEST_CASE("disable of an outermost block holding deferred assertions is rejected") {
    const LowerResult r = lower_text(R"(
        module m;
            integer x;
            initial begin : whole
                x = 0;
                assert #0 (x == 1) else $error("bad");
                disable whole;
            end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("LRM 16.4.4") != std::string::npos);

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
