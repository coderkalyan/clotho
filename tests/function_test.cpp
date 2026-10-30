#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and return everything it wrote to stdout. The functions under
// test are exercised from an `initial` block that reports results with $display,
// so the captured text is the observable behavior (mirrors case_test.cpp).
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

// An unwritten return path yields the return variable's current value (LRM 13.4:
// a function with no statements "returns the current value of the implicit
// variable"). Nothing has set it on the first call, so it is the type's default,
// X for a 4-state vector (Table 6-7). A static function's variable keeps what an
// earlier call wrote (13.4.2, docs/LRM_NOTES.md note 42), an automatic one is
// initialized on each entry (6.21).
TEST_CASE("unassigned return path yields the variable's current value") {
    constexpr std::string_view src = R"(
        module m;
            function [7:0] never(input [7:0] a); endfunction
            function [7:0] maybe(input [7:0] a, input sel);
                if (sel) maybe = a;
            endfunction
            function automatic [7:0] maybea(input [7:0] a, input sel);
                if (sel) maybea = a;
            endfunction
            initial begin
                $display("%h", never(8'd1));
                $display("%0d", maybe(8'd55, 1'b1));
                $display("%0d", maybe(8'd55, 1'b0));
                $display("%0d", maybea(8'd55, 1'b1));
                $display("%h", maybea(8'd55, 1'b0));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "xx\n55\n55\n55\nxx\n");
}

// LRM 13.4.2: a static (non-automatic) function's NAMED local persists across
// calls (the queue accumulates 1,2,3), and so does its implicit return variable,
// which the test above and the ones at the end of this file cover.
TEST_CASE("static function: a named local persists across calls") {
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

// A static local with an initializer needs once-at-time-zero initialization,
// which is not modeled; reject it rather than re-init on every call.
// A static local's initializer runs once before time zero (LRM 13.4.2), not per
// call: the counter persists and increments across calls; an automatic local
// re-inits each call.
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

// Output / inout formals copy back to the caller's actual at return (LRM 13.5):
// the callee writes its frame copy, which the backend copies out through a caller
// pointer at every return point. iverilog rejects a non-input function port
// ("Function arguments must be input ports").
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

// `->` is short-circuit like `&&`/`||` (LRM 11.3.5's list is &&, ||, ->, ?:):
// a definite-false left makes the implication definitely true without
// evaluating the right operand. `<->` is deliberately absent from that list
// -- LRM 11.4.7 requires each operand "evaluated exactly once" -- so it always
// calls the side-effecting right operand (docs/LRM_NOTES.md, note 14). iverilog
// does not implement ->'s short-circuit (evaluates the right operand
// unconditionally).
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

// The same clause, for an arm that is PURE but can trap. Deciding which arms may be
// speculated used to be a blacklist of three AST node kinds (a call, an assignment,
// an increment), so every one of these read like a pure expression, was evaluated on
// both arms, and killed the simulation from the arm the condition had ruled out. The
// predicate is a whitelist now, so an unlisted shape merely takes the branching path
// (lower/Lowerer.h's SpeculationScan).
TEST_CASE("?: does not evaluate a trapping arm the condition rules out") {
    SUBCASE("null class-handle property") {
        constexpr std::string_view src = R"##(
            module m;
                class C; int x = 7; endclass
                initial begin
                    C h;
                    $display("%0d", (h == null) ? -1 : h.x);
                    h = new();
                    $display("%0d", (h == null) ? -1 : h.x);
                end
            endmodule
        )##";
        CHECK(run_capture(src) == "-1\n7\n");
    }
    SUBCASE("non-active tagged-union member (LRM 11.9)") {
        constexpr std::string_view src = R"##(
            module m;
                typedef union tagged packed { int a; int b; } u_t;
                logic sel;
                initial begin
                    u_t u;
                    u   = tagged a 3;
                    sel = 1;
                    // Reading `u.b` while the tag says `a` is an LRM 11.9 violation
                    // and a located fatal, so the arm must not run.
                    $display("%0d", sel ? u.a : u.b);
                end
            endmodule
        )##";
        CHECK(run_capture(src) == "3\n");
    }
    SUBCASE("null virtual interface (LRM 25.9)") {
        constexpr std::string_view src = R"##(
            interface bus_if; logic [3:0] d; endinterface
            module m;
                bus_if b();
                virtual bus_if v;
                initial begin
                    b.d = 4'h5;
                    $display("%0d", (v == null) ? 4'hf : v.d);
                    v = b;
                    $display("%0d", (v == null) ? 4'hf : v.d);
                end
            endmodule
        )##";
        CHECK(run_capture(src) == "15\n5\n");
    }
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

TEST_CASE("non-lvalue whole-array actuals bound to an input formal") {
    // LRM 13.5: an input actual "can be any expression" — a `'{...}` pattern
    // (positional and replicated) is built into a caller-side canonical temp,
    // and a call result passes the caller-side out-param temp the call already
    // wrote. iverilog rejects whole-array formals outright; values
    // cross-checked against verilator 5.050.
    CHECK(run_capture(R"(
        module m;
            typedef int a2_t [0:1];
            typedef int a4_t [0:3];
            function automatic a2_t g();
                a2_t r;
                r[0] = 5; r[1] = 6;
                return r;
            endfunction
            function automatic int f2(a2_t v); return v[0] + v[1]; endfunction
            function automatic int f4(a4_t v); return v[0] + v[1] + v[2] + v[3]; endfunction
            initial begin
                $display("pat %0d", f2('{5, 6}));
                $display("repl %0d", f4('{4{7}}));
                $display("call %0d", f2(g()));
            end
        endmodule
    )") == "pat 11\n"
           "repl 28\n"
           "call 11\n");
}

// A subroutine variable declared without `automatic` has STATIC lifetime (LRM
// 13.4.2), so the frontend gives it a module signal rather than a frame slot --
// and a plain scalar-leaf array signal keeps FLAT `[N x slot]` storage, not the
// canonical aggregate mirror a frame local always gets. The result copy-out used
// to resolve the returned object through the aggregate GEP path unconditionally,
// which dereferences a null root type for a flat place: the compiler segfaulted
// with no diagnostic at all. Every array-return case above returns an
// `automatic` local, which is why none of them reached it.
TEST_CASE("fixed array returned from a static function local") {
    CHECK(run_capture(R"(
        typedef int intarr_t [0:1];
        module m;
            function intarr_t mk();
                intarr_t r;
                r[0] = 7; r[1] = 8;
                return r;
            endfunction
            intarr_t o;
            initial begin
                static intarr_t d = mk();
                o = mk();
                $display("assign %p", o);
                $display("declinit %p", d);
            end
        endmodule
    )") == "assign '{7, 8}\n"
           "declinit '{7, 8}\n");
}

// The flat and canonical images are NOT interchangeable, so the copy-out cannot
// simply memcpy the mirror's byte count over flat storage. A flat element slot is
// may-z analysed per signal while an aggregate leaf's is fixed by its type, and
// they disagree in both directions: a never-z `logic [7:0]` array is `[2 x
// { i16 }]` against the canonical `[2 x { i16, i8 }]` (a memcpy would read four
// bytes past the signal), and a 2-state array the analysis cannot prove z-free is
// `[2 x { i64, i32 }]` against the canonical `[2 x { i32 }]` (a memcpy would take
// the first element's low half as the whole array). Both shapes convert per
// element instead.
TEST_CASE("fixed array return converts a may-z-narrowed element slot") {
    CHECK(run_capture(R"(
        module m;
            typedef logic [7:0] larr_t [0:1];
            typedef int iarr_t [0:1];
            logic signed [31:0] zsrc;
            function larr_t mkl();
                larr_t r;
                r[0] = 8'hA5; r[1] = 8'h3C;
                return r;
            endfunction
            function iarr_t mki();
                iarr_t r;
                r[0] = zsrc; r[1] = 9;
                return r;
            endfunction
            larr_t lo;
            iarr_t io;
            initial begin
                zsrc = 32'bz;
                lo = mkl();
                io = mki();
                $display("nz4 %0h %0h", lo[0], lo[1]);
                $display("mayz2 %0d %0d", io[0], io[1]);
            end
        endmodule
    )") == "nz4 a5 3c\n"
           "mayz2 0 9\n");
}

// Flat storage flattens every dimension into one run of leaves while the mirror
// nests one ArrayType per dimension. The two are the same bytes in the same
// row-major order, which is what lets one copy serve both shapes.
TEST_CASE("multidimensional fixed array returned from a static local") {
    CHECK(run_capture(R"(
        module m;
            typedef int m2_t [0:1][0:1];
            typedef int m3_t [0:1][0:1][0:1];
            function m2_t mk2(); m2_t r; r[0][0] = 1; r[1][1] = 4; return r; endfunction
            function m3_t mk3(); m3_t r; r[0][0][0] = 5; r[1][1][1] = 6; return r; endfunction
            m2_t a2; m3_t a3;
            initial begin
                a2 = mk2(); a3 = mk3();
                $display("d2 %p", a2);
                $display("d3 %p", a3);
            end
        endmodule
    )") == "d2 '{'{1, 0}, '{0, 4}}\n"
           "d3 '{'{'{5, 0}, '{0, 0}}, '{'{0, 0}, '{0, 6}}}\n");
}

// A `real` leaf is a 2-state 64-bit slot, so its flat and canonical shapes
// coincide and the copy is the plain storage memcpy. A `string` leaf owns heap,
// so the same copy-out routes through the per-leaf deep copy instead -- over flat
// `[N x rt_string]`, whose bytes are the mirror's exactly, since a managed slot
// has no planes for the may-z analysis to narrow.
TEST_CASE("real and string element arrays returned from a static local") {
    CHECK(run_capture(R"(
        module m;
            typedef real rarr_t [0:1];
            typedef string sarr_t [0:1];
            function rarr_t mkr(); rarr_t r; r[0] = 1.5; r[1] = -2.25; return r; endfunction
            function sarr_t mks(); sarr_t r; r[0] = "hi"; r[1] = "there"; return r; endfunction
            rarr_t ro; sarr_t so;
            initial begin
                ro = mkr(); so = mks();
                $display("real %p", ro);
                $display("str %p", so);
            end
        endmodule
    )") == "real '{1.5, -2.25}\n"
           "str '{\"hi\", \"there\"}\n");
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

TEST_CASE("unpacked array and struct as output / inout function arguments") {
    // The other two by-value directions over the same storage-pointer ABI: an
    // output formal starts at its frame default and is copied back at return, an
    // inout is seeded from the actual first (LRM 13.5.1). A flat array actual goes
    // through the caller's canonical temp, a struct through its own storage.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [7:0] b; } s_t;
            function automatic void fill(output int a [0:2]);
                a[0] = 10; a[1] = 20; a[2] = 30;
            endfunction
            function automatic void bump(inout int a [0:2]);
                a[1] = a[1] + 5;
            endfunction
            function automatic void mk(output s_t v);
                v.a = 7; v.b = 8'h11;
            endfunction
            function automatic void inc(inout s_t v);
                v.a = v.a + 1;
            endfunction
            int q [0:2];
            s_t s;
            initial begin
                fill(q);
                $display("fill %0d %0d %0d", q[0], q[1], q[2]);
                bump(q);
                $display("bump %0d %0d %0d", q[0], q[1], q[2]);
                mk(s);
                $display("mk a=%0d b=%0h", s.a, s.b);
                inc(s);
                $display("inc a=%0d", s.a);
            end
        endmodule
    )") == "fill 10 20 30\n"
           "bump 10 25 30\n"
           "mk a=7 b=11\n"
           "inc a=8\n");
}

// $time / $stime / $realtime inside a plain function (LRM 20.3). A function is
// not a coroutine and carries no wheel parameter, but the wheel is a module
// global (clotho.wheel) that every body binds, so a function reads the clock
// exactly as a task does. $time rounds the tick count to the time unit of the
// scope that DECLARES the function -- 1ns here -- while $realtime keeps the
// sub-unit fraction. The inner $display runs during argument evaluation, so it
// precedes the caller's line. Cross-checked against iverilog.
TEST_CASE("$time / $stime / $realtime inside a function read the live clock") {
    const std::string out = run_capture(R"(`timescale 1ns/1ps
        module m;
            function automatic longint unsigned stamp();
                $display("fn t=%0d st=%0d rt=%0.3f", $time, $stime, $realtime);
                return $time;
            endfunction
            initial begin
                #5   $display("ret=%0d", stamp());
                #2.5 $display("ret=%0d", stamp());
            end
        endmodule
    )");
    CHECK(out == "fn t=5 st=5 rt=5.000\nret=5\n"
                 "fn t=8 st=8 rt=7.500\nret=8\n");
}

// LRM 3.14.2.3: the read is in the units of the scope that DECLARES the
// function, never the one that calls it -- the body is lowered once and shared
// by every call site (function_id), so scaling it by the caller would leave at
// most one caller correct. The package function reads nanoseconds while its
// caller, whose unit is microseconds, reads the same instant as 5000.
// Cross-checked against iverilog.
TEST_CASE("a package function's $time uses the declaring scope's timescale") {
    const std::string out = run_capture(R"(`timescale 1ns/1ns
        package p;
            function automatic longint unsigned now_ns; return $time; endfunction
        endpackage
        `timescale 1us/1ns
        module m;
            import p::*;
            initial begin #5000; $display("fn %0d caller %0d", now_ns(), $time); end
        endmodule
    )");
    CHECK(out == "fn 5000000 caller 5000\n");
}

// A class method is a function too, and its declaring scope is the class's
// enclosing package (Scope::getTimeScale walks out to it), so the method reads
// picoseconds while the calling module reads nanoseconds. Also covers the
// this-carrying ABI: the wheel is a global, not an added formal, so nothing about
// the call shape changes. Cross-checked against iverilog.
TEST_CASE("a class method reads $time in its declaring package's timescale") {
    const std::string out = run_capture(R"(`timescale 1ps/1ps
        package p;
            class C;
                function longint unsigned t(); return $time; endfunction
            endclass
        endpackage
        `timescale 1ns/1ps
        module m;
            import p::*;
            C c = new;
            initial begin #4; $display("method=%0d caller=%0d", c.t(), $time); end
        endmodule
    )");
    CHECK(out == "method=4000 caller=4\n");
}

// LRM 13.4 defines `return expr` as assigning `expr` to the function's implicit
// return variable and returning that variable, so `return` must accept exactly the
// rvalues an assignment accepts. Clotho used to refuse every non-lvalue aggregate
// ("aggregate return of a non-lvalue expression is not supported"), which is what
// blocked the UVM shape below. The three rvalue forms take two different routes:
// a call result already sits in the caller-side out-param temp the call allocates,
// so it is returned in place; a `'{...}` pattern has no storage yet and is built
// straight into the return variable, which costs no temp.
TEST_CASE("aggregate rvalue forms returned from a function (LRM 13.4)") {
    SUBCASE("unpacked struct") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; int b; } st;
                function automatic st g(); st s; s.a = 1; s.b = 2; return s; endfunction
                function automatic st from_call();    return g();      endfunction
                function automatic st from_pattern(); return '{3, 4};  endfunction
                function automatic st from_pop();
                    st q[$]; q.push_back('{5, 6}); return q.pop_back();
                endfunction
                initial begin
                    st o;
                    o = from_call();    $display("call %0d %0d", o.a, o.b);
                    o = from_pattern(); $display("pat %0d %0d", o.a, o.b);
                    o = from_pop();     $display("pop %0d %0d", o.a, o.b);
                end
            endmodule
        )") == "call 1 2\npat 3 4\npop 5 6\n");
    }
    SUBCASE("fixed unpacked array") {
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                function automatic ai g(); ai r; r[0] = 7; r[1] = 8; r[2] = 9; return r; endfunction
                function automatic ai from_call();    return g();          endfunction
                function automatic ai from_pattern(); return '{4, 5, 6};   endfunction
                function automatic ai from_pop();
                    ai q[$]; q.push_back('{1, 2, 3}); return q.pop_back();
                endfunction
                initial begin
                    $display("call %p", from_call());
                    $display("pat %p", from_pattern());
                    $display("pop %p", from_pop());
                end
            endmodule
        )") == "call '{7, 8, 9}\npat '{4, 5, 6}\npop '{1, 2, 3}\n");
    }
}

// A subroutine variable declared without `automatic` has STATIC lifetime (LRM
// 13.4.2) and becomes a module signal with FLAT storage, while an `automatic` one
// is a frame local in the canonical aggregate mirror. The result copy-out resolves
// either model ("abi: an aggregate result copy-out resolves either storage model"), and the new rvalue return paths must reach both: the
// pattern form stores into the return variable, whose own storage is always a frame
// local, but the returned OBJECT of the inner call is the static signal.
TEST_CASE("aggregate rvalue return from a static function local") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; int b; } st;
            typedef int ai [0:1];
            function st sg(); st s; s.a = 1; s.b = 2; return s; endfunction
            function st s_call();    return sg();     endfunction
            function st s_pattern(); return '{3, 4};  endfunction
            function ai sga(); ai r; r[0] = 7; r[1] = 8; return r; endfunction
            function ai sa_call();    return sga();    endfunction
            function ai sa_pattern(); return '{5, 6};  endfunction
            initial begin
                $display("scall %p", s_call());
                $display("spat %p", s_pattern());
                $display("acall %p", sa_call());
                $display("apat %p", sa_pattern());
            end
        endmodule
    )") == "scall '{a:1, b:2}\nspat '{a:3, b:4}\nacall '{7, 8}\napat '{5, 6}\n");
}

// A string leaf owns heap, so the staged and returned copies must each be a deep
// copy: the pattern form stores the literal into the return variable's own slot,
// and the call form copies the callee's out-param temp out again. A shallow copy
// would either alias one buffer across two owners or double-free it at frame exit.
TEST_CASE("aggregate return of a struct with a string member deep-copies") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; string s; } sst;
            function automatic sst mk();  return '{7, "hello"};  endfunction
            function automatic sst fwd(); return mk();           endfunction
            function automatic sst pop();
                sst q[$]; q.push_back('{9, "world"}); return q.pop_back();
            endfunction
            initial begin
                sst o;
                o = mk();  $display("mk %0d %s", o.a, o.s);
                o = fwd(); $display("fwd %0d %s", o.a, o.s);
                o = pop(); $display("pop %0d %s", o.a, o.s);
                $display("nested %p", fwd());
            end
        endmodule
    )") == "mk 7 hello\nfwd 7 hello\npop 9 world\nnested '{a:7, s:\"hello\"}\n");
}

