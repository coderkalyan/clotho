#include <doctest/doctest.h>

#include <algorithm>
#include <string_view>

#include "Lower.h"
#include "ir/Analysis.h"

using namespace clotho;

namespace {

struct Analyzed {
    LowerResult     lowered;
    ir::DesignFacts facts;

    const ir::Module& mod() const {
        return *lowered.module;
    }

    // Index of the signal whose (possibly hierarchical) name is `name` or ends
    // with ".name". Fails the test if no such signal exists.
    uint32_t sig(std::string_view name) const {
        const auto signals = mod().signals();
        for (size_t i = 0; i < signals.size(); ++i) {
            const std::string& n = signals[i].name;
            if (n == name || (n.size() > name.size() && n.compare(n.size() - name.size(), name.size(), name) == 0 &&
                              n[n.size() - name.size() - 1] == '.')) {
                return static_cast<uint32_t>(i);
            }
        }
        FAIL("no signal named ", name);
        return 0;
    }
};

Analyzed run(std::string_view source) {
    Analyzed a{ .lowered = lower_text(source), .facts = {} };
    REQUIRE_MESSAGE(a.lowered.ok, a.lowered.diagnostic_text());
    REQUIRE(a.lowered.module != nullptr);
    a.facts = ir::analyze(*a.lowered.module);

    return a;
}

} // namespace

// The two polarities are the load-bearing property of TagInfo: the purity
// whitelist must fail closed (a new op is NOT pure, so it cannot be fused) while
// the thunk blacklist fails open (a new op does not block elision). Getting these
// backwards would either silently disable fusion or silently mis-elide.
TEST_CASE("tag_info keeps the whitelist and blacklist polarities apart") {
    // Pure value ops are fusable and do not block thunk elision.
    for (const ir::Inst::Tag t :
         { ir::Inst::Tag::Add, ir::Inst::Tag::Band, ir::Inst::Tag::Select, ir::Inst::Tag::Cond }) {
        CHECK(ir::tag_info(t).pure);
        CHECK_FALSE(ir::tag_info(t).blocks_thunk);
    }
    // Suspension points and frame-resident RNG block elision and are not pure.
    for (const ir::Inst::Tag t : { ir::Inst::Tag::Delay,
                                   ir::Inst::Tag::Wait,
                                   ir::Inst::Tag::TaskCall,
                                   ir::Inst::Tag::Fork,
                                   ir::Inst::Tag::Urandom,
                                   ir::Inst::Tag::StrobePend }) {
        CHECK(ir::tag_info(t).blocks_thunk);
        CHECK_FALSE(ir::tag_info(t).pure);
    }
    // Effectful ops are neither: not fusable, but they do not stop elision.
    for (const ir::Inst::Tag t :
         { ir::Inst::Tag::Display, ir::Inst::Tag::Store, ir::Inst::Tag::StoreNb, ir::Inst::Tag::Call }) {
        CHECK_FALSE(ir::tag_info(t).pure);
        CHECK_FALSE(ir::tag_info(t).blocks_thunk);
    }
    // $random stays thunk-eligible while $urandom does not -- the distinction is
    // where the stream lives, not that both are random.
    CHECK_FALSE(ir::tag_info(ir::Inst::Tag::Random).blocks_thunk);
    CHECK(ir::tag_info(ir::Inst::Tag::Urandom).blocks_thunk);
}

TEST_CASE("undriven nets are found and partially-driven nets get a bit mask") {
    const Analyzed a = run(R"(
        module top;
          wire [7:0] floating;
          wire [7:0] half;
          logic [3:0] src;
          assign half[3:0] = src;
          initial src = 4'h5;
        endmodule
    )");

    // Nothing drives `floating` at all.
    CHECK(a.facts.undriven[a.sig("floating")]);
    // `half` has a driver, so it is not whole-signal undriven ...
    CHECK_FALSE(a.facts.undriven[a.sig("half")]);
    // ... but its upper nibble is undriven and powers on z.
    const std::span<const uint64_t> mask = a.facts.undriven_mask(static_cast<ir::Signal>(a.sig("half")));
    REQUIRE(mask.size() == 1);
    CHECK((mask[0] & 0x0f) == 0);    // bits 3:0 driven
    CHECK((mask[0] & 0xf0) == 0xf0); // bits 7:4 undriven
    // A fully undriven net takes the whole-signal path, not a partial mask.
    CHECK(a.facts.undriven_mask(static_cast<ir::Signal>(a.sig("floating"))).empty());
}

