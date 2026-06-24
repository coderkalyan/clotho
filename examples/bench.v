// Simulation-time benchmark: two concurrent timed processes, each looping
// millions of times with a #delay per iteration. Exercises the timing wheel and
// coroutine suspend/resume on every step (the bulk of the work is scheduling,
// not arithmetic), plus same-instant wakeups where the two processes collide
// (every 6 ticks). Deterministic output: the two results are order-independent
// and printed at distinct times (2*N and 3*N), so it matches any conformant
// simulator.
module bench;
  integer i, j;
  integer a, b;

  // Worker A: steps every 2 ticks, finishes at simulated time 2*N.
  initial begin
    a = 0;
    for (i = 0; i < 5000000; i = i + 1) begin
      #2;
      a = a + (i * 3) - 1;
    end
    $display("a=%0d", a);
  end

  // Worker B: steps every 3 ticks, finishes at simulated time 3*N.
  initial begin
    b = 0;
    for (j = 0; j < 5000000; j = j + 1) begin
      #3;
      b = b + (j * j);
    end
    $display("b=%0d", b);
  end
endmodule
