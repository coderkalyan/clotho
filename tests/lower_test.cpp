#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

TEST_CASE("a level-sensitive process lowers the operator/select datapath") {
    const LowerResult r = lower_text(kAlu);
    REQUIRE(r.ok);

    // The `next` assign is the level-sensitive process with the arithmetic + selects.
    bool found_datapath = false;
    for (const ir::Process& p : r.module->processes()) {
        if (!is_edge_triggered(p) && has_tag(p.ir, ir::Inst::Tag::Select)) {
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

TEST_CASE("constant case lowers to a switch") {
    const LowerResult r = lower_case_module("case", R"(
        2'd0: x = 2'b00;
        2'd1: x = 2'b01;
        default: x = 2'b11;
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // Constant, deduped, wildcard-free labels lower to a multiway switch, not
    // the O(N) case-equality if-else chain.
    CHECK(has_tag(p->ir, ir::Inst::Tag::Switch));
    CHECK_FALSE(has_tag(p->ir, ir::Inst::Tag::CaseEq));
    CHECK_FALSE(has_tag(p->ir, ir::Inst::Tag::Cond));
}

TEST_CASE("non-constant case label uses the case-equality cond chain") {
    // A label reading `x` is not a constant, so the switch key cannot be built
    // and lowering falls back to the if-else chain; a grouped item ORs its
    // comparisons.
    const LowerResult r = lower_case_module("case", "x, 2'd1: x = 2'b01; default: x = 2'b00;");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    CHECK(has_tag(p->ir, ir::Inst::Tag::CaseEq));
    CHECK(has_tag(p->ir, ir::Inst::Tag::Cond));
    CHECK(has_tag(p->ir, ir::Inst::Tag::Lor));
    CHECK_FALSE(has_tag(p->ir, ir::Inst::Tag::Switch));
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

TEST_CASE("grouped constant labels share one switch arm") {
    const LowerResult r = lower_case_module("case", "2'd0, 2'd1: x = 2'b01; default: x = 2'b00;");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // Both keys map to one arm body; no OR-of-comparisons chain.
    CHECK(has_tag(p->ir, ir::Inst::Tag::Switch));
    CHECK_FALSE(has_tag(p->ir, ir::Inst::Tag::Lor));
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

TEST_CASE("a real (fractional) delay is reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            initial #1.5 $display("x");
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("delay") != std::string::npos);
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

// A `final` block lowers to a run-once end-of-sim process (LRM 9.2.3), scheduled
// separately from initial/always (the backend runs it on the exit paths).
TEST_CASE("final block lowers to a Final-activation process") {
    const LowerResult r = lower_text(R"(
        module m;
            final $display("done");
        endmodule
    )");
    REQUIRE(r.ok);
    REQUIRE(r.module != nullptr);
    bool has_final = false;
    for (const ir::Process& p : r.module->processes()) {
        has_final |= p.activation == ir::Activation::Final;
    }
    CHECK(has_final);
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

// Writes `text` to a fresh temp file and returns its path; the file is removed
// by RAII on scope exit.
struct TempSource {
    std::filesystem::path path;
    explicit TempSource(std::string_view name, std::string_view text)
      : path(std::filesystem::temp_directory_path() / name) {
        std::ofstream(path) << text;
    }
    ~TempSource() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

TEST_CASE("lower_files compiles a design split across two files into one design") {
    // The child module lives in one file, the top that instantiates it in
    // another: cross-file name resolution requires both trees in one Compilation.
    TempSource child("clotho_mf_inv.sv", R"(
        module inv(input a, output y);
            assign y = ~a;
        endmodule
    )");
    TempSource top("clotho_mf_top.sv", R"(
        module top(input g, output o);
            wire w;
            inv u_inv(.a(g), .y(w));
            assign o = w;
        endmodule
    )");

    const std::array<std::string_view, 2> paths{ top.path.native(), child.path.native() };
    const LowerResult                     r = lower_files(paths);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(r.module->name == "top");
    CHECK(signal_names(*r.module) == std::set<std::string>{ "g", "o", "w", "u_inv.a", "u_inv.y" });
}

TEST_CASE("lower_files reports a missing file loudly") {
    const std::array<std::string_view, 1> paths{ "/no/such/clotho/file.sv" };
    const LowerResult                     r = lower_files(paths);
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("cannot read") != std::string::npos);
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

TEST_CASE("whole unpacked array blocking assignment lowers (memcpy)") {
    // A blocking copy of a string-free unpacked array is a single memcpy; a
    // nonblocking copy / pattern / string array is still guarded.
    const LowerResult r = lower_text(R"(
        module top;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            initial a = b;
        endmodule
    )");
    CHECK(r.ok);
}

TEST_CASE("nonblocking whole unpacked array assignment fails loudly") {
    const LowerResult r = lower_text(R"(
        module top;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            logic clk;
            always @(posedge clk) a <= b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("whole unpacked array assignment") != std::string::npos);
}

TEST_CASE("a nonblocking write to an array element lowers") {
    const LowerResult r = lower_text(R"(
        module top;
            logic [7:0] mem [0:3];
            logic clk;
            always @(posedge clk) mem[0] <= 8'd1;
        endmodule
    )");
    CHECK(r.ok);
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

TEST_CASE("a packed struct declares and carries its field table") {
    const LowerResult r = lower_text(R"(
        module top;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            s_t y;
            initial y = x;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    REQUIRE(r.module != nullptr);

    const ir::Module& m = *r.module;
    REQUIRE(m.signals().size() == 2);
    const ir::Ref ty = m.signals()[0].type;

    const ir::Layout lay = m.pool.layout(ty);
    // a (4-bit logic) makes the whole struct four-state; total width 4 + 8.
    CHECK(lay.domain == ir::Domain::Four);
    CHECK(lay.width == 12);

    const ir::Type& t = std::get<ir::Type>(m.pool.get(ty));
    REQUIRE(t.kind == ir::Type::Kind::Struct);
    CHECK(t.is_packed);
    const auto fields = m.pool.struct_fields(t.fields_id);
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].name == "a");
    CHECK(fields[1].name == "b");
    // LRM 7.2.1: the first declared member is the most significant, so `a`
    // (the second field, width 8) sits above `b` at the LSB.
    CHECK(fields[0].bit_offset == 8);
    CHECK(fields[1].bit_offset == 0);
}

TEST_CASE("an unpacked struct with a real member lowers (recursive aggregate)") {
    // The recursive aggregate model backs a real / string / array / nested-struct
    // member; only an unpacked union member stays guarded.
    const LowerResult r = lower_text(R"(
        module top;
            typedef struct {
                logic [3:0] a;
                real        b;
            } s_t;
            s_t x;
            initial begin x.a = 4'h1; x.b = 2.5; end
        endmodule
    )");
    CHECK(r.ok);
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

// -- Type parameters (LRM 6.20.3) --

TEST_CASE("a type parameter default sets a port width") {
    const LowerResult r = lower_text(R"(
        module m #(parameter type T = logic [7:0]) (output T o);
            assign o = 8'hFF;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "o") == 8);
}

TEST_CASE("a #() type override specializes each instance's width independently") {
    const LowerResult r = lower_text(R"(
        module m #(parameter type T = logic [7:0]) (output T o);
            assign o = 0;
        endmodule
        module top(output [2:0] a, output [15:0] b);
            m #(.T(logic [2:0])) u0(.o(a));
            m #(.T(logic [15:0])) u1(.o(b));
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "u0.o") == 3);
    CHECK(signal_width(*r.module, "u1.o") == 16);
}

TEST_CASE("a type parameter default sets an internal variable's width") {
    const LowerResult r = lower_text(R"(
        module m #(parameter type T = logic [3:0]) (output [3:0] o);
            T v;
            assign v = 4'd5;
            assign o = v;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "v") == 4);
}

TEST_CASE("a type parameter bound to an unsupported type fails loudly") {
    // `chandle` is not integral/real/string/array/event, so it falls through
    // lower_type's final guard; binding a type parameter to one must hit
    // that guard too, not silently accept it. (`event` was the
    // prior example here; it is now lowered as a named event, LRM 15.5.)
    const LowerResult r = lower_text(R"(
        module m #(parameter type T = chandle);
            T v;
        endmodule
    )");
    CHECK(!r.ok);
    CHECK(r.diagnostics.find("unsupported non-integral type") != std::string::npos);
}

TEST_CASE("a type parameter flows into a function argument and return type") {
    const LowerResult r = lower_text(R"(
        module m #(parameter type T = logic [7:0]) (input T a, output T o);
            function automatic T f(T x);
                f = x;
            endfunction
            assign o = f(a);
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(signal_width(*r.module, "a") == 8);
    CHECK(signal_width(*r.module, "o") == 8);
}

TEST_CASE("a task body neither adds to nor subtracts from always_comb sensitivity") {
    // LRM 9.2.2.2.1: a called task's argument expressions are sensitive, but
    // the task's contents contribute nothing -- in particular a task-body
    // write must not subtract a signal the block itself reads.
    const LowerResult r = lower_text(R"(
        module m(input a, input b, input g, output reg y);
            reg side;
            task automatic t;
                side = g;
            endtask
            always_comb begin
                y = a | b;
                if (b) t();
            end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // g (read only inside the task) is out; a and b stay in. Before the fix
    // the walk descended into t, so g leaked in.
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "b" });
}

TEST_CASE("a task-body write does not subtract a block-level read") {
    const LowerResult r = lower_text(R"(
        module m(input b, output reg y);
            reg a;
            task automatic t;
                a = 1'b1;
            endtask
            always_comb begin
                y = a | b;
                if (b) t();
            end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    // a is read at block level; the write inside t is a task write and does
    // not exclude it (only block / called-function writes do).
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "b" });
}

TEST_CASE("always @* keeps an intermediate that is both read and written") {
    // LRM 9.4.2.2 (Example 2): @* excludes only identifiers appearing solely
    // as an assignment's LHS; tmp is read, so it stays sensitive -- unlike
    // always_comb, which subtracts written signals (9.2.2.2.1 (b)).
    const LowerResult r = lower_text(R"(
        module m(input a, input b, input c, output reg y);
            reg t;
            always @* begin
                t = a & b;
                y = t | c;
            end
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);

    const ir::Process* p = find_level_process(*r.module);
    REQUIRE(p != nullptr);
    CHECK(sensitivity_names(*r.module, *p) == std::set<std::string>{ "a", "b", "c", "t" });
}

TEST_CASE("always @* is sensitive to function arguments, not function contents") {
    // LRM 9.2.2.2.2: @* sees only a called function's arguments; always_comb
    // additionally sees the signals the function body reads.
    const LowerResult star = lower_text(R"(
        module m(input a, input g, output reg y);
            function automatic logic f(logic x);
                return x ^ g;
            endfunction
            always @* y = f(a);
        endmodule
    )");
    REQUIRE_MESSAGE(star.ok, star.diagnostics);
    const ir::Process* sp = find_level_process(*star.module);
    REQUIRE(sp != nullptr);
    CHECK(sensitivity_names(*star.module, *sp) == std::set<std::string>{ "a" });

    const LowerResult comb = lower_text(R"(
        module m(input a, input g, output reg y);
            function automatic logic f(logic x);
                return x ^ g;
            endfunction
            always_comb y = f(a);
        endmodule
    )");
    REQUIRE_MESSAGE(comb.ok, comb.diagnostics);
    const ir::Process* cp = find_level_process(*comb.module);
    REQUIRE(cp != nullptr);
    CHECK(sensitivity_names(*comb.module, *cp) == std::set<std::string>{ "a", "g" });
}

TEST_CASE("an always_comb that infers a latch warns but still lowers") {
    // LRM 9.2.2.2: tools should warn when an always_comb does not model
    // combinational logic; slang's dataflow analysis reports it.
    const LowerResult r = lower_text(R"(
        module m(input en, input d, output reg q);
            always_comb
                if (en) q = d;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(r.diagnostics.find("latch inferred") != std::string::npos);
}

TEST_CASE("a blocking task called from always_comb is rejected") {
    // LRM 9.2.2.2: statements in an always_comb shall not block. The direct
    // form is caught at elaboration; the called-task form needs analysis.
    const LowerResult r = lower_text(R"(
        module m(output reg y);
            task automatic t;
                #1 y = 1'b1;
            endtask
            always_comb t();
        endmodule
    )");
    CHECK(!r.ok);
    CHECK(r.diagnostics.find("blocking timing control") != std::string::npos);
}

TEST_CASE("a variable driven by always_comb and another process warns") {
    const LowerResult r = lower_text(R"(
        module m(output reg y);
            always_comb y = 1'b0;
            initial y = 1'b1;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(r.diagnostics.find("cannot be written to by any other process") != std::string::npos);
}

TEST_CASE("a timing-control-free always warns but still lowers") {
    // slang's analysis makes this a hard error; clotho lowers the zero-delay
    // infinite loop faithfully (LRM 9.2.2.1 calls it a deadlock, not illegal),
    // so the severity is downgraded to a warning.
    const LowerResult r = lower_text(R"(
        module m;
            reg a;
            always a = ~a;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
    CHECK(r.diagnostics.find("does not advance time") != std::string::npos);
}

// -- Multiple continuous drivers on one net (LRM 6.6.1) --
//
// Clotho has no net resolution: overlapping continuous/structural drivers
// must fail loudly instead of silently last-writer-winning (Table 6-2 says
// conflicting wire drivers resolve to x).

TEST_CASE("two continuous assigns driving one net fail loudly") {
    const LowerResult r = lower_text(R"(
        module m;
            wire w;
            assign w = 1'b0;
            assign w = 1'b1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("multiple drivers on net 'w'") != std::string::npos);
}

TEST_CASE("a net declaration assignment plus a continuous assign fails loudly") {
    const LowerResult r = lower_text(R"(
        module m(input a);
            wire w = 1'b0;
            assign w = a;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("multiple drivers on net 'w'") != std::string::npos);
}

TEST_CASE("an output port connection plus a continuous assign fails loudly") {
    const LowerResult r = lower_text(R"(
        module sub(output y);
            assign y = 1'b0;
        endmodule
        module m;
            wire w;
            sub u(.y(w));
            assign w = 1'b1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("multiple drivers on net 'w'") != std::string::npos);
}

TEST_CASE("overlapping constant part-select drivers fail loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] a, input [4:0] b);
            wire [7:0] w;
            assign w[3:0] = a;
            assign w[6:2] = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("multiple drivers on net 'w'") != std::string::npos);
}

TEST_CASE("a concatenation lvalue operand overlapping another driver fails loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [1:0] a, input b);
            wire x, y;
            assign {x, y} = a;
            assign y = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("multiple drivers on net 'y'") != std::string::npos);
}

TEST_CASE("non-overlapping constant bit drivers of one net are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b, output [1:0] y);
            wire [1:0] w;
            assign w[0] = a;
            assign w[1] = b;
            assign y = w;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostics);
}

TEST_CASE("non-overlapping constant part-select drivers are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] a, input [3:0] b, output [7:0] y);
            wire [7:0] w;
            assign w[3:0] = a;
            assign w[7:4] = b;
            assign y = w;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostics);
}

// -- Unpacked net arrays driven one element at a time (LRM 6.6.1) --
//
// Distinct constant array elements are distinct storage, so per-element
// continuous drivers are single-driver and must be accepted; the guard tracks
// an element band, not just a packed bit range.

TEST_CASE("distinct constant elements of an unpacked net array are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input [7:0] a, input [7:0] b);
            wire [7:0] mem [0:3];
            assign mem[0] = a;
            assign mem[1] = b;
            assign mem[2] = a ^ b;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostics);
}

TEST_CASE("distinct elements of a scalar unpacked net array are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b);
            wire w [0:1];
            assign w[0] = a;
            assign w[1] = b;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostics);
}

TEST_CASE("distinct elements of a multidimensional net array are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input [7:0] a, input [7:0] b, input [7:0] c, input [7:0] d);
            wire [7:0] mem [0:1][0:1];
            assign mem[0][0] = a;
            assign mem[0][1] = b;
            assign mem[1][0] = c;
            assign mem[1][1] = d;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostics);
}

TEST_CASE("disjoint bits of one net-array element are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] a, input [3:0] b);
            wire [7:0] mem [0:1];
            assign mem[0][3:0] = a;
            assign mem[0][7:4] = b;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostics);
}

TEST_CASE("two drivers of the same net-array element fail loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [7:0] a, input [7:0] b);
            wire [7:0] mem [0:3];
            assign mem[0] = a;
            assign mem[0] = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("multiple drivers on net 'mem'") != std::string::npos);
}

TEST_CASE("overlapping bit drivers of one net-array element fail loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [7:0] a, input [7:0] b);
            wire [7:0] mem [0:1];
            assign mem[0][3:0] = a;
            assign mem[0][2:1] = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("multiple drivers on net 'mem'") != std::string::npos);
}

TEST_CASE("a net with one continuous driver read procedurally is accepted") {
    // Procedural blocks are not continuous drivers: one assign plus reads
    // (including in an always body) must not trip the multi-driver guard.
    const LowerResult r = lower_text(R"(
        module m(input a, output reg q);
            wire w;
            assign w = a;
            always @(w) q = w;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostics);
}

TEST_CASE("a non-default drive strength fails loudly") {
    const LowerResult ra = lower_text(R"(
        module m(input a);
            wire w;
            assign (weak0, weak1) w = a;
        endmodule
    )");
    CHECK_FALSE(ra.ok);
    CHECK(ra.diagnostics.find("drive strength") != std::string::npos);

    const LowerResult rn = lower_text(R"(
        module m(input a);
            wire (pull0, pull1) w = a;
        endmodule
    )");
    CHECK_FALSE(rn.ok);
    CHECK(rn.diagnostics.find("drive strength") != std::string::npos);

    // An explicit (strong0, strong1) equals the default (LRM 10.3.4): accepted.
    const LowerResult rs = lower_text(R"(
        module m(input a);
            wire w;
            assign (strong0, strong1) w = a;
        endmodule
    )");
    CHECK_MESSAGE(rs.ok, rs.diagnostics);
}

// -- Net delay (LRM 28.16) --

TEST_CASE("a net declaration delay fails loudly") {
    const LowerResult ri = lower_text(R"(
        module m(input a);
            wire #5 w = a;
        endmodule
    )");
    CHECK_FALSE(ri.ok);
    CHECK(ri.diagnostics.find("net delay") != std::string::npos);

    const LowerResult rd = lower_text(R"(
        module m(input a);
            wire #5 w;
            assign w = a;
        endmodule
    )");
    CHECK_FALSE(rd.ok);
    CHECK(rd.diagnostics.find("net delay") != std::string::npos);

    // No delay: a plain net declaration assignment still lowers.
    const LowerResult rp = lower_text(R"(
        module m(input a);
            wire w = a;
        endmodule
    )");
    CHECK_MESSAGE(rp.ok, rp.diagnostics);
}

// -- `matches` pattern conditions (LRM 12.6.2) --

TEST_CASE("a matches pattern in an if condition fails loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] v, output reg q);
            always @(*)
                if (v matches 4'd3) q = 1'b1;
                else q = 1'b0;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("pattern matching in conditions") != std::string::npos);
}

TEST_CASE("a matches pattern in a conditional expression fails loudly") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] v, output y);
            assign y = v matches 4'd3 ? 1'b1 : 1'b0;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.find("pattern matching in conditions") != std::string::npos);
}
