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

// JIT-run a module and capture stdout (mirrors task_test.cpp). Named-event
// behavior for `->` / `@(e)` / `always @(e)` was cross-checked against Icarus
// Verilog; `.triggered` is not implemented by iverilog ("Event name e can't
// have member names"), so those cases are pinned to the LRM 15.5.3 semantics
// (triggered true only within the triggering time step).
std::string run_capture(std::string_view source, ir::SimulationConfig config = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module, nullptr, config);

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

// `->> e` (LRM 15.5.1) defers the wake to the NBA region: a blocking `-> e`
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

// `->> #d e` (LRM 15.5.1) is a nonblocking, *delayed* trigger: the process does
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

// The `->> #d e` expiry thunk is one function shared by every delayed trigger in
// the design, and the design is split into one LLVM module per definition plus a
// root. A trigger in a fork child and another in a plain process reach it from two
// modules, and LLVM's verifier rejected the second one's bare reference ("Global
// is referenced in a different module!") for every delay. The waiters observe the
// two triggers' own instants. iverilog implements no `->>`.
TEST_CASE("->> #d e in a fork child and in a plain process shares one expiry thunk") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          initial begin
            fork
              ->> #1 e;
            join_none
          end
          initial ->> #2 e;
          initial begin
            @(e) $display("a t=%0t", $time);
            @(e) $display("b t=%0t", $time);
          end
        endmodule
    )");
    CHECK(out == "a t=1\nb t=2\n");
}

// LRM 15.5.1: the update event of `->> #d e` triggers the event "in the
// nonblocking assignment region" of the instant its delay expires, as `->> #0 e`
// does. A positive delay's timer used to fire the event at the top of that instant,
// so its waiter woke in the Active region and read `q` before the NBA update that
// the same instant executes (4.4.2.4) had committed it. The waiter now sees the
// committed value, `->> e`'s behaviour (above). iverilog implements no `->>`.
TEST_CASE("->> #d e wakes its waiter in the NBA region of the expiry instant") {
    const std::string out = run_capture(R"(
        module top;
          event e;
          logic [7:0] q = 0;
          initial ->> #5 e;
          initial #5 q <= 8'hAA;
          initial begin @(e); $display("nb=%02h t=%0t", q, $time); end
        endmodule
    )");
    CHECK(out == "nb=aa t=5\n");
}

// `->> #0 e` (LRM 15.5.1): the delay control expires in the current time slot, so
// the trigger is that slot's NBA-region event, exactly `->> e`. The delay used to
// reach the wheel, which cannot hold a timer due now, so the waiter never woke and
// a fork joining on it never joined. The expected order is the LRM's regions
// (4.4.2.3, 4.4.2.4): the `#0` continuation runs in the Inactive region, before
// the NBA region that triggers `e`. Each fork branch `@(f)` runs in the Active
// region, before the NBA region too, so it is waiting by the time `f` triggers
// whatever order the branches start in. iverilog implements no `->>`, so this is
// pinned to the LRM. It runs in a process of its own because the unfixed wheel
// asserts, which would otherwise end the whole test file.
TEST_CASE("->> #0 e triggers in the current slot's NBA region") {
    constexpr std::string_view src = R"(
        module top;
          event e, f, g;
          int d;
          initial begin
            d = 0;
            #1;
            ->> #0 e;
            #0 $display("inactive t=%0t", $time);
          end
          initial begin @(e); $display("e woke t=%0t", $time); end
          initial begin
            #2;
            fork
              @(f);
              ->> #0 f;
            join
            $display("f joined t=%0t", $time);
          end
          initial begin
            #3;
            fork
              @(g);
              ->> #(d) g;
            join
            $display("g joined t=%0t", $time);
          end
        endmodule
    )";
    const LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(test::run_design_subprocess(src) ==
          "inactive t=1\ne woke t=1\nf joined t=2\ng joined t=3\n");
}

// LRM 9.4.1: a delay that evaluates to an unknown value "shall be interpreted as
// zero delay", and `->> [delay_or_event_control]` (15.5.1) takes its delay control
// from the same production, so with an x or z bit `->> #(x) e` is `->> #0 e`: each
// trigger below waited 9 units and now wakes its waiter in the NBA region of the
// instant it ran in, after the `#0` continuation (LRM 4.4.2.3, 4.4.2.4). The
// literal delay is zero at lowering time and the variable one at run time.
// iverilog implements no `->>`, so this is pinned to the LRM.
TEST_CASE("->> #(x) e: a delay with an x or z bit is a zero delay") {
    const std::string out = run_capture(R"(
        module top;
          event e, f;
          logic [3:0] dx = 4'b1x01;
          initial begin
            #1;
            ->> #(4'b1x01) e;
            #0 $display("inactive t=%0t", $time);
            #1;
            ->> #(dx) f;
            #0 $display("inactive t=%0t", $time);
          end
          initial begin @(e); $display("e woke t=%0t", $time); end
          initial begin @(f); $display("f woke t=%0t", $time); end
        endmodule
    )");
    CHECK(out == "inactive t=1\ne woke t=1\ninactive t=2\nf woke t=2\n");
}

// Tier-2 forms loud-fail rather than being silently mis-lowered (PRINCIPLES.md).
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

