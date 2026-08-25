#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors function_test.cpp). String
// literals are used as packed-vector rvalues and read back with $display, so the
// captured hex is the observable value. Expectations cross-checked vs iverilog.
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

// -- Runtime (non-constant) replication multiplier, string result only --
//
// A DELIBERATE DEVIATION from LRM 11.4.12.1, which requires the multiplier to
// be a non-negative, non-x, non-z constant expression (see lower_expr's
// Replication case and the OPEN M3 ledger item in docs/TODO.md). The clause
// demands a constant so a packed result's width is known at elaboration; a
// string result has no elaboration-time width. iverilog 13 refuses the
// construct, Verilator 5.050 accepts it; UVM ships it (uvm_printer.svh:1501).

// The exact uvm_printer shape: the `dash.len() < m` guard around `{m{"-"}}`.
TEST_CASE("string replication with a runtime multiplier (the UVM guard shape)") {
    constexpr std::string_view src = R"(
        module m;
            string dash, m_space;
            int mx;
            initial begin
                dash = "";
                mx = 5;
                if (dash.len() < mx) begin
                    dash    = {mx{"-"}};
                    m_space = {mx{" "}};
                end
                $display("[%s][%s]", dash, m_space);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[-----][     ]\n");
}

// Two evaluations with different counts prove the multiplier is read at run
// time, not folded once. Zero is the clause's own blessed case ("a size of
// zero"): a runtime zero yields the empty string. (A CONSTANT zero outside a
// concatenation is refused by slang at elaboration, per the clause; no runtime
// check can apply there.)
TEST_CASE("runtime replication multiplier is read at each evaluation, zero gives empty") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            int n;
            integer k;
            initial begin
                n = 2;
                s = {n{"ab"}};
                $display("[%s]", s);
                n = 4;
                s = {n{"ab"}};
                $display("[%s]", s);
                n = 0;
                s = {n{"ab"}};
                $display("%0d[%s]", s.len(), s);
                // A DEFINITE 4-state count passes the x/z probe and replicates
                // like a 2-state one.
                k = 2;
                s = {k{"cd"}};
                $display("[%s]", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[abab]\n[abababab]\n0[]\n[cdcd]\n");
}

// The replication repeats the whole braced concatenation: a multi-character
// operand, a multi-piece operand, and a string VARIABLE operand with a
// constant count (the pre-deviation string shape, must not regress).
TEST_CASE("runtime replication of multi-character and multi-piece operands") {
    constexpr std::string_view src = R"(
        module m;
            string s, t;
            int n;
            initial begin
                n = 3;
                s = {n{"ab"}};
                $display("[%s]", s);
                s = {n{"a","b"}};
                $display("[%s]", s);
                t = "xy";
                s = {2{t}};
                $display("[%s]", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[ababab]\n[ababab]\n[xyxy]\n");
}

// A nested replication (constant and runtime inner counts) and a replication
// embedded in a larger concatenation, all with a runtime multiplier somewhere
// and a string result.
TEST_CASE("nested and embedded runtime string replication") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            int n, k;
            initial begin
                n = 2;
                s = {n{{2{"ab"}}}};
                $display("[%s]", s);
                k = 3;
                s = {n{{k{"x"}}}};
                $display("[%s]", s);
                n = 3;
                s = {"<", {n{"-"}}, ">"};
                $display("[%s]", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[abababab]\n[xxxxxx]\n[<--->]\n");
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

// atoi/atohex/atooct/atobin (LRM 6.16.9): leading digits (and '_') of the
// given base, stopping at the first other character; 0 if none matched. No
// leading whitespace or sign handling, and overflow truncates silently.
// Expectations cross-checked vs iverilog.
TEST_CASE("atoi family scans leading digits per LRM 6.16.9") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            integer r;
            initial begin
                s = "123"; r = s.atoi(); $display("%0d", r);
                s = "-123"; r = s.atoi(); $display("%0d", r);   // no sign handling -> 0
                s = "1_2_3"; r = s.atoi(); $display("%0d", r);  // '_' skipped
                s = "12abc"; r = s.atoi(); $display("%0d", r);  // stops at 'a'
                s = ""; r = s.atoi(); $display("%0d", r);
                s = "4294967296123"; r = s.atoi(); $display("%0d", r); // truncates
                s = "FF"; r = s.atohex(); $display("%0d", r);
                s = "17"; r = s.atooct(); $display("%0d", r);
                s = "101"; r = s.atobin(); $display("%0d", r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "123\n0\n123\n12\n0\n123\n255\n15\n5\n");
}

// atoreal (LRM 6.16.10): unlike the atoi family, leading whitespace and an
// optional '+' are accepted (iverilog-verified); 0.0 if no digits matched.
TEST_CASE("atoreal parses a real-literal prefix per LRM 6.16.10") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            real r;
            initial begin
                s = "3.14"; r = s.atoreal(); $display("%f", r);
                s = "  -3.14"; r = s.atoreal(); $display("%f", r);
                s = "+3.14"; r = s.atoreal(); $display("%f", r);
                s = "abc"; r = s.atoreal(); $display("%f", r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3.140000\n-3.140000\n3.140000\n0.000000\n");
}

// itoa/hextoa/octtoa/bintoa (LRM 6.16.11-6.16.14): sign + magnitude in the
// given base, lowercase digits — not a two's-complement bit-pattern dump.
// x/z bits of the operand read as 0 (StrFromPacked's
// must-plane convention). Expectations cross-checked vs iverilog.
TEST_CASE("itoa family formats sign + magnitude per LRM 6.16.11-6.16.14") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            integer r;
            initial begin
                r = 123; s.itoa(r); $display("%s", s);
                r = -123; s.itoa(r); $display("%s", s);
                r = 255; s.hextoa(r); $display("%s", s);
                r = -256; s.hextoa(r); $display("%s", s);
                r = 8; s.octtoa(r); $display("%s", s);
                r = 5; s.bintoa(r); $display("%s", s);
                r = 32'h1x; s.itoa(r); $display("%s", s); // x reads as 0 -> 0x10
            end
        endmodule
    )";
    CHECK(run_capture(src) == "123\n-123\nff\n-100\n10\n101\n16\n");
}

