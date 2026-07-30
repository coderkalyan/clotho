#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

#include <cstdio>

using namespace clotho;

namespace {

// Run a design and capture its stdout (see two_state_test.cpp for the same
// redirect dance).
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

// Assert `source` is rejected with a diagnostic containing `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

// Writes `text` to a fresh temp file and returns its path; the file is removed
// by RAII on scope exit (same pattern as lower_test.cpp's multi-file case).
struct TempSource {
    std::filesystem::path path;
    explicit TempSource(std::string_view name, std::string_view text)
      : path(std::filesystem::temp_directory_path() / name) {
        std::ofstream(path) << text;
    }
    ~TempSource() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

// A package's typedef and parameter are ordinary scope members (LRM 26.2);
// `import p::*;` resolves them transparently via slang's own lookup, and
// Clotho's lower_type/lower_constant are scope-agnostic (they only ever see
// the resolved Type/ConstantValue, never the declaring scope), so this needs
// no package-specific code today.
TEST_CASE("package typedef and parameter via wildcard import") {
    const std::string out = run_capture(R"(
        package p;
            parameter int W = 8;
            typedef logic [7:0] byte_t;
        endpackage
        module m;
            import p::*;
            byte_t b;
            initial begin
                b = 8'hAB;
                $display("W=%0d b=%h", W, b);
            end
        endmodule
    )");
    CHECK(out == "W=8 b=ab\n");
}

// A package function is lowered lazily at its first call site (like a
// module-scope function), keyed only by its SubroutineSymbol pointer — the
// declaring scope never enters the lookup.
TEST_CASE("package function called via wildcard import") {
    const std::string out = run_capture(R"(
        package p;
            function automatic int double_it(int x);
                return x * 2;
            endfunction
        endpackage
        module m;
            import p::*;
            initial $display("%0d", p::double_it(21));
        endmodule
    )");
    CHECK(out == "42\n");
}

// Package-scope enums surface the same TransparentMember/EnumValueSymbol
// members a module-scope enum would.
TEST_CASE("package enum via wildcard import") {
    const std::string out = run_capture(R"(
        package p;
            typedef enum logic [1:0] {RED, GREEN, BLUE} color_e;
        endpackage
        module m;
            import p::*;
            color_e c;
            initial begin
                c = GREEN;
                $display("c=%0d", c);
            end
        endmodule
    )");
    CHECK(out == "c=1\n");
}

// Explicit single-name import (`import p::W;`) makes only that name directly
// visible; a sibling package member still needs its own import or a scope-
// resolved reference.
TEST_CASE("explicit single-name import") {
    const std::string out = run_capture(R"(
        package p;
            parameter int W = 8;
            parameter int H = 4;
        endpackage
        module m;
            import p::W;
            initial $display("W=%0d H=%0d", W, p::H);
        endmodule
    )");
    CHECK(out == "W=8 H=4\n");
}

// A name not brought in by any import is simply undeclared — this is a
// slang-level lookup failure (diag::UndeclaredIdentifier), not anything
// Clotho's lowering needs to guard: proves imports are genuinely selective,
// not an accidental wildcard-everything.
TEST_CASE("unimported package member is not visible unqualified") {
    check_rejected(R"(
        package p;
            parameter int W = 8;
            parameter int H = 4;
        endpackage
        module m;
            import p::W;
            initial $display("%0d", H);
        endmodule
    )",
                   "undeclared identifier");
}

// Scope resolution `pkg::name` (LRM 26.3) needs no `import` at all: it is a
// distinct lookup path (Lookup::qualified over a ScopedNameSyntax) that
// resolves directly against the named package.
TEST_CASE("pkg::name scope resolution without any import") {
    const std::string out = run_capture(R"(
        package p;
            parameter int W = 8;
            typedef enum logic [1:0] {RED, GREEN, BLUE} color_e;
        endpackage
        module m;
            initial $display("W=%0d GREEN=%0d", p::W, p::GREEN);
        endmodule
    )");
    CHECK(out == "W=8 GREEN=1\n");
}

// A package is compiled once and shared: two modules importing the same
// package resolve the same typedef/parameter/function, and multi-file
// compilation (package in one file, consumer in another) needs only the
// existing lower_files / shared-Compilation path.
TEST_CASE("package shared across files and across multiple importing modules") {
    TempSource pkg_file("clotho_pkg_test_pkg.sv", R"(
        package p;
            parameter int W = 8;
            typedef logic [7:0] byte_t;
            function automatic int double_it(int x);
                return x * 2;
            endfunction
        endpackage
    )");
    TempSource top_file("clotho_pkg_test_top.sv", R"(
        module m1;
            import p::*;
            byte_t b;
            initial begin
                b = 8'hFE;
                $display("m1 W=%0d b=%h double=%0d", W, b, double_it(4));
            end
        endmodule
        module m2;
            import p::*;
            initial $display("m2 W=%0d double=%0d", W, double_it(5));
        endmodule
    )");

    // .string(), not .native(): path::string_type is wstring on Windows.
    const std::string                     pkg_path{ pkg_file.path.string() };
    const std::string                     top_path{ top_file.path.string() };
    const std::array<std::string_view, 2> paths{ pkg_path, top_path };
    const LowerResult                     r = lower_files(paths);
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

    CHECK(out == "m1 W=8 b=fe double=8\nm2 W=8 double=10\n");
}

// A package-scope variable (LRM 6.8) is genuine persistent state, unlike a
// typedef/parameter/function: it needs its own signal (collect_global_scope_signals)
// and its declaration initializer must run exactly once, in the shared PreInit
// process lower_globals builds before any top instance is lowered.
TEST_CASE("package-scope variable: read, write, and declaration initializer") {
    const std::string out = run_capture(R"(
        package p;
            int counter = 100;
        endpackage
        module m;
            import p::*;
            initial begin
                $display("initial=%0d", counter);
                counter = counter + 1;
                $display("after=%0d", counter);
            end
        endmodule
    )");
    CHECK(out == "initial=100\nafter=101\n");
}

// A `$unit`-scope variable (declared outside any module, in the compilation
// unit itself) is visible without any import — it hits the exact same
// collect_global_scope_signals path as a package variable, just walked off
// RootSymbol::compilationUnits instead of Compilation::getPackages().
TEST_CASE("$unit-scope variable is visible without any import") {
    const std::string out = run_capture(R"(
        int global_var;
        module m;
            initial begin
                global_var = 7;
                $display("global_var=%0d", global_var);
            end
        endmodule
    )");
    CHECK(out == "global_var=7\n");
}

// A package variable has no instance multiplicity (LRM 6.8): two top-level
// modules importing the same package share the exact same signal, not one
// copy per top. m2 reads after a #1 delay so its read is ordered after m1's
// time-zero write, regardless of same-instant top-to-top scheduling order.
TEST_CASE("package variable is one shared signal across multiple top-level modules") {
    const std::string out = run_capture(R"(
        package p;
            int shared_var;
        endpackage
        module m1;
            import p::*;
            initial shared_var = 42;
        endmodule
        module m2;
            import p::*;
            initial begin
                #1;
                $display("m2 sees shared_var=%0d", shared_var);
            end
        endmodule
    )");
    CHECK(out == "m2 sees shared_var=42\n");
}

// A package that itself imports another package (LRM 26.3) can use the
// imported names in its own declarations (parameter expressions, function
// bodies); this is ordinary transparent import resolution one level removed,
// needing nothing package-specific in Clotho.
TEST_CASE("package importing another package: names usable in its own declarations") {
    const std::string out = run_capture(R"(
        package p;
            parameter int W = 8;
        endpackage
        package q;
            import p::*;
            parameter int W2 = W * 2;
            function automatic int scaled(int x);
                return x * W;
            endfunction
        endpackage
        module m;
            import q::*;
            initial $display("W2=%0d scaled=%0d", W2, scaled(3));
        endmodule
    )");
    CHECK(out == "W2=16 scaled=24\n");
}

// A package-internal import is not transitively visible through a further
// import of that package (LRM 26.3/26.4): `q`'s own use of `p::W` does not
// make `W` itself reachable via `import q::*;` without an explicit `export`.
// Verified vs iverilog (same "unable to bind" / undeclared-identifier
// failure).
TEST_CASE("package-internal import does not leak without export") {
    check_rejected(R"(
        package p;
            parameter int W = 8;
        endpackage
        package q;
            import p::*;
            parameter int W2 = W * 2;
        endpackage
        module m;
            import q::*;
            initial $display("%0d", W);
        endmodule
    )",
                   "undeclared identifier");
}

