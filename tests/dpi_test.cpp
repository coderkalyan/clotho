#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "runtime/svdpi.h"

using namespace clotho;

// DPI-C open-array imports (LRM 35.5.6.2) used by the open-array tests below. They
// are external-linkage `extern "C"` symbols the JIT's process-symbol generator
// resolves through dlsym(RTLD_DEFAULT) (clotho_test is built with ENABLE_EXPORTS),
// the same path the libc/libm scalar imports use. Each exercises an svOpenArray
// accessor path: element pointer (zero-copy) and per-element vecval copy (marshal).
extern "C" {

// Zero-copy int elements: sum via element pointers (svGetArrElemPtr1). Also probes
// svSize / svLow / svHigh / svLeft / svRight for a 1-D handle.
int dpi_oa_sum_int(const svOpenArrayHandle h) {
    long acc = 0;
    for (int i = svLow(h, 1); i <= svHigh(h, 1); ++i) {
        acc += *static_cast<int*>(svGetArrElemPtr1(h, i));
    }
    return static_cast<int>(acc);
}

// Marshalled logic[7:0] elements: OR-reduce the aval and bval planes so the caller
// can assert x/z survived the aval/bval marshal. Returns (bval_or << 8) | aval_or.
int dpi_oa_logic_planes(const svOpenArrayHandle h) {
    unsigned aval_or = 0;
    unsigned bval_or = 0;
    for (int i = svLeft(h, 1); ; i += (svLeft(h, 1) <= svRight(h, 1) ? 1 : -1)) {
        svLogicVecVal v;
        svGetLogicArrElem1VecVal(&v, h, i);
        aval_or |= v.aval & 0xff;
        bval_or |= v.bval & 0xff;
        if (i == svRight(h, 1)) {
            break;
        }
    }
    return static_cast<int>((bval_or << 8) | aval_or);
}

// Marshalled bit[15:0] elements: XOR-reduce (svGetBitArrElem1VecVal).
int dpi_oa_bit_xor(const svOpenArrayHandle h) {
    unsigned acc = 0;
    for (int i = svLow(h, 1); i <= svHigh(h, 1); ++i) {
        svBitVecVal v;
        svGetBitArrElem1VecVal(&v, h, i);
        acc ^= v & 0xffff;
    }
    return static_cast<int>(acc);
}

// output: fill an int array with i*i (zero-copy write through element pointers).
void dpi_oa_fill_sq(const svOpenArrayHandle h) {
    for (int i = svLow(h, 1); i <= svHigh(h, 1); ++i) {
        *static_cast<int*>(svGetArrElemPtr1(h, i)) = i * i;
    }
}

// input bit[63:0]: XOR-reduce, reading each element as two svBitVecVal chunks —
// exercises the wide (33..64-bit) two-chunk marshal.
long long dpi_oa_xor64(const svOpenArrayHandle h) {
    unsigned long long acc = 0;
    for (int i = svLow(h, 1); i <= svHigh(h, 1); ++i) {
        svBitVecVal v[2];
        svGetBitArrElem1VecVal(v, h, i);
        acc ^= (static_cast<unsigned long long>(v[1]) << 32) | v[0];
    }
    return static_cast<long long>(acc);
}

// inout: increment each definite logic[7:0] element (marshalled read + write-back).
void dpi_oa_inc_logic(const svOpenArrayHandle h) {
    for (int i = svLow(h, 1); i <= svHigh(h, 1); ++i) {
        svLogicVecVal v;
        svGetLogicArrElem1VecVal(&v, h, i);
        const unsigned val = ((v.aval & ~v.bval) + 1) & 0xff;
        v.aval = val;
        v.bval = 0;
        svPutLogicArrElem1VecVal(h, &v, i);
    }
}

} // extern "C"

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

// Lower a design expected to fail a DPI guard; return the rendered diagnostics.
std::string lower_fail(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

// An `int` DPI import maps to C `int` (i32) by value: the SV call passes the
// value plane and reads the C return straight back (LRM 35.5.6). libc `abs`.
TEST_CASE("DPI import: int argument and return") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int abs(input int x);
          initial $display("%0d", abs(-7) + 1);
        endmodule)";
    CHECK(run_capture(src) == "8\n"); // abs(-7) == 7, used in arithmetic
}

// A `longint` maps to C `long long` (i64). libc `llabs`.
TEST_CASE("DPI import: longint round-trips 64 bits") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function longint llabs(input longint x);
          initial $display("%0d", llabs(-64'd5000000000));
        endmodule)";
    CHECK(run_capture(src) == "5000000000\n");
}

// `real` maps to C `double`; the value rides the fp form across the boundary.
// libm `sqrt` / `pow`, result narrowed to int to keep the assertion exact.
TEST_CASE("DPI import: real argument and return") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function real sqrt(input real x);
          import "DPI-C" function real pow(input real b, input real e);
          initial begin
            $display("%0d", int'(sqrt(144.0)));
            $display("%0d", int'(pow(2.0, 10.0)));
          end
        endmodule)";
    CHECK(run_capture(src) == "12\n1024\n");
}

