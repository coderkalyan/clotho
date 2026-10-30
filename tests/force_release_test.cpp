#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "Lower.h"
#include "Subprocess.h"
#include "ir/Jit.h"

using namespace clotho;

namespace {

// JIT-run a design that ends via $finish and return its stdout. The simulation
// gets a process of its own (Subprocess.h explains why $finish cannot share
// the runner's); lowering here as well is what reports a broken source.
std::string run_capture_finish(std::string_view source) {
    const LowerResult r = lower_text(source);
    REQUIRE_MESSAGE(r.ok, r.diagnostic_text());
    REQUIRE(r.module != nullptr);

    return test::run_design_subprocess(source);
}

std::string lower_error(std::string_view source) {
    const LowerResult r = lower_text(source);
    return r.ok ? std::string{} : r.diagnostic_text();
}

} // namespace

// LRM 10.6.2: a force overrides procedural assignment; on release a variable
// holds its forced value until the next procedural write. Byte-identical to
// iverilog 13 (t=1 a=10, t=2 a=99, t=3 a=99 (write suppressed), t=4 a=99 (held
// through release), t=5 a=7).
TEST_CASE("force/release: override, suppression, and release-hold") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial begin
                a = 8'd10;
                #1 $display("t=%0t a=%0d", $time, a);
                force a = 8'd99;
                #1 $display("t=%0t a=%0d", $time, a);
                a = 8'd5;
                #1 $display("t=%0t a=%0d", $time, a);
                release a;
                #1 $display("t=%0t a=%0d", $time, a);
                a = 8'd7;
                #1 $display("t=%0t a=%0d", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) ==
          "t=1 a=10\nt=2 a=99\nt=3 a=99\nt=4 a=99\nt=5 a=7\n");
}

// A force RHS is a continuous relationship (LRM 10.6.2): `force a = b` tracks
// every later change of b. Matches iverilog 13.
TEST_CASE("force: RHS is continuously tracked") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                b = 8'd20; a = 8'd1;
                #1 force a = b;
                #1 $display("a=%0d", a);
                b = 8'd50;
                #1 $display("a=%0d", a);
                b = 8'd200;
                #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=20\na=50\na=200\n");
}

// Re-force is last-wins (LRM 10.6.2): a second force to the same variable
// supersedes the first, breaking its continuous relationship
// (docs/LRM_NOTES.md, note 10). iverilog 13 diverges here (it keeps the first
// force's RHS link live, so a later change to that RHS leaks through).
TEST_CASE("force: re-force is last-wins, old RHS link broken") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                b = 8'd20; a = 8'd1;
                #1 force a = b;
                #1 $display("a=%0d", a);
                force a = 8'd77;
                b = 8'd99;
                #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=20\na=77\n");
}

// A force overrides a nonblocking (clocked) driver; on release the NBA driver
// resumes. Matches iverilog 13.
TEST_CASE("force: suppresses an NBA clocked driver, resumes on release") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] q = 0;
            logic clk = 0;
            always #1 clk = ~clk;
            always @(posedge clk) q <= q + 1;
            initial begin
                #3 force q = 8'hAA;
                #10 $display("q=%0d", q);
                release q;
                #4 $display("q=%0d", q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "q=170\nq=172\n");
}

// Two independent forced variables do not interfere.
TEST_CASE("force: independent targets") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                a = 1; b = 2;
                force a = 8'd10;
                force b = 8'd20;
                #1 $display("a=%0d b=%0d", a, b);
                release a;
                a = 8'd3;
                #1 $display("a=%0d b=%0d", a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=10 b=20\na=3 b=20\n");
}

// release of a variable never forced is a legal no-op (LRM 10.6.2).
TEST_CASE("release: never-forced variable is a no-op") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial begin
                a = 8'd5;
                release a;
                #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=5\n");
}

