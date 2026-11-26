#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors sysfunc_test.cpp / case_test.cpp).
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

// The LRM 18.16 example: weights 3 / 1 / 4 give probabilities 0.375 / 0.125 /
// 0.5. Bounds, not exact counts — the point is the distribution, and a 8000-draw
// window is far tighter than +/-15%.
TEST_CASE("randcase branch weights set the probabilities (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            int x, i, c1, c2, c3;
            initial begin
                for (i = 0; i < 8000; i++) begin
                    randcase
                        3 : x = 1;
                        1 : x = 2;
                        4 : x = 3;
                    endcase
                    case (x)
                        1: c1++;
                        2: c2++;
                        default: c3++;
                    endcase
                end
                $display("%0d %0d %0d", c1 + c2 + c3, (c1 > 2550 && c1 < 3450),
                         (c2 > 850 && c2 < 1150) && (c3 > 3400 && c3 < 4600));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "8000 1 1\n");
}

// A zero-weight branch is never taken, and when every weight is zero no branch
// runs at all (LRM 18.16). The optional all-zero warning is not emitted.
TEST_CASE("randcase zero weights") {
    constexpr std::string_view src = R"(
        module m;
            int x, i, miss;
            initial begin
                x = 99;
                randcase
                    0 : x = 1;
                    0 : x = 2;
                endcase
                $display("all zero x=%0d", x);
                for (i = 0; i < 200; i++) begin
                    randcase
                        0 : x = 5;
                        7 : x = 6;
                    endcase
                    if (x != 6) miss++;
                end
                $display("zero arm taken %0d times", miss);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "all zero x=99\nzero arm taken 0 times\n");
}

// Weight precision is self-determined and every summand is unsigned (LRM 18.16),
// so a negative weight is reinterpreted, not clamped and not sign-extended:
// `a - b` is -1 in 8 bits, i.e. a weight of 255, and it takes every draw.
// verilator agrees (5.050).
TEST_CASE("randcase weights are unsigned (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            byte a, b;
            int  x;
            initial begin
                a = 0;
                b = 1;
                randcase
                    a - b : x = 1;
                    0     : x = 2;
                endcase
                $display("x=%0d", x);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "x=1\n");
}

// The sum takes the widest weight's precision (LRM 18.16): the 12-bit literal
// makes the 8-bit 255 a plain 255 rather than something that wrapped, so the
// split is 255 : 2048. verilator agrees (5.050).
TEST_CASE("randcase sum precision is the widest weight (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            int x, i, c1;
            initial begin
                for (i = 0; i < 4000; i++) begin
                    randcase
                        8'd255  : x = 1;
                        12'h800 : x = 2;
                    endcase
                    if (x == 1) c1++;
                end
                $display("%0d", c1 > 330 && c1 < 560);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// Corollary of that precision rule: the sum is a fixed-width unsigned addition,
// so it wraps. 8'd200 + 8'd100 sums to 44 in 8 bits, every draw lands under 200,
// and the first branch always wins. This is the literal LRM 18.16 reading
// ("standard addition semantics (maximum precision of all weights)"); verilator
// (5.050) instead widens the accumulator and takes the second branch ~1/3 of the
// time. Clotho keeps the wrapping (docs/LRM_NOTES.md, note 26).
TEST_CASE("randcase sum wraps at the weight precision (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            int x, i, miss;
            initial begin
                for (i = 0; i < 200; i++) begin
                    randcase
                        8'd200 : x = 1;
                        8'd100 : x = 2;
                    endcase
                    if (x != 1) miss++;
                end
                $display("miss=%0d", miss);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "miss=0\n");
}

// The draw comes from the process RNG (LRM 18.16 draws through
// $urandom_range), which lives in the coroutine frame — so a randcase in an
// always block keeps drawing fresh values across activations instead of
// repeating the first one.
TEST_CASE("randcase in an always block draws per activation") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0;
            int x, n, ones;
            initial begin
                repeat (200) #5 clk = ~clk;
                $display("n=%0d mixed=%0d", n, ones > 20 && ones < 80);
                $finish;
            end
            always @(posedge clk) begin
                randcase
                    1 : x = 1;
                    1 : x = 2;
                endcase
                n++;
                if (x == 1) ones++;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=100 mixed=1\n");
}

// randcase draws through $urandom_range, so it works wherever that does -- the
// caller's RNG reaches a subroutine body through the call ABI. The weights here
// leave one branch reachable, so the outcome is determined by the weights and not
// by the draw.
TEST_CASE("randcase in a function and in a task") {
    const std::string src = R"(
        module m;
            function automatic int f();
                randcase
                    0 : return 1;
                    3 : return 2;
                endcase
            endfunction
            task automatic t(output int o);
                randcase
                    2 : o = 7;
                    0 : o = 8;
                endcase
            endtask
            int x;
            initial begin t(x); $display("%0d %0d", f(), x); end
        endmodule
    )";
    CHECK(run_capture(src) == "2 7\n");
}

