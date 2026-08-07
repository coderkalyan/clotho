// Concurrent assertions (LRM 16): the front-end's lowering to ir::AssertionMonitor.
//
// These check the tables clotho hands the backend, because that is the contract
// between the two halves and it is the part libsva's own tests cannot see. The
// temporal semantics themselves are already verified inside libsva by differential
// and mutation testing, and the automaton step's codegen has its own oracle; see
// docs/design/concurrent-assertions.md §11.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"
#include "ir/Module.h"

using namespace clotho;

namespace {

// Lowers `source` and hands back the module. A concurrent assertion is emitted
// now, so an ok lowering is the normal case; the helper still tolerates a failed
// one, because the refusal tests below check the module was built up to the point
// the guard fired.
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

// -- Monitor emission (LRM 16, concurrent-assertions.md 7) -------------------
//
// The automaton step is emitted as ordinary clotho IR from the front end: hidden
// two-state signals hold the state, and the per-cycle protocol becomes masks and
// selects over compile-time constant rows. These check the shape of what a
// monitor costs and what it is allowed to read; the temporal semantics are
// libsva's, and the step's arithmetic follows its differential-tested reference.

namespace {

const ir::SignalInfo* find_signal(const ir::Module& m, std::string_view name) {
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

uint32_t state_width(const ir::Module& m, std::string_view name) {
    const ir::SignalInfo* s = find_signal(m, name);
    REQUIRE(s != nullptr);
    // A state mask is a set, never a design value, so it carries no x/z.
    CHECK(m.pool.layout(s->type).domain == ir::Domain::Two);
    return m.pool.layout(s->type).width;
}

size_t activation_count(const ir::Module& m, ir::Activation want) {
    size_t n = 0;
    for (const ir::Process& p : m.processes()) {
        n += p.activation == want ? 1 : 0;
    }
    return n;
}

// The one Observed process a monitor contributes. A design that is nothing but
// an assertion has no other: nothing else clotho emits runs in Observed, so the
// region holds exactly the monitors.
const ir::Process& monitor_process(const ir::Module& m) {
    const ir::Process* found = nullptr;
    for (const ir::Process& p : m.processes()) {
        if (p.activation == ir::Activation::Observed) {
            REQUIRE(found == nullptr);
            found = &p;
        }
    }
    REQUIRE(found != nullptr);
    return *found;
}

bool touches(const ir::Module& m, const ir::Process& p, std::string_view name) {
    for (size_t i = 0; i < p.ir.inst_count(); i++) {
        const auto idx = static_cast<ir::Index>(i);
        if (p.ir.tag(idx) == ir::Inst::Tag::Signal &&
            m.signal(p.ir.payload(idx).signal).name == name) {
            return true;
        }
    }
    return false;
}

bool has_tag(const ir::Process& p, ir::Inst::Tag want) {
    for (size_t i = 0; i < p.ir.inst_count(); i++) {
        if (p.ir.tag(static_cast<ir::Index>(i)) == want) {
            return true;
        }
    }
    return false;
}

// The same, across every lowered function. An action block is a synthetic
// zero-formal task rather than a process, so its body is not reachable through
// monitor_process.
bool any_function_has_tag(const ir::Module& m, ir::Inst::Tag want) {
    for (size_t k = 0; k < m.function_count(); k++) {
        const ir::Ir& ir = m.function(static_cast<ir::FunctionId>(k)).ir;
        for (size_t i = 0; i < ir.inst_count(); i++) {
            if (ir.tag(static_cast<ir::Index>(i)) == want) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("a nonoverlapping implication emits a monitor process and its state") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(implication, r);
    CHECK(r.ok);

    REQUIRE(m.monitors.size() == 1);
    const ir::Process& p = monitor_process(m);
    // One cycle is one clock edge. An Observed process is resumed once per time
    // slot rather than woken by an event, so the wait list records which event
    // delimits the cycle while the body detects the edge itself; nothing else may
    // step the monitor -- not even time zero, which is why it does not self-start.
    REQUIRE(p.wait_events.size() == 1);
    CHECK(p.wait_events[0].signal == m.monitors[0].clock);
    CHECK(p.wait_events[0].edge == ir::Edge::Positive);
    CHECK_FALSE(p.self_start);

    // One letter each side, so a one-state mask each, and one attempt at a time.
    CHECK(state_width(m, "sva.0.trig") == 1);
    CHECK(state_width(m, "sva.0.pend0") == 1);
    CHECK(state_width(m, "sva.0.occ") == 1);
    CHECK(state_width(m, "sva.0.fired") == 1);
    CHECK(find_signal(m, "sva.0.pend1") == nullptr);

    // The roots stay values inside this process, so nothing indexes back into a
    // body the monitor owns.
    CHECK(m.monitors[0].roots.empty());
}

TEST_CASE("the monitor reads sampled shadows, and the disable condition live") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, rst, a, b;
  ap: assert property (@(posedge clk) disable iff (rst) a |=> b);
endmodule
)", r);
    CHECK(r.ok);

    const ir::Process& p = monitor_process(m);
    // LRM 16.5: an assertion expression reads the value a variable held before
    // this step's updates, so the step names shadows and never the live slots.
    CHECK(touches(m, p, "sva.shadow.a"));
    CHECK(touches(m, p, "sva.shadow.b"));
    CHECK_FALSE(touches(m, p, "a"));
    CHECK_FALSE(touches(m, p, "b"));
    // LRM 16.15: `disable iff` is the one condition deliberately not sampled.
    CHECK(touches(m, p, "rst"));
    CHECK_FALSE(touches(m, p, "sva.shadow.rst"));
}

TEST_CASE("a bare sequence property keeps no trigger state") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a ##1 b);
endmodule
)", r);
    CHECK(r.ok);

    // No antecedent: an attempt starts every cycle, so there is nothing to
    // remember about a trigger and no `fired` to carry into the next one.
    CHECK(find_signal(m, "sva.0.trig") == nullptr);
    CHECK(find_signal(m, "sva.0.fired") == nullptr);
    // Two letters, so a two-state mask, and two attempts can be pending at once.
    CHECK(state_width(m, "sva.0.pend0") == 2);
    CHECK(state_width(m, "sva.0.pend1") == 2);
    CHECK(state_width(m, "sva.0.occ") == 2);
    CHECK(find_signal(m, "sva.0.pend2") == nullptr);
}

TEST_CASE("two assertions in one module get separate state and separate processes") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b);
  bp: assert property (@(posedge clk) b |=> a);
endmodule
)", r);
    CHECK(r.ok);

    REQUIRE(m.monitors.size() == 2);
    // Named by position in the monitor table, so two assertions cannot collide.
    CHECK(find_signal(m, "sva.0.pend0") != nullptr);
    CHECK(find_signal(m, "sva.1.pend0") != nullptr);
    CHECK(activation_count(m, ir::Activation::Observed) == 2);
    // Both assertions sample the same two variables, and a shadow is shared.
    size_t shadows = 0;
    for (const ir::SignalInfo& s : m.signals()) {
        shadows += s.name.starts_with("sva.shadow.") ? 1 : 0;
    }
    CHECK(shadows == 2);
}

TEST_CASE("only a strong property drains its obligations at end of trace") {
    // LRM 16.12.2: an attempt still pending when the trace ends holds for a weak
    // property and fails for a strong one, so only the strong one needs a Final
    // process to report it.
    LowerResult       weak;
    const ir::Module& wm = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a ##1 b);
endmodule
)", weak);
    CHECK(weak.ok);
    CHECK(activation_count(wm, ir::Activation::Final) == 0);

    LowerResult       strong;
    const ir::Module& sm = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) strong(a ##1 b));
endmodule
)", strong);
    CHECK(strong.ok);
    CHECK(activation_count(sm, ir::Activation::Final) == 1);
}