// Two instances of one module each declare events, trigger them every way
// (`->`, `->>`), wait on them (`@e`, `always @(e)`) and take event ports bound to
// different events of their parent: each keeps its own objects, though the
// module's code is compiled once (cli_test: --stat=unit-code).
TEST_CASE("twin instances keep their own events") {
    const std::string out = run_capture(R"(
        module leaf(input event go, output event done);
          event e, f;
          int n = 0, m = 0, k = 0;
          always @(e) n++;
          initial forever begin @(f); m++; $display("f t=%0t trig=%0d m=%0d", $time, f.triggered, m); end
          initial forever begin
            @(go);
            k++;
            -> e;
            ->> f;
            #1 -> done;
          end
        endmodule
        module top;
          event g0, g1, d0, d1;
          leaf u0(g0, d0);
          leaf u1(g1, d1);
          initial forever begin @(d0); $display("d0 t=%0t", $time); end
          initial forever begin @(d1); $display("d1 t=%0t", $time); end
          initial begin
            #1 -> g0;
            #2 -> g1;
            #2 ->> g0;
            #2 -> g0;
            #3 -> g1;
            #5 $display("u0 n=%0d m=%0d k=%0d u1 n=%0d m=%0d k=%0d", u0.n, u0.m, u0.k, u1.n, u1.m, u1.k);
          end
        endmodule
    )");
    CHECK(out == "f t=1 trig=1 m=1\nd0 t=2\nf t=3 trig=1 m=1\nd1 t=4\nf t=5 trig=1 m=2\nd0 t=6\n"
                 "f t=7 trig=1 m=3\nd0 t=8\nf t=10 trig=1 m=2\nd1 t=11\n"
                 "u0 n=3 m=3 k=3 u1 n=2 m=2 k=2\n");
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

// LRM Table 6-7 gives an event's default initial value as "New event", so every
// event-typed storage location is bound to its OWN object when that storage comes
// into existence. Distinctness is the property worth pinning: a waiter on one leaf
// must not be woken by a trigger of another.
TEST_CASE("an event in an aggregate is one event per leaf") {
    SUBCASE("fixed unpacked array") {
        CHECK(run_capture(R"(
            module top;
              event ev[4]; int n0 = 0, n2 = 0;
              initial forever begin @(ev[0]); n0++; end
              initial forever begin @(ev[2]); n2++; end
              initial begin
                #1 -> ev[0]; #1 -> ev[0]; #1 -> ev[2];
                #1 $display("n0=%0d n2=%0d", n0, n2);
              end
            endmodule
        )") == "n0=2 n2=1\n");
    }
    SUBCASE("multidimensional array") {
        CHECK(run_capture(R"(
            module top;
              event ev[2][3]; int n = 0;
              initial forever begin @(ev[1][2]); n++; end
              initial begin #1 -> ev[1][2]; #1 -> ev[0][0]; #1 $display("n=%0d", n); end
            endmodule
        )") == "n=1\n");
    }
    SUBCASE("struct member, one per struct variable") {
        CHECK(run_capture(R"(
            module top;
              typedef struct { event e; } s_t;
              s_t a, b; int na = 0, nb = 0;
              initial forever begin @(a.e); na++; end
              initial forever begin @(b.e); nb++; end
              initial begin #1 -> a.e; #1 -> a.e; #1 -> b.e;
                            #1 $display("na=%0d nb=%0d", na, nb); end
            endmodule
        )") == "na=2 nb=1\n");
    }
}

// A container element is bound by rt_elem_init, which for an event constructs a new
// object per slot instead of copying the prototype image. Copying an element copies
// the HANDLE, so both slots then name one object (LRM 15.5.5's merge).
TEST_CASE("an event in a container is one event per element") {
    SUBCASE("dynamic array") {
        CHECK(run_capture(R"(
            module top;
              event d[]; int n = 0;
              initial begin
                d = new[3];
                fork forever begin @(d[1]); n++; end join_none
                #1 -> d[1]; #1 -> d[1]; #1 -> d[0];
                #1 $display("n=%0d", n);
              end
            endmodule
        )") == "n=2\n");
    }
    SUBCASE("queue, and a pushed handle is shared") {
        CHECK(run_capture(R"(
            module top;
              event q[$]; event a; int n = 0;
              initial begin
                q.push_back(a); q.push_back(a);
                fork forever begin @(q[0]); n++; end join_none
                #1 -> a;
                #1 $display("n=%0d same=%0b", n, q[0] == q[1]);
              end
            endmodule
        )") == "n=1 same=1\n");
    }
    SUBCASE("associative array") {
        CHECK(run_capture(R"(
            module top;
              event aa[string]; int n = 0;
              initial begin
                fork forever begin @(aa["k"]); n++; end join_none
                #1 -> aa["k"]; #1 -> aa["k"];
                #1 $display("n=%0d", n);
              end
            endmodule
        )") == "n=2\n");
    }
    SUBCASE("an absent key reads a non-null event, not null") {
        // LRM 7.9.11 gives the element default, which for an event is a new event
        // (Table 6-7) -- so `== null` must read 0.
        CHECK(run_capture("module top; event aa[string]; "
                          "initial $display(\"%0b\", aa[\"absent\"] != null); endmodule") == "1\n");
    }
}

// LRM 15.5.5.3: an event compares against another event or null with ==, !=, ===
// and !==, and tests as a Boolean that is 0 iff it is null.
TEST_CASE("an event tests as a Boolean and compares with every allowed operator") {
    CHECK(run_capture(R"(
        module top;
          event a, b;
          initial begin
            $display("%0b%0b%0b%0b", a == b, a != b, a === b, a !== b);
            if (a) $display("bound");
            a = null;
            if (!a) $display("null");
          end
        endmodule
    )") == "0101\nbound\nnull\n");
}

// `->> @(x) e` / `->> repeat(n) @(x) e` (LRM 15.5.1): a DETACHED one-shot
// subscription on the edge. The process does not block, and when the edge matures
// the subscription the event is triggered in the NBA region. Registrations are
// runtime-owned and growable, which is what a static wait bucket could not bound.
TEST_CASE("an event-controlled nonblocking trigger fires from a detached subscription") {
    SUBCASE("does not block, fires on the edge") {
        const std::string out = run_capture(R"(
            module top;
              logic clk = 0; event e;
              initial begin @(e); $display("woke t=%0t", $time); end
              initial begin ->> @(posedge clk) e; #1 $display("free t=%0t", $time); end
              initial begin #5 clk = 1; #5 $finish; end
            endmodule
        )");
        CHECK(out.find("free t=1") != std::string::npos);
        CHECK(out.find("woke t=5") != std::string::npos);
    }
    SUBCASE("repeat(n) waits for the nth occurrence") {
        CHECK(run_capture(R"(
            module top;
              logic clk = 0; event e;
              initial begin @(e); $display("woke t=%0t", $time); end
              initial ->> repeat(2) @(posedge clk) e;
              initial begin #2 clk=1; #2 clk=0; #2 clk=1; #4 $finish; end
            endmodule
        )").find("woke t=6") != std::string::npos);
    }
    SUBCASE("the wake lands in the NBA region") {
        CHECK(run_capture(R"(
            module top;
              logic clk = 0; event e; logic [7:0] q = 0;
              initial begin @(e); $display("q=%02h", q); end
              initial ->> @(posedge clk) e;
              initial begin #5 q <= 8'hAA; clk = 1; #5 $finish; end
            endmodule
        )").find("q=aa") != std::string::npos);
    }
    SUBCASE("registrations are unbounded") {
        CHECK(run_capture(R"(
            module top;
              logic clk = 0; event e; int n = 0;
              initial forever begin @(e); n++; end
              initial begin
                for (int i = 0; i < 300; i++) ->> @(posedge clk) e;
                #1 clk = 1;
                #2 $display("n=%0d", n);
              end
            endmodule
        )") == "n=1\n");
    }
}

// The control of a detached subscription may itself be a named event
// (`->> @(e1) e2`, LRM 15.5.1). An event wakes through its object, not a signal
// edge, so the subscription has to be counted down by the trigger of `e1`; it used
// to be registered on a signal trigger nothing ever fired, and `e2` never
// occurred. iverilog does not implement `->>` with an event control, so this is
// pinned to the LRM.
TEST_CASE("a nonblocking trigger controlled by a named event fires when that event does") {
    SUBCASE("plain") {
        const std::string out = run_capture(R"(
            module top;
              event e1, e2;
              initial begin ->> @(e1) e2; #1 -> e1; end
              initial begin @(e2); $display("e2 t=%0t", $time); end
              initial #10 $finish;
            endmodule
        )");
        CHECK(out.find("e2 t=1") != std::string::npos);
    }
    SUBCASE("repeat(n) counts occurrences of the controlling event") {
        const std::string out = run_capture(R"(
            module top;
              event e1, e2;
              initial ->> repeat (2) @(e1) e2;
              initial begin @(e2); $display("e2 t=%0t", $time); end
              initial begin #1 -> e1; #1 -> e1; end
              initial #10 $finish;
            endmodule
        )");
        CHECK(out.find("e2 t=2") != std::string::npos);
    }
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

// Every event op stamps or compares the current tick. A function carries no wheel
// parameter, so the wheel is a module global (clotho.wheel) and a function reads
// the clock exactly as a task does -- rather than the op being refused.
TEST_CASE("event operations work inside a function") {
    SUBCASE("trigger from a function wakes a waiter") {
        CHECK(run_capture("module top;\n"
                          "  event e; int n = 0;\n"
                          "  always @(e) n++;\n"
                          "  function int f(); -> e; return 1; endfunction\n"
                          "  initial begin #1 if (f()) ; #1 $display(\"n=%0d\", n); end\n"
                          "endmodule") == "n=1\n");
    }
    SUBCASE(".triggered read from a function") {
        CHECK(run_capture("module top;\n"
                          "  event e;\n"
                          "  function int f(); return e.triggered; endfunction\n"
                          "  initial begin -> e; $display(\"t=%0d\", f()); end\n"
                          "endmodule") == "t=1\n");
    }
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

// wait_order (LRM 15.5.4) suspends until every listed event triggers left to
// right, failing the moment a not-yet-reached event triggers out of turn. It
// desugars to one park per stage, each waiting on the events still ahead of it.
TEST_CASE("wait_order sequences events") {
    const auto seq = [](std::string_view order) {
        return run_capture(std::string("module top;\n"
                                       "  event a, b, c;\n"
                                       "  initial wait_order(a, b, c) $display(\"PASS\"); "
                                       "else $display(\"FAIL\");\n"
                                       "  initial begin ") +
                           std::string(order) + " #2 $display(\"end\"); end\nendmodule\n");
    };
    SUBCASE("in order passes") {
        CHECK(seq("#1 -> a; #1 -> b; #1 -> c;") == "PASS\nend\n");
    }
    SUBCASE("out of order fails") {
        CHECK(seq("#1 -> a; #1 -> c; #1 -> b;") == "FAIL\nend\n");
    }
    SUBCASE("a preceding event may retrigger") {
        // LRM: "once an event occurs in the prescribed order, it can be triggered
        // again without causing the construct to fail".
        CHECK(seq("#1 -> a; #1 -> a; #1 -> b; #1 -> a; #1 -> c;") == "PASS\nend\n");
    }
    SUBCASE("only the first event may use the persistent triggered state") {
        CHECK(run_capture(R"(
            module top;
              event a, b;
              initial -> a;
              initial wait_order(a, b) $display("PASS"); else $display("FAIL");
              initial begin #1 -> b; #1 $display("end"); end
            endmodule
        )") == "PASS\nend\n");
    }
    SUBCASE("the status-variable form") {
        CHECK(run_capture(R"(
            module top;
              event a, b; bit success;
              initial begin wait_order(a, b) success = 1; else success = 0;
                            $display("s=%0b", success); end
              initial #1 -> b;
            endmodule
        )") == "s=0\n");
    }
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

// -- Event control on a class property (LRM 9.4.2) ---------------------------
//
// A class property is heap storage inside an object, so it owns none of the
// per-signal wait machinery. All three shapes below failed before the change
// notifiers landed: `@p` and `@(obj.p)` reported the property as an unknown
// signal, and `wait(p)` (tests/wait_test.cpp) parked on an empty event list and
// hung with no diagnostic at all. Verilator 5.050 is the oracle -- it prints
// `woke at 1` for the first two -- while iverilog 13 rejects every one of them
// ("cannot synthesize expression: @.<0>"), so it is no oracle here.

// The uvm_event.svh:120 shape: a bare `@p` in a method, on the enclosing `this`.
TEST_CASE("a bare class property in an event control wakes on a change") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                bit on;
                task w(); @on; $display("woke at %0t", $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w(); join_none
                #1 c.on = 1;
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 1\n");
}

// `@(obj.p)` from outside the class. The OBJECT must be resolved when the
// statement executes, not when the process is built: `c` is still null at time
// zero here, and resolving it there aborted with "dereference of a null class
// handle, at 0 s" -- while Verilator prints `woke at 1`.
TEST_CASE("an event control on a property through a handle resolves the object at execution") {
    const std::string out = run_capture(R"(
        module m;
            class C; bit on; endclass
            C c;
            initial begin
                c = new;
                fork begin @(c.on); $display("woke at %0t", $time); end join_none
                #1 c.on = 1;
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 1\n");
}

// LRM 9.4.2: "a non-edge implicit event shall be detected on any CHANGE in the
// value of the expression". A write of the value already there is not a change,
// so it must not satisfy the wait. This is the assertion a notify-on-every-write
// implementation fails, and it holds here because the store itself classifies
// the transition and fires nothing when there is none.
TEST_CASE("a write that does not change a class property does not wake a non-edge waiter") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                bit on = 1;
                task w(); @on; $display("woke at %0t", $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w(); join_none
                #1 c.on = c.on;
                #1 c.on = 0;
                #1;
            end
        endmodule
    )");
    // The unchanged write at t=1 is NOT a wake; the real change at t=2 is.
    CHECK(out == "woke at 2\n");
}

// The notifiers live in the OBJECT, so two instances of one class do not share
// them: writing one must leave the other's waiter parked.
TEST_CASE("a class property change wakes only the written object's waiters") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                bit on;
                task w(string n); @on; $display("%s woke at %0t", n, $time); endtask
            endclass
            C a, b;
            initial begin
                a = new; b = new;
                fork a.w("a"); join_none
                fork b.w("b"); join_none
                #1 a.on = 1;
                #1 b.on = 1;
                #1;
            end
        endmodule
    )");
    CHECK(out == "a woke at 1\nb woke at 2\n");
}

// One change wakes every waiter on that property: the notifier is an intrusive
// list, not a single slot. Wake ORDER within an instant is unspecified (LRM
// 4.7), so only the set and the time are asserted.
TEST_CASE("two waiters on one class property both wake") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                bit on;
                task w(string n); @on; $display("%s woke at %0t", n, $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w("p"); join_none
                fork c.w("q"); join_none
                #1 c.on = 1;
                #1;
            end
        endmodule
    )");
    CHECK(out.find("p woke at 1\n") != std::string::npos);
    CHECK(out.find("q woke at 1\n") != std::string::npos);
    // Exactly those two lines: no third wake, no missing one.
    CHECK(out.size() == std::string_view("p woke at 1\nq woke at 1\n").size());
}

// Writes reach a property from more than a plain store. A `ref` formal is a raw
// aliased pointer the callee writes through, and a base-class method writes an
// inherited property at its BASE-relative field index -- which is why the
// notifier is laid out immediately after the property rather than appended,
// keeping the base's layout a prefix of the derived class's.
TEST_CASE("a class property written through a ref argument or a base method wakes its waiter") {
    const std::string out = run_capture(R"(
        module m;
            class B;
                bit on;
                function void set_base(bit v); on = v; endfunction
            endclass
            class D extends B;
                task w(); @on; $display("woke at %0t", $time); endtask
            endclass
            function automatic void flip(ref bit x); x = 1; endfunction
            D d, e;
            initial begin
                d = new; e = new;
                fork d.w(); join_none
                fork e.w(); join_none
                #1 flip(d.on);
                #1 e.set_base(1);
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 1\nwoke at 2\n");
}

// A runtime pointer write-back is a write path too. $sscanf writes through the
// property's storage pointer, and it wakes because the notifier rides the same
// snapshot/service bracket every blocking write path already goes through.
TEST_CASE("a class property written by $sscanf wakes its waiter") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                int v;
                task w(); @v; $display("woke at %0t v=%0d", $time, v); endtask
            endclass
            C c;
            int n;
            initial begin
                c = new;
                fork c.w(); join_none
                #1 n = $sscanf("42", "%d", c.v);
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 1 v=42\n");
}

// randomize() writes its rand properties through the same store path, so a
// waited-on rand property wakes.
TEST_CASE("a rand class property written by randomize wakes its waiter") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                rand bit [7:0] v;
                task w(); @v; $display("woke at %0t", $time); endtask
            endclass
            C c;
            int ok;
            initial begin
                c = new;
                fork c.w(); join_none
                #1 ok = c.randomize();
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 1\n");
}

// Edges on a property are the SAME classification a signal store performs
// (emit_edge_facts), so Table 9-2 applies unchanged: x->x is no edge at all,
// x->0 is a negedge, 0->1 a posedge. Asserted against the identical signal-only
// design, which is the claim that matters -- the mechanism is uniform, not
// merely plausible. (Verilator disagrees on x->0 for SIGNALS too, so the
// divergence is clotho's existing Table 9-2 position, not new here.)
TEST_CASE("edge event controls on a class property follow Table 9-2") {
    // Asserted against the identical signal-only design below.
    const std::string prop = run_capture(R"(
        module m;
            class C;
                logic f;
                task p(); @(posedge f); $display("pos at %0t f=%b", $time, f); endtask
                task n(); @(negedge f); $display("neg at %0t f=%b", $time, f); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.p(); join_none
                fork c.n(); join_none
                #1 c.f = 1'bx;
                #1 c.f = 0;
                #1 c.f = 1;
                #1;
            end
        endmodule
    )");
    const std::string sig  = run_capture(R"(
        module m;
            logic f;
            initial begin @(posedge f); $display("pos at %0t f=%b", $time, f); end
            initial begin @(negedge f); $display("neg at %0t f=%b", $time, f); end
            initial begin
                #1 f = 1'bx;
                #1 f = 0;
                #1 f = 1;
                #1;
            end
        endmodule
    )");
    CHECK(prop == "neg at 2 f=0\npos at 3 f=1\n");
    CHECK(prop == sig);
}

// A property control in a process's SENSITIVITY list, not its body. It cannot be
// hoisted to a static subscription (the seed loop cannot evaluate `c`), so
// sensitivity_needs_handle keeps it in the body as an intra-body wait.
TEST_CASE("always on a class property re-arms each change") {
    const std::string out = run_capture(R"(
        module m;
            class C; bit on; endclass
            C c;
            initial c = new;
            always @(c.on) $display("woke at %0t", $time);
            initial begin
                #1 c.on = 1;
                #1 c.on = 0;
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 1\nwoke at 2\n");
}

// The object need not be named by a variable. The notifier handle is loaded from
// whatever object the expression evaluates to at the moment the statement runs,
// so an element of an associative array keyed by a class handle -- the
// uvm_objection.svh:1078 shape, `@(m_events[obj].raised)` -- works with nothing
// added, and keeps per-element identity.
TEST_CASE("an event control on a property of an assoc-array element wakes per element") {
    const std::string out = run_capture(R"(
        module m;
            class K; int id; endclass
            class E; bit raised; endclass
            E m_events[K];
            K k1, k2;
            initial begin
                k1 = new; k2 = new;
                m_events[k1] = new; m_events[k2] = new;
                fork begin @(m_events[k1].raised); $display("k1 at %0t", $time); end join_none
                fork begin @(m_events[k2].raised); $display("k2 at %0t", $time); end join_none
                #1 m_events[k2].raised = 1;
                #1 m_events[k1].raised = 1;
                #1;
            end
        endmodule
    )");
    CHECK(out == "k2 at 1\nk1 at 2\n");
}

// Same for a queue and a fixed unpacked array of handles.
TEST_CASE("an event control on a property of a queue or array element wakes per element") {
    const std::string out = run_capture(R"(
        module m;
            class E; bit raised; endclass
            E q[$]; E arr[2]; E tmp;
            initial begin
                tmp = new; q.push_back(tmp); arr[0] = new; arr[1] = new;
                fork begin @(q[0].raised); $display("q at %0t", $time); end join_none
                fork begin @(arr[1].raised); $display("arr at %0t", $time); end join_none
                #1 arr[1].raised = 1;
                #1 q[0].raised = 1;
                #1;
            end
        endmodule
    )");
    CHECK(out == "arr at 1\nq at 2\n");
}

// LRM 8.4: reaching a non-static member through a null handle is a located
// fatal, and the object is resolved when the statement RUNS -- so the report
// carries the time the `@` was reached (1 s), not time zero. The point of the
// case is that an unresolvable object is loud, never a silent park.
TEST_CASE("an event control on a property of a null handle is a located fatal") {
    constexpr std::string_view src = R"(
        module m;
            class E; bit raised; endclass
            E e;
            initial begin
                #1 @(e.raised);
                $display("UNREACHABLE");
            end
        endmodule
    )";
    const LowerResult          r   = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    // The fatal exits the process, so the run needs one of its own. The report
    // itself goes to stderr (rt_null_handle); what stdout must show is that the
    // statement after the `@` never ran -- loud abort, not a silent fall-through
    // and not a silent park.
    CHECK(test::run_design_subprocess(src).find("UNREACHABLE") == std::string::npos);
}

// A shallow copy (LRM 8.12) copies the property VALUES. It must not copy the
// notifier handle: doing so would alias one waiter list into two objects and
// orphan every waiter already parked on the destination's own notifier -- a
// silent hang for a process that was correctly waiting.
TEST_CASE("a shallow copy does not alias or steal a change notifier") {
    const std::string out = run_capture(R"(
        module m;
            class C; bit on; endclass
            C a, b;
            initial begin
                a = new;
                fork begin @(a.on); $display("a woke at %0t", $time); end join_none
                #1 b = new a;
                #1 b.on = 1;
                #1 a.on = 1;
                #1 $display("a.on=%b b.on=%b", a.on, b.on);
            end
        endmodule
    )");
    CHECK(out == "a woke at 3\na.on=1 b.on=1\n");
}

// The blocker this work exists for: Accellera UVM's uvm_event_base::wait_off
// (base/uvm_event.svh:120) parks on `@on`, where `on` is a `protected bit`
// property declared at :54. Byte-identical to Verilator 5.050 on this design.
TEST_CASE("the uvm_event wait_off shape resumes when its protected bit changes") {
    const std::string out = run_capture(R"(
        module m;
            class uvm_event_base;
                protected bit on;
                protected int num_waiters;
                virtual task wait_off(bit delta = 0);
                    if (!on) begin
                        $display("already off at %0t", $time);
                        return;
                    end
                    num_waiters++;
                    @on;
                    num_waiters--;
                    $display("wait_off resumed at %0t on=%b waiters=%0d", $time, on, num_waiters);
                endtask
                virtual function void reset(); on = 0; endfunction
                virtual function void trigger(); on = 1; endfunction
            endclass
            uvm_event_base e;
            initial begin
                e = new;
                e.trigger();
                fork e.wait_off(); join_none
                #1 e.reset();
                #1 e.wait_off();
                #1;
            end
        endmodule
    )");
    CHECK(out == "wait_off resumed at 1 on=0 waiters=0\nalready off at 2\n");
}

// -- Event control on a SCALAR class property (LRM 9.4.2) --------------------
//
// `@p` on every property type whose value one store can compare: a class /
// chandle / virtual interface handle, real / shortreal, and a string. Unlike
// `wait`, `@` is change-DETECTING, so these cases are where "any change in the
// value of the expression" has to be exact -- a store of the value already there
// must not wake, which is the assertion a notify-on-every-write implementation
// fails.

TEST_CASE("a bare class-handle property in an event control wakes on a change") {
    const std::string out = run_capture(R"(
        module m;
            class T; int x; endclass
            class C;
                T h;
                task w(); @h; $display("woke at %0t", $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w(); join_none
                #1 c.h = new;
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 1\n");
}

TEST_CASE("an event control on a real and a shortreal property wakes on a change") {
    const std::string out = run_capture(R"(
        module m;
            class C; real r; shortreal q; endclass
            C c;
            initial begin
                c = new;
                fork begin @(c.r); $display("r at %0t", $time); end join_none
                fork begin @(c.q); $display("q at %0t", $time); end join_none
                #1 c.r = 3.25;
                #1 c.q = -1.5;
                #1;
            end
        endmodule
    )");
    CHECK(out == "r at 1\nq at 2\n");
}

TEST_CASE("an event control on a string property wakes on a content change") {
    const std::string out = run_capture(R"(
        module m;
            class C; string s; endclass
            C c;
            initial begin
                c = new;
                fork begin @(c.s); $display("s at %0t", $time); end join_none
                #1 c.s = "hello";
                #1;
            end
        endmodule
    )");
    CHECK(out == "s at 1\n");
}

TEST_CASE("an event control on a virtual interface property wakes when it is bound") {
    const std::string out = run_capture(R"(
        interface ev_if; logic c; endinterface
        module m;
            ev_if bi();
            class C; virtual ev_if v; endclass
            C c;
            initial begin
                c = new;
                fork begin @(c.v); $display("v at %0t", $time); end join_none
                #1 c.v = bi;
                #1;
            end
        endmodule
    )");
    CHECK(out == "v at 1\n");
}

// LRM 9.4.2's "any CHANGE in the value" doing observable work, once per type.
// The handle case is identity: re-assigning the SAME handle is not a change. The
// real case is the stored IEEE-754 pattern. The string case is the one that
// catches comparing POINTERS instead of content -- `t` is a different rt_string
// holding the same characters, so a handle compare would call it a change and
// wake at t=1 instead of t=4.
TEST_CASE("a scalar property store with no value change does not wake a non-edge waiter") {
    const std::string out = run_capture(R"(
        module m;
            class T; int x; endclass
            class C;
                string s = "same";
                real   r = 1.25;
                T      h;
            endclass
            C c; T k; string t;
            initial begin
                c = new; k = new; c.h = k; t = "same";
                fork begin @(c.s); $display("s at %0t", $time); end join_none
                fork begin @(c.r); $display("r at %0t", $time); end join_none
                fork begin @(c.h); $display("h at %0t", $time); end join_none
                #1 c.s = t;
                #1 c.r = 1.25;
                #1 c.h = k;
                #1 c.s = "other"; c.r = 9.5; c.h = null;
                #1;
            end
        endmodule
    )");
    // Nothing at t=1, 2 or 3: none of those stores changed a value.
    CHECK(out == "s at 4\nr at 4\nh at 4\n");
}

// An unpacked struct is what is left unsupported after this rung: comparing one
// needs a walk over its leaves rather than a word compare. `@` on the whole
// property is refused by the LANGUAGE -- an event expression must be integral or a
// named event (LRM 9.4.2), which slang enforces -- so this is pinned to the
// diagnostic a user actually sees, not to a clotho message. `wait` over one of its
// leaves is refused by clotho instead, with "no change notifier"
// (tests/wait_test.cpp), because the level path is where the silent park lived.
TEST_CASE("an event control on an aggregate class property is rejected") {
    const std::string err =
      reject("module m; typedef struct { int a; int b; } pr_t; class C; pr_t p; "
             "task w(); @p; $display(\"x\"); endtask endclass "
             "C c; initial begin c = new; c.w(); end endmodule");
    CHECK(err.find("invalid type 'pr_t' for event expression") != std::string::npos);
}

// An EDGE on any of these types is meaningless and the LANGUAGE says so, not just
// clotho: LRM 9.4.2 with Table 9-2 defines posedge / negedge as the least
// significant bit of the expression moving through the 0 < x/z < 1 lattice, so an
// edge-qualified event expression must be integral. Bit 0 of a heap pointer, of an
// IEEE-754 significand or of a string's first character is not a logic level. slang
// enforces exactly that, so the diagnostic is a type error and arrives before
// lowering; clotho keeps a backstop only to keep the message accurate if it ever
// becomes reachable. Pinned here so that a future relaxation cannot silently invent
// an edge for these types.
TEST_CASE("an edge event control on a non-integral class property is rejected") {
    const char* decls[] = { "real r; task w(); @(posedge r); endtask",
                            "shortreal q; task w(); @(posedge q); endtask",
                            "string s; task w(); @(edge s); endtask",
                            "chandle ch; task w(); @(negedge ch); endtask" };
    for (const char* d : decls) {
        const std::string err =
          reject(std::string("module m; class C; ") + d +
                 " endclass C c; initial begin c = new; c.w(); end endmodule");
        CHECK(err.find("is not integral") != std::string::npos);
    }
}

// -- Event-typed class properties: the wait side (LRM 9.4.2 over 15.5.1) --------
//
// An event property needs no change notifier: the property IS an object with a
// waiter list, the very one `-> e` fires. What was missing was the RESOLUTION of
// a bare `@e` inside a method to `this`'s property: event_signal reported
// "reference to unknown signal", because a property is heap storage, not a
// signal. The wait now parks on the handle the property holds when the wait
// executes (event_wait_needs_handle), the path `@(obj.e)` always took.

// The easyUVM driver shape (driver.sv:64's record_tr): two virtual protected
// tasks forked from a virtual task, over a virtual interface for clocking; one
// triggers event properties between clock edges, the other waits on them bare.
// The wake TIMES are the assertion -- byte-identical to Verilator 5.050
// ("begin at 5\nend at 15\n").
TEST_CASE("an event property triggered in one method wakes a bare wait in a forked sibling") {
    const std::string out = run_capture(R"(
        interface input_if;
            logic clk;
        endinterface
        class driver;
            typedef virtual input_if input_vif;
            input_vif vif;
            event begin_record, end_record;
            virtual task run_phase();
                fork
                    get_and_drive();
                    record_tr();
                join
            endtask
            virtual protected task get_and_drive();
                @(posedge vif.clk);
                -> begin_record;
                @(posedge vif.clk);
                -> end_record;
            endtask
            virtual protected task record_tr();
                @(begin_record);
                $display("begin at %0t", $time);
                @(end_record);
                $display("end at %0t", $time);
            endtask
        endclass
        module top;
            input_if intf();
            driver d;
            initial begin
                intf.clk = 0;
                forever #5 intf.clk = ~intf.clk;
            end
            initial begin
                d = new;
                d.vif = intf;
                d.run_phase();
                $finish;
            end
        endmodule
    )");
    CHECK(out == "begin at 5\nend at 15\n");
}

// Every spelling of the same wait: bare `@e`, parenthesized `@(e)`, an explicit
// `@(this.e)`, another object's property `@(o.e)` from inside a method, and an
// element of an event ARRAY property. All park on the same object `-> e` fires.
TEST_CASE("bare, parenthesized, this-qualified and other-object event property waits all wake") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                event e;
                event arr[2];
                task w_bare(); @e; $display("bare woke at %0t", $time); endtask
                task w_paren(); @(e); $display("paren woke at %0t", $time); endtask
                task w_this(); @(this.e); $display("this woke at %0t", $time); endtask
                task w_other(C o); @(o.e); $display("other woke at %0t", $time); endtask
                task w_arr(int i); @(arr[i]); $display("arr woke at %0t", $time); endtask
                task t_arr(int i); #2 -> arr[i]; endtask
            endclass
            C c, d;
            initial begin
                c = new; d = new;
                fork c.w_bare(); join_none
                fork c.w_paren(); join_none
                fork c.w_this(); join_none
                fork d.w_other(c); join_none
                fork c.w_arr(1); join_none
                c.t_arr(1);
                #3 -> c.e;
                #1;
            end
        endmodule
    )");
    CHECK(out.find("arr woke at 2\n") != std::string::npos);
    CHECK(out.find("bare woke at 5\n") != std::string::npos);
    CHECK(out.find("paren woke at 5\n") != std::string::npos);
    CHECK(out.find("this woke at 5\n") != std::string::npos);
    CHECK(out.find("other woke at 5\n") != std::string::npos);
}

// `->>` triggers in the NBA region of the same time step (LRM 15.5.1), so the
// waiter still wakes in that step. A queue element holding the property's
// object (push_back copies the handle, LRM 15.5.5) parks on the same object.
TEST_CASE("a nonblocking trigger wakes a bare event property wait in the same step") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                event e;
                event q[$];
                function new(); q.push_back(e); endfunction
                task w(); @e; $display("woke at %0t", $time); endtask
                task wq(); @(q[0]); $display("q woke at %0t", $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w(); join_none
                fork c.wq(); join_none
                #5 ->> c.e;
                #1;
            end
        endmodule
    )");
    CHECK(out.find("woke at 5\n") != std::string::npos);
    CHECK(out.find("q woke at 5\n") != std::string::npos);
}

// The negative space: a trigger of a DIFFERENT event property of the same
// object, and of the SAME-NAMED property of a different object, must wake
// nothing -- each property is its own object with its own waiter list.
TEST_CASE("an event property waiter ignores other properties and other objects") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                event e, f;
                task w(); @e; $display("woke at %0t", $time); endtask
            endclass
            C c, d;
            initial begin
                c = new; d = new;
                fork c.w(); join_none
                #1 -> c.f;
                #1 -> d.e;
                #1 -> c.e;
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 3\n");
}

// One trigger wakes every waiter parked on that property's object. Wake ORDER
// within an instant is unspecified (LRM 4.7), so the set and time are asserted.
TEST_CASE("two waiters on one event property both wake") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                event e;
                task w(string n); @e; $display("%s woke at %0t", n, $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w("p"); join_none
                fork c.w("q"); join_none
                #1 -> c.e;
                #1;
            end
        endmodule
    )");
    CHECK(out.find("p woke at 1\n") != std::string::npos);
    CHECK(out.find("q woke at 1\n") != std::string::npos);
    CHECK(out.size() == std::string_view("p woke at 1\nq woke at 1\n").size());
}

// An inherited event property: the BASE method triggers at the base-relative
// field index, the DERIVED method waits -- the flattened-field-table prefix
// invariant (a base's table is a prefix of every derived class's, see
// docs/design/classes.md) is what makes both address the same slot.
TEST_CASE("an inherited event property triggered by a base method wakes a derived waiter") {
    const std::string out = run_capture(R"(
        module m;
            class B;
                event go;
                task fire(); #2 -> go; endtask
            endclass
            class D extends B;
                int pad;
                task w(); @go; $display("woke at %0t", $time); endtask
            endclass
            D d;
            initial begin
                d = new;
                fork d.w(); join_none
                d.fire();
                #1;
            end
        endmodule
    )");
    CHECK(out == "woke at 2\n");
}

// `wait(e.triggered)` on an event property (LRM 15.5.3 under 9.4.3): the wait
// parks on the event OBJECT -- the only thing whose trigger changes the
// condition -- and the persistent within-timestep `triggered` state satisfies a
// wait that starts AFTER the trigger in the same instant, which `@e` would miss.
TEST_CASE("wait on an event property's triggered state parks on the object") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                event e;
                task w(string n); wait(e.triggered); $display("%s woke at %0t", n, $time); endtask
            endclass
            C c;
            initial begin
                c = new;
                fork c.w("early"); join_none
                #5 -> c.e;
                #1;
                -> c.e;
                c.w("late");
                #1;
            end
        endmodule
    )");
    CHECK(out == "early woke at 5\nlate woke at 6\n");
}

// A wait on a member of a virtual interface NO instance of which exists is the
// located LRM 25.9 run-time fatal the access paths already emit -- not a
// compile-time refusal of the whole method. The old path left the event list
// empty and refused with an internal invariant's wording ("intra-body event
// wait has no signal events").
TEST_CASE("an event wait through a never-bound virtual interface is a located run-time fatal") {
    constexpr std::string_view src = R"(
        interface never_if; logic clk; endinterface
        module m;
            class C;
                virtual never_if vif;
                task w(); @(posedge vif.clk); $display("UNREACHABLE"); endtask
            endclass
            C c;
            initial begin c = new; c.w(); end
        endmodule
    )";
    const LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    const test::DesignRun run = test::run_design_captured(src);
    CHECK(run.out.find("UNREACHABLE") == std::string::npos);
    CHECK(run.err.find("use of a null virtual interface") != std::string::npos);
}

namespace {

// Run `source` under the default lowering and under each gate that lowers an always process
// differently: -O, no thunks, no fusion.
void check_every_config(std::string_view source, const std::string& expected) {
    CHECK(run_capture(source) == expected);
    CHECK(run_capture(source, { .o3 = true }) == expected);
    CHECK(run_capture(source, { .thunk = false }) == expected);
    CHECK(run_capture(source, { .fuse = false }) == expected);
}

} // namespace

// IEEE 1800 9.4.2: a process waits only at the event control it has reached, so `always
// @(posedge clk) begin @(posedge done); ... end` is `forever begin @(posedge clk); @(posedge
// done); ... end` and the posedge of clk at t=3 below arrives while the process waits for done and
// wakes nothing. The leading control used to be a static subscription (docs/design/
// scheduler-rules.md S1) whose entry stayed live at the inner wait: it found the wake word armed
// there and resumed the process at t=3, and the entry the wait had left in done's bucket then
// resumed the next activation at its top at t=4 (the second "enter" below).
// Oracle: Icarus Verilog 13 (iverilog -g2012, vvp -n) prints "1 enter" and "4 woke n=1"; the same
// design without the $display of "enter" printed "4 woke n=1" there too.
TEST_CASE("an always parked at an intra-body @ is not woken by its own leading control") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0, done = 0; integer n = 0;
          always @(posedge clk) begin
            $display("%0t enter", $time);
            @(posedge done);
            n = n + 1; $display("%0t woke n=%0d", $time, n);
          end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(src, "1 enter\n4 woke n=1\n");
}

// The same wake delivered to a wait that never completes: the wake at t=3 left the entry the wait
// registered at t=1 in done's bucket, the activation at t=5 registered the process a second time
// in a bucket sized for one, and the store landed past its end (the 40-cycle form below ended in
// "simulation did not converge" at t=13, the 5-cycle form printed "n=2"). That run ends the
// process, so the design runs in a process of its own.
// Oracle: Icarus Verilog 13 prints "n=0".
TEST_CASE("a wait that never completes is never passed by its always's own clock") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0, done = 0; integer n = 0;
          always @(posedge clk) begin @(posedge done); n = n + 1; end
          initial begin
            repeat (40) begin #1 clk = 1; #1 clk = 0; end
            $display("n=%0d", n); $finish;
          end
        endmodule
    )";
    CHECK(test::run_design_subprocess(src) == "n=0\n");
}

// An intra-body control on the leading signal with the other edge: the negedge at t=2 and t=4 woke
// the process parked at `@(posedge clk)` through the any-change entry.
// Oracle: Icarus Verilog 13 prints "3 pos" and "5 pos".
TEST_CASE("an inner @(posedge clk) under an always @(clk) is not woken by a negedge") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0;
          always @(clk) begin @(posedge clk); $display("%0t pos", $time); end
          initial begin repeat (6) #1 clk = ~clk; #1 $finish; end
        endmodule
    )";
    check_every_config(src, "3 pos\n5 pos\n");
}

// `wait (c)` is a loop around an event control that re-tests c on every wake, so a wake from the
// leading control re-evaluated c at t=3; c has a side effect to make the evaluations visible.
// Oracle: Icarus Verilog 13 prints "1 eval d=0", "4 eval d=1" and "4 passed".
TEST_CASE("a wait statement under an always is evaluated only when its own operands change") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0, done = 0;
          function automatic bit ready(input bit d);
            $display("%0t eval d=%0d", $time, d);
            return d;
          endfunction
          always @(posedge clk) begin wait (ready(done)); $display("%0t passed", $time); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(src, "1 eval d=0\n4 eval d=1\n4 passed\n");
}

