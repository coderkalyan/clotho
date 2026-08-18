#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors task_test.cpp). Named-event
// behavior for `->` / `@(e)` / `always @(e)` was cross-checked against Icarus
// Verilog; `.triggered` is not implemented by iverilog ("Event name e can't
// have member names"), so those cases are pinned to the LRM 15.5.3 semantics
// (triggered true only within the triggering time step).
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

// `-> e` wakes an intra-body `@(e)` waiter in the same instant (LRM 15.5.2). A
// bare `@(e)` catches only triggers that occur while it is suspended.
TEST_CASE("blocking trigger wakes @(e) waiter") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          initial begin
            #5  -> e;
            #5  -> e;
            #10 $finish;
          end
          initial forever begin
            @(e);
            $display("woke t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out.find("woke t=5") != std::string::npos);
    CHECK(out.find("woke t=10") != std::string::npos);
}

// A trigger re-runs an `always @(e)` process (event in the sensitivity list).
TEST_CASE("always @(e) re-runs on every trigger") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          int hits = 0;
          initial begin
            #5 -> e;
            #5 -> e;
            #5 -> e;
            #5 $finish;
          end
          always @(e) begin
            hits++;
            $display("hit=%0d", hits);
          end
        endmodule
    )");
    CHECK(out.find("hit=1") != std::string::npos);
    CHECK(out.find("hit=2") != std::string::npos);
    CHECK(out.find("hit=3") != std::string::npos);
}

// A mixed event list `@(e or f)` wakes on any member (LRM 9.4.2).
TEST_CASE("mixed @(e or f) wakes on either event") {
    const std::string out = run_capture(R"(
        module top;
          event e, f;
          initial begin #7 -> f; #10 $finish; end
          initial begin
            @(e or f);
            $display("mixed woke t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out.find("mixed woke t=7") != std::string::npos);
}

// An event's waiters are an intrusive list on its object rather than a
// fixed-capacity bucket (docs/design/event-handles.md), so the number of
// simultaneous waiters is bounded by nothing the compiler has to predict. 200
// fork children parked on one event must all wake from one trigger.
TEST_CASE("an event wakes an unbounded number of simultaneous waiters") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          int woke = 0;
          initial begin
            for (int i = 0; i < 200; i++) fork begin @(e); woke++; end join_none
            #1 -> e;
            #1 $display("woke=%0d", woke);
          end
        endmodule
    )");
    CHECK(out == "woke=200\n");
}

// Persistent (`always @(e)`) and one-shot (`@(e)` in a body) waiters share one
// list per event, with nodes from different storage: the contiguous
// clotho.event_nodes array and the coroutine frame respectively. A trigger must
// reach both kinds, and each `always` block must be woken independently.
TEST_CASE("persistent and one-shot event waiters share one list") {
    SUBCASE("mixed kinds") {
        const std::string out = run_capture(R"(
            module top;
              event e; int a = 0, b = 0;
              always @(e) a++;
              initial forever begin @(e); b++; end
              initial begin #1 -> e; #1 -> e; #1 $display("a=%0d b=%0d", a, b); end
            endmodule
        )");
        CHECK(out == "a=2 b=2\n");
    }
    SUBCASE("three persistent waiters on one event") {
        const std::string out = run_capture(R"(
            module top;
              event e; int x = 0, y = 0, z = 0;
              always @(e) x++;
              always @(e) y++;
              always @(e) z++;
              initial begin #1 -> e; #1 -> e; #1 $display("%0d %0d %0d", x, y, z); end
            endmodule
        )");
        CHECK(out == "2 2 2\n");
    }
}