TEST_CASE("a cover counts matches where an assert reports failures") {
    LowerResult       cov;
    const ir::Module& cm = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  cp: cover property (@(posedge clk) a ##1 b);
endmodule
)", cov);
    CHECK(cov.ok);

    // A cover has no failure verdict (LRM 16.12.6): it tallies matches, and with
    // no action block it schedules nothing into the Reactive region at all.
    CHECK(find_signal(cm, "sva.0.cov") != nullptr);
    CHECK(state_width(cm, "sva.0.cov") == 32);
    CHECK_FALSE(has_tag(monitor_process(cm), ir::Inst::Tag::ReactivePend));

    LowerResult       as;
    const ir::Module& am = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a ##1 b);
endmodule
)", as);
    CHECK(as.ok);
    CHECK(find_signal(am, "sva.0.cov") == nullptr);
    // The default failure report is the implicit fail action (LRM 16.14.1), so it
    // takes the same route as a written one: pended into Reactive, never inline.
    CHECK(has_tag(monitor_process(am), ir::Inst::Tag::ReactivePend));
    CHECK_FALSE(has_tag(monitor_process(am), ir::Inst::Tag::Severity));
}

TEST_CASE("an assertion action block becomes reactive-region code") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b) else $error("custom fail msg");
endmodule
)", r);
    CHECK(r.ok);

    // LRM 4.4.2.5: a property is evaluated in Observed and the pass / fail code it
    // triggers is scheduled into Reactive. The monitor is the Observed half.
    const ir::Process& p = monitor_process(m);
    CHECK(p.activation == ir::Activation::Observed);

    // Still one step per clock edge, but the edge is now detected against the
    // monitor's own one-slot clock history rather than by a sensitivity list.
    CHECK(state_width(m, "sva.0.clk") == 1);

    // The action is scheduled, never run inline: its $error lives in the
    // synthesized task, so the monitor body itself reports nothing.
    CHECK(has_tag(p, ir::Inst::Tag::ReactivePend));
    CHECK_FALSE(has_tag(p, ir::Inst::Tag::Severity));
}

TEST_CASE("a cover property runs its pass action on a match") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  cp: cover property (@(posedge clk) a ##1 b) $display("hit");
endmodule
)", r);
    CHECK(r.ok);

    // A cover's action block is its match action (LRM 16.12.6). It still tallies,
    // and the action is pended exactly like an assert's -- which is what orders
    // the print after the same edge's Active-region output instead of racing it.
    CHECK(find_signal(m, "sva.0.cov") != nullptr);
    CHECK(has_tag(monitor_process(m), ir::Inst::Tag::ReactivePend));
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

TEST_CASE("an assertion with no clocking event and nothing to infer from is refused") {
    // An `initial` with no timing control offers 16.14.6 no candidate event, and
    // there is no default clocking block, so the assertion has no clock at all.
    // slang's own analysis is what catches this one -- clotho's guard sits behind
    // it as a backstop -- but the requirement is only that it stay loud, so the
    // needle is the wording both share.
    refuses(R"(
module t;
  bit clk, a, b;
  initial ap: assert property (a |=> b);
endmodule
)", "clocking event");

    // Same, with the clock present but disqualified: LRM 16.14.6 only infers an
    // event whose terms appear nowhere else in the procedure, and here `clk` is
    // also read into `seen`. Nothing is inferable, so this must not quietly pick
    // `clk` anyway.
    refuses(R"(
module t;
  bit clk, a, b, seen;
  always @(posedge clk) begin
    seen <= clk;
    assert property (a |=> b);
  end
endmodule
)", "clocking event");
}

TEST_CASE("a nonblocking assignment in an action block lowers, for Re-NBA to commit") {
    // LRM 4.4.2.8 gives the reactive region set its own update region, so this is
    // legal rather than refused: the store stages like any other nonblocking
    // assignment and the backend's re_nba.commit applies it after the Reactive
    // and Re-Inactive regions settle, still inside this time slot.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b, flag;
  ap: assert property (@(posedge clk) a |=> b) else flag <= 1;
endmodule
)", r);
    REQUIRE(r.ok);

    // The action block lowers to a synthetic zero-formal task rather than a
    // process, so the store has to be looked for among the functions. Checking it
    // survived lowering is the point: dropping it silently is the failure this
    // guards, now that no refusal stands in the way.
    CHECK(any_function_has_tag(m, ir::Inst::Tag::StoreNb));
    // Still pended into Reactive -- the region it commits from is what changed,
    // not the region it runs in.
    CHECK(has_tag(monitor_process(m), ir::Inst::Tag::ReactivePend));
}

TEST_CASE("a nonzero delay in an action block is refused, but a #0 is not") {
    // Reactive code may suspend only where a reactive region can resume it. LRM
    // 4.4.2.7 gives `#0` Re-Inactive, which promotes back into Reactive at the
    // same instant; `#5` would come back through the wheel, which pushes onto the
    // Active ring. So the gate is on the amount being zero, not on it being a
    // delay -- the two must not be conflated.
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b) else #5 $error("late");
endmodule
)", "a nonzero delay in an assertion action block");

    // A run-time amount is refused for the same reason: it is not provably zero.
    refuses(R"(
module t;
  bit clk, a, b;
  int d;
  ap: assert property (@(posedge clk) a |=> b) else #d $error("late");
endmodule
)", "a nonzero delay in an assertion action block");

    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b) else #0 $error("late");
endmodule
)", r);
    CHECK(r.ok);
    CHECK(has_tag(monitor_process(m), ir::Inst::Tag::ReactivePend));
}

TEST_CASE("an event control in an action block is refused") {
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b) else @(posedge clk) $error("late");
endmodule
)", "an event control in an assertion action block");
}

TEST_CASE("property operators libsva has no tag for are refused") {
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a until b);
endmodule
)", "not supported yet");
}

TEST_CASE("first_match determinises its operand, in the position where it counts") {
    // `first_match` was the last sequence-layer operator of LRM 16.9 without a
    // libsva tag, so there is no longer a "no tag" case at the sequence layer to
    // test -- the property layer keeps its own, above.
    //
    // The ANTECEDENT is deliberate, and should survive whoever edits this. In a
    // consequent, or as a bare top-level sequence property, `first_match` cannot
    // change a verdict at all: a sequence property holds the moment its sequence
    // completes a match, and the earliest match is exactly the one `first_match`
    // keeps. Only an antecedent, where every match seeds its OWN obligation,
    // makes the suppression observable. See concurrent-assertions.md 9.2.
    //
    // What is checked here is that the operand's automaton was really rebuilt.
    // libsva lowers `first_match` by DETERMINISING its operand rather than by
    // combining automata, because whether a match survives depends on whether a
    // shorter one covers the same cycles. So a lowering that quietly dropped the
    // operator -- the one mutant a consequent-position test cannot catch -- would
    // leave the trigger automaton identical to the bare operand's. It must not be.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) first_match(a ##[1:2] b) |=> c);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    REQUIRE(m.monitors[0].has_trigger);

    LowerResult       plain_r;
    const ir::Module& plain = lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> c);
endmodule
)", plain_r);
    REQUIRE(plain_r.ok);
    REQUIRE(plain.monitors.size() == 1);

    // The subset construction over `a ##[1:2] b` costs a state: the two matches
    // that the bare operand keeps apart have to be told apart by the automaton
    // itself before the longer one can be dropped.
    CHECK(m.monitors[0].trigger.state_count > plain.monitors[0].trigger.state_count);
    // tests/sva/diff_firstmatch.sv is the behavioural half: the same antecedent
    // on a trace where suppression removes three obligations, giving 4 reports
    // where the operator-free design reports 7.
}

