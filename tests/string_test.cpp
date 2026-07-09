#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors function_test.cpp). String
// literals are used as packed-vector rvalues and read back with $display, so the
// captured hex is the observable value. Expectations cross-checked vs iverilog.
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

// A string literal is a packed array of 8-bit ASCII bytes (LRM 5.9). Assigned to
// an exactly-sized vector it packs left-to-right, MSB first.
TEST_CASE("string literal packs ASCII into an equal-width vector") {
    constexpr std::string_view src = R"(
        module m;
            reg [15:0] b;
            initial begin b = "AB"; $display("%h", b); end
        endmodule
    )";
    CHECK(run_capture(src) == "4142\n");
}

// A literal narrower than its target is right-justified and zero-padded.
TEST_CASE("string literal is zero-padded to a wider vector") {
    constexpr std::string_view src = R"(
        module m;
            reg [31:0] a;
            initial begin a = "AB"; $display("%h", a); end
        endmodule
    )";
    CHECK(run_capture(src) == "00004142\n");
}

// A literal wider than its target keeps the low bytes (the last characters).
TEST_CASE("string literal is truncated to a narrower vector") {
    constexpr std::string_view src = R"(
        module m;
            reg [7:0] s;
            initial begin s = "AB"; $display("%h", s); end
        endmodule
    )";
    CHECK(run_capture(src) == "42\n"); // low byte 'B'
}

// An empty string is all zero (used in picorv32 as a disassembler default).
TEST_CASE("empty string literal is zero") {
    constexpr std::string_view src = R"(
        module m;
            reg [63:0] e;
            initial begin e = ""; $display("%h", e); end
        endmodule
    )";
    CHECK(run_capture(src) == "0000000000000000\n");
}

// A full 64-bit (8-character) literal fills the word exactly.
TEST_CASE("8-character string fills a 64-bit vector") {
    constexpr std::string_view src = R"(
        module m;
            reg [63:0] c;
            initial begin c = "abcdefgh"; $display("%h", c); end
        endmodule
    )";
    CHECK(run_capture(src) == "6162636465666768\n");
}

// A literal wider than 64 bits is lowered correctly (not truncated to a word).
// Read back through <=64-bit part-selects, since wide $display is separately
// limited to 64 bits today.
TEST_CASE("string literal wider than 64 bits is not truncated") {
    constexpr std::string_view src = R"(
        module m;
            reg [95:0] w;
            initial begin
                w = "hello world!";
                $display("%h %h", w[95:64], w[63:0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "68656c6c 6f20776f726c6421\n"); // "hell" / "o world!"
}

// -- Dynamic string objects (LRM 6.16) --
//
// Expectations cross-checked vs iverilog where it supports the construct;
// iverilog lacks compare/icompare/toupper/tolower/putc and misreports
// arr[i].len() (returns a wrong length) and prints a spurious space for an
// unassigned string array element, so those cases follow the LRM directly.

TEST_CASE("a string defaults to empty and tracks assignment length") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            string t = "init";
            initial begin
                $display("%0d [%s]", s.len(), s);
                s = "hello";
                $display("%0d [%s]", s.len(), s);
                s = "";
                $display("%0d [%s]", s.len(), s);
                $display("%0d [%s]", t.len(), t);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 []\n5 [hello]\n0 []\n4 [init]\n");
}

TEST_CASE("string literals drop NUL bytes on conversion (LRM 6.16)") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                s = "hello\0world";
                $display("%0d [%s]", s.len(), s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10 [helloworld]\n");
}

TEST_CASE("string concatenation and replication, including self-append") {
    constexpr std::string_view src = R"(
        module m;
            string s, t;
            initial begin
                s = "ab";
                s = {s, s, "c"};
                $display("[%s]", s);
                t = {3{"xy"}};
                $display("[%s]", t);
                t = {2{""}};
                $display("%0d", t.len());
                s = {"H", ""};
                $display("[%s]", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[ababc]\n[xyxyxy]\n0\n[H]\n");
}

// The concat-into-store forwarding path (build_str_temps) plus the alias-aware
// in-place append in rt_string_concat. Each case cross-checked vs iverilog 13.
TEST_CASE("self-append concat grows in place, including multiple self-occurrences") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                s = "0123456789";
                s = {s, s};
                s = {s, s};
                $display("%0d %s", s.len(), s.substr(30, 39));
                s = {s, "X", s};
                $display("%0d %s", s.len(), s.substr(39, 41));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "40 0123456789\n81 9X0\n");
}

