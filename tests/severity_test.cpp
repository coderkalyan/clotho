#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// Captured output of a JIT run, split by stream. Severity reports ($info /
// $warning / $error / $fatal and immediate-assertion failures) go to stderr
// (simulator diagnostics, like the $finish banner); $display goes to stdout, so
// a pass/fail action block's own output lands there. Behavior below was
// cross-checked against Icarus Verilog while developing these cases.
struct Captured {
    std::string out;
    std::string err;
};

std::string drain(std::FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string s;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    return s;
}

Captured run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* out_tmp = std::tmpfile();
    std::FILE* err_tmp = std::tmpfile();
    REQUIRE(out_tmp != nullptr);
    REQUIRE(err_tmp != nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(out_tmp), fileno(stdout));
    dup2(fileno(err_tmp), fileno(stderr));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    Captured c{ drain(out_tmp), drain(err_tmp) };
    std::fclose(out_tmp);
    std::fclose(err_tmp);
    return c;
}

// Lower and return the rendered diagnostics; ok must be false (a rejected design).
std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostic_text();
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

size_t count_occ(std::string_view hay, std::string_view needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string_view::npos; p = hay.find(needle, p + needle.size())) {
        n++;
    }
    return n;
}

} // namespace

// -- Severity system tasks (LRM 20.10) --

TEST_CASE("severity tasks render label + formatted message to stderr") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  int x = 5;\n"
                                   "  $info(\"info x=%0d\", x);\n"
                                   "  $warning(\"warn\");\n"
                                   "  $error(\"err x=%0d\", x);\n"
                                   "end endmodule");
    CHECK(contains(c.err, "INFO: "));
    CHECK(contains(c.err, "info x=5"));
    CHECK(contains(c.err, "WARNING: "));
    CHECK(contains(c.err, "warn"));
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "err x=5"));
    // Severity reports never pollute the design's stdout stream.
    CHECK(c.out.empty());
}

TEST_CASE("bare $error with no message prints an empty message and continues") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $error;\n"
                                   "  $display(\"after\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "ERROR: "));
    // The task is non-terminating: the following $display still runs.
    CHECK(contains(c.out, "after"));
}

TEST_CASE("$fatal reports then terminates like $finish") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $fatal(0, \"boom %0d\", 7);\n"
                                   "  $display(\"unreachable\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "FATAL: "));
    CHECK(contains(c.err, "boom 7"));
    // $fatal ends the simulation; the trailing $display never executes.
    CHECK_FALSE(contains(c.out, "unreachable"));
}

TEST_CASE("$fatal finish-number >= 1 also prints the $finish banner") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $fatal(1, \"stop\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "FATAL: "));
    CHECK(contains(c.err, "stop"));
    CHECK(contains(c.err, "$fatal called at"));
}

// -- Immediate assertions (LRM 16.3): assert / assume --

TEST_CASE("immediate assert runs the pass action when true, no report") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (1) $display(\"PASS\"); else $display(\"FAIL\");\n"
                                   "end endmodule");
    CHECK(contains(c.out, "PASS"));
    CHECK_FALSE(contains(c.out, "FAIL"));
    CHECK(c.err.empty());
}

TEST_CASE("immediate assert runs the else action when false") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (0) $display(\"PASS\"); else $error(\"bad\");\n"
                                   "end endmodule");
    CHECK_FALSE(contains(c.out, "PASS"));
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "bad"));
}

TEST_CASE("failing immediate assert with no else emits the default error report") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (0);\n"
                                   "  $display(\"after\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "assertion failed"));
    // The default failure is error severity, non-terminating.
    CHECK(contains(c.out, "after"));
}

TEST_CASE("assume checks like assert: silent when true, reports when false") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assume (1);\n"
                                   "  assume (0);\n"
                                   "end endmodule");
    // Exactly one report — only the false assume fires.
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "assertion failed"));
    CHECK_EQ(c.err.find("ERROR: "), c.err.rfind("ERROR: "));
}

