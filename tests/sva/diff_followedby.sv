module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          g   = 1, h = 0, k = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d g=%b h=%b k=%b", $time, cyc, g, h, k);
  end

  fo: assert property (@(posedge clk) g #-# h);
  fn: assert property (@(posedge clk) g #=# h);
  fq: assert property (@(posedge clk) (g ##1 h) #=# k);

  initial begin
    @(posedge clk);            // cyc0: g=1 h=0 k=1
    h <= 1;
    @(posedge clk);            // cyc1: h=1
    @(posedge clk);            // cyc2
    h <= 0;
    @(posedge clk);            // cyc3: h=0
    h <= 1;
    @(posedge clk);            // cyc4: h=1
    k <= 0;
    @(posedge clk);            // cyc5: k=0
    k <= 1; g <= 0;
    @(posedge clk);            // cyc6: g=0, so no antecedent matches here
    g <= 1;
    @(posedge clk);            // cyc7  the tail is quiet apart from the two
    @(posedge clk);            // cyc8  fq attempts left open at cycles 8 and
    @(posedge clk);            // cyc9  9, both of which hold
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
