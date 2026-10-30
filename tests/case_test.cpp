#include <doctest/doctest.h>

#include <cstdio>
#include <format>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module's initial blocks and return everything it wrote to stdout.
// The case statements under test sit in an `initial` block and report the chosen
// branch with $display, so the captured text is the observable behavior.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);

    return out;
}

} // namespace

// IEEE 1800-2023 12.5 Example 1: literal x and z are DISTINCT case items, and a
// normal `case` matches them exactly (case-equality).
TEST_CASE("normal case matches literal x and z exactly (LRM 12.5 Ex.1)") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'b0z;
                case (sel)
                    2'b00: $display("zero");
                    2'b0x: $display("isx");
                    2'b0z: $display("isz");
                    default: $display("other");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "isz\n");
}

// 12.5.1 Example 1: casez treats z / ? as don't-care; the MSB selects the match.
TEST_CASE("casez treats ? as a wildcard (LRM 12.5.1 Ex.1)") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] ir;
            initial begin
                ir = 8'b1010_0000;
                casez (ir)
                    8'b1???????: $display("instr1");
                    8'b01??????: $display("instr2");
                    default:     $display("none");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "instr1\n");
}

// 12.5.1: casex treats x AND z as don't-care; the xx low bits are ignored.
TEST_CASE("casex treats x/z label bits as don't-care (LRM 12.5.1 Ex.2)") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] r;
            initial begin
                r = 8'b0110_0110;
                casex (r)
                    8'b011001xx: $display("stat1");
                    8'b1100xx00: $display("stat2");
                    default:     $display("none");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "stat1\n");
}

TEST_CASE("default fires only when nothing matched") {
    constexpr std::string_view hit = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd2;
                case (s)
                    2'd0: $display("a");
                    2'd1: $display("b");
                    default: $display("dflt");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(hit) == "dflt\n");
}

TEST_CASE("no match and no default executes nothing") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd3;
                case (s)
                    2'd0: $display("a");
                    2'd1: $display("b");
                endcase
                $display("after");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "after\n");
}

TEST_CASE("first matching item wins, search stops") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd1;
                case (s)
                    2'd1: $display("first");
                    2'd0: $display("zero");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "first\n");
}

// -- Priority-order case dedup (LRM 12.5 first-match) --

// A later constant label duplicating an earlier one is dead: the first item's
// body is the only one that can run for that value.
TEST_CASE("duplicate constant label: first body wins, duplicate is dead") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd1;
                case (s)
                    2'd1: $display("first");
                    2'd1: $display("dup");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "first\n");
}

// The duplicate collapses across differing widths once resized to the common
// compare width (8'd5 === 4'd5), so the wider later label is still dead.
TEST_CASE("duplicate constant label across widths") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] s;
            initial begin
                s = 8'd5;
                case (s)
                    4'd5: $display("first");
                    8'd5: $display("dup");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "first\n");
}

// Deduping a label out of a group must not remove a still-live sibling label.
TEST_CASE("dedup within a group keeps live sibling") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] s;
            initial begin
                s = 3'd4;
                case (s)
                    3'd1:       $display("one");
                    3'd1, 3'd4: $display("group");
                    default:    $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "group\n");
}

// Non-constant labels are not deduped (only constants are, for now), so a
// side-effecting label between two identical `k` labels must still run and the
// later `k` re-evaluate: k starts 1 (no match at s=2), bump() raises k to 2 and
// matches. Guards that constant-only dedup never disturbs non-constant labels.
TEST_CASE("non-constant labels keep evaluation and side effects (not deduped)") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s, k;
            function automatic logic [3:0] bump;
                k = k + 4'd1;
                return k;
            endfunction
            initial begin
                k = 4'd1;
                s = 4'd2;
                case (s)
                    k:      $display("k1");
                    bump(): $display("bumped");
                    k:      $display("k2");
                    default:$display("def");
                endcase
            end
        endmodule
    )";
    // k starts 1 (no match), bump() -> k=2 matches s=2.
    CHECK(run_capture(src) == "bumped\n");
}

// A duplicate call label must keep its second evaluation (side effect), so the
// duplicate is never dropped. Each call increments a counter; the body that
// runs reports the counter, proving both calls executed.
TEST_CASE("duplicate call label keeps its side effect") {
    constexpr std::string_view src = R"(
        module m;
            integer n = 0;
            logic [3:0] s;
            function automatic logic [3:0] tick;
                n = n + 1;
                return 4'd7;
            endfunction
            initial begin
                s = 4'd3;
                case (s)
                    tick(): $display("a");
                    tick(): $display("b");
                    default: $display("n=%0d", n);
                endcase
            end
        endmodule
    )";
    // s=3 matches neither tick()==7; both calls run -> n=2 at default.
    CHECK(run_capture(src) == "n=2\n");
}

// casez: two identical wildcard labels -- the later is dead (identical label
// yields an identical match predicate).
TEST_CASE("duplicate casez wildcard label is dead") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b1010;
                casez (s)
                    4'b1?1?: $display("first");
                    4'b1?1?: $display("dup");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "first\n");
}

