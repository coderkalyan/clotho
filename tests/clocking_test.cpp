#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "Lower.h"
#include "PosixIo.h"
#include "ir/Jit.h"

using namespace clotho;

// Clocking blocks per LRM 14. A clocking block is one Observed-region process
// (LRM 4.4.2.5) that, at each clocking event, updates the sampled value of every
// input clockvar and then triggers the event named by the block -- in that order,
// which is what LRM 14.13 guarantees to a process waiting on the block.
//
// Two things are observable and are what these check:
//
//  * WHICH value a clockvar holds. An input skew of `#1step` (the LRM 14.4
//    default) samples the value the signal held immediately before the edge, so a
//    write in the edge's own time slot is NOT seen; an explicit `#0` skew samples
//    in the Observed region and IS. The pair of tests below differ in exactly that
//    one token and print different values.
//  * WHEN a `@(cb)` waiter runs: after the Active region work of the same time
//    slot, because the trigger happens in Observed (LRM 14.10).
//
// Only input clockvars are lowered so far; a synchronous drive (LRM 14.16) is
// refused loudly, as is every skew other than `#1step` / `#0` -- see the refusal
// tests at the bottom.

namespace {

// JIT-run a design and capture stdout (mirrors program_test.cpp).
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

// Assert that `source` is rejected by the frontend with a diagnostic containing
// `needle`.
void check_rejected(std::string_view source, std::string_view needle) {
    const LowerResult r = lower_text(source);
    CHECK_FALSE(r.ok);
    CHECK_MESSAGE(r.diagnostic_text().find(needle) != std::string::npos, r.diagnostic_text());
}

} // namespace

// -- Input sampling ---------------------------------------------------------

// The defining behavior of a clocking block, and the reason it exists (LRM 24.4):
// `d` is written in the SAME time slot as the posedge, and the clockvar does not
// see it. LRM 14.4: "the value sampled is always the signal's last value
// immediately before the corresponding clock edge". The live `d` printed beside it
// is the new value, so this also shows the two are different reads.
TEST_CASE("a #1step input clockvar samples the value before the edge") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); input d; endclocking
          always #5 clk = ~clk;
          initial #5 d = 1;
          always @(cb) $display("[%0t] cb.d=%b d=%b", $time, cb.d, d);
          initial #26 $finish;
        endmodule
    )") == "[5] cb.d=0 d=1\n[15] cb.d=1 d=1\n[25] cb.d=1 d=1\n");
}

// The same design with one token changed: an explicit `#0` skew is sampled in the
// Observed region, at the same time as the clocking event (LRM 14.4), so it DOES
// see the write made in the edge's time slot.
TEST_CASE("an explicit #0 input skew samples in the Observed region") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); input #0 d; endclocking
          always #5 clk = ~clk;
          initial #5 d = 1;
          always @(cb) $display("[%0t] cb.d=%b d=%b", $time, cb.d, d);
          initial #26 $finish;
        endmodule
    )") == "[5] cb.d=1 d=1\n[15] cb.d=1 d=1\n[25] cb.d=1 d=1\n");
}

// A block-level `default input` item supplies the skew for an item that does not
// write its own (LRM 14.3), and the per-item skew overrides it. `a` takes the
// block default `#0` and sees the same-slot write; `b` overrides back to `#1step`
// and does not.
TEST_CASE("a per-item skew overrides the block default skew") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit a = 0, b = 0;
          clocking cb @(posedge clk);
            default input #0;
            input a;
            input #1step b;
          endclocking
          always #5 clk = ~clk;
          initial #5 begin a = 1; b = 1; end
          always @(cb) $display("[%0t] a=%b b=%b", $time, cb.a, cb.b);
          initial #16 $finish;
        endmodule
    )") == "[5] a=1 b=0\n[15] a=1 b=1\n");
}

// Values that come and go between two clocking events are invisible to the block:
// `d` pulses entirely inside one clock period, and the sampled stream never leaves
// 0, so a posedge of the CLOCKVAR never happens. This is what makes a clocking
// block insensitive to a race the live signal has (LRM 14.15: "the values used by
// the synchronization event control are the synchronous values").
TEST_CASE("a pulse between two clocking events is not sampled") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); input d; endclocking
          always #5 clk = ~clk;
          initial begin #6 d = 1; #2 d = 0; end
          always @(posedge cb.d) $display("[%0t] SAMPLED POSEDGE", $time);
          initial #26 $finish;
        endmodule
    )") == "");
}

