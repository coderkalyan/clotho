#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

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

void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

} // namespace

// The `process` class (LRM 9.7). A thread's control block is native storage -- a
// static slot per process, a pooled record per fork child -- and a `process` handle
// is an ordinary class handle onto it. See docs/design/process-class.md.
//
// ORACLE. iverilog 13 cannot parse the class at all (`process p;` is a syntax
// error), so it scores nothing here. Verilator 5.050 implements self / status /
// await / kill and object identity, and those cases were cross-checked against it;
// it does NOT implement suspend / resume (an explicit $error) and does not
// re-parent a finished thread's children, so those cases are pinned from the LRM
// text and say so per case.

// -- process::self() and identity (LRM 9.7) -----------------------------------

// Verilator-scored: `process::self()` twice in one thread compares equal. Clotho
// memoizes one object per control block to get this, because a class-handle `==` is
// a bare pointer compare (docs/design/process-class.md §5.1).
TEST_CASE("process: self() is stable within a thread") {
    const std::string out = run_capture(R"(
        module t;
          process a, b;
          initial begin
            a = process::self();
            b = process::self();
            $display("same=%0d null=%0d", a == b, a == null);
          end
        endmodule
    )");
    CHECK(out == "same=1 null=0\n");
}

// Two different threads get two different handles: identity names the THREAD.
TEST_CASE("process: self() differs between threads") {
    const std::string out = run_capture(R"(
        module t;
          process p1, p2;
          initial begin
            fork
              begin p1 = process::self(); end
              begin p2 = process::self(); end
            join
            $display("distinct=%0d", p1 != p2);
          end
        endmodule
    )");
    CHECK(out == "distinct=1\n");
}

// -- process::status() (LRM 9.7) ----------------------------------------------

// Verilator-scored. RUNNING for the calling thread, WAITING for one parked in a
// blocking statement, FINISHED once it has completed. The state values are slang's
// synthesized enum: FINISHED=0, RUNNING=1, WAITING=2, SUSPENDED=3, KILLED=4.
TEST_CASE("process: status reports RUNNING, WAITING and FINISHED") {
    const std::string out = run_capture(R"(
        module t;
          process me, child;
          initial begin
            me = process::self();
            $display("self=%0d", me.status());
            fork
              begin child = process::self(); #10; end
            join_none
            #1 $display("parked=%0d", child.status());
            #20 $display("done=%0d", child.status());
          end
        endmodule
    )");
    CHECK(out == "self=1\nparked=2\ndone=0\n");
}

// A status enum prints through the ordinary enum-name path: slang's synthesized
// `state` is an ordinary int enum to clotho.
TEST_CASE("process: status().name() renders the enum member") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            process me = process::self();
            $display("%s", me.status().name());
          end
        endmodule
    )");
    // A static-lifetime declaration initializes in the PreInit process (LRM 6.21),
    // so `me` names that thread, which has finished by the time the body runs.
    CHECK(out == "FINISHED\n");
}

// -- process::await() (LRM 9.7) ----------------------------------------------

// Verilator-scored: await blocks until the target completes, and the target's side
// effects are visible when it returns.
TEST_CASE("process: await blocks until the target completes") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          int n = 0;
          initial begin
            fork
              begin child = process::self(); #100 n = 7; end
            join_none
            #1 child.await();
            $display("t=%0t n=%0d status=%0d", $time, n, child.status());
          end
        endmodule
    )");
    CHECK(out == "t=100 n=7 status=0\n");
}

// Awaiting a thread that has already completed returns in the same instant: the
// wait is a loop around a status test, not an unconditional park.
TEST_CASE("process: await on a finished thread returns at once") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin child = process::self(); #5; end
            join
            child.await();
            $display("t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "t=5\n");
}

// Several threads awaiting one target are all woken by its completion: the waiter
// list is unbounded (frame-resident nodes), unlike a static wait bucket.
TEST_CASE("process: two threads await one target") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin child = process::self(); #20; end
            join_none
            #1;
            fork
              begin child.await(); $display("A at %0t", $time); end
              begin child.await(); $display("B at %0t", $time); end
            join
            $display("both done at %0t", $time);
          end
        endmodule
    )");
    // Wake ORDER among threads released by one completion is unspecified (LRM 4.7
    // leaves the order of processes woken by one event arbitrary); the waiter list is
    // push-front, so clotho releases them in reverse link order. Assert the set and
    // the instant, not the order.
    CHECK(out.find("A at 20\n") != std::string::npos);
    CHECK(out.find("B at 20\n") != std::string::npos);
    CHECK(out.find("both done at 20\n") != std::string::npos);
}

