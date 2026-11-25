#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
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

// input bit[127:0] (> 64-bit -> inline-IR marshal, 4 chunks): XOR every 32-bit chunk
// of every element.
int dpi_oa_fold128(const svOpenArrayHandle h) {
    unsigned acc = 0;
    for (int i = svLow(h, 1); i <= svHigh(h, 1); ++i) {
        svBitVecVal v[4];
        svGetBitArrElem1VecVal(v, h, i);
        acc ^= v[0] ^ v[1] ^ v[2] ^ v[3];
    }
    return static_cast<int>(acc);
}

// inout logic[99:0] (> 64-bit, W not 32-aligned -> exercises the unaligned may
// extract/insert): overwrite each element with a definite 0x3<<96 | 0xF, clearing
// any x/z. Verifies the wide-IR write-back.
void dpi_oa_set100(const svOpenArrayHandle h) {
    for (int i = svLow(h, 1); i <= svHigh(h, 1); ++i) {
        svLogicVecVal v[4] = {};
        v[0].aval = 0xF;
        v[3].aval = 0x3;
        svPutLogicArrElem1VecVal(h, v, i);
    }
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

// JIT-run a module and capture stdout (mirrors string_test.cpp). The DPI imports
// resolve against libc / libm, which are dynamic symbols in the test process, so
// run_jit's process-symbol generator finds them with no --dpi-lib needed. The
// scalar marshaling (int / longint / real) is exercised end to end; the
// byte/shortint/bit/logic and svLogic x/z paths, and the --dpi-lib .so path, are
// exercised at the CLI instead (see docs/design/dpi-c.md). `config` carries a
// `--dpi-lib` object for the tests whose C calls an export.
std::string run_capture(std::string_view source, const ir::SimulationConfig& config = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module, nullptr, config);

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
    return r.diagnostic_text();
}

// Lower and emit textual LLVM IR. Thunk emission is driven by module_.dpi_exports,
// so the IR is where per-top export binding is observable — the C-side round trip
// itself needs a .o and stays the verilator CLI cross-check.
std::string emit_llvm_text(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*r.module, os));
    return text;
}

// The text of one emitted LLVM definition, from its `define` line to the closing
// brace, so a check can see what a thunk reaches whether or not the optimizer
// inlined the SV function into it.
std::string llvm_definition(const std::string& ir_text, std::string_view name) {
    const std::string needle = "@" + std::string(name) + "(";
    for (size_t at = ir_text.find(needle); at != std::string::npos; at = ir_text.find(needle, at + 1)) {
        const size_t nl   = ir_text.rfind('\n', at);
        const size_t line = nl == std::string::npos ? 0 : nl + 1;
        if (ir_text.compare(line, 7, "define ") != 0) {
            continue;
        }
        const size_t end = ir_text.find("\n}", at);
        REQUIRE(end != std::string::npos);
        return ir_text.substr(line, end - line);
    }
    FAIL("no LLVM definition of ", name);
    return {};
}

#if !defined(_WIN32)
// A simulation that links the relocatable object `object` (`--dpi-lib`): the C side
// of a design whose C calls an export. Only an object can, because the export's
// thunk lives in the JIT's own dylib, which a shared library cannot see.
ir::SimulationConfig linking(std::string_view object) {
    ir::SimulationConfig config;
    config.dpi_libs.emplace_back(object);
    return config;
}

// The same, in a child process: for a design that ends in a runtime fatal.
test::DesignRun run_captured_linking(std::string_view source, std::string_view object) {
    return test::run_design_captured(source, "CLOTHO_TEST_DPI_LIB=\"" + std::string(object) + "\" ");
}
#endif

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

// An `output` scalar (Phase 2): the C writes through a pointer; the backend
// copies the C scratch back into the caller temp, which the CopyOutArg path
// stores to the actual. libm `frexp(x, int* e)` returns the mantissa and writes
// the exponent through its output pointer. 8.0 = 0.5 * 2^4.
//
// The paths libc cannot reach are exercised at the CLI instead: scalar `inout`;
// scalar `logic` output/return incl. svLogic X/Z (exercises the z-elision fix in
// Analysis.cpp); and Phase 3 packed vectors — `bit[N]` in/out/inout (svBitVecVal),
// `logic[N]` carrying 0/1/x/z (svLogicVecVal, the aval/bval marshal), across
// multi-chunk and non-32-aligned widths.
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

