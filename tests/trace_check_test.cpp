// The scheduler trace's checker (docs/design/scheduler-trace.md): whether the build plants the
// events at all, the predicates of rule R5 over event sequences, the report of a violation, a
// run that breaks a rule, and the suite's known list.
//
// A design cannot break a rule of the scheduler while the scheduler keeps it: a violation is a
// Clotho bug. So the predicates are tested over the events directly -- the sequences of the bugs
// the checker exists for (docs/design/scheduler-rules.md §4), which stay proven after the bugs
// are fixed -- and the run-ending mechanics through a design whose DPI-C import reports one
// event the scheduler never made.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "Subprocess.h"
#include "TraceCheck.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "runtime/Trace.h"

using namespace clotho;

namespace trace_check = clotho::test::trace_check;

namespace {

// The unoptimized modules of a design with an always that parks mid-body, as text.
std::string module_text(const ir::SimulationConfig& config) {
    const LowerResult r = lower_text(R"(
        module t;
          reg clk = 0, done = 0;
          always @(posedge clk) begin @(posedge done); end
          initial begin #1 clk = 1; #1 done = 1; end
        endmodule)");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    auto host = ir::host_target();
    REQUIRE(static_cast<bool>(host));
    auto tm = host->createTargetMachine();
    REQUIRE(static_cast<bool>(tm));

    llvm::LLVMContext ctx;
    const auto        built = ir::build_modules_unoptimized(*r.module, ctx, **tm, config);
    REQUIRE_FALSE(built.modules.empty());
    std::string              text;
    llvm::raw_string_ostream os(text);
    for (const auto& m : built.modules) {
        m->print(os, nullptr);
    }
    return text;
}

bool plants_trace(const ir::SimulationConfig& config) {
    return module_text(config).find("rt_trace_") != std::string::npos;
}

} // namespace

// Planted before optimization, exactly when CLOTHO_EXTRA_ASSERTS is defined and the config asks
// for the check, and never otherwise: a shipped build emits no call.
TEST_CASE("the trace events are generated only in a CLOTHO_EXTRA_ASSERTS build") {
    ir::SimulationConfig off;
    off.trace_check = false;
    CHECK_FALSE(plants_trace(off));

    ir::SimulationConfig asked;
    asked.trace_check = true;
#ifdef CLOTHO_EXTRA_ASSERTS
    CHECK(plants_trace(asked));
    // On by default, unless the environment turns the mode off.
    CHECK(plants_trace(ir::SimulationConfig{}) == (std::getenv("CLOTHO_NO_TRACE_CHECK") == nullptr));
#else
    // Asking for the check in a build without it changes nothing.
    CHECK_FALSE(plants_trace(asked));
    CHECK_FALSE(plants_trace(ir::SimulationConfig{}));
#endif
}

#if !defined(_WIN32)
// The test binary exports its symbols (ENABLE_EXPORTS), as the DPI-C imports defined in tests
// need, so a symbol the build links is found by name and one it does not is not.
TEST_CASE("a build without CLOTHO_EXTRA_ASSERTS has no trace checker symbol") {
    REQUIRE(dlsym(RTLD_DEFAULT, "rt_run") != nullptr);
    for (const char* name : { "rt_trace_begin", "rt_trace_spawn", "rt_trace_park", "rt_trace_wake", "rt_trace_dispatch",
                              "rt_trace_resume", "rt_trace_end", "rt_trace_slot", "rt_trace_halted", "rt_trace_region",
                              "rt_trace_nba_create", "rt_trace_nba_stage", "rt_trace_nba_release", "rt_trace_nba_pass",
                              "rt_trace_pend", "rt_trace_trigger", "rt_trace_slot_end", "rt_trace_finish",
                              "rt_trace_change", "rt_trace_block", "rt_trace_disable", "rt_trace_abort", "rt_trace_once",
                              "rt_trace_fork", "rt_trace_child_done", "rt_trace_join", "rt_trace_members", "rt_trace_member", "rt_trace_inertial_arm", "rt_trace_inertial_decide",
                              "rt_trace_inertial_fire", "rt_trace_inertial_applied", "rt_trace_nba_event" }) {
#ifdef CLOTHO_EXTRA_ASSERTS
        CHECK_MESSAGE(dlsym(RTLD_DEFAULT, name) != nullptr, name);
#else
        CHECK_MESSAGE(dlsym(RTLD_DEFAULT, name) == nullptr, name);
#endif
    }
}
#endif

#ifdef CLOTHO_EXTRA_ASSERTS

namespace {

// The park sites and origin of the designs below, by what the report names.
const rt_trace_control kProcess{ "design.sv:3:3", RT_TRACE_ORG_PROCESS };
const rt_trace_control kStatic{ "design.sv:3:3", RT_TRACE_CTL_STATIC };
const rt_trace_control kHead{ "design.sv:3:3", RT_TRACE_CTL_EVENT };
const rt_trace_control kWait{ "design.sv:3:30", RT_TRACE_CTL_EVENT };
const rt_trace_control kDelay{ "design.sv:7:5", RT_TRACE_CTL_DELAY };
const rt_trace_control kJoin{ "design.sv:9:5", RT_TRACE_CTL_JOIN };
const rt_trace_control kCall{ "design.sv:11:5", RT_TRACE_CTL_CALL };

// A bracket's report of a write, before the wakes it makes (rt_trace_change): a posedge of `x` as
// far as the code's facts say.
const rt_trace_change_site kStore{ "design.sv:8:3", "clk", RT_TRACE_CHG_STORE };

void wrote_posedge(const void* x) {
    rt_trace_change(&kStore, x, RT_TRACE_SEEN_POS_KNOWN | RT_TRACE_SEEN_POS);
}

// Signal storage and activations, by address.
int  clk, done;
char proc_a, proc_b, proc_c;

std::string rule() {
    const char* r = rt_trace_rule();
    return r != nullptr ? r : "";
}

bool has(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// The always of scheduler-rules.md §4 as the scheduler runs it with the B51 fix: it waits at its
// head for `clk` as a dynamic waiter, and parks mid-body at `@(posedge done)`.
void head_wait(void* a) {
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_park(a, &kHead, 0);
}

void body_wait(void* a) {
    rt_trace_waits_for(a, &done, "done", RT_TRACE_POS, nullptr);
    rt_trace_park(a, &kWait, 0);
}

} // namespace

TEST_CASE("trace check: an always that waits at its head and parks mid-body breaks no rule") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_wake(a, RT_TRACE_SEED, nullptr, 0);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    head_wait(a);

    rt_trace_slot(1);
    wrote_posedge(&clk);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_resume(a, &kHead);
    body_wait(a);

    // The second posedge of clk finds no entry: the body is not waiting for clk.
    rt_trace_slot(3);
    rt_trace_slot(4);
    wrote_posedge(&done);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &done, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_resume(a, &kWait);
    head_wait(a);

    CHECK(rt_trace_halted() == 0);
    CHECK(rule().empty());
}

// B51: the always was subscribed to clk statically and parked at @(posedge done), so the next
// posedge of clk woke it at a control that does not list clk.
TEST_CASE("trace check: a static subscription that wakes an activation parked mid-body is flagged (B51)") {
    rt_trace_begin();
    void*                 a     = &proc_a;
    const rt_trace_entry  sub[] = { { &clk, "clk", RT_TRACE_POS, nullptr } };
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_subscribe(a, sub, 1);
    rt_trace_park(a, &kStatic, 0);

    rt_trace_slot(1);
    wrote_posedge(&clk);
    rt_trace_wake(a, RT_TRACE_STATIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    body_wait(a);
    CHECK(rt_trace_halted() == 0);

    rt_trace_slot(3);
    rt_trace_wake(a, RT_TRACE_STATIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_halted() == 1);
    CHECK(rule() == "R5.1");

    const std::string report = rt_trace_report();
    CHECK(has(report, "rule R5.1 (wake)"));
    CHECK(has(report, "tick 3"));
    CHECK(has(report, "design.sv:3:3"));
    CHECK(has(report, "parked at event control at design.sv:3:30, waiting for posedge done"));
    CHECK(has(report, "a static subscription: posedge clk"));
    CHECK(has(report, "last events of this activation"));

    // The run is over: later events are ignored, and no dispatch goes ahead.
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 0);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &done, RT_TRACE_POS);
    CHECK(rule() == "R5.1");
}

// The same bug where the control lists the signal and the edge the static entry names: a static
// entry is inert unless the activation waits at its static control.
TEST_CASE("trace check: a static wake of an activation parked at an event control is flagged even when it lists the signal") {
    rt_trace_begin();
    void*                a     = &proc_a;
    const rt_trace_entry sub[] = { { &clk, "clk", RT_TRACE_POS, nullptr } };
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_subscribe(a, sub, 1);
    rt_trace_park(a, &kStatic, 0);
    wrote_posedge(&clk);
    rt_trace_wake(a, RT_TRACE_STATIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_park(a, &kWait, 0);

    rt_trace_wake(a, RT_TRACE_STATIC, &clk, RT_TRACE_POS);
    CHECK(rule() == "R5.1");
    CHECK(has(rt_trace_report(), "not parked at its static control"));
}

// B3 and B44: an activation pushed twice before it runs.
TEST_CASE("trace check: a wake of a queued activation is flagged (B3, B44)") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    head_wait(a);
    wrote_posedge(&clk);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_halted() == 0);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rule() == "R5.1");
    CHECK(has(rt_trace_report(), "already queued"));
}

// B13 and B44: a `disable` wakes an activation parked on a #delay, and its timer wakes it again.
TEST_CASE("trace check: a force wake and then the timer's wake of one park is flagged (B13, B44)") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_park(a, &kDelay, 5);
    rt_trace_slot(2);
    rt_trace_wake(a, RT_TRACE_FORCE, nullptr, 0);
    CHECK(rt_trace_halted() == 0);
    rt_trace_slot(5);
    rt_trace_wake(a, RT_TRACE_TIMER, nullptr, 0);
    CHECK(rule() == "R5.1");
    CHECK(has(rt_trace_report(), "already queued"));
}

// B13: the second wake reaches the activation after it ran to its end, and its frame is gone.
TEST_CASE("trace check: an activation that ended gets no wake and no dispatch (B13)") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_park(a, &kDelay, 5);
    rt_trace_slot(2);
    rt_trace_wake(a, RT_TRACE_FORCE, nullptr, 0);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_resume(a, &kDelay);
    rt_trace_end(a);
    CHECK(rt_trace_halted() == 0);

    // The timer fires on a frame that has been freed, and the entry that was queued for it too.
    rt_trace_slot(5);
    rt_trace_wake(a, RT_TRACE_TIMER, nullptr, 0);
    CHECK(rule() == "R5.4");
    CHECK(has(rt_trace_report(), "already ended"));

    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_end(a);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 0);
    CHECK(rule() == "R5.4");
}

// An address is reused once its frame is freed: the spawn that creates the new activation starts it
// afresh.
TEST_CASE("trace check: a spawn reuses the address of an activation that ended") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_end(a);
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_wake(a, RT_TRACE_SPAWN, nullptr, 0);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    CHECK(rt_trace_halted() == 0);
}

// B28 and B32: a child that runs inline on its parent's stack ends the block the parent is in, and
// the force wake pushes the parent while it is running.
TEST_CASE("trace check: a wake of the parent that runs a child inline is flagged (B28, B32)") {
    rt_trace_begin();
    void* parent = &proc_a;
    void* child  = &proc_b;
    rt_trace_spawn(parent, &kProcess, nullptr);
    rt_trace_dispatch(parent, RT_TRACE_INLINE);
    rt_trace_spawn(child, &kProcess, parent);
    rt_trace_dispatch(child, RT_TRACE_INLINE);
    rt_trace_wake(parent, RT_TRACE_FORCE, nullptr, 0);
    CHECK(rule() == "R5.1");
    CHECK(has(rt_trace_report(), "is running"));
}

TEST_CASE("trace check: a join, a task call and a delay each wake only their own park") {
    rt_trace_begin();
    void* parent = &proc_a;
    void* child  = &proc_b;
    rt_trace_spawn(parent, &kProcess, nullptr);
    rt_trace_dispatch(parent, RT_TRACE_INLINE);
    rt_trace_spawn(child, &kProcess, parent);
    rt_trace_dispatch(child, RT_TRACE_INLINE);
    rt_trace_park(child, &kDelay, 0);
    rt_trace_park(parent, &kJoin, 0);
    rt_trace_wake(child, RT_TRACE_INACTIVE, nullptr, 0);
    rt_trace_dispatch(child, RT_TRACE_RING);
    rt_trace_resume(child, &kDelay);
    rt_trace_end(child);
    rt_trace_wake(parent, RT_TRACE_CHILD, nullptr, 0);
    CHECK(rt_trace_halted() == 0);
    rt_trace_dispatch(parent, RT_TRACE_RING);
    rt_trace_resume(parent, &kJoin);
    rt_trace_park(parent, &kCall, 0);
    // A child's completion cannot wake a caller parked at a task call.
    rt_trace_wake(parent, RT_TRACE_CHILD, nullptr, 0);
    CHECK(rule() == "R5.1");
    CHECK(has(rt_trace_report(), "not parked at a join"));
}

TEST_CASE("trace check: a timer wakes an activation at its deadline and no other time") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_slot(2);
    rt_trace_park(a, &kDelay, 3);
    rt_trace_slot(4);
    rt_trace_wake(a, RT_TRACE_TIMER, nullptr, 0);
    CHECK(rule() == "R5.1");
    CHECK(has(rt_trace_report(), "the delay ends at tick 5, not at tick 4"));

    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_slot(2);
    rt_trace_park(a, &kDelay, 3);
    rt_trace_slot(5);
    rt_trace_wake(a, RT_TRACE_TIMER, nullptr, 0);
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a posedge wakes an edge, a change and a posedge entry, and a negedge only the others") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_NEG, nullptr);
    rt_trace_park(a, &kWait, 0);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rule() == "R5.1");

    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_ANY, nullptr);
    rt_trace_park(a, &kWait, 0);
    wrote_posedge(&clk);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_halted() == 0);

    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_park(a, &kWait, 0);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_ANY);
    CHECK(rule() == "R5.1");
}

TEST_CASE("trace check: a park the trace does not report fails its resume, and an event of an unknown activation fails") {
    rt_trace_begin();
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_park(a, &kDelay, 1);
    rt_trace_slot(1);
    rt_trace_wake(a, RT_TRACE_TIMER, nullptr, 0);
    rt_trace_dispatch(a, RT_TRACE_RING);
    // The activation parked somewhere the checker was not told of, and resumed there.
    rt_trace_resume(a, &kWait);
    CHECK(rule() == "R5.3");
    CHECK(has(rt_trace_report(), "a park the trace does not report"));

    rt_trace_begin();
    rt_trace_wake(&proc_c, RT_TRACE_TIMER, nullptr, 0);
    CHECK(rule() == "R5.1");
    CHECK(has(rt_trace_report(), "no spawn announced"));
}

