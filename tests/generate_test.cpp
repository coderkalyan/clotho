#include <doctest/doctest.h>

#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture everything it writes to stdout. Generate-block
// behaviour is observed through the values its assigns/processes drive, printed
// by a single initial block after a settling #1 delay.
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

// All flattened signal names in the lowered module.
std::set<std::string> signal_names(const ir::Module& m) {
    std::set<std::string> names;
    for (const ir::SignalInfo& s : m.signals()) {
        names.insert(s.name);
    }
    return names;
}

} // namespace

// IEEE 1800-2023 27.4: a loop generate instantiates its body once per genvar
// value. Each instantiated assign drives its element; the result is observable.
TEST_CASE("for-generate instantiates the loop body per genvar value") {
    constexpr std::string_view src = R"(
        module m;
            wire [2:0] o;
            genvar i;
            generate
                for (i = 0; i < 3; i = i + 1) begin : g
                    assign o[i] = 1'b1;
                end
            endgenerate
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "111\n");
}

// 27.4: each loop entry is a distinct scope; its nets flatten under a
// subscripted hierarchical path so repeated bodies stay unique.
TEST_CASE("for-generate flattens each entry under a subscripted prefix") {
    const LowerResult r = lower_text(R"(
        module m;
            genvar i;
            generate
                for (i = 0; i < 2; i = i + 1) begin : g
                    wire [3:0] x;
                    assign x = i;
                end
            endgenerate
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("g[0].x"));
    CHECK(names.contains("g[1].x"));
}

// 27.5: a conditional generate instantiates the taken branch only; the other is
// uninstantiated and must not contribute a second driver.
TEST_CASE("if-generate selects the true branch") {
    constexpr std::string_view src = R"(
        module m;
            parameter EN = 1;
            wire [1:0] o;
            generate
                if (EN) begin : g
                    assign o = 2'b11;
                end else begin : g
                    assign o = 2'b00;
                end
            endgenerate
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "11\n");
}

TEST_CASE("if-generate selects the false branch") {
    constexpr std::string_view src = R"(
        module m;
            parameter EN = 0;
            wire [1:0] o;
            generate
                if (EN) begin : g
                    assign o = 2'b11;
                end else begin : g
                    assign o = 2'b00;
                end
            endgenerate
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "00\n");
}

// 27.5: a case generate instantiates the matching item's block.
TEST_CASE("case-generate selects the matching item") {
    constexpr std::string_view src = R"(
        module m #(parameter SEL = 1);
            wire [1:0] o;
            generate
                case (SEL)
                    0: begin : g assign o = 2'b00; end
                    1: begin : g assign o = 2'b01; end
                    default: begin : g assign o = 2'b10; end
                endcase
            endgenerate
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "01\n");
}

// Nested loop generates: the inner block elaborates once per (i, j) pair, each
// entry a distinct scope, and all assigns simulate.
TEST_CASE("nested for-generate instantiates the inner body per pair") {
    constexpr std::string_view src = R"(
        module m;
            wire [3:0] o;
            genvar i;
            generate
                for (i = 0; i < 2; i = i + 1) begin : outer
                    genvar j;
                    for (j = 0; j < 2; j = j + 1) begin : inner
                        assign o[i*2+j] = 1'b1;
                    end
                end
            endgenerate
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "1111\n");
}

// A procedural block inside a generate block lowers and runs like any other.
TEST_CASE("an always block inside a for-generate simulates") {
    constexpr std::string_view src = R"(
        module m;
            reg [2:0] o;
            wire clk;
            assign clk = 1'b0;
            genvar i;
            generate
                for (i = 0; i < 3; i = i + 1) begin : g
                    always @(*) o[i] = 1'b1;
                end
            endgenerate
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "111\n");
}

// A generate block instantiating a module flattens the child below the block's
// hierarchical prefix.
TEST_CASE("a module instance inside a for-generate flattens under the block") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module m(input g_in, output [1:0] o);
            genvar i;
            generate
                for (i = 0; i < 2; i = i + 1) begin : g
                    inv u(.a(g_in), .y(o[i]));
                end
            endgenerate
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("g[0].u.a"));
    CHECK(names.contains("g[0].u.y"));
    CHECK(names.contains("g[1].u.a"));
    CHECK(names.contains("g[1].u.y"));
}

// An unsupported construct inside a generate block must still fail loudly via
// the shared member walk, not be silently dropped.
TEST_CASE("an instance array inside a generate block fails loudly") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module m(input [3:0] g_in, output [3:0] o);
            genvar i;
            generate
                if (1) begin : g
                    inv u[3:0](.a(g_in), .y(o));
                end
            endgenerate
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("instance array") != std::string::npos);
}
