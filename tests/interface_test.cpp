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

// Two modules driving one interface net from different instance scopes is an
// ordinary multi-driver WHOLE-NET conflict (LRM 6.6.1): each `assign` stages
// its value into its own driver slot regardless of which scope discovered
// it, and a resolver process (attributed to no single scope, since its slots
// now come from more than one) folds them exactly as it would for two
// same-scope drivers -- a definite winner when only one drives, X where two
// disagree.
TEST_CASE("multiple continuous drivers on an interface net resolve across scopes") {
    const std::string out = run_capture(R"(
        interface nbus; wire [7:0] d; endinterface
        module drv1(nbus b); assign b.d = 8'd1; endmodule
        module drv2(nbus b); assign b.d = 8'd2; endmodule
        module top;
          nbus bus();
          drv1 u1(bus);
          drv2 u2(bus);
          initial #1 begin $display("d=%b", bus.d); $finish; end
        endmodule
    )");
    // 8'd1 = 0000_0001, 8'd2 = 0000_0010: bits 0 and 1 disagree (fold to x),
    // every other bit agrees at 0.
    CHECK(out == "d=000000xx\n");
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
                vif.req = 1'b1;
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
                #1 vif.d = 8'h3c;
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
    CHECK(run.err.find("fatal: use of a null virtual interface") != std::string::npos);
    CHECK(run.err.find("source:9:9") != std::string::npos);
}

// An event control through a handle parks a frame-resident node on the member's
// waiter list in the instance the handle names -- the intrusive list the event
// objects use, so there is no capacity to size and no bucket to pick. Retargeting
// the handle between waits moves which instance is watched.
TEST_CASE("virtual interface: an event control through a handle") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic clk;
            logic [7:0] d;
        endinterface
        module top;
            ibus a(), b();
            virtual ibus vif;
            initial begin
                a.clk = 0; b.clk = 0;
                forever #1 begin a.clk = ~a.clk; b.clk = ~b.clk; end
            end
            initial begin
                vif = a;
                @(posedge vif.clk) $display("pos %0t", $time);
                vif = b;
                @(negedge vif.clk) $display("neg %0t", $time);
                @(vif.d)           $display("d %h %0t", vif.d, $time);
                $finish;
            end
            initial #6 b.d = 8'h09;
        endmodule
    )");
    CHECK(out == "pos 1\nneg 2\nd 09 6\n");
}

// The level form: the reads collector sees only the HANDLE variable, so the
// members are gathered separately and each parks on its own instance.
TEST_CASE("virtual interface: a level wait on a member") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus bus();
            virtual ibus vif;
            initial begin
                vif = bus;
                wait(vif.d == 8'h5);
                $display("woken %0t", $time);
            end
            initial #3 bus.d = 8'h5;
        endmodule
    )");
    CHECK(out == "woken 3\n");
}

// An event control over an EXPRESSION of members (LRM 9.4.2) cannot become a
// continuous net: that net's process would read the handle before it was assigned
// and freeze onto one instance. Desugared instead into the shape a `wait` already
// uses -- sample, park on each member, re-evaluate -- so it retargets with the
// handle.
TEST_CASE("virtual interface: an event control on a member expression") {
    const std::string out = run_capture(R"(
        interface ibus; logic a, b; endinterface
        module top;
            ibus bus();
            virtual ibus vif;
            initial begin
                vif = bus;
                @(vif.a & vif.b);
                $display("changed %0t", $time);
            end
            initial begin #1 bus.a = 1; #1 bus.b = 1; end
        endmodule
    )");
    CHECK(out == "changed 2\n");
}

