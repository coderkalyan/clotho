#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Lower.h"
#include "ir/Builder.h"
#include "ir/InternPool.h"
#include "ir/Ir.h"
#include "ir/Module.h"
#include "ir/Printer.h"

using namespace clotho::ir;

namespace {
// A 1-bit known constant value.
Ref konst(InternPool& pool, uint64_t v) {
    return pool.put_int(pool.bit_type, v);
}

// The exact text `clotho build --emit ir` writes for a design: print_module with
// the location resolver the CLI installs (write_artifact in main.cpp).
std::string emit_ir(std::string_view source) {
    const clotho::LowerResult r = clotho::lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::ostringstream os;
    print_module(*r.module, os, clotho::loc_resolver(r));

    return os.str();
}

size_t occurrences(std::string_view hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string_view::npos; at = hay.find(needle, at + 1)) {
        ++n;
    }

    return n;
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

TEST_CASE("a packed struct's field table participates in interning") {
    InternPool pool;

    // struct packed { logic [3:0] a; bit [7:0] b; } — a at the MSB (LRM 7.2.1).
    const uint32_t fields_a = pool.intern_struct_fields({
      { "a", pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.logic_type, .left = 3, .right = 0 }), 8 },
      { "b", pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.bit_type, .left = 7, .right = 0 }), 0 },
    });
    Type s;
    s.kind      = Type::Kind::Struct;
    s.is_packed = true;
    s.element   = pool.logic_type; // any field is 4-state -> whole struct is
    s.left      = 11;
    s.right     = 0;
    s.fields_id = fields_a;

    const Ref r1 = pool.put(Item{ s });
    const Ref r2 = pool.put(Item{ s }); // identical Type value -> same fields_id
    CHECK(r1 == r2);

    const Layout lay = pool.layout(r1);
    CHECK(lay.domain == Domain::Four);
    CHECK(lay.width == 12);

    const auto fields = pool.struct_fields(fields_a);
    REQUIRE(fields.size() == 2);
    CHECK(pool.field_name(fields[0]) == "a");
    CHECK(fields[0].bit_offset == 8);
    CHECK(pool.field_name(fields[1]) == "b");
    CHECK(fields[1].bit_offset == 0);

    // A second, independently-interned field table with the *same shape* is a
    // distinct id — the pool must not accidentally alias the two structs (each
    // src/lower/ call site is responsible for its own dedup via a symbol-keyed
    // cache; the pool only dedups by identical fields_id).
    const uint32_t fields_b = pool.intern_struct_fields({
      { "a", pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.logic_type, .left = 3, .right = 0 }), 8 },
      { "b", pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.bit_type, .left = 7, .right = 0 }), 0 },
    });
    CHECK(fields_b != fields_a);
    Type s2  = s;
    s2.fields_id = fields_b;
    const Ref r3 = pool.put(Item{ s2 });
    CHECK(r3 != r1);

    // Unpacked struct: no plane-packed value (mirrors String's sentinel).
    Type u;
    u.kind      = Type::Kind::Struct;
    u.is_packed = false;
    u.fields_id = fields_a;
    const Layout ulay = pool.layout(pool.put(Item{ u }));
    CHECK(ulay.width == 0);
    CHECK(ulay.byte_count == 0);
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

TEST_CASE("put_int on a >64-bit type zero-extends both planes") {
    InternPool pool;

    // logic [127:0]: val + xz planes of 16 bytes each. put_int used to copy
    // from an 8-byte source word with the plane width, reading out of bounds
    // and leaving garbage in the extension bytes.
    const Ref wide =
      pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.logic_type, .left = 127, .right = 0 });
    const Ref v = pool.put_int(wide, 0x0123456789ABCDEFull);

    const auto bytes = pool.bytes(v);
    REQUIRE(bytes.size() == 32);
    uint64_t low = 0;
    for (int i = 7; i >= 0; --i) {
        low = (low << 8) | bytes[static_cast<size_t>(i)];
    }
    CHECK(low == 0x0123456789ABCDEFull);
    for (size_t i = 8; i < 32; ++i) {
        CHECK(bytes[i] == 0x00); // val extension and the whole xz plane
    }

    // Same value again dedups; a 2-state wide type has a single 16-byte plane.
    CHECK(pool.put_int(wide, 0x0123456789ABCDEFull) == v);
    const Ref wide2 =
      pool.put(Type{ .kind = Type::Kind::PackedArray, .element = pool.bit_type, .left = 127, .right = 0 });
    CHECK(pool.bytes(pool.put_int(wide2, 42)).size() == 16);
    CHECK(pool.bytes(pool.put_int(wide2, 42))[0] == 42);
}

