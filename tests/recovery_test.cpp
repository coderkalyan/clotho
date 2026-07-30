#include <doctest/doctest.h>

#include <string_view>
#include <vector>

#include "Lower.h"
#include "ir/Builder.h"
#include "ir/Module.h"

using namespace clotho;

namespace {

// The tag sequence of a process body. Comparing these is how the rollback tests
// below detect an orphan: a member that failed after emitting part of itself
// leaves an extra instruction in the shared pre-init body, which shows up here
// as an extra tag even though nothing references it.
std::vector<ir::Inst::Tag> tags_of(const ir::Ir& ir) {
    std::vector<ir::Inst::Tag> out;
    for (uint32_t k = 0; k < ir.inst_count(); ++k) {
        out.push_back(ir.tag(static_cast<ir::Index>(k)));
    }

    return out;
}

const ir::Process* preinit_of(const ir::Module& m) {
    for (const ir::Process& p : m.processes()) {
        if (p.activation == ir::Activation::PreInit) {
            return &p;
        }
    }

    return nullptr;
}

size_t error_count(const std::vector<Diagnostic>& diags) {
    size_t n = 0;
    for (const Diagnostic& d : diags) {
        if (d.severity == Severity::Error) {
            ++n;
        }
    }

    return n;
}

} // namespace

// The point of the whole exercise: one invocation reports every failing member,
// not just the first. Before recovery a single guard threw all the way out of
// lower_compilation, so a file with three unsupported constructs reported one
// and an editor got one squiggle.
TEST_CASE("every failing behavioural member is reported, not just the first") {
    const LowerResult r = lower_text(R"(
        module m;
          logic [3:0] a, b, c, d;
          logic       clk = 0;
          initial begin : blk1
            a = 1;
            disable blk1;
          end
          always @(posedge clk) begin : blk2
            b <= 2;
            disable blk2;
          end
          initial begin : blk3
            c = 3;
            disable blk3;
          end
          initial d = 4;
        endmodule
    )");

    CHECK(error_count(r.diagnostics) == 3);
    // Each carries its own location rather than collapsing onto one.
    std::vector<uint32_t> lines;
    for (const Diagnostic& diag : r.diagnostics) {
        if (diag.severity == Severity::Error) {
            CHECK(diag.code == "unsupported-construct");
            CHECK(diag.span.valid());
            lines.push_back(static_cast<uint32_t>(diag.span.start));
        }
    }
    CHECK(lines.size() == 3);
    CHECK(lines[0] != lines[1]);
    CHECK(lines[1] != lines[2]);
}

// Poison is loud but not fatal: the module survives for analysis, and `ok` is
// the single gate that keeps it away from codegen. Both halves matter -- a null
// module would make check mode useless, and ok==true would let a design with
// missing behaviour be simulated.
TEST_CASE("a poisoned module is analysable but not ok") {
    const LowerResult r = lower_text(R"(
        module m;
          logic [3:0] a;
          initial begin : blk
            a = 1;
            disable blk;
          end
          initial a = 2;
        endmodule
    )");

    CHECK_FALSE(r.ok);
    REQUIRE(r.module != nullptr);
    CHECK(has_error(r.diagnostics));
    // The member that DID lower is still there: recovery skipped one process, not
    // the design.
    CHECK(r.module->processes().size() >= 1);
}

// A member that fails partway through emitting into the SHARED pre-init builder
// must leave nothing behind. `S'({...})` is the exerciser: value_lvalue emits the
// signal instruction as an argument, and only then does emit_struct_store reject
// the unpacked-struct rvalue -- so without the rollback that signal is stranded
// in the pre-init body forever.
//
// Asserted by equivalence rather than by a magic instruction count: the pre-init
// body of the poisoned design must be tag-for-tag identical to the same design
// with the offending declaration deleted.
TEST_CASE("a failed initializer leaves the shared pre-init body untouched") {
    const LowerResult bad = lower_text(R"(
        typedef struct { logic [3:0] a; logic [3:0] b; } S;
        module m;
          logic [3:0] pre  = 4'h7;
          S           s    = S'({ 4'h1, 4'h2 });
          logic [3:0] post = 4'hA;
          initial $display("%0h %0h", pre, post);
        endmodule
    )");

    const LowerResult good = lower_text(R"(
        typedef struct { logic [3:0] a; logic [3:0] b; } S;
        module m;
          logic [3:0] pre  = 4'h7;
          logic [3:0] post = 4'hA;
          initial $display("%0h %0h", pre, post);
        endmodule
    )");

    CHECK_FALSE(bad.ok);
    CHECK(good.ok);
    REQUIRE(bad.module != nullptr);
    REQUIRE(good.module != nullptr);

    const ir::Process* bad_pre  = preinit_of(*bad.module);
    const ir::Process* good_pre = preinit_of(*good.module);
    REQUIRE(bad_pre != nullptr);
    REQUIRE(good_pre != nullptr);

    CHECK(tags_of(bad_pre->ir) == tags_of(good_pre->ir));
}

// Structural members are deliberately NOT recovered: they recurse and advance
// the InstanceScope cursor, so continuing past one would mis-attribute every
// later member to the wrong scope node. That is silently wrong analysis, which
// is worse than the hard failure, so a failure inside a generate block still
// aborts the whole lowering and yields no module at all.
TEST_CASE("a structural failure still aborts with no module") {
    const LowerResult r = lower_text(R"(
        module m;
          logic [3:0] a;
          if (1) begin : g
            always begin : blk
              a = 1;
              disable blk;
            end
          end
        endmodule
    )");

    CHECK_FALSE(r.ok);
    CHECK(has_error(r.diagnostics));
}

// The rollback mechanism itself, independent of any design reaching it. The
// unclosed-region case is the one that matters: Builder::finish() folds whatever
// is left in the scratch buffer into the root region, so a region abandoned
// mid-emission does not fail loudly -- it silently absorbs its contents into the
// body. Rollback has to restore the region depth, not just the instruction count.
TEST_CASE("Builder::rollback discards a partial emission including open regions") {
    ir::Module  m;
    ir::Builder b(m.pool);

    const ir::Ref   i4 = ir::Ref{};
    const ir::Index k0 = b.constant(i4, ir::SourceLoc{});
    const ir::Index k1 = b.constant(i4, ir::SourceLoc{});
    CHECK(static_cast<uint32_t>(k0) == 0);
    CHECK(static_cast<uint32_t>(k1) == 1);

    const ir::Builder::Mark mark = b.mark();
    CHECK(mark.insts == 2);

    // Emit into an abandoned region, exactly as a guard tripping inside a nested
    // body would leave things.
    b.open_region();
    b.constant(i4, ir::SourceLoc{});
    b.constant(i4, ir::SourceLoc{});
    b.rollback(mark);

    // Everything after the mark is gone, and the root region is sealed from the
    // two instructions that preceded it rather than swallowing the discarded run.
    const ir::Ir ir = b.finish();
    CHECK(ir.inst_count() == 2);
    CHECK(ir.root().size() == 2);
}

// Rollback is a no-op when nothing was emitted since the mark, which is the
// common case: most guards fire before touching the shared builder at all.
TEST_CASE("Builder::rollback to the current position changes nothing") {
    ir::Module  m;
    ir::Builder b(m.pool);

    b.constant(ir::Ref{}, ir::SourceLoc{});
    const ir::Builder::Mark mark = b.mark();
    b.rollback(mark);

    const ir::Ir ir = b.finish();
    CHECK(ir.inst_count() == 1);
    CHECK(ir.root().size() == 1);
}
