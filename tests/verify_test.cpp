#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ir/Builder.h"
#include "ir/InternPool.h"
#include "ir/Ir.h"
#include "ir/Module.h"
#include "ir/Typing.h"
#include "ir/Verify.h"

using namespace clotho::ir;

// The IR verifier on small hand-built modules: for each rule, one body that
// satisfies it and one that breaks it. Built by hand because the frontend does
// not (and must not be relied on to) emit the violating shapes on purpose.
namespace {

constexpr SourceLoc kLoc{ 1 };

Ref vec(InternPool& pool, bool four, uint32_t width, bool is_signed) {
    return pool.put(Type{ .kind      = Type::Kind::PackedArray,
                          .is_signed = is_signed,
                          .element   = four ? pool.logic_type : pool.bit_type,
                          .left      = width - 1,
                          .right     = 0 });
}

// A module with one initial process under construction.
struct Body {
    Module  m;
    Builder b{ m };

    Ref bits(uint32_t width, bool is_signed = false) {
        return vec(m.pool, false, width, is_signed);
    }
    Ref logic(uint32_t width, bool is_signed = false) {
        return vec(m.pool, true, width, is_signed);
    }
    Index konst(Ref type, uint64_t v = 1) {
        return b.constant(m.pool.put_int(type, v), kLoc);
    }
    Index signal(Ref type) {
        return b.signal(m.add_signal({ .name = "s", .type = type }), kLoc);
    }

    std::vector<Violation> verified() {
        m.add_process(Process{ .activation = Activation::Initial, .ir = b.finish() });
        return verify(m);
    }
};

size_t count(const std::vector<Violation>& v, Rule rule) {
    size_t n = 0;
    for (const Violation& x : v) {
        n += x.rule == rule ? 1 : 0;
    }
    return n;
}

// Exactly one violation, of `rule`.
void only(const std::vector<Violation>& v, Rule rule) {
    REQUIRE(v.size() == 1);
    CHECK(std::string(rule_name(v[0].rule)) == rule_name(rule));
}

// A function `f(formal) -> ret` whose body is built by `fill` over its own builder.
template<typename Fill>
FunctionId add_function(Module& m, Ref ret, std::vector<Ref> formals, Fill fill) {
    const FunctionId id = m.reserve_function(ret);
    Builder          fb(m);
    Function         f;
    f.name        = "f";
    f.return_type = ret;
    f.param_types = std::move(formals);
    fill(fb);
    f.ir = fb.finish();
    m.set_function(id, std::move(f));
    return id;
}

} // namespace

TEST_CASE("verify: a well-typed binary operator is clean") {
    Body      t;
    const Ref u8 = t.bits(8);
    t.b.binary(Inst::Tag::Add, t.konst(u8), t.konst(u8), kLoc);
    t.b.binary(Inst::Tag::ULt, t.konst(u8), t.konst(u8), kLoc);
    t.b.binary(Inst::Tag::CaseEq, t.konst(u8), t.konst(u8), kLoc);
    CHECK(t.verified().empty());
}

TEST_CASE("verify: operand width") {
    Body t;
    t.b.binary(Inst::Tag::Sub, t.konst(t.bits(8)), t.konst(t.bits(4)), kLoc);
    const auto v = t.verified();
    only(v, Rule::OperandWidth);
    CHECK(v[0].tag == Inst::Tag::Sub);
    CHECK(v[0].types[0].width == 8);
    CHECK(v[0].types[1].width == 4);
    CHECK(v[0].loc == kLoc);
}

TEST_CASE("verify: operand width on a case-equality, which the backend would pad") {
    Body t;
    t.b.binary(Inst::Tag::WildEq, t.konst(t.logic(8)), t.konst(t.logic(4)), kLoc);
    only(t.verified(), Rule::OperandWidth);
}

TEST_CASE("verify: operand domain") {
    Body t;
    t.b.binary(Inst::Tag::Band, t.konst(t.logic(8)), t.konst(t.bits(8)), kLoc);
    only(t.verified(), Rule::OperandDomain);
}