TEST_CASE("first_match with match items is refused") {
    // Match items assign to local variables on a match (LRM 16.10), and libsva's
    // automaton is a plain state set with no per-attempt data to assign into.
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) first_match(a ##[1:2] b, $display("m")) |=> c);
endmodule
)", "sequence match items are not supported yet");

    // The same property without the match item lowers, so the refusal above is
    // attributable to the match item and not to the operator around it.
    LowerResult r;
    lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) first_match(a ##[1:2] b) |=> c);
endmodule
)", r);
    CHECK(r.ok);
}

TEST_CASE("a multiclock assertion is refused") {
    refuses(R"(
module t;
  bit clk, fast, a, b;
  ap: assert property (@(posedge clk) a |=> @(posedge fast) b);
endmodule
)", "not supported yet");
}

// -- A clock declared inside a sequence or property (LRM 16.13.1) -----------
//
// `sequence s; @(posedge clk) a ##1 b; endsequence` puts the assertion's only
// clocking event below the instantiation, where peeling the top of the property
// spec cannot see it. Clock flow carries it out over the whole assertion, and
// the nested event is then stepped over rather than treated as a second clock.
// tests/sva/diff_seqclock.sv is the differential half of this.

TEST_CASE("a sequence carrying its own clock clocks the assertion that uses it") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, gate, a, b, c;
  sequence sab;
    @(negedge clk) a ##1 b;
  endsequence
  property pbc;
    @(negedge clk) b |=> c;
  endproperty
  ap: assert property (sab);
  bp: assert property (sab |=> @(negedge clk) c);
  cp: assert property (pbc);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 3);

    // The edge matters as much as the signal: a resolver that found `clk` but
    // defaulted the edge would halve the sample rate and still look right here.
    // `gate` is in scope and unused, so nothing can be arriving from a
    // design-wide guess either.
    //
    // `bp` is also where equivalence earns its keep. A clock does not flow from
    // an antecedent into a consequent (16.13.1 hands the consequent the clock
    // from outside the operator, and there is none), so the consequent spells
    // the event a second time -- two separately bound TimingControl objects
    // that a pointer comparison would call two clocks.
    for (const ir::AssertionMonitor& mon : m.monitors) {
        CHECK(m.signal(mon.clock).name == "clk");
        CHECK(mon.edge == ir::Edge::Negative);
    }
    // The bare sequence is a sequence property; the other two are implications,
    // so the nested event was stepped over at both layers.
    CHECK_FALSE(m.monitors[0].has_trigger);
    CHECK(m.monitors[1].has_trigger);
    CHECK(m.monitors[2].has_trigger);
}

TEST_CASE("a clock in the body beats one inferred from the procedure") {
    // 16.14.6 inference is the last resort, not the first: the procedure offers
    // `posedge gate` and the sequence declares `negedge clk`, and the declared
    // one is the assertion's.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, gate, a, b;
  sequence sab;
    @(negedge clk) a ##1 b;
  endsequence
  always @(posedge gate) assert property (sab);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.signal(m.monitors[0].clock).name == "clk");
    CHECK(m.monitors[0].edge == ir::Edge::Negative);
}

TEST_CASE("one sequence used twice is one clock, not two") {
    // Two uses of one declaration, which slang happens to bind to a single
    // shared TimingControl -- so this does not pin equivalence-over-identity the
    // way `bp` above does. What it pins is that the resolver's choice survives
    // being met twice on the way down, instead of the second encounter reading
    // as a second clock.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  sequence sab;
    @(posedge clk) a ##1 b;
  endsequence
  ap: assert property (sab ##1 sab);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.signal(m.monitors[0].clock).name == "clk");
    CHECK(m.monitors[0].edge == ir::Edge::Positive);
}

TEST_CASE("two different clocks reached through named declarations are refused") {
    // The clock hoist must not turn a genuinely multiclocked assertion into a
    // single-clocked one by picking whichever event it happened to find first.
    refuses(R"(
module t;
  bit clk, fast, a, b;
  sequence slow;
    @(posedge clk) a ##1 b;
  endsequence
  sequence quick;
    @(posedge fast) b ##1 a;
  endsequence
  ap: assert property (slow ##1 quick);
endmodule
)", "multiclock");
}

TEST_CASE("an explicit clock on the assertion wins over one in the body") {
    // Precedence is observable through the conflict it creates. The assertion
    // says posedge and the sequence says negedge; because the assertion's is the
    // one chosen, the sequence's is a second clock and the whole thing is
    // refused. Had the body's won, this would have lowered on negedge.
    refuses(R"(
module t;
  bit clk, a, b;
  sequence sab;
    @(negedge clk) a ##1 b;
  endsequence
  ap: assert property (@(posedge clk) sab);
endmodule
)", "multiclock");

    // And when the two agree, the nested one is stepped over rather than
    // refused for being nested at all.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  sequence sab;
    @(posedge clk) a ##1 b;
  endsequence
  ap: assert property (@(posedge clk) sab);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.signal(m.monitors[0].clock).name == "clk");
    CHECK(m.monitors[0].edge == ir::Edge::Positive);
}

TEST_CASE("a function call in an assertion expression is refused") {
    // Sampling captures the variables an expression names, and clotho does not
    // trace a subroutine body, so a call would read live state from the middle of
    // a sampled expression.
    refuses(R"(
module t;
  bit clk, a, b;
  function automatic bit f(bit x);
    return !x;
  endfunction
  ap: assert property (@(posedge clk) f(a) |=> b);
endmodule
)", "function call in a concurrent assertion expression");
}

// -- Inferred clocking (LRM 16.14.6) ----------------------------------------
//
// A property with no clocking event of its own takes the one from the procedure
// it sits in. slang's analysis pass does the inference; these check that clotho
// reads it and that what reaches the monitor is the enclosing procedure's clock
// -- both the signal and the edge, since defaulting the edge to posedge would
// pass a signal-only check while silently halving the sample rate.
// tests/sva/diff_inferclk.sv is the differential half of this.

TEST_CASE("an assertion in a clocked procedure infers that procedure's clock") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  always @(posedge clk) assert property (a |=> b);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);

    const ir::AssertionMonitor& mon = m.monitors[0];
    CHECK(m.signal(mon.clock).name == "clk");
    CHECK(mon.edge == ir::Edge::Positive);
    // The inferred clock is not the only signal in scope, so the check above
    // would also pass by accident if the monitor were stepped by nothing at all.
    CHECK(mon.has_trigger);
    CHECK_FALSE(mon.overlap);
}

TEST_CASE("an inferred clock carries the procedure's edge, not a default") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, gate, a, b;
  always @(posedge gate) assert property (a |-> b);
  always @(negedge clk) assert property (a |=> b);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 2);

    // Two procedures, two different clocks and two different edges: each monitor
    // must take its own, so neither the signal nor the edge can be coming from a
    // single design-wide guess.
    CHECK(m.signal(m.monitors[0].clock).name == "gate");
    CHECK(m.monitors[0].edge == ir::Edge::Positive);
    CHECK(m.signal(m.monitors[1].clock).name == "clk");
    CHECK(m.monitors[1].edge == ir::Edge::Negative);
}

TEST_CASE("a reset flop infers the clock and not the reset") {
    // The pervasive testbench shape. Two events are in the list, and 16.14.6
    // picks the one whose terms the body does not otherwise read: the body tests
    // `rst_n`, so `negedge rst_n` is disqualified and `posedge clk` is left. This
    // is the case a "just take the procedure's timing control" shortcut gets
    // wrong, because the timing control here is the whole two-event list.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, rst_n, a, b;
  int unsigned n;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) n <= 0;
    else n <= n + 1;
    assert property (a |=> b);
  end
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.signal(m.monitors[0].clock).name == "clk");
    CHECK(m.monitors[0].edge == ir::Edge::Positive);
}