// The same shape with the pulse left high: the clockvar rises at the next clocking
// event, and `@(posedge cb.d)` fires there -- one time slot's worth of edge, not
// where the live signal changed.
TEST_CASE("an edge of a clockvar is an edge of the sampled stream") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); input d; endclocking
          always #5 clk = ~clk;
          initial #6 d = 1;
          always @(posedge cb.d) $display("[%0t] SAMPLED POSEDGE", $time);
          initial #26 $finish;
        endmodule
    )") == "[15] SAMPLED POSEDGE\n");
}

// -- Scheduling -------------------------------------------------------------

// LRM 14.10: the clocking block event is triggered in the Observed region, so a
// process waiting on it "executes in the first Active region after the Observed
// region" -- i.e. after every Active-region process sensitive to the same clock,
// including one woken by another's write (`cascade`). An Active-region trigger
// would interleave instead.
TEST_CASE("a @(cb) waiter runs after all Active work of the time slot") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 1;
          int v = 0;
          clocking cb @(posedge clk); input d; endclocking
          always #5 clk = ~clk;
          always @(posedge clk) begin
            $display("[%0t] ACTIVE first", $time);
            v = v + 1;
          end
          always @(v) $display("[%0t] ACTIVE cascade", $time);
          always @(cb) $display("[%0t] CLOCKING cb.d=%b", $time, cb.d);
          initial #6 $finish;
        endmodule
    )") == "[5] ACTIVE first\n[5] ACTIVE cascade\n[5] CLOCKING cb.d=1\n");
}

// LRM 14.13: the samples are updated BEFORE the block's event is triggered, so a
// process woken by `@(cb)` is guaranteed to read this cycle's sample and never the
// previous one. `d` is written a full time step before the edge, so a waiter that
// read a stale sample would print the value from two cycles back.
TEST_CASE("a @(cb) waiter reads the sample of the cycle that woke it") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int unsigned v = 0;
          clocking cb @(posedge clk); input v; endclocking
          always #5 clk = ~clk;
          initial begin #4 v = 1; #4 v = 2; #4 v = 3; end
          always @(cb) $display("[%0t] cb.v=%0d", $time, cb.v);
          initial #26 $finish;
        endmodule
    )") == "[5] cb.v=1\n[15] cb.v=3\n[25] cb.v=3\n");
}

// Two blocks over one signal, each with its own clocking event (LRM 14.6 / 14.13:
// "each clocking block samples the corresponding signal with its own clocking
// event"). The negedge block sees a value the posedge block never does.
TEST_CASE("two clocking blocks sample one signal on their own events") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          int unsigned v = 0;
          clocking cp @(posedge clk); input v; endclocking
          clocking cn @(negedge clk); input v; endclocking
          always #5 clk = ~clk;
          initial begin #4 v = 1; #4 v = 2; #4 v = 3; end
          always @(cp) $display("[%0t] POS cp.v=%0d", $time, cp.v);
          always @(cn) $display("[%0t] NEG cn.v=%0d", $time, cn.v);
          initial #21 $finish;
        endmodule
    )") == "[5] POS cp.v=1\n[10] NEG cn.v=2\n[15] POS cp.v=3\n[20] NEG cn.v=3\n");
}

// -- Interfaces -------------------------------------------------------------

// A clocking block declared in an interface (LRM 14.9) reaches the same member
// walk as a module's, and its clockvars are read through the interface port from
// the module: `bus.cb.data`. The clockvar storage lives in the interface
// instance's scope, which is also where the sampled signal it copies lives.
TEST_CASE("a clocking block declared in an interface samples and is waited on") {
    CHECK(run_capture(R"(
        interface bus_if(input bit clk);
          bit data;
          clocking cb @(posedge clk); input data; endclocking
        endinterface

        module t;
          bit clk = 0;
          bus_if bus(clk);
          always #5 clk = ~clk;
          initial #5 bus.data = 1;
          always @(bus.cb) $display("[%0t] IF cb.data=%b data=%b", $time, bus.cb.data, bus.data);
          initial #16 $finish;
        endmodule
    )") == "[5] IF cb.data=0 data=1\n[15] IF cb.data=1 data=1\n");
}

// -- A renamed clocking item ------------------------------------------------

// LRM 14.3: `input name = signal` gives the clockvar a name of its own while
// sampling another signal in the enclosing scope. The simple-name form of 14.5 is
// supported; a slice or concatenation is not (below).
TEST_CASE("a clocking item may rename the signal it samples") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit raw = 0;
          clocking cb @(posedge clk); input sampled_raw = raw; endclocking
          always #5 clk = ~clk;
          initial #4 raw = 1;
          always @(cb) $display("[%0t] cb.sampled_raw=%b", $time, cb.sampled_raw);
          initial #16 $finish;
        endmodule
    )") == "[5] cb.sampled_raw=1\n[15] cb.sampled_raw=1\n");
}

