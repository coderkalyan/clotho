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

// JIT-run a module and capture stdout (mirrors string_test.cpp). Expectations
// below are cross-checked against iverilog where its class support suffices;
// iverilog 13 rejects handle comparison, nested member paths, and
// self-references (see docs/design/classes.md §10).
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

// Reference semantics (LRM 8.6): `h2 = h1` copies the handle, not the object, so a
// write through one handle is visible through the other. The single most important
// test distinguishing a class handle from the deep-copy handle types. iverilog
// cannot compare handles in the same test.
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

// null literal, identity `==` / `!=` (LRM 8.4).
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

// Self-referential property (a handle to the enclosing class) + nested member path
// (LRM 8.4). Reference semantics make the cycle trivial (a property is a pointer).
// iverilog rejects both the self-reference and `a.next.val`.
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

// Non-static property default initializers run at construction, in declaration
// order, before the constructor body (LRM 8.8) — for scalars and strings, and with
// or without an explicit constructor.
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

// A property declaration initializer may CALL a method of the same class: LRM 8.7
// runs those initializers as part of construction ("each property defined in the
// class shall be initialized to its explicit default value"), so `this` is in scope
// and a bare `f()` is an ordinary method call on the object being constructed.
// `y = g()` also pins the ordering: g reads `x`, which the preceding initializer
// already stored, so 3 + 4 rather than 0 + 4.
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

// A 4-state property powers on X (LRM 8.8 default init); a 2-state property powers
// on 0.
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

// -- Single inheritance (LRM 8.13), Phase 2 --
// Prefix layout: base properties are laid first, so an inherited property keeps
// its base-relative field index and an upcast is the same pointer. Non-virtual
// methods bind statically. super.new / super.method run against the same object.
// iverilog's class support cannot resolve inherited methods from a derived
// method body.

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

// `extends Base(args)` clause form (LRM 8.15): the base constructor is invoked
// from the extends clause (class scope, so a constant / param, not a ctor
// formal), both when the derived class has its own constructor and when it does
// not.
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

// -- Static members (LRM 8.9), Phase 2 --
// A static property is one slot shared across all instances (and inherited by
// derived classes), initialized once at time zero, reachable as `obj.p`, `C::p`,
// or a bare `p` in a static method.
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

// A static property's initializer runs once in PreInit (LRM 8.9: "fileID shall be
// created and initialized once") whatever its type: an unpacked struct and an
// unpacked array take the same element-wise aggregate store a module-level
// declaration initializer does, a queue / dynamic array build their handles, and a
// string assigns whole. Reading the elements back proves each handle was allocated
// to the right length, not bit-blasted.
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

// -- Virtual methods / dynamic dispatch (LRM 8.20), Phase 3 --
// A virtual method called through a base handle dispatches on the object's
// concrete type, not the handle's static type. An override reuses its base
// method's vtable slot; a non-overriding derived class inherits the base body.
// A non-virtual method (describe) that internally calls a virtual one dispatches
// dynamically too. `super.method()` binds STATICALLY to the base version.
// iverilog cannot resolve these.
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

// A virtual method returning a non-scalar (string / unpacked struct / container)
// dispatches like any virtual call; the call-temp scanners (build_str_temps,
// build_dynarr_temps, agg-return) recognize CallVirtual.
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

// A virtual TASK (LRM 8.20) dispatches on the concrete type through a base handle
// over the task (coroutine) ABI — including a time-consuming task (#delay) and a
// non-virtual task that calls a virtual one internally.
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

// A pure virtual TASK (LRM 8.21 + 13.3) dispatches on the concrete type over the
// task (coroutine) ABI: the abstract-class slot's never-called stub carries the
// `is_task` flag, so the TaskCallVirtual signature is the one every override was
// built with. Covers an argument, a time-consuming override, and a concrete base
// task that enables the pure one internally.
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

// -- Abstract classes (`virtual class`, LRM 8.21) --
// An abstract class cannot be instantiated but is a valid handle type and base;
// its concrete virtual methods dispatch dynamically on the derived object. A
// derived class that omits an override inherits the abstract base's body.
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

