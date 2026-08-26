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

// LRM 7.5.1's "resizeable assignment": a dynamic array can be assigned from
// another dynamic array (above), a fixed-size unpacked array, or a queue --
// each of the latter two needs its own runtime path (a fixed array has no
// handle at all, just flat storage; a queue's is ring-indexed, not linear),
// previously missing: `lower_dyn_array_assign`'s fallback fed the mismatched
// source straight to `lower_lvalue`/`dyn_array_copy`, which either hit an
// unrelated "unsupported assignment target" refusal (a fixed-array source,
// wrapped in slang's implicit Conversion node) or silently misread another
// container's handle bytes as an rt_dynarr (a queue source -- no Conversion
// node to catch it, so nothing refused it at all). No working oracle: iverilog
// 13 crashes an internal assertion on this file entirely; verilator 5.050's
// lint accepts it but its generated C++ fails to build in this environment
// (an unrelated local toolchain issue, not a rejection). Hand-checked against
// the LRM.
TEST_CASE("dynamic array assignment from a fixed-size array or a queue (LRM 7.5.1)") {
    constexpr std::string_view src = R"(
        module m;
            int f[5];
            int d[];
            int qsrc[$];
            initial begin
                f = '{1, 2, 3, 4, 5};
                d = f;
                $display("fixed s=%0d %0d %0d %0d %0d %0d", d.size(), d[0], d[1], d[2], d[3], d[4]);

                qsrc = '{10, 20, 30};
                d = qsrc;
                $display("queue s=%0d %0d %0d %0d", d.size(), d[0], d[1], d[2]);

                qsrc.delete();
                d = qsrc;
                $display("empty queue s=%0d", d.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "fixed s=5 1 2 3 4 5\n"
                              "queue s=3 10 20 30\n"
                              "empty queue s=0\n");
}