// A task runs in its caller's thread (LRM 13.3), so an await inside one parks that
// thread and `process::self()` inside it names the caller, not the activation.
TEST_CASE("process: await and self inside a task name the calling thread") {
    const std::string out = run_capture(R"(
        module t;
          process child, seen;
          task automatic waiter(process p);
            seen = process::self();
            p.await();
          endtask
          initial begin
            process me = null;
            me = process::self();
            fork
              begin child = process::self(); #30; end
            join_none
            #1 waiter(child);
            $display("t=%0t caller=%0d", $time, seen == me);
          end
        endmodule
    )");
    CHECK(out == "t=30 caller=1\n");
}

// -- Refusals (the phases not yet implemented) --------------------------------

// Every method of LRM 9.7 is implemented, so what is pinned here is the shape of the
// surface rather than a gap: a `process` handle behaves like any other class handle,
// and the still-unimplemented std classes keep their own refusal
// (tests/class_test.cpp).
TEST_CASE("process: a handle is an ordinary class handle") {
    const std::string out = run_capture(R"(
        module t;
          class Holder;
            process p;
          endclass
          initial begin
            Holder h = new;
            $display("null=%0d", h.p == null);
            h.p = process::self();
            $display("set=%0d same=%0d", h.p != null, h.p == process::self());
          end
        endmodule
    )");
    CHECK(out == "null=1\nset=1 same=1\n");
}

// `process` is abstract in slang, so a user `new` is rejected before clotho sees it
// -- objects exist because threads do.
TEST_CASE("process: cannot be constructed with new") {
    check_rejected("module m; initial begin process p; p = new; end endmodule",
                   "cannot create instance of virtual class 'process'");
}

// -- process::kill() (LRM 9.7) and `disable fork` (LRM 9.6.3) ------------------

// Verilator-scored (5.050 prints `status=4 ticks=3` / `after ticks=3` for this
// exact source): a killed thread stops at once and reads KILLED immediately, before
// it has unwound -- the state is set where `kill` runs, not where the thread notices.
TEST_CASE("process: kill stops a thread and reports KILLED at once") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          int ticks = 0;
          initial begin
            fork
              begin child = process::self(); forever begin #10; ticks++; end end
            join_none
            #35 child.kill();
            $display("status=%0d ticks=%0d", child.status(), ticks);
            #100 $display("after ticks=%0d", ticks);
          end
        endmodule
    )");
    CHECK(out == "status=4 ticks=3\nafter ticks=3\n");
}

// LRM-pinned: 9.7 kills "the given process and all its sub-processes", and 9.6.3
// spells out that descendants include those of children that already terminated.
// Verilator cannot score this -- its control block detaches a finished thread's
// children instead of re-parenting them -- so the expectation is the LRM text.
TEST_CASE("process: kill reaches a whole subtree") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin
                child = process::self();
                fork
                  begin #200 $display("grandchild ran"); end
                join_none
                #400 $display("child ran");
              end
            join_none
            #35 child.kill();
            #500 $display("end t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "end t=535\n");
}

// A thread parked inside a time-consuming task call is parked in an INNER frame, and
// the whole chain has to unwind: the innermost activation aborts at its poll, wakes
// its caller, and the caller aborts at its own. This is the case remote `disable`
// still refuses (`hard_park`), and it is why the control block carries an activation
// stack rather than one occupant slot.
TEST_CASE("process: kill unwinds a thread parked inside a task call") {
    const std::string out = run_capture(R"(
        module t;
          process deep;
          task automatic slow();
            #50 $display("task returned");
          endtask
          initial begin
            fork
              begin deep = process::self(); slow(); $display("body after task"); end
            join_none
            #5 deep.kill();
            #100 $display("end t=%0t status=%0d", $time, deep.status());
          end
        endmodule
    )");
    CHECK(out == "end t=105 status=4\n");
}