// realtoa (LRM 6.16.15): full round-trip precision, not iverilog's 6-sig-fig
// %g truncation (a deliberate LRM-compliant deviation).
TEST_CASE("realtoa formats at full round-trip precision") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            real r;
            initial begin
                r = 3.14; s.realtoa(r); $display("%s", s);
                r = 100.0; s.realtoa(r); $display("%s", s);
                r = 123456789.123; s.realtoa(r); $display("%s", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3.14\n100\n123456789.123\n");
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

// -- `output` / `inout` string formals (LRM 13.5) --
//
// A string is Managed (docs/design/data-model.md §6): the formal is passed as one
// rt_string handle pointer aimed at a caller temp, the callee deep-copies in
// (`inout` only) and moves its frame slot back out at completion, and the frontend
// then copies the temp into the actual. Oracle: iverilog 13 for every TASK case
// below (each expectation is byte-identical to it). iverilog rejects an output
// formal on a FUNCTION outright ("Function arguments must be input ports"), so the
// function cases rest on LRM 13.5.1 / 13.5.3 directly, like the existing scalar
// function out / inout support.

// The direction table: an `output` formal starts EMPTY however full the actual is
// (nothing is copied in), an `inout` formal starts from the actual, and both land
// in the actual at completion.
TEST_CASE("output string starts empty and inout string starts from the actual") {
    constexpr std::string_view src = R"(
        module m;
            task automatic app(output string o, inout string u);
                o = {o, "o"};
                u = {u, "u"};
            endtask
            string a, b;
            initial begin
                a = "PRE";
                b = "PRE";
                app(a, b);
                $display("a=[%s] b=[%s]", a, b);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a=[o] b=[PREu]\n");
}

// A function's out / inout string coexists with its return value, and the callee's
// own frame copy is what it mutates.
TEST_CASE("function output and inout string arguments alongside a return value") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int bump(output string o, inout string io, input string i);
                o  = {"o<", i, ">"};
                io = {io, "+", i};
                return io.len();
            endfunction
            string a, b;
            int    r;
            initial begin
                b = "B";
                r = bump(a, b, "in");
                $display("a=[%s] b=[%s] r=%0d", a, b, r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a=[o<in>] b=[B+in] r=4\n");
}

// Copy-out re-resolves the actual's lvalue AFTER the call, so any place a string
// store can name is a legal actual: a queue / dynamic-array / associative-array
// element, an unpacked-struct member, a fixed-array element and a class property.
// Two actuals of the same call may name elements of one container (`q[0]`, `q[1]`).
TEST_CASE("output/inout string actual can be a container element, member or property") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                string p;
            endclass
            typedef struct { string f; int n; } S;
            task automatic fill(output string o, inout string io, input string tag);
                o  = {"[", tag, "]"};
                io = {io, "+", tag};
            endtask
            string q[$];
            string d[];
            string mi[int];
            string ms[string];
            string arr[2];
            S      st;
            C      obj;
            initial begin
                q.push_back("q0");
                q.push_back("q1");
                d = new[2];
                d[0] = "d0";
                d[1] = "d1";
                mi[7]   = "m7";
                ms["k"] = "mk";
                arr[1]  = "a1";
                st.f    = "sf";
                obj     = new;
                obj.p   = "op";
                fill(q[0], q[1], "Q");
                $display("q=[%s][%s]", q[0], q[1]);
                fill(d[1], d[0], "D");
                $display("d=[%s][%s]", d[0], d[1]);
                fill(mi[7], ms["k"], "M");
                $display("m=[%s][%s]", mi[7], ms["k"]);
                fill(arr[1], st.f, "S");
                $display("s=[%s][%s]", arr[1], st.f);
                fill(obj.p, st.f, "C");
                $display("c=[%s][%s]", obj.p, st.f);
            end
        endmodule
    )";
    // The queue / dynamic-array lines are byte-identical to iverilog 13; it
    // supports neither a string-keyed associative array nor these class / struct
    // actuals, so those follow LRM 13.5 (an ordinary write to the actual's place).
    CHECK(run_capture(src) == "q=[[Q]][q1+Q]\nd=[d0+D][[D]]\nm=[[M]][mk+M]\n"
                              "s=[[S]][sf+S]\nc=[[C]][sf+S+C]\n");
}

// An out / inout formal is a private copy, not an alias (that is `ref`): the callee
// reading the caller's variable directly sees its pre-call value, and the actual
// changes only at completion.
TEST_CASE("output/inout string formals are copies, not aliases") {
    constexpr std::string_view src = R"(
        module m;
            string g;
            task automatic peek(output string o, inout string u);
                o = {"saw:", g};
                u = {"saw:", g};
                g = "callee-wrote-g";
            endtask
            string a, b;
            initial begin
                g = "G0";
                a = "A0";
                b = "B0";
                peek(a, b);
                $display("a=[%s] b=[%s] g=[%s]", a, b, g);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a=[saw:G0] b=[saw:G0] g=[callee-wrote-g]\n");
}

// A task's copy-out rides its completion epilogue, so a write before a `#delay` is
// NOT visible to a witness process mid-body — it lands when the caller resumes.
TEST_CASE("task output/inout string lands at completion, not at the write") {
    constexpr std::string_view src = R"(
        module m;
            string s, io;
            task automatic slow(output string o, inout string u);
                o = "early";
                #5;
                o = {o, "-late"};
                u = {u, "-done"};
            endtask
            initial begin
                io = "K";
                slow(s, io);
                $display("caller resumed at %0t: s=[%s] io=[%s]", $time, s, io);
            end
            initial begin
                #1 $display("witness at %0t: s=[%s] io=[%s]", $time, s, io);
                #10 $display("witness at %0t: s=[%s] io=[%s]", $time, s, io);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "witness at 1: s=[] io=[K]\n"
                              "caller resumed at 5: s=[early-late] io=[K-done]\n"
                              "witness at 11: s=[early-late] io=[K-done]\n");
}

// A `disable`d activation takes the ordinary epilogue with the CALLEE's copy-out
// skipped (LRM 9.6.2 leaves out / inout results undefined), so each actual receives
// whatever its caller temp holds: for an `output` the untouched temp (an empty
// string, a zero scalar), for an `inout` the value copied in before the call. The
// callee's frame slot is still deinit'd, so the abandoned string is freed, not
// leaked. DELIBERATE DIVERGENCE from iverilog 13, which copies out the values the
// body wrote before the abort (`k1=[pre] k2=[K2-pre] n=7`); 9.6.2 makes both
// conforming, and this is clotho's pre-existing policy for scalar out / inout args
// (the scalar `n` in this design), which a string now matches exactly rather than
// growing a second rule.
TEST_CASE("disable of a task skips its string copy-out") {
    constexpr std::string_view src = R"(
        module m;
            string k1, k2;
            int    n;
            task automatic killme(output string o, inout string io, output int cnt);
                o   = "pre";
                io  = {io, "-pre"};
                cnt = 7;
                #20;
                o   = "post";
                io  = {io, "-post"};
                cnt = 9;
            endtask
            initial begin
                k1 = "K1";
                k2 = "K2";
                n  = 3;
                killme(k1, k2, n);
                $display("resumed at %0t: k1=[%s] k2=[%s] n=%0d", $time, k1, k2, n);
            end
            initial #2 disable killme;
        endmodule
    )";
    CHECK(run_capture(src) == "resumed at 2: k1=[] k2=[K2] n=0\n");
}

// Recursion through an inout string: each activation owns its frame handle, so the
// accumulated value threads down and back up one copy per level. The loop is a
// leak smoke test — 200 iterations x 20 levels of copy-in / copy-out.
TEST_CASE("recursion through an inout string argument (leak smoke)") {
    constexpr std::string_view src = R"(
        module m;
            task automatic grow(inout string acc, input int n);
                if (n == 0) return;
                acc = {acc, "ab"};
                grow(acc, n - 1);
            endtask
            string s;
            int    i;
            initial begin
                s = "";
                grow(s, 6);
                $display("%0d [%s]", s.len(), s);
                for (i = 0; i < 200; i++) begin
                    s = "";
                    grow(s, 20);
                end
                $display("loop end %0d", s.len());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "12 [abababababab]\nloop end 40\n");
}

