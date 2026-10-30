// Runtime-offset bit / part-selects of a vector too wide to shift inline
// (LRM 11.5.1 / 11.5.2). Above `wide_dyn_select_bits` (256) the backend stops
// asking LLVM for a variable shift on an i(2W) value -- which legalizes into
// O(W/64) selects per word, so JIT time grew as O(W^2): 21 s for a 12 Kbit vector
// and over half an hour for ivtest pr722's 65537-bit one -- and calls
// rt_wide_sel / rt_wide_ins, which index the containing 64-bit word instead.
//
// These cases pin the *semantics* of that path, since it reimplements in C++ what
// the inline form gets from the shift: the in-range read / write, LRM 11.5.1's
// per-bit out-of-range rule in both directions, an x/z index, 4-state payloads
// through a selection wider than one word, non-zero-based and descending
// containers, and the nonblocking form (which stages into the shadow and its dirty
// mask the same way). The crossover to the inline path is a pure codegen choice
// with no observable difference, so the narrow twin of each case is the same
// text.
//
// Timing is not asserted here (a machine-speed assertion in a unit test is a
// flake); tests/perf carries the JIT-time guard.
//
// If the nonblocking case fails in an **ASan build** with an empty capture and a
// `Delta32 fixup ... out of range` JIT session error on stderr, that is the
// pre-existing code/data distance hazard, not this path: a multi-kilobit signal
// with a nonblocking write hits it at any width (4095 bits included, on main too),
// about one run in five, because ASan's shadow reservations fragment the address
// space.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"
#include "ir/LlvmBackend.h"
#include "runtime/Runtime.h"

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

std::string emit_ir(std::string_view source) {
    const LowerResult r = lower_ok(source);
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*r.module, os));
    return text;
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
    // The crossover is a codegen choice: a 256-bit vector shifts inline, a
    // 264-bit one goes through the runtime, and the same program text on each
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
    const std::string inl  = with_width("256"); // inline variable shift
    const std::string wide = with_width("264"); // runtime word-indexed path
    CHECK(inl == "deadbeef\nx0\nxx\nxx\nff\n");
    CHECK(wide == inl);
}

TEST_CASE("a nested select of a very wide vector with an invalid outer index reads x and writes nothing") {
    // LRM 11.5.1: every level of a nested select is an address that can be
    // invalid, and an invalid one makes the whole access read x (0 for 2-state)
    // and write nothing. The outer index here is one past the end, negative
    // (which used to address storage before the vector and crash), or x; the
    // inner one is in range. Oracle: the flattened `w[i*128 + j]`, which
    // iverilog accepts, reads x and writes nothing for all three.
    const std::string out = run_capture(R"(
        module m;
            logic [63:0][127:0] w;
            bit   [63:0][127:0] bw;
            logic [6:0]         wx;
            int                 i, j;
            initial begin
                w = '0; bw = '0; j = 5;
                i = 64;     $display("%b %b %b %b", w[i][j], w[i][j +: 2], bw[i][j], bw[i][j +: 2]);
                i = -1;     $display("%b %b %b %b", w[i][j], w[i][j +: 2], bw[i][j], bw[i][j +: 2]);
                wx = 7'bx;  $display("%b %b %b %b", w[wx][j], w[wx][j +: 2], bw[wx][j], bw[wx][j +: 2]);
                i = 64; w[i][j] = 1'b1; w[i][j +: 2] = 2'b11; bw[i][j] = 1'b1; bw[i][j +: 2] = 2'b11;
                i = -1; w[i][j] = 1'b1; w[i][j +: 2] = 2'b11; bw[i][j] = 1'b1; bw[i][j +: 2] = 2'b11;
                wx = 7'bx; w[wx][j] = 1'b1; w[wx][j +: 2] = 2'b11; bw[wx][j] = 1'b1; bw[wx][j +: 2] = 2'b11;
                $display("%b %b", w === '0, bw === '0);
                i = 64; w[i][j] <= 1'b1; bw[i][j +: 2] <= 2'b11;
                i = -1; w[i][j] <= 1'b1; bw[i][j +: 2] <= 2'b11;
                wx = 7'bx; w[wx][j] <= 1'b1; bw[wx][j +: 2] <= 2'b11;
                #1 $display("%b %b", w === '0, bw === '0);
                // Valid outer indices, including the last element, still land.
                i = 63; j = 127; w[i][j] = 1'b1;
                i = 3;  j = 5;   w[i][j] = 1'b1;
                $display("%b %b %b", w[63][127], w[3][5], w[0][0]);
            end
        endmodule
    )");
    CHECK(out ==
          "x xx 0 00\n"
          "x xx 0 00\n"
          "x xx 0 00\n"
          "1 1\n"
          "1 1\n"
          "1 1 0\n");
}

