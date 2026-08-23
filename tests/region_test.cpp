// Region guards: a fused group runs only the members whose own sensitivity
// fired, instead of every member on any union event
// (docs/design/regions.md, SimulationConfig::regions).
//
// What these pin, and why each one exists:
//
//   * The trigger bits are set where the EDGE IS CLASSIFIED -- at the store --
//     not re-derived at group entry from a sampled previous value. The
//     "glitch within one instant" case is what rules the sampling design out:
//     a signal driven 0->1->0 inside one instant raises a real event that a
//     before/after comparison cannot see.
//   * A guard must not break the producer-before-consumer settle INSIDE a
//     group. An earlier member's store hands the downstream member its bit
//     mid-pass, so the word is re-read per member; snapshotting it once at
//     group entry stranded every consumer fed from inside the group (this is
//     the bug the chain and diamond cases below catch).
//   * A guard can only ever REMOVE an execution the union subscription would
//     have performed, and a wake that cannot name its trigger sets every bit.
//     So values, activation counts and output are identical with the gate on
//     and off -- which is what check_regions asserts on every case here,
//     across the fused / unfused and thunk / coroutine lowerings too.
//
// Every expected string is oracle-verified against iverilog 13 (2026-08-23).

#include <cstdio>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/SimConfig.h"

using namespace clotho;

namespace {

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
    char   buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

// Assert the design prints `expected` guarded and unguarded, and under the
// lowerings the guard interacts with: the coroutine group path (`thunk` off)
// carries its own copy of the guard, and `fuse` off removes the groups
// entirely, which is the value the guard must agree with.
void check_regions(std::string_view source, const std::string& expected) {
    CHECK(run_capture(source) == expected);
    CHECK(run_capture(source, { .regions = false }) == expected);
    CHECK(run_capture(source, { .thunk = false }) == expected);
    CHECK(run_capture(source, { .thunk = false, .regions = false }) == expected);
    CHECK(run_capture(source, { .fuse = false, .thunk = false }) == expected);
}

std::string emit(std::string_view source, ir::SimulationConfig config = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    std::string        out;
    llvm::raw_string_ostream os(out);
    REQUIRE(ir::emit_llvm(*r.module, os, config));
    return out;
}

size_t count_substr(const std::string& hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

// A component whose members are all `interior` (every sensitivity signal is
// written by another candidate), so comb fusion merges them, with DIFFERENT
// sensitivities so the members can be told apart. `s1` keeps its external
// trigger and stays a singleton, which is what makes the rest interior.
constexpr const char* kChain = R"(
module top;
  reg  [7:0] a = 0;
  wire [7:0] s1, s2, s3, s4;
  assign s1 = a + 1;
  assign s2 = s1 + 2;
  assign s3 = s1 ^ s2;
  assign s4 = s3 + 3;
  initial begin
    #1 a = 8'h10;
    #1 $display("%h %h %h %h", s1, s2, s3, s4);
    #1 a = 8'h20;
    #1 $display("%h %h %h %h", s1, s2, s3, s4);
  end
endmodule
)";

} // namespace

TEST_CASE("regions: a consumer fed from inside its own group still settles") {
    // THE regression pin. s2 and s3/s4 are one guarded group; a change to s1
    // fires only s2's own bit, and s2's store is what hands s3 its bit, which
    // hands s4 its bit -- all inside one activation. A guard that reads the
    // wake word once at group entry sees only s1's bit and strands s3 and s4 at
    // their time-zero values.
    check_regions(kChain, "11 13 02 05\n21 23 02 05\n");
}

TEST_CASE("regions: only the branch whose input changed re-evaluates") {
    // Two independent branches off one shared producer. Touching x0 must leave
    // c1's value alone and still produce the right one -- the point of the
    // guard is that c1's body does not run, which is unobservable except by
    // its result being unchanged and correct.
    static const char* src = R"(
module top;
  reg  [7:0] x0 = 1, x1 = 2, s = 7;
  wire [7:0] sc, y0, y1, c0, c1;
  assign sc = s + 1;
  assign y0 = x0 + 1;
  assign y1 = x1 + 1;
  assign c0 = sc ^ y0;
  assign c1 = sc ^ y1;
  initial begin
    #1 $display("%h %h", c0, c1);
    x0 = 8'h10; #1 $display("%h %h", c0, c1);
    x1 = 8'h20; #1 $display("%h %h", c0, c1);
    s  = 8'h30; #1 $display("%h %h", c0, c1);
  end
endmodule
)";
    check_regions(src, "0a 0b\n19 0b\n19 29\n20 10\n");
}

