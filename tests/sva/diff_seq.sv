module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // A bare sequence property: an attempt starts EVERY cycle, so several are in
  // flight at once and each must be tracked separately.
  ap: assert property (@(posedge clk) a ##1 b);

  initial begin
    @(posedge clk);
    a <= 1;            // cyc1: a -> attempt from cyc1 needs b at cyc2
    @(posedge clk);
    a <= 0; b <= 1;    // cyc2: b -> that attempt passes
    @(posedge clk);
    b <= 0; a <= 1;    // cyc3
    @(posedge clk);
    a <= 0;            // cyc4: no b -> FAIL for the cyc3 attempt
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