// unique case with duplicate labels: the whole-statement case_dedup pass is
// skipped for unique/unique0, so both items' predicates survive for the
// separate overlap-check sum; dispatch itself still runs through a switch
// (deduped locally, first occurrence wins), unaffected by the overlap check.
TEST_CASE("unique case with duplicate label still executes first match") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd2;
                unique case (s)
                    2'd2: $display("first");
                    2'd2: $display("dup");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "first\n");
}

// -- Switch-lowered constant case: packed-key routing --
// These run through the multiway switch path (all labels clean constants). The
// key is {may^must, must}: a clean selector keys on its value; any x/z bit sets
// the high half so the key lands outside every case value and hits default.

TEST_CASE("switch case: clean selector hits its arm") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'd10;
                case (s)
                    4'd8:  $display("eight");
                    4'd10: $display("ten");
                    4'd12: $display("twelve");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ten\n");
}

TEST_CASE("switch case: clean unlisted value hits default") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'd7;
                case (s)
                    4'd8:  $display("eight");
                    4'd10: $display("ten");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "def\n");
}

// A selector with an x bit cannot === any clean label; the packed key routes it
// to default. Without the may^must field it would alias a clean label.
TEST_CASE("switch case: x-bit selector routes to default") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b101x;
                case (s)
                    4'd8:  $display("eight");
                    4'd10: $display("ten");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "def\n");
}

TEST_CASE("switch case: z-bit selector routes to default") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b101z;
                case (s)
                    4'd8:  $display("eight");
                    4'd10: $display("ten");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "def\n");
}

// Grouped keys share one arm; matching the second key still runs that body.
TEST_CASE("switch case: grouped key second value hits shared arm") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'd3;
                case (s)
                    4'd1, 4'd3, 4'd5: $display("odd");
                    4'd2, 4'd4:       $display("even");
                    default:          $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "odd\n");
}

// -- One-hot reverse-case `case (1'b1)` over base[i] selects (ctz/clz + jump) --
// Cross-checked against iverilog.

TEST_CASE("one-hot ascending: lowest set bit wins") {
    // bits 1 and 3 set; LSB-first source order -> bit 1 (arm a1) wins.
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b1010;
                case (1'b1)
                    s[0]: $display("a0");
                    s[1]: $display("a1");
                    s[2]: $display("a2");
                    s[3]: $display("a3");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a1\n");
}

TEST_CASE("one-hot descending: highest set bit wins") {
    // bits 1 and 3 set; MSB-first source order -> bit 3 (arm m3) wins.
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b1010;
                case (1'b1)
                    s[3]: $display("m3");
                    s[2]: $display("m2");
                    s[1]: $display("m1");
                    s[0]: $display("m0");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "m3\n");
}

TEST_CASE("one-hot: x/z bit is not a definite 1 (skipped)") {
    // bit 0 = x (no match), bit 2 = 1 -> arm a2.
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b010x;
                case (1'b1)
                    s[0]: $display("a0");
                    s[1]: $display("a1");
                    s[2]: $display("a2");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a2\n");
}

TEST_CASE("one-hot: no definite-1 bit hits default") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'bxxxx;
                case (1'b1)
                    s[0]: $display("a0");
                    s[1]: $display("a1");
                    s[2]: $display("a2");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "def\n");
}

TEST_CASE("a vector condition with a definite 1 bit is definitely true") {
    // The three neighbouring truth-of-X tests all use 1-bit conditions, so none
    // pins the definite-1-dominates direction on a multi-bit vector. A reduction
    // written as "any bit unknown -> X" passes all of them, yet would classify
    // every partially-unknown vector condition as X-controlled.
    //
    // LRM 11.4.7 gives a true / false / "ambiguous truth value" trichotomy, and
    // 11.4.9 Table 11-17 (reduction OR) decides which bucket applies: row 1 is
    // all ones, so one definite-1 bit forces a definite 1. Hence 4'b010x is
    // unambiguously true despite the x, and only 4'b000x is ambiguous.
    //
    // Note the published rule disagrees: Sutherland 2005 and Standard Gotchas
    // 2006 6.10 both require that no bit be x/z for the condition to be true,
    // 6.10 using 4'b010z as its worked example. iverilog agrees with the
    // expectation below on every line.
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] v;
            initial begin
                v = 4'b010x;
                if (v) $display("t"); else $display("f");
                v = 4'b010z;
                if (v) $display("t"); else $display("f");
                v = 4'b000x;
                if (v) $display("t"); else $display("f");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "t\nt\nf\n");
}

TEST_CASE("one-hot with a gap in indices masks unselected bits") {
    // Only bits 0 and 3 are arms; bit 1 is set but has no arm -> must not
    // shadow. Here bit 3 set, bit 1 set -> arm for bit 3 (ascending: lowest
    // *selected* set bit is 3 since bit-0 arm's bit is 0).
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b1010;
                case (1'b1)
                    s[0]: $display("a0");
                    s[3]: $display("a3");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "a3\n");
}

// Non-monotonic arm order can't use ctz/clz (source-order priority differs from
// bit order under multiple set bits); it falls back to the chain and still
// yields the source-order first match.
TEST_CASE("one-hot non-monotonic order falls back to chain (source priority)") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                s = 4'b0011;
                case (1'b1)
                    s[2]: $display("n2");
                    s[0]: $display("n0");
                    s[1]: $display("n1");
                    default: $display("nd");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "n0\n");
}

