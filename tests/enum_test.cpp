#include <doctest/doctest.h>

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

// first()/last()/num() (LRM 6.19.4-6.19.5) depend only on the type: the first
// and last member values in declaration order, and the member count. Values
// verified vs iverilog.
TEST_CASE("enum first / last / num") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum logic [2:0] {RED=1, GREEN=3, BLUE=6} c_t;
          c_t x;
          initial $display("num=%0d first=%0d last=%0d", x.num(), x.first(), x.last());
        endmodule
    )");
    CHECK(out == "num=3 first=1 last=6\n");
}

// next(N)/prev(N) (LRM 6.19.5) walk declaration order with wrap; N is an
// optional constant step (default 1); a non-member receiver yields X. All
// values verified vs iverilog.
TEST_CASE("enum next / prev with wrap, step, and non-member") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum logic [2:0] {A=1, B=3, C=6} e_t;
          e_t x;
          initial begin
            x = A; $display("%0d %0d", x.next(), x.prev());
            x = C; $display("%0d %0d", x.next(), x.prev());
            x = B; $display("%0d %0d", x.next(2), x.prev(2));
            x = A; $display("%0d %0d", x.next(0), x.next(4));
            x = e_t'(3'd2); $display("%0d", x.next());
          end
        endmodule
    )");
    CHECK(out == "3 6\n1 3\n1 6\n1 3\nx\n");
}

// name() (LRM 6.19.2) returns the source name of the matching member, or "" for
// a non-member value. Verified vs iverilog.
TEST_CASE("enum name") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum logic [2:0] {RED=1, GREEN=3, BLUE=6} c_t;
          c_t x, y;
          initial begin
            x = RED;   $display("[%s]", x.name());
            x = BLUE;  $display("[%s]", x.name());
            x = c_t'(3'd2); $display("[%s]", x.name());
            x = RED; y = x.next(); $display("[%s]", y.name());
          end
        endmodule
    )");
    CHECK(out == "[RED]\n[BLUE]\n[]\n[GREEN]\n");
}

// -- Unsupported enum constructs fail loudly (never silently accepted) --

// Enum methods (.name/.next/.prev/.first/.last/.num, LRM 6.19.2-6.19.5) are not
// implemented; each must reject rather than silently mis-lower. slang models
// them as system subroutines, so they hit the unsupported-system-function guard.
// name() on a base type wider than 64 bits: the member table stores each value
// as its must-plane bytes and the runtime byte-compares, so any width works.
// Cross-checked against Icarus Verilog (A/B/C match, a non-member yields "").
TEST_CASE("enum name with a base type wider than 64 bits") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum logic [95:0] {
            A = 96'hDEADBEEFCAFEBABE01234567,
            B = 96'h0000000000000000000000FF,
            C = 96'hFFFFFFFFFFFFFFFFFFFFFFFF
          } e_t;
          e_t e;
          initial begin
            e = A; $display("[%s]", e.name());
            e = B; $display("[%s]", e.name());
            e = C; $display("[%s]", e.name());
            e = e_t'(96'h123); $display("[%s]", e.name());
          end
        endmodule
    )");
    CHECK(out == "[A]\n[B]\n[C]\n[]\n");
}

TEST_CASE("enum methods are rejected") {
    // A runtime (non-constant) step to next/prev is not supported.
    check_rejected(R"(
        module m;
          typedef enum {A, B} e_t;
          e_t x; int k;
          initial begin x = A; k = 1; $display("%0d", x.next(k)); end
        endmodule
    )",
                   "non-constant step");
}

// $cast (LRM 6.24.2 / 8.16), Tier 1: numeric + enum destinations. Returns 1 and
// assigns on success, else 0 with dest unchanged.
TEST_CASE("$cast: numeric and enum destinations") {
    const std::string out = run_capture(R"(
        module m;
          typedef enum { A, B, C } E;  // 0,1,2
          int a;
          E e;
          initial begin
            a = 0; e = C;
            $display("%0d %0d", $cast(a, 2.9 * 2.0), a);   // real->int: 1, a=6
            $display("%0d %0d", $cast(e, 1), e);            // valid enum: 1, e=1
            e = C;
            $display("%0d %0d", $cast(e, 9), e);            // invalid: 0, e unchanged (2)
          end
        endmodule
    )");
    CHECK(out == "1 6\n1 1\n0 2\n");
}

// $cast as a task (bare statement) performs the assignment (numeric always
// succeeds); the result is discarded.
TEST_CASE("$cast: task form assigns") {
    const std::string out = run_capture(R"(
        module m;
          int a;
          initial begin a = 0; $cast(a, 2.1 * 3.7); $display("%0d", a); end
        endmodule
    )");
    CHECK(out == "8\n");  // 2.1*3.7 = 7.77 -> rounds to 8
}

// A class-handle $cast (dynamic down-cast, Tier 2) needs a runtime type identity;
// it must fail loudly, not silently mis-lower.
TEST_CASE("$cast: class handle rejected") {
    check_rejected(R"(
        module m;
          class Base; endclass
          class Der extends Base; endclass
          initial begin Base b; Der d; b = new(); if ($cast(d, b)) ; end
        endmodule
    )",
                   "class handle is not supported");
}
