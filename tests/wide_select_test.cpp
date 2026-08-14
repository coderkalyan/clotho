
#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    return r;
}

std::string run_capture(std::string_view source) {
    const LowerResult r = lower_ok(source);

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

TEST_CASE("a variable bit-select of a very wide vector reads and writes in place") {
    // ivtest pr722 in miniature: one bit of a 65537-bit vector, addressed by a
    // variable. The whole vector is 8 KiB of storage; only its top word is touched.
    const std::string out = run_capture(R"(
        module m;
            reg [65536:0] mem;
            integer       i;
            initial begin
                mem = 0;
                i   = 65536;
                mem[i] = 1;
                $display("%b %b", mem[65536], mem[65535]);
                i = 0;
                mem[i] = 1;
                $display("%b %b", mem[0], mem[1]);
                i = 32768;
                $display("%b", mem[i]);
            end
        endmodule
    )");
    CHECK(out == "1 0\n1 0\n0\n");
}

TEST_CASE("a variable part-select of a very wide vector spans words") {
    // A 32-bit window at a runtime offset that straddles a 64-bit word boundary
    // (bit 8190 of an 8192-bit vector), and a 128-bit one that spans three words.
    const std::string out = run_capture(R"(
        module m;
            reg [8191:0] a;
            integer      i;
            initial begin
                a = 0;
                i = 8150;
                a[i +: 32] = 32'hdeadbeef;
                $display("%h", a[8181:8150]);
                i = 8150;
                $display("%h", a[i +: 32]);
                i = 1000;
                a[i +: 128] = 128'hfeedfacecafebeef0123456789abcdef;
                i = 1000;
                $display("%h", a[i +: 128]);
                i = 2000;
                a[i -: 16] = 16'h5a5a;
                $display("%h", a[2000:1985]);
            end
        endmodule
    )");
    CHECK(out == "deadbeef\ndeadbeef\nfeedfacecafebeef0123456789abcdef\n5a5a\n");
}

TEST_CASE("an out-of-range access to a wide vector reads x and writes nothing") {
    // LRM 11.5.1: an out-of-range read of a 4-state value is x, and a write
    // "shall have no effect"; a partially out-of-range access keeps exactly the
    // bits that are in range. A negative index is out of range like any other.
    const std::string out = run_capture(R"(
        module m;
            reg [8191:0] a;
            integer      i;
            initial begin
                a = 0;
                i = 9000;
                $display("%b", a[i]);
                i = -1;
                $display("%b", a[i]);
                i = 9000;
                a[i] = 1;
                $display("%h", a[63:0]);
                i = -1;
                a[i] = 1;
                $display("%h", a[63:0]);
                i = 8188;
                $display("%h", a[i +: 8]);
                a[i +: 8] = 8'hff;
                $display("%h %h", a[8191:8184], a[8183:8176]);
                a = 0;
                i = -4;
                a[i +: 8] = 8'hff;
                $display("%h", a[7:0]);
            end
        endmodule
    )");
    CHECK(out == "x\nx\n0000000000000000\n0000000000000000\nx0\nf0 00\n0f\n");
}

TEST_CASE("an x/z index of a wide vector is wholly out of range") {
    // LRM 11.5.1: "If the bitselect address is invalid ... then the value returned
    // shall be x"; the matching write is a no-op.
    const std::string out = run_capture(R"(
        module m;
            reg [8191:0] a;
            integer      xi;
            initial begin
                a  = 0;
                xi = 4'bxx01;
                $display("%b %h", a[xi], a[xi +: 8]);
                a[xi]      = 1'b1;
                a[xi +: 8] = 8'hff;
                $display("%h", a[63:0]);
            end
        endmodule
    )");
    CHECK(out == "x xx\n0000000000000000\n");
}

TEST_CASE("a wide-vector selection carries x and z payloads") {
    // The 4-state planes travel through the runtime helper intact, including a
    // selection wider than one word, and a 2-state vector keeps its 0-fill rule
    // for an out-of-range read.
    const std::string out = run_capture(R"(
        module m;
            reg [8191:0] a;
            bit [8191:0] b;
            integer      i;
            initial begin
                a = 0;
                i = 5000;
                a[i] = 1'bx;
                a[i + 1] = 1'bz;
                $display("%b %b", a[5000], a[5001]);
                i = 3000;
                a[i +: 96] = {32'haaaa5555, 32'hxxxxzzzz, 32'h00ff00ff};
                i = 3000;
                $display("%h", a[i +: 96]);
                b = 0;
                i = 4000;
                b[i +: 12] = 12'h987;
                i = 4000;
                $display("%h", b[i +: 12]);
                i = 9000;
                $display("%h", b[i +: 12]);
            end
        endmodule
    )");
    CHECK(out == "x z\naaaa5555xxxxzzzz00ff00ff\n987\n000\n");
}

TEST_CASE("a wide selection clamps to its container, not to word boundaries") {
    // A non-zero-based container ([9000:5000]) and a descending one ([0:8191]):
    // the runtime clamps to the declared range's translated bounds, so a select
    // below the base is out of range even though the storage word exists.
    const std::string out = run_capture(R"(
        module m;
            reg [9000:5000] c;
            reg [0:8191]    d;
            integer         i;
            initial begin
                c = 0;
                i = 6000;
                c[i +: 16] = 16'hf00d;
                $display("%h", c[6015:6000]);
                i = 6000;
                $display("%h", c[i +: 16]);
                i = 4990;
                $display("%h", c[i +: 16]);
                i = 100;
                $display("%h", c[i +: 16]);
                d = 0;
                i = 300;
                d[i +: 8] = 8'h3c;
                $display("%h", d[300:307]);
                i = 300;
                $display("%h", d[i +: 8]);
            end
        endmodule
    )");
    CHECK(out == "f00d\nf00d\n0Xxx\nxxxx\n3c\n3c\n");
}

TEST_CASE("nonblocking writes to wide-vector selects commit together") {
    // The NBA path stages into the shadow slot and records the same bits in the
    // signal's dirty mask -- itself one plane of the signal's width -- so two
    // dynamic sites in one block both land, overlapping repeats resolve per bit
    // (last write wins), and an out-of-range or x/z site stages nothing.
    const std::string out = run_capture(R"(
        module m;
            reg [8191:0] a;
            integer      i, j;
            initial begin
                a = 0;
                i = 100;
                j = 4000;
                a[i]       <= 1'b1;
                a[j +: 32] <= 32'hcafebabe;
                #1 $display("%b %h %b", a[100], a[4031:4000], a[101]);
                i = 200;
                a[i +: 16] <= 16'hffff;
                a[i +: 8]  <= 8'h00;
                #1 $display("%h", a[215:200]);
                i = 8180;
                a[i +: 32] <= 32'hffffffff;
                #1 $display("%h %h", a[8191:8180], a[8179:8176]);
                a = 0;
                i = 4'bx;
                a[i +: 8] <= 8'hff;
                #1 $display("%h", a[15:0]);
                i = 700;
                a[i +: 16] <= 16'hxz01;
                #1 $display("%h", a[715:700]);
            end
        endmodule
    )");
    CHECK(out == "1 cafebabe 0\nff00\nfff 0\n0000\nxz01\n");
}

TEST_CASE("a blocking and a nonblocking write to one wide vector keep their order") {
    // The blocking write is visible immediately, the nonblocking one only after
    // the region: the same rule as any signal, through the wide path's shadow.
    const std::string out = run_capture(R"(
        module m;
            reg [8191:0] b;
            integer      i;
            initial begin
                b = 0;
                i = 1000;
                b[i]     = 1'b1;
                b[i + 1] <= 1'b1;
                $display("%b %b", b[1000], b[1001]);
                #1 $display("%b %b", b[1000], b[1001]);
            end
        endmodule
    )");
    CHECK(out == "1 0\n1 1\n");
}

TEST_CASE("the wide and inline select paths agree across the threshold") {
    // The crossover is a codegen choice: a 4096-bit vector shifts inline, a
    // 4104-bit one goes through the runtime, and the same program text on each
    // must print the same thing (in range, out of range, and x-indexed).
    constexpr std::string_view src        = R"(
        module m;
            reg [WIDTH-1:0] a;
            integer         i;
            initial begin
                a = 0;
                i = 100;
                a[i +: 32] = 32'hdeadbeef;
                i = 100;
                $display("%h", a[i +: 32]);
                i = WIDTH - 4;
                $display("%h", a[i +: 8]);
                i = WIDTH + 9;
                $display("%h", a[i +: 8]);
                i = 4'bx;
                $display("%h", a[i +: 8]);
                i = WIDTH - 8;
                a[i +: 16] = 16'hffff;
                $display("%h", a[WIDTH-1:WIDTH-8]);
            end
        endmodule
    )";
    const auto                 with_width = [&](std::string_view w) {
        std::string s(src);
        for (size_t p = s.find("WIDTH"); p != std::string::npos; p = s.find("WIDTH", p)) {
            s.replace(p, 5, w);
            p += w.size();
        }
        return run_capture(s);
    };
    const std::string inl  = with_width("4096"); // inline variable shift
    const std::string wide = with_width("4104"); // runtime word-indexed path
    CHECK(inl == "deadbeef\nx0\nxx\nxx\nff\n");
    CHECK(wide == inl);
}