// LRM 10.6: the full procedural continuous-assignment priority ladder on a
// variable — procedural, then `assign` (overrides procedural), then `force`
// (overrides assign), `release` (reverts to the active assign), `deassign`
// (holds), procedural resumes. Byte-identical to iverilog 13.
TEST_CASE("assign/force/release/deassign: full priority ladder") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                a = 8'd1; b = 8'd10;
                #1 a = 8'd2;    $display("a=%0d", a);
                assign a = b;
                #1 $display("a=%0d", a);
                a = 8'd99;
                #1 $display("a=%0d", a);
                b = 8'd20; #1 $display("a=%0d", a);
                force a = 8'd77;
                b = 8'd30; #1 $display("a=%0d", a);
                release a;
                #1 $display("a=%0d", a);
                deassign a;
                #1 $display("a=%0d", a);
                a = 8'd5; #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) ==
          "a=2\na=10\na=10\na=20\na=77\na=30\na=30\na=5\n");
}

// A procedural continuous `assign` is a continuous relationship and is last-wins:
// a second `assign` supersedes the first. Matches iverilog 13.
TEST_CASE("assign: continuous tracking and last-wins") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b, c;
            initial begin
                b = 8'd10; c = 8'd50;
                assign a = b;
                #1 $display("a=%0d", a);
                b = 8'd11; #1 $display("a=%0d", a);
                assign a = c;
                #1 $display("a=%0d", a);
                b = 8'd12; #1 $display("a=%0d", a);
                c = 8'd55; #1 $display("a=%0d", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=10\na=11\na=50\na=50\na=55\n");
}

// LRM 10.6.2: a force on a net overrides all its drivers (here a gate) and the
// RHS is a continuous relationship; on release the net immediately reverts to
// the driver-resolved value. This is the standard's own `and`-gate-patched-to-`or`
// example. iverilog agrees on values but evaluates the net-force RHS only
// once — its own documented limitation.
TEST_CASE("force: net override of a gate driver, continuous, revert on release") {
    constexpr std::string_view src = R"(
        module m;
            logic a, b, c;
            wire e;
            and (e, a, b, c);
            initial begin
                a = 1; b = 0; c = 1;
                #1 $display("e=%b", e);
                force e = (a | b | c);
                #1 $display("e=%b", e);
                release e;
                #1 $display("e=%b", e);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "e=0\ne=1\ne=0\n");
}

// A forced net tracks its RHS; on release a single continuous driver takes over
// immediately.
TEST_CASE("force: net tracks RHS, reverts to assign driver on release") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] src, drv;
            wire [7:0] y;
            assign y = drv;
            initial begin
                drv = 8'd1; src = 8'd50;
                #1 force y = src;
                #1 $display("y=%0d", y);
                src = 8'd77; #1 $display("y=%0d", y);
                release y; #1 $display("y=%0d", y);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "y=50\ny=77\ny=1\n");
}

// An undriven net floats z; a force overrides it and release reverts to z.
TEST_CASE("force: undriven net reverts to z on release") {
    constexpr std::string_view src = R"(
        module m;
            wire [3:0] u;
            initial begin
                #1 $display("u=%b", u);
                force u = 4'd5;
                #1 $display("u=%0d", u);
                release u;
                #1 $display("u=%b", u);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "u=zzzz\nu=5\nu=zzzz\n");
}

// A force on a multiply-driven (resolved) net overrides the resolution; on
// release the net reverts to the resolved value (x on a 0/1 conflict).
TEST_CASE("force: resolved net override reverts to resolution on release") {
    constexpr std::string_view src = R"(
        module m;
            logic da, db;
            wire w;
            assign w = da;
            assign w = db;
            initial begin
                da = 0; db = 0; #1 $display("w=%b", w);
                force w = 1'b1; #1 $display("w=%b", w);
                da = 1; db = 0; #1 $display("w=%b", w);
                release w; #1 $display("w=%b", w);
                db = 1; #1 $display("w=%b", w);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "w=0\nw=1\nw=1\nw=x\nw=1\n");
}

