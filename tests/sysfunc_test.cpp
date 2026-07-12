#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "ir/Jit.h"

#include <cstdio>
#include <unistd.h>

using namespace clotho;

namespace {

// JIT the design and capture everything it writes to stdout. Mirrors the helper
// in the other backend tests.
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

std::string prog(std::string_view decls, std::string_view body) {
    return "module m;\n" + std::string(decls) + "\n initial begin\n" + std::string(body) +
           "\n $finish;\n end\nendmodule\n";
}

// Confirm lowering fails loudly for an unsupported construct.
std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

// -- $bits ------------------------------------------------------------------

// $bits on a fixed-size operand is an elaboration constant (slang folds it),
// independent of the operand's runtime value. All values iverilog-verified.
TEST_CASE("$bits of fixed-size operands folds to the type width") {
    const std::string out = run_capture(prog(
      "logic [7:0] v; logic [127:0] wide; bit [15:0] two; logic [7:0] arr [0:3];",
      "$display(\"%0d %0d %0d %0d\", $bits(v), $bits(wide), $bits(two), $bits(arr));"));
    CHECK(out == "8 128 16 32\n");
}

// A dynamic string is sized at call time: 8 bits per current character, 0 when
// empty (LRM 20.6.2). iverilog returns a constant 1 for any string — a known
// iverilog limitation; the LRM (and the byte count) makes 8*len correct.
TEST_CASE("$bits of a dynamic string is 8 per character") {
    const std::string out = run_capture(
      prog("string s;",
           "s = \"hello\"; $display(\"%0d\", $bits(s));"
           "s = \"hi\";    $display(\"%0d\", $bits(s));"
           "s = \"\";      $display(\"%0d\", $bits(s));"));
    CHECK(out == "40\n16\n0\n");
}

// $bits usable as an elaboration-time constant in a parameter / type context.
TEST_CASE("$bits as an elaboration-time constant") {
    const std::string out = run_capture(prog(
      "logic [7:0] arr [0:3]; localparam int BW = $bits(arr); logic [$bits(arr)-1:0] packed_copy;",
      "$display(\"%0d %0d\", BW, $bits(packed_copy));"));
    CHECK(out == "32 32\n");
}

// -- $countones -------------------------------------------------------------

// Only definite-1 bits count; x/z never do (matches `=== 1'b1`). Constant and
// runtime operands, narrow and wide, all iverilog-verified.
TEST_CASE("$countones counts definite-1 bits") {
    const std::string out =
      run_capture(prog("logic [7:0] c; logic [127:0] wide;",
                       "c = 8'b101x_z01x;"
                       "wide = 128'h0; wide[0] = 1; wide[64] = 1; wide[127] = 1;"
                       "$display(\"%0d %0d %0d\", $countones(8'hFF), $countones(c), $countones(wide));"));
    CHECK(out == "8 3 3\n");
}

// A 2-state operand carries no x/z: every set bit counts, isunknown is 0.
TEST_CASE("$countones / $isunknown on a 2-state operand") {
    const std::string out = run_capture(prog("bit [15:0] two;",
                                             "two = 16'b1010_1010_1010_1010;"
                                             "$display(\"%0d %0b\", $countones(two), $isunknown(two));"));
    CHECK(out == "8 0\n");
}

// -- $countbits -------------------------------------------------------------

// $countbits tallies the bits matching any listed control value 0/1/x/z (LRM
// 20.9). Values iverilog-verified against 8'b101x_z01x (ones {7,5,1}, zeros
// {6,2}, x {4,0}, z {3}).
TEST_CASE("$countbits over each control class") {
    const std::string out = run_capture(prog("logic [7:0] c;",
                                             "c = 8'b101x_z01x;"
                                             "$display(\"%0d %0d %0d %0d %0d\","
                                             " $countbits(c, 1'b1), $countbits(c, 1'b0),"
                                             " $countbits(c, 1'bx), $countbits(c, 1'bz),"
                                             " $countbits(c, 1'bx, 1'bz));"));
    CHECK(out == "3 2 2 1 3\n");
}

// -- $onehot / $onehot0 -----------------------------------------------------

// $onehot: exactly one definite-1 bit. $onehot0: at most one. x/z corners
// iverilog-verified.
TEST_CASE("$onehot / $onehot0 semantics including x/z") {
    const std::string out = run_capture(prog("logic [3:0] a;",
                                             "a = 4'b0100; $display(\"%0b %0b\", $onehot(a), $onehot0(a));"
                                             "a = 4'b0000; $display(\"%0b %0b\", $onehot(a), $onehot0(a));"
                                             "a = 4'b0110; $display(\"%0b %0b\", $onehot(a), $onehot0(a));"
                                             "a = 4'b01x0; $display(\"%0b %0b\", $onehot(a), $onehot0(a));"
                                             "a = 4'b000x; $display(\"%0b %0b\", $onehot(a), $onehot0(a));"
                                             "a = 4'b1zzz; $display(\"%0b %0b\", $onehot(a), $onehot0(a));"));
    CHECK(out == "1 1\n0 1\n0 0\n1 1\n0 1\n1 1\n");
}

// $onehot / $onehot0 fold in a constant context.
TEST_CASE("$onehot in a constant context") {
    const std::string out = run_capture(
      prog("localparam bit OH = $onehot(4'b0100); localparam bit OH0 = $onehot0(4'b0);",
           "$display(\"%0b %0b\", OH, OH0);"));
    CHECK(out == "1 1\n");
}

// -- $isunknown -------------------------------------------------------------

// $isunknown: 1 iff any bit is x or z. iverilog-verified.
TEST_CASE("$isunknown detects any x or z") {
    const std::string out = run_capture(prog("logic [7:0] v;",
                                             "v = 8'hA5;   $display(\"%0b\", $isunknown(v));"
                                             "v = 8'b0000_000x; $display(\"%0b\", $isunknown(v));"
                                             "v = 8'b0000_000z; $display(\"%0b\", $isunknown(v));"));
    CHECK(out == "0\n1\n1\n");
}

// -- Results feed further expressions ---------------------------------------

// The counts are ordinary 32-bit ints: usable in arithmetic / conditions.
TEST_CASE("bit-query results feed expressions") {
    const std::string out = run_capture(prog("logic [7:0] v;",
                                             "v = 8'hFF;"
                                             "if ($countones(v) == 8) $display(\"all set\");"
                                             "$display(\"%0d\", $countones(v) + $bits(v));"));
    CHECK(out == "all set\n16\n");
}

// -- $typename --------------------------------------------------------------

TEST_CASE("$typename formats the supported type kinds") {
    const std::string out = run_capture(prog(
      "typedef bit node; bit b; logic l; reg r; int i; integer ig; byte by;"
      " shortint sh; longint lo; time tm; bit [7:0] bv; logic [15:0] lv;"
      " logic signed [7:0] sv; bit [0:3] rev; node [2:0] nn; int unsigned iu;"
      " logic [7:0] arr [0:9]; string s; real rr; shortreal sr;",
      "$write(\"%s|%s|%s|%s|%s|%s|%s|\", $typename(b), $typename(l), $typename(r),"
      " $typename(i), $typename(ig), $typename(by), $typename(sh));"
      "$write(\"%s|%s|%s|%s|%s|%s|\", $typename(lo), $typename(tm), $typename(bv),"
      " $typename(lv), $typename(sv), $typename(rev));"
      "$display(\"%s|%s|%s|%s|%s|%s\", $typename(nn), $typename(iu), $typename(arr),"
      " $typename(s), $typename(rr), $typename(sr));"));
    CHECK(out ==
          "bit|logic|reg|int|integer|byte|shortint|"
          "longint|time|bit[7:0]|logic[15:0]|logic signed[7:0]|bit[0:3]|"
          "bit[2:0]|int unsigned|logic[7:0]$[0:9]|string|real|shortreal\n");
}

// A data-type argument names the same type as an expression of that type.
TEST_CASE("$typename of a data-type argument") {
    const std::string out = run_capture(
      prog("", "$display(\"%s %s\", $typename(logic [7:0]), $typename(int));"));
    CHECK(out == "logic[7:0] int\n");
}

// enum / struct / union / class are legal types whose $typename string
// Clotho does not produce yet, so $typename rejects them loudly rather than
// fabricating a string.
TEST_CASE("$typename of an unsupported type kind fails loudly") {
    CHECK(lower_error("module m; typedef enum {A,B} e; e x; initial $display($typename(x)); endmodule")
            .find("$typename") != std::string::npos);
    CHECK(lower_error("module m; typedef struct packed {logic a;} s; s x;"
                      " initial $display($typename(x)); endmodule")
            .find("$typename") != std::string::npos);
}

// -- $random(seed) / $dist_* / $urandom (LRM 20.15, 18.13) ------------------
//
// The $dist_* routines and seeded $random are the IEEE reference algorithm
// (mandated verbatim), so the expected values below are byte-identical to
// iverilog (cross-checked). $urandom is a per-process xoshiro256++ whose stream
// is intentionally NOT iverilog-identical (LRM leaves it unspecified); it is pinned to its own deterministic output and its
// LRM-required properties (range bounds, per-process independence, reseed).

TEST_CASE("$random(seed) threads the seed and matches the reference stream") {
    const std::string out = run_capture(prog(
      "integer seed; integer r; int i;",
      "seed = 1;"
      "for (i=0;i<3;i=i+1) begin r=$random(seed); $display(\"%0d %0d\", r, seed); end"));
    CHECK(out == "-2147414528 69070\n"
                 "-1671855048 475628535\n"
                 "1129920902 -1017563188\n");
}

TEST_CASE("$dist_* match the IEEE reference stream (iverilog oracle)") {
    const std::string out = run_capture(prog(
      "integer s; int i;",
      "s=7; for(i=0;i<3;i=i+1) $display(\"%0d\", $dist_uniform(s,10,20));"
      "s=7; for(i=0;i<3;i=i+1) $display(\"%0d\", $dist_normal(s,100,15));"
      "s=7; for(i=0;i<3;i=i+1) $display(\"%0d\", $dist_exponential(s,5));"
      "s=7; for(i=0;i<3;i=i+1) $display(\"%0d\", $dist_poisson(s,4));"
      "s=7; for(i=0;i<3;i=i+1) $display(\"%0d\", $dist_chi_square(s,3));"
      "s=7; for(i=0;i<3;i=i+1) $display(\"%0d\", $dist_t(s,3));"
      "s=7; for(i=0;i<3;i=i+1) $display(\"%0d\", $dist_erlang(s,2,5));"));
    CHECK(out == "10\n18\n17\n"      // uniform
                 "117\n91\n98\n"     // normal
                 "45\n1\n2\n"        // exponential
                 "0\n6\n1\n"         // poisson
                 "4\n11\n4\n"        // chi_square
                 "1\n-1\n0\n"        // t
                 "23\n2\n4\n");      // erlang
}

TEST_CASE("$urandom_range stays within [min,max] and swaps min>max") {
    const std::string out = run_capture(prog(
      "int a,i,ok; ",
      "ok=1; for(i=0;i<20000;i=i+1) begin a=$urandom_range(50,10);"
      "  if (a<10||a>50) ok=0; end $display(\"lo %0d\", ok);"
      "ok=1; for(i=0;i<20000;i=i+1) begin a=$urandom_range(10,50);"  // swapped args
      "  if (a<10||a>50) ok=0; end $display(\"sw %0d\", ok);"
      "$display(\"one %0d\", $urandom_range(7,7));"));
    CHECK(out == "lo 1\nsw 1\none 7\n");
}

TEST_CASE("$urandom is a deterministic per-process stream (pinned)") {
    // A single process's first draws — deterministic across runs (seeded from
    // the process index). Pins the xoshiro256++ stream so a lowering change that
    // perturbs it is caught.
    const std::string out = run_capture(prog(
      "int i;",
      "for(i=0;i<4;i=i+1) $display(\"%0d\", $urandom);"));
    CHECK(out == "1394040161\n1641705277\n1544544145\n49201036\n");
}

TEST_CASE("$urandom(seed) reseeds reproducibly") {
    const std::string out = run_capture(prog(
      "int a,b;",
      "a=$urandom(42); b=$urandom(42); $display(\"eq %0d\", a==b);"));
    CHECK(out == "eq 1\n");
}

TEST_CASE("two processes each get an independent $urandom stream") {
    // Distinct per-process frame RNGs: the two processes' first draws differ.
    // Written to module vars at t0, compared by a later process (robust to the
    // t0 process order and to the exact draw values).
    const std::string out = run_capture(
      "module m;\n"
      " int va, vb;\n"
      " initial va = $urandom;\n"
      " initial vb = $urandom;\n"
      " initial #1 begin $display(\"indep %0d\", va != vb); $finish; end\n"
      "endmodule\n");
    CHECK(out == "indep 1\n");
}

TEST_CASE("$urandom in a function fails loudly (no per-process RNG in frame)") {
    CHECK(lower_error("module m; function int f(); return $urandom; endfunction\n"
                      " initial $display(\"%0d\", f()); endmodule")
            .find("$urandom") != std::string::npos);
}