namespace {

// JIT-run a module and return everything it wrote to stderr. Priority-case
// violation reports (LRM 12.5.3) go to stderr (a simulator diagnostic, not
// design output), so the captured text is the observable violation behavior.
std::string run_capture_stderr(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));

    ir::run_jit(*r.module);

    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);

    return out;
}

// Count non-overlapping occurrences of `needle` in `hay`.
size_t count_occ(std::string_view hay, std::string_view needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string_view::npos; p = hay.find(needle, p + needle.size())) {
        n++;
    }
    return n;
}

} // namespace

// LRM 12.5.3: a `priority case` with no default reached with no matching item
// issues a violation report. Execution is unchanged (no branch runs); the report
// is the only added behavior.
TEST_CASE("priority case: no match, no default reports a violation") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd3;
                priority case (sel)
                    2'd0: $display("zero");
                    2'd1: $display("one");
                endcase
            end
        endmodule
    )";
    const std::string          err = run_capture_stderr(src);
    CHECK(err.find("no condition is true") != std::string::npos);
    // Design output is unchanged: nothing matched, so no $display fired.
    CHECK(run_capture(src).empty());
}

// LRM 12.4.2 / 12.5.3 defer the violation report to the Observed region
// (LRM 4.4.2.4), not the Active region where the case executes. A clocked
// design exercises the drain-loop Observed flush (emit_active_run) rather than
// the time-0 initial-block path: two posedge instants each hit a no-match
// priority case, so each settled instant emits one report stamped with that
// instant's time (t=5, t=15), before the design finishes at t=25.
TEST_CASE("priority case: Observed-region report is stamped with the settled instant") {
    constexpr std::string_view src = R"(
        module m;
            logic       clk = 0;
            logic [1:0] sel = 2'b11;
            int         cnt = 0;
            always #5 clk = ~clk;
            always @(posedge clk) begin
                priority case (sel)
                    2'b00: ;
                    2'b01: ;
                endcase
                sel <= sel - 1;      // 11 (viol), 10 (viol), 01 (match)
                cnt <= cnt + 1;
                if (cnt == 2) $finish;
            end
        endmodule
    )";
    const std::string          err = run_capture_stderr(src);
    // One report per violating settle pass; pending reports are not flushed (docs/LRM_NOTES.md, note 18).
    CHECK(count_occ(err, "no condition is true") == 2);
    // Each report carries the time of its own instant, proving the deferral
    // fires at the settled instant (drain-loop Observed flush), not at t=0.
    CHECK(err.find("time 5 ") != std::string::npos);
    CHECK(err.find("time 15 ") != std::string::npos);
}

// A matching item suppresses the report (the priority assertion holds).
TEST_CASE("priority case: a matching item reports nothing") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd1;
                priority case (sel)
                    2'd0: $display("zero");
                    2'd1: $display("one");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("no condition is true") == std::string::npos);
    CHECK(run_capture(src) == "one\n");
}

// A priority case WITH a default can never violate: the default always matches.
TEST_CASE("priority case: a default suppresses the violation") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd3;
                priority case (sel)
                    2'd0: $display("zero");
                    default: $display("def");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("no condition is true") == std::string::npos);
    CHECK(run_capture(src) == "def\n");
}

// The report fires once per dynamic execution that finds no match.
TEST_CASE("priority case: reports once per no-match execution") {
    constexpr std::string_view src = R"(
        module m;
            int i;
            logic [1:0] sel;
            initial begin
                for (i = 0; i < 3; i++) begin
                    sel = 2'd3;
                    priority case (sel)
                        2'd0: $display("zero");
                    endcase
                end
            end
        endmodule
    )";
    CHECK(count_occ(run_capture_stderr(src), "no condition is true") == 3);
}

// A wildcard `priority casez` keeps the if-else chain (no switch/one-hot path)
// and still reports when no item's pattern matches.
TEST_CASE("priority casez: no matching pattern reports a violation") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd3;
                priority casez (sel)
                    2'b0?: $display("lo");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("no condition is true") != std::string::npos);
}

// -- unique / unique0 (LRM 12.5.3) --

// unique reports a no-match like priority (default-less, no item matched).
TEST_CASE("unique case: no match, no default reports a no-match violation") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd3;
                unique case (sel) 2'd0: $display("z"); 2'd1: $display("o"); endcase
            end
        endmodule
    )";
    const std::string          err = run_capture_stderr(src);
    CHECK(err.find("no condition is true") != std::string::npos);
    CHECK(err.find("not unique") == std::string::npos);
}

// unique0 does NOT report a no-match (LRM 12.5.3: zero matches is legal).
TEST_CASE("unique0 case: no match is silent") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd3;
                unique0 case (sel) 2'd0: $display("z"); 2'd1: $display("o"); endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
}

// unique reports an overlap when >1 item matches (wildcard patterns), and still
// executes the first match.
TEST_CASE("unique casez: overlapping items report and run the first match") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'b11;
                unique casez (sel)
                    2'b1?: $display("hi");
                    2'b?1: $display("lo");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
    CHECK(run_capture(src) == "hi\n"); // first match wins despite the violation
}

// unique0 reports overlap too (only its no-match check differs from unique).
TEST_CASE("unique0 casez: overlapping items report a violation") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'b11;
                unique0 casez (sel) 2'b1?: $display("hi"); 2'b?1: $display("lo"); endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
}

