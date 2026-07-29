#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "ir/Jit.h"

#include <cstdio>
#include <unistd.h>

using namespace clotho;

// `uwire` (LRM 6.6.7) is a single-driver net: exactly one driver is legal,
// multiple drivers is an error. Clotho has no net resolution, so every net is
// single-driver-only today -- which happens to be exactly uwire's semantics.
// A single-driver uwire therefore lowers and simulates identically to a wire,
// and a multi-driver uwire is rejected by slang's analysis pass
// (MultipleUWireDrivers) before Clotho lowers.

namespace {

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

}  // namespace

TEST_CASE("a single-driver uwire drives its value like a wire") {
    const std::string out = run_capture(R"(
        module m;
            logic a, b;
            uwire w;
            assign w = a & b;
            initial begin
                a = 1; b = 1; #1 $display("%b", w);
                b = 0;         #1 $display("%b", w);
            end
        endmodule
    )");
    CHECK(out == "1\n0\n");
}

TEST_CASE("a uwire net-declaration assignment drives its value") {
    const std::string out = run_capture(R"(
        module m;
            logic [3:0] a;
            uwire [3:0] w = a + 4'd1;
            initial begin
                a = 4'd5; #1 $display("%0d", w);
            end
        endmodule
    )");
    CHECK(out == "6\n");
}

TEST_CASE("two continuous assigns driving one uwire fail loudly") {
    const LowerResult r = lower_text(R"(
        module m;
            logic a, b;
            uwire w;
            assign w = a;
            assign w = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("'uwire' net 'w' cannot have multiple drivers") !=
          std::string::npos);
}

TEST_CASE("a uwire net-decl assignment plus a continuous assign fail loudly") {
    const LowerResult r = lower_text(R"(
        module m(input a);
            uwire w = 1'b0;
            assign w = a;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("'uwire' net 'w' cannot have multiple drivers") !=
          std::string::npos);
}

TEST_CASE("a uwire driven by a single output-port connection is accepted") {
    const std::string out = run_capture(R"(
        module sub(output y);
            assign y = 1'b1;
        endmodule
        module m;
            uwire w;
            sub u(.y(w));
            initial begin #1 $display("%b", w); end
        endmodule
    )");
    CHECK(out == "1\n");
}