// An `input string` passes the handle's bytes as a NUL-terminated `const char*`
// (zero-copy — the rt_string NUL invariant makes rt_string_data a valid C string).
// libc strlen is the no-.so probe. The return-string path (C returns char* ->
// rt_dpi_str_out) is covered at the CLI instead; libc has no deterministic
// char*-returning function.
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

// A > 64-bit packed element exceeds any native/128-bit integer, so its marshal is
// emitted as inline LLVM IR (native iW/i2W ops) instead of the C runtime. bit[127:0]
// input: four svVecVal chunks per element.
TEST_CASE("DPI import: open array of 128-bit vector (inline-IR marshal)") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int dpi_oa_fold128(input bit [127:0] a[]);
          bit [127:0] a[0:1];
          initial begin
            a[0]=128'h11112222_33334444_55556666_77778888;
            a[1]=128'hDEADBEEF_CAFEBABE_01234567_89ABCDEF;
            $display("%0d", dpi_oa_fold128(a));
          end
        endmodule)";
    // 0x9CE1_A791 folded across both elements; as a signed int (the return type).
    CHECK(run_capture(src) == "-1663368111\n");
}

// An inout > 64-bit element whose width is NOT 32-aligned (logic[99:0], W=100): the
// `may` region starts mid-word, so the inline-IR marshal's `lshr i2W, W` crosses a
// 32-bit boundary — the case a hand-written C bignum would need a funnel shift for.
// The C writes a definite value back, clearing the input's x/z.
TEST_CASE("DPI import: inout 100-bit element (unaligned wide write-back)") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function void dpi_oa_set100(inout logic [99:0] b[]);
          logic [99:0] b[0:0];
          initial begin
            b[0]={4'b10xz, 96'hDEADBEEF_00000000_0000FFFF};
            dpi_oa_set100(b);
            $display("%h", b[0]);
          end
        endmodule)";
    CHECK(run_capture(src) == "300000000000000000000000f\n");
}

// A dynamic-array actual (LRM 7.5) passed to an open-array formal: the runtime
// { data, len } handle drives the descriptor — nelem is the runtime size, and a
// named-scalar (int) element is zero-copy (the descriptor points at the heap
// buffer, read and output-written in place). Same C fns as the fixed-array tests;
// only the actual's storage kind differs.
TEST_CASE("DPI import: dynamic-array actual is zero-copy (runtime size)") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int  dpi_oa_sum_int(input int a[]);
          import "DPI-C" function void dpi_oa_fill_sq(output int a[]);
          int da[];
          initial begin
            da = new[5];
            da[0]=10; da[1]=20; da[2]=30; da[3]=40; da[4]=50;
            $display("%0d", dpi_oa_sum_int(da));    // 150
            dpi_oa_fill_sq(da);
            $display("%0d %0d %0d %0d %0d", da[0], da[1], da[2], da[3], da[4]); // 0 1 4 9 16
          end
        endmodule)";
    CHECK(run_capture(src) == "150\n0 1 4 9 16\n");
}

// An unallocated / empty dynamic array (size 0, LRM 7.5): the descriptor carries
// nelem 0, and svLow/svHigh return an empty span so a `svLow..svHigh` C loop reads
// nothing (no null-buffer deref).
TEST_CASE("DPI import: empty dynamic-array actual reads as size 0") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int dpi_oa_sum_int(input int a[]);
          int da[];
          initial $display("%0d", dpi_oa_sum_int(da));   // 0, no crash
        endmodule)";
    CHECK(run_capture(src) == "0\n");
}

// Out-of-scope constructs must fail loudly with a TODO, never silently miscompile
// (PRINCIPLES.md). Each is a distinct guard in build_dpi_import.
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
    SUBCASE("`pure` on an imported task (LRM 35.5.1.3)") {
        // "An imported task can never be declared pure." Clotho's severity policy
        // makes slang's -Wdpi-pure-task an error; accepting it would let is_pure
        // mark the C declaration readnone + willreturn, and the optimizer CSE or
        // delete a call kept purely for its side effects.
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" pure task t(input int x);
              initial t(1);
            endmodule)";
        CHECK(lower_fail(src).find("DPI tasks cannot be marked 'pure'") != std::string::npos);
    }
    SUBCASE("an import task enabled from a function (inherited from slang)") {
        // LRM 35.8's SV-side half: a function may not perform a task enable, so a
        // DPI import task is unreachable from a function body. slang rejects this
        // before lowering ("cannot invoke a task from within a function"), which is
        // what makes the export-thunk legality check the only run-time piece.
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" task t(input int x);
              function int f(input int y); t(y); return y; endfunction
              initial $display("%0d", f(1));
            endmodule)";
        CHECK(lower_fail(src).find("cannot invoke a task from within a function") != std::string::npos);
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