// A net force must be visible to a same-instant read (`force n = v; if (n===v)`),
// not deferred one delta to the resolver. Icarus ivtest force1/force2 shape.
// Matches iverilog 13.
TEST_CASE("force: net force is visible in the same time step") {
    constexpr std::string_view src = R"(
        module m;
            reg [3:0] drv = 4'b0101;
            wire [3:0] val = drv;
            initial begin
                force val = 4'b1010;
                if (val !== 4'b1010) $display("FAIL a");
                force val = 4'bzzzz;
                if (val !== 4'bzzzz) $display("FAIL b");
                $display("v=%b", val);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "v=zzzz\n");
}

// Constant bit/part-select force of a net (LRM 10.6.2): only the selected bits are
// overridden; unforced bits keep the driver (z preserved), and a range `release`
// reverts just those bits. Icarus ivtest force_release_wire_pv shape, byte-identical
// to iverilog 13.
TEST_CASE("force: net bit/part-select overlay, z-preserving, range release") {
    constexpr std::string_view src = R"(
        module m;
            reg [3:0] in = 4'b0;
            wire [3:0] bus = in;
            initial begin
                #1 force bus[0] = 1'b1;    #1 $display("%b", bus);
                #1 force bus[3:2] = 2'b11; #1 $display("%b", bus);
                in[1] = 1'bz;              #1 $display("%b", bus);
                release bus[0];            #1 $display("%b", bus);
                release bus[3:2];          #1 $display("%b", bus);
                force bus[2:1] = 2'bx1;    #1 $display("%b", bus);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "0001\n1101\n11z1\n11z0\n00z0\n0x10\n");
}

// LRM 10.6.2: a force on a net "shall override all drivers of the net", so while it
// is active a driver changing changes nothing a reader of the net can observe: the
// net holds the forced value and no value change event occurs. The resolver folded
// the drivers into the net and only then laid the force over the result, and each of
// the two stores woke the net's waiters on its own old -> new transition, so every
// driver change under a force gave an edge reader a negedge and a posedge (pos=3
// neg=4 where the force makes one real posedge and the x -> 0 at time 0 one real
// negedge). Oracle: iverilog 13 prints pos=1 neg=1.
TEST_CASE("force: a driver changing under a forced net gives its readers no event") {
    constexpr std::string_view src = R"(
        module m;
            logic a;
            wire w;
            assign w = a;
            int pos, neg;
            always @(posedge w) pos++;
            always @(negedge w) neg++;
            initial begin
                a = 0;
                #1 force w = 1'b1;
                #1 a = 1;
                #1 a = 0;
                #1 a = 1;
                #1 a = 0;
                #1 $display("pos=%0d neg=%0d w=%b", pos, neg, w);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "pos=1 neg=1 w=1\n");
}

// The same transient seen by a change reader when a driver's own delay lands under the
// force: `wire #5 w = a` is that driver's continuous-assignment delay, not a net delay
// (LRM 10.3.3), so a=1 at 10 reaches the net's resolver at 15 and re-runs it under the
// force taken at 12. `always @(w)` printed a change at 15 although w stayed z. Release
// at 20 then shows the driver's value. Oracle: iverilog 13, byte-identical.
TEST_CASE("force: a delayed driver landing under a forced net gives its readers no event") {
    constexpr std::string_view src = R"(
        module m;
            logic a;
            wire #5 w = a;
            always @(w) $display("%0t chg w=%b", $time, w);
            initial begin
                a = 0;
                #10 a = 1;
                #2 force w = 1'bz;
                #6 $strobe("%0t s w=%b", $time, w);
                #2 release w;
                $strobe("%0t s w=%b", $time, w);
                #10 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "5 chg w=0\n12 chg w=z\n18 s w=z\n20 chg w=1\n20 s w=1\n");
}

// force / assign target an OUTPUT-port variable driven internally (an input/inout
// port is the guarded case). sv-tests 10.6.1 / 10.6.2 shape.
TEST_CASE("force: output-port variable is forceable") {
    constexpr std::string_view src = R"(
        module m(output logic [7:0] q);
            initial begin
                q = 8'd1;
                force q = 8'd42;
                #1 $display("q=%0d", q);
                release q;
                #1 q = 8'd7;
                $display("q=%0d", q);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "q=42\nq=7\n");
}

