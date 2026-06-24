// Intra-body event waits as a stimulus driver. NOTE: does not compile yet —
// `@(edge)`/`@(signal)` waits inside a process body have no lowering (see
// Lower.cpp's Timed case). This is the target for the wait-event feature: a
// blocking-assignment process that suspends on an event and resumes when it
// fires. The design is a clocked register fed by a level-sensitive combinational
// block; the initial block drives the clock and synchronizes to it with waits.
module wait_events;
  reg        clk;
  reg        rst;
  reg  [7:0] d;
  reg  [7:0] q;
  reg  [7:0] doubled;

  // Sequential: capture the combinational result on each rising clock edge with
  // a synchronous (nonblocking) assignment.
  always @(posedge clk)
    if (rst)
      q <= 8'd0;
    else
      q <= doubled;

  // Level-sensitive combinational block (no always_comb yet): recompute on any
  // input change with a blocking assignment.
  always @(d or q)
    doubled = d + q;

  // Stimulus: drive the clock and inputs, then block on events. The `@(...)`
  // waits sitting between blocking assignments are exactly what the wait-event
  // lowering must support.
  initial begin
    clk = 0;
    rst = 1;
    d   = 8'd3;

    #5 clk = 1;     // rising edge: register loads (rst held)
    #5 clk = 0;

    @(q);           // wait until the register output settles
    rst = 0;

    #5 clk = 1;     // next rising edge: register loads `doubled`
    #5 clk = 0;

    @(doubled);     // wait until the combinational result updates
    $display("q=%0d doubled=%0d", q, doubled);
  end
endmodule
