// Overlapping implication `|->`.
// The trace is driven with nonblocking assignments so every sampled value is
// unambiguous, and $display marks each cycle so a report can be attributed.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // Overlapping: the consequent is checked in the SAME cycle the antecedent ends.
  ap: assert property (@(posedge clk) a |-> b);

  initial begin
    @(posedge clk);
    a <= 1; b <= 1;   // cyc1: a&&b -> pass
    @(posedge clk);
    a <= 1; b <= 0;   // cyc2: a && !b -> FAIL
    @(posedge clk);
    a <= 0; b <= 0;   // cyc3: no attempt
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