// force/release and assign/deassign on a real variable (LRM 10.6). The machinery
// is type-generic; the value store is a plain real store. Icarus ivtest
// real_force_rel / real_assign_deassign shape. Matches iverilog 13.
TEST_CASE("force/assign: real variable") {
    constexpr std::string_view src = R"(
        module m;
            real f;
            reg d;
            always @(d) assign f = 1.5;
            initial begin
                f = 0.0;
                #1 $display("f=%g", f);
                f = 2.0;
                #1 $display("f=%g", f);
                d = 0;                      // assign f = 1.5 activates
                #1 f = 9.0;                 // suppressed by assign
                #1 $display("f=%g", f);
                force f = 7.25;
                #1 f = 9.0;                 // suppressed by force
                #1 $display("f=%g", f);
                release f;                  // reverts to assign (1.5)
                #1 $display("f=%g", f);
                deassign f;                 // holds
                #1 f = 3.0;
                #1 $display("f=%g", f);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "f=0\nf=2\nf=1.5\nf=7.25\nf=1.5\nf=3\n");
}

// LRM 10.6.2 suppresses the procedural UPDATE, whatever its shape: a bit select,
// a part select and a read-modify-write of a forced variable all leave the
// forced value standing, and each takes effect again after `release`.
// Byte-identical to iverilog 13 (a5, a5, a5, a5, a4, 34, 36).
TEST_CASE("force: bit-select, part-select and compound writes are suppressed") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a;
            initial begin
                a = 8'h00;
                force a = 8'hA5;
                #1 $display("t=%0t a=%h", $time, a);
                a[0] = 1'b0;
                a[7:4] = 4'h0;
                #1 $display("t=%0t a=%h", $time, a);
                a += 8'd1;
                #1 $display("t=%0t a=%h", $time, a);
                release a;
                #1 $display("t=%0t a=%h", $time, a);
                a[0] = 1'b0;
                #1 $display("t=%0t a=%h", $time, a);
                a[7:4] = 4'h3;
                #1 $display("t=%0t a=%h", $time, a);
                a += 8'd2;
                #1 $display("t=%0t a=%h", $time, a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "t=1 a=a5\nt=2 a=a5\nt=3 a=a5\nt=4 a=a5\nt=5 a=a4\nt=6 a=34\nt=7 a=36\n");
}

// The gate keys off the lvalue's ROOT, so every shape rooted at a forced
// variable is covered: a packed-array element, a packed-struct member, and the
// nonblocking form of either. Matches iverilog 13.
TEST_CASE("force: packed-array element, struct member and NBA select writes") {
    constexpr std::string_view src = R"(
        module m;
            typedef struct packed { logic [3:0] hi; logic [3:0] lo; } sp_t;
            logic [3:0][7:0] pk;
            sp_t s;
            initial begin
                pk = 32'h1122_3344;
                s  = 8'h5A;
                force pk = 32'hDEAD_BEEF;
                force s  = 8'h77;
                #1 $display("pk=%h s=%h", pk, s);
                pk[2] = 8'h00;
                s.lo  = 4'h0;
                #1 $display("pk=%h s=%h", pk, s);
                pk[1] <= 8'hFF;
                #1 $display("pk=%h s=%h", pk, s);
                release pk;
                release s;
                pk[2] = 8'h00;
                s.lo  = 4'h0;
                #1 $display("pk=%h s=%h", pk, s);
                pk[1] <= 8'hFF;
                #1 $display("pk=%h s=%h", pk, s);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "pk=deadbeef s=77\npk=deadbeef s=77\npk=deadbeef s=77\n"
                                     "pk=de00beef s=70\npk=de00ffef s=70\n");
}