// A weight wider than 32 bits takes several stitched draws (LRM 18.16: "This can
// result in multiple calls to $urandom_range() to handle numbers greater than 32
// bits"). Two weights of 2^32 sum to 2^33: a single 32-bit draw would always land
// under the first weight, so a 50/50 split shows the high word is drawn too.
// Oracle: the probability rule of 18.16 (weight / sum); iverilog 13 has no
// randcase.
TEST_CASE("randcase weights wider than 32 bits split by their ratio (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            longint unsigned a, b;
            int x, i, c1;
            initial begin
                a = 64'h1_0000_0000;
                b = 64'h1_0000_0000;
                for (i = 0; i < 4000; i++) begin
                    randcase
                        a : x = 1;
                        b : x = 2;
                    endcase
                    if (x == 1) c1++;
                end
                $display("%0d", c1 > 1800 && c1 < 2200);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// 3 : 1 with every weight bit above bit 39: a draw or a sum truncated to 32 bits
// would see 0 and take no branch at all. Oracle: 18.16's probability rule.
TEST_CASE("randcase 3 to 1 at 64 bits (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            longint unsigned a, b;
            int x, i, c1, c2;
            initial begin
                a = 64'd3 << 40;
                b = 64'd1 << 40;
                for (i = 0; i < 4000; i++) begin
                    x = 0;
                    randcase
                        a : x = 1;
                        b : x = 2;
                    endcase
                    if (x == 1) c1++;
                    if (x == 2) c2++;
                end
                $display("%0d %0d", c1 + c2, c1 > 2800 && c1 < 3200);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4000 1\n");
}

// A width that is not a multiple of 32 and one past 64: the draw is masked to the
// sum's bit length, so no bit above it can skew the split. The weights 2^63, 2^63
// and 2^62 sum to 5 * 2^62, which fits 65 bits, and split 0.4 / 0.4 / 0.2.
// The 100-bit pair is 50/50. Oracle: 18.16's probability rule.
TEST_CASE("randcase weights of 65 and 100 bits (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            bit [64:0] a, b, c;
            bit [99:0] p, q;
            int x, i, c1, c2, c3, d1;
            initial begin
                a = 65'd1 << 63;
                b = 65'd1 << 63;
                c = 65'd1 << 62;
                p = 100'd1 << 98;
                q = 100'd1 << 98;
                for (i = 0; i < 4000; i++) begin
                    randcase
                        a : x = 1;
                        b : x = 2;
                        c : x = 3;
                    endcase
                    if (x == 1) c1++;
                    if (x == 2) c2++;
                    if (x == 3) c3++;
                    randcase
                        p : x = 1;
                        q : x = 2;
                    endcase
                    if (x == 1) d1++;
                end
                $display("%0d %0d %0d %0d", c1 > 1400 && c1 < 1800, c2 > 1400 && c2 < 1800,
                         c3 > 650 && c3 < 950, d1 > 1800 && d1 < 2200);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 1 1\n");
}

// 18.16's precision rules carry over to a wide sum: each weight is unsigned, so a
// negative longint is 2^64 - 1 and wins every draw, and the sum wraps at 64 bits
// (2^64 - 1 plus 2 is 1, so the draw is 0 and the first branch always wins), as
// the 8-bit case does (docs/LRM_NOTES.md, note 26). Oracle: 18.16's text; iverilog
// 13 has no randcase.
TEST_CASE("randcase wide weights are unsigned and the sum wraps (LRM 18.16)") {
    constexpr std::string_view src = R"(
        module m;
            longint          s;
            longint unsigned a, b;
            int              x, i, miss, wrap_miss;
            initial begin
                s = -1;
                a = 64'hFFFF_FFFF_FFFF_FFFF;
                b = 2;
                for (i = 0; i < 200; i++) begin
                    randcase
                        s : x = 1;
                        0 : x = 2;
                    endcase
                    if (x != 1) miss++;
                    randcase
                        a : x = 1;
                        b : x = 2;
                    endcase
                    if (x != 1) wrap_miss++;
                end
                $display("%0d %0d", miss, wrap_miss);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 0\n");
}

// Zero total at 64 bits: no branch runs and, as for a narrow sum, no draw is taken
// from the process stream, so the value after the randcase is the one without it.
// Oracle: 18.16 ("no branch is taken"); the draw count is the contract of
// docs/design/randsequence.md section 12.
TEST_CASE("randcase with an all-zero wide sum takes no branch and no draw") {
    constexpr std::string_view src = R"(
        module m;
            longint unsigned z;
            int              x, r0, r1, r2;
            initial begin
                z  = 0;
                x  = 99;
                r0 = $urandom(7);
                r1 = $urandom;
                r0 = $urandom(7);
                randcase
                    z : x = 1;
                    z : x = 2;
                endcase
                r2 = $urandom;
                $display("x=%0d same=%0d", x, r1 == r2);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "x=99 same=1\n");
}

// A sum of at most 32 bits takes exactly one draw, the same one $urandom_range
// (sum - 1, 0) takes: the wide path must not move a narrow randcase's stream
// (docs/design/randsequence.md section 12). The randcase here picks branch 2 or
// 1 from the same draw as the range call, so both leave the stream in one state.
TEST_CASE("randcase over a narrow sum draws once") {
    constexpr std::string_view src = R"(
        module m;
            int x, r0, r1, r2, t;
            initial begin
                r0 = $urandom(11);
                randcase
                    8'd3 : x = 1;
                    8'd5 : x = 2;
                endcase
                r1 = $urandom;
                r0 = $urandom(11);
                t  = $urandom_range(7, 0);
                r2 = $urandom;
                $display("%0d %0d", r1 == r2, x == (t < 3 ? 1 : 2));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}
