#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// $display formatting per LRM §21.2.1: automatic sizing, field widths, x/z
// casing, %m, empty arguments, radix task variants. Expectations are
// cross-checked against Icarus Verilog except where noted (iverilog deviates
// from the LRM's own examples on explicit widths for h/o/b).

namespace {

// JIT-run a module and capture stdout (mirrors string_test.cpp).
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

// Assert that `source` is rejected with a diagnostic containing `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

} // namespace

// The printval example of LRM §21.2.1.2, verbatim.
TEST_CASE("automatic sizing and the %0 minimal form (LRM printval example)") {
    CHECK(run_capture(R"(
        module printval;
            logic [11:0] r1;
            initial begin
                r1 = 10;
                $display( "Printing with maximum size - :%d: :%h:", r1,r1 );
                $display( "Printing with minimum size - :%0d: :%0h:", r1,r1 );
            end
        endmodule
    )") == "Printing with maximum size - :  10: :00a:\n"
           "Printing with minimum size - :10: :a:\n");
}

// The field-width example table of LRM §21.2.1.2. NOTE: iverilog prints %3h of
// 32'h5 as 00000005 (natural width); the LRM table says 005 — LRM wins.
TEST_CASE("explicit field widths pad and expand per LRM 21.2.1.2") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display(":%3d:", 32'd5);
                $display(":%3d:", 32'd100);
                $display(":%3d:", 32'd1234);
                $display(":%3h:", 32'h5);
                $display(":%3h:", 32'h100);
                $display(":%3h:", 32'h1234);
                $display(":%3s:", "a");
                $display(":%3s:", "abc");
                $display(":%3s:", "abcdef");
            end
        endmodule
    )") == ":  5:\n:100:\n:1234:\n:005:\n:100:\n:1234:\n:  a:\n:abc:\n:abcdef:\n");
}

// LRM §21.2.1.3 examples, verbatim: whole-field decimal collapse and per-digit
// hex/octal casing (lowercase when every bit of the group is x/z, uppercase
// when only some are).
TEST_CASE("x/z display follows LRM 21.2.1.3") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display("%d", 1'bx);
                $display("%h", 14'bx01010);
                $display("%h %o", 12'b001xxx101x01, 12'b001xxx101x01);
            end
        endmodule
    )") == "x\nxxXa\nXXX 1x5X\n");
}

TEST_CASE("decimal x/z casing and justification") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display(":%d:", 8'bxxxxxxxx);
                $display(":%d:", 8'bzzzzzzzz);
                $display(":%d:", 8'b1xxxxxxx);
                $display(":%d:", 8'b1zzzzzzz);
                $display(":%d:", 8'bxzzzzzzz);
                $display(":%0d:", 8'hxx);
            end
        endmodule
    )") == ":  x:\n:  z:\n:  X:\n:  Z:\n:  X:\n:x:\n");
}

// Signed decimal auto-sizes to the most negative value's text (sign included).
TEST_CASE("signed decimal automatic sizing") {
    CHECK(run_capture(R"(
        module m;
            logic signed [7:0] s;
            initial begin
                s = -1;
                $display(":%d:", s);
                s = 100;
                $display(":%d:", s);
                $display(":%d:", 8'd7);
            end
        endmodule
    )") == ":  -1:\n: 100:\n:  7:\n");
}

// `-` left-justifies with trailing spaces (an extension beyond the LRM's
// format list).
TEST_CASE("left justification") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display(":%-5d:", 8'd7);
                $display(":%-5s:", "ab");
                $display(":%5s:", "ab");
            end
        endmodule
    )") == ":7    :\n:ab   :\n:   ab:\n");
}

// A bare argument uses the task's auto-sized default radix (LRM §21.2.1.1);
// an empty argument (adjacent commas) prints one space (LRM §21.2.1).
TEST_CASE("bare-argument defaults, radix variants, and empty arguments") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display(8'd7);
                $displayb(5'b101x0);
                $displayo(9'o567);
                $displayh(12'hEF);
                $writeh(12'hEF); $write("\n");
                $display("e1:", , "e2");
            end
        endmodule
    )") == "  7\n101x0\n567\n0ef\n0ef\ne1: e2\n");
}

// Leading NUL bytes of a packed %s operand are never printed (LRM §21.2.1.7
// and the §21.2.1.1 example; iverilog deviates).
TEST_CASE("%s drops leading NUL bytes of a packed operand") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display("%s is ascii value for 101", 101);
                $display(":%s:", 32'h616263);
                $display(":%5s:", 32'h61);
            end
        endmodule
    )") == "e is ascii value for 101\n:abc:\n:    a:\n");
}

// %m splices the static hierarchical name of the invoking scope: named block,
// task, and instance path under a multi-top design (LRM §21.2.1.5).
TEST_CASE("%m reports the invoking scope") {
    CHECK(run_capture(R"(
        module m;
            task t; $display("task %m"); endtask
            initial begin : blk
                $display("blk %m");
                t;
            end
        endmodule
        module m2;
            sub u1();
        endmodule
        module sub;
            initial #1 $display("sub %m");
        endmodule
    )") == "blk m.blk\ntask m.t\nsub m2.u1\n");
}

// %l splices "library.cell" (LRM §21.2.1.3): the current module instance's
// library binding and cell (module definition) name. Clotho has no
// library/config system (Clause 33), so there is no library map to consult and
// every instance is unconditionally in "work". Static per lowering site, same
// as %m: the cell is the module's DEFINITION name, not the instance name
// (`s1`'s cell is still "sub").
TEST_CASE("%l reports the invoking instance's library.cell") {
    CHECK(run_capture(R"(
        module sub;
            initial $display("sub scope: %l");
        endmodule
        module top;
            sub s1();
            initial $display("top scope: %l");
        endmodule
    )") == "sub scope: work.sub\ntop scope: work.top\n");
}

// An explicit %t width overrides $timeformat's minimum width; %0t is minimal.
TEST_CASE("%t honors explicit widths over $timeformat") {
    CHECK(run_capture(R"(
        `timescale 1ns/1ps
        module m;
            initial begin
                $display(":%0t:", $time);
                #5 $display(":%t:", $time);
                $display(":%15t:", $time);
            end
        endmodule
    )") == ":0:\n:                5000:\n:           5000:\n");
}

// A string literal that an earlier specifier is waiting on is its value
// argument; otherwise a literal is format text.
TEST_CASE("string literals fill pending specifiers") {
    CHECK(run_capture(R"(
        module m;
            initial begin
                $display("%s", "hi");
                $display("a", "b");
                $display("%d then %s more", 5, "txt", " end");
            end
        endmodule
    )") == "hi\nab\n          5 then txt more end\n");
}

// -- Rejections: everything unimplemented fails loudly at lower time --

TEST_CASE("unsupported or invalid format specifiers are rejected") {
    SUBCASE("undefined letter") {
        // slang diagnoses this at elaboration, before Clotho's own guard.
        check_rejected(R"(
            module m;
                initial $display("%q", 1);
            endmodule
        )", "unknown format specifier '%q'");
    }
    SUBCASE("%v strength") {
        check_rejected(R"(
            module m;
                wire w;
                initial $display("%v", w);
            endmodule
        )", "%v (net signal strength) is not supported");
    }
    SUBCASE("%p of a scalar renders its decimal value") {
        CHECK(run_capture(R"(
            module m;
                initial $display("%p", 1);
            endmodule
        )") == "1\n");
    }
    SUBCASE("integral under a real specifier converts (LRM 6.12.1)") {
        CHECK(run_capture(R"(
            module m;
                initial $display("%f", 1);
            endmodule
        )") == "1.000000\n");
    }
    SUBCASE("precision on an integer conversion") {
        // slang diagnoses this at elaboration, before Clotho's own guard.
        check_rejected(R"(
            module m;
                initial $display("%5.2d", 1);
            endmodule
        )", "floating point format specifier options used on non-floating point field");
    }
    SUBCASE("specifier without an argument") {
        // slang diagnoses this at elaboration; Clotho's own count check backs
        // it up for the $swrite-style interleaved forms slang cannot see.
        check_rejected(R"(
            module m;
                initial $display("%d %d", 1);
            endmodule
        )", "no argument provided for '%d'");
    }
    SUBCASE("width on %u") {
        // slang diagnoses this at elaboration, before Clotho's own guard.
        check_rejected(R"(
            module m;
                initial $display("%8u", 32'h1);
            endmodule
        )", "field width not allowed on '%u'");
    }
}

// Operands wider than 64 bits: the rt_arg planes are multi-word, so every radix
// renders in full (%d via a bignum divmod). Expectations cross-checked against
// Icarus Verilog, except the explicit-width h/o/b zero-pad
// (iverilog space-pads; the LRM says leading zeros, so Clotho zero-pads).
TEST_CASE("display operands wider than 64 bits (LRM 21.2)") {
    SUBCASE("128-bit unsigned across every radix") {
        CHECK(run_capture(R"(
            module m;
                logic [127:0] a;
                initial begin
                    a = 128'hDEADBEEFCAFEBABE0123456789ABCDEF;
                    $display(":%0d: :%h: :%o:", a, a, a);
                    $display(":%b:", a);
                end
            endmodule
        )") == ":295990755076957304698161171062762229231: "
               ":deadbeefcafebabe0123456789abcdef: "
               ":3365333735762577272574004432126361152746757:\n"
               ":11011110101011011011111011101111110010101111111010111010"
               "101111100000000100100011010001010110011110001001101010111100110111101111:\n");
    }

    SUBCASE("auto-width %d pads to the wide operand's maximum") {
        // A 128-bit unsigned's widest value is 39 digits, so a small magnitude
        // pads left with 37 spaces (LRM 21.2.1.2).
        CHECK(run_capture(R"(
            module m;
                logic [127:0] a;
                initial begin a = 128'd42; $display(":%d:", a); end
            endmodule
        )") == ":                                     42:\n");
    }

    SUBCASE("signed negative wide %d prints sign and magnitude") {
        CHECK(run_capture(R"(
            module m;
                logic signed [95:0] s;
                initial begin
                    s = -96'sd170141183460469231731;
                    $display(":%0d: :%d:", s, s);
                end
            endmodule
        )") == ":-170141183460469231731: :        -170141183460469231731:\n");
    }

    SUBCASE("packed ASCII wider than 64 bits (%s)") {
        CHECK(run_capture(R"(
            module m;
                logic [95:0] str;
                initial begin str = "Hello, world"; $display(":%s:", str); end
            endmodule
        )") == ":Hello, world:\n");
    }

    SUBCASE("x/z bits in a wide operand: per-digit and whole-field collapse") {
        // A 100-bit value with an x nibble and a z nibble: %h shows them per
        // digit; %d collapses the whole field to uppercase X (x mixed with
        // known bits) per LRM 21.2.1.3.
        CHECK(run_capture(R"(
            module m;
                logic [99:0] xv;
                initial begin
                    xv = {32'hDEADBEEF, 4'bxxxx, 32'hCAFEBABE, 4'bzzzz, 28'h1234567};
                    $display(":%h:", xv);
                    $display(":%0d:", xv);
                end
            endmodule
        )") == ":deadbeefxcafebabez1234567:\n:X:\n");
    }

    SUBCASE("fully-x and fully-z wide operands collapse lowercase") {
        CHECK(run_capture(R"(
            module m;
                logic [99:0] xv;
                initial begin
                    xv = 100'hx; $display(":%0d: :%h:", xv, xv);
                    xv = 100'hz; $display(":%0d: :%h:", xv, xv);
                end
            endmodule
        )") == ":x: :xxxxxxxxxxxxxxxxxxxxxxxxx:\n:z: :zzzzzzzzzzzzzzzzzzzzzzzzz:\n");
    }

    SUBCASE("72-bit operand across the word boundary; explicit widths") {
        // The high 8 bits sit in the second plane word. An explicit width larger
        // than the natural size zero-pads (LRM 21.2.1.2; iverilog space-pads).
        CHECK(run_capture(R"(
            module m;
                logic [71:0] sm;
                initial begin
                    sm = 72'hAA_BBBBBBBB_CCCCCCCC;
                    $display(":%0d: :%h: :%20h:", sm, sm, sm);
                end
            endmodule
        )") == ":3149474104851630443724: :aabbbbbbbbcccccccc: :00aabbbbbbbbcccccccc:\n");
    }

    SUBCASE("%u emits wide operands as 32-bit lanes, LSB lane first") {
        // {32'hC,32'hB,32'hA} -> lanes 0x0A, 0x0B, 0x0C, each little-endian.
        CHECK(run_capture(R"(
            module m;
                logic [95:0] v;
                initial begin v = {32'hC, 32'hB, 32'hA}; $write("%u", v); end
            endmodule
        )") == std::string("\x0a\x00\x00\x00\x0b\x00\x00\x00\x0c\x00\x00\x00", 12));
    }

    SUBCASE("$sformat with a wide operand") {
        CHECK(run_capture(R"(
            module m;
                logic [127:0] a;
                string out;
                initial begin
                    a = 128'hDEADBEEFCAFEBABE0123456789ABCDEF;
                    $sformat(out, "%h", a);
                    $display(":%s:", out);
                end
            endmodule
        )") == ":deadbeefcafebabe0123456789abcdef:\n");
    }
}

TEST_CASE("%p renders unpacked aggregates as assignment patterns (LRM 21.2.1.6)") {
    // `'{...}` with struct member names and decimal leaves: the clause prints a
    // singular element "as [it] would unformatted", which 21.2.1.1 makes decimal.
    // Verilator instead tags leaves `'h`.
    SUBCASE("fixed unpacked array") {
        CHECK(run_capture(R"(
            module m;
                int arr [0:3];
                int i;
                initial begin
                    for (i = 0; i < 4; i++) arr[i] = i * 10;
                    $display("%p", arr);
                end
            endmodule
        )") == "'{0, 10, 20, 30}\n");
    }
    SUBCASE("nested array of struct, and interleaved specs") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { logic [7:0] a; int b; } s_t;
                s_t sarr [0:1];
                initial begin
                    sarr[0].a = 8'h1; sarr[0].b = 1;
                    sarr[1].a = 8'h2; sarr[1].b = 2;
                    $display("x=%0d v=%p y=%0d", 7, sarr, 9);
                end
            endmodule
        )") == "x=7 v='{'{a:1, b:1}, '{a:2, b:2}} y=9\n");
    }
    SUBCASE("real, string, and string-literal leaves") {
        // The clause's remaining per-element rules: a string "enclosed in quotes",
        // a real as it would print unformatted. A bare string literal is an
        // integral packed value (LRM 5.9), so it needs the literal's own text, not
        // its decimal value.
        CHECK(run_capture(R"(
            module m;
                typedef struct { real r; string s; int n; } s_t;
                s_t v;
                string t;
                initial begin
                    v = '{1.5, "hi", -3};
                    t = "top";
                    $display("%p %p %p %p", v, t, "lit", 1.0/3.0);
                end
            endmodule
        )") == "'{r:1.5, s:\"hi\", n:-3} \"top\" \"lit\" 0.333333\n");
    }
    SUBCASE("a class handle operand is refused loudly") {
        // 21.2.1.6 wants an implementation-defined handle form with `null` for a null
        // handle; printing the raw handle word as a decimal would be silently wrong.
        check_rejected(R"(
            module m;
                class C; int x; endclass
                C c;
                initial $display("%p", c);
            endmodule
        )", "handle is not supported");
    }
}
