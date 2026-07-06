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

// -- Unsupported string constructs fail loudly (never silently accepted) --

namespace {

// Assert that `source` is rejected with a diagnostic containing `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
}

} // namespace

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
    SUBCASE("$sformatf") {
        check_rejected(R"(
            module m;
                string s;
                initial s = $sformatf("%0d", 1);
            endmodule
        )", "$sformatf");
    }
}