// The declaration-initializer / handle-member-store form (emit_handle_member_copy)
// had the identical bug independently: `int d[] = f;` (fixed-array source) hit a
// genuine SEGFAULT (a fixed array's flat storage read as an rt_dynarr{data,len}
// handle -- a garbage data pointer dereferenced through a garbage length), and
// `int d[] = q;` a garbage-comparison crash (a queue's ring fields read as
// rt_dynarr's two). A class-property initializer shares the same lowering.
TEST_CASE("dynamic array declaration initializer from a fixed-size array or a queue") {
    constexpr std::string_view src = R"(
        module m;
            class c;
                int fld[3] = '{1, 2, 3};
                int fromfixed[] = fld;
            endclass
            int f[3] = '{4, 5, 6};
            int fromfixed[] = f;
            int qsrc[$] = '{7, 8, 9};
            int fromqueue[] = qsrc;
            c obj;
            initial begin
                $display("fixed s=%0d %0d %0d %0d", fromfixed.size(), fromfixed[0], fromfixed[1], fromfixed[2]);
                $display("queue s=%0d %0d %0d %0d", fromqueue.size(), fromqueue[0], fromqueue[1], fromqueue[2]);
                obj = new;
                $display("class prop s=%0d %0d %0d %0d",
                         obj.fromfixed.size(), obj.fromfixed[0], obj.fromfixed[1], obj.fromfixed[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "fixed s=3 4 5 6\n"
                              "queue s=3 7 8 9\n"
                              "class prop s=3 1 2 3\n");
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

// === / !== over a dynamic array shares the queue's aggregate-equality
// reduction (both are Kind::DynArray/Kind::Queue handles with no plane
// value); a string leaf never carries x/z (LRM 6.16.9) so the X-propagating
// == walk is exact for === too.
TEST_CASE("dynamic array case-equality with a string element (LRM 11.2.2, 11.4.5)") {
    CHECK(run_capture(R"(
        module m;
            string a[], b[];
            initial begin
                a = new[2]; a[0] = "x"; a[1] = "y";
                b = new[2]; b[0] = "x"; b[1] = "y";
                $display("ceq=%b", a === b);
                b = new[3]; b[0] = "x"; b[1] = "y"; b[2] = "z";
                $display("ceq=%b cne=%b", a === b, a !== b);
            end
        endmodule
    )") == "ceq=1\nceq=0 cne=1\n");
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

// -- new[] as a declaration initializer (LRM 6.21, 7.5, 7.5.1) -----------------
//
// `T x = new[N]` / `new[N](src)` used to be refused wherever it initialized a
// declaration rather than an assignment statement -- automatic and static
// locals, module-scope variables, and class properties all hit
// "queue / dynamic array member assignment from a non-variable value" (a class
// property hit the more primitive "unsupported expression kind=NewArray").
// `emit_dyn_array_new_expr` now does the sizing / truncate / default-fill
// construction once, shared by the statement form (`arr = new[N]`,
// lower_dyn_array_assign) and every declaration-initializer form
// (emit_handle_member_copy / emit_class_property_inits). Cross-checked against
// verilator 5.050 on every case below; iverilog 13 corroborates the ones its
// parser accepts (noted per test) and is not a full oracle here (no `static`
// lifetime override, no class array-property indexing, no unpacked-struct
// `$bits` operand).

TEST_CASE("dynamic array decl initializer from a bare new[]") {
    constexpr std::string_view src = R"(
        module m;
            function int f();
                int a[] = new[3];
                $display("%0d %0d %0d %0d", a.size(), a[0], a[1], a[2]);
                a[0] = 10; a[1] = 20; a[2] = 30;
                $display("%0d %0d %0d", a[0], a[1], a[2]);
                return 0;
            endfunction
            initial begin int r; r = f(); end
        endmodule
    )";
    // Cross-checked against iverilog 13 and verilator 5.050 (both agree).
    CHECK(run_capture(src) == "3 0 0 0\n10 20 30\n");
}

// new[N](src): N > src.size() default-fills the tail, N == src.size() copies
// every element, N < src.size() truncates (LRM 7.5.1). `automatic` so each
// call re-evaluates the initializer against `s`'s CURRENT contents rather than
// capturing it once as a static local would (see the timing tests below).
TEST_CASE("dynamic array decl initializer using the new[n](src) copy-constructor form") {
    constexpr std::string_view src = R"(
        module m;
            int s[];
            function automatic int grow();
                int a[] = new[5](s);
                $display("grow %0d %0d %0d %0d %0d %0d", a.size(), a[0], a[1], a[2], a[3], a[4]);
                return 0;
            endfunction
            function automatic int same();
                int a[] = new[3](s);
                $display("same %0d %0d %0d %0d", a.size(), a[0], a[1], a[2]);
                return 0;
            endfunction
            function automatic int shrink();
                int a[] = new[2](s);
                $display("shrink %0d %0d %0d", a.size(), a[0], a[1]);
                return 0;
            endfunction
            initial begin
                int r;
                s = new[3]; s[0] = 7; s[1] = 8; s[2] = 9;
                r = grow(); r = same(); r = shrink();
            end
        endmodule
    )";
    // Cross-checked against iverilog 13 and verilator 5.050 (both agree).
    CHECK(run_capture(src) == "grow 5 7 8 9 0 0\n"
                              "same 3 7 8 9\n"
                              "shrink 2 7 8\n");
}

TEST_CASE("dynamic array decl initializer with new[0] and new[0](src)") {
    constexpr std::string_view src = R"(
        module m;
            int s[];
            function int bare0();
                int a[] = new[0];
                return a.size();
            endfunction
            function int copy0();
                int a[] = new[0](s);
                return a.size();
            endfunction
            initial begin
                s = new[3];
                $display("%0d %0d", bare0(), copy0());
            end
        endmodule
    )";
    // Cross-checked against iverilog 13 and verilator 5.050 (both agree).
    CHECK(run_capture(src) == "0 0\n");
}

// A class property's `new[]` initializer runs per-object, as part of
// construction, before any of the object's own statements (LRM 8.7 / 8.8) --
// this used to fail with the more primitive "unsupported expression
// kind=NewArray" (emit_class_property_inits had no aggregate/container gate at
// all, unlike a module-scope or static-local declaration initializer).
TEST_CASE("dynamic array new[] declaration initializer on a class property (LRM 8.7/8.8)") {
    constexpr std::string_view src = R"(
        module m;
            class c;
                int a[] = new[3];
                function void bump();
                    foreach (a[i]) a[i] = a[i] + (i + 1) * 10;
                endfunction
            endclass
            c obj;
            initial begin
                obj = new;
                $display("%0d %0d %0d %0d", obj.a.size(), obj.a[0], obj.a[1], obj.a[2]);
                obj.bump();
                $display("%0d %0d %0d", obj.a[0], obj.a[1], obj.a[2]);
            end
        endmodule
    )";
    // iverilog 13 cannot index a class array property ("Array index of array
    // properties not supported") -- verilator 5.050 only.
    CHECK(run_capture(src) == "3 0 0 0\n10 20 30\n");
}

