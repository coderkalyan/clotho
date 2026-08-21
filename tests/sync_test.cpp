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

// -- mailbox #(T), non-blocking (LRM 15.4.1, 15.4.2, 15.4.4, 15.4.6, 15.4.8) ---

// Verilator-scored: it implements the whole non-blocking surface.
TEST_CASE("mailbox: num and the try_ trio") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            int a, r;
            m = new(2);
            $display("num=%0d", m.num());
            r = m.try_put(7);  $display("try_put -> %0d num=%0d", r, m.num());
            r = m.try_peek(a); $display("try_peek -> %0d a=%0d num=%0d", r, a, m.num());
            r = m.try_get(a);  $display("try_get -> %0d a=%0d num=%0d", r, a, m.num());
            r = m.try_get(a);  $display("try_get empty -> %0d", r);
            r = m.try_put(1); r = m.try_put(2); r = m.try_put(3);
            $display("full try_put -> %0d num=%0d", r, m.num());
          end
        endmodule
    )");
    CHECK(out == "num=0\ntry_put -> 1 num=1\ntry_peek -> 1 a=7 num=1\ntry_get -> 1 a=7 num=0\n"
                 "try_get empty -> 0\nfull try_put -> 0 num=2\n");
}

// LRM 15.4.1: bound 0 is unbounded and "a put() operation shall never block".
TEST_CASE("mailbox: an unbounded mailbox never blocks a send") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            m = new();
            for (int i = 0; i < 100; i++) m.put(i);
            $display("num=%0d t=%0t", m.num(), $time);
          end
        endmodule
    )");
    CHECK(out == "num=100 t=0\n");
}

// -- mailbox #(T), blocking (LRM 15.4.3, 15.4.5, 15.4.7) ------------------------

// A bounded mailbox with a producer and a consumer: the blocked `put` resumes in
// the same instant the `get` frees room. Verilator-scored -- this trace was taken
// from Verilator 5.050 and matches instant for instant.
TEST_CASE("mailbox: a bounded put blocks and resumes when room appears") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            m = new(2);
            fork
              begin
                for (int i = 0; i < 4; i++) begin
                  m.put(i);
                  $display("[%0t] put %0d num=%0d", $time, i, m.num());
                end
              end
              begin
                int v;
                #10;
                for (int i = 0; i < 4; i++) begin
                  m.get(v);
                  $display("[%0t] got %0d num=%0d", $time, v, m.num());
                  #5;
                end
              end
            join
          end
        endmodule
    )");
    CHECK(out == "[0] put 0 num=1\n[0] put 1 num=2\n[10] got 0 num=1\n[10] put 2 num=2\n"
                 "[15] got 1 num=1\n[15] put 3 num=2\n[20] got 2 num=1\n[25] got 3 num=0\n");
}

// LRM 15.4.5's "The mailbox waiting queue is FIFO", with three receivers so FIFO
// is distinguishable from LIFO. LRM-pinned.
TEST_CASE("mailbox: three receivers are served in arrival order") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            m = new();
            fork
              begin int v; #1 m.get(v); $display("[%0t] A got %0d", $time, v); end
              begin int v; #2 m.get(v); $display("[%0t] B got %0d", $time, v); end
              begin int v; #3 m.get(v); $display("[%0t] C got %0d", $time, v); end
              begin #10 m.put(1); #10 m.put(2); #10 m.put(3); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[10] A got 1\n[20] B got 2\n[30] C got 3\ndone t=30\n");
}

// The same rule on the send side of a bounded mailbox: three blocked senders are
// admitted in arrival order as the receiver drains. LRM-pinned (15.4.5).
TEST_CASE("mailbox: three blocked senders are admitted in arrival order") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            m = new(1);
            fork
              begin m.put(1); $display("[%0t] first", $time); end
              begin #1 m.put(2); $display("[%0t] second", $time); end
              begin #2 m.put(3); $display("[%0t] third", $time); end
              begin int v; #10 m.get(v); #10 m.get(v); #10 m.get(v); end
            join
            $display("done t=%0t num=%0d", $time, m.num());
          end
        endmodule
    )");
    CHECK(out == "[0] first\n[10] second\n[20] third\ndone t=30 num=0\n");
}

