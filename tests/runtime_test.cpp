#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "runtime/Runtime.h"
#include "runtime/svdpi.h"

namespace {

// Shared state threaded through rt_wheel_tick as the opaque context, so the
// non-capturing C callback can reach test bookkeeping and the wheel itself.
struct Context {
    rt_wheel*        wheel = nullptr;
    std::vector<int> order;
};

// An awaiter-like object embedding an intrusive rt_timer, mirroring how a design
// would own its timer node.
struct Awaiter {
    rt_timer timer{};
    int      id       = 0;
    uint64_t fired_at = 0;
};

// Recover the owning Awaiter from its embedded timer node.
Awaiter& owner(rt_timer* t) {
    return *reinterpret_cast<Awaiter*>(reinterpret_cast<char*>(t) - offsetof(Awaiter, timer));
}

// Log the fire order and the tick each timer fired at.
void record(rt_timer* t, void* ctx) {
    auto&    c = *static_cast<Context*>(ctx);
    Awaiter& a = owner(t);
    a.fired_at = rt_wheel_now(c.wheel);
    c.order.push_back(a.id);
}

} // namespace

TEST_CASE("the runtime wheel jumps over empty ticks and fires its timers in order") {
    rt_wheel w;
    rt_wheel_init(&w);
    Context c{ .wheel = &w };
    Awaiter a{ .id = 1 };
    Awaiter b{ .id = 2 };

    rt_wheel_schedule(&w, &a.timer, 5, &record);
    rt_wheel_schedule(&w, &b.timer, 2, &record);
    CHECK(rt_wheel_now(&w) == 0);

    rt_wheel_tick(&w, &c); // tick 1 is empty -> jump straight to 2
    CHECK(rt_wheel_now(&w) == 2);
    REQUIRE(c.order.size() == 1);
    CHECK(c.order[0] == 2);

    rt_wheel_tick(&w, &c); // jump to 5
    CHECK(rt_wheel_now(&w) == 5);
    REQUIRE(c.order.size() == 2);
    CHECK(c.order[1] == 1);
    CHECK(a.fired_at == 5);
    CHECK(b.fired_at == 2);

    rt_wheel_deinit(&w);
}

TEST_CASE("a runtime-wheel callback may re-arm its own timer across a deadline beyond one revolution") {
    rt_wheel w;
    rt_wheel_init(&w);
    Context c{ .wheel = &w };
    Awaiter a{ .id = 7 };

    // A deadline several revolutions out exercises the overflow heap and cascade.
    rt_wheel_schedule(&w, &a.timer, 4 * 256 + 3, &record);
    rt_wheel_tick(&w, &c);

    CHECK(rt_wheel_now(&w) == 4 * 256 + 3);
    REQUIRE(c.order.size() == 1);
    CHECK(c.order[0] == 7);
    CHECK(a.fired_at == 4 * 256 + 3);

    rt_wheel_deinit(&w);
}

TEST_CASE("rt_wheel_tick on an idle runtime wheel is a no-op") {
    rt_wheel w;
    rt_wheel_init(&w);
    Context c{ .wheel = &w };

    rt_wheel_tick(&w, &c);
    CHECK(rt_wheel_now(&w) == 0);
    CHECK(c.order.empty());

    rt_wheel_deinit(&w);
}

TEST_CASE("rt_readmem with a null z_base loads values without touching z memory") {
    // Two adjacent 8-bit elements, mm word (2 bytes) each, no z word: stride 2.
    // A z write through a null-z layout would clobber the next element's mm word.
    char path[] = "/tmp/clotho_rt_readmem_noz.hex";
    std::FILE* f = std::fopen(path, "w");
    REQUIRE(f != nullptr);
    std::fputs("a5 3c\n", f);
    std::fclose(f);

    // The filename crosses the ABI as a right-justified packed-ASCII vector
    // (byte 0 = last character).
    std::vector<char> packed;
    for (size_t k = std::strlen(path); k-- > 0;) {
        packed.push_back(path[k]);
    }

    uint8_t mm[4] = { 0xee, 0xee, 0xee, 0xee };
    rt_readmem(packed.data(), static_cast<uint32_t>(packed.size()), mm, nullptr, 2, 2, 8, 0, 1, 0, 0,
               /*flags: hex, 2-state*/ 1);

    // must = may = value for a 2-state load: mm word is val | (val << 8).
    CHECK(mm[0] == 0xa5);
    CHECK(mm[1] == 0xa5);
    CHECK(mm[2] == 0x3c);
    CHECK(mm[3] == 0x3c);
    std::remove(path);
}

