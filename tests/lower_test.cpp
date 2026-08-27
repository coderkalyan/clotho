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
template<class Pred>
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
    return find_process(
      m, [](const ir::Process& p) { return p.activation == ir::Activation::Always && !is_edge_triggered(p); });
}

} // namespace

TEST_CASE("lowering the registered ALU produces a module") {
    const LowerResult r = lower_text(kAlu);

    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(rz.ok, rz.diagnostic_text());
    CHECK(has_tag(find_level_process(*rz.module)->ir, ir::Inst::Tag::CasezEq));

    const LowerResult rx = lower_case_module("casex", "2'b1x: x = 2'b01; default: x = 2'b00;");
    REQUIRE_MESSAGE(rx.ok, rx.diagnostic_text());
    CHECK(has_tag(find_level_process(*rx.module)->ir, ir::Inst::Tag::CasexEq));
}

TEST_CASE("grouped constant labels share one switch arm") {
    const LowerResult r = lower_case_module("case", "2'd0, 2'd1: x = 2'b01; default: x = 2'b00;");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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

TEST_CASE("$random with a seed argument lowers (threads the inout seed)") {
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
    CHECK(r.ok);
}

TEST_CASE("a real (fractional) delay lowers (LRM 3.14 / 22.7)") {
    const LowerResult r = lower_text(R"(
        module m;
            initial #1.5 $display("x");
        endmodule
    )");
    CHECK(r.ok);
}

// LRM 6.5 / 10.3.2: "shall be an error" for a variable driven by more than one
// continuous assignment, or by a mix of continuous and procedural assignments.
// slang defaults both to warnings; Clotho promotes them to errors (its signal
// model has no multi-driver resolution for variables, so silently keeping the
// default severity would mean one driver is silently dropped).
TEST_CASE("mixing continuous and procedural assignment to a variable is an error") {
    const LowerResult r = lower_text(R"(
        module m;
            logic a;
            assign a = 1'b0;
            initial a = 1'b1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("cannot mix continuous and procedural assignments") != std::string::npos);
}

TEST_CASE("multiple continuous assignments to a variable is an error") {
    const LowerResult r = lower_text(R"(
        module m;
            logic a;
            assign a = 1'b0;
            assign a = 1'b1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("cannot have multiple continuous assignments") != std::string::npos);
}

// slang also defaults a same-scope name redefinition to a warning and
// silently keeps one declaration; Clotho's symbol table has no "last one
// wins" rule, so this is promoted to an error too.
TEST_CASE("redefining a name in the same scope is an error") {
    const LowerResult r = lower_text(R"(
        module m;
            logic a;
            logic a;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("redefinition of") != std::string::npos);
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
    CHECK(r.diagnostic_text().find("suffix must be a string literal") != std::string::npos);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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

    // .string(), not .native(): path::string_type is wstring on Windows.
    const std::string                     top_path{ top.path.string() };
    const std::string                     child_path{ child.path.string() };
    const std::array<std::string_view, 2> paths{ top_path, child_path };
    const LowerResult                     r = lower_files(paths);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(r.module->name == "top");
    CHECK(signal_names(*r.module) == std::set<std::string>{ "g", "o", "w", "u_inv.a", "u_inv.y" });
}

TEST_CASE("lower_files reports a missing file loudly") {
    const std::array<std::string_view, 1> paths{ "/no/such/clotho/file.sv" };
    const LowerResult                     r = lower_files(paths);
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("cannot read") != std::string::npos);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    const std::set<std::string> names = signal_names(*r.module);
    CHECK(names.contains("u0.a"));
    CHECK(names.contains("u0.y"));
    CHECK(names.contains("u1.a"));
    CHECK(names.contains("u1.y"));
}

// An inout port lowers via the shared-storage collapse (LRM 23.3.3); behavior is
// exercised in xhier_test.cpp.
TEST_CASE("an inout port connection lowers (shared storage)") {
    const LowerResult r = lower_text(R"(
        module pass(inout a);
        endmodule
        module top(inout b);
            pass u(.a(b));
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("whole unpacked array blocking assignment lowers (memcpy)") {
    // A blocking copy of a string-free unpacked array is a single memcpy; a
    // string array (heap-owning elements) is still guarded.
    const LowerResult r = lower_text(R"(
        module top;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            initial a = b;
        endmodule
    )");
    CHECK(r.ok);
}

TEST_CASE("nonblocking whole unpacked array assignment lowers") {
    // A whole-array `a <= b` decomposes to per-element StoreNb, each riding the
    // backend's deferred array-NBA queue. Value semantics are pinned in
    // assign_pattern_test.cpp against verilator.
    const LowerResult r = lower_text(R"(
        module top;
            logic [7:0] a [0:3];
            logic [7:0] b [0:3];
            logic clk;
            always @(posedge clk) a <= b;
        endmodule
    )");
    CHECK(r.ok);
}

// A fixed-size unpacked array slice (LRM 7.4.5: "a slice name of an unpacked
// array is an unpacked array", e.g. `busB = busA[7:6]`) is element-addressed
// storage, lowered as a position-0-anchored ArrSlice lvalue consumed by the
// per-element copy / equality / store paths (not a packed part-select). Value
// semantics (element-order, descending bases, equality, NBA) are pinned in
// assign_pattern_test.cpp against verilator.
TEST_CASE("a fixed-size unpacked array slice lowers in each context") {
    SUBCASE("as a comparison operand") {
        const LowerResult r = lower_text(R"(
            module top;
                bit [31:0] a [7:0];
                bit [31:0] b [7:0];
                logic eq;
                initial eq = a[7:6] == b[7:6];
            endmodule
        )");
        CHECK(r.ok);
    }
    SUBCASE("as an assignment rvalue") {
        const LowerResult r = lower_text(R"(
            module top;
                bit signed [31:0] a [7:0];
                int b [1:0];
                initial b = a[7:6];
            endmodule
        )");
        CHECK(r.ok);
    }
    SUBCASE("as an assignment lvalue") {
        const LowerResult r = lower_text(R"(
            module top;
                bit signed [31:0] a [7:0];
                int b [1:0];
                initial a[7:6] = b;
            endmodule
        )");
        CHECK(r.ok);
    }
    SUBCASE("an indexed part-select slice lowers in the same contexts") {
        // `a[i +: 2]` has no anchored sub-array (its first position is a runtime
        // value), so it expands position-wise instead of becoming an ArrSlice.
        const LowerResult r = lower_text(R"(
            module top;
                bit [31:0] a [7:0];
                bit [31:0] b [1:0];
                logic eq;
                int i;
                initial begin
                    b = a[i +: 2];
                    a[i -: 2] = b;
                    a[i +: 2] <= b;
                    eq = a[i +: 2] == b;
                end
            endmodule
        )");
        CHECK(r.ok);
    }
    SUBCASE("an indexed part-select slice in an unsupported context is guarded") {
        // A general by-value read (here `%p`) still needs the whole-array rvalue
        // ABI; the diagnostic names the contexts that do work.
        const LowerResult r = lower_text(R"(
            module top;
                bit [31:0] a [7:0];
                int i;
                initial $display("%p", a[i +: 2]);
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("indexed part-select slice of an unpacked array") != std::string::npos);
    }
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

TEST_CASE("a dynamic array lowers (LRM 7.5)") {
    const LowerResult r = lower_text(R"(
        module top;
            int dyn [];
            initial dyn = new [4];
        endmodule
    )");
    CHECK(r.ok);
}

TEST_CASE("a queue lowers (LRM 7.10)") {
    const LowerResult r = lower_text(R"(
        module top;
            int q [$];
            initial q.push_back(1);
        endmodule
    )");
    CHECK(r.ok);
}

// Associative arrays (LRM 7.8/7.9) lower and run; see
// associative_array_test.cpp. A string / nested-container VALUE element is the
// remaining guarded gap.
TEST_CASE("an associative array lowers") {
    const LowerResult r = lower_text(R"(
        module top;
            int a [string];
            initial a["x"] = 1;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("an associative array with a class-handle value element lowers") {
    // A class handle is a plain 64-bit word: objects are never freed
    // (docs/design/classes.md §7), so an entry needs no lifecycle beyond the POD
    // copy, `null` is the all-zero Table 7-1 default, and `==` is the identity
    // compare a word compare gives. That is the same reasoning a queue / dynamic
    // -array element of class-handle type already relies on, so the assoc value
    // element admits exactly the set guard_container_element does.
    const LowerResult r = lower_text(R"(
        class C; int x; endclass
        module top;
            C a [int];
            initial a[0] = null;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("nonblocking whole-struct copy with a queue member fails loudly") {
    const LowerResult r = lower_text(R"(
        module top;
            typedef struct { int q[$]; int x; } t;
            t a, b;
            initial b <= a;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("queue / dynamic array member") != std::string::npos);
}

TEST_CASE("equality of a struct with a queue member lowers (element-wise walk)") {
    // The AggEq leaf dispatch now reaches rt_container_eq for the queue member
    // (runtime behavior pinned in queue_test.cpp); here just confirm it lowers.
    const LowerResult r = lower_text(R"(
        module top;
            typedef struct { int q[$]; int x; } t;
            t a, b;
            initial $display("%0d", a == b);
        endmodule
    )");
    CHECK(r.ok);
}

TEST_CASE("queue member assignment from a pattern builds the queue element-wise") {
    const LowerResult r = lower_text(R"(
        module top;
            typedef struct { int q[$]; int x; } t;
            t a;
            initial a = '{ '{1, 2}, 3 };
        endmodule
    )");
    CHECK(r.ok);
}

// An event in an aggregate is one event PER LEAF, bound when the storage comes
// into existence (LRM Table 6-7's "New event"): a fixed unpacked array binds each
// element in init_state's array loop, a struct member and a class property in the
// aggregate init walk that module signals, frame locals and object bodies share.
TEST_CASE("an event struct member declares and lowers") {
    const LowerResult r = lower_text(R"(
        module top;
            typedef struct { event e; int x; } t;
            t a;
            initial begin a.x = 1; -> a.e; end
        endmodule
    )");
    CHECK(r.ok);
    CHECK(r.diagnostic_text().find("event struct member") == std::string::npos);
}

TEST_CASE("an unpacked array of events declares and lowers") {
    const LowerResult r = lower_text(R"(
        module top;
            event ev [4];
            initial ->ev[0];
        endmodule
    )");
    CHECK(r.ok);
}

TEST_CASE("an event class property declares and lowers") {
    const LowerResult r = lower_text(R"(
        module top;
            class C; event e; endclass
            C c;
            initial begin c = new; -> c.e; end
        endmodule
    )");
    CHECK(r.ok);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    CHECK(m.pool.field_name(fields[0]) == "a");
    CHECK(m.pool.field_name(fields[1]) == "b");
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(signal_width(*r.module, "u0.o") == 3);
    CHECK(signal_width(*r.module, "u1.o") == 8);
}

TEST_CASE("a parameter referenced in an rvalue folds to a constant, not a signal read") {
    const LowerResult r = lower_text(R"(
        module m #(parameter W = 5) (input [7:0] a, output [7:0] y);
            assign y = a + W;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(signal_width(*r.module, "o") == 8);
}

TEST_CASE("a parameter expression in a part-select bound folds") {
    const LowerResult r = lower_text(R"(
        module m #(parameter W = 8) (input [W-1:0] a, output [1:0] o);
            assign o = a[W-1:W-2];
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(signal_width(*r.module, "o") == 2);
}

// -- Type parameters (LRM 6.20.3) --

TEST_CASE("a type parameter default sets a port width") {
    const LowerResult r = lower_text(R"(
        module m #(parameter type T = logic [7:0]) (output T o);
            assign o = 8'hFF;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(signal_width(*r.module, "v") == 4);
}

TEST_CASE("a type parameter bound to an unsupported type fails loudly") {
    // Binding a type parameter to a not-yet-lowered type must hit the same loud
    // guard as a direct declaration, not silently accept it. (`event`,
    // `chandle`, `class`, a virtual interface, an associative array with a
    // class-handle value and one with an EVENT value, and an associative array
    // keyed by a chandle / event / virtual interface / unpacked union, were all
    // prior examples here; every one of them lowers now.) `mailbox #(T)` bound to
    // T = event stands in as the still-unsupported type, reached THROUGH the type
    // parameter rather than written directly: an event ELEMENT's construction
    // mints a new sync object (LRM Table 6-7), which mailbox's copy-in would
    // overwrite and leak (Lower.cpp:4148).
    const LowerResult r = lower_text(R"(
        module m #(parameter type T = int);
            mailbox #(T) v;
        endmodule
        module top;
            m #(.T(event)) u();
        endmodule
    )");
    CHECK(!r.ok);
    CHECK(r.diagnostic_text().find("a mailbox of events is not supported") != std::string::npos);
}

TEST_CASE("a class type parameter lowers as a handle") {
    // The class example above now lowers: a class-typed signal is a handle (a
    // 2-state 64-bit value), so binding a type parameter to a class succeeds.
    const LowerResult r = lower_text(R"(
        class C; int f; endclass
        module m #(parameter type T = C);
            T v;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

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
    REQUIRE_MESSAGE(star.ok, star.diagnostic_text());
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
    REQUIRE_MESSAGE(comb.ok, comb.diagnostic_text());
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(r.diagnostic_text().find("latch inferred") != std::string::npos);
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
    CHECK(r.diagnostic_text().find("blocking timing control") != std::string::npos);
}

TEST_CASE("a variable driven by always_comb and another process warns") {
    const LowerResult r = lower_text(R"(
        module m(output reg y);
            always_comb y = 1'b0;
            initial y = 1'b1;
        endmodule
    )");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(r.diagnostic_text().find("cannot be written to by any other process") != std::string::npos);
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
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(r.diagnostic_text().find("does not advance time") != std::string::npos);
}

// -- Multiple continuous drivers on one net (LRM 6.6) --
//
// Multi-driver net resolution (LRM 6.6, no strengths): overlapping drivers of a
// packed net resolve through per-driver slots + a resolver (whole-net drivers
// stage their value, constant packed part-select drivers stage it with a Z
// background so each bit resolves only across its own drivers). Concatenation-
// operand, dynamic-index, and unpacked-array/struct drivers of a resolved net
// are not yet supported and fail loudly (never silently last-writer-win).
// Runtime resolution values are checked in net_resolution_test.cpp against
// iverilog.

TEST_CASE("two whole-net continuous assigns resolve") {
    const LowerResult r = lower_text(R"(
        module m;
            wire w;
            assign w = 1'b0;
            assign w = 1'b1;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("a net declaration assignment plus a whole-net continuous assign resolve") {
    const LowerResult r = lower_text(R"(
        module m(input a);
            wire w = 1'b0;
            assign w = a;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("an output port connection plus a whole-net continuous assign resolve") {
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
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("overlapping constant part-select drivers resolve") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] a, input [4:0] b);
            wire [7:0] w;
            assign w[3:0] = a;
            assign w[6:2] = b;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
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
    CHECK(r.diagnostic_text().find("concatenation driver of a multiply-driven net") != std::string::npos);
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
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
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
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
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
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("distinct elements of a scalar unpacked net array are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input a, input b);
            wire w [0:1];
            assign w[0] = a;
            assign w[1] = b;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
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
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("disjoint bits of one net-array element are accepted") {
    const LowerResult r = lower_text(R"(
        module m(input [3:0] a, input [3:0] b);
            wire [7:0] mem [0:1];
            assign mem[0][3:0] = a;
            assign mem[0][7:4] = b;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("two drivers of the same net-array element fail loudly") {
    // An unpacked-array net has no packed plane the resolver can fold; per-element
    // resolution is a documented follow-up, so it fails loudly rather than winning.
    const LowerResult r = lower_text(R"(
        module m(input [7:0] a, input [7:0] b);
            wire [7:0] mem [0:3];
            assign mem[0] = a;
            assign mem[0] = b;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("unpacked-array or unpacked-struct multiply-driven net") != std::string::npos);
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
    CHECK(r.diagnostic_text().find("unpacked-array or unpacked-struct multiply-driven net") != std::string::npos);
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
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

// Drive strengths (LRM 6.6.1, 28.11) lower and resolve; the multi-driver
// resolution semantics are exercised in net_resolution_test.cpp.
TEST_CASE("drive strengths lower on continuous assigns and net declarations") {
    const LowerResult ra = lower_text(R"(
        module m(input a);
            wire w;
            assign (weak0, weak1) w = a;
        endmodule
    )");
    CHECK_MESSAGE(ra.ok, ra.diagnostic_text());

    const LowerResult rn = lower_text(R"(
        module m(input a);
            wire (pull0, pull1) w = a;
        endmodule
    )");
    CHECK_MESSAGE(rn.ok, rn.diagnostic_text());

    const LowerResult rs = lower_text(R"(
        module m(input a);
            wire w;
            assign (strong0, strong1) w = a;
        endmodule
    )");
    CHECK_MESSAGE(rs.ok, rs.diagnostic_text());
}

// -- Net delay (LRM 28.16) -- full behavior coverage (timing, multi-driver
// resolution, wide values, and the Delay3 / aggregate guards) lives in
// net_delay_test.cpp; this is a lowering-only sanity check that both forms
// compile.

TEST_CASE("a net declaration delay lowers") {
    const LowerResult ri = lower_text(R"(
        module m(input a);
            wire #5 w = a;
        endmodule
    )");
    CHECK_MESSAGE(ri.ok, ri.diagnostic_text());

    const LowerResult rd = lower_text(R"(
        module m(input a);
            wire #5 w;
            assign w = a;
        endmodule
    )");
    CHECK_MESSAGE(rd.ok, rd.diagnostic_text());

    // No delay: a plain net declaration assignment still lowers.
    const LowerResult rp = lower_text(R"(
        module m(input a);
            wire w = a;
        endmodule
    )");
    CHECK_MESSAGE(rp.ok, rp.diagnostic_text());
}

// -- `matches` pattern conditions (LRM 12.6.2) --

TEST_CASE("a matches pattern in an if condition lowers (LRM 12.6.2)") {
    // `if (v matches <pattern>)` is supported (Phase 2). Behavior — tagged/
    // variable/structure patterns, `&&&` filters — is exercised end to end in
    // struct_test.cpp; here just confirm the condition form elaborates.
    const LowerResult r = lower_text(R"(
        module m(input [3:0] v, output reg q);
            always @(*)
                if (v matches 4'd3) q = 1'b1;
                else q = 1'b0;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

TEST_CASE("a matches pattern in a conditional expression lowers (LRM 12.6.2)") {
    // `expr matches pattern ? a : b` is supported (Phase 2); behavior (incl. the
    // lazy single-evaluation path with a bound pattern var) is exercised in
    // struct_test.cpp.
    const LowerResult r = lower_text(R"(
        module m(input [3:0] v, output y);
            assign y = v matches 4'd3 ? 1'b1 : 1'b0;
        endmodule
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
}

// A compilation unit with no top-level module/interface/program instance
// (e.g. a package-only file) has nothing to simulate, but is not an
// elaboration error (LRM 23.3 resolves whatever top instances exist; nothing
// obligates a compilation unit to declare any).
TEST_CASE("a package-only source with no module compiles successfully") {
    const LowerResult r = lower_text(R"(
        package p;
            parameter int W = 8;
            function int double_w(); return W * 2; endfunction
        endpackage
    )");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(r.module != nullptr);
}

// Same for a bare $unit-scope declaration with no module at all.
TEST_CASE("a source with only a $unit-scope declaration compiles successfully") {
    const LowerResult r = lower_text("parameter int W = 8;");
    CHECK_MESSAGE(r.ok, r.diagnostic_text());
    CHECK(r.module != nullptr);
}

// Regression: a bit-stream cast of a dynamic array to a packed vector
// (`byte_vector'(a)`) fell through to the plain scalar `coerce`/resize path,
// which silently produced the wrong value for a variable operand and
// SIGSEGV'd for a function-call operand (the call's temporary dynamic-array
// return handle). Neither is implemented (LRM 6.24.2 needs an element-wise
// pack of the heap contents); guard loudly instead of miscompiling or
// crashing. Fixes the sv-tests ivtest sv_darray_function crash.
TEST_CASE("bit-stream cast of a dynamic array to a packed type fails loudly") {
    const LowerResult r = lower_text(R"(
        module top;
            typedef logic [7:0] byte_array [];
            typedef logic [23:0] byte_vector;
            byte_vector c;
            byte_array a;
            initial begin
                a = new[3];
                c = byte_vector'(a);
            end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("bit-stream cast") != std::string::npos);
}

TEST_CASE("bit-stream cast of a dynamic-array-returning call fails loudly") {
    const LowerResult r = lower_text(R"(
        module top;
            typedef logic [7:0] byte_array [];
            typedef logic [23:0] byte_vector;
            function byte_array make();
                byte_array tmp;
                tmp = new[3];
                return tmp;
            endfunction
            byte_vector c;
            initial c = byte_vector'(make());
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("bit-stream cast") != std::string::npos);
}

// -- lower_buffer: the language server's entry point ------------------------

TEST_CASE("lower_buffer attributes diagnostics to the name it was given") {
    const LowerResult r = lower_buffer("/work/latchy.sv", R"(
        module latchy(input en, input d, output logic q);
            always_comb begin
                if (en) q = d;
            end
        endmodule
    )");

    REQUIRE(r.diagnostics.size() >= 1);
    const SourcePosition pos = r.sources->resolve(r.diagnostics[0].span.start);
    CHECK(pos.valid);
    CHECK(r.sources->full_path(r.diagnostics[0].span.start) == "/work/latchy.sv");
    CHECK(r.diagnostic_text().find("latch inferred") != std::string::npos);
}

TEST_CASE("lower_buffer analyses a file whose dependencies are absent") {
    // The editor case: one file of a design, opened on its own. Without lint
    // mode this reports `unknown module` per instance and analyses nothing.
    const LowerResult r = lower_buffer("/work/parent.sv", R"(
        module parent(input en, input d, output logic q, output logic [3:0] y);
            child u (.y(y));
            always_comb begin
                if (en) q = d;
            end
        endmodule
    )");

    CHECK(r.diagnostic_text().find("unknown module") == std::string::npos);
    // And the rest of the module is still analysed, which is the point.
    CHECK(r.diagnostic_text().find("latch inferred") != std::string::npos);
}

TEST_CASE("lower_buffer owns its source manager, so revisions do not collide") {
    // Same path twice, which is every keystroke. slang's SourceManager is
    // append-only and assignBuffer throws on a path it has already seen, so
    // this only works because each call builds its own manager -- the same
    // property that makes the entry point reentrant, unlike lower_text.
    constexpr std::string_view kFirst  = "module a; logic q; always_comb if (1) q = 0; endmodule";
    constexpr std::string_view kSecond = "\n\nmodule a; logic q; always_comb if (1) q = 0; endmodule";

    const LowerResult first  = lower_buffer("/work/a.sv", kFirst);
    const LowerResult second = lower_buffer("/work/a.sv", kSecond);

    REQUIRE(!first.diagnostics.empty());
    REQUIRE(!second.diagnostics.empty());

    // The older result stays resolvable after the newer one exists: its context
    // owns the manager its locations were minted against, so an in-flight
    // analysis cannot be invalidated by the next keystroke.
    const SourcePosition old_pos = first.sources->resolve(first.diagnostics[0].span.start);
    const SourcePosition new_pos = second.sources->resolve(second.diagnostics[0].span.start);
    CHECK(old_pos.valid);
    CHECK(new_pos.valid);
    CHECK(first.sources->full_path(first.diagnostics[0].span.start) == "/work/a.sv");
    CHECK(second.sources->full_path(second.diagnostics[0].span.start) == "/work/a.sv");
    // Two blank lines were prepended, and only the newer context knows that.
    CHECK(new_pos.line == old_pos.line + 2);
}

TEST_CASE("lower_buffer keeps use-based findings but drops driver-based ones") {
    // The split matters because a buffer is compiled without the rest of its
    // design. slang binds an absent submodule's port connections as ordinary
    // expressions, so a net wired to one reads as USED -- sound, the connection
    // is a use -- but never as DRIVEN, because nothing says which port is an
    // output. Reporting the second family would invent a problem per connection
    // (measured: 108 of them on hazard3_core.v opened alone).
    SUBCASE("a net driven only by an absent module is not reported undriven") {
        const LowerResult r = lower_buffer("/work/parent.sv", R"(
            module parent(output logic [3:0] y);
                logic [3:0] internal;
                child u (.o(internal));
                assign y = internal;
            endmodule
        )");
        CHECK(r.diagnostic_text().find("never driven") == std::string::npos);
        CHECK(r.diagnostic_text().find("is never assigned") == std::string::npos);
    }

    SUBCASE("a net nothing ever reads is still reported unused") {
        // No instances at all, so nothing about this file is partial.
        const LowerResult r = lower_buffer("/work/test.sv", R"(
            module test ();
                wire [2:0] foo = 4'b0000;
            endmodule
        )");
        CHECK(r.diagnostic_text().find("unused net 'foo'") != std::string::npos);
    }
}
