module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 0;  // arms pe's attempt at cyc1
  bit          x   = 0;  // arms pt's attempt at cyc3
  bit          b   = 0;  // never raised: that is what makes an armed attempt fail
  int unsigned cyc = 0;

  // The active-region ruler, two lines per tick. The first runs in the Active
  // region and so reads `cyc` before its own nonblocking increment commits; the
  // second cannot run until that increment commits in the NBA region, so it is
  // the LAST active-region line of the tick and every reactive-region line of the
  // same tick must sort after it.
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] ACT edge cyc=%0d a=%b x=%b b=%b", $time, cyc, a, x, b);
  end
  always @(cyc) $display("[%0t] ACT nba  cyc=%0d", $time, cyc);

  // Wake path 1: an event control. The action block runs in the Reactive region
  // of cyc2 (16.14.1), by which time this tick's posedge is over, so the wait is
  // satisfied by the NEXT one -- in the Reactive region of cyc3.
  pe: assert property (@(posedge clk) a |=> b)
      else begin
        $display("[%0t] RCT pe 1 fail, waiting for a posedge", $time);
        @(posedge clk);
        $display("[%0t] RCT pe 2 resumed on the posedge", $time);
      end

  // Wake path 2: a timer, which matures at the top of a later time slot -- before
  // that slot's Active region has run, which is where routing the resumed handle
  // to the active ring looks most natural. The `#0` afterwards checks the thread
  // is STILL reactive once a timer has resumed it: 4.4.2.7's Re-Inactive round
  // trip must land it back in Reactive at the same instant.
  pt: assert property (@(posedge clk) x |=> b)
      else begin
        $display("[%0t] RCT pt 1 fail, delaying 10", $time);
        #10;
        $display("[%0t] RCT pt 2 resumed after #10", $time);
        #0;
        $display("[%0t] RCT pt 3 resumed after #0", $time);
      end

  initial begin
    @(posedge clk);   // cyc0 t=5
    a <= 1;           // sampled high at cyc1: arms pe's attempt, which needs b at cyc2
    @(posedge clk);   // cyc1 t=15
    a <= 0;
    @(posedge clk);   // cyc2 t=25   pe fails here and starts waiting for a posedge
    x <= 1;           // sampled high at cyc3: arms pt's attempt, which needs b at cyc4
    @(posedge clk);   // cyc3 t=35   pe resumes here
    x <= 0;
    @(posedge clk);   // cyc4 t=45   pt fails here and delays 10
    @(posedge clk);   // cyc5 t=55   pt resumes here, twice
    #1;               // off-tick, so nothing races the end of the run
    $finish;          // t=56
  end
endmodule