TEST_CASE("assert condition truth follows if-semantics (any bit set is true)") {
    // 2'b10 is nonzero -> true (LRM 12.4 reduction), so the pass action runs.
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (2'b10) $display(\"PASS\"); else $display(\"FAIL\");\n"
                                   "end endmodule");
    CHECK(contains(c.out, "PASS"));
    CHECK(c.err.empty());
}

// -- Guards: unimplemented forms fail loudly (never silently miscompiled) --

// Deferred / final immediate assertions (LRM 16.4) are now supported; see the
// dedicated deferred-assertion section at the end of this file. A passing one in
// an initial block lowers cleanly and reports nothing.
TEST_CASE("deferred and final immediate assertions lower and pass silently") {
    CHECK(run_capture("module m; initial assert #0 (1);\nendmodule").err.empty());
    CHECK(run_capture("module m; initial assert final (1);\nendmodule").err.empty());
}

// -- Immediate cover (LRM 16.3): pass action on true, nothing on false --

TEST_CASE("immediate cover runs the pass action when true") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  cover (1) $display(\"COVERED\");\n"
                                   "end endmodule");
    CHECK(contains(c.out, "COVERED"));
    CHECK(c.err.empty());
}

TEST_CASE("immediate cover does nothing when false: no pass action, no report") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  cover (0) $display(\"COVERED\");\n"
                                   "  $display(\"after\");\n"
                                   "end endmodule");
    CHECK_FALSE(contains(c.out, "COVERED"));
    CHECK(contains(c.out, "after"));
    // cover has no fail action — a false condition emits no default report.
    CHECK(c.err.empty());
}

TEST_CASE("bare immediate cover with no action is a no-op") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  cover (0);\n"
                                   "  cover (1);\n"
                                   "  $display(\"done\");\n"
                                   "end endmodule");
    CHECK(contains(c.out, "done"));
    CHECK(c.err.empty());
}

// -- Deferred immediate assertions (LRM 16.4) --

// A deferred assertion's report matures in the Observed region, but a design
// with no glitch reports it just like a simple immediate assertion. The default
// failure ($error) fires with the "assertion failed" message. $finish sits in a
// separate process at a later instant so the report matures naturally (a $finish
// in the same instant preempts maturation and discards the report — see below).
TEST_CASE("deferred #0 assertion: default failure report matures and fires") {
    const Captured c = run_capture("module m;\n"
                                   "  logic clk = 0; logic [7:0] x = 3;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  always @(posedge clk) a1: assert #0 (x == 0);\n"
                                   "  initial #7 $finish;\n"
                                   "endmodule");
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "assertion failed"));
}

TEST_CASE("deferred #0 assertion: else action with sampled arguments") {
    const Captured c = run_capture("module m;\n"
                                   "  logic clk = 0; logic [7:0] x = 42;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  always @(posedge clk) a1: assert #0 (x == 0) else $error(\"bad x=%0d\", x);\n"
                                   "  initial #7 $finish;\n"
                                   "endmodule");
    CHECK(contains(c.err, "bad x=42"));
}

TEST_CASE("deferred #0 assertion: a same-instant $finish discards the pending report") {
    const Captured c = run_capture("module m;\n"
                                   "  logic clk = 0; logic [7:0] x = 5;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  always @(posedge clk) begin\n"
                                   "    a1: assert #0 (x == 0) else $error(\"discarded x=%0d\", x);\n"
                                   "    $finish;\n"
                                   "  end\n"
                                   "endmodule");
    CHECK_FALSE(contains(c.err, "discarded"));
}