// Combinational body:  y = a & b;
TEST_CASE("build a combinational process and query it") {
    Module          m;
    InternPool&     pool = m.pool;
    Builder         b(m);
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

    std::array<Index, 4> scratch{};
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
    Module          m;
    InternPool&     pool = m.pool;
    Builder         b(m);
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

    std::array<Index, 4> scratch{};
    const auto           cond_ops = ir.operands(cnd, scratch);
    REQUIRE(cond_ops.size() == 1); // only the condition; regions aren't operands
    CHECK(cond_ops[0] == vc);

    const Regions regions = ir.extra_data<Regions>(ir.payload(cnd).op_extra.extra);
    CHECK(ir.region(regions.then_).size() == 2); // constant + store_nb
    CHECK(ir.region(regions.else_).size() == 2);
    CHECK(ir.tag(ir.region(regions.then_)[1]) == Inst::Tag::StoreNb);
}

TEST_CASE("case-equality comparisons are binary ops over two operands") {
    Module          m;
    InternPool&     pool = m.pool;
    Builder         b(m);
    const SourceLoc loc{ 1 };

    const Index sel = b.unary(Inst::Tag::Load, b.signal(Signal{ 0 }, loc), loc);
    const Index lab = b.constant(konst(pool, 1), loc);
    const Index eq  = b.binary(Inst::Tag::CasezEq, sel, lab, loc);

    Ir ir = b.finish();
    CHECK(ir.tag(eq) == Inst::Tag::CasezEq);

    std::array<Index, 4> scratch{};
    const auto           ops = ir.operands(eq, scratch);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0] == sel);
    CHECK(ops[1] == lab);
}