// Each object gets its OWN array from the initializer, not a shared one (LRM
// 8.7: the initializer runs once PER OBJECT, unlike a static property's single
// shared slot).
TEST_CASE("dynamic array class property new[] initializer is per-object, not shared") {
    constexpr std::string_view src = R"(
        module m;
            class c;
                int a[] = new[2];
            endclass
            c x, y;
            initial begin
                x = new; y = new;
                x.a[0] = 11;
                y.a[0] = 22;
                $display("%0d %0d %0d %0d", x.a.size(), y.a.size(), x.a[0], y.a[0]);
            end
        endmodule
    )";
    // iverilog 13 cannot index a class array property -- verilator 5.050 only.
    CHECK(run_capture(src) == "2 2 11 22\n");
}

TEST_CASE("dynamic array class property new[n](src) copy-constructor initializer") {
    constexpr std::string_view src = R"(
        module m;
            int seed[];
            class c;
                int a[] = new[4](seed);
            endclass
            c obj;
            initial begin
                seed = new[2]; seed[0] = 5; seed[1] = 6;
                obj = new;
                $display("%0d %0d %0d %0d %0d", obj.a.size(), obj.a[0], obj.a[1], obj.a[2], obj.a[3]);
            end
        endmodule
    )";
    // iverilog 13 cannot index a class array property -- verilator 5.050 only.
    CHECK(run_capture(src) == "4 5 6 0 0\n");
}

// An AUTOMATIC declaration initializer runs on every entry to its block (LRM
// 6.21): three calls each get a freshly new[]'d array, so a mutation in one
// call is never visible to the next.
TEST_CASE("dynamic array automatic decl initializer new[] re-runs on every call (LRM 6.21)") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int f();
                int a[] = new[3];
                int prior;
                prior = a[0];
                a[0] = 77;
                return prior;
            endfunction
            initial $display("%0d %0d %0d", f(), f(), f());
        endmodule
    )";
    // Cross-checked against iverilog 13 and verilator 5.050 (both agree).
    CHECK(run_capture(src) == "0 0 0\n");
}

// A STATIC local's declaration initializer runs exactly once (LRM 6.21,
// 13.4.2): the SAME array persists and accumulates the mutation across all
// three calls, unlike the automatic case above.
TEST_CASE("dynamic array static-local decl initializer new[] runs once, storage persists (LRM 6.21)") {
    constexpr std::string_view src = R"(
        module m;
            function automatic int g();
                static int a[] = new[3];
                a[0] = a[0] + 1;
                return a[0];
            endfunction
            initial $display("%0d %0d %0d", g(), g(), g());
        endmodule
    )";
    // iverilog 13 cannot override the default variable lifetime ("Overriding
    // the default variable lifetime is not yet supported") -- verilator 5.050 only.
    CHECK(run_capture(src) == "1 2 3\n");
}

