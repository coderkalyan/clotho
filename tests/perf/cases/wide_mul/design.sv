// Wide-vector arithmetic: 2048-bit multiply/shift/add chain in a zero-time
// loop. The work lands in wide-integer libcalls, so the
// JIT's dispatch advantage is mostly neutralized -- clotho's smallest
// compute-bound lead (~4.5x over iverilog, 2026-07; contrast the 64-bit
// loops at >100x). Tracks wide-op codegen; a width-specialized
// multiply or 2-state lowering moves this number.
module top;
  logic [2047:0] a, b;
  int i;
  initial begin
    a = 2048'h1;
    b = {64{32'h9e3779b1}};
    for (i = 0; i < 200000; i++) begin
      a = a * b + (a >> 137) + 2048'h5;
    end
    $display("done %h", a[63:0]);
    $finish;
  end
endmodule
