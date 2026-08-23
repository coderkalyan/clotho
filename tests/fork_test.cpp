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

// fork … join (LRM 9.3.2), V1. Values cross-checked against Icarus Verilog 13.
// Child execution order among same-instant children is LRM-unspecified; Clotho
// runs them in source order, so the deterministic checks below assume that.

TEST_CASE("fork: zero-time children all run before the join completes") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              $display("A");
              $display("B");
            join
            $display("done");
          end
        endmodule
    )");
    CHECK(out == "A\nB\ndone\n");
}

TEST_CASE("fork: parent blocks until the last child finishes") {
    // B finishes at 5, A at 10; join resumes the parent at 10.
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              begin #10 $display("A@%0t", $time); end
              begin #5  $display("B@%0t", $time); end
            join
            $display("done@%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "B@5\nA@10\ndone@10\n");
}

TEST_CASE("fork: event-controlled children write shared parent variables") {
    const std::string out = run_capture(R"(
        module t;
          logic [7:0] a = 0, b = 0;
          logic clk = 0;
          always #5 clk = ~clk;
          initial begin
            fork
              begin @(posedge clk) a = 8'd11; end
              begin @(posedge clk) b = 8'd22; end
            join
            $display("a=%0d b=%0d t=%0t", a, b, $time);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "a=11 b=22 t=5\n");
}

TEST_CASE("fork: join serializes reuse across loop iterations") {
    // Each iteration blocks at the join, so children never overlap: the bounded
    // static ring holds (docs/design/fork-join.md).
    const std::string out = run_capture(R"(
        module t;
          logic [7:0] a = 0, b = 0;
          initial begin
            for (int i = 0; i < 3; i = i + 1) begin
              fork
                a = a + 8'd1;
                b = b + 8'd2;
              join
            end
            $display("a=%0d b=%0d", a, b);
          end
        endmodule
    )");
    CHECK(out == "a=3 b=6\n");
}

TEST_CASE("fork: nonblocking assigns from children commit") {
    const std::string out = run_capture(R"(
        module t;
          logic clk = 0; logic [7:0] q1 = 0, q2 = 0;
          always #5 clk = ~clk;
          initial begin
            fork
              repeat (2) @(posedge clk) q1 <= q1 + 8'd1;
              repeat (2) @(posedge clk) q2 <= q2 + 8'd2;
            join
            #1 $display("q1=%0d q2=%0d", q1, q2);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "q1=2 q2=4\n");
}

TEST_CASE("fork: single-child and empty forks") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork $display("solo"); join
            fork join
            $display("after");
          end
        endmodule
    )");
    CHECK(out == "solo\nafter\n");
}

// The two statically-sized structures a fork child reaches whose capacity used to
// ignore fork width entirely. Both overran silently: the wait bucket was sized by
// the PROCESS count (a width-4 fork already stored past the end and resumed a
// garbage handle), and the `#0` batch is a bump allocator with no bounds check.
// Values cross-checked against Icarus Verilog 13.

TEST_CASE("fork: children waiting on one edge fit the dynamic wait bucket") {
    // Eight children park on (clk, posedge) at the same instant. `wait_counts`
    // reserved `mod_.processes().size()` slots for ALL task waits together — three
    // here — so this stored four entries past the end of clotho.wait.clk.dyn.pos.
    // Blocking adds to one variable, so the total is order-independent.
    const std::string out = run_capture(R"(
        module t;
          bit clk = 0;
          int hits = 0;
          always #5 clk = ~clk;
          initial begin
            fork
              begin @(posedge clk); hits = hits + 1; end
              begin @(posedge clk); hits = hits + 1; end
              begin @(posedge clk); hits = hits + 1; end
              begin @(posedge clk); hits = hits + 1; end
              begin @(posedge clk); hits = hits + 1; end
              begin @(posedge clk); hits = hits + 1; end
              begin @(posedge clk); hits = hits + 1; end
              begin @(posedge clk); hits = hits + 1; end
            join
            $display("hits=%0d t=%0t", hits, $time);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "hits=8 t=5\n");
}

TEST_CASE("fork: children parked on #0 fit the inactive batch") {
    // Every child parks on the inactive (#0) batch at once, so the batch has to be
    // sized by fork width like the ring is. Overrunning it is silent: the push is a
    // bump store with no mask, and its guard is emitted only for a design that
    // forks.
    const std::string out = run_capture(R"(
        module t;
          int n = 0;
          initial begin
            fork
              #0 n = n + 1;
              #0 n = n + 1;
              #0 n = n + 1;
              #0 n = n + 1;
              #0 n = n + 1;
              #0 n = n + 1;
              #0 n = n + 1;
              #0 n = n + 1;
            join
            $display("n=%0d t=%0t", n, $time);
          end
        endmodule
    )");
    CHECK(out == "n=8 t=0\n");
}

// fork … join inside a TASK body (LRM 9.3.2 with 13.4.2). slang itself rejects
// `join` / `join_any` inside a function, so a task is the only subroutine that can
// reach the lowering. Values cross-checked against Icarus Verilog 13.