TEST_CASE("select and concat expose their operands") {
    Module          mod;
    InternPool&     pool = mod.pool;
    Builder         b(mod);
    const SourceLoc loc{ 1 };

    const Index c = b.constant(konst(pool, 1), loc);
    const Index x = b.constant(konst(pool, 2), loc);
    const Index z = b.constant(konst(pool, 3), loc);
    const Index m = b.select(c, x, z, loc);

    const std::array<Index, 2> parts{ x, z };
    const Index                cc = b.concat(parts, loc);

    Ir ir = b.finish();

    std::array<Index, 4> scratch{};
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
    Module          m;
    InternPool&     pool = m.pool;
    Builder         b(m);
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
    Module          m;
    InternPool&     pool = m.pool;
    Builder         b(m);
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
    Module          m;
    InternPool&     pool = m.pool;
    Builder         b(m);
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
    std::array<Index, 4> scratch{};
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

TEST_CASE("real / shortreal types intern with float layouts") {
    InternPool pool;

    CHECK(pool.real_type != pool.shortreal_type);
    CHECK(pool.is_float(pool.real_type));
    CHECK(pool.is_float(pool.shortreal_type));
    CHECK(!pool.is_float(pool.bit_type));

    // LRM 6.12: real is an IEEE-754 double, shortreal a float; both 2-state.
    const Layout r = pool.layout(pool.real_type);
    CHECK(r.domain == Domain::Two);
    CHECK(r.width == 64);
    CHECK(r.byte_count == 8);

    const Layout s = pool.layout(pool.shortreal_type);
    CHECK(s.domain == Domain::Two);
    CHECK(s.width == 32);
    CHECK(s.byte_count == 4);
}

TEST_CASE("real values intern by IEEE-754 bit pattern") {
    InternPool pool;

    const Ref a = pool.put_real(pool.real_type, 3.25);
    const Ref b = pool.put_real(pool.real_type, 3.25);
    const Ref c = pool.put_real(pool.real_type, -3.25);
    CHECK(a == b); // bit-identical -> deduplicated
    CHECK(a != c);

    const std::span<const uint8_t> bytes = pool.bytes(a);
    REQUIRE(bytes.size() == 8);
    uint64_t pat = 0;
    std::memcpy(&pat, bytes.data(), 8);
    CHECK(pat == std::bit_cast<uint64_t>(3.25));

    // -0.0 and +0.0 compare equal as doubles but are distinct bit patterns,
    // and interning is pattern-based.
    CHECK(pool.put_real(pool.real_type, 0.0) != pool.put_real(pool.real_type, -0.0));

    // NaN interning is stable: the same payload dedups to the same Ref.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(pool.put_real(pool.real_type, nan) == pool.put_real(pool.real_type, nan));

    // Same numeric value under different types stays distinct.
    CHECK(pool.put_real(pool.real_type, 1.5) != pool.put_real(pool.shortreal_type, 1.5));
}

TEST_CASE("shortreal values round to nearest float on interning") {
    InternPool pool;

    // 0.1 is not exactly representable; the pattern must be float(0.1), not a
    // truncated double (LRM 20.5 NOTE 1: round to nearest valid representation).
    const Ref v = pool.put_real(pool.shortreal_type, 0.1);
    const std::span<const uint8_t> bytes = pool.bytes(v);
    REQUIRE(bytes.size() == 4);
    uint32_t pat = 0;
    std::memcpy(&pat, bytes.data(), 4);
    CHECK(pat == std::bit_cast<uint32_t>(0.1f));
}

TEST_CASE("float tags round-trip through builder, payload_kind, and operands") {
    Module      m;
    InternPool& pool = m.pool;
    Builder     b(m);
    const auto l = static_cast<SourceLoc>(0);

    const Index x = b.constant(pool.put_real(pool.real_type, 1.5), l);
    const Index y = b.constant(pool.put_real(pool.real_type, 2.5), l);

    const Index sum = b.binary(Inst::Tag::FAdd, x, y, l);
    const Index neg = b.unary(Inst::Tag::FNeg, sum, l);
    const Index cvt = b.float_cvt(Inst::Tag::FtoI, neg, pool.bit_type, l);
    const std::array<Index, 2> margs{ x, y };
    const Index                pw = b.real_math(RealMathOp::Pow, margs, l);
    b.unary(Inst::Tag::Ret, cvt, l);
    b.unary(Inst::Tag::Ret, pw, l);

    const Ir ir = b.finish();
    CHECK(payload_kind(ir.tag(sum)) == Inst::PayloadKind::Binary);
    CHECK(payload_kind(ir.tag(neg)) == Inst::PayloadKind::Unary);
    CHECK(payload_kind(ir.tag(cvt)) == Inst::PayloadKind::OpType);
    CHECK(payload_kind(ir.tag(pw)) == Inst::PayloadKind::OpExtra);

    CHECK(ir.payload(cvt).op_type.type == pool.bit_type);

    const auto info = ir.extra_data<RealMathBinaryInfo>(ir.payload(pw).op_extra.extra);
    CHECK(info.fn == static_cast<uint32_t>(RealMathOp::Pow));

    std::array<Index, 4> scratch;
    const auto           ops = ir.operands(pw, scratch);
    REQUIRE(ops.size() == 2);
    CHECK(ops[0] == x);
    CHECK(ops[1] == y);
}

TEST_CASE("one-operand real math rides the op_imm payload") {
    Module      m;
    InternPool& pool = m.pool;
    Builder     b(m);
    const auto l = static_cast<SourceLoc>(0);

    const Index                x = b.constant(pool.put_real(pool.real_type, 1.5), l);
    const std::array<Index, 1> margs{ x };
    const Index                sq = b.real_math(RealMathOp::Sqrt, margs, l);
    b.unary(Inst::Tag::Ret, sq, l);

    const Ir ir = b.finish();
    CHECK(payload_kind(ir.tag(sq)) == Inst::PayloadKind::OpImm);
    CHECK(ir.payload(sq).op_imm.imm == static_cast<uint32_t>(RealMathOp::Sqrt));

    std::array<Index, 4> scratch;
    const auto           ops = ir.operands(sq, scratch);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == x);
}

