// Registered ALU datapath: a combinational next-result block feeding a clocked
// register with synchronous reset. Exercises the lowering proof-of-concept end
// to end (operators, ?: select, if/else, blocking + nonblocking assigns).
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

    // Combinational next-result selection.
    assign next = (op == 2'd0) ? (a + b) :
                  (op == 2'd1) ? (a - b) :
                  (op == 2'd2) ? (a & b) :
                                 (a ^ b);

    // Sequential register with synchronous reset.
    always @(posedge clk)
        if (rst)
            acc <= 8'd0;
        else
            acc <= next;

    assign result = acc;
endmodule
