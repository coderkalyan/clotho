#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// Assignment patterns `'{...}` (LRM 10.9): positional (Simple), keyed
// (Structured: member / index / default), and replicated. Packed operands
// lower to a concatenation of the ordered elements (`lower_packed_pattern`);
// unpacked structs/arrays decompose to per-leaf stores (`emit_agg_store`).
//
// Expected values are pinned against verilator; it diverges from slang only on a
// keyed pattern over a *descending* packed array, where slang mis-folds the
// element order. Clotho reverses it to correct that (docs/LRM_NOTES.md, note 11),
// exercised by the descending cases below.

namespace {

// JIT-run a module and capture stdout (mirrors struct_test.cpp).
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

TEST_CASE("packed struct pattern: positional and keyed") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { logic [7:0] hi, lo; } pr_t;
            pr_t a, b;
            initial begin
                a = '{8'hAA, 8'hDD};          // positional: first = hi (MSB)
                b = '{lo: 8'h22, hi: 8'h11};  // keyed by member name
                $display("%h %h", a, b);
            end
        endmodule
    )") == "aadd 1122\n");
}

TEST_CASE("packed array pattern: positional direction-independent") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0][7:0] d; logic [0:3][7:0] a;
            initial begin
                d = '{8'h11, 8'h22, 8'h33, 8'h44};
                a = '{8'h11, 8'h22, 8'h33, 8'h44};
                $display("%h %h", d, a);
            end
        endmodule
    )") == "11223344 11223344\n");
}

TEST_CASE("packed array pattern: keyed, descending reverses (slang-bug workaround)") {
    // verilator: '{0:x,3:y} of logic[3:0] places index 0 at the LSB.
    CHECK(run_capture(R"(
        module m;
            logic [3:0][7:0] d; logic [0:3][7:0] a;
            initial begin
                d = '{0:8'h11, 1:8'h22, 2:8'h33, 3:8'h44};
                a = '{0:8'h11, 1:8'h22, 2:8'h33, 3:8'h44};
                $display("%h %h", d, a);
            end
        endmodule
    )") == "44332211 11223344\n");
}

TEST_CASE("packed array pattern: default and replicated") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0][7:0] p; logic [1:0][7:0] r;
            initial begin
                p = '{default: 8'hAB};
                r = '{2{8'h7}};
                $display("%h %h", p, r);
            end
        endmodule
    )") == "abababab 0707\n");
}

TEST_CASE("unpacked array pattern: positional maps first to leftmost index") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] d [3:0]; logic [7:0] a [0:3];
            initial begin
                d = '{8'h11, 8'h22, 8'h33, 8'h44};
                a = '{8'h11, 8'h22, 8'h33, 8'h44};
                $display("%h%h%h%h %h%h%h%h",
                         d[3], d[2], d[1], d[0], a[0], a[1], a[2], a[3]);
            end
        endmodule
    )") == "11223344 11223344\n");
}

TEST_CASE("unpacked array pattern: keyed index, both range directions") {
    // Key K binds array index K regardless of declared direction.
    CHECK(run_capture(R"(
        module m;
            logic [7:0] d [3:0]; logic [7:0] a [0:3];
            initial begin
                d = '{0:8'hAA, 3:8'hDD, default:8'h00};
                a = '{0:8'hAA, 3:8'hDD, default:8'h00};
                $display("%h%h %h%h", d[0], d[3], a[0], a[3]);
            end
        endmodule
    )") == "aadd aadd\n");
}

TEST_CASE("unpacked array pattern: replicated expands to fill") {
    CHECK(run_capture(R"(
        module m;
            int r [0:3]; logic [7:0] s [0:3];
            initial begin
                r = '{2{5, 6}};
                s = '{4{8'h7}};
                $display("%0d %0d %0d %0d %h%h%h%h",
                         r[0], r[1], r[2], r[3], s[0], s[1], s[2], s[3]);
            end
        endmodule
    )") == "5 6 5 6 07070707\n");
}

TEST_CASE("unpacked struct pattern with default") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; int b; } us_t;
            us_t u;
            initial begin
                u = '{a: 8'h3, default: 9};
                $display("%h %0d", u.a, u.b);
            end
        endmodule
    )") == "03 9\n");
}