TEST_CASE("verify: operand signedness of ordered compares, div and mod; equality is exempt") {
    Body      t;
    const Ref s8 = t.bits(8, true);
    const Ref u8 = t.bits(8);
    t.b.binary(Inst::Tag::SLt, t.konst(s8), t.konst(u8), kLoc);
    t.b.binary(Inst::Tag::SMod, t.konst(t.logic(8, true)), t.konst(t.logic(8)), kLoc);
    t.b.binary(Inst::Tag::Eq, t.konst(s8), t.konst(u8), kLoc);
    const auto v = t.verified();
    CHECK(count(v, Rule::OperandSign) == 2);
    CHECK(v.size() == 2);
}

TEST_CASE("verify: the result of an add is the left operand's signedness") {
    Body t;
    t.b.binary(Inst::Tag::Add, t.konst(t.bits(8, true)), t.konst(t.bits(8)), kLoc);
    only(t.verified(), Rule::ResultSign);
}

TEST_CASE("verify: div, mod and pow are 4-state in the LRM") {
    Body      t;
    const Ref u8 = t.bits(8);
    t.b.binary(Inst::Tag::UDiv, t.konst(u8), t.konst(u8), kLoc);
    t.b.pow(t.konst(u8), t.konst(t.bits(3)), false, false, kLoc);
    const auto v = t.verified();
    CHECK(count(v, Rule::ResultDomain) == 2);
    CHECK(v.size() == 2);
    CHECK(is_record(Rule::ResultDomain));

    Body      ok;
    const Ref l8 = ok.logic(8);
    ok.b.binary(Inst::Tag::UDiv, ok.konst(l8), ok.konst(l8), kLoc);
    ok.b.pow(ok.konst(l8), ok.konst(ok.bits(3)), false, false, kLoc);
    CHECK(ok.verified().empty());
}

TEST_CASE("verify: an integer operator rejects a real operand") {
    Body t;
    t.b.binary(
      Inst::Tag::Add, t.b.constant(t.m.pool.put_real(t.m.pool.real_type, 1.0), kLoc), t.konst(t.bits(64)), kLoc);
    only(t.verified(), Rule::OperandKind);
}

TEST_CASE("verify: a shift amount is self-determined but cannot widen the domain") {
    Body ok;
    // Any width, sign and (when the operand is 4-state) domain of amount.
    ok.b.binary(Inst::Tag::Sll, ok.konst(ok.bits(8)), ok.konst(ok.bits(3)), kLoc);
    ok.b.binary(Inst::Tag::Sra, ok.konst(ok.logic(8, true)), ok.konst(ok.logic(5, true)), kLoc);
    CHECK(ok.verified().empty());

    Body t;
    t.b.binary(Inst::Tag::Srl, t.konst(t.bits(8)), t.konst(t.logic(3)), kLoc);
    only(t.verified(), Rule::ShiftResult);
}

TEST_CASE("verify: select arms") {
    Body      ok;
    const Ref u8 = ok.bits(8);
    ok.b.select(ok.konst(ok.m.pool.bit_type), ok.konst(u8), ok.konst(u8), kLoc);
    CHECK(ok.verified().empty());

    Body        t;
    const Index c = t.konst(t.m.pool.bit_type);
    t.b.select(c, t.konst(t.bits(8)), t.konst(t.bits(4)), kLoc);
    t.b.select(c, t.konst(t.bits(8)), t.konst(t.logic(8)), kLoc);
    t.b.select(c, t.konst(t.bits(8)), t.konst(t.bits(8, true)), kLoc);
    const auto v = t.verified();
    CHECK(count(v, Rule::SelectArmWidth) == 1);
    CHECK(count(v, Rule::SelectArmDomain) == 1);
    CHECK(count(v, Rule::SelectArmSign) == 1);
    CHECK(v.size() == 3);
}

TEST_CASE("verify: a lazy select's arms are checked like an eager one's") {
    Body        t;
    const Index pred = t.konst(t.m.pool.bit_type);
    t.b.open_region();
    const Index tv = t.konst(t.logic(8));
    const auto  tr = t.b.close_region();
    t.b.open_region();
    const Index ev = t.konst(t.bits(8));
    const auto  er = t.b.close_region();
    t.b.select_lazy(pred, tr, tv, er, ev, kLoc);
    const auto v = t.verified();
    only(v, Rule::SelectArmDomain);
    CHECK(v[0].tag == Inst::Tag::SelectLazy);
}