// Distinct constant labels can never overlap: a unique case over them reports
// nothing and dispatches correctly (the constant switch fast path).
TEST_CASE("unique case: distinct constants never overlap") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd2;
                unique case (sel)
                    2'd0: $display("a");
                    2'd1: $display("b");
                    2'd2: $display("c");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
    CHECK(run_capture(src) == "c\n");
}

// A duplicate constant label IS an overlap: when the selector hits the shared
// value both items match. Dispatch runs through the deduped switch (first
// occurrence wins, "x"); the separately-computed overlap-predicate sum still
// sees both labels and reports the violation regardless.
TEST_CASE("unique case: duplicate constant label reports an overlap") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd1;
                unique case (sel) 2'd1: $display("x"); 2'd1: $display("y"); endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
    CHECK(run_capture(src) == "x\n"); // first match still runs
}

// A default suppresses the no-match report for unique (default always matches),
// and no overlap arises from distinct constants.
TEST_CASE("unique case: a default suppresses the no-match report") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd3;
                unique case (sel) 2'd0: $display("a"); default: $display("d"); endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
    CHECK(run_capture(src) == "d\n");
}

// unique over non-constant labels: overlap when two labels evaluate equal to the
// selector at runtime (no constant fast path applies).
TEST_CASE("unique case: non-constant labels overlap at runtime") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] sel, a, b;
            initial begin
                a = 4'd5; b = 4'd5; sel = 4'd5;
                unique case (sel) a: $display("a"); b: $display("b"); endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
    CHECK(run_capture(src) == "a\n");
}

// -- priority if (LRM 12.4.2) --
// Execution is ordered first-match (identical to a plain if); only the no-match
// report is added. unique/unique0 if overlap is still TODO.

// A default-less priority-if chain with no true condition reports a no-match.
TEST_CASE("priority if: no true condition, no else reports a violation") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] a;
            initial begin
                a = 3'd5;
                priority if (a==0) $display("z");
                else if (a==1) $display("o");
                else if (a==2) $display("t");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("no condition is true") != std::string::npos);
    CHECK(run_capture(src).empty()); // nothing matched, so no branch ran
}

// A matching condition suppresses the report and runs that arm (first match).
TEST_CASE("priority if: a matching condition reports nothing") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] a;
            initial begin
                a = 3'd1;
                priority if (a==0) $display("z");
                else if (a==1) $display("o");
                else if (a==2) $display("t");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
    CHECK(run_capture(src) == "o\n");
}

// An explicit final else can never no-match: no report even when no `if`
// condition is true.
TEST_CASE("priority if: a final else suppresses the report") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] a;
            initial begin
                a = 3'd5;
                priority if (a==0) $display("z");
                else if (a==1) $display("o");
                else $display("d");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
    CHECK(run_capture(src) == "d\n");
}

// A single-arm priority if (no else-if, no else) reports when its condition is
// false.
TEST_CASE("priority if: single arm, condition false, reports") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] a;
            initial begin
                a = 3'd3;
                priority if (a==7) $display("hit");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("no condition is true") != std::string::npos);
    CHECK(run_capture(src).empty());
}

// A plain (unqualified) if never reports, even with no match and no else.
TEST_CASE("plain if: no match, no else is silent") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] a;
            initial begin
                a = 3'd5;
                if (a==0) $display("z"); else if (a==1) $display("o");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
}

// -- unique / unique0 if (LRM 12.4.2) --
// Overlap detection is eager (all conditions evaluated); execution runs the
// first true arm.

// More than one condition true is a uniqueness violation; the first true arm
// still runs.
TEST_CASE("unique if: overlapping conditions report and run the first") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            initial begin
                a = 4'b1001;
                unique if (a[0]) $display("lo"); else if (a[3]) $display("hi");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
    CHECK(run_capture(src) == "lo\n"); // first true arm wins despite the violation
}

// Exactly one condition true: no violation.
TEST_CASE("unique if: exactly one true condition is silent") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            initial begin
                a = 4'b0001;
                unique if (a[0]) $display("lo"); else if (a[3]) $display("hi");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
    CHECK(run_capture(src) == "lo\n");
}

// unique reports a no-match (no condition true, no final else) like priority.
TEST_CASE("unique if: no true condition, no else reports a no-match") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            initial begin
                a = 4'b0100;
                unique if (a[0]) $display("lo"); else if (a[3]) $display("hi");
            end
        endmodule
    )";
    const std::string          err = run_capture_stderr(src);
    CHECK(err.find("no condition is true") != std::string::npos);
    CHECK(err.find("not unique") == std::string::npos);
}

// unique0 does NOT report a no-match (LRM 12.4.2: zero true is legal).
TEST_CASE("unique0 if: no true condition is silent") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            initial begin
                a = 4'b0100;
                unique0 if (a[0]) $display("lo"); else if (a[3]) $display("hi");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
}

// unique0 reports overlap (only its no-match check differs from unique).
TEST_CASE("unique0 if: overlapping conditions report") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            initial begin
                a = 4'b1001;
                unique0 if (a[0]) $display("lo"); else if (a[3]) $display("hi");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
}

