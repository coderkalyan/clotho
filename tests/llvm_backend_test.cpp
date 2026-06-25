#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
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

    // One coroutine per process, named @<module>.<source byte offset>: two
    // combinational assigns + one sequential always block. Each is a switched-
    // resume coroutine ramp returning the handle and taking the bare state ptr plus
    // the timing-wheel ptr. (CoroSplit also emits .resume/.destroy/.cleanup parts,
    // which don't match this "define ptr @alu." prefix.)
    CHECK(count_substr(ir, "define ptr @alu.") == 3);
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
    // Driven from an initial (not assigns) so the state buffer is live in main and
    // survives optimization — only scheduled processes touch state, and combinational
    // assigns are not scheduled yet.
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
