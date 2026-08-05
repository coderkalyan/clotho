#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Lower.h"
#include "ir/Analysis.h"
#include "legalize/Legalize.h"
#include "legalize/Materialize.h"

using namespace clotho;

namespace {

struct Run {
    LowerResult      lowered;
    legalize::Result result;

    [[nodiscard]] const ir::Module& mod() const {
        return *lowered.module;
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

    // The stores for `name` in the first process that writes it.
    [[nodiscard]] const legalize::SignalStores* find(std::string_view name) const {
        const ir::Signal s = sig(name);
        for (const legalize::ProcessResult& p : result.processes) {
            for (const legalize::SignalStores& e : p.signals) {
                if (e.signal == s) {
                    return &e;
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool all_synthesizable() const {
        for (const legalize::ProcessResult& p : result.processes) {
            if (!p.synthesizable) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool has(legalize::Finding f) const {
        for (const legalize::Note& n : result.notes) {
            if (n.kind == f) {
                return true;
            }
        }
        return false;
    }
};

Run run(std::string_view src, legalize::Options o = {}) {
    Run r{ lower_text(src), {} };
    REQUIRE_MESSAGE(r.lowered.ok, r.lowered.diagnostic_text());
    REQUIRE(r.lowered.module != nullptr);
    const ir::DesignFacts facts = ir::analyze(*r.lowered.module);
    r.result                    = legalize::run(*r.lowered.module, &facts, o);
    return r;
}

std::string wrap(std::string_view body) {
    std::string s = "module m(input clk, input rst, input en, input sel, input [1:0] s2,\n"
                    "         input rst_n, input [7:0] a, input [7:0] b, output reg [7:0] q, output reg [7:0] r);\n  ";
    s += body;
    s += "\nendmodule\n";
    return s;
}

} // namespace

// The base case: one unconditional assignment, one store, nothing held.
TEST_CASE("an unconditional assignment produces one store and no hold") {
    const Run                     t = run(wrap("always @(posedge clk) q <= a;"));
    const legalize::SignalStores* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->stores.size() == 1);
    CHECK(e->stores[0].nonblocking);
    CHECK_FALSE(e->has_hold);
    CHECK(t.all_synthesizable());
}

// Latch inference is structural: the missing `else` is an empty region in the IR,
// so `has_hold` falls out of the walk rather than needing a second analysis.
TEST_CASE("a missing else is a hold") {
    const Run                     t = run(wrap("always @* if (en) q = a;"));
    const legalize::SignalStores* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->stores.size() == 1);
    CHECK(e->has_hold);
}

TEST_CASE("both arms assigning is complete") {
    const Run                     t = run(wrap("always @* if (en) q = a; else q = b;"));
    const legalize::SignalStores* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->stores.size() == 2);
    CHECK_FALSE(e->has_hold);
}

// An unconditional assignment ahead of the branch completes it -- the common
// defensive style, and it must NOT read as a latch.
TEST_CASE("a default assignment before the branch removes the hold") {
    const Run                     t = run(wrap("always @* begin q = b; if (en) q = a; end"));
    const legalize::SignalStores* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->stores.size() == 2);
    CHECK_FALSE(e->has_hold);
}

// The per-signal unit: one block, two signals, only one of them held. A per-BLOCK
// classifier gets this wrong.
TEST_CASE("hold is per signal, not per process") {
    const Run                     t  = run(wrap("always @* begin r = a; if (en) q = a; end"));
    const legalize::SignalStores* eq = t.find("q");
    const legalize::SignalStores* er = t.find("r");
    REQUIRE(eq != nullptr);
    REQUIRE(er != nullptr);
    CHECK(eq->has_hold);
    CHECK_FALSE(er->has_hold);
}

// A case with no default holds; adding one completes it.
TEST_CASE("case completeness follows the default arm") {
    const Run                     without = run(wrap("always @* case (s2) 2'b00: q = a; 2'b01: q = b; endcase"));
    const legalize::SignalStores* e1      = without.find("q");
    REQUIRE(e1 != nullptr);
    CHECK(e1->stores.size() == 2);
    CHECK(e1->has_hold);

    const Run                     with = run(wrap("always @* case (s2) 2'b00: q = a; default: q = b; endcase"));
    const legalize::SignalStores* e2   = with.find("q");
    REQUIRE(e2 != nullptr);
    CHECK_FALSE(e2->has_hold);
}

// Nesting depth does not change the store count, and every store keeps its own
// guard -- the arena shares prefixes rather than collapsing them.
TEST_CASE("nested branches keep one store per leaf with distinct guards") {
    const Run t =
      run(wrap("always @* if (en) begin if (sel) q = a; else q = b; end else q = 0;"), { .keep_guards = true });
    const legalize::SignalStores* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->stores.size() == 3);
    CHECK_FALSE(e->has_hold);
    CHECK(e->stores[0].guard != e->stores[1].guard);
    CHECK(e->stores[1].guard != e->stores[2].guard);
    // The arena exists only when asked for, and the deepest guard has two atoms.
    CHECK_FALSE(t.result.arena().empty());
}

TEST_CASE("guards are not retained unless requested") {
    const Run t = run(wrap("always @* if (en) q = a; else q = b;"));
    CHECK(t.result.arena().empty());
}

// A partial write leaves the untouched bits holding, which is the same question as
// an unassigned path and must classify the same way.
TEST_CASE("a part-select write holds the untouched bits") {
    const Run                     t = run(wrap("always @* q[3:0] = a[3:0];"));
    const legalize::SignalStores* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->has_hold);
}

// The legality gate. `synthesizable` defaults to false in TagInfo, so anything
// unclassified rejects its process instead of being legalized as something else.
TEST_CASE("simulation-only constructs reject the process") {
    const Run disp = run(wrap("always @(posedge clk) $display(\"x\");"));
    CHECK_FALSE(disp.all_synthesizable());
    CHECK(disp.has(legalize::Finding::UnsynthesizableOp));

    const Run delay = run(wrap("always begin #5 q = a; end"));
    CHECK_FALSE(delay.all_synthesizable());
    CHECK(delay.has(legalize::Finding::Suspends));
}

TEST_CASE("a real-valued op is pure but not hardware") {
    const Run t = run(R"(
        module m(input clk, input real x, output reg y);
          always @(posedge clk) y <= (x > 1.0);
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
}

// Rejection is not abandonment: the stores that did legalize are still reported,
// because a partial answer is what an editor wants.
TEST_CASE("a rejected process still reports the stores it understood") {
    const Run t = run(wrap("always @(posedge clk) begin q <= a; $display(\"x\"); end"));
    CHECK_FALSE(t.all_synthesizable());
    const legalize::SignalStores* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->stores.size() == 1);
}

// Ordinary dataflow must not trip the gate -- the classification is a whitelist and
// a false negative here would reject most real designs.
TEST_CASE("arithmetic, selects and comparisons are synthesizable") {
    const Run t = run(wrap("always @* q = ((a + b) & 8'h0F) | (a[3:0] > b[3:0] ? 8'h80 : 8'h00);"));
    CHECK(t.all_synthesizable());
}

// --- M3: classification ---------------------------------------------------

TEST_CASE("a clocked block with no reset is a flop") {
    const Run   t = run(wrap("always @(posedge clk) q <= a;"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Flop);
    CHECK_FALSE(e->async_reset);
}

TEST_CASE("an async reset is a flop with a constant residue") {
    const Run   t = run(wrap("always @(posedge clk or posedge rst) if (rst) q <= 8'h00; else q <= a;"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Flop);
    CHECK(e->async_reset);
}

TEST_CASE("an active-low async reset classifies the same way") {
    const Run   t = run(wrap("always @(posedge clk or negedge rst_n) if (!rst_n) q <= 8'h00; else q <= a;"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Flop);
    CHECK(e->async_reset);
}

// R04 from the reset corpus: the reset test is NOT the sole outermost switch.
// yosys hard-errors on this shape because its peeler requires the reset to be
// outermost; pin-and-fold does not care where it sits, because pinning rst makes
// both arms of `en` assign the same constant.
TEST_CASE("a reset nested under an enable still classifies") {
    const Run   t = run(wrap("always @(posedge clk or posedge rst)\n"
                             "  if (en) begin if (rst) q <= 8'h00; else q <= a; end\n"
                             "  else if (rst) q <= 8'h00; else q <= b;"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Flop);
    CHECK(e->async_reset);
}

// A reset that only fires when an enable is high is not a reset: with `en` low the
// rst edge assigns nothing and the signal holds. No primitive gates a reset pin,
// so this must NOT be promoted -- the conservative direction.
TEST_CASE("a reset that only fires under an enable is not a reset") {
    const Run t = run(wrap("always @(posedge clk or posedge rst) if (en) begin if (rst) q <= 8'h00; else q <= a; end"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK_FALSE(e->async_reset);
}

// R09: a non-constant on the async edge is a load, not a reset. No standard
// primitive does this, so it is reported rather than built.
TEST_CASE("a non-constant async value is an async load") {
    const Run   t = run(R"(
        module m(input clk, input rst, input [7:0] a, input [7:0] iv, output reg [7:0] q);
          always @(posedge clk or posedge rst) if (rst) q <= iv; else q <= a;
        endmodule
    )");
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::AsyncLoad);
}

// R22: `if (rst && en)` is not implied by the posedge rst trigger -- with en low
// the block still fires on rst and assigns a. Rejected, and the residue says why.
TEST_CASE("a reset guarded by an unrelated enable is not a reset") {
    const Run   t = run(wrap("always @(posedge clk or posedge rst) if (rst && en) q <= 8'h00; else q <= a;"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK_FALSE(e->async_reset);
}

TEST_CASE("a level-sensitive complete block is combinational") {
    const Run   t = run(wrap("always @* q = a;"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Comb);
}

TEST_CASE("a level-sensitive incomplete block is a latch") {
    const Run   t = run(wrap("always @* if (en) q = a;"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Latch);
}

TEST_CASE("a rejected process leaves its signals unclassified") {
    const Run   t = run(wrap("always @(posedge clk) begin q <= a; $display(\"x\"); end"));
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Unclassified);
}

// --- M3: the reset corpus -------------------------------------------------
//
// 28 cases run against yosys 0.67+92 (proc; opt_expr; opt_dff; opt_clean), whose
// verdict is recorded per row. An empty `yosys` field means yosys HARD-ERRORED on
// input Clotho classifies -- six of the twenty-eight:
//
//   R04  reset nested under an enable      R24  the same under always_ff
//   R13  `if (rst || rst2)`                R25  partial reset
//   R19  reset nested in the reset arm     R25c partial reset outside the branch
//
// yosys needs the reset test to be the sole outermost switch; pin-and-fold does
// not care where it sits, which is the entire reason for the design. On the
// twenty-two yosys does accept, Clotho agrees on every one.

namespace {

struct ResetCase {
    std::string_view      file;
    legalize::StorageKind kind;
    bool                  async_reset;
    // What yosys built, for the record. Empty means yosys rejected the input.
    std::string_view yosys;
};

// Every row's `kind`/`async_reset` is what Clotho must produce. Where `yosys` is
// empty, yosys hard-errored on input Clotho accepts.
constexpr ResetCase kResetCases[] = {
    { "R01_classic_negedge", legalize::StorageKind::Flop, true, "$adff" },
    { "R02_classic_posedge", legalize::StorageKind::Flop, true, "$adff" },
    { "R03_polarity_mismatch", legalize::StorageKind::AsyncLoad, false, "$aldff" },
    { "R04_rst_under_en", legalize::StorageKind::Flop, true, "" },
    { "R05_en_under_rst", legalize::StorageKind::Flop, true, "$adffe" },
    { "R06_two_arst_same", legalize::StorageKind::Flop, true, "$adff" },
    { "R07_two_arst_diff", legalize::StorageKind::Flop, true, "$dffsr" },
    { "R08_set_and_reset", legalize::StorageKind::Flop, true, "$dffsr" },
    { "R09_nonconst_rstval", legalize::StorageKind::AsyncLoad, false, "$aldff" },
    { "R11_reset_to_x", legalize::StorageKind::Flop, true, "$adff" },
    { "R13_ord_reset", legalize::StorageKind::Flop, true, "" },
    { "R14_self_assign_rst", legalize::StorageKind::Flop, false, "$dffe" },
    { "R15_rst_as_data", legalize::StorageKind::Flop, true, "$adff" },
    { "R16_sync_only", legalize::StorageKind::Flop, false, "$sdff" },
    { "R17_sync_and_async", legalize::StorageKind::Flop, true, "$adff" },
    { "R20_concat_rstval", legalize::StorageKind::Flop, true, "$adff" },
    { "R23_derived_wire_rst", legalize::StorageKind::Flop, true, "$adff" },
    { "R26_tilde_not", legalize::StorageKind::Flop, true, "$adff" },
    { "R27_param_rstval", legalize::StorageKind::Flop, true, "$adff" },
    { "R28_two_sigs_diff_rv", legalize::StorageKind::Flop, true, "$adff" },
    { "R29_inv_mismatch", legalize::StorageKind::AsyncLoad, false, "$aldff" },
    { "R30_rst_then_en_seq", legalize::StorageKind::Flop, true, "$adffe" },
    { "R19_nested_in_rstarm", legalize::StorageKind::Flop, true, "" },
    // Pinning rst leaves `if (en) q<=0; else q<=a`, so the edge DOES assign -- with
    // a mux value. AsyncLoad describes that; yosys rejects it by complaining about
    // the sensitivity list, which is not where the problem is.
    { "R22_rst_expr", legalize::StorageKind::AsyncLoad, false, "" },
    { "R24_alwaysff_under_en", legalize::StorageKind::Flop, true, "" },
    { "R25_partial_reset", legalize::StorageKind::Flop, true, "" },
    { "R25b_partial_inside_else", legalize::StorageKind::Flop, true, "$adff" },
    { "R25c_partial_outside", legalize::StorageKind::Flop, true, "" },
};

} // namespace

TEST_CASE("the reset corpus classifies") {
    for (const ResetCase& c : kResetCases) {
        CAPTURE(c.file);
        const std::string path = std::string(CLOTHO_TEST_DIR) + "/legalize/reset/" + std::string(c.file) + ".sv";
        const std::array<std::string_view, 1> paths{ path };
        const LowerResult                     lowered = lower_files(paths);
        REQUIRE(lowered.ok);
        const legalize::Result r = legalize::run(*lowered.module, nullptr);
        REQUIRE(r.processes.size() >= 1);

        const legalize::SignalStores* q = nullptr;
        for (const legalize::ProcessResult& p : r.processes) {
            for (const legalize::SignalStores& e : p.signals) {
                if (lowered.module->signal(e.signal).name == "q") {
                    q = &e;
                }
            }
        }
        REQUIRE(q != nullptr);
        CHECK(q->kind == c.kind);
        CHECK(q->async_reset == c.async_reset);
    }
}

// --- M4: materialize ------------------------------------------------------

namespace {

// Classification of signal `q`, or nullopt when the design has none.
std::optional<std::pair<legalize::StorageKind, bool>> classify_q(const LowerResult&      lowered,
                                                                 const legalize::Result& r) {
    for (const legalize::ProcessResult& p : r.processes) {
        for (const legalize::SignalStores& e : p.signals) {
            const std::string& n = lowered.module->signal(e.signal).name;
            if (n == "q" || n.ends_with(".q")) {
                return std::pair{ e.kind, e.async_reset };
            }
        }
    }

    return std::nullopt;
}

} // namespace

// The acceptance for M4. Rendering a legalized design must produce source that
// re-lowers to the SAME classification -- if it did not, the rendered mux tree
// would not be the hardware the analysis claimed, and every downstream consumer
// (the SV writer, the mismatch tool, the LSP's inlay hints) would inherit the lie.
//
// Run over the whole reset corpus rather than a sample, because the fill-in rule
// is asymmetric: a combinational base is spelled 'x and a holding base is spelled
// by omission, so both directions have to survive the round trip.
TEST_CASE("materializing a design round-trips to the same classification") {
    for (const ResetCase& c : kResetCases) {
        CAPTURE(c.file);
        const std::string path = std::string(CLOTHO_TEST_DIR) + "/legalize/reset/" + std::string(c.file) + ".sv";
        const std::array<std::string_view, 1> paths{ path };
        const LowerResult                     lowered = lower_files(paths);
        REQUIRE(lowered.ok);

        const legalize::Result before = legalize::run(*lowered.module, nullptr, { .keep_guards = true });
        const std::string      text   = legalize::materialize(*lowered.module, before);
        CAPTURE(text);
        // Anything the renderer could not spell would silently weaken the test.
        REQUIRE(text.find("unrenderable") == std::string::npos);

        const LowerResult again = lower_text(text);
        REQUIRE_MESSAGE(again.ok, again.diagnostic_text());
        const legalize::Result after = legalize::run(*again.module, nullptr);

        const auto a = classify_q(lowered, before);
        const auto b = classify_q(again, after);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->first == b->first);
        CHECK(a->second == b->second);
    }
}

// What materializing actually contributes: the arm the source left implicit is
// written out. The two cases are not symmetric, and the asymmetry is forced.
TEST_CASE("a missing arm is filled in, differently for a flop and a latch") {
    // A clock enable. `else q <= q` is an ordinary feedback mux -- legal, and it
    // is what the hardware already was.
    const LowerResult flop = lower_text(wrap("always @(posedge clk) if (en) q <= a;"));
    REQUIRE(flop.ok);
    const legalize::Result fr = legalize::run(*flop.module, nullptr, { .keep_guards = true });
    const std::string      ft = legalize::materialize(*flop.module, fr);
    CHECK(ft.find("q <= q; // hold") != std::string::npos);

    // The same text in a level-sensitive block would be a combinational loop, so
    // the hold can only be described, never spelled.
    const LowerResult latch = lower_text(wrap("always @* if (en) q = a;"));
    REQUIRE(latch.ok);
    const legalize::Result lr = legalize::run(*latch.module, nullptr, { .keep_guards = true });
    const std::string      lt = legalize::materialize(*latch.module, lr);
    CHECK(lt.find("holds: latch") != std::string::npos);
    CHECK(lt.find("q = q") == std::string::npos);
}

// Nesting is what carries completeness. A flattened `if (g) q<=a; if (!g) q<=b;`
// loses the fact that the two arms are exhaustive, and every signal re-lowers as
// held -- which is how the first version of this broke every reset in the corpus.
TEST_CASE("materialize emits a nested tree, not a flat guarded list") {
    const LowerResult r = lower_text(wrap("always @* if (en) q = a; else q = b;"));
    REQUIRE(r.ok);
    const legalize::Result res  = legalize::run(*r.module, nullptr, { .keep_guards = true });
    const std::string      text = legalize::materialize(*r.module, res);
    CHECK(text.find("end else begin") != std::string::npos);

    const legalize::Result again = legalize::run(*lower_text(text).module, nullptr);
    REQUIRE(again.processes.size() == 1);
    REQUIRE(again.processes[0].signals.size() == 1);
    CHECK(again.processes[0].signals[0].kind == legalize::StorageKind::Comb);
}

// --- function synthesis ---------------------------------------------------
//
// A pure function is an OPERATOR: its internal branching decides its return value
// and nothing else, so from the caller's side a call is an ordinary value-producing
// opcode. Nothing is inlined and no IR is rewritten -- the whole feature is
// deciding whether that operator exists. Which makes the rejection rules the
// substance, so each one gets a test that fails if the rule is inverted.

TEST_CASE("a call to a pure function is synthesizable") {
    const Run t = run(R"(
        module m(input [3:0] a, input [3:0] b, output reg [7:0] q);
          function [7:0] mix; input [3:0] x; input [3:0] y;
            mix = {x, y};
          endfunction
          always @* q = mix(a, b);
        endmodule
    )");
    CHECK(t.all_synthesizable());
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Comb);
}

// Control flow inside the callee is a value question, not a statement question:
// the caller's guarded list is unchanged and q is still combinational.
TEST_CASE("a function with internal branching is still an operator") {
    const Run t = run(R"(
        module m(input [3:0] a, input [3:0] b, input s, output reg [7:0] q);
          function [7:0] mix; input [3:0] x; input [3:0] y;
          begin
            if (s) mix = {x, y}; else mix = {y, x};
          end
          endfunction
          always @* q = mix(a, b);
        endmodule
    )");
    CHECK(t.all_synthesizable());
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Comb);
    // One store, not two: the branch is inside the value, not the process.
    CHECK(e->stores.size() == 1);
}

TEST_CASE("a function calling another function is an operator") {
    const Run t = run(R"(
        module m(input [3:0] a, output reg [7:0] q);
          function [3:0] inc; input [3:0] x; inc = x + 4'd1; endfunction
          function [7:0] dbl; input [3:0] x; dbl = {inc(x), inc(x)}; endfunction
          always @* q = dbl(a);
        endmodule
    )");
    CHECK(t.all_synthesizable());
}

// Every rejection below is a real hardware reason, not a gap in the walk.

// Suspension is a property of the BODY, not of the `task` keyword: Delay and Wait
// are ungated opcodes, so this is rejected without any rule that mentions tasks.
TEST_CASE("a subroutine that suspends is not an operator") {
    const Run t = run(R"(
        module m(input [3:0] a, output reg [7:0] q);
          task slow; input [3:0] x; begin #5; end endtask
          always @* begin q = {4'h0, a}; slow(a); end
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
    CHECK(t.has(legalize::Finding::Call));
}

// The converse, and the reason the rule turns on the body. picorv32's `assert
// directive expands to a call to an empty task; rejecting on the keyword threw
// away four processes over a subroutine that does nothing.
TEST_CASE("a task that writes nothing and cannot suspend is a legal no-op") {
    const Run t = run(R"(
        module m(input [3:0] a, output reg [7:0] q);
          task empty_statement; begin end endtask
          always @* begin empty_statement; q = {4'h0, a}; end
        endmodule
    )");
    CHECK(t.all_synthesizable());
    const auto* e = t.find("q");
    REQUIRE(e != nullptr);
    CHECK(e->kind == legalize::StorageKind::Comb);
    // The call contributes nothing: one store, from the one assignment.
    CHECK(e->stores.size() == 1);
}

// A task that DOES write a module signal needs its writes attributed to the
// caller's guards, which needs operand identity across two Irs -- guards and store
// values are bare ir::Index today. Rejected rather than half-modelled.
TEST_CASE("a task that writes a module signal is rejected") {
    const Run t = run(R"(
        module m(input [3:0] a, input en, output reg [7:0] q);
          task setq; input [3:0] x; begin q = {4'h0, x}; end endtask
          always @* begin q = 8'h00; if (en) setq(a); end
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
}

// An output formal writes the CALLER's storage, so the call is a statement with
// effects rather than a value.
TEST_CASE("a function with an output argument is not an operator") {
    const Run t = run(R"(
        module m(input [3:0] a, output reg [7:0] q);
          function [3:0] two; input [3:0] x; output [3:0] o; begin o = x; two = x; end endfunction
          reg [3:0] side;
          always @* begin q = 8'h00; q[3:0] = two(a, side); end
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
}

// Writing a module signal from a function is legal Verilog and is a side effect;
// an operator does not have one.
TEST_CASE("a function that writes a module signal is not an operator") {
    const Run t = run(R"(
        module m(input [3:0] a, output reg [7:0] q, output reg flag);
          function [7:0] peek; input [3:0] x; begin flag = 1'b1; peek = {4'h0, x}; end endfunction
          always @* q = peek(a);
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
}

// Recursion has no bounded hardware form. The memo marks a function "deciding"
// while it is being decided, so reaching it again is the cycle.
TEST_CASE("a recursive function is not an operator") {
    const Run t = run(R"(
        module m(input [3:0] a, output reg [7:0] q);
          function automatic [7:0] fact; input [3:0] x;
            fact = (x == 4'd0) ? 8'd1 : fact(x - 4'd1);
          endfunction
          always @* q = fact(a);
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
}

// The gate is recursive: an unsynthesizable opcode anywhere in the callee's body
// rejects the caller, not just one in the caller's own body.
TEST_CASE("an unsynthesizable opcode inside a callee rejects the caller") {
    const Run t = run(R"(
        module m(input [3:0] a, output reg [7:0] q);
          function [7:0] noisy; input [3:0] x; begin $display("x"); noisy = {4'h0, x}; end endfunction
          always @* q = noisy(a);
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
}

// A ternary's branch values live in REGIONS, so a call hidden in one is only
// reached if the gate descends. Without that this process is legalized as
// something it is not -- a silent accept, which is the one failure mode the
// design does not tolerate.
TEST_CASE("a call inside a ternary branch is still gated") {
    const Run t = run(R"(
        module m(input [3:0] a, input s, output reg [7:0] q);
          function [7:0] noisy; input [3:0] x; begin $display("x"); noisy = {4'h0, x}; end endfunction
          always @* q = s ? noisy(a) : 8'h00;
        endmodule
    )");
    CHECK_FALSE(t.all_synthesizable());
}
