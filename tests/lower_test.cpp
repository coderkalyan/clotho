#include <doctest/doctest.h>

#include <set>
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
bool has_tag(const ir::Ir& ir, ir::Inst::Tag tag) {
    for (size_t i = 0; i < ir.inst_count(); ++i) {
        if (ir.tag(static_cast<ir::Index>(i)) == tag) {
            return true;
        }
    }

    return false;
}

// True if the process is edge-triggered: some wait event is a clock edge rather
// than a level-sensitive (any-change) entry.
bool is_edge_triggered(const ir::Process& p) {
    for (const ir::WaitEvent& w : p.wait_events) {
        if (w.edge != ir::Edge::Any) {
            return true;
        }
    }

    return false;
}

// First process matching `pred`, or null.
template <class Pred>
const ir::Process* find_process(const ir::Module& m, Pred pred) {
    for (const ir::Process& p : m.processes()) {
        if (pred(p)) {
            return &p;
        }
    }

    return nullptr;
}

// First edge-triggered process (the lone always @(posedge/negedge ...) block).
const ir::Process* find_edge_process(const ir::Module& m) {
    return find_process(m, is_edge_triggered);
}

// First level-sensitive process: an always-activated process with no edge wait
// events (a continuous assign or a @(*) / level-sensitive always block).
const ir::Process* find_level_process(const ir::Module& m) {
    return find_process(m, [](const ir::Process& p) {
        return p.activation == ir::Activation::Always && !is_edge_triggered(p);
    });
}

} // namespace

TEST_CASE("lowering the registered ALU produces a module") {
    const LowerResult r = lower_text(kAlu);

    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    const ir::Module& m = *r.module;
    CHECK(m.name == "alu");
    // clk, rst, op, a, b, result, acc, next.
    CHECK(m.signals().size() == 8);
    // Two continuous assigns (next, result) + one always block.
    CHECK(m.processes().size() == 3);
}

TEST_CASE("the edge-triggered process carries its wait event and nonblocking stores") {
    const LowerResult r = lower_text(kAlu);
    REQUIRE(r.ok);

    const ir::Process* seq = find_edge_process(*r.module);
    REQUIRE(seq != nullptr);
    CHECK(seq->activation == ir::Activation::Always);
    // A single wait event: posedge of clk.
    REQUIRE(seq->wait_events.size() == 1);
    CHECK(seq->wait_events[0].edge == ir::Edge::Positive);
    CHECK(r.module->signal(seq->wait_events[0].signal).name == "clk");
    CHECK(has_tag(seq->ir, ir::Inst::Tag::Cond));
    CHECK(has_tag(seq->ir, ir::Inst::Tag::StoreNb));
}

TEST_CASE("a level-sensitive process lowers the operator/mux datapath") {
    const LowerResult r = lower_text(kAlu);
    REQUIRE(r.ok);

    // The `next` assign is the level-sensitive process with the arithmetic + selects.
    bool found_datapath = false;
    for (const ir::Process& p : r.module->processes()) {
        if (!is_edge_triggered(p) && has_tag(p.ir, ir::Inst::Tag::Mux)) {
            found_datapath = true;
            CHECK(has_tag(p.ir, ir::Inst::Tag::Add));
            CHECK(has_tag(p.ir, ir::Inst::Tag::Store));
        }
    }
    CHECK(found_datapath);
}

// Lower a single combinational process wrapping `body` and return its IR.
LowerResult lower_case_module(std::string_view kind, std::string_view body) {
    std::string src = "module m(input [1:0] s, output reg [1:0] x);\n always @(*)\n  ";
    src += kind;
    src += " (s)\n";
    src += body;
    src += "\n  endcase\nendmodule\n";

    return lower_text(src);
}

