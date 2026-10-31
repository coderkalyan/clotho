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

// The IR as the backend emits it, before the optimizer has folded or removed any of
// it: for a test of what the backend does, not of what is left.
std::string emit_raw_ir(std::string_view source) {
    const LowerResult        r = lower_ok(source);
    std::string              text;
    llvm::raw_string_ostream os(text);
    REQUIRE(ir::emit_llvm(*r.module, os, { .lower_coro = false }));
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

// Whether storage bit `i` of a `width`-bit vector is one `c` can address: inside the
// vector and inside the container (LRM 11.5.1). 128-bit arithmetic, so that no
// extreme base or offset overflows in the statement the kernels are checked against.
bool addressable(const KernelCase& c, uint32_t width, __int128 i) {
    return i >= 0 && i < width && i >= c.base && i < static_cast<__int128>(c.base) + c.cwidth;
}

// Both kernels on one case of a `width`-bit slot, checked bit by bit against
// LRM 11.5.1: a selection bit that is addressable is the storage bit, every other one
// reads x (2-state: 0) and is not written, and an x/z index addresses nothing. The
// slot has the planes `four` / `has_z` say (a null pointer for a plane it lacks), so
// the kernel must not touch a plane it was not given.
void check_kernels(uint32_t width, const KernelCase& c, bool four, bool has_z, bool unknown) {
    CAPTURE(width);
    CAPTURE(c.base);
    CAPTURE(c.cwidth);
    CAPTURE(c.off);
    CAPTURE(c.n);
    CAPTURE(four);
    CAPTURE(has_z);
    CAPTURE(unknown);

    const auto in_range = [&](uint32_t k) {
        return !unknown && addressable(c, width, static_cast<__int128>(c.off) + k);
    };
    const auto storage_bit = [&](uint32_t k) { return static_cast<uint64_t>(static_cast<__int128>(c.off) + k); };

    // Read. The buffers start as all ones, so a word the kernel forgets to write shows.
    Guarded               mm(four ? 2 * width : width, 1);
    Guarded               z(width, 2);
    const Guarded         mm_before = mm;
    const Guarded         z_before  = z;
    const uint32_t        words     = (c.n + 63) / 64;
    std::vector<uint64_t> got_must(words, ~uint64_t{ 0 }), got_may(words, ~uint64_t{ 0 }), got_z(words, ~uint64_t{ 0 });
    rt_wide_sel(mm.data(),
                has_z ? z.data() : nullptr,
                width,
                c.base,
                c.cwidth,
                c.off,
                c.n,
                unknown,
                got_must.data(),
                four ? got_may.data() : nullptr,
                has_z ? got_z.data() : nullptr);
    std::vector<uint64_t> want_must(words, 0), want_may(words, 0), want_z(words, 0);
    for (uint32_t k = 0; k < c.n; ++k) {
        const auto put = [&](std::vector<uint64_t>& p, bool v) {
            if (v) {
                p[k >> 6] |= uint64_t{ 1 } << (k & 63);
            }
        };
        if (in_range(k)) {
            put(want_must, mm.bit(storage_bit(k)));
            put(want_may, four && mm.bit(width + storage_bit(k)));
            put(want_z, has_z && z.bit(storage_bit(k)));
        } else {
            put(want_may, four); // x for a 4-state slot, 0 for 2-state
        }
    }
    CHECK(got_must == want_must);
    if (four) {
        CHECK(got_may == want_may);
    }
    if (has_z) {
        CHECK(got_z == want_z);
    }
    CHECK(mm.words == mm_before.words); // a read changes nothing
    CHECK(z.words == z_before.words);

    // Write.
    const std::vector<uint64_t> in_must = sel_plane(c.n, 3);
    const std::vector<uint64_t> in_may  = sel_plane(c.n, 4);
    const std::vector<uint64_t> in_z    = sel_plane(c.n, 5);
    Guarded                     want_mm = mm;
    Guarded                     want_zz = z;
    for (uint32_t k = 0; k < c.n; ++k) {
        if (!in_range(k)) {
            continue;
        }
        want_mm.set(storage_bit(k), plane_bit(in_must, k));
        if (four) {
            want_mm.set(width + storage_bit(k), plane_bit(in_may, k));
        }
        if (has_z) {
            want_zz.set(storage_bit(k), plane_bit(in_z, k));
        }
    }
    rt_wide_ins(mm.data(),
                has_z ? z.data() : nullptr,
                width,
                c.base,
                c.cwidth,
                c.off,
                c.n,
                unknown,
                in_must.data(),
                four ? in_may.data() : nullptr,
                has_z ? in_z.data() : nullptr);
    CHECK(mm.words == want_mm.words); // guards and padding included
    CHECK(z.words == want_zz.words);
}

// Every plane combination a slot can have: 2-state, 4-state without z, 4-state.
template<class Check>
void for_each_slot_shape(Check&& check) {
    check(false, false);
    check(true, false);
    check(true, true);
}

} // namespace

