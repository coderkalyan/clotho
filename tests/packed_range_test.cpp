// Packed ranges other than [N-1:0]: reversed ([0:7]), non-zero aligned
// ([15:8]), and negative ([-1:-8]) declared ranges, across constant and
// runtime bit selects, part selects, indexed part selects, and their lvalue
// forms. Every expected string was cross-checked against Icarus Verilog
// (iverilog v13) as the oracle.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and return everything it wrote to stdout.
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

TEST_CASE("non-[N-1:0] packed ranges: reads agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            logic [0:7]   big;
            logic [15:8]  mid;
            logic [-1:-8] neg;
            integer i;
            initial begin
                big = 8'hA5; mid = 8'hA5; neg = 8'hA5;
                $display("cbit %b%b %b%b %b%b", big[0], big[7], mid[15], mid[8], neg[-1], neg[-8]);
                for (i = 0; i < 8; i = i + 1) $display("dyn %0d %b %b", i, big[i], mid[i+8]);
                for (i = -8; i <= -1; i = i + 1) $display("dynneg %0d %b", i, neg[i]);
                $display("rng %h%h %h%h %h%h", big[0:3], big[4:7], mid[15:12], mid[11:8], neg[-1:-4], neg[-5:-8]);
                $display("ips %h %h %h", big[2 +: 4], mid[13 -: 4], neg[-6 +: 4]);
                i = 2; $display("dips %h %h", big[i +: 4], mid[i+8 +: 4]);
                i = -5; $display("dipsneg %h", neg[i +: 4]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "cbit 11 11 11\n"
          "dyn 0 1 1\ndyn 1 0 0\ndyn 2 1 1\ndyn 3 0 0\n"
          "dyn 4 0 0\ndyn 5 1 1\ndyn 6 0 0\ndyn 7 1 1\n"
          "dynneg -8 1\ndynneg -7 0\ndynneg -6 1\ndynneg -5 0\n"
          "dynneg -4 0\ndynneg -3 1\ndynneg -2 0\ndynneg -1 1\n"
          "rng a5 a5 a5\n"
          "ips 9 9 9\n"
          "dips 9 9\n"
          "dipsneg 4\n");
}

TEST_CASE("non-[N-1:0] packed ranges: writes agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            logic [0:7]   big;
            logic [15:8]  mid;
            logic [-1:-8] neg;
            integer i;
            initial begin
                big = 8'h00; mid = 8'h00; neg = 8'h00;
                big[1] = 1'b1; mid[14] = 1'b1; neg[-2] = 1'b1;
                $display("wbit %h %h %h", big, mid, neg);
                big[2:5] = 4'hC; mid[13:10] = 4'hC; neg[-3:-6] = 4'hC;
                $display("wrng %h %h %h", big, mid, neg);
                i = 2;
                big[i +: 3] = 3'b101; mid[i+8 +: 3] = 3'b101;
                $display("wips %h %h", big, mid);
                i = -5; neg[i +: 3] = 3'b111;
                $display("wipsneg %h", neg);
                i = 5; big[i -: 3] = 3'b011;
                $display("wipsd %h", big);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "wbit 40 40 40\n"
          "wrng 70 70 70\n"
          "wips 68 74\n"
          "wipsneg 78\n"
          "wipsd 6c\n");
}

// Narrow indices exercise the offset widening: an unsigned index must
// zero-extend (7 stays 7) and a signed index must sign-extend (-3 stays -3)
// before the range-normalization subtraction.
TEST_CASE("narrow unsigned and signed runtime indices agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            logic [0:7]        big;
            logic [-1:-8]      neg;
            logic [2:0]        j;
            logic signed [3:0] sj;
            initial begin
                big = 8'hA5; neg = 8'h5A;
                j = 3'd6;   $display("nu %b", big[j]);
                j = 3'd7;   $display("nu2 %b", big[j]);
                sj = -4'sd3; $display("ns %b", neg[sj]);
                sj = -4'sd8; $display("ns2 %b", neg[sj]);
                sj = -4'sd6; $display("nips %h", neg[sj +: 3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "nu 0\nnu2 1\nns 0\nns2 0\nnips 6\n");
}

