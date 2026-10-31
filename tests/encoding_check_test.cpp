// The 4-state encoding assertions (docs/design/kernel.md, Values). No design can
// break the encoding -- a violation is an internal error -- so the three halves are
// tested apart: the predicate over constants, the report at the function that prints
// it, and whether the build plants the check at all.

#include <doctest/doctest.h>

#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Lower.h"
#include "Subprocess.h"
#include "ir/LlvmBackend.h"
#include "ir/llvm/EncodingCheck.h"

using namespace clotho;

namespace {

// Whether a 4-bit value with these planes violates the encoding: the predicate folded
// over constants, which is all that is needed to read its answer.
bool violates(unsigned must, unsigned may, unsigned z) {
    llvm::LLVMContext ctx;
    llvm::IRBuilder<> b(ctx);
    auto*             mm = llvm::ConstantInt::get(llvm::Type::getIntNTy(ctx, 8), (may << 4) | must);
    auto*             zp = llvm::ConstantInt::get(llvm::Type::getIntNTy(ctx, 4), z);
    auto*             r  = llvm::dyn_cast<llvm::ConstantInt>(ir::backend::encoding_violated(b, mm, zp));
    REQUIRE(r != nullptr);
    return r->isOne();
}

} // namespace

// 0 = (must 0, may 0), 1 = (1, 1), X = (0, 1), Z = (0, 1) with its z bit.
TEST_CASE("the encoding predicate accepts every legal bit and rejects the rest") {
    // 4'b01XZ: bit3 0, bit2 1, bit1 X, bit0 Z.
    CHECK_FALSE(violates(/*must=*/0b0100, /*may=*/0b0111, /*z=*/0b0001));
    CHECK_FALSE(violates(0b0000, 0b1111, 0b0000)); // all X
    CHECK_FALSE(violates(0b0000, 0b1111, 0b1111)); // all Z
    CHECK_FALSE(violates(0b1111, 0b1111, 0b0000)); // all ones
    CHECK_FALSE(violates(0, 0, 0));

    // A definite 1 that is not a possible 1.
    CHECK(violates(0b0001, 0b0000, 0b0000));
    CHECK(violates(0b1010, 0b0010, 0b0000));
    // z on a bit that is 0, and on a bit that is 1.
    CHECK(violates(0b0000, 0b0000, 0b0001));
    CHECK(violates(0b0100, 0b0100, 0b0100));
    // The unknown bits are fine; one stray z beside them is not.
    CHECK(violates(0b0000, 0b0011, 0b0100));
}

// The report names the place, shows the offending source line under a caret, says
// whose bug it is, and ends the process with a nonzero status.
TEST_CASE("an encoding violation is reported with its location and ends the run") {
    const test::DesignRun run =
      test::detail::spawn_design("    q = a + b;", /*capture_err=*/true, /*env=*/{}, test::encoding_violation_flag);

    CHECK(test::design_exit_code(run) == 1);
    CHECK(run.out.empty());
    CHECK(run.err.find("internal error: a 4-state value violates the encoding invariants") != std::string::npos);
    CHECK(run.err.find("design.sv:7:12") != std::string::npos);
    CHECK(run.err.find("7 |     q = a + b;\n") != std::string::npos);
    CHECK(run.err.find("  |            ^^^\n") != std::string::npos);
    CHECK(run.err.find("this is a bug in Clotho, not in your design") != std::string::npos);
}

// Whether the generated code carries the checks follows the build: planted before
// optimization (which folds the ones it can prove away) exactly when
// CLOTHO_EXTRA_ASSERTS is defined, and never otherwise.
TEST_CASE("the encoding check is generated only in a CLOTHO_EXTRA_ASSERTS build") {
    const LowerResult r = lower_text(R"(
        module m;
          logic [7:0] a, q;
          initial begin
            $sscanf("5", "%d", a);
            q = a;
          end
        endmodule)");
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());

    auto host = ir::host_target();
    REQUIRE(static_cast<bool>(host));
    auto tm = host->createTargetMachine();
    REQUIRE(static_cast<bool>(tm));

    llvm::LLVMContext ctx;
    const auto        built = ir::build_modules_unoptimized(*r.module, ctx, **tm);
    REQUIRE_FALSE(built.modules.empty());
    std::string              text;
    llvm::raw_string_ostream os(text);
    for (const auto& m : built.modules) {
        m->print(os, nullptr);
    }
    const bool planted = text.find("rt_encoding_violation") != std::string::npos;
#ifdef CLOTHO_EXTRA_ASSERTS
    CHECK(planted);
#else
    CHECK_FALSE(planted);
#endif
}
