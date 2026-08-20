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

// JIT-run a design and capture stdout (the process_test.cpp / class_test.cpp
// harness). Oracle policy per case, below.
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

std::string fails(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostic_text();
}

} // namespace

// -- semaphore, non-blocking (LRM 15.3.1, 15.3.2, 15.3.4) ----------------------

// Verilator-scored: every line here was cross-checked against Verilator 5.050,
// which implements the whole non-blocking surface.
TEST_CASE("semaphore: try_get is availability-only, and put has no ceiling") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          initial begin
            int r;
            s = new(3);
            r = s.try_get(4); $display("try_get(4) of 3 -> %0d", r);
            r = s.try_get(3); $display("try_get(3) -> %0d", r);
            r = s.try_get(1); $display("try_get(1) -> %0d", r);
            s.put(2);
            r = s.try_get(2); $display("after put(2) try_get(2) -> %0d", r);
            s.put(); r = s.try_get(); $display("default put/get -> %0d", r);
          end
        endmodule
    )");
    CHECK(out == "try_get(4) of 3 -> 0\ntry_get(3) -> 1\ntry_get(1) -> 0\n"
                 "after put(2) try_get(2) -> 1\ndefault put/get -> 1\n");
}

// LRM 15.3.1: "The number of keys in the bucket can increase beyond keyCount when
// more keys are put into the semaphore than are removed." So the count is not
// clamped to the constructor argument, and `new` with no argument starts at 0.
TEST_CASE("semaphore: keys accumulate past the constructor count") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          initial begin
            int r;
            s = new;
            r = s.try_get(1); $display("empty -> %0d", r);
            s.put(1); s.put(1); s.put(1);
            r = s.try_get(3); $display("after three puts -> %0d", r);
          end
        endmodule
    )");
    CHECK(out == "empty -> 0\nafter three puts -> 1\n");
}

// -- semaphore, blocking (LRM 15.3.2, 15.3.3) ----------------------------------

// LRM 15.3.2: a `put` executes a process that has been suspended waiting for a
// key, in the current time step. Verilator-scored.
TEST_CASE("semaphore: get blocks until a put arrives, and resumes at that instant") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          initial begin
            s = new(0);
            fork
              begin s.get(1); $display("[%0t] got", $time); end
              begin #10 s.put(1); $display("[%0t] put", $time); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[10] put\n[10] got\ndone t=10\n");
}

// **LRM-pinned, and a deliberate deviation from Verilator 5.050.** LRM 15.3.3:
// "The semaphore waiting queue is first-in first-out (FIFO). This does not
// guarantee the order in which processes arrive at the queue, only that their
// arrival order shall be preserved by the semaphore." Accellera's own reading
// (SemaphoreExamples.pdf, Interpretation #1) spells out the consequence: a
// blocked request blocks later ones "even if there are enough keys to immediately
// satisfy the request from a later semaphore request".
//
// So A, which arrived first wanting 2, is served at the second put -- B does not
// take the single key that arrives at 5 even though it would satisfy it. Verilator
// serves B first (verilated_std.sv's `m_nextKeyCount > m_keyCount` fast path) and
// then deadlocks A forever; scoring this against it would encode that deviation.
TEST_CASE("semaphore: a blocked head blocks a later smaller request") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          initial begin
            s = new(1);
            fork
              begin s.get(2); $display("[%0t] A got 2", $time); end
              begin #1 s.get(1); $display("[%0t] B got 1", $time); end
              begin #5 s.put(1); #4 s.put(1); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[5] A got 2\n[9] B got 1\ndone t=9\n");
}

// The arrival-order guarantee with three waiters: two cannot distinguish FIFO from
// LIFO, which is what makes a re-enqueue-on-spurious-wake bug invisible below
// three. LRM-pinned (15.3.3).
TEST_CASE("semaphore: three waiters are served in arrival order") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          initial begin
            s = new(0);
            fork
              begin #1 s.get(1); $display("[%0t] first", $time); end
              begin #2 s.get(1); $display("[%0t] second", $time); end
              begin #3 s.get(1); $display("[%0t] third", $time); end
              begin #10 s.put(1); #10 s.put(1); #10 s.put(1); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[10] first\n[20] second\n[30] third\ndone t=30\n");
}

// One `put` of several keys satisfies several waiters in one instant: the thread
// that leaves the queue hands off to the new head, so the chain runs without a
// second put. LRM 15.3.2 + 15.3.3.
TEST_CASE("semaphore: one put satisfies two waiters in arrival order") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          initial begin
            s = new(0);
            fork
              begin #1 s.get(1); $display("[%0t] A", $time); end
              begin #2 s.get(1); $display("[%0t] B", $time); end
              begin #5 s.put(2); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[5] A\n[5] B\ndone t=5\n");
}

// LRM-pinned (§15 of the design doc): `try_get` is defined on availability alone
// (15.3.4) and never joins the waiting queue, so 15.3.3's FIFO rule -- which is
// about that queue -- does not constrain it. A `try_get` may therefore take the key
// a blocked waiter is waiting for. Verilator agrees.
TEST_CASE("semaphore: try_get may take a key a blocked waiter wanted") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          initial begin
            int r;
            s = new(0);
            fork
              begin s.get(1); $display("[%0t] blocked getter", $time); end
              begin #5 s.put(1); r = s.try_get(1); $display("[%0t] try_get -> %0d", $time, r); #5 s.put(1); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[5] try_get -> 1\n[10] blocked getter\ndone t=10\n");
}

