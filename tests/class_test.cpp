#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
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

std::string fails(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostic_text();
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

TEST_CASE("property initializer calling a method of the same class") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                int a = f();
                int x = 3;
                int y = g();
                function int f(); return 7; endfunction
                function int g(); return x + 4; endfunction
            endclass
            initial begin
                C o;
                o = new();
                $display("%0d %0d %0d", o.a, o.x, o.y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "7 3 7\n");
}

// The handle such an initializer calls through is the object UNDER CONSTRUCTION, not
// whatever `this` the enclosing scope happens to have. Here the `new` sits inside
// Maker's method, so Maker's own `this` is live at the call site; C::g must still
// read C's `x` (3), never Maker's same-named property (100).
TEST_CASE("property initializer method call from inside another object's method") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                int x = 3;
                int y = g();
                function int g(); return x + 4; endfunction
            endclass
            class Maker;
                int x = 100;
                function C make();
                    C c;
                    c = new();
                    return c;
                endfunction
            endclass
            initial begin
                Maker mk;
                C o;
                mk = new();
                o = mk.make();
                $display("%0d %0d", o.x, o.y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 7\n");
}

// The same rule for a property initializer that READS a sibling property instead
// of calling a method (LRM 8.7: the initializer runs during construction, so a
// bare name is `this`-relative to the object being built). Both constructions
// are exercised, because they used to fail differently: at module scope the read
// had no frame `this` at all and died as an unknown signal, while inside Maker's
// method it silently read MAKER's `a` and printed 101. iverilog 13 prints 5/6 for
// both.
TEST_CASE("property initializer reading a sibling property of the same object") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                int a = 5;
                int b = a + 1;
            endclass
            class Maker;
                int a = 100;
                function C make();
                    C c;
                    c = new();
                    return c;
                endfunction
            endclass
            initial begin
                Maker mk;
                C direct, viamk;
                mk = new();
                direct = new();
                viamk = mk.make();
                $display("%0d %0d %0d %0d", direct.a, direct.b, viamk.a, viamk.b);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "5 6 5 6\n");
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

TEST_CASE("class static property: aggregate and container initializers") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a; int b; } P;
            class C;
                static int arr[4] = '{1,2,3,4};
                static string s = "hi";
                static P st = '{11,22};
                static int q[$] = '{5,6};
                static int d[] = '{7,8,9};
            endclass
            initial begin
                C o;
                o = new();
                $display("%0d %0d %0d %0d %s", C::arr[0], C::arr[1], C::arr[2], C::arr[3], C::s);
                $display("%0d %0d %0d %0d %0d %0d %0d",
                         C::st.a, C::st.b, C::q[0], C::q[1], C::d[0], C::d[1], C::d[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 3 4 hi\n11 22 5 6 7 8 9\n");
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

TEST_CASE("pure virtual task: dynamic dispatch, time-consuming") {
    constexpr std::string_view src = R"(
        module m;
            virtual class Base;
                pure virtual task t(int x);
                task go(); t(3); $display("go %0t", $time); endtask
            endclass
            class Der extends Base;
                virtual task t(int x); #(x) $display("der t %0d %0t", x, $time); endtask
            endclass
            class Twice extends Base;
                virtual task t(int x); #(x*2) $display("twice t %0d %0t", x, $time); endtask
            endclass
            initial begin
                Base  b;
                Der   d = new();
                Twice w = new();
                b = d; b.t(4);
                b = w; b.t(4);
                b = d; b.go();
            end
        endmodule
    )";
    // Der t(4): #4 -> t=4. Twice t(4): #8 -> t=12. go: dispatched t(3), #3 -> t=15.
    CHECK(run_capture(src) == "der t 4 4\ntwice t 4 12\nder t 3 15\ngo 15\n");
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

TEST_CASE("pure virtual: string return") {
    constexpr std::string_view src = R"(
        module m;
            virtual class Named;
                pure virtual function string name();
                function string shout(); return {name(), "!"}; endfunction
            endclass
            class Dog extends Named; virtual function string name(); return "dog"; endfunction endclass
            class Cat extends Named; function string name(); return "cat"; endfunction endclass
            initial begin
                Named n;
                Dog   d = new();
                Cat   c = new();
                n = d; $display("%s %s", n.name(), n.shout());
                n = c; $display("%s %s", n.name(), n.shout());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "dog dog!\ncat cat!\n");
}

TEST_CASE("pure virtual: unpacked struct return") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int x; int y; } pt;
            virtual class Maker;
                pure virtual function pt make();
            endclass
            class Origin extends Maker;
                virtual function pt make(); pt p; p.x = 0; p.y = 0; return p; endfunction
            endclass
            class Shifted extends Maker;
                virtual function pt make(); pt p; p.x = 10; p.y = 20; return p; endfunction
            endclass
            initial begin
                Maker   mk;
                Origin  o = new();
                Shifted s = new();
                pt      r;
                mk = o; r = mk.make(); $display("%0d %0d", r.x, r.y);
                mk = s; r = mk.make(); $display("%0d %0d", r.x, r.y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 0\n10 20\n");
}