// The `assign` layer (LRM 10.6.1) outranks ordinary procedural writes the same
// way, so it suppresses a select write and a compound update too; `deassign`
// lets both through again (the variable holds 10 at that point). Matches
// iverilog 13.
TEST_CASE("assign: select and compound writes are suppressed until deassign") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b;
            initial begin
                b = 8'h10;
                a = 8'h01;
                assign a = b;
                #1 $display("a=%h", a);
                a[3:0] = 4'hF;
                a += 8'd1;
                #1 $display("a=%h", a);
                deassign a;
                a[3:0] = 4'hF;
                a += 8'd1;
                #1 $display("a=%h", a);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=10\na=10\na=20\n");
}

// `i++` is the read-modify-write `i = i + 1` (LRM 11.4.2), so the update is
// dropped while the target is forced but the expression's value still stands:
// post yields the forced value, pre yields it plus one. Byte-identical to
// iverilog 13.
TEST_CASE("force: increment drops the update, keeps the expression value") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] x, y, z;
            initial begin
                x = 8'd1;
                force x = 8'd5;
                #1 x++;
                $display("a x=%0d", x);
                y = x++;
                $display("b x=%0d y=%0d", x, y);
                z = ++x;
                $display("c x=%0d z=%0d", x, z);
                release x;
                x++;
                $display("d x=%0d", x);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a x=5\nb x=5 y=5\nc x=5 z=6\nd x=6\n");
}

// A concatenation lvalue has no single root: each operand carries its own force
// state, so a forced operand keeps its value while its siblings take their
// slice of the right-hand side. Matches iverilog 13.
TEST_CASE("force: a concatenation lvalue is gated per operand") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] a, b;
            initial begin
                a = 4'h0; b = 4'h0;
                force a = 4'hA;
                #1 {a, b} = 8'h56;
                $display("a=%h b=%h", a, b);
                release a;
                {a, b} = 8'h78;
                $display("a=%h b=%h", a, b);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a=a b=6\na=7 b=8\n");
}

// An output argument's copy-back is an ordinary procedural write to the actual
// (LRM 13.5.1), so a forced actual drops it; the call itself still runs.
// Matches iverilog 13.
TEST_CASE("force: a task output copy-back to a forced actual is suppressed") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] x;
            task automatic t(output logic [7:0] o);
                o = 8'd9;
            endtask
            initial begin
                x = 8'd1;
                force x = 8'd5;
                #1 t(x);
                $display("a x=%0d", x);
                release x;
                t(x);
                $display("b x=%0d", x);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "a x=5\nb x=9\n");
}

// -- Loud guards (the remaining force/assign gaps) --

// The gate decides at store time, but an intra-assignment delay commits later,
// so the write path cannot yet carry it (see the TODO in lower_assign).
TEST_CASE("intra-assignment timing on a write to a forced variable is rejected") {
    constexpr std::string_view src = R"(
        module m; logic [7:0] a;
            initial begin force a = 8'd1; a = #5 8'd2; end
        endmodule)";
    CHECK(lower_error(src).find("intra-assignment timing on a write to a force-target") != std::string::npos);
}

// A delayed continuous assignment (`assign #5 v = a`) commits its value from the
// timing wheel straight into the variable, and the commit carries no force gate, so
// the value landed over an active force: v=1 at 15 where the force taken at 12 holds
// z until the release at 20 (LRM 10.6.2; iverilog 13 holds z). What the release then
// does is not settled either: 10.6.2 says it "shall reestablish that assignment and
// schedule a reevaluation in the continuous assignment's scheduling region", and
// 10.3.3 delays an assignment's update by its delay. Refused rather than guessed.
// A net with a delayed driver is unaffected: its driver stores into a slot that the
// resolver folds under the force, as the test above ("a delayed driver landing")
// shows.
TEST_CASE("a delayed continuous assignment to a forced variable is rejected") {
    constexpr std::string_view src = R"(
        module m;
            logic a, v;
            assign #5 v = a;
            initial begin force v = 1'bz; release v; end
        endmodule)";
    CHECK(lower_error(src).find("continuous assignment delay on a force-target variable") != std::string::npos);
}

