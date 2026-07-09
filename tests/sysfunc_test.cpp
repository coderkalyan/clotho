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

// -- $typename (guarded) ----------------------------------------------------

// slang's type-name printer omits the LRM 20.6.1 whitespace, so $typename is
// rejected loudly rather than emitting a non-compliant string.
TEST_CASE("$typename is rejected loudly") {
    const std::string err = lower_error("module m; logic [7:0] v; initial $display(\"%s\", $typename(v)); endmodule");
    CHECK(err.find("$typename") != std::string::npos);
}
