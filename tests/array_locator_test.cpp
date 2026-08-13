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

// LRM 7.12 allows these methods on any unpacked array, and on an associative
// array. Both are refused loudly rather than silently mislowered.
TEST_CASE("array manipulation methods on unsupported receivers fail loudly") {
    expect_fail("module m; int a[5]; int q[$];\n"
                " initial q = a.find(x) with (x > 1); endmodule",
                "fixed-size unpacked array");
    expect_fail("module m; int a[int]; int q[$];\n"
                " initial q = a.find(x) with (x > 1); endmodule",
                "associative array method 'find'");
}
