#pragma once

// Trace-check mode for the suite (docs/VERIFICATION.md, "Detectors"; docs/design/
// scheduler-trace.md §6): in a build with CLOTHO_EXTRA_ASSERTS every design a test simulates
// is checked against the scheduler's rules while it runs, and the test that ran a design that
// breaks one fails, naming the rule, the time, the activation, the control and the cause. The
// checker is the runtime's (src/runtime/TraceCheck.cpp); this is the suite's side of it: the
// hook run_jit calls once a run that broke a rule has returned, the report a child process
// hands back through a file (Subprocess.h), the known list, and the summary.
//
// On by default in such a build, as twin mode is; CLOTHO_NO_TRACE_CHECK=1 turns it off, to time
// the suite without it. A violation is a Clotho bug or a rule that needs rewording, never a
// property of the design. A test case that exists to run a design with a known violation is
// listed in tests/trace-check-known.txt.

#ifdef CLOTHO_EXTRA_ASSERTS

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "KnownList.h"
#include "Subprocess.h"
#include "ir/Jit.h"
#include "runtime/Trace.h"

namespace clotho::test::trace_check {

// A test case whose design breaks a rule and is understood: `bug` (a Clotho bug, with a row in
// docs/design/scheduler-rules.md §4 or an entry in docs/TODO.md) or `rule` (the LRM allows what
// the predicate refuses; the rule wants rewording).
struct KnownEntry {
    std::string category;
    std::string rule; // "R5.1"
    std::string reason;
    size_t      line = 0;
};

// Parse the known list: `<category> "<test case name>" <rule> <reason, to end of line>`, the
// format of tests/config-matrix-known.txt with the rule first in the reason.
inline std::map<std::string, KnownEntry> parse_known(std::string_view text, std::string_view file = "trace-check-known.txt") {
    std::map<std::string, KnownEntry> entries;
    for (const auto& [name, e] : known_list::parse(text, file, { "bug", "rule" })) {
        const size_t           end  = e.reason.find_first_of(" \t");
        const std::string      rule = e.reason.substr(0, end);
        const std::string_view rest = end == std::string::npos ? std::string_view{} : std::string_view(e.reason).substr(end);
        const bool             shaped = rule.size() >= 2 && rule[0] == 'R' &&
                            rule.find_first_not_of("0123456789.", 1) == std::string::npos && rule.back() != '.';
        const auto fail = [&](const std::string& why) {
            throw std::runtime_error(std::string(file) + ":" + std::to_string(e.line) + ": " + why);
        };
        if (!shaped) {
            fail("the reason must start with the rule the entry covers (R5.1, ...), not '" + rule + "'");
        }
        if (rest.find_first_not_of(" \t") == std::string_view::npos) {
            fail("an entry needs a reason after its rule");
        }
        entries.emplace(name, KnownEntry{ e.category, rule, std::string(rest.substr(rest.find_first_not_of(" \t"))), e.line });
    }
    return entries;
}

// The check and its bookkeeping. The suite installs one (install below); the tests of the check
// build their own.
class Suite {
public:
    explicit Suite(std::map<std::string, KnownEntry> known)
        : known_(std::move(known)) {}

    // The doctest test case the designs that follow belong to; known entries are keyed by it.
    void begin_test(std::string name) { test_ = std::move(name); }

    // A known entry whose test passed without breaking its rule is out of date: the bug was
    // fixed, or the test changed.
    void end_test(bool success) {
        if (success && known_.count(test_) != 0 && seen_.count(test_) == 0) {
            stale_.push_back(test_);
        }
        test_.clear();
    }

    // A run broke `rule`: throw, so that the test that ran the design fails, unless the test case
    // running is listed for that rule. An entry covers only its own rule.
    void violation(const std::string& rule, const std::string& report) {
        ++violations_;
        const auto it = known_.find(test_);
        if (it != known_.end() && it->second.rule == rule) {
            ++known_hits_;
            seen_.insert(test_);
            return;
        }
        throw std::runtime_error(
          "scheduler trace violation (" + rule + "): a design this test simulated breaks a rule of the scheduler\n" +
          report +
          "  A Clotho bug, or a rule that needs rewording (docs/design/scheduler-rules.md §3), never a property of the\n"
          "  design. Replay it with `clotho run` for locations. Once it is understood, a bug gets a row in\n"
          "  docs/design/scheduler-rules.md §4 and an entry in tests/trace-check-known.txt.");
    }

