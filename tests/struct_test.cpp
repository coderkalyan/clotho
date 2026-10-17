#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

// Packed struct member access (LRM 7.2.1). Field lowering itself (the IR
// Type::Kind::Struct, its field table, and the frontend-only guards) is
// covered in ir_test.cpp / lower_test.cpp; these are end-to-end JIT-run tests
// since the one real bug found here (a field narrower than the struct's own
// forced-4-state domain producing a mismatched LLVM type) only surfaced at
// LLVM verification, not in the IR.

namespace {

// JIT-run a module and capture stdout (mirrors display_test.cpp).
std::string run_capture(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    std::fflush(stdout);
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    std::rewind(tmp);
    std::string out;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return out;
}

} // namespace

TEST_CASE("blocking read and write of packed struct fields") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'hAB;
                x.a = 4'h5;
                $display("a=%h b=%h whole=%h", x.a, x.b, x);
            end
        endmodule
    )") == "a=5 b=ab whole=5ab\n");
}

TEST_CASE("nonblocking write to a struct field") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            logic clk;
            initial begin
                clk = 0;
                x.b = 8'h00;
                #1 clk = 1;
                #1; // let the NBA region commit before reading
                $display("b=%h", x.b);
            end
            always @(posedge clk) x.b <= x.b + 8'd1;
        endmodule
    )") == "b=01\n");
}

// Regression: field `b` is 2-state (`bit`) but sibling field `a` is 4-state
// (`logic`), so the whole struct's physical storage is forced 4-state (LRM
// 6.11.2). The backend's PartSelect type inference inherits the base
// signal's domain (correct for a domain-uniform vector/array, the only prior
// case), so a bare struct-field read was 4-state-shaped even though `b`'s
// declared type is 2-state — invisible until the value fed an arithmetic op
// that trusts the declared type, which crashed LLVM verification (mismatched
// operand widths). Fixed by resizing a MemberAccess read to the field's own
// declared type before use.
TEST_CASE("arithmetic on a 2-state field of an otherwise 4-state struct") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'd1;
                x.b = x.b + 8'd1;
                $display("b=%h", x.b);
            end
        endmodule
    )") == "b=02\n");
}

// Same regression class, reached through a bit/range-select stacked on top
// of a narrow-domain field (`x.b[7:4]`) rather than the bare field read.
// SV can't chain a second bit/range-select onto that result (only member
// access dots further), so this and the MemberAccess case above are the only
// two points a struct field's value can escape into further computation.
TEST_CASE("arithmetic on a range-select of a 2-state struct field") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'hF0;
                $display("slice=%h add=%h", x.b[7:4], x.b[7:4] + 4'h1);
            end
        endmodule
    )") == "slice=f add=0\n");
}

TEST_CASE("compound assignment to a struct field") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [3:0] a;
                bit [7:0] b;
            } s_t;
            s_t x;
            initial begin
                x.b = 8'd10;
                x.b += 8'd5;
                $display("b=%h", x.b);
            end
        endmodule
    )") == "b=0f\n");
}

TEST_CASE("nested packed struct member access") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed {
                logic [1:0] suba;
                bit [3:0] subb;
            } inner_t;
            typedef struct packed {
                inner_t x;
                logic [2:0] y;
            } outer_t;
            outer_t o;
            initial begin
                o.x.suba = 2'b10;
                o.x.subb = 4'hC;
                o.y = 3'b101;
                $display("whole=%h", o);
            end
        endmodule
    )") == "whole=165\n");
}

TEST_CASE("packed union members overlay the LSB (LRM 7.3.1)") {
    // Every member of a packed union overlays the least-significant bits, so a
    // whole-value write is visible through the members and a member write is
    // visible through the whole value. Byte-identical to iverilog.
    CHECK(run_capture(R"(
        module m;
            typedef union packed {
                logic [15:0] w;
                struct packed { logic [7:0] hi; logic [7:0] lo; } b;
                logic [15:0] aliasw;
            } u_t;
            u_t u;
            initial begin
                u.w = 16'hABCD;
                $display("w=%h hi=%h lo=%h alias=%h", u.w, u.b.hi, u.b.lo, u.aliasw);
                u.b.hi = 8'h12;
                u.b.lo = 8'h34;
                $display("w=%h hi=%h lo=%h", u.w, u.b.hi, u.b.lo);
            end
        endmodule
    )") == "w=abcd hi=ab lo=cd alias=abcd\nw=1234 hi=12 lo=34\n");
}

TEST_CASE("packed union nested in a packed struct") {
    CHECK(run_capture(R"(
        module m;
            typedef union packed {
                logic [15:0] w;
                struct packed { logic [7:0] hi; logic [7:0] lo; } b;
            } u_t;
            typedef struct packed { logic [3:0] tag; u_t payload; } wrap_t;
            wrap_t wr;
            initial begin
                wr.tag = 4'h5;
                wr.payload.w = 16'hDEAD;
                $display("tag=%h w=%h hi=%h", wr.tag, wr.payload.w, wr.payload.b.hi);
                wr.payload.b.lo = 8'hEE;
                $display("w=%h", wr.payload.w);
            end
        endmodule
    )") == "tag=5 w=dead hi=de\nw=deee\n");
}

TEST_CASE("packed union x-propagates through overlaid members") {
    CHECK(run_capture(R"(
        module m;
            union packed {
                logic [7:0] a;
                struct packed { logic [3:0] h; logic [3:0] l; } b;
            } u;
            initial begin
                u.a = 8'bxxxx_0011;
                $display("a=%b h=%b l=%b", u.a, u.b.h, u.b.l);
            end
        endmodule
    )") == "a=xxxx0011 h=xxxx l=0011\n");
}

TEST_CASE("packed tagged union: setter, member read, whole value (LRM 7.3.2)") {
    // A `tagged Member(value)` setter builds the flat {tag, payload}: tag
    // (member index) in the high bit(s), payload in the low bits. Member read is
    // the low payload; the whole value is the raw 33-bit vector. %b prints the
    // raw bits rather than rendering the tag symbolically.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            opt_t a, b;
            initial begin
                a = tagged Valid (42);
                b = tagged Invalid;
                $display("%0d %b %b %b", a.Valid, a, b, a == b);
                b = a;
                $display("%0d %b", b.Valid, a == b);
            end
        endmodule
    )") == "42 100000000000000000000000000101010 000000000000000000000000000000000 0\n"
           "42 1\n");
}

TEST_CASE("packed tagged union: narrow member gap is x for 4-state, run==param") {
    // A member narrower than the widest leaves a gap between payload and tag. For
    // a 4-state union that gap is x (the type's power-on default), matching
    // slang's own const-eval — so a runtime setter and a folded parameter of the
    // same expression produce identical bits.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { logic [7:0] narrow; logic [31:0] wide; } u_t;
            localparam u_t P = tagged narrow (8'hAB);
            u_t x;
            initial begin
                x = tagged narrow (8'hAB);
                $display("%b", x);
                $display("%b", P);
            end
        endmodule
    )") == "0xxxxxxxxxxxxxxxxxxxxxxxx10101011\n"
           "0xxxxxxxxxxxxxxxxxxxxxxxx10101011\n");
}

TEST_CASE("tagged union: a member write does not retag; it checks the tag (LRM 11.9)") {
    // LRM 11.9: "the value read or assigned shall be consistent with the current
    // tag ... An attempt to read or assign a value whose type is inconsistent with
    // the tag results in a run-time error", and its own example calls
    // `i1.Add = '{19, 4, 3};` legal "only if the instruction variable i1 currently
    // has tag Add". So a member write never moves the tag — only a whole-value
    // `tagged` expression does (LRM 7.3.2).
    const std::string src = "module m;\n"
                            "    typedef union tagged packed { void Invalid; int Valid; } opt_t;\n"
                            "    opt_t a;\n"
                            "    initial begin\n"
                            "        a = tagged Invalid;\n"
                            "        $display(\"before\");\n"
                            "        a.Valid = 7;\n"
                            "        $display(\"after\");\n"
                            "    end\n"
                            "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    // The store never happens, and nothing after it runs.
    CHECK(run.out == "before\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("fatal: assignment to tagged-union member `Valid`") != std::string::npos);
    CHECK(run.err.find("the tag selects `Invalid`") != std::string::npos);
    CHECK(run.err.find("source:7:9") != std::string::npos);
    CHECK(run.err.find("7 |         a.Valid = 7;\n") != std::string::npos);
}

TEST_CASE("tagged union: writing the member the tag already selects is fine") {
    // The other side of LRM 11.9: a member write is legal exactly while its own tag
    // is current, and it updates only the payload.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            opt_t a;
            initial begin
                a = tagged Valid (5);
                a.Valid = 7;
                a.Valid += 1;
                $display("%0d %b", a.Valid, a);
            end
        endmodule
    )") == "8 100000000000000000000000000001000\n");
}

TEST_CASE("tagged union: reading a member the tag does not select is a fatal (LRM 11.9)") {
    const std::string src = "module m;\n"
                            "    typedef union tagged packed { void Invalid; int Valid; } opt_t;\n"
                            "    opt_t a;\n"
                            "    initial begin\n"
                            "        a = tagged Invalid;\n"
                            "        $display(\"%0d\", a.Valid);\n"
                            "    end\n"
                            "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);

    CHECK(run.out.empty());
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("fatal: read of tagged-union member `Valid`") != std::string::npos);
    CHECK(run.err.find("(LRM 11.9)") != std::string::npos);
}

TEST_CASE("tagged union: a compound or concat member write is checked too (LRM 11.9)") {
    // Both reach the member lvalue through paths that used to skip the tag entirely
    // (lower_compound_assign / store_concat), so both silently wrote a payload under
    // a stale tag before the guard moved into lower_member_lvalue.
    const std::string compound = "module m;\n"
                                 "    typedef union tagged packed { void Invalid; int Valid; } opt_t;\n"
                                 "    opt_t a;\n"
                                 "    initial begin a = tagged Invalid; a.Valid += 1; end\n"
                                 "endmodule\n";
    const test::DesignRun cr = test::run_design_captured(compound);
    CHECK(test::design_exit_code(cr) != 0);
    CHECK(cr.err.find("fatal: assignment to tagged-union member `Valid`") != std::string::npos);

    const std::string concat = "module m;\n"
                               "    typedef union tagged packed { void Invalid; int Valid; } opt_t;\n"
                               "    opt_t a;\n"
                               "    logic z;\n"
                               "    initial begin a = tagged Invalid; {a.Valid, z} = 33'h1234; end\n"
                               "endmodule\n";
    const test::DesignRun kr = test::run_design_captured(concat);
    CHECK(test::design_exit_code(kr) != 0);
    CHECK(kr.err.find("fatal: assignment to tagged-union member `Valid`") != std::string::npos);
}

TEST_CASE("tagged union: an x tag selects no member and names itself as undefined") {
    // LRM 11.9: "An uninitialized variable of tagged union type shall be undefined.
    // This includes the tag bits." An x tag equals no member's index, so any member
    // access reports — and the report cannot name a member for it.
    const std::string src = "module m;\n"
                            "    typedef union tagged packed { void Invalid; logic [31:0] Valid; } opt_t;\n"
                            "    opt_t a;\n"
                            "    initial $display(\"%0d\", a.Valid);\n"
                            "endmodule\n";

    const test::DesignRun run = test::run_design_captured(src);
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("the tag selects undefined (x)") != std::string::npos);
}