// A pure virtual method (LRM 8.21) — a MethodPrototypeSymbol with no body, whose
// concrete overrides slang does not link via getOverride() — dispatches on the
// object's concrete type through an abstract base handle, both with and without
// arguments, and via a non-virtual method that calls it internally. The
// name-keyed vtable slot and a prototype-built signature stub (never executed)
// make this work.
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

// A pure virtual carried through two abstract levels, overridden in the concrete
// leaf WITHOUT the `virtual` keyword (an implicit override slang does not link) —
// the name-keyed slot still matches it.
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

// A pure virtual returning a non-scalar: the never-called stub reuses
// build_function's return shape, so its signature is the one the override was built
// with. A string handle returns as a loaded value (and feeds a concrete base method
// that concatenates it); an unpacked struct returns through the trailing out-param
// the caller owns.
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

// A container result (queue / dynamic array / assoc) takes the third return shape:
// the stub Rets its frame handle lvalue, like build_function, so the backend
// deep-copies it out through the trailing out-param. Dispatched directly and from a
// concrete base method that consumes the pure result.
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

// An aggregate that owns heap (a string member) now returns by value like any
// other: the return ABI's copy is per-leaf (emit_agg_deep_copy) instead of a
// memcpy, so the caller's result temp owns its own strings. Through a virtual
// dispatch, so the pure stub's signature and the override's agree.
TEST_CASE("pure virtual: aggregate return with a string member") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { string s; int i; } rec;
            virtual class Base;
                pure virtual function rec f();
            endclass
            class Der extends Base;
                string tag;
                function new(string t); tag = t; endfunction
                virtual function rec f();
                    rec r;
                    r.s = {"<", tag, ">"};
                    r.i = tag.len();
                    return r;
                endfunction
            endclass
            Base b;
            rec  got;
            initial begin
                b   = Der::new("first");
                got = b.f();
                $display("[%s] %0d", got.s, got.i);
                b   = Der::new("0123456789012345678901234567890123456789");
                got = b.f();
                $display("[%s] %0d", got.s, got.i);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[<first>] 5\n[<0123456789012345678901234567890123456789>] 40\n");
}

// -- Parameterized classes (LRM 8.25) --
// slang monomorphizes each `C#(args)` into a distinct, fully-substituted
// ClassType, so each specialization lowers as an independent nominal class. Type
// and value parameters, param-sized members, and methods over the type parameter
// all fall out.
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

// -- Forward typedef classes (`typedef class C;`, LRM 6.18 / 8.27) --
// Class resolution itself needed no work: lower_class_type's intern-first
// cache keys on the ClassType symbol pointer, which slang resolves a forward
// declaration and its later full definition to identically, so mutual
// cross-class handle references fall out for free. lower_members did need a
// fix -- it loud-failed on the ForwardingTypedef symbol (the declaration
// itself) wherever it appears outside the top compilation-unit scope; see
// docs/TODO.md.

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

// The `std` package's built-in classes are synthesized by slang with signatures but
// no bodies -- the simulator supplies the semantics -- so the guard in
// lower_class_type is keyed by class name and each name is either implemented or
// refused. `process` (LRM 9.7), `semaphore` (LRM 15.3) and `mailbox #(T)` (LRM
// 15.4) are implemented (tests/process_test.cpp, tests/sync_test.cpp). What is left
// is `std::weak_reference` (LRM 8.30), which still has no runtime, and the TYPELESS
// mailbox (15.4.9), which is refused by its own message because it needs run-time
// type identity -- covered in tests/sync_test.cpp.
TEST_CASE("built-in std classes are refused in the frontend") {
    // A declaration is enough: the type lowers before any method is reached.
    CHECK(fails("module m; std::weak_reference #(int) w; initial w = null; endmodule")
            .find("not supported") != std::string::npos);
}

// -- shallow copy, `dst = new src` (LRM 8.12) ---------------------------------

// LRM 8.12's own worked example, values included: `b2.i = 10` changes only the
// copy, `b2.a.j = 50` changes the object BOTH handles point at ("objects, however,
// are not copied, only their handles"). iverilog 13 rejects the nested member
// path `b1.a.j`.
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

