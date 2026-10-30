#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// Unbased unsized literals '0/'1/'x/'z (LRM 5.7.1): the single bit fills the
// whole context-determined width. Expectations cross-checked against Icarus
// Verilog.

namespace {

// JIT-run a module and capture stdout (mirrors display_test.cpp).
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

// Each literal fills the target's width (not a zero-extended single bit), and
// 'x / 'z occupy distinct planes.
TEST_CASE("unbased unsized literals fill the assignment width") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0] a1, a0, ax, az;
            initial begin
                a1 = '1; a0 = '0; ax = 'x; az = 'z;
                $display("%b %b %b %b", a1, a0, ax, az);
            end
        endmodule
    )") == "1111 0000 xxxx zzzz\n");
}

// Wide (>64-bit) target exercises svint_to_ref's general packed path: '1 must
// fill all 96 bits, not truncate.
TEST_CASE("unbased unsized '1 fills a wide vector") {
    CHECK(run_capture(R"(
        module m;
            logic [95:0] w;
            initial begin
                w = '1;
                $display("%h", w);
            end
        endmodule
    )") == "ffffffffffffffffffffffff\n");
}

// The width is taken from the surrounding operator/context, not self-determined.
TEST_CASE("unbased unsized literals take their width from context") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] x;
            initial begin
                x = 8'h0f;
                $display("%h %h", x & '1, x | '0);
            end
        endmodule
    )") == "0f 0f\n");
}

// Signed context: '1 fills every bit, so a signed int reads -1 (LRM 5.7.1 with
// the sign of the context type).
TEST_CASE("unbased unsized '1 in a signed context is all-ones") {
    CHECK(run_capture(R"(
        module m;
            int s;
            initial begin
                s = '1;
                $display("%0d", s);
            end
        endmodule
    )") == "-1\n");
}

// Comparison context: the literal sizes to the other operand. 4'b1010 != '1,
// and a definite value !== 'x.
TEST_CASE("unbased unsized literals in comparison contexts") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0] v;
            initial begin
                v = 4'b1010;
                $display("%b %b", v == '1, v === 'x);
            end
        endmodule
    )") == "0 0\n");
}

// Based unsized literals whose high-order digit is x or z (LRM 5.7.1): "Unsized
// unsigned literal constants where the high-order bit is unknown (X or x) or
// high-impedance (Z or z) shall be extended to the size of the expression
// containing the literal constant", with x or z rather than zeros. Without that
// the literal stops at 32 bits and the rest of a wider context reads 0. slang
// owns the extension (patches/slang-unsized-xz-extend.patch), for the AST Clotho
// lowers and for every value it folds. Expectations are the LRM text; Icarus
// Verilog 13 agrees except where noted.

// LRM 5.7.1 Example 4, whose 85-bit targets are past the 32-bit boundary.
TEST_CASE("LRM 5.7.1 example 4: automatic left padding of literal numbers") {
    CHECK(run_capture(R"(
        module m;
            logic [11:0] a, b, c, d;
            logic [84:0] e, f, g;
            initial begin
                a = 'h x;
                b = 'h 3x;
                c = 'h z3;
                d = 'h 0z3;
                e = 'h5;
                f = 'hx;
                g = 'hz;
                $display("%h %h %h %h", a, b, c, d);
                $display("%h", e);
                $display("%h", f);
                $display("%h", g);
            end
        endmodule
    )") == "xxx 03x zz3 0z3\n"
           "0000000000000000000005\n"
           "xxxxxxxxxxxxxxxxxxxxxx\n"
           "zzzzzzzzzzzzzzzzzzzzzz\n");
}

// The pad is x/z only for a literal with no size. A sized literal is exactly its
// size wide, so 8'bx into a wider target pads with zeros like any narrower
// unsigned value (LRM 5.7.1 / 10.7), and a 2-state target flattens x/z to 0.
TEST_CASE("unsized based x/z literal pads wide 4-state targets only") {
    CHECK(run_capture(R"(
        module m;
            logic [63:0] a, b, c, d, sized_x, sized_z;
            logic [99:0] w;
            bit   [63:0] two;
            logic signed [63:0] sg;
            initial begin
                a = 'bx;
                b = 'bz;
                c = 'hx3x2x1x0;
                d = 'hx3;
                w = 'o?;
                two = 'hx3;
                sg = 'shx;
                sized_x = 8'bx;
                sized_z = 8'hz3;
                $display("%h %h %h %h", a, b, c, d);
                $display("%h", w);
                $display("%h %h", two, sg);
                $display("%h %h", sized_x, sized_z);
            end
        endmodule
    )") == "xxxxxxxxxxxxxxxx zzzzzzzzzzzzzzzz xxxxxxxxx3x2x1x0 xxxxxxxxxxxxxxx3\n"
           "zzzzzzzzzzzzzzzzzzzzzzzzz\n"
           "0000000000000003 xxxxxxxxxxxxxxxx\n"
           "00000000000000xx 00000000000000z3\n");
}

