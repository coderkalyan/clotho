#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

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
