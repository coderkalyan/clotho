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

// The counted twin of the above: the smallest counted monitor that HAS a trigger.
// Its antecedent matches in exactly one cycle, which is the shape the counted
// attempt ids must cost nothing on.
constexpr std::string_view counted_implication = R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> s_eventually b);
endmodule
)";

// A monitor's emitted shape, as a string: its hidden signals by name, then the
// instruction TAG SEQUENCE of each process it owns plus that process's extra
// side-table, folded into one digest.
//
// This is the guard on refactors of the step. The verdict lattice
// (ir::AssertionVerdict) was named by pulling the three masks the ring path used
// to compute inline into one helper, and the whole point of doing it that way was
// that the emitted IR must not move: the ring's semantics are pinned by the
// behavioural cases below and by libsva's own differential tests, so any change
// here is a regression by definition rather than something to re-bless. Tags and
// the extra table rather than payload words: those two capture emission ORDER and
// region structure, which is what a reordered fold would disturb, and they carry
// no union padding to make the digest depend on the build.
std::string monitor_shape(const ir::Module& m) {
    std::string out;
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name.find("sva.") != std::string::npos) {
            out += s.name;
            out += ' ';
        }
    }
    for (const ir::Process& p : m.processes()) {
        if (p.activation != ir::Activation::Observed && p.activation != ir::Activation::Final) {
            continue;
        }
        // FNV-1a over the tag sequence and the extra table. A digest and not the
        // sequence itself only because the sequence is thousands of entries long;
        // it is byte-exact either way.
        uint64_t h = 0xcbf29ce484222325ull;
        const auto eat = [&h](uint64_t word) {
            h = (h ^ word) * 0x100000001b3ull;
        };
        eat(p.ir.inst_count());
        for (uint32_t i = 0; i < p.ir.inst_count(); ++i) {
            eat(static_cast<uint64_t>(p.ir.tag(ir::Index{ i })));
        }
        for (const uint32_t word : p.ir.extra_raw()) {
            eat(word);
        }
        out += p.activation == ir::Activation::Observed ? "step:" : "final:";
        out += std::to_string(p.ir.inst_count());
        out += ':';
        out += std::to_string(h);
        out += ' ';
    }

    return out;
}

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

TEST_CASE("naming the verdict lattice leaves a ring monitor's emitted IR alone") {
    // The acceptance test for the lattice refactor. `a |=> b` is the smallest ring
    // monitor there is -- one trigger state, one obligation state, depth 1 -- so its
    // step is almost entirely the verdict fold that moved, and its digest is the most
    // sensitive one available.
    //
    // Pinned as a literal rather than compared against a second lowering: the claim is
    // that pulling the ring's three verdict masks into ring_verdict() added no
    // instruction and reordered none, and only a constant can say that. It was
    // established by measuring this digest, reverting the helper back to the inline
    // fold, rebuilding, and getting the same number -- not by reading the diff.
    //
    // So if this fires, re-establish it the same way rather than pasting the new
    // digest in. THREE things legitimately move it: a change to the step's emission
    // order, which is the regression this exists to catch; a change to libsva's
    // tables for this property, which reshuffles the guard fold without clotho
    // touching anything; and a change to `ir::Inst::Tag`'s DECLARATION ORDER, which
    // renumbers every tag after the insertion point and so rehashes a step that is
    // otherwise identical instruction for instruction. The COUNT separates the first
    // from the other two -- it is stable across them and not across it.
    //
    // The third one is not hypothetical: "disable: lower a lexically
    // enclosing target (LRM 9.6.2)" inserted NamedBlock and Disable mid-enum and moved both
    // digests in this file. Confirmed by moving those two enumerators to the end of
    // the enum, rebuilding, and watching both literals below match their previous
    // values again -- which is the same style of experiment as the one above, run
    // against the renumbering instead of against the refactor.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(implication, r);

    REQUIRE(m.monitors.size() == 1);
    REQUIRE(m.monitors[0].tracking == ir::AssertionTracking::Ring);
    CHECK(monitor_shape(m) ==
          "sva.shadow.a sva.shadow.b sva.0.trig sva.0.fired sva.0.pend0 sva.0.occ sva.0.clk "
          "step:125:8656654477628751033 ");
}

TEST_CASE("counting a trigger's attempts leaves a single-cycle antecedent alone") {
    // The zero-cost half of the counted-trigger fix. Several antecedent matches can
    // end in one cycle, each owing a verdict of its own, so a seed adds the NUMBER of
    // attempts that matched rather than 1 -- and an antecedent that matches in a
    // single cycle can never have more than one, so it must pay nothing for the
    // arithmetic. `a |=> s_eventually b` is the smallest counted monitor with a
    // trigger, so it is where that costing is visible.
    //
    // Two claims, and they take different evidence. That no id STATE is allocated is
    // the signal list, and the absent `tid`/`fid`/`idn` names below are legible on
    // their own. That no INSTRUCTION was added is the digest, which only says so
    // against a number established the way the case above describes -- so if this
    // fires, re-establish it that way rather than pasting the new digest in.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(counted_implication, r);

    REQUIRE(m.monitors.size() == 1);
    REQUIRE(m.monitors[0].tracking == ir::AssertionTracking::Counted);
    CHECK(monitor_shape(m) ==
          "sva.shadow.a sva.shadow.b sva.0.trig sva.0.fired sva.0.cnt1 sva.0.clk "
          "step:117:1903636621696783129 final:18:14459492915158645094 ");
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

TEST_CASE("a restrict is lowered in full and then says nothing") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  // Unlabelled: slang's module-item parser accepts a label only ahead of
  // assert / assume / cover, so `rp: restrict ...` is a parse error there.
  restrict property (@(posedge clk) a |=> b);
endmodule
)", r);
    CHECK(r.ok);

    // Lowered in full: `restrict` constrains a FORMAL tool's input space and the
    // LRM gives it no simulation verdict, but that is a reason to emit no report,
    // not a reason to drop the property. The monitor exists, so the temporal
    // expression was really bound and anything libsva cannot handle inside one is
    // still refused rather than waved through.
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.monitors[0].kind == ir::AssertionKind::Restrict);
    CHECK(m.monitors[0].has_trigger);

    // And then silent: no verdict pended into Reactive, no report inline, and no
    // tally -- the three things the other kinds do.
    CHECK_FALSE(has_tag(monitor_process(m), ir::Inst::Tag::ReactivePend));
    CHECK_FALSE(has_tag(monitor_process(m), ir::Inst::Tag::Severity));
    CHECK(find_signal(m, "sva.0.cov") == nullptr);
    CHECK(activation_count(m, ir::Activation::Final) == 0);
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

// The single monitor in a lowered module, for the shape checks below.
const ir::AssertionMonitor& only_monitor(const ir::Module& m) {
    REQUIRE(m.monitors.size() == 1);
    return m.monitors[0];
}

// One assertion body, wrapped in the smallest module that gives it a clock.
std::string one_assertion(std::string_view decls, std::string_view body) {
    return std::string("module t;\n  bit clk;\n  ") + std::string(decls) +
           "\n  ap: assert property (@(posedge clk) " + std::string(body) + ");\nendmodule\n";
}

} // namespace

TEST_CASE("an unbounded consequent under a trigger lowers, counted") {
    // `##[1:$]` in a consequent has no bounded longest match, so no ring can be
    // sized for it -- but a TRIGGER above it is existential, one state vector for
    // all of its attempts, and needs no bound of its own. The two therefore
    // compose: the antecedent stays a trigger and the obligation is counted.
    //
    // This is the shape phase 3 exists to unblock; it was refused outright before.
    // The mode mix that is still refused is an implication INSIDE an unbounded
    // operator, which is a different arrangement -- see the mode-mix case below.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> ##[1:$] b);
endmodule
)", r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    const ir::AssertionMonitor& mon = m.monitors.at(0);
    CHECK(mon.tracking == ir::AssertionTracking::Counted);
    CHECK(mon.has_trigger);
    // The trigger keeps its own state; the obligation has counters and no pool.
    CHECK(find_signal(m, "sva.0.trig") != nullptr);
    CHECK(find_signal(m, "sva.0.fired") != nullptr);
    CHECK(find_signal(m, "sva.0.pend0") == nullptr);
    CHECK(find_signal(m, "sva.0.occ") == nullptr);
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

TEST_CASE("first_match determinises its operand, in the position where it counts") {
    // `first_match` was the last sequence-layer operator of LRM 16.9 without a
    // libsva tag, and `implies` was the last one at the property layer, so there is
    // no longer a "no tag" case to test at either layer. What is left is `iff`, which
    // has a tag and is refused for a reason of its own; see the negation section.
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

TEST_CASE("a sequence method is refused by name, not as a stray function call") {
    // `.triggered` and `.matched` are calls only syntactically. Both report on the
    // attempts of the NAMED SEQUENCE they are called on, which would need that
    // sequence stepped as an automaton of its own -- nothing to do with the
    // sampling rule the generic call refusal is about. A reader told "a function
    // call ... would not be sampled" goes looking for the wrong thing.
    constexpr std::string_view triggered = R"(
module t;
  bit clk, a, b, c;
  sequence sab;
    a ##1 b;
  endsequence
  ap: assert property (@(posedge clk) sab.triggered |=> c);
endmodule
)";
    refuses(triggered, "`.triggered` on a named sequence");

    // `.matched` is the same method across a clock boundary; slang rejects it
    // outside an assertion expression, so this is the only place it can appear.
    refuses(R"(
module t;
  bit clk, a, b, c;
  sequence sab;
    a ##1 b;
  endsequence
  ap: assert property (@(posedge clk) sab.matched |=> c);
endmodule
)", "`.matched` on a named sequence");

    // Outside an assertion expression the same call reaches the system-function
    // tail instead, which would have named no feature at all.
    refuses(R"(
module t;
  bit clk, a, b, hit;
  sequence sab;
    a ##1 b;
  endsequence
  always @(posedge clk) hit <= sab.triggered;
endmodule
)", "`.triggered` on a named sequence");
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

TEST_CASE("a default clocking supplies the clock of an unclocked assertion") {
    // The last rule of the precedence chain (LRM 16.16: explicit > contextually
    // inferred > default clocking). With no explicit clock and no candidate event in
    // the procedure, LRM 16.14.6 sends the clock to the scope's `default clocking`,
    // and 16.16(a) says it is "treated as though the default clocking event had been
    // written explicitly as the leading clocking event" -- so the monitor must be
    // indistinguishable from the explicitly clocked one, not merely present.
    LowerResult       def_r;
    const ir::Module& def = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  default clocking cb @(posedge clk); endclocking
  ap: assert property (a |=> b);
endmodule
)", def_r);
    LowerResult       explicit_r;
    const ir::Module& expl = lower_for_monitor(implication, explicit_r);
    REQUIRE_MESSAGE(def_r.ok, def_r.diagnostic_text());
    REQUIRE(explicit_r.ok);
    REQUIRE(def.monitors.size() == 1);
    REQUIRE(expl.monitors.size() == 1);

    const ir::AssertionMonitor& d = def.monitors[0];
    const ir::AssertionMonitor& e = expl.monitors[0];
    CHECK(def.signal(d.clock).name == expl.signal(e.clock).name);
    CHECK(d.edge == e.edge);
    CHECK(d.kind == e.kind);
    CHECK(d.depth == e.depth);
    CHECK(d.obligation.state_count == e.obligation.state_count);
}

TEST_CASE("a default clocking on the negedge clocks the assertion there") {
    // The event is taken from the block, so its edge is too.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  default clocking cb @(negedge clk); endclocking
  ap: assert property (a |=> b);
endmodule
)", r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(m.monitors.size() == 1);
    CHECK(m.monitors[0].edge == ir::Edge::Negative);
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

