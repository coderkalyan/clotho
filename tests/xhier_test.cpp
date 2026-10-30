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

// A bidirectional bus with drivers in two separate instance scopes (the
// classic tri-state-bus-of-multiple-drivers shape) used to be refused
// outright: alloc_driver_slot required every driver of one net to share the
// SAME instance scope, since only the scope that discovered the FIRST driver
// was ever recorded, and a second driver from elsewhere failed loudly rather
// than risk a resolver that could not address a slot from another scope.
// That address concern turned out not to apply: register_signal already
// gives every driver slot a design-wide, flat ir::Signal id regardless of
// which scope's Classify/Emit pass allocated it (the scope it records a slot
// under is purely for VCD grouping of that ONE slot), so emit_wire_resolvers'
// fold already reaches a cross-scope slot with no change. The one real
// question -- which scope should the RESOLVER process itself (the thing that
// reads every slot and writes the net) be attributed to, once its slots come
// from more than one scope -- has an existing answer: no_scope, the same
// "belongs to no single instance" marker a per-top PreInit process already
// uses. Verified against the WireRes conflict-resolution semantics a
// same-scope multi-driver net already has: undriven is z, either driver
// alone wins outright, and two drivers disagreeing on a bit folds to x --
// identical whether or not the drivers share an instance scope.
TEST_CASE("a tri-state bus driven from two different instance scopes resolves correctly") {
    constexpr std::string_view src = R"(
        module tsdrv(inout wire bus, input drive, input val);
          assign bus = drive ? val : 1'bz;
        endmodule
        module top;
          wire bus;
          logic d1, v1, d2, v2;
          tsdrv a(bus, d1, v1);
          tsdrv b(bus, d2, v2);
          initial begin
            d1 = 0; v1 = 0; d2 = 0; v2 = 0;
            #1 $display("undriven=%b", bus);
            d1 = 1; v1 = 1;
            #1 $display("a_drives_1=%b", bus);
            d1 = 0;
            d2 = 1; v2 = 0;
            #1 $display("b_drives_0=%b", bus);
            d1 = 1; v1 = 1;
            #1 $display("conflict=%b", bus);
          end
        endmodule
    )";
    CHECK(run_capture(src) == "undriven=z\n"
                              "a_drives_1=1\n"
                              "b_drives_0=0\n"
                              "conflict=x\n");
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

// A `logic` interface member (a VARIABLE, not a net -- LRM 6.6 resolution is
// net-kind only) driven from a single cross-scope source: a `bind`-inserted
// module reaching the interface instance via LRM 23.8 upward name
// referencing. This is the exact shape confirmed on a real design: uvm-
// examples' uvm_tb_build_ss_tb/sub_system_tbs/pss_tb binds an `apb_prober`
// module into its DUT, and the prober does `assign APB.PSEL = ...;` with
// `APB` unresolved locally, so it searches upward through the bind target's
// enclosing scopes and lands on the interface instantiated in the testbench
// top. One driver reaches the net, so there is nothing to resolve -- this is
// an ordinary (if unusually reached) hierarchical write, not multi-driver
// resolution, and it lowers on the plain single-driver path like any other
// xhier write above.
TEST_CASE("xhier: bind-inserted module drives an interface member via upward name") {
    const std::string out = run_capture(R"(
        interface my_if(input clk);
          logic [3:0] sig;
        endinterface
        module dut;
          logic [3:0] internal_sig = 4'b0101;
        endmodule
        module prober(input logic [3:0] sig_in);
          assign MY.sig = sig_in;
        endmodule
        bind dut prober probe(.sig_in(internal_sig));
        module top;
          logic clk = 0;
          my_if MY(clk);
          dut d();
          initial #1 $display("sig=%b", MY.sig);
        endmodule
    )");
    CHECK(out == "sig=0101\n");
}

