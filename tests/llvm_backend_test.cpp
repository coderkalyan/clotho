#include <doctest/doctest.h>

#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <unistd.h>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "ir/Builder.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/Module.h"

using namespace clotho;

namespace {

// Lower SystemVerilog text to a Module and emit LLVM IR as a string.
std::string emit(std::string_view source) {
    const LowerResult result = lower_text(source);
    REQUIRE(result.ok);
    REQUIRE(result.module != nullptr);

    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*result.module, os));
    return text;
}

// JIT-run a module and return everything its initial blocks wrote to stdout.
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

// Count non-overlapping occurrences of `needle` in `hay`.
size_t count_substr(const std::string& hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

constexpr std::string_view kAlu = R"(
    module alu(
        input            clk,
        input            rst,
        input     [1:0]  op,
        input     [7:0]  a,
        input     [7:0]  b,
        output    [7:0]  result
    );
        reg  [7:0] acc;
        wire [7:0] next;

        assign next = (op == 2'd0) ? (a + b) :
                      (op == 2'd1) ? (a - b) :
                      (op == 2'd2) ? (a & b) :
                                     (a ^ b);

        always @(posedge clk)
            if (rst) acc <= 8'd0;
            else     acc <= next;

        assign result = acc;
    endmodule
)";

} // namespace

TEST_CASE("alu emits verifiable llvm ir") {
    const std::string ir = emit(kAlu);

    // State is typed: the module definition gets a named state struct, the root
    // %clotho.state composes it, and main stack-allocates the root type.
    CHECK(ir.find("%alu.state = type") != std::string::npos);
    CHECK(ir.find("alloca %clotho.state") != std::string::npos);

    // One coroutine per process, named @<module>.p<index>: two combinational
    // assigns + one sequential always block. Each is a switched-resume coroutine
    // ramp returning the handle and taking the bare state ptr plus the timing-wheel
    // ptr. (CoroSplit also emits .resume/.destroy/.cleanup parts, which don't match
    // this "define ptr @alu.p" prefix.)
    CHECK(count_substr(ir, "define ptr @alu.p") == 3);
    CHECK(ir.find("(ptr %state, ptr %wheel)") != std::string::npos);

    // Datapath ops present.
    CHECK(ir.find(" add ") != std::string::npos);
    CHECK(ir.find(" sub ") != std::string::npos);

    // Signal access via constant-index typed GEPs into the state structs.
    CHECK(ir.find("getelementptr inbounds %clotho.state, ptr") != std::string::npos);
    CHECK(ir.find("store i8") != std::string::npos);

    // The if/else lowers to branches.
    CHECK(ir.find("br i1") != std::string::npos);
}

TEST_CASE("2-state vs 4-state widths") {
    // Driven from an initial so the writes land in one initial process; an assign
    // would lower to its own scheduled process, which is fine but adds coroutine
    // noise this layout-focused case does not need.
    constexpr std::string_view src = R"(
        module m(input bit [3:0] x, output bit [3:0] y, output logic [7:0] z);
            initial begin
                y = x & x;
                z = 8'hAB;
            end
        endmodule
    )";
    const std::string          ir  = emit(src);

    // Every signal keeps its LLVM slot type, named per (domain, width) — and
    // the module's state struct lists them in declaration order: x, y
    // (bit[3:0]) then z (logic[7:0]). The 2-state signals are decoupled from the
    // packed-word paths (no ABI / NBA / array), so they get the genuine single-iW
    // slot `%bit.4 = { i4 }`; the 4-state z keeps the i(2W) must|may word.
    CHECK(ir.find("%m.state = type { %bit.4, %bit.4, %logic.nz.8 }") != std::string::npos);
    CHECK(ir.find("%bit.4 = type { i4 }") != std::string::npos);
    CHECK(ir.find("%logic.nz.8 = type { i16 }") != std::string::npos);
    CHECK(ir.find("alloca %clotho.state") != std::string::npos);
    // z's must|may word is an i16 (2W = 16).
    CHECK(ir.find("store i16") != std::string::npos);
    CHECK(ir.find("define ptr @m.") != std::string::npos);
}

TEST_CASE("instance state structs compose nested and replicate per instantiation") {
    constexpr std::string_view src = R"(
        module leaf(input logic a, output logic [7:0] q);
            logic [7:0] cnt;
            always @(posedge a) cnt <= cnt + 8'd1;
            assign q = cnt;
        endmodule
        module top;
            logic clk;
            logic [7:0] q0, q1;
            leaf u0(.a(clk), .q(q0));
            leaf u1(.a(clk), .q(q1));
        endmodule
    )";
    const std::string          ir  = emit(src);

    // One state struct per module definition; the parent embeds one field per
    // child instance (two %leaf.state replicas), and the root composes the top
    // instance plus the NBA shadow region. No z sources → every slot elided.
    CHECK(ir.find("%leaf.state = type { %logic.nz.1, %logic.nz.8, %logic.nz.8 }") != std::string::npos);
    CHECK(ir.find("%top.state = type { %logic.nz.1, %logic.nz.8, %logic.nz.8, %leaf.state, %leaf.state }") !=
          std::string::npos);
    CHECK(ir.find("%clotho.state = type { %top.state, %clotho.nba.shadow }") != std::string::npos);
    // Exactly one definition of the leaf struct type (deduplicated, no .0 clone).
    CHECK(ir.find("%leaf.state.0") == std::string::npos);
}

TEST_CASE("single-bit xor uses the packed truth-table immediate") {
    constexpr std::string_view src = R"(
        module m(input logic a, input logic b, output logic y);
            assign y = a ^ b;
        endmodule
    )";
    const std::string          ir  = emit(src);

    // Width-1 XOR shifts the 16-entry packed LUT (0x23A200E0 = 597819616) by the
    // operand index instead of unpacking must/may planes.
    CHECK(ir.find("lshr i32 597819616") != std::string::npos);
}

TEST_CASE("guarded wide xor: known fast path and X slow path agree with LRM") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b, y;
            initial begin
                a = 8'hAA;        b = 8'hCC;        y = a ^ b; $display("%b", y);
                a = 8'b1010_101x; b = 8'b0000_0000; y = a ^ b; $display("%b", y);
                a = 8'b0000_00xx; b = 8'b0000_0011; y = a ^ b; $display("%b", y);
            end
        endmodule
    )";
    // Known^known takes the fast leg (AA^CC); any X bit takes the slow leg, where
    // X^anything = X and known bits xor normally.
    CHECK(run_capture(src) == "01100110\n1010101x\n000000xx\n");
}

TEST_CASE("wide xor keeps the generic unpack path") {
    constexpr std::string_view src = R"(
        module m(input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
            assign y = a ^ b;
        endmodule
    )";
    const std::string          ir  = emit(src);

    // The LUT immediate is single-bit only; an 8-bit XOR must not use it.
    CHECK(ir.find("597819616") == std::string::npos);
}

TEST_CASE("single-bit add reuses the xor truth-table immediate, no branch") {
    constexpr std::string_view src = R"(
        module m(input logic a, input logic b, output logic y);
            assign y = a + b;
        endmodule
    )";
    const std::string          ir  = emit(src);

    // 1-bit add truncates the carry to a^b, so it indexes the LUT (== XOR1_LUT)
    // instead of the branching arithmetic path.
    CHECK(ir.find("lshr i32 597819616") != std::string::npos);
    CHECK(ir.find("arith.x") == std::string::npos);
}

TEST_CASE("single-bit add agrees with LRM, including X") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, y;
            initial begin
                a = 1'b1; b = 1'b1; y = a + b; $display("%b", y);
                a = 1'b1; b = 1'b0; y = a + b; $display("%b", y);
                a = 1'bx; b = 1'b1; y = a + b; $display("%b", y);
            end
        endmodule
    )";
    // 1+1 truncates to 0; 1+0 = 1; x + anything = x.
    CHECK(run_capture(src) == "0\n1\nx\n");
}

TEST_CASE("wide add keeps the branching arithmetic path") {
    constexpr std::string_view src = R"(
        module m(input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
            assign y = a + b;
        endmodule
    )";
    const std::string          ir  = emit(src);

    // Not the single-bit LUT path; the branching arithmetic lowering is used.
    CHECK(ir.find("597819616") == std::string::npos);
    CHECK(ir.find("arith") != std::string::npos);
}

TEST_CASE("single-bit sub reuses the xor truth-table immediate, no branch") {
    const std::string ir = emit(R"(
        module m(input logic a, input logic b, output logic y);
            assign y = a - b;
        endmodule
    )");
    // 1-bit sub truncates the borrow to a^b, so it shares the XOR LUT.
    CHECK(ir.find("lshr i32 597819616") != std::string::npos);
    CHECK(ir.find("arith.x") == std::string::npos);
}

TEST_CASE("single-bit sub agrees with LRM, including X") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, y;
            initial begin
                a = 1'b1; b = 1'b1; y = a - b; $display("%b", y);
                a = 1'b0; b = 1'b1; y = a - b; $display("%b", y);
                a = 1'bx; b = 1'b1; y = a - b; $display("%b", y);
            end
        endmodule
    )";
    // 1-1 = 0; 0-1 borrows, truncates to 1; x - anything = x.
    CHECK(run_capture(src) == "0\n1\nx\n");
}

