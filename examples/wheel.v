module foo;
  initial begin
    $display("Hello, world!");
    #20;
    $display("Goodbye!");
  end

  initial begin
    #10;
    $display("Waited...");
  end
endmodule
