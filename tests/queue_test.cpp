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

// A design the frontend must refuse, and the guard text it must name.
void expect_fail(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CAPTURE(r.diagnostic_text());
    CHECK(r.diagnostic_text().find(needle) != std::string::npos);
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

// Queue subroutine ABI (LRM 13.5, 7.10): input copy-in (value semantics),
// output/inout copy-out, and function return. iverilog cannot pass queue
// arguments. The value-semantics rationale is shared with
// dynamic arrays.
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

// -- Composition: heap-handle leaves inside aggregates (recursive lifecycle) --
//
// Queue / dynamic-array members of unpacked structs and class bodies, and
// fixed arrays of queues, ride the recursive aggregate storage model: real
// handle slots, power-on empty, per-leaf deep copy, teardown walks. iverilog
// has no struct/queue composition support at all.

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

// -- String elements (rt_elem_ops managed lifecycle) --
//
// `string q[$]` / `string d[]`: elements are rt_string handles constructed,
// deep-copied, and destroyed through the per-element descriptor. iverilog has no
// string-element container support.

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

// `q = f` (LRM 7.10.2): a queue can be assigned from a fixed-size unpacked
// array too, not just another queue or a dynamic array. No direct runtime
// path exists for this shape (rt_queue_append_range only walks an rt_queue
// source, and a fixed array has no handle at all -- just flat storage), so
// it stages through a temp dynamic array (the same fill a dynamic-array
// TARGET uses) and reuses queue_copy_dynarr. Previously this silently
// misread the fixed array's flat bytes as an rt_queue{base,head,len,cap}
// handle instead of refusing or working -- a genuine data-corrupting bug, not
// merely an unsupported construct. No oracle: iverilog crashes an internal
// assertion compiling ANY queue in this file's shape. Hand-checked against
// the LRM.
TEST_CASE("queue assignment from a fixed-size array") {
    constexpr std::string_view src = R"(
        module m;
            int f[5];
            int q[$];
            initial begin
                f = '{1, 2, 3, 4, 5};
                q = f;
                $display("s=%0d %0d %0d %0d %0d %0d", q.size(), q[0], q[1], q[2], q[3], q[4]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "s=5 1 2 3 4 5\n");
}

// The declaration-initializer form (`int q[$] = f;` / `int q[$] = d;`) shares
// emit_handle_member_copy with every other handle-typed member initializer
// and had the identical bug independently: a fixed-array source produced a
// silently WRONG result (garbage bytes happened to decode as an empty queue
// rather than crashing), and a dynamic-array source was a genuine crash.
TEST_CASE("queue declaration initializer from a fixed-size array or a dynamic array") {
    constexpr std::string_view src = R"(
        module m;
            int f[3] = '{1, 2, 3};
            int q[$] = f;
            int d[] = '{4, 5, 6, 7};
            int qd[$] = d;
            initial begin
                $display("fixed s=%0d %0d %0d", q.size(), q[0], q[2]);
                $display("dynarr s=%0d %0d %0d", qd.size(), qd[0], qd[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "fixed s=3 1 3\n"
                              "dynarr s=4 4 7\n");
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

// `$` in a queue range-select bound (LRM 7.10.4) is the queue's runtime
// size() - 1, not a fixed/packed array's declared high bound: `q = q[1:$]`
// == `q.delete(0)`, `q = q[0:$-1]` drops the tail, `q[$:$]` is the last
// element alone, and arithmetic on it (`$-1`) is ordinary int math. `q = {}`
// (an empty concatenation) clears a queue exactly like `q.delete()` (7.10.2).
// No iverilog oracle: it rejects `$` in a queue range-select as a syntax
// error outright. Hand-checked against the LRM.
TEST_CASE("queue $ slice bound and empty-literal clear") {
    constexpr std::string_view src = R"(
        module m;
            int q[$], s[$];
            initial begin
                q = '{1, 2, 3, 4, 5};
                q = {};
                $display("clear s=%0d", q.size());

                q = '{1, 2, 3, 4, 5};
                q = q[1:$];
                $display("popfront s=%0d %0d", q.size(), q[0]);

                q = '{1, 2, 3, 4, 5};
                q = q[0:$-1];
                $display("popback s=%0d %0d", q.size(), q[q.size()-1]);

                q = '{7, 8, 9};
                s = q[$:$];
                $display("lastonly s=%0d %0d", s.size(), s[0]);

                q = '{1, 2, 3, 4, 5, 6};
                q = q[1:$-1];
                $display("middle s=%0d %0d %0d", q.size(), q[0], q[q.size()-1]);

                q = {};
                s = q[1:$];
                $display("emptysrc s=%0d", s.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "clear s=0\n"
          "popfront s=4 2\n"
          "popback s=4 4\n"
          "lastonly s=1 9\n"
          "middle s=4 2 5\n"
          "emptysrc s=0\n");
}

// Indexed part-select `q[i +: n]` / `q[i -: n]` (LRM 11.5.1, extended to
// queues by 7.10.4). Unlike a fixed/packed array, a queue's width `n` need
// not be a compile-time constant. Both bounds clamp against the queue's
// actual length exactly like the `a:b` form (`queue slice`, above), and the
// base index may itself spell `$` (the last valid index). No iverilog
// oracle: it crashes (an internal assertion) compiling ANY indexed
// part-select of a queue, constant width included. Hand-checked against the
// LRM.
TEST_CASE("queue indexed part-select slice") {
    constexpr std::string_view src = R"(
        module m;
            int q[$], s[$];
            int n, i;
            initial begin
                q = '{1, 2, 3, 4, 5, 6, 7, 8};
                s = q[2 +: 3];
                $display("up-const s=%0d %0d %0d %0d", s.size(), s[0], s[1], s[2]);
                s = q[5 -: 3];
                $display("down-const s=%0d %0d %0d %0d", s.size(), s[0], s[1], s[2]);
                n = 3;
                s = q[2 +: n];
                $display("up-runtime-n s=%0d %0d %0d %0d", s.size(), s[0], s[1], s[2]);
                i = 4;
                s = q[i +: 2];
                $display("up-runtime-i s=%0d %0d", s.size(), s[0]);
                s = q[$ -: 3];
                $display("down-from-end s=%0d %0d %0d %0d", s.size(), s[0], s[1], s[2]);
                s = q[6 +: 5];
                $display("up-oob s=%0d %0d", s.size(), s[0]);
                s = q[1 -: 5];
                $display("down-oob s=%0d %0d", s.size(), s[0]);
                s = q[100 +: 3];
                $display("fully-oob s=%0d", s.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) ==
          "up-const s=3 3 4 5\n"
          "down-const s=3 4 5 6\n"
          "up-runtime-n s=3 3 4 5\n"
          "up-runtime-i s=2 5\n"
          "down-from-end s=3 6 7 8\n"
          "up-oob s=2 7\n"
          "down-oob s=2 1\n"
          "fully-oob s=0\n");
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

// %p (LRM 21.2.1.6) renders '{e0, e1, ...} over the runtime element count:
// integral leaves as minimal decimal (the clause's "as [it] would unformatted",
// with 21.2.1.3's x/z collapse), reals as %g, strings quoted, empty as '{}.
// Nested-in-struct reuses the same runtime path.
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

// === / !== over a queue used to reach a generic scalar-Load lowering
// instead of the aggregate-equality reduction above (the frontend only
// special-cased Equality/Inequality): the backend's defense-in-depth check
// caught the resulting bad IR shape and aborted with an unlocated "plane
// load of a container handle". Found via UVM's `uvm_field_queue_string`
// field-automation macro, whose generated body compares a `string q[$]`
// field with `===`. A string leaf never carries x/z (LRM 6.16.9), so the
// X-propagating == walk is exact for === here too.
TEST_CASE("queue case-equality with a string element (LRM 11.2.2, 11.4.5)") {
    CHECK(run_capture(R"(
        module m;
            string q1[$], q2[$];
            initial begin
                q1.push_back("a"); q1.push_back("b");
                q2.push_back("a"); q2.push_back("b");
                $display("ceq=%b cne=%b", q1 === q2, q1 !== q2);
                q2[1] = "c";
                $display("ceq=%b cne=%b", q1 === q2, q1 !== q2);
            end
        endmodule
    )") == "ceq=1 cne=0\nceq=0 cne=1\n");
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

// -- Class-handle elements (LRM 7.10 with 8.3) ---------------------------------
//
// A handle element needs no lifecycle: the value IS the 64-bit handle, objects are
// never freed (docs/design/classes.md §7), copying a queue copies handles (not
// objects), and `==` on two handles is identity. So the POD element path carries it
// as-is -- only the element-type gate had to admit it.

TEST_CASE("queue of class handles: push, index, and reference semantics") {
    constexpr std::string_view src = R"(
        class Item;
            int v;
            function new(int v_);
                v = v_;
            endfunction
        endclass
        module m;
            Item q[$];
            Item a, b;
            initial begin
                a = new(7);
                b = new(9);
                q.push_back(a);
                q.push_back(b);
                q.push_front(a);
                $display("n=%0d %0d %0d %0d", q.size(), q[0].v, q[1].v, q[2].v);
                // The queue holds handles, so mutating through the element is
                // visible through the original variable.
                q[1].v = 70;
                $display("a=%0d", a.v);
                // Identity, not value: q[0] and q[1] are the same object.
                $display("same=%0b other=%0b", q[0] == q[1], q[0] == q[2]);
                q.delete(0);
                $display("after n=%0d %0d", q.size(), q[0].v);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=3 7 7 9\n"
                              "a=70\n"
                              "same=1 other=0\n"
                              "after n=2 70\n");
}

TEST_CASE("queue of class handles: null default, whole-queue copy, foreach") {
    constexpr std::string_view src = R"(
        class Item;
            int v;
        endclass
        module m;
            Item q[$], c[$];
            Item x;
            int  sum;
            initial begin
                // An out-of-range read yields the element default, which for a
                // handle is null (LRM Table 7-1).
                x = q[0];
                $display("empty null=%0b", x == null);
                for (int i = 0; i < 3; i++) begin
                    // `automatic` is load-bearing: a block declaration defaults to
                    // STATIC lifetime, whose initializer runs once before time zero
                    // (LRM 6.21), so a static `it` would be ONE object pushed three
                    // times and every element would read back the same v.
                    automatic Item it = new;
                    it.v = i + 1;
                    q.push_back(it);
                end
                // A whole-queue copy is a value copy of the HANDLES: the copy is
                // independent as a container, but shares the objects.
                c = q;
                c.push_back(null);
                q[0].v = 100;
                foreach (c[i]) begin
                    if (c[i] != null) sum += c[i].v;
                end
                $display("q=%0d c=%0d sum=%0d", q.size(), c.size(), sum);
                $display("tail null=%0b", c[$] == null);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "empty null=1\n"
                              "q=3 c=4 sum=105\n"
                              "tail null=1\n");
}

TEST_CASE("dynamic array of class handles") {
    constexpr std::string_view src = R"(
        class Item;
            int v;
        endclass
        module m;
            Item d[];
            initial begin
                d = new[2];
                $display("null0=%0b", d[0] == null);
                d[0] = new;
                d[0].v = 5;
                d[1] = d[0];
                d[1].v = 6;
                $display("n=%0d v=%0d", d.size(), d[0].v);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "null0=1\n"
                              "n=2 v=6\n");
}

// `%p` over a container of class handles. Each element renders as `null` or its class
// name -- the same rule a handle follows at any other depth, which is what keeps it
// cycle-safe. The element cannot go through the flat runtime descriptor (a handle is
// a 64-bit word, so it would print the pointer), so a class element routes the
// container through the loop render instead, like every other element the descriptor
// cannot describe. Verilator 5.050 prints `'{t::Item}` here, the same shape.
TEST_CASE("%p of a container of class handles prints the class name per element") {
    CHECK(run_capture(R"(
        class Item;
            int v;
        endclass
        module m;
            Item q[$];
            Item d[];
            Item a[2];
            initial begin
                Item it = new;
                q.push_back(it);
                q.push_back(null);
                d = new[2];
                d[1] = new;
                a[0] = new;
                $display("q=%p d=%p a=%p", q, d, a);
                q.delete();
                $display("empty=%p", q);
            end
        endmodule
    )") == "q='{Item, null} d='{null, Item} a='{Item, null}\nempty='{}\n");
}

TEST_CASE("an event element is a distinct event per slot") {
    // An event element is a 64-bit handle like a class handle, with one difference
    // the descriptor carries: rt_elem_event CONSTRUCTS a new object per slot
    // (LRM Table 6-7's "New event"), where every other kind copies a constant
    // prototype. Copying an element copies the handle, so both slots then name one
    // object -- LRM 15.5.5's merge.
    CHECK(run_capture(R"(
        module m;
            event q[$];
            event a;
            int n = 0;
            initial begin
                q.push_back(a);
                q.push_back(a);
                fork forever begin @(q[0]); n++; end join_none
                #1 -> a;
                #1 $display("n=%0d same=%0b size=%0d", n, q[0] == q[1], q.size());
            end
        endmodule
    )") == "n=1 same=1 size=2\n");
}

// -- Aggregate elements (LRM 7.10 with 7.2) ------------------------------------

// -- chandle elements (LRM 7.10 with 6.14) -------------------------------------
//
// A chandle element needs no lifecycle either: the value IS the opaque 64-bit
// word (LRM 6.14), so it rides the exact same POD element path a class handle
// does -- guard_container_element (src/Lower.cpp) admits the two for the same
// reason. SystemVerilog offers no way to MAKE a non-null chandle without DPI, so
// libc malloc/free stand in as the no-.so probe the rest of the chandle test
// suite already uses (display_test.cpp, dpi_test.cpp).

TEST_CASE("queue of chandles: push, index, and identity") {
    constexpr std::string_view src = R"(
        module m;
            import "DPI-C" function chandle malloc(input longint size);
            import "DPI-C" function void    free(input chandle p);
            chandle q[$];
            chandle a, b;
            initial begin
                a = malloc(8);
                b = malloc(8);
                q.push_back(a);
                q.push_back(b);
                q.push_front(a);
                $display("n=%0d", q.size());
                // Identity, not value: q[0] and q[1] are the SAME chandle (both a);
                // q[1] and q[2] differ (a vs b).
                $display("same=%0b other=%0b", q[0] == q[1], q[1] == q[2]);
                q.delete(0);
                $display("after n=%0d head=%0b", q.size(), q[0] == a);
                free(a);
                free(b);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=3\n"
                              "same=1 other=0\n"
                              "after n=2 head=1\n");
}

TEST_CASE("queue of chandles: null default and whole-queue copy independence") {
    constexpr std::string_view src = R"(
        module m;
            import "DPI-C" function chandle malloc(input longint size);
            import "DPI-C" function void    free(input chandle p);
            chandle q[$], c[$];
            chandle x, h1, h2;
            initial begin
                // An out-of-range read yields the element default, which for a
                // handle is null (LRM Table 7-1).
                x = q[0];
                $display("empty null=%0b", x == null);
                h1 = malloc(8);
                h2 = malloc(8);
                q.push_back(h1);
                q.push_back(h2);
                // A whole-queue copy is a value copy of the HANDLES: the copy is
                // independent as a container (pushing to `c` does not touch `q`)
                // but shares the chandle identities it already held.
                c = q;
                c.push_back(null);
                $display("q=%0d c=%0d", q.size(), c.size());
                $display("shared=%0b tail null=%0b", c[0] == q[0], c[$] == null);
                free(h1);
                free(h2);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "empty null=1\n"
                              "q=2 c=3\n"
                              "shared=1 tail null=1\n");
}

//
// The element slot holds the recursive storage image, so `q[i]` is a heap-anchored
// aggregate lvalue: member and element selects descend it with the ordinary
// aggregate machinery, and a whole element crosses the rt_queue_* ABI by pointer.
// (iverilog cannot parse a queue of structs.)

TEST_CASE("queue of unpacked structs: element, member, and whole-element access") {
    constexpr std::string_view src = R"(
        typedef struct {
            int         id;
            logic [3:0] tag;
        } pkt_t;
        module m;
            pkt_t q[$];
            pkt_t p, r;
            initial begin
                p.id  = 1;
                p.tag = 4'hA;
                q.push_back(p);
                q.push_back('{id: 2, tag: 4'hB});
                q.push_front('{id: 9, tag: 4'hC});
                $display("n=%0d", q.size());
                $display("e %0d %0h %0d %0h %0d %0h", q[0].id, q[0].tag, q[1].id, q[1].tag, q[2].id, q[2].tag);
                // Whole-element read, then write.
                r = q[1];
                q[2] = r;
                $display("r %0d %0h e2 %0d %0h", r.id, r.tag, q[2].id, q[2].tag);
                // Member write through the element.
                q[0].id = 77;
                $display("m %0d", q[0].id);
                // q[$] is the last element.
                $display("last %0d", q[$].id);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=3\n"
                              "e 9 c 1 a 2 b\n"
                              "r 1 a e2 1 a\n"
                              "m 77\n"
                              "last 1\n");
}

