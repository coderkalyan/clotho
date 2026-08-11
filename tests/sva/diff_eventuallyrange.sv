module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          e   = 1, f = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d e=%b f=%b", $time, cyc, e, f);
  end

  we: assert property (@(posedge clk) eventually   [1:2] e);
  se: assert property (@(posedge clk) s_eventually [1:2] e);
  wf: assert property (@(posedge clk) eventually   [0:1] f);
  sf: assert property (@(posedge clk) s_eventually [0:1] f);

  initial begin
    @(posedge clk);            // cyc0
    @(posedge clk);            // cyc1
    @(posedge clk);            // cyc2
    e <= 0;
    @(posedge clk);            // cyc3: e=0
    @(posedge clk);            // cyc4: e=0
    @(posedge clk);            // cyc5: e=0
    e <= 1; f <= 0;
    @(posedge clk);            // cyc6: e=1 f=0
    @(posedge clk);            // cyc7: f=0
    f <= 1;
    @(posedge clk);            // cyc8: f=1
    e <= 0; f <= 0;
    @(posedge clk);            // cyc9: e=0 f=0, both attempts left open here
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
