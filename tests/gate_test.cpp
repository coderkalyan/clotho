// Structural gate primitives (LRM 28.4, 28.5): frontend lowering to the module
// gate table, and (in later sections) runtime behavior of the backend's
// table-driven gate evaluation.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"

using namespace clotho;

namespace {

// Lower and require success, returning the module.
LowerResult lower_ok(std::string_view src) {
    LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    return r;
}

// The gate at index `i`, bounds-checked.
const ir::GateInfo& gate_at(const ir::Module& m, size_t i) {
    REQUIRE(i < m.gates().size());

    return m.gates()[i];
}

} // namespace

TEST_CASE("two-input gates lower to gate table rows, not processes") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire y0, y1, y2, y3, y4, y5;
            reg a, b;
            and  g0(y0, a, b);
            nand g1(y1, a, b);
            or   g2(y2, a, b);
            nor  g3(y3, a, b);
            xor  g4(y4, a, b);
            xnor g5(y5, a, b);
        endmodule
    )");
    const ir::Module& m = *r.module;

    REQUIRE(m.gates().size() == 6);
    CHECK(m.processes().size() == 0);
    CHECK(gate_at(m, 0).op == ir::GateOp::And);
    CHECK(gate_at(m, 1).op == ir::GateOp::Nand);
    CHECK(gate_at(m, 2).op == ir::GateOp::Or);
    CHECK(gate_at(m, 3).op == ir::GateOp::Nor);
    CHECK(gate_at(m, 4).op == ir::GateOp::Xor);
    CHECK(gate_at(m, 5).op == ir::GateOp::Xnor);
    for (const ir::GateInfo& g : m.gates()) {
        CHECK(g.in_count == 2);
    }
}

TEST_CASE("n-input and one-input forms carry their arity") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire y3, y1;
            reg a, b, c;
            and g0(y3, a, b, c);
            and g1(y1, a);
        endmodule
    )");
    CHECK(gate_at(*r.module, 0).in_count == 3);
    CHECK(gate_at(*r.module, 1).in_count == 1);
}

TEST_CASE("multi-output buf and not lower to one gate per output") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire o0, o1, o2;
            reg a;
            buf g0(o0, o1, a);
            not g1(o2, a);
        endmodule
    )");
    const ir::Module& m = *r.module;

    REQUIRE(m.gates().size() == 3);
    CHECK(gate_at(m, 0).op == ir::GateOp::Buf);
    CHECK(gate_at(m, 1).op == ir::GateOp::Buf);
    CHECK(gate_at(m, 2).op == ir::GateOp::Not);
    // Both buf outputs read the same single input.
    CHECK(gate_at(m, 0).in_count == 1);
    CHECK(gate_at(m, 1).in_count == 1);
    const ir::GateInput& in0 = m.gate_inputs()[gate_at(m, 0).in_start];
    const ir::GateInput& in1 = m.gate_inputs()[gate_at(m, 1).in_start];
    CHECK(in0.sig == in1.sig);
    // Distinct output signals.
    CHECK(gate_at(m, 0).output.sig != gate_at(m, 1).output.sig);
}

TEST_CASE("constant gate inputs fold to plane codes") {
    const LowerResult r = lower_ok(R"(
        module m;
            parameter P = 1'b0;
            wire y;
            reg a;
            and g(y, a, 1'b1, P, 1'bx, 1'bz);
        endmodule
    )");
    const ir::Module&   m = *r.module;
    const ir::GateInfo& g = gate_at(m, 0);

    REQUIRE(g.in_count == 5);
    const auto in = [&](uint32_t k) -> const ir::GateInput& { return m.gate_inputs()[g.in_start + k]; };
    CHECK_FALSE(in(0).is_const);
    CHECK(in(1).is_const);
    CHECK(in(1).const_code == 0b11);
    CHECK(in(2).is_const);
    CHECK(in(2).const_code == 0b00);
    // x and z both fold to the X code (Z reads as X, LRM Table 28-3).
    CHECK(in(3).is_const);
    CHECK(in(3).const_code == 0b10);
    CHECK(in(4).is_const);
    CHECK(in(4).const_code == 0b10);
}

TEST_CASE("bit-select terminals resolve to LSB-relative static bits") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire [7:0] y;
            wire [7:4] hi;
            wire [0:3] up;
            reg  [7:0] a;
            and g0(y[3], a[6], a[0]);
            and g1(hi[5], a[1], a[2]);
            and g2(up[1], a[1], a[2]);
        endmodule
    )");
    const ir::Module& m = *r.module;

    REQUIRE(m.gates().size() == 3);
    CHECK(gate_at(m, 0).output.bit == 3);
    CHECK(m.gate_inputs()[gate_at(m, 0).in_start].bit == 6);
    // [7:4]: declared index 5 is bit 1 from the LSB end.
    CHECK(gate_at(m, 1).output.bit == 1);
    // [0:3] ascending: declared index 1 is bit 2 from the LSB end.
    CHECK(gate_at(m, 2).output.bit == 2);
}

