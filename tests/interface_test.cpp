#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors event_test.cpp). The interface /
// modport behavior exercised below is defined by LRM 25; iverilog 13 has no
// SystemVerilog interface support (it rejects an interface port at parse time).
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

// Lower and return the rendered diagnostics; ok must be false (a rejected design).
std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostic_text();
}

} // namespace

// A modport port shares the interface instance's storage with the enclosing
// interface: the producer drives it, the consumer reads it, both across module
// boundaries with no copy process. The consumer lags the producer by one
// posedge (the producer's NBA to data/valid commits after both blocks sample).
TEST_CASE("modport port shares storage across modules") {
    const std::string out = run_capture(R"(
        interface simple_bus;
          logic       clk;
          logic [7:0] data;
          logic       valid;
          modport master (output data, output valid, input clk);
          modport slave  (input  data, input  valid, input clk);
        endinterface

        module producer(simple_bus.master p);
          always @(posedge p.clk) begin
            p.data  <= p.data + 8'd1;
            p.valid <= 1'b1;
          end
        endmodule

        module consumer(simple_bus.slave s);
          always @(posedge s.clk) begin
            if (s.valid) $display("got=%0d", s.data);
          end
        endmodule

        module top;
          simple_bus bus();
          producer u_p(bus);
          consumer u_c(bus);
          initial begin bus.clk = 0; bus.data = 8'd0; bus.valid = 0; end
          always #5 bus.clk = ~bus.clk;
          initial #52 $finish;
        endmodule
    )");
    CHECK(out == "got=1\ngot=2\ngot=3\ngot=4\n");
}

// An interface subroutine reached through a modport `import`, an array of
// interfaces (per-element storage), and modport input/output directions.
// Each worker resolves w.sum() to its own interface instance's function.
TEST_CASE("interface subroutine, array, and directions") {
    const std::string out = run_capture(R"(
        interface ibus;
          logic [7:0] a, b;
          function automatic int sum(); return a + b; endfunction
          modport mp (input a, output b, import sum);
        endinterface

        module worker(ibus.mp w);
          always @(w.a) begin
            w.b = w.a + 8'd1;
            $display("worker a=%0d b=%0d sum=%0d", w.a, w.b, w.sum());
          end
        endmodule

        module top;
          ibus bus[2]();
          worker w0(bus[0]);
          worker w1(bus[1]);
          initial begin
            bus[0].a = 8'd10;
            bus[1].a = 8'd20;
            #1;
            $display("top b0=%0d b1=%0d", bus[0].b, bus[1].b);
            $finish;
          end
        endmodule
    )");
    CHECK(out ==
          "worker a=10 b=11 sum=21\n"
          "worker a=20 b=21 sum=41\n"
          "top b0=11 b1=21\n");
}

// An unrestricted (no-modport) interface port resolves member references
// directly to the interface instance's own signals — no ModportPort proxy.
TEST_CASE("unrestricted interface port") {
    const std::string out = run_capture(R"(
        interface ibus;
          logic [7:0] data;
          logic       valid;
        endinterface

        module producer(ibus p);
          initial begin p.data = 8'd7; p.valid = 1'b1; end
        endmodule

        module top;
          ibus bus();
          producer u_p(bus);
          initial begin
            #1 $display("data=%0d valid=%0d", bus.data, bus.valid);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "data=7 valid=1\n");
}

// Two modules driving one interface net is a genuine multiple-driver conflict
// on shared storage (the canonicalized driver tracking sees both as drivers of
// the same underlying net). Whole-net resolution exists (LRM 6.6.1), but the
// drivers here live in different instance scopes (drv1 / drv2), which V1
// resolution does not yet support, so it must still fail loudly.
TEST_CASE("multiple continuous drivers on an interface net fail loudly") {
    const std::string diag = reject(R"(
        interface nbus; wire [7:0] d; endinterface
        module drv1(nbus b); assign b.d = 8'd1; endmodule
        module drv2(nbus b); assign b.d = 8'd2; endmodule
        module top;
          nbus bus();
          drv1 u1(bus);
          drv2 u2(bus);
          initial #1 $finish;
        endmodule
    )");
    CHECK(diag.find("multiple drivers") != std::string::npos);
}

// A modport expression port that renames a whole interface signal
// (`.clean(raw)`, LRM 25.5.3) shares that signal's storage: writing p.clean
// writes raw.
TEST_CASE("modport rename expression port") {
    const std::string out = run_capture(R"(
        interface ibus;
          logic [7:0] raw;
          modport mp (output .clean(raw));
        endinterface
        module leaf(ibus.mp p);
          initial p.clean = 8'd5;
        endmodule
        module top;
          ibus bus();
          leaf l(bus);
          initial #1 begin $display("%0d", bus.raw); $finish; end
        endmodule
    )");
    CHECK(out == "5\n");
}

// A modport expression port over a bit sub-range (`.lo(raw[3:0])`) maps to
// part of a signal, not a whole one; Clotho does not lower it as a sub-lvalue
// yet, so it must reject rather than silently write the whole signal.
TEST_CASE("modport part-select expression port fails loudly") {
    const std::string diag = reject(R"(
        interface ibus;
          logic [7:0] raw;
          modport mp (output .lo(raw[3:0]));
        endinterface
        module leaf(ibus.mp p);
          initial p.lo = 4'd7;
        endmodule
        module top;
          ibus bus();
          leaf l(bus);
          initial #1 begin $display("%0h", bus.raw); $finish; end
        endmodule
    )");
    CHECK(diag.find("modport expression port") != std::string::npos);
}

// -- Virtual interfaces (LRM 25.9) --
//
// A handle is the address of one elaborated instance's runtime context record; a
// member reached through it is that instance's state base plus the member's
// def-local field index. iverilog has no interface support at all, so every
// expectation below is pinned to the LRM (Verilator agrees on the ones it runs).

// The minimal end-to-end shape: bind a handle to an instance, write a member
// through it, and see the write on the instance itself.
TEST_CASE("virtual interface: bind, write and read a member") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [7:0] d;
            logic       req;
        endinterface
        module top;
            ibus bus();
            virtual ibus vif;
            initial begin
                vif = bus;
                vif.d = 8'hA5;
                #1;
                $display("%h %h", bus.d, vif.d);
                vif.req <= 1'b1;
                #1;
                $display("%b", bus.req);
            end
        endmodule
    )");
    CHECK(out == "a5 a5\n1\n");
}

