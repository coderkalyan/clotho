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
