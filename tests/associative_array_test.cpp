#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors queue_test.cpp / dynamic_array_test.cpp).
// Associative-array behaviour below follows LRM 7.8/7.9. iverilog 13 cannot parse
// an associative-array declaration at all, and verilator violates LRM 7.8 by
// CREATING an entry on a read of an absent key (its num()/foreach then include
// the phantom key).
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

} // namespace

// a[k] write creates the entry; read returns the value; num() counts entries;
// a read of an absent key returns the element default and does NOT create it
// (LRM 7.8 — verilator wrongly creates it, so num stays 2 here for clotho).
TEST_CASE("assoc integral put/get/num, absent-read no create") {
    constexpr std::string_view src = R"(
        module m;
            int a[int];
            initial begin
                a[5] = 100;
                a[2] = 200;
                $display("num=%0d a5=%0d a2=%0d", a.num(), a[5], a[2]);
                $display("miss=%0d numafter=%0d", a[7], a.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=2 a5=100 a2=200\nmiss=0 numafter=2\n");
}

// exists() reports membership without creating; delete(k) removes one entry;
// delete() clears all (LRM 7.9.1/7.9.2).
TEST_CASE("assoc exists/delete/delete-all") {
    constexpr std::string_view src = R"(
        module m;
            int a[int];
            initial begin
                a[5] = 1; a[2] = 2; a[9] = 3;
                $display("ex5=%0d ex7=%0d", a.exists(5), a.exists(7));
                a.delete(2);
                $display("num=%0d ex2=%0d", a.num(), a.exists(2));
                a.delete();
                $display("cleared=%0d", a.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ex5=1 ex7=0\nnum=2 ex2=0\ncleared=0\n");
}

// foreach binds the loop variable to each key in ASCENDING key order
// (LRM 7.9.3), regardless of insertion order.
TEST_CASE("assoc foreach ascending key order") {
    constexpr std::string_view src = R"(
        module m;
            int a[int];
            int k;
            initial begin
                a[9] = 90; a[2] = 20; a[5] = 50; a[7] = 70;
                foreach (a[k]) $display("k=%0d v=%0d", k, a[k]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "k=2 v=20\nk=5 v=50\nk=7 v=70\nk=9 v=90\n");
}

// A signed key type orders negative keys below positive ones (the normalized
// offset-binary form makes two's-complement sort numerically).
TEST_CASE("assoc signed key ordering") {
    constexpr std::string_view src = R"(
        module m;
            int a[int];
            int k;
            initial begin
                a[3] = 1; a[-7] = 2; a[0] = 3; a[-1] = 4;
                foreach (a[k]) $display("k=%0d", k);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "k=-7\nk=-1\nk=0\nk=3\n");
}

// first/last seed the min/max key; next/prev step; each returns 1 while a key
// remains, 0 at the end (LRM 7.9.3/7.9.4). next/prev accept an absent probe key.
TEST_CASE("assoc first/last/next/prev traversal") {
    constexpr std::string_view src = R"(
        module m;
            int a[int];
            int k;
            initial begin
                a[10] = 0; a[20] = 0; a[30] = 0;
                void'(a.first(k)); $display("first=%0d", k);
                void'(a.last(k));  $display("last=%0d", k);
                k = 20; void'(a.next(k)); $display("next20=%0d", k);
                k = 20; void'(a.prev(k)); $display("prev20=%0d", k);
                k = 15; void'(a.next(k)); $display("next15=%0d", k);
                k = 30; $display("nextend=%0d", a.next(k));
                k = 10; $display("preven=%0d", a.prev(k));
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "first=10\nlast=30\nnext20=30\nprev20=10\nnext15=20\nnextend=0\npreven=0\n");
}

// String-keyed arrays iterate in lexicographic (byte) order (LRM 7.8.2/7.9.3),
// and exists/index/foreach work as for integral keys.
TEST_CASE("assoc string keys, lexicographic foreach") {
    constexpr std::string_view src = R"(
        module m;
            int a[string];
            string s;
            initial begin
                a["bob"] = 1; a["amy"] = 2; a["cat"] = 3; a["ab"] = 4;
                $display("num=%0d ex=%0d exmiss=%0d amy=%0d", a.num(),
                         a.exists("amy"), a.exists("zzz"), a["amy"]);
                foreach (a[s]) $display("s=%s v=%0d", s, a[s]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=4 ex=1 exmiss=0 amy=2\n"
                              "s=ab v=4\ns=amy v=2\ns=bob v=1\ns=cat v=3\n");
}