TEST_CASE("two disable iff clauses in one assertion are refused") {
    // LRM 16.12: "Nesting of disable iff clauses, explicitly or through property
    // instantiations, is not allowed". So there is no precedence to define between
    // a clause on the assertion and one in the property it instantiates, and
    // clotho defines none -- it refuses the pair rather than picking a winner,
    // because either choice would be a reset semantics clotho invented.
    //
    // slang gets here first (diag::NestedDisableIff), which is why the needle is
    // the wording both share. clotho keeps its own guard behind that, because
    // which node became the reset is clotho's invariant and not slang's to
    // maintain.
    refuses(R"(
module t;
  bit clk, rst, halt, a, b;
  property p;
    disable iff (rst) a |=> b;
  endproperty
  ap: assert property (@(posedge clk) disable iff (halt) p);
endmodule
)", "disable iff");

    // The other half of the same rule (16.12.1): an instance used as an operand of
    // a property-building operator may not carry a clause. The reset would belong
    // to that operand alone, so hoisting it would silently widen it to the whole
    // assertion -- body_disable therefore does not descend here, and the clause
    // reaches sva::lower's DisableIff arm with nothing hoisted to match it.
    refuses(R"(
module t;
  bit clk, rst, a, b, c;
  property p;
    disable iff (rst) a |=> b;
  endproperty
  ap: assert property (@(posedge clk) p and c);
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

TEST_CASE("a repetition range lowers bounded on the ring and unbounded counted") {
    LowerResult r;
    lower_for_monitor(R"(
module t;
  bit clk, a, b, c;
  ap: assert property (@(posedge clk) a |=> b[*1:3] ##1 c);
endmodule
)", r);
    CHECK(r.ok);

    // `[*1:$]` in a consequent has no bounded longest match, so no ring can be
    // sized -- but a trigger above it needs no bound of its own, so the pair lowers
    // with a ring-free trigger over a counted obligation. It was refused before
    // phase 3.
    LowerResult       open;
    const ir::Module& om = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a |=> b[*1:$]);
endmodule
)", open);
    REQUIRE_MESSAGE(open.ok, open.diagnostic_text());
    CHECK(only_monitor(om).tracking == ir::AssertionTracking::Counted);
    // The bounded one above stays on the ring; only the open bound moves.
    CHECK(only_monitor(*r.module).tracking == ir::AssertionTracking::Ring);
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

TEST_CASE("goto and non-consecutive repetition in an antecedent are refused") {
    // Both have an unbounded longest match, and on the LEFT of an implication that is
    // a refusal rather than the free ride an existential trigger looks like it should
    // give. LRM 16.14.1 owes an evaluation of the consequent per antecedent MATCH, so
    // a cycle several matches end in owes several verdicts -- and an unbounded
    // antecedent has unboundedly many attempts that could end a match there, so no
    // fixed ring of attempt ids tells them apart. Answering with the
    // count of matching CYCLES instead is what one bit gives and is short by however
    // many attempts ended together, which is what clotho used to do here.
    const std::string_view goto_left = R"(
module t;
  bit clk, b, c, rst;
  ap: assert property (@(posedge clk) (rst ##1 b[->2]) |=> c);
endmodule
)";
    refuses(goto_left, "no bounded longest match");
    // And the refusal names the REMEDY, not just the problem. This is legal
    // SystemVerilog that clotho used to accept, so a message saying only what is wrong
    // earns the same question from every user who meets it.
    refuses(goto_left, "Bound the antecedent");
    refuses(R"(
module t;
  bit clk, b, c, rst;
  ap: assert property (@(posedge clk) (rst ##1 b[=2]) |=> c);
endmodule
)", "no bounded longest match");

    // The CONSEQUENT side is untouched, and saying so is what keeps the refusal above
    // about the antecedent rather than about the operators: an unbounded obligation is
    // exactly what the counted discipline exists for, and the same two lower there.
    LowerResult r;
    lower_for_monitor(R"(
module t;
  bit clk, b, rst;
  ap: assert property (@(posedge clk) rst |=> b[->2]);
endmodule
)", r);
    CHECK(r.ok);

    lower_for_monitor(R"(
module t;
  bit clk, b, rst;
  ap: assert property (@(posedge clk) rst |=> b[=2]);
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

TEST_CASE("a goto repetition in a consequent lowers, counted") {
    // `b[->2]` and `b[=2]` have no bounded longest match, so the obligations they
    // leave pending are unbounded and no ring can be sized. Both were refused
    // before phase 3; both are now tracked by counting, with the antecedent still
    // an ordinary trigger above them.
    for (const std::string_view rep : { "b[->2]", "b[=2]" }) {
        LowerResult       r;
        const ir::Module& m =
            lower_for_monitor(one_assertion("bit a, b;", std::string("a |=> ") + std::string(rep)), r);
        REQUIRE_MESSAGE(r.ok, rep);
        CHECK_MESSAGE(only_monitor(m).tracking == ir::AssertionTracking::Counted, rep);
        CHECK_MESSAGE(only_monitor(m).has_trigger, rep);
    }
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

TEST_CASE("the and/intersect asymmetry shows up as the tracking discipline") {
    // Identical operands under the two operators, so only the operator differs.
    //
    // `and` runs to the later of its two ends, so ONE unbounded operand leaves the
    // pair unbounded. `intersect` ends where both end together, so the bounded side
    // bounds the pair. That asymmetry used to be the difference between a refusal
    // and an acceptance; now that an unbounded obligation under a trigger is
    // tracked by counting, it is the difference between the two DISCIPLINES -- which
    // is the same fact, observable instead of fatal.
    LowerResult       conj;
    const ir::Module& cm = lower_for_monitor(R"(
module t;
  bit clk, req, ack, done, rdy, go;
  ap: assert property (@(posedge clk) req |=> ((ack ##[1:$] done) and (rdy ##[1:2] go)));
endmodule
)", conj);
    REQUIRE_MESSAGE(conj.ok, conj.diagnostic_text());
    CHECK(only_monitor(cm).tracking == ir::AssertionTracking::Counted);

    LowerResult       meet;
    const ir::Module& mm = lower_for_monitor(R"(
module t;
  bit clk, req, ack, done, rdy, go;
  ap: assert property (@(posedge clk) req |=> ((ack ##[1:$] done) intersect (rdy ##[1:2] go)));
endmodule
)", meet);
    REQUIRE_MESSAGE(meet.ok, meet.diagnostic_text());
    CHECK(only_monitor(mm).tracking == ir::AssertionTracking::Ring);
    // Three cycles, from the bounded side alone -- the bound the ring is sized by.
    CHECK(only_monitor(mm).depth == 3);
}

TEST_CASE("a universal position still needs a single-cycle operand") {
    // What SVA_ERROR_UNBOUNDED_OBLIGATION narrowed to. `always p` and the left
    // operand of an `until` are UNIVERSAL over cycles: "p at every cycle of an
    // unbounded window" is one attempt of p per cycle, all overlapping, and a
    // counted attempt is a single state that cannot carry a set of sub-attempts
    // growing with the trace. A single-cycle operand resolves each sub-attempt
    // inside its own cycle, so the set never grows and the shape is fine.
    refuses(one_assertion("bit a, b;", "always (a ##1 b)"), "must match in a single cycle");
    refuses(one_assertion("bit a, b, c;", "(a ##1 b) until c"), "must match in a single cycle");

    // `s_eventually` is EXISTENTIAL -- one witness, not one per cycle -- so it takes
    // a multi-cycle operand happily. That contrast is why the rule is about the
    // position and not about unboundedness.
    LowerResult ok;
    lower_for_monitor(one_assertion("bit a, b;", "s_eventually (a ##1 b)"), ok);
    CHECK_MESSAGE(ok.ok, ok.diagnostic_text());

    // And the RIGHT operand of an until is existential in the same way.
    LowerResult rhs;
    lower_for_monitor(one_assertion("bit a, b, c;", "a until (b ##1 c)"), rhs);
    CHECK_MESSAGE(rhs.ok, rhs.diagnostic_text());
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

std::string report_times(std::string_view source, std::string_view marker = "assertion failed") {
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
        } else if (line.find(marker) != std::string_view::npos) {
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

// How many times `needle` occurs in `hay`. The pass/cover cases below turn on a
// report happening ONCE rather than merely happening.
size_t count_of(std::string_view hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + needle.size())) {
        ++n;
    }

    return n;
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

// Substitute one placeholder in a fixture. Several cases below differ only in the
// body or the task they run, so the trace itself is written once and the case
// supplies the line -- what a reader has to compare between two expectations is
// then exactly the difference between the two bodies.
std::string substituted(std::string_view source, std::string_view token, std::string_view with) {
    std::string out(source);
    out.replace(out.find(token), token.size(), with);
    return out;
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

TEST_CASE("a gating expression on $past skips the ticks it does not hold on") {
    // LRM 16.9.3 reads $past(v, n, g) at "the kth strictly prior time step in
    // which the event ev iff g occurred", so the gate decides which ticks the
    // history counts rather than which value it hands back. The gate is sampled
    // like any other assertion boolean; only the `disable iff` condition is not.
    //
    // Samples are t5: v=0 g=1, t15: v=1 g=1, t25: v=2 g=1, t35: v=3 g=0,
    // t45: v=2 g=1. The gate is low for the t=35 tick alone, so the ring still
    // holds the t=25 sample of 2 when t=45 reads it.
    //
    // `$past != v` therefore fails at t=45 (2 against 2), on top of the first
    // tick, where the seed of 0 meets a sample of 0. Drop the gate on the floor
    // and the ring holds the t=35 sample of 3 instead, which passes -- so the
    // t=45 report is exactly the gate being honoured.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  bit       g = 1;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $past(v, 1, g) != v);
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 2;
    @(posedge clk); v <= 3; g <= 0;
    @(posedge clk); v <= 2; g <= 1;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 45");

    // The ungated twin on the identical trace, which is what makes the line above
    // a measurement rather than an assertion about itself.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit [3:0] v = 0;
  bit       g = 1;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) $past(v) != v);
  initial begin
    @(posedge clk); v <= 1;
    @(posedge clk); v <= 2;
    @(posedge clk); v <= 3; g <= 0;
    @(posedge clk); v <= 2; g <= 1;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5");
}

TEST_CASE("a gated $past gets a ring of its own, at its own depth") {
    // Rings are keyed by (variable, gate) and not by the variable alone: the two
    // advance on different tick sets, so one ring could answer at most one of
    // them. Depth is still the largest n asked of EACH ring -- two entries gated,
    // one ungated -- so a gate costs a second ring and never a deeper one.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk, g;
  bit [3:0] v;
  ap: assert property (@(posedge clk) $past(v, 2, g) != v && $past(v) != v);
endmodule
)", r);

    CHECK(ring_count(m, "v") == 3);

    size_t gated = 0;
    for (const ir::SignalInfo& s : m.signals()) {
        gated += s.name.find(".past.v.gate") != std::string::npos ? 1 : 0;
    }
    CHECK(gated == 2);
}

// -- Sampled value function refusals ----------------------------------------

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

// -- A disable iff declared in a named property (LRM 16.12) ------------------
//
// The clause sits at the top of a property_spec, and a property declaration's
// body IS one, so `property p; disable iff (rst) ...; endproperty` carries a
// reset that belongs to every assertion instantiating p. lower_concurrent_assertion
// lifts it (body_disable) and leaves the node standing for sva::lower to step
// over. These run the monitor, because the observable is whether the reset
// actually suppresses -- the structural half is the reset-root test at the top of
// this file, and the nesting refusals are with the other refusals above.

TEST_CASE("a disable iff in a named property suppresses the assertion") {
    // a is sampled high at t=15 alone, so `a |=> b` opens one attempt whose
    // obligation is checked at t=25 against a b that never rises. rst is driven
    // with a nonblocking assignment at t=25, which commits in NBA and so is
    // live-high when the monitor runs in Observed at that same tick -- LRM 16.15
    // reads the disable condition live rather than sampled.
    //
    // The control first: the identical trace with no clause at all, so the silence
    // below is the reset working and not the trace failing to fail.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, rst = 0;
  always @(posedge clk) $display("[%0t]", $time);
  property p;
    a |=> b;
  endproperty
  ap: assert property (@(posedge clk) p);
  initial begin
    @(posedge clk); a <= 1;
    @(posedge clk); a <= 0;
    @(posedge clk); rst <= 1;
    @(posedge clk); rst <= 0;
    @(posedge clk); $finish;
  end
endmodule
)") == "25");

    // Same trace, same property, plus the clause the declaration now carries.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, rst = 0;
  always @(posedge clk) $display("[%0t]", $time);
  property p;
    disable iff (rst) a |=> b;
  endproperty
  ap: assert property (@(posedge clk) p);
  initial begin
    @(posedge clk); a <= 1;
    @(posedge clk); a <= 0;
    @(posedge clk); rst <= 1;
    @(posedge clk); rst <= 0;
    @(posedge clk); $finish;
  end
endmodule
)").empty());

    // And with the clock inside the declaration as well. LRM 16.12's property_spec
    // puts the clocking event ABOVE the clause, so this is the shape with a
    // Clocking wrapper between the instantiated body and the reset -- the one step
    // body_disable takes that body_clock does not need.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, rst = 0;
  always @(posedge clk) $display("[%0t]", $time);
  property p;
    @(posedge clk) disable iff (rst) a |=> b;
  endproperty
  ap: assert property (p);
  initial begin
    @(posedge clk); a <= 1;
    @(posedge clk); a <= 0;
    @(posedge clk); rst <= 1;
    @(posedge clk); rst <= 0;
    @(posedge clk); $finish;
  end
endmodule
)").empty());
}

// -- The bounded property operators (LRM 16.12) ------------------------------
//
// `nexttime`, bounded `always` / `eventually`, `#-#` / `#=#`, property-level
// `and` / `or`, and the `if` / `case` guarded selection forms. The temporal
// semantics are libsva's and are checked differentially in tests/sva/diff_*.sv;
// what these pin is the wiring -- which builder each spelling reaches, which
// range it passes, and which strength it forces -- plus that every shape still
// out of reach is refused rather than quietly mislowered.

TEST_CASE("nexttime[n] shifts its operand by exactly n cycles") {
    // `nexttime[n] p` is `##n p` at the property layer, so the obligation grows one
    // filler state per cycle skipped ahead of the operand's own. n = 1 and n = 2
    // differ by exactly that one state, which is what says the range reached
    // sva_prop_shift rather than being rounded to a constant.
    LowerResult r1;
    const uint32_t one =
        only_monitor(lower_for_monitor(one_assertion("bit a;", "nexttime [1] a"), r1))
            .obligation.state_count;
    REQUIRE(r1.ok);

    LowerResult r2;
    const uint32_t two =
        only_monitor(lower_for_monitor(one_assertion("bit a;", "nexttime [2] a"), r2))
            .obligation.state_count;
    REQUIRE(r2.ok);

    CHECK(two == one + 1);
}

TEST_CASE("a bare nexttime is nexttime[1] and not nexttime[0]") {
    // LRM 16.12.10: the unranged form advances one cycle. Reading the absent range
    // as zero would make it the operand itself, which is the one wrong answer that
    // still lowers.
    LowerResult bare;
    const ir::AssertionMonitor& b =
        only_monitor(lower_for_monitor(one_assertion("bit a;", "nexttime a"), bare));
    REQUIRE(bare.ok);

    LowerResult ranged;
    const ir::AssertionMonitor& one =
        only_monitor(lower_for_monitor(one_assertion("bit a;", "nexttime [1] a"), ranged));
    REQUIRE(ranged.ok);

    CHECK(b.obligation.state_count == one.obligation.state_count);
    CHECK(b.depth == one.depth);
    // And not the bare operand, which would be a single state.
    CHECK(b.obligation.state_count > 1);
}

TEST_CASE("always [m:n] reads its range conjunctively and eventually disjunctively") {
    // The same range under the two operators: `always` obliges the operand at EVERY
    // offset and `eventually` at ONE of them, so only the conjunctive form pays for
    // the offsets whose copies of the operand are live at once. With a body whose
    // match length varies the two separate sharply -- libsva's own measurement --
    // and what that pins here is that `always` reached SVA_SHIFT_EVERY.
    LowerResult ra;
    const uint32_t every =
        only_monitor(
            lower_for_monitor(one_assertion("bit a, b;", "always [1:3] (a ##[1:2] b)"), ra))
            .obligation.state_count;
    REQUIRE(ra.ok);

    LowerResult re;
    const uint32_t some =
        only_monitor(
            lower_for_monitor(one_assertion("bit a, b;", "eventually [1:3] (a ##[1:2] b)"), re))
            .obligation.state_count;
    REQUIRE(re.ok);

    CHECK(every > some);
}

TEST_CASE("the strong property operators force a strong subtree and the weak ones do not") {
    // LRM 16.12.15: the `s_` forms require the clock to tick enough times, which
    // libsva encodes as the strength of the prop_seq leaf. That is the whole
    // observable difference between diff_strongprop.sv and diff_nexttime.sv, and it
    // is set by the operator's own spelling -- `strong`/`weak` wrap sequences, not
    // properties, so nothing above it can override it.
    struct Case {
        std::string_view op;
        ir::AssertionStrength want;
    };
    for (const Case c : { Case{ "nexttime [1]", ir::AssertionStrength::Weak },
                          Case{ "s_nexttime [1]", ir::AssertionStrength::Strong },
                          Case{ "always [1:2]", ir::AssertionStrength::Weak },
                          Case{ "s_always [1:2]", ir::AssertionStrength::Strong },
                          Case{ "eventually [1:2]", ir::AssertionStrength::Weak },
                          Case{ "s_eventually [1:2]", ir::AssertionStrength::Strong } }) {
        LowerResult r;
        const ir::Module& m =
            lower_for_monitor(one_assertion("bit a;", std::string(c.op) + " a"), r);
        CHECK_MESSAGE(r.ok, c.op);
        if (r.ok) {
            CHECK_MESSAGE(only_monitor(m).strength == c.want, c.op);
        }
    }
}

TEST_CASE("a followed-by never becomes a trigger, unlike the implication it mirrors") {
    // LRM 16.12.9 defines `s #-# p` as `not (s |-> not p)`. Negating the
    // implication turns its universal over antecedent matches into an existential,
    // so the antecedent joins the OBLIGATION instead of becoming a trigger. That is
    // the structural difference from `|->`, whose antecedent does.
    LowerResult fb;
    const ir::AssertionMonitor& followed =
        only_monitor(lower_for_monitor(one_assertion("bit a, b;", "a #-# b"), fb));
    REQUIRE(fb.ok);
    CHECK_FALSE(followed.has_trigger);

    LowerResult impl;
    const ir::AssertionMonitor& implies =
        only_monitor(lower_for_monitor(one_assertion("bit a, b;", "a |-> b"), impl));
    REQUIRE(impl.ok);
    CHECK(implies.has_trigger);
}

TEST_CASE("the non-overlapping followed-by starts its consequent a cycle later") {
    // `#=#` against `#-#`: one more cycle between the antecedent's match and the
    // consequent, which is one more state in the obligation.
    LowerResult ro;
    const uint32_t overlap =
        only_monitor(lower_for_monitor(one_assertion("bit a, b;", "a #-# b"), ro))
            .obligation.state_count;
    REQUIRE(ro.ok);

    LowerResult rn;
    const uint32_t nonoverlap =
        only_monitor(lower_for_monitor(one_assertion("bit a, b;", "a #=# b"), rn))
            .obligation.state_count;
    REQUIRE(rn.ok);

    CHECK(nonoverlap == overlap + 1);
}

TEST_CASE("property-level and/or is reached only when an operand is a property") {
    // LRM 16.9 and 16.12 spell `and`/`or` with the same token and slang builds one
    // node kind for both, so the operands decide the layer, not the operator. Two
    // sequences must still take the sequence-layer builder; a property operand on
    // either side switches to sva_prop_junction.
    LowerResult seq;
    const ir::Module& sm = lower_for_monitor(
        one_assertion("bit a, b, c, d;", "(a ##1 b) and (c ##1 d)"), seq);
    REQUIRE(seq.ok);
    CHECK_FALSE(only_monitor(sm).has_trigger);

    LowerResult prop;
    const ir::Module& pm =
        lower_for_monitor(one_assertion("bit a, b;", "(nexttime [1] a) and b"), prop);
    REQUIRE(prop.ok);
    CHECK_FALSE(only_monitor(pm).has_trigger);

    LowerResult disj;
    lower_for_monitor(one_assertion("bit a, b;", "(nexttime [1] a) or b"), disj);
    CHECK(disj.ok);
}

TEST_CASE("an if property desugars to a disjunction of followed-bys, not of implications") {
    // The distinction that makes the desugaring correct: a followed-by whose
    // antecedent never matches FAILS, so the untaken branch contributes nothing and
    // the taken one alone decides the verdict. With implications there, the untaken
    // branch would hold vacuously and the disjunction would always pass -- a
    // monitor that can never report. diff_propif.sv pins that behaviourally; this
    // pins that the shape carries no trigger, which a disjunction of implications
    // could not manage either.
    LowerResult r;
    const ir::Module& m =
        lower_for_monitor(one_assertion("bit c, a, b;", "if (c) a else b"), r);
    REQUIRE(r.ok);
    CHECK_FALSE(only_monitor(m).has_trigger);
}

TEST_CASE("an if property with no else holds when the condition is false") {
    // LRM 16.12.16 makes the missing `else` the TRUE property rather than an
    // absence, so the form lowers with one arm written and must not become a
    // monitor that fails whenever the condition is low. diff_propif.sv's `ifn`
    // holds the behavioural half.
    LowerResult r;
    lower_for_monitor(one_assertion("bit c, a;", "if (c) a"), r);
    CHECK(r.ok);
}

TEST_CASE("a case property compares against each item label and honours the default") {
    // LRM 16.12.17. The comparison has no Expression of its own in the AST -- slang
    // binds the case expression and every label separately -- so each guard is an
    // atom carrying BOTH halves, which is the one atom shape with two expressions.
    LowerResult plain;
    lower_for_monitor(
        one_assertion("bit x, y; bit [1:0] sel;", "case (sel) 2'd0: x; 2'd1: y; endcase"),
        plain);
    CHECK(plain.ok);

    LowerResult defaulted;
    lower_for_monitor(one_assertion("bit x, y; bit [1:0] sel;",
                                    "case (sel) 2'd0: x; 2'd1: y; default: 1'b0; endcase"),
                      defaulted);
    CHECK(defaulted.ok);

    // Every guard is an atom over (sel, label) in both polarities, so a two-item
    // case has strictly more atoms than its two arms.
    if (plain.ok) {
        CHECK(only_monitor(*plain.module).cubes.size() > 2);
    }
}

TEST_CASE("a case property item with several labels matches any of them") {
    // `2'd0, 2'd1: x;` is one arm guarded by a disjunction of two comparisons, and
    // its miss is the conjunction of the two negated ones.
    LowerResult r;
    lower_for_monitor(one_assertion("bit x, y; bit [1:0] sel;",
                                    "case (sel) 2'd0, 2'd1: x; default: y; endcase"),
                      r);
    CHECK(r.ok);
}

// -- The refusals the property layer keeps ----------------------------------

TEST_CASE("an unbounded always lowers to a counted monitor") {
    // Phase 3. `always p` is `always [0:$] p` (LRM 16.12.13), whose obligation has
    // no bounded longest match -- so no ring can be sized for it, and the monitor
    // is tracked by counting attempts per determinised state instead.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(one_assertion("bit a;", "always a"), r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(only_monitor(m).tracking == ir::AssertionTracking::Counted);
}

TEST_CASE("an unbounded s_eventually lowers to a counted monitor") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(one_assertion("bit a;", "s_eventually a"), r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(only_monitor(m).tracking == ir::AssertionTracking::Counted);
    // The strong form, so a pending attempt is a failure at end of trace -- which
    // for this operator is the ONLY place it can fail.
    CHECK(only_monitor(m).strength == ir::AssertionStrength::Strong);
}

TEST_CASE("a property range with an open upper bound is the unbounded form") {
    // `always [2:$]` is `always` written as a range, so it reaches sva_prop_shift
    // with SVA_UNBOUNDED as its upper bound rather than being refused ahead of it.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(one_assertion("bit a;", "always [2:$] a"), r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(only_monitor(m).tracking == ir::AssertionTracking::Counted);

    // A bounded range is still the ring: nothing about this change moves a
    // property that can size a pool onto the counted discipline.
    LowerResult       bounded;
    const ir::Module& bm = lower_for_monitor(one_assertion("bit a;", "always [2:4] a"), bounded);
    REQUIRE_MESSAGE(bounded.ok, bounded.diagnostic_text());
    CHECK(only_monitor(bm).tracking == ir::AssertionTracking::Ring);
}

// -- Negation, and the two operators that reduce to it (LRM 16.12.2, 16.12.6) --

TEST_CASE("the not property operator lowers to a complement of its operand") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(one_assertion("bit a, b;", "not (a ##1 b)"), r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    // The complement needs the operand DETERMINISED -- the state-set complement of a
    // nondeterministic automaton is not its complement -- so the obligation arrives
    // from the total subset construction and is tracked accordingly. What is checked
    // here is only that the operand did not survive as itself: a `not` that lowered
    // to its operand would pass every behavioural case below inverted, and this is
    // the cheapest thing that separates them.
    CHECK_FALSE(only_monitor(m).has_trigger);
}

TEST_CASE("not swaps the weak and strong end-of-trace disposition") {
    // LRM 16.12.2 is the whole of this: a pending weak property HOLDS when the trace
    // ends and a pending strong one FAILS, so negating one gives the other. Reported
    // through ir::AssertionMonitor::strength, which is what
    // ir::verdict_at_end_of_trace turns into the lattice member the flush reads.
    struct Case {
        std::string_view body;
        ir::AssertionStrength want;
    };
    for (const Case c : {
           // An `assert property` is weak by default (LRM 16.12.2), so `a` is weak
           // and its negation is therefore strong. The swap is unconditional: there
           // is no "unmarked" case that escapes it.
           Case{ "not a", ir::AssertionStrength::Strong },
           // `strong`/`weak` wrap the operand, and `not` inverts what they set.
           Case{ "not strong(a ##1 b)", ir::AssertionStrength::Weak },
           Case{ "not weak(a ##1 b)", ir::AssertionStrength::Strong },
           // Two negations are the identity, which is what makes this a SWAP rather
           // than a one-way forcing -- and what `not (not p)` had better be.
           Case{ "not (not weak(a ##1 b))", ir::AssertionStrength::Weak },
           Case{ "not (not strong(a ##1 b))", ir::AssertionStrength::Strong },
         }) {
        LowerResult       r;
        const ir::Module& m =
          lower_for_monitor(one_assertion("bit a, b;", std::string(c.body)), r);
        REQUIRE_MESSAGE(r.ok, c.body);
        CHECK_MESSAGE(only_monitor(m).strength == c.want, c.body);
    }
}

TEST_CASE("implies lowers only where the operand strengths make one disposition") {
    // `p implies q` is `(not p) or q`, and the negation inverts the left operand's
    // end-of-trace disposition along with it -- so the `or`'s two sides agree
    // exactly when the operands as WRITTEN disagree. A monitor carries one
    // disposition, so that is the accept condition rather than a preference.
    LowerResult ok;
    lower_for_monitor(one_assertion("bit a, b, c;", "strong(a ##1 b) implies c"), ok);
    CHECK_MESSAGE(ok.ok, ok.diagnostic_text());

    LowerResult flipped;
    lower_for_monitor(one_assertion("bit a, b, c;", "c implies strong(a ##1 b)"), flipped);
    CHECK_MESSAGE(flipped.ok, flipped.diagnostic_text());

    // Matching strengths mix, in both directions, and the message says which
    // operator the user wrote rather than naming the `or` underneath it.
    refuses(one_assertion("bit a, b;", "a implies b"), "`implies`");
    refuses(one_assertion("bit a, b;", "strong(a) implies strong(b)"), "`implies`");
}

TEST_CASE("iff is refused for every pair of operand strengths, naming iff") {
    // Not a gap in this pass. `p iff q` is the conjunction of both implications:
    // one direction needs !sp == sq and the other needs !sq == sp, so the outer
    // `and` always compares sq against !sq. Every assignment mixes, which is why
    // this is spelled as four cases rather than one.
    for (const std::string_view body : { "a iff b",
                                         "strong(a) iff b",
                                         "a iff strong(b)",
                                         "strong(a) iff strong(b)" }) {
        refuses(one_assertion("bit a, b;", body), "`iff`");
    }
}

TEST_CASE("an operand of not that raises its own obligation is refused, naming not") {
    // A complement is taken over the OBLIGATION and a trigger is not part of one,
    // so `not (a |=> b)` would need the monitor to hold the antecedent's attempts
    // and the complement of what they oblige. libsva reports it too; clotho checks
    // first so the diagnostic names the operator in the source.
    refuses(one_assertion("bit a, b;", "not (a |=> b)"), "`not`");
}

TEST_CASE("an abort below a not is refused in all four spellings, naming not") {
    // libsva refuses this and the reason is ARMING, not the state mask, which the
    // determinisation carries fine. An abort is armed from the UNFILTERED one-step
    // image of where its attempt is, deliberately, so that it covers the cycle its
    // subtree's last thread dies in -- which is the cycle `accept_on` exists for,
    // discharging an attempt the property was about to fail. A determinised successor
    // is already valuation-filtered, and the complement gives a never-satisfiable
    // guard to the very state that image would be read off, so the abort reads as
    // unarmed in exactly the cycle it should fire in.
    //
    // Which is why this is a refusal and not an inverted effect. Lowering it -- the
    // reading that an abort under a complement simply swaps accept for reject -- was
    // tried and a differential test in libsva disproved it: it turns every such
    // `accept_on` discharge silently into a failure. Do not "fix" this test by
    // implementing that.
    //
    // All four spellings are one AbortAssertionExpr, so all four are covered by the
    // one walk and all four are checked here to keep it that way.
    for (const std::string_view body : { "not (accept_on (c) (a ##1 b))",
                                         "not (reject_on (c) (a ##1 b))",
                                         "not (sync_accept_on (c) (a ##1 b))",
                                         "not (sync_reject_on (c) (a ##1 b))" }) {
        refuses(one_assertion("bit a, b, c;", body), "`not`");
    }

    // Nested rather than immediate, so the refusal is the WALK and not a check on the
    // operand's own top node.
    refuses(one_assertion("bit a, b, c;", "not (nexttime [1] (accept_on (c) (a ##1 b)))"),
            "`not`");

    // And the other side of it: an abort ABOVE the `not` lowers. Its scope is then the
    // whole complemented property rather than a state set inside it, so there is no
    // renumbering to survive -- which is what makes the refusal above attributable to
    // the nesting rather than to the pair of operators appearing together.
    LowerResult above;
    lower_for_monitor(one_assertion("bit a, b, c;", "accept_on (c) (not (a ##1 b))"), above);
    CHECK_MESSAGE(above.ok, above.diagnostic_text());
}

TEST_CASE("not reports where its operand holds and is silent where it fails") {
    // The observable of a complement. `a` samples 1, 0, 1, 1 at t=5, 15, 25 and 35,
    // so `a` as a property holds, fails, holds, holds -- and `not a` is the exact
    // inverse, reporting at every tick but the one `a` failed at.
    //
    // The trailing tick before `$finish` is load-bearing in every trace below: the
    // monitor runs in Observed and `$finish` in the Active region of the same time
    // step, so the tick that executes `$finish` never steps the monitor. Whatever is
    // to be observed has to be sampled a tick earlier.
    //
    // A `not` that lowered to its operand reads "15" here.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) not a);
  initial begin
    @(posedge clk); a <= 0;
    @(posedge clk); a <= 1;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "5 25 35");
}

TEST_CASE("a not still pending at the end of the trace resolves by the SWAPPED strength") {
    // THE case for LRM 16.12.2, and the one that is easiest to leave untested: while
    // a property is pending it is neither true nor false, and negating pending is
    // pending, so the only thing that separates `not weak(p)` from `not strong(p)`
    // is what the end of the trace does to it.
    //
    // `a` samples 0 at t=5, 15 and 25 and 1 at t=35, which is the last tick the
    // monitor steps in. So `a ##1 b` is seeded four times, fails on its first letter
    // three times -- which makes `not (a ##1 b)` HOLD, silently, at those ticks --
    // and is left PENDING exactly once, needing a cycle that never comes. One
    // pending attempt, one disposition to report, nothing else in the trace to
    // confuse it with.
    const auto pending_at_finish = [](std::string_view body) {
        return report_times(std::string(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 1;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) )") +
                            std::string(body) + R"();
  initial begin
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); a <= 1;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)");
    };

    // The operand is weak, so pending would HOLD for it -- and therefore FAIL for
    // its negation, which is the end-of-trace report.
    CHECK(pending_at_finish("not weak(a ##1 b)") == "45");

    // The operand is strong, so pending FAILS for it and HOLDS for its negation:
    // no report at all. Not vacuous silence -- the attempt exists and is pending,
    // which is what the case above proves on the same trace.
    CHECK(pending_at_finish("not strong(a ##1 b)") == "");
}

TEST_CASE("implies holds vacuously where its antecedent fails") {
    // Vacuity is the whole content of an implication, and `implies` is the property
    // operator of LRM 16.12.6 rather than `|->`: with no antecedent match there is
    // nothing to oblige, so the property holds and reports nothing.
    //
    // `a` samples 1 only at t=5, so `a ##1 b` matches once, ending at t=15 -- and
    // `c` is 0 there, so the consequent fails and the implication with it. Every
    // other tick has no antecedent match and is silent. Reading vacuity as a
    // FAILURE instead reads "5 15 25 35".
    //
    // `strong(...)` on the antecedent is not decoration: `implies` negates its left
    // operand, so the pair has one end-of-trace disposition only when the two
    // operands are spelled with OPPOSITE strengths.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 1, c = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) strong(a ##1 b) implies c);
  initial begin
    @(posedge clk); a <= 0;
    @(posedge clk);
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "15");
}

TEST_CASE("an x-valued atom is false in both polarities, and not does not inherit that") {
    // concurrent-assertions.md 5.2: LRM 16.6 collapses x and z to false, and the
    // collapse happens at the ATOM. So `a` and `!a` are BOTH false when `a` is x --
    // the one place SVA booleans stop being two-valued -- and both assertions fail
    // at every tick.
    //
    // `!a` is false because clotho builds guards in negation normal form: the `!`
    // reaches the leaf and eval_atom applies Lnot to the FOUR-state value, before the
    // four-to-two collapse. Emitting it as a complement of the collapsed bit instead
    // would read TRUE under x and the second case below would print nothing.
    constexpr std::string_view trace = R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  logic a = 1'bx;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) BODY);
  initial begin
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)";
    const auto with = [&trace](std::string_view body) {
        return report_times(substituted(trace, "BODY", body));
    };

    CHECK(with("a") == "5 15");
    CHECK(with("!a") == "5 15");

    // And the PROPERTY layer does not inherit the collapse, because it is not over
    // values at all: `not` complements a VERDICT. `a` fails under x, so `not a`
    // holds, and it holds at every tick -- which is exactly the opposite of what
    // `!a` does one line above. That difference is the reason negation could not
    // simply be pushed into the atom and had to wait for the complement.
    CHECK(with("not a") == "");
}

TEST_CASE("a determinised cell partitions on the COLLAPSED guard, so x lands in not-a") {
    // The other half of the rule above, and the half that is easy to get backwards.
    // libsva's cell splitter appends `.negated = !literal.negated` when it
    // determinises, so `first_match` carries complemented guard literals today with no
    // `not` anywhere near it -- and those are NOT the user's `!`.
    //
    // A cell literal is the two-valued complement of the collapsed bit, because the
    // cells exist to PARTITION the valuation space and the thing being partitioned is
    // the collapsed decision "did the guard hold". `b` is x, so `b` did not hold, so
    // the not-`b` cell is the one enabled and the attempt stays alive across the
    // `[1:2]` window before failing.
    //
    // Evaluating the cell's negation three-valuedly -- the way the user's `!a` must be
    // evaluated -- leaves `b` and `!b` BOTH false, which is a gap rather than a
    // partition: no cell is enabled, the attempt has no successor, and it dies a cycle
    // early. That mutation reads "15 25 35 45" here instead. So this pins the
    // distinction rather than either rule on its own, which is why it is worth its own
    // case: the two negations look identical in the tables and differ only under x.
    LowerResult probe;
    lower_for_monitor(one_assertion("bit a; logic b;", "first_match(a ##[1:2] b)"), probe);
    REQUIRE_MESSAGE(probe.ok, probe.diagnostic_text());
    // The premise: the determinisation really did emit a complemented literal. Without
    // this the case below would pass for the trivial reason that there is nothing to
    // get wrong.
    bool any_negated = false;
    for (const ir::AssertionLiteral& lit : only_monitor(*probe.module).literals) {
        any_negated = any_negated || lit.negated;
    }
    CHECK(any_negated);

    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1;
  logic b = 1'bx;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) first_match(a ##[1:2] b));
  initial begin
    @(posedge clk);
    @(posedge clk);
    @(posedge clk);
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "25 35 45");
}

TEST_CASE("the until family lowers, and its strength comes from its own spelling") {
    // LRM 16.12.11. All four raise an unbounded obligation -- nothing in the
    // property says when `q` arrives -- so all four are counted. The `s_` forms owe
    // that `q` and are strong; the plain ones do not and are weak.
    struct Case {
        std::string_view op;
        ir::AssertionStrength strength;
    };
    for (const Case c : { Case{ "until", ir::AssertionStrength::Weak },
                          Case{ "until_with", ir::AssertionStrength::Weak },
                          Case{ "s_until", ir::AssertionStrength::Strong },
                          Case{ "s_until_with", ir::AssertionStrength::Strong } }) {
        LowerResult       r;
        const ir::Module& m = lower_for_monitor(
            one_assertion("bit a, b;", std::string("a ") + std::string(c.op) + " b"), r);
        REQUIRE_MESSAGE(r.ok, c.op);
        CHECK_MESSAGE(only_monitor(m).tracking == ir::AssertionTracking::Counted, c.op);
        CHECK_MESSAGE(only_monitor(m).strength == c.strength, c.op);
    }
}

TEST_CASE("an implication under an unbounded operator is refused as a mode mix") {
    // The one shape the two tracking disciplines cannot share: the implication
    // wants the ring, the `always` wants the counted states, and tracking is chosen
    // once per monitor. The refusal has to name the rewrite, because the property
    // is both legal and idiomatic SystemVerilog -- and `always (a |=> b)` is just
    // `a |=> b`, since LRM 16.12.1 already starts an attempt every cycle.
    refuses(one_assertion("bit a, b;", "always (a |=> b)"),
            "different attempt-tracking disciplines");
    refuses(one_assertion("bit a, b, c;", "(a |=> b) until c"),
            "different attempt-tracking disciplines");

    // The rewrite it names must actually lower, or the advice is wrong.
    LowerResult ok;
    const ir::Module& m = lower_for_monitor(one_assertion("bit a, b;", "a |=> b"), ok);
    REQUIRE_MESSAGE(ok.ok, ok.diagnostic_text());
    CHECK(only_monitor(m).tracking == ir::AssertionTracking::Ring);
}

// -- The counted tracking discipline ----------------------------------------
//
// An unbounded obligation cannot size a ring, so its monitor tracks a COUNT of
// attempts per state of the determinised obligation instead
// (ir::AssertionTracking, docs/design/sva-property-layer.md 3). What these check
// is the part libsva's own tests cannot see: that the ring path is untouched,
// that the counted path allocates and steps what it should, and above all that
// several attempts resolving in ONE cycle are reported once EACH. That last one
// is the entire reason the discipline carries counts rather than a merged state
// vector -- merging preserves the cycle set and loses the count, measured in
// ../libsva/test/merge.zig.

namespace {

// Signals whose name contains `needle`. The counted path names one counter per
// carrying obligation state and the state numbering is libsva's, so a test can
// pin how MANY there are without pinning which indices survived the prune.
size_t signal_count(const ir::Module& m, std::string_view needle) {
    size_t n = 0;
    for (const ir::SignalInfo& s : m.signals()) {
        n += s.name.find(needle) != std::string::npos ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("a ring monitor allocates exactly the state it always has") {
    // The regression guard for the whole change: adding a second discipline must
    // not perturb the first. A bounded property still gets its slot vectors and its
    // occupancy mask, and no counter appears anywhere near it.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(implication, r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    CHECK(only_monitor(m).tracking == ir::AssertionTracking::Ring);
    CHECK(state_width(m, "sva.0.pend0") == 1);
    CHECK(state_width(m, "sva.0.occ") == 1);
    CHECK(state_width(m, "sva.0.trig") == 1);
    CHECK(state_width(m, "sva.0.fired") == 1);
    CHECK(signal_count(m, "sva.0.cnt") == 0);
}

TEST_CASE("a counted monitor allocates counters and none of the ring") {
    // The mirror image. There is no pool to hold attempts in, so there are no slot
    // vectors and no occupancy mask -- an attempt's identity IS the state it sits
    // in, and how many attempts sit there is the counter.
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(one_assertion("bit a, b;", "a until b"), r);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    CHECK(only_monitor(m).tracking == ir::AssertionTracking::Counted);
    CHECK(find_signal(m, "sva.0.pend0") == nullptr);
    CHECK(find_signal(m, "sva.0.occ") == nullptr);
    // No trigger either: an implication under an unbounded operator is refused
    // outright, so a counted monitor never has one.
    CHECK(find_signal(m, "sva.0.trig") == nullptr);
    CHECK(find_signal(m, "sva.0.fired") == nullptr);

    // At least one state can carry an attempt into the next cycle, or the property
    // could never be pending at all. A state that resolves on entry -- one in
    // `last` or in `dead` -- gets no counter, so the numbering has holes and only
    // the count is pinned here.
    CHECK(signal_count(m, "sva.0.cnt") > 0);
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name.find("sva.0.cnt") != std::string::npos) {
            // 64 bits: at the measured tick rate a 32-bit counter wraps within
            // minutes of wall clock.
            CHECK(state_width(m, s.name) == 64);
        }
    }
}

TEST_CASE("an unbounded always reports at the first cycle its operand fails") {
    // `a` samples 1 at t=5 and 0 at t=15. Every attempt is `always a` from its own
    // start cycle, so at t=15 BOTH the attempt opened at t=5 and the one opened at
    // t=15 fail -- the latter in the very cycle it starts, which is the seed landing
    // in a dead state.
    //
    // `a` is back to 1 from t=25, so the attempts opened there stay pending, and
    // `always` is weak, so the end of the trace reports none of them. Reporting the
    // failure once instead of once per attempt reads "15"; deferring it a cycle
    // reads "25 25".
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) always a);
  initial begin
    @(posedge clk); a <= 0;
    @(posedge clk); a <= 1;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "15 15");
}

TEST_CASE("an until reports while it is still waiting for its right operand") {
    // `a until b`: `a` must hold every cycle up to the one `b` first holds in. `b`
    // never holds through t=15, and `a` samples 0 there, so the attempt opened at
    // t=5 fails and so does the one opened at t=15.
    //
    // From t=25 `b` holds, and `b` holding in an attempt's FIRST cycle discharges
    // it without `a` ever being required -- so those attempts match rather than
    // fail, even though `a` is 0. That is the half of 16.12.11 an implementation
    // that just required `a` every cycle would get wrong, and it is why the report
    // set stops at 15 instead of running to the end of the trace.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) a until b);
  initial begin
    @(posedge clk); a <= 0;
    @(posedge clk); b <= 1;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)") == "15 15");
}

TEST_CASE("an s_eventually cannot fail before the end of the trace") {
    // Nothing mid-trace can refute `s_eventually a`: the witness may always be one
    // cycle further out. So an attempt that has not seen `a` yet stays pending
    // rather than failing, and when `a` finally samples 1 at t=25 every attempt
    // opened so far discharges at once.
    //
    // Resolving eagerly -- treating "not yet" as "no" -- would report at 5 and 15.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) s_eventually a);
  initial begin
    @(posedge clk); a <= 1;
    @(posedge clk);
    @(posedge clk);
  end
  initial #38 $finish;
endmodule
)") == "");
}

