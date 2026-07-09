#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include <cstdio>
#include <unistd.h>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"

// Dynamic domain specialization (compute_domain_spec in LlvmBackend.cpp): an
// eligible thunk group carries a second assume-clean body dispatched on a
// per-group dirty counter maintained at write sites. These tests pin the
// runtime transition machinery (tag flips through blocking stores, partial
// stores, NBA commits, and the fast body's own stores) against iverilog-
// verified outputs, and always A/B the specialized run against
// --no-domain-spec — the two lowerings must agree byte-for-byte.

using namespace clotho;

namespace {

std::string run_capture(std::string_view source, ir::SimulationConfig config = {}) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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

// Run with specialization on and off; the outputs must match each other and
// the expected (iverilog-verified) text. Returns the specialized output.
std::string run_ab(std::string_view source, std::string_view expected) {
    // min_ops = 0: exercise the machinery on deliberately tiny bodies that
    // the default profitability gate would (correctly) skip.
    const std::string spec   = run_capture(source, { .domain_spec_min_ops = 0 });
    const std::string nospec = run_capture(source, { .domain_spec = false });
    CHECK_EQ(spec, nospec);
    CHECK_EQ(spec, expected);
    return spec;
}

// Emit unoptimized LLVM IR to assert on the dispatch structure (dom.fast
// blocks form only for eligible groups).
std::string emit_raw(std::string_view source) {
    const LowerResult result = lower_text(source);
    REQUIRE_MESSAGE(result.ok, result.diagnostics);
    REQUIRE(result.module != nullptr);
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*result.module, os, { .optimize = false, .domain_spec_min_ops = 0 }));
    return text;
}

} // namespace

TEST_CASE("blocking-store domain round trip: x -> clean -> x -> clean") {
    // The reader thunk must follow every transition: the fast body may only
    // run while `a` is clean, and each re-dirtying must fall back to the
    // generic body (iverilog: xx / 11 / xx / 21).
    run_ab(R"(
        module top;
          reg [7:0] a;
          reg clk = 0;
          reg [7:0] y = 0;
          always @(posedge clk) y = a + 8'd1;
          initial begin
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            a = 8'h10;
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            a = 8'hxx;
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            a = 8'h20;
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            $finish(0);
          end
        endmodule
    )",
           "xx\n11\nxx\n21\n");
}

TEST_CASE("partial stores keep a signal dirty until the whole word is clean") {
    // First part-write leaves the upper half x (the add must still produce x);
    // the second part-write clears the last x bit and must promote the signal
    // — the full stored word is retested, not just the written slice
    // (iverilog: xxxxxxxx / 0000cdac).
    run_ab(R"(
        module top;
          reg [15:0] a;
          reg clk = 0;
          reg [31:0] s = 0;
          always @(posedge clk) s = {16'h0, a} + 32'd1;
          initial begin
            a[7:0] = 8'hAB;
            #1 clk = 1; #1 clk = 0;
            $display("%h", s);
            a[15:8] = 8'hCD;
            #1 clk = 1; #1 clk = 0;
            $display("%h", s);
            $finish(0);
          end
        endmodule
    )",
           "xxxxxxxx\n0000cdac\n");
}

TEST_CASE("NBA commits transition the domain tag, not the staging store") {
    // A clean signal is re-dirtied by a nonblocking x assignment (committed in
    // the NBA region) and later re-promoted the same way; the reader must
    // print the x product in between (iverilog: 0a / 0a / xx / 0e).
    run_ab(R"(
        module top;
          reg clk = 0;
          reg [7:0] a = 8'h05;
          reg [7:0] y = 0;
          reg [2:0] n = 0;
          always @(posedge clk) y = a * 8'd2;
          always @(negedge clk) begin
            n = n + 3'd1;
            if (n == 3'd2) a <= 8'hxx;
            if (n == 3'd3) a <= 8'h07;
          end
          initial begin
            #1 clk=1; #1 clk=0;
            $display("%h", y);
            #1 clk=1; #1 clk=0;
            $display("%h", y);
            #1 clk=1; #1 clk=0;
            $display("%h", y);
            #1 clk=1; #1 clk=0;
            $display("%h", y);
            $finish(0);
          end
        endmodule
    )",
           "0a\n0a\nxx\n0e\n");
}

TEST_CASE("a fast body's own store promotes a downstream group's read set") {
    // Once `a` goes clean, thunk1's (fast) store of the clean sum must clear
    // b's tag so the woken comb reader can dispatch its own fast body; at t0
    // the x-valued b never changes (x -> x), so the comb thunk never wakes
    // (iverilog: "xx 00" then "31 32").
    run_ab(R"(
        module top;
          reg clk = 0;
          reg [7:0] a;
          reg [7:0] b;
          reg [7:0] y = 0;
          always @(posedge clk) b = a + 8'd1;
          always @(b) y = b + 8'd1;
          initial begin
            #1 clk=1; #1 clk=0;
            $display("%h %h", b, y);
            a = 8'h30;
            #1 clk=1; #1 clk=0;
            $display("%h %h", b, y);
            $finish(0);
          end
        endmodule
    )",
           "xx 00\n31 32\n");
}

