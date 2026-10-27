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
    int         status = 0;
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

    const int status = ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    Captured c{ drain(out_tmp), drain(err_tmp), status };
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

// LRM 20.10: every report names the file and line of the call, the hierarchical
// name of the scope it is made in and the simulation time. The test buffer is
// named "source" and a lone top module is named by its own name.
TEST_CASE("severity tasks render label + formatted message to stderr") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  int x = 5;\n"
                                   "  $info(\"info x=%0d\", x);\n"
                                   "  $warning(\"warn\");\n"
                                   "  $error(\"err x=%0d\", x);\n"
                                   "end endmodule");
    CHECK(c.err == "INFO: source:3: m at 0 s: info x=5\n"
                   "WARNING: source:4: m at 0 s: warn\n"
                   "ERROR: source:5: m at 0 s: err x=5\n");
    // Severity reports never pollute the design's stdout stream.
    CHECK(c.out.empty());
}

TEST_CASE("bare $error with no message prints an empty message and continues") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $error;\n"
                                   "  $display(\"after\");\n"
                                   "end endmodule");
    // No message, so no separator left dangling after the time.
    CHECK(c.err == "ERROR: source:2: m at 0 s\n");
    // The task is non-terminating: the following $display still runs.
    CHECK(contains(c.out, "after"));
}

TEST_CASE("a severity report carries the simulation time of the call") {
    const Captured c = run_capture("`timescale 1ns/1ns\n"
                                   "module m;\n"
                                   "  initial begin\n"
                                   "    #10 $warning(\"first\");\n"
                                   "    #15 $error(\"second\");\n"
                                   "  end\n"
                                   "endmodule\n");
    CHECK(c.err == "WARNING: source:4: m at 10 ns: first\n"
                   "ERROR: source:5: m at 25 ns: second\n");
}

TEST_CASE("a severity report names the instance it is made in") {
    const Captured c = run_capture("module leaf;\n"
                                   "  initial $info(\"hello\");\n"
                                   "endmodule\n"
                                   "module top;\n"
                                   "  leaf u1();\n"
                                   "  leaf u2();\n"
                                   "  if (1) begin : g\n"
                                   "    leaf u3();\n"
                                   "  end\n"
                                   "  initial $info(\"from top\");\n"
                                   "endmodule\n");
    CHECK(contains(c.err, "INFO: source:2: top.u1 at 0 s: hello\n"));
    CHECK(contains(c.err, "INFO: source:2: top.u2 at 0 s: hello\n"));
    CHECK(contains(c.err, "INFO: source:2: top.g.u3 at 0 s: hello\n"));
    CHECK(contains(c.err, "INFO: source:10: top at 0 s: from top\n"));
    CHECK_EQ(count_occ(c.err, "INFO: "), 4u);
}

TEST_CASE("a severity report names the task, function or block it is made in") {
    const Captured c = run_capture("module top;\n"
                                   "  task automatic t;\n"
                                   "    $error(\"in task\");\n"
                                   "  endtask\n"
                                   "  function automatic void f;\n"
                                   "    $warning(\"in function\");\n"
                                   "  endfunction\n"
                                   "  initial begin\n"
                                   "    t();\n"
                                   "    f();\n"
                                   "    begin : blk\n"
                                   "      $info(\"in block\");\n"
                                   "    end\n"
                                   "  end\n"
                                   "endmodule\n");
    CHECK(c.err == "ERROR: source:3: top.t at 0 s: in task\n"
                   "WARNING: source:6: top.f at 0 s: in function\n"
                   "INFO: source:12: top.blk at 0 s: in block\n");
}

// A subroutine is lowered once per instance that defines it, so each copy names
// its own instance rather than the one that happened to be lowered first.
TEST_CASE("a subroutine's report names the instance whose copy ran") {
    const Captured c = run_capture("module leaf;\n"
                                   "  task automatic t;\n"
                                   "    $error(\"x\");\n"
                                   "  endtask\n"
                                   "  initial t();\n"
                                   "endmodule\n"
                                   "module top;\n"
                                   "  leaf u1();\n"
                                   "  leaf u2();\n"
                                   "endmodule\n");
    CHECK(contains(c.err, "ERROR: source:3: top.u1.t at 0 s: x\n"));
    CHECK(contains(c.err, "ERROR: source:3: top.u2.t at 0 s: x\n"));
    CHECK_EQ(count_occ(c.err, "ERROR: "), 2u);
}

TEST_CASE("a severity report from a final block carries the time the run ended at") {
    const Captured c = run_capture("`timescale 1ns/1ns\n"
                                   "module m;\n"
                                   "  initial #7 $finish;\n"
                                   "  final $error(\"at the end\");\n"
                                   "endmodule\n");
    CHECK(contains(c.err, "ERROR: source:4: m at 7 ns: at the end\n"));
}

TEST_CASE("$fatal reports then terminates like $finish") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $fatal(0, \"boom %0d\", 7);\n"
                                   "  $display(\"unreachable\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "FATAL: source:2: m at 0 s: boom 7\n"));
    // $fatal ends the simulation; the trailing $display never executes.
    CHECK_FALSE(contains(c.out, "unreachable"));
    // LRM 20.10: it ends it with an error code, which a $finish would not.
    CHECK(c.status == 1);
}