TEST_CASE("trace check: a run starts clean and the counts only grow") {
    rt_trace_counts before{};
    rt_trace_totals(&before);
    rt_trace_begin();
    CHECK(rt_trace_halted() == 0);
    CHECK(rt_trace_rule() == nullptr);
    void* a = &proc_a;
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_park(a, &kDelay, 1);
    rt_trace_slot(1);
    rt_trace_wake(a, RT_TRACE_TIMER, nullptr, 0);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.runs == before.runs + 1);
    CHECK(after.spawns == before.spawns + 1);
    CHECK(after.parks == before.parks + 1);
    CHECK(after.wakes == before.wakes + 1);
    CHECK(after.dispatches == before.dispatches + 1);
    CHECK(after.events > before.events);
}

// ---- Nonblocking updates, `->>` and `$finish` (R12, R20, R24) ----
//
// Each sequence below is the events the scheduler and the generated code report, with the stores of
// the NBA commit made by hand between the beginning and the end of a pass. The commit that follows
// creation order leaves a target as the order says; the commit of each bug the rules exist for
// (docs/design/scheduler-rules.md §4) leaves it as that bug did, and the checker names the rule.

namespace {

// The statements: a plain NBA, a `<= #0`, a `<= #d` and a `<= @e`, each to a 32-bit 2-state target.
const rt_trace_nba_site kBlock{ "design.sv:5:3", "q", RT_TRACE_NBA_BLOCK, 32, 0 };
const rt_trace_nba_site kZero{ "design.sv:6:3", "q", RT_TRACE_NBA_ZERO, 32, 0 };
const rt_trace_nba_site kTimer{ "design.sv:7:3", "q", RT_TRACE_NBA_TIMER, 32, 0 };
const rt_trace_nba_site kEvent{ "design.sv:8:3", "q", RT_TRACE_NBA_EVENT, 32, 0 };
// A forced target: the commit may drop its update.
const rt_trace_nba_site kGated{ "design.sv:9:3", "f", RT_TRACE_NBA_BLOCK | RT_TRACE_NBA_GATED, 32, 0 };
// A 4-state target of 4 bits has a must|may word of 8 bits and a z word of 4.
const rt_trace_nba_site kFour{ "design.sv:10:3", "v", RT_TRACE_NBA_BLOCK, 8, 4 };
const rt_trace_nba_site kWide{ "design.sv:11:3", "w", RT_TRACE_NBA_BLOCK, 128, 0 };

// The targets, the records timed updates are armed on, and the event objects.
uint32_t q, f;
uint8_t  vmm, vz;
uint64_t wide[2];
char     rec1, rec2, rec3;
int      ev1, ev2;

// A statement creates an update of `value` under `mask` (every bit when none is given).
void nba_put(const rt_trace_nba_site& s, uint32_t* dst, uint32_t value, uint32_t mask = 0xffffffffu) {
    const uint64_t v = value;
    const uint64_t m = mask;
    rt_trace_nba_create(&s, dst, nullptr, &v, mask == 0xffffffffu ? nullptr : &m, nullptr, nullptr);
}

// A statement arms a record, which a timer (`ticks` from now) or an event releases later.
void nba_arm(const void* rec, const rt_trace_nba_site& s, uint32_t* dst, uint32_t value, uint64_t ticks = 1) {
    const uint64_t v = value;
    rt_trace_nba_stage(rec, ticks, &s, dst, nullptr, &v, nullptr, nullptr, nullptr);
}

// The event of a `<= @e` record's control that it counts `count` occurrences of.
void nba_waits(const void* rec, uint32_t count, const void* x, uint32_t edge) {
    rt_trace_nba_event(rec, count, x, edge);
}

// The NBA region of a slot, once the Active region has drained.
void nba_begin() {
    rt_trace_nba_pass(1, RT_TRACE_REG_NBA);
}

void nba_end() {
    rt_trace_nba_pass(0, RT_TRACE_REG_NBA);
}

// An always that is parked at its leading control for `clk`, and one the checker has seen running.
void parked(void* a) {
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    head_wait(a);
}

} // namespace

TEST_CASE("trace check: the updates of a pass land in creation order and the slot ends clean") {
    rt_trace_counts before{};
    rt_trace_totals(&before);
    rt_trace_begin();
    q = 0;
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_put(kBlock, &q, 1);
    nba_put(kBlock, &q, 2);
    nba_begin();
    q = 2;
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    CHECK(rule().empty());
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.creates[RT_TRACE_NBA_BLOCK] == before.creates[RT_TRACE_NBA_BLOCK] + 2);
    CHECK(after.nba_passes == before.nba_passes + 1);
}

// B9 and B22: a zero delay is a plain NBA, so it is performed in the order its statement executed.
TEST_CASE("trace check: a zero delay and a plain NBA to one target commit in the order they executed (B9, B22)") {
    for (int zero_first = 0; zero_first < 2; ++zero_first) {
        for (int right = 0; right < 2; ++right) {
            rt_trace_begin();
            q = 0;
            rt_trace_region(RT_TRACE_REG_ACTIVE);
            nba_put(zero_first != 0 ? kZero : kBlock, &q, 1);
            nba_put(zero_first != 0 ? kBlock : kZero, &q, 2);
            nba_begin();
            // The later statement wins; a commit in the other order leaves the earlier one's value.
            q = right != 0 ? 2 : 1;
            nba_end();
            CHECK((rt_trace_halted() == 0) == (right != 0));
            if (right == 0) {
                CHECK(rule() == "R12.1");
            }
        }
    }
}

// B38: `x <= #1 1; #1; x <= 2;`. The timer released the first update at the top of the slot, before
// the second statement executed, so the plain NBA is the later update.
TEST_CASE("trace check: a plain NBA executed after a timed update was released commits after it (B38)") {
    const auto run = [](uint32_t committed) {
        rt_trace_begin();
        q = 0;
        nba_arm(&rec1, kTimer, &q, 1);
        rt_trace_slot_end();
        rt_trace_slot(1);
        rt_trace_nba_release(&rec1, RT_TRACE_NBA_TIMER, 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        nba_put(kBlock, &q, 2);
        nba_begin();
        q = committed;
        nba_end();
    };
    run(2);
    CHECK(rt_trace_halted() == 0);

    // The block shadow committed ahead of the ready list: the timed update came last.
    run(1);
    CHECK(rule() == "R12.1");
    const std::string report = rt_trace_report();
    CHECK(has(report, "rule R12.1 (nba order)"));
    CHECK(has(report, "does not hold what its updates give in creation order"));
    CHECK(has(report, "design.sv:7:3"));
    CHECK(has(report, "design.sv:5:3"));
    CHECK(has(report, "released by its timer"));
}

// B40: `x <= @e1 1; x <= @e2 2; #1 -> e2; -> e1;`. e2 occurred first, so its update was created first
// and e1's is the last. Records one occurrence released together keep the order of their statements.
TEST_CASE("trace check: updates released by different occurrences commit in the order the occurrences came (B40)") {
    for (int one_occurrence = 0; one_occurrence < 2; ++one_occurrence) {
        for (int right = 0; right < 2; ++right) {
            rt_trace_begin();
            q = 0;
            nba_arm(&rec1, kEvent, &q, 1);
            nba_arm(&rec2, kEvent, &q, 2);
            rt_trace_slot(1);
            rt_trace_region(RT_TRACE_REG_ACTIVE);
            rt_trace_nba_release(&rec2, RT_TRACE_NBA_EVENT, 7);
            rt_trace_nba_release(&rec1, RT_TRACE_NBA_EVENT, one_occurrence != 0 ? 7 : 8);
            nba_begin();
            const uint32_t last = one_occurrence != 0 ? 2 : 1;
            q                   = right != 0 ? last : 3 - last;
            nba_end();
            CHECK((rt_trace_halted() == 0) == (right != 0));
            if (right == 0) {
                CHECK(rule() == "R12.1");
            }
        }
    }
}

// B42: `x <= #1 1; x <= @(posedge x) 0;`. The commit of the first update raises the edge the second
// waits for, and the record that releases is created while the pass applies the first.
TEST_CASE("trace check: an update released during a pass is applied in it or in the next, and never lost (B42)") {
    const auto start = [] {
        rt_trace_begin();
        q = 0;
        nba_arm(&rec1, kTimer, &q, 1);
        nba_arm(&rec2, kEvent, &q, 0);
        rt_trace_slot(1);
        rt_trace_nba_release(&rec1, RT_TRACE_NBA_TIMER, 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        nba_begin();
        q = 1;
        rt_trace_nba_release(&rec2, RT_TRACE_NBA_EVENT, 3);
    };

    // Released by the commit's own wake, after its site counted its records: the next pass applies it.
    start();
    nba_end();
    CHECK(rt_trace_halted() == 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_begin();
    q = 0;
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // The fix reverted: the ready list is freed with the record on it, and no second pass comes.
    start();
    nba_end();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_halted() == 0);
    rt_trace_slot_end();
    CHECK(rule() == "R12.2");
    CHECK(has(rt_trace_report(), "still unapplied when the slot ends"));
    CHECK(has(rt_trace_report(), "design.sv:8:3"));

    // Released before its site counted its records: the same pass applies it.
    rt_trace_begin();
    q = 0;
    nba_arm(&rec1, kTimer, &q, 1);
    nba_arm(&rec2, kEvent, &q, 0);
    rt_trace_slot(1);
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_TIMER, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_begin();
    rt_trace_nba_release(&rec2, RT_TRACE_NBA_EVENT, 3);
    q = 0;
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: updates to the bits of one word compose, and an update the commit lost is flagged") {
    for (int right = 0; right < 2; ++right) {
        rt_trace_begin();
        q = 0xaaaaaaaau;
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        nba_put(kBlock, &q, 0x00000011u, 0x000000ffu);
        nba_put(kBlock, &q, 0x22000000u, 0xff000000u);
        nba_begin();
        q = right != 0 ? 0x22aaaa11u : 0xaaaaaa11u;
        nba_end();
        CHECK((rt_trace_halted() == 0) == (right != 0));
        if (right == 0) {
            CHECK(rule() == "R12.1");
        }
    }
}

TEST_CASE("trace check: a plane wider than a word, a z word, and a forced target") {
    // 128 bits under a mask that covers one byte of the high word.
    const uint64_t val[2]  = { 0x1111, 0x2222 };
    const uint64_t mask[2] = { ~uint64_t{ 0 }, 0xff00 };
    for (int right = 0; right < 2; ++right) {
        rt_trace_begin();
        wide[0] = wide[1] = 0;
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        rt_trace_nba_create(&kWide, wide, nullptr, val, mask, nullptr, nullptr);
        nba_begin();
        wide[0] = 0x1111;
        wide[1] = right != 0 ? 0x2200 : 0x2222;
        nba_end();
        CHECK((rt_trace_halted() == 0) == (right != 0));
    }

    // A 4-state target: the must|may word and the z word are each compared, and the bits above a
    // plane's width are not.
    const uint64_t mm = 0x35;
    const uint64_t z  = 0x2;
    for (int right = 0; right < 2; ++right) {
        rt_trace_begin();
        vmm = vz = 0;
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        rt_trace_nba_create(&kFour, &vmm, &vz, &mm, nullptr, &z, nullptr);
        nba_begin();
        vmm = 0x35;
        vz  = right != 0 ? 0xa2 : 0x0;
        nba_end();
        CHECK((rt_trace_halted() == 0) == (right != 0));
        if (right == 0) {
            CHECK(rule() == "R12.1");
            CHECK(has(rt_trace_report(), "(z word)"));
        }
    }

    // The commit of a forced target may drop the update: its bytes are not compared.
    rt_trace_begin();
    f = 7;
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_put(kGated, &f, 5);
    nba_begin();
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

// A target whose bytes the trace does not know (a runtime-offset select of a very wide vector, a plane
// past the width the backend spills) is not compared, but its update is still owed a pass.
TEST_CASE("trace check: an update whose bits are not known is not compared and is still applied by a pass") {
    const rt_trace_nba_site opaque{ "design.sv:12:3", "big", RT_TRACE_NBA_BLOCK | RT_TRACE_NBA_OPAQUE, 0, 0 };
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_nba_create(&opaque, &q, nullptr, nullptr, nullptr, nullptr, nullptr);
    nba_begin();
    q = 12345;
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_nba_create(&opaque, &q, nullptr, nullptr, nullptr, nullptr, nullptr);
    rt_trace_slot_end();
    CHECK(rule() == "R12.2");
    CHECK(has(rt_trace_report(), "its bits are not known to the trace"));
}

TEST_CASE("trace check: an NBA pass is entered from the region it follows, and no statement runs inside one") {
    // A statement that executes while the region applies updates: no process runs there.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_begin();
    nba_put(kBlock, &q, 1);
    CHECK(rule() == "R12.3");
    CHECK(has(rt_trace_report(), "inside an NBA pass"));

    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_begin();
    nba_begin();
    CHECK(rule() == "R12.3");
    CHECK(has(rt_trace_report(), "inside another"));

    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_end();
    CHECK(rule() == "R12.3");

    // The NBA region follows the Active region, not the top of the slot.
    rt_trace_begin();
    rt_trace_slot(1);
    nba_begin();
    CHECK(rule() == "R12.3");
    CHECK(has(rt_trace_report(), "not from the active region"));

    // The Re-NBA region follows the Reactive region.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    rt_trace_nba_pass(1, RT_TRACE_REG_RE_NBA);
    rt_trace_nba_pass(0, RT_TRACE_REG_RE_NBA);
    CHECK(rt_trace_halted() == 0);
    rt_trace_nba_pass(1, RT_TRACE_REG_RE_NBA);
    CHECK(rule() == "R12.3");

    // A record released that no statement armed, and one released the way it was not armed.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_nba_release(&rec3, RT_TRACE_NBA_TIMER, 0);
    CHECK(rule() == "R12.3");
    CHECK(has(rt_trace_report(), "no statement armed"));
    rt_trace_begin();
    nba_arm(&rec1, kTimer, &q, 1);
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_EVENT, 1);
    CHECK(rule() == "R12.3");
}

TEST_CASE("trace check: no update is left unapplied once the regions that apply it are behind") {
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_put(kBlock, &q, 1);
    rt_trace_slot_end();
    CHECK(rule() == "R12.2");
    CHECK(has(rt_trace_report(), "design.sv:5:3"));
    CHECK(has(rt_trace_report(), "no NBA pass applied it"));

    // Observed comes after the NBA region.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_put(kBlock, &q, 1);
    rt_trace_region(RT_TRACE_REG_OBSERVED);
    CHECK(rule() == "R12.2");

    // What the Reactive set staged waits for its Re-NBA region, but not past Postponed.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    nba_put(kBlock, &q, 1);
    rt_trace_region(RT_TRACE_REG_RE_INACTIVE);
    CHECK(rt_trace_halted() == 0);
    rt_trace_region(RT_TRACE_REG_POSTPONED);
    CHECK(rule() == "R12.2");
}

TEST_CASE("trace check: ->> events trigger in an NBA pass, in the order they were pended") {
    rt_trace_counts before{};
    rt_trace_totals(&before);
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_pend(&ev1);
    rt_trace_pend(&ev2);
    nba_begin();
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NB);
    rt_trace_trigger(&ev2, RT_TRACE_TRIGGER_NB);
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.pends == before.pends + 2);
    CHECK(after.triggers == before.triggers + 2);

    // `->` triggers at once, wherever, and leaves a pend owed.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_pend(&ev1);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW);
    CHECK(rt_trace_halted() == 0);
    rt_trace_slot_end();
    CHECK(rule() == "R20.3");

    // The Reactive set's `->>` trigger in its Re-NBA region.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    rt_trace_pend(&ev1);
    rt_trace_nba_pass(1, RT_TRACE_REG_RE_NBA);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NB);
    rt_trace_nba_pass(0, RT_TRACE_REG_RE_NBA);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

