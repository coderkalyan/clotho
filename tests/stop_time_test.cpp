// `--stop-time` (SimulationConfig::stop_time_fs): the run ends once every event at
// or before the stop time has run, exactly as a $finish at that instant would.
//
// In-process: the option itself is parsed by the driver, but what it means lives
// in the runtime, and the config field is all the runtime sees. Each case runs the
// design under a stop time and reads back what the run printed and how it ended.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

constexpr uint64_t kNs = 1'000'000; // femtoseconds in a nanosecond

// What one run wrote to each stream, and the status it ended with.
struct Run {
    std::string out;
    std::string err;
    int         status = -1;
};

std::string drain(std::FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string s;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    return s;
}

Run run(std::string_view source, std::optional<uint64_t> stop_fs) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* out_tmp = std::tmpfile();
    std::FILE* err_tmp = std::tmpfile();
    REQUIRE(out_tmp != nullptr);
    REQUIRE(err_tmp != nullptr);

    ir::SimulationConfig config;
    config.stop_time_fs = stop_fs;

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(out_tmp), fileno(stdout));
    dup2(fileno(err_tmp), fileno(stderr));

    Run result;
    result.status = ir::run_jit(*r.module, nullptr, config);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    result.out = drain(out_tmp);
    result.err = drain(err_tmp);
    std::fclose(out_tmp);
    std::fclose(err_tmp);
    return result;
}

// Three events ten nanoseconds apart, and a final block that says when the run
// ended. Nothing in it calls $finish: only the stop time ends it early.
constexpr std::string_view kThreeEvents = "`timescale 1ns/1ns\n"
                                          "module t;\n"
                                          "  initial begin\n"
                                          "    #10 $display(\"a\");\n"
                                          "    #10 $display(\"b\");\n"
                                          "    #10 $display(\"c\");\n"
                                          "  end\n"
                                          "  final $display(\"final at %0d\", $time);\n"
                                          "endmodule\n";

} // namespace

TEST_CASE("stop time: events at the stop time run, the next one after it does not") {
    const Run r = run(kThreeEvents, 20 * kNs);
    CHECK(r.out == "a\nb\nfinal at 20\n");
    CHECK(r.err == "clotho: stopped at --stop-time 20 ns\n");
    CHECK(r.status == 0);
}

TEST_CASE("stop time: a stop time between two events ends the run at the stop time") {
    const Run r = run(kThreeEvents, 15 * kNs);
    // The step at 20 ns never runs, and the finals read the time the run ended at,
    // not the time of the last event before it.
    CHECK(r.out == "a\nfinal at 15\n");
    CHECK(r.err == "clotho: stopped at --stop-time 15 ns\n");
    CHECK(r.status == 0);
}

TEST_CASE("stop time: a stop time before every event runs only what happens at time zero") {
    const Run r = run("`timescale 1ns/1ns\n"
                      "module t;\n"
                      "  initial $display(\"zero\");\n"
                      "  initial #1 $display(\"one\");\n"
                      "  final $display(\"final at %0d\", $time);\n"
                      "endmodule\n",
                      0);
    CHECK(r.out == "zero\nfinal at 0\n");
    CHECK(r.err == "clotho: stopped at --stop-time 0 ns\n");
}

TEST_CASE("stop time: a stop time finer than the time precision rounds up to the next tick") {
    // 1.5 ns on a 1 ns grid: stopping at 1 ns would drop the event at 2 ns that
    // the requested time reaches past, so the run goes on to 2 ns.
    const Run r = run("`timescale 1ns/1ns\n"
                      "module t;\n"
                      "  initial begin\n"
                      "    #1 $display(\"one\");\n"
                      "    #1 $display(\"two\");\n"
                      "    #1 $display(\"three\");\n"
                      "  end\n"
                      "endmodule\n",
                      kNs + kNs / 2);
    CHECK(r.out == "one\ntwo\n");
    CHECK(r.err == "clotho: stopped at --stop-time 2 ns\n");
}

