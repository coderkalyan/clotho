// Wait-semantics benchmark (counterpart to bench.v's timing-wheel benchmark).
// A clock generator drives clk with blocking toggles — each toggle services
// clk's wait queue — while a worker suspends on @(posedge clk) every iteration
// and accumulates. The bulk of the work is wait-queue registration, edge-matched
// servicing, and coroutine suspend/resume: N suspend/resume cycles and N edge
// services. Deterministic output (acc depends only on the edge count, and the
// lone worker makes intra-instant ordering moot), so it matches any conformant
// simulator — verified equal to Icarus (acc=628038624 at N=5,000,000).
module wbench;
  reg     clk;
  integer i, j;
  integer acc;

  // Clock generator: 2*N blocking toggles -> N rising edges.
  initial begin
    clk = 0;
    for (i = 0; i < 2 * 5000000; i = i + 1) begin
      #5;
      clk = ~clk;
    end
  end

  // Worker: suspend on each rising edge, accumulate, finish after N edges.
  initial begin
    acc = 0;
    for (j = 0; j < 5000000; j = j + 1) begin
      @(posedge clk);
      acc = acc + (j * 3) - 1;
    end
    $display("acc=%0d", acc);
  end
endmodule
