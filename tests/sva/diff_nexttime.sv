module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b", $time, cyc, a);
  end

  wn1: assert property (@(posedge clk) nexttime [1] a);
  wn2: assert property (@(posedge clk) nexttime [2] a);

  initial begin
    @(posedge clk);            // cyc0: a=1
    a <= 0;
    @(posedge clk);            // cyc1: a=0
    a <= 1;
    @(posedge clk);            // cyc2: a=1
    @(posedge clk);            // cyc3: a=1
    a <= 0;
    @(posedge clk);            // cyc4: a=0
    a <= 1;
    @(posedge clk);            // cyc5: a=1
    @(posedge clk);            // cyc6  the tail is quiet so that the attempts
    @(posedge clk);            // cyc7  left open at the end are the only thing
    @(posedge clk);            // cyc8  still owed, and weakly they hold
    @(posedge clk);            // cyc9
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
