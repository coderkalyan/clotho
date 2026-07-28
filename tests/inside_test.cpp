#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module's initial blocks and return everything written to stdout.
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

void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
}

} // namespace

// The `inside` set-membership operator (LRM 11.4.13). Singular members compare
// with ==? (only the member's x/z are wildcards); a [lo:hi] range is
// sel>=lo && sel<=hi; the whole result is the OR-reduction of the members.
// Definite (0/1) results cross-checked against Verilator 5.048; iverilog 13 does
// not implement the inside operator ("sorry: inside expressions not supported").
TEST_CASE("inside: singular membership and ranges (LRM 11.4.13)") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] a;
          initial begin
            a = 4'd5;  $display("%b", a inside {4'd1, 4'd5, 4'd9});          // 1
            a = 4'd5;  $display("%b", a inside {4'd1, 4'd2});                // 0
            a = 4'd7;  $display("%b", a inside {[4'd5:4'd10]});             // 1
            a = 4'd3;  $display("%b", a inside {[4'd5:4'd10]});             // 0
            a = 4'd5;  $display("%b", a inside {[4'd5:4'd10]});             // 1 (lo boundary)
            a = 4'd10; $display("%b", a inside {[4'd5:4'd10]});            // 1 (hi boundary)
            a = 4'd11; $display("%b", a inside {[4'd5:4'd10]});            // 0
            a = 4'd8;  $display("%b", a inside {4'd1, [4'd5:4'd10], 4'd15}); // 1 (mixed)
          end
        endmodule
    )");
    CHECK(out == "1\n0\n1\n0\n1\n1\n0\n1\n");
}

// A member's x/z bits are wildcards (==? semantics); the left operand's are not.
TEST_CASE("inside: member x/z bits are wildcards") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] a;
          initial begin
            a = 4'b1011; $display("%b", a inside {4'b10x1}); // 1 (bit1 wildcard)
            a = 4'b0011; $display("%b", a inside {4'b10x1}); // 0 (bit3 mismatch)
          end
        endmodule
    )");
    CHECK(out == "1\n0\n");
}

// Left-operand x/z is NOT a wildcard: an uncertain compare with no definite match
// yields x (LRM 11.4.13, ==? propagates from the left operand's unknowns).
TEST_CASE("inside: unknown left operand yields x when no definite match") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] a;
          initial begin
            a = 4'b00xx; $display("%b", a inside {4'b0000}); // x (low bits x vs 0)
            a = 4'bxxxx; $display("%b", a inside {4'b10x1}); // x (no definite mismatch)
          end
        endmodule
    )");
    CHECK(out == "x\nx\n");
}

TEST_CASE("inside: usable as an if / while condition") {
    const std::string out = run_capture(R"(
        module m;
          logic [7:0] a;
          initial begin
            a = 8'd42;
            if (a inside {[8'd40:8'd50]}) $display("in"); else $display("out");
            a = 8'd7;
            if (a inside {8'd1, 8'd2, 8'd3}) $display("in"); else $display("out");
          end
        endmodule
    )");
    CHECK(out == "in\nout\n");
}

TEST_CASE("inside: signed range membership") {
    // Common comparison type is signed (both operands signed), so the range
    // compare is signed: -3 lies inside [-5:5].
    const std::string out = run_capture(R"(
        module m;
          logic signed [7:0] a;
          initial begin
            a = -3;  $display("%b", a inside {[-5:5]});  // 1
            a = -9;  $display("%b", a inside {[-5:5]});  // 0
          end
        endmodule
    )");
    CHECK(out == "1\n0\n");
}

// Unsupported forms fail loudly (never silently accept).
TEST_CASE("inside: unsupported forms are rejected") {
    check_rejected(R"(
        module m;
          string s;
          initial begin s = "hi"; $display("%b", s inside {"hi", "bye"}); end
        endmodule
    )",
                   "string operands");

    check_rejected(R"(
        module m;
          int a; int mem [0:3];
          initial begin a = 2; $display("%b", a inside {mem}); end
        endmodule
    )",
                   "unpacked-array set member");

    check_rejected(R"(
        module m;
          real a;
          initial begin a = 2.0; $display("%b", a inside {[1.0:3.0]}); end
        endmodule
    )",
                   "real operands");
}
