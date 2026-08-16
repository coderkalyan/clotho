// A named sequence with formal arguments is instantiated as the antecedent of an
// implication, and a named property wraps a second implication, so both layers
// of AssertionInstanceExpression::body are exercised -- a sequence instance
// lowers at the sequence layer, a property instance at the property layer.
// Both formals of `ab` are substituted differently by the two uses.
//
// Each assertion passes once and fails once, so a monitor that never fires is
// distinguishable from one that fires correctly.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, d = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b d=%b", $time, cyc, a, b, c, d);
  end

  sequence ab(x, y);
    x ##1 y;
  endsequence

  property implies_next(x, y, z);
    ab(x, y) |=> z;
  endproperty

  // Overlapping: the consequent is checked in the cycle the antecedent ends.
  ap: assert property (@(posedge clk) ab(a, b) |-> c);
  // Nonoverlapping, and the implication itself lives inside the declaration.
  bp: assert property (@(posedge clk) implies_next(a, b, d));

  initial begin
    @(posedge clk);                  // cyc0
    a <= 1;                          // cyc1: antecedent starts
    @(posedge clk);                  // cyc1
    a <= 0; b <= 1; c <= 1;          // cyc2: ab(a,b) matches, c holds -> ap passes
    @(posedge clk);                  // cyc2
    b <= 0; c <= 0; a <= 1; d <= 1;  // cyc3: d holds -> bp passes
    @(posedge clk);                  // cyc3
    a <= 0; b <= 1; d <= 0;          // cyc4: ab(a,b) matches, no c -> ap FAILS
    @(posedge clk);                  // cyc4
    b <= 0;                          // cyc5: no d -> bp FAILS
    @(posedge clk);                  // cyc5
    @(posedge clk);                  // cyc6
    $finish;
  end
endmodule
