module t(input clk, input rst, input [7:0] a, output reg [7:0] q, output reg [7:0] q2);
  always @(posedge clk or posedge rst) begin
    if (rst) q<=0; else q<=a;
    q2<=a;
  end
endmodule
