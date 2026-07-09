// Streaming operator (pack/unpack) tests.
// RHS-only for now; fixed-width operands per LRM §11.4.14.
// Oracle: slang constant evaluator (iverilog 13 does not support streaming operators).

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

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
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
        out.append(buf, n);
    std::fclose(tmp);
    return out;
}

void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
}

} // namespace

// `>>` (sliceSize==0): no reorder — same as plain concatenation.
TEST_CASE("stream >> no-reorder") {
    const std::string out = run_capture(R"(
        module t;
            logic [7:0] a = 8'hAB, b = 8'hCD;
            logic [15:0] r;
            initial begin
                r = {>> {a, b}};
                $display("%h", r);
            end
        endmodule
    )");
    // {>> {a, b}} == {a, b} == 16'hABCD
    CHECK(out == "abcd\n");
}

// `<<` with 1-bit slices: full bit reversal (default slice size).
TEST_CASE("stream << bit-reverse") {
    const std::string out = run_capture(R"(
        module t;
            logic [7:0] v = 8'b1010_1011;
            logic [7:0] r;
            initial begin
                r = {<< {v}};
                $display("%b", r);
            end
        endmodule
    )");
    // LRM §11.4.14.2: {<<{8'b0011_0101}} → 'b1010_1100; same algorithm, different input.
    // Bit reversal of 10101011 = 11010101
    CHECK(out == "11010101\n");
}

// `<<8`: byte-swap two 8-bit operands.
TEST_CASE("stream << byte-swap two operands") {
    const std::string out = run_capture(R"(
        module t;
            logic [7:0] a = 8'hAB, b = 8'hCD;
            logic [15:0] r;
            initial begin
                r = {<<8 {a, b}};
                $display("%h", r);
            end
        endmodule
    )");
    // generic stream {AB,CD}=16'hABCD; reverse 8-bit blocks → {CD,AB}=16'hCDAB
    CHECK(out == "cdab\n");
}

// `<<8` single operand: byte-swap a 16-bit word.
TEST_CASE("stream << byte-swap 16-bit word") {
    const std::string out = run_capture(R"(
        module t;
            logic [15:0] v = 16'hABCD;
            logic [15:0] r;
            initial begin
                r = {<<8 {v}};
                $display("%h", r);
            end
        endmodule
    )");
    // blocks: [CD, AB] → reversed → {CD, AB} placed MSB-first = 16'hCDAB
    CHECK(out == "cdab\n");
}

// `<<4`: swap nibbles within a byte (4-bit blocks).
TEST_CASE("stream << nibble-swap") {
    const std::string out = run_capture(R"(
        module t;
            logic [7:0] v = 8'hAB;
            logic [7:0] r;
            initial begin
                r = {<<4 {v}};
                $display("%h", r);
            end
        endmodule
    )");
    // 8'hAB → blocks: [B, A] → reversed (B first) = 8'hBA
    CHECK(out == "ba\n");
}

// `<<16`: swap two 16-bit halves of a 32-bit word.
TEST_CASE("stream << 32-bit word half-swap") {
    const std::string out = run_capture(R"(
        module t;
            logic [31:0] v = 32'hDEADBEEF;
            logic [31:0] r;
            initial begin
                r = {<<16 {v}};
                $display("%h", r);
            end
        endmodule
    )");
    // blocks: [BEEF, DEAD] → reversed → {BEEF, DEAD} MSB-first = 32'hBEEFDEAD
    CHECK(out == "beefdead\n");
}

// Non-power-of-two slice size with a width not evenly divisible.
// LRM §11.4.14.2: last (leftmost) block may be smaller.
TEST_CASE("stream << non-power-of-two slice") {
    const std::string out = run_capture(R"(
        module t;
            logic [5:0] v = 6'b11_0101;
            logic [5:0] r;
            initial begin
                r = {<<4 {v}};
                $display("%b", r);
            end
        endmodule
    )");
    // LRM §11.4.14.2 example: {<<4{6'b110101}} → 'b0101_11
    // block[0]=bits[3:0]=0101, block[1]=bits[5:4]=11 → {0101,11}=6'b010111
    CHECK(out == "010111\n");
}

// Multiple operands of different widths.
TEST_CASE("stream << multi-width operands") {
    const std::string out = run_capture(R"(
        module t;
            logic [15:0] a = 16'h1234;
            logic [7:0]  b = 8'h56;
            logic [23:0] r;
            initial begin
                r = {<<8 {a, b}};
                $display("%h", r);
            end
        endmodule
    )");
    // generic stream {a,b} = 24'h123456
    // 8-bit blocks from right: [56, 34, 12] → reversed → {56,34,12} MSB-first = 24'h563412
    CHECK(out == "563412\n");
}

// Nested streaming.
// NOTE: LRM §11.4.14.2 lists {<<2{{<<4{4'b1101}}}} as generating 'b1110, but
// all other LRM examples match, slang's constant evaluator gives 'b0111, and
// manual derivation gives 'b0111 — this appears to be a typo in the LRM.
TEST_CASE("stream << nested") {
    const std::string out = run_capture(R"(
        module t;
            logic [3:0] v = 4'b1101;
            logic [3:0] r;
            initial begin
                r = {<<2 { {<<4 {v}} }};
                $display("%b", r);
            end
        endmodule
    )");
    // inner {<<4{v}}: single 4-bit block, no reorder → 4'b1101
    // outer {<<2{4'b1101}}: block[0]=bits[1:0]=01, block[1]=bits[3:2]=11
    //   → {01,11} MSB-first = 4'b0111
    CHECK(out == "0111\n");
}

// `>>` with explicit (but ignored) slice size.
TEST_CASE("stream >> slice size ignored") {
    const std::string out = run_capture(R"(
        module t;
            logic [7:0] v = 8'hAB;
            logic [7:0] r;
            initial begin
                r = {>>8 {v}};   // slice size ignored for >>
                $display("%h", r);
            end
        endmodule
    )");
    CHECK(out == "ab\n");
}

// X/Z propagation: x in any operand bit propagates through the reorder.
TEST_CASE("stream << x propagation") {
    const std::string out = run_capture(R"(
        module t;
            logic [3:0] v = 4'bxx10;
            logic [3:0] r;
            initial begin
                r = {<<2 {v}};
                $display("%b", r);
            end
        endmodule
    )");
    // block[0]=bits[1:0]=10, block[1]=bits[3:2]=xx
    // reversed: {10, xx} MSB-first = 4'b10xx
    CHECK(out == "10xx\n");
}

// Dynamically-sized operands (string) should fail loudly.
TEST_CASE("stream dynamic operand rejected") {
    check_rejected(R"(
        module t;
            string s = "hello";
            logic [39:0] r;
            initial r = {>> {s}};
        endmodule
    )", "dynamically-sized");
}