TEST_CASE("flap pin: a signal oscillating between domains stays correct") {
    // `a` flips clean/x every cycle, far past domain_flap_cap (16), so the
    // signal pins permanently dirty partway through — after which the reader
    // must keep following the oscillation through its generic body. 60 edges
    // of alternation, sampled at the end on both phases.
    run_ab(R"(
        module top;
          reg clk = 0;
          reg [7:0] a = 8'h01;
          reg [7:0] y = 0;
          reg [7:0] n = 0;
          always @(posedge clk) y = a + 8'd1;
          always @(negedge clk) begin
            n = n + 8'd1;
            if (n[0]) a = 8'hxx;
            else a = {4'h0, n[3:0]};
          end
          initial begin
            repeat (60) begin #1 clk = 1; #1 clk = 0; end
            $display("%h", y);
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            $finish(0);
          end
        endmodule
    )",
           "xx\n0d\nxx\n");
}

TEST_CASE("provably in-range dynamic bit-select stays eligible and exact") {
    // A 3-bit index zero-extends to at most 7 < 8 = base width, so the select
    // can never read the out-of-range x that separates the domains; the fast
    // body may keep it (iverilog: xx / 03 / 08).
    run_ab(R"(
        module top;
          reg [7:0] a;
          reg [2:0] i;
          reg clk = 0;
          reg [7:0] y = 0;
          always @(posedge clk) y = {7'b0, a[i]} + {5'b0, i};
          initial begin
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            a = 8'hA5; i = 3'd2;
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            i = 3'd7;
            #1 clk = 1; #1 clk = 0;
            $display("%h", y);
            $finish(0);
          end
        endmodule
    )",
           "xx\n03\n08\n");
    // The bounded form specializes; the 8-bit-index form (256 > 8) cannot.
    const std::string ok = emit_raw(R"(
        module top;
          reg [7:0] a; reg [2:0] i; reg clk = 0; reg [7:0] y = 0;
          always @(posedge clk) y = {7'd0, a[i]};
          initial begin #1 clk = 1; $finish(0); end
        endmodule
    )");
    CHECK(ok.find("dom.fast") != std::string::npos);
}

TEST_CASE("dispatch structure: fast body forms only for eligible groups") {
    // Eligible: plain arithmetic on 4-state scalars.
    const std::string ok = emit_raw(R"(
        module top;
          reg [7:0] a; reg clk = 0; reg [7:0] y = 0;
          always @(posedge clk) y = a + 8'd1;
          initial begin #1 clk = 1; $finish(0); end
        endmodule
    )");
    CHECK(ok.find("dom.fast") != std::string::npos);
    CHECK(ok.find("clotho.domain_ctr") != std::string::npos);

    // Division can turn a clean input into x (divide by zero) — ineligible.
    const std::string div = emit_raw(R"(
        module top;
          reg [7:0] a; reg clk = 0; reg [7:0] y = 0;
          always @(posedge clk) y = a / 8'd2;
          initial begin #1 clk = 1; $finish(0); end
        endmodule
    )");
    CHECK(div.find("dom.fast") == std::string::npos);

    // casez wildcard patterns carry x/z literal bits — ineligible.
    const std::string csz = emit_raw(R"(
        module top;
          reg [7:0] a; reg clk = 0; reg [7:0] y = 0;
          always @(posedge clk) casez (a) 8'b1???????: y = 8'd1; default: y = 8'd2; endcase
          initial begin #1 clk = 1; $finish(0); end
        endmodule
    )");
    CHECK(csz.find("dom.fast") == std::string::npos);

    // A dynamic bit-select whose index range exceeds the base can read x out
    // of range — ineligible (the in-range 3-bit form is admitted; see the
    // bounded-select test).
    const std::string bsel = emit_raw(R"(
        module top;
          reg [7:0] a; reg [7:0] i; reg clk = 0; reg [7:0] y = 0;
          always @(posedge clk) y = {7'd0, a[i]};
          initial begin #1 clk = 1; $finish(0); end
        endmodule
    )");
    CHECK(bsel.find("dom.fast") == std::string::npos);
}
