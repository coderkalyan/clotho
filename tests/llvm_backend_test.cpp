#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"

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

    // State is an opaque byte buffer, not a typed LLVM struct: main stack-
    // allocates an `[N x i8]` and there is no named `%alu.state` type.
    CHECK(ir.find("alloca [") != std::string::npos);
    CHECK(ir.find("%alu.state") == std::string::npos);

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

    // Signal access via constant-offset i8 GEP + load/store.
    CHECK(ir.find("getelementptr inbounds i8, ptr") != std::string::npos);
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

    // No typed value/state struct — state is laid out by clotho into an opaque
    // byte buffer (the only named types are coroutine frames, an unrelated
    // lowering artifact). The 4-state z (logic[7:0], align 2) packs ahead of the
    // 2-state x/y (bit[3:0], align 1); the buffer is 8 bytes, align 8.
    CHECK(ir.find("%m.state") == std::string::npos);
    CHECK(ir.find("alloca [8 x i8], align 8") != std::string::npos);
    // z's must|may word sits first at offset 0 as an i16 (2W = 16).
    CHECK(ir.find("store i16") != std::string::npos);
    CHECK(ir.find("define ptr @m.") != std::string::npos);
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

TEST_CASE("power lowers to an rt_pow_u64 call") {
    const std::string ir = emit(R"(
        module m;
            initial begin
                logic [7:0] a, b, c;
                a = 8'd17; b = 8'd5;
                c = a ** b;
            end
        endmodule
    )");
    CHECK(ir.find("call i64 @rt_pow_u64(") != std::string::npos);
}

TEST_CASE("unsupported divide/modulo/power forms are rejected, not miscompiled") {
    // The backend refuses (returns no module) rather than silently emitting wrong
    // code for the unimplemented corners: wide divide/modulo (needs 128-bit
    // division libcalls), signed power, and wide power.
    auto rejects = [](std::string_view src) {
        const LowerResult r = lower_text(src);
        REQUIRE_MESSAGE(r.ok, r.diagnostics);
        REQUIRE(r.module != nullptr);
        std::string              text;
        llvm::raw_string_ostream os(text);
        return !ir::emit_llvm(*r.module, os);
    };

    CHECK(rejects(R"(module m; initial begin logic [99:0] a, b, c; a = 1; b = 1; c = a / b; end endmodule)"));
    CHECK(rejects(R"(module m; initial begin logic [99:0] a, b, c; a = 1; b = 1; c = a % b; end endmodule)"));
    CHECK(rejects(R"(module m; initial begin int a, b, c; a = 2; b = 3; c = a ** b; end endmodule)"));
    CHECK(rejects(R"(module m; initial begin logic [99:0] a, b, c; a = 2; b = 3; c = a ** b; end endmodule)"));
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
