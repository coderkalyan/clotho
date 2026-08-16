// Double commit in one instant: nonblocking updates staged from BOTH the active region
// and reactive code at the same instant, through both the block-commit path
// (scalars) and the shared array-record buffer (runtime index).
//
// If the Re-NBA commit re-applied what the NBA commit already committed, actcnt
// or mem[0] would advance by 2 on the failing edge. If it lost the reactive
// update, rcnt or mem[1] would never move.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0;

  int unsigned cyc    = 0;
  int unsigned actcnt = 0;   // active-region scalar NBA
  int unsigned rcnt   = 0;   // reactive-region scalar NBA
  int unsigned mem [0:3];    // array NBA, written from both regions
  int ai = 0, ri = 1;        // runtime indices, to force the record path

  initial begin
    mem[0] = 0; mem[1] = 0; mem[2] = 0; mem[3] = 0;
  end

  always @(posedge clk) begin
    cyc     <= cyc + 1;
    actcnt  <= actcnt + 1;
    mem[ai] <= mem[ai] + 1;
    $display("[%0t] cyc=%0d actcnt=%0d rcnt=%0d mem0=%0d mem1=%0d",
             $time, cyc, actcnt, rcnt, mem[0], mem[1]);
  end

  ap: assert property (@(posedge clk) a |=> b)
      else begin
        rcnt    <= rcnt + 1;
        mem[ri] <= mem[ri] + 1;
        $display("[%0t] FAIL rcnt=%0d mem1=%0d", $time, rcnt, mem[1]);
      end

  initial begin
    @(posedge clk);
    a <= 1;
    @(posedge clk);
    a <= 0;      // fails here
    @(posedge clk);
    a <= 1;
    @(posedge clk);
    a <= 0;      // and here
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