// LRM 11.5.1: a dynamic bit/part select out of the vector bounds reads x and
// writes nothing; a partially out-of-range select applies the rule bit-by-bit;
// an x/z index is wholly out of range. All lines cross-checked against iverilog.
TEST_CASE("out-of-bounds dynamic selects read x and write nothing (LRM 11.5.1)") {
    constexpr std::string_view src = R"(
        module t;
            logic [7:0] v;
            logic [3:0] p;
            logic       b;
            logic [3:0] xi;
            integer     i;
            initial begin
                // Reads: fully / partially out of range yield x.
                v = 8'hA5;
                i = 9;  b = v[i];      $display("%b", b);        // x
                i = -1; b = v[i];      $display("%b", b);        // x
                i = 6;  p = v[i +: 4]; $display("%b", p);        // xx10 (v[7:6]=10)
                i = 20; p = v[i +: 4]; $display("%b", p);        // xxxx
                i = -2; p = v[i +: 4]; $display("%b", p);        // 01xx (v[1:0]=01)
                // Writes: out-of-range bits are not modified.
                v = 8'hFF; i = 9;  v[i]      = 1'b0; $display("%h", v);  // ff
                v = 8'h00; i = 6;  v[i +: 4] = 4'hF; $display("%h", v);  // c0
                v = 8'h33; i = 20; v[i +: 4] = 4'hF; $display("%h", v);  // 33
                // An x/z index is wholly out of range: read x, write no-op.
                xi = 4'bxx01;
                v = 8'hA5; p = v[xi +: 4];         $display("%b", p);    // xxxx
                v = 8'h5A; v[xi +: 4] = 4'hF;      $display("%h", v);    // 5a
            end
        endmodule
    )";
    CHECK(run_capture(src) == "x\nx\nxx10\nxxxx\n01xx\nff\nc0\n33\nxxxx\n5a\n");
}

// The out-of-bounds rule also applies to nonblocking writes: the deferred merge
// must mask off the out-of-range bits. Cross-checked against iverilog.
TEST_CASE("out-of-bounds nonblocking writes affect only in-range bits") {
    constexpr std::string_view src = R"(
        module t;
            logic [7:0] v;
            integer     i;
            initial begin
                v = 8'hFF; i = 9;  v[i]      <= 1'b0;  #1 $display("%h", v);  // ff
                v = 8'h00; i = 6;  v[i +: 4] <= 4'hF;  #1 $display("%h", v);  // c0
                v = 8'h33; i = 20; v[i +: 4] <= 4'hF;  #1 $display("%h", v);  // 33
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ff\nc0\n33\n");
}

