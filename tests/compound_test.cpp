#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module's initial blocks and return everything it wrote to stdout.
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

// Unsupported compound-assignment lvalue shapes must fail loudly, not silently
// mis-lower (never silently accept an unimplemented construct).
TEST_CASE("compound assignment to a concatenation is rejected") {
    const LowerResult r = lower_text(R"(
        module m;
            logic [3:0] a, b;
            initial begin {a, b} += 1; end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find("compound assignment to a concatenation") != std::string::npos,
                  r.diagnostics);
}

TEST_CASE("compound assignment to a string element is rejected") {
    const LowerResult r = lower_text(R"(
        module m;
            string s;
            initial begin s = "hi"; s[0] += 1; end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find("compound assignment to a string") != std::string::npos,
                  r.diagnostics);
}

// An assignment used as an rvalue is not supported yet (LRM 11.3.1); it must
// fail loudly rather than silently drop the store or mis-value the result.
TEST_CASE("assignment used as an expression is rejected") {
    const LowerResult r = lower_text(R"(
        module m;
            int x, y;
            initial begin x = 0; y = (x += 1); end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find("assignment used as an expression") != std::string::npos,
                  r.diagnostics);
}
