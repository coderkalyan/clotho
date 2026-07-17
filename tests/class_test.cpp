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

TEST_CASE("class static property: shared slot, inherited, all access forms") {
    constexpr std::string_view src = R"(
        module m;
            class Counter;
                static int count = 0;
                int id;
                function new(); count = count + 1; id = count; endfunction
                static function int get_count(); return count; endfunction
            endclass
            class Derived extends Counter;
                int extra;
            endclass
            initial begin
                Counter a = new();
                Counter b = new();
                Derived c = new();
                Counter::count = Counter::count + 100;
                $display("%0d %0d %0d %0d %0d %0d",
                         a.id, b.id, c.id, a.count, Counter::count, Counter::get_count());
            end
        endmodule
    )";
    // ids 1 2 3 (shared, inherited); then count 3->103 read three ways
    CHECK(run_capture(src) == "1 2 3 103 103 103\n");
}

// A static property of a class that is never instantiated still initializes once
// (lazily, in PreInit); `local`/`static const` carry no runtime obligation.
TEST_CASE("class static property: no instance, const, local") {
    constexpr std::string_view src = R"(
        module m;
            class Config;
                static int limit = 42;
                static const int MAX = 99;
                local static int secret = 7;
                static function int reveal(); return secret; endfunction
            endclass
            initial begin
                Config::limit = Config::limit + 1;
                $display("%0d %0d %0d", Config::limit, Config::MAX, Config::reveal());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "43 99 7\n");
}

TEST_CASE("class virtual dispatch: override, inherit, internal call, super") {
    constexpr std::string_view src = R"(
        module m;
            class Animal;
                virtual function int legs(); return 0; endfunction
                function int describe(); return legs() + 1; endfunction
                virtual function int noise(); return 1; endfunction
            endclass
            class Dog extends Animal;
                virtual function int legs(); return 4; endfunction
                virtual function int noise(); return 10 + super.noise(); endfunction
            endclass
            class Cat extends Animal;
                function int legs(); return 40; endfunction
            endclass
            initial begin
                Animal a;
                Dog    d = new();
                Cat    c = new();
                a = d;
                $display("%0d %0d %0d", a.legs(), a.describe(), a.noise());
                a = c;
                $display("%0d %0d %0d", a.legs(), a.describe(), a.noise());
                a = new();
                $display("%0d", a.legs());
            end
        endmodule
    )";
    // Dog: legs 4, describe legs()+1=5, noise 10+super(1)=11
    // Cat: legs 40, describe 41, noise inherited Animal 1
    // Animal: legs 0
    CHECK(run_capture(src) == "4 5 11\n40 41 1\n0\n");
}

// A newly-introduced virtual method in a derived class (appended vtable slot),
// dispatched through the derived-static handle, alongside an inherited slot.
TEST_CASE("class virtual dispatch: derived-introduced virtual, multi-slot") {
    constexpr std::string_view src = R"(
        module m;
            class Base;
                virtual function int a(); return 1; endfunction
            endclass
            class Mid extends Base;
                virtual function int a(); return 2; endfunction
                virtual function int b(); return 20; endfunction
            endclass
            initial begin
                Base bh;
                Mid  m = new();
                bh = m;
                $display("%0d %0d", bh.a(), m.b());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 20\n");
}

TEST_CASE("class virtual dispatch: string return") {
    constexpr std::string_view src = R"(
        module m;
            class Base;    virtual function string name(); return "base";    endfunction endclass
            class Derived extends Base;
                virtual function string name(); return "derived"; endfunction
            endclass
            initial begin
                Base b;
                Derived d = new();
                b = new(); $display("%s", b.name());
                b = d;     $display("%s", b.name());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "base\nderived\n");
}

TEST_CASE("class virtual dispatch: unpacked struct return") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int x; int y; } pt;
            class Base;
                virtual function pt origin(); pt p; p.x = 0; p.y = 0; return p; endfunction
            endclass
            class Shifted extends Base;
                virtual function pt origin(); pt p; p.x = 10; p.y = 20; return p; endfunction
            endclass
            initial begin
                Base    b;
                Shifted s = new();
                pt      r;
                b = new(); r = b.origin(); $display("%0d %0d", r.x, r.y);
                b = s;     r = b.origin(); $display("%0d %0d", r.x, r.y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 0\n10 20\n");
}

TEST_CASE("class virtual task: dynamic dispatch, time-consuming") {
    constexpr std::string_view src = R"(
        module m;
            class Base;
                virtual task run(); $display("base run"); endtask
                virtual task step(int n); #(n) $display("base step %0t", $time); endtask
                task go(); run(); step(5); endtask
            endclass
            class Der extends Base;
                virtual task run(); $display("der run"); endtask
                virtual task step(int n); #(n*2) $display("der step %0t", $time); endtask
            endclass
            initial begin
                Base b;
                Der  d = new();
                b = new(); b.go();
                b = d;     b.go();
            end
        endmodule
    )";
    // base go: run + #5 step (t=5). der go: dispatched run + #10 step (t=5+10=15).
    CHECK(run_capture(src) == "base run\nbase step 5\nder run\nder step 15\n");
}

