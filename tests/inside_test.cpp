#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run an already lowered design and return everything written to stdout.
std::string run_lowered(const LowerResult& r) {
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

// JIT-run a module's initial blocks and return everything written to stdout.
std::string run_capture(std::string_view source) {
    return run_lowered(lower_text(source));
}

// The same for a design compiled at the IEEE 1800-2023 language level, which the
// tolerance ranges of LRM 11.4.13 need: lower_text parses at the default, 1800-2017.
std::string run_capture_2023(std::string_view source) {
    const std::filesystem::path path =
      std::filesystem::temp_directory_path() / (clotho::test::detail::unique_stem() + ".sv");
    {
        std::ofstream out(path, std::ios::binary);
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
    }
    const std::string                   file = path.string();
    const std::vector<std::string_view> args = { "--std", "1800-2023", file };
    const std::string                   out  = run_lowered(lower_sources(args));
    std::filesystem::remove(path);
    return out;
}

void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

} // namespace

// The `inside` set-membership operator (LRM 11.4.13). Singular members compare
// with ==? (only the member's x/z are wildcards); a [lo:hi] range is
// sel>=lo && sel<=hi; the whole result is the OR-reduction of the members.
// Definite (0/1) results cross-checked against Verilator 5.048; iverilog 13 does
// not implement the inside operator ("sorry: inside expressions not supported").
TEST_CASE("inside: singular membership and ranges (LRM 11.4.13)") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] a;
          initial begin
            a = 4'd5;  $display("%b", a inside {4'd1, 4'd5, 4'd9});          // 1
            a = 4'd5;  $display("%b", a inside {4'd1, 4'd2});                // 0
            a = 4'd7;  $display("%b", a inside {[4'd5:4'd10]});             // 1
            a = 4'd3;  $display("%b", a inside {[4'd5:4'd10]});             // 0
            a = 4'd5;  $display("%b", a inside {[4'd5:4'd10]});             // 1 (lo boundary)
            a = 4'd10; $display("%b", a inside {[4'd5:4'd10]});            // 1 (hi boundary)
            a = 4'd11; $display("%b", a inside {[4'd5:4'd10]});            // 0
            a = 4'd8;  $display("%b", a inside {4'd1, [4'd5:4'd10], 4'd15}); // 1 (mixed)
          end
        endmodule
    )");
    CHECK(out == "1\n0\n1\n0\n1\n1\n0\n1\n");
}

// A member's x/z bits are wildcards (==? semantics); the left operand's are not.
TEST_CASE("inside: member x/z bits are wildcards") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] a;
          initial begin
            a = 4'b1011; $display("%b", a inside {4'b10x1}); // 1 (bit1 wildcard)
            a = 4'b0011; $display("%b", a inside {4'b10x1}); // 0 (bit3 mismatch)
          end
        endmodule
    )");
    CHECK(out == "1\n0\n");
}

// Left-operand x/z is NOT a wildcard: an uncertain compare with no definite match
// yields x (LRM 11.4.13, ==? propagates from the left operand's unknowns).
TEST_CASE("inside: unknown left operand yields x when no definite match") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] a;
          initial begin
            a = 4'b00xx; $display("%b", a inside {4'b0000}); // x (low bits x vs 0)
            a = 4'bxxxx; $display("%b", a inside {4'b10x1}); // x (no definite mismatch)
          end
        endmodule
    )");
    CHECK(out == "x\nx\n");
}

