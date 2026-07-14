// Multi-driver net resolution (LRM 6.6.1, wire/tri, no drive strengths): the
// lattice join z ⊑ {0,1} ⊑ x. Whole-net overlapping drivers each stage into a
// private slot and a WireRes resolver folds them into the net. Every expected
// value below was cross-checked against iverilog 13 (a full net-resolution
// oracle) on the same source.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);
    return r;
}

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_ok(source);

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

// -- Runtime resolution values (oracle: iverilog 13) --

TEST_CASE("tri-state bus: z identity, agreement, conflict") {
    const std::string out = run_capture(R"(
        module top;
          logic a_en, b_en;
          logic [3:0] a_val, b_val;
          wire  [3:0] bus;
          assign bus = a_en ? a_val : 4'bz;
          assign bus = b_en ? b_val : 4'bz;
          initial begin
            a_en=0; b_en=0; a_val=4'h5; b_val=4'hA; #1; $display("%b", bus);
            a_en=1; #1;                                  $display("%b", bus);
            a_en=0; b_en=1; #1;                          $display("%b", bus);
            a_en=1; b_en=1; #1;                          $display("%b", bus);
            a_val=4'hC; b_val=4'h8; #1;                  $display("%b", bus);
            $finish;
          end
        endmodule
    )");
    // none=zzzz, a=0101, b=1010, conflict 5vsA=xxxx, C(1100)vs8(1000)=1x00.
    CHECK(out == "zzzz\n0101\n1010\nxxxx\n1x00\n");
}

TEST_CASE("three tri-state drivers fold left through WireRes") {
    const std::string out = run_capture(R"(
        module top;
          logic [1:0] sel;
          logic [7:0] d0, d1, d2;
          wire  [7:0] bus;
          assign bus = (sel==0) ? d0 : 8'bz;
          assign bus = (sel==1) ? d1 : 8'bz;
          assign bus = (sel==2) ? d2 : 8'bz;
          initial begin
            d0=8'h11; d1=8'h22; d2=8'h44;
            sel=0; #1; $display("%h", bus);
            sel=1; #1; $display("%h", bus);
            sel=2; #1; $display("%h", bus);
            sel=3; #1; $display("%h", bus);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "11\n22\n44\nzz\n");
}

TEST_CASE("two definite drivers with no z conflict to x, agree to the value") {
    const std::string out = run_capture(R"(
        module top;
          logic p, q;
          wire  cw;
          assign cw = p;
          assign cw = q;
          initial begin
            p=0; q=0; #1; $display("%b", cw);
            p=1; q=1; #1; $display("%b", cw);
            p=1; q=0; #1; $display("%b", cw);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "0\n1\nx\n");
}

TEST_CASE("two submodule outputs resolve on one parent bus") {
    const std::string out = run_capture(R"(
        module drv(input en, input [3:0] d, output [3:0] o);
          assign o = en ? d : 4'bz;
        endmodule
        module top;
          logic e0, e1;
          logic [3:0] d0, d1;
          wire  [3:0] bus;
          drv u0(.en(e0), .d(d0), .o(bus));
          drv u1(.en(e1), .d(d1), .o(bus));
          initial begin
            d0=4'h3; d1=4'hC;
            e0=0; e1=0; #1; $display("%b", bus);
            e0=1; e1=0; #1; $display("%b", bus);
            e0=0; e1=1; #1; $display("%b", bus);
            e0=1; e1=1; #1; $display("%b", bus);
            $finish;
          end
        endmodule
    )");
    CHECK(out == "zzzz\n0011\n1100\nxxxx\n");
}

TEST_CASE("a single-driver net keeps the plain-store path") {
    // The common case: one driver, no resolver, plain store propagates.
    const std::string out = run_capture(R"(
        module top;
          logic a;
          wire  w;
          assign w = a;
          initial begin a=1; #1; $display("%b", w); $finish; end
        endmodule
    )");
    CHECK(out == "1\n");
}

// -- Loud-failure guards for the shapes V1 does not resolve --

TEST_CASE("a partial-select driver of a resolved net fails loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] a, input [4:0] b);
            wire [7:0] w;
            assign w[3:0] = a;
            assign w[6:2] = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("partial-select driver of a multiply-driven net") != std::string::npos);
}
