module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // Consecutive repetition with a fixed count: `b[*2]` is two back-to-back
  // cycles of b, so the obligation stays pending for exactly two cycles and a
  // single b is a failure rather than a match.
  ap2: assert property (@(posedge clk) a |=> b[*2]);

  // Consecutive repetition with a range. A weak sequence consequent passes on
  // the FIRST match, so what the range buys is the trailing `##1 c`: b may run
  // one, two or three cycles before c, and a c arriving only after a fourth b
  // is out of reach. The failing attempt below is exactly that -- c does come,
  // one cycle later than `[*1:3]` can wait for it -- so it pins the upper
  // bound rather than merely showing that a missing c fails.
  apr: assert property (@(posedge clk) rst |=> b[*1:3] ##1 c);

  initial begin
    @(posedge clk);
    a <= 1;                     // cyc1: a -> seeds a [*2] obligation at cyc2
    @(posedge clk);
    a <= 0; b <= 1;             // cyc2: first b
    @(posedge clk);
    @(posedge clk);             // cyc3: b still set -> second b, ap2 passes
    b <= 0; a <= 1;             // cyc4: seed [*2] again
    @(posedge clk);
    a <= 0; b <= 1;             // cyc5: one b only
    @(posedge clk);
    b <= 0;                     // cyc6: no second b -> ap2 FAILS
    @(posedge clk);
    rst <= 1;                   // cyc7: seeds a [*1:3] obligation at cyc8
    @(posedge clk);
    rst <= 0; b <= 1;           // cyc8: b
    @(posedge clk);
    b <= 0; c <= 1;             // cyc9: c -> apr passes at the [*1] length
    @(posedge clk);
    c <= 0; rst <= 1;           // cyc10: seed [*1:3] again
    @(posedge clk);
    rst <= 0; b <= 1;           // cyc11: b, and it runs through cyc14
    @(posedge clk);             // cyc11
    @(posedge clk);             // cyc12: b
    @(posedge clk);             // cyc13: b
    @(posedge clk);             // cyc14: b -- the longest branch wanted c
                                //   here, so apr FAILS at cyc14
    b <= 0; c <= 1;             // cyc15: c arrives one cycle too late
    @(posedge clk);
    @(posedge clk);
    c <= 0;
    @(posedge clk);
    $finish;
  end
endmodule