// B41: the pend chain links the newest event first and the drain walked it from the head, so two
// `->>` of one slot triggered their events in the opposite order.
TEST_CASE("trace check: ->> events that trigger out of the order they were pended are flagged (B41)") {
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_pend(&ev1);
    rt_trace_pend(&ev2);
    nba_begin();
    rt_trace_trigger(&ev2, RT_TRACE_TRIGGER_NB);
    CHECK(rule() == "R20.2");
    CHECK(has(rt_trace_report(), "out of the order they were pended in"));
    CHECK(has(rt_trace_report(), "the oldest `->>` not yet triggered"));
}

// B21: the expiry thunk of `->> #5 e` triggered the event at the top of the slot, ahead of the Active
// region, and its waiter read before an NBA of the slot had committed.
TEST_CASE("trace check: a ->> that triggers outside an NBA pass is flagged (B21)") {
    rt_trace_begin();
    rt_trace_slot(5);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NB);
    CHECK(rule() == "R20.1");
    CHECK(has(rt_trace_report(), "outside an NBA pass, in the top-of-slot region"));

    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NB);
    CHECK(rule() == "R20.1");
}

TEST_CASE("trace check: a ->> trigger needs a pend, and a ->> is pended once") {
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_begin();
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NB);
    CHECK(rule() == "R20.2");
    CHECK(has(rt_trace_report(), "no statement pended"));

    rt_trace_begin();
    rt_trace_pend(&ev1);
    rt_trace_pend(&ev1);
    CHECK(rule() == "R20.2");

    // Pended and never triggered by the end of the slot.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_pend(&ev1);
    rt_trace_slot_end();
    CHECK(rule() == "R20.3");
    CHECK(has(rt_trace_report(), "still untriggered when the slot ends"));
}

// B37: the active ring was reset by a `$finish` only if an earlier body had made it, so a process the
// finishing thread had woken ran after the finish.
TEST_CASE("trace check: after $finish an activation queued before it is not dispatched (B37)") {
    rt_trace_begin();
    void* woken     = &proc_b;
    void* finishing = &proc_a;
    parked(woken);
    rt_trace_spawn(finishing, &kProcess, nullptr);
    rt_trace_dispatch(finishing, RT_TRACE_INLINE);
    wrote_posedge(&clk);
    rt_trace_wake(woken, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    rt_trace_finish();
    CHECK(rt_trace_halted() == 0);
    CHECK(rt_trace_dispatch(woken, RT_TRACE_RING) == 0);
    CHECK(rule() == "R24.2");
    CHECK(has(rt_trace_report(), "queued before it"));
}

// The cooperative exit: a task that blocked and then finishes wakes its caller, which resumes at the
// call, sees the flag and leaves through its own epilogue.
TEST_CASE("trace check: after $finish each frame leaves by the completion that wakes it") {
    rt_trace_begin();
    void* caller = &proc_a;
    void* callee = &proc_b;
    rt_trace_spawn(caller, &kProcess, nullptr);
    rt_trace_dispatch(caller, RT_TRACE_INLINE);
    rt_trace_spawn(callee, &kProcess, caller);
    rt_trace_dispatch(callee, RT_TRACE_INLINE);
    rt_trace_park(callee, &kDelay, 5);
    rt_trace_park(caller, &kCall, 0);
    rt_trace_slot(5);
    rt_trace_wake(callee, RT_TRACE_TIMER, nullptr, 0);
    CHECK(rt_trace_dispatch(callee, RT_TRACE_RING) == 1);
    rt_trace_resume(callee, &kDelay);
    rt_trace_finish();
    rt_trace_wake(caller, RT_TRACE_CALLEE, nullptr, 0);
    rt_trace_end(callee);
    CHECK(rt_trace_dispatch(caller, RT_TRACE_RING) == 1);
    rt_trace_resume(caller, &kCall);
    rt_trace_end(caller);
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: after $finish nothing stores, stages, triggers or wakes but the exit") {
    const auto after_finish = [](auto&& event) {
        rt_trace_begin();
        parked(&proc_b);
        rt_trace_finish();
        event();
    };
    after_finish([] {
        wrote_posedge(&clk);
        rt_trace_wake(&proc_b, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    });
    CHECK(rule() == "R24.1");
    after_finish([] { nba_put(kBlock, &q, 1); });
    CHECK(rule() == "R24.1");
    after_finish([] { nba_arm(&rec1, kTimer, &q, 1); });
    CHECK(rule() == "R24.1");
    after_finish([] { rt_trace_pend(&ev1); });
    CHECK(rule() == "R24.1");
    after_finish([] { rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW); });
    CHECK(rule() == "R24.1");
    // A body that starts after the finish runs code the finish ended.
    after_finish([] {
        rt_trace_spawn(&proc_c, &kProcess, nullptr);
        CHECK(rt_trace_dispatch(&proc_c, RT_TRACE_INLINE) == 0);
    });
    CHECK(rule() == "R24.2");
}

TEST_CASE("trace check: the finals run after $finish, and what the finishing slot left staged is not checked") {
    rt_trace_counts before{};
    rt_trace_totals(&before);
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_put(kBlock, &q, 1);
    rt_trace_pend(&ev1);
    rt_trace_finish();
    rt_trace_region(RT_TRACE_REG_FINAL);
    rt_trace_spawn(&proc_a, &kProcess, nullptr);
    CHECK(rt_trace_dispatch(&proc_a, RT_TRACE_RUN_ONCE) == 1);
    rt_trace_end(&proc_a);
    // A `$finish` in a final does not hold the finals that follow it back.
    rt_trace_finish();
    rt_trace_spawn(&proc_b, &kProcess, nullptr);
    CHECK(rt_trace_dispatch(&proc_b, RT_TRACE_RUN_ONCE) == 1);
    CHECK(rt_trace_halted() == 0);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.finishes == before.finishes + 2);
}

// ---- The known list ----

TEST_CASE("trace check: the known list names a category, a test case, a rule and a reason") {
    const auto entries = trace_check::parse_known(
      "# a comment\n"
      "\n"
      "bug \"always parks at an intra-body event control\" R5.1 B51: the leading control's static entries stay live\n"
      "  rule   \"fork: a join_none child\"   R5.2  the dispatch of an inline child  \r\n");
    REQUIRE(entries.size() == 2);
    const auto& a = entries.at("always parks at an intra-body event control");
    CHECK(a.category == "bug");
    CHECK(a.rule == "R5.1");
    CHECK(a.reason == "B51: the leading control's static entries stay live");
    CHECK(a.line == 3);
    const auto& b = entries.at("fork: a join_none child");
    CHECK(b.category == "rule");
    CHECK(b.rule == "R5.2");
    CHECK(b.reason == "the dispatch of an inline child");

    const auto fails_on = [](std::string_view text, std::string_view line) {
        try {
            trace_check::parse_known(text, "known.txt");
        } catch (const std::runtime_error& e) {
            return std::string_view(e.what()).substr(0, line.size()) == line;
        }
        return false;
    };
    CHECK(fails_on("race \"t\" R5.1 why\n", "known.txt:1: category 'race' is not bug or rule"));
    CHECK(fails_on("bug \"t\" why it breaks\n", "known.txt:1: the reason must start with the rule"));
    CHECK(fails_on("bug \"t\" R5.\n", "known.txt:1: the reason must start with the rule"));
    CHECK(fails_on("bug \"t\" R5.1\n", "known.txt:1: an entry needs a reason after its rule"));
    CHECK(fails_on("bug \"t\" R5.1 a\nrule \"t\" R5.2 b\n", "known.txt:2: \"t\" is already listed at line 1"));
}

TEST_CASE("trace check: the checked in known list parses") {
    std::ifstream in(std::string(CLOTHO_TEST_DIR) + "/trace-check-known.txt", std::ios::binary);
    REQUIRE(static_cast<bool>(in));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_NOTHROW(trace_check::parse_known(text, "trace-check-known.txt"));
}

TEST_CASE("trace check: a listed test case is excused for its own rule only, and a stale entry is found") {
    const auto entries = trace_check::parse_known("bug \"listed\" R5.1 B51: understood\n");
    {
        trace_check::Suite suite(entries);
        suite.begin_test("unlisted");
        CHECK_THROWS_WITH_AS(suite.violation("R5.1", "report text\n"),
                             doctest::Contains("scheduler trace violation (R5.1)"), std::runtime_error);
        suite.end_test(false);

        suite.begin_test("listed");
        CHECK_NOTHROW(suite.violation("R5.1", "report text\n"));
        // Another rule still fails the test.
        CHECK_THROWS_AS(suite.violation("R5.2", "report text\n"), std::runtime_error);
        suite.end_test(true);
        CHECK(suite.stale().empty());
        CHECK(suite.violations() == 3);
        CHECK(suite.known_hits() == 1);
    }
    {
        // A listed test that passes without breaking its rule: the entry is out of date.
        trace_check::Suite suite(entries);
        suite.begin_test("listed");
        suite.end_test(true);
        REQUIRE(suite.stale().size() == 1);
        CHECK(suite.stale().front() == "listed");
    }
    {
        // A listed test that failed for another reason says nothing of the entry.
        trace_check::Suite suite(entries);
        suite.begin_test("listed");
        suite.end_test(false);
        CHECK(suite.stale().empty());
    }
}

// ---- A run that breaks a rule ----

namespace {

// The DPI-C import of the designs below reports an event the scheduler never made: the wake of an
// activation no spawn announced, which no design can cause.
const char* const kPokeDesign = R"(
    module t;
      import "DPI-C" function void clotho_trace_poke();
      initial begin
        #1 $display("before");
        clotho_trace_poke();
        #1 $display("after");
      end
    endmodule)";

} // namespace

extern "C" void clotho_trace_poke(void) {
    rt_trace_wake(reinterpret_cast<void*>(0x1000), RT_TRACE_TIMER, nullptr, 0);
}

#if !defined(_WIN32)
namespace {

// Stdout of `run`, with whatever the hook or the run itself throws left to the caller. Restores
// stdout on every path.
template <typename F>
std::string capture_stdout(F&& run) {
    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));
    struct Restore {
        int         saved;
        std::FILE*  tmp;
        std::string text;
        ~Restore() {
            std::fflush(stdout);
            dup2(saved, fileno(stdout));
            close(saved);
            std::fclose(tmp);
        }
    } restore{ saved, tmp, {} };
    run();
    std::fflush(stdout);
    std::rewind(tmp);
    char buf[256];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), tmp)) > 0;) {
        restore.text.append(buf, n);
    }
    return restore.text;
}

} // namespace

TEST_CASE("trace check: a run that breaks a rule ends at once and hands its report to the host's hook") {
    const LowerResult r = lower_text(kPokeDesign);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    trace_check::Paused  paused;
    ir::SimulationConfig config;
    config.trace_check = true;

    std::string rule_seen;
    std::string report_seen;
    ir::set_trace_hook([&](const ir::TraceViolation& v) {
        rule_seen   = v.rule;
        report_seen = v.report;
    });
    int               rc  = 0;
    const std::string out = capture_stdout([&] { rc = ir::run_jit(*r.module, nullptr, config); });

    // The design printed what ran before the violation and nothing after it, and the run's status is
    // the checker's own.
    CHECK(out == "before\n");
    CHECK(rc == 99);
    CHECK(rule_seen == "R5.1");
    CHECK(has(report_seen, "no spawn announced"));

    // A hook that throws fails whoever ran the design, after the run has returned.
    ir::set_trace_hook([](const ir::TraceViolation& v) { throw std::runtime_error("violation " + v.rule); });
    CHECK_THROWS_WITH_AS(capture_stdout([&] { ir::run_jit(*r.module, nullptr, config); }), "violation R5.1",
                         std::runtime_error);
}

// What a child process's violation looks like from outside: the marker line and the report on
// stderr, the status, and the report handed back through the file spawn_design names.
TEST_CASE("trace check: a child process that breaks a rule prints the report, exits with status 99 and raises it") {
    if (std::getenv("CLOTHO_NO_TRACE_CHECK") != nullptr) {
        // The mode is off for the whole run: no design emits events to check.
        return;
    }
    trace_check::Paused paused;
    std::string         rule_seen;
    std::string         report_seen;
    test::trace_child_report = [&](const std::string& rule, const std::string& report) {
        rule_seen   = rule;
        report_seen = report;
    };
    const test::DesignRun run = test::run_design_captured(kPokeDesign);

    CHECK(test::design_exit_code(run) == 99);
    CHECK(run.out == "before\n");
    CHECK(has(run.err, "clotho: scheduler trace violation:\nrule R5.1 (wake)"));
    CHECK(has(run.err, "no spawn announced"));
    CHECK(rule_seen == "R5.1");
    CHECK(has(report_seen, "no spawn announced"));
}

namespace {

// A design that stages an update by every path, triggers an event with `->>` and ends with `$finish`.
const char* const kAllPathsDesign = R"(
    module t;
      typedef struct { logic [7:0] f; logic [7:0] g; } s_t;
      logic [7:0] a, b, c, d;
      logic [7:0] m [0:3];
      s_t         s [0:1];
      event       e, ev;
      int         i = 1;
      initial begin
        a <= 8'd1;
        m[i] <= 8'd2;
        s[i].f <= 8'd3;
        b <= #0 8'd4;
        d <= #1 8'd5;
        c <= @(ev) 8'd6;
        ->> e;
        @(e) $display("e");
        #1 -> ev;
        #2 $display("a=%0d m=%0d s=%0d b=%0d d=%0d c=%0d", a, m[1], s[1].f, b, d, c);
        $finish;
      end
    endmodule)";

constexpr const char* kAllPathsOutput = "e\na=1 m=2 s=3 b=4 d=5 c=6\n";

} // namespace

// The events as the generated code and the scheduler make them: the design above runs without a
// violation, and each staging path, the passes, the pend and the triggers and the finish are counted.
TEST_CASE("trace check: a design that stages every kind of update, triggers with ->> and finishes runs clean and is counted") {
    if (std::getenv("CLOTHO_NO_TRACE_CHECK") != nullptr) {
        return;
    }
    const LowerResult r = lower_text(kAllPathsDesign);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    rt_trace_counts before{};
    rt_trace_totals(&before);
    ir::SimulationConfig config;
    config.trace_check = true;
    const std::string out = capture_stdout([&] { ir::run_jit(*r.module, nullptr, config); });
    CHECK(out == kAllPathsOutput);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    // A compile-time `#0` takes the plain path, so the zero-delay path is not asserted here.
    for (const uint32_t path : { RT_TRACE_NBA_BLOCK, RT_TRACE_NBA_ARRAY, RT_TRACE_NBA_AGG, RT_TRACE_NBA_TIMER, RT_TRACE_NBA_EVENT }) {
        CHECK_MESSAGE(after.creates[path] > before.creates[path], "staging path ", path);
    }
    CHECK(after.pends == before.pends + 1);
    CHECK(after.triggers == before.triggers + 2);
    CHECK(after.finishes == before.finishes + 1);
    CHECK(after.nba_passes > before.nba_passes);
}

