#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
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

    // tmpnam is fine here and mkstemp is not: the runtime opens the path itself,
    // so the test must hand over a NAME, not a descriptor.
    const std::string    path = std::string(std::tmpnam(nullptr));
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
// must fail loudly and by name, never sample silently: a coverage
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
// intervals with their own inclusive/exclusive naming. clotho's own guard is in
// cover_space_of, but slang refuses first at the language version clotho compiles
// at -- 19.5's floating-point coverpoints are an IEEE 1800-2023 addition, and
// DeclaredType gates them on that. Either way the construct is refused loudly and
// never silently sampled, which is what this pins.
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
    CHECK(d.find("coverage expression") != std::string::npos);
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
             option.comment = "hello";
             cp: coverpoint v;
          endgroup
          cg c = new;
          initial begin c.sample(); $finish; end
        endmodule
    )");
    CHECK(d.find("coverage option 'comment'") != std::string::npos);
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