// Wide (>64-bit) constants built during lowering (repeat counters, ++ step,
// unary-minus zero) used to read out of bounds in InternPool::put_int and
// produce garbage planes. Cross-checked against iverilog.
TEST_CASE("wide-typed lowering constants: repeat, ++, unary minus") {
    constexpr std::string_view src = R"(
        module t;
            logic [95:0]  w;
            logic [127:0] v;
            integer n;
            initial begin
                w = 96'h3;
                n = 0;
                repeat (w) n = n + 1;
                $display("rep %0d", n);
                for (v = 128'hFFFF_FFFF_FFFF_FFFF_FFFF; v < 128'hFFFF_FFFF_FFFF_FFFF_FFFF + 3; v++)
                    $display("v %h %h", v[127:64], v[63:0]);
                v = 128'h1;
                v = -v;
                $display("neg %h %h", v[127:64], v[63:0]);
                w = -96'h5;
                $display("neg2 %h %h", w[95:64], w[63:0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "rep 3\n"
          "v 000000000000ffff ffffffffffffffff\n"
          "v 0000000000010000 0000000000000000\n"
          "v 0000000000010000 0000000000000001\n"
          "neg ffffffffffffffff ffffffffffffffff\n"
          "neg2 ffffffff fffffffffffffffb\n");
}

TEST_CASE("multi-dim packed array: element/range/nested selects (LRM 7.4.5/7.4.6)") {
    // iverilog crashes on a multi-dim packed select (elab_expr.cc assertion), so
    // the oracle here is Verilator 5.048; every string is also hand-derivable
    // from the flat bit layout (element k of width W occupies bits [k*W+W-1:k*W]).
    constexpr std::string_view src = R"(
        module t;
            logic [3:0][7:0]      a;   // 4 elements x 8 bits, descending
            logic [1:0][2:0][3:0] b;   // 3-D: 2 x 3 x 4
            logic [0:3][7:0]      c;   // ascending outer dim
            integer i;
            initial begin
                a = 32'hDEADBEEF;      // a[3]=DE a[2]=AD a[1]=BE a[0]=EF
                $display("e %h %h %h %h", a[0], a[1], a[2], a[3]);
                $display("nb %b %b", a[2][7], a[0][0]);          // nested bit
                $display("ps %h %h", a[1][7:4], a[3][3:0]);      // part of element
                $display("rs %h %h", a[2:1], a[3:2]);            // element range (16b)
                for (i = 0; i < 4; i = i + 1) $display("dyn %0d %h", i, a[i]);
                $display("ips %h %h", a[1 +: 2], a[3 -: 2]);     // indexed element range
                i = 9; $display("oor %h", a[i]);                 // out of range -> X
                a = 0;
                a[2] = 8'h5A; a[0][3:0] = 4'hF; a[3][7] = 1'b1; a[1:0] = 16'h1234;
                $display("wr %h", a);
                for (i = 0; i < 4; i = i + 1) a[i] = i[7:0] + 8'h10;
                $display("dw %h", a);
                b = 24'hABCDEF;        // b[1]=ABC b[0]=DEF (each 12 bits = 3x4)
                $display("d3 %h %h %h %b", b[0], b[1], b[1][2], b[0][0][3]);
                b[1][0] = 4'h7; $display("d3w %h", b);
                c = 32'h11223344;      // c[0]=11 c[1]=22 c[2]=33 c[3]=44
                $display("asc %h %h %h", c[0], c[3], c[1:2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "e ef be ad de\n"
          "nb 1 1\n"
          "ps b e\n"
          "rs adbe dead\n"
          "dyn 0 ef\n"
          "dyn 1 be\n"
          "dyn 2 ad\n"
          "dyn 3 de\n"
          "ips adbe dead\n"
          "oor xx\n"
          "wr 805a1234\n"
          "dw 13121110\n"
          "d3 def abc a 1\n"
          "d3w ab7def\n"
          "asc 11 44 2233\n");
}

// LRM 11.5.1: a write through a select "shall only affect the bits that are in
// range", so a constant part-select or packed-struct member stacked on a
// dynamic element select writes its own bits and leaves the rest of the element
// alone, blocking, nonblocking and in a function local alike. Oracle: the flat
// bit layout (element k of width W occupies bits [k*W+W-1:k*W]); iverilog
// rejects the nested spelling, and its flattened equivalent (`a[i*8 +: 4] = 0`)
// agrees on every line.
TEST_CASE("a constant part-select under a dynamic select writes only its bits (LRM 11.5.1)") {
    constexpr std::string_view src = R"(
        module t;
            typedef struct packed { logic [3:0] hi; logic [3:0] lo; } s_t;
            typedef struct packed { bit [3:0] hi; bit [3:0] lo; } bs_t;
            logic [3:0][7:0] a;
            bit   [3:0][7:0] b;
            logic [1:0][7:0] m [2];
            s_t   [3:0]      sa;
            bs_t  [3:0]      bsa;
            int i, k;
            function automatic logic [31:0] f(int k);
                logic [3:0][7:0] loc;
                loc = '1;
                loc[k][3:0] = 4'h0;
                return loc;
            endfunction
            function automatic bit [31:0] g(int k);
                bit [3:0][7:0] loc;
                loc = '1;
                loc[k][7:4] = 4'h0;
                return loc;
            endfunction
            initial begin
                a = '1;   i = 1; a[i][3:0] = 4'h0;   $display("%h %h", a, a[i][3:0]);
                a = '1;   i = 2; a[i][6:5] = 2'b00;  $display("%h", a);
                b = '1;   i = 3; b[i][7:4] = 4'h5;   $display("%h", b);
                sa = '1;  i = 2; sa[i].lo = 4'h0;    $display("%h", sa);
                bsa = '1; i = 0; bsa[i].hi = 4'h0;   $display("%h", bsa);
                $display("%h %h", f(2), g(1));
                a = '1; b = '1; i = 1;
                a[i][3:0] <= 4'h0;
                b[i][7:4] <= 4'h0;
                #1 $display("%h %h", a, b);
                // An element of a memory, and a delayed nonblocking write.
                m[0] = '1; m[1] = '1; k = 1; i = 0;
                m[k][i][3:0] = 4'h0;
                $display("%h %h", m[0], m[1]);
                m[0] = '1; m[1] = '1;
                m[k][i][7:4] <= 4'h0;
                #1 $display("%h %h", m[0], m[1]);
                a = '1; i = 1;
                a[i][3:0] <= #1 4'h0;
                #2 $display("%h", a);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "fffff0ff 0\n"
          "ff9fffff\n"
          "5fffffff\n"
          "fff0ffff\n"
          "ffffff0f\n"
          "fff0ffff ffff0fff\n"
          "fffff0ff ffff0fff\n"
          "ffff fff0\n"
          "ffff ff0f\n"
          "fffff0ff\n");
}

// LRM 11.5.1: a select is invalid when its address is out of bounds or has an x
// or z bit, and then reads x (0 for 2-state) and writes nothing. In a nested
// select `a[i][j]` every level is such an address, so an invalid OUTER index
// invalidates the whole access, whatever the inner index is. Oracle: the
// flattened spelling `a[i*8 + j]`, which iverilog accepts: for an x, a high or a
// negative `i` it reads x / xx and writes nothing, and for a valid one it sets
// bit 19 (00080000).
TEST_CASE("a nested select with an invalid outer index reads x and writes nothing (LRM 11.5.1)") {
    constexpr std::string_view src = R"(
        module t;
            logic [3:0][7:0]      a;
            bit   [3:0][7:0]      b;
            logic [1:0][3:0][1:0] c;
            logic [1:0] ix;
            int i, j, ii, jj, kk;
            initial begin
                // An x outer index.
                a = 32'h5A5A5A5A; b = 32'h5A5A5A5A; ix = 2'bx; j = 3;
                $display("%b %b %b %b", a[ix][j], a[ix][j +: 2], b[ix][j], b[ix][j +: 2]);
                a[ix][j] = 1'b0;  a[ix][j +: 2] = 2'b00;
                b[ix][j] = 1'b1;  b[ix][j +: 2] = 2'b11;
                $display("%h %h", a, b);
                a[ix][j] <= 1'b0;  a[ix][j +: 2] <= 2'b00;
                b[ix][j] <= 1'b1;  b[ix][j +: 2] <= 2'b11;
                #1 $display("%h %h", a, b);
                // An outer index past either end.
                a = 32'hFFFFFFFF; b = 32'h0;
                i = 4;  $display("%b %b %b", a[i][j], a[i][j +: 2], b[i][j]);
                i = -1; $display("%b %b %b", a[i][j], a[i][j +: 2], b[i][j]);
                i = 4;  a[i][j] = 1'b0; b[i][j] = 1'b1;
                i = -1; a[i][j] = 1'b0; b[i][j] = 1'b1;
                $display("%h %h", a, b);
                // A valid outer index is unaffected.
                a = 0; i = 2; a[i][j] = 1'b1;
                $display("%h %b %b", a, a[i][j], a[i][j +: 2]);
                // Three levels: the outermost one is the invalid one.
                c = '1; ii = 2; jj = 1; kk = 0;
                $display("%b", c[ii][jj][kk]);
                c[ii][jj][kk] = 1'b0;
                $display("%h", c);
                ii = 1; c[ii][jj][kk] = 1'b0;
                $display("%h", c);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "x xx 0 00\n"
          "5a5a5a5a 5a5a5a5a\n"
          "5a5a5a5a 5a5a5a5a\n"
          "x xx 0\n"
          "x xx 0\n"
          "ffffffff 00000000\n"
          "00080000 1 01\n"
          "x\n"
          "ffff\n"
          "fbff\n");
}