// LRM 15.4.7: "Calling the peek() method can also cause one message to unblock more
// than one process. As long as a message remains in the mailbox queue, any process
// blocked in either a peek() or get() operation shall become unblocked." So one
// message unblocks BOTH peekers and the receiver, and the peekers -- which consume
// nothing -- observe it before the receiver takes it. LRM-pinned: the wake order is
// this design's reading of that sentence (docs/design/mailbox-and-semaphore.md §15).
TEST_CASE("mailbox: one message unblocks every peeker and one receiver") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            m = new();
            fork
              begin int v; m.peek(v); $display("[%0t] P1 %0d", $time, v); end
              begin int v; m.peek(v); $display("[%0t] P2 %0d", $time, v); end
              begin int v; m.get(v);  $display("[%0t] G %0d", $time, v); end
              begin #10 m.put(9); end
            join
            $display("done t=%0t num=%0d", $time, m.num());
          end
        endmodule
    )");
    CHECK(out == "[10] P1 9\n[10] P2 9\n[10] G 9\ndone t=10 num=0\n");
}

// A peeker is NOT subject to the arrival-order rule, because it consumes nothing
// and so can starve nobody: it peeks the message even though two receivers arrived
// before it. LRM-pinned -- 15.4.7 against 15.4.5, recorded in §15.
TEST_CASE("mailbox: a peeker behind two receivers still peeks") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            m = new();
            fork
              begin int v; m.get(v); $display("[%0t] G1 got %0d", $time, v); end
              begin int v; #1 m.get(v); $display("[%0t] G2 got %0d", $time, v); end
              begin int v; #2 m.peek(v); $display("[%0t] P peeked %0d num=%0d", $time, v, m.num()); end
              begin #10 m.put(1); #10 m.put(2); end
            join
            $display("done t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "[10] P peeked 1 num=1\n[10] G1 got 1\n[20] G2 got 2\ndone t=20\n");
}

// A blocking `peek` on an empty mailbox waits, then copies WITHOUT removing (the
// following `get` still finds the message). Verilator-scored.
TEST_CASE("mailbox: peek blocks on empty and does not consume") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          initial begin
            int v;
            m = new();
            fork
              begin #20 m.put(7); $display("[%0t] put", $time); end
              begin
                m.peek(v); $display("[%0t] peek %0d num=%0d", $time, v, m.num());
                m.get(v);  $display("[%0t] get %0d num=%0d", $time, v, m.num());
              end
            join
          end
        endmodule
    )");
    CHECK(out == "[20] put\n[20] peek 7 num=1\n[20] get 7 num=0\n");
}

// -- the payload matrix (LRM 15.4.3's "any singular expression") ----------------

// A string, a class handle, a wide 4-state vector, an unpacked struct with a string
// leaf, and a nested queue: every one rides the queue element descriptor the
// container path already builds, so the message needs no mailbox-specific code.
// The string case is sv-tests' 15.4--mailbox-blocking.sv; the class case is
// Verilator-scored (handle identity is preserved).
TEST_CASE("mailbox: the payload matrix") {
    const std::string out = run_capture(R"(
        typedef struct { int a; string s; } pkt;
        class obj; int x; function new(int v); x = v; endfunction endclass
        module t;
          typedef int iq[$];
          mailbox #(string)        ms;
          mailbox #(obj)           mo;
          mailbox #(logic [95:0])  mw;
          mailbox #(pkt)           mp;
          mailbox #(iq)            mq;
          initial begin
            string       s = "abc", sr;
            obj          a, b;
            logic [95:0] w = 96'hdead_beef_1234_5678_9abc_def0, wr;
            pkt          p, pr;
            iq           q, qr;
            ms = new(); mo = new(); mw = new(); mp = new(); mq = new();
            a = new(5);
            p.a = 3; p.s = "hi";
            q.push_back(4); q.push_back(5);
            ms.put(s); mo.put(a); mw.put(w); mp.put(p); mq.put(q);
            ms.get(sr); mo.get(b); mw.get(wr); mp.get(pr); mq.get(qr);
            $display("string '%s'", sr);
            $display("handle x=%0d identity=%0d", b.x, a == b);
            $display("wide %h", wr);
            $display("struct a=%0d s=%s", pr.a, pr.s);
            $display("queue size=%0d %0d %0d", qr.size(), qr[0], qr[1]);
          end
        endmodule
    )");
    CHECK(out == "string 'abc'\nhandle x=5 identity=1\nwide deadbeef123456789abcdef0\n"
                 "struct a=3 s=hi\nqueue size=2 4 5\n");
}