// An interface subroutine reached through a handle (LRM 25.7) is lowered ONCE for
// the definition, with the handle as formal 0, and addresses its signals through
// the instance that names. Two handles on twin instances must therefore drive
// their own instances, and a blocking task must suspend on its own instance's
// signals.
TEST_CASE("virtual interface: a subroutine call through a handle") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [7:0] d;
            task automatic drive(input logic [7:0] v); d = v; #1 d = d + 1; endtask
            function automatic int getd(); return d; endfunction
        endinterface
        module top;
            ibus a(), b();
            virtual ibus v1, v2;
            initial begin
                v1 = a; v2 = b;
                v1.drive(8'h10);
                v2.drive(8'h20);
                $display("a=%h b=%h peek=%0d/%0d", a.d, b.d, v1.getd(), v2.getd());
            end
        endmodule
    )");
    CHECK(out == "a=11 b=21 peek=17/33\n");
}

// A bare call to a sibling subroutine of the same interface, from inside such a
// body, is a call on the SAME instance: the context travels on as arg 0.
TEST_CASE("virtual interface: a sibling call keeps the instance") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [7:0] d;
            function automatic void poke(input logic [7:0] v); d = v; endfunction
            task automatic seq(); poke(8'hAA); #1 poke(8'hBB); endtask
        endinterface
        module top;
            ibus a(), b();
            virtual ibus v2;
            initial begin v2 = b; v2.seq(); $display("a=%h b=%h", a.d, b.d); end
        endmodule
    )");
    CHECK(out == "a=xx b=bb\n");
}

// A member some handle nonblocking-writes leaves the per-signal block shadow (one
// slot per signal, which two handles naming twin instances would collide in) for
// the deferred-record path, whose record carries the destination resolved at
// statement-execution time. Execution order still decides the winner.
TEST_CASE("virtual interface: a nonblocking write through a handle") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus a(), b();
            virtual ibus v1, v2;
            initial begin
                v1 = a; v2 = b;
                v1.d <= 8'h11;
                v2.d <= 8'h22;
                a.d  <= 8'h33;
                #1 $display("a=%h b=%h", a.d, b.d);
            end
        endmodule
    )");
    CHECK(out == "a=33 b=22\n");
}

// A clocking block reached through a handle (LRM 25.9.1 / 25.9.2): the clockvar
// read resolves against the sampled storage of the instance the handle names, and
// a synchronous drive lands in that instance's Re-NBA region.
TEST_CASE("virtual interface: a clocking block through a handle") {
    const std::string out = run_capture(R"(
        interface ibus(input bit clk);
            logic [7:0] d, q;
            clocking cb @(posedge clk); input q; output d; endclocking
        endinterface
        module top;
            bit clk = 0;
            ibus a(clk), b(clk);
            virtual ibus v1, v2;
            always #1 clk = ~clk;
            initial begin
                v1 = a; v2 = b;
                @(posedge clk);
                v1.cb.d <= 8'h1;
                v2.cb.d <= 8'h2;
                @(posedge clk);
                // A clockvar read returns the value sampled at the last clocking
                // event, so each handle must reach its own instance's sample.
                $display("q=%h/%h", v1.cb.q, v2.cb.q);
                @(posedge clk);
                $display("a=%h b=%h", a.d, b.d);
                $finish;
            end
            initial begin a.q = 8'h55; b.q = 8'h66; end
        endmodule
    )");
    CHECK(out == "q=55/66\na=01 b=02\n");
}

// With no instance of the interface anywhere, the handle is null for the whole run
// and every access through it is a run-time fatal. A design that never executes the
// access is still legal, so this is a warning at compile time, not a refusal.
TEST_CASE("virtual interface: a member of an uninstantiated interface is a fatal") {
    const test::DesignRun run = test::run_design_captured(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            virtual ibus vif;
            initial begin
                $display("before");
                vif.d = 8'h1;
                $display("after");
            end
        endmodule
    )");
    CHECK(run.err.find("use of a null virtual interface") != std::string::npos);
    // The write never happens, and nothing after it runs.
    CHECK(run.out == "before\n");
}

