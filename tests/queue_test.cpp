#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
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

TEST_CASE("queue struct member: push/size/index and whole-struct deep copy") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int q[$]; int x; } sq_t;
            sq_t a, b;
            initial begin
                a.x = 5; a.q.push_back(42); a.q.push_back(7);
                b = a;
                a.q.push_back(99); a.x = 1;
                $display("a: x=%0d n=%0d %0d %0d %0d", a.x, a.q.size(), a.q[0], a.q[1], a.q[2]);
                $display("b: x=%0d n=%0d %0d %0d", b.x, b.q.size(), b.q[0], b.q[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a: x=1 n=3 42 7 99\nb: x=5 n=2 42 7\n");
}

TEST_CASE("dynamic-array struct member: new/write and whole-struct deep copy") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int d[]; int x; } sd_t;
            sd_t a, b;
            initial begin
                a.x = 3; a.d = new[2]; a.d[0] = 7; a.d[1] = 8;
                b = a;
                a.d[0] = 100;
                $display("a: %0d %0d %0d %0d", a.x, a.d.size(), a.d[0], a.d[1]);
                $display("b: %0d %0d %0d %0d", b.x, b.d.size(), b.d[0], b.d[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a: 3 2 100 8\nb: 3 2 7 8\n");
}

TEST_CASE("queue class property") {
    constexpr std::string_view src = R"(
        module m;
            class C; int q[$]; int x; endclass
            C c;
            initial begin
                c = new; c.x = 1; c.q.push_back(2); c.q.push_back(3);
                $display("%0d %0d %0d %0d", c.x, c.q.size(), c.q[0], c.q.pop_back());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 2 3\n");
}

TEST_CASE("fixed array of queues: per-cell independence and whole-array copy") {
    constexpr std::string_view src = R"(
        module m;
            int q[3][$];
            int a[2][$], b[2][$];
            initial begin
                q[0].push_back(10); q[2].push_back(30); q[2].push_back(31);
                $display("%0d %0d %0d %0d %0d", q[0].size(), q[1].size(), q[2].size(),
                         q[0][0], q[2][1]);
                a[0].push_back(1); a[1].push_back(2); a[1].push_back(3);
                b = a;
                a[1].push_back(4);
                $display("%0d %0d %0d %0d", b[0].size(), b[1].size(), b[1][1], a[1].size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 0 2 10 31\n1 2 3 3\n");
}

TEST_CASE("nested struct with queue and string leaves deep-copies per leaf") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int q[$]; string s; } in_t;
            typedef struct { in_t inner; int y; } out_t;
            out_t a, b;
            initial begin
                a.y = 9; a.inner.s = "hi"; a.inner.q.push_back(5);
                b = a;
                a.inner.q.push_back(6); a.inner.s = "bye";
                $display("a: %0d %s %0d", a.y, a.inner.s, a.inner.q.size());
                $display("b: %0d %s %0d %0d", b.y, b.inner.s, b.inner.q.size(), b.inner.q[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a: 9 bye 2\nb: 9 hi 1 5\n");
}

TEST_CASE("frame-local struct with queue and string members: per-call lifecycle") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int q[$]; string s; } t;
            function automatic int f(int i);
                t v;
                v.q.push_back(i); v.s = "x";
                return v.q[0] + v.q.size();
            endfunction
            initial begin
                int acc;
                for (int i = 0; i < 100; i++) acc += f(i);
                $display("acc=%0d", acc);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "acc=5050\n");
}

TEST_CASE("NBA to a scalar member coexists with a queue member") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int q[$]; logic [7:0] v; } t;
            t s;
            logic clk = 0;
            always @(posedge clk) s.v <= 8'hAB;
            initial begin
                s.q.push_back(3);
                #1 clk = 1;
                #1 $display("%h %0d", s.v, s.q[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ab 3\n");
}

TEST_CASE("foreach over a struct-member queue") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int q[$]; } t;
            t s;
            initial begin
                int acc;
                s.q.push_back(1); s.q.push_back(2); s.q.push_back(3);
                foreach (s.q[i]) acc += s.q[i];
                $display("acc=%0d", acc);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "acc=6\n");
}

TEST_CASE("string queue: push/pop/index/element methods/OOB default") {
    constexpr std::string_view src = R"(
        module m;
            string q[$];
            initial begin
                q.push_back("hello");
                q.push_back("world");
                q.push_front("first");
                $display("%0d %s %s %s", q.size(), q[0], q[1], q[2]);
                $display("pop=%s pop=%s n=%0d", q.pop_front(), q.pop_back(), q.size());
                q[0] = "changed";
                $display("%s len=%0d oob=[%s]", q[0], q[0].len(), q[5]);
                q.delete(0);
                $display("after del n=%0d", q.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "3 first hello world\npop=first pop=world n=1\nchanged len=7 oob=[]\n"
          "after del n=0\n");
}