// Killing a parent that is parked at a `join` with a live child: the join record
// outlives the parent's frame (it is heap and reference-counted once a design has
// process control), so the child's epilogue cannot write through a freed frame.
// Under ASan this is the case that catches the ownership change being wrong.
TEST_CASE("process: kill a parent parked at a join") {
    const std::string out = run_capture(R"(
        module t;
          process parent;
          initial begin
            fork
              begin
                parent = process::self();
                fork
                  begin #500 $display("join child ran"); end
                join
                $display("parent past join");
              end
            join_none
            #10 parent.kill();
            #600 $display("end t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "end t=610\n");
}

// Killing the calling thread ends it where the call is: nothing else would ever
// resume it to notice the mark, so the abort is taken directly.
TEST_CASE("process: kill of self ends the caller") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            process me = null;
            me = process::self();
            $display("before");
            me.kill();
            $display("after - unreachable");
          end
          initial #10 $display("other thread still runs");
        endmodule
    )");
    CHECK(out == "before\nother thread still runs\n");
}

// A kill of a thread that has already completed has no effect (the state is already
// terminal), and a second kill of a killed thread is likewise a no-op.
TEST_CASE("process: kill of a completed thread is a no-op") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin child = process::self(); #5; end
            join
            child.kill();
            child.kill();
            $display("status=%0d t=%0t", child.status(), $time);
          end
        endmodule
    )");
    CHECK(out == "status=0 t=5\n");
}

// `disable fork` (LRM 9.6.3) ends every descendant of the calling thread and leaves
// the caller running -- the same walk as `kill`, with the root spared. LRM-pinned.
TEST_CASE("process: disable fork ends the descendants only") {
    const std::string out = run_capture(R"(
        module t;
          int n = 0, m = 0;
          initial begin
            fork
              begin forever begin #10 n++; end end
              begin forever begin #10 m++; end end
            join_none
            #35 disable fork;
            $display("at kill n=%0d m=%0d", n, m);
            #50 $display("later n=%0d m=%0d caller alive", n, m);
          end
        endmodule
    )");
    CHECK(out == "at kill n=3 m=3\nlater n=3 m=3 caller alive\n");
}

// `await` on a killed thread returns: KILLED is a completion as far as 9.7's await
// is concerned, which is also what keeps an awaiting thread from hanging forever.
TEST_CASE("process: await returns when the target is killed") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin child = process::self(); #1000; end
            join_none
            fork
              begin #20 child.kill(); end
              begin #1 child.await(); $display("await returned at %0t status=%0d", $time, child.status()); end
            join
          end
        endmodule
    )");
    CHECK(out == "await returned at 20 status=4\n");
}

// -- process::suspend() / resume() (LRM 9.7) -----------------------------------

// LRM-pinned: Verilator 5.050 refuses `suspend()` outright ($error), so it is no
// oracle for any of these. The rule being encoded is 9.7's own sentence about
// resume: "Calling resume on a process that was suspended while blocked on another
// condition shall re-sensitize the process ... If the wait condition is now true or
// the original delay has transpired, the process is scheduled onto the Active or
// Reactive region, so as to continue its execution in the current time step."
//
// So: a thread suspended while parked on a #20 does NOT run at 20, and the resume
// at 45 continues it AT 45.
TEST_CASE("process: a suspended thread does not run, and resume continues it now") {
    const std::string out = run_capture(R"(
        module t;
          process w;
          int n = 0;
          initial begin
            fork
              begin w = process::self(); #20 n = 1; $display("worker at %0t", $time); end
            join_none
            #5  w.suspend();
            #40 $display("t=%0t n=%0d status=%0d", $time, n, w.status());
            w.resume();
            #1  $display("t=%0t n=%0d status=%0d", $time, n, w.status());
          end
        endmodule
    )");
    CHECK(out == "t=45 n=0 status=3\nworker at 45\nt=46 n=1 status=0\n");
}

// A self-suspend "takes place immediately" and the thread resumes at the statement
// after the call, which is what makes `suspend` a function rather than a task here.
TEST_CASE("process: a thread can suspend itself and be resumed by another") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            process me = null;
            me = process::self();
            fork
              begin #10 me.resume(); end
            join_none
            $display("before t=%0t", $time);
            me.suspend();
            $display("after t=%0t status=%0d", $time, me.status());
          end
        endmodule
    )");
    CHECK(out == "before t=0\nafter t=10 status=1\n");
}