TEST_CASE("prepend concat (destination as a later part) keeps the scratch path") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            int i;
            initial begin
                s = "T";
                for (i = 0; i < 20; i++) s = {"ab", s};
                $display("%0d %s %s", s.len(), s.substr(0, 3), s.substr(38, 40));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "41 abab abT\n");
}

TEST_CASE("append loop crosses the inline-to-heap boundary mid-append") {
    constexpr std::string_view src = R"(
        module m;
            string s, chunk;
            int i;
            initial begin
                s = "";
                chunk = "abcd";
                for (i = 0; i < 64; i++) s = {s, chunk.substr(i % 4, i % 4)};
                $display("%0d %s %s", s.len(), s.substr(28, 35), s.substr(60, 63));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "64 abcdabcd abcd\n");
}

TEST_CASE("string append forwards for locals and stays on the temp path for array elements") {
    constexpr std::string_view src = R"(
        module m;
            string arr[2];
            function string join3(string a);
                string r;
                r = "";
                r = {r, a};
                r = {r, "-"};
                r = {r, a};
                return r;
            endfunction
            initial begin
                arr[0] = "x";
                arr[0] = {arr[0], "yz"};
                arr[1] = {arr[0], arr[0]};
                $display("%s %s %s", arr[0], arr[1], join3("ab"));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "xyz xyzxyz ab-ab\n");
}

TEST_CASE("string comparison operators and compare/icompare order in strcmp fashion") {
    constexpr std::string_view src = R"(
        module m;
            string s, t;
            initial begin
                s = "abc"; t = "abd";
                $display("%0d %0d %0d %0d %0d %0d", s==t, s!=t, s<t, s<=t, s>t, s>=t);
                t = "abc";
                $display("%0d %0d %0d", s==t, s<=t, s>=t);
                t = "ab"; // prefix orders before its extension
                $display("%0d", s > t);
                $display("%0d %0d", s.compare("abd") < 0, s.icompare("ABC"));
                $display("%0d", s == "abc"); // literal converts to string
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 1 1 1 0 0\n1 1 1\n1\n1 0\n1\n");
}

TEST_CASE("string indexing reads bytes and writes in place with LRM out-of-range rules") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                s = "abc";
                $display("%0d %0d %0d", s[0], s[2], s[5]); // OOB read -> 0
                s[1] = "Z";
                $display("[%s]", s);
                s[10] = "q";    // OOB write ignored
                s[0] = "\0";    // zero byte ignored (LRM 6.16.2)
                $display("[%s]", s);
                s.putc(2, "k");
                $display("[%s] %0d", s, s.getc(2));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "97 99 0\n[aZc]\n[aZc]\n[aZk] 107\n");
}

// s[i] and getc() return `byte`, a signed 8-bit type (LRM 6.16.3 and Table 6-8),
// so a 0xff character is -1 under %d, and widening into a 32-bit context sign-
// extends. NOTE: iverilog contradicts the LRM here (prints 255); the LRM is the
// oracle for this case, not iverilog.
TEST_CASE("string index and getc are signed bytes") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            byte b;
            bit [31:0] u;
            initial begin
                s = "a";
                s.putc(0, 8'hff);
                $display("%0d %0d", s[0], s.getc(0));
                b = s[0];
                $display("%0d", b);
                u = s[0];
                $display("%h", u);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-1 -1\n-1\nffffffff\n");
}