TEST_CASE("regions: a signal stored twice in one instant keeps its trigger") {
    // Two stores to `a` with no delay between: the second wake finds the group
    // already queued, so it takes the already-queued arm of the scan, which must
    // still OR the trigger bits in. Drop that OR and the members wait for a bit
    // that never arrives and the chain never settles on the final value.
    //
    // Note what is NOT pinned here. Whether a signal that returns to its
    // starting value inside one instant (0 -> x -> 0) is observable at all is
    // active-region ordering: iverilog interleaves the continuous-assign
    // evaluation between the two blocking assignments and sees a transient,
    // clotho defers both and does not, and LRM 4.7 allows either. It is
    // unobservable for a PURE comb member by construction, which is part of why
    // guarding one is safe. It stops being unobservable when regions admits
    // edge-sensitive members (regions.md P3), and that is where the glitch pin
    // belongs -- a posedge member must fire for an edge the schedule classified,
    // even if the level is back where it started by the time the member runs.
    static const char* src = R"(
module top;
  reg  [7:0] a = 0;
  wire [7:0] s1, s2, s3, s4;
  assign s1 = a + 1;
  assign s2 = s1 + 2;
  assign s3 = s1 ^ s2;
  assign s4 = s3 + 3;
  initial begin
    a = 8'h10;
    a = 8'h20;
    #1 $display("%h %h %h %h", s1, s2, s3, s4);
  end
endmodule
)";
    check_regions(src, "21 23 02 05\n");
}

TEST_CASE("regions: unequal-depth reconvergence settles to final values") {
    // comb-fusion.md's diamond, with guards on: b is one assign deep, d is two,
    // and the join e must end up with both final inputs however the guards
    // order the members.
    static const char* src = R"(
module top;
  reg [7:0] a = 0;
  wire [7:0] b, c, d, e, f;
  assign b = a + 8'd1;
  assign c = a << 1;
  assign d = c + 8'd3;
  assign e = b ^ d;
  assign f = e + 8'd1;
  initial begin
    a = 8'd5; #1 $display("%h %h", e, f);
    a = 8'd200; #1 $display("%h %h", e, f);
  end
endmodule
)";
    check_regions(src, "0b 0c\n5a 5b\n");
}

TEST_CASE("regions: a component wider than the trigger colour space is correct") {
    // 63 usable colours; this component has more distinct events than that, so
    // compute_trigger_masks shares colours between events. Sharing costs
    // precision (a shared bit fires both members) and must cost nothing else.
    std::string src = "module top;\n  reg [7:0] a = 0;\n  wire [7:0] hub;\n  assign hub = a + 1;\n";
    for (int i = 0; i < 70; ++i) {
        src += "  wire [7:0] p" + std::to_string(i) + ", q" + std::to_string(i) + ";\n";
    }
    for (int i = 0; i < 70; ++i) {
        src += "  assign p" + std::to_string(i) + " = hub + 8'd" + std::to_string(i) + ";\n";
        src += "  assign q" + std::to_string(i) + " = p" + std::to_string(i) + " ^ 8'h5a;\n";
    }
    src += "  initial begin\n    #1 a = 8'h10;\n    #1 $display(\"%h %h %h %h\", p0, q0, p69, q69);\n  end\nendmodule\n";
    check_regions(src, "11 4b 56 0c\n");
}

TEST_CASE("regions: an event-triggered member runs on a wake it cannot colour") {
    // `-> ev` wakes through the event object's waiter list, which has no
    // (signal, edge) to colour, so that path sets every trigger bit. The member
    // sensitive to the event must still run.
    static const char* src = R"(
module top;
  event ev;
  reg  [7:0] a = 0;
  reg  [7:0] seen = 0;
  wire [7:0] s1, s2;
  assign s1 = a + 1;
  assign s2 = s1 + 2;
  always @(ev) seen = s2;
  initial begin
    #1 a = 8'h10;
    #1 -> ev;
    #1 $display("%h %h %h", s1, s2, seen);
  end
endmodule
)";
    check_regions(src, "11 13 13\n");
}

TEST_CASE("regions: guards are emitted for heterogeneous groups only") {
    // A fused group whose members share one sensitivity gains nothing from a
    // guard (every mask would be the union), so none is emitted; a group whose
    // members differ gets one per member. Pins the decision, not a value.
    static const char* same = R"(
module top;
  reg  [7:0] a = 0;
  wire [7:0] u, v, w;
  assign u = a + 1;
  assign v = u + 2;
  assign w = u + 3;
  initial begin #1 a = 1; #1 $display("%h %h %h", u, v, w); end
endmodule
)";
    // v and w both wake on u alone: one group, one sensitivity, no guards.
    CHECK(count_substr(emit(same), "mem.run") == 0);
    // The chain's members differ, so each gets a guard.
    CHECK(count_substr(emit(kChain), "mem.run") > 0);
    // And with the gate off, neither shape emits any.
    CHECK(count_substr(emit(kChain, ir::SimulationConfig{ .regions = false }), "mem.run") == 0);
    CHECK(count_substr(emit(same, ir::SimulationConfig{ .regions = false }), "mem.run") == 0);
}

TEST_CASE("regions: the wake word keeps its queued/not-queued discipline") {
    // The trigger bits ride above bit 0 of the same word the scheduled flag
    // always used, so a process woken by two of its signals in one instant must
    // still run ONCE. Counted directly.
    static const char* src = R"(
module top;
  reg  [7:0] a = 0, b = 0;
  wire [7:0] s1, s2;
  integer runs = 0;
  assign s1 = a + b;
  assign s2 = s1 + 1;
  always @(a or b) runs = runs + 1;
  initial begin
    #1 runs = 0;
    a = 1; b = 1;      // both change in one instant: one activation
    #1 $display("%0d %h %h", runs, s1, s2);
  end
endmodule
)";
    check_regions(src, "1 02 03\n");
}