TEST_CASE("verify: predicates are one bit") {
    Body t;
    t.b.open_region();
    const auto r = t.b.close_region();
    t.b.cond(t.konst(t.bits(8)), r, r, kLoc);
    only(t.verified(), Rule::PredicateWidth);

    Body ok;
    ok.b.open_region();
    const auto r2 = ok.b.close_region();
    ok.b.cond(ok.konst(ok.m.pool.logic_type), r2, r2, kLoc);
    CHECK(ok.verified().empty());
}

TEST_CASE("verify: store width and domain are separate records") {
    Body      ok;
    const Ref u8 = ok.bits(8);
    ok.b.binary(Inst::Tag::Store, ok.signal(u8), ok.konst(u8), kLoc);
    CHECK(ok.verified().empty());

    Body w;
    w.b.binary(Inst::Tag::StoreNb, w.signal(w.bits(8)), w.konst(w.bits(4)), kLoc);
    only(w.verified(), Rule::StoreWidth);

    Body d;
    d.b.binary(Inst::Tag::Store, d.signal(d.bits(8)), d.konst(d.logic(8)), kLoc);
    const auto v = d.verified();
    only(v, Rule::StoreDomain);
    CHECK(is_record(v[0].rule));
}

TEST_CASE("verify: a store to a part select compares against the select's width") {
    Body        t;
    const Index base = t.signal(t.bits(8));
    const Index part = t.b.part_select(base, 3, 0, kLoc);
    t.b.binary(Inst::Tag::Store, part, t.konst(t.bits(4)), kLoc);
    CHECK(t.verified().empty());

    Body        bad;
    const Index base2 = bad.signal(bad.bits(8));
    const Index part2 = bad.b.part_select(base2, 3, 0, kLoc);
    bad.b.binary(Inst::Tag::Store, part2, bad.konst(bad.bits(8)), kLoc);
    only(bad.verified(), Rule::StoreWidth);
}

TEST_CASE("verify: resize records a signedness change, split by what it does to the width") {
    Body      t;
    const Ref u4 = t.bits(4);
    const Ref s8 = t.bits(8, true);
    const Ref s4 = t.bits(4, true);
    // Same signedness: nothing to record.
    t.b.resize(t.konst(u4), t.bits(8), kLoc);
    t.b.resize(t.konst(s8), s4, kLoc);
    // Signedness differs.
    t.b.resize(t.konst(u4), s8, kLoc);        // widen
    t.b.resize(t.konst(s8), t.bits(4), kLoc); // narrow
    t.b.resize(t.konst(u4), s4, kLoc);        // retag
    const auto v = t.verified();
    CHECK(count(v, Rule::ResizeSignWiden) == 1);
    CHECK(count(v, Rule::ResizeSignNarrow) == 1);
    CHECK(count(v, Rule::ResizeSignRetag) == 1);
    CHECK(v.size() == 3);
    for (const Violation& x : v) {
        CHECK(is_record(x.rule));
    }
}

TEST_CASE("verify: call actuals against formals") {
    Body             t;
    const Ref        u8 = t.bits(8);
    const FunctionId f =
      add_function(t.m, u8, { u8 }, [&](Builder& fb) { fb.unary(Inst::Tag::Ret, fb.arg(0, u8, kLoc), kLoc); });

    const auto call = [&](Index actual) {
        const std::array<Index, 1> args{ actual };
        t.b.call(f, args, kLoc);
    };
    call(t.konst(u8));
    CHECK(t.verified().empty());

    Body             w;
    const Ref        w8 = w.bits(8);
    const FunctionId fw =
      add_function(w.m, w8, { w8 }, [&](Builder& fb) { fb.unary(Inst::Tag::Ret, fb.arg(0, w8, kLoc), kLoc); });
    const std::array<Index, 1> narrow{ w.konst(w.bits(4)) };
    const std::array<Index, 1> four{ w.konst(w.logic(8)) };
    w.b.call(fw, narrow, kLoc);
    w.b.call(fw, four, kLoc);
    const auto v = w.verified();
    CHECK(count(v, Rule::CallArgWidth) == 1);
    CHECK(count(v, Rule::CallArgDomain) == 1);
    CHECK(v.size() == 2);
}