// LRM 8.12 step 1: the allocation "shall not call the object's constructor or
// execute any variable declaration initialization assignments". So the source's
// current value survives into the copy instead of being reset to the initializer,
// and the constructor runs once for the whole test.
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

// A property that is a value type owning a heap buffer copies by VALUE (LRM 8.12
// "all of the variables are copied"), so writing through the copy cannot reach the
// source's buffer. Covers all four: string, queue, dynamic array, associative
// array.
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

// The object allocated is of the SOURCE's class, not of the expression's static
// type: copying a Derived through a Base-typed handle must yield a Derived, which
// is observable through a virtual call on the copy. Clotho dispatches the copy
// itself through the source's vtable to that class's synthesized copy thunk
// (compute_vtable's kCopySlot), which is what makes the allocation the right size.
// iverilog 13 ASSERTS on this case (vvp_cobject.cc:83 `defn_ == that->defn_`).
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

// The static type may be an abstract class (LRM 8.21): nothing instantiates it, the
// dispatch still lands on the concrete class two levels down, and inherited
// properties come across.
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

// Copying `this` from inside a method, an unpacked struct and a fixed unpacked
// array property copying element-wise, and a self-referential handle property
// staying a handle.
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

// The declaration-initializer spelling of the same expression (the LRM's own
// `B b2 = new b1;`), which reaches lower_expr through lower_var_decl rather than
// through an assignment. Declared `automatic` deliberately: a block-scope
// declaration in a module defaults to STATIC lifetime, whose initializer runs
// before time 0 (LRM 6.21) -- so the un-annotated spelling copies a still-null
// handle, which is a separate question from this feature. With `automatic` the
// copy happens where it is written, which is what the line below pins.
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

// `new null_handle` has no object to copy. Both lowerings reach the same guard:
// the non-polymorphic one dereferences the source to read its properties, the
// polymorphic one dereferences it to load the vtable it dispatches through. Out of
// process because the fatal exits. (iverilog 13 dies here without a diagnostic
// naming the source line.)
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

// An `extern virtual` method (LRM 8.24) declares the prototype in the class and
// the body outside it, so the body is not a member subroutine of the class and a
// vtable built only from members leaves the slot empty. Every level here is
// covered: a root abstract class whose extern body IS the slot's implementation, a
// derived class overriding one extern with another extern while inheriting the
// other, and a leaf overriding an inherited extern with an inline body. Tasks as
// well as functions, since the two take different ABIs.
TEST_CASE("virtual dispatch reaches an out-of-block `extern virtual` body") {
    constexpr std::string_view src = R"(
        module m;
            virtual class Abs;
                extern virtual function int tag();
                extern virtual task show();
                pure virtual function int pv();
            endclass
            function int Abs::tag(); return 1; endfunction
            task Abs::show(); #1 $display("abs show"); endtask

            class Mid extends Abs;
                extern virtual function int tag();
                virtual function int pv(); return 7; endfunction
            endclass
            function int Mid::tag(); return 2; endfunction

            class Leaf extends Mid;
                virtual task show(); #1 $display("leaf show"); endtask
            endclass

            initial begin
                Abs  a;
                Mid  mid = new;
                Leaf lf  = new;
                a = mid;
                $display("%0d %0d", a.tag(), a.pv());
                a.show();
                a = lf;
                $display("%0d %0d", a.tag(), a.pv());
                a.show();
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 7\nabs show\n2 7\nleaf show\n");
}

// -- Runtime type identity and `$cast` (LRM 6.24.2 / 8.16) --

// The object body carries a class-descriptor pointer at field 0 in EVERY class,
// not only a polymorphic one. It has to be uniform across a hierarchy: prefix
// layout puts the base's properties first so an upcast is the same pointer at the
// same offsets, and a base laid out without the header while its derived class
// has one shifts every inherited property by a word. Whether some derived class
// introduces a virtual method is not a property of the base being laid out, so
// the header cannot be conditioned on it. Reading `a.x` here through the base
// handle used to return the low half of the vtable pointer.
TEST_CASE("a plain base of a polymorphic class keeps its property offsets") {
    constexpr std::string_view src = R"(
        module m;
            class A;
                int x;
                function new(); x = 7; endfunction
            endclass
            class B extends A;
                virtual function int f(); return 42; endfunction
            endclass
            initial begin
                B b = new;
                A a = b;
                $display("%0d %0d %0d", b.x, a.x, b.f());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "7 7 42\n");
}

