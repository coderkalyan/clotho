#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors array_ordering_test.cpp).
// The LRM 7.12.1 locator and 7.12.3 reduction results below are matched against
// verilator 5.050 unless noted; iverilog 13.0 implements none of LRM 7.12.
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

// Confirm lowering fails loudly, mentioning `needle`.
void expect_fail(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

} // namespace

// The six find* methods (LRM 7.12.1), which all require a `with` predicate.
// find/find_index return every match, find_first*/find_last* one or none, and a
// predicate that matches nothing returns an empty queue.
TEST_CASE("array locator find family (LRM 7.12.1)") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int q[$];
            initial begin
                a = new[6];
                a[0]=3; a[1]=7; a[2]=1; a[3]=7; a[4]=9; a[5]=2;
                q = a.find(x) with (x > 3);
                $write("find:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_index with (item > 3);
                $write("find_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_first with (item == 7);
                $write("first:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_first_index with (item == 7);
                $write("first_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_last with (item == 7);
                $write("last:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_last_index with (item == 7);
                $write("last_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find(x) with (x > 100);
                $display("nomatch=%0d", q.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "find: 7 7 9\n"
                              "find_index: 1 3 4\n"
                              "first: 7\n"
                              "first_index: 1\n"
                              "last: 7\n"
                              "last_index: 3\n"
                              "nomatch=0\n");
}

// min/max/unique/unique_index (LRM 7.12.1) with the `with` clause omitted, which
// compares the element itself. min/max return a one-element queue, and a tie
// keeps the earlier element ("only one element is returned"). The order of the
// unique* results is LRM-unspecified; Clotho keeps first-occurrence order, which
// this pins.
TEST_CASE("array locator min/max/unique (LRM 7.12.1)") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int q[$];
            initial begin
                a = new[6];
                a[0]=3; a[1]=7; a[2]=1; a[3]=7; a[4]=9; a[5]=2;
                q = a.min;
                $write("min:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.max;
                $write("max:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.unique;
                $write("unique:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.unique_index;
                $write("unique_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                a.delete();
                $display("empty min=%0d max=%0d", a.min().size(), a.max().size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "min: 1\n"
                              "max: 9\n"
                              "unique: 3 7 1 9 2\n"
                              "unique_index: 0 1 2 4 5\n"
                              "empty min=0 max=0\n");
}

// The five reduction methods (LRM 7.12.3). The result carries the element type's
// width, so a `byte` queue whose elements sum past 255 wraps (7.12.3's own
// example note), and a `with` clause retypes the fold.
TEST_CASE("array reduction methods (LRM 7.12.3)") {
    constexpr std::string_view src = R"(
        module m;
            int  a[];
            byte bq[$];
            initial begin
                a = new[6];
                a[0]=3; a[1]=7; a[2]=1; a[3]=7; a[4]=9; a[5]=2;
                $display("%0d %0d %0d %0d %0d", a.sum, a.product, a.and, a.or, a.xor);
                $display("with=%0d", a.sum with (item + 1));
                bq.push_back(8'd100); bq.push_back(8'd100); bq.push_back(8'd100);
                $display("wrap=%0d widened=%0d", bq.sum, bq.sum with (int'(item)));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "29 2646 0 15 9\n"
                              "with=35\n"
                              "wrap=44 widened=300\n");
}

// An empty array reduces to 0 for every method, including `product` and `and`
// whose fold identities are 1 and all-ones. LRM 7.12.3 does not say; verilator
// also answers 0.
TEST_CASE("array reduction of an empty array is zero") {
    constexpr std::string_view src = R"(
        module m;
            int         q[$];
            logic [7:0] lq[$];
            initial begin
                $display("%0d %0d %0d %0d %0d", q.sum, q.product, q.and, q.or, q.xor);
                $display("%h %h %h", lq.sum, lq.product, lq.and);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 0 0 0 0\n"
                              "00 00 00\n");
}

// A 4-state element folds with the ordinary operator semantics, so x propagates
// per LRM 11.4.x rather than being dropped: the sum is fully unknown while the
// bitwise AND keeps the bits every element pins to 0.
TEST_CASE("array reduction propagates x") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] lq[$];
            initial begin
                lq.push_back(8'h05); lq.push_back(8'hxx);
                lq.push_back(8'h03); lq.push_back(8'hxx);
                $display("%h %h", lq.sum, lq.and);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "xx 0X\n");
}

// min/max over 4-state elements: an unknown-bearing compare is not a definite 1,
// so an unknown element never wins. LRM 7.12.1 does not define the placement of
// unknowns, so this pins Clotho's rule.
TEST_CASE("array min/max over 4-state elements skips unknowns") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] lq[$];
            logic [7:0] r[$];
            initial begin
                lq.push_back(8'h05); lq.push_back(8'hxx);
                lq.push_back(8'h03); lq.push_back(8'hxx);
                r = lq.min;
                $write("min:"); foreach (r[k]) $write(" %h", r[k]); $display;
                r = lq.max;
                $write("max:"); foreach (r[k]) $write(" %h", r[k]); $display;
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "min: 03\n"
                              "max: 05\n");
}

// `item.index` inside a `with` clause (LRM 7.12.4), including under a
// user-chosen iterator name, and the dedup scan of unique() leaving the outer
// index intact. 7.12.1 leaves the order of unique's result open; Clotho returns
// first-occurrence order.
TEST_CASE("iterator index method (LRM 7.12.4)") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int q[$];
            initial begin
                a = new[6];
                a[0]=0; a[1]=5; a[2]=2; a[3]=3; a[4]=1; a[5]=5;
                q = a.find_index with (item == item.index);
                $write("self:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find(v) with (v.index > 3);
                $write("tail:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.unique(v) with (v + v.index);
                $write("uniq:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "self: 0 2 3 5\n"
                              "tail: 1 5\n"
                              "uniq: 0 5 2 1 5\n");
}

// A queue receiver takes the same methods, over the ring's logical order.
TEST_CASE("locator and reduction methods on a queue") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int r[$];
            initial begin
                q.push_back(4); q.push_back(9); q.push_front(2);
                q.push_back(9);
                r = q.find_index with (item == 9);
                $write("idx:"); foreach (r[k]) $write(" %0d", r[k]); $display;
                r = q.max;
                $write("max:"); foreach (r[k]) $write(" %0d", r[k]); $display;
                $display("sum=%0d uniq=%0d", q.sum, q.unique().size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "idx: 2 3\n"
                              "max: 9\n"
                              "sum=24 uniq=3\n");
}

// The methods are element-type agnostic because the clause and the comparison go
// through the ordinary expression path: string, real and packed-struct elements
// all work (verilator mis-prints the struct member select).
TEST_CASE("locator methods over string, real and struct elements") {
    constexpr std::string_view src = R"(
        typedef struct packed { byte red, green, blue; } rgb_t;
        module m;
            string SA[];
            string qs[$];
            real   rq[$];
            real   qr[$];
            rgb_t  cq[$];
            rgb_t  qc[$];
            initial begin
                SA = new[4];
                SA[0]="Bob"; SA[1]="Al"; SA[2]="Bob"; SA[3]="Zed";
                qs = SA.find_first with (item == "Bob");
                $write("first:"); foreach (qs[k]) $write(" %s", qs[k]); $display;
                qs = SA.unique;
                $write("uniq:"); foreach (qs[k]) $write(" %s", qs[k]); $display;
                qs = SA.max;
                $write("max:"); foreach (qs[k]) $write(" %s", qs[k]); $display;

                rq.push_back(2.5); rq.push_back(-1.5); rq.push_back(9.25);
                qr = rq.min;
                $write("rmin:"); foreach (qr[k]) $write(" %0.2f", qr[k]); $display;
                qr = rq.max;
                $write("rmax:"); foreach (qr[k]) $write(" %0.2f", qr[k]); $display;

                cq.push_back('{red:8'd1, green:8'd9, blue:8'd3});
                cq.push_back('{red:8'd7, green:8'd2, blue:8'd4});
                qc = cq.find(c) with (c.red > 3);
                $write("green:"); foreach (qc[k]) $write(" %0d", qc[k].green); $display;
                $display("reds=%0d", cq.sum with (int'(item.red)));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "first: Bob\n"
                              "uniq: Bob Al Zed\n"
                              "max: Zed\n"
                              "rmin: -1.50\n"
                              "rmax: 9.25\n"
                              "green: 2\n"
                              "reds=8\n");
}

// Inside a function the loop's hidden cells and the result queue are frame
// locals, so a locator works there and each activation keeps its own.
TEST_CASE("locator method inside a function") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            function automatic int biggest(int d[]);
                int mq[$];
                mq = d.max;
                return mq.size() == 0 ? -1 : mq[0];
            endfunction
            initial begin
                a = new[4];
                a[0]=6; a[1]=12; a[2]=1; a[3]=4;
                $display("%0d", biggest(a));
                a.delete();
                $display("%0d", biggest(a));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "12\n"
                              "-1\n");
}

