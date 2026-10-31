#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// `program` blocks per LRM §24. A program elaborates exactly like a module --
// same scope, same variables, same subroutines -- and differs in one thing: its
// `initial` procedures run in the Reactive region (§24.3) rather than the
// Active one, so everything they print lands after the Active-region work of
// the same time slot. When the last one completes the simulation ends on its
// own (§24.3, the implicit $finish).
//
// The region is observable only as print ordering, so that is what these check.
//
// Before this existed, `program` was accepted and silently lowered as a plain
// module, which put its initials in the Active region -- a wrong-scheduling bug
// that happened to print plausibly. Every shape not verified here is refused
// loudly rather than given module semantics again.

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

// The observable that proves the region. `cascade` is woken by a write the
// first always block makes, so it joins the Active region AFTER the program was
// already waiting on the same posedge. An Active-region program initial would
// print between the two; a Reactive one prints after both, because Reactive
// runs only once the whole Active/Inactive/NBA set has settled.
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

// -- LRM 24.3: the implicit finish -------------------------------------------

// When the last program initial completes, simulation ends by itself -- the
// module's own $finish at 25 is never reached, and the third clock edge never
// prints.
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

// The LRM does not settle what happens when the last program initial's final
// act is a blocking store that wakes an always block (docs/LRM_NOTES.md, note 35): whether
// that block runs before the implicit $finish. Clotho does not run it, because
// that is what clotho's explicit $finish already does (lower_finish empties the
// rings) and a second, contradictory convention for the same LRM sentence would
// be worse than either answer. Pinned so the choice stays deliberate.
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

// A #0 in a program initial parks in Re-Inactive, not Inactive (LRM 4.4.2.7),
// and the batch promotes back into the Reactive ring of the SAME instant. The
// observable is where the resumed print sorts relative to OTHER reactive work
// of that instant: `a` suspends first but must resume after `b` has run,
// because Re-Inactive promotes only once the reactive ring has drained.
//
// The LRM names the region (4.4.2.7 "an explicit #0 ... shall be scheduled in
// the Re-Inactive region") but says nothing about how two program initials of
// one instant interleave around it. What is pinned here is that the #0 resumes
// in Reactive at the same time as it suspended, and lands behind reactive work
// already queued -- the Re-Inactive signature. The a/b interleave is clotho's
// choice, not something the LRM fixes.
TEST_CASE("a #0 in a program initial resumes in Reactive at the same instant") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE active", $time);
          pa ia();
          pb ib();
          initial #40 $finish;
        endmodule

        program pa;
          initial begin
            @(posedge t.clk);
            $display("[%0t] A before #0", $time);
            #0;
            $display("[%0t] A after #0", $time);
          end
        endprogram

        program pb;
          initial begin
            @(posedge t.clk);
            $display("[%0t] B reactive", $time);
          end
        endprogram
    )") == "[5] MODULE active\n[5] A before #0\n[5] B reactive\n[5] A after #0\n");
}

// A fork inside a program initial. Each child parks on the wheel, and the last
// one to finish resumes the joined parent -- through the join record, which is
// the third wake path (after a signal subscription and a wheel timer) that has
// to name a destination region.
//
// Every reactive print must land after the Active-region output of the slot it
// wakes in, and both wake paths are measured in slots that HAVE Active work: the
// wheel delivers a child at the very top of its slot, before the active drain
// runs, so a child pushed onto the active ring would print first instead of
// last. The two children are given different delays so each lands in its own
// slot, which also pins that `join` waits for the later one -- and keeps the
// test off the wheel's within-bucket firing order, which is not a region fact.
//
// Simulation ends at 25 rather than at the module's `#40 $finish`: the program
// initial completes there, and LRM 24.3 stops the run when the last one does.
TEST_CASE("a fork inside a program initial rejoins in Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE active", $time);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            fork
              begin #10 $display("[%0t] P child 1", $time); end
              begin #20 $display("[%0t] P child 2", $time); end
            join
            $display("[%0t] P after join", $time);
          end
        endprogram
    )") == "[5] MODULE active\n"
           "[15] MODULE active\n[15] P child 1\n"
           "[25] MODULE active\n[25] P child 2\n[25] P after join\n");
}

// This case used to pin a refusal: "suspending inside a task called from a
// `program` block is not supported: the task body is lowered once and cannot
// record two home regions". LRM 24.3.1 puts the region on the THREAD -- "all
// subsequent blocking statements in that thread are scheduled in the Reactive
// region ... a section of sequential code anywhere in the design or testbench
// inherits the scheduling region of the thread that calls it" -- so the premise
// was wrong: a body reached only from program code is reactive-home, and a body
// two kinds of caller reach records no home at all and reads the calling
// thread's. Same source as the refusal test, now run for its ordering.
//
// The program initial calls `waiter` at time 0; the task parks on the design
// clock, and the posedge at 5 must bring it back in REACTIVE -- after the
// module's own Active-region print of the same slot. Simulation ends at 5, when
// the only program initial completes (LRM 24.3), so the module's #40 never fires.
TEST_CASE("a suspending task called from a program resumes in Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE active", $time);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          task automatic waiter(); @(posedge t.clk); endtask
          initial begin waiter(); $display("[%0t] after task", $time); end
        endprogram
    )") == "[5] MODULE active\n[5] after task\n");
}

// -- Reactive-home continuous processes (LRM 24.3.1) -------------------------
//
// A continuous construct declared inside a program belongs to the reactive
// region set, not the active one:
//
//   Statements and constructs within a program block that are sensitive to
//   changes (e.g., update events) on design signals are scheduled in the
//   Reactive region. [...] The continuous assignment assign tclk = clk; would
//   also be scheduled in the Reactive region.
//
// Before this existed, `assign` inside a program was lowered by
// add_continuous_process with no regard for the enclosing program -- module
// semantics, silently, which is the failure mode this file exists to prevent.
// The region is observable only as print ordering against an Active-region
// cascade, so that is what these check.

// The program's own reactive code sees the assign's result within the SAME
// reactive drain: the module's `always` writes `v` in the Active region, which
// wakes the program's assign into Reactive, whose store then wakes the program
// `initial` that is waiting on the mirrored signal. Both prints of a slot are
// therefore ordered MODULE-then-PROGRAM, and the second iteration pins that the
// process RE-ARMS reactive-home rather than only settling that way once.
//
// Simulation ends at 15 rather than at the module's `#40 $finish`: the program
// `initial` completes there and LRM 24.3 stops the run when the last one does.
TEST_CASE("a program's continuous assign runs in Reactive and re-arms there") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int v = 0;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            $display("[%0t] MODULE v=%0d", $time, v);
            v = v + 1;
          end
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          wire [31:0] mirror;
          assign mirror = t.v;
          initial begin
            repeat (2) begin
              @(mirror);
              $display("[%0t] PROGRAM mirror=%0d", $time, mirror);
            end
          end
        endprogram
    )") == "[5] MODULE v=0\n[5] PROGRAM mirror=1\n"
           "[15] MODULE v=1\n[15] PROGRAM mirror=2\n");
}