// A final else suppresses the unique no-match report (it always matches); no
// overlap arises when at most one condition is true.
TEST_CASE("unique if: a final else suppresses the no-match report") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a;
            initial begin
                a = 4'b0100;
                unique if (a[0]) $display("lo"); else if (a[3]) $display("hi"); else $display("d");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
    CHECK(run_capture(src) == "d\n");
}

// -- case ... inside (LRM 12.5.4 / 11.4.13) --

// The LRM 12.5.4 worked example verbatim: a wildcard singular label (3'b0?0)
// and a [4:7] range share one item; every 3-bit value is covered.
TEST_CASE("case inside: LRM 12.5.4 worked example") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] status;
            initial begin
                for (int i = 0; i < 8; i++) begin
                    status = i[2:0];
                    priority case (status) inside
                        1, 3:          $display("task1");
                        3'b0?0, [4:7]: $display("task2");
                    endcase
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "task2\ntask1\ntask2\ntask1\ntask2\ntask2\ntask2\ntask2\n");
    CHECK(run_capture_stderr(src).empty());
}

// A singular label uses wildcard equality (11.4.13): the selector's own x/z
// bits are never wildcards, but a z/? in the label is (2'b1z matches 2'd3,
// not the selector's own x/z).
TEST_CASE("case inside: singular label wildcard-matches selector, not vice versa") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] sel;
            initial begin
                sel = 2'd3;
                case (sel) inside
                    2'b1z: $display("hit");
                    default: $display("miss");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "hit\n");
}

// A [lo:hi] range tests inclusive membership via relational (not wildcard)
// comparison, so a negative signed selector is handled correctly too.
TEST_CASE("case inside: signed range membership") {
    constexpr std::string_view src = R"(
        module m;
            int x;
            initial begin
                x = -3;
                case (x) inside
                    [-5:5]:  $display("in");
                    default: $display("out");
                endcase
                x = 7;
                case (x) inside
                    [-5:5]:  $display("in");
                    default: $display("out");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "in\nout\n");
}

// 11.4.13: only a label's own x/z is a wildcard, so an x/z selector never
// matches any singular or range member -- a default-less priority case
// reports no-match exactly on the LRM's own cited failing values.
TEST_CASE("case inside: an x/z selector reports no-match (LRM 12.5.4 note)") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] status;
            initial begin
                status = 3'b00x;
                priority case (status) inside
                    1, 3:          $display("task1");
                    3'b0?0, [4:7]: $display("task2");
                endcase
                status = 3'bxxx;
                priority case (status) inside
                    1, 3:          $display("task1");
                    3'b0?0, [4:7]: $display("task2");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src).empty());
    CHECK(count_occ(run_capture_stderr(src), "no condition is true") == 2);
}

// unique case inside: two items overlapping on the same value is a reportable
// violation, but first-match execution still runs the earlier item.
TEST_CASE("unique case inside: overlapping range and label report and run the first") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] status;
            initial begin
                status = 3'd5;
                unique case (status) inside
                    [0:5]: $display("A");
                    5, 7:  $display("B");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
    CHECK(run_capture(src) == "A\n");
}

// Repeated/overlapping members within one item are explicitly legal (11.4.13)
// and never a violation -- only a match across DIFFERENT items is an overlap.
TEST_CASE("unique case inside: overlapping members of the same item is not a violation") {
    constexpr std::string_view src = R"(
        module m;
            logic [2:0] status;
            initial begin
                status = 3'd5;
                unique case (status) inside
                    [0:5], 5: $display("A");
                    6, 7:     $display("B");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).empty());
    CHECK(run_capture(src) == "A\n");
}

// A range with a tolerance bound (LRM 11.4.13's `+/-` / `+%-`) is a documented
// gap -- guarded loudly rather than silently mis-simulated.
TEST_CASE("case inside: tolerance range bound is reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            int x;
            initial begin
                x = 5;
                case (x) inside
                    [3 +/- 2]: $display("hit");
                    default:   $display("miss");
                endcase
            end
        endmodule
    )");
    CHECK_FALSE(r.ok);
}

// An unbounded (`$`) range bound is likewise a documented gap.
TEST_CASE("case inside: unbounded range bound is reported as unsupported") {
    const LowerResult r = lower_text(R"(
        module m;
            int x;
            initial begin
                x = 5;
                case (x) inside
                    [$:10]:  $display("hit");
                    default: $display("miss");
                endcase
            end
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("unbounded") != std::string::npos);
}

// -- Recursive switch segmentation (find_switch_run) --
// A non-constant/range/wildcard item no longer disables switch dispatch for
// the REST of the statement -- only the item's own run boundary. These cases
// specifically exercise more than one switch run in a single statement.

// Two clean-constant runs split by a residual (non-constant label) item: both
// runs dispatch through their own switch, the residual item's own test still
// runs in between, and priority order holds across all three segments.
TEST_CASE("case: two switch runs split by a residual item stay in priority order") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            logic [3:0] k;
            initial begin
                k = 4'd15;
                for (int i = 0; i < 7; i++) begin
                    s = i[3:0];
                    case (s)
                        4'd1: $display("A");
                        4'd2: $display("B");
                        k:    $display("K");
                        4'd5: $display("D");
                        4'd6: $display("E");
                        default: $display("F");
                    endcase
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "F\nA\nB\nF\nF\nD\nE\n");
}