TEST_CASE("unpacked tagged union: LSB overlay under the synthesized tag (LRM 7.3.2)") {
    // An unpacked tagged union lowers to the same flat {synthesized-tag, payload}
    // value, so a member read is the low payload bits. Only the member the tag
    // selects may be read (LRM 11.9), so this walks the tag along with the reads
    // rather than reading one member through another's tag. iverilog and verilator
    // have no tagged-union support; values are hand-checkable overlay.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged { void none; byte b; int w; } u_t;
            u_t x;
            initial begin
                x = tagged b (8'hAB);
                $display("%h", x.b);
                x = tagged w (32'hDEADBEEF);
                $display("%h", x.w);
                x.w = 32'h0000005A;
                $display("%h", x.w);
            end
        endmodule
    )") == "ab\ndeadbeef\n0000005a\n");
}

TEST_CASE("tagged union pattern case: tag match, .var binding, filter (LRM 12.6)") {
    // `case (u) matches`: first-match over tagged patterns; `.n` binds the
    // payload, `&&& (n > 10)` filters. iverilog / verilator have no tagged-union
    // support.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            task automatic show(opt_t u);
                case (u) matches
                    tagged Invalid              : $display("invalid");
                    tagged Valid .n &&& (n > 10): $display("big %0d", n);
                    tagged Valid .n             : $display("small %0d", n);
                endcase
            endtask
            initial begin
                show(tagged Valid (42));
                show(tagged Valid (3));
                show(tagged Invalid);
            end
        endmodule
    )") == "big 42\nsmall 3\ninvalid\n");
}

TEST_CASE("tagged union: if/else matches with pattern-var binding (LRM 12.6.2)") {
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            opt_t a;
            initial begin
                a = tagged Valid (7);
                if (a matches tagged Valid .n) $display("match %0d", n);
                if (a matches tagged Invalid) $display("no"); else $display("else ok");
            end
        endmodule
    )") == "match 7\nelse ok\n");
}

TEST_CASE("tagged union: ?: matches binds and single-evaluates the chosen arm") {
    // `expr matches pattern ? a : b` (LRM 12.6.2). `.n` binds the payload; when
    // an arm has an observable effect (a call), only the chosen arm evaluates
    // (11.4.11 single-evaluation).
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            function automatic int side(int x); $display("eval %0d", x); return x; endfunction
            initial begin
                opt_t u;
                u = tagged Valid (5);
                $display("r=%0d", u matches tagged Valid .n ? side(n) : side(99));
                u = tagged Invalid;
                $display("r=%0d", u matches tagged Valid .n ? side(n) : side(99));
            end
        endmodule
    )") == "eval 5\nr=5\neval 99\nr=99\n");
}

TEST_CASE("pattern case: default, unpacked-union tag, and structure pattern (LRM 12.6)") {
    // A structure pattern binds and constant-matches per field; `.*` is a
    // field wildcard.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged { void none; byte b; int w; } u_t;
            typedef struct packed { logic [7:0] x; logic [7:0] y; } pt;
            initial begin
                u_t u;
                pt p;
                u = tagged w (100);
                case (u) matches
                    tagged none : $display("none");
                    tagged b .c : $display("byte %0d", c);
                    tagged w .d : $display("word %0d", d);
                    default     : $display("def");
                endcase
                p = '{8'hAB, 8'hCD};
                if (p matches '{x: 8'hAB, y: .yy}) $display("yy=%h", yy);
                if (p matches '{x: .*, y: 8'h00}) $display("bad"); else $display("sok");
            end
        endmodule
    )") == "word 100\nyy=cd\nsok\n");
}

TEST_CASE("casez/casex pattern case wildcards the label bits (LRM 12.6.1)") {
    // LRM 12.6.1: casez/casex `matches` wildcard the z (casez) or x/z (casex)
    // bits of the pattern's labels, over tag and member bits alike. Clotho is
    // LRM-compliant here; iverilog/verilator have no tagged unions, so the
    // expected values are hand-derived from the normative text.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void none; logic [7:0] data; } u_t;
            task automatic zc(u_t u);
                casez (u) matches
                    tagged data 8'b1zzz_zzzz : $display("high");
                    tagged data 8'b0000_zzzz : $display("lo0");
                    tagged none              : $display("none");
                    default                  : $display("other");
                endcase
            endtask
            task automatic xc(u_t u);
                casex (u) matches
                    tagged data 8'b1xxx_xxxx : $display("xhigh");
                    default                  : $display("xother");
                endcase
            endtask
            initial begin
                zc(tagged data (8'hC3));  // 1100_0011 -> high
                zc(tagged data (8'h05));  // 0000_0101 -> lo0
                zc(tagged data (8'h35));  // 0011_0101 -> other
                zc(tagged none);
                xc(tagged data (8'hC3));  // 1... -> xhigh
                xc(tagged data (8'h35));  // 0... -> xother
            end
        endmodule
    )") == "high\nlo0\nother\nnone\nxhigh\nxother\n");
}

TEST_CASE("unpacked union overlays members at the LSB (flat value, LRM 7.3)") {
    // An unpacked union lowers to a flat plane value sized to the widest member,
    // every member overlaying the low bits. A narrower member write leaves the
    // high bits untouched. Byte-identical to verilator (iverilog has no unpacked
    // union / struct support).
    CHECK(run_capture(R"(
        module m;
            typedef union {
                logic [31:0] word;
                struct packed { logic [15:0] hi; logic [15:0] lo; } halves;
                byte b;
            } u_t;
            u_t u;
            initial begin
                u.word = 32'hDEAD_BEEF;
                $display("word=%h hi=%h lo=%h", u.word, u.halves.hi, u.halves.lo);
                u.halves.hi = 16'h1234;
                $display("word=%h", u.word);
                u.b = 8'h77;
                $display("word=%h b=%h", u.word, u.b);
            end
        endmodule
    )") == "word=deadbeef hi=dead lo=beef\nword=1234beef\nword=1234be77 b=77\n");
}

TEST_CASE("unpacked union: whole-union copy and nesting in an unpacked struct") {
    CHECK(run_capture(R"(
        module m;
            typedef union { logic [31:0] w; logic [7:0] b; } u_t;
            typedef struct { logic [7:0] tag; u_t payload; } wrap_t;
            u_t u1, u2;
            wrap_t wr;
            initial begin
                u1.w = 32'h10;
                u2   = u1;
                $display("u2.w=%h u2.b=%h", u2.w, u2.b);
                wr.tag       = 8'hA5;
                wr.payload.w = 32'h0000_FFFF;
                $display("tag=%h w=%h b=%h", wr.tag, wr.payload.w, wr.payload.b);
            end
        endmodule
    )") == "u2.w=00000010 u2.b=10\ntag=a5 w=0000ffff b=ff\n");
}

TEST_CASE("unpacked union member must be integral") {
    const LowerResult r = lower_text(R"(
        module m;
            union { logic [31:0] w; real r; } u;
            initial u.w = 1;
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("unpacked union member must be an integral type") != std::string::npos);
}

TEST_CASE("tagged union with a non-integral member is rejected loudly (Phase 3 gap)") {
    // A non-integral member of an unpacked tagged union (here a `real` payload)
    // has no flat-bit overlay — the P1 flat-value representation cannot hold it,
    // so it fails loudly per PRINCIPLES.md (docs/design/tagged-unions.md Phase 3).
    const LowerResult r = lower_text(R"(
        module m;
            union tagged { void none; real r; } u;
            initial u = tagged r (1.5);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("unpacked union member must be an integral type") != std::string::npos);
}

TEST_CASE("%p of a tagged union prints '{tag:value} (LRM 21.2.1.6)") {
    // LRM 21.2.1.6: "For unions, only the first declared elements shall be printed.
    // In the case of a tagged union, it shall print "tag:value" along with the
    // currently valid element." Which member that is only the run-time tag knows,
    // so this is the one %p shape rendered in the runtime. A `void` member has no
    // element, so its tag prints alone. The payload renders at the MEMBER's own
    // width, not the union's payload region, so the gap bits never leak in, and at
    // the member's own signedness (`byte b` of 8'hAB is -85).
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            typedef union tagged { void none; byte b; int w; } up_t;
            opt_t a, i;
            up_t  u;
            initial begin
                a = tagged Valid (42);
                i = tagged Invalid;
                $display("%p %p", a, i);
                u = tagged b (8'hAB);
                $display("%p", u);
                u = tagged w (32'hDEADBEEF);
                $display("%p", u);
                u = tagged none;
                $display("%p", u);
            end
        endmodule
    )") == "'{Valid:42} '{Invalid}\n'{b:-85}\n'{w:-559038737}\n'{none}\n");
}

TEST_CASE("%p of a tagged union whose tag is x falls back to the flat value") {
    // An uninitialized tagged union has undefined tag bits (LRM 11.9), so no member
    // is valid and there is no `tag:value` to print. LRM 21.2.1.6 does not cover
    // that, so the whole flat value renders as one integral leaf — all-x under the
    // decimal leaf rule, rather than inventing a member name (docs/LRM_NOTES.md, note 32).
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; logic [15:0] Valid; } opt_t;
            opt_t a;
            initial $display("%p", a);
        endmodule
    )") == "'{x}\n");
}

TEST_CASE("%p of a tagged union is a legal assignment pattern (sv-tests 7.3.2)") {
    // The whole `%p` output "shall be a legal interpretation of the assignment
    // pattern syntax" (LRM 21.2.1.6), so the `tag:value` pair is bracketed. This is
    // the shape sv-tests chapter-7/unions/tagged/basic.sv asserts.
    // The assert text embeds `)"`, so the raw string needs its own delimiter.
    CHECK(run_capture(R"SV(
        module m;
            union tagged { void invalid; bit [3:0] valid; } un;
            initial begin
                un = tagged valid (10);
                $display(":assert: ('%p' == ''{valid:10}')", un);
            end
        endmodule
    )SV") == ":assert: (''{valid:10}' == ''{valid:10}')\n");
}

TEST_CASE("%p of a nested tagged union / packed struct / enum (LRM 21.2.1.6)") {
    // Each element of an aggregate is printed under the clause's per-element rules,
    // so a tagged union, a packed struct, and an enum nested inside an unpacked
    // struct each render in their own form rather than as a bare integer. This is
    // what needs the slang type threaded through the %p expansion: `ir::Type` cannot
    // tell a tagged union from a plain packed union.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            typedef enum logic [1:0] { RED, GREEN } c_t;
            typedef struct packed { byte hi; byte lo; } ps_t;
            typedef struct { opt_t o; c_t c; ps_t p; int n; } outer_t;
            opt_t arr [0:1];
            outer_t x;
            initial begin
                x.o = tagged Valid (7);
                x.c = GREEN;
                x.p = '{8'h11, 8'h22};
                x.n = 5;
                $display("%p", x);
                arr[0] = tagged Valid (1);
                arr[1] = tagged Invalid;
                $display("%p", arr);
            end
        endmodule
    )") == "'{o:'{Valid:7}, c:GREEN, p:'{hi:17, lo:34}, n:5}\n"
           "'{'{Valid:1}, '{Invalid}}\n");
}

TEST_CASE("%p of a packed struct, a plain union, and an enum (LRM 21.2.1.6)") {
    // Three more `shall` clauses that the aggregate-ABI dispatch used to miss, all
    // rendering as a bare decimal before: "A packed structure data type shall print
    // its value as an assignment pattern with named elements", "For unions, only the
    // first declared elements shall be printed", and "An enumerated data type shall
    // print its value as an enumeration name if the value is valid for that type.
    // Otherwise the value shall print according to the base type" — so a value
    // matching no member of a `byte`-based enum prints as a signed decimal (the
    // clause names the base type, and `byte` is signed).
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { byte a; byte b; } ps_t;
            typedef union packed { byte a; byte b; } pu_t;
            typedef enum logic [1:0] { RED, GREEN, BLUE } c_t;
            typedef enum byte { LOW = 8'sh01 } sb_t;
            ps_t s;
            pu_t u;
            c_t  c;
            sb_t sb;
            initial begin
                s = '{8'hAB, 8'h2A};
                u.a = 8'hAB;
                $display("%p %p", s, u);
                c = GREEN;
                $display("%p", c);
                c = c_t'(2'd3);
                $display("%p", c);
                sb = sb_t'(8'hF7);
                $display("%p", sb);
            end
        endmodule
    )") == "'{a:-85, b:42} '{a:-85}\nGREEN\n3\n-9\n");
}

TEST_CASE("an escaped %%p does not consume the %p splice") {
    // build_format substitutes the expansion at the first `%p` in the format text;
    // scanning has to skip a `%%` escape, or `%%p` swallows the splice and leaves
    // the real specifier for the runtime formatter to reject.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int x; } s_t;
            s_t a, c;
            initial begin
                a = '{1};
                c = '{2};
                $display("%%p [%p] and [%p] 100%%", a, c);
            end
        endmodule
    )") == "%p ['{x:1}] and ['{x:2}] 100%\n");
}

TEST_CASE("unique / unique0 if accepts a pattern condition (LRM 12.4.2 + 12.6.2)") {
    // A pattern is a cond_predicate (LRM 12.6.2) and unique_priority attaches to
    // `if (cond_predicate)` (12.4.2), so `unique if (x matches p)` is legal SV. It
    // used to fail loudly because lower_unique_if collected a bare Expression per
    // arm instead of the arm's condition list; a `&&&` filter failed even earlier,
    // since it makes the cond_predicate multi-condition.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            opt_t a;
            initial begin
                a = tagged Valid (5);
                unique if (a matches tagged Valid .n) $display("v %0d", n);
                else $display("inv");
                a = tagged Invalid;
                unique0 if (a matches tagged Valid .m) $display("v %0d", m);
                else $display("inv");
                a = tagged Valid (5);
                unique if (a matches tagged Valid .p &&& (p > 10)) $display("big %0d", p);
                else if (a matches tagged Valid .q &&& (q <= 10)) $display("small %0d", q);
                else $display("none");
            end
        endmodule
    )") == "v 5\ninv\nsmall 5\n");
}

TEST_CASE("%p of a queue of tagged unions names the tag") {
    // LRM 21.2.1.6 requires `tag:value` for a tagged union, which the flat runtime
    // container descriptor cannot express (it carries no member names). Such a
    // container renders through the frontend's own element loop instead, so the
    // member-named form is the same one a bare tagged union prints.
    CHECK(run_capture(R"(
        module m;
            typedef union tagged packed { void Invalid; int Valid; } opt_t;
            opt_t q[$];
            initial begin
                q.push_back(tagged Valid (1));
                q.push_back(tagged Invalid);
                $display("%p", q);
            end
        endmodule
    )") == "'{'{Valid:1}, '{Invalid}}\n");
}

// --- Unpacked structs (aggregate llvm::StructType backing, Stage 1) ---
// An unpacked struct has no flat bit-vector value (LRM 7.2): each member owns
// its own storage, addressed by a struct GEP (MemberSelect), not a bit offset.
// (iverilog 13 does not support unpacked structs.)

TEST_CASE("blocking read and write of unpacked struct members") {
    CHECK(run_capture(R"(
        module m;
            typedef struct {
                logic [7:0] a;
                bit  [3:0]  b;
                logic       c;
            } s_t;
            s_t x;
            initial begin
                x.a = 8'hAB;
                x.b = 4'd5;
                x.c = 1'b1;
                $display("a=%h b=%h c=%b", x.a, x.b, x.c);
            end
        endmodule
    )") == "a=ab b=5 c=1\n");
}

TEST_CASE("unpacked struct member power-on: 4-state X, 2-state 0") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x;
            initial $display("a=%h b=%h", x.a, x.b);
        endmodule
    )") == "a=xx b=0\n");
}

// -- Struct member default initializers (LRM 7.2's struct_union_member
// list_of_variable_decl_assignments, mirroring a class property default,
// LRM 8.8): a field's own `= expr` applies when a variable of the struct
// type is declared with no initializer of its own. iverilog has no unpacked
// struct support.

TEST_CASE("struct field default applies when the variable has no initializer") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int x = 5; int y; } s_t;
            s_t s;
            initial $display("%0d %0d", s.x, s.y);
        endmodule
    )") == "5 0\n");
}

TEST_CASE("struct field default equal to a parameter") {
    CHECK(run_capture(R"(
        module m;
            parameter int W = 8;
            typedef struct { int x = W; int y = 5; } s_t;
            s_t s;
            initial $display("%0d %0d", s.x, s.y);
        endmodule
    )") == "8 5\n");
}

// An explicit variable initializer overrides every field default (it's a
// whole different '{...} pattern, not merged with the type's defaults).
TEST_CASE("an explicit variable initializer overrides struct field defaults") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int x = 5; int y = 6; } s_t;
            s_t s = '{x: 99, y: 100};
            initial $display("%0d %0d", s.x, s.y);
        endmodule
    )") == "99 100\n");
}

