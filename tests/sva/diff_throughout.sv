module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit trig = 0, en = 0, b = 0, c = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d trig=%b en=%b b=%b c=%b", $time, cyc, trig, en, b, c);
  end

  // `en throughout (b ##1 c)`. The left operand is a BOOLEAN, not a sequence,
  // and it has to hold in EVERY cycle the right operand's match spans -- the
  // first and the last included.
  //
  // The two failing attempts below are what separates that from ignoring `en`:
  // in both, `b ##1 c` matches on its own and the attempt still fails, once
  // because `en` drops on the last cycle of the span and once on the first. The
  // second of those also pins WHEN the failure is reported, since there is
  // nothing left to wait for once the first cycle is lost.
  apt: assert property (@(posedge clk) trig |=> (en throughout (b ##1 c)));

  initial begin
    @(posedge clk);
    trig <= 1;                  // cyc1: seeds the obligation at cyc2
    @(posedge clk);
    trig <= 0; en <= 1; b <= 1; // cyc2: en && b
    @(posedge clk);
    b <= 0; c <= 1;             // cyc3: en && c -> matches cyc2..cyc3, passes
    @(posedge clk);
    c <= 0; en <= 0; trig <= 1; // cyc4: seeds again, at cyc5
    @(posedge clk);
    trig <= 0; en <= 1; b <= 1; // cyc5: en && b
    @(posedge clk);
    b <= 0; c <= 1; en <= 0;    // cyc6: c holds but en does not -> apt FAILS
    @(posedge clk);
    c <= 0; trig <= 1;          // cyc7: seeds again, at cyc8
    @(posedge clk);
    trig <= 0; b <= 1;          // cyc8: b holds and en does not -> apt FAILS
    @(posedge clk);             //   here, on the FIRST cycle of the span
    b <= 0; c <= 1; en <= 1;    // cyc9: c and en, too late to matter
    @(posedge clk);
    c <= 0; en <= 0;            // cyc10
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
