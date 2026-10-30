// Bit/part-select on a concatenation rvalue (LRM 11.4.12) and an output port
// connected to a concatenation target `.o({a,b})` (LRM 23.3.3). iverilog 13
// cannot parse a select applied directly to a concatenation, so the select
// values were cross-checked against verilator 5.048 (direct form) and against
// iverilog on the temp-wire equivalent (`c = {a,b}; c[7:0]`); both agree. The
// port destructure was verified against iverilog directly.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    return r;
}

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

TEST_CASE("bit and part select on a concatenation rvalue") {
    // {a,b} = 1010_0101. Part-selects, a constant bit-select, a runtime
    // bit-select, and the replication form {2{a}} = 1010_1010.
    const std::string out = run_capture(R"(
        module m;
            logic [3:0] a = 4'hA;
            logic [3:0] b = 4'h5;
            logic [1:0] i = 2'd2;
            initial begin
                $display("%b", {a,b}[7:0]);
                $display("%b", {a,b}[3:0]);
                $display("%b", {a,b}[7:4]);
                $display("%b", {a,b}[3]);
                $display("%b", {a,b}[4]);
                $display("%b", {a,b}[7]);
                $display("%b", {a,b}[i]);
                $display("%b", {2{a}}[5]);
                $display("%b", {2{a}}[3:0]);
            end
        endmodule
    )");
    CHECK(out == "10100101\n0101\n1010\n0\n0\n1\n1\n1\n1010\n");
}

TEST_CASE("output port connected to a concatenation target") {
    // sub drives 8'hC3; the parent scatters it MSB-first: x = C, y = 3.
    const std::string out = run_capture(R"(
        module sub(output [7:0] o);
            assign o = 8'hC3;
        endmodule
        module m;
            logic [3:0] x, y;
            sub s(.o({x, y}));
            initial begin
                #1;
                $display("%b %b", x, y);
            end
        endmodule
    )");
    CHECK(out == "1100 0011\n");
}

TEST_CASE("output port to a concatenation of differing widths") {
    // A 12-bit output scattered into a 4-bit and an 8-bit target.
    const std::string out = run_capture(R"(
        module sub(output [11:0] o);
            assign o = 12'hABC;
        endmodule
        module m;
            logic [3:0]  hi;
            logic [7:0]  lo;
            sub s(.o({hi, lo}));
            initial begin
                #1;
                $display("%h %h", hi, lo);
            end
        endmodule
    )");
    CHECK(out == "a bc\n");
}

// An output port assigns to the connected expression "using the same rules as an
// assignment" (LRM 23.3.3.8), and the sign attribute does not cross the
// hierarchy, so what extends a port value narrower than its connection is the
// port's own signedness (LRM 10.7): a signed port is sign-extended, an unsigned
// one zero-extended, an x sign bit stays x. The connection may be a net, a
// variable or a concatenation target. Expected values from the LRM; iverilog 13
// prints the same.
TEST_CASE("a signed output port sign-extends into a wider connection") {
    const std::string out = run_capture(R"(
        module s_neg(output signed [3:0] o);
            assign o = -4'sd1;
        endmodule
        module s_pos(output signed [3:0] o);
            assign o = 4'sd5;
        endmodule
        module s_x(output signed [3:0] o);
            assign o = 4'bx01;
        endmodule
        module u_neg(output [3:0] o);
            assign o = 4'b1111;
        endmodule
        module s_var(output logic signed [3:0] o);
            initial o = -4'sd2;
        endmodule
        module s_cat(output signed [3:0] o);
            assign o = -4'sd3;
        endmodule
        module top;
            wire  [7:0] w1, w2, w3, w4, w5;
            wire  [1:0] hi;
            wire  [5:0] lo;
            logic [7:0] v6;
            s_neg u1(w1);
            s_pos u2(w2);
            s_x   u3(w3);
            u_neg u4(w4);
            s_var u5(w5);
            s_cat u6({hi, lo});
            s_var u7(v6);
            initial #1 $display("%b %b %b %b %b %b %b %b", w1, w2, w3, w4, w5, hi, lo, v6);
        endmodule
    )");
    CHECK(out == "11111111 00000101 xxxxxx01 00001111 11111110 11 111101 11111110\n");
}