// A signal declared inside a GENERATE block of the interface is still a field of
// the instance's own state struct -- a generate block is not a separate instance --
// so a handle reaches it like any other member. It cannot be found by name in the
// body's member list, though, so the representative is located by its position,
// which is identical across instances of one definition.
TEST_CASE("virtual interface: a generate-block member through a handle") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [7:0] d;
            for (genvar i = 0; i < 2; i++) begin : g logic [7:0] e; end
        endinterface
        module top;
            ibus a(), b();
            virtual ibus v1, v2;
            initial begin
                v1 = a; v2 = b;
                v1.g[0].e = 8'h11;
                v2.g[1].e = 8'h22;
                $display("%h/%h %h/%h", a.g[0].e, a.g[1].e, b.g[0].e, b.g[1].e);
            end
        endmodule
    )");
    CHECK(out == "11/xx xx/22\n");
}

// An interface instantiated INSIDE the interface a handle names is a separate
// instance: a separate state struct and a separate context record, which the
// handle's record says nothing about. Resolving the member by type alone would
// find the nested instance's own signal and then read it off the outer instance's
// record -- the wrong layout off the wrong base, which used to be a segfault.
TEST_CASE("virtual interface: a member of a nested interface instance is refused") {
    const std::string diag = reject(R"(
        interface inner; logic [7:0] d; endinterface
        interface outer; inner i(); logic [7:0] e; endinterface
        module top;
            outer o();
            virtual outer vif;
            initial begin vif = o; vif.i.d = 8'h3; end
        endmodule
    )");
    CHECK(diag.find("a separate instance the handle does not name") != std::string::npos);
}

// A nonblocking write inside an interface subroutine reached through a handle is a
// write through a handle, even though the statement names a bare member: the
// instance is the one formal 0 named. It has to take the deferred-record path like
// any other handle write, or it stages into the representative's block shadow and
// commits into the wrong instance.
TEST_CASE("virtual interface: a nonblocking write inside a subroutine through a handle") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [7:0] d;
            task automatic put(input logic [7:0] x); d <= x; endtask
        endinterface
        module top;
            ibus a(), b();
            virtual ibus v1, v2;
            initial begin
                v1 = a; v2 = b;
                v1.put(8'h11);
                v2.put(8'h22);
                #1 $display("a=%h b=%h", a.d, b.d);
            end
        endmodule
    )");
    CHECK(out == "a=11 b=22\n");
}

// An event member is woken through its own object's intrusive list, not through
// the member's signal wait buckets, and the object lives in the instance the handle
// names. Waiting on it has to park on that object -- parking on the vif waiter list
// instead never woke at all, because `-> vif.e` fires the object.
TEST_CASE("virtual interface: waiting on an event member through a handle") {
    const std::string out = run_capture(R"(
        interface ibus; event e; endinterface
        module top;
            ibus a(), b();
            virtual ibus v1, v2;
            initial begin
                v1 = a; v2 = b;
                fork begin @(v2.e); $display("b at %0t", $time); end join_none
                #1 -> v1.e;
                #1 -> v2.e;
                #1 $display("done");
            end
        endmodule
    )");
    // Only b's event wakes it, at 2 -- not a's at 1.
    CHECK(out == "b at 2\ndone\n");
}

// The run-time null report names what was actually null. A virtual interface is
// bound to an elaborated instance, not constructed with `new`, so the class-handle
// wording would misdirect.
TEST_CASE("virtual interface: a null handle reports as an interface, not a class") {
    const test::DesignRun run = test::run_design_captured(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus a();
            virtual ibus v;
            initial begin v = a; v = null; $display("before"); v.d = 8'h1; end
        endmodule
    )");
    CHECK(run.err.find("use of a null virtual interface") != std::string::npos);
    CHECK(run.err.find("names no interface instance") != std::string::npos);
    CHECK(run.err.find("(LRM 25.9)") != std::string::npos);
    CHECK(run.err.find("class handle") == std::string::npos);
    CHECK(run.out == "before\n");
}

