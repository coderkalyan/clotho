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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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

TEST_CASE("bidirectional pass switch primitives are reported as unsupported") {
    // Terminals are nets: tran-family inout terminals reject variables in slang
    // before clotho's own check would run. (bufif/notif, pullup/pulldown and the
    // MOS switches nmos/pmos/rnmos/rpmos/cmos/rcmos are now supported; only the
    // bidirectional family remains. slang classifies rtran/rtranif0/rtranif1 as
    // Fixed rather than BiDiSwitch, so all six have to be named explicitly.)
    for (const char* inst : { "tran g(y, a);",
                              "rtran g(y, a);",
                              "tranif0 g(y, a, b);",
                              "tranif1 g(y, a, b);",
                              "rtranif0 g(y, a, b);",
                              "rtranif1 g(y, a, b);" }) {
        const LowerResult r = lower_text("module m;\n wire y;\n wire a, b;\n " + std::string(inst) + "\nendmodule");
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(r.diagnostic_text().find("is not supported") != std::string::npos, r.diagnostic_text(), " for ", inst);
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

// LRM 9.4.1: a delay that evaluates to an unknown value "shall be interpreted as
// zero delay". 28.16 does not repeat it for a gate delay, which takes the same
// delay_value (see scaled_delay), so any x or z bit zeroes it: `buf #D` with
// `D = 4'b1x01` waited 9 and now drives its output in the same instant (the NBA
// region of t=10, seen at t=11). Byte-identical to iverilog 13.
TEST_CASE("gate delay with an x or z bit is a zero delay") {
    const std::string out = run_capture(R"(
        `timescale 1ns/1ns
        module m;
            localparam logic [3:0] D = 4'b1x01;
            logic a; wire y;
            buf #D g (y, a);
            initial begin
                a = 0;
                #10 a = 1;
                #1 $display("t=%0t y=%b", $time, y);
                #30 $finish;
            end
        endmodule
    )");
    CHECK(out == "t=11 y=1\n");
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
    CHECK(r.diagnostic_text().find("rise/fall/turn-off") != std::string::npos);
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
    CHECK(r.diagnostic_text().find("whole scalar output") != std::string::npos);
}

// LRM 28.16 puts a gate delay on the DRIVER, ahead of net resolution: `buf #5`
// beside a plain continuous assign on the same net delays only the gate's own
// contribution, and the resolver re-folds when the gate's slot lands. The
// undelayed driver therefore still conflicts at its own instant (t=13 x), and
// the gate keeps inertial pulse rejection (the 2-wide pulse at t=14 is swallowed).
// Byte-identical to iverilog 13.
TEST_CASE("gate delay on a multiply-driven net is per driver (LRM 28.16)") {
    const std::string out = run_capture(R"(
        module m;
            logic a, b;
            wire  w;
            buf #5 g (w, a);
            assign   w = b;
            initial begin
                a = 0; b = 1'bz;
                #1 $display("t=%0t w=%b", $time, w);  // gate's slot powers up x
                #5 $display("t=%0t w=%b", $time, w);  // 0 landed at t=5
                a = 1;
                #4 $display("t=%0t w=%b", $time, w);  // t=10, lands at t=11
                #2 $display("t=%0t w=%b", $time, w);  // t=12, resolved 1
                b = 0;
                #1 $display("t=%0t w=%b", $time, w);  // undelayed conflict -> x
                b = 1'bz;
                #1 $display("t=%0t w=%b", $time, w);
                a = 0; #2 a = 1;                      // 2-wide pulse < 5: rejected
                #8 $display("t=%0t w=%b", $time, w);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "t=1 w=x\nt=6 w=0\nt=10 w=0\nt=12 w=1\nt=13 w=x\nt=14 w=1\nt=24 w=1\n");
}

// A cmos is two drivers on one net (LRM 28.9), so `cmos #5` delays each half's
// own slot. Both halves conduct while ncontrol==1 and pcontrol==0; flipping both
// at t=12 floats the switch at t=17, and re-enabling at t=19 makes the delayed
// 1 conflict with the plain driver's 0 at t=24. Byte-identical to iverilog 13,
// which also matches `nmos #5` + `pmos #5` written out by hand.
TEST_CASE("cmos delay on a multiply-driven net delays each half (LRM 28.9, 28.16)") {
    const std::string out = run_capture(R"(
        module m;
            logic d, nc, pc, b;
            wire  w;
            cmos #5 c (w, d, nc, pc);
            assign    w = b;
            initial begin
                d = 0; nc = 1; pc = 0; b = 1'bz;
                #1 $display("t=%0t w=%b", $time, w);  // both halves power up x
                #5 $display("t=%0t w=%b", $time, w);  // conducting 0, landed t=5
                d = 1;
                #6 $display("t=%0t w=%b", $time, w);  // t=12, conducting 1
                nc = 0; pc = 1;
                #6 $display("t=%0t w=%b", $time, w);  // t=18, both off -> z at 17
                b = 0;
                #1 $display("t=%0t w=%b", $time, w);  // only the plain driver
                nc = 1; pc = 0;
                #6 $display("t=%0t w=%b", $time, w);  // t=25, 1 vs 0 -> x
                $finish;
            end
        endmodule
    )");
    CHECK(out == "t=1 w=x\nt=6 w=0\nt=12 w=1\nt=18 w=z\nt=19 w=0\nt=25 w=x\n");
}

// Guard: cmos shares the gate rise/fall/turn-off gap (LRM 28.16); its own path
// must reject the triple rather than lower only the first value.
TEST_CASE("cmos rise/fall/turn-off delay triples are reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            wire y;
            reg d, nc, pc;
            cmos #(1,2) c (y, d, nc, pc);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("rise/fall/turn-off") != std::string::npos);
}

// Guard: a delayed PARTIAL driver of a resolved net would fold as x rather than
// z in the bits it does not drive for the whole initial window, because the slot
// powers up all-x and the delay defers its first Z-background store.
TEST_CASE("gate delay on a bit of a multiply-driven net is reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            wire [3:0] y;
            reg a, b, c;
            and #2 g1 (y[1], a, b);
            and    g2 (y[1], a, c);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("partial driver of a multiply-driven") != std::string::npos);
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
    CHECK(r.diagnostic_text().find("is not supported") != std::string::npos);
}

TEST_CASE("unsupported gate output lvalues are rejected") {
    // Concatenation output: no single (signal, bit) terminal to store into, and
    // no single net to stage a driver slot for.
    const LowerResult r1 = lower_text(R"(
        module m;
            wire y;
            reg a, b;
            and g({y}, a, b);
        endmodule
    )");
    CHECK_FALSE(r1.ok);
    CHECK(r1.diagnostic_text().find("gate output") != std::string::npos);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    // an x/z control collapse to X, as there is no strength model
    // (docs/LRM_NOTES.md, note 37); every row matches iverilog on a plain wire.
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

TEST_CASE("MOS switch truth tables match LRM table 28-6, including the z pass-through") {
    // Data d and control c sweep 0/1/x/z. A MOS switch is a pass device: unlike a
    // bufif (Table 28-5) it never manufactures an x from a floating input, so the
    // whole DATA=z row is z. Every other cell is character-identical to Table
    // 28-5, the weak L/H results for an x/z control collapsing to X (Clotho has
    // no value-strength model). rnmos/rpmos are indistinguishable from
    // nmos/pmos: the LRM 28.14 strength reduction is not modeled
    // (docs/LRM_NOTES.md, note 38). Oracle: iverilog 13.
    const std::string out = run_capture(R"(
        module top;
            logic d, c;
            wire nm, pm, rn, rp;
            nmos  g0(nm, d, c);
            pmos  g1(pm, d, c);
            rnmos g2(rn, d, c);
            rpmos g3(rp, d, c);
            function logic bit4(int k);
                case (k) 0: bit4 = 1'b0; 1: bit4 = 1'b1; 2: bit4 = 1'bx; default: bit4 = 1'bz; endcase
            endfunction
            initial begin
                for (int di = 0; di < 4; di++) begin
                    for (int ci = 0; ci < 4; ci++) begin
                        d = bit4(di); c = bit4(ci); #1;
                        $display("%b%b %b%b%b%b", d, c, nm, pm, rn, rp);
                    end
                end
            end
        endmodule
    )");
    CHECK(out == "00 z0z0\n01 0z0z\n0x xxxx\n0z xxxx\n"
                 "10 z1z1\n11 1z1z\n1x xxxx\n1z xxxx\n"
                 "x0 zxzx\nx1 xzxz\nxx xxxx\nxz xxxx\n"
                 "z0 zzzz\nz1 zzzz\nzx zzzz\nzz zzzz\n");
}

TEST_CASE("a constant z data terminal passes through a MOS switch as z") {
    // A constant terminal folds at compile time, so the z has to survive as its
    // own code: a MOS switch propagates it (Table 28-6) where every other gate
    // reads z as x (Table 28-3). A constant x still reads as x. Oracle: iverilog 13.
    const std::string out = run_capture(R"(
        module top;
            wire y0, y1, y2, y3;
            nmos g0(y0, 1'bz, 1'b1);
            pmos g1(y1, 1'bz, 1'b0);
            nmos g2(y2, 1'b1, 1'b1);
            nmos g3(y3, 1'bx, 1'b1);
            initial begin
                #1;
                $display("%b%b%b%b", y0, y1, y2, y3);
            end
        endmodule
    )");
    CHECK(out == "zz1x\n");
}

TEST_CASE("a cmos switch drives its output from both halves independently") {
    // LRM 28.9 defines cmos(w, d, nc, pc) as nmos(w, d, nc) + pmos(w, d, pc), so
    // it is two drivers on one net. The nc=0/pc=0 rows are the ones that matter:
    // the nmos half is off (z) while the pmos half conducts, and the data value
    // must survive the resolver rather than blending to x. It only does if the
    // Classify pass recorded two drivers so the net actually resolved — with one
    // recorded driver both halves take the plain-store path and stomp each other.
    // rcmos behaves identically: no strength reduction (docs/LRM_NOTES.md, note 38).
    // Oracle: iverilog 13.
    const std::string out = run_capture(R"(
        module top;
            logic d, nc, pc;
            wire  y, ry;
            cmos  c0(y, d, nc, pc);
            rcmos c1(ry, d, nc, pc);
            initial begin
                d = 1'b0; nc = 1'b0; pc = 1'b0; #1; $display("%b%b", y, ry); // nmos off, pmos passes 0
                d = 1'b1; nc = 1'b0; pc = 1'b0; #1; $display("%b%b", y, ry); // nmos off, pmos passes 1
                d = 1'b1; nc = 1'b1; pc = 1'b0; #1; $display("%b%b", y, ry); // both halves agree
                d = 1'b0; nc = 1'b1; pc = 1'b0; #1; $display("%b%b", y, ry);
                d = 1'b1; nc = 1'b0; pc = 1'b1; #1; $display("%b%b", y, ry); // both off -> floats
                d = 1'b0; nc = 1'b1; pc = 1'b1; #1; $display("%b%b", y, ry); // nmos passes 0, pmos off
                d = 1'b1; nc = 1'b1; pc = 1'b1; #1; $display("%b%b", y, ry); // nmos passes 1, pmos off
                d = 1'bz; nc = 1'b1; pc = 1'b0; #1; $display("%b%b", y, ry); // z passes through both
            end
        endmodule
    )");
    CHECK(out == "00\n11\n11\n00\nzz\n00\n11\nzz\n");
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

// -- 2-state gate output terminals --
//
// LRM 10.3.2 lets exactly one primitive output drive a variable, so a `bit`
// target is legal, and LRM 6.11.2 makes the automatic 4-state -> 2-state
// conversion turn x and z into 0. iverilog 13 refuses every primitive-driven
// variable (2-state or not), so the oracle is the LRM-equivalent continuous
// assign of each gate, which iverilog does accept: `assign q = a & b;`,
// `assign t = en ? a : 1'bz;` and `assign v = ~a;` on the same `bit` targets
// print 110 / 010 / 000 / 000 for the stimulus below (the 4-state net form of
// the same three assigns prints 110 / 010 / 0z0 / xxx, i.e. exactly these
// values before the conversion).

TEST_CASE("gate outputs on 2-state targets convert x and z to 0") {
    const std::string out = run_capture(R"(
        module top;
            logic a, b, en;
            bit   q, t;
            bit [7:0] v;
            and    g0(q, a, b);
            bufif1 g1(t, a, en);
            not    g2(v[3], a);
            initial begin
                a = 1; b = 1; en = 1;   #1 $display("%b%b%b", q, t, v[3]);
                b = 0;                  #1 $display("%b%b%b", q, t, v[3]);
                en = 0;                 #1 $display("%b%b%b", q, t, v[3]);
                a = 1'bx; b = 1; en = 1; #1 $display("%b%b%b", q, t, v[3]);
            end
        endmodule
    )");
    CHECK(out == "110\n010\n000\n000\n");
}

// A 2-state member of a 4-state packed net is the only 2-state net-driver shape
// that exists: LRM 6.7.1 restricts a net's data type to a 4-state integral type,
// so the net (and every driver slot cut from its type) is 4-state and holds the Z
// background of a partial driver, while the member's own 2-state view converts
// the driver's value. Both gates below drive bit 0 of the union, so the net is
// multiply driven and folds through WireRes.
//
// This case is not cross-checked as a whole, so the expectation is composed from two
// iverilog-checked halves: the 2-state driver conversion (see above) and the
// WireRes fold of the surviving definite values (net_resolution_test.cpp). The
// third line is the discriminating one: the `bit` member's driver converts its x
// to 0 and then AGREES with the 4-state member's 0, so bit 0 resolves 0 — had the
// x reached the resolver the bit would be x.
TEST_CASE("gate driving a 2-state member of a 4-state packed net resolves") {
    const std::string out = run_capture(R"(
        module top;
            logic a, b, c, d;
            wire union packed { bit [3:0] lo; logic [3:0] hi; } u;
            and g0(u.lo[0], a, b);
            and g1(u.hi[0], c, d);
            initial begin
                a = 1; b = 1; c = 1; d = 1; #1 $display("%b", u);
                b = 0;                      #1 $display("%b", u);
                a = 1'bx; b = 1; d = 0;     #1 $display("%b", u);
                d = 1;                      #1 $display("%b", u);
            end
        endmodule
    )");
    // u[3:1] undriven (z). bit 0: 1&1 vs 1&1 agree -> 1; 0 vs 1 conflict -> x;
    // (x&1 -> bit -> 0) vs 0 agree -> 0; 0 vs 1 conflict -> x.
    CHECK(out == "zzz1\nzzzx\nzzz0\nzzzx\n");
}

// The remaining gate-output shapes stay guarded loud: a struct/union MEMBER is
// not a static (signal, bit) terminal, so a lone gate driving one has no slot to
// stage into and no bit-addressed store path.
TEST_CASE("a single gate driving a packed-net member is rejected") {
    const LowerResult r = lower_text(R"(
        module top;
            logic a, b;
            wire struct packed { bit [3:0] lo; logic [3:0] hi; } s;
            and g0(s.lo[0], a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("unsupported gate output terminal") != std::string::npos);
}

// LRM 28.13 / 28.14: a MOS switch passes the strength of its data input to its
// output, and a resistive one reduces it. Clotho has no strength on a net value,
// so a switch always drives strong; that only shows when the output net resolves
// the switch against another driver, and then the design is refused instead of
// resolved wrongly (docs/LRM_NOTES.md note 38). Oracle: iverilog 13 prints b=1
// c=1 / q=0 r=0 for the first two sources, where Clotho used to print x.
TEST_CASE("a MOS switch whose strength matters is refused, not resolved wrongly") {
    const auto refused = [](std::string_view body, std::string_view what) {
        const LowerResult r = lower_text("module m;\n" + std::string(body) + "\nendmodule");
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(r.diagnostic_text().find(what) != std::string::npos, r.diagnostic_text());
    };
    // A weak data input (explicit drive strength), through nmos and cmos.
    refused("wire a, b, c;\n assign (strong1, weak0) a = 1'b0;\n nmos n(b, a, 1'b1);\n assign b = 1'b1;", "LRM 28.13");
    refused("wire a, c;\n assign (strong1, weak0) a = 1'b0;\n cmos n(c, a, 1'b1, 1'b0);\n assign c = 1'b1;",
            "LRM 28.13");
    // A pulled data input, through pmos.
    refused("wire w, q;\n pullup (w);\n pmos (q, w, 1'b0);\n buf (q, 1'b0);", "LRM 28.13");
    refused("tri1 w;\n wire q;\n nmos (q, w, 1'b1);\n assign q = 1'b0;", "LRM 28.13");
    // A resistive switch, whatever its input.
    refused("wire c, r;\n assign c = 1'b1;\n rnmos (r, c, 1'b1);\n assign r = 1'b0;", "LRM 28.14");
    refused("reg d;\n wire r;\n rpmos (r, d, 1'b0);\n assign r = 1'b0;", "LRM 28.14");
    // A strong input into a shared net, and a switch alone on its net, are exact.
    const LowerResult ok = lower_text(R"(
        module m;
          reg d;
          wire q, r;
          nmos (q, d, 1'b1);
          assign q = 1'b0;
          rnmos (r, d, 1'b1);
        endmodule)");
    CHECK_MESSAGE(ok.ok, ok.diagnostic_text());
}