// LRM 24.3.2's own example, reduced to drop the ports it is written with (those
// are still refused). The clause states the expected schedule outright:
//
//   [...] the assign statement in program p shall not be executed until the
//   Reactive region. And when it executes and triggers activity on the always
//   procedure in module m, that always procedure is not executed until the
//   Active region in the next iteration of the overall scheduling loop.
//
// So `A` (a design reader of a design net), then `R` (the program), then `B` (a
// design reader of the program-driven net). `A, R, B` is fully deterministic
// even though the program's `assign` and the program's `initial` are BOTH in the
// reactive ring and LRM 4.4.2.6 lets those two run in either order: whichever
// goes first, `B` is active-home, so its handle joins the ACTIVE ring and cannot
// be resumed until the entire reactive drain has emptied and the drain loop
// re-settles the active set. Getting this wrong prints `A, B, R`, which is what
// clotho did before the reactive-home classification existed.
//
// posedge rather than any-change sensitivity so the time-zero settle (x -> 0 on
// both nets) wakes nothing and the trace is only the transition at 10; the
// settle has its own case below. The program `initial`'s trailing `#20` keeps a
// program thread alive past the observation, since the implicit finish empties
// both rings and would otherwise drop `B`.
TEST_CASE("a design reader of a program-driven net runs in the next Active") {
    CHECK(run_capture(R"(
        module t;
          logic r = 0;
          wire  dw1, dw2;
          initial #10 r = 1;
          assign dw1 = r;
          always @(posedge dw1) $display("A dw1=%b", dw1);
          always @(posedge dw2) $display("B dw2=%b", dw2);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          assign t.dw2 = t.dw1;
          initial begin @(posedge t.dw1); $display("R reactive"); #20; end
        endprogram
    )") == "A dw1=1\nR reactive\nB dw2=1\n");
}

// The time-zero settle of a reactive-home continuous process happens in slot 0's
// REACTIVE region, not its Active one. Both prints carry time 0, so the settle
// did happen at time 0 and was not deferred a step; and they are ordered, which
// is the region: `dw` is driven by the module's assign in Active, while `pd` is
// driven by the program's and so only changes once the active set has settled,
// putting its design reader in the next Active iteration of the same slot.
TEST_CASE("a program's continuous assign settles at time zero in Reactive") {
    CHECK(run_capture(R"(
        module t;
          logic r = 1;
          wire  dw, pd;
          assign dw = r;
          always @(dw) $display("[%0t] MODULE dw=%b", $time, dw);
          always @(pd) $display("[%0t] MODULE pd=%b", $time, pd);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          assign t.pd = t.dw;
          initial #12 $display("[%0t] PROGRAM done", $time);
        endprogram
    )") == "[0] MODULE dw=1\n[0] MODULE pd=1\n[12] PROGRAM done\n");
}

// The control for all of the above: a MODULE's continuous assign in a design
// that also has a program must keep Active-region semantics. The whole chain
// `r -> m -> the always that reads m` completes in the Active region of slot 5,
// ahead of the program's reactive print. Had the module's assign been classified
// reactive-home too, `m` would change in Reactive and `MODULE m=1` would land
// after `PROGRAM reactive`, in the following Active iteration.
TEST_CASE("a module's continuous assign still runs in Active beside a program") {
    CHECK(run_capture(R"(
        module t;
          bit   clk = 0;
          logic r = 0;
          wire  m;
          always #5 clk = ~clk;
          always @(posedge clk) r = 1;
          assign m = r;
          always @(posedge m) $display("[%0t] MODULE m=%b", $time, m);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          initial begin @(posedge t.clk); $display("[%0t] PROGRAM reactive", $time); #12; end
        endprogram
    )") == "[5] MODULE m=1\n[5] PROGRAM reactive\n");
}

// A program driving a MULTIPLY-DRIVEN net is no longer refused: see the
// "Reactive net resolution" banner at the end of this file.

// -- Per-program termination of descendant threads (LRM 24.3) ----------------
//
// The clause's own two sentences, which are two different granularities:
//
//   When all initial procedures within a program have reached their end, that
//   program shall immediately terminate all descendent threads of initial
//   procedures within that program. If there is at least one initial procedure
//   within at least one program block, the entire simulation shall terminate by
//   means of an implicit call to the $finish system task immediately after all
//   the threads and all their descendent threads originating from all initial
//   procedures within all programs have ended.
//
// The first sentence was missing entirely, and with a SINGLE program the implicit
// $finish hid it: the finish empties the rings, so an orphaned child never got a
// chance to run and the rule appeared to hold. It takes a second program keeping
// the run alive to see the wrong answer, which is why every orphan case below has
// one. The observable is always "the descendant does not print, and the other
// program still runs to its own end".
//
// A killed descendant is a `process::kill` subtree abort (LRM 9.7): it is marked,
// force-woken once and leaves through its own abort exit, so what these pin is
// the absence of its print, not any particular unwinding order.

// The reproduction from docs/design/program-blocks.md §2. Program `a`'s initial
// forks a detached child that would print at 35 and then ends at 5; 24.3 requires
// the child dead at 5. Program `b` keeps the simulation alive to 50, so nothing
// else can mask the result -- before this, "A orphan ran" printed at 35.
TEST_CASE("a program initial's join_none child is terminated when the initial ends") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          a ai();
          b bi();
          initial #100 $finish;
        endmodule

        program a;
          initial begin
            @(posedge t.clk);
            fork
              begin #30 $display("[%0t] A orphan ran", $time); end
            join_none
            $display("[%0t] A initial ends", $time);
          end
        endprogram

        program b;
          initial #50 $display("[%0t] B done", $time);
        endprogram
    )") == "[5] A initial ends\n[50] B done\n");
}

// "all descendent threads", so the relation is transitive: a child's own child
// dies too. Both are genuinely live when the initial ends -- the `#1` lets the
// child run far enough to spawn its grandchild and for both to park on their
// delays -- so this kills two threads at two different depths, neither of which
// is the thread doing the killing.
TEST_CASE("a program initial's descendants are terminated transitively") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          a ai();
          b bi();
          initial #100 $finish;
        endmodule

        program a;
          initial begin
            @(posedge t.clk);
            fork
              begin
                fork
                  begin #30 $display("[%0t] A grandchild ran", $time); end
                join_none
                #20 $display("[%0t] A child ran", $time);
              end
            join_none
            #1;
            $display("[%0t] A initial ends", $time);
          end
        endprogram

        program b;
          initial #60 $display("[%0t] B done", $time);
        endprogram
    )") == "[6] A initial ends\n[60] B done\n");
}

// "When ALL initial procedures within a program have reached their end": one of
// two is not all, so the first initial ending must leave its child running. The
// child prints at 35, between the two initials' ends, which is exactly what a
// per-`initial` (rather than per-program) kill would have destroyed. It also pins
// the per-instance counter reaching zero only at the second end -- the implicit
// $finish happens at 80, so the module's #100 never prints either.
TEST_CASE("a program initial's descendants survive while a sibling initial runs") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #100 $display("[%0t] MODULE late", $time);
          // Backstop: the implicit finish is expected at 80, so this never runs.
          // It is here so a regression in the countdown is a diff rather than a
          // hang -- `always #5 clk` alone never lets the wheel run dry. Two of the
          // cases below deliberately keep no `$finish` at all, to pin that the
          // implicit one gates the drain's finished test (design_has_finish).
          initial #150 $finish;
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            fork
              begin #30 $display("[%0t] child ran", $time); end
            join_none
            $display("[%0t] first ends", $time);
          end
          initial #80 $display("[%0t] second ends", $time);
        endprogram
    )") == "[5] first ends\n[35] child ran\n[80] second ends\n");
}

// Two programs, one instance each, and only ONE of them is finished at 5: `a`'s
// termination must not reach `b`'s descendant. `b`'s child prints at 40, long
// after `a` has ended and terminated its own, and `a`'s orphan never prints at
// all -- so the kill is per program INSTANCE, not design-wide.
TEST_CASE("one program's termination leaves another program's descendants alone") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          a ai();
          b bi();
          initial #100 $finish;
        endmodule

        program a;
          initial begin
            @(posedge t.clk);
            fork
              begin #30 $display("[%0t] A orphan ran", $time); end
            join_none
            $display("[%0t] A initial ends", $time);
          end
        endprogram

        program b;
          initial begin
            @(posedge t.clk);
            fork
              begin #35 $display("[%0t] B child ran", $time); end
            join_none
            #50 $display("[%0t] B initial ends", $time);
          end
        endprogram
    )") == "[5] A initial ends\n[40] B child ran\n[55] B initial ends\n");
}

// An `initial` aborted by `disable` has "reached its end" for 24.3: the clause
// counts procedures that have ended, not ones that ended normally, and an aborted
// one is never going to end any other way. So the countdown must still reach zero
// and the implicit $finish must still happen -- if the aborted procedure were not
// counted, the run would carry on to the module's #100 print instead of stopping
// at 5. `victim` leaves its block on the abort and falls out of the initial, the
// shape tests/loop_test.cpp pins for a remote `disable`.
TEST_CASE("an initial aborted by disable still counts down the program") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE", $time);
          p pi();
          initial #100 $display("[%0t] MODULE late", $time);
        endmodule

        program p;
          initial begin
            begin : victim
              @(posedge t.clk);
              @(posedge t.clk);
              $display("[%0t] victim resumed", $time);
            end
          end
          initial begin
            @(posedge t.clk);
            disable victim;
            $display("[%0t] killer done", $time);
          end
        endprogram
    )") == "[5] MODULE\n[5] killer done\n");
}

// The same for `process::kill` (LRM 9.7), which is the other way an `initial` can
// end without running off its own end -- and the one that leaves through the
// thread's abort exit rather than through a block's. Same requirement: it counts,
// so the run still ends at 5 rather than reaching the module's #100. The handle
// travels through a program-scope variable, since design code may not reference a
// program signal (24.3).
TEST_CASE("an initial killed by process::kill still counts down the program") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE", $time);
          p pi();
          initial #100 $display("[%0t] MODULE late", $time);
        endmodule

        program p;
          process victim_h;
          initial begin
            victim_h = process::self();
            @(posedge t.clk);
            @(posedge t.clk);
            $display("[%0t] victim resumed", $time);
          end
          initial begin
            @(posedge t.clk);
            victim_h.kill();
            $display("[%0t] killer done", $time);
          end
        endprogram
    )") == "[5] MODULE\n[5] killer done\n");
}

// A child the initial WAITED for is not a descendant to terminate: by the time
// `wait fork` returns the subtree is empty (LRM 9.6.1), so the child has already
// run to completion and printed. This is the negative case for the kill -- it
// must not reach a thread that ended on its own -- and it also pins that
// terminating descendants does not disturb the `wait fork` barrier the same
// initial used a moment earlier.
TEST_CASE("a program initial's joined child is unaffected by the termination") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #100 $display("[%0t] MODULE late", $time);
          initial #150 $finish; // backstop, as above
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            fork
              begin #20 $display("[%0t] child done", $time); end
            join_none
            wait fork;
            $display("[%0t] initial ends", $time);
          end
        endprogram
    )") == "[25] child done\n[25] initial ends\n");
}

// LRM 24.3's implicit `$finish` is the ONLY way this design can end: the clock
// `always` never lets the wheel run dry and no body holds a `$finish`. It
// therefore pins design_has_finish -- the drains test the cooperative finish flag
// only when the design can finish at all, and the implicit call is not an
// Inst::Tag::Finish in any body, so a design shaped like this used to run
// forever. Kept deliberately backstop-free; a regression here hangs.
TEST_CASE("the implicit finish ends a design with no explicit $finish") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE", $time);
          p pi();
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            @(posedge t.clk);
            $display("[%0t] PROGRAM done", $time);
          end
        endprogram
    )") == "[5] MODULE\n[15] MODULE\n[15] PROGRAM done\n");
}

