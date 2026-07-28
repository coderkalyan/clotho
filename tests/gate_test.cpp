// Structural gate primitives (LRM 28.4, 28.5): frontend lowering to
// continuous-assign processes, and (in later sections) runtime behavior.

#include <doctest/doctest.h>

#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// Lower and require success, returning the module.
LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    return r;
}

// JIT-run a module and capture everything it writes to stdout.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_ok(source);

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

// -- Structural lowering checks --

TEST_CASE("two-input gate primitives lower to continuous-assign processes") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire y0, y1, y2, y3, y4, y5;
            reg a, b;
            and  g0(y0, a, b);
            nand g1(y1, a, b);
            or   g2(y2, a, b);
            nor  g3(y3, a, b);
            xor  g4(y4, a, b);
            xnor g5(y5, a, b);
        endmodule
    )");
    const ir::Module& m = *r.module;

    // Each gate becomes one self-starting comb process.
    CHECK(m.processes().size() == 6);
    for (const ir::Process& p : m.processes()) {
        CHECK(p.activation == ir::Activation::Always);
        CHECK(p.self_start);
        CHECK(!p.wait_events.empty());
    }
}

TEST_CASE("multi-output buf and not lower to one process per output") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire o0, o1, o2;
            reg a;
            buf g0(o0, o1, a);
            not g1(o2, a);
        endmodule
    )");
    const ir::Module& m = *r.module;

    // buf with 2 outputs = 2 processes; not = 1 process.
    CHECK(m.processes().size() == 3);
    for (const ir::Process& p : m.processes()) {
        CHECK(p.activation == ir::Activation::Always);
        CHECK(p.self_start);
    }
}

TEST_CASE("a complex gate input becomes an implicit continuous assign to a temp net") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire y;
            reg a, b, c;
            reg [3:0] v;
            and g(y, a & b, c, v[1] | a, b);
        endmodule
    )");
    const ir::Module& m = *r.module;

    // 2 complex inputs -> 2 temp-net processes; 1 gate process = 3 total.
    CHECK(m.processes().size() == 3);
    size_t temps = 0;
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name.find("$gate") != std::string::npos) {
            ++temps;
        }
    }
    CHECK(temps == 2);
    for (const ir::Process& p : m.processes()) {
        CHECK(p.activation == ir::Activation::Always);
        CHECK(p.self_start);
        CHECK(!p.wait_events.empty());
    }
}

TEST_CASE("gate instance arrays lower one process per element") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire [3:0] y;
            reg  [3:0] a, b;
            and g[3:0](y, a, b);
        endmodule
    )");
    CHECK(r.module->processes().size() == 4);
}

TEST_CASE("gates lower inside generate blocks and child instances") {
    const LowerResult r = lower_ok(R"(
        module sub(input a, input b, output y);
            and g(y, a, b);
        endmodule
        module m;
            wire y0, y1;
            reg a, b;
            genvar i;
            generate
                for (i = 0; i < 1; i = i + 1) begin : blk
                    or g(y0, a, b);
                end
            endgenerate
            sub u(a, b, y1);
        endmodule
    )");
    // 2 gate processes + port-copy processes for sub's ports.
    // At minimum the two gate processes must be present.
    size_t gate_procs = 0;
    for (const ir::Process& p : r.module->processes()) {
        if (p.self_start && !p.wait_events.empty()) {
            ++gate_procs;
        }
    }
    CHECK(gate_procs >= 2);
}

TEST_CASE("MOS / bidirectional switch primitives are reported as unsupported") {
    // Terminals are nets: tran-family inout terminals reject variables in slang
    // before clotho's own check would run. (bufif/notif and pullup/pulldown are
    // now supported.)
    for (const char* inst : { "nmos g(y, a, b);", "pmos g(y, a, b);", "tran g(y, a);" }) {
        const LowerResult r = lower_text("module m;\n wire y;\n wire a, b;\n " + std::string(inst) + "\nendmodule");
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(r.diagnostics.find("is not supported") != std::string::npos, r.diagnostics, " for ", inst);
    }
}

TEST_CASE("single-value gate delay is inertial (LRM 28.16)") {
    // `and #3` schedules the output for now+3; a pulse narrower than the delay is
    // rejected, and the net powers up x (its driver's initial output), not z.
    // Byte-identical to iverilog.
    const std::string out = run_capture(R"(
        module m;
            logic a, b; wire y;
            and #3 (y, a, b);
            initial begin
                a=0; b=0;
                #1 $display("y=%b", y);   // initial window: driver's x, not net z
                a=1; b=1;
                #4 $display("y=%b", y);   // t5: propagated 1
                a=0;
                #4 $display("y=%b", y);   // t9: propagated 0
                a=1; #1 a=0;              // 1-wide pulse < 3 -> rejected
                #4 $display("y=%b", y);   // still 0
                $finish;
            end
        endmodule
    )");
    CHECK(out == "y=x\ny=1\ny=0\ny=0\n");
}

