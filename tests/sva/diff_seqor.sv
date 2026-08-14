module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // Sequence disjunction. Both branches are live at once rather than one being
  // tried and then the other, so the two orderings below each match on their
  // own branch, and the attempt fails only when NEITHER can.
  apo: assert property (@(posedge clk) a |=> (b ##1 c) or (c ##1 b));

  // REMOVED, and the removal is the record. This fixture used to carry
  //
  //     apg: assert property (@(posedge clk) (rst ##1 b[->2]) |=> c);
  //
  // to exercise goto repetition in an ANTECEDENT, where `b[->2]` ends exactly
  // on the second b however many non-b cycles separate them. That shape no
  // longer lowers: an unbounded reach means unboundedly many attempts can be
  // part-way through the antecedent at once, and counting the matches that end
  // in one cycle needs per-attempt identity whose window is the antecedent's
  // longest match. libsva refuses it with SVA_ERROR_UNBOUNDED_ANTECEDENT
  // rather than under-report.
  //
  // The coverage moved: tests/sva_test.cpp asserts the refusal by name and
  // asserts that the same operator still lowers in a CONSEQUENT on the counted
  // discipline. Restoring this line needs the trigger determinised, tracked in
  // docs/TODO.md; re-score the file when it lands.

  initial begin
    @(posedge clk);
    a <= 1;                     // cyc1: seeds an `or` obligation at cyc2
    @(posedge clk);
    a <= 0; b <= 1;             // cyc2: b -> the (b ##1 c) branch is live
    @(posedge clk);
    b <= 0; c <= 1;             // cyc3: c -> that branch matches, apo passes
    @(posedge clk);
    c <= 0; a <= 1;             // cyc4: seed again
    @(posedge clk);
    a <= 0; c <= 1;             // cyc5: c -> now the (c ##1 b) branch is live
    @(posedge clk);
    c <= 0; b <= 1;             // cyc6: b -> the other branch matches, passes
    @(posedge clk);
    b <= 0; a <= 1;             // cyc7: seed a third time
    @(posedge clk);
    a <= 0; b <= 1;             // cyc8: b
    @(posedge clk);
    b <= 1;                     // cyc9: b again, so neither branch can finish
    @(posedge clk);             //   -> apo FAILS at cyc9
    b <= 0; rst <= 1;           // cyc10: seeds the goto attempt at cyc11
    @(posedge clk);
    rst <= 0;                   // cyc11: no b yet -- goto waits, it does not
    @(posedge clk);             //   require back-to-back occurrences
    b <= 1;                     // cyc12: first b
    @(posedge clk);
    b <= 0;                     // cyc13: gap
    @(posedge clk);
    b <= 1;                     // cyc14: second b -> the goto match ends here
    @(posedge clk);
    b <= 0; c <= 1;             // cyc15: c -> apg passes
    @(posedge clk);
    c <= 0; rst <= 1;           // cyc16: seed the goto again
    @(posedge clk);
    rst <= 0; b <= 1;           // cyc17: first b
    @(posedge clk);
    @(posedge clk);             // cyc18: second b -> match ends at cyc18
    b <= 0;                     // cyc19: no c -> apg FAILS at cyc19
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
