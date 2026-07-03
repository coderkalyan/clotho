#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ir/Builder.h"
#include "ir/InternPool.h"
#include "ir/Ir.h"
#include "ir/Printer.h"

using namespace clotho::ir;

namespace {
// A 1-bit known constant value.
Ref konst(InternPool& pool, uint64_t v) {
    return pool.put_int(pool.bit_type, v);
}
} // namespace

TEST_CASE("intern pool deduplicates types in one Ref space") {
    InternPool pool;

    CHECK(pool.void_type != pool.logic_type);
    CHECK(pool.logic_type != pool.bit_type);

    // logic [7:0] — packed array of logic.
    const auto vec = [&](bool is_signed) {
        return pool.put(Type{
          .kind = Type::Kind::PackedArray, .is_signed = is_signed, .element = pool.logic_type, .left = 7, .right = 0 });
    };
    CHECK(vec(false) == vec(false)); // structurally identical -> same Ref
    CHECK(vec(false) != vec(true));  // signedness distinguishes

    // logic [7:0] mem [0:255] — unpacked array (memory) of the vector.
    const Ref mem = pool.put(Type{ .kind = Type::Kind::UnpackedArray, .element = vec(false), .left = 0, .right = 255 });
    const Type& mem_t = std::get<Type>(pool.get(mem));
    CHECK(mem_t.kind == Type::Kind::UnpackedArray);
    CHECK(mem_t.element == vec(false));
    CHECK(std::get<Type>(pool.get(vec(false))).left == 7);
}

TEST_CASE("interned values round-trip in canonical planes") {
    InternPool pool;

    // logic [7:0]: 4-state, width 8, val + xz planes of 1 byte each.
    const Ref byte8 =
      pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.logic_type, .left = 7, .right = 0 });
    const Layout lay = pool.layout(byte8);
    CHECK(lay.domain == Domain::Four);
    CHECK(lay.width == 8);
    CHECK(lay.byte_count == 2);

    const Ref a = pool.put_int(byte8, 0xA5);
    const Ref b = pool.put_int(byte8, 0xA5);
    const Ref c = pool.put_int(byte8, 0x5A);
    CHECK(a == b); // identical values dedup
    CHECK(a != c);

    const auto av = pool.bytes(a);
    REQUIRE(av.size() == 2);
    CHECK(av[0] == 0xA5); // val plane
    CHECK(av[1] == 0x00); // xz plane: all bits known

    // bit (2-state) stores a single plane.
    CHECK(pool.bytes(pool.put_int(pool.bit_type, 1)).size() == 1);

    // logic [127:0]: 32 bytes (> inline) -> spills, and still dedups.
    const Ref wide =
      pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.logic_type, .left = 127, .right = 0 });
    CHECK(pool.layout(wide).byte_count == 32);
    std::vector<uint8_t> raw(32, 0);
    raw[0]            = 0xFF;
    const Ref w       = pool.put_value(wide, raw);
    const Ref w_again = pool.put_value(wide, raw);
    CHECK(w == w_again); // spilled values dedup too
    REQUIRE(pool.bytes(w).size() == 32);
    CHECK(pool.bytes(w)[0] == 0xFF);
}

// Combinational body:  y = a & b;
TEST_CASE("build a combinational process and query it") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };

    const Index a  = b.signal(Signal{ 0 }, loc);
    const Index bs = b.signal(Signal{ 1 }, loc);
    const Index y  = b.signal(Signal{ 2 }, loc);
    const Index va = b.unary(Inst::Tag::Load, a, loc);
    const Index vb = b.unary(Inst::Tag::Load, bs, loc);
    const Index r  = b.binary(Inst::Tag::Band, va, vb, loc);
    b.binary(Inst::Tag::Store, y, r, loc);

    Ir ir = b.finish();
    REQUIRE(ir.inst_count() == 7);
    REQUIRE(ir.root().size() == 7); // everything is top-level here

    CHECK(ir.tag(a) == Inst::Tag::Signal);
    CHECK(ir.tag(va) == Inst::Tag::Load);
    CHECK(ir.tag(r) == Inst::Tag::Band);

    std::array<Index, 3> scratch{};
    CHECK(ir.operands(a, scratch).empty()); // signal is a leaf

    const auto load_ops = ir.operands(va, scratch);
    REQUIRE(load_ops.size() == 1);
    CHECK(load_ops[0] == a);

    const auto and_ops = ir.operands(r, scratch);
    REQUIRE(and_ops.size() == 2);
    CHECK(and_ops[0] == va);
    CHECK(and_ops[1] == vb);
}

