#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// Unbased unsized literals '0/'1/'x/'z (LRM 5.7.1): the single bit fills the
// whole context-determined width. Expectations cross-checked against Icarus
// Verilog.

namespace {

// JIT-run a module and capture stdout (mirrors display_test.cpp).
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

// Each literal fills the target's width (not a zero-extended single bit), and
// 'x / 'z occupy distinct planes.
TEST_CASE("unbased unsized literals fill the assignment width") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0] a1, a0, ax, az;
            initial begin
                a1 = '1; a0 = '0; ax = 'x; az = 'z;
                $display("%b %b %b %b", a1, a0, ax, az);
            end
        endmodule
    )") == "1111 0000 xxxx zzzz\n");
}

// Wide (>64-bit) target exercises svint_to_ref's general packed path: '1 must
// fill all 96 bits, not truncate.
TEST_CASE("unbased unsized '1 fills a wide vector") {
    CHECK(run_capture(R"(
        module m;
            logic [95:0] w;
            initial begin
                w = '1;
                $display("%h", w);
            end
        endmodule
    )") == "ffffffffffffffffffffffff\n");
}

// The width is taken from the surrounding operator/context, not self-determined.
TEST_CASE("unbased unsized literals take their width from context") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] x;
            initial begin
                x = 8'h0f;
                $display("%h %h", x & '1, x | '0);
            end
        endmodule
    )") == "0f 0f\n");
}

// Signed context: '1 fills every bit, so a signed int reads -1 (LRM 5.7.1 with
// the sign of the context type).
TEST_CASE("unbased unsized '1 in a signed context is all-ones") {
    CHECK(run_capture(R"(
        module m;
            int s;
            initial begin
                s = '1;
                $display("%0d", s);
            end
        endmodule
    )") == "-1\n");
}

// Comparison context: the literal sizes to the other operand. 4'b1010 != '1,
// and a definite value !== 'x.
TEST_CASE("unbased unsized literals in comparison contexts") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0] v;
            initial begin
                v = 4'b1010;
                $display("%b %b", v == '1, v === 'x);
            end
        endmodule
    )") == "0 0\n");
}