// A partial force of a VARIABLE stays rejected — the site state is per whole
// variable, and LRM 10.6.2 permits a partial force only on a net. slang flags
// the lvalue (-Wbad-procedural-force) and leaves the statement unbound, so the
// rejection surfaces as the generic unsupported-statement guard rather than the
// force-specific one. iverilog 13 refuses the unpacked-array word form too
// ("cannot %force/vec4 to the word of a variable array").
TEST_CASE("force of a variable bit-select or array word is rejected") {
    constexpr std::string_view bit_sel = R"(
        module m; logic [7:0] a;
            initial begin force a[0] = 1'b1; end
        endmodule)";
    CHECK(lower_error(bit_sel).find("unsupported statement") != std::string::npos);
    constexpr std::string_view arr_word = R"(
        module m; logic [7:0] mem [0:3];
            initial begin force mem[1] = 8'd9; end
        endmodule)";
    CHECK(lower_error(arr_word).find("unsupported statement") != std::string::npos);
}

// LRM 10.6.2: releasing a variable driven by a continuous assignment
// reestablishes that assignment (the variable reverts to the driver's current
// value, not held). Byte-identical to iverilog 13.
TEST_CASE("release: reestablishes a continuous assignment on the variable") {
    constexpr std::string_view src = R"(
        module m;
            logic [7:0] a, b, d;
            assign d = a + b;
            initial begin
                a = 1; b = 2;
                #1 $display("d=%0d", d);
                force d = 8'd100;
                #1 a = 10;
                #1 $display("d=%0d", d);
                release d;
                #1 $display("d=%0d", d);
                b = 20;
                #1 $display("d=%0d", d);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "d=3\nd=100\nd=12\nd=30\n");
}

// `input var` is what makes the port a VARIABLE. Spelled `input logic [7:0] p`
// this is a net (LRM 23.2.2.3: "For input and inout ports, the port shall
// default to a net of default net type"; patches/slang-input-port-net-kind.patch)
// and a `force` on it is perfectly legal -- the case below.
TEST_CASE("force on an input port variable is rejected") {
    constexpr std::string_view src = R"(
        module sub(input var logic [7:0] p); endmodule
        module m;
            logic [7:0] x;
            sub s(.p(x));
            initial begin force s.p = 8'd1; end
        endmodule)";
    CHECK(lower_error(src).find("input or inout port variable") != std::string::npos);
}

// The same port with its LRM-default kind is a net, and LRM 10.6.2 allows a
// `force` on a net: the forced value overrides the port copy's driver until the
// `release`, which is why the refusal above is about the VARIABLE case only and
// not about ports.
TEST_CASE("force on an input port net drives it") {
    constexpr std::string_view src = R"(
        module sub(input logic [7:0] p);
            initial begin #1 $display("p=%0d", p); #2 $display("p=%0d", p); end
        endmodule
        module m;
            logic [7:0] x;
            sub s(.p(x));
            initial begin
                x = 5;
                force s.p = 8'd1;
                #2 release s.p;
                #2 $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "p=1\np=5\n");
}

// LRM 10.6.2: "when released, the net shall immediately be assigned the value
// determined by the drivers of the net". The statement right after a `release`
// of a whole net, of one forced bit, of an undriven net (z) or of a resolved
// net already sees the driven value, not the forced one. Oracle: iverilog 13
// prints these lines for this source.
TEST_CASE("release of a net takes the driven value at once") {
    constexpr std::string_view src = R"(
        module m;
            logic [3:0] in = 4'b0000;
            wire  [3:0] bus = in;
            wire  [3:0] pb = in;
            wire  [3:0] und;
            wire  [3:0] md;
            assign md = in;
            assign md[1] = 1'b0;
            initial begin
                #1 force bus = 4'b1111;
                #1 release bus;
                $display("bus=%b", bus);
                force pb[0] = 1'b1;
                force pb[3] = 1'b1;
                release pb[0];
                $display("pb=%b", pb);
                release pb[3];
                $display("pb=%b", pb);
                force und = 4'b1010;
                release und;
                $display("und=%b", und);
                in = 4'b0100;
                #1 force md = 4'b1111;
                release md;
                $display("md=%b", md);
                $finish;
            end
        endmodule)";
    CHECK(run_capture_finish(src) == "bus=0000\npb=1000\npb=0000\nund=zzzz\nmd=0100\n");
}

