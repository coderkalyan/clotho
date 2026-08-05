module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // Length-matching intersection: both operands must match over the SAME span,
  // so this is `b` followed by `b && c`. The failing attempt below is the one
  // that separates intersect from a length-independent conjunction -- the left
  // operand matches on its own, and the attempt still fails because the right
  // one does not match those same two cycles.
  api: assert property (@(posedge clk) a |=> (b ##1 c) intersect (b[*2]));

  apn: assert property (@(posedge clk) (rst ##1 b[=2]) |=> c);

  initial begin
    @(posedge clk);
    a <= 1;                     // cyc1: seeds an intersect obligation at cyc2
    @(posedge clk);
    a <= 0; b <= 1;             // cyc2: b -- both operands are live
    @(posedge clk);
    c <= 1;                     // cyc3: b && c -> both match cyc2..cyc3, pass
    @(posedge clk);
    b <= 0; c <= 0; a <= 1;     // cyc4: seed the intersect again
    @(posedge clk);
    a <= 0; b <= 1;             // cyc5: b
    @(posedge clk);
    b <= 0; c <= 1;             // cyc6: c but NOT b, so (b ##1 c) matches and
    @(posedge clk);             //   b[*2] does not -> api FAILS at cyc6
    c <= 0; rst <= 1;           // cyc7: seeds the [=2] attempt at cyc8
    @(posedge clk);
    rst <= 0;                   // cyc8: no b yet
    @(posedge clk);
    b <= 1;                     // cyc9: first b
    @(posedge clk);
    b <= 0;                     // cyc10: gap -- [=2] does not need the two
    @(posedge clk);             //   occurrences back to back
    b <= 1; c <= 1;             // cyc11: second b -> the antecedent matches
    @(posedge clk);             //   here, so the obligation wants c at cyc12
                                // cyc12: b stays set -> the third b closes the
    @(posedge clk);             //   extension, and c holds -> apn passes
    b <= 0; c <= 0;             // cyc13
    @(posedge clk);
    rst <= 1;                   // cyc14: seed the [=2] attempt again
    @(posedge clk);
    rst <= 0;                   // cyc15
    @(posedge clk);
    b <= 1;                     // cyc16: first b
    @(posedge clk);
    b <= 0;                     // cyc17: gap
    @(posedge clk);
    b <= 1;                     // cyc18: second b -> obligation wants c at
    @(posedge clk);             //   cyc19
                                // cyc19: b stays set -> third b closes the
    @(posedge clk);             //   extension, and c is 0 -> apn FAILS
    b <= 0;                     // cyc20
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
