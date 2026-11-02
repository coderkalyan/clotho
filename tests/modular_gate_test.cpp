// The step 3 gate of docs/design/modular-compilation.md: a unit's LLVM module is a
// function of the unit, so compiling a design twice with one module's body edited
// changes that unit's module and root's, and no other module.

#include <doctest/doctest.h>

#include <map>
#include <sstream>
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
  logic ok = 0;
  initial begin
    @(posedge clk);
    expect (@(posedge clk) d != 8'hff ##1 r != 8'hff) ok = 1; else ok = 0;
  end
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
constexpr std::string_view kPairExtra =
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
    SUBCASE("the same in the top") {
        only_unit_changed(base, rich({ .top_extra = kTopExtra }), "top", 1);
    }
    // The top writes the ports of the module in the middle and services their wake
    // buckets: what it finds there is the port's, not a place in the module's record,
    // which what the module gains elsewhere moves.
    SUBCASE("the same in a middle module") {
        only_unit_changed(base, rich({ .pair_extra = kPairExtra }), "pair", 1);
    }
    SUBCASE("an assertion in a middle module") {
        only_unit_changed(base, rich({ .pair_extra = "assert property (@(posedge s0) t0 != 8'hfe) else $error(\"t0\");" }),
                          "pair", 1);
    }
    SUBCASE("a process in a middle module") {
        only_unit_changed(base, rich({ .pair_extra = "logic [7:0] pc = 0; always #7 pc = pc + 1;" }), "pair", 1);
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
        CHECK(text.find("clotho.expect") == std::string::npos); // a monitor's number is the design's
    }
}

namespace {

// A module whose parent writes its signals every way a unit's code can reach
// another unit's: a blocking write that wakes its waiters, a nonblocking one
// (block shadow, a partial select), a memory element, an event, and everything
// dumped. The child already holds every kind of thing an edit adds, so an edit adds
// to a design that has them (the first of a kind changes every unit's code).
constexpr std::string_view kBound = R"(
module child (input logic clk, input logic [7:0] d);
@CPRE@
  logic [7:0] v = 0;
  logic [7:0] mem [0:3];
  logic [7:0] w = 0;
  event ev;
  int seen = 0;
  always @(v) seen = seen + 1;
  always @(posedge clk) seen = seen + 2;
  always @(ev) seen = seen + 3;
  always @(w[0]) seen = seen + 4;
  logic base_r; event base_e;
  always_ff @(posedge clk) begin base_r <= ~d[0]; -> base_e; end
  covergroup base_cg @(posedge clk); coverpoint d; endgroup
  base_cg base_cgi = new;
  assert property (@(posedge clk) d != 8'hfd) else $error("base");
@CPOST@
endmodule

module top;
  logic       clk = 0;
  logic [7:0] d   = 1;
  child c0 (.clk, .d);
  child c1 (.clk, .d);
  always #5 clk = ~clk;
  always @(posedge clk) begin
    c0.v = d;
    c1.v <= d;
    c0.mem[d[1:0]] <= d;
    c1.w[0] <= d[0];
    -> c0.ev;
    d <= d + 1;
  end
  initial begin
    $dumpvars(0, top);
    #100 $display("%0d", c0.seen);
    $finish;
  end
endmodule
)";

std::string bound(std::string_view pre, std::string_view post) {
    std::string text(kBound);
    const auto  put = [&](std::string_view key, std::string_view value) {
        const size_t at = text.find(key);
        REQUIRE(at != std::string::npos);
        text.replace(at, key.size(), value);
    };
    put("@CPRE@", pre);
    put("@CPOST@", post);
    return text;
}

} // namespace

TEST_CASE("a module that writes another unit's signals depends on the signals and not on the unit's body") {
    const std::string base = bound("", "");
    // The top is the only other unit: both children (the two differ in how the top
    // uses them) change, and the top does not.
    SUBCASE("a block, an event, a covergroup and an assertion after the signals") {
        only_unit_changed(base,
                          bound("",
                                "logic extra; event ex; always_ff @(posedge clk) begin extra <= ~d[0]; -> ex; end "
                                "covergroup cg2 @(posedge clk); coverpoint d; endgroup cg2 cgi2 = new; "
                                "assert property (@(posedge clk) d != 8'hfe) else $error(\"d\");"),
                          "child", 2);
    }
    SUBCASE("the same before them") {
        only_unit_changed(base,
                          bound("logic pre; logic [7:0] pre8 = 0; event pex; "
                                "always_ff @(posedge clk) begin pre <= ~d[0]; pre8 <= pre8 + 1; -> pex; end",
                                ""),
                          "child", 2);
    }
    SUBCASE("a memory that is written nonblocking, before them") {
        only_unit_changed(base, bound("logic [7:0] pm [0:1]; always_ff @(posedge clk) pm[d[0]] <= d;", ""), "child", 2);
    }
    SUBCASE("signals before them") {
        only_unit_changed(base, bound("logic [7:0] ps0 = 0, ps1 = 0;", ""), "child", 2);
    }
}