// A class method's out / inout string: `this` is formal 0, so the string formals
// sit at shifted parameter indices — the per-signature index table has to stride
// one pointer for each of them.
TEST_CASE("class method with output and inout string arguments") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                string p;
                task automatic mt(output string s, inout string u);
                    s = {"C:", p};
                    u = {u, "!"};
                endtask
                function automatic int mf(output string s);
                    s = {"F:", p};
                    return 1;
                endfunction
            endclass
            C      obj;
            string c;
            int    r;
            initial begin
                obj   = new;
                obj.p = "P";
                c     = "C0";
                obj.mt(obj.p, c);
                $display("p=[%s] c=[%s]", obj.p, c);
                r = obj.mf(obj.p);
                $display("p=[%s] r=%0d", obj.p, r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "p=[C:P] c=[C0!]\np=[F:C:P] r=1\n");
}

// -- Whole-string nonblocking assignment (LRM 10.4.2, 4.9.4) --
//
// A string is Managed, so its NBA cannot stage into the plane-shaped per-signal
// shadow: `s <= t` reserves a record holding an OWNED deep copy of the RHS in the
// Active region and rt_string_move's it into the target in the NBA region (the
// same assign-at-sample / move-at-commit discipline the whole-container NBA uses;
// docs/design/queue-nba-and-nonpod.md). No oracle: iverilog 13 aborts on `s <= t`
// (assertion in eval_vec4.c), so these follow the LRM's region semantics, which
// the swap below is the canonical witness of.
TEST_CASE("whole-string nonblocking assignment samples in Active and commits in NBA") {
    constexpr std::string_view src = R"(
        module m;
            string a, b;
            initial begin
                a = "A";
                b = "B";
                a <= b;
                b <= a;
                $display("same instant: a=[%s] b=[%s]", a, b);
                #1 $display("after NBA: a=[%s] b=[%s]", a, b);
            end
        endmodule
    )";
    // The swap is what distinguishes sample-then-commit from a blocking store: a
    // blocking pair would leave both "B".
    CHECK(run_capture(src) == "same instant: a=[A] b=[B]\nafter NBA: a=[B] b=[A]\n");
}

// Statement-execution order decides the winner (LRM 10.4.2): records replay in push
// order, and each move frees what the target held, so the superseded values are
// released exactly once. The lengths straddle the 30-byte inline/heap boundary in
// both directions, which is where a mis-staged handle would corrupt or leak.
TEST_CASE("repeated string NBAs to one target: last write wins across inline and heap") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                s <= "first";
                s <= "0123456789012345678901234567890123456789";
                s <= "third";
                #1 $display("[%s] %0d", s, s.len());
                s <= "0123456789012345678901234567890123456789";
                #1 $display("[%s] %0d", s, s.len());
                s <= "tiny";
                #1 $display("[%s] %0d", s, s.len());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[third] 5\n[0123456789012345678901234567890123456789] 40\n[tiny] 4\n");
}

// The RHS is deep-copied at sample time, so a self-referencing RHS and a source
// mutated later in the same instant both see the sampled value, not the committed
// one. A task's NBA to a module string stages into the same per-signal record.
TEST_CASE("string NBA samples the RHS value, including from a task") {
    constexpr std::string_view src = R"(
        module m;
            string a, b, c;
            task automatic stage(input string v);
                c <= v;
            endtask
            initial begin
                a = "x";
                a <= {a, a, "!"};
                b = "src";
                c <= b;
                b = "mutated-after-sampling";
                #1 $display("a=[%s] c=[%s]", a, c);
                stage("from-task");
                #1 $display("c=[%s]", c);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a=[xx!] c=[src]\nc=[from-task]\n");
}

// A container element or struct member as the SOURCE of a string NBA is ordinary:
// the record stages a copy of whatever the RHS reads.
TEST_CASE("string NBA from a container element or struct member") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { string f; } S;
            string q[$];
            string mp[string];
            S      st;
            string w;
            initial begin
                q.push_back("qv");
                mp["k"] = "mv";
                st.f    = "sv";
                w <= q[0];
                #1 $display("[%s]", w);
                w <= mp["k"];
                #1 $display("[%s]", w);
                w <= st.f;
                #1 $display("[%s]", w);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[qv]\n[mv]\n[sv]\n");
}

// A string LEAF as the target: a struct member and a fixed-array element. The
// record carries the leaf's own destination pointer, so one record range per root
// signal serves every leaf of it — and the leaf swap proves each is sampled before
// any is committed. A whole-struct NBA carrying a string member decomposes into the
// same per-leaf records.
TEST_CASE("string NBA to a struct member and a fixed-array element") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { string s; int n; } SS;
            SS     a, b;
            string arr[2];
            string t;
            initial begin
                t = "T";
                a.s = "A";
                a.n = 1;
                a.s <= t;
                arr[0] <= t;
                arr[1] <= "0123456789012345678901234567890123456789";
                #1 $display("1: [%s] [%s] [%s] %0d", a.s, arr[0], arr[1], arr[1].len());
                b.s = "B";
                b.n = 9;
                a <= b;
                #1 $display("2: [%s] %0d", a.s, a.n);
                arr[0] = "x";
                arr[1] = "y";
                arr[0] <= arr[1];
                arr[1] <= arr[0];
                #1 $display("3: [%s][%s]", arr[0], arr[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1: [T] [T] [0123456789012345678901234567890123456789] 40\n"
                              "2: [B] 9\n3: [y][x]\n");
}

// One root signal taking BOTH a string-leaf NBA and a byte-image aggregate NBA in
// the same instant: the two record ranges are independent, and both commit.
TEST_CASE("string leaf and integral leaf NBA on one aggregate signal") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { string s; int n[2]; } MX;
            MX     v;
            string t;
            int    i;
            initial begin
                t = "T";
                i = 1;
                v.s    <= t;
                v.n[i] <= 42;
                v.n[0] <= 7;
                #1 $display("[%s] %0d %0d", v.s, v.n[0], v.n[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[T] 7 42\n");
}

// 4000 commits alternating heap and inline: the staged record's capacity is retained
// across instants (rt_string_move re-inits the source), so this must neither grow
// nor leak. Mirrors the "string reassignment in a loop reuses capacity" smoke test.
TEST_CASE("string NBA in a loop reuses the record's capacity (leak smoke)") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            initial begin
                for (int i = 0; i < 2000; i++) begin
                    s <= "0123456789012345678901234567890123456789012345678901234567890123";
                    #1;
                    s <= "s";
                    #1;
                end
                $display("[%s] %0d", s, s.len());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[s] 1\n");
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

