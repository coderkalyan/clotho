module m;
  bit clk;
  bit [7:0] mem [0:3];
  int i;
  initial begin
    mem[0]=0; mem[1]=0; mem[2]=0; mem[3]=0;
    for (i=0;i<3;i++) begin clk=0; #1; clk=1; #1; end
    $display("%0d %0d %0d %0d", mem[0], mem[1], mem[2], mem[3]);
    $finish;
  end
  always @(posedge clk) begin mem[0] <= mem[0]+8'd1; mem[2] <= mem[2]+8'd5; end
endmodule