TEST_CASE("rt_nba_arr slot buffer: distinct slots, counts, reset keeps capacity") {
    rt_nba_arr_reset();
    const uint32_t stride = 16;
    // Two arrays; reserve slots and confirm they are distinct, stride-spaced, and
    // that the writes round-trip through base + i*stride. Each slot is written
    // immediately after it is reserved: rt_nba_arr_slot may resize (and thus
    // reallocate) the buffer, invalidating pointers from earlier calls, so the
    // backend contract is write-then-advance — a later grow resize-copies the
    // already-written records forward (Runtime.cpp).
    void* a0 = rt_nba_arr_slot(0, stride);
    static_cast<uint64_t*>(a0)[0] = 0xAAAA;
    void* a1 = rt_nba_arr_slot(0, stride);
    static_cast<uint64_t*>(a1)[0] = 0xBBBB;
    void* b0 = rt_nba_arr_slot(1, stride);
    static_cast<uint64_t*>(b0)[0] = 0xCCCC;
    CHECK(rt_nba_arr_count(0) == 2);
    CHECK(rt_nba_arr_count(1) == 1);
    CHECK(rt_nba_arr_any() == 3);
    auto* base0 = static_cast<uint8_t*>(rt_nba_arr_base(0));
    CHECK(*reinterpret_cast<uint64_t*>(base0 + 0 * stride) == 0xAAAA);
    CHECK(*reinterpret_cast<uint64_t*>(base0 + 1 * stride) == 0xBBBB);

    rt_nba_arr_reset();
    CHECK(rt_nba_arr_count(0) == 0);
    CHECK(rt_nba_arr_any() == 0);
    // Capacity retained: a fresh slot reuses the same buffer address.
    CHECK(rt_nba_arr_slot(0, stride) == base0);
    rt_nba_arr_reset(); // leave no staged records for the next test
}

namespace {

// The bytes of an rt_string as a std::string, via the public getc API.
std::string str_bytes(const rt_string& s) {
    std::string out;
    const auto  n = rt_string_len(&s);
    for (uint64_t k = 0; k < n; ++k) {
        out += static_cast<char>(rt_string_getc(&s, static_cast<int32_t>(k)));
    }
    return out;
}

// A guard pairing init with deinit so tests can't leak on CHECK failures.
struct Str {
    rt_string s;
    Str() {
        rt_string_init(&s);
    }
    explicit Str(std::string_view v) {
        rt_string_init(&s);
        rt_string_assign_bytes(&s, v.data(), v.size());
    }
    ~Str() {
        rt_string_deinit(&s);
    }
    Str(const Str&)            = delete;
    Str& operator=(const Str&) = delete;
};

} // namespace

TEST_CASE("rt_string_data is NUL-terminated at byte[len] (C-string interop)") {
    // Empty, short inline, exactly the inline cap (30), one past (heap), and a long
    // heap string — rt_string_data must be a valid C string in every form.
    for (const size_t n : { size_t{ 0 }, size_t{ 1 }, size_t{ 15 }, size_t{ 30 }, size_t{ 31 }, size_t{ 200 } }) {
        std::string v;
        for (size_t k = 0; k < n; ++k) {
            v += static_cast<char>('a' + k % 26);
        }
        Str         s(v);
        const char* c = rt_string_data(&s.s);
        REQUIRE(rt_string_len(&s.s) == n);
        CHECK(c[n] == '\0');            // terminator at byte[len]
        CHECK(std::string(c) == v);     // C-string view equals the content
    }
}

