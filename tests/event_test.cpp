#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
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

// Tier-2 forms loud-fail rather than being silently mis-lowered.
TEST_CASE("event handle / nonblocking forms are rejected") {
    CHECK(reject("module top; event e, f; initial e = f; endmodule").find(
            "event assignment") != std::string::npos);
    CHECK(reject("module top; event e; initial e = null; endmodule").find(
            "event assignment") != std::string::npos);
    CHECK(reject("module top; event e; initial ->> e; endmodule").find(
            "nonblocking event trigger") != std::string::npos);
    CHECK(reject("module top; event e; input e; endmodule").find(
            "event") != std::string::npos);
}
