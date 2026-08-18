#include <algorithm>
#include <vector>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>

#include "Lower.h"
#include "ir/Ir.h"
#include "ir/Module.h"

using namespace clotho;

namespace {

struct Design {
    std::string_view name;
    std::string_view source;
};

// A spread of opcodes wide enough that the invariant sweep below is worth
// running. These are lowered, never executed, so $fscanf's file need not exist.
constexpr Design kDesigns[] = {
    // $monitor is the case that motivated this file: its extra record is a
    // StrobePendInfo, not an operand run.
    { "monitor",
      R"(
        module m;
          int x;
          initial begin
            x = 1;
            $monitor("x=%0d", x);
            $strobe("x=%0d", x);
            $display("x=%0d", x);
            $write("w");
            $monitoroff;
            $monitoron;
            #1 $finish;
          end
        endmodule
      )" },
    { "file_io",
      R"(
        module m;
          integer fd, n, a, b;
          string s;
          initial begin
            fd = $fopen("clotho_operands_test_missing.txt", "r");
            n = $fscanf(fd, "%d %d", a, b);
            n = $sscanf("1 2", "%d %d", a, b);
            $fdisplay(fd, "a=%0d", a);
            $sformat(s, "a=%0d", a);
            s = $sformatf("b=%0d", b);
            $fclose(fd);
          end
        endmodule
      )" },
    { "control",
      R"(
        module m;
          int i, y;
          logic [7:0] sel;
          initial begin
            for (i = 0; i < 4; i = i + 1) y = y + i;
            while (i > 0) i = i - 1;
            repeat (3) y = y + 1;
            case (sel)
              8'd0: y = 1;
              8'd1, 8'd2: y = 2;
              default: y = 3;
            endcase
            if (y inside {1, 2, [5:9]}) y = 0;
          end
        endmodule
      )" },
    { "fork",
      R"(
        module m;
          int g;
          task automatic tk(input int d);
            int v;
            v = d;
            fork
              #1 g = v;
              #2 v = v + 1;
            join
          endtask
          initial fork
            #1 $display("a");
            #2 $display("b");
          join
          initial tk(3);
        endmodule
      )" },
    { "containers",
      R"(
        module m;
          int q[$];
          int d[];
          int aa[string];
          int v;
          initial begin
            q.push_back(1);
            q.push_front(2);
            v = q.pop_back();
            v = q.size();
            q.delete();
            d = new[4];
            v = d.size();
            d.delete();
            aa["k"] = 5;
            v = aa["k"];
            v = aa.num();
            if (aa.exists("k")) aa.delete("k");
          end
        endmodule
      )" },
    { "exprs",
      R"(
        module m;
          logic [7:0] a, b;
          logic [15:0] c;
          logic [31:0] r;
          initial begin
            a = 8'hA5;
            c = {a, b};
            r = {4{a}};
            b = a[3:0];
            b = c[a];
            b = a ? b : c[7:0];
            r = a + b * 2;
          end
        endmodule
      )" },
    { "strength",
      R"(
        module m;
          wire w;
          logic x, y;
          assign (strong0, strong1) w = x;
          assign (weak0,   weak1)   w = y;
        endmodule
      )" },
    { "subroutines",
      R"(
        module m;
          function automatic int dbl(input int v); return v * 2; endfunction
          task automatic show(input int v); $display("%0d", v); endtask
          int r;
          initial begin
            r = dbl(21);
            show(r);
            if (r != 42) $error("bad %0d", r);
            $info("info");
            $warning("warn");
          end
        endmodule
      )" },
};

// The invariant every operand must satisfy, for every tag, in every body: an
// operand is a real instruction of the same body that was created before its
// user. A span decoded from the wrong extra record breaks one or both, so this
// catches a mis-decoded tag without needing to know what that tag is.
void check_body(const ir::Ir& ir, std::string_view design, std::string_view body, std::set<ir::Inst::Tag>& seen) {
    std::array<ir::Index, 4> scratch;
    const uint32_t           n = ir.inst_count();

    for (uint32_t k = 0; k < n; ++k) {
        const auto i   = static_cast<ir::Index>(k);
        const auto tag = static_cast<uint32_t>(ir.tag(i));
        seen.insert(ir.tag(i));

        const std::span<const ir::Index> ops = ir.operands(i, scratch);
        // Size first: a fabricated {start, end} pair whose end precedes its
        // start wraps to a huge count, and dereferencing that segfaults before
        // any per-operand check could report it.
        REQUIRE_MESSAGE(ops.size() <= n,
                        design,
                        "/",
                        body,
                        ": tag ",
                        tag,
                        " at %",
                        k,
                        " returned ",
                        ops.size(),
                        " operands but the body only has ",
                        n,
                        " instructions");

        for (const ir::Index op : ops) {
            const auto raw = static_cast<uint32_t>(op);
            REQUIRE_MESSAGE(raw < n,
                            design,
                            "/",
                            body,
                            ": tag ",
                            tag,
                            " at %",
                            k,
                            " returned out-of-bounds operand %",
                            raw,
                            " (body has ",
                            n,
                            " instructions)");
            REQUIRE_MESSAGE(raw < k,
                            design,
                            "/",
                            body,
                            ": tag ",
                            tag,
                            " at %",
                            k,
                            " returned operand %",
                            raw,
                            " which does not precede it");
        }

        // The total walk gets the same treatment, plus the two properties that
        // relate it to the span: it must be a superset, and it must actually
        // deliver a tail where the classifier claims one. Without the second
        // check a for_each_operand that silently forgot its switch would pass
        // everything else here.
        std::vector<ir::Index> all;
        ir.for_each_operand(i, [&](ir::Index op) { all.push_back(op); });

        REQUIRE_MESSAGE(all.size() <= n + 2,
                        design,
                        "/",
                        body,
                        ": tag ",
                        tag,
                        " at %",
                        k,
                        " walked ",
                        all.size(),
                        " operands but the body only has ",
                        n,
                        " instructions");

        for (const ir::Index op : all) {
            const auto raw = static_cast<uint32_t>(op);
            REQUIRE_MESSAGE(raw < n,
                            design,
                            "/",
                            body,
                            ": tag ",
                            tag,
                            " at %",
                            k,
                            " walked out-of-bounds operand %",
                            raw,
                            " (body has ",
                            n,
                            " instructions)");
            REQUIRE_MESSAGE(raw < k,
                            design,
                            "/",
                            body,
                            ": tag ",
                            tag,
                            " at %",
                            k,
                            " walked operand %",
                            raw,
                            " which does not precede it");
        }

        for (const ir::Index op : ops) {
            REQUIRE_MESSAGE(std::find(all.begin(), all.end(), op) != all.end(),
                            design,
                            "/",
                            body,
                            ": tag ",
                            tag,
                            " at %",
                            k,
                            ": operands() returned %",
                            static_cast<uint32_t>(op),
                            " which for_each_operand does not walk");
        }

        const bool has_tail = ir::extra_operand_tail(ir.tag(i)) != ir::ExtraOperandTail::None;
        REQUIRE_MESSAGE(has_tail == (all.size() > ops.size()),
                        design,
                        "/",
                        body,
                        ": tag ",
                        tag,
                        " at %",
                        k,
                        ": classifier says tail=",
                        has_tail,
                        " but the walk added ",
                        all.size() - ops.size(),
                        " operands");
    }
}

std::set<ir::Inst::Tag> sweep_all() {
    std::set<ir::Inst::Tag> seen;

    for (const Design& d : kDesigns) {
        const LowerResult r = lower_text(d.source);
        REQUIRE_MESSAGE(r.ok, d.name, ": ", r.diagnostic_text());
        REQUIRE(r.module != nullptr);

        const ir::Module& m = *r.module;
        for (size_t k = 0; k < m.processes().size(); ++k) {
            check_body(m.processes()[k].ir, d.name, "process", seen);
        }
        for (size_t f = 0; f < m.function_count(); ++f) {
            check_body(m.function(static_cast<ir::FunctionId>(f)).ir, d.name, "function", seen);
        }
    }

    return seen;
}

} // namespace