TEST_CASE("each duplicated instance body gets its own inferred clock") {
    // slang analyses one instance body and skips every exact duplicate of it
    // (InstanceSymbol::getCanonicalBody), while clotho lowers each instance's own
    // body. An AST-keyed inference table misses the duplicates outright, and
    // would name the analysed instance's clock signal if it did not. Two
    // instances on two different clocks pin both halves.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module sub(input bit clk, input bit a, input bit b);
  always @(posedge clk) assert property (a |=> b);
endmodule
module t;
  bit ck0, ck1, a, b;
  sub u0(ck0, a, b);
  sub u1(ck1, a, b);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 2);
    CHECK(m.signal(m.monitors[0].clock).name == "u0.clk");
    CHECK(m.signal(m.monitors[1].clock).name == "u1.clk");
}

TEST_CASE("an inferred clock clotho cannot place in the procedure is refused") {
    // slang infers a clock here -- the `@(posedge clk)` the loop blocks on -- but
    // it is not one of the events the procedure waits on, so clotho has no
    // instance-local TimingControl to resolve it to. Refused, rather than
    // substituting whatever event control happens to be at hand.
    refuses(R"(
module t;
  bit clk, a, b;
  always begin
    @(posedge clk);
    assert property (a |=> b);
  end
endmodule
)", "not one the enclosing procedure waits on");
}

TEST_CASE("an inferred clock and an explicit one lower identically") {
    // The clocking event is peeled to the same TimingControl either way, so the
    // monitor's whole shape -- not just its clock -- must be indistinguishable.
    LowerResult       inferred_r;
    const ir::Module& inferred = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  always @(posedge clk) assert property (a |=> b);
endmodule
)", inferred_r);
    LowerResult       explicit_r;
    const ir::Module& expl = lower_for_monitor(implication, explicit_r);
    REQUIRE(inferred_r.ok);
    REQUIRE(explicit_r.ok);
    REQUIRE(inferred.monitors.size() == 1);
    REQUIRE(expl.monitors.size() == 1);

    const ir::AssertionMonitor& i = inferred.monitors[0];
    const ir::AssertionMonitor& e = expl.monitors[0];
    CHECK(inferred.signal(i.clock).name == expl.signal(e.clock).name);
    CHECK(i.edge == e.edge);
    CHECK(i.kind == e.kind);
    CHECK(i.strength == e.strength);
    CHECK(i.has_trigger == e.has_trigger);
    CHECK(i.overlap == e.overlap);
    CHECK(i.depth == e.depth);
    CHECK(i.trigger.state_count == e.trigger.state_count);
    CHECK(i.obligation.state_count == e.obligation.state_count);
    CHECK(i.cubes.size() == e.cubes.size());
}

TEST_CASE("a default clocking block still refuses, ahead of any inference") {
    // The other half of 16.14.6: with no candidate event in the procedure, the
    // scope's `default clocking` supplies one (LRM 14.12) and slang hands it back
    // through the same channel as a procedural inference. Clotho cannot lower a
    // clocking block at all yet, so the member walk refuses the design before the
    // assertion is reached -- which is the right order. This pins that it is the
    // clocking block being refused and not the assertion being silently
    // mis-clocked; it flips to a positive test when clocking blocks land.
    refuses(R"(
module t;
  bit clk, a, b;
  default clocking cb @(posedge clk); endclocking
  initial assert property (a |=> b);
endmodule
)", "kind=ClockingBlock");
}

// -- Named sequences, properties and lets (LRM 16.8, 16.12) -----------------
//
// slang expands an instance eagerly, so the contract clotho has to keep is that
// instantiation is *transparent*: a named sequence or property must lower to
// exactly the monitor its hand-expanded form does. Comparing the two whole
// automata is a stronger check than any single field, and it is the one that
// catches lowering an instance at the wrong layer.

namespace {

void same_automaton(const ir::AssertionAutomaton& got, const ir::AssertionAutomaton& want) {
    CHECK(got.state_count == want.state_count);
    CHECK(got.words == want.words);
    CHECK(got.guard_of == want.guard_of);
    CHECK(got.follow == want.follow);
    CHECK(got.first == want.first);
    CHECK(got.last == want.last);
    CHECK(got.guard_states == want.guard_states);
}

// Lowers both sources and checks the two monitors are the same automaton.
void lowers_like(std::string_view named, std::string_view expanded) {
    LowerResult named_r;
    LowerResult expanded_r;
    const ir::Module& a = lower_for_monitor(named, named_r);
    const ir::Module& b = lower_for_monitor(expanded, expanded_r);
    CHECK_MESSAGE(named_r.ok, named_r.diagnostic_text());
    CHECK_MESSAGE(expanded_r.ok, expanded_r.diagnostic_text());
    REQUIRE(a.monitors.size() == 1);
    REQUIRE(b.monitors.size() == 1);

    CHECK(a.monitors[0].has_trigger == b.monitors[0].has_trigger);
    CHECK(a.monitors[0].overlap == b.monitors[0].overlap);
    CHECK(a.monitors[0].strength == b.monitors[0].strength);
    CHECK(a.monitors[0].depth == b.monitors[0].depth);
    CHECK(a.monitors[0].cubes.size() == b.monitors[0].cubes.size());
    same_automaton(a.monitors[0].trigger, b.monitors[0].trigger);
    same_automaton(a.monitors[0].obligation, b.monitors[0].obligation);
}

} // namespace

TEST_CASE("a named sequence with formal arguments lowers as if written inline") {
    // Two different actuals through the same declaration, so this fails if the
    // body were shared rather than expanded per use.
    lowers_like(R"(
module t;
  bit clk, a, b, c;
  sequence ab(x, y);
    x ##1 y;
  endsequence
  ap: assert property (@(posedge clk) ab(a, b) |-> c);
endmodule
)", R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a ##1 b) |-> c);
endmodule
)");
}

TEST_CASE("a named property lowers at the property layer, not the sequence one") {
    // The body is an implication, so it has to be handed to prop_implies. Routed
    // through the sequence layer instead it would reject, and a property node
    // reaching prop_sequence would trip libsva's operand-layer assertion.
    lowers_like(R"(
module t;
  bit clk, a, b;
  property p(x, y);
    x |=> y;
  endproperty
  ap: assert property (@(posedge clk) p(a, b));
endmodule
)", R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b);
endmodule
)");
}

TEST_CASE("a named sequence nests inside another named declaration") {
    lowers_like(R"(
module t;
  bit clk, a, b, c;
  sequence ab(x, y);
    x ##1 y;
  endsequence
  property leads_to(x, y, z);
    ab(x, y) |=> z;
  endproperty
  ap: assert property (@(posedge clk) leads_to(a, b, c));
endmodule
)", R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a ##1 b) |=> c);
endmodule
)");
}

TEST_CASE("a let expands into the boolean layer with no assertion machinery") {
    // slang expands a let call straight to the substituted Expression rather
    // than to an AssertionInstanceExpression, so a let inside an assertion is
    // already an ordinary boolean by the time the walker sees it.
    lowers_like(R"(
module t;
  bit clk, a, b, c;
  let both(x, y) = x && y;
  ap: assert property (@(posedge clk) both(a, b) |=> c);
endmodule
)", R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a && b) |=> c);
endmodule
)");
}