// A field default nested inside another struct field applies recursively.
TEST_CASE("a nested struct field's own default applies") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a = 7; } inner_t;
            typedef struct { inner_t i; int b = 3; } outer_t;
            outer_t o;
            initial $display("%0d %0d", o.i.a, o.b);
        endmodule
    )") == "7 3\n");
}

// A static function-local with a struct field default is guarded loudly
// (same "runs once, not per call" hazard as an explicit static-local
// initializer): applying it per call would clobber a previous call's
// mutations.
// A static local struct's field-level defaults apply once before time zero
// (LRM 13.4.2 / 7.2), then the object persists across calls.
TEST_CASE("static local struct field default applies once") {
    const std::string out = run_capture(R"(
        module m;
            typedef struct { int x = 5; } s_t;
            function automatic int f();
                static s_t s;
                s.x++;
                return s.x;
            endfunction
            initial $display("%0d %0d", f(), f());
        endmodule
    )");
    CHECK(out == "6 7\n");  // init once to 5, ++ each call -> 6, 7
}

// An array of a struct with field defaults, and no array initializer of its
// own, is a documented follow-up (not yet element-wise defaulted) -- guarded
// loudly rather than silently leaving every element's fields at their plain
// power-on value.
TEST_CASE("an array of a struct with field defaults fails loudly") {
    const LowerResult r = lower_text(R"(
        module m;
            typedef struct { int x = 5; } s_t;
            s_t arr [1:0];
            initial $display("%0d", arr[0].x);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("array of a struct with field-level default initializers") !=
          std::string::npos);
}

TEST_CASE("whole unpacked struct copy is field-wise") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x, y;
            initial begin
                x.a = 8'h12; x.b = 4'hF;
                y = x;
                $display("y=%h %h", y.a, y.b);
            end
        endmodule
    )") == "y=12 f\n");
}

TEST_CASE("unpacked struct assignment pattern initializes each member") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; logic c; } s_t;
            s_t x;
            initial begin
                x = '{a: 8'hA5, b: 4'h3, c: 1'b1};
                $display("%h %h %b", x.a, x.b, x.c);
            end
        endmodule
    )") == "a5 3 1\n");
}

TEST_CASE("unpacked struct declaration initializer") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x = '{a: 8'h77, b: 4'hE};
            initial $display("%h %h", x.a, x.b);
        endmodule
    )") == "77 e\n");
}

TEST_CASE("unpacked struct equality is field-wise; X makes it ambiguous (LRM 11.4.5)") {
    // == / != reduce over members; an unknown member makes the relation
    // ambiguous, so the result is X (docs/LRM_NOTES.md, note 13).
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x, y, z;
            initial begin
                x.a = 8'h11; x.b = 4'h2;
                y.a = 8'h11; y.b = 4'h2;
                z.a = 8'h11; z.b = 4'h3;
                $display("eq=%b ne=%b", x == y, x != z);
                x.a = 8'hxx;
                $display("xeq=%b", x == y);
            end
        endmodule
    )") == "eq=1 ne=1\nxeq=x\n");
}

// Case equality (===/!==) over an unpacked struct / queue / dynamic array
// (LRM 11.4.5, Table 11-1) used to fall through past the == / != aggregate
// reduction entirely: the frontend only special-cased Equality/Inequality,
// so === on an aggregate/container operand reached the generic bitstream-
// cast lowering, which tries to LOAD the whole aggregate as a scalar. For a
// struct that miscompiled silently; for a queue/dynamic-array (no plane
// value at all) the backend's defense-in-depth check caught it and aborted
// with an unlocated "plane load of a container handle" -- found via UVM's
// `uvm_field_queue_string` field-automation macro. Reusing the X-propagating
// == walk for === is exact whenever no leaf can ever carry x/z (a string
// leaf never does, LRM 6.16.9): case equality's only behavioral difference
// from logical equality is how x/z compare, so with no x/z possible the two
// operators agree by construction.
TEST_CASE("unpacked struct case-equality with a string member reuses the "
          "X-propagating walk (LRM 11.4.5)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { string s; bit [3:0] b; } s_t;
            s_t x, y;
            initial begin
                x.s = "hi"; x.b = 4'h2;
                y.s = "hi"; y.b = 4'h2;
                $display("ceq=%b cne=%b", x === y, x !== y);
                y.s = "ho";
                $display("ceq=%b cne=%b", x === y, x !== y);
            end
        endmodule
    )") == "ceq=1 cne=0\nceq=0 cne=1\n");
}

// A class-handle leaf is a definite pointer or null, never x/z, so it is
// case-equality-safe by the same reasoning as a string leaf.
TEST_CASE("unpacked struct case-equality with a class-handle member") {
    CHECK(run_capture(R"(
        module m;
            class c; int v; endclass
            typedef struct { c h; int i; } s_t;
            s_t x, y;
            c o;
            initial begin
                o = new; x.h = o; y.h = o; x.i = 5; y.i = 5;
                $display("ceq=%b", x === y);
                y.h = null;
                $display("ceq=%b cne=%b", x === y, x !== y);
            end
        endmodule
    )") == "ceq=1\nceq=0 cne=1\n");
}

// A 4-state leaf CAN carry x/z, where case and logical equality genuinely
// disagree (case compares the bits directly, 12.5; logical X-propagates,
// 11.4.5): reusing the == walk would be wrong, so this stays a located
// refusal rather than a silent wrong answer.
TEST_CASE("struct case-equality with a 4-state leaf is refused, not "
          "silently approximated (LRM 11.4.5)") {
    const LowerResult r = lower_text(R"(
        module m;
            typedef struct { logic [7:0] a; } s_t;
            s_t x, y;
            initial $display("%b", x === y);
        endmodule
    )");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostic_text().find("case equality of an aggregate or container "
                                    "with a 4-state or real leaf is not supported") != std::string::npos);
}