// The IR type printer's class rendering (Printer.cpp's render_class_label). A class
// prints by NAME, and print_module describes each one exactly once in a class table
// ahead of the signals. Expanding the body inline at every occurrence, as the printer
// once did, was unbounded twice over: a class that reached itself made `--emit ir`
// recurse until the stack ran out -- 2.3 MB of `class{nm:string, kids:class{...` then
// SIGSEGV, on every UVM design, since uvm_printer.svh:419 declares
// `uvm_printer_element m_children[$]`, a queue of the enclosing class -- and an
// ACYCLIC class DAG was printed once per PATH, so the dump grew exponentially in the
// class count. Naming closes both at once: render_type never enters a class body, so
// there is nothing to recurse into and nothing to repeat, and the `class^N` cycle
// back-reference the first fix introduced is gone with it.
TEST_CASE("--emit ir describes a self-referential class once, by name") {
    const std::string ir = emit_ir(R"(
class node;
  string nm;
  node kids[$];
endclass
module top;
  node n;
  initial begin n = new(); n.nm = "root"; $display("ok %s", n.nm); end
endmodule
)");

    CHECK(ir.find("\n  class node {nm:string, kids:class node [$]}\n") != std::string::npos);
    CHECK(ir.find("s0: class node n (internal)") != std::string::npos);
    // The cycle reads as the name recurring inside the body, which is how the source
    // writes it. One description, and no `^` anywhere: there is no elision to mark.
    CHECK(occurrences(ir, "class node {") == 1);
    CHECK(occurrences(ir, "^") == 0);
}

TEST_CASE("--emit ir describes a mutual class cycle as two named classes") {
    const std::string ir = emit_ir(R"(
typedef class pong;
class ping; pong p; endclass
class pong; ping q; endclass
module top;
  ping a;
  initial begin a = new(); $display("ok"); end
endmodule
)");

    // A <-> B closes without a depth marker: each body names the other class.
    CHECK(ir.find("\n  class ping {p:class pong}\n  class pong {q:class ping}\n") != std::string::npos);
    CHECK(ir.find("s0: class ping a (internal)") != std::string::npos);
    CHECK(occurrences(ir, "^") == 0);
}

TEST_CASE("--emit ir describes a cycle through a container element") {
    const std::string ir = emit_ir(R"(
class box;
  box dyn[];
  box map[string];
endclass
module top;
  box c;
  initial begin c = new(); $display("ok"); end
endmodule
)");

    // Every container kind terminates for one reason: the element is a class and a
    // class is named, never expanded.
    CHECK(ir.find("\n  class box {dyn:class box [], map:class box [string]}\n") != std::string::npos);
    CHECK(occurrences(ir, "class box {") == 1);
}