TEST_CASE("inside: usable as an if / while condition") {
    const std::string out = run_capture(R"(
        module m;
          logic [7:0] a;
          initial begin
            a = 8'd42;
            if (a inside {[8'd40:8'd50]}) $display("in"); else $display("out");
            a = 8'd7;
            if (a inside {8'd1, 8'd2, 8'd3}) $display("in"); else $display("out");
          end
        endmodule
    )");
    CHECK(out == "in\nout\n");
}

TEST_CASE("inside: signed range membership") {
    // Common comparison type is signed (both operands signed), so the range
    // compare is signed: -3 lies inside [-5:5].
    const std::string out = run_capture(R"(
        module m;
          logic signed [7:0] a;
          initial begin
            a = -3;  $display("%b", a inside {[-5:5]});  // 1
            a = -9;  $display("%b", a inside {[-5:5]});  // 0
          end
        endmodule
    )");
    CHECK(out == "1\n0\n");
}

// -- real operands (LRM 11.4.13) --
// 11.4.13 uses `==` (not `==?`) on a nonintegral expression, and Table 11-1
// defines `>=` / `<=` for reals, so a real set is IEEE-754 equality plus the
// inclusive relational range. iverilog 13 cannot compile `inside` at all
// ("sorry: inside expressions not supported yet").
TEST_CASE("inside: real values and ranges") {
    const std::string out = run_capture(R"(
        module m;
          real      r;
          shortreal sr;
          initial begin
            r = 1.5;
            $display("%b %b %b", r inside {1.5, 2.5}, r inside {0.5, 2.5}, r inside {[1.0:2.0]});
            $display("%b %b", r inside {[2.0:3.0]}, r inside {[1.5:1.5]});
            r = 2.0;
            $display("%b", r inside {2, 7});
            sr = 2.5;
            $display("%b %b", sr inside {2.5}, sr inside {[2.0:2.4]});
          end
        endmodule
    )");
    // An integral member converts to real (11.8.1), so `2` matches 2.0; a range
    // is closed at both ends.
    CHECK(out == "1 0 1\n0 1\n1\n1 0\n");
}

// -- unpacked-array set members (LRM 11.4.13) --
// "If an expression in the list is an unpacked array, its elements are traversed
// by descending into the array until reaching a singular value." A fixed-size
// member unrolls, a queue / dynamic array walks at runtime; 11.4.13's own
// example is the queue one.
TEST_CASE("inside: an array member is traversed elementwise") {
    const std::string out = run_capture(R"(
        module m;
          int q[$];
          int d[];
          int fa [0:2];
          int n  [0:1][0:1];
          int eq[$];
          initial begin
            q = '{3, 4, 5};
            d = new[2]; d[0] = 8; d[1] = 9;
            fa = '{11, 12, 13};
            n[0][0] = 1; n[0][1] = 2; n[1][0] = 3; n[1][1] = 4;
            $display("%b %b", 4 inside {1, 2, q}, 7 inside {1, 2, q});
            $display("%b %b", 9 inside {d}, 1 inside {d});
            $display("%b %b", 12 inside {fa}, 14 inside {fa});
            $display("%b %b", 3 inside {n}, 9 inside {n});
            $display("%b", 5 inside {eq});
            $display("%b", 13 inside {[100:200], fa, q});
          end
        endmodule
    )");
    // A nested dimension descends; an empty queue contributes nothing; an array
    // member mixes with ordinary values and ranges in one set.
    CHECK(out == "1 0\n1 0\n1 0\n1 0\n0\n1\n");
}

// An element of a 4-state array member keeps the ==? asymmetry: the MEMBER's x/z
// bits are do-not-cares, the left operand's are not (11.4.13).
TEST_CASE("inside: x/z in an array member element is a wildcard") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] warr [0:1];
          initial begin
            warr[0] = 4'b1x01; warr[1] = 4'b0000;
            $display("%b %b", 4'b1101 inside {warr}, 4'b1110 inside {warr});
          end
        endmodule
    )");
    CHECK(out == "1 0\n");
}

// A real set member inside an array works through the same leaf comparison.
TEST_CASE("inside: a real left operand against a real array member") {
    const std::string out = run_capture(R"(
        module m;
          real rq[$];
          real r;
          initial begin
            rq = '{1.25, 2.5};
            r = 2.5;
            $display("%b %b", r inside {rq}, 3.5 inside {rq});
          end
        endmodule
    )");
    CHECK(out == "1 0\n");
}