TEST_CASE("toupper, tolower, and substr follow LRM 6.16 semantics") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                s = "MiXeD 42!";
                $display("[%s] [%s]", s.toupper(), s.tolower());
                $display("[%s]", s); // receiver unchanged
                $display("[%s]", s.substr(1, 3));
                $display("%0d", s.substr(3, 2).len());  // j < i -> ""
                $display("%0d", s.substr(0, 20).len()); // j >= len -> ""
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[MIXED 42!] [mixed 42!]\n[MiXeD 42!]\n[iXe]\n0\n0\n");
}

TEST_CASE("string casts pack and unpack characters per LRM 6.16") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            typedef bit [11:0] r12_t;
            reg [31:0] r32;
            r12_t r12;
            initial begin
                s = string'(12'ha41); // LRM example: 2 chars, 0x0a 0x41
                $display("%0d", s.len());
                s = string'(16'h0041); // zero byte dropped
                $display("%0d [%s]", s.len(), s);
                s = "AB";
                r32 = int'(s); // right-justified, zero-filled left
                $display("%h", r32);
                s = "ABCDE";
                r12 = r12_t'(s); // truncated on the left
                $display("%h", r12);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n1 [A]\n00004142\n445\n");
}

TEST_CASE("strings pass to functions by value and return from them") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            function string exclaim(string x);
                return {x, "!"};
            endfunction
            function int len2(string x);
                x = {x, "xx"}; // mutation must stay local
                return x.len();
            endfunction
            initial begin
                $display("[%s]", exclaim("hi"));
                s = "keep";
                $display("%0d [%s]", len2(s), s);
                $display("[%s]", exclaim(exclaim(s)));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[hi!]\n6 [keep]\n[keep!!]\n");
}

TEST_CASE("strings pass to task inputs by value") {
    constexpr std::string_view src = R"(
        module m;
            string logline;
            task record(input string tag);
                logline = {logline, "<", tag, ">"};
            endtask
            initial begin
                record("a");
                record("bc");
                $display("[%s]", logline);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[<a><bc>]\n");
}

TEST_CASE("unpacked arrays of string init empty and assign per element") {
    constexpr std::string_view src = R"(
        module m;
            string arr [0:3];
            initial begin
                arr[0] = "zero";
                arr[1] = "one";
                arr[3] = {arr[0], "-", arr[1]};
                $display("[%s] [%s] [%s] [%s]", arr[0], arr[1], arr[2], arr[3]);
                $display("%0d %0d", arr[3].len(), arr[2].len());
                arr[3][4] = "+";
                $display("[%s]", arr[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[zero] [one] [] [zero-one]\n8 0\n[zero+one]\n");
}

TEST_CASE("string reassignment in a loop reuses capacity (leak smoke)") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            integer i;
            initial begin
                for (i = 0; i < 1000; i = i + 1) begin
                    s = {"iteration-", "with-a-heap-sized-payload-string"};
                    s = {s, s};
                end
                $display("%0d", s.len());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "84\n");
}

TEST_CASE("string comparisons drive control flow") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                s = "go";
                if (s == "go") $display("yes"); else $display("no");
                while (s.len() < 6) s = {s, "go"};
                $display("[%s]", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "yes\n[gogogo]\n");
}

TEST_CASE("if on a string tests emptiness") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                if (s) $display("t1"); else $display("f1");
                s = "x";
                if (s) $display("t2"); else $display("f2");
                s = "";
                if (s) $display("t3"); else $display("f3");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "f1\nt2\nf3\n");
}

TEST_CASE("while on a string runs until it empties") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            integer n;
            initial begin
                s = "abcd";
                n = 0;
                while (s) begin
                    s = s.substr(1, s.len() - 1);
                    n = n + 1;
                end
                $display("%0d %0d", n, s.len());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4 0\n");
}

TEST_CASE("for-loop string condition tests emptiness") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            integer n;
            initial begin
                n = 0;
                for (s = "ab"; s; s = s.substr(1, s.len() - 1)) n = n + 1;
                $display("%0d", n);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n");
}

