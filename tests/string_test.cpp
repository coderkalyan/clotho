#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors function_test.cpp). String
// literals are used as packed-vector rvalues and read back with $display, so the
// captured hex is the observable value. Expectations cross-checked vs iverilog.
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
