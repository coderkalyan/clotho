// 200k-iteration zero-time loop exercising the string numeric-conversion
// methods (LRM 6.16.9/6.16.11: atoi/atohex/atoreal, itoa/hextoa/realtoa).
// Operands are kept non-negative and the real value always exact in <=6
// significant digits, so the output does not depend on the choices recorded in
// docs/LRM_NOTES.md notes 1 (sign-magnitude toa) and 2 (full-precision realtoa),
// neither of which applies to this operand range --
// verified vs iverilog and verilator, same checksum on both.
module top;
  string s;
  integer r, acc;
  real rl, racc;
  int i;
  initial begin
    acc = 0;
    racc = 0.0;
    for (i = 0; i < 200000; i = i + 1) begin
      s = "12345";
      r = s.atoi();
      acc = acc + r;

      s = "ff";
      r = s.atohex();
      acc = acc + r;

      s = "3.5";
      rl = s.atoreal();
      racc = racc + rl;

      r = i % 100000;
      s.itoa(r);
      acc = acc + s.len();

      r = i % 65536;
      s.hextoa(r);
      acc = acc + s.len();

      rl = (i % 1000) * 0.5;
      s.realtoa(rl);
      acc = acc + s.len();
    end
    $display("done %0d %f", acc, racc);
    $finish;
  end
endmodule
