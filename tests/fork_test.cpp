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

// Unsupported variants fail loudly (never silently serialized).
TEST_CASE("fork: join_any / join_none rejected") {
    check_rejected(R"(
        module t; initial fork #5 $display("x"); #6 $display("y"); join_any endmodule
    )",
                   "join_any");
    check_rejected(R"(
        module t; initial fork #5 $display("x"); #6 $display("y"); join_none endmodule
    )",
                   "join_none");
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

// The two dynamic process-control statements (LRM 9.6.1 / 9.6.3) both need the
// live-child set fork V2 owes — `disable fork` kills every descendant, `wait
// fork` blocks on them — and the join-all count cannot express either. Refused
// with their own names, not the generic "unsupported statement".
TEST_CASE("fork: disable fork / wait fork rejected") {
    check_rejected(R"(
        module t;
          initial begin
            fork #1 $display("x"); join
            disable fork;
          end
        endmodule
    )",
                   "disable fork is not supported");
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