// A wildcard index (`[*]`, LRM 7.8.1) treats numerically-equal indices of any
// width as the SAME entry, and supports index/num/exists/delete (no ordered
// traversal — there is no key type).
TEST_CASE("assoc wildcard cross-width equality") {
    constexpr std::string_view src = R"(
        module m;
            int a[*];
            initial begin
                a[8'd5]  = 50;
                a[32'd5] = 99;
                a[300]   = 7;
                $display("num=%0d w5=%0d w300=%0d ex5=%0d ex6=%0d",
                         a.num(), a[5], a[300], a.exists(5), a.exists(6));
                a.delete(5);
                $display("afterdel num=%0d ex5=%0d", a.num(), a.exists(5));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=2 w5=99 w300=7 ex5=1 ex6=0\nafterdel num=1 ex5=0\n");
}

// ---------------------------------------------------------------------------
// Unpacked-struct keys (LRM 7.8.5 "other user-defined types") and class-handle
// keys (LRM 7.8.3). Both fold into the same normalized byte span the integral
// and string keys use: a struct serializes leaf by leaf (never a memcpy of its
// padded storage image), a handle serializes its 64-bit identity word.
// ---------------------------------------------------------------------------

// A struct key orders lexicographically by member in declaration order, with each
// member compared in its own signedness -- the ordering LRM 7.8.5 leaves free.
TEST_CASE("assoc unpacked-struct keys: distinct entries and member-order traversal") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { byte b; int i; } key_t;
            typedef struct { int hi; byte lo; } sk_t;
            int   a [key_t];
            int   s [sk_t];
            key_t k;
            sk_t  sk;
            initial begin
                k = '{8'd1, 100}; a[k] = 11;
                k = '{8'd1, 200}; a[k] = 12;
                k = '{8'd2, 100}; a[k] = 21;
                k = '{8'd1, 100}; a[k] = 99;       // same key: overwrite
                $display("num=%0d get=%0d ex=%0d", a.num(), a[k], a.exists(k));
                k = '{8'd9, 999};
                $display("absent=%0d ex=%0d", a[k], a.exists(k));
                foreach (a[kk]) $display("k b=%0d i=%0d v=%0d", kk.b, kk.i, a[kk]);
                k = '{8'd1, 200}; a.delete(k);
                $display("del num=%0d ex=%0d", a.num(), a.exists(k));
                sk = '{-5, 8'd1}; s[sk] = 1;
                sk = '{3, 8'd1};  s[sk] = 2;
                sk = '{-5, 8'd7}; s[sk] = 3;
                foreach (s[x]) $display("s hi=%0d lo=%0d v=%0d", x.hi, x.lo, s[x]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=3 get=99 ex=1\n"
                              "absent=0 ex=0\n"
                              "k b=1 i=100 v=99\n"
                              "k b=1 i=200 v=12\n"
                              "k b=2 i=100 v=21\n"
                              "del num=2 ex=0\n"
                              "s hi=-5 lo=1 v=1\n"
                              "s hi=-5 lo=7 v=3\n"
                              "s hi=3 lo=1 v=2\n");
}

// Nesting composes: a struct member, a fixed unpacked array member, and the
// first/next readout writing a whole struct key variable back.
TEST_CASE("assoc struct keys: nested struct, array member, traversal readout") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { byte b; int i; } inner_t;
            typedef struct { inner_t in; logic [3:0] tag; } nest_t;
            typedef struct { int a [0:1]; byte b; } witharr_t;
            int       n [nest_t];
            int       w [witharr_t];
            nest_t    nk;
            witharr_t wk;
            initial begin
                nk = '{'{8'd1, 10}, 4'h2}; n[nk] = 1;
                nk = '{'{8'd1, 11}, 4'h2}; n[nk] = 2;
                nk = '{'{8'd1, 10}, 4'h3}; n[nk] = 3;
                $display("nnum=%0d", n.num());
                foreach (n[k]) $display("n b=%0d i=%0d tag=%0h v=%0d", k.in.b, k.in.i, k.tag, n[k]);
                if (n.first(nk)) $display("first b=%0d i=%0d tag=%0h", nk.in.b, nk.in.i, nk.tag);
                if (n.next(nk))  $display("next  b=%0d i=%0d tag=%0h", nk.in.b, nk.in.i, nk.tag);
                if (n.last(nk))  $display("last  b=%0d i=%0d tag=%0h", nk.in.b, nk.in.i, nk.tag);
                wk = '{'{1, 2}, 8'd7}; w[wk] = 10;
                wk = '{'{1, 3}, 8'd7}; w[wk] = 20;
                foreach (w[k]) $display("w a0=%0d a1=%0d b=%0d v=%0d", k.a[0], k.a[1], k.b, w[k]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "nnum=3\n"
                              "n b=1 i=10 tag=2 v=1\n"
                              "n b=1 i=10 tag=3 v=3\n"
                              "n b=1 i=11 tag=2 v=2\n"
                              "first b=1 i=10 tag=2\n"
                              "next  b=1 i=10 tag=3\n"
                              "last  b=1 i=11 tag=2\n"
                              "w a0=1 a1=2 b=7 v=10\n"
                              "w a0=1 a1=3 b=7 v=20\n");
}