TEST_CASE("single-bit mul uses its own truth-table immediate, no branch") {
    const std::string ir = emit(R"(
        module m(input logic a, input logic b, output logic y);
            assign y = a * b;
        endmodule
    )");
    // Mul's table differs from xor (0*x = x), so a distinct immediate.
    CHECK(ir.find("lshr i32 -526254048") != std::string::npos);
    CHECK(ir.find("597819616") == std::string::npos);
    CHECK(ir.find("arith.x") == std::string::npos);
}

TEST_CASE("single-bit mul agrees with LRM: value is and, but X propagates") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, y;
            initial begin
                a = 1'b1; b = 1'b1; y = a * b; $display("%b", y);
                a = 1'b1; b = 1'b0; y = a * b; $display("%b", y);
                a = 1'b0; b = 1'bx; y = a * b; $display("%b", y);
                a = 1'bx; b = 1'b1; y = a * b; $display("%b", y);
            end
        endmodule
    )";
    // 1*1 = 1; 1*0 = 0; but 0*x = x (arithmetic X rule), unlike bitwise 0&x = 0.
    CHECK(run_capture(src) == "1\n0\nx\nx\n");
}

TEST_CASE("wide mul keeps the branching arithmetic path") {
    const std::string ir = emit(R"(
        module m(input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
            assign y = a * b;
        endmodule
    )");
    CHECK(ir.find("-526254048") == std::string::npos);
    CHECK(ir.find("arith") != std::string::npos);
}

TEST_CASE("single-bit div/mod/pow use their own truth-table immediates, no branch") {
    const std::string div = emit(R"(
        module m(input logic a, input logic b, output logic y); assign y = a / b; endmodule
    )");
    CHECK(div.find("lshr i32 -492699614") != std::string::npos); // DIV1_LUT
    CHECK(div.find("divmod.x") == std::string::npos);

    const std::string mod = emit(R"(
        module m(input logic a, input logic b, output logic y); assign y = a % b; endmodule
    )");
    CHECK(mod.find("lshr i32 581042210") != std::string::npos);  // MOD1_LUT
    CHECK(mod.find("divmod.x") == std::string::npos);

    const std::string pow = emit(R"(
        module m(input logic a, input logic b, output logic y); assign y = a ** b; endmodule
    )");
    CHECK(pow.find("lshr i32 -475922397") != std::string::npos); // POW1_LUT
    CHECK(pow.find("pow.x") == std::string::npos);
}

TEST_CASE("single-bit div agrees with LRM, including divide-by-zero and X") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, y;
            initial begin
                a = 1'b1; b = 1'b1; y = a / b; $display("%b", y);
                a = 1'b0; b = 1'b1; y = a / b; $display("%b", y);
                a = 1'b1; b = 1'b0; y = a / b; $display("%b", y);
                a = 1'bx; b = 1'b1; y = a / b; $display("%b", y);
            end
        endmodule
    )";
    // 1/1 = 1; 0/1 = 0; divide-by-zero = x; x / anything = x.
    CHECK(run_capture(src) == "1\n0\nx\nx\n");
}

TEST_CASE("single-bit mod agrees with LRM, including divide-by-zero and X") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, y;
            initial begin
                a = 1'b1; b = 1'b1; y = a % b; $display("%b", y);
                a = 1'b0; b = 1'b1; y = a % b; $display("%b", y);
                a = 1'b1; b = 1'b0; y = a % b; $display("%b", y);
                a = 1'bx; b = 1'b1; y = a % b; $display("%b", y);
            end
        endmodule
    )";
    // a % 1 = 0; divide-by-zero = x; x % anything = x.
    CHECK(run_capture(src) == "0\n0\nx\nx\n");
}

TEST_CASE("single-bit pow agrees with LRM, including X") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, y;
            initial begin
                a = 1'b0; b = 1'b0; y = a ** b; $display("%b", y);
                a = 1'b0; b = 1'b1; y = a ** b; $display("%b", y);
                a = 1'b1; b = 1'b0; y = a ** b; $display("%b", y);
                a = 1'bx; b = 1'b1; y = a ** b; $display("%b", y);
            end
        endmodule
    )";
    // 0**0 = 1; 0**1 = 0; 1**0 = 1; x ** anything = x.
    CHECK(run_capture(src) == "1\n0\n1\nx\n");
}

TEST_CASE("wide div keeps the branching divmod path") {
    const std::string ir = emit(R"(
        module m(input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
            assign y = a / b;
        endmodule
    )");
    CHECK(ir.find("-492699614") == std::string::npos);
    CHECK(ir.find("divmod") != std::string::npos);
}

TEST_CASE("single-bit == and != use truth-table immediates") {
    const std::string eq = emit(R"(
        module m(input logic a, input logic b, output logic y); assign y = (a == b); endmodule
    )");
    CHECK(eq.find("lshr i32 -526254045") != std::string::npos); // EQ1_LUT

    const std::string ne = emit(R"(
        module m(input logic a, input logic b, output logic y); assign y = (a != b); endmodule
    )");
    CHECK(ne.find("lshr i32 597819616") != std::string::npos);  // NE1_LUT == XOR1_LUT
}

TEST_CASE("single-bit == / != agree with LRM, including X") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, e, n;
            initial begin
                a = 1'b1; b = 1'b1; e = (a == b); n = (a != b); $display("%b%b", e, n);
                a = 1'b1; b = 1'b0; e = (a == b); n = (a != b); $display("%b%b", e, n);
                a = 1'b0; b = 1'b0; e = (a == b); n = (a != b); $display("%b%b", e, n);
                a = 1'bx; b = 1'b1; e = (a == b); n = (a != b); $display("%b%b", e, n);
            end
        endmodule
    )";
    // equal->==1,!=0; unequal->==0,!=1; X operand -> both x.
    CHECK(run_capture(src) == "10\n01\n10\nxx\n");
}

TEST_CASE("wide == keeps the icmp compare path") {
    const std::string ir = emit(R"(
        module m(input logic [7:0] a, input logic [7:0] b, output logic y);
            assign y = (a == b);
        endmodule
    )");
    CHECK(ir.find("-526254045") == std::string::npos);
    CHECK(ir.find("icmp") != std::string::npos);
}

TEST_CASE("===, !==, ==?, and ^~ operators agree with LRM 4-state semantics") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            initial begin
                a = 4'b001x; b = 4'b0011;
                $display("%b%b%b%b", a===b, a!==b, a==?b, a^~b);
                a = 4'b001x; b = 4'b001x;
                $display("%b%b%b%b", a===b, a!==b, a==?b, a^~b);
                a = 4'b0011; b = 4'b001x;
                $display("%b%b%b%b", a===b, a!==b, a==?b, a^~b);
                a = 4'bzz01; b = 4'bzz01;
                $display("%b%b%b%b", a===b, a!==b, a==?b, a^~b);
            end
        endmodule
    )";
    // Each line is %b%b%b%b = ===, !==, ==? (1 bit each) then ^~ (4 bits).
    // === bit-exact (incl x/z); !== its negation; ==? wildcards the right operand's
    // x/z then X-propagating == (so 001x==?0011 is x, 001x==?001x is 1); ^~ is
    // bitwise xnor with X where either bit is x/z. Cross-checked against iverilog.
    CHECK(run_capture(src) ==
          "01x111x\n"   // 001x vs 0011: 0,1,x,111x
          "101111x\n"   // 001x vs 001x: 1,0,1,111x
          "011111x\n"   // 0011 vs 001x: 0,1,1,111x
          "101xx11\n"); // zz01 vs zz01: 1,0,1,xx11
}

TEST_CASE("!=? is the negation of ==?, X-propagating") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            initial begin
                a = 4'b0011; b = 4'b001x; $display("%b%b", a==?b, a!=?b);
                a = 4'b001x; b = 4'b0011; $display("%b%b", a==?b, a!=?b);
                a = 4'b0101; b = 4'b001x; $display("%b%b", a==?b, a!=?b);
            end
        endmodule
    )";
    // !=? negates ==?; an x result stays x. vs iverilog.
    CHECK(run_capture(src) == "10\nxx\n01\n");
}

TEST_CASE("single-bit ^~ uses the bxnor truth-table immediate (== EQ1)") {
    const std::string ir = emit(R"(
        module m(input logic a, input logic b, output logic y); assign y = a ^~ b; endmodule
    )");
    CHECK(ir.find("lshr i32 -526254045") != std::string::npos); // BXNOR1_LUT == EQ1_LUT
}

