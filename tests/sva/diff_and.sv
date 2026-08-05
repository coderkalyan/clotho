module t;
  bit clk = 0;
  always #5 clk = ~clk;
  bit trig = 0, a = 0, b = 0, c = 0, d = 0;
  int unsigned cyc = 0;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0t] cyc=%0d trig=%b a=%b b=%b c=%b d=%b", $time, cyc, trig, a, b, c, d);
  end

  // Length-independent conjunction: both operands match from the SAME start
  // cycle, and the pair ends at the LATER of the two ends. The operands have
  // different lengths on purpose -- `(a ##1 b)` spans two cycles and `c` one --
  // because that is the case an intersection cannot express at all.
  //
  // The last attempt below is the one that pins "later end": `c` has already
  // matched when `b` fails to arrive, and the attempt still fails.
  apc: assert property (@(posedge clk) trig |=> ((a ##1 b) and c));

  // The same conjunction as an ANTECEDENT, which pins that end cycle from the
  // other side: the obligation is seeded one cycle after the match ends, so if
  // the pair ended where `c` does rather than where `(a ##1 b)` does, `d` would
  // be wanted a cycle earlier than it is.
  apa: assert property (@(posedge clk) ((a ##1 b) and c) |=> d);

  initial begin
    @(posedge clk);
    trig <= 1;                     // cyc1: seeds the apc obligation at cyc2
    @(posedge clk);
    trig <= 0; a <= 1; c <= 1;     // cyc2: both operands start here
    @(posedge clk);
    a <= 0; c <= 0; b <= 1;        // cyc3: the longer operand ends -> apc
    @(posedge clk);                //   passes, and apa wants d at cyc4
    b <= 0; d <= 1;                // cyc4: apa passes
    @(posedge clk);
    d <= 0; trig <= 1;             // cyc5: seeds apc again, at cyc6
    @(posedge clk);
    trig <= 0; a <= 1;             // cyc6: a but NOT c, so the right operand is
    @(posedge clk);                //   dead on the start cycle -> apc FAILS
    a <= 0; b <= 1;                // cyc7: the left operand would have matched
    @(posedge clk);
    b <= 0;                        // cyc8
    @(posedge clk);
    a <= 1; c <= 1;                // cyc9: apa's antecedent starts
    @(posedge clk);
    a <= 0; c <= 0; b <= 1;        // cyc10: it matches cyc9..cyc10, so the
    @(posedge clk);                //   obligation wants d at cyc11
    b <= 0;                        // cyc11: d is 0 -> apa FAILS
    @(posedge clk);
    trig <= 1;                     // cyc12: seeds apc again, at cyc13
    @(posedge clk);
    trig <= 0; a <= 1; c <= 1;     // cyc13: `c` matches here and is DONE
    @(posedge clk);
    a <= 0; c <= 0;                // cyc14: b never arrives, so the pair waits
    @(posedge clk);                //   for the later end and apc FAILS
    @(posedge clk);
    $finish;
  end
endmodule
