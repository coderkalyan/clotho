// The ranged `eventually` and `s_eventually` property operators.
//
// Clotho lowers this: both forms reach sva_prop_shift as SVA_SHIFT_SOME, the
// range read disjunctively, with the operator's own spelling setting the
// strength of its subtree.
// See docs/design/sva-property-layer.md, phase 1 (as built).
//
// The RANGED eventually family, LRM 16.12.13. Unranged `s_eventually` is
// unbounded and stays refused (phase 3); unranged weak `eventually` does not
// exist, because "the range for a weak eventually shall be bounded".
//
//   "eventually [constant_range] property_expr evaluates to true if, and only
//    if, either there exists a current or future clock tick within the range
//    ... at which property_expr evaluates to true OR NOT ALL the current or
//    future clock ticks within the range specified by constant_range exist."
//   "s_eventually [cycle_delay_const_range_expression] property_expr evaluates
//    to true if, and only if, there exists a current or future clock tick
//    within the range ... at which property_expr evaluates to true."
//
// The same clause gives the two equivalences that make this easy to reason
// about: `eventually [2:5] a` is `weak(##[2:5] a)` and `s_eventually [2:5] a`
// is `strong(##[2:5] a)`. So an attempt fails when the operand is low at every
// tick of the window, and the weak form additionally forgives a window that
// runs off the end of the trace while the strong form does not.
//
// A failing attempt is only decided at the LAST tick of its window -- until
// then the operand might still come high -- so a report lands at offset n from
// the attempt, not offset m. That is the opposite of `always`, where the report
// lands at the first offset that fails, and it is why this fixture needs a RUN
// of low cycles rather than a single one: a window of width w only fails if w
// consecutive ticks are low.
//
// Two ranges are asserted, on two different signals, so their reports land at
// different times. `[0:1]` is included because offset 0 is the attempt's own
// tick, the corner a desugaring into delays is most likely to misplace.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    e  f
//    0     5    1  1
//    1    15    1  1
//    2    25    1  1
//    3    35    0  1     <- run of three lows on e, cycles 3..5
//    4    45    0  1
//    5    55    0  1
//    6    65    1  0     <- run of two lows on f, cycles 6..7
//    7    75    1  0
//    8    85    1  1
//    9    95    0  0     both low at the last evaluated tick, so that the
//   10   105    1  1     attempts still open here separate weak from strong
//                        ($finish executes at cyc10, which never evaluates)
//
// Derived from the LRM:
//
//   we  eventually   [1:2] e -- attempt at N needs e high at tick N+1 or N+2.
//       Both are low for N = 2 (ticks 3,4) and N = 3 (ticks 4,5); each is
//       decided at the later tick of its window. N = 4 sees tick 6 high and
//       passes. The attempts from cycles 8 and 9 have windows containing tick
//       10, which never evaluates, so "not all the ticks in the range exist"
//       and they hold weakly -- even though e is low at cycle 9.
//                                                     -> FAIL 45, FAIL 55
//   se  s_eventually [1:2] e -- the same two, plus cycles 8 and 9, which find
//       no high e in the ticks that do exist and are owed ticks that never
//       come.                            -> FAIL 45, FAIL 55, FAIL <eot> x2
//   wf  eventually   [0:1] f -- attempt at N needs f high at tick N or N+1.
//       Only N = 6 (ticks 6,7) has both low. N = 7 sees tick 8 high. The
//       attempt from cycle 9 covers ticks 9 and 10; tick 10 never evaluates,
//       so it holds weakly despite f being low at cycle 9.        -> FAIL 75
//   sf  s_eventually [0:1] f -- the same one, plus cycle 9.
//                                                  -> FAIL 75, FAIL <eot> x1
//
//   expected: 2 reports at 45, 2 at 55, 2 at 75, and 3 at the end of trace --
//   nine in all. The end-of-trace split is 2 from s_eventually[1:2] and 1 from
//   s_eventually[0:1]; the weak pair contributes nothing there.
//
// Not vacuous: cycle 9 is the load-bearing cycle. Both operands are LOW there,
// so a weak attempt covering it and a strong one covering it see identical
// operand values and still disagree -- the weak one holds purely because a tick
// in its window is missing. An implementation that treats weak and strong alike
// either loses three reports or gains three, and either way the count moves.
// Away from the end of trace all four assertions both pass and fail, so a
// dead monitor and a stuck-firing one are both distinguishable.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          e   = 1, f = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d e=%b f=%b", $time, cyc, e, f);
  end

  we: assert property (@(posedge clk) eventually   [1:2] e);
  se: assert property (@(posedge clk) s_eventually [1:2] e);
  wf: assert property (@(posedge clk) eventually   [0:1] f);
  sf: assert property (@(posedge clk) s_eventually [0:1] f);

  initial begin
    @(posedge clk);            // cyc0
    @(posedge clk);            // cyc1
    @(posedge clk);            // cyc2
    e <= 0;
    @(posedge clk);            // cyc3: e=0
    @(posedge clk);            // cyc4: e=0
    @(posedge clk);            // cyc5: e=0
    e <= 1; f <= 0;
    @(posedge clk);            // cyc6: e=1 f=0
    @(posedge clk);            // cyc7: f=0
    f <= 1;
    @(posedge clk);            // cyc8: f=1
    e <= 0; f <= 0;
    @(posedge clk);            // cyc9: e=0 f=0, both attempts left open here
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