TEST_CASE("a constant part-select hanging over a very wide vector reads x outside and writes only inside") {
    // LRM 11.5.1: the bits of a constant part-select that are out of range read x
    // (0 for 2-state) and are not written, whichever end of the vector they hang
    // over, on a vector too wide to shift inline as on a narrow one.
    const std::string out = run_capture(R"(
        module m;
            reg [8191:0] a;
            bit [8191:0] b;
            initial begin
                a = '1; b = '1;
                $display("%b %b", a[8195:8190], b[8195:8190]);
                $display("%b %b", a[3:-2], b[3:-2]);
                a = 0; a[8195:8190] = 6'b101101;
                $display("%b", a[8191:8188]);
                a = 0; a[3:-2] = 6'b110011;
                $display("%h", a[3:0]);
                a = 0; a[8200:8195] = 6'h3f;
                $display("%b %b", a === 8192'b0, a[8200:8195]);
                a = 0; a[8195:8190] <= 6'b101101;
                #1 $display("%b", a[8191:8188]);
                a = 0; a[8195:8190] <= #1 6'b101101;
                #2 $display("%b", a[8191:8188]);
            end
        endmodule
    )");
    CHECK(out ==
          "xxxx11 000011\n"
          "1111xx 111100\n"
          "0100\n"
          "c\n"
          "1 xxxxxx\n"
          "0100\n"
          "0100\n");
}

namespace {

// One plane of storage with guard words before and after it, so a kernel that
// strays outside its vector disturbs something the test can see.
struct Guarded {
    static constexpr size_t   kGuard     = 2;
    static constexpr uint64_t kGuardWord = ~uint64_t{ 0 };

    std::vector<uint64_t> words;

    Guarded(uint32_t bits, uint64_t seed) : words(kGuard + (bits + 63) / 64 + kGuard, kGuardWord) {
        // The vector's own words, padding bits included, get a fixed pattern.
        for (size_t k = kGuard; k < words.size() - kGuard; ++k) {
            seed     = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            words[k] = seed;
        }
    }

    uint64_t* data() { return words.data() + kGuard; }

    bool bit(uint64_t i) const { return ((words[kGuard + (i >> 6)] >> (i & 63)) & 1) != 0; }

    void set(uint64_t i, bool v) {
        const uint64_t m = uint64_t{ 1 } << (i & 63);
        v ? words[kGuard + (i >> 6)] |= m : words[kGuard + (i >> 6)] &= ~m;
    }
};

// A selection plane of `n` bits as a word buffer.
std::vector<uint64_t> sel_plane(uint32_t n, uint64_t seed) {
    std::vector<uint64_t> p((n + 63) / 64, 0);
    for (uint64_t& w : p) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        w    = seed;
    }
    return p;
}

bool plane_bit(const std::vector<uint64_t>& p, uint32_t k) {
    return ((p[k >> 6] >> (k & 63)) & 1) != 0;
}

struct KernelCase {
    int64_t  base;
    uint32_t cwidth;
    int64_t  off;
    uint32_t n;
};

} // namespace