TEST_CASE("an uninstantiated sequence / property / let declaration emits nothing") {
    // A declaration is a template: it has no behavior of its own, so skipping it
    // in the module-member walk is the whole of its lowering.
    LowerResult r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  sequence unused_seq(x, y);
    x ##1 y;
  endsequence
  property unused_prop(x, y);
    unused_seq(x, y) |=> y;
  endproperty
  let unused_let(x, y) = x && y;
  initial #1 $finish;
endmodule
)", r);
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(m.monitors.empty());
    // No sampling shadow and no Preponed sampler either: nothing here is
    // assertion machinery, because nothing was instantiated.
    for (const ir::SignalInfo& s : m.signals()) {
        CHECK_FALSE(s.name.starts_with("sva."));
    }
    for (const ir::Process& p : m.processes()) {
        CHECK(p.activation != ir::Activation::Preponed);
        CHECK(p.activation != ir::Activation::Observed);
    }
}

TEST_CASE("a sequence with a local variable is refused") {
    // LRM 16.10 local variables are per-attempt storage, and libsva's automaton
    // is a plain state set with no data to attach a register file to. Refused at
    // the instance rather than deeper, so the message names the real cause.
    refuses(R"(
module t;
  bit clk, a, b, c;
  sequence s;
    int v;
    (a, v = 1) ##1 (b && v == 1);
  endsequence
  ap: assert property (@(posedge clk) s |=> c);
endmodule
)", "local variables in a sequence or property");
}

TEST_CASE("a recursive property is refused") {
    // LRM 16.12.4. `|=>` supplies the time advance that makes the recursion
    // legal, so slang accepts this and hands back an instance whose body is a
    // placeholder -- which is why the check is on the flag, not on the body.
    refuses(R"(
module t;
  bit clk, a;
  property p(x);
    x |=> p(x);
  endproperty
  ap: assert property (@(posedge clk) p(a));
endmodule
)", "recursive property");
}

TEST_CASE("a disable iff inside a named property is refused, not silently dropped") {
    // Only a `disable iff` at the top of the assertion is peeled into the
    // request. LRM 16.12 also allows one at the top of a property declaration,
    // where it would have to be hoisted back out through the instantiation.
    refuses(R"(
module t;
  bit clk, rst, a, b;
  property p;
    disable iff (rst) a |=> b;
  endproperty
  ap: assert property (@(posedge clk) p);
endmodule
)", "disable iff");
}

// -- Sampling (LRM 16.5, 4.4.2.1) -------------------------------------------
//
// An assertion reads sampled values, and what is sampled is the variable: one
// hidden shadow per variable, copied whole by one Preponed process. These check
// the shadow set itself, because it is what decides how much state a design pays
// for assertions and which values the monitor can possibly see.

namespace {

size_t shadow_count(const ir::Module& m) {
    size_t n = 0;
    for (const ir::SignalInfo& s : m.signals()) {
        n += s.name.starts_with("sva.shadow.") ? 1 : 0;
    }
    return n;
}

bool has_shadow(const ir::Module& m, std::string_view live) {
    const std::string want = "sva.shadow." + std::string(live);
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name == want) {
            return true;
        }
    }
    return false;
}

size_t preponed_count(const ir::Module& m) {
    size_t n = 0;
    for (const ir::Process& p : m.processes()) {
        n += p.activation == ir::Activation::Preponed ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("sampling shadows one signal per assertion variable") {
    LowerResult       r;
    // `a` is read by both the antecedent and the consequent, and NNF makes the
    // negated occurrence its own atom, so the variable reaches the shadow map
    // twice. It must still buy exactly one shadow -- the sampled thing is the
    // variable, not the atom.
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a;
  ap: assert property (@(posedge clk) a |=> !a);
endmodule
)", r);

    CHECK(shadow_count(m) == 1);
    CHECK(has_shadow(m, "a"));
    // The clock delimits cycles, it is not an operand of a boolean root, so it is
    // never sampled.
    CHECK_FALSE(has_shadow(m, "clk"));
    CHECK(preponed_count(m) == 1);
}

TEST_CASE("a shadow keeps the live variable's whole type") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk;
  logic [7:0] v;
  ap: assert property (@(posedge clk) v == 8'hA5 |=> v != 8'hA5);
endmodule
)", r);

    REQUIRE(has_shadow(m, "v"));
    const ir::SignalInfo* live   = nullptr;
    const ir::SignalInfo* shadow = nullptr;
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name == "v") {
            live = &s;
        } else if (s.name == "sva.shadow.v") {
            shadow = &s;
        }
    }
    REQUIRE(live != nullptr);
    REQUIRE(shadow != nullptr);
    // Full width, full 4-state. Narrowing the shadow to the bit the root needs
    // would evaluate the comparison at sample time instead of in Observed.
    CHECK(shadow->type == live->type);
    // Nothing drives a shadow but the sampling process: it is a variable, so it
    // neither powers up to z nor joins net resolution.
    CHECK_FALSE(shadow->is_net);
    CHECK(shadow->dir == ir::SignalInfo::Direction::Internal);
}

TEST_CASE("a disable condition is read live and buys no shadow") {
    LowerResult       r;
    // LRM 16.15: `disable iff` is deliberately not sampled, so a variable that
    // appears only there must not be shadowed.
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, rst, a, b;
  ap: assert property (@(posedge clk) disable iff (rst) a |=> b);
endmodule
)", r);

    CHECK(has_shadow(m, "a"));
    CHECK(has_shadow(m, "b"));
    CHECK_FALSE(has_shadow(m, "rst"));
    CHECK(shadow_count(m) == 2);
}

TEST_CASE("a design with no assertion pays nothing for sampling") {
    const LowerResult r = lower_text(R"(
module t;
  bit clk, a;
  always @(posedge clk) a <= !a;
endmodule
)");
    REQUIRE(r.module != nullptr);

    CHECK(shadow_count(*r.module) == 0);
    CHECK(preponed_count(*r.module) == 0);
}

TEST_CASE("the sampling process is a plain copy, with no expression in it") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk;
  logic [7:0] v, w;
  ap: assert property (@(posedge clk) v[3] |=> (v > w));
endmodule
)", r);

    REQUIRE(shadow_count(m) == 2);
    const ir::Process* p = nullptr;
    for (const ir::Process& proc : m.processes()) {
        if (proc.activation == ir::Activation::Preponed) {
            REQUIRE(p == nullptr); // one process for the whole top
            p = &proc;
        }
    }
    REQUIRE(p != nullptr);
    CHECK(p->wait_events.empty());
    CHECK_FALSE(p->self_start);

    // What is sampled is the variable, so the body is Store(shadow, Load(live))
    // and nothing else: an operator here would evaluate at sample time what LRM
    // 16.5 evaluates in Observed. `v[3]` and `v > w` contribute no bit-select and
    // no compare -- both stay in the monitor.
    size_t stores = 0;
    for (size_t i = 0; i < p->ir.inst_count(); i++) {
        const ir::Inst::Tag tag = p->ir.tag(static_cast<ir::Index>(i));
        stores += tag == ir::Inst::Tag::Store ? 1 : 0;
        CHECK((tag == ir::Inst::Tag::Store || tag == ir::Inst::Tag::Load ||
               tag == ir::Inst::Tag::Signal));
    }
    CHECK(stores == 2);
}

TEST_CASE("sampling a variable of another instance is refused") {
    // The shadow is per-instance state addressed through one scope anchor, so a
    // variable the asserting instance does not declare cannot be sampled yet.
    refuses(R"(
module sub;
  bit a;
endmodule
module t;
  bit clk;
  sub s();
  ap: assert property (@(posedge clk) s.a |=> s.a);
endmodule
)", "may only sample variables of the instance that declares it");
}