// Two specializations are two unrelated types with two bodies of their own, which
// is the parameterized-class path with nothing added.
TEST_CASE("mailbox: two specializations are independent") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int)    a;
          mailbox #(string) b;
          initial begin
            int i; string s;
            a = new(); b = new();
            a.put(1); b.put("x");
            a.get(i); b.get(s);
            $display("%0d %s", i, s);
          end
        endmodule
    )");
    CHECK(out == "1 x\n");
}

// A mailbox as a class property and in a queue: an ordinary class handle either way.
TEST_CASE("mailbox: a handle in a class property and in a queue") {
    const std::string out = run_capture(R"(
        class agent;
          mailbox #(int) mb;
          function new(); mb = new(); endfunction
          task send(int v); mb.put(v); endtask
        endclass
        module t;
          agent          a;
          mailbox #(int) chans[$];
          initial begin
            int v, r;
            a = new();
            a.send(11);
            a.mb.get(v);
            $display("property v=%0d num=%0d", v, a.mb.num());
            chans.push_back(a.mb);
            r = chans[0].try_put(3);
            r = chans[0].try_get(v);
            $display("through a queue v=%0d", v);
          end
        endmodule
    )");
    CHECK(out == "property v=11 num=0\nthrough a queue v=3\n");
}

// -- interaction with process control (LRM 9.7) --------------------------------

// LRM-pinned. All three blocking methods park
// in the same retry loop, so all three must abort: a receiver on an empty mailbox,
// a sender on a full one, and a peeker on an empty one.
TEST_CASE("mailbox: killing a thread blocked in get, put or peek terminates it") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) mg, mp;
          process g, p, k;
          int n = 0;
          initial begin
            int r;
            mg = new();
            mp = new(1);
            r  = mp.try_put(1);
            fork
              begin int v; g = process::self(); mg.get(v);  n = 1; $display("getter ran"); end
            join_none
            fork
              begin p = process::self(); mp.put(2); n = 2; $display("sender ran"); end
            join_none
            fork
              begin int v; k = process::self(); mg.peek(v); n = 3; $display("peeker ran"); end
            join_none
            #5 g.kill(); p.kill(); k.kill();
            #5 $display("t=%0t n=%0d g=%0d p=%0d k=%0d num=%0d",
                        $time, n, g.status(), p.status(), k.status(), mp.num());
          end
        endmodule
    )");
    CHECK(out == "t=10 n=0 g=4 p=4 k=4 num=1\n");
}

