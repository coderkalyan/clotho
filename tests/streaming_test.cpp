// Streaming operator (pack/unpack) tests.
// Pack into a packed value, a string, a queue or a dynamic array; unpack from a
// fixed-width source into any of those (LRM §11.4.14).
// Oracle: verilator where it supports the shape (noted per case), otherwise
// slang's constant evaluator -- iverilog 13 has no streaming operators at all.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

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
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
        out.append(buf, n);
    std::fclose(tmp);
    return out;
}

void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
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

// A dynamically sized stream reaching a FIXED packed target: the runtime bit
// count decides whether it fits, and LRM 11.4.14.3's answer for the overflow is
// still open, so this stays refused rather than guessed. (A string / queue /
// dynamic-array target has no such question and works -- see the cases below.)
TEST_CASE("stream dynamic operand into a packed target rejected") {
    check_rejected(R"(
        module t;
            string s = "hello";
            logic [39:0] r;
            initial r = {>> {s}};
        endmodule
    )", "dynamically-sized");
}

TEST_CASE("whole unpacked array as a streaming-concat operand (LRM 11.4.14)") {
    // The array serializes to its leaf bit-concatenation, first element at the
    // MSB; `<<` reverses the block order. Cross-checked against verilator.
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:3];
            logic [31:0] be, le;
            int i;
            initial begin
                for (i = 0; i < 4; i++) a[i] = 8'h10 + i;
                be = {>>{a}};
                le = {<<8{a}};
                $display("%h %h", be, le);
            end
        endmodule
    )") == "10111213 13121110\n");
}

// -- A string destination (LRM 11.4.14: a string is a bit-stream type of 8*len
// bits, so it is byte-granular in both directions) --

