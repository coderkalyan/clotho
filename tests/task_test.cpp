#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and return everything it wrote to stdout. Tasks under test are
// driven from `initial` blocks that report with $display, so the captured text is
// the observable behavior (mirrors function_test.cpp). Behavior was cross-checked
// against Icarus Verilog while developing these cases.
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

// Lower and return the rendered diagnostics; ok must be false (a rejected design).
std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostic_text();
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

// A ref formal aliases the actual's live storage (LRM 13.5.2): a function writes
// the caller's variable directly, with no copy-out snapshot. (iverilog rejects
// ref ports: "Reference ports not supported yet".)
TEST_CASE("function ref mutates the caller variable") {
    constexpr std::string_view src = R"(
        module m;
            function automatic void addone(ref int x);
                x = x + 1;
            endfunction
            int p, q;
            initial begin
                p = 40; addone(p); $display("p=%0d", p);
                q = 5; addone(q); addone(q); $display("q=%0d", q);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "p=41\nq=7\n");
}

// Two ref formals aliasing distinct variables resolve independently, and the
// callee's reads observe each other's writes within the body (single live cell).
TEST_CASE("function ref aliasing across two formals") {
    constexpr std::string_view src = R"(
        module m;
            function automatic void accum(ref int a, ref int b);
                a = a + b;
                b = a - b;
            endfunction
            int p, q;
            initial begin
                p = 7; q = 3; accum(p, q); $display("p=%0d q=%0d", p, q);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "p=10 q=7\n");
}

// A const ref reads the actual live; a 4-state actual carries x through the
// pointer (arithmetic with any x operand is all-x, LRM 11.4.3), and the 2-state
// int return coerces x to 0 (LRM 6.3.1).
TEST_CASE("const ref reads the actual, x propagates") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int rd(const ref logic [7:0] v);
                return v + 8'd3;
            endfunction
            logic [7:0] w;
            initial begin
                w = 8'd100; $display("rd=%0d", rd(w));
                w = 8'bxxxx0000; $display("xref=%0d", rd(w));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "rd=103\nxref=0\n");
}

// A task ref is live across a #delay: the pre-delay write is observed by a
// concurrent process mid-suspend, and the post-delay write lands in the caller.
// This is the case a copy-in/copy-out snapshot would get wrong (it would show the
// mid value as the caller's stale copy).
TEST_CASE("task ref observed live across a #delay") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] shared;
            task automatic bump(ref logic [7:0] r);
                r = r + 8'd1;
                #5 r = r + 8'd10;
            endtask
            initial begin
                shared = 8'd0;
                bump(shared);
                $display("after=%0d", shared);
            end
            initial begin
                #1 $display("mid=%0d", shared);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "mid=1\nafter=11\n");
}

// A blocking write through a ref formal is a write to the actual, so it must
// service the actual signal's wait queue — an `always @(sig)` woken solely by a
// ref write fires. Serviced at the call site (a function is atomic).
TEST_CASE("ref write wakes a process sensitive to the actual") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] g;
            logic [7:0] mon;
            int hits;
            function automatic void gset(ref logic [7:0] r, input logic [7:0] val);
                r = val;
            endfunction
            always @(g) begin mon = g; hits = hits + 1; end
            initial begin
                hits = 0; mon = 8'd0;
                #1 gset(g, 8'h11);
                #1 $display("mon=%h hits=%0d", mon, hits);
                gset(g, 8'h22);
                #1 $display("mon=%h hits=%0d", mon, hits);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "mon=11 hits=1\nmon=22 hits=2\n");
}

// An unpacked-array element is a whole `{i(2W),iW}` slot, so it is a legal ref
// actual (LRM 13.5.2): the callee aliases that element's storage. Const and
// dynamic index, nested dimensions, and a task ref across a #delay.
TEST_CASE("ref to an unpacked-array element") {
    constexpr std::string_view src = R"(
        module m;
            function automatic void addone(ref int x); x = x + 1; endfunction
            function automatic void setb(ref logic [3:0] b); b = b + 4'd7; endfunction
            task automatic bump(ref int r);
                r = r + 1;
                #5 r = r + 10;
            endtask
            int a[4];
            logic [3:0] pa [0:3];
            int m2 [2][2];
            initial begin
                a[0]=100; a[1]=200; a[2]=0; a[3]=400;
                addone(a[1]); $display("a1=%0d", a[1]);
                for (int i = 0; i < 4; i++) begin int k; k = i; addone(a[k]); end
                $display("a=%0d %0d %0d %0d", a[0], a[1], a[2], a[3]);
                pa[2] = 4'd2; setb(pa[2]); $display("pa2=%0d", pa[2]);
                m2[1][0] = 5; addone(m2[1][0]); $display("m2=%0d", m2[1][0]);
                a[2] = 50; bump(a[2]); $display("a2=%0d", a[2]);
            end
            initial begin #1 $display("mid=%0d", a[2]); end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "a1=201\na=101 202 1 401\npa2=9\nm2=6\nmid=51\na2=61\n");
}

