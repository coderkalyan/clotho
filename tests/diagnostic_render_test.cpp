// How diagnostics are rendered: the driver notices that would only repeat what
// slang's Driver already printed.
//
// These are library-level checks of `diagnostic_text` and `has_error`.
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
