// The clocking event is declared INSIDE the named sequence or property rather
// than on the assertion (LRM 16.13.1, clock flow), so nothing at the top of
// `assert property (...)` says what clocks it. Three shapes:
//
//   ap  a named sequence carrying its own clock, used bare
//   bp  the same sequence as the antecedent of an implication. A clock does NOT
//       flow from an antecedent into a consequent -- 16.13.1 hands the
//       consequent the clock from OUTSIDE the operator, and there is none here
//       -- so the consequent spells the same event again. Two clocking events,
//       written twice, one clock: they are compared for equivalence and not for
//       identity, because slang expands each use into its own bound tree.
//   cp  a named property carrying its own clock, wrapped around an implication,
//       so the event is stepped over at the property layer, not the sequence one
//
// The clock is deliberately a NEGEDGE, and $display runs on BOTH edges. A
// resolver that found the declaration's event but dropped its edge would report
// half a period away, and with only one $display per period those reports would
// be attributed to the same cycle as the correct ones and read as agreement.
// Marking both edges makes a wrong edge land on its own timestamp.
//
// Sampled values, one row per negedge (cycle N is at t = 10 + 10N):
//
//   cyc   t   a b c d
//    0   10   0 0 0 0
//    1   20   1 0 0 0
//    2   30   0 1 0 0
//    3   40   0 0 1 0
//    4   50   1 0 0 1
//    5   60   0 1 0 0
//    6   70   0 0 0 0
//    7   80   0 0 1 0
//    8   90   0 0 0 0
//    9  100   1 1 0 0
//   10  110   1 1 0 0   $finish
//
// Derived from that:
//
//   ap  an attempt starts every cycle. It fails in cycle N when a is low there,
//       and in cycle N+1 when a was high and b is low. a is low in cycles
//       0,2,3,5,6,7,8 -> reports at 10, 30, 40, 60, 70, 80, 90. The attempts
//       from cycles 1, 4 and 9 all find their b and pass. The attempt from
//       cycle 10 is still open when the trace ends and the property is weak, so
//       it does not report.
//   bp  the antecedent matches at cycles 2, 5 and 10. c holds at cycle 3 (pass),
//       is low at cycle 6 (report at 70), and cycle 11 never happens (weak, no
//       report).
//   cp  c holds at cycles 3 and 7. d holds at cycle 4 (pass) and is low at
//       cycle 8 (report at 90).
//
//   expected: FAIL 10 30 40 60 70(ap) 70(bp) 80 90(ap) 90(cp) -- nine reports,
//   ap before bp before cp within a time step, which is source order.
//
// Every assertion passes at least once and reports at least once, so a monitor
// that never fires and one that always fires are both distinguishable from a
// correct one. The trace is driven with nonblocking assignments so every
// sampled value is unambiguous.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, d = 0;
  int unsigned cyc = 0;
  always @(clk)
    $display("[%0t] clk=%b cyc=%0d a=%b b=%b c=%b d=%b", $time, clk, cyc, a, b, c, d);
  always @(negedge clk) cyc <= cyc + 1;

  sequence sab;
    @(negedge clk) a ##1 b;
  endsequence

  property pcd;
    @(negedge clk) c |=> d;
  endproperty

  // Bare: an attempt starts every cycle, so several are in flight at once.
  ap: assert property (sab);
  // The same sequence as an antecedent, against a consequent clocked to match.
  bp: assert property (sab |=> @(negedge clk) c);
  // The declaration wraps the whole implication.
  cp: assert property (pcd);

  initial begin
    @(negedge clk);              // cyc0: quiet, so ap's first attempt reports
    a <= 1;                      // cyc1: sab starts
    @(negedge clk);              // cyc1
    a <= 0; b <= 1;              // cyc2: sab matches -> bp needs c at cyc3
    @(negedge clk);              // cyc2
    b <= 0; c <= 1;              // cyc3: c -> bp passes; cp needs d at cyc4
    @(negedge clk);              // cyc3
    c <= 0; d <= 1; a <= 1;      // cyc4: d -> cp passes; sab starts again
    @(negedge clk);              // cyc4
    a <= 0; d <= 0; b <= 1;      // cyc5: sab matches -> bp needs c at cyc6
    @(negedge clk);              // cyc5
    b <= 0;                      // cyc6: no c -> bp reports
    @(negedge clk);              // cyc6
    c <= 1;                      // cyc7: cp needs d at cyc8
    @(negedge clk);              // cyc7
    c <= 0;                      // cyc8: no d -> cp reports
    @(negedge clk);              // cyc8
    a <= 1; b <= 1;              // cyc9: quiet the tail so nothing reports in
    @(negedge clk);              // cyc9   the same time slot as $finish, where
    @(negedge clk);              // cyc10  the region a report lands in is moot
    $finish;
  end
endmodule
