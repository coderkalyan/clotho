#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

namespace fs = std::filesystem;

// A scratch directory that writes source files to disk (slang's Driver loads
// files from disk, and `` `include `` search needs real paths) and removes them
// on destruction.
struct ScratchDir {
    fs::path root;
    ScratchDir()
      : root(fs::temp_directory_path() /
             fs::path("clotho_pp_" + std::to_string(::getpid()))) {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    // Write `contents` to `rel` (creating parent dirs) and return the full path.
    fs::path write(const fs::path& rel, std::string_view contents) const {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << contents;
        return p;
    }
};

// Lower via the slang-Driver entry (source files + preprocessor flags exactly
// as the CLI passes them), JIT, and capture stdout. Mirrors run_capture in the
// backend tests.
std::string run_args(const std::vector<std::string>& args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    const LowerResult             r = lower_sources(views);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module, nullptr, {}, {});

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

// -- `+define` / `-D` macro predefines (LRM 22.5.1) ---------------------

TEST_CASE("command-line define selects an `ifdef branch") {
    ScratchDir dir;
    const auto top = dir.write("top.sv", R"(
module m;
  initial begin
`ifdef FOO
    $display("foo_on");
`else
    $display("foo_off");
`endif
  end
endmodule
)");
    const std::string tp = top.string();

    SUBCASE("-D form") { CHECK(run_args({"-DFOO", tp}) == "foo_on\n"); }
    SUBCASE("+define+ form") { CHECK(run_args({"+define+FOO", tp}) == "foo_on\n"); }
    SUBCASE("undefined") { CHECK(run_args({tp}) == "foo_off\n"); }
}

TEST_CASE("command-line define carries a value") {
    ScratchDir dir;
    const auto top = dir.write("top.sv", R"(
module m;
  initial $display("w=%0d", `WIDTH);
endmodule
)");
    const std::string tp = top.string();
    CHECK(run_args({"-DWIDTH=12", tp}) == "w=12\n");
    CHECK(run_args({"+define+WIDTH=9", tp}) == "w=9\n");
}

// -- `+incdir` / `-I` include search dirs (LRM 22.4) --------------------

TEST_CASE("include dir resolves a `include from a search path") {
    ScratchDir dir;
    dir.write("inc/defs.svh", "localparam int K = 7;\n");
    const auto top = dir.write("top.sv", R"(
module m;
`include "defs.svh"
  initial $display("k=%0d", K);
endmodule
)");
    const std::string tp     = top.string();
    const std::string incdir = (dir.root / "inc").string();
    CHECK(run_args({"-I" + incdir, tp}) == "k=7\n");
    CHECK(run_args({"+incdir+" + incdir, tp}) == "k=7\n");
}

TEST_CASE("include without a matching search dir fails loudly") {
    ScratchDir dir;
    const auto top = dir.write("top.sv", R"(
module m;
`include "missing.svh"
  initial $display("unreachable");
endmodule
)");
    const std::string             tp    = top.string();
    std::vector<std::string_view> views = {tp};
    const LowerResult             r     = lower_sources(views);
    CHECK_FALSE(r.ok);
}

// -- single compilation unit (LRM 3.12) ---------------------------------

// Files are merged into one CU, so a macro defined in one file is visible in a
// later one. This is the chosen default (docs/TODO.md); under multi-unit it
// would be an undeclared-macro error.
TEST_CASE("macro defined in one file is visible in a later file (single unit)") {
    ScratchDir dir;
    const auto a = dir.write("a.sv", "`define SHARED 42\n");
    const auto b = dir.write("b.sv", R"(
module m;
  initial $display("s=%0d", `SHARED);
endmodule
)");
    CHECK(run_args({a.string(), b.string()}) == "s=42\n");
}
