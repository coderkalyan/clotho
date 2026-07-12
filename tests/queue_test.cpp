#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors dynamic_array_test.cpp).
// Expectations are cross-checked against iverilog (the LRM 7.10 oracle) except
// where noted: iverilog's 2-state queue support crashes and its `int` queue is
// internally 4-state, so empty/out-of-range defaults use a 4-state (`logic`)
// element for the oracle and a separate 2-state check for the LRM default.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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

// push_back / push_front append at the tail / head; size() counts; q[i] reads
// the element at logical position i; q[$] is the last element (LRM 7.10.2).
TEST_CASE("queue push/size/index") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            initial begin
                q.push_back(10);
                q.push_back(20);
                q.push_front(5);
                $display("size=%0d q0=%0d q1=%0d q2=%0d", q.size(), q[0], q[1], q[2]);
                $display("last=%0d", q[$]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "size=3 q0=5 q1=10 q2=20\nlast=20\n");
}

// pop_front / pop_back remove and return the head / tail; insert places before
// a position; delete(i) removes one element; delete() clears (LRM 7.10.2).
TEST_CASE("queue pop/insert/delete") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int a, b;
            initial begin
                q.push_back(1); q.push_back(2); q.push_back(3);
                a = q.pop_front();
                b = q.pop_back();
                $display("a=%0d b=%0d size=%0d mid=%0d", a, b, q.size(), q[0]);
                q.insert(0, 100);
                q.insert(q.size(), 200);
                $display("s=%0d q0=%0d q1=%0d q2=%0d", q.size(), q[0], q[1], q[2]);
                q.delete(1);
                $display("after del s=%0d q0=%0d q1=%0d", q.size(), q[0], q[1]);
                q.delete();
                $display("after clear s=%0d", q.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "a=1 b=3 size=1 mid=2\n"
          "s=3 q0=100 q1=2 q2=200\n"
          "after del s=2 q0=100 q1=200\n"
          "after clear s=0\n");
}

// foreach iterates 0..size-1 (LRM 12.7.3).
TEST_CASE("queue foreach") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int i, sum;
            initial begin
                q.push_back(4); q.push_back(5); q.push_back(6);
                sum = 0;
                foreach (q[i]) sum = sum + q[i];
                $display("sum=%0d q0=%0d q2=%0d", sum, q[0], q[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "sum=15 q0=4 q2=6\n");
}

// Assignment is a deep copy: mutating the copy leaves the source untouched
// (value semantics, LRM 7.10).
TEST_CASE("queue assignment is a deep copy") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int r[$];
            initial begin
                q.push_back(4); q.push_back(5); q.push_back(6);
                r = q;
                r[0] = 99;
                r.push_back(7);
                $display("q0=%0d r0=%0d qs=%0d rs=%0d", q[0], r[0], q.size(), r.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "q0=4 r0=99 qs=3 rs=4\n");
}

// q[i] element write updates in place; an out-of-range write is a no-op and an
// out-of-range read yields the element default (LRM 7.10). A 2-state element's
// default is 0 (iverilog's 2-state queue support crashes here, so this is the
// LRM value, not an oracle match; the 4-state x default is oracle-checked below).
TEST_CASE("queue element write and out-of-range default (2-state)") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int r;
            initial begin
                q.push_back(1); q.push_back(2);
                q[1] = 20;
                q[5] = 123;      // out of range: no-op
                r = q[5];        // out of range: default 0
                $display("q0=%0d q1=%0d oob=%0d size=%0d", q[0], q[1], r, q.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "q0=1 q1=20 oob=0 size=2\n");
}

// A 4-state (logic) element powers on / defaults to x: an out-of-range read and
// a pop from an empty queue both yield x (oracle-matched vs iverilog).
TEST_CASE("queue 4-state defaults to x") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] q[$];
            logic [7:0] e;
            initial begin
                $display("oob=%h", q[0]);
                e = q.pop_back();
                $display("popempty=%h", e);
                q.push_back(8'hA5);
                $display("q0=%h oob=%h", q[0], q[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "oob=xx\npopempty=xx\nq0=a5 oob=xx\n");
}

// A pop from an empty 2-state queue returns the element-type default 0 (LRM
// 7.10.2). iverilog returns x here because its `int` queue is internally
// 4-state; Clotho is LRM-correct.
TEST_CASE("queue pop from empty 2-state queue is default 0") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int a;
            initial begin
                a = q.pop_front();
                $display("popempty=%0d size=%0d", a, q.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "popempty=0 size=0\n");
}

