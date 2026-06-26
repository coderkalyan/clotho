// Nonblocking-assignment semantics, verified against Icarus. Exercises the NBA
// update region: intra-process RAW swap, a shift register sampling old values, a
// gated register, and a part-select target (the partial mask-RMW commit path).
// A flop feeds a level-sensitive combinational block to confirm the comb re-runs
// after the NBA commit, within the same instant, before time advances.
module nba_test;
  reg        clk;
  reg  [7:0] a, b;        // RAW swap
  reg  [7:0] q1, q2, q3;  // shift register
  reg        en;
  reg  [7:0] g;           // gated register
  reg  [7:0] d;           // shift-register / gate input
  reg  [7:0] p;           // part-select target
  reg  [7:0] doubled;     // combinational, sensitive to q1
  integer    i;

  // RAW swap: must exchange (blocking would copy).
  always @(posedge clk) begin
    a <= b;
    b <= a;
  end

  // Shift register: each stage samples the OLD value of the previous one.
  always @(posedge clk) begin
    q1 <= d;
    q2 <= q1;
    q3 <= q2;
  end

  // Gated register: only updates when enabled; flag set inside the branch.
  always @(posedge clk)
    if (en)
      g <= d;

  // Part-select target: two halves written by separate NBAs in one instant.
  always @(posedge clk) begin
    p[3:0] <= d[3:0];
    p[7:4] <= d[7:4] + 4'd1;
  end

  // Combinational, level-sensitive on q1 (a committed NBA output): must re-run
  // after the NBA region commits q1, in the same instant.
  always @(q1)
    doubled = q1 + q1;

  initial begin
    clk = 0;
    a = 8'h11; b = 8'h22;
    q1 = 0; q2 = 0; q3 = 0;
    en = 0; g = 8'hEE;
    p = 0; d = 0;
    for (i = 1; i <= 5; i = i + 1) begin
      d  = i;
      en = i[0];           // enable on odd cycles
      #5 clk = 1;          // rising edge: all NBAs sample then commit
      #5 clk = 0;
      $display("c%0d a=%0d b=%0d q1=%0d q2=%0d q3=%0d g=%0d p=%h doubled=%0d",
               i, a, b, q1, q2, q3, g, p, doubled);
    end
  end
endmodule