// -- $exit (LRM 24.7) --------------------------------------------------------
//
//   Calling $exit from a thread or its descendent thread originating in an
//   initial procedure of a program block shall terminate all initial procedures
//   and their descendent threads within that originating program block. Calling
//   $exit from a thread or its descendent thread that does not originate in an
//   initial procedure in a program shall be ignored, and a warning may be issued
//   to indicate that the call to $exit has been ignored.
//
// So $exit is the 24.3 termination above, requested early and from the inside:
// it ends the program's OWN `initial` procedures as well as their descendants,
// which is what makes it different from `disable fork`. It is emitted as the
// `process::kill` subtree abort over that instance's `initial`s plus its
// termination root, and it does no counting of its own -- every terminated
// `initial` still passes through the program-end path exactly once, so the
// per-program counter cannot be double-decremented and the design-wide implicit
// $finish still fires exactly once. Each case therefore also pins that the run
// ENDS: a leaked counter would show up as the module's late $display appearing.
//
// The calling thread is itself one of the things being terminated, so every case
// checks that the statement after $exit does not print.

// $exit from a program `initial`: its own procedure, its sibling `initial`, and
// its detached child all end at 5. Without the sibling arm this would pass even
// if $exit only ended the calling procedure.
TEST_CASE("$exit from a program initial ends that program's initials and descendants") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial begin #100 $display("[%0t] MODULE late", $time); $finish; end
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            fork
              begin #30 $display("[%0t] child ran", $time); end
            join_none
            $display("[%0t] exiting", $time);
            $exit;
            $display("[%0t] never after exit", $time);
          end
          initial #80 $display("[%0t] sibling ran", $time);
        endprogram
    )") == "[5] exiting\n");
}

// "from a thread or its descendent thread": a fork child of a program `initial`
// is such a descendant, so its $exit ends the whole program -- including the
// `initial` that spawned it, which is still parked on its own delay at 15.
TEST_CASE("$exit from a fork child of a program initial ends the program") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial begin #100 $display("[%0t] MODULE late", $time); $finish; end
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            fork
              begin
                #10 $display("[%0t] child exits", $time);
                $exit;
                $display("[%0t] never after exit", $time);
              end
            join_none
            #40 $display("[%0t] never parent", $time);
          end
        endprogram
    )") == "[15] child exits\n");
}

// A subroutine reachable from exactly ONE program instance's `initial`s can name
// that instance, so this must work rather than hit the ambiguity refusal below.
// It also exercises the other exit path: the task leaves through its epilogue and
// the caller aborts at its own poll after the call, rather than branching
// straight out of the `initial` body.
TEST_CASE("$exit inside a task of the program that calls it ends that program") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial begin #100 $display("[%0t] MODULE late", $time); $finish; end
        endmodule

        program p;
          task automatic bail();
            $display("[%0t] bailing", $time);
            $exit;
            $display("[%0t] never after exit", $time);
          endtask
          initial begin
            @(posedge t.clk);
            bail();
            $display("[%0t] never after call", $time);
          end
        endprogram
    )") == "[5] bailing\n");
}

// 24.7 scopes the termination to "that originating program block", so a second
// program is untouched: `b` keeps running for 45 more time units after `a` has
// exited, and its own end is what finally stops the simulation.
TEST_CASE("$exit in one program leaves another program running") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          a ai();
          b bi();
          initial begin #100 $display("[%0t] MODULE late", $time); $finish; end
        endmodule

        program a;
          initial begin
            @(posedge t.clk);
            fork
              begin #30 $display("[%0t] A orphan ran", $time); end
            join_none
            $display("[%0t] A exits", $time);
            $exit;
          end
        endprogram

        program b;
          initial begin
            #20 $display("[%0t] B tick", $time);
            #30 $display("[%0t] B done", $time);
          end
        endprogram
    )") == "[5] A exits\n[20] B tick\n[50] B done\n");
}

// The second sentence: a thread that does not originate in a program `initial`
// gets nothing at all, not an error -- clotho used to refuse $exit outright,
// which is itself non-compliant. The design thread continues on the next
// statement, and the program's own end still stops the run at 40. The ignored
// call also produces a `program-exit-ignored` warning, which is a diagnostic
// rather than output, so it is not visible here.
TEST_CASE("$exit in design code is ignored and the thread continues") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial begin
            #10 $exit;
            $display("[%0t] MODULE continues", $time);
          end
          initial begin #200 $display("[%0t] MODULE late", $time); $finish; end
        endmodule

        program p;
          initial #40 $display("[%0t] PROGRAM done", $time);
        endprogram
    )") == "[10] MODULE continues\n[40] PROGRAM done\n");
}

// TODO: $exit in a subroutine whose calling thread is not statically known. 24.7
// answers by the thread, and a body lowered once for every caller cannot name
// one: here `bail` is reached from a program `initial` AND from a module
// `initial`, so the same lowered task would have to be both the "terminate that
// program" arm and the "shall be ignored" arm. Needs per-thread program identity
// in the control block. Refused rather than picking an instance.
TEST_CASE("$exit in a subroutine with no single owning program is rejected") {
    check_backend_rejected(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          task automatic bail(); $exit; endtask
          p pi();
          initial begin @(posedge clk); bail(); end
          initial #100 $finish;
        endmodule

        program p;
          initial begin @(posedge t.clk); t.bail(); end
        endprogram
    )",
                           "reachable from more than one program instance");
}

// TODO: $exit inside a function. A function is not a coroutine and has no abort
// exit, and the abort poll that would carry the termination out to its caller is
// emitted only after a suspending op -- a plain call is not one -- so the caller
// would run on past the point 24.7 ends its thread. The shape that would carry
// it is the may-finish closure $finish already uses.
TEST_CASE("$exit inside a function is rejected") {
    check_backend_rejected(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #100 $finish;
        endmodule

        program p;
          function int bail(); $exit; return 1; endfunction
          initial begin @(posedge t.clk); if (bail() == 1) $display("no"); end
        endprogram
    )",
                           "`$exit` inside a function is not supported");
}

// $exit before a sibling `initial` has ever run. Both are seeded onto the reactive
// ring at time zero, and the first to be resumed exits before the second has
// started, so the second is marked while it is still queued at its initial
// suspend -- it has no activation for the mark walk to force-wake. Nothing else in
// the design can produce that state: only $exit names a procedure by its static
// control block. Without the entry test in lower_group the terminated procedure
// would run its whole head segment and print, which is what this pins.
TEST_CASE("$exit terminates a sibling initial that has not started yet") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial begin #100 $display("[%0t] MODULE late", $time); $finish; end
        endmodule

        program p;
          initial begin
            $display("[%0t] first runs", $time);
            $exit;
          end
          initial $display("[%0t] second must not run", $time);
        endprogram
    )") == "[0] first runs\n");
}

// $exit from a `fork ... join` child during the parent `initial`'s FIRST
// activation. The child marks and force-wakes the parent, but the parent is
// RUNNING on the stack rather than parked -- `join` resumes a child inline -- so
// its park record is null and the force-wake falls through to the scheduled flag.
// The seed loop now marks a program `initial` scheduled as it enqueues it, so the
// flag is set and the push is skipped; without that, the parent's handle would be
// queued a second time, and the drain would resume it after the parent had reached
// its final suspend and had its frame destroyed. This case pins the shape rather
// than the mechanism: it must print the line before the fork and nothing after it.
TEST_CASE("$exit from a join child during the parent initial's first activation") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial begin #100 $display("[%0t] MODULE late", $time); $finish; end
        endmodule

        program p;
          initial begin
            $display("[%0t] before fork", $time);
            fork
              begin $exit; end
            join
            $display("[%0t] never after join", $time);
          end
        endprogram
    )") == "[0] before fork\n");
}

// -- Program ports (LRM 24.3, 24.3.2) ---------------------------------------
//
// A program port is an ordinary module port -- LRM 24.3: "Program port
// declaration syntax and semantics are the same as those of modules (see
// 23.2.2)" -- carrying exactly one extra rule, 24.3.2:
//
//   Program ports are program-scope objects. They are always connected to
//   design objects (nets and variables), since programs can only be
//   instantiated in design scopes.
//
//   Sequential code declared in programs always executes in the reactive region
//   set. Thus, variables on the other side of a program port connection are
//   updated in the reactive region set. Similarly, the driving and resolution of
//   nets on the other side of a program port connection also occurs in the
//   reactive region set. [...] Design processes sensitive to those cross-region
//   variables and nets are scheduled for wake up in the active region set.
//
// So a value port's copy process is reactive-home in both directions -- the
// input direction being 24.3.1's own "The continuous assignment assign tclk =
// clk; would also be scheduled in the Reactive region" -- while an
// interface/modport port has no copy process at all (canonicalization) and an
// inout/ref port has none either (both sides collapse onto one signal). There is
// no such thing as port skew; the recorded claim that these needed the clockvar
// machinery was simply wrong.
//
// The region is observable only as print ordering against an Active-region
// cascade, so that is what these check. Every case keeps a late `$finish` in the
// design as a backstop: a program with no `initial` (24.3.2's own example has
// none) triggers no implicit finish, and would otherwise run the free clock
// forever rather than fail.