TEST_CASE("pure virtual: queue return") {
    constexpr std::string_view src = R"(
        module m;
            typedef int q_t[$];
            virtual class Src;
                pure virtual function q_t items();
                function int total(); q_t v; int s; s = 0; v = items(); foreach (v[i]) s += v[i]; return s; endfunction
            endclass
            class Two extends Src;
                virtual function q_t items(); q_t r; r.push_back(1); r.push_back(2); return r; endfunction
            endclass
            class Three extends Src;
                virtual function q_t items(); q_t r; r.push_back(7); r.push_back(8); r.push_back(9); return r; endfunction
            endclass
            initial begin
                Src   s;
                Two   a = new();
                Three b = new();
                q_t   v;
                s = a; v = s.items(); $display("%0d %0d %0d", v.size(), v[1], s.total());
                s = b; v = s.items(); $display("%0d %0d %0d", v.size(), v[2], s.total());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 2 3\n3 9 24\n");
}

// The one pure virtual return shape still deferred: an aggregate that owns heap (a
// string member) returns by value, which needs a per-leaf deep copy rather than the
// memcpy the return ABI emits. build_pure_stub guards it exactly as build_function
// guards a concrete body, so the declaration is rejected whichever is lowered first.
TEST_CASE("pure virtual returning a string-containing aggregate fails loudly") {
    const std::string d = fails(R"(
        module m;
            typedef struct { string s; int i; } rec;
            virtual class Base; pure virtual function rec f(); endclass
            class Der extends Base;
                virtual function rec f(); rec r; r.s = "x"; r.i = 1; return r; endfunction
            endclass
            initial begin Der d; d = new(); end
        endmodule
    )");
    CHECK(d.find("by-value return of a string-containing aggregate is not supported") != std::string::npos);
}

TEST_CASE("parameterized class: type + value params, param-sized array") {
    constexpr std::string_view src = R"(
        module m;
            class Vec #(type T = int, int N = 4);
                T   data[N];
                function void set(int i, T v); data[i] = v; endfunction
                function T    get(int i);       return data[i]; endfunction
                function int  size();           return N; endfunction
            endclass
            initial begin
                Vec#(int, 3)  vi = new();
                Vec#(byte, 2) vb = new();
                vi.set(0, 100); vi.set(1, 200);
                vb.set(0, 8'sd5);
                $display("%0d %0d %0d", vi.get(0), vi.get(1), vi.size());
                $display("%0d %0d", vb.get(0), vb.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "100 200 3\n5 2\n");
}

// A derived class extending a specific specialization (`extends base#(5)`); the
// value parameter flows into a member initializer.
TEST_CASE("parameterized class: inheritance from a specialization") {
    constexpr std::string_view src = R"(
        module m;
            class base_cls #(int b = 20); int a = b; endclass
            class ext_cls #(int e = 25) extends base_cls #(5); int c = e; endclass
            initial begin
                ext_cls #(15) inst = new();
                $display("%0d %0d", inst.a, inst.c);
            end
        endmodule
    )";
    // base#(5) -> a=5; ext#(15) -> c=15.
    CHECK(run_capture(src) == "5 15\n");
}

