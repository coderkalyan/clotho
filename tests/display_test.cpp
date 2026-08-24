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
}

// `%0p` is a SEPARATE specifier in 21.2.1.6, not a width applied to `%p`: it "shall
// print aggregate expressions ... in a shorter, implementation specific form". slang
// enforces the distinction ("Pattern args allow the zero-pad specifier"; every other
// width and left-justify is rejected for %p).
//
// The 0 reads as it does elsewhere -- drop the %d-width padding on integral leaves --
// and clotho's `%p` already renders every leaf minimally, so the two forms coincide.
// Pinned because the equality is a decision, not an accident: were `%p` ever to adopt
// padded leaves, `%0p` would have to keep the minimal ones. Verilator agrees the two
// are identical; iverilog cannot `%p` an aggregate at all, so it is no oracle here.
TEST_CASE("%0p is the shorter form of %p (LRM 21.2.1.6)") {
    // A scalar operand is the case the padding question actually bites: a bare %d of
    // an int pads to 11 columns, and neither %p nor %0p may.
    SUBCASE("scalar, struct and array agree with %p") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } s_t;
                s_t s; int arr [0:1]; int i;
                initial begin
                    s.a = 7; s.b = -3; arr[0] = 1; arr[1] = -2; i = 42;
                    $display("%p|%p|%p", s, arr, i);
                    $display("%0p|%0p|%0p", s, arr, i);
                end
            endmodule
        )") == "'{a:7, b:-3}|'{1, -2}|42\n"
               "'{a:7, b:-3}|'{1, -2}|42\n");
    }
    // The token is 3 characters, so the splice has to use the spec's real length: a
    // fixed 2 leaves a stray `p`. Mixed with `%p` it also pins that the FIFO order
    // survives, and the `%%p` escape is not mistaken for a conversion to splice over.
    SUBCASE("token length: mixed with %p and after a %%p escape") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; } s_t;
                s_t s; int arr [0:1];
                initial begin
                    s.a = 5; arr[0] = 1; arr[1] = 2;
                    $display("%0p-%p-%0d", s, arr, 9);
                    $display("%%p=%0p", s);
                end
            endmodule
        )") == "'{a:5}-'{1, 2}-9\n"
               "%p='{a:5}\n");
    }
}

// `%p` of a class handle. 21.2.1.6 describes %p over aggregate VALUES and a handle is
// not one, so the form is implementation-defined; clotho prints `null` for a null
// handle and the class name otherwise, at every depth. That uniformity is what makes
// it cycle-safe -- UVM's graphs are cyclic (uvm_component holds m_parent and
// m_children), so expanding properties would not terminate. Verilator 5.050 expands a
// direct operand (`'{x:'h5}`) but prints the type name inside a container
// (`'{t::C}`), so there is no consistent reference behaviour to match.
TEST_CASE("%p of a class handle prints null or the class name") {
    const std::string out = run_capture(R"(
        module m;
            class Inner;
                int y = 9;
            endclass
            class Outer;
                int   x = 5;
                Inner i;
            endclass
            typedef struct { int a; Outer h; } st;
            Outer o, nul;
            st    s;
            initial begin
                o = new;
                o.i = new;
                $display("null:   %p", nul);
                $display("live:   %p", o);
                s.a = 3;
                s.h = o;
                $display("member: %p", s);
                s.h = null;
                $display("mnull:  %p", s);
            end
        endmodule
    )");
    CHECK(out == "null:   null\nlive:   Outer\nmember: '{a:3, h:Outer}\nmnull:  '{a:3, h:null}\n");
}