// Sequential body:  if (c) q <= 1; else q <= 0;
TEST_CASE("structured cond carries then/else regions") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };

    const Index c  = b.signal(Signal{ 0 }, loc);
    const Index vc = b.unary(Inst::Tag::Load, c, loc);
    const Index q  = b.signal(Signal{ 1 }, loc);

    b.open_region();
    b.binary(Inst::Tag::StoreNb, q, b.constant(konst(pool, 1), loc), loc);
    const ExtraSlice then_region = b.close_region();

    b.open_region();
    b.binary(Inst::Tag::StoreNb, q, b.constant(konst(pool, 0), loc), loc);
    const ExtraSlice else_region = b.close_region();

    const Index cnd = b.cond(vc, then_region, else_region, loc);

    Ir ir = b.finish();

    // Root holds the signals, the load, and the cond — not the branch bodies.
    REQUIRE(ir.root().size() == 4);
    CHECK(ir.tag(ir.root()[3]) == Inst::Tag::Cond);

    std::array<Index, 3> scratch{};
    const auto           cond_ops = ir.operands(cnd, scratch);
    REQUIRE(cond_ops.size() == 1); // only the condition; regions aren't operands
    CHECK(cond_ops[0] == vc);

    const Regions regions = ir.extra_data<Regions>(ir.payload(cnd).op_extra.extra);
    CHECK(ir.region(regions.then_).size() == 2); // constant + store_nb
    CHECK(ir.region(regions.else_).size() == 2);
    CHECK(ir.tag(ir.region(regions.then_)[1]) == Inst::Tag::StoreNb);
}

TEST_CASE("case-equality comparisons are binary ops over two operands") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };

    const Index sel = b.unary(Inst::Tag::Load, b.signal(Signal{ 0 }, loc), loc);
    const Index lab = b.constant(konst(pool, 1), loc);
    const Index eq  = b.binary(Inst::Tag::CasezEq, sel, lab, loc);

    Ir ir = b.finish();
    CHECK(ir.tag(eq) == Inst::Tag::CasezEq);

    std::array<Index, 3> scratch{};
    const auto           ops = ir.operands(eq, scratch);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0] == sel);
    CHECK(ops[1] == lab);
}

TEST_CASE("mux and concat expose their operands") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };

    const Index c = b.constant(konst(pool, 1), loc);
    const Index x = b.constant(konst(pool, 2), loc);
    const Index z = b.constant(konst(pool, 3), loc);
    const Index m = b.mux(c, x, z, loc);

    const std::array<Index, 2> parts{ x, z };
    const Index                cc = b.concat(parts, loc);

    Ir ir = b.finish();

    std::array<Index, 3> scratch{};
    const auto           mux_ops = ir.operands(m, scratch);
    REQUIRE(mux_ops.size() == 3); // condition + both arms
    CHECK(mux_ops[0] == c);
    CHECK(mux_ops[1] == x);
    CHECK(mux_ops[2] == z);

    const auto concat_ops = ir.operands(cc, scratch);
    REQUIRE(concat_ops.size() == 2);
    CHECK(concat_ops[0] == x);
    CHECK(concat_ops[1] == z);
}

TEST_CASE("printer renders a flat combinational body") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };

    const Index a  = b.signal(Signal{ 0 }, loc);
    const Index bs = b.signal(Signal{ 1 }, loc);
    const Index y  = b.signal(Signal{ 2 }, loc);
    const Index va = b.unary(Inst::Tag::Load, a, loc);
    const Index vb = b.unary(Inst::Tag::Load, bs, loc);
    const Index r  = b.binary(Inst::Tag::Band, va, vb, loc);
    b.binary(Inst::Tag::Store, y, r, loc);

    Ir ir = b.finish();

    const std::string expected = "%0 = signal(s0)\n"
                                 "%1 = signal(s1)\n"
                                 "%2 = signal(s2)\n"
                                 "%3 = load(%0)\n"
                                 "%4 = load(%1)\n"
                                 "%5 = band(%3, %4)\n"
                                 "%6 = store(%2, %5)\n";
    CHECK(toString(ir) == expected);
}

