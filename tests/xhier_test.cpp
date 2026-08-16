#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a design's initial/always blocks to quiescence and return stdout.
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

} // namespace

// Cross-hierarchy references (LRM 23.6): a name qualified by an instance path
// reads or writes the signal in that instance. All values cross-checked against
// Icarus Verilog 13.

TEST_CASE("xhier: downward read of a child instance signal") {
    // The common testbench shape: instantiate a DUT, drive its clock, read an
    // internal counter. 3 posedges over 6 toggles.
    const std::string out = run_capture(R"(
        module sub(input logic clk);
          logic [7:0] cnt = 0;
          always @(posedge clk) cnt <= cnt + 8'd1;
        endmodule
        module top;
          logic clk = 0;
          sub u1(.clk(clk));
          initial begin repeat (6) #5 clk = ~clk; $display("cnt=%0d", u1.cnt); end
        endmodule
    )");
    CHECK(out == "cnt=3\n");
}

TEST_CASE("xhier: downward write of a child instance signal") {
    const std::string out = run_capture(R"(
        module sub; logic [7:0] cnt = 0; endmodule
        module top;
          sub u1();
          initial begin u1.cnt = 8'd99; #1; $display("cnt=%0d", u1.cnt); end
        endmodule
    )");
    CHECK(out == "cnt=99\n");
}

TEST_CASE("xhier: upward read of a parent signal from a child") {
    const std::string out = run_capture(R"(
        module sub; initial #1 $display("up=%0d", top.g); endmodule
        module top; logic [7:0] g = 8'd42; sub u1(); endmodule
    )");
    CHECK(out == "up=42\n");
}

TEST_CASE("xhier: reference resolves regardless of declaration order") {
    // The referencing process is declared *before* the instance it reaches
    // into: the two-pass lowering collects every signal before any body lowers.
    const std::string out = run_capture(R"(
        module sub; logic [7:0] cnt = 8'd7; endmodule
        module top;
          initial begin #1; $display("cnt=%0d", u1.cnt); end
          sub u1();
        endmodule
    )");
    CHECK(out == "cnt=7\n");
}

TEST_CASE("xhier: sibling reference (one leaf reads another)") {
    const std::string out = run_capture(R"(
        module a; initial #2 $display("a.g=%0d", top.b0.g); endmodule
        module b; logic [7:0] g = 8'd9; endmodule
        module top; a a0(); b b0(); endmodule
    )");
    CHECK(out == "a.g=9\n");
}

TEST_CASE("xhier: deep path, bit-select, part-select, and a deep write") {
    const std::string out = run_capture(R"(
        module leaf(input logic clk);
          logic [7:0]  q   = 8'd0;
          logic [15:0] bus = 16'hABCD;
          always @(posedge clk) q <= q + 8'd1;
        endmodule
        module mid(input logic clk); leaf lf(.clk(clk)); endmodule
        module top;
          logic clk = 0;
          mid u1(.clk(clk));
          initial begin
            repeat (6) #5 clk = ~clk;
            $display("q=%0d bit=%b part=%h", u1.lf.q, u1.lf.bus[3], u1.lf.bus[11:4]);
            u1.lf.q = 8'd50;
            #1 $display("after=%0d", u1.lf.q);
          end
        endmodule
    )");
    CHECK(out == "q=3 bit=1 part=bc\nafter=50\n");
}

TEST_CASE("xhier: event control and NBA read across the hierarchy") {
    // @(posedge <hier>) subscribes to the target signal; the NBA RHS reads a
    // hierarchical value. mon trails q by one delta (NBA).
    const std::string out = run_capture(R"(
        module leaf(input logic clk);
          logic [7:0] q = 8'd0;
          always @(posedge clk) q <= q + 8'd1;
        endmodule
        module top;
          logic clk = 0;
          leaf lf(.clk(clk));
          logic [7:0] mon = 0;
          always @(posedge lf.clk) mon <= lf.q;
          initial begin repeat (6) #5 clk = ~clk; $display("q=%0d mon=%0d", lf.q, mon); end
        endmodule
    )");
    CHECK(out == "q=3 mon=2\n");
}

TEST_CASE("xhier: reference into a generate-loop block") {
    // A signal declared directly in a loop-generate block, reached by index.
    const std::string out = run_capture(R"(
        module top;
          logic clk = 0;
          genvar i;
          generate for (i = 0; i < 3; i = i + 1) begin : g
            logic [7:0] c = 8'd0;
            always @(posedge clk) c <= c + 8'd1;
          end endgenerate
          initial begin repeat (4) #5 clk = ~clk; $display("g0=%0d g2=%0d", g[0].c, g[2].c); end
        endmodule
    )");
    CHECK(out == "g0=2 g2=2\n");
}

// inout / ref ports (LRM 23.3.3): the child port's internal net/variable shares
// storage with the parent connection (build_inout_aliases) — no copy process, so
// a value driven on either side is seen on the other.
TEST_CASE("inout port: child drives, parent (and a sibling) read") {
    const std::string out = run_capture(R"(
        module io4(inout wire [3:0] b);
          assign b = 4'b1010;
        endmodule
        module rd(inout wire [3:0] b, output logic [3:0] o);
          assign o = b;
        endmodule
        module top;
          wire [3:0] w1, w2;
          logic [3:0] o;
          io4 u(w1);          // child drives w1 through its inout
          assign w2 = 4'h5;   // parent drives w2
          rd  r(w2, o);       // child reads w2 through its inout
          initial #1 $display("%b %b", w1, o);
        endmodule
    )");
    CHECK(out == "1010 0101\n");
}

// A bidirectional bus with drivers in two separate instance scopes is the
// pre-existing cross-scope multi-driver limitation (not inout-specific), and a
// bit-select / concat inout connection needs sub-signal aliasing the whole-signal
// collapse cannot express. Both stay loud.
TEST_CASE("cross-scope inout bus driver fails loudly") {
    const LowerResult r = lower_text(R"(
        module tsdrv(inout wire bus, input drive, input val);
          assign bus = drive ? val : 1'bz;
        endmodule
        module top;
          wire bus;
          logic d1, v1, d2, v2;
          tsdrv a(bus, d1, v1);
          tsdrv b(bus, d2, v2);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("across different scopes") != std::string::npos);
}

TEST_CASE("bit-select inout connection fails loudly") {
    const LowerResult r = lower_text(R"(
        module leaf(inout wire b);
          assign b = 1'b1;
        endmodule
        module top;
          wire [3:0] bus;
          leaf u(bus[0]);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("non-whole-signal connection") != std::string::npos);
}