TEST_CASE("verify: a call with the wrong number of actuals") {
    Body             t;
    const Ref        u8 = t.bits(8);
    const FunctionId f =
      add_function(t.m, u8, { u8 }, [&](Builder& fb) { fb.unary(Inst::Tag::Ret, fb.arg(0, u8, kLoc), kLoc); });
    t.b.call(f, {}, kLoc);
    const auto v = t.verified();
    only(v, Rule::CallArity);
    CHECK(v[0].types[0].width == 0);
    CHECK(v[0].types[1].width == 1);
}

TEST_CASE("verify: an Arg and a Ret against the function's signature") {
    Body      t;
    const Ref u8 = t.bits(8);
    add_function(t.m, u8, { u8 }, [&](Builder& fb) {
        // The formal is bit [7:0]; the body reads it as logic [7:0] and returns logic [3:0].
        fb.arg(0, t.logic(8), kLoc);
        fb.unary(Inst::Tag::Ret, fb.constant(t.m.pool.put_int(t.logic(4), 0), kLoc), kLoc);
    });
    const auto v = t.verified();
    CHECK(count(v, Rule::ArgType) == 1);
    CHECK(count(v, Rule::RetWidth) == 1);
    CHECK(count(v, Rule::RetDomain) == 1);
    CHECK(v.size() == 3);
    for (const Violation& x : v) {
        CHECK(x.site.kind == Site::Kind::Function);
    }
}

TEST_CASE("verify: the typing is the backend's: a signal reports its slot's leaf") {
    Body t;
    // A 4-state memory element: the value type is the element's, not the array's.
    const Ref elem = t.logic(8, true);
    const Ref mem  = t.m.pool.put(Type{ .kind = Type::Kind::UnpackedArray, .element = elem, .left = 0, .right = 3 });
    t.signal(mem);
    const Ir   ir    = t.b.finish();
    const auto types = value_types(t.m, ir);
    REQUIRE(types.size() == 1);
    CHECK(types[0].width == 8);
    CHECK(types[0].four_state);
    // The array's own signedness, which an unpacked array never carries.
    CHECK(!types[0].is_signed);
}

TEST_CASE("verify: a violation formats with its rule, site, instruction, tag and types") {
    Body t;
    t.b.binary(Inst::Tag::Add, t.konst(t.bits(8)), t.konst(t.logic(8, true)), kLoc);
    const auto v = t.verified();
    REQUIRE(v.size() == 2);
    const auto at = [](SourceSpan) { return SourceText{ .where = "x.sv:3:4" }; };
    CHECK(format(t.m, v[0], at) == "ir-verify: operand-domain: process 0 %2 add(bit[8], signed logic[8]) at x.sv:3:4");
    CHECK(format(t.m, v[0]).find(" at ") == std::string::npos);
}

TEST_CASE("verify: a select over real arms is not an arm-sign violation") {
    // A real signal's slot is not flagged signed while a real constant is.
    Body        t;
    const Ref   real = t.m.pool.real_type;
    const Index sig  = t.signal(real);
    const Index load = t.b.unary(Inst::Tag::Load, sig, kLoc);
    const Index zero = t.b.constant(t.m.pool.put_real(real, 0.0), kLoc);
    t.b.select(t.konst(t.m.pool.bit_type), load, zero, kLoc);
    CHECK(t.verified().empty());
}

TEST_CASE("verify: a DPI open-array formal is a phantom slot, not a value to compare") {
    Body             t;
    const Ref        i32 = t.bits(32, true);
    const FunctionId f   = t.m.reserve_function(i32);
    Function         g;
    g.name           = "oa";
    g.return_type    = i32;
    g.param_types    = { i32 };
    g.is_dpi         = true;
    g.dpi_arg_ctypes = { DpiCType::OpenArray };
    t.m.set_function(f, std::move(g));
    // The actual is the array lvalue itself: a dynamic array's slot reports 1 bit.
    const Ref                  dyn = t.m.pool.put(Type{ .kind = Type::Kind::DynArray, .element = i32 });
    const std::array<Index, 1> args{ t.signal(dyn) };
    t.b.call(f, args, kLoc);
    CHECK(t.verified().empty());
}

