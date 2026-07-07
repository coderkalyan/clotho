#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"

#include <cstdio>
#include <unistd.h>

#include <llvm/Support/raw_ostream.h>

using namespace clotho;

namespace {

// Emit unoptimized LLVM IR: these tests assert the backend's own emission
// structure (native iW ops, slot types), which the optimization pipeline is
// free to rewrite or constant-fold.
std::string emit(std::string_view source) {
    const LowerResult result = lower_text(source);
    REQUIRE_MESSAGE(result.ok, result.diagnostics);
    REQUIRE(result.module != nullptr);
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*result.module, os, { .optimize = false }));
    return text;
}

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

// Wrap a body in a single initial block inside module m.
std::string prog(std::string_view decls, std::string_view body) {
    return "module m;\n" + std::string(decls) + "\n initial begin\n" + std::string(body) + "\n $finish;\n end\nendmodule\n";
}

} // namespace

// The whole point of stage 1: an integer op on 2-state operands lowers to a
// single native LLVM op on the iW value, not the 4-state must|may branch. Check
// the emitted (unoptimized) IR carries the native op at the value width.
TEST_CASE("2-state operators lower to native iW ops") {
    const std::string ir = emit(R"(
        module m;
            bit [7:0] a, b, c;
            initial begin
                a = 8'd10; b = 8'd3;
                c = a + b;
                c = a - b;
                c = a * b;
                c = a & b;
                c = a | b;
                c = a ^ b;
                c = ~a;
            end
        endmodule
    )");
    CHECK(ir.find("add i8") != std::string::npos);
    CHECK(ir.find("sub i8") != std::string::npos);
    CHECK(ir.find("mul i8") != std::string::npos);
    CHECK(ir.find("and i8") != std::string::npos);
    CHECK(ir.find("or i8") != std::string::npos);
    CHECK(ir.find("xor i8") != std::string::npos);
    // No 4-state arithmetic X-branch is emitted for 2-state arithmetic.
    CHECK(ir.find("arith.x") == std::string::npos);
}

