module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b", $time, cyc, a);
  end

  se: assert property (@(posedge clk) s_eventually       a);
  we: assert property (@(posedge clk) eventually   [1:2] a);

  ce: cover  property (@(posedge clk) s_eventually       a)
        $display("[%0t] COVER ce", $time);

  initial begin
    @(posedge clk);            // cyc0: a=0
    @(posedge clk);            // cyc1: a=0
    @(posedge clk);            // cyc2: a=0
    a <= 1;
    @(posedge clk);            // cyc3: a=1  four attempts of `se` end here
    a <= 0;
    @(posedge clk);            // cyc4: a=0
    a <= 1;
    @(posedge clk);            // cyc5: a=1  two more end here
    a <= 0;
    @(posedge clk);            // cyc6: a=0  a stays low for the rest of the
    @(posedge clk);            // cyc7: a=0  trace, so every attempt from here
    @(posedge clk);            // cyc8: a=0  on is still looking when the clock
    @(posedge clk);            // cyc9: a=0  stops
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
