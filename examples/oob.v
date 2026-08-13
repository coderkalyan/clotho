module oob (
    input int x
);
  logic foo[10];

  initial begin
    foo[0] = 1;
    $display(foo[~(x&1)*10]);
  end
endmodule
