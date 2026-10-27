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

// JIT the design and return everything the BACKEND wrote to stderr, resolved
// against the real source so a location can be asserted. The frontend must
// accept the design: this exercises the refusals that only the backend can
// make, because only it knows what storage a destination actually has.
std::string backend_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));

    const int rc = ir::run_jit(*r.module, nullptr, {}, {}, loc_resolver(r));

    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    CHECK(rc < 0); // the design must not have run

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

// -- %s into a `string` object (LRM §21.6) --------------------------------
//
// "%s -- string (no conversion)": the remainder IS the value. A `string` has no
// declared width, so unlike the packed-vector target above nothing is
// right-justified, capped or padded. The three cases the clause distinguishes
// (match, no match, match with an empty remainder) are asserted on both the
// return value and the destination, because the clause pins both.
TEST_CASE("$value$plusargs: %s into a string object") {
    const char* src = R"(
        module m;
            string s;
            initial begin
                s = "untouched";
                if ($value$plusargs("FOO=%s", s))
                    $display("found [%s]", s);
                else
                    $display("absent [%s]", s);
                $finish;
            end
        endmodule
    )";
    SUBCASE("present") {
        CHECK(run_capture(src, { "FOO=bar" }) == "found [bar]\n");
    }
    SUBCASE("absent leaves the string untouched") {
        CHECK(run_capture(src) == "absent [untouched]\n");
    }
    SUBCASE("an empty remainder stores the empty string") {
        // LRM: "If there is no remaining string, the value stored into the
        // variable shall be either a zero or an empty string value." The match
        // still succeeded, so the return value is nonzero.
        CHECK(run_capture(src, { "FOO=" }) == "found []\n");
    }
}

// No width cap: a value longer than the 64 bits a packed-vector target holds
// arrives whole. This is the property that distinguishes the string path from
// the plane path, so it is worth its own case.
TEST_CASE("$value$plusargs: a string destination is not truncated to 64 bits") {
    const char* src = R"(
        module m;
            string s;
            initial begin
                void'($value$plusargs("S=%s", s));
                $display("%s|%0d", s, s.len());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "S=abcdefghijklmnopqrstuvwxyz" }) ==
          "abcdefghijklmnopqrstuvwxyz|26\n");
}

// Uppercase and leading-0 forms are valid spellings of the same conversion
// (LRM §21.6: "uppercase and lowercase as well as leading 0 forms are valid").
TEST_CASE("$value$plusargs: %S and %0s into a string object") {
    const char* src = R"(
        module m;
            string a, b;
            initial begin
                void'($value$plusargs("A=%S", a));
                void'($value$plusargs("B=%0s", b));
                $display("%s %s", a, b);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "A=one", "B=two" }) == "one two\n");
}

// A string destination and an integral one in the same design, so the two write
// paths (rt_value_plusargs_str's string handle, rt_value_plusargs's word planes)
// stay pinned together: neither can be broken without this failing.
TEST_CASE("$value$plusargs: string and integral destinations in one design") {
    const char* src = R"(
        module m;
            string       s;
            integer      i;
            logic [31:0] v;
            initial begin
                void'($value$plusargs("S=%s", s));
                void'($value$plusargs("I=%d", i));
                void'($value$plusargs("V=%h", v));
                $display("%s %0d %h", s, i, v);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "S=hello", "I=-7", "V=deadbeef" }) == "hello -7 deadbeef\n");
}

// The shape UVM depends on: uvm_root.svh:551 selects the test to run with
// exactly this call, so UVM cannot function without it.
TEST_CASE("$value$plusargs: the UVM_TESTNAME shape") {
    const char* src = R"(
        module m;
            string test_name;
            initial begin
                if ($value$plusargs("UVM_TESTNAME=%s", test_name))
                    $display("running %s", test_name);
                else
                    $display("no test named");
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src, { "UVM_TESTNAME=simple_test" }) == "running simple_test\n");
    // uvm_cmdline_report.svh:66 keeps the return value rather than branching on
    // it, so the integer result has to be right too.
    const char* used = R"(
        module m;
            string verb_string;
            int    verb_count;
            initial begin
                verb_count = $value$plusargs("UVM_VERBOSITY=%s", verb_string);
                $display("%0d %s", verb_count, verb_string);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(used, { "UVM_VERBOSITY=UVM_HIGH" }) == "1 UVM_HIGH\n");
}