TEST_CASE("wide ^~ lowers as xnor over the xor path") {
    const std::string ir = emit(R"(
        module m(input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
            assign y = a ^~ b;
        endmodule
    )");
    CHECK(ir.find("-526254045") == std::string::npos); // not the 1-bit LUT
    CHECK(ir.find("fshl") != std::string::npos);        // bnot's plane swap
}

TEST_CASE("single-bit &&, ||, !, ~ agree with LRM Kleene semantics") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            initial begin
                a = 1'b0; b = 1'b0; $display("%b%b%b%b", a&&b, a||b, !a, ~a);
                a = 1'b1; b = 1'bx; $display("%b%b%b%b", a&&b, a||b, !a, ~a);
                a = 1'b0; b = 1'bx; $display("%b%b%b%b", a&&b, a||b, !a, ~a);
                a = 1'bx; b = 1'bx; $display("%b%b%b%b", a&&b, a||b, !a, ~a);
                a = 1'b1; b = 1'b0; $display("%b%b%b%b", a&&b, a||b, !a, ~a);
            end
        endmodule
    )";
    // 1-bit && / || are the monotone bitwise & / |; ! is the same op as ~ at one
    // bit. Verified against iverilog: false dominates && (0&&x=0), true dominates
    // || (1||x=1).
    CHECK(run_capture(src) == "0011\nx100\n0x11\nxxxx\n0100\n");
}

TEST_CASE("single-bit && lowers to a packed and (no truth reduction)") {
    const std::string ir = emit(R"(
        module m(input logic a, input logic b, output logic y); assign y = a && b; endmodule
    )");
    CHECK(ir.find("and i2") != std::string::npos);
}

TEST_CASE("multi-bit relational and logical guard X (branchy), agree with LRM") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            initial begin
                a=4'd5;    b=4'd3; $display("%b%b%b%b%b%b", a<b, a<=b, a>b, a>=b, a&&b, a||b);
                a=4'd3;    b=4'd3; $display("%b%b%b%b%b%b", a<b, a<=b, a>b, a>=b, a&&b, a||b);
                a=4'b001x; b=4'd3; $display("%b%b%b%b%b%b", a<b, a<=b, a>b, a>=b, a&&b, a||b);
                a=4'd0;    b=4'b001x; $display("%b%b%b%b%b%b", a<b, a<=b, a>b, a>=b, a&&b, a||b);
                a=4'b001x; b=4'd0; $display("%b%b%b%b%b%b", a<b, a<=b, a>b, a>=b, a&&b, a||b);
            end
        endmodule
    )";
    // Relationals: any x/z -> X. Logical: cold (Kleene) path still resolves a
    // dominating operand (001x is nonzero -> true; 0 -> false). vs iverilog.
    CHECK(run_capture(src) ==
          "001111\n"   // 5,3
          "010111\n"   // 3,3
          "xxxx11\n"   // 001x(true),3   -> rel x, &&=1, ||=1
          "xxxx01\n"   // 0(false),001x  -> &&=0, ||=1
          "xxxx01\n"); // 001x(true),0   -> &&=0, ||=1
}

TEST_CASE("ordered relational guards X with a branch") {
    const std::string ir = emit(R"(
        module m(input logic [7:0] a, input logic [7:0] b, output logic y);
            assign y = a < b;
        endmodule
    )");
    CHECK(ir.find("bit.val") != std::string::npos); // branchy fast/cold split
    CHECK(ir.find("icmp ult i8") != std::string::npos);
}

TEST_CASE("delay suspends and reschedules on the wheel") {
    constexpr std::string_view src = R"(
        module d;
            initial begin
                $display("hi");
                #20;
                $display("bye");
            end
        endmodule
    )";
    const std::string          ir  = emit(src);

    // #20 lowers to: read current tick, schedule a wheel timer, and suspend. The
    // coroutine intrinsics themselves are lowered away by CoroSplit; the runtime
    // calls are what remain.
    CHECK(ir.find("call i64 @rt_wheel_now") != std::string::npos);
    CHECK(ir.find("call void @rt_wheel_schedule") != std::string::npos);
    // The timer callback that wakes the suspended coroutine by pushing its handle
    // onto the active queue.
    CHECK(ir.find("define internal void @clotho.enqueue_timer") != std::string::npos);
    // main drains the wheel: tick while timers are pending.
    CHECK(ir.find("call i32 @rt_wheel_pending") != std::string::npos);
    CHECK(ir.find("call void @rt_wheel_tick") != std::string::npos);
}

TEST_CASE("multi-bit if / ?: predicates reduce to LRM truth values") {
    // A predicate wider than one bit must be reduced to its 1-bit truth value
    // (LRM §12.4: true if any bit is 1). `sel = 8'd2` has bit 0 clear but bit 1
    // set, so a naive low-bit truncation would wrongly read false. Both `if` and
    // `?:` must agree it is true. Before the reduction landed this also produced a
    // mismatched-width icmp that failed verifyModule.
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [7:0] sel;
                integer r;
                sel = 8'd2;
                if (sel) r = 1; else r = 0;
                $display("if=%0d", r);
                r = sel ? 7 : 9;
                $display("tern=%0d", r);
                sel = 8'd0;
                if (sel) r = 1; else r = 0;
                $display("ifz=%0d", r);
                r = sel ? 7 : 9;
                $display("ternz=%0d", r);
            end
        endmodule
    )");
    CHECK(out == "if=1\ntern=7\nifz=0\nternz=9\n");
}

TEST_CASE("if vs ?: propagate an ambiguous predicate differently") {
    // An X/Z condition is ambiguous. `if` takes the else branch (LRM §12.4);
    // `?:` returns the bitwise merge of both operands (LRM §11.4.11): bits that
    // agree pass through, bits that differ become X. Z bits read as X. Verified
    // against iverilog.
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [3:0] s;
                logic [3:0] r;
                s = 4'bxxxx;
                if (s) r = 4'b1010; else r = 4'b0101;
                $display("if=%b", r);
                r = s ? 4'b1100 : 4'b1010;
                $display("tern=%b", r);
                s = 4'bzz00;
                if (s) r = 4'b1111; else r = 4'b0000;
                $display("ifz=%b", r);
            end
        endmodule
    )");
    CHECK(out == "if=0101\ntern=1xx0\nifz=0000\n");
}

