#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "Lower.h"
#include "ir/Eval.h"
#include "ir/Module.h"

using namespace clotho;

namespace {

struct Design {
    LowerResult r;

    [[nodiscard]] const ir::Module& mod() const {
        return *r.module;
    }

    [[nodiscard]] ir::Signal sig(std::string_view name) const {
        const auto signals = mod().signals();
        for (size_t i = 0; i < signals.size(); ++i) {
            const std::string& n = signals[i].name;
            if (n == name || (n.size() > name.size() && n.ends_with(name) && n[n.size() - name.size() - 1] == '.')) {
                return static_cast<ir::Signal>(i);
            }
        }
        FAIL("no signal named ", name);
        return ir::Signal{};
    }

    // The process that has edge triggers, or process 0 when none does. Designs
    // below have exactly one interesting process.
    [[nodiscard]] const ir::Process& proc() const {
        for (const ir::Process& p : mod().processes()) {
            if (!p.wait_events.empty()) {
                return p;
            }
        }
        return mod().processes().front();
    }
};

// Every `cond` predicate in a body, outermost first, depth-first. Index 0 is the
// outermost `if`, which is the one the reset rule looks at.
void collect_preds(const ir::Ir& ir, std::span<const ir::Index> body, std::vector<ir::Index>& out) {
    for (const ir::Index i : body) {
        if (ir.tag(i) != ir::Inst::Tag::Cond) {
            continue;
        }
        out.push_back(ir.payload(i).op_extra.op);
        const auto regs = ir.extra_data<ir::Regions>(ir.payload(i).op_extra.extra);
        collect_preds(ir, ir.region(regs.then_), out);
        collect_preds(ir, ir.region(regs.else_), out);
    }
}

Design lower(std::string_view src) {
    Design d{ lower_text(src) };
    REQUIRE_MESSAGE(d.r.ok, d.r.diagnostic_text());
    REQUIRE(d.r.module != nullptr);
    return d;
}

// Truth of the Nth `cond` predicate with `name` pinned to `bits`.
ir::Tri pinned_truth(const Design& d, std::string_view name, uint64_t bits, size_t nth = 0) {
    const ir::Ir&          ir = d.proc().ir;
    std::vector<ir::Index> preds;
    collect_preds(ir, ir.root(), preds);
    REQUIRE(preds.size() > nth);

    return ir::truth_with_pin(d.mod(), ir, preds[nth], d.sig(name), bits);
}

constexpr std::string_view kHdr = "module m(input clk, input rst, input rst_n, input a, input b, input en,\n"
                                  "         input [7:0] dat, output reg [7:0] q);\n";

Design body(std::string_view stmt) {
    std::string s(kHdr);
    s += "  ";
    s += stmt;
    s += "\nendmodule\n";
    return lower(s);
}

} // namespace

// The classic shapes, both polarities. Polarity is not a special case: it is which
// value the edge pins, and the guard folds accordingly (§4A.1).
TEST_CASE("active-high reset folds to true when pinned high") {
    const Design d = body("always @(posedge clk or posedge rst) if (rst) q<=0; else q<=dat;");
    CHECK(pinned_truth(d, "rst", 1) == ir::Tri::True);
    CHECK(pinned_truth(d, "rst", 0) == ir::Tri::False);
}

TEST_CASE("active-low reset folds through the inversion") {
    const Design d = body("always @(posedge clk or negedge rst_n) if (!rst_n) q<=0; else q<=dat;");
    // negedge pins the asserted state to 0, and `!rst_n` then reduces to 1.
    CHECK(pinned_truth(d, "rst_n", 0) == ir::Tri::True);
    CHECK(pinned_truth(d, "rst_n", 1) == ir::Tri::False);
}

// R03. The typo case: `negedge rst_n` with `if (rst_n)`. Pinning the asserted
// state (0) makes the guard FALSE, so the else arm is what runs under the reset
// edge -- which is why this classifies as an async load rather than a reset, with
// no polarity-mismatch rule anywhere.
TEST_CASE("polarity mismatch folds false, not true") {
    const Design d = body("always @(posedge clk or negedge rst_n) if (rst_n) q<=0; else q<=dat;");
    CHECK(pinned_truth(d, "rst_n", 0) == ir::Tri::False);
}

