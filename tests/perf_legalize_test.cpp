#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Lower.h"
#include "legalize/Legalize.h"

using namespace clotho;

namespace {

// RTL only: a testbench is simulation source and rejecting it is correct, so
// including one would measure the wrong thing.
bool is_testbench(const std::filesystem::path& p) {
    const std::string n = p.filename().string();

    return n.find("testbench") != std::string::npos || n.find("_tb") != std::string::npos;
}

struct Coverage {
    double best_ms   = 0;
    size_t processes = 0;
    size_t rejected  = 0;
    size_t signals   = 0;
};

Coverage measure(const std::filesystem::path& dir) {
    std::vector<std::string> owned;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const auto ext = e.path().extension();
        if ((ext == ".v" || ext == ".sv") && !is_testbench(e.path())) {
            owned.push_back(e.path().string());
        }
    }
    std::sort(owned.begin(), owned.end());
    REQUIRE_FALSE(owned.empty());

    const std::vector<std::string_view> paths(owned.begin(), owned.end());
    const LowerResult                   r = lower_files(paths);
    if (!r.ok) {
        // A frontend gap is a different axis from legalization coverage, and
        // hiding it would be worse than reporting it.
        // Report the errors only: the warning stream buries them, and the errors
        // are the actionable part.
        std::string       errs;
        const std::string all = r.diagnostic_text();
        for (size_t a = 0, b; a < all.size(); a = b + 1) {
            b = std::min(all.find('\n', a), all.size());
            const std::string_view line(all.data() + a, b - a);
            if (line.find("error:") != std::string_view::npos) {
                errs += std::string(line) + "\n";
            }
        }
        MESSAGE("    does not lower:\n", errs);
        return Coverage{};
    }

    Coverage out;
    out.best_ms = 1e18;
    // Best of five: the walk is the measurement, not the frontend.
    for (int k = 0; k < 5; ++k) {
        const auto             a   = std::chrono::steady_clock::now();
        const legalize::Result res = legalize::run(*r.module, nullptr);
        const auto             b   = std::chrono::steady_clock::now();
        out.best_ms                = std::min(out.best_ms, std::chrono::duration<double, std::milli>(b - a).count());
        REQUIRE_FALSE(res.processes.empty());
    }

    const legalize::Result res = legalize::run(*r.module, nullptr);
    out.processes              = res.processes.size();
    for (const legalize::ProcessResult& p : res.processes) {
        out.signals += p.signals.size();
        out.rejected += static_cast<size_t>(!p.synthesizable);
    }
    std::map<int, int> why;
    for (const legalize::Note& n : res.notes) {
        ++why[static_cast<int>(n.kind)];
    }
    for (const auto& [kind, count] : why) {
        MESSAGE("    finding ", kind, " x", count);
    }

    return out;
}

} // namespace

// Scale and coverage on real RTL, opt-in via --no-skip.
//
// The 28-case reset corpus proves the analysis is RIGHT and says nothing about
// whether it stays linear or how much real source it accepts. Both are measured
// here, on more than one design, because a single design hides exactly the gap
// that functions turned out to be: hazard3 rejected 14 processes for 200 calls
// until calls were legalized as operators.
//
// yosys's proc_mux re-walks the whole decision tree once per written signal and
// measures O(S^2) -- 18.3 s on hazard3 -- so per-signal cost is the thing that
// must not regress.
//
// Skipped by default because this RTL is fetched (each case's fetch.sh), not
// tracked.
TEST_CASE("PERF legalize real designs" * doctest::skip()) {
    const std::filesystem::path       root     = std::filesystem::path(CLOTHO_TEST_DIR) / "perf/cases";
    static constexpr std::string_view kCases[] = { "picorv32", "hazard3", "coremark_cv32e40p" };

    size_t measured = 0;
    for (const std::string_view name : kCases) {
        const std::filesystem::path dir = root / name / "src";
        if (!std::filesystem::is_directory(dir)) {
            MESSAGE(name, ": sources absent; run its fetch.sh");
            continue;
        }
        ++measured;
        const Coverage c = measure(dir);
        if (c.processes == 0) {
            continue;
        }
        MESSAGE(name,
                ": ",
                c.best_ms,
                " ms | processes ",
                c.processes,
                " (rejected ",
                c.rejected,
                ") | written signals ",
                c.signals);

        // Linear, not quadratic. A loose ceiling scaled by size: it fires on a
        // return to per-signal re-walking, not on ordinary noise.
        CHECK(c.best_ms < 1.0 + 0.05 * static_cast<double>(c.signals));
    }
    if (measured == 0) {
        MESSAGE("no fetched designs; nothing measured");
    }
}