TEST_CASE("unpacked struct equality with a string member") {
    // The string leaf compares by content (strcmp == 0), reduced with the
    // scalar members like any other leaf.
    CHECK(run_capture(R"(
        module m;
            typedef struct { string s; bit [3:0] b; } s_t;
            s_t x, y;
            initial begin
                x.s = "hi"; x.b = 4'h2;
                y.s = "hi"; y.b = 4'h2;
                $display("eq=%b", x == y);
                y.s = "ho";
                $display("eq=%b ne=%b", x == y, x != y);
            end
        endmodule
    )") == "eq=1\neq=0 ne=1\n");
}

// A real member compares through the FLOAT tag. This used to SEGFAULT with no
// diagnostic: the backend's AggEq leaf compare converted the leaf to a real plane
// and then ran `lower_compare(Inst::Tag::Eq, ...)`, the INTEGER compare, which read
// the double's bit pattern as an integer image and walked planes a real plane does
// not carry. Found while adding the aggregate `?:`, whose ambiguous path reaches the
// same node, but it predated that. Verilator agrees on every line below.
TEST_CASE("unpacked struct equality with a real member") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { real r; string s; } s_t;
            typedef struct { real r; int n; } n_t;
            s_t x, y; n_t p, q;
            initial begin
                x = '{r:1.5, s:"a"}; y = '{r:1.5, s:"a"};
                $display("same=%0d", x == y);
                y.r = 2.5; $display("diffreal=%0d", x == y);
                y.r = 1.5; y.s = "b"; $display("diffstr=%0d", x == y);
                p = '{r:0.1, n:3}; q = '{r:0.1, n:3};
                $display("mixed=%0d ne=%0d", p == q, p != q);
                q.r = 0.2; $display("mixeddiff=%0d", p == q);
            end
        endmodule
    )") == "same=1\ndiffreal=0\ndiffstr=0\nmixed=1 ne=0\nmixeddiff=0\n");
}

// A real / string ELEMENT of a fixed array is fine under the same ambiguous
// conditional, because the element-by-element merge compares each element with
// the tag for its own value kind (FEq for a real) instead of going through AggEq.
TEST_CASE("ambiguous conditional over an array of reals or strings") {
    SUBCASE("real elements") {
        CHECK(run_capture(R"(
            module m;
                typedef real ra [0:1];
                initial begin
                    ra p, q, r;
                    logic c;
                    c = 1'bx;
                    p = '{1.5, 2.5};
                    q = '{1.5, 9.5};
                    r = c ? p : q;
                    $display("%p", r);
                end
            endmodule
        )") == "'{1.5, 0}\n");
    }
    SUBCASE("string elements") {
        CHECK(run_capture(R"(
            module m;
                typedef string sa [0:1];
                initial begin
                    sa p, q, r;
                    logic c;
                    c = 1'bx;
                    p = '{"a", "b"};
                    q = '{"a", "z"};
                    r = c ? p : q;
                    $display("%p", r);
                end
            endmodule
        )") == "'{\"a\", \"\"}\n");
    }
}

TEST_CASE("arithmetic on a mixed-domain unpacked struct member") {
    // A 2-state `bit` member beside a 4-state `logic` member: the member read
    // must carry the member's own declared domain, not the aggregate's.
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [7:0] b; } s_t;
            s_t x;
            initial begin
                x.a = 8'd10;
                x.b = 8'd20;
                $display("sum=%0d", x.a + x.b);
            end
        endmodule
    )") == "sum=30\n");
}

TEST_CASE("nested packed struct member inside an unpacked struct") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { logic [3:0] hi; logic [3:0] lo; } byte_t;
            typedef struct { byte_t p; logic [7:0] q; } s_t;
            s_t x;
            initial begin
                x.p.hi = 4'hA; x.p.lo = 4'h5;
                x.q = 8'hCD;
                $display("p=%h q=%h", x.p, x.q);
            end
        endmodule
    )") == "p=a5 q=cd\n");
}

// --- Unpacked struct nonblocking assignment (per-member aggregate shadow) ---
// A struct member commits from its own shadow slot; a whole-struct `<=`
// decomposes to per-member NBA.

TEST_CASE("nonblocking write to unpacked struct members, one cycle latency") {
    // Two clock edges: member NBA reads the pre-edge value and commits after, so
    // after edge 1 a==11 (10+1), and the comb read settles to the committed value.
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x;
            logic clk;
            initial begin
                clk = 0;
                x.a = 8'd10; x.b = 4'd2;
                #1 clk = 1; #1 clk = 0;
                #1 clk = 1; #1 clk = 0; #1;
                $display("a=%0d b=%0d", x.a, x.b);
            end
            always @(posedge clk) begin
                x.a <= x.a + 8'd1;
                x.b <= x.b + 4'd1;
            end
        endmodule
    )") == "a=12 b=4\n");
}

TEST_CASE("whole unpacked struct nonblocking copy has NBA sampling") {
    // y <= x samples x in the active region, so y trails x by one clock.
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x, y;
            logic clk;
            initial begin
                clk = 0;
                x.a = 8'd1; x.b = 4'd0;
                #1 clk = 1; #1 clk = 0;   // x <= 2, y <= 1 (old x)
                #1 clk = 1; #1 clk = 0; #1;
                $display("x=%0d y=%0d", x.a, y.a);
            end
            always @(posedge clk) x.a <= x.a + 8'd1;
            always @(posedge clk) y <= x;
        endmodule
    )") == "x=3 y=2\n");
}

TEST_CASE("combinational reader re-evaluates after a struct-member NBA commit") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t x;
            logic clk;
            logic [7:0] sum;
            always_comb sum = x.a + {4'd0, x.b};
            always @(posedge clk) begin x.a <= 8'd20; x.b <= 4'd5; end
            initial begin
                clk = 0; x.a = 8'd0; x.b = 4'd0;
                #1 clk = 1; #1;
                $display("sum=%0d", sum);
            end
        endmodule
    )") == "sum=25\n");
}

TEST_CASE("partial nonblocking write to a struct member (masked deferred record)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; logic [7:0] b; } s_t;
            s_t x;
            logic clk;
            initial begin
                clk = 0; x.a = 8'hFF; x.b = 8'h00;
                #1 clk = 1; #1 clk = 0; #1;
                $display("%h %h", x.a, x.b);
            end
            always @(posedge clk) begin x.a[3:0] <= 4'h5; x.b[7:4] <= 4'hC; end
        endmodule
    )") == "f5 c0\n");
}

TEST_CASE("partial NBA preserves x in the unwritten bits of a 4-state leaf") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; } s_t;
            s_t x;
            logic clk;
            initial begin
                clk = 0; x.a = 8'hxA;
                #1 clk = 1; #1 clk = 0; #1;
                $display("%h", x.a);
            end
            always @(posedge clk) x.a[3:0] <= 4'h7;
        endmodule
    )") == "x7\n");
}

TEST_CASE("partial NBA through a runtime array index into a struct member") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; } s_t;
            s_t arr [0:3];
            logic clk; logic [1:0] idx;
            initial begin
                clk = 0; idx = 2'd0;
                for (int i = 0; i < 4; i++) arr[i].a = 8'hAA;
                idx = 2'd2; #1 clk = 1; #1 clk = 0; #1;
                $display("%h %h", arr[2].a, arr[0].a);
            end
            always @(posedge clk) arr[idx].a[3:0] <= 4'h3;
        endmodule
    )") == "a3 aa\n");
}

// --- Recursive aggregates: arbitrary nesting (array-of-struct, nested struct,
// struct-with-array-member). Reads + blocking writes. ---

TEST_CASE("array of unpacked struct: runtime-indexed member access") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t arr [0:3];
            initial begin
                for (int i = 0; i < 4; i++) begin
                    arr[i].a = i[7:0];
                    arr[i].b = i[3:0] + 4'd1;
                end
                $display("%h %h %h %h", arr[0].a, arr[2].a, arr[2].b, arr[3].b);
            end
        endmodule
    )") == "00 02 3 4\n");
}

TEST_CASE("nested unpacked struct member access") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } inner_t;
            typedef struct { inner_t inr; logic [7:0] tag; } outer_t;
            outer_t o;
            initial begin
                o.inr.a = 8'h33; o.inr.b = 4'hC; o.tag = 8'h99;
                $display("%h %h %h", o.inr.a, o.inr.b, o.tag);
            end
        endmodule
    )") == "33 c 99\n");
}

TEST_CASE("unpacked struct with an unpacked-array member") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] mem [0:2]; logic v; } s_t;
            s_t w;
            initial begin
                w.mem[0] = 8'h11; w.mem[1] = 8'h77; w.mem[2] = 8'h22; w.v = 1'b1;
                $display("%h %h %h %b", w.mem[0], w.mem[1], w.mem[2], w.v);
            end
        endmodule
    )") == "11 77 22 1\n");
}

TEST_CASE("array of unpacked struct power-on: 4-state members are X") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t arr [0:1];
            initial $display("%h %h", arr[1].a, arr[1].b);
        endmodule
    )") == "xx 0\n");
}

TEST_CASE("two-dimensional array of unpacked struct") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t g [0:1][0:1];
            initial begin
                g[0][1].a = 8'hA1; g[1][0].b = 4'h7;
                $display("%h %h", g[0][1].a, g[1][0].b);
            end
        endmodule
    )") == "a1 7\n");
}

TEST_CASE("nested struct member nonblocking assignment") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; } inner_t;
            typedef struct { inner_t inr; logic [7:0] tag; } outer_t;
            outer_t o;
            logic clk;
            initial begin
                clk = 0; o.inr.a = 8'd5; o.tag = 8'd9;
                #1 clk = 1; #1 clk = 0; #1;
                $display("a=%0d tag=%0d", o.inr.a, o.tag);
            end
            always @(posedge clk) begin o.inr.a <= o.inr.a + 8'd1; o.tag <= 8'd42; end
        endmodule
    )") == "a=6 tag=42\n");
}

TEST_CASE("combinational reader wakes on a struct-array-member NBA commit") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] mem [0:3]; logic [7:0] cnt; } s_t;
            s_t w;
            logic clk;
            initial begin
                clk = 0; w.cnt = 8'd0; w.mem[2] = 8'h55;
                #1 clk = 1; #1 clk = 0; #1;
                $display("%h %0d", w.mem[2], w.cnt);
            end
            always @(posedge clk) w.cnt <= w.cnt + 8'd1;
        endmodule
    )") == "55 1\n");
}

TEST_CASE("whole-object copy of a nested struct is per-leaf") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } inner_t;
            typedef struct { inner_t inr; logic [7:0] tag; } outer_t;
            outer_t x, y;
            initial begin
                x.inr.a = 8'hAA; x.inr.b = 4'h5; x.tag = 8'h33;
                y = x;
                $display("%h %h %h", y.inr.a, y.inr.b, y.tag);
            end
        endmodule
    )") == "aa 5 33\n");
}

TEST_CASE("nested unpacked struct equality (x-propagating)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; } inner_t;
            typedef struct { inner_t i; logic [3:0] t; } outer_t;
            outer_t x, y;
            initial begin
                x.i.a = 8'h5; x.t = 4'h3; y.i.a = 8'h5; y.t = 4'h3;
                $display("%b", x == y);
                y.t = 4'h4;
                $display("%b", x == y);
            end
        endmodule
    )") == "1\n0\n");
}

