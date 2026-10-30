// A --dpi-lib definition shadows a same-named libc symbol (CMakeLists.txt, tests
// `dpi_shadows_libc` for the JIT and `exe_dpi_shadow` for the linked executable).
// Oracle: none; LRM 35 binds an import to the C function the user's library
// supplies, and the real getpid returns a pid that is never 31337 here.
module top;
    import "DPI-C" function int getpid();

    initial $display("pid=%0d", getpid());
endmodule