TEST_CASE("queue of unpacked structs: pop, insert, delete, foreach") {
    constexpr std::string_view src = R"(
        typedef struct {
            int a;
            int b;
        } pair_t;
        module m;
            pair_t q[$];
            pair_t r;
            int    sum;
            initial begin
                q.push_back('{a: 1, b: 10});
                q.push_back('{a: 2, b: 20});
                q.push_back('{a: 3, b: 30});
                // A pop yields the whole element by value.
                r = q.pop_front();
                $display("pop %0d %0d n=%0d", r.a, r.b, q.size());
                r = q.pop_back();
                $display("pop %0d %0d n=%0d", r.a, r.b, q.size());
                q.insert(0, '{a: 7, b: 70});
                q.insert(2, '{a: 8, b: 80});
                foreach (q[i]) begin
                    sum += q[i].a + q[i].b;
                    $display("fe %0d %0d %0d", i, q[i].a, q[i].b);
                end
                $display("sum=%0d", sum);
                q.delete(1);
                $display("del n=%0d %0d %0d", q.size(), q[0].a, q[1].a);
                q.delete();
                $display("clr n=%0d", q.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "pop 1 10 n=2\n"
                              "pop 3 30 n=1\n"
                              "fe 0 7 70\n"
                              "fe 1 2 20\n"
                              "fe 2 8 80\n"
                              "sum=187\n"
                              "del n=2 7 8\n"
                              "clr n=0\n");
}

TEST_CASE("queue of unpacked structs: out-of-range and empty-pop defaults") {
    // LRM 7.10.2 / Table 7-1: an invalid element read yields the element type's
    // power-on image, per leaf -- 0 for a 2-state leaf, x for a 4-state one -- and
    // an out-of-range write is dropped. An empty pop yields the same image.
    constexpr std::string_view src = R"(
        typedef struct {
            int         two;
            logic [3:0] four;
        } mix_t;
        module m;
            mix_t q[$];
            mix_t r;
            initial begin
                r = q[0];
                $display("empty %0d %0h", r.two, r.four);
                r = q.pop_front();
                $display("pop %0d %0h", r.two, r.four);
                q.push_back('{two: 5, four: 4'h5});
                r = q[99];
                $display("oob %0d %0h", r.two, r.four);
                // Dropped, not a heap write past the live length.
                q[99] = '{two: 6, four: 4'h6};
                q[99].two = 7;
                $display("kept %0d %0h n=%0d", q[0].two, q[0].four, q.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "empty 0 x\n"
                              "pop 0 x\n"
                              "oob 0 x\n"
                              "kept 5 5 n=1\n");
}

TEST_CASE("queue of unpacked structs: nested array member and deep copy") {
    constexpr std::string_view src = R"(
        typedef struct {
            int         v;
            logic [7:0] b[2];
        } nest_t;
        module m;
            nest_t q[$], c[$];
            nest_t n;
            initial begin
                n.v    = 7;
                n.b[0] = 8'h11;
                n.b[1] = 8'h22;
                q.push_back(n);
                // An element select INSIDE the element: two GEP levels over one
                // heap-anchored root.
                q[0].b[1] = 8'h33;
                $display("q %0d %0h %0h", q[0].v, q[0].b[0], q[0].b[1]);
                // A whole-queue copy is a deep copy: the copy's elements are its own.
                c = q;
                c[0].b[0] = 8'hFF;
                c[0].v    = 8;
                $display("q %0d %0h c %0d %0h", q[0].v, q[0].b[0], c[0].v, c[0].b[0]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "q 7 11 33\n"
                              "q 7 11 c 8 ff\n");
}

TEST_CASE("queue of unpacked structs: bound, slice, concat, and literals") {
    constexpr std::string_view src = R"(
        typedef struct { int a; } t;
        module m;
            t q[$], s[$];
            t bq[$:1];
            initial begin
                q = '{t'{a: 1}, t'{a: 2}, t'{a: 3}};
                $display("lit %0d %0d %0d %0d", q.size(), q[0].a, q[1].a, q[2].a);
                s = q[1:2];
                $display("sl %0d %0d %0d", s.size(), s[0].a, s[1].a);
                q = {q, t'{a: 4}};
                $display("cc %0d %0d", q.size(), q[3].a);
                bq.push_back(t'{a: 1});
                bq.push_back(t'{a: 2});
                bq.push_back(t'{a: 3});
                $display("bd %0d %0d", bq.size(), bq[$].a);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "lit 3 1 2 3\n"
                              "sl 2 2 3\n"
                              "cc 4 4\n"
                              "bd 2 2\n");
}

TEST_CASE("queue of unpacked structs: subroutine ABI, class property, struct member") {
    constexpr std::string_view src = R"(
        typedef struct { int a; } t;
        typedef struct {
            t   q[$];
            int n;
        } outer_t;
        class C;
            t q[$];
        endclass
        module m;
            t       src[$];
            outer_t o;
            C       c;
            function automatic int total(t f[$]);
                int s = 0;
                foreach (f[i]) s += f[i].a;
                return s;
            endfunction
            task automatic append(inout t f[$]);
                f.push_back(t'{a: 100});
            endtask
            initial begin
                src.push_back(t'{a: 5});
                src.push_back(t'{a: 6});
                $display("total=%0d", total(src));
                append(src);
                $display("io %0d %0d", src.size(), src[2].a);
                o.q.push_back(t'{a: 3});
                o.q[0].a = 4;
                $display("mem %0d %0d", o.q.size(), o.q[0].a);
                c = new;
                c.q.push_back(t'{a: 1});
                c.q[0].a = 9;
                $display("prop %0d %0d", c.q.size(), c.q[0].a);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "total=11\n"
                              "io 3 100\n"
                              "mem 1 4\n"
                              "prop 1 9\n");
}

TEST_CASE("nonblocking whole-queue assignment with aggregate elements") {
    constexpr std::string_view src = R"(
        typedef struct { int a; } t;
        module m;
            t     q[$], d[$];
            logic clk = 0;
            initial begin
                d.push_back(t'{a: 2});
                #1 clk = 1;
                #1 $display("nba %0d %0d", q.size(), q[0].a);
                $finish;
            end
            always @(posedge clk) q <= d;
        endmodule
    )";
    CHECK(run_capture(src) == "nba 1 2\n");
}

// -- Aggregate elements with managed leaves (rt_elem_ops kind 4) ----------------
//
// A string (or, later, container) leaf inside an element is heap-owning, so the
// element cannot be copied or freed as bytes. The backend emits an offset-sorted
// leaf table beside the element descriptor and the runtime deep-copies each leaf
// and raw-copies the gaps between them. Run under ASan/LSan, so a missed free or a
// double free fails the case rather than passing quietly.

TEST_CASE("queue of structs with a string member") {
    constexpr std::string_view src = R"(
        typedef struct {
            int    id;
            string name;
        } rec_t;
        module m;
            rec_t q[$];
            rec_t r;
            initial begin
                q.push_back('{id: 1, name: "alpha"});
                q.push_back('{id: 2, name: "beta"});
                $display("n=%0d %0d %s %0d %s", q.size(), q[0].id, q[0].name, q[1].id, q[1].name);
                // A string leaf written through the element.
                q[0].name = "ALPHA";
                $display("w %s", q[0].name);
                // A whole-element read is a deep copy: the copy's string is its own.
                r = q[1];
                r.name = "local";
                $display("r %s q1 %s", r.name, q[1].name);
                // ... and so is a whole-element write.
                q[0] = r;
                r.name = "changed";
                $display("q0 %s r %s", q[0].name, r.name);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=2 1 alpha 2 beta\n"
                              "w ALPHA\n"
                              "r local q1 beta\n"
                              "q0 local r changed\n");
}

TEST_CASE("popping a managed element repeatedly frees what it replaces") {
    // The runtime RELOCATES a popped element into a body-lifetime buffer, so the
    // buffer owns its string: the next pop must free the previous one, and the last
    // must be freed at the body's exit. LSan is the assertion.
    constexpr std::string_view src = R"(
        typedef struct {
            int    id;
            string name;
        } rec_t;
        module m;
            rec_t q[$];
            rec_t r;
            initial begin
                for (int i = 0; i < 4; i++) begin
                    q.push_back('{id: i, name: $sformatf("item-%0d-with-a-heap-tail", i)});
                end
                while (q.size() > 0) begin
                    r = q.pop_front();
                    $display("pop %0d %s", r.id, r.name);
                end
                // An empty pop yields the element default, not the last value.
                r = q.pop_back();
                $display("empty %0d [%s]", r.id, r.name);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "pop 0 item-0-with-a-heap-tail\n"
                              "pop 1 item-1-with-a-heap-tail\n"
                              "pop 2 item-2-with-a-heap-tail\n"
                              "pop 3 item-3-with-a-heap-tail\n"
                              "empty 0 []\n");
}

TEST_CASE("managed leaves survive copy, delete and an array-of-strings member") {
    constexpr std::string_view src = R"(
        typedef struct {
            string tags[2];
            int    n;
        } tags_t;
        module m;
            tags_t t[$], c[$];
            initial begin
                t.push_back('{tags: '{"x", "y"}, n: 2});
                // One leaf-table entry per array element, in ascending offset order.
                t[0].tags[1] = "a-much-longer-tag-that-heap-allocates";
                $display("t %s %s %0d", t[0].tags[0], t[0].tags[1], t[0].n);
                // A whole-queue copy deep-copies every leaf of every element.
                c = t;
                c[0].tags[0] = "replaced";
                $display("t %s c %s", t[0].tags[0], c[0].tags[0]);
                // Clearing frees each element's leaves.
                c.delete();
                t.delete();
                $display("n=%0d %0d", t.size(), c.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "t x a-much-longer-tag-that-heap-allocates 2\n"
                              "t x c replaced\n"
                              "n=0 0\n");
}

TEST_CASE("dynamic array of structs with a string member") {
    constexpr std::string_view src = R"(
        typedef struct {
            int    id;
            string name;
        } rec_t;
        module m;
            rec_t d[];
            initial begin
                d = new[2];
                // A fresh element's string leaf powers on empty, not garbage.
                $display("init %0d [%s]", d[0].id, d[0].name);
                d[0] = '{id: 9, name: "dyn-element-with-a-heap-tail"};
                d[1].name = "second";
                $display("d %0d %s %s", d.size(), d[0].name, d[1].name);
                d.delete();
                $display("del %0d", d.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "init 0 []\n"
                              "d 2 dyn-element-with-a-heap-tail second\n"
                              "del 0\n");
}

// -- Nested container elements (LRM 7.10 with 7.5) -----------------------------
//
// `int q[$][$]`: the element slot holds the inner container's handle and the
// element descriptor chains through rt_elem_ops::inner, so the runtime deep-copies
// and frees the inner container per element. `q[i]` is a handle lvalue, so the
// inner container's own methods work on it unchanged.

TEST_CASE("queue of queues: copy semantics, nested access, nested foreach") {
    constexpr std::string_view src = R"(
        module m;
            int qq[$][$];
            int inner[$];
            int row[$];
            initial begin
                inner.push_back(1);
                inner.push_back(2);
                qq.push_back(inner);
                inner.push_back(3);
                qq.push_back(inner);
                // Each push took a deep copy, so the two elements differ and the
                // source is independent of both.
                $display("n=%0d %0d %0d src=%0d", qq.size(), qq[0].size(), qq[1].size(), inner.size());
                $display("e %0d %0d %0d", qq[0][0], qq[1][1], qq[1][2]);
                // A method on the inner container mutates the element in place.
                qq[0].push_back(99);
                $display("m %0d %0d src=%0d", qq[0].size(), qq[0][$], inner.size());
                // A whole-element read is a deep copy too.
                row = qq[1];
                row.push_back(7);
                $display("row %0d qq1 %0d", row.size(), qq[1].size());
                foreach (qq[i]) begin
                    foreach (qq[i][j]) $display("fe %0d %0d %0d", i, j, qq[i][j]);
                end
                qq.delete();
                $display("clr %0d", qq.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=2 2 3 src=3\n"
                              "e 1 2 3\n"
                              "m 3 99 src=3\n"
                              "row 4 qq1 3\n"
                              "fe 0 0 1\n"
                              "fe 0 1 2\n"
                              "fe 0 2 99\n"
                              "fe 1 0 1\n"
                              "fe 1 1 2\n"
                              "fe 1 2 3\n"
                              "clr 0\n");
}

TEST_CASE("an out-of-range nested container element is empty, and writes to it vanish") {
    // LRM 7.4.5 through a container-valued element: the read is the type's default
    // (an EMPTY container), and a mutating method on it must not touch the heap
    // buffer past its length. Both ride the per-element-type sink handle.
    constexpr std::string_view src = R"(
        module m;
            int qq[$][$];
            int inner[$];
            initial begin
                inner.push_back(1);
                qq.push_back(inner);
                $display("oob %0d", qq[9].size());
                qq[9].push_back(5);
                // Still empty, and the live element is untouched.
                $display("after %0d %0d %0d", qq[9].size(), qq.size(), qq[0].size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "oob 0\n"
                              "after 0 1 1\n");
}

TEST_CASE("popping a nested container hands back its handle and frees the old one") {
    constexpr std::string_view src = R"(
        module m;
            int qq[$][$];
            int row[$];
            initial begin
                // `tmp` is cleared per row rather than redeclared: a block-scoped
                // container local is not re-initialized on re-entry (a separate gap),
                // and this case is about the pops.
                int tmp[$];
                for (int k = 0; k < 3; k++) begin
                    tmp.delete();
                    for (int j = 0; j <= k; j++) tmp.push_back(j);
                    qq.push_back(tmp);
                end
                while (qq.size() > 0) begin
                    row = qq.pop_front();
                    $display("pop %0d %0d", row.size(), row[$]);
                end
                // An empty pop yields an empty container, not the last one.
                row = qq.pop_back();
                $display("empty %0d", row.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "pop 1 0\n"
                              "pop 2 1\n"
                              "pop 3 2\n"
                              "empty 0\n");
}

TEST_CASE("mixed nesting: queue of dynamic arrays, dynamic array of queues, three deep") {
    constexpr std::string_view src = R"(
        module m;
            int qd[$][];
            int dq[][$];
            int q3[$][$][$];
            int d[];
            int q[$];
            int i2[$][$];
            int i1[$];
            initial begin
                d = new[2];
                d[0] = 5;
                d[1] = 6;
                qd.push_back(d);
                qd[0][1] = 9;
                $display("qd %0d %0d %0d %0d", qd.size(), qd[0].size(), qd[0][1], d[1]);
                dq = new[2];
                q.push_back(3);
                dq[0] = q;
                dq[0].push_back(4);
                $display("dq %0d %0d %0d %0d", dq.size(), dq[0].size(), dq[0][1], dq[1].size());
                i1.push_back(7);
                i2.push_back(i1);
                q3.push_back(i2);
                $display("q3 %0d %0d %0d %0d", q3.size(), q3[0].size(), q3[0][0].size(), q3[0][0][0]);
                q3.delete();
                qd.delete();
                dq.delete();
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "qd 1 2 9 6\n"
                              "dq 2 2 4 0\n"
                              "q3 1 1 1 7\n");
}

TEST_CASE("a struct element with a queue member deep-copies that member") {
    constexpr std::string_view src = R"(
        typedef struct {
            int n;
            int q[$];
        } t;
        module m;
            t outer[$];
            initial begin
                t x;
                x.n = 1;
                x.q.push_back(5);
                outer.push_back(x);
                $display("o %0d %0d %0d %0d", outer.size(), outer[0].n, outer[0].q.size(), outer[0].q[0]);
                // The element's queue is its own: pushing to it leaves x alone.
                outer[0].q.push_back(6);
                $display("m %0d %0d", outer[0].q.size(), x.q.size());
                // And out of range it vanishes, rather than writing past the buffer.
                outer[9].q.push_back(7);
                $display("oob %0d %0d", outer[9].q.size(), outer.size());
                outer.delete();
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "o 1 1 1 5\n"
                              "m 2 1\n"
                              "oob 0 1\n");
}

TEST_CASE("an associative-array element deep-copies per element") {
    // rt_elem_assoc (kind 5) drives an assoc element's lifecycle exactly as
    // rt_elem_queue drives a nested queue's: the slot holds its handle and the
    // descriptor chains through `inner`, so a push deep-copies the map and a pop or
    // a whole-queue teardown frees it. No oracle: iverilog cannot parse an
    // associative-array declaration at all.
    constexpr std::string_view src = R"(
        module m;
            int q[$][string];
            int m0[string];
            initial begin
                m0["a"] = 1;
                m0["b"] = 2;
                q.push_back(m0);
                m0["c"] = 3;
                q.push_back(m0);
                $display("n=%0d e0=%0d e1=%0d", q.size(), q[0].num(), q[1].num());
                $display("a0=%0d c1=%0d", q[0]["a"], q[1]["c"]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n=2 e0=2 e1=3\na0=1 c1=3\n");
}

// -- Operations over a non-scalar element --------------------------------------

TEST_CASE("locator and reduction methods over struct elements, keyed by a clause") {
    // 7.12.1 / 7.12.3 through an aggregate element: the clause supplies the key, the
    // iterator variable binds by aggregate copy, and a locator's result queue
    // receives whole elements.
    constexpr std::string_view src = R"(
        typedef struct {
            int a;
            int b;
        } t;
        module m;
            t   q[$], r[$];
            int idx[$];
            initial begin
                q.push_back('{a: 1, b: 10});
                q.push_back('{a: 5, b: 50});
                q.push_back('{a: 3, b: 30});
                r = q.find(x) with (x.a > 2);
                $display("find %0d %0d %0d", r.size(), r[0].a, r[1].a);
                idx = q.find_index(x) with (x.a == 3);
                $display("idx %0d %0d", idx.size(), idx[0]);
                r = q.find_first(x) with (x.b > 5);
                $display("first %0d %0d", r.size(), r[0].a);
                r = q.min(x) with (x.a);
                $display("min %0d %0d", r.size(), r[0].a);
                r = q.max(x) with (x.b);
                $display("max %0d %0d", r.size(), r[0].b);
                $display("sum %0d", q.sum(x) with (x.a));
                // Dedup by the clause's key: two elements share key 1.
                r = q.unique(x) with (x.a > 2);
                $display("uniq %0d %0d %0d", r.size(), r[0].a, r[1].a);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "find 2 5 3\n"
                              "idx 1 2\n"
                              "first 1 1\n"
                              "min 1 1\n"
                              "max 1 50\n"
                              "sum 9\n"
                              "uniq 2 1 5\n");
}

TEST_CASE("equality of containers with aggregate, nested and wide elements") {
    // LRM 11.4.5 / 7.10: equal sizes and equal elements, X-propagating. The flat
    // runtime comparator cannot read any of these element shapes, so the walk is
    // built per element pair.
    constexpr std::string_view src = R"(
        typedef struct {
            int         a;
            logic [3:0] f;
        } t;
        module m;
            t            p[$], q[$];
            int          n1[$][$], n2[$][$];
            int          r[$];
            logic [95:0] w1[$], w2[$];
            initial begin
                p.push_back('{a: 1, f: 4'h2});
                q.push_back('{a: 1, f: 4'h2});
                $display("agg %0b", p == q);
                q[0].a = 9;
                $display("ne %0b %0b", p == q, p != q);
                q[0].a = 1;
                q.push_back('{a: 2, f: 4'h3});
                $display("size %0b", p == q);
                // An unknown leaf makes the whole compare x, not 0.
                q.delete();
                q.push_back('{a: 1, f: 4'hx});
                $display("x %0b", p == q);
                r.push_back(5);
                n1.push_back(r);
                n2.push_back(r);
                $display("nest %0b", n1 == n2);
                n2[0].push_back(6);
                $display("nest2 %0b", n1 == n2);
                w1.push_back(96'hdead_beef_cafe_babe_1234_5678);
                w2.push_back(96'hdead_beef_cafe_babe_1234_5678);
                $display("wide %0b", w1 == w2);
                w2[0] = 96'h1;
                $display("wide2 %0b", w1 == w2);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "agg 1\n"
                              "ne 0 1\n"
                              "size 0\n"
                              "x x\n"
                              "nest 1\n"
                              "nest2 0\n"
                              "wide 1\n"
                              "wide2 0\n");
}

TEST_CASE("%p of a container with a non-scalar or wide element") {
    // LRM 21.2.1.6 over the live element count. The runtime formatter renders from a
    // flat descriptor (one word, no member names), so an aggregate, a nested
    // container, a wide element and a tagged union render through the frontend loop
    // instead -- reusing the ordinary aggregate expansion, hence the member names.
    constexpr std::string_view src = R"(
        typedef struct {
            int         a;
            logic [3:0] f;
            string      s;
        } t;
        typedef union tagged packed {
            logic [7:0] i;
            logic [7:0] f;
        } tu_t;
        module m;
            t            q[$];
            int          n[$][$];
            int          r[$], e[$];
            logic [95:0] w[$];
            tu_t         u[$];
            initial begin
                q.push_back('{a: 1, f: 4'h2, s: "hi"});
                q.push_back('{a: -3, f: 4'hx, s: ""});
                $display("q=%p", q);
                r.push_back(5);
                r.push_back(6);
                n.push_back(r);
                n.push_back(e);
                $display("n=%p", n);
                $display("empty=%p", e);
                w.push_back(96'h1_0000_0000_0000_0000);
                $display("w=%p", w);
                u.push_back(tagged i 8'h7);
                $display("u=%p", u);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "q='{'{a:1, f:2, s:\"hi\"}, '{a:-3, f:x, s:\"\"}}\n"
                              "n='{'{5, 6}, '{}}\n"
                              "empty='{}\n"
                              "w='{18446744073709551616}\n"
                              "u='{'{i:7}}\n");
}

TEST_CASE("a keyless locator over an aggregate element is refused") {
    // With no clause the key is the element itself (7.12.1), and an unpacked
    // aggregate has no ordering to sort or fold by.
    expect_fail(R"(
        module m;
            typedef struct { int a; } t;
            t q[$], r[$];
            initial r = q.unique();
        endmodule
    )",
                "needs a `with` clause");
}