// LRM 8.16 case 2, the only case that needs a run-time test: the destination
// descends from the source's static type, so the object's own class decides. A
// hit assigns and answers 1; a miss leaves the destination alone and answers 0.
TEST_CASE("$cast: down-cast succeeds on the object's own class") {
    constexpr std::string_view src = R"(
        module m;
            class A; int x; endclass
            class B extends A; int y; endclass
            class C extends A; int z; endclass
            initial begin
                A a; B b; C c; int r;
                b = new; b.x = 1; b.y = 2;
                a = b;
                r = $cast(b, a);
                $display("%0d %0d %0d", r, b.x, b.y);
                c = new; c.z = 9;
                r = $cast(c, a);
                $display("%0d %0d", r, c.z);
            end
        endmodule
    )";
    // The sibling cast fails and leaves `c` pointing at its own object.
    CHECK(run_capture(src) == "1 1 2\n0 9\n");
}

// The test is an ancestor test, not an equality test: a grandchild satisfies a
// cast to the middle class, and the exact class satisfies its own.
TEST_CASE("$cast: a down-cast accepts any descendant of the destination") {
    constexpr std::string_view src = R"(
        module m;
            class A; endclass
            class B extends A; int y; endclass
            class D extends B; int w; endclass
            initial begin
                A a; B b; D d; int r;
                d = new; d.w = 5; d.y = 3;
                a = d;
                r = $cast(b, a);
                $display("%0d %0d", r, b.y);
                r = $cast(d, a);
                $display("%0d %0d", r, d.w);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 3\n1 5\n");
}

// LRM 8.16 case 1 (the destination is the source's class or an ancestor of it)
// and case 3 (the literal `null`) cannot fail, so they lower to a plain
// assignment and a constant 1 with no run-time test at all.
TEST_CASE("$cast: an up-cast, a same-type cast and a literal null always succeed") {
    constexpr std::string_view src = R"(
        module m;
            class A; int x; endclass
            class B extends A; endclass
            initial begin
                A a; B b; B b2; int r;
                b = new; b.x = 4;
                r = $cast(a, b);
                $display("%0d %0d", r, a.x);
                r = $cast(b2, b);
                $display("%0d %0d", r, b2.x);
                r = $cast(b2, null);
                $display("%0d %0d", r, b2 == null);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 4\n1 4\n1 1\n");
}

// A source that is null at run time, in a cast-compatible pair, succeeds and
// assigns null. LRM 8.16 is ambiguous here -- case 2 wants "an object that is
// assignment compatible", which null is not, but the closing sentence forces
// failure on a null source only "when the source and destination types are not
// cast compatible", a qualification that would be redundant if every run-time
// null failed. iverilog does not implement $cast at all so it is no oracle;
// Verilator answers 1 and assigns null, which is what this pins.
TEST_CASE("$cast: a run-time null source assigns null and succeeds") {
    constexpr std::string_view src = R"(
        module m;
            class A; endclass
            class B extends A; endclass
            initial begin
                A a; B b; int r;
                b = new;
                a = null;
                r = $cast(b, a);
                $display("%0d %0d", r, b == null);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}

// Two classes in unrelated hierarchies: no object can satisfy the cast, so it is
// a compile-time constant 0 that leaves the destination untouched. Single
// inheritance is what makes that conclusive.
TEST_CASE("$cast: unrelated class types always fail") {
    constexpr std::string_view src = R"(
        module m;
            class A; endclass
            class Z; int q; endclass
            initial begin
                A a; Z z; int r;
                a = new;
                z = new; z.q = 6;
                r = $cast(z, a);
                $display("%0d %0d", r, z.q);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 6\n");
}