// A string left operand: per-member equality, and lexicographic relationals for
// a range (LRM 11.4.13 / 6.16.9). The UVM shape is uvm_reg_field::needs_update,
// `get_access() inside {"RO","RC","RS"}`, reached by every factory-registered
// register field once static initialization is eager. Verified vs Verilator 5.050.
TEST_CASE("inside: a string left operand against string members and a range") {
    const std::string out = run_capture(R"(
        module m;
          string s;
          initial begin
            s = "RC";
            $display("%0d %0d %0d", s inside {"RO","RC","RS"}, "zz" inside {"RO","RC"}, s inside {["RA":"RD"]});
          end
        endmodule
    )");
    CHECK(out == "1 0 1\n");
}
// A range of the tolerance forms `[a +/- b]` and `[a +%- b]` (LRM 11.4.13) is
// [a - type(a)'(b) : a + type(a)'(b)] and [a - type(a)'(a*b/100.0) : a +
// type(a)'(a*b/100.0)], the two bounds swapped when need be; a real tolerance
// converts to an integral type by truncation, not rounding. The worked values are
// the LRM's own (a = 7 at 25% is [6:8], a = -7 is [-8:-6]), and each line prints
// the answer for a run of selector values. iverilog 13 cannot parse the forms,
// so the expectations come from the clause.
TEST_CASE("inside: tolerance ranges (LRM 11.4.13)") {
    const std::string out = run_capture_2023(R"(
        module m;
          int x;
          real r;
          initial begin
            for (x = 5; x <= 15; x++) $write("%b", x inside {[10 +/- 3]});   $display("");
            for (x = 5; x <= 15; x++) $write("%b", x inside {[10 +%- 25]});  $display("");
            for (x = 5; x <= 15; x++) $write("%b", x inside {[10 +/- -3]});  $display("");
            for (x = 5; x <= 9; x++) $write("%b", x inside {[7 +%- 25]});    $display("");
            for (x = -9; x <= -5; x++) $write("%b", x inside {[-7 +%- 25]}); $display("");
            for (x = -13; x <= -7; x++) $write("%b", x inside {[-10 +%- 25]}); $display("");
            r = 5.25; $write("%b", r inside {[7.0 +%- 25]});
            r = 8.75; $write("%b", r inside {[7.0 +%- 25]});
            r = 5.0;  $write("%b", r inside {[7.0 +%- 25]});
            r = 9.0;  $write("%b", r inside {[7.0 +%- 25]});                 $display("");
            for (x = 5; x <= 9; x++) $write("%b", x inside {[7.0 +/- 1.5]}); $display("");
            for (x = 6; x <= 14; x++) $write("%b", x inside {[10 +/- 2.9]}); $display("");
            for (x = 8; x <= 12; x++) $write("%b", x inside {[10 +%- 12.5]}); $display("");
            for (x = 5; x <= 9; x++) $write("%b", x inside {1, [7 +/- 1], 20}); $display("");
          end
        endmodule
    )");
    CHECK(out ==
          "00111111100\n00011111000\n00111111100\n01110\n01110\n0111110\n1100\n01110\n001111100\n01110\n01110\n");
}

