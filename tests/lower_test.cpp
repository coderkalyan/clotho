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

TEST_CASE("$time lowers to a SimTime instruction") {
    const LowerResult r = lower_text(R"(
        module m;
            initial $display("%0d", $time);
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    bool found = false;
    for (const ir::Process& p : r.module->processes()) {
        found = found || has_tag(p.ir, ir::Inst::Tag::SimTime);
    }
    CHECK(found);
}

TEST_CASE("a `timescale directive lowers") {
    const LowerResult r = lower_text(R"(`timescale 1ns/1ps
        module m;
            initial #5 $display("x");
        endmodule
    )");
    CHECK(r.ok);
}

TEST_CASE("timeunit / timeprecision declarations lower") {
    const LowerResult r = lower_text(R"(
        module m;
            timeunit 1ns;
            timeprecision 1ps;
            initial #5 $display("x");
        endmodule
    )");
    CHECK(r.ok);
}

TEST_CASE("$random with a seed argument is reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            integer seed;
            integer x;
            initial begin
                seed = 1;
                x = $random(seed);
            end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("$random with a seed argument") != std::string::npos);
}

TEST_CASE("$realtime is reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            initial $display("%0d", $realtime);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("$realtime") != std::string::npos);
}

TEST_CASE("$timeformat with a non-literal suffix is reported as unsupported") {
    // The suffix must be a compile-time string literal; a string parameter
    // reference has no runtime string representation yet, so it is rejected.
    const LowerResult r = lower_text(R"(
        module m;
            parameter string S = " ns";
            initial $timeformat(-9, 2, S, 10);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("suffix must be a string literal") != std::string::npos);
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

// ---- Module instantiation / hierarchy ----

namespace {

// All signal names declared across the flattened module.
std::set<std::string> signal_names(const ir::Module& m) {
    std::set<std::string> names;
    for (const ir::SignalInfo& s : m.signals()) {
        names.insert(s.name);
    }
    return names;
}

// Packed bit width of the named signal, or 0 if absent.
uint32_t signal_width(const ir::Module& m, std::string_view name) {
    for (const ir::SignalInfo& s : m.signals()) {
        if (s.name == name) {
            return m.pool.layout(s.type).width;
        }
    }
    return 0;
}

// Two-level hierarchy: an inverter child wired into a top via one input and one
// output port.
constexpr std::string_view kHier = R"(
    module inv(input a, output y);
        assign y = ~a;
    endmodule
    module top(input g, output o);
        wire w;
        inv u_inv(.a(g), .y(w));
        assign o = w;
    endmodule
)";

} // namespace

TEST_CASE("instantiation flattens the child under a hierarchical prefix") {
    const LowerResult r = lower_text(kHier);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Module& m = *r.module;
    CHECK(m.name == "top");
    // Top signals are unprefixed; the child's are under "u_inv.".
    CHECK(signal_names(m) == std::set<std::string>{ "g", "o", "w", "u_inv.a", "u_inv.y" });
    // top assign (o=w) + child assign (y=~a) + 2 port-copy processes (.a in, .y out).
    CHECK(m.processes().size() == 4);
}

TEST_CASE("an input port connection copies into the child with the expression's sensitivity") {
    const LowerResult r = lower_text(R"(
        module add1(input [7:0] a, output [7:0] y);
            assign y = a + 8'd1;
        endmodule
        module top(input [7:0] x, input [7:0] z, output [7:0] o);
            wire [7:0] w;
            add1 u(.a(x + z), .y(w));
            assign o = w;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    // The copy driving u.a re-evaluates on x and z (the connection expression's reads).
    const ir::Process* copy = find_process(*r.module, [&](const ir::Process& p) {
        return sensitivity_names(*r.module, p) == std::set<std::string>{ "x", "z" };
    });
    CHECK(copy != nullptr);
}

TEST_CASE("the same module instantiated twice gets distinct signal storage") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module top(input g, output [1:0] o);
            inv u0(.a(g), .y(o[0]));
            inv u1(.a(g), .y(o[1]));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("u0.a"));
    CHECK(names.contains("u0.y"));
    CHECK(names.contains("u1.a"));
    CHECK(names.contains("u1.y"));
}

