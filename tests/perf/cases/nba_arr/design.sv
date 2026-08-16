// Nonblocking writes to unpacked-array elements, 256 per clock: stresses the
// rt_nba_arr_push commit queue -- four heap allocations per element write and
// a per-timestep queue walk (a known win: inline storage
// for bounded blobs). The register-NBA benches (regs/thunk_ff) never touch
// this path.
module top;
  reg clk = 0;
  reg [31:0] mem [0:1023];
  reg [31:0] acc = 0;
  integer i;
  integer c = 0;
  initial begin
    for (i = 0; i < 1024; i = i + 1) mem[i] = i;
  end
  always #5 clk = ~clk;
  always @(posedge clk) begin
    for (i = 0; i < 256; i = i + 1)
      mem[(c + i) & 1023] <= (acc ^ i) * 32'h9e3779b1;
    acc <= acc + mem[c & 1023] + 1;
    c <= c + 1;
    if (c == 50000) begin
      $display("done %h", acc);
      $finish;
    end
  end
endmodule