// LRM 24.3.2's worked example, structure for structure: `module m` with `logic
// r`, `wire dw1, dw2`, `assign dw1 = r`, the positional `p p_i(dw2, dw1)` and the
// `always @(dw2)` reader, against `program p(output pw2, input pw1); assign pw2 =
// pw1;`. The clause states the schedule outright:
//
//   [...] the assign statement in program p shall not be executed until the
//   Reactive region. And when it executes and triggers activity on the always
//   procedure in module m, that always procedure is not executed until the
//   Active region in the next iteration of the overall scheduling loop.
//
// Two deviations from the printed example, both to make the trace an observable
// rather than to change what is tested:
//
//  - `posedge` instead of any-change sensitivity, so the time-zero settle
//    (z -> x -> 0 down the whole chain) wakes nothing and the trace is only the
//    transition at 10;
//  - a program `initial` printing `R`. `R` is what makes the order mean
//    anything: `A` before `dw2` would hold even with an Active-home port copy,
//    since dw1 changes before dw2 either way, but `dw2` after `R` cannot. `R` is
//    a program initial and so unambiguously Reactive, while the `always @(dw2)`
//    is active-home, so its handle joins the ACTIVE ring and cannot be resumed
//    until the whole reactive drain has emptied and the drain loop re-settles the
//    active set. Its trailing `#20` keeps a program thread alive past the
//    observation, since the implicit finish empties both rings.
//
// Three reactive-home processes chain inside one reactive drain here: the input
// copy dw1 -> pw1, the program's own `assign pw2 = pw1`, and the output copy
// pw2 -> dw2.
TEST_CASE("LRM 24.3.2's example: a design reader of a program output port runs in the next Active") {
    CHECK(run_capture(R"(
        module m;
          logic r;
          wire  dw1, dw2;

          initial begin
            r = 0;
            #10 r = 1;
          end

          assign dw1 = r;

          p p_i(dw2, dw1);

          always @(posedge dw1) $display("A dw1=%b", dw1);
          always @(posedge dw2) $display("dw2 is %b", dw2);
          initial #40 $finish;
        endmodule

        program p(output pw2, input pw1);
          assign pw2 = pw1;
          initial begin @(posedge pw1); $display("R reactive"); #20; end
        endprogram
    )") == "A dw1=1\nR reactive\ndw2 is 1\n");
}

// An `input` port a program `initial` waits on. The copy re-arms reactive-home,
// so every edge of the design clock reaches the program's `@(posedge clk)` in
// that slot's Reactive -- ordered against a design cascade woken by the same
// edge, exactly as the port-less case at the top of this file is. Two iterations,
// so what is pinned is the re-arm and not only the time-zero settle.
//
// `bit` throughout, so the time-zero settle stores 0 over the port net's 2-state
// power-on 0 and wakes nothing.
TEST_CASE("a program initial waits on an input port and resumes in Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int v = 0;
          always #5 clk = ~clk;
          always @(posedge clk) begin
            $display("[%0t] DESIGN first", $time);
            v = v + 1;
          end
          always @(v) $display("[%0t] DESIGN cascade", $time);
          p pi(clk);
          initial #40 $finish;
        endmodule

        program p(input bit clk);
          initial begin
            repeat (2) begin
              @(posedge clk);
              $display("[%0t] PROGRAM reactive", $time);
            end
          end
        endprogram
    )") == "[5] DESIGN first\n[5] DESIGN cascade\n[5] PROGRAM reactive\n"
           "[15] DESIGN first\n[15] DESIGN cascade\n[15] PROGRAM reactive\n");
}

// An `output` port a program `initial` drives procedurally. `output bit [7:0] o`
// is a VARIABLE (LRM 23.2.2.3: an output port with a data type and no net type is
// a variable), so a procedure may assign it; the copy wakes on it and stores the
// design net in Reactive, which puts the design reader in the next Active
// iteration of the same slot -- 24.3.2's ordering reached procedurally rather
// than through an `assign`.
//
// The first line is the reactive-home settle: the copy runs in slot 0's Reactive
// and takes the design net from `z` to the variable's power-on 0.
TEST_CASE("a program initial drives an output port and the design reads it") {
    CHECK(run_capture(R"(
        module t;
          bit        clk = 0;
          wire [7:0] dv;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] DESIGN active", $time);
          always @(dv) $display("[%0t] DESIGN dv=%0d", $time, dv);
          p pi(dv);
          initial #40 $finish;
        endmodule

        program p(output bit [7:0] o);
          initial begin @(posedge t.clk); o = 8'd165; #20; end
        endprogram
    )") == "[0] DESIGN dv=0\n[5] DESIGN active\n[5] DESIGN dv=165\n"
           // The program's `#20` keeps it alive to 25, so two further clock edges
           // precede the implicit finish; the observable under test is
           // `DESIGN dv=165` following `DESIGN active` within slot 5.
           "[15] DESIGN active\n[25] DESIGN active\n");
}

// -- Program input ports read settled ----------------------------------------
//
// A value port's copy is a Reactive-region continuous process (24.3.1: "The
// continuous assignment assign tclk = clk; would also be scheduled in the
// Reactive region"), and so is a program's own `assign`. Program code runs in
// Reactive as well, and 4.7 leaves the order of events within one region open,
// so which goes first is Clotho's choice: every queued reactive continuous
// process runs before any program procedural code, so the program always reads
// its inputs settled (docs/design/program-blocks.md §3, docs/LRM_NOTES.md note
// 50). Before that, both kinds shared one FIFO ring, the seed order put a
// program's `initial` ahead of its port copies, and a copy woken by a design
// write queued behind a program thread already resumed in the same slot.
//
// Oracles, measured: Verilator 5.050 prints the expected output of every case
// here. iverilog 13 agrees except in the two cases where the program thread
// resumes in the slot of the design write, where it prints the stale value: it
// does not schedule program code in the Reactive region at all (§2's oracle
// note), so the write and the read race there.