// The sign of an INPUT port's value is the connected expression's (the same
// 23.3.3.8 rule, with the port as the assignment's left-hand side), so the
// signedness of the port does not matter. This direction was already right.
TEST_CASE("an input port extends its value by the connected expression's sign") {
    const std::string out = run_capture(R"(
        module in_s(input signed [7:0] i);
        endmodule
        module in_u(input [7:0] i);
        endmodule
        module top;
            reg signed [3:0] rs = -4'sd2;
            reg        [3:0] ru = 4'b1110;
            in_s a(rs);
            in_s b(ru);
            in_u c(rs);
            in_u d(ru);
            initial #1 $display("%b %b %b %b", a.i, b.i, c.i, d.i);
        endmodule
    )");
    CHECK(out == "11111110 00001110 11111110 00001110\n");
}

// slang wraps the port in the implicit conversion to the connection's type, which
// is also what carries a real <-> integral change (LRM 6.12.1): a real rounds
// half away from zero into an integral connection, an integral port converts into
// a real variable by its own sign. The real port used to reach the backend as a
// resize of a real value, an internal error.
TEST_CASE("an output port converts between real and integral connections") {
    const std::string out = run_capture(R"(
        module r_out(output real r);
            initial r = 2.5;
        endmodule
        module r_neg(output real r);
            initial r = -2.5;
        endmodule
        module s_to_real(output signed [7:0] o);
            assign o = -8'sd2;
        endmodule
        module u_to_real(output [7:0] o);
            assign o = 8'd254;
        endmodule
        module top;
            wire [7:0] wr, wn;
            real rr, ru;
            r_out u1(wr);
            r_neg u2(wn);
            s_to_real u3(rr);
            u_to_real u4(ru);
            initial #1 $display("%0d %0d %0.1f %0.1f", wr, $signed(wn), rr, ru);
        endmodule
    )");
    CHECK(out == "3 -3 -2.0 254.0\n");
}

// A non-ANSI port reference with a constant select (`module m(arg[3:2])`, LRM
// 23.2.2.1: "a bit-select of a vector declared within the module", "a
// part-select") is those bits of the declared port and nothing else: the
// connection drives (input) or reads (output) them alone, the rest of the net is
// not part of the port. Two ports may split one vector, and an explicitly named
// port takes the same select. Expected values from the LRM; iverilog 13 prints
// the same.
TEST_CASE("a non-ANSI port reference with a select connects only those bits") {
    const std::string out = run_capture(R"(
        module p_part(arg[3:2]);
            input [3:0] arg;
        endmodule
        module p_bit(arg[1]);
            input [2:0] arg;
        endmodule
        module p_expl(.x(o[3:2]));
            input [3:0] o;
        endmodule
        module p_two(arg[3:2], arg[1:0]);
            input [3:0] arg;
        endmodule
        module p_out(o[3:2]);
            output [3:0] o;
            assign o = 4'b1001;
        endmodule
        module p_outv(r[2:1]);
            output reg [3:0] r;
            initial r = 4'b0100;
        endmodule
        module p_mix(arg[3:2]);
            input [3:0] arg;
            assign arg[1:0] = 2'b01;
        endmodule
        module top;
            reg  [1:0] a = 2'b10;
            reg        b = 1'b1;
            reg  [1:0] c = 2'b01;
            wire [1:0] wo;
            wire [3:0] wo4;
            wire [1:0] wv;
            p_part u1(a);
            p_bit  u2(b);
            p_expl u3(.x(a));
            p_two  u4(a, c);
            p_out  u5(wo);
            p_out  u6(wo4);
            p_outv u7(wv);
            p_mix  u8(a);
            initial #1 $display("%b %b %b %b %b %b %b %b", u1.arg, u2.arg, u3.o, u4.arg, wo, wo4, wv, u8.arg);
        endmodule
    )");
    CHECK(out == "10zz z1z 10zz 1001 10 0010 10 1001\n");
}

// The select port's bits drive the net like any other partial driver, so they
// resolve against another driver of the same bits (here a hierarchical `assign`
// from the parent; one made by the module itself is a port coercion, LRM 23.3.3.1):
// the connection (1z) and the assign (z1) fold to 11, and the bits no driver reaches
// stay z.
TEST_CASE("a select port resolves against another driver of the same bits") {
    const std::string out = run_capture(R"(
        module p_res(arg[1:0]);
            input [3:0] arg;
        endmodule
        module top;
            reg [1:0] a = 2'b1z;
            p_res u(a);
            assign u.arg[1:0] = 2'bz1;
            initial #1 $display("%b", u.arg);
        endmodule
    )");
    CHECK(out == "zz11\n");
}
