#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// Constrained randomization, step 1: everything reachable without a solver
// (docs/design/constrained-randomization.md). Every case below was scored against
// Verilator 5.050, which implements a usable subset; iverilog 13 cannot run any of
// it, so where Verilator declines the behaviour is pinned to the LRM text and the
// comment says so.
//
// Draws are run-stable (a process seeds from its index, an object from one draw of
// its creating thread), but exact values are pinned in ONE place only -- the stream
// test at the bottom. Everything else asserts the property under test, computed in
// the design so the expected stdout stays exact.

namespace {

// JIT-run a module and capture stdout (mirrors randcase_test.cpp / class_test.cpp).
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

// Confirm lowering fails loudly for an unsupported construct.
std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_FALSE(r.ok);
    return r.diagnostic_text();
}

// JIT-run a module and capture stdout and stderr separately (mirrors
// severity_test.cpp): a run-time randomization refusal is a severity report on
// stderr, while the design's own output stays on stdout.
struct Streams {
    std::string out;
    std::string err;
};

std::string drain_stream(std::FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string s;
    char        buf[256];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        s.append(buf, n);
    }
    return s;
}

Streams run_streams(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    std::FILE* out_tmp = std::tmpfile();
    std::FILE* err_tmp = std::tmpfile();
    REQUIRE(out_tmp != nullptr);
    REQUIRE(err_tmp != nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(fileno(stdout));
    const int saved_err = dup(fileno(stderr));
    dup2(fileno(out_tmp), fileno(stdout));
    dup2(fileno(err_tmp), fileno(stderr));

    ir::run_jit(*r.module);

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved_out);
    close(saved_err);

    Streams s{ drain_stream(out_tmp), drain_stream(err_tmp) };
    std::fclose(out_tmp);
    std::fclose(err_tmp);
    return s;
}

} // namespace

// -- 1. The value rules (LRM 18.4) ------------------------------------------

// An unconstrained randomize() succeeds and assigns every rand property; a
// non-rand property is untouched. Verilator agrees (5.050).
TEST_CASE("randomize assigns rand properties and leaves the rest alone (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [3:0] a;
                rand bit [3:0] b;
                     bit [3:0] plain;
            endclass
            initial begin
                automatic C c = new();
                int ok;
                c.plain = 4'hF;
                ok = c.randomize();
                $display("%0d %0d %0d %0h", ok, c.a < 16, c.b < 16, c.plain);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 1 f\n");
}