// The UVM driver shape (easyUVM driver.sv:26): the handle is a CLASS PROPERTY and
// the nonblocking write executes inside a class task. The target is the interface
// SIGNAL the handle names (LRM 25.9), not the property, so the write stages like
// any other NBA -- the guard used to walk past the handle access to the handle's
// own storage class and refuse this as "a class property".
//
// The timing is the point (LRM 10.4.2): a reader in the same time step, after the
// task returned, still sees the old values; the update commits in that instant's
// NBA region, which is what wakes the negedge watcher at time 0.
TEST_CASE("virtual interface: a nonblocking write in a class task lands in the NBA region") {
    const std::string out = run_capture(R"(
        interface ibus; logic valid; logic [31:0] A; endinterface
        class drv;
            virtual ibus vif;
            task put();
                vif.valid <= 1'b0;
                vif.A <= 'x;
            endtask
        endclass
        module top;
            ibus bus();
            drv d;
            initial begin
                bus.valid = 1'b1; bus.A = 32'd5;
                d = new; d.vif = bus;
                d.put();
                $display("same-step %b %h", bus.valid, bus.A);
            end
            // 1 -> 0 is the only negedge here (x -> 1 above is a posedge), so this
            // wakes exactly when the staged write commits -- still time 0.
            initial @(negedge bus.valid) $display("committed %b %h %0t", bus.valid, bus.A, $time);
        endmodule
    )");
    CHECK(out == "same-step 1 00000005\ncommitted 0 xxxxxxxx 0\n");
}

// The full easyUVM reset loop: forever { clear; @(posedge vif.rst); } forked from
// the module. Each pass stages its writes in the instant it executes, so the
// values dirtied between reset edges are clean again one step after the edge.
TEST_CASE("virtual interface: the UVM reset loop clears signals on each reset edge") {
    const std::string out = run_capture(R"(
        interface ibus; logic rst; logic valid; logic [31:0] A; endinterface
        class drv;
            virtual ibus vif;
            task reset_signals();
                forever begin
                    vif.valid <= '0;
                    vif.A <= 'x;
                    @(posedge vif.rst);
                end
            endtask
        endclass
        module top;
            ibus bus();
            drv d;
            initial begin
                bus.rst = 0; bus.valid = 1'b1; bus.A = 32'hdeadbeef;
                d = new; d.vif = bus;
                fork d.reset_signals(); join_none
                #1 $display("t1 %b %h", bus.valid, bus.A);
                bus.valid = 1'b1; bus.A = 32'h55;
                #1 bus.rst = 1;
                #1 $display("t3 %b %h", bus.valid, bus.A);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "t1 0 xxxxxxxx\nt3 0 xxxxxxxx\n");
}

// One handle, retargeted between the write's execution and the NBA region: the
// update lands on the instance the handle named WHEN THE STATEMENT EXECUTED
// (LRM 10.4.2 evaluates now, defers only the update; 25.9 says which instance),
// exactly as the record path bakes the destination in at staging time.
TEST_CASE("virtual interface: a retargeted handle does not move a staged write") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        class drv;
            virtual ibus vif;
            task put(input logic [7:0] x); vif.d <= x; endtask
        endclass
        module top;
            ibus a(), b();
            drv m;
            initial begin
                a.d = 8'h05; b.d = 8'h06;
                m = new; m.vif = a;
                m.put(8'h11);
                m.vif = b;
                m.put(8'h22);
                #1 $display("a=%h b=%h", a.d, b.d);
            end
        endmodule
    )");
    CHECK(out == "a=11 b=22\n");
}

// Select forms through the handle in a class task: a whole vector member, a bit
// select and a part select of one, and an element of an unpacked-array member.
// Each stages its own record with the positioned value and write mask, so the
// two selects of `v` merge and the untouched bits keep their old value.
TEST_CASE("virtual interface: nonblocking selects through a handle in a class task") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [15:0] v, w;
            logic [7:0] arr[4];
        endinterface
        class drv;
            virtual ibus vif;
            task go();
                vif.w <= 16'habcd;
                vif.v[0] <= 1'b1;
                vif.v[11:8] <= 4'h7;
                vif.arr[2] <= 8'h9a;
            endtask
        endclass
        module top;
            ibus bus();
            drv d;
            initial begin
                bus.v = 16'h0; bus.arr[2] = 8'h00;
                d = new; d.vif = bus;
                d.go();
                #1 $display("%h %h %h", bus.v, bus.w, bus.arr[2]);
            end
        endmodule
    )");
    CHECK(out == "0701 abcd 9a\n");
}

