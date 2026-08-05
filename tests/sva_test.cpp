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

// The one Always process a monitor contributes. A design that is nothing but an
// assertion has no other: no continuous assign, no always block, no port copy.
const ir::Process& monitor_process(const ir::Module& m) {
    const ir::Process* found = nullptr;
    for (const ir::Process& p : m.processes()) {
        if (p.activation == ir::Activation::Always) {
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

} // namespace

TEST_CASE("a nonoverlapping implication emits a monitor process and its state") {
    LowerResult       r;
    const ir::Module& m = lower_for_monitor(implication, r);
    CHECK(r.ok);

    REQUIRE(m.monitors.size() == 1);
    const ir::Process& p = monitor_process(m);
    // One cycle is one clock edge, and nothing else may step the monitor -- not
    // even time zero, which is why it does not self-start.
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
    CHECK(activation_count(m, ir::Activation::Always) == 2);
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

    // A cover has no failure verdict (LRM 16.12.6): it tallies matches and emits
    // no report at all.
    CHECK(find_signal(cm, "sva.0.cov") != nullptr);
    CHECK(state_width(cm, "sva.0.cov") == 32);
    CHECK_FALSE(has_tag(monitor_process(cm), ir::Inst::Tag::Severity));

    LowerResult       as;
    const ir::Module& am = lower_for_monitor(R"(
module t;
  bit clk, a, b;
  ap: assert property (@(posedge clk) a ##1 b);
endmodule
)", as);
    CHECK(as.ok);
    CHECK(find_signal(am, "sva.0.cov") == nullptr);
    CHECK(has_tag(monitor_process(am), ir::Inst::Tag::Severity));
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