// The point of the construct: a transient combinational failure that the block
// settles away from within the same time step is flushed and never reported,
// while a simple immediate assertion in the same block reports the glitch. The
// `#0` between the two writes forces the always_comb to activate twice at t=10:
// first a=1,b=0 (mismatch), then a=1,b=1 (match). LRM 16.4.2 flush on the second
// activation drops the deferred report; the immediate one already fired.
TEST_CASE("deferred #0 assertion: glitch is flushed, immediate is not") {
    const Captured c = run_capture("module m;\n"
                                   "  logic a = 0, b = 0;\n"
                                   "  always_comb begin\n"
                                   "    chk: assert #0 (a == b) else $error(\"defer a=%0d b=%0d\", a, b);\n"
                                   "    imm: assert (a == b) else $error(\"imm a=%0d b=%0d\", a, b);\n"
                                   "  end\n"
                                   "  initial begin\n"
                                   "    #10 a = 1;\n"
                                   "    #0  b = 1;\n"
                                   "    #10 $finish;\n"
                                   "  end\n"
                                   "endmodule");
    // The glitch really happened: the immediate assertion caught the transient.
    CHECK(contains(c.err, "imm a=1 b=0"));
    // The deferred assertion suppressed it (flushed by the second activation).
    CHECK(count_occ(c.err, "defer a=") == 0);
}

// `assert final` matures in the Postponed region; with no Reactive-region
// re-trigger in Clotho it reports the settled result, like `#0` here.
TEST_CASE("deferred final assertion: report matures in the Postponed region") {
    const Captured c = run_capture("module m;\n"
                                   "  logic clk = 0; logic [7:0] x = 9;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  always @(posedge clk) af: assert final (x == 0) else $error(\"final bad x=%0d\", x);\n"
                                   "  initial #7 $finish;\n"
                                   "endmodule");
    CHECK(contains(c.err, "final bad x=9"));
}

// A passing deferred assertion reports nothing.
TEST_CASE("deferred #0 assertion: a passing check reports nothing") {
    const Captured c = run_capture("module m;\n"
                                   "  logic clk = 0; logic [7:0] x = 0;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  always @(posedge clk) begin\n"
                                   "    a1: assert #0 (x == 0) else $error(\"should not fire\");\n"
                                   "    $finish;\n"
                                   "  end\n"
                                   "endmodule");
    // Only the $finish banner reaches stderr; the passing assertion adds nothing.
    CHECK_FALSE(contains(c.err, "should not fire"));
    CHECK_FALSE(contains(c.err, "ERROR: "));
}

// Each instance of a module with a deferred assertion gets its own per-process
// generation counter (Clotho flattens instances into distinct processes, and
// coroutines are never deduplicated), so the flush is per-instance: all three
// instances report their own failing value, none flushed by another's activation.
TEST_CASE("deferred #0 assertion: per-instance flush across module instances") {
    const Captured c = run_capture("module chk(input logic clk, input logic [7:0] v);\n"
                                   "  always @(posedge clk) a: assert #0 (v == 0) else $error(\"inst v=%0d\", v);\n"
                                   "endmodule\n"
                                   "module top;\n"
                                   "  logic clk = 0; logic [7:0] p = 1, q = 2, r = 3;\n"
                                   "  chk c1(clk, p); chk c2(clk, q); chk c3(clk, r);\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  initial #7 $finish;\n"
                                   "endmodule");
    CHECK(contains(c.err, "inst v=1"));
    CHECK(contains(c.err, "inst v=2"));
    CHECK(contains(c.err, "inst v=3"));
    CHECK_EQ(count_occ(c.err, "inst v="), 3u);
}

// Phase 1 guards: a deferred action that is not a severity report, or a deferred
// assertion inside a function, is rejected loudly.
TEST_CASE("deferred assertion: unsupported action / context rejected") {
    // $display action (stdout, not a severity task) — Phase 2.
    CHECK(contains(reject("module m; initial begin\n"
                          "  a1: assert #0 (0) else $display(\"x\");\n"
                          "end endmodule"),
                   "deferred assertion action"));
    // $fatal action (terminates) — Phase 2.
    CHECK(contains(reject("module m; initial begin\n"
                          "  a1: assert #0 (0) else $fatal(1);\n"
                          "end endmodule"),
                   "deferred assertion action"));
    // Deferred assertion inside a function — Phase 2 (generation threading).
    CHECK(contains(reject("module m;\n"
                          "  function int f(int v);\n"
                          "    p: assert #0 (v > 0);\n"
                          "    return v;\n"
                          "  endfunction\n"
                          "  int y;\n"
                          "  initial y = f(3);\n"
                          "endmodule"),
                   "deferred assertion inside a function"));
}
