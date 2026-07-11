#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// Captured output of a JIT run, split by stream. Severity reports ($info /
// $warning / $error / $fatal and immediate-assertion failures) go to stderr
// (simulator diagnostics, like the $finish banner); $display goes to stdout, so
// a pass/fail action block's own output lands there. Behavior below was
// cross-checked against Icarus Verilog while developing these cases.
struct Captured {
    std::string out;
    std::string err;
};

std::string drain(std::FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string s;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    return s;
}

Captured run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    std::FILE* out_tmp = std::tmpfile();
    std::FILE* err_tmp = std::tmpfile();
    REQUIRE(out_tmp != nullptr);
    REQUIRE(err_tmp != nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(out_tmp), fileno(stdout));
    dup2(fileno(err_tmp), fileno(stderr));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    Captured c{ drain(out_tmp), drain(err_tmp) };
    std::fclose(out_tmp);
    std::fclose(err_tmp);
    return c;
}

// Lower and return the rendered diagnostics; ok must be false (a rejected design).
std::string reject(std::string_view source) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    return r.diagnostics;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

} // namespace

// -- Severity system tasks (LRM 20.10) --

TEST_CASE("severity tasks render label + formatted message to stderr") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  int x = 5;\n"
                                   "  $info(\"info x=%0d\", x);\n"
                                   "  $warning(\"warn\");\n"
                                   "  $error(\"err x=%0d\", x);\n"
                                   "end endmodule");
    CHECK(contains(c.err, "INFO: "));
    CHECK(contains(c.err, "info x=5"));
    CHECK(contains(c.err, "WARNING: "));
    CHECK(contains(c.err, "warn"));
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "err x=5"));
    // Severity reports never pollute the design's stdout stream.
    CHECK(c.out.empty());
}

TEST_CASE("bare $error with no message prints an empty message and continues") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $error;\n"
                                   "  $display(\"after\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "ERROR: "));
    // The task is non-terminating: the following $display still runs.
    CHECK(contains(c.out, "after"));
}

TEST_CASE("$fatal reports then terminates like $finish") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $fatal(0, \"boom %0d\", 7);\n"
                                   "  $display(\"unreachable\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "FATAL: "));
    CHECK(contains(c.err, "boom 7"));
    // $fatal ends the simulation; the trailing $display never executes.
    CHECK_FALSE(contains(c.out, "unreachable"));
}

TEST_CASE("$fatal finish-number >= 1 also prints the $finish banner") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  $fatal(1, \"stop\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "FATAL: "));
    CHECK(contains(c.err, "stop"));
    CHECK(contains(c.err, "$fatal called at"));
}

// -- Immediate assertions (LRM 16.3): assert / assume --

TEST_CASE("immediate assert runs the pass action when true, no report") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (1) $display(\"PASS\"); else $display(\"FAIL\");\n"
                                   "end endmodule");
    CHECK(contains(c.out, "PASS"));
    CHECK_FALSE(contains(c.out, "FAIL"));
    CHECK(c.err.empty());
}

TEST_CASE("immediate assert runs the else action when false") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (0) $display(\"PASS\"); else $error(\"bad\");\n"
                                   "end endmodule");
    CHECK_FALSE(contains(c.out, "PASS"));
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "bad"));
}

TEST_CASE("failing immediate assert with no else emits the default error report") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (0);\n"
                                   "  $display(\"after\");\n"
                                   "end endmodule");
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "assertion failed"));
    // The default failure is error severity, non-terminating.
    CHECK(contains(c.out, "after"));
}

TEST_CASE("assume checks like assert: silent when true, reports when false") {
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assume (1);\n"
                                   "  assume (0);\n"
                                   "end endmodule");
    // Exactly one report — only the false assume fires.
    CHECK(contains(c.err, "ERROR: "));
    CHECK(contains(c.err, "assertion failed"));
    CHECK_EQ(c.err.find("ERROR: "), c.err.rfind("ERROR: "));
}

TEST_CASE("assert condition truth follows if-semantics (any bit set is true)") {
    // 2'b10 is nonzero -> true (LRM 12.4 reduction), so the pass action runs.
    const Captured c = run_capture("module m; initial begin\n"
                                   "  assert (2'b10) $display(\"PASS\"); else $display(\"FAIL\");\n"
                                   "end endmodule");
    CHECK(contains(c.out, "PASS"));
    CHECK(c.err.empty());
}

// -- Guards: unimplemented forms fail loudly (never silently miscompiled) --

TEST_CASE("deferred and final immediate assertions are rejected") {
    CHECK(contains(reject("module m; initial assert #0 (1);\nendmodule"),
                   "deferred / final immediate assertions are not supported"));
    CHECK(contains(reject("module m; initial assert final (1);\nendmodule"),
                   "deferred / final immediate assertions are not supported"));
}

TEST_CASE("immediate cover is rejected") {
    CHECK(contains(reject("module m; initial cover (1);\nendmodule"),
                   "only immediate assert / assume are supported"));
}
