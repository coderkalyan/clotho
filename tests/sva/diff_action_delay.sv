module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end

  // `a` with no `b` next cycle fails, and the action prints twice: once
  // immediately in Reactive, once after a `#0` round trip through Re-Inactive.
  ap: assert property (@(posedge clk) a |=> b)
      else begin
        $display("[%0t] FAIL now", $time);
        #0 $display("[%0t] FAIL after #0", $time);
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