TEST_CASE("an s_eventually that never sees its operand fails once per attempt at the end") {
    // The same property with `a` never high. Four clocking edges fall before the
    // trace ends, so four attempts are open and every one of them is owed a
    // witness that never came: four failures, all drained by the Final process.
    //
    // `$finish` is a plain delay rather than an edge so the last tick is not racing
    // the teardown -- the number of attempts has to be unambiguous for the count to
    // mean anything.
    //
    // Collapsing the residue to a single report reads 1. That is the end-of-trace
    // face of the same loss the per-cycle counts prevent.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0;
  ap: assert property (@(posedge clk) s_eventually a);
  initial #38 $finish;
endmodule
)");
    CHECK(count_of(out, "assertion failed") == 4);
}

TEST_CASE("weak and strong until differ only at the end of the trace") {
    // LRM 16.12.2 on the same trace, run both ways. `b` never holds and `a` always
    // does, so no attempt of either property can fail during the trace and all four
    // are still pending when it ends.
    //
    // The weak form holds -- it never owed the `b` -- and reports nothing. The
    // strong form owed it, and owes one failure per pending attempt. If the residue
    // were resolved by strength but not by count, the strong side would read 1.
    const auto run = [](std::string_view op) {
        return sva_run_capture(std::string(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 0;
  ap: assert property (@(posedge clk) a )") + std::string(op) + R"( b);
  initial #38 $finish;
endmodule
)");
    };
    CHECK(count_of(run("until"), "assertion failed") == 0);
    CHECK(count_of(run("s_until"), "assertion failed") == 4);
}

TEST_CASE("a strong cover drains no failures at the end of the trace") {
    // Regression. A cover counts matches and never reports (LRM 16.12.6), and slang
    // rejects an `else` on one -- so the end-of-trace drain has nothing to emit for
    // a cover except the DEFAULT failure report, which is not a verdict a cover
    // owns. The drain is therefore gated on whether the directive can fail at all
    // and not on its kind.
    //
    // Only reachable once the property is strong, which is why the unbounded
    // operators surfaced it: `b` never holds, so every attempt of the cover is still
    // pending when the trace ends. Found on tests/sva/diff_until.sv.
    const std::string cover = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 0;
  cp: cover property (@(posedge clk) a s_until b) $display("HIT");
  initial #38 $finish;
endmodule
)");
    CHECK(count_of(cover, "assertion failed") == 0);

    // The identical property as an ASSERT does drain, so the silence above is the
    // cover being exempt and not the trace simply having nothing left pending.
    const std::string assertion = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 0;
  ap: assert property (@(posedge clk) a s_until b);
  initial #38 $finish;
endmodule
)");
    CHECK(count_of(assertion, "assertion failed") == 4);
}