TEST_CASE("nested pattern: array of unpacked struct") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; int b; } us_t;
            us_t sa [0:1];
            initial begin
                sa = '{'{a: 8'h10, b: 1}, '{a: 8'h20, b: 2}};
                $display("%h %0d %h %0d", sa[0].a, sa[0].b, sa[1].a, sa[1].b);
            end
        endmodule
    )") == "10 1 20 2\n");
}

TEST_CASE("packed pattern as rvalue expression (not whole-object store)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { logic [7:0] hi, lo; } pr_t;
            logic [15:0] w;
            initial begin
                w = pr_t'('{hi: 8'hDE, lo: 8'hAD}) | 16'h0000;
                $display("%h", w);
            end
        endmodule
    )") == "dead\n");
}

TEST_CASE("nonblocking packed-array pattern commits whole value") {
    CHECK(run_capture(R"(
        module m;
            logic [1:0][31:0] q;
            logic clk;
            initial begin
                clk = 0;
                #1 clk = 1;
                #1 $display("%h", q);
            end
            always @(posedge clk) q <= '{default: 32'hA};
        endmodule
    )") == "0000000a0000000a\n");
}

// ---------------------------------------------------------------------------
// Whole fixed unpacked-array operations (LRM 7.4.5 / 11.2.2): nonblocking
// whole-array copy + pattern, whole-array equality, and array slices. iverilog
// supports none of these (rejects whole-array `<=`, `==`, and slices); all
// value expectations below are cross-checked against verilator 5.048.
// ---------------------------------------------------------------------------

TEST_CASE("nonblocking whole unpacked-array copy commits deferred (swap idiom)") {
    // b <= a and a <= b in one block both read OLD values -> the arrays swap.
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            logic clk = 0;
            int i;
            always #5 clk = ~clk;
            always @(posedge clk) begin
                b <= a;
                a <= b;
            end
            initial begin
                for (i = 0; i < 4; i++) begin a[i] = i + 1; b[i] = 8'hF0 + i; end
                @(posedge clk); #1;
                $display("%0d %0d %0d %0d %0h %0h %0h %0h",
                    a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                $finish;
            end
        endmodule
    )") == "240 241 242 243 1 2 3 4\n");
}

TEST_CASE("nonblocking '{...} pattern to an unpacked array") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:3];
            logic clk = 0;
            always #5 clk = ~clk;
            always @(posedge clk) a <= '{8'd10, 8'd20, 8'd30, 8'd40};
            initial begin
                @(posedge clk); #1;
                $display("%0d %0d %0d %0d", a[0], a[1], a[2], a[3]);
                $finish;
            end
        endmodule
    )") == "10 20 30 40\n");
}

TEST_CASE("whole unpacked-array equality is per-element, X-propagating") {
    // Equal -> 1, one definite mismatch -> 0, an ambiguous (x) member with all
    // others equal -> x, per LRM 11.4.5 (docs/LRM_NOTES.md, note 13).
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            int i;
            logic r1, r2, r3;
            initial begin
                for (i = 0; i < 4; i++) begin a[i] = i; b[i] = i; end
                r1 = (a == b);
                b[2] = 8'd99;
                r2 = (a != b);
                for (i = 0; i < 4; i++) b[i] = i;
                a[1] = 8'hxx;
                r3 = (a == b);
                $display("%b %b %b", r1, r2, r3);
                $finish;
            end
        endmodule
    )") == "1 1 x\n");
}

TEST_CASE("fixed unpacked-array slice: copy, equality, lvalue, descending base") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:7];
            logic [7:0] bd [7:0];
            logic [7:0] s2 [0:1];
            logic [7:0] sd [1:0];
            int i;
            logic eqa, eqb;
            initial begin
                for (i = 0; i < 8; i++) begin a[i] = i + 1; bd[i] = 8'h10 + i; end
                s2 = a[2:3];             // element order: s2[0]=a[2]=3, s2[1]=a[3]=4
                sd = bd[5:4];            // descending base: sd[1]=bd[5], sd[0]=bd[4]
                a[5:6] = '{8'd100, 8'd101};
                eqa = (a[2:3] == s2);    // 1
                eqb = (a[2:3] == a[0:1]);// 0
                $display("%0d %0d %0h %0h %0d %0d %b %b",
                    s2[0], s2[1], sd[1], sd[0], a[5], a[6], eqa, eqb);
                $finish;
            end
        endmodule
    )") == "3 4 15 14 100 101 1 0\n");
}

