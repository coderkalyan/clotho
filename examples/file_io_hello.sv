// Minimal file-I/O demo (LRM §21.3): open a file, write to it, close it.
module hello;
    initial begin
        int fd;
        fd = $fopen("hello.txt", "w");
        $fdisplay(fd, "hello, file %0d", 42);
        $fclose(fd);
    end
endmodule
