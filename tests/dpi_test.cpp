#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

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

// Lower a design expected to fail a DPI guard; return the rendered diagnostics.
std::string lower_fail(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

// An `int` DPI import maps to C `int` (i32) by value: the SV call passes the
// value plane and reads the C return straight back (LRM 35.5.6). libc `abs`.
TEST_CASE("DPI import: int argument and return") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function int abs(input int x);
          initial $display("%0d", abs(-7) + 1);
        endmodule)";
    CHECK(run_capture(src) == "8\n"); // abs(-7) == 7, used in arithmetic
}

// A `longint` maps to C `long long` (i64). libc `llabs`.
TEST_CASE("DPI import: longint round-trips 64 bits") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function longint llabs(input longint x);
          initial $display("%0d", llabs(-64'd5000000000));
        endmodule)";
    CHECK(run_capture(src) == "5000000000\n");
}

// `real` maps to C `double`; the value rides the fp form across the boundary.
// libm `sqrt` / `pow`, result narrowed to int to keep the assertion exact.
TEST_CASE("DPI import: real argument and return") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function real sqrt(input real x);
          import "DPI-C" function real pow(input real b, input real e);
          initial begin
            $display("%0d", int'(sqrt(144.0)));
            $display("%0d", int'(pow(2.0, 10.0)));
          end
        endmodule)";
    CHECK(run_capture(src) == "12\n1024\n");
}

// A `pure` import (LRM 35.6) is marked readnone/willreturn; it must still
// produce the right value. abs is effectively pure.
TEST_CASE("DPI import: pure function runs correctly") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" pure function int abs(input int x);
          initial $display("%0d", abs(-4));
        endmodule)";
    CHECK(run_capture(src) == "4\n");
}

TEST_CASE("DPI import: output argument") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function real frexp(input real x, output int e);
          initial begin
            real mant; int ex;
            mant = frexp(8.0, ex);
            $display("%0d %0d", ex, int'(mant * 1000.0));
          end
        endmodule)";
    CHECK(run_capture(src) == "4 500\n");
}

// A `chandle` (LRM 6.14) round-trips an opaque C pointer: it is lowered as a
// 2-state i64 and marshaled to/from `void*`. libc `malloc`/`free` are the ideal
// no-.so probe — malloc returns a chandle, free takes one; `null` / `==` / `!=`
// are integer ops on the handle bits.
TEST_CASE("DPI import: chandle round-trips an opaque pointer") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function chandle malloc(input longint size);
          import "DPI-C" function void    free(input chandle p);
          chandle h;
          initial begin
            h = malloc(64);
            $display("%b", h != null);   // malloc(64) succeeds -> non-null handle
            free(h);
            h = null;
            $display("%b", h == null);    // 1
          end
        endmodule)";
    CHECK(run_capture(src) == "1\n1\n");
}

TEST_CASE("DPI import: input string passes a NUL-terminated char*") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" function longint strlen(input string s);
          initial begin
            $display("%0d", strlen("hello world"));   // 11
            $display("%0d", strlen(""));               // 0 (empty-string NUL)
          end
        endmodule)";
    CHECK(run_capture(src) == "11\n0\n");
}

// Out-of-scope constructs must fail loudly with a TODO, never silently miscompile.
// Each is a distinct guard in build_dpi_import.
TEST_CASE("DPI import: unsupported forms are rejected") {
    SUBCASE("output string argument (unbounded-buffer hazard, deferred)") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" function void f(output string s);
              string y;
              initial f(y);
            endmodule)";
        CHECK(lower_fail(src).find("output/inout string") != std::string::npos);
    }
    SUBCASE("DPI import task") {
        constexpr std::string_view src = R"(
            module m;
              import "DPI-C" task t(input int x);
              initial t(1);
            endmodule)";
        CHECK(lower_fail(src).find("DPI import tasks") != std::string::npos);
    }
}

// `context` (LRM 35.5.3) is accepted and lowers like a plain import — Clotho's
// single flat design state needs no svScope. Here it lowers and runs (libc strlen).
TEST_CASE("DPI import: context is accepted") {
    constexpr std::string_view src = R"(
        module m;
          import "DPI-C" context function longint strlen(input string s);
          initial $display("%0d", strlen("hello"));   // 5
        endmodule)";
    CHECK(run_capture(src) == "5\n");
}

// export "DPI-C" (LRM 35.5): the SV function is exposed as a C-callable thunk and
// still works as an ordinary SV function. The full C-calls-into-SV round-trip
// needs a .o linked via addObjectFile, so it is the verilator CLI cross-check
// (docs/design/dpi-c.md); here we confirm the export directive lowers, the thunk
// emits, and the design runs.
TEST_CASE("DPI export: a design with an export directive lowers and runs") {
    constexpr std::string_view src = R"(
        module m;
          export "DPI-C" function dbl;
          function int dbl(input int x); return x * 2; endfunction
          initial $display("%0d", dbl(21));   // SV-side call still works: 42
        endmodule)";
    CHECK(run_capture(src) == "42\n");
}

// Export out-of-scope forms loud-fail (collect_dpi_exports guards).
TEST_CASE("DPI export: unsupported forms are rejected") {
    SUBCASE("exported task") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" task t;
              task t(input int x); endtask
            endmodule)";
        CHECK(lower_fail(src).find("DPI export tasks") != std::string::npos);
    }
    SUBCASE("exported output argument") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" function f;
              function void f(output int x); x = 1; endfunction
            endmodule)";
        CHECK(lower_fail(src).find("output/inout") != std::string::npos);
    }
    SUBCASE("exported packed-vector argument (deferred)") {
        constexpr std::string_view src = R"(
            module m;
              export "DPI-C" function g;
              function int g(input bit [7:0] v); return v; endfunction
            endmodule)";
        CHECK(lower_fail(src).find("not supported yet") != std::string::npos);
    }
}
