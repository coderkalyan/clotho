#include <doctest/doctest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "Lower.h"
#include "Twin.h"

// Twin mode (Lower.h, docs/VERIFICATION.md): what it hands the check, what it
// skips, and that a failing check fails the lowering that triggered it.

using namespace clotho;

namespace {

// Installs `check` for the scope and puts the suite's back after.
struct TwinScope {
    explicit TwinScope(TwinCheck check) { set_twin_check(std::move(check)); }
    ~TwinScope() {
#ifdef CLOTHO_EXTRA_ASSERTS
        set_twin_check(test::suite_twin_check);
#else
        set_twin_check({});
#endif
    }
};

uint64_t skipped(const std::string& why) {
    const auto& all = twin_stats().skipped;
    const auto  it  = all.find(why);
    return it == all.end() ? 0 : it->second;
}

} // namespace

TEST_CASE("twin mode: each top of a design is instantiated twice under one wrapper") {
    // Oracle: LRM 23.3.1 (a top-level module is instantiated once) and the construction
    // twin mode documents: each top becomes two instances of one module.
    std::vector<std::string> names;
    size_t                   top_count = 0;
    const TwinScope          scope([&](const LowerResult& twin) {
        top_count = twin.module->tops().size();
        for (const ir::InstanceScope& child : twin.module->tops().front().children) {
            names.push_back(child.def_name);
        }
        return TwinVerdict::Checked;
    });
    const uint64_t           before = twin_stats().twinned;

    const LowerResult r = lower_text(R"(
module a; logic x; initial x = 1; endmodule
module b; logic y; initial y = 0; endmodule
)");
    REQUIRE(r.ok);
    CHECK(r.module->tops().size() == 2); // the design itself is not twinned
    CHECK(top_count == 1);
    CHECK(names == std::vector<std::string>{ "a", "a", "b", "b" });
    CHECK(twin_stats().twinned == before + 1);
}

TEST_CASE("twin mode: a top with ports is instantiated with them open") {
    int             calls = 0;
    const TwinScope scope([&](const LowerResult&) {
        ++calls;
        return TwinVerdict::Checked;
    });
    const LowerResult r = lower_text(R"(
module t (input logic clk, input logic [3:0] d, output logic [3:0] q, inout wire w);
  always @(posedge clk) q <= d;
endmodule
)");
    REQUIRE(r.ok);
    CHECK(calls == 1);
}

TEST_CASE("twin mode: a check that finds a mismatch fails the lowering that compiled the design") {
    const TwinScope scope([](const LowerResult&) -> TwinVerdict { throw std::runtime_error("shared code differs"); });
    CHECK_THROWS_WITH_AS(lower_text("module t; endmodule"), "shared code differs", std::runtime_error);
}

TEST_CASE("twin mode: the twin lowering is not itself twinned, and a design that fails is not twinned") {
    int             calls = 0;
    const TwinScope scope([&](const LowerResult&) {
        ++calls;
        return TwinVerdict::Checked;
    });
    CHECK(lower_text("module t; endmodule").ok);
    CHECK(calls == 1);
    CHECK_FALSE(lower_text("module t; this is not systemverilog").ok);
    CHECK(calls == 1);
}

TEST_CASE("twin mode: a top that holds a class and a DPI export is lowered twice like any other") {
    int             calls = 0;
    const TwinScope scope([&](const LowerResult& twin) {
        ++calls;
        CHECK(twin.module->tops().size() == 1);
        return TwinVerdict::Checked;
    });
    const LowerResult r = lower_text(R"(
module t;
  class c; int v; endclass
  c o;
  export "DPI-C" function f;
  function int f(); return 1; endfunction
  initial begin o = new; o.v = f(); $display("%0d", o.v); end
endmodule
)");
    REQUIRE(r.ok);
    CHECK(calls == 1);
}

TEST_CASE("twin mode: a design with no top-level module has nothing to instantiate and is counted") {
    int             calls = 0;
    const TwinScope scope([&](const LowerResult&) {
        ++calls;
        return TwinVerdict::Checked;
    });
    const std::string why    = "no top-level module";
    const uint64_t    before = skipped(why);
    REQUIRE(lower_text("package p; int x; endpackage").ok);
    CHECK(calls == 0);
    CHECK(skipped(why) == before + 1);
}

TEST_CASE("twin mode: a hierarchical name that reaches a top by its module name still resolves") {
    int             calls = 0;
    const TwinScope scope([&](const LowerResult&) {
        ++calls;
        return TwinVerdict::Checked;
    });
    REQUIRE(lower_text(R"(
module t;
  logic x;
  initial begin x = 1; $display("%0d", t.x); end
endmodule
)").ok);
    CHECK(calls == 1);
}
