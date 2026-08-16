// The `within` operator: an inner match contained in an outer one.
// The trace is driven with nonblocking assignments so every sampled value is
// unambiguous, and $display marks each cycle so a report can be attributed.
module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit s = 0, e = 0, b = 0, c = 0, ok = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d s=%b e=%b b=%b c=%b ok=%b", $time, cyc, s, e, b, c, ok);
  end

  // A match of the inner sequence lies somewhere inside a match of the outer
  // one, and the pair spans OUTER: it starts and ends where outer does. As an
  // antecedent that end cycle is observable, because the obligation is seeded
  // one cycle after it.
  //
  // `ok` is held at 0 everywhere except the one passing attempt, so a match
  // clotho finds and the outer sequence does not contain would report and be
  // caught. The outer match at cyc15..cyc18 has no inner match inside it and
  // must stay silent; the ones at cyc21 and cyc27 put the inner match flush
  // against outer's first and last cycle, where the containment is inclusive.
  apw: assert property (@(posedge clk) ((b ##1 c) within (s ##3 e)) |=> ok);

  initial begin
    @(posedge clk);
    s <= 1;                     // cyc1: outer starts
    @(posedge clk);
    s <= 0; b <= 1;             // cyc2: inner starts, one cycle in
    @(posedge clk);
    b <= 0; c <= 1;             // cyc3: inner ends
    @(posedge clk);
    c <= 0; e <= 1;             // cyc4: outer ends -> within spans cyc1..cyc4
    @(posedge clk);
    e <= 0; ok <= 1;            // cyc5: apw passes
    @(posedge clk);
    ok <= 0;                    // cyc6
    @(posedge clk);
    s <= 1;                     // cyc7: outer starts
    @(posedge clk);
    s <= 0; b <= 1;             // cyc8
    @(posedge clk);
    b <= 0; c <= 1;             // cyc9
    @(posedge clk);
    c <= 0; e <= 1;             // cyc10: within spans cyc7..cyc10
    @(posedge clk);
    e <= 0;                     // cyc11: ok is 0 -> apw FAILS
    @(posedge clk);
                                // cyc12
    @(posedge clk);
    b <= 1;                     // cyc13: an inner match with no outer around it
    @(posedge clk);
    b <= 0; c <= 1;             // cyc14: inner ends
    @(posedge clk);
    c <= 0; s <= 1;             // cyc15: outer starts AFTER that inner match
    @(posedge clk);
    s <= 0;                     // cyc16
    @(posedge clk);
                                // cyc17
    @(posedge clk);
    e <= 1;                     // cyc18: outer spans cyc15..cyc18 with nothing
    @(posedge clk);             //   inside it -> no match, no obligation
    e <= 0;                     // cyc19: ok is 0, so an over-eager match would
    @(posedge clk);             //   report here
                                // cyc20
    @(posedge clk);
    s <= 1; b <= 1;             // cyc21: inner starts FLUSH with outer's first
    @(posedge clk);             //   cycle
    s <= 0; b <= 0; c <= 1;     // cyc22: inner ends
    @(posedge clk);
    c <= 0;                     // cyc23
    @(posedge clk);
    e <= 1;                     // cyc24: within spans cyc21..cyc24
    @(posedge clk);
    e <= 0;                     // cyc25: ok is 0 -> apw FAILS
    @(posedge clk);
                                // cyc26
    @(posedge clk);
    s <= 1;                     // cyc27: outer starts
    @(posedge clk);
    s <= 0;                     // cyc28
    @(posedge clk);
    b <= 1;                     // cyc29
    @(posedge clk);
    b <= 0; c <= 1; e <= 1;     // cyc30: inner ends FLUSH with outer's last
    @(posedge clk);             //   cycle
    c <= 0; e <= 0;             // cyc31: ok is 0 -> apw FAILS
    @(posedge clk);
    @(posedge clk);
    $finish;
  end
endmodule
