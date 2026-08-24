#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "Subprocess.h"
#include "PosixIo.h"
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

// LRM 7.5.1 sizes a dynamic array with an `int`, and gives no meaning to a
// negative one. The count used to cross the runtime ABI zero-extended, so
// `new[-1]` asked for 4294967295 elements and the element-count-times-stride
// product then wrapped: a small buffer the constructor loop immediately ran off
// the end of. The count is signed at the ABI now, so the runtime sees the
// negative the design wrote and refuses it with a located fatal. Out of process
// because the fatal exits.
TEST_CASE("new[] with a negative size is a located fatal") {
    constexpr std::string_view src = "module m;\n"
                                     "    int arr[];\n"
                                     "    int n;\n"
                                     "    initial begin\n"
                                     "        n = -1;\n"
                                     "        arr = new[n];\n"
                                     "        $display(\"unreachable\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out.empty());
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("invalid negative size for this dynamic array: -1") != std::string::npos);
    // The site record is what makes the report point at the design rather than
    // at the runtime.
    CHECK(run.err.find("source:6:9") != std::string::npos);
    CHECK(run.err.find("6 |         arr = new[n];\n") != std::string::npos);
}

// The same all-ones bit pattern that is -1 in a signed `int` is 4294967295 in an
// unsigned `bit [31:0]`, and must reach the runtime zero-extended.
//
// The element is a megabit wide so the request is 4 PiB: past the address space
// of any host, so the answer is a refusal naming the byte count rather than
// something whose outcome varies per machine. That byte count IS the assertion —
// 4294967295 * 1 MiB is reachable only from a zero-extended count, where a
// sign-extended one would have tripped the negative check.
//
// The size matters for a second reason: an oversized request Linux happens to
// grant (its default overcommit heuristic is lenient) used to be filled element
// by element, which paged in the whole array and got the test runner — and
// whatever else the machine was doing — killed by the OOM killer. A zero-image
// element is calloc'd now, so even a granted request costs nothing until it is
// written. Keep both properties when touching this test.
//
// `allocator_may_return_null=1` for the same reason as the refused-allocation
// case below: AddressSanitizer's allocator aborts on a request this size before
// the runtime's own null check can report it. A build without ASAN ignores it.
TEST_CASE("new[] with an unsigned count is not read as negative") {
    constexpr std::string_view src = "module m;\n"
                                     "    bit [8388607:0] arr[];\n"
                                     "    bit [31:0] n;\n"
                                     "    initial begin\n"
                                     "        n = 32'hFFFF_FFFF;\n"
                                     "        arr = new[n];\n"
                                     "        $display(\"allocated\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run =
      test::run_design_captured(src, "ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 ");

    CHECK(run.err.find("negative size") == std::string::npos);
    if (run.err.find("out of memory") != std::string::npos) {
        CHECK(run.err.find("the system refused 4503599626321920 bytes") != std::string::npos);
    } else {
        // A host whose address space could hold the mapping: nothing is touched,
        // so it succeeds and the count is proven by the array existing at all.
        CHECK(run.out.find("allocated") != std::string::npos);
    }
}

// The element count times the element stride is a checked multiply. Wrapped, it
// sized a buffer from the low 64 bits of the product and the constructor loop
// then wrote `n` elements into it.
TEST_CASE("new[] whose element count times stride overflows is a located fatal") {
    constexpr std::string_view src = "module m;\n"
                                     "    longint unsigned big[];\n"
                                     "    longint unsigned n;\n"
                                     "    initial begin\n"
                                     "        n = 64'h4000_0000_0000_0001;\n"
                                     "        big = new[n];\n"
                                     "        $display(\"unreachable\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("size for this dynamic array overflows") != std::string::npos);
    CHECK(run.err.find("source:6:9") != std::string::npos);
}

