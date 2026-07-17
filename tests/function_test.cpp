#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and return everything it wrote to stdout. The functions under
// test are exercised from an `initial` block that reports results with $display,
// so the captured text is the observable behavior (mirrors case_test.cpp).
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
[[maybe_unused]] std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

// A plain function returning via the function-name variable (13.4.1).
TEST_CASE("function returns via function-name assignment") {
    constexpr std::string_view src = R"(
        module m;
            function [7:0] add(input [7:0] a, b);
                add = a + b;
            endfunction
            initial $display("%0d", add(8'd40, 8'd2));
        endmodule
    )";
    CHECK(run_capture(src) == "42\n");
}

// `return expr` is a real terminator: the trailing statement runs only when the
// early return did not fire (13.4.1).
TEST_CASE("early return wins over fall-through (max)") {
    constexpr std::string_view src = R"(
        module m;
            function [7:0] mx(input [7:0] a, b);
                if (a > b) return a;
                return b;
            endfunction
            initial $display("%0d %0d", mx(8'd2, 8'd5), mx(8'd8, 8'd1));
        endmodule
    )";
    CHECK(run_capture(src) == "5 8\n");
}

// A `return` inside a loop terminates the whole function, not just the iteration.
TEST_CASE("return inside a loop exits the function") {
    constexpr std::string_view src = R"(
        module m;
            function [3:0] lsb(input [7:0] v);
                integer i;
                lsb = 4'd8;
                for (i = 0; i < 8; i = i + 1)
                    if (v[i]) return i[3:0];
            endfunction
            initial $display("%0d %0d %0d",
                lsb(8'b0010_0000), lsb(8'b0000_0001), lsb(8'b0000_0000));
        endmodule
    )";
    CHECK(run_capture(src) == "5 0 8\n");
}

// Two calls to one function in a single expression evaluate independently.
TEST_CASE("multiple calls in one expression") {
    constexpr std::string_view src = R"(
        module m;
            function [15:0] sq(input [7:0] x);
                sq = x * x;
            endfunction
            initial $display("%0d", sq(8'd3) + sq(8'd4));
        endmodule
    )";
    CHECK(run_capture(src) == "25\n");
}

// One function calling another.
TEST_CASE("nested function calls") {
    constexpr std::string_view src = R"(
        module m;
            function [7:0] inc(input [7:0] x); inc = x + 8'd1; endfunction
            function [7:0] inc2(input [7:0] x); inc2 = inc(inc(x)); endfunction
            initial $display("%0d", inc2(8'd40));
        endmodule
    )";
    CHECK(run_capture(src) == "42\n");
}

// Recursion works: each call gets an independent stack frame (automatic lifetime).
// Inlining could not express this; a real call ABI can.
TEST_CASE("recursive factorial") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int fact(input int n);
                if (n <= 1) return 1;
                return n * fact(n - 1);
            endfunction
            initial $display("%0d %0d %0d", fact(1), fact(5), fact(8));
        endmodule
    )";
    CHECK(run_capture(src) == "1 120 40320\n");
}

// Two self-calls per level — frame independence under recursion.
TEST_CASE("recursive fibonacci") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int fib(input int n);
                if (n < 2) return n;
                return fib(n - 1) + fib(n - 2);
            endfunction
            initial $display("%0d %0d %0d", fib(6), fib(10), fib(15));
        endmodule
    )";
    CHECK(run_capture(src) == "8 55 610\n");
}

// An unwritten return path yields X (the return variable's uninitialized value).
TEST_CASE("unassigned return path yields X") {
    constexpr std::string_view src = R"(
        module m;
            function [7:0] maybe(input [7:0] a, input sel);
                if (sel) maybe = a;
            endfunction
            initial begin
                $display("%0d", maybe(8'd55, 1'b1));
                $display("%h", maybe(8'd55, 1'b0));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "55\nxx\n");
}

// LRM 13.4.2 distinction (matches iverilog): a static (non-automatic) function's
// NAMED local persists across calls (the queue accumulates 1,2,3), while the
// implicit return variable is fresh each call (`maybe` unwritten path yields X
// above, not the prior call's value). A frame-Local return var gives the latter;
// persisting it would leak the prior value on the unwritten path.
TEST_CASE("static function: named local persists, return var is fresh") {
    constexpr std::string_view src = R"(
        module m;
            function int q_accum();
                int q[$];
                q.push_back(1);
                return q.size();
            endfunction
            initial $display("%0d %0d %0d", q_accum(), q_accum(), q_accum());
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 3\n");
}