// A `'{...}` pattern is a legal key expression: it has no storage of its own, so
// it is staged into a canonical temp before serialization.
TEST_CASE("assoc struct key from an assignment pattern, and a struct value") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { byte b; int i; } key_t;
            typedef struct { int x; logic [7:0] y; } val_t;
            val_t a [key_t];
            key_t k;
            val_t v;
            initial begin
                a['{8'd3, 30}] = '{7, 8'h55};
                k = '{8'd3, 30};
                v = a[k];
                $display("num=%0d x=%0d y=%0h", a.num(), v.x, v.y);
                a[k].x = 8;
                $display("upd x=%0d", a[k].x);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=1 x=7 y=55\n"
                              "upd x=8\n");
}

// A class-handle key is the handle's identity (LRM 7.8.3): distinct objects are
// distinct keys, `null` is a valid key, and an index of a DERIVED class is legal.
// The traversal order is "deterministic but arbitrary" per 7.8.3 -- it is handle
// order here -- so this checks order-independent facts only.
TEST_CASE("assoc class-handle keys: identity, null, derived index, traversal") {
    constexpr std::string_view src = R"(
        module m;
            class B; int id; endclass
            class D extends B; int extra; endclass
            string a [B];
            B      b1, b2, kb;
            D      d1;
            int    seen, sum;
            initial begin
                b1 = new; b1.id = 1;
                b2 = new; b2.id = 2;
                d1 = new; d1.id = 3;
                a[b1] = "one"; a[b2] = "two"; a[d1] = "three"; a[null] = "nul";
                $display("num=%0d one=%s two=%s three=%s nul=%s",
                         a.num(), a[b1], a[b2], a[d1], a[null]);
                $display("ex=%0d exnull=%0d", a.exists(b1), a.exists(null));
                a[b1] = "uno";
                $display("ovw=%s num=%0d", a[b1], a.num());
                // Order-independent: every entry is visited exactly once. A `?:` is
                // avoided on purpose -- both arms may be evaluated, and one of the
                // keys is null.
                seen = 0; sum = 0;
                if (a.first(kb)) begin
                    seen = seen + 1;
                    if (kb == null) sum = sum + 100; else sum = sum + kb.id;
                    while (a.next(kb)) begin
                        seen = seen + 1;
                        if (kb == null) sum = sum + 100; else sum = sum + kb.id;
                    end
                end
                $display("seen=%0d sum=%0d", seen, sum);
                a.delete(null);
                $display("del num=%0d exnull=%0d b1=%s", a.num(), a.exists(null), a[b1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=4 one=one two=two three=three nul=nul\n"
                              "ex=1 exnull=1\n"
                              "ovw=uno num=4\n"
                              "seen=4 sum=106\n"
                              "del num=3 exnull=0 b1=uno\n");
}

// LRM 7.8.4 / 7.8.5: an index that is or contains an x/z bit is INVALID, and
// 7.8.6 makes a read of an invalid index return the element default (creating
// nothing) and a write no-op. Clotho used to normalize the must plane, so `a['x]`
// silently aliased `a[0]`.
TEST_CASE("assoc invalid (x/z) key: read defaults, write drops, exists is 0") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { logic [3:0] n; logic [7:0] w; } k4_t;
            int     ik [integer];
            int     sk [k4_t];
            integer xi;
            logic [3:0] zi;
            k4_t    k;
            initial begin
                xi = 'x; ik[xi] = 5;                  // dropped
                ik[0] = 7;
                $display("i num=%0d xrd=%0d zero=%0d ex=%0d", ik.num(), ik[xi], ik[0], ik.exists(xi));
                zi = 4'bz01; ik[zi] = 6;              // dropped (z is invalid too)
                $display("z num=%0d", ik.num());
                k = '{4'h3, 8'h55}; sk[k] = 7;
                $display("s num=%0d ok=%0d", sk.num(), sk[k]);
                k = '{4'hx, 8'h55}; sk[k] = 9;        // dropped: a member is x
                $display("s xnum=%0d xrd=%0d ex=%0d", sk.num(), sk[k], sk.exists(k));
                sk.delete(k);                          // no-op, not a wild delete
                $display("s del=%0d", sk.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "i num=1 xrd=0 zero=7 ex=0\n"
                              "z num=1\n"
                              "s num=1 ok=7\n"
                              "s xnum=1 xrd=0 ex=0\n"
                              "s del=1\n");
}

