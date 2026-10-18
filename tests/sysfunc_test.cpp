#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

#include <cstdio>

using namespace clotho;

namespace {

// JIT the design and capture everything it writes to stdout. Mirrors the helper
// in the other backend tests.
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

std::string prog(std::string_view decls, std::string_view body) {
    return "module m;\n" + std::string(decls) + "\n initial begin\n" + std::string(body) +
           "\n $finish;\n end\nendmodule\n";
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
// iverilog limitation; the LRM (and the byte count) makes 8*len correct
// (docs/LRM_NOTES.md, note 27).
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

// -- $isunbounded -----------------------------------------------------------

// $isunbounded (LRM 20.6.1): 1 when the operand is a parameter whose value is
// the unbounded literal `$`, 0 otherwise. The argument is never evaluated, so
// the call is always an elaboration constant.
TEST_CASE("$isunbounded distinguishes an unbounded parameter") {
    const std::string out =
      run_capture(prog("parameter int UNB = $; parameter int LIM = 16; parameter real R = 3.5;",
                       "$display(\"%0b %0b %0b\", $isunbounded(UNB), $isunbounded(LIM),"
                       " $isunbounded(R));"));
    CHECK(out == "1 0 0\n");
}

// The idiomatic use: the unbounded bound picks a branch during elaboration, so
// the result reaches lowering already folded into the dependent parameters.
TEST_CASE("$isunbounded folds in a constant context") {
    const std::string out = run_capture(
      prog("parameter int UNB = $; localparam bit U = $isunbounded(UNB);"
           " localparam int LIMIT = U ? 0 : 8;",
           "$display(\"%0b %0d\", U, LIMIT);"));
    CHECK(out == "1 0\n");
}

// -- $typename --------------------------------------------------------------

// Clotho's own LRM 20.6.1 formatter (format_typename) over the supported type
// kinds. The declared `logic` keyword is kept rather than canonicalized to the
// legacy `reg`, matching slang and the LRM's modern keyword, and the
// non-default `int unsigned` signing is preserved, as LRM 20.6.1(b) keeps it.
// Both an expression and a data-type argument name the same type.
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

// The aggregate and container kinds, which Clotho used to refuse rather than name.
// A named type carries its declaring scope, exactly as LRM 20.6.1's own `A::bit[9:1]`
// example does; an enum spells its members with their sized values; a specialization
// carries its arguments (`P#(byte)`), which is the form UVM's `uvm_typename(T)` needs.
TEST_CASE("$typename names the aggregate and container kinds") {
    const std::string out = run_capture(prog(
      "typedef enum {A,B} e; e x;"
      " typedef struct packed {logic a;} sp; sp y;"
      " typedef union {int i; byte b;} un; un u;"
      " class C; int n; endclass class P #(type T = int); T v; endclass"
      " typedef int qt[$]; qt q; typedef int dt[]; dt d; typedef int at[string]; at aa;"
      " C k; P#(byte) pk; chandle ch; event ev;",
      "$write(\"%s|%s|%s|\", $typename(x), $typename(y), $typename(u));"
      "$write(\"%s|%s|\", $typename(k), $typename(pk));"
      "$display(\"%s|%s|%s|%s|%s\", $typename(q), $typename(d), $typename(aa),"
      " $typename(ch), $typename(ev));"));
    CHECK(out ==
          "enum{A=32'sd0,B=32'sd1}m.e|struct packed{logic a;}m.sp|union{int i;byte b;}m.un|"
          "C|P#(byte)|"
          "int$[$]|int$[]|int$[string]|chandle|event\n");
}

// 20.6.1 gives an anonymous type an implementation-generated name, and the clause is
// only usable because those names are UNIQUE: $typename strings are specified to match
// for equivalent types, so two identically-shaped anonymous structs -- distinct types --
// must not collide. That uniqueness is the reason for pinning the printer to
// `SystemName` over the friendlier structural spelling.
TEST_CASE("$typename gives distinct anonymous types distinct names") {
    const std::string out =
      run_capture(prog("struct {int a;} p; struct {int a;} q;",
                       "$display(\"%s|%s\", $typename(p), $typename(q));"));
    CHECK(out == "struct{int a;}m.s$1|struct{int a;}m.s$2\n");
}

// -- $random(seed) / $dist_* / $urandom (LRM 20.15, 18.13) ------------------
//
// The $dist_* routines and seeded $random are the IEEE reference algorithm
// (mandated verbatim), so the expected values below are byte-identical to
// iverilog (cross-checked). $urandom is a per-process xoshiro256++ whose stream
// is intentionally NOT iverilog-identical, since the LRM leaves it unspecified
// (docs/LRM_NOTES.md, note 25); it is pinned to its own deterministic output and its
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

// The RNG is per THREAD, not per body (LRM 18.14.1): a function or task runs in
// its caller's process, so the backend threads the caller's state through the
// call ABI and a draw inside one continues the calling process's stream. Pinned
// against a reseed rather than against literal draws, so these test the sharing
// and not the (deliberately unspecified) stream.
TEST_CASE("$urandom inside a function draws and advances the caller's stream") {
    const std::string out = run_capture(
      "module m;\n"
      " function automatic int r(); return $urandom; endfunction\n"
      " int x, y, a, b;\n"
      " initial begin\n"
      "  x = $urandom(7); y = $urandom;\n"   // the pair the stream gives after seed 7
      "  a = $urandom(7); b = r();\n"        // the same pair, second draw via a call
      "  $display(\"share %0d\", x == a && y == b);\n"
      "  $finish;\n"
      " end\n"
      "endmodule\n");
    CHECK(out == "share 1\n");
}

TEST_CASE("$urandom inside a task draws and advances the caller's stream") {
    const std::string out = run_capture(
      "module m;\n"
      " task automatic t(output int o); o = $urandom; endtask\n"
      " int x, y, a, b;\n"
      " initial begin\n"
      "  x = $urandom(7); y = $urandom;\n"
      "  a = $urandom(7); t(b);\n"
      "  $display(\"share %0d\", x == a && y == b);\n"
      "  $finish;\n"
      " end\n"
      "endmodule\n");
    CHECK(out == "share 1\n");
}

TEST_CASE("two processes calling one $urandom function keep independent streams") {
    // The state is the caller's, so one shared function body still yields two
    // streams -- the property 18.14.1 requires and a body-local state would break.
    const std::string out = run_capture(
      "module m;\n"
      " function automatic int r(); return $urandom; endfunction\n"
      " int va, vb;\n"
      " initial va = r();\n"
      " initial vb = r();\n"
      " initial #1 begin $display(\"indep %0d\", va != vb); $finish; end\n"
      "endmodule\n");
    CHECK(out == "indep 1\n");
}

TEST_CASE("a process reaching $urandom only through a call keeps its state across activations") {
    // Such a process cannot be coroutine-elided: a thunk has no frame to hold the
    // xoshiro state in, so its stream would restart on every activation and both
    // draws below would be equal (compute_fusion_groups excludes it for that).
    const std::string out = run_capture(
      "module m;\n"
      " function automatic int r(); return $urandom; endfunction\n"
      " reg clk; int a, b, n;\n"
      " always @(posedge clk) begin n = n + 1; if (n == 1) a = r(); else b = r(); end\n"
      " initial begin\n"
      "  clk = 0; n = 0;\n"
      "  #1 clk = 1; #1 clk = 0; #1 clk = 1;\n"
      "  #1 $display(\"adv %0d\", a != b); $finish;\n"
      " end\n"
      "endmodule\n");
    CHECK(out == "adv 1\n");
}

TEST_CASE("each fork child gets its own stream, seeded from the parent's") {
    // LRM 18.14.1 gives a spawned process its own generator, seeded from its
    // parent's -- so the two children differ from each other and from the parent's
    // next draw. A child sharing the parent's state pointer would also be a
    // dangling one under join_none, which is why it owns a state at all.
    const std::string out = run_capture(
      "module m;\n"
      " int a, b, c;\n"
      " initial begin\n"
      "  fork\n"
      "   a = $urandom;\n"
      "   b = $urandom;\n"
      "  join\n"
      "  c = $urandom;\n"
      "  $display(\"kids %0d\", a != b && a != c && b != c);\n"
      "  $finish;\n"
      " end\n"
      "endmodule\n");
    CHECK(out == "kids 1\n");
}
