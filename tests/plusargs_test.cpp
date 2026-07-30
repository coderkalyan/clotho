#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

#include <cstdio>

using namespace clotho;

namespace {

// JIT the design with the given plusargs and capture everything it writes to
// stdout. Mirrors the run_capture helper in the other backend tests, plus a
// plusargs list threaded to run_jit.
std::string run_capture(std::string_view source, std::vector<std::string_view> plusargs = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module, nullptr, {}, plusargs);

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

// Confirm lowering fails loudly for an unsupported construct.
std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostic_text();
}

} // namespace

// -- $test$plusargs -----------------------------------------------------

// The LRM §21.6 worked example verbatim: with `+HELLO` on the command line,
// only prefixes of "HELLO" match.
TEST_CASE("$test$plusargs: LRM worked example") {
    const char* src = R"(
        module m;
            initial begin
                if ($test$plusargs("HELLO")) $display("Hello argument found.");
                if ($test$plusargs("HE")) $display("The HE subset string is detected.");
                if ($test$plusargs("H")) $display("Argument starting with H found.");
                if ($test$plusargs("HELLO_HERE")) $display("Long argument.");
                if ($test$plusargs("HI")) $display("Simple greeting.");
                if ($test$plusargs("LO")) $display("Does not match.");
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "HELLO" }) == "Hello argument found.\n"
                                           "The HE subset string is detected.\n"
                                           "Argument starting with H found.\n");
}

TEST_CASE("$test$plusargs: no plusargs given") {
    const char* src = R"(
        module m;
            initial begin
                $display("%0d", $test$plusargs("HELLO"));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0\n");
}

// A runtime (non-literal) query string is a documented gap: loud frontend
// failure, not silent misbehavior.
TEST_CASE("$test$plusargs: non-literal argument is rejected") {
    const std::string diag = lower_error(R"(
        module m;
            string s;
            initial begin
                s = "HELLO";
                void'($test$plusargs(s));
            end
        endmodule
    )");
    CHECK(diag.find("string literal") != std::string::npos);
}

// -- $value$plusargs ------------------------------------------------------

// The LRM §21.6 `goodtasks` example: +TEST=5 is found and converted.
TEST_CASE("$value$plusargs: decimal conversion, found") {
    const char* src = R"(
        module m;
            integer i1;
            initial begin
                if ($value$plusargs("TEST=%d", i1))
                    $display("value was %0d", i1);
                else
                    $display("+TEST= not found");
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "TEST=5" }) == "value was 5\n");
}

// The LRM §21.6 `goodtasks` example with no matching plusarg: not found, and
// the variable is left untouched.
TEST_CASE("$value$plusargs: not found leaves the variable untouched") {
    const char* src = R"(
        module m;
            integer i1;
            initial begin
                i1 = 999;
                if ($value$plusargs("TEST=%d", i1))
                    $display("value was %0d", i1);
                else
                    $display("+TEST= not found, i1=%0d", i1);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "+TEST= not found, i1=999\n");
}

// The LRM §21.6 `ieee1364_example`: +TESTNAME=t1 into a %s target. Shrunk to
// 64 bits from the LRM's 256-bit example target — MVP targets are capped at
// 64 bits, like $fscanf/$sscanf's own %s target.
TEST_CASE("$value$plusargs: %s conversion into a packed-vector target") {
    const char* src = R"(
        module m;
            logic [8*8:1] testname;
            initial begin
                if ($value$plusargs("TESTNAME=%s", testname))
                    $display(" TESTNAME= %s.", testname);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "TESTNAME=t1" }) == " TESTNAME= t1.\n");
}

// Multiple conversions, first-match-in-source-order plusarg wins (LRM: "The
// plusargs present on the command line are searched in the order provided").
TEST_CASE("$value$plusargs: first matching plusarg wins") {
    const char* src = R"(
        module m;
            integer i1;
            initial begin
                void'($value$plusargs("X=%d", i1));
                $display("%0d", i1);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "X=1", "X=2" }) == "1\n");
}

// No remaining string after the matching prefix: LRM says the stored value is
// zero (for a numeric conversion).
TEST_CASE("$value$plusargs: empty remainder stores zero") {
    const char* src = R"(
        module m;
            integer i1;
            initial begin
                void'($value$plusargs("X=%d", i1));
                $display("%0d", i1);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "X=" }) == "0\n");
}

// An illegal character for the conversion writes 'bx to the whole variable
// (LRM §21.6), not a partial/truncated parse like $sscanf.
TEST_CASE("$value$plusargs: illegal character writes 'bx") {
    const char* src = R"(
        module m;
            integer i1;
            initial begin
                void'($value$plusargs("X=%d", i1));
                $display("%0d", i1);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "X=12xz" }) == "x\n");
}

// A negative decimal value.
TEST_CASE("$value$plusargs: negative decimal") {
    const char* src = R"(
        module m;
            integer i1;
            initial begin
                void'($value$plusargs("X=%d", i1));
                $display("%0d", i1);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "X=-42" }) == "-42\n");
}

// %h and %x are synonyms (LRM §21.6).
TEST_CASE("$value$plusargs: hex conversion") {
    const char* src = R"(
        module m;
            integer i1;
            initial begin
                void'($value$plusargs("X=%h", i1));
                $display("%0d", i1);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "X=ff" }) == "255\n");
}

// A target wider than 64 bits: the plusarg value assembles multi-word (%h shift,
// %d multiply-add). Read back through 64-bit part-selects.
TEST_CASE("$value$plusargs into a target wider than 64 bits") {
    const char* src = R"(
        module m;
            logic [127:0] a;
            initial begin
                void'($value$plusargs("H=%h", a));
                $display("%h_%h", a[127:64], a[63:0]);
                void'($value$plusargs("D=%d", a));
                $display("%h_%h", a[127:64], a[63:0]);
                $finish;
            end
        endmodule
    )";
    // H = the 128-bit hex verbatim; D = 2^127 = 0x8000..0000.
    CHECK(run_capture(src, { "H=deadbeefcafebabe0123456789abcdef", "D=170141183460469231731687303715884105728" }) ==
          "deadbeefcafebabe_0123456789abcdef\n"
          "8000000000000000_0000000000000000\n");
}

// A written target wakes anything sensitive to it, like $sscanf's write-back.
TEST_CASE("$value$plusargs wakes waiting processes") {
    const char* src = R"(
        module m;
            integer i1;
            always @(i1) $display("i1 woke %0d", i1);
            initial begin
                #1 void'($value$plusargs("X=%d", i1));
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "X=7" }) == "i1 woke 7\n");
}

// A runtime (non-literal) user_string is a documented gap.
TEST_CASE("$value$plusargs: non-literal user_string is rejected") {
    const std::string diag = lower_error(R"(
        module m;
            string pstring;
            integer i1;
            initial begin
                pstring = "TEST=%d";
                void'($value$plusargs(pstring, i1));
            end
        endmodule
    )");
    CHECK(diag.find("string literal") != std::string::npos);
}

// %e/%f/%g need real support (the same gap as $fscanf/$sscanf's own
// %f/%e/%g) — loud frontend failure.
TEST_CASE("$value$plusargs: real conversion is rejected") {
    const std::string diag = lower_error(R"(
        module m;
            real f;
            initial void'($value$plusargs("FREQ=%f", f));
        endmodule
    )");
    CHECK(diag.find("not supported") != std::string::npos);
}