// A DPI import TASK (LRM 35.8), end to end against libc. `puts` is the exact
// shape a task import needs from the harness: it takes the `input string` the
// import path already marshals zero-copy, and it returns `int` -- which is what
// an import task's C prototype returns anyway (35.8's disable channel), and which
// the SV side never sees. So this covers the whole T1 import path with no
// --dpi-lib: the i32 C return is declared and discarded, and the call site stays
// an ordinary plain call rather than growing a task ramp.
TEST_CASE("DPI import: a task lowers, runs, and discards its int return") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" context puts = task sv_puts(input string s);
          initial begin
            sv_puts("from a DPI import task");
            $display("back in SV");
          end
        endmodule)";
    CHECK(run_capture(src) == "from a DPI import task\nback in SV\n");
}

// The C declaration an import task gets: `int`-returning (LRM 35.8) even though
// the SV task has no result, and reached by a plain `call` -- no coroutine ramp,
// no task record -- because T1's import call site cannot suspend.
TEST_CASE("DPI import: a task declares an int-returning C prototype") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" context task t(input int x);
          initial t(7);
        endmodule)";
    const std::string ir = emit_llvm_text(src);
    CHECK(ir.find("declare i32 @t(i32)") != std::string::npos);
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

// An exported function may draw from the RNG (LRM 18.13) even though the C entry
// that calls it has no calling process: the thunk passes the module-wide state,
// which main seeds at startup. Emitting the thunk is what this covers -- the
// C-calls-into-SV half is the verilator CLI cross-check -- plus the SV-side use,
// where the draws come from the calling process instead.
TEST_CASE("DPI export: an exported function may draw from the RNG") {
    constexpr std::string_view src = R"(
        module m;
          export "DPI-C" function rnd;
          function int rnd(); return $urandom; endfunction
          int a, b;
          initial begin a = rnd(); b = rnd(); $display("%0d", a != b); end
        endmodule)";
    CHECK(run_capture(src) == "1\n");
}

// Export out-of-scope forms loud-fail (collect_dpi_exports guards).
TEST_CASE("DPI export: unsupported forms are rejected") {
    // LRM 35.8: "All aspects of exported functions described above in 35.7 apply to
    // exported tasks." So an exported TASK inherits every refusal the function path
    // has, rather than slipping past them on a task-only code path.
    SUBCASE("exported task: output string argument (inherited refusal)") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" task t;
              task t(output string s); s = "x"; endtask
            endmodule)";
        CHECK(lower_fail(src).find("output/inout string") != std::string::npos);
    }
    SUBCASE("exported task: unpacked-aggregate argument (inherited refusal)") {
        constexpr std::string_view src = R"(
            module m;
              typedef struct { int a; int b; } p_t;
              export "DPI-C" task t;
              task t(input p_t p); endtask
            endmodule)";
        CHECK(lower_fail(src).find("export argument type is not supported") != std::string::npos);
    }
    SUBCASE("exported task: inout string argument (inherited refusal)") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" task t;
              task t(inout string s); s = "x"; endtask
            endmodule)";
        CHECK(lower_fail(src).find("output/inout string") != std::string::npos);
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

// export "DPI-C" task (LRM 35.8): the SV task is exposed as a C-callable thunk and
// still works as an ordinary SV task. As for exported functions, the C-calls-into-
// SV round trip needs a .o linked through addObjectFile, which the doctest harness
// does not wire -- that half is the verilator CLI cross-check recorded in
// docs/design/dpi-c.md (measured byte-identical). Here: the directive lowers, the
// thunk emits, and the design runs.
TEST_CASE("DPI export: a design with an exported task lowers and runs") {
    constexpr std::string_view src = R"(
        module m;
          export "DPI-C" task sv_note;
          export "DPI-C" function get_base;
          int base;
          task sv_note(input int v, output int doubled);
            doubled = v * 2 + base;
          endtask
          function int get_base(); return base; endfunction
          int r;
          initial begin
            base = 100;
            sv_note(4, r);                       // SV-side use of the exported task
            $display("%0d %0d", r, get_base());  // 108 100
          end
        endmodule)";
    CHECK(run_capture(src) == "108 100\n");
}

