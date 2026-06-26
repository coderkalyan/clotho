#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module's initial blocks and return everything it wrote to stdout.
// Loop bodies report their progress with $display, so the captured text is the
// observable behavior.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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
