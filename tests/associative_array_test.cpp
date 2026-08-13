#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

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
