// Bit/part-select on a concatenation rvalue (LRM 11.4.12) and an output port
// connected to a concatenation target `.o({a,b})` (LRM 23.3.3). iverilog 13
// cannot parse a select applied directly to a concatenation, so the select
// values were cross-checked against verilator 5.048 (direct form) and against
// iverilog on the temp-wire equivalent (`c = {a,b}; c[7:0]`); both agree. The
// port destructure was verified against iverilog directly.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    return r;
}

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_ok(source);

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

TEST_CASE("bit and part select on a concatenation rvalue") {
    // {a,b} = 1010_0101. Part-selects, a constant bit-select, a runtime
    // bit-select, and the replication form {2{a}} = 1010_1010.
    const std::string out = run_capture(R"(
        module m;
            logic [3:0] a = 4'hA;
            logic [3:0] b = 4'h5;
            logic [1:0] i = 2'd2;
            initial begin
                $display("%b", {a,b}[7:0]);
                $display("%b", {a,b}[3:0]);
                $display("%b", {a,b}[7:4]);
                $display("%b", {a,b}[3]);
                $display("%b", {a,b}[4]);
                $display("%b", {a,b}[7]);
                $display("%b", {a,b}[i]);
                $display("%b", {2{a}}[5]);
                $display("%b", {2{a}}[3:0]);
            end
        endmodule
    )");
    CHECK(out == "10100101\n0101\n1010\n0\n0\n1\n1\n1\n1010\n");
}

TEST_CASE("output port connected to a concatenation target") {
    // sub drives 8'hC3; the parent scatters it MSB-first: x = C, y = 3.
    const std::string out = run_capture(R"(
        module sub(output [7:0] o);
            assign o = 8'hC3;
        endmodule
        module m;
            logic [3:0] x, y;
            sub s(.o({x, y}));
            initial begin
                #1;
                $display("%b %b", x, y);
            end
        endmodule
    )");
    CHECK(out == "1100 0011\n");
}

TEST_CASE("output port to a concatenation of differing widths") {
    // A 12-bit output scattered into a 4-bit and an 8-bit target.
    const std::string out = run_capture(R"(
        module sub(output [11:0] o);
            assign o = 12'hABC;
        endmodule
        module m;
            logic [3:0]  hi;
            logic [7:0]  lo;
            sub s(.o({hi, lo}));
            initial begin
                #1;
                $display("%h %h", hi, lo);
            end
        endmodule
    )");
    CHECK(out == "a bc\n");
}
