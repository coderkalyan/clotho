#include <doctest/doctest.h>

#include <string_view>

#include "Lower.h"
#include "ir/Analysis.h"

using namespace clotho;

namespace {

// Lowered module plus its may-z result, addressable by signal name.
struct Analyzed {
    LowerResult       lowered;
    std::vector<bool> may_z;

    // May-z of the signal whose (possibly hierarchical) name is `name` or ends
    // with ".name". Fails the test if no such signal exists.
    bool z(std::string_view name) const {
        const auto signals = lowered.module->signals();
        for (size_t i = 0; i < signals.size(); ++i) {
            const std::string& n = signals[i].name;
            if (n == name || (n.size() > name.size() && n.compare(n.size() - name.size(), name.size(), name) == 0 &&
                              n[n.size() - name.size() - 1] == '.')) {
                return may_z[i];
            }
        }
        FAIL("no signal named ", name);
        return true;
    }
};

Analyzed analyze(std::string_view source) {
    Analyzed a{ .lowered = lower_text(source), .may_z = {} };
    REQUIRE_MESSAGE(a.lowered.ok, a.lowered.diagnostic_text());
    REQUIRE(a.lowered.module != nullptr);
    a.may_z = ir::analyze(*a.lowered.module).may_z;

    return a;
}

} // namespace

TEST_CASE("a fully-driven design without z literals proves every signal never-z") {
    // Testbench-shaped: no ports, every net driven. Top-level input ports would
    // be undriven nets and thus z (see the undriven-net seed test below).
    const Analyzed a = analyze(R"(
        module top;
            reg clk = 0;
            reg [7:0] d = 8'h3c;
            reg [7:0] q;
            wire [7:0] n = d ^ 8'hff;
            always #5 clk = ~clk;
            always @(posedge clk) q <= n + 1;
        endmodule
    )");
    for (const bool z : a.may_z) {
        CHECK(!z);
    }
}

TEST_CASE("an undriven 4-state net defaults to z and seeds the analysis (LRM 6.6)") {
    const Analyzed a = analyze(R"(
        module top(input [3:0] in);
            wire [3:0] w;
            wire [3:0] driven = 4'b1010;
            logic [3:0] v;
            bit  [3:0] w2;
            logic [3:0] copy;
            assign copy = w;
        endmodule
    )");
    // Undriven nets (including undriven top-level input ports) hold z.
    CHECK(a.z("w"));
    CHECK(a.z("in"));
    CHECK(a.z("copy"));
    // A driven net, an undriven variable (defaults x), and a 2-state signal
    // cannot hold z.
    CHECK(!a.z("driven"));
    CHECK(!a.z("v"));
    CHECK(!a.z("w2"));
}

// The per-bit counterpart of the seed above: the undriven bits of a partially
// driven net power on z, so the net and everything that copies it may hold z.
TEST_CASE("a partially-driven net holds z in its undriven bits and seeds the analysis (LRM 6.6)") {
    const Analyzed a = analyze(R"(
        module top;
            wire [7:0] part;
            assign part[3:0] = 4'd2;
            wire [7:0] copy = part;
            logic [7:0] var_copy;
            always @* var_copy = part;
            wire [3:0] covered;
            assign covered[1:0] = 2'd1;
            assign covered[3:2] = 2'd2;
            wire [3:0] covered_copy = covered;
        endmodule
    )");
    CHECK(a.z("part"));
    CHECK(a.z("copy"));
    CHECK(a.z("var_copy"));
    // Disjoint drivers that together cover every bit leave nothing at z.
    CHECK(!a.z("covered"));
    CHECK(!a.z("covered_copy"));
}

TEST_CASE("a z literal marks its target and transitive copies") {
    const Analyzed a = analyze(R"(
        module top;
            logic [3:0] w = 4'bz01x;
            logic [3:0] y;
            logic [3:0] clean = 4'b1010;
            assign y = w;
        endmodule
    )");
    CHECK(a.z("w"));
    CHECK(a.z("y"));
    CHECK(!a.z("clean"));
}