TEST_CASE("ternary with a string condition selects by emptiness") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            integer v;
            initial begin
                v = s ? 1 : 2;
                $display("%0d", v);
                s = "go";
                v = s ? 1 : 2;
                $display("%0d", v);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n1\n");
}

// A constant string condition (a string'() cast of a literal folds to a pooled
// string constant at lowering) takes the same truthiness. A bare string literal
// in a condition is NOT a string: slang types it as a packed byte vector (LRM
// 5.9), and "" is 8'b0, so both routes agree that empty is false.
TEST_CASE("constant string conditions fold with the same truthiness") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                if (string'("")) $display("e-t"); else $display("e-f");
                if (string'("abc")) $display("n-t"); else $display("n-f");
                $display("%0d", string'("") ? 1 : 0);
                $display("%0d", string'("x") ? 1 : 0);
                if ("") $display("lit-t"); else $display("lit-f");
                if ("x") $display("litx-t"); else $display("litx-f");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "e-f\nn-t\n0\n1\nlit-f\nlitx-t\n");
}

// -- Unsupported string constructs fail loudly (never silently accepted) --

namespace {

// Assert that `source` is rejected with a diagnostic containing `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
}

} // namespace

// slang binds `!string` without diagnostic (Type::isBooleanConvertible
// whitelists StringType) but LRM Table 11-1 makes it illegal; Clotho rejects it.
TEST_CASE("logical negation of a string is rejected") {
    check_rejected(R"(
        module m;
            string s;
            reg r;
            initial r = !s;
        endmodule
    )", "logical negation of a string is not legal (LRM Table 11-1)");
}

TEST_CASE("unsupported string constructs are rejected loudly") {
    SUBCASE("nonblocking assignment to a string") {
        check_rejected(R"(
            module m;
                string s;
                initial s <= "x";
            endmodule
        )", "nonblocking assignment to a string");
    }
    SUBCASE("atoi method family") {
        check_rejected(R"(
            module m;
                string s;
                integer v;
                initial begin s = "42"; v = s.atoi(); end
            endmodule
        )", "string method 'atoi'");
    }
    SUBCASE("string module port") {
        check_rejected(R"(
            module sub(input string name);
            endmodule
            module m;
                string s;
                sub u(.name(s));
            endmodule
        )", "string module ports");
    }
    SUBCASE("string in an event control") {
        check_rejected(R"(
            module m;
                string s;
                initial @(s) $display("x");
            endmodule
        )", "string in an event control");
    }
    SUBCASE("ternary over strings") {
        check_rejected(R"(
            module m;
                string s, t;
                reg c;
                initial s = c ? s : t;
            endmodule
        )", "conditional operator (?:) over strings");
    }
    SUBCASE("case over strings") {
        check_rejected(R"(
            module m;
                string s;
                initial case (s) "a": $display("a"); endcase
            endmodule
        )", "case statement over strings");
    }
    SUBCASE("non-constant string replication count") {
        check_rejected(R"(
            module m;
                string s;
                integer n;
                initial begin n = 2; s = {n{"ab"}}; end
            endmodule
        )", "constant");
    }
    SUBCASE("output string task argument") {
        check_rejected(R"(
            module m;
                string s;
                task t(output string o);
                    o = "x";
                endtask
                initial t(s);
            endmodule
        )", "output/inout string task arguments");
    }
    SUBCASE("string argument to a non-%s specifier") {
        check_rejected(R"(
            module m;
                string s;
                initial $display("%d", s);
            endmodule
        )", "requires %s");
    }
    SUBCASE("$sformatf with a non-literal format") {
        check_rejected(R"(
            module m;
                string s, f;
                initial s = $sformatf(f, 1);
            endmodule
        )", "$sformatf format must be a string literal");
    }
}

TEST_CASE("$sformatf formats into a string value") {
    CHECK(run_capture(R"(
        module m;
            string s;
            initial begin
                s = $sformatf("v=%0d h=%h s=%s", 42, 8'hab, "hi");
                $display("[%s] len=%0d", s, s.len());
            end
        endmodule
    )") == "[v=42 h=ab s=hi] len=14\n");
}