// The cast yields a handle to the same object, so a virtual call through it
// still dispatches on the concrete class, and the properties are the ones the
// object already had.
TEST_CASE("$cast: the cast handle shares the object") {
    constexpr std::string_view src = R"(
        module m;
            class A;
                int x;
                virtual function int who(); return 1; endfunction
            endclass
            class B extends A;
                virtual function int who(); return 2; endfunction
            endclass
            initial begin
                A a; B b;
                b = new; b.x = 8;
                a = b;
                b = null;
                if (!$cast(b, a)) $display("cast failed");
                b.x = 9;
                $display("%0d %0d %0d", b.who(), a.x, a == b);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 9 1\n");
}

// A hierarchy with no virtual method at all still carries type identity, so the
// down-cast works there too -- and the shallow copy of such a hierarchy no longer
// slices, which `$cast` is what makes observable (LRM 8.12 copies the source's
// own class, not the static type of the expression).
TEST_CASE("$cast: a hierarchy with no virtual method") {
    constexpr std::string_view src = R"(
        module m;
            class P; int x; endclass
            class Q extends P; int y; endclass
            initial begin
                P p; Q q; int r;
                q = new; q.x = 3; q.y = 4;
                p = q;
                r = $cast(q, p);
                $display("%0d %0d %0d", r, q.x, q.y);
                p = new q;
                r = $cast(q, p);
                $display("%0d %0d %0d", r, q.x, q.y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 3 4\n1 3 4\n");
}

// Two specializations of one generic class are distinct types (LRM 8.25), and
// each gets its own id: neither casts into the other.
TEST_CASE("$cast: parameterized specializations are distinct types") {
    constexpr std::string_view src = R"(
        module m;
            class Base; endclass
            class C #(int N = 0) extends Base; int n = N; endclass
            initial begin
                Base b; C#(1) c1; C#(2) c2; int r;
                c1 = new;
                b  = c1;
                r  = $cast(c2, b);
                $display("%0d", r);
                r = $cast(c1, b);
                $display("%0d %0d", r, c1.n);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0\n1 1\n");
}

// LRM 6.24.2: called as a task, an invalid assignment is a run-time error and
// the destination is left unchanged. Out of process because the report ends the
// run. The function form of the same cast reports nothing and answers 0.
TEST_CASE("$cast: the task form of a failed cast is a run-time error") {
    constexpr std::string_view src = "module m;\n"
                                     "    class A; endclass\n"
                                     "    class B extends A; endclass\n"
                                     "    initial begin\n"
                                     "        A a; B b;\n"
                                     "        a = new;\n"
                                     "        $display(\"before\");\n"
                                     "        $cast(b, a);\n"
                                     "        $display(\"after\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out.find("before\n") != std::string::npos);
    CHECK(run.out.find("after") == std::string::npos);
    CHECK(run.err.find("$cast: the source is not assignment compatible with the destination") != std::string::npos);
    CHECK(run.err.find("source:8") != std::string::npos);
}

// A task-form cast that succeeds reports nothing and runs on.
TEST_CASE("$cast: the task form of a successful cast is silent") {
    constexpr std::string_view src = R"(
        module m;
            class A; endclass
            class B extends A; int y; endclass
            initial begin
                A a; B b;
                b = new; b.y = 5;
                a = b;
                b = null;
                $cast(b, a);
                $display("%0d", b.y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "5\n");
}

// A class id is design-wide, keyed on the slang ClassType, not on the lowered
// Ref. It has to be: a package-scope class is lowered once per Lowerer -- the
// shared `globals` one that owns the package function's body, and each top that
// names the class -- so one class is several nominal Refs with several
// descriptors. Comparing ids rather than descriptor addresses is what lets an
// object the package function allocated cast correctly inside either top.
TEST_CASE("$cast: a package class crosses tops and Lowerers") {
    constexpr std::string_view src = R"(
        package p;
            class A; int x; endclass
            class B extends A; int y; endclass
            function automatic A make();
                B b = new;
                b.x = 1;
                b.y = 2;
                return b;
            endfunction
        endpackage
        module t1;
            import p::*;
            initial begin
                A a; B b;
                a = make();
                $display("t1 %0d %0d", $cast(b, a), b.y);
            end
        endmodule
        module t2;
            import p::*;
            initial begin
                A a; B b;
                #1;
                a = make();
                $display("t2 %0d %0d", $cast(b, a), b.x);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "t1 1 2\nt2 1 1\n");
}