// A void function called as a statement runs for its side effects.
TEST_CASE("void function called as a statement") {
    constexpr std::string_view src = R"(
        module m;
            integer c;
            function void bump(input integer n); c = c + n; endfunction
            initial begin
                c = 0;
                bump(5);
                bump(7);
                $display("%0d", c);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "12\n");
}

// A combinational process re-triggers when a signal read INSIDE a called function
// changes — sensitivity descends into the callee body (13.4, 9.2.2.2).
TEST_CASE("always_comb sensitivity includes signals read inside a function") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, g, y;
            function [7:0] f(input [7:0] x); f = x + g; endfunction
            always_comb y = f(a);
            initial begin
                a = 8'd1; g = 8'd10; #1; $display("%0d", y);
                g = 8'd20; #1; $display("%0d", y);
                a = 8'd5; #1; $display("%0d", y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "11\n21\n25\n");
}

// A continuous assign is sensitive only to the operands of its RHS (LRM 10.3.2);
// the function-body contribution is an always_comb-only rule (9.2.2.2.2). So the
// same `f(a)` reading module signal g does NOT re-evaluate y when g alone
// changes — only a change to the argument a does. Contrast the always_comb case
// above (11/21/25). Verified vs iverilog.
TEST_CASE("continuous assign is not sensitive to signals read inside a function") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, g, y;
            function [7:0] f(input [7:0] x); f = x + g; endfunction
            assign y = f(a);
            initial begin
                a = 8'd1; g = 8'd10; #1; $display("%0d", y);
                g = 8'd20; #1; $display("%0d", y);
                a = 8'd5; #1; $display("%0d", y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "11\n11\n25\n");
}

// Default lifetime in a non-automatic module is STATIC: a declared local persists
// across calls and is initialized once (LRM 13.4.2). counter() => 1, 2, 3.
TEST_CASE("static (default) local persists across calls") {
    constexpr std::string_view src = R"(
        module m;
            function int counter();
                int c;
                c = c + 1;
                return c;
            endfunction
            initial $display("%0d %0d %0d", counter(), counter(), counter());
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 3\n");
}

// An `automatic` local is fresh on every call: acounter() => 1, 1, 1. Same body
// as the static case above — only the lifetime differs.
TEST_CASE("automatic local is fresh each call") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int counter();
                int c;
                c = c + 1;
                return c;
            endfunction
            initial $display("%0d %0d %0d", counter(), counter(), counter());
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 1\n");
}

// Each module instance has its own copy of a static local declared in its
// subroutine (LRM 6.21 / 13.4.2): both instances count from 1 independently, so
// the second does NOT continue the first's sequence. slang re-elaborates each
// instance into distinct symbols, and the static is named under the instance
// prefix, so the two copies stay separate.
TEST_CASE("static function local is per module instance") {
    constexpr std::string_view src = R"(
        module sub ();
            function int bump();
                int c;
                c = c + 1;
                return c;
            endfunction
            initial begin
                int x, y;
                x = bump();
                y = bump();
                $display("%0d %0d", x, y);
            end
        endmodule
        module top;
            sub u0();
            sub u1();
        endmodule
    )";
    CHECK(run_capture(src) == "1 2\n1 2\n");
}

TEST_CASE("static local initializer runs once at time zero") {
    constexpr std::string_view src = R"(
        module m;
            function int counter();
                static int c = 100;
                c = c + 1;
                return c;
            endfunction
            function automatic int fresh();
                int a = 5;
                a = a + 1;
                return a;
            endfunction
            initial begin
                $display("%0d %0d %0d", counter(), counter(), counter());
                $display("%0d %0d", fresh(), fresh());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "101 102 103\n6 6\n");
}