// Arithmetic / bitwise / shift / compare / reduction results agree with the
// iverilog oracle (values checked against iverilog 13).
TEST_CASE("2-state arithmetic and logic behavior") {
    const std::string out = run_capture(prog(
      "bit [7:0] a, b;",
      R"(
        a = 8'd200; b = 8'd7;
        $display("%0d %0d %0d %0d %0d", a+b, a-b, a*b, a/b, a%b);
        $display("%0d %0d %0d %0d", a&b, a|b, a^b, ~a);
        $display("%0d %0d %0d %0d", a<<2, a>>2, a<b, a>=b);
        $display("%0d %0d %0d %0d", &a, |a, ^a, (a&&b));
        $display("%0d %0d", {a,b}, {2{b}});
      )"));
    CHECK(out ==
          "207 193 120 28 4\n"
          "0 207 207 55\n"
          "32 50 0 1\n"
          "0 1 1 1\n"
          "51207 1799\n");
}

// A signed 2-state value uses the signed native ops (ashr / sdiv / signed
// compares) and prints negative under %0d.
TEST_CASE("2-state signed operators") {
    const std::string out = run_capture(prog(
      "int a, b;",
      R"(
        a = -40; b = 3;
        $display("%0d %0d %0d", a/b, a%b, a>>>1);
        $display("%0d %0d", a<b, a<0);
      )"));
    CHECK(out ==
          "-13 -1 -20\n"
          "1 1\n");
}

// LRM 6.3.1: assigning a 4-state value to a 2-state target drops x/z to 0.
TEST_CASE("4-state to 2-state conversion drops x and z") {
    const std::string out = run_capture(prog(
      "bit [7:0] a; logic [7:0] lg;",
      R"(
        lg = 8'b1010_xz01;
        a = lg;
        $display("%0d", a);
      )"));
    // 1010_xz01 with x/z -> 0 gives 1010_0001 = 0xA1 = 161.
    CHECK(out == "161\n");
}

// A 2-state value widened to 4-state carries no x/z (round-trips cleanly).
TEST_CASE("2-state to 4-state conversion") {
    const std::string out = run_capture(prog(
      "bit [7:0] a; logic [7:0] lg;",
      R"(
        a = 8'd42;
        lg = a;
        $display("%0d %b", lg, lg);
      )"));
    CHECK(out == "42 00101010\n");
}

// The sized integer aliases lower to the underlying bit/logic type with the
// right width, signedness, and 2-state vs 4-state domain.
TEST_CASE("sized integer aliases: width, signedness, domain") {
    const std::string out = run_capture(prog(
      "byte sb; shortint sh; int si; longint sl; integer ig; bit b1;",
      R"(
        sb = -8'sd5; sh = -30000; si = -1234; sl = -64'sd5000000000; ig = -7;
        $display("%0d %0d %0d %0d %0d", sb, sh, si, sl, ig);
        // byte is 8 bits signed: 200 wraps to -56.
        sb = 8'd200; $display("%0d", sb);
      )"));
    CHECK(out ==
          "-5 -30000 -1234 -5000000000 -7\n"
          "-56\n");
}

// `integer` is 4-state (can hold x), unlike the 2-state `int`.
TEST_CASE("integer is 4-state, int is 2-state") {
    const std::string ir = emit(R"(
        module m;
            int     a;
            integer b;
            initial begin a = 0; b = 0; end
        endmodule
    )");
    // int -> a narrow single-iW bit slot; integer -> a 4-state logic slot.
    CHECK(ir.find("%bit.32 = type { i32 }") != std::string::npos);
    CHECK(ir.find("%logic.nz.32 = type { i64 }") != std::string::npos);
}

// A decoupled 2-state signal gets the genuine single-iW slot and native iW
// load/store, with no packed-word scaffolding.
TEST_CASE("2-state signal storage shrinks to a single iW slot") {
    const std::string ir = emit(R"(
        module m;
            bit [7:0] a, b;
            initial begin a = 8'd1; b = a + 8'd1; end
        endmodule
    )");
    CHECK(ir.find("%bit.8 = type { i8 }") != std::string::npos);
    CHECK(ir.find("load i8") != std::string::npos);
    CHECK(ir.find("store i8") != std::string::npos);
    // No packed i16 word for these signals.
    CHECK(ir.find("%bit.nz.8") == std::string::npos);
}

// A 2-state NBA-target signal narrows: both its live slot and NBA shadow are a
// single iW, and clocked nonblocking updates still commit correctly.
TEST_CASE("2-state NBA-target signal narrows to iW") {
    const std::string ir = emit(R"(
        module m;
            bit clk;
            bit [7:0] q;
            always @(posedge clk) q <= q + 8'd1;
        endmodule
    )");
    CHECK(ir.find("%bit.8 = type { i8 }") != std::string::npos);
    CHECK(ir.find("%bit.nz.8") == std::string::npos);

    // Clocked accumulate through the NBA commit path.
    const std::string out = run_capture(R"(
        module m;
            bit clk;
            bit [7:0] q;
            int i;
            always @(posedge clk) q <= q + 8'd1;
            initial begin
                for (i = 0; i < 5; i = i + 1) begin clk = 0; #1; clk = 1; #1; end
                $display("%0d", q);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "5\n");
}

// A plain 2-state memory shrinks to [N x { iW }] elements and reads/writes
// correctly.
TEST_CASE("2-state memory element storage shrinks to iW") {
    const std::string ir = emit(R"(
        module m;
            bit [7:0] a [0:3];
            initial begin a[0] = 8'd5; a[1] = a[0] + 8'd1; end
        endmodule
    )");
    CHECK(ir.find("[4 x %bit.8]") != std::string::npos);
    CHECK(ir.find("%bit.8 = type { i8 }") != std::string::npos);

    const std::string out = run_capture(prog(
      "bit [7:0] a [0:3]; int s;",
      R"(
        a[0]=8'd5; a[1]=8'd10; a[2]=8'd20; a[3]=8'd40;
        s = a[0]+a[1]+a[2]+a[3];
        $display("%0d %0d", s, a[2]);
      )"));
    CHECK(out == "75 20\n");
}

// A 2-state array that is an unpacked-array NBA target also narrows, and its
// clocked element updates commit correctly through the deferred queue.
TEST_CASE("2-state array NBA target narrows") {
    const std::string ir = emit(R"(
        module m;
            bit clk;
            bit [7:0] mem [0:3];
            always @(posedge clk) mem[0] <= mem[0] + 8'd1;
        endmodule
    )");
    CHECK(ir.find("[4 x %bit.8]") != std::string::npos);

    const std::string out = run_capture(R"(
        module m;
            bit clk;
            bit [7:0] mem [0:1];
            int i;
            always @(posedge clk) begin mem[0] <= mem[0] + 8'd1; mem[1] <= mem[1] + 8'd5; end
            initial begin
                mem[0] = 0; mem[1] = 0;
                for (i = 0; i < 3; i = i + 1) begin clk = 0; #1; clk = 1; #1; end
                $display("%0d %0d", mem[0], mem[1]);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "3 15\n");
}

// posedge / negedge detection works on a 2-state (bit) clock.
TEST_CASE("edge sensitivity on a bit clock") {
    const std::string out = run_capture(R"(
        module m;
            bit clk;
            int pos, neg;
            initial begin
                clk = 0;
                #1 clk = 1; #1 clk = 0; #1 clk = 1; #1 clk = 0; #1;
                $display("%0d %0d", pos, neg);
                $finish;
            end
            always @(posedge clk) pos = pos + 1;
            always @(negedge clk) neg = neg + 1;
        endmodule
    )");
    CHECK(out == "2 2\n");
}

// A 2-state value is truthy iff nonzero (no x path).
TEST_CASE("2-state truthiness in if / while") {
    const std::string out = run_capture(prog(
      "bit [3:0] a; int hit;",
      R"(
        a = 4'd0;  if (a) hit = 1; else hit = 2;
        $display("%0d", hit);
        a = 4'd5;  if (a) hit = 3; else hit = 4;
        $display("%0d", hit);
      )"));
    CHECK(out == "2\n3\n");
}

// Runtime pointer-ABI write-backs into narrow 2-state slots must write only the
// iW value word. Before the narrow flag was threaded through, each of these
// wrote the packed i(2W) word (or 2*capacity bytes), overrunning the slot into
// the adjacent state field — the canaries catch that.
TEST_CASE("$sformat into a narrow 2-state slot does not overrun (canary)") {
    const std::string out = run_capture(R"(
        module m;
            bit [31:0] dst, canary;
            initial begin
                canary = 32'hDEADBEEF;
                $sformat(dst, "ABCD");
                $display("%h %h", dst, canary);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "41424344 deadbeef\n");
}

TEST_CASE("$sscanf into a narrow 2-state slot does not overrun (canary)") {
    const std::string out = run_capture(R"(
        module m;
            int x, y, n;
            initial begin
                y = 32'h12345678;
                n = $sscanf("41", "%d", x);
                $display("%0d %0d %h", n, x, y);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "1 41 12345678\n");
}

TEST_CASE("scalar $fread into a narrow 2-state slot does not overrun (canary)") {
    const char* path = "clotho_fread_narrow_test.bin";
    {
        std::FILE* f = std::fopen(path, "wb");
        REQUIRE(f != nullptr);
        const unsigned char bytes[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        std::fwrite(bytes, 1, 4, f);
        std::fclose(f);
    }
    const std::string out = run_capture(R"(
        module m;
            bit [31:0] w, canary;
            int fd, n;
            initial begin
                canary = 32'hDEADBEEF;
                fd = $fopen("clotho_fread_narrow_test.bin", "r");
                n = $fread(w, fd);
                $fclose(fd);
                $display("%0d %h %h", n, w, canary);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "4 aabbccdd deadbeef\n");
    std::remove(path);
}

TEST_CASE("$fgets into a narrow 2-state slot does not overrun (canary)") {
    const char* path = "clotho_fgets_narrow_test.txt";
    {
        std::FILE* f = std::fopen(path, "w");
        REQUIRE(f != nullptr);
        std::fputs("hi\n", f);
        std::fclose(f);
    }
    const std::string out = run_capture(R"(
        module m;
            bit [31:0] s, canary;
            int fd, n;
            initial begin
                canary = 32'hDEADBEEF;
                fd = $fopen("clotho_fgets_narrow_test.txt", "r");
                n = $fgets(s, fd);
                $fclose(fd);
                $display("%0d %h %h", n, s, canary);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "3 0068690a deadbeef\n");
    std::remove(path);
}

// Regression: a 2-state NBA target commits through a NARROW bucket whose slot
// word is a single iW. The Table 9-2 edge classifier derives the may plane via
// LShr(word, width), which reads 0 on an iW word — posedge waiters on a bit
// clock driven by `<=` then never fire. emit_nba_service must select the plain
// 0<->1 classifier for a narrow bucket.
TEST_CASE("a 2-state NBA clock fires posedge and negedge waiters") {
    const std::string out = run_capture(R"(
        module m;
            bit clk;
            int pos = 0, neg = 0;
            always @(posedge clk) pos = pos + 1;
            always @(negedge clk) neg = neg + 1;
            initial begin
                repeat (3) begin
                    #1 clk <= 1'b1;
                    #1 clk <= 1'b0;
                end
                #1 $display("pos=%0d neg=%0d", pos, neg);
                $finish;
            end
        endmodule
    )");
    CHECK(out == "pos=3 neg=3\n");
}