TEST_CASE("many attempts resolving in one cycle are reported once each") {
    // THE case, and the reason the counted discipline carries a count at all.
    //
    // `a` samples 1 at t=5..35 and 0 at t=45, so five attempts of `always a` -- the
    // ones opened at 5, 15, 25, 35 and 45 -- are alive and all fail in that single
    // cycle. Reports are per attempt, so the answer is
    // five reports at t=45 and not one.
    //
    // This is exactly what one merged state vector cannot do: it preserves the
    // cycle set and reads "45", because a vector carries at most one verdict per
    // cycle. Measured over every 6- and 8-cycle trace in ../libsva/test/merge.zig,
    // which is what put the count in the design.
    //
    // `a` returns to 1 afterwards so nothing else fails and the count is only ever
    // this one cycle's.
    const std::string source = R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) always a);
  initial begin
    @(posedge clk);
    @(posedge clk);
    @(posedge clk);
    @(posedge clk); a <= 0;
    @(posedge clk); a <= 1;
    @(posedge clk);
    @(posedge clk); $finish;
  end
endmodule
)";
    CHECK(count_of(sva_run_capture(source), "assertion failed") == 5);
    // And every one of them in the cycle the operand went low, not spread out.
    CHECK(report_times(source) == "45 45 45 45 45");
}