// `%p` of a chandle, an event and a virtual-interface handle. This used to be
// refused loud, which was a refusal of an LRM `shall`: 21.2.1.6 says "a chandle,
// class handle, interface class handle, event, or virtual interface shall print its
// value in an implementation-dependent format, except that a null handle value shall
// print the word null". Only the non-null form is ours to choose, and it is chosen
// the same way the class-handle case above was -- the NAME of what the handle points
// at, at every depth, which is cycle-safe by construction:
//
//   a chandle -> `chandle`, an event -> `event` (LRM 6.14 / 15.5 give these types no
//   other name), a virtual interface -> the interface DEFINITION's name.
//
// The instance's hierarchical path was weighed for the virtual interface and rejected:
// it is instance identity where the class arm prints a type, and the handle is a
// context-record address with no runtime path table behind it. A raw pointer value
// was rejected because it is unstable across runs.
//
// libc malloc is the no-.so probe for a NON-NULL chandle: without DPI every chandle
// in a design is null (LRM 6.14 admits assignment only from another chandle or null),
// so it is the only way to exercise the non-null arm at run time.
TEST_CASE("%p of a chandle / event / virtual-interface handle (LRM 21.2.1.6)") {
    SUBCASE("direct operand, struct member and fixed-array element") {
        const std::string out = run_capture(R"(
            interface some_if;
                logic [7:0] d;
            endinterface
            module m;
                import "DPI-C" function chandle malloc(input longint size);
                import "DPI-C" function void    free(input chandle p);
                typedef struct { int a; chandle c; event e; virtual some_if v; } st;
                some_if bus();
                chandle         ch, chnul;
                event           ev, evnul;
                virtual some_if vif, vifnul;
                st              s;
                chandle         ca [0:1];
                event           ea [0:1];
                virtual some_if va [0:1];
                initial begin
                    ch    = malloc(64);
                    vif   = bus;
                    evnul = null;
                    $display("cnull: %p", chnul);
                    $display("clive: %p", ch);
                    $display("enull: %p", evnul);
                    $display("elive: %p", ev);
                    $display("vnull: %p", vifnul);
                    $display("vlive: %p", vif);
                    s = '{7, ch, ev, vif};
                    $display("mem:   %p", s);
                    s = '{7, null, null, null};
                    $display("mnul:  %p", s);
                    ca[1] = ch;
                    ea[0] = null;
                    va[1] = bus;
                    $display("arr:   %p %p %p", ca, ea, va);
                    $display("arr0:  %0p %0p %0p", ca, ea, va);
                    free(ch);
                end
            endmodule
        )");
        // An `event` declared without an initializer is bound to a NEW object (LRM
        // Table 6-7), so `ev` and `ea[1]` are non-null while an explicit `null` is not.
        CHECK(out == "cnull: null\n"
                     "clive: chandle\n"
                     "enull: null\n"
                     "elive: event\n"
                     "vnull: null\n"
                     "vlive: some_if\n"
                     "mem:   '{a:7, c:chandle, e:event, v:some_if}\n"
                     "mnul:  '{a:7, c:null, e:null, v:null}\n"
                     "arr:   '{null, chandle} '{null, event} '{null, some_if}\n"
                     "arr0:  '{null, chandle} '{null, event} '{null, some_if}\n");
    }
    // A container ELEMENT is the depth the runtime formatter serves from a FLAT
    // element descriptor, which describes one integral / real / string -- so a handle
    // element took it and printed the handle WORD (`'{1, 2}` for two events, a slot
    // index that is neither `null` nor a name and not stable). A queue of events is
    // the one handle element the container type system admits today, so it is the live
    // regression: it forces the frontend loop render, where the element goes through
    // the ordinary expansion and gets its name.
    SUBCASE("an event container element renders its name, not the handle word") {
        const std::string out = run_capture(R"(
            module m;
                event e0, e1;
                event q [$];
                event da [];
                event aa [int];
                initial begin
                    q.push_back(e0);
                    q.push_back(null);
                    da = new[2];
                    aa[3] = e1;
                    aa[9] = null;
                    $display("q:   %p", q);
                    $display("da:  %p", da);
                    $display("aa:  %p", aa);
                    $display("q0:  %0p", q);
                end
            endmodule
        )");
        // A dynamic array's elements are constructed, not defaulted to null (LRM
        // Table 6-7 again, via rt_elem_event), so `new[2]` yields two live events.
        CHECK(out == "q:   '{event, null}\n"
                     "da:  '{event, event}\n"
                     "aa:  '{3:event, 9:null}\n"
                     "q0:  '{event, null}\n");
    }
    // A queue / dynamic / associative array of chandles now lowers (the
    // container-element-type entry in docs/TODO.md); a virtual-interface handle is
    // the one still refused at TYPE lowering rather than by `%p`, tracked by the
    // same entry. `elem_is_handle` needed no change to print the chandle case --
    // it was already built chandle-aware, ahead of guard_container_element
    // catching up (the comment above this SUBCASE). libc malloc is the no-.so
    // probe for a non-null chandle.
    SUBCASE("a queue of chandles renders its element, no longer refused by the container") {
        const std::string out = run_capture(R"(
            module m;
                import "DPI-C" function chandle malloc(input longint size);
                import "DPI-C" function void    free(input chandle p);
                chandle q[$];
                chandle ch;
                initial begin
                    ch = malloc(8);
                    q.push_back(ch);
                    q.push_back(null);
                    $display("q: %p", q);
                    free(ch);
                end
            endmodule
        )");
        CHECK(out == "q: '{chandle, null}\n");
    }
    // A virtual-interface handle in a container is the one shape still refused at
    // TYPE lowering (LRM 25.9; guard_container_element does not admit it), pinned
    // so the diagnostic stays the accurate one and does not silently become a
    // `%p` refusal instead.
    SUBCASE("a queue of virtual-interface handles is refused by the container, not by %p") {
        check_rejected(R"(
            interface some_if; logic d; endinterface
            module m;
                some_if bus();
                virtual some_if q [$];
                initial $display("%p", q);
            endmodule
        )", "queue element type is not supported");
    }
    // A RUNTIME format string marshals its arguments through `rt_arg`, which carries
    // one integral / real / string value, and the frontend cannot know which specifier
    // an argument will meet -- so it cannot pre-render the handle either. That is not
    // hypothetical: `$display("%d", ch)` is accepted and prints the handle word, so a
    // handle genuinely may meet a numeric conversion and a pre-rendered name would be
    // wrong text. The refusal stays, and this pins that it names the type precisely.
    SUBCASE("a runtime format string refuses a handle argument, accurately") {
        check_rejected(R"(
            interface some_if; logic [7:0] d; endinterface
            module m;
                some_if bus();
                virtual some_if vif;
                string f, s;
                initial begin
                    vif = bus;
                    f   = "%p";
                    s   = $sformatf(f, vif);
                end
            endmodule
        )", "argument 2 is a virtual interface handle, which a runtime format string cannot render");
    }
}