TEST_CASE("the wide select kernels touch only bits inside the vector, whatever container and offset") {
    // The contract under the SV tests above: a nested select whose outer select
    // is out of range hands the kernels a container that lies outside the vector,
    // and an offset is any i32. Neither may make them read or write a bit outside
    // [0, width), nor outside the container; every other bit reads x (2-state: 0)
    // and is not written. LRM 11.5.1, checked bit by bit against that statement.
    constexpr uint32_t kWidth = 200; // the last word of a plane is partly padding
    const KernelCase   cases[] = {
        { 0, 200, 10, 70 },                        // inside
        { 0, 200, 180, 40 },                       // straddles the top
        { 0, 200, -20, 40 },                       // straddles the bottom
        { -64, 600, -40, 150 },                    // container wider than the vector at both ends
        { 200, 128, 200, 128 },                    // the element just past the end
        { -128, 128, -128, 128 },                  // the element just before the start
        { -1000, 100, -1000, 64 },                 // far below
        { 5000, 300, 5100, 90 },                   // far above
        { INT32_MIN, UINT32_MAX, INT32_MIN, 130 }, // the most extreme container and offset
        { INT32_MIN, UINT32_MAX, INT32_MAX - 20, 70 },
        { 0, 200, 0, 200 },                        // the whole vector
    };

    for (const KernelCase& c : cases) {
        for (uint32_t four = 0; four <= 1; ++four) {
            for (uint32_t has_z = 0; has_z <= four; ++has_z) {
                for (uint32_t unknown = 0; unknown <= 1; ++unknown) {
                    CAPTURE(c.base);
                    CAPTURE(c.cwidth);
                    CAPTURE(c.off);
                    CAPTURE(c.n);
                    CAPTURE(four);
                    CAPTURE(has_z);
                    CAPTURE(unknown);

                    const auto in_range = [&](int64_t i) {
                        return unknown == 0 && i >= 0 && i < static_cast<int64_t>(kWidth) &&
                               i >= c.base && i < c.base + static_cast<int64_t>(c.cwidth);
                    };

                    // Read.
                    Guarded mm(four != 0 ? 2 * kWidth : kWidth, 1);
                    Guarded z(kWidth, 2);
                    const Guarded mm_before = mm;
                    const Guarded z_before  = z;
                    const uint32_t words    = (c.n + 63) / 64;
                    std::vector<uint64_t> got_must(words, ~uint64_t{ 0 }), got_may(words, ~uint64_t{ 0 }),
                      got_z(words, ~uint64_t{ 0 });
                    rt_wide_sel(mm.data(), has_z != 0 ? z.data() : nullptr, kWidth, c.base, c.cwidth,
                                c.off, c.n, four, has_z, unknown, got_must.data(), got_may.data(),
                                got_z.data());
                    std::vector<uint64_t> want_must(words, 0), want_may(words, 0), want_z(words, 0);
                    for (uint32_t k = 0; k < c.n; ++k) {
                        const int64_t i = c.off + static_cast<int64_t>(k);
                        const auto    put = [&](std::vector<uint64_t>& p, bool v) {
                            if (v) {
                                p[k >> 6] |= uint64_t{ 1 } << (k & 63);
                            }
                        };
                        if (in_range(i)) {
                            put(want_must, mm.bit(static_cast<uint64_t>(i)));
                            put(want_may, four != 0 && mm.bit(kWidth + static_cast<uint64_t>(i)));
                            put(want_z, four != 0 && has_z != 0 && z.bit(static_cast<uint64_t>(i)));
                        } else {
                            put(want_may, four != 0); // x for a 4-state slot, 0 for 2-state
                        }
                    }
                    CHECK(got_must == want_must);
                    CHECK(got_may == want_may);
                    CHECK(got_z == want_z);
                    CHECK(mm.words == mm_before.words); // a read changes nothing
                    CHECK(z.words == z_before.words);

                    // Write.
                    const std::vector<uint64_t> in_must = sel_plane(c.n, 3);
                    const std::vector<uint64_t> in_may  = sel_plane(c.n, 4);
                    const std::vector<uint64_t> in_z    = sel_plane(c.n, 5);
                    Guarded                     want_mm = mm;
                    Guarded                     want_zz = z;
                    for (uint32_t k = 0; k < c.n; ++k) {
                        const int64_t i = c.off + static_cast<int64_t>(k);
                        if (!in_range(i)) {
                            continue;
                        }
                        const auto u = static_cast<uint64_t>(i);
                        want_mm.set(u, plane_bit(in_must, k));
                        if (four != 0) {
                            want_mm.set(kWidth + u, plane_bit(in_may, k));
                            if (has_z != 0) {
                                want_zz.set(u, plane_bit(in_z, k));
                            }
                        }
                    }
                    rt_wide_ins(mm.data(), has_z != 0 ? z.data() : nullptr, kWidth, c.base, c.cwidth,
                                c.off, c.n, four, has_z, unknown, in_must.data(), in_may.data(),
                                in_z.data());
                    CHECK(mm.words == want_mm.words); // guards and padding included
                    CHECK(z.words == want_zz.words);
                }
            }
        }
    }
}