// --- Deferred nonblocking assignment through a runtime array index into an
// aggregate (arr[i].f <= v, arr[i] <= s, x.mem[i] <= v). ---

TEST_CASE("runtime-indexed register file of structs (deferred NBA)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; logic [7:0] b; } s_t;
            s_t rf [0:3];
            logic clk; logic [1:0] wa;
            initial begin
                clk = 0; wa = 2'd0;
                for (int i = 0; i < 4; i++) begin rf[i].a = 8'd0; rf[i].b = 8'd0; end
                wa = 2'd2; #1 clk = 1; #1 clk = 0;
                wa = 2'd1; #1 clk = 1; #1 clk = 0; #1;
                $display("%0d %0d %0d %0d", rf[0].a, rf[1].a, rf[2].a, rf[3].a);
            end
            always @(posedge clk) rf[wa].a <= rf[wa].a + 8'd7;
        endmodule
    )") == "0 7 7 0\n");
}

TEST_CASE("whole struct-array element nonblocking assignment") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t arr [0:2]; s_t src;
            logic clk;
            initial begin
                clk = 0; src.a = 8'hDD; src.b = 4'h6; arr[1].a = 8'h11; arr[1].b = 4'h1;
                #1 clk = 1; #1 clk = 0; #1;
                $display("%h %h", arr[1].a, arr[1].b);
            end
            always @(posedge clk) arr[1] <= src;
        endmodule
    )") == "dd 6\n");
}

TEST_CASE("runtime-indexed nonblocking write into a struct array member") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] mem [0:3]; logic [7:0] tag; } s_t;
            s_t w;
            logic clk; logic [1:0] idx;
            initial begin
                clk = 0; idx = 2'd0;
                for (int i = 0; i < 4; i++) w.mem[i] = 8'd0;
                w.tag = 8'd9;
                idx = 2'd3; #1 clk = 1; #1 clk = 0; #1;
                $display("%0d %0d %0d", w.mem[3], w.mem[0], w.tag);
            end
            always @(posedge clk) w.mem[idx] <= 8'd88;
        endmodule
    )") == "88 0 9\n");
}

// --- Whole-object blocking copy via a single llvm.memcpy (string-free
// aggregate); the per-leaf path is kept only for string-containing ones. ---

TEST_CASE("whole-object copy of a struct with an array member (memcpy)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] mem [0:3]; logic [7:0] v; } s_t;
            s_t x, y;
            initial begin
                for (int i = 0; i < 4; i++) x.mem[i] = i[7:0] + 8'd10;
                x.v = 8'h99;
                y = x;
                $display("%h %h %h", y.mem[0], y.mem[3], y.v);
            end
        endmodule
    )") == "0a 0d 99\n");
}

TEST_CASE("whole array-of-struct copy (memcpy)") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t p [0:2], q [0:2];
            initial begin
                for (int i = 0; i < 3; i++) begin p[i].a = i[7:0] + 8'd1; p[i].b = i[3:0]; end
                q = p;
                $display("%h %h %h", q[0].a, q[2].a, q[1].b);
            end
        endmodule
    )") == "01 03 1\n");
}

TEST_CASE("copy a struct-array element into a struct variable") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; bit [3:0] b; } s_t;
            s_t arr [0:3]; s_t y;
            initial begin
                for (int i = 0; i < 4; i++) begin arr[i].a = i[7:0] + 8'd5; arr[i].b = i[3:0]; end
                y = arr[2];
                $display("%h %h", y.a, y.b);
            end
        endmodule
    )") == "07 2\n");
}

TEST_CASE("nonblocking whole-object copy over an array member commits deferred") {
    // A struct with an unpacked-array member: `y <= x` decomposes to per-leaf
    // StoreNb (each array element rides the deferred array-NBA queue), so y
    // reads x's OLD value at the NBA commit -- the classic swap idiom leaves y
    // holding x's pre-edge contents. Verified vs verilator (iverilog rejects
    // whole-array NBA).
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] mem [0:3]; logic v; } s_t;
            s_t x, y;
            logic clk = 0;
            int i;
            always #5 clk = ~clk;
            always @(posedge clk) begin
                y <= x;
                x.mem[0] <= x.mem[0] + 8'd1;
            end
            initial begin
                for (i = 0; i < 4; i++) x.mem[i] = i[7:0] + 8'd10;
                x.v = 1;
                @(posedge clk); #1;
                $display("%0d %0d %0d %0d %0d", y.mem[0], y.mem[1], y.mem[2], y.mem[3], y.v);
                $finish;
            end
        endmodule
    )") == "10 11 12 13 1\n");
}

// A whole blocking copy of a plain (string-free) unpacked array shares the
// AggCopy / memcpy path with aggregates.

TEST_CASE("whole plain unpacked array copy is an independent memcpy") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:3], b [0:3];
            initial begin
                for (int i = 0; i < 4; i++) a[i] = i[7:0] + 8'd20;
                b = a;
                a[1] = 8'hFF;
                $display("%h %h %h %h", b[0], b[1], b[3], a[1]);
            end
        endmodule
    )") == "14 15 17 ff\n");
}

TEST_CASE("whole two-dimensional plain array copy") {
    CHECK(run_capture(R"(
        module m;
            logic [7:0] a [0:1][0:1], b [0:1][0:1];
            initial begin
                a[0][0] = 8'hA; a[0][1] = 8'hB; a[1][0] = 8'hC; a[1][1] = 8'hD;
                b = a;
                $display("%h %h %h %h", b[0][0], b[0][1], b[1][0], b[1][1]);
            end
        endmodule
    )") == "0a 0b 0c 0d\n");
}

TEST_CASE("unpacked struct passed by value to a function") {
    // The actual passes by pointer; the callee copies it into its frame, so a
    // mutation of the formal does not touch the caller (LRM 13.5.1 copy-in).
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [7:0] b; } s_t;
            s_t x;
            int s, mret;
            function automatic int sum(s_t v); return v.a + v.b; endfunction
            function automatic int mutate(s_t v); v.a = 999; return v.a; endfunction
            initial begin
                x.a = 5; x.b = 8'd7;
                s = sum(x);
                mret = mutate(x);
                $display("sum=%0d mret=%0d xa=%0d", s, mret, x.a);
            end
        endmodule
    )") == "sum=12 mret=999 xa=5\n");
}

TEST_CASE("unpacked struct returned by value from a function") {
    // The callee writes the result into a caller-provided temp; the assignment
    // copies the temp into the target.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [7:0] b; } s_t;
            s_t q;
            function automatic s_t mk(int a, logic [7:0] b);
                s_t r; r.a = a; r.b = b; return r;
            endfunction
            initial begin
                q = mk(5, 8'd7);
                $display("qa=%0d qb=%0d", q.a, q.b);
            end
        endmodule
    )") == "qa=5 qb=7\n");
}

TEST_CASE("nested unpacked struct passed and returned by value") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [7:0] b; } pair_t;
            typedef struct { pair_t p; int c; } nest_t;
            pair_t q;
            nest_t z;
            function automatic nest_t mknest(pair_t p, int c);
                nest_t n; n.p = p; n.c = c; return n;
            endfunction
            initial begin
                q.a = 5; q.b = 8'd7;
                z = mknest(q, 42);
                $display("%0d %0d %0d", z.p.a, z.p.b, z.c);
            end
        endmodule
    )") == "5 7 42\n");
}

TEST_CASE("array of unpacked struct passed by value (recursive copy-in)") {
    // The whole array-of-struct actual passes by pointer; the callee's frame copy
    // is mutated without touching the caller.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [7:0] b; } pair_t;
            pair_t pa [0:1];
            int s;
            function automatic int sum_arr(pair_t v [0:1]);
                v[0].a = -1;
                return v[0].a + v[1].a + v[0].b + v[1].b;
            endfunction
            initial begin
                pa[0].a = 3; pa[0].b = 8'd4;
                pa[1].a = 5; pa[1].b = 8'd6;
                s = sum_arr(pa);
                $display("s=%0d pa0a=%0d", s, pa[0].a);
            end
        endmodule
    )") == "s=14 pa0a=3\n");
}

TEST_CASE("unpacked struct with an array member returned by value") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int m [0:2]; int c; } arrm_t;
            arrm_t z;
            function automatic arrm_t mk(int base);
                arrm_t r;
                r.m[0] = base; r.m[1] = base+1; r.m[2] = base+2;
                r.c = base*10;
                return r;
            endfunction
            initial begin
                z = mk(7);
                $display("%0d %0d %0d c=%0d", z.m[0], z.m[1], z.m[2], z.c);
            end
        endmodule
    )") == "7 8 9 c=70\n");
}

TEST_CASE("unpacked aggregate task arguments pass by value (LRM 13.5.1)") {
    // A task takes the same one-storage-pointer ABI a function does: the caller
    // passes the actual's storage (a canonical temp for a flat array), the task
    // prologue copies it into its frame Local, and the completion epilogue copies an
    // output / inout Local back. So an input mutation stays local, and an output /
    // inout lands in the caller.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [7:0] b; } s_t;
            typedef int arr_t [0:2];
            s_t   s, s2;
            arr_t q, q2;
            task automatic t_in(s_t v);
                v.a = -1; // the frame copy only
                $display("in a=%0d b=%0d", v.a, v.b);
            endtask
            task automatic t_out(output s_t v);      v.a = 3; v.b = 8'h22; endtask
            task automatic t_inout(inout s_t v);     v.a = v.a * 2; endtask
            task automatic t_arr_in(arr_t v);        $display("arr %0d %0d %0d", v[0], v[1], v[2]); endtask
            task automatic t_arr_out(output arr_t v); v[0] = 1; v[1] = 2; v[2] = 3; endtask
            task automatic t_arr_inout(inout arr_t v); v[2] = v[2] + 100; endtask
            initial begin
                s = '{5, 8'hBB};
                t_in(s);
                $display("kept a=%0d", s.a);
                t_out(s2);   $display("out a=%0d b=%0h", s2.a, s2.b);
                t_inout(s);  $display("inout a=%0d", s.a);
                q = '{4, 5, 6};
                t_arr_in(q);
                t_arr_out(q2);   $display("arr_out %0d %0d %0d", q2[0], q2[1], q2[2]);
                t_arr_inout(q);  $display("arr_inout %0d %0d %0d", q[0], q[1], q[2]);
            end
        endmodule
    )") == "in a=-1 b=187\n"
           "kept a=5\n"
           "out a=3 b=22\n"
           "inout a=10\n"
           "arr 4 5 6\n"
           "arr_out 1 2 3\n"
           "arr_inout 4 5 106\n");
}

TEST_CASE("an aggregate task argument copies out when the task returns, not earlier") {
    // A task may consume time, and 13.5.1 copies output / inout arguments back on
    // return. The copy-out rides the coroutine's completion epilogue, so the caller
    // observes the write only after the task's last delay — and the caller's temp /
    // storage stays live across the suspend. `@0` before, `@5` after.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; } s_t;
            typedef int arr_t [0:1];
            s_t   s;
            arr_t q;
            task automatic t_slow(inout s_t v, inout arr_t w);
                #5;
                v.a  = v.a + 1000;
                w[1] = w[1] + 1000;
            endtask
            initial begin
                s = '{7};
                q = '{1, 2};
                $display("before a=%0d w1=%0d @%0t", s.a, q[1], $time);
                t_slow(s, q);
                $display("after  a=%0d w1=%0d @%0t", s.a, q[1], $time);
            end
        endmodule
    )") == "before a=7 w1=2 @0\n"
           "after  a=1007 w1=1002 @5\n");
}