TEST_CASE("may_z proves a clean design never-z and tracks a z literal") {
    const Analyzed clean = run(R"(
        module top;
          logic a, b;
          initial begin a = 0; b = a; end
        endmodule
    )");
    CHECK_FALSE(clean.facts.may_z[clean.sig("a")]);
    CHECK_FALSE(clean.facts.may_z[clean.sig("b")]);

    const Analyzed zed = run(R"(
        module top;
          logic [3:0] a, b;
          initial begin a = 4'bz01x; b = a; end
        endmodule
    )");
    CHECK(zed.facts.may_z[zed.sig("a")]);
    // z rides through a plain copy.
    CHECK(zed.facts.may_z[zed.sig("b")]);
}

TEST_CASE("may_z absorption: an arithmetic result is never z even from a z operand") {
    const Analyzed a = run(R"(
        module top;
          logic [3:0] z, sum;
          initial begin z = 4'bzzzz; sum = z + 1; end
        endmodule
    )");
    CHECK(a.facts.may_z[a.sig("z")]);
    // Add emits a zero z plane: a z operand reads as x, so the result is never z.
    CHECK_FALSE(a.facts.may_z[a.sig("sum")]);
    // But it IS may-x, because the absorbed z becomes x.
    CHECK(a.facts.may_x[a.sig("sum")]);
}

TEST_CASE("may_x is clean for 2-state storage and set by an x literal") {
    const Analyzed a = run(R"(
        module top;
          bit [7:0] two_state;
          initial two_state = 8'hAA;
        endmodule
    )");
    // A 2-state signal has no x plane at all.
    CHECK_FALSE(a.facts.may_x[a.sig("two_state")]);

    const Analyzed x = run(R"(
        module top;
          logic [3:0] a, b;
          initial begin a = 4'bx0x0; b = a; end
        endmodule
    )");
    CHECK(x.facts.may_x[x.sig("a")]);
    CHECK(x.facts.may_x[x.sig("b")]);
}

TEST_CASE("a 2-state conversion clears x as well as z") {
    const Analyzed a = run(R"(
        module top;
            logic [3:0] l;
            bit   [3:0] b;
            initial begin
                l = 4'bx0x0;
                b = l;
            end
        endmodule
    )");
    CHECK(a.facts.may_x[a.sig("l")]);
    // LRM 6.3.1: the 2-state target keeps only the must plane.
    CHECK_FALSE(a.facts.may_x[a.sig("b")]);
}

TEST_CASE("design-wide use flags gate scheduler machinery") {
    const Analyzed plain = run("module top; initial $finish; endmodule");
    CHECK_FALSE(plain.facts.uses.strobe);
    CHECK_FALSE(plain.facts.uses.monitor);
    CHECK_FALSE(plain.facts.uses.vcd);

    const Analyzed rich = run(R"(
        module top;
          logic a;
          initial begin
            $dumpfile("x.vcd");
            $dumpvars;
            a = 1;
            $strobe("%0d", a);
            $monitor("%0d", a);
            $finish;
          end
        endmodule
    )");
    CHECK(rich.facts.uses.strobe);
    CHECK(rich.facts.uses.monitor);
    CHECK(rich.facts.uses.vcd);
}

