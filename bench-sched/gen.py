#!/usr/bin/env python3
"""Generate tier-1 scheduling benchmarks.

Bench "regs": N independent clocked NBA processes (split) vs one fused block.
  Measures per-activation overhead: same total work, N x fewer activations
  when fused. NBA-only cross reads make declaration order irrelevant
  (control for noise).

Bench "thunk": isolates per-activation *coroutine* overhead (frame + resume/
  suspend machinery) for simple non-suspending processes -- the population a
  planned thunk lowering would run as a plain call instead of a coroutine.
  Trivial bodies so the coroutine machinery, not the work, dominates.
  Shape "comb": N combinational procs each with a DISTINCT sensitivity
    (always @(a_i) y_i = f(a_i)), fed by one clocked driver that toggles all
    a_i every cycle. Distinct sensitivity sets are fusion-INELIGIBLE, so the
    default (fusion-on) count stays N -- coroutine overhead fusion cannot hide.
    N activations + N driver writes per cycle.
  Shape "ff": N edge-triggered procs with a trivial blocking self-update
    (always @(posedge clk) if (cyc<N) q_i = q_i + k). Blocking, not NBA, so the
    signal is coroutine overhead alone, not the NBA heap-churn path whose
    allocator jitter otherwise dominates; each q_i reads only itself so it is
    race-free (the cyc guard stops updates before the settled read). Shared
    sensitivity fuses to one coroutine by default; --no-fuse exposes the
    N-coroutine cost (brackets the residual overhead a thunk removes on fusion).

Bench "tri": triangular blocking-assign combinational network. Proc i is
  sensitive to (s, a1..a_{i-1}) and recomputes a_i = s + sum(a_j). All K
  procs wake on every change of s. Clotho's active queue is a LIFO fed in
  subscription (= declaration) order, so declaration in topo order pops in
  reverse topo order (worst case, O(K^2) activations per edge) and reverse
  topo declaration pops in topo order (best case, K activations per edge).
  Measures wasted effort from arbitrary scheduling order.
"""

import argparse
import random
import sys


def gen_regs(n: int, cycles: int, fused: bool, seed: int | None) -> str:
    out = []
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  reg [31:0] cyc = 0;")
    for i in range(n):
        out.append(f"  reg [31:0] x{i} = 32'd{i * 2654435761 % 4294967296};")
    out.append("  always #5 clk = ~clk;")
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    out.append(f"    if (cyc == 32'd{cycles}) begin")
    xor = " ^ ".join(f"x{i}" for i in range(n))
    out.append(f'      $display("done %h", {xor});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    stmts = [
        f"x{i} <= (x{i} ^ x{(i + 1) % n}) + 32'd{(i * 40503 + 12345) % 65536};"
        for i in range(n)
    ]
    if fused:
        out.append("  always @(posedge clk) begin")
        for s in stmts:
            out.append(f"    {s}")
        out.append("  end")
    else:
        order = list(range(n))
        if seed is not None:
            random.Random(seed).shuffle(order)
        for i in order:
            out.append(f"  always @(posedge clk) {stmts[i]}")
    out.append("endmodule")
    return "\n".join(out) + "\n"


