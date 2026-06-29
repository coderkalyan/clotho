#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and return everything it wrote to stdout. Tasks under test are
// driven from `initial` blocks that report with $display, so the captured text is
// the observable behavior (mirrors function_test.cpp). Behavior was cross-checked
// against Icarus Verilog while developing these cases.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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

// Lower and return the rendered diagnostics; ok must be false (a rejected design).
std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

// A time-consuming void task blocks its caller: control returns only after the
// task's delays elapse, and $time advances across them (LRM 13.3).
TEST_CASE("task consumes time and blocks the caller") {
    constexpr std::string_view src = R"(
        module m;
            logic clk;
            task automatic toggle(input int n);
                for (int i = 0; i < n; i++)
                    #5 clk = ~clk;
            endtask
            initial begin
                clk = 0;
                toggle(4);
                $display("done t=%0d clk=%b", $time, clk);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "done t=20 clk=0\n");
}

// A task that consumes no time completes synchronously on the caller's stack
// (the fast path: resume, see `done`, continue inline) and still copies outputs.
TEST_CASE("zero-time task completes synchronously") {
    constexpr std::string_view src = R"(
        module m;
            task automatic addone(input int a, output int b);
                b = a + 1;
            endtask
            int r;
            initial begin
                addone(41, r);
                addone(r, r);
                $display("r=%0d", r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "r=43\n");
}

// An output argument is copied back to the caller at completion, even when the
// task blocked in between (LRM 13.5).
TEST_CASE("task output argument copies back at completion") {
    constexpr std::string_view src = R"(
        module m;
            task automatic count(input int n, output int total);
                total = 0;
                for (int i = 0; i < n; i++)
                    #1 total = total + 1;
            endtask
            int c;
            initial begin
                c = 99;
                count(3, c);
                $display("c=%0d t=%0d", c, $time);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "c=3 t=3\n");
}

// An inout argument passes the caller's value in and the mutated value back out.
TEST_CASE("task inout argument round-trips") {
    constexpr std::string_view src = R"(
        module m;
            task automatic bump(inout int v, input int by);
                #1 v = v + by;
            endtask
            int x;
            initial begin
                x = 10;
                bump(x, 5);
                $display("x=%0d t=%0d", x, $time);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "x=15 t=1\n");
}

// A task blocks on @(posedge): the caller resumes only after the edge fires.
TEST_CASE("task blocks on an event control") {
    constexpr std::string_view src = R"(
        module m;
            logic clk;
            task automatic wait_n(input int k);
                for (int i = 0; i < k; i++) @(posedge clk);
            endtask
            initial begin
                clk = 0;
                wait_n(3);
                $display("woke t=%0d", $time);
            end
            initial #100 $finish;
            initial forever #5 clk = ~clk;
        endmodule
    )";
    // posedges at t=5,15,25; the third wakes the caller.
    CHECK(run_capture(src) == "woke t=25\n");
}

// A task may enable other tasks; control returns only after all nested tasks
// complete (LRM 13.3).
TEST_CASE("nested task enables complete before the caller resumes") {
    constexpr std::string_view src = R"(
        module m;
            task automatic inner(input int d);
                #(d) ;
            endtask
            task automatic outer(output int t);
                inner(3);
                inner(4);
                t = 1;
            endtask
            int done;
            initial begin
                outer(done);
                $display("done=%0d t=%0d", done, $time);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "done=1 t=7\n");
}

// A task recurses across a delay: each invocation gets its own frame, so the
// recurrence computes correctly (fib) despite the time consumed.
TEST_CASE("task recursion with timing keeps independent frames") {
    constexpr std::string_view src = R"(
        module m;
            task automatic fib(input int n, output int r);
                int a, b;
                if (n < 2) begin r = n; return; end
                fib(n - 1, a);
                fib(n - 2, b);
                #1 r = a + b;
            endtask
            int f;
            initial begin
                fib(7, f);
                $display("fib7=%0d", f);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "fib7=13\n");
}

// Two processes calling the same automatic task concurrently each get private
// local storage (LRM 13.3.2).
TEST_CASE("concurrent automatic task invocations keep independent state") {
    constexpr std::string_view src = R"(
        module m;
            task automatic accum(input int id, input int n);
                int s;
                s = 0;
                for (int i = 0; i < n; i++)
                    #1 s = s + 1;
                $display("id=%0d s=%0d", id, s);
            endtask
            initial accum(1, 3);
            initial accum(2, 5);
            initial #100 $finish;
        endmodule
    )";
    // id=1 finishes first (t=3) with its own sum, id=2 later (t=5).
    CHECK(run_capture(src) == "id=1 s=3\nid=2 s=5\n");
}

