#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include "Lower.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a module's initial blocks and return everything it wrote to stdout.
// The case statements under test sit in an `initial` block and report the chosen
// branch with $display, so the captured text is the observable behavior.
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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

// unique case with duplicate labels: dedup is skipped so the items survive
// (overlap is a reportable violation; first-match execution stays conforming).
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
    REQUIRE_MESSAGE(r.ok, r.diagnostics);
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
    const std::string err = run_capture_stderr(src);
    CHECK(err.find("no condition is true") != std::string::npos);
    // Design output is unchanged: nothing matched, so no $display fired.
    CHECK(run_capture(src).empty());
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
    const std::string err = run_capture_stderr(src);
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
    CHECK(run_capture(src) == "hi\n");  // first match wins despite the violation
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
// value both items match. Detected at runtime (routed off the switch fast path).
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
    CHECK(run_capture(src) == "x\n");  // first match still runs
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
    CHECK(run_capture(src).empty());  // nothing matched, so no branch ran
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
    CHECK(run_capture(src) == "lo\n");  // first true arm wins despite the violation
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
    const std::string err = run_capture_stderr(src);
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
