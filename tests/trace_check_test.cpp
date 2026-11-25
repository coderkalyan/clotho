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
                              "rt_trace_resume", "rt_trace_end", "rt_trace_slot", "rt_trace_halted" }) {
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
#endif

#endif // CLOTHO_EXTRA_ASSERTS