// A duplicate constant that lands INSIDE a switch run (not just the residual
// chain) still resolves to the first occurrence: case_dedup's whole-statement
// pass is what find_switch_run consults, not a locally re-derived dedup.
TEST_CASE("case: a within-run duplicate constant resolves to the first occurrence") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            logic [3:0] k;
            initial begin
                k = 4'd15;
                s = 4'd5;
                case (s)
                    k:    $display("R");
                    4'd5: $display("A");
                    4'd5: $display("Adup");
                    default: $display("F");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "A\n");
}

// case ... inside with two constant runs split by a range item: a wholly new
// capability (case-inside previously never reached the switch at all).
TEST_CASE("case inside: multiple switch runs split by a range item") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            initial begin
                for (int i = 0; i < 7; i++) begin
                    s = i[3:0];
                    case (s) inside
                        1, 2:    $display("A");
                        [3:4]:   $display("B");
                        5, 6:    $display("C");
                        default: $display("D");
                    endcase
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "D\nA\nA\nB\nB\nC\nC\n");
}

// unique: a duplicate constant inside a switch-eligible run is still a
// reportable overlap (the overlap-check pass is fully decoupled from
// dispatch -- it sums every item's own predicate unconditionally), while
// dispatch itself now runs through the run's deduped switch instead of the
// old had_dup-gated whole-statement chain fallback.
TEST_CASE("unique case: duplicate inside a switch run still reports overlap") {
    constexpr std::string_view src = R"(
        module m;
            logic [1:0] s;
            initial begin
                s = 2'd1;
                unique case (s)
                    2'd1: $display("A");
                    2'd1: $display("B");
                    2'd2: $display("C");
                endcase
            end
        endmodule
    )";
    CHECK(run_capture(src) == "A\n");
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
}

// A run whose every item duplicates an EARLIER residual item's constant
// dedups to zero live arms; find_switch_run must skip switch emission and
// recurse straight through rather than emit an empty/invalid switch.
TEST_CASE("case: a run that dedups to zero arms falls through correctly") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] s;
            logic [3:0] k;
            initial begin
                k = 4'd15;
                for (int i = 0; i < 10; i++) begin
                    s = i[3:0];
                    case (s)
                        4'd9, k: $display("R");
                        4'd9:    $display("dup1");
                        4'd9:    $display("dup2");
                        default: $display("F");
                    endcase
                end
            end
        endmodule
    )";
    CHECK(run_capture(src) == "F\nF\nF\nF\nF\nF\nF\nF\nF\nR\n");
}

// type() (LRM 6.23): a type reference is a compile-time-only marker, legal
// only in an equality comparison with another type reference or as a case
// selector/item. Resolved entirely at lowering time (Type::isMatching), not
// through the runtime packed-comparison path.
TEST_CASE("type() equality and inequality comparisons") {
    constexpr std::string_view src = R"(
        module m;
            bit [12:0] a;
            real r;
            initial begin
                if (type(a) == type(bit[12:0])) $display("eq-same"); else $display("eq-diff");
                if (type(a) == type(r)) $display("eq-same"); else $display("eq-diff");
                if (type(a) != type(r)) $display("ne-diff"); else $display("ne-same");
                // byte is `bit signed [7:0]` (LRM 6.11.1) -- matches only the
                // signed form, not plain (unsigned) bit[7:0].
                if (type(bit[7:0]) == type(byte)) $display("eq-same"); else $display("eq-diff");
                if (type(bit signed [7:0]) == type(byte)) $display("eq-same"); else $display("eq-diff");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "eq-same\neq-diff\nne-diff\neq-diff\neq-same\n");
}

// LRM 6.23's own example shape: a parameter type resolved via type(), dispatched
// through a `case (type(...))` selector.
TEST_CASE("case statement over type() selects the matching type item") {
    constexpr std::string_view src = R"(
        module m #(parameter type T = int) ();
            initial begin
                case (type(T))
                    type(bit[12:0]): $display("bit12");
                    type(real):      $display("real");
                    default:         $display("other");
                endcase
            end
        endmodule
        module top;
            m #(.T(bit[12:0])) u1();
            m #(.T(real)) u2();
            m #(.T(int)) u3();
        endmodule
    )";
    CHECK(run_capture(src) == "bit12\nreal\nother\n");
}

// No matching item and no default: the case statement is a no-op (mirrors
// plain-value case semantics), not a loud failure.
TEST_CASE("case statement over type() with no match and no default is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                case (type(int))
                    type(real): $display("real");
                endcase
                $display("done");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "done\n");
}

// LRM 12.6.1: a pattern-matching case can carry unique/priority qualifiers,
// which report the same violations as a plain case (LRM 12.5.3).
TEST_CASE("priority pattern case: no match, no default reports a violation") {
    constexpr std::string_view src = R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            initial begin
                opt_t u;
                u = tagged Invalid;
                priority case (u) matches
                    tagged Valid .n : $display("valid %0d", n);
                endcase
                $display("done");
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("no condition is true") != std::string::npos);
    // Nothing matched (Invalid), no default: only the trailing $display fires.
    CHECK(run_capture(src) == "done\n");
}