// The reported shape: an `initial` reading an input port at time zero must see
// the design's declaration-initialized 1, not the port's power-on x.
TEST_CASE("a program initial reads an input port settled at time zero") {
    CHECK(run_capture(R"(
        module top;
          logic a = 1;
          prog p(a);
          initial #40 $finish;
        endmodule

        program prog(input logic a);
          initial $display("t0 a=%b", a);
        endprogram
    )") == "t0 a=1\n");
}

// The design writes the port's source in slot 5's Active region, which wakes
// the copy; the program's `#5` resumes in the same slot's Reactive region
// (24.3.1: "The standard # delay operator within program blocks schedules
// process resumption in the Reactive region"), already queued when the copy
// wakes. The copy still runs first.
TEST_CASE("a program thread resuming in the slot of a design write reads the port settled") {
    CHECK(run_capture(R"(
        module top;
          logic a = 1;
          initial #5 a = 0;
          prog p(a);
          initial #40 $finish;
        endmodule

        program prog(input logic a);
          initial #5 $display("[%0t] a=%b", $time, a);
        endprogram
    )") == "[5] a=0\n");
}

// A program waiting on its own input port. The port settles before the
// `initial` reaches its `@(a)`, so the wait sees no time-zero x -> 1 -- that
// would be the port catching up with a value the design had all along -- and
// wakes on the design's change at 5.
TEST_CASE("a program waiter on an input port wakes on the design change, not the settle") {
    CHECK(run_capture(R"(
        module top;
          logic a = 1;
          initial #5 a = 0;
          prog p(a);
          initial #40 $finish;
        endmodule

        program prog(input logic a);
          initial begin
            @(a);
            $display("[%0t] a=%b", $time, a);
          end
        endprogram
    )") == "[5] a=0\n");
}

// A program's own `assign` chained behind the port copy: both are reactive
// continuous processes, so the chain settles before the `initial` reads, at
// time zero and again in slot 5 after the design write.
TEST_CASE("a program initial reads its own assign of an input port settled") {
    CHECK(run_capture(R"(
        module top;
          logic a = 1;
          initial #5 a = 0;
          prog p(a);
          initial #40 $finish;
        endmodule

        program prog(input logic a);
          logic t;
          assign t = a;
          initial begin
            $display("[%0t] t=%b", $time, t);
            #5 $display("[%0t] t=%b", $time, t);
          end
        endprogram
    )") == "[0] t=1\n[5] t=0\n");
}

// The drain order holds for the whole Reactive region, not only at its start. One
// program thread triggers an event, which queues another program's reading
// thread, and then writes the variable that port copies: the copy is queued
// BEHIND the reader, and still runs before it. The reader being queued first is
// what the event buys, independent of which timer the wheel fires first.
TEST_CASE("a port copy woken behind a queued program thread still settles before it reads") {
    CHECK(run_capture(R"(
        module top;
          logic d = 0;
          event go;
          prog1 p1();
          prog2 p2(d);
          initial #40 $finish;
        endmodule

        program prog1;
          initial begin
            #5;
            -> top.go;
            top.d = 1;
          end
        endprogram

        program prog2(input logic a);
          initial begin
            @(top.go);
            $display("[%0t] a=%b", $time, a);
          end
        endprogram
    )") == "[5] a=1\n");
}

// The output direction is unchanged: its copy is a reactive continuous process
// too, so the design reader of the port still runs in the Active region of the
// next iteration (24.3.2), after the program code that wrote the port.
TEST_CASE("a program output port still reaches its design reader in the next Active") {
    CHECK(run_capture(R"(
        module top;
          logic [7:0] d;
          always @(d) $display("[%0t] DESIGN d=%0d", $time, d);
          prog p(d);
          initial #40 $finish;
        endmodule

        program prog(output logic [7:0] o);
          initial begin
            o = 1;
            $display("[%0t] PROGRAM wrote %0d", $time, o);
            #5 o = 2;
            $display("[%0t] PROGRAM wrote %0d", $time, o);
            #10;
          end
        endprogram
    )") == "[0] PROGRAM wrote 1\n[0] DESIGN d=1\n[5] PROGRAM wrote 2\n[5] DESIGN d=2\n");
}

// An interface port -- `program test ( interface device_ifc );` is LRM 24.3's own
// second example. No copy process exists: canonical_value_symbol resolves the
// program's `b.data` and the module's onto the interface instance's one signal
// (docs/design/interfaces.md), so the program's Reactive code reads the live
// design value exactly as it would through a hierarchical name, and there is no
// region question to answer. The design's cascade off `b.vld` is Active-region
// work of slot 5 and therefore precedes the program's read.
TEST_CASE("a program reads an interface port") {
    CHECK(run_capture(R"(
        interface bus_if;
          logic [7:0] data;
          logic       vld;
        endinterface

        module t;
          bit    clk = 0;
          bus_if b();
          always #5 clk = ~clk;
          always @(posedge clk) begin b.data = 8'd9; b.vld = 1'b1; end
          always @(b.vld) $display("[%0t] DESIGN vld=%b", $time, b.vld);
          p pi(b);
          initial #40 $finish;
        endmodule

        program p(bus_if b);
          initial begin @(posedge t.clk); $display("[%0t] PROGRAM data=%0d", $time, b.data); end
        endprogram
    )") == "[5] DESIGN vld=1\n[5] PROGRAM data=9\n");
}

// A modport port (LRM 25.5): the same canonicalization one indirection further --
// `b.data` in the program resolves through the modport onto the same interface
// signal -- so again no copy process and no region question. Input-only through
// the modport, which is what a testbench modport is for.
TEST_CASE("a program reads a modport port") {
    CHECK(run_capture(R"(
        interface bus_if;
          logic [7:0] data;
          modport tb (input data);
        endinterface

        module t;
          bit    clk = 0;
          bus_if b();
          always #5 clk = ~clk;
          always @(posedge clk) begin
            b.data = 8'd21;
            $display("[%0t] DESIGN wrote", $time);
          end
          p pi(b);
          initial #40 $finish;
        endmodule

        program p(bus_if.tb b);
          initial begin @(posedge t.clk); $display("[%0t] PROGRAM data=%0d", $time, b.data); end
        endprogram
    )") == "[5] DESIGN wrote\n[5] PROGRAM data=21\n");
}

// The canonical testbench shape, and the one docs/design/clocking-blocks.md said
// was blocked on this work: a clocking block inside a program sampling an `input`
// port. The clockvar is ordinary Clause 14 machinery reading an ordinary
// program-scope signal; the port contributes nothing of its own to it.
//
// The clocking EVENT is a design signal by hierarchical reference rather than a
// second input port, which is what LRM 24.4 asks for -- "Programs that read
// design values exclusively through clocking blocks with clocks that are design
// signals are insensitive to read-write races" -- and a program port is not a
// design signal (24.3: "Variables declared within the scope of a program,
// including variables declared as ports, are called program variables"). A
// port-clocked block also exposes a scheduler gap that predates and outlives this
// phase: sampling runs in Observed, which precedes Reactive, so a clock only
// updated by the reactive-home port copy is not seen until a later pass through
// Observed. Recorded in docs/design/program-blocks.md §4.
//
// `d` is driven a full step before each edge, so the sample is the same value
// whether the block reads the pre-edge shadow (the #1step default) or the live
// signal, and the observable is only that the port's value reached the clockvar.
TEST_CASE("a clocking block in a program samples an input port") {
    CHECK(run_capture(R"(
        module t;
          bit       clk = 0;
          bit [7:0] d   = 0;
          always #5 clk = ~clk;
          initial begin #2 d = 8'd6; #10 d = 8'd7; end
          p pi(d);
          initial #40 $finish;
        endmodule

        program p(input bit [7:0] d);
          clocking cb @(posedge t.clk); input d; endclocking
          initial begin
            repeat (2) begin
              @(cb);
              $display("[%0t] PROGRAM cb.d=%0d", $time, cb.d);
            end
          end
        endprogram
    )") == "[5] PROGRAM cb.d=6\n[15] PROGRAM cb.d=7\n");
}

// An `inout` port the program drives and the design reads. build_inout_aliases
// collapses the two sides onto ONE signal, so there is no copy process at all:
// the program's reactive-home `assign` writes the shared net directly, which is
// 24.3.2's sentence with nothing in between, and the design reader is woken into
// Active by the ordinary wake path. Ordered against a design print of the same
// slot to show the reader is a region behind; the first line is the reactive-home
// settle at time zero.
TEST_CASE("a program drives an inout port and the design reads it") {
    CHECK(run_capture(R"(
        module t;
          bit        clk = 0;
          wire [7:0] data;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] DESIGN active", $time);
          always @(data) $display("[%0t] DESIGN data=%0d", $time, data);
          p pi(data);
          initial #40 $finish;
        endmodule

        program p(inout wire [7:0] data);
          bit [7:0] val = 0;
          assign data = val;
          initial begin @(posedge t.clk); val = 8'd7; #20; end
        endprogram
    )") == "[0] DESIGN data=0\n[5] DESIGN active\n[5] DESIGN data=7\n"
           // The program's `#20` keeps it alive to 25, so two more clock edges
           // land before the implicit finish. Nothing to do with the port; the
           // observable under test is `DESIGN data=7` following `DESIGN active`
           // in slot 5.
           "[15] DESIGN active\n[25] DESIGN active\n");
}

// LRM 24.3's own first example, declared verbatim: `program test (input clk,
// input [16:1] addr, inout [7:0] data);`. Three directions in one header -- two
// reactive-home input copies and one collapsed inout -- read together from the
// program's Reactive code at a design clock edge. The program is woken by its own
// clk copy, so it cannot observe a half-copied header: `addr` last changed two
// slots earlier and `data` is the shared net itself.
TEST_CASE("LRM 24.3's declaration shape lowers and reads in Reactive") {
    CHECK(run_capture(R"(
        module m;
          bit          clk = 0;
          logic [16:1] a = 0;
          logic  [7:0] d = 0;
          wire   [7:0] data;
          always #5 clk = ~clk;
          assign data = d;
          initial begin #2 a = 16'd300; d = 8'd44; end
          test ti(clk, a, data);
          initial #40 $finish;
        endmodule

        program test (input clk, input [16:1] addr, inout [7:0] data);
          initial begin
            @(posedge clk);
            $display("[%0t] addr=%0d data=%0d", $time, addr, data);
          end
        endprogram
    )") == "[5] addr=300 data=44\n");
}

// An explicitly unconnected port (LRM 23.3.2.7). No copy process is emitted in
// either direction, so the program's internal net keeps the undriven default --
// `z` for a 4-state net (LRM 6.6). This is also why a TOP-LEVEL program with
// ports needs no special case of its own: a top instance has no port connections
// at all, which is the same "no copy process" outcome by a different route.
//
// The port kind is omitted, which is the whole point: LRM 23.2.2.3 says "For
// input and inout ports, the port shall default to a net of default net type", so
// `input logic [7:0] d` is a net and reads `z`. slang classified such a port as a
// variable -- reading `x` -- until patches/slang-input-port-net-kind.patch, which
// this case pins from the clotho side (docs/design/program-blocks.md §11).
TEST_CASE("an unconnected program port keeps its undriven default") {
    CHECK(run_capture(R"(
        module t;
          p pi(.d());
          initial #20 $finish;
        endmodule

        program p(input logic [7:0] d);
          initial $display("d=%b", d);
        endprogram
    )") == "d=zzzzzzzz\n");
}

// A `.*` connection (LRM 23.3.2.4), matching `clk` and `q` by name in the parent.
// This and the two cases below are the remaining PortConnection shapes slang
// builds differently; all of them were refused before this phase, and none of
// them needs anything from wire_ports beyond what the positional and named forms
// above already exercise.
TEST_CASE("a program instantiated with .* connects by name") {
    CHECK(run_capture(R"(
        module t;
          bit       clk = 0;
          bit [7:0] q;
          always #5 clk = ~clk;
          always @(q) $display("[%0t] DESIGN q=%0d", $time, q);
          p pi(.*);
          initial #40 $finish;
        endmodule

        program p(input bit clk, output bit [7:0] q);
          initial begin @(posedge clk); q = 8'd3; #20; end
        endprogram
    )") == "[5] DESIGN q=3\n");
}

// A non-ANSI port list -- `program_nonansi_header` in 24.3's own syntax, where
// the port declaration is a separate program_item.
TEST_CASE("a program with a non-ANSI port list connects") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi(clk);
          initial #40 $finish;
        endmodule

        program p(pclk);
          input pclk;
          initial begin @(posedge pclk); $display("[%0t] PROGRAM edge", $time); end
        endprogram
    )") == "[5] PROGRAM edge\n");
}

// A `bind`-inserted program with a port (LRM 23.11, which admits a
// program_instantiation). The bound instance is an ordinary member of the target
// scope by the time lower_members walks it, so the port takes the same
// reactive-home copy; what this pins is that the connection expression resolves
// in the BIND TARGET's scope rather than at the bind directive's own.
TEST_CASE("a bind-inserted program with a port connects") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] DESIGN active", $time);
          initial #40 $finish;
        endmodule

        program p(input bit c);
          initial begin @(posedge c); $display("[%0t] PROGRAM bound", $time); end
        endprogram

        bind t p pi(clk);
    )") == "[5] DESIGN active\n[5] PROGRAM bound\n");
}

// Width coercion across a program port, both directions in one case: the design's
// 8-bit `src` narrows into a 4-bit input port (200 -> 8) and the 4-bit output port
// widens into the design's 8-bit `snk`. Nothing about the coercion is
// program-specific -- it is the same `coerce` the module path uses -- but it is
// the only thing a port copy does besides choosing a region, so it is pinned here
// too.
//
// The program waits on the PORT rather than on the design clock, so it is woken
// by the copy and cannot observe the pre-copy value: the copy and the program
// initial are both reactive-ring work of the same slot and the LRM fixes no order
// between them.
TEST_CASE("a program port connection coerces width in both directions") {
    CHECK(run_capture(R"(
        module t;
          bit       clk = 0;
          bit [7:0] src = 0;
          bit [7:0] snk;
          always #5 clk = ~clk;
          always @(posedge clk) src = 8'd200;
          always @(snk) $display("[%0t] DESIGN snk=%0d", $time, snk);
          p pi(src, snk);
          initial #40 $finish;
        endmodule

        program p(input bit [3:0] in4, output [3:0] out4);
          assign out4 = in4;
          initial begin @(in4); $display("[%0t] PROGRAM in4=%0d", $time, in4); #20; end
        endprogram
    )") == "[5] PROGRAM in4=8\n[5] DESIGN snk=8\n");
}

// Both multiply-driven port shapes are now implemented, not refused: see the
// "Reactive net resolution" banner at the end of this file.

// A container port reaches the pre-existing refusal through a program exactly as
// through a module -- the copy would have to deep-copy a handle on every change
// of the source and detect that change through a heap buffer. Nothing about it is
// program-specific, so the message names no construct.
TEST_CASE("a container program port is rejected") {
    check_rejected(R"(
        module t;
          int fifo[$];
          p pi(fifo);
          initial #10 $finish;
        endmodule
        program p(input int fifo[$]); initial $display("no"); endprogram
    )",
                   "a dynamic-array, queue or associative-array port connection is not supported");
}

// build_inout_aliases needs a single canonical net to collapse onto and a
// bit-select has none. Refused identically for a module (xhier_test.cpp), so this
// message needs no program wording either.
TEST_CASE("a non-whole-signal inout program port connection is rejected") {
    check_rejected(R"(
        module t;
          wire [1:0] w;
          p pi(w[0]);
          initial #10 $finish;
        endmodule
        program p(inout wire b); initial $display("no"); endprogram
    )",
                   "inout/ref port with a non-whole-signal connection is not supported");
}

// -- The home region belongs to the thread (LRM 24.3.1, 24.5) ----------------
//
// 24.3.1:
//
//   Once a program process starts a thread of execution, all subsequent blocking
//   statements in that thread are scheduled in the Reactive region. This includes
//   subroutine code called by the thread, even if the subroutine code is declared
//   in a module, package, or interface. Effectively, a section of sequential code
//   anywhere in the design or testbench inherits the scheduling region of the
//   thread that calls it.
//
// 24.5 says the same thing about the shape it is easiest to hit: "When a task
// within a design module is called from a program, it shall use the reactive
// region set for its scheduling activities."
//
// So the region is NOT a property of the body. A body only program code reaches
// folds Reactive, a body only ordinary code reaches folds Active, and a body BOTH
// reach carries its caller's answer in its coroutine frame
// (build_shared_home_fns / home_ptr in ir/llvm/Scheduler.cpp). Every case below is an
// ordering observable, since that is the only way the region shows.

// 24.5's own example, as close to the clause's text as clotho's tests get:
//
//   module ...
//     task T;
//      S1: a = b;     // executes in reactive region set if called from a program
//      #5;
//      S2: b <= 1'b1; // executes in reactive region set if called from a program
//     endtask
//   endmodule
//
// The task is declared in the module and called hierarchically from the program,
// which is legal by 24.5's own first paragraph ("Programs are allowed to call
// subroutines in other programs or within design modules"). Its `#10` matures on
// the clock posedge at 15, where the module also has Active-region work, so the
// print order is the region: MODULE first, the task's resume second.
//
// The task is reached from program code ONLY here, so its home is folded Reactive
// at lowering time -- no ramp parameter, no load. The next case is the shared one.
TEST_CASE("a design module's suspending task called from a program uses Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE active", $time);
          task automatic T(); #10; $display("[%0t] TASK resumed", $time); endtask
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          initial begin
            @(posedge t.clk);
            t.T();
            $display("[%0t] P after task", $time);
          end
        endprogram
    )") == "[5] MODULE active\n"
           "[15] MODULE active\n[15] TASK resumed\n[15] P after task\n");
}

// The whole point of the change: ONE task, called from a program `initial` and
// from a module `initial` in the same design, each caller keeping its own region.
// This is what the retired backend refusal ("the body is lowered once and can
// record only one home region") rejected; the body is still lowered once, and now
// records no home at all -- it reads the calling thread's from a ramp parameter.
//
// Both callers park a `#5`, so both are inside `bump` when it parks its own `#10`,
// and both of those timers mature in slot 15. The wheel delivers them at the top
// of that slot into DIFFERENT rings -- id=1's onto the active ring, id=2's onto
// the reactive one -- so the drain order is the observable and it does not depend
// on the wheel's within-bucket order, which is not a region fact.
//
// `v = 1` after the module caller returns is the Active-region cascade: an
// `always @(v)` woken from Active runs while the active set is still settling,
// i.e. strictly before the reactive drain. So the program's half of the output
// must come after it.
TEST_CASE("one suspending task called from both a program and a module keeps two regions") {
    CHECK(run_capture(R"(
        module t;
          int v = 0;
          always @(v) $display("[%0t] MODULE cascade v=%0d", $time, v);
          task automatic bump(input int id);
            #10;
            $display("[%0t] bump resumed id=%0d", $time, id);
          endtask
          p pi();
          initial begin #5; bump(1); v = 1; end
          initial #100 $finish;
        endmodule

        program p;
          initial begin #5; t.bump(2); $display("[%0t] P after bump", $time); end
        endprogram
    )") == "[15] bump resumed id=1\n"
           "[15] MODULE cascade v=1\n"
           "[15] bump resumed id=2\n[15] P after bump\n");
}

// Transitive inheritance, which is what "all SUBSEQUENT blocking statements in
// that thread" requires: `outer` does not suspend, `inner` does, and both are
// shared. `inner`'s region therefore cannot come from either declaration -- it
// comes from `outer`'s frame, which came from `outer`'s caller. Each call site
// passes the home it is currently running under rather than its lexical one
// (callee_home in ir/llvm/Scheduler.cpp), so this holds at any depth.
//
// Same slot-15 arrangement and same Active cascade as the case above.
TEST_CASE("a task called from a program passes its region to the task it calls") {
    CHECK(run_capture(R"(
        module t;
          int v = 0;
          always @(v) $display("[%0t] MODULE cascade v=%0d", $time, v);
          task automatic inner(input int id);
            #10;
            $display("[%0t] inner resumed id=%0d", $time, id);
          endtask
          task automatic outer(input int id);
            inner(id);
            $display("[%0t] outer done id=%0d", $time, id);
          endtask
          p pi();
          initial begin #5; outer(1); v = 1; end
          initial #100 $finish;
        endmodule

        program p;
          initial begin #5; t.outer(2); end
        endprogram
    )") == "[15] inner resumed id=1\n[15] outer done id=1\n"
           "[15] MODULE cascade v=1\n"
           "[15] inner resumed id=2\n[15] outer done id=2\n");
}

// The UVM-shaped case, and the one that blocks real testbenches: a class METHOD
// that waits on a design clock, called from a program `initial`. A method is
// lowered as an ordinary subroutine body, so it answers to the same
// classification as a task -- reached from program code only, hence reactive-home
// -- and the wait's parked handle carries that region.
//
// The method also does a `#0` after its edge, which is the reactive region set's
// OTHER destination: a #0 in reactive code parks in the Re-Inactive batch (LRM
// 4.4.2.7), not the Inactive queue, and comes back in the same slot's Reactive
// after the ring drains. `pb` proves it: it prints in slot 5's Reactive too, and
// must land between the method's two prints.
TEST_CASE("a class method waiting on a design clock runs in Reactive from a program") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          always @(posedge clk) $display("[%0t] MODULE active", $time);
          pa ia();
          pb ib();
          initial #40 $finish;
        endmodule

        program pa;
          class Driver;
            task automatic wait_edge();
              @(posedge t.clk);
              $display("[%0t] METHOD edge", $time);
              #0;
              $display("[%0t] METHOD after zero", $time);
            endtask
          endclass
          initial begin
            Driver d = new;
            d.wait_edge();
          end
        endprogram

        program pb;
          initial begin @(posedge t.clk); $display("[%0t] PB reactive", $time); end
        endprogram
    )") == "[5] MODULE active\n"
           "[5] METHOD edge\n[5] PB reactive\n[5] METHOD after zero\n");
}