TEST_CASE("signed arithmetic obeys LRM signedness across the operator set") {
    // Sign extension on widening, signed compare, signed divide/modulo (trunc
    // toward zero, INT_MIN/-1 overflow wrap), signed multiply/add widening, and
    // the mixed-sign rule (LRM §11.8.1: any unsigned operand => unsigned op).
    // $signed/$unsigned reinterpret without changing the bits but drive later
    // sign-extension. All values cross-checked against iverilog.
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic signed [3:0] a4; logic signed [7:0] b8, b8u;
                logic signed [3:0] d1, d2;
                logic signed [7:0] e1; logic [7:0] e2;
                logic [3:0] u4; logic signed [3:0] sm4;
                logic signed [31:0] mn, q, r;
                logic signed [3:0] h1, h2; logic signed [7:0] hr, ir2;
                a4 = -4'sd3; b8 = a4;       $display("%0d", b8);            // -3
                b8u = $unsigned(4'b1111);   $display("%0d", b8u);           // 15
                $display("%0d", $signed(4'b1111));                          // -1
                d1 = -1; d2 = 1;            $display("%0d", d1 < d2);       // 1
                e1 = -1; e2 = 1;            $display("%0d", e1 < e2);       // 0
                u4 = 4'b1111;               $display("%0d", $signed(u4) < 0); // 1
                $display("%0d %0d %0d %0d", -7/2, 7/-2, -7%2, 7%-2);        // -3 -3 -1 1
                mn = -2147483648; q = mn/-1; r = mn%-1;
                $display("%0d %0d", q, r);                                  // -2147483648 0
                h1 = -2; h2 = 3; hr = h1*h2; $display("%0d", hr);           // -6
                ir2 = h1 + h1;             $display("%0d", ir2);            // -4
            end
        endmodule
    )");
    CHECK(out == "-3\n15\n-1\n1\n0\n1\n-3 -3 -1 1\n-2147483648 0\n-6\n-4\n");
}

TEST_CASE("shifts define the out-of-range amount instead of emitting poison") {
    // LRM §11.4.10: a shift amount >= the operand width yields 0 for <<, >>, and
    // unsigned >>>, while signed >>> fills with the sign bit. LLVM shift ops are
    // poison at amount >= width, so the backend must guard the range; it also
    // must test the untruncated amount so multiples of 2**width (e.g. >>256 on an
    // 8-bit value) do not alias to a no-op. Cross-checked against iverilog.
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [7:0] u; logic signed [7:0] s; logic [15:0] amt;
                u = 8'hFF; s = -8'sd1;
                $display("%0d %0d %0d", u << 8, u >> 8, u >> 300);          // 0 0 0
                $display("%0d %0d", u << 256, u >> 256);                    // 0 0
                $display("%0d %0d", s >>> 8, s >>> 256);                    // -1 -1
                $display("%0d", $signed(8'b11111000) >>> 1);               // -4
                $display("%0d", u >>> 1);                                   // 127
                amt = 16'd256; $display("%0d", u >> amt);                   // 0
                amt = 16'd3;   $display("%0d", u >> amt);                   // 31
            end
        endmodule
    )");
    CHECK(out == "0 0 0\n0 0\n-1 -1\n-4\n127\n0\n31\n");
}

TEST_CASE("delay inside a conditional splits cleanly") {
    // A #delay nested in an if/else branch suspends mid-conditional. The point is
    // that CoroSplit handles the resulting CFG and the module still verifies —
    // emit() REQUIREs emit_llvm (which runs the coroutine passes and verifyModule)
    // to succeed, so a malformed split would fail this test outright.
    constexpr std::string_view src = R"(
        module c;
            logic flag;
            initial begin
                flag = 1;
                $display("start");
                if (flag) begin
                    #10;
                    $display("then");
                end else begin
                    #99;
                    $display("else");
                end
                $display("done");
            end
        endmodule
    )";
    const std::string          ir  = emit(src);

    // The conditional's branch and both delays' schedules survive lowering.
    CHECK(ir.find("br i1") != std::string::npos);
    CHECK(count_substr(ir, "call void @rt_wheel_schedule") == 2);
    CHECK(ir.find("define internal void @clotho.enqueue_timer") != std::string::npos);
}

TEST_CASE("event waits emit wait queues, always loops, and queue servicing") {
    // A clocked counter feeding a level-sensitive combinational block, driven by
    // an initial that blocks on @(doubled). Exercises every wait mechanism: a
    // persistent always-block subscription, a one-shot intra-body @(sig), and the
    // blocking store that services the queue. emit() REQUIREs verifyModule, so a
    // malformed always-loop / wait-suspend CFG fails this test outright.
    constexpr std::string_view src = R"(
        module w;
            reg clk, rst;
            reg [7:0] d, q, doubled;
            integer i;
            always @(posedge clk) if (rst) q = 8'd0; else q = q + 8'd1;
            always @(d or q)      doubled = d + q;
            initial begin
                rst = 1; d = 8'd10; clk = 0;
                #5 clk = 1; #5 clk = 0; rst = 0;
                for (i = 0; i < 3; i = i + 1) begin
                    #5 clk = 1;
                    @(doubled);
                    $display("q=%0d doubled=%0d", q, doubled);
                    #5 clk = 0;
                end
            end
        endmodule
    )";
    const std::string          ir  = emit(src);

    // Per-edge split wait queues: @(posedge clk) -> clk's static positive bucket;
    // @(d or q) -> d's and q's static any-change buckets; intra-body @(doubled) ->
    // doubled's dynamic any-change bucket (with a length). Each queue is named for
    // its edge and emitted only when used; the per-process scheduled-flag array
    // backs the wake-once dedup.
    CHECK(ir.find("@clotho.wait.clk.sta.pos = internal global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.d.sta.any = internal global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.q.sta.any = internal global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.doubled.dyn.any = internal global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.doubled.dyn.any.len = internal global") != std::string::npos);
    CHECK(ir.find("@clotho.proc_sched = internal global") != std::string::npos);
    // The always blocks lower to a forever wait loop; servicing has both a static
    // and a dynamic scan.
    CHECK(ir.find("always.loop") != std::string::npos);
    CHECK(ir.find("sta.head") != std::string::npos);
    CHECK(ir.find("dyn.head") != std::string::npos);
}

TEST_CASE("$finish lowers to an rt_finish call") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                $display("done");
                $finish;
            end
        endmodule
    )";
    const std::string          ir  = emit(src);

    CHECK(ir.find("call void @rt_finish()") != std::string::npos);
}

TEST_CASE("$stop lowers identically to $finish") {
    constexpr std::string_view src = R"(
        module m;
            initial $stop;
        endmodule
    )";
    const std::string          ir  = emit(src);

    CHECK(ir.find("call void @rt_finish()") != std::string::npos);
}

TEST_CASE("$finish with a diagnostic-level argument lowers (argument ignored)") {
    constexpr std::string_view src = R"(
        module m;
            initial $finish(2);
        endmodule
    )";
    const std::string          ir  = emit(src);

    CHECK(ir.find("call void @rt_finish()") != std::string::npos);
}

TEST_CASE("$time and $stime read the simulation clock") {
    // With no timescale the wheel tick count is the time-unit-scaled value, so
    // $time returns the raw clock and $stime mirrors it (truncated to 32 bits).
    const std::string out = run_capture(R"(
        module m;
            initial begin
                $display("%0d", $time);
                #5;
                $display("%0d %0d", $time, $stime);
                #3;
                $display("%0d", $time);
            end
        endmodule
    )");

    CHECK(out == "0\n5 5\n8\n");
}

TEST_CASE("$time lowers to an rt_wheel_now call") {
    constexpr std::string_view src = R"(
        module m;
            initial $display("%0d", $time);
        endmodule
    )";
    const std::string          ir  = emit(src);

    CHECK(ir.find("call i64 @rt_wheel_now(") != std::string::npos);
}

TEST_CASE("timescale scales delays into the global precision and $time back to the unit") {
    // The wheel counts in the design's finest precision (1ps here, from top).
    // sub's #5 is 5 ns == 5000 ticks, so it fires AFTER top's #4000 (4000 ticks),
    // proving the delay scaled up. sub's $time scales back to its 1ns unit (5),
    // while top's 1ps unit reads ticks directly (4000).
    const std::string out = run_capture(R"(`timescale 1ns/1ps
        module sub;
            initial #5 $display("sub %0d", $time);
        endmodule
        `timescale 1ps/1ps
        module top;
            sub s();
            initial #4000 $display("top %0d", $time);
        endmodule
    )");

    CHECK(out == "top 4000\nsub 5\n");
}

TEST_CASE("$time rounds the scaled clock to the nearest module time unit") {
    // Global precision is 1ns. fine's #15 lands the wheel on tick 15; top's unit
    // is 10ns, so $time = round(15 / 10) = 2 (1.5 rounds up, LRM §20.3.1).
    const std::string out = run_capture(R"(`timescale 1ns/1ns
        module fine(output reg ping);
            initial begin ping = 0; #15 ping = 1; end
        endmodule
        `timescale 10ns/1ns
        module top;
            wire ping;
            fine f(.ping(ping));
            always @(posedge ping) $display("%0d", $time);
        endmodule
    )");

    CHECK(out == "2\n");
}

TEST_CASE("$printtimescale prints the scope unit and precision") {
    const std::string out = run_capture(R"(`timescale 1ns/1ps
        module m;
            initial $printtimescale;
        endmodule
    )");

    CHECK(out == "Time scale of (m) is 1ns / 1ps\n");
}

TEST_CASE("%t formats time per $timeformat units, precision, suffix, and width") {
    // Verified against iverilog. $time is 100 ns; changing only the units reprints
    // the same instant in ns then ps, and width right-justifies number + suffix.
    const std::string out = run_capture(R"(`timescale 1ns/1ns
        module m;
            initial begin
                $timeformat(-9, 2, " ns", 10);
                #100 $display("a:%t", $time);
                $timeformat(-12, 0, " ps", 8);
                $display("b:%t", $time);
                $timeformat(-9, 3, " ns", 0);
                $display("c:%t", $time);
            end
        endmodule
    )");

    CHECK(out == "a: 100.00 ns\nb:100000 ps\nc:100.000 ns\n");
}

TEST_CASE("%t default format uses the smallest precision and width 20") {
    // No $timeformat call: units default to the design's smallest precision (1ps),
    // precision 0, empty suffix, field width 20 (LRM Table 20-3). $time is 100 ns
    // == 100000 ps, right-justified in 20 columns. Matches iverilog.
    const std::string out = run_capture(R"(`timescale 1ns/1ps
        module m;
            initial #100 $display("d:%t", $time);
        endmodule
    )");

    CHECK(out == "d:              100000\n");
}

TEST_CASE("%t scales to ticks independent of the calling module time unit") {
    // top's unit is 1us; its $time reads 5 (us). With units -9 the same instant
    // prints as 5000 ns, proving %t scales through real time, not the module unit
    // (LRM §20.4.3). The design tick is 1ns (sub's unit). Matches iverilog.
    const std::string out = run_capture(R"(`timescale 1ns/1ns
        module sub; initial #5500 ; endmodule
        `timescale 1us/1ns
        module top;
            sub s();
            initial begin
                $timeformat(-9, 0, " ns", 0);
                #5 $display("t:%t", $time);
            end
        endmodule
    )");

    CHECK(out == "t:5000 ns\n");
}

TEST_CASE("a later $timeformat changes the format mid-simulation") {
    const std::string out = run_capture(R"(`timescale 1ns/1ns
        module m;
            initial begin
                $timeformat(-9, 0, "ns", 0);
                #10 $display("%t", $time);
                $timeformat(-12, 1, "ps", 0);
                #10 $display("%t", $time);
            end
        endmodule
    )");

    CHECK(out == "10ns\n20000.0ps\n");
}