TEST_CASE("z rides through transparent ops") {
    const Analyzed a = analyze(R"(
        module top(input sel);
            logic [3:0] w = 4'bz01x;
            logic [3:0] m, c, r, sh, ps;
            logic [7:0] sx;
            logic       b;
            assign m  = sel ? w : 4'b0;
            assign c  = { w[1:0], 2'b00 };
            assign r  = { 2{ w[1:0] } };
            assign sh = w << 1;
            assign ps = { 2'b0, w[3:2] };
            assign sx = $signed(w);
            assign b  = w[0];
        endmodule
    )");
    CHECK(a.z("m"));
    CHECK(a.z("c"));
    CHECK(a.z("r"));
    CHECK(a.z("sh"));
    CHECK(a.z("ps"));
    CHECK(a.z("sx"));
    CHECK(a.z("b"));
}

TEST_CASE("z is absorbed by arithmetic, comparisons, reductions, and case-equality") {
    const Analyzed a = analyze(R"(
        module top;
            logic [3:0] w = 4'bz01x;
            logic [3:0] sum, andv;
            logic       eq, ceq, red, lnot;
            assign sum  = w + 1;
            assign andv = w & 4'hf;
            assign eq   = w == 4'h3;
            assign ceq  = w === 4'bz01x;
            assign red  = &w;
            assign lnot = !w;
        endmodule
    )");
    CHECK(a.z("w"));
    CHECK(!a.z("sum"));
    CHECK(!a.z("andv"));
    CHECK(!a.z("eq"));
    CHECK(!a.z("ceq"));
    CHECK(!a.z("red"));
    CHECK(!a.z("lnot"));
}

TEST_CASE("an unknown select condition or shift count does not launder z away") {
    // The z operand still rides through even when the selector itself is clean;
    // conversely a z-free datapath stays clean under a 4-state selector.
    const Analyzed a = analyze(R"(
        module top(input s);
            logic [3:0] w = 4'bzzzz;
            logic [3:0] viaz, clean;
            assign viaz  = s ? w : w >> 1;
            assign clean = s ? 4'h1 : 4'h2;
        endmodule
    )");
    CHECK(a.z("viaz"));
    CHECK(!a.z("clean"));
}

TEST_CASE("$readmem seeds a 4-state memory but not a 2-state one") {
    const Analyzed a = analyze(R"(
        module top;
            logic [7:0] mem4 [0:3];
            bit   [7:0] mem2 [0:3];
            logic [7:0] out4;
            bit   [7:0] out2;
            initial begin
                $readmemh("f.hex", mem4);
                $readmemh("f.hex", mem2);
                out4 = mem4[0];
                out2 = mem2[0];
            end
        endmodule
    )");
    CHECK(a.z("mem4"));
    CHECK(!a.z("mem2"));
    CHECK(a.z("out4"));
    CHECK(!a.z("out2"));
}

TEST_CASE("z crosses instance boundaries through port copies") {
    const Analyzed a = analyze(R"(
        module child(input [3:0] i, output [3:0] o);
            assign o = i;
        endmodule
        module top;
            logic [3:0] src = 4'bz0z0;
            logic [3:0] dst;
            child c(.i(src), .o(dst));
        endmodule
    )");
    CHECK(a.z("src"));
    CHECK(a.z("dst"));
    CHECK(a.z("i"));
    CHECK(a.z("o"));
}

TEST_CASE("function results carry may-z only when the body can produce z") {
    const Analyzed a = analyze(R"(
        module top;
            function automatic logic [3:0] zfn(input logic [3:0] x);
                return 4'bz000;
            endfunction
            function automatic logic [3:0] addfn(input logic [3:0] x);
                return x + 1;
            endfunction
            function automatic logic [3:0] passfn(input logic [3:0] x);
                return x;
            endfunction
            logic [3:0] a1, a2, a3, a4;
            initial begin
                a1 = zfn(4'h0);
                a2 = addfn(4'bzzzz);
                a3 = passfn(4'bz000);
                a4 = passfn(4'h5);
            end
        endmodule
    )");
    CHECK(a.z("a1"));
    CHECK(!a.z("a2"));
    // passfn joins over call sites: one z actual poisons the shared summary.
    CHECK(a.z("a3"));
    CHECK(a.z("a4"));
}

TEST_CASE("task output formals propagate z to the caller") {
    const Analyzed a = analyze(R"(
        module top;
            task automatic zt(output logic [3:0] o);
                o = 4'bz111;
            endtask
            task automatic ct(output logic [3:0] o);
                o = 4'b0111;
            endtask
            logic [3:0] zdst, cdst;
            initial begin
                zt(zdst);
                ct(cdst);
            end
        endmodule
    )");
    CHECK(a.z("zdst"));
    CHECK(!a.z("cdst"));
}

TEST_CASE("z flows around loop back-edges to fixpoint") {
    // dst picks up z only via the feedback signal written later in the process.
    const Analyzed a = analyze(R"(
        module top(input clk);
            logic [3:0] fb = 4'h0;
            logic [3:0] dst;
            always @(posedge clk) begin
                dst <= fb;
                fb  <= 4'bz000;
            end
        endmodule
    )");
    CHECK(a.z("fb"));
    CHECK(a.z("dst"));
}

TEST_CASE("$sscanf / $fscanf seed 4-state output targets but not 2-state ones") {
    // %h / %b / %o accept z and ? digits (LRM Table 21-7); a 2-state target
    // drops x/z to 0 (LRM 6.3.1) and stays clean. The z must also flow onward
    // through an ordinary assignment.
    const Analyzed a = analyze(R"(
        module top;
            logic [31:0] r4;
            bit   [31:0] r2;
            logic [31:0] fwd;
            int n;
            initial begin
                n = $sscanf("zz", "%h", r4);
                n = $sscanf("zz", "%h", r2);
                fwd = r4;
            end
        endmodule
    )");
    CHECK(a.z("r4"));
    CHECK(!a.z("r2"));
    CHECK(a.z("fwd"));
}

TEST_CASE("a scanf z reaching a signal through a function local is seeded") {
    // The scan target is a 4-state frame local; the z must propagate through
    // the return value into the module signal.
    const Analyzed a = analyze(R"(
        module top;
            function automatic logic [31:0] scan1();
                logic [31:0] t;
                int n;
                n = $sscanf("z", "%h", t);
                return t;
            endfunction
            logic [31:0] s;
            initial s = scan1();
        endmodule
    )");
    CHECK(a.z("s"));
}