// LRM 11.5.1: an out-of-bounds index reads x (0 for 2-state) and writes nothing,
// so what is compared is the index VALUE, at its own width and signedness: a
// 64-bit index of 2**32 + 1 is not bit 1, and neither is one whose offset
// arithmetic (`i * 8`, `u - right`) lands in range only by wrapping 32 bits. A
// window that straddles the container keeps the bits that are in range. Oracle:
// the LRM text (iverilog truncates the index to 32 bits and wraps the stride, as
// Clotho did).
TEST_CASE("a packed select index is compared at its own width, not truncated or wrapped (LRM 11.5.1)") {
    constexpr std::string_view src = R"(
        module t;
            logic [7:0]      v;
            bit   [7:0]      bv;
            logic [15:8]     mid;
            logic [3:0][7:0] a;
            logic [-1:-8]    neg;
            longint unsigned lu;
            longint          ls;
            int              i;
            int unsigned     u;
            initial begin
                // A 64-bit index: 2**32 + 1 and -(2**32 - 1) are both 1 modulo 2**32.
                v = 8'hFF; bv = 8'hFF; lu = 64'h1_0000_0001; ls = -64'sd4294967295;
                $display("%b %b %b %b", v[lu], bv[lu], v[ls], bv[ls]);
                v[lu] = 1'b0; bv[ls] = 1'b0; v[ls +: 4] = 4'h0; bv[lu +: 4] = 4'h0;
                $display("%h %h", v, bv);
                $display("%b %b", v[lu +: 4], bv[ls +: 4]);
                v[lu] <= 1'b0; bv[ls +: 2] <= 2'b00;
                #1 $display("%h %h", v, bv);
                // In range through a 64-bit index, and windows that straddle or miss [15:8].
                lu = 4; ls = 14; mid = 8'hA5;
                $display("%b %b", v[lu], mid[ls]);
                ls = 6;  $display("%b", mid[ls +: 4]);
                ls = 8;  $display("%b", mid[ls +: 4]);
                ls = -1; $display("%b %b", mid[ls], mid[ls +: 4]);
                // 2**29 + 1 elements of 8 bits is 2**32 + 8 bits: not element 1.
                a = 32'hDDCCBBAA; i = 536870913;
                $display("%h", a[i]);
                a[i] = 8'h00; a[i] <= 8'h00;
                #1 $display("%h", a);
                // 4294967289 is not in [-1:-8], though 4294967289 + 8 wraps to 1.
                neg = 8'h5A; u = 32'hFFFF_FFF9;
                $display("%b", neg[u]);
                neg[u] = 1'b0;
                $display("%h", neg);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "x 0 x 0\n"
          "ff ff\n"
          "xxxx 0000\n"
          "ff ff\n"
          "1 0\n"
          "01xx\n"
          "0101\n"
          "x xxxx\n"
          "xx\n"
          "ddccbbaa\n"
          "x\n"
          "5a\n");
}

