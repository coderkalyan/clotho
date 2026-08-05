// Concurrent assertions (LRM 16): the front-end's lowering to ir::AssertionMonitor.
//
// These check the tables clotho hands the backend, because that is the contract
// between the two halves and it is the part libsva's own tests cannot see. The
// temporal semantics themselves are already verified inside libsva by differential
// and mutation testing, and the automaton step's codegen has its own oracle; see
// docs/design/concurrent-assertions.md §11.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "ir/Module.h"

using namespace clotho;

namespace {

// Lowering a concurrent assertion still fails loudly — the monitor is built but
// not code-generated yet — so the module has to be recovered from a failed
// lowering. `recover()` records the diagnostic and keeps going, and the monitor is
// pushed before the guard fires, which is exactly what lets these run ahead of
// the backend.
const ir::Module& lower_for_monitor(std::string_view source, LowerResult& out) {
    out = lower_text(source);
    REQUIRE(out.module != nullptr);
    return *out.module;
}

// The one shape Phase 2 targets.
constexpr std::string_view implication = R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b);
endmodule
)";

} // namespace

TEST_CASE("a nonoverlapping implication lowers to a trigger and an obligation") {
    LowerResult r;
    const ir::Module& m = lower_for_monitor(implication, r);

    REQUIRE(m.monitors.size() == 1);
    const ir::AssertionMonitor& mon = m.monitors[0];

    CHECK(mon.kind == ir::AssertionKind::Assert);
    // A property with no strong()/weak() is weak by default in an assert
    // (LRM 16.12.2), so a pending attempt holds when the trace ends.
    CHECK(mon.strength == ir::AssertionStrength::Weak);
    CHECK(mon.edge == ir::Edge::Positive);

    // `|=>` seeds the obligation the cycle after the trigger matches.
    CHECK(mon.has_trigger);
    CHECK_FALSE(mon.overlap);

    // One letter each side, so one state each.
    CHECK(mon.trigger.state_count == 1);
    CHECK(mon.obligation.state_count == 1);
    CHECK(mon.obligation.words == 1);
    // The obligation's longest match is one cycle, so one attempt can be pending.
    CHECK(mon.depth == 1);

    // The single obligation state both begins and completes a match, and has no
    // successor.
    CHECK(mon.obligation.first[0] == 1);
    CHECK(mon.obligation.last[0] == 1);
    CHECK(mon.obligation.follow[0] == 0);

    // Two atoms plus the constant-false disable condition, which is a real root
    // rather than an absent edge.
    CHECK(mon.roots.empty()); // the caller fills these when it emits them
    REQUIRE(r.module != nullptr);
    CHECK(mon.reset_root < 3);

    // Guard 0 is the empty cube and guard 1 the reserved unsatisfiable one, so a
    // real guard starts at 2.
    REQUIRE(mon.cubes.size() >= 3);
    CHECK(mon.cubes[0].len == 0);
    // Every state names a guard in range, and every literal a root in range.
    for (const uint32_t guard : mon.obligation.guard_of) {
        REQUIRE(guard < mon.cubes.size());
        const ir::AssertionCube& cube = mon.cubes[guard];
        for (uint32_t i = 0; i < cube.len; i++) {
            CHECK(mon.literals[cube.start + i].root < 3);
            // Clotho emits negation normal form, so no cube literal is negated:
            // a negated term is its own atom (concurrent-assertions.md §5.3).
            CHECK_FALSE(mon.literals[cube.start + i].negated);
        }
    }

    // guard_states is the transpose of guard_of, and is what lets a backend fold
    // the satisfied mask over guards instead of gathering per state.
    REQUIRE(mon.obligation.guard_states.size() == mon.cubes.size() * mon.obligation.words);
    for (uint32_t state = 0; state < mon.obligation.state_count; state++) {
        const uint32_t guard = mon.obligation.guard_of[state];
        CHECK((mon.obligation.guard_states[guard] >> state & 1) == 1);
    }
}

TEST_CASE("an overlapping implication differs only in when it seeds") {
    LowerResult r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |-> b);
endmodule
)", r);
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.monitors[0].has_trigger);
    CHECK(m.monitors[0].overlap);
}