TEST_CASE("unique pattern case: two matching items report an overlap") {
    constexpr std::string_view src = R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            initial begin
                opt_t u;
                u = tagged Valid (5);
                unique case (u) matches
                    tagged Valid .a : $display("a %0d", a);
                    tagged Valid .b : $display("b %0d", b);
                endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
    // First-match execution is unchanged: only the first Valid item runs.
    CHECK(run_capture(src) == "a 5\n");
}

// -- real selectors and labels (LRM 12.5) --
// 12.5 words matching as a bit-for-bit `===` and Table 11-1 bars `===` on a
// real, so the standard defines the comparison only for integral operands. A
// real carries no x/z -- the entire content of the 4-state rule -- so clotho
// matches with IEEE-754 equality, the same degeneration it already applies to a
// string (6.16.9). iverilog 13 agrees on every case below.

// An integral label against a real selector converts to real (11.8.1), so `2`
// matches 2.0; -0.0 and 0.0 are the same IEEE-754 value; a multi-label item ORs
// its labels.
TEST_CASE("case with a real selector compares by value") {
    const std::string out = run_capture(R"(
        module m;
            real r;
            initial begin
                r = 2.0;
                case (r) 1.5: $display("a"); 2: $display("int-label"); default: $display("d"); endcase
                r = -0.0;
                case (r) 0.0: $display("negzero"); default: $display("nz-d"); endcase
                r = 2.5;
                case (r) 1.5, 2.5, 3.5: $display("multi"); default: $display("md"); endcase
                r = 9.0;
                case (r) 1.5: $display("x"); 2.5: $display("y"); default: $display("dflt"); endcase
            end
        endmodule
    )");
    CHECK(out == "int-label\nnegzero\nmulti\ndflt\n");
}

// casez / casex over a real never reaches lowering: 12.5.1's do-not-care
// positions are BIT positions, and slang's binder refuses a non-integral
// selector for either keyword ("invalid type 'real' for casez expression").
// iverilog 13 accepts it and treats it as a plain case; clotho follows slang's
// stricter reading, and the rejection is pinned in tests/real_test.cpp.

// 12.5's linear search stops at the first match, so a later label is never
// evaluated -- the real path chains lazily rather than computing every
// predicate up front. iverilog also reports zero calls to bump().
TEST_CASE("a real case stops the search at the first match") {
    const std::string out = run_capture(R"(
        module m;
            real r;
            int  hit;
            function automatic real bump();
                hit = hit + 1;
                return 9.0;
            endfunction
            initial begin
                hit = 0;
                r = 1.0;
                case (r) 1.0: $display("first"); bump(): $display("second"); default: $display("dd"); endcase
                $display("bump calls=%0d", hit);
            end
        endmodule
    )");
    CHECK(out == "first\nbump calls=0\n");
}

// `case ... inside` over reals: 11.4.13 membership is the pair of relational
// comparisons, and Table 11-1 DOES define those for reals, so this arm needs no
// interpretation. iverilog 13 does not parse a ranged `case ... inside` at all,
// so the expectations are the LRM's: closed bounds, and a singular member
// compares by value. (A case_inside_item is a bare range_list, A.6.7: no braces,
// unlike the `inside` operator's open_range_list.)
TEST_CASE("case inside with real ranges and members") {
    const std::string out = run_capture(R"(
        module m;
            real r;
            initial begin
                r = 1.25;
                case (r) inside [1.0:2.0]: $display("in-range"); default: $display("out"); endcase
                r = 5.5;
                case (r) inside [1.0:2.0], 5.5: $display("in-set"); default: $display("out2"); endcase
                r = 3.0;
                case (r) inside [1.0:2.0], 5.5: $display("in3"); default: $display("out3"); endcase
                r = 2.0;
                case (r) inside [1.0:2.0]: $display("closed-hi"); default: $display("open-hi"); endcase
            end
        endmodule
    )");
    CHECK(out == "in-range\nin-set\nout3\nclosed-hi\n");
}

// unique/unique0 keep their 12.5.3 checks on the real path: the per-item
// predicates that feed the overlap sum are the same ones dispatch uses.
TEST_CASE("unique case on a real reports an overlap and runs the first match") {
    constexpr std::string_view src = R"(
        module m;
            real r;
            initial begin
                r = 1.5;
                unique case (r) 1.5: $display("a"); 1.5: $display("b"); endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("not unique") != std::string::npos);
    CHECK(run_capture(src) == "a\n");
}

// A default-less priority case over reals that matches nothing reports the
// no-match violation, exactly as the integral path does.
TEST_CASE("priority case on a real reports a no-match") {
    constexpr std::string_view src = R"(
        module m;
            real r;
            initial begin
                r = 7.25;
                priority case (r) 1.5: $display("a"); 2.5: $display("b"); endcase
            end
        endmodule
    )";
    CHECK(run_capture_stderr(src).find("no condition is true") != std::string::npos);
    CHECK(run_capture(src).empty());
}