// LRM 11.5.1: a part-select that is partially out of range "shall, when read,
// return x for the bits that are out of range and shall, when written, only
// affect the bits that are in range"; a wholly out-of-range one reads x and
// changes nothing. 2-state reads give 0 for the missing bits. A constant window
// is checked against the vector it selects from, the element for `a[i][11:8]`.
// Oracle: the LRM text and the flat bit layout; iverilog accepts the vector
// forms and agrees on the reads (xxxx xx11 11xx) and on the writes (ff, bf, fd).
TEST_CASE("a constant part-select outside its vector reads x and writes only the bits inside (LRM 11.5.1)") {
    constexpr std::string_view src = R"(
        module t;
            logic [7:0]      v;
            bit   [7:0]      bv;
            logic [3:0][7:0] a;
            int              i;
            function automatic logic [7:0] f();
                logic [7:0] loc;
                loc = 8'hFF; loc[9:6] = 4'b1010;
                return loc;
            endfunction
            initial begin
                v = 8'hFF; bv = 8'hFF;
                $display("%b %b %b", v[11:8], v[9:6], v[1:-2]);
                $display("%b %b %b", bv[11:8], bv[9:6], bv[1:-2]);
                v[11:8] = 4'h0;
                $display("%h", v);
                v[9:6] = 4'b1010;
                $display("%h", v);
                v = 8'hFF; v[1:-2] = 4'b0110;
                $display("%h", v);
                bv[11:8] = 4'h0; bv[9:6] = 4'b1010;
                $display("%h", bv);
                v = 8'hFF; v[9:6] <= 4'b1010; v[11:8] <= 4'h0;
                #1 $display("%h", v);
                $display("%h", f());
                // An element index past the end, and a window past the end of its element.
                a = '1; i = 1;
                $display("%b %b", a[5], a[i][11:8]);
                a[5] = 8'h00; a[i][11:8] = 4'h0;
                $display("%h", a);
                a[i][9:6] = 4'b1010;
                $display("%h", a);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "xxxx xx11 11xx\n"
          "0000 0011 1100\n"
          "ff\n"
          "bf\n"
          "fd\n"
          "bf\n"
          "bf\n"
          "bf\n"
          "xxxxxxxx xxxx\n"
          "ffffffff\n"
          "ffffbfff\n");
}