// A module-scope variable is implicitly static (LRM 6.8): its new[]
// initializer runs once, in the run-once PreInit process, before any initial
// block -- so `a` is already sized by the time this initial block reads it,
// with no assignment statement anywhere in the design.
TEST_CASE("dynamic array module-scope decl initializer new[] runs once at time zero") {
    constexpr std::string_view src = R"(
        module m;
            int a[] = new[4];
            initial $display("%0d", a.size());
        endmodule
    )";
    // Cross-checked against iverilog 13 and verilator 5.050 (both agree).
    CHECK(run_capture(src) == "4\n");
}

// The `new[N]` size expression is lowered by the ordinary expression path, not
// a constant-folder, so every expression shape already legal elsewhere in the
// language works with no special casing: a constant, `$bits` of an aggregate
// (LRM 20.7), a plain runtime argument, a function-call result, and an
// expression mixing an argument, a call and `$bits` together.
TEST_CASE("dynamic array new[] declaration initializer size expression forms") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a; int b; } pair_t;
            function automatic int extra(); return 2; endfunction
            function automatic int const_sz();
                int a[] = new[3];
                return a.size();
            endfunction
            function automatic int bits_sz();
                pair_t data;
                int a[] = new[$bits(data)];
                return a.size();
            endfunction
            function automatic int var_sz(int n);
                int a[] = new[n];
                return a.size();
            endfunction
            function automatic int call_sz();
                int a[] = new[extra()];
                return a.size();
            endfunction
            function automatic int mixed_sz(int n);
                int a[] = new[n + extra() + $bits(byte)];
                return a.size();
            endfunction
            initial $display("%0d %0d %0d %0d %0d",
                              const_sz(), bits_sz(), var_sz(5), call_sz(), mixed_sz(4));
        endmodule
    )";
    // iverilog 13 cannot parse an unpacked-struct $bits operand ("Unpacked
    // structs not supported") -- verilator 5.050 only.
    CHECK(run_capture(src) == "3 64 5 2 14\n");
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

// A dynamic-array slice (LRM 7.4.5/7.5.1) types as a FixedSizeUnpackedArrayType,
// never a queue -- unlike a queue slice, both bounds must be compile-time
// constant (no `$`, no runtime bound). Its storage is handle-indirect
// (rt_dynarr{data,len}, heap, relocatable) rather than flat/address-indexed, so
// -- unlike a fixed array's own constant-bounds slice, a live zero-copy
// ArrSlice view -- it materializes position-wise into a temp. Each position is
// an ordinary ElemSelect, so out-of-range reads/writes follow the same LRM
// 7.5.1 rule an ordinary `da[i]` already has: a read past `size()` yields the
// element default (0 for a 2-state `int`), a write there is silently dropped.
// No oracle: verilator 5.050 rejects `da[a:b]` outright ("Illegal range
// select"), and iverilog 13 crashes an internal assertion on it
// (`elab_expr.cc:5902`). Hand-checked against the LRM: the open `$` bound
// and a runtime bound in the `a:b` form are queue-only, and a constant-bounds
// slice of a dynamic array is legal (7.4.5, 7.5.1).
TEST_CASE("dynamic array constant-bounds slice read/write") {
    constexpr std::string_view src = R"(
        module m;
            int da[];
            int f3[3];
            initial begin
                da = new[8];
                foreach (da[k]) da[k] = k + 1;
                f3 = da[2:4];
                $display("read %0d %0d %0d", f3[0], f3[1], f3[2]);

                f3 = '{100, 200, 300};
                da[1:3] = f3;
                $display("write %0d %0d %0d %0d %0d", da[0], da[1], da[2], da[3], da[4]);

                // OOB: da has indices 0..7. A read past the end defaults; a
                // write there is dropped and the array does not resize.
                f3 = da[6:8];
                $display("oob-read %0d %0d %0d", f3[0], f3[1], f3[2]);
                da = new[8];
                foreach (da[k]) da[k] = k + 1;
                f3 = '{111, 222, 333};
                da[6:8] = f3;
                $display("oob-write %0d %0d size=%0d", da[6], da[7], da.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "read 3 4 5\n"
                              "write 1 100 200 300 5\n"
                              "oob-read 7 8 0\n"
                              "oob-write 111 222 size=8\n");
}