// Keys are owned by the map, so a struct-keyed array deep-copies like any other.
TEST_CASE("assoc struct-keyed deep copy independence") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { byte b; int i; } key_t;
            int   a [key_t];
            int   c [key_t];
            key_t k;
            initial begin
                k = '{8'd1, 10}; a[k] = 1;
                k = '{8'd2, 20}; a[k] = 2;
                c = a;
                k = '{8'd1, 10}; c[k] = 111;
                k = '{8'd3, 30}; c[k] = 3;
                $display("a=%0d c=%0d", a.num(), c.num());
                k = '{8'd1, 10}; $display("a1=%0d c1=%0d", a[k], c[k]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a=2 c=3\n"
                              "a1=1 c1=111\n");
}

// The key-type gates that remain (LRM 7.8.5 allows a string / dynamically sized
// member; Clotho's fixed-width leaf span does not model one yet).
TEST_CASE("assoc key types Clotho still refuses loudly") {
    SUBCASE("struct key with a string member") {
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { string s; int i; } k_t;
                int a [k_t];
                k_t k;
                initial a[k] = 1;
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("key type with a string member") != std::string::npos);
    }
    SUBCASE("struct key with a queue member") {
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { int q[$]; int i; } k_t;
                int a [k_t];
                k_t k;
                initial a[k] = 1;
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("queue / dynamic-array member") != std::string::npos);
    }
    SUBCASE("event key") {
        const LowerResult r = lower_text(R"(
            module m;
                int a [event];
                event e;
                initial a[e] = 1;
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("key type is not supported") != std::string::npos);
    }
    // A chandle VALUE now lowers (see the chandle-value test below); a chandle KEY
    // is a separate, still-unimplemented admit-list widening tracked in
    // docs/design/assoc-keys.md's P0 phase ("Status: scoping"), deliberately left
    // alone here.
    SUBCASE("chandle key") {
        const LowerResult r = lower_text(R"(
            module m;
                int a [chandle];
                chandle c;
                initial a[c] = 1;
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("key type is not supported") != std::string::npos);
    }
}

// A chandle VALUE element (LRM 6.14): a plain 64-bit POD word, admitted by the
// same guard_container_element gate a class-handle value uses (see the
// "assoc class-handle keys" test above for the KEY-side analogue). libc
// malloc/free is the no-.so probe for a non-null chandle.
TEST_CASE("assoc chandle value: absent-key null default and identity") {
    constexpr std::string_view src = R"(
        module m;
            import "DPI-C" function chandle malloc(input longint size);
            import "DPI-C" function void    free(input chandle p);
            chandle a[int];
            chandle h1, h2, x;
            initial begin
                x = a[5];
                $display("absent null=%0b", x == null);
                h1 = malloc(8);
                h2 = malloc(8);
                a[1] = h1;
                a[2] = h2;
                $display("num=%0d same=%0b other=%0b", a.num(), a[1] == h1, a[1] == a[2]);
                a.delete(1);
                $display("num=%0d exists1=%0b", a.num(), a.exists(1));
                free(h1);
                free(h2);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "absent null=1\n"
                              "num=2 same=1 other=0\n"
                              "num=1 exists1=0\n");
}

// A 4-state value element reads its power-on default X for an absent key
// (Table 7-1); a 2-state element reads 0.
TEST_CASE("assoc absent-key element default by domain") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a[int];
            bit   [7:0] b[int];
            initial begin
                a[1] = 8'hAB; b[1] = 8'hCD;
                $display("a1=%h amiss=%h", a[1], a[9]);
                $display("b1=%h bmiss=%h", b[1], b[9]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a1=ab amiss=xx\nb1=cd bmiss=00\n");
}

// Assignment is a deep copy (value semantics, LRM 7.9): mutating the copy does
// not touch the source, and vice versa.
TEST_CASE("assoc deep-copy independence") {
    constexpr std::string_view src = R"(
        module m;
            int a[int];
            int c[int];
            initial begin
                a[1] = 10; a[2] = 20;
                c = a;
                c[1] = 999;   // mutate copy
                c[3] = 30;    // add to copy
                a[2] = 888;   // mutate source
                $display("a1=%0d a2=%0d a3ex=%0d anum=%0d", a[1], a[2], a.exists(3), a.num());
                $display("c1=%0d c2=%0d c3=%0d cnum=%0d", c[1], c[2], c[3], c.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a1=10 a2=888 a3ex=0 anum=2\n"
                              "c1=999 c2=20 c3=30 cnum=3\n");
}