TEST_CASE("per-process read and write sets resolve through select chains") {
    const Analyzed a = run(R"(
        module top;
          logic [7:0] src, dst;
          always @(src) dst[3:0] = src[7:4];
          initial src = 8'h5A;
        endmodule
    )");

    const uint32_t src   = a.sig("src");
    const uint32_t dst   = a.sig("dst");
    bool           found = false;
    for (size_t k = 0; k < a.mod().processes().size(); ++k) {
        if (a.mod().processes()[k].activation != ir::Activation::Always) {
            continue;
        }
        const ir::BodyFacts& b = a.facts.processes[k].body;
        // A part-select read/write attributes to the base signal.
        const bool reads_src = std::find(b.reads.begin(), b.reads.end(), static_cast<ir::Signal>(src)) != b.reads.end();
        const bool writes_dst =
          std::find(b.writes.begin(), b.writes.end(), static_cast<ir::Signal>(dst)) != b.writes.end();
        if (reads_src && writes_dst) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("comb-fusion verdict rejects a read outside the sensitivity list") {
    // Explicit `@(a)` while reading `b`: a spurious fused run would sample a
    // value the unfused schedule never observes.
    const Analyzed a   = run(R"(
        module top;
          logic a, b, y;
          always @(a) y = a & b;
          initial begin a = 0; b = 0; end
        endmodule
    )");
    bool           saw = false;
    for (size_t k = 0; k < a.mod().processes().size(); ++k) {
        if (a.mod().processes()[k].activation != ir::Activation::Always) {
            continue;
        }
        if (a.facts.processes[k].comb_fuse == ir::CombFuse::ReadOutsideSens) {
            saw = true;
        }
    }
    CHECK(saw);
}

TEST_CASE("comb-fusion verdict accepts a complete always_comb") {
    const Analyzed a        = run(R"(
        module top;
          logic p, q, y;
          always_comb y = p ^ q;
          initial begin p = 0; q = 1; end
        endmodule
    )");
    bool           eligible = false;
    for (size_t k = 0; k < a.mod().processes().size(); ++k) {
        const ir::Process& proc = a.mod().processes()[k];
        if (proc.activation != ir::Activation::Always || proc.wait_events.empty()) {
            continue;
        }
        if (a.facts.processes[k].comb_fuse == ir::CombFuse::Eligible) {
            eligible = true;
            CHECK(a.facts.processes[k].simple_always);
        }
    }
    CHECK(eligible);
}

TEST_CASE("an edge-sensitive process is not comb-fusable") {
    const Analyzed a        = run(R"(
        module top;
          logic clk, d, q;
          always @(posedge clk) q <= d;
          initial begin clk = 0; d = 1; end
        endmodule
    )");
    bool           saw_edge = false;
    for (size_t k = 0; k < a.mod().processes().size(); ++k) {
        if (a.facts.processes[k].comb_fuse == ir::CombFuse::EdgeSensitive) {
            saw_edge = true;
        }
    }
    CHECK(saw_edge);
}

TEST_CASE("a suspending process is not thunk-eligible") {
    const Analyzed a = run(R"(
        module top;
          logic clk, y;
          always @(clk) begin #1 y = ~clk; end
          initial begin clk = 0; #10 $finish; end
        endmodule
    )");
    for (size_t k = 0; k < a.mod().processes().size(); ++k) {
        const ir::Process& proc = a.mod().processes()[k];
        if (proc.activation != ir::Activation::Always || proc.wait_events.empty()) {
            continue;
        }
        // The body contains a #delay, so coroutine elision is impossible.
        CHECK_FALSE(a.facts.processes[k].simple_always);
        CHECK(a.facts.processes[k].comb_fuse == ir::CombFuse::NotSimpleAlways);
    }
}

TEST_CASE("ptr_arg marks the actual behind a task output argument") {
    const Analyzed a = run(R"(
        module top;
          logic [7:0] out;
          task set(output logic [7:0] o);
            o = 8'h11;
          endtask
          initial set(out);
        endmodule
    )");
    // The frontend does NOT hand `out` to the task directly: it materializes a
    // `__taskarg` temp as the actual and copies back afterwards. So the temp is
    // what the pointer ABI touches, and the temp is what must keep its z word.
    // Asserting on `out` here would be asserting on the wrong signal -- worth
    // pinning, because a synthesis or LSP consumer reading these facts has to know
    // the actual is a compiler temp rather than the user's variable.
    CHECK(a.facts.ptr_arg[a.sig("__taskarg0")]);
    CHECK_FALSE(a.facts.ptr_arg[a.sig("out")]);
    // Both end up driven: the temp by the callee's copy-out, `out` by the copy-back
    // store the frontend emits at the call site.
    CHECK_FALSE(a.facts.undriven[a.sig("__taskarg0")]);
    CHECK_FALSE(a.facts.undriven[a.sig("out")]);
}

// The InsideSet arm of the value sweep is a real operand join now that
// for_each_operand reaches its members, where it used to be a hardcoded may-x
// with "Ir::operands cannot express its member operands" as the reason.
//
// Deliberately NOT asserted through DesignFacts::may_x here: every 4-state
// signal is may-x regardless, because it holds x before its first assignment,
// and the only signals that report clean are 2-state ones where the result is
// trivially false. So a may_x test on this would pass whatever the arm did. The
// change is pinned instead by the operand-walk properties in operands_test.cpp
// (the walk must reach the selector, both members and both range endpoints) and
// by the whole suite staying green; it becomes observable when a consumer reads
// per-value x, which is S5's abstract interpreter rather than anything today.
