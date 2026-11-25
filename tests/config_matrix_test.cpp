#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ConfigMatrix.h"
#include "Lower.h"

// Config-matrix mode (tests/ConfigMatrix.h, docs/VERIFICATION.md): the hook, the
// variants, what it skips, how a difference is reported and listed. Oracle: the
// mode's own contract. A design whose output the LRM fixes runs the same under every
// gate; one with a race the LRM leaves open (4.7) may not, and the known list
// says so. Each test of the check builds its own suite and pauses the suite's hook,
// because the self-test variant differs on purpose and the whole suite would fail on it.

using namespace clotho;
namespace cm = clotho::test::config_matrix;

namespace {

// Output the LRM fixes: one process, a clock, a counter and a finish.
constexpr std::string_view kClean = R"(
module t;
  logic clk = 0;
  int n = 0;
  always #5 clk = ~clk;
  always @(posedge clk) n <= n + 1;
  initial begin
    #52;
    $display("n=%0d", n);
    $finish;
  end
endmodule
)";

LowerInputs text_inputs(std::string_view source, ir::SystemAccess access = ir::SystemAccess::Off) {
    return LowerInputs{ .entry = LowerInputs::Entry::Text, .text = source, .allow_system = access };
}

// Lower `source` and hand it to `suite` as the hook would; what the check threw,
// or empty.
std::string check(cm::Suite& suite, std::string_view source, ir::SystemAccess access = ir::SystemAccess::Off) {
    const LowerResult r = lower_text(source, nullptr, access);
    try {
        suite.check(r, text_inputs(source, access));
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

cm::Options only(std::string variant) {
    cm::Options options;
    options.variants = { std::move(variant) };
    return options;
}

uint64_t skipped(const cm::Suite& suite, const std::string& why) {
    const auto it = suite.stats().skipped.find(why);
    return it == suite.stats().skipped.end() ? 0 : it->second;
}

} // namespace

TEST_CASE("config matrix: each variant flips exactly one gate of the default configuration") {
    CHECK(cm::kVariants[0].name == "default");
    const auto gates = [](const ir::SimulationConfig& c) {
        return std::array<bool, 9>{ c.z_elision, c.o3, c.fuse, c.comb_fuse, c.thunk, c.regions, c.dedup, c.split, c.gc_stress };
    };
    const std::array<bool, 9> base = gates(ir::SimulationConfig{});
    std::set<size_t>          flipped;
    for (const cm::Variant& v : cm::kVariants) {
        CAPTURE(v.name);
        ir::SimulationConfig config;
        v.apply(config);
        const std::array<bool, 9> now = gates(config);
        size_t                    diff = 0;
        for (size_t k = 0; k < base.size(); ++k) {
            if (now[k] != base[k]) {
                ++diff;
                flipped.insert(k);
            }
        }
        CHECK(diff == (v.name == "default" ? 0 : 1));
        CHECK(cm::find_variant(v.name) == &v);
    }
    // split is not a variant yet (ConfigMatrix.h): eight gates, not nine.
    CHECK(flipped.size() == base.size() - 1);
    CHECK(cm::find_variant("selftest=differ") != nullptr);
    CHECK(cm::find_variant("fuse=true") == nullptr);
}

TEST_CASE("config matrix: the stop time counts ticks of the design's own time unit and saturates") {
    CHECK(cm::stop_time_fs(10, 1) == 10);
    CHECK(cm::stop_time_fs(10, 1'000'000) == 10'000'000);
    CHECK(cm::stop_time_fs(10, 0) == 10);
    CHECK(cm::stop_time_fs(cm::kStopTicks, 1'000'000'000'000'000) == UINT64_MAX);
}

TEST_CASE("config matrix: the known list names a category, a test case and a reason") {
    const auto entries = cm::parse_known(
      "# a comment\n"
      "\n"
      "race \"event: two always blocks\" 4.7: the second block reads what the first writes\n"
      "  bug   \"fork: a join_none child\"   thunk=false loses the wakeup  \r\n");
    REQUIRE(entries.size() == 2);
    CHECK(entries.at("event: two always blocks").category == "race");
    CHECK(entries.at("event: two always blocks").reason == "4.7: the second block reads what the first writes");
    CHECK(entries.at("event: two always blocks").line == 3);
    CHECK(entries.at("fork: a join_none child").category == "bug");
    CHECK(entries.at("fork: a join_none child").reason == "thunk=false loses the wakeup");

    const auto fails_on = [](std::string_view text, std::string_view line) {
        try {
            cm::parse_known(text, "known.txt");
        } catch (const std::runtime_error& e) {
            return std::string_view(e.what()).substr(0, line.size()) == line;
        }
        return false;
    };
    CHECK(fails_on("flaky \"t\" why\n", "known.txt:1: category 'flaky'"));
    CHECK(fails_on("race t why\n", "known.txt:1: the test case name"));
    CHECK(fails_on("# c\nrace \"t why\n", "known.txt:2: the test case name has no closing quote"));
    CHECK(fails_on("race \"t\"\n", "known.txt:1: an entry needs a reason"));
    CHECK(fails_on("race \"\" why\n", "known.txt:1: the test case name is empty"));
    CHECK(fails_on("race \"t\" a\nbug \"t\" b\n", "known.txt:2: \"t\" is already listed at line 1"));
}

TEST_CASE("config matrix: a difference is located by its stream and first line") {
    CHECK(cm::first_difference("a\nb\n", "a\nb\n").empty());
    CHECK(cm::first_difference("a\nb\n", "a\nc\n") == "differs at line 2: default `b`, variant `c`");
    CHECK(cm::first_difference("a\n", "a\nb\n") == "differs at line 2: default <end of output>, variant `b`");
    CHECK(cm::first_difference("a\nb\n", "a\n") == "differs at line 2: default `b`, variant <end of output>");
    CHECK(cm::first_difference("a\n", "a") == "differs only in its final newline");
    CHECK(cm::first_difference("a\tb\1\n", "x\n") == "differs at line 1: default `a\tb\\x01`, variant `x`");

    test::DesignRun base;
    base.status = 0;
    base.out    = "1\n2\n";
    test::DesignRun other = base;
    CHECK(cm::difference(base, other).empty());
    other.out = "1\n3\n";
    CHECK(cm::difference(base, other) == "stdout differs at line 2: default `2`, variant `3`");
    other.err = "boom\n";
    CHECK(cm::difference(base, other) ==
          "stdout differs at line 2: default `2`, variant `3`; stderr differs at line 1: default <end of output>, "
          "variant `boom`");
}

TEST_CASE("config matrix: a design that reaches outside the process is skipped, and counted by why") {
    const cm::Paused paused;
    cm::Suite        suite({}, {});
    suite.begin_test("skips");

    const auto expect_skip = [&](std::string_view source, const std::string& why,
                                 ir::SystemAccess access = ir::SystemAccess::Off) {
        CAPTURE(source);
        CHECK(check(suite, source, access).empty());
        CHECK(skipped(suite, why) == 1);
    };
    expect_skip("module t; integer fd; initial begin fd = $fopen(\"cm.txt\", \"w\"); $fclose(fd); end endmodule",
                "opens a file ($fopen)");
    expect_skip("module t; integer c; initial c = $fgetc(32'h8000_0000); endmodule",
                "reads or closes a file descriptor ($fgetc, $fscanf, ...)");
    expect_skip("module t; initial $system(\"true\"); endmodule", "runs a shell command ($system)",
                ir::SystemAccess::Const);
    expect_skip("module t; import \"DPI-C\" function int c_abs(int x); initial $display(c_abs(-3)); endmodule",
                "imports a DPI-C function");
    expect_skip("module t; initial if ($test$plusargs(\"v\")) $display(\"v\"); endmodule", "reads plusargs");
    expect_skip("module t; initial begin $dumpfile(\"cm.vcd\"); $dumpvars; end endmodule", "writes a VCD file");
    expect_skip("module t; wire w; endmodule", "no process");
    expect_skip("package p; int x; endpackage", "no top-level module");
    expect_skip("module t; this is not systemverilog", "lowering refused the design");
    CHECK(suite.stats().checked == 0);
}

#if !defined(_WIN32)

TEST_CASE("config matrix: a difference under a variant is caught, located and listed") {
    const cm::Paused paused;

    // Unlisted: the check throws, naming the variant and the first differing line.
    cm::Suite unlisted(only("selftest=differ"), {});
    unlisted.begin_test("differing design");
    const std::string what = check(unlisted, kClean);
    CHECK(contains(what, "behaves differently under 1 of 1 variants"));
    CHECK(contains(what, "selftest=differ: stdout differs at line 1: default `n=5`, variant `selftest: a line only this variant prints`"));
    CHECK(contains(what, "replay: "));
    CHECK(contains(what, "--clotho-run-variant selftest=differ text 0 0"));
    CHECK(unlisted.stats().unlisted == 1);
    CHECK(unlisted.stats().checked == 1);

    // Listed: the test passes, the difference is counted under its category, and the
    // entry stays valid for as long as the test keeps differing.
    cm::Suite listed(only("selftest=differ"),
                     cm::parse_known("race \"differing design\" 4.7: the two always blocks race on x (example)\n"));
    listed.begin_test("differing design");
    CHECK(check(listed, kClean).empty());
    CHECK(listed.stats().known.at("race") == 1);
    CHECK(listed.stats().unlisted == 0);
    listed.end_test(true);
    CHECK(listed.stale().empty());
}

TEST_CASE("config matrix: a clean design runs the same under every variant, once") {
    const cm::Paused paused;
    cm::Suite        suite({}, {});
    suite.begin_test("clean");
    CHECK(check(suite, kClean).empty());
    CHECK(suite.stats().checked == 1);
    CHECK(suite.stats().unlisted == 0);
    CHECK(suite.stats().known.empty());

    // The same text again is the same design: not run again.
    CHECK(check(suite, kClean).empty());
    CHECK(suite.stats().checked == 1);
    CHECK(suite.stats().repeated == 1);
}

TEST_CASE("config matrix: a known entry that no longer differs is out of date") {
    const cm::Paused paused;
    cm::Suite        suite(only("fuse=false"), cm::parse_known("bug \"clean\" the design never differed\n"));
    suite.begin_test("clean");
    CHECK(check(suite, kClean).empty());
    suite.end_test(true);
    CHECK(suite.stale() == std::vector<std::string>{ "clean" });

    // A test that failed for another reason says nothing about its entry.
    cm::Suite failing(only("fuse=false"), cm::parse_known("bug \"clean\" the design never differed\n"));
    failing.begin_test("clean");
    failing.end_test(false);
    CHECK(failing.stale().empty());
}

TEST_CASE("config matrix: a default run that reaches the time limit is skipped, not hung on") {
    const cm::Paused paused;
    cm::Options      options = only("fuse=false");
    options.limit            = std::chrono::milliseconds(500);
    cm::Suite suite(options, {});
    suite.begin_test("endless");

    const auto before = std::chrono::steady_clock::now();
    // The loop never waits, so the stop time is never reached.
    CHECK(check(suite, "module t; int x = 0; initial forever x = $urandom; endmodule").empty());
    CHECK(skipped(suite, "the default run reached the time limit") == 1);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(30));
}

TEST_CASE("config matrix: a replay that cannot lower the design is skipped") {
    const cm::Paused paused;
    cm::Suite        suite(only("fuse=false"), {});
    suite.begin_test("replay");

    const std::string      missing = (std::filesystem::temp_directory_path() / "clotho-config-matrix-missing.sv").string();
    const std::string_view paths[] = { missing };
    const LowerResult      r       = lower_text(kClean);
    LowerInputs            inputs{ .entry = LowerInputs::Entry::Files, .args = paths };
    suite.check(r, inputs);
    CHECK(skipped(suite, "the replay did not lower the design") == 1);
}

#endif

namespace {

// The lowering entry points call whatever check is installed, and nothing else
// clears it: an entry point that skipped it would let a design go unchecked.
struct RecordedCheck {
    std::vector<LowerInputs::Entry> entries;
    std::vector<std::string>        texts;
    std::vector<std::string>        args;

    explicit RecordedCheck(bool throws = false) {
        set_design_check([this, throws](const LowerResult& result, const LowerInputs& in) {
            entries.push_back(in.entry);
            texts.emplace_back(in.text);
            for (const std::string_view arg : in.args) {
                args.emplace_back(arg);
            }
            CHECK(result.ok);
            if (throws) {
                throw std::runtime_error("the check ran");
            }
        });
    }
    ~RecordedCheck() { set_design_check({}); }
    RecordedCheck(const RecordedCheck&)            = delete;
    RecordedCheck& operator=(const RecordedCheck&) = delete;
};

} // namespace

TEST_CASE("config matrix: lower_text, lower_files and lower_sources hand the check their inputs") {
    const cm::Paused paused;
    namespace fs = std::filesystem;
    static std::atomic<int> seq{ 0 };
    const fs::path          file = fs::temp_directory_path() / ("clotho-config-matrix-hook-" + std::to_string(seq++) + ".sv");
    {
        std::ofstream f(file);
        f << "module t; initial $display(\"hook\"); endmodule\n";
    }
    const std::string      path = file.string();
    const std::string_view one[]  = { path };

    {
        RecordedCheck recorded;
        CHECK(lower_text("module t; endmodule").ok);
        CHECK(lower_files(one).ok);
        CHECK(lower_sources(one).ok);
        // A partial design (the editor's) and the twin form of a design are not handed over.
        CHECK(lower_buffer("buffer.sv", "module t; endmodule").ok);
        REQUIRE(recorded.entries.size() == 3);
        CHECK(recorded.entries[0] == LowerInputs::Entry::Text);
        CHECK(recorded.texts[0] == "module t; endmodule");
        CHECK(recorded.entries[1] == LowerInputs::Entry::Files);
        CHECK(recorded.entries[2] == LowerInputs::Entry::Sources);
        CHECK(recorded.args == std::vector<std::string>{ path, path });
    }
    {
        // A throw from the check comes out of the entry point, so the test that
        // lowered the design fails.
        RecordedCheck recorded(/*throws=*/true);
        CHECK_THROWS_WITH_AS(lower_text("module t; endmodule"), "the check ran", std::runtime_error);
    }
    std::error_code ec;
    fs::remove(file, ec);
}