TEST_CASE("$fatal finish-number >= 1 also prints the $finish banner") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $fatal(1, \"stop\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "FATAL: source:2: m at 0 s: stop\n"));
    CHECK(contains(c.err, "$fatal called at"));
    CHECK(c.status == 1);
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
    // The default failure is a $error (LRM 16.3), so it is a report like any other:
    // error severity, naming its scope and the time, and non-terminating.
    CHECK(c.err == "ERROR: source:2: m at 0 s: assertion failed\n");
    CHECK(contains(c.out, "after"));
    CHECK(c.status == 0);
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

// A label names the assertion, and LRM 23.6 makes assertion labels part of a
// hierarchical name, so the report of a labelled assertion is made in
// `<scope>.<label>` whether its failure action is the default or the user's own.
TEST_CASE("a labelled immediate assertion's report names the label") {
    const Captured c = run_capture("`timescale 1ns/1ns\n"
                                   "module top;\n"
                                   "  initial begin\n"
                                   "    a1: assert (0);\n"
                                   "    assert (0);\n"
                                   "    #4 a2: assert (0) else $error(\"custom\");\n"
                                   "  end\n"
                                   "endmodule\n");
    CHECK(c.err == "ERROR: source:4: top.a1 at 0 ns: assertion failed\n"
                   "ERROR: source:5: top at 0 ns: assertion failed\n"
                   "ERROR: source:6: top.a2 at 4 ns: custom\n");
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
    // Stamped with the instant the assertion ran, which is the one it matures in,
    // and made in the scope of its label.
    CHECK(c.err.starts_with("ERROR: source:4: m.a1 at 5 s: assertion failed\n"));
}

// An explicit else action ($error with a message) defers, and its arguments are
// sampled at the point the assertion executes (LRM 16.4.1).
TEST_CASE("deferred #0 assertion: else action with sampled arguments") {
    const Captured c = run_capture("module m;\n"
                                   "  logic clk = 0; logic [7:0] x = 42;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  always @(posedge clk) a1: assert #0 (x == 0) else $error(\"bad x=%0d\", x);\n"
                                   "  initial #7 $finish;\n"
                                   "endmodule");
    CHECK(contains(c.err, "ERROR: source:4: m.a1 at 5 s: bad x=42\n"));
}

// A $finish in the same instant preempts the Observed region, so the un-matured
// deferred report is discarded (LRM 16.4.1 maturation).
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

// -- Concurrent assertions (LRM 16.14): the default failure action is a $error --

// An assertion's report is made in its own scope: the instance, plus the label
// when it has one (LRM 23.6). The failure is reported at the clock tick that
// resolved it.
TEST_CASE("a concurrent assertion's default report names its scope and the time of the failure") {
    const Captured c = run_capture("module top;\n"
                                   "  logic clk = 0, a = 0, b = 0;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  assert property (@(posedge clk) a |=> b);\n"
                                   "  ap: assert property (@(posedge clk) a |=> b);\n"
                                   "  initial begin #12 a = 1; #10 a = 0; #30 $finish; end\n"
                                   "endmodule\n");
    CHECK(contains(c.err, "ERROR: source:4: top at 25 s: assertion failed\n"));
    CHECK(contains(c.err, "ERROR: source:5: top.ap at 25 s: assertion failed\n"));
    CHECK_EQ(count_occ(c.err, "ERROR: "), 2u);
}

TEST_CASE("a concurrent assertion in an instance names the instance") {
    const Captured c = run_capture("module chk(input logic clk, input logic a, input logic b);\n"
                                   "  ap: assert property (@(posedge clk) a |=> b);\n"
                                   "endmodule\n"
                                   "module top;\n"
                                   "  logic clk = 0, a = 0, b = 0;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  chk u(clk, a, b);\n"
                                   "  initial begin #12 a = 1; #10 a = 0; #30 $finish; end\n"
                                   "endmodule\n");
    CHECK(contains(c.err, "ERROR: source:2: top.u.ap at 25 s: assertion failed\n"));
}

TEST_CASE("a failing expect's default report names its scope") {
    const Captured c = run_capture("module top;\n"
                                   "  bit clk = 0, b = 0;\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  initial begin\n"
                                   "    #1;\n"
                                   "    e1: expect (@(posedge clk) ##1 b);\n"
                                   "    $finish;\n"
                                   "  end\n"
                                   "endmodule\n");
    CHECK(contains(c.err, "ERROR: source:6: top.e1 at 15 s: assertion failed\n"));
}

// -- The other reports a design can make through the severity path --

// LRM 15.5.4: a wait_order with no fail statement reports a run-time error, which
// is a $error made where the wait_order is.
TEST_CASE("a failing wait_order's default report names its scope and the time") {
    const Captured c = run_capture("module top;\n"
                                   "  event a, b;\n"
                                   "  initial begin #1 -> b; #1 -> a; end\n"
                                   "  initial wait_order (a, b);\n"
                                   "endmodule\n");
    CHECK(c.err == "ERROR: source:4: top at 1 s: wait_order: events triggered out of order\n");
}
