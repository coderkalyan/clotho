module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          c   = 1, a = 1, b = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d c=%b a=%b b=%b", $time, cyc, c, a, b);
  end

  ifn: assert property (@(posedge clk) if (c) a);
  ife: assert property (@(posedge clk) if (c) a else b);

  initial begin
    @(posedge clk);                  // cyc0: c=1 a=1 b=1
    a <= 0;
    @(posedge clk);                  // cyc1: c=1 a=0 b=1
    c <= 0;
    @(posedge clk);                  // cyc2: c=0 a=0 b=1
    a <= 1; b <= 0;
    @(posedge clk);                  // cyc3: c=0 a=1 b=0
    c <= 1;
    @(posedge clk);                  // cyc4: c=1 a=1 b=0
    a <= 0;
    @(posedge clk);                  // cyc5: c=1 a=0 b=0
    c <= 0;
    @(posedge clk);                  // cyc6: c=0 a=0 b=0
    c <= 1; a <= 1; b <= 1;
    @(posedge clk);                  // cyc7: c=1 a=1 b=1
    c <= 0; a <= 0;
    @(posedge clk);                  // cyc8: c=0 a=0 b=1  quiet tail, so that
    c <= 1; a <= 1;                  //       nothing reports in the same slot
    @(posedge clk);                  // cyc9: c=1 a=1 b=1  as $finish
    @(posedge clk);                  // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
