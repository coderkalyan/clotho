module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, d = 0;
  int unsigned cyc = 0;
  always @(clk)
    $display("[%0t] clk=%b cyc=%0d a=%b b=%b c=%b d=%b", $time, clk, cyc, a, b, c, d);
  always @(negedge clk) cyc <= cyc + 1;

  sequence sab;
    @(negedge clk) a ##1 b;
  endsequence

  property pcd;
    @(negedge clk) c |=> d;
  endproperty

  // Bare: an attempt starts every cycle, so several are in flight at once.
  ap: assert property (sab);
  // The same sequence as an antecedent, against a consequent clocked to match.
  bp: assert property (sab |=> @(negedge clk) c);
  // The declaration wraps the whole implication.
  cp: assert property (pcd);

  initial begin
    @(negedge clk);              // cyc0: quiet, so ap's first attempt reports
    a <= 1;                      // cyc1: sab starts
    @(negedge clk);              // cyc1
    a <= 0; b <= 1;              // cyc2: sab matches -> bp needs c at cyc3
    @(negedge clk);              // cyc2
    b <= 0; c <= 1;              // cyc3: c -> bp passes; cp needs d at cyc4
    @(negedge clk);              // cyc3
    c <= 0; d <= 1; a <= 1;      // cyc4: d -> cp passes; sab starts again
    @(negedge clk);              // cyc4
    a <= 0; d <= 0; b <= 1;      // cyc5: sab matches -> bp needs c at cyc6
    @(negedge clk);              // cyc5
    b <= 0;                      // cyc6: no c -> bp reports
    @(negedge clk);              // cyc6
    c <= 1;                      // cyc7: cp needs d at cyc8
    @(negedge clk);              // cyc7
    c <= 0;                      // cyc8: no d -> cp reports
    @(negedge clk);              // cyc8
    a <= 1; b <= 1;              // cyc9: quiet the tail so nothing reports in
    @(negedge clk);              // cyc9   the same time slot as $finish, where
    @(negedge clk);              // cyc10  the region a report lands in is moot
    $finish;
  end
endmodule