// A run whose code reports nothing (the config turns the check off in a build that has the checker, as
// CLOTHO_NO_TRACE_CHECK=1 does) still has the scheduler release records, end slots and finish: none of
// that is a run for the checker, and it breaks no rule.
TEST_CASE("trace check: a run with the check off in a build that has the checker breaks no rule") {
    const LowerResult r = lower_text(kAllPathsDesign);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    rt_trace_counts before{};
    rt_trace_totals(&before);
    ir::SimulationConfig config;
    config.trace_check = false;
    const std::string out = capture_stdout([&] { ir::run_jit(*r.module, nullptr, config); });
    CHECK(out == kAllPathsOutput);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.runs == before.runs);
}

TEST_CASE("trace check: the scheduler's own events say nothing of a run whose code reports none") {
    rt_trace_begin();
    rt_trace_slot(1);
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_TIMER, 0);
    rt_trace_slot_end();
    rt_trace_finish();
    rt_trace_region(RT_TRACE_REG_FINAL);
    CHECK(rt_trace_halted() == 0);
}

// A block disabled from another process with a fork child inside it, and a thread killed while it
// waits: the generated code reports each node it links and unlinks, each activation a walk marks and
// each region the scheduler runs, and the run breaks no rule. LRM-pinned (9.6.2: the disable ends "all
// activities enabled within the named block", the child of a `join_none` included; 9.7: `kill`).
TEST_CASE("trace check: a design that disables a block with a fork child inside and kills a thread runs clean and is counted") {
    if (std::getenv("CLOTHO_NO_TRACE_CHECK") != nullptr) {
        return;
    }
    const LowerResult r = lower_text(R"(
        module t;
          process pk;
          int n = 0;
          initial begin : worker
            fork
              #15 $display("child ran");
            join_none
            #20 $display("worker ran on");
          end
          initial begin
            #10 disable worker;
            fork
              begin pk = process::self(); forever #4 n++; end
            join_none
            #30 pk.kill();
            #5 $display("n=%0d status=%0d", n, pk.status());
          end
        endmodule)");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    rt_trace_counts before{};
    rt_trace_totals(&before);
    ir::SimulationConfig config;
    config.trace_check = true;
    const std::string out = capture_stdout([&] { ir::run_jit(*r.module, nullptr, config); });
    CHECK(out == "n=7 status=4\n");
    rt_trace_counts after{};
    rt_trace_totals(&after);
    // The worker and its child are on the list of `worker` and leave it; the kill marks the thread.
    CHECK(after.blocks >= before.blocks + 4);
    CHECK(after.aborts >= before.aborts + 3);
}
#endif

// ---- Writes and waiters (R6, R7) ----
//
// A write is a `change` before its wakes; each sequence below is what the brackets and the scheduler
// report for the bugs the rules exist for (docs/design/scheduler-rules.md §4), with the bytes of the
// storage a waiter lists changed by hand where the bug was a store no bracket saw.

namespace {

// A 32-bit 2-state signal, and a 1-bit 4-state one with a z word: its must|may word of two bits
// (value 0: 00, 1: 11, x or z: 10) and then its z word.
uint32_t             word32;
const rt_trace_shape kWord{ 32, 32, 0, 0, 4, 1 };
struct Bit4 {
    uint8_t mm;
    uint8_t z;
} bit4;
const rt_trace_shape kBit4{ 1, 2, 1, 1, 2, 1 };

// What a bracket saw of bit 0 going from `o` to `n` (0, 1, 2 = x or z), with the facts the code
// computed as Table 9-2 gives them; `zd` is a z word that differs.
uint32_t seen_for(int o, int n, bool zd) {
    const auto rank = [](int s) { return s == 1 ? 2 : (s == 2 ? 1 : 0); };
    uint32_t   s    = RT_TRACE_SEEN_FOUR | RT_TRACE_SEEN_HAS_Z | RT_TRACE_SEEN_BIT0 | RT_TRACE_SEEN_DIFF_KNOWN |
                RT_TRACE_SEEN_ZDIFF_KNOWN | RT_TRACE_SEEN_POS_KNOWN | RT_TRACE_SEEN_NEG_KNOWN | RT_TRACE_SEEN_ANY_KNOWN;
    s |= o == 1 ? RT_TRACE_SEEN_OLD_MUST : 0;
    s |= o != 0 ? RT_TRACE_SEEN_OLD_MAY : 0;
    s |= n == 1 ? RT_TRACE_SEEN_NEW_MUST : 0;
    s |= n != 0 ? RT_TRACE_SEEN_NEW_MAY : 0;
    s |= o != n ? RT_TRACE_SEEN_DIFF : 0;
    s |= zd ? RT_TRACE_SEEN_ZDIFF : 0;
    s |= rank(n) > rank(o) ? RT_TRACE_SEEN_POS : 0;
    s |= rank(n) < rank(o) ? RT_TRACE_SEEN_NEG : 0;
    s |= (o != n || zd) ? RT_TRACE_SEEN_ANY : 0;
    return s;
}

// An activation parked at an event control that lists one entry.
void park_on(void* a, const void* x, const char* name, uint64_t edge, const rt_trace_shape* shape) {
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_waits_for(a, x, name, edge, shape);
    rt_trace_park(a, &kWait, 0);
}

} // namespace

// B5: edge facts that ignored x and z. Every move of bit 0 has the facts Table 9-2 gives; a fact the
// code computed otherwise is named.
TEST_CASE("trace check: the facts of a write are Table 9-2's on the ranks 0 < x = z < 1 (B5)") {
    for (int o = 0; o < 3; ++o) {
        for (int n = 0; n < 3; ++n) {
            rt_trace_begin();
            rt_trace_change(&kStore, &bit4, seen_for(o, n, false));
            CHECK_MESSAGE(rt_trace_halted() == 0, o << " -> " << n);
        }
    }
    // 0 -> x is a posedge: the code that took bit 0 of the must plane alone saw none.
    rt_trace_begin();
    rt_trace_change(&kStore, &bit4, seen_for(0, 2, false) & ~RT_TRACE_SEEN_POS);
    CHECK(rule() == "R7.1");
    CHECK(has(rt_trace_report(), "posedge fact"));
    // x -> 1 likewise, and 1 -> x is a negedge.
    rt_trace_begin();
    rt_trace_change(&kStore, &bit4, seen_for(2, 1, false) & ~RT_TRACE_SEEN_POS);
    CHECK(rule() == "R7.1");
    rt_trace_begin();
    rt_trace_change(&kStore, &bit4, seen_for(1, 2, false) & ~RT_TRACE_SEEN_NEG);
    CHECK(rule() == "R7.1");
    // x <-> z is a change and no edge: an edge fact there is wrong.
    rt_trace_begin();
    rt_trace_change(&kStore, &bit4, seen_for(2, 2, true) | RT_TRACE_SEEN_POS);
    CHECK(rule() == "R7.1");
    // A z-only change is a change; `changed` that compared the planes alone missed it.
    rt_trace_begin();
    rt_trace_change(&kStore, &bit4, seen_for(2, 2, true));
    CHECK(rt_trace_halted() == 0);
    rt_trace_change(&kStore, &bit4, seen_for(2, 2, true) & ~RT_TRACE_SEEN_ANY);
    CHECK(rule() == "R7.2");
    // A write that changed nothing did not change.
    rt_trace_begin();
    rt_trace_change(&kStore, &bit4, seen_for(1, 1, false) | RT_TRACE_SEEN_ANY);
    CHECK(rule() == "R7.2");
    // A z word the bracket did not read: `changed` may be true only where a plane differs, and a
    // plane that differs must make it true.
    const uint32_t no_z = (seen_for(0, 1, false) & ~(RT_TRACE_SEEN_ZDIFF_KNOWN | RT_TRACE_SEEN_ZDIFF));
    rt_trace_begin();
    rt_trace_change(&kStore, &bit4, no_z);
    CHECK(rt_trace_halted() == 0);
    rt_trace_change(&kStore, &bit4, no_z & ~RT_TRACE_SEEN_ANY);
    CHECK(rule() == "R7.2");
    // A 2-state slot has two states: 0 -> 1 is a posedge and 1 -> 0 a negedge.
    const uint32_t two = RT_TRACE_SEEN_BIT0 | RT_TRACE_SEEN_POS_KNOWN | RT_TRACE_SEEN_NEG_KNOWN;
    rt_trace_begin();
    rt_trace_change(&kStore, &word32, two | RT_TRACE_SEEN_NEW_MUST | RT_TRACE_SEEN_POS);
    CHECK(rt_trace_halted() == 0);
    rt_trace_change(&kStore, &word32, two | RT_TRACE_SEEN_OLD_MUST | RT_TRACE_SEEN_NEG);
    CHECK(rt_trace_halted() == 0);
    rt_trace_change(&kStore, &word32, two | RT_TRACE_SEEN_OLD_MUST | RT_TRACE_SEEN_POS);
    CHECK(rule() == "R7.1");
}

// B12's wake half and B8: a write was seen and a waiter whose entry it satisfies was not woken.
TEST_CASE("trace check: every activation a write satisfies is woken before the next dispatch, park or region (B8, B12)") {
    void* a = &proc_a;
    // Woken: nothing is owed.
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // Not woken: the next dispatch, park, region and the end of the slot each find it.
    for (int point = 0; point < 4; ++point) {
        rt_trace_begin();
        park_on(a, &clk, "clk", RT_TRACE_POS, nullptr);
        rt_trace_spawn(&proc_b, &kProcess, nullptr);
        rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
        CHECK(rt_trace_halted() == 0);
        switch (point) {
            case 0: rt_trace_dispatch(&proc_b, RT_TRACE_INLINE); break;
            case 1:
                rt_trace_waits_for(&proc_b, &done, "done", RT_TRACE_POS, nullptr);
                rt_trace_park(&proc_b, &kWait, 0);
                break;
            case 2: rt_trace_region(RT_TRACE_REG_INACTIVE); break;
            default: rt_trace_slot_end(); break;
        }
        CHECK_MESSAGE(rule() == "R6.1", point);
        CHECK(has(rt_trace_report(), "was not woken"));
    }

    // The entries a write does not satisfy are owed nothing: a negedge entry on a posedge, an edge on
    // x <-> z, and a change entry on a write that changed nothing.
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_NEG, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(2, 2, true));
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_ANY, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(1, 1, false));
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    // ... but a change entry is owed a z-only change, and a level wake that the code does not
    // classify is owed to every change entry and to no edge entry.
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_ANY, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(2, 2, true));
    rt_trace_slot_end();
    CHECK(rule() == "R6.1");
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_ANY, nullptr);
    rt_trace_change(&kStore, &clk, RT_TRACE_SEEN_LEVEL);
    rt_trace_slot_end();
    CHECK(rule() == "R6.1");
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_change(&kStore, &clk, RT_TRACE_SEEN_LEVEL);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // A trigger is owed to every activation parked on the event.
    rt_trace_begin();
    park_on(a, &ev1, "e", RT_TRACE_OBJ, nullptr);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW);
    rt_trace_slot_end();
    CHECK(rule() == "R6.1");
    rt_trace_begin();
    park_on(a, &ev1, "e", RT_TRACE_OBJ, nullptr);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW);
    rt_trace_wake(a, RT_TRACE_EVENT, &ev1, RT_TRACE_OBJ);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

// A wake by a store needs a write that satisfies it in the same bracket.
TEST_CASE("trace check: a store's wake follows a write that changed the way the wake says") {
    void* a = &proc_a;
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rule() == "R6.2");
    CHECK(has(rt_trace_report(), "no bracket reported"));

    // A write that changed nothing wakes nobody.
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(1, 1, false));
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rule() == "R6.2");

    // A negedge entry woken by a write that was a posedge.
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_NEG, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_NEG);
    CHECK(rule() == "R6.2");

    // The write of an earlier step does not excuse a wake made after the activation ran and parked.
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_park(a, &kWait, 0);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rule() == "R6.2");

    // A level write is a change to every level entry.
    rt_trace_begin();
    park_on(a, &clk, "clk", RT_TRACE_ANY, nullptr);
    rt_trace_change(&kStore, &clk, RT_TRACE_SEEN_LEVEL);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_ANY);
    CHECK(rt_trace_halted() == 0);
}

// B7, B25: a store that no bracket serviced. The bytes of what a parked activation waits for read
// differently at the end of the slot, and nothing woke it.
TEST_CASE("trace check: a value that changed under a parked waiter with no wake is found at the end of the slot (B7, B25)") {
    void* a = &proc_a;
    word32  = 0;
    rt_trace_begin();
    park_on(a, &word32, "word", RT_TRACE_ANY, &kWord);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    word32 = 5;
    rt_trace_slot_end();
    CHECK(rule() == "R6.3");
    CHECK(has(rt_trace_report(), "0x00000000 when the activation parked and are 0x00000005 now"));

    // A change that reverts before the slot ends is not a change of its bytes.
    word32 = 0;
    rt_trace_begin();
    park_on(a, &word32, "word", RT_TRACE_ANY, &kWord);
    word32 = 5;
    word32 = 0;
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // A waiter that was woken is no longer owed.
    rt_trace_begin();
    park_on(a, &word32, "word", RT_TRACE_ANY, &kWord);
    word32 = 7;
    rt_trace_change(&kStore, &word32, RT_TRACE_SEEN_LEVEL);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &word32, RT_TRACE_ANY);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // An edge entry reads bit 0 of the planes: 0 -> x is a posedge, x -> z (the z word alone) is none.
    bit4 = { 0, 0 };
    rt_trace_begin();
    park_on(a, &bit4, "b", RT_TRACE_POS, &kBit4);
    bit4 = { 2, 1 };
    rt_trace_slot_end();
    CHECK(rule() == "R6.3");
    bit4 = { 2, 0 };
    rt_trace_begin();
    park_on(a, &bit4, "b", RT_TRACE_POS, &kBit4);
    bit4.z = 1;
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    park_on(&proc_b, &bit4, "b", RT_TRACE_ANY, &kBit4);
    bit4.z = 0;
    rt_trace_slot_end();
    CHECK(rule() == "R6.3");

    // A static subscription is read the same way: an activation that waits at its static control
    // for an edge is parked there between runs.
    word32 = 0;
    rt_trace_begin();
    const rt_trace_entry sub[] = { { &word32, "word", RT_TRACE_ANY, &kWord } };
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_subscribe(a, sub, 1);
    rt_trace_park(a, &kStatic, 0);
    word32 = 1;
    rt_trace_slot_end();
    CHECK(rule() == "R6.3");
}