// Indexed part-select `da[i +: n]` / `da[i -: n]` (LRM 11.5.1, extended to a
// dynamic array by 7.5.1): unlike the constant-bounds form, `i` may be a
// runtime value (only `n`, the result's element count, must stay constant).
TEST_CASE("dynamic array indexed part-select slice") {
    constexpr std::string_view src = R"(
        module m;
            int da[];
            int f3[3];
            int i;
            initial begin
                da = new[8];
                foreach (da[k]) da[k] = k + 1;

                f3 = da[2 +: 3];
                $display("up-const %0d %0d %0d", f3[0], f3[1], f3[2]);
                f3 = da[5 -: 3];
                $display("down-const %0d %0d %0d", f3[0], f3[1], f3[2]);
                i = 3;
                f3 = da[i +: 3];
                $display("up-runtime %0d %0d %0d", f3[0], f3[1], f3[2]);

                // OOB via a runtime base past the end: in-range positions
                // apply, the rest are dropped.
                i = 7;
                f3 = '{11, 22, 33};
                da[i +: 3] = f3;
                $display("oob-write %0d size=%0d", da[7], da.size());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "up-const 3 4 5\n"
                              "down-const 4 5 6\n"
                              "up-runtime 4 5 6\n"
                              "oob-write 11 size=8\n");
}

// A dynamic-array slice as an equality operand, a by-value function argument,
// and an output-argument write-back target -- the contexts a slice with no
// anchored lvalue must reach through materialize_indexed_slice /
// emit_indexed_slice_copy rather than a live view. The output case is the one
// that needs `for_write` threaded to the container-side element select (a
// dynamic array is a heap handle, unlike a fixed array): without it a write
// through the slice would resolve the wrong HandleUse and, for a waited-on
// container, never notify a mutation waiter (LRM 9.4.3).
TEST_CASE("dynamic array slice as an equality operand and a subroutine argument") {
    constexpr std::string_view src = R"(
        module m;
            int da[];
            int f3[3];

            function automatic void takes_slice(input int s[3]);
                $display("arg %0d %0d %0d", s[0], s[1], s[2]);
            endfunction

            task automatic writes_out(output int s[3]);
                s = '{7, 8, 9};
            endtask

            initial begin
                da = new[8];
                foreach (da[k]) da[k] = k + 1;
                f3 = '{3, 4, 5};
                $display("eq %0d", da[2:4] == f3);
                f3 = '{3, 4, 6};
                $display("neq %0d", da[2:4] == f3);

                takes_slice(da[1:3]);

                writes_out(da[2:4]);
                $display("outarg %0d %0d %0d %0d %0d", da[0], da[1], da[2], da[3], da[4]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "eq 1\n"
                              "neq 0\n"
                              "arg 2 3 4\n"
                              "outarg 1 2 7 8 9\n");
}

// `da[0:2] = da[1:3]` (both sides slices of the SAME array, overlapping
// ranges): LRM 10.4.1 evaluates the whole RHS before any assignment happens,
// so the write must see the pre-assignment values throughout, not a value
// some earlier position of the same statement already overwrote. A dynamic
// array's slice is always independently materialized first (never a live
// view), so this falls out for free -- unlike the fixed-array live-ArrSlice
// case, which needs an explicit overlap-staging check (array_copy_may_overlap).
TEST_CASE("dynamic array overlapping slice-to-slice assignment reads pre-assignment values") {
    constexpr std::string_view src = R"(
        module m;
            int da[];
            initial begin
                da = new[8];
                foreach (da[k]) da[k] = k + 1;
                da[0:2] = da[1:3];
                $display("%0d %0d %0d %0d", da[0], da[1], da[2], da[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "2 3 4 4\n");
}
