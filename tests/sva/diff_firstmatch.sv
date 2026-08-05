module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b ok=%b", $time, cyc, a, b, ok);
  end

  // `first_match` keeps only the shortest match from each start cycle. That is
  // unobservable in a CONSEQUENT or as a bare top-level sequence property -- a
  // sequence property holds the moment its sequence completes a match, and the
  // earliest match is exactly the one `first_match` keeps, so a correct lowering
  // and a no-op agree cycle for cycle. This fixture therefore puts it in an
  // ANTECEDENT, where every match seeds its OWN obligation, so suppressing the
  // later match of an attempt suppresses an obligation and changes the number of
  // reports. See concurrent-assertions.md 9.2.
  //
  // `a ##[1:2] b` can match twice from one start, at b in the first or the
  // second cycle after `a`, and the trace below drives BOTH so the suppression
  // has something to suppress. Six windows, one `a` pulse each:
  //
  //   cyc1  both matches; the kept one passes, the suppressed one WOULD fail
  //   cyc7  only the early match exists, and it fails       (kept, reports)
  //   cyc11 only the LATE match exists, and it fails        (kept, reports)
  //   cyc16 both matches; kept passes, suppressed WOULD fail
  //   cyc21 both matches; the kept one fails, the later one would have passed
  //   cyc26 both matches; both would fail, so only the kept one reports
  //
  // cyc11 is the window that separates this from a lowering that just truncated
  // `##[1:2]` to `##1`: with nothing at the first offset, the second-offset match
  // IS the first match and must survive.
  apf: assert property (@(posedge clk) first_match(a ##[1:2] b) |=> ok);

  initial begin
    @(posedge clk);
    a <= 1;                     // cyc1: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc2: early match ends -> obligation at cyc3
    @(posedge clk);
    ok <= 1;                    // cyc3: b still 1, so the late match ends here
    @(posedge clk);             //   -> its obligation is cyc4. Kept one passes.
    b <= 0; ok <= 0;            // cyc4: suppressed obligation would FAIL here
    @(posedge clk);
                                // cyc5
    @(posedge clk);
                                // cyc6
    @(posedge clk);
    a <= 1;                     // cyc7: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc8: early match ends -> obligation at cyc9
    @(posedge clk);
    b <= 0;                     // cyc9: no late match; ok is 0 -> apf FAILS
    @(posedge clk);
                                // cyc10
    @(posedge clk);
    a <= 1;                     // cyc11: attempt starts
    @(posedge clk);
    a <= 0;                     // cyc12: b is 0, so NO early match
    @(posedge clk);
    b <= 1;                     // cyc13: the late match is the only match, so
    @(posedge clk);             //   first_match must keep it
    b <= 0;                     // cyc14: ok is 0 -> apf FAILS
    @(posedge clk);
                                // cyc15
    @(posedge clk);
    a <= 1;                     // cyc16: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc17: early match ends -> obligation at cyc18
    @(posedge clk);
    ok <= 1;                    // cyc18: late match ends; kept obligation passes
    @(posedge clk);
    b <= 0; ok <= 0;            // cyc19: suppressed obligation would FAIL here
    @(posedge clk);
                                // cyc20
    @(posedge clk);
    a <= 1;                     // cyc21: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc22: early match ends -> obligation at cyc23
    @(posedge clk);
                                // cyc23: b still 1 -> late match ends here, and
    @(posedge clk);             //   ok is 0, so the KEPT obligation FAILS
    b <= 0; ok <= 1;            // cyc24: the suppressed obligation would pass,
    @(posedge clk);             //   so suppression must not silence cyc23
    ok <= 0;                    // cyc25
    @(posedge clk);
    a <= 1;                     // cyc26: attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc27: early match ends -> obligation at cyc28
    @(posedge clk);
                                // cyc28: b still 1 -> late match ends; ok is 0,
    @(posedge clk);             //   so the kept obligation FAILS here
    b <= 0;                     // cyc29: the suppressed obligation would FAIL
    @(posedge clk);             //   here too -- one window, two reports without
                                // cyc30    first_match and one with it
    @(posedge clk);
    $finish;
  end
endmodule