// The export-TASK thunk's shape, which is where T1 actually lives. Four things
// have to be true of it and none is observable from SV: the C prototype returns
// i32 (LRM 35.8, not the SV task's void), the callee is the task COROUTINE driven
// through its resume/destroy pointers rather than a plain function call, a task
// that fails to complete reaches the T2 fatal, and an entry with no imported-task
// frame live reaches the 35.8 legality fatal.
//
// emit_llvm_text runs the coroutine passes, so the llvm.coro.* intrinsics are
// already gone here -- CoroSplit has rewritten them into the ramp's resume /
// destroy function pointers. Match what survives that: the task coroutine symbol
// (`m.task.<name>`, not the `m.fn.<name>` a plain function would give) and the
// three arms the thunk branches into.
TEST_CASE("DPI export: the exported-task thunk drives a coroutine and guards both fatals") {
    constexpr std::string_view src = R"(
        module m;
          export "DPI-C" task sv_note;
          task sv_note(input int v, output int doubled); doubled = v * 2; endtask
          int r;
          initial sv_note(1, r);
        endmodule)";
    const std::string thunk = llvm_definition(emit_llvm_text(src), "sv_note");
    CHECK(thunk.find("define i32 @sv_note(") != std::string::npos);
    CHECK(thunk.find(".task.sv_note") != std::string::npos);
    CHECK(thunk.find("dpi.task.done") != std::string::npos);
    CHECK(thunk.find("dpi.task.suspended") != std::string::npos);
    CHECK(thunk.find("dpi.task.no_frame") != std::string::npos);
    CHECK(thunk.find("@clotho.dpi.in_import_task") != std::string::npos);
    CHECK(thunk.find("rt_dpi_export_task_suspended") != std::string::npos);
    CHECK(thunk.find("rt_dpi_export_task_no_task_frame") != std::string::npos);
    // 0 = "no disable is active on the current execution thread" (LRM 35.8).
    CHECK(thunk.find("ret i32 0") != std::string::npos);
}

// One c_identifier exported from several scopes (LRM 35.4) is one C symbol. slang
// keeps only the first of them, so the frontend collects every scope's directive
// itself, and the one thunk picks the instance by the scope current when C calls it
// (svSetScope, LRM 35.5.3). A task shares the symbol exactly as a function does.
TEST_CASE("DPI export: one c_identifier from two scopes is one thunk that dispatches on the scope") {
    constexpr std::string_view src = R"(
        module ma;
          export "DPI-C" dup = task t;
          task t(input int x); endtask
        endmodule
        module mb;
          export "DPI-C" dup = task u;
          task u(input int x); endtask
        endmodule)";
    const std::string ir = emit_llvm_text(src);
    size_t             n = 0;
    for (size_t at = ir.find("define i32 @dup("); at != std::string::npos;
         at      = ir.find("define i32 @dup(", at + 1)) {
        ++n;
    }
    CHECK(n == 1);
    const std::string thunk = llvm_definition(ir, "dup");
    CHECK(thunk.find("rt_svdpi_scope_index") != std::string::npos);
    CHECK(thunk.find("rt_dpi_export_no_scope") != std::string::npos);
    CHECK(thunk.find(".task.t") != std::string::npos);
    CHECK(thunk.find(".task.u") != std::string::npos);
}

