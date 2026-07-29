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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("u[0][0].a"));
    CHECK(names.contains("u[0][1].a"));
    CHECK(names.contains("u[1][0].a"));
    CHECK(names.contains("u[1][1].a"));
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

// A whole unpacked array passed as a module port actual (input and output):
// wire_ports copies the whole array via emit_port_agg_copy (a memcpy for a
// plain, trivially-copyable array) rather than the scalar-store path, which
// only handled a single element. Cross-checked vs iverilog.
TEST_CASE("whole unpacked array as an input and output module port") {
    constexpr std::string_view src = R"(
        module ram(input logic [7:0] wv[0:1], output logic [7:0] rv[0:1]);
            assign rv[0] = wv[0] + 8'd1;
            assign rv[1] = wv[1] + 8'd2;
        endmodule
        module m;
            logic [7:0] wv[0:1], rv[0:1];
            ram u(.wv(wv), .rv(rv));
            initial begin
                wv[0] = 8'h10; wv[1] = 8'h20;
                #1; $display("rv0=%h rv1=%h", rv[0], rv[1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "rv0=11 rv1=22\n");
}

// A whole unpacked sub-array (a constant multi-dim element-select `arr[i][j]`)
// passed as a module port actual, in both directions. The AggCopy addresses the
// sub-array at its flat leaf offset (carried in elem_idx) with the copy type's
// element count, not the whole signal's — the rsd D$ `dataArrayByteWE[i][way]`
// port shape. Cross-checked vs verilator.
TEST_CASE("whole unpacked sub-array (arr[i][j]) as a module port, in and out") {
    constexpr std::string_view src = R"(
        module child(input logic [7:0] wi [0:1], output logic [7:0] ro [0:1]);
            assign ro[0] = wi[0] + 8'd1;
            assign ro[1] = wi[1] + 8'd2;
        endmodule
        module m;
            logic [7:0] big [0:3][0:1];
            logic [7:0] out [0:3][0:1];
            child c0(.wi(big[0]), .ro(out[0]));
            child c2(.wi(big[2]), .ro(out[2]));
            initial begin
                big[0][0]=8'h10; big[0][1]=8'h20; big[2][0]=8'h30; big[2][1]=8'h40;
                #1; $display("%h %h %h %h", out[0][0], out[0][1], out[2][0], out[2][1]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "11 22 31 42\n");
}

// A whole-array copy between two same-typed arrays whose z-elided slot layouts
// differ only in the z word's presence: the dest is may-z (assigned z elsewhere),
// this source is proven never-z. memcpy strides differ, so the copy is
// element-wise (mm word + a zero-filled dest z). `d = s` copies s wholesale, so
// the prior z in d[0] is overwritten (hand-verified; iverilog / 2-state verilator
// do not cleanly run a z-bearing whole-array copy).
TEST_CASE("whole-array copy between a may-z and a never-z array") {
    constexpr std::string_view src = R"(
        module m;
            logic [15:0] d [0:2];
            logic [15:0] s [0:2];
            initial begin
                s[0]=16'h1111; s[1]=16'h2222; s[2]=16'h3333;
                d[0]=16'hzzzz;
                d = s;
                #1 $display("%h %h %h", d[0], d[1], d[2]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1111 2222 3333\n");
}
