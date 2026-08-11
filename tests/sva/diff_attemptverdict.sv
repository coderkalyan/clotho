module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, ok = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b ok=%b", $time, cyc, a, b, ok);
  end

  // The four ways one attempt's two obligations can land, which is what decides
  // between "report the first", "report the last" and "one verdict per attempt".
  // `a ##[1:2] b` matches twice from a single `a`, so each
  // window below is ONE attempt owing ONE verdict:
  //
  //   cyc1  fail-then-pass -> FAIL at cyc3, and the later pass is not reported
  //   cyc9  fail-then-fail -> FAIL at cyc11 only; the second failure is the
  //                           same attempt's and is not a second report
  //   cyc16 pass-then-fail -> FAIL at cyc19; the earlier pass was not the
  //                           attempt's, because an obligation was still owed
  //   cyc24 pass-then-pass -> no report at all
  //
  // The cyc9 window alone cannot tell "report the first" from "one verdict";
  // cyc1 and cyc16 are what separate them, since the reported cycle is the
  // earlier obligation's in one and the later obligation's in the other.
  ap: assert property (@(posedge clk) (a ##[1:2] b) |=> ok);

  initial begin
    @(posedge clk);
    a <= 1;                     // cyc1  attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc2  match1 ends -> obligation checks ok at cyc3
    @(posedge clk);
                                // cyc3  match2 ends -> obligation checks ok at cyc4;
                                //       ok is 0 here, so obligation1 FAILS
    @(posedge clk);
    b <= 0; ok <= 1;            // cyc4  obligation2 would pass -- attempt already failed
    @(posedge clk);
    ok <= 0;                    // cyc5
    @(posedge clk);
                                // cyc6
    @(posedge clk);
                                // cyc7
    @(posedge clk);
                                // cyc8
    @(posedge clk);
    a <= 1;                     // cyc9  attempt starts
    @(posedge clk);
    a <= 0; b <= 1;             // cyc10 match1 ends -> obligation checks ok at cyc11
    @(posedge clk);
                                // cyc11 match2 ends; ok is 0, so obligation1 FAILS
    @(posedge clk);
    b <= 0;                     // cyc12 obligation2 fails too -- same attempt, no report
    @(posedge clk);
                                // cyc13
    @(posedge clk);
                                // cyc14
    @(posedge clk);
                                // cyc15
    @(posedge clk);
    a <= 1;                     // cyc16 attempt starts
    @(posedge clk);
    a <= 0; b <= 1; ok <= 1;    // cyc17 match1 ends -> obligation checks ok at cyc18
    @(posedge clk);
                                // cyc18 match2 ends; ok is 1, so obligation1 passes
    @(posedge clk);
    b <= 0; ok <= 0;            // cyc19 obligation2 sees ok=0 -> the attempt FAILS
    @(posedge clk);
                                // cyc20
    @(posedge clk);
                                // cyc21
    @(posedge clk);
                                // cyc22
    @(posedge clk);
                                // cyc23
    @(posedge clk);
    a <= 1;                     // cyc24 attempt starts
    @(posedge clk);
    a <= 0; b <= 1; ok <= 1;    // cyc25 match1 ends -> obligation checks ok at cyc26
    @(posedge clk);
                                // cyc26 match2 ends; ok is 1, so obligation1 passes
    @(posedge clk);
    b <= 0;                     // cyc27 obligation2 passes too -> attempt passes
    @(posedge clk);
    ok <= 0;                    // cyc28
    @(posedge clk);
                                // cyc29
    @(posedge clk);
    $finish;
  end
endmodule