// The same module instantiated twice exports its function from two scopes, which is
// the shape slang's own dedup hid most often.
TEST_CASE("DPI export: two instances of one module dispatch, one instance does not") {
    constexpr std::string_view two = R"(
        module m;
          export "DPI-C" function who;
          function int who(); return 1; endfunction
        endmodule
        module top; m u0(); m u1(); endmodule)";
    const std::string thunk = llvm_definition(emit_llvm_text(two), "who");
    CHECK(thunk.find("rt_svdpi_scope_index") != std::string::npos);
    CHECK(thunk.find("rt_dpi_export_no_scope") != std::string::npos);

    // A c_identifier with one exporting scope has nothing to choose between, so its
    // thunk is the plain call it has always been (C that reaches a package's export
    // from whatever scope it happens to run in keeps working).
    constexpr std::string_view one = R"(
        module m;
          export "DPI-C" function who;
          function int who(); return 1; endfunction
        endmodule
        module top; m u(); endmodule)";
    const std::string plain = llvm_definition(emit_llvm_text(one), "who");
    CHECK(plain.find("rt_svdpi_scope_index") == std::string::npos);
    CHECK(plain.find("rt_dpi_export_no_scope") == std::string::npos);
}

// Only a `context` import is instrumented (LRM 35.5.3): its call sets the current
// scope to the import's declaration scope for the duration of the call.
TEST_CASE("DPI import: only a context import call sets the scope") {
    SUBCASE("context") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" context function int cf(input int x);
              initial $display("%0d", cf(1));
            endmodule)";
        const std::string ir = emit_llvm_text(src);
        CHECK(ir.find("rt_svdpi_scope_enter") != std::string::npos);
        CHECK(ir.find("rt_svdpi_scope_leave") != std::string::npos);
    }
    SUBCASE("plain import") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" function int abs(input int x);
              initial $display("%0d", abs(-1));
            endmodule)";
        const std::string ir = emit_llvm_text(src);
        CHECK(ir.find("rt_svdpi_scope_enter") == std::string::npos);
        CHECK(ir.find("rt_svdpi_scope_leave") == std::string::npos);
    }
}

// Nothing of the scope machinery is emitted for a design that does not use it.
TEST_CASE("DPI scope: a design without DPI carries no scope machinery") {
    constexpr std::string_view src = R"(
        module m;
          initial $display("hi");
        endmodule)";
    CHECK(emit_llvm_text(src).find("rt_svdpi") == std::string::npos);
}

// An exported FUNCTION keeps the plain-call thunk it always had: no coroutine
// drive, no caller-identity load, no task fatals. The task arm is additive.
TEST_CASE("DPI export: an exported function's thunk is unchanged by the task path") {
    constexpr std::string_view src = R"(
        module m;
          export "DPI-C" function dbl;
          function int dbl(input int x); return x * 2; endfunction
          initial $display("%0d", dbl(21));
        endmodule)";
    const std::string thunk = llvm_definition(emit_llvm_text(src), "dbl");
    CHECK(thunk.find("define i32 @dbl(") != std::string::npos);
    CHECK(thunk.find("llvm.coro.resume") == std::string::npos);
    CHECK(thunk.find("rt_dpi_export_task_suspended") == std::string::npos);
    CHECK(thunk.find("rt_dpi_export_task_no_task_frame") == std::string::npos);
    CHECK(thunk.find("clotho.dpi.caller_proc") == std::string::npos);
}

// A multi-top design (LRM 23.3.1) may export from every top. slang hands Clotho one
// entry per c_identifier, and each is collected by the Lowerer of the top that owns
// it, so its thunk calls that top's own lowered function, handing it that instance's
// record (@clotho.inst.<top>.ctx) -- nothing is resolved by scope at the C boundary.
TEST_CASE("DPI export: multi-top design exports from each top") {
    constexpr std::string_view src = R"(
        module ta;
          int va;
          export "DPI-C" function ta_get;
          function int ta_get(); return va; endfunction
          initial begin va = 7; #1 $display("ta %0d", ta_get()); end
        endmodule
        module tb;
          int vb;
          export "DPI-C" function tb_get;
          function int tb_get(); return vb; endfunction
          initial begin vb = 9; #2 $display("tb %0d", tb_get()); end
        endmodule)";
    // Both tops lower and run, each function reading its own module's signal.
    CHECK(run_capture(src) == "ta 7\ntb 9\n");

    // One thunk per export, each reaching only its own top's state. A c_identifier
    // collected twice would have been renamed `<name>.1` by llvm::Function::Create,
    // so the absence of a rename is the routing invariant the frontend's duplicate
    // check also guards.
    const std::string ir_text = emit_llvm_text(src);
    CHECK(ir_text.find("@ta_get.1") == std::string::npos);
    CHECK(ir_text.find("@tb_get.1") == std::string::npos);

    const std::string ta_thunk = llvm_definition(ir_text, "ta_get");
    CHECK((ta_thunk.find("clotho.inst.ta.") != std::string::npos || ta_thunk.find(".fn.ta_get") != std::string::npos));
    CHECK(ta_thunk.find("clotho.inst.tb.") == std::string::npos);
    CHECK(ta_thunk.find(".fn.tb_get") == std::string::npos);

    const std::string tb_thunk = llvm_definition(ir_text, "tb_get");
    CHECK((tb_thunk.find("clotho.inst.tb.") != std::string::npos || tb_thunk.find(".fn.tb_get") != std::string::npos));
    CHECK(tb_thunk.find("clotho.inst.ta.") == std::string::npos);
    CHECK(tb_thunk.find(".fn.ta_get") == std::string::npos);
}