// A write through a handle has to wake the waiters of the instance it names --
// the bound instance's own wait buckets, not the ones of whichever instance the
// access was compiled against. Both a blocking write and a nonblocking one.
TEST_CASE("virtual interface: a write through a handle wakes the instance's waiters") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic clk;
            logic [7:0] d;
        endinterface
        module top;
            ibus bus();
            virtual ibus vif;
            initial begin
                bus.clk = 0;
                vif = bus;
                #1 vif.clk = 1;
                #1 vif.d <= 8'h3c;
                #2 $finish;
            end
            always @(posedge bus.clk) $display("posedge %0t", $time);
            always @(bus.d)           $display("d %h %0t", bus.d, $time);
        endmodule
    )");
    CHECK(out == "posedge 1\nd 3c 2\n");
}

// One handle variable naming different instances at different times, which is
// the whole point of the construct (LRM 25.9: "A single virtual interface
// variable can thus represent different interface instances").
TEST_CASE("virtual interface: one handle, several instances") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus a(), b();
            virtual ibus vif;
            initial begin
                vif = a; vif.d = 8'h11;
                vif = b; vif.d = 8'h22;
                #1 $display("%h %h", a.d, b.d);
            end
        endmodule
    )");
    CHECK(out == "11 22\n");
}

// The whole operation set LRM 25.9 allows on the variable itself: assignment
// from an instance, from another handle and from null; `==` / `!=` against each.
TEST_CASE("virtual interface: null, identity and assignment") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus a(), b();
            virtual ibus v1, v2;
            initial begin
                $display("%0d %0d", v1 == null, v1 != null);
                v1 = a; v2 = a;
                $display("%0d %0d", v1 == v2, v1 == b);
                v2 = b;
                $display("%0d", v1 == v2);
                v2 = v1;
                $display("%0d", v1 == v2);
                v1 = null;
                $display("%0d %0d", v1 == null, v1 == v2);
            end
        endmodule
    )");
    CHECK(out == "1 0\n1 0\n0\n1\n1 0\n");
}

// A handle is a plane word, so it rides every storage class unchanged: a class
// property initialized from a `new()` argument (the LRM 25.9 SBusTransactor
// shape) and an unpacked array of handles.
TEST_CASE("virtual interface: class property, constructor argument and array") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        class Drv;
            virtual ibus bus;
            function new(virtual ibus b); bus = b; endfunction
            task drive(logic [7:0] v); bus.d = v; endtask
        endclass
        module top;
            ibus s[1:2] ();
            virtual ibus arr[1:2];
            Drv t[1:2];
            initial begin
                arr[1] = s[1]; arr[2] = s[2];
                t[1] = new(arr[1]);
                t[2] = new(arr[2]);
                t[1].drive(8'h0a);
                t[2].drive(8'h0b);
                #1 $display("%h %h", s[1].d, s[2].d);
            end
        endmodule
    )");
    CHECK(out == "0a 0b\n");
}

