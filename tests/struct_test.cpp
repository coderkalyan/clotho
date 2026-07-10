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

TEST_CASE("blocking read and write of unpacked struct members") {
    CHECK(run_capture(R"(
        module m;
            typedef struct {
                logic [7:0] a;
                bit  [3:0]  b;
                logic       c;
            } s_t;
            s_t x;
            initial begin
                x.a = 8'hAB;
                x.b = 4'd5;
                x.c = 1'b1;
                $display("a=%h b=%h c=%b", x.a, x.b, x.c);
            end
        endmodule
    )") == "a=ab b=5 c=1\n");
}

TEST_CASE("unpacked struct member power-on: 4-state X, 2-state 0") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x;
            initial $display("a=%h b=%h", x.a, x.b);
        endmodule
    )") == "a=xx b=0\n");
}

TEST_CASE("whole unpacked struct copy is field-wise") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x, y;
            initial begin
                x.a = 8'h12; x.b = 4'hF;
                y = x;
                $display("y=%h %h", y.a, y.b);
            end
        endmodule
    )") == "y=12 f\n");
}

TEST_CASE("unpacked struct assignment pattern initializes each member") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; logic c; } s_t;
            s_t x;
            initial begin
                x = '{a: 8'hA5, b: 4'h3, c: 1'b1};
                $display("%h %h %b", x.a, x.b, x.c);
            end
        endmodule
    )") == "a5 3 1\n");
}

TEST_CASE("unpacked struct declaration initializer") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x = '{a: 8'h77, b: 4'hE};
            initial $display("%h %h", x.a, x.b);
        endmodule
    )") == "77 e\n");
}

TEST_CASE("unpacked struct equality is field-wise; X makes it ambiguous (LRM 11.4.5)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x, y, z;
            initial begin
                x.a = 8'h11; x.b = 4'h2;
                y.a = 8'h11; y.b = 4'h2;
                z.a = 8'h11; z.b = 4'h3;
                $display("eq=%b ne=%b", x == y, x != z);
                x.a = 8'hxx;
                $display("xeq=%b", x == y);
            end
        endmodule
    )") == "eq=1 ne=1\nxeq=x\n");
}

TEST_CASE("arithmetic on a mixed-domain unpacked struct member") {
    // A 2-state `bit` member beside a 4-state `logic` member: the member read
    // must carry the member's own declared domain, not the aggregate's.
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [7:0] b; } s_t;
            s_t x;
            initial begin
                x.a = 8'd10;
                x.b = 8'd20;
                $display("sum=%0d", x.a + x.b);
            end
        endmodule
    )") == "sum=30\n");
}

TEST_CASE("nested packed struct member inside an unpacked struct") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { logic [3:0] hi; logic [3:0] lo; } byte_t;
            typedef struct { byte_t p; logic [7:0] q; } s_t;
            s_t x;
            initial begin
                x.p.hi = 4'hA; x.p.lo = 4'h5;
                x.q = 8'hCD;
                $display("p=%h q=%h", x.p, x.q);
            end
        endmodule
    )") == "p=a5 q=cd\n");
}

TEST_CASE("nonblocking write to unpacked struct members, one cycle latency") {
    // Two clock edges: member NBA reads the pre-edge value and commits after, so
    // after edge 1 a==11 (10+1), and the comb read settles to the committed value.
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x;
            logic clk;
            initial begin
                clk = 0;
                x.a = 8'd10; x.b = 4'd2;
                #1 clk = 1; #1 clk = 0;
                #1 clk = 1; #1 clk = 0; #1;
                $display("a=%0d b=%0d", x.a, x.b);
            end
            always @(posedge clk) begin
                x.a <= x.a + 8'd1;
                x.b <= x.b + 4'd1;
            end
        endmodule
    )") == "a=12 b=4\n");
}

TEST_CASE("whole unpacked struct nonblocking copy has NBA sampling") {
    // y <= x samples x in the active region, so y trails x by one clock.
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x, y;
            logic clk;
            initial begin
                clk = 0;
                x.a = 8'd1; x.b = 4'd0;
                #1 clk = 1; #1 clk = 0;   // x <= 2, y <= 1 (old x)
                #1 clk = 1; #1 clk = 0; #1;
                $display("x=%0d y=%0d", x.a, y.a);
            end
            always @(posedge clk) x.a <= x.a + 8'd1;
            always @(posedge clk) y <= x;
        endmodule
    )") == "x=3 y=2\n");
}

TEST_CASE("combinational reader re-evaluates after a struct-member NBA commit") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x;
            logic clk;
            logic [7:0] sum;
            always_comb sum = x.a + {4'd0, x.b};
            always @(posedge clk) begin x.a <= 8'd20; x.b <= 4'd5; end
            initial begin
                clk = 0; x.a = 8'd0; x.b = 4'd0;
                #1 clk = 1; #1;
                $display("sum=%0d", sum);
            end
        endmodule
    )") == "sum=25\n");
}

TEST_CASE("nonblocking write to part of a struct member is guarded (Stage 2)") {
    const LowerResult r = lower_text(R"(
        module m;
            typedef struct { logic [7:0] a; } s_t;
            s_t x;
            logic clk;
            always @(posedge clk) x.a[3:0] <= 4'h1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("nonblocking assignment to part of an unpacked struct member") !=
          std::string::npos);
}

TEST_CASE("array of unpacked struct is guarded (not silently mislowered)") {
    // A heterogeneous [N x StructType] the homogeneous-leaf array model cannot
    // express; must fail loudly, never crash (regression: it segfaulted).
    const LowerResult r = lower_text(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t arr [0:3];
            initial arr[1].a = 8'hAB;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("array of unpacked struct is not supported") != std::string::npos);
}

TEST_CASE("whole unpacked struct as a by-value rvalue is guarded") {
    // Passing a whole unpacked struct by value (here a function argument) has no
    // flat value to load and no by-value struct ABI yet (Stage 2).
    const LowerResult r = lower_text(R"(
        module m;
            typedef struct { logic [7:0] a; } s_t;
            s_t x;
            int y;
            function int f(s_t s); return 1; endfunction
            initial y = f(x);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("unpacked struct as a whole-value rvalue") != std::string::npos);
}
