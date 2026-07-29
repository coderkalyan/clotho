#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture stdout (mirrors string_test.cpp). Expectations
// are cross-checked against iverilog (the LRM 7.5 oracle).
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

// new[n] allocates n default-initialized elements; size() reports the count;
// element read/write address the heap buffer (LRM 7.5).
TEST_CASE("dynamic array new/size/element access") {
    constexpr std::string_view src = R"(
        module m;
            int arr[];
            initial begin
                arr = new[3];
                $display("%0d %0d %0d %0d", arr.size(), arr[0], arr[1], arr[2]);
                arr[0] = 10; arr[1] = 20; arr[2] = 30;
                $display("%0d %0d %0d", arr[0], arr[1], arr[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 0 0 0\n10 20 30\n");
}

// new[n](src) copies min(n, src.size) elements, the rest default (LRM 7.5.1).
TEST_CASE("dynamic array new with init source resizes") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin
                a = new[2];
                a[0] = 7; a[1] = 9;
                a = new[4](a);
                $display("%0d %0d %0d %0d %0d", a.size(), a[0], a[1], a[2], a[3]);
                a = new[1](a);
                $display("%0d %0d", a.size(), a[0]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4 7 9 0 0\n1 7\n");
}

// Assignment is a deep copy: mutating the copy leaves the source untouched
// (value semantics, LRM 7.5).
TEST_CASE("dynamic array assignment is a deep copy") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int b[];
            initial begin
                a = new[2];
                a[0] = 1; a[1] = 2;
                b = a;
                b[0] = 99;
                $display("%0d %0d %0d %0d", a[0], a[1], b[0], b[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 2 99 2\n");
}

// delete() frees the storage and resets the size to 0 (LRM 7.5).
TEST_CASE("dynamic array delete resets size") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin
                a = new[5];
                $display("%0d", a.size());
                a.delete();
                $display("%0d", a.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "5\n0\n");
}

// foreach iterates 0..size-1 (LRM 12.7.3).
TEST_CASE("dynamic array foreach") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int sum;
            initial begin
                a = new[4];
                foreach (a[i]) a[i] = i * i;
                sum = 0;
                foreach (a[i]) sum = sum + a[i];
                $display("%0d %0d %0d %0d sum=%0d", a[0], a[1], a[2], a[3], sum);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 1 4 9 sum=14\n");
}

// An out-of-range read yields the element default (0 for a 2-state int); an
// out-of-range write is a no-op and does not corrupt the heap (LRM 7.5.1).
TEST_CASE("dynamic array out-of-range access") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            int r;
            initial begin
                a = new[2];
                a[0] = 5; a[1] = 6;
                a[5] = 123;          // out of range: no-op
                r = a[5];            // out of range: default 0
                $display("%0d %0d %0d", a[0], a[1], r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "5 6 0\n");
}

// A dynamic array declared as a function-frame local: allocated, used, and
// freed per call (value semantics, scope-based deallocation). Calling twice
// with different sizes exercises the per-call frame.
TEST_CASE("dynamic array as a function local") {
    constexpr std::string_view src = R"(
        module m;
            function int fsum(int n);
                int t[];
                int s;
                t = new[n];
                foreach (t[i]) t[i] = i + 1;
                s = 0;
                foreach (t[i]) s = s + t[i];
                return s;
            endfunction
            initial $display("%0d %0d", fsum(4), fsum(3));
        endmodule
    )";
    CHECK(run_capture(src) == "10 6\n");
}

// A task-frame local dynamic array works the same (time-consuming frame).
TEST_CASE("dynamic array as a task local") {
    constexpr std::string_view src = R"(
        module m;
            task run(input int n, output int mx);
                int t[];
                t = new[n];
                foreach (t[i]) t[i] = (i * 7) % 5;
                mx = 0;
                foreach (t[i]) if (t[i] > mx) mx = t[i];
            endtask
            int r;
            initial begin run(6, r); $display("%0d", r); end
        endmodule
    )";
    CHECK(run_capture(src) == "4\n");
}

