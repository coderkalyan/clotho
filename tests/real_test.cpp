#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

// End-to-end real / shortreal semantics (LRM 6.12, 11.x, 21.2). Every golden
// string here is cross-checked byte-for-byte against iverilog 12 unless a
// comment says otherwise.

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

// Expect the frontend (or slang) to reject `source` with `needle` in the
// diagnostics.
void expect_reject(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
}

} // namespace

TEST_CASE("real literals, parameters, and display formats") {
    const std::string out = run_capture(R"(
        module t;
            real r;
            shortreal s;
            parameter real PI = 3.14159;
            parameter Q = 2.5; // untyped real parameter (LRM 6.20)
            initial begin
                r = 1.5;
                s = 0.25;
                $display("%g %g", r, s);
                $display("%g", PI);
                $display("%g", Q);
                $display("%f", 1.0 / 3.0);
                $display(2.0 ** -1);      // bare real: %#g, matching iverilog
                $display("%8.3f|", PI);   // C width/precision passthrough
                $display("%-8.2e|", 12345.0);
                $display("%e", 1e-3);
            end
        endmodule
    )");
    CHECK(out == "1.5 0.25\n3.14159\n2.5\n0.333333\n0.500000\n   3.142|\n1.23e+04|\n1.000000e-03\n");
}

TEST_CASE("real arithmetic and the LRM 11.4.3 power corners") {
    const std::string out = run_capture(R"(
        module t;
            real r;
            int  j;
            initial begin
                j = 2.0 ** -3'sb1 * 1000; // 0.5 * 1000
                $display("%0d", j);
                r = 9 ** 0.5;      $display("%g", r); // 3: int base converts to real
                r = 9.0 ** (1/2);  $display("%g", r); // 1: integer division truncates
                r = -3.0 ** 2.0;   $display("%g", r); // 9
                r = 7.5 + 2;       $display("%g", r); // mixed int/real converts
                r = -r;            $display("%g", r);
            end
        endmodule
    )");
    CHECK(out == "500\n3\n1\n9\n9.5\n-9.5\n");
}

TEST_CASE("real to integral conversion rounds half away from zero") {
    // LRM 6.12.1: assignment / cast conversion rounds; x/z bits of an
    // integral -> real conversion read as 0.
    const std::string out = run_capture(R"(
        module t;
            integer   i;
            bit [7:0] b8;
            real      r;
            initial begin
                i = 2.5;   $display("%0d", i);  // 3
                i = -1.5;  $display("%0d", i);  // -2
                i = 35.2;  $display("%0d", i);  // 35
                b8 = -1.5; $display("%0d", b8); // 254: wraps at the target width
                r = 4'b10x1; $display("%g", r); // 9: x reads as 0
            end
        endmodule
    )");
    CHECK(out == "3\n-2\n35\n254\n9\n");
}

TEST_CASE("real comparisons and logical contexts are definite") {
    const std::string out = run_capture(R"(
        module t;
            real r;
            initial begin
                r = 0.1 + 0.2;
                $display("%0d", r == 0.3); // 0: binary floating point
                $display("%0d", r < 0.31); // 1
                $display("%0d", 2 > 1.5);  // 1: int operand converts to real
                if (r) $display("truthy");
                if (!(0.0)) $display("zero is false");
                $display("%0d", r && 1.0);
                $display("%0d", 0.0 || 0.0);
            end
        endmodule
    )");
    CHECK(out == "0\n1\n1\ntruthy\nzero is false\n1\n0\n");
}

