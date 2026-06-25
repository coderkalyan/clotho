// Intra-body event waits as a stimulus driver, exercising the full wait path:
// a clocked counter, a level-sensitive combinational block with a multi-signal
// sensitivity list, and an initial block that drives the clock and synchronizes
// to the combinational result with @(signal) waits. All assignments are blocking
// (no NBA yet) — this design has no intra-block RAW hazards, so blocking matches
// what a synchronous register would produce. Terminating and deterministic;
// output verified equal to Icarus.
module wait_events;
  reg        clk;
  reg        rst;
  reg  [7:0] d;
  reg  [7:0] q;
  reg  [7:0] doubled;
  integer    i;

  // Sequential: a counter, reset-cleared, incremented on each rising clock edge.
  always @(posedge clk)
    if (rst)
      q = 8'd0;
    else
      q = q + 8'd1;

  // Level-sensitive combinational block (no always_comb yet): recompute on any
  // change of d or q.
  always @(d or q)
    doubled = d + q;

  // Stimulus: hold reset for one edge, then pulse the clock and, each time, block
  // on the combinational result settling before sampling it.
  initial begin
    rst = 1;
    d   = 8'd10;
    clk = 0;
    #5 clk = 1;   // reset edge: q <= 0
    #5 clk = 0;
    rst = 0;
    for (i = 0; i < 3; i = i + 1) begin
      #5 clk = 1; // counting edge: q increments, so doubled changes
      @(doubled); // wait for the combinational result to settle
      $display("q=%0d doubled=%0d", q, doubled);
      #5 clk = 0;
    end
  end
endmodule
