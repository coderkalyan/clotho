// $past's gating expression (LRM 16.9.3). diff_past.sv covers $past itself; this
// file covers the third argument, and nothing else. Every assertion below reads
// the same variable over the same trace, so the only thing that can move a
// report is which TICKS each one's history counted.
//
// LRM 16.9.3, on $past(expression1, number_of_ticks, expression2):
//
//   "expression2 is used as a gating expression for the clocking event."
//
//   "If number_of_ticks equals k and if ev is the event expression underlying
//    clocking_event, then the particular time step is the kth strictly prior
//    time step in which the event ev iff expression2 occurred. If there do not
//    exist k strictly prior time steps in which the event ev iff expression2
//    occurred, then the value returned from the $past function is the default
//    sampled value of expression1."
//
// So the gate does not select a value out of the history: it decides which ticks
// the history counts at all. A tick the gate holds back is a tick that, for that
// $past, never happened -- the depth-k window slides over the gated subsequence
// of ticks, not over the clock.
//
// `v` is 2-state and starts at 0, deliberately. LRM 16.5.1 makes the default
// sampled value of a 2-state type 0, and v's value at time zero is also 0, so the
// no-k-prior-ticks case above reads 0 under both the LRM's rule and the
// time-zero seeding clotho implements (docs/design/concurrent-assertions.md
// 4.1). The two answers coincide here, so this fixture cannot be read as
// evidence for either, and keeping that question out of this file is what lets
// every line below be derived from 16.9.3 alone.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N). A row shows
// what the monitor SEES at that tick, which is each variable's value just before
// the edge; the $display prints the same values, because it runs in Active and
// the trace's nonblocking assignments do not land until NBA.
//
//   cyc    t     v    g    h
//    0     5     0    1    0
//    1    15     1    1    0
//    2    25     0    0    0
//    3    35     3    0    1
//    4    45     4    1    1
//    5    55     1    1    1
//    6    65     5    0    1
//    7    75     1    1    1
//    8    85     6    1    1
//    9    95     6    1    1
//   10   105     -    -    -    $finish executes here, so this tick never evaluates
//
// g is low at cycles 2, 3 and 6: one run of two and one lone tick, so a gate
// handled as "skip one" and a gate handled as "skip a run" are told apart. h is
// low at cycles 0, 1 and 2, which is the other boundary -- a gated $past whose
// gate has not yet been true even once, where 16.9.3 falls back to the default
// sampled value.
//
// Each ring's contents, derived by hand. `entry k` is the value $past(v, k+1, .)
// reads at that tick, and the shift happens after the read (16.9.3 says STRICTLY
// prior), so a row shows what the tick reads and the next row shows the effect of
// that tick's shift.
//
//   ungated (u1 reads entry 0, u2 entry 1)
//     cyc      0    1    2    3    4    5    6    7    8    9
//     entry0   0    0    1    0    3    4    1    5    1    6
//     entry1   0    0    0    1    0    3    4    1    5    1
//
//   gated on g (g1 reads entry 0, g2 entry 1); cycles 2, 3 and 6 shift nothing
//     cyc      0    1    2    3    4    5    6    7    8    9
//     entry0   0    0    1    1    1    4    1    1    1    6
//     entry1   0    0    0    0    0    1    4    4    1    1
//
//   gated on h (gh reads entry 0); cycles 0, 1 and 2 shift nothing, so the first
//   three ticks all read the default sampled value
//     cyc      0    1    2    3    4    5    6    7    8    9
//     entry0   0    0    0    0    3    4    1    5    1    6
//
// Derived from the LRM. Each assertion reports at the ticks where its history
// repeats the current sample:
//
//   u1  $past(v) != v            -- 0==0 at cyc0, 6==6 at cyc9
//                                                          -> FAIL 5, 95
//   u2  $past(v, 2) != v         -- 0==0 at cyc0, 0==0 at cyc2 (entry1 is still
//       the seed there), 1==1 at cyc7                      -> FAIL 5, 25, 75
//   g1  $past(v, 1, g) != v      -- 0==0 at cyc0, 6==6 at cyc9, and 1==1 at cyc7
//       because the cycle-6 shift was gated off, so entry0 is still the cycle-5
//       sample of 1 rather than the cycle-6 sample of 5. That extra report at 75
//       is the whole point of the file: u1 passes there.
//                                                          -> FAIL 5, 75, 95
//   g2  $past(v, 2, g) != v      -- 0==0 at cyc0 and cyc2 as for u2, then 1==1 at
//       cyc5: two gated ticks back from cycle 5 is cycle 1, not cycle 3, because
//       cycles 2 and 3 did not count. u2 passes at 55 and reports at 75 instead,
//       so the two disagree at BOTH ends.                  -> FAIL 5, 25, 55
//   gh  $past(v, 1, h) != v      -- 0==0 at cyc0, and again at cyc2, where h has
//       never yet been true and 16.9.3 gives back the default sampled value of 0
//       while v has come back down to 0. Then 6==6 at cyc9. u1 passes at 25.
//                                                          -> FAIL 5, 25, 95
//
//   expected: 5 reports at t=5, 3 at t=25, 1 at t=55, 2 at t=75, 3 at t=95 --
//   fourteen in all, and NOTHING at 105.
//
// Not vacuous: every assertion passes at six or more of the ten evaluated ticks
// and reports at two or three, and no two of the five have the same report set.
// A monitor that ignored the gate entirely would score u1's set for g1 and gh and
// u2's for g2, losing 55 and gaining 75 -- a difference at three separate ticks,
// in both directions.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit [3:0]    v   = 0;
  bit          g   = 1;
  bit          h   = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d v=%0d g=%b h=%b", $time, cyc, v, g, h);
  end

  u1: assert property (@(posedge clk) $past(v)       != v);
  u2: assert property (@(posedge clk) $past(v, 2)    != v);
  g1: assert property (@(posedge clk) $past(v, 1, g) != v);
  g2: assert property (@(posedge clk) $past(v, 2, g) != v);
  gh: assert property (@(posedge clk) $past(v, 1, h) != v);

  initial begin
    @(posedge clk);            // cyc0: v=0 g=1 h=0
    v <= 1;
    @(posedge clk);            // cyc1: v=1 g=1 h=0
    v <= 0;  g <= 0;
    @(posedge clk);            // cyc2: v=0 g=0 h=0
    v <= 3;  h <= 1;
    @(posedge clk);            // cyc3: v=3 g=0 h=1
    v <= 4;  g <= 1;
    @(posedge clk);            // cyc4: v=4 g=1 h=1
    v <= 1;
    @(posedge clk);            // cyc5: v=1 g=1 h=1
    v <= 5;  g <= 0;
    @(posedge clk);            // cyc6: v=5 g=0 h=1
    v <= 1;  g <= 1;
    @(posedge clk);            // cyc7: v=1 g=1 h=1
    v <= 6;
    @(posedge clk);            // cyc8: v=6 g=1 h=1
    @(posedge clk);            // cyc9: v=6 g=1 h=1
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