TEST_CASE("the type printer names a class with no recorded name by its Ref") {
    // Built through the pool rather than from source for two reasons. slang refuses a
    // struct type that transitively reaches itself through a class handle ("'wrap'
    // recursively depends on its own definition"), so this cycle shape is currently
    // unreachable from SystemVerilog even though the type graph models it and the
    // printer must survive it. And a class interned straight into the pool registers
    // no name, which is the other case the label has to cover: it falls back on the
    // Ref, the same number the backend names the class by (`clotho.classdesc.<ref>`).
    // The type has to be built in the MODULE's pool: print_module resolves a
    // signal's Ref against `m.pool`, so a Ref minted anywhere else names a
    // different item.
    Module m;
    m.name = "top";

    // class box { struct { box inner; bit k; } w; } -- the class Ref must exist
    // before the struct that names it, which is what the reserved field table is
    // for (InternPool.h:371-378).
    const uint32_t box_fields  = m.pool.reserve_struct_fields();
    const Ref      box         = m.pool.put(Type{ .kind = Type::Kind::Class, .fields_id = box_fields });
    const uint32_t wrap_fields = m.pool.intern_struct_fields({ { "inner", box, 0 }, { "k", m.pool.bit_type, 1 } });
    const Ref      wrap        = m.pool.put(Type{ .kind = Type::Kind::Struct, .fields_id = wrap_fields });
    m.pool.set_struct_fields(box_fields, { { "w", wrap, 0 } });

    m.add_signal(SignalInfo{ .name = "c", .type = box });

    const std::string label = "class#" + std::to_string(static_cast<uint32_t>(box));
    CHECK(toString(m)
          == "module top\n  " + label + " {w:struct{inner:" + label + ", k:bit}}\n  s0: " + label
               + " c (internal)\n");
}

TEST_CASE("--emit ir describes a three-hop class cycle as three named classes") {
    const std::string ir = emit_ir(R"(
typedef class cb;
typedef class cc;
class ca; cb b; endclass
class cb; cc c; endclass
class cc; ca a; endclass
module top;
  ca t;
  initial begin t = new(); $display("ok"); end
endmodule
)");

    // A -> B -> C -> A. The table is in intern order, which is the order the cycle is
    // walked, so the hop that closes it is the last body's last property.
    CHECK(ir.find("\n  class ca {b:class cb}\n  class cb {c:class cc}\n  class cc {a:class ca}\n")
          != std::string::npos);
    CHECK(occurrences(ir, "^") == 0);
}

TEST_CASE("--emit ir of a small class design stays readable") {
    const std::string ir = emit_ir(R"(
class leaf; int x; endclass
class plain;
  int      x;
  string   s;
  leaf     l;
  logic    q[$];
  leaf     m[int];
endclass
module top;
  plain p;
  initial begin p = new(); $display("ok"); end
endmodule
)");

    // The whole head of the dump, pinned: two class-table lines then the signal. The
    // common case must not get worse for the sake of the large one, so this holds the
    // table to one line per class, the signal line to a bare name, and both to the
    // exact spelling the old inline form used inside its braces.
    CHECK(ir.find("module top\n"
                  "  class plain {x:signed bit[31:0], s:string, l:class leaf, q:logic [$], "
                  "m:class leaf [signed bit[31:0]]}\n"
                  "  class leaf {x:signed bit[31:0]}\n"
                  "  s0: class plain p (internal)")
          == 0);
    CHECK(occurrences(ir, "^") == 0);
}

TEST_CASE("--emit ir is linear in the class count") {
    // Class i holds handles to i+1, i+2 and i+3: strictly ACYCLIC, so no cycle guard
    // can account for the old growth, but a class is reachable from c0 by
    // exponentially many distinct PATHS and used to be printed once per path. Measured
    // then: 10 classes 7 KB, 14 classes 82 KB, 18 classes 933 KB, 22 classes 10.7 MB,
    // i.e. ~1.8x per class added (docs/TODO.md).
    const auto chain = [](int n) {
        std::string src;
        for (int i = 0; i < n; ++i) {
            src += "typedef class c" + std::to_string(i) + ";\n";
        }
        for (int i = 0; i < n; ++i) {
            src += "class c" + std::to_string(i) + ";";
            for (int d = 1; d <= 3; ++d) {
                if (i + d < n) {
                    const std::string k = std::to_string(i + d);
                    src += " c" + k + " p" + k + ";";
                }
            }
            src += " endclass\n";
        }
        src += "module top;\n  c0 t;\n"
               "  initial begin t = new(); $display(\"ok\"); end\nendmodule\n";

        return src;
    };

    const std::string small = emit_ir(chain(10));
    const std::string large = emit_ir(chain(20));

    // A concrete cap: twenty classes of three properties each cannot need 8 KB to
    // describe. The old printer spent ~3 MB on this design.
    CHECK(large.size() < 8 * 1024);
    // And the growth is linear, not exponential: each added class costs one
    // class-table line and one property mention in up to three others, which is a
    // per-class constant. The same ten classes multiplied the old dump ~350-fold.
    CHECK(large.size() > small.size());
    CHECK(large.size() - small.size() < 10 * 128);
    // Nothing was elided to get there.
    CHECK(occurrences(large, "^") == 0);
}