// The parks that keep the leading control's static subscription: a `#delay` holds the wake word
// set across the park, and a task that waits parks on a wake word of its own.
// Oracle: Icarus Verilog 13 prints "4 delayed" and "8 delayed" for the first design, "4 woke n=1"
// for the second.
TEST_CASE("an always parked at a delay or in a task is not woken by its own leading control") {
    constexpr std::string_view delayed = R"(
        module t;
          reg clk = 0;
          always @(posedge clk) begin #3 $display("%0t delayed", $time); end
          initial begin repeat (8) #1 clk = ~clk; #4 $finish; end
        endmodule
    )";
    check_every_config(delayed, "4 delayed\n8 delayed\n");

    constexpr std::string_view in_task = R"(
        module t;
          reg clk = 0, done = 0; integer n = 0;
          task wait_done; @(posedge done); endtask
          always @(posedge clk) begin wait_done; n = n + 1; $display("%0t woke n=%0d", $time, n); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(in_task, "4 woke n=1\n");
}

// A named event waits on its object's list, not in a signal bucket, and so does an `always @(e)`:
// its persistent node was the entry that woke the process at the inner wait.
// Oracle: Icarus Verilog 13 prints "4 woke n=1" for both designs.
TEST_CASE("an inner @e and a leading @(e) under an always do not wake the process at another control") {
    constexpr std::string_view inner_event = R"(
        module t;
          reg clk = 0; event ev; integer n = 0;
          always @(posedge clk) begin @ev; n = n + 1; $display("%0t woke n=%0d", $time, n); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 -> ev; #5 $finish; end
        endmodule
    )";
    check_every_config(inner_event, "4 woke n=1\n");

    constexpr std::string_view leading_event = R"(
        module t;
          reg done = 0; event ev; integer n = 0;
          always @(ev) begin @(posedge done); n = n + 1; $display("%0t woke n=%0d", $time, n); end
          initial begin #1 -> ev; #2 -> ev; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(leading_event, "4 woke n=1\n");
}

