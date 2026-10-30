#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"

#include <cstdio>

#include <llvm/Support/raw_ostream.h>

using namespace clotho;

namespace {

// Emit unoptimized LLVM IR: these tests assert the backend's own emission
// structure (native iW ops, slot types), which the optimization pipeline is
// free to rewrite or constant-fold.
std::string emit(std::string_view source) {
    const LowerResult result = lower_text(source);
    REQUIRE_MESSAGE(result.ok, result.diagnostic_text());
    REQUIRE(result.module != nullptr);
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*result.module, os, { .lower_coro = false }));
    return text;
}

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
        // Binary: text mode would write "hi\r\n" on Windows, and this canary
        // checks the exact bytes $fgets deposits.
        std::FILE* f = std::fopen(path, "wb");
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

// Regression: lower_arith/lower_divmod/lower_bitwise dispatched their native
// (2-state) fast path solely on the LEFT operand's domain (`if (a.two)`),
// blindly using the right operand's .mm as if it were also a bare iW value.
// A 2-state left operand (`int`) mixed with a 4-state right operand (`logic`)
// fed an i(2W) value where an iW was assumed (or vice versa): malformed LLVM
// IR (mismatched-width icmp/select/binop operands), which crashed inside
// LLVM's constant folder for a large enough design (sv-tests ivtest
// comp1000/comp1001) rather than just miscompiling. Verified vs iverilog 13.
TEST_CASE("arithmetic/bitwise ops mix a 2-state and a 4-state operand correctly") {
    const std::string out = run_capture(R"(
        module m;
            int a;
            logic [7:0] b;
            initial begin
                a = 17;
                b = 5;
                $display("%0d %0d %0d %0d %0d %0d %0d", a % b, a / b, a & b, a | b, a ^ b,
                         a + b, a - b);
            end
        endmodule
    )");
    CHECK(out == "2 3 1 21 20 22 12\n");
}

TEST_CASE("arithmetic/bitwise ops mix a 4-state left operand with a 2-state right operand") {
    const std::string out = run_capture(R"(
        module m;
            logic [7:0] a;
            int b;
            initial begin
                a = 17;
                b = 5;
                $display("%0d %0d %0d %0d %0d", a % b, a / b, a & b, a | b, a ^ b);
            end
        endmodule
    )");
    CHECK(out == "2 3 1 21 20\n");
}