// The ancestor display a cast reads is one fixed width for every class, so a
// chain deeper than it has nowhere to record its ancestors. Refused loudly
// rather than silently answering the wrong thing.
TEST_CASE("an inheritance chain deeper than the display is refused") {
    std::string src = "module m;\n    class C0; endclass\n";
    for (int k = 1; k <= 16; ++k) {
        src += "    class C" + std::to_string(k) + " extends C" + std::to_string(k - 1) + "; endclass\n";
    }
    src += "    initial begin C16 d; d = new; end\nendmodule\n";

    CHECK(fails(src).find("deeper than 16 levels") != std::string::npos);
}

// Lowering a property's TYPE recurses without bound: a class-handle property lowers
// its pointee class, whose vtable construction lowers that class's method bodies,
// and those bodies can read a property of the class still being laid out below on
// the stack. Here B's first property is a handle to A, A's virtual method reads B's
// SECOND property, and B is lowered first — so `flag` is read while B's own layout
// is mid-flight. Every index has to be assigned before the first property type is
// lowered; assigning them one at a time left `flag` unindexed.
TEST_CASE("class layout: a property type that reaches back into the same class") {
    constexpr std::string_view src = R"(
        module m;
            typedef class A;
            class B;
                A        ha;
                local bit flag;
                function int f(); return flag ? 1 : 2; endfunction
            endclass
            class A;
                virtual function int g();
                    B b = new;
                    return b.f();
                endfunction
            endclass
            initial begin
                static B b = new;
                static A a = new;
                $display("%0d %0d", b.f(), a.g());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 2\n");
}

// The same cycle across inheritance, which is what decides field INDICES rather
// than just their presence. Base holds a handle to its own derived class, so
// lowering Base's properties lays out D, whose prefix is Base's own property table
// — still empty at that moment. Copying it there gave D's `d0` the index of
// Base's `b0`, silently aliasing an inherited property with an own one. The prefix
// is rebuilt from the base chain's property symbols instead, which needs no other
// class's layout to be finished. `dd.sum()` reads the inherited pair through the
// derived body; the values separate a correct layout from an aliased one.
TEST_CASE("class layout: a base property holding a handle to its own derived class") {
    constexpr std::string_view src = R"(
        module m;
            typedef class D;
            class Base;
                int b0;
                D   hd;
                int b1;
                function int sum(); return b0 + b1; endfunction
            endclass
            class D extends Base;
                int d0 = 7;
                virtual function int all(); return sum() + d0; endfunction
            endclass
            initial begin
                static Base bb = new;
                static D    dd = new;
                bb.b0 = 1;  bb.b1 = 2;
                dd.b0 = 10; dd.b1 = 20;
                $display("%0d %0d %0d %0d", bb.sum(), dd.sum(), dd.all(), dd.d0);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 30 37 7\n");
}

// A function body is lowered on demand, at its first call, and such a body can reach
// a virtual call whose DECLARING class has not been numbered yet. Here Base's first
// property is a handle to Helper, so lowering it runs Helper's compute_vtable, which
// builds `poke` -- and `poke` virtual-calls `tag` on Base, which is still mid-layout
// below on the stack and has no slot map at all. The lookup was an unguarded `.at()`
// and threw std::out_of_range with no source location; a bare `uvm_pkg` elaboration
// aborted this way on `uvm_object::get_full_name`. The slot numbering is now done on
// demand, like function_id lowers a callee on demand.
//
// `d.tag()` and `hp.poke(d)` both have to reach the derived override: the numbering
// this produces has to agree with the one Base's own frame would have produced, not
// merely exist.
TEST_CASE("class vtable: a virtual call reached before its class is numbered") {
    constexpr std::string_view src = R"(
        module m;
            typedef class Helper;
            class Base;
                Helper h;
                virtual function int tag(); return 1; endfunction
            endclass
            class Helper;
                virtual function int poke(Base b); return b.tag(); endfunction
            endclass
            class Derived extends Base;
                virtual function int tag(); return 2; endfunction
            endclass
            initial begin
                static Base    b  = new;
                static Derived d  = new;
                static Helper  hp = new;
                $display("%0d %0d %0d", b.tag(), d.tag(), hp.poke(d));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 2\n");
}
