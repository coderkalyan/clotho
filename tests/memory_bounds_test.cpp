#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors dynamic_array_test.cpp).
// Expectations are cross-checked against iverilog (the LRM 7.4.5 oracle).
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

// Regression: a variable index into a fixed-size unpacked array (a "memory")
// had no bounds guard at all -- an out-of-range write walked off the array's
// flat storage and corrupted adjacent global state (SIGSEGV), and the same
// gap applied to a nonblocking write (which stages a raw destination pointer
// into a deferred record replayed unconditionally in the NBA region). Verified
// vs iverilog 13 (ivtest memidxrng/memport_bs/array_lval_select{1,2}).

TEST_CASE("out-of-range blocking write to a fixed-size unpacked array is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            reg array1[2:1];
            integer index;
            initial begin
                array1[1] = 1'b0;
                array1[2] = 1'b0;
                index = 3;
                array1[index] = 1'b1;
                $display("%b %b", array1[2], array1[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 0\n");
}

TEST_CASE("an unknown (x) index into a fixed-size unpacked array is out of range") {
    constexpr std::string_view src = R"(
        module m;
            reg array2[1:0];
            integer index;
            initial begin
                array2[0] = 1'b0;
                array2[1] = 1'b0;
                index = 'bx;
                array2[index] = 1'b1;
                $display("%b %b", array2[1], array2[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 0\n");
}

TEST_CASE("out-of-range read from a fixed-size unpacked array yields the element default") {
    constexpr std::string_view src = R"(
        module m;
            reg mem[12:2];
            reg [7:0] i;
            initial begin
                for (i = 0; i < 17; i = i + 1) mem[i] = ^i;
                $display("%b %b %b", mem[1], mem[13], mem[7]);
            end
        endmodule
    )";
    // mem[1] / mem[13] are out of [2:12] (default x); mem[7] is in range.
    CHECK(run_capture(src) == "x x 1\n");
}

TEST_CASE("out-of-range nonblocking write to a fixed-size unpacked array is a no-op, "
         "in-bounds writes still land") {
    constexpr std::string_view src = R"(
        module m;
            reg mem[3:0];
            integer i;
            initial begin
                for (i = 0; i < 255; i = i + 1) mem[i] <= 1'b1;
                #1;
                $display("%b %b %b %b", mem[0], mem[1], mem[2], mem[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 1 1\n");
}
