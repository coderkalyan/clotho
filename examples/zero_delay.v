// Zero-delay (#0 / inactive region) semantics, verified against Icarus. A #0
// parks the process in the inactive queue: it resumes at the SAME time, after
// the active region settles but BEFORE the NBA region commits, and it must not
// advance simulation time. Output is value-based so it is deterministic (it does
// not rely on inter-process active ordering, which the LRM
// leaves implementation-defined).
module zero_delay_test;
  reg [7:0] x;
  reg [7:0] flag;
  reg [7:0] saw_x, saw_flag, saw_rt;
  integer   z;

  // (1) Inactive runs before the NBA commit: x is blocking-set to 1 and an NBA
  //     stages x<=5; the #0 continuation must still observe the pre-commit 1.
  initial begin
    x = 1;
    x <= 5;
    #0 saw_x = x;          // expect 1 (NBA not yet committed)
  end

  // (2) #0 does not advance time: flag is set at time 5; a #0 at time 0 must
  //     still observe it as 0.
  initial #5 flag = 1;
  initial begin
    flag = 0;
    #0 saw_flag = flag;    // expect 0
  end

  // (3) A runtime-evaluated zero delay (#(z) with z==0) is inactive too.
  initial begin
    z = 0;
    saw_rt = 8'hAA;
    #(z) saw_rt = 8'hBB;   // parks inactive, resumes the same instant
  end

  // Report once everything has settled (x has committed to 5 by now).
  initial begin
    #10 $display("saw_x=%0d saw_flag=%0d saw_rt=%h x=%0d",
                 saw_x, saw_flag, saw_rt, x);
  end
endmodule