// Each specialization has its own static storage (LRM 8.25): distinct
// ClassPropertySymbols -> distinct signals. Read via `C#(N)::s`.
TEST_CASE("parameterized class: per-specialization static members") {
    constexpr std::string_view src = R"(
        module m;
            class Counter #(int ID = 0);
                static int count = 0;
                function new(); count++; endfunction
            endclass
            initial begin
                Counter#(1) a = new();
                Counter#(1) b = new();
                Counter#(2) c = new();
                $display("%0d %0d", Counter#(1)::count, Counter#(2)::count);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 1\n");
}

// A value parameter read through an instance (`obj.param`, LRM 8.5) and via scope
// resolution (`C#(N)::param`) — both fold to the specialization's constant.
TEST_CASE("parameterized class: value parameter access") {
    constexpr std::string_view src = R"(
        module m;
            class C #(int W = 8); int x = W; endclass
            initial begin
                C#(34) o = new();
                $display("%0d %0d", o.W, C#(16)::W);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "34 16\n");
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

// LRM 8.4: "Accessing non-static members ... via a null object handle is
// illegal. The result of an illegal access via a null object handle is
// indeterminate, and implementations may issue an error." A class handle is raw
// pointer bits, so the untrapped outcome is a load from address zero and a
// segfault naming nothing; clotho guards every dereference instead and ends the
// run with a located fatal. Out of process because the fatal exits.
TEST_CASE("null handle dereference is a located fatal, not a segfault") {
    constexpr std::string_view src = "module m;\n"
                                     "    class Packet;\n"
                                     "        int id;\n"
                                     "    endclass\n"
                                     "    Packet p;\n"
                                     "    initial begin\n"
                                     "        $display(\"before\");\n"
                                     "        p.id = 1;\n"
                                     "        $display(\"after\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    // The store never happens, and nothing after it runs.
    CHECK(run.out == "before\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("fatal: dereference of a null class handle") != std::string::npos);
    // The caret lands on the handle expression, not on the whole `p.id` member
    // access: the report has to name what was null.
    CHECK(run.err.find("source:8:9") != std::string::npos);
    CHECK(run.err.find("8 |         p.id = 1;\n") != std::string::npos);
    CHECK(run.err.find("  |         ^\n") != std::string::npos);
}

// A method called on a null handle passes null as `this`, so the guard fires at
// the first property the body touches -- inside the callee, where the source
// spells no handle at all. The report then points at the property reference.
TEST_CASE("a method reached through a null handle traps at its property access") {
    constexpr std::string_view src = "module m;\n"
                                     "    class Counter;\n"
                                     "        int n;\n"
                                     "        function void bump();\n"
                                     "            n = n + 1;\n"
                                     "        endfunction\n"
                                     "    endclass\n"
                                     "    Counter c;\n"
                                     "    initial begin\n"
                                     "        c = null;\n"
                                     "        c.bump();\n"
                                     "        $display(\"after\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out.empty());
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("fatal: dereference of a null class handle") != std::string::npos);
    CHECK(run.err.find("5 |             n = n + 1;\n") != std::string::npos);
}

// The guard is a null test, not a "was it ever new'd" test: an explicit
// `h = null` after a live object is just as fatal, and a handle that holds an
// object stays untouched by the check.
TEST_CASE("a handle nulled after use is fatal; a live one is not") {
    constexpr std::string_view live = R"(
        module m;
            class C; int v; endclass
            initial begin
                C a, b;
                a = new();
                a.v = 4;
                b = a;
                b.v = b.v + 1;
                $display("%0d", a.v);
            end
        endmodule
    )";
    CHECK(run_capture(live) == "5\n");

    constexpr std::string_view nulled = "module m;\n"
                                        "    class C;\n"
                                        "        int v;\n"
                                        "    endclass\n"
                                        "    initial begin\n"
                                        "        C a;\n"
                                        "        a = new();\n"
                                        "        a.v = 4;\n"
                                        "        $display(\"%0d\", a.v);\n"
                                        "        a = null;\n"
                                        "        a.v = 5;\n"
                                        "    end\n"
                                        "endmodule\n";

    const test::DesignRun run = test::run_design_captured(nulled);
    CHECK(run.out == "4\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("11 |         a.v = 5;\n") != std::string::npos);
}

// LRM 8.4 puts a virtual method reached through a null handle in the same class
// of illegal accesses as a property. The vtable load is the first thing that
// would fault, so the receiver takes the guard before it — and the caret lands
// on the call site, which is where the design named the null handle.
TEST_CASE("virtual dispatch through a null handle is a located fatal") {
    constexpr std::string_view src = "module m;\n"
                                     "    class Base;\n"
                                     "        virtual function int tag();\n"
                                     "            return 1;\n"
                                     "        endfunction\n"
                                     "    endclass\n"
                                     "    Base b;\n"
                                     "    initial begin\n"
                                     "        $display(\"before\");\n"
                                     "        $display(\"%0d\", b.tag());\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out == "before\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("fatal: dereference of a null class handle") != std::string::npos);
    CHECK(run.err.find("10 |         $display(\"%0d\", b.tag());\n") != std::string::npos);
}

// `semaphore` (LRM 15.3), `mailbox` (LRM 15.4), `process` (LRM 9.7) and
// `std::weak_reference` (LRM 8.30) are classes slang synthesizes into the `std`
// package with signatures but no bodies -- the simulator is expected to supply
// the semantics. Clotho has no runtime for any of them, so each
// must be refused in the frontend. Before this guard, lowering a `semaphore`
// bound a bodyless method with no formals and died on an out-of-range formal
// index; `mailbox` reached lower_type's catch-all and said only "unsupported
// non-integral type".
TEST_CASE("built-in std classes are refused in the frontend") {
    // A declaration is enough: the type lowers before any method is reached.
    CHECK(fails("module m; semaphore s; initial s = new(1); endmodule")
            .find("the built-in 'semaphore' class is not supported") != std::string::npos);
    CHECK(fails("module m; mailbox b; initial b = new(); endmodule")
            .find("the built-in 'mailbox' class is not supported") != std::string::npos);
    // A specialization and an explicit `std::` qualification are the same type.
    CHECK(fails("module m; mailbox #(int) b; initial b = new(2); endmodule")
            .find("the built-in 'mailbox' class is not supported") != std::string::npos);
    CHECK(fails("module m; std::semaphore s; initial s = new(1); endmodule")
            .find("the built-in 'semaphore' class is not supported") != std::string::npos);
    CHECK(fails("module m; initial begin process p; p = process::self(); end endmodule")
            .find("the built-in 'process' class is not supported") != std::string::npos);
    // Reached indirectly: as a property of a user class, and as a formal.
    CHECK(fails("module m; class H; semaphore lock; endclass H h; initial h = new; endmodule")
            .find("the built-in 'semaphore' class is not supported") != std::string::npos);
    CHECK(fails("module m; task automatic t(mailbox b); endtask"
                " mailbox g; initial begin g = new(); t(g); end endmodule")
            .find("the built-in 'mailbox' class is not supported") != std::string::npos);
}

// -- shallow copy, `dst = new src` (LRM 8.12) ---------------------------------

TEST_CASE("class shallow copy: values are independent, handles are shared") {
    constexpr std::string_view src = R"(
        module m;
            class baseA; integer j = 5; endclass
            class B;
                integer i = 1;
                baseA a = new;
            endclass
            initial begin
                B b1, b2;
                b1 = new;
                b2 = new b1;
                b2.i = 10;
                b2.a.j = 50;
                $display("%0d %0d %0d %b", b1.i, b1.a.j, b2.i, b1.a == b2.a);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 50 10 1\n");
}

TEST_CASE("class shallow copy: no constructor, no declaration initializers") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                int i = 1;
                function new(); $display("ctor"); endfunction
            endclass
            initial begin
                C c1, c2;
                c1 = new;
                c1.i = 99;
                c2 = new c1;
                $display("%0d", c2.i);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ctor\n99\n");
}

TEST_CASE("class shallow copy: string, queue, dynamic and associative members copy by value") {
    constexpr std::string_view src = R"(
        module m;
            class Bag;
                string s;
                int    q[$];
                int    d[];
                int    a[string];
            endclass
            initial begin
                Bag g1, g2;
                g1 = new;
                g1.s = "abc";
                g1.q.push_back(7);
                g1.d = new[2];
                g1.d[0] = 3;
                g1.a["k"] = 4;
                g2 = new g1;
                g2.s = "xyz";
                g2.q.push_back(8);
                g2.d[0] = 30;
                g2.a["k"] = 40;
                $display("%s %0d %0d %0d", g1.s, g1.q.size(), g1.d[0], g1.a["k"]);
                $display("%s %0d %0d %0d", g2.s, g2.q.size(), g2.d[0], g2.a["k"]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "abc 1 3 4\nxyz 2 30 40\n");
}

TEST_CASE("class shallow copy: through a base handle copies the derived object") {
    constexpr std::string_view src = R"(
        module m;
            class Base;
                int bi = 1;
                virtual function string who(); return "Base"; endfunction
            endclass
            class Derived extends Base;
                int di = 2;
                virtual function string who(); return "Derived"; endfunction
            endclass
            initial begin
                Base    pb, pc;
                Derived d1;
                d1 = new;
                d1.bi = 11;
                d1.di = 22;
                pb = d1;
                pc = new pb;
                $display("%s %0d", pc.who(), pc.bi);
                pc.bi = 111;
                $display("%0d %0d", d1.bi, pc.bi);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "Derived 11\n11 111\n");
}

