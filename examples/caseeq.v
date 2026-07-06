module caseeq (
    input  [31:0] a,
    input  [31:0] b,
    output result
);
  assign result = a === b;
endmodule
