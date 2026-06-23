module foo;
  logic clk, rst;
  logic [7:0] data;

  initial begin
    clk  = 0;
    rst  = 1;
    data = 100 + 23;
  end
endmodule