TEST_CASE("class shallow copy: through an abstract handle reaches the concrete class") {
    constexpr std::string_view src = R"(
        module m;
            virtual class Abs;
                int ai = 1;
                pure virtual function int tag();
            endclass
            class Mid extends Abs;
                int mi = 2;
                virtual function int tag(); return 20; endfunction
            endclass
            class Leaf extends Mid;
                virtual function int tag(); return 30; endfunction
            endclass
            initial begin
                Abs  ah, ac;
                Leaf lf;
                lf = new;
                lf.ai = 11;
                lf.mi = 12;
                ah = lf;
                ac = new ah;
                $display("%0d %0d", ac.tag(), ac.ai);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "30 11\n");
}

TEST_CASE("class shallow copy: `new this` in a method, aggregate members, self-reference") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int x; int y; } pt_t;
            class Agg;
                pt_t p;
                int  arr[3];
                Agg  peer;
                function Agg dup(); return new this; endfunction
            endclass
            initial begin
                Agg a1, a2;
                a1 = new;
                a1.p.x = 1;
                a1.p.y = 2;
                a1.arr[0] = 5;
                a1.arr[2] = 6;
                a1.peer = a1;
                a2 = a1.dup();
                a2.p.x = 100;
                a2.arr[0] = 500;
                $display("%0d %0d %0d %0d", a1.p.x, a1.p.y, a1.arr[0], a1.arr[2]);
                $display("%0d %0d %0d %b", a2.p.x, a2.p.y, a2.arr[0], a2.peer == a1);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 5 6\n100 2 500 1\n");
}