// ---- Regions, time zero, record identity and blocks (R1, R2, R4, R15, R17, R23, R25, B43) ----
//
// As above: each sequence is what the scheduler and the generated code report, for the bugs the
// rules exist for (docs/design/scheduler-rules.md §4). A seed row names the facts of its processes in
// the kind of its origin, and a wake the ring it pushes onto in the bits above its cause.

namespace {

constexpr uint32_t kOrgProcess = RT_TRACE_ORG_PROCESS;

const rt_trace_control kInitial{ "design.sv:20:3", kOrgProcess | RT_TRACE_ORG_PROCEDURE };
const rt_trace_control kAssign{ "design.sv:21:3", kOrgProcess | RT_TRACE_ORG_SELF_START };
const rt_trace_control kComb{ "design.sv:22:3", kOrgProcess | RT_TRACE_ORG_SELF_START | RT_TRACE_ORG_LATE };
const rt_trace_control kProgram{ "design.sv:23:3", kOrgProcess | RT_TRACE_ORG_PROGRAM };
const rt_trace_control kCopy{ "design.sv:24:3", kOrgProcess | RT_TRACE_ORG_SELF_START | RT_TRACE_ORG_REACTIVE_CONT };
const rt_trace_control kStrobeBody{ "design.sv:25:3", RT_TRACE_ORG_STROBE };
const rt_trace_control kForkChild{ "design.sv:27:3", RT_TRACE_ORG_FORK };
const rt_trace_control kSyncWait{ "design.sv:12:5", RT_TRACE_CTL_SYNC };
const rt_trace_control kAwaitWait{ "design.sv:13:5", RT_TRACE_CTL_AWAIT };
const rt_trace_control kExpectWait{ "design.sv:14:5", RT_TRACE_CTL_EXPECT };

constexpr uint32_t kReactiveRing = RT_TRACE_QUEUE_REACTIVE << RT_TRACE_QUEUE_SHIFT;
constexpr uint32_t kContRing     = RT_TRACE_QUEUE_CONT << RT_TRACE_QUEUE_SHIFT;

// The lists of occupants of two `disable` targets, and two records.
int list_a, list_b, rec_a, rec_b;

// An activation address of its own: the tag bits of a parked handle are stripped, so proc_a and proc_c
// name one activation.
alignas(8) char proc_d;

uint64_t id_of(const void* p) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}

// An activation the checker has seen running and parked at an event control for `clk`.
void parked_on_clk(void* a, const rt_trace_control* origin = &kProcess) {
    rt_trace_spawn(a, origin, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS, nullptr);
    rt_trace_park(a, &kWait, 0);
}

} // namespace

// B2: a continuous assignment whose source holds a declaration initializer (which makes no edge) is
// never evaluated if the seed does not run it once at time zero.
TEST_CASE("trace check: a continuously active process the seed never ran at time zero is flagged (R15, B2)") {
    rt_trace_begin();
    void* assign = &proc_a;
    void* other  = &proc_b;
    rt_trace_spawn(assign, &kAssign, nullptr);
    rt_trace_spawn(other, &kInitial, nullptr);
    rt_trace_wake(other, RT_TRACE_SEED, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(other, RT_TRACE_RING) == 1);
    rt_trace_end(other);
    rt_trace_slot_end();
    CHECK(rule() == "R15.1");
    CHECK(has(rt_trace_report(), "not evaluated at time zero"));
    CHECK(has(rt_trace_report(), "design.sv:21:3"));

    // Queued by the seed and run once: nothing is owed, and a later slot owes nothing more.
    rt_trace_begin();
    rt_trace_spawn(assign, &kAssign, nullptr);
    rt_trace_wake(assign, RT_TRACE_SEED, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(assign, RT_TRACE_RING) == 1);
    rt_trace_park(assign, &kStatic, 0);
    rt_trace_slot_end();
    rt_trace_slot(1);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

// B24: an always_comb written above an `initial` ran first, before the `initial` had started.
TEST_CASE("trace check: an always_comb that starts before a procedure the seed queued is flagged (R17, B24)") {
    void* comb = &proc_a;
    void* init = &proc_b;
    rt_trace_begin();
    rt_trace_spawn(comb, &kComb, nullptr);
    rt_trace_spawn(init, &kInitial, nullptr);
    rt_trace_wake(comb, RT_TRACE_SEED, nullptr, 0);
    rt_trace_wake(init, RT_TRACE_SEED, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(comb, RT_TRACE_RING) == 0);
    CHECK(rule() == "R17.1");
    CHECK(has(rt_trace_report(), "before every procedure had"));
    CHECK(has(rt_trace_report(), "design.sv:20:3"));

    // Queued behind every procedure: each is dispatched first.
    rt_trace_begin();
    rt_trace_spawn(comb, &kComb, nullptr);
    rt_trace_spawn(init, &kInitial, nullptr);
    rt_trace_wake(init, RT_TRACE_SEED, nullptr, 0);
    rt_trace_wake(comb, RT_TRACE_SEED, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(init, RT_TRACE_RING) == 1);
    CHECK(rt_trace_dispatch(comb, RT_TRACE_RING) == 1);
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: the regions of a slot come in order, and work in a later one sends control back (R1.1)") {
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_region(RT_TRACE_REG_INACTIVE);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_begin();
    nba_end();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_region(RT_TRACE_REG_OBSERVED);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_region(RT_TRACE_REG_OBSERVED);
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    rt_trace_region(RT_TRACE_REG_RE_INACTIVE);
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    rt_trace_nba_pass(1, RT_TRACE_REG_RE_NBA);
    rt_trace_nba_pass(0, RT_TRACE_REG_RE_NBA);
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    rt_trace_region(RT_TRACE_REG_POSTPONED);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    // Nothing follows Postponed in its slot.
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rule() == "R1.1");
    CHECK(has(rt_trace_report(), "begins after the postponed region"));

    // A region that sends control back to one that is not the head of a loop, and the top of a slot
    // after a region.
    const std::pair<uint32_t, uint32_t> steps[] = { { RT_TRACE_REG_OBSERVED, RT_TRACE_REG_INACTIVE },
                                                    { RT_TRACE_REG_REACTIVE, RT_TRACE_REG_OBSERVED },
                                                    { RT_TRACE_REG_RE_INACTIVE, RT_TRACE_REG_INACTIVE },
                                                    { RT_TRACE_REG_ACTIVE, RT_TRACE_REG_PREPONED } };
    for (const auto& [from, to] : steps) {
        rt_trace_begin();
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        rt_trace_region(from);
        CHECK_MESSAGE(rt_trace_halted() == 0, from);
        rt_trace_region(to);
        CHECK_MESSAGE(rule() == "R1.1", from, " -> ", to);
    }

    // A ring is drained in the Active and Reactive regions only.
    rt_trace_begin();
    rt_trace_spawn(&proc_a, &kProcess, nullptr);
    rt_trace_dispatch(&proc_a, RT_TRACE_INLINE);
    rt_trace_park(&proc_a, &kDelay, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_region(RT_TRACE_REG_INACTIVE);
    rt_trace_wake(&proc_a, RT_TRACE_INACTIVE, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_OBSERVED);
    CHECK(rt_trace_dispatch(&proc_a, RT_TRACE_RING) == 0);
    CHECK(rule() == "R1.1");
    CHECK(has(rt_trace_report(), "popped from a ring outside the Active and Reactive regions"));
}

// A region ends only when it is empty, so control that moves on with an activation still queued has
// skipped it: the Observed bodies woke an Active process and the scheduler went on to Reactive.
TEST_CASE("trace check: control that moves on with work left in an earlier region is flagged (R1.2)") {
    struct Step {
        const char* name;
        void (*go)();
    };
    const Step steps[] = {
        { "inactive", [] { rt_trace_region(RT_TRACE_REG_INACTIVE); } },
        { "NBA", [] { nba_begin(); } },
        { "observed", [] { rt_trace_region(RT_TRACE_REG_OBSERVED); } },
        { "reactive", [] { rt_trace_region(RT_TRACE_REG_REACTIVE); } },
        { "postponed", [] { rt_trace_region(RT_TRACE_REG_POSTPONED); } },
        { "slot end", [] { rt_trace_slot_end(); } },
    };
    for (const Step& step : steps) {
        rt_trace_begin();
        rt_trace_spawn(&proc_a, &kProcess, nullptr);
        rt_trace_wake(&proc_a, RT_TRACE_SEED, nullptr, 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        step.go();
        CHECK_MESSAGE(rule() == "R1.2", step.name);
        CHECK_MESSAGE(has(rt_trace_report(), "queued on the active ring and never dispatched"), step.name);

        // Dispatched before control moves on: nothing is left.
        rt_trace_begin();
        rt_trace_spawn(&proc_a, &kProcess, nullptr);
        rt_trace_wake(&proc_a, RT_TRACE_SEED, nullptr, 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        CHECK(rt_trace_dispatch(&proc_a, RT_TRACE_RING) == 1);
        rt_trace_end(&proc_a);
        step.go();
        CHECK_MESSAGE(rt_trace_halted() == 0, step.name);
    }

    // The Reactive set: its rings are empty before the Re-Inactive region and the Postponed one.
    for (const bool postponed : { false, true }) {
        rt_trace_begin();
        rt_trace_spawn(&proc_a, &kProgram, nullptr);
        rt_trace_wake(&proc_a, RT_TRACE_SEED | kReactiveRing, nullptr, 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        rt_trace_region(RT_TRACE_REG_REACTIVE);
        CHECK(rt_trace_halted() == 0);
        rt_trace_region(postponed ? RT_TRACE_REG_POSTPONED : RT_TRACE_REG_RE_INACTIVE);
        CHECK(rule() == "R1.2");
        CHECK(has(rt_trace_report(), "queued on the reactive ring"));
    }
}

TEST_CASE("trace check: every #0 of a batch is promoted together and none is left behind (R2.2)") {
    const auto zero_parked = [] {
        rt_trace_begin();
        for (void* x : { static_cast<void*>(&proc_a), static_cast<void*>(&proc_b) }) {
            rt_trace_spawn(x, &kProcess, nullptr);
            rt_trace_dispatch(x, RT_TRACE_INLINE);
            rt_trace_park(x, &kDelay, 0);
        }
        rt_trace_region(RT_TRACE_REG_ACTIVE);
    };
    zero_parked();
    rt_trace_region(RT_TRACE_REG_INACTIVE);
    rt_trace_wake(&proc_a, RT_TRACE_INACTIVE, nullptr, 0);
    rt_trace_wake(&proc_b, RT_TRACE_INACTIVE, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_halted() == 0);

    // One of them is not promoted: it would run in a later instant.
    zero_parked();
    rt_trace_region(RT_TRACE_REG_INACTIVE);
    rt_trace_wake(&proc_a, RT_TRACE_INACTIVE, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rule() == "R2.2");
    CHECK(has(rt_trace_report(), "every process parked at a #0 becomes active together"));

    // The NBA region begins after the Inactive one has emptied, and so does the end of the slot.
    zero_parked();
    nba_begin();
    CHECK(rule() == "R1.2");
    CHECK(has(rt_trace_report(), "parked at a #0 that no promotion released"));
    zero_parked();
    rt_trace_slot_end();
    CHECK(rule() == "R1.2");

    // The Reactive set has a batch of its own, which a reactive thread's #0 waits in.
    rt_trace_begin();
    rt_trace_spawn(&proc_a, &kProgram, nullptr);
    rt_trace_dispatch(&proc_a, RT_TRACE_INLINE);
    rt_trace_park(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&proc_a) | 2), &kDelay, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    CHECK(rt_trace_halted() == 0);
    rt_trace_region(RT_TRACE_REG_RE_INACTIVE);
    rt_trace_wake(&proc_a, RT_TRACE_INACTIVE | kReactiveRing, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a delay's timer that never fired is found when its slot is over (R2.3)") {
    void* a = &proc_a;
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_park(a, &kDelay, 3);
    rt_trace_slot(2);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    rt_trace_slot(3);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_slot_end();
    CHECK(rule() == "R2.3");
    CHECK(has(rt_trace_report(), "the delay ends at tick 3 and the slot of tick 3 is over"));

    // Woken by its timer in its slot: nothing is overdue.
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_park(a, &kDelay, 3);
    rt_trace_slot(3);
    rt_trace_wake(a, RT_TRACE_TIMER, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_resume(a, &kDelay);
    rt_trace_end(a);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a body the scheduler runs once runs in its region, and a pended $strobe in Postponed (R4)") {
    void* a = &proc_a;
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_region(RT_TRACE_REG_OBSERVED);
    rt_trace_once(RT_TRACE_ORG_OBSERVED);
    rt_trace_spawn(a, &kProcess, nullptr);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RUN_ONCE) == 1);
    rt_trace_end(a);
    rt_trace_once(0);
    CHECK(rt_trace_halted() == 0);

    // The Observed bodies ran in the Active region.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_once(RT_TRACE_ORG_OBSERVED);
    rt_trace_spawn(a, &kProcess, nullptr);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RUN_ONCE) == 0);
    CHECK(rule() == "R4.1");
    CHECK(has(rt_trace_report(), "the scheduler is in the active region"));

    // A $strobe body is drained in Postponed.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_spawn(a, &kStrobeBody, nullptr);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RUN_ONCE) == 0);
    CHECK(rule() == "R4.1");
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_spawn(a, &kStrobeBody, nullptr);
    rt_trace_region(RT_TRACE_REG_POSTPONED);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RUN_ONCE) == 1);
    rt_trace_end(a);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // A $strobe pended in the slot and never drained.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_spawn(a, &kStrobeBody, nullptr);
    rt_trace_region(RT_TRACE_REG_POSTPONED);
    rt_trace_slot_end();
    CHECK(rule() == "R4.2");
    CHECK(has(rt_trace_report(), "never ran"));
}