TEST_CASE("nonblocking unpacked-array slice copy commits deferred") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            logic clk = 0;
            int i;
            always #5 clk = ~clk;
            always @(posedge clk) a[1:2] <= b[1:2];
            initial begin
                for (i = 0; i < 4; i++) begin a[i] = i; b[i] = 8'hA0 + i; end
                @(posedge clk); #1;
                $display("%0d %0h %0h %0d", a[0], a[1], a[2], a[3]);
                $finish;
            end
        endmodule
    )") == "0 a1 a2 3\n");
}

// ---------------------------------------------------------------------------
// Indexed part-select slice of an unpacked array, `a[i +: n]` / `a[i -: n]`
// (LRM 7.4.5: "The size of the part-select or slice shall be constant, but the
// position can be variable"). Expanded position-wise at lower time, so every
// position is an ordinary element access and carries the clause's out-of-range
// rule on its own. iverilog rejects unpacked-array slices outright; the two
// deliberate choices Clotho makes here are judging each position on its own and
// treating an x/z base as invalid (docs/LRM_NOTES.md, note 5).
// ---------------------------------------------------------------------------

TEST_CASE("indexed unpacked-array slice: direction, endianness, and element order") {
    // Positions run in the BASE array's declaration order, so `+:` and `-:` only
    // choose which contiguous index window is selected -- exactly like a packed
    // indexed part-select.
    CHECK(run_capture(R"(
        module m;
            logic [7:0] q [0:7];   // ascending
            logic [7:0] d [7:0];   // descending
            logic [7:0] r2 [0:1];
            logic [7:0] r1 [0:0];
            int i;
            initial begin
                for (int k = 0; k < 8; k++) begin q[k] = 8'h10 + k; d[k] = 8'ha0 + k; end
                i = 2; r2 = q[i +: 2]; $display("asc_up   %h %h", r2[0], r2[1]);
                i = 5; r2 = q[i -: 2]; $display("asc_down %h %h", r2[0], r2[1]);
                i = 5; r2 = d[i +: 2]; $display("des_up   %h %h", r2[0], r2[1]);
                i = 5; r2 = d[i -: 2]; $display("des_down %h %h", r2[0], r2[1]);
                i = 5; r1 = q[i +: 1]; $display("width1   %h", r1[0]);
            end
        endmodule
    )") == "asc_up   12 13\n"
           "asc_down 14 15\n"
           "des_up   a6 a5\n"
           "des_down a5 a4\n"
           "width1   15\n");
}

TEST_CASE("indexed unpacked-array slice writes, including `-:` and a descending base") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] q [0:7];
            logic [7:0] d [7:0];
            logic [7:0] s2 [0:1];
            int i;
            initial begin
                for (int k = 0; k < 8; k++) begin q[k] = 8'h10 + k; d[k] = 8'ha0 + k; end
                i = 1; q[i +: 2] = '{8'hf0, 8'hf1};
                $display("wr_up    %h %h %h %h", q[0], q[1], q[2], q[3]);
                i = 3; q[i -: 2] = '{8'hb2, 8'hb3};
                $display("wr_down  %h %h", q[2], q[3]);
                i = 6; d[i -: 2] = '{8'hc0, 8'hc1};
                $display("wr_des   %h %h %h", d[7], d[6], d[5]);
                s2 = '{8'h55, 8'h56};
                i = 4; q[i +: 2] = s2;                 // array source
                $display("wr_arr   %h %h", q[4], q[5]);
                i = 0; q[i +: 2] = q[4 +: 2];          // slice source
                $display("wr_slice %h %h", q[0], q[1]);
            end
        endmodule
    )") == "wr_up    10 f0 f1 13\n"
           "wr_down  b2 b3\n"
           "wr_des   a7 c0 c1\n"
           "wr_arr   55 56\n"
           "wr_slice 55 56\n");
}

