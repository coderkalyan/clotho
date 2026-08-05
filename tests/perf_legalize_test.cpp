#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Lower.h"
#include "legalize/Legalize.h"

using namespace clotho;

// Scale check on real RTL, opt-in via --no-skip.
//
// The 28-case corpus proves the analysis is RIGHT; it says nothing about whether
// it stays linear. yosys's proc_mux re-walks the whole decision tree once per
// written signal and measures O(S^2) -- 18.3 s on this same design -- so the one
// thing that must not regress is the per-signal cost.
//
// Skipped by default because hazard3's RTL is fetched (tests/perf/cases/hazard3/
// fetch.sh), not tracked.
TEST_CASE("PERF legalize hazard3" * doctest::skip()) {
    const std::filesystem::path dir = std::filesystem::path(CLOTHO_TEST_DIR) / "perf/cases/hazard3/src";
    if (!std::filesystem::is_directory(dir)) {
        MESSAGE("hazard3 sources absent; run tests/perf/cases/hazard3/fetch.sh");
        return;
    }

    std::vector<std::string> owned;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".v") {
            owned.push_back(e.path().string());
        }
    }
    REQUIRE_FALSE(owned.empty());

    const std::vector<std::string_view> paths(owned.begin(), owned.end());
    const LowerResult                   r = lower_files(paths);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    // Best of five: the walk is the measurement, not the frontend.
    double best = 1e18;
    for (int k = 0; k < 5; ++k) {
        const auto             a   = std::chrono::steady_clock::now();
        const legalize::Result res = legalize::run(*r.module, nullptr);
        const auto             b   = std::chrono::steady_clock::now();
        best                       = std::min(best, std::chrono::duration<double, std::milli>(b - a).count());
        REQUIRE_FALSE(res.processes.empty());
    }

    const legalize::Result res  = legalize::run(*r.module, nullptr);
    size_t                 sigs = 0;
    size_t                 rej  = 0;
    for (const legalize::ProcessResult& p : res.processes) {
        sigs += p.signals.size();
        rej += static_cast<size_t>(!p.synthesizable);
    }
    std::map<int, int> why;
    for (const legalize::Note& n : res.notes) {
        ++why[static_cast<int>(n.kind)];
    }

    MESSAGE(
      "legalize ", best, " ms | processes ", res.processes.size(), " (rejected ", rej, ") | written signals ", sigs);
    for (const auto& [kind, count] : why) {
        MESSAGE("  finding ", kind, " x", count);
    }

    // A generous ceiling: the measurement is ~1 ms, and Clotho's existing
    // whole-design analysis walk over the same design is 11.5 ms. This fires on a
    // return to quadratic, not on ordinary noise.
    CHECK(best < 50.0);
}