// The generic sweep. Ir::operands() is one of the two reusable walkers the IR
// layer exports, so it has to be total: any consumer may call it on any tag.
//
// What this does and does not guarantee is worth being precise about. A span
// decoded from the wrong record is caught here whenever the bogus {start, end}
// escapes the body -- the reversed-pair case wraps to a huge count and the
// out-of-range case points past the end. It is NOT caught when the fabricated
// pair happens to land in bounds with backward-pointing values, which is
// exactly what reintroducing the MonitorSet bug produces on these designs. That
// silent variant is pinned by the shape-table and monitor_set cases below; this
// sweep is the net for tags nobody thought to write a focused test for.
TEST_CASE("operands() returns only real, dominating operands for every tag") {
    const std::set<ir::Inst::Tag> seen = sweep_all();
    // A lowering change that gutted the sweep would still pass the pinned-tag
    // check below if it kept those few; this guards the breadth around them.
    CHECK(seen.size() >= 50);
}

// The sweep is only worth as much as the opcodes it reaches, so pin the ones
// that motivated it. Without this a lowering change could quietly stop
// producing MonitorSet and the regression above would pass vacuously.
TEST_CASE("the operand sweep reaches the tags it is meant to cover") {
    const std::set<ir::Inst::Tag> seen = sweep_all();

    for (const ir::Inst::Tag t : { ir::Inst::Tag::MonitorSet,
                                   ir::Inst::Tag::StrobePend,
                                   ir::Inst::Tag::Display,
                                   ir::Inst::Tag::FDisplay,
                                   ir::Inst::Tag::Fscanf,
                                   ir::Inst::Tag::Sscanf,
                                   ir::Inst::Tag::Sformat,
                                   ir::Inst::Tag::InsideSet,
                                   ir::Inst::Tag::Fork,
                                   ir::Inst::Tag::Loop,
                                   ir::Inst::Tag::Concat,
                                   ir::Inst::Tag::Repl,
                                   ir::Inst::Tag::StrengthRes,
                                   ir::Inst::Tag::Finish,
                                   ir::Inst::Tag::Call,
                                   ir::Inst::Tag::TaskCall,
                                   ir::Inst::Tag::Severity,
                                   ir::Inst::Tag::QueuePushBack,
                                   ir::Inst::Tag::AssocSelect }) {
        CHECK_MESSAGE(seen.contains(t), "sweep never reached tag ", static_cast<uint32_t>(t));
    }
}

