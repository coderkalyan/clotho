#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a design and return its stdout. The config lets each case pin
// behavior across lowerings (fused / unfused, thunk / coroutine) so the
// comb-fusion work cannot silently change any of them.
std::string run_capture(std::string_view source, ir::SimulationConfig config = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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

    std::string out;
    std::rewind(tmp);
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

// Assert the design prints `expected` under every lowering the comb-fusion
// work interacts with.
void check_all_lowerings(std::string_view source, const std::string& expected) {
    CHECK(run_capture(source) == expected);
    CHECK(run_capture(source, { .thunk = false }) == expected);
    CHECK(run_capture(source, { .fuse = false, .thunk = false }) == expected);
}

} // namespace

// Semantic pins for comb-process fusion (see the comb-fusion design phases).
// Every expected string is oracle-verified against iverilog 13 (2026-07-08,
// byte-identical). These lock in the event-driven behavior that a fused
// static schedule must preserve; they run against every lowering config so a
// regression cannot hide behind a non-default flag.

TEST_CASE("comb fusion: diamond reconvergence settles to final values") {
    // Unequal-depth reconvergent paths (b is 1 assign deep, d is 2). Whether
    // the join glitches mid-instant is scheduler nondeterminism; the settled
    // value is not. A fused topo-order schedule evaluates the join once with
    // final inputs — same settled values.
    static const char* src = R"(
module top;
  reg [7:0] a = 0;
  wire [7:0] b, c, d, e;
  assign b = a + 8'd1;
  assign c = a << 1;
  assign d = c + 8'd3;
  assign e = b ^ d;
  initial begin
    a = 8'd5; #1 $display("%h", e);
    a = 8'd200; #1 $display("%h", e);
  end
endmodule
)";
    check_all_lowerings(src, "0b\n5a\n");
}

TEST_CASE("comb fusion: external observer wakes once per real change") {
    // A process outside any fusable group, sensitive to a mid-chain signal.
    // It must wake exactly once per value change of w2 — including under a
    // fused chain, whose internal stores still service external wait queues —
    // and must NOT wake for the no-change rewrite (a = 1 twice). Counted as a
    // delta from after time 0, so the pin is independent of the LRM-legal
    // time-zero ordering between the observer's first block and the
    // self-start assign evaluations.
    static const char* src = R"(
module top;
  reg [7:0] a = 0;
  wire [7:0] w1, w2, w3;
  assign w1 = a + 8'd1;
  assign w2 = w1 + 8'd1;
  assign w3 = w2 + 8'd1;
  integer cnt = 0, base;
  always @(w2) cnt = cnt + 1;
  initial begin
    #1 base = cnt;
    a = 8'd1; #1;
    a = 8'd1; #1;
    a = 8'd2; #1;
    $display("%0d %0d", cnt - base, w3);
  end
endmodule
)";
    check_all_lowerings(src, "2 5\n");
}

TEST_CASE("comb fusion: $display body keeps per-trigger activation count") {
    // An impure comb body (comb_fuse_verdict: ImpureOp) must never be fused:
    // its activation count is directly observable. One line per real change
    // of the sensitivity set, none for the no-change rewrite of a.
    static const char* src = R"(
module top;
  reg [3:0] a = 0, b = 0;
  wire [3:0] s;
  assign s = a + b;
  always @(a or b) $display("e %0d %0d", a, b);
  initial begin
    #1 a = 4'd1;
    #1 b = 4'd2;
    #1 a = 4'd1;
    #1 $display("s %0d", s);
  end
endmodule
)";
    check_all_lowerings(src, "e 1 0\ne 1 2\ns 3\n");
}

TEST_CASE("comb fusion: narrow explicit sensitivity keeps stale reads") {
    // `always @(a) c = b;` reads b outside its sensitivity (comb_fuse_verdict:
    // ReadOutsideSens — must never be fused). LRM 9.4.2: the process runs only
    // on a-changes, so c holds the b value sampled at the last a-change; a
    // spurious fused run would illegally observe the newer b.
    static const char* src = R"(
module top;
  reg a = 0, b = 0;
  reg c;
  always @(a) c = b;
  initial begin
    #1 b = 1;
    #1 a = 1;
    #1 $display("%b", c);
    #1 b = 0;
    #1 $display("%b", c);
  end
endmodule
)";
    check_all_lowerings(src, "1\n1\n");
}

TEST_CASE("comb fusion: combinational loop settles by re-wake iteration") {
    // q <-> g form an SCC (a latch): excluded from fusion (no topo order);
    // the unfused re-wake-on-change iteration must keep reaching the
    // fixpoint, including holding state after en drops.
    static const char* src = R"(
module top;
  reg en = 0, d = 0;
  wire q, g;
  assign g = en ? d : q;
  assign q = g;
  initial begin
    en = 1; d = 1; #1 $display("%b", q);
    en = 0; #1 d = 0; #1 $display("%b", q);
  end
endmodule
)";
    check_all_lowerings(src, "1\n1\n");
}

// NOTE: multiple continuous drivers on one net (resolution to x, LRM 6.6.1)
// are not a fusion hazard: each driver stages into its own distinct slot
// signal and a single WireRes resolver drives the net, so every process still
// has exactly one write target. No fused group can contain two drivers of the
// same storage — the shared-target hazard is structurally impossible.