// A tolerance range runs the same arithmetic in a constant expression, which slang
// evaluates on its own: one function, called from a localparam (slang's evaluation)
// and at run time (Clotho's lowering), must give every selector the same answer.
// The operand types are mixed on purpose -- an unsigned selector against signed
// literals, a narrow signed one, (slang's own evaluator rounds a real absolute tolerance where 11.4.13 truncates, so none is swept).
TEST_CASE("inside: a tolerance range agrees with slang's constant evaluation") {
    const std::string out = run_capture_2023(R"(
        module m;
          function automatic bit [39:0] sweep(int a, int b);
            bit [39:0] bits;
            logic [7:0] u;
            logic signed [7:0] s;
            for (int i = 0; i < 10; i++) begin
              u = i * 3; s = i * 3 - 12;
              bits[i]      = int'(u) inside {[a +/- b]};
              bits[10 + i] = int'(s) inside {[a +%- b]};
              bits[20 + i] = u inside {[a +/- b]};
              bits[30 + i] = s inside {[a +/- 2]};
            end
            return bits;
          endfunction
          localparam bit [39:0] FOLDED = sweep(9, 4);
          bit [39:0] ran;
          int a = 9, b = 4;
          initial begin
            ran = sweep(a, b);
            $display("%0d %b", ran == FOLDED, ran == 40'd0);
          end
        endmodule
    )");
    CHECK(out == "1 0\n");
}

// A `$` range bound (LRM 11.4.13) stands for the lowest or highest value of the
// type of the left operand, so `[$:hi]` is "at most hi" and `[lo:$]` "at least lo",
// whatever the width and signedness of the operand, and for a real. iverilog 13
// cannot parse the form: the expectations come from the clause.
TEST_CASE("inside: unbounded range bounds (LRM 11.4.13)") {
    const std::string out = run_capture(R"(
        module m;
          int x;
          logic [7:0] u;
          byte sb;
          real r;
          initial begin
            for (x = 6; x <= 9; x++) $write("%b", x inside {[$:7]}); $display("");
            for (x = 6; x <= 9; x++) $write("%b", x inside {[8:$]}); $display("");
            x = 2147483647;       $write("%b", x inside {[5:$]});
            x = -2147483647 - 1;  $write("%b", x inside {[$:5]}); $write("%b", x inside {[5:$]}); $display("");
            u = 200; $write("%b", u inside {[100:$]}); $write("%b", u inside {[$:100]});
            u = 0;   $write("%b", u inside {[$:0]});                                    $display("");
            sb = -128; $write("%b", sb inside {[$:-100]});
            sb = 127;  $write("%b", sb inside {[100:$]});
            sb = -1;   $write("%b", sb inside {[0:$]});                                 $display("");
            r = 1.0e30;  $write("%b", r inside {[5.0:$]});
            r = -1.0e30; $write("%b", r inside {[$:5.0]});
            r = 4.0;     $write("%b", r inside {[5.0:$]});                              $display("");
            x = 3;  $write("%b", x inside {1, [10:$]});
            x = 11; $write("%b", x inside {1, [10:$]});
            x = 1;  $write("%b", x inside {1, [10:$]});                                 $display("");
          end
        endmodule
    )");
    CHECK(out == "1100\n0011\n110\n101\n110\n110\n011\n");
}

// A set member that is a container of strings (LRM 11.4.13: "descending into the
// array until reaching a singular value"): a queue, a dynamic array, a fixed-size
// array (nested too) and an associative array, alone and beside other members, plus
// the associative array of ints that had no key walk either. The comparison is the
// content equality of 6.16.9. iverilog 13 cannot compile `inside` at all, so the
// expectations come from the clause.
TEST_CASE("inside: a container of strings as a set member") {
    const std::string out = run_capture(R"(
        module m;
          string q[$]; string d[]; string fa [0:2]; string fn [0:1][0:1]; string aa[int]; string ee[$];
          string s;
          int amap [string];
          int a;
          initial begin
            q = '{"ab", "cd"};
            d = new[2]; d[0] = "x"; d[1] = "y";
            fa = '{"p", "q", "r"};
            fn[0][0] = "a"; fn[0][1] = "b"; fn[1][0] = "c"; fn[1][1] = "d";
            aa[5] = "m"; aa[9] = "n";
            s = "cd"; $display("%b %b", s inside {q}, s inside {"zz", q});
            s = "ef"; $display("%b", s inside {q});
            s = "y";   $display("%b %b", s inside {d}, s inside {fa});
            s = "q";   $display("%b %b", s inside {d}, s inside {fa});
            s = "c";   $display("%b", s inside {fn});
            s = "e";   $display("%b", s inside {fn});
            s = "n";   $display("%b %b", s inside {aa}, s inside {ee});
            s = "o";   $display("%b", s inside {aa});
            s = "pq";  $display("%b", s inside {["a":"z"], q});
            s = "m";   $display("%b", s inside {aa, "zz"});
            amap["k"] = 2; a = 2;
            $display("%b %b", a inside {amap}, 3 inside {amap});
          end
        endmodule
    )");
    CHECK(out == "1 1\n0\n1 0\n0 1\n1\n0\n1 0\n0\n1\n1\n1 0\n");
}

// Unsupported forms fail loudly (PRINCIPLES.md: never silently accept).
TEST_CASE("inside: unsupported forms are rejected") {


    // Descending into a non-singular element of a VARIABLE-size member needs the
    // walk nested inside itself; the fixed-size member recurses instead.
    check_rejected(R"(
        module m;
          int a; int qq [$][0:1];
          initial begin a = 2; $display("%b", a inside {qq}); end
        endmodule
    )",
                   "not singular");
}
