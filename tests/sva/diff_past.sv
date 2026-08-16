// $past and $sampled (LRM 16.9.3). The trace drives `v` and `w` with nonblocking
// assignments so every sampled value is unambiguous, and $display marks each
// cycle so a report can be attributed to one.
//
// `noise` is as much the point of this fixture as `v` is. It toggles on a period
// coprime with the clock, so most time steps carry no clocking edge at all. LRM
// 16.9.3 advances history per CLOCK TICK -- "the most recent strictly prior time
// step in which the clocking event occurred" -- so those extra steps must leave
// $past alone. A history ring shifted by the per-time-step sampling process
// rather than by the monitor would skew several ticks per cycle here and move
// every report below.
//
// `v` is 4-state and `w` is 2-state so that both halves of LRM 16.5.1's default
// sampled value are exercised: before the first tick $past(v) is X, which makes
// `!=` unknown and therefore false, while $past(w) is 0.
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
