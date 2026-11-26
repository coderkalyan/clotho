#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module's initial blocks and return everything it wrote to stdout.
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

// IEEE 1800-2023 11.4.1: `a op= b` is `a = a op b` with the LHS evaluated once.
// Each operator is exercised against a known result (cross-checked vs iverilog).
TEST_CASE("compound assignment applies every arithmetic/bitwise operator") {
    constexpr std::string_view src = R"(
        module m;
            int a;
            initial begin
                a = 10; a += 5;  $display("%0d", a);
                a -= 3;          $display("%0d", a);
                a *= 2;          $display("%0d", a);
                a /= 4;          $display("%0d", a);
                a %= 4;          $display("%0d", a);
                a = 12; a &= 10; $display("%0d", a);
                a |= 1;          $display("%0d", a);
                a ^= 3;          $display("%0d", a);
                a <<= 2;         $display("%0d", a);
                a >>= 1;         $display("%0d", a);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "15\n12\n24\n6\n2\n8\n9\n10\n40\n20\n");
}

// The result truncates to the LHS width, like a plain assignment.
TEST_CASE("compound assignment truncates to the lvalue width") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] x;
            initial begin x = 4'd14; x += 4; $display("%0d", x); end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n");
}

// The LHS (including its index) is evaluated exactly once: `mem[i++] += v`
// bumps i a single time and updates the element i named before the bump.
TEST_CASE("compound assignment evaluates the lvalue once") {
    constexpr std::string_view src = R"(
        module m;
            int mem[0:3];
            int i;
            initial begin
                mem[0] = 1; mem[1] = 2;
                i = 0;
                mem[i++] += 100;
                $display("%0d %0d %0d", i, mem[0], mem[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 101 2\n");
}

// 11.4.2: post-increment yields the old value, pre-increment the new; likewise
// for decrement. The stored value is always bumped by one.
TEST_CASE("pre/post increment and decrement return the right value") {
    constexpr std::string_view src = R"(
        module m;
            int i, r;
            initial begin
                i = 5;
                r = i++; $display("%0d %0d", i, r);
                r = ++i; $display("%0d %0d", i, r);
                r = i--; $display("%0d %0d", i, r);
                r = --i; $display("%0d %0d", i, r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "6 5\n7 7\n6 7\n5 5\n");
}

// A bare `i++;` statement bumps the target and discards the value.
TEST_CASE("increment as an expression statement") {
    constexpr std::string_view src = R"(
        module m;
            int i;
            initial begin i = 0; i++; ++i; $display("%0d", i); end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n");
}

// A compound assignment as a for-step drives the loop.
TEST_CASE("compound assignment as a for-step") {
    constexpr std::string_view src = R"(
        module m;
            int sum;
            initial begin
                sum = 0;
                for (int k = 1; k <= 4; k++) sum += k;
                $display("%0d", sum);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10\n");
}

// `{a, b} op= x` is `{a, b} = {a, b} op x` with the operands evaluated once (LRM
// 11.4.1, 11.4.12): the concatenation reads as one unsigned vector, the result
// truncates to its width, and the leftmost operand takes the most significant
// bits. Expected values cross-checked against iverilog 13 (every operator, carry
// between operands, a signed and a part-selected operand, nesting, and the single
// evaluation of `mw[i++]`).
TEST_CASE("compound assignment to a concatenation applies every operator") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b, c;
            logic signed [3:0] sa;
            logic [7:0] w;
            logic [7:0] mw [0:3];
            int i;
            initial begin
                a = 4'h2; b = 4'h5; {a, b} += 8'd9;   $display("+= %h%h", a, b);
                {a, b} -= 8'd100;                      $display("-= %h%h", a, b);
                a = 4'h1; b = 4'h3; {a, b} *= 8'd5;    $display("*= %h%h", a, b);
                {a, b} /= 8'd4;                        $display("/= %h%h", a, b);
                a = 4'h1; b = 4'h3; {a, b} %= 8'd7;    $display("%%= %h%h", a, b);
                a = 4'hC; b = 4'hA; {a, b} &= 8'h3C;   $display("&= %h%h", a, b);
                {a, b} |= 8'h81;                       $display("|= %h%h", a, b);
                {a, b} ^= 8'hFF;                       $display("^= %h%h", a, b);
                a = 4'h8; b = 4'h1; {a, b} <<= 3;      $display("<<= %h%h", a, b);
                a = 4'h8; b = 4'h1; {a, b} >>= 3;      $display(">>= %h%h", a, b);
                a = 4'h8; b = 4'h1; {a, b} <<<= 1;     $display("<<<= %h%h", a, b);
                a = 4'h8; b = 4'h1; {a, b} >>>= 1;     $display(">>>= %h%h", a, b);
                a = 4'hF; b = 4'hF; {a, b} += 1;       $display("wrap %h%h", a, b);
                a = 4'h0; b = 4'hF; {a, b} += 1;       $display("carry %h%h", a, b);
                a = 4'h1; b = 4'h2; c = 4'h3; {a, b, c} += 12'h0FF; $display("three %h%h%h", a, b, c);
                sa = -4'sd2; b = 4'h0; {sa, b} += 1;   $display("signed %h%h", sa, b);
                sa = 4'sh7; b = 4'hF; {sa, b} >>>= 1;  $display("signed>>> %h%h", sa, b);
                w = 8'h0F; a = 4'h0; {w[7:4], a} += 8'h11; $display("part %h %h", w, a);
                a = 4'h0; b = 4'hF; c = 4'hF; {a, {b, c}} += 1; $display("nested %h%h%h", a, b, c);
                mw[0] = 8'h12; mw[1] = 8'h34; b = 4'h0; i = 0;
                {mw[i++], b} -= 1;                     $display("once %h %h i=%0d", mw[0], b, i);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "+= 2e\n-= ca\n*= 5f\n/= 17\n%= 05\n&= 08\n|= 89\n^= 76\n<<= 08\n>>= 10\n<<<= 02\n>>>= 40\n"
          "wrap 00\ncarry 10\nthree 222\nsigned e1\nsigned>>> 3f\npart 1f 1\nnested 100\nonce 11 f i=1\n");
}

