#!/usr/bin/env python3
"""Generate tier-1 scheduling benchmarks.

Bench "regs": N independent clocked NBA processes (split) vs one fused block.
  Measures per-activation overhead: same total work, N x fewer activations
  when fused. NBA-only cross reads make declaration order irrelevant
  (control for noise).

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
    ap.add_argument("bench", choices=["regs", "tri", "chain"])
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--cycles", type=int, required=True)
    ap.add_argument("--fused", action="store_true")
    ap.add_argument("--seed", type=int, default=None, help="regs: shuffle decl order")
    ap.add_argument("--order", default="topo", help="tri: topo|revtopo|s<seed>")
    a = ap.parse_args()
    if a.bench == "regs":
        sys.stdout.write(gen_regs(a.n, a.cycles, a.fused, a.seed))
    elif a.bench == "tri":
        sys.stdout.write(gen_tri(a.n, a.cycles, a.fused, a.order))
    else:
        sys.stdout.write(gen_chain(a.n, a.cycles, a.fused, a.order))
