#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a design and capture stdout (mirrors display_test.cpp / final_test.cpp).
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

// Assert that `source` is rejected by the frontend with a diagnostic containing
// `needle`. Covers both clotho's own guards and the LRM 24.3 rules slang
// enforces before lowering ever sees them.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

// Assert that `source` lowers but the backend refuses it, with `needle` in the
// message it writes to stderr. Used for the two rejections that are properties
// of the emitted schedule rather than of the source.
void check_backend_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));

    ir::run_jit(*r.module);

    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);

    std::rewind(tmp);
    std::string err;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        err.append(buf, n);
    }
    std::fclose(tmp);
    CHECK_MESSAGE(err.find(needle) != std::string::npos, err);
}

} // namespace

// -- Scheduling: the Reactive region ----------------------------------------

TEST_CASE("program initial runs in the Reactive region, after all Active work") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int v = 0;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            $display("MODULE first");
            v = v + 1;
          end
          always @(v) $display("MODULE cascade");
          p pi();
          initial begin repeat(3) @(posedge clk); $finish; end
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            $display("PROGRAM reactive");
          end
        endprogram
    )") == "MODULE first\nMODULE cascade\nPROGRAM reactive\n");
}

// A program initial is a real process, not a zero-time thunk: it may suspend on
// an event control and resume -- and it must resume back into Reactive, not
// into the Active region its waker pushed it from. Two suspensions, so the
// re-entry is exercised more than once. This is tests/sva/diff_program_ref.sv.
TEST_CASE("program initial suspends on an event and resumes in Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int unsigned cyc = 0;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            cyc <= cyc + 1;
            $display("[%0t] MODULE active cyc=%0d", $time, cyc);
          end
          p pi();
          initial begin repeat(3) @(posedge clk); $finish; end
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            $display("[%0t] PROGRAM reactive #1", $time);
            @(posedge t.clk);
            $display("[%0t] PROGRAM reactive #2", $time);
          end
        endprogram
    )") == "[5] MODULE active cyc=0\n[5] PROGRAM reactive #1\n"
           "[15] MODULE active cyc=1\n[15] PROGRAM reactive #2\n");
}

// A nonzero #delay suspends the program initial onto the wheel; the resume
// still lands in Reactive. The module print at 5 and the program print at 7 are
// in different time slots, so this checks the delay works at all, not ordering.
TEST_CASE("program initial may suspend on a nonzero delay") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE", $time);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            #2 $display("[%0t] PROGRAM after delay", $time);
          end
        endprogram
    )") == "[5] MODULE\n[7] PROGRAM after delay\n");
}

// -- Elaboration: a program is a module with different scheduling ------------

// Variables, functions and (non-suspending) tasks declared in a program scope
// all elaborate through the ordinary instance machinery.
TEST_CASE("program scope carries variables, functions and tasks") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          int base = 21;
          function int dbl(int x); return x * 2; endfunction
          task automatic show(); $display("task print %0d", dbl(base)); endtask
          initial begin
            @(posedge t.clk);
            show();
          end
        endprogram
    )") == "task print 42\n");
}

// Two program instances of the same definition are two independent processes,
// each with its own scope, exactly as two module instances would be.
TEST_CASE("multiple program instances each get their own initial") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p #(1) a();
          p #(2) b();
          initial #40 $finish;
        endmodule

        program p #(parameter int ID = 0);
          initial begin
            @(posedge t.clk);
            $display("program %0d", ID);
          end
        endprogram
    )") == "program 1\nprogram 2\n");
}

// -- LRM 24.6: the implicit finish -------------------------------------------

TEST_CASE("simulation ends when the last program initial completes") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int unsigned cyc = 0;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            cyc <= cyc + 1;
            $display("[%0t] MODULE cyc=%0d", $time, cyc);
          end
          p pi();
          initial begin repeat(5) @(posedge clk); $finish; end
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            @(posedge t.clk);
            $display("[%0t] PROGRAM done", $time);
          end
        endprogram
    )") == "[5] MODULE cyc=0\n[15] MODULE cyc=1\n[15] PROGRAM done\n");
}