// The structural half of the fix: PayloadKind cannot say how to decode an
// Extra record because all nine Extra tags share the `extra` member, so the
// shape table has to cover exactly them. Drift in either direction is a bug --
// an unclassified Extra tag decodes as the wrong record, and a classified
// non-Extra tag means someone misread which member is live.
TEST_CASE("extra_operand_shape classifies exactly the Extra-payload tags") {
    CHECK(ir::extra_operand_shapes_total());

    for (unsigned v = 0; v <= 0xFF; ++v) {
        const auto tag        = static_cast<ir::Inst::Tag>(v);
        const bool is_extra   = ir::payload_kind(tag) == ir::Inst::PayloadKind::Extra;
        const bool classified = ir::extra_operand_shape(tag) != ir::ExtraOperandShape::NotExtra;
        CHECK_MESSAGE(
          is_extra == classified, "tag ", v, ": payload_kind Extra=", is_extra, " but classified=", classified);
    }
}

// Concat is the only tag whose extra IS the operand run; everything else in the
// family either has a named slice inside its record or no operands at all.
// Getting this wrong is precisely the MonitorSet bug.
TEST_CASE("only concat's extra payload is itself an operand run") {
    using S = ir::ExtraOperandShape;
    CHECK(ir::extra_operand_shape(ir::Inst::Tag::Concat) == S::SliceIsOperands);
    CHECK(ir::extra_operand_shape(ir::Inst::Tag::StrengthRes) == S::StrengthResValues);
    // fork: its record holds BOTH a FunctionId run (the children) and an operand
    // run (the frame lvalues the children capture by reference), so the named
    // slice, not the record, is the operand span.
    CHECK(ir::extra_operand_shape(ir::Inst::Tag::Fork) == S::ForkArgs);

    for (const ir::Inst::Tag t : { ir::Inst::Tag::MonitorSet,
                                   ir::Inst::Tag::StrobePend,
                                   ir::Inst::Tag::Loop,
                                   ir::Inst::Tag::Finish,
                                   ir::Inst::Tag::CaseViol,
                                   ir::Inst::Tag::TestPlusArgs }) {
        CHECK_MESSAGE(ir::extra_operand_shape(t) == S::None,
                      "tag ",
                      static_cast<uint32_t>(t),
                      " must not decode as an operand slice");
    }
}

