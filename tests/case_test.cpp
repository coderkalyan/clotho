#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module's initial blocks and return everything it wrote to stdout.
// The case statements under test sit in an `initial` block and report the chosen
// branch with $display, so the captured text is the observable behavior.
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

// IEEE 1800-2023 12.5 Example 1: literal x and z are DISTINCT case items, and a
// normal `case` matches them exactly (case-equality).
TEST_CASE("normal case matches literal x and z exactly (LRM 12.5 Ex.1)") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'b0z;
                case (sel)
                    2'b00: $display("zero");
                    2'b0x: $display("isx");
                    2'b0z: $display("isz");
                    default: $display("other");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "isz\n");
}

// 12.5.1 Example 1: casez treats z / ? as don't-care; the MSB selects the match.
TEST_CASE("casez treats ? as a wildcard (LRM 12.5.1 Ex.1)") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] ir;
            initial begin
                ir = 8'b1010_0000;
                casez (ir)
                    8'b1???????: $display("instr1");
                    8'b01??????: $display("instr2");
                    default:     $display("none");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "instr1\n");
}

// 12.5.1: casex treats x AND z as don't-care; the xx low bits are ignored.
TEST_CASE("casex treats x/z label bits as don't-care (LRM 12.5.1 Ex.2)") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] r;
            initial begin
                r = 8'b0110_0110;
                casex (r)
                    8'b011001xx: $display("stat1");
                    8'b1100xx00: $display("stat2");
                    default:     $display("none");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "stat1\n");
}

TEST_CASE("default fires only when nothing matched") {
    constexpr std::string_view hit = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd2;
                case (s)
                    2'd0: $display("a");
                    2'd1: $display("b");
                    default: $display("dflt");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(hit) == "dflt\n");
}

TEST_CASE("no match and no default executes nothing") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd3;
                case (s)
                    2'd0: $display("a");
                    2'd1: $display("b");
                endcase
                $display("after");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "after\n");
}

TEST_CASE("first matching item wins, search stops") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd1;
                case (s)
                    2'd1: $display("first");
                    2'd0: $display("zero");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "first\n");
}