// -- Synchronous drives (LRM 14.16) -----------------------------------------

// The canonical testbench drive: wait for the clocking event, then drive. The
// value is written in the Re-NBA region of THAT time slot (LRM 14.16), so it lands
// at the same simulation time the drive executed -- one cycle late would be the
// classic off-by-one.
TEST_CASE("a synchronous drive lands in the Re-NBA region of its clocking event") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); output d; endclocking
          always #5 clk = ~clk;
          initial begin
            @(cb); cb.d <= 1;
            @(cb); cb.d <= 0;
          end
          always @(d) $display("[%0t] d=%b", $time, d);
          initial #26 $finish;
        endmodule
    )") == "[5] d=1\n[15] d=0\n");
}

// LRM 14.16: a drive "shall execute without blocking" and performs its action at
// the clocking event, so the target still holds the old value on the statement
// after the drive -- and takes the new one later in the same time slot.
TEST_CASE("a synchronous drive does not write its target immediately") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); output d; endclocking
          always #5 clk = ~clk;
          initial begin
            @(cb);
            cb.d <= 1;
            $display("[%0t] after the drive d=%b", $time, d);
          end
          always @(d) $display("[%0t] d became %b", $time, d);
          initial #16 $finish;
        endmodule
    )") == "[5] after the drive d=0\n[5] d became 1\n");
}

// LRM 14.16: a drive executed at a time NOT coincident with the clocking event
// "shall perform their drive action as if they had executed at the time of the
// next clocking event". Driven at 3, applied at the posedge at 5.
TEST_CASE("a drive between two clocking events matures at the next one") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); output d; endclocking
          always #5 clk = ~clk;
          initial #3 cb.d <= 1;
          always @(d) $display("[%0t] d=%b", $time, d);
          initial #16 $finish;
        endmodule
    )") == "[5] d=1\n");
}

// LRM 14.16.2's own example: two drives of the same clockvar maturing in the same
// Re-NBA region, where "the last value is the only value driven onto the output
// signal". Only one change of `nib` is ever visible.
TEST_CASE("the last of several drives in one cycle is the only one driven") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          logic [3:0] nib = 0;
          clocking cb @(posedge clk); output nib; endclocking
          always #5 clk = ~clk;
          initial begin
            @(cb);
            cb.nib <= 4'b0101;
            cb.nib <= 4'b0011;
          end
          always @(nib) $display("[%0t] nib=%b", $time, nib);
          initial #16 $finish;
        endmodule
    )") == "[5] nib=0011\n");
}

// LRM 14.16.1: "a drive does not change the clocking block input ... reading the
// input always yields the last sampled value, and not the driven value". This is
// the LRM's own `cb.a <= c; cb.b <= cb.a;` shape: `b` takes the value `a` had
// before the change, so the two directions of an `inout` cannot share storage.
TEST_CASE("a drive to an inout clockvar does not change its sampled input") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit a = 0, b = 0;
          clocking cb @(posedge clk); inout a; output b; endclocking
          always #5 clk = ~clk;
          initial begin
            @(cb);
            cb.a <= 1;
            cb.b <= cb.a;
          end
          final $display("a=%b b=%b", a, b);
          initial #16 $finish;
        endmodule
    )") == "a=1 b=0\n");
}

// The pairing LRM 24.3 and 14.16 are built for: a program drives the design
// through a clocking block. The drive is issued in the Reactive region and applied
// in the Re-NBA region of the same time slot, so the design sees it at the clock
// edge the program was woken by.
//
// The second `@(t.cb)` keeps the program alive past the cycle it drove in: LRM
// 24.6 ends the simulation when the last program initial completes, and clotho's
// finish empties the queues of the wave it happens in, which would otherwise drop
// the very Re-NBA pass under test.
TEST_CASE("a program drives the design through a clocking block") {
    CHECK(run_capture(R"(
        module t;
          bit clk = 0;
          bit d = 0;
          clocking cb @(posedge clk); output d; endclocking
          always #5 clk = ~clk;
          always @(d) $display("[%0t] d=%b", $time, d);
          p pi();
          initial #26 $finish;
        endmodule

        program p;
          initial begin
            @(t.cb);
            t.cb.d <= 1;
            @(t.cb);
          end
        endprogram
    )") == "[5] d=1\n");
}

// -- What is refused, and loudly ---------------------------------------------

// A nonzero output skew drives "in the Re-NBA region of a future time step" (LRM
// 14.16), which needs a scheduled maturation rather than this block's own event.
TEST_CASE("a nonzero output skew is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, d = 0;
          clocking cb @(posedge clk); output #2 d; endclocking
          initial #1 $finish;
        endmodule
    )",
                   "output skew other than `#0`");
}

