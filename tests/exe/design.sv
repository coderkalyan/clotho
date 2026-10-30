// Built into an executable by `clotho build` and run (CMakeLists.txt, test
// `exe`). Two definitions, so the per-definition units are linked back into
// one object; a 128-bit division, which calls compiler-rt's builtins; a 192-bit
// multiply, divide and modulo, which call the runtime's rt_wide_* routines; a
// plusarg, which reaches the design through rt_run.
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
    logic [191 : 0] w, wq, wr;
    int unsigned n;

    child c(.clk(clk),
            .d(d),
            .q(q));

    initial begin
        if (!$value$plusargs("n=%d", n))
            $fatal(1, "exe: no +n plusarg");
        d = {n, 96'd0};
        w = {n, 160'h0123456789abcdef0123456789abcdef01234567};
        #1 clk = 1;
        #1;
        if (q != {n, 96'd0} / 128'd7)
            $fatal(1, "exe: wrong quotient %h", q);
        wq = w / 192'd7;
        wr = w % 192'd7;
        if (wq * 192'd7 + wr != w)
            $fatal(1, "exe: wrong 192-bit quotient %h remainder %h", wq, wr);
        if (w * 192'd3 != w + w + w)
            $fatal(1, "exe: wrong 192-bit product");
        $display("exe: ok %0d %h", n, q);
        $finish;
    end
endmodule
