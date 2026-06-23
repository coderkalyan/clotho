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

    // A state struct with two i-fields per signal (val + xz planes).
    CHECK(ir.find("%alu.state = type") != std::string::npos);

    // One function per process, named @<module>.<source byte offset>: two
    // combinational assigns + one sequential always block.
    CHECK(count_substr(ir, "define void @alu.") == 3);

    // Datapath ops present.
    CHECK(ir.find(" add ") != std::string::npos);
    CHECK(ir.find(" sub ") != std::string::npos);

    // Signal access via struct GEP + load/store.
    CHECK(ir.find("getelementptr") != std::string::npos);
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

    // Deduplicated, nested signal value types: bit.4 used twice (x, y), logic.8
    // once (z); each is a { i(2W) must|may, iW z } struct nested in the state struct.
    CHECK(ir.find("%bit.4 = type { i8, i4 }") != std::string::npos);
    CHECK(ir.find("%logic.8 = type { i16, i8 }") != std::string::npos);
    CHECK(ir.find("%m.state = type { %bit.4, %bit.4, %logic.8 }") != std::string::npos);
    CHECK(ir.find("define void @m.") != std::string::npos);
}
