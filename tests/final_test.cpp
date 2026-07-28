#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// `final` blocks per LRM §9.2.3: run once at the end of simulation, on both the
// natural-drain and $finish exit paths, after every other process has settled,
// observing the final design state. Ordering among multiple final blocks is
// unspecified by the LRM; Clotho runs them in source order (deterministic).
// Cross-checked against Icarus Verilog.

namespace {

// JIT-run a module and capture stdout (mirrors display_test.cpp).
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

// Assert that `source` is rejected with a diagnostic containing `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
}

} // namespace

// A final block runs at end of simulation, after the initial block completes.
TEST_CASE("final runs once at end of simulation") {
    CHECK(run_capture(R"(
        module m;
            initial $display("initial");
            final $display("final");
        endmodule
    )") == "initial\nfinal\n");
}

// The final block observes the settled state left by the run, not time-zero
// values: it sees the last value written to a signal.
TEST_CASE("final observes the final design state") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] x;
            initial begin
                x = 8'd1;
                #5 x = 8'd42;
            end
            final $display("x=%0d", x);
        endmodule
    )") == "x=42\n");
}

// A final block runs even when the simulation ends via an explicit $finish (the
// $finish path longjmps out of the drain; the final block still executes).
TEST_CASE("final runs after an explicit $finish") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display("before finish");
                $finish;
                $display("after finish");
            end
            final $display("final ran");
        endmodule
    )") == "before finish\nfinal ran\n");
}

// Multiple final blocks all run; Clotho orders them in source order.
TEST_CASE("multiple final blocks run in source order") {
    CHECK(run_capture(R"(
        module m;
            final $display("final A");
            final $display("final B");
            final $display("final C");
        endmodule
    )") == "final A\nfinal B\nfinal C\n");
}

// A final block runs after time has advanced through the whole run; it sees the
// last-scheduled value across timed updates.
TEST_CASE("final runs after all timed activity completes") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] c;
            initial c = 0;
            initial begin
                #1 c = c + 1;
                #1 c = c + 1;
                #1 c = c + 1;
            end
            final $display("count=%0d", c);
        endmodule
    )") == "count=3\n");
}

// A final block is zero-time (LRM 9.2.3): a timing control inside it is illegal.
// slang diagnoses this at elaboration; Clotho's own guard is defense in depth.
TEST_CASE("timing control in a final block is rejected") {
    check_rejected(R"(
        module m;
            initial $display("run");
            final #5 $display("bad");
        endmodule
    )", "final");
}