TEST_CASE("string queue deep copy is per-element") {
    constexpr std::string_view src = R"(
        module m;
            string q[$], q2[$];
            initial begin
                q.push_back("keep");
                q2 = q;
                q[0] = "zzz";
                $display("copy=%s orig=%s", q2[0], q[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "copy=keep orig=zzz\n");
}

TEST_CASE("string dynamic array: new/element write/concat from element") {
    constexpr std::string_view src = R"(
        module m;
            string d[];
            initial begin
                d = new[2];
                d[0] = "dyn0"; d[1] = {d[0], "+"};
                $display("%s %s %0d", d[0], d[1], d.size());
                d = new[1](d);
                $display("%s %0d", d[0], d.size());
                d.delete();
                $display("%0d", d.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "dyn0 dyn0+ 2\ndyn0 1\n0\n");
}

TEST_CASE("struct member queue of strings deep-copies; frame-local lifecycle") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { string q[$]; int x; } t;
            t a, b;
            function automatic string f(string base);
                string lq[$];
                lq.push_back({base, "!"});
                return lq.pop_front();
            endfunction
            initial begin
                a.x = 1; a.q.push_back("aa"); a.q.push_back("bb");
                b = a;
                a.q[0] = "mut";
                $display("%0d %s %s | %s", b.x, b.q[0], b.q[1], a.q[0]);
                $display("%s", f("hi"));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 aa bb | mut\nhi!\n");
}

// Assignment-pattern source `q = '{...}` builds the queue element-wise (LRM
// 7.10). Verified against verilator (iverilog crashes on queues).
TEST_CASE("queue assignment from an assignment pattern") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            initial begin
                q = '{10, 20, 30, 40};
                $display("s=%0d %0d %0d %0d %0d", q.size(), q[0], q[1], q[2], q[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "s=4 10 20 30 40\n");
}

// Concatenation source `q = {a, e, b}` splices whole-queue operands and single
// elements; a self-referential `q = {q, e}` is alias-safe (temp materialized
// before the deep copy back). Verified against verilator.
TEST_CASE("queue assignment from a concatenation") {
    constexpr std::string_view src = R"(
        module m;
            int q[$], r[$];
            initial begin
                q = '{1, 2, 3};
                r = {5, q, 6, q};
                $display("r s=%0d %0d %0d %0d %0d %0d %0d %0d %0d",
                         r.size(), r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
                q = {q, 4};
                $display("self s=%0d last=%0d", q.size(), q[$]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "r s=8 5 1 2 3 6 1 2 3\n"
          "self s=4 last=4\n");
}

// A queue slice `q[a:b]` (LRM 7.10.4) yields a new queue. Bounds clamp to
// [0, size-1]; a>b is empty. A self-slice `q = q[a:b]` is alias-safe. Verified
// against verilator.
TEST_CASE("queue slice") {
    constexpr std::string_view src = R"(
        module m;
            int q[$], s[$];
            initial begin
                q = '{10, 20, 30, 40, 50};
                s = q[1:3];
                $display("mid s=%0d %0d %0d %0d", s.size(), s[0], s[1], s[2]);
                s = q[-2:1];
                $display("loclamp s=%0d %0d %0d", s.size(), s[0], s[1]);
                s = q[3:100];
                $display("hiclamp s=%0d %0d %0d", s.size(), s[0], s[1]);
                s = q[3:1];
                $display("empty s=%0d", s.size());
                q = q[1:3];
                $display("self s=%0d %0d %0d %0d", q.size(), q[0], q[1], q[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "mid s=3 20 30 40\n"
          "loclamp s=2 10 20\n"
          "hiclamp s=2 40 50\n"
          "empty s=0\n"
          "self s=3 20 30 40\n");
}

// A bounded queue built from a too-long concatenation keeps the first N+1
// elements (LRM 7.10.4). No verilator oracle (it miscompiles bounded-queue
// concat); hand-checked against the LRM.
TEST_CASE("queue slice/concat into a bounded queue drops the tail") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            int b3[$:3];
            initial begin
                q = '{1, 2, 3, 4, 5};
                b3 = {q, q};
                $display("s=%0d %0d %0d %0d %0d", b3.size(), b3[0], b3[1], b3[2], b3[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "s=4 1 2 3 4\n");
}

TEST_CASE("%p of a queue") {
    constexpr std::string_view src = R"(
        module m;
            int q[$];
            string sq[$];
            logic [7:0] lq[$];
            int e[$];
            typedef struct { int a; int q[$]; } t;
            t s;
            initial begin
                q = '{10, 20, 30};
                $display("%p", q);
                sq = '{"ab", "cd"};
                $display("%p", sq);
                lq.push_back(8'hA); lq.push_back(8'hz5); lq.push_back(8'hFF);
                $display("%p", lq);
                $display("%p", e);
                s.a = 1; s.q.push_back(4); s.q.push_back(5);
                $display("%p", s);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "'{10, 20, 30}\n"
          "'{\"ab\", \"cd\"}\n"
          "'{10, Z, 255}\n"
          "'{}\n"
          "'{a:1, q:'{4, 5}}\n");
}

// Whole-queue == / != (LRM 11.2.2, 11.4.5): different sizes are a definite 0;
// same size folds a per-element compare with x-propagation for 4-state elements
// (any definite mismatch -> 0, else x if any unknown, else 1). Verified against
// verilator on 2-state/string; the 4-state x cases hand-derive to 11.4.5 (same
// rule as a packed vector).
TEST_CASE("queue equality with x-propagation") {
    constexpr std::string_view src = R"(
        module m;
            int a[$], b[$];
            logic [7:0] la[$], lb[$];
            string sa[$], sb[$];
            initial begin
                a = '{1,2,3}; b = '{1,2,3};
                $display("%b%b", (a==b), (a!=b));
                b = '{1,2,4}; $display("%b", (a==b));
                b = '{1,2};   $display("%b", (a==b));
                la = '{8'h1, 8'h2}; lb = '{8'h1, 8'hxx};
                $display("%b", (la==lb));
                la = '{8'h1, 8'h9}; lb = '{8'h1, 8'hx3};
                $display("%b", (la==lb));
                sa = '{"hi","yo"}; sb = '{"hi","yo"}; $display("%b", (sa==sb));
                sb = '{"hi","no"}; $display("%b", (sa==sb));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10\n0\n0\nx\n0\n1\n0\n");
}

// == of an unpacked struct with a queue member walks the member element-wise
// (the AggEq leaf dispatch reaches rt_container_eq). Verified against verilator.
TEST_CASE("struct-with-queue-member equality") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int x; int q[$]; } t;
            t s1, s2;
            initial begin
                s1.x = 1; s1.q = '{5,6};
                s2.x = 1; s2.q = '{5,6};
                $display("%b", (s1==s2));
                s2.q = '{5,7};
                $display("%b", (s1==s2));
                s2.q = '{5,6}; s2.x = 2;
                $display("%b", (s1==s2));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n0\n0\n");
}