TEST_CASE("aggregate task arguments nest, recurse, and run concurrently") {
    // One frame per call: a task handing its own inout formal to another task, a
    // recursive call, and two concurrent fork branches each keep their own copies.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int x; } s_t;
            s_t o, r, f1, f2;
            task automatic t_inner(inout s_t v);  v.x = v.x + 1; endtask
            task automatic t_outer(inout s_t v);  #1 t_inner(v); t_inner(v); endtask
            task automatic t_rec(input int n, inout s_t v);
                v.x = v.x + n;
                if (n > 1) t_rec(n - 1, v);
            endtask
            task automatic t_fork(inout s_t v, input int d); #(d) v.x = v.x + d; endtask
            initial begin
                o = '{7};
                t_outer(o);  $display("outer x=%0d @%0t", o.x, $time);
                r = '{0};
                t_rec(4, r); $display("rec x=%0d", r.x);
                f1 = '{10}; f2 = '{20};
                fork
                    t_fork(f1, 5);
                    t_fork(f2, 2);
                join
                $display("fork %0d %0d @%0t", f1.x, f2.x, $time);
            end
        endmodule
    )") == "outer x=9 @1\n"
           "rec x=10\n"
           "fork 15 22 @6\n");
}

TEST_CASE("aggregate task argument shapes: struct-with-array, array-of-struct, 2-D, slice") {
    // Every unpacked shape rides the same recursive storage copy, and a slice actual
    // is a legal whole-array actual (its element count matches the formal).
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [3:0] f [0:1]; } witharr_t;
            typedef struct { int x; } small_t;
            typedef small_t sarr_t [0:1];
            typedef int m2_t [0:1][0:1];
            witharr_t wa;
            sarr_t    sa;
            m2_t      m2;
            int       big [0:4];
            task automatic t_witharr(inout witharr_t v); v.a = v.a + 1; v.f[0] = v.f[0] + 4'h2; endtask
            task automatic t_sarr(inout sarr_t v);       v[0].x = v[0].x * 10; v[1].x = v[1].x * 100; endtask
            task automatic t_m2(inout m2_t v);           v[0][1] = 77; endtask
            task automatic t_slice3(input int v [0:2]);   $display("slice %0d %0d %0d", v[0], v[1], v[2]); endtask
            initial begin
                wa = '{5, '{4'h1, 4'h2}};
                t_witharr(wa); $display("witharr a=%0d f0=%0h f1=%0h", wa.a, wa.f[0], wa.f[1]);
                sa = '{'{2}, '{3}};
                t_sarr(sa);    $display("sarr %0d %0d", sa[0].x, sa[1].x);
                m2 = '{'{1, 2}, '{3, 4}};
                t_m2(m2);      $display("m2 %0d %0d %0d %0d", m2[0][0], m2[0][1], m2[1][0], m2[1][1]);
                big = '{10, 11, 12, 13, 14};
                t_slice3(big[1:3]);
            end
        endmodule
    )") == "witharr a=6 f0=3 f1=2\n"
           "sarr 20 300\n"
           "m2 1 77 3 4\n"
           "slice 11 12 13\n");
}

TEST_CASE("a class method task takes an inout unpacked struct") {
    // The `this` handle is spliced as formal 0 and stays a scalar; the aggregate
    // formal keeps its own storage pointer beside it.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int x; } s_t;
            class C;
                int tag;
                task automatic bump(inout s_t v); v.x = v.x + tag; endtask
            endclass
            C   c;
            s_t v;
            initial begin
                c = new; c.tag = 9;
                v = '{1};
                c.bump(v);
                $display("bump x=%0d", v.x);
            end
        endmodule
    )") == "bump x=10\n");
}

TEST_CASE("an aggregate output argument's copy-out wakes the actual's waiters") {
    // The copy-out has to reach the actual through the ordinary store path, or the
    // signal's wait queue goes unserviced and `always @(s.a)` never runs. So an
    // output / inout aggregate actual always goes through a caller temp: the callee
    // memcpies the temp, and emit_call_copyouts copies the temp into the actual.
    // (Before this, a struct-bearing formal handed the callee the actual's own
    // storage and the wake was silently lost — for functions as well as tasks.)
    // Each shape wakes its waiter once per call.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; } s_t;
            typedef int a2_t [0:1];
            s_t  s;
            a2_t q;
            int  seen_s = 0, seen_q = 0;
            task automatic t_s(output s_t v);  v.a = 42; endtask
            function automatic void f_q(output a2_t v); v[0] = 7; v[1] = 8; endfunction
            always @(s.a)  begin seen_s = seen_s + 1; $display("wake_s a=%0d @%0t", s.a, $time); end
            always @(q[0]) begin seen_q = seen_q + 1; $display("wake_q q0=%0d @%0t", q[0], $time); end
            initial begin
                #1 t_s(s);
                #1 f_q(q);
                #1 $display("seen_s=%0d seen_q=%0d", seen_s, seen_q);
            end
        endmodule
    )") == "wake_s a=42 @1\n"
           "wake_q q0=7 @2\n"
           "seen_s=1 seen_q=1\n");
}

// A string-bearing aggregate now passes by value: the copy is per leaf, so the
// callee's copy is independent of the caller's and neither frees the other's heap.
TEST_CASE("string-bearing aggregate passes to a task by value") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { string s; int x; } sv_t;
            sv_t v;
            task automatic t(sv_t a);
                a.s = "callee";
                $display("%0d [%s]", a.x, a.s);
            endtask
            initial begin
                v.s = "caller";
                v.x = 3;
                t(v);
                $display("[%s]", v.s);
            end
        endmodule
    )") == "3 [callee]\n[caller]\n");
}

