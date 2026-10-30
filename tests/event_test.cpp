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

// `->>` triggers in the NBA region of the same time step (LRM 15.5.2), so the
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
