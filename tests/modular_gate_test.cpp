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
  pair p0 (.clk, .d, .q(q0));
  pair p1 (.clk, .d, .q(q1));
  pair p2 (.clk, .d, .q(q2));
  pair p3 (.clk, .d, .q(q3));
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

TEST_CASE("compiling the same design twice gives the same modules") {
    const auto a = modules_of(instantiate("d + 8'(N)", "t2"));
    const auto b = modules_of(instantiate("d + 8'(N)", "t2"));
    const auto [gone, fresh] = differing(a, b);
    CHECK(gone.empty());
    CHECK(fresh.empty());
}