// The literal takes the size of the expression around it (LRM 11.8.1): the other
// operand of a comparison or bitwise operator, the arms of `?:`, a cast, a call
// argument. Self-determined it is still 32 bits.
TEST_CASE("unsized based x/z literal takes the size of its expression") {
    CHECK(run_capture(R"(
        module m;
            logic [63:0] v = 64'bz;
            function automatic logic [63:0] id(input logic [63:0] a);
                return a;
            endfunction
            initial begin
                $display("%b %b %b", 64'bz === 'bz, 64'bx === 'bx, v === 'bz);
                $display("%h", 'hx3 | 64'h0);
                $display("%h", 1'b1 ? 'hz : 40'h0);
                $display("%h", 64'('hx3));
                $display("%h", id('bx));
                $display("%h", 'bx);
            end
        endmodule
    )") == "1 1 1\n"
           "xxxxxxxxxxxxxxx3\n"
           "zzzzzzzzzz\n"
           "xxxxxxxxxxxxxxx3\n"
           "xxxxxxxxxxxxxxxx\n"
           "xxxxxxxx\n");
}

// The idiom the bug broke: a disabled tri-state driver on a bus wider than 32
// bits drove its upper half to 0 and fought the other driver.
TEST_CASE("tri-state driver 'bz on a 64-bit bus is z in every bit") {
    CHECK(run_capture(R"(
        module m;
            logic en1 = 0, en2 = 1;
            wire [63:0] bus, lone;
            assign bus  = en1 ? 64'h1 : 'bz;
            assign bus  = en2 ? 64'hFFFF_FFFF_FFFF_FFFF : 'bz;
            assign lone = en1 ? 64'h1 : 'bz;
            initial begin
                #1 $display("%h %h", bus, lone);
                en2 = 0;
                #1 $display("%h %h", bus, lone);
                en1 = 1;
                #1 $display("%h %h", bus, lone);
            end
        endmodule
    )") == "ffffffffffffffff zzzzzzzzzzzzzzzz\n"
           "zzzzzzzzzzzzzzzz zzzzzzzzzzzzzzzz\n"
           "0000000000000001 0000000000000001\n");
}

// Values slang folds at elaboration take the same path: a localparam, an enum
// member (LRM 6.19: its value expression "is evaluated in the context of a cast
// to the enum base type"; Icarus Verilog 13 zero-pads it, against the LRM), a
// constant function's result, and an instance's parameter default and override.
TEST_CASE("unsized based x/z literal extends in elaboration-time constants") {
    CHECK(run_capture(R"(
        module sub #(parameter int ID = 1, parameter logic [99:0] PP = 'bz) ();
            initial #ID $display("sub%0d %h", ID, PP);
        endmodule
        module m;
            localparam logic [63:0] P = 'hx;
            localparam logic [63:0] Q = 'hx3x2x1x0;
            typedef enum logic [39:0] { EX = 'hx, EZ = 'hz } e_t;
            function automatic logic [63:0] f();
                return 'bx;
            endfunction
            localparam logic [63:0] F = f();
            sub #(.ID(1), .PP('hx5)) s1 ();
            sub #(.ID(2)) s2 ();
            initial begin
                $display("%h %h", P, Q);
                $display("%h %h %h", EX, EZ, F);
            end
        endmodule
    )") == "xxxxxxxxxxxxxxxx xxxxxxxxx3x2x1x0\n"
           "xxxxxxxxxx zzzzzzzzzz xxxxxxxxxxxxxxxx\n"
           "sub1 xxxxxxxxxxxxxxxxxxxxxxxx5\n"
           "sub2 zzzzzzzzzzzzzzzzzzzzzzzzz\n");
}

// A cast to an enum "shall cause a conversion of the expression to its base type"
// (LRM 6.19.4), and a static cast returns "the value that a variable of the
// casting type would hold after being assigned the expression" (6.24.1), so the
// operand is sized by the base type like any assignment's right-hand side: an
// unsized x or z literal fills it (5.7.1), `'1` fills it, a sum keeps its carry,
// and a wider value truncates. slang did that for every cast but one to an enum
// (patches/slang-enum-cast-context.patch). Icarus Verilog 13 agrees.
TEST_CASE("a cast to an enum sizes its operand by the enum's base type") {
    CHECK(run_capture(R"(
        module m;
            typedef enum logic [39:0] { E0, E1 } e40_t;
            typedef enum logic [2:0] { A, B, C } e3_t;
            typedef logic [39:0] l40_t;
            logic [1:0] x = 2'd3;
            initial begin
                $display("%h %h %h", e40_t'('hx), e40_t'('hz3), l40_t'('hx));
                $display("%0d %0d %0d", e3_t'('1), e3_t'(x + 2'd1), e3_t'(4'd9));
            end
        endmodule
    )") == "xxxxxxxxxx zzzzzzzzz3 xxxxxxxxxx\n"
           "7 4 1\n");
}

// A port connection is an assignment-like context (LRM 10.8), so an unsized x or z
// literal connected to a port wider than 32 bits fills the port. Icarus Verilog 13
// is not followed: it warns "Padding 68 high bits of the port" and zero-pads them.
TEST_CASE("an unsized x or z literal connected to a wide port fills the port") {
    CHECK(run_capture(R"(
        module sub(input logic [99:0] in, output logic [99:0] o);
            assign o = in;
        endmodule
        module m;
            logic [99:0] o1, o2;
            sub s1(.in('bx), .o(o1));
            sub s2(.in('hz), .o(o2));
            initial #1 $display("%h %h", o1, o2);
        endmodule
    )") == "xxxxxxxxxxxxxxxxxxxxxxxxx zzzzzzzzzzzzzzzzzzzzzzzzz\n");
}

// An untyped parameter whose value is unsized takes an implementation-dependent
// width of at least 32 bits (LRM 6.20.2), although `'1` alone is one bit wide in
// a self-determined context (5.7.1). docs/LRM_NOTES.md note 45.
TEST_CASE("an untyped parameter set to '1 is at least 32 bits of ones") {
    const std::string out = run_capture(R"(
        module m;
            parameter P = '1;
            initial $display("%0d %h", $bits(P), P);
        endmodule
    )");
    CHECK(out == "32 ffffffff\n");
}