// Several always blocks on one leading list: the two that only compute fuse into one thunk group
// (and run on every posedge of clk), while each of the other two parks at a control of its own and
// must not be woken by the group's subscription.
// Oracle: Icarus Verilog 13 prints "4 b", "7 d" and "na=3 nb=1 nc=3 nd=1".
TEST_CASE("always blocks sharing a leading list: only the one at its own control waits there") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0, done = 0; integer na = 0, nb = 0, nc = 0, nd = 0;
          always @(posedge clk) na = na + 1;
          always @(posedge clk) begin @(posedge done); nb = nb + 1; $display("%0t b", $time); end
          always @(posedge clk) nc = nc + 1;
          always @(posedge clk) begin @(negedge done); nd = nd + 1; $display("%0t d", $time); end
          initial begin
            #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #1 clk = 0; #1 clk = 1; #1 done = 0;
            #2 $display("na=%0d nb=%0d nc=%0d nd=%0d", na, nb, nc, nd); $finish;
          end
        endmodule
    )";
    check_every_config(src, "4 b\n7 d\nna=3 nb=1 nc=3 nd=1\n");
}

// The other forms of a leading list and of an intra-body park: an inferred `@*` list, a list of
// several events (one park must leave no entry in the buckets of the others), a control inside an
// intra-assignment, a counted `repeat (2) @(...)`, and a fork whose child waits.
// Oracle: Icarus Verilog 13 prints "4 y=1", "4 woke a=0 b=1", "4 x=1", "7 two ticks" and "4 joined".
TEST_CASE("every form of leading list ignores its events while the body is parked") {
    constexpr std::string_view star = R"(
        module t;
          reg a = 0, done = 0, y = 0;
          always @* begin @(posedge done); y = a; $display("%0t y=%0d", $time, y); end
          initial begin #1 a = 1; #1 a = 0; #1 a = 1; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(star, "4 y=1\n");

    constexpr std::string_view several = R"(
        module t;
          reg a = 0, b = 0, done = 0;
          always @(a or b) begin @(posedge done); $display("%0t woke a=%0d b=%0d", $time, a, b); end
          initial begin #1 a = 1; #1 b = 1; #1 a = 0; #1 done = 1; #1 b = 0; #5 $finish; end
        endmodule
    )";
    check_every_config(several, "4 woke a=0 b=1\n");

    constexpr std::string_view intra = R"(
        module t;
          reg clk = 0, done = 0; integer x = 0;
          always @(posedge clk) begin x = @(posedge done) x + 1; $display("%0t x=%0d", $time, x); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(intra, "4 x=1\n");

    constexpr std::string_view counted = R"(
        module t;
          reg clk = 0, tick = 0;
          always @(posedge clk) begin repeat (2) @(posedge tick); $display("%0t two ticks", $time); end
          initial begin
            #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 tick = 1; #1 tick = 0; #1 clk = 0; #1 tick = 1;
            #5 $finish;
          end
        endmodule
    )";
    check_every_config(counted, "7 two ticks\n");

    constexpr std::string_view forked = R"(
        module t;
          reg clk = 0, done = 0;
          always @(posedge clk) begin fork @(posedge done); #1; join $display("%0t joined", $time); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(forked, "4 joined\n");
}