// LRM 5.9: "A string literal can be assigned to an unpacked array of bytes.
// If the size differs, it is left justified." Left-justified: character 0
// lands at the array's declared left bound; a shorter string zero-pads the
// remaining elements, a longer string's tail is dropped. iverilog does not
// implement the construct at all.
TEST_CASE("string literal assigns element-wise into an unpacked byte array") {
    constexpr std::string_view src = R"(
        module m;
            byte exact [0:2] = "abc";
            byte sm [0:4] = "abcdefgh";
            byte big [0:4] = "ab";
            byte desc [4:0] = "ab";
            initial begin
                $display("%0d %0d %0d", exact[0], exact[1], exact[2]);
                $display("%0d %0d %0d %0d %0d", sm[0], sm[1], sm[2], sm[3], sm[4]);
                $display("%0d %0d %0d %0d %0d", big[0], big[1], big[2], big[3], big[4]);
                $display("%0d %0d %0d %0d %0d", desc[4], desc[3], desc[2], desc[1], desc[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "97 98 99\n97 98 99 100 101\n97 98 0 0 0\n97 98 0 0 0\n");
}

TEST_CASE("string literal assigns element-wise into an unpacked byte array via plain assignment") {
    constexpr std::string_view src = R"(
        module m;
            byte arr [0:4];
            initial begin
                arr = "hi";
                $display("%0d %0d %0d %0d %0d", arr[0], arr[1], arr[2], arr[3], arr[4]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "104 105 0 0 0\n");
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

// -- Aggregates whose leaves own heap, passed BY VALUE (LRM 13.5.1, 7.4) --
//
// An unpacked array of strings (or any aggregate with a string / container leaf)
// copies per leaf rather than by memcpy: duplicating a handle's bytes would leave
// two owners of one allocation (docs/design/data-model.md §6, the "Aggregate,
// managed leaf" row). No oracle: iverilog supports neither unpacked-struct nor
// unpacked-array subroutine ports, so these follow the LRM.

// The defining property of by-value: the callee's mutation of its own copy is
// invisible to the caller, and the caller's strings survive the call intact (a
// memcpy copy-in would have had the callee's frame free them at its return).
TEST_CASE("string-element array passes by value") {
    constexpr std::string_view src = R"(
        module m;
            typedef string SA[2];
            function automatic int chew(SA x);
                x[0] = "mutated";
                x[1] = {x[1], x[1]};
                return x[1].len();
            endfunction
            SA a;
            int r;
            initial begin
                a[0] = "keep0";
                a[1] = "0123456789012345678901234567890123456789";
                r = chew(a);
                $display("%0d [%s] [%s] %0d", r, a[0], a[1], a[1].len());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "80 [keep0] [0123456789012345678901234567890123456789] 40\n");
}

// output / inout formals of the same shape: the callee fills its frame copy and the
// epilogue copies it back per leaf, releasing what the actual held.
TEST_CASE("string-element array as an output and inout formal") {
    constexpr std::string_view src = R"(
        module m;
            typedef string SA[2];
            task automatic fill(output SA o, inout SA io);
                o[0] = "o0";
                o[1] = "0123456789012345678901234567890123456789";
                io[0] = {io[0], "+"};
                io[1] = "replaced";
            endtask
            SA c, b;
            initial begin
                b[0] = "b0";
                b[1] = "b1";
                fill(c, b);
                $display("[%s][%s] [%s][%s]", c[0], c[1], b[0], b[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[o0][0123456789012345678901234567890123456789] [b0+][replaced]\n");
}

// By-value RETURN: the result lands in a caller-owned, body-lifetime buffer whose
// leaves the callee assigns into, so the returned strings outlive the callee's
// frame and are released at the caller's exit rather than leaked.
TEST_CASE("aggregate return with string leaves, including a nested array") {
    constexpr std::string_view src = R"(
        module m;
            typedef string SA[2];
            typedef struct { string s; SA a; int n; } SN;
            function automatic SA mk_arr();
                SA r;
                r[0] = "arr0";
                r[1] = "0123456789012345678901234567890123456789";
                return r;
            endfunction
            function automatic SN mk_rec();
                SN r;
                r.s    = "rec";
                r.a    = mk_arr();
                r.n    = 7;
                return r;
            endfunction
            SA a;
            SN n;
            initial begin
                a = mk_arr();
                $display("[%s][%s] %0d", a[0], a[1], a[1].len());
                n = mk_rec();
                $display("[%s] [%s][%s] %0d", n.s, n.a[0], n.a[1], n.n);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[arr0][0123456789012345678901234567890123456789] 40\n"
                              "[rec] [arr0][0123456789012345678901234567890123456789] 7\n");
}

// A whole-array assignment and a slice of a string array are element-wise deep
// copies: mutating the source afterwards must not touch the destination.
TEST_CASE("whole-array and slice assignment of a string array deep-copy") {
    constexpr std::string_view src = R"(
        module m;
            string a[4], b[4];
            initial begin
                a[0] = "zero";
                a[1] = "0123456789012345678901234567890123456789";
                a[2] = "two";
                a[3] = "three";
                b = a;
                a[0] = "clobbered";
                a[1] = "also-clobbered";
                $display("[%s][%s][%s][%s]", b[0], b[1], b[2], b[3]);
                b[0:1] = a[2:3];
                $display("[%s][%s]", b[0], b[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[zero][0123456789012345678901234567890123456789][two][three]\n[two][three]\n");
}

// A string-element array as a subroutine LOCAL: the frame's aggregate mirror
// constructs each leaf at activation and releases it at every exit, so recursion
// and repeated calls neither leak nor alias.
TEST_CASE("string-element array as a subroutine local, recursively") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int build(int depth);
                string tmp[2];
                tmp[0] = "0123456789012345678901234567890123456789";
                tmp[1] = "b";
                if (depth == 0) return tmp[0].len();
                return tmp[1].len() + build(depth - 1);
            endfunction
            initial $display("%0d", build(5));
        endmodule
    )";
    CHECK(run_capture(src) == "45\n");
}

// The by-value copy of a container leaf is a deep copy too, so a queue member of a
// passed struct is independent in the callee.
TEST_CASE("aggregate with a queue member passes by value") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int q[$]; string tag; } QS;
            function automatic int grow(QS x);
                x.q.push_back(99);
                x.tag = "callee";
                return x.q.size();
            endfunction
            QS v;
            int r;
            initial begin
                v.q.push_back(1);
                v.q.push_back(2);
                v.tag = "caller";
                r = grow(v);
                $display("%0d %0d [%s]", r, v.q.size(), v.tag);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 2 [caller]\n");
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

// -- String truthiness in conditions --
//
// The LRM leaves the truth of a string undefined (§12.4 tests a "numeric
// value"), so clotho defines it: non-empty = true, empty = false, which is what
// slang's const-eval also does. iverilog crashes on every one of these forms.

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

// `?:` yielding a string (LRM 11.4.11). The arms are rt_string handles, so the
// select merges pointers; the consumer (assignment, concat, argument, compare)
// copies what it keeps. Oracle: hand-derived from the LRM plus slang's own
// constant evaluator. iverilog crashes on a string ternary and verilator
// disagrees on an ambiguous condition (it takes the false arm), so neither is an
// oracle for the X case below.
TEST_CASE("ternary over strings selects an arm") {
    constexpr std::string_view src = R"(
        module m;
            string a = "yes", b = "no", r;
            reg c;
            initial begin
                c = 1'b1;
                r = c ? a : b;
                $display("[%s]", r);
                c = 1'b0;
                r = c ? a : b;
                $display("[%s]", r);
                r = c ? a : "lit";
                $display("[%s]", r);
                r = (a == "yes") ? {a, "!"} : b;
                $display("[%s]", r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[yes]\n[no]\n[lit]\n[yes!]\n");
}

// The target appearing as an arm: rt_string_assign treats dst == src as a no-op,
// so the self-referential form neither frees nor truncates the live string.
TEST_CASE("ternary over strings assigns through its own target") {
    constexpr std::string_view src = R"(
        module m;
            string s = "keep", t = "other";
            reg c;
            initial begin
                c = 1'b1;
                s = c ? s : t;
                $display("[%s]", s);
                c = 1'b0;
                s = c ? s : t;
                $display("[%s]", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[keep]\n[other]\n");
}

// An arm with an observable effect takes the lazy path (a branch per arm, not a
// branchless select), so the unchosen call never runs. $sformatf and substr both
// produce a fresh temp handle inside their arm's region.
TEST_CASE("ternary over strings evaluates only the chosen call") {
    constexpr std::string_view src = R"(
        module m;
            string s = "abcd", t = "tail", r;
            integer n, calls;
            function string bump(string v);
                calls = calls + 1;
                return {v, "*"};
            endfunction
            initial begin
                n = 7;
                r = (n > 5) ? $sformatf("big %0d", n) : "small";
                $display("[%s]", r);
                r = (n < 5) ? $sformatf("big %0d", n) : "small";
                $display("[%s]", r);
                r = (n > 5) ? s.substr(0, 1) : t;
                $display("[%s]", r);
                calls = 0;
                r = (n > 5) ? bump(s) : bump(t);
                $display("[%s] %0d", r, calls);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[big 7]\n[small]\n[ab]\n[abcd*] 1\n");
}

// An ambiguous condition cannot blend two handles the way it blends bits, so it
// yields the common value when the arms are equal and the string default ""
// otherwise -- the same rule real uses with 0.0 (LRM 11.4.11 defines its
// bitwise merge for integral results only, and slang's constant evaluator falls
// back to the type's default value).
TEST_CASE("ternary over strings with an ambiguous condition") {
    constexpr std::string_view src = R"(
        module m;
            string a = "aa", b = "bb", e1 = "same", e2 = "same", r;
            reg c;
            initial begin
                c = 1'bx;
                r = c ? a : b;
                $display("[%s] %0d", r, r.len());
                r = c ? e1 : e2;
                $display("[%s]", r);
                c = 1'bz;
                r = c ? a : b;
                $display("[%s]", r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[] 0\n[same]\n[]\n");
}

// A string ternary in a consumer that is not an assignment: the selected handle
// is borrowed, exactly like a Load or a passed argument.
TEST_CASE("ternary over strings feeds concat, compare, argument and display") {
    constexpr std::string_view src = R"(
        module m;
            string a = "aa", b = "bb", r;
            reg c;
            function integer plen(string v);
                return v.len();
            endfunction
            initial begin
                c = 1'b1;
                $display("[%s]", c ? a : b);
                r = {(c ? a : b), "-tail"};
                $display("[%s]", r);
                $display("%0d", (c ? a : b) == "aa");
                $display("%0d", plen(c ? a : "xyz"));
                c = 1'b0;
                $display("%0d", plen(c ? a : "xyz"));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "[aa]\n[aa-tail]\n1\n2\n3\n");
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
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
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

// A negative runtime replication multiplier is an ILLEGAL value, not a defined
// case: LRM 11.4.12.1 blesses only zero. Clamping to zero would invent
// semantics the clause does not grant, and Verilator gives no answer to match
// either way (its generated C++ for `{-2{"ab"}}` fails to compile, make exits
// 2). So: a located fatal naming the construct and the value, the same shape
// as `new[]` with a negative size. Out of process because the fatal exits.
TEST_CASE("negative runtime replication multiplier is a located fatal") {
    constexpr std::string_view src = "module m;\n"
                                     "    string s;\n"
                                     "    int n;\n"
                                     "    initial begin\n"
                                     "        n = -2;\n"
                                     "        s = {n{\"ab\"}};\n"
                                     "        $display(\"unreachable\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out.empty());
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("invalid negative multiplier for this string replication: -2 "
                       "(LRM 11.4.12.1 requires a non-negative multiplier)") != std::string::npos);
    // The site record points the report at the design, not at the runtime.
    CHECK(run.err.find("source:6:") != std::string::npos);
    CHECK(run.err.find("s = {n{\"ab\"}};") != std::string::npos);
}

// An x/z multiplier is the clause's other illegal value. Only a 4-state count
// can hold one — the probe is emitted only for a 4-state count plane, so the
// 2-state `int` multiplier above pays nothing for this check.
TEST_CASE("x/z runtime replication multiplier is a located fatal") {
    constexpr std::string_view src = "module m;\n"
                                     "    string s;\n"
                                     "    integer n;\n"
                                     "    initial begin\n"
                                     "        n = 'x;\n"
                                     "        s = {n{\"ab\"}};\n"
                                     "        $display(\"unreachable\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out.empty());
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("invalid x/z multiplier for this string replication "
                       "(LRM 11.4.12.1 requires a non-x, non-z multiplier)") != std::string::npos);
    CHECK(run.err.find("source:6:") != std::string::npos);
}

// The deviation is ONLY for a string result: a runtime multiplier with a
// PACKED result still has a width that must be known at elaboration, so it
// stays refused. slang itself rejects the shape (the multiplier fails constant
// evaluation) before clotho's backstop in lower_expr's Replication case — the
// needle below is slang's diagnostic.
TEST_CASE("runtime replication multiplier with a packed result stays refused") {
    check_rejected(R"(
        module m;
            logic [7:0] p;
            int n;
            initial begin
                n = 4;
                p = {n{2'b01}};
            end
        endmodule
    )", "reference to non-constant variable 'n' is not allowed in a constant expression");
}

// Still refused, each for its own reason: a single CHARACTER needs a masked record
// rather than a whole handle, a class property has no per-object shadow at all
// (whether or not the reference is qualified), and intra-assignment timing rides a
// different record pool. A string element of a QUEUE / dynamic / associative array
// is not on the list: LRM 10.4.2 forbids a nonblocking assignment to an element of
// a dynamically sized array, so slang rejects it before lowering.
TEST_CASE("string nonblocking targets that stay refused") {
    SUBCASE("one character") {
        check_rejected(R"(
            module m;
                string s;
                initial begin s = "abc"; s[0] <= "z"; end
            endmodule
        )",
                       "nonblocking assignment to a string");
    }
    SUBCASE("unqualified class property inside a method") {
        check_rejected(R"(
            module m;
                class C;
                    string p;
                    task automatic set(string v);
                        p <= v;
                    endtask
                endclass
                C c;
                initial begin c = new; c.set("x"); end
            endmodule
        )",
                       "nonblocking assignment to a class property");
    }
    SUBCASE("intra-assignment delay") {
        check_rejected(R"(
            module m;
                string s, t;
                initial begin t = "x"; s <= #5 t; end
            endmodule
        )",
                       "intra-assignment timing");
    }
}

TEST_CASE("unsupported string constructs are rejected loudly") {
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
    SUBCASE("case ... inside with a string range bound") {
        check_rejected(R"(
            module m;
                string s;
                initial case (s) inside ["a":"z"]: $display("a"); endcase
            endmodule
        )",
                       "string range bound");
    }
    SUBCASE("string argument to a non-%s specifier") {
        check_rejected(R"(
            module m;
                string s;
                initial $display("%d", s);
            endmodule
        )", "requires %s");
    }
    // LRM 21.3.3 makes $sformat / $sformatf the exception to the literal-format
    // rule, so a runtime format is accepted (see the cases below). What stays
    // rejected is an argument the runtime descriptor cannot carry.
    SUBCASE("aggregate argument against a runtime $sformat format") {
        check_rejected(R"(
            module m;
                typedef struct { int a; int b; } pair_t;
                pair_t p;
                string s, f;
                initial begin f = "%p"; $sformat(s, f, 1, p); end
            endmodule
        )", "$sformat argument 4 is an unpacked struct");
    }
    SUBCASE("queue argument against a runtime $sformatf format") {
        check_rejected(R"(
            module m;
                int    q[$];
                string s, f;
                initial begin f = "%p"; s = $sformatf(f, q); end
            endmodule
        )", "$sformatf argument 2 is a queue");
    }
    SUBCASE("fixed unpacked array argument against a runtime $sformat format") {
        check_rejected(R"(
            module m;
                int    a[3];
                string s, f;
                initial begin f = "%p"; $sformat(s, f, a); end
            endmodule
        )", "$sformat argument 3 is a fixed unpacked array");
    }
    // Only $sformatf reaches Clotho with one: slang's SFormatFunction permits an
    // empty argument past the format, while the $sformat TASK rejects it outright.
    SUBCASE("empty argument against a runtime $sformatf format") {
        check_rejected(R"(
            module m;
                string s, f;
                initial begin f = "x"; s = $sformatf(f, , 1); end
            endmodule
        )", "$sformatf argument 2 is empty");
    }
    // slang admits any canBeStringLike format operand, which includes a byte
    // array -- whose characters live one per element, not packed, so there is no
    // LRM 6.16 conversion to reach for.
    SUBCASE("byte-array runtime $sformat format") {
        check_rejected(R"(
            module m;
                byte   ba[];
                string s;
                initial begin ba = new[3]; $sformat(s, ba, 1); end
            endmodule
        )", "$sformat format is a dynamic array");
    }
}