// The countdown waits for EVERY program initial, not just the first to finish.
// The early one completing must not end the simulation while the late one is
// still waiting for its edge.
TEST_CASE("implicit finish waits for every program initial") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          early e();
          late  l();
          initial #100 $finish;
        endmodule

        program early;
          initial begin @(posedge t.clk); $display("early done"); end
        endprogram

        program late;
          initial begin
            repeat (3) @(posedge t.clk);
            $display("late done");
          end
        endprogram
    )") == "early done\nlate done\n");
}

TEST_CASE("implicit finish does not drain work the last program initial woke") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int fromprog = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("MODULE active");
          always @(fromprog) $display("MODULE woken, val=%0d", fromprog);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            $display("PROGRAM storing");
            t.fromprog = 7;
          end
        endprogram
    )") == "MODULE active\nPROGRAM storing\n");
}

// A design with no program at all must be untouched by any of this: no reactive
// ring, no countdown, and an initial that still runs in the Active region.
TEST_CASE("a design with no program is unaffected") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("MODULE");
          initial begin @(posedge clk); $display("INITIAL"); end
          initial #20 $finish;
        endmodule
    )") == "MODULE\nINITIAL\nMODULE\n");
}

// -- Loud rejections ---------------------------------------------------------
//
// Each of these was accepted and mis-lowered, or would be, without a guard.

// LRM 24.3 forbids an `always` procedure in a program; slang rejects it as a
// member that may not appear there, before lowering sees it.
TEST_CASE("an always procedure in a program is rejected") {
    check_rejected(R"(
        module t; bit clk = 0; p pi(); initial #10 $finish; endmodule
        program p; always @(posedge t.clk) $display("no"); endprogram
    )", "member not allowed in program declaration");
}

TEST_CASE("always_comb in a program is rejected") {
    check_rejected(R"(
        module t; bit clk = 0; p pi(); initial #10 $finish; endmodule
        program p; int x; always_comb x = 1; endprogram
    )", "member not allowed in program declaration");
}

// A program may not be declared inside another program (LRM 24.3).
TEST_CASE("a nested program declaration is rejected") {
    check_rejected(R"(
        module t; bit clk = 0; p pi(); initial #10 $finish; endmodule
        program p;
          program q; initial $display("q"); endprogram
          q qi();
        endprogram
    )", "member not allowed in program declaration");
}

TEST_CASE("a member of an anonymous program is rejected") {
    check_rejected(R"(
        module t; bit clk = 0; initial #10 $finish; endmodule
        program; initial $display("anon"); endprogram
    )", "member not allowed in anonymous program declaration");
}

// TODO: program ports (LRM 24.3). They would elaborate through the ordinary
// port machinery, but a program's ports carry clocking/skew semantics nothing
// here implements, so they are refused rather than given module semantics.
TEST_CASE("a program with ports is rejected") {
    check_rejected(R"(
        module t; bit clk = 0; p pi(.clk(clk)); initial #10 $finish; endmodule
        program p(input bit clk); initial @(posedge clk) $display("port"); endprogram
    )", "a `program` with ports is not supported");
}

// TODO: $exit (LRM 24.5) terminates the calling program. Rejected as an
// unsupported system task until program termination is modelled.
TEST_CASE("$exit is rejected") {
    check_rejected(R"(
        module t; bit clk = 0; p pi(); initial #10 $finish; endmodule
        program p; initial begin $display("a"); $exit; end endprogram
    )", "$exit");
}

// TODO: a #0 inside a program would come back through Inactive rather than
// Re-Inactive, ordering it against other reactive work at the instant
// differently from LRM 4.4.2.7. Not verified, so refused.
TEST_CASE("a #0 inside a program is rejected") {
    check_backend_rejected(R"(
        module t; bit clk = 0; always #5 clk = ~clk; p pi(); initial #40 $finish; endmodule
        program p; initial begin @(posedge t.clk); #0 $display("zero"); end endprogram
    )", "#0 delay inside a `program` block is not supported");
}

// TODO: a task called from a program is a coroutine of its own; the reactive
// re-entry would push the task's handle rather than the calling process's.
// A task that does not suspend is fine and is covered above.
TEST_CASE("a suspending task called from a program is rejected") {
    check_backend_rejected(R"(
        module t; bit clk = 0; always #5 clk = ~clk; p pi(); initial #40 $finish; endmodule
        program p;
          task automatic waiter(); @(posedge t.clk); endtask
          initial begin waiter(); $display("after task"); end
        endprogram
    )", "suspending inside a task called from a `program` block is not supported");
}