TEST_CASE("printer indents cond regions and decodes constants") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };

    const Index c  = b.signal(Signal{ 0 }, loc);
    const Index vc = b.unary(Inst::Tag::Load, c, loc);
    const Index q  = b.signal(Signal{ 1 }, loc);

    b.open_region();
    b.binary(Inst::Tag::StoreNb, q, b.constant(konst(pool, 1), loc), loc);
    const ExtraSlice then_region = b.close_region();

    b.open_region();
    b.binary(Inst::Tag::StoreNb, q, b.constant(konst(pool, 0), loc), loc);
    const ExtraSlice else_region = b.close_region();

    b.cond(vc, then_region, else_region, loc);

    Ir ir = b.finish();

    const std::string expected = "%0 = signal(s0)\n"
                                 "%1 = load(%0)\n"
                                 "%2 = signal(s1)\n"
                                 "%7 = cond(%1) {\n"
                                 "  then:\n"
                                 "    %3 = constant(bit 0x01)\n"
                                 "    %4 = store_nb(%2, %3)\n"
                                 "  else:\n"
                                 "    %5 = constant(bit 0x00)\n"
                                 "    %6 = store_nb(%2, %5)\n"
                                 "}\n";
    CHECK(toString(ir) == expected);
}

TEST_CASE("string type interns once and string constants dedup on content") {
    InternPool pool;

    CHECK(pool.string_type != Ref::Invalid);
    CHECK(pool.put(Type{ .kind = Type::Kind::String }) == pool.string_type);

    // Not plane-sized: width 0 flags "branch on the kind before using layout".
    const Layout lay = pool.layout(pool.string_type);
    CHECK(lay.width == 0);
    CHECK(lay.byte_count == 0);

    const auto chars = [](std::string_view s) {
        return std::span<const uint8_t>{ reinterpret_cast<const uint8_t*>(s.data()), s.size() };
    };
    const Ref a = pool.put_value(pool.string_type, chars("hello"));
    const Ref b = pool.put_value(pool.string_type, chars("hello"));
    const Ref c = pool.put_value(pool.string_type, chars("world"));
    CHECK(a == b);
    CHECK(a != c);
    const auto got = pool.bytes(a);
    CHECK(std::string(reinterpret_cast<const char*>(got.data()), got.size()) == "hello");
}

TEST_CASE("string instructions carry their operands and print") {
    InternPool      pool;
    Builder         b(pool);
    const SourceLoc loc{ 1 };

    const auto      chars = std::string_view{ "hi" };
    const Ref       lit   = pool.put_value(
      pool.string_type, { reinterpret_cast<const uint8_t*>(chars.data()), chars.size() });

    const Index s   = b.signal(Signal{ 0 }, loc);
    const Index v   = b.unary(Inst::Tag::Load, s, loc);
    const Index k   = b.constant(lit, loc);
    const Index len = b.unary(Inst::Tag::StrLen, v, loc);
    const Index ch  = b.binary(Inst::Tag::StrGetc, v, len, loc);
    b.str_putc(s, len, ch, loc);
    b.str_substr(v, len, len, loc);
    b.str_to_packed(v, pool.bit_type, loc);
    b.binary(Inst::Tag::StrCmp, v, k, loc);

    Ir ir = b.finish();

    // Operand exposure: str_putc / str_substr surface op + their extra operands.
    std::array<Index, 3> scratch{};
    const auto           putc_ops = ir.operands(Index{ 5 }, scratch);
    REQUIRE(putc_ops.size() == 3);
    CHECK(putc_ops[0] == s);
    CHECK(putc_ops[1] == len);
    CHECK(putc_ops[2] == ch);

    const std::string expected = "%0 = signal(s0)\n"
                                 "%1 = load(%0)\n"
                                 "%2 = constant(string \"hi\")\n"
                                 "%3 = str_len(%1)\n"
                                 "%4 = str_getc(%1, %3)\n"
                                 "%5 = str_putc(%0[%3] = %4)\n"
                                 "%6 = str_substr(%1, %3, %3)\n"
                                 "%7 = str_to_packed(%1 -> bit)\n"
                                 "%8 = str_cmp(%1, %2)\n";
    CHECK(toString(ir) == expected);
}