// An event declared in the interface is a handle word in the instance's state, so
// it needs no virtual-interface machinery of its own: reading it through the
// handle yields the bound instance's event object, and triggering it wakes a
// waiter that named the instance directly.
TEST_CASE("virtual interface: trigger an interface event through a handle") {
    const std::string out = run_capture(R"(
        interface ibus; event e; endinterface
        module top;
            ibus bus();
            virtual ibus vif;
            initial begin
                vif = bus;
                fork
                    begin @(bus.e) $display("woken %0t", $time); end
                    begin #1 -> vif.e; end
                join_any
                #1 $finish;
            end
        endmodule
    )");
    CHECK(out == "woken 1\n");
}

// A modport-qualified handle (LRM 25.9: the modport is part of the type) reaches
// the interface's own storage behind the modport port, so it shares it with
// everything else that names the instance.
TEST_CASE("virtual interface: modport-qualified handle") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [7:0] d;
            modport mp (output d);
        endinterface
        module top;
            ibus bus();
            virtual ibus.mp vif;
            initial begin
                vif = bus;
                vif.d = 8'h5a;
                #1 $display("%h", bus.d);
            end
        endmodule
    )");
    CHECK(out == "5a\n");
}

// Two parameterizations are distinct types (LRM 25.9), and each handle reaches
// its own definition's layout -- the parameter is part of the interface's type,
// so the two do not share a state struct.
TEST_CASE("virtual interface: parameterized interfaces") {
    const std::string out = run_capture(R"(
        interface pbus #(parameter WIDTH = 8);
            logic [WIDTH-1:0] d;
        endinterface
        module top;
            pbus #(16) p16();
            pbus #(32) p32();
            virtual pbus #(16) v16;
            virtual pbus #(32) v32;
            initial begin
                v16 = p16; v32 = p32;
                v16.d = 16'hbeef;
                v32.d = 32'hdeadbeef;
                #1 $display("%h %h", p16.d, p32.d);
            end
        endmodule
    )");
    CHECK(out == "beef deadbeef\n");
}

// LRM 25.9: "Attempting to use a null virtual interface shall result in a fatal
// run-time error." Out of process because the report ends the run.
TEST_CASE("virtual interface: a null handle dereference is a located fatal") {
    constexpr std::string_view src = "interface ibus;\n"
                                     "    logic [7:0] d;\n"
                                     "endinterface\n"
                                     "module top;\n"
                                     "    ibus bus();\n"
                                     "    virtual ibus vif;\n"
                                     "    initial begin\n"
                                     "        $display(\"before\");\n"
                                     "        vif.d = 8'h1;\n"
                                     "        $display(\"after\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out == "before\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("fatal: dereference of a null class handle") != std::string::npos);
    CHECK(run.err.find("source:9:9") != std::string::npos);
}

// An event control through a handle needs the waiter to park on the instance the
// handle names, which the per-signal wait buckets cannot express. Refused loudly
// rather than synthesized into a continuous net, which would both dereference the
// handle before it is assigned and freeze onto one instance.
TEST_CASE("virtual interface: an event control through a handle is refused") {
    const std::string diag = reject(R"(
        interface ibus; logic clk; endinterface
        module top;
            ibus bus();
            virtual ibus vif;
            initial begin vif = bus; @(posedge vif.clk); end
        endmodule
    )");
    CHECK(diag.find("event control through a virtual interface") != std::string::npos);
}

// Same gap, level form: the reads collector would otherwise record the HANDLE
// variable as the sensitivity and the wait would never wake.
TEST_CASE("virtual interface: a level wait on a member is refused") {
    const std::string diag = reject(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus bus();
            virtual ibus vif;
            initial begin vif = bus; wait(vif.d == 8'h5); end
        endmodule
    )");
    CHECK(diag.find("`wait` on a virtual interface member") != std::string::npos);
}

// A clockvar read resolves, but a synchronous drive is performed by the block's
// own Re-NBA process, which is bound to one instance. Refused whole.
TEST_CASE("virtual interface: a clocking block through a handle is refused") {
    const std::string diag = reject(R"(
        interface ibus(input bit clk);
            logic [7:0] d;
            clocking cb @(posedge clk); output d; endclocking
        endinterface
        module top;
            bit clk = 0;
            ibus bus(clk);
            virtual ibus vif;
            initial begin vif = bus; vif.cb.d <= 8'h1; end
        endmodule
    )");
    CHECK(diag.find("clocking block reached through a virtual interface") != std::string::npos);
}

// With no instance of the interface anywhere, there is no layout to resolve a
// member against -- and no assignment could ever make the handle non-null.
TEST_CASE("virtual interface: a member of an uninstantiated interface is refused") {
    const std::string diag = reject(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            virtual ibus vif;
            initial vif.d = 8'h1;
        endmodule
    )");
    CHECK(diag.find("no elaborated instance") != std::string::npos);
}
