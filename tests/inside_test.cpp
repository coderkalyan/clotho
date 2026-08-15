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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
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

TEST_CASE("inside: real values and ranges") {
    const std::string out = run_capture(R"(
        module m;
          real      r;
          shortreal sr;
          initial begin
            r = 1.5;
            $display("%b %b %b", r inside {1.5, 2.5}, r inside {0.5, 2.5}, r inside {[1.0:2.0]});
            $display("%b %b", r inside {[2.0:3.0]}, r inside {[1.5:1.5]});
            r = 2.0;
            $display("%b", r inside {2, 7});
            sr = 2.5;
            $display("%b %b", sr inside {2.5}, sr inside {[2.0:2.4]});
          end
        endmodule
    )");
    // An integral member converts to real (11.8.1), so `2` matches 2.0; a range
    // is closed at both ends.
    CHECK(out == "1 0 1\n0 1\n1\n1 0\n");
}

TEST_CASE("inside: an array member is traversed elementwise") {
    const std::string out = run_capture(R"(
        module m;
          int q[$];
          int d[];
          int fa [0:2];
          int n  [0:1][0:1];
          int eq[$];
          initial begin
            q = '{3, 4, 5};
            d = new[2]; d[0] = 8; d[1] = 9;
            fa = '{11, 12, 13};
            n[0][0] = 1; n[0][1] = 2; n[1][0] = 3; n[1][1] = 4;
            $display("%b %b", 4 inside {1, 2, q}, 7 inside {1, 2, q});
            $display("%b %b", 9 inside {d}, 1 inside {d});
            $display("%b %b", 12 inside {fa}, 14 inside {fa});
            $display("%b %b", 3 inside {n}, 9 inside {n});
            $display("%b", 5 inside {eq});
            $display("%b", 13 inside {[100:200], fa, q});
          end
        endmodule
    )");
    // A nested dimension descends; an empty queue contributes nothing; an array
    // member mixes with ordinary values and ranges in one set.
    CHECK(out == "1 0\n1 0\n1 0\n1 0\n0\n1\n");
}

TEST_CASE("inside: x/z in an array member element is a wildcard") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0] warr [0:1];
          initial begin
            warr[0] = 4'b1x01; warr[1] = 4'b0000;
            $display("%b %b", 4'b1101 inside {warr}, 4'b1110 inside {warr});
          end
        endmodule
    )");
    CHECK(out == "1 0\n");
}

// A real set member inside an array works through the same leaf comparison.
TEST_CASE("inside: a real left operand against a real array member") {
    const std::string out = run_capture(R"(
        module m;
          real rq[$];
          real r;
          initial begin
            rq = '{1.25, 2.5};
            r = 2.5;
            $display("%b %b", r inside {rq}, 3.5 inside {rq});
          end
        endmodule
    )");
    CHECK(out == "1 0\n");
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

    // An associative array has no index walk to share with the ordered
    // containers; its traversal is the first/next key pair (LRM 7.9.4).
    check_rejected(R"(
        module m;
          int a; int amap [string];
          initial begin a = 2; amap["k"] = 2; $display("%b", a inside {amap}); end
        endmodule
    )",
                   "associative-array set member");

    // Descending into a non-singular element of a VARIABLE-size member needs the
    // walk nested inside itself; the fixed-size member recurses instead.
    check_rejected(R"(
        module m;
          int a; int qq [$][0:1];
          initial begin a = 2; $display("%b", a inside {qq}); end
        endmodule
    )",
                   "not singular");
}
