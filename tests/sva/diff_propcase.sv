module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit [1:0]    sel = 2'd0;
  bit          x   = 1, y = 1;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d sel=%0d x=%b y=%b", $time, cyc, sel, x, y);
  end

  cs:  assert property (@(posedge clk) case (sel)
                                         2'd0: x;
                                         2'd1: y;
                                       endcase);
  csd: assert property (@(posedge clk) case (sel)
                                         2'd0:    x;
                                         2'd1:    y;
                                         default: 1'b0;
                                       endcase);

  initial begin
    @(posedge clk);                  // cyc0: sel=0 x=1 y=1
    @(posedge clk);                  // cyc1
    x <= 0;
    @(posedge clk);                  // cyc2: sel=0 x=0 y=1
    sel <= 2'd1; x <= 1;
    @(posedge clk);                  // cyc3: sel=1 x=1 y=1
    y <= 0;
    @(posedge clk);                  // cyc4: sel=1 x=1 y=0
    sel <= 2'd2; x <= 0;
    @(posedge clk);                  // cyc5: sel=2 x=0 y=0
    sel <= 2'd3;
    @(posedge clk);                  // cyc6: sel=3 x=0 y=0
    sel <= 2'd0; x <= 1; y <= 1;
    @(posedge clk);                  // cyc7  quiet tail, so that nothing
    sel <= 2'd1;                     //       reports in the same slot as
    @(posedge clk);                  // cyc8  $finish
    sel <= 2'd0;
    @(posedge clk);                  // cyc9
    @(posedge clk);                  // cyc10: $finish, never evaluated
    $finish;
  end
endmodule