// An input associative-array argument is a copy-in (LRM 13.5.1 value semantics):
// the callee mutating its formal leaves the caller's array unchanged.
TEST_CASE("assoc input argument is copy-in") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int clobber(input int x[int]);
                x[1] = 777;
                x[9] = 9;
                return x.num();
            endfunction
            int a[int];
            int rc;
            initial begin
                a[1] = 1; a[2] = 2;
                rc = clobber(a);
                $display("rc=%0d a1=%0d anum=%0d a9ex=%0d", rc, a[1], a.num(), a.exists(9));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "rc=3 a1=1 anum=2 a9ex=0\n");
}

// String value element (LRM 7.8 + Table 7-1): a[k] write deep-copies the string
// into the entry; a read of an absent key returns "" (the string default) and
// does NOT create the entry (num unchanged).
TEST_CASE("assoc string value put/get, absent-read empty no create") {
    constexpr std::string_view src = R"(
        module m;
            string a[int];
            initial begin
                a[5] = "hello";
                a[2] = "world";
                $display("num=%0d a5=%s a2=%s", a.num(), a[5], a[2]);
                $display("miss=[%s] numafter=%0d", a[9], a.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=2 a5=hello a2=world\nmiss=[] numafter=2\n");
}

// Overwriting an existing key deep-copies the new string into the live entry
// (no leak, count unchanged); delete removes it.
TEST_CASE("assoc string value overwrite and delete") {
    constexpr std::string_view src = R"(
        module m;
            string a[int];
            initial begin
                a[1] = "first";
                a[1] = "second";
                $display("num=%0d a1=%s", a.num(), a[1]);
                a.delete(1);
                $display("num=%0d ex1=%0d", a.num(), a.exists(1));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=1 a1=second\nnum=0 ex1=0\n");
}

// Both key and value are strings (LRM 7.8.1 string index), ascending-key foreach.
TEST_CASE("assoc string key and string value, ascending foreach") {
    constexpr std::string_view src = R"(
        module m;
            string a[string];
            initial begin
                a["bob"] = "dog";
                a["amy"] = "cat";
                a["cal"] = "fox";
                foreach (a[k])
                    $display("k=%s v=%s", k, a[k]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "k=amy v=cat\nk=bob v=dog\nk=cal v=fox\n");
}

// Whole-array deep-copy assignment (LRM 7.8): the copy is independent, and each
// entry's string leaf is deep-copied — mutating the copy leaves the source
// untouched.
TEST_CASE("assoc string value deep-copy independence") {
    constexpr std::string_view src = R"(
        module m;
            string a[int];
            string b[int];
            initial begin
                a[1] = "x";
                a[2] = "y";
                b = a;
                b[2] = "changed";
                b[3] = "new";
                $display("a2=%s anum=%0d", a[2], a.num());
                $display("b2=%s b3=%s bnum=%0d", b[2], b[3], b.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a2=y anum=2\nb2=changed b3=new bnum=3\n");
}

// Unpacked-struct value element (LRM 7.8 + 7.2): member read/write per key, a
// member write creates the entry, an absent-key member read returns the leaf
// default (Table 7-1: 0 for a 2-state member) and does not create the entry.
TEST_CASE("assoc unpacked-struct value member access") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a; byte b; } pair;
            pair t[int];
            initial begin
                t[1] = '{a:10, b:20};
                t[2].a = 30;
                t[2].b = 40;
                $display("num=%0d a1=%0d b1=%0d", t.num(), t[1].a, t[1].b);
                $display("a2=%0d b2=%0d", t[2].a, t[2].b);
                $display("miss_a=%0d ex9=%0d num=%0d", t[9].a, t.exists(9), t.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=2 a1=10 b1=20\na2=30 b2=40\nmiss_a=0 ex9=0 num=2\n");
}

// A 4-state struct leaf reads x on an absent key (proto x-init image), a 2-state
// leaf reads 0 -- the per-leaf power-on default the entry is created with.
TEST_CASE("assoc unpacked-struct absent-key 4-state default") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { logic [7:0] x; int y; } t_t;
            t_t t[int];
            initial begin
                t[1] = '{x:8'hab, y:5};
                $display("x1=%h y1=%0d", t[1].x, t[1].y);
                $display("xmiss=%h ymiss=%0d num=%0d", t[9].x, t[9].y, t.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "x1=ab y1=5\nxmiss=xx ymiss=0 num=1\n");
}

