#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "Lower.h"
#include "ir/Jit.h"

#include <cstdio>
#include <unistd.h>

using namespace clotho;

namespace {

// Run a design and capture its stdout (see two_state_test.cpp for the same
// redirect dance).
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

// Assert `source` is rejected with a diagnostic containing `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostics.find(needle) != std::string::npos, r.diagnostics);
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

    const std::array<std::string_view, 2> paths{ pkg_file.path.native(), top_file.path.native() };
    const LowerResult                     r = lower_files(paths);
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

    CHECK(out == "m1 W=8 b=fe double=8\nm2 W=8 double=10\n");
}