// Both UVM sites write into a SUBROUTINE-local string, not a module-level one:
// uvm_root.svh's destination is run_test's own `string test_name=""` formal, and
// uvm_cmdline_report.svh's is a function local. That is a different lvalue --
// frame storage rather than a signal slot -- so the module-level cases above do
// not cover it.
TEST_CASE("$value$plusargs: a string destination in a subroutine") {
    SUBCASE("a task formal, as uvm_root::run_test declares it") {
        const char* src = R"(
            module m;
                task automatic t(string test_name = "");
                    if ($value$plusargs("UVM_TESTNAME=%s", test_name))
                        $display("running %s", test_name);
                    else
                        $display("default [%s]", test_name);
                endtask
                initial begin
                    t();
                    $finish;
                end
            endmodule
        )";
        CHECK(run_capture(src, { "UVM_TESTNAME=simple_test" }) == "running simple_test\n");
        CHECK(run_capture(src) == "default []\n");
    }
    SUBCASE("a class property, written from a method") {
        const char* src = R"(
            module m;
                class c;
                    string name;
                    int    got;
                    function void grab();
                        got = $value$plusargs("UVM_TESTNAME=%s", name);
                    endfunction
                endclass
                initial begin
                    static c o = new();
                    o.grab();
                    $display("%0d [%s]", o.got, o.name);
                    $finish;
                end
            endmodule
        )";
        CHECK(run_capture(src, { "UVM_TESTNAME=simple_test" }) == "1 [simple_test]\n");
    }
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

// The other half of the same gap, and the one that used to be SILENT: with
// %e/%f/%g refused above, an integral conversion was the only way to name a real
// destination, and it wrote the parsed bit pattern into the double's storage --
// `+V=12` with %d read back as 0.000000, no diagnostic. Refused now.
TEST_CASE("$value$plusargs: an integral conversion into a real is rejected") {
    for (const char* conv : { "%d", "%o", "%h", "%b", "%s" }) {
        const std::string src = std::string("module m;\n"
                                            "  real f;\n"
                                            "  shortreal g;\n"
                                            "  initial begin\n"
                                            "    void'($value$plusargs(\"V=") +
                                 conv + "\", f));\n  end\nendmodule\n";
        CHECK_MESSAGE(lower_error(src).find("into a real variable is not supported") !=
                        std::string::npos,
                      conv);
    }
    CHECK(lower_error("module m;\n"
                      "  shortreal g;\n"
                      "  initial void'($value$plusargs(\"V=%d\", g));\n"
                      "endmodule\n")
            .find("into a real variable is not supported")
          != std::string::npos);
}

// -- The shape of a backend-stage refusal ---------------------------------
//
// Two destination shapes in this path are still refused, and both refusals are
// made in the BACKEND, because only it knows what storage a destination has.
// That used to render as a bare `llvm backend error: <prose>` with no location
// and no code: nothing to jump to and nothing to filter on. These pin the
// repaired shape -- `file:line:col: error: <message> [<code>]`, then the source
// line with a caret under it, the form Diagnostic.cpp's render gives a frontend
// guard -- and pin it EXACTLY, since the location and the code are the whole
// point.
//
// The plumbing is general: the span is attached by lower_region for whichever
// instruction tripped the guard, so every refusal made while lowering an
// instruction is located, not just these two.
TEST_CASE("a backend refusal carries a source location and a diagnostic code") {
    SUBCASE("a numeric conversion into a string object") {
        // TODO(clotho): LRM §21.6 stores the CONVERTED value, so %d into a
        // `string` is LRM 6.16's integral-to-string assignment of it.
        const std::string err = backend_error("module m;\n"
                                              "    string s;\n"
                                              "    initial begin\n"
                                              "        void'($value$plusargs(\"X=%d\", s));\n"
                                              "    end\n"
                                              "endmodule\n");
        CHECK(err == "source:4:15: error: $value$plusargs %d into a string object is not supported "
                     "[unsupported-construct]\n"
                     "        void'($value$plusargs(\"X=%d\", s));\n"
                     "              ^\n");
    }
    SUBCASE("a part-select target") {
        const std::string err = backend_error("module m;\n"
                                              "    logic [31:0] v;\n"
                                              "    initial begin\n"
                                              "        void'($value$plusargs(\"X=%d\", v[7:0]));\n"
                                              "    end\n"
                                              "endmodule\n");
        CHECK(err == "source:4:15: error: $value$plusargs into a part-select target is not supported "
                     "[unsupported-construct]\n"
                     "        void'($value$plusargs(\"X=%d\", v[7:0]));\n"
                     "              ^\n");
    }
}
