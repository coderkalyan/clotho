module t;
  bit clk = 0;
  always #5 clk = ~clk;

  bit noise = 0;
  always #3 noise = ~noise;

  logic [3:0]  v = 0;
  bit   [3:0]  w = 0;
  int unsigned cyc = 0;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d v=%0d w=%0d noise=%b", $time, cyc, v, w, noise);
  end

  // Fails on a tick whose sample repeats the previous tick's, and on the first
  // tick, where $past(v) is the X default.
  a_p1: assert property (@(posedge clk) $past(v) != v);

  // Two ticks back, so it fails on a repeat with one tick of separation -- and
  // on the first two ticks, which have no second predecessor.
  a_p2: assert property (@(posedge clk) $past(v, 2) != v);

  // The 2-state twin. Its pre-first-tick default is 0 rather than X, so the
  // first tick fails on a real comparison instead of an unknown one.
  a_w1: assert property (@(posedge clk) $past(w) != w);

  // $sampled is THIS tick's sample, never history, so it always equals the
  // operand read beside it. Never reports; it is here so that a $sampled that
  // wrongly resolved to a ring entry would start reporting.
  a_s: assert property (@(posedge clk) $sampled(v) == v);

  initial begin
    @(posedge clk);            // tick 1 samples v=0 w=0
    v <= 1;  w <= 1;
    @(posedge clk);            // tick 2 samples v=1 w=1
    v <= 3;  w <= 1;
    @(posedge clk);            // tick 3 samples v=3 w=1
    v <= 3;  w <= 2;
    @(posedge clk);            // tick 4 samples v=3 w=2
    v <= 1;  w <= 3;
    @(posedge clk);            // tick 5 samples v=1 w=3
    v <= 5;  w <= 3;
    @(posedge clk);            // tick 6 samples v=5 w=3
    @(posedge clk);            // tick 7 samples v=5 w=3
    $finish;
  end
endmodule
