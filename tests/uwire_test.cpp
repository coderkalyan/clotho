#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

#include <cstdio>

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

namespace {

size_t count_code(const LowerResult& r, std::string_view code) {
    size_t n = 0;
    for (const Diagnostic& d : r.diagnostics) {
        n += d.code == code ? 1 : 0;
    }

    return n;
}

} // namespace

// LRM 23.3.3.6: "If the net on either side of a port has the net type uwire, a
// warning shall be issued if the nets are not merged into a single net, as
// described in 23.3.3.7." 23.3.3.7 only makes merging permissible, and Clotho
// keeps the nets of an input or output port apart (a continuous assignment from
// source to sink, 23.3.3), so it warns at the connection. Oracle: the LRM text;
// iverilog 13 merges the nets, so it has nothing to say.
TEST_CASE("a uwire on either side of an output or input port warns that the nets are not merged") {
    const LowerResult output_side = lower_text(R"(
        module sub(output y);
            assign y = 1'b1;
        endmodule
        module m;
            uwire w;
            sub u(.y(w));
        endmodule
    )");
    REQUIRE_MESSAGE(output_side.ok, output_side.diagnostic_text());
    const auto it = std::ranges::find_if(output_side.diagnostics, [](const Diagnostic& d) {
        return d.code == "uwire-port-not-merged";
    });
    REQUIRE(it != output_side.diagnostics.end());
    CHECK(it->severity == Severity::Warning);
    CHECK(it->origin == DiagnosticOrigin::Analysis);
    CHECK(it->span.valid());

    const LowerResult input_side = lower_text(R"(
        module sub(input uwire a);
            wire b = a;
        endmodule
        module m;
            wire w = 1'b1;
            sub u(w);
        endmodule
    )");
    REQUIRE_MESSAGE(input_side.ok, input_side.diagnostic_text());
    CHECK(count_code(input_side, "uwire-port-not-merged") == 1);
}

// One finding per connection in the source, however many instances elaborate it.
TEST_CASE("the uwire port warning is reported once per connection, not per instance") {
    const LowerResult r = lower_text(R"(
        module sub(output y);
            assign y = 1'b1;
        endmodule
        module m;
            uwire [3:0] w;
            for (genvar i = 0; i < 4; i++) begin : g
                sub u(.y(w[i]));
            end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(count_code(r, "uwire-port-not-merged") == 1);
}

TEST_CASE("a port between wires says nothing about uwire") {
    const LowerResult r = lower_text(R"(
        module sub(output y);
            assign y = 1'b1;
        endmodule
        module m;
            wire w;
            sub u(.y(w));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(count_code(r, "uwire-port-not-merged") == 0);
}