TEST_CASE("sampling a variable with no packed value is refused") {
    // The copy is one whole-signal Store, so an unpacked aggregate (LRM 16.6
    // names dynamic arrays, queues and associative arrays explicitly) would need
    // a per-leaf copy clotho does not emit.
    refuses(R"(
module t;
  bit clk;
  bit m [0:3];
  ap: assert property (@(posedge clk) m[0] |=> m[1]);
endmodule
)", "may only sample a variable with a packed value");

    refuses(R"(
module t;
  bit clk;
  string s;
  ap: assert property (@(posedge clk) (s == "x") |=> (s != "x"));
endmodule
)", "may only sample a variable with a packed value");
}

// -- Sequence repetition and combinators (LRM 16.9) --------------------------
//
// libsva ASSERTS its repetition preconditions rather than reporting them, so a
// shape that slips past the frontend aborts the compiler instead of producing a
// diagnostic. Several of the guards below are unreachable today because slang
// refuses the same shape first; they are still tested, because what must never
// happen is the operand pair reaching sva_seq_repeat, and each test records
// which layer is actually doing the refusing.

TEST_CASE("consecutive repetition lowers to one obligation state per cycle") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b[*2]);
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    const ir::AssertionMonitor& mon = m.monitors[0];

    CHECK(mon.has_trigger);
    // Glushkov gives one state per letter OCCURRENCE, and `b[*2]` is two
    // occurrences of the same letter rather than one state visited twice.
    CHECK(mon.obligation.state_count == 2);
    // Two cycles of longest match is two attempts that can be pending at once.
    CHECK(mon.depth == 2);
}

TEST_CASE("a bounded repetition range lowers, and an unbounded one does not") {
    LowerResult r;
    lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) a |=> b[*1:3] ##1 c);
endmodule
)", r);
    CHECK(r.ok);

    // `[*1:$]` in a consequent has no bounded longest match, so the number of
    // concurrent obligations is unbounded. This is sva_lower reporting, not a
    // frontend guard -- the IR is perfectly well formed.
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b[*1:$]);
endmodule
)", "no bounded longest match");
}

TEST_CASE("sequence or lowers to the union of both branches") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) a |=> (b ##1 c) or (c ##1 b));
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    // A disjunction is the disjoint union of the two fragments, so two letters
    // per branch is four states: `or` does not merge the shared alphabet.
    CHECK(m.monitors[0].obligation.state_count == 4);
}

TEST_CASE("sequence intersect lowers over both operands") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) a |=> (b ##1 c) intersect (b[*2]));
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    // Both operands span the same two cycles, so the obligation's longest match
    // is two cycles -- intersect constrains the span, it does not extend it.
    CHECK(m.monitors[0].depth == 2);
}

TEST_CASE("goto and non-consecutive repetition lower in an antecedent") {
    // The trigger side is existential, so an unbounded match is free there.
    LowerResult r;
    lower_for_monitor(R"(
module t;
  bit clk, b, c, rst;
  ap: assert property (@(posedge clk) (rst ##1 b[->2]) |=> c);
endmodule
)", r);
    CHECK(r.ok);

    lower_for_monitor(R"(
module t;
  bit clk, b, c, rst;
  ap: assert property (@(posedge clk) (rst ##1 b[=2]) |=> c);
endmodule
)", r);
    CHECK(r.ok);
}

TEST_CASE("a repetition with a zero lower bound is refused") {
    // libsva's seqRepeat asserts `lo >= 1`: the IR has no empty match, and
    // removing one is a rewrite of whatever ENCLOSES the repetition, not of the
    // repetition itself. The LRM only bans an empty-admitting sequence in a few
    // positions, so slang passes both of these through and clotho's guard is
    // the only thing standing between them and an aborted compiler.
    refuses(R"(
module t;
  bit clk, a, c;
  ap: assert property (@(posedge clk) a[*0:2] |=> c);
endmodule
)", "zero lower bound admits the empty sequence");

    // Reached through a concatenation rather than directly as an antecedent.
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a[*0:2] ##1 b) |=> c);
endmodule
)", "zero lower bound admits the empty sequence");

    // `[=0:n]` and `[->0]` are the same assertion for the other two flavors.
    refuses(R"(
module t;
  bit clk, b, c, rst;
  ap: assert property (@(posedge clk) (rst ##1 b[=0:2]) |=> c);
endmodule
)", "zero lower bound admits the empty sequence");
}

TEST_CASE("a reversed repetition range never reaches libsva") {
    // libsva's seqRepeat asserts `lo <= hi`. slang rejects the reversed range
    // while binding, so clotho's own guard is unreachable through source today;
    // this pins the fact that SOMETHING refuses it before the builder runs.
    refuses(R"(
module t;
  bit clk, a, c;
  ap: assert property (@(posedge clk) a[*3:1] |=> c);
endmodule
)", "greater than specified maximum");
}

TEST_CASE("goto repetition over a compound sequence never reaches libsva") {
    // seqRepeat asserts a seq_boolean operand for goto and non-consecutive
    // repetition: both are defined by the cycles in which the operand does NOT
    // hold, which needs its negation, and "not necessarily back to back" has no
    // reading for an operand whose own matches can overlap. slang refuses the
    // shape outright (SeqInstanceRepetition), for a parenthesized sequence...
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a ##1 b)[->2] |=> c);
endmodule
)", "non-consecutive or go-to repetition");

    // ...and for a named one, which is the other way a compound operand could
    // reach sva_seq_repeat now that named sequences expand inline.
    refuses(R"(
module t;
  bit clk, a, b, c;
  sequence s;
    a ##1 b;
  endsequence
  ap: assert property (@(posedge clk) s[=2] |=> c);
endmodule
)", "non-consecutive or go-to repetition");
}

TEST_CASE("a goto repetition in a consequent is refused") {
    // Legal SystemVerilog, and a well formed IR -- but `b[->2]` has no bounded
    // longest match, so the obligations it can leave pending are unbounded.
    // sva_lower reports this one; it is not a frontend guard.
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b[->2]);
endmodule
)", "no bounded longest match");

    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b[=2]);
endmodule
)", "no bounded longest match");
}