// A concurrent assertion's action block beside a shared task, in one design. The
// action block is reactive code with a home of its own, and it is stated at the
// PEND site rather than inherited: LRM 4.4.2.5 hands the block to the Reactive
// region of the slot whatever region decided the verdict, so lower_reactive_pend
// ramps it with a Reactive home even though the Observed process that pends it is
// active-home.
//
// This is the case the retired `reject_shared_reactive_suspend` was written for,
// approached from the reachable side. A suspending task cannot be called from an
// action block at all -- the frontend refuses it because it cannot trace the body
// (see tests/sva_test.cpp, "a task call in an assertion action block"), which is
// why that backend refusal was never reachable in practice. What IS reachable,
// and what this pins, is the two mechanisms coexisting: the shared task's ramp
// parameter is present design-wide, and the action block's ramp gets the constant
// Reactive rather than a load of the pending thread's home.
//
// Two slots, each pinning one half. In slot 5 the assertion fails (its Preponed
// sample of `v` is still 1, since the module only clears it at 6), so the action
// block runs in that slot's Reactive -- after the module initial's Active print.
// In slot 15 the two `bump` activations mature together, and the wheel delivers
// them into different rings, so the active one drains first. The clock's own
// toggler is deliberately not printed from: it is a wheel entry too, and its
// order against a `bump` timer inside one bucket is not a region fact.
//
// Simulation ends at 15, when the program's only initial completes, so the
// assertion never reaches the posedge at 25.
TEST_CASE("an assertion action block and a shared suspending task coexist") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int v = 1;
          always #5 clk = ~clk;
          ap: assert property (@(posedge clk) v == 0)
              else $display("[%0t] ACTION block", $time);
          task automatic bump(input int id);
            #10;
            $display("[%0t] bump resumed id=%0d", $time, id);
          endtask
          p pi();
          initial #6 v = 0;
          initial begin #5 $display("[%0t] MODULE initial", $time); bump(1); end
          initial #100 $finish;
        endmodule

        program p;
          initial begin #5; t.bump(2); end
        endprogram
    )") == "[5] MODULE initial\n"
           "[5] ACTION block\n"
           "[15] bump resumed id=1\n"
           "[15] bump resumed id=2\n");
}

