#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Builder.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "ir/Module.h"

using namespace clotho;

namespace {

// Parameter list of every `define` whose symbol contains `needle`, in emission
// order. Lets a case assert the ABI SHAPE of a set of functions without pinning
// LLVM's uniquing suffixes, which shift when unrelated functions are added.
std::vector<std::string> define_params(const std::string& ir, std::string_view needle) {
    std::vector<std::string> out;
    for (size_t p = ir.find("define "); p != std::string::npos; p = ir.find("define ", p + 1)) {
        const std::string line = ir.substr(p, ir.find('\n', p) - p);
        const size_t      open = line.find('(');
        const size_t      close = line.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) {
            continue;
        }
        if (line.substr(0, open).find(needle) != std::string::npos) {
            out.push_back(line.substr(open, close - open + 1));
        }
    }
    return out;
}

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

// Emit with an explicit config (e.g. {.thunk = false} to force the coroutine
// form for structural assertions on both lowerings).
std::string emit(std::string_view source, ir::SimulationConfig config) {
    const LowerResult result = lower_text(source);
    REQUIRE(result.ok);
    REQUIRE(result.module != nullptr);

    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*result.module, os, config));
    return text;
}

// Emit without the optimization pipeline: for assertions on the backend's own
// emission structure (block labels, typed GEPs), which the pipeline is free to
// rewrite.
std::string emit_raw(std::string_view source, ir::SimulationConfig config = { .lower_coro = false }) {
    const LowerResult result = lower_text(source);
    REQUIRE(result.ok);
    REQUIRE(result.module != nullptr);

    std::string              text;
    llvm::raw_string_ostream os(text);
    config.lower_coro = false;
    REQUIRE(ir::emit_llvm(*result.module, os, config));
    return text;
}

// JIT-run a module and return everything its initial blocks wrote to stdout.
// The config lets a test A/B the same design across lowerings (e.g. thunk vs
// --no-thunk) and assert byte-identical output.
std::string run_capture(std::string_view source, ir::SimulationConfig config = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module, nullptr, config);

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

// Run a design and capture what it writes to stderr — used to observe the
// $finish / $stop exit banner (LRM Table 20-1), which is a simulator diagnostic
// routed to stderr, not the design's stdout.
std::string run_capture_stderr(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));

    ir::run_jit(*r.module);

    std::fflush(stderr);
    dup2(saved, fileno(stderr));
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
    const std::string ir  = emit(kAlu);
    const std::string raw = emit_raw(kAlu);

    // State is typed: the module definition gets a named state struct, the root
    // %clotho.state composes it, and it lives as one internal global whose
    // initializer is the whole power-on state (X = 0xFF00 for a 4-state byte),
    // so main emits no rt_alloc and no per-signal init stores.
    CHECK(raw.find("%alu.state = type") != std::string::npos);
    CHECK(raw.find("@clotho.state = internal global") != std::string::npos);
    CHECK(raw.find("i16 -256") != std::string::npos);

    // One process per @<module>.p<index>: two combinational assigns + one
    // sequential always block. All three are suspension-free, so by default they
    // lower to plain per-activation thunks — void, taking the state ptr plus the
    // timing-wheel ptr, no coroutine. Under --no-thunk each is instead a
    // switched-resume coroutine ramp returning the handle (CoroSplit then emits
    // .resume/.destroy/.cleanup parts, which don't match "define ptr @alu.p").
    CHECK(count_substr(ir, "define void @clotho.inst.alu.p") == 3);
    // Definitions only: the root module's seed loop re-declares each thunk
    // (split emission), and declarations print without argument names.
    CHECK(count_substr(ir, ".thunk(ptr noalias %state") == 3);
    CHECK(ir.find("(ptr noalias %state, ptr %wheel)") != std::string::npos);
    const std::string co = emit(kAlu, { .thunk = false });
    CHECK(count_substr(co, "define ptr @clotho.inst.alu.p") == 3);
    CHECK(co.find("(ptr %state, ptr %wheel)") != std::string::npos);

    // Datapath ops present.
    CHECK(ir.find(" add ") != std::string::npos);
    CHECK(ir.find(" sub ") != std::string::npos);

    // Signal access via constant typed GEPs, anchored at the owning instance
    // scope's own state global: def-relative (%alu.state) off
    // @clotho.inst.alu.state, folded link-time constant (asserted on raw IR;
    // instcombine canonicalizes GEP shapes).
    CHECK(raw.find("%alu.state, ptr @clotho.inst.alu.state") != std::string::npos);
    CHECK(ir.find("store i16") != std::string::npos);

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
    const std::string          raw = emit_raw(src);

    // Every signal keeps its LLVM slot type, named per (domain, width) — and
    // the module's state struct lists them in declaration order: x, y
    // (bit[3:0]) then z (logic[7:0]). The 2-state signals are decoupled from the
    // packed-word paths (no ABI / NBA / array), so they get the genuine single-iW
    // slot `%bit.4 = { i4 }`; the 4-state z keeps the i(2W) must|may word.
    // Named-type layout survives only in the raw IR (the optimizer drops it).
    CHECK(raw.find("%m.state = type { %bit.4, %bit.4, %logic.nz.8 }") != std::string::npos);
    CHECK(raw.find("%bit.4 = type { i4 }") != std::string::npos);
    CHECK(raw.find("%logic.nz.8 = type { i16 }") != std::string::npos);
    CHECK(raw.find("@clotho.state = internal global") != std::string::npos);
    // z's must|may word is an i16 (2W = 16).
    CHECK(ir.find("store i16") != std::string::npos);
    CHECK(ir.find("define ptr @clotho.inst.m.p") != std::string::npos);
}

TEST_CASE("instance state structs replicate per instantiation as per-scope globals") {
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
    const std::string          raw = emit_raw(src);

    // One state struct per module definition holding its OWN signals only;
    // each instance scope owns a separate state global of that type (two
    // %leaf.state globals for the two instances), and the standalone NBA
    // shadow global carries the staging fields. No z sources → every slot
    // elided. Named-type layout survives only in the raw IR (the optimizer
    // drops it).
    CHECK(raw.find("%leaf.state = type { %logic.nz.1, %logic.nz.8, %logic.nz.8 }") != std::string::npos);
    CHECK(raw.find("%top.state = type { %logic.nz.1, %logic.nz.8, %logic.nz.8 }") != std::string::npos);
    // Referenced from the definition-unit modules, so the split emission
    // promotes them out of internal linkage (plain external `global`).
    CHECK(raw.find("@clotho.inst.top.state = dso_local global %top.state") != std::string::npos);
    CHECK(raw.find("@clotho.inst.top.u0.state = dso_local global %leaf.state") != std::string::npos);
    CHECK(raw.find("@clotho.inst.top.u1.state = dso_local global %leaf.state") != std::string::npos);
    CHECK(raw.find("@clotho.inst.top.u0.shadow = internal global %leaf.state.shadow") != std::string::npos);
    CHECK(raw.find("@clotho.inst.top.u1.shadow = internal global %leaf.state.shadow") != std::string::npos);
    // Exactly one definition of the leaf struct type (deduplicated, no .0 clone).
    CHECK(raw.find("%leaf.state.0") == std::string::npos);
}