// A watched vector wakes its waiters from the transition of what a wide-select store
// actually wrote. The store's change detection looks only at the 64-bit words the
// store can touch (plus the two words that hold bit 0 of the planes, which is all an
// edge reads, LRM 9.4.2), not at the whole 64 Kbit vector, so what must hold is that
// every kind of store still wakes the right waiters: an Any-edge waiter on any value
// change, including a z-only one (x -> z is a change although must and may agree)
// and one in a word nowhere near bit 0; posedge / negedge only when bit 0 moves up or
// down the 0 < x/z < 1 lattice of Table 9-2, a store high in the vector never; and
// none of them for a write that changes nothing, lands out of range, is dropped for an
// x/z index, or hangs off an end of the vector and changes only the bits inside it.
// The 2-state twin `b` has no x, z or may plane. The expected counts are iverilog
// 13's (the same design, line for line).
TEST_CASE("a watched wide vector wakes the right waiters for every kind of select store") {
    const std::string out = run_capture(R"(
        module m;
          logic [65535:0] v;
          bit   [65535:0] b;
          integer anyv, posv, negv, anyb, posb, negb;
          integer i;
          reg [31:0] ix;
          always @(v)         anyv = anyv + 1;
          always @(posedge v) posv = posv + 1;
          always @(negedge v) negv = negv + 1;
          always @(b)         anyb = anyb + 1;
          always @(posedge b) posb = posb + 1;
          always @(negedge b) negb = negb + 1;
          task report(input [8*14:1] tag);
            $display("%0s v: any=%0d pos=%0d neg=%0d  b: any=%0d pos=%0d neg=%0d",
                     tag, anyv, posv, negv, anyb, posb, negb);
          endtask
          initial begin
            v = {65536{1'b0}}; b = 0; #1;
            anyv = 0; posv = 0; negv = 0; anyb = 0; posb = 0; negb = 0;
            i = 0;     v[i] = 1; b[i] = 1; #1 report("lsb 0->1");
            i = 0;     v[i] = 1; b[i] = 1; #1 report("lsb same");
            i = 40000; v[i] = 1; b[i] = 1; #1 report("high bit set");
            i = 64;    v[i] = 1; b[i] = 1; #1 report("word edge 64");
            i = 63;    v[i] = 1; b[i] = 1; #1 report("word edge 63");
            i = 40000; v[i] = 0; b[i] = 0; #1 report("high bit clr");
            i = 0;     v[i] = 0; b[i] = 0; #1 report("lsb 1->0");
            i = 0;     v[i] = 1'bx; #1 report("lsb 0->x");
            i = 0;     v[i] = 1'bz; #1 report("lsb x->z");
            i = 0;     v[i] = 1; #1 report("lsb z->1");
            i = 0;     v[i] = 1'bx; #1 report("lsb 1->x");
            i = 0;     v[i] = 0; #1 report("lsb x->0");
            i = 5;     v[i] = 1'bx; #1 report("bit5 0->x");
            i = 5;     v[i] = 1'bz; #1 report("bit5 x->z");
            i = 65000; v[i] = 1'bz; #1 report("bit65000 z");
            i = 70000; v[i] = 1; b[i] = 1; #1 report("oor high");
            i = -1;    v[i] = 1; b[i] = 1; #1 report("oor neg");
            ix = 32'bx; v[ix] = 1; b[ix] = 1; #1 report("x index");
            i = 65532; v[i +: 8] = 8'hff; b[i +: 8] = 8'hff; #1 report("part hi hang");
            i = -4;    v[i +: 8] = 8'hff; b[i +: 8] = 8'hff; #1 report("part lo hang");
            i = -4;    v[i +: 8] = 8'hff; b[i +: 8] = 8'hff; #1 report("part lo again");
            i = 100;   v[i +: 200] = {200{1'b1}}; b[i +: 200] = {200{1'b1}}; #1 report("wide part");
            i = 100;   v[i +: 200] = {200{1'b0}}; b[i +: 200] = {200{1'b0}}; #1 report("wide part clr");
            i = 0;     v[i +: 4] = 4'b0001; b[i +: 4] = 4'b0001; #1 report("lsb via part");
          end
        endmodule
    )");
    CHECK(out ==
          "lsb 0->1 v: any=1 pos=1 neg=0  b: any=1 pos=1 neg=0\n"
          "lsb same v: any=1 pos=1 neg=0  b: any=1 pos=1 neg=0\n"
          "high bit set v: any=2 pos=1 neg=0  b: any=2 pos=1 neg=0\n"
          "word edge 64 v: any=3 pos=1 neg=0  b: any=3 pos=1 neg=0\n"
          "word edge 63 v: any=4 pos=1 neg=0  b: any=4 pos=1 neg=0\n"
          "high bit clr v: any=5 pos=1 neg=0  b: any=5 pos=1 neg=0\n"
          "lsb 1->0 v: any=6 pos=1 neg=1  b: any=6 pos=1 neg=1\n"
          "lsb 0->x v: any=7 pos=2 neg=1  b: any=6 pos=1 neg=1\n"
          "lsb x->z v: any=8 pos=2 neg=1  b: any=6 pos=1 neg=1\n"
          "lsb z->1 v: any=9 pos=3 neg=1  b: any=6 pos=1 neg=1\n"
          "lsb 1->x v: any=10 pos=3 neg=2  b: any=6 pos=1 neg=1\n"
          "lsb x->0 v: any=11 pos=3 neg=3  b: any=6 pos=1 neg=1\n"
          "bit5 0->x v: any=12 pos=3 neg=3  b: any=6 pos=1 neg=1\n"
          "bit5 x->z v: any=13 pos=3 neg=3  b: any=6 pos=1 neg=1\n"
          "bit65000 z v: any=14 pos=3 neg=3  b: any=6 pos=1 neg=1\n"
          "oor high v: any=14 pos=3 neg=3  b: any=6 pos=1 neg=1\n"
          "oor neg v: any=14 pos=3 neg=3  b: any=6 pos=1 neg=1\n"
          "x index v: any=14 pos=3 neg=3  b: any=6 pos=1 neg=1\n"
          "part hi hang v: any=15 pos=3 neg=3  b: any=7 pos=1 neg=1\n"
          "part lo hang v: any=16 pos=4 neg=3  b: any=8 pos=2 neg=1\n"
          "part lo again v: any=16 pos=4 neg=3  b: any=8 pos=2 neg=1\n"
          "wide part v: any=17 pos=4 neg=3  b: any=9 pos=2 neg=1\n"
          "wide part clr v: any=18 pos=4 neg=3  b: any=10 pos=2 neg=1\n"
          "lsb via part v: any=19 pos=4 neg=3  b: any=11 pos=2 neg=1\n");
}

// The same for a select nested in an element of a wide packed array: its offset is
// the enclosing element's plus its own, and an index outside its element (or an
// element that does not exist) writes nothing, so it wakes nothing. No oracle: iverilog
// takes no variable index there; the counts are LRM 11.5.1 / 9.4.2 worked by hand
// and equal what the whole-vector change detection gave before.
TEST_CASE("a watched wide packed array wakes the right waiters for a nested select store") {
    const std::string out = run_capture(R"(
        module m;
          logic [3:0][8191:0] pk;
          integer anyp, posp, negp;
          integer i, j;
          always @(pk)         anyp = anyp + 1;
          always @(posedge pk) posp = posp + 1;
          always @(negedge pk) negp = negp + 1;
          task report(input [8*14:1] tag);
            $display("%0s any=%0d pos=%0d neg=%0d", tag, anyp, posp, negp);
          endtask
          initial begin
            pk = {4{8192'b0}}; #1;
            anyp = 0; posp = 0; negp = 0;
            i = 0; j = 0;    pk[i][j] = 1; #1 report("elem0 bit0");
            i = 1; j = 0;    pk[i][j] = 1; #1 report("elem1 bit0");
            i = 3; j = 8191; pk[i][j] = 1; #1 report("elem3 top");
            i = 1; j = 8192; pk[i][j] = 1; #1 report("j past elem");
            i = 1; j = -1;   pk[i][j] = 1; #1 report("j neg");
            i = 4; j = 0;    pk[i][j] = 1; #1 report("i past");
            i = -1; j = 0;   pk[i][j] = 1; #1 report("i neg");
            i = 0; j = 0;    pk[i][j] = 0; #1 report("elem0 bit0 clr");
            i = 2; j = 4000; pk[i][j +: 64] = 64'hf0f0f0f0f0f0f0f0; #1 report("elem2 part");
          end
        endmodule
    )");
    CHECK(out ==
          "elem0 bit0 any=1 pos=1 neg=0\n"
          "elem1 bit0 any=2 pos=1 neg=0\n"
          "elem3 top any=3 pos=1 neg=0\n"
          "j past elem any=3 pos=1 neg=0\n"
          "j neg any=3 pos=1 neg=0\n"
          "i past any=3 pos=1 neg=0\n"
          "i neg any=3 pos=1 neg=0\n"
          "elem0 bit0 clr any=4 pos=1 neg=1\n"
          "elem2 part any=5 pos=1 neg=1\n");
}

// The change detection around a wide-select store must not load and compare the
// whole vector: for a one-bit store to a watched 64 Kbit vector that was a load and
// a compare of the whole 131072-bit must|may word, before and after the store. The
// design has no whole-vector store, so any i131072 load or compare left in the
// module is the select store's bracket.
TEST_CASE("a watched wide select store does not load or compare the whole vector") {
    const std::string ir = emit_ir(R"(
        module m;
          logic [65535:0] v;
          integer i;
          always @(v) $display("any");
          always @(posedge v) $display("pos");
          initial begin
            i = $urandom;
            v[i] = 1'b1;
          end
        endmodule
    )");
    CHECK(ir.find("load i131072") == std::string::npos);
    CHECK(ir.find("icmp ne i131072") == std::string::npos);
}

TEST_CASE("a constant-offset select of wide storage stays inline and a runtime offset calls the helper") {
    // kernel.md: the helper serves runtime-indexed selects only. A constant offset has
    // no variable shift to legalize, so a 512-bit vector, past the threshold, is read and
    // written with plain constant-position code.
    const auto source = [](std::string_view index) {
        std::string s = R"(
            module m;
              logic [511:0] v;
              bit   [511:0] b;
              int           i, acc;
              initial begin
                i = $urandom;
                v = 0; b = 0;
                v[INDEX] = 1'b1; b[INDEX] = 1'b1;
                acc = v[INDEX] + b[INDEX];
                v[INDEX +: 8] = 8'hff;
                $display("%0d %h", acc, v[INDEX +: 8]);
              end
            endmodule
        )";
        for (size_t p = s.find("INDEX"); p != std::string::npos; p = s.find("INDEX", p)) {
            s.replace(p, 5, index);
            p += index.size();
        }
        return s;
    };
    const std::string constant = emit_ir(source("300"));
    const std::string runtime  = emit_ir(source("i"));
    CHECK(constant.find("@rt_wide_sel") == std::string::npos);
    CHECK(constant.find("@rt_wide_ins") == std::string::npos);
    CHECK(runtime.find("@rt_wide_sel") != std::string::npos);
    CHECK(runtime.find("@rt_wide_ins") != std::string::npos);
    CHECK(run_capture(source("300")) == "2 ff\n");
}