// A `pure` import (LRM 35.6) is marked readnone/willreturn; it must still
// produce the right value. abs is effectively pure.
TEST_CASE("DPI import: pure function runs correctly") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" pure function int abs(input int x);
          initial $display("%0d", abs(-4));
        endmodule)";
    CHECK(run_capture(src) == "4\n");
}

TEST_CASE("DPI import: output argument") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function real frexp(input real x, output int e);
          initial begin
            real mant; int ex;
            mant = frexp(8.0, ex);
            $display("%0d %0d", ex, int'(mant * 1000.0));
          end
        endmodule)";
    CHECK(run_capture(src) == "4 500\n");
}

// A `chandle` (LRM 6.14) round-trips an opaque C pointer: it is lowered as a
// 2-state i64 and marshaled to/from `void*`. libc `malloc`/`free` are the ideal
// no-.so probe — malloc returns a chandle, free takes one; `null` / `==` / `!=`
// are integer ops on the handle bits.
TEST_CASE("DPI import: chandle round-trips an opaque pointer") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function chandle malloc(input longint size);
          import "DPI-C" function void    free(input chandle p);
          chandle h;
          initial begin
            h = malloc(64);
            $display("%b", h != null);   // malloc(64) succeeds -> non-null handle
            free(h);
            h = null;
            $display("%b", h == null);    // 1
          end
        endmodule)";
    CHECK(run_capture(src) == "1\n1\n");
}

TEST_CASE("DPI import: input string passes a NUL-terminated char*") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function longint strlen(input string s);
          initial begin
            $display("%0d", strlen("hello world"));   // 11
            $display("%0d", strlen(""));               // 0 (empty-string NUL)
          end
        endmodule)";
    CHECK(run_capture(src) == "11\n0\n");
}

// An open-array (`[]`) input formal (LRM 35.5.6.2) arrives as an svOpenArrayHandle
// whose bounds come from the actual. A named-scalar (`int`) element is zero-copy —
// Clotho's narrow `[N x i32]` element storage is already the C `int[]`, so the
// descriptor points straight at the design array (no marshal).
TEST_CASE("DPI import: open array of int is zero-copy") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int dpi_oa_sum_int(input int a[]);
          int arr[0:4];
          initial begin
            arr[0]=1; arr[1]=2; arr[2]=3; arr[3]=4; arr[4]=5;
            $display("%0d", dpi_oa_sum_int(arr));   // 15
          end
        endmodule)";
    CHECK(run_capture(src) == "15\n");
}

// A packed `logic[7:0]` element is marshalled into a contiguous svLogicVecVal buffer
// (rt_dpi_pack_arr_logic). x/z bits survive as aval/bval. Descending bounds ([3:1])
// verify the svLeft/svRight/svLow/svHigh index normalization.
TEST_CASE("DPI import: open array of logic marshals x/z (descending bounds)") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int dpi_oa_logic_planes(input logic [7:0] b[]);
          logic [7:0] la[3:1];
          initial begin
            la[3]=8'hA5; la[2]=8'b1010_xz01; la[1]=8'hff;
            $display("%0h", dpi_oa_logic_planes(la));
          end
        endmodule)";
    // aval_or = A5 | A9 | FF = FF; bval_or (x/z bits) = 00 | 0C | 00 = 0C.
    CHECK(run_capture(src) == "cff\n");
}

// A packed `bit[15:0]` (2-state) element marshals through svBitVecVal.
TEST_CASE("DPI import: open array of bit vector marshals to svBitVecVal") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int dpi_oa_bit_xor(input bit [15:0] b[]);
          bit [15:0] bv[0:2];
          initial begin
            bv[0]=16'h1234; bv[1]=16'hABCD; bv[2]=16'h0001;
            $display("%0h", dpi_oa_bit_xor(bv));   // 1234 ^ ABCD ^ 0001 = B9F8
          end
        endmodule)";
    CHECK(run_capture(src) == "b9f8\n");
}

// An `output` open array: the C writes elements back. A named-scalar (`int`)
// element is zero-copy, so the C writes the design storage directly (no copy-back).
TEST_CASE("DPI import: output open array (zero-copy write-back)") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function void dpi_oa_fill_sq(output int a[]);
          int arr[0:4];
          initial begin
            dpi_oa_fill_sq(arr);
            $display("%0d %0d %0d %0d %0d", arr[0], arr[1], arr[2], arr[3], arr[4]);
          end
        endmodule)";
    CHECK(run_capture(src) == "0 1 4 9 16\n");
}