// The other side of the classification, and the regression this most needs: a
// task NO program reaches keeps folding the Active home. `modonly` is called only
// from the module `initial`, so nothing about it changes -- and because no body in
// this design is shared, the ramp parameter is not emitted at all, which is the
// "identical signatures for a design with no shared body" property.
//
// Slot 15 holds both the module task's #10 and the program initial's #15, so the
// active ring draining before the reactive one is the observable.
TEST_CASE("a task called only from module code still suspends into Active") {
    CHECK(run_capture(R"(
        module t;
          int v = 0;
          always @(v) $display("[%0t] MODULE cascade v=%0d", $time, v);
          task automatic modonly(); #10; $display("[%0t] MOD task resumed", $time); endtask
          p pi();
          initial begin #5; modonly(); v = 1; end
          initial #100 $finish;
        endmodule

        program p;
          initial begin
            #15 $display("[%0t] PROGRAM reactive", $time);
            #10 $display("[%0t] PROGRAM done", $time);
          end
        endprogram
    )") == "[15] MOD task resumed\n"
           "[15] MODULE cascade v=1\n"
           "[15] PROGRAM reactive\n"
           "[25] PROGRAM done\n");
}

// -- A clocking event on a program signal (LRM 24.3.1) -----------------------
//
// LRM 24.3 fixes the population by declaring scope:
//
//   Variables declared within the scope of a program, including variables
//   declared as ports, are called program variables. [...] Program variables
//   and nets are collectively termed program signals.
//
// and 24.3.1 says what clocking an Observed-region body off one would require:
//
//   If an assertion is clocked by activity on a program object (not
//   recommended), the scheduler will iterate from the reactive region set back
//   around the outer loop in Figure 4-1, through the Observed region, where the
//   assertion is evaluated.
//
// Clotho's loop does not close that way -- `reactive.done` re-settles to the
// active drain or falls through to Postponed, never back to `wheel.check` -- so
// a clock written only in the reactive region set is not seen until the NEXT
// slot's Observed pass. That was a SILENT one-slot-late answer, and it is now
// two loud refusals. Both fixes and the termination argument a real one needs
// are recorded on `program_clock_signal` and in
// docs/design/program-blocks.md §10.
//
// The cases below deliberately do not need a clock reader anywhere in the
// Active region: any Active reader's wake leaves the active ring non-empty at
// `reactive.done`, control returns to the active drain, and Observed is then
// re-entered by accident -- which is why this never surfaced.

// A program PORT is a program signal, not a design signal, however the design
// drives the other side of it. This is the shape closest to LRM 24.3's own
// `program test (input clk, ...)` example, which is why it is first: the port
// copy is a reactive-home continuous process (24.3.1's `assign tclk = clk;`
// sentence), so `pclk` changes in Reactive, one region after the Observed pass
// that would have to sample it.
TEST_CASE("a clocking block in a program clocked by a program port is rejected") {
    check_rejected(R"(
        module t;
          bit         clk = 0;
          logic [7:0] d   = 0;
          always #5 clk = ~clk;
          p pi(clk, d);
          initial #40 $finish;
        endmodule

        program p(input bit pclk, input logic [7:0] pd);
          clocking cb @(posedge pclk); input pd; endclocking
          initial begin @(cb); $display("unreachable"); end
        endprogram
    )",
                   "a clocking block clocked by the program signal");
}

// The same defect without any port: a program-local variable the program's own
// Reactive code writes. This is the route that pre-dates the port work, and the
// needle here pins the REGION half of the message the case above pins the
// construct half of.
TEST_CASE("a clocking block in a program clocked by a program variable is rejected") {
    check_rejected(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          bit tclk = 0;
          int v    = 0;
          clocking cb @(posedge tclk); input v; endclocking
          initial begin
            @(posedge t.clk) tclk = 1;
            @(cb) $display("unreachable");
          end
        endprogram
    )",
                   "the block samples in the Observed region");
}

// The assertion half, and the shape 24.3.1's sentence is literally about. A
// monitor is an Observed body too, so it is the same one-slot-late answer -- and
// the refusal is separate because the fix is not: a monitor detects its clock
// edge from the Preponed shadow, which does not change within a step, so it
// cannot survive a second Observed pass of the same slot the way a clocking
// block can.
TEST_CASE("an assertion in a program clocked by a program port is rejected") {
    check_rejected(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi(clk);
          initial #40 $finish;
        endmodule

        program p(input bit pclk);
          int v = 0;
          ap: assert property (@(posedge pclk) v == 0);
          initial begin @(posedge t.clk); #20; end
        endprogram
    )",
                   "a concurrent assertion clocked by the program signal");
}

// And the assertion on a program-local clock, needling the region half.
TEST_CASE("an assertion in a program clocked by a program variable is rejected") {
    check_rejected(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          bit tclk = 0;
          int v    = 0;
          ap: assert property (@(posedge tclk) v == 0);
          initial begin @(posedge t.clk) tclk = 1; #20; end
        endprogram
    )",
                   "the assertion is evaluated in the Observed region");
}

// What stays legal, and the whole point of the refusals: a DESIGN clock. LRM
// 24.4 asks for exactly this independently of any of the above -- "Programs that
// read design values exclusively through clocking blocks with clocks that are
// design signals are insensitive to read-write races" -- and 24.3 makes it the
// dual of a program signal: "Any net or variable declared within a module,
// interface, package, or $unit is considered to be a design signal."
//
// The BOOLEAN is a program signal (`pv`), which is untouched by the refusal:
// only the clock decides the region, and a sampled value is read from the
// Preponed shadow of the PREVIOUS slot whatever region wrote it.
//
// Derived instants. `clk` posedges at 5, 15, 25. Preponed of slot 5 shadows
// `pv` = 1, so `pv == 0` is false at the 5 edge and the action block -- handed
// to Reactive by LRM 4.4.2.5 -- prints there. The program sets `pv = 0` at 6, so
// Preponed of slot 15 shadows 0 and the 15 and 25 edges pass silently. The
// initial's LAST statement is the display at 26, not a delay, so it ends at 26
// and LRM 24.3's implicit $finish fires there -- the module's #100 backstop is
// never reached.
TEST_CASE("an assertion in a program clocked by a hierarchical design signal runs") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #100 $finish;
        endmodule

        program p;
          int pv = 1;
          ap: assert property (@(posedge t.clk) pv == 0)
              else $display("[%0t] ACTION block", $time);
          initial begin
            #6  pv = 0;
            #20 $display("[%0t] PROGRAM done", $time);
          end
        endprogram
    )") == "[5] ACTION block\n"
           "[26] PROGRAM done\n");
}

// `default clocking` inside a program, and `##n` against it, on a design clock.
// Both go through the same clocking_event / clocking_state path the refusal now
// sits in, so this is the case that catches an over-refusal there; nothing else
// in the suite pins a `default clocking` declared in a program scope.
//
// The block carries no clocking item, which LRM 14.3's `{ clocking_item }` admits
// and which is all `##n` needs -- it waits on the block's event (LRM 14.11), not
// on a clockvar. Written as the inline `default clocking` declaration rather than
// a `clocking` block plus a separate `default clocking cb;`, because the standalone
// form is a member kind clotho's module-member walk has no case for and would
// refuse -- an unrelated gap, and not one this case is here to measure.
//
// Derived instants, and they are not derived blind: this is
// `tests/clocking_test.cpp`'s "a cycle delay waits for that many clocking events"
// moved from a module into a program, with the same clock and the same two delays.
// Clocking events at each `t.clk` posedge: 5, 15, 25. `##1` from time 0 resumes at
// the first, 5. `##2` from there resumes at the third, 25 -- the event already
// consumed at 5 does not count toward it. 25 is also where the only initial ends,
// so LRM 24.3's implicit $finish fires there and the module's #100 is never
// reached. Only the instants are observable here, not the region, since nothing
// else prints in either slot.
TEST_CASE("a default clocking in a program drives ##n from a design clock") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          always #5 clk = ~clk;
          p pi();
          initial #100 $finish;
        endmodule

        program p;
          default clocking cb @(posedge t.clk); endclocking
          initial begin
            ##1 $display("[%0t] after one cycle", $time);
            ##2 $display("[%0t] after two more", $time);
          end
        endprogram
    )") == "[5] after one cycle\n"
           "[25] after two more\n");
}