// Suspending a completed thread has nothing to suspend, and resuming a thread that
// is not suspended is a no-op (LRM 9.7 says nothing about either, so the conservative
// reading is that neither changes a terminal or running state).
TEST_CASE("process: suspend and resume are no-ops on a completed thread") {
    const std::string out = run_capture(R"(
        module t;
          process child;
          initial begin
            fork
              begin child = process::self(); #5; end
            join
            child.suspend();
            child.resume();
            $display("status=%0d t=%0t", child.status(), $time);
          end
        endmodule
    )");
    CHECK(out == "status=0 t=5\n");
}

// -- `wait fork` (LRM 9.6.1) ---------------------------------------------------

// LRM-pinned. The barrier covers the whole subtree, not just immediate children: the
// grandchild here finishes last (150) and that is when `wait fork` returns.
TEST_CASE("process: wait fork blocks on the whole descendant set") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              begin #30 $display("child at %0t", $time); end
              begin #10 fork begin #40 $display("grandchild at %0t", $time); end join_none end
            join_none
            wait fork;
            $display("wait fork returned at %0t", $time);
          end
        endmodule
    )");
    CHECK(out == "child at 30\ngrandchild at 50\nwait fork returned at 50\n");
}

// With no live descendants the barrier is one pointer test and costs no suspend.
TEST_CASE("process: wait fork with nothing forked returns at once") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            #7 wait fork;
            $display("t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "t=7\n");
}

// `wait fork` stays refused inside an assertion action block, and for an identity
// reason rather than a region one: the block runs on the monitor process's thread.
TEST_CASE("process: wait fork is refused in an assertion action block") {
    check_rejected("module t; bit clk, a, b;"
                   " ap: assert property (@(posedge clk) a |=> b) else wait fork;"
                   " endmodule",
                   "the descendants it would wait for are the monitor process's");
}

// -- srandom / get_randstate / set_randstate (LRM 9.7, 18.14) -------------------

// Verilator-scored for the shape (it implements all three), though the state STRING
// is implementation-defined and so is not compared with Verilator's: what is pinned
// is the contract -- a state captured and restored replays the same draws, and the
// same seed reproduces the same stream.
TEST_CASE("process: randstate round-trips and srandom is reproducible") {
    const std::string out = run_capture(R"(
        module t;
          process p;
          string st;
          int a, b, c;
          initial begin
            p = process::self();
            p.srandom(7);
            a  = $urandom();
            st = p.get_randstate();
            b  = $urandom();
            p.set_randstate(st);
            c  = $urandom();
            p.srandom(7);
            $display("len=%0d replay=%0d reseed=%0d", st.len(), b == c, $urandom() == a);
          end
        endmodule
    )");
    CHECK(out == "len=64 replay=1 reseed=1\n");
}

// LRM 18.14: the generator is PER PROCESS, so seeding through a handle reaches that
// thread's stream and not the caller's. Reseeding mid-flight changes what the target
// draws next, which is the whole point of the method.
TEST_CASE("process: srandom through a handle reseeds that thread") {
    const std::string out = run_capture(R"(
        module t;
          process w;
          int x1 = 0, x2 = 0;
          initial begin
            fork
              begin
                w = process::self();
                #1  x1 = $urandom();
                #10 x2 = $urandom();
              end
            join_none
            #5  w.srandom(99);
            #20 $display("changed=%0d", x1 != x2);
          end
        endmodule
    )");
    CHECK(out == "changed=1\n");
}

// Two threads seeded identically draw identical streams -- random stability per
// thread (LRM 18.14.1), reached here through the handle rather than at the fork.
TEST_CASE("process: two threads seeded alike draw alike") {
    const std::string out = run_capture(R"(
        module t;
          int u = 0, v = 0;
          initial begin
            fork
              begin process s = null; s = process::self(); s.srandom(1234); u = $urandom(); end
              begin process s = null; s = process::self(); s.srandom(1234); v = $urandom(); end
            join
            $display("equal=%0d", u == v);
          end
        endmodule
    )");
    CHECK(out == "equal=1\n");
}