TEST_CASE("--emit ir describes a class reachable by two paths once") {
    const std::string ir = emit_ir(R"(
class leaf; int x; endclass
class mid; leaf a; endclass
class holder; mid m; leaf b; endclass
module top;
  holder h;
  initial begin h = new(); $display("ok"); end
endmodule
)");

    // `leaf` is reachable as h.b and as h.m.a. One description, two mentions.
    CHECK(occurrences(ir, "class leaf {") == 1);
    CHECK(ir.find("\n  class holder {m:class mid, b:class leaf}\n") != std::string::npos);
    CHECK(ir.find("\n  class mid {a:class leaf}\n") != std::string::npos);
}

TEST_CASE("--emit ir names a class specialization by its arguments") {
    const std::string ir = emit_ir(R"(
class box #(type T = int, int N = 1); T v; endclass
module top;
  box #(int)       bi;
  box #(string, 2) bs;
  initial begin bi = new(); bs = new(); $display("ok"); end
endmodule
)");

    // slang monomorphizes each `box#(args)` into its own ClassType but names every one
    // of them plainly `box`, so the label has to take the arguments from the type's
    // own rendering or every specialization of a generic class would collide -- which
    // in UVM is most of them (`uvm_queue#(uvm_callback)`).
    CHECK(ir.find("\n  class box#(int,1) {v:signed bit[31:0]}\n") != std::string::npos);
    CHECK(ir.find("\n  class box#(string,2) {v:string}\n") != std::string::npos);
    CHECK(ir.find("s0: class box#(int,1) bi (internal)") != std::string::npos);
    CHECK(ir.find("s1: class box#(string,2) bs (internal)") != std::string::npos);
}

TEST_CASE("--emit ir tells two same-named classes in different scopes apart") {
    const std::string ir = emit_ir(R"(
package p1; class c; int a; endclass endpackage
package p2; class c; string b; endclass endpackage
module top;
  p1::c x;
  p2::c y;
  initial begin x = new(); y = new(); $display("ok"); end
endmodule
)");

    // Names are unqualified, so these two collide -- and a label that names two
    // classes would make the class table unreadable, since every mention resolves
    // through it. Both then take the Ref, and NEITHER keeps the bare name: `class c`
    // next to `class c#13` would read as one class described twice. Asserted as an
    // invariant over the whole dump rather than a count, because the Refs depend on
    // what else the design interned.
    CHECK(occurrences(ir, "class c") > 0);
    CHECK(occurrences(ir, "class c") == occurrences(ir, "class c#"));
    // One table line each, and the bodies say which is which.
    CHECK(occurrences(ir, " {a:signed bit[31:0]}\n") == 1);
    CHECK(occurrences(ir, " {b:string}\n") == 1);
}

TEST_CASE("--emit ir names a nested class by its own name") {
    const std::string ir = emit_ir(R"(
class outer;
  class inner; int y; endclass
  inner i;
endclass
module top;
  outer o;
  initial begin o = new(); $display("ok"); end
endmodule
)");

    // A nested class (LRM 8.24) is named by its own identifier, not by a path through
    // its enclosing class: the enclosing scope is dropped for the same reason a
    // package's is.
    CHECK(ir.find("\n  class outer {i:class inner}\n") != std::string::npos);
    CHECK(ir.find("\n  class inner {y:signed bit[31:0]}\n") != std::string::npos);
}
