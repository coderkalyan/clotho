// The weak indexed `nexttime` property operator.
//
// Clotho lowers this: `nexttime` reaches sva_prop_shift with lo == hi == n.
// See docs/design/sva-property-layer.md, phase 1 (as built).
//
// WEAK indexed nexttime only. The strong forms live in diff_strongprop.sv, on
// the same stimulus, so the weak/strong contrast this file's tail is shaped for
// is completed there.
//
// LRM 16.12.10:
//
//   "nexttime [n] property_expr evaluates to true if, and only if, either
//    there are not n clock ticks or property_expr evaluates to true beginning
//    at the last of the next n clock ticks."
//
// and the more precise restatement in the same clause: counting "starts at the
// current time step", so nexttime[n] p is p at the tick n after the attempt's
// own. LRM 16.12.15 puts `nexttime` in the weak list: it "does not impose any
// requirement on the terminating condition, and does not require the clock to
// tick", so an attempt whose target tick never arrives HOLDS.
//
// Two indices are asserted rather than one. For a boolean operand every
// nexttime[k] fails at exactly the ticks where the operand is low, whatever k
// is -- the report is keyed by the failing tick, not the starting one -- so the
// index is invisible in the middle of a trace. It is visible only at the
// boundaries. A low at cycle 1 is reachable by nexttime[1] (from cycle 0) and
// by nothing else, so t=15 belongs to wn1 alone and an off-by-one in the index
// moves the report set.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    a
//    0     5    1
//    1    15    0     <- low at cycle 1: only nexttime[1] can reach it
//    2    25    1
//    3    35    1
//    4    45    0     <- low mid-trace: both indices reach it
//    5    55    1
//    6    65    1
//    7    75    1
//    8    85    1
//    9    95    1     last evaluated tick
//   10   105    1     $finish executes here, so this tick never evaluates
//
// Derived from the LRM:
//
//   wn1  nexttime[1] a -- attempt at N checks a at N+1 when that tick exists.
//        a is low at cycles 1 and 4, reached from cycles 0 and 3. The attempt
//        from cycle 9 wants tick 10, which never evaluates; weak, so it holds
//        and reports nothing.                         -> FAIL 15, FAIL 45
//   wn2  nexttime[2] a -- checks a at N+2. Cycle 1 is unreachable (it would
//        need an attempt at cycle -1), so only the cycle-4 low reports. The
//        attempts from cycles 8 and 9 hold, weakly.             -> FAIL 45
//
//   expected: 1 report at 15 and 2 at 45 -- three in all, and NOTHING at 105.
//
// The silence at 105 is load-bearing rather than incidental: three attempts
// (wn1 from cycle 9, wn2 from cycles 8 and 9) are pending when the trace ends
// and all three hold. diff_strongprop.sv runs the identical stimulus through
// s_nexttime and gets exactly those three reports, so the pair of files
// isolates the weak/strong difference to the end of trace and nothing else.
//
// Not vacuous: a is high at eight of the ten evaluated ticks, so both
// assertions pass far more often than they report, and each reports at least
// once. A monitor that never fires loses three reports and one that fires every
// cycle gains seventeen, so neither reads as correct.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b", $time, cyc, a);
  end

  wn1: assert property (@(posedge clk) nexttime [1] a);
  wn2: assert property (@(posedge clk) nexttime [2] a);

  initial begin
    @(posedge clk);            // cyc0: a=1
    a <= 0;
    @(posedge clk);            // cyc1: a=0
    a <= 1;
    @(posedge clk);            // cyc2: a=1
    @(posedge clk);            // cyc3: a=1
    a <= 0;
    @(posedge clk);            // cyc4: a=0
    a <= 1;
    @(posedge clk);            // cyc5: a=1
    @(posedge clk);            // cyc6  the tail is quiet so that the attempts
    @(posedge clk);            // cyc7  left open at the end are the only thing
    @(posedge clk);            // cyc8  still owed, and weakly they hold
    @(posedge clk);            // cyc9
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
