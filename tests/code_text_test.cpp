// What Backend::verify_shared_group compares: the printed code of two lowerings
// of one group, equal up to what legitimately differs between them (the
// function's name, the names and numbering of private constants, types and
// attribute groups) and different in anything the code does.

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "ir/llvm/CodeText.h"

using clotho::ir::backend::CodeText;

namespace {

// A lowered group, the way the backend writes one: a function of its instance's
// record that reads a field of it and a global, names a private string constant,
// and builds an aggregate it hands to a callee.
struct Group {
    std::string              name      = "clotho.inst.top.a.p0";
    unsigned                 field     = 2;
    std::string              text      = "hello";
    std::string              text_name = ""; // the constant's name; unnamed when empty
    std::string              callee    = "rt_put";
    std::string              global    = "clotho.state";
    std::vector<llvm::Type*> agg;
};

llvm::Function* lower(llvm::Module& m, Group g) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::IRBuilder<>  b(ctx);
    auto*              ptr = b.getPtrTy();
    if (g.agg.empty()) {
        g.agg = { b.getInt32Ty(), b.getInt64Ty() };
    }
    auto* rec = llvm::StructType::get(ctx, { b.getInt32Ty(), b.getInt32Ty(), b.getInt64Ty() });
    auto* fn  = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), { ptr }, false),
                                       llvm::Function::ExternalLinkage, g.name, m);
    fn->addFnAttr(llvm::Attribute::NoInline);
    fn->getArg(0)->setName("ctx");
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", fn));
    auto* field = b.CreateLoad(b.getInt32Ty(), b.CreateConstInBoundsGEP2_32(rec, fn->getArg(0), 0, g.field));
    auto* glob  = b.CreateLoad(b.getInt32Ty(), m.getOrInsertGlobal(g.global, b.getInt32Ty()));
    auto* str   = b.CreateGlobalString(g.text, g.text_name);
    auto* agg   = llvm::StructType::create(ctx, g.agg, "clotho.agg");
    auto* slot  = b.CreateAlloca(agg);
    b.CreateStore(b.CreateAdd(field, glob), b.CreateStructGEP(agg, slot, 0));
    b.CreateCall(m.getOrInsertFunction(g.callee, llvm::FunctionType::get(b.getVoidTy(), { ptr, ptr }, false)),
                 { str, slot });
    b.CreateRetVoid();
    return fn;
}

// A module that has been lowered into before, so what a group names in it is
// numbered after other constants, types and attribute groups.
void busy(llvm::Module& m) {
    llvm::IRBuilder<> b(m.getContext());
    b.CreateGlobalString("padding", "", 0, &m);
    auto* other = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), {}, false),
                                         llvm::Function::InternalLinkage, "other", m);
    other->addFnAttr(llvm::Attribute::NoUnwind);
    other->addFnAttr(llvm::Attribute::NoRecurse);
    b.SetInsertPoint(llvm::BasicBlock::Create(m.getContext(), "entry", other));
    b.CreateRetVoid();
}

bool same(llvm::Function* x, llvm::Function* y) {
    return CodeText(x).joined() == CodeText(y).joined();
}

std::string difference(llvm::Function* shared, llvm::Function* own) {
    return CodeText::difference(CodeText(shared), CodeText(own));
}

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("code text: the same group lowered for two instances compares equal") {
    llvm::LLVMContext ctx;
    llvm::Module      a("a", ctx), b("b", ctx);
    busy(b);
    llvm::Function* shared = lower(a, { .name = "clotho.inst.top.a.p0" });
    // Another function name, a string constant that is named and numbered
    // otherwise, an aggregate type named after the ones made before it, and
    // attribute groups numbered after another's: none of it is the code.
    llvm::Function* own = lower(b, { .name = "clotho.inst.top.g[1].b.p0", .text_name = ".str.7" });
    CHECK(CodeText(shared).raw != CodeText(own).raw);
    CHECK(same(shared, own));
}

TEST_CASE("code text: code that does something else compares different") {
    llvm::LLVMContext ctx;
    llvm::Module      a("a", ctx), b("b", ctx);
    llvm::Function*   shared = lower(a, {});

    SUBCASE("a record field") {
        llvm::Function* own = lower(b, { .name = "own", .field = 1 });
        CHECK_FALSE(same(shared, own));
        const std::string d = difference(shared, own);
        CHECK(has(d, "first difference at line"));
        CHECK(has(d, "i32 0, i32 2"));
        CHECK(has(d, "i32 0, i32 1"));
    }
    SUBCASE("a global of one instance") {
        llvm::Function* own = lower(b, { .name = "own", .global = "clotho.inst.top.b.state" });
        CHECK_FALSE(same(shared, own));
        const std::string d = difference(shared, own);
        CHECK(has(d, "@clotho.state"));
        CHECK(has(d, "@clotho.inst.top.b.state"));
    }
    SUBCASE("a callee") {
        CHECK_FALSE(same(shared, lower(b, { .name = "own", .callee = "rt_other" })));
    }
    SUBCASE("a constant the printed code names alike") {
        // Both name `@inst.path`; one holds `top.a`, the other `top.b`.
        llvm::Function* x = lower(a, { .name = "x", .text = "top.a", .text_name = "inst.path" });
        llvm::Module    c("c", ctx);
        llvm::Function* y = lower(c, { .name = "y", .text = "top.b", .text_name = "inst.path" });
        CHECK(CodeText(x).raw.size() == CodeText(y).raw.size());
        CHECK_FALSE(same(x, y));
        const std::string d = difference(x, y);
        CHECK(has(d, "top.a"));
        CHECK(has(d, "top.b"));
    }
    SUBCASE("a type of another shape") {
        const std::vector<llvm::Type*> two_words{ llvm::Type::getInt32Ty(ctx), llvm::Type::getInt32Ty(ctx) };
        CHECK_FALSE(same(shared, lower(b, { .name = "own", .agg = two_words })));
    }
}
