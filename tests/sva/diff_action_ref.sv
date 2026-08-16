// Custom action blocks: an `else $error` on an assertion and a `$display` pass
// statement on a cover, so the directives report through their own statements
// rather than through the default report.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end

  ap: assert property (@(posedge clk) a |=> b)
      else $error("custom fail msg");
  cp: cover property (@(posedge clk) a ##1 b)
      $display("[%0t] COVER hit", $time);

  initial begin
    @(posedge clk);
    a <= 1;
    @(posedge clk);
    a <= 0; b <= 1;
    @(posedge clk);
    b <= 0; a <= 1;
    @(posedge clk);
    a <= 0;
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
