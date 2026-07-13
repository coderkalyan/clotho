#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

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

std::string fails(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

// A single object: allocate with `new`, run the constructor, read/write a field
// through the handle (LRM 8.4/8.7). Cross-checked vs iverilog.
TEST_CASE("class new, constructor, field read/write") {
    constexpr std::string_view src = R"(
        module m;
            class Packet;
                int addr;
                int data;
                function new(int a); addr = a; data = 0; endfunction
            endclass
            initial begin
                Packet p;
                p = new(5);
                p.data = 7;
                $display("%0d %0d", p.addr, p.data);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "5 7\n");
}

TEST_CASE("handle assignment shares the object") {
    constexpr std::string_view src = R"(
        module m;
            class C; int f; endclass
            initial begin
                C h1, h2;
                h1 = new();
                h2 = h1;
                h1.f = 5;
                $display("%0d", h2.f);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "5\n");
}

TEST_CASE("null and handle identity") {
    constexpr std::string_view src = R"(
        module m;
            class C; int f; endclass
            initial begin
                C a, b, n;
                a = new();
                b = a;
                $display("%b %b %b %b", a == b, a != n, n == null, a == null);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 1 0\n");
}

TEST_CASE("self-referential class and nested member path") {
    constexpr std::string_view src = R"(
        module m;
            class Node; int val; Node next; endclass
            initial begin
                Node a, b;
                a = new(); b = new();
                a.val = 10; b.val = 20;
                a.next = b;
                a.next.val = 99;
                $display("%0d %0d %b", a.next.val, b.val, a.next == null);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "99 99 0\n");
}

TEST_CASE("property default initializers") {
    constexpr std::string_view src = R"(
        module m;
            class A;
                int x = 100;
                string s = "ctr";
                int y = 200;
            endclass
            class B;
                int a = 7;
                int b;
                function new(); b = 9; endfunction
            endclass
            initial begin
                A o; B q;
                o = new(); q = new();
                $display("%0d %s %0d", o.x, o.s, o.y);
                $display("%0d %0d", q.a, q.b);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "100 ctr 200\n7 9\n");
}

// Methods: implicit and explicit `this`, one method calling another, and recursion
// (LRM 8.11). Cross-checked vs iverilog.
TEST_CASE("methods: this, method-to-method, recursion") {
    constexpr std::string_view src = R"(
        module m;
            class Counter;
                int count = 100;
                function void inc(); count = count + 1; endfunction
                function void inc2(); inc(); inc(); endfunction
                function int get(); return this.count; endfunction
                function int fact(int n);
                    if (n <= 1) return 1;
                    return n * fact(n - 1);
                endfunction
            endclass
            initial begin
                Counter c;
                c = new();
                c.inc();
                c.inc2();
                $display("%0d %0d %0d", c.get(), c.count, c.fact(5));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "103 103 120\n");
}

// A task method blocks and mutates through `this`; a handle passed to a free
// function is passed by value (the pointer), so the callee sees the same object.
// Cross-checked vs iverilog.
TEST_CASE("task method and handle as a function argument") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                int v;
                task set(int x); v = x; endtask
            endclass
            function automatic int peek(C h); return h.v; endfunction
            initial begin
                C c;
                c = new();
                c.set(42);
                $display("%0d", peek(c));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "42\n");
}

TEST_CASE("property power-on defaults by domain") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                logic [7:0] g;
                int i;
            endclass
            initial begin
                C c;
                c = new();
                $display("%h %0d", c.g, c.i);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "xx 0\n");
}

// Deferred Phase 2/3 features must fail loudly, not silently miscompile.
// Inheritance is the representative guard.
TEST_CASE("class inheritance fails loudly") {
    const std::string d = fails(R"(
        module m;
            class Base; int a; endclass
            class Derived extends Base; int b; endclass
            initial begin Derived d; d = new(); d.b = 1; end
        endmodule
    )");
    CHECK(d.find("class inheritance is not supported") != std::string::npos);
}
