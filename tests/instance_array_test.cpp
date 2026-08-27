#include <doctest/doctest.h>

#include <cstdio>
#include <set>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
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

// The same sub-array port actual, but with the two sides in DIFFERENT per-element
// slot layouts. A 4-state NET keeps its z plane; a 4-state variable the may-z
// analysis proves never holds z is elided to the mm word alone
// (docs/design/never-x-inference.md), so `.wi(big[0])` onto a `wire` port pairs a
// z-carrying destination slot with a z-elided source one. The whole-object copy
// used to take the SOURCE's element slot from the DESTINATION whenever the source
// was an offset place -- and a constant element-select is one, because ElemSelect
// always carries the LRM 7.4.5 in-range gate -- so it memcpy'd a destination-shaped
// 8 bytes out of a 4-byte object and every element past the first came out of the
// next sub-array: `11 xx` for `11 22`. The `var` spelling was right only because
// both sides happened to be elided alike. Both directions and both spellings are
// pinned here, plus the whole-array net port that was always correct (no element
// select, hence no override, hence honest slots on both sides).
TEST_CASE("whole unpacked sub-array port actual across differing element slots") {
    SUBCASE("input wire port: z-carrying net destination, z-elided source") {
        constexpr std::string_view src = R"(
            module child(input wire logic [7:0] wi [0:1], output logic [7:0] ro [0:1]);
                assign ro[0] = wi[0] + 8'd1;
                assign ro[1] = wi[1] + 8'd2;
            endmodule
            module m;
                logic [7:0] big [0:3][0:1];
                logic [7:0] out [0:3][0:1];
                child c0(.wi(big[0]), .ro(out[0]));
                initial begin
                    big[0][0]=8'h10; big[0][1]=8'h20;
                    #1; $display("%h %h", out[0][0], out[0][1]);
                end
            endmodule
        )";
        CHECK(run_capture(src) == "11 22\n");
    }
    SUBCASE("input var port: both sides z-elided") {
        constexpr std::string_view src = R"(
            module child(input var logic [7:0] wi [0:1], output logic [7:0] ro [0:1]);
                assign ro[0] = wi[0] + 8'd1;
                assign ro[1] = wi[1] + 8'd2;
            endmodule
            module m;
                logic [7:0] big [0:3][0:1];
                logic [7:0] out [0:3][0:1];
                child c0(.wi(big[0]), .ro(out[0]));
                initial begin
                    big[0][0]=8'h10; big[0][1]=8'h20;
                    #1; $display("%h %h", out[0][0], out[0][1]);
                end
            endmodule
        )";
        CHECK(run_capture(src) == "11 22\n");
    }
    // The opposite direction: the differing slot is the DESTINATION's. A singly
    // driven net needs no resolver slot, so the output copy lands straight in the
    // parent net's own storage (wire_ports), which keeps its z plane against the
    // child's z-elided `ro`.
    SUBCASE("output port into a net-typed parent sub-array actual") {
        constexpr std::string_view src = R"(
            module child(input logic [7:0] wi [0:1], output logic [7:0] ro [0:1]);
                assign ro[0] = wi[0] + 8'd1;
                assign ro[1] = wi[1] + 8'd2;
            endmodule
            module m;
                logic [7:0] big [0:3][0:1];
                wire logic [7:0] netout [0:3][0:1];
                child c0(.wi(big[0]), .ro(netout[0]));
                initial begin
                    big[0][0]=8'h10; big[0][1]=8'h20;
                    #1; $display("%h %h", netout[0][0], netout[0][1]);
                end
            endmodule
        )";
        CHECK(run_capture(src) == "11 22\n");
    }
    SUBCASE("whole-array (non-sub-array) net port") {
        constexpr std::string_view src = R"(
            module child(input wire logic [7:0] wi [0:1], output logic [7:0] ro [0:1]);
                assign ro[0] = wi[0] + 8'd1;
                assign ro[1] = wi[1] + 8'd2;
            endmodule
            module m;
                logic [7:0] a [0:1];
                logic [7:0] r [0:1];
                child c(.wi(a), .ro(r));
                initial begin
                    a[0]=8'h10; a[1]=8'h20;
                    #1; $display("%h %h", r[0], r[1]);
                end
            endmodule
        )";
        CHECK(run_capture(src) == "11 22\n");
    }
}

// A non-lvalue aggregate actual (a `'{...}` pattern, or a struct-returning
// function call) bound to a whole-struct / whole-array INPUT port used to be
// refused outright ("aggregate/array port connection must be a variable"):
// wire_ports required an lvalue-shaped source to copy from directly. Fixed by
// staging a non-lvalue source into a fresh canonical temp first
// (agg_input_actual), the same machinery an aggregate function-call argument
// already uses -- LRM 23.3.3 makes a port connection a continuous copy
// exactly like a value-typed call argument, so the same staging applies. An
// lvalue-shaped actual (the common case) still passes straight through with
// no extra copy.
TEST_CASE("non-lvalue aggregate actual bound to an input port") {
    SUBCASE("a '{...} struct literal") {
        constexpr std::string_view src = R"(
            typedef struct { int a; int b; } struct_t;
            module child(input struct_t s, output int sum);
                assign sum = s.a + s.b;
            endmodule
            module m;
                int r;
                child c(.s('{1, 2}), .sum(r));
                initial begin
                    #1; $display("sum=%0d", r);
                end
            endmodule
        )";
        CHECK(run_capture(src) == "sum=3\n");
    }
    SUBCASE("a '{...} unpacked-array literal") {
        constexpr std::string_view src = R"(
            module child(input logic [7:0] wv [0:1], output logic [7:0] rv [0:1]);
                assign rv[0] = wv[0] + 8'd1;
                assign rv[1] = wv[1] + 8'd2;
            endmodule
            module m;
                logic [7:0] rv[0:1];
                child c(.wv('{8'h10, 8'h20}), .rv(rv));
                initial begin
                    #1; $display("rv0=%h rv1=%h", rv[0], rv[1]);
                end
            endmodule
        )";
        CHECK(run_capture(src) == "rv0=11 rv1=22\n");
    }
    SUBCASE("a struct-returning function call") {
        constexpr std::string_view src = R"(
            typedef struct { int a; int b; } struct_t;
            function struct_t make(int x, int y);
                make.a = x; make.b = y;
            endfunction
            module child(input struct_t s, output int sum);
                assign sum = s.a + s.b;
            endmodule
            module m;
                int r;
                child c(.s(make(3, 4)), .sum(r));
                initial begin
                    #1; $display("sum=%0d", r);
                end
            endmodule
        )";
        CHECK(run_capture(src) == "sum=7\n");
    }
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
