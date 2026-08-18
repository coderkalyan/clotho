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

// Confirm lowering fails loudly for an unsupported construct.
std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostic_text();
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
// time.
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

// A sum wider than 32 bits would need several stitched $urandom_range draws
// (LRM 18.16); guarded loud instead of silently truncating the weights.
TEST_CASE("randcase with a 64-bit weight fails loudly") {
    CHECK(lower_error("module m; longint w; int x;\n"
                      " initial begin w = 5; randcase w : x = 1; 1 : x = 2; endcase end\n"
                      "endmodule")
            .find("32 bits") != std::string::npos);
}
