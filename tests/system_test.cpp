#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"
#include "ir/SimConfig.h"

#include <cstdio>
#include <string>

// $system shell-exec task (LRM §20.17.1). Clotho takes a deliberate, security-
// motivated exception to LRM compliance here: $system is refused by default and
// gated behind an explicit --allow-system=<const|full> operator opt-in. These tests pin the three access tiers, both call forms, and
// the raw system() return convention.

using namespace clotho;

namespace {

// Lower `source` at the given access tier and JIT it, capturing everything the
// design writes to stdout. `tier` is threaded into BOTH the compile-time gate
// (lower_text) and the runtime backstop (run_jit's config).
std::string run_capture(std::string_view source, ir::SystemAccess tier) {
    const LowerResult r = lower_text(source, nullptr, tier);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::SimulationConfig cfg;
    cfg.allow_system = tier;
    ir::run_jit(*r.module, nullptr, cfg);

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

// Lower at `tier` and expect a loud compile-time failure; return the diagnostic.
std::string lower_error(std::string_view source, ir::SystemAccess tier) {
    const LowerResult r = lower_text(source, nullptr, tier);
    REQUIRE_FALSE(r.ok);
    return r.diagnostics;
}

} // namespace

// -- Tier gating ------------------------------------------------------------

TEST_CASE("$system is refused at compile time without --allow-system") {
    const std::string diag = lower_error(
      "module m; initial begin $system(\"true\"); $finish; end endmodule",
      ir::SystemAccess::Off);
    CHECK(diag.find("$system requires --allow-system") != std::string::npos);
}

TEST_CASE("$system const tier requires a constant command string") {
    // A string literal constant-folds -> allowed.
    const std::string out = run_capture(
      "module m; initial begin $system(\"true\"); $display(\"done\"); $finish; end endmodule",
      ir::SystemAccess::Const);
    CHECK(out == "done\n");

    // A runtime variable does not fold -> refused, steering the user to full.
    const std::string diag = lower_error(
      "module m; string s = \"true\"; initial begin $system(s); $finish; end endmodule",
      ir::SystemAccess::Const);
    CHECK(diag.find("requires a constant command string") != std::string::npos);
    CHECK(diag.find("--allow-system=full") != std::string::npos);
}

TEST_CASE("$system full tier accepts a runtime-computed command") {
    // A string built at runtime is rejected by const but runs under full.
    const std::string src =
      "module m; string s; initial begin s = \"true\"; $system(s);"
      " $display(\"ran\"); $finish; end endmodule";
    lower_error(src, ir::SystemAccess::Const);
    CHECK(run_capture(src, ir::SystemAccess::Full) == "ran\n");
}

// -- Semantics --------------------------------------------------------------

TEST_CASE("$system function form returns the raw system() status") {
    // LRM §20.17.1: the function form returns the C system() return, a POSIX
    // wait-status. "exit 3" -> WEXITSTATUS 3, encoded as 3<<8 = 768; "exit 0"
    // -> 0. The value follows directly from the C system() the LRM defers to;
    // iverilog 13 has no $system support (neither task nor function form), so it
    // is not an oracle here.
    const std::string out = run_capture(
      "module m; initial begin int a, b;"
      " a = $system(\"exit 3\"); b = $system(\"exit 0\");"
      " $display(\"%0d %0d\", a, b); $finish; end endmodule",
      ir::SystemAccess::Full);
    CHECK(out == "768 0\n");
}

TEST_CASE("$system no-arg form probes for a shell") {
    // $system() calls system(NULL), which returns nonzero when a shell exists.
    const std::string out = run_capture(
      "module m; initial begin int r; r = $system();"
      " $display(\"shell=%0d\", r != 0); $finish; end endmodule",
      ir::SystemAccess::Full);
    CHECK(out == "shell=1\n");
}

TEST_CASE("$system task form runs the command for its side effect") {
    // Touch a unique marker file, then confirm it was created. Exercises the
    // task-statement path (return value discarded) and a real exec side effect.
    const std::string marker =
      "/tmp/clotho_system_test_" + std::to_string(getpid()) + ".marker";
    std::remove(marker.c_str());

    const std::string src = "module m; initial begin $system(\"touch " + marker +
                            "\"); $finish; end endmodule";
    run_capture(src, ir::SystemAccess::Full);

    CHECK(access(marker.c_str(), F_OK) == 0);
    std::remove(marker.c_str());
}
