// The four value change functions of LRM 16.9.3: $rose, $fell, $stable and
// $changed. Each is asserted bare, so each reports on exactly the ticks where it
// does NOT hold, and the four report sets between them pin the whole semantics.
//
// Three things are deliberately in the trace:
//
//  * `v` is 4-state and never initialised, so it is x until the second tick.
//    That exercises the edge order 0 < x/z < 1 that LRM 16.9.3 shares with
//    @(posedge): x -> 1 IS a rise, and x -> x is no edge at all.
//  * `v` is four bits wide and bit 0 does not follow the whole value, so
//    $rose/$fell (LSB only) and $stable/$changed (whole value) disagree about
//    which ticks changed. 0011 -> 0010 changes the value while bit 0 falls.
//  * `noise` toggles every 3 against a clock of 5, so most time steps carry no
//    clocking edge. History advances per CLOCK TICK, so those steps must not
//    disturb any of the four.
//
// Expectations below were derived from the LRM.
// Samples, one per tick, are the value the previous time step ended with:
//
//   tick   t=5    t=15   t=25   t=35   t=45
//   prev   xxxx   xxxx   0001   0011   0010     (t=5's prev is the time-zero seed)
//   cur    xxxx   0001   0011   0010   0010
//   rose    -      1      -      -      -       (LSB x->1 only)
//   fell    -      -      -      1      -       (LSB 1->0 only)
//   stable  1      -      -      -      1
//   changed -      1      1      1      -
//
// so, reporting where each does not hold:
//
//   a_rose     5 25 35 45
//   a_fell     5 15 25 45
//   a_stable   15 25 35
//   a_changed  5 45
//
// which is 3 reports at t=5, 2 at t=15, 3 at t=25, 2 at t=35 and 3 at t=45.
// t=55 is where $finish lands, and a tick whose slot ends before the Observed
// region never evaluates, so it contributes nothing.
module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit noise = 0;
  always #3 noise = ~noise;

  logic [3:0]  v;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d v=%b noise=%b", $time, cyc, v, noise);
  end

  a_rose:    assert property (@(posedge clk) $rose(v));
  a_fell:    assert property (@(posedge clk) $fell(v));
  a_stable:  assert property (@(posedge clk) $stable(v));
  a_changed: assert property (@(posedge clk) $changed(v));

  initial begin
    @(posedge clk);            // tick 1 samples xxxx
    v <= 4'b0001;
    @(posedge clk);            // tick 2 samples 0001
    v <= 4'b0011;
    @(posedge clk);            // tick 3 samples 0011
    v <= 4'b0010;
    @(posedge clk);            // tick 4 samples 0010
    @(posedge clk);            // tick 5 samples 0010
    @(posedge clk);            // tick 6: $finish, never evaluated
    $finish;
  end
endmodule
