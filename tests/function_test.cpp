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
std::string reject(std::string_view source) {
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

// A static local with an initializer needs once-at-time-zero initialization,
// which is not modeled; reject it rather than re-init on every call.
TEST_CASE("static local with an initializer is rejected") {
    constexpr std::string_view src = R"(
        module m;
            function int f();
                static int c = 0;
                c = c + 1;
                return c;
            endfunction
            int y;
            initial y = f();
        endmodule
    )";
    CHECK(reject(src).find("static function local with an initializer") != std::string::npos);
}

// Output / inout / ref formal arguments need copy-out / aliasing and are not
// implemented.
TEST_CASE("function with an output argument is rejected") {
    constexpr std::string_view src = R"(
        module m;
            function void dbl(input int a, output int o); o = a * 2; endfunction
            int y;
            initial dbl(3, y);
        endmodule
    )";
    CHECK(reject(src).find("output/inout/ref") != std::string::npos);
}

// A task enable consumes time / needs the scheduler; only functions are lowered.
TEST_CASE("task enable is rejected") {
    constexpr std::string_view src = R"(
        module m;
            task t(input int a); endtask
            initial t(3);
        endmodule
    )";
    CHECK(reject(src).find("task enable is not supported") != std::string::npos);
}
