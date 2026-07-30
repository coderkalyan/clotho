#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"

using namespace clotho;

namespace {

struct Checked {
    LowerResult r;

    [[nodiscard]] const Diagnostic* find(std::string_view code) const {
        for (const Diagnostic& d : r.diagnostics) {
            if (d.code == code) {
                return &d;
            }
        }

        return nullptr;
    }

    [[nodiscard]] bool has(std::string_view code) const {
        return find(code) != nullptr;
    }
};

// Wrap a case statement in a combinational block. `always_comb` so the design is
// realistic; the pragma checks care only about the case itself.
Checked check(std::string_view case_stmt, std::string_view sel = "logic [1:0] s;") {
    std::string src = "module m;\n  ";
    src += sel;
    src += "\n  logic [3:0] y;\n  always_comb begin\n    y = 0;\n    ";
    src += case_stmt;
    src += "\n  end\nendmodule\n";

    Checked c{ lower_text(src) };
    REQUIRE_MESSAGE(c.r.ok, c.r.diagnostic_text());

    return c;
}

constexpr std::string_view kFull     = "full-case-not-full";
constexpr std::string_view kParallel = "parallel-case-overlap";

} // namespace

// The finding these pragmas exist to produce. `full_case` asserts every selector
// value matches an item; here two of four do not, so synthesis is free to treat
// them as don't-care while simulation holds y's previous value. Cummings: the
// directives "are always most dangerous when they work", because the divergence
// scales with the optimisation the pragma unlocked.
TEST_CASE("full_case on a case that is not full is reported") {
    const Checked c = check("(* full_case *) case (s) 2'b00: y=1; 2'b01: y=2; endcase");

    const Diagnostic* d = c.find(kFull);
    REQUIRE(d != nullptr);
    // A legal, simulatable design: this is a mismatch risk, not an error, and the
    // severity policies of design 3.4 are what will promote it for synthesis.
    CHECK(d->severity == Severity::Warning);
    CHECK(d->origin == DiagnosticOrigin::Analysis);
    CHECK(d->span.valid());
    // The count is the actionable part -- "2 of 4" tells you the scale.
    CHECK(d->message.find("2 of 4") != std::string::npos);
}

// The claim being true must be silent, or the check is noise. All four values of
// a 2-bit selector are named.
TEST_CASE("full_case on a genuinely full case is silent") {
    const Checked c = check("(* full_case *) case (s) 2'b00: y=1; 2'b01: y=2; 2'b10: y=3; 2'b11: y=4; endcase");
    CHECK_FALSE(c.has(kFull));
}

// A default arm makes the case full by construction, so the pragma is redundant
// rather than untrue. Reporting redundancy here would fire on the common defensive
// coding style and train users to ignore the check.
TEST_CASE("full_case alongside a default is not reported") {
    const Checked c = check("(* full_case *) case (s) 2'b00: y=1; default: y=9; endcase");
    CHECK_FALSE(c.has(kFull));
}

// Without the pragma there is no claim to contradict. An incomplete case is
// ordinary latch-or-hold behaviour and slang already warns about the missing
// default; this check must not double up on it.
TEST_CASE("an incomplete case with no pragma is not reported") {
    const Checked c = check("case (s) 2'b00: y=1; 2'b01: y=2; endcase");
    CHECK_FALSE(c.has(kFull));
}

// LRM 5.12 gives a valueless attribute the value 1, so `= 0` is how a claim is
// withdrawn and must not be reported against.
TEST_CASE("full_case = 0 withdraws the claim") {
    const Checked c = check("(* full_case = 0 *) case (s) 2'b00: y=1; endcase");
    CHECK_FALSE(c.has(kFull));
}