TEST_CASE("an overlapping slice copy reads the source's pre-assignment values") {
    // LRM 10.4.1 evaluates the right-hand side and then assigns, so a shifted copy
    // must not read a position it has already written. Both slice forms stage the
    // source in a temp when the two sides are rooted at the same array. (The
    // constant-bounds form used to propagate its own writes: `q[1:3] = q[0:2]` gave
    // `10 10 10 10`.)
    CHECK(run_capture(R"(
        module m;
            logic [7:0] q [0:7];
            logic [7:0] p [0:7];
            int i;
            initial begin
                for (int k = 0; k < 8; k++) begin q[k] = 8'h10 + k; p[k] = 8'h10 + k; end
                q[1:3] = q[0:2];
                $display("const %h %h %h %h", q[0], q[1], q[2], q[3]);
                i = 1; p[i +: 3] = p[0 +: 3];
                $display("idx   %h %h %h %h", p[0], p[1], p[2], p[3]);
            end
        endmodule
    )") == "const 10 10 11 12\n"
           "idx   10 10 11 12\n");
}

TEST_CASE("an out-of-range indexed slice reads the element default per position") {
    // LRM 7.4.5: an invalid index reads Table 7-1's value (x for 4-state, 0 for
    // 2-state) and writes nothing. Each POSITION is judged on its own, so a
    // partially out-of-range slice keeps its in-range positions where they are --
    // the same rule 11.5.1 states for a packed part-select (docs/LRM_NOTES.md, note 5).
    CHECK(run_capture(R"(
        module m;
            logic [7:0] q [0:7];
            int         n [0:7];
            logic [7:0] r3 [0:2];
            int         n2 [0:1];
            int i;
            initial begin
                for (int k = 0; k < 8; k++) begin q[k] = 8'h10 + k; n[k] = k + 1; end
                i = 6; r3 = q[i +: 3];  $display("part_hi  %h %h %h", r3[0], r3[1], r3[2]);
                i = 1; r3 = q[i -: 3];  $display("part_lo  %h %h %h", r3[0], r3[1], r3[2]);
                i = 20; r3 = q[i +: 3]; $display("all_oob  %h %h %h", r3[0], r3[1], r3[2]);
                i = 7; n2 = n[i +: 2];  $display("two_st   %0d %0d", n2[0], n2[1]);
                i = 7; q[i +: 2] = '{8'he0, 8'he1};
                $display("wr_part  %h %h", q[6], q[7]);
                i = 30; q[i +: 2] = '{8'hd0, 8'hd1};
                $display("wr_oob   %h %h %h", q[0], q[1], q[7]);
            end
        endmodule
    )") == "part_hi  16 17 xx\n"
           "part_lo  xx 10 11\n"
           "all_oob  xx xx xx\n"
           "two_st   8 0\n"
           "wr_part  16 e0\n"
           "wr_oob   10 11 e0\n");
}

TEST_CASE("an x/z indexed slice base is an invalid index (LRM 7.4.5)") {
    // "If an index expression is out of bounds or if any bit in the index
    // expression is x or z, then the index shall be invalid." Every position of
    // the slice inherits the unknown, so the read is all-x and the write a no-op
    // (docs/LRM_NOTES.md, note 5).
    CHECK(run_capture(R"(
        module m;
            logic [7:0] q [0:7];
            logic [7:0] r2 [0:1];
            integer     xi;         // 4-state, unlike `int`
            logic [3:0] zi;
            initial begin
                for (int k = 0; k < 8; k++) q[k] = 8'h10 + k;
                xi = 'x;      r2 = q[xi +: 2]; $display("rd_x     %h %h", r2[0], r2[1]);
                zi = 4'bz001; r2 = q[zi +: 2]; $display("rd_z     %h %h", r2[0], r2[1]);
                xi = 'x;      q[xi +: 2] = '{8'hc0, 8'hc1};
                $display("wr_x     %h %h", q[0], q[1]);
            end
        endmodule
    )") == "rd_x     xx xx\n"
           "rd_z     xx xx\n"
           "wr_x     10 11\n");
}

