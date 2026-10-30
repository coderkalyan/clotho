#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"

#include <llvm/Support/raw_ostream.h>

using namespace clotho;

namespace {

// JIT-run a design and return its stdout. The config lets each case pin
// behavior across lowerings (fused / unfused, thunk / coroutine) so the
// comb-fusion work cannot silently change any of them.
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

// Lower to LLVM IR text, for the cases that pin a FUSION DECISION rather than a
// value: a fused group of N > 1 members emits one `.fN` unit, so counting those
// symbols is how a test observes what compute_fusion_groups decided.
std::string emit(std::string_view source, ir::SimulationConfig config = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*r.module, os, config));
    return text;
}

// Count non-overlapping occurrences of `needle` in `hay`.
size_t count_substr(const std::string& hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at        = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
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

// A write reached only through a call is still a write. The pin below is on the
// FUSION DECISION rather than on a settled value, deliberately: see
// docs/design/fork-join.md §V5.8. `compute_fusion_groups`' interior guard
// independently prevents a hidden edge from ever closing a cycle INSIDE a group,
// so no value divergence is reachable today and manufacturing one would be a lie.
// What is reachable, and what this locks, is that the wake graph now SEES the edge.
TEST_CASE("comb fusion: a write reached only through a call joins the wake graph, "
          "so the loop stops fusing") {
    // A four-process combinational loop whose back edge hides inside a function:
    // `back()` blocking-writes x, so the true graph is
    //   (@w -> x) -> (@x -> y) -> (@y -> z) -> (@z -> back writes x) -> ...
    //
    // The shape is built specifically to defeat the interior guard, which is the
    // only reason the two middle processes are fusion candidates at all: x has a
    // SECOND, directly visible writer (`always @(w) x = w;`), so `cand_writes[x]`
    // is set and `always @(x) y = x;` counts as interior. Without the function's
    // edge, `@x -> y` and `@y -> z` look like an acyclic pair, Tarjan finds no
    // nontrivial SCC, and they fuse into one `.f2` unit. With it they are inside a
    // comb loop, `scc_nontrivial_` excludes them, and re-wake fixpoint iteration
    // handles the loop instead — which is what the unfused schedule always did.
    static const char* src = R"(
module top;
  reg [7:0] w = 0, x = 0, y = 0, z = 0;
  integer d = 0;
  function integer back(input [7:0] v);
    if (v < 8'd5) x = v + 8'd1;
    back = 0;
  endfunction
  always @(w) x = w;
  always @(x) y = x;
  always @(y) z = y;
  always @(z) d = back(z);
  initial begin
    #1 w = 8'd1;
    #20 $display("%0d %0d %0d %0d", w, x, y, z);
  end
endmodule
)";
    // No fused comb unit is emitted: a `.fN` symbol exists only for a group of
    // N > 1. Before the call edge reached `BodyFacts::calls` this design emitted
    // exactly one `.f2` (measured), so this is the assertion that flips.
    CHECK(count_substr(emit(src), ".f2.thunk(ptr noalias %state") == 0);
    CHECK(count_substr(emit(src, { .thunk = false }), ".f2(ptr %state") == 0);

    // And the loop still settles, identically under every lowering — excluding
    // the pair from fusion must not cost the fixpoint.
    check_all_lowerings(src, "1 5 5 5\n");
}

// LRM 9.2.2.2 (and 9.2.2.3 for always_latch): an always_comb "is automatically
// triggered once at time zero, after all initial and always procedures have been
// started". The time-zero run used to be queued in source order with everything else,
// so a comb written above an `initial` ran first. A disable of a block that has not
// been started is a no-op (9.6.2), so `disable blk` ran before `blk` existed and the
// block went on to print. Oracle: iverilog 13, which runs it last.
TEST_CASE("always_comb and always_latch run once at time zero after every initial has started") {
    SUBCASE("disable of a block of a later initial, always_comb") {
        check_all_lowerings(R"(
            module t;
                always_comb disable blk;
                initial begin : blk
                    #1 $display("FAILED: blk not disabled");
                end
                initial #2 $display("done");
            endmodule)",
                            "done\n");
    }
    SUBCASE("disable of a block of a later initial, always_latch") {
        check_all_lowerings(R"(
            module t;
                always_latch disable blk;
                initial begin : blk
                    #1 $display("FAILED: blk not disabled");
                end
                initial #2 $display("done");
            endmodule)",
                            "done\n");
    }
    SUBCASE("a comb that reads something runs after the initial that wrote it") {
        check_all_lowerings(R"(
            module t;
                logic a, y;
                always_comb y = a;
                initial a = 1'b1;
                initial #1 $display("y=%b", y);
            endmodule)",
                            "y=1\n");
    }
    SUBCASE("source order does not matter, in either direction") {
        const std::string comb_first = R"(
            module t;
                logic y;
                always_comb y = 1'b0;
                initial $display("y=%b", y);
            endmodule)";
        const std::string comb_last  = R"(
            module t;
                logic y;
                initial $display("y=%b", y);
                always_comb y = 1'b0;
            endmodule)";
        check_all_lowerings(comb_first, "y=x\n");
        check_all_lowerings(comb_last, "y=x\n");
    }
    SUBCASE("a continuous assignment keeps its place: it is not a procedure") {
        check_all_lowerings(R"(
            module t;
                logic y;
                assign y = 1'b1;
                initial #0 $display("y=%b", y);
            endmodule)",
                            "y=1\n");
    }
}