TEST_CASE("an inout port connection fails loudly") {
    const LowerResult r = lower_text(R"(
        module pass(inout a);
        endmodule
        module top(inout b);
            pass u(.a(b));
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("inout") != std::string::npos);
}

TEST_CASE("an instance array fails loudly") {
    const LowerResult r = lower_text(R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
        module top(input [3:0] g, output [3:0] o);
            inv u[3:0](.a(g), .y(o));
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("instance array") != std::string::npos);
}

TEST_CASE("whole unpacked array assignment fails loudly") {
    const LowerResult r = lower_text(R"(
        module top;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            initial a = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("whole unpacked array assignment") != std::string::npos);
}

TEST_CASE("nonblocking assignment to an array element fails loudly (Phase 2)") {
    const LowerResult r = lower_text(R"(
        module top;
            logic [7:0] mem [0:3];
            logic clk;
            always @(posedge clk) mem[0] <= 8'd1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("nonblocking assignment to an unpacked array element") != std::string::npos);
}

TEST_CASE("a dynamic array fails loudly") {
    const LowerResult r = lower_text(R"(
        module top;
            int dyn [];
            initial dyn = new [4];
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("dynamic / queue / associative") != std::string::npos);
}

TEST_CASE("nested instances chain their hierarchical prefixes") {
    const LowerResult r = lower_text(R"(
        module leaf(input a, output y);
            assign y = ~a;
        endmodule
        module mid(input mi, output mo);
            leaf u_leaf(.a(mi), .y(mo));
        endmodule
        module top(input g, output o);
            mid u_mid(.mi(g), .mo(o));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const std::set<std::string> names = signal_names(*r.module);
    // The grandchild's nets carry the full path through both instance levels.
    CHECK(names.contains("u_mid.u_leaf.a"));
    CHECK(names.contains("u_mid.u_leaf.y"));
    CHECK(names.contains("u_mid.mi"));
    CHECK(names.contains("u_mid.mo"));
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

// -- Parameters (LRM 6.20) --

TEST_CASE("a parameter sets a signal width and drives a replication count") {
    const LowerResult r = lower_text(R"(
        module m #(parameter W = 4) (output [W-1:0] o);
            assign o = {W{1'b1}};
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "o") == 4);
}

TEST_CASE("a #() override specializes each instance's width independently") {
    const LowerResult r = lower_text(R"(
        module m #(parameter W = 4) (output [W-1:0] o);
            assign o = {W{1'b0}};
        endmodule
        module top(output [2:0] a, output [7:0] b);
            m #(.W(3)) u0(.o(a));
            m #(.W(8)) u1(.o(b));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "u0.o") == 3);
    CHECK(signal_width(*r.module, "u1.o") == 8);
}

TEST_CASE("a parameter referenced in an rvalue folds to a constant, not a signal read") {
    const LowerResult r = lower_text(R"(
        module m #(parameter W = 5) (input [7:0] a, output [7:0] y);
            assign y = a + W;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    // W is a compile-time constant: the assign is sensitive only to `a`.
    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a" });
}

TEST_CASE("a localparam derived from a parameter resolves its width") {
    const LowerResult r = lower_text(R"(
        module m #(parameter W = 4) (output [2*W-1:0] o);
            localparam D = 2 * W;
            assign o = {D{1'b0}};
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "o") == 8);
}

TEST_CASE("a parameter expression in a part-select bound folds") {
    const LowerResult r = lower_text(R"(
        module m #(parameter W = 8) (input [W-1:0] a, output [1:0] o);
            assign o = a[W-1:W-2];
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "o") == 2);
}
