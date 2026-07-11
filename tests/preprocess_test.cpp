#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

namespace fs = std::filesystem;

// A scratch directory that writes source files to disk (needed to exercise
// `` `include `` search, which cannot run from an in-memory buffer) and removes
// them on destruction.
struct ScratchDir {
    fs::path root;
    ScratchDir()
      : root(fs::temp_directory_path() /
             fs::path("clotho_pp_" + std::to_string(::getpid()))) {
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

// Lower the on-disk files with the given front-end options, JIT, and capture
// stdout. Mirrors run_capture in the backend tests.
std::string run_files(const std::vector<std::string>& paths, const FrontendOptions& fe) {
    std::vector<std::string_view> views(paths.begin(), paths.end());
    const LowerResult             r = lower_files(views, fe);
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
    ScratchDir  dir;
    const auto  top = dir.write("top.sv", R"(
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

    SUBCASE("defined") {
        FrontendOptions fe;
        fe.defines = {"FOO"};
        CHECK(run_files({top.string()}, fe) == "foo_on\n");
    }
    SUBCASE("undefined") {
        CHECK(run_files({top.string()}, {}) == "foo_off\n");
    }
}

TEST_CASE("command-line define carries a value") {
    ScratchDir dir;
    const auto top = dir.write("top.sv", R"(
module m;
  initial $display("w=%0d", `WIDTH);
endmodule
)");
    FrontendOptions fe;
    fe.defines = {"WIDTH=12"};
    CHECK(run_files({top.string()}, fe) == "w=12\n");
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
    FrontendOptions fe;
    fe.include_dirs = {(dir.root / "inc").string()};
    CHECK(run_files({top.string()}, fe) == "k=7\n");
}

TEST_CASE("include without a matching search dir fails loudly") {
    ScratchDir dir;
    const auto top = dir.write("top.sv", R"(
module m;
`include "missing.svh"
  initial $display("unreachable");
endmodule
)");
    const std::string             path  = top.string();
    std::vector<std::string_view> views = {path};
    const LowerResult             r     = lower_files(views, {});
    CHECK_FALSE(r.ok);
}