// A one-shot waiter owns its node and unlinks it on resume. Firing does NOT clear
// the list (unlike a dynamic bucket, which resets its whole length), so a node
// left linked would be woken again by the next trigger and, in a forever loop,
// re-linked on top of itself. Re-arming thousands of times must stay exact.
TEST_CASE("a one-shot event waiter unlinks itself on resume") {
    SUBCASE("repeated re-arm") {
        const std::string out = run_capture(R"(
            module top;
              event e; int n = 0;
              initial forever begin @(e); n++; end
              initial begin
                for (int i = 0; i < 500; i++) begin #1 -> e; end
                #1 $display("n=%0d", n);
              end
            endmodule
        )");
        CHECK(out == "n=500\n");
    }
    SUBCASE("multi-event wait unlinks from every event it linked") {
        const std::string out = run_capture(R"(
            module top;
              event a, b, c; int n = 0;
              initial forever begin @(a or b or c); n++; end
              initial begin #1 -> a; #1 -> b; #1 -> c; #1 -> a; #1 $display("n=%0d", n); end
            endmodule
        )");
        CHECK(out == "n=4\n");
    }
    SUBCASE("event and signal in one wait") {
        const std::string out = run_capture(R"(
            module top;
              event e; logic clk = 0; int n = 0;
              initial forever begin @(e or posedge clk); n++; end
              initial begin #1 -> e; #1 clk=1; #1 clk=0; #1 -> e; #1 $display("n=%0d", n); end
            endmodule
        )");
        CHECK(out == "n=3\n");
    }
}

// Each instance of a module that declares an event owns its own event object.
// This is why a compile-time object slot is sound: an event signal is an
// instance-specific hint, so module dedup never body-shares such a definition.
TEST_CASE("each instance of a module owns its own event") {
    const std::string out = run_capture(R"(
        module sub;
          event e; int hits = 0;
          initial forever begin @(e); hits++; end
        endmodule
        module top;
          sub u0(); sub u1();
          initial begin
            #1 -> u0.e; #1 -> u0.e; #1 -> u1.e;
            #1 $display("u0=%0d u1=%0d", u0.hits, u1.hits);
          end
        endmodule
    )");
    CHECK(out == "u0=2 u1=1\n");
}

// `.triggered` is true for the whole triggering step and false afterwards
// (LRM 15.5.3): same-step read after `-> e` is 1, a later step reads 0.
TEST_CASE("e.triggered persists within the triggering step only") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          initial begin
            #5 -> e;
            $display("same=%0b", e.triggered);
            #5 $display("next=%0b", e.triggered);
            $finish;
          end
        endmodule
    )");
    CHECK(out.find("same=1") != std::string::npos);
    CHECK(out.find("next=0") != std::string::npos);
}

// `wait(e.triggered)` proceeds immediately when the event was already triggered
// in the current step, and otherwise blocks until the next trigger — this is the
// race-free counterpart to `@(e)`.
TEST_CASE("wait(e.triggered) catches a same-step trigger") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          initial begin #5 -> e; #10 $finish; end
          initial begin
            wait(e.triggered);
            $display("wait fired t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out.find("wait fired t=5") != std::string::npos);
}

// `wait(<level-expr>)` (LRM 9.4.3): block until an arbitrary condition is true,
// re-testing on any change of the signals it reads. Verified vs iverilog.
TEST_CASE("wait(<level-expr>) blocks until the condition holds") {
    const std::string out = run_capture(R"(
        module top;
          logic [7:0] count = 0;
          initial forever #1 count++;
          initial begin
            wait(count == 8);
            $display("reached at t=%0t", $time);
            $finish;
          end
        endmodule
    )");
    CHECK(out.find("reached at t=8") != std::string::npos);
}

// `->> e` (LRM 15.5.4) defers the wake to the NBA region: a blocking `-> e`
// waiter wakes in the Active region and sees the pre-commit value, while a
// nonblocking `->> e` waiter wakes after the NBA commit and sees the new value.
// (iverilog implements neither `->>` nor `.triggered`; pinned to the LRM.)
TEST_CASE("->> e wakes in the NBA region, after nonblocking updates") {
    const std::string out = run_capture(R"(
        module top;
          event eb, en;
          logic [7:0] q = 0;
          initial begin
            #5;
            q <= 8'hAA;
            -> eb;
            ->> en;
          end
          initial begin @(eb); $display("blk=%02h", q); end
          initial begin @(en); $display("nb=%02h", q); end
          initial #10 $finish;
        endmodule
    )");
    CHECK(out.find("blk=00") != std::string::npos);
    CHECK(out.find("nb=aa") != std::string::npos);
}