TEST_CASE("class shallow copy: as a declaration initializer") {
    constexpr std::string_view src = R"(
        module m;
            class C; int i; endclass
            initial begin
                C c1;
                c1 = new;
                c1.i = 4;
                begin
                    automatic C c2 = new c1;
                    c2.i = 5;
                    $display("%0d %0d", c1.i, c2.i);
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4 5\n");
}

TEST_CASE("class shallow copy: a null source is a located fatal") {
    constexpr std::string_view plain = "module m;\n"
                                       "    class C;\n"
                                       "        int i;\n"
                                       "    endclass\n"
                                       "    initial begin\n"
                                       "        C p, q;\n"
                                       "        $display(\"before\");\n"
                                       "        q = new p;\n"
                                       "        $display(\"after\");\n"
                                       "    end\n"
                                       "endmodule\n";

    const test::DesignRun run = test::run_design_captured(plain);
    CHECK(run.out == "before\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("fatal: dereference of a null class handle") != std::string::npos);
    CHECK(run.err.find("8 |         q = new p;\n") != std::string::npos);

    constexpr std::string_view poly = "module m;\n"
                                      "    class V;\n"
                                      "        int i;\n"
                                      "        virtual function int f();\n"
                                      "            return i;\n"
                                      "        endfunction\n"
                                      "    endclass\n"
                                      "    initial begin\n"
                                      "        V p, q;\n"
                                      "        $display(\"before\");\n"
                                      "        q = new p;\n"
                                      "        $display(\"after\");\n"
                                      "    end\n"
                                      "endmodule\n";

    const test::DesignRun vrun = test::run_design_captured(poly);
    CHECK(vrun.out == "before\n");
    CHECK(test::design_exit_code(vrun) != 0);
    CHECK(vrun.err.find("fatal: dereference of a null class handle") != std::string::npos);
}