TEST_CASE("trace check: program code runs in the Reactive region and design code in the Active one (R25.1)") {
    void* a = &proc_a;
    // A program's initial the seed queued for the Active region.
    rt_trace_begin();
    rt_trace_spawn(a, &kProgram, nullptr);
    rt_trace_wake(a, RT_TRACE_SEED, nullptr, 0);
    CHECK(rule() == "R25.1");
    CHECK(has(rt_trace_report(), "a thread of a program was queued for the Active region"));
    // ... and a design process queued for the Reactive one.
    rt_trace_begin();
    rt_trace_spawn(a, &kInitial, nullptr);
    rt_trace_wake(a, RT_TRACE_SEED | kReactiveRing, nullptr, 0);
    CHECK(rule() == "R25.1");
    CHECK(has(rt_trace_report(), "a thread of the design was queued for the Reactive region"));

    // A child forked by a program's thread is a program's thread: its wake takes the Reactive ring.
    for (const bool right : { false, true }) {
        rt_trace_begin();
        rt_trace_spawn(a, &kProgram, nullptr);
        rt_trace_wake(a, RT_TRACE_SEED | kReactiveRing, nullptr, 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        rt_trace_region(RT_TRACE_REG_REACTIVE);
        CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
        rt_trace_spawn(&proc_b, &kForkChild, a);
        rt_trace_wake(&proc_b, RT_TRACE_SPAWN | (right ? kReactiveRing : 0), nullptr, 0);
        CHECK_MESSAGE(rt_trace_halted() == (right ? 0 : 1), right);
    }

    // An `expect` verdict sends the thread to the Reactive region for good: its later wakes take that
    // ring.
    for (const bool right : { false, true }) {
        rt_trace_begin();
        rt_trace_spawn(a, &kProcess, nullptr);
        rt_trace_dispatch(a, RT_TRACE_INLINE);
        rt_trace_park(a, &kExpectWait, id_of(&rec_a));
        rt_trace_wake(a, RT_TRACE_EXPECT | kReactiveRing, &rec_a, 0);
        CHECK(rt_trace_halted() == 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        rt_trace_region(RT_TRACE_REG_REACTIVE);
        CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
        rt_trace_resume(a, &kExpectWait);
        rt_trace_park(a, &kDelay, 4);
        rt_trace_slot(4);
        rt_trace_wake(a, RT_TRACE_TIMER | (right ? kReactiveRing : 0), nullptr, 0);
        CHECK_MESSAGE(rt_trace_halted() == (right ? 0 : 1), right);
    }

    // The drain that pops an activation is the one of the ring it was queued on.
    rt_trace_begin();
    parked_on_clk(a);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_region(RT_TRACE_REG_REACTIVE);
    wrote_posedge(&clk);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 0);
    CHECK(rule() == "R25.1");
    CHECK(has(rt_trace_report(), "queued for the Active region ran in the Reactive region"));
}

// B39: a program's initial read an input port as x at time zero (seeded ahead of the port copy), and
// a #5 read in the slot of the design's write read the old value (the copy was woken behind the thread).
TEST_CASE("trace check: program code does not run while a program's continuous process is queued (R25.2, B39)") {
    void* copy = &proc_a;
    void* prog = &proc_b;
    for (const bool copy_first : { false, true }) {
        rt_trace_begin();
        rt_trace_spawn(copy, &kCopy, nullptr);
        rt_trace_spawn(prog, &kProgram, nullptr);
        rt_trace_wake(prog, RT_TRACE_SEED | kReactiveRing, nullptr, 0);
        rt_trace_wake(copy, RT_TRACE_SEED | kContRing, nullptr, 0);
        rt_trace_region(RT_TRACE_REG_ACTIVE);
        rt_trace_region(RT_TRACE_REG_REACTIVE);
        if (copy_first) {
            CHECK(rt_trace_dispatch(copy, RT_TRACE_RING) == 1);
            CHECK(rt_trace_dispatch(prog, RT_TRACE_RING) == 1);
            CHECK(rt_trace_halted() == 0);
        } else {
            CHECK(rt_trace_dispatch(prog, RT_TRACE_RING) == 0);
            CHECK(rule() == "R25.2");
            CHECK(has(rt_trace_report(), "would read its inputs stale"));
            CHECK(has(rt_trace_report(), "design.sv:24:3"));
        }
    }
}

// B43: an `expect`'s waiter queue named by the design-wide monitor number was one queue for the
// instances of a unit, so one instance's verdict released another's waiter. A wake names the record or
// queue its park waits on.
TEST_CASE("trace check: a wake names the record or queue its park waits on (R5.1, B43)") {
    struct Case {
        const rt_trace_control* park;
        uint32_t                cause;
    };
    const Case cases[] = { { &kJoin, RT_TRACE_CHILD },
                           { &kCall, RT_TRACE_CALLEE },
                           { &kSyncWait, RT_TRACE_SYNC },
                           { &kAwaitWait, RT_TRACE_AWAIT },
                           { &kExpectWait, RT_TRACE_EXPECT | kReactiveRing } };
    for (const Case& c : cases) {
        const auto parked = [&] {
            rt_trace_begin();
            rt_trace_spawn(&proc_a, &kProcess, nullptr);
            rt_trace_dispatch(&proc_a, RT_TRACE_INLINE);
            rt_trace_park(&proc_a, c.park, id_of(&rec_a));
        };
        parked();
        rt_trace_wake(&proc_a, c.cause, &rec_a, 0);
        CHECK_MESSAGE(rt_trace_halted() == 0, c.park->where);
        // A wake that names no record says nothing of it.
        parked();
        rt_trace_wake(&proc_a, c.cause, nullptr, 0);
        CHECK_MESSAGE(rt_trace_halted() == 0, c.park->where);
        parked();
        rt_trace_wake(&proc_a, c.cause, &rec_b, 0);
        CHECK_MESSAGE(rule() == "R5.1", c.park->where);
        CHECK_MESSAGE(has(rt_trace_report(), "another"), c.park->where);
        CHECK_MESSAGE(has(rt_trace_report(), "of the record or queue at"), c.park->where);
    }
}

TEST_CASE("trace check: a node on a list of occupants is off it before its activation ends or enters again (R23.1, B29)") {
    void* a = &proc_a;
    rt_trace_counts before{};
    rt_trace_totals(&before);
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    for (int pass = 0; pass < 2; ++pass) {
        rt_trace_block(a, &list_a, RT_TRACE_BLOCK_ENTER);
        rt_trace_block(a, &list_b, RT_TRACE_BLOCK_ENTER);
        rt_trace_block(a, &list_b, RT_TRACE_BLOCK_EXIT);
        rt_trace_block(a, &list_a, RT_TRACE_BLOCK_EXIT);
    }
    rt_trace_end(a);
    CHECK(rt_trace_halted() == 0);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.blocks == before.blocks + 8);

    // An exit that skips the block's end leaves the node linked, and the activation ends with it.
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_block(a, &list_a, RT_TRACE_BLOCK_ENTER);
    rt_trace_end(a);
    CHECK(rule() == "R23.1");
    CHECK(has(rt_trace_report(), "ended inside a block or task some disable names"));

    // ... or goes round the loop and enters the block again.
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_block(a, &list_a, RT_TRACE_BLOCK_ENTER);
    rt_trace_block(a, &list_a, RT_TRACE_BLOCK_ENTER);
    CHECK(rule() == "R23.1");
    CHECK(has(rt_trace_report(), "already on"));

    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_block(a, &list_a, RT_TRACE_BLOCK_EXIT);
    CHECK(rule() == "R23.1");
    CHECK(has(rt_trace_report(), "not on"));
}

TEST_CASE("trace check: a disable marks every occupant of the list it walks (R23.2)") {
    void* occupant = &proc_a;
    void* other    = &proc_b;
    void* disabler = &proc_d;
    const auto setup = [&](bool other_leaves) {
        rt_trace_begin();
        parked_on_clk(occupant);
        rt_trace_block(occupant, &list_a, RT_TRACE_BLOCK_ENTER);
        parked_on_clk(other);
        rt_trace_block(other, &list_a, RT_TRACE_BLOCK_ENTER);
        if (other_leaves) {
            rt_trace_block(other, &list_a, RT_TRACE_BLOCK_EXIT);
        }
        rt_trace_spawn(disabler, &kProcess, nullptr);
        rt_trace_dispatch(disabler, RT_TRACE_INLINE);
        rt_trace_disable(disabler, &list_a);
    };
    setup(false);
    rt_trace_counts before{};
    rt_trace_totals(&before);
    rt_trace_abort(occupant, &list_a, RT_TRACE_ABORT_DISABLE);
    rt_trace_wake(occupant, RT_TRACE_FORCE, nullptr, 0);
    rt_trace_abort(other, &list_a, RT_TRACE_ABORT_DISABLE);
    rt_trace_wake(other, RT_TRACE_FORCE, nullptr, 0);
    rt_trace_park(disabler, &kDelay, 1);
    CHECK(rt_trace_halted() == 0);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.aborts == before.aborts + 2);

    // The walk misses one: a node that was never on the list, or a walk that stopped early. The
    // activation would run on, and the checker finds it at the next park.
    setup(false);
    rt_trace_abort(occupant, &list_a, RT_TRACE_ABORT_DISABLE);
    rt_trace_wake(occupant, RT_TRACE_FORCE, nullptr, 0);
    rt_trace_park(disabler, &kDelay, 1);
    CHECK(rule() == "R23.2");
    CHECK(has(rt_trace_report(), "did not reach an activation that is inside the block or task it names"));
    CHECK(has(rt_trace_report(), "design.sv:3:3"));

    // An occupant that left before the walk is not owed.
    setup(true);
    rt_trace_abort(occupant, &list_a, RT_TRACE_ABORT_DISABLE);
    rt_trace_park(disabler, &kDelay, 1);
    CHECK(rt_trace_halted() == 0);
}

// B27: `disable F` of a named fork was a silent no-op: the children were on no list.
TEST_CASE("trace check: a fork child is on the lists the IR says it is within before it first runs (R23.3, B27)") {
    void* parent = &proc_a;
    void* child  = &proc_b;
    rt_trace_begin();
    rt_trace_spawn(parent, &kProcess, nullptr);
    rt_trace_dispatch(parent, RT_TRACE_INLINE);
    rt_trace_block(child, &list_a, RT_TRACE_BLOCK_RAMP);
    rt_trace_spawn(child, &kForkChild, parent);
    rt_trace_block(child, &list_a, RT_TRACE_BLOCK_INHERIT);
    CHECK(rt_trace_dispatch(child, RT_TRACE_INLINE) == 1);
    rt_trace_block(child, &list_a, RT_TRACE_BLOCK_EXIT);
    rt_trace_end(child);
    CHECK(rt_trace_halted() == 0);

    // The ramp linked it on no list.
    rt_trace_begin();
    rt_trace_spawn(parent, &kProcess, nullptr);
    rt_trace_dispatch(parent, RT_TRACE_INLINE);
    rt_trace_spawn(child, &kForkChild, parent);
    rt_trace_block(child, &list_a, RT_TRACE_BLOCK_INHERIT);
    CHECK(rt_trace_dispatch(child, RT_TRACE_INLINE) == 0);
    CHECK(rule() == "R23.3");
    CHECK(has(rt_trace_report(), "runs before it is an occupant"));

    // A ramp that linked an earlier incarnation of the address does not carry over.
    rt_trace_begin();
    rt_trace_spawn(child, &kForkChild, parent);
    rt_trace_dispatch(child, RT_TRACE_INLINE);
    rt_trace_block(child, &list_b, RT_TRACE_BLOCK_ENTER);
    rt_trace_block(child, &list_b, RT_TRACE_BLOCK_EXIT);
    rt_trace_end(child);
    rt_trace_spawn(child, &kForkChild, parent);
    rt_trace_block(child, &list_a, RT_TRACE_BLOCK_INHERIT);
    CHECK(rt_trace_dispatch(child, RT_TRACE_INLINE) == 0);
    CHECK(rule() == "R23.3");
}

// R23 × S1: `p.kill()` of an always parked at its static leading control resumed its body once and
// the always went on, because the loop's park had no abort poll.
TEST_CASE("trace check: an activation a kill or a disable marked leaves before it parks again (R23.4)") {
    void* a      = &proc_a;
    void* killer = &proc_b;
    const rt_trace_entry sub[] = { { &clk, "clk", RT_TRACE_POS, nullptr } };
    const auto           setup = [&] {
        rt_trace_begin();
        rt_trace_spawn(a, &kProcess, nullptr);
        rt_trace_subscribe(a, sub, 1);
        rt_trace_park(a, &kStatic, 0);
        rt_trace_spawn(killer, &kProcess, nullptr);
        rt_trace_dispatch(killer, RT_TRACE_INLINE);
    };

    // The kill force-wakes the always, which runs its body and parks at the static control again.
    setup();
    rt_trace_abort(a, nullptr, RT_TRACE_ABORT_KILL);
    rt_trace_wake(a, RT_TRACE_FORCE, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_resume(a, &kStatic);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rule() == "R23.4");
    CHECK(has(rt_trace_report(), "marked by a kill of its thread"));
    CHECK(has(rt_trace_report(), "parked again after a disable or a kill marked it to leave"));

    // With the poll, it ends.
    setup();
    rt_trace_abort(a, nullptr, RT_TRACE_ABORT_KILL);
    rt_trace_wake(a, RT_TRACE_FORCE, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_resume(a, &kStatic);
    rt_trace_end(a);
    CHECK(rt_trace_halted() == 0);

    // A disable's mark is obeyed by leaving the block it names, after which the activation may park.
    for (const bool leaves : { false, true }) {
        rt_trace_begin();
        parked_on_clk(a);
        rt_trace_block(a, &list_a, RT_TRACE_BLOCK_ENTER);
        rt_trace_spawn(killer, &kProcess, nullptr);
        rt_trace_dispatch(killer, RT_TRACE_INLINE);
        rt_trace_disable(killer, &list_a);
        rt_trace_abort(a, &list_a, RT_TRACE_ABORT_DISABLE);
        rt_trace_wake(a, RT_TRACE_FORCE, nullptr, 0);
        CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
        rt_trace_resume(a, &kWait);
        if (leaves) {
            rt_trace_block(a, &list_a, RT_TRACE_BLOCK_EXIT);
        }
        rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS, nullptr);
        rt_trace_park(a, &kWait, 0);
        CHECK_MESSAGE(rt_trace_halted() == (leaves ? 0 : 1), leaves);
    }

    // The two parks that run a child or a callee inline first can hold a mark: the wake they wait for
    // comes from its completion, and the poll after it takes the activation out.
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_abort(a, nullptr, RT_TRACE_ABORT_KILL);
    rt_trace_park(a, &kJoin, id_of(&rec_a));
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: only an activation a walk marked is force-woken (R23.5)") {
    void* marked   = &proc_a;
    void* unmarked = &proc_b;
    rt_trace_begin();
    parked_on_clk(marked);
    parked_on_clk(unmarked);
    // A run whose walks report nothing says nothing of its force wakes.
    rt_trace_wake(unmarked, RT_TRACE_FORCE, nullptr, 0);
    CHECK(rt_trace_halted() == 0);

    rt_trace_begin();
    parked_on_clk(marked);
    parked_on_clk(unmarked);
    rt_trace_abort(marked, nullptr, RT_TRACE_ABORT_KILL);
    rt_trace_wake(marked, RT_TRACE_FORCE, nullptr, 0);
    CHECK(rt_trace_halted() == 0);
    rt_trace_wake(unmarked, RT_TRACE_FORCE, nullptr, 0);
    CHECK(rule() == "R23.5");
    CHECK(has(rt_trace_report(), "no disable or kill had marked"));
}

