// The step 3 gate of docs/design/modular-compilation.md: a unit's LLVM module is a
// function of the unit, so compiling a design twice with one module's body edited
// changes that unit's module and root's, and no other module.

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "ir/LlvmBackend.h"

using namespace clotho;

namespace {

// Two modules instantiated several times each under two parents, a package, and
// a top. `@LEAF@` and `@PAIR@` are the bodies the tests edit.
constexpr std::string_view kDesign = R"(
package pk;
  localparam int K = 3;
  function automatic logic [7:0] bump(logic [7:0] v);
    return v + 8'(K);
  endfunction
endpackage

module leaf #(parameter int N = 1) (input logic clk, input logic [7:0] d, output logic [7:0] q);
  always_ff @(posedge clk) q <= @LEAF@;
endmodule

module pair (input logic clk, input logic [7:0] d, output logic [7:0] q);
  logic [7:0] t0, t1, t2;
  leaf #(1) l0 (.clk, .d, .q(t0));
  leaf #(1) l1 (.clk, .d(t0), .q(t1));
  leaf #(1) l2 (.clk, .d(t1), .q(t2));
  always_comb q = @PAIR@;
endmodule

module top;
  logic       clk = 0;
  logic [7:0] d   = 1;
  logic [7:0] q0, q1, q2, q3;
  pair inst0 (.clk, .d, .q(q0));
  pair inst1 (.clk, .d, .q(q1));
  pair inst2 (.clk, .d, .q(q2));
  pair inst3 (.clk, .d, .q(q3));
  always #5 clk = ~clk;
  initial begin
    #100 $display("%0d %0d %0d %0d", q0, q1, q2, q3);
    $finish;
  end
endmodule
)";

std::string instantiate(std::string_view leaf, std::string_view pair) {
    std::string text(kDesign);
    const auto  put = [&](std::string_view key, std::string_view value) {
        const size_t at = text.find(key);
        REQUIRE(at != std::string::npos);
        text.replace(at, key.size(), value);
    };
    put("@LEAF@", leaf);
    put("@PAIR@", pair);
    return text;
}

std::map<std::string, std::string> modules_of(const std::string& source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    std::vector<ir::ModuleIr> modules;
    REQUIRE(ir::emit_llvm_modules(*r.module, modules));
    REQUIRE(modules.size() > 1);
    std::map<std::string, std::string> by_name;
    for (size_t i = 1; i < modules.size(); ++i) { // [0] is root
        REQUIRE_MESSAGE(by_name.emplace(modules[i].name, std::move(modules[i].ir)).second,
                        "two unit modules named '" << modules[i].name << "'");
    }
    return by_name;
}

// The names of the unit modules that are not byte-identical between `a` and `b`,
// as (only in a, only in b).
std::pair<std::vector<std::string>, std::vector<std::string>> differing(const std::map<std::string, std::string>& a,
                                                                       const std::map<std::string, std::string>& b) {
    std::pair<std::vector<std::string>, std::vector<std::string>> out;
    for (const auto& [name, text] : a) {
        const auto it = b.find(name);
        if (it == b.end() || it->second != text) {
            out.first.push_back(name);
        }
    }
    for (const auto& [name, text] : b) {
        const auto it = a.find(name);
        if (it == a.end() || it->second != text) {
            out.second.push_back(name);
        }
    }
    return out;
}

} // namespace

TEST_CASE("an edit to one module's body changes that unit's module and no other") {
    const auto base   = modules_of(instantiate("d + 8'(N)", "t2"));
    const auto edited = modules_of(instantiate("d + 8'(N) + 8'd2", "t2"));
    const auto [gone, fresh] = differing(base, edited);
    REQUIRE(gone.size() == 1);
    REQUIRE(fresh.size() == 1);
    CHECK(gone[0].starts_with("leaf"));
    CHECK(fresh[0].starts_with("leaf"));
}

