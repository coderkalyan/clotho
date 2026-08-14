module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit          a   = 1, b = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b", $time, cyc, a, b);
  end

  nw:  assert property (@(posedge clk) not         (a ##1     b));
  ns:  assert property (@(posedge clk) not strong  (a ##1     b));
  nwr: assert property (@(posedge clk) not         (a ##[1:3] b));
  nsr: assert property (@(posedge clk) not strong  (a ##[1:3] b));

  sq:  assert property (@(posedge clk)             (a ##1     b));
  nn:  assert property (@(posedge clk) not (not    (a ##1     b)));

  cn:  cover  property (@(posedge clk) not weak    (a ##1     b))
         $display("[%0t] COVER cn", $time);

  initial begin
    @(posedge clk);            // cyc0: a=1 b=0
    b <= 1;
    @(posedge clk);            // cyc1: a=1 b=1  the operand matches from cyc0
    a <= 0; b <= 0;
    @(posedge clk);            // cyc2: a=0 b=0  refuted at its own start tick
    a <= 1;
    @(posedge clk);            // cyc3: a=1 b=0
    b <= 1;
    @(posedge clk);            // cyc4: a=1 b=1  second match, reached from cyc1
    b <= 0;                    //                and cyc3 by the window form
    @(posedge clk);            // cyc5: a=1 b=0  b never rises again
    a <= 0;
    @(posedge clk);            // cyc6: a=0 b=0
    a <= 1;
    @(posedge clk);            // cyc7: a=1 b=0  a stays high to the end, so the
    @(posedge clk);            // cyc8: a=1 b=0  window form leaves attempts 7, 8
    @(posedge clk);            // cyc9: a=1 b=0  and 9 pending and `a ##1 b`
                               //                leaves attempt 9 pending
    @(posedge clk);            // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