TEST_CASE("function output and inout arguments copy back") {
    constexpr std::string_view src = R"(
        module m;
            function automatic void divmod(input int a, input int b,
                                           output int q, output int r);
                q = a / b;
                r = a % b;
            endfunction
            function automatic void accumulate(inout int acc, input int x);
                acc = acc + x;
            endfunction
            int q, r, acc;
            initial begin
                divmod(17, 5, q, r); $display("q=%0d r=%0d", q, r);
                acc = 10; accumulate(acc, 7); accumulate(acc, 3);
                $display("acc=%0d", acc);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "q=3 r=2\nacc=20\n");
}

// A conditional output write with an early `return` copies out on every return
// path, and an output / inout formal coexists with the function's own return
// value. An output to a bit-select actual copies back through the normal store.
TEST_CASE("function inout with early return and a complex output actual") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int trydec(inout int n);
                if (n == 0) return 0;
                n = n - 1;
                return 1;
            endfunction
            function automatic void setlow(output logic [3:0] o);
                o = 4'hA;
            endfunction
            int q, rc;
            logic [7:0] v;
            initial begin
                q = 3; rc = trydec(q); $display("rc=%0d q=%0d", rc, q);
                q = 0; rc = trydec(q); $display("rc=%0d q=%0d", rc, q);
                v = 8'hF0; setlow(v[3:0]); $display("v=%h", v);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "rc=1 q=2\nrc=0 q=0\nv=fa\n");
}

// Short-circuit `&&` / `||` must not evaluate the right operand when the left
// already decides the result, so a side-effecting call is skipped (LRM 11.3.5,
// 11.4.7). The function announces every call, making evaluation observable.
TEST_CASE("&& and || short-circuit a side-effecting right operand") {
    constexpr std::string_view src = R"(
        module m;
            function automatic bit f(input bit x);
                $display("f");
                return x;
            endfunction
            bit a, b, r;
            initial begin
                a = 0; b = 1;
                r = a && f(b); $display("r=%b", r);
                a = 1;
                r = a && f(b); $display("r=%b", r);
                r = a || f(b); $display("r=%b", r);
                a = 0;
                r = a || f(b); $display("r=%b", r);
            end
        endmodule
    )";
    // a=0: && skips f. a=1: && calls f. a=1: || skips f. a=0: || calls f.
    CHECK(run_capture(src) == "r=0\nf\nr=1\nr=1\nf\nr=1\n");
}