TEST_CASE("a trigger over an unbounded consequent seeds and resolves per match") {
    // The headline shape: `req |=> s_eventually ack`. The antecedent stays a
    // trigger -- one state vector, no pool -- and the obligation is counted, so an
    // attempt is seeded only where `req` matched rather than every cycle.
    //
    // `req` samples 1 at t=15 and t=25, so obligations open at t=25 and t=35, and
    // `ack` never comes. Both are still pending when the trace ends and
    // `s_eventually` is strong, so both are owed a failure: two, not one, and not
    // one per tick. Seeding every cycle instead of on the trigger would give five.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit req = 0, ack = 0;
  ap: assert property (@(posedge clk) req |=> s_eventually ack);
  initial begin
    @(posedge clk); req <= 1;
    @(posedge clk);
    @(posedge clk); req <= 0;
  end
  initial #48 $finish;
endmodule
)");
    CHECK(count_of(out, "assertion failed") == 2);
}

TEST_CASE("a junction of operands with different strength is refused") {
    // libsva reports SVA_ERROR_MIXED_STRENGTH: the pair is pending until both sides
    // resolve, and one weak with one strong side has no single end-of-trace
    // disposition. clotho checks first so the message names the operator.
    refuses(one_assertion("bit a, b, c;", "strong(a ##1 b) and weak(c ##1 a)"),
            "different strength");
}

TEST_CASE("a junction over an implication is refused, naming the operand") {
    // A monitor has one trigger and a junction keeps neither side's, so an operand
    // that raises an obligation of its own has nowhere to put its antecedent.
    refuses(one_assertion("bit a, b, c;", "(a |=> b) and (nexttime [1] c)"),
            "an operand of the property `and` operator is an implication");
}

TEST_CASE("a followed-by whose consequent is an implication is refused") {
    // The antecedent is fused onto the consequent's automaton, so the consequent's
    // states do not survive as a block and a second trigger has nowhere to go.
    refuses(one_assertion("bit a, b, c;", "a #-# (b |=> c)"), "consequent of a followed-by");
}

TEST_CASE("a ranged eventually over an implication is refused but a ranged always is not") {
    // A shift over an implication moves onto the ANTECEDENT, and a DISJUNCTIVE range
    // does not survive that move: each disjunct would hold vacuously on its own and
    // one trigger cannot say so. The conjunctive `always` form does survive it, so
    // that one must still lower -- which is what keeps this from being a blanket
    // refusal of shifts over implications.
    refuses(one_assertion("bit a, b;", "eventually [1:2] (a |=> b)"), "over an implication");

    LowerResult ok;
    lower_for_monitor(one_assertion("bit a, b;", "always [1:2] (a |=> b)"), ok);
    CHECK(ok.ok);

    // A single-offset shift is not a range at all, so it survives too.
    LowerResult single;
    lower_for_monitor(one_assertion("bit a, b;", "nexttime [2] (a |=> b)"), single);
    CHECK(single.ok);
}

TEST_CASE("a branch of an if or case property that is an implication is refused") {
    refuses(one_assertion("bit c, a, b;", "if (c) (a |=> b)"), "branch of an `if` or `case`");
    refuses(one_assertion("bit a, b; bit [1:0] sel;",
                          "case (sel) 2'd0: (a |=> b); default: a; endcase"),
            "branch of an `if` or `case`");
}

TEST_CASE("all four abort operators lower, carrying effect and sampling") {
    struct Case {
        std::string_view         op;
        ir::AssertionAbortEffect effect;
        bool                     sampled;
    };
    for (const Case& c : { Case{ "accept_on", ir::AssertionAbortEffect::Accept, false },
                           Case{ "reject_on", ir::AssertionAbortEffect::Reject, false },
                           Case{ "sync_accept_on", ir::AssertionAbortEffect::Accept, true },
                           Case{ "sync_reject_on", ir::AssertionAbortEffect::Reject, true } }) {
        LowerResult       r;
        const ir::Module& m = lower_for_monitor(
          one_assertion("bit a, b, c;", std::string(c.op) + " (c) (a |=> b)"), r);
        REQUIRE_MESSAGE(r.ok, c.op);

        const ir::AssertionMonitor& mon = only_monitor(m);
        REQUIRE_MESSAGE(mon.aborts.size() == 1, c.op);
        CHECK_MESSAGE(mon.aborts[0].effect == c.effect, c.op);
        // The `sync_` pair reads the Preponed sample; the plain pair is evaluated
        // at the granularity of the simulation time step and reads live, which is
        // the rule `disable iff` already follows.
        CHECK_MESSAGE(mon.aborts[0].sampled == c.sampled, c.op);
        // Above the implication, so the scope reaches the antecedent, and it
        // settles the whole attempt rather than clearing part of it.
        CHECK_MESSAGE(mon.aborts[0].covers_trigger, c.op);
        CHECK_MESSAGE(mon.aborts[0].resolves_attempt, c.op);
    }
}

TEST_CASE("an abort below an implication does not cover the trigger") {
    // The distinction the whole construction turns on, and the reason clotho must
    // not rewrite one shape into the other: above the implication the abort covers
    // the attempt's own cycle, below it the abort only arms once the consequent
    // starts. Different properties, so the flag has to separate them.
    LowerResult                 above;
    const ir::AssertionMonitor& hi = only_monitor(
      lower_for_monitor(one_assertion("bit a, b, c;", "accept_on (c) (a |=> b)"), above));
    REQUIRE(above.ok);
    REQUIRE(hi.aborts.size() == 1);
    CHECK(hi.aborts[0].covers_trigger);

    LowerResult                 below;
    const ir::AssertionMonitor& lo = only_monitor(
      lower_for_monitor(one_assertion("bit a, b, c;", "a |=> accept_on (c) b"), below));
    REQUIRE(below.ok);
    REQUIRE(lo.aborts.size() == 1);
    CHECK_FALSE(lo.aborts[0].covers_trigger);
}

TEST_CASE("the monitor reads a plain abort condition live and a sync one sampled") {
    LowerResult       async;
    const ir::Module& am = lower_for_monitor(
      one_assertion("bit a, b, c;", "accept_on (c) (a |=> b)"), async);
    REQUIRE(async.ok);

    const ir::Process& ap = monitor_process(am);
    CHECK(touches(am, ap, "sva.shadow.a"));
    // LRM 16.12.7 puts the plain forms on `disable iff`'s footing, so the
    // condition is read live and pays for no shadow. The latch beside it is what
    // lets a pulse between two ticks still be seen.
    CHECK(touches(am, ap, "c"));
    CHECK_FALSE(touches(am, ap, "sva.shadow.c"));
    CHECK(find_signal(am, "sva.0.abrt0") != nullptr);

    LowerResult       sync;
    const ir::Module& sm = lower_for_monitor(
      one_assertion("bit a, b, c;", "sync_accept_on (c) (a |=> b)"), sync);
    REQUIRE(sync.ok);

    const ir::Process& sp = monitor_process(sm);
    CHECK(touches(sm, sp, "sva.shadow.c"));
    // Nothing to remember between ticks, so no latch.
    CHECK(find_signal(sm, "sva.0.abrt0") == nullptr);
}

