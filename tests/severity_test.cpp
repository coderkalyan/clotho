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

// The banner is joined when the report is issued, from the instance's path and the
// compile-time text on both sides of it, so nothing about the path may matter: a
// length past a string's 30 inline bytes, a `%` in an escaped name, and a generate
// block, a subroutine and a named block after it.
TEST_CASE("a severity report joins the instance path to the scope text after it") {
    const Captured c = run_capture("module leaf;\n"
                                   "  if (1) begin : g\n"
                                   "    task automatic t;\n"
                                   "      begin : blk\n"
                                   "        $error(\"deep\");\n"
                                   "      end\n"
                                   "    endtask\n"
                                   "    initial t();\n"
                                   "  end\n"
                                   "endmodule\n"
                                   "module top;\n"
                                   "  leaf an_instance_name_well_past_thirty_bytes_long ();\n"
                                   "  leaf \\v%s ();\n"
                                   "endmodule\n");
    CHECK(contains(c.err, "ERROR: source:5: top.an_instance_name_well_past_thirty_bytes_long.g.t.blk at 0 s: deep\n"));
    CHECK(contains(c.err, "ERROR: source:5: top.v%s.g.t.blk at 0 s: deep\n"));
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

// Each instance of a module with a deferred assertion has its own generation
// counter (a field of its record, though the unit's code is shared), so the flush
// is per-instance: all three instances report their own failing value, none
// flushed by another's activation.
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

// The generation counter that drops a pending report is the instance's own: one
// instance disables its procedure and loses its report, its twin (the same code)
// keeps its own.
TEST_CASE("deferred #0 assertion: twin instances flush their own pending reports") {
    const Captured c = run_capture("module chk(input int k);\n"
                                   "  int x;\n"
                                   "  initial begin : whole\n"
                                   "    #1;\n"
                                   "    x = 0;\n"
                                   "    assert #0 (x == 1) else $error(\"k=%0d\", k);\n"
                                   "    if (k == 2) disable whole;\n"
                                   "  end\n"
                                   "endmodule\n"
                                   "module top;\n"
                                   "  chk c1(.k(1)); chk c2(.k(2)); chk c3(.k(3));\n"
                                   "endmodule");
    CHECK(contains(c.err, "k=1"));
    CHECK_FALSE(contains(c.err, "k=2"));
    CHECK(contains(c.err, "k=3"));
}

// A deferred report is queued with the same pieces and rendered at once, so it names
// its own instance and label as an immediate one does.
TEST_CASE("deferred #0 assertion: the report names the instance and the label") {
    const Captured c = run_capture("module chk(input logic clk, input logic [7:0] v);\n"
                                   "  always @(posedge clk) a: assert #0 (v == 0) else $error(\"v=%0d\", v);\n"
                                   "endmodule\n"
                                   "module top;\n"
                                   "  logic clk = 0; logic [7:0] p = 1, q = 2;\n"
                                   "  chk c1(clk, p); chk c2(clk, q);\n"
                                   "  always #5 clk = ~clk;\n"
                                   "  initial #7 $finish;\n"
                                   "endmodule");
    CHECK(contains(c.err, "ERROR: source:2: top.c1.a at 5 s: v=1\n"));
    CHECK(contains(c.err, "ERROR: source:2: top.c2.a at 5 s: v=2\n"));
    CHECK_EQ(count_occ(c.err, "ERROR: "), 2u);
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

// LRM 16.4.4: disabling "a specific deferred assertion" cancels its pending reports. A label is
// a block (9.3.5), and `disable a1;` used to compile as a disable of a block nobody is ever in:
// it did nothing and the reports matured. The reports are a process's queue with no identity per
// assertion to cancel by, so the disable is refused; a block that merely holds an assertion is
// an ordinary target and still works. Expected values are LRM-derived (no oracle implements
// deferred assertions).
TEST_CASE("deferred assertion: disable of an assertion's label is refused") {
    for (const char* spelling : { "a1: assert #0 (x) else $error(\"r\");",
                                  "a1: assert final (x) else $error(\"r\");",
                                  "a1: assume #0 (x) else $error(\"r\");",
                                  "a1: cover #0 (!x) $info(\"r\");" }) {
        CAPTURE(spelling);
        const std::string src = std::string("module t;\n  logic x = 0;\n  initial begin : P\n    ") + spelling +
                                "\n    disable a1;\n  end\nendmodule";
        CHECK(contains(reject(src), "disable of a deferred assertion is not supported yet"));
    }

    // A block that merely holds the assertion is an ordinary disable target, not its label: the
    // disable ends nothing it is not in, and the report, from a scope that is not the process's
    // outermost, matures.
    const Captured held = run_capture(R"(
        module t;
          logic x = 0;
          initial begin : P
            B: begin assert #0 (x) else $error("kept"); end
            disable B;
            $display("after");
          end
        endmodule
    )");
    CHECK(held.out == "after\n");
    CHECK_EQ(count_occ(held.err, "kept"), 1u);
}

// LRM 16.4.2: a process that resumes from an event control or a wait statement has reached a
// flush point, so the reports it queued before it suspended are dropped and never mature. The
// expected values come from the clause; no simulator here implements deferred assertions
// (iverilog rejects them, Verilator reports at the point of execution).
TEST_CASE("deferred assertion: resuming from an event control or a wait statement flushes the report") {
    SUBCASE("an event control") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0; event e;
              initial begin
                assert #0 (x) else $error("stale");
                @(e);
                $display("resumed %0t", $time);
              end
              initial #0 -> e;
            endmodule
        )");
        CHECK(c.out == "resumed 0\n");
        CHECK_FALSE(contains(c.err, "stale"));
    }

    SUBCASE("a wait statement that suspends") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0, go = 0;
              initial begin
                assert #0 (x) else $error("stale");
                wait (go);
                $display("resumed %0t", $time);
              end
              initial #0 go = 1;
            endmodule
        )");
        CHECK(c.out == "resumed 0\n");
        CHECK_FALSE(contains(c.err, "stale"));
    }

    SUBCASE("an event control on an assignment") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0, y = 0; event e;
              initial begin
                assert #0 (x) else $error("stale");
                y = @(e) 1'b1;
                $display("resumed %0t y=%b", $time, y);
              end
              initial #0 -> e;
            endmodule
        )");
        CHECK(c.out == "resumed 0 y=1\n");
        CHECK_FALSE(contains(c.err, "stale"));
    }

    // `always begin @(e); ... end` is `always @(e) ...` by another spelling: the activation
    // that the second trigger starts flushes the first one's report in both.
    SUBCASE("an always body that opens with the event control, resumed twice in one step") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0; event e;
              always begin
                @(e);
                a1: assert #0 (x) else $error("report at %0t", $time);
              end
              initial begin #1 -> e; #0 -> e; end
            endmodule
        )");
        CHECK_EQ(count_occ(c.err, "report at 1"), 1u);
    }

    SUBCASE("an always block with the event control as its sensitivity, the same design") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0; event e;
              always @(e) begin
                a1: assert #0 (x) else $error("report at %0t", $time);
              end
              initial begin #1 -> e; #0 -> e; end
            endmodule
        )");
        CHECK_EQ(count_occ(c.err, "report at 1"), 1u);
    }

    // Each deferred statement the LRM defines shares the one queue, so each is flushed; and
    // without the resume (a `#0` is not one) the same report matures.
    SUBCASE("every deferred statement") {
        struct Kind {
            std::string name;
            std::string stmt;
        };
        const Kind kinds[] = {
            { "assert #0", R"(assert #0 (x) else $error("late report");)" },
            { "assert final", R"(assert final (x) else $error("late report");)" },
            { "assume #0", R"(assume #0 (x) else $error("late report");)" },
            { "assume final", R"(assume final (x) else $error("late report");)" },
            { "cover #0", R"(cover #0 (!x) $info("late report");)" },
            { "cover final", R"(cover final (!x) $info("late report");)" },
        };
        for (const Kind& k : kinds) {
            CAPTURE(k.name);
            const auto with_stmt = [&](std::string text) {
                text.replace(text.find("STMT"), 4, k.stmt);
                return text;
            };
            const Captured flushed = run_capture(with_stmt(R"(
                module t;
                  logic x = 0; event e;
                  initial begin STMT @(e); end
                  initial #0 -> e;
                endmodule
            )"));
            CHECK_EQ(count_occ(flushed.err, "late report"), 0u);

            const Captured kept = run_capture(with_stmt(R"(
                module t;
                  logic x = 0;
                  initial begin STMT #0; end
                endmodule
            )"));
            CHECK_EQ(count_occ(kept.err, "late report"), 1u);
        }
    }
}

// What is not a resume leaves the reports alone: a delay, a wait whose condition is already
// true (the process never suspends), a wake that finds the condition or the `iff` guard still
// false (the process stays suspended through the instant the report matures in), and a
// resume of some other process.
TEST_CASE("deferred assertion: what is not a resume from an event control keeps the pending report") {
    SUBCASE("a #0 delay") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0;
              initial begin
                assert #0 (x) else $error("kept");
                #0;
                $display("after #0 %0t", $time);
              end
            endmodule
        )");
        CHECK(c.out == "after #0 0\n");
        CHECK_EQ(count_occ(c.err, "kept"), 1u);
    }

    SUBCASE("a wait whose condition is already true") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0, go = 1;
              initial begin
                assert #0 (x) else $error("kept");
                wait (go);
                $display("passed %0t", $time);
              end
            endmodule
        )");
        CHECK(c.out == "passed 0\n");
        CHECK_EQ(count_occ(c.err, "kept"), 1u);
    }

    // The wake at time 0 finds `cnt == 2` false, so the process stays suspended and the report
    // matures at time 0; the resume at time 5 finds nothing left to flush.
    SUBCASE("a wait that wakes and finds its condition still false") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0; logic [1:0] cnt = 0;
              initial begin
                assert #0 (x) else $error("kept");
                wait (cnt == 2);
                $display("done %0t", $time);
              end
              initial begin #0 cnt = 1; #5 cnt = 2; end
            endmodule
        )");
        CHECK(c.out == "done 5\n");
        CHECK_EQ(count_occ(c.err, "kept"), 1u);
    }

    SUBCASE("an event control whose iff guard is false") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0, g = 0; event e;
              initial begin
                assert #0 (x) else $error("kept");
                @(e iff g);
                $display("resumed %0t", $time);
              end
              initial begin #0 -> e; #5 g = 1; #1 -> e; end
            endmodule
        )");
        CHECK(c.out == "resumed 6\n");
        CHECK_EQ(count_occ(c.err, "kept"), 1u);
    }

    SUBCASE("a fork child's event control, and a task that only delays") {
        const Captured c = run_capture(R"(
            module t;
              logic x = 0; event e;
              task T; #1 $display("T done %0t", $time); endtask
              initial begin
                assert #0 (x) else $error("kept");
                fork begin @(e); $display("child resumed %0t", $time); end join_none
                #0 -> e;
                T();
              end
            endmodule
        )");
        CHECK(c.out == "child resumed 0\nT done 1\n");
        CHECK_EQ(count_occ(c.err, "kept"), 1u);
    }
}