TEST_CASE("gate rise/fall/turn-off delay triples are reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            wire y;
            reg a, b;
            and #(1,2) g(y, a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("rise/fall/turn-off") != std::string::npos);
}

TEST_CASE("gate delay on a bit-addressed output is reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            wire [3:0] y;
            reg a, b;
            and #2 g(y[1], a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("whole scalar output") != std::string::npos);
}

TEST_CASE("gate drive strengths lower (resolution covered in net_resolution_test)") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire y;
            reg a, b;
            and (weak0, weak1) g(y, a, b);
        endmodule
    )");
    CHECK(r.module->processes().size() == 1);

    const LowerResult ok = lower_ok(R"(
        module m;
            wire y;
            reg a, b;
            and (strong0, strong1) g(y, a, b);
        endmodule
    )");
    CHECK(ok.module->processes().size() == 1);
}

TEST_CASE("user-defined primitives are reported as unsupported") {
    const LowerResult r = lower_text(R"(
        primitive udp_and(output y, input a, input b);
            table
                1 1 : 1;
                0 ? : 0;
                ? 0 : 0;
            endtable
        endprimitive
        module m;
            wire y;
            reg a, b;
            udp_and g(y, a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("is not supported") != std::string::npos);
}

TEST_CASE("unsupported gate output lvalues are rejected") {
    // Concatenation output.
    const LowerResult r1 = lower_text(R"(
        module m;
            wire y;
            reg a, b;
            and g({y}, a, b);
        endmodule
    )");
    CHECK_FALSE(r1.ok);
    CHECK(r1.diagnostics.find("gate output") != std::string::npos);

    // 2-state output net: a gate can drive X, which the slot cannot hold.
    const LowerResult r2 = lower_text(R"(
        module m;
            bit y;
            reg a, b;
            and g(y, a, b);
        endmodule
    )");
    CHECK_FALSE(r2.ok);
    CHECK(r2.diagnostics.find("2-state") != std::string::npos);
}

// Module instance arrays (LRM 23.3.6) share InstanceArraySymbol with gate
// arrays in slang but elaborate to InstanceSymbol leaves; lower_instance_array
// dispatches each leaf kind to its own scalar lowering path.
TEST_CASE("module instance arrays lower one instance per element") {
    const LowerResult r = lower_text(R"(
        module sub(input a, output y);
            assign y = a;
        endmodule
        module m;
            wire [1:0] y;
            reg  [1:0] a;
            sub u[1:0](a, y);
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    std::set<std::string> names;
    for (const ir::SignalInfo& s : r.module->signals()) {
        names.insert(s.name);
    }
    CHECK(names.contains("u[0].a"));
    CHECK(names.contains("u[0].y"));
    CHECK(names.contains("u[1].a"));
    CHECK(names.contains("u[1].y"));
}

// -- Runtime behavior --
// All expected outputs cross-checked against iverilog (12.0).

TEST_CASE("gate truth tables match LRM tables 28-3 / 28-4, including X") {
    const std::string out = run_capture(R"(
        module top;
            reg a, b;
            wire y_and, y_nand, y_or, y_nor, y_xor, y_xnor, y_buf, y_not;
            and  (y_and, a, b);
            nand (y_nand, a, b);
            or   (y_or, a, b);
            nor  (y_nor, a, b);
            xor  (y_xor, a, b);
            xnor (y_xnor, a, b);
            buf  (y_buf, a);
            not  (y_not, a);
            initial begin
                a = 0; b = 0; #1;
                $display("00: %b %b %b %b %b %b %b %b", y_and, y_nand, y_or, y_nor, y_xor, y_xnor, y_buf, y_not);
                a = 0; b = 1; #1;
                $display("01: %b %b %b %b %b %b %b %b", y_and, y_nand, y_or, y_nor, y_xor, y_xnor, y_buf, y_not);
                a = 1; b = 0; #1;
                $display("10: %b %b %b %b %b %b %b %b", y_and, y_nand, y_or, y_nor, y_xor, y_xnor, y_buf, y_not);
                a = 1; b = 1; #1;
                $display("11: %b %b %b %b %b %b %b %b", y_and, y_nand, y_or, y_nor, y_xor, y_xnor, y_buf, y_not);
                a = 1'bx; b = 0; #1;
                $display("x0: %b %b %b %b %b %b %b %b", y_and, y_nand, y_or, y_nor, y_xor, y_xnor, y_buf, y_not);
                a = 1'bx; b = 1; #1;
                $display("x1: %b %b %b %b %b %b %b %b", y_and, y_nand, y_or, y_nor, y_xor, y_xnor, y_buf, y_not);
            end
        endmodule
    )");
    CHECK(out == "00: 0 1 0 1 0 1 0 1\n"
                 "01: 0 1 1 0 1 0 0 1\n"
                 "10: 0 1 1 0 1 0 1 0\n"
                 "11: 1 0 1 0 0 1 1 0\n"
                 "x0: 0 1 x x x x x x\n"
                 "x1: x x 1 0 x x x x\n");
}

TEST_CASE("three-state gate truth tables match LRM table 28-5, including X/Z") {
    // Data d and control c sweep 0/1/x/z. The weak L/H results the LRM lists for
    // an x/z control collapse to X (no strength model); every row matches
    // iverilog on a plain wire.
    const std::string out = run_capture(R"(
        module top;
            logic d, c;
            wire b1, b0, n1, n0;
            bufif1 g1(b1, d, c);
            bufif0 g0(b0, d, c);
            notif1 m1(n1, d, c);
            notif0 m0(n0, d, c);
            function logic bit4(int k);
                case (k) 0: bit4 = 1'b0; 1: bit4 = 1'b1; 2: bit4 = 1'bx; default: bit4 = 1'bz; endcase
            endfunction
            initial begin
                for (int di = 0; di < 4; di++) begin
                    for (int ci = 0; ci < 4; ci++) begin
                        d = bit4(di); c = bit4(ci); #1;
                        $display("%b%b %b%b%b%b", d, c, b1, b0, n1, n0);
                    end
                end
            end
        endmodule
    )");
    CHECK(out == "00 z0z1\n01 0z1z\n0x xxxx\n0z xxxx\n"
                 "10 z1z0\n11 1z0z\n1x xxxx\n1z xxxx\n"
                 "x0 zxzx\nx1 xzxz\nxx xxxx\nxz xxxx\n"
                 "z0 zxzx\nz1 xzxz\nzx xxxx\nzz xxxx\n");
}

TEST_CASE("a three-state bus resolves multiple bufif drivers") {
    // Two bufif1 drivers onto one tri net: floats to Z when both disabled, passes
    // the enabled driver, and conflicts to X (matches iverilog).
    const std::string out = run_capture(R"(
        module top;
            logic a, b, ena, enb;
            tri bus;
            bufif1 d0(bus, a, ena);
            bufif1 d1(bus, b, enb);
            initial begin
                a = 0; b = 1; ena = 0; enb = 0; #1 $display("%b", bus);
                ena = 1;                         #1 $display("%b", bus);
                ena = 0; enb = 1;                #1 $display("%b", bus);
                ena = 1; enb = 1;                #1 $display("%b", bus);
                a = 1;                           #1 $display("%b", bus);
            end
        endmodule
    )");
    CHECK(out == "z\n0\n1\nx\n1\n");
}

TEST_CASE("n-input gates fold over all inputs") {
    const std::string out = run_capture(R"(
        module top;
            reg a, b, c, d;
            wire y3, y4, x3, y1;
            and g0(y3, a, b, c);
            or  g1(y4, a, b, c, d);
            xor g2(x3, a, b, c);
            and g3(y1, a);
            initial begin
                a = 1; b = 1; c = 1; d = 0; #1;
                $display("%b %b %b %b", y3, y4, x3, y1);
                c = 0; #1;
                $display("%b %b %b %b", y3, y4, x3, y1);
            end
        endmodule
    )");
    CHECK(out == "1 1 1 1\n0 1 0 1\n");
}

TEST_CASE("multi-output buf and not drive every output") {
    const std::string out = run_capture(R"(
        module top;
            reg a;
            wire o0, o1, n0, n1;
            buf gb(o0, o1, a);
            not gn(n0, n1, a);
            initial begin
                a = 1; #1;
                $display("%b %b %b %b", o0, o1, n0, n1);
            end
        endmodule
    )");
    CHECK(out == "1 1 0 0\n");
}

TEST_CASE("constant and tie inputs fold, including an all-constant gate") {
    const std::string out = run_capture(R"(
        module top;
            reg a;
            wire y0, y1, y2, y3;
            and g0(y0, a, 1'b1);
            and g1(y1, a, 1'b0);
            and g2(y2, 1'b1, 1'b1);
            xor g3(y3, a, 1'bx);
            initial begin
                a = 1; #1;
                $display("%b %b %b %b", y0, y1, y2, y3);
            end
        endmodule
    )");
    CHECK(out == "1 0 1 x\n");
}

TEST_CASE("bit-select terminals read and drive individual vector bits") {
    const std::string out = run_capture(R"(
        module top;
            reg [3:0] a;
            wire [3:0] y;
            and g0(y[0], a[3], a[2]);
            or  g1(y[3], a[1], a[0]);
            initial begin
                a = 4'b1101; #1;
                $display("%b %b", y[0], y[3]);
            end
        endmodule
    )");
    CHECK(out == "1 1\n");
}

TEST_CASE("a complex input expression drives the gate through its implicit net") {
    const std::string out = run_capture(R"(
        module top;
            reg a, b, c;
            wire y;
            and g(y, a & b, c);
            initial begin
                a = 1; b = 1; c = 1; #1;
                $display("%b", y);
                b = 0; #1;
                $display("%b", y);
            end
        endmodule
    )");
    CHECK(out == "1\n0\n");
}

TEST_CASE("a gate-built ripple-carry adder settles through the fanout worklist") {
    const std::string out = run_capture(R"(
        module fa(input a, input b, input ci, output s, output co);
            wire ab, t1, t2;
            xor x0(ab, a, b);
            xor x1(s, ab, ci);
            and a0(t1, a, b);
            and a1(t2, ab, ci);
            or  o0(co, t1, t2);
        endmodule
        module top;
            reg  [3:0] x, y;
            wire [3:0] s;
            wire [4:0] c;
            assign c[0] = 1'b0;
            genvar i;
            generate
                for (i = 0; i < 4; i = i + 1) begin : ba
                    fa u(x[i], y[i], c[i], s[i], c[i+1]);
                end
            endgenerate
            initial begin
                x = 4'd5;  y = 4'd9;  #1 $display("%0d %b", s, c[4]);
                x = 4'd15; y = 4'd15; #1 $display("%0d %b", s, c[4]);
            end
        endmodule
    )");
    CHECK(out == "14 0\n14 1\n");
}

TEST_CASE("a gate-driven clock wakes edge-sensitive processes") {
    const std::string out = run_capture(R"(
        module top;
            reg clk;
            wire clkb;
            integer pcnt, ncnt;
            buf b0(clkb, clk);
            always @(posedge clkb) pcnt = pcnt + 1;
            always @(negedge clkb) ncnt = ncnt + 1;
            initial begin
                pcnt = 0; ncnt = 0; clk = 0; #1;
                clk = 1; #1;
                clk = 0; #1;
                clk = 1; #1;
                $display("%0d %0d", pcnt, ncnt);
            end
        endmodule
    )");
    // clkb starts x (clk uninitialized), so the x->0 at t0 is a negedge and the
    // 1->0 at t2 is the second; posedges at t1 and t3. Matches iverilog (2 2).
    CHECK(out == "2 2\n");
}

TEST_CASE("a cross-coupled nand latch converges through gate feedback") {
    const std::string out = run_capture(R"(
        module top;
            reg s_n, r_n;
            wire q, qn;
            nand n0(q, s_n, qn);
            nand n1(qn, r_n, q);
            initial begin
                s_n = 0; r_n = 1; #1 $display("set  q=%b qn=%b", q, qn);
                s_n = 1;          #1 $display("hold q=%b qn=%b", q, qn);
                r_n = 0;          #1 $display("rst  q=%b qn=%b", q, qn);
            end
        endmodule
    )");
    CHECK(out == "set  q=1 qn=0\nhold q=1 qn=0\nrst  q=0 qn=1\n");
}

TEST_CASE("an NBA commit pushes gate fanout in the same instant") {
    const std::string out = run_capture(R"(
        module top;
            reg clk, q;
            wire wq;
            not n0(wq, q);
            always @(posedge clk) q <= ~q;
            initial begin
                clk = 0; q = 0; #1;
                clk = 1; #1;
                $display("%b %b", q, wq);
            end
        endmodule
    )");
    CHECK(out == "1 0\n");
}

TEST_CASE("gates settle before a #0 resumption observes them") {
    // Region order at one instant: active (incl. gates) -> inactive (#0) ->
    // NBA. A blocking store's gate fanout settles before the #0 resumes; an
    // NBA commit lands after it.
    const std::string out = run_capture(R"(
        module top;
            reg a, b;
            wire y;
            and g(y, a, b);
            initial begin
                a = 1; b = 1;
                #0 $display("%b", y);
            end
        endmodule
    )");
    CHECK(out == "1\n");
}

TEST_CASE("gate instance arrays evaluate per sliced bit") {
    const std::string out = run_capture(R"(
        module top;
            reg  [3:0] a, b;
            wire [3:0] y;
            and g[3:0](y, a, b);
            initial begin
                a = 4'b1100; b = 4'b1010; #1;
                $display("%b", y);
            end
        endmodule
    )");
    CHECK(out == "1000\n");
}