TEST_CASE("rt_string keeps the length authoritative with an embedded NUL") {
    Str s(std::string_view("ab\0cd", 5)); // 5 bytes including a mid-string NUL
    CHECK(rt_string_len(&s.s) == 5);                 // full length preserved
    CHECK(rt_string_data(&s.s)[5] == '\0');          // terminator still at byte[len]
    CHECK(std::strlen(rt_string_data(&s.s)) == 2);   // C view stops at the embedded NUL
}

TEST_CASE("rt_string round-trips byte sequences across the inline/heap boundary") {
    Str s;
    CHECK(rt_string_len(&s.s) == 0);

    // Lengths straddling the 31-byte inline capacity, both growing and shrinking.
    for (const size_t n : { size_t{ 0 }, size_t{ 1 }, size_t{ 30 }, size_t{ 31 }, size_t{ 32 }, size_t{ 100 },
                            size_t{ 1000 }, size_t{ 31 }, size_t{ 0 }, size_t{ 500 } }) {
        std::string v;
        for (size_t k = 0; k < n; ++k) {
            v += static_cast<char>('a' + k % 26);
        }
        rt_string_assign_bytes(&s.s, v.data(), v.size());
        CHECK(rt_string_len(&s.s) == n);
        CHECK(str_bytes(s.s) == v);
    }
}

TEST_CASE("rt_string_assign deep-copies and tolerates self-assignment") {
    Str a{ "hello world, this string is long enough to live on the heap" };
    Str b;
    rt_string_assign(&b.s, &a.s);
    CHECK(str_bytes(b.s) == str_bytes(a.s));

    // Deep copy: mutating the source must not affect the copy.
    rt_string_putc(&a.s, 0, 'H');
    CHECK(str_bytes(b.s)[0] == 'h');

    rt_string_assign(&a.s, &a.s);
    CHECK(str_bytes(a.s)[0] == 'H');
}

TEST_CASE("rt_string_getc and putc enforce the LRM out-of-range and NUL rules") {
    Str s{ "abc" };
    CHECK(rt_string_getc(&s.s, 0) == 'a');
    CHECK(rt_string_getc(&s.s, 2) == 'c');
    CHECK(rt_string_getc(&s.s, 3) == 0);
    CHECK(rt_string_getc(&s.s, -1) == 0);

    rt_string_putc(&s.s, 1, 'X');
    CHECK(str_bytes(s.s) == "aXc");
    rt_string_putc(&s.s, 3, 'q');  // OOB: ignored
    rt_string_putc(&s.s, -1, 'q'); // OOB: ignored
    rt_string_putc(&s.s, 0, 0);    // NUL: ignored
    CHECK(str_bytes(s.s) == "aXc");
}

TEST_CASE("rt_string_cmp and icmp order in strcmp fashion") {
    Str abc{ "abc" };
    Str abd{ "abd" };
    Str ab{ "ab" };
    Str upper{ "ABC" };
    Str empty;

    CHECK(rt_string_cmp(&abc.s, &abd.s) < 0);
    CHECK(rt_string_cmp(&abd.s, &abc.s) > 0);
    CHECK(rt_string_cmp(&ab.s, &abc.s) < 0); // prefix orders before its extension
    CHECK(rt_string_cmp(&abc.s, &abc.s) == 0);
    CHECK(rt_string_cmp(&empty.s, &abc.s) < 0);
    CHECK(rt_string_cmp(&upper.s, &abc.s) < 0); // 'A' < 'a' in byte order

    CHECK(rt_string_icmp(&upper.s, &abc.s) == 0);
    CHECK(rt_string_icmp(&upper.s, &abd.s) < 0);
}