// A `$unit`- or package-scope export has no instance multiplicity (LRM 6.8): it
// lowers once through the shared globals Lowerer, so exactly one top may collect it.
// Collecting it per top would trip the duplicate-c_identifier guard, and unguarded
// would let llvm::Function::Create rename the second thunk and bind C to whichever
// won.
TEST_CASE("DPI export: a $unit-scope export is collected once in a multi-top design") {
    constexpr std::string_view src = R"(
        function int u_get(); return 5; endfunction
        export "DPI-C" function u_get;
        module ta;
          int va;
          export "DPI-C" function ta_get;
          function int ta_get(); return va + u_get(); endfunction
          initial begin va = 7; #1 $display("ta %0d", ta_get()); end
        endmodule
        module tb;
          export "DPI-C" function tb_get;
          function int tb_get(); return u_get() * 2; endfunction
          initial begin #2 $display("tb %0d", tb_get()); end
        endmodule)";
    CHECK(run_capture(src) == "ta 12\ntb 10\n");

    const std::string ir_text = emit_llvm_text(src);
    CHECK(ir_text.find("@u_get.1") == std::string::npos);
    // The one $unit thunk belongs to no instance, so it reaches no instance state.
    CHECK(llvm_definition(ir_text, "u_get").find("clotho.inst.") == std::string::npos);
}

// A design with NO top module at all (a bare package, or -- the real-world
// trigger -- an interface-only source paired with a package,
// uvm-examples/apb_protocol_monitor and uvm_alu_config_analysis) still
// collects its package-scope export. lower_compilation's per-top loop is what
// normally admits the one-time $unit/package collection ("collect_globals=true
// on the first top"); with zero tops that loop never runs at all, so nothing
// collected it -- a JIT-time "Symbols not found" for any native code that calls
// back into the export (uvm_dpi.o calling m__uvm_report_dpi is the real-world
// trigger). No process reaches the export from the SV side either (there is
// nothing to run), so this checks the thunk emits, not a runtime result.
TEST_CASE("DPI export: a package-scope export is collected in a design with no top module") {
    constexpr std::string_view src = R"(
        package p;
          export "DPI-C" function f;
          function int f(input int x);
            return x * 2;
          endfunction
        endpackage)";
    const std::string ir_text = emit_llvm_text(src);
    CHECK(llvm_definition(ir_text, "f").find("clotho.inst.") == std::string::npos);
}

#if !defined(_WIN32)

// -- svScope (LRM 35.5.3, H.9.3) --
//
// What C sees as the current scope, and so which instance a c_identifier exported
// from several scopes reaches. The C side is tests/dpi_scope/dpi_scope.c, a
// relocatable object linked into the JIT (only an object can call an export); each
// step is a small import and the design prints every result.
//
// No oracle: iverilog's DPI is VPI-based and Verilator keeps one svScope per top, so
// the expected output is derived from the LRM text, read from the public 1800-2017
// edition (the 2023 PDF was not available; the DPI clause numbers are unchanged).
// 35.5.3: "When an import invokes the svSetScope utility prior to calling the export,
// it sets the context explicitly. Otherwise, the context will be the context of the
// instantiated scope where the import declaration is located." H.9.3: svGetScope is
// "the scope of the function's declaration site, not call site", svSetScope "returns
// the previous active scope", svGetScopeFromName "shall be NULL for unrecognized scope
// names" and "it is never possible to share user data storage across different
// contexts".