// An x/z left operand is ambiguous, not decisive, so the right operand is still
// evaluated and the full 3-valued result is produced (LRM 11.4.7 requires the
// second operand only when the first is definitively false / true).
TEST_CASE("&& and || evaluate the right operand on an x/z left") {
    constexpr std::string_view src = R"(
        module m;
            function automatic logic f(input logic x);
                $display("f");
                return x;
            endfunction
            logic a, r;
            initial begin
                a = 1'bx;
                r = a && f(1'b0); $display("r=%b", r);
                r = a && f(1'b1); $display("r=%b", r);
                r = a || f(1'b1); $display("r=%b", r);
                r = a || f(1'b0); $display("r=%b", r);
            end
        endmodule
    )";
    // Every branch evaluates f (a is x). x&&0=0, x&&1=x, x||1=1, x||0=x.
    CHECK(run_capture(src) == "f\nr=0\nf\nr=x\nf\nr=1\nf\nr=x\n");
}

TEST_CASE("-> short-circuits but <-> always evaluates its right operand") {
    constexpr std::string_view src = R"(
        module m;
            function automatic bit f(input bit x);
                $display("f");
                return x;
            endfunction
            bit a, r;
            initial begin
                a = 0;
                r = a -> f(1); $display("r=%b", r); // false -> skip f, r=1
                a = 1;
                r = a -> f(0); $display("r=%b", r); // true -> call f, r=0
                a = 1;
                r = a <-> f(0); $display("r=%b", r); // <-> always calls f, r=0
                a = 0;
                r = a <-> f(0); $display("r=%b", r); // <-> always calls f, r=1
            end
        endmodule
    )";
    CHECK(run_capture(src) == "r=1\nf\nr=0\nf\nr=0\nf\nr=1\n");
}

// An x/z left operand is ambiguous for `->` too, so the right operand is
// still evaluated (mirrors the &&/|| case above).
TEST_CASE("-> evaluates the right operand on an x/z left") {
    constexpr std::string_view src = R"(
        module m;
            function automatic logic f(input logic x);
                $display("f");
                return x;
            endfunction
            logic a, r;
            initial begin
                a = 1'bx;
                r = a -> f(1'b0); $display("r=%b", r);
                r = a -> f(1'b1); $display("r=%b", r);
            end
        endmodule
    )";
    // x->0 = x (ambiguous: could be 0->0=1 or 1->0=0); x->1 = 1 (dominated).
    CHECK(run_capture(src) == "f\nr=x\nf\nr=1\n");
}

// LRM 11.4.11: a definite ?: condition evaluates only the chosen arm; an
// ambiguous (x/z) condition evaluates BOTH arms and blends them bit by bit
// per Table 11-20. The side-effecting function makes evaluation observable.
// Verified against iverilog 13.
TEST_CASE("?: evaluates one arm on a definite condition, both on x") {
    constexpr std::string_view src = R"##(
        module m;
            function automatic logic [1:0] f(input logic [1:0] v);
                $display("f(%b)", v);
                return v;
            endfunction
            logic c;
            logic [1:0] r;
            initial begin
                c = 1; r = c ? f(2'b01) : f(2'b10); $display("r=%b", r);
                c = 0; r = c ? f(2'b01) : f(2'b10); $display("r=%b", r);
                c = 1'bx;
                r = c ? f(2'b01) : f(2'b10); $display("r=%b", r);
                r = c ? f(2'b11) : f(2'b11); $display("r=%b", r);
            end
        endmodule
    )##";
    CHECK(run_capture(src) == "f(01)\nr=01\nf(10)\nr=10\n"
                              "f(01)\nf(10)\nr=xx\nf(11)\nf(11)\nr=11\n");
}

// ---------------------------------------------------------------------------
// Whole unpacked arrays through the function/task ABI (LRM 13.4/13.5): by-value
// arguments (value semantics), array return, and array frame locals. iverilog
// rejects whole-array arguments; values cross-checked against verilator 5.048.
// ---------------------------------------------------------------------------

TEST_CASE("unpacked array as a by-value function argument (value semantics)") {
    // 2-state array (layout-direct) sums; a 4-state array is copied into a
    // canonical temp and the callee's mutation of its formal does not escape.
    CHECK(run_capture(R"(
        module m;
            function automatic int suma(input int a [0:3]);
                int t; int i;
                t = 0;
                for (i = 0; i < 4; i++) t = t + a[i];
                return t;
            endfunction
            function automatic int clobber(input logic [7:0] a [0:3]);
                a[0] = 8'hFF;   // must not affect the caller
                return a[1];
            endfunction
            int  q [0:3];
            logic [7:0] lq [0:3];
            int i, r1, r2;
            initial begin
                for (i = 0; i < 4; i++) begin q[i] = i + 1; lq[i] = 8'h10 + i; end
                r1 = suma(q);
                r2 = clobber(lq);
                $display("%0d %0d %0h", r1, r2, lq[0]);
            end
        endmodule
    )") == "10 17 10\n");
}

TEST_CASE("unpacked array returned by value from a function") {
    CHECK(run_capture(R"(
        typedef int intarr_t [0:3];
        module m;
            function automatic intarr_t scale2(input int a [0:3]);
                intarr_t r; int i;
                for (i = 0; i < 4; i++) r[i] = a[i] * 2;
                return r;
            endfunction
            int q [0:3];
            int o [0:3];
            int i;
            initial begin
                for (i = 0; i < 4; i++) q[i] = i + 1;
                o = scale2(q);
                $display("%0d %0d %0d %0d", o[0], o[1], o[2], o[3]);
            end
        endmodule
    )") == "2 4 6 8\n");
}

TEST_CASE("unpacked array as a function frame local") {
    CHECK(run_capture(R"(
        module m;
            function automatic int rev_sum(input int n);
                int tmp [0:3];
                int i, s;
                for (i = 0; i < 4; i++) tmp[i] = n + i;
                s = 0;
                for (i = 0; i < 4; i++) s = s + tmp[3 - i];
                return s;
            endfunction
            int r;
            initial begin
                r = rev_sum(10);
                $display("%0d", r);
            end
        endmodule
    )") == "46\n");
}