// An early `return` still copies output arguments out (the copy-out runs on every
// return path).
TEST_CASE("early return copies outputs out") {
    constexpr std::string_view src = R"(
        module m;
            task automatic er(input int n, output int r);
                if (n == 0) begin #2 return; end
                #1 r = 99;
            endtask
            int a;
            initial begin
                a = 7;
                er(0, a);
                $display("a=%0d t=%0d", a, $time);
            end
        endmodule
    )";
    // n==0 returns early before assigning r, so the output defaults to 0.
    CHECK(run_capture(src) == "a=0 t=2\n");
}

// $time is readable inside a task (it carries the wheel, unlike a function).
TEST_CASE("$time inside a task reads the live clock") {
    constexpr std::string_view src = R"(
        module m;
            task automatic report;
                #7 $display("t=%0d", $time);
            endtask
            initial report;
        endmodule
    )";
    CHECK(run_capture(src) == "t=7\n");
}

// A static task retains its locals between (non-overlapping) calls (LRM 13.3.2).
TEST_CASE("static task locals persist between calls") {
    constexpr std::string_view src = R"(
        module m;
            task tick(output int v);
                static int n;
                n = n + 1;
                v = n;
            endtask
            int a, b, c;
            initial begin
                tick(a);
                tick(b);
                tick(c);
                $display("%0d %0d %0d", a, b, c);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 3\n");
}

// -- Guarded gaps: must fail loudly, not silently miscompile. --

TEST_CASE("ref argument is rejected") {
    constexpr std::string_view src = R"(
        module m;
            int x;
            task automatic t(ref int a);
                a = 1;
            endtask
            initial t(x);
        endmodule
    )";
    CHECK(reject(src).find("ref arguments") != std::string::npos);
}

TEST_CASE("disable inside a task is rejected") {
    constexpr std::string_view src = R"(
        module m;
            int x;
            task t;
                #5 disable t;
            endtask
            initial t;
        endmodule
    )";
    CHECK(reject(src).find("disable is not supported") != std::string::npos);
}

// A nonblocking assignment to a module signal from inside a task stages into the
// signal's shadow and commits in the caller instant's NBA region: the old value is
// still visible immediately after the call, the new value one delta later (LRM
// 13.3 / 10.4.2). Cross-checked against Icarus Verilog.
TEST_CASE("nonblocking assignment to a module signal in a task defers") {
    constexpr std::string_view src = R"(
        module m;
            reg [7:0] q;
            reg [7:0] seen;
            task set_q(input [7:0] v);
                begin
                    q <= v;
                    seen = q;
                end
            endtask
            initial begin
                q = 8'd10;
                #1 set_q(8'd99);
                $display("a q=%0d seen=%0d", q, seen);
                #1 $display("b q=%0d", q);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a q=10 seen=10\nb q=99\n");
}

TEST_CASE("nonblocking assignment in a function is rejected") {
    constexpr std::string_view src = R"(
        module m;
            logic x;
            function void f;
                x <= 1;
            endfunction
            initial f();
        endmodule
    )";
    CHECK(reject(src).find("nonblocking assignment in a function") != std::string::npos);
}

TEST_CASE("nonblocking assignment to a task output argument is rejected") {
    constexpr std::string_view src = R"(
        module m;
            reg [7:0] r;
            task t(output [7:0] o);
                o <= 8'd7;
            endtask
            initial t(r);
        endmodule
    )";
    CHECK(reject(src).find("task-local variable") != std::string::npos);
}

// A nonblocking write to an unpacked-array element from a task defers through the
// global runtime queue, just as it does from a process: a runtime-indexed element
// plus byte-lane part-select (the picorv32 AXI write pattern) commits one delta
// later. Cross-checked against Icarus Verilog.
TEST_CASE("nonblocking write to an array element in a task defers") {
    constexpr std::string_view src = R"(
        module m;
            reg [31:0] mem [0:1];
            task wr(input i, input [31:0] v, input [3:0] strb);
                begin
                    if (strb[0]) mem[i][ 7: 0] <= v[ 7: 0];
                    if (strb[1]) mem[i][15: 8] <= v[15: 8];
                    if (strb[2]) mem[i][23:16] <= v[23:16];
                    if (strb[3]) mem[i][31:24] <= v[31:24];
                end
            endtask
            initial begin
                mem[0] = 32'h0;
                #1 wr(0, 32'hDEADBEEF, 4'b1010);
                $display("a %08x", mem[0]);
                #1 $display("b %08x", mem[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a 00000000\nb de00be00\n");
}
