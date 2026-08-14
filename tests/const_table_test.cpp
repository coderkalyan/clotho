// Constant tables: an unpacked-array parameter / localparam read with a runtime
// index (LRM 6.20 + 11.5.1). Such a parameter has no storage and cannot fold, so
// the lowerer materializes its value as a read-only signal whose image the
// backend bakes into the state initializer (docs/design/const-tables.md). The
// packed sibling of this feature — a runtime bit/part-select of a packed
// parameter — lives in packed_range_test.cpp.
//
// Expected strings were cross-checked against Icarus Verilog (iverilog v13) on
// the `const` variable form of the same tables: iverilog rejects unpacked-array
// parameters outright ("sorry: unpacked array parameters are not supported
// yet"), and a `const` unpacked array is the same value with the same element
// semantics. The one deliberate divergence is an out-of-range read of a 2-state
// element, which yields the element default 0 per LRM 7.4.5 + Table 7-1 rather
// than iverilog's forced x.

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

TEST_CASE("runtime-indexed unpacked-array parameters agree with iverilog") {
    constexpr std::string_view src = R"(
        module t;
            localparam int         P[0:3] = '{1,2,3,4};
            localparam logic [3:0] X[0:3] = '{4'b10x1, 4'bzzzz, 4'h5, 4'bx0z1};
            localparam logic [7:0] B[0:3] = '{8'hA1,8'hB2,8'hC3,8'hD4};
            localparam int         M[0:1][0:1] = '{'{1,2},'{3,4}};
            localparam real        R[0:2] = '{1.5,2.5,3.5};
            integer i, j;
            int     q[0:3];
            function automatic int pick(input int a[0:3], input integer k);
                pick = a[k];
            endfunction
            initial begin
                for (i = 0; i < 4; i = i + 1) $write("%0d ", P[i]);
                $write("\n");
                for (i = 0; i < 4; i = i + 1) $write("%b ", X[i]);   // x/z round-trip
                $write("\n");
                i = 2; $display("bsel %h %h", B[i], B[i][7:4]);      // select inside an element
                i = 1; j = 0; $display("mdim %0d", M[i][j]);
                i = 2; $display("real %0.2f", R[i]);
                i = 9; $display("oob %0d %b", P[i], X[i]);           // LRM 7.4.5 element default
                q = P; i = 3; $display("copy %0d %0d", q[i], q[0]);  // whole-array copy
                foreach (P[k]) $write("%0d", P[k]);
                $write("\n");
                i = 1; $display("call %0d", pick(P, i));             // table as a by-value actual
                $display("fold %0d", P[2]);                          // still folds, no storage
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "1 2 3 4 \n"
          "10x1 zzzz 0101 x0z1 \n"
          "bsel c3 c\n"
          "mdim 3\n"
          "real 3.50\n"
          "oob 0 xxxx\n"
          "copy 4 1\n"
          "1234\n"
          "call 2\n"
          "fold 3\n");
}

// A table is materialized per elaborated parameter symbol, so two instances of
// one module with different overrides read different tables, and a package
// parameter is one table shared by every reader.
TEST_CASE("per-instance overrides and package constant tables stay distinct") {
    constexpr std::string_view src = R"(
        package pk;
            localparam int K[0:2] = '{10,20,30};
        endpackage
        module sub #(parameter int P[0:1] = '{0,0}) ();
            import pk::*;
            int i;
            initial begin
                i = 1;
                $display("%m %0d %0d", P[i], K[i]);
                i = 0;
                $display("%m %0d %0d", P[i], K[i]);
            end
        endmodule
        module t;
            sub #(.P('{7,9})) u();
            sub #(.P('{3,4})) v();
        endmodule
    )";
    CHECK(run_capture(src) ==
          "t.u 9 20\n"
          "t.u 7 10\n"
          "t.v 4 20\n"
          "t.v 3 10\n");
}

