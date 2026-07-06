module m;
  bit [7:0] a [0:3];        // plain -> narrow
  int sum;
  initial begin
    a[0]=8'd5; a[1]=8'd10; a[2]=8'd20; a[3]=8'd40;
    sum = a[0]+a[1]+a[2]+a[3];
    $display("sum=%0d a2=%0d", sum, a[2]);
    $finish;
  end
endmodule
