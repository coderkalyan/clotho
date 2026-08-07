module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit noise = 0;
  always #3 noise = ~noise;

  logic [3:0]  v;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d v=%b noise=%b", $time, cyc, v, noise);
  end

  a_rose:    assert property (@(posedge clk) $rose(v));
  a_fell:    assert property (@(posedge clk) $fell(v));
  a_stable:  assert property (@(posedge clk) $stable(v));
  a_changed: assert property (@(posedge clk) $changed(v));

  initial begin
    @(posedge clk);            // tick 1 samples xxxx
    v <= 4'b0001;
    @(posedge clk);            // tick 2 samples 0001
    v <= 4'b0011;
    @(posedge clk);            // tick 3 samples 0011
    v <= 4'b0010;
    @(posedge clk);            // tick 4 samples 0010
    @(posedge clk);            // tick 5 samples 0010
    @(posedge clk);            // tick 6: $finish, never evaluated
    $finish;
  end
endmodule