// Regression: two or more self-starting continuous assigns share the nproc-sized
// active queue. An initial block drains first (LIFO) and its input writes service
// the assigns' still-unrun static subscriptions; without marking them scheduled at
// seed time each is re-pushed, overflowing the queue and corrupting a handle.
TEST_CASE("multiple continuous assigns woken by a store do not overflow the queue") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, c, d, y1, y2;
            assign y1 = a & b;
            assign y2 = c | d;
            initial begin
                a = 1; b = 1; c = 0; d = 0;
                #1 $display("%b %b", y1, y2);
                a = 0; d = 1;
                #1 $display("%b %b", y1, y2);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 0\n0 1\n");
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
    const std::string          ir  = emit_raw(src);

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
    const std::string ir = emit_raw(R"(
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

// Signed ** covers LRM Table 11-4: signed base at a non-negative exponent (the
// squaring loop is sign-agnostic) and the negative-exponent corners (op1==0 -> X,
// op1==1 -> 1, op1==-1 -> parity of op2, |op1|>1 -> 0). Values match iverilog 13.
TEST_CASE("signed pow agrees with LRM Table 11-4") {
    constexpr std::string_view src = R"(
        module m;
            integer a, e, r;
            task show(input integer base, input integer exp);
                begin a = base; e = exp; r = a ** e; $display("%0d", r); end
            endtask
            initial begin
                show(-3, 3);   // -27  signed base, positive exponent
                show(-2, 2);   // 4
                show(-3, 0);   // 1    exp 0 -> 1
                show(2, -2);   // 0    |op1|>1, negative exp
                show(1, -5);   // 1
                show(-1, -7);  // -1   op1==-1, odd exp
                show(-1, -8);  // 1    op1==-1, even exp
                show(-1, 7);   // -1
                show(0, -1);   // x    op1==0, negative exp
                show(5, -1);   // 0
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-27\n4\n1\n0\n1\n-1\n1\n-1\nx\n0\n");
}

// A constant exponent lets the optimizer fold the squaring loop; a bare literal
// exponent is 2-state, so with a 4-state base the operands are mixed-domain. The
// backend must lift the exponent to 4-state or the X-mask shifts an iW word by its
// full width (poison), which the optimizer turns into an unreachable process body.
// Regression: `x ** 3` and an unsigned constant exponent both once produced no
// output. 2-state `int` operands exercise the recursion + wide-multiply path.
TEST_CASE("pow with a constant / mixed-domain exponent is not miscompiled") {
    constexpr std::string_view src = R"(
        module m;
            integer a4, r4;
            reg [31:0] u;
            int ai, ei, ri;
            initial begin
                a4 = -3;     r4 = a4 ** 3;      $display("%0d", r4);  // -27, literal exp (2-state)
                r4 = (-3) ** 3;                 $display("%0d", r4);  // -27, both constant
                u = 32'd3;   u = u ** 32'd3;    $display("%0d", u);   // 27,  unsigned const exp
                ai = -4; ei = 3; ri = ai ** ei; $display("%0d", ri);  // -64, 2-state signed
                ai = 2;  ei = -3; ri = ai ** ei; $display("%0d", ri); // 0,   2-state neg exp
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-27\n-27\n27\n-64\n0\n");
}

// The x-producing corner of Table 11-4 (0 ** negative) is coerced to 0 when the
// result type is 2-state (LRM 6.3.1) but kept as x in a 4-state result. iverilog
// keeps x even for a 2-state `int` here, an inconsistency with its own plain
// x-assignment coercion; Clotho follows the LRM.
TEST_CASE("0 ** negative coerces X to 0 in a 2-state result") {
    CHECK(run_capture(R"(module m; int r; integer q;
        initial begin r = 0 ** -1; $display("%0d", r); q = 0 ** -1; $display("%0d", q); end
        endmodule)") == "0\nx\n");
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

TEST_CASE("wide == / != are X only when ambiguous (LRM 11.4.5)") {
    // 11.4.5: x/z bits yield X only if the relation is ambiguous. A position
    // where both operands are known and unequal decides the result (== 0, != 1)
    // regardless of the unknown bits; only "known bits all equal + some x/z"
    // is X. All lines byte-match iverilog.
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                a = 8'b10000000; b = 8'b0000000x; $display("%b%b", a == b, a != b);
                a = 8'b0000100x; b = 8'b00000001; $display("%b%b", a == b, a != b);
                a = 8'b1000000x; b = 8'b10000001; $display("%b%b", a == b, a != b);
                a = 8'b000000x1; b = 8'b00000001; $display("%b%b", a == b, a != b);
                a = 8'b0000zz11; b = 8'b00001011; $display("%b%b", a == b, a != b);
                a = 8'h5a;       b = 8'h5a;       $display("%b%b", a == b, a != b);
                a = 8'h5a;       b = 8'h5b;       $display("%b%b", a == b, a != b);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "01\n"  // bit 7 known-unequal: definite, despite b's x
          "01\n"  // bit 3 known-unequal: definite, despite a's x
          "xx\n"  // known bits equal, bit 0 unknown: ambiguous
          "xx\n"  // known bits equal, bit 1 unknown: ambiguous
          "xx\n"  // z counts as unknown; known bits equal: ambiguous
          "10\n"  // known equal
          "01\n"); // known unequal
}

TEST_CASE("=== / !== stay bit-exact where == is definite or ambiguous") {
    // 11.4.5's ambiguity rule does not apply to case equality: x/z compare
    // bit-for-bit and the result is always known. Operands mirror the == test
    // above so a regression that reroutes === through the sharpened == fails.
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                a = 8'b10000000; b = 8'b0000000x; $display("%b%b", a === b, a !== b);
                a = 8'b1000000x; b = 8'b10000001; $display("%b%b", a === b, a !== b);
                a = 8'b1000000x; b = 8'b1000000x; $display("%b%b", a === b, a !== b);
            end
        endmodule
    )";
    // vs iverilog.
    CHECK(run_capture(src) == "01\n01\n10\n");
}

TEST_CASE("==? kept positions follow the sharpened == (LRM 11.4.6)") {
    // Non-wildcard positions compare "as for the logical equality" operator, so
    // a definite mismatch on a kept bit beats a left-operand x elsewhere.
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                a = 8'b10000000; b = 8'b0000000x; $display("%b%b", a ==? b, a !=? b);
                a = 8'b1000000x; b = 8'b00000001; $display("%b%b", a ==? b, a !=? b);
                a = 8'b1000000x; b = 8'b10000001; $display("%b%b", a ==? b, a !=? b);
            end
        endmodule
    )";
    // Line 1: bit 0 wildcarded, bit 7 known-unequal -> definite 0/1.
    // Line 2: no wildcards; bit 7 known-unequal decides despite a's x -> 0/1.
    // Line 3: no wildcards; known bits equal, a bit 0 x -> ambiguous. vs iverilog.
    CHECK(run_capture(src) == "01\n01\nxx\n");
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

// -> / <-> (LRM 11.4.7): a -> b == !a || b, a <-> b == (a->b) && (b->a).
// Verified against iverilog -- no side effects here, so the short-circuit vs.
// eager evaluation difference (see function_test.cpp) does not affect the
// computed value, only whether a side effect runs.
TEST_CASE("-> and <-> agree with LRM Kleene semantics") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b;
            initial begin
                a = 1'b0; b = 1'b0; $display("%b%b", a->b, a<->b);
                a = 1'b0; b = 1'b1; $display("%b%b", a->b, a<->b);
                a = 1'b1; b = 1'b0; $display("%b%b", a->b, a<->b);
                a = 1'b1; b = 1'b1; $display("%b%b", a->b, a<->b);
                a = 1'bx; b = 1'b1; $display("%b%b", a->b, a<->b);
                a = 1'bx; b = 1'b0; $display("%b%b", a->b, a<->b);
                a = 1'bx; b = 1'bx; $display("%b%b", a->b, a<->b);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "11\n10\n00\n11\n1x\nxx\nxx\n");
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
    const std::string ir = emit_raw(R"(
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

    // #20 lowers to: schedule a wheel timer relative to now (one ABI crossing, the
    // runtime adds `now`), and suspend. The coroutine intrinsics themselves are
    // lowered away by CoroSplit; the runtime calls are what remain.
    CHECK(ir.find("call i64 @rt_wheel_now") == std::string::npos);
    CHECK(ir.find("call void @rt_wheel_schedule_rel") != std::string::npos);
    // The timer callback that wakes the suspended coroutine by pushing its handle
    // onto the active queue.
    CHECK(ir.find("define internal void @clotho.enqueue_timer") != std::string::npos);
    // main drains the wheel: advance while timers are pending (the pending-check
    // is folded into the tick, one ABI crossing per timestep).
    CHECK(ir.find("call i32 @rt_wheel_advance") != std::string::npos);
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
    // Root-defined, promoted to external (dso_local) linkage where the
    // definition-unit module's bodies reference them (split emission).
    CHECK(ir.find("@clotho.wait.clk.sta.pos = dso_local global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.d.sta.any = dso_local global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.q.sta.any = dso_local global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.doubled.dyn.any = dso_local global") != std::string::npos);
    CHECK(ir.find("@clotho.wait.doubled.dyn.any.len = dso_local global") != std::string::npos);
    CHECK(ir.find("@clotho.proc_sched = internal global") != std::string::npos);
    // Both always blocks are suspension-free, so by default they lower to plain
    // per-activation thunks (no forever wait loop); servicing still has a static
    // and a dynamic scan from the blocking stores. The always-loop coroutine
    // form appears under --no-thunk. Labels asserted on raw IR (simplifycfg
    // merges and renames blocks).
    const std::string raw = emit_raw(src);
    CHECK(raw.find("sta.head") != std::string::npos);
    CHECK(raw.find("dyn.head") != std::string::npos);
    CHECK(count_substr(raw, ".thunk(ptr noalias %state") == 2);
    CHECK(raw.find("always.loop") == std::string::npos);
    CHECK(emit_raw(src, { .thunk = false }).find("always.loop") != std::string::npos);
}

TEST_CASE("bare initial @(event) blocks; it does not run at time zero") {
    // A top-level event control on an `initial` is a one-shot wait, not process
    // sensitivity (LRM 9.4): with no driver ever producing the edge the body
    // must never run. Regression: the wait used to be hoisted into the
    // process's sensitivity list and discarded, firing the body at time zero.
    constexpr std::string_view src = R"(
        module m;
            reg s = 1;
            initial @(negedge s) $display("fired");
            initial begin #10; $display("end at %0t", $time); $finish; end
        endmodule
    )";
    // vs iverilog: "fired" never appears.
    CHECK(run_capture(src) == "end at 10\n");
}

TEST_CASE("bare initial @(event) fires once when the edge arrives") {
    constexpr std::string_view src = R"(
        module m;
            reg s = 1;
            initial @(negedge s) $display("fired at %0t", $time);
            initial begin #3 s = 0; #10 $finish; end
        endmodule
    )";
    // vs iverilog.
    CHECK(run_capture(src) == "fired at 3\n");
}

TEST_CASE("bare initial @(posedge clk) waits for the first edge only") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0;
            always #5 clk = ~clk;
            initial @(posedge clk) $display("posedge at %0t", $time);
            initial #23 $finish;
        endmodule
    )";
    // One shot: fires at the first posedge (t=5), not again at t=15. vs iverilog.
    CHECK(run_capture(src) == "posedge at 5\n");
}

TEST_CASE("bare initial #delay defers the statement (not hoisted)") {
    // The sibling of the @(event) case: a top-level delay on an `initial`
    // already lowered intra-body; pin that down.
    constexpr std::string_view src = R"(
        module m;
            initial #5 $display("at %0t", $time);
            initial #10 $finish;
        endmodule
    )";
    // vs iverilog.
    CHECK(run_capture(src) == "at 5\n");
}

TEST_CASE("always @(posedge clk) sensitivity hoisting is unchanged") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0;
            reg [3:0] n = 0;
            always #5 clk = ~clk;
            always @(posedge clk) begin
                n = n + 1;
                $display("tick %0d at %0t", n, $time);
            end
            initial #23 $finish;
        endmodule
    )";
    // vs iverilog.
    CHECK(run_capture(src) == "tick 1 at 5\ntick 2 at 15\n");
}

TEST_CASE("$finish in a process lowers to a cooperative rt_finish_coop call") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                $display("done");
                $finish;
            end
        endmodule
    )";
    const std::string          ir  = emit(src);

    // rt_finish_coop(banner_ptr, now, level); omitted argument defaults to 1.
    // A process body takes the cooperative form, which returns so the body can
    // leave through its own epilogue (docs/design/cooperative-finish.md); the
    // noreturn rt_finish is for sites that cannot yet unwind that way.
    CHECK(ir.find("@rt_finish_coop(") != std::string::npos);
    CHECK(ir.find(", i32 1)") != std::string::npos);
    // ...and only that form: the noreturn rt_finish must not appear, or the
    // body would jump out over its own frame instead of returning through it.
    CHECK(ir.find("@rt_finish(") == std::string::npos);
}

TEST_CASE("$stop lowers identically to $finish") {
    constexpr std::string_view src = R"(
        module m;
            initial $stop;
        endmodule
    )";
    const std::string          ir  = emit(src);

    CHECK(ir.find("@rt_finish_coop(") != std::string::npos);
}

TEST_CASE("$finish with a diagnostic-level argument passes the level through") {
    constexpr std::string_view src = R"(
        module m;
            initial $finish(2);
        endmodule
    )";
    const std::string          ir  = emit(src);

    // The 0/1/2 level reaches the runtime as the third argument (here 2).
    CHECK(ir.find("@rt_finish_coop(") != std::string::npos);
    CHECK(ir.find(", i32 2)") != std::string::npos);
}

// LRM §20.2 / Table 20-1: the default $finish (level 1) prints "simulation time
// and location" — verified vs iverilog, which prints a banner with the time and
// file:line. Clotho routes it to stderr (a diagnostic), so the design's stdout
// stays clean. The lower_text buffer is named "source"; the design has no
// timescale, so time 0 renders in the default 1 s precision.
TEST_CASE("$finish (default level) prints a time+location banner to stderr") {
    constexpr std::string_view src = R"(
        module m;
            initial $finish;
        endmodule
    )";
    const std::string err = run_capture_stderr(src);
    CHECK(err == "source:3: $finish called at 0 s\n");
}

// $stop shares the banner path but names itself in the text.
TEST_CASE("$stop prints its own banner") {
    constexpr std::string_view src = R"(
        module m;
            initial $stop;
        endmodule
    )";
    const std::string err = run_capture_stderr(src);
    CHECK(err == "source:3: $stop called at 0 s\n");
}

// Level 2 appends the resource line (LRM Table 20-1). The numbers are host
// state, so only the shape is pinned — but the shape is what carries the unit
// normalization: getrusage reports ru_maxrss in kilobytes on Linux and bytes on
// the BSDs (macOS), and Windows has no getrusage at all, so an unnormalized
// read would silently misreport by 1024x on one host family and not build on
// another.
TEST_CASE("$finish(2) appends a memory and CPU-time line") {
    constexpr std::string_view src = R"(
        module m;
            initial $finish(2);
        endmodule
    )";
    const std::string err = run_capture_stderr(src);
    CHECK(err.starts_with("source:3: $finish called at 0 s\n  Memory: "));
    CHECK(err.find(" KB  CPU time: ") != std::string::npos);
    CHECK(err.ends_with(" seconds\n"));
    // A live process has a nonzero peak resident set on every supported host;
    // a zero here means the query failed rather than that memory was free.
    const size_t mem = err.find("Memory: ") + 8;
    CHECK(std::stoull(err.substr(mem)) > 0);
}

