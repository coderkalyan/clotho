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

TEST_CASE("unsupported string constructs are rejected loudly") {
    SUBCASE("nonblocking assignment to a string") {
        check_rejected(R"(
            module m;
                string s;
                initial s <= "x";
            endmodule
        )", "nonblocking assignment to a string");
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