// A size the machine cannot satisfy is a diagnosed failure, not a null buffer
// the caller writes through. 1 EiB of `int` is past the address space of any
// host — 400 GB, what this asked for before, is not: x86-64 maps 128 TiB, Linux
// grants the mapping, and the array then filled itself page by page until the
// OOM killer took the test runner down with it. It stays well inside size_t, so
// it still exercises the malloc-returned-null arm rather than the overflow arm.
// The child needs `allocator_may_return_null=1` because AddressSanitizer's
// allocator otherwise aborts on an oversized request before the runtime's own
// null check can run; a build without ASAN ignores the variable.
TEST_CASE("an allocation the system refuses is a located fatal") {
    constexpr std::string_view src = "module m;\n"
                                     "    int a[];\n"
                                     "    longint unsigned n;\n"
                                     "    initial begin\n"
                                     "        n = 64'd300_000_000_000_000_000;\n"
                                     "        a = new[n];\n"
                                     "        $display(\"unreachable\");\n"
                                     "    end\n"
                                     "endmodule\n";

    const test::DesignRun run =
      test::run_design_captured(src, "ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 ");

    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("out of memory for this dynamic array") != std::string::npos);
    CHECK(run.err.find("source:6:9") != std::string::npos);
}

// -- Aggregate elements (LRM 7.5 with 7.2) -------------------------------------
//
// Same element capability as a queue: both drive rt_elem_ops, and the element slot
// presents the same heap-anchored aggregate lvalue.

TEST_CASE("dynamic array of unpacked structs") {
    constexpr std::string_view src = R"(
        typedef struct {
            int         a;
            logic [3:0] f;
        } t;
        module m;
            t d[], e[];
            t r;
            initial begin
                d = new[2];
                // A fresh element is its power-on image, per leaf.
                $display("init %0d %0h", d[0].a, d[0].f);
                d[0].a = 1;
                d[0].f = 4'h2;
                d[1]   = '{a: 3, f: 4'h4};
                $display("d %0d %0h %0d %0h", d[0].a, d[0].f, d[1].a, d[1].f);
                r = d[1];
                $display("r %0d %0h", r.a, r.f);
                // new[n](src) copies the old elements and defaults the rest.
                e = new[3](d);
                $display("e %0d %0d %0d %0h", e.size(), e[1].a, e[2].a, e[2].f);
                // A whole-array copy is independent.
                e = d;
                e[0].a = 9;
                $display("cp %0d %0d", d[0].a, e[0].a);
                // An out-of-range read is the element default; the write is dropped.
                r = d[99];
                d[99].a = 5;
                $display("oob %0d %0h size=%0d", r.a, r.f, d.size());
                d.delete();
                $display("del %0d", d.size());
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "init 0 x\n"
                              "d 1 2 3 4\n"
                              "r 3 4\n"
                              "e 3 3 0 x\n"
                              "cp 1 9\n"
                              "oob 0 x size=2\n"
                              "del 0\n");
}

TEST_CASE("dynamic array of class handles and of struct with an array member") {
    constexpr std::string_view src = R"(
        class C;
            int v;
        endclass
        typedef struct {
            int         v;
            logic [7:0] b[2];
        } nest_t;
        module m;
            C      h[];
            nest_t n[];
            initial begin
                h = new[2];
                $display("null=%0b", h[0] == null);
                h[0] = new;
                h[0].v = 4;
                h[1] = h[0];
                h[1].v = 5;
                $display("h %0d %0d", h.size(), h[0].v);
                n = new[1];
                n[0].b[1] = 8'hAB;
                $display("n %0h %0h", n[0].b[0], n[0].b[1]);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "null=1\n"
                              "h 2 5\n"
                              "n xx ab\n");
}

// A chandle element (LRM 6.14): a plain 64-bit POD word, admitted by the same
// guard_container_element gate a class handle uses. libc malloc/free is the
// no-.so probe (no way to make a non-null chandle otherwise).
TEST_CASE("dynamic array of chandles") {
    constexpr std::string_view src = R"(
        module m;
            import "DPI-C" function chandle malloc(input longint size);
            import "DPI-C" function void    free(input chandle p);
            chandle d[];
            chandle h;
            initial begin
                d = new[2];
                $display("null0=%0b null1=%0b", d[0] == null, d[1] == null);
                h = malloc(8);
                d[0] = h;
                d[1] = d[0];
                $display("n=%0d same=%0b", d.size(), d[0] == d[1]);
                free(h);
                $finish;
            end
        endmodule
    )";
    CHECK(run_capture(src) == "null0=1 null1=1\n"
                              "n=2 same=1\n");
}