// The uvm_resource#(T) shape (uvm-core/src/base/uvm_resource.svh:93): a parameterized
// class whose only view of T is `$sformatf("%p", val)`, specialized at
// T = virtual some_if by every `uvm_config_db#(virtual X)` in every UVM testbench.
// This exact site was the refusal that motivated the work, so it is pinned end to end
// rather than only through the pieces above.
TEST_CASE("%p of a virtual interface through a class parameter (uvm_resource shape)") {
    const std::string out = run_capture(R"(
        interface some_if;
            logic [7:0] d;
        endinterface
        class res #(type T = int);
            T val;
            virtual function string m_value_as_string();
                return $sformatf("%p", val);
            endfunction
        endclass
        module top;
            some_if bus();
            res #(virtual some_if) r;
            res #(int)             ri;
            initial begin
                r = new;
                ri = new;
                $display("unset: %s", r.m_value_as_string());
                r.val = bus;
                $display("bound: %s", r.m_value_as_string());
                ri.val = 42;
                $display("int:   %s", ri.m_value_as_string());
            end
        endmodule
    )");
    CHECK(out == "unset: null\nbound: some_if\nint:   42\n");
}

// A `%p` operand is expanded at lower time and needs an lvalue to walk, so an
// aggregate-returning call used to be refused here with an assignment-target
// message ("unsupported assignment target kind=Call") for what is a pure read. The
// call's value IS the caller-side out-param temp the callee wrote through, which is
// exactly the lvalue the walk wants.
TEST_CASE("%p of an aggregate function result (LRM 21.2.1.6)") {
    const std::string out = run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; int b; } st;
            typedef int ai [0:2];
            function automatic st gs(); st s; s.a = 8'hAB; s.b = 42; return s; endfunction
            function automatic ai ga(); ai r; r[0] = 7; r[1] = 8; r[2] = 9; return r; endfunction
            initial begin
                $display("struct %p", gs());
                $display("array %p", ga());
            end
        endmodule
    )");
    CHECK(out == "struct '{a:171, b:42}\narray '{7, 8, 9}\n");
}

