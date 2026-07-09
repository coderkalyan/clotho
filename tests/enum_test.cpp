#include <doctest/doctest.h>

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

} // namespace

// An enum member is a named base-typed constant; auto-increment and explicit
// values, comparison, and case dispatch all fall out of the base integer once
// the member reference lowers to its constant. Values verified vs iverilog.
TEST_CASE("enum members lower as base-type constants") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum logic [1:0] {RED, GREEN, BLUE} color_t;
          color_t c;
          enum {A=5, B, C=10} e;
          integer ei;
          initial begin
            c = GREEN;
            $display("c=%0d", c);
            c = BLUE;
            if (c == BLUE) $display("is blue");
            case (c)
              RED:   $display("red");
              GREEN: $display("green");
              BLUE:  $display("blue");
            endcase
            ei = B;
            $display("A=%0d B=%0d C=%0d ei=%0d", A, B, C, ei);
          end
        endmodule
    )");
    CHECK(out == "c=1\nis blue\nblue\nA=5 B=6 C=10 ei=6\n");
}

// A signed base type carries through: negative explicit values and signed
// comparison match iverilog. The enum is also usable as a parameter type.
TEST_CASE("enum with signed base type and enum-typed parameter") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum bit signed [3:0] {NEG=-2, ZERO=0, POS=3} s_t;
          parameter s_t P = POS;
          s_t x;
          initial begin
            x = NEG;
            $display("x=%0d P=%0d", x, P);
            if (x < ZERO) $display("neg");
          end
        endmodule
    )");
    CHECK(out == "x=-2 P=3\nneg\n");
}

// -- Unsupported enum constructs fail loudly (never silently accepted) --

// Enum methods (.name/.next/.prev/.first/.last/.num, LRM 6.19.2-6.19.5) are not
// implemented; each must reject rather than silently mis-lower. slang models
// them as system subroutines, so they hit the unsupported-system-function guard.
TEST_CASE("enum methods are rejected") {
    // .name() returns a string (needs %s); the others return the enum / an int.
    for (const char* method : { "next", "prev", "first", "last", "num" }) {
        const std::string src = std::string(R"(
            module m;
              typedef enum {A, B} e_t;
              e_t x;
              initial begin x = A; $display("%0d", x.)") +
                                method + "()); end\n endmodule\n";
        check_rejected(src, "unsupported system function '" + std::string(method) + "'");
    }
    check_rejected(R"(
        module m;
          typedef enum {A, B} e_t;
          e_t x;
          initial begin x = A; $display("%s", x.name()); end
        endmodule
    )",
                   "unsupported system function 'name'");
}