// A modport-typed handle nonblocking-writes like the plain-typed one: the modport
// symbol canonicalizes to the same interface signal.
TEST_CASE("virtual interface: a nonblocking write through a modport-typed handle") {
    const std::string out = run_capture(R"(
        interface ibus;
            logic [7:0] d;
            modport mp(output d);
        endinterface
        class drv;
            virtual ibus.mp vif;
            task put(); vif.d <= 8'h5a; endtask
        endclass
        module top;
            ibus bus();
            drv m;
            initial begin
                m = new; m.vif = bus;
                m.put();
                #1 $display("%h", bus.d);
            end
        endmodule
    )");
    CHECK(out == "5a\n");
}

// A blocking and a nonblocking write to the same member in one time step
// (LRM 10.4.2): the blocking one lands immediately, the nonblocking one was
// scheduled at ITS execution and commits in the NBA region, so it wins the step.
TEST_CASE("virtual interface: blocking and nonblocking writes to one member in one step") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        class drv;
            virtual ibus vif;
            task mix();
                vif.d <= 8'h42;
                vif.d = 8'h13;
            endtask
        endclass
        module top;
            ibus bus();
            drv m;
            initial begin
                m = new; m.vif = bus;
                m.mix();
                $display("active %h", bus.d);
                #1 $display("after %h", bus.d);
            end
        endmodule
    )");
    CHECK(out == "active 13\nafter 42\n");
}

// The split the misnamed diagnostic hid: a nonblocking write THROUGH the handle
// targets an interface signal and works (above); a nonblocking write TO the
// handle property targets class storage and is the real class-property NBA gap
// (legal SV -- LRM 6.21 bans only continuous forms on a non-static property --
// but an object body has no per-object NBA shadow). The refusal must name the
// property, and only fire for it.
TEST_CASE("virtual interface: NBA to the handle property itself is the class-property gap") {
    const std::string diag = reject(R"(
        interface ibus; logic [7:0] d; endinterface
        class drv;
            virtual ibus vif;
        endclass
        module top;
            ibus bus();
            drv m;
            initial begin
                m = new;
                m.vif <= bus;
            end
        endmodule
    )");
    CHECK(diag.find("nonblocking assignment to a class property is not supported") !=
          std::string::npos);
}

// Intra-assignment timing on a nonblocking write through a handle (LRM 9.4.5 +
// 25.9). The deferred record carries the destination pointer and the instance
// context the handle resolved to AT ARM TIME, which is LRM 10.4.2's rule and not
// an optimization: "If variable_lvalue requires an evaluation, such as an index
// expression, class handle, or virtual interface reference, it shall be evaluated
// at the same time as the expression on the right-hand side."
//
// Two instances, the handle pointing at the second: the update must land in the
// second and leave the first untouched. Not expressible against iverilog (no
// interface support at all), so this is derived from 10.4.2 -- and it is the
// assertion the old refusal existed to make unnecessary, because the wrong answer
// (committing into the representative instance) is silent.
TEST_CASE("virtual interface: intra-assignment delay commits into the handle's instance") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus b0(); ibus b1();
            virtual ibus vif;
            initial begin
                b0.d = 8'h00;
                b1.d = 8'h00;
                vif  = b1;
                #1 vif.d <= #5 8'ha5;
                #4 $display("t=%0t b0=%h b1=%h", $time, b0.d, b1.d);
                #2 $display("t=%0t b0=%h b1=%h", $time, b0.d, b1.d);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "t=5 b0=00 b1=00\nt=7 b0=00 b1=a5\n");
}