// Clotho's 2-state fullness rule, which is NOT the simulation one: only 2-state
// values count toward covering the selector, and an item's x and z bits count
// for nothing. So under plain
// `case` an all-x label covers nothing at all -- which
// is exactly why "assign x in the default to be safe" plus full_case is a trap.
TEST_CASE("a plain-case x label covers nothing under the 2-state rule") {
    const Checked c = check("(* full_case *) case (s) 2'bxx: y=1; endcase");

    const Diagnostic* d = c.find(kFull);
    REQUIRE(d != nullptr);
    CHECK(d->message.find("4 of 4") != std::string::npos);
}

// casez, by contrast, DOES make z/? bits don't-care, so wildcard labels really do
// cover the space and the pragma is honest.
TEST_CASE("casez wildcards count toward fullness") {
    const Checked c = check("(* full_case *) casez (s) 2'b?0: y=1; 2'b?1: y=2; endcase");
    CHECK_FALSE(c.has(kFull));
}

// parallel_case asserts the items are mutually exclusive, so synthesis may drop
// the priority order. Two casez labels that share a value break that: `4'b??01`
// and `4'b01??` both match 4'b0101.
TEST_CASE("parallel_case on overlapping items is reported") {
    const Checked c =
      check("(* parallel_case *) casez (s) 4'b??01: y=1; 4'b01??: y=2; default: y=9; endcase", "logic [3:0] s;");

    const Diagnostic* d = c.find(kParallel);
    REQUIRE(d != nullptr);
    CHECK(d->severity == Severity::Warning);
    CHECK(d->origin == DiagnosticOrigin::Analysis);
    // Exactly one value (4'b0101) is in both.
    CHECK(d->message.find("1 selector value") != std::string::npos);
}

TEST_CASE("parallel_case on disjoint items is silent") {
    const Checked c =
      check("(* parallel_case *) casez (s) 4'b00??: y=1; 4'b11??: y=2; default: y=9; endcase", "logic [3:0] s;");
    CHECK_FALSE(c.has(kParallel));
}

// The two checks are independent and both run from one expansion of the value
// space. 4'b0??1 covers {0001,0011,0101,0111} and 4'b01?? covers
// {0100,0101,0110,0111}: six distinct values, so ten are uncovered, and two are
// covered twice.
TEST_CASE("both pragmas are checked independently on one case") {
    const Checked c =
      check("(* full_case *) (* parallel_case *) casez (s) 4'b0??1: y=1; 4'b01??: y=2; endcase", "logic [3:0] s;");

    const Diagnostic* full = c.find(kFull);
    const Diagnostic* par  = c.find(kParallel);
    REQUIRE(full != nullptr);
    REQUIRE(par != nullptr);
    CHECK(full->message.find("10 of 16") != std::string::npos);
    CHECK(par->message.find("2 selector value") != std::string::npos);
}

// Silence on anything unprovable is the property that makes a finding worth
// acting on. A non-constant label cannot be expanded, so the whole case is left
// alone rather than half-analysed.
TEST_CASE("a non-constant case label makes the check say nothing") {
    const Checked c = check("(* full_case *) case (s) p: y=1; endcase", "logic [1:0] s; logic [1:0] p;");
    CHECK_FALSE(c.has(kFull));
}

// `case ... inside` matches by set membership and ranges, a different coverage
// computation; not attempted rather than approximated.
TEST_CASE("case inside is not analysed") {
    const Checked c = check("(* full_case *) case (s) inside 2'b00: y=1; endcase");
    CHECK_FALSE(c.has(kFull));
}

// A finding must not change whether the design runs: these pragmas have no
// simulation semantics at all, which is the whole reason they cause mismatch.
TEST_CASE("a pragma finding leaves the design simulatable") {
    const Checked c = check("(* full_case *) case (s) 2'b00: y=1; endcase");

    CHECK(c.r.ok);
    CHECK(c.r.module != nullptr);
    CHECK(c.has(kFull));
    // Warnings only: nothing here reaches Error, so a check run exits clean.
    CHECK_FALSE(has_error(c.r.diagnostics));
}