// Whole-queue nonblocking assignment `q <= rhs` (LRM 10.4.2): the RHS is
// deep-copied in the Active region and moved into the target in the NBA region,
// so the update is not visible until after the current region. A later blocking
// write is overwritten by the NBA commit (last write wins). Verified against
// verilator on 2-state; string-element move exercises the managed-deinit path.
TEST_CASE("nonblocking whole-queue assignment") {
    constexpr std::string_view src = R"(
        module m;
            int q[$], r[$];
            initial begin
                q = '{1, 2, 3};
                r <= q;
                $display("imm=%0d", r.size());
                #1;
                $display("post=%0d %0d %0d %0d", r.size(), r[0], r[1], r[2]);
                q = '{9, 8};
                q <= '{5, 5, 5};
                q.push_back(7);
                $display("qimm=%0d", q.size());
                #1;
                $display("qpost=%0d %0d", q.size(), q[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "imm=0\n"
          "post=3 1 2 3\n"
          "qimm=3\n"
          "qpost=3 5\n");
}

// A clocked queue register `q <= d` reads the sampled d each posedge (LRM
// 10.4.2). Verified byte-identical to verilator; string elements move-commit
// cleanly (the NBA move frees the target's prior strings).
TEST_CASE("nonblocking whole-queue register and managed elements") {
    constexpr std::string_view src = R"(
        module m;
            logic clk = 0;
            int q[$], d[$];
            string s[$], t[$];
            always #5 clk = ~clk;
            initial begin
                d = '{10};
                @(posedge clk); #1;
                $display("c1 %0d %0d", q.size(), q[0]);
                d = '{20, 30};
                @(posedge clk); #1;
                $display("c2 %0d %0d %0d", q.size(), q[0], q[1]);
                t = '{"x", "y", "z"};
                s = '{"aa", "bb"};
                t <= s;
                #1;
                $display("t %0d %s %s", t.size(), t[0], t[1]);
                $finish;
            end
            always @(posedge clk) q <= d;
        endmodule
    )";
    CHECK(run_capture(src) ==
          "c1 1 10\n"
          "c2 2 20 30\n"
          "t 2 aa bb\n");
}