TEST_CASE("a complex gate input becomes an implicit continuous assign to a temp net") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire y;
            reg a, b, c;
            reg [3:0] v;
            and g(y, a & b, c, v[1] | a, b);
        endmodule
    )");
    const ir::Module& m = *r.module;

    REQUIRE(m.gates().size() == 1);
    CHECK(gate_at(m, 0).in_count == 4);
    // Two synthesized temp nets, each driven by a continuous-assign process.
    CHECK(m.processes().size() == 2);
    size_t temps = 0;
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name.find("$gate") != std::string::npos) {
            ++temps;
        }
    }
    CHECK(temps == 2);
    for (const ir::Process& p : m.processes()) {
        CHECK(p.activation == ir::Activation::Always);
        CHECK(p.self_start);
        CHECK(!p.wait_events.empty());
    }
}

TEST_CASE("gate instance arrays lower one gate per element with sliced terminals") {
    const LowerResult r = lower_ok(R"(
        module m;
            wire [3:0] y;
            reg  [3:0] a, b;
            and g[3:0](y, a, b);
        endmodule
    )");
    const ir::Module& m = *r.module;

    REQUIRE(m.gates().size() == 4);
    // Element i drives y[i] from a[i], b[i] (LSB -> right-hand range index).
    for (uint32_t i = 0; i < 4; ++i) {
        bool found = false;
        for (const ir::GateInfo& g : m.gates()) {
            if (g.output.bit != i) {
                continue;
            }
            found = true;
            REQUIRE(g.in_count == 2);
            CHECK(m.gate_inputs()[g.in_start].bit == i);
            CHECK(m.gate_inputs()[g.in_start + 1].bit == i);
        }
        CHECK_MESSAGE(found, "no gate drives output bit ", i);
    }
}

TEST_CASE("gates lower inside generate blocks and child instances") {
    const LowerResult r = lower_ok(R"(
        module sub(input a, input b, output y);
            and g(y, a, b);
        endmodule
        module m;
            wire y0, y1;
            reg a, b;
            genvar i;
            generate
                for (i = 0; i < 1; i = i + 1) begin : blk
                    or g(y0, a, b);
                end
            endgenerate
            sub u(a, b, y1);
        endmodule
    )");
    REQUIRE(r.module->gates().size() == 2);
}

TEST_CASE("tri-state and switch primitives are reported as unsupported") {
    // Terminals are nets: tran-family inout terminals reject variables in slang
    // before clotho's own check would run.
    for (const char* inst : { "bufif1 g(y, a, b);", "notif0 g(y, a, b);", "nmos g(y, a, b);",
                              "pullup g(y);", "tran g(y, a);" }) {
        const LowerResult r = lower_text("module m;\n wire y;\n wire a, b;\n " + std::string(inst) + "\nendmodule");
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(r.diagnostics.find("is not supported") != std::string::npos, r.diagnostics, " for ", inst);
    }
}

TEST_CASE("gate delays are reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            wire y;
            reg a, b;
            and #2 g(y, a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("gate delay") != std::string::npos);
}

TEST_CASE("non-default gate drive strengths are reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            wire y;
            reg a, b;
            and (weak0, weak1) g(y, a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("drive strength") != std::string::npos);

    // The default strength written out explicitly is accepted.
    const LowerResult ok = lower_ok(R"(
        module m;
            wire y;
            reg a, b;
            and (strong0, strong1) g(y, a, b);
        endmodule
    )");
    CHECK(ok.module->gates().size() == 1);
}

TEST_CASE("user-defined primitives are reported as unsupported") {
    const LowerResult r = lower_text(R"(
        primitive udp_and(output y, input a, input b);
            table
                1 1 : 1;
                0 ? : 0;
                ? 0 : 0;
            endtable
        endprimitive
        module m;
            wire y;
            reg a, b;
            udp_and g(y, a, b);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("is not supported") != std::string::npos);
}

TEST_CASE("unsupported gate output lvalues are rejected") {
    // Concatenation output.
    const LowerResult r1 = lower_text(R"(
        module m;
            wire y;
            reg a, b;
            and g({y}, a, b);
        endmodule
    )");
    CHECK_FALSE(r1.ok);
    CHECK(r1.diagnostics.find("gate output") != std::string::npos);

    // 2-state output net: a gate can drive X, which the slot cannot hold.
    const LowerResult r2 = lower_text(R"(
        module m;
            bit y;
            reg a, b;
            and g(y, a, b);
        endmodule
    )");
    CHECK_FALSE(r2.ok);
    CHECK(r2.diagnostics.find("2-state") != std::string::npos);
}

TEST_CASE("module instance arrays still fail loudly") {
    const LowerResult r = lower_text(R"(
        module sub(input a, output y);
            assign y = a;
        endmodule
        module m;
            wire [1:0] y;
            reg  [1:0] a;
            sub u[1:0](a, y);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("instance arrays") != std::string::npos);
}