// `ref` binds every storage class through the same fat pointer
// (docs/design/data-model.md §3), so a non-integral formal aliases its actual
// exactly as a scalar one does: an unpacked array by its canonical image, a
// string and a container by their handles; iverilog rejects ref ports outright.
TEST_CASE("ref of an unpacked array aliases the actual") {
    constexpr std::string_view src = R"(
        module m;
            function automatic void f(ref int a[4]);
                a[0] = 1;
                a[3] = a[1] + 40;
            endfunction
            int x[4];
            initial begin
                x[0] = 9; x[1] = 2; x[2] = 3; x[3] = 4;
                f(x);
                $display("%0d %0d %0d %0d", x[0], x[1], x[2], x[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 3 42\n");
}

TEST_CASE("ref of a string aliases the actual") {
    constexpr std::string_view src = R"(
        module m;
            function automatic void f(ref string s);
                s = {s, " world"};
            endfunction
            string t;
            initial begin
                t = "hello";
                f(t);
                $display("%s", t);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "hello world\n");
}

TEST_CASE("ref of a queue aliases the actual, mutations included") {
    constexpr std::string_view src = R"(
        module m;
            task automatic push(ref int q[$], input int v);
                q.push_back(v);
            endtask
            int q[$];
            initial begin
                q.push_back(1);
                push(q, 2);
                push(q, 3);
                $display("size=%0d last=%0d", q.size(), q[$]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "size=3 last=3\n");
}

TEST_CASE("ref of an unpacked struct aliases the actual") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int x; string s; } s_t;
            function automatic void bump(ref s_t v);
                v.x = v.x + 1;
                v.s = "set";
            endfunction
            s_t a;
            initial begin
                a.x = 41;
                bump(a);
                $display("%0d %s", a.x, a.s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "42 set\n");
}

// -- Guarded gaps: must fail loudly, not silently miscompile. --