TEST_CASE("an accept discharges an attempt where a reset abandons it") {
    // The distinction `disable iff` does not draw, and the reason an abort needs a
    // table of its own rather than reusing the reset root. Same trace, same
    // condition, same underlying property, and the pending attempt is resolved in
    // both -- but an accept resolves it to a PASS an action block sees, while a
    // reset leaves no verdict at all.
    //
    // `c` rises strictly after tick 1 and is still high at tick 2, so the attempt
    // tick 1's antecedent match seeds into tick 2 is what both wrappers act on.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ac: assert property (@(posedge clk) accept_on (c) (a |=> b)) $display("ACCEPT-PASS");
  rs: assert property (@(posedge clk) disable iff (c) a |=> b) $display("RESET-PASS");
  initial begin
    @(posedge clk);            // cyc0
    a <= 1;
    @(posedge clk);            // cyc1: a high, seeds an obligation into cyc2
    a <= 0;
    @(posedge clk);            // cyc2: b low, so the attempt would fail here
    @(posedge clk);
    $finish;
  end
  initial begin
    #22 c = 1;                 // after tick 1 (15), before tick 2 (25)
    #10 c = 0;                 // t=32
  end
endmodule
)");
    CHECK(out.find("ACCEPT-PASS") != std::string::npos);
    CHECK(out.find("RESET-PASS") == std::string::npos);
    // Neither reports: the accept discharged the attempt and the reset threw it
    // away, so the failure `b` would otherwise have caused is gone from both. That
    // is what makes the pass action the only thing separating them.
    CHECK(out.find("assertion failed") == std::string::npos);
}

TEST_CASE("a reject fails a covered attempt that would otherwise have passed") {
    // The mask path: the attempt is already seeded when the condition fires. `b` is
    // high at tick 2, so the attempt matches there and an unaborted monitor reports
    // nothing at all -- the reject turns that pass into a failure, which is the only
    // way to show the operator doing something an ordinary failure could not.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 1, c = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) reject_on (c) (a |=> b));
  initial begin
    @(posedge clk);            // cyc0
    a <= 1;
    @(posedge clk);            // cyc1: a high, seeds into cyc2
    a <= 0;
    @(posedge clk);            // cyc2: b high, so this attempt matches -- but c is high
    @(posedge clk);
    $finish;
  end
  initial begin
    #22 c = 1;                 // after tick 1 (15), before tick 2 (25)
    #6  c = 0;                 // t=28
  end
endmodule
)") == "25");
}

TEST_CASE("an abort covering a trigger resolves the attempt in its own start cycle") {
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 1, c = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) reject_on (c) (a |=> b));
  initial begin
    @(posedge clk);            // cyc0
    a <= 1;
    @(posedge clk);            // cyc1: a high AND c high, so it is rejected here
    a <= 0;
    @(posedge clk);            // cyc2: b high, so it would otherwise have passed
    @(posedge clk);
    $finish;
  end
  initial begin
    #12 c = 1;                 // between tick 0 (5) and tick 1 (15)
    #6  c = 0;                 // t=18
  end
endmodule
)") == "15");
}

TEST_CASE("only the synchronous abort forms miss a pulse between two ticks") {
    // The async/sync split, and the one place the two accept forms differ. `c`
    // pulses strictly between ticks 1 and 2, so BOTH ticks sample it low: the
    // synchronous form cannot see it and the failure at tick 2 stands, while the
    // asynchronous form is evaluated at every simulation time step (LRM 16.12.7)
    // and discharges its attempt.
    //
    // One report, from `sa` alone. Sample the plain form too and both report,
    // giving `25 25`; read the `sync_` form live and neither does.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0;
  always @(posedge clk) $display("[%0t]", $time);
  aa: assert property (@(posedge clk) accept_on      (c) (a |=> b));
  sa: assert property (@(posedge clk) sync_accept_on (c) (a |=> b));
  initial begin
    @(posedge clk);            // cyc0
    a <= 1;
    @(posedge clk);            // cyc1: a high, seeds into cyc2
    a <= 0;
    @(posedge clk);            // cyc2: b low, so the attempt fails unless discharged
    @(posedge clk);
    $finish;
  end
  initial begin
    #18 c = 1;                 // strictly between tick 1 (15) and tick 2 (25)
    #4  c = 0;                 // t=22
  end
endmodule
)") == "25");
}

TEST_CASE("an abort over a multi-cycle antecedent resolves every attempt in flight") {
    // The last residue of abort-above-an-implication support, and it is closed. With a
    // one-cycle antecedent the match ending this cycle IS this cycle's one attempt, so
    // one bit answered for the whole abort. With a longer one, several attempts can be
    // part-way through the antecedent when the condition fires, and LRM 16.12.7 resolves
    // an attempt wherever it has got to -- so each of them owes its own verdict. The
    // attempt ids are what tells them apart, which is why this lands with the counting
    // and not before it.
    //
    // `a` is high at ticks 0 and 1, so attempt A starts at 5 and B at 15. `b` never
    // rises, so at tick 1 A is part-way through `a ##[1:2] b` and B has just started:
    // two attempts in flight, neither with an obligation yet, and neither would ever
    // report on its own. `z` is high at tick 1 alone.
    const auto with = [](std::string_view abort_form, std::string_view action, std::string_view drive = "z <= 1;") {
        return std::string(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0, z = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) )") + std::string(abort_form) +
               " (z) ((a ##[1:2] b) |=> ok))" + std::string(action) + R"(;
  initial begin
    a <= 1;
    @(posedge clk);            // 5  (tick0): attempt A starts
    )" + std::string(drive) + R"(
    @(posedge clk);            // 15 (tick1): B starts, A is part-matched, z fires
    a <= 0; z <= 0;
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
)";
    };

    // A reject fails both of them, in the one cycle, and the two reports are the claim:
    // one report is the old union-tracked answer and would be a silent under-count.
    const std::string rejected = sva_run_capture(with("sync_reject_on", ""));
    CHECK(count_of(rejected, "assertion failed") == 2);
    CHECK(report_times(with("sync_reject_on", "")) == "15 15");

    // An accept DISCHARGES both instead -- a pass each, which an `else`-less assert
    // reports nothing for but a pass action sees. Same count, opposite verdict, so the
    // two together pin that the abort's effect is carried per attempt and not just its
    // firing.
    const std::string accepted = sva_run_capture(with("sync_accept_on", " $display(\"[%0t] AB-PASS\", $time)"));
    CHECK(count_of(accepted, "assertion failed") == 0);
    CHECK(count_of(accepted, "AB-PASS") == 2);
    CHECK(accepted.find("[15] AB-PASS") != std::string::npos);

    // The baseline: with `z` never high neither attempt resolves at all -- `b` never
    // rises, so there is no antecedent match, no obligation and no verdict. Every report
    // above is the abort's doing and not the trace's.
    const std::string quiet = sva_run_capture(with("sync_reject_on", "", ""));
    CHECK(count_of(quiet, "assertion failed") == 0);

    // A one-cycle antecedent was always exact and stays supported.
    LowerResult ok;
    lower_for_monitor(one_assertion("bit a, b, c;", "accept_on (c) (a |=> b)"), ok);
    CHECK(ok.ok);
}

TEST_CASE("where two aborts fire on one attempt, the outermost wins") {
    // LRM 16.12.14: the outer accept makes the overall evaluation TRUE, whatever
    // the inner reject would have said. Outermost is LAST in the monitor's abort
    // list, because lowering appends outward, so the step scans that list
    // backwards -- scanning it forwards lets the innermost take the attempt and
    // turns this pass into a failure. libsva had exactly that bug in both its own
    // step and its reference backend; this is the third implementation of the
    // rule, so it gets its own check rather than relying on theirs.
    //
    // Both aborts sit BELOW the implication, so this is the mask path: one
    // seeded attempt, two aborts armed on it. `b` is high, so the attempt matches
    // on its own and every report below is the aborts' doing.
    const auto reports = [](std::string_view drive) {
        return report_times(std::string(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 1, x = 0, y = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) a |=> accept_on (x) (reject_on (y) b));
  initial begin
    @(posedge clk);            // cyc0
    a <= 1;
    @(posedge clk);            // cyc1: seeds into cyc2
    a <= 0;
    @(posedge clk);            // cyc2: the attempt resolves here
    @(posedge clk);
    $finish;
  end
  initial begin )") + std::string(drive) + R"( end
endmodule
)");
    };

    // Neither condition: the attempt matches on `b`, so nothing is reported and
    // the two cases below are both departures from a silent baseline.
    CHECK(reports("") == "");
    // The inner reject alone turns that match into a failure, which is what makes
    // the last case a genuine precedence test rather than a reject that never
    // fired.
    CHECK(reports("#22 y = 1;") == "25");
    // The outer accept alone discharges it.
    CHECK(reports("#22 x = 1;") == "");
    // Both, on the same stimulus that reports when only the reject is high. The
    // outer accept overrides it; innermost-wins would read `25`.
    CHECK(reports("#22 x = 1; y = 1;") == "");
}

TEST_CASE("a twice-matching attempt reports only its first failing obligation") {
    // The repro for the per-attempt rule, in miniature. `a ##[1:2] b` matches at tick 2 and
    // again at tick 3 from the single `a` at tick 1, seeding obligations that
    // check `ok` at ticks 3 and 4. Both fail, and the attempt reports once.
    //
    // Reporting per obligation reads `35 45`.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> ok);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1: a high
    a <= 0; b <= 1;
    @(posedge clk);            // tick2: the ##1 match ends
    @(posedge clk);            // tick3: the ##2 match ends; obligation 1 FAILS
    b <= 0;
    @(posedge clk);            // tick4: obligation 2 would fail here
    @(posedge clk);
    $finish;
  end
endmodule
)") == "35");
}

TEST_CASE("a failed attempt does not report a later obligation's pass") {
    // The same shape with `ok` high at tick 4, so the attempt's SECOND obligation
    // passes after its first has already failed. A failure resolves the attempt,
    // so the pass is not the attempt's and no action block runs for it.
    //
    // Per-obligation verdicts print MM-PASS here.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> ok) $display("[%0t] MM-PASS", $time);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1
    a <= 0; b <= 1;
    @(posedge clk);            // tick2: the ##1 match ends
    @(posedge clk);            // tick3: the ##2 match ends; obligation 1 FAILS
    b <= 0; ok <= 1;
    @(posedge clk);            // tick4: obligation 2 sees ok=1
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "MM-PASS") == 0);
    CHECK(count_of(out, "assertion failed") == 1);
}

TEST_CASE("an attempt passes at its last obligation, not its first") {
    // Both obligations pass, so the attempt does too -- once, and in the cycle the
    // LATER one resolves. This is the case that rules out plain report coalescing:
    // suppressing all but the first report would put the pass at 35.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> ok) $display("[%0t] MM-PASS", $time);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1
    a <= 0; b <= 1; ok <= 1;
    @(posedge clk);            // tick2: the ##1 match ends
    @(posedge clk);            // tick3: the ##2 match ends; obligation 1 passes
    b <= 0;
    @(posedge clk);            // tick4: obligation 2 passes -> the attempt passes
    ok <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "MM-PASS") == 1);
    CHECK(out.find("[45] MM-PASS") != std::string::npos);
    CHECK(count_of(out, "assertion failed") == 0);
}

TEST_CASE("an attempt whose second obligation fails reports only that failure") {
    // Obligation 1 passes at tick 3 and obligation 2 fails at tick 4. The pass was
    // never the attempt's -- an obligation was still owed -- so the only report is
    // the failure, and it is at the LATER cycle.
    //
    // Per-obligation verdicts print MM-PASS at 35 as well as failing at 45.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> ok) $display("[%0t] MM-PASS", $time);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1
    a <= 0; b <= 1; ok <= 1;
    @(posedge clk);            // tick2: the ##1 match ends
    @(posedge clk);            // tick3: the ##2 match ends; obligation 1 passes
    b <= 0; ok <= 0;
    @(posedge clk);            // tick4: obligation 2 FAILS
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "MM-PASS") == 0);
    CHECK(count_of(out, "assertion failed") == 1);
    CHECK(out.find("[45]\n") != std::string::npos);
}

TEST_CASE("an attempt's pass waits for its antecedent window to close") {
    // Only ONE obligation here: `b` is high at tick 2 alone, so `a ##[1:3] b`
    // matches once and the obligation passes at tick 3. The attempt still cannot
    // pass there -- its ##2 and ##3 threads are alive and could seed another
    // obligation -- so the verdict lands at tick 4, when the last of them dies.
    //
    // Reporting the pass where the obligation resolved puts it at 35.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:3] b) |=> ok) $display("[%0t] MM-PASS", $time);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1
    a <= 0; b <= 1; ok <= 1;
    @(posedge clk);            // tick2: the only match ends here
    b <= 0;
    @(posedge clk);            // tick3: the obligation passes; no ##2 match
    ok <= 0;
    @(posedge clk);            // tick4: no ##3 match -- the window closes
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "MM-PASS") == 1);
    CHECK(out.find("[45] MM-PASS") != std::string::npos);
}

