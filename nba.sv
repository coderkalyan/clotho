module m;
  bit clk;
  bit [7:0] q, r;
  int shifted;
  bit [7:0] pw;
  initial begin
    q = 0; r = 8'hFF; pw = 0;
    for (int i=0;i<4;i++) begin clk=0; #1; clk=1; #1; end
    $display("q=%0d r=%0d pw=%0d", q, r, pw);
    $finish;
  end
  always @(posedge clk) begin
    q <= q + 8'd3;           // whole-signal NBA
    r <= r - 8'd1;
    pw[3:0] <= pw[3:0] + 4'd2;  // partial (part-select) NBA
  end
endmodule
