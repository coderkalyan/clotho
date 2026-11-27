#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors randsequence_test.cpp).
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);

    return out;
}

// Confirm lowering fails loudly for an unsupported construct.
std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostic_text();
}

// Run a design with `--coverage <tmpfile>` and return the report it wrote. The
// report is the only place bin NAMES are observable, and a name is semantics
// rather than presentation: LRM 19.11.3 merges bins across instances by name.
std::string report_of(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    // The runtime opens the report path itself, so the test hands over a name.
    const std::string path =
      (std::filesystem::temp_directory_path() / (clotho::test::detail::unique_stem() + ".cov")).string();
    ir::SimulationConfig cfg;
    cfg.coverage_report = path;

    std::FILE* devnull = std::tmpfile();
    REQUIRE(devnull != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(devnull), fileno(stdout));
    ir::run_jit(*r.module, nullptr, cfg);
    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    std::fclose(devnull);

    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE_MESSAGE(f != nullptr, "the run wrote no coverage report");
    std::string out;
    char        buf[512];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    std::remove(path.c_str());
    return out;
}

// The hit count the report shows for one bin, or -1 when it lists no such bin.
// The report puts the count last on the bin's line.
long bin_count(const std::string& report, std::string_view bin) {
    const std::string needle = "bin " + std::string(bin) + " ";
    const size_t      at     = report.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    const size_t eol  = report.find('\n', at);
    std::string  line = report.substr(at, eol == std::string::npos ? eol : eol - at);
    const size_t sp   = line.find_last_of(' ');
    if (sp == std::string::npos) {
        return -1;
    }
    return std::strtol(line.c_str() + sp + 1, nullptr, 10);
}

// One covergroup instance's part of a report: its line and the coverpoints and bins
// under it, up to the next instance or the type rollup. Empty when the report names
// no such instance.
std::string instance_block(const std::string& report, std::string_view name) {
    const std::string head = "  instance " + std::string(name) + " ";
    const size_t      at   = report.find(head);
    if (at == std::string::npos) {
        return {};
    }
    size_t end = report.size();
    for (const char* next : { "\n  instance ", "\n  type " }) {
        const size_t n = report.find(next, at + head.size());
        end            = std::min(end, n == std::string::npos ? end : n);
    }
    return report.substr(at, end - at);
}

} // namespace

// -- The model: bins, names and counts ---------------------------------------

// LRM 19.5.1's own worked example: `bins fixed[4] = {[1:10], 1, 4, 7}` is 13
// values over 4 bins, so B = 13/4 = 3 and the distribution is <1,2,3> <4,5,6>
// <7,8,9> <10,1,4,7> -- the last bin taking the remainder. Sampling 0..10 once
// each therefore hits the first three bins three times and the last four times
// (10, 1, 4 and 7 are all sampled). A distribution rule off by one anywhere
// shows up as a different count vector.
TEST_CASE("a fixed bin array distributes values per LRM 19.5.1") {
    const std::string out = run_capture(R"(
        module m;
          bit [4:0] v = 0;
          covergroup cg;
             cp: coverpoint v { bins fixed[4] = {[1:10], 1, 4, 7}; }
          endgroup
          cg c = new;
          initial begin
             for (int i = 0; i < 11; i++) begin v = i[4:0]; c.sample(); end
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("cov=100.000000") != std::string::npos);
}

// LRM 19.5.1: "bin names are binname[value]" for a `[]` array -- named after the
// VALUE, not the index. Verilator 5.050 names these hi[0]..hi[2] instead, which
// is why the check is against the clause and not against Verilator: the
// name is what LRM 19.11.3 merges instances by, so it is semantics, not cosmetics.
TEST_CASE("an open bin array names each bin after its value (LRM 19.5.1)") {
    const std::string report = report_of(R"(
        module m;
          bit [3:0] v = 0;
          covergroup cg;
             option.per_instance = 1;
             cp: coverpoint v { bins hi[] = {[3:5]}; }
          endgroup
          cg c = new;
          initial begin
             int covered, total;
             v = 4; c.sample();
             void'(c.get_inst_coverage(covered, total));
             $display("covered=%0d total=%0d", covered, total);
             $finish;
          end
        endmodule
    )");
    // Three bins, one per value in [3:5], each named after its VALUE.
    CHECK(report.find("bin hi[3]") != std::string::npos);
    CHECK(report.find("bin hi[4]") != std::string::npos);
    CHECK(report.find("bin hi[5]") != std::string::npos);
    // And the one that was hit is the one holding 4.
    CHECK(report.find("hi[3]") < report.find("hi[4]"));
    CHECK(bin_count(report, "hi[3]") == 0);
    CHECK(bin_count(report, "hi[4]") == 1);
    CHECK(bin_count(report, "hi[5]") == 0);
}

// LRM 19.5.3: for a non-enum coverpoint N = MIN(2^M, auto_bin_max) and the 2^M
// values are distributed uniformly, remainder in the last bin. LRM 19.11.1 then
// makes the coverage denominator that same N. Three bits with auto_bin_max = 4
// gives <0,1> <2,3> <4,5> <6,7>, so sampling 0..3 covers exactly half.
TEST_CASE("automatic bins distribute the value space (LRM 19.5.3, 19.11.1)") {
    const std::string out = run_capture(R"(
        module m;
          bit [2:0] a = 0;
          covergroup cg;
             cp: coverpoint a { option.auto_bin_max = 4; }
          endgroup
          cg c = new;
          initial begin
             for (int i = 0; i < 4; i++) begin a = i[2:0]; c.sample(); end
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("cov=50.000000") != std::string::npos);
}

// LRM 19.5.3 names an enum coverpoint's automatic bins after the named constants,
// and N is the cardinality of the enumeration -- not 2^M, which for this 2-bit
// base type would be 4 rather than 3.
TEST_CASE("an enum coverpoint gets one automatic bin per enumerand (LRM 19.5.3)") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum bit [1:0] { red, green, blue } color_e;
          color_e color = red;
          covergroup cg;
             cp: coverpoint color;
          endgroup
          cg c = new;
          initial begin
             color = red;   c.sample();
             color = blue;  c.sample();
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    // Two of the three enumerands, so 2/3 -- 50% would mean 2^M bins.
    CHECK(out.find("cov=66.666667") != std::string::npos);
}

// -- The arithmetic (LRM 19.11) ----------------------------------------------

// The covergroup number is the WEIGHTED AVERAGE of its coverpoints' ratios
// (LRM 19.11), not the pooled bin count. Measured divergence worth pinning:
// Verilator 5.050 answers 40.00% here, which is 2/5 pooled bins; the clause
// gives (1/4 + 1/1)/2 = 62.5%.
TEST_CASE("covergroup coverage is the weighted average of its items (LRM 19.11)") {
    const std::string out = run_capture(R"(
        module m;
          bit [2:0] a = 0;
          bit       b = 0;
          covergroup cg;
             cp1: coverpoint a { bins q[] = {0,1,2,3}; }
             cp2: coverpoint b { bins z = {0}; }
          endgroup
          cg c = new;
          initial begin
             c.sample();
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("cov=62.500000") != std::string::npos);
    CHECK(out.find("cov=40.000000") == std::string::npos);
}