TEST_CASE("a cover of an implication counts a twice-matching attempt once") {
    // A cover counts attempts, not obligations (LRM 16.12.6), so an antecedent matching twice with both consequents
    // holding is ONE hit -- in the cycle the attempt settles.
    //
    // Counting per obligation hits twice, at 35 and 45.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ci: cover property (@(posedge clk) (a ##[1:2] b) |=> ok) $display("[%0t] CI-HIT", $time);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1
    a <= 0; b <= 1; ok <= 1;
    @(posedge clk);            // tick2: the ##1 match ends
    @(posedge clk);            // tick3: the ##2 match ends; obligation 1 passes
    b <= 0;
    @(posedge clk);            // tick4: obligation 2 passes
    ok <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "CI-HIT") == 1);
    CHECK(out.find("[45] CI-HIT") != std::string::npos);
}

TEST_CASE("a cover sequence counts every match where a cover property counts attempts") {
    // The design of the case above, verbatim, with `cover property` replaced by
    // `cover sequence`. That is the whole difference the LRM draws (16.12.6): a
    // cover property reports how many ATTEMPTS succeeded, a cover sequence how
    // many times the sequence MATCHED, and an attempt can match more than once.
    //
    // Here the antecedent matches at tick2 and tick3, so the two obligations pass
    // at tick3 (35) and tick4 (45). Grouped into one attempt that is a single
    // cover-property hit at 45; counted per match it is two.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  cs: cover sequence (@(posedge clk) (a ##[1:2] b) |=> ok) $display("[%0t] CS-HIT", $time);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1
    a <= 0; b <= 1; ok <= 1;
    @(posedge clk);            // tick2: the ##1 match ends
    @(posedge clk);            // tick3: the ##2 match ends; obligation 1 passes
    b <= 0;
    @(posedge clk);            // tick4: obligation 2 passes
    ok <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "CS-HIT") == 2);
    CHECK(out.find("[35] CS-HIT") != std::string::npos);
    CHECK(out.find("[45] CS-HIT") != std::string::npos);
}

TEST_CASE("a restrict whose property fails reports nothing at all") {
    // The behavioural half of the structural check above, over the trace of "two
    // attempts of one assertion each keep their own verdict" below: two attempts
    // match `a ##1 b` and both consequents are low, so an assert reports at 35 and
    // 45. An empty report set only means something next to a non-empty one, so the
    // same trace is run both ways.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##1 b) |=> ok);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1: attempt A starts
    b <= 1;
    @(posedge clk);            // tick2: attempt B starts; A matches
    a <= 0;
    @(posedge clk);            // tick3: B matches; A's obligation FAILS
    b <= 0;
    @(posedge clk);            // tick4: B's obligation FAILS
    @(posedge clk);
    $finish;
  end
endmodule
)") == "35 45");

    // Same property, same trace, `restrict`: both attempts still fail, and the
    // directive still says nothing. LRM 16.12 gives restrict no simulation
    // verdict -- it constrains what a formal tool may drive, and slang enforces
    // the silence by rejecting an action block on one.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  restrict property (@(posedge clk) (a ##1 b) |=> ok);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1: attempt A starts
    b <= 1;
    @(posedge clk);            // tick2: attempt B starts; A matches
    a <= 0;
    @(posedge clk);            // tick3: B matches; A's obligation FAILS
    b <= 0;
    @(posedge clk);            // tick4: B's obligation FAILS
    @(posedge clk);
    $finish;
  end
endmodule
)").empty());
}

TEST_CASE("a cover does not count an attempt one of whose obligations failed") {
    // The same cover with the second consequent low. The attempt does not pass, so
    // it is not covered at all -- the earlier obligation's match is not a hit of
    // its own. This is the half of the cover rule that a per-obligation tally gets
    // wrong in the other direction, hitting once at 35.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ci: cover property (@(posedge clk) (a ##[1:2] b) |=> ok) $display("[%0t] CI-HIT", $time);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1
    a <= 0; b <= 1; ok <= 1;
    @(posedge clk);            // tick2: the ##1 match ends
    @(posedge clk);            // tick3: the ##2 match ends; obligation 1 passes
    b <= 0; ok <= 0;
    @(posedge clk);            // tick4: obligation 2 FAILS
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "CI-HIT") == 0);
}

TEST_CASE("two attempts of one assertion each keep their own verdict") {
    // The guard on the other side of the grouping: `a` is high at ticks 1 and 2, so
    // two DISTINCT attempts each match `a ##1 b` once and each fail, one cycle
    // apart. Grouping keyed on anything coarser than the attempt -- an
    // assertion-wide "already reported" flag, or an id ring too small to keep two
    // live attempts apart -- swallows the second.
    //
    // This one holds before the fix as well as after; it is here to fail if the
    // per-attempt machinery ever over-merges, which is the failure mode the rest of
    // these cases cannot see.
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##1 b) |=> ok);
  initial begin
    @(posedge clk);            // tick0
    a <= 1;
    @(posedge clk);            // tick1: attempt A starts
    b <= 1;
    @(posedge clk);            // tick2: a still high -> attempt B starts; A matches
    a <= 0;
    @(posedge clk);            // tick3: B matches; A's obligation FAILS
    b <= 0;
    @(posedge clk);            // tick4: B's obligation FAILS
    @(posedge clk);
    $finish;
  end
endmodule
)") == "35 45");
}

TEST_CASE("three attempts whose antecedent matches share an end cycle each report") {
    // `a` is high at ticks 0, 1 and 2, so three attempts start. `b` is high at tick
    // 3 alone, so the `##3` of the first, the `##2` of the second and the `##1` of
    // the third all end there, and `|->` checks `ok` in that same cycle. Three
    // attempts, one obligation, `ok` low: three failures owed, all at 35.
    //
    // Reporting per obligation reads "35": one report for the slot the three share.
    const std::string source = R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:3] b) |-> ok);
  initial begin
    a <= 1;
    @(posedge clk);            // tick0 (5):  attempt A starts
    @(posedge clk);            // tick1 (15): attempt B starts
    @(posedge clk);            // tick2 (25): attempt C starts
    a <= 0; b <= 1;
    @(posedge clk);            // tick3 (35): A's ##3, B's ##2 and C's ##1 all end
    b <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
)";
    // The tally and not just the log: three reports, and all three in the one cycle.
    CHECK(count_of(sva_run_capture(source), "assertion failed") == 3);
    CHECK(report_times(source) == "35 35 35");
}

TEST_CASE("two attempts sharing an end cycle report twice") {
    // The same rule at the smallest multiplicity that can show it. Two is worth
    // testing beside three because a fix that recovered only the first extra attempt --
    // reporting per slot plus one -- would pass the three-attempt case at 2 and this
    // one at 2 as well, so the two together pin the count rather than the delta.
    const std::string source = R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |-> ok);
  initial begin
    @(posedge clk);            // tick0 (5)
    a <= 1;
    @(posedge clk);            // tick1 (15): attempt A starts
    @(posedge clk);            // tick2 (25): attempt B starts
    a <= 0; b <= 1;
    @(posedge clk);            // tick3 (35): A's ##2 and B's ##1 both end
    b <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
)";
    CHECK(count_of(sva_run_capture(source), "assertion failed") == 2);
    CHECK(report_times(source) == "35 35");
}

TEST_CASE("attempts of a ranged antecedent resolving in different cycles still report each") {
    // The control. Same operator, same shape of trace, but `b` is high early enough
    // that each attempt's FIRST match ends in a cycle of its own -- so the two
    // attempts never share an obligation and the per-slot reporting this section
    // exists to fix was already right here. It passed before the fix and has to keep
    // passing, which is what separates "the count was wrong" from "the reports moved".
    CHECK(report_times(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |-> ok);
  initial begin
    @(posedge clk);            // tick0 (5)
    a <= 1;
    @(posedge clk);            // tick1 (15): attempt A starts
    b <= 1;
    @(posedge clk);            // tick2 (25): attempt B starts; A's ##1 ends -> FAIL
    a <= 0;
    @(posedge clk);            // tick3 (35): B's ##1 ends -> FAIL
    b <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
)") == "25 35");
}

TEST_CASE("a cover counts every attempt sharing an end cycle") {
    // The tally rather than the log, which is what makes the shared end cycle a wrong
    // ANSWER and not a cosmetic duplicate-suppression. The three attempts of the first
    // case with `ok` high, so all three pass and a `cover property` is owed three
    // hits. Each attempt's hit lands in the cycle it goes quiet -- A and B at 45, C
    // one cycle later because its `##3` window has not closed until 55 -- so the
    // count is the claim and the ticks come with it.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ci: cover property (@(posedge clk) (a ##[1:3] b) |-> ok) $display("[%0t] CI-HIT", $time);
  initial begin
    a <= 1; ok <= 1;
    @(posedge clk);            // tick0 (5):  attempt A starts
    @(posedge clk);            // tick1 (15): attempt B starts
    @(posedge clk);            // tick2 (25): attempt C starts
    a <= 0; b <= 1;
    @(posedge clk);            // tick3 (35): A's ##3, B's ##2 and C's ##1 all end
    b <= 0;
    @(posedge clk);            // tick4 (45): A and B can match no more -> two hits
    @(posedge clk);            // tick5 (55): C's window closes -> the third hit
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "CI-HIT") == 3);
    CHECK(out.find("[45] CI-HIT") != std::string::npos);
    CHECK(out.find("[55] CI-HIT") != std::string::npos);
}

TEST_CASE("a counted obligation seeded by several attempts at once owes each a verdict") {
    // The same drop reached by the other discipline, and the reason the diagnosis had
    // to name where the count was lost rather than just make the case above print 3.
    // A counted monitor holds its pending attempts as a number per state instead of a
    // slot each, so nothing here reports per slot -- and it was still wrong, because
    // the SEED added 1 where three antecedent matches had ended. One number too small
    // from the start.
    //
    // `s_eventually` makes the obligation counted and strong, so the three attempts
    // are still pending when the trace ends and each owes a failure there.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:3] b) |-> s_eventually ok);
  initial begin
    a <= 1;
    @(posedge clk);            // tick0 (5):  attempt A starts
    @(posedge clk);            // tick1 (15): attempt B starts
    @(posedge clk);            // tick2 (25): attempt C starts
    a <= 0; b <= 1;
    @(posedge clk);            // tick3 (35): A's ##3, B's ##2 and C's ##1 all end
    b <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "assertion failed") == 3);
}

TEST_CASE("one owner of a shared obligation failing does not silence the other") {
    // The sharp edge of letting one obligation stand for several attempts: what
    // happens when the attempts stop agreeing. A fails on an EARLIER obligation of its
    // own while the LATER obligation it shares with B is still pending, and a failure
    // resolves the whole attempt -- so A's share of that obligation is abandoned. B's
    // is not, and freeing the slot on the first owner to fail loses B with no verdict
    // at all.
    //
    // `a` at ticks 1 and 2 with `b` at 2 and 3: A matches `a ##1 b` at 25 and
    // `a ##2 b` at 35, B matches `a ##1 b` at 35. `|=>` puts A's first obligation at
    // 35..45 and BOTH A's second and B's only at 45..55 -- one slot, two owners. `q`
    // is low at 45, so A's first obligation fails there; `q` is high at 55, so the
    // shared one matches and that match is B's.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, p = 0, q = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> (p ##1 q)) $display("[%0t] MM-PASS", $time);
  initial begin
    @(posedge clk);            // tick0 (5)
    a <= 1;
    @(posedge clk);            // tick1 (15): attempt A starts
    b <= 1;
    @(posedge clk);            // tick2 (25): attempt B starts; A's ##1 match ends
    a <= 0; p <= 1;
    @(posedge clk);            // tick3 (35): A's ##2 and B's ##1 end; A's first obligation seeded
    b <= 0;
    @(posedge clk);            // tick4 (45): the shared obligation is seeded; A's first FAILS
    q <= 1;
    @(posedge clk);            // tick5 (55): the shared obligation matches -- for B
    @(posedge clk);
    $finish;
  end
endmodule
)");
    // A's one failure, and no second one: the shared obligation is not A's to fail
    // again.
    CHECK(count_of(out, "assertion failed") == 1);
    // B's pass, which abandoning the whole slot swallows.
    CHECK(count_of(out, "MM-PASS") == 1);
    CHECK(out.find("[55] MM-PASS") != std::string::npos);
}

TEST_CASE("both owners of a shared obligation pass when it matches") {
    // The control for the case above, and the proof that its slot really does have two
    // owners rather than B never having had an obligation at all. Same trace with `q`
    // high at 45 too, so A's first obligation MATCHES instead of failing and neither
    // attempt is resolved early: the shared obligation matches at 55 and both owners
    // pass off it.
    const std::string out = sva_run_capture(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, p = 0, q = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> (p ##1 q)) $display("[%0t] MM-PASS", $time);
  initial begin
    @(posedge clk);            // tick0 (5)
    a <= 1;
    @(posedge clk);            // tick1 (15): attempt A starts
    b <= 1;
    @(posedge clk);            // tick2 (25): attempt B starts; A's ##1 match ends
    a <= 0; p <= 1;
    @(posedge clk);            // tick3 (35): A's ##2 and B's ##1 end; A's first obligation seeded
    b <= 0;
    q <= 1;
    @(posedge clk);            // tick4 (45): the shared obligation is seeded; A's first MATCHES
    @(posedge clk);            // tick5 (55): the shared obligation matches
    @(posedge clk);
    $finish;
  end
endmodule
)");
    CHECK(count_of(out, "assertion failed") == 0);
    CHECK(count_of(out, "MM-PASS") == 2);
}

// -- Assertion control (LRM 20.12) -------------------------------------------
//
// One trace serves the first four cases, so that what changes between them is
// only the control task. `a` is high and `b` low for the whole run, so `a |=> b`
// starts an attempt at every tick and every one of them fails one tick later:
// ticks at 5, 15, ..., 75, and an unmodified run reports at 15 through 75.
//
// The control tasks fire at 22 and 52, BETWEEN ticks, which is the interesting
// placement: at 22 the attempt whose antecedent matched at 15 is already in
// flight and owes a verdict at 25, and that verdict is exactly what separates
// `off` from `kill`.
constexpr std::string_view control_trace = R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1;
  bit b = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) a |=> b);
  initial begin
    #22 CONTROL_OFF;
    #30 CONTROL_ON;
  end
  initial #78 $finish;
