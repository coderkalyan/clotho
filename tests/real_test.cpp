#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// End-to-end real / shortreal semantics (LRM 6.12, 11.x, 21.2). Every golden
// string here is cross-checked byte-for-byte against iverilog 12 unless a
// comment says otherwise.

namespace {

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

// Expect the frontend (or slang) to reject `source` with `needle` in the
// diagnostics.
void expect_reject(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
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

TEST_CASE("real to integral conversion is exact at the target's width and signedness (LRM 6.12.1)") {
    // The real is rounded to the nearest integer, ties away from zero, and that
    // integer is assigned to the target: a value the target holds is exact (an
    // unsigned 64-bit above 2**63, an 80-bit signed), and one it does not keeps
    // its low bits (10.7). Expected values are the exact integers (iverilog gives
    // the same for the in-range ones).
    const std::string out = run_capture(R"(
        module t;
            real                r;
            longint unsigned    u64;
            longint             s64;
            logic [63:0]        l64;
            logic [79:0]        l80;
            logic signed [79:0] s80;
            integer             i32;
            initial begin
                r = 1.5e19;  u64 = r;  $display("%0d", u64);
                r = 9223372036854775808.0; u64 = r; $display("%0d", u64);
                r = 1e20;    l64 = r;  $display("%0d", l64);
                l80 = r;               $display("%0d", l80);
                r = -1e20;   s80 = r;  $display("%0d", s80);
                l80 = r;               $display("%0d", l80);
                r = -1e19;   s64 = r;  $display("%0d", s64);
                i32 = $rtoi(r * 10.0); $display("%0d", i32);
                r = 2.5;     s64 = r;  $display("%0d", s64);
                r = -2.5;    s80 = r;  $display("%0d", s80);
            end
        endmodule
    )");
    CHECK(out ==
          "15000000000000000000\n"
          "9223372036854775808\n"
          "7766279631452241920\n"
          "100000000000000000000\n"
          "-100000000000000000000\n"
          "1208825819614629174706176\n"
          "8446744073709551616\n"
          "-1661992960\n"
          "3\n"
          "-3\n");
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

// A mixed real/integral `?:` used to reach the backend's integer-plane depth
// guard ("internal: integer plane of a real value") whenever one arm was not
// speculatable, because the lazy path's PHI merge only knew integer planes and
// string handles. iverilog 13 and Verilator 5.050 agree on all three lines.
TEST_CASE("a mixed real / integral ?: yields a real (LRM 11.4.11)") {
    const std::string out = run_capture(R"(
        module t;
            time et; real r; int i;
            initial begin
                i = 0;
                et = (i == 0) ? $realtime : et; // real arm + integral arm
                $display("et=%0t", et);
                r = (i == 0) ? 1.5 : i;         // real + int
                $display("r=%0g", r);
                r = (i == 1) ? i : 2.5;         // int + real
                $display("r2=%0g", r);
            end
        endmodule
    )");
    CHECK(out == "et=0\nr=1.5\nr2=2.5\n");
}

// LRM 11.4.11: "If both expressions are real, then the resulting type is real.
// If one expression is real and the other expression is shortreal or integral,
// the other expression is cast to real, and the resulting type is real. If one
// expression is shortreal and the other expression is integral, the integral
// expression is cast to shortreal, and the resulting type is shortreal."
// Every row runs twice: with two speculation-safe arms (the branchless packed
// select) and with a function-call arm, which `is_speculation_safe` refuses and
// so routes through `lower_lazy_conditional`'s SelectLazy merge. The last row
// pins the RESULT TYPE rather than the value: `(c ? 1 : 2.5) / 2` is 0.5 only
// if the chosen integral arm became 1.0, and 0 if it stayed an int.
TEST_CASE("the LRM 11.4.11 real promotion matrix over ?: arms") {
    const std::string out = run_capture(R"(
        module t;
            real r; shortreal s; time tm; longint li; int i; logic [3:0] q; bit c;
            function automatic real      fr(); return 7.25; endfunction
            function automatic shortreal fs(); return 7.25; endfunction
            function automatic int       fi(); return 3;    endfunction
            initial begin
                c = 1; i = 3; li = -5; tm = 40; q = 4'b10x1; s = 1.25;
                $display("%g %g", c ? 1.5 : i,   c ? i : 2.5);
                $display("%g %g", c ? 1.5 : s,   c ? s : 1.5);
                $display("%g %g", c ? s : i,     c ? i : s);
                $display("%g %g", c ? 1.5 : 2.5, c ? shortreal'(1.5) : shortreal'(2.5));
                $display("%g %g", c ? 1.5 : tm,  c ? 1.5 : li);
                $display("%g %g", c ? q : 1.5,   (i == 0) ? 1.5 : q); // x/z bits read as 0
                $display("%g %g", c ? fr() : i,  c ? fi() : 2.5);
                $display("%g %g", c ? fr() : shortreal'(2.5), c ? fs() : i);
                $display("%g %g", c ? fi() : fs(), c ? fr() : 2.5);
                $display("%g %g", c ? fr() : tm, c ? fr() : q);
                $display("%g", (i == 0) ? fr() : tm);
                $display("%g %g %g", (c ? 1 : 2.5) / 2, (c ? fi() : 2.5) / 2,
                                     (c ? 1 : shortreal'(2.5)) / 2);
            end
        endmodule
    )");
    CHECK(out == "1.5 3\n1.5 1.25\n1.25 3\n1.5 1.5\n1.5 1.5\n9 9\n7.25 3\n7.25 7.25\n"
                 "3 7.25\n7.25 7.25\n40\n0.5 1.5 0.5\n");
}

// The ambiguous-condition rule sees the PROMOTED arms, so `(1'bx) ? 3.0 : i`
// with i == 3 compares 3.0 against 3.0 and is logically equivalent. Table 11-20
// is defined for integral results only, so a non-equivalent real pair falls to
// the type default 0.0 — same reading as the string case, and what iverilog 13
// does. Verilator 5.050 instead returns the false arm here and is not an oracle
// for these three columns.
TEST_CASE("the ambiguous ?: rule applies after the real promotion") {
    const std::string out = run_capture(R"(
        module t;
            int i;
            function automatic real fr(); return 7.25; endfunction
            initial begin
                i = 3;
                $display("%g %g", (1'bx) ? fr() : 7.25, (1'bx) ? fr() : 5.0);
                $display("%g %g", (1'bx) ? 3.0 : i,     (1'bx) ? fr() : i);
                $display("%g %g", (1'bz) ? i : 3.0,     (1'bx) ? fr() : fr());
            end
        endmodule
    )");
    CHECK(out == "7.25 0\n3 0\n3 7.25\n");
}

// Accellera UVM `uvm_transaction::end_tr` (uvm_transaction.svh:829):
//     this.end_time = (end_time == 0) ? $realtime : end_time;
// $realtime is real and end_time is a `time`, so 11.4.11 makes the conditional
// real and the store back into `time` is an ordinary 6.12.1 conversion — which
// ROUNDS, not truncates, and rounds a half away from zero. Checked at nonzero,
// fractional simulation times so the conversion is not trivially exact.
// Byte-checked against iverilog 13.
TEST_CASE("a time destination from `cond ? $realtime : time_var` rounds") {
    const std::string out = run_capture(R"(`timescale 1ns/1ps
        module t;
            time et;
            function automatic time end_tr(time end_time = 0);
                return (end_time == 0) ? $realtime : end_time;
            endfunction
            initial begin
                #1.5;
                et = end_tr();
                $display("frac  %0d", et);  // 1.5 -> 2
                et = end_tr(7);
                $display("given %0d", et);
                #0.75;
                et = 0;
                et = (et == 0) ? $realtime : et;
                $display("inline %0d", et); // 2.25 -> 2
                #0.5;
                et = 0;
                et = (et == 0) ? $realtime : et;
                $display("half  %0d", et);  // 2.75 -> 3
            end
        endmodule
    )");
    CHECK(out == "frac  2\ngiven 7\ninline 2\nhalf  3\n");
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

TEST_CASE("%h / %o / %b / %x of a real round like %d, then format in that base") {
    // Table 21-1 gives %d/%h/%x/%o/%b a common integral operand, and LRM
    // 21.2.1.2 converts a real argument to it by the LRM 6.12.1 rule: round
    // half away from zero (NOT truncation -- 3.7 is 4, not 3). Only the base
    // differs, so all four print the same rounded value. Oracle-matched
    // against iverilog.
    const std::string out = run_capture(R"(
        module t;
            initial begin
                $display("%0d %0h %0o %0b", 3.7, 3.7, 3.7, 3.7);
                $display("%0d %0h", -3.7, -3.7);
                $display("%0d %0h %0x %0X %0H", 1e12, 1e12, 1e12, 1e12, 1e12);
            end
        endmodule
    )");
    // The rounded integer is a signed 64-bit value, so a negative prints as
    // its two's complement in the non-decimal bases, exactly as %d's operand
    // would. %x/%X are clotho's hex aliases for %h.
    CHECK(out == "4 4 4 100\n"
                 "-4 fffffffffffffffc\n"
                 "1000000000000 e8d4a51000 e8d4a51000 e8d4a51000 e8d4a51000\n");
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

// A real `case` selector is supported: it matches by IEEE-754 equality
// (docs/LRM_NOTES.md, note 19), as the cases in case_test.cpp pin. What stays rejected is
// `casez` / `casex` over a real: 12.5.1's do-not-care positions are bit
// positions, and slang's binder refuses a non-integral selector for either
// keyword before clotho sees it. (iverilog 13 accepts it and treats it as a
// plain case; clotho follows slang's stricter reading, since a real has no bit
// to be a don't-care.)
TEST_CASE("casez / casex over a real is rejected") {
    expect_reject(R"(
        module m;
            real r;
            initial casez (r) 1.0: ; default: ; endcase
        endmodule
    )",
                  "invalid type 'real' for casez expression");
    expect_reject(R"(
        module m;
            real r;
            initial casex (r) 1.0: ; default: ; endcase
        endmodule
    )",
                  "invalid type 'real' for casex expression");
}

// Real (fractional) `#delay` (LRM 3.14 / 22.7): rounds to the module precision,
// then advances the wheel on the finer grid. Golden values cross-checked against
// iverilog 13 (1ns/1ps: constant and runtime real delays, a sub-precision delay
// that rounds to zero, and a half-ps that rounds away from zero).
TEST_CASE("real delays round to precision and advance time") {
    const std::string out = run_capture(R"(
        `timescale 1ns/1ps
        module m;
          real d;
          initial begin
            #1.5    $display("%0.4f", $realtime);
            #0.25   $display("%0.4f", $realtime);
            #1.005  $display("%0.4f", $realtime);
            #0.0004 $display("%0.4f", $realtime);
            d = 1.4995;
            #(d)    $display("%0.4f", $realtime);
          end
        endmodule
    )");
    CHECK(out == "1.5000\n1.7500\n2.7550\n2.7550\n4.2550\n");
}

// A real delay on a continuous assign (LRM 10.3.3) and an intra-assignment
// nonblocking delay (LRM 9.4.5) both round to precision and advance on the fine
// grid. Cross-checked against iverilog 13.
TEST_CASE("real delays on continuous and nonblocking assigns") {
    const std::string out = run_capture(R"(
        `timescale 1ns/1ps
        module m;
          logic [7:0] q; wire [7:0] w;
          assign #2.5 w = q;
          initial begin
            q = 8'hAA;
            q <= #1.5 8'h55;
            #1.0 $display("q=%h @%0.4f", q, $realtime);
            #1.0 $display("q=%h @%0.4f", q, $realtime);
            #5   $display("w=%h @%0.4f", w, $realtime);
          end
        endmodule
    )");
    CHECK(out == "q=aa @1.0000\nq=55 @2.0000\nw=55 @7.0000\n");
}

// Only the module carrying a real delay drops to its precision (1ps); an
// integer-only enclosing module keeps its coarse unit tick. Both times print
// exactly, confirming cross-timescale scaling onto the shared wheel.
TEST_CASE("real delay in a submodule, integer-only parent") {
    const std::string out = run_capture(R"(
        `timescale 1ns/1ps
        module sub;
          initial #1.5 $display("sub %0.4f", $realtime);
        endmodule
        module m;
          sub s();
          initial #3 $display("top %0.4f", $realtime);
        endmodule
    )");
    CHECK(out == "sub 1.5000\ntop 3.0000\n");
}

// A real delay (or %t time) whose tick count does not fit a 64-bit time cannot be
// represented, and the LRM does not say what it means. Clotho saturates it, so a
// huge delay never wraps into a small or negative one and fires early: a delay of
// 2**64 ns is 2**64 * 1000 ps, whose low 64 bits are 0, so it used to fire at
// once. Oracle: none; the expected output follows from that decision (no event
// fires before the finite ones at 15 and 35, and every unrepresentable time
// prints as the same saturated time of its own sign, a representable one does
// not; a negative time is negative, docs/LRM_NOTES.md note 40).
TEST_CASE("a real delay beyond 2**63 ticks saturates instead of wrapping into an early event") {
    const std::string out = run_capture(R"(
        `timescale 1ns/1ps
        module m;
          real big, neg, inf, zero, mid;
          string s1, s2, s3, s4;
          initial begin
            zero = 0.0;
            big  = 18446744073709551616.0;
            neg  = -18446744073709551616.0;
            inf  = 1.0 / zero;
            #5;
            fork
              begin #(big) $display("FAIL: delay 2**64 ns fired at %0t", $time); end
              begin #(neg) $display("FAIL: delay -2**64 ns fired at %0t", $time); end
              begin #(inf) $display("FAIL: delay +inf fired at %0t", $time); end
            join_none
            #10 $display("t=15");
            #20 $display("t=35");
            mid = 1.0e6;
            s1 = $sformatf("%0t", 1.0e30);
            s2 = $sformatf("%0t", big);
            s3 = $sformatf("%0t", neg);
            s4 = $sformatf("%0t", mid);
            $display("%0d %0d %0d", s1 == s2, s3 == $sformatf("-%s", s2), s4 == s1);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "t=15\nt=35\n1 1 0\n");
}

// The same when the design tick is finer than the delaying module's precision:
// a delay of N ps is N * 1000 fs, and N = 18446744073709552 makes that
// 2**64 + 384, which wrapped to a delay of 384 fs. The count that saturates is the
// final one, not the real before the multiply.
TEST_CASE("a real delay saturates after the precision-to-tick multiply, not before it") {
    const std::string out = run_capture(R"(
        `timescale 1ns/1ps
        module sub;
          real r;
          initial begin
            r = 18446744073709.552;
            #(r) $display("FAIL: sub delay fired");
          end
        endmodule
        `timescale 1fs/1fs
        module m;
          sub s();
          initial begin
            #1000 $display("t=1000 fs");
            $finish;
          end
        endmodule
    )");
    CHECK(out == "t=1000 fs\n");
}

// A shortreal delay (and a shortreal %t operand) used to fail the LLVM verifier:
// the real-to-ticks path scaled and range-tested a float against double constants
// (`fmul float, double`, `fcmp oge float, double`). A shortreal widens exactly to
// real (LRM 6.12.2), so the amount is converted first and then follows the real
// path. 1ns/1ps takes the unit-over-precision multiply; 1ns/1ns does not.
// Cross-checked against iverilog 13.
TEST_CASE("a shortreal delay and %t operand widen to real before scaling to ticks") {
    const std::string out = run_capture(R"(
        `timescale 1ns/1ps
        module m;
          shortreal s;
          logic [7:0] q;
          initial begin
            s = 1.5;
            #(s) $display("%0.4f", $realtime);
            s = 0.25;
            #(s) $display("%0.4f", $realtime);
            s = 2.5;
            $display("%0t", s);
            q = 8'hAA;
            s = 0.5;
            q <= #(s) 8'h55;
            #0.25 $display("q=%h @%0.4f", q, $realtime);
            #0.5  $display("q=%h @%0.4f", q, $realtime);
          end
        endmodule
    )");
    CHECK(out == "1.5000\n1.7500\n2500\nq=aa @2.0000\nq=55 @2.5000\n");

    const std::string same_unit = run_capture(R"(
        `timescale 1ns/1ns
        module m;
          shortreal s;
          initial begin
            s = 3.0;
            #(s) $display("%0.4f", $realtime);
            $display("%0t", s);
          end
        endmodule
    )");
    CHECK(same_unit == "3.0000\n3\n");
}

TEST_CASE("%c of a real is still rejected loudly") {
    // %h/%o/%b/%x of a real now round-and-format (see the base-conversion case
    // above), but %c has no defined real operand conversion in Table 21-1, so
    // it stays guarded rather than silently picking a character code.
    expect_reject(R"(module m; initial $display("%c", 1.5); endmodule)", "%c with a real argument is not supported");
}

// A scan destination is written as must/may plane words, and a real slot is an
// IEEE-754 double, so the parsed bits used to land in its mantissa: this
// returned 1 and left `r` reading 0.000000, with no diagnostic at all. %f/%e/%g
// (the clause's own real conversions) were already refused, so the integral
// spelling was the way in. Same guard, same reason, as $value$plusargs's.
TEST_CASE("a real $fscanf / $sscanf destination is rejected") {
    expect_reject(R"(module m; real r; initial void'($sscanf("12", "%d", r)); endmodule)",
                  "$fscanf / $sscanf into a real variable is not supported");
    expect_reject(R"(module m; shortreal s; initial void'($sscanf("12", "%h", s)); endmodule)",
                  "$fscanf / $sscanf into a real variable is not supported");
    // The integral destination beside it must still work, so the guard is not a
    // blanket refusal of the family.
    CHECK(run_capture(
            R"(module m; int n, a; initial begin n = $sscanf("12", "%d", a); $display("%0d %0d", n, a); end endmodule)")
          == "1 12\n");
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

// $realtime is readable inside a plain function: the wheel is a module global
// (clotho.wheel), not a body parameter, so a function reaches the clock exactly
// as a task does. The value is scaled to the unit of the scope that DECLARES the
// function (LRM 3.14.2.3) and keeps the sub-unit fraction $time rounds away.
// Cross-checked against iverilog.
TEST_CASE("$realtime inside a function keeps the sub-unit fraction") {
    const std::string out = run_capture(R"(`timescale 1ns/1ps
        module m;
            function automatic real rt(); return $realtime; endfunction
            initial begin
                #1.5 $display("%0.4f %0d", rt(), $time);
                #0.25 $display("%0.4f %0d", rt(), $time);
            end
        endmodule
    )");
    CHECK(out == "1.5000 2\n1.7500 2\n");
}

TEST_CASE("real function arguments, returns, and recursion") {
    const std::string out = run_capture(R"(
        module t;
            real g;

            function real avg(input real a, input real b);
                return (a + b) / 2.0;
            endfunction

            function real fact(input real n);
                if (n <= 1.0) return 1.0;
                return n * fact(n - 1.0);
            endfunction

            function shortreal halve(input shortreal x);
                return x / 2.0;
            endfunction

            function int roundit(input real r);
                return r; // implicit real -> int rounds (LRM 6.12.1)
            endfunction

            initial begin
                $display("%g", avg(1.5, 2.5));
                $display("%g", fact(5.0));
                $display("%g", halve(7.0));
                $display("%0d", roundit(2.5));
                $display("%g", avg(3, 4)); // int actuals convert at the call
                g = avg(g, 8.0);           // module real through a function
                $display("%g", g);
            end
        endmodule
    )");
    CHECK(out == "2\n120\n3.5\n3\n3.5\n4\n");
}

TEST_CASE("real task formals: input, output, and inout copy in and out") {
    const std::string out = run_capture(R"(
        module t;
            real o, a;
            task automatic scale(input real in, output real out, inout real acc);
                out = in * 3.0;
                acc = acc + in;
            endtask
            initial begin
                a = 10.0;
                scale(1.25, o, a);
                $display("%g %g", o, a);
            end
        endmodule
    )");
    CHECK(out == "3.75 11.25\n");
}

TEST_CASE("a real task formal survives a timing suspend") {
    // The formal's frame slot (its 64-bit pattern) must live across the
    // coroutine suspend and resume.
    const std::string out = run_capture(R"(
        module t;
            task automatic later(input real v);
                #2 $display("%g", v * 2.0);
            endtask
            initial later(1.75);
        endmodule
    )");
    CHECK(out == "3.5\n");
}
