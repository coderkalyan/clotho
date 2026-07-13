module m;
  import "DPI-C" function int f(input int a[]);
  int q[$];
  initial begin q.push_back(1); void'(f(q)); end
endmodule