// The wake goes with the value. A vif member is a scalar with real edge semantics
// (`@(posedge b1.d)` is legal, unlike `@(posedge mem[i])`), so the commit cannot
// wake level-only the way the array path does -- it wakes PER RECORD under the
// instance context the record captured. The waiter on the written instance fires
// once, at the deferred instant; the waiter on the twin never fires.
TEST_CASE("virtual interface: intra-assignment delay wakes only the written instance") {
    const std::string out = run_capture(R"(
        interface ibus; logic d; endinterface
        module top;
            ibus b0(); ibus b1();
            virtual ibus vif;
            initial begin
                b0.d = 1'b0;
                b1.d = 1'b0;
                vif  = b1;
                #1 vif.d <= #5 1'b1;
                #20 $finish;
            end
            always @(posedge b1.d) $display("t=%0t posedge b1", $time);
            always @(posedge b0.d) $display("t=%0t posedge b0", $time);
        endmodule
    )");
    CHECK(out == "t=6 posedge b1\n");
}

// The event form takes the same pointer-carrying record as the delay form: the
// countdown changes when the update lands, not where.
TEST_CASE("virtual interface: intra-assignment event commits into the handle's instance") {
    const std::string out = run_capture(R"(
        interface ibus; logic [7:0] d; endinterface
        module top;
            ibus b0(); ibus b1();
            virtual ibus vif;
            logic clk = 0;
            always #5 clk = ~clk;
            initial begin
                b0.d = 8'h00;
                b1.d = 8'h00;
                vif  = b1;
                #1 vif.d <= repeat(2) @(posedge clk) 8'h3c;
                #10 $display("t=%0t b0=%h b1=%h", $time, b0.d, b1.d);
                #12 $display("t=%0t b0=%h b1=%h", $time, b0.d, b1.d);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "t=11 b0=00 b1=00\nt=23 b0=00 b1=3c\n");
}

// A write through a handle after the trigger of an event-controlled update to the same
// member commits after the update (LRM 4.6 b), 10.4.2; docs/design/intra-assignment-
// timing.md, "Order against plain NBAs"), in the instance the handle names, and a plain
// write to the twin instance is not disturbed by the pending record. The same holds
// when the update is a delayed one. Derived from the LRM clauses: iverilog 13 has no
// interface support.
TEST_CASE("virtual interface: a plain write after the trigger commits after a timed update") {
    SUBCASE("event control") {
        const std::string out = run_capture(R"(
            interface ibus; logic [7:0] d; endinterface
            module top;
                ibus b0(); ibus b1();
                virtual ibus vif;
                event e;
                initial begin
                    b0.d = 8'h00;
                    b1.d = 8'h00;
                    vif  = b1;
                    vif.d <= @e 8'h11;
                    #1;
                    -> e;
                    vif.d <= 8'h22;
                    b0.d  <= 8'h33;
                    #1 $display("b0=%h b1=%h", b0.d, b1.d);
                    $finish;
                end
            endmodule
        )");
        CHECK(out == "b0=33 b1=22\n");
    }
    SUBCASE("delay") {
        const std::string out = run_capture(R"(
            interface ibus; logic [7:0] d; endinterface
            module top;
                ibus b0(); ibus b1();
                virtual ibus vif;
                initial begin
                    b0.d = 8'h00;
                    b1.d = 8'h00;
                    vif  = b1;
                    vif.d <= #1 8'h11;
                    #1;
                    vif.d <= 8'h22;
                    b0.d  <= 8'h33;
                    #1 $display("b0=%h b1=%h", b0.d, b1.d);
                    $finish;
                end
            endmodule
        )");
        CHECK(out == "b0=33 b1=22\n");
    }
}