// The construct that motivated the whole path: Accellera's uvm_queue.svh:189 is
// `virtual function T pop_back(); return queue.pop_back(); endfunction`. The
// returned expression is a queue-method call whose result is an aggregate, reached
// through a virtual method on a parameterized class, so it exercises the call arm
// of the lvalue path under the this-carrying ABI.
TEST_CASE("a uvm_queue-shaped class returns a struct from a queue pop") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int id; string nm; } item_t;
            class uvm_queue #(type T = int);
                T queue[$];
                function void push_back(T item); queue.push_back(item); endfunction
                virtual function T pop_back();  return queue.pop_back();  endfunction
                virtual function T pop_front(); return queue.pop_front(); endfunction
                function int size(); return queue.size(); endfunction
            endclass
            initial begin
                uvm_queue #(item_t) q = new();
                item_t first, second, x;
                first  = '{1, "alpha"};
                second = '{2, "beta"};
                q.push_back(first);
                q.push_back(second);
                x = q.pop_back();  $display("back %0d %s", x.id, x.nm);
                x = q.pop_front(); $display("front %0d %s", x.id, x.nm);
                $display("size %0d", q.size());
            end
        endmodule
    )") == "back 2 beta\nfront 1 alpha\nsize 0\n");
}

// LRM 13.4 defines `return expr` as assigning `expr` to the function's implicit
// return variable, so `return` accepts exactly the rvalues an assignment does --
// which now includes an aggregate `?:` (LRM 11.4.11). agg_return_operand needs no
// case of its own for it: the operand falls through to emit_agg_store, which
// branches and builds the selected arm straight into the return variable, so
// neither arm is staged anywhere first.
TEST_CASE("aggregate return of a conditional operator") {
    SUBCASE("a struct, both directions, other arm unevaluated") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                int nt = 0, nf = 0;
                function automatic st ft(); nt++; return '{a:1, b:2}; endfunction
                function automatic st ff(); nf++; return '{a:3, b:4}; endfunction
                function automatic st pick(input bit c); return c ? ft() : ff(); endfunction
                initial begin
                    $display("%p nt=%0d nf=%0d", pick(1), nt, nf);
                    $display("%p nt=%0d nf=%0d", pick(0), nt, nf);
                end
            endmodule
        )") == "'{a:1, b:2} nt=1 nf=0\n'{a:3, b:4} nt=1 nf=1\n");
    }
    SUBCASE("a fixed unpacked array") {
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                ai p, q;
                function automatic ai pick(input bit c); return c ? p : q; endfunction
                initial begin
                    p = '{1, 2, 3};
                    q = '{4, 5, 6};
                    $display("%p %p", pick(1), pick(0));
                end
            endmodule
        )") == "'{1, 2, 3} '{4, 5, 6}\n");
    }
    SUBCASE("an ambiguous condition takes the Table 7-1 default") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                st x, y;
                function automatic st pick(input logic c); return c ? x : y; endfunction
                initial begin
                    x = '{a:1, b:2};
                    y = '{a:3, b:4};
                    $display("%p", pick(1'bx));
                end
            endmodule
        )") == "'{a:0, b:0}\n");
    }
}

