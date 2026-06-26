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