TEST_CASE("repeated instantiation emits distinct process symbols") {
    // Two instances of one module share AST source locations; the backend must
    // name their processes uniquely (by index) or LLVM would reject duplicate
    // symbol definitions. emit() REQUIREs a well-formed module.
    constexpr std::string_view src = R"(
        module counter(input clk, output reg [3:0] cnt);
            always @(posedge clk) cnt <= cnt + 4'd1;
        endmodule
        module top(input clk, output [3:0] a, output [3:0] b);
            counter u0(.clk(clk), .cnt(a));
            counter u1(.clk(clk), .cnt(b));
        endmodule
    )";
    const std::string          ir  = emit(src);

    // Per instance: 1 always block + 1 input-port copy (clk) + 1 output-port copy
    // (cnt) = 3 ramps; 2 instances + 0 top processes = 6 distinct @top.p<N>
    // coroutine ramps.
    CHECK(count_substr(ir, "define ptr @top.p") == 6);
}

// Regression: a single-operand concat ({x}) and a single replication ({1{x}})
// once shifted the accumulator by the full result width, which is poison in LLVM
// (shift >= bitwidth). Replication wraps its operand in a single-element concat,
// so {N{x}} read garbage. Assert the concrete bit pattern.
TEST_CASE("single-element concat and replication produce correct bits") {
    const std::string out = run_capture(R"(
        module top;
            reg [1:0] a;
            initial begin
                a = 2'b10;
                $display("%b %b %b %b", {a}, {1{a}}, {4{a}}, {4{1'b1}});
            end
        endmodule
    )");
    CHECK(out == "10 10 10101010 1111\n");
}

// $random's global stream is a fixed-seed std::mt19937 (Runtime.cpp), so the
// sequence is fully determined. This is the only test in the suite that draws
// from it, so it observes the stream from its first value. The expected numbers
// are mt19937{0x1979} reinterpreted as int32; negatives prove the signed return
// type, and the distinct values prove it varies.
TEST_CASE("$random yields the deterministic signed-32 stream") {
    const std::string out = run_capture(R"(
        module top;
            integer i;
            initial
                for (i = 0; i < 8; i = i + 1)
                    $display("%0d", $random);
        endmodule
    )");
    CHECK(out ==
          "2009080500\n"
          "-537682317\n"
          "-1813350883\n"
          "1054232434\n"
          "-1699253010\n"
          "1848886818\n"
          "1569458764\n"
          "969765471\n");
}

TEST_CASE("$random lowers to an rt_random call") {
    constexpr std::string_view src = R"(
        module m;
            integer x;
            initial x = $random;
        endmodule
    )";
    const std::string          ir  = emit(src);

    CHECK(ir.find("call i32 @rt_random(") != std::string::npos);
}

TEST_CASE("unsigned divide and modulo truncate toward zero") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [7:0] a, b;
                a = 8'd200; b = 8'd3;
                $display("%0d %0d", a / b, a % b);
            end
        endmodule
    )");
    CHECK(out == "66 2\n");
}

TEST_CASE("divide or modulo by zero yields x") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [7:0] a, b;
                a = 8'd17; b = 8'd0;
                $display("%0d %0d", a / b, a % b);
            end
        endmodule
    )");
    CHECK(out == "x x\n");
}

TEST_CASE("an unknown operand makes divide and modulo x") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [3:0] a, b;
                a = 4'b001x; b = 4'd2;
                $display("%0d %0d", a / b, a % b);
            end
        endmodule
    )");
    CHECK(out == "x x\n");
}

TEST_CASE("signed divide truncates toward zero and modulo takes the dividend sign") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                int a, b;
                a = -7; b = 2;
                $display("%0d %0d", a / b, a % b);
            end
        endmodule
    )");
    CHECK(out == "-3 -1\n");
}

TEST_CASE("signed INT_MIN divided by -1 wraps instead of trapping") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic signed [7:0] a, b;
                a = -128; b = -1;
                $display("%0d %0d", a / b, a % b);
            end
        endmodule
    )");
    // -128 / -1 overflows the 8-bit signed range and wraps back to -128; the
    // remainder is 0.
    CHECK(out == "-128 0\n");
}

TEST_CASE("unsigned power") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [15:0] a, b;
                a = 16'd3; b = 16'd4;
                $display("%0d", a ** b);
            end
        endmodule
    )");
    CHECK(out == "81\n");
}

TEST_CASE("power wraps to the result width and handles the exponent corners") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [7:0] a, e, z;
                a = 8'd2; e = 8'd10; z = 8'd0;
                $display("%0d %0d %0d", a ** e, a ** z, z ** e);
            end
        endmodule
    )");
    // 2**10 = 1024 = 0 mod 256; 2**0 = 1; 0**10 = 0.
    CHECK(out == "0 1 0\n");
}

TEST_CASE("an unknown operand makes power x") {
    const std::string out = run_capture(R"(
        module m;
            initial begin
                logic [7:0] a, b;
                a = 8'b0000_001x; b = 8'd3;
                $display("%0d", a ** b);
            end
        endmodule
    )");
    CHECK(out == "x\n");
}

// Wide (>64-bit) multiply, divide, modulo and power compute correctly. The
// result of each is compared against a hand-computed wide literal so the check
// only ever $displays a 1-bit value (the >64-bit display path is loud-fail).
TEST_CASE("wide multiply, divide, modulo, power produce correct values") {
    // 10^12 * 10^12 = 10^24, which exceeds 64 bits and fits in 100.
    CHECK(run_capture(R"(module m; initial begin
        logic [99:0] a, b, c;
        a = 100'd1000000000000; b = 100'd1000000000000;
        c = a * b;
        $display("%0d", c == 100'd1000000000000000000000000);
    end endmodule)") == "1\n");

    CHECK(run_capture(R"(module m; initial begin
        logic [99:0] a, b, c;
        a = 100'd1000000000000000000000000; b = 100'd1000000000000;
        c = a / b;
        $display("%0d", c == 100'd1000000000000);
    end endmodule)") == "1\n");

    CHECK(run_capture(R"(module m; initial begin
        logic [99:0] a, b, c;
        a = 100'd1000000000000000000000007; b = 100'd10;
        c = a % b;
        $display("%0d", c == 100'd7);
    end endmodule)") == "1\n");

    // 2 ** 70 = 1180591620717411303424, also > 64 bits.
    CHECK(run_capture(R"(module m; initial begin
        logic [99:0] a, c; logic [7:0] e;
        a = 100'd2; e = 8'd70;
        c = a ** e;
        $display("%0d", c == 100'd1180591620717411303424);
    end endmodule)") == "1\n");
}

// The compiler-rt/libgcc libcalls (__multi3 / __udivti3 / __umodti3) must
// resolve and execute in the JIT. $random operands are opaque to the optimizer,
// so these multiplies/divides/mods run at runtime rather than constant-folding —
// if the builtins were unresolved the JIT lookup would fail and produce no
// output. Each design asserts a value-independent identity, so the result is 1
// regardless of the drawn operands.
TEST_CASE("wide arithmetic libcalls resolve and run in the JIT") {
    // a*b fits in 100 bits (operands <= 48 bits), so (a*b)/b == a and (a*b)%b == 0.
    CHECK(run_capture(R"(module m; initial begin
        logic [31:0] r0, r1, r2, r3;
        logic [99:0] a, b, c;
        r0 = $random; r1 = $random; r2 = $random; r3 = $random;
        a = {r0[15:0], r1};
        b = {r2[15:0], r3} | 100'd1;
        c = a * b;
        $display("%0d", (c / b == a) && (c % b == 100'd0));
    end endmodule)") == "1\n");

    // Wide power at runtime: base 0..15, exponent runtime, so base**(e+1) fits in
    // 100 bits and equals (base**e) * base.
    CHECK(run_capture(R"(module m; initial begin
        logic [7:0] rb, re; logic [99:0] base;
        rb = $random; re = $random;
        base = rb & 8'hF;
        re = re & 8'h7;
        $display("%0d", (base ** (re + 8'd1)) == ((base ** re) * base));
    end endmodule)") == "1\n");
}

// Signed power remains unimplemented and must be rejected, not miscompiled.
TEST_CASE("signed power is rejected, not miscompiled") {
    auto rejects = [](std::string_view src) {
        const LowerResult r = lower_text(src);
        REQUIRE_MESSAGE(r.ok, r.diagnostics);
        REQUIRE(r.module != nullptr);
        std::string              text;
        llvm::raw_string_ostream os(text);
        return !ir::emit_llvm(*r.module, os);
    };

    CHECK(rejects(R"(module m; initial begin int a, b, c; a = 2; b = 3; c = a ** b; end endmodule)"));
}

// Multiple top-level modules are each implicitly instantiated once and
// flattened into the one design, sharing the global timeline (LRM 3.11, 23.3.1).
TEST_CASE("multiple top-level modules each elaborate into the design") {
    const std::string out = run_capture(R"(
        module a;
            initial $display("a");
        endmodule
        module b;
            initial $display("b");
        endmodule
    )");
    // Both tops run; time-zero ordering across tops is nondeterministic (LRM
    // 4.7), so accept either interleaving.
    CHECK((out == "a\nb\n" || out == "b\na\n"));
}

// Same-named signals in separate tops must not collide: each top's instance
// name prefixes its flattened paths, and signals resolve by handle regardless.
TEST_CASE("identically named signals in separate tops stay independent") {
    const std::string out = run_capture(R"(
        module a;
            reg [7:0] x;
            initial begin x = 8'd10; $display("%0d", x); end
        endmodule
        module b;
            reg [7:0] x;
            initial begin x = 8'd20; $display("%0d", x); end
        endmodule
    )");
    CHECK((out == "10\n20\n" || out == "20\n10\n"));
}

// -- Unpacked arrays (memories) --

// Element-addressed storage: blocking writes then reads round-trip per element.
TEST_CASE("unpacked array blocking write and read") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:3];
            integer i;
            initial begin
                for (i = 0; i < 4; i = i + 1) mem[i] = i * 16 + 1;
                for (i = 0; i < 4; i = i + 1) $display("%0d", mem[i]);
            end
        endmodule
    )");
    CHECK(out == "1\n17\n33\n49\n");
}

// A nested select indexes a bit / part of an element: mem[i][bit] and a
// read-modify-write part-select mem[i][3:0].
TEST_CASE("unpacked array element bit- and part-select") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:3];
            initial begin
                mem[2] = 8'h21;
                $display("%b", mem[2][7]);
                $display("%b", mem[2][0]);
                mem[2][3:0] = 4'h0;
                $display("%0h", mem[2]);
            end
        endmodule
    )");
    CHECK(out == "0\n1\n20\n");
}

// A descending element range [3:0] maps indices to storage by declared
// direction; element selects must honour it.
TEST_CASE("unpacked array descending index range") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [3:0];
            initial begin
                mem[3] = 8'd9;
                mem[0] = 8'd1;
                $display("%0d %0d", mem[3], mem[0]);
            end
        endmodule
    )");
    CHECK(out == "9 1\n");
}

// Multidimensional unpacked arrays nest: mem[a][b] accumulates a byte offset
// across both dimensions.
TEST_CASE("multidimensional unpacked array") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] md [0:1][0:1];
            initial begin
                md[0][0] = 8'd10; md[0][1] = 8'd20;
                md[1][0] = 8'd30; md[1][1] = 8'd40;
                $display("%0d %0d %0d %0d", md[0][0], md[0][1], md[1][0], md[1][1]);
            end
        endmodule
    )");
    CHECK(out == "10 20 30 40\n");
}

// A 4-state memory reads X at power-on, before any element is written.
TEST_CASE("unpacked array elements power on to X") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:3];
            initial $display("%b", mem[1]);
        endmodule
    )");
    CHECK(out == "xxxxxxxx\n");
}

// A dynamically-computed index reads back the element written at that index.
TEST_CASE("unpacked array dynamic index round-trip") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:255];
            integer i;
            initial begin
                for (i = 0; i < 256; i = i + 1) mem[i] = i[7:0];
                $display("%0d", mem[200]);
            end
        endmodule
    )");
    CHECK(out == "200\n");
}