// LRM 11.5.1: a bit-select whose address has an x or z bit is invalid: it reads
// x (0 for 2-state) and writes nothing, also on a vector one bit wide, where the
// select covers the whole storage. Oracle: iverilog (x, no-op).
TEST_CASE("a constant x index into a one-bit vector reads x and writes nothing (LRM 11.5.1)") {
    constexpr std::string_view src = R"(
        module t;
            localparam PX = 1'bx;
            localparam PZ = 1'bz;
            logic [0:0] v1;
            bit   [0:0] b1;
            logic [7:0] v8;
            initial begin
                v1 = 1'b1; b1 = 1'b1; v8 = 8'hFF;
                $display("%b %b %b %b", v1[PX], v1[PZ], b1[PX], v8[PX]);
                v1[PX] = 1'b0; b1[PZ] = 1'b0; v8[PX] = 1'b0;
                $display("%b %b %h", v1, b1, v8);
                v1[PX] <= 1'b0; b1[PX] <= 1'b0;
                #1 $display("%b %b", v1, b1);
                $display("%b %b", v1[0], b1[0]);
                v1[0] = 1'b0; $display("%b", v1);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "x x 0 x\n"
          "1 1 ff\n"
          "1 1\n"
          "1 1\n"
          "0\n");
}

TEST_CASE("array query functions (LRM 20.7): $size/$dimensions/$left/... fold to constants") {
    // All eight are constant type/bounds queries for a fixed-size operand, so
    // slang folds them; every string below matches iverilog v13 exactly (it
    // supports these on fixed arrays, unlike a multi-dim packed *select*).
    constexpr std::string_view src = R"(
        module t;
            logic [15:0]     v;
            logic [0:7]      asc;              // ascending, negative increment
            logic [3:0][7:0] a;                // packed 2-D
            logic [7:0]      mem [0:15];       // unpacked 1-D
            logic [7:0]      m2  [3:0][0:9];   // unpacked 2-D
            initial begin
                $display("v L%0d R%0d H%0d Lo%0d I%0d S%0d D%0d",
                         $left(v), $right(v), $high(v), $low(v), $increment(v), $size(v), $dimensions(v));
                $display("asc L%0d R%0d H%0d Lo%0d I%0d",
                         $left(asc), $right(asc), $high(asc), $low(asc), $increment(asc));
                $display("a S%0d D%0d bits%0d", $size(a), $dimensions(a), $bits(a));
                $display("a2 L%0d R%0d S%0d", $left(a, 2), $right(a, 2), $size(a, 2));
                $display("mem S%0d L%0d H%0d D%0d UD%0d",
                         $size(mem), $left(mem), $high(mem), $dimensions(mem), $unpacked_dimensions(mem));
                $display("m2 S%0d Sd2%0d D%0d UD%0d",
                         $size(m2), $size(m2, 2), $dimensions(m2), $unpacked_dimensions(m2));
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "v L15 R0 H15 Lo0 I1 S16 D1\n"
          "asc L0 R7 H7 Lo0 I-1\n"
          "a S4 D2 bits32\n"
          "a2 L7 R0 S8\n"
          "mem S16 L0 H15 D2 UD1\n"
          "m2 S4 Sd210 D3 UD2\n");
}