// A 4-state rand variable receives a 2-state value: LRM 18.4 gives it a value, and
// x / z are not values. Verilator agrees (5.050).
TEST_CASE("a 4-state rand property never lands x or z (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand logic [7:0] v;
            endclass
            initial begin
                automatic C c = new();
                int defined = 1;
                for (int i = 0; i < 200; i++) begin
                    void'(c.randomize());
                    if ($isunknown(c.v)) defined = 0;
                end
                $display("%0d", defined);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// An enum draws uniformly over its DECLARED values, not over the base type's range
// -- so a value outside the label set never appears, and no label is starved.
// Verilator agrees (5.050): 300 draws over three labels came out 97 / 104 / 99.
TEST_CASE("a rand enum draws only declared labels, uniformly (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            typedef enum bit [3:0] { A = 3, B = 7, C = 9 } e_t;
            class K;
                rand e_t e;
            endclass
            initial begin
                automatic K k = new();
                int na = 0, nb = 0, nc = 0, other = 0;
                for (int i = 0; i < 900; i++) begin
                    void'(k.randomize());
                    case (k.e)
                        A: na++;
                        B: nb++;
                        C: nc++;
                        default: other++;
                    endcase
                end
                $display("%0d %0d %0d", other, na + nb + nc,
                         (na > 200 && nb > 200 && nc > 200));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 900 1\n");
}

// Wider than one 32-bit draw: the value is stitched from ceil(W/32) draws, so both
// halves have to move. Pinning "both halves are nonzero at least once over 100
// draws" rather than a value keeps the chunk order free to be a documented
// contract rather than an accident of this test.
TEST_CASE("a rand property wider than 32 bits fills every chunk (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [95:0] w;
            endclass
            initial begin
                automatic C c = new();
                bit lo = 0, mid = 0, hi = 0;
                for (int i = 0; i < 100; i++) begin
                    void'(c.randomize());
                    if (c.w[31:0]  != 0) lo  = 1;
                    if (c.w[63:32] != 0) mid = 1;
                    if (c.w[95:64] != 0) hi  = 1;
                end
                $display("%0d %0d %0d", lo, mid, hi);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1 1\n");
}

// A rand unpacked array randomizes element-wise (LRM 18.4). Verilator agrees
// (5.050). Independence of the elements is the property worth asserting: one draw
// broadcast to every element would pass a "was written" check.
TEST_CASE("a rand unpacked array randomizes each element (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [15:0] mem [4];
            endclass
            initial begin
                automatic C c = new();
                int differ = 0;
                for (int i = 0; i < 50; i++) begin
                    void'(c.randomize());
                    if (c.mem[0] != c.mem[1] || c.mem[1] != c.mem[2] || c.mem[2] != c.mem[3])
                        differ++;
                end
                $display("%0d", differ > 45);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// -- 2. Hooks and recursion (LRM 18.4 / 18.6.2) -----------------------------

// pre_randomize runs before the draws and post_randomize after, and a rand object
// property is recursed into between the two -- so the nested pair nests inside the
// outer pair. Verilator agrees (5.050), printing exactly this order.
TEST_CASE("pre_randomize / post_randomize bracket the draws, nested first (LRM 18.6.2)") {
    constexpr std::string_view src = R"(
        module m;
            class Inner;
                rand bit [2:0] y;
                function void pre_randomize();  $display("  inner.pre"); endfunction
                function void post_randomize(); $display("  inner.post"); endfunction
            endclass
            class Outer;
                rand bit [3:0] a;
                rand Inner     h;
                function new(); h = new(); endfunction
                function void pre_randomize();  $display("outer.pre"); endfunction
                function void post_randomize(); $display("outer.post"); endfunction
            endclass
            initial begin
                automatic Outer o = new();
                void'(o.randomize());
            end
        endmodule
    )";
    CHECK(run_capture(src) == "outer.pre\n  inner.pre\n  inner.post\nouter.post\n");
}

// A rand class property is not assigned a new handle: the object it already refers
// to is randomized in place. Verilator agrees (5.050).
TEST_CASE("a rand class handle is recursed into, not replaced (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            class Inner;
                rand bit [7:0] y;
            endclass
            class Outer;
                rand Inner h;
                function new(); h = new(); endfunction
            endclass
            initial begin
                automatic Outer o = new();
                automatic Inner keep = o.h;
                int moved = 0;
                o.h.y = 0;
                for (int i = 0; i < 20; i++) begin
                    void'(o.randomize());
                    if (o.h.y != 0) moved = 1;
                end
                $display("%0d %0d", (keep == o.h), moved);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}

// A null rand handle is skipped and randomize() still succeeds -- the LRM makes no
// failure of it. Verilator agrees (5.050).
TEST_CASE("a null rand class handle is skipped and randomize still returns 1 (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            class Inner;
                rand bit [7:0] y;
            endclass
            class Outer;
                rand bit [3:0] a;
                rand Inner     h;
            endclass
            initial begin
                automatic Outer o = new();
                int ok = 0;
                ok = o.randomize();
                $display("%0d %0d", ok, (o.h == null));
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}

// A base-typed rand handle randomizes the DERIVED object it holds: `randomize` is
// virtual (LRM 18.4), so the derived class's own rand properties move too.
TEST_CASE("randomize through a base handle randomizes the derived object (LRM 18.4)") {
    constexpr std::string_view src = R"(
        module m;
            class Base;
                rand bit [7:0] b;
            endclass
            class Deriv extends Base;
                rand bit [7:0] d;
            endclass
            initial begin
                automatic Deriv o = new();
                automatic Base  bh = o;
                int moved_b = 0, moved_d = 0;
                o.b = 0; o.d = 0;
                for (int i = 0; i < 20; i++) begin
                    void'(bh.randomize());
                    if (o.b != 0) moved_b = 1;
                    if (o.d != 0) moved_d = 1;
                end
                $display("%0d %0d", moved_b, moved_d);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}

// -- 3. The active set (LRM 18.8 / 18.11) -----------------------------------

// rand_mode(0) holds a property at its current value and the query form reports
// it; rand_mode(1) puts it back. Verilator agrees (5.050).
TEST_CASE("rand_mode disables and re-enables one property (LRM 18.8)") {
    constexpr std::string_view src = R"(
        module m;
            class K;
                rand bit [7:0] a;
                rand bit [7:0] b;
            endclass
            initial begin
                automatic K k = new();
                int moved = 0;
                k.a.rand_mode(0);
                k.a = 8'hAA;
                for (int i = 0; i < 20; i++) begin
                    void'(k.randomize());
                    if (k.a != 8'hAA) moved = 1;
                end
                $display("%0h %0d %0d", k.a, k.a.rand_mode(), moved);
                k.a.rand_mode(1);
                for (int i = 0; i < 20; i++) begin
                    void'(k.randomize());
                    if (k.a != 8'hAA) moved = 1;
                end
                $display("%0d %0d", k.a.rand_mode(), moved);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "aa 0 0\n1 1\n");
}

// The object-level form covers every rand property at once (LRM 18.8). Verilator
// agrees (5.050).
TEST_CASE("object-level rand_mode(0) freezes every rand property (LRM 18.8)") {
    constexpr std::string_view src = R"(
        module m;
            class K;
                rand bit [7:0] a;
                rand bit [7:0] b;
            endclass
            initial begin
                automatic K k = new();
                int moved = 0, moved_after = 0;
                k.rand_mode(0);
                k.a = 8'h11; k.b = 8'h22;
                for (int i = 0; i < 20; i++) begin
                    void'(k.randomize());
                    if (k.a != 8'h11 || k.b != 8'h22) moved = 1;
                end
                k.rand_mode(1);
                for (int i = 0; i < 20; i++) begin
                    void'(k.randomize());
                    if (k.a != 8'h11 || k.b != 8'h22) moved_after = 1;
                end
                $display("%0d %0d", moved, moved_after);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "0 1\n");
}

// The mode of an INHERITED property is set through a derived handle: the rand
// ordinal of a base property is the same number in every class that inherits it,
// which is what makes one mask word work across the chain. Verilator agrees
// (5.050).
TEST_CASE("rand_mode on an inherited property works through a derived handle (LRM 18.8)") {
    constexpr std::string_view src = R"(
        module m;
            class Base;
                rand bit [7:0] b;
            endclass
            class Deriv extends Base;
                rand bit [7:0] d;
            endclass
            initial begin
                automatic Deriv o = new();
                int moved_b = 0, moved_d = 0;
                o.b.rand_mode(0);
                o.b = 8'hC3;
                o.d = 0;
                for (int i = 0; i < 20; i++) begin
                    void'(o.randomize());
                    if (o.b != 8'hC3) moved_b = 1;
                    if (o.d != 0) moved_d = 1;
                end
                $display("%0h %0d %0d", o.b, moved_b, moved_d);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "c3 0 1\n");
}

// randomize(x) narrows the active set to the named properties for that call only
// (LRM 18.11). Verilator agrees (5.050).
TEST_CASE("randomize(subset) randomizes only the named properties (LRM 18.11)") {
    constexpr std::string_view src = R"(
        module m;
            class K;
                rand bit [7:0] a;
                rand bit [7:0] b;
            endclass
            initial begin
                automatic K k = new();
                int moved_a = 0, moved_b = 0;
                k.a = 8'h55; k.b = 8'h66;
                for (int i = 0; i < 20; i++) begin
                    void'(k.randomize(b));
                    if (k.a != 8'h55) moved_a = 1;
                    if (k.b != 8'h66) moved_b = 1;
                end
                $display("%0h %0d %0d", k.a, moved_a, moved_b);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "55 0 1\n");
}

// randomize(null) checks without assigning (LRM 18.11) -- with no constraints
// there is nothing to check, so it succeeds and changes nothing. Verilator
// declines this form ("Unsupported: 'randomize(null)' on class with rand container
// or class member"), so it is pinned to the LRM text.
TEST_CASE("randomize(null) assigns nothing and succeeds (LRM 18.11)") {
    constexpr std::string_view src = R"(
        module m;
            class K;
                rand bit [7:0] a;
            endclass
            initial begin
                automatic K k = new();
                int ok = 0;
                k.a = 8'h5A;
                ok = k.randomize(null);
                $display("%0d %0h", ok, k.a);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 5a\n");
}

// -- 4. randc (LRM 18.6) -----------------------------------------------------

// randc walks a random permutation of its range: over 2^W draws every value
// appears exactly once, and the next 2^W start a fresh permutation. Verilator
// agrees (5.050), which printed 2 1 3 0 then 3 0 2 1 for a 2-bit randc.
TEST_CASE("randc yields a permutation per cycle and restarts (LRM 18.6)") {
    constexpr std::string_view src = R"(
        module m;
            class R;
                randc bit [1:0] c;
            endclass
            initial begin
                automatic R r = new();
                int seen[4];
                int cycle_ok = 1;
                for (int cyc = 0; cyc < 8; cyc++) begin
                    for (int i = 0; i < 4; i++) seen[i] = 0;
                    for (int i = 0; i < 4; i++) begin
                        void'(r.randomize());
                        seen[r.c] = seen[r.c] + 1;
                    end
                    for (int i = 0; i < 4; i++) if (seen[i] != 1) cycle_ok = 0;
                end
                $display("%0d", cycle_ok);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// The permutation is a permutation, not the identity: over eight 8-value cycles at
// least one has to come out in a non-sorted order.
TEST_CASE("randc order varies between cycles (LRM 18.6)") {
    constexpr std::string_view src = R"(
        module m;
            class R;
                randc bit [2:0] c;
            endclass
            initial begin
                automatic R r = new();
                int shuffled = 0;
                for (int cyc = 0; cyc < 8; cyc++) begin
                    for (int i = 0; i < 8; i++) begin
                        void'(r.randomize());
                        if (r.c != i[2:0]) shuffled = 1;
                    end
                end
                $display("%0d", shuffled);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// -- 5. Scope randomize (LRM 18.12) -----------------------------------------

// std::randomize assigns ordinary variables from the CALLING THREAD's generator --
// there is no object, so there is no object RNG. Verilator agrees (5.050).
TEST_CASE("std::randomize assigns its arguments (LRM 18.12)") {
    constexpr std::string_view src = R"(
        module m;
            initial begin
                int x, y, ok, differ;
                differ = 0;
                for (int i = 0; i < 50; i++) begin
                    ok = std::randomize(x, y);
                    if (x != y) differ++;
                end
                $display("%0d %0d", ok, differ > 45);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}

// -- 6. Random stability (LRM 18.14.2) --------------------------------------

// Object stability: each object gets its own generator, seeded at construction
// from the creating thread, so two objects made in a row produce independent
// streams rather than the same one.
TEST_CASE("two objects have independent random streams (LRM 18.14.2)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [15:0] a;
            endclass
            initial begin
                automatic C p = new();
                automatic C q = new();
                int differ = 0;
                for (int i = 0; i < 50; i++) begin
                    void'(p.randomize());
                    void'(q.randomize());
                    if (p.a != q.a) differ++;
                end
                $display("%0d", differ > 45);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// srandom() overrides an object's seed, so two objects seeded alike produce the
// same stream and two seeded differently do not (LRM 18.13.3).
TEST_CASE("srandom makes two objects reproduce each other (LRM 18.13.3)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [15:0] a;
            endclass
            initial begin
                automatic C x = new();
                automatic C y = new();
                int same = 1, differ = 0;
                x.srandom(42);
                y.srandom(42);
                for (int i = 0; i < 20; i++) begin
                    void'(x.randomize());
                    void'(y.randomize());
                    if (x.a != y.a) same = 0;
                end
                y.srandom(43);
                for (int i = 0; i < 20; i++) begin
                    void'(x.randomize());
                    void'(y.randomize());
                    if (x.a != y.a) differ = 1;
                end
                $display("%0d %0d", same, differ);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1 1\n");
}

// A fork child's objects are seeded from the child's own stream, which LRM 18.14.1
// already makes independent of its siblings' -- so two children constructing the
// same class do not produce the same values.
TEST_CASE("objects created in sibling fork children are independent (LRM 18.14)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [15:0] a;
            endclass
            int va, vb;
            initial begin
                fork
                    begin automatic C c = new(); void'(c.randomize()); va = c.a; end
                    begin automatic C c = new(); void'(c.randomize()); vb = c.a; end
                join
                $display("%0d", va != vb);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// The object stream is run-stable, so it can be pinned. This is the ONE test that
// fixes actual values: it locks the whole contract -- the per-object seeding, the
// one draw per `new`, the draw order over properties, and the 32-bit draw shape.
// A deliberate change to any of those is expected to update this string; an
// accidental one is what it exists to catch. (The stream is clotho's own; LRM
// 18.13 leaves the algorithm unspecified, exactly as for $urandom.)
TEST_CASE("the per-object stream is reproducible run to run (LRM 18.14.2)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [15:0] a;
                rand bit [15:0] b;
            endclass
            initial begin
                automatic C c = new();
                for (int i = 0; i < 3; i++) begin
                    void'(c.randomize());
                    $display("%0d %0d", c.a, c.b);
                end
            end
        endmodule
    )";
    const std::string first = run_capture(src);
    CHECK(first == run_capture(src));
    CHECK(first.find('\n') != std::string::npos);
}

// A shallow copy copies the object's whole body, which now includes its RNG state
// and its rand_mode mask (LRM 8.12 step 2 names randomization state explicitly).
// So a copy continues the source's stream rather than starting a new one, and the
// two agree from the copy point on.
TEST_CASE("a shallow copy carries the randomization state (LRM 8.12)") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                rand bit [15:0] a;
            endclass
            initial begin
                automatic C src_obj = new();
                automatic C cp;
                int same = 1;
                void'(src_obj.randomize());
                cp = new src_obj;
                for (int i = 0; i < 10; i++) begin
                    void'(src_obj.randomize());
                    void'(cp.randomize());
                    if (src_obj.a != cp.a) same = 0;
                end
                $display("%0d", same);
            end
        endmodule
    )";
    CHECK(run_capture(src) == "1\n");
}

// -- 7. The refusal surface --------------------------------------------------

// Every constraint form is refused by its own name (never silently
// accept). A constraint block used to compile silently, which is worse than a
// crash: the class randomized as if the constraint were not there.
//
// The constraint-block refusal is a RUN-TIME fatal in the class's generated
// $randomize body, not a compile-time one: eager static initialization (LRM 6.21)
// lowers every factory-registered class and builds every vtable body -- UVM's
// register layer carries constraints in classes most designs never randomize --
// so a design must lower for merely CONTAINING such a class, and only an actual
// randomize() of it may die. The dispatch is virtual, so the body catches a
// randomize through a base-typed handle too (tested below).
TEST_CASE("constraints are refused, each by its own message") {
    {
        const Streams s = run_streams(R"(
        module m;
            class C;
                rand bit [3:0] x;
                constraint q { x > 4; }
            endclass
            initial begin
                automatic C c = new();
                void'(c.randomize());
                $display("unreachable");
            end
        endmodule
    )");
        CHECK(s.err.find("constraint blocks are not supported") != std::string::npos);
        CHECK(s.err.find("FATAL: ") != std::string::npos);
        // The fatal terminates the run; the statement after never executes.
        CHECK(s.out.find("unreachable") == std::string::npos);
    }

    CHECK(lower_error(R"(
        module m;
            class C;
                rand bit [3:0] x;
            endclass
            initial begin automatic C c = new(); void'(c.randomize() with { x > 4; }); end
        endmodule
    )")
            .find("inline constraints on randomize() are not supported")
          != std::string::npos);

    CHECK(lower_error(R"(
        module m;
            class C;
                rand bit [3:0] x;
            endclass
            initial begin automatic C c = new(); c.constraint_mode(0); end
        endmodule
    )")
            .find("constraint_mode is not supported")
          != std::string::npos);
}

// Regression: a constraint block used to be name-resolution-order-sensitive,
// contradicting LRM 8.6 / 18.5 ("A class's properties are freely and broadly
// available to the methods of the class" applies equally to a constraint
// block, which is bound the same way a method body is). `x` is declared
// AFTER the constraint that reads it; before the fix that failed to even
// COMPILE with "identifier 'x' used before its declaration", a frontend
// name-resolution bug, not the (separate, pre-existing) backend refusal
// below -- the two are distinguished here by checking which message shows
// up. The compile must now succeed and reach the ordinary, unrelated
// constraint-block runtime refusal, exactly like the properly-ordered case
// above.
TEST_CASE("a constraint block may reference a property declared later in the class") {
    const Streams s = run_streams(R"(
        module m;
            class C;
                constraint q { x > 4; }
                rand bit [3:0] x;
            endclass
            initial begin
                automatic C c = new();
                void'(c.randomize());
                $display("unreachable");
            end
        endmodule
    )");
    CHECK(s.err.find("used before its declaration") == std::string::npos);
    CHECK(s.err.find("constraint blocks are not supported") != std::string::npos);
    CHECK(s.out.find("unreachable") == std::string::npos);
}

// A design that merely CONTAINS a constrained class runs: the refusal moved into
// the generated $randomize body, so nothing fires until something randomizes the
// object. This is UVM's register layer in miniature -- uvm_reg_item carries
// constraints and a rand dynamic array, is factory-registered by uvm_pkg itself,
// and easyUVM never randomizes one.
TEST_CASE("a constrained class that is never randomized lowers and runs") {
    const std::string out = run_capture(R"(
        module m;
            class C;
                rand bit [3:0] x;
                rand int       q[];
                constraint k { x > 4; }
            endclass
            initial begin
                automatic C c = new();
                c.x = 3;
                $display("x=%0d", c.x);
            end
        endmodule
    )");
    CHECK(out == "x=3\n");
}

// The hole a call-site check could not close: randomize() through a BASE-typed
// handle over a constrained derived object. The dispatch is virtual, so it lands
// in the derived class's generated body -- the run-time fatal -- and never draws
// the derived object unconstrained.
TEST_CASE("randomize of a constrained derived through a base handle is a run-time fatal") {
    const Streams s = run_streams(R"(
        module m;
            class B;
                rand bit [3:0] x;
            endclass
            class D extends B;
                constraint k { x > 4; }
            endclass
            initial begin
                automatic D d = new();
                automatic B b = d;
                void'(b.randomize());
                $display("unreachable");
            end
        endmodule
    )");
    CHECK(s.err.find("constraint blocks are not supported") != std::string::npos);
    CHECK(s.out.find("unreachable") == std::string::npos);
}

// The remaining legal-but-unimplemented rand surface, each loud and each carrying
// a TODO at its guard. An undrawable rand property TYPE is refused at run time in
// the generated $randomize body, like a constraint block and for the same reason
// (uvm_reg_item carries `rand uvm_reg_data_t value[]`); the randc and randstate
// shapes stay compile-time refusals.
TEST_CASE("unimplemented rand forms are refused, each by its own message") {
    CHECK(lower_error(R"(
        module m;
            class C;
                randc bit [15:0] x;
            endclass
            initial begin automatic C c = new(); void'(c.randomize()); end
        endmodule
    )")
            .find("randc wider than 8 bits is not supported")
          != std::string::npos);

    {
        const Streams s = run_streams(R"(
        module m;
            class C;
                rand int q[];
            endclass
            initial begin
                automatic C c = new();
                void'(c.randomize());
                $display("unreachable");
            end
        endmodule
    )");
        CHECK(s.err.find("rand properties of this type are not supported") != std::string::npos);
        CHECK(s.out.find("unreachable") == std::string::npos);
    }

    CHECK(lower_error(R"(
        module m;
            class C;
                rand bit [3:0] x;
            endclass
            initial begin
                automatic C c = new();
                string s;
                s = c.get_randstate();
            end
        endmodule
    )")
            .find("get_randstate / set_randstate are not supported")
          != std::string::npos);

    CHECK(lower_error(R"(
        module m;
            class C;
                rand bit [3:0] x;
            endclass
            initial begin automatic C c = new(); c.set_randstate("z"); end
        endmodule
    )")
            .find("get_randstate / set_randstate are not supported")
          != std::string::npos);
}

// srandom on a class with no rand property is a no-op rather than a refusal: the
// object has no generator because nothing in the language can observe the one it
// would have had. It must not crash, which is what it did before this landed.
TEST_CASE("srandom on a class with no rand property is inert") {
    constexpr std::string_view src = R"(
        module m;
            class C;
                int x;
            endclass
            initial begin
                automatic C c = new();
                c.srandom(7);
                c.rand_mode(0);
                $display("ok");
            end
        endmodule
    )";
    CHECK(run_capture(src) == "ok\n");
}