// The reported bug: two instances of one module export `who`, and C picks one with
// svSetScope. Clotho bound C's `who` to the first instance whatever the scope.
TEST_CASE("DPI scope: svSetScope selects the instance a shared export runs in") {
    constexpr std::string_view src = R"(
        module m #(parameter int ID = 0);
          export "DPI-C" function who;
          function int who(); return ID; endfunction
        endmodule
        module top;
          m #(10) u0();
          m #(11) u1();
          import "DPI-C" function chandle c_scope(input string name);
          import "DPI-C" context function int c_who_in(input chandle s);
          initial begin
            $display("u1 %0d", c_who_in(c_scope("top.u1")));
            $display("u0 %0d", c_who_in(c_scope("top.u0")));
            $display("u1 %0d", c_who_in(c_scope("top.u1")));
          end
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "u1 11\nu0 10\nu1 11\n");
}

// Without svSetScope the export runs where the import is declared, so each instance's
// own import reaches its own export. A call chain that leaves for another scope (the
// export calls a package's context import) returns to the scope it left: the C sees
// its own scope again afterwards.
TEST_CASE("DPI scope: a context import runs in its declaration scope and the scope is restored") {
    constexpr std::string_view src = R"(
        package pk;
          import "DPI-C" context function string c_here();
        endpackage
        module m #(parameter int ID = 0);
          export "DPI-C" function who;
          import "DPI-C" context function string c_here();
          import "DPI-C" context function int c_who_here();
          string trail;
          function int who();
            trail = pk::c_here();
            return ID;
          endfunction
          string here;
          int    r;
          initial begin
            #(ID);
            here = c_here();
            r    = c_who_here();
            $display("%s %0d %s", here, r, trail);
          end
        endmodule
        module top;
          m #(1) u0();
          m #(2) u1();
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "top.u0 1 pk\ntop.u1 2 pk\n");
}

// svGetScopeFromName finds every scope that has an export or a context import, however
// deep, and answers NULL for a name that is none; svSetScope returns the scope that was
// current and svGetScope then sees the new one.
TEST_CASE("DPI scope: names resolve for nested scopes, and svSetScope swaps") {
    constexpr std::string_view src = R"(
        module inner;
          import "DPI-C" context function string c_here();
          string s;
          initial s = c_here();
        endmodule
        module outer;
          inner i();
        endmodule
        module top;
          outer o();
          import "DPI-C" function chandle c_scope(input string name);
          import "DPI-C" function string c_name(input chandle s);
          import "DPI-C" context function int c_swap(input chandle s);
          initial begin
            $display("%s", c_name(c_scope("top.o.i")));
            $display("%s", c_name(c_scope("top")));
            $display("%s", c_name(c_scope("top.nope")));
            $display("%0d", c_swap(c_scope("top.o.i")));
          end
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "top.o.i\ntop\n(null)\n3\n");
}

// User data belongs to a scope: the same context import declared in two instances
// counts separately, because the two run under different values of svScope.
TEST_CASE("DPI scope: user data is per instance") {
    constexpr std::string_view src = R"(
        module leaf #(parameter int ID = 0);
          import "DPI-C" context function int c_count();
          import "DPI-C" context function string c_here();
          int n1, n2;
          initial begin
            #(ID);
            n1 = c_count();
            n2 = c_count();
            $display("%s %0d %0d", c_here(), n1, n2);
          end
        endmodule
        module top;
          leaf #(1) a();
          leaf #(2) b();
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "top.a 1 2\ntop.b 1 2\n");
}

// A generate block is a scope of its own (LRM H.9.2): the export of each iteration is
// reached by the block's name, though all of them sit in one instance.
TEST_CASE("DPI scope: generate blocks are scopes") {
    constexpr std::string_view src = R"(
        module top;
          for (genvar i = 0; i < 2; i++) begin : blk
            export "DPI-C" function who;
            function int who(); return 100 + i; endfunction
          end
          import "DPI-C" function chandle c_scope(input string name);
          import "DPI-C" function string c_name(input chandle s);
          import "DPI-C" context function int c_who_in(input chandle s);
          initial begin
            $display("%s %0d", c_name(c_scope("top.blk[1]")), c_who_in(c_scope("top.blk[1]")));
            $display("%s %0d", c_name(c_scope("top.blk[0]")), c_who_in(c_scope("top.blk[0]")));
          end
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "top.blk[1] 101\ntop.blk[0] 100\n");
}