TEST_CASE("indexed unpacked-array slice in every supported context") {
    // Equality operand, nonblocking source and target, by-value / output / inout
    // subroutine argument, a slice of a struct's array member, a slice of a
    // multidimensional array's outer dimension, and a base expression evaluated
    // exactly once (LRM 11.5.1's self-determined context).
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] arr [0:5]; int tag; } s_t;
            logic [7:0] q  [0:7];
            logic [7:0] r2 [0:1];
            int         m4 [0:3][0:1];
            int         m2 [0:1][0:1];
            s_t         s;
            int i, calls = 0;
            function automatic int base_once(); calls = calls + 1; return 2; endfunction
            function automatic void f_in(logic [7:0] v [0:1]);
                $display("arg_in   %h %h", v[0], v[1]);
            endfunction
            function automatic void f_out(output logic [7:0] v [0:1]);
                v[0] = 8'h71; v[1] = 8'h72;
            endfunction
            function automatic void f_inout(inout logic [7:0] v [0:1]);
                v[0] = v[0] + 8'h1; v[1] = v[1] + 8'h2;
            endfunction
            initial begin
                for (int k = 0; k < 8; k++) q[k] = 8'h10 + k;
                for (int k = 0; k < 4; k++) begin m4[k][0] = k * 10; m4[k][1] = k * 10 + 1; end
                for (int k = 0; k < 6; k++) s.arr[k] = 8'h50 + k;
                i = 4; r2 = '{8'h14, 8'h15};
                $display("eq       %b %b", q[i +: 2] == r2, q[0 +: 2] == r2);
                i = 2; r2 <= q[i +: 2];
                q[6 +: 2] <= '{8'ha6, 8'ha7};
                #1 $display("nba      %h %h | %h %h", r2[0], r2[1], q[6], q[7]);
                i = 4; f_in(q[i +: 2]);
                i = 0; f_out(q[i +: 2]);
                $display("arg_out  %h %h", q[0], q[1]);
                i = 0; f_inout(q[i +: 2]);
                $display("arg_io   %h %h", q[0], q[1]);
                i = 3; r2 = s.arr[i +: 2];
                $display("member   %h %h", r2[0], r2[1]);
                i = 1; s.arr[i +: 2] = '{8'h61, 8'h62};
                $display("mem_wr   %h %h %h", s.arr[0], s.arr[1], s.arr[2]);
                i = 1; m2 = m4[i +: 2];
                $display("multidim %0d %0d %0d %0d", m2[0][0], m2[0][1], m2[1][0], m2[1][1]);
                i = 2; m4[i +: 2] = '{'{77, 78}, '{79, 80}};
                $display("md_wr    %0d %0d %0d %0d", m4[2][0], m4[2][1], m4[3][0], m4[3][1]);
                r2 = q[base_once() +: 2];
                $display("once     %h %h calls=%0d", r2[0], r2[1], calls);
            end
        endmodule
    )") == "eq       1 0\n"
           "nba      12 13 | a6 a7\n"
           "arg_in   14 15\n"
           "arg_out  71 72\n"
           "arg_io   72 74\n"
           "member   53 54\n"
           "mem_wr   50 61 62\n"
           "multidim 10 11 20 21\n"
           "md_wr    77 78 79 80\n"
           "once     12 13 calls=1\n");
}

// ---------------------------------------------------------------------------
// Assignment-pattern lvalue destructuring, `'{a, b, c} = rhs` / typed
// `U'{a, b, c} = rhs` (LRM 10.9). Positional only ("the positional notation
// shall be required" when a pattern is used as an lvalue). iverilog and
// verilator both reject the syntax outright. Clotho destructures *structurally*
// (element-for-element against the right-hand type), not as a flat bit-slice —
// the truncation test below pins that, since a flat-bit-concat reading would not
// reproduce the plain per-element truncated values.
// ---------------------------------------------------------------------------

TEST_CASE("assignment-pattern lvalue: bare positional destructure of a fixed array") {
    CHECK(run_capture(R"(
        module m;
            int a, b, c, d;
            int B [4];
            initial begin
                B = '{1, 2, 3, 4};
                '{a, b, c, d} = B;
                $display("%0d %0d %0d %0d", a, b, c, d);
            end
        endmodule
    )") == "1 2 3 4\n");
}

TEST_CASE("assignment-pattern lvalue: typed pattern destructure") {
    CHECK(run_capture(R"(
        module m;
            typedef byte U [3];
            U A;
            byte x, y, z;
            initial begin
                A = '{10, 20, 30};
                U'{x, y, z} = A;
                $display("%0d %0d %0d", x, y, z);
            end
        endmodule
    )") == "10 20 30\n");
}

TEST_CASE("assignment-pattern lvalue: packed struct right-hand side (bit-slice path)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { logic [7:0] hi; logic [7:0] lo; } pair_t;
            pair_t p;
            byte x, y;
            initial begin
                p = '{8'hAA, 8'hBB};
                '{x, y} = p;
                $display("%h %h", x, y);
            end
        endmodule
    )") == "aa bb\n");
}

TEST_CASE("assignment-pattern lvalue: nested pattern over a struct-of-struct") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int x; int y; } pt_t;
            typedef struct { pt_t p; int z; } rec_t;
            rec_t r;
            int a, b, c;
            initial begin
                r = '{'{1, 2}, 3};
                '{'{a, b}, c} = r;
                $display("%0d %0d %0d", a, b, c);
            end
        endmodule
    )") == "1 2 3\n");
}

