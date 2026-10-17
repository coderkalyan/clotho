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

// Captures stdout and stderr separately: severity reports ($fatal and friends)
// go to stderr, like $display's own banner, so a runtime (not lowering-time)
// LRM violation -- e.g. a dynamically-sized stream too wide for its fixed
// target (LRM 11.4.14) -- shows up there rather than in `out`.
struct Captured {
    std::string out;
    std::string err;
};

Captured run_capture_err(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* out_tmp = std::tmpfile();
    std::FILE* err_tmp = std::tmpfile();
    REQUIRE(out_tmp != nullptr);
    REQUIRE(err_tmp != nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(out_tmp), fileno(stdout));
    dup2(fileno(err_tmp), fileno(stderr));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    std::rewind(out_tmp);
    std::rewind(err_tmp);
    const auto drain = [](std::FILE* f) {
        std::string s;
        char        buf[256];
        size_t      n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            s.append(buf, n);
        std::fclose(f);
        return s;
    };
    return { drain(out_tmp), drain(err_tmp) };
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
// Clotho follows the rules (docs/LRM_NOTES.md, note 16).
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

// A dynamically sized stream reaching a FIXED packed target (LRM 11.4.14):
// the runtime bit count decides fit/pad/overflow, so the stream is walked bit
// by bit (stream_to_fixed in Lower.cpp) rather than concatenated at compile
// time. `s` is 5 bytes = 40 bits, exactly filling the 40-bit target, so this
// case does not exercise padding -- see "narrower/wider/exact" below for that.
TEST_CASE("stream a dynamically-sized string operand into a packed target (LRM 11.4.14)") {
    CHECK(run_capture(R"(
        module t;
            string s = "hello";
            logic [39:0] r;
            initial begin
                r = {>> {s}};
                $display("%h", r);
            end
        endmodule
    )") == "68656c6c6f\n");
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

// -- A dynamically sized stream reaching a fixed packed target (LRM 11.4.14,
// 11.4.14.2, 11.4.14.3): stream_to_fixed walks the stream bit by bit since the
// width is a run-time value, folding in both `<<` block reversal and the
// left-align-into-target placement per bit. --

// The ticket repro, cross-checked against Verilator 5.050 for `rev` only.
// `a` is 8 bits `0,1,0,1,0,1,0,1` (a[i] = i[0]); the generic stream (LRM
// 11.4.14.1, foreach order, a[0] first/MSB) is 8'b0101_0101 = 0x55 either way
// -- `>>` never reorders (11.4.14.2), and `<<`'s default 1-bit slice is a full
// bit reversal, 8'b1010_1010 = 0xAA.
//
// LRM 11.4.14 (assignment target, non-streaming-concat case): "the stream ...
// shall first be widened ... to left-align it in the target ... filling with
// zero bits on the right" -- unconditionally, not just for `<<`. Clotho's own
// already-tested FIXED-size lower_streaming_concat (Lower.cpp) implements
// exactly that (concat the stream with a zero pad, then resize), regardless of
// direction, and `rev` below matches it: 0xAA -> 0xAA000000.
//
// Verilator matches that for `rev` (0xaa000000) but NOT for `fwd`: it prints
// `55` (0x00000055, right-justified) instead of the LRM's 0x55000000. The
// `>>` case is the only difference from `rev` -- Verilator does not reorder
// either way, so this looks like Verilator specifically treating a `>>`
// (no-op reorder) stream as an ordinary numeric RHS instead of running it
// through the streaming assignment's own left-align rule. Clotho follows the
// LRM text and its own precedent instead of Verilator here.
TEST_CASE("stream a dynamically-sized bit array and byte queue into a packed target (LRM 11.4.14)") {
    CHECK(run_capture(R"(
        module t;
            bit a[]; int v; byte q[$];
            initial begin
                a = new [8]; for (int i=0;i<8;i++) a[i] = i[0];
                v = { << bit { a }};                 $display("rev=%0h", v);
                v = { >> bit { a }};                 $display("fwd=%0h", v);
                q = {8'h12, 8'h34};
                v = { >> byte { q }};                $display("q=%0h", v);
            end
        endmodule
    )") == "rev=aa000000\nfwd=55000000\nq=12340000\n");
}

// Narrower (padded, above), exact fit (no padding), and wider (run-time error,
// below) -- the three width relations LRM 11.4.14 distinguishes.
TEST_CASE("stream a dynamically-sized queue that exactly fills its fixed target") {
    CHECK(run_capture(R"(
        module t;
            byte q[$]; int v;
            initial begin
                q = {8'h11, 8'h22, 8'h33, 8'h44};
                v = { >> { q }};
                $display("%h", v);
            end
        endmodule
    )") == "11223344\n");
}

// LRM 11.4.14: "If the target represents a fixed-size variable that is
// narrower [...] than the stream, an error shall be generated" -- the width is
// a run-time value here, so the check (and the report) happen at run time
// (stream_to_fixed's check_stream_fits_fixed) rather than as a lowering
// rejection. Verilator instead truncates silently to the low 32 bits (no
// diagnostic at all) for this exact case; the LRM's own worked example (`int j
// = {>>{a,b,c}};` with a,b,c a 32-bit `int` each, "error: j is 32 bits < 96
// bits") is unambiguous, so Clotho reports rather than matching Verilator.
TEST_CASE("stream a dynamically-sized bit array wider than its fixed target fatals (LRM 11.4.14)") {
    const Captured c = run_capture_err(R"(
        module t;
            bit a[]; int v;
            initial begin
                a = new [40]; for (int i=0;i<40;i++) a[i] = i[0];
                v = { >> bit { a }};
                $display("unreachable: %0h", v);
            end
        endmodule
    )");
    CHECK(c.err.find("wider than its fixed-size target (LRM 11.4.14)") != std::string::npos);
    CHECK(c.out.find("unreachable") == std::string::npos);
}

// LRM 11.4.14 gives no special case for a zero-length operand: a 0-bit stream
// is simply narrower than any non-zero fixed target, so it is padded with
// zero on the right the same as any other narrower stream -- an all-zero
// result either direction. Cross-checked against Verilator.
TEST_CASE("stream an empty dynamically-sized array into a fixed target") {
    CHECK(run_capture(R"(
        module t;
            bit a[]; int v;
            initial begin
                a = new [0];
                v = { << bit { a }};   $display("rev=%0h", v);
                v = { >> bit { a }};   $display("fwd=%0h", v);
            end
        endmodule
    )") == "rev=0\nfwd=0\n");
}

// `{<<3{a}}` over the same 8-bit `a`: slice size 3 does not divide 8, so the
// last (leftmost) block is the 2-bit remainder (LRM 11.4.14.2, "no padding or
// truncation"). Derived independently three ways -- Clotho's own position
// formula, a bit-index re-derivation cross-checked against the LRM's own
// {<<4{6'b110101}}->6'b010111 example (also one of this file's existing fixed-
// size tests, "stream << non-power-of-two slice"), and direct string
// manipulation of the blocks -- all agreeing on 0xA9 (0xA9000000 padded).
// Verilator 5.050 instead gives 0xa8800000 for this case; since three
// independent derivations agree with each other and with Clotho's own
// existing, already-tested fixed-size block-reversal behavior for an
// analogous (W=6, S=4) partial-block case, Verilator looks wrong here rather
// than Clotho, so this is pinned to the derived value, not the oracle.
TEST_CASE("stream << with a slice size over a dynamically-sized array, partial final block (LRM 11.4.14.2)") {
    CHECK(run_capture(R"(
        module t;
            bit a[]; int v;
            initial begin
                a = new [8]; for (int i=0;i<8;i++) a[i] = i[0];
                v = { << 3 { a }};
                $display("%0h", v);
            end
        endmodule
    )") == "a9000000\n");
}

// A slice size equal to the whole (dynamic) stream width is exactly one
// block, so `<<` cannot reorder anything -- same value as the no-reorder `>>`
// case above.
TEST_CASE("stream << with a slice size equal to the whole dynamically-sized stream is a no-op reorder") {
    CHECK(run_capture(R"(
        module t;
            bit a[]; int v;
            initial begin
                a = new [8]; for (int i=0;i<8;i++) a[i] = i[0];
                v = { << 8 { a }};
                $display("%0h", v);
            end
        endmodule
    )") == "55000000\n");
}

// -- Operand shapes a dynamically-sized stream can be built from (LRM 11.4.14
// / 11.4.14.1) --

// A dynamic array of a packed struct: each element serializes to its own
// packed bits (LRM 11.4.14.1's bit-stream-cast case, not the unpacked-array
// recursion), so the two elements contribute 0x1122 and 0x3344 whole.
TEST_CASE("stream a dynamic array of a packed struct into a fixed target") {
    CHECK(run_capture(R"(
        typedef struct packed { byte hi; byte lo; } pair_t;
        module t;
            pair_t p[]; longint v;
            initial begin
                p = new[2];
                p[0] = '{hi:8'h11, lo:8'h22};
                p[1] = '{hi:8'h33, lo:8'h44};
                v = { >> { p }};
                $display("%h", v);
            end
        endmodule
    )") == "1122334400000000\n");
}

// A dynamic array nested inside an otherwise-fixed struct: the struct itself
// is not fixed size (its `payload` member is dynamic), so the whole operand
// takes collect_agg_stream_chunks's recursive leaf walk instead of the single-
// chunk serialize_agg_lvalue path a truly fixed aggregate operand uses (see
// "stream a dynamic array of a packed struct" above, and the pre-existing
// "whole unpacked array as a streaming-concat operand" test, for that path).
// `hdr` (8 bits, 0xAB) then `payload` (4 bits, 0,1,0,1 -> 0x5) = 12 bits =
// 0xAB5, left-aligned into the 64-bit target.
TEST_CASE("stream a dynamic array nested inside a fixed aggregate into a fixed target") {
    CHECK(run_capture(R"(
        typedef struct { byte hdr; bit payload[]; } pkt_t;
        module t;
            pkt_t pkt; longint v;
            initial begin
                pkt.hdr = 8'hAB;
                pkt.payload = new[4];
                foreach (pkt.payload[i]) pkt.payload[i] = i[0];
                v = { >> { pkt }};
                $display("%h", v);
            end
        endmodule
    )") == "ab50000000000000\n");
}

// Two dynamically-sized operands concatenated: `a` (4 bits, 0101) then `q`
// (16 bits, 0xABCD) = 20 bits = 0x5ABCD, left-aligned into the 32-bit target.
TEST_CASE("stream two dynamically-sized operands into one fixed target") {
    CHECK(run_capture(R"(
        module t;
            bit a[]; byte q[$]; int v;
            initial begin
                a = new[4]; foreach (a[i]) a[i] = i[0];
                q = {8'hAB, 8'hCD};
                v = { >> { a, q }};
                $display("%h", v);
            end
        endmodule
    )") == "5abcd000\n");
}

// A dynamically-sized operand mixed with a fixed one: `fx` (8 bits, 0xCD) then
// `a` (4 bits, 0101) = 12 bits = 0xCD5, left-aligned into the 32-bit target.
TEST_CASE("stream a dynamically-sized operand mixed with a fixed operand into a fixed target") {
    CHECK(run_capture(R"(
        module t;
            bit a[]; logic [7:0] fx; int v;
            initial begin
                a = new[4]; foreach (a[i]) a[i] = i[0];
                fx = 8'hCD;
                v = { >> { fx, a }};
                $display("%h", v);
            end
        endmodule
    )") == "cd500000\n");
}

// LRM 6.24.1 lists what a bit-stream type is (integral, packed, or an
// unpacked array/struct of bit-stream types) and does not include an
// associative array -- an unbounded key -> value map has no ordered bit
// representation at all -- so it is illegal as a stream operand, at the top
// level or buried in an aggregate, independent of size.
TEST_CASE("associative-array operand is rejected as not a bit-stream type (LRM 6.24.1)") {
    SUBCASE("top-level operand") {
        check_rejected(R"(
            module t;
                int a[string]; longint v;
                initial v = { >> { a }};
            endmodule
        )", "not a bit-stream type");
    }
    SUBCASE("aggregate member") {
        check_rejected(R"(
            typedef struct { int a[string]; byte b; } bad_t;
            module t;
                bad_t x; longint v;
                initial v = { >> { x }};
            endmodule
        )", "not a bit-stream type");
    }
}

// The `do_bus_access` shape (uvm_reg_map.svh: `{<<8{ bit_q_t'({<<{p}}) }}`
// where `p` is a byte queue, assigned to another queue) is slice size on a
// dynamically-sized stream feeding a QUEUE destination, not a fixed one --
// stream_to_fixed's per-bit position map (above) does not reach this path,
// since collect_stream_chunks only passes dyn_slice_ok for the fixed-target
// caller. Reordering into a growable string/queue/dynamic-array destination
// would need its own materialize-and-reindex mechanism (each of those three
// consumes the stream one granule at a time, never holding the whole thing),
// which is a separate, comparably-sized change this ticket does not make;
// do_bus_access is UVM's register layer, never reached by easyUVM. This is
// the pre-existing "slice size over a dynamically-sized stream" SUBCASE below
// with a queue destination in place of a string one, confirming the same
// refusal covers it.
TEST_CASE("slice size on a dynamically-sized stream into a queue destination stays refused (do_bus_access's shape)") {
    check_rejected(R"(
        module m;
            byte p[$]; byte bits[$];
            initial bits = {<<8{p}};
        endmodule
    )", "slice size on a dynamically-sized streaming concatenation");
}