// A wait inside a task the process calls is a resume of the PROCESS, whose reports the task
// cannot reach, and `wait fork` / `wait_order` are wait statements whose resume the clause does
// not settle: a process holding a deferred assertion is refused these rather than left to
// report what the LRM says is flushed. A virtual call may reach any override.
TEST_CASE("deferred assertion: shapes whose flush points are not reachable are refused") {
    const auto holds = [](std::string_view decls, std::string_view body) {
        return std::string("module t;\n  logic x = 0; event e;\n") + std::string(decls) +
               "\n  initial begin\n    assert #0 (x) else $error(\"r\");\n" + std::string(body) + "\n  end\nendmodule";
    };
    const std::string wait_refusal = "that holds a deferred assertion is not supported yet";

    CHECK(contains(reject(holds("task T; @(e); endtask", "T();")), wait_refusal));
    CHECK(contains(reject(holds("task T; wait (x); endtask", "T();")), wait_refusal));
    CHECK(contains(reject(holds("task T; logic y; y = @(e) 1'b1; endtask", "T();")), wait_refusal));
    CHECK(
      contains(reject(holds("task Inner; @(e); endtask\n  task Outer; Inner(); endtask", "Outer();")), wait_refusal));
    CHECK(
      contains(reject(holds("class C; virtual task v(); #1; endtask endclass\n  C h = new;", "h.v();")), wait_refusal));
    CHECK(contains(reject(holds("", "fork #1; join_none\n    wait fork;")), "wait fork in a process"));
    CHECK(contains(reject(holds("event f;", "wait_order (e, f) $display(\"ok\"); else $display(\"bad\");")),
                   "wait_order in a process"));
}

