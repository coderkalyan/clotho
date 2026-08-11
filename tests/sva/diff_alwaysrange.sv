module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1, b = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end

  wa: assert property (@(posedge clk) always [1:3] a);
  wb: assert property (@(posedge clk) always [0:2] b);

  initial begin
    @(posedge clk);            // cyc0
    @(posedge clk);            // cyc1
    @(posedge clk);            // cyc2
    @(posedge clk);            // cyc3
    a <= 0;
    @(posedge clk);            // cyc4: a=0
    a <= 1;
    @(posedge clk);            // cyc5
    @(posedge clk);            // cyc6
    b <= 0;
    @(posedge clk);            // cyc7: b=0
    b <= 1;
    @(posedge clk);            // cyc8  quiet tail: the attempts left open here
    @(posedge clk);            // cyc9  all hold, weakly
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
