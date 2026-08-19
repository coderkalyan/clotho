#include <doctest/doctest.h>

#include <slang/text/SourceLocation.h>

#include "Lower.h"
#include "SourceContext.h"
#include "SourceMap.h"

using namespace clotho;

TEST_CASE("slang source location round-trips through the opaque handle") {
    const slang::SourceLocation original(slang::BufferID(7, ""), 123456);
    const ir::SourceLoc         handle = to_source_loc(original);
    const slang::SourceLocation back   = to_slang_location(handle);

    CHECK(back.buffer().getId() == 7);
    CHECK(back.offset() == 123456);
    CHECK(back == original);
}

TEST_CASE("source spans round-trip and report validity") {
    const slang::SourceLocation a(slang::BufferID(3, ""), 100);
    const slang::SourceLocation b(slang::BufferID(3, ""), 140);
    const ir::SourceSpan        span = to_source_span(slang::SourceRange(a, b));

    CHECK(span.valid());
    CHECK(to_slang_location(span.start) == a);
    CHECK(to_slang_location(span.end) == b);
    // A default span carries no location, which is how a compiler-synthesized
    // construct presents.
    CHECK_FALSE(ir::SourceSpan{}.valid());
}

// The point of S0: a SourceLoc minted during lowering stays decodable after
// lowering has returned. Before the SourceContext existed, the slang
// SourceManager was a stack local in each entry point, so every location in the
// IR became garbage the moment control left `lower_*`.
TEST_CASE("locations survive lowering and resolve to real positions") {
    const LowerResult r = lower_text(R"(
module m;
  logic [3:0] counter;
  initial counter = 0;
endmodule
)");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.sources != nullptr);
    REQUIRE_FALSE(r.sources->empty());

    // The declaration of `counter` is on line 3 of the fragment above.
    const ir::SignalInfo* counter = nullptr;
    for (const ir::SignalInfo& s : r.module->signals()) {
        if (s.name == "counter") {
            counter = &s;
        }
    }
    REQUIRE(counter != nullptr);

    const SourcePosition pos = r.sources->resolve(counter->loc);
    CHECK(pos.valid);
    CHECK(pos.line == 3);
    CHECK(pos.column > 0);
    CHECK(r.sources->render(counter->loc).find(":3:") != std::string::npos);

    // The `initial` block's process carries a real extent, not just a point.
    REQUIRE_FALSE(r.module->processes().empty());
    bool found_initial = false;
    for (const ir::Process& p : r.module->processes()) {
        if (p.activation != ir::Activation::Initial) {
            continue;
        }
        found_initial = true;
        CHECK(p.span.valid());
        CHECK(r.sources->resolve(p.span.start).line == 4);
        // end > start is what makes this a span rather than a bare location.
        CHECK(static_cast<uint64_t>(p.span.end) > static_cast<uint64_t>(p.span.start));
    }
    CHECK(found_initial);
}

TEST_CASE("an unresolvable location degrades instead of lying") {
    const LowerResult r = lower_text("module m; initial $finish; endmodule");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    // SourceLoc{0} is the "no location" value every synthesized construct uses.
    CHECK_FALSE(r.sources->resolve(ir::SourceLoc{ 0 }).valid);
    CHECK(r.sources->render(ir::SourceLoc{ 0 }) == "<unknown>");
    // A handle naming a buffer this context never saw must not be reported as a
    // real position -- that would be a silent wrong answer.
    const ir::SourceLoc alien = to_source_loc(slang::SourceLocation(slang::BufferID(9999, ""), 4));
    CHECK_FALSE(r.sources->resolve(alien).valid);
}

TEST_CASE("a lowering guard reports a located, coded diagnostic") {
    const LowerResult r = lower_text(R"(
module m;
  logic a;
  initial begin : blk
    a = 1;
    wait fork;
  end
endmodule
)");
    REQUIRE_FALSE(r.ok);

    // Selected by code, not by position or count: the design also draws an
    // unused-variable warning, and a test of the lowering guard's diagnostic has
    // no business asserting how many OTHER diagnostics the design produces.
    const Diagnostic* found = nullptr;
    for (const Diagnostic& item : r.diagnostics) {
        if (item.code == "unsupported-construct") {
            found = &item;
        }
    }
    REQUIRE(found != nullptr);
    const Diagnostic& d = *found;
    CHECK(d.severity == Severity::Error);
    CHECK(d.origin == DiagnosticOrigin::Unsupported);
    CHECK(d.code == "unsupported-construct");
    // The message is the bare complaint: no location baked into the text.
    CHECK(d.message.find("wait fork") != std::string::npos);
    CHECK(d.message.find(".sv") == std::string::npos);
    CHECK(d.message.find(":6:") == std::string::npos);
    // The location lives in the span instead, and points at the `disable`.
    REQUIRE(d.span.valid());
    CHECK(r.sources->resolve(d.span.start).line == 6);
    // Rendering is what re-attaches the location.
    CHECK(r.diagnostic_text().find(":6:") != std::string::npos);
    CHECK(r.diagnostic_text().find("[unsupported-construct]") != std::string::npos);
}

TEST_CASE("slang diagnostics keep their own rendering but gain structure") {
    // A width-truncating assignment: slang warns, and the design still lowers.
    const LowerResult r = lower_text(R"(
module m;
  logic [3:0] a;
  initial begin
    a = 8'hFF;
  end
endmodule
)");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE_FALSE(r.diagnostics.empty());

    const Diagnostic& d = r.diagnostics.front();
    CHECK(d.origin == DiagnosticOrigin::Frontend);
    CHECK(d.severity == Severity::Warning);
    // slang's stable option name comes through as the code.
    CHECK(d.code == "constant-conversion");
    CHECK(d.span.valid());
    // slang's pretty text (source snippet + caret) is preserved rather than
    // replaced by a plainer hand-rolled rendering.
    CHECK(d.rendered.find('^') != std::string::npos);
    CHECK(r.diagnostic_text().find('^') != std::string::npos);
    CHECK_FALSE(has_error(r.diagnostics));
}
