module t;
  bit clk = 0;
  always #5 clk = ~clk;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] MODULE active cyc=%0d", $time, cyc);
  end
  p pi();
  initial begin repeat(3) @(posedge clk); $finish; end
endmodule

program p;
  initial begin
    @(posedge t.clk);
    $display("[%0t] PROGRAM reactive #1", $time);
    @(posedge t.clk);
    $display("[%0t] PROGRAM reactive #2", $time);
  end
endprogram