// The original crash, pinned directly: monitor_set carries a StrobePendInfo
// (one FunctionId word). Decoded as an ExtraSlice that word becomes `start` and
// an unrelated neighbouring word becomes `end`, so the deferred body's args
// masquerade as operands of the monitor_set itself.
TEST_CASE("monitor_set exposes no operands") {
    const LowerResult r = lower_text(kDesigns[0].source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::array<ir::Index, 4> scratch;
    bool                     found = false;
    for (const ir::Process& p : r.module->processes()) {
        for (uint32_t k = 0; k < p.ir.inst_count(); ++k) {
            const auto i = static_cast<ir::Index>(k);
            if (p.ir.tag(i) != ir::Inst::Tag::MonitorSet) {
                continue;
            }
            found = true;
            CHECK(p.ir.operands(i, scratch).empty());
        }
    }
    CHECK(found);
}

// The tail table's own drift check, mirroring the shape one above. Three of the
// four tails are implied by PayloadKind, so they are biconditional: a new tag
// added to any of those kinds fails here until classified, and a tag wrongly
// given one of those tails fails too.
TEST_CASE("extra_operand_tail classifies exactly the tags whose operands overflow one span") {
    CHECK(ir::extra_operand_tails_total());

    using K = ir::Inst::PayloadKind;
    using E = ir::ExtraOperandTail;
    for (unsigned v = 0; v <= 0xFF; ++v) {
        const auto tag  = static_cast<ir::Inst::Tag>(v);
        const auto tail = ir::extra_operand_tail(tag);
        CHECK((ir::payload_kind(tag) == K::FDisplay) == (tail == E::Descriptor));
        CHECK((ir::payload_kind(tag) == K::Sformat) == (tail == E::SformatDst));
        CHECK((ir::payload_kind(tag) == K::Scanf) == (tail == E::ScanfSource));
        if (tail == E::InsideSetMembers) {
            CHECK(tag == ir::Inst::Tag::InsideSet);
        }
    }
}

// Exactly seven tags, named. The count is the claim the header comment makes, so
// pin it: an eighth appearing means someone widened a record without extending
// the walk, and one disappearing means the walk got a capability nobody noticed.
TEST_CASE("exactly seven tags hold operands back from the span") {
    std::set<ir::Inst::Tag> tailed;
    for (unsigned v = 0; v <= 0xFF; ++v) {
        const auto tag = static_cast<ir::Inst::Tag>(v);
        if (ir::extra_operand_tail(tag) != ir::ExtraOperandTail::None) {
            tailed.insert(tag);
        }
    }

    const std::set<ir::Inst::Tag> expected = { ir::Inst::Tag::FDisplay, ir::Inst::Tag::MonitorEmit,
                                               ir::Inst::Tag::Fscanf,   ir::Inst::Tag::Sscanf,
                                               ir::Inst::Tag::Sformat,  ir::Inst::Tag::InsideSet,
                                               // Wait: an event edge may park on a
                                               // handle VALUE, strided inside the
                                               // packed WaitEdge run rather than a
                                               // slice, so it cannot be a span.
                                               ir::Inst::Tag::Wait };
    CHECK(tailed == expected);
}

// InsideSet is the tag whose tail carries the most: two whole slices, where the
// span holds only the selector. The corresponding precision claim -- that a
// definite `inside` now proves definite -- lives in analysis_test.cpp, which
// owns the DesignFacts helpers.
TEST_CASE("inside walks its selector, members and range endpoints") {
    constexpr std::string_view src = R"(
        module m;
          logic [3:0] sel;
          logic       hit;
          initial begin
            sel = 4'd3;
            hit = sel inside {4'd1, 4'd3, [4'd8:4'd9]};
            $display("%b", hit);
          end
        endmodule
    )";

    const LowerResult r = lower_text(src);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    bool found = false;
    for (const ir::Process& p : r.module->processes()) {
        const ir::Ir& ir = p.ir;
        for (uint32_t k = 0; k < ir.inst_count(); ++k) {
            const auto i = static_cast<ir::Index>(k);
            if (ir.tag(i) != ir::Inst::Tag::InsideSet) {
                continue;
            }
            found = true;

            std::array<ir::Index, 4> scratch;
            // The span is the selector alone.
            CHECK(ir.operands(i, scratch).size() == 1);
            // Two members plus a two-endpoint range, so four more.
            size_t walked = 0;
            ir.for_each_operand(i, [&](ir::Index) { ++walked; });
            CHECK(walked == 5);
        }
    }
    CHECK(found);
}