// A negative 4-state signed selector against a negative signed label (LRM 12.5:
// "If all of these expressions are signed, then they shall be treated as
// signed"). The label's value is -1 at every width, so every form must match.
// The jump-table path, taken while a 4-state compare fits 64 key bits (width
// <= 32), keyed a negative label on its sign-extended 64-bit value instead of its
// bits, so `-1` never equalled the selector. Widths straddle that boundary
// (31/32 use it, 33 and up fall back to the compare chain). Expectations are the
// LRM text; Verilator 5.050 agrees on every form, Icarus Verilog 13 on all but
// `case ... inside`, which it cannot parse.
TEST_CASE("case on a negative 4-state signed value matches a signed label (LRM 12.5)") {
    constexpr int widths[] = { 2, 3, 8, 16, 31, 32, 33, 64, 65 };
    std::string   src;
    std::string   expected;
    int           t = 0;
    for (const int n : widths) {
        ++t;
        src += std::format(R"(
        module m{0};
            logic signed [{0}-1:0] s;
            bit c1, c2, c3, c4, c5, c6, c7, c8, c9, c10;
            initial begin
                #{1};
                s = -1;
                case (s) -{0}'sd1: c1 = 1; default: c1 = 0; endcase
                case (s) -1: c2 = 1; default: c2 = 0; endcase
                case (s) inside -1: c3 = 1; default: c3 = 0; endcase
                unique case (s) 1, -1: c4 = 1; 2: c4 = 0; default: c4 = 0; endcase
                casez (s) -1: c5 = 1; default: c5 = 0; endcase
                casex (s) -1: c6 = 1; default: c6 = 0; endcase
                unique0 case (s) 1: c9 = 0; -1: c9 = 1; default: c9 = 0; endcase
                priority case (s) -1: c10 = 1; default: c10 = 0; endcase
                s = 1;
                case (s) 1: c7 = 1; default: c7 = 0; endcase
                case (s) -1: c8 = 1; default: c8 = 0; endcase
                $display("{0}: %b%b%b%b%b%b%b%b%b%b", c1, c2, c3, c4, c5, c6, c7, c8, c9, c10);
            end
        endmodule
        )",
                           n,
                           t);
        expected += std::format("{}: 1111111011\n", n);
    }
    src += "module top;\n";
    for (const int n : widths) {
        src += std::format("    m{0} u{0}();\n", n);
    }
    src += "endmodule\n";
    CHECK(run_capture(src) == expected);
}

// If any case expression is unsigned all are compared as unsigned (LRM 12.5), so
// a negative signed selector zero-extends to the wider unsigned label's width.
TEST_CASE("case with a mixed-sign label compares unsigned") {
    CHECK(run_capture(R"(
        module m;
            logic signed [2:0] s;
            initial begin
                s = -1;
                case (s) 4'b0111: $display("zero-extended"); default: $display("other"); endcase
                case (s) 4'b1111: $display("sign-extended"); default: $display("other"); endcase
                case (s) 3'b111: $display("same width"); default: $display("other"); endcase
            end
        endmodule
    )") == "zero-extended\nother\nsame width\n");
}

// Labels of other widths and signs against a signed 4-state selector: the
// comparison is made at the longest width, signed only if every operand is
// (LRM 12.5). iverilog 13 gives the same ten answers.
TEST_CASE("case compares at the longest width, signed only if all operands are") {
    CHECK(run_capture(R"(
        module m;
            logic signed [7:0] s;
            logic signed [2:0] n;
            logic [2:0] u;
            initial begin
                s = -1; n = -1; u = 3'b111;
                case (s) 3'sb111: $display("A match"); default: $display("A NO"); endcase
                case (s) 3'b111: $display("B match"); default: $display("B NO"); endcase
                case (n) 8'sb11111111: $display("C match"); default: $display("C NO"); endcase
                case (n) 8'b00000111: $display("D match"); default: $display("D NO"); endcase
                case (u) -1: $display("E match"); default: $display("E NO"); endcase
                case (s) n: $display("F match"); default: $display("F NO"); endcase
                case (s) -8'sd1, 5: $display("G match"); default: $display("G NO"); endcase
                s = -128;
                case (s) -128: $display("H match"); default: $display("H NO"); endcase
                case (s) 8'sh80: $display("I match"); default: $display("I NO"); endcase
                case (s) 8'h80: $display("J match"); default: $display("J NO"); endcase
            end
        endmodule
    )") == "A match\nB NO\nC match\nD match\nE NO\nF match\nG match\nH match\nI match\nJ match\n");
}

// An unsized `'bx` / `'bz` label is extended with x / z to the width every case
// expression is compared at (LRM 5.7.1, 12.5), so against a 64-bit selector it is
// 64 x bits, not 32 x bits under 32 zeros. A plain case then needs an exact match
// (x is not z), and casez / casex read the label's z / x as do-not-care. Icarus
// Verilog 13 agrees.
TEST_CASE("an unsized x or z case label takes the width of the selector") {
    CHECK(run_capture(R"(
        module m;
            logic [63:0] x64 = 64'bx, z64 = 64'bz, v = 64'h1234;
            initial begin
                case (x64) 'bx: $display("x match"); default: $display("x none"); endcase
                case (z64) 'bx: $display("zx match"); default: $display("zx none"); endcase
                case (z64) 'bz: $display("z match"); default: $display("z none"); endcase
                case (v) 'bx: $display("v bx"); default: $display("v none"); endcase
                casez (v) 'bz: $display("casez any"); default: $display("casez none"); endcase
                casex (v) 'bx: $display("casex any"); default: $display("casex none"); endcase
            end
        endmodule
    )") == "x match\nzx none\nz match\nv none\ncasez any\ncasex any\n");
}