// A result queue feeds range selects and further methods without a temporary,
// per LRM 7.12's "range selects and properties shall follow the with clause".
TEST_CASE("locator result chains into another method") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin
                a = new[5];
                foreach (a[k]) a[k] = k * 3;
                $display("%0d %0d", a.find(x) with (x > 3).size(), a.unique().sum());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 30\n");
}

// A fixed-size unpacked-array receiver (LRM 7.12: "any unpacked array"). Both
// families walk it in place; the only thing that differs from a queue is that
// the indices it REPORTS are its declared range, which the cases below pin on a
// non-zero-based and a descending one. This is the UVM shape's family:
// uvm_printer.svh:1498 assigns `q.max` of an `int q[5]` into a queue and reads
// element 0.
TEST_CASE("locator and reduction methods on a fixed-size unpacked array (LRM 7.12)") {
    constexpr std::string_view src = R"(
        module m;
            int a[5];
            int q[$];
            int m;
            initial begin
                a[0]=3; a[1]=7; a[2]=1; a[3]=7; a[4]=9;
                $display("%0d %0d %0d %0d %0d", a.sum, a.product, a.and, a.or, a.xor);
                q = a.min; $display("min=%0d", q[0]);
                q = a.max; $display("max=%0d", q[0]);
                q = a.find(x) with (x > 3);
                $write("find:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_index with (item > 3);
                $write("find_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_first with (item > 6);
                $write("first:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find_last_index with (item == 7);
                $write("last_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.unique;
                $write("unique:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.unique_index;
                $write("unique_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = a.find(x) with (x > 100);
                $display("nomatch=%0d", q.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "27 1323 1 15 11\n"
                              "min=1\n"
                              "max=9\n"
                              "find: 7 7 9\n"
                              "find_index: 1 3 4\n"
                              "first: 7\n"
                              "last_index: 3\n"
                              "unique: 3 7 1 9\n"
                              "unique_index: 0 1 2 4\n"
                              "nomatch=0\n");
}

// The crux of a fixed receiver: an index result is a DECLARED index, not a
// 0-based offset. 7.12.1 defines the first element as the one "closest to the
// leftmost indexed element", so `int b[3:7]` starts at 3 and the DESCENDING
// `int c[7:3]` starts at 7 and walks down to 3 -- the left bound either way. A
// 0-based implementation passes nothing here.
TEST_CASE("index results on a fixed array are declared indices (LRM 7.12.1)") {
    constexpr std::string_view src = R"(
        module m;
            int b[3:7];
            int c[7:3];
            int q[$];
            initial begin
                b[3]=10; b[4]=40; b[5]=20; b[6]=40; b[7]=30;
                $display("b.sum=%0d", b.sum);
                q = b.find_index with (item == 40);
                $write("b.find_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = b.find_first_index with (item > 15);
                $write("b.first_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = b.find_last_index with (item > 15);
                $write("b.last_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = b.unique_index;
                $write("b.unique_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;

                c[7]=10; c[6]=40; c[5]=20; c[4]=40; c[3]=30;
                $display("c.sum=%0d", c.sum);
                q = c.find_index with (item == 40);
                $write("c.find_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = c.find_first_index with (item > 15);
                $write("c.first_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = c.find_last_index with (item > 15);
                $write("c.last_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = c.unique_index;
                $write("c.unique_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "b.sum=140\n"
                              "b.find_index: 4 6\n"
                              "b.first_index: 4\n"
                              "b.last_index: 7\n"
                              "b.unique_index: 3 4 5 7\n"
                              "c.sum=140\n"
                              "c.find_index: 6 4\n"
                              "c.first_index: 6\n"
                              "c.last_index: 3\n"
                              "c.unique_index: 7 6 5 3\n");
}

// `item.index` (LRM 7.12.4) inside the clause is the declared index too, for
// either range direction, and it resolves through the same binding stack the
// queue receiver uses.
TEST_CASE("item.index on a fixed array is the declared index (LRM 7.12.4)") {
    constexpr std::string_view src = R"(
        module m;
            int b[3:7];
            int c[7:3];
            int q[$];
            initial begin
                foreach (b[k]) b[k] = k * 10;
                foreach (c[k]) c[k] = k * 10;
                q = b.find_index with (item.index == 5);
                $write("b.at5:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = b.find with (item.index == 5);
                $write("b.val5:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                q = c.find_index with (item.index == 5);
                $write("c.at5:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                // Every element equals ten times its own declared index, so the
                // clause holds for all five whichever way the range runs.
                q = b.find_index with (item == item.index * 10);
                $display("b.all=%0d", q.size());
                q = c.find_index with (item == item.index * 10);
                $display("c.all=%0d", q.size());
                $display("b.idxsum=%0d c.idxsum=%0d",
                         b.sum with (item.index), c.sum with (item.index));
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "b.at5: 5\n"
                              "b.val5: 50\n"
                              "c.at5: 5\n"
                              "b.all=5\n"
                              "c.all=5\n"
                              "b.idxsum=25 c.idxsum=25\n");
}

// A multidimensional fixed array. 7.12.3's own worked example is the nested
// form -- `m.sum with (item.sum with (item))` -- because the ELEMENTS of `m` are
// its sub-arrays, not its leaves: the outer clause reduces one sub-array per
// iteration and the inner one reduces that sub-array's leaves. A bare `m.sum` is
// therefore not the flattened sum and is refused (see the loud-refusal case).
// Index results name the OUTER dimension, in declared coordinates.
TEST_CASE("multidimensional fixed array reduces through a nested clause (LRM 7.12.3)") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] md [2][2] = '{ '{5, 10}, '{15, 20} };
            int n [2:3][3];
            int y;
            int q[$];
            int mq[$][3];
            initial begin
                y = md.sum with (item.sum with (item));
                $display("nested=%0d", y);
                y = md.sum with (item.sum with (item.index));
                $display("inner_index=%0d", y);
                n[2][0]=1; n[2][1]=2; n[2][2]=3;
                n[3][0]=40; n[3][1]=50; n[3][2]=60;
                q = n.find_index with (item.sum > 10);
                $write("outer_index:"); foreach (q[k]) $write(" %0d", q[k]); $display;
                mq = n.max with (item.sum);
                $display("max=%0d %0d %0d", mq[0][0], mq[0][1], mq[0][2]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "nested=50\n"
                              "inner_index=2\n"
                              "outer_index: 3\n"
                              "max=40 50 60\n");
}

// What 7.12 itself does not allow, refused loudly with the clause's own words.
// slang's binder gets to most of these first, which is the right place for them
// -- they are type rules, not lowering gaps -- so these pin that the pair of
// clotho and slang cannot regress into a silent accept.
TEST_CASE("array manipulation methods on unsupported receivers fail loudly") {
    // An associative array is still clotho's own gap.
    expect_fail("module m; int a[int]; int q[$];\n"
                " initial q = a.find(x) with (x > 1); endmodule",
                "associative array method 'find'");
    // 7.12.3 applies "to any unpacked array of integral values". A
    // multidimensional array's elements are its sub-arrays, so a bare reduction
    // has no defined operation for them; so do a string and a real element.
    expect_fail("module m; int a[2][3];\n initial $display(a.sum); endmodule", "unpacked arrays of integral values");
    expect_fail("module m; string s[4];\n initial $display(s.sum); endmodule", "unpacked arrays of integral values");
    expect_fail("module m; real r[4];\n initial $display(r.sum); endmodule", "unpacked arrays of integral values");
    // 7.12.1's min/max/unique need the relational operators defined for what is
    // compared, which a sub-array does not have either.
    expect_fail("module m; int a[2][3]; int q[$];\n initial q = a.max; endmodule",
                "unpacked arrays of comparable values");
    // 7.12.4's index method takes a dimension; only the first is supported.
    expect_fail("module m; int a[5]; int q[$];\n"
                " initial q = a.find_index with (item.index(2) == 1); endmodule",
                "only supported for dimension 1");
}