// $left/$right/$low/$high/$increment (LRM 20.7) do not fold for a runtime
// dynamic array or queue (slang's own ArrayQueryFunction::getDim always
// evaluates the operand's value, even for $left / $low / $increment, whose
// results never actually depend on it), so they reach Clotho's own runtime
// lowering below, alongside $size's existing path. $left / $low = 0 and
// $increment = -1 are LRM 20.7's own UNCONDITIONAL rule for a queue or
// dynamic array dimension -- not a runtime $left-vs-$right comparison, and
// not special-cased for an empty (or single-element) collection: neither is
// special, $increment stays -1. Only $right / $high (= size - 1) need the
// runtime size. Cross-checked against Verilator 5.050 (byte-identical).
TEST_CASE("array query functions (LRM 20.7): dynamic array / queue runtime path") {
    constexpr std::string_view src = R"(
        module t;
            int d[]; int q[$];
            function automatic int sum_bounds(int a[]);
                return $left(a) + $right(a) + $low(a) + $high(a) + $increment(a) + $size(a);
            endfunction
            initial begin
                d = new [5]; q = {1,2,3};
                $display("d: size=%0d left=%0d right=%0d low=%0d high=%0d inc=%0d dims=%0d ud=%0d",
                         $size(d), $left(d), $right(d), $low(d), $high(d), $increment(d), $dimensions(d),
                         $unpacked_dimensions(d));
                $display("q: size=%0d left=%0d right=%0d low=%0d high=%0d inc=%0d dims=%0d ud=%0d",
                         $size(q), $left(q), $right(q), $low(q), $high(q), $increment(q), $dimensions(q),
                         $unpacked_dimensions(q));
                d = new [0];
                $display("empty: size=%0d right=%0d high=%0d inc=%0d", $size(d), $right(d), $high(d), $increment(d));
                d = new [1];
                $display("one: right=%0d inc=%0d", $right(d), $increment(d));
                d = new [4];
                $display("dim1: left=%0d right=%0d", $left(d, 1), $right(d, 1));
                $display("formal: %0d", sum_bounds(d));
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "d: size=5 left=0 right=4 low=0 high=4 inc=-1 dims=2 ud=1\n"
          "q: size=3 left=0 right=2 low=0 high=2 inc=-1 dims=2 ud=1\n"
          "empty: size=0 right=-1 high=-1 inc=-1\n"
          "one: right=0 inc=-1\n"
          "dim1: left=0 right=3\n"
          "formal: 9\n");
}

