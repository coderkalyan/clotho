#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors queue_test.cpp / dynamic_array_test.cpp).
// Ordering results (LRM 7.12.2 reverse / sort / rsort) are hand-checkable and,
// where iverilog supports the method, oracle-matched.
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

// sort() ascending, rsort() descending, reverse() flips order — dynamic array,
// signed `int` element (LRM 7.12.2). reverse of the rsorted vector returns the
// ascending order.
TEST_CASE("dynamic array sort/rsort/reverse (int)") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin
                a = new[5];
                a[0]=3; a[1]=1; a[2]=4; a[3]=1; a[4]=5;
                a.sort();
                $write("sort:");  foreach (a[i]) $write(" %0d", a[i]); $display;
                a.rsort();
                $write("rsort:"); foreach (a[i]) $write(" %0d", a[i]); $display;
                a.reverse();
                $write("rev:");   foreach (a[i]) $write(" %0d", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sort: 1 1 3 4 5\nrsort: 5 4 3 1 1\nrev: 1 1 3 4 5\n");
}

// A signed element orders arithmetically: negatives sort below zero (the signed
// comparator, not a raw bit-pattern compare).
TEST_CASE("dynamic array sort is signed for a signed element") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin
                a = new[4];
                a[0]=-2; a[1]=5; a[2]=-10; a[3]=0;
                a.sort();
                foreach (a[i]) $write("%0d ", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-10 -2 0 5 \n");
}

// An unsigned element orders by magnitude: 8'hFF is the largest, not negative.
TEST_CASE("dynamic array sort is unsigned for an unsigned element") {
    constexpr std::string_view src = R"(
        module m;
            bit [7:0] a[];
            initial begin
                a = new[3];
                a[0]=8'hff; a[1]=8'h01; a[2]=8'h80;
                a.sort();
                foreach (a[i]) $write("%02x ", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "01 80 ff \n");
}

// Real elements order by IEEE value (the real comparator).
TEST_CASE("dynamic array sort (real)") {
    constexpr std::string_view src = R"(
        module m;
            real a[];
            initial begin
                a = new[4];
                a[0]=2.5; a[1]=-1.5; a[2]=0.0; a[3]=10.25;
                a.sort();
                foreach (a[i]) $write("%g ", a[i]); $display;
                a.rsort();
                foreach (a[i]) $write("%g ", a[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "-1.5 0 2.5 10.25 \n10.25 2.5 0 -1.5 \n");
}

// Queue ordering: reverse / sort / rsort operate on the ring's logical order.
TEST_CASE("queue sort/rsort/reverse (int)") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            initial begin
                q.push_back(3); q.push_back(1); q.push_back(4);
                q.push_back(1); q.push_back(5);
                q.sort();
                $write("sort:");  foreach (q[i]) $write(" %0d", q[i]); $display;
                q.rsort();
                $write("rsort:"); foreach (q[i]) $write(" %0d", q[i]); $display;
                q.reverse();
                $write("rev:");   foreach (q[i]) $write(" %0d", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sort: 1 1 3 4 5\nrsort: 5 4 3 1 1\nrev: 1 1 3 4 5\n");
}

// A queue whose head has wrapped (push_front then push_back) still sorts by
// logical order — the ring's head offset is honoured.
TEST_CASE("queue sort after push_front (wrapped head)") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            initial begin
                q.push_back(2); q.push_front(7); q.push_back(1); q.push_front(9);
                q.sort();
                foreach (q[i]) $write("%0d ", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 7 9 \n");
}

// shuffle() produces a permutation (LRM 7.12.2): sorting the shuffled array
// recovers the original, and the specific order is the process RNG's stable
// stream (seed = process index, like $urandom). Both the exact
// permutation and the sort-recovers invariant are pinned.
TEST_CASE("dynamic array + queue shuffle is a stable permutation") {
    constexpr std::string_view src = R"(
        module m;
            int a[]; int q[$]; int i;
            initial begin
                a = new[6]; foreach (a[i]) a[i] = i;
                a.shuffle();
                $write("sh:"); foreach (a[i]) $write(" %0d", a[i]); $display;
                a.sort();
                $write("re:"); foreach (a[i]) $write(" %0d", a[i]); $display;
                for (i = 0; i < 5; i++) q.push_back(i);
                q.shuffle();
                $write("q:"); foreach (q[i]) $write(" %0d", q[i]); $display;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sh: 2 3 1 4 0 5\nre: 0 1 2 3 4 5\nq: 0 4 1 2 3\n");
}

// shuffle draws from the frame RNG shared with $urandom: the process becomes a
// coroutine and the RNG state persists across activations (each posedge draws a
// fresh permutation) while the multiset is preserved (sum invariant).
TEST_CASE("shuffle in an always block keeps a per-process RNG") {
    constexpr std::string_view src = R"(
        module m;
            reg clk = 0; int a[]; int i, s;
            initial begin
                a = new[5]; foreach (a[i]) a[i] = i + 1;
                repeat (4) #5 clk = ~clk;
                $finish;
            end
            always @(posedge clk) begin
                a.shuffle();
                s = 0; foreach (a[i]) s += a[i];
                $display("sum=%0d a0=%0d", s, a[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sum=15 a0=5\nsum=15 a0=3\n");
}

// Ordering an empty or single-element container is a no-op (no crash).
TEST_CASE("array ordering on empty / single element is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int q[$];
            initial begin
                a.sort(); a.rsort(); a.reverse();
                q.sort(); q.rsort(); q.reverse();
                a = new[1]; a[0]=42;
                a.sort(); a.reverse();
                q.push_back(7); q.rsort();
                $display("%0d %0d", a[0], q[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "42 7\n");
}
