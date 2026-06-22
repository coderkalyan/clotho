#include <doctest/doctest.h>

#include <array>
#include <variant>

#include "ir/Builder.h"
#include "ir/InternPool.h"
#include "ir/Ir.h"

using namespace clotho::ir;

namespace {
// Intern an int constant of the built-in int type, returning its handle.
Ref int_constant(InternPool& pool, int64_t v) {
    return pool.put(Constant{ .kind = Constant::Kind::int_, .ty = pool.int_type, .int_val = v });
}
} // namespace

TEST_CASE("intern pool deduplicates items in one Ref space") {
    InternPool pool;

    const Ref int_ty = pool.put(Type{ .kind = Type::Kind::int_ });
    const Ref flt_ty = pool.put(Type{ .kind = Type::Kind::float_ });

    const Ref a = pool.put(Constant{ .kind = Constant::Kind::int_, .ty = int_ty, .int_val = 42 });
    const Ref b = pool.put(Constant{ .kind = Constant::Kind::int_, .ty = int_ty, .int_val = 42 });
    const Ref c = pool.put(Constant{ .kind = Constant::Kind::int_, .ty = int_ty, .int_val = 7 });

    CHECK(a == b); // identical items dedup
    CHECK(a != c);
    CHECK(int_ty != flt_ty);
    CHECK(int_ty != a); // types and constants share the space but never alias

    CHECK(std::get<Constant>(pool.get(a)).int_val == 42);
    CHECK(std::get<Constant>(pool.get(a)).ty == int_ty);

    // Structural pointer types dedup on their element.
    const Ref ptr_int = pool.put(Type{ .kind = Type::Kind::ptr, .elem = int_ty });
    CHECK(ptr_int == pool.put(Type{ .kind = Type::Kind::ptr, .elem = int_ty }));
    CHECK(ptr_int != pool.put(Type{ .kind = Type::Kind::ptr, .elem = flt_ty }));
}

TEST_CASE("extra side-table round-trips via bit_cast") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };
    // Build a call so an ExtraSlice gets written, then decode it back.
    const Index                callee = b.constant(int_constant(pool, 0), loc);
    const Index                x      = b.constant(int_constant(pool, 1), loc);
    const std::array<Index, 1> args{ x };
    const Index                c  = b.call(callee, args, loc);
    Ir                         ir = b.finish();

    const ExtraSlice slice = ir.extraData<ExtraSlice>(ir.payload(c).unary_extra.extra);
    const auto       words = ir.indexSlice(slice);
    REQUIRE(words.size() == 1);
    CHECK(words[0] == x);
}

// Build a flat body:  t = add(c0, c1);  ret t
TEST_CASE("build a flat instruction list and query it") {
    InternPool pool;
    Builder    b(pool);

    const SourceLoc loc{ 1 };
    const Index     c0  = b.constant(int_constant(pool, 2), loc);
    const Index     c1  = b.constant(int_constant(pool, 3), loc);
    const Index     sum = b.add(c0, c1, static_cast<SourceLoc>(99));
    b.ret(sum, loc);

    Ir ir = b.finish();
    REQUIRE(ir.instCount() == 4);
    CHECK(ir.tag(c0) == Inst::Tag::constant);
    CHECK(ir.tag(sum) == Inst::Tag::add);
    CHECK(ir.tag(Index{ 3 }) == Inst::Tag::ret);

    // Source locations are stored per instruction and read back verbatim.
    CHECK(ir.loc(sum) == static_cast<SourceLoc>(99));

    std::array<Index, 2> scratch{};

    const auto add_ops = ir.operands(sum, scratch);
    REQUIRE(add_ops.size() == 2);
    CHECK(add_ops[0] == c0);
    CHECK(add_ops[1] == c1);

    CHECK(ir.operands(c0, scratch).empty()); // constant has no operands

    const auto ret_ops = ir.operands(Index{ 3 }, scratch);
    REQUIRE(ret_ops.size() == 1);
    CHECK(ret_ops[0] == sum);
}

TEST_CASE("call exposes variadic args as operands") {
    InternPool pool;
    Builder    b(pool);

    const SourceLoc            loc{ 1 };
    const Index                callee = b.constant(int_constant(pool, 0), loc);
    const Index                a0     = b.constant(int_constant(pool, 1), loc);
    const Index                a1     = b.constant(int_constant(pool, 2), loc);
    const std::array<Index, 2> args{ a0, a1 };
    const Index                c = b.call(callee, args, loc);
    b.ret(c, loc);

    Ir                   ir = b.finish();
    std::array<Index, 2> scratch{};
    const auto           ops = ir.operands(c, scratch); // variadic: view into extra
    REQUIRE(ops.size() == 2);
    CHECK(ops[0] == a0);
    CHECK(ops[1] == a1);
}
