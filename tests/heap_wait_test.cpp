#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// `wait` and `@` over expressions whose value lives in objects (LRM 9.4.2, 9.4.3):
// a method call, an expression of several properties, a built-in class's state.
//
// 9.4.2: "Changing the value of object data members, aggregate elements, or the size
// of a dynamically sized array referenced by a method or function shall cause the
// event expression to be reevaluated", and "A change of value in any operand of the
// expression without a change in the result of the expression shall not be detected
// as an event."
//
// No oracle: iverilog 13 stops at `wait (f())` with an internal error and does not
// take a method call or a class property in an event expression. Each expected line
// is the clause applied to the stores the design makes, and each waiter's event
// falls in a time step of its own, so no line depends on the order two processes
// woken together run in.

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

} // namespace

TEST_CASE("wait on a method is tested again when a property the method reads changes") {
    constexpr std::string_view src = R"(
        module m;
          class C;
            int v;
            function int get(); return v; endfunction
          endclass
          C c = new;
          initial begin
            fork
              begin wait (c.get() == 3); $display("wait %0t", $time); end
            join_none
            #2 c.v = 1;
            #2 c.v = 3;
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "wait 4\ndone 5\n");
}

// The method reads another object's properties, through a second method of its own.
// The waiter names neither object nor property.
TEST_CASE("wait on a method follows the methods it calls and the objects they reach") {
    constexpr std::string_view src = R"(
        module m;
          class Leaf; bit done; int n; endclass
          class Node;
            Leaf leaf;
            int  limit;
            function new(); leaf = new; limit = 2; endfunction
            function bit over(); return leaf.n > limit; endfunction
            function bit ready(); return leaf.done && over(); endfunction
          endclass
          Node node = new, other = new;
          initial begin
            fork
              begin wait (node.ready()); $display("ready %0t", $time); end
            join_none
            #1 other.leaf.done = 1;
            other.leaf.n = 9;
            #1 node.leaf.done = 1;
            #1 node.leaf.n = 3;
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "ready 3\ndone 4\n");
}

// A virtual call runs the method of the object's class, and a call through an
// interface class handle the implementing class's (LRM 8.20, 8.26): what the wait
// depends on is what that method reads, not what the method named at the call does.
TEST_CASE("wait on a virtual method depends on what the overriding method reads") {
    constexpr std::string_view src = R"(
        module m;
          interface class Probe;
            pure virtual function int level();
          endclass
          class Base implements Probe;
            int a;
            virtual function int level(); return a; endfunction
          endclass
          class Deriv extends Base;
            int b;
            virtual function int level(); return b; endfunction
          endclass
          Deriv d = new;
          Base  base;
          Probe probe;
          initial begin
            base  = d;
            probe = d;
            fork
              begin wait (base.level() == 5); $display("base %0t", $time); end
              begin wait (probe.level() == 7); $display("probe %0t", $time); end
            join_none
            #1 d.a = 5;
            #1 d.b = 5;
            #1 d.b = 7;
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "base 2\nprobe 3\ndone 4\n");
}

// A mailbox's message count and a semaphore's key count are state too: num() and
// try_get() read it (LRM 15.3, 15.4), at the top of the condition or inside a
// method, of the built-in class or of a class derived from it.
TEST_CASE("wait on a mailbox's or a semaphore's function wakes when a put changes it") {
    constexpr std::string_view src = R"(
        module m;
          class Chan;
            mailbox #(int) box = new;
            function bit has(); return box.num() != 0; endfunction
          endclass
          class Pair extends mailbox #(int);
            function bit two(); return num() >= 2; endfunction
          endclass
          mailbox #(int) mb = new;
          semaphore      s  = new(0);
          Chan           ch = new;
          Pair           pr = new;
          initial begin
            fork
              begin wait (mb.num() > 0); $display("num %0t", $time); end
              begin wait (ch.has()); $display("has %0t", $time); end
              begin wait (s.try_get(1)); $display("key %0t", $time); end
              begin wait (pr.two()); $display("two %0t", $time); end
            join_none
            #1 mb.put(1);
            #1 ch.box.put(2);
            #1 s.put(1);
            #1 pr.put(3);
            #1 pr.put(4);
            #1 $display("done %0t %0d", $time, s.try_get(1));
          end
        endmodule
    )";
    CHECK(run_capture(src) == "num 1\nhas 2\nkey 3\ntwo 5\ndone 6 0\n");
}

