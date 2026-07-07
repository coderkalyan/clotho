// File read demo (LRM §21.3.4): write two lines, reopen for reading, then pull
// them back with $fgets / $fgetc and report the stream position.
module readback;
    integer fd, n, pos;
    reg [8*8-1:0] line;
    initial begin
        fd = $fopen("readback.txt", "w");
        $fdisplay(fd, "hello");
        $fdisplay(fd, "world");
        $fclose(fd);

        fd  = $fopen("readback.txt", "r");
        n   = $fgets(line, fd);
        pos = $ftell(fd);
        $display("read %0d chars, now at offset %0d, last byte %c", n, pos, line[7:0]);
        $rewind(fd);
        $display("first char after rewind: %c", $fgetc(fd));
        $fclose(fd);
    end
endmodule
