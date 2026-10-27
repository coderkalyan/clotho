// How diagnostics are rendered: the source snippet under a Clotho refusal, the
// colour of the text, and the driver notices that would only repeat what slang's
// Driver already printed.
//
// These are library-level checks of `Diagnostic::rendered` and `diagnostic_text`.
// The command line's own behaviour (what reaches stderr, the exit status) is
// pinned as a process in cli_test.cpp.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "Diagnostic.h"
#include "Lower.h"

using namespace clotho;

namespace {

namespace fs = std::filesystem;

// A wait on a fork-captured automatic event: legal SystemVerilog Clotho refuses
// with a located `unsupported-construct` error. Line 5, column 5 is the refusal.
constexpr std::string_view kRefused = R"(module top;
  task automatic t;
    event e;
    fork begin #1 -> e; end join_none
    wait (e.triggered);
  endtask
  initial t();
endmodule
)";

constexpr std::string_view kEsc = "\x1b[";

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// A directory of design files, removed with the test.
struct Files {
    fs::path dir;

    Files() : dir(fs::temp_directory_path() / ("clotho_diag_render_" + std::to_string(counter()++))) {
        fs::create_directories(dir);
    }
    ~Files() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Files(const Files&)            = delete;
    Files& operator=(const Files&) = delete;

    std::string write(std::string_view name, std::string_view text) const {
        const fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p.string();
    }

    static int& counter() {
        static int n = 0;
        return n;
    }
};

LowerResult lower_args(const std::vector<std::string>& args) {
    const std::vector<std::string_view> views(args.begin(), args.end());
    return lower_sources(views);
}

} // namespace

TEST_CASE("a Clotho refusal carries its source line and a caret") {
    const LowerResult r = lower_text(kRefused);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.diagnostics.size() == 1);
    const Diagnostic& d = r.diagnostics[0];
    CHECK(d.origin == DiagnosticOrigin::Unsupported);
    CHECK(contains(d.rendered, ":5:5: error: "));
    CHECK(contains(d.rendered, "[unsupported-construct]"));
    CHECK(contains(d.rendered, "\n    wait (e.triggered);\n    ^"));
    CHECK(d.rendered == r.diagnostic_text());
}

TEST_CASE("text lowered for an editor is never coloured") {
    const LowerResult r = lower_text(kRefused);
    REQUIRE_FALSE(r.diagnostics.empty());
    for (const Diagnostic& d : r.diagnostics) {
        CHECK_FALSE(contains(d.rendered, kEsc));
    }
    CHECK_FALSE(contains(r.diagnostic_text(), kEsc));

    // A slang item, too: the buffer entry point is what the language server calls.
    const LowerResult slang_error = lower_buffer("a.sv", "module m;\n  initial a = b + 1;\nendmodule\n");
    REQUIRE_FALSE(slang_error.diagnostics.empty());
    CHECK_FALSE(contains(slang_error.diagnostic_text(), kEsc));
}

TEST_CASE("--color-diagnostics colours slang's items and Clotho's, and absence leaves both plain") {
    const Files        f;
    const std::string  refused = f.write("refused.sv", kRefused);
    const std::string  unused  = f.write("unused.sv", "module m;\n  int unused_var;\nendmodule\n");

    SUBCASE("a refusal") {
        const LowerResult plain = lower_args({ refused });
        REQUIRE_FALSE(plain.ok);
        CHECK_FALSE(contains(plain.diagnostic_text(), kEsc));

        const LowerResult colour = lower_args({ "--color-diagnostics", refused });
        REQUIRE_FALSE(colour.ok);
        CHECK(contains(colour.diagnostic_text(), kEsc));
        CHECK(contains(colour.diagnostics[0].message, "wait on the fork-captured"));
        CHECK_FALSE(contains(colour.diagnostics[0].message, kEsc));
    }
    SUBCASE("a slang warning") {
        const LowerResult plain = lower_args({ unused });
        REQUIRE(plain.ok);
        REQUIRE_FALSE(plain.diagnostics.empty());
        CHECK_FALSE(contains(plain.diagnostic_text(), kEsc));

        const LowerResult colour = lower_args({ "--color-diagnostics", unused });
        REQUIRE(colour.ok);
        CHECK(contains(colour.diagnostic_text(), kEsc));
        // Colour is presentation only: the same item, the same structured fields.
        REQUIRE(colour.diagnostics.size() == plain.diagnostics.size());
        CHECK(colour.diagnostics[0].message == plain.diagnostics[0].message);
        CHECK(colour.diagnostics[0].code == plain.diagnostics[0].code);
    }
}

