// Repeated string append: s = {s, chunk}, the common logging/report-building
// idiom. Was O(n^2): rt_string concat built the result in scratch, copied to a
// temp, then deep-copied into s every iteration -- 896 ms sim. Fixed 2026-07:
// the backend forwards the concat into the store target and rt_string_concat
// appends in place with amortized growth when the destination is the prefix
// part -- 1.2 ms sim, linear scaling. iverilog remains quadratic here.
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
