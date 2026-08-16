// Inferred clocking (LRM 16.14.6). No property here carries a clocking event, so
// each takes the one from the procedure it sits in.
//
//   ap_pos  posedge procedure
//   ap_neg  negedge procedure -- same clock, so the two reports separate in time
//           only if the inferred EDGE is honoured and not just the signal
//   ap_rst  the ordinary reset-flop procedure: two events in the list, and
//           16.14.6 picks `posedge clk` because the body reads `rst_n` and so
//           disqualifies it. Its report must coincide with ap_pos's
//
// `clk` appears in each asserting procedure only as an event control, which is
// the condition 16.14.6 puts on a candidate clock; the counter and the trace
// driver are separate procedures and do not disturb that.
//
// The trace is driven with nonblocking assignments so every sampled value is
// unambiguous, and $display marks each edge so a report can be attributed.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0;
  bit rst_n = 0;
  int unsigned cyc = 0, rcnt = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] posedge cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end
  always @(negedge clk) $display("[%0t] negedge a=%b b=%b", $time, a, b);

  // rst_n rises at t=12 and never falls, so the reset procedure only ever wakes
  // on clk -- but `negedge rst_n` is still in its sensitivity list, which is what
  // makes the inference a choice rather than the only option.
  initial #12 rst_n = 1;

  always @(posedge clk) assert property (a |=> b);
  always @(negedge clk) assert property (a |=> b);
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) rcnt <= 0;
    else rcnt <= rcnt + 1;
    assert property (a |=> b);
  end

  // posedges at 5,15,25,...; negedges at 10,20,30,...
  initial begin
    @(posedge clk);    // t=5:  a=0 b=0
    a <= 1;
    @(posedge clk);    // t=15: a=1 b=0 -- posedge antecedent, owes b at t=25
    a <= 0; b <= 1;    //       (t=10: a=1 b=0 -- negedge antecedent, owes b at t=20)
    @(posedge clk);    // t=25: a=0 b=1 -- that posedge obligation passes
    b <= 0; a <= 1;    //       (t=20: a=0 b=1 -- the negedge one passes)
    @(posedge clk);    // t=35: a=1 b=0 -- posedge antecedent, owes b at t=45
    a <= 0;            //       (t=30: a=1 b=0 -- negedge antecedent, owes b at t=40)
    @(posedge clk);    // t=45: a=0 b=0 -- posedge FAIL   (t=40: negedge FAIL)
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
