#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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
    return r.diagnostics;
}

} // namespace

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

// A continuous driver on an interface net from two modules is a genuine
// multiple-driver conflict on shared storage; Clotho has no net resolution, so
// it must fail loudly rather than silently pick one (the canonicalized driver
// tracking sees both as drivers of the same underlying net).
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