TEST_CASE("stop time: rounding is to the design's precision, not to its time unit") {
    // The grid is 1 ps here. 1 ns + 1 fs is just past tick 1000, so the stop
    // tick is 1001 ps: the event at exactly 1001 ps runs, the one at 1002 does not.
    const Run r = run("`timescale 1ns/1ps\n"
                      "module t;\n"
                      "  initial begin\n"
                      "    #1     $display(\"x\");\n"
                      "    #0.001 $display(\"y\");\n"
                      "    #0.001 $display(\"z\");\n"
                      "  end\n"
                      "endmodule\n",
                      kNs + 1);
    CHECK(r.out == "x\ny\n");
    CHECK(r.err == "clotho: stopped at --stop-time 1001 ps\n");
}

TEST_CASE("stop time: a design that ends first is not stopped") {
    const char* const sources[] = {
        // Nothing left to run before the stop time.
        "`timescale 1ns/1ns\n"
        "module t;\n"
        "  initial #5 $display(\"done\");\n"
        "endmodule\n",
        // $finish before the stop time.
        "`timescale 1ns/1ns\n"
        "module t;\n"
        "  initial begin #5 $display(\"done\"); $finish; end\n"
        "endmodule\n",
    };
    for (const char* const source : sources) {
        CAPTURE(source);
        const Run r = run(source, 100 * kNs);
        CHECK(r.out == "done\n");
        CHECK(r.err.find("--stop-time") == std::string::npos);
        CHECK(r.status == 0);
    }
}

TEST_CASE("stop time: the final blocks run, like after a $finish") {
    const Run r = run("`timescale 1ns/1ns\n"
                      "module t;\n"
                      "  int n = 0;\n"
                      "  always #5 n++;\n"
                      "  final $display(\"n=%0d at %0d\", n, $time);\n"
                      "endmodule\n",
                      50 * kNs);
    CHECK(r.out == "n=10 at 50\n");
    CHECK(r.err == "clotho: stopped at --stop-time 50 ns\n");
}

TEST_CASE("stop time: a $fatal before the stop time still ends the run with its status") {
    const Run r = run("`timescale 1ns/1ns\n"
                      "module t;\n"
                      "  initial #5 $fatal(0, \"boom\");\n"
                      "endmodule\n",
                      100 * kNs);
    CHECK(r.err.find("--stop-time") == std::string::npos);
    CHECK(r.status == 1);
}

TEST_CASE("stop time: a stop time belongs to the run it was given to") {
    // The runtime's state is process-wide and clotho_test runs thousands of
    // simulations in one process: a stop time that outlived its run would cut the
    // next design short.
    CHECK(run(kThreeEvents, 10 * kNs).out == "a\nfinal at 10\n");
    CHECK(run(kThreeEvents, std::nullopt).out == "a\nb\nc\nfinal at 30\n");
}

TEST_CASE("stop time: a run that is cut off leaves a dump that ends at the stop time") {
    const std::string path = (std::filesystem::temp_directory_path() / (test::detail::unique_stem() + ".vcd"))
                               .generic_string();
    const Run          r   = run("`timescale 1ns/1ns\n"
                                 "module t;\n"
                                 "  logic clk = 0;\n"
                                 "  always #5 clk = ~clk;\n"
                                 "  initial begin $dumpfile(\"" + path + "\"); $dumpvars(0, t); end\n"
                                 "endmodule\n",
                                 50 * kNs);
    CHECK(r.err == "clotho: stopped at --stop-time 50 ns\n");

    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    std::remove(path.c_str());
    const std::string vcd = ss.str();

    // The clock toggles at every step, so the dump's last marker is the last
    // step that ran, which is the stop time, and no step past it was started.
    CHECK(vcd.find("\n#50\n") != std::string::npos);
    CHECK(vcd.find("\n#55\n") == std::string::npos);
    CHECK(vcd.ends_with("\n"));
}