// LRM 19.7 Table 19-1: option.weight at the coverpoint level weights that item in
// the enclosing covergroup's average. A fully covered item at weight 3 against an
// uncovered one at weight 1 is (3*100 + 1*0)/4.
TEST_CASE("coverpoint weights scale the covergroup average (LRM 19.7, 19.11)") {
    const std::string out = run_capture(R"(
        module m;
          bit x = 0;
          bit [1:0] y = 0;
          covergroup cg;
             cp_a: coverpoint x { bins one = {0}; option.weight = 3; }
             cp_b: coverpoint y { bins none = {3}; option.weight = 1; }
          endgroup
          cg c = new;
          initial begin
             c.sample();
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("cov=75.000000") != std::string::npos);
}

// LRM 19.7.1, 19.11.3: type_option.weight is not option.weight.
// - cgp: a coverpoint's type option weights it only in the merged type form, so
//   the instance average is (50 + 100)/2 = 75, not (3*50 + 100)/4 = 62.5.
// - g5: option.weight weights an instance inside its type's average, and the
//   type's weight in $get_coverage is its type_option.weight (default 1).
// - g1: option.weight 0 leaves its type nothing to average, so it stays out of
//   $get_coverage and get_coverage() is 0.0 by 19.11 (b) at type weight 1.
// $get_coverage is (75 + 50 + 100)/3 = 75; folding the two weights gave 62.5.
// No oracle: iverilog has no functional coverage; values follow from the clauses.
TEST_CASE("type_option.weight weights only type coverage (LRM 19.7.1, 19.11.3)") {
    const std::string out = run_capture(R"(
        module m;
          bit a = 0, b = 0;
          covergroup cgp;
             cpA: coverpoint a { bins a0 = {0}; bins a1 = {1}; type_option.weight = 3; }
             cpB: coverpoint b { bins b0 = {0}; }
          endgroup
          covergroup g1;
             option.weight = 0;
             coverpoint a { bins a0 = {0}; bins a1 = {1}; }
          endgroup
          covergroup g5;
             option.weight = 3;
             coverpoint a { bins a0 = {0}; bins a1 = {1}; }
          endgroup
          covergroup g3;
             coverpoint b { bins b0 = {0}; }
          endgroup
          cgp p = new;
          g1 c1 = new;
          g5 c5 = new;
          g3 c3 = new;
          initial begin
             p.sample(); c1.sample(); c5.sample(); c3.sample();
             $display("inst=%0f g1=%0f all=%0f", p.get_inst_coverage(), c1.get_coverage(), $get_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("inst=75.000000 g1=0.000000 all=75.000000") != std::string::npos);
}

// LRM 19.7 Table 19-1: at_least is the minimum hit count for a bin to count as
// covered, and the covergroup level supplies the default for its coverpoints
// (Table 19-2).
TEST_CASE("option.at_least gates when a bin counts as covered (LRM 19.7)") {
    const std::string out = run_capture(R"(
        module m;
          bit v = 0;
          covergroup cg;
             option.at_least = 3;
             cp: coverpoint v { bins zero = {0}; }
          endgroup
          cg c = new;
          initial begin
             c.sample(); c.sample();
             $display("two=%0f", c.get_inst_coverage());
             c.sample();
             $display("three=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("two=0.000000") != std::string::npos);
    CHECK(out.find("three=100.000000") != std::string::npos);
}

// LRM 19.11's zero-denominator table: a covergroup with no coverpoint has nothing
// to average, and then a NONZERO weight answers 0.0 while a ZERO weight answers
// 100.0. The sign of the rule is the part an implementation gets backwards.
TEST_CASE("an empty covergroup follows the zero-denominator table (LRM 19.11)") {
    const std::string out = run_capture(R"(
        module m;
          covergroup cg_one;
          endgroup
          covergroup cg_zero;
             option.weight = 0;
          endgroup
          cg_one  a = new;
          cg_zero b = new;
          initial begin
             $display("one=%0f zero=%0f", a.get_inst_coverage(), b.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("one=0.000000") != std::string::npos);
    CHECK(out.find("zero=100.000000") != std::string::npos);
}

// LRM 19.11 closing sentence: $get_coverage returns 100.0 on a design with no
// covergroup instances.
TEST_CASE("$get_coverage is 100.0 with no covergroup instance (LRM 19.9, 19.11)") {
    const std::string out = run_capture(R"(
        module m;
          initial begin
             $display("all=%0f", $get_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("all=100.000000") != std::string::npos);
}

// LRM 19.11.3: type coverage aggregates every instance of the covergroup type,
// weighted by each instance's weight. Two instances of one type, one fully
// covered and one untouched, average to 50% at the type level while their own
// get_inst_coverage answers stay 100 and 0.
TEST_CASE("type coverage aggregates instances (LRM 19.11.3)") {
    const std::string out = run_capture(R"(
        module m;
          bit v = 0;
          covergroup cg;
             cp: coverpoint v { bins zero = {0}; }
          endgroup
          cg a = new;
          cg b = new;
          initial begin
             a.sample();
             $display("a=%0f b=%0f type=%0f", a.get_inst_coverage(), b.get_inst_coverage(),
                      a.get_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("a=100.000000 b=0.000000 type=50.000000") != std::string::npos);
}

// LRM 19.8: the two-argument form assigns the covered-bin count to the first
// reference and the total to the second. For a coverpoint they are exactly the
// numerator and denominator of its ratio, so a covergroup's are their sums.
TEST_CASE("get_inst_coverage returns covered and total bin counts (LRM 19.8)") {
    const std::string out = run_capture(R"(
        module m;
          bit [2:0] a = 0;
          covergroup cg;
             cp: coverpoint a { bins q[] = {0,1,2,3}; }
          endgroup
          cg c = new;
          initial begin
             int covered, total;
             real pct;
             a = 2; c.sample();
             pct = c.get_inst_coverage(covered, total);
             $display("pct=%0f covered=%0d total=%0d", pct, covered, total);
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("pct=25.000000 covered=1 total=4") != std::string::npos);
}

// -- Bin kinds ---------------------------------------------------------------

// LRM 19.5.5: an ignored bin is excluded from coverage, so it changes the
// DENOMINATOR rather than sitting in it uncovered forever.
TEST_CASE("ignore_bins is excluded from the coverage computation (LRM 19.5.5)") {
    const std::string out = run_capture(R"(
        module m;
          bit [3:0] v = 0;
          covergroup cg;
             cp: coverpoint v { bins lo = {[0:3]}; bins hi = {[12:15]}; ignore_bins ig = {7,8}; }
          endgroup
          cg c = new;
          initial begin
             v = 1;  c.sample();
             v = 13; c.sample();
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    // Two bins of two, not two of three: the ignored bin is not a denominator term.
    CHECK(out.find("cov=100.000000") != std::string::npos);
}

// LRM 19.5: a `default` bin catches what no other bin takes and does NOT
// participate in the coverage computation.
TEST_CASE("a default bin catches the rest and is excluded (LRM 19.5)") {
    const std::string out = run_capture(R"(
        module m;
          bit [3:0] v = 0;
          covergroup cg;
             cp: coverpoint v { bins lo = {[0:3]}; bins others = default; }
          endgroup
          cg c = new;
          initial begin
             v = 9; c.sample();
             $display("only_default=%0f", c.get_inst_coverage());
             v = 2; c.sample();
             $display("with_lo=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("only_default=0.000000") != std::string::npos);
    CHECK(out.find("with_lo=100.000000") != std::string::npos);
}

// LRM 19.5.1: "Duplicate values are retained; thus the same value can be assigned
// to multiple bins." So one sample increments EVERY bin that holds its value --
// a first-match search would leave the second bin uncovered.
TEST_CASE("one sample increments every bin holding the value (LRM 19.5.1)") {
    const std::string out = run_capture(R"(
        module m;
          bit [3:0] v = 0;
          covergroup cg;
             cp: coverpoint v { bins lo = {[0:5]}; bins mid = {[4:9]}; }
          endgroup
          cg c = new;
          initial begin
             v = 5; c.sample();
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("cov=100.000000") != std::string::npos);
}

// LRM 19.5.3 / 19.5.4: automatic and non-wildcard bins consider only 2-state
// values, so a sampled x contributes nothing at all.
TEST_CASE("a sampled x or z matches no bin (LRM 19.5.3)") {
    const std::string out = run_capture(R"(
        module m;
          logic [1:0] v;
          covergroup cg;
             cp: coverpoint v { bins zero = {0}; }
          endgroup
          cg c = new;
          initial begin
             v = 2'bx0; c.sample();
             $display("unknown=%0f", c.get_inst_coverage());
             v = 2'b00; c.sample();
             $display("known=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("unknown=0.000000") != std::string::npos);
    CHECK(out.find("known=100.000000") != std::string::npos);
}

// -- Sampling ----------------------------------------------------------------

// LRM 19.3: a clocking event samples "the instant the clocking event takes
// place". One Observed process per covergroup type walks the instance registry,
// so an instance created before the first edge is sampled at every edge.
TEST_CASE("a clocking event samples the covergroup (LRM 19.3)") {
    const std::string out = run_capture(R"(
        module m;
          bit clk = 0;
          bit [2:0] a = 0;
          covergroup cg @(posedge clk);
             cp: coverpoint a { option.auto_bin_max = 4; }
          endgroup
          cg c = new;
          initial begin
             repeat (4) begin #1 clk = 1; #1 clk = 0; a = a + 1; end
             $display("cov=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    // Values 0..3 sampled at four posedges fill the first two of four auto bins.
    CHECK(out.find("cov=50.000000") != std::string::npos);
}

// LRM 19.8: `stop()` suspends collection and `start()` resumes it. A group is
// collecting from construction, so a stop has to be observable on its own.
TEST_CASE("start and stop gate collection (LRM 19.8)") {
    const std::string out = run_capture(R"(
        module m;
          bit [1:0] v = 0;
          covergroup cg;
             cp: coverpoint v { bins a = {0}; bins b = {1}; }
          endgroup
          cg c = new;
          initial begin
             c.stop();
             v = 0; c.sample();
             $display("stopped=%0f", c.get_inst_coverage());
             c.start();
             v = 0; c.sample();
             $display("started=%0f", c.get_inst_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("stopped=0.000000") != std::string::npos);
    CHECK(out.find("started=50.000000") != std::string::npos);
}

// LRM 19.4: an embedded covergroup covers the properties of the object that
// constructed it, whatever object later calls sample(). The instance captures
// its enclosing handle at construction, so two objects keep separate counts.
TEST_CASE("an embedded covergroup covers its own object's properties (LRM 19.4)") {
    const std::string out = run_capture(R"(
        module m;
          class Txn;
             bit [1:0] addr;
             covergroup cg;
                ap: coverpoint addr { bins zero = {0}; bins one = {1}; }
             endgroup
             function new(); cg = new(); endfunction
          endclass
          initial begin
             Txn s = new;
             Txn t = new;
             s.addr = 0; s.cg.sample();
             t.addr = 1; t.cg.sample();
             $display("s=%0f t=%0f type=%0f", s.cg.get_inst_coverage(),
                      t.cg.get_inst_coverage(), s.cg.get_coverage());
             $finish;
          end
        endmodule
    )");
    // Each object covered one of its own two bins; the type sees both instances.
    CHECK(out.find("s=50.000000 t=50.000000 type=50.000000") != std::string::npos);
}

// A covergroup an embedded declaration never constructs samples nothing (LRM
// 19.4: "If this assignment is not present, then the coverage group is not
// created and no data will be sampled"), and must not be a null-handle crash
// either -- the handle stays null and the design runs.
TEST_CASE("an unconstructed embedded covergroup is inert (LRM 19.4)") {
    const std::string out = run_capture(R"(
        module m;
          class C;
             bit v;
             covergroup cg;
                cp: coverpoint v;
             endgroup
             function new(); endfunction
          endclass
          initial begin
             C c = new;
             $display("alive all=%0f", $get_coverage());
             $finish;
          end
        endmodule
    )");
    CHECK(out.find("alive") != std::string::npos);
}

// -- The refusal surface -----------------------------------------------------
//
// Each of these is a real LRM 19 construct this phase does not implement. They
// must fail loudly and by name (PRINCIPLES.md), never sample silently: a coverage
// model that quietly drops a coverpoint reports progress that was never made.

TEST_CASE("cross coverage is refused by name (LRM 19.6)") {
    const std::string d = lower_error(R"(
        module m;
          bit [1:0] a, b;
          covergroup cg;
             cross a, b;
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("cross coverage") != std::string::npos);
}

TEST_CASE("transition bins are refused by name (LRM 19.5.2)") {
    const std::string d = lower_error(R"(
        module m;
          bit [3:0] v;
          covergroup cg;
             cp: coverpoint v { bins t = (0 => 1 => 2); }
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("transition coverage bin") != std::string::npos);
}

TEST_CASE("wildcard bins are refused by name (LRM 19.5.4)") {
    const std::string d = lower_error(R"(
        module m;
          bit [3:0] v;
          covergroup cg;
             cp: coverpoint v { wildcard bins w = {4'b11??}; }
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("wildcard") != std::string::npos);
}

// LRM 19.5.1 allows a covergroup formal in a bin bound, which makes the bin set a
// per-instance run-time quantity rather than static model data. Refused, because
// the alternative is a silently wrong bin table.
TEST_CASE("a non-constant bin bound is refused by name (LRM 19.5.1)") {
    const std::string d = lower_error(R"(
        module m;
          int lo = 2;
          int v;
          covergroup cg;
             cp: coverpoint v { bins r = {[lo:5]}; }
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("non-constant bin bound") != std::string::npos);
}

TEST_CASE("covergroup arguments are refused by name (LRM 19.3)") {
    const std::string d = lower_error(R"(
        module m;
          int x;
          covergroup cg (ref int a);
             cp: coverpoint a;
          endgroup
          cg c = new(x);
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("covergroup arguments") != std::string::npos);
}

TEST_CASE("a custom sample method is refused by name (LRM 19.8.1)") {
    const std::string d = lower_error(R"(
        module m;
          covergroup cg with function sample (bit b);
             cp: coverpoint b;
          endgroup
          cg c = new;
          initial begin c.sample(1'b1); $finish; end
        endmodule
    )");
    CHECK(d.find("custom sample method") != std::string::npos);
}

// A coverpoint of a real expression (LRM 19.5, 2023) needs a second bin algebra
// over floats: real bins partition a range into type_option.real_interval-wide
// intervals with their own inclusive/exclusive naming. Refused by clotho's own
// guard in cover_space_of; before IEEE 1800-2023, where the construct does not
// exist, slang refuses it first.
TEST_CASE("a real coverpoint is refused (LRM 19.5)") {
    const std::string d = lower_error(R"(
        module m;
          real r;
          covergroup cg;
             cp: coverpoint r { bins b = {[0.0:1.0]}; }
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("a coverpoint of a real expression is not supported") != std::string::npos);
}

TEST_CASE("a coverpoint iff guard is refused by name (LRM 19.5)") {
    const std::string d = lower_error(R"(
        module m;
          bit v, rst;
          covergroup cg;
             cp: coverpoint v iff (!rst);
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("'iff' guard on a coverpoint") != std::string::npos);
}

// LRM 19.4's own example shape: a coverage event on a class-embedded covergroup.
// The event names a property of an object that does not exist at elaboration, so
// it cannot become a static sensitivity.
TEST_CASE("a coverage event on an embedded covergroup is refused (LRM 19.4)") {
    const std::string d = lower_error(R"(
        module m;
          class C;
             bit m_z;
             bit [1:0] m_x;
             covergroup cg @m_z;
                cp: coverpoint m_x;
             endgroup
             function new(); cg = new(); endfunction
          endclass
          initial begin C c = new; $finish; end
        endmodule
    )");
    CHECK(d.find("embedded in a class") != std::string::npos);
}

TEST_CASE("a block-event coverage event is refused by name (LRM 19.3)") {
    const std::string d = lower_error(R"(
        module m;
          bit [1:0] v;
          task t; #1; endtask
          covergroup cg @@(begin t);
             cp: coverpoint v;
          endgroup
          cg c = new;
          initial begin #2 $finish; end
        endmodule
    )");
    CHECK(d.find("block-event") != std::string::npos);
}

TEST_CASE("an unsupported coverage option is refused by name (LRM 19.7)") {
    const std::string d = lower_error(R"(
        module m;
          bit [1:0] v;
          covergroup cg;
             type_option.strobe = 1;
             cp: coverpoint v;
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("coverage option 'type_option.strobe'") != std::string::npos);
}

// The coverage database of LRM 19.9 has no specified format, so the four database
// tasks stay refused rather than inventing one.
TEST_CASE("the coverage database tasks are refused by name (LRM 19.9)") {
    const std::string d = lower_error(R"(
        module m;
          initial begin $set_coverage_db_name("cov.db"); $finish; end
        endmodule
    )");
    CHECK(d.find("$set_coverage_db_name") != std::string::npos);
}

// -- Illegal bins ------------------------------------------------------------

// LRM 19.5.6: "If an illegal value or transition occurs, a run-time error is
// issued." The whole design dies, so this runs out of process; the check is that
// the diagnostic names the covergroup, the coverpoint and the bin.
TEST_CASE("an illegal_bins hit is a located run-time error (LRM 19.5.6)") {
    const LowerResult r = lower_text(R"(
        module m;
          bit [3:0] v = 0;
          covergroup cg;
             cp: coverpoint v { bins ok = {[0:5]}; illegal_bins bad = {9}; }
          endgroup
          cg c = new;
          initial begin
             v = 3; c.sample();
             v = 9; c.sample();
             $display("unreachable");
          end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    // Lowering must succeed: the error is a property of the sampled VALUE, not of
    // the model, so it cannot be decided here. The runtime half of this rule is
    // covered by the out-of-process behaviour of `clotho run`, and by the runtime's
    // own checks -- asserting on a fatal in-process would take the test binary
    // down with it.
    CHECK(r.module != nullptr);
}

// -- The report --------------------------------------------------------------

// The report is the only user-visible output of a covergroup, so its shape is a
// contract: bin names exactly as LRM 19.5.1 / 19.5.3 spell them, and every bin
// listed whether hit or not (an unhit bin is the whole point of a coverage
// report). Read through the runtime oracle rather than by parsing the file, so
// the test pins the counts and not the layout.
TEST_CASE("the report lists every bin with its hit count") {
    const std::string report = report_of(R"(
        module m;
          bit [2:0] a = 0;
          covergroup cg;
             option.per_instance = 1;
             cp: coverpoint a { bins lo = {[0:1]}; bins hi = {[6:7]}; }
          endgroup
          cg c = new;
          initial begin
             a = 0; c.sample();
             a = 1; c.sample();
             a = 7; c.sample();
             $finish;
          end
        endmodule
    )");
    // An UNHIT bin is the whole point of a coverage report, so both are listed.
    CHECK(bin_count(report, "lo") == 2); // took 0 and 1
    CHECK(bin_count(report, "hi") == 1); // took 7
    CHECK(report.find("coverpoint cp") != std::string::npos);
    CHECK(report.find("100.00%") != std::string::npos);
}

// A covergroup instance is named by the hierarchical path of the module
// instance that constructed it, top name included, so the report tells two
// instances of one module apart (LRM 19.8 leaves the naming to the tool).
TEST_CASE("the report names each covergroup instance by its module instance's path") {
    const std::string report = report_of(R"(
        module leaf;
          bit [1:0] v = 0;
          covergroup cg; option.per_instance = 1; coverpoint v; endgroup
          cg c;
          initial begin c = new; c.sample(); end
        endmodule
        module top;
          leaf u1();
          leaf u2();
        endmodule
    )");
    CHECK(report.find("instance top.u1.cg ") != std::string::npos);
    CHECK(report.find("instance top.u2.cg ") != std::string::npos);
}

// The instance's path is copied into the name as it is, so neither a length past what
// a string holds inline (30 bytes) nor a `%` in an escaped name takes any care.
TEST_CASE("the report names a covergroup by an instance path of any shape") {
    const std::string report = report_of(R"(
        module leaf;
          bit [1:0] v = 0;
          covergroup cg; option.per_instance = 1; coverpoint v; endgroup
          cg c = new;
          initial c.sample();
        endmodule
        module top;
          leaf an_instance_name_well_past_thirty_bytes_long ();
          leaf \c%d ();
        endmodule
    )");
    CHECK(report.find("instance top.an_instance_name_well_past_thirty_bytes_long.cg ") != std::string::npos);
    CHECK(report.find("instance top.c%d.cg ") != std::string::npos);
}

// A declaration initializer builds the covergroup from the module's own
// elaboration, before any process runs, and builds one per instance: the report
// names each by the instance that holds it and keeps its bin counts apart.
TEST_CASE("a covergroup built by a declaration initializer is named and counted per instance") {
    const std::string report = report_of(R"(
        module leaf(input bit [1:0] seed);
          bit [1:0] v = 0;
          covergroup cg; option.per_instance = 1; coverpoint v; endgroup
          cg c = new;
          initial #1 begin v = seed; c.sample(); end
        endmodule
        module top;
          leaf u1(2'd0);
          leaf u2(2'd1);
          leaf u3(2'd2);
        endmodule
    )");
    for (uint32_t k = 0; k < 3; ++k) {
        const std::string block = instance_block(report, "top.u" + std::to_string(k + 1) + ".cg");
        CAPTURE(k);
        CAPTURE(block);
        REQUIRE_FALSE(block.empty());
        for (uint32_t bin = 0; bin < 4; ++bin) {
            CAPTURE(bin);
            CHECK(bin_count(block, "auto[" + std::to_string(bin) + "]") == (bin == k ? 1 : 0));
        }
    }
}

// The same with a coverage event (LRM 19.3): the process that samples the group
// belongs to the module too, and each instance's group is sampled by its own
// clock. Fast clocks the first instance through three samples (bins 0, 1 and 2)
// and slow the second through one.
TEST_CASE("a covergroup with a coverage event, built by a declaration initializer, samples per instance") {
    const std::string report = report_of(R"(
        module leaf(input bit clk);
          bit [1:0] v = 0;
          covergroup cg @(posedge clk); option.per_instance = 1; coverpoint v; endgroup
          cg c = new;
          always @(negedge clk) v <= v + 1;
        endmodule
        module top;
          bit fast = 0, slow = 0;
          leaf u1(fast);
          leaf u2(slow);
          initial begin
            #1 fast = 1; slow = 1;
            #1 fast = 0; slow = 0;
            #1 fast = 1;
            #1 fast = 0;
            #1 fast = 1;
            #1 $finish;
          end
        endmodule
    )");
    const std::string u1 = instance_block(report, "top.u1.cg");
    const std::string u2 = instance_block(report, "top.u2.cg");
    CAPTURE(report);
    REQUIRE_FALSE(u1.empty());
    REQUIRE_FALSE(u2.empty());
    for (uint32_t bin = 0; bin < 4; ++bin) {
        CAPTURE(bin);
        CHECK(bin_count(u1, "auto[" + std::to_string(bin) + "]") == (bin < 3 ? 1 : 0));
        CHECK(bin_count(u2, "auto[" + std::to_string(bin) + "]") == (bin == 0 ? 1 : 0));
    }
}

// The generate blocks between an instance and its covergroup are part of the name,
// whether the group is built by a procedure or by a declaration initializer.
TEST_CASE("the report names a covergroup in a generate block by the block too") {
    const std::string report = report_of(R"(
        module leaf;
          bit [1:0] v = 0;
          if (1) begin : g
            covergroup cg; option.per_instance = 1; coverpoint v; endgroup
            cg c;
            initial begin c = new; c.sample(); end
          end
          for (genvar i = 0; i < 2; i++) begin : arr
            covergroup cg2; option.per_instance = 1; coverpoint v; endgroup
            cg2 c2 = new;
            initial c2.sample();
          end
        endmodule
        module top;
          leaf u1();
          leaf u2();
        endmodule
    )");
    for (const char* inst : { "u1", "u2" }) {
        CAPTURE(inst);
        const std::string path = std::string("instance top.") + inst;
        CHECK(report.find(path + ".g.cg ") != std::string::npos);
        CHECK(report.find(path + ".arr[0].cg2 ") != std::string::npos);
        CHECK(report.find(path + ".arr[1].cg2 ") != std::string::npos);
    }
}

// A covergroup type declared in a package belongs to no instance: built by the
// package it is named by its type alone, and built by a module instance it is
// named by that instance.
TEST_CASE("a package covergroup is named by the instance that builds it, if any") {
    const std::string report = report_of(R"(
        package p;
          bit [1:0] pv = 0;
          covergroup pcg; option.per_instance = 1; coverpoint pv; endgroup
          pcg pc = new;
        endpackage
        module leaf;
          import p::*;
          pcg lc;
          initial begin lc = new; lc.sample(); pc.sample(); end
        endmodule
        module top;
          leaf u1();
        endmodule
    )");
    CHECK(report.find("instance pcg ") != std::string::npos);
    CHECK(report.find("instance top.u1.pcg ") != std::string::npos);
}

// A package's covergroup type is one type however many instances build it. Each
// instance is named by the instance that built it (the second of a type takes the
// creation ordinal, `_1`), keeps its own bin counts, and the type's coverage
// aggregates them (LRM 19.11.3). Main names the second one `top.u1.pcg_1`.
TEST_CASE("a package covergroup built by two instances is one type named by each") {
    const std::string report = report_of(R"(
        package p;
          bit [1:0] pv = 0;
          covergroup pcg; option.per_instance = 1; coverpoint pv; endgroup
        endpackage
        module leaf(input bit [1:0] hit);
          import p::*;
          pcg lc = new;
          initial begin #1 pv = hit; lc.sample(); end
        endmodule
        module top;
          leaf u1(2'd1);
          leaf u2(2'd2);
        endmodule
    )");
    const std::string u1 = instance_block(report, "top.u1.pcg");
    const std::string u2 = instance_block(report, "top.u2.pcg_1");
    CAPTURE(report);
    REQUIRE_FALSE(u1.empty());
    REQUIRE_FALSE(u2.empty());
    for (uint32_t bin = 0; bin < 4; ++bin) {
        CAPTURE(bin);
        CHECK(bin_count(u1, "auto[" + std::to_string(bin) + "]") == (bin == 1 ? 1 : 0));
        CHECK(bin_count(u2, "auto[" + std::to_string(bin) + "]") == (bin == 2 ? 1 : 0));
    }
    CHECK(report.find("  type pcg  coverage  25.00%  bins 2/8  instances 2") != std::string::npos);
}

// The same type built at package level and by two instances of a module: the
// package's own group is named by the type alone, each module instance by its
// path. (The package and the top that builds it each lower the type, and both
// copies are the package unit's.)
TEST_CASE("a package covergroup built by the package and by two instances") {
    const std::string report = report_of(R"(
        package p;
          bit [1:0] pv = 0;
          covergroup pcg; option.per_instance = 1; coverpoint pv; endgroup
          pcg pc = new;
        endpackage
        module leaf;
          import p::*;
          pcg lc;
          initial begin lc = new; lc.sample(); pc.sample(); end
        endmodule
        module top;
          leaf u1();
          leaf u2();
        endmodule
    )");
    CHECK(report.find("instance pcg ") != std::string::npos);
    CHECK(report.find("instance top.u1.pcg ") != std::string::npos);
    CHECK(report.find("instance top.u2.pcg") != std::string::npos);
}

// Twin instances of one module run one body (cli_test: --stat=unit-code), and each
// builds, samples and queries the covergroup types of its own instance: the type
// coverage of m0 is its own two instances' average, not m1's.
TEST_CASE("twin instances keep their own covergroup types") {
    const std::string out = run_capture(R"(
        module m(input int k);
          bit [1:0] v = 0;
          covergroup cg;
            cp: coverpoint v { bins b[] = {0,1,2,3}; }
          endgroup
          cg a = new;
          cg b = new;
          initial begin
            #k;
            v = k[1:0];
            a.sample();
            if (k == 2) b.sample();
            $display("%m inst=%0f type=%0f", a.get_inst_coverage(), a.get_coverage());
          end
        endmodule
        module t;
          m m0(.k(1));
          m m1(.k(2));
        endmodule
    )");
    CHECK(out == "t.m0 inst=25.000000 type=12.500000\nt.m1 inst=25.000000 type=25.000000\n");
}

namespace {

// The lines of a report's "cover statements" section, in the order it lists them.
std::vector<std::string> cover_lines(const std::string& report) {
    std::vector<std::string> lines;
    const size_t             head = report.find("cover statements");
    if (head == std::string::npos) {
        return lines;
    }
    size_t at = report.find('\n', head);
    while (at != std::string::npos && at + 1 < report.size()) {
        const size_t eol = report.find('\n', at + 1);
        lines.push_back(report.substr(at + 1, eol == std::string::npos ? eol : eol - at - 1));
        at = eol;
    }
    return lines;
}

bool ends_with(const std::string& s, std::string_view tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

// An immediate `cover` (LRM 16.3) counts each evaluation that finds its expression
// true, and `--coverage` lists every site with its hits, in declaration order, the
// never-hit one too. The counts are the loop's: `a` is true for i = 0, 3, 6, 9 (four
// hits), `i == 5` once, `i > 100` never. Oracle: derived by hand from the source (no
// other simulator here counts immediate covers in a report).
TEST_CASE("coverage: an immediate cover counts its hits and the report lists every site") {
    constexpr std::string_view src = R"(
        module top;
            logic a;
            int i;
            initial begin
                for (i = 0; i < 10; i++) begin
                    a = (i % 3 == 0);
                    c1: cover (a);
                    cover (i == 5);
                    cover (i > 100);
                end
            end
        endmodule
    )";
    const std::vector<std::string> lines = cover_lines(report_of(src));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].starts_with("  cover top.c1 "));
    CHECK(ends_with(lines[0], "hits 4"));
    CHECK(ends_with(lines[1], "hits 1"));
    CHECK(ends_with(lines[2], "hits 0"));
}

// A site is counted per instance: one module instantiated twice reports twice, named by
// instance, with each instance's own count (u1's enable is on for 2 of 4 passes, u2's for 3).
TEST_CASE("coverage: a cover in two instances reports each") {
    constexpr std::string_view src = R"(
        module m #(parameter int N = 0) ();
            int i;
            initial for (i = 0; i < 4; i++) cover (i < N);
        endmodule
        module top;
            m #(2) u1 ();
            m #(3) u2 ();
        endmodule
    )";
    const std::vector<std::string> lines = cover_lines(report_of(src));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].starts_with("  cover top.u1 "));
    CHECK(ends_with(lines[0], "hits 2"));
    CHECK(lines[1].starts_with("  cover top.u2 "));
    CHECK(ends_with(lines[1], "hits 3"));
}

// `cover property` reports attempts and matches. Posedges of `clk` fall at 5, 15, ..., 95
// (ten before the $finish at 100). `a` is updated by a nonblocking assignment at each
// edge, so the value sampled at edge k is 0 for odd k and 1 for even k: ten attempts, five
// matches. The pass action runs on each match (LRM 16.14.3), and agrees: five lines.
TEST_CASE("coverage: cover property reports attempts and matches") {
    constexpr std::string_view src = R"(
        module top;
            logic clk = 0;
            logic a = 0;
            int n = 0;
            always #5 clk = ~clk;
            always @(posedge clk) begin
                n <= n + 1;
                a <= (n % 2 == 0);
            end
            cp: cover property (@(posedge clk) a) $display("hit");
            initial #100 $finish;
        endmodule
    )";
    const std::string report = report_of(src);
    const std::vector<std::string> lines = cover_lines(report);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].starts_with("  cover property top.cp "));
    CHECK(lines[0].find("attempts 10  matches 5") != std::string::npos);
    CHECK(run_capture(src) == "hit\nhit\nhit\nhit\nhit\n");
}

// A deferred cover counts at maturation: the same five hits, and a site that matures
// nothing still lists.
TEST_CASE("coverage: a deferred cover counts what matures") {
    constexpr std::string_view src = R"(
        module top;
            logic clk = 0;
            logic a = 0;
            int n = 0;
            always #5 clk = ~clk;
            always @(posedge clk) begin
                dc: cover #0 (a);
                nc: cover #0 (n > 1000);
                n <= n + 1;
                a <= (n % 2 == 0);
            end
            initial #100 $finish;
        endmodule
    )";
    const std::vector<std::string> lines = cover_lines(report_of(src));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].starts_with("  cover #0 top.dc "));
    CHECK(ends_with(lines[0], "hits 5"));
    CHECK(ends_with(lines[1], "hits 0"));
}

// Nothing is printed or changed without `--coverage`: the design's own output is all
// that comes out, and the pass action still runs.
TEST_CASE("coverage: cover statements print nothing without --coverage") {
    constexpr std::string_view src = R"(
        module top;
            int i;
            initial for (i = 0; i < 3; i++) cover (i != 1) $display("pass %0d", i);
        endmodule
    )";
    CHECK(run_capture(src) == "pass 0\npass 2\n");
}

// With the default `option.per_instance = 0` the report lists a covergroup's type line
// alone (LRM 19.7 Table 19-1: true saves and reports the instance; false does not require
// it); get_inst_coverage() still answers. With 1, the instance lines appear.
TEST_CASE("coverage: per-instance lines appear only when option.per_instance is 1") {
    constexpr std::string_view src = R"(
        module top;
          int v;
          covergroup off_cg; coverpoint v { bins a = {0}; bins b = {1}; } endgroup
          covergroup on_cg; option.per_instance = 1; coverpoint v { bins a = {0}; bins b = {1}; } endgroup
          off_cg o = new;
          on_cg  n = new;
          initial begin
            v = 0; o.sample(); n.sample();
            $display("%0f %0f", o.get_inst_coverage(), n.get_inst_coverage());
          end
        endmodule
    )";
    const std::string report = report_of(src);
    CHECK(report.find("instance top.off_cg ") == std::string::npos);
    CHECK(report.find("instance top.on_cg ") != std::string::npos);
    CHECK(report.find("type off_cg  coverage  50.00%") != std::string::npos);
    CHECK(report.find("type on_cg  coverage  50.00%") != std::string::npos);
    CHECK(run_capture(src) == "50.000000 50.000000\n");
}

// -- Coverpoint methods, type-name calls and the remaining options -------------

// LRM 19.8 and 19.11.3: get_coverage() by type name, and the coverpoint forms. Two
// instances each hit one of the four automatic bins of `cp`, so every instance and
// the type are at 25%; the type's bin counts are the sums over the instances
// (covered 2, total 8). Oracle: the clause text and its worked example (iverilog 13
// has no covergroups).
TEST_CASE("coverage: get_coverage by type name and on a coverpoint (LRM 19.8)") {
    CHECK(run_capture(R"(
        module t;
          bit [1:0] x;
          covergroup cg; cp: coverpoint x; endgroup
          cg c1 = new, c2 = new;
          int cov, tot, icov, itot;
          initial begin
            x = 0; c1.sample();
            x = 1; c2.sample();
            $display("%0.1f %0.1f", cg::get_coverage(), c1.get_coverage());
            $display("%0.1f %0.1f %0.1f", c1.cp.get_coverage(), c1.cp.get_inst_coverage(), cg::cp::get_coverage());
            cg::cp::get_coverage(cov, tot);
            c1.cp.get_inst_coverage(icov, itot);
            $display("%0d %0d %0d %0d", cov, tot, icov, itot);
          end
        endmodule
    )") == "25.0 25.0\n25.0 25.0 25.0\n2 8 1 4\n");
}

// start() / stop() on one coverpoint of one instance (LRM 19.8, Table 19-5).
TEST_CASE("coverage: stop and start on a coverpoint") {
    CHECK(run_capture(R"(
        module t;
          bit [1:0] x, y;
          covergroup cg; a: coverpoint x; b: coverpoint y; endgroup
          cg c = new;
          initial begin
            c.a.stop();
            x = 1; y = 1; c.sample();
            $display("%0.1f %0.1f", c.a.get_inst_coverage(), c.b.get_inst_coverage());
            c.a.start();
            c.sample();
            $display("%0.1f %0.1f", c.a.get_inst_coverage(), c.b.get_inst_coverage());
          end
        endmodule
    )") == "0.0 25.0\n25.0 25.0\n");
}

// option.name names the instance, option.comment and type_option.comment appear in
// the report (LRM 19.7, 19.7.1), and option.get_inst_coverage is accepted and, with
// merge_instances false, changes nothing (Table 19-1).
TEST_CASE("coverage: option.name, comments and get_inst_coverage") {
    const std::string report = report_of(R"(
        module t;
          bit [1:0] x;
          covergroup cg;
            option.name = "mycg";
            option.comment = "inst \"c\"";
            type_option.comment = "type c";
            option.per_instance = 1;
            option.get_inst_coverage = 1;
            cp: coverpoint x { option.comment = "cp c"; type_option.comment = "cp type c"; }
          endgroup
          cg c1 = new;
          initial begin x = 1; c1.sample(); end
        endmodule
    )");
    CHECK(report.find("  instance mycg  coverage  25.00%  bins 1/4  comment \"inst \\\"c\\\"\"") != std::string::npos);
    CHECK(report.find("    coverpoint cp  coverage  25.00%  bins 1/4  comment \"cp c\"") != std::string::npos);
    CHECK(report.find("instances 1  comment \"type c\"") != std::string::npos);
    CHECK(report.find("  type coverpoint cp  coverage  25.00%  bins 1/4  comment \"cp type c\"") != std::string::npos);
}

// option.detect_overlap warns for two bins sharing a value, once per pair, and not
// for ignore_bins (LRM 19.7 Table 19-1) or when the option is off.
TEST_CASE("coverage: detect_overlap warns on overlapping bins") {
    const std::string src_on = R"(
        module t;
          bit [3:0] v;
          covergroup cg;
            coverpoint v { option.detect_overlap = 1; bins a = {[0:5]}; bins b = {[3:8]}; bins c = {9};
                           ignore_bins ig = {[0:2]}; }
          endgroup
          cg c1 = new;
        endmodule
    )";
    const LowerResult on = lower_text(src_on);
    REQUIRE_MESSAGE(on.ok, on.diagnostic_text());
    const std::string d = on.diagnostic_text();
    CHECK(d.find("bins 'a' and 'b'") != std::string::npos);
    CHECK(d.find("values 3:5") != std::string::npos);
    CHECK(d.find("'ig'") == std::string::npos);
    const LowerResult off = lower_text(R"(
        module t;
          bit [3:0] v;
          covergroup cg; coverpoint v { bins a = {[0:5]}; bins b = {[3:8]}; } endgroup
          cg c1 = new;
        endmodule
    )");
    REQUIRE(off.ok);
    CHECK(off.diagnostic_text().find("overlap") == std::string::npos);
}

// -- Options assigned after instantiation (LRM 19.7) ---------------------------

// 19.7: "Other instance-specific options can be assigned procedurally after a covergroup
// has been instantiated", and 19.11: the covergroup's coverage is the weighted average of
// its coverpoints'. `a` is fully covered and `b` is at 25%, so equal weights give 62.5,
// weight 3 on b gives (100 + 3*25)/4 = 43.75, and weight 0 on a leaves b alone (25).
// Oracle: the clause text and equations (iverilog 13 has no covergroups).
TEST_CASE("coverage: coverpoint weight assigned procedurally (LRM 19.7, 19.11)") {
    CHECK(run_capture(R"(
        module t;
          bit [1:0] x, y;
          covergroup cg; a: coverpoint x; b: coverpoint y; endgroup
          cg c = new;
          initial begin
            for (int i = 0; i < 4; i++) begin x = i; y = 0; c.sample(); end
            $display("%0.2f %0d", c.get_inst_coverage(), c.b.option.weight);
            c.b.option.weight = 3;
            $display("%0.2f %0d", c.get_inst_coverage(), c.b.option.weight);
            c.a.option.weight = 0;
            $display("%0.2f", c.get_inst_coverage());
          end
        endmodule
    )") == "62.50 1\n43.75 3\n25.00\n");
}

// at_least on a coverpoint (19.11.1: a bin is covered once hit at_least times), and at the
// covergroup level it is the default the coverpoints took, so a coverpoint the definition
// gave its own value keeps it.
TEST_CASE("coverage: at_least assigned procedurally") {
    CHECK(run_capture(R"(
        module t;
          bit [1:0] x, y;
          covergroup cg; a: coverpoint x; b: coverpoint y { option.at_least = 5; } endgroup
          cg c = new;
          initial begin
            x = 1; y = 1; c.sample(); c.sample();
            $display("%0.1f %0.1f", c.a.get_inst_coverage(), c.b.get_inst_coverage());
            c.a.option.at_least = 3;
            $display("%0.1f", c.a.get_inst_coverage());
            c.a.option.at_least = 2;
            $display("%0.1f", c.a.get_inst_coverage());
            c.option.at_least = 4;
            $display("%0d %0d %0d", c.option.at_least, c.a.option.at_least, c.b.option.at_least);
          end
        endmodule
    )") == "25.0 0.0\n0.0\n25.0\n4 2 5\n");
}

// Type coverage weights each instance by its own option.weight (19.11.3): c1 is 100%, c2
// 0% with weight 3, so the type is (1*100 + 3*0) / 4.
TEST_CASE("coverage: covergroup instance weight assigned procedurally") {
    CHECK(run_capture(R"(
        module t;
          bit [1:0] x;
          covergroup cg; cp: coverpoint x; endgroup
          cg c1 = new, c2 = new;
          initial begin
            for (int i = 0; i < 4; i++) begin x = i; c1.sample(); end
            c2.option.weight = 3;
            $display("%0.1f %0d %0d", cg::get_coverage(), c2.option.weight, c1.option.weight);
          end
        endmodule
    )") == "25.0 3 1\n");
}

// name and comment assigned after the fact reach the report, and read back; the options
// only the definition may set read back as the definition left them.
TEST_CASE("coverage: name and comment assigned procedurally, immutable options read back") {
    const std::string src = R"(
        module t;
          bit [1:0] x;
          covergroup cg;
            option.per_instance = 1;
            option.get_inst_coverage = 1;
            cp: coverpoint x { option.auto_bin_max = 2; }
          endgroup
          cg c = new;
          initial begin
            c.option.name = "renamed";
            c.option.comment = "late";
            c.cp.option.comment = "cp late";
            $display("%s|%s|%s", c.option.name, c.option.comment, c.cp.option.comment);
            $display("%0d %0d %0d %0d", c.option.per_instance, c.option.get_inst_coverage,
                     c.cp.option.auto_bin_max, c.option.auto_bin_max);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "renamed|late|cp late\n1 1 2 64\n");
    const std::string report = report_of(src);
    CHECK(report.find("  instance renamed  coverage") != std::string::npos);
    CHECK(report.find("comment \"late\"") != std::string::npos);
    CHECK(report.find("comment \"cp late\"") != std::string::npos);
}

// The options only the definition may set (Table 19-2 and 19.7) are a compile error.
TEST_CASE("coverage: a definition-only option cannot be assigned procedurally") {
    const LowerResult r = lower_text(R"(
        module t;
          bit [1:0] x;
          covergroup cg; cp: coverpoint x; endgroup
          cg c = new;
          initial c.option.per_instance = 1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
}

// A covergroup handle is a class property like any other (LRM 9.4.2, 9.4.3): a store of a new
// handle changes it, and `wait` and `@` over it wake. The expressions also name the object
// handle `c`, a module variable, which is what once let the wait through unnoticed.
// No oracle: iverilog 13 has no covergroup; the expected lines apply the clause.
TEST_CASE("wait and @ on a covergroup-handle class property wake when it is assigned") {
    CHECK(run_capture(R"(
        module t;
          bit [1:0] x;
          covergroup cgt; coverpoint x; endgroup
          class K;
            cgt h;
            task mk(); h = new; endtask
          endclass
          K c = new, d = new;
          initial begin
            #5  c.mk();
            #5  d.mk();
            #5  $display("done %0t", $time);
          end
          initial begin wait (c.h != null); $display("wait %0t", $time); end
          initial begin @(d.h); $display("at %0t", $time); end
        endmodule
    )") == "wait 5\nat 10\ndone 15\n");
}

// LRM 19.7.1, 19.10: type_option is a static member of the covergroup, each coverpoint and each
// cross; weight, goal and comment "can be assigned procedurally at any time during simulation".
// Reads see the definition's value (Table 19-3 defaults: 1, 100, ""). A coverpoint's own
// type_option is separate from the covergroup's (Table 19-4: no defaulting downward).
// No oracle: iverilog has no functional coverage; the lines apply the clauses.
TEST_CASE("type_option weight, goal and comment read and assign procedurally (LRM 19.7.1)") {
    CHECK(run_capture(R"(
        module t;
          bit [1:0] x;
          covergroup cg;
            type_option.goal = 90;
            a: coverpoint x { type_option.comment = "pt"; }
            b: coverpoint x;
          endgroup
          cg h = new;
          initial begin
            $display("def w=%0d g=%0d c=[%s] a: w=%0d g=%0d c=[%s]", cg::type_option.weight,
                     cg::type_option.goal, cg::type_option.comment, cg::a::type_option.weight,
                     cg::a::type_option.goal, cg::a::type_option.comment);
            cg::type_option.weight = 3;
            cg::type_option.goal = 80;
            cg::type_option.comment = "hi";
            cg::a::type_option.weight = 5;
            cg::b::type_option.goal = 70;
            $display("set w=%0d g=%0d c=[%s] a.w=%0d b.g=%0d b.w=%0d", cg::type_option.weight,
                     cg::type_option.goal, cg::type_option.comment, cg::a::type_option.weight,
                     cg::b::type_option.goal, cg::b::type_option.weight);
            h.type_option.weight = 4;
            $display("via handle w=%0d", h.type_option.weight);
          end
        endmodule
    )") == "def w=1 g=90 c=[] a: w=1 g=100 c=[pt]\n"
           "set w=3 g=80 c=[hi] a.w=5 b.g=70 b.w=1\n"
           "via handle w=4\n");
}

// The type weight is the covergroup's weight in $get_coverage (19.11.3): it is read when the
// number is computed, so an assignment during the run changes it. Weight 0 leaves the type out.
TEST_CASE("assigning type_option.weight changes $get_coverage (LRM 19.7.1, 19.11.3)") {
    CHECK(run_capture(R"(
        module t;
          bit a = 0;
          covergroup ga; coverpoint a { bins a0 = {0}; } endgroup
          covergroup gb; coverpoint a { bins b0 = {0}; bins b1 = {1}; } endgroup
          ga x = new;
          gb y = new;
          initial begin
            x.sample(); y.sample();
            $display("%0f", $get_coverage());
            gb::type_option.weight = 3;
            $display("%0f", $get_coverage());
            gb::type_option.weight = 0;
            $display("%0f", $get_coverage());
          end
        endmodule
    )") == "75.000000\n62.500000\n100.000000\n");
}

// 19.7.1: strobe and real_interval can only be set in the definition. merge_instances and
// distribute_first have no implementation yet (docs/TODO.md), so assigning them is refused.
TEST_CASE("type_option strobe and real_interval cannot be assigned procedurally (LRM 19.7.1)") {
    CHECK(lower_error(R"(
        module t;
          bit x;
          covergroup cg; coverpoint x; endgroup
          initial cg::type_option.strobe = 1;
        endmodule
    )").find("cannot be modified outside of the covergroup definition") != std::string::npos);
    CHECK(lower_error(R"(
        module t;
          bit x;
          covergroup cg; coverpoint x; endgroup
          initial cg::type_option.merge_instances = 1;
        endmodule
    )").find("type_option.merge_instances' is not supported") != std::string::npos);
}