TEST_CASE("fork: join inside a task blocks the caller until the last child") {
    const std::string out = run_capture(R"(
        module t;
          logic [7:0] a = 0, b = 0;
          task run;
            fork
              begin #10 a = 8'd11; end
              begin #5  b = 8'd22; end
            join
            $display("a=%0d b=%0d t=%0t", a, b, $time);
          endtask
          initial begin
            run;
            $display("after t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "a=11 b=22 t=10\nafter t=10\n");
}

TEST_CASE("fork: a task containing a fork survives concurrent activations") {
    // The ring-capacity case. One task body, three live activations at once, each
    // forking three children that all mature at the same instant: nine child
    // handles ready together, where counting child BODIES would have reserved
    // three (ensure_active_queue's derivation). Blocking adds to one variable, so
    // the total is order-independent and the check does not assume a child order.
    const std::string out = run_capture(R"(
        module t;
          int c = 0;
          task automatic bump;
            fork
              begin #5 c = c + 1; end
              begin #5 c = c + 10; end
              begin #5 c = c + 100; end
            join
          endtask
          initial begin
            fork
              bump;
              bump;
              bump;
            join
            $display("c=%0d t=%0t", c, $time);
          end
        endmodule
    )");
    CHECK(out == "c=333 t=5\n");
}

TEST_CASE("fork: a child reads and writes the enclosing task's formal and local") {
    // The capture path: `n` is a formal, `m` an automatic local, and both live only
    // in the task's frame. Each child takes them as ArgDir::Ref formals, so the
    // second child's write to `m` is visible to the task after the join.
    const std::string out = run_capture(R"(
        module t;
          task automatic show(input int n);
            int m;
            m = n * 2;
            fork
              begin #1 $display("child n=%0d m=%0d", n, m); end
              begin #2 m = m + 1; end
            join
            $display("after m=%0d t=%0t", m, $time);
          endtask
          initial show(3);
        endmodule
    )");
    CHECK(out == "child n=3 m=6\nafter m=7 t=2\n");
}

TEST_CASE("fork: one task's fork, two concurrent calls, children reading its formals") {
    // Multiple activations AND capture at once: each activation's children must see
    // their own frame's `d` and `v`, not the other's.
    const std::string out = run_capture(R"(
        module t;
          logic [7:0] x = 0, y = 0;
          task automatic inner(input int d, input logic [7:0] v);
            fork
              begin #(d)     x = x + v; end
              begin #(2 * d) y = y + v; end
            join
            $display("inner d=%0d t=%0t", d, $time);
          endtask
          initial begin
            fork
              inner(5, 8'd1);
              inner(2, 8'd16);
            join
            $display("x=%0d y=%0d t=%0t", x, y, $time);
          end
        endmodule
    )");
    CHECK(out == "inner d=2 t=4\ninner d=5 t=10\nx=17 y=17 t=10\n");
}

// What capture used to refuse, under `join`. The old ABI passed a pointer to a
// packed slot, so a `fork..join` child could capture only what that could carry (a
// packed integral or a dynamic array); a string, real, unpacked, queue or class
// handle was named and refused. The storage rule is now one GC box per captured
// variable for every join kind, so the repertoire is the declared type
// (docs/design/fork-join.md V4).
TEST_CASE("fork: a join child captures a non-integral task local") {
    const std::string out = run_capture(R"(
        module t;
          typedef struct { int a; string s; } pair_t;
          task automatic show;
            string  s = "hi";
            real    r = 1.5;
            pair_t  p = '{7, "in"};
            int     q[$];
            q.push_back(3);
            fork
              #1 $display("%s %0.1f %0d %s %0d", s, r, p.a, p.s, q[0]);
              #2 begin s = "bye"; r = 2.5; end
            join
            $display("after %s %0.1f", s, r);
          endtask
          initial show;
        endmodule
    )");
    CHECK(out == "hi 1.5 7 in 3\nafter bye 2.5\n");
}

// A bare property reference inside a method captures the METHOD'S `this`, not the
// property: value_lvalue re-roots such a reference at the `this` automatic, and that
// automatic is a real slang ValueSymbol living in the frame like any other, so it
// boxes and travels by exactly the same machinery. Refused outright before, for every
// join kind, because a handle was not one of the ref ABI's carryable types.
TEST_CASE("fork: a child reads and writes a class property through the implicit this") {
    const std::string out = run_capture(R"(
        module t;
          class C;
            int    v = 1;
            string n = "c";
            task go();
              fork
                #1 begin v = v + 10; $display("child v=%0d n=%s", v, n); end
                #2 $display("later v=%0d", v);
              join
              $display("after v=%0d", v);
            endtask
            task spawn();
              fork
                #3 $display("detached v=%0d this.n=%s", v, this.n);
              join_none
            endtask
          endclass
          initial begin
            C c;
            c = new();
            c.spawn();
            c.go();
            #5 $display("end v=%0d", c.v);
          end
        endmodule
    )");
    // `go`'s join holds the caller to t=2, so `after` precedes the detached child
    // `spawn` left on the wheel for t=3 -- which still runs, and still reads the same
    // object, after the activation that spawned it returned.
    CHECK(out ==
          "child v=11 n=c\n"
          "later v=11\n"
          "after v=11\n"
          "detached v=11 this.n=c\n"
          "end v=11\n");
}

// `disable fork` (LRM 9.6.3) and `wait fork` (LRM 9.6.1) both land with the
// `process` control blocks that carry the descendant set they need -- the behaviour
// is pinned in tests/process_test.cpp, and docs/design/process-class.md §8 records
// why fork V3 turned out to be a corollary of LRM 9.7 rather than a phase of its own.
// What fork keeps here is the ring-bound reasoning the detaching kinds rest on.

// `disable` reaching INTO a detached child (LRM 9.6.2 × 9.3.2). A `join_none` child
// can outlive its parent, so its blocks are the case a single occupant slot could
// never have covered: the block keeps a list of the activations inside it, and one
// `disable` ends every one. Oracle: iverilog.
TEST_CASE("fork: a detached child's block can be disabled from another process") {
    CHECK(run_capture(R"(
        module t;
          initial begin
            fork
              begin : detached
                #10 $display("never at %0t", $time);
              end
            join_none
            $display("past join_none at %0t", $time);
          end
          initial #5 disable detached;
          initial #30 $finish;
        endmodule
    )") == "past join_none at 0\n");
}

// Two detached activations of one task, both inside the same block, ended by one
// statement -- both callers then run their own continuation.
TEST_CASE("fork: one disable ends the same block in every detached activation") {
    CHECK(run_capture(R"(
        module t;
          task worker;
            begin : blk
              #10 $display("never at %0t", $time);
            end
            $display("worker done at %0t", $time);
          endtask
          initial begin
            fork
              worker;
              worker;
            join_none
            $display("spawned at %0t", $time);
          end
          initial #5 disable t.worker.blk;
          initial #30 $finish;
        endmodule
    )") == "spawned at 0\nworker done at 5\nworker done at 5\n");
}

// fork … join_none / join_any (LRM 9.3.2). Values cross-checked against Icarus
// Verilog 13, except where noted: iverilog's vvp aborts on either kind inside an
// automatic task (vthread.cc:3793, of_JOIN_DETACH assertion), so those cases are
// pinned from the LRM instead. Same source-order caveat as above — iverilog runs
// same-instant children in the reverse order, which LRM 9.3.2 permits, so every
// check below is either order-independent or ordered by delay.

TEST_CASE("fork: join_none does not run its children at the fork") {
    // The whole point of join_none: the parent does not block, so it reaches its
    // next statement BEFORE any child body runs. Children start when it blocks.
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              $display("child");
            join_none
            $display("parent");
            #1 $display("end");
          end
        endmodule
    )");
    CHECK(out == "parent\nchild\nend\n");
}

TEST_CASE("fork: a join_none child outlives the parent that spawned it") {
    // The parent ends at time 0 with a child still parked on the wheel. The join
    // record cannot live in the parent's frame for this to work — it is heap and
    // reference-counted, and the child holds the last reference.
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              #5 $display("late@%0t", $time);
            join_none
            $display("gone@%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "gone@0\nlate@5\n");
}

TEST_CASE("fork: join_none spawns an unbounded number of live children") {
    // A loop around a join_none accumulates children with no static bound — the
    // shape the whole capacity argument rests on (docs/design/fork-join.md). 100 of
    // them are live at one instant, well past the one-slot-per-process baseline.
    const std::string out = run_capture(R"(
        module t;
          int done = 0;
          task automatic kid;
            #1 done = done + 1;
          endtask
          initial begin
            for (int i = 0; i < 100; i = i + 1) begin
              fork
                kid;
              join_none
            end
            #10 $display("done=%0d", done);
          end
        endmodule
    )");
    CHECK(out == "done=100\n");
}

TEST_CASE("fork: unbounded join_none children all fit one signal's wait bucket") {
    // Every spawned child parks on the same (clk, posedge). The dynamic wait bucket
    // is sized statically, so an unbounded fork kind has to saturate its bound; a
    // per-activation count would have reserved one slot for the whole loop.
    const std::string out = run_capture(R"(
        module t;
          bit clk = 0;
          int hits = 0;
          always #5 clk = ~clk;
          task automatic kid;
            @(posedge clk);
            hits = hits + 1;
          endtask
          initial begin
            for (int i = 0; i < 40; i = i + 1) begin
              fork
                kid;
              join_none
            end
            repeat (3) @(posedge clk);
            $display("hits=%0d", hits);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "hits=40\n");
}

TEST_CASE("fork: join_any resumes on the first child and leaves the rest running") {
    const std::string out = run_capture(R"(
        module t;
          initial begin
            fork
              begin #10 $display("A@%0t", $time); end
              begin #5  $display("B@%0t", $time); end
              begin #20 $display("C@%0t", $time); end
            join_any
            $display("resumed@%0t", $time);
            #30 $display("end@%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "B@5\nresumed@5\nA@10\nC@20\nend@35\n");
}

TEST_CASE("fork: join_any does not run the siblings it is not waiting for") {
    // Both children would complete without blocking. join_any must proceed on the
    // first one rather than running them all, or it is `join` with extra steps: n is
    // 1 at the display, not 2. Both children do the same thing, so the check does
    // not depend on which of them runs first.
    const std::string out = run_capture(R"(
        module t;
          int n = 0;
          initial begin
            fork
              n = n + 1;
              n = n + 1;
            join_any
            $display("n=%0d t=%0t", n, $time);
          end
        endmodule
    )");
    CHECK(out == "n=1 t=0\n");
}

TEST_CASE("fork: a second fork site does not disturb the first's live children") {
    // The join_any parent walks on to another fork while its `#9` sibling is still
    // live. A single record slot reused across sites would have the second fork's
    // init stomp the first's live count.
    const std::string out = run_capture(R"(
        module t;
          int a = 0, b = 0;
          initial begin
            fork
              begin #1 a = a + 1; end
              begin #9 a = a + 10; end
            join_any
            fork
              begin #2 b = b + 1; end
              begin #3 b = b + 2; end
            join
            #20 $display("a=%0d b=%0d t=%0t", a, b, $time);
          end
        endmodule
    )");
    CHECK(out == "a=11 b=3 t=24\n");
}

TEST_CASE("fork: join_none children reach the NBA region") {
    // A child the parent never waits for still stages into the NBA region — the
    // classify walk covers fork-child bodies regardless of join kind.
    const std::string out = run_capture(R"(
        module t;
          bit clk = 0;
          logic [7:0] q = 0;
          always #5 clk = ~clk;
          initial begin
            fork
              @(posedge clk) q <= q + 8'd3;
            join_none
            repeat (2) @(posedge clk);
            $display("q=%0d", q);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "q=3\n");
}

TEST_CASE("fork: join_none inside a task returns without blocking the caller") {
    // LRM-pinned: iverilog's vvp aborts on a detaching fork inside an automatic
    // task (of_JOIN_DETACH assertion), so it cannot oracle this. join_none never
    // blocks, so the task returns at time 0 and its child still fires at #2.
    const std::string out = run_capture(R"(
        module t;
          int n = 0;
          task automatic spawner;
            fork
              #2 n = n + 1;
            join_none
            $display("spawner returns@%0t", $time);
          endtask
          initial begin
            spawner;
            #5 $display("n=%0d@%0t", n, $time);
          end
        endmodule
    )");
    CHECK(out == "spawner returns@0\nn=1@5\n");
}

TEST_CASE("fork: join_any inside a task resumes it on the first completion") {
    // LRM-pinned for the same reason as above. The task resumes at #1 with n = 1;
    // its #6 sibling keeps running and has added 10 by #11.
    const std::string out = run_capture(R"(
        module t;
          int n = 0;
          task automatic pick;
            fork
              #1 n = n + 1;
              #6 n = n + 10;
            join_any
          endtask
          initial begin
            pick;
            $display("mid n=%0d@%0t", n, $time);
            #10 $display("end n=%0d@%0t", n, $time);
          end
        endmodule
    )");
    CHECK(out == "mid n=1@1\nend n=11@11\n");
}

// A detaching child capturing a task automatic. LRM 9.3.2 makes a reference to an
// enclosing scope's automatic a reference to THAT variable -- which is why the clause
// has to spell `automatic int k = j;` to get a copy -- so parent and child share one
// `k` and the child's write is visible to the parent. Copying at spawn would lose it.
// iverilog agrees on the sharing; Verilator copies and deviates
// (docs/design/fork-join.md V4 records the three-way measurement).
//
// The storage is a per-variable GC box, which is what lets the child outlive the
// activation the variable was declared in.
TEST_CASE("fork: a join_none child shares the task automatic it captures") {
    const std::string out = run_capture(R"(
        module t;
          task automatic go;
            int k = 1;
            fork
              #1 k = k + 1;
            join_none
            #2 $display("parent k=%0d", k);
          endtask
          initial go;
        endmodule
    )");
    CHECK(out == "parent k=2\n");
}

TEST_CASE("fork: join_any children share the task automatic they capture") {
    const std::string out = run_capture(R"(
        module t;
          task automatic go;
            int k = 1;
            fork
              #1 k = k + 1;
              #2 k = k + 2;
            join_any
            $display("after join_any k=%0d", k);
            #3 $display("end k=%0d", k);
          endtask
          initial go;
        endmodule
    )");
    CHECK(out == "after join_any k=2\nend k=4\n");
}

TEST_CASE("fork: join_none in an assertion action block queues into Reactive") {
    // An action block is lowered as a task body and lives in the Reactive region, so
    // its join_none children have to be queued onto the REACTIVE ring, not the active
    // one (emit_region_push). The action block still observes none of the child's
    // effect -- n is 0 there and 1 once the child has run.
    const std::string out = run_capture(R"(
        module t;
          bit clk = 0;
          int n = 0;
          bit fail = 0;
          always #5 clk = ~clk;
          assert property (@(posedge clk) !fail)
            else begin
              fork
                n = n + 1;
              join_none
              $display("action@%0t n=%0d", $time, n);
            end
          initial begin
            #7 fail = 1;
            #10 $display("done n=%0d", n);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "action@15 n=0\ndone n=1\n");
}

// fork … join_none inside a FUNCTION (LRM 13.4.4: "fork-join_none constructs shall
// be allowed inside a function", and within one the children may hold anything a
// task may). `join` / `join_any` there are illegal SV and slang rejects them.
//
// Ordering is pinned from the LRM (9.3.2 Table 9-1: the children start when the
// calling THREAD next blocks or ends, not when the function returns) rather than
// from iverilog, which rejects a delay in such a child outright and runs delay-free
// ones at the fork. Verilator 5.050 --timing agrees with the values below.
TEST_CASE("fork: join_none in a function schedules children without blocking") {
    const std::string out = run_capture(R"(
        module t;
          int n = 0;
          function void spawn;
            fork
              n = n + 1;
              #5 n = n + 10;
            join_none
            $display("fn done n=%0d", n);
          endfunction
          initial begin
            spawn;
            $display("after call n=%0d@%0t", n, $time);
            #1 $display("mid n=%0d@%0t", n, $time);
            #10 $display("end n=%0d@%0t", n, $time);
          end
        endmodule
    )");
    CHECK(out == "fn done n=0\nafter call n=0@0\nmid n=1@1\nend n=11@11\n");
}

TEST_CASE("fork: join_none in a function called from a task") {
    const std::string out = run_capture(R"(
        module t;
          int n = 0;
          function void spawn;
            fork
              #2 n = n + 1;
            join_none
          endfunction
          task automatic go;
            spawn;
            #1 $display("task n=%0d@%0t", n, $time);
          endtask
          initial begin
            go;
            #5 $display("end n=%0d@%0t", n, $time);
          end
        endmodule
    )");
    CHECK(out == "task n=0@1\nend n=1@6\n");
}

// An `always_comb` IS an always procedure, so 13.4.4 allows it to reach a spawning
// function, and it spawns once per evaluation. The process stays on the thunk path
// (is_simple_always is syntactic on the process body and a Call is not a barrier),
// which is fine: a thunk has the state and wheel it needs and the rings are globals.
TEST_CASE("fork: join_none in a function called from always_comb") {
    const std::string out = run_capture(R"(
        module t;
          int a = 0;
          int n = 0;
          int y;
          function int bump(int x);
            fork
              #1 n = n + 1;
            join_none
            return x + 1;
          endfunction
          always_comb y = bump(a);
          initial begin
            #2 a = 1;
            #5 $display("y=%0d n=%0d@%0t", y, n, $time);
          end
        endmodule
    )");
    CHECK(out == "y=2 n=2@7\n");
}

TEST_CASE("fork: a loop of calls accumulates one live child per call") {
    const std::string out = run_capture(R"(
        module t;
          int n = 0;
          function void spawn;
            fork
              #3 n = n + 1;
            join_none
          endfunction
          initial begin
            for (int i = 0; i < 8; i++) begin
              spawn;
            end
            #1 $display("mid n=%0d", n);
            #10 $display("end n=%0d", n);
          end
        endmodule
    )");
    CHECK(out == "mid n=0\nend n=8\n");
}

// A child spawned inside a function belongs to the CALLING thread (its control
// block is the caller's, LRM 9.6.3's descendant relation), so the caller's
// `wait fork` and `disable fork` reach it with no extra plumbing.
TEST_CASE("fork: wait fork and disable fork reach a child spawned in a function") {
    const std::string waited = run_capture(R"(
        module t;
          int n = 0;
          function void spawn;
            fork
              #1 n = n + 1;
              #3 n = n + 1;
            join_none
          endfunction
          initial begin
            spawn;
            wait fork;
            $display("waited n=%0d@%0t", n, $time);
          end
        endmodule
    )");
    CHECK(waited == "waited n=2@3\n");

    const std::string killed = run_capture(R"(
        module t;
          int n = 0;
          function void spawn;
            fork
              #1 n = n + 1;
              #3 n = n + 1;
            join_none
          endfunction
          initial begin
            spawn;
            #2 disable fork;
            #5 $display("killed n=%0d@%0t", n, $time);
          end
        endmodule
    )");
    CHECK(killed == "killed n=1@7\n");
}

// A function's formals are exactly what LRM 13.4.4's own `watch_for_zero` example
// captures, and the function's frame is gone the moment it returns -- so this is the
// case that made copy-into-the-frame unworkable and the GC box necessary. The child
// runs after the call has returned and still reads the captured handle.
TEST_CASE("fork: a join_none child outlives the function whose local it captured") {
    const std::string out = run_capture(R"(
        module t;
          function void spawn(int k);
            fork
              #1 $display("child k=%0d", k);
            join_none
          endfunction
          initial begin
            spawn(3);
            $display("returned");
            #2 $display("end");
          end
        endmodule
    )");
    CHECK(out == "returned\nchild k=3\nend\n");
}

// 13.4.4's example itself, in the shape clotho can express today: a captured class
// HANDLE formal, read by the child two units after the function returned. The old ABI
// refused this twice over -- a detached child could not capture at all, and a handle
// was not one of the capturable types.
TEST_CASE("fork: a join_none child reads a captured class handle after the call returns") {
    const std::string out = run_capture(R"(
        class IntClass;
          int a = 5;
        endclass
        module t;
          function automatic bit watch(IntClass p);
            fork
              begin #2 $display("child a=%0d", p.a); end
            join_none
            return (p.a == 0);
          endfunction
          IntClass c;
          initial begin
            c = new;
            $display("ret=%0b", watch(c));
            #1 c.a = 9;
            #3 $display("end a=%0d", c.a);
          end
        endmodule
    )");
    CHECK(out == "ret=0\nchild a=9\nend a=9\n");
}

// A child whose ONLY reference to the enclosing activation is a bare call to a sibling
// method. `bump();` resolves against the enclosing `this` (resolve_method_this) without
// naming it, so neither the capture walk nor the box scan saw a reference: the walk
// only recognised `this` through a bare PROPERTY read, and install_capture_boxes
// early-returned on an empty candidate set. The child then lowered a call with no
// receiver and was refused ("class method call without an object handle"), which is
// what uvm_task_phase::execute does with `exec_task(comp, phase)`.
//
// `n` is read through the captured handle, so the assertion also pins that the child
// mutates the SAME object the caller holds, not a copy.
TEST_CASE("fork: a join_none child may call a sibling method on the captured this") {
    const std::string out = run_capture(R"(
        class W;
          int n;
          task automatic bump(); n++; endtask
          virtual function void go();
            fork
              begin #1 bump(); end
            join_none
          endfunction
        endclass
        module t;
          W w;
          initial begin
            w = new;
            w.go();
            $display("after go n=%0d", w.n);
            #2 $display("later n=%0d", w.n);
          end
        endmodule
    )");
    CHECK(out == "after go n=0\nlater n=1\n");
}

// LRM 9.3.2's own idiom, which is the case the whole model turns on. Without an
// explicit `automatic` copy the children share the loop variable, so all three read
// its final value -- iverilog prints `4 4 4` and slang warns about it by name
// (-Wfork-loop-var, "all loop iterations will see the same stale value"). Verilator
// prints `1 2 3`, which is the deviation this deliberately does not copy.
//
// In a process this already held, because a process local gets static signal-like
// storage; the point of the test is that it now holds in a TASK, where the variable
// really is frame-resident and the capture used to be refused outright.
TEST_CASE("fork: join_none children in a task share the loop variable") {
    const std::string out = run_capture(R"(
        module t;
          task automatic go;
            for (int j = 1; j <= 3; ++j)
              fork
                $write("%0d ", j);
              join_none
            #1 $display("| shared");
          endtask
          initial go;
        endmodule
    )");
    CHECK(out == "4 4 4 | shared\n");
}

// LRM 9.3.2's spawn-time initialization, which is the other half of V4:
//
//   "Variables declared in the block_item_declaration of a fork-join block shall be
//    initialized to their initialization value expression whenever execution enters
//    their scope and before any processes are spawned."
//
// The clause's own example, verbatim, and it exercises BOTH declaration positions at
// once -- which is why it is the case to pin. `k` is declared in the FORK's
// block_item_declaration, so it is initialized in the parent at each execution of the
// fork and each child sees its own value ("local copy, k, for each value of j"): 1, 2,
// 3. `m` is declared in a `begin ... end` NESTED in the fork, which is a different
// block, so it is initialized when that block is entered -- when the child body runs,
// after the loop -- and the clause states the answer in a comment of its own: "the
// value of m is 4".
//
// Verilator 5.050 prints `123` for `k`, agreeing on the fork-scope half, and `<m1>
// <m2><m3>` for `m`, which contradicts the clause's own comment; the value below is
// the clause's. iverilog 13 cannot oracle any of this -- it rejects the construct
// outright with "sorry: Overriding the default variable lifetime is not yet
// supported".
TEST_CASE("fork: LRM 9.3.2's own example initializes a fork-scope declaration at the spawn") {
    const std::string out = run_capture(R"(
        module t;
          initial
            for (int j = 1; j <= 3; ++j)
              fork
                automatic int k = j;
                #k $write("%0d", k);
                begin
                  automatic int m = j;
                  #(3 + j) $write("<m%0d>", m);
                end
              join_none
          initial #20 $display("|");
        endmodule
    )");
    CHECK(out == "123<m4><m4><m4>|\n");
}

// The three positions a loop variable can be read from, side by side, because the
// whole clause turns on their being different and a single implementation mistake
// collapses any two of them into one answer. This replaces the refusal that used to
// stand here (the guard read "LRM 9.3.2 initializes it at the spawn"), which refused
// the body-local form as well as the fork-scope one.
//
//   bare `j`        -- a reference to the enclosing automatic IS that one variable
//                      (9.3.2), so every child reads its final value: 444. This is the
//                      deliberate reading recorded in docs/design/fork-join.md and it
//                      is NOT changed by spawn-time initialization.
//   fork-scope `k`  -- the clause's opt-in to a per-iteration copy: 123.
//   body-local `m`  -- a declaration of the CHILD's own block, entered when the child
//                      runs: 444, the clause's "the value of m is 4".
//
// The failure modes this catches, all of which look like a working feature:
// per-ACTIVATION rather than per-ENTRY storage for `k` prints 333 (one box written
// three times, every child reading the last value); treating the fork-scope
// declaration as a child-body-local prints 444; treating a body-local as fork-scope
// prints 123 for `m`, which is what Verilator does.
//
// Measured on Verilator 5.050 --timing: `444|123|123|` -- it agrees on the first two
// and conflates the third.
TEST_CASE("fork: a fork's own declaration, a child block's declaration and a bare capture differ") {
    const std::string out = run_capture(R"(
        module t;
          task automatic bare();
            for (int j = 1; j <= 3; ++j)
              fork
                #4 $write("%0d", j);
              join_none
            #9 $write("|");
          endtask
          task automatic fork_scope();
            for (int j = 1; j <= 3; ++j)
              fork
                automatic int k = j;
                #k $write("%0d", k);
              join_none
            #9 $write("|");
          endtask
          task automatic body_local();
            for (int j = 1; j <= 3; ++j)
              fork
                begin
                  automatic int m = j;
                  #m $write("%0d", m);
                end
              join_none
            #9 $display("|");
          endtask
          initial begin
            bare();
            fork_scope();
            body_local();
          end
        endmodule
    )");
    CHECK(out == "444|123|444|\n");
}

// The idiom Accellera UVM uses in three critical paths -- uvm_phase_hopper.svh:381,
// uvm_objection.svh:818 (a LABELLED fork) and uvm_sequencer_base.svh:855 -- reduced to
// its shape: a class-handle-typed fork-scope declaration copied from a formal, with the
// parent overwriting the source before either child runs. Each child must see the
// handle its own spawn captured.
//
// The label is the part an implementation misses silently: `fork : guard` carries its
// children as the body's top-level statements exactly as a bare fork does, so keying
// the split on blockKind (is_fork_block) is what keeps both on one path. Verilator
// 5.050 agrees with every line below.
TEST_CASE("fork: a class-handle fork-scope declaration captures the handle at the spawn") {
    const std::string out = run_capture(R"(
        class ph_t;
          int id;
          function new(int i); id = i; endfunction
        endclass
        module t;
          ph_t src;
          task automatic plain(ph_t p);
            fork
              automatic ph_t phase = p;
              begin #1 $display("plain id=%0d", phase.id); end
            join_none
          endtask
          task automatic labelled(ph_t p);
            fork : guard
              automatic ph_t phase = p;
              begin #2 $display("labelled id=%0d", phase.id); end
            join_none
          endtask
          initial begin
            src = new(7);
            plain(src);
            src = new(8);
            labelled(src);
            src = new(99);
            $display("parent overwrote src to %0d", src.id);
            #5 $display("|");
          end
        endmodule
    )");
    CHECK(out ==
          "parent overwrote src to 99\n"
          "plain id=7\n"
          "labelled id=8\n"
          "|\n");
}

// Everything a fork-scope initializer is allowed to read, in one fork. The last two
// are the clause's own carve-out:
//
//   "Within a fork-join_any or fork-join_none block, it shall be illegal to refer to
//    formal arguments passed by reference other than in the initialization value
//    expressions of variables declared in a block_item_declaration of the fork"
//
// so an `output` / `ref` formal is legal HERE and nowhere else in a detaching fork.
// It is implementable for the same reason it is legal: the initializer is evaluated in
// the PARENT at the fork, so no child-side access to the formal exists and the
// copy-out that a captured formal would lose is never in question. The refusal for a
// child BODY reading such a formal is unaffected -- see the rejection test below.
// Verilator 5.050 agrees with both lines.
TEST_CASE("fork: a fork-scope initializer reads signals, locals, formals, ref formals and this") {
    const std::string out = run_capture(R"(
        module t;
          int sig = 5;
          class C;
            int p = 3;
            task automatic go();
              fork
                automatic int fromthis = p;
                begin #4 $display("this p=%0d", fromthis); end
              join_none
            endtask
          endclass
          task automatic srcs(int inarg, output int o, ref int r);
            int loc = 20;
            o = 30;
            fork
              automatic int a = sig;
              automatic int b = loc;
              automatic int c = inarg;
              automatic int d = o;
              automatic int e = r;
              begin #3 $display("a=%0d b=%0d c=%0d d=%0d e=%0d", a, b, c, d, e); end
            join_none
          endtask
          C  cc;
          int oo, rr;
          initial begin
            cc = new();
            cc.go();
            rr = 40;
            srcs(10, oo, rr);
            sig = 0; rr = 0;
            #9 $display("|");
          end
        endmodule
    )");
    CHECK(out ==
          "a=5 b=20 c=10 d=30 e=40\n"
          "this p=3\n"
          "|\n");
}

// "Before any processes are spawned", made observable with a side effect. The
// initializer is a function call, so the count says both how many times it ran and
// when: three fork executions produce exactly three calls, and the parent already sees
// all three before any child body has run.
TEST_CASE("fork: a fork-scope initializer runs once per fork execution, in the parent") {
    const std::string out = run_capture(R"(
        module t;
          int calls = 0;
          function automatic int bump();
            calls++;
            return calls;
          endfunction
          task automatic go();
            for (int i = 0; i < 3; i++)
              fork
                automatic int seq = bump();
                begin #(seq) $display("child seq=%0d", seq); end
              join_none
            $display("parent after 3 forks: calls=%0d", calls);
          endtask
          initial begin
            go();
            #9 $display("|");
          end
        endmodule
    )");
    CHECK(out ==
          "parent after 3 forks: calls=3\n"
          "child seq=1\n"
          "child seq=2\n"
          "child seq=3\n"
          "|\n");
}

// One variable per ENTRY, shared by every child of that entry -- not a per-child copy.
// A write by one child is visible to the next, in both directions, which is what makes
// the box a variable rather than a value passed at the spawn.
//
// Second half: several declarations in one fork, a later initializer reading an earlier
// one. Declaration order decides it, per the clause's "whenever execution enters their
// scope": each box is registered and filled before the next declaration is reached, so
// `b = a + 1` reads the `a` just written.
TEST_CASE("fork: one shared variable per fork entry, and declarations initialize in order") {
    const std::string out = run_capture(R"(
        module t;
          task automatic sharing();
            fork
              automatic int s = 1;
              begin #1 s = 42; end
              begin #2 $display("sibling sees s=%0d", s); end
              begin #3 s = s + 1; end
              begin #4 $display("finally s=%0d", s); end
            join_none
          endtask
          task automatic multi(int j);
            fork
              automatic int a = j;
              automatic int b = a + 1;
              automatic int c = a + b;
              begin #5 $display("a=%0d b=%0d c=%0d", a, b, c); end
            join_none
          endtask
          initial begin
            sharing();
            multi(5);
            #9 $display("|");
          end
        endmodule
    )");
    CHECK(out ==
          "sibling sees s=42\n"
          "finally s=43\n"
          "a=5 b=6 c=11\n"
          "|\n");
}

// All three join kinds carry a fork-scope declaration, and the last case keeps V4's
// lifetime property: `outlive`'s activation has returned by the time its child reads
// `k`, so the box has to outlive the frame that made it. Verilator 5.050 agrees with
// every line, timestamps included.
TEST_CASE("fork: a fork-scope declaration works under join, join_any and join_none") {
    const std::string out = run_capture(R"(
        module t;
          task automatic j_none(int v);
            fork
              automatic int k = v;
              begin #1 $display("join_none k=%0d", k); end
            join_none
            $display("join_none parent returned");
          endtask
          task automatic j_any(int v);
            fork
              automatic int k = v;
              begin #2 $display("join_any first k=%0d", k); end
              begin #6 $display("join_any second k=%0d", k); end
            join_any
            $display("join_any parent resumed at %0t", $time);
          endtask
          task automatic j_all(int v);
            fork
              automatic int k = v;
              begin #1 $display("join a k=%0d", k); end
              begin #2 $display("join b k=%0d", k); end
            join
            $display("join parent resumed at %0t", $time);
          endtask
          task automatic outlive(int v);
            fork
              automatic int k = v;
              begin #3 $display("outlive k=%0d", k); end
            join_none
          endtask
          initial begin
            j_none(11);
            #5 j_any(22);
            #10 j_all(33);
            #5 outlive(44);
            $display("outlive caller returned at %0t", $time);
            #10 $display("|");
          end
        endmodule
    )");
    CHECK(out ==
          "join_none parent returned\n"
          "join_none k=11\n"
          "join_any first k=22\n"
          "join_any parent resumed at 7\n"
          "join_any second k=22\n"
          "join a k=33\n"
          "join b k=33\n"
          "join parent resumed at 19\n"
          "outlive caller returned at 24\n"
          "outlive k=44\n"
          "|\n");
}

// The two gaps around spawn-time initialization that are still refused rather than
// answered. The first is the clause's carve-out read at its exact width: legal in an
// initializer, still illegal in a child body, where the copy-out of an output formal
// would lose whatever the child wrote through the box.
TEST_CASE("fork: a child body reading an output formal, and a static fork declaration, are rejected") {
    check_rejected(R"(
        module t;
          task automatic go(output int o);
            fork
              begin #1 o = 5; end
            join_none
          endtask
          int oo;
          initial go(oo);
        endmodule
    )",
                   "capturing an output or inout formal would lose");

    check_rejected(R"(
        module t;
          task automatic go();
            fork
              static int k = 1;
              begin #1 $display("k=%0d", k); end
            join_none
          endtask
          initial go();
        endmodule
    )",
                   "initializes a fork's declarations on every entry to its scope");
}

// LRM 13.4.4 second paragraph: the calling thread must be created by an initial or
// always procedure, and an implementation "shall issue an error either at compile
// time or run time" otherwise. Each of these accepted the spawn silently before the
// check landed; the `final` one also leaked the record and the child frame.
TEST_CASE("fork: a spawning function called from a non-procedural context is rejected") {
    const auto spawner = R"(
          int n = 0;
          function int spawn;
            fork
              #1 n = n + 1;
            join_none
            return 1;
          endfunction
    )";

    check_rejected(std::string("module t;") + spawner + R"(
          int y = spawn();
        endmodule
    )",
                   "called from a static variable initializer");

    check_rejected(std::string("module t;") + spawner + R"(
          logic [31:0] z;
          assign z = spawn();
        endmodule
    )",
                   "called from a continuous assignment");

    check_rejected(std::string("module t;") + spawner + R"(
          final begin
            void'(spawn());
          end
        endmodule
    )",
                   "called from a `final` block");

    check_rejected(std::string("module t;") + spawner + R"(
          initial $strobe("y=%0d", spawn());
        endmodule
    )",
                   "called from a $strobe / $monitor body");

    check_rejected(std::string("module t;") + spawner + R"(
          export "DPI-C" function spawn;
        endmodule
    )",
                   "called from a DPI export");
}