// `iff` (9.4.2.3) gates the leading event. Icarus Verilog 13 has no `iff`, so the expected text is
// derived from 9.4.2 as the code cites it (the clause text was not read: no copy of the LRM was
// available): the posedge at t=3 arrives while the process waits for done.
TEST_CASE("an always with an iff guard parked at an intra-body @ ignores its guarded event") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0, en = 1, done = 0;
          always @(posedge clk iff en) begin @(posedge done); $display("%0t woke", $time); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 done = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(src, "4 woke\n");
}

// `wait fork` parks the process in a list of its own; a wake from the leading control is one the
// barrier would absorb by testing again, which is why this shape printed the right text before.
// Oracle: Icarus Verilog 13 prints "4 joined n=1" and "8 joined n=2".
TEST_CASE("wait fork under an always is not completed by the leading control") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0; integer n = 0;
          always @(posedge clk) begin
            fork #3 n = n + 1; join_none
            wait fork;
            $display("%0t joined n=%0d", $time, n);
          end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 clk = 0; #1 clk = 1; #6 $finish; end
        endmodule
    )";
    check_every_config(src, "4 joined n=1\n8 joined n=2\n");
}

// The built-in blocking methods park in queues of the object they belong to: `semaphore::get`
// (15.3), `mailbox::get` (15.4) and `process::await` (9.7). Icarus Verilog 13 has none of these
// classes, so the expected text is derived from the clauses as the code cites them (not read: no
// copy of the LRM was available): each always waits for the key, the message or the process, and
// the posedge of clk at t=3 arrives while it waits.
TEST_CASE("a semaphore, a mailbox and an await under an always are not completed by the leading control") {
    constexpr std::string_view sem = R"(
        module t;
          reg clk = 0; semaphore s = new(0);
          always @(posedge clk) begin s.get(1); $display("%0t got", $time); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 s.put(1); #5 $finish; end
        endmodule
    )";
    check_every_config(sem, "4 got\n");

    constexpr std::string_view mbox = R"(
        module t;
          reg clk = 0; mailbox #(int) mb = new; int v;
          always @(posedge clk) begin mb.get(v); $display("%0t got %0d", $time, v); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 mb.put(7); #5 $finish; end
        endmodule
    )";
    check_every_config(mbox, "4 got 7\n");

    constexpr std::string_view await = R"(
        module t;
          reg clk = 0; process p;
          initial begin p = process::self(); #4; end
          always @(posedge clk) begin p.await(); $display("%0t awaited", $time); end
          initial begin #1 clk = 1; #1 clk = 0; #1 clk = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(await, "4 awaited\n");
}

