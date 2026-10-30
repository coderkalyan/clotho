#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Analysis.h"
#include "ir/Jit.h"

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

// JIT-run a module and return what it wrote to stdout.
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

TEST_CASE("a 2-state target takes the must plane alone, so a conversion clears z") {
    const Analyzed a = analyze(R"(
        module top;
            logic [3:0] w = 4'bz01x;
            bit   [3:0] b;
            bit   [3:0] arr [0:1];
            logic [3:0] widened;
            initial begin
                b       = w;
                arr[1]  = w;
                widened = b;
            end
        endmodule
    )");
    CHECK(a.z("w"));
    // LRM 6.3.1: x and z become 0, so what a 2-state signal holds has no z plane,
    // and neither has anything it is widened back to.
    CHECK(!a.z("b"));
    CHECK(!a.z("arr"));
    CHECK(!a.z("widened"));
}

TEST_CASE("a constant table's image seeds z for every signal read from it") {
    const Analyzed a = analyze(R"(
        typedef struct { int n; logic [3:0] f; } s_t;
        module top;
            localparam logic [3:0] ZT[0:1] = '{4'bzzzz, 4'h5};
            localparam logic [3:0] CT[0:1] = '{4'h1, 4'h5};
            localparam s_t         ZS[0:1] = '{'{1, 4'bx01z}, '{3, 4'hF}};
            localparam s_t         CS[0:1] = '{'{1, 4'h2}, '{3, 4'hF}};
            logic [3:0] from_zt, from_ct, from_zs, from_cs;
            int i;
            initial begin
                i       = 0;
                from_zt = ZT[i];
                from_ct = CT[i];
                from_zs = ZS[i].f;
                from_cs = CS[i].f;
            end
        endmodule
    )");
    // The table is never stored to, so the fixpoint cannot see its image: the
    // analysis reads the image itself, through a flat table and a struct table.
    CHECK(a.z("from_zt"));
    CHECK(!a.z("from_ct"));
    CHECK(a.z("from_zs"));
    CHECK(!a.z("from_cs"));
}

TEST_CASE("a 4-state member read from a container element of an unpacked struct may carry z") {
    const Analyzed a = analyze(R"(
        typedef struct { logic [3:0] f; } fs_t;
        typedef struct { int n; } ns_t;
        module top;
            fs_t fq[$];
            fs_t fd[];
            fs_t fa[int];
            ns_t nq[$];
            logic [3:0]  from_q, from_d, from_a;
            logic [31:0] from_n;
            initial begin
                from_q = fq[0].f;
                from_d = fd[0].f;
                from_a = fa[1].f;
                from_n = nq[0].n;
            end
        endmodule
    )");
    // An unpacked struct has no plane layout of its own, so the element's domain
    // is read off its leaves, not off Layout.
    CHECK(a.z("from_q"));
    CHECK(a.z("from_d"));
    CHECK(a.z("from_a"));
    CHECK(!a.z("from_n"));
}

TEST_CASE("$sscanf seeds a 4-state member of an unpacked struct") {
    const Analyzed a = analyze(R"(
        typedef struct { logic [3:0] f; bit [3:0] g; } s_t;
        module top;
            s_t         st;
            logic [3:0] fwd;
            int         n;
            initial begin
                n   = $sscanf("zzzz", "%b", st.f);
                fwd = st.f;
            end
        endmodule
    )");
    CHECK(a.z("fwd"));
}

TEST_CASE("an exported subroutine's 4-state input and inout formals are seeded: C can pass z") {
    const Analyzed a = analyze(R"(
        module top;
            logic [7:0] s4, io4, o4;
            bit   [7:0] s2;
            export "DPI-C" function set4;
            export "DPI-C" function set2;
            function void set4(input logic [7:0] v, inout logic [7:0] x, output logic [7:0] y);
                s4  = v;
                io4 = x;
                y   = 8'h01;
            endfunction
            function void set2(input bit [7:0] v);
                s2 = v;
            endfunction
            initial o4 = 8'h00;
        endmodule
    )");
    CHECK(a.z("s4"));
    CHECK(a.z("io4"));
    CHECK(!a.z("s2"));
}

// The facts above decide a slot's layout, and a wrong "never z" is a silent wrong
// value. These run the designs, so a lost z shows up as the x it decays to.
TEST_CASE("z read out of a constant table, a container element or a scanf target survives") {
    constexpr std::string_view src = R"(
        typedef struct { int n; logic [3:0] f; } t_t;
        typedef struct { logic [3:0] f; } s_t;
        module top;
            localparam logic [3:0] X[0:3] = '{4'b10x1, 4'bzzzz, 4'h5, 4'bx0z1};
            localparam t_t         P[0:1] = '{'{1, 4'bx01z}, '{3, 4'hF}};
            s_t   q[$];
            s_t   d[];
            s_t   a[int];
            s_t   st;
            logic [3:0] v, vq, vd, va, vs;
            integer i, n;
            initial begin
                for (i = 0; i < 4; i = i + 1) begin
                    v = X[i];
                    $write("%b ", v);
                end
                $write("\n");
                for (i = 0; i < 2; i = i + 1) begin
                    v = P[i].f;
                    $write("%b ", v);
                end
                $write("\n");
                q.push_back('{f: 4'bz1x0});
                d = new[1];
                d[0].f = 4'b1zx0;
                a[3].f = 4'bzz01;
                n = $sscanf("1z0x", "%b", st.f);
                vq = q[0].f;
                vd = d[0].f;
                va = a[3].f;
                vs = st.f;
                $display("%b %b %b %b", vq, vd, va, vs);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "10x1 zzzz 0101 x0z1 \n"
          "x01z 1111 \n"
          "z1x0 1zx0 zz01 1z0x\n");
}