    uint64_t                                 violations() const { return violations_; }
    uint64_t                                 known_hits() const { return known_hits_; }
    const std::vector<std::string>&          stale() const { return stale_; }
    const std::map<std::string, KnownEntry>& known() const { return known_; }

private:
    std::map<std::string, KnownEntry> known_;
    std::set<std::string>             seen_;
    std::vector<std::string>          stale_;
    std::string                       test_;
    uint64_t                          violations_ = 0;
    uint64_t                          known_hits_ = 0;
};

// ---- The suite's own instance ----

inline std::unique_ptr<Suite>& installed() {
    static std::unique_ptr<Suite> suite;
    return suite;
}

inline std::string known_file() {
    return std::string(CLOTHO_TEST_DIR) + "/trace-check-known.txt";
}

// Hook every run that follows, in this process and in the child processes of Subprocess.h.
inline void hook_up() {
    ir::set_trace_hook([](const ir::TraceViolation& v) { installed()->violation(v.rule, v.report); });
    trace_child_report = [](const std::string& rule, const std::string& report) { installed()->violation(rule, report); };
}

// Turn the mode on: read the known list and hook every run that follows. Throws on a malformed
// known list.
inline void install() {
    const std::string path = known_file();
    std::ifstream     in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("trace check: cannot read " + path);
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    installed() = std::make_unique<Suite>(parse_known(text, path));
    hook_up();
}

// Takes the suite's hooks off for a scope. A test of the check runs designs that break a rule on
// purpose, and installs hooks of its own.
class Paused {
public:
    Paused() {
        ir::set_trace_hook({});
        trace_child_report = {};
    }
    ~Paused() {
        ir::set_trace_hook({});
        trace_child_report = {};
        if (installed()) {
            hook_up();
        }
    }
    Paused(const Paused&)            = delete;
    Paused& operator=(const Paused&) = delete;
};

inline void begin_test(const char* name) {
    if (installed()) {
        installed()->begin_test(name);
    }
}

inline void end_test(bool success) {
    if (installed()) {
        installed()->end_test(success);
    }
}

// Print the mode's summary and return the status the run must end with: non-zero when a known
// entry is out of date. The counts show a checker that stopped emitting as zeros.
inline int finish() {
    const Suite* suite = installed().get();
    if (suite == nullptr) {
        return 0;
    }
    rt_trace_counts t{};
    rt_trace_totals(&t);
    std::fprintf(stderr,
                 "trace check: %llu runs, %llu events, %llu parks, %llu wakes, %llu writes, %llu violations (%llu known)\n",
                 static_cast<unsigned long long>(t.runs), static_cast<unsigned long long>(t.events),
                 static_cast<unsigned long long>(t.parks), static_cast<unsigned long long>(t.wakes),
                 static_cast<unsigned long long>(t.changes), static_cast<unsigned long long>(suite->violations()),
                 static_cast<unsigned long long>(suite->known_hits()));
    std::fprintf(stderr,
                 "trace check: NBA updates created: %llu block, %llu array, %llu aggregate, %llu zero-delay, %llu timer, "
                 "%llu event; %llu NBA passes, %llu pends, %llu triggers, %llu finishes; %llu block events, %llu aborts; "
                 "%llu forks, %llu joins, %llu member runs, %llu inertial arms\n",
                 static_cast<unsigned long long>(t.creates[RT_TRACE_NBA_BLOCK]),
                 static_cast<unsigned long long>(t.creates[RT_TRACE_NBA_ARRAY]),
                 static_cast<unsigned long long>(t.creates[RT_TRACE_NBA_AGG]),
                 static_cast<unsigned long long>(t.creates[RT_TRACE_NBA_ZERO]),
                 static_cast<unsigned long long>(t.creates[RT_TRACE_NBA_TIMER]),
                 static_cast<unsigned long long>(t.creates[RT_TRACE_NBA_EVENT]),
                 static_cast<unsigned long long>(t.nba_passes), static_cast<unsigned long long>(t.pends),
                 static_cast<unsigned long long>(t.triggers), static_cast<unsigned long long>(t.finishes),
                 static_cast<unsigned long long>(t.blocks), static_cast<unsigned long long>(t.aborts),
                 static_cast<unsigned long long>(t.forks), static_cast<unsigned long long>(t.joins),
                 static_cast<unsigned long long>(t.member_runs), static_cast<unsigned long long>(t.inertial_arms));
    for (const std::string& name : suite->stale()) {
        const KnownEntry& e = suite->known().at(name);
        std::fprintf(stderr,
                     "trace check: tests/trace-check-known.txt line %zu lists \"%s\", which no longer breaks %s: delete the "
                     "entry\n",
                     e.line, name.c_str(), e.rule.c_str());
    }
    return suite->stale().empty() ? 0 : 1;
}

} // namespace clotho::test::trace_check

#else

namespace clotho::test::trace_check {

inline void begin_test(const char*) {}
inline void end_test(bool) {}
inline int  finish() { return 0; }

} // namespace clotho::test::trace_check

#endif // CLOTHO_EXTRA_ASSERTS