// A virtual task reached THROUGH another task is as unreachable as a direct virtual call: the
// body read for the callee is the declared method's, and any override may be the one that runs and
// waits, so a virtual task called anywhere along the way counts as waiting, as the direct call is
// refused. Only the direct call was: a non-virtual task that called `h.v()` was accepted, and the
// process printed the report that its resume from the override's event control flushes. A
// virtual FUNCTION cannot wait, a `super.` call binds statically to the base method, and a task
// that only delays does not wait, so those stay accepted and keep their report. LRM-derived; no
// oracle implements deferred assertions.
TEST_CASE("deferred assertion: a virtual task called through another task is refused like a direct one") {
    const auto holds = [](std::string_view decls, std::string_view body) {
        return std::string("module t;\n  logic x = 0; event e;\n") + std::string(decls) +
               "\n  initial begin\n    assert #0 (x) else $error(\"r\");\n" + std::string(body) + "\n  end\nendmodule";
    };
    const std::string wait_refusal = "that holds a deferred assertion is not supported yet";
    const std::string classes      = "class A; virtual task v(); endtask endclass\n"
                                     "  class D extends A; virtual task v(); @(e); endtask endclass\n"
                                     "  D d = new;\n";
    // Accepted: the design runs and its one report matures.
    const auto kept = [&](std::string_view decls, std::string_view body) {
        const Captured c = run_capture(holds(decls, body));
        CHECK_EQ(count_occ(c.err, "ERROR: "), 1u);
    };

    SUBCASE("through a non-virtual task") {
        CHECK(contains(reject(holds(classes + "  task T(A h); h.v(); endtask", "T(d);")), wait_refusal));
    }

    SUBCASE("two tasks down") {
        CHECK(
          contains(reject(holds(classes + "  task T2(A h); h.v(); endtask\n  task T1(A h); T2(h); endtask", "T1(d);")),
                   wait_refusal));
    }

    SUBCASE("through a method that calls it on its own object") {
        CHECK(contains(reject(holds("class B; virtual task v(); endtask task w(); v(); endtask endclass\n"
                                    "  class C extends B; virtual task v(); @(e); endtask endclass\n"
                                    "  C c = new;",
                                    "c.w();")),
                       wait_refusal));
    }

    SUBCASE("a chain of tasks that only delays is not refused") {
        kept("task T2; #1; endtask\n  task T1; T2(); endtask", "T1();");
    }

    SUBCASE("a virtual function, which cannot wait, is not refused") {
        kept("class A; virtual function int f(); return 1; endfunction endclass\n  A a = new;\n"
             "  task T; int y; y = a.f(); endtask",
             "T();");
    }

    SUBCASE("a super call, which binds to the base method, is not refused") {
        kept("class A; virtual task v(); #1; endtask endclass\n"
             "  class D extends A; virtual task v(); super.v(); endtask task w(); super.v(); endtask endclass\n"
             "  D d = new;",
             "d.w();");
    }
}

