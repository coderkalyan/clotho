#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors struct_test.cpp).
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

// Whole fixed unpacked-array / unpacked-struct equality (LRM 11.2.2 / 11.4.5):
// a per-leaf compare AND-reduced with X-propagation. iverilog rejects
// whole-array `==`; the value expectation is cross-checked against verilator
// 5.048 (definite cases) and the LRM (the X-propagating case).
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

