#include <doctest/doctest.h>

#include <string_view>

#include "Lower.h"

using namespace clotho;

namespace {

// Registered ALU datapath: comb next-result + clocked register w/ sync reset.
constexpr std::string_view kAlu = R"(
    module alu(
        input            clk,
        input            rst,
        input     [1:0]  op,
        input     [7:0]  a,
        input     [7:0]  b,
        output    [7:0]  result
    );
        reg  [7:0] acc;
        wire [7:0] next;

        assign next = (op == 2'd0) ? (a + b) :
                      (op == 2'd1) ? (a - b) :
                      (op == 2'd2) ? (a & b) :
                                     (a ^ b);

        always @(posedge clk)
            if (rst) acc <= 8'd0;
            else     acc <= next;

        assign result = acc;
    endmodule
)";

// Does the IR contain at least one instruction with the given tag?
bool hasTag(const ir::Ir& ir, ir::Inst::Tag tag) {
    for (size_t i = 0; i < ir.instCount(); ++i) {
        if (ir.tag(static_cast<ir::Index>(i)) == tag) {
            return true;
        }
    }

    return false;
}

const ir::Process* findKind(const ir::Module& m, ir::ProcKind kind) {
    for (const ir::Process& p : m.processes()) {
        if (p.kind == kind) {
            return &p;
        }
    }

    return nullptr;
}

} // namespace

TEST_CASE("lowering the registered ALU produces a module") {
    const LowerResult r = lowerText(kAlu);

    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    const ir::Module& m = *r.module;
    CHECK(m.name == "alu");
    // clk, rst, op, a, b, result, acc, next.
    CHECK(m.signals().size() == 8);
    // Two continuous assigns (next, result) + one always block.
    CHECK(m.processes().size() == 3);
}

TEST_CASE("the sequential process carries clocking and nonblocking stores") {
    const LowerResult r = lowerText(kAlu);
    REQUIRE(r.ok);

    const ir::Process* seq = findKind(*r.module, ir::ProcKind::Sequential);
    REQUIRE(seq != nullptr);
    CHECK(seq->clock_edge == ir::Edge::Posedge);
    CHECK(hasTag(seq->ir, ir::Inst::Tag::Cond));
    CHECK(hasTag(seq->ir, ir::Inst::Tag::StoreNb));
    // The clock signal handle resolves to "clk".
    CHECK(r.module->signal(seq->clock).name == "clk");
}

TEST_CASE("a combinational process lowers the operator/mux datapath") {
    const LowerResult r = lowerText(kAlu);
    REQUIRE(r.ok);

    // The `next` assign is the comb process with the arithmetic + selects.
    bool found_datapath = false;
    for (const ir::Process& p : r.module->processes()) {
        if (p.kind == ir::ProcKind::Comb && hasTag(p.ir, ir::Inst::Tag::Mux)) {
            found_datapath = true;
            CHECK(hasTag(p.ir, ir::Inst::Tag::Add));
            CHECK(hasTag(p.ir, ir::Inst::Tag::Store));
        }
    }
    CHECK(found_datapath);
}

TEST_CASE("case statements are reported as unsupported") {
    const LowerResult r = lowerText(R"(
        module m(input [1:0] s, output reg x);
            always @(*)
                case (s)
                    2'd0: x = 1'b0;
                    default: x = 1'b1;
                endcase
        endmodule
    )");

    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("case") != std::string::npos);
}