// -- A $sformat / $sformatf format that is not a string literal (LRM 21.3.3) --
//
// The clause is explicit that these two are the exception: "The format arguments
// to these tasks must be string literals ... The only exception is $sformat,
// whose second argument can be an expression of string data type." $sformatf is
// the same facility returning a string, so it takes the same format.
//
// Every expectation below was cross-checked against iverilog 13 and Verilator
// 5.050, which agree with each other on all of these shapes.

// The blocker that motivated this: Accellera UVM's uvm_phase.svh:1963 builds its
// jump-failure message from a four-piece concatenation of string literals. A
// concatenation is an integral expression of type bit[N] (LRM 5.9), not a string
// literal, but it IS a constant, so it folds to its text at compile time and takes
// the ordinary literal path (specifier validation included).
TEST_CASE("$sformat with a concatenated-literal format (the UVM shape)") {
    CHECK(run_capture(R"(
        module m;
            string s;
            initial begin
                $sformat(s, {"phase %s is neither a predecessor or successor of ",
                             "phase %s or is non-existant, so we cannot jump to it.  ",
                             "Phase control flow is now undefined so the simulation ",
                             "must terminate"}, "run", "main");
                $display("[%s]", s);
            end
        endmodule
    )") == "[phase run is neither a predecessor or successor of phase main or is "
           "non-existant, so we cannot jump to it.  Phase control flow is now "
           "undefined so the simulation must terminate]\n");
}

// The byte-to-text conversion of the folded bit[N] constant is LRM 6.16's: 8-bit
// chunks most significant first, with a zero chunk dropped. "x" + "%0d" + "yz" is
// 6 characters = 48 bits, and each piece on its own is 8 / 24 / 16, so the total
// is a multiple of 8 only by accident of this example -- the interesting case is
// a piece that is not, which the `%%` case below covers at 6 + 6 = 12 bytes with
// a 5-byte middle. Both must come out with no stray NUL and no lost character.
TEST_CASE("$sformat concatenated-literal format: odd lengths and %% escapes") {
    CHECK(run_capture(R"(
        module m;
            string s;
            initial begin
                $sformat(s, {"x", "%0d", "yz"}, 5);
                $display("[%s]", s);
                $sformat(s, {"pct=%%", " v=%0d"}, 3);
                $display("[%s]", s);
                $sformat(s, {"a", "b", "c", "d", "e"});
                $display("[%s]", s);
            end
        endmodule
    )") == "[x5yz]\n[pct=% v=3]\n[abcde]\n");
}

// A `localparam string` format is the same situation as the concatenation: not a
// literal, still constant, so it folds. Here the constant arrives already as a
// string ConstantValue rather than an integral, which is the other half of
// const_format_text.
TEST_CASE("$sformat / $sformatf with a localparam string format") {
    CHECK(run_capture(R"(
        module m;
            localparam string P = "p=%0d%%";
            string s;
            initial begin
                $sformat(s, P, 4);
                $display("[%s]", s);
                $display("[%s]", $sformatf(P, 5));
            end
        endmodule
    )") == "[p=4%]\n[p=5%]\n");
}

// A genuinely runtime format: the string variable is reassigned between the two
// calls, so the same call site renders two different formats. This is what proves
// the format is parsed at run time and not folded -- a folding implementation
// cannot produce two answers from one site.
TEST_CASE("$sformat with a runtime string format is genuinely dynamic") {
    CHECK(run_capture(R"(
        module m;
            string s, f;
            initial begin
                f = "one %0d";
                $sformat(s, f, 1);
                $display("[%s]", s);
                f = "two %0d %0d";
                $sformat(s, f, 2, 3);
                $display("[%s]", s);
                $display("[%s]", $sformatf(f, 4, 5));
            end
        endmodule
    )") == "[one 1]\n[two 2 3]\n[two 4 5]\n");
}

// The oracle-confirmed shapes, verbatim: iverilog 13 and Verilator 5.050 both
// produce exactly these three lines.
TEST_CASE("$sformat / $sformatf runtime format matches iverilog and Verilator") {
    CHECK(run_capture(R"(
        module m;
            string s, f;
            initial begin
                $sformat(s, {"a=%0d ", "b=%0d"}, 1, 2);
                $display("[%s]", s);
                f = "c=%0d";
                $sformat(s, f, 7);
                $display("[%s]", s);
                $display("[%s]", $sformatf(f, 9));
            end
        endmodule
    )") == "[a=1 b=2]\n[c=7]\n[c=9]\n");
}

// Degenerate formats: nothing to render, and nothing that consumes an argument.
// An empty rt_string still hands the renderer a valid NUL-terminated pointer.
TEST_CASE("$sformat runtime format: empty, specifier-free, and %% only") {
    CHECK(run_capture(R"(
        module m;
            string s, f;
            initial begin
                f = "";     $sformat(s, f); $display("[%s]", s);
                f = "plain"; $sformat(s, f); $display("[%s]", s);
                f = "a%%b";  $sformat(s, f); $display("[%s]", s);
                $display("[%s]", $sformatf(f));
            end
        endmodule
    )") == "[]\n[plain]\n[a%b]\n[a%b]\n");
}

// Every conversion family through a runtime format, so the marshalling is
// exercised per argument type: an integral under the integer radices, a string
// object under %s, a character code under %c, a real under the C family, and a
// time under %t. With a constant format the frontend picks each argument's
// conversion and fixes it up in the IR; here the runtime does it from the rt_arg
// flags alone, so the two paths are asserted to produce the same bytes.
TEST_CASE("$sformat runtime format covers every conversion family") {
    CHECK(run_capture(R"(
        module m;
            string s, f, sv;
            real   r;
            initial begin
                sv = "hi";
                r  = 1.5;
                $sformat(s, "%d|%0d|%h|%b|%o|%s|%c", 8'hab, 8'hab, 8'hab, 4'b1010, 8'o17, sv, 65);
                $display("const [%s]", s);
                f = "%d|%0d|%h|%b|%o|%s|%c";
                $sformat(s, f, 8'hab, 8'hab, 8'hab, 4'b1010, 8'o17, sv, 65);
                $display("dyn   [%s]", s);
                $sformat(s, "%e|%f|%g", r, r, r);
                $display("const [%s]", s);
                f = "%e|%f|%g";
                $sformat(s, f, r, r, r);
                $display("dyn   [%s]", s);
                $sformat(s, "%t", 7);
                $display("const [%s]", s);
                f = "%t";
                $sformat(s, f, 7);
                $display("dyn   [%s]", s);
            end
        endmodule
    )") == "const [171|171|ab|1010|017|hi|A]\n"
           "dyn   [171|171|ab|1010|017|hi|A]\n"
           "const [1.500000e+00|1.500000|1.5]\n"
           "dyn   [1.500000e+00|1.500000|1.5]\n"
           "const [                   7]\n"
           "dyn   [                   7]\n");
}

// %t needs the calling module's ticks-per-time-unit, which the frontend applies
// itself for a constant format (scale_to_ticks) and cannot for a runtime one --
// it does not know which argument will meet the %t. The multiplier rides
// rt_fmt_dyn instead. `fine` drags the design's tick down to 1 ps while `m`
// counts in ns, so the multiplier is 1000 rather than the identity, and the real
// case pins the ORDER: scaling before rounding gives 2.500 ns, rounding first
// would quantize to a whole ns and give 3.000.
TEST_CASE("%t through a runtime format scales like the constant path") {
    CHECK(run_capture(R"(
        `timescale 1ps/1ps
        module fine;
            initial #1 ;
        endmodule
        `timescale 1ns/1ns
        module m;
            string s, f;
            real   r;
            fine u();
            initial begin
                $timeformat(-9, 3, " ns", 12);
                r = 2.5;
                $sformat(s, "%t", 7);   $display("[%s]", s);
                $sformat(s, "%t", r);   $display("[%s]", s);
                f = "%t";
                $sformat(s, f, 7);      $display("[%s]", s);
                $sformat(s, f, r);      $display("[%s]", s);
            end
        endmodule
    )") == "[    7.000 ns]\n[    2.500 ns]\n[    7.000 ns]\n[    2.500 ns]\n");
}

// A conversion whose argument type does not match it is reconciled by the runtime
// exactly as build_format reconciles it in the IR: an integral under %e/%f/%g
// converts to real and a real under %d/%h/%b/%o rounds half away from zero into a
// signed 64-bit (LRM 21.2.1.2 -> 6.12.1), so -3.7 prints as -4 and as the 64-bit
// two's complement under %h.
TEST_CASE("runtime format reconciles a mismatched argument like the constant path") {
    CHECK(run_capture(R"(
        module m;
            string  s, f;
            real    r;
            integer i;
            initial begin
                i = -7;
                r = -3.7;
                $sformat(s, "%g|%f", i, i);
                $display("const [%s]", s);
                f = "%g|%f";
                $sformat(s, f, i, i);
                $display("dyn   [%s]", s);
                f = "%0d|%h";
                $sformat(s, f, r, r);
                $display("dyn   [%s]", s);
            end
        endmodule
    )") == "const [-7|-7.000000]\ndyn   [-7|-7.000000]\ndyn   [-4|fffffffffffffffc]\n");
}

// %p of a SINGULAR value is defined as that value "as [it] would [print]
// unformatted" (LRM 21.2.1.6's closing paragraph), which needs no aggregate
// machinery -- so it works through a runtime format even though the frontend
// cannot splice it away. The runtime's renderer must agree with the frontend's
// expand_p_scalar: minimal decimal at the operand's own signedness, %g for a
// real, quotes around a string.
//
// A string LITERAL argument is a packed bit[N] (LRM 5.9), not a string, and with
// a runtime format nothing can tell the renderer otherwise -- so it prints as the
// integral it is. Verilator 5.050 produces exactly this (29041 == 16'h7171);
// iverilog does not implement %p in $sformat at all, so it is not an oracle here.
TEST_CASE("%p of a scalar, a real and a string through a runtime format") {
    CHECK(run_capture(R"(
        module m;
            string s, f, sv;
            real   r;
            initial begin
                sv = "qq";
                r  = 1.5;
                f  = "%p|%p|%p";
                $sformat(s, f, -3, r, sv);
                $display("[%s]", s);
                f = "%0p";
                $display("[%s]", $sformatf(f, sv));
                f = "%p";
                $sformat(s, f, "qq");
                $display("lit [%s]", s);
            end
        endmodule
    )") == "[-3|1.5|\"qq\"]\n[\"qq\"]\nlit [29041]\n");
}

// A wide operand and an x/z one go through the runtime path unchanged: rt_arg
// already carries full planes of any width, so nothing about the marshalling is
// specific to the constant format.
TEST_CASE("runtime format renders wide and x/z operands like the constant path") {
    CHECK(run_capture(R"(
        module m;
            string     s, f;
            reg [95:0] w;
            reg [7:0]  u;
            initial begin
                w = 96'hdeadbeef_cafebabe_12345678;
                u = 8'bxxxx_zzzz;
                $sformat(s, "%h|%0d", w, w);
                $display("const [%s]", s);
                f = "%h|%0d";
                $sformat(s, f, w, w);
                $display("dyn   [%s]", s);
                $sformat(s, "%0d|%h|%b", u, u, u);
                $display("const [%s]", s);
                f = "%0d|%h|%b";
                $sformat(s, f, u, u, u);
                $display("dyn   [%s]", s);
            end
        endmodule
    )") == "const [deadbeefcafebabe12345678|68915718020162848918556923512]\n"
           "dyn   [deadbeefcafebabe12345678|68915718020162848918556923512]\n"
           "const [X|xz|xxxxzzzz]\n"
           "dyn   [X|xz|xxxxzzzz]\n");
}

// A runtime format writing a PACKED destination, not a string object: the other
// rt_sformat entry point, which stores planes rather than assigning a handle.
TEST_CASE("$sformat with a runtime format into a packed destination") {
    CHECK(run_capture(R"(
        module m;
            reg [31:0] d;
            string     f;
            initial begin
                f = "ab%0d";
                $sformat(d, f, 7);
                $display("%h", d);
            end
        endmodule
    )") == "00616237\n");
}

// A runtime format that is a PACKED VECTOR rather than a `string`: slang admits
// any string-like format operand, and the frontend converts one by the same LRM
// 6.16 rule const_format_text applies to a constant, so the same source shape
// renders the same text whether or not it folds.
TEST_CASE("$sformat with a runtime packed-vector format") {
    CHECK(run_capture(R"(
        module m;
            reg [47:0] pf;
            string     s;
            initial begin
                pf = "a=%0d";
                $sformat(s, pf, 6);
                $display("[%s]", s);
            end
        endmodule
    )") == "[a=6]\n");
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

// $psprintf (a nonstandard tool alias for $sformatf, LRM 21.3.3) dispatches
// through the identical lowering -- slang models both via the same
// SFormatFunction class, differing only by the nonstandard-extension warning.
TEST_CASE("$psprintf is an alias for $sformatf") {
    CHECK(run_capture(R"(
        module m;
            string s;
            initial begin
                s = $psprintf("v=%0d h=%h s=%s", 42, 8'hab, "hi");
                $display("[%s] len=%0d", s, s.len());
            end
        endmodule
    )") == "[v=42 h=ab s=hi] len=14\n");
}

// A dynamic `string` value is accepted as a $fopen filename / mode and a $sscanf
// source (LRM 21.3/21.4 allow a string-typed expression). The backend reads its
// rt_string bytes in natural order (spill_ascii_arg forward=1), so the filename
// is not byte-reversed like a packed vector. Round-trips through a real file.
TEST_CASE("dynamic-string $fopen filename/mode and $sscanf source") {
    // generic_string(): the path is spliced into a SystemVerilog string literal,
    // where a Windows path's backslashes would read as escapes.
    const std::string fpath =
      (std::filesystem::temp_directory_path() / (clotho::test::detail::unique_stem() + ".txt"))
        .generic_string();
    const std::string out = run_capture(std::string(R"(
        module m;
            integer fd, rc;
            string  fname, mode, line;
            int     a, b;
            initial begin
                fname = ")") + fpath + R"(";
                mode  = "w";
                fd = $fopen(fname, mode);
                $fwrite(fd, "12 34\n");
                $fclose(fd);
                fd = $fopen(fname, "r");          // dynamic name, literal mode
                rc = $fscanf(fd, "%d %d", a, b);
                $fclose(fd);
                $display("file rc=%0d a=%0d b=%0d", rc, a, b);
                line = "77 88";
                rc = $sscanf(line, "%d %d", a, b); // dynamic-string source
                $display("sscanf rc=%0d a=%0d b=%0d", rc, a, b);
            end
        endmodule
    )");
    CHECK(out == "file rc=2 a=12 b=34\nsscanf rc=2 a=77 b=88\n");
}

// `case` over a string selector. Not sanctioned by LRM 12.5 -- it words matching as
// `===`, which Table 11-1 bars on a string -- but not prohibited either, and read as
// content equality (6.16.9) because that is what `===` degenerates to on an operand
// with no x/z. See lower_case_string and docs/TODO.md. UVM needs it:
// uvm_string_to_verbosity dispatches `+UVM_VERBOSITY=` exactly this way.
//
// No oracle: iverilog 13 aborts compiling a string `case` (draw_eval_vec4
// assertion). Verilator 5.050 agrees with every expectation here.
TEST_CASE("case over a string selector matches on content") {
    const std::string out = run_capture(R"(
        module m;
            string s;
            initial begin
                s = "MEDIUM";
                case (s)
                    "LOW":    $display("1:low");
                    "MEDIUM": $display("1:medium");
                    default:  $display("1:other");
                endcase

                s = "zzz";
                case (s)
                    "LOW":   $display("2:low");
                    default: $display("2:default");
                endcase

                // A literal's NUL bytes are dropped where it becomes a string
                // (6.16), so this label IS "A" and matches -- there is no
                // zero-extension to a common width, which a string has no part in.
                s = "A";
                case (s)
                    "\000A": $display("3:nul-dropped");
                    default: $display("3:default");
                endcase

                s = "";
                case (s)
                    "":      $display("4:empty");
                    default: $display("4:default");
                endcase

                // First match wins (12.5), and a multi-label item is an OR.
                s = "b";
                case (s)
                    "a", "b": $display("5:set");
                    "b":      $display("5:later");
                    default:  $display("5:default");
                endcase

                // A label set through `inside` is the same comparison.
                s = "q";
                case (s) inside
                    "p", "q": $display("6:inside-set");
                    default:  $display("6:default");
                endcase
            end
        endmodule
    )");
    CHECK(out == "1:medium\n2:default\n3:nul-dropped\n4:empty\n5:set\n6:inside-set\n");
}

// A later label -- and any side effect in it -- must not be evaluated once an earlier
// item has matched (12.5's first-match chain), which is why the string path builds
// each predicate in the previous item's else region rather than all of them up front.
TEST_CASE("case over strings does not evaluate a later label after a match") {
    const std::string out = run_capture(R"(
        module m;
            int calls = 0;
            function automatic string tag();
                calls++;
                return "LATER";
            endfunction
            initial begin
                string s = "FIRST";
                case (s)
                    "FIRST": $display("matched first");
                    tag():   $display("matched later");
                    default: $display("default");
                endcase
                $display("calls=%0d", calls);
            end
        endmodule
    )");
    CHECK(out == "matched first\ncalls=0\n");
}

// `unique` keeps its 12.5.3 checks on the string path: the overlap report comes off
// the same per-item predicates the integral and real paths use. Two items match "X"
// here, so the statement runs the first and reports the overlap.
TEST_CASE("unique case over strings reports an overlap") {
    const test::DesignRun run = test::run_design_captured("module m;\n"
                                                          "    initial begin\n"
                                                          "        string s = \"X\";\n"
                                                          "        unique case (s)\n"
                                                          "            \"X\": $display(\"first\");\n"
                                                          "            \"X\": $display(\"second\");\n"
                                                          "        endcase\n"
                                                          "    end\n"
                                                          "endmodule\n");
    CHECK(run.out == "first\n");
    CHECK(run.err.find("unique") != std::string::npos);
}
