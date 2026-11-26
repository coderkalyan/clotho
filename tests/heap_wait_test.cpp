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

std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostic_text();
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

// What no process can sample is refused, where it used to be handed to a continuous
// process that read the handle once, before anything had assigned it, and never ran
// again. In a list with a signal or a named event the woken process cannot tell
// which member woke it; the control of a nonblocking assignment or trigger is a
// record, and a clocking event is a clock.
TEST_CASE("an event control over heap state that nothing can sample is refused") {
    constexpr std::string_view needle  = "supported only as a procedural `@`";
    constexpr std::string_view head    = R"(
        module m;
          class C;
            bit clk; int v;
            function int get(); return v; endfunction
          endclass
          C c = new;
          bit sysclk, a, x;
          event e;
    )";
    const auto                 refused = [&](std::string_view item) {
        const std::string err = lower_error(std::string(head) + std::string(item) + "\nendmodule\n");
        CHECK_MESSAGE(err.find(needle) != std::string::npos, err);
    };
    SUBCASE("a list with a signal edge") {
        refused("initial begin @(c.get() or posedge sysclk); end");
        refused("always @(c.v == 3 or posedge sysclk) x = 1;");
    }
    SUBCASE("a list with a named event") {
        refused("initial begin @(c.get() or e); end");
    }
    SUBCASE("the control of a nonblocking assignment") {
        refused("initial x <= @(c.get()) 1'b1;");
    }
    SUBCASE("the control of a nonblocking trigger") {
        refused("initial ->> @(c.v == 3) e;");
    }
    SUBCASE("a clocking block's event") {
        refused("clocking cb @(posedge c.get()); input a; endclocking");
    }
    SUBCASE("a covergroup's sampling event") {
        refused("covergroup cg @(posedge c.get()); coverpoint a; endgroup\ncg g = new;");
    }
}

// -- Waiting on a process's status (LRM 9.7 over 9.4.2, 9.4.3) --------------------
//
// `status()` reads a thread's state, which changes when the thread blocks, ends, is
// killed, or is suspended or resumed. No store of the design makes any of those
// changes, so each wait below is woken by the change itself or not at all.

TEST_CASE("wait on process::status() wakes when the thread finishes or is killed") {
    constexpr std::string_view src = R"(
        module m;
          process a, b;
          initial begin
            fork
              begin a = process::self(); #5; end
              begin b = process::self(); #9; end
            join_none
            #1;
            fork
              begin wait (a.status() == process::FINISHED); $display("a finished at %0t", $time); end
              begin wait (b.status() == process::KILLED); $display("b killed at %0t", $time); end
              begin wait (b.status() != process::WAITING); $display("b not waiting at %0t", $time); end
            join_none
            #6 b.kill();
            #1 $display("end");
          end
        endmodule
    )";
    // `a` ends at 5. `b` is blocked until the kill at 7 makes it KILLED, which is
    // also the first time it is not WAITING. Which of the two waiters on `b` runs
    // first is not fixed.
    const std::string out = run_capture(src);
    CHECK_MESSAGE(out.starts_with("a finished at 5\n"), out);
    CHECK_MESSAGE(out.find("b killed at 7\n") != std::string::npos, out);
    CHECK_MESSAGE(out.find("b not waiting at 7\n") != std::string::npos, out);
    CHECK_MESSAGE(out.ends_with("end\n"), out);
}

// suspend() and resume() change the state from another thread. A thread resumed
// while it is still blocked goes back to WAITING: resume "shall resensitize the
// process to the event expression or to wait for the wait condition to become true
// or for the delay to expire". status() used to read RUNNING from then on.
TEST_CASE("wait on process::status() wakes on suspend and on resume") {
    constexpr std::string_view src = R"(
        module m;
          process p;
          initial begin
            fork
              begin p = process::self(); #10; end
            join_none
            #1;
            fork
              begin wait (p.status() == process::SUSPENDED); $display("suspended at %0t", $time); end
            join_none
            #1 p.suspend();
            #1;
            fork
              begin wait (p.status() == process::WAITING); $display("waiting at %0t", $time); end
            join_none
            #1 p.resume();
            #1 $display("status %s", p.status().name());
            wait (p.status() == process::FINISHED);
            $display("finished at %0t", $time);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "suspended at 2\nwaiting at 4\nstatus WAITING\nfinished at 10\n");
}

// The state a method reads is what a wait on the method depends on, and `@` over it
// is a change of the value it returns.
TEST_CASE("wait and @ reach process::status() through a method") {
    constexpr std::string_view src = R"(
        module m;
          class Job;
            process p;
            function bit done();
              return p != null && p.status() == process::FINISHED;
            endfunction
            function int state();
              return p.status();
            endfunction
          endclass
          Job j = new;
          initial begin
            fork
              begin j.p = process::self(); #3; end
            join_none
            #1;
            fork
              begin @(j.state()); $display("changed at %0t", $time); end
            join_none
            wait (j.done());
            $display("done at %0t", $time);
          end
        endmodule
    )";
    const std::string          out = run_capture(src);
    CHECK_MESSAGE(out.find("changed at 3\n") != std::string::npos, out);
    CHECK_MESSAGE(out.find("done at 3\n") != std::string::npos, out);
}

// Two threads that wait on each other's status each block, which is a change of
// state the other waits on: the second to block wakes the first, which blocks
// again. Only a change another thread can observe counts (a thread that was
// blocked, ran and blocks again was never seen running), so the pair settles
// instead of waking each other for ever.
TEST_CASE("threads waiting on each other's status do not wake each other for ever") {
    constexpr std::string_view src = R"(
        module m;
          process a, b;
          bit go;
          initial begin
            fork
              begin
                a = process::self();
                wait ((b != null && b.status() == process::FINISHED) || go);
                $display("a at %0t", $time);
              end
              begin
                b = process::self();
                wait ((a != null && a.status() == process::FINISHED) || go);
                $display("b at %0t", $time);
              end
            join_none
            #5 go = 1;
            #1 $display("end");
          end
        endmodule
    )";
    const std::string          out = run_capture(src);
    CHECK_MESSAGE(out.find("a at 5\n") != std::string::npos, out);
    CHECK_MESSAGE(out.find("b at 5\n") != std::string::npos, out);
    CHECK_MESSAGE(out.ends_with("end\n"), out);
}

// An `always` procedure never completes of itself, so a kill is the only thing that
// ends it. The killed thread unwinds in the time step of the kill, and that is
// where its KILLED is announced.
TEST_CASE("wait on process::status() wakes when an always procedure is killed") {
    constexpr std::string_view src = R"(
        module m;
          process p;
          always begin
            if (p == null) p = process::self();
            #1;
          end
          initial begin
            #2;
            fork
              begin wait (p.status() == process::KILLED); $display("killed at %0t", $time); end
            join_none
            #1 p.kill();
            #2 $display("end");
          end
        endmodule
    )";
    CHECK(run_capture(src) == "killed at 3\nend\n");
}
