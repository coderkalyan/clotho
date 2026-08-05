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