// The call ABI is by domain (docs/design/kernel.md, Calls and the runtime
// boundary): a 2-state scalar formal or result is one native iW, an out / inout
// formal one pointer to an iW slot, and a 4-state scalar keeps the plane pair
// ((ptr, ptr) for an out / inout). Checked on unoptimized IR, where the
// declaration is the one the backend wrote.
TEST_CASE("a 2-state formal and result cross a call as one native iW") {
    const std::string ir = emit(R"(
        module m;
            bit [7:0] r8;
            logic [3:0] rl;
            function automatic bit [7:0] f2(input bit [7:0] a, input int b, output bit [3:0] o, inout bit [15:0] io);
                o  = a[3:0];
                io = io + 1;
                return a + b[7:0];
            endfunction
            function automatic logic [3:0] f4(input logic [3:0] a, output logic [3:0] o, inout bit [7:0] io);
                o = a;
                return ~a;
            endfunction
            initial begin
                bit [3:0] o2;
                bit [15:0] io2 = 16'd4;
                logic [3:0] o4;
                bit [7:0] io4 = 8'd9;
                r8 = f2(8'd1, 32'd2, o2, io2);
                rl = f4(4'b01xz, o4, io4);
            end
        endmodule
    )");
    // a: i8, b: i32, o: one pointer, io: one pointer; the result is a bare i8.
    CHECK(ir.find("i8 @m.fn.f2(ptr noalias %state, ptr %rng, i8 %0, i32 %1, ptr %2, ptr %3)") !=
          std::string::npos);
    // A 4-state formal is the i8 plane pair (i4 -> i8 packed, i4 z), an output its
    // (ptr, ptr), the 2-state inout one pointer; the result is the plane struct.
    CHECK(ir.find("{ i8, i4 } @m.fn.f4(ptr noalias %state, ptr %rng, i8 %0, i4 %1, ptr %2, ptr %3, ptr %4)") !=
          std::string::npos);
}

TEST_CASE("scalar formals and results of every domain cross calls intact") {
    // Function and task callers (frame-local temps) and a process caller (module
    // signal temps), value results of every width class, real, enum, 4-state, a
    // task that suspends between its inputs and its outputs, and a virtual method
    // with an output. Every line was cross-checked by hand; iverilog rejects
    // output arguments of a function, so it cannot be the oracle here.
    const std::string out = run_capture(R"(
        module top;
            typedef enum bit [2:0] { A, B, C } e_t;
            bit         b1;
            bit [7:0]   b8;
            int         i32;
            longint     i64;
            bit [64:0]  w65;
            byte        by;
            real        r;
            e_t         en;
            logic [7:0] l8;

            function automatic bit f1(input bit a, input bit b);            return a ^ b;  endfunction
            function automatic bit [7:0] f8(input bit [7:0] a);             return a + 8'd3; endfunction
            function automatic int fi(input int a, input int b);            return a * b - 1; endfunction
            function automatic longint fl(input longint a);                 return a << 3; endfunction
            function automatic bit [64:0] fw(input bit [64:0] a);           return {a[0], a[64:1]}; endfunction
            function automatic byte fy(input byte a);                       return -a; endfunction
            function automatic real fr(input real x, input int n);          return x * n; endfunction
            function automatic e_t fe(input e_t a);                         return (a == C) ? A : e_t'(a + 1); endfunction
            function automatic logic [7:0] fz(input logic [7:0] a);         return a | 8'h01; endfunction

            function automatic void outs(input int x, output int y, output bit [7:0] z, inout longint w,
                                         output bit q, inout real rr, output e_t e, output logic [3:0] lz);
                y  = x + 1;
                z  = x[7:0] ^ 8'hff;
                w  = w + 5;
                q  = x > 3;
                rr = rr * 2.0;
                e  = B;
                lz = 4'bz1x0;
            endfunction

            task automatic touts(input bit [15:0] x, output bit [15:0] y, inout int n, output bit p);
                #3;
                y = ~x;
                n = n - 1;
                #2 p = ^x;
            endtask

            function automatic int nested(input int x);
                int t;
                bit [7:0] u;
                longint w;
                bit q;
                real rr;
                e_t e;
                logic [3:0] lz;
                w = 10;
                rr = 1.5;
                outs(x, t, u, w, q, rr, e, lz);
                return t + u + int'(w) + q + int'(rr) + e + (lz === 4'bz1x0);
            endfunction

            task automatic nested_t(input bit [15:0] x, output bit [15:0] y);
                int n;
                bit p;
                n = 7;
                touts(x, y, n, p);
                y = y + n + p;
            endtask

            class C0;
                virtual function int vf(input bit [7:0] a, output int o);
                    o = a + 1;
                    return a * 2;
                endfunction
            endclass
            class C1 extends C0;
                virtual function int vf(input bit [7:0] a, output int o);
                    o = a + 100;
                    return a * 3;
                endfunction
            endclass

            initial begin
                int yy; bit [7:0] zz; longint ww; bit qq; real rrr; e_t ee; logic [3:0] lzz;
                bit [15:0] y16, y17; int n; bit p; int ov;
                C0 c;
                b1  = f1(1, 0);
                b8  = f8(8'hfe);
                i32 = fi(-7, 6);
                i64 = fl(64'h1234_5678_9abc_def0);
                w65 = fw(65'h1_0000_0000_0000_0001);
                by  = fy(8'sd100);
                r   = fr(1.25, 6);
                en  = fe(C);
                l8  = fz(8'b1010_x0z0);
                $display("%b %0h %0d %0h %0h %0d %f %0d %b", b1, b8, i32, i64, w65, by, r, en, l8);
                ww = 100;
                rrr = 2.5;
                outs(10, yy, zz, ww, qq, rrr, ee, lzz);
                $display("%0d %0h %0d %b %f %0d %b", yy, zz, ww, qq, rrr, ee, lzz);
                n = 20;
                touts(16'h0f0f, y16, n, p);
                $display("%0h %0d %b", y16, n, p);
                nested_t(16'h00ff, y17);
                $display("%0h %0d", y17, nested(9));
                c = new;
                $display("%0d %0d", c.vf(8'd5, ov), ov);
                begin
                    C1 d = new;
                    c = d;
                end
                $display("%0d %0d", c.vf(8'd5, ov), ov);
            end
        endmodule
    )");
    CHECK(out ==
          "1 1 -43 91a2b3c4d5e6f780 18000000000000000 -100 7.500000 0 1010x0x1\n"
          "11 f5 105 1 5.000000 1 z1x0\n"
          "f0f0 19 0\n"
          "ff06 277\n"
          "10 6\n"
          "15 105\n");
}
