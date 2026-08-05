module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // A delay range: the obligation stays pending across several cycles, so the
  // attempt ring and the filler states are both exercised.
  ap: assert property (@(posedge clk) a |=> b ##[1:2] c);

  initial begin
    @(posedge clk);
    a <= 1;                     // cyc1: seeds an obligation
    @(posedge clk);
    a <= 0; b <= 1;             // cyc2: b holds
    @(posedge clk);
    b <= 0; c <= 1;             // cyc3: c holds -> pass at the ##1 branch
    @(posedge clk);
    c <= 0; a <= 1;             // cyc4: seed again
    @(posedge clk);
    a <= 0; b <= 1;             // cyc5: b
    @(posedge clk);
    b <= 0;                     // cyc6: no c
    @(posedge clk);
    @(posedge clk);             // cyc7: still no c -> FAIL
    @(posedge clk);
    $finish;
  end
endmodule
