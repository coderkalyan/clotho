module top;
  string s;
  int i;
  initial begin
    s = "";
    for (i = 0; i < 60000; i++) begin
      s = {s, "abcdefgh"};
    end
    $display("done %0d %s", s.len(), s.substr(479990, 479999));
    $finish;
  end
endmodule
