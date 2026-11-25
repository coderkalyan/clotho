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
                              "rt_trace_pend", "rt_trace_trigger", "rt_trace_slot_end", "rt_trace_finish" }) {
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
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS);
    rt_trace_park(a, &kHead, 0);
}

void body_wait(void* a) {
    rt_trace_waits_for(a, &done, "done", RT_TRACE_POS);
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
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_resume(a, &kHead);
    body_wait(a);

    // The second posedge of clk finds no entry: the body is not waiting for clk.
    rt_trace_slot(3);
    rt_trace_slot(4);
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
    const rt_trace_entry  sub[] = { { &clk, "clk", RT_TRACE_POS } };
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_subscribe(a, sub, 1);
    rt_trace_park(a, &kStatic, 0);

    rt_trace_slot(1);
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
    const rt_trace_entry sub[] = { { &clk, "clk", RT_TRACE_POS } };
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_subscribe(a, sub, 1);
    rt_trace_park(a, &kStatic, 0);
    rt_trace_wake(a, RT_TRACE_STATIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_dispatch(a, RT_TRACE_RING) == 1);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS);
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
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_NEG);
    rt_trace_park(a, &kWait, 0);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rule() == "R5.1");

    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_ANY);
    rt_trace_park(a, &kWait, 0);
    rt_trace_wake(a, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS);
    CHECK(rt_trace_halted() == 0);

    rt_trace_begin();
    rt_trace_spawn(a, &kProcess, nullptr);
    rt_trace_dispatch(a, RT_TRACE_INLINE);
    rt_trace_waits_for(a, &clk, "clk", RT_TRACE_POS);
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

// A statement arms a record, which a timer or an event releases later.
void nba_arm(const void* rec, const rt_trace_nba_site& s, uint32_t* dst, uint32_t value) {
    const uint64_t v = value;
    rt_trace_nba_stage(rec, &s, dst, nullptr, &v, nullptr, nullptr, nullptr);
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
    after_finish([] { rt_trace_wake(&proc_b, RT_TRACE_DYNAMIC, &clk, RT_TRACE_POS); });
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
#endif

#endif // CLOTHO_EXTRA_ASSERTS
