// Synthetic z-elision benchmark: latency-bound random access over a 4-state
// memory. Every signal is driven, so the analysis elides all z words:
//   elided:   64K x { i128 }       = 1 MB  (fits the 1 MB per-core L2)
//   unelided: 64K x { i128, i64 }  = 2 MB  (spills to L3)
// The acc += mem[...] chains serialize on load latency, so the cache level
// the working set lands in dominates runtime.
module zbench_mem;
    logic [63:0] mem [0:65535];
    logic [63:0] lfsr = 64'hdeadbeefcafe1234;
    logic [63:0] acc  = 0;
    integer i;
    initial begin
        for (i = 0; i < 65536; i = i + 1)
            mem[i] = {32'h0, i[31:0]} * 64'h9e3779b97f4a7c15;
        for (i = 0; i < 40000000; i = i + 1) begin
            lfsr = {lfsr[62:0], lfsr[63] ^ lfsr[62] ^ lfsr[60] ^ lfsr[59]};
            acc  = acc + mem[lfsr[15:0]];
            acc  = acc + mem[acc[47:32]];
            mem[lfsr[31:16]] = acc ^ lfsr;
        end
        $display("%h", acc);
        $finish;
    end
endmodule
