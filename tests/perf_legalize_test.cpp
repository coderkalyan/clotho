#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <slang/ast/Compilation.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/util/Bag.h>

#include "Lower.h"
#include "legalize/Legalize.h"
#include "legalize/Materialize.h"

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
    // CLOTHO_PERF_CASES points at a checkout that has already fetched this RTL, so
    // running against another tree needs no symlink into this one. (Making that
    // symlink, then cleaning it up with `rm -rf tests/perf`, is how I briefly
    // deleted the tracked perf harness.)
    const char* const           env = std::getenv("CLOTHO_PERF_CASES");
    const std::filesystem::path root =
      env != nullptr ? std::filesystem::path(env) : std::filesystem::path(CLOTHO_TEST_DIR) / "perf/cases";
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

namespace {

// Classification per signal, for signals that are comparable across a round trip.
//
// A signal written by two processes has two classifications, so collapsing by name
// compares arbitrary pairs -- an unsound comparison that produced phantom
// divergences until it was noticed. Only single-writer signals are compared, and
// loop-written ones are excluded because materialize emits a loop body once
// without its iteration (GuardedStore::in_loop marks them).
struct Classified {
    std::map<std::string, std::pair<int, bool>> by_name;
    size_t                                      looped = 0;
    size_t                                      multi  = 0;
};

Classified classify(const ir::Module& m, const legalize::Result& r, bool drop_looped) {
    std::map<std::string, int> writers;
    for (const legalize::ProcessResult& p : r.processes) {
        for (const legalize::SignalStores& e : p.signals) {
            ++writers[m.signal(e.signal).name];
        }
    }

    Classified out;
    for (const legalize::ProcessResult& p : r.processes) {
        for (const legalize::SignalStores& e : p.signals) {
            const std::string& n = m.signal(e.signal).name;
            if (writers[n] != 1) {
                ++out.multi;
                continue;
            }
            bool looped = false;
            for (const legalize::GuardedStore& st : e.stores) {
                looped = looped || st.in_loop;
            }
            if (looped && drop_looped) {
                ++out.looped;
                continue;
            }
            out.by_name[n] = { static_cast<int>(e.kind), e.async_reset };
        }
    }

    return out;
}

// Does `text` parse and elaborate as IEEE 1364-2005?
size_t errors_as_2005(std::string_view text) {
    slang::Bag                   options;
    slang::parsing::LexerOptions lex;
    lex.languageVersion = slang::LanguageVersion::v1364_2005;
    options.set(lex);
    slang::ast::CompilationOptions copts;
    copts.languageVersion = slang::LanguageVersion::v1364_2005;
    options.set(copts);

    slang::ast::Compilation comp(options);
    comp.addSyntaxTree(slang::syntax::SyntaxTree::fromText(text, options, "emitted"));
    size_t n = 0;
    for (const slang::Diagnostic& d : comp.getAllDiagnostics()) {
        n += static_cast<size_t>(d.isError());
    }

    return n;
}

std::string mangled(std::string_view name) {
    std::string out;
    for (const char c : name) {
        out += (c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? c : '_';
    }

    return out;
}

} // namespace

// The round trip that matters: render a whole CPU, re-lower the rendered source,
// re-classify, and require every comparable signal to land the same way.
//
// The 28-case corpus round trip cannot catch what this does. Everything below was
// found by running it, and each was a real unfaithfulness in the renderer rather
// than a limit of the analysis:
//
//   * partial writes rendered as whole ones (`q[i] = x` as `q = x`), turning every
//     bit-reversal loop's target from a latch into combinational logic;
//   * `casez` patterns rendered as exact constants, because 4-state literals
//     collapsed to `'x` and threw the wildcards away -- every predecode signal in
//     hazard3 lost its arms and re-read as a latch;
//   * `case` arms dropped entirely, destroying the completeness a default provides;
//   * dynamic part-selects and element selects unhandled.
TEST_CASE("PERF materialize round-trips real designs" * doctest::skip()) {
    const char* const           env = std::getenv("CLOTHO_PERF_CASES");
    const std::filesystem::path root =
      env != nullptr ? std::filesystem::path(env) : std::filesystem::path(CLOTHO_TEST_DIR) / "perf/cases";

    for (const std::string_view nm : { "picorv32", "hazard3" }) {
        CAPTURE(nm);
        const std::filesystem::path dir = root / nm / "src";
        if (!std::filesystem::is_directory(dir)) {
            MESSAGE(nm, ": sources absent; run its fetch.sh");
            continue;
        }
        std::vector<std::string> owned;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            const auto x = e.path().extension();
            if ((x == ".v" || x == ".sv") && !is_testbench(e.path())) {
                owned.push_back(e.path().string());
            }
        }
        std::sort(owned.begin(), owned.end());
        const std::vector<std::string_view> paths(owned.begin(), owned.end());
        const LowerResult                   r = lower_files(paths);
        REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

        const legalize::Result before = legalize::run(*r.module, nullptr, { .keep_guards = true });

        // The 1:1 path first: no Result, nothing inferred, region tree printed as
        // statements. It has to reach the same classification as the original, or
        // "structural translation" would be a claim rather than a property.
        {
            const std::string plain = legalize::materialize(*r.module, nullptr);
            CHECK(plain.find("not renderable") == std::string::npos);
            const LowerResult reparsed = lower_text(plain);
            REQUIRE_MESSAGE(reparsed.ok, reparsed.diagnostic_text());
            const legalize::Result pr = legalize::run(*reparsed.module, nullptr);
            const Classified       pb = classify(*r.module, before, /*drop_looped=*/true);
            const Classified       pa = classify(*reparsed.module, pr, /*drop_looped=*/false);
            size_t                 pm = 0;
            size_t                 pd = 0;
            size_t                 pg = 0;
            for (const auto& [name, want] : pb.by_name) {
                const auto it = pa.by_name.find(mangled(name));
                if (it == pa.by_name.end()) {
                    ++pg;
                } else if (it->second == want) {
                    ++pm;
                } else {
                    if (pd < 4) {
                        MESSAGE("    STRUCT DIFF ", name, ": ", want.first, "->", it->second.first);
                    }
                    ++pd;
                }
            }
            MESSAGE(nm, " structural: compared ", pb.by_name.size(), " match ", pm, " differ ", pd, " absent ", pg);
            CHECK(pd == 0);
            CHECK(pg == 0);
        }

        // Verilog-2005 out of both renderers, on a whole CPU. Establishes the
        // target rather than asserting it.
        {
            const size_t le =
              errors_as_2005(legalize::materialize(*r.module, &before, { .target = legalize::Target::V2005 }));
            const size_t se =
              errors_as_2005(legalize::materialize(*r.module, nullptr, { .target = legalize::Target::V2005 }));
            MESSAGE(nm, " 2005: legalized errors ", le, ", structural errors ", se);
            CHECK(le == 0);
            CHECK(se == 0);
        }

        const std::string text = legalize::materialize(*r.module, &before);
        CHECK(text.find("unrenderable") == std::string::npos);

        const LowerResult again = lower_text(text);
        REQUIRE_MESSAGE(again.ok, again.diagnostic_text());
        const legalize::Result after = legalize::run(*again.module, nullptr);

        const Classified b = classify(*r.module, before, /*drop_looped=*/true);
        const Classified a = classify(*again.module, after, /*drop_looped=*/false);

        size_t match = 0;
        size_t diff  = 0;
        size_t gone  = 0;
        for (const auto& [name, want] : b.by_name) {
            const auto it = a.by_name.find(mangled(name));
            if (it == a.by_name.end()) {
                ++gone;
                continue;
            }
            if (it->second == want) {
                ++match;
                continue;
            }
            if (diff < 6) {
                MESSAGE("    DIFF ",
                        name,
                        ": kind ",
                        want.first,
                        "->",
                        it->second.first,
                        " arst ",
                        static_cast<int>(want.second),
                        "->",
                        static_cast<int>(it->second.second));
            }
            ++diff;
        }
        MESSAGE(nm,
                ": compared ",
                b.by_name.size(),
                " match ",
                match,
                " differ ",
                diff,
                " absent ",
                gone,
                " (excluded ",
                b.looped,
                " loop-written, ",
                b.multi,
                " multi-writer)");
        CHECK(diff == 0);
        CHECK(gone == 0);
    }
}