// `release` is legal wherever a statement that does not block is (LRM 13.4.4), so
// it can sit in a function or a class method function, and the net takes its
// driven value at once there too (LRM 10.6.2). The release runs the net's
// resolver, which therefore has to be callable from a plain function. A net with
// a net delay folds through an inertial store, the other shape of that resolver:
// it is released from a function in two instances and settles on its driver.
// Oracle: iverilog 13 prints the first three lines for the same source (it does
// not support net delays, so the last two are the LRM's: the driver's value).
TEST_CASE("release of a net inside a function and a class method function") {
    constexpr std::string_view src = R"(
        module sub(input [3:0] d);
            wire [3:0] bus = d;
            wire [3:0] #2 dbus;
            assign dbus = d;
            function void rel();
                release bus;
                release dbus;
            endfunction
            initial begin
                #1 force bus = 4'b1111;
                force dbus = 4'b1010;
                #1 rel();
                $display("%m bus=%b", bus);
                #8 $display("%m dbus=%b", dbus);
            end
        endmodule
        module top;
            logic [3:0] in = 4'b0000;
            wire  [3:0] cbus = in;
            sub a(4'b0001);
            sub b(4'b0110);
            class Drv;
                function void unforce();
                    release cbus;
                endfunction
            endclass
            Drv drv;
            initial begin
                drv = new;
                #1 force cbus = 4'b1111;
                #1 drv.unforce();
                $display("cbus=%b", cbus);
                #9 $finish;
            end
        endmodule)";
    const std::string          out = run_capture_finish(src);
    CHECK(out.find("cbus=0000\n") != std::string::npos);
    CHECK(out.find("top.a bus=0001\n") != std::string::npos);
    CHECK(out.find("top.b bus=0110\n") != std::string::npos);
    CHECK(out.find("top.a dbus=0001\n") != std::string::npos);
    CHECK(out.find("top.b dbus=0110\n") != std::string::npos);
}

// LRM 10.6.2: releasing a bit or part of a forced range frees only those bits, the
// rest stay forced. A force site is released as a unit, so executing a release
// that overlaps an ACTIVE site without covering all of it stops the run with a
// diagnostic, instead of releasing all of it (or none: `force bus = ..; release
// bus[0];` read 1111 where iverilog 13 prints 1110). A release that covers the
// site, misses it or meets a site that is not forced at that moment is exact, as
// before: the overlap below is a force that happens after the release.
TEST_CASE("release of part of an active forced range stops the run") {
    const test::DesignRun run = test::run_design_captured(R"(
        module m;
            logic [3:0] in = 4'b0;
            wire [3:0] bus = in;
            initial begin
                #1 force bus[3:2] = 2'b11;
                #1 release bus[3:2];
                $display("ok %b", bus);
                #1 force bus[2:1] = 2'b11;
                #1 release bus[2:1];
                $display("again %b", bus);
                #1 force bus = 4'b1111;
                #1 release bus[0];
                $display("not reached %b", bus);
                $finish;
            end
        endmodule)");
    CHECK(run.out == "ok 0000\nagain 0000\n");
    CHECK(test::design_exit_code(run) != 0);
    CHECK(run.err.find("release of part of an active force is not supported") != std::string::npos);
}

// `force a = '{..}` of a whole unpacked net array registered a force site as wide as
// ONE element, so it forced element 0 with a garbled value and left the rest
// (a=0xx1 xxxx where the LRM gives 0011 0101). iverilog 13 asserts on it. The
// target is refused, like a force of a select of an unpacked array.
TEST_CASE("force of a whole unpacked net array is rejected") {
    const std::string err = lower_error(R"(
        module m;
            wire [3:0] a [0:1];
            initial begin
                #1 force a = '{4'h3, 4'h5};
                #1 release a;
            end
        endmodule)");
    CHECK(err.find("force is only supported on a whole variable/net") != std::string::npos);
}
