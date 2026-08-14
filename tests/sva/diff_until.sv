module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1, b = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end

  u:   assert property (@(posedge clk) a until        b);
  su:  assert property (@(posedge clk) a s_until      b);
  uw:  assert property (@(posedge clk) a until_with   b);
  suw: assert property (@(posedge clk) a s_until_with b);

  cs:  cover  property (@(posedge clk) a s_until      b)
         $display("[%0t] COVER cs", $time);

  initial begin
    @(posedge clk);            // cyc0: a=1 b=0
    @(posedge clk);            // cyc1: a=1 b=0
    a <= 0;
    @(posedge clk);            // cyc2: a=0 b=0
    a <= 1; b <= 1;
    @(posedge clk);            // cyc3: a=1 b=1
    b <= 0;
    @(posedge clk);            // cyc4: a=1 b=0
    a <= 0; b <= 1;
    @(posedge clk);            // cyc5: a=0 b=1  the overlap corner
    a <= 1; b <= 0;
    @(posedge clk);            // cyc6  b never rises again, so the four
    @(posedge clk);            // cyc7  attempts starting here are still
    @(posedge clk);            // cyc8  waiting when the clock stops: weakly
    @(posedge clk);            // cyc9  they hold, strongly they fail
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
