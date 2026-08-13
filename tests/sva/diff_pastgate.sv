module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit [3:0]    v   = 0;
  bit          g   = 1;
  bit          h   = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d v=%0d g=%b h=%b", $time, cyc, v, g, h);
  end

  u1: assert property (@(posedge clk) $past(v)       != v);
  u2: assert property (@(posedge clk) $past(v, 2)    != v);
  g1: assert property (@(posedge clk) $past(v, 1, g) != v);
  g2: assert property (@(posedge clk) $past(v, 2, g) != v);
  gh: assert property (@(posedge clk) $past(v, 1, h) != v);

  initial begin
    @(posedge clk);            // cyc0: v=0 g=1 h=0
    v <= 1;
    @(posedge clk);            // cyc1: v=1 g=1 h=0
    v <= 0;  g <= 0;
    @(posedge clk);            // cyc2: v=0 g=0 h=0
    v <= 3;  h <= 1;
    @(posedge clk);            // cyc3: v=3 g=0 h=1
    v <= 4;  g <= 1;
    @(posedge clk);            // cyc4: v=4 g=1 h=1
    v <= 1;
    @(posedge clk);            // cyc5: v=1 g=1 h=1
    v <= 5;  g <= 0;
    @(posedge clk);            // cyc6: v=5 g=0 h=1
    v <= 1;  g <= 1;
    @(posedge clk);            // cyc7: v=1 g=1 h=1
    v <= 6;
    @(posedge clk);            // cyc8: v=6 g=1 h=1
    @(posedge clk);            // cyc9: v=6 g=1 h=1
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
