// DPI-C in an executable (CMakeLists.txt, tests `exe_dpi_*`): an import, and an
// import that calls back into the design through an export, which the C must
// reach from wherever the library is linked. A second callback passes a logic
// value with x and z bits, which the design must still hold as such.
module top;
    import "DPI-C" function int c_add
        (input int a,
         input int b);
    import "DPI-C" context function int c_calls_back
        (input int x);
    import "DPI-C" context function void c_poke_unknown();
    export "DPI-C" function sv_twice;
    export "DPI-C" function sv_note;

    logic [7:0] noted;

    function void sv_note
        (input logic [7:0] v);
        noted = v;
    endfunction

    function int sv_twice
        (input int x);
        return 2 * x;
    endfunction

    initial begin
        if (c_add(40, 2) != 42)
            $fatal(1, "dpi: import returned %0d", c_add(40, 2));
        if (c_calls_back(21) != 43)
            $fatal(1, "dpi: callback returned %0d", c_calls_back(21));
        c_poke_unknown();
        if (noted !== 8'bzx10_zx10)
            $fatal(1, "dpi: a logic export argument arrived as %b", noted);
        $display("dpi: ok");
    end
endmodule
