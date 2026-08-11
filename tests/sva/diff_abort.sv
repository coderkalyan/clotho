module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          trig = 0, e = 1, abx = 0, aby = 0;
  int unsigned cyc  = 0;

  // Both clock edges AND every change of an abort condition are marked,
  // because an asynchronous abort can be seen at a time step carrying no tick
  // and a report there has to be attributable to it rather than to the last
  // tick.
  always @(clk or abx or aby)
    $display("[%0t] clk=%b cyc=%0d trig=%b e=%b abx=%b aby=%b",
             $time, clk, cyc, trig, e, abx, aby);
  always @(posedge clk) cyc <= cyc + 1;

  aa: assert property (@(posedge clk) accept_on      (abx) (trig |=> e));
  sa: assert property (@(posedge clk) sync_accept_on (abx) (trig |=> e));
  rr: assert property (@(posedge clk) reject_on      (aby) (trig |=> e));
  sr: assert property (@(posedge clk) sync_reject_on (aby) (trig |=> e));

  initial begin
    @(posedge clk);            // cyc0
    trig <= 1;
    @(posedge clk);            // cyc1: trig=1, seeds
    trig <= 0; e <= 0;
    @(posedge clk);            // cyc2: e=0, the control failure
    trig <= 1; e <= 1;
    @(posedge clk);            // cyc3: trig=1, seeds into the abx pulse
    trig <= 0; e <= 0;
    @(posedge clk);            // cyc4: e=0
    e <= 1;
    @(posedge clk);            // cyc5
    @(posedge clk);            // cyc6: nothing pending across t=72
    trig <= 1;
    @(posedge clk);            // cyc7: trig=1 AND aby sampled high here
    trig <= 0;
    @(posedge clk);            // cyc8: e=1, so cycle 7 would have passed
    trig <= 1;
    @(posedge clk);            // cyc9: seeds an obligation nothing can settle
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end

  // Absolute times, not clock-relative: the whole point of abx is that it
  // falls between two ticks, and the whole point of aby is that it does not.
  initial begin
    #38 abx = 1;               // between ticks 3 (35) and 4 (45)
    #4  abx = 0;               // t=42
    #30 aby = 1;               // t=72: nothing is scheduled before the t=75
    #4  aby = 0;               // t=76   tick, so the tick samples it first
  end
endmodule
