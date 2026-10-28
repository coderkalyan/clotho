// Compiled with `build --emit obj`, linked against clotho_runtime by this
// build's own toolchain and run (CMakeLists.txt, test `aot`). Two definitions,
// so the per-definition units are linked back into one object; a 128-bit
// division, which calls the compiler's runtime builtins; a plusarg, which
// reaches the design through rt_run.
module child
    (input logic clk,
     input logic [127 : 0] d,
     output logic [127 : 0] q);
    always_ff @(posedge clk)
        q <= d / 128'd7;
endmodule

module top;
    logic clk = 0;
    logic [127 : 0] d, q;
    int unsigned n;

    child c(.clk(clk),
            .d(d),
            .q(q));

    initial begin
        if (!$value$plusargs("n=%d", n))
            $fatal(1, "aot: no +n plusarg");
        d = {n, 96'd0};
        #1 clk = 1;
        #1;
        if (q != {n, 96'd0} / 128'd7)
            $fatal(1, "aot: wrong quotient %h", q);
        $display("aot: ok %0d %h", n, q);
        $finish;
    end
endmodule