// A body still being built is typed by the function that types a finished one,
// instruction by instruction, so the frontend sees the types the backend will.
TEST_CASE("typing: Builder::type_of is value_types of the body so far") {
    Body        t;
    const Ref   u8   = t.bits(8);
    const Ref   l4   = t.logic(4);
    const Index a    = t.konst(u8, 3);
    const Index sum  = t.b.binary(Inst::Tag::Add, a, a, kLoc);
    const Index four = t.konst(l4, 2);
    // A concat decodes its operand run from the extra table the builder is still
    // appending to.
    const std::array<Index, 2> parts{ sum, four };
    const Index                cat = t.b.concat(parts, kLoc);

    CHECK(t.b.type_of(a) == ValueType{ 8, false, false });
    CHECK(t.b.type_of(sum) == ValueType{ 8, false, false });
    CHECK(t.b.type_of(cat) == ValueType{ 12, true, false });

    // What the builder said is what the finished body is typed as.
    const std::vector<ValueType> seen{ t.b.type_of(a), t.b.type_of(sum), t.b.type_of(four), t.b.type_of(cat) };
    const Ir                     ir    = t.b.finish();
    const std::vector<ValueType> after = value_types(t.m, ir);
    REQUIRE(after.size() == seen.size());
    for (size_t k = 0; k < seen.size(); ++k) {
        CHECK(after[k] == seen[k]);
    }
}

TEST_CASE("typing: a call to a function reserved but not yet built has its declared return type") {
    Body             t;
    const Ref        i16 = t.bits(16, true);
    const FunctionId f   = t.m.reserve_function(i16);
    const Index      c   = t.b.call(f, {}, kLoc);
    CHECK(t.b.type_of(c) == ValueType{ 16, false, true });
}

TEST_CASE("typing: rolling back the builder drops the types of the discarded instructions") {
    Body                t;
    const Builder::Mark mark = t.b.mark();
    const Index         c1   = t.konst(t.bits(8), 1);
    CHECK(t.b.type_of(c1) == ValueType{ 8, false, false });
    t.b.rollback(mark);
    // The next instruction reuses c1's index with a different type.
    const Index c2 = t.konst(t.logic(8), 1);
    CHECK(c2 == c1);
    CHECK(t.b.type_of(c2) == ValueType{ 8, true, false });
}

// The opcode names the signedness it reads its operands with; while the IR's types
// still carry one, the frontend's choice has to agree with them.
TEST_CASE("verify: an opcode's signedness agrees with its operands' types") {
    Body      t;
    const Ref s8 = t.logic(8, true);
    const Ref u8 = t.logic(8);
    t.b.binary(Inst::Tag::SLt, t.konst(s8), t.konst(s8), kLoc);
    t.b.binary(Inst::Tag::UGe, t.konst(u8), t.konst(u8), kLoc);
    t.b.binary(Inst::Tag::SDiv, t.konst(s8), t.konst(s8), kLoc);
    t.b.binary(Inst::Tag::Sra, t.konst(s8), t.konst(u8), kLoc);
    t.b.pow(t.konst(s8), t.konst(u8), /*base_signed=*/true, /*exp_signed=*/false, kLoc);
    const std::array<Index, 2> range{ t.konst(s8), t.konst(s8) };
    t.b.inside_set(t.konst(s8), {}, range, /*ranges_signed=*/true, kLoc);
    t.b.float_cvt(Inst::Tag::UItoF, t.konst(u8), t.m.pool.real_type, kLoc);
    CHECK(t.verified().empty());
}

TEST_CASE("verify: an opcode that reads its operands with the wrong signedness is reported") {
    Body      t;
    const Ref s8 = t.logic(8, true);
    const Ref u8 = t.logic(8);
    t.b.binary(Inst::Tag::ULt, t.konst(s8), t.konst(s8), kLoc);
    t.b.binary(Inst::Tag::SMod, t.konst(u8), t.konst(u8), kLoc);
    t.b.binary(Inst::Tag::Sra, t.konst(u8), t.konst(u8), kLoc);
    t.b.pow(t.konst(u8), t.konst(s8), /*base_signed=*/true, /*exp_signed=*/true, kLoc);
    const std::array<Index, 2> range{ t.konst(u8), t.konst(u8) };
    t.b.inside_set(t.konst(u8), {}, range, /*ranges_signed=*/true, kLoc);
    t.b.float_cvt(Inst::Tag::SItoF, t.konst(u8), t.m.pool.real_type, kLoc);
    const auto v = t.verified();
    // One per instruction: pow's base is the unsigned one, its exponent agrees.
    CHECK(count(v, Rule::OpSign) == 6);
    CHECK(v.size() == 6);
}
