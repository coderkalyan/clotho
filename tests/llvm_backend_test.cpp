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
    // resume coroutine ramp returning the handle and taking a bare state ptr.
    // (CoroSplit also emits .resume/.destroy/.cleanup parts, which don't match
    // this "define ptr @alu." prefix.)
    CHECK(count_substr(ir, "define ptr @alu.") == 3);
    CHECK(ir.find("(ptr %state)") != std::string::npos);

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
    constexpr std::string_view src = R"(
        module m(input bit [3:0] x, output bit [3:0] y, output logic [7:0] z);
            assign y = x & x;
            assign z = 8'hAB;
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