TEST_CASE("a module that writes another unit's signals names no instance") {
    for (const auto& [name, text] : modules_of(bound("", ""))) {
        INFO("module ", name);
        CHECK(text.find("clotho.inst.") == std::string::npos);
        CHECK(text.find("top.c") == std::string::npos); // an instance path of the design
    }
}

namespace {

// A module with every kind of thing root has to set up for an instance: a
// declaration initializer, processes that start at time zero and ones that wait, a
// static and an event subscription, an always_comb, a final block and an assertion.
// `@N@` instances of it under one parent, so a design of 2 and a design of 7 differ
// in nothing but how many times the module is instantiated.
constexpr std::string_view kTiles = R"(
module tile (input logic clk, input logic [7:0] d, output logic [7:0] q);
  logic [7:0] r = 8'd3;
  logic [7:0] v = 0;
  event ev;
  int seen = 0;
  always @(v) seen = seen + 1;
  always @(posedge clk) seen = seen + 2;
  always @(ev) seen = seen + 3;
  always_ff @(posedge clk) begin r <= r + d; -> ev; end
  always_comb q = r ^ d;
  assert property (@(posedge clk) d != 8'hfd) else $error("tile");
  final $display("%0d", seen);
endmodule

module top;
  logic       clk = 0;
  logic [7:0] d   = 1;
  for (genvar i = 0; i < @N@; i++) begin : g
    logic [7:0] q;
    tile t (.clk, .d, .q);
  end
  always #5 clk = ~clk;
  initial begin
    $dumpvars(0, top);
    #50 $finish;
  end
endmodule
)";

std::string tiles(unsigned n) {
    std::string text(kTiles);
    const size_t at = text.find("@N@");
    REQUIRE(at != std::string::npos);
    text.replace(at, 3, std::to_string(n));
    return text;
}

// The instructions of each function of root's module, by name.
std::map<std::string, size_t> root_code(const std::string& source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);
    std::vector<ir::ModuleIr> modules;
    REQUIRE(ir::emit_llvm_modules(*r.module, modules));
    REQUIRE(!modules.empty());
    std::map<std::string, size_t> counts;
    std::istringstream            in(modules.front().ir); // [0] is root
    std::string                   line;
    std::string                   current;
    while (std::getline(in, line)) {
        if (line.starts_with("define ")) {
            const size_t at  = line.find('@');
            const size_t end = line.find('(', at);
            current          = line.substr(at + 1, end - at - 1);
            if (current.starts_with('"')) {
                current = current.substr(1, current.size() - 2);
            }
            counts[current] = 0;
        } else if (line.starts_with("}")) {
            current.clear();
        } else if (!current.empty() && line.starts_with("  ") && !line.starts_with("  ;")) {
            ++counts[current];
        }
    }
    return counts;
}

} // namespace

TEST_CASE("root's code does not grow with the number of instances") {
    // Link glue is data walked by loops: the records are initializers, and what main
    // would have stored for each instance is not code.
    const auto few  = root_code(tiles(2));
    const auto many = root_code(tiles(7));
    for (const std::string name : { "clotho_main", "sim_drain", "sim_finals", "clotho.seed", "clotho.run_once",
                                    "clotho.vcd.register" }) {
        INFO("function ", name);
        REQUIRE(few.contains(name));
        REQUIRE(many.contains(name));
        // A constant of the design (a ring's capacity, a table's length) can fold an
        // instruction or two differently; five more instances add hundreds.
        const auto a = static_cast<int64_t>(few.at(name));
        const auto b = static_cast<int64_t>(many.at(name));
        CHECK(std::abs(a - b) <= 2);
    }
}
