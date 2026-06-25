// Wait-semantics benchmark exercising both wait-queue kinds on the same signal,
// at scale. A clock generator drives clk with blocking toggles; on every rising
// edge the service scans clk's STATIC queue (two always @(posedge clk) blocks,
// persistent subscribers) and its DYNAMIC queue (an intra-body @(posedge clk)
// worker, a one-shot waiter re-registered each iteration). N suspend/resume
// cycles plus N edge services across both queue types. Deterministic: the
// worker's acc depends only on its own loop, the always counters only on the
// edge count, and the two $display lines print at distinct times — so it matches
// any conformant simulator (verified equal to Icarus).
module wbench;
  reg     clk;
  integer i, j;
  integer acc;        // dynamic-queue worker accumulator
  integer cnt1, cnt2; // static-queue (always-block) counters

  // Static-queue subscribers: count every rising edge.
  always @(posedge clk) cnt1 = cnt1 + 1;
  always @(posedge clk) cnt2 = cnt2 + 2;

  // Clock generator: 2*N blocking toggles -> N rising edges, then settle and
  // report the always-block counters (all edges drained by the trailing delay).
  initial begin
    cnt1 = 0;
    cnt2 = 0;
    clk  = 0;
    for (i = 0; i < 2 * 5000000; i = i + 1) begin
      #5 clk = ~clk;
    end
    #5;
    $display("cnt1=%0d cnt2=%0d", cnt1, cnt2);
  end

  // Dynamic-queue worker: suspend on each rising edge, accumulate, finish after N.
  initial begin
    acc = 0;
    for (j = 0; j < 5000000; j = j + 1) begin
      @(posedge clk);
      acc = acc + (j * 3) - 1;
    end
    $display("acc=%0d", acc);
  end
endmodule