// -- Reactive net resolution (LRM 24.3.2, 6.6) -------------------------------
//
// 24.3.2:
//
//   Similarly, the driving and resolution of nets on the other side of a program
//   port connection also occurs in the reactive region set. Such driving and
//   resolution occurs immediately after an event causes a change to a driver on a
//   program net. Design processes sensitive to those cross-region variables and
//   nets are scheduled for wake up in the active region set.
//
// A multiply-driven net is folded from its per-driver slots by ONE resolver
// process (emit_wire_resolvers), so it has exactly one resolution region. When
// any driver of the net is program-side, that resolver is reactive-home -- which
// necessarily moves the net's DESIGN-side drivers' resolution into Reactive too.
// The clause does not distinguish which driver changed, and per-driver resolution
// regions (two resolvers folding one net) are not a thing any sentence of the LRM
// asks for. docs/design/program-blocks.md §4 A5 records the decision.
//
// The observable is print order, as everywhere else in this file: a design reader
// of a reactively-resolved net cannot run until the next Active iteration, so a
// program's reactive print beats it. The last case is the control that pins the
// opposite order for a net with no program driver, and it is what makes the rest
// mean anything.

// A design net driven by BOTH a program output port and a design continuous
// assign. The program drives z, so LRM 6.6's Table 6-2 gives the design driver's
// value ("z" row: z vs 1 -> 1) and `w` simply follows `r` -- the fold is not what
// this case measures, the REGION of the fold is.
//
// At t=5 the active region set does `clk` -> `r = 1` -> the design driver's slot
// store. The resolver does not run there: it is reactive-home because `pi`'s
// output port is one of the net's drivers. So `w` changes in Reactive, alongside
// the program's own print, and `always @(posedge w)` -- a DESIGN process -- is
// woken into the Active region of the NEXT iteration of the scheduling loop,
// which is 24.3.2's last sentence exactly.
TEST_CASE("a program output port makes a resolved design net resolve in Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit   clk = 0;
          logic r = 0;
          wire  w;
          always #5 clk = ~clk;
          always @(posedge clk) r = 1;
          assign w = r;
          p pi(w);
          always @(posedge w) $display("[%0t] DESIGN w=%b", $time, w);
          initial #40 $finish;
        endmodule

        program p(output o);
          assign o = 1'bz;
          initial begin @(posedge t.clk); $display("[%0t] PROGRAM mark", $time); #20; end
        endprogram
    )") == "[5] PROGRAM mark\n[5] DESIGN w=1\n");
}

// The same net read by the program's OWN reactive code, which is the half of
// 24.3.2 a design-side reader cannot see: the program waits on the resolved net
// `t.w` and must observe the new value in the reactive region set of slot 5, not
// a region or a slot later. Had the resolver stayed Active-home, the fold would
// land in the next Active iteration and this wait would resume in the Reactive
// region AFTER it -- i.e. the design reader would print first, which is the
// staleness the refusal this replaced was protecting against.
//
// `#20` after the print, so the implicit $finish (24.3) cannot end the run before
// the design reader gets its next Active iteration.
TEST_CASE("a program reads its own reactively-resolved net without a region of lag") {
    CHECK(run_capture(R"(
        module t;
          bit   clk = 0;
          logic r = 0;
          wire  w;
          always #5 clk = ~clk;
          always @(posedge clk) r = 1;
          assign w = r;
          p pi(w);
          always @(posedge w) $display("[%0t] DESIGN w=%b", $time, w);
          initial #40 $finish;
        endmodule

        program p(output o);
          assign o = 1'bz;
          initial begin @(posedge t.w); $display("[%0t] PROGRAM w=%b", $time, t.w); #20; end
        endprogram
    )") == "[5] PROGRAM w=1\n[5] DESIGN w=1\n");
}

// The same thing reached without any port at all: a program's hierarchical
// `assign` to a design net that the design also drives. This is the shape phase
// A2 refused, and it is the reason A5 is not a port question -- 24.3.1 puts "the
// continuous assignment assign tclk = clk;" in the Reactive region whether or not
// a port is involved.
TEST_CASE("a program's hierarchical assign to a multiply-driven net resolves in Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit   clk = 0;
          logic r = 0;
          wire  w;
          always #5 clk = ~clk;
          always @(posedge clk) r = 1;
          assign w = r;
          always @(posedge w) $display("[%0t] DESIGN w=%b", $time, w);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          assign t.w = 1'bz;
          initial begin @(posedge t.clk); $display("[%0t] PROGRAM mark", $time); #20; end
        endprogram
    )") == "[5] PROGRAM mark\n[5] DESIGN w=1\n");
}

// The mirror on the program side of the boundary: a program INPUT port whose net
// has a second driver. The second driver can only come from another program --
// LRM 24.3 makes a design reference to a program signal an error but explicitly
// allows "hierarchical references to extend from one program scope to another
// program scope" -- so `q` is what makes `pi.pw` multiply-driven. Both of its
// drivers are program-side (the input copy, and `q`'s assign), so nothing about
// the design's schedule moves here.
//
// The whole chain after the design's Active print is reactive: the input copy
// stores its slot, the resolver folds `pw`, and `p`'s wait resumes on it -- all
// before the slot's next active region set. `q` has no `initial`, so it
// contributes nothing to the implicit $finish (24.3, "if there is at least one
// initial procedure"); `p`'s ends at 25 and ends the run.
TEST_CASE("a program input port net with a second program driver resolves in Reactive") {
    CHECK(run_capture(R"(
        module t;
          bit  clk = 0;
          wire d;
          always #5 clk = ~clk;
          assign d = clk;
          always @(posedge d) $display("[%0t] DESIGN d=%b", $time, d);
          p pi(d);
          q qi();
          initial #40 $finish;
        endmodule

        program p(input pw);
          initial begin @(posedge pw); $display("[%0t] PROGRAM pw=%b", $time, pw); #20; end
        endprogram

        program q;
          assign t.pi.pw = 1'bz;
        endprogram
    )") == "[5] DESIGN d=1\n[5] PROGRAM pw=1\n"
           // `p`'s initial ends on the `#20`, so the run lives to 25 and the
           // design print repeats at 15 and 25. The observable under test is the
           // slot-5 pair: the design's Active print, then the program's read of a
           // net folded by a reactive-home resolver.
           "[15] DESIGN d=1\n[25] DESIGN d=1\n");
}

// The region moved; the FOLD did not. LRM 6.6.1: "Logical conflicts from multiple
// sources of the same strength on a wire or a tri net result in x (unknown)
// values", and Table 6-2's z row resolves z against 1 to 1. Both nets here are
// driven by one design assign and one program output port, so both resolve
// through the reactive-home resolver, and both must still give the answer the
// table gives:
//
//   wx: design 1 vs program 0 -> x   (Table 6-2, row 1 column 0)
//   wz: design z vs program 1 -> 1   (Table 6-2, row z column 1)
//
// The `#1` puts the read after slot 0, whose reactive region set is where a
// reactive-home resolver performs its time-zero settle. The initial then ends, so
// the implicit $finish stops the run at 1 and the module's #40 never fires.
TEST_CASE("reactive resolution still folds by LRM 6.6") {
    CHECK(run_capture(R"(
        module t;
          wire wx;
          wire wz;
          assign wx = 1'b1;
          assign wz = 1'bz;
          p pi(wx, wz);
          initial #40 $finish;
        endmodule

        program p(output ox, output oz);
          assign ox = 1'b0;
          assign oz = 1'b1;
          initial begin #1; $display("[%0t] wx=%b wz=%b", $time, t.wx, t.wz); end
        endprogram
    )") == "[1] wx=x wz=1\n");
}

// The control, and the case that gives all of the above their meaning: a resolved
// net with NO program driver keeps its Active-home resolver even in a design that
// contains a program. Structurally identical to the hierarchical-assign case
// above except that the second driver is a MODULE assign rather than a program
// one, and the order is the exact opposite: `w` folds inside the active region
// set of slot 5, so its design reader prints there, ahead of the program's
// reactive mark. Were program_driven_nets_ over-approximating -- marking every
// resolved net in a design that happens to contain a program -- this case would
// print in the other order.
TEST_CASE("a resolved net with no program driver still resolves in Active") {
    CHECK(run_capture(R"(
        module t;
          bit   clk = 0;
          logic r = 0;
          wire  w;
          always #5 clk = ~clk;
          always @(posedge clk) r = 1;
          assign w = r;
          assign w = 1'bz;
          always @(posedge w) $display("[%0t] DESIGN w=%b", $time, w);
          p pi();
          initial #40 $finish;
        endmodule

        program p;
          initial begin @(posedge t.clk); $display("[%0t] PROGRAM mark", $time); #20; end
        endprogram
    )") == "[5] DESIGN w=1\n[5] PROGRAM mark\n");
}