// Whole-struct read (s = a[k]) and write (a[k] = s) copy by value: mutating the
// copy leaves the map entry untouched.
TEST_CASE("assoc unpacked-struct whole-value read and write") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a; byte b; } pair;
            pair t[int];
            pair s;
            initial begin
                t[1] = '{a:7, b:8};
                s = t[1];
                $display("sa=%0d sb=%0d", s.a, s.b);
                t[2] = s;
                s.a = 99;
                $display("t2a=%0d t2b=%0d sa=%0d", t[2].a, t[2].b, s.a);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sa=7 sb=8\nt2a=7 t2b=8 sa=99\n");
}

// Whole-struct equality (LRM 11.4.5 per-member reduction) over an assoc element,
// including an absent key reading the all-default value.
TEST_CASE("assoc unpacked-struct equality") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a; byte b; } pair;
            pair t[int];
            pair s;
            pair d;
            pair z;
            initial begin
                t[1] = '{a:5, b:6};
                s = '{a:5, b:6};
                d = '{a:5, b:7};
                z = '{a:0, b:0};
                $display("eq=%0d ne=%0d misseq=%0d num=%0d",
                         t[1] == s, t[1] == d, t[9] == z, t.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "eq=1 ne=0 misseq=1 num=1\n");
}

// Fixed unpacked-array value element: element access per key, whole-array read,
// absent-key element default.
TEST_CASE("assoc fixed-array value element") {
    constexpr std::string_view src = R"(
        module m;
            typedef int trip [0:2];
            trip t[int];
            trip r;
            initial begin
                t[1] = '{10, 20, 30};
                $display("num=%0d e0=%0d e1=%0d e2=%0d", t.num(), t[1][0], t[1][1], t[1][2]);
                r = t[1];
                $display("r0=%0d r2=%0d", r[0], r[2]);
                $display("miss=%0d num=%0d", t[5][1], t.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=1 e0=10 e1=20 e2=30\nr0=10 r2=30\nmiss=0 num=1\n");
}

// Whole-array deep-copy of an aggregate-valued assoc: the copy is independent.
TEST_CASE("assoc unpacked-struct deep-copy independence") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a; byte b; } pair;
            pair a[int];
            pair b[int];
            initial begin
                a[1] = '{a:1, b:2};
                a[2] = '{a:3, b:4};
                b = a;
                b[2].a = 88;
                b[3] = '{a:5, b:6};
                $display("a2a=%0d anum=%0d", a[2].a, a.num());
                $display("b2a=%0d b3a=%0d bnum=%0d", b[2].a, b[3].a, b.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a2a=3 anum=2\nb2a=88 b3a=5 bnum=3\n");
}

// -- Associative array literals (LRM 7.9.11) --------------------------------

// The LRM 7.9.11 examples verbatim, and the sv-tests chapter-7 literals case: a
// `'{default: v}` initializer makes every absent key read back `v` (and no
// warning), keyed entries are created, and the default does not count as one.
TEST_CASE("assoc literal with a default (LRM 7.9.11)") {
    constexpr std::string_view src = R"(
        module m;
            string  words [int]    = '{default: "hello"};
            integer tab [string]   = '{"Peter":20, "Paul":22, "Mary":23, default:-1};
            initial begin
                $display("%s", words[1]);
                words[1] = "world";
                $display("%s %s num=%0d", words[0], words[1], words.num());
                $display("%0d %0d %0d num=%0d", tab["Peter"], tab["Mary"], tab["Nobody"],
                         tab.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "hello\n"
                              "hello world num=1\n"
                              "20 23 -1 num=3\n");
}

// A keyed literal with no default leaves absent keys at the Table 7-1 element
// default, and the entries it wrote are ordinary entries (num, foreach, first).
TEST_CASE("assoc literal without a default") {
    constexpr std::string_view src = R"(
        module m;
            int    f[int]    = '{1:10, 2:20};
            string k;
            int    g[string] = '{"b":2, "a":1};
            initial begin
                $display("f1=%0d f3=%0d num=%0d", f[1], f[3], f.num());
                if (g.first(k)) $display("first=%s", k);
                foreach (g[key]) $display("%s=%0d", key, g[key]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "f1=10 f3=0 num=2\n"
                              "first=a\n"
                              "a=1\n"
                              "b=2\n");
}

// The default is per-array state, not an entry: a.delete() keeps it (LRM 7.9.11
// makes it independent of the array's contents), and a created entry starts from
// the ELEMENT default rather than the pattern default, so a partial write into an
// absent key yields x-bits, not the default.
TEST_CASE("assoc literal default survives delete and does not seed entries") {
    constexpr std::string_view src = R"(
        module m;
            int         a[int] = '{default: 7};
            logic [7:0] d[int] = '{default: 8'hAA};
            initial begin
                a[1] = 1;
                a.delete();
                $display("miss=%0d num=%0d", a[9], a.num());
                d[5][0] = 1'b0;
                $display("d5=%h num=%0d", d[5], d.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "miss=7 num=0\n"
                              "d5=xX num=1\n");
}