// `++` / `--` on a concatenation are `{a, b} = {a, b} +/- 1` (LRM 11.4.2): the
// post form yields the old value, the pre form the new one, and a compound
// assignment used as a value yields the concatenation after the store (11.3.1).
// No oracle runs these: iverilog 13 rejects `{a, b}++` and an assignment as an
// operand, so the values are worked out from the clauses.
TEST_CASE("increment, decrement and the value of a compound assignment to a concatenation") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            int z;
            initial begin
                a = 4'h1; b = 4'h2; z = ({a, b} += 8'd1); $display("value %0d %h%h", z, a, b);
                a = 4'h0; b = 4'hF; {a, b}++;             $display("inc %h%h", a, b);
                {a, b}--;                                 $display("dec %h%h", a, b);
                a = 4'h0; b = 4'hF; z = {a, b}++;         $display("post %0d %h%h", z, a, b);
                a = 4'h0; b = 4'hF; z = ++{a, b};         $display("pre %0d %h%h", z, a, b);
                a = 4'hF; b = 4'hF; {a, b}++;             $display("wrap %h%h", a, b);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "value 19 13\ninc 10\ndec 0f\npost 15 10\npre 16 10\nwrap 00\n");
}

// `s[i] op= x` is `s[i] = s[i] op x`, a character read being `getc` and a write
// `putc` (LRM 6.16, 11.4.1): an index outside the string, or a byte of zero written,
// leaves the string as it was, and the index expression runs once. iverilog 13
// aborts on a compound assignment to a string element, so the values are worked
// out from 6.16.2 and 6.16.3: 'h' + 1 = 'i', 'l' - 1 = 'k', 'a' ^ 0x20 = 'A'.
TEST_CASE("compound assignment to a string element") {
    constexpr std::string_view src = R"(
        module m;
            string s;
            int i, r;
            initial begin
                s = "hal";
                s[0] += 1;
                s[2] -= 1;
                $display("%s", s);
                i = 1;
                s[i] ^= 8'h20;
                $display("%s %0d", s, i);
                s[5] += 1;
                s[-1] += 1;
                $display("%s", s);
                s = "ab"; i = 0;
                s[i++] += 1;
                $display("%s %0d", s, i);
                s = "a"; r = s[0]++;
                $display("%s %0d", s, r);
                r = ++s[0];
                $display("%s %0d", s, r);
                r = s[0]--;
                $display("%s %0d", s, r);
                s = "\xff"; s[0]++;
                $display("%0d", s[0] & 8'hff);
                s = "a"; s[0] <<= 1;
                $display("%0d", s[0] & 8'hff);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "iak\niAk 1\niAk\nbb 1\nb 97\nc 99\nb 99\n255\n194\n");
}

// An assignment used as an rvalue yields the LHS after the store, truncated to
// the LHS type (LRM 11.3.1). Cross-checked vs Verilator; iverilog rejects the
// construct outright, so it cannot serve as the oracle here.
TEST_CASE("assignment used as an rvalue yields the stored value") {
    // Simple `y = (x = v)` and compound `z = (x += v)` propagate the value.
    CHECK(run_capture(R"(
        module m;
            int x, y, z;
            initial begin
                y = (x = 5);   $display("%0d %0d", x, y);
                x = 10;
                z = (x += 3);  $display("%0d %0d", x, z);
                x = (y = (z = 7)); $display("%0d %0d %0d", x, y, z);
            end
        endmodule
    )") == "5 5\n13 13\n7 7 7\n");
}

// The rvalue is truncated to the LHS type, not the raw (wider) RHS.
TEST_CASE("assignment rvalue truncates to the LHS width") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0] a, b;
            initial begin b = (a = 8'hAB); $display("%h %h", a, b); end
        endmodule
    )") == "b b\n");
}

// A whole-string assignment as an rvalue yields the assigned handle.
TEST_CASE("string assignment used as an rvalue yields the string") {
    CHECK(run_capture(R"(
        module m;
            string s, t;
            initial begin s = (t = "world"); $display("%s %s", s, t); end
        endmodule
    )") == "world world\n");
}