// Level 0 prints nothing (LRM Table 20-1).
TEST_CASE("$finish(0) prints no banner") {
    constexpr std::string_view src = R"(
        module m;
            initial $finish(0);
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
}

// The banner reports the simulation time at the point of the call, rendered in
// the design's time precision.
TEST_CASE("$finish banner reports the current simulation time") {
    constexpr std::string_view src = R"(
        `timescale 1ns/1ps
        module m;
            initial #12 $finish;
        endmodule
    )";
    const std::string err = run_capture_stderr(src);
    CHECK(err == "source:4: $finish called at 12 ns\n");
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

// LRM §20.4: the optional argument selects another scope, whose timescale may
// differ from the caller's. Expected text verified against iverilog.
TEST_CASE("$printtimescale with a child instance reports that child's timescale") {
    const std::string out = run_capture(R"(`timescale 1us/1ns
        module sub;
        endmodule
        `timescale 1ns/1ps
        module top;
            sub u1();
            initial $printtimescale(u1);
        endmodule
    )");

    CHECK(out == "Time scale of (top.u1) is 1us / 1ns\n");
}

// The argument may name a scope above the caller, so the lookup is genuinely
// cross-hierarchy, not a downward walk. Verified against iverilog.
TEST_CASE("$printtimescale reaches a scope above the caller") {
    const std::string out = run_capture(R"(`timescale 1us/1ns
        module sub;
            initial $printtimescale(top);
        endmodule
        `timescale 1ns/1ps
        module top;
            sub u1();
        endmodule
    )");

    CHECK(out == "Time scale of (top) is 1ns / 1ps\n");
}

// The argument path comes from slang's hierarchical-path formatter while the
// no-argument path comes from Clotho's own flattened prefix. Naming the same
// generate-nested instance both ways pins the "[i]" formatting of both, and the
// two lines are identical, so the check does not depend on process order.
// Verified against iverilog.
TEST_CASE("$printtimescale spells a generate-nested instance the same way with and without an argument") {
    const std::string out = run_capture(R"(`timescale 1us/1ns
        module sub;
            initial $printtimescale;
        endmodule
        `timescale 1ns/1ps
        module top;
            genvar i;
            generate
                for (i = 0; i < 1; i = i + 1) begin : g
                    sub u();
                end
            endgenerate
            initial $printtimescale(g[0].u);
        endmodule
    )");

    CHECK(out == "Time scale of (top.g[0].u) is 1us / 1ns\n"
                 "Time scale of (top.g[0].u) is 1us / 1ns\n");
}

// A scope with no timescale reports the simulator default rather than failing.
// Clotho's unitless base is one second per tick, which is also what iverilog
// prints for this design.
TEST_CASE("$printtimescale reports the unitless base when the design sets no timescale") {
    const std::string out = run_capture(R"(
        module top;
            initial $printtimescale;
        endmodule
    )");

    CHECK(out == "Time scale of (top) is 1s / 1s\n");
}

// $unit and $root are the two non-instance scopes the argument accepts (LRM
// §20.4). The compilation unit carries the timeunit/timeprecision declared at
// file scope; $root declares none of its own, so it reports the unitless base.
// iverilog rejects both forms, so the text follows Clotho's own shape.
TEST_CASE("$printtimescale names the $unit and $root scopes") {
    const std::string out = run_capture(R"(timeunit 1ns;
        timeprecision 1ps;
        module top;
            initial begin
                $printtimescale($unit);
                $printtimescale($root);
            end
        endmodule
    )");

    CHECK(out == "Time scale of ($unit) is 1ns / 1ps\n"
                 "Time scale of ($root) is 1s / 1s\n");
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
    // (cnt). The two clk input-port copies share the identical sensitivity
    // (@top clk) and fuse into one unit (`.f2`); the clocked always blocks watch
    // per-instance clk locals and stay separate. 6 processes emit as 5 distinct
    // @top.p<N> units. All are suspension-free, so by default they are thunks
    // (the fused pair is `.f2.thunk`); under --no-thunk they are coroutine ramps.
    // Definitions only (split emission re-declares cross-module symbols;
    // declarations and calls print without argument names).
    CHECK(count_substr(ir, "define void @clotho.inst.top.") == 5);
    CHECK(count_substr(ir, ".thunk(ptr noalias %state") == 5);
    CHECK(count_substr(ir, ".f2.thunk(ptr noalias %state") == 1);
    const std::string co = emit(src, { .thunk = false });
    CHECK(count_substr(co, "define ptr @clotho.inst.top.") == 5);
    CHECK(count_substr(co, ".f2(ptr %state") == 1);
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

// $random's global stream is the IEEE 1800-2023 Annex N reference PRNG from an
// unseeded start (seed 0), so the sequence is fully determined. This is the only
// test in the suite that draws from it, so it observes the stream from its first
// value. These exact numbers are produced bit-for-bit by iverilog as well
// (verified), which is the point of using the reference PRNG; negatives prove
// the signed return type and the distinct values prove variation.
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
          "303379748\n"
          "-1064739199\n"
          "-2071669239\n"
          "-1309649309\n"
          "112818957\n"
          "1189058957\n"
          "-1295874971\n"
          "-1992863214\n");
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

// $random(seed) (LRM §20.15.1): the inout `seed` integer carries the whole PRNG
// state, so the draw and the updated seed after each call are both observable.
// These exact numbers (draw and seed, seed initialized to 1) are produced
// bit-for-bit by iverilog (verified), which validates both the reference stream
// off a nonzero seed and the inout write-back.
TEST_CASE("$random(seed) yields the deterministic seeded stream and writes the seed back") {
    const std::string out = run_capture(R"(
        module top;
            integer seed;
            integer i;
            integer x;
            initial begin
                seed = 1;
                for (i = 0; i < 8; i = i + 1) begin
                    x = $random(seed);
                    $display("%0d seed=%0d", x, seed);
                end
            end
        endmodule
    )");
    CHECK(out ==
          "-2147414528 seed=69070\n"
          "-1671855048 seed=475628535\n"
          "1129920902 seed=-1017563188\n"
          "-1374483364 seed=772999773\n"
          "1730349006 seed=-417135238\n"
          "1674352583 seed=-473131853\n"
          "-485282618 seed=1662200408\n"
          "-103324941 seed=2044158073\n");
}

TEST_CASE("$random(seed) lowers to a seeded rt_dist_uniform call") {
    constexpr std::string_view src = R"(
        module m;
            integer seed;
            integer x;
            initial begin
                seed = 7;
                x = $random(seed);
            end
        endmodule
    )";
    const std::string          ir  = emit(src);

    // $random(seed) == rtl_dist_uniform(seed, INT32_MIN, INT32_MAX): it shares
    // the DistCall path, so the runtime crossing is the packed-i64 dist call.
    CHECK(ir.find("call i64 @rt_dist_uniform(") != std::string::npos);
}

// $clog2 (LRM §20.8.1): ceil(log2(arg)), arg unsigned, $clog2(0) == 0. A
// constant argument folds to a literal; here it drives a localparam width and
// direct literal/parameter arguments. Boundary values (exact powers of two vs
// one past) cross-checked against iverilog.
TEST_CASE("$clog2 folds to the ceiling of log2") {
    const std::string out = run_capture(R"(
        module m;
            parameter DEPTH = 129;
            localparam AW = $clog2(DEPTH);
            initial
                $display("%0d %0d %0d %0d %0d %0d %0d",
                         $clog2(0), $clog2(1), $clog2(2), $clog2(3),
                         $clog2(256), $clog2(257), AW);
        endmodule
    )");
    CHECK(out == "0 0 1 2 8 9 8\n");
}

// A non-constant $clog2 argument lowers to a runtime count-leading-zeros. The
// operand's own width bounds the count (a 100-bit vector reaches 100), and the
// argument is treated as unsigned. Boundary and wide values cross-checked
// against iverilog.
TEST_CASE("$clog2 of a non-constant argument counts leading zeros at runtime") {
    const std::string out = run_capture(R"(
        module m;
            logic [7:0]  x;
            logic [15:0] w;
            logic [99:0] big;
            initial begin
                x = 0;    $display("%0d", $clog2(x));
                x = 1;    $display("%0d", $clog2(x));
                x = 5;    $display("%0d", $clog2(x));
                x = 8;    $display("%0d", $clog2(x));
                x = 255;  $display("%0d", $clog2(x));
                w = 256;  $display("%0d", $clog2(w));
                w = 257;  $display("%0d", $clog2(w));
                big = 100'h8000000000000000000000000; $display("%0d", $clog2(big));
                big = 100'h8000000000000000000000001; $display("%0d", $clog2(big));
            end
        endmodule
    )");
    CHECK(out == "0\n0\n3\n3\n8\n8\n9\n99\n100\n");
}

// An x/z bit anywhere in the operand makes the whole 32-bit result X (iverilog
// oracle; the LRM does not define x/z operands for $clog2).
TEST_CASE("$clog2 propagates X when the operand has unknown bits") {
    const std::string out = run_capture(R"(
        module m;
            logic [3:0] n;
            logic [7:0] u;
            initial begin
                n = 4'b001x;      $display("%b", $clog2(n));
                u = 8'bxxxxxxxx;  $display("%b", $clog2(u));
            end
        endmodule
    )");
    CHECK(out == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
                 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
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

// Signed power now lowers (LRM Table 11-4); it must emit, not reject. Behavior is
// covered by the Table 11-4 test above.
TEST_CASE("signed power lowers to valid IR") {
    const LowerResult r =
      lower_text(R"(module m; initial begin int a, b, c; a = 2; b = 3; c = a ** b; end endmodule)");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    std::string              text;
    llvm::raw_string_ostream os(text);
    CHECK(ir::emit_llvm(*r.module, os));
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
// Reused-definition instances: nonblocking assignment through the per-instance
// shadow bases and def-relative block registration (module-dedup prep). A
// design whose every NBA target lives in a reused definition exercises the
// instance shadow path with no root shadow-base consumer at all — the shape
// that once crashed when the per-instance base stores were guarded on the
// (never-created) root base. Values pinned against iverilog.
TEST_CASE("nonblocking assignment across reused module instances") {
    const std::string out = run_capture(R"(
        module leaf(input wire clk, input wire [7:0] d, output reg [7:0] q, output reg [7:0] r);
            initial begin q = 8'h5a; r = 8'ha5; end
            always @(posedge clk) begin q <= d + q; r <= q ^ d; end
        endmodule
        module top;
            reg clk = 0; reg [7:0] d = 1;
            wire [7:0] q0, r0, q1, r1, q2, r2;
            leaf l0(clk, d, q0, r0);
            leaf l1(clk, q0, q1, r1);
            leaf l2(clk, r1, q2, r2);
            initial begin
                repeat (200) begin #5 clk = ~clk; d = d + 3; end
                $display("%h %h %h %h %h %h", q0, r0, q1, r1, q2, r2);
            end
        endmodule
    )");
    CHECK(out == "ee ce b2 82 d3 a3\n");
}

// Body sharing across twin instances (share_twin_groups) must not change
// behavior: the same reused-instance design runs byte-identically with
// sharing disabled. Default config runs WITH sharing; the pinned expected
// value above already covers that side.
TEST_CASE("reused module instances: dedup off matches dedup on") {
    const char* src = R"(
        module leaf(input wire clk, input wire [7:0] d, output reg [7:0] q, output reg [7:0] r);
            initial begin q = 8'h5a; r = 8'ha5; end
            always @(posedge clk) begin q <= d + q; r <= q ^ d; end
        endmodule
        module top;
            reg clk = 0; reg [7:0] d = 1;
            wire [7:0] q0, r0, q1, r1, q2, r2;
            leaf l0(clk, d, q0, r0);
            leaf l1(clk, q0, q1, r1);
            leaf l2(clk, r1, q2, r2);
            initial begin
                repeat (200) begin #5 clk = ~clk; d = d + 3; end
                $display("%h %h %h %h %h %h", q0, r0, q1, r1, q2, r2);
            end
        endmodule
    )";
    const std::string on  = run_capture(src);
    const std::string off = run_capture(src, { .dedup = false });
    CHECK(on == "ee ce b2 82 d3 a3\n");
    CHECK(off == on);
}

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

// Per-element continuous drivers on an unpacked net array (LRM 6.6.1): each
// element is an independent single-driver, so the values are the per-element
// assigns, not a last-write-wins collision. Matches iverilog.
TEST_CASE("unpacked net array driven one element at a time") {
    const std::string out = run_capture(R"(
        module top;
            logic [7:0] a, b;
            wire [7:0] mem [0:3];
            assign mem[0] = a;
            assign mem[1] = b;
            assign mem[2] = a ^ b;
            initial begin
                a = 8'h0A; b = 8'h05;
                #1 $display("%h %h %h", mem[0], mem[1], mem[2]);
            end
        endmodule
    )");
    CHECK(out == "0a 05 0f\n");
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

// `always @*` sensitivity includes signals both read and written by the block
// (LRM 9.4.2.2 excludes only identifiers appearing solely as lvalues), unlike
// always_comb's written-exclusion (9.2.2.2.1 b, legal there only because of the
// single-writer rule). Regression: `a` was subtracted from the @* list, so the
// external `a = 100` write at t=1 never re-triggered the block and `y` stayed 0.
// Expected output matches iverilog.
TEST_CASE("always @* re-triggers on external writes to read+written signals") {
    const std::string out = run_capture(R"(
        module m;
            integer a, b, d, y;
            always @* begin
                if (b > 0) a = b;
                y = a + d;
            end
            initial begin
                b = 0; d = 7; a = 0;
                #1 a = 100;
                #1 $display("y=%0d", y);
                #1 b = 5;
                #1 $display("y=%0d", y);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "y=107\ny=12\n");
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
// Binary: a text-mode stream rewrites \n as \r\n on Windows, and these fixtures
// are compared byte-for-byte against what the design reads back.
void write_file(const char* path, std::string_view text) {
    std::FILE* f = std::fopen(path, "wb");
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

// A memory whose elements are wider than 64 bits: rt_readmem assembles each
// value across multiple words (the <= 64-bit path stays scalar). Read back
// through 64-bit part-selects. Cross-checked against Icarus Verilog, including
// x/z nibbles, a short (zero-extended) token, and an untouched element.
TEST_CASE("$readmemh into a memory with elements wider than 64 bits") {
    const char* path = "clotho_readmemh_wide_test.hex";
    write_file(path,
               "DEADBEEFCAFEBABE0123456789ABCDEF\n"
               "FEDCBA9876543210zzzzzzzzxxxxxxxx\n"
               "00000000000000000000000000000042\n");
    const std::string src = std::string(R"(
        module m;
            reg [127:0] mem [0:3];
            integer k;
            initial begin
                $readmemh(")") + path + R"(", mem);
                for (k = 0; k < 4; k = k + 1)
                    $display("%h_%h", mem[k][127:64], mem[k][63:0]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "deadbeefcafebabe_0123456789abcdef\n"
                              "fedcba9876543210_zzzzzzzzxxxxxxxx\n"
                              "0000000000000000_0000000000000042\n"
                              "xxxxxxxxxxxxxxxx_xxxxxxxxxxxxxxxx\n");
    std::remove(path);
}

// A 2-state (narrow) wide memory stores only the iW value word; x/z read as 0
// (LRM 6.3.1). Cross-checked against Icarus Verilog.
TEST_CASE("$readmemh into a 2-state memory with elements wider than 64 bits") {
    const char* path = "clotho_readmemh_wide2s_test.hex";
    write_file(path, "DEADBEEFCAFEBABE0123456789ABCDEF\n0000000000000000000000000000002A\n");
    const std::string src = std::string(R"(
        module m;
            bit [127:0] mem [0:1];
            integer k;
            initial begin
                $readmemh(")") + path + R"(", mem);
                for (k = 0; k < 2; k = k + 1)
                    $display("%h_%h", mem[k][127:64], mem[k][63:0]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "deadbeefcafebabe_0123456789abcdef\n"
                              "0000000000000000_000000000000002a\n");
    std::remove(path);
}

// $fread into elements wider than 64 bits: bytes pack MSB-first across words.
// Element 0 is full (16 bytes); element 1 reads 4 bytes then hits EOF, so its
// unread low bytes zero-fill (Icarus shows the prior x there,
// a documented Clotho deviation). Element 0 and the byte count match Icarus.
TEST_CASE("$fread into a memory with elements wider than 64 bits") {
    const char*       path  = "clotho_fread_wide_test.bin";
    const std::string bytes = { '\x01', '\x23', '\x45', '\x67', '\x89', '\xab', '\xcd',
                                '\xef', '\xfe', '\xdc', '\xba', '\x98', '\x76', '\x54',
                                '\x32', '\x10', '\xaa', '\xbb', '\xcc', '\xdd' };
    write_file(path, bytes);
    const std::string src = std::string(R"(
        module m;
            reg [127:0] mem [0:1];
            integer fd, rc;
            initial begin
                fd = $fopen(")") + path + R"(", "rb");
                rc = $fread(mem, fd);
                $display("rc=%0d", rc);
                $display("%h_%h", mem[0][127:64], mem[0][63:0]);
                $display("%h_%h", mem[1][127:64], mem[1][63:0]);
                $fclose(fd);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "rc=20\n"
                              "0123456789abcdef_fedcba9876543210\n"
                              "aabbccdd00000000_0000000000000000\n");
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

// Slurp a file written by a file-I/O test.
std::string read_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    REQUIRE(f != nullptr);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

// Two-argument $fopen returns an FD; $fdisplay / $fwrite route to it and the
// trailing newline distinguishes them (LRM §21.3).
TEST_CASE("$fopen FD form: $fdisplay and $fwrite") {
    const char* path = "clotho_fd_test.txt";
    std::remove(path);
    const std::string src = std::string(R"(
        module m;
            integer fd;
            initial begin
                fd = $fopen(")") + path + R"(", "w");
                $fdisplay(fd, "line %0d", 1);
                $fwrite(fd, "no-newline ");
                $fwrite(fd, "tail\n");
                $fclose(fd);
            end
        endmodule
    )";
    run_capture(src);
    CHECK(read_file(path) == "line 1\nno-newline tail\n");
    std::remove(path);
}

// %u writes the 2-value projection, %z vpi aval/bval word pairs, both as
// 32-bit words LSB-first (LRM §21.2.1.1). Byte-exact against Icarus Verilog:
// 8'b1x0z101z has aval 0xca (x reads 1, z reads 0) and bval 0x51 (the xz mask).
TEST_CASE("$fwrite %u and %z emit unformatted words") {
    const char* path = "clotho_ufmt_test.bin";
    std::remove(path);
    const std::string src = std::string(R"(
        module m;
            integer fd;
            initial begin
                fd = $fopen(")") + path + R"(", "wb");
                $fwrite(fd, "%u", 32'h01020304);
                $fwrite(fd, "%z", 8'b1x0z_101z);
                $fclose(fd);
            end
        endmodule
    )";
    run_capture(src);
    CHECK(read_file(path) == std::string("\x04\x03\x02\x01\xca\x00\x00\x00\x51\x00\x00\x00", 12));
    std::remove(path);
}

// One-argument $fopen returns a multichannel descriptor opened for writing.
TEST_CASE("$fopen MCD form writes to its channel") {
    const char* path = "clotho_mcd_test.txt";
    std::remove(path);
    const std::string src = std::string(R"(
        module m;
            integer mcd;
            initial begin
                mcd = $fopen(")") + path + R"(");
                $fdisplay(mcd, "hello %0d", 42);
                $fclose(mcd);
            end
        endmodule
    )";
    run_capture(src);
    CHECK(read_file(path) == "hello 42\n");
    std::remove(path);
}

// OR-ing two MCDs fans a single write out to every set channel (LRM §21.3.4).
TEST_CASE("$fdisplay to a combined MCD writes both files") {
    const char* p1 = "clotho_mcd_a.txt";
    const char* p2 = "clotho_mcd_b.txt";
    std::remove(p1);
    std::remove(p2);
    const std::string src = "module m;\n"
                            "  integer a, b;\n"
                            "  initial begin\n"
                            "    a = $fopen(\"" +
                            std::string(p1) +
                            "\");\n"
                            "    b = $fopen(\"" +
                            std::string(p2) +
                            "\");\n"
                            "    $fdisplay(a | b, \"both %0d\", 7);\n"
                            "    $fclose(a);\n"
                            "    $fclose(b);\n"
                            "  end\n"
                            "endmodule\n";
    run_capture(src);
    CHECK(read_file(p1) == "both 7\n");
    CHECK(read_file(p2) == "both 7\n");
    std::remove(p1);
    std::remove(p2);
}

// $fflush forces buffered output to disk without closing: the file is readable
// mid-run, before any $fclose (LRM §21.3.6).
TEST_CASE("$fflush(fd) flushes before close") {
    const char* path = "clotho_fflush_test.txt";
    std::remove(path);
    const std::string src = std::string(R"(
        module m;
            integer fd;
            initial begin
                fd = $fopen(")") + path + R"(", "w");
                $fwrite(fd, "flushed");
                $fflush(fd);
            end
        endmodule
    )";
    run_capture(src);
    CHECK(read_file(path) == "flushed");
    std::remove(path);
}

// $display / $write still reach stdout after the merge onto rt_fwrite (the
// standard-output MCD, bit 0).
TEST_CASE("$display still targets stdout after the fwrite merge") {
    CHECK(run_capture(R"(
        module m;
            initial $display("to stdout %0d", 9);
        endmodule
    )") == "to stdout 9\n");
}

// $fgets reads a line (through the newline) into a packed vector, stored
// right-justified, and returns the character count. The %h layout is
// cross-checked against Icarus Verilog (n=6, 0x...68656c6c6f0a for "hello\n").
TEST_CASE("$fgets reads a line right-justified") {
    const char* path = "clotho_fgets_test.txt";
    write_file(path, "hello\nworld\n");
    const std::string src = std::string(R"(
        module m;
            integer fd, n;
            reg [8*8-1:0] line;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                n = $fgets(line, fd);
                $display("%0d %h", n, line);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "6 000068656c6c6f0a\n");
    std::remove(path);
}

// $fgets into a `string` destination (LRM §21.3.4): no fixed width to
// right-justify into, so the destination is assigned exactly the bytes read
// (through the newline).
TEST_CASE("$fgets into a string variable") {
    const char* path = "clotho_fgets_str_test.txt";
    write_file(path, "hello\nworld\n");
    const std::string src = std::string(R"(
        module m;
            integer fd, n;
            string line;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                n = $fgets(line, fd);
                $display("%0d %s", n, line);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "6 hello\n\n");
    std::remove(path);
}

// Unlike the packed-vector destination (capped at its declared width), a
// string destination has no capacity limit: a line longer than any
// reasonable fixed-width reg is still read in full.
TEST_CASE("$fgets into a string variable is not width-capped") {
    const char* path = "clotho_fgets_str_long_test.txt";
    const std::string long_line(100, 'x');
    write_file(path, long_line + "\n");
    const std::string src = std::string(R"(
        module m;
            integer fd, n;
            string line;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                n = $fgets(line, fd);
                $display("%0d", n);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "101\n"); // 100 'x' + the newline
    std::remove(path);
}

// $fgets into a string variable at EOF (nothing read) leaves the destination
// unchanged and returns 0, mirroring the packed-vector destination's
// behavior on a 0-byte read.
TEST_CASE("$fgets into a string variable at EOF leaves it unchanged") {
    const char* path = "clotho_fgets_str_eof_test.txt";
    write_file(path, "");
    const std::string src = std::string(R"(
        module m;
            integer fd, n;
            string line;
            initial begin
                line = "untouched";
                fd = $fopen(")") + path + R"(", "r");
                n = $fgets(line, fd);
                $display("%0d %s", n, line);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 untouched\n");
    std::remove(path);
}

// $fgetc returns bytes then -1 at EOF; $ungetc pushes a byte back for the next
// read; $feof reports the end condition (LRM §21.3.4).
TEST_CASE("$fgetc / $ungetc / $feof") {
    const char* path = "clotho_fgetc_test.txt";
    write_file(path, "AB");
    const std::string src = std::string(R"(
        module m;
            integer fd, c1, c2, c3, c4, eof;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                c1 = $fgetc(fd);        // 'A'
                c2 = $ungetc(c1, fd);   // -> 'A'
                c2 = $fgetc(fd);        // 'A' again
                c3 = $fgetc(fd);        // 'B'
                c4 = $fgetc(fd);        // EOF
                eof = $feof(fd);
                $display("%0d %0d %0d %0d %0d", c1, c2, c3, c4, eof);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "65 65 66 -1 1\n");
    std::remove(path);
}

// $sformat renders an explicit format into a string variable, stored
// right-justified. %h layout cross-checked against Icarus (0x..6465633d37).
TEST_CASE("$sformat into a string variable") {
    CHECK(run_capture(R"(
        module m;
            reg [8*8-1:0] s;
            initial begin
                $sformat(s, "hex=%0h dec=%0d", 255, 7);
                $display("%h", s);
            end
        endmodule
    )") == "6666206465633d37\n"); // "ff dec=7" (low 8 bytes of "hex=ff dec=7")
}

// An oversized $sformat result keeps its low (rightmost) bytes, matching a
// string-to-narrower-reg assignment. Cross-checked against Icarus.
TEST_CASE("$sformat truncates to the low bytes on overflow") {
    CHECK(run_capture(R"(
        module m;
            reg [8*4-1:0] s;
            initial begin
                $sformat(s, "abcdef");
                $display("%h", s);
            end
        endmodule
    )") == "63646566\n"); // "cdef"
}

// $swrite concatenates $write-style; a bare value uses the auto-sized decimal
// default (LRM §21.2.1.1), so "v" + 10-wide "         5" keeps its low 8 bytes.
TEST_CASE("$swrite concatenates into a string variable") {
    CHECK(run_capture(R"(
        module m;
            reg [8*8-1:0] s;
            initial begin
                $swrite(s, "v", 5);
                $display("%h", s);
            end
        endmodule
    )") == "2020202020202035\n"); // "       5"
}

// $ferror reports a clear stream as code 0 (the message write-back path).
TEST_CASE("$ferror on a clear stream returns 0") {
    const char* path = "clotho_ferror_test.txt";
    const std::string src = std::string(R"(
        module m;
            integer fd, code;
            reg [8*80-1:0] msg;
            initial begin
                fd = $fopen(")") + path + R"(", "w");
                code = $ferror(fd, msg);
                $display("%0d", code);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0\n");
    std::remove(path);
}

// $ferror into a `string` destination reports a clear stream as code 0 with
// an empty message, mirroring the packed-vector destination's behavior.
TEST_CASE("$ferror into a string variable on a clear stream returns 0") {
    const char* path = "clotho_ferror_str_test.txt";
    const std::string src = std::string(R"(
        module m;
            integer fd, code;
            string msg;
            initial begin
                fd = $fopen(")") + path + R"(", "w");
                code = $ferror(fd, msg);
                $display("%0d :%s:", code, msg);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 ::\n");
    std::remove(path);
}

// An invalid descriptor (never opened) reports code -1 with a diagnostic
// message, same as the packed-vector destination.
TEST_CASE("$ferror into a string variable on an invalid descriptor") {
    const std::string src = R"(
        module m;
            integer code;
            string msg;
            initial begin
                code = $ferror(32'h8000_0063, msg);
                $display("%0d %s", code, msg);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-1 invalid file descriptor\n");
}

// $fscanf parses fields into integer lvalues and returns the conversion count.
// Values cross-checked against Icarus Verilog.
TEST_CASE("$fscanf parses integer fields") {
    const char* path = "clotho_fscanf_test.txt";
    write_file(path, "42 -7 ff\n");
    const std::string src = std::string(R"(
        module m;
            integer fd, n, a, b, c;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                n = $fscanf(fd, "%d %d %h", a, b, c);
                $display("%0d %0d %0d %0d", n, a, b, c);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 42 -7 255\n");
    std::remove(path);
}

// %s reads a whitespace-delimited token (right-justified), and $fscanf resumes at
// the next field. Cross-checked against Icarus.
TEST_CASE("$fscanf with %s and a trailing field") {
    const char* path = "clotho_fscanf_s_test.txt";
    write_file(path, "hello 255\n");
    const std::string src = std::string(R"(
        module m;
            integer fd, n, v;
            reg [8*8-1:0] word;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                n = $fscanf(fd, "%s %d", word, v);
                $display("%0d %h %0d", n, word, v);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 00000068656c6c6f 255\n"); // "hello"
    std::remove(path);
}

// $sscanf parses a literal string; the `*` flag suppresses a field (no arg).
// Cross-checked against Icarus.
TEST_CASE("$sscanf with a suppressed field") {
    CHECK(run_capture(R"(
        module m;
            integer n, a, b;
            initial begin
                n = $sscanf("10 20 30", "%d %*d %d", a, b);
                $display("%0d %0d %0d", n, a, b);
            end
        endmodule
    )") == "2 10 30\n");
}

// $fscanf returns -1 (EOF) when the input is exhausted before the first
// conversion (LRM §21.3.4).
TEST_CASE("$fscanf returns -1 at end of file") {
    const char* path = "clotho_fscanf_eof_test.txt";
    write_file(path, "");
    const std::string src = std::string(R"(
        module m;
            integer fd, n, a;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                n = $fscanf(fd, "%d", a);
                $display("%0d", n);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-1\n");
    std::remove(path);
}

// The file-io write-backs are blocking writes: each must service the target
// signal's wait queues like an ordinary assignment. Verified against iverilog.
TEST_CASE("$sscanf / $sformat / $fgets / scalar $fread wake waiting processes") {
    const char* path = "clotho_fio_wake_test.txt";
    // 4 bytes so the scalar $fread fills its target exactly (a short read's
    // low-byte fill is LRM-unpinned).
    write_file(path, "hi!\n");
    const std::string src = R"sv(
        module m;
            int r, n, fd;
            logic [31:0] s;
            always @(r) $display("r woke %h", r);
            always @(s) $display("s woke %h", s);
            initial begin
                #1 n = $sscanf("5", "%d", r);
                #1 $sformat(s, "AB");
                #1 fd = $fopen(")sv" + std::string(path) + R"sv(", "r");
                n = $fgets(s, fd);
                $fclose(fd);
                #1 fd = $fopen(")sv" + std::string(path) + R"sv(", "rb");
                n = $fread(r, fd);
                $fclose(fd);
                #1 $finish;
            end
        endmodule
    )sv";
    CHECK(run_capture(src) == "r woke 00000005\ns woke 00004142\ns woke 6869210a\nr woke 6869210a\n");
    std::remove(path);
}

// $fread into a memory wakes the array's Any-edge waiters (here the implicit
// sensitivity of a continuous assign reading an element). The time-0 line is
// the assign's initial x -> 00 transition on the 4-state wire. Verified
// against iverilog (which schedules the time-0 evaluation before the @(y)
// registration and so skips the first line — both orders are legal).
TEST_CASE("$fread into a memory wakes array readers") {
    const char* path = "clotho_fread_wake_test.bin";
    write_file(path, std::string("\xAA\xBB\xCC\xDD", 4));
    const std::string src = std::string(R"(
        module m;
            bit [7:0] mem [0:3];
            wire [7:0] y = mem[1];
            int fd, n;
            always @(y) $display("y woke %h", y);
            initial begin
                #1 fd = $fopen(")") + path + R"(", "rb");
                n = $fread(mem, fd);
                $fclose(fd);
                #1 $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "y woke 00\ny woke bb\n");
    std::remove(path);
}

// $fread packs raw bytes MSB-first into a scalar and then a memory, sharing the
// $readmem geometry. Cross-checked against Icarus Verilog.
TEST_CASE("$fread into a scalar and a memory") {
    const char* path = "clotho_fread_test.bin";
    write_file(path, std::string("\xDE\xAD\xBE\xEF\x11\x22\x33\x44", 8));
    const std::string src = std::string(R"(
        module m;
            integer fd, n1, n2;
            reg [31:0] r;
            reg [7:0] mem [0:3];
            initial begin
                fd = $fopen(")") + path + R"(", "rb");
                n1 = $fread(r, fd);
                n2 = $fread(mem, fd);
                $display("%0d %h %0d %h %h %h %h", n1, r, n2, mem[0], mem[1], mem[2], mem[3]);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4 deadbeef 4 11 22 33 44\n");
    std::remove(path);
}

// $fread(mem, fd, start, count) fills a bounded address window. Cross-checked
// against Icarus.
TEST_CASE("$fread with start and count into a memory") {
    const char* path = "clotho_fread_sc_test.bin";
    write_file(path, std::string("\xAA\xBB\xCC", 3));
    const std::string src = std::string(R"(
        module m;
            integer fd, n;
            reg [7:0] mem [0:5];
            integer k;
            initial begin
                for (k = 0; k < 6; k = k + 1) mem[k] = 0;
                fd = $fopen(")") + path + R"(", "rb");
                n = $fread(mem, fd, 2, 3);
                $display("%0d %h %h %h %h %h %h", n, mem[0], mem[1], mem[2], mem[3], mem[4], mem[5]);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 00 00 aa bb cc 00\n");
    std::remove(path);
}

// $ftell reports the offset, $fseek repositions (operation 0 = SEEK_SET), and
// $rewind returns to the start (LRM §21.3.5).
TEST_CASE("$ftell / $fseek / $rewind") {
    const char* path = "clotho_fseek_test.txt";
    write_file(path, "0123456789");
    const std::string src = std::string(R"(
        module m;
            integer fd, p1, s, p2, c, r, c2;
            initial begin
                fd = $fopen(")") + path + R"(", "r");
                c  = $fgetc(fd); c = $fgetc(fd); c = $fgetc(fd);
                p1 = $ftell(fd);        // 3
                s  = $fseek(fd, 7, 0);  // 0
                p2 = $ftell(fd);        // 7
                c  = $fgetc(fd);        // '7'
                r  = $rewind(fd);       // 0
                c2 = $fgetc(fd);        // '0'
                $display("%0d %0d %0d %c %0d %c", p1, s, p2, c, r, c2);
                $fclose(fd);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 0 7 7 0 0\n");
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
    const std::string raw = emit_raw(src);
    // v and y carry z (full slot); clean is proven never-z (elided slot).
    // Named-type layout survives only in the raw IR (the optimizer drops it).
    CHECK(raw.find("%logic.4 = type { i8, i4 }") != std::string::npos);
    CHECK(raw.find("%logic.nz.4 = type { i8 }") != std::string::npos);
    CHECK(run_capture(src) == "z01x z01x 1010\n1 1\ncasez hit\n");
}

TEST_CASE("NBA of a z value round-trips a static-select block commit (z and never-z targets)") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0;
            logic [3:0] zq;
            logic [3:0] nq;
            logic [3:0] zsrc = 4'bzz01;
            always @(posedge clk) begin
                zq[3:0] <= zsrc;
                nq[3:0] <= nq === 4'bxxxx ? 4'd5 : nq + 1;
            end
            initial begin
                #1 clk = 1;
                #1 $display("%b %b", zq, nq);
                $finish;
            end
        endmodule
    )";
    // Static bit-range NBA targets commit through the block path with a compile-time
    // mask; no per-signal bucket globals form. zq carries z (full slot), nq is
    // proven never-z, and both round-trip their shadow commit.
    const std::string ir = emit(src);
    CHECK(ir.find("clotho.nba.commit_blk") != std::string::npos);
    CHECK(ir.find("clotho.nba.w4") == std::string::npos);
    CHECK(run_capture(src) == "zz01 0101\n");
}

TEST_CASE("whole-scalar NBAs lower to the block-commit path by default") {
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
    // One straight-line block: a single enqueue registration and one commit
    // function; no per-signal w4 bucket worklists form.
    CHECK(ir.find("clotho.nba_block_enqueue") != std::string::npos);
    CHECK(ir.find("clotho.nba.commit_blk0") != std::string::npos);
    CHECK(ir.find("clotho.nba.w4.full") == std::string::npos);
    // Shadow accesses go through the owning instance's opaque base pointer,
    // not state-relative constant offsets (memory-dependence mis-speculation
    // defense; see shadow_ptr).
    CHECK(ir.find("clotho.inst.m.shadow_base") != std::string::npos);
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

// %h accepts z / ? digits (LRM Table 21-7); the scan target must keep its z
// plane under z elision (the Scanf seed in the may-z analysis), and a 2-state
// target drops the z digits to 0 (LRM 6.3.1).
TEST_CASE("$sscanf z digits survive z elision; a 2-state target reads 0") {
    constexpr std::string_view src = R"(
        module m;
            logic [31:0] r4;
            int r2, n;
            initial begin
                n = $sscanf("zzzzzzzz", "%h", r4);
                $display("%h", r4);
                n = $sscanf("z1x0zz1?", "%h", r4);
                $display("%h", r4);
                n = $sscanf("zzzzzzzz", "%h", r2);
                $display("%h", r2);
                $finish;
            end
        endmodule
    )";
    // Verified against iverilog except the ? digit, which iverilog reads as x;
    // ? is an alternative for z in the LRM (5.7.1), so z is kept.
    CHECK(run_capture(src) == "zzzzzzzz\nz1x0zz1z\n00000000\n");
}

// $sscanf into a target wider than 64 bits: the runtime scanner assembles the
// value multi-word (hex shift, decimal 1e0-base multiply-add, %s byte pack, two's
// complement for a negative %d). Read back through 64-bit part-selects.
// Cross-checked against Icarus Verilog.
TEST_CASE("$sscanf into a target wider than 64 bits") {
    constexpr std::string_view src = R"(
        module m;
            logic [127:0] a;
            logic signed [95:0] s;
            int rc;
            initial begin
                rc = $sscanf("deadbeefcafebabe0123456789abcdef", "%h", a);
                $display("%0d %h_%h", rc, a[127:64], a[63:0]);
                rc = $sscanf("340282366920938463463374607431768211455", "%d", a);
                $display("%0d %h_%h", rc, a[127:64], a[63:0]);
                rc = $sscanf("-170141183460469231731", "%d", s);
                $display("%0d %h_%h", rc, s[95:64], s[63:0]);
                rc = $sscanf("HelloWideWorld!!", "%s", a);
                $display("%0d %h_%h", rc, a[127:64], a[63:0]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 deadbeefcafebabe_0123456789abcdef\n"
                              "1 ffffffffffffffff_ffffffffffffffff\n"
                              "1 fffffff6_c6d11716de2a2f8d\n"
                              "1 48656c6c6f576964_65576f726c642121\n");
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
    CHECK(run_capture(src) == "z111  7\n"); // %d of a 4-bit value auto-sizes to 2
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*r.module, os, { .lower_coro = false, .z_elision = false }));
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

TEST_CASE("unmodeled net kinds fail loudly") {
    // wire/tri/uwire/wand/wor/tri0/tri1 are modeled; trireg/supply* carry charge
    // or strength semantics clotho does not implement.
    const LowerResult r = lower_text(R"(
        module m;
            trireg w;
        endmodule
    )");
    CHECK(!r.ok);
    CHECK(r.diagnostic_text().find("unsupported net type") != std::string::npos);
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

TEST_CASE("a signal written by a called task stays in always_comb sensitivity") {
    // Regression: the sensitivity walk used to descend into task bodies, so
    // the task's write of `a` subtracted the block-level read and a change to
    // `a` never re-triggered the comb process (LRM 9.2.2.2.1: task contents
    // contribute nothing).
    const std::string_view src = R"(
        module m;
            reg a, b, y;
            task automatic t;
                a = 1'b1;
            endtask
            always_comb begin
                y = a | b;
                if (b) t();
            end
            initial begin
                a = 1'b0; b = 1'b0;
                #1 a = 1'b1;
                #1 $display("%b", y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

TEST_CASE("always @* re-triggers on a read+written intermediate") {
    // Regression: @* subtracted written signals like always_comb, so another
    // process's write to `t` never woke the block (LRM 9.4.2.2 Example 2
    // keeps a read intermediate sensitive).
    const std::string_view src = R"(
        module m;
            reg a, b, c, t, y;
            always @* begin
                y = t | c;
                if (c) t = a & b;
            end
            initial begin
                a = 1'b0; b = 1'b0; c = 1'b0; t = 1'b0;
                #1 t = 1'b1;
                #1 $display("%b", y);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

TEST_CASE("posedge/negedge classify x/z transitions per LRM Table 9-2") {
    // Regression: edge detection read only the must-plane LSB, so a 0->x
    // posedge (and its mirrors) never fired. Sequence below has two posedges
    // (0->x, x->1); x->0 and 1->z are negedges and z->x is no edge.
    const std::string_view src = R"(
        module m;
            reg clk;
            integer n;
            initial n = 0;
            always @(posedge clk) n = n + 1;
            initial begin
                #1 clk = 1'b0;
                #1 clk = 1'bx;
                #1 clk = 1'b1;
                #1 clk = 1'bz;
                #1 clk = 1'bx;
                #1 $display("%0d", n);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n");
}

TEST_CASE("a level wait wakes on a z-only value change") {
    // Regression: the any-change compare covered only the must/may planes, so
    // x<->z transitions (visible only in the z word) never woke @(s).
    const std::string_view src = R"(
        module m;
            reg s;
            integer n;
            initial n = 0;
            always @(s) n = n + 1;
            initial begin
                #1 s = 1'bz;
                #1 s = 1'bx;
                #1 s = 1'b0;
                #1 $display("%0d", n);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3\n");
}

TEST_CASE("the edge keyword fires on posedge or negedge, not x<->z") {
    // `edge` = posedge|negedge (LRM 9.4.2): the x->z step is no edge; z->1 is
    // a posedge and 1->x a negedge.
    const std::string_view src = R"(
        module m;
            reg s;
            integer n;
            initial n = 0;
            always @(edge s) n = n + 1;
            initial begin
                #1 s = 1'bz;
                #1 s = 1'b1;
                #1 s = 1'bx;
                #1 $display("%0d", n);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2\n");
}

TEST_CASE("NBA commits classify x/z edges and z-only changes the same way") {
    // The nonblocking commit path has its own edge-fact emission; the clock
    // sequence has two posedges (0->x, x->1), and the level wait on s must
    // wake for the x->z NBA commit.
    const std::string_view src = R"(
        module m;
            reg clk, s;
            integer n, k;
            initial begin n = 0; k = 0; end
            always @(posedge clk) n = n + 1;
            always @(s) k = k + 1;
            initial begin
                #1 clk <= 1'b0;
                #1 clk <= 1'bx;
                #1 clk <= 1'b1;
                #1 s <= 1'bz;
                #1 $display("%0d %0d", n, k);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 1\n");
}

// -- Real (floating-point) backend tests --
//
// The frontend does not lower real expressions yet, so these build the process
// IR by hand and observe results through integer $display of converted values.

namespace {

// JIT-run a hand-built module and capture its stdout.
std::string run_module(ir::Module& m) {
    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(m);

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

// A 32-bit signed two-state type (int).
ir::Ref int_type(ir::InternPool& pool) {
    return pool.put(
      ir::Type{ .kind = ir::Type::Kind::PackedArray, .is_signed = true, .element = pool.bit_type, .left = 31, .right = 0 });
}

constexpr auto kLoc = static_cast<ir::SourceLoc>(0);

// Append an initial process that %0d-displays `v` (an int-typed value in `b`).
void display_int(ir::Builder& b, ir::Index v) {
    const std::array<ir::Index, 1> args{ v };
    b.display("%0d", true, args, kLoc);
}

} // namespace

TEST_CASE("real arithmetic, rounding, and truncation") {
    ir::Module  m;
    ir::Builder b(m.pool);
    const ir::Ref i32  = int_type(m.pool);
    const auto    real = [&](double v) { return b.constant(m.pool.put_real(m.pool.real_type, v), kLoc); };
    const auto    ftoi = [&](ir::Index v) { return b.float_cvt(ir::Inst::Tag::FtoI, v, i32, kLoc); };

    // (2.0 * 1.25 + 0.5) == 3.0.
    const ir::Index prod = b.binary(ir::Inst::Tag::FMul, real(2.0), real(1.25), kLoc);
    const ir::Index sum  = b.binary(ir::Inst::Tag::FAdd, prod, real(0.5), kLoc);
    display_int(b, ftoi(sum));

    // LRM 6.12.1: assignment conversion rounds half away from zero.
    display_int(b, ftoi(real(2.5)));
    display_int(b, ftoi(real(-1.5)));
    // LRM 20.5: $rtoi truncates toward zero.
    display_int(b, b.float_cvt(ir::Inst::Tag::FtoITrunc, real(-15.7), i32, kLoc));
    display_int(b, b.float_cvt(ir::Inst::Tag::FtoITrunc, real(123.45), i32, kLoc));

    // 2.0 ** -1 == 0.5 -> FPow with real operands; 0.5 rounds to 1.
    const ir::Index p = b.binary(ir::Inst::Tag::FPow, real(2.0), real(-1.0), kLoc);
    display_int(b, ftoi(p));

    // FNeg keeps sign: -(2.5) rounds to -3.
    display_int(b, ftoi(b.unary(ir::Inst::Tag::FNeg, real(2.5), kLoc)));

    m.add_process(ir::Process{ .activation = ir::Activation::Initial, .span = { kLoc, kLoc }, .ir = b.finish() });
    CHECK(run_module(m) == "3\n3\n-2\n-15\n123\n1\n-3\n");
}

TEST_CASE("real compares are definite and NaN-correct") {
    ir::Module  m;
    ir::Builder b(m.pool);
    // Widen the 1-bit compare result through an unsigned type (a signed resize
    // would sign-extend 1'b1 to -1).
    const ir::Ref u32 = m.pool.put(
      ir::Type{ .kind = ir::Type::Kind::PackedArray, .element = m.pool.bit_type, .left = 31, .right = 0 });
    const auto real = [&](double v) { return b.constant(m.pool.put_real(m.pool.real_type, v), kLoc); };
    const auto show = [&](ir::Index bit) { display_int(b, b.resize(bit, u32, kLoc)); };

    const double    qnan = std::numeric_limits<double>::quiet_NaN();
    const ir::Index nan  = b.constant(m.pool.put_real(m.pool.real_type, qnan), kLoc);

    show(b.binary(ir::Inst::Tag::FEq, nan, nan, kLoc));         // 0: NaN == NaN is false
    show(b.binary(ir::Inst::Tag::FNe, nan, nan, kLoc));         // 1: NaN != NaN is true
    show(b.binary(ir::Inst::Tag::FLt, real(1.5), real(2.5), kLoc)); // 1
    show(b.binary(ir::Inst::Tag::FGe, real(1.5), real(2.5), kLoc)); // 0
    show(b.binary(ir::Inst::Tag::FEq, real(0.0), real(-0.0), kLoc)); // 1: +0.0 == -0.0

    m.add_process(ir::Process{ .activation = ir::Activation::Initial, .span = { kLoc, kLoc }, .ir = b.finish() });
    CHECK(run_module(m) == "0\n1\n1\n0\n1\n");
}

TEST_CASE("real mux: definite select and the ambiguous-condition rule") {
    ir::Module  m;
    ir::Builder b(m.pool);
    const ir::Ref i32  = int_type(m.pool);
    const auto    real = [&](double v) { return b.constant(m.pool.put_real(m.pool.real_type, v), kLoc); };
    const auto    ftoi = [&](ir::Index v) { return b.float_cvt(ir::Inst::Tag::FtoI, v, i32, kLoc); };

    // A definite-1 and a definite-0 1-bit condition.
    const ir::Index c1 = b.constant(m.pool.put_int(m.pool.logic_type, 1), kLoc);
    const ir::Index c0 = b.constant(m.pool.put_int(m.pool.logic_type, 0), kLoc);
    // A 1-bit X condition: logic canonical planes are val, xz.
    const std::array<uint8_t, 2> xbytes{ 0x01, 0x01 };
    const ir::Index cx = b.constant(m.pool.put_value(m.pool.logic_type, xbytes), kLoc);

    display_int(b, ftoi(b.select(c1, real(3.0), real(5.0), kLoc))); // 3
    display_int(b, ftoi(b.select(c0, real(3.0), real(5.0), kLoc))); // 5
    // LRM 11.4.11: ambiguous condition -> equal alternatives give that value,
    // unequal alternatives give the real default 0.0.
    display_int(b, ftoi(b.select(cx, real(3.0), real(3.0), kLoc))); // 3
    display_int(b, ftoi(b.select(cx, real(3.0), real(5.0), kLoc))); // 0

    m.add_process(ir::Process{ .activation = ir::Activation::Initial, .span = { kLoc, kLoc }, .ir = b.finish() });
    CHECK(run_module(m) == "3\n5\n3\n0\n");
}

TEST_CASE("real signal storage round-trips, defaults to 0.0, and wakes waiters") {
    ir::Module      m;
    const ir::Signal r = m.add_signal(ir::SignalInfo{ .name = "r",
                                                      .type = m.pool.real_type,
                                                      .dir  = ir::SignalInfo::Direction::Internal });
    const ir::Ref    i32 = int_type(m.pool);

    // Watcher: @(r) $display("%0d", FtoI(r)).
    {
        ir::Builder     b(m.pool);
        const ir::Index sig  = b.signal(r, kLoc);
        const ir::Index load = b.unary(ir::Inst::Tag::Load, sig, kLoc);
        display_int(b, b.float_cvt(ir::Inst::Tag::FtoI, load, i32, kLoc));
        m.add_process(
          ir::Process{ .activation = ir::Activation::Always, .wait_events = { { r, ir::Edge::Any } }, .span = { kLoc, kLoc }, .ir = b.finish() });
    }

    // Driver: default is 0.0 (2-state zero pattern), then blocking-store 2.25,
    // then NBA-store 4.5 (commits after the active drain and wakes the watcher).
    {
        ir::Builder     b(m.pool);
        const auto      real = [&](double v) { return b.constant(m.pool.put_real(m.pool.real_type, v), kLoc); };
        const ir::Index sig  = b.signal(r, kLoc);
        const ir::Index ld0  = b.unary(ir::Inst::Tag::Load, sig, kLoc);
        display_int(b, b.float_cvt(ir::Inst::Tag::FtoI, ld0, i32, kLoc)); // 0 (default 0.0)
        b.binary(ir::Inst::Tag::Store, sig, real(2.25), kLoc);            // wakes watcher: 2
        b.binary(ir::Inst::Tag::StoreNb, sig, real(4.5), kLoc);           // commits later: 5 (4.5 rounds)
        m.add_process(ir::Process{ .activation = ir::Activation::Initial, .span = { kLoc, kLoc }, .ir = b.finish() });
    }

    CHECK(run_module(m) == "0\n2\n5\n");
}

TEST_CASE("real bitcasts and integral conversions") {
    ir::Module  m;
    ir::Builder b(m.pool);
    const ir::Ref i32 = int_type(m.pool);
    const ir::Ref u64 = m.pool.put(
      ir::Type{ .kind = ir::Type::Kind::PackedArray, .element = m.pool.bit_type, .left = 63, .right = 0 });
    const auto real = [&](double v) { return b.constant(m.pool.put_real(m.pool.real_type, v), kLoc); };

    // $realtobits(1.0) is the IEEE-754 pattern of 1.0.
    const ir::Index bits = b.float_cvt(ir::Inst::Tag::FBitcast, real(1.0), u64, kLoc);
    display_int(b, bits); // 4607182418800017408

    // $bitstoreal round-trip: pattern -> real -> FtoI.
    const ir::Index back = b.float_cvt(ir::Inst::Tag::FBitcast, bits, m.pool.real_type, kLoc);
    display_int(b, b.float_cvt(ir::Inst::Tag::FtoI, back, i32, kLoc)); // 1

    // ItoF reads x/z bits as 0 (LRM 6.12.1): 8'b1x1x -> 8'b1010 -> 10.0.
    const ir::Ref                logic8 = m.pool.put(
      ir::Type{ .kind = ir::Type::Kind::PackedArray, .element = m.pool.logic_type, .left = 7, .right = 0 });
    const std::array<uint8_t, 2> vbytes{ 0b1111, 0b0101 }; // val, xz planes
    const ir::Index              vx = b.constant(m.pool.put_value(logic8, vbytes), kLoc);
    const ir::Index              fv = b.float_cvt(ir::Inst::Tag::ItoF, vx, m.pool.real_type, kLoc);
    display_int(b, b.float_cvt(ir::Inst::Tag::FtoI, fv, i32, kLoc)); // 10

    // real <-> shortreal: FResize keeps the value for exactly-representable
    // numbers; 3.5 survives the f32 round-trip.
    const ir::Index sr  = b.float_cvt(ir::Inst::Tag::FResize, real(3.5), m.pool.shortreal_type, kLoc);
    const ir::Index rr  = b.float_cvt(ir::Inst::Tag::FResize, sr, m.pool.real_type, kLoc);
    display_int(b, b.float_cvt(ir::Inst::Tag::FtoI, rr, i32, kLoc)); // 4 (3.5 rounds away from zero)

    m.add_process(ir::Process{ .activation = ir::Activation::Initial, .span = { kLoc, kLoc }, .ir = b.finish() });
    CHECK(run_module(m) == "4607182418800017408\n1\n10\n4\n");
}

TEST_CASE("real math ops lower to intrinsics and libm calls") {
    ir::Module  m;
    ir::Builder b(m.pool);
    const ir::Ref i32  = int_type(m.pool);
    const auto    real = [&](double v) { return b.constant(m.pool.put_real(m.pool.real_type, v), kLoc); };
    const auto    show = [&](ir::Index v) { display_int(b, b.float_cvt(ir::Inst::Tag::FtoI, v, i32, kLoc)); };
    const auto    m1   = [&](ir::RealMathOp op, ir::Index x) {
        const std::array<ir::Index, 1> a{ x };
        return b.real_math(op, a, kLoc);
    };
    const auto m2 = [&](ir::RealMathOp op, ir::Index x, ir::Index y) {
        const std::array<ir::Index, 2> a{ x, y };
        return b.real_math(op, a, kLoc);
    };

    show(m1(ir::RealMathOp::Sqrt, real(9.0)));                  // 3
    show(m1(ir::RealMathOp::Floor, real(2.9)));                 // 2
    show(m1(ir::RealMathOp::Ceil, real(2.1)));                  // 3
    show(m2(ir::RealMathOp::Pow, real(2.0), real(10.0)));       // 1024
    show(m1(ir::RealMathOp::Exp, real(0.0)));                   // 1
    show(m1(ir::RealMathOp::Ln, real(1.0)));                    // 0
    show(m1(ir::RealMathOp::Log10, real(1000.0)));              // 3
    show(m2(ir::RealMathOp::Hypot, real(3.0), real(4.0)));      // 5
    show(m2(ir::RealMathOp::Atan2, real(0.0), real(1.0)));      // 0
    // Pure-libm entries (no LLVM intrinsic): JIT symbol resolution risk.
    show(m1(ir::RealMathOp::Tanh, real(0.0)));                  // 0
    show(m1(ir::RealMathOp::Asinh, real(0.0)));                 // 0
    show(m1(ir::RealMathOp::Acosh, real(1.0)));                 // 0

    m.add_process(ir::Process{ .activation = ir::Activation::Initial, .span = { kLoc, kLoc }, .ir = b.finish() });
    CHECK(run_module(m) == "3\n2\n3\n1024\n1\n0\n3\n5\n0\n0\n0\n0\n");
}

TEST_CASE("real display marshals through rt_fwrite with %g / %f") {
    ir::Module  m;
    ir::Builder b(m.pool);
    const auto real = [&](double v) { return b.constant(m.pool.put_real(m.pool.real_type, v), kLoc); };

    const std::array<ir::Index, 2> args{ real(3.6), real(0.25) };
    b.display("%g %f", true, args, kLoc);

    // A shortreal marshals by widening to double.
    const ir::Index sr = b.float_cvt(ir::Inst::Tag::FResize, real(1.5), m.pool.shortreal_type, kLoc);
    const std::array<ir::Index, 1> args2{ sr };
    b.display("%g", true, args2, kLoc);

    m.add_process(ir::Process{ .activation = ir::Activation::Initial, .span = { kLoc, kLoc }, .ir = b.finish() });
    CHECK(run_module(m) == "3.6 0.250000\n1.5\n");
}

// -- V1 static process fusion --

TEST_CASE("identical-sensitivity clocked processes fuse and stay correct") {
    // Three same-clock NBA processes with cross-reads: every read must observe
    // the pre-edge value regardless of the fused member order (NBA semantics),
    // so the settled values after two edges are order-independent and any
    // divergence from the unfused schedule is a fusion bug.
    constexpr std::string_view src = R"(
        module top;
            reg clk = 0;
            reg [7:0] a = 1, b = 10, c = 100;
            always @(posedge clk) a <= a + c;
            always @(posedge clk) b <= a + b;
            always @(posedge clk) c <= b + c;
            initial begin
                #5 clk = 1;
                #5 clk = 0;
                #5 clk = 1;
                #1 $display("%0d %0d %0d", a, b, c);
                $finish;
            end
        endmodule
    )";
    // Edge 1: a=101, b=11, c=110. Edge 2: a=211, b=112, c=121.
    CHECK(run_capture(src) == "211 112 121\n");
    // All three lower into one fused unit: a thunk by default (suspension-free),
    // a coroutine under --no-thunk.
    CHECK(count_substr(emit(src), ".f3.thunk(ptr noalias %state") == 1);
    CHECK(count_substr(emit(src, { .thunk = false }), ".f3(ptr %state") == 1);
}

TEST_CASE("thunk lowering: eligibility matrix and coroutine equivalence") {
    // A suspension-free always-with-sensitivity (no #delay / intra-body @wait /
    // task call, and no blocking write to its own sensitivity) elides its
    // coroutine and lowers to a plain per-activation thunk. Constructs that can
    // suspend, or self-wake, must stay coroutines.
    constexpr std::string_view src = R"(
        module top;
            reg clk = 0;
            reg [7:0] a = 0, b = 0, x = 0, y = 0, w = 0;
            always @(posedge clk) a <= a + 8'd1;          // clocked NBA -> thunk
            always @(b)           x = b + 8'd1;           // level comb   -> thunk
            always @(posedge clk) begin #1 y <= y + 8'd1; end // #delay   -> coroutine
            always @(w) if (w < 8'd3) w = w + 8'd1;       // self-write   -> coroutine
            initial begin #5 clk = 1; b = 8'd7; #5 clk = 0; #5 $finish; end
        endmodule
    )";
    // Exactly the two simple always blocks become thunks; the two non-elidable
    // always blocks and the initial stay coroutines.
    CHECK(count_substr(emit(src), ".thunk(ptr noalias %state") == 2);
    // Under --no-thunk nothing elides, and there are strictly more coroutine
    // frames (the two simple procs are coroutines again).
    CHECK(count_substr(emit(src, { .thunk = false }), ".thunk(") == 0);
    CHECK(count_substr(emit_raw(src), "call ptr @llvm.coro.begin")
          < count_substr(emit_raw(src, { .thunk = false }), "call ptr @llvm.coro.begin"));
    // Elision is behavior-preserving: the same design produces byte-identical
    // output with thunks on and off.
    CHECK(run_capture(src) == run_capture(src, { .thunk = false }));
}

TEST_CASE("suspension points and self-sensitivity writes block fusion") {
    // p1/p2 share @(posedge clk) but p2 contains a #delay: a suspended fused
    // body would stall its sibling, so it must stay a separate coroutine.
    // p3/p4 share @(a or b) but p4 blocking-writes b, a signal in its own
    // sensitivity set: fused, the self-wake the unfused schedule performs
    // would be lost under the held sched flag.
    constexpr std::string_view src = R"(
        module top;
            reg clk = 0;
            reg [7:0] a = 0, b = 0, x = 0, y = 0;
            always @(posedge clk) x <= x + 8'd1;
            always @(posedge clk) begin #1 y <= y + 8'd1; end
            always @(a or b) x = a;
            always @(a or b) b = a;
        endmodule
    )";
    const std::string ir = emit(src);
    CHECK(count_substr(ir, ".f2(") == 0);
    CHECK(count_substr(ir, ".f3(") == 0);
}

TEST_CASE("time-zero stores do not overflow the active queue via self-start re-wakes") {
    // Regression: the seed loop pushed self-start processes (port copies /
    // continuous assigns) without setting their scheduled flag, so an initial
    // block draining first at time zero re-woke the still-queued copies and the
    // duplicate entries overflowed the fixed-capacity active queue (a segfault
    // at millions-of-entries scale designs; corruption here).
    // Cross-checked against iverilog.
    const std::string out = run_capture(R"(
        module sub(input a, input b, output y);
            assign y = a & b;
        endmodule
        module top;
            reg a, b;
            wire y;
            sub u(a, b, y);
            initial begin
                a = 1; b = 1;
                #1 $display("y=%b", y);
            end
        endmodule
    )");
    CHECK(out == "y=1\n");
}

// ---- The `ref` ABI shape: narrow by default, fat only where it is load-bearing.

// Matching the four-word form to the actual is a silent-corruption seam: a caller
// and callee disagreeing about the arity write past each other's arguments, and no
// test that only checks values would notice. So these assert the emitted shape
// directly, in both directions, alongside the behaviour.

TEST_CASE("a ref to a place that cannot move is one pointer, with no epoch check") {
    // A ref formal carries the relocation epoch only when some call site binds it
    // inside a container's heap buffer (docs/design/data-model.md §3). A plain
    // variable's storage never moves, so the fat form would be three dead words and
    // a branch that can never be taken.
    const std::string ir = emit_raw(R"(
        module m;
            int v;
            function automatic void f(ref int r); r = r + 1; endfunction
            initial begin v = 5; f(v); $display("%0d", v); end
        endmodule
    )");
    CHECK(count_substr(ir, "call void @m.fn.f(ptr %state, ptr @clotho.inst.m.state)") == 1);
    CHECK(count_substr(ir, "rt_ref_stale") == 0);
    CHECK(run_capture(R"(
        module m;
            int v;
            function automatic void f(ref int r); r = r + 1; endfunction
            initial begin v = 5; f(v); $display("%0d", v); end
        endmodule
    )") == "6\n");
}

TEST_CASE("a ref to a whole container handle is one pointer") {
    // The handle POD is design state: it never moves, only the buffer it points at
    // does. So a `ref` to the WHOLE queue needs no epoch even though a ref to one of
    // its elements does -- the two cases differ by which allocation is aliased.
    const std::string ir = emit_raw(R"(
        module m;
            int q[$];
            function automatic void f(ref int r[$]); r.push_back(9); endfunction
            initial begin q.push_back(5); f(q); $display("%0d", q.size()); end
        endmodule
    )");
    CHECK(count_substr(ir, "call void @m.fn.f(ptr %state, ptr @clotho.inst.m.state)") == 1);
}

TEST_CASE("a ref to a container element keeps the epoch words") {
    // The one shape that earns them: `slot` is an interior pointer into the heap
    // ring, and the epoch it is checked against lives in the handle -- a different
    // allocation, which is exactly why a stale pointer cannot reach its own witness.
    const std::string ir = emit_raw(R"(
        module m;
            int q[$];
            function automatic void f(ref int r); r = r + 1; endfunction
            initial begin q.push_back(5); f(q[0]); $display("%0d", q[0]); end
        endmodule
    )");
    CHECK(ir.find("i64 %ref.epoch.at.call") != std::string::npos);
    // The site record is the callee's, built at the guard, so it is not an operand
    // of the call; the container's name reaches the report through the epoch table.
    CHECK(ir.find("@clotho.ref.names") != std::string::npos);
    CHECK(ir.find("@clotho.ref.site") != std::string::npos);
}

TEST_CASE("one ref formal reached by both actual shapes stays fat at every call") {
    // The fact is per FORMAL, not per call site: one heap-rooted actual anywhere
    // makes the formal fat, and the variable call site then passes the no-epoch
    // sentinel rather than a shorter argument list.
    constexpr std::string_view src = R"(
        module m;
            int q[$]; int v;
            function automatic void f(ref int r); r = r + 1; endfunction
            initial begin
                q.push_back(5); v = 1;
                f(v); f(q[0]);
                $display("%0d %0d", v, q[0]);
            end
        endmodule
    )";
    const std::string          ir  = emit_raw(src);
    // The sentinel appears as a call OPERAND (the global's own definition line
    // matches the bare name too, hence the surrounding argument text).
    CHECK(count_substr(ir, ", ptr @clotho.ref.noepoch, i64 0)") == 1);
    CHECK(count_substr(ir, "call void @m.fn.f(ptr %state, ptr @clotho.inst.m.state)") == 0);
    CHECK(run_capture(src) == "2 6\n");
}

TEST_CASE("virtual overrides sharing a vtable slot agree on the ref ABI") {
    // Dispatch picks the body at run time, so the caller cannot know which override
    // it reaches. If one sibling took the fat form and another the narrow one, an
    // indirect call would write three arguments the callee never declared. The
    // analysis unions the answer across the slot: here only D::g is passed a queue
    // element, and that must widen B::g too.
    constexpr std::string_view src = R"(
        class B; virtual function void g(ref int r); r = r + 1; endfunction endclass
        class D extends B; virtual function void g(ref int r); r = r + 2; endfunction endclass
        module m;
            int q[$]; int v;
            initial begin
                B b; D d;
                q.push_back(5); v = 1;
                b = new; d = new;
                b.g(v);
                d.g(q[0]);
                $display("%0d %0d", v, q[0]);
            end
        endmodule
    )";
    const std::string          ir  = emit_raw(src);
    // Both bodies take the SAME parameter list: state, the object handle pair, then
    // the three ref words. Compared as lists rather than as whole symbol names --
    // LLVM's uniquing suffix on the second `g` shifts as the module gains unrelated
    // functions, and that is not what this case is about.
    const std::vector<std::string> sigs = define_params(ir, "fn.g");
    REQUIRE(sigs.size() == 2);
    CHECK(sigs[0] == sigs[1]);
    CHECK(sigs[0] == "(ptr noalias %state, i128 %0, i64 %1, ptr %2, ptr %3, i64 %4)");
    CHECK(run_capture(src) == "2 7\n");

    // And with no heap-rooted actual at either site, the whole slot narrows.
    const std::string narrow = emit_raw(R"(
        class B; virtual function void g(ref int r); r = r + 1; endfunction endclass
        class D extends B; virtual function void g(ref int r); r = r + 2; endfunction endclass
        module m;
            int v;
            initial begin
                B b; D d;
                v = 1; b = new; d = new;
                b.g(v); d.g(v);
                $display("%0d", v);
            end
        endmodule
    )");
    const std::vector<std::string> nsigs = define_params(narrow, "fn.g");
    REQUIRE(nsigs.size() == 2);
    CHECK(nsigs[0] == nsigs[1]);
    CHECK(nsigs[0] == "(ptr noalias %state, i128 %0, i64 %1, ptr %2)");
}