TEST_CASE("rt_string_substr yields the inclusive range or empty on invalid bounds") {
    Str s{ "hello" };
    Str d;
    rt_string_substr(&d.s, &s.s, 1, 3);
    CHECK(str_bytes(d.s) == "ell");
    rt_string_substr(&d.s, &s.s, 0, 4);
    CHECK(str_bytes(d.s) == "hello");
    rt_string_substr(&d.s, &s.s, -1, 2); // i < 0
    CHECK(rt_string_len(&d.s) == 0);
    rt_string_substr(&d.s, &s.s, 3, 2); // j < i
    CHECK(rt_string_len(&d.s) == 0);
    rt_string_substr(&d.s, &s.s, 0, 5); // j >= len
    CHECK(rt_string_len(&d.s) == 0);
}

TEST_CASE("rt_string_concat handles aliasing and empty parts; repl handles count zero") {
    Str a{ "foo" };
    Str b{ "bar" };
    Str empty;

    const rt_string* parts1[] = { &a.s, &empty.s, &b.s };
    Str              d;
    rt_string_concat(&d.s, parts1, 3);
    CHECK(str_bytes(d.s) == "foobar");

    // dst aliases a part: a = {a, b}.
    const rt_string* parts2[] = { &a.s, &b.s };
    rt_string_concat(&a.s, parts2, 2);
    CHECK(str_bytes(a.s) == "foobar");

    Str r;
    rt_string_repl(&r.s, &b.s, 3);
    CHECK(str_bytes(r.s) == "barbarbar");
    rt_string_repl(&r.s, &b.s, 0);
    CHECK(rt_string_len(&r.s) == 0);
}

TEST_CASE("rt_string_toupper and tolower transform ASCII letters, aliasing allowed") {
    Str s{ "Hello, World 123!" };
    Str d;
    rt_string_toupper(&d.s, &s.s);
    CHECK(str_bytes(d.s) == "HELLO, WORLD 123!");
    rt_string_tolower(&d.s, &d.s);
    CHECK(str_bytes(d.s) == "hello, world 123!");
}

TEST_CASE("rt_string_from_packed drops zero bytes and masks the partial top byte") {
    // LRM §6.16 example: string'(12'ha41) — bytes MSB-first are 0x0a, 0x41.
    Str           d;
    const uint8_t v12[2] = { 0x41, 0x0a }; // little-endian val plane
    rt_string_from_packed(&d.s, v12, 12);
    CHECK(str_bytes(d.s) == "\x0a\x41");

    // A zero byte inside the value is dropped.
    const uint8_t v24[4] = { 0x42, 0x00, 0x41, 0x00 }; // 24'h410042, plane_bytes(24)=4
    rt_string_from_packed(&d.s, v24, 24);
    CHECK(str_bytes(d.s) == "AB");

    const uint8_t zero[1] = { 0x00 };
    rt_string_from_packed(&d.s, zero, 8);
    CHECK(rt_string_len(&d.s) == 0);

    // Top-byte masking: bits above width read as 0. 12-bit value whose buffer
    // byte 1 carries junk above bit 3.
    const uint8_t vjunk[2] = { 0x41, 0xf0 };
    rt_string_from_packed(&d.s, vjunk, 12);
    CHECK(str_bytes(d.s) == "A");
}

TEST_CASE("rt_string_to_packed right-justifies with left truncation and zero fill") {
    Str     s{ "AB" }; // 0x41 0x42
    uint8_t out8[8] = {};

    rt_string_to_packed(&s.s, out8, 16);
    CHECK(out8[0] == 0x42);
    CHECK(out8[1] == 0x41);

    // Wider than the string: zero-filled on the left.
    rt_string_to_packed(&s.s, out8, 32);
    CHECK(out8[0] == 0x42);
    CHECK(out8[1] == 0x41);
    CHECK(out8[2] == 0x00);
    CHECK(out8[3] == 0x00);

    // Narrower: truncated on the left, keeping the low bytes.
    rt_string_to_packed(&s.s, out8, 8);
    CHECK(out8[0] == 0x42);

    // Partial top byte masks to the width.
    Str t{ "\x0a\x41" };
    rt_string_to_packed(&t.s, out8, 12);
    CHECK(out8[0] == 0x41);
    CHECK(out8[1] == 0x0a);

    Str empty;
    rt_string_to_packed(&empty.s, out8, 16);
    CHECK(out8[0] == 0x00);
    CHECK(out8[1] == 0x00);
}

