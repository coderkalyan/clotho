// Registered ALU datapath: a combinational next-result block feeding a clocked
// register with synchronous reset. Exercises the lowering proof-of-concept end
// to end (chained operators, if/else, blocking + nonblocking assigns).
module alu(
    input             clk,
    input             rst,
    input      [1:0]  op,
    input      [7:0]  a,
    input      [7:0]  b,
    output     [7:0]  result
);
    reg  [7:0] acc;
    wire [7:0] next;

    // Combinational next-result: a chained-operator datapath. Real designs lean
    // on operator depth like this far more than the wide mux-trees that come from
    // case statements (case lowering is a TODO).
    assign next = (((a + b) ^ (a & b)) + ((a | b) - (a ^ b))) >> op;

    // Sequential register with synchronous reset.
    always @(posedge clk)
        if (rst)
            acc <= 8'd0;
        else
            acc <= next;

    assign result = acc;
endmodule
