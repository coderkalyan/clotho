// DPI-C in an executable (CMakeLists.txt, tests `exe_dpi_*`): an import, and an
// import that calls back into the design through an export, which the C must
// reach from wherever the library is linked.
module top;
    import "DPI-C" function int c_add
        (input int a,
         input int b);
    import "DPI-C" context function int c_calls_back
        (input int x);
    export "DPI-C" function sv_twice;

    function int sv_twice
        (input int x);
        return 2 * x;
    endfunction

    initial begin
        if (c_add(40, 2) != 42)
            $fatal(1, "dpi: import returned %0d", c_add(40, 2));
        if (c_calls_back(21) != 43)
            $fatal(1, "dpi: callback returned %0d", c_calls_back(21));
        $display("dpi: ok");
    end
endmodule