// LRM 13.5: "If an argument in the subroutine is declared as an input, then the
// corresponding expression in the subroutine call can be any expression" — a
// non-lvalue actual is materialized in a caller-side temp (13.5.1 copies it into
// the subroutine area anyway). iverilog has no unpacked-struct support at all,
// so every expected value here is cross-checked against verilator 5.050 only.
TEST_CASE("non-lvalue aggregate actuals bound to an input formal") {
    // A positional pattern, a named pattern, a `'{default:}` pattern, a call
    // result, a call chain, a ternary of an lvalue and a pattern, and a task
    // actual (the task ABI shares build_call_args with functions).
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; int b; } s_t;
            function automatic s_t h(); h.a = 1; h.b = 2; endfunction
            function automatic s_t g(s_t i); g.a = i.a + 2; g.b = i.b + 2; endfunction
            function automatic int f(s_t v); return v.a * 10 + v.b; endfunction
            task automatic tk(s_t v); $display("task %0d", v.a); endtask
            s_t x;
            bit c;
            initial begin
                x = '{1, 2};
                c = 0;
                $display("pat %0d", f('{3, 4}));
                $display("named %0d", f('{b:4, a:3}));
                $display("dflt %0d", f('{default:5}));
                $display("call %0d", f(h()));
                $display("chain %0d", f(g(h())));
                $display("tern %0d", f(c ? x : '{3, 4}));
                tk('{11, 0});
            end
        endmodule
    )") == "pat 34\n"
           "named 34\n"
           "dflt 55\n"
           "call 12\n"
           "chain 34\n"
           "tern 34\n"
           "task 11\n");
}

TEST_CASE("a materialized input actual is a copy, evaluated once") {
    // 13.5.1: "If the arguments are changed within the subroutine, the changes
    // are not visible outside" — the callee clobbers its formal and the
    // pattern-element source variable must not move. 13.5's "any expression" is
    // evaluated exactly once: the side-effecting g() feeds one call, and a
    // ternary actual keeps its arms conditional (11.4.11 evaluates only the
    // chosen arm), so the unchosen g() arm bumps nothing.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; int b; } s_t;
            int n = 0;
            int src = 3;
            function automatic s_t g(); n = n + 1; g.a = 3; g.b = 4; endfunction
            function automatic int f(s_t v); v.a = v.a + 100; return v.a + v.b; endfunction
            s_t x;
            initial begin
                x = '{1, 2};
                $display("pat %0d src=%0d", f('{src, 4}), src);
                $display("call %0d n=%0d", f(g()), n);
                $display("tern %0d n=%0d", f(1 ? x : g()), n);
            end
        endmodule
    )") == "pat 107 src=3\n"
           "call 107 n=1\n"
           "tern 103 n=1\n");
}

TEST_CASE("two materialized actuals in one expression do not alias") {
    // Each call site gets its own canonical temp (a fresh frame Local / module
    // signal per make_canonical_agg_temp call), so two live temps in one
    // expression hold distinct values, and a recursive callee whose actual is a
    // pattern over its own formal gets a fresh temp per frame.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; int b; } s_t;
            typedef struct { int a; } r_t;
            function automatic int f(s_t v); return v.a * 10 + v.b; endfunction
            function automatic int fr(r_t v);
                if (v.a <= 0) return 0;
                return v.a + fr('{v.a - 1});
            endfunction
            initial begin
                $display("sum %0d", f('{1, 2}) + f('{3, 4}));
                $display("rec %0d", fr('{4}));
            end
        endmodule
    )") == "sum 46\n"
           "rec 10\n");
}

TEST_CASE("a pattern actual with managed leaves deep-copies per leaf") {
    // A string and a queue member cross the by-value boundary as real copies
    // (emit_agg_store builds the temp per leaf; the callee's copy-in deep-copies
    // the managed leaves), so the callee's push_back cannot grow the caller's
    // queue through the temp.
    CHECK(run_capture(R"(
        module m;
            typedef struct { string s; int q[$]; int a; } sv_t;
            int qq[$];
            function automatic int f(sv_t v);
                v.q.push_back(9);
                $display("in s=%s qs=%0d", v.s, v.q.size());
                return v.a;
            endfunction
            initial begin
                qq.push_back(1); qq.push_back(2);
                $display("r=%0d caller_qs=%0d", f('{"hi", qq, 7}), qq.size());
            end
        endmodule
    )") == "in s=hi qs=3\n"
           "r=7 caller_qs=2\n");
}

TEST_CASE("aggregate subroutine arguments Clotho still refuses loudly") {
    // The non-input directions require an assignable / lvalue actual (LRM 13.5,
    // 13.5.2), and slang rejects those upstream — pinned here so a future guard
    // change cannot let one through. The struct-bearing slice is clotho's own
    // wall: the backend has no aggregate-element slice copy.
    SUBCASE("an assignment-pattern actual on an output formal (slang, LRM 13.5)") {
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { int x; } s_t;
                function automatic int f(output s_t v); v.x = 1; return 0; endfunction
                int o;
                initial o = f('{9});
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("expression is not assignable") != std::string::npos);
    }
    SUBCASE("a call-result actual on a ref formal (slang, LRM 13.5.2)") {
        // Verilator 5.050 accepts this, but 13.5.2 passes "a reference to the
        // original argument" and a function result is not an lvalue home.
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { int x; } s_t;
                function automatic s_t g(); g.x = 5; endfunction
                function automatic int f(ref s_t v); return v.x; endfunction
                int o;
                initial o = f(g());
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("invalid expression for pass by reference") != std::string::npos);
    }
    SUBCASE("a slice of a struct-bearing array actual") {
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { int x; } s_t;
                typedef s_t sa_t [0:1];
                s_t big [0:3];
                function automatic int f(sa_t v); return v[0].x; endfunction
                int o;
                initial o = f(big[1:2]);
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("a slice of a struct-bearing array argument is not supported") !=
              std::string::npos);
    }
}

TEST_CASE("whole unpacked struct as a $display %p operand renders a pattern") {
    // %p expands the struct at lower time into `'{field:val, ...}` (LRM 21.2.1.6);
    // integral leaves render as the clause's unformatted decimal (docs/LRM_NOTES.md, note 32).
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; int b; } s_t;
            s_t x;
            initial begin
                x.a = 8'hAB; x.b = 42;
                $display("%p", x);
            end
        endmodule
    )") == "'{a:171, b:42}\n");
}

TEST_CASE("chained continuous copy of a struct wakes downstream readers") {
    // A whole-object copy (AggCopy memcpy) into a signal must service its Any-edge
    // waiters, so a second `assign` sensitive to the first's target re-evaluates.
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; logic [7:0] b; } p_t;
            p_t x, mid, y;
            assign mid = x;
            assign y = mid;
            initial begin
                x.a = 5; x.b = 8'd9;
                #1 $display("mid=%0d y=%0d %0d", mid.a, y.a, y.b);
            end
        endmodule
    )") == "mid=5 y=5 9\n");
}

TEST_CASE("unpacked struct passed through module ports") {
    // Input and output struct ports lower to whole-object continuous copies.
    CHECK(run_capture(R"(
        typedef struct { int a; logic [7:0] b; } p_t;
        module passthru(input p_t i, output p_t o);
            assign o = i;
        endmodule
        module m;
            p_t x, y;
            passthru u(.i(x), .o(y));
            initial begin
                x.a = 5; x.b = 8'd9;
                #1 $display("y=%0d %0d", y.a, y.b);
                x.a = 7;
                #1 $display("y=%0d", y.a);
            end
        endmodule
    )") == "y=5 9\ny=7\n");
}

TEST_CASE("nested unpacked struct through a module port") {
    CHECK(run_capture(R"(
        typedef struct { int a; logic [7:0] b; } p_t;
        typedef struct { p_t p; int c; } n_t;
        module sub(input n_t i, output int suma);
            assign suma = i.p.a + i.c;
        endmodule
        module m;
            n_t z;
            int s;
            sub v(.i(z), .suma(s));
            initial begin
                z.p.a = 10; z.p.b = 8'd1; z.c = 20;
                #1 $display("s=%0d", s);
            end
        endmodule
    )") == "s=30\n");
}

TEST_CASE("unpacked struct net declaration continuous assignment") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { logic [7:0] a; logic [7:0] b; } p_t;
            p_t x;
            wire p_t n = x;
            initial begin
                x.a = 8'd5; x.b = 8'd9;
                #1 $display("n=%0d %0d", n.a, n.b);
                x.a = 8'd7;
                #1 $display("n=%0d", n.a);
            end
        endmodule
    )") == "n=5 9\nn=7\n");
}

// Member select of an elaboration-constant struct (localparam / parameter,
// LRM 11.2.1). The outermost MemberAccess has no signal to load; it must fold
// via constant evaluation like its ElementSelect/RangeSelect siblings. A
// trailing bit/part select (`P.a[3]`) already folded through those paths — the
// gap was the bare and nested member form.
TEST_CASE("member select of a constant packed struct folds") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { logic [7:0] a; logic [7:0] b; } pair_t;
            localparam pair_t P = '{a: 8'hAB, b: 8'hCD};
            initial $display("%h %h", P.a, P.b);
        endmodule
    )") == "ab cd\n");
}

TEST_CASE("member select of a constant unpacked struct folds") {
    CHECK(run_capture(R"(
        module m;
            typedef struct { int a; int b; } pair_t;
            localparam pair_t P = '{a: 1, b: 2};
            initial $display("%0d %0d", P.a, P.b);
        endmodule
    )") == "1 2\n");
}

TEST_CASE("nested member select of a constant struct folds") {
    CHECK(run_capture(R"(
        module m;
            typedef struct packed { logic [7:0] a; logic [7:0] b; } inner_t;
            typedef struct packed { inner_t x; logic [3:0] y; } outer_t;
            localparam outer_t P = '{x: '{a: 8'h11, b: 8'h22}, y: 4'h3};
            initial $display("%h %h %h", P.x.a, P.x.b, P.y);
        endmodule
    )") == "11 22 3\n");
}

// -- Narrow-slot (2-state / real) aggregate leaves --
//
// A 2-state or real member takes the genuine single-iW leaf slot inside an
// aggregate; every consumer (load/store, NBA shadow + commit, deferred array
// records, copies) must dispatch on the narrow shape. Values hand-checkable.

TEST_CASE("2-state and real members compute and copy through narrow leaf slots") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { bit [15:0] b; int i; real r; logic [7:0] l; } t;
            t a, b2;
            initial begin
                a.b = 16'hBEEF; a.i = -7; a.r = 2.5; a.l = 8'h5A;
                a.i = a.i + 10;
                a.r = a.r * 2.0;
                b2 = a;
                a.b = 0;
                $display("%h %0d %g %h", b2.b, b2.i, b2.r, b2.l);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "beef 3 5 5a\n");
}

TEST_CASE("NBA to a 2-state member commits through the narrow shadow") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int i; logic [7:0] l; } t;
            t s;
            logic clk = 0;
            always @(posedge clk) begin
                s.i <= 41 + 1;
                s.l <= 8'hCD;
            end
            initial begin
                #1 clk = 1;
                #1 $display("%0d %h", s.i, s.l);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "42 cd\n");
}

TEST_CASE("NBA to a narrow leaf under an array index uses the deferred record") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int i; logic [7:0] l; } t;
            t arr [2];
            logic clk = 0;
            always @(posedge clk) begin
                arr[1].i <= 7;
                arr[1].l <= 8'h3C;
            end
            initial begin
                #1 clk = 1;
                #1 $display("%0d %h %0d", arr[1].i, arr[1].l, arr[0].i);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "7 3c 0\n");
}

// Regression: a replicated assignment pattern ('{N{...}}) targeting an
// unpacked struct stores only its per-repeat item list in slang's elements()
// (nelems < the field count) -- emit_agg_store indexed elems[i] once per
// field unconditionally, an out-of-bounds span read past a 1-element list for
// a 3-field struct (SIGSEGV). Fixes sv-tests chapter-5/5.10-structure-replication.
TEST_CASE("replicated assignment pattern into an unpacked struct decl initializer") {
    constexpr std::string_view src = R"(
        module m;
            struct {int X,Y,Z;} XYZ = '{3{1}};
            initial $display("%0d %0d %0d", XYZ.X, XYZ.Y, XYZ.Z);
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 1\n");
}

TEST_CASE("nested replicated patterns into a 2-D array of structs with an array field") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct { int a, b[4]; } ab_t;
            int a, b, c;
            initial begin
                ab_t v1[1:0][2:0];
                a = 10; b = 20; c = 30;
                v1 = '{2{'{3{'{a,'{2{b,c}}}}}}};
                $display("%0d %0d %0d %0d %0d",
                         v1[0][0].a, v1[0][0].b[0], v1[0][0].b[1], v1[0][0].b[2], v1[0][0].b[3]);
                $display("%0d %0d %0d %0d %0d",
                         v1[1][2].a, v1[1][2].b[0], v1[1][2].b[1], v1[1][2].b[2], v1[1][2].b[3]);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "10 20 30 20 30\n10 20 30 20 30\n");
}

// Selecting out of an aggregate function result -- `f().m`, `f()[i]` -- used to be
// refused with an assignment-target message ("unsupported assignment target
// kind=Call") for what is a pure read. `f().m` in particular is an everyday
// SystemVerilog idiom. The select's base is the caller-side out-param temp the
// callee wrote through, and the call is lowered once for the whole select chain, so
// a side-effecting callee runs exactly once per select.
TEST_CASE("member and element selects on an aggregate function result") {
    SUBCASE("struct member select") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; int b; } st;
                function automatic st g(); st s; s.a = 1; s.b = 2; return s; endfunction
                initial $display("%0d %0d", g().a, g().b);
            endmodule
        )") == "1 2\n");
    }
    SUBCASE("array element select") {
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                function automatic ai ga(); ai r; r[0] = 7; r[1] = 8; r[2] = 9; return r; endfunction
                initial $display("%0d %0d", ga()[1], ga()[2]);
            endmodule
        )") == "8 9\n");
    }
    SUBCASE("a nested select reaches through both") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int v; } inner_t;
                typedef inner_t nest_t [0:1];
                function automatic nest_t gn(); nest_t r; r[0].v = 4; r[1].v = 5; return r; endfunction
                initial $display("%0d", gn()[1].v);
            endmodule
        )") == "5\n");
    }
    SUBCASE("the selected call runs exactly once") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; } st;
                int calls = 0;
                function automatic st bump(); calls = calls + 1; return '{calls}; endfunction
                initial begin
                    $display("sel %0d", bump().a);
                    $display("calls %0d", calls);
                end
            endmodule
        )") == "sel 1\ncalls 1\n");
    }
}

