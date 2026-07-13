module m;
  import "DPI-C" function int f(input bit [15:0] a[]);
  bit [15:0] da[];
  initial begin da = new[2]; void'(f(da)); end
endmodule