// `expect` (16.17) blocks the process until its property has a verdict, and parks it in a queue
// the monitor drains in the Reactive region. A wake from the leading control resumed the process
// at t=3 with no verdict, so "after" printed before the property was decided. Icarus Verilog 13
// has no `expect`, so the expected text is derived from the clause as the code cites it (not
// read: no copy of the LRM was available). The property passes at the clk posedge at t=5, where
// a=1, the action block runs, and the statement after the expect follows it.
TEST_CASE("an expect under an always is not resumed by the leading control") {
    constexpr std::string_view src = R"(
        module t;
          reg clk = 0, go = 0, a = 0;
          always @(posedge go) begin
            expect (@(posedge clk) a) $display("%0t pass", $time); else $display("%0t fail", $time);
            $display("%0t after", $time);
          end
          initial begin #1 go = 1; #1 go = 0; #1 go = 1; #1 a = 1; #1 clk = 1; #5 $finish; end
        endmodule
    )";
    check_every_config(src, "5 pass\n5 after\n");
}

// The one shape that would need a statically subscribed process to wait at a control its leading
// list does not name -- a task with an event control called from an always_comb or always_ff --
// is not legal (13.3, 9.2.2.2, 9.2.2.4), so no process of it exists to be woken wrongly. Icarus
// Verilog 13 rejects it too.
TEST_CASE("a task that waits cannot be called from always_comb or always_ff") {
    const std::string comb = reject(R"(
        module t;
          logic a = 0, b, done = 0;
          task automatic wait_done; @(posedge done); endtask
          always_comb begin wait_done(); b = a; end
        endmodule
    )");
    CHECK(comb.find("blocking timing controls") != std::string::npos);

    const std::string ff = reject(R"(
        module t;
          logic clk = 0, done = 0; integer n = 0;
          task automatic wait_done; @(posedge done); endtask
          always_ff @(posedge clk) begin wait_done(); n <= n + 1; end
        endmodule
    )");
    CHECK(ff.find("blocking timing controls") != std::string::npos);
}

// An implicit event control `@*` before a statement of a procedural body (LRM
// 9.4.2.2): wait for a change in any net or variable that statement reads, so a
// statement that reads nothing gives an empty list and the process never wakes (the
// LRM names no other outcome; iverilog 13 warns "@* found no sensitivities so it
// will never trigger"). Expected output from iverilog 13 on each of these four.
TEST_CASE("implicit @* before a statement that reads nothing parks the process") {
    const std::string out = run_capture(R"(
        module top;
          initial begin
            $display("before");
            @* $display("never");
            $display("after");
          end
          initial #10 $display("done %0t", $time);
        endmodule
    )");
    CHECK(out == "before\ndone 10\n");
}