// A function reads a module variable and a method a static property: both are
// signals, and a change of either re-evaluates the expression that calls it. `@`
// over a plain function subscribes to a net a continuous process drives from the
// expression, so its waiter runs after the `wait`'s in the same time step.
TEST_CASE("wait and @ on a function depend on the variables the function reads") {
    constexpr std::string_view src = R"(
        module m;
          int g;
          class C;
            static int count;
            function int total(); return g + count; endfunction
          endclass
          function automatic int plain(); return g; endfunction
          C c = new;
          initial begin
            fork
              begin wait (c.total() == 3); $display("total %0t", $time); end
              begin wait (plain() == 2); $display("plain %0t", $time); end
              begin @(plain()); $display("event %0t", $time); end
            join_none
            #1 g = 2;
            #2 C::count = 1;
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "plain 1\nevent 1\ntotal 3\ndone 4\n");
}

// `@` detects a change in the value of its expression and nothing else: a store of
// the value already there, and a change of an operand that leaves the result where
// it was, are not events. The handles are assigned after the processes exist.
TEST_CASE("@ over a method call or an expression of properties detects a change of its value") {
    constexpr std::string_view src = R"(
        module m;
          class C;
            int v;
            bit f;
            int q[$];
            function int get(); return v; endfunction
            function bit flag(); return f; endfunction
          endclass
          C c, d, e;
          initial begin
            c = new; d = new; e = new;
            fork
              begin @(c.get()); $display("get %0t", $time); end
              begin @(d.v + e.v); $display("sum %0t", $time); end
              begin @(c.v == 3); $display("eq %0t", $time); end
              begin @(posedge c.flag()); $display("pos %0t", $time); end
              begin @(negedge c.flag()); $display("neg %0t", $time); end
              begin @(c.q.size()); $display("size %0t", $time); end
            join_none
            #1 c.v = 0;
            d.v = 0;
            #1 c.v = 1;
            #1 e.v = 5;
            #1 c.v = 3;
            #1 c.f = 1;
            #1 c.f = 0;
            #1 c.q.push_back(1);
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "get 2\nsum 3\neq 4\npos 5\nneg 6\nsize 7\ndone 8\n");
}

// Table 9-2 on the least significant bit of a 4-state result: posedge is 0 to
// anything else and anything but 1 to 1, negedge the mirror, `edge` either, and a
// move between x and z neither. Each waiter logs its own times, so the order the
// four run in at one time does not show.
TEST_CASE("@ with an edge over a method call follows Table 9-2") {
    constexpr std::string_view src = R"(
        module m;
          class C;
            logic l;
            function logic lv(); return l; endfunction
          endclass
          C      c = new;
          string pos, neg, either, any;
          initial begin
            fork
              forever begin @(posedge c.lv()); pos = {pos, $sformatf(" %0t", $time)}; end
              forever begin @(negedge c.lv()); neg = {neg, $sformatf(" %0t", $time)}; end
              forever begin @(edge c.lv()); either = {either, $sformatf(" %0t", $time)}; end
              forever begin @(c.lv()); any = {any, $sformatf(" %0t", $time)}; end
            join_none
            #1 c.l = 1'bz;
            #1 c.l = 1'b1;
            #1 c.l = 1'bx;
            #1 c.l = 1'b0;
            #1 c.l = 1'bz;
            #1 c.l = 1'bz;
            #1 $display("pos%s", pos);
            $display("neg%s", neg);
            $display("edge%s", either);
            $display("any%s", any);
            $finish;
          end
        endmodule
    )";
    CHECK(run_capture(src).starts_with("pos 2 5\nneg 3 4\nedge 2 3 4 5\nany 1 2 3 4 5\n"));
}

