// `disable iff` abandoning the attempts that are in flight.
// The trace is driven with nonblocking assignments so every sampled value is
// unambiguous, and $display marks each cycle so a report can be attributed.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit a = 0, b = 0, c = 0, rst = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d a=%b b=%b c=%b rst=%b", $time, cyc, a, b, c, rst);
  end

  // disable iff abandons everything in flight, and seeds nothing that cycle.
  ap: assert property (@(posedge clk) disable iff (rst) a |=> b);

  initial begin
    @(posedge clk);
    a <= 1;              // cyc1: seeds
    @(posedge clk);
    a <= 0; rst <= 1;    // cyc2: reset kills the pending obligation -> no fail
    @(posedge clk);
    rst <= 0; a <= 1;    // cyc3: seeds again
    @(posedge clk);
    a <= 0;              // cyc4: b low -> FAIL
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