// The default is part of the value, so a whole-array copy carries it and a copy
// from an array without one clears the target's.
TEST_CASE("assoc literal default follows a whole-array copy") {
    constexpr std::string_view src = R"(
        module m;
            int a[int] = '{default: 7};
            int b[int];
            int c[int];
            initial begin
                b = a;
                $display("bmiss=%0d bexists=%0d", b[9], b.exists(9));
                c[3] = 3;
                a = c;
                $display("amiss=%0d anum=%0d", a[9], a.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "bmiss=7 bexists=0\n"
                              "amiss=0 anum=1\n");
}

// A literal is also an assignment RHS, and it replaces the whole array: the old
// entries and the old default both go. Works on a procedural local and inside a
// function, where the array is frame storage.
TEST_CASE("assoc literal as an assignment and in a subroutine") {
    constexpr std::string_view src = R"(
        module m;
            int g[int] = '{default: 1};
            function automatic int pick(int k);
                int t[int] = '{5:50, default:8};
                return t[k];
            endfunction
            initial begin
                g[7] = 70;
                g = '{2:20, default:42};
                $display("g2=%0d g7=%0d num=%0d", g[2], g[7], g.num());
                g = '{3:30};
                $display("g3=%0d gmiss=%0d num=%0d", g[3], g[9], g.num());
                $display("fn=%0d %0d", pick(5), pick(6));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "g2=20 g7=42 num=1\n"
                              "g3=30 gmiss=0 num=1\n"
                              "fn=50 8\n");
}

// A wildcard-index array takes a default too (7.9.11 puts no restriction on the
// index type; only the index LOCATOR methods exclude `[*]`), and a real value
// element folds the same way.
TEST_CASE("assoc literal over a wildcard index and a real element") {
    constexpr std::string_view src = R"(
        module m;
            int  w[*]   = '{default: 5};
            real r[int] = '{1:1.5, default:2.5};
            initial begin
                $display("w=%0d num=%0d", w[3], w.num());
                $display("r1=%0.2f rmiss=%0.2f", r[1], r[9]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "w=5 num=0\n"
                              "r1=1.50 rmiss=2.50\n");
}

// -- Selects THROUGH an associative entry (LRM 7.8 x 7.10 / 7.5) --
//
// `a[k][i]` has to resolve the entry before it can do the inner container's ring
// arithmetic, so the get_ptr / put_ptr choice cannot wait for the consumer the way a
// bare `a[k]` can: the access direction is threaded down the select chain instead
// (ir::ElemSelect::for_write). The LRM constraint both halves must respect is 7.8.6 —
// a READ creates nothing — and 7.8: a WRITE creates the entry, after which the fresh
// empty container drops the element write (7.10).
//
// No oracle: iverilog compiles neither a nested associative array nor a `foreach`
// over one.
TEST_CASE("element select through an associative entry that is a queue") {
    constexpr std::string_view src = R"(
        module m;
            int all[string][$];
            initial begin
                all["k"].push_back(5);
                all["k"].push_back(6);
                $display("read %0d %0d size=%0d", all["k"][0], all["k"][1], all["k"].size());
                all["k"][1] = 9;
                $display("wrote %0d num=%0d", all["k"][1], all.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "read 5 6 size=2\nwrote 9 num=1\n");
}

TEST_CASE("element select through an associative entry that is a dynamic array") {
    constexpr std::string_view src = R"(
        module m;
            int all[string][];
            initial begin
                all["k"] = new[3];
                all["k"][2] = 8;
                $display("%0d %0d num=%0d", all["k"][0], all["k"][2], all.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 8 num=1\n");
}

// The 7.8.6 half, stated as an assertion rather than a comment: a read of an absent
// key yields the element default and leaves num / exists alone. Checked at all three
// nesting shapes, because each resolves the entry through a different path — the
// container-element ring math, the inner map's handle, and a struct member's GEP.
TEST_CASE("a nested read never creates the entry") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int f[$]; } S;
            int aq[string][$];
            int aa[string][string];
            S   as[string];
            initial begin
                aq["k"].push_back(1);
                aa["a"]["b"] = 2;
                as["k"].f.push_back(3);
                $display("%0d %0d %0d", aq["miss"][0], aa["miss"]["x"], as["miss"].f[0]);
                $display("num %0d %0d %0d", aq.num(), aa.num(), as.num());
                $display("exists %0d %0d %0d", aq.exists("miss"), aa.exists("miss"), as.exists("miss"));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 0 0\nnum 1 1 1\nexists 0 0 0\n");
}

