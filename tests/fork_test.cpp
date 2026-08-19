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

// What capture cannot carry. A ref formal is a pointer to a packed slot, so the
// captures are exactly the ref ABI's own repertoire (docs/design/ref-args.md);
// anything else is named rather than mis-lowered, and both lift together.
TEST_CASE("fork: a child reading an uncapturable task local is rejected by name") {
    check_rejected(R"(
        module t;
          task automatic show;
            string s = "hi";
            fork
              #1 $display("%s", s);
            join
          endtask
          initial show;
        endmodule
    )",
                   "reads the enclosing task's 's' is not supported");
    check_rejected(R"(
        module t;
          class C;
            int v;
            task go(); fork #1 v = 5; join endtask
          endclass
          initial begin
            C c;
            c = new();
            c.go();
          end
        endmodule
    )",
                   "reads a class property of the enclosing method is not supported");
}

// `wait fork` (LRM 9.6.1) still needs the barrier over the live descendant set;
// `disable fork` (9.6.3) has landed with the `process` control blocks that carry
// that set (tests/process_test.cpp, docs/design/process-class.md §8). Refused with
// its own name, not the generic "unsupported statement".
TEST_CASE("fork: wait fork rejected") {
    check_rejected(R"(
        module t;
          initial begin
            fork #1 $display("x"); join
            wait fork;
          end
        endmodule
    )",
                   "wait fork is not supported");
}

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

// The ref-capture ABI hands a child a raw pointer into the enclosing frame, which
// needs that frame to outlive every child. `join` guarantees it; the detaching
// kinds do not, so a capture is refused by name rather than dangling. Lifts with
// V4's per-spawn copies (docs/design/fork-join.md).
TEST_CASE("fork: a join_any / join_none child capturing a task local is rejected") {
    check_rejected(R"(
        module t;
          task automatic go;
            int k = 1;
            fork
              #1 k = k + 1;
            join_none
          endtask
          initial go;
        endmodule
    )",
                   "fork..join_none child that reads the enclosing task's 'k' is not supported");
    check_rejected(R"(
        module t;
          task automatic go;
            int k = 1;
            fork
              #1 k = k + 1;
              #2 k = k + 2;
            join_any
          endtask
          initial go;
        endmodule
    )",
                   "fork..join_any child that reads the enclosing task's 'k' is not supported");
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