// `iff` (LRM 9.4.2.3) holds an event back unless its condition is true when the
// event occurs; a change of the condition alone is not an event. In a list, any
// member's event is the list's.
TEST_CASE("@ over method calls takes an iff guard and an event list") {
    constexpr std::string_view src = R"(
        module m;
          class C;
            int v;
            function int get(); return v; endfunction
          endclass
          C   c = new, d = new;
          bit en;
          initial begin
            fork
              begin @(c.get() iff en); $display("iff %0t", $time); end
              begin @(c.get() or d.get()); $display("first %0t", $time); end
              begin #2 @(c.get() or d.get()); $display("second %0t", $time); end
            join_none
            #1 c.v = 1;
            #2 en = 1;
            #1 d.v = 4;
            #1 c.v = 2;
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "first 1\nsecond 4\niff 5\ndone 6\n");
}

// A sensitivity list of an always procedure, and several threads waiting at one
// statement, each on the object its own captured variable names.
TEST_CASE("@ over properties as a sensitivity list, and in threads forked at one statement") {
    constexpr std::string_view src = R"(
        module m;
          class C; int v; endclass
          C   k = new;
          C   objs[3];
          int n;
          always @(k.v > 1) n++;
          initial begin
            foreach (objs[i]) objs[i] = new;
            for (int i = 0; i < 3; i++) begin
              fork
                automatic int j = i;
                begin @(objs[j].v == 1); $display("obj %0d at %0t", j, $time); end
              join_none
            end
            #1 objs[2].v = 1;
            k.v = 2;
            #1 objs[0].v = 1;
            k.v = 3;
            #1 objs[1].v = 1;
            k.v = 0;
            #1 $display("n=%0d", n);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "obj 2 at 1\nobj 0 at 2\nobj 1 at 3\nn=2\n");
}

// A string result compares by its characters, a real by its value.
TEST_CASE("@ over a method that returns a string or a real") {
    constexpr std::string_view src = R"(
        module m;
          class C;
            string s;
            real   r;
            function string label(); return {s, "!"}; endfunction
            function real   half(); return r / 2.0; endfunction
          endclass
          C c = new;
          initial begin
            fork
              begin @(c.label()); $display("label %0t", $time); end
              begin @(c.half()); $display("half %0t", $time); end
            join_none
            #1 c.s = "";
            c.r = 0.0;
            #1 c.s = "a";
            #1 c.r = 3.0;
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "label 2\nhalf 3\ndone 4\n");
}

// A fork-captured automatic is heap state too (a capture box), so `@` on one is
// sampled like any other: a change of its value, or Table 9-2 on its low bit. Each
// execution of the fork has its own variables (LRM 9.3.2), in a process body and
// in a task alike.
TEST_CASE("@ on a fork-captured automatic") {
    constexpr std::string_view src = R"(
        module m;
          task automatic spawn();
            for (int i = 0; i < 2; i++)
              fork
                automatic int id = i;
                automatic int k = 0;
                begin @(k); $display("task k=%0d at %0t", k, $time); end
                begin #(id + 5) k = id + 7; end
              join_none
          endtask
          initial begin
            for (int i = 0; i < 2; i++)
              fork
                automatic int id = i;
                automatic int k = 0;
                automatic bit b = 0;
                begin @(k); $display("k=%0d at %0t", k, $time); end
                begin @(posedge b); $display("b%0d rose at %0t", id, $time); end
                begin #(id + 1) k = id + 5; #2 b = 1; end
              join_none
            spawn();
            #8 $display("done");
          end
        endmodule
    )";
    CHECK(run_capture(src) == "k=5 at 1\nk=6 at 2\nb0 rose at 3\nb1 rose at 4\n"
                              "task k=7 at 5\ntask k=8 at 6\ndone\n");
}

// A list whose terms all read heap state is one sampled wait, whatever kinds they are: a
// method call, a property by itself (woken here by its any-change notifier and compared
// with its sample, as the others are) and a member of an aggregate property. Each term is
// its own event (LRM 9.4.2.1), a write that leaves a term's value alone is none, and a
// store to a sibling member (`s.g`) wakes the loop and is none either. No oracle parses
// classes; the expectations are the clauses'.
TEST_CASE("@ over a list of a method call, a property and a member of an aggregate property") {
    constexpr std::string_view src = R"(
        module m;
          typedef struct { int f; int g; } s_t;
          class C;
            int v;
            int x;
            s_t s;
            function int get(); return v; endfunction
          endclass
          C      c = new, o = new;
          string log;
          initial begin
            fork
              forever begin @(c.get() or o.x or o.s.f) log = {log, $sformatf("%0t ", $time)}; end
            join_none
            #1 c.v = 1;
            #1 o.x = 2;
            #1 o.s.g = 3;
            #1 o.s.f = 4;
            #1 c.v = 1;
            #1 o.x = 2;
            #1 c.v = 5;
            #1 $display("%s", log);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 4 7 \n");
}