// Efficiency: a large memory must be element-addressed. Element access loads an
// element-width word (i16 for logic[7:0]) via a dynamic GEP; it must NOT
// materialize an array-width integer (here 256*8*2 = i4096) for a single access.
TEST_CASE("large memory uses element-addressed access, not array-width integers") {
    const std::string ir = emit(R"(
        module top;
            logic [7:0] mem [0:255];
            integer i;
            initial begin
                for (i = 0; i < 256; i = i + 1) mem[i] = i[7:0];
                $display("%0d", mem[200]);
            end
        endmodule
    )");
    CHECK(ir.find("i4096") == std::string::npos);
    CHECK(ir.find("i2048") == std::string::npos);
    CHECK(ir.find("getelementptr") != std::string::npos);
}

// -- Unpacked array nonblocking writes (synchronous memories, phase 2) --

// A clocked write commits in the NBA region, so a same-edge read sees the old
// value and the next edge sees the committed one.
TEST_CASE("synchronous memory write defers to the NBA region") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:3];
            logic clk;
            integer i;
            initial begin
                clk = 0;
                for (i = 0; i < 4; i = i + 1) mem[i] = 0;
                for (i = 0; i < 6; i = i + 1) #5 clk = ~clk;
            end
            always @(posedge clk) mem[1] <= mem[1] + 8'd10;
            always @(posedge clk) $display("%0d", mem[1]);
        endmodule
    )");
    // Each posedge: the reader sees the pre-commit value, then mem[1] += 10.
    CHECK(out == "0\n10\n20\n");
}

// Nonblocking writes sample every RHS before any commit, so a paired swap
// exchanges two elements.
TEST_CASE("nonblocking element writes sample RHS before commit (swap)") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:1];
            logic clk;
            initial begin
                clk = 0;
                mem[0] = 8'd1; mem[1] = 8'd2;
                #5 clk = 1;
                #5 $display("%0d %0d", mem[0], mem[1]);
            end
            always @(posedge clk) begin
                mem[0] <= mem[1];
                mem[1] <= mem[0];
            end
        endmodule
    )");
    CHECK(out == "2 1\n");
}

// A loop issues many element NBAs in one instant; all commit.
TEST_CASE("a loop of element nonblocking writes all commit") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:3];
            logic clk;
            integer i;
            initial begin
                clk = 0;
                for (i = 0; i < 4; i = i + 1) mem[i] = i;
                #5 clk = 1;
                #5 $display("%0d %0d %0d %0d", mem[0], mem[1], mem[2], mem[3]);
            end
            always @(posedge clk)
                for (i = 0; i < 4; i = i + 1) mem[i] <= mem[i] + 8'd100;
        endmodule
    )");
    CHECK(out == "100 101 102 103\n");
}

// A nonblocking write to a part-select of an element merges only those bits at
// commit, preserving the rest.
TEST_CASE("nonblocking part-select write to an element merges at commit") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:1];
            logic clk;
            initial begin
                clk = 0;
                mem[1] = 8'h2A;
                #5 clk = 1;
                #5 $display("%0h", mem[1]);
            end
            always @(posedge clk) mem[1][3:0] <= 4'h7;
        endmodule
    )");
    CHECK(out == "27\n");
}

// A combinational read port (sensitive to the whole memory) re-evaluates after a
// nonblocking element write commits.
TEST_CASE("combinational read port wakes after a nonblocking memory write") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] mem [0:3];
            logic [7:0] q;
            logic [1:0] raddr;
            logic clk;
            assign q = mem[raddr];
            initial begin
                clk = 0;
                raddr = 0;
                mem[0] = 8'd11;
                #1 $display("%0d", q);
                #5 clk = 1;
                #5 $display("%0d", q);
            end
            always @(posedge clk) mem[0] <= 8'd55;
        endmodule
    )");
    CHECK(out == "11\n55\n");
}

// A top with submodules elaborates alongside a flat top; events on both tops
// advance on the one shared wheel.
TEST_CASE("multiple tops share one timeline") {
    const std::string out = run_capture(R"(
        module a;
            initial begin #5 $display("a@%0d", $time); end
        endmodule
        module b;
            initial begin #10 $display("b@%0d", $time); end
        endmodule
    )");
    CHECK(out == "a@5\nb@10\n");
}