// An event is a 64-bit handle (docs/design/event-handles.md), so it passes like any
// other scalar in every direction. LRM 15.5.5's own example is a task that triggers
// the caller's event through an `input` formal; `ref` retargets the caller's
// variable, as it does for a class handle.
TEST_CASE("an event passes as a subroutine argument in every direction") {
    SUBCASE("input: the callee triggers the caller's event") {
        CHECK(run_capture(R"(
            module m;
              event e; int n = 0;
              task automatic fire(event x); -> x; endtask
              initial forever begin @(e); n++; end
              initial begin #1 fire(e); #1 fire(e); #1 $display("n=%0d", n); end
            endmodule
        )") == "n=2\n");
    }
    SUBCASE("ref: the callee retargets the caller's variable") {
        CHECK(run_capture(R"(
            module m;
              event a, b; int nb = 0;
              task automatic point_at_b(ref event x); x = b; endtask
              initial forever begin @(b); nb++; end
              initial begin point_at_b(a); #1 -> a; #1 $display("nb=%0d", nb); end
            endmodule
        )") == "nb=1\n");
    }
    SUBCASE("output: the callee hands back a binding") {
        CHECK(run_capture(R"(
            module m;
              event src, got; int n = 0;
              task automatic give(output event x); x = src; endtask
              initial forever begin @(src); n++; end
              initial begin give(got); #1 -> got; #1 $display("n=%0d", n); end
            endmodule
        )") == "n=1\n");
    }
}

// The one event-argument form still missing: a WaitEdge names a signal, and an
// event held in a call frame has none. Triggering through a formal works.
TEST_CASE("waiting on an event held in a subroutine argument is rejected") {
    CHECK(reject("module m;\n"
                 "    event e;\n"
                 "    task automatic w(event x); @(x); endtask\n"
                 "    initial w(e);\n"
                 "endmodule\n")
            .find("waiting on an event held in a subroutine argument") != std::string::npos);
}

// A packed bit/part-select is not a whole slot, so it is not a legal ref actual —
// slang itself rejects it ("only variables, class properties, and members of
// unpacked structs and arrays are allowed"), before Clotho's own guard.
TEST_CASE("ref actual that is a packed part-select is rejected") {
    constexpr std::string_view src = R"(
        module m;
            function automatic void f(ref logic [3:0] n); n = 4'd5; endfunction
            logic [7:0] v;
            initial f(v[3:0]);
        endmodule
    )";
    CHECK_FALSE(lower_text(src).ok);
}

// A task target ends EVERY activation of the task (LRM 9.6.2), whatever names it,
// and each caller resumes at the statement after its task-enabling statement —
// which is exactly what the ordinary epilogue already does, so the abort takes that
// epilogue with copy-out skipped. Oracle: iverilog.
TEST_CASE("disable of a task ends its activation and resumes the caller") {
    CHECK(run_capture(R"(
        module m;
            task slow;
                #100 $display("never at %0t", $time);
            endtask
            initial begin
                slow;
                $display("after slow at %0t", $time);
            end
            initial #5 disable slow;
        endmodule
    )") == "after slow at 5\n");
}

// LRM 9.6.2 example 4: a task disabling ITSELF. The activation running the
// statement is one of the ones that has to end, and it is the only one the
// force-wake cannot reach — it is running, not parked — so the frontend follows the
// mark with an explicit abort.
TEST_CASE("a task disabling itself ends that activation and returns") {
    CHECK(run_capture(R"(
        module m;
            task selfkill;
                #5 disable selfkill;
                $display("never at %0t", $time);
            endtask
            initial begin
                selfkill();
                $display("returned at %0t", $time);
            end
        endmodule
    )") == "returned at 5\n");
}

// "If a task is enabled more than once, then disabling such a task shall disable
// all activations" (LRM 9.6.2). Two concurrent activations, one `disable`, and both
// callers resume. The two resumptions land in the same instant, whose order the LRM
// leaves unspecified, so the two callers are told apart by time rather than by
// print order.
TEST_CASE("disable of a task ends every activation of it") {
    CHECK(run_capture(R"(
        module m;
            task slow(input integer id);
                #100 $display("never %0d", id);
            endtask
            initial begin
                slow(1);
                #1 $display("caller 1 back, now %0t", $time);
            end
            initial begin
                #1 slow(2);
                #2 $display("caller 2 back, now %0t", $time);
            end
            initial #5 disable slow;
        endmodule
    )") == "caller 1 back, now 6\ncaller 2 back, now 7\n");
}

// No live activation means no effect, for a task exactly as for a block: the list
// is empty, so the walk touches nothing.
TEST_CASE("disable of a task that is not running has no effect") {
    CHECK(run_capture(R"(
        module m;
            task later;
                #10 $display("later ran at %0t", $time);
            endtask
            initial #5 disable later;
            initial #20 later;
        endmodule
    )") == "later ran at 30\n");
}

// Still refused: a task whose body can park where the abort cannot claim the park.
TEST_CASE("disable of a task that parks on a nested call is rejected") {
    constexpr std::string_view nested = R"(
        module m;
            task inner; #5; endtask
            task outer; inner(); endtask
            initial outer;
            initial #1 disable outer;
        endmodule
    )";
    CHECK(reject(nested).find("cannot claim that park") != std::string::npos);
}

// A named block inside a task is not single-activation, so a `disable` naming it
// marks every activation on the block's list and then takes the ordinary structured
// exit out of the one running. The block still behaves as a labelled break for that
// activation, which is what the idiom is for. Oracle: iverilog.
TEST_CASE("disable of an enclosing block inside a task leaves that block") {
    CHECK(run_capture(R"(
        module m;
            task t;
                begin : blk
                    #5 disable blk;
                    $display("never");
                end
                $display("t done at %0t", $time);
            endtask
            initial t;
        endmodule
    )") == "t done at 5\n");
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

// A nonblocking assignment to a module signal from inside a function is legal
// (LRM 13.4.4: "nonblocking assignments ... shall be allowed inside a function")
// and stages into the signal's shadow exactly like a task: the function returns
// immediately with the old value still live, the new value commits one delta
// later. iverilog rejects function NBA — a stale Verilog-2001 constraint the
// LRM lifted. Old q=0 mem[1]=0 right after the call; committed q=5 mem[1]=6
// after #1.
TEST_CASE("nonblocking assignment to a module signal in a function defers") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] q, mem [0:3];
            function automatic void f(input logic [7:0] v);
                q <= v;
                mem[1] <= v + 8'd1;
            endfunction
            initial begin
                q = 8'd0; mem[1] = 8'd0;
                f(8'd5);
                $display("t0 q=%0d mem1=%0d", q, mem[1]);
                #1 $display("t1 q=%0d mem1=%0d", q, mem[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "t0 q=0 mem1=0\nt1 q=5 mem1=6\n");
}

// The same signal NBA'd from a function in two instances stages into each
// instance's own shadow (per-instance ctx / def-relative block id): both counters
// advance independently.
TEST_CASE("nonblocking assignment from a function is per-instance") {
    constexpr std::string_view src = R"(
        module sub(input logic clk);
            logic [7:0] q;
            function automatic void bump(input logic [7:0] v); q <= v; endfunction
            initial q = 0;
            always @(posedge clk) bump(q + 8'd1);
        endmodule
        module m;
            logic clk = 0;
            sub a(clk); sub b(clk);
            always #1 clk = ~clk;
            initial begin
                repeat (6) @(posedge clk);
                #1 $display("a.q=%0d b.q=%0d", a.q, b.q);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a.q=6 b.q=6\n");
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
    CHECK(reject(src).find("subroutine-local variable") != std::string::npos);
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

TEST_CASE("a task with a 64-bit input formal does not overflow its frame") {
    // Regression: an i128 must|may plane spilled into the coroutine frame made
    // CoroSplit lay out fields past llvm.coro.size; the allocation must cover
    // the over-alignment slack (coro.size + coro.align) or the heap corrupts.
    const std::string out = run_capture(R"(
        module t;
            task automatic show(input reg [63:0] in);
                $display("%0d", in);
            endtask
            initial show(64'd42);
        endmodule
    )");
    CHECK(out == "42\n");
}

// LRM 3.14.2.3: a delay inside a subroutine is expressed in the time units of
// the scope that DECLARES the subroutine, never the one that calls it. The body
// is lowered once and shared by every call site, so scaling it by the caller
// would leave at most one caller correct -- here the package task's `#5` is 5 ns
// even though the only caller measures time in microseconds. `$time` read in the
// body follows the same scope, and the caller's own `$time` still reports in the
// caller's units (5 ns rounds to 0 us). Cross-checked against Icarus Verilog.
TEST_CASE("a package task's delay and $time use the declaring scope's timescale") {
    const std::string out = run_capture(R"(`timescale 1ns/1ns
        package p;
            task automatic w5; #5; $display("task %0d", $time); endtask
        endpackage
        `timescale 1us/1ns
        module m;
            import p::*;
            initial begin w5(); $display("caller %0d", $time); end
        endmodule
    )");
    CHECK(out == "task 5\ncaller 0\n");
}

// The same package task called from two modules with different timescales. Both
// wake at the same absolute instant (5 ns), and each caller reports it in its
// own units -- 0 in microseconds, 5000 in picoseconds. This is the case the
// caller-scaled lowering could not express at all.
TEST_CASE("one package task serves callers of different timescales") {
    const std::string out = run_capture(R"(`timescale 1ns/1ns
        package p;
            task automatic w5; #5; endtask
        endpackage
        `timescale 1us/1ns
        module mu; import p::*; initial begin w5(); $display("us %0d", $time); end endmodule
        `timescale 1ps/1ps
        module mp; import p::*; initial begin w5(); $display("ps %0d", $time); end endmodule
    )");
    CHECK(out.find("us 0\n") != std::string::npos);
    CHECK(out.find("ps 5000\n") != std::string::npos);
}

// A real delay rounds to the precision of the scope that declares the code
// running it (LRM 22.7). Inside a task that is the task's own scope, so the
// wheel tick has to divide that precision -- the tick computation reads
// subroutine bodies for real delays, not just procedural blocks. #1.5 in a
// 1ns/1ps module is 1500 ps, and $time in that module rounds it to 2 ns.
TEST_CASE("a real delay inside a task rounds to the declaring scope's precision") {
    const std::string out = run_capture(R"(`timescale 1ns/1ps
        module m;
            task automatic w; #1.5; endtask
            initial begin w(); $display("%0d %0t", $time, $time); end
        endmodule
    )");
    CHECK(out == "2 2000\n");
}