// Threads forked at one statement wait on one list, each on the objects its own captured
// variable names and each against its own samples: a store that changes one thread's term
// leaves the others' alone.
TEST_CASE("@ over a list in threads forked at one statement") {
    constexpr std::string_view src = R"(
        module m;
          class C;
            int v;
            int w;
          endclass
          C objs[3];
          initial begin
            foreach (objs[i]) objs[i] = new;
            for (int i = 0; i < 3; i++) begin
              fork
                automatic int j = i;
                begin @(objs[j].v == 1 or objs[j].w); $display("obj %0d at %0t", j, $time); end
              join_none
            end
            #1 objs[2].v = 1;
            #1 objs[0].w = 5;
            #1 objs[1].v = 1;
            #1 $display("done");
          end
        endmodule
    )";
    CHECK(run_capture(src) == "obj 2 at 1\nobj 0 at 2\nobj 1 at 3\ndone\n");
}

// Reading a string place yields the place, not a copy, so the sample of a string term is
// copied: a store of the same text is no event, a store of other text is, and the compare
// is against what the string was, not against itself.
TEST_CASE("@ over a list with a string property compares with a copy of the string") {
    constexpr std::string_view src = R"(
        module m;
          class C;
            string t;
            int    v;
            function int get(); return v; endfunction
          endclass
          C      c = new;
          string log;
          initial begin
            fork
              forever begin @(c.get() or c.t) log = {log, $sformatf("%0t ", $time)}; end
            join_none
            #1 c.t = "a";
            #1 c.t = "a";
            #1 c.t = "b";
            #1 c.v = 1;
            #1 $display("%s", log);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "1 3 4 \n");
}

// A fork-captured automatic is heap state too, and a property by itself beside it is
// compared with a sample like the automatic is: either one wakes the list.
TEST_CASE("@ over a list of a fork-captured automatic and a property") {
    constexpr std::string_view src = R"(
        module m;
          class C; int x; endclass
          C o = new;
          initial begin
            fork
              automatic int k = 0;
              begin @(k or o.x); $display("first k=%0d x=%0d at %0t", k, o.x, $time); end
              begin #1 k = 5; end
            join_none
            #2;
            fork
              automatic int k = 0;
              begin @(k or o.x); $display("second k=%0d x=%0d at %0t", k, o.x, $time); end
              begin #1 o.x = 3; end
            join_none
            #2 $display("done");
          end
        endmodule
    )";
    CHECK(run_capture(src) == "first k=5 x=0 at 1\nsecond k=0 x=3 at 3\ndone\n");
}

// A method that reads an aggregate property (an unpacked struct, union or fixed array) is
// waited on through the property's design-wide notifier, as for a scalar one: a store to
// any member of it wakes `wait` and `@`, and `@` compares the method's value. No oracle
// parses classes or unpacked structs; the expectations are the clauses'.
TEST_CASE("wait and @ on a method that reads an aggregate property") {
    constexpr std::string_view src = R"(
        module m;
          typedef struct { int f; int g; } s_t;
          class C;
            s_t s;
            int a[2];
            function int get_f(); return s.f; endfunction
            function int get_a(); return a[1]; endfunction
          endclass
          C c = new;
          initial begin
            fork
              begin wait (c.get_f() == 1); $display("wait f %0t", $time); end
              begin @(c.get_f()); $display("at f %0t", $time); end
              begin @(c.get_a()); $display("at a %0t", $time); end
            join_none
            #1 c.s.g = 7;
            #1 c.s.f = 1;
            #1 c.a[0] = 3;
            #1 c.a[1] = 4;
            #1 $display("done %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "wait f 2\nat f 2\nat a 4\ndone 5\n");
}