// A class value parameter is an ordinary ParameterSymbol of the specialization,
// so an unpacked-array one materializes like any other.
TEST_CASE("a class parameter table is indexable at run time") {
    constexpr std::string_view src = R"(
        class C #(parameter int P[0:2] = '{1,2,3});
            static function int get(int k);
                return P[k];
            endfunction
        endclass
        module t;
            int i;
            initial begin
                i = 2;
                $display("%0d %0d", C#()::get(i), C#(.P('{5,6,7}))::get(i));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 7\n");
}

// A materialized table is not a design variable: it must not appear in the VCD,
// which dumps the instance hierarchy's signals.
TEST_CASE("a constant table is not dumped as a design variable") {
    constexpr std::string_view src = R"(
        module t;
            localparam int P[0:3] = '{1,2,3,4};
            logic [1:0] i;
            int         x;
            initial begin
                $dumpfile("/dev/null");
                $dumpvars;
                i = 1;
                #1 x = P[i];
                $display("%0d", x);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n");

    const LowerResult r = lower_text(src);
    REQUIRE(r.ok);
    uint32_t dumped = 0;
    for (const ir::InstanceScope& top : r.module->tops()) {
        dumped += static_cast<uint32_t>(top.signals.size());
    }
    // Only `i` and `x` belong to the dumped hierarchy; the table is unscoped.
    CHECK(dumped == 2);
}

// An unpacked struct has no pooled plane layout of its own, so its image is
// written and read back leaf-major through the aggregate storage mirror. Nested
// arrays, 4-state members and a runtime index into a member array all exercise
// that walk.
TEST_CASE("unpacked-struct constant tables bake leaf-major") {
    constexpr std::string_view src = R"(
        typedef struct { int a; logic [3:0] b; } s_t;
        typedef struct { int v[0:1]; logic [3:0] f; } n_t;
        module t;
            localparam s_t P[0:1] = '{'{1, 4'bx01z}, '{3, 4'hF}};
            localparam n_t N[0:1] = '{'{'{1,2}, 4'h5}, '{'{3,4}, 4'h6}};
            localparam n_t ONE    = '{'{7,8}, 4'h9};
            int i, j;
            initial begin
                for (i = 0; i < 2; i = i + 1) $display("s %0d %b", P[i].a, P[i].b);
                i = 1; j = 1; $display("n %0d %h", N[i].v[j], N[i].f);
                j = 0; $display("one %0d %h", ONE.v[j], ONE.f);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "s 1 x01z\n"
          "s 3 1111\n"
          "n 4 6\n"
          "one 7 9\n");
}

// A value holding a heap-backed leaf (string / queue / dynamic array /
// associative array) cannot be baked — only the runtime can build those handles
// — so it is seeded once into the PreInit process instead. Element values are
// the LRM 7.8 / 7.9 / 6.16 ones; iverilog cannot oracle these (it rejects
// unpacked-array parameters, has no string-keyed associative arrays, and reports
// 3 for `"bb".len()` read out of a string array), so each shape is pinned
// against the equivalent variable form clotho's own container tests cover.
TEST_CASE("heap-backed constant tables are seeded at time zero") {
    constexpr std::string_view src = R"(
        typedef struct { string n; int v; } kv_t;
        module t;
            localparam string S[0:2] = '{"aa","bb","cc"};
            localparam string LONG   = "a string that is definitely longer than thirty bytes";
            localparam string L[0:0] = '{"another string well past the inline capacity limit"};
            localparam int    Q[$]   = '{1,2,3};
            localparam string D[]    = '{"x","yy"};
            localparam int    A[string] = '{default:9, "a":1, "b":2};
            localparam string M[int] = '{1:"one", 2:"two"};
            localparam kv_t   KV[0:1] = '{'{"one",1},'{"two",2}};
            int    i;
            string k;
            initial begin
                for (i = 0; i < 3; i = i + 1) $write("%s%0d ", S[i], S[i].len());
                $write("\n");
                $display("long %0d %0d", LONG.len(), L[0].len());
                i = 1; $display("q %0d %0d", Q[i], Q.size());
                i = 1; $display("d %s %0d", D[i], D.size());
                k = "b"; $display("a %0d %0d %0d", A[k], A["zz"], A.num());
                i = 2; $display("m %s", M[i]);
                i = 1; $display("kv %s %0d", KV[i].n, KV[i].v);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "aa2 bb2 cc2 \n"
          "long 52 50\n"
          "q 2 3\n"
          "d yy 2\n"
          "a 2 9 2\n"
          "m two\n"
          "kv two 2\n");
}

// A `string` / `null` parameter is a scalar constant, not a table: it folds like
// any other value once lower_constant knows the shape.
TEST_CASE("string and null parameters fold") {
    constexpr std::string_view src = R"(
        class C; int x; endclass
        module t;
            localparam string NAME = "abc";
            localparam C      H    = null;
            string s;
            initial begin
                s = NAME;
                $display("%s %0d %0d %0d", s, NAME.len(), s == "abc", H == null);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "abc 3 1 1\n");
}

// What still has no constant-table form: an associative array whose key type is
// a wildcard (no declared type to intern the key against, and the runtime keys
// on the byte span), and a container whose element is itself an aggregate (one
// entry op carries one value, not an lvalue to build into).
TEST_CASE("constant tables with no representable form are rejected") {
    constexpr std::string_view wildcard = R"(
        module t;
            localparam int A[*] = '{1:10};
            int k;
            initial begin k = 1; $display("%0d", A[k]); end
        endmodule
    )";
    CHECK(reject(wildcard).find("wildcard index") != std::string::npos);

    constexpr std::string_view agg_value = R"(
        typedef struct { int a; } s_t;
        module t;
            localparam s_t A[int] = '{1:'{5}};
            int k;
            initial begin k = 1; $display("%0d", A[k].a); end
        endmodule
    )";
    CHECK(reject(agg_value).find("aggregate or container elements") != std::string::npos);
}