// LRM 14.16: a net target needs a `'z`-initialized (strong1, strong0) driver of
// its own, resolved against the net's other drivers.
TEST_CASE("a clocking block output on a net is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0;
          wire d;
          clocking cb @(posedge clk); output d; endclocking
          initial #1 $finish;
        endmodule
    )",
                   "clocking block output on a net is not supported");
}

// LRM 14.16: `cb.sig <= ##n expr` postpones the drive by n cycles of the
// clockvar's own clocking block.
TEST_CASE("a cycle delay on a synchronous drive is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, d = 0;
          default clocking cb @(posedge clk); output d; endclocking
          initial begin @(cb); cb.d <= ##1 1'b1; end
        endmodule
    )",
                   "cycle delay on a synchronous drive is not supported");
}

// LRM 14.16 allows a bit-select or slice as the drive target; the pending value
// here is one whole-signal store, so a partial drive is refused.
TEST_CASE("a synchronous drive of a bit-select is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0;
          logic [3:0] nib = 0;
          clocking cb @(posedge clk); output nib; endclocking
          initial begin @(cb); cb.nib[0] <= 1'b1; end
        endmodule
    )",
                   "bit-select or slice of a clockvar is not supported");
}

// LRM 14.14. Also the gate on `$global_clock` and on the `global clocking` half of
// multiclock assertions.
TEST_CASE("global clocking is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0;
          global clocking gc @(posedge clk); endclocking
          initial #1 $finish;
        endmodule
    )",
                   "`global clocking` is not supported");
}

// An input skew of `#N` samples the value the signal held N time units before the
// clocking event (LRM 14.13), which is not knowable when the edge arrives.
TEST_CASE("a nonzero input skew other than 1step is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, d = 0;
          clocking cb @(posedge clk); input #3 d; endclocking
          initial #1 $finish;
        endmodule
    )",
                   "input skew other than `#1step` or `#0`");
}

// An edge-based skew (LRM 14.4) samples on a named edge of the clocking signal,
// which is a second event for the block to step on.
TEST_CASE("an edge-based skew is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, d = 0;
          clocking cb @(posedge clk); input posedge d; endclocking
          initial #1 $finish;
        endmodule
    )",
                   "edge-based clocking skew is not supported");
}

// The Observed body detects its own edge from a one-slot history of the clocking
// signal, which a level-sensitive clock has no single answer for.
TEST_CASE("a level-sensitive clocking event is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, d = 0;
          clocking cb @(clk); input d; endclocking
          initial #1 $finish;
        endmodule
    )",
                   "clocking event must be a posedge or a negedge");
}

TEST_CASE("a multi-signal clocking event is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, clk2 = 0, d = 0;
          clocking cb @(posedge clk or posedge clk2); input d; endclocking
          initial #1 $finish;
        endmodule
    )",
                   "only a single-signal clocking event is supported");
}

// LRM 14.5 allows a slice or concatenation of signals as a clocking item. The
// sample is one whole-signal copy, so only a whole signal is supported.
TEST_CASE("a concatenation clocking item is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, a = 0, b = 0;
          clocking cb @(posedge clk); input d = {a, b}; endclocking
          initial #1 $finish;
        endmodule
    )",
                   "bound to an expression rather than to a signal");
}

// LRM 14.3 admits an assertion_item_declaration as a clocking item; LRM 16.16(b)
// then gives it the block's event as its leading clocking event, which nothing
// here implements.
TEST_CASE("a sequence declared inside a clocking block is refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, d = 0;
          clocking cb @(posedge clk);
            input d;
            sequence s; d; endsequence
          endclocking
          initial #1 $finish;
        endmodule
    )",
                   "declared inside a clocking block is not supported");
}

// A cycle delay (LRM 14.11) waits for N clocking block events of the default
// clocking. The clocking block it needs now elaborates, so this refusal has moved
// from the member walk to the statement itself.
TEST_CASE("a cycle delay statement is still refused") {
    check_rejected(R"(
        module t;
          bit clk = 0, d = 0;
          default clocking cb @(posedge clk); input d; endclocking
          initial begin ##1; $finish; end
        endmodule
    )",
                   "unsupported timing control inside a process body");
}