// Concatenation as an assignment target (LRM 11.4.12): the RHS bits partition
// across the operands, leftmost taking the most-significant slice.
TEST_CASE("concatenation lvalue distributes the RHS bits") {
    // Plain split: hi gets the high nibble, lo the low nibble.
    CHECK(run_capture(R"(
        module m;
            logic [3:0] hi, lo;
            initial begin {hi, lo} = 8'hAB; $display("%h%h", hi, lo); end
        endmodule
    )") == "ab\n");

    // Non-contiguous slices of one reg: a nibble swap, the canonical picorv32
    // immediate-swizzle shape.
    CHECK(run_capture(R"(
        module m;
            logic [7:0] r;
            initial begin {r[3:0], r[7:4]} = 8'hAB; $display("%h", r); end
        endmodule
    )") == "ba\n");

    // Uneven widths plus X distribution: each operand keeps the X bits of its slice.
    CHECK(run_capture(R"(
        module m;
            logic x; logic [1:0] y; logic z;
            initial begin {x, y, z} = 4'b10x1; $display("%b%b%b", x, y, z); end
        endmodule
    )") == "10x1\n");
}

// Nonblocking concat assignment samples the RHS once and commits each slice in
// the NBA region; the values are visible after the time step.
TEST_CASE("concatenation lvalue works for nonblocking assignment") {
    CHECK(run_capture(R"(
        module m;
            logic [3:0] hi, lo;
            initial begin {hi, lo} <= 8'hC3; #1 $display("%h%h", hi, lo); end
        endmodule
    )") == "c3\n");
}

// A concatenation operand may itself be a concatenation; the slice recurses.
TEST_CASE("nested concatenation lvalue recurses") {
    CHECK(run_capture(R"(
        module m;
            logic [1:0] a, b; logic [3:0] c;
            initial begin { {a, b}, c } = 8'hD7; $display("%h%h%h", a, b, c); end
        endmodule
    )") == "317\n");
}

// The picorv32 RISC-V J-immediate swizzle (picorv32.v:880): one assignment
// scatters a sign-extended source across non-contiguous fields of one register.
// Reference output cross-checked against iverilog.
TEST_CASE("concatenation lvalue lowers the picorv32 J-immediate swizzle") {
    CHECK(run_capture(R"(
        module m;
            reg [31:0] decoded_imm_j;
            initial begin
                { decoded_imm_j[31:20], decoded_imm_j[10:1], decoded_imm_j[11],
                  decoded_imm_j[19:12], decoded_imm_j[0] } = $signed({20'hABCDE, 1'b0});
                $display("%h", decoded_imm_j);
            end
        endmodule
    )") == "fffde2bc\n");
}

// Indexed part-selects with a runtime base (`[i +: w]` / `[i -: w]`) lower to a
// variable shift + mask over the existing dynamic-offset lvalue path. Read and
// write, both directions; cross-checked against iverilog.
TEST_CASE("dynamic indexed part-select read and write") {
    const std::string out = run_capture(R"(
        module m;
            reg [31:0] x;
            reg [7:0]  r;
            integer i;
            initial begin
                x = 32'hAABBCCDD;
                for (i = 0; i < 4; i = i + 1) begin
                    r = x[i*8 +: 8];
                    $display("up%0d=%02x", i, r);
                end
                $display("dn=%02x", x[31 -: 8]);
                for (i = 0; i < 4; i = i + 1)
                    x[i*8 +: 8] = i;
                $display("wr=%08x", x);
                x = 32'h0;
                x[15 -: 8] = 8'hEF;
                $display("wrdn=%08x", x);
            end
        endmodule
    )");
    CHECK(out == "up0=dd\nup1=cc\nup2=bb\nup3=aa\ndn=aa\nwr=03020100\nwrdn=0000ef00\n");
}

// The picorv32 fast-multiplier carry chain: a runtime-base indexed part-select on
// both sides of an assignment inside a bounded procedural loop (picorv32.v:2264).
// A constant CARRY_CHAIN keeps the loop unrollable so the selects fold, but the
// frontend lowers them dynamically regardless.
TEST_CASE("dynamic part-select carry-chain accumulate") {
    const std::string out = run_capture(R"(
        module m;
            localparam CARRY_CHAIN = 4;
            reg [15:0] a, b_, sum;
            integer j;
            initial begin
                a = 16'h1234; b_ = 16'h5678; sum = 0;
                for (j = 0; j < 16; j = j + CARRY_CHAIN)
                    sum[j +: CARRY_CHAIN] = a[j +: CARRY_CHAIN] + b_[j +: CARRY_CHAIN];
                $display("%04x", sum);
            end
        endmodule
    )");
    CHECK(out == "68ac\n");
}

// A module variable's declaration initializer (LRM 6.8) sets the value at time
// zero, before any procedure runs; it is not dropped (regression) nor modeled as
// a continuous assignment. Reading it from an initial block sees the value.
TEST_CASE("module variable declaration initializers apply at time zero") {
    const std::string out = run_capture(R"(
        module m;
            reg [7:0] v = 8'hA5;
            int n = 42;
            initial $display("%h %0d", v, n);
        endmodule
    )");
    CHECK(out == "a5 42\n");
}

// The core decl-initializer-vs-edge bug: a clock with a declaration initializer
// (`reg clk = 0`) must start at 0 so the toggling `always` produces real posedges.
// Before the fix clk stayed X, ~X stayed X, and the posedge block never fired. The
// clock is driven from an initial (bounded) so the design drains without $finish.
TEST_CASE("declaration-initialized clock produces posedges") {
    const std::string out = run_capture(R"(
        module m;
            reg clk = 0;
            always @(posedge clk) $display("posedge %0d", $time);
            initial repeat (6) #5 clk = ~clk;
        endmodule
    )");
    CHECK(out == "posedge 5\nposedge 15\nposedge 25\n");
}

// Declaration initializers must apply even when the design has more than one top
// module (regression). finalize_preinit ran only for a top with an empty prefix,
// but a multi-top design prefixes every top with "<name>.", so the pre-init
// process was never emitted and every initializer was dropped. This is the
// picorv32 AXI-testbench stall: unreferenced tops (picorv32_wb, ...) made the
// design multi-top, so axi4_memory's `async_axi_transaction = ~0` came up X.
TEST_CASE("declaration initializers apply with multiple top modules") {
    const std::string out = run_capture(R"(
        module other;
        endmodule
        module m;
            reg [4:0] x = ~0;
            initial $display("%b", x);
        endmodule
    )");
    CHECK(out == "11111\n");
}

// A declaration initializer must propagate to continuously-active consumers at
// time zero (regression). A continuous assign / port copy / always_comb subscribes
// to its sources but is event-driven; when a source is written only by the
// non-servicing pre-init pass it generates no edge, so the consumer must instead
// self-start (run once at time zero, LRM 10.3 / 9.2.2.2). Before the fix `y` and
// the child's reset input stayed X, which is what left the native picorv32 core in
// reset with mem_valid = X. Covers a continuous assign, a module-port copy, and the
// posedge reset that reads the copied value.
TEST_CASE("declaration initializers propagate to continuous consumers at time zero") {
    const std::string out = run_capture(R"(
        module dut(input clk, input rstn, output reg valid);
            always @(posedge clk) valid <= rstn ? 1'b1 : 1'b0;
        endmodule
        module m;
            reg  en = 0;
            wire y = en;                 // continuous assign of a decl-init reg
            reg  rstn = 0;               // reaches dut through a port copy
            reg  clk = 0;
            wire dvalid;
            dut u(.clk(clk), .rstn(rstn), .valid(dvalid));
            always #5 clk = ~clk;
            initial begin
                #12 $display("y=%b dvalid=%b", y, dvalid);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "y=0 dvalid=0\n");
}

// A net declaration assignment (`wire w = a & b`) is a continuous assignment
// (LRM 6.7.1), re-evaluated when its source changes — not a one-shot init and not
// silently dropped (regression: it previously left the net at X).
TEST_CASE("net declaration assignment is continuous") {
    const std::string out = run_capture(R"(
        module m;
            logic a, b;
            wire w = a & b;
            initial begin
                a = 1; b = 0; #1 $display("%b", w);
                b = 1;        #1 $display("%b", w);
            end
        endmodule
    )");
    CHECK(out == "0\n1\n");
}

// Write `text` to `path` for a $readmem test, replacing any existing file.
void write_file(const char* path, std::string_view text) {
    std::FILE* f = std::fopen(path, "w");
    REQUIRE(f != nullptr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

// $readmemh fills the memory from the file, honoring @address jumps and skipping
// // and /* */ comments; untouched words keep their X startup value (LRM §21.4).
// Cross-checked against Icarus Verilog.
TEST_CASE("$readmemh loads a memory with @address and comments") {
    const char* path = "clotho_readmemh_test.hex";
    write_file(path, "// header comment\nDEADBEEF 01234567\n@4 /* jump */ AABBCCDD\n");
    const std::string src = std::string(R"(
        module m;
            reg [31:0] mem [0:5];
            integer k;
            initial begin
                $readmemh(")") + path + R"(", mem);
                for (k = 0; k < 6; k = k + 1) $display("%08x", mem[k]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "deadbeef\n01234567\nxxxxxxxx\nxxxxxxxx\naabbccdd\nxxxxxxxx\n");
    std::remove(path);
}

// $readmemb parses binary digits and x/z; a runtime-string filename (a reg holding
// packed ASCII, the picorv32 pattern) resolves to the path at run time.
TEST_CASE("$readmemb parses x/z and a runtime-string filename") {
    const char* path = "clotho_readmemb_test.bin";
    write_file(path, "1010xx01\nzzzz0000\n");
    const std::string src = std::string(R"(
        module m;
            reg [7:0] mem [0:2];
            reg [255:0] fname;
            initial begin
                fname = ")") + path + R"(";
                $readmemb(fname, mem);
                $display("%b %b %b", mem[0], mem[1], mem[2]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1010xx01 zzzz0000 xxxxxxxx\n");
    std::remove(path);
}

// A string-variable filename (LRM §21.4: the filename may be a string data type)
// resolves through the rt_string handle at run time; the bytes are in natural
// order, unlike the reversed packed-ASCII form. Cross-checked against Icarus
// Verilog. Regression: this previously crashed the compiler (null value plane).
TEST_CASE("$readmemh accepts a string-variable filename") {
    const char* path = "clotho_readmemh_strvar_test.hex";
    write_file(path, "de ad be ef\n");
    const std::string src = std::string(R"(
        module m;
            logic [7:0] mem [0:3];
            string fname;
            initial begin
                fname = ")") + path + R"(";
                $readmemh(fname, mem);
                $display("%h %h %h %h", mem[0], mem[1], mem[2], mem[3]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "de ad be ef\n");
    std::remove(path);
}

// A string-expression filename (a concatenation) is also legal per LRM §21.4:
// "The filename is an expression that is a string literal, string data type, or
// an integral data type". Cross-checked against Icarus Verilog.
TEST_CASE("$readmemh accepts a string-expression filename") {
    const char* path = "clotho_readmemh_strexpr_test.hex";
    write_file(path, "12 34\n");
    const std::string src = R"(
        module m;
            logic [7:0] mem [0:1];
            string base;
            initial begin
                base = "clotho_readmemh_strexpr_test";
                $readmemh({base, ".hex"}, mem);
                $display("%h %h", mem[0], mem[1]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "12 34\n");
    std::remove(path);
}

// Default load fills from the lowest address upward regardless of the declared
// range direction; a descending range [3:0] therefore maps the first datum to
// address 0 (storage slot 3). This is the LRM §21.4 rule, which Icarus follows and
// which differs from a naive storage-order fill.
TEST_CASE("$readmemh default fill honors a descending declared range") {
    const char* path = "clotho_readmem_desc_test.hex";
    write_file(path, "F0 0F C3\n");
    const std::string src = std::string(R"(
        module m;
            reg [7:0] mem [3:0];
            initial begin
                $readmemh(")") + path + R"(", mem);
                $display("%h %h %h %h", mem[3], mem[2], mem[1], mem[0]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "xx c3 0f f0\n");
    std::remove(path);
}

// -- Z elision --
//
// Signals the may-z analysis proves z-free store no z word (%logic.nz.W slots);
// signals that can carry z keep the full slot and must round-trip z bit-exact.

TEST_CASE("a z-carrying signal keeps its z word and round-trips z through copies") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] v = 4'bz01x;
            logic [3:0] y;
            logic [3:0] clean = 4'b1010;
            assign y = v;
            initial begin
                #1 $display("%b %b %b", v, y, clean);
                $display("%b %b", v === 4'bz01x, y === 4'bz01x);
                casez (y)
                    4'bz01?: $display("casez hit");
                    default: $display("casez miss");
                endcase
            end
        endmodule
    )";
    const std::string ir = emit(src);
    // v and y carry z (full slot); clean is proven never-z (elided slot).
    CHECK(ir.find("%logic.4 = type { i8, i4 }") != std::string::npos);
    CHECK(ir.find("%logic.nz.4 = type { i8 }") != std::string::npos);
    CHECK(run_capture(src) == "z01x z01x 1010\n1 1\ncasez hit\n");
}

TEST_CASE("NBA of a z value round-trips the shadow commit; never-z NBAs use the .noz bucket") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0;
            logic [3:0] zq;
            logic [3:0] nq;
            logic [3:0] zsrc = 4'bzz01;
            always @(posedge clk) begin
                zq <= zsrc;
                nq <= nq === 4'bxxxx ? 4'd5 : nq + 1;
            end
            initial begin
                #1 clk = 1;
                #1 $display("%b %b", zq, nq);
                $finish;
            end
        endmodule
    )";
    const std::string ir = emit(src);
    // Same width, different z-ness: two distinct commit buckets.
    CHECK(ir.find("clotho.nba.w4.full = ") != std::string::npos);
    CHECK(ir.find("clotho.nba.w4.full.noz = ") != std::string::npos);
    CHECK(run_capture(src) == "zz01 0101\n");
}

TEST_CASE("partial NBA merges z bits into a z-carrying target") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0;
            logic [3:0] q = 4'b0000;
            always @(posedge clk) q[2:1] <= 2'bz0;
            initial begin
                #1 clk = 1;
                #1 $display("%b", q);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0z00\n");
}

TEST_CASE("array NBA works against a never-z memory and keeps z in a 4-state one") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0;
            bit   [7:0] mem2 [0:1];
            logic [7:0] mem4 [0:1];
            always @(posedge clk) begin
                mem2[0] <= 8'hA5;
                mem4[1] <= 8'bzz01zz10;
            end
            initial begin
                #1 clk = 1;
                #1 $display("%h %h %b", mem2[0], mem2[1], mem4[1]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a5 00 zz01zz10\n");
}

TEST_CASE("$readmem with z digits into a 2-state memory reads 0 and spares neighbors") {
    const char* path = "clotho_readmem_2state_z.hex";
    write_file(path, "zz 5a z1\n");
    const std::string src = std::string(R"(
        module m;
            bit [7:0] mem [0:2];
            initial begin
                $readmemh(")") + path + R"(", mem);
                $display("%h %h %h", mem[0], mem[1], mem[2]);
                $finish;
            end
        endmodule
    )";
    // z reads as 0 in a 2-state memory (elided z plane, null z_base); the
    // neighboring elements' must|may words must be untouched by the skipped
    // z writes.
    CHECK(run_capture(src) == "00 5a 01\n");
    std::remove(path);
}

TEST_CASE("a task output formal carries z back to the caller") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] zdst;
            logic [3:0] cdst;
            task automatic zt(output logic [3:0] o);
                o = 4'bz111;
            endtask
            task automatic ct(input logic [3:0] a, output logic [3:0] o);
                o = a + 1;
            endtask
            initial begin
                zt(zdst);
                ct(4'd6, cdst);
                $display("%b %d", zdst, cdst);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "z111 7\n");
}