// An `inout` open array of a packed `logic[7:0]` element: marshalled in, the C
// mutates it, and the scratch is unpacked back into the design storage after the
// call (rt_dpi_unpack_arr_logic).
TEST_CASE("DPI import: inout open array marshals in and back") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function void dpi_oa_inc_logic(inout logic [7:0] b[]);
          logic [7:0] la[0:2];
          initial begin
            la[0]=8'd10; la[1]=8'd20; la[2]=8'd99;
            dpi_oa_inc_logic(la);
            $display("%0d %0d %0d", la[0], la[1], la[2]);
          end
        endmodule)";
    CHECK(run_capture(src) == "11 21 100\n");
}

// A 33..64-bit packed element marshals into two svVecVal words per element (the
// mm plane is split through a 128-bit intermediate). bit[63:0] here.
TEST_CASE("DPI import: open array of 64-bit vector (two-chunk marshal)") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function longint dpi_oa_xor64(input bit [63:0] a[]);
          bit [63:0] a[0:2];
          initial begin
            a[0]=64'h1122334455667788; a[1]=64'hAAAABBBBCCCCDDDD; a[2]=64'h1;
            $display("%h", dpi_oa_xor64(a));
          end
        endmodule)";
    // 0x1122334455667788 ^ 0xAAAABBBBCCCCDDDD ^ 0x1 = 0xBB8888FF99AAAA54.
    CHECK(run_capture(src) == "bb8888ff99aaaa54\n");
}

// Out-of-scope constructs must fail loudly with a TODO, never silently miscompile.
// Each is a distinct guard in build_dpi_import.
TEST_CASE("DPI import: unsupported forms are rejected") {
    SUBCASE("output string argument (unbounded-buffer hazard, deferred)") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" function void f(output string s);
              string y;
              initial f(y);
            endmodule)";
        CHECK(lower_fail(src).find("output/inout string") != std::string::npos);
    }
    SUBCASE("DPI import task") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" task t(input int x);
              initial t(1);
            endmodule)";
        CHECK(lower_fail(src).find("DPI import tasks") != std::string::npos);
    }
    SUBCASE("open array of unsupported element (string)") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" function int f(input string a[]);
              string q[0:1];
              initial void'(f(q));
            endmodule)";
        CHECK(lower_fail(src).find("open-array element type") != std::string::npos);
    }
    SUBCASE("open array of packed element wider than 64 bits") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" function int f(input logic [71:0] a[]);
              logic [71:0] q[0:1];
              initial void'(f(q));
            endmodule)";
        CHECK(lower_fail(src).find("wider than 64 bits") != std::string::npos);
    }
}

// `context` (LRM 35.5.3) is accepted and lowers like a plain import — Clotho's
// single flat design state needs no svScope. Here it lowers and runs (libc strlen).
TEST_CASE("DPI import: context is accepted") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" context function longint strlen(input string s);
          initial $display("%0d", strlen("hello"));   // 5
        endmodule)";
    CHECK(run_capture(src) == "5\n");
}

// export "DPI-C" (LRM 35.5): the SV function is exposed as a C-callable thunk and
// still works as an ordinary SV function. The full C-calls-into-SV round-trip
// needs a .o linked via addObjectFile, so it is the verilator CLI cross-check
// (docs/design/dpi-c.md); here we confirm the export directive lowers, the thunk
// emits, and the design runs.
TEST_CASE("DPI export: a design with export directives lowers and runs") {
    // Exercises thunk emission for output/inout, packed-vector, and string export
    // formals (the SV functions also work SV-side). The C-calls-into-SV round-trip
    // for all these is the verilator CLI cross-check.
    constexpr std::string_view src = R"(
        module m;
          export "DPI-C" function dbl;
          export "DPI-C" function dm;
          export "DPI-C" function bvid;
          export "DPI-C" function slen;
          function int  dbl(input int x); return x * 2; endfunction
          function void dm(input int a, input int b, output int q, inout int r);
            q = a / b; r = r + a % b;
          endfunction
          function int  bvid(input bit [15:0] v); return v[7:0]; endfunction
          function int  slen(input string s); return s.len(); endfunction
          int rr;
          initial begin
            rr = 1; dm(17, 5, rr, rr);            // SV-side use of the exports
            $display("%0d %0d", dbl(21), rr);      // 42 3  (r = 1 + 17%5 = 3)
          end
        endmodule)";
    CHECK(run_capture(src) == "42 3\n");
}

// Export out-of-scope forms loud-fail (collect_dpi_exports guards).
TEST_CASE("DPI export: unsupported forms are rejected") {
    SUBCASE("exported task") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" task t;
              task t(input int x); endtask
            endmodule)";
        CHECK(lower_fail(src).find("DPI export tasks") != std::string::npos);
    }
    SUBCASE("exported string return (returned-char* lifetime)") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" function f;
              function string f(input int x); return "hi"; endfunction
            endmodule)";
        CHECK(lower_fail(src).find("string return is deferred") != std::string::npos);
    }
    SUBCASE("exported output string argument") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" function g;
              function void g(output string s); s = "x"; endfunction
            endmodule)";
        CHECK(lower_fail(src).find("output/inout string") != std::string::npos);
    }
}
