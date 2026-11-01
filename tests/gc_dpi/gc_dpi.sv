// A class handle held only by an SV frame across a DPI import whose C calls an exported
// subroutine that allocates enough to force collections from inside the allocation
// (docs/design/garbage-collection.md 6.1). Class handles cannot cross the DPI boundary
// (LRM 35.5.6), so what is under test is that the frames on both sides of the C frame
// keep what they hold. Prints "gc dpi: 7 8 9 10" when every handle is intact.
class C; int v; string tag; endclass
class Junk; int v; string tag; endclass
module top;
  parameter int N = 3000;
  import "DPI-C" context function int c_churn(input int n);
  import "DPI-C" context function int c_hold(input int n);
  import "DPI-C" context task c_task_hold(input int n);
  export "DPI-C" function sv_churn;
  export "DPI-C" function sv_hold;
  export "DPI-C" task sv_task_hold;

  int task_r;

  function automatic C make(int v);
    C c = new;
    c.v = v;
    c.tag = $sformatf("kept%0d", v);
    return c;
  endfunction

  function automatic int sv_churn(input int n);
    Junk j;
    for (int i = 0; i < n; i++) begin
      j = new;
      j.v = 32'h5A5A5A5A;
      j.tag = "junk";
    end
    return n;
  endfunction

  function automatic int sv_hold(input int n);
    C h = make(9);
    void'(sv_churn(n));
    return h.v + (h.tag == "kept9" ? 0 : 1000);
  endfunction

  task automatic sv_task_hold(input int n);
    C h = make(10);
    void'(sv_churn(n));
    task_r = h.v + (h.tag == "kept10" ? 0 : 1000);
  endtask

  function automatic int fn_hold(int n);
    C h = make(8);
    void'(c_churn(n));
    return h.v + (h.tag == "kept8" ? 0 : 1000);
  endfunction

  task automatic task_hold(int n, output int r);
    C h = make(7);
    void'(c_churn(n));
    r = h.v + (h.tag == "kept7" ? 0 : 1000);
  endtask

  initial begin
    int a, b, c, d;
    task_hold(N, a);
    b = fn_hold(N);
    c = c_hold(N);
    c_task_hold(N);
    d = task_r;
    $display("gc dpi: %0d %0d %0d %0d", a, b, c, d);
  end
endmodule
