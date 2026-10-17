// The weak ranged `always` property operator.
//
// Clotho lowers this: `always [m:n]` reaches sva_prop_shift as SVA_SHIFT_EVERY,
// the range read conjunctively.
// See docs/design/sva-property-layer.md, phase 1 (as built).
//
// WEAK ranged always only. `s_always` lives in diff_strongprop.sv. The unranged
// forms are unbounded and stay refused (phase 3), so they are absent by design.
//
// LRM 16.12.11:
//
//   "always [cycle_delay_const_range_expression] property_expr evaluates to
//    true if, and only if, property_expr holds at every current or future
//    clock tick that is within the range ... IT IS NOT REQUIRED THAT ALL CLOCK
//    TICKS WITHIN THIS RANGE EXIST."
//
// with the precise restatement counting offsets "starting at the current time
// step", so offset 0 is the attempt's own tick. That last sentence is the whole
// of the weak side: a window running off the end of the trace is forgiven.
//
// Two ranges on two different signals, so their reports land at different times
// and no report is ambiguous about which range produced it. The range is pinned
// by the report COUNT at a single low tick -- a window of width w is entered by
// exactly w attempts, so each range gives three reports at its own signal's
// only low cycle.
//
// `[0:2]` is here on purpose: offset 0 is the attempt's own tick, so that
// assertion can fail in the cycle it starts in (visible as a report whose start
// time equals its failure time). It is the corner an implementation that
// desugars `always [m:n]` into a chain of delays is most likely to get wrong.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    a  b
//    0     5    1  1
//    1    15    1  1
//    2    25    1  1
//    3    35    1  1
//    4    45    0  1     <- the only low on a
//    5    55    1  1
//    6    65    1  1
//    7    75    1  0     <- the only low on b
//    8    85    1  1
//    9    95    1  1     last evaluated tick
//   10   105    1  1     $finish executes here, so this tick never evaluates
//
// The two lows are three cycles apart, deliberately: with a window of width
// three no single attempt can see both, so no attempt has two failures in it.
// That keeps this fixture clear of the per-attempt reporting rule, under which an
// attempt with two failures reports once (docs/LRM_NOTES.md, note 23).
//
// Derived from the LRM:
//
//   wa  always [1:3] a -- attempt at N covers ticks N+1..N+3. Cycle 4 is in
//       that window for N = 1, 2 and 3, so three attempts fail, all at the tick
//       where a is low. The attempts from cycles 7, 8 and 9 have windows
//       running past tick 9 and hold weakly.                   -> FAIL 45 x3
//   wb  always [0:2] b -- attempt at N covers ticks N..N+2. Cycle 7 is in that
//       window for N = 5, 6 and 7 -- note N = 7 fails in its OWN starting
//       cycle, which is what offset 0 means.                   -> FAIL 75 x3
//
//   expected: 3 reports at 45 and 3 at 75 -- six in all, and NOTHING at 105.
//
// The silence at 105 is load-bearing rather than incidental: five attempts (wa
// from cycles 7, 8 and 9, wb from cycles 8 and 9) are pending when the trace
// ends and all five hold. diff_strongprop.sv runs the identical stimulus
// through s_always and gets exactly those five reports, so the pair of files
// isolates the weak/strong difference to the end of trace and nothing else.
//
// Not vacuous: on each signal only one tick is low, so most attempts pass --
// `always [1:3] a` passes from cycles 0, 4, 5 and 6 and fails from 1, 2 and 3.
// A monitor that never fires loses six reports and one that fires every cycle
// gains fourteen, so neither reads as correct.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1, b = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end

  wa: assert property (@(posedge clk) always [1:3] a);
  wb: assert property (@(posedge clk) always [0:2] b);

  initial begin
    @(posedge clk);            // cyc0
    @(posedge clk);            // cyc1
    @(posedge clk);            // cyc2
    @(posedge clk);            // cyc3
    a <= 0;
    @(posedge clk);            // cyc4: a=0
    a <= 1;
    @(posedge clk);            // cyc5
    @(posedge clk);            // cyc6
    b <= 0;
    @(posedge clk);            // cyc7: b=0
    b <= 1;
    @(posedge clk);            // cyc8  quiet tail: the attempts left open here
    @(posedge clk);            // cyc9  all hold, weakly
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