endmodule
)";

std::string control_run(std::string_view stop, std::string_view start) {
    return report_times(substituted(substituted(control_trace, "CONTROL_OFF", stop), "CONTROL_ON", start));
}

TEST_CASE("assertion control leaves an uncontrolled design alone") {
    // The baseline the three cases below are read against. It also pins the
    // shape of the trace: one report per tick from 15, so any hole in a later
    // case is a suppression and not an artefact of the stimulus.
    CHECK(control_run("$display(\"[control]\")", "$display(\"[control]\")") ==
          "15 25 35 45 55 65 75");
}

TEST_CASE("$assertoff stops attempts starting and lets a pending one finish") {
    // THE case for what `off` means (LRM 20.12): "No new attempts will be
    // started. Attempts that are already executing ... are not affected."
    //
    // 25 survives and 35 does not, and the pair is the whole semantics. The
    // attempt reporting at 25 was seeded by an antecedent that matched at 15,
    // before the task ran -- it is in flight, so it runs to its verdict. The one
    // that would have reported at 35 needed a fresh antecedent match at 25, and
    // that is a new attempt, which is what the enable gates.
    //
    // Gate the advance or the verdict instead of the seed and 25 disappears with
    // the rest. Gate nothing and the run is the baseline. Resuming at 52 puts the
    // first new antecedent match at 55 and so the first new report at 65: an
    // implication costs a tick between the two, and reading `15 25 55 ...` here
    // would mean the seed was let through a tick early.
    CHECK(control_run("$assertoff", "$asserton") == "15 25 65 75");
}

TEST_CASE("$assertkill also abandons the attempt $assertoff would have finished") {
    // Same trace, same instants, one word different -- and 25 is gone. That is
    // the only difference `kill` is allowed to make over `off`, so a change here
    // that also moved 65 or 75 would mean the kill outlived its cycle rather than
    // being the one-cycle reset it is built as.
    CHECK(control_run("$assertkill", "$asserton") == "15 65 75");
}

TEST_CASE("$assertfailoff hides the report while the automaton keeps running") {
    // The filters are not the enable. `55` is the proof: its attempt's antecedent
    // matched at 45, while reporting was off, and the attempt still ran to a
    // verdict that lands once reporting is back. Under `$assertoff` the same
    // instants give `15 25 65 75` -- no 55, because no attempt started at 45 to
    // report at 55, and an extra 25, because the filter suppresses a verdict the
    // enable never stops.
    //
    // So the two mechanisms differ in both directions on one trace, which is what
    // makes this case able to fail if they were ever wired to the same bit.
    CHECK(control_run("$assertfailoff", "$assertfailon") == "15 55 65 75");
}

TEST_CASE("a bare sequence takes the same control as an implication") {
    // The other seed path. With an antecedent the enable gates the trigger's
    // first states and the seed is left alone -- an in-flight attempt reaching
    // its obligation must not be stopped. With no antecedent there is no trigger
    // to gate and the seed IS the start, so it takes the gate instead. Two
    // different lines of emit_monitor, and this pins the second to the same
    // observable behaviour as the first.
    //
    // `a ##1 b` with a high and b low starts an attempt every tick and fails it
    // the next, so the report sets are the ones the implication cases produce,
    // instant for instant.
    const auto bare = [](std::string_view stop, std::string_view start) {
        return report_times(substituted(substituted(R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 0;
  always @(posedge clk) $display("[%0t]", $time);
  ap: assert property (@(posedge clk) a ##1 b);
  initial begin
    #22 CONTROL_OFF;
    #30 CONTROL_ON;
  end
  initial #78 $finish;
endmodule
)",
                                                    "CONTROL_OFF",
                                                    stop),
                                       "CONTROL_ON",
                                       start));
    };

    CHECK(bare("$display(\"[control]\")", "$display(\"[control]\")") == "15 25 35 45 55 65 75");
    CHECK(bare("$assertoff", "$asserton") == "15 25 65 75");
    CHECK(bare("$assertkill", "$asserton") == "15 65 75");
    CHECK(bare("$assertfailoff", "$assertfailon") == "15 55 65 75");
}

// Three assertions, one per level: `tp` in the top, `mp` in u1, `lp` in u1.u2.
// All three fail at every tick from 15, so a report set counts how many are
// still enabled -- "35" means one survived that tick and "35 35" means two.
constexpr std::string_view control_hier = R"(
module leaf(input bit clk, input bit a, input bit b);
  lp: assert property (@(posedge clk) a |=> b);
endmodule
module mid(input bit clk, input bit a, input bit b);
  mp: assert property (@(posedge clk) a |=> b);
  leaf u2(.clk(clk), .a(a), .b(b));
endmodule
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 0;
  always @(posedge clk) $display("[%0t]", $time);
  mid u1(.clk(clk), .a(a), .b(b));
  tp: assert property (@(posedge clk) a |=> b);
  initial #22 CONTROL;
  initial #48 $finish;
endmodule
)";

std::string control_hier_run(std::string_view control) {
    return report_times(substituted(control_hier, "CONTROL", control));
}

TEST_CASE("an assertion control task addresses the scope it names") {
    // Ticks 15 and 25 are three reports each either way: 15 is before the task
    // runs, and 25 belongs to attempts already in flight at 22. What the target
    // decides is 35 and 45.
    //
    // Every assertion, named by leaving the list off entirely.
    CHECK(control_hier_run("$assertoff") == "15 15 15 25 25 25");
    // u1 and everything under it: `mp` and `lp` stop, `tp` in the top does not.
    CHECK(control_hier_run("$assertoff(0, u1)") == "15 15 15 25 25 25 35 45");
    // One assertion by name. A label makes a scope slang resolves the
    // hierarchical name to, which is what LRM 20.12's list_of_scopes_or_assertions
    // means by naming an assertion.
    CHECK(control_hier_run("$assertoff(0, u1.u2.lp)") == "15 15 15 25 25 25 35 35 45 45");
}

TEST_CASE("an assertion control task's levels bounds how far down it reaches") {
    // 1 is the named scope alone (the $dumpvars rule 20.12 defers to), so `mp`
    // stops and `lp` one level below it does not: two reports left per tick.
    CHECK(control_hier_run("$assertoff(1, u1)") == "15 15 15 25 25 25 35 35 45 45");
    // 2 reaches u1.u2 as well, and 0 means every level below -- the same set
    // here, which is what makes the pair a check on the counting rather than on
    // one arithmetic accident.
    CHECK(control_hier_run("$assertoff(2, u1)") == "15 15 15 25 25 25 35 45");
    CHECK(control_hier_run("$assertoff(0, u1)") == "15 15 15 25 25 25 35 45");
}

// A cover and an assert side by side, each with an action block that prints its
// own marker, so a directive_type filter is visible as one running while the
// other does not. The cover is a bare sequence: an implication would cover
// vacuously and slang warns about exactly that.
constexpr std::string_view control_actions = R"(
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 1, b = 1;
  always @(posedge clk) $display("[%0t]", $time);
  cp: cover property (@(posedge clk) a ##1 b) $display("cover hit");
  ap: assert property (@(posedge clk) a |=> b) $display("assert pass");
  initial begin CONTROL end
  initial #38 $finish;
endmodule
)";

std::string control_action_run(std::string_view control, std::string_view marker) {
    return report_times(substituted(control_actions, "CONTROL", control), marker);
}

TEST_CASE("$assertcontrol's directive_type picks which directives it controls") {
    // Both actions run at 15, 25 and 35 with nothing controlling them.
    CHECK(control_action_run("", "cover hit") == "15 25 35");
    CHECK(control_action_run("", "assert pass") == "15 25 35");
    // PassOff over concurrent ASSERT directives only (LRM Table 20-7 bit 1): the
    // assert's pass action stops and the cover's match action does not.
    CHECK(control_action_run("#12 $assertcontrol(7, 1, 1);", "assert pass") == "");
    CHECK(control_action_run("#12 $assertcontrol(7, 1, 1);", "cover hit") == "15 25 35");
    // Bit 2 is COVER, and selects the other one.
    CHECK(control_action_run("#12 $assertcontrol(7, 1, 2);", "assert pass") == "15 25 35");
    CHECK(control_action_run("#12 $assertcontrol(7, 1, 2);", "cover hit") == "");
}

TEST_CASE("$assertcontrol LOCK holds a control state against a later task") {
    // Locked at 7, so the PassOff at 12 writes nothing and both actions keep
    // running. Without the lock the same PassOff silences both, which is the
    // second line -- so this fails if the lock select were dropped from the
    // control writes rather than merely reordered.
    CHECK(control_action_run("#7 $assertcontrol(1); #12 $assertpassoff;", "cover hit") == "15 25 35");
    CHECK(control_action_run("#7 $assertcontrol(1); #12 $assertpassoff;", "assert pass") == "15 25 35");
    CHECK(control_action_run("#12 $assertpassoff;", "cover hit") == "");
    // UNLOCK is not itself blocked by the lock it lifts, or a lock would be
    // permanent. The PassOff lands at 19, after the hit at 15 and before the
    // ones at 25 and 35, so those two are the ones it takes.
    CHECK(control_action_run("#7 $assertcontrol(1); #10 $assertcontrol(2); #2 $assertpassoff;", "cover hit") == "15");
}

TEST_CASE("a non-constant assertion control argument is refused") {
    // The control code decides which bits are emitted into which assertions, so
    // it is not a value a running process can supply -- and the message says so
    // rather than reporting the task as unsupported, which would be wrong.
    refuses(R"(
module t;
  bit clk = 0;
  int c;
  bit a = 1, b = 0;
  ap: assert property (@(posedge clk) a |=> b);
  initial begin
    c = 3;
    $assertcontrol(c);
  end
endmodule
)",
            "control_type argument of an assertion control task must be a constant");

    // The same for `levels`, which decides how much of the hierarchy is written.
    refuses(R"(
module t;
  bit clk = 0;
  int n;
  bit a = 1, b = 0;
  ap: assert property (@(posedge clk) a |=> b);
  initial begin
    n = 1;
    $assertoff(n);
  end
endmodule
)",
            "levels argument of an assertion control task must be a constant");

    // A code outside LRM Table 20-5 is a design error rather than a gap, and is
    // named as one.
    refuses(R"(
module t;
  bit clk = 0;
  bit a = 1, b = 0;
  ap: assert property (@(posedge clk) a |=> b);
  initial $assertcontrol(99);
endmodule
)",
            "control_type 99 is not one of LRM Table 20-5's codes");
}

TEST_CASE("assertion control over an immediate assertion is refused, not ignored") {
    // The default assertion_type covers immediate assertions, which lower inline
    // with no state to gate. Leaving them running under `$assertoff` would be a
    // silent divergence, so the task refuses and says how to narrow it.
    refuses(R"(
module t;
  bit clk = 0;
  bit a = 1, b = 0;
  ap: assert property (@(posedge clk) a |=> b);
  always @(posedge clk) assert (a);
  initial #12 $assertoff;
endmodule
)",
            "over immediate assertions is not supported");
}

TEST_CASE("a design with no control task carries no control state") {
    // The whole mechanism is allocated only for a design that calls a control
    // task, so an ordinary assertion's emitted state is exactly what it was.
    LowerResult       out;
    const ir::Module& m = lower_for_monitor(R"(
module t;
  bit clk;
  bit a, b;
  ap: assert property (@(posedge clk) a |=> b);
endmodule
)",
                                            out);
    CHECK(out.ok);
    for (const ir::SignalInfo& s : m.signals()) {
        CHECK(s.name.find("sva.ctl") == std::string::npos);
    }

    // And a design that does call one carries exactly the five bits, once.
    LowerResult       ctl;
    const ir::Module& cm = lower_for_monitor(R"(
module t;
  bit clk;
  bit a, b;
  ap: assert property (@(posedge clk) a |=> b);
  initial $assertoff;
endmodule
)",
                                             ctl);
    CHECK(ctl.ok);
    CHECK(find_signal(cm, "sva.ctl0.off") != nullptr);
    CHECK(find_signal(cm, "sva.ctl0.kill") != nullptr);
    CHECK(find_signal(cm, "sva.ctl0.poff") != nullptr);
    CHECK(find_signal(cm, "sva.ctl0.foff") != nullptr);
    CHECK(find_signal(cm, "sva.ctl0.lock") != nullptr);
    CHECK(find_signal(cm, "sva.ctl1.off") == nullptr);
    // The monitor's own state is untouched by any of it.
    CHECK(state_width(cm, "sva.0.pend0") == 1);
    CHECK(state_width(cm, "sva.0.clk") == 1);
}