TEST_CASE("throughout lowers its left operand as a boolean, not a sequence") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, en, b, c;
  ap: assert property (@(posedge clk) a |=> (en throughout (b ##1 c)));
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    // `en` is conjoined into every state of the right operand's automaton
    // rather than matching alongside it, so it costs no state and no cycle:
    // the span is the two cycles of `b ##1 c` alone. Lowering it through
    // sequence() instead would have added both.
    CHECK(m.monitors[0].obligation.state_count == 2);
    CHECK(m.monitors[0].depth == 2);
}

TEST_CASE("sequence and spans to the later of its two ends") {
    // `depth` is the obligation's longest match, so an `and` over operands of
    // different lengths shows directly which end the pair takes.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b, c, d;
  ap: assert property (@(posedge clk) a |=> ((b ##1 c) and d));
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.monitors[0].depth == 2);

    LowerResult       longer;
    const ir::Module& n = lower_for_monitor(R"(
module t;
  bit clk, a, b, c, d, e;
  ap: assert property (@(posedge clk) a |=> ((b ##1 c ##1 e) and d));
endmodule
)", longer);
    REQUIRE(longer.ok);
    REQUIRE(n.monitors.size() == 1);
    // Three cycles against one: the pair takes the longer side. tests/sva/
    // diff_and.sv holds the other half of this, that the trailing cycles are
    // really waited for and not merely counted.
    CHECK(n.monitors[0].depth == 3);
}

TEST_CASE("within lowers to its padded intersection") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b, c, d, e;
  ap: assert property (@(posedge clk) a |=> ((b ##1 c) within (d ##3 e)));
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    // libsva builds `(true ##[0:$] inner ##[0:$] true) intersect outer`. The
    // padding is unbounded on both sides, so it is the intersection that bounds
    // the pair, at outer's four cycles.
    CHECK(m.monitors[0].depth == 4);
}

TEST_CASE("the left operand of throughout must be a boolean") {
    // sva_seq_throughout takes a BOOLEAN node there and asserts it rather than
    // reporting it, so a sequence must never reach the builder. slang refuses
    // every spelling itself (ThroughoutLhsInvalid), which is the diagnostic a
    // user sees; clotho's own guard behind it is unreachable through source
    // today. This pins that SOMETHING refuses each of them before a node exists.
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) ((a ##1 b) throughout (c ##1 c)) |=> c);
endmodule
)", "must be a simple expression");

    // A repetition makes the operand a sequence too...
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) (a[*2] throughout (b ##1 c)) |=> c);
endmodule
)", "must be a simple expression");

    // ...and so does a named sequence, which is the other way a compound
    // operand could reach the builder now that named sequences expand inline.
    refuses(R"(
module t;
  bit clk, a, b, c;
  sequence s;
    a ##1 b;
  endsequence
  ap: assert property (@(posedge clk) (s throughout (b ##1 c)) |=> c);
endmodule
)", "must be a simple expression");
}

TEST_CASE("an unbounded and in a consequent names the and/intersect asymmetry") {
    // Identical operands under the two operators, so only the operator differs.
    //
    // `and` runs to the later of its two ends, so ONE unbounded operand leaves
    // the pair unbounded and the obligation untrackable. A user told only "no
    // bounded longest match" would not guess that is the whole problem, so the
    // refusal spells the rule out.
    const LowerResult bad = lower_text(R"(
module t;
  bit clk, req, ack, done, rdy, go;
  ap: assert property (@(posedge clk) req |=> ((ack ##[1:$] done) and (rdy ##[1:2] go)));
endmodule
)");
    CHECK_FALSE(bad.ok);
    const std::string text = bad.diagnostic_text();
    CHECK(text.find("no bounded longest match") != std::string::npos);
    CHECK_MESSAGE(text.find("BOTH operands need a bounded match") != std::string::npos,
                  "the and/intersect asymmetry is unguessable, so it has to be named: ", text);

    // The other half, and the reason the message has to name it: an
    // intersection's ends COINCIDE, so the bounded operand bounds the pair and
    // the very same shape is accepted.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, req, ack, done, rdy, go;
  ap: assert property (@(posedge clk) req |=> ((ack ##[1:$] done) intersect (rdy ##[1:2] go)));
endmodule
)", r);
    REQUIRE(r.ok);
    REQUIRE(m.monitors.size() == 1);
    // Three cycles, from the bounded side alone.
    CHECK(m.monitors[0].depth == 3);

    // ...and nothing else picked up the explanation on the way past: an
    // unbounded consequent with no `and` in it keeps the plain refusal.
    const LowerResult plain = lower_text(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b[->2]);
endmodule
)");
    CHECK_FALSE(plain.ok);
    CHECK(plain.diagnostic_text().find("BOTH operands") == std::string::npos);
}

namespace {

// JIT-run a design and capture stdout AND stderr into one stream, in the order
// they were written (mirrors program_test.cpp's helper, which needs stdout
// alone).
//
// Both are needed, interleaved: $display goes to stdout and an assertion's
// default report to stderr, and it is their ORDER that attributes a report to a
// clock tick. Both are switched to unbuffered for the duration, because a stdout
// redirected to a file is fully buffered by default and would otherwise flush
// after every report rather than between them.
std::string sva_run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stdout));
    dup2(fileno(tmp), fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

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

std::string report_times(std::string_view source) {
    const std::string out = sva_run_capture(source);
    std::string       times;
    std::string       tick;

    size_t pos = 0;
    while (pos < out.size()) {
        const size_t eol  = out.find('\n', pos);
        const auto   line = std::string_view(out).substr(
          pos, (eol == std::string::npos ? out.size() : eol) - pos);

        if (line.starts_with("[")) {
            const size_t close = line.find(']');
            if (close != std::string_view::npos) {
                tick = std::string(line.substr(1, close - 1));
            }
        } else if (line.find("assertion failed") != std::string_view::npos) {
            times += times.empty() ? "" : " ";
            times += tick;
        }

        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 1;
    }
    return times;
}

// How many history ring entries the module holds for `live`. Ring signals are
// named `sva.<monitor>.past.<variable>.<index>`.
size_t ring_count(const ir::Module& m, std::string_view live) {
    const std::string needle = ".past." + std::string(live) + ".";
    size_t            n      = 0;
    for (const ir::SignalInfo& s : m.signals()) {
        n += s.name.find(needle) != std::string::npos ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("history advances per clock tick, not per time step") {
    // THE case. `noise` toggles every 3 with the clock at every 5, so most time
    // steps carry no clocking edge -- and the sample taken at those steps must
    // never reach the ring.
    //
    // v is 0 before t=5, 1 from t=5, 2 from t=15. Sampling at a step captures the
    // value the previous step ended with, so the per-TICK samples are t5=0, t15=1,
    // t25=2, t35=2 and `$past(v) != v` fails only where a sample repeats its
    // predecessor: the first tick (2-state default 0 against a sample of 0) and
    // t=35 (2 against 2).
    //
    // Shift per time step instead and $past(v) at t=15 becomes the sample from
    // t=12, which is already 1, so the tick reports; likewise t=25 from t=24.
    // That mutation therefore reads `5 15 25 35`, and this is the check that
    // separates them.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit noise = 0;
  always #3 noise = ~noise;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $past(v) != v);
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 2;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 35");
}

TEST_CASE("the ring is read before it is shifted, so $past is the previous tick") {
    // The off-by-one guard. Shifting ahead of the roots instead of after them
    // would make ring[0] this tick's own shadow, so `$past(v) != v` would be false
    // at every tick and the report set would be every tick rather than two of
    // them. Same trace as above without the extra time steps, so the two failure
    // modes stay distinguishable.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $past(v) != v);
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 2;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 35");
}

TEST_CASE("$past(v, n) reads n ticks back") {
    // Samples per tick are t5=0, t15=1, t25=2, t35=1, t45=5, t55=5, so two ticks
    // back repeats the current sample at t=35 (1 against 1) and, at the first
    // tick, compares the 2-state default 0 against a sample of 0.
    //
    // The neighbours are deliberately different sets: reading ring[0] instead
    // would report `5 55`, and ring[2] would report `5` alone. Nothing here
    // survives an off-by-one in the slot index.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $past(v, 2) != v);
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 2;
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 5;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 35");
}

TEST_CASE("a deep ring shifts without smearing one entry through the rest") {
    // Depth 3, because depth 2 cannot tell a correct shift from a broken one: the
    // copy loop runs a single iteration there, so ascending and descending order
    // agree. Only from three entries up does copying in the wrong direction
    // overwrite ring[1] before ring[2] reads it, which would leave every entry
    // above the first holding the same value.
    //
    // Samples are t5=0, t15=1, t25=2, t35=3, t45=1, t55=9, so three ticks back
    // repeats the current sample at t=45 (1 against 1), and at t=5 the seed of 0
    // meets a sample of 0. Copy the ring upwards instead and $past(v, 3) at t=45
    // becomes 2 rather than 1, which passes, leaving "5" alone.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $past(v, 3) != v);
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 2;
    @(posedge clk); v <= 3;
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 9;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 45");
}

TEST_CASE("a ring is as deep as the largest $past over that variable") {
    // Depth is a compile-time constant: the largest n any use asks for, with the
    // entries shared rather than one ring per call site.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk;
  bit [3:0] v;
  bit       b;
  ap: assert property (@(posedge clk) $past(v) != v && $past(v, 3) != v && $past(b) != b);
endmodule
)", r);

    CHECK(ring_count(m, "v") == 3);
    CHECK(ring_count(m, "b") == 1);
}

TEST_CASE("$sampled buys no ring entry") {
    // $sampled is the shadow, not history, so it must not deepen a ring that
    // nothing shifts on its behalf.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk;
  bit [3:0] v;
  ap: assert property (@(posedge clk) $sampled(v) == v);
endmodule
)", r);

    CHECK(has_shadow(m, "v"));
    CHECK(ring_count(m, "v") == 0);
}

TEST_CASE("a ring entry keeps the live variable's whole type") {
    // Same rule as the shadow: a collapsed bit could answer neither $past(bus, 2)
    // nor a whole-value $stable.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk;
  logic [7:0] v;
  ap: assert property (@(posedge clk) $past(v) != v);
endmodule
)", r);

    const ir::SignalInfo* live = nullptr;
    const ir::SignalInfo* ring = nullptr;
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name == "v") {
            live = &s;
        } else if (s.name.find(".past.v.0") != std::string::npos) {
            ring = &s;
        }
    }
    REQUIRE(live != nullptr);
    REQUIRE(ring != nullptr);
    CHECK(ring->type == live->type);
}

TEST_CASE("$stable and $changed compare the whole sampled value") {
    // Not the LSB: v changes 4'b0001 -> 4'b1001 between the second and third
    // ticks, which only a whole-value comparison sees.
    //
    // Samples are t5=0, t15=1, t25=9, t35=9. $changed therefore holds at t=15 and
    // t=25 and not at t=5 (default 0 against 0) or t=35, so asserting $changed
    // reports at 5 and 35.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $changed(v));
  initial begin
    @(posedge clk); v <= 4'b0001;
    @(posedge clk); v <= 4'b1001;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 35");

    // $stable is the exact complement, so the same trace inverts the report set.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $stable(v));
  initial begin
    @(posedge clk); v <= 4'b0001;
    @(posedge clk); v <= 4'b1001;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "15 25");
}

TEST_CASE("$rose and $fell read the LSB, not the whole value") {
    // v goes 0 -> 4'b0010 -> 4'b0011 -> 4'b0010. Bit 0 therefore rises once, at
    // the tick that samples 4'b0011, even though the value changes at every tick.
    //
    // Samples: t5=0, t15=2, t25=3, t35=2. $rose holds only at t=25, so asserting
    // it reports at 5, 15 and 35.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $rose(v));
  initial begin
    @(posedge clk); v <= 4'b0010;
    @(posedge clk); v <= 4'b0011;
    @(posedge clk); v <= 4'b0010;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 15 35");

    // $fell on the same trace holds only where bit 0 goes back down, at t=35.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $fell(v));
  initial begin
    @(posedge clk); v <= 4'b0010;
    @(posedge clk); v <= 4'b0011;
    @(posedge clk); v <= 4'b0010;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 15 25");
}

TEST_CASE("a transition out of x is an edge, per the 4-state edge order") {
    // LRM 16.9.3 puts $rose on the same 0 < x/z < 1 order the simulator uses for
    // @(posedge), so x -> 1 IS a rise and 1 -> x is not. v is 4-state and starts
    // at x, so the tick that samples 1 rises out of the unknown.
    //
    // Samples: t5=x, t15=1, t25=1. $rose holds only at t=15 -- t=5 compares the X
    // default against x, which is no movement at all.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  logic v;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $rose(v));
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 25");
}

TEST_CASE("$sampled is this tick's value and never history") {
    // $sampled(v) == v is a tautology inside an assertion, because both sides are
    // the same shadow. It reports at every tick if $sampled ever resolves to a
    // ring entry, and at none if it stays the shadow.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $sampled(v) == v);
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 2;
    @(posedge clk); $finish;
  end
endmodule
)").empty());
}

TEST_CASE("history keeps advancing while the assertion is disabled") {
    // LRM 16.9.3 counts ticks on which the clocking event occurred, and a
    // `disable iff` does not stop the clock. The ring is therefore gated on the
    // edge alone, exactly like the monitor's own clkprev.
    //
    // Samples are t5=0, t15=1, t25=2, t35=2, and rst is live high only for the
    // t=25 tick. With the ring shifting through the disabled tick, $past at t=35
    // is the t=25 sample of 2, so `$past(v) != v` is false and t=35 reports.
    // Freeze the ring while disabled instead and $past at t=35 would still be the
    // t=15 sample of 1, which differs from 2, so the t=35 report would disappear
    // and this would read "5" alone.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  bit rst = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) disable iff (rst) $past(v) != v);
  initial begin
    @(posedge clk);
    v <= 1;
    @(posedge clk);
    v <= 2;
    @(posedge clk); rst <= 1;
    @(posedge clk); rst <= 0;
    @(posedge clk); $finish;
  end
endmodule
)") == "5 35");
}

TEST_CASE("a ring entry is seeded with the variable's time-zero value") {
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  logic [3:0] v = 4'd7;
  initial #2 v = 4'd3;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $past(v, 2) == 4'd7);
  initial begin
    @(posedge clk); v <= 4'd1;
    @(posedge clk); v <= 4'd2;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "25 35");
}

// -- Sampled value function refusals ----------------------------------------

TEST_CASE("a gating expression on $past is refused") {
    refuses(R"(
module t;
  bit clk, a, g, b;
  ap: assert property (@(posedge clk) $past(a, 1, g) |=> b);
endmodule
)", "gating expression on $past");
}

TEST_CASE("a non-constant tick count on $past is refused") {
    // The ring is sized at compile time, so the count has to be constant. slang
    // binds the argument in a constant context and so refuses this one before
    // lowering sees it; clotho keeps its own guard behind that, because the ring
    // depth is clotho's invariant and not slang's to maintain.
    refuses(R"(
module t;
  bit clk, a, b;
  int n;
  ap: assert property (@(posedge clk) $past(a, n) |=> b);
endmodule
)", "non-constant");
}

TEST_CASE("a tick count below one on $past is refused") {
    refuses(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) $past(a, 0) |=> b);
endmodule
)", "greater than or equal to 1");
}

TEST_CASE("an explicit clocking argument on a sampled value function is refused") {
    // A clocking argument gives the function its own tick stream, independent of
    // the assertion's: a second ring shifted by a second edge.
    refuses(R"(
module t;
  bit clk, clk2, a, b;
  ap: assert property (@(posedge clk) $rose(a, @(posedge clk2)) |=> b);
endmodule
)", "explicit clocking argument");

    refuses(R"(
module t;
  bit clk, clk2, a, b;
  ap: assert property (@(posedge clk) $past(a, 1, , @(posedge clk2)) |=> b);
endmodule
)", "explicit clocking argument");
}

TEST_CASE("a sampled value function over an expression is refused") {
    // The shadow and the ring are both keyed by variable (LRM 16.6 captures
    // operands, not expressions), so there is nothing to hang an expression's
    // history on.
    refuses(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) $rose(a && b) |=> c);
endmodule
)", "plain variable reference");
}

TEST_CASE("a sampled value function outside an assertion is refused") {
    // Legal in procedural code per LRM 16.9.3, but nothing samples there: no
    // clocking event means no tick for history to advance on.
    refuses(R"(
module t;
  bit clk, a;
  always @(posedge clk) if ($rose(a)) $display("edge");
endmodule
)", "only supported inside a concurrent assertion expression");
}

TEST_CASE("a sampled value function in a disable iff condition is refused") {
    // LRM 16.15 evaluates the disable condition from LIVE values, so there is no
    // sampled value underneath it for these to be built from.
    refuses(R"(
module t;
  bit clk, a, b, rst;
  ap: assert property (@(posedge clk) disable iff ($rose(rst)) a |=> b);
endmodule
)", "disable iff");
}