// Pure virtual task's abstract-class slot would need a coroutine-shaped stub
// (deferred); it must fail loudly.
TEST_CASE("class pure virtual task fails loudly") {
    const std::string d = fails(R"(
        module m;
            virtual class Base; pure virtual task t(); endclass
            class Der extends Base; task t(); #1; endtask endclass
            initial begin Der x; x = new(); end
        endmodule
    )");
    CHECK(d.find("pure virtual task is not supported") != std::string::npos);
}

TEST_CASE("abstract class: concrete virtual dispatch through the base") {
    constexpr std::string_view src = R"(
        module m;
            virtual class Shape;
                virtual function int area(); return 0; endfunction
                virtual function int kind(); return 0; endfunction
                function int describe(); return area() + kind(); endfunction
            endclass
            class Square extends Shape;
                int s;
                function new(int side); s = side; endfunction
                virtual function int area(); return s * s; endfunction
                virtual function int kind(); return 1; endfunction
            endclass
            class Circle extends Shape;
                int r;
                function new(int rad); r = rad; endfunction
                virtual function int area(); return 3 * r * r; endfunction
            endclass
            initial begin
                Shape  sh;
                Square sq = new(5);
                Circle ci = new(2);
                sh = sq; $display("%0d %0d %0d", sh.area(), sh.kind(), sh.describe());
                sh = ci; $display("%0d %0d %0d", sh.area(), sh.kind(), sh.describe());
            end
        endmodule
    )";
    // Square: area 25, kind 1, describe 26. Circle: area 12, kind inherited 0, describe 12.
    CHECK(run_capture(src) == "25 1 26\n12 0 12\n");
}

TEST_CASE("pure virtual: dispatch through the abstract base, no args") {
    constexpr std::string_view src = R"(
        module m;
            virtual class Shape;
                pure virtual function int area();
                virtual function int kind(); return 0; endfunction
                function int describe(); return area() + kind(); endfunction
            endclass
            class Square extends Shape;
                int s;
                function new(int side); s = side; endfunction
                virtual function int area(); return s * s; endfunction
                virtual function int kind(); return 1; endfunction
            endclass
            class Circle extends Shape;
                int r;
                function new(int rad); r = rad; endfunction
                virtual function int area(); return 3 * r * r; endfunction
            endclass
            initial begin
                Shape  sh;
                Square sq = new(5);
                Circle ci = new(2);
                sh = sq; $display("%0d %0d %0d", sh.area(), sh.kind(), sh.describe());
                sh = ci; $display("%0d %0d %0d", sh.area(), sh.kind(), sh.describe());
            end
        endmodule
    )";
    // Square: area 25, kind 1, describe 26. Circle: area 12, kind inherited 0, describe 12.
    CHECK(run_capture(src) == "25 1 26\n12 0 12\n");
}

TEST_CASE("pure virtual: dispatch with arguments") {
    constexpr std::string_view src = R"(
        module m;
            virtual class Op;
                pure virtual function int apply(int a, int b);
                function int twice(int x); return apply(x, x); endfunction
            endclass
            class Add extends Op; virtual function int apply(int a, int b); return a + b; endfunction endclass
            class Mul extends Op; virtual function int apply(int a, int b); return a * b; endfunction endclass
            initial begin
                Op  o;
                Add a = new();
                Mul mm = new();
                o = a;  $display("%0d %0d", o.apply(3, 4), o.twice(5));
                o = mm; $display("%0d %0d", o.apply(3, 4), o.twice(5));
            end
        endmodule
    )";
    // Add: 3+4=7, twice(5)=5+5=10. Mul: 3*4=12, twice(5)=5*5=25.
    CHECK(run_capture(src) == "7 10\n12 25\n");
}

TEST_CASE("pure virtual: multi-level abstract, implicit override") {
    constexpr std::string_view src = R"(
        module m;
            virtual class A; pure virtual function int v(); endclass
            virtual class B extends A; virtual function int w(); return 100; endfunction endclass
            class C extends B; function int v(); return 7; endfunction endclass
            initial begin
                A a;
                B b;
                C c = new();
                a = c; $display("%0d", a.v());
                b = c; $display("%0d %0d", b.v(), b.w());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "7\n7 100\n");
}

// A pure virtual returning a non-scalar needs the aggregate/handle return ABI in
// the never-called signature stub (deferred); it must fail loudly.
TEST_CASE("pure virtual with non-scalar return fails loudly") {
    const std::string d = fails(R"(
        module m;
            virtual class Base; pure virtual function string name(); endclass
            class Derived extends Base; function string name(); return "d"; endfunction endclass
            initial begin Derived dd; dd = new(); $display("%s", dd.name()); end
        endmodule
    )");
    CHECK(d.find("pure virtual method with a non-scalar return is not supported") != std::string::npos);
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
