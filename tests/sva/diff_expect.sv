module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          d   = 0;  // expect1 meets it high, expect2 meets it low
  bit          z   = 0;  // never raised: expect3's window can never be satisfied
  int unsigned cyc = 0;

  // The active-region ruler, two lines per tick. The first runs in the Active
  // region and so reads `cyc` before its own nonblocking increment commits; the
  // second cannot run until that increment commits in the NBA region, so it is
  // the LAST active-region line of the tick and every reactive-region line of the
  // same tick must sort after it.
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] ACT edge cyc=%0d d=%b z=%b", $time, cyc, d, z);
  end
  always @(cyc) $display("[%0t] ACT nba  cyc=%0d", $time, cyc);

  // The process under test. Every expect executes at an off-edge time, so "the
  // next clocking event" (16.17) is unambiguous, and every action block prints on
  // both branches, so a wrong verdict is a wrong line rather than a missing one.
  initial begin
    #1;
    $display("[%0t] EXP  1 before expect1", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP  2 expect1 PASS action", $time);
      else                        $display("[%0t] EXP  3 expect1 FAIL action", $time);
    $display("[%0t] EXP  4 after expect1", $time);

    // t=16. Nothing else in the design runs at this instant, so the region this
    // `#1` resumes into is deliberately unobservable: what region a process
    // belongs to AFTER an expect has resumed it is not something 16.17 settles,
    // and this fixture does not ask.
    #1;
    $display("[%0t] EXP  5 before expect2", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP  6 expect2 PASS action", $time);
      else                        $display("[%0t] EXP  7 expect2 FAIL action", $time);
    $display("[%0t] EXP  8 after expect2", $time);

    #1;
    $display("[%0t] EXP  9 before expect3", $time);
    expect (@(posedge clk) ##[1:8] z) $display("[%0t] EXP 10 expect3 PASS action", $time);
      else                            $display("[%0t] EXP 11 expect3 FAIL action", $time);
    // Reached only if a pending expect unblocks its process at the end of the
    // run. It must never appear before the $finish banner: that would mean the
    // expect did not block at all.
    $display("[%0t] EXP 12 after expect3 -- MUST NOT APPEAR", $time);
  end

  initial begin
    @(posedge clk);   // cyc0 t=5
    d <= 1;           // sampled 1 at cyc1, where expect1's `##1 d` is tested
    @(posedge clk);   // cyc1 t=15   expect1 succeeds here
    d <= 0;           // sampled 0 from cyc2 on, so expect2's `##1 d` fails at cyc3
    @(posedge clk);   // cyc2 t=25   expect2's attempt starts here
    @(posedge clk);   // cyc3 t=35   expect2 fails here
    @(posedge clk);   // cyc4 t=45   expect3's attempt starts here
    @(posedge clk);   // cyc5 t=55
    @(posedge clk);   // cyc6 t=65   last evaluated tick
    #1;               // off-tick, so nothing races the end of the run
    $finish;          // t=66, with expect3 pending
  end
endmodule
