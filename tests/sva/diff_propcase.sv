// The `case` property form.
//
// Clotho lowers this: the `case` property form desugars to a disjunction of
// followed-bys, one per item, each guarded by its own labels matching AND no
// earlier item's, with an absent default standing for the true property.
// See docs/design/sva-property-layer.md, phase 1 (as built).
//
// The other half of the guarded-selection family, the `if`/`else` property
// form, is in diff_propif.sv.
//
// LRM 16.12.16:
//
//   "During the linear search, if one of the case item expressions matches the
//    case expression ... then the property statement associated with that case
//    item shall be evaluated, and the linear search shall terminate. If there
//    is a default case item, it is ignored during this linear search. If all
//    comparisons fail and the default item is given, then the default item
//    property statement shall be executed. IF THE DEFAULT PROPERTY STATEMENT
//    IS NOT GIVEN AND ALL OF THE COMPARISONS FAIL, then none of the case item
//    property statements shall be evaluated and the evaluation of the case
//    property statement from that start point SUCCEEDS AND RETURNS TRUE
//    (VACUOUSLY)."
//
// That last sentence is the reason `cs` and `csd` are both here. They are the
// same case statement apart from a `default: 1'b0`, so every difference between
// their report sets is attributable to the default arm alone. Cycles 5 and 6,
// where sel selects no item, are where they separate.
//
// sel is a 2-state `bit [1:0]`, so 16.12.16's deferral to the 12.5 comparison
// rules -- which are bit-for-bit and x/z-sensitive -- can never be reached with
// an unknown. That is deliberate: the x/z corner of a case property is a
// separate question and mixing it in would make an unexpected report ambiguous.
//
// Both arms are plain booleans, so no attempt outlives its own cycle and the
// form is not strength-sensitive; end-of-trace behaviour belongs to
// diff_strongprop.sv and diff_eventuallyrange.sv. The tail is quiet.
//
// Sampled values, one row per posedge (cycle N is at t = 5 + 10N):
//
//   cyc    t   sel  x  y
//    0     5    0   1  1
//    1    15    0   1  1
//    2    25    0   0  1    <- item 0 selected and low: BOTH report
//    3    35    1   1  1
//    4    45    1   1  0    <- item 1 selected and low: BOTH report
//    5    55    2   0  0    <- no item matches: cs holds, csd runs the default
//    6    65    3   0  0    <- no item matches: cs holds, csd runs the default
//    7    75    0   1  1
//    8    85    1   1  1
//    9    95    0   1  1    last evaluated tick
//   10   105    0   1  1    $finish executes here, so this tick never evaluates
//
// x and y are BOTH low at cycles 5 and 6, on purpose. If an implementation
// wrongly evaluated an unselected item it would have a low operand to trip
// over, so `cs` holding there is evidence of vacuity rather than an accident of
// the operands happening to be high. Cycles 2 and 4 are the converse control:
// the item that is NOT selected is high while the selected one is low, so a
// report there cannot come from the wrong arm.
//
// Derived from the LRM:
//
//   cs   case (sel) 2'd0: x; 2'd1: y; endcase
//        sel picks x at cycle 2 (low) and y at cycle 4 (low). At cycles 5 and 6
//        sel is 2 and 3, no item matches and there is no default, so both hold
//        vacuously.                                    -> FAIL 25, FAIL 45
//   csd  case (sel) 2'd0: x; 2'd1: y; default: 1'b0; endcase
//        the same two, plus cycles 5 and 6, where the default arm runs and is
//        a constant false.             -> FAIL 25, FAIL 45, FAIL 55, FAIL 65
//
//   expected: 2 reports at 25, 2 at 45, 1 at 55, 1 at 65 -- six in all. t=55
//   and t=65 name the default arm outright.
//
// Not vacuous: both assertions pass on six of the ten evaluated cycles and
// report on at least two. An implementation that treats an unmatched case as a
// failure gains two reports at 55 and 65 under `cs`; one that drops the default
// loses those two under `csd`; one that is simply dead loses all six.
//
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit [1:0]    sel = 2'd0;
  bit          x   = 1, y = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d sel=%0d x=%b y=%b", $time, cyc, sel, x, y);
  end

  cs:  assert property (@(posedge clk) case (sel)
                                         2'd0: x;
                                         2'd1: y;
                                       endcase);
  csd: assert property (@(posedge clk) case (sel)
                                         2'd0:    x;
                                         2'd1:    y;
                                         default: 1'b0;
                                       endcase);

  initial begin
    @(posedge clk);                  // cyc0: sel=0 x=1 y=1
    @(posedge clk);                  // cyc1
    x <= 0;
    @(posedge clk);                  // cyc2: sel=0 x=0 y=1
    sel <= 2'd1; x <= 1;
    @(posedge clk);                  // cyc3: sel=1 x=1 y=1
    y <= 0;
    @(posedge clk);                  // cyc4: sel=1 x=1 y=0
    sel <= 2'd2; x <= 0;
    @(posedge clk);                  // cyc5: sel=2 x=0 y=0
    sel <= 2'd3;
    @(posedge clk);                  // cyc6: sel=3 x=0 y=0
    sel <= 2'd0; x <= 1; y <= 1;
    @(posedge clk);                  // cyc7  quiet tail, so that nothing
    sel <= 2'd1;                     //       reports in the same slot as
    @(posedge clk);                  // cyc8  $finish
    sel <= 2'd0;
    @(posedge clk);                  // cyc9
    @(posedge clk);                  // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
