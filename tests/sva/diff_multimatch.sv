module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b ok=%b", $time, cyc, a, b, ok);
  end

  // The repro for the per-attempt rule: `diff_firstmatch.sv`'s trace with `first_match`
  // dropped, so the later match of each window seeds an obligation of its own
  // instead of being suppressed.
  //
  // An attempt (LRM 16.12.1, one per tick) whose antecedent matches twice owes
  // ONE verdict, not one per obligation: it fails at the FIRST obligation to
  // fail and never reports again, and it passes only once every one of them has
  // passed. Reporting per obligation would print the
  // cyc26 window below twice. Six windows, one `a` pulse each:
  //
  //   cyc1  both matches; the early obligation passes, the late one fails
  //   cyc7  only the early match exists, and it fails
  //   cyc11 only the LATE match exists, and it fails
  //   cyc16 both matches; the early obligation passes, the late one fails
  //   cyc21 both matches; the early one FAILS, so the later pass is not reached
  //   cyc26 both matches and both fail -- one report, at the first of them
  //
  // cyc11 is the window that separates this from a lowering that truncated
  // `##[1:2]` to `##1`: with nothing at the first offset, the second-offset
  // match is the attempt's only one and must still report.
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> ok);

  initial begin
    @(posedge clk);
    a <= 1;                     // cyc1: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc2: early match ends -> obligation at cyc3
    @(posedge clk);
    ok <= 1;                    // cyc3: b still 1, so the late match ends here
    @(posedge clk);             //   -> its obligation is cyc4. The early one passes.
    b <= 0; ok <= 0;            // cyc4: the late obligation FAILS here
    @(posedge clk);
                                // cyc5
    @(posedge clk);
                                // cyc6
    @(posedge clk);
    a <= 1;                     // cyc7: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc8: early match ends -> obligation at cyc9
    @(posedge clk);
    b <= 0;                     // cyc9: no late match; ok is 0 -> ap FAILS
    @(posedge clk);
                                // cyc10
    @(posedge clk);
    a <= 1;                     // cyc11: attempt starts
    @(posedge clk);
    a <= 0;                     // cyc12: b is 0, so NO early match
    @(posedge clk);
    b <= 1;                     // cyc13: the late match is the attempt's only one
    @(posedge clk);
    b <= 0;                     // cyc14: ok is 0 -> ap FAILS
    @(posedge clk);
                                // cyc15
    @(posedge clk);
    a <= 1;                     // cyc16: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc17: early match ends -> obligation at cyc18
    @(posedge clk);
    ok <= 1;                    // cyc18: late match ends; the early obligation passes
    @(posedge clk);
    b <= 0; ok <= 0;            // cyc19: the late obligation FAILS here
    @(posedge clk);
                                // cyc20
    @(posedge clk);
    a <= 1;                     // cyc21: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc22: early match ends -> obligation at cyc23
    @(posedge clk);
                                // cyc23: b still 1 -> late match ends here, and
    @(posedge clk);             //   ok is 0, so the EARLY obligation FAILS
    b <= 0; ok <= 1;            // cyc24: the late obligation would have passed,
    @(posedge clk);             //   but its attempt is already resolved
    ok <= 0;                    // cyc25
    @(posedge clk);
    a <= 1;                     // cyc26: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc27: early match ends -> obligation at cyc28
    @(posedge clk);
                                // cyc28: b still 1 -> late match ends; ok is 0,
    @(posedge clk);             //   so the early obligation FAILS here
    b <= 0;                     // cyc29: the late obligation would fail here too
    @(posedge clk);             //   -- one attempt, one report
                                // cyc30
    @(posedge clk);
    $finish;
  end
endmodule