TEST_CASE("the wide select kernels touch only bits inside the vector, whatever container and offset") {
    // The contract under the SV tests above: a nested select whose outer select
    // is out of range hands the kernels a container that lies outside the vector,
    // and an offset is any i64. Neither may make them read or write a bit outside
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
        { INT32_MIN, UINT32_MAX, INT32_MIN, 130 }, // the most extreme i32 container and offset
        { INT32_MIN, UINT32_MAX, INT32_MAX - 20, 70 },
        { 0, 200, 0, 200 }, // the whole vector
        // Any i64 base and offset: nothing in the arithmetic may overflow on the way to
        // "addresses nothing", or to the few bits that are addressable.
        { INT64_MIN, UINT32_MAX, INT64_MIN, 130 },
        { INT64_MIN, UINT32_MAX, INT64_MAX, 70 },
        { INT64_MAX, UINT32_MAX, INT64_MAX - 20, 70 },
        { INT64_MAX, 100, 0, 100 },
        { 0, 200, INT64_MAX, 64 },
        { 0, 200, INT64_MIN, 64 },
        { INT64_MIN, UINT32_MAX, -1, 3 },
    };

    for (const KernelCase& c : cases) {
        for (const bool unknown : { false, true }) {
            for_each_slot_shape([&](bool four, bool has_z) { check_kernels(kWidth, c, four, has_z, unknown); });
        }
    }
}

TEST_CASE("the wide select kernels agree with the per-bit statement at every word boundary") {
    // A selection of each width that lies on or next to a word boundary, at every
    // offset from wholly below the vector to wholly above it, for slots whose may
    // plane starts mid-word (70, 200) and on a word boundary (256).
    for (const uint32_t width : { 70u, 200u, 256u }) {
        for (const uint32_t n : { 1u, 2u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 128u, 129u, 200u }) {
            for (int64_t off = -int64_t{ n } - 1; off <= int64_t{ width } + 1; ++off) {
                for_each_slot_shape(
                  [&](bool four, bool has_z) { check_kernels(width, { 0, width, off, n }, four, has_z, false); });
            }
        }
    }
}