TEST_CASE("real ?: follows the LRM 11.4.11 ambiguous-condition rule") {
    const std::string out = run_capture(R"(
        module t;
            real r;
            initial begin
                r = 1.5;
                $display("%g", r == 1.5 ? 10.0 : 20.0);
                $display("%g", (1'bx) ? 3.0 : 5.0); // unequal arms -> 0.0
                $display("%g", (1'bx) ? 4.0 : 4.0); // equal arms -> that value
            end
        endmodule
    )");
    CHECK(out == "10\n0\n4\n");
}

TEST_CASE("real signals: sensitivity, NBA, continuous assign, ports, arrays") {
    const std::string out = run_capture(R"(
        module sub(input var real in, output var real out);
            assign out = in * 2.0; // continuous assign to a real variable (LRM 6.5)
        endmodule

        module t;
            real r, cr;
            shortreal s;
            real arr [0:3];
            sub u(.in(r), .out(cr));

            always @(r) $display("r changed: %g", r); // any-change on a real

            initial begin
                #1 r = 1.25;
                #1 r <= 7.5;   // NBA to a real
                #1;
                $display("cont: %g", cr);
                s = 0.1;
                $display("mix: %g", r + s); // shortreal promotes to real
                arr[2] = 6.75;
                $display("arr: %g", arr[2]);
            end
        endmodule
    )");
    CHECK(out == "r changed: 1.25\nr changed: 7.5\ncont: 15\nmix: 7.6\narr: 6.75\n");
}

TEST_CASE("reals default to 0.0, not X") {
    // LRM 6.8 Table 6-7: real / shortreal power up at 0.0.
    const std::string out = run_capture(R"(
        module t;
            real r;
            shortreal s;
            initial $display("%g %g", r, s);
        endmodule
    )");
    CHECK(out == "0 0\n");
}

TEST_CASE("%d of a real rounds; %e/%f/%g of an integral converts") {
    const std::string out = run_capture(R"(
        module t;
            initial begin
                $display("%0d", 4.5);  // rounds half away from zero
                $display("%0d", -4.5);
                $display("%g", 7);     // integral under a real specifier
            end
        endmodule
    )");
    CHECK(out == "5\n-5\n7\n");
}

TEST_CASE("real ++/-- steps by 1.0 in for loops") {
    // LRM 11.4.2: increment / decrement on a real adds / subtracts 1.0.
    const std::string out = run_capture(R"(
        module t;
            real r;
            initial begin
                for (r = 0.25; r < 3.0; r++)
                    $display("%g", r);
            end
        endmodule
    )");
    CHECK(out == "0.25\n1.25\n2.25\n");
}

// -- Rejections: illegal or unimplemented constructs fail loudly --

TEST_CASE("operators illegal on reals are rejected") {
    // Table 11-1: %, bitwise, shifts, ===, ==?, concat, reduction are illegal
    // on real operands; slang rejects them during elaboration.
    expect_reject("module m; real r; initial r = r % 2.0; endmodule", "real");
    expect_reject("module m; real r; initial $display(r === 1.0); endmodule", "real");
    expect_reject("module m; real r; initial $display({r, r}); endmodule", "concatenat");
    expect_reject("module m; real r; initial $display(r << 1); endmodule", "real");
    expect_reject("module m; real r; initial $display(~r); endmodule", "real");
    expect_reject("module m; real r; initial $display(&r); endmodule", "real");
    expect_reject("module m; real r; initial $display(r[0]); endmodule", "");
}

TEST_CASE("real nets, edge events, and casts slang rejects") {
    // 6.7.1: built-in net types cannot carry real.
    expect_reject("module m; wire real w; endmodule", "");
    // 6.12 / 9.4.2: no edge event controls on reals.
    expect_reject("module m; real r; always @(posedge r) ; endmodule", "");
    // 6.24.1: size / sign casts require an integral operand.
    expect_reject("module m; real r; int i; initial i = signed'(r); endmodule", "");
}

TEST_CASE("case on a real selector is rejected loudly") {
    expect_reject(R"(
        module m;
            real r;
            initial case (r) 1.0: ; default: ; endcase
        endmodule
    )", "case on a real selector is not supported");
}

TEST_CASE("real delays are rejected loudly") {
    expect_reject("module m; initial #1.5 $display(1); endmodule",
                  "real (fractional) delay values are not supported");
}

TEST_CASE("%h of a real is rejected loudly") {
    expect_reject(R"(module m; initial $display("%h", 1.5); endmodule)",
                  "%h with a real argument is not supported");
}

TEST_CASE("LRM 20.5 conversion system functions") {
    const std::string out = run_capture(R"(
        module t;
            real r;
            integer i;
            reg [63:0] b64;
            shortreal s;
            reg [31:0] b32;
            initial begin
                i = $rtoi(123.45);   $display("%0d", i); // truncates
                i = $rtoi(-15.7);    $display("%0d", i); // -15: toward zero
                i = int'(-15.7);     $display("%0d", i); // -16: cast rounds (6.12.1)
                r = $itor(123);      $display("%g", r);
                b64 = $realtobits(1.0); $display("%0d", b64);
                r = $bitstoreal(b64);   $display("%g", r);
                s = 1.5;
                b32 = $shortrealtobits(s); $display("%h", b32);
                s = $bitstoshortreal(32'h3fc00000); $display("%g", s);
                r = $bitstoreal($realtobits(3.14159)); $display("%g", r);
            end
        endmodule
    )");
    CHECK(out == "123\n-15\n-16\n123\n4607182418800017408\n1\n3fc00000\n1.5\n3.14159\n");
}

TEST_CASE("LRM 20.8.2 real math functions match C libm") {
    const std::string out = run_capture(R"(
        module t;
            real r;
            initial begin
                $display("%g", $sqrt(9.0));
                $display("%g", $ln($exp(2.0)));
                $display("%g", $log10(1000.0));
                $display("%g", $pow(2.0, 10.0));
                $display("%g", $floor(2.9));
                $display("%g", $ceil(2.1));
                $display("%g", $cos(0.0));
                $display("%g", $atan2(0.0, 1.0));
                $display("%g", $hypot(3.0, 4.0));
                $display("%g", $cosh(0.0));
                // Pure-libm entries (no LLVM intrinsic): symbol-resolution risk.
                $display("%g", $tan(0.0));
                $display("%g", $asin(1.0) == $acos(-1.0) / 2.0);
                $display("%g", $tanh(0.0));
                $display("%g", $asinh(0.0));
                $display("%g", $acosh(1.0));
                $display("%g", $atanh(0.0));
                $display("%g", $sqrt(2) * $sqrt(2) > 1.99); // integral arg converts
                r = 16;
                $display("%g", $sqrt(r)); // non-constant argument
            end
        endmodule
    )");
    CHECK(out == "3\n2\n3\n1024\n2\n3\n1\n0\n5\n1\n0\n1\n0\n0\n0\n0\n1\n4\n");
}

TEST_CASE("$realtime keeps the sub-unit fraction and %t accepts reals") {
    // The fast child pins the wheel tick at 1ns; the top's unit is 10ns, so a
    // 16ns event reads as 1.6 units from $realtime while $time rounds to 2
    // (LRM 20.3.1 / 20.3.3).
    const std::string out = run_capture(R"(
        `timescale 1ns/1ns
        module fast(output reg fire);
            initial begin fire = 0; #16 fire = 1; end
        endmodule
        `timescale 10ns/1ns
        module t;
            wire fire;
            fast f(fire);
            initial begin
                @(posedge fire);
                $display("%g", $realtime);
                $display("%0d", $time);
                $timeformat(-9, 2, " ns", 10);
                $display("%t", $realtime);
                $display("%t", 1.5); // a raw real scales in the real domain
            end
        endmodule
    )");
    CHECK(out == "1.6\n2\n  16.00 ns\n  15.00 ns\n");
}

TEST_CASE("$realtime inside a function is rejected loudly") {
    expect_reject(R"(
        module m;
            function real f(); return $realtime; endfunction
            initial $display("%g", f());
        endmodule
    )", "$realtime inside a function is not supported");
}
