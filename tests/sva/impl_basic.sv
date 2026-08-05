// The Phase 2 target shape: one nonoverlapping implication over 1-bit 2-state
// signals, no disable iff, no action block beyond the default report.
//
// The trace is chosen so every verdict class appears exactly once and each is
// attributable: a pass, a failure, and a cycle where no attempt is pending.
module t;
    bit clk = 0;
    bit a = 0;
    bit b = 0;

    always #5 clk = ~clk;

    // a |=> b : whenever a holds on a posedge, b must hold on the next one.
    ap: assert property (@(posedge clk) a |=> b);

    int unsigned cyc = 0;
    always @(posedge clk) begin
        cyc <= cyc + 1;
        $display("[%0t] cycle=%0d a=%b b=%b", $time, cyc, a, b);
    end

    initial begin
        // cycle 0 sampled: a=0 -> no attempt
        @(posedge clk);
        a <= 1;          // cycle 1 sampled: a=1 -> obligation on cycle 2
        @(posedge clk);
        a <= 0; b <= 1;  // cycle 2 sampled: b=1 -> pass
        @(posedge clk);
        b <= 0; a <= 1;  // cycle 3 sampled: a=1 -> obligation on cycle 4
        @(posedge clk);
        a <= 0;          // cycle 4 sampled: b=0 -> FAIL
        @(posedge clk);
        @(posedge clk);
        $display("done");
        $finish;
    end
endmodule
