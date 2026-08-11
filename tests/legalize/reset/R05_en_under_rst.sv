module t(input clk, input rst, input rst2, input st, input srst, input rst_n,
         input en, input [7:0] a, input [7:0] iv, output reg [7:0] q);
  always @(posedge clk or posedge rst) if (rst) q<=0; else if (en) q<=a;
endmodule