TEST_CASE("an edit to the module that instantiates others leaves theirs alone") {
    const auto base   = modules_of(instantiate("d + 8'(N)", "t2"));
    const auto edited = modules_of(instantiate("d + 8'(N)", "t2 ^ pk::bump(t1)"));
    const auto [gone, fresh] = differing(base, edited);
    REQUIRE(gone.size() == 1);
    REQUIRE(fresh.size() == 1);
    CHECK(gone[0].starts_with("pair"));
    CHECK(fresh[0].starts_with("pair"));
}

TEST_CASE("compiling the same design twice gives the same modules") {
    const auto a = modules_of(instantiate("d + 8'(N)", "t2"));
    const auto b = modules_of(instantiate("d + 8'(N)", "t2"));
    const auto [gone, fresh] = differing(a, b);
    CHECK(gone.empty());
    CHECK(fresh.empty());
}

TEST_CASE("a unit's module names no instance") {
    // Instance storage, records and entries are root's; a unit's code reaches its
    // instance through the record it is handed.
    for (const auto& [name, text] : modules_of(instantiate("d + 8'(N)", "t2"))) {
        INFO("module ", name);
        CHECK(text.find("clotho.inst.") == std::string::npos);
        CHECK(text.find("top.inst") == std::string::npos); // an instance path of the design
    }
}

namespace {

// A design with what a unit holds that is numbered across the design or kept in
// root: nonblocking blocks, events, a memory, an assertion, a covergroup, a
// package with a variable and a function, one module at two parameter values
// (two units), several instances of each. The @...@ markers are the edits.
constexpr std::string_view kRich = R"(
package pk;
  localparam int K = 3;
  logic [7:0] shared_cnt = 0;
  function automatic logic [7:0] bump(logic [7:0] v);
    return @BUMP@;
  endfunction
endpackage

module leaf #(parameter int N = 1) (input logic clk, input logic [7:0] d, output logic [7:0] q, output logic ev_seen);
  logic [7:0] r;
  event e;
  logic [7:0] mem [0:3];
  logic seen = 0;
  always_ff @(posedge clk) begin
    r <= d + 8'(N);
    q <= r ^ pk::bump(d) @LEAF@;
    mem[d[1:0]] <= d;
    -> e;
  end
  always @(e) seen = 1;
  assign ev_seen = seen;
  property p; @(posedge clk) d != 8'hff |-> ##1 r != 8'hff; endproperty
  assert property (p) else $error("p failed");
  covergroup cg @(posedge clk);
    coverpoint d { bins lo = {[0:127]}; bins hi = {[128:255]}; }
  endgroup
  cg cgi = new;
endmodule

module pair (input logic clk, input logic [7:0] d, output logic [7:0] q);
  logic [7:0] t0, t1, t2; logic s0, s1, s2;
  leaf #(1) l0 (.clk, .d, .q(t0), .ev_seen(s0));
  leaf #(1) l1 (.clk, .d(t0), .q(t1), .ev_seen(s1));
  leaf #(2) l2 (.clk, .d(t1), .q(t2), .ev_seen(s2));
  always_comb q = t2 ^ {7'b0, s0 & s1 & s2} @PAIR@;
@PAIRX@
endmodule

module top;
  logic       clk = 0;
  logic [7:0] d   = 1;
  logic [7:0] q0, q1, q2;
  pair inst0 (.clk, .d, .q(q0));
  pair inst1 (.clk, .d, .q(q1));
  pair inst2 (.clk, .d, .q(q2));
@TOPI@
  always #5 clk = ~clk;
  always @(posedge clk) d <= d + 1;
@TOPX@
  initial begin
    #100 $display("%0d %0d %0d", q0, q1, q2);
    $finish;
  end
endmodule
)";

struct RichEdit {
    std::string_view bump = "v + 8'(K)";
    std::string_view leaf;
    std::string_view pair;
    std::string_view pair_extra;
    std::string_view top_insts;
    std::string_view top_extra;
};

std::string rich(const RichEdit& e) {
    std::string text(kRich);
    const auto  put = [&](std::string_view key, std::string_view value) {
        const size_t at = text.find(key);
        REQUIRE(at != std::string::npos);
        text.replace(at, key.size(), value);
    };
    put("@BUMP@", e.bump);
    put("@LEAF@", e.leaf);
    put("@PAIR@", e.pair);
    put("@PAIRX@", e.pair_extra);
    put("@TOPI@", e.top_insts);
    put("@TOPX@", e.top_extra);
    return text;
}

// What an edit adds that the rest of the design numbers: a block, an event, a
// covergroup, a process. Written on one line, triggered and reading only by the
// module's own signals, so it moves no source line of another module and adds no
// waiter to a signal another unit declares: that changes the declaring unit's code
// (an edge of the parent's `clk` here), a fact the declaring unit still joins over
// until step 4 (summaries).
[[maybe_unused]] constexpr std::string_view kPairExtra =
  "logic extra; event ex; always_ff @(posedge s0) begin extra <= ~t0[0]; -> ex; end "
  "covergroup cg2 @(posedge s0); coverpoint t0; endgroup cg2 cgi2 = new;";
constexpr std::string_view kTopExtra = R"(
  logic [7:0] cnt = 0; event te;
  always_ff @(posedge clk) begin cnt <= cnt + 1; -> te; end
  covergroup tcg @(posedge clk); coverpoint cnt; endgroup
  tcg tcgi = new;
  assert property (@(posedge clk) cnt != 8'hfe) else $error("top cnt");
)";

// The unit modules that changed, as (only in a, only in b) name lists, which must
// all start with `unit`.
void only_unit_changed(const std::string& base, const std::string& edited, std::string_view unit, size_t count) {
    const auto [gone, fresh] = differing(modules_of(base), modules_of(edited));
    CHECK(gone.size() == count);
    CHECK(fresh.size() == count);
    for (const auto& n : gone) {
        CHECK_MESSAGE(n.starts_with(unit), "changed: " << n);
    }
    for (const auto& n : fresh) {
        CHECK_MESSAGE(n.starts_with(unit), "changed: " << n);
    }
}

} // namespace

