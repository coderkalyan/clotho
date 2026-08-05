module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // Boolean structure: a conjunction splits into cube literals, a disjunction
  // stays one root, and a negation becomes its own atom under NNF.
  ap1: assert property (@(posedge clk) (a && !b) |=> c);
  ap2: assert property (@(posedge clk) (a || b) |=> c);

  initial begin
    @(posedge clk);
    a <= 1; b <= 0;      // cyc1: both antecedents hold
    @(posedge clk);
    a <= 0; b <= 0; c <= 1; // cyc2: c -> both pass
    @(posedge clk);
    c <= 0; a <= 0; b <= 1; // cyc3: only ap2's antecedent holds
    @(posedge clk);
    b <= 0;              // cyc4: no c -> ap2 FAILs, ap1 does not
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