TEST_CASE("assignment-pattern lvalue: per-element width truncates like an ordinary assignment") {
    // A flat bit-concat reading of `'{a,b,c,d} = B` would take the top 32 bits
    // of a 128-bit concatenation of B's four 32-bit elements (~0), not each
    // element's low byte; Clotho destructures element for element, so each
    // target truncates its own element.
    CHECK(run_capture(R"(
        module m;
            byte a, b, c, d;
            int B [4];
            initial begin
                B = '{257, 258, 259, 260};
                '{a, b, c, d} = B;
                $display("%0d %0d %0d %0d", a, b, c, d);
            end
        endmodule
    )") == "1 2 3 4\n");
}

TEST_CASE("assignment-pattern lvalue: nonblocking commits deferred") {
    CHECK(run_capture(R"(
        module m;
            int a, b;
            int B [2];
            logic clk = 0;
            always #5 clk = ~clk;
            always @(posedge clk) '{a, b} <= B;
            initial begin
                B = '{10, 20};
                @(posedge clk); #1;
                $display("%0d %0d", a, b);
                $finish;
            end
        endmodule
    )") == "10 20\n");
}

TEST_CASE("assignment-pattern lvalue: keyed pattern is rejected as an lvalue") {
    // slang itself rejects a keyed pattern as an lvalue at binding time
    // ("expression is not assignable"), ahead of Lower.cpp's own guard for
    // the same rule (kept as defense in depth for any shape slang doesn't
    // catch first).
    CHECK_FALSE(reject(R"(
        module m;
            int a, b;
            int B [2];
            initial '{0: a, 1: b} = B;
        endmodule
    )").empty());
}

TEST_CASE("assignment-pattern lvalue: compound assignment is rejected") {
    // Also caught by slang at binding time ("invalid operands to binary
    // expression") ahead of Lower.cpp's own guard.
    CHECK_FALSE(reject(R"(
        module m;
            int a, b;
            int B [2];
            initial '{a, b} += B;
        endmodule
    )").empty());
}

TEST_CASE("assignment-pattern lvalue: queue right-hand side is rejected") {
    // slang rejects this at binding time ("lvalue assignment patterns cannot
    // be assigned from dynamic arrays"), ahead of Lower.cpp's own guard (kept
    // as defense in depth).
    CHECK_FALSE(reject(R"(
        module m;
            int a, b, c;
            int q [$];
            initial begin
                q = '{1, 2, 3};
                '{a, b, c} = q;
            end
        endmodule
    )").empty());
}

TEST_CASE("assignment-pattern lvalue: element count mismatch is rejected") {
    // slang rejects this at binding time ("requires 4 elements but 3 were
    // provided"), ahead of Lower.cpp's own guard (kept as defense in depth).
    CHECK_FALSE(reject(R"(
        module m;
            int a, b, c;
            int B [4];
            initial '{a, b, c} = B;
        endmodule
    )").empty());
}

// An associative-array literal (LRM 7.9.11) builds the array through the
// container path; the shapes that path does not implement fail loudly instead of
// being dropped, which is how the initializer used to be lost silently.
TEST_CASE("associative array literal: unsupported shapes are rejected") {
    // A whole-array copy `a <= b` now stages through HandleCopyNb; a LITERAL right
    // -hand side still cannot, because it builds entries in place rather than
    // deep-copying a handle the NBA region can move.
    CHECK(reject("module m; int a[int]; initial a <= '{default:1}; endmodule")
            .find("nonblocking assignment of a literal into an associative array") !=
          std::string::npos);
    // An aggregate value element needs a per-leaf store per entry.
    CHECK(reject(R"(
        module m;
            typedef struct { int x; byte y; } p;
            p a[int] = '{1:'{x:1, y:2}};
            initial $display("%0d", a[1].x);
        endmodule
    )").find("aggregate value element") != std::string::npos);
}
