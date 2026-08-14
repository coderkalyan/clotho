module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b", $time, cyc, a);
  end

  wa: assert property (@(posedge clk) always         a);
  sa: assert property (@(posedge clk) s_always [0:2] a);

  initial begin
    @(posedge clk);            // cyc0: a=1
    @(posedge clk);            // cyc1: a=1
    a <= 0;
    @(posedge clk);            // cyc2: a=0
    a <= 1;
    @(posedge clk);            // cyc3: a=1  the four-tick gap to the next low
    @(posedge clk);            // cyc4  is what separates an unbounded reach
    @(posedge clk);            // cyc5  from the twin's [0:2] window
    @(posedge clk);            // cyc6: a=1
    a <= 0;
    @(posedge clk);            // cyc7: a=0
    a <= 1;
    @(posedge clk);            // cyc8: a=1  a stays high, so these two attempts
    @(posedge clk);            // cyc9: a=1  are open when the clock stops
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
