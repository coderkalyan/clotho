// The `if` and `if`/`else` property forms.
//
// Clotho lowers this: the `if`/`else` property form desugars to a disjunction of
// followed-bys, `(c #-# p) or (!c #-# q)`, with an unwritten `else` standing for
// the true property.
// See docs/design/sva-property-layer.md, phase 1 (as built).
//
// The sibling `case` property form is the other half of this family, and lives
// in diff_propcase.sv.
//
// LRM 16.12.6:
//
//   "A property of the first form [if (c) p] evaluates to true if, and only
//    if, EITHER expression_or_dist EVALUATES TO FALSE or property_expr
//    evaluates to true. A property of the second form [if (c) p else q]
//    evaluates to true if, and only if, either expression_or_dist evaluates to
//    true and property_expr1 evaluates to true or expression_or_dist evaluates
//    to false and property_expr2 evaluates to true."
//
// So the else-less form is VACUOUSLY TRUE when the guard is false, and adding
// an `else` is precisely what makes a false guard checkable. That difference is
// the whole content of this fixture, and it is tested rather than assumed:
// cycles 3 and 6 have the guard low AND the else-branch low, so `ifn` holds and
// `ife` reports ON THE SAME CYCLE FROM THE SAME OPERANDS. A silent `ifn` there
// therefore cannot be confused with a monitor that is not running -- its
// sibling proves the cycle is live.
//
// Both branches are plain booleans, so no attempt outlives the cycle it starts
// in and neither form is strength-sensitive. End-of-trace behaviour is not
// exercised here BY DESIGN; it belongs to the operators that carry a strength,
// and is covered in diff_eventuallyrange.sv and diff_strongprop.sv. The last
// two cycles are quiet so nothing shares a slot with $finish.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t    c  a  b
//    0     5    1  1  1
//    1    15    1  0  1    <- guard high, then-branch low: BOTH report
//    2    25    0  0  1    <- guard low, else-branch high: neither reports,
//                             and `a` is low, so a form that wrongly evaluated
//                             the then-branch under a false guard would trip
//    3    35    0  1  0    <- guard low, else-branch low: ONLY ife reports
//    4    45    1  1  0    <- guard high and satisfied; the low else-branch is
//                             not looked at
//    5    55    1  0  0    <- guard high, then-branch low: BOTH report
//    6    65    0  0  0    <- guard low, else-branch low: ONLY ife reports
//    7    75    1  1  1
//    8    85    0  0  1
//    9    95    1  1  1    last evaluated tick
//   10   105    1  1  1    $finish executes here, so this tick never evaluates
//
// Cycles 2 and 8 are the mirror of cycles 3 and 6 and are there to catch the
// opposite mistake: guard low with the then-branch low but the else-branch
// high, where BOTH forms must stay silent. Between the two pairs, an
// implementation that evaluates the wrong branch under a false guard reports
// where this fixture expects silence, and one that drops the else reports two
// fewer.
//
// Derived from the LRM:
//
//   ifn  if (c) a         -- reports only where c is high and a is low, which
//        is cycles 1 and 5. Cycles 3 and 6 have a low else-branch but there is
//        no else, so they hold vacuously.             -> FAIL 15, FAIL 55
//   ife  if (c) a else b  -- cycles 1 and 5 as above, plus cycles 3 and 6,
//        where c is low and b is low.
//                                 -> FAIL 15, FAIL 35, FAIL 55, FAIL 65
//
//   expected: 2 reports at 15, 1 at 35, 2 at 55, 1 at 65 -- six in all. t=35
//   and t=65 name `ife`'s else-branch outright.
//
// Not vacuous: both assertions pass on most cycles (`ifn` on eight of ten) and
// both report at least twice. A monitor that never fires loses six reports and
// one that fires every cycle gains fourteen.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          c   = 1, a = 1, b = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d c=%b a=%b b=%b", $time, cyc, c, a, b);
  end

  ifn: assert property (@(posedge clk) if (c) a);
  ife: assert property (@(posedge clk) if (c) a else b);

  initial begin
    @(posedge clk);                  // cyc0: c=1 a=1 b=1
    a <= 0;
    @(posedge clk);                  // cyc1: c=1 a=0 b=1
    c <= 0;
    @(posedge clk);                  // cyc2: c=0 a=0 b=1
    a <= 1; b <= 0;
    @(posedge clk);                  // cyc3: c=0 a=1 b=0
    c <= 1;
    @(posedge clk);                  // cyc4: c=1 a=1 b=0
    a <= 0;
    @(posedge clk);                  // cyc5: c=1 a=0 b=0
    c <= 0;
    @(posedge clk);                  // cyc6: c=0 a=0 b=0
    c <= 1; a <= 1; b <= 1;
    @(posedge clk);                  // cyc7: c=1 a=1 b=1
    c <= 0; a <= 0;
    @(posedge clk);                  // cyc8: c=0 a=0 b=1  quiet tail, so that
    c <= 1; a <= 1;                  //       nothing reports in the same slot
    @(posedge clk);                  // cyc9: c=1 a=1 b=1  as $finish
    @(posedge clk);                  // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
