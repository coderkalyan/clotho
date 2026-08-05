module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, d = 0, ok_and = 0, ok_int = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b d=%b ok_and=%b ok_int=%b", $time, cyc, a, b, c, d,
             ok_and, ok_int);
  end

  // Separate consequents so the two never report in the same cycle: which of
  // them fires is the whole observable here, and a shared consequent would put
  // both reports at one timestamp where only their order told them apart.
  aand: assert property (@(posedge clk) ((a ##[1:2] b) and (c ##1 d)) |=> ok_and);
  aint: assert property (@(posedge clk) ((a ##[1:2] b) intersect (c ##1 d)) |=> ok_int);

  initial begin
    @(posedge clk);
    a <= 1; c <= 1;             // cyc1: both operands start
    @(posedge clk);
    a <= 0; c <= 0; d <= 1;     // cyc2: the right operand ends HERE
    @(posedge clk);
    d <= 0; b <= 1;             // cyc3: the left one ends a cycle later, so the
    @(posedge clk);             //   ends differ: `and` matches cyc1..cyc3 and
    b <= 0;                     //   `intersect` does not match at all
                                // cyc4: ok_and is 0 -> aand FAILS, aint silent.
    @(posedge clk);             //   A pair ending at the EARLIER end would have
                                //   reported a cycle sooner than this.
    @(posedge clk);
                                // cyc6
    @(posedge clk);
    a <= 1; c <= 1;             // cyc7: both operands start again
    @(posedge clk);
    a <= 0; c <= 0; b <= 1; d <= 1; // cyc8: this time both end here, so the
    @(posedge clk);             //   intersection matches too
    b <= 0; d <= 0; ok_and <= 1; // cyc9: aand passes and aint FAILS -- the
    @(posedge clk);             //   intersection is silent above by its own
    ok_and <= 0;                // cyc10:  semantics, not for want of lowering
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