// The SAME interface member also connected as an output port elsewhere is a
// different matter, and it is illegal SystemVerilog rather than an
// unimplemented gap: `logic` is a variable, and LRM 23.3.3 treats a port
// connection as an implicit continuous assignment, so a variable output port
// PLUS the bind-inserted `assign` above are two continuous assignments to one
// variable -- LRM 6.5 / 10.3.2 make that a flat error regardless of scope,
// with no wired resolution to fall back on the way a `wire`/`tri` net has
// (LRM 6.6). slang raises MultipleContAssigns for exactly this (promoted to
// Error in apply_severity_policy, lower/Lower.cpp), before lower/Lower.cpp's own
// net-driver tracking -- which is net-kind-only via canonical_value_symbol's
// `SymbolKind::Net` gate and never sees a `logic` symbol -- is even reached.
// Confirmed against the real design this mirrors: uvm_tb_build_ss_tb's
// hdl_top.sv keeps its `apb_driver_bfm` instance's connection to the shared
// `APB` interface commented out specifically in the composition where
// `apb_prober` is also bound onto the same net (both live at once would hit
// this exact rule); with only the prober's driver connected, as the design
// ships, the case above is what actually reaches clotho, not this one.
TEST_CASE("a bind-inserted assign and an output port both driving a logic interface member fails loudly") {
    const LowerResult r = lower_text(R"(
        interface my_if(input clk);
          logic [3:0] sig;
        endinterface
        module drv_bfm(output logic [3:0] sig);
          assign sig = 4'b1010;
        endmodule
        module dut;
          logic [3:0] internal_sig = 4'b0101;
        endmodule
        module prober(input logic [3:0] sig_in);
          assign MY.sig = sig_in;
        endmodule
        bind dut prober probe(.sig_in(internal_sig));
        module top;
          logic clk = 0;
          my_if MY(clk);
          drv_bfm b(.sig(MY.sig));
          dut d();
          initial #1 $display("sig=%b", MY.sig);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("multiple continuous assignments") != std::string::npos);
}

// defparam (LRM 23.10.1). An implicitly typed parameter takes the type of its
// final value (LRM 6.20.2), and slang keeps a defparam'd one at its original
// initializer's type, so a defparam that changes that type is refused.

TEST_CASE("defparam: a same-typed value and an explicitly typed target simulate") {
    // Icarus Verilog 13 prints "7 7": q's declared int converts 6.5 to 7.
    CHECK(run_capture(R"(
        module lower;
          parameter p = 4;
          parameter int q = 1;
          initial $display("%0d %0d", p, q);
        endmodule
        module top;
          defparam dut.p = 7;
          defparam dut.q = 6.5;
          lower dut();
        endmodule
    )") == "7 7\n");
}

TEST_CASE("defparam: a real value onto an implicitly typed integer parameter is refused") {
    // ivtest pr1907192: aborted on an assertion; built without assertions it
    // compared the real held in an int-typed constant.
    const LowerResult r = lower_text(R"(
        module lower;
          parameter lwrval = 4;
          initial if (lwrval != 6.0) $display("FAILED"); else $display("PASSED");
        endmodule
        module top;
          defparam dut.lwrval = 6.5;
          lower dut();
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find("a defparam gives parameter 'lwrval' a value of type 'real'") !=
                    std::string::npos,
                  r.diagnostic_text());
}

TEST_CASE("defparam: a narrower value is refused even where only a width reads it") {
    // By LRM 6.20.2 `w` becomes a 3-bit unsigned value; slang keeps it a 32-bit
    // int. No expression references `w` at run time, only a declaration's range.
    const LowerResult r = lower_text(R"(
        module lower;
          parameter w = 4;
          wire [w-1:0] x = '1;
          initial #1 $display("%b", x);
        endmodule
        module top;
          defparam dut.w = 3'b101;
          lower dut();
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find("a defparam gives parameter 'w' a value of type") != std::string::npos,
                  r.diagnostic_text());
}

TEST_CASE("defparam check: a localparam computed by a function using %= simulates") {
    // No defparam here. slang's constant evaluation of `x %= 2` returns a value
    // whose width and signedness differ from the localparam's type, which an
    // earlier check on the value refused (ivtest cfunc_assign_op_vec). Icarus
    // Verilog 13 prints "1 1".
    CHECK(run_capture(R"(
        module top;
          function integer mod2(input integer x);
            x %= 2;
            mod2 = x;
          endfunction
          function integer mod2b(input integer x);
            mod2b = x % 2;
          endfunction
          localparam a = mod2(5);
          localparam b = mod2b(5);
          initial $display("%0d %0d", a, b);
        endmodule
    )") == "1 1\n");
}

// An inout port that is a select of its net (`module m(io[1:0])`, LRM 23.2.2.1)
// connects those bits only, but an alias shares the connection's storage with the
// whole net, so it would put a 2-bit signal under all 4 bits.
TEST_CASE("an inout port that is a select of its net fails loudly") {
    const LowerResult r = lower_text(R"(
        module leaf(io[1:0]);
          inout [3:0] io;
        endmodule
        module top;
          wire [1:0] w;
          leaf u(w);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("select port expression") != std::string::npos);
}

// An inout port and the net it connects share one storage, and a port that is not
// as wide as its net has none in common with it: LRM 23.3.3 makes the connection
// of the bits they share a transistor, the rest of the wider side unconnected, and
// aliasing put the whole port over the net's slot. A 64-bit port on a 1-bit net
// wrote past it, onto the variables declared next to it (ivtest br_gh127b/e).
TEST_CASE("an inout port connected to a net of another width fails loudly") {
    SUBCASE("a wider port, with variables beside the net it used to overwrite") {
        const LowerResult r = lower_text(R"(
            module drv(inout [63:0] p);
              assign p = 64'h1;
            endmodule
            module top;
              wire w;
              logic [7:0] v_post = 8'h22;
              wire  [7:0] n = 8'h5a;
              drv d(w);
              initial #1 $display("%h %h", v_post, n);
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("net of another width (64 and 1 bits)") != std::string::npos);
    }
    SUBCASE("a narrower port") {
        const LowerResult r = lower_text(R"(
            module drv(inout [1:0] p);
              assign p = 2'b01;
            endmodule
            module top;
              wire [2:0] v;
              drv d(v);
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("net of another width (2 and 3 bits)") != std::string::npos);
    }
}

// LRM 23.2.2.1 Example 4 and Example 6: two ports of one module bound to one
// internal net. The two nets they connect become one net, which an alias (one
// target per internal net) cannot express: the second port used to replace the
// first, leaving one of the nets unconnected.
TEST_CASE("two inout ports bound to one internal net fail loudly") {
    SUBCASE("implicit ports (a, a)") {
        const LowerResult r = lower_text(R"(
            module id(a, a);
              inout a;
            endmodule
            module top;
              wire b, c;
              id i1(b, c);
              assign b = 1'b0;
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("two inout/ref ports bound to one net") != std::string::npos);
    }
    SUBCASE("explicitly named ports (.p(i), .q(i))") {
        const LowerResult r = lower_text(R"(
            module same_port(.p(i), .q(i));
              inout i;
            endmodule
            module top;
              wire b, c;
              same_port u(.p(b), .q(c));
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("two inout/ref ports bound to one net") != std::string::npos);
    }
}

// The other directions need no merge and always worked: two input connections
// drive the one net, which resolves them ("The inputs are tied together", LRM
// 23.2.2.1 Example 6), two output connections each receive its value, and an
// inout whose second port is left unconnected is one connection. iverilog 13
// prints the same.
TEST_CASE("two ports bound to one internal net connect each (LRM 23.2.2.1)") {
    const std::string out = run_capture(R"(
        module same_input(a, a);
          input a;
        endmodule
        module same_output(o, o);
          output o;
          assign o = 1'b1;
        endmodule
        module one_side(a, a);
          inout a;
        endmodule
        module top;
          reg  p = 1'b0, q = 1'b1, r = 1'b1, s = 1'b1;
          wire o1, o2;
          wire b;
          same_input  i1(p, q);
          same_input  i2(r, s);
          same_output i3(o1, o2);
          one_side    i4(b, );
          assign b = 1'b0;
          initial #1 $display("%b %b %b %b %b", i1.a, i2.a, o1, o2, i4.a);
        endmodule
    )");
    CHECK(out == "x 1 1 1 0\n");
}

// Port coercion (LRM 23.3.3.1: a port "declared as input (output) but used as an
// output (input) or inout may be coerced to inout"). Clotho copies the value across
// an input or output port and coerces only under --allow-port-coercion, and only a
// port whose coercion the design could observe: an input the module drives, or an
// output whose net has other drivers while the module reads it. Expected values from
// the LRM (a coerced port is one net with its connection); iverilog 13, which always
// coerces, prints the same.
namespace {

LowerResult lower_coerced(std::string_view source) {
    return lower_text(source, nullptr, ir::SystemAccess::Off, /*allow_port_coercion=*/true);
}

std::string run_coerced(std::string_view source) {
    const LowerResult r = lower_coerced(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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

constexpr std::string_view kInputDrivenInside = R"(
    module m(input a); assign a = 1'b1; endmodule
    module top; wire w; m u(w); initial #1 $display("%b", w); endmodule
)";

constexpr std::string_view kOutputReadWhileDriven = R"(
    module m(output o); assign o = 1'b0; wire r = o; endmodule
    module top; wire w; assign w = 1'b1; m u(w); initial #1 $display("w=%b m sees %b", w, u.r); endmodule
)";

} // namespace

TEST_CASE("an input port the module drives is refused unless coercion is allowed") {
    const LowerResult r = lower_text(kInputDrivenInside);
    CHECK_FALSE(r.ok);
    const std::string text = r.diagnostic_text();
    CHECK(text.find("input port 'a' is driven from inside the module") != std::string::npos);
    CHECK(text.find("--allow-port-coercion") != std::string::npos);
    // slang's own warning says Clotho coerced the port, which it did not.
    CHECK(text.find("coerced to 'inout'") == std::string::npos);
    // With the flag the two nets are one net: the module's driver reaches `w`.
    CHECK(run_coerced(kInputDrivenInside) == "1\n");
}

TEST_CASE("an output port read by its module while its net has other drivers is refused unless coercion is allowed") {
    const LowerResult r = lower_text(kOutputReadWhileDriven);
    CHECK_FALSE(r.ok);
    const std::string text = r.diagnostic_text();
    CHECK(text.find("output port 'o' is read inside the module while its net has other drivers") != std::string::npos);
    CHECK(text.find("--allow-port-coercion") != std::string::npos);
    CHECK(text.find("coerced to 'inout'") == std::string::npos);
    // Coerced, the module's `0` and the parent's `1` resolve on one net it reads too.
    CHECK(run_coerced(kOutputReadWhileDriven) == "w=x m sees x\n");
}

TEST_CASE("ports whose coercion changes nothing are accepted by default") {
    // A bus of output ports (LRM 23.3.3.2), a module that reads an output nobody else
    // drives, a module that only drives an output the parent also drives, and an
    // unconnected input the module drives: none can tell a copy from one net.
    constexpr std::string_view src      = R"(
        module drv(input en, input v, output o); assign o = en ? v : 1'bz; endmodule
        module rd(output o); assign o = 1'b1; wire r = o; endmodule
        module wr(output o); assign o = 1'b0; endmodule
        module un(input a); assign a = 1'b1; endmodule
        module top;
            reg e1 = 1, e2 = 0;
            wire bus, w2, w3;
            assign w3 = 1'b1;
            drv a(e1, 1'b1, bus);
            drv b(e2, 1'b0, bus);
            rd  c(w2);
            wr  d(w3);
            un  e();
            initial #1 $display("bus=%b w2=%b w3=%b c.r=%b e.a=%b", bus, w2, w3, c.r, e.a);
        endmodule
    )";
    const std::string          expected = "bus=1 w2=1 w3=x c.r=1 e.a=1\n";
    CHECK(run_capture(src) == expected);
    CHECK(run_coerced(src) == expected);
}

TEST_CASE("coercion covers non-ANSI ports and every instance") {
    constexpr std::string_view src = R"(
        module m(a); input a; assign a = 1'b1; endmodule
        module top; wire w, x; m u(w); m v(x); initial #1 $display("%b%b", w, x); endmodule
    )";
    const LowerResult          r   = lower_text(src);
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("input port 'a' is driven from inside the module") != std::string::npos);
    CHECK(run_coerced(src) == "11\n");
}

TEST_CASE("a port that needs coercion but cannot be coerced is an error with the flag") {
    SUBCASE("a variable connection") {
        const LowerResult r = lower_coerced(R"(
            module m(input a); assign a = 1'b1; endmodule
            module top; logic v; m u(v); endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("a variable cannot be connected to an inout port") != std::string::npos);
    }
    SUBCASE("a connection of another width") {
        const LowerResult r = lower_coerced(R"(
            module m(input [3:0] a); assign a = 4'b1; endmodule
            module top; wire [1:0] w; m u(w); endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("only supported for a port connected to a whole net of the same width") !=
              std::string::npos);
    }
    SUBCASE("a select connection") {
        const LowerResult r = lower_coerced(R"(
            module m(input a); assign a = 1'b1; endmodule
            module top; wire [3:0] w; m u(w[0]); endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("only supported for a port connected to a whole net of the same width") !=
              std::string::npos);
    }
}