def gen_thunk(n: int, cycles: int, shape: str) -> str:
    out = []
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  reg [31:0] cyc = 0;")
    out.append("  always #5 clk = ~clk;")
    if shape == "comb":
        # One clocked driver toggles every source; each source feeds exactly one
        # combinational proc with its own (distinct) sensitivity -> no fusion.
        for i in range(n):
            out.append(f"  reg [31:0] a{i} = 32'd{i * 2654435761 % 4294967296};")
        for i in range(n):
            out.append(f"  reg [31:0] y{i} = 0;")
        out.append("  always @(posedge clk) begin")
        for i in range(n):
            out.append(f"    a{i} <= a{i} + 32'd{(i * 40503 + 12345) % 65536 | 1};")
        out.append("  end")
        for i in range(n):
            out.append(f"  always @(a{i}) y{i} = a{i} ^ 32'd{(i * 2246822519 % 4294967296)};")
        checksum = " ^ ".join(f"y{i}" for i in range(n))
    elif shape == "ff":
        # Blocking (not NBA) self-update so the bench isolates per-activation
        # coroutine overhead, not the NBA heap-churn path (rt_nba_arr_push, an
        # unimplemented inline-storage win) whose allocator jitter otherwise
        # dominates the measurement. Each q_i reads only itself, so blocking is
        # race-free among the procs; the cyc<CYCLES guard stops updates before
        # the settled read below (same discipline as the comb finisher).
        for i in range(n):
            out.append(f"  reg [31:0] q{i} = 32'd{i * 2654435761 % 4294967296};")
        for i in range(n):
            out.append(f"  always @(posedge clk) if (cyc < 32'd{cycles})"
                       f" q{i} = q{i} + 32'd{(i * 40503 + 12345) % 65536 | 1};")
        checksum = " ^ ".join(f"q{i}" for i in range(n))
    else:
        raise SystemExit(f"bad shape: {shape}")
    stop = cycles + 10 if shape == "ff" else cycles
    # Finisher runs a few edges past the stimulus stop so the net is settled
    # when the checksum is printed (avoids an active-region read race).
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    out.append(f"    if (cyc == 32'd{stop}) begin")
    out.append(f'      $display("done %h", {checksum});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"


def gen_tri(k: int, cycles: int, fused: bool, order: str) -> str:
    out = []
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  reg [31:0] cyc = 0;")
    out.append("  reg [31:0] s = 0;")
    for i in range(1, k + 1):
        out.append(f"  reg [31:0] a{i} = 32'd{i};")
    out.append("  always #5 clk = ~clk;")
    # Stimulus stops CYCLES edges in; finisher fires 10 edges later so the
    # combinational net is settled when the result is printed (avoids an
    # active-region read race that would differ across orders).
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    out.append(f"    if (cyc < 32'd{cycles}) s = s + 1;")
    out.append(f"    if (cyc == 32'd{cycles + 10}) begin")
    out.append(f'      $display("done %h", a{k});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")

    def sens(i: int) -> str:
        terms = ["s"] + [f"a{j}" for j in range(1, i)]
        return " or ".join(terms)

    def body(i: int) -> str:
        terms = ["s"] + [f"a{j}" for j in range(1, i)]
        return f"a{i} = " + " + ".join(terms) + ";"

    if fused:
        out.append("  always @(s) begin")
        for i in range(1, k + 1):
            out.append(f"    {body(i)}")
        out.append("  end")
    else:
        idx = list(range(1, k + 1))
        if order == "topo":
            pass
        elif order == "revtopo":
            idx.reverse()
        elif order.startswith("s"):
            random.Random(int(order[1:])).shuffle(idx)
        else:
            raise SystemExit(f"bad order: {order}")
        for i in idx:
            out.append(f"  always @({sens(i)}) {body(i)}")
    out.append("endmodule")
    return "\n".join(out) + "\n"


def gen_chain(k: int, cycles: int, fused: bool, order: str) -> str:
    """Sparse counterpart of tri: proc i reads only s and a_{i-1}. Waste from
    bad order is bounded (~2x), the realistic envelope for RTL-like nets."""
    out = []
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  reg [31:0] cyc = 0;")
    out.append("  reg [31:0] s = 0;")
    for i in range(1, k + 1):
        out.append(f"  reg [31:0] a{i} = 32'd{i};")
    out.append("  always #5 clk = ~clk;")
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    out.append(f"    if (cyc < 32'd{cycles}) s = s + 1;")
    out.append(f"    if (cyc == 32'd{cycles + 10}) begin")
    out.append(f'      $display("done %h", a{k});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")

    def stmt(i: int) -> str:
        prev = "s" if i == 1 else f"a{i - 1}"
        return f"a{i} = s + {prev};"

    if fused:
        out.append("  always @(s) begin")
        for i in range(1, k + 1):
            out.append(f"    {stmt(i)}")
        out.append("  end")
    else:
        idx = list(range(1, k + 1))
        if order == "topo":
            pass
        elif order == "revtopo":
            idx.reverse()
        elif order.startswith("s"):
            random.Random(int(order[1:])).shuffle(idx)
        else:
            raise SystemExit(f"bad order: {order}")
        for i in idx:
            sens = "s" if i == 1 else f"s or a{i - 1}"
            out.append(f"  always @({sens}) {stmt(i)}")
    out.append("endmodule")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("bench", choices=["regs", "tri", "chain", "thunk"])
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--cycles", type=int, required=True)
    ap.add_argument("--fused", action="store_true")
    ap.add_argument("--seed", type=int, default=None, help="regs: shuffle decl order")
    ap.add_argument("--order", default="topo", help="tri: topo|revtopo|s<seed>")
    ap.add_argument("--shape", default="comb", help="thunk: comb|ff")
    a = ap.parse_args()
    if a.bench == "regs":
        sys.stdout.write(gen_regs(a.n, a.cycles, a.fused, a.seed))
    elif a.bench == "tri":
        sys.stdout.write(gen_tri(a.n, a.cycles, a.fused, a.order))
    elif a.bench == "chain":
        sys.stdout.write(gen_chain(a.n, a.cycles, a.fused, a.order))
    else:
        sys.stdout.write(gen_thunk(a.n, a.cycles, a.shape))
