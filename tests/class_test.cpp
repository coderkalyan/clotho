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

// Inherited field + own field, an inherited method run on a derived object, and
// a non-virtual override selected by the STATIC handle type (the reference test
// distinguishing non-virtual binding from dispatch, docs/design/classes.md §10):
// `bh` points at the derived object yet `bh.who()` calls Base::who.
TEST_CASE("class inheritance: fields, inherited method, non-virtual override") {
    constexpr std::string_view src = R"(
        module m;
            class Base;
                int a;
                function new(int x); a = x; endfunction
                function int f(); return a + 1; endfunction
                function int who(); return 100; endfunction
            endclass
            class Derived extends Base;
                int b;
                function new(int x, int y); super.new(x); b = y; endfunction
                function int who(); return 200; endfunction
                function int sum(); return f() + b + super.who(); endfunction
            endclass
            initial begin
                Derived d = new(5, 7);
                Base    bh = d;
                $display("%0d %0d %0d %0d %0d %0d %0d",
                         d.a, d.b, d.f(), d.who(), bh.who(), d.sum(), bh.a);
            end
        endmodule
    )";
    // a=5 b=7 f=6 d.who=200 bh.who=100 sum=6+7+100=113 upcast a=5
    CHECK(run_capture(src) == "5 7 6 200 100 113 5\n");
}

// Three-level chain with an implicit (constructor-less) middle class, chained
// property defaults, and a constructor body that observes its own default
// (A::new does a=a+1 over the default 11 -> 12). Exercises the implicit no-arg
// base construction emitted at the constructor entry.
TEST_CASE("class inheritance: multi-level chain, implicit ctor, chained defaults") {
    constexpr std::string_view src = R"(
        module m;
            class A;
                int a = 11;
                function new(); a = a + 1; endfunction
                function int ga(); return a; endfunction
            endclass
            class B extends A;
                int b = 22;
                function int gb(); return b; endfunction
            endclass
            class C extends B;
                int c = 33;
                function new(int z); super.new(); c = z; endfunction
                function int total(); return ga() + gb() + c; endfunction
            endclass
            initial begin
                C obj = new(99);
                A up  = obj;
                $display("%0d %0d %0d %0d %0d",
                         obj.a, obj.b, obj.c, obj.total(), up.ga());
            end
        endmodule
    )";
    // a=12 b=22 c=99 total=12+22+99=133 up.ga()=12 (shared object via upcast)
    CHECK(run_capture(src) == "12 22 99 133 12\n");
}

TEST_CASE("class inheritance: extends-args base constructor") {
    constexpr std::string_view src = R"(
        module m;
            class A;
                int a;
                function new(int x); a = x * 10; endfunction
                function int ga(); return a; endfunction
            endclass
            class B2 extends A(7);
                int c = 5;
            endclass
            class B3 extends A(9);
                int d;
                function new(int q); d = q; endfunction
            endclass
            initial begin
                B2 y = new();
                B3 z = new(4);
                $display("%0d %0d %0d %0d", y.ga(), y.c, z.ga(), z.d);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "70 5 90 4\n");
}

// Reference semantics survive an upcast: a write through the derived handle is
// visible through a base handle to the same object (LRM 8.6 shared object).
TEST_CASE("class inheritance: upcast shares the object") {
    constexpr std::string_view src = R"(
        module m;
            class Base; int a; endclass
            class Derived extends Base; int b; endclass
            initial begin
                Derived d = new();
                Base    bh = d;
                d.a = 9;
                bh.a = bh.a + 1;
                $display("%0d %0d", d.a, bh.a);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10 10\n");
}

// A virtual method needs dynamic dispatch (Phase 3, vtable); it must fail loudly,
// not silently lower as a direct call. The implicit built-in
// `randomize` virtual must NOT trip this guard.
TEST_CASE("class virtual method fails loudly") {
    const std::string d = fails(R"(
        module m;
            class Base; virtual function int f(); return 1; endfunction endclass
            class Derived extends Base; function int f(); return 2; endfunction endclass
            initial begin Derived d; d = new(); $display("%0d", d.f()); end
        endmodule
    )");
    CHECK(d.find("virtual methods are not supported") != std::string::npos);
}

// Two classes forward-referencing each other's handle type: A's field type B
// is declared before B exists, and B's method takes an A argument before A's
// body (only its header) is complete.
TEST_CASE("forward typedef: two classes with mutual handle references") {
    constexpr std::string_view src = R"(
        module m;
            typedef class B;
            class A;
                B b;
                int x;
                function new(); x = 1; endfunction
                function void link(B other); b = other; endfunction
                function int read_b(); return b.y; endfunction
            endclass
            class B;
                A a;
                int y;
                function new(int yy); y = yy; endfunction
            endclass
            initial begin
                A obj; B bobj;
                obj = new(); bobj = new(42);
                obj.link(bobj);
                $display("%0d %0d %b", obj.x, obj.read_b(), obj.b == null);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 42 0\n");
}

// A three-way forward-reference cycle (A -> B -> C -> A), each needing only
// the others' names, not their bodies, at declaration time.
TEST_CASE("forward typedef: three-way mutual reference cycle") {
    constexpr std::string_view src = R"(
        module m;
            typedef class B;
            typedef class C;
            class A; B b; int x; function new(); x = 1; endfunction endclass
            class B; C c; int y; function new(); y = 2; endfunction endclass
            class C; A a; int z; function new(); z = 3; endfunction endclass
            initial begin
                A obj;
                obj = new();
                $display("%0d", obj.x);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// LRM 6.18: "legal to have a forward type declaration in the same scope,
// either before or after the final type definition" -- a forward decl
// trailing its own definition is a legal no-op.
TEST_CASE("forward typedef: declaration after the definition is legal") {
    constexpr std::string_view src = R"(
        module m;
            class D; int w; function new(); w = 7; endfunction endclass
            typedef class D;
            initial begin
                D d;
                d = new();
                $display("%0d", d.w);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "7\n");
}

// LRM 6.18: "legal to have multiple forward type declarations for the same
// type identifier in the same scope".
TEST_CASE("forward typedef: duplicate forward declarations are legal") {
    constexpr std::string_view src = R"(
        module m;
            typedef class E;
            typedef class E;
            class E; int v; function new(); v = 9; endfunction endclass
            initial begin
                E e;
                e = new();
                $display("%0d", e.v);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "9\n");
}

// LRM 6.18: "It shall be an error if the type_identifier does not resolve to
// a data type" -- a forward decl with no matching definition anywhere in
// scope is rejected (by slang, at bind time).
TEST_CASE("forward typedef: an unresolved forward declaration is an error") {
    const std::string d = fails(R"(
        module m;
            typedef class F;
            initial $display("unreachable");
        endmodule
    )");
    CHECK(d.find("forward typedef") != std::string::npos);
}