// The wait walk is transitive over a recursive cycle too, from either member, and the same walk as
// the remote-disable park walk (reaches_property). The wait is in Y after its call back into the
// cycle. Such a design was already refused before the walk was made exact (a refused answer ends
// the compile, so a stale "no" never decided anything), so these pin the walk rather than flip. A
// cycle that waits nowhere is accepted and keeps its report.
TEST_CASE("deferred assertion: the wait walk is exact over a recursive cycle") {
    const auto holds = [](std::string_view decls, std::string_view body) {
        return std::string("module t;\n  logic x = 0; event e;\n") + std::string(decls) +
               "\n  initial begin\n    assert #0 (x) else $error(\"r\");\n" + std::string(body) + "\n  end\nendmodule";
    };
    const std::string wait_refusal = "that holds a deferred assertion is not supported yet";
    const std::string cycle = "task X(int n); Y(n); endtask\n  task Y(int n); if (n > 0) X(n - 1); @(e); endtask";

    CHECK(contains(reject(holds(cycle, "X(1);")), wait_refusal));
    CHECK(contains(reject(holds(cycle, "Y(1);")), wait_refusal));

    const Captured c =
      run_capture(holds("task X(int n); Y(n); endtask\n  task Y(int n); if (n > 0) X(n - 1); endtask", "X(2);"));
    CHECK_EQ(count_occ(c.err, "ERROR: "), 1u);
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