TEST_CASE("a bare sequence property has no trigger and starts every cycle") {
    LowerResult r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a ##1 b);
endmodule
)", r);
    REQUIRE(m.monitors.size() == 1);
    const ir::AssertionMonitor& mon = m.monitors[0];
    CHECK_FALSE(mon.has_trigger);
    // Two letters, so two states, and the longest match spans two cycles.
    CHECK(mon.obligation.state_count == 2);
    CHECK(mon.depth == 2);
    // State 0 begins a match and state 1 completes it.
    CHECK(mon.obligation.first[0] == 0b01);
    CHECK(mon.obligation.last[0] == 0b10);
    CHECK(mon.obligation.follow[0] == 0b10);
}

TEST_CASE("a delay range adds a filler state per cycle it may skip") {
    LowerResult r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a ##[1:3] b);
endmodule
)", r);
    REQUIRE(m.monitors.size() == 1);
    const ir::AssertionMonitor& mon = m.monitors[0];
    // `a`, `b`, and two filler states for the cycles the delay may skip.
    CHECK(mon.obligation.state_count == 4);
    CHECK(mon.depth == 4);
}

TEST_CASE("strong() flips what a pending attempt does at end of trace") {
    LowerResult r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) strong(a ##1 b));
endmodule
)", r);
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.monitors[0].strength == ir::AssertionStrength::Strong);
}

TEST_CASE("negation normal form gives a negated term its own atom") {
    LowerResult r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) !(a && b) |=> c);
endmodule
)", r);
    REQUIRE(m.monitors.size() == 1);
    const ir::AssertionMonitor& mon = m.monitors[0];
    // De Morgan turns `!(a && b)` into `!a || !b`, so the antecedent's guard is a
    // disjunction — which a cube cannot split, making it one literal over one root
    // whose expression is the whole disjunction.
    REQUIRE(mon.trigger.state_count == 1);
    const ir::AssertionCube& cube = mon.cubes[mon.trigger.guard_of[0]];
    CHECK(cube.len == 1);
    CHECK_FALSE(mon.literals[cube.start].negated);
}

TEST_CASE("disable iff becomes the reset root") {
    LowerResult r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, rst, a, b;
  ap: assert property (@(posedge clk) disable iff (rst) a |=> b);
endmodule
)", r);
    REQUIRE(m.monitors.size() == 1);
    const ir::AssertionMonitor& mon = m.monitors[0];
    // rst is a real atom now rather than a constant, so it occupies a root of its
    // own alongside a and b.
    CHECK(mon.reset_root < 3);
}

// -- Loud refusals ----------------------------------------------------------
//
// Every shape libsva cannot lower, and every shape clotho has not wired up, must
// be refused with a message rather than accepted. The project forbids silently
// accepting a construct that is not correctly lowered.

namespace {

void refuses(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos,
                  r.diagnostic_text());
}

} // namespace

TEST_CASE("an unbounded obligation is refused, naming the cause") {
    // `##[1:$]` in a consequent has no bounded longest match, so the number of
    // concurrent obligations is unbounded and the automaton cannot track it.
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> ##[1:$] b);
endmodule
)", "no bounded longest match");
}

TEST_CASE("a nested implication in a consequent is refused") {
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) a |=> (b |=> c));
endmodule
)", "itself an implication");
}

TEST_CASE("an assertion with no clocking event is refused") {
    refuses(R"(
module t;
  bit clk, a, b;
  initial ap: assert property (a |=> b);
endmodule
)", "explicit clocking event");
}

TEST_CASE("an action block is refused until the Reactive region exists") {
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b) else $error("boom");
endmodule
)", "action blocks are not supported");
}

TEST_CASE("property operators libsva has no tag for are refused") {
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a until b);
endmodule
)", "not supported yet");
}

TEST_CASE("sequence operators libsva has no tag for are refused") {
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a ##1 b) and (a ##1 c));
endmodule
)", "not supported yet");
}

TEST_CASE("a multiclock assertion is refused") {
    refuses(R"(
module t;
  bit clk, fast, a, b;
  ap: assert property (@(posedge clk) a |=> @(posedge fast) b);
endmodule
)", "not supported yet");
}

TEST_CASE("code generation is not implemented, and says so") {
    // The honest guard: the monitor is lowered, but nothing emits it yet, so the
    // construct must still be refused rather than silently ignored.
    refuses(implication, "not yet code-generated");
}