// A blocking `get` behind a task call: the task runs in its caller's thread, so the
// park has to unwind through the call, which is what `tag_suspends` covers.
TEST_CASE("semaphore: get blocks inside a task called by the thread") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          task automatic grab(semaphore h, int n);
            h.get(n);
            $display("[%0t] grabbed %0d", $time, n);
          endtask
          initial begin
            s = new(0);
            fork
              grab(s, 2);
              begin #7 s.put(1); #7 s.put(1); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[14] grabbed 2\ndone t=14\n");
}

// -- semaphore as an ordinary object (LRM 8) -----------------------------------

// The handle is an ordinary class handle: storable in a class property, comparable,
// and null until constructed.
TEST_CASE("semaphore: a handle is an ordinary class handle") {
    const std::string out = run_capture(R"(
        class chan;
          semaphore lock;
          function new(); lock = new(1); endfunction
        endclass
        module t;
          chan c;
          semaphore a, b;
          initial begin
            int r;
            c = new();
            r = c.lock.try_get(1); $display("property -> %0d", r);
            a = new(1);
            b = a;
            $display("identity %0d %0d %0d", a == b, a == null, c.lock == a);
            r = b.try_get(1); $display("through the alias -> %0d", r);
            r = a.try_get(1); $display("the alias shared the object -> %0d", r);
          end
        endmodule
    )");
    CHECK(out == "property -> 1\nidentity 1 0 0\nthrough the alias -> 1\n"
                 "the alias shared the object -> 0\n");
}

// A semaphore in a queue: the element is a class handle, which the container path
// already admits.
TEST_CASE("semaphore: a handle in a queue") {
    const std::string out = run_capture(R"(
        module t;
          semaphore q[$];
          initial begin
            int r;
            semaphore a;
            a = new(2);
            q.push_back(a);
            r = q[0].try_get(2);
            $display("through a queue -> %0d size=%0d", r, q.size());
          end
        endmodule
    )");
    CHECK(out == "through a queue -> 1 size=1\n");
}

// LRM 8.4: reaching a method through a null handle is a run-time error, not a
// segfault. Out of process, because it exits.
TEST_CASE("semaphore: a method on a null handle is a located fatal") {
    const char* src = "module t;\n"
                      "  semaphore s;\n"
                      "  initial s.put(1);\n"
                      "endmodule\n";
    const test::DesignRun run = test::run_design_captured(src);
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("dereference of a null class handle") != std::string::npos);
}

// -- interaction with process control (LRM 9.7) --------------------------------

// LRM-pinned. A killed thread parked in
// `get` must never run its continuation, and a later `put` must not resurrect it.
// This is the retry-loop hazard the `process` work had in `await`: the wait is a
// loop inside one instruction, so the abort is tested by the op itself.
TEST_CASE("semaphore: killing a thread blocked in get terminates it") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          process w;
          int n = 0;
          initial begin
            s = new(0);
            fork
              begin w = process::self(); s.get(1); n = 1; $display("killed waiter ran at %0t", $time); end
            join_none
            #10 w.kill();
            #20 s.put(1);
            #30 $display("t=%0t n=%0d w=%0d", $time, n, w.status());
          end
        endmodule
    )");
    CHECK(out == "t=60 n=0 w=4\n");
}

// LRM 9.7: a suspended process does not execute, and `resume` continues it in the
// current time step. So the key that arrives at 15 is NOT consumed while the
// waiter is suspended -- the retest runs after the suspension poll, never before
// it -- and the waiter takes it the instant it is resumed. LRM-pinned: Verilator
// refuses `suspend()` outright.
TEST_CASE("semaphore: a suspended waiter does not consume, and resume takes the key") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          process w;
          initial begin
            s = new(0);
            fork
              begin w = process::self(); s.get(1); $display("[%0t] got key", $time); end
            join_none
            #5  w.suspend();
            #10 s.put(1);
            #10 $display("[%0t] suspended status=%0d", $time, w.status());
            w.resume();
            #1  $display("[%0t] status=%0d", $time, w.status());
          end
        endmodule
    )");
    CHECK(out == "[25] suspended status=3\n[25] got key\n[26] status=0\n");
}

// `disable fork` reaches a descendant blocked in `get`, by the same poll.
TEST_CASE("semaphore: disable fork ends a descendant blocked in get") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          int n = 0;
          initial begin
            s = new(0);
            fork
              begin s.get(1); n = 1; $display("disabled waiter ran at %0t", $time); end
            join_none
            #10 disable fork;
            #10 s.put(1);
            #10 $display("t=%0t n=%0d", $time, n);
          end
        endmodule
    )");
    CHECK(out == "t=30 n=0\n");
}

// -- what is still refused ------------------------------------------------------

// `mailbox` (LRM 15.4) and `std::weak_reference` (LRM 8.30) keep the std-package
// guard, each by its own name, now that `semaphore` no longer does.
TEST_CASE("built-in std classes still refused after semaphore lands") {
    CHECK(fails("module m; mailbox b; initial b = new(); endmodule")
            .find("the built-in 'mailbox' class is not supported") != std::string::npos);
    CHECK(fails("module m; mailbox #(int) b; initial b = new(2); endmodule")
            .find("the built-in 'mailbox' class is not supported") != std::string::npos);
}