// R22: the children of a fork and the join that waits for them. The parent is proc_a, the children
// proc_b, proc_d and proc_e (the tag bits of a parked handle are stripped, so proc_c would name proc_a),
// and the fork's record is rec_a.
namespace {

alignas(8) char proc_e, proc_f;

void* const kParent     = &proc_a;
void* const kChildren[] = { &proc_b, &proc_d, &proc_e };

// The parent runs and forks `width` children, which the spawns that follow announce.
void fork_of(uint32_t kind, uint32_t width) {
    rt_trace_begin();
    rt_trace_spawn(kParent, &kProcess, nullptr);
    rt_trace_dispatch(kParent, RT_TRACE_INLINE);
    rt_trace_fork(kParent, &rec_a, kind, width);
    for (uint32_t i = 0; i < width; ++i) {
        rt_trace_spawn(kChildren[i], &kForkChild, kParent);
    }
}

// A child resumed at the fork, which runs to its end.
void completes_inline(void* kid) {
    CHECK_MESSAGE(rt_trace_dispatch(kid, RT_TRACE_INLINE) == 1, std::string(rt_trace_report()));
    rt_trace_child_done(kid, &rec_a);
    rt_trace_end(kid);
}

// A child resumed at the fork, which parks at a `#ticks`.
void parks_inline(void* kid, uint64_t ticks) {
    CHECK_MESSAGE(rt_trace_dispatch(kid, RT_TRACE_INLINE) == 1, std::string(rt_trace_report()));
    rt_trace_park(kid, &kDelay, ticks);
}

// The timer of a child's delay expires at `tick` and the child runs to its completion, which the
// caller then follows (or not) with the wake of the parent.
void timer_completes(void* kid, uint64_t tick) {
    rt_trace_slot(tick);
    rt_trace_wake(kid, RT_TRACE_TIMER, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK_MESSAGE(rt_trace_dispatch(kid, RT_TRACE_RING) == 1, std::string(rt_trace_report()));
    rt_trace_resume(kid, &kDelay);
    rt_trace_child_done(kid, &rec_a);
}

void parent_joins() {
    rt_trace_park(kParent, &kJoin, id_of(&rec_a));
}

} // namespace

TEST_CASE("trace check: a join is woken by the last of its children and passed only then (R22)") {
    rt_trace_counts before{};
    rt_trace_totals(&before);
    fork_of(RT_TRACE_JOIN_ALL, 3);
    completes_inline(kChildren[0]);
    parks_inline(kChildren[1], 1);
    parks_inline(kChildren[2], 2);
    parent_joins();
    timer_completes(kChildren[1], 1);
    rt_trace_end(kChildren[1]);
    timer_completes(kChildren[2], 2);
    rt_trace_wake(kParent, RT_TRACE_CHILD, &rec_a, 0);
    rt_trace_end(kChildren[2]);
    CHECK(rt_trace_dispatch(kParent, RT_TRACE_RING) == 1);
    rt_trace_resume(kParent, &kJoin);
    rt_trace_join(kParent, &rec_a);
    CHECK(rt_trace_halted() == 0);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.forks == before.forks + 1);
    CHECK(after.joins == before.joins + 1);

    // Children that all completed at the fork leave nothing to park for: the parent passes the join.
    fork_of(RT_TRACE_JOIN_ALL, 2);
    completes_inline(kChildren[0]);
    completes_inline(kChildren[1]);
    rt_trace_join(kParent, &rec_a);
    CHECK(rt_trace_halted() == 0);

    // Woken by the second of three: the join would go on with a child running.
    fork_of(RT_TRACE_JOIN_ALL, 3);
    completes_inline(kChildren[0]);
    parks_inline(kChildren[1], 1);
    parks_inline(kChildren[2], 2);
    parent_joins();
    timer_completes(kChildren[1], 1);
    rt_trace_wake(kParent, RT_TRACE_CHILD, &rec_a, 0);
    CHECK(rule() == "R22.3");
    CHECK(has(rt_trace_report(), "2 of the 3 children had completed"));
    CHECK(has(rt_trace_report(), "woken by a completion its join is not due at"));

    // The last child completes and wakes nobody: the parent stays at its join for good.
    fork_of(RT_TRACE_JOIN_ALL, 3);
    completes_inline(kChildren[0]);
    parks_inline(kChildren[1], 1);
    parks_inline(kChildren[2], 2);
    parent_joins();
    timer_completes(kChildren[1], 1);
    rt_trace_end(kChildren[1]);
    timer_completes(kChildren[2], 2);
    rt_trace_end(kChildren[2]);
    rt_trace_slot_end();
    CHECK(rule() == "R22.3");
    CHECK(has(rt_trace_report(), "was not woken by the completion its join is due at"));
    CHECK(has(rt_trace_report(), "still parked at the join"));

    // The parent goes on past a join with a child still to complete.
    fork_of(RT_TRACE_JOIN_ALL, 3);
    completes_inline(kChildren[0]);
    rt_trace_join(kParent, &rec_a);
    CHECK(rule() == "R22.4");
    CHECK(has(rt_trace_report(), "1 of the 3 children had completed"));
}

TEST_CASE("trace check: a join_any is woken by its first child, which the parent passes it on, and the siblings start later (R22)") {
    // The first child completes at the fork: the parent does not park, and the siblings are queued.
    fork_of(RT_TRACE_JOIN_ANY, 3);
    completes_inline(kChildren[0]);
    rt_trace_wake(kChildren[1], RT_TRACE_SPAWN, nullptr, 0);
    rt_trace_wake(kChildren[2], RT_TRACE_SPAWN, nullptr, 0);
    rt_trace_join(kParent, &rec_a);
    CHECK(rt_trace_halted() == 0);
    rt_trace_park(kParent, &kDelay, 1);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(kChildren[1], RT_TRACE_RING) == 1);
    rt_trace_child_done(kChildren[1], &rec_a);
    rt_trace_end(kChildren[1]);
    CHECK(rt_trace_dispatch(kChildren[2], RT_TRACE_RING) == 1);
    rt_trace_child_done(kChildren[2], &rec_a);
    rt_trace_end(kChildren[2]);
    CHECK(rt_trace_halted() == 0);

    // Every child parks: the first to complete wakes the parent, and the second wakes nobody.
    fork_of(RT_TRACE_JOIN_ANY, 2);
    parks_inline(kChildren[0], 1);
    parks_inline(kChildren[1], 2);
    parent_joins();
    timer_completes(kChildren[0], 1);
    rt_trace_wake(kParent, RT_TRACE_CHILD, &rec_a, 0);
    rt_trace_end(kChildren[0]);
    CHECK(rt_trace_dispatch(kParent, RT_TRACE_RING) == 1);
    rt_trace_resume(kParent, &kJoin);
    rt_trace_join(kParent, &rec_a);
    rt_trace_park(kParent, &kDelay, 5);
    timer_completes(kChildren[1], 2);
    rt_trace_end(kChildren[1]);
    CHECK(rt_trace_halted() == 0);

    // The first completion wakes nobody: the next event shows the parent still at its join.
    fork_of(RT_TRACE_JOIN_ANY, 2);
    parks_inline(kChildren[0], 1);
    parks_inline(kChildren[1], 2);
    parent_joins();
    timer_completes(kChildren[0], 1);
    rt_trace_end(kChildren[0]);
    rt_trace_slot(2);
    CHECK(rule() == "R22.3");
    CHECK(has(rt_trace_report(), "join_any"));
    // Woken before any child completed: the parent would go on with all of them still running.
    fork_of(RT_TRACE_JOIN_ANY, 2);
    parks_inline(kChildren[0], 1);
    parks_inline(kChildren[1], 2);
    parent_joins();
    rt_trace_wake(kParent, RT_TRACE_CHILD, &rec_a, 0);
    CHECK(rule() == "R22.3");
    CHECK(has(rt_trace_report(), "0 of the 2 children had completed"));

    // With no child completed, a join_any does not pass.
    fork_of(RT_TRACE_JOIN_ANY, 2);
    parks_inline(kChildren[0], 1);
    rt_trace_join(kParent, &rec_a);
    CHECK(rule() == "R22.4");
    CHECK(has(rt_trace_report(), "0 of the 2 children had completed"));
}

// R22: a join_none child starts when its parent blocks or ends, and a join_any sibling not at all
// before that once the parent is due to go on (fork_test.cpp, "fork: join_none does not run its
// children at the fork" and "fork: join_any does not run the siblings it is not waiting for").
TEST_CASE("trace check: a child that the parent does not wait for starts from a ring, not at the fork (R22)") {
    // join_none: queued, and run once the parent has blocked.
    fork_of(RT_TRACE_JOIN_NONE, 2);
    rt_trace_wake(kChildren[0], RT_TRACE_SPAWN, nullptr, 0);
    rt_trace_wake(kChildren[1], RT_TRACE_SPAWN, nullptr, 0);
    rt_trace_park(kParent, &kDelay, 1);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(kChildren[0], RT_TRACE_RING) == 1);
    rt_trace_child_done(kChildren[0], &rec_a);
    rt_trace_end(kChildren[0]);
    CHECK(rt_trace_dispatch(kChildren[1], RT_TRACE_RING) == 1);
    rt_trace_child_done(kChildren[1], &rec_a);
    rt_trace_end(kChildren[1]);
    CHECK(rt_trace_halted() == 0);

    // join_none: resumed at the fork, while the parent runs.
    fork_of(RT_TRACE_JOIN_NONE, 2);
    CHECK(rt_trace_dispatch(kChildren[0], RT_TRACE_INLINE) == 0);
    CHECK(rule() == "R22.1");
    CHECK(has(rt_trace_report(), "a join_none child ran at the fork"));
    CHECK(has(rt_trace_report(), "design.sv:27:3"));

    // join_any: the first child completed, and a sibling is resumed all the same.
    fork_of(RT_TRACE_JOIN_ANY, 3);
    completes_inline(kChildren[0]);
    CHECK(rt_trace_dispatch(kChildren[1], RT_TRACE_INLINE) == 0);
    CHECK(rule() == "R22.1");
    CHECK(has(rt_trace_report(), "a join_any sibling ran after the first child had completed"));

    // join_any: while none has completed, the parent still waits for each of them and runs them inline.
    fork_of(RT_TRACE_JOIN_ANY, 3);
    parks_inline(kChildren[0], 1);
    parks_inline(kChildren[1], 2);
    completes_inline(kChildren[2]);
    CHECK(rt_trace_halted() == 0);

    // A fork in a function has no activation and no join: its children are not tied to a parent, and
    // run from the ring.
    rt_trace_begin();
    rt_trace_fork(nullptr, &rec_a, RT_TRACE_JOIN_NONE, 2);
    rt_trace_spawn(kChildren[0], &kForkChild, nullptr);
    rt_trace_spawn(kChildren[1], &kForkChild, nullptr);
    rt_trace_wake(kChildren[0], RT_TRACE_SPAWN, nullptr, 0);
    rt_trace_wake(kChildren[1], RT_TRACE_SPAWN, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(kChildren[0], RT_TRACE_RING) == 1);
    rt_trace_child_done(kChildren[0], &rec_a);
    rt_trace_end(kChildren[0]);
    CHECK(rt_trace_dispatch(kChildren[1], RT_TRACE_RING) == 1);
    rt_trace_child_done(kChildren[1], &rec_a);
    rt_trace_end(kChildren[1]);
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a child completes once into the record of the fork that made it, and a join is of a fork whole (R22)") {
    // Into the record of another fork: the count of the wrong parent would fall, and its wake would come
    // from the wrong fork.
    fork_of(RT_TRACE_JOIN_ALL, 2);
    rt_trace_fork(nullptr, &rec_b, RT_TRACE_JOIN_NONE, 1);
    CHECK(rt_trace_dispatch(kChildren[0], RT_TRACE_INLINE) == 1);
    rt_trace_child_done(kChildren[0], &rec_b);
    CHECK(rule() == "R22.2");
    CHECK(has(rt_trace_report(), "the record of another fork"));

    // Twice.
    fork_of(RT_TRACE_JOIN_ALL, 2);
    completes_inline(kChildren[0]);
    rt_trace_child_done(kChildren[0], &rec_a);
    CHECK(rule() == "R22.2");
    CHECK(has(rt_trace_report(), "completes twice"));

    // Into a record that no fork announced.
    fork_of(RT_TRACE_JOIN_ALL, 2);
    rt_trace_spawn(&proc_f, &kProcess, nullptr);
    CHECK(rt_trace_dispatch(&proc_f, RT_TRACE_INLINE) == 1);
    rt_trace_child_done(&proc_f, &rec_b);
    CHECK(rule() == "R22.2");
    CHECK(has(rt_trace_report(), "no fork announced"));

    // A join before the fork created every child it announced: its count could never be reached.
    rt_trace_begin();
    rt_trace_spawn(kParent, &kProcess, nullptr);
    rt_trace_dispatch(kParent, RT_TRACE_INLINE);
    rt_trace_fork(kParent, &rec_a, RT_TRACE_JOIN_ALL, 3);
    rt_trace_spawn(kChildren[0], &kForkChild, kParent);
    rt_trace_spawn(kChildren[1], &kForkChild, kParent);
    rt_trace_join(kParent, &rec_a);
    CHECK(rule() == "R22.4");
    CHECK(has(rt_trace_report(), "announced 3 children and had created 2"));

    // The join of a fork that another activation made.
    fork_of(RT_TRACE_JOIN_ALL, 1);
    completes_inline(kChildren[0]);
    rt_trace_spawn(kChildren[1], &kProcess, nullptr);
    rt_trace_dispatch(kChildren[1], RT_TRACE_INLINE);
    rt_trace_join(kChildren[1], &rec_a);
    CHECK(rule() == "R22.4");
    CHECK(has(rt_trace_report(), "another made"));

    // A new fork on the address a finished one used starts a new record: the old count is gone.
    fork_of(RT_TRACE_JOIN_ALL, 1);
    completes_inline(kChildren[0]);
    rt_trace_join(kParent, &rec_a);
    rt_trace_fork(kParent, &rec_a, RT_TRACE_JOIN_ALL, 1);
    rt_trace_spawn(kChildren[1], &kForkChild, kParent);
    rt_trace_join(kParent, &rec_a);
    CHECK(rule() == "R22.4");
    CHECK(has(rt_trace_report(), "0 of the 1 children had completed"));
}

// S9: the members of a fused group, each guarded on its own sensitivity (B15). The group is proc_a and lists `in_sig`
// and `mid_sig` for any change; member 0 reads `in_sig` and writes `mid_sig`, and member 1 lists `mid_sig`.
namespace {

int in_sig, mid_sig;

// A bracket that names no signal, so the names the lists gave are the ones a report uses.
const rt_trace_change_site kPlain{ "design.sv:8:3", nullptr, RT_TRACE_CHG_STORE };

const rt_trace_member_entry kMembers[] = { { &in_sig, RT_TRACE_ANY, 0 }, { &mid_sig, RT_TRACE_ANY, 1 } };

// The seed's reports of the group: its static list, its members, its park at the static control.
// A group that the seed does not queue waits at its static control; one it queues does not.
void seeded_group(void* a, uint32_t exact, bool parked = true) {
    const rt_trace_entry list[] = { { &in_sig, "in", RT_TRACE_ANY, nullptr }, { &mid_sig, "mid", RT_TRACE_ANY, nullptr } };
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_subscribe(a, list, 2);
    rt_trace_members(a, kMembers, 2, 2, exact);
    if (parked) {
        rt_trace_park(a, &kStatic, 0);
    }
}

// `x` changes, the group is woken by it and dispatched.
void woken_by(void* a, int* x) {
    rt_trace_change(&kPlain, x, RT_TRACE_SEEN_LEVEL);
    rt_trace_wake(a, RT_TRACE_STATIC, x, RT_TRACE_ANY);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
}

} // namespace