// `export pkg::*;` (LRM 26.4) makes a re-exported name reachable through a
// further `import`, when the exporting package's own body also references
// that name somewhere. slang resolves it that way,
// and the fully general case has no independent confirmation.
TEST_CASE("export re-export via a further import") {
    const std::string out = run_capture(R"(
        package p;
            parameter int W = 8;
        endpackage
        package q;
            import p::*;
            export p::*;
            parameter int W2 = W * 2;
        endpackage
        module m;
            import q::*;
            initial $display("W=%0d W2=%0d", W, W2);
        endmodule
    )");
    CHECK(out == "W=8 W2=16\n");
}

// A package-scope function has no instance multiplicity (LRM 6.8): its
// static local must be exactly one persistent storage location shared by
// every top-level module that calls it, not one copy per top ("Wall A" —
// each top used to get its own fresh Lowerer with its own fnmap_/sigmap_, so
// the same package function got lowered — and its static local's signal
// created — once per top, independently). m2 waits #1 so its call is ordered
// after m1's two time-zero calls. Verified vs iverilog.
TEST_CASE("package function's static local is shared across top-level modules") {
    const std::string out = run_capture(R"(
        package p;
            function int counter();
                static int n;
                n++;
                return n;
            endfunction
        endpackage
        module m1;
            import p::*;
            initial $display("m1 counter=%0d counter=%0d", counter(), counter());
        endmodule
        module m2;
            import p::*;
            initial #1 $display("m2 counter=%0d", counter());
        endmodule
    )");
    CHECK(out == "m1 counter=1 counter=2\nm2 counter=3\n");
}
