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

// Lower and return the rendered diagnostics; ok must be false (a rejected design).
std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostics;
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

TEST_CASE("event handle / event-controlled trigger forms are rejected") {
    CHECK(reject("module top; event e, f; initial e = f; endmodule").find(
            "event assignment") != std::string::npos);
    CHECK(reject("module top; event e; initial e = null; endmodule").find(
            "event assignment") != std::string::npos);
    CHECK(reject("module top; logic clk; event e; initial ->> @(posedge clk) e; endmodule").find(
            "event-controlled nonblocking trigger") != std::string::npos);
    CHECK(reject("module top; event e; input e; endmodule").find(
            "event") != std::string::npos);
}

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