// The list is what the statement READS, and an identifier that only names the
// target of an assignment is left out: y is written by another process at 1 without
// waking the controlled statement, a changes at 2 and does.
TEST_CASE("implicit @* waits on the operands of the statement it controls") {
    const std::string out = run_capture(R"(
        module top;
          logic [3:0] a, b, y;
          initial begin
            a = 1; b = 2;
            @* y = a + b;
            $display("woke %0t y=%0d", $time, y);
          end
          initial begin #1 y = 9; #1 a = 5; end
        endmodule
    )");
    CHECK(out == "woke 2 y=7\n");
}

// LRM 9.4.2.2 example 3: `@* begin @(i) kid = b; end` is `@(b)`: an identifier that
// appears only in a timing control inside the statement is not added.
TEST_CASE("implicit @* leaves out an identifier that only appears in a timing control") {
    const std::string out = run_capture(R"(
        module top;
          logic i = 0, b = 0, kid = 0;
          initial begin
            @* begin @(i) kid = b; end
            $display("kid=%b %0t", kid, $time);
          end
          initial begin #1 i = 1; #1 b = 1; #1 i = 0; #1 i = 1; end
        endmodule
    )");
    CHECK(out == "kid=1 3\n");
}

// LRM 9.4.2.2 example 4: the outer `@*` of the always block is `@(a or b or c or d)`
// and the inner one, over `x = c ^ d`, is `@(c or d)`.
TEST_CASE("implicit @* nested in an always @*") {
    const std::string out = run_capture(R"(
        module top;
          logic a = 0, b = 0, c = 0, d = 0, x;
          always @* begin
            x = a ^ b; $display("outer %0t %b", $time, x);
            @* x = c ^ d; $display("inner %0t %b", $time, x);
          end
          initial begin #1 a = 1; #1 c = 1; #1 b = 1; #1 $finish; end
        endmodule
    )");
    CHECK(out == "outer 1 1\ninner 2 1\nouter 3 0\n");
}

// Inferred sensitivity through a handle (LRM 9.4.2.2, 9.2.2.2.1). `always @*` and an
// `@*` before a statement read every variable the statement reads, so a class property
// or a virtual-interface member read through a handle wakes on a change of the member
// and on a change of the handle (re-armed on the object the handle then names).
// `always_comb` adds the same for a virtual-interface member, but "references to class
// objects ... do not add anything to the sensitivity list of an always_comb", so a
// class property read adds neither member nor handle. No oracle parses a class or a
// virtual interface here; the expectations are the clauses'. Each `W` is a run of the body.
TEST_CASE("always @* wakes on a class property and on its handle") {
    const std::string out = run_capture(R"(
        class C; int x; endclass
        module top;
          C obj = new, other = new; int y;
          always @* begin $display("W %0t", $time); y = obj.x; end
          initial begin #1 obj.x = 5; #1 obj = other; #1 other.x = 7; #1 $finish; end
        endmodule
    )");
    CHECK(out == "W 1\nW 2\nW 3\n");
}

TEST_CASE("always @* wakes on a virtual-interface member and on its handle") {
    const std::string out = run_capture(R"(
        interface ifc; logic a = 0; endinterface
        module top;
          ifc i1(), i2(); virtual ifc vif = i1; logic y;
          always @* begin $display("W %0t", $time); y = vif.a; end
          initial begin #1 i1.a = 1; #1 vif = i2; #1 i2.a = 1; #1 $finish; end
        endmodule
    )");
    CHECK(out == "W 1\nW 2\nW 3\n");
}

TEST_CASE("implicit @* before a statement wakes on a class property and on its handle") {
    const std::string out = run_capture(R"(
        class C; int x; endclass
        module top;
          C obj = new, other = new; int y;
          initial forever @* begin $display("W %0t", $time); y = obj.x; end
          initial begin #1 obj.x = 5; #1 obj = other; #1 other.x = 7; #1 $finish; end
        endmodule
    )");
    CHECK(out == "W 1\nW 2\nW 3\n");
}

TEST_CASE("implicit @* before a statement wakes on a virtual-interface member and on its handle") {
    const std::string out = run_capture(R"(
        interface ifc; logic a = 0; endinterface
        module top;
          ifc i1(), i2(); virtual ifc vif = i1; logic y;
          initial forever @* begin $display("W %0t", $time); y = vif.a; end
          initial begin #1 i1.a = 1; #1 vif = i2; #1 i2.a = 1; #1 $finish; end
        endmodule
    )");
    CHECK(out == "W 1\nW 2\nW 3\n");
}

TEST_CASE("always_comb wakes on a virtual-interface member and on its handle") {
    const std::string out = run_capture(R"(
        interface ifc; logic a = 0; endinterface
        module top;
          ifc i1(), i2(); virtual ifc vif = i1; logic y;
          always_comb begin $display("W %0t", $time); y = vif.a; end
          initial begin #1 i1.a = 1; #1 vif = i2; #1 i2.a = 1; #1 $finish; end
        endmodule
    )");
    CHECK(out == "W 0\nW 1\nW 2\nW 3\n");
}

TEST_CASE("always_comb adds nothing for a class property, handle included") {
    const std::string out = run_capture(R"(
        class C; int x; endclass
        module top;
          C obj = new, other = new; int y;
          always_comb begin $display("W %0t", $time); y = obj.x; end
          initial begin #1 obj.x = 5; #1 obj = other; #1 other.x = 7; #1 $finish; end
        endmodule
    )");
    CHECK(out == "W 0\n");
}

// The time-zero run of an `always_comb` that reads a virtual-interface member is the
// one LRM 9.2.2.2 fixes (LRM_NOTES 44), after every initial and always has run to its
// first suspension -- not in the Inactive region a `#0` stands for. So an initial sees
// the declaration value (x) before it and the settled one after `#0` and after an
// `@(y)` armed at time zero, exactly as with an always_comb over a plain signal.
TEST_CASE("always_comb over a virtual-interface member starts as a plain one does") {
    const auto design = [](std::string_view decls, std::string_view read) {
        return std::string("interface ifc; logic a = 1; endinterface\nmodule top;\n") + std::string(decls) +
               "\n  always_comb y = " + std::string(read) +
               ";\n  initial begin $display(\"first %b\", y); #0 $display(\"after #0 %b\", y); end\n"
               "  initial begin @(y) $display(\"event %b at %0t\", y, $time); end\n"
               "  initial begin @(y) #0 $display(\"event #0 %b\", y); end\n"
               "endmodule\n";
    };
    const std::string vif   = run_capture(design("ifc i(); virtual ifc vif = i; logic y;", "vif.a"));
    const std::string plain = run_capture(design("logic a = 1; logic y;", "a"));
    CHECK(vif == plain);
    CHECK(vif == "first x\nevent 1 at 0\nafter #0 1\nevent #0 1\n");
}

// An unpacked struct or fixed-array class property is one variable (LRM 9.4.2.2): a change
// anywhere in it wakes `@*` and `@(obj.s.f)`, a store of the same value wakes nothing. No
// oracle parses classes or unpacked structs; the expectations are the clauses'.
TEST_CASE("always @* wakes on a change anywhere in an aggregate class property") {
    const std::string out = run_capture(R"(
        typedef struct { int f; int g; string t; } s_t;
        class C; s_t s; int a[2]; endclass
        module top;
          C obj = new, o2 = new; int y;
          always @* begin $display("W %0t", $time); y = obj.s.f + obj.a[1]; end
          initial begin
            #1 obj.s.f = 5; #1 obj.s.f = 5; #1 obj.s.g = 6; #1 obj.a[1] = 2;
            #1 obj.s.t = "x"; #1 obj.s = o2.s; #1 obj.a = o2.a; #1 $finish;
          end
        endmodule
    )");
    CHECK(out == "W 1\nW 3\nW 4\nW 5\nW 6\nW 7\n");
}

TEST_CASE("@(obj.s.f) wakes only when the member's value changes") {
    const std::string out = run_capture(R"(
        typedef struct { int f; int g; } s_t;
        class C; s_t s; endclass
        module top;
          C obj = new;
          initial begin @(obj.s.f) $display("woke %0t %0d", $time, obj.s.f); $finish; end
          initial begin #1 obj.s.g = 4; #1 obj.s.f = 7; end
        endmodule
    )");
    CHECK(out.find("woke 2 7") != std::string::npos);
}

// An event member of an aggregate class property is an event object like an event
// property: `@`, `wait(.triggered)` and `->` / `->>` reach it, in a module and in a method
// (implicit this), and a store to a sibling member still wakes `@*` and `wait`.
// Refused or silent before. LRM-derived; no oracle parses classes.
TEST_CASE("event member of an aggregate class property") {
    const std::string out = run_capture(R"(
        typedef struct { event e; int f; } s_t;
        class C; s_t s;
          task w(); @(s.e) $display("method"); endtask
          task w2(); wait (s.e.triggered) $display("trig"); endtask
        endclass
        module top;
          C obj = new; int y;
          initial begin @(obj.s.e) $display("at"); end
          initial begin wait (obj.s.f == 1) $display("wait f"); end
          initial begin @(obj.s.e) $display("at nb"); end
          initial fork obj.w(); obj.w2(); join_none
          always @* begin y = obj.s.f; $display("W %0t", $time); end
          initial begin #1 -> obj.s.e; #1 obj.s.f = 1; #1 ->> obj.s.e; #1 $finish; end
        endmodule
    )");
    for (const char* want : { "method", "trig", "at\n", "wait f", "W 2" }) {
        CHECK_MESSAGE(out.find(want) != std::string::npos, want);
    }
}