TEST_CASE("rt_string_atoi family scans leading digits, no sign or whitespace") {
    Str s{ "123" };
    CHECK(rt_string_atoi(&s.s) == 123);

    Str neg{ "-123" };
    CHECK(rt_string_atoi(&neg.s) == 0); // no sign handling (LRM 6.16.9)

    Str ws{ "  123" };
    CHECK(rt_string_atoi(&ws.s) == 0); // no leading-whitespace skip

    Str us{ "1_2_3" };
    CHECK(rt_string_atoi(&us.s) == 123); // '_' separators skipped

    Str partial{ "12abc" };
    CHECK(rt_string_atoi(&partial.s) == 12); // stops at the first non-digit

    Str none{ "abc" };
    CHECK(rt_string_atoi(&none.s) == 0);

    Str empty;
    CHECK(rt_string_atoi(&empty.s) == 0);

    Str overflow{ "4294967296123" }; // wraps mod 2^32
    CHECK(rt_string_atoi(&overflow.s) == 123);

    Str hex{ "ff" };
    CHECK(rt_string_atohex(&hex.s) == 255);
    Str oct{ "17" };
    CHECK(rt_string_atooct(&oct.s) == 15);
    Str bin{ "101" };
    CHECK(rt_string_atobin(&bin.s) == 5);
}

TEST_CASE("rt_string_atoreal accepts leading whitespace and '+', unlike atoi") {
    Str s{ "3.14" };
    CHECK(rt_string_atoreal(&s.s) == doctest::Approx(3.14));

    Str ws{ "  -3.14" };
    CHECK(rt_string_atoreal(&ws.s) == doctest::Approx(-3.14));

    Str plus{ "+3.14" };
    CHECK(rt_string_atoreal(&plus.s) == doctest::Approx(3.14));

    Str exp{ "3.14e2" };
    CHECK(rt_string_atoreal(&exp.s) == doctest::Approx(314.0));

    Str none{ "abc" };
    CHECK(rt_string_atoreal(&none.s) == 0.0);
}

TEST_CASE("rt_string itoa family formats sign + magnitude, not two's-complement") {
    Str d;
    rt_string_itoa(&d.s, 123);
    CHECK(str_bytes(d.s) == "123");
    rt_string_itoa(&d.s, -123);
    CHECK(str_bytes(d.s) == "-123");

    rt_string_hextoa(&d.s, 255);
    CHECK(str_bytes(d.s) == "ff");
    rt_string_hextoa(&d.s, -256); // sign + magnitude, matching iverilog
    CHECK(str_bytes(d.s) == "-100");

    rt_string_octtoa(&d.s, 8);
    CHECK(str_bytes(d.s) == "10");
    rt_string_bintoa(&d.s, 5);
    CHECK(str_bytes(d.s) == "101");
}

TEST_CASE("rt_string_realtoa formats at full round-trip precision") {
    Str d;
    rt_string_realtoa(&d.s, 3.14);
    CHECK(str_bytes(d.s) == "3.14");
    rt_string_realtoa(&d.s, 100.0);
    CHECK(str_bytes(d.s) == "100"); // no trailing ".0"
    rt_string_realtoa(&d.s, 123456789.123);
    CHECK(str_bytes(d.s) == "123456789.123"); // full precision, unlike iverilog's %g
}

// -- rt_fwrite real formatting --

