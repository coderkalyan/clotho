#include <doctest/doctest.h>

#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module and capture everything it writes to stdout.
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

// All flattened signal names in the lowered module.
std::set<std::string> signal_names(const ir::Module& m) {
    std::set<std::string> names;
    for (const ir::SignalInfo& s : m.signals()) {
        names.insert(s.name);
    }
    return names;
}

} // namespace

// LRM 23.3.6: `foo u [N:M] (...)` instantiates one copy of foo per element;
// a scalar port connected to a vector actual of matching width connects one
// bit per instance. Verified against iverilog (`101` for g_in=3'b101, inverted).
TEST_CASE("descending instance array bit-slices a vector port connection") {
    constexpr std::string_view src = R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module m;
            wire [2:0] g_in = 3'b101;
            wire [2:0] o;
            inv u[2:0](.a(g_in), .y(o));
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "010\n");
}

// Each array element is a distinct instance with its own flattened signal
// path, named <array>[<index>] using the declared (not canonical) index.
TEST_CASE("descending instance array elements flatten under <name>[<index>]") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module m;
            wire [2:0] g_in;
            wire [2:0] o;
            inv u[2:0](.a(g_in), .y(o));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("u[0].a"));
    CHECK(names.contains("u[1].a"));
    CHECK(names.contains("u[2].a"));
    CHECK(names.contains("u[0].y"));
    CHECK(names.contains("u[1].y"));
    CHECK(names.contains("u[2].y"));
}

// An ascending range (`[0:2]`, left < right) still names elements by their
// declared source index, not canonical position — the array's declared
// numbering direction must not leak an off-by-reversal into element names.
TEST_CASE("ascending instance array keeps declared indices in element names") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module m;
            wire [2:0] g_in;
            wire [2:0] o;
            inv u[0:2](.a(g_in), .y(o));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("u[0].a"));
    CHECK(names.contains("u[1].a"));
    CHECK(names.contains("u[2].a"));
}

// A non-zero-based range (`[5:2]`) must keep the user's declared bounds in
// the flattened names, not renumber from 0.
TEST_CASE("non-zero-based instance array range keeps declared bounds in names") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module m;
            wire [3:0] g_in;
            wire [3:0] o;
            inv u[5:2](.a(g_in), .y(o));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("u[5].a"));
    CHECK(names.contains("u[4].a"));
    CHECK(names.contains("u[3].a"));
    CHECK(names.contains("u[2].a"));
    CHECK_FALSE(names.contains("u[0].a"));
    CHECK_FALSE(names.contains("u[1].a"));
}

// Multidimensional instance arrays (`foo u [1:0][1:0] (...)`) nest one nested
// InstanceArraySymbol per extra dimension in slang; each leaf still flattens
// to a unique path carrying every dimension's declared index.
TEST_CASE("multidimensional instance array flattens each leaf with all indices") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module m;
            wire g_in;
            inv u[1:0][1:0](.a(g_in));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("u[0][0].a"));
    CHECK(names.contains("u[0][1].a"));
    CHECK(names.contains("u[1][0].a"));
    CHECK(names.contains("u[1][1].a"));
}

// Gate-primitive arrays share InstanceArraySymbol with module/interface arrays
// in slang, but elaborate to PrimitiveInstanceSymbol elements — gate
// primitives are not lowered at all yet (unsupported constructs
// must fail loudly, not be silently dropped), so this must fail the same way
// a single primitive gate instantiation already does.
TEST_CASE("a gate-primitive instance array fails loudly") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b, output [3:0] o);
            and g[3:0](o, a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
}

// Per-instance connections (each array element wired to a distinct bit)
// behave as N independent instances, not N copies sharing one connection —
// exercised end-to-end with distinguishable per-element inputs.
TEST_CASE("instance array elements behave as independently wired instances") {
    constexpr std::string_view src = R"(
        module buf1(input a, output y);
            assign y = a;
        endmodule
        module m;
            wire [3:0] g_in = 4'b1010;
            wire [3:0] o;
            buf1 u[3:0](.a(g_in), .y(o));
            initial begin #1; $display("%b", o); end
        endmodule
    )";
    CHECK(run_capture(src) == "1010\n");
}
