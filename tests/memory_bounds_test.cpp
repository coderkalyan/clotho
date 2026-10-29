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

// LRM 7.4.5: an index is invalid when it is out of bounds, and an invalid one
// reads the Table 7-1 default (x for a 4-state element, 0 for a 2-state one)
// and writes nothing. What is compared is the index VALUE, at its own width and
// signedness: a 2-bit signed -1 is -1, not 3, and 2**32 + 1 is not 1. Oracle:
// the LRM text; iverilog agrees on the signed index and truncates the 64-bit
// one to 32 bits, which the clause does not allow.
TEST_CASE("a signed narrow or 64-bit index into a fixed-size array is compared at its own width") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a [0:3];
            bit   [7:0] b [0:3];
            logic [7:0] t [0:1];
            bit   [7:0] bt [0:1];
            logic signed [1:0] s2;
            longint unsigned   lu;
            longint            ls;
            initial begin
                a = '{8'h10, 8'h11, 8'h12, 8'h13};
                b = '{8'h10, 8'h11, 8'h12, 8'h13};
                s2 = -1;
                $display("%h %h", a[s2], b[s2]);
                a[s2] = 8'hee; b[s2] = 8'hee;
                $display("%h %h %h %h", a[3], b[3], a[0], b[0]);
                a[s2] <= 8'hee; b[s2] <= 8'hee;
                #1 $display("%h %h %h %h", a[3], b[3], a[0], b[0]);
                lu = 64'h1_0000_0001; ls = -64'sd4294967295; // both are 1 modulo 2**32
                $display("%h %h %h %h", a[lu], b[lu], a[ls], b[ls]);
                a[lu] = 8'hee; b[lu] = 8'hee; a[ls] = 8'hdd; b[ls] = 8'hdd;
                $display("%h %h", a[1], b[1]);
                a[lu] <= 8'hee; b[ls] <= 8'hdd;
                #1 $display("%h %h", a[1], b[1]);
                // The same for the base of an indexed slice: not positions 1 and 2.
                t = a[lu +: 2]; bt = b[lu +: 2];
                $display("%h %h %h %h", t[0], t[1], bt[0], bt[1]);
                a[lu +: 2] = '{8'hee, 8'hdd};
                $display("%h %h %h %h", a[0], a[1], a[2], a[3]);
                s2 = 1; lu = 2; ls = 3; // in range: still addressed
                $display("%h %h %h %h", a[s2], b[lu], a[ls], b[s2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "xx 00\n"
          "13 13 10 10\n"
          "13 13 10 10\n"
          "xx 00 xx 00\n"
          "11 11\n"
          "11 11\n"
          "xx xx 00 00\n"
          "10 11 12 13\n"
          "11 12 13 11\n");
}