namespace {

// A value-form descriptor for the tests: single-word planes given by value.
// capture_fwrite materializes the pointer-based rt_arg with stable backing
// storage (all test operands are <= 64 bits, so one word per plane).
struct Arg {
    uint32_t width;
    uint32_t flags;
    uint64_t val;
    uint64_t xz;
    uint64_t z;
};

// Capture what rt_fwrite writes to the standard-output MCD.
std::string capture_fwrite(const char* fmt, std::initializer_list<Arg> args) {
    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    // `store` holds each plane triple; a deque keeps element addresses stable as
    // it grows, so the rt_arg plane pointers stay valid through rt_fwrite.
    std::deque<std::array<uint64_t, 3>> store;
    std::vector<rt_arg>                 argv;
    for (const Arg& a : args) {
        if ((a.flags & 2u) != 0) {
            argv.push_back(rt_arg{ a.width, a.flags, reinterpret_cast<const uint64_t*>(a.val), nullptr, nullptr });
            continue;
        }
        store.push_back({ a.val, a.xz, a.z });
        const auto& w = store.back();
        argv.push_back(rt_arg{ a.width, a.flags, &w[0], &w[1], &w[2] });
    }
    rt_fwrite(1, fmt, static_cast<uint32_t>(argv.size()), argv.data());

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

// A real-flagged descriptor (val = double bit pattern, flags bit 2 + signed).
Arg real_arg(double v) {
    uint64_t pat = 0;
    std::memcpy(&pat, &v, sizeof pat);
    return Arg{ 64, 0b101u, pat, 0, 0 };
}

} // namespace

TEST_CASE("rt_fwrite renders reals with %e / %f / %g and C passthrough") {
    CHECK(capture_fwrite("%f", { real_arg(3.6) }) == "3.600000");
    CHECK(capture_fwrite("%e", { real_arg(1234.5) }) == "1.234500e+03");
    CHECK(capture_fwrite("%g", { real_arg(3.6) }) == "3.6");
    CHECK(capture_fwrite("%g", { real_arg(0.00001234) }) == "1.234e-05");

    // Full C flag / width / precision passthrough (LRM 21.2 Table 21-2).
    CHECK(capture_fwrite("%10.3g|", { real_arg(3.14159) }) == "      3.14|");
    CHECK(capture_fwrite("%-8.2f|", { real_arg(2.5) }) == "2.50    |");
    CHECK(capture_fwrite("%08.2f", { real_arg(-1.5) }) == "-0001.50");

    // Case variants and non-finite values follow C.
    CHECK(capture_fwrite("%E", { real_arg(1234.5) }) == "1.234500E+03");
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(capture_fwrite("%f", { real_arg(inf) }) == "inf");
    CHECK(capture_fwrite("%g", { real_arg(-inf) }) == "-inf");
    CHECK(capture_fwrite("%f", { real_arg(std::numeric_limits<double>::quiet_NaN()) }) == "nan");
}

TEST_CASE("rt_fwrite integer specifiers are unchanged around real fields") {
    // Mixed integer + real fields consume descriptors in order; integer
    // fields keep the width-honoring justification (LRM §21.2.1.2).
    CHECK(capture_fwrite("a=%0d r=%g", { Arg{ 32, 1, 42, 0, 0 }, real_arg(0.5) }) == "a=42 r=0.5");
    CHECK(capture_fwrite("%5d", { Arg{ 8, 0, 7, 0, 0 } }) == "    7");
}

// -- svdpi.h packed-vector accessors (LRM Annex H) --

TEST_CASE("svdpi bit-select round-trips across word boundaries") {
    svBitVecVal b[2] = { 0, 0 }; // 64 bits
    for (int i = 0; i < 64; ++i) {
        svPutBitselBit(b, i, static_cast<svBit>(i % 5 == 0 ? 1 : 0));
    }
    for (int i = 0; i < 64; ++i) {
        CHECK(svGetBitselBit(b, i) == static_cast<svBit>(i % 5 == 0 ? 1 : 0));
    }
    CHECK(b[0] == 0x42108421u); // bits 0,5,10,15,20,25,30 set in the low word
}

TEST_CASE("svdpi logic bit-select encodes 0/1/Z/X into aval/bval") {
    svLogicVecVal v = { 0, 0 };
    svPutBitselLogic(&v, 0, sv_0);
    svPutBitselLogic(&v, 1, sv_1);
    svPutBitselLogic(&v, 2, sv_z);
    svPutBitselLogic(&v, 3, sv_x);
    CHECK(svGetBitselLogic(&v, 0) == sv_0);
    CHECK(svGetBitselLogic(&v, 1) == sv_1);
    CHECK(svGetBitselLogic(&v, 2) == sv_z);
    CHECK(svGetBitselLogic(&v, 3) == sv_x);
    // aval = value-or-X bits (1 and X): bits 1,3. bval = unknown bits (Z and X): 2,3.
    CHECK(v.aval == 0xAu);
    CHECK(v.bval == 0xCu);
}

TEST_CASE("svdpi part-select extracts and inserts a slice") {
    const svBitVecVal s = 0x1234ABCDu;
    svBitVecVal       d = 0;
    svGetPartselBit(&d, &s, 8, 8); // s[15:8]
    CHECK(d == 0xABu);

    svBitVecVal e = 0;
    svPutPartselBit(&e, 0xABu, 8, 8); // write 0xAB at bit 8
    CHECK(e == 0x0000AB00u);
}

TEST_CASE("svdpi logic part-select preserves x/z") {
    svLogicVecVal s = { 0, 0 };
    svPutBitselLogic(&s, 4, sv_x);
    svPutBitselLogic(&s, 5, sv_z);
    svPutBitselLogic(&s, 6, sv_1);
    svLogicVecVal d = { 0, 0 };
    svGetPartselLogic(&d, &s, 4, 3); // s[6:4] -> d[2:0]
    CHECK(svGetBitselLogic(&d, 0) == sv_x);
    CHECK(svGetBitselLogic(&d, 1) == sv_z);
    CHECK(svGetBitselLogic(&d, 2) == sv_1);
}

// -- svScope (LRM 35.5.3): single-instance scope + user data --

TEST_CASE("svdpi scope: get/set, name, and per-key user data") {
    rt_svdpi_scope_init("top.dut", 7);
    svScope s = svGetScope();
    REQUIRE(s != nullptr);
    CHECK(std::string(svGetNameFromScope(s)) == "top.dut");
    CHECK(svGetScopeFromName("top.dut") == s);
    CHECK(svGetScopeFromName("nope") == nullptr);

    // svSetScope swaps the active scope and returns the previous.
    CHECK(svSetScope(s) == s);

    // User data is per (scope, key); absent keys read null.
    int a = 11, b = 22;
    CHECK(svGetUserData(s, const_cast<char*>("a")) == nullptr);
    svPutUserData(s, const_cast<char*>("a"), &a);
    svPutUserData(s, const_cast<char*>("b"), &b);
    CHECK(svGetUserData(s, const_cast<char*>("a")) == &a);
    CHECK(svGetUserData(s, const_cast<char*>("b")) == &b);

    // Re-init resets the user data (a fresh simulation starts clean).
    rt_svdpi_scope_init("m", 1);
    CHECK(svGetUserData(svGetScope(), const_cast<char*>("a")) == nullptr);
    CHECK(std::string(svGetNameFromScope(svGetScope())) == "m");
}

// -- svdpi open-array descriptor + element marshal (LRM 35.5.6.2) --

TEST_CASE("svdpi open array: 1-D descriptor accessors") {
    int data[4] = { 10, 20, 30, 40 };
    rt_svopen a{ data, 0, 3, 4, sizeof(int), 32, 0 }; // int a[0:3], zero-copy
    auto*     h = static_cast<svOpenArrayHandle>(&a);
    CHECK(svDimensions(h) == 1);
    CHECK(svSize(h, 1) == 4);
    CHECK(svLeft(h, 1) == 0);
    CHECK(svRight(h, 1) == 3);
    CHECK(svLow(h, 1) == 0);
    CHECK(svHigh(h, 1) == 3);
    CHECK(svIncrement(h, 1) == -1); // left < right
    CHECK(svGetArrayPtr(h) == data);
    CHECK(*static_cast<int*>(svGetArrElemPtr1(h, 0)) == 10);
    CHECK(*static_cast<int*>(svGetArrElemPtr1(h, 3)) == 40);
}

TEST_CASE("svdpi open array: descending bounds normalize the element index") {
    int       data[3] = { 100, 200, 300 };
    rt_svopen a{ data, 2, 0, 3, sizeof(int), 32, 0 }; // int a[2:0]: a[2] is elem 0
    auto*     h = static_cast<svOpenArrayHandle>(&a);
    CHECK(svIncrement(h, 1) == 1); // left >= right
    CHECK(*static_cast<int*>(svGetArrElemPtr1(h, 2)) == 100); // a[2] -> pos 0
    CHECK(*static_cast<int*>(svGetArrElemPtr1(h, 0)) == 300); // a[0] -> pos 2
}

TEST_CASE("svdpi open array: 2-state element marshal (bit[15:0] <-> svBitVecVal)") {
    uint16_t    in[3] = { 0x1234, 0xABCD, 0x0001 }; // narrow {i16} slots, stride 2
    svBitVecVal out[3];
    rt_dpi_pack_arr_bit(in, sizeof(uint16_t), 16, 3, out);
    CHECK(out[0] == 0x1234u);
    CHECK(out[1] == 0xABCDu);
    CHECK(out[2] == 1u);
    uint16_t back[3] = {};
    rt_dpi_unpack_arr_bit(out, sizeof(uint16_t), 16, 3, back);
    CHECK(back[0] == in[0]);
    CHECK(back[1] == in[1]);
    CHECK(back[2] == in[2]);
}

TEST_CASE("svdpi open array: 4-state element marshal preserves x/z (logic[7:0])") {
    // Clotho logic[7:0] element slot {i16 mm, i8 z} padded to 4 bytes.
    struct Slot {
        uint16_t mm;
        uint8_t  z;
        uint8_t  pad;
    };
    static_assert(sizeof(Slot) == 4);
    Slot in[2] = { { 0x7744, 0x11, 0 }, { 0xFFFF, 0x00, 0 } }; // 01xz01xz, then 8'hFF
    svLogicVecVal out[2];
    rt_dpi_pack_arr_logic(in, sizeof(Slot), offsetof(Slot, z), /*has_z=*/1, 8, 2, out);
    CHECK(out[0].aval == 0x66u); // value-or-X
    CHECK(out[0].bval == 0x33u); // X or Z
    CHECK(out[1].aval == 0xFFu);
    CHECK(out[1].bval == 0x00u);
    Slot back[2] = {};
    rt_dpi_unpack_arr_logic(out, sizeof(Slot), offsetof(Slot, z), /*has_z=*/1, 8, 2, back);
    CHECK(back[0].mm == in[0].mm);
    CHECK(back[0].z == in[0].z);
    CHECK(back[1].mm == in[1].mm);
    CHECK(back[1].z == in[1].z);
}

TEST_CASE("svdpi open array: never-z 4-state element (no z word, has_z=0)") {
    // A logic[7:0] whose z was elided: slot is {i16 mm} only, stride 2, no z field.
    // A clean value V has must==may==V, so mm = V | (V<<8).
    uint16_t      in[3] = { 0xA5A5, 0x3C3C, 0x0F0F }; // clean values (must==may)
    svLogicVecVal out[3];
    rt_dpi_pack_arr_logic(in, sizeof(uint16_t), /*z_off=*/0, /*has_z=*/0, 8, 3, out);
    CHECK(out[0].aval == 0xA5u);
    CHECK(out[0].bval == 0x00u);
    CHECK(out[1].aval == 0x3Cu);
    CHECK(out[2].aval == 0x0Fu);
}
