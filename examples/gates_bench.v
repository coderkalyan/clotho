// Gate-level scheduling benchmark: a long buf chain rippled by a toggling
// clock. Every clock edge re-evaluates the whole chain through the gate
// worklist (STAGES gate evals per toggle), so wall-clock tracks raw gate
// event throughput. Functional check: the chain end equals the clock after
// settling, and the counter counts posedges through a gate-driven net.
module top;
    localparam STAGES  = 2000;
    localparam TOGGLES = 2000;

    reg  clk;
    wire [STAGES:0] w;
    wire chain_end;
    integer cnt, i;

    buf b0(w[0], clk);
    genvar g;
    generate
        for (g = 0; g < STAGES; g = g + 1) begin : chain
            buf bg(w[g + 1], w[g]);
        end
    endgenerate
    buf be(chain_end, w[STAGES]);

    always @(posedge chain_end) cnt = cnt + 1;

    initial begin
        cnt = 0;
        clk = 0;
        for (i = 0; i < TOGGLES; i = i + 1) begin
            #1 clk = ~clk;
        end
        #1 $display("cnt=%0d end=%b", cnt, chain_end);
    end
endmodule