// LRM 11.4.11's conditional operator over an aggregate operand. The clause's
// last legality rule puts an unpacked struct / fixed unpacked array here ("For
// all other cases, the type of the first expression and second expression shall
// be equivalent"), and its ambiguous-condition rule is what makes the aggregate
// case differ from the integral one: an array merges element by element, while
// everything else -- a struct included -- takes its whole Table 7-1 default.
//
// Neither simulator serves for the ambiguous half: Verilator returns the
// false arm instead of the clause's default, and iverilog cannot compile an
// unpacked struct at all. So these expectations come from the clause text, not
// from a cross-check (see docs/TODO.md).
TEST_CASE("conditional operator over an unpacked struct") {
    SUBCASE("a definite condition evaluates only its own arm") {
        // "If cond_predicate is true, the operator returns the value of the first
        // expression without evaluating the second expression; if false, it
        // returns the value of the second expression without evaluating the
        // first expression." The counters are the observable half of that.
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                int nt = 0, nf = 0;
                function automatic st ft(); nt++; return '{a:1, b:2}; endfunction
                function automatic st ff(); nf++; return '{a:3, b:4}; endfunction
                initial begin
                    st v;
                    bit c;
                    c = 1;
                    v = c ? ft() : ff();
                    $display("%p nt=%0d nf=%0d", v, nt, nf);
                    c = 0;
                    v = c ? ft() : ff();
                    $display("%p nt=%0d nf=%0d", v, nt, nf);
                end
            endmodule
        )") == "'{a:1, b:2} nt=1 nf=0\n'{a:3, b:4} nt=1 nf=1\n");
    }
    SUBCASE("declaration initializer and return operand positions") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                function automatic st pick(bit c, st x, st y); return c ? x : y; endfunction
                initial begin
                    static st x = '{a:1, b:2};
                    static st y = '{a:3, b:4};
                    bit c;
                    c = 0;
                    begin
                        st d = c ? x : y;
                        $display("%p", d);
                    end
                    $display("%p %p", pick(1, x, y), pick(0, x, y));
                end
            endmodule
        )") == "'{a:3, b:4}\n'{a:1, b:2} '{a:3, b:4}\n");
    }
    SUBCASE("an ambiguous condition with differing arms is the Table 7-1 default") {
        // "For all other data types, the value specified in Table 7-1 for the
        // resulting type as defined previously shall be returned." Both members
        // are 2-state, so Table 7-1 gives '0 for each -- NOT the false arm, which
        // is what Verilator returns here.
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                initial begin
                    static st x = '{a:1, b:2};
                    static st y = '{a:3, b:4};
                    st v;
                    logic c;
                    c = 1'bx;
                    v = c ? x : y;
                    $display("x %p", v);
                    c = 1'bz;
                    v = c ? x : y;
                    $display("z %p", v);
                end
            endmodule
        )") == "x '{a:0, b:0}\nz '{a:0, b:0}\n");
    }
    SUBCASE("an ambiguous condition with equal arms returns that value") {
        // "compared for logical equivalence as described in 11.4.5. If that
        // comparison is true (1), the operator shall return either the first or
        // second expression."
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                initial begin
                    static st x = '{a:1, b:2};
                    st v;
                    logic c;
                    c = 1'bx;
                    v = c ? x : x;
                    $display("%p", v);
                end
            endmodule
        )") == "'{a:1, b:2}\n");
    }
    SUBCASE("an ambiguous condition evaluates BOTH arms") {
        // "then both the first expression and the second expression shall be
        // evaluated" -- unlike a definite condition, this one is required to run
        // each arm exactly once.
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; } st;
                int nt = 0, nf = 0;
                function automatic st ft(); nt++; return '{a:5}; endfunction
                function automatic st ff(); nf++; return '{a:5}; endfunction
                initial begin
                    st v;
                    logic c;
                    c = 1'bx;
                    v = c ? ft() : ff();
                    $display("%p nt=%0d nf=%0d", v, nt, nf);
                end
            endmodule
        )") == "'{a:5} nt=1 nf=1\n");
    }
    SUBCASE("a 4-state member defaults to x, a 2-state one to 0") {
        // Table 7-1 is applied per member: "4-state integral type" -> 'x,
        // "2-state integral type" -> '0.
        CHECK(run_capture(R"(
            module m;
                typedef struct { logic [7:0] w; int a; } st;
                initial begin
                    static st x = '{w:8'hAA, a:1};
                    static st y = '{w:8'h55, a:2};
                    st v;
                    logic c;
                    c = 1'bx;
                    v = c ? x : y;
                    $display("%p", v);
                end
            endmodule
        )") == "'{w:x, a:0}\n");
    }
    SUBCASE("a member with a declaration initializer keeps that value") {
        // Table 7-1, unpacked struct: "each of whose members has the value
        // specified in this table for that member's type, UNLESS the member has an
        // initial assignment as part of its declaration (see 7.2.2), in which case
        // the member's value shall be as given by its initial assignment".
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; int b = 7; string s = "keep"; } st;
                initial begin
                    static st x = '{a:1, b:1, s:"x"};
                    static st y = '{a:2, b:2, s:"y"};
                    st v;
                    logic c;
                    c = 1'bx;
                    v = c ? x : y;
                    $display("%p", v);
                end
            endmodule
        )") == "'{a:0, b:7, s:\"keep\"}\n");
    }
    SUBCASE("the UVM shape: a class handle plus a string") {
        // uvm_factory.svh:2318, which is what this construct was implemented for:
        // `match_original_type ? override.orig : override.ovrd` over
        // `struct { uvm_object_wrapper m_type; string m_type_name; }`. The
        // condition is a `bit`, so no ambiguity machinery is emitted at all.
        CHECK(run_capture(R"(
            module m;
                class wrapper;
                    int id;
                    function new(int i); id = i; endfunction
                endclass
                typedef struct { wrapper m_type; string m_type_name; } pair_t;
                typedef struct { pair_t orig, ovrd; } ovr_t;
                initial begin
                    ovr_t o;
                    pair_t got;
                    bit match;
                    o.orig.m_type = new(1); o.orig.m_type_name = "orig";
                    o.ovrd.m_type = new(2); o.ovrd.m_type_name = "ovrd";
                    match = 1;
                    got = match ? o.orig : o.ovrd;
                    $display("%0d %s", got.m_type.id, got.m_type_name);
                    match = 0;
                    got = match ? o.orig : o.ovrd;
                    $display("%0d %s", got.m_type.id, got.m_type_name);
                end
            endmodule
        )") == "1 orig\n2 ovrd\n");
    }
    SUBCASE("nested rvalue positions") {
        // A conditional in a position that structurally needs an lvalue: a member
        // select of it, a %p operand, an aggregate equality operand. Each stages
        // the result in a canonical aggregate temp (lower_lvalue).
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                initial begin
                    static st x = '{a:1, b:2};
                    static st y = '{a:3, b:4};
                    bit c;
                    c = 0;
                    $display("%0d", (c ? x : y).a);
                    $display("%p", c ? x : y);
                    $display("%0d %0d", (c ? x : y) == y, (c ? x : y) == x);
                end
            endmodule
        )") == "3\n'{a:3, b:4}\n1 0\n");
    }
    SUBCASE("a nested conditional chain") {
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; } st;
                initial begin
                    static st x = '{a:1};
                    static st y = '{a:2};
                    static st z = '{a:3};
                    bit c1, c2;
                    c1 = 0; c2 = 1;
                    $display("%p %0d", c1 ? x : (c2 ? y : z), (c1 ? x : (c2 ? y : z)).a);
                end
            endmodule
        )") == "'{a:2} 2\n");
    }
    SUBCASE("a 2-state condition works even over container members") {
        // A consequence of proving ambiguity impossible for a 2-state condition:
        // no 11.4.5 compare is emitted, so a member shape the compare cannot
        // handle (an associative array) is still assignable this way.
        CHECK(run_capture(R"(
            module m;
                typedef struct { int m[string]; int a; } st;
                initial begin
                    st x, y, v;
                    bit c;
                    c = 1;
                    x.a = 1; x.m["k"] = 11;
                    y.a = 2; y.m["k"] = 22;
                    v = c ? x : y;
                    $display("%0d %0d", v.a, v.m["k"]);
                end
            endmodule
        )") == "1 11\n");
    }
    SUBCASE("an ambiguous condition over an associative member is refused") {
        // The 11.4.5 compare has no ordered element walk for an associative
        // member, so the ambiguous path refuses rather than guessing.
        const LowerResult r = lower_text(R"(
            module m;
                typedef struct { int m[string]; int a; } st;
                initial begin
                    st x, y, v;
                    logic c;
                    c = 1'bx;
                    v = c ? x : y;
                end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("equality of an aggregate with an associative-array member is not supported") !=
              std::string::npos);
    }
    SUBCASE("a conditional over a whole queue is refused") {
        const LowerResult r = lower_text(R"(
            module m;
                initial begin
                    int p[$], q[$], v[$];
                    bit c;
                    c = 1;
                    v = c ? p : q;
                end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("not supported") != std::string::npos);
    }
    SUBCASE("an ambiguous condition over an unpacked-union member is refused") {
        // Table 7-1 returns the union's FIRST member's value, and clotho's flat
        // unpacked-union blob cannot represent that: its domain is the union of
        // every member's, so the fill wrote all-x here and `v.u.w` read back `xx`
        // where the first member (`int`) is 2-state and the table requires '0.
        const LowerResult r = lower_text(R"(
            module m;
                typedef union { int i; logic [7:0] w; } u_t;
                typedef struct { u_t u; int a; } su;
                initial begin
                    su x, y, v;
                    logic c;
                    c = 1'bx;
                    v = c ? x : y;
                end
            endmodule
        )");
        CHECK_FALSE(r.ok);
        CHECK(r.diagnostic_text().find("Table 7-1 default for an unpacked union is not supported") !=
              std::string::npos);
    }
    SUBCASE("a 2-state condition over an unpacked-union member still works") {
        // The refusal above is scoped to the Table 7-1 fill, so a condition that
        // cannot be ambiguous never reaches it.
        CHECK(run_capture(R"(
            module m;
                typedef union { int i; logic [7:0] w; } u_t;
                typedef struct { u_t u; int a; } su;
                initial begin
                    su x, y, v;
                    bit c;
                    c = 1;
                    x.u.i = 7; x.a = 1;
                    y.u.i = 9; y.a = 2;
                    v = c ? x : y;
                    $display("%0d %0d", v.u.i, v.a);
                end
            endmodule
        )") == "7 1\n");
    }
}

// The array half of LRM 11.4.11's ambiguous-condition rule, which is where the
// aggregate case splits: an array merges element by element and keeps the
// elements that match, while a struct (above) defaults as a whole.
TEST_CASE("conditional operator over a fixed unpacked array") {
    SUBCASE("a definite condition selects one arm, unevaluated other arm") {
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                int nt = 0, nf = 0;
                function automatic ai ft(); nt++; return '{1, 2, 3}; endfunction
                function automatic ai ff(); nf++; return '{4, 5, 6}; endfunction
                initial begin
                    ai v;
                    bit c;
                    c = 1;
                    v = c ? ft() : ff();
                    $display("%p nt=%0d nf=%0d", v, nt, nf);
                    c = 0;
                    v = c ? ft() : ff();
                    $display("%p nt=%0d nf=%0d", v, nt, nf);
                end
            endmodule
        )") == "'{1, 2, 3} nt=1 nf=0\n'{4, 5, 6} nt=1 nf=1\n");
    }
    SUBCASE("an ambiguous condition merges element by element") {
        // "For aggregate array data types, except associative arrays, where both
        // expressions contain the same number of elements their results shall be
        // combined element by element. If the elements match, the element shall be
        // returned. If they do not match, then the value specified in Table 7-1
        // for that element's type shall be returned." Positions 0 and 2 match and
        // survive; position 1 differs and takes int's '0.
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                initial begin
                    ai p, q, r;
                    logic c;
                    c = 1'bx;
                    p = '{1, 2, 3};
                    q = '{1, 9, 3};
                    r = c ? p : q;
                    $display("%p", r);
                end
            endmodule
        )") == "'{1, 0, 3}\n");
    }
    SUBCASE("a mismatching aggregate element defaults whole") {
        // The clause names "the value specified in Table 7-1 for that element's
        // type", so a struct element that differs in one member defaults ENTIRELY
        // rather than merging member-wise -- the merge is one level deep.
        CHECK(run_capture(R"(
            module m;
                typedef struct { int a; byte b; } st;
                typedef st sa [0:1];
                initial begin
                    sa p, q, r;
                    logic c;
                    c = 1'bx;
                    p = '{'{a:1, b:2}, '{a:5, b:6}};
                    q = '{'{a:1, b:2}, '{a:5, b:9}};
                    r = c ? p : q;
                    $display("%p", r);
                end
            endmodule
        )") == "'{'{a:1, b:2}, '{a:0, b:0}}\n");
    }
    SUBCASE("an ambiguous condition with equal arms returns that value") {
        CHECK(run_capture(R"(
            module m;
                typedef int ai [0:2];
                initial begin
                    ai p, r;
                    logic c;
                    c = 1'bx;
                    p = '{1, 2, 3};
                    r = c ? p : p;
                    $display("%p", r);
                end
            endmodule
        )") == "'{1, 2, 3}\n");
    }
}
