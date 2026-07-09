#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

// Packed struct member access (LRM 7.2.1). Field lowering itself (the IR
// Type::Kind::Struct, its field table, and the frontend-only guards) is
// covered in ir_test.cpp / lower_test.cpp; these are end-to-end JIT-run tests
// since the one real bug found here (a field narrower than the struct's own
// forced-4-state domain producing a mismatched LLVM type) only surfaced at
// LLVM verification, not in the IR.

namespace {

// JIT-run a module and capture stdout (mirrors display_test.cpp).
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

TEST_CASE("blocking read and write of packed struct fields") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'hAB;
                x.a = 4'h5;
                $display("a=%h b=%h whole=%h", x.a, x.b, x);
            end
        endmodule
    )") == "a=5 b=ab whole=5ab\n");
}

TEST_CASE("nonblocking write to a struct field") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            logic clk;
            initial begin
                clk = 0;
                x.b = 8'h00;
                #1 clk = 1;
                #1; // let the NBA region commit before reading
                $display("b=%h", x.b);
            end
            always @(posedge clk) x.b <= x.b + 8'd1;
        endmodule
    )") == "b=01\n");
}

// Regression: field `b` is 2-state (`bit`) but sibling field `a` is 4-state
// (`logic`), so the whole struct's physical storage is forced 4-state (LRM
// 6.11.2). The backend's PartSelect type inference inherits the base
// signal's domain (correct for a domain-uniform vector/array, the only prior
// case), so a bare struct-field read was 4-state-shaped even though `b`'s
// declared type is 2-state — invisible until the value fed an arithmetic op
// that trusts the declared type, which crashed LLVM verification (mismatched
// operand widths). Fixed by resizing a MemberAccess read to the field's own
// declared type before use.
TEST_CASE("arithmetic on a 2-state field of an otherwise 4-state struct") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'd1;
                x.b = x.b + 8'd1;
                $display("b=%h", x.b);
            end
        endmodule
    )") == "b=02\n");
}

// Same regression class, reached through a bit/range-select stacked on top
// of a narrow-domain field (`x.b[7:4]`) rather than the bare field read.
// SV can't chain a second bit/range-select onto that result (only member
// access dots further), so this and the MemberAccess case above are the only
// two points a struct field's value can escape into further computation.
TEST_CASE("arithmetic on a range-select of a 2-state struct field") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'hF0;
                $display("slice=%h add=%h", x.b[7:4], x.b[7:4] + 4'h1);
            end
        endmodule
    )") == "slice=f add=0\n");
}

TEST_CASE("compound assignment to a struct field") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'd10;
                x.b += 8'd5;
                $display("b=%h", x.b);
            end
        endmodule
    )") == "b=0f\n");
}

TEST_CASE("nested packed struct member access") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [1:0] suba;
                bit [3:0] subb;
            } inner_t;
            typedef struct packed {
                inner_t x;
                logic [2:0] y;
            } outer_t;
            outer_t o;
            initial begin
                o.x.suba = 2'b10;
                o.x.subb = 4'hC;
                o.y = 3'b101;
                $display("whole=%h", o);
            end
        endmodule
    )") == "whole=165\n");
}

TEST_CASE("packed union member access is rejected loudly") {
    const LowerResult r = lower_text(R"(
        module m;
            typedef union packed {
                logic [7:0] a;
                bit [7:0] b;
            } u_t;
            u_t u;
            initial u.a = 8'hAB;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("union member access is not supported") != std::string::npos);
}