TEST_CASE("trace check: a member of a fused group runs when a change satisfies its own list, and that includes a change a member before it made (S9, B15)") {
    void* a = &proc_a;
    rt_trace_counts before{};
    rt_trace_totals(&before);
    // `in` wakes the group; member 0 runs and writes `mid`, which hands member 1 its trigger bit although the group is
    // already running; both run, and the group goes back to sleep.
    seeded_group(a, 1);
    woken_by(a, &in_sig);
    rt_trace_member(a, 0);
    rt_trace_change(&kPlain, &mid_sig, RT_TRACE_SEEN_LEVEL);
    rt_trace_member(a, 1);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rt_trace_halted() == 0);
    rt_trace_counts after{};
    rt_trace_totals(&after);
    CHECK(after.member_runs == before.member_runs + 2);

    // The next activation is the group's own again: woken by `mid` alone, only member 1 is owed.
    woken_by(a, &mid_sig);
    rt_trace_member(a, 1);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rt_trace_halted() == 0);

    // B15: the guard tested the word as it was when the group began, so member 1 never saw the bit that member 0's
    // write gave it, and the group went back to sleep without running it.
    seeded_group(a, 1);
    woken_by(a, &in_sig);
    rt_trace_member(a, 0);
    rt_trace_change(&kPlain, &mid_sig, RT_TRACE_SEEN_LEVEL);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rule() == "R6.4");
    CHECK(has(rt_trace_report(), "member 1 of the 2"));
    CHECK(has(rt_trace_report(), "did not run before the group went back to sleep"));
    CHECK(has(rt_trace_report(), "mid"));

    // A change that arrives while the group is parked or queued is owed to its member too.
    seeded_group(a, 1);
    rt_trace_change(&kPlain, &mid_sig, RT_TRACE_SEEN_LEVEL);
    rt_trace_wake(a, RT_TRACE_STATIC, &mid_sig, RT_TRACE_ANY);
    rt_trace_change(&kPlain, &in_sig, RT_TRACE_SEEN_LEVEL);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_member(a, 1);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rule() == "R6.4");
    CHECK(has(rt_trace_report(), "member 0 of the 2"));

    // A member that has run is not owed a second run by a change that comes after it: the producer is before its
    // consumers in a fused group, and a later write finds the member already past.
    seeded_group(a, 1);
    woken_by(a, &in_sig);
    rt_trace_member(a, 0);
    rt_trace_change(&kPlain, &mid_sig, RT_TRACE_SEEN_LEVEL);
    rt_trace_member(a, 1);
    rt_trace_change(&kPlain, &mid_sig, RT_TRACE_SEEN_LEVEL);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a member of a fused group does not run unless one of its own events fired, a wake that names none excepted (S9)") {
    void* a = &proc_a;
    // Member 1 runs, and only `in` changed.
    seeded_group(a, 1);
    woken_by(a, &in_sig);
    rt_trace_member(a, 0);
    rt_trace_member(a, 1);
    CHECK(rule() == "R5.5");
    CHECK(has(rt_trace_report(), "none of its own events fired"));
    CHECK(has(rt_trace_report(), "any change of mid"));

    // Trigger bits shared by two events of the group let a sibling's event run a member: no member run is then
    // unjustified.
    seeded_group(a, 0);
    woken_by(a, &in_sig);
    rt_trace_member(a, 0);
    rt_trace_member(a, 1);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rt_trace_halted() == 0);

    // A wake that cannot name its trigger sets every bit and runs every member: the seed's time-zero run.
    seeded_group(a, 1, false);
    rt_trace_wake(a, RT_TRACE_SEED, nullptr, 0);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_member(a, 0);
    rt_trace_member(a, 1);
    rt_trace_park(a, &kStatic, 0);
    CHECK(rt_trace_halted() == 0);

    // A member run of a group whose members the seed did not report.
    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_member(a, 0);
    CHECK(rule() == "R5.3");
    CHECK(has(rt_trace_report(), "did not report its members"));
}

// R16: the update of an inertial store, `assign #d x = y` (S26). The target's site is 3 and its records are
// irec_a and irec_b. The store evaluates a value and decides (rt_inertial_decide); the generated code arms the
// record the decision asks for; the wheel fires it, at the top of the slot of its deadline, and the commit
// applies it before the Active region.
namespace {

constexpr uint32_t kSite = 3;
char               irec_a, irec_b;

void evaluates(uint32_t same_pending, uint32_t same_cur, uint64_t epoch) {
    rt_trace_inertial_decide(kSite, same_pending, same_cur, epoch);
}

void arms(const void* rec, uint64_t ticks, uint64_t epoch) {
    rt_trace_inertial_arm(kSite, rec, ticks, epoch);
}

// The top of the slot of `tick`: the wheel fires the record, and the commit applies it if it is current.
void top_of_slot(uint64_t tick, const void* rec, uint32_t current) {
    rt_trace_slot(tick);
    rt_trace_inertial_fire(rec, current);
    rt_trace_region(RT_TRACE_REG_PREPONED);
    if (current != 0) {
        rt_trace_inertial_applied(rec);
    }
    rt_trace_region(RT_TRACE_REG_ACTIVE);
}

} // namespace

TEST_CASE("trace check: an inertial update is armed for its delay, fired by the wheel at its deadline and applied at the top of the slot (R16)") {
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    top_of_slot(5, &irec_a, 1);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // A new value before the deadline supersedes the pending update: the first record is dropped when it fires, and
    // the second is the one applied, at its own deadline.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(2);
    evaluates(0, 0, 2);
    arms(&irec_b, 5, 2);
    top_of_slot(5, &irec_a, 0);
    rt_trace_slot_end();
    top_of_slot(7, &irec_b, 1);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // A pulse shorter than the delay: the value goes back to the current one before the deadline, which cancels the
    // pending update and arms nothing.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(2);
    evaluates(0, 1, 0);
    top_of_slot(5, &irec_a, 0);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // The same value again keeps the timer running: no new record, and the pending one is applied at its deadline.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(2);
    evaluates(1, 0, 0);
    top_of_slot(5, &irec_a, 1);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // A zero delay cannot arm the wheel: its record fires at once and is applied in the NBA region of the slot.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    evaluates(0, 0, 1);
    arms(&irec_a, 0, 1);
    rt_trace_inertial_fire(&irec_a, 1);
    nba_begin();
    rt_trace_inertial_applied(&irec_a);
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a newer store cancels a pending inertial update, and the same value does not restart it (R16)") {
    // The pulse gets through: the record that the cancel superseded is applied.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(2);
    evaluates(0, 1, 0);
    rt_trace_slot(5);
    rt_trace_inertial_fire(&irec_a, 1);
    CHECK(rule() == "R16.4");
    CHECK(has(rt_trace_report(), "superseded"));

    // The update is lost: a record nothing superseded is dropped.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(5);
    rt_trace_inertial_fire(&irec_a, 0);
    CHECK(rule() == "R16.4");
    CHECK(has(rt_trace_report(), "no later store had superseded"));

    // A new value supersedes the record, and it is applied all the same.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(2);
    evaluates(0, 0, 2);
    arms(&irec_b, 5, 2);
    rt_trace_slot(5);
    rt_trace_inertial_fire(&irec_a, 1);
    CHECK(rule() == "R16.4");

    // The same value restarts the timer: a record was armed for a value the pending update already holds.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(2);
    evaluates(1, 0, 2);
    CHECK(rule() == "R16.1");
    CHECK(has(rt_trace_report(), "the timer restarted"));

    // The value went back to the current one and a record was armed anyway; a new value armed none.
    rt_trace_begin();
    evaluates(0, 1, 1);
    CHECK(rule() == "R16.1");
    rt_trace_begin();
    evaluates(0, 0, 0);
    CHECK(rule() == "R16.1");
    CHECK(has(rt_trace_report(), "armed no update"));

    // A record armed that no decision asked for, and one armed for another epoch than the decision's.
    rt_trace_begin();
    arms(&irec_a, 5, 1);
    CHECK(rule() == "R16.2");
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 2);
    CHECK(rule() == "R16.2");
}

TEST_CASE("trace check: an inertial record fires at its deadline and its update is applied before the Active region (R16)") {
    // Fired a tick late, and a tick early.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(6);
    rt_trace_inertial_fire(&irec_a, 1);
    CHECK(rule() == "R16.3");
    CHECK(has(rt_trace_report(), "armed for tick 5"));
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(4);
    rt_trace_inertial_fire(&irec_a, 1);
    CHECK(rule() == "R16.3");

    // A record that the wheel never fired: found when the slot of its deadline is over.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(5);
    rt_trace_slot_end();
    CHECK(rule() == "R16.3");
    CHECK(has(rt_trace_report(), "never fired"));

    // A record that nothing armed.
    rt_trace_begin();
    rt_trace_slot(5);
    rt_trace_inertial_fire(&irec_a, 1);
    CHECK(rule() == "R16.3");

    // Fired by the wheel and not applied by the commit at the top of the slot: the Active region would read the old value.
    rt_trace_begin();
    evaluates(0, 0, 1);
    arms(&irec_a, 5, 1);
    rt_trace_slot(5);
    rt_trace_inertial_fire(&irec_a, 1);
    rt_trace_region(RT_TRACE_REG_PREPONED);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    CHECK(rule() == "R16.5");
    CHECK(has(rt_trace_report(), "before the Active region began"));

    // A zero-delay record applied outside an NBA pass, and one never applied.
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    evaluates(0, 0, 1);
    arms(&irec_a, 0, 1);
    rt_trace_inertial_fire(&irec_a, 1);
    rt_trace_inertial_applied(&irec_a);
    CHECK(rule() == "R16.5");
    CHECK(has(rt_trace_report(), "outside an NBA pass"));
    rt_trace_begin();
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    evaluates(0, 0, 1);
    arms(&irec_a, 0, 1);
    rt_trace_inertial_fire(&irec_a, 1);
    rt_trace_slot_end();
    CHECK(rule() == "R16.5");
    CHECK(has(rt_trace_report(), "still unapplied"));
}

// R11: a timed nonblocking update is created when its delay elapses (`<= #d`) or when its event occurs the number of
// times it counts (`<= @e`, `<= repeat(n) @e`), and not before (B23: a `<= @e` on a declared event was never counted
// down, so it was never created).

TEST_CASE("trace check: a `<= #d` update is created when its delay elapses and not before (R11)") {
    // Armed for 5 ticks at tick 0: the timer releases it at tick 5, and the NBA region applies it.
    rt_trace_begin();
    q = 0;
    nba_arm(&rec1, kTimer, &q, 1, 5);
    rt_trace_slot(5);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_TIMER, 0);
    nba_begin();
    q = 1;
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // Released a tick early.
    rt_trace_begin();
    nba_arm(&rec1, kTimer, &q, 1, 5);
    rt_trace_slot(4);
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_TIMER, 0);
    CHECK(rule() == "R11.1");
    CHECK(has(rt_trace_report(), "created at tick 4"));
    CHECK(has(rt_trace_report(), "ends at tick 5"));

    // Never released: found when the slot of its deadline is over.
    rt_trace_begin();
    nba_arm(&rec1, kTimer, &q, 1, 5);
    rt_trace_slot(5);
    rt_trace_slot_end();
    CHECK(rule() == "R11.1");
    CHECK(has(rt_trace_report(), "no update was created"));

    // A record armed for a later tick is not owed at the end of an earlier slot.
    rt_trace_begin();
    nba_arm(&rec1, kTimer, &q, 1, 5);
    rt_trace_slot(2);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a `<= @e` update is created at the n-th occurrence of its control, and only then (R11, B23)") {
    // `q <= repeat(2) @(posedge clk) v`: the first posedge counts one, a negedge none, the second creates it.
    rt_trace_begin();
    q = 0;
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 2, &clk, RT_TRACE_POS);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_change(&kStore, &clk, seen_for(1, 0, false));
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_EVENT, 3);
    nba_begin();
    q = 1;
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // Created at the first occurrence of two.
    rt_trace_begin();
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 2, &clk, RT_TRACE_POS);
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_EVENT, 1);
    CHECK(rule() == "R11.2");
    CHECK(has(rt_trace_report(), "1 of the 2 occurrences"));

    // The second occurrence comes and nothing creates the update: the next event shows it.
    rt_trace_begin();
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 2, &clk, RT_TRACE_POS);
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    rt_trace_change(&kStore, &clk, seen_for(0, 1, false));
    CHECK(rt_trace_halted() == 0);
    rt_trace_slot_end();
    CHECK(rule() == "R11.2");
    CHECK(has(rt_trace_report(), "was not created"));

    // B23: the record waits on a declared event, and `-> e` fires the object's own waiters, never the record.
    rt_trace_begin();
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 1, &ev1, RT_TRACE_OBJ);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW);
    rt_trace_slot_end();
    CHECK(rule() == "R11.2");
    // ... and with the release the trigger owes, it is clean.
    rt_trace_begin();
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 1, &ev1, RT_TRACE_OBJ);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW);
    rt_trace_nba_release(&rec1, RT_TRACE_NBA_EVENT, 1);
    rt_trace_region(RT_TRACE_REG_ACTIVE);
    nba_begin();
    q = 1;
    nba_end();
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
}

TEST_CASE("trace check: a `<= @(a or b)` update counts an occurrence of either, and an event listed twice counts one (R11, R21)") {
    // Either entry matures a count of one; an event that is not listed does not.
    for (const void* occurs : { static_cast<const void*>(&clk), static_cast<const void*>(&done) }) {
        rt_trace_begin();
        nba_arm(&rec1, kEvent, &q, 1, 0);
        nba_waits(&rec1, 1, &clk, RT_TRACE_POS);
        nba_waits(&rec1, 1, &done, RT_TRACE_POS);
        rt_trace_change(&kStore, occurs, seen_for(0, 1, false));
        rt_trace_nba_release(&rec1, RT_TRACE_NBA_EVENT, 1);
        CHECK(rt_trace_halted() == 0);
    }
    rt_trace_begin();
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 1, &clk, RT_TRACE_POS);
    rt_trace_change(&kStore, &done, seen_for(0, 1, false));
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);

    // One occurrence of an event listed under two names counts once: a count of two is not reached.
    rt_trace_begin();
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 2, &ev1, RT_TRACE_OBJ);
    nba_waits(&rec1, 2, &ev1, RT_TRACE_OBJ);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW);
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    rt_trace_trigger(&ev1, RT_TRACE_TRIGGER_NOW);
    rt_trace_slot_end();
    CHECK(rule() == "R11.2");

    // A change entry counts any write that changed the value; a record armed on a slot the trace was not told the wait
    // of (an old hand-built sequence) is not asked.
    rt_trace_begin();
    nba_arm(&rec1, kEvent, &q, 1, 0);
    nba_waits(&rec1, 1, &clk, RT_TRACE_ANY);
    rt_trace_change(&kStore, &clk, seen_for(1, 1, false));
    rt_trace_slot_end();
    CHECK(rt_trace_halted() == 0);
    rt_trace_change(&kStore, &clk, seen_for(1, 0, false));
    rt_trace_slot_end();
    CHECK(rule() == "R11.2");
}
#endif // CLOTHO_EXTRA_ASSERTS
