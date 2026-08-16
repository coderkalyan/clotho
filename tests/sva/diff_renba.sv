// An action block that performs a nonblocking assignment. LRM 4.4.2.8 commits it
// in the Re-NBA region -- the reactive region set's dual of the NBA region --
// which runs once Reactive and Re-Inactive have settled but still inside the
// failing edge's own time slot, before Postponed. Print ordering is the whole
// observable, as it is in diff_action_delay.sv:
//
//   * the action block's own $display, placed immediately after `flag <= 1`,
//     must still read the OLD flag: Re-NBA has not committed yet.
//   * `always @(flag)` is woken by that commit and must run at the SAME time,
//     because LRM 4.4.2's outer loop returns to the Active region set when the
//     reactive one has put work back into it.
//
// Staging the store into the ordinary NBA region instead would commit it a full
// time step late and move both of those lines to the next edge.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0;
  bit flag = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b flag=%b", $time, cyc, a, b, flag);
  end

  // Woken by the Re-NBA commit, so its print pins where that commit landed.
  always @(flag) $display("[%0t] flag woke -> %b", $time, flag);

  // `a` with no `b` next cycle fails, and the action stages flag for Re-NBA.
  ap: assert property (@(posedge clk) a |=> b)
      else begin
        $display("[%0t] FAIL flag=%b", $time, flag);
        flag <= 1;
        $display("[%0t] after NBA flag=%b", $time, flag);
      end

  initial begin
    @(posedge clk);
    a <= 1;            // cyc1: arms an attempt needing b at cyc2
    @(posedge clk);
    a <= 0;            // cyc2: no b -> that attempt fails here
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
