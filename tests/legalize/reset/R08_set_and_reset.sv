module t(input clk, input rst, input rst2, input st, input srst, input rst_n,
         input en, input [7:0] a, input [7:0] iv, output reg [7:0] q);
  always @(posedge clk or posedge rst or posedge st) if (rst) q<=0; else if (st) q<=8'hFF; else q<=a;
endmodule
