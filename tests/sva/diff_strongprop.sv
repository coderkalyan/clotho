// The strong property operators `s_nexttime` and `s_always`.
//
// Clotho lowers this: `s_nexttime` and `s_always` reach sva_prop_shift with a
// strong subtree, which is the whole content of the fixture.
// See docs/design/sva-property-layer.md, phase 1 (as built).
//
// The weak forms this file contrasts against live in diff_nexttime.sv and
// diff_alwaysrange.sv.
//
// LRM 16.12.10 for nexttime and 16.12.11 for always, with 16.12.15 supplying
// the strength rule that is the entire content of this fixture:
//
//   "The property operators s_nexttime, s_always, s_eventually, s_until,
//    s_until_with, and sequence operator strong are strong: they require that
//    some terminating condition happen in the future, and this includes THE
//    REQUIREMENT THAT THE PROPERTY CLOCK TICKS ENOUGH TIME to enable the
//    condition to happen."
//
// and, specifically, "s_always [constant_range] property_expr evaluates to
// true if, and only if, ALL current or future clock ticks specified by
// constant_range EXIST and property_expr holds at each of these clock ticks"
// (16.12.11), against weak always's "it is not required that all clock ticks
// within this range exist".
//
// THE STIMULUS IS DELIBERATELY IDENTICAL to the two weak fixtures: signal `a`
// carries diff_nexttime.sv's waveform and signal `b` carries the `[0:2]` half
// of diff_alwaysrange.sv's, shifted onto one trace. Every mid-trace report
// below therefore has a twin in one of those files, and every END-OF-TRACE
// report below has no twin at all. That is the weak/strong difference, isolated
// to the one place LRM 16.12.15 puts it and nowhere else.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    a  b
//    0     5    1  1
//    1    15    0  1     <- low on a at cycle 1: only s_nexttime[1] reaches it
//    2    25    1  1
//    3    35    1  1
//    4    45    0  1     <- low on a mid-trace: both indices reach it
//    5    55    1  1
//    6    65    1  1
//    7    75    1  0     <- the only low on b
//    8    85    1  1
//    9    95    1  1     last evaluated tick
//   10   105    1  1     $finish executes here, so this tick never evaluates
//
// Derived from the LRM:
//
//   sn1  s_nexttime[1] a  -- checks a at N+1. Low at cycles 1 and 4, reached
//        from cycles 0 and 3. Plus the attempt from cycle 9, which requires a
//        tick 10 that never evaluates.
//                                        -> FAIL 15, FAIL 45, FAIL 105 x1
//   sn2  s_nexttime[2] a  -- checks a at N+2. Cycle 1 is unreachable. Plus the
//        attempts from cycles 8 and 9, each owed a tick that never comes.
//                                                 -> FAIL 45, FAIL 105 x2
//   sa13 s_always [1:3] b -- covers ticks N+1..N+3. Cycle 7 is in that window
//        for N = 4, 5 and 6. Plus cycles 7, 8 and 9, whose windows need ticks
//        10, 11 and 12.                        -> FAIL 75 x3, FAIL 105 x3
//   sa02 s_always [0:2] b -- covers ticks N..N+2. Cycle 7 is in that window for
//        N = 5, 6 and 7 -- N = 7 failing in its own starting cycle is what
//        offset 0 means. Plus cycles 8 and 9. -> FAIL 75 x3, FAIL 105 x2
//
//   expected: 1 report at 15, 2 at 45, 6 at 75, and 8 at 105 -- seventeen in
//   all. The end-of-trace split is 1 + 2 + 3 + 2, and every one of those eight
//   is a report the corresponding WEAK operator does not make.
//
// Not vacuous, in both directions. Each signal is low at very few ticks, so all
// four assertions pass far more often than they report: a stuck-firing monitor
// would produce far more than seventeen. And each of the four reports before
// the end of trace as well as at it, so a monitor that only drained at $finish
// -- which is the plausible way to get strength wrong -- would lose nine
// reports while keeping eight. The count at 105 pins the index and the window
// width independently of the mid-trace counts: 1 versus 2 separates
// s_nexttime[1] from s_nexttime[2], and 3 versus 2 separates s_always[1:3]
// from s_always[0:2].
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

  sn1:  assert property (@(posedge clk) s_nexttime [1]   a);
  sn2:  assert property (@(posedge clk) s_nexttime [2]   a);
  sa13: assert property (@(posedge clk) s_always   [1:3] b);
  sa02: assert property (@(posedge clk) s_always   [0:2] b);

  initial begin
    @(posedge clk);            // cyc0: a=1 b=1
    a <= 0;
    @(posedge clk);            // cyc1: a=0
    a <= 1;
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
    @(posedge clk);            // cyc8  quiet tail: everything still open here
    @(posedge clk);            // cyc9  is owed a tick that never comes
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