// An inferred `@*` waits before its body runs (LRM 9.4.2.2), so a null handle at the wait
// must not be dereferenced: it parks on the handle alone and subscribes to the property's
// notifier once the handle is set. LRM-derived; no oracle parses classes.
TEST_CASE("always @* over a property of a null handle waits on the handle") {
    const std::string out = run_capture(R"(
        typedef struct { event e; int f; } s_t;
        class C; s_t s; endclass
        module m;
          C obj; int y;
          always @* y = obj.s.f;
          initial begin
            obj = new;
            fork
              begin @(obj.s.e) $display("woke e at %0t", $time); end
              begin wait (obj.s.f == 1) $display("wait f at %0t", $time); end
            join_none
            #1 -> obj.s.e;
            #1 obj.s.f = 1;
            #1 $display("y=%0d at %0t", y, $time);
            $finish;
          end
        endmodule
    )");
    CHECK(out.find("woke e at 1") != std::string::npos);
    CHECK(out.find("wait f at 2") != std::string::npos);
    CHECK(out.find("y=1 at 3") != std::string::npos);
}

// An event list is the OR of its terms (LRM 9.4.2.1), and a term over a class property wakes
// it, in either position, on a change of the term's value only (9.4.2): a write to a sibling
// member and a write of the value it already holds are no event. A method names the property
// through the implicit this, and `always @(list)` re-arms on every term. Before: the property
// term never woke, and in a method the lowering crashed. No oracle parses classes or unpacked
// structs; the expectations are the clauses'.
TEST_CASE("an event list with a class property term wakes on each of its terms") {
    SUBCASE("a process, the term in either position") {
        const std::string out = run_capture(R"(
            typedef struct { int f; int g; } s_t;
            class C; s_t s; endclass
            module top;
              C obj = new;
              logic b = 0;
              string fb, bf;
              initial begin
                fork
                  forever begin @(obj.s.f or b) fb = {fb, $sformatf("%0t ", $time)}; end
                  forever begin @(b or obj.s.f) bf = {bf, $sformatf("%0t ", $time)}; end
                join_none
                #1 obj.s.g = 1;
                #1 obj.s.f = 5;
                #1 b = 1;
                #1 obj.s.f = 5;
                #1 b = 1;
                #1 obj.s.f = 6;
                #1 b = 0;
                #1 $display("fb: %s", fb);
                $display("bf: %s", bf);
              end
            endmodule
        )");
        CHECK(out == "fb: 2 3 6 7 \nbf: 2 3 6 7 \n");
    }
    SUBCASE("a method") {
        const std::string out = run_capture(R"(
            typedef struct { int f; } s_t;
            class C;
              s_t s; logic flag;
              task automatic watch(int id);
                @(s.f or flag) $display("w%0d at %0t", id, $time);
              endtask
            endclass
            module top;
              C obj = new;
              initial begin
                fork obj.watch(1); join_none
                #1 obj.flag = 1;
                fork obj.watch(2); join_none
                #1 obj.s.f = 3;
                #1 $finish;
              end
            endmodule
        )");
        CHECK(out == "w1 at 1\nw2 at 2\n");
    }
    SUBCASE("a process's sensitivity list") {
        const std::string out = run_capture(R"(
            typedef struct { int f; } s_t;
            class C; s_t s; endclass
            module top;
              C obj = new;
              logic b = 0;
              int n = 0;
              always @(obj.s.f or b) n++;
              initial begin
                #1 obj.s.f = 1;
                #1 b = 1;
                #1 obj.s.f = 1;
                #1 b = 1;
                #1 $display("n=%0d", n);
              end
            endmodule
        )");
        CHECK(out == "n=2\n");
    }
}

// An edge on a term is the edge of its least significant bit (Table 9-2), here through an
// index that is itself a variable the wait watches, and `iff` gates its own term, evaluated
// when the term changes (9.4.2.3: "evaluated when a changes and not when enable changes").
// The plain-signal analog of the edge design gave `1 3 4 9` on iverilog 13; the `iff` list
// is LRM-derived, iverilog does not parse it. Before: the first never woke on the property,
// and the second was refused (iff on a multi-event control).
TEST_CASE("an event list over class property terms follows Table 9-2 and iff") {
    SUBCASE("edges") {
        const std::string out = run_capture(R"(
            class C; logic [1:0] a [2]; endclass
            module top;
              C obj = new;
              int i = 1;
              logic c = 1;
              string log;
              initial begin
                obj.a[0] = 2'b00; obj.a[1] = 2'b00;
                fork
                  forever begin @(posedge obj.a[i] or negedge c) log = {log, $sformatf("%0t ", $time)}; end
                join_none
                #1 obj.a[1] = 2'b01;
                #1 obj.a[1] = 2'b10;
                #1 obj.a[1] = 2'b11;
                #1 c = 0;
                #1 c = 1;
                #1 obj.a[0] = 2'b01;
                #1 i = 0;
                #1 obj.a[0] = 2'b00;
                #1 obj.a[0] = 2'b01;
                #1 $display("%s", log);
              end
            endmodule
        )");
        CHECK(out == "1 3 4 9 \n");
    }
    SUBCASE("iff on each term") {
        const std::string out = run_capture(R"(
            typedef struct { int f; } s_t;
            class C; s_t s; endclass
            module top;
              C obj = new;
              logic b = 0, en = 0, en2 = 0;
              string log;
              initial begin
                fork
                  forever begin @(obj.s.f iff en or posedge b iff en2) log = {log, $sformatf("%0t ", $time)}; end
                join_none
                #1 obj.s.f = 1;
                #1 en = 1;
                #1 obj.s.f = 2;
                #1 b = 1;
                #1 b = 0;
                #1 en2 = 1;
                #1 b = 1;
                #1 obj.s.f = 2;
                #1 $display("%s", log);
              end
            endmodule
        )");
        CHECK(out == "3 7 \n");
    }
}

// A list mixes terms of every kind: a compound over a property, a real and a string property
// named by themselves (a real by its bit pattern, a string by its content, so a write of the
// same value is no event), and a named event, which wakes it on a trigger in a later step
// too. The same terms in a task, whose samples live in its frame. Before: the compound
// never woke. LRM-derived; no oracle parses classes.
TEST_CASE("an event list with a real, a string and an event term") {
    SUBCASE("a process") {
        const std::string out = run_capture(R"(
            typedef struct { int f; } s_t;
            class C; s_t s; real r; string t; endclass
            module top;
              C obj = new;
              event ev;
              string log;
              initial begin
                obj.r = 1.0; obj.t = "a";
                fork
                  forever begin @(obj.s.f or obj.r or obj.t or ev) log = {log, $sformatf("%0t ", $time)}; end
                join_none
                #1 obj.s.f = 1;
                #1 obj.r = 1.0;
                #1 obj.r = 2.5;
                #1 obj.t = "b";
                #1 -> ev;
                #1 -> ev;
                #1 obj.t = "b";
                #1 $display("%s", log);
              end
            endmodule
        )");
        CHECK(out == "1 3 4 5 6 \n");
    }
    SUBCASE("a task") {
        const std::string out = run_capture(R"(
            typedef struct { int f; } s_t;
            class C;
              s_t s; real r; string t;
              task automatic watch(int id);
                @(s.f or r or t) $display("w%0d at %0t f=%0d r=%0f t=%s", id, $time, s.f, r, t);
              endtask
            endclass
            module top;
              C obj = new;
              initial begin
                obj.r = 1.0; obj.t = "a";
                fork obj.watch(1); join_none
                #1 obj.t = "a";
                #1 obj.t = "b";
                fork obj.watch(2); join_none
                #1 obj.r = 1.0;
                #1 obj.r = 3.5;
                fork obj.watch(3); join_none
                #1 obj.s.f = 9;
                #1 $finish;
              end
            endmodule
        )");
        CHECK(out == "w1 at 2 f=0 r=1.000000 t=b\nw2 at 4 f=0 r=3.500000 t=b\nw3 at 5 f=9 r=3.500000 t=b\n");
    }
}

// An explicit control evaluates its expression where the wait begins, so a null class handle
// in a term of a list, or in a lone compound term, is the located fatal of LRM 8.4 that
// `@(obj.x)` already gives, at the time the wait is reached (1 s), not at time zero. An
// inferred `@*` parks on the handle instead (LRM_NOTES 57, 58). Before: the list fataled at
// time zero, from the net its term was synthesized into, ahead of the first statement. Out of
// process because the report ends the run.
TEST_CASE("an event list or compound term over a property of a null handle is a located fatal") {
    for (const char* control : { "@(obj.s.f or b)", "@(b or obj.s.f)", "@(obj.s.f)" }) {
        const std::string src = std::string(R"(
            typedef struct { int f; } s_t;
            class C; s_t s; endclass
            module top;
              C obj; logic b;
              initial begin
                $display("before");
                #1 )") + control + R"(;
                $display("UNREACHABLE");
              end
            endmodule
        )";
        const test::DesignRun run = test::run_design_captured(src);
        INFO(control);
        CHECK(run.out == "before\n");
        CHECK(test::design_exit_code(run) != 0);
        CHECK(run.err.find("fatal: dereference of a null class handle") != std::string::npos);
        CHECK(run.err.find("the handle holds no object at 1 s") != std::string::npos);
    }
}
