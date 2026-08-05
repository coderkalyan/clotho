module t #(parameter [7:0] RV=8'h3C) (input clk, input rst, input rst2, input st, input srst,
  input rst_n, input en, input [7:0] a, input [7:0] iv, output reg [7:0] q, output reg [7:0] q2);
  always_ff @(posedge clk or posedge rst) begin if (en) begin if (rst) q<=0; else q<=a; end else if (rst) q<=0; end
endmodule