TEST_CASE("aggregate returns Clotho still refuses loudly") {
    // Bounding `return` by exactly what assignment supports is what makes the LRM
    // 13.4 equivalence honest, so the refusal below is the ASSIGNMENT path's own
    // diagnostic, reached because the return operand is staged through it.
    // Not a return-path limit: inside a subroutine the slice source and the return
    // variable are both canonical aggregate frame storage, and the backend has no
    // aggregate-element slice, so a plain `o = x[0:1]` between two frame locals
    // already fails the same way. Refused here with its own message rather than
    // lowering into a backend error.
    SUBCASE("an unpacked-array slice") {
        const std::string d = reject(R"(
            module m;
                typedef int a3 [0:2];
                typedef int a2 [0:1];
                function automatic a2 sl(); a3 x; x[0] = 1; return x[0:1]; endfunction
                initial $display("%p", sl());
            endmodule
        )");
        CHECK(d.find("return of an unpacked-array slice is not supported") != std::string::npos);
    }
}

// LRM 13.4.3: a constant function call initializes its variables "as they would
// be for normal simulation", static ones included, and no value carries over
// between calls. slang skips the static initializer without a patch.
TEST_CASE("a constant function call runs its static variable initializers") {
    CHECK(run_capture(R"(
        module m;
            function int f(int n); static int acc = 5; acc += n; return acc; endfunction
            localparam int P = f(1);
            localparam int Q = f(1);
            initial $display("%0d %0d %0d", P, Q, f(1));
        endmodule
    )") == "6 6 6\n");
}

// A call to a user function in a run-time expression runs every time the
// expression is evaluated; it is never folded to the value slang's constant
// evaluator computes. That evaluator gives each call fresh variables (LRM
// 13.4.3, for a constant function call) and accepts any function whose only
// state is its own static locals, so a call to one used to be folded where the
// lowering tried to specialize a case label, a select index, a system function
// argument or a one-hot selector, and the function's state never advanced.
// Expectations are the LRM text (13.4.2: a static function's variables are
// statically allocated, so `tick()` counts 1, 2, 3 ...); Icarus Verilog 13
// agrees on every design below.
TEST_CASE("a call to a stateful function in a case label runs at each comparison") {
    CHECK(run_capture(R"(
        module m;
            function int tick(); static int n = 0; n = n + 1; return n; endfunction
            int sel = 3;
            initial begin
                case (sel) tick(): $display("first"); tick(): $display("second");
                           tick(): $display("third"); default: $display("none"); endcase
                unique case (sel) 9: $display("nine"); tick(): $display("fourth");
                                  default: $display("none"); endcase
            end
        endmodule
    )") == "third\nnone\n");
}

TEST_CASE("a call to a stateful function as a select index runs at each evaluation") {
    CHECK(run_capture(R"(
        module m;
            function int tick(); static int n = 0; n = n + 1; return n; endfunction
            logic [7:0] v = 8'b0000_0110;
            logic [7:0] w = 0;
            int a [0:7];
            initial begin
                $display("%b %b %b", v[tick()], v[tick()], v[tick()]);
                for (int i = 0; i < 3; i++) begin
                    w[tick()] = 1'b1;
                    a[tick()] = 10;
                end
                $display("%b", w);
                $display("%0d %0d %0d %0d", a[4], a[6], a[8 - 1], a[10 - 7]);
            end
        endmodule
    )") == "1 1 0\n"
           "01010000\n"
           "0 0 10 0\n");
}

// The bit queries read their operand's value, so a call in it runs each time;
// $bits does not evaluate its operand (LRM 20.6.2), so a call in it still folds.
TEST_CASE("a call in the operand of a value-reading system function runs each time") {
    CHECK(run_capture(R"(
        module m;
            function int tick(); static int n = 0; n = n + 1; return n; endfunction
            initial begin
                $display("%0d %0d %0d", $clog2(tick()), $clog2(tick()), $clog2(tick()));
                $display("%0d %0d %0d", $countones(tick()), $countones(tick()), $countones(tick()));
                $display("%0d", $bits(tick()));
                $display("%0d", tick());
            end
        endmodule
    )") == "0 1 2\n"
           "1 2 2\n"
           "32\n"
           "7\n");
}

// The one-hot reverse case, `case (1'b1)` over single-bit selects, is chosen at
// lowering time when its selector folds to 1. A selector that calls a function
// is evaluated at run time: 1'(1) is 1 and selects the set bit, 1'(2) is 0 and
// selects the first clear one.
TEST_CASE("a call in a case selector keeps the one-hot form from being chosen") {
    CHECK(run_capture(R"(
        module m;
            function int tick(); static int n = 0; n = n + 1; return n; endfunction
            logic [3:0] v = 4'b0100;
            initial begin
                repeat (2) begin
                    case (1'(tick()))
                        v[0]: $write("A ");
                        v[1]: $write("B ");
                        v[2]: $write("C ");
                        v[3]: $write("D ");
                    endcase
                end
                $display("");
            end
        endmodule
    )") == "C A \n");
}

// Where the grammar demands a constant -- a part-select bound, a replication
// count, a net lvalue's select -- a constant function call is legal and still
// folds.
TEST_CASE("a constant function call still folds where a constant is required") {
    CHECK(run_capture(R"(
        module m;
            function automatic int w(int a); return a * 2; endfunction
            logic [7:0] v = 8'hA5;
            wire [3:0] o;
            assign o[w(1)] = 1'b1;
            and g (o[w(1) + 1], 1'b1, 1'b1);
            initial #1 $display("%b %b %b %b", v[w(2) - 1:0], o[w(1)], o[w(1) + 1], {w(1){1'b1}});
        endmodule
    )") == "0101 1 1 11\n");
}

// The implicit return variable of a static function (LRM 13.4.1, 13.4.2). The
// definition "shall implicitly declare a variable, internal to the function, with
// the same name as the function", and a static function has "all declared items
// being statically allocated", shared "across all uses of the function executing
// concurrently". A static variable exists for the whole simulation and is
// initialized on each entry only if it is automatic (6.21), so the variable
// starts at its type's default and keeps its value from call to call. Icarus
// Verilog 13 and Verilator 5.050 reset it on every call (iverilog even keeps a
// queue and resets an int), so the expectations below are the LRM text alone
// (docs/LRM_NOTES.md, note 42).
TEST_CASE("the implicit return variable of a static function keeps its value between calls") {
    CHECK(run_capture(R"(
        module m;
            function int g(int x); g = g + x; endfunction
            function automatic int ga(int x); ga = ga + x; endfunction
            initial $display("%0d %0d | %0d %0d", g(5), g(5), ga(5), ga(5));
        endmodule
    )") == "5 10 | 5 5\n");
}

// Where the function lives decides the default lifetime (13.4.2): a module,
// interface, program or package function, and a $unit one, is static; a class
// method is always automatic (8.6), a static one too (8.10: "the lifetime of the
// method within the class", not of its variables); `automatic` and a module
// declared `automatic` make it automatic; `static` inside an automatic module
// brings it back. Each instance of a module has its own static storage.
TEST_CASE("the lifetime of a function decides whether its implicit return variable persists") {
    CHECK(run_capture(R"(
        package P;
            function int pg(int x); pg = pg + x; endfunction
        endpackage
        function int ug(int x); ug = ug + x; endfunction
        class C;
            function int cm(int x); cm = cm + x; endfunction
            static function int sm(int x); sm = sm + x; endfunction
        endclass
        interface ifc;
            function int ig(int x); ig = ig + x; endfunction
        endinterface
        module sub #(int D = 1);
            function int g(int x); g = g + x; endfunction
            initial begin
                int a, b;
                #D;
                a = g(1); b = g(1);
                $display("%m %0d %0d", a, b);
            end
        endmodule
        module automatic am;
            function int g(int x); g = g + x; endfunction
            function static int gs(int x); gs = gs + x; endfunction
            initial begin
                #3;
                $display("am g %0d %0d gs %0d %0d", g(5), g(5), gs(5), gs(5));
            end
        endmodule
        module m;
            ifc i();
            C c1 = new, c2 = new;
            sub #(1) s1();
            sub #(2) s2();
            am a();
            initial begin
                #4;
                $display("pkg %0d %0d | unit %0d %0d", P::pg(2), P::pg(2), ug(2), ug(2));
                $display("cm %0d %0d sm %0d %0d", c1.cm(5), c2.cm(5), C::sm(5), C::sm(5));
                $display("ifc %0d %0d", i.ig(5), i.ig(5));
            end
        endmodule
    )") == "m.s1 1 2\n"
           "m.s2 1 2\n"
           "am g 5 5 gs 5 10\n"
           "pkg 2 4 | unit 2 4\n"
           "cm 5 5 sm 5 5\n"
           "ifc 5 10\n");
}

// Every way the entry value can be seen: read before the write, a write on only
// some paths (a branch, a case with no default, a loop that may not run), a
// compound assignment, a part-select write that leaves the other bits, a nested
// assignment, and the function reached again through itself. Recursion shares the
// one variable (13.4.2), so the outer call finds what the inner one left: 0 here,
// where the automatic twin keeps its own and gives 2 + (1 + 0) = 3.
TEST_CASE("an implicit return variable whose entry value is observed persists") {
    CHECK(run_capture(R"(
        module m;
            function int cp(int x); cp += x; endfunction
            function int lp(int n); for (int i = 0; i < n; i++) lp = i; endfunction
            function int cs(int x); case (x) 0: cs = 10; 1: cs = 11; endcase endfunction
            function int dw(int x); do dw = dw + x; while (0); endfunction
            function bit [7:0] part(bit [7:0] x); part[3:0] = x[3:0]; part[7] = ~part[7]; endfunction
            function int nest(int x); if ((nest = x) > 100) nest = 0; endfunction
            function int rec(int n); rec = n; if (n > 0) begin int t; t = rec(n - 1); rec = rec + t; end endfunction
            function automatic int reca(int n);
                reca = n; if (n > 0) begin int t; t = reca(n - 1); reca = reca + t; end
            endfunction
            function logic [3:0] f4(logic en); if (en) f4 = 4'd9; endfunction
            initial begin
                $display("cp %0d %0d", cp(5), cp(5));
                $display("lp %0d %0d %0d", lp(3), lp(0), lp(2));
                $display("cs %0d %0d %0d", cs(0), cs(2), cs(1));
                $display("dw %0d %0d", dw(2), dw(2));
                $display("part %h %h %h", part(8'hA5), part(8'h3C), part(8'hF7));
                $display("nest %0d %0d", nest(7), nest(200));
                $display("rec %0d reca %0d", rec(2), reca(2));
                $display("f4 %b %b %b", f4(1'b0), f4(1'b1), f4(1'b0));
            end
        endmodule
    )") == "cp 5 10\n"
           "lp 2 2 1\n"
           "cs 10 10 11\n"
           "dw 2 4\n"
           "part 85 0c 87\n"
           "nest 7 0\n"
           "rec 0 reca 3\n"
           "f4 xxxx 1001 1001\n");
}

// An implicit return variable that is overwritten on every path before it is read
// is indistinguishable from a fresh one, which is what keeps these functions free
// of design-state storage; `return expr` hands the caller `expr` and does not
// write the variable (13.4.1: it "shall override any value assigned to the
// function name"), so `h(-1)` leaves what `h(5)` stored for the next call.
TEST_CASE("an implicit return variable overwritten before any read, and return expr") {
    CHECK(run_capture(R"(
        module m;
            function int w1(int x); w1 = x + 1; endfunction
            function int w2(int x); if (x > 0) w2 = 1; else w2 = 2; endfunction
            function int w3(int x); case (x) 0: w3 = 5; default: w3 = 6; endcase endfunction
            function int w4(int x); return x * 2; endfunction
            function int w5(int x); w5 = 0; for (int i = 0; i < x; i++) w5 = w5 + i; endfunction
            function int h(int x); if (x < 0) return 100; h = h + x; endfunction
            function int r(int x); return r + x; endfunction
            initial begin
                $display("%0d %0d | %0d %0d | %0d %0d | %0d %0d | %0d %0d",
                         w1(1), w1(1), w2(1), w2(0), w3(0), w3(9), w4(3), w4(3), w5(4), w5(4));
                $display("h %0d %0d %0d r %0d %0d", h(5), h(-1), h(5), r(5), r(5));
            end
        endmodule
    )") == "2 2 | 1 2 | 5 6 | 6 6 | 6 6\n"
           "h 5 100 10 r 5 5\n");
}

// The variable has the function's whole return type: a string appends, an
// unpacked struct keeps its members, and a queue keeps its elements, while the
// caller gets a copy (the queue a caller holds does not grow with later calls).
TEST_CASE("the implicit return variable persists for a string, a struct and a queue") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; int b; } st_t;
            typedef int q_t[$];
            function string s(string t); s = {s, t}; endfunction
            function st_t sf(int x); sf.a = sf.a + x; sf.b = x; endfunction
            function q_t qf(int x); qf.push_back(x); endfunction
            initial begin
                st_t t1, t2;
                q_t  q1, q2;
                $display("s %s|%s", s("ab"), s("cd"));
                t1 = sf(3); t2 = sf(4);
                $display("sf %0d %0d %0d %0d", t1.a, t1.b, t2.a, t2.b);
                q1 = qf(1); q2 = qf(2);
                $display("qf %0d %0d %0d %0d", q1.size(), q2.size(), q1[0], q2[1]);
            end
        endmodule
    )") == "s ab|abcd\n"
           "sf 3 3 7 4\n"
           "qf 1 2 1 2\n");
}

// LRM 13.4.3: a constant function call at elaboration time leaves the variables of
// simulation time alone, and each call there starts afresh, so the two localparams
// are 5 and 5 while the simulation-time calls go on from 0. slang evaluates it.
TEST_CASE("a constant function call does not touch the simulation-time return variable") {
    CHECK(run_capture(R"(
        module m;
            function int g(int x); g = g + x; endfunction
            localparam int P1 = g(5);
            localparam int P2 = g(5);
            initial $display("P1=%0d P2=%0d g=%0d g=%0d", P1, P2, g(5), g(5));
        endmodule
    )") == "P1=5 P2=5 g=5 g=10\n");
}

// The case label of a stateful static function sees the running value too: the
// label folding that used to hide it is the previous commit.
TEST_CASE("a case label calling a function that reads its own return variable") {
    CHECK(run_capture(R"(
        module m;
            function int g(int x); g = g + x; endfunction
            int sel = 10;
            initial case (sel) g(5): $display("first"); g(5): $display("second"); default: $display("none"); endcase
        endmodule
    )") == "second\n");
}

// An automatic variable "is initialized on each entry to the call" (LRM 6.21), to
// the default of Table 6-7: `'0` for a 2-state integral, `'x` for a 4-state one,
// 0.0 for a real, "" for a string, an empty queue, null for a class handle. That
// holds for the implicit return variable of an automatic function or class method,
// an `output` argument of an automatic task (13.3.2: "initialized to the default
// initialization value whenever execution enters their scope") and an automatic
// local, of every type. An unpacked struct or array powered up its 2-state and
// real members from whatever the stack held: the typed fill wrote only the
// 4-state and managed leaves and left the rest to the zero a signal starts from,
// which a frame does not. `dirty` fills the stack with a pattern between the calls
// so a read of leftover memory cannot pass for a zero; the output was the same
// under every pattern tried.
TEST_CASE("aggregate and scalar locals of an automatic subroutine start at their default on every entry") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; int b; } st_t;
            typedef struct { logic [3:0] l; bit [3:0] b; real r; string s; int q[$]; st_t in; } mix_t;
            typedef int arr_t [3];
            typedef logic [3:0] la_t [2];
            typedef real ra_t [2];
            typedef int q_t[$];
            class C; int v; endclass
            function automatic int dirty(int d);
                int big [64];
                for (int i = 0; i < 64; i++) big[i] = 32'hdeadbeef + d;
                return big[d];
            endfunction
            function automatic st_t  f_st(int x); f_st.a = f_st.a + x; endfunction
            function automatic arr_t f_ar(int x); f_ar[1] = f_ar[1] + x; endfunction
            function automatic la_t  f_la(int x); f_la[0] = f_la[0] + 1; endfunction
            function automatic ra_t  f_ra(int x); f_ra[0] = f_ra[0] + 1.5; endfunction
            function automatic string f_s(string t); f_s = {f_s, t}; endfunction
            function automatic q_t   f_q(int x); f_q.push_back(f_q.size()); endfunction
            function automatic C     f_c(int x); if (f_c == null) f_c = new; f_c.v = x; endfunction
            function automatic logic [7:0] f_p(int x); f_p[0] = 1; endfunction
            function automatic bit [7:0]   f_b(int x); f_b[0] = 1; endfunction
            function automatic mix_t f_mx(int x);
                f_mx.b = f_mx.b + 1; f_mx.r = f_mx.r + 2.5; f_mx.s = {f_mx.s, "z"};
                f_mx.q.push_back(f_mx.q.size()); f_mx.in.b = f_mx.in.b + 1;
            endfunction
            class K;
                function st_t  m_st(int x); m_st.a = m_st.a + x; endfunction
                function arr_t m_ar(int x); m_ar[1] = m_ar[1] + x; endfunction
                function logic [7:0] m_p(int x); m_p[0] = 1; endfunction
                static function st_t sm_st(int x); sm_st.a = sm_st.a + x; endfunction
            endclass
            task automatic t_out(output st_t o, output arr_t a, input int x);
                o.b = o.b + x; a[2] = a[2] + x;
            endtask
            task automatic t_loc(int x);
                st_t l; arr_t a; mix_t mx;
                l.a = l.a + x; a[0] = a[0] + x; mx.b = mx.b + 1; mx.s = {mx.s, "t"};
                $display("t_loc %0d %0d %0d %s", l.a, a[0], mx.b, mx.s);
            endtask
            initial begin
                st_t s1, s2; arr_t a1, a2; la_t l1, l2; ra_t r1, r2; string t1, t2; q_t q1, q2; C c1, c2;
                logic [7:0] p1, p2; bit [7:0] b1, b2; mix_t m1, m2; K k = new; int d;
                d = dirty(0);
                s1 = f_st(3); d = dirty(1); s2 = f_st(4);
                $display("st %0d %0d", s1.a, s2.a);
                a1 = f_ar(3); d = dirty(2); a2 = f_ar(4);
                $display("ar %0d %0d %0d %0d", a1[0], a1[1], a2[0], a2[1]);
                l1 = f_la(0); d = dirty(3); l2 = f_la(0);
                $display("la %b %b %b %b", l1[0], l1[1], l2[0], l2[1]);
                r1 = f_ra(0); d = dirty(4); r2 = f_ra(0);
                $display("ra %0f %0f %0f %0f", r1[0], r1[1], r2[0], r2[1]);
                t1 = f_s("a"); d = dirty(5); t2 = f_s("b");
                $display("s [%s] [%s]", t1, t2);
                q1 = f_q(0); d = dirty(6); q2 = f_q(0);
                $display("q %0d %0d", q1.size(), q2.size());
                c1 = f_c(1); d = dirty(7); c2 = f_c(2);
                $display("c %0d %0d %b", c1.v, c2.v, c1 == c2);
                p1 = f_p(0); d = dirty(8); p2 = f_p(0);
                $display("p %b %b", p1, p2);
                b1 = f_b(0); d = dirty(9); b2 = f_b(0);
                $display("b %b %b", b1, b2);
                m1 = f_mx(0); d = dirty(10); m2 = f_mx(0);
                $display("mx %b %b %0f %0f [%s] [%s] %0d %0d %0d %0d", m1.b, m2.b, m1.r, m2.r, m1.s, m2.s,
                         m1.q.size(), m2.q.size(), m1.in.b, m2.in.b);
                s1 = k.m_st(3); d = dirty(11); s2 = k.m_st(4);
                a1 = k.m_ar(3); d = dirty(12); a2 = k.m_ar(4);
                p1 = k.m_p(0);  d = dirty(13); p2 = k.m_p(0);
                $display("k %0d %0d | %0d %0d | %b %b", s1.a, s2.a, a1[1], a2[1], p1, p2);
                s1 = K::sm_st(3); d = dirty(14); s2 = K::sm_st(4);
                $display("ksm %0d %0d", s1.a, s2.a);
                t_out(s1, a1, 3); d = dirty(15); t_out(s2, a2, 4);
                $display("tout %0d %0d %0d %0d", s1.b, a1[2], s2.b, a2[2]);
                t_loc(1); d = dirty(16); t_loc(2);
                for (int i = 0; i < 2; i++) begin
                    automatic st_t l; automatic arr_t a; automatic string s; automatic q_t q;
                    d = dirty(20 + i);
                    l.a = l.a + 5; a[0] = a[0] + 5; s = {s, "p"}; q.push_back(q.size());
                    $display("loop %0d %0d %0d [%s] %0d", i, l.a, a[0], s, q.size());
                end
            end
        endmodule
    )") == "st 3 4\n"
           "ar 0 3 0 4\n"
           "la xxxx xxxx xxxx xxxx\n"
           "ra 1.500000 0.000000 1.500000 0.000000\n"
           "s [a] [b]\n"
           "q 1 1\n"
           "c 1 2 0\n"
           "p xxxxxxx1 xxxxxxx1\n"
           "b 00000001 00000001\n"
           "mx 0001 0001 2.500000 2.500000 [z] [z] 1 1 1 1\n"
           "k 3 4 | 3 4 | xxxxxxx1 xxxxxxx1\n"
           "ksm 3 4\n"
           "tout 3 3 4 4\n"
           "t_loc 1 1 1 t\n"
           "t_loc 2 2 1 t\n"
           "loop 0 5 5 [p] 1\n"
           "loop 1 5 5 [p] 1\n");
}
