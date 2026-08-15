module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          d   = 0;  // expect1 and expect2b meet it high, expect2 meets it low
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

  // The process under test. expect1, expect2 and expect3 execute at an off-edge
  // time, so "the next clocking event" (16.17) is unambiguous; expect2b instead
  // executes at an edge ON PURPOSE, from the continuation of expect2, which is the
  // one place the answer is not obvious. Every action block prints on both
  // branches, so a wrong verdict is a wrong line rather than a missing one.
  initial begin
    #1;
    $display("[%0t] EXP  1 before expect1", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP  2 expect1 PASS action", $time);
      else                        $display("[%0t] EXP  3 expect1 FAIL action", $time);
    $display("[%0t] EXP  4 after expect1", $time);

    // t=16. Nothing else in the design runs at this instant, so the region this
    // `#1` resumes into is deliberately unobservable HERE -- 16.17 does not say
    // what region a process belongs to once an expect has resumed it, though LRM
    // 24.3.1's inheritance rule answers it for a thread of the reactive region
    // set. expect2b below is where this file does ask, and it asks in the one form
    // whose answer is a time stamp rather than a line order.
    #1;
    $display("[%0t] EXP  5 before expect2", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP  6 expect2 PASS action", $time);
      else                        $display("[%0t] EXP  7 expect2 FAIL action", $time);
    $display("[%0t] EXP  8 after expect2", $time);

    // NO DELAY HERE, deliberately: expect2b is executed from expect2's own
    // continuation, so it runs at t=35 with cyc3's Observed region already
    // processed. "The next clocking event" is therefore cyc4 and not cyc3, its
    // `##1 d` is tested at cyc5 where d is high, and it passes at t=55. An
    // implementation that counts cyc3 as still available prints the OTHER branch
    // at the OTHER time, t=45.
    $display("[%0t] EXP  9 before expect2b, back to back", $time);
    expect (@(posedge clk) ##1 d) $display("[%0t] EXP 10 expect2b PASS action", $time);
      else                        $display("[%0t] EXP 11 expect2b FAIL action", $time);
    $display("[%0t] EXP 12 after expect2b", $time);

    #1;
    $display("[%0t] EXP 13 before expect3", $time);
    expect (@(posedge clk) ##[1:8] z) $display("[%0t] EXP 14 expect3 PASS action", $time);
      else                            $display("[%0t] EXP 15 expect3 FAIL action", $time);
    // Reached only if a pending expect unblocks its process at the end of the
    // run. It must never appear before the $finish banner: that would mean the
    // expect did not block at all.
    $display("[%0t] EXP 16 after expect3 -- MUST NOT APPEAR", $time);
  end

  initial begin
    @(posedge clk);   // cyc0 t=5
    d <= 1;           // sampled 1 at cyc1, where expect1's `##1 d` is tested
    @(posedge clk);   // cyc1 t=15   expect1 succeeds here
    d <= 0;           // sampled 0 from cyc2 on, so expect2's `##1 d` fails at cyc3
    @(posedge clk);   // cyc2 t=25   expect2's attempt starts here
    @(posedge clk);   // cyc3 t=35   expect2 fails here, and expect2b is executed
    @(posedge clk);   // cyc4 t=45   expect2b's attempt starts here
    d <= 1;           // sampled 1 at cyc5, where expect2b's `##1 d` is tested, and
                      // LOW at cyc4, where a one-tick-early attempt would test it
    @(posedge clk);   // cyc5 t=55   expect2b succeeds here
    @(posedge clk);   // cyc6 t=65   expect3's attempt starts here
    @(posedge clk);   // cyc7 t=75
    @(posedge clk);   // cyc8 t=85   last evaluated tick
    #1;               // off-tick, so nothing races the end of the run
    $finish;          // t=86, with expect3 pending
  end
endmodule