// `->> #d e` (LRM 15.5.4) is a nonblocking, *delayed* trigger: the process does
// not block, and the event fires `d` ticks later (unlike `#d -> e`, which sleeps
// the process). iverilog implements no `->>`, so this is pinned to the LRM.
TEST_CASE("->> #d e schedules a detached trigger without blocking") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          initial begin
            ->> #5 e;
            $display("cont=%0t", $time);   // still 0 — did not block
          end
          initial begin @(e); $display("fired=%0t", $time); end
          initial #10 $finish;
        endmodule
    )");
    CHECK(out.find("cont=0") != std::string::npos);
    CHECK(out.find("fired=5") != std::string::npos);
}

// Tier-2 forms loud-fail rather than being silently mis-lowered.
// A named event declared at package scope materializes its trigger state like a
// module-scope event, so triggering/waiting it (imported or explicitly scoped)
// works instead of dereferencing a null event slot. Regression: it used to
// SIGSEGV (collect_global_scope_signals never set SignalInfo::is_event, so the
// backend allocated no trigger-timestamp global).
TEST_CASE("package-scoped named event triggers and wakes a waiter") {
    const std::string out = run_capture(R"(
        package p;
          event e;
        endpackage
        import p::*;
        module top;
          initial begin
            #5 -> e;
            #5 $finish;
          end
          initial forever begin
            @(e);
            $display("woke t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out.find("woke t=5") != std::string::npos);
}

// -- Event handles (LRM 15.5.5): docs/design/event-handles.md --

// An event variable is a handle to a sync object, so `a = b` makes both names
// refer to ONE object: a trigger through either wakes every waiter parked on it.
TEST_CASE("event assignment merges the two handles onto one object") {
    const std::string out = run_capture(R"(
        module top;
          event a, b; int na = 0, nb = 0;
          initial begin
            a = b;
            fork forever begin @(a); na++; end join_none
            fork forever begin @(b); nb++; end join_none
            #1 -> a;
            #1 $display("na=%0d nb=%0d", na, nb);
          end
        endmodule
    )");
    CHECK(out == "na=1 nb=1\n");
}

// A wait captures the object the variable names WHEN IT EXECUTES. Re-pointing the
// variable afterwards must not move an already-parked waiter.
TEST_CASE("a parked event waiter stays on the object it parked on") {
    const std::string out = run_capture(R"(
        module top;
          event a, b; int na = 0, nb = 0;
          initial forever begin @(a); na++; end
          initial forever begin @(b); nb++; end
          initial begin
            #1 a = b;
            #1 -> a;
            #1 $display("na=%0d nb=%0d", na, nb);
          end
        endmodule
    )");
    // The @(a) waiter parked on a's original object at t=0; `-> a` at t=2 fires b's
    // object, so only the @(b) waiter wakes.
    CHECK(out == "na=0 nb=1\n");
}

// `null` is the all-zero handle and the power-on default (LRM Table 7-1 / 15.5.5):
// `-> null` is a no-op, `null.triggered` is 0, and `@(null)` blocks forever.
TEST_CASE("a null event handle triggers nothing and never wakes") {
    SUBCASE("no-op trigger, triggered stays 0") {
        CHECK(run_capture("module top; event a; initial begin a = null; -> a; "
                          "$display(\"t=%0b\", a.triggered); end endmodule") == "t=0\n");
    }
    SUBCASE("power-on default is null") {
        CHECK(run_capture("module top; event a; initial $display(\"n=%0b\", a == null); "
                          "endmodule") == "n=0\n");
    }
    SUBCASE("@(null) blocks forever") {
        CHECK(run_capture("module top; event a; initial begin a = null; @(a); $display(\"BAD\"); end "
                          "initial #5 $display(\"blocked\"); endmodule") == "blocked\n");
    }
    SUBCASE("re-pointing away from null restores triggering") {
        CHECK(run_capture(R"(
            module top;
              event a, keep; int n = 0;
              initial forever begin @(keep); n++; end
              initial begin
                a = keep; -> a; #1;
                a = null; -> a; #1;
                a = keep; -> a; #1;
                $display("n=%0d", n);
              end
            endmodule
        )") == "n=2\n");
    }
}

// `==` on two event handles is an identity compare of the objects they name, not a
// value compare (there is no value): distinct declarations never compare equal, and
// a merged handle compares equal to its source.
TEST_CASE("event handle equality is object identity") {
    const std::string out = run_capture(R"(
        module top;
          event a, b, c;
          initial begin
            $display("%0b%0b", a == a, a == b);
            c = a;
            $display("%0b%0b", c == a, c == b);
          end
        endmodule
    )");
    CHECK(out == "10\n10\n");
}

// Both nonblocking forms follow the handle, not the declaration: after `a = b`,
// `->> a` and `->> #d a` must fire b's object.
TEST_CASE("nonblocking triggers follow a merged handle") {
    SUBCASE("->> in the NBA region") {
        const std::string out = run_capture(R"(
            module top;
              event a, b; logic [7:0] q = 0;
              initial begin a = b; #5; q <= 8'hAA; ->> a; end
              initial begin @(b); $display("q=%02h", q); end
              initial #10 $finish;
            endmodule
        )");
        CHECK(out.find("q=aa") != std::string::npos);
    }
    SUBCASE("->> #d detached timer") {
        const std::string out = run_capture(R"(
            module top;
              event a, b;
              initial begin a = b; ->> #4 a; end
              initial begin @(b); $display("t=%0t", $time); end
              initial #10 $finish;
            endmodule
        )");
        CHECK(out.find("t=4") != std::string::npos);
    }
}

// A `static` class property of event type is ordinary persistent module-signal
// storage holding a handle, so it needs no per-object machinery and works: ONE
// event shared by every instance of the class (LRM 8.9).
TEST_CASE("a static event class property is one shared event") {
    const std::string out = run_capture(R"(
        module top;
          class C;
            static event e;
            task fire(); -> e; endtask
          endclass
          int n = 0;
          initial forever begin @(C::e); n++; end
          initial begin
            C c1 = new(); C c2 = new();
            #1 c1.fire();
            #1 c2.fire();
            #1 $display("n=%0d", n);
          end
        endmodule
    )");
    CHECK(out == "n=2\n");
}

// An event port is an ALIASING port in every direction (LRM 15.5.5): both sides
// name the SAME sync object, so a trigger on either side reaches waiters on the
// other. There is no copy process -- copying the handle would break a later merge,
// and there is no value to propagate. (`inout event` is illegal SV: slang requires
// a net, and an event is not one.)
TEST_CASE("an event port shares one object across the boundary") {
    SUBCASE("input: parent triggers, child waits") {
        const std::string out = run_capture(R"(
            module sub(input event e);
              initial begin @(e); $display("child t=%0t", $time); end
            endmodule
            module top; event e; sub u(e); initial begin #3 -> e; #1 $finish; end endmodule
        )");
        CHECK(out.find("child t=3") != std::string::npos);
    }
    SUBCASE("output: child triggers, parent waits") {
        const std::string out = run_capture(R"(
            module sub(output event e); initial #2 -> e; endmodule
            module top;
              event e; sub u(e);
              initial begin @(e); $display("parent t=%0t", $time); end
            endmodule
        )");
        CHECK(out.find("parent t=2") != std::string::npos);
    }
    SUBCASE("two instances keep distinct events") {
        const std::string out = run_capture(R"(
            module sub(input event e); int n = 0; initial forever begin @(e); n++; end endmodule
            module top;
              event a, b;
              sub u0(a); sub u1(b);
              initial begin #1 -> a; #1 -> a; #1 -> b; #1 $display("u0=%0d u1=%0d", u0.n, u1.n); end
            endmodule
        )");
        CHECK(out == "u0=2 u1=1\n");
    }
}

// LRM 15.5.5's own example: pass a handle, and the callee triggers the caller's
// event. Every direction works, because the handle is an ordinary scalar.
TEST_CASE("an event passes as a subroutine argument") {
    SUBCASE("input formal, triggered by the callee") {
        CHECK(run_capture(R"(
            module top;
              event e; int n = 0;
              task automatic fire(event x); -> x; endtask
              initial forever begin @(e); n++; end
              initial begin #1 fire(e); #1 fire(e); #1 $display("n=%0d", n); end
            endmodule
        )") == "n=2\n");
    }
    SUBCASE("nonblocking and delayed triggers through a formal") {
        CHECK(run_capture(R"(
            module top;
              event e;
              task automatic fire(event x); ->> #2 x; endtask
              initial begin @(e); $display("t=%0t", $time); end
              initial begin #1 fire(e); #5 $finish; end
            endmodule
        )").find("t=3") != std::string::npos);
    }
    SUBCASE("a class method taking an event") {
        CHECK(run_capture(R"(
            module top;
              event e; int n = 0;
              class C; task fire(event x); -> x; endtask endclass
              initial forever begin @(e); n++; end
              initial begin C c = new(); #1 c.fire(e); #1 $display("n=%0d", n); end
            endmodule
        )") == "n=1\n");
    }
}

TEST_CASE("event forms that are still rejected") {
    CHECK(reject("module top; logic clk; event e; initial ->> @(posedge clk) e; endmodule").find(
            "event-controlled nonblocking trigger") != std::string::npos);
    CHECK(reject("module top; event a, b; initial wait_order(a, b); endmodule").find(
            "wait_order is not supported") != std::string::npos);
}

// An event declared in a procedural scope gets signal-like storage through
// declare_local / declare_static_local. Those two paths never set
// SignalInfo::is_event, so the backend allocated no trigger-timestamp global and
// emit_event_fire stored through a null pointer: `clotho build` died with SIGSEGV
// on any of these, with no diagnostic. Same failure mode as the package-scope
// regression above, in the other two registration paths.
TEST_CASE("a procedural-scope named event triggers and reads .triggered") {
    SUBCASE("initial block-local") {
        CHECK(run_capture("module top; initial begin event e; -> e; "
                          "$display(\"t=%0b\", e.triggered); end endmodule") == "t=1\n");
    }
    SUBCASE("named block-local") {
        CHECK(run_capture("module top; initial begin : blk event e; -> e; "
                          "$display(\"t=%0b\", e.triggered); end endmodule") == "t=1\n");
    }
    SUBCASE("static local in a process") {
        CHECK(run_capture("module top; initial begin static event e; -> e; "
                          "$display(\"t=%0b\", e.triggered); end endmodule") == "t=1\n");
    }
    SUBCASE("static local in a task") {
        CHECK(run_capture("module top; task t(); static event e; -> e; "
                          "$display(\"t=%0b\", e.triggered); endtask "
                          "initial t(); endmodule") == "t=1\n");
    }
    SUBCASE("loop-body declaration, re-entered") {
        CHECK(run_capture("module top; initial for (int i=0;i<2;i++) begin "
                          "event e; -> e; $display(\"i%0d=%0b\", i, e.triggered); "
                          "end endmodule") == "i0=1\ni1=1\n");
    }
}

// A block-local event owns real wait queues, not just a timestamp: a waiter
// parked on it must actually be woken by the trigger.
TEST_CASE("a block-local named event wakes a waiter") {
    const std::string out = run_capture(R"(
        module top;
          initial begin
            event e;
            fork begin @(e); $display("woke t=%0t", $time); end join_none
            #5 -> e;
            #1 $display("done");
          end
        endmodule
    )");
    CHECK(out == "woke t=5\ndone\n");
}

// An automatic-lifetime event local is bound to a FRESH object on every entry
// (LRM Table 6-7's "New event"), so two activations never share one.
TEST_CASE("an automatic-lifetime event local is a new event per activation") {
    SUBCASE("each call gets its own") {
        CHECK(run_capture("module top; int n = 0;\n"
                          "  task automatic t(); event e; -> e; if (e.triggered) n++; endtask\n"
                          "  initial begin t(); t(); t(); $display(\"n=%0d\", n); end\n"
                          "endmodule") == "n=3\n");
    }
    SUBCASE("activations do not share the object") {
        // `pre` reads .triggered BEFORE this activation triggers. Both calls run
        // in the same time step, so a SHARED object would still carry the first
        // call's stamp and the second would read 1. One object per activation makes
        // both read 0.
        CHECK(run_capture(R"(
            module top;
              task automatic t(output bit pre);
                event e;
                pre = e.triggered;
                -> e;
              endtask
              bit b1, b2;
              initial begin
                t(b1); t(b2);
                $display("%0b%0b", b1, b2);
              end
            endmodule
        )") == "00\n");
    }
}

// Every event op stamps or compares the current tick via rt_wheel_now(wheel), and
// a function carries no wheel in its ABI (the same restriction $time, $strobe and
// $monitor already state). The call was emitted with a null operand, so the LLVM
// verifier rejected the module ("Operand is null") with no located diagnostic.
// A task is a coroutine and does carry the wheel, so it must keep working.
TEST_CASE("an event operation inside a function is rejected") {
    CHECK(reject("module top; event a; function int f(); -> a; return 1; endfunction "
                 "initial $display(\"%0d\", f()); endmodule")
            .find("'-> e' inside a function is not supported") != std::string::npos);
    CHECK(reject("module top; event a; function int f(); ->> a; return 1; endfunction "
                 "initial $display(\"%0d\", f()); endmodule")
            .find("'->> e' inside a function is not supported") != std::string::npos);
    CHECK(reject("module top; event a; function int f(); return a.triggered; endfunction "
                 "initial $display(\"%0d\", f()); endmodule")
            .find("'e.triggered' inside a function is not supported") != std::string::npos);
    // The same statement in a task is fine.
    CHECK(run_capture("module top; event a; task t(); -> a; endtask "
                      "initial begin t(); $display(\"t=%0b\", a.triggered); end endmodule") ==
          "t=1\n");
}

// `@(u.e)` has always accepted a hierarchical event (event_signal admits
// HierarchicalValue), but trigger_target demanded a bare NamedValue -- so a
// submodule's event was waitable from outside and not triggerable. Both paths end
// in the same signal_of, so the asymmetry was purely the expression-kind test.
TEST_CASE("a hierarchical named event can be triggered, not just waited on") {
    SUBCASE("through an instance name") {
        const std::string out = run_capture(R"(
            module sub; event e; initial begin @(e); $display("sub woke t=%0t", $time); end endmodule
            module top; sub u(); initial begin #3 -> u.e; #1 $display("done"); end endmodule
        )");
        CHECK(out == "sub woke t=3\ndone\n");
    }
    SUBCASE("through an interface port") {
        const std::string out = run_capture(R"(
            interface I; event e; endinterface
            module child(I p); initial begin @(p.e); $display("child woke t=%0t", $time); end endmodule
            module top; I i(); child c(i); initial begin #4 -> i.e; #1 $display("done"); end endmodule
        )");
        CHECK(out == "child woke t=4\ndone\n");
    }
    SUBCASE("nonblocking trigger of a hierarchical event") {
        const std::string out = run_capture(R"(
            module sub; event e; initial begin @(e); $display("sub woke t=%0t", $time); end endmodule
            module top; sub u(); initial begin #1 ->> u.e; #2 $display("done"); end endmodule
        )");
        CHECK(out == "sub woke t=1\ndone\n");
    }
}

// A per-instance event class property is one event PER OBJECT: the body's leaf is
// bound by emit_class_body_init at `new` time, from the runtime object pool.
TEST_CASE("a per-instance event class property is one event per object") {
    const std::string out = run_capture(R"(
        module top;
          class C; event e; endclass
          int n1 = 0, n2 = 0;
          C c1, c2;
          initial begin
            c1 = new(); c2 = new();
            fork forever begin @(c1.e); n1++; end join_none
            fork forever begin @(c2.e); n2++; end join_none
            #1 -> c1.e; #1 -> c1.e; #1 -> c2.e;
            #1 $display("n1=%0d n2=%0d", n1, n2);
          end
        endmodule
    )");
    CHECK(out == "n1=2 n2=1\n");
}

// `wait_order` (LRM 15.5.6) fell through lower_stmt's `default:` to the bare
// "unsupported statement", naming nothing.
TEST_CASE("wait_order is rejected by name") {
    CHECK(reject("module top; event a, b; initial wait_order(a, b); endmodule")
            .find("wait_order is not supported") != std::string::npos);
    CHECK(reject("module top; event a, b; initial wait_order(a, b) $display(\"x\"); "
                 "else $display(\"y\"); endmodule")
            .find("wait_order is not supported") != std::string::npos);
}

// LRM 9.4.2.3: `@(edge sig iff cond)` triggers only when the edge fires and the
// guard holds at event time. Clotho gates the body with `if (cond)` (observably
// identical, the block re-subscribes each pass).
TEST_CASE("iff qualifier gates a top-level sensitivity list") {
    const std::string out = run_capture(R"(
        module m;
            logic clk=0, en=0; int count=0;
            always @(posedge clk iff en) count++;
            initial begin
                en=0; clk=1; #1 clk=0; #1;
                en=1; clk=1; #1 clk=0; #1;
                clk=1; #1 clk=0; #1;
                en=0; clk=1; #1 clk=0; #1;
                $display("count=%0d", count);
            end
        endmodule
    )");
    CHECK(out == "count=2\n");
}

// Intra-body `@(edge iff cond)` re-waits until the edge fires with the guard
// true: forever { @(ev); if (cond) break; }.
TEST_CASE("iff qualifier gates an intra-body event wait") {
    const std::string out = run_capture(R"(
        module m;
            logic clk=0, en=0; int hits=0;
            initial forever begin
                @(posedge clk iff en);
                hits++;
                if (hits==2) begin $display("hits=%0d", hits); $finish; end
            end
            initial begin
                en=0; clk=1; #1 clk=0; #1;
                en=1; clk=1; #1 clk=0; #1;
                clk=1; #1 clk=0; #1;
            end
        endmodule
    )");
    CHECK(out == "hits=2\n");
}

// An event control on a struct member (`@(posedge s.clk)`) has no direct
// whole-signal to watch (the member is a part-select at a nonzero bit offset).
// Clotho synthesizes a temp net driven by the member and subscribes to it: the
// net's bit 0 is the member's LSB (exact edge), its whole word feeds the level
// `changed` compare. Oracle: iverilog 13 (cnt=2 lvl=3).
TEST_CASE("event control on struct members (edge and level)") {
    const std::string out = run_capture(R"(
        module m;
            typedef struct packed { logic [7:0] data; logic clk; } s_t;
            s_t s; int cnt=0, lvl=0;
            always @(posedge s.clk) cnt++;
            always @(s.data) lvl++;
            initial begin
                s='0;
                #1 s.clk=1; #1 s.clk=0; #1 s.clk=1;
                #1 s.data=8'hAA; #1 s.data=8'h55;
                #1 $display("cnt=%0d lvl=%0d", cnt, lvl);
            end
        endmodule
    )");
    CHECK(out == "cnt=2 lvl=3\n");
}

// A dynamically-indexed element select in an event control (`@(posedge
// arr[idx][0])`) synthesizes a temp net whose continuous driver is sensitive to
// both `arr` and `idx` (the select's reads). Oracle: iverilog 13 (cnt=2).
TEST_CASE("event control on a dynamic element select") {
    const std::string out = run_capture(R"(
        module m;
            logic [7:0] arr [0:3]; int idx=0, cnt=0;
            always @(posedge arr[idx][0]) cnt++;
            initial begin
                arr[0]=0; arr[1]=0; arr[2]=0; arr[3]=0;
                #1 arr[0][0]=1;
                #1 arr[1][0]=1;
                #1 idx=1;
                #1 arr[1][0]=0;
                #1 arr[1][0]=1;
                #1 $display("cnt=%0d", cnt);
            end
        endmodule
    )");
    CHECK(out == "cnt=2\n");
}

// Per-event `iff` in a multi-event list is not representable: the shared-wake
// engine cannot tell which event fired to pick the matching guard, so it is
// rejected loudly (LRM 9.4.2.3, documented gap).
TEST_CASE("iff on a multi-event control is rejected") {
    CHECK(reject("module m; logic a=0,b=0,en=0; int c=0; "
                 "always @(posedge a iff en or posedge b) c++; endmodule")
            .find("iff qualifier on a multi-event control") != std::string::npos);
}