TEST_CASE("a function returning z feeds a z-capable target") {
    constexpr std::string_view src = R"(
        module m;
            function automatic logic [3:0] zfn(input logic [3:0] x);
                return { 2'bz0, x[1:0] };
            endfunction
            logic [3:0] r;
            initial begin
                r = zfn(4'b0011);
                $display("%b", r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "z011\n");
}

TEST_CASE("--no-z-elision keeps every slot's z word") {
    constexpr std::string_view src = R"(
        module m(input bit [3:0] x, output bit [3:0] y);
            initial y = x + 1;
        endmodule
    )";
    const LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*r.module, os, { .optimize = false, .z_elision = false }));
    CHECK(text.find("%bit.4 = type { i8, i4 }") != std::string::npos);
    CHECK(text.find(".nz.") == std::string::npos);
}

TEST_CASE("an undriven 4-state net powers up z, an undriven variable x (LRM 6.6)") {
    constexpr std::string_view src = R"(
        module m;
            wire  [3:0] w;
            logic [3:0] v;
            wire  eqz = w === 4'bzzzz;
            initial #1 $display("%b %b %b", w, v, eqz);
        endmodule
    )";
    // Verified against iverilog: undriven net z, undriven variable x.
    CHECK(run_capture(src) == "zzzz xxxx 1\n");
}

TEST_CASE("undriven top-level input ports read z") {
    constexpr std::string_view src = R"(
        module m(input [3:0] a, input [3:0] b);
            wire r = a === b;
            initial #1 $display("%b %b", a, r);
        endmodule
    )";
    CHECK(run_capture(src) == "zzzz 1\n");
}

TEST_CASE("an unconnected child input port reads z inside the child") {
    constexpr std::string_view src = R"(
        module child(input [3:0] i);
            initial #1 $display("%b", i);
        endmodule
        module top;
            child c();
        endmodule
    )";
    CHECK(run_capture(src) == "zzzz\n");
}

TEST_CASE("non-wire net kinds fail loudly") {
    const LowerResult r = lower_text(R"(
        module m;
            wand w;
        endmodule
    )");
    CHECK(!r.ok);
    CHECK(r.diagnostics.find("unsupported net type") != std::string::npos);
}

// The frontend cannot produce string IR yet, so build a module by hand: a
// string signal assigned a literal, then len / toupper / getc observed through
// $display. Exercises the string slot layout, startup init / teardown deinit,
// temps, and the rt_string_* call lowering end to end.
TEST_CASE("backend lowers string signals, constants, and string ops (hand-built IR)") {
    using namespace clotho::ir;

    auto mod  = std::make_unique<Module>();
    mod->name = "m";
    const Signal s =
      mod->add_signal({ .name = "s", .type = mod->pool.string_type, .dir = SignalInfo::Direction::Internal });

    Builder         b(mod->pool);
    const SourceLoc loc{ 1 };
    const auto      chars = std::string_view{ "Hello" };
    const Ref       lit   = mod->pool.put_value(
      mod->pool.string_type, { reinterpret_cast<const uint8_t*>(chars.data()), chars.size() });
    const Ref int_ty =
      mod->pool.put(Type{ .kind = Type::Kind::PackedArray, .is_signed = true, .element = mod->pool.bit_type,
                          .left = 31, .right = 0 });

    const Index sig = b.signal(s, loc);
    const Index k   = b.constant(lit, loc);
    b.binary(Inst::Tag::Store, sig, k, loc);
    const Index v   = b.unary(Inst::Tag::Load, sig, loc);
    const Index len = b.unary(Inst::Tag::StrLen, v, loc);
    b.display("%0d", true, std::span<const Index>{ &len, 1 }, loc);
    const Index up  = b.unary(Inst::Tag::StrUpper, v, loc);
    const Index idx = b.constant(mod->pool.put_int(int_ty, 4), loc);
    const Index ch  = b.binary(Inst::Tag::StrGetc, up, idx, loc);
    b.display("%c", true, std::span<const Index>{ &ch, 1 }, loc);

    mod->add_process(Process{ .activation = Activation::Initial, .ir = b.finish() });

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));
    run_jit(*mod);
    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[64];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    CHECK(out == "5\nO\n");
}