// R13. Bit-level absorption is the whole reason EvalValue carries a mask: `b` is
// unknown, but a known 1 on `a` settles the OR.
TEST_CASE("or-ed reset folds from either side alone") {
    const Design d = body("always @(posedge clk or posedge a or posedge b) if (a|b) q<=0; else q<=dat;");
    CHECK(pinned_truth(d, "a", 1) == ir::Tri::True);
    CHECK(pinned_truth(d, "b", 1) == ir::Tri::True);
    CHECK(pinned_truth(d, "a", 0) == ir::Tri::Unknown); // b still unknown
}

// R22. The rejection case, and the point is that it folds to Unknown rather than
// to either answer: with `rst` high the guard is `en`, which the evaluator cannot
// settle. That residue is what the diagnostic reports.
TEST_CASE("reset qualified by an enable stays unknown") {
    const Design d = body("always @(posedge clk or posedge rst) if (rst && en) q<=0; else q<=dat;");
    CHECK(pinned_truth(d, "rst", 1) == ir::Tri::Unknown);
    // But a definitely-false `&&` operand still settles it.
    CHECK(pinned_truth(d, "rst", 0) == ir::Tri::False);
}

TEST_CASE("nesting does not affect the fold") {
    const Design d = body("always @(posedge clk or posedge rst)\n"
                          "    if (en) begin if (rst) q<=0; else q<=dat; end else if (rst) q<=0;");
    CHECK(pinned_truth(d, "rst", 1, 0) == ir::Tri::Unknown); // outer: `en`
    CHECK(pinned_truth(d, "rst", 1, 1) == ir::Tri::True);    // inner: `rst`
    CHECK(pinned_truth(d, "rst", 1, 2) == ir::Tri::True);    // else-side `rst`
}

// Pinning nothing must still fold constants -- that is what makes the same
// evaluator usable for case coverage and for S2's folding.
TEST_CASE("constants fold with nothing pinned") {
    const Design           d  = body("always @(posedge clk) if (1'b1) q<=0; else q<=dat;");
    const ir::Ir&          ir = d.proc().ir;
    std::vector<ir::Index> preds;
    collect_preds(ir, ir.root(), preds);
    REQUIRE_FALSE(preds.empty());
    CHECK(ir::truth_with_pin(d.mod(), ir, preds[0], static_cast<ir::Signal>(~0u), 0) == ir::Tri::True);
}

// An unhandled shape must degrade to Unknown, never to a wrong answer. A function
// call is the standing example and the documented ceiling: no inlining.
TEST_CASE("a value the evaluator cannot see is unknown, not wrong") {
    const Design d = lower(R"(
        module m(input clk, input rst, input [7:0] dat, output reg [7:0] q);
          function automatic bit f(input bit x); return x; endfunction
          always @(posedge clk or posedge rst) if (f(rst)) q<=0; else q<=dat;
        endmodule
    )");
    CHECK(pinned_truth(d, "rst", 1) == ir::Tri::Unknown);
    CHECK(pinned_truth(d, "rst", 0) == ir::Tri::Unknown);
}

// `rst || rst2` short-circuits into a select_lazy whose arms live in REGIONS. With
// rst2 pinned the taken arm computes `rst | 1` and the other is the literal 1, so
// the result is 1 without knowing rst. The bor/lor absorption above is not enough:
// it never runs, because the operands are behind the lazy select. R13 in the reset
// corpus is exactly this shape and cannot be classified without it.
TEST_CASE("a lazy select folds when both arms agree") {
    const Design d = lower(R"(
        module m(input clk, input rst, input rst2, input [7:0] a, output reg [7:0] q);
          always @(posedge clk or posedge rst or posedge rst2)
            if (rst || rst2) q <= 8'h00; else q <= a;
        endmodule
    )");
    // Either reset pinned proves the guard, though neither appears alone in the
    // arm that computes it.
    CHECK(pinned_truth(d, "rst2", 1) == ir::Tri::True);
    CHECK(pinned_truth(d, "rst", 1) == ir::Tri::True);
}