// A package and a module may export one c_identifier too (LRM 35.4: different scopes),
// and a package's scope is its name.
TEST_CASE("DPI scope: a package and a module export one c_identifier") {
    constexpr std::string_view src = R"(
        package p;
          export "DPI-C" function who;
          function int who(); return 7; endfunction
        endpackage
        module top;
          export "DPI-C" who = function mine;
          function int mine(); return 8; endfunction
          import "DPI-C" function chandle c_scope(input string name);
          import "DPI-C" context function int c_who_in(input chandle s);
          initial begin
            $display("p %0d", c_who_in(c_scope("p")));
            $display("top %0d", c_who_in(c_scope("top")));
          end
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "p 7\ntop 8\n");
}

// Each of several top-level modules is a scope, and exports from different tops share a
// c_identifier like any others.
TEST_CASE("DPI scope: every top is a scope") {
    constexpr std::string_view src = R"(
        module ta;
          export "DPI-C" function who;
          function int who(); return 1; endfunction
        endmodule
        module tb;
          export "DPI-C" function who;
          function int who(); return 2; endfunction
          import "DPI-C" function chandle c_scope(input string name);
          import "DPI-C" context function int c_who_in(input chandle s);
          initial begin
            $display("ta %0d", c_who_in(c_scope("ta")));
            $display("tb %0d", c_who_in(c_scope("tb")));
          end
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "ta 1\ntb 2\n");
}

// An exported task is reached the same way (LRM 35.8: all of 35.7 applies to it).
TEST_CASE("DPI scope: svSetScope selects the instance a shared exported task runs in") {
    constexpr std::string_view src = R"(
        module m #(parameter int ID = 0);
          export "DPI-C" task who;
          task who(output int o); o = ID; endtask
        endmodule
        module top;
          m #(5) u0();
          m #(6) u1();
          import "DPI-C" function chandle c_scope(input string name);
          import "DPI-C" context task c_who_task(input chandle s, output int r);
          int r;
          initial begin
            c_who_task(c_scope("top.u1"), r);
            $display("u1 %0d", r);
            c_who_task(c_scope("top.u0"), r);
            $display("u0 %0d", r);
          end
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_TASK_OBJECT)) == "u1 6\nu0 5\n");
}

// An export that exists in several scopes but not in the one that is current has no
// instance to run: the call is a located fatal rather than a guess at the first one.
TEST_CASE("DPI scope: a shared export called from a scope that lacks it is a located fatal") {
    constexpr std::string_view src = R"(
        module m #(parameter int ID = 0);
          export "DPI-C" function who;
          function int who(); return ID; endfunction
        endmodule
        module top;
          m #(1) u0();
          m #(2) u1();
          import "DPI-C" context function int c_who_here();
          initial $display("%0d", c_who_here());
        endmodule)";
    const test::DesignRun run = run_captured_linking(src, CLOTHO_DPI_SCOPE_OBJECT);
    CHECK(test::design_exit_code(run) == 1);
    CHECK(run.out.empty());
    CHECK(run.err.find("FATAL: DPI export 'who'") != std::string::npos);
    // It names the scope C was in, and every scope that does export it.
    CHECK(run.err.find("'top'") != std::string::npos);
    CHECK(run.err.find("top.u0") != std::string::npos);
    CHECK(run.err.find("top.u1") != std::string::npos);
    CHECK(run.err.find("svSetScope") != std::string::npos);
}

// A c_identifier exported from ONE scope has nothing to choose between, so C reaches it
// whatever scope is current: native code that calls a package's export from the scope of
// an unrelated import (UVM's m__uvm_report_dpi) is not an error.
TEST_CASE("DPI scope: an export from a single scope is reached from any scope") {
    constexpr std::string_view src = R"(
        module m;
          export "DPI-C" function who;
          function int who(); return 42; endfunction
        endmodule
        module top;
          m u();
          import "DPI-C" context function int c_who_here();
          initial $display("%0d", c_who_here());
        endmodule)";
    CHECK(run_capture(src, linking(CLOTHO_DPI_SCOPE_OBJECT)) == "42\n");
}

#endif // !defined(_WIN32)