TEST_CASE("case lowers to a case-equality / cond chain") {
    const LowerResult r = lower_case_module("case", R"(
        2'd0: x = 2'b00;
        2'd1: x = 2'b01;
        default: x = 2'b11;
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    CHECK(has_tag(p->ir, ir::Inst::Tag::CaseEq));
    CHECK(has_tag(p->ir, ir::Inst::Tag::Cond));
    // Normal case uses exact equality, never the X-propagating logical Eq.
    CHECK_FALSE(has_tag(p->ir, ir::Inst::Tag::Eq));
}

TEST_CASE("casez / casex select the wildcard comparison") {
    const LowerResult rz = lower_case_module("casez", "2'b1?: x = 2'b01; default: x = 2'b00;");
    REQUIRE_MESSAGE(rz.ok, rz.diagnostics);
    CHECK(has_tag(find_level_process(*rz.module)->ir, ir::Inst::Tag::CasezEq));

    const LowerResult rx = lower_case_module("casex", "2'b1x: x = 2'b01; default: x = 2'b00;");
    REQUIRE_MESSAGE(rx.ok, rx.diagnostics);
    CHECK(has_tag(find_level_process(*rx.module)->ir, ir::Inst::Tag::CasexEq));
}

TEST_CASE("a case with multiple labels per item ORs the comparisons") {
    const LowerResult r = lower_case_module("case", "2'd0, 2'd1: x = 2'b01; default: x = 2'b00;");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    CHECK(has_tag(p->ir, ir::Inst::Tag::Lor));
}

TEST_CASE("case inside is reported as unsupported") {
    const LowerResult ri = lower_text(R"(
        module m(input [2:0] s, output reg x);
            always @(*)
                case (s) inside
                    [0:3]: x = 1'b1;
                    default: x = 1'b0;
                endcase
        endmodule
    )");
    CHECK_FALSE(ri.ok);
    CHECK(ri.diagnostics.find("inside") != std::string::npos);
}

// -- Inferred (@*) combinational sensitivity --

// The set of signal names a process is sensitive to, all expected level-sensitive.
std::set<std::string> sensitivity_names(const ir::Module& m, const ir::Process& p) {
    std::set<std::string> names;
    for (const ir::WaitEvent& w : p.wait_events) {
        CHECK(w.edge == ir::Edge::Any);
        names.insert(m.signal(w.signal).name);
    }
    return names;
}

TEST_CASE("always_comb infers its sensitivity from the RHS reads") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b, input sel, output reg y);
            always_comb
                y = sel ? a : b;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // Reads a, b, sel; the write target y is excluded.
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "b", "sel" });
}

TEST_CASE("always @(*) infers the same sensitivity as always_comb") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b, output reg y);
            always @(*)
                y = a & b;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "b" });
}

TEST_CASE("always_latch infers sensitivity the same way as always_comb") {
    const LowerResult r = lower_text(R"(
        module m(input g, input d, output reg q);
            always_latch
                if (g) q = d;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // Enable and data are reads; the (conditionally) written q holds otherwise.
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "g", "d" });
}

TEST_CASE("an intermediate that is written then read is not in the sensitivity list") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b, input c, output reg y);
            reg t;
            always_comb begin
                t = a & b;
                y = t | c;
            end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // t is read on the second line but written on the first, so it drops out.
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "b", "c" });
}

TEST_CASE("an index expression on the write target is a read") {
    const LowerResult r = lower_text(R"(
        module m(input [7:0] d, input [2:0] i, output reg [7:0] y);
            always_comb begin
                y    = 8'd0;
                y[i] = d[0];
            end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // i (LHS index) and d (RHS) are reads; y is only ever written.
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "d", "i" });
}

// -- Continuous-assign sensitivity (LRM 4.9.1) --

TEST_CASE("a continuous assign infers sensitivity from its RHS reads") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b, input sel, output y);
            assign y = sel ? a : b;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "b", "sel" });
}

TEST_CASE("a continuous assign with combinational feedback stays sensitive to the driven net") {
    const LowerResult r = lower_text(R"(
        module m(input a, output y);
            assign y = a & y;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // The driven net y is read on the RHS, so unlike always_comb it is NOT
    // subtracted: real feedback must re-trigger the assign.
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "y" });
}

TEST_CASE("a continuous assign with a constant RHS has an empty wait list") {
    const LowerResult r = lower_text(R"(
        module m(output [3:0] y);
            assign y = 4'b0;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // No source elements: the process runs once at time zero and never re-triggers.
    CHECK(p->wait_events.empty());
}