// A `%p` operand was once lowered TWICE, which ran a call operand twice. The
// aggregate call result is precisely the shape that would reintroduce it, since the
// expansion walks the operand leaf by leaf: each leaf must select out of ONE
// evaluation, not re-lower the call. Counted with an observable side effect rather
// than by inspecting IR -- `calls` must advance by exactly one per printed call.
TEST_CASE("a %p call operand is evaluated exactly once") {
    const std::string out = run_capture(R"(
        module m;
            typedef struct { int a; int b; } st;
            int calls = 0;
            function automatic st bump(); calls = calls + 1; return '{calls, calls * 10}; endfunction
            initial begin
                $display("before %0d", calls);
                $display("p %p", bump());
                $display("after %0d", calls);
                $display("p %p", bump());
                $display("after %0d", calls);
            end
        endmodule
    )");
    CHECK(out == "before 0\np '{a:1, b:10}\nafter 1\np '{a:2, b:20}\nafter 2\n");
}

// An aggregate `?:` as a `%p` operand (LRM 11.4.11 + 21.2.1.6). The expansion
// walks the operand leaf by leaf and so needs anchored storage, which a
// conditional has none of; lower_lvalue stages it into one canonical aggregate
// temp. Like the call operand above, the arms must be evaluated the clause's
// number of times and no more -- once for the chosen arm of a definite condition,
// never for the other -- so the counters carry the real assertion.
TEST_CASE("a %p conditional operand") {
    SUBCASE("a struct, only the chosen arm evaluated") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; int b; } st;
                int nt = 0, nf = 0;
                function automatic st ft(); nt++; return '{a:1, b:2}; endfunction
                function automatic st ff(); nf++; return '{a:3, b:4}; endfunction
                initial begin
                    bit c;
                    c = 1;
                    $display("p %p nt=%0d nf=%0d", c ? ft() : ff(), nt, nf);
                    c = 0;
                    $display("p %p nt=%0d nf=%0d", c ? ft() : ff(), nt, nf);
                end
            endmodule
        )") == "p '{a:1, b:2} nt=1 nf=0\np '{a:3, b:4} nt=1 nf=1\n");
    }
    SUBCASE("a fixed unpacked array") {
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                ai p, q;
                initial begin
                    bit c;
                    p = '{1, 2, 3};
                    q = '{4, 5, 6};
                    c = 0;
                    $display("%p", c ? p : q);
                end
            endmodule
        )") == "'{4, 5, 6}\n");
    }
    SUBCASE("an ambiguous condition renders the merged result") {
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                ai p, q;
                initial begin
                    logic c;
                    p = '{1, 2, 3};
                    q = '{1, 9, 3};
                    c = 1'bx;
                    $display("%p", c ? p : q);
                end
            endmodule
        )") == "'{1, 0, 3}\n");
    }
}

// $display's format argument MUST be a string literal, and LRM 21.3.3 is explicit
// that the only exception is $sformat's: "The format arguments to these tasks must
// be string literals, i.e., they cannot be expressions of string data type. The
// only exception is $sformat, whose second argument can be an expression of string
// data type." So neither shape below is a format here:
//
//   - a concatenation of string literals is one integral value (LRM 5.9), printed
//     as such under the task's default radix;
//   - a `string` variable is a value argument, printed under %s, and the integer
//     after it is a bare value argument of its own.
//
// Verilator 5.050 agrees byte for byte. iverilog is lenient -- it
// treats both as formats -- and is wrong on the clause, so it is not the oracle.
//
// Pinned because $sformat gained a runtime format (see string_test.cpp): the two
// tasks share build_format, and nothing added there may leak $sformat's exception
// into the rest of the family.
TEST_CASE("$display's format argument is not an expression (LRM 21.3.3)") {
    CHECK(run_capture(R"(
        module m;
            string f;
            initial begin
                $display({"a=%0d ", "b=%0d"}, 1, 2);
                f = "c=%0d";
                $display(f, 7);
            end
        endmodule
    )") == "117554554874552423354282084          1          2\n"
           "c=%0d          7\n");
}
