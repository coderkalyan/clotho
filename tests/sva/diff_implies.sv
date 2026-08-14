module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1, b = 0, c = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b", $time, cyc, a, b, c);
  end

  // Three of the four directives this fixture was written for do not lower
  // today, and the reason is a real restriction rather than a gap in the
  // fixture. `implies` reduces to `(not p) or q`, `not` inverts the strength of
  // the operand it complements, and a monitor carries ONE end-of-trace
  // disposition -- so the reduction can only be tracked when the two operand
  // strengths DIFFER, and `iff`, which obliges both directions at once, can
  // never satisfy that for any pair. libsva refuses both with
  // SVA_ERROR_MIXED_STRENGTH, naming the operator the user wrote.
  //
  // Their derivations are kept above rather than deleted, because the lift is
  // known and scoped: a per-state end-of-trace disposition, OR-combined over
  // the live vector, retires MIXED_STRENGTH entirely. When it lands, restore
  // these three lines and score the header's full table.
  //
  //   im:  assert property (@(posedge clk)        (a ##[1:3] b) implies c);
  //   ii:  assert property (@(posedge clk)        (a ##[1:3] b) iff     c);
  //   iis: assert property (@(posedge clk) strong (a ##[1:3] b) iff     c);
  ims: assert property (@(posedge clk) strong (a ##[1:3] b) implies c);

  ic:  assert property (@(posedge clk)        (a ##[1:3] b) |->     c);

  initial begin
    @(posedge clk);            // cyc0: a=1 b=0 c=0
    b <= 1; c <= 1;
    @(posedge clk);            // cyc1: a=1 b=1 c=1  first antecedent match tick
    a <= 0; b <= 0;
    @(posedge clk);            // cyc2: a=0 b=0 c=1  antecedent false at its own
    a <= 1; c <= 0;            //                    start tick, consequent high
    @(posedge clk);            // cyc3: a=1 b=0 c=0
    b <= 1;
    @(posedge clk);            // cyc4: a=1 b=1 c=0  last antecedent match tick,
    b <= 0; c <= 1;            //                    reached from cyc1 and cyc3
    @(posedge clk);            // cyc5: a=1 b=0 c=1  b never rises again
    a <= 0; c <= 0;
    @(posedge clk);            // cyc6: a=0 b=0 c=0
    a <= 1;
    @(posedge clk);            // cyc7: a=1 b=0 c=0  a stays high and b stays low
    @(posedge clk);            // cyc8: a=1 b=0 c=0  to the end, so attempts 7, 8
    c <= 1;                    //                    and 9 are pending at $finish
    @(posedge clk);            // cyc9: a=1 b=0 c=1  with c low, low and HIGH --
                               //                    which is what makes the weak
                               //                    and strong `iff` fail on
                               //                    disjoint attempts
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