// A dimension argument out of range for the operand's total dimension count
// is a slang FRONTEND diagnostic ("N is not a valid dimension for '...'"),
// not a runtime 'x, for a dynamically-sized operand exactly as for a fixed
// one -- verified directly against clotho and unaffected by the runtime
// path above (checkArguments / checkDim run during binding, before Clotho
// ever lowers the call). Pinned here so a change to that boundary is caught.
TEST_CASE("array query functions (LRM 20.7): an out-of-range dimension on a dynamic operand is a compile error") {
    const LowerResult r = lower_text(R"(
        module t;
            int d[];
            initial begin
                d = new [4];
                $display("%0d", $left(d, 3));
            end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("not a valid dimension") != std::string::npos);
}

// A dimension argument that is not itself a compile-time constant cannot be
// resolved by Clotho's runtime lowering either (LRM 20.7.1 only defines the
// dim-absent/dim=1 and fixed-inner-dimension shapes, both of which fold);
// refused loudly rather than silently treated as dim 1.
TEST_CASE("array query functions (LRM 20.7): a non-constant dimension argument on a dynamic operand is refused") {
    const LowerResult r = lower_text(R"(
        module t;
            int d[];
            initial begin
                int k;
                d = new [4];
                k = 1;
                $display("%0d", $left(d, k));
            end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("non-constant dimension argument") != std::string::npos);
}

// The `iff` guard (LRM 9.4.2.3) must be honored, not dropped: a posedge with the
// guard false does not run the body, one with it true does. (This construct was
// formerly rejected; it is now lowered as `always @(ev) if (cond) body`.)
TEST_CASE("iff qualifier on an event control gates the body, not dropped") {
    constexpr std::string_view src = R"(
        module t;
            logic clk = 0, en = 0;
            always @(posedge clk iff en) $display("tick");
            initial begin
                en = 0; clk = 1; #1 clk = 0; #1;   // guard false: no tick
                en = 1; clk = 1; #1 clk = 0; #1;   // guard true: tick
            end
        endmodule
    )";
    CHECK(run_capture(src) == "tick\n");
}

TEST_CASE("behavior-bearing members are rejected, not dropped") {
    constexpr std::string_view specify = R"(
        module m(input a, output b);
            assign b = a;
            specify (a => b) = 5; endspecify
        endmodule
        module t;
            reg a; wire b;
            m u(a, b);
        endmodule
    )";
    CHECK(reject(specify).find("SpecifyBlock") != std::string::npos);
}

// A bit/part/indexed select of a parameter has no signal storage: a constant
// select folds (LRM 11.2.1), and a runtime index into a packed parameter spills
// the constant into a temp and selects at run time (hazard3_pmp.v PMP_HARDWIRED[i]
// with a procedural loop variable). Regression for "reference to unknown signal".
// Every line cross-checked against iverilog v13.
TEST_CASE("parameter bit/part/indexed selects agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            parameter [15:0] P = 16'hBEEF;
            parameter        Q = 6;
            parameter [7:0]  R = 8'hB5;
            parameter [31:0] W = 32'hDEADBEEF;
            reg [3:0] ctr;
            integer i;
            initial begin
                $display("cbit %b %b", P[0], P[15]);
                $display("rng %h %h", P[7:0], P[11:4]);
                $display("ips %h", P[4 +: 8]);
                $display("qsel %b %h", Q[0], Q[2:0]);
                ctr = 4'd10; ctr = ctr - Q[2:0];   // the hazard3 muldiv shape
                $display("ctr %0d", ctr);
                for (i = 0; i < 8; i = i + 1) $write("%b", R[i]);       // runtime bit-select
                $write("\n");
                for (i = 0; i < 4; i = i + 1) $display("nib %h", W[8*i +: 8]); // runtime part-select
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "cbit 1 1\n"
          "rng ef ee\n"
          "ips ee\n"
          "qsel 0 6\n"
          "ctr 4\n"
          "10101101\n"
          "nib ef\nnib be\nnib ad\nnib de\n");
}