// A kill landing on the queue's HEAD in the same instant as a `put` that would
// have satisfied it: the aborting thread hands off, so the second key still
// reaches the next live waiter. Without the hand-off on the abort path the wake
// died with the killed thread and every waiter behind it stranded -- `put(2)`
// served exactly one of four. LRM-pinned (15.3.2 + 15.3.3).
TEST_CASE("semaphore: killing the queue head hands off to the next waiter") {
    const std::string out = run_capture(R"(
        module t;
          semaphore s;
          process p0, p1, p2, p3;
          int n = 0;
          initial begin
            s = new(0);
            fork
              begin p0 = process::self(); s.get(1); n = n + 1; $display("[%0t] w0", $time); end
              begin p1 = process::self(); s.get(1); n = n + 1; $display("[%0t] w1", $time); end
              begin p2 = process::self(); s.get(1); n = n + 1; $display("[%0t] w2", $time); end
              begin p3 = process::self(); s.get(1); n = n + 1; $display("[%0t] w3", $time); end
            join_none
            #10;
            s.put(2);
            p1.kill();
            #10 $display("t=%0t n=%0d %0d %0d %0d %0d",
                         $time, n, p0.status(), p1.status(), p2.status(), p3.status());
          end
        endmodule
    )");
    // w0 takes the first key; the killed head hands the second to w2; w3 waits.
    CHECK(out == "[10] w0\n[10] w2\nt=20 n=2 0 4 0 2\n");
}

// The other order of the same race, on a mailbox: the message a killed receiver
// was about to take stays in the mailbox rather than vanishing with it.
TEST_CASE("mailbox: a put racing a kill of the receiver keeps the message") {
    const std::string out = run_capture(R"(
        module t;
          mailbox #(int) m;
          process w;
          int n = 0;
          initial begin
            m = new();
            fork
              begin int v; w = process::self(); #5; m.get(v); n = 1; $display("receiver ran"); end
            join_none
            #10;
            m.put(3);
            w.kill();
            #5 $display("t=%0t n=%0d w=%0d num=%0d", $time, n, w.status(), m.num());
          end
        endmodule
    )");
    CHECK(out == "t=15 n=0 w=4 num=1\n");
}

// -- what is refused, and why (each loud, each a TODO) --------------------------

// LRM 15.4.1: "The bound shall be positive. Negative bounds are illegal and can
// result in indeterminate behavior, but implementations can issue a warning."
// Clotho never silently accepts, so it reports -- at run time, because the bound
// need not be a constant.
TEST_CASE("mailbox: a negative bound is a located fatal") {
    const char* src = "module t;\n"
                      "  mailbox #(int) m;\n"
                      "  int nb;\n"
                      "  initial begin\n"
                      "    nb = -1;\n"
                      "    $display(\"before\");\n"
                      "    m = new(nb);\n"
                      "    $display(\"after\");\n"
                      "  end\n"
                      "endmodule\n";
    const test::DesignRun run = test::run_design_captured(src);
    CHECK(run.out.find("before") != std::string::npos);
    CHECK(run.out.find("after") == std::string::npos);
    CHECK(run.err.find("mailbox bound is negative") != std::string::npos);
}

// The typeless mailbox (LRM 15.4.9's `dynamic_type`) is legal SystemVerilog and
// slang type-checks it, so it is refused by its own message rather than the generic
// std-package one: it needs a dynamically typed element and a run-time type
// identity for 15.4.5's check and 15.4.6's negative return.
TEST_CASE("mailbox: the typeless mailbox is refused by its own name") {
    CHECK(fails("module t; mailbox m; initial begin int a; m = new(); m.put(1); m.get(a); end endmodule")
            .find("the typeless std::mailbox (LRM 15.4.9) is not supported") != std::string::npos);
}

// An event element would leak one sync object per `put`: rt_queue_push_back
// constructs the element before copying it, and constructing an event element mints
// a new object (LRM Table 6-7) that the copy then overwrites -- and event objects
// are never freed.
TEST_CASE("mailbox: a mailbox of events is refused") {
    CHECK(fails("module t; mailbox #(event) m; initial m = new(); endmodule")
            .find("a mailbox of events is not supported") != std::string::npos);
}

// -- what is still refused ------------------------------------------------------

// `mailbox` (LRM 15.4) and `std::weak_reference` (LRM 8.30) keep the std-package
// guard, each by its own name, now that `semaphore` no longer does.
TEST_CASE("built-in std classes: weak_reference keeps the std-package guard") {
    CHECK(fails("module m; std::weak_reference #(int) w; initial w = null; endmodule")
            .find("not supported") != std::string::npos);
}