// The shape UVM's m_uvm_string_queue_join uses (uvm_misc.svh): streaming a queue
// of strings into a string is a join. Cross-checked against verilator, which
// prints the same "abcde".
TEST_CASE("stream a queue of strings into a string joins them (LRM 11.4.14)") {
    CHECK(run_capture(R"(
        module m;
            string q[$];
            string r;
            initial begin
                q = {q, "ab"};
                q = {q, "cde"};
                r = {>>{q}};
                $display("[%s] %0d", r, r.len());
            end
        endmodule
    )") == "[abcde] 5\n");
}

TEST_CASE("stream a byte queue and a string into a string") {
    CHECK(run_capture(R"(
        module m;
            byte q[$];
            string s = "hi";
            string r;
            initial begin
                q = {q, 8'h61};
                q = {q, 8'h62};
                r = {>>{q}};
                $display("[%s]", r);
                r = {>>{s, q}};
                $display("[%s]", r);
            end
        endmodule
    )") == "[ab]\n[hiab]\n");
}

// A packed operand converts through the 8-bit-chunk rule of LRM 6.16, dropped
// 0x00 chunk included -- the same rule slang's constant evaluator applies, so the
// folded parameter and the runtime assignment must agree. They are checked
// together here because a divergence between them is the failure mode.
TEST_CASE("stream a packed value into a string drops zero bytes") {
    CHECK(run_capture(R"(
        module m;
            localparam string P = {>>{32'h41004243}};
            string s;
            initial begin
                s = {>>{32'h41004243}};
                $display("%0d %0d [%s] [%s]", s.len(), P.len(), s, P);
            end
        endmodule
    )") == "3 3 [ABC] [ABC]\n");
}

// -- A queue / dynamic-array destination --

TEST_CASE("stream a packed value into a queue and a dynamic array") {
    CHECK(run_capture(R"(
        module m;
            byte q[$];
            byte d[];
            int v = 32'h01020304;
            initial begin
                q = {>>{v}};
                d = {>>{v}};
                $display("%0d %h %h %h %h", q.size(), q[0], q[1], q[2], q[3]);
                $display("%0d %h %h", d.size(), d[0], d[3]);
            end
        endmodule
    )") == "4 01 02 03 04\n4 01 04\n");
}

TEST_CASE("stream a string into a byte queue") {
    CHECK(run_capture(R"(
        module m;
            byte q[$];
            string s = "abc";
            initial begin
                q = {>>{s}};
                $display("%0d %h %h %h", q.size(), q[0], q[1], q[2]);
            end
        endmodule
    )") == "3 61 62 63\n");
}

// The source is materialized into a temp before the target is cleared, the same
// rule `q = {q, e}` relies on, so a self-referential stream keeps its elements.
TEST_CASE("stream a queue into itself") {
    CHECK(run_capture(R"(
        module m;
            byte q[$];
            initial begin
                q = {q, 8'h11};
                q = {q, 8'h22};
                q = {>>{q}};
                $display("%0d %h %h", q.size(), q[0], q[1]);
            end
        endmodule
    )") == "2 11 22\n");
}

// -- Unpack: a streaming concatenation as the assignment target (LRM 11.4.14.3) --

// Bits are consumed from the source's left. Cross-checked against verilator for
// the fixed-array, `<<` and unpacked-struct forms.
TEST_CASE("unpack a packed value into a fixed unpacked array") {
    CHECK(run_capture(R"(
        module m;
            byte a[4];
            byte c[4];
            initial begin
                {>>{a}} = 32'hdeadbeef;
                {<<8{c}} = 32'hdeadbeef;
                $display("%h %h %h %h", a[0], a[1], a[2], a[3]);
                $display("%h %h %h %h", c[0], c[1], c[2], c[3]);
            end
        endmodule
    )") == "de ad be ef\nef be ad de\n");
}

TEST_CASE("unpack a packed value into an unpacked struct") {
    CHECK(run_capture(R"(
        typedef struct { byte x; shortint y; } s_t;
        module m;
            s_t v;
            initial begin
                {>>{v}} = 24'h112233;
                $display("%h %h", v.x, v.y);
            end
        endmodule
    )") == "11 2233\n");
}

// LRM 11.4.14.3 is greedy: the first dynamically sized destination takes every
// bit the fixed ones leave, so `h` takes the top byte and the queue the rest.
TEST_CASE("unpack into a queue, a dynamic array and a string") {
    CHECK(run_capture(R"(
        module m;
            byte h;
            byte q[$];
            byte d[];
            string s;
            initial begin
                {>>{q}} = 32'hdeadbeef;
                $display("%0d %h %h", q.size(), q[0], q[3]);
                {>>{h, q}} = 32'hdeadbeef;
                $display("%h %0d %h %h", h, q.size(), q[0], q[2]);
                {>>{d}} = 16'h1234;
                $display("%0d %h %h", d.size(), d[0], d[1]);
                {>>{s}} = 32'h41424344;
                $display("[%s] %0d", s, s.len());
            end
        endmodule
    )") == "4 de ef\nde 3 ad ef\n2 12 34\n[ABCD] 4\n");
}

// -- What stays refused, loudly (each needs a bit-level buffer or an LRM answer
// this implementation does not have) --

TEST_CASE("streaming shapes that need bit-level realignment are rejected") {
    SUBCASE("slice size over a dynamically-sized stream") {
        check_rejected(R"(
            module m;
                byte q[$];
                string s;
                initial s = {<<8{q}};
            endmodule
        )", "slice size on a dynamically-sized streaming concatenation");
    }
    SUBCASE("operand that is not a whole number of bytes into a string") {
        check_rejected(R"(
            module m;
                bit [5:0] q[$];
                string s;
                initial s = {>>{q}};
            endmodule
        )", "not a whole number of bytes");
    }
    SUBCASE("operand that does not divide into whole elements") {
        check_rejected(R"(
            module m;
                byte q[$];
                bit [11:0] v;
                initial q = {>>{v}};
            endmodule
        )", "not a whole number of elements");
    }
    SUBCASE("dynamically-sized source in an unpack") {
        check_rejected(R"(
            module m;
                byte q[$];
                string s = "hi";
                initial {>>{q}} = s;
            endmodule
        )", "dynamically-sized source");
    }
    SUBCASE("container of strings as an unpack destination") {
        check_rejected(R"(
            module m;
                string q[$];
                initial {>>{q}} = 16'h4142;
            endmodule
        )", "not integral or string");
    }
}