// The 7.8 half: writing through an absent key creates the entry, and the element
// write then falls off the end of the fresh empty container (7.10) rather than
// growing it.
TEST_CASE("a nested write creates the entry and drops the out-of-range element") {
    constexpr std::string_view src = R"(
        module m;
            int all[string][$];
            initial begin
                all["new"][0] = 7;
                $display("num=%0d size=%0d", all.num(), all["new"].size());
                all["new"].push_back(1);
                all["new"][0] = 7;
                $display("num=%0d size=%0d v=%0d", all.num(), all["new"].size(), all["new"][0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "num=1 size=0\nnum=1 size=1 v=7\n");
}

// Three levels, and a mutating method on a nested receiver: `all[a][b].push_back(v)`
// must create BOTH entries (the receiver of a mutating method is a variable, 7.10),
// while the read leaves the map alone.
TEST_CASE("three levels of nesting, and a mutating method through two entries") {
    constexpr std::string_view src = R"(
        module m;
            int all[string][string][$];
            initial begin
                all["a"]["b"].push_back(4);
                $display("v=%0d num=%0d inner=%0d", all["a"]["b"][0], all.num(), all["a"].num());
                $display("absent=%0d num=%0d", all["x"]["y"][0], all.num());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "v=4 num=1 inner=1\nabsent=0 num=1\n");
}

// A `ref` bound THROUGH an entry aliases the element live, and both nesting levels
// are watched: the map's own epoch (a delete frees the entry) and the entry's
// container epoch (a push moves the element). The witnesses are checked
// outermost-first, which is what makes the inner one safe to read at all — it lives
// inside the map's payload. See docs/design/data-model.md §3.1.
TEST_CASE("ref through an associative entry sees writes and detects both levels") {
    CHECK(run_capture(R"(
        module m;
            int all[string][$];
            task automatic bump(ref int x);
                #5 x = x + 100;
            endtask
            initial begin
                all["k"].push_back(7);
                bump(all["k"][0]);
                $display("v=%0d num=%0d", all["k"][0], all.num());
            end
        endmodule
    )") == "v=107 num=1\n");
}

// `%p` of an associative array (LRM 21.2.1.6). The flat runtime container formatter
// addresses an element as `base + i*stride`, which a keyed table cannot answer, so
// this renders through the same frontend IR loop a queue with a non-flat element
// already uses -- keyed by rt_assoc_first / next instead of by an index
// (Lower.cpp's build_assoc_p_string). Ordering is asserted only for integral and
// string keys; LRM 7.8.5 leaves it implementation-defined otherwise
// (docs/design/assoc-keys.md).
TEST_CASE("%p of an associative array renders '{key:value} in key order") {
    SUBCASE("integral key, integral value, as a direct operand") {
        const std::string out = run_capture(R"(
            module t;
              int a[int];
              initial begin
                $display("%p", a);
                a[3] = 30; a[1] = 10; a[2] = 20;
                $display("%p", a);
              end
            endmodule
        )");
        CHECK(out == "'{}\n'{1:10, 2:20, 3:30}\n");
    }
    SUBCASE("string key, string value") {
        const std::string out = run_capture(R"(
            module t;
              string a[string];
              initial begin
                a["b"] = "two"; a["a"] = "one";
                $display("%p", a);
              end
            endmodule
        )");
        CHECK(out == "'{\"a\":\"one\", \"b\":\"two\"}\n");
    }
    SUBCASE("aggregate and container values recurse like any other leaf") {
        const std::string out = run_capture(R"(
            module t;
              typedef struct { int a; string s; } pair_t;
              pair_t p[int];
              int    q[int][$];
              initial begin
                p[1] = '{7, "hi"};
                p[2] = '{8, "yo"};
                q[5].push_back(1);
                q[5].push_back(2);
                $display("%p", p);
                $display("%p", q);
              end
            endmodule
        )");
        CHECK(out == "'{1:'{a:7, s:\"hi\"}, 2:'{a:8, s:\"yo\"}}\n'{5:'{1, 2}}\n");
    }
    SUBCASE("as a leaf of an unpacked struct, and over a wildcard index") {
        const std::string out = run_capture(R"(
            module t;
              typedef struct { int n; int m[int]; } holder_t;
              holder_t h;
              int      w[*];
              initial begin
                h.n = 4;
                h.m[9] = 90;
                w[7] = 70;
                $display("%p", h);
                $display("%p", w);
              end
            endmodule
        )");
        CHECK(out == "'{n:4, m:'{9:90}}\n'{7:70}\n");
    }
}