// A dynamic array passed as an input argument: the callee reads it (LRM 13.5.1).
TEST_CASE("dynamic array as an input function argument") {
    constexpr std::string_view src = R"(
        module m;
            function int sumarr(int a[]);
                int s;
                s = 0;
                foreach (a[i]) s = s + a[i];
                return s;
            endfunction
            int q[];
            initial begin
                q = new[3]; q[0] = 10; q[1] = 20; q[2] = 30;
                $display("%0d %0d", sumarr(q), q.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "60 3\n");
}

// An input argument is deep-copied (LRM 13.5.1 value semantics): the callee
// mutating its copy must NOT change the caller's array.
TEST_CASE("dynamic array input argument is copy-in (value semantics)") {
    constexpr std::string_view src = R"(
        module m;
            function int clobber(int a[]);
                a[0] = 999;
                a = new[1];
                return 0;
            endfunction
            int q[];
            int dummy;
            initial begin
                q = new[2]; q[0] = 5; q[1] = 6;
                dummy = clobber(q);
                $display("%0d %0d %0d", q.size(), q[0], q[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 5 6\n");
}

// A dynamic array as a task input argument.
TEST_CASE("dynamic array as a task input argument") {
    constexpr std::string_view src = R"(
        module m;
            task total(input int a[], output int t);
                t = 0;
                foreach (a[i]) t = t + a[i];
            endtask
            int q[];
            int r;
            initial begin
                q = new[4]; foreach (q[i]) q[i] = i + 1;
                total(q, r);
                $display("%0d", r);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10\n");
}

// An output dynamic-array argument: the callee builds a fresh array in its
// frame and it is copied out into the caller's variable at return (LRM 13.5.1).
TEST_CASE("dynamic array as an output argument") {
    constexpr std::string_view src = R"(
        module m;
            task fill(output int a[], input int n);
                a = new[n];
                foreach (a[i]) a[i] = (i + 1) * 10;
            endtask
            int q[];
            initial begin
                fill(q, 3);
                $display("%0d %0d %0d %0d", q.size(), q[0], q[1], q[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 10 20 30\n");
}

// An inout dynamic-array argument: copied in, mutated, copied back out.
TEST_CASE("dynamic array as an inout argument") {
    constexpr std::string_view src = R"(
        module m;
            function int bump(inout int a[]);
                foreach (a[i]) a[i] = a[i] + 1;
                return a.size();
            endfunction
            int q[];
            int n;
            initial begin
                q = new[3]; q[0] = 10; q[1] = 20; q[2] = 30;
                n = bump(q);
                $display("%0d %0d %0d %0d", n, q[0], q[1], q[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 11 21 31\n");
}

// A ref dynamic-array argument aliases the caller's handle: the callee's new[]
// and element writes affect the caller directly, with no copy (LRM 13.5.2).
TEST_CASE("dynamic array as a ref argument") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int grow(ref int a[], input int n);
                a = new[n];
                foreach (a[i]) a[i] = i;
                return 0;
            endfunction
            int q[];
            int d;
            initial begin
                d = grow(q, 4);
                $display("%0d %0d %0d %0d %0d", q.size(), q[0], q[1], q[2], q[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4 0 1 2 3\n");
}

// A function returning a dynamic array: the result is deep-copied into the
// caller's variable (LRM 13.4 value return).
TEST_CASE("dynamic array function return value") {
    constexpr std::string_view src = R"(
        module m;
            typedef int iarr[];
            function iarr mkarr(int n);
                iarr t;
                t = new[n];
                foreach (t[i]) t[i] = i * 2;
                return t;
            endfunction
            int q[];
            initial begin
                q = mkarr(4);
                $display("%0d %0d %0d %0d %0d", q.size(), q[0], q[1], q[2], q[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "4 0 2 4 6\n");
}

// A dynamic-array return value used directly (not via an assignment): its
// result handle backs .size() / element access.
TEST_CASE("dynamic array return value used inline") {
    constexpr std::string_view src = R"(
        module m;
            typedef int iarr[];
            function iarr seq(int n);
                iarr t;
                t = new[n];
                foreach (t[i]) t[i] = i + 1;
                return t;
            endfunction
            initial $display("%0d %0d", seq(5).size(), seq(3)[2]);
        endmodule
    )";
    CHECK(run_capture(src) == "5 3\n");
}

// A 4-state (logic) element powers on as X under new[] (LRM 7.5: elements take
// their type's default, which for logic is x).
TEST_CASE("dynamic array 4-state element defaults to x") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a[];
            initial begin
                a = new[2];
                $display("%h %h", a[0], a[1]);
                a[0] = 8'hA5;
                $display("%h %h", a[0], a[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "xx xx\na5 xx\n");
}

// $size(a) on a runtime dynamic array lowers to .size() (LRM 20.7).
TEST_CASE("dynamic array $size") {
    constexpr std::string_view src = R"(
        module m;
            int a[];
            initial begin a = new[5]; $display("%0d", $size(a)); end
        endmodule
    )";
    CHECK(run_capture(src) == "5\n");
}

// Regression: a dynamic-array declaration initializer used to lower a `{...}`
// or `'{...}` literal as a plain packed bit-concat and Store it raw into the
// {ptr,len} handle slot -- an out-of-bounds write past the 16-byte handle that
// corrupted adjacent global state (SIGSEGV). Verified vs iverilog 13.
TEST_CASE("dynamic array decl initializer from a bare concatenation literal") {
    constexpr std::string_view src = R"(
        module m;
            int ia[] = {4,5,3,1};
            initial $display("%0d %0d %0d %0d", ia[0], ia[1], ia[2], ia[3]);
        endmodule
    )";
    CHECK(run_capture(src) == "4 5 3 1\n");
}

TEST_CASE("dynamic array decl initializer from a '{...} pattern literal") {
    constexpr std::string_view src = R"(
        module m;
            int ia[] = '{4,5,3,1};
            initial $display("%0d %0d %0d %0d", ia[0], ia[1], ia[2], ia[3]);
        endmodule
    )";
    CHECK(run_capture(src) == "4 5 3 1\n");
}

TEST_CASE("dynamic array decl initializer copied from another dynamic array is a deep copy") {
    constexpr std::string_view src = R"(
        module m;
            int src[] = '{7,8,9};
            int ia[] = src;
            initial $display("%0d %0d %0d %0d %0d", ia.size(), ia[0], ia[1], ia[2], src[0]);
        endmodule
    )";
    CHECK(run_capture(src) == "3 7 8 9 7\n");
}

TEST_CASE("queue decl initializer from a bare concatenation literal") {
    constexpr std::string_view src = R"(
        module m;
            int q[$] = {4,5,3,1};
            initial $display("%0d %0d %0d %0d %0d", q.size(), q[0], q[1], q[2], q[3]);
        endmodule
    )";
    CHECK(run_capture(src) == "4 4 5 3 1\n");
}

// Fixed-size unpacked array of scalar elements: the same decl-initializer gap
// previously silently mis-lowered the packed-concat bit order into the flat
// array storage (ia[0] printed 1 instead of 4) instead of crashing -- a
// silent miscompile, now closed by the same fix.
TEST_CASE("fixed-size unpacked array decl initializer from a bare concatenation literal") {
    constexpr std::string_view src = R"(
        module m;
            int ia[4] = {4,5,3,1};
            initial $display("%0d %0d %0d %0d", ia[0], ia[1], ia[2], ia[3]);
        endmodule
    )";
    CHECK(run_capture(src) == "4 5 3 1\n");
}

// A dynamic array assigned or new[]-initialized from an assignment-pattern
// literal materializes element-wise (LRM 10.9 / 7.5) rather than copying a
// handle. `new[N]('{...}')` sizes to N and fills the low elements. Self-checked
// against iverilog (ivtest br_gh164c / sv_assign_pattern_part).
TEST_CASE("dynamic array from an assignment-pattern literal") {
    constexpr std::string_view src = R"(
        module m;
            int d[];
            int e[];
            initial begin
                d = '{10, 20, 30};
                e = new[4]('{1, 2, 3, 4});
                $display("%0d %0d %0d %0d", d.size(), d[0], d[1], d[2]);
                $display("%0d %0d %0d %0d %0d", e.size(), e[0], e[1], e[2], e[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "3 10 20 30\n4 1 2 3 4\n");
}