TEST_CASE("--diag-source, --diag-column and --diag-location shape a refusal like a slang error") {
    const Files        f;
    const std::string  refused = f.write("refused.sv", kRefused);

    SUBCASE("--diag-source=false drops the snippet") {
        const LowerResult r = lower_args({ "--diag-source=false", refused });
        REQUIRE_FALSE(r.ok);
        CHECK(contains(r.diagnostic_text(), ":5:5: error: "));
        CHECK_FALSE(contains(r.diagnostic_text(), "wait (e.triggered);"));
    }
    SUBCASE("--diag-column=false drops the column") {
        const LowerResult r = lower_args({ "--diag-column=false", refused });
        REQUIRE_FALSE(r.ok);
        CHECK(contains(r.diagnostic_text(), ":5: error: "));
        CHECK(contains(r.diagnostic_text(), "wait (e.triggered);"));
    }
    SUBCASE("--diag-location=false drops the location") {
        const LowerResult r = lower_args({ "--diag-location=false", refused });
        REQUIRE_FALSE(r.ok);
        CHECK(r.diagnostic_text().starts_with("error: "));
    }
    SUBCASE("--diag-option=false drops the code") {
        const LowerResult r = lower_args({ "--diag-option=false", refused });
        REQUIRE_FALSE(r.ok);
        CHECK_FALSE(contains(r.diagnostic_text(), "[unsupported-construct]"));
        CHECK(r.diagnostics[0].code == "unsupported-construct");
    }
}

TEST_CASE("a notice for a failure the Driver already printed is kept but not repeated") {
    const Files        f;
    const std::string  missing = (f.dir / "missing.sv").string();

    const LowerResult r = lower_args({ missing });
    CHECK_FALSE(r.ok);
    // Still an error, so a caller keying on it (the exit status) is unchanged.
    CHECK(has_error(r.diagnostics));
    REQUIRE_FALSE(r.diagnostics.empty());
    CHECK(r.diagnostics[0].reported);
    // But rendering says nothing: the Driver's "error: '...': No such file or
    // directory" is the whole message.
    CHECK(r.diagnostic_text().empty());
}

TEST_CASE("an invalid --top is reported once, not again as a missing top") {
    const Files        f;
    const std::string  two = f.write("two.sv", "module a; endmodule\nmodule b; endmodule\n");

    const LowerResult r = lower_args({ two, "--top", "c" });
    CHECK_FALSE(r.ok);
    CHECK(has_error(r.diagnostics));
    CHECK(contains(r.diagnostic_text(), "'c' is not a valid top-level module"));
    CHECK_FALSE(contains(r.diagnostic_text(), "no top-level modules found"));
    for (const Diagnostic& d : r.diagnostics) {
        CHECK(d.code != "missing-top");
    }
}

TEST_CASE("a design with no top at all still says so") {
    const Files        f;
    const std::string  pkg = f.write("pkg.sv", "package p; endpackage\n");

    // Only the redundant case is dropped: with no invalid --top to blame, the
    // missing-top warning is the one sentence that explains an empty design.
    const LowerResult r = lower_args({ pkg });
    CHECK(contains(r.diagnostic_text(), "no top-level modules found in design"));
}