TEST_CASE("the wide select kernels clamp to a container inside the vector at every word boundary") {
    // A nested select reads a window of the vector: the container is that window,
    // whose edges fall on, before and after the word boundaries.
    constexpr uint32_t kWidth = 300;
    for (const int64_t base : { -70, -1, 0, 1, 63, 64, 65, 127, 128, 129, 250 }) {
        for (const uint32_t cwidth : { 1u, 63u, 64u, 65u, 128u, 130u }) {
            for (const int64_t rel : { -65, -1, 0, 1, 63, 64, 65 }) {
                for (const uint32_t n : { 1u, 64u, 65u, 130u }) {
                    for_each_slot_shape([&](bool four, bool has_z) {
                        check_kernels(kWidth, { base, cwidth, base + rel, n }, four, has_z, false);
                    });
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

TEST_CASE("a wide selection reads and writes runs that cross word boundaries, in every kind of slot") {
    // The helpers move a selection a word at a time, so the runs that matter are the
    // ones that start before a word boundary and end after it, and the planes that
    // start mid-word: the may plane of a 300-bit 4-state slot starts at bit 300, in
    // word 4 at bit 44. `a` has a z plane, `b` is 2-state, and `c` is 4-state but
    // never z (its may plane starts on a word boundary, at bit 256). Checked against
    // iverilog 13, which agrees on every line but the last (it gives a 2-state vector
    // x for an out-of-range bit where LRM 11.5.1 says 0).
    const std::string out = run_capture(R"(
        module m;
            logic [299:0] a;
            bit   [299:0] b;
            logic [255:0] c;
            integer       i;
            initial begin
                a = 0; b = 0; c = 0;
                i = 0;   a[i +: 64] = 64'hdeadbeefcafef00d; b[i +: 64] = 64'hdeadbeefcafef00d; c[i +: 64] = 64'hdeadbeefcafef00d;
                i = 1;   $display("%h %h %h", a[i +: 64], b[i +: 64], c[i +: 64]);
                i = 60;  a[i +: 64] = 64'h0123456789abcdef; b[i +: 64] = 64'h0123456789abcdef; c[i +: 64] = 64'h0123456789abcdef;
                $display("%h %h %h", a[123:60], b[123:60], c[123:60]);
                i = 63;  $display("%h %h %h", a[i +: 3], b[i +: 3], c[i +: 3]);
                i = 64;  $display("%h %h %h", a[i +: 64], b[i +: 64], c[i +: 64]);
                i = 100; a[i +: 130] = {2'b10, 64'hxxxxxxxx_zzzzzzzz, 64'hffffffff_00000000};
                i = 100; $display("%h", a[i +: 130]);
                i = 190; $display("%h", a[i +: 64]);
                i = 128; c[i +: 128] = {64'hfedcba9876543210, 64'h0f0f0f0f0f0f0f0f};
                $display("%h", c[255:128]);
                i = 236; a[i +: 64] = 64'hffffffffffffffff;
                $display("%h", a[299:236]);
                i = 237; a[i +: 64] = 64'h0;
                $display("%h", a[299:236]);
                i = 290; $display("%h %h", a[i +: 16], b[i +: 16]);
            end
        endmodule
    )");
    CHECK(out == "6f56df77e57f7806 6f56df77e57f7806 6f56df77e57f7806\n"
                 "0123456789abcdef 0123456789abcdef 0123456789abcdef\n"
                 "5 5 5\n"
                 "00123456789abcde 00123456789abcde 00123456789abcde\n"
                 "2xxxxxxxxzzzzzzzzffffffff00000000\n"
                 "000000XxxxxxxxXz\n"
                 "fedcba98765432100f0f0f0f0f0f0f0f\n"
                 "ffffffffffffffff\n"
                 "0000000000000001\n"
                 "xX00 0000\n");
}

TEST_CASE("nonblocking writes to a wide vector cross word boundaries and hang off either end") {
    // The same slots, staged: a 70-bit run across a word boundary, one that hangs
    // below the vector, one that hangs above it, a wholly outside one and one with an
    // x index, whose staged bits (and dirty-mask bits) are only the in-range ones.
    // Checked against iverilog 13 line for line.
    const std::string out = run_capture(R"(
        module m;
            logic [299:0] a;
            bit   [299:0] b;
            logic [255:0] c;
            integer       i;
            reg [3:0]     xi;
            initial begin
                a = 0; b = 0; c = 0;
                i = 60;
                a[i +: 70] <= {6'b101xz1, 64'hf0f0f0f0f0f0f0f0};
                b[i +: 70] <= {6'b101111, 64'hf0f0f0f0f0f0f0f0};
                c[i +: 70] <= {6'b101011, 64'hf0f0f0f0f0f0f0f0};
                #1 $display("%h %h %h", a[129:60], b[129:60], c[129:60]);
                i = -3;
                a[i +: 70] <= {6'b110011, 64'h123456789abcdef0};
                b[i +: 70] <= {6'b110011, 64'h123456789abcdef0};
                c[i +: 70] <= {6'b110011, 64'h123456789abcdef0};
                #1 $display("%h %h %h", a[66:0], b[66:0], c[66:0]);
                i = 250;
                a[i +: 70] <= {6'b110011, 64'h123456789abcdef0};
                b[i +: 70] <= {6'b110011, 64'h123456789abcdef0};
                c[i +: 70] <= {6'b110011, 64'h123456789abcdef0};
                #1 $display("%h %h %h", a[299:250], b[299:250], c[255:250]);
                i = 400;
                a[i +: 70] <= 70'h3fffffffffffffffff;
                b[i +: 70] <= 70'h3fffffffffffffffff;
                c[i +: 70] <= 70'h3fffffffffffffffff;
                xi = 4'bx1x0;
                a[xi +: 70] <= 70'h3fffffffffffffffff;
                b[xi +: 70] <= 70'h3fffffffffffffffff;
                c[xi +: 70] <= 70'h3fffffffffffffffff;
                #1 $display("%b %b %b", a[299:296], b[299:296], c[255:252]);
                $display("%h|%h", a[299:200], c[255:192]);
            end
        endmodule
    )");
    CHECK(out == "2Xf0f0f0f0f0f0f0f0 2ff0f0f0f0f0f0f0f0 2bf0f0f0f0f0f0f0f0\n"
                 "662468acf13579bde 662468acf13579bde 662468acf13579bde\n"
                 "056789abcdef0 056789abcdef0 30\n"
                 "0001 0001 1100\n"
                 "159e26af37bc0000000000000|c000000000000000\n");
}

namespace {

// How many times `needle` occurs in `hay`.
size_t count_of(std::string_view hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string_view::npos; at = hay.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

// The one line of `ir` that calls `callee`, which the test's design makes exactly one of.
std::string call_line(const std::string& ir, std::string_view callee) {
    const std::string key = "call void @" + std::string(callee) + "(";
    REQUIRE(count_of(ir, key) == 1);
    const size_t at = ir.find(key);
    return ir.substr(at, ir.find('\n', at) - at);
}

} // namespace

TEST_CASE("a wide select gives the runtime only the planes its slot has") {
    // kernel.md: the helper receives only the planes the destination needs. A null
    // pointer stands for each plane the slot lacks, and no buffer is made for it: a
    // 2-state slot is selected through one buffer, a 4-state one without a z field
    // through two, and a 4-state one with z through three. Raw (unoptimized) IR, so
    // that what is asserted is what the backend emits, not what the optimizer left.
    const auto design = [](std::string_view type, std::string_view value) {
        return "module m;\n  " + std::string(type) + " [511:0] v;\n  int i, acc;\n" +
               "  initial begin\n    i = $urandom;\n    v = 0;\n    v[i] = " + std::string(value) + ";\n" +
               "    acc = v[i];\n    $display(\"%0d\", acc);\n  end\nendmodule\n";
    };
    const std::string two  = emit_raw_ir(design("bit", "1'b1"));
    const std::string four = emit_raw_ir(design("logic", "1'b1")); // no z is ever written
    const std::string zed  = emit_raw_ir(design("logic", "1'bz"));

    CHECK(four.find("%logic.nz.512") != std::string::npos); // the design is what the case says
    CHECK(zed.find("%logic.512") != std::string::npos);

    // The two helpers' calls: the slot's z pointer, then the three plane buffers.
    const auto nulls = [](const std::string& ir, std::string_view callee) {
        return count_of(call_line(ir, callee), "ptr null");
    };
    CHECK(nulls(two, "rt_wide_sel") == 3);
    CHECK(nulls(two, "rt_wide_ins") == 3);
    CHECK(nulls(four, "rt_wide_sel") == 2);
    CHECK(nulls(four, "rt_wide_ins") == 2);
    CHECK(nulls(zed, "rt_wide_sel") == 0);
    CHECK(nulls(zed, "rt_wide_ins") == 0);

    // Nor is a buffer made for a plane that is not passed.
    for (const std::string_view name : { "wsel.may", "wsel.z", "wins.may", "wins.z" }) {
        CHECK_MESSAGE(two.find(name) == std::string::npos, name);
    }
    for (const std::string_view name : { "wsel.z", "wins.z" }) {
        CHECK_MESSAGE(four.find(name) == std::string::npos, name);
    }
    for (const std::string_view name : { "wsel.must", "wsel.may", "wsel.z", "wins.must", "wins.may", "wins.z" }) {
        CHECK_MESSAGE(zed.find(name) != std::string::npos, name);
    }
}

TEST_CASE("a nonblocking wide select stages its dirty mask through one plane") {
    // The shadow takes the slot's planes, and the dirty mask is one 2-state plane of
    // the signal's width, so its call carries a single buffer whatever the slot is.
    const std::string ir = emit_raw_ir(R"(
        module m;
          logic [511:0] v;
          int i;
          initial begin
            i = $urandom;
            v = 0;
            v[i] <= 1'bz;
          end
        endmodule
    )");
    REQUIRE(count_of(ir, "call void @rt_wide_ins(") == 2);
    size_t with_z = 0, mask_only = 0;
    for (size_t at = ir.find("call void @rt_wide_ins("); at != std::string::npos;
         at        = ir.find("call void @rt_wide_ins(", at + 1)) {
        const std::string line  = ir.substr(at, ir.find('\n', at) - at);
        const size_t      nulls = count_of(line, "ptr null");
        with_z += nulls == 0;
        mask_only += nulls == 3;
    }
    CHECK(with_z == 1);    // the shadow: must, may and z
    CHECK(mask_only == 1); // the dirty mask: must only
}

TEST_CASE("a constant select of a wide vector reads and writes the words that hold it") {
    // kernel.md: constant selects touch only the 64-bit words they cover, plane by
    // plane: bits on both sides of every word boundary, a run across several, x and
    // z payloads, a window hanging over either end of the vector, in a 4-state slot,
    // a 2-state one and a z-free one (its may plane starts on a word boundary), by
    // blocking and nonblocking writes, and in a signal that also takes a run-time
    // index (so its dirty mask is a plane). Checked against iverilog 13 line for line.
    CHECK(run_capture(R"(
module t;
    logic [299:0] a;
    bit   [299:0] b;
    logic [255:0] c;
    logic [99:0]  r;
    integer       i;
    initial begin
        a = 0; b = 0; c = 0;
        a[0] = 1'b1; a[63] = 1'b1; a[64] = 1'b1; a[127] = 1'b1; a[128] = 1'b1; a[299] = 1'bx;
        b[0] = 1'b1; b[63] = 1'b1; b[64] = 1'b1; b[127] = 1'b1; b[128] = 1'b1; b[299] = 1'b1;
        c[0] = 1'b1; c[63] = 1'b1; c[64] = 1'b1; c[127] = 1'b1; c[128] = 1'b1; c[255] = 1'b1;
        $display("%h\n%h\n%h", a, b, c);
        $display("%b%b%b%b%b%b %b%b", a[0], a[63], a[64], a[127], a[128], a[299], b[63], c[255]);
        a[130:60] = {7'b1xz0xz1, 64'hf0f0_f0f0_0f0f_0f0f};
        b[130:60] = {7'b1010101, 64'hf0f0_f0f0_0f0f_0f0f};
        c[130:60] = {7'b1100110, 64'hf0f0_f0f0_0f0f_0f0f};
        $display("%h\n%h\n%h", a[140:50], b[140:50], c[140:50]);
        r = a[299:200]; $display("%h", r);
        a[299:236] = 64'hdead_beef_cafe_f00d; b[299:236] = 64'hdead_beef_cafe_f00d;
        $display("%h %h", a[299:236], b[299:236]);
        a[63:0] <= 64'h0123_4567_89ab_cdef; b[191:128] <= 64'h1111_2222_3333_4444; c[255:192] <= 64'h5555_6666_7777_8888;
        #1 $display("%h %h %h", a[63:0], b[191:128], c[255:192]);
        i = 5;
        a[i] <= 1'b1; a[200:190] <= 11'h7ff; b[i] <= 1'b0; b[100:90] <= 11'h555;
        #1 $display("%h %h", a[200:190], a[7:0]);
        $display("%h %h", b[100:90], b[7:0]);
        a[303:296] = 8'hff; a[3:-4] = 8'h5a;
        $display("%h %h", a[303:296], a[7:0]);
        $display("%b %b", a[303:296], a[3:-4]);
        a[303:296] <= 8'h00; a[3:-4] <= 8'hff;
        #1 $display("%h %h", a[299:296], a[7:0]);
    end
endmodule
    )") == "X00000000000000000000000000000000000000000180000000000000018000000000000001\n"
           "800000000000000000000000000000000000000000180000000000000018000000000000001\n"
           "8000000000000000000000000000000180000000000000018000000000000001\n"
           "11111x 11\n"
           "001XZc3c3c3c03c3c3c3c00\n"
           "00157c3c3c3c03c3c3c3c00\n"
           "0019bc3c3c3c03c3c3c3c00\n"
           "X000000000000000000000000\n"
           "deadbeefcafef00d deadbeefcafef00d\n"
           "0123456789abcdef 1111222233334444 5555666677778888\n"
           "7ff ef\n"
           "555 01\n"
           "xf e5\n"
           "xxxx1111 0101xxxx\n"
           "0 ef\n");
}

TEST_CASE("a constant select of a wide vector addresses no word it does not cover") {
    // Raw IR: a one-word window of a 4-state 512-bit vector is an i64 load and store
    // on each plane, never the 1024-bit must|may word or the 512-bit z word.
    const std::string ir = emit_raw_ir(R"(
        module m;
          logic [511:0] v;
          bit   [511:0] b;
          logic [5:0]   r6;
          logic         r;
          initial begin
            v[70:65] = 6'h2a;
            b[70:65] = 6'h15;
            r6 = v[70:65];
            r6 = b[70:65];
            v[300] = 1'b1;
            r = v[300];
          end
        endmodule
    )");
    const size_t      at = ir.find("define ptr @clotho.inst.m.p");
    REQUIRE(at != std::string::npos);
    const std::string body = ir.substr(at, ir.find("\n}\n", at) - at);
    for (const std::string_view whole : { "load i1024", "store i1024", "load i512", "store i512" }) {
        CHECK_MESSAGE(body.find(whole) == std::string::npos, whole);
    }
    CHECK(body.find("load i64") != std::string::npos);
}