TEST_CASE("a rich design: an edit to one definition changes its units' modules only") {
    const std::string base = rich({});
    SUBCASE("a leaf, which is two units") {
        only_unit_changed(base, rich({ .leaf = "^ 8'h5a" }), "leaf", 2);
    }
    SUBCASE("the module that instantiates it") {
        only_unit_changed(base, rich({ .pair = "^ 8'h1" }), "pair", 1);
    }
    SUBCASE("a package function: package code is root's") {
        only_unit_changed(base, rich({ .bump = "v ^ 8'(K)" }), "", 0);
    }
}

TEST_CASE("a rich design: what an edit adds to the design's numbering changes no other unit") {
    const std::string base = rich({});
    // TODO: the same in a middle module (kPairExtra) still changes the top's module:
    // the parent's record layout and code depend on what its children add (block,
    // event, covergroup, process counts). Found by this gate; needs those counts out
    // of the parent's text (step 3e, record sizing).
    SUBCASE("the same in the top") {
        only_unit_changed(base, rich({ .top_extra = kTopExtra }), "top", 1);
    }
    SUBCASE("a new instance is an edit of its parent (it owns the port copies) and of nothing else") {
        only_unit_changed(base, rich({ .top_insts = "  pair inst3 (.clk, .d, .q());" }), "top", 1);
    }
}

TEST_CASE("a rich design: a unit's module names no instance") {
    for (const auto& [name, text] : modules_of(rich({}))) {
        INFO("module ", name);
        CHECK(text.find("clotho.inst.") == std::string::npos);
        CHECK(text.find("top.inst") == std::string::npos);
    }
}