// A bounded queue `q[$:N]` holds at most N+1 elements (LRM 7.10.4). A push onto
// a full queue keeps the first N+1 elements: push_back drops the just-pushed
// element, push_front drops the old tail. Oracle-matched vs iverilog.
TEST_CASE("queue bounded push drops excess") {
    constexpr std::string_view src = R"(
        module m;
            int q[$:2];
            int i;
            initial begin
                q.push_back(1); q.push_back(2); q.push_back(3); q.push_back(4);
                $write("pb s=%0d:", q.size());
                foreach (q[i]) $write(" %0d", q[i]);
                $display("");
                q.delete();
                q.push_front(1); q.push_front(2); q.push_front(3); q.push_front(4);
                $write("pf s=%0d:", q.size());
                foreach (q[i]) $write(" %0d", q[i]);
                $display("");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "pb s=3: 1 2 3\npf s=3: 4 3 2\n");
}

// Building a queue in a loop exercises the ring buffer's growth and the
// scratch-alloca hoisting (the scratch is allocated once, not per iteration).
TEST_CASE("queue built in a loop") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int i, sum;
            initial begin
                for (i = 0; i < 100; i = i + 1) q.push_back(i * 2);
                sum = 0;
                foreach (q[i]) sum = sum + q[i];
                $display("size=%0d first=%0d last=%0d sum=%0d",
                         q.size(), q[0], q[99], sum);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "size=100 first=0 last=198 sum=9900\n");
}

TEST_CASE("queue input argument is copy-in (value semantics)") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int qsum(int a[$]);
                int t = 0;
                foreach (a[i]) t += a[i];
                a.push_back(999);         // mutate the copy
                return t;
            endfunction
            int q[$];
            int s;
            initial begin
                q.push_back(10); q.push_back(20); q.push_back(30);
                s = qsum(q);
                $display("%0d %0d", s, q.size());   // caller unchanged: 60 3
            end
        endmodule
    )";
    CHECK(run_capture(src) == "60 3\n");
}

TEST_CASE("queue output and inout arguments") {
    constexpr std::string_view src = R"(
        module m;
            task automatic fill(output int a[$], input int n);
                for (int i = 0; i < n; i++) a.push_back((i + 1) * 10);
            endtask
            function automatic int bump(inout int a[$]);
                foreach (a[i]) a[i] = a[i] + 1;
                return a.size();
            endfunction
            int q[$];
            int d;
            initial begin
                fill(q, 3);
                $display("%0d %0d %0d %0d", q.size(), q[0], q[1], q[2]);   // 3 10 20 30
                d = bump(q);
                $display("%0d %0d %0d %0d", d, q[0], q[1], q[2]);          // 3 11 21 31
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 10 20 30\n3 11 21 31\n");
}

TEST_CASE("queue function return value") {
    constexpr std::string_view src = R"(
        module m;
            typedef int iq[$];
            function automatic iq mk(int n);
                iq t;
                for (int i = 0; i < n; i++) t.push_back(i * i);
                return t;
            endfunction
            int q[$];
            initial begin
                q = mk(4);
                $display("%0d %0d %0d %0d %0d", q.size(), q[0], q[1], q[2], q[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4 0 1 4 9\n");
}

// A queue as a function-frame local (per-call handle slot, freed at exit).
TEST_CASE("queue as a function local") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int f(int n);
                int t[$];
                for (int i = 0; i < n; i++) t.push_back(i + 1);
                return t.size() + t[0] + t[n - 1];
            endfunction
            initial $display("%0d %0d", f(4), f(2));
        endmodule
    )";
    CHECK(run_capture(src) == "9 5\n");
}

// $size(q) on a runtime queue lowers to .size() (LRM 20.7).
TEST_CASE("queue $size") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            initial begin
                q.push_back(1); q.push_back(2); q.push_back(3);
                $display("%0d", $size(q));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3\n");
}
