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

def gen_wideff(n: int, cycles: int) -> str:
    # One clocked process whose single straight-line block holds N whole-signal
    # scalar NBAs to independent flops. Isolates NBA *enqueue* overhead: per-signal
    # lowering enqueues each of the N signals every cycle (O(N)); block-commit
    # lowering enqueues the one block (O(1)). Self-increment RHS keeps the work
    # per assignment trivial so enqueue/commit machinery dominates. All state,
    # the cycle counter, and the finisher share the one block (one process, one
    # block) so nothing else perturbs the enqueue count.
    out = []
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  reg [31:0] cyc = 0;")
    for i in range(n):
        out.append(f"  reg [31:0] q{i} = 32'd{i * 2654435761 % 4294967296};")
    out.append("  always #5 clk = ~clk;")
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    for i in range(n):
        out.append(f"    q{i} <= q{i} + 32'd{(i * 40503 + 12345) % 65536 | 1};")
    xor = " ^ ".join(f"q{i}" for i in range(n))
    out.append(f"    if (cyc == 32'd{cycles}) begin")
    out.append(f'      $display("done %h", {xor});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"

def gen_nbablocks(blocks: int, sig_per_block: int, cycles: int) -> str:
    # `blocks` independent clocked processes, each staging `sig_per_block` whole-
    # signal scalar NBAs to independent flops. Total committed signals =
    # blocks * sig_per_block; holding that product fixed while varying the split
    # isolates NBA-region *dispatch* cost (per-block commit-fn dispatch) from
    # per-signal *commit* work. blocks=1 is one wide block (= wideff);
    # sig_per_block=1 is dispatch-dominated (one commit fn per flop per cycle),
    # the case that separates threaded dispatch from a central worklist loop.
    n = blocks * sig_per_block
    out = ["module top;", "  reg clk = 0;", "  reg [31:0] cyc = 0;"]
    for i in range(n):
        out.append(f"  reg [31:0] q{i} = 32'd{i * 2654435761 % 4294967296};")
    out.append("  always #5 clk = ~clk;")
    for b in range(blocks):
        out.append("  always @(posedge clk) begin")
        for s in range(sig_per_block):
            i = b * sig_per_block + s
            out.append(f"    q{i} <= q{i} + 32'd{(i * 40503 + 12345) % 65536 | 1};")
        out.append("  end")
    xor = " ^ ".join(f"q{i}" for i in range(n))
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    out.append(f"    if (cyc == 32'd{cycles}) begin")
    out.append(f'      $display("done %h", {xor});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"

def gen_nbamem(n: int, width: int, cycles: int, select: str) -> str:
    # N nonblocking writes to distinct elements of one unpacked-array (memory) per
    # cycle, via a dynamic-index loop -- the array-element NBA deferred-queue path.
    # `width` is the element width (sweep incl. >64 to exercise wide records).
    # select="whole": mem[j] <= ...  (mask-elided); select="part": mem[j][lo] <= ...
    # (masked). Checksum XOR-reduces all elements at the end.
    w = width
    inc = "1"
    # 32-bit XOR fold of each element (>64-bit $display is unsupported).
    words = " ^ ".join(f"mem[j][{k*32+31}:{k*32}]" for k in range((w + 31) // 32))
    out = ["module top;", "  reg clk = 0;", "  reg [31:0] cyc = 0;",
           f"  reg [{w-1}:0] mem [0:{n-1}];",
           "  reg [31:0] acc;", "  integer j;"]
    out.append("  initial begin")
    out.append(f"    for (j = 0; j < {n}; j = j + 1) mem[j] = j * 32'd2654435761;")
    out.append("  end")
    out.append("  always #5 clk = ~clk;")
    out.append("  always @(posedge clk) begin")
    if select == "part":
        half = w // 2
        out.append(f"    for (j = 0; j < {n}; j = j + 1) mem[j][{half-1}:0] <= mem[j][{half-1}:0] + {inc};")
    else:
        out.append(f"    for (j = 0; j < {n}; j = j + 1) mem[j] <= mem[j] + {inc};")
    out.append("  end")
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    out.append(f"    if (cyc == 32'd{cycles}) begin")
    out.append("      acc = 0;")
    out.append(f"      for (j = 0; j < {n}; j = j + 1) acc = acc ^ {words};")
    out.append('      $display("done %h", acc);')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"

def gen_partblocks(blocks: int, sig_per_block: int, cycles: int, select: str) -> str:
    # Like nbablocks, but every NBA is a *partial* (bit/part-select) target, to
    # exercise the masked commit path. select="static": a constant [15:0] half-word
    # RMW (compile-time mask); select="dynamic": an indexed part-select [off +: 8]
    # at a runtime offset (runtime mask). Same blocks x sig_per_block dispatch axis
    # as nbablocks so the two are comparable.
    n = blocks * sig_per_block
    out = ["module top;", "  reg clk = 0;", "  reg [31:0] cyc = 0;"]
    for i in range(n):
        out.append(f"  reg [31:0] q{i} = 32'd{i * 2654435761 % 4294967296};")
    out.append("  always #5 clk = ~clk;")
    for b in range(blocks):
        out.append("  always @(posedge clk) begin")
        for s in range(sig_per_block):
            i = b * sig_per_block + s
            c = (i * 40503 + 12345) % 65536 | 1
            if select == "dynamic":
                # off in {0,8,16,24} (always in bounds for [off +: 8]).
                out.append(f"    q{i}[(cyc[1:0] << 3) +: 8] <= q{i}[7:0] + 8'd{c & 0xff | 1};")
            else:
                out.append(f"    q{i}[15:0] <= q{i}[15:0] + 16'd{c};")
        out.append("  end")
    xor = " ^ ".join(f"q{i}" for i in range(n))
    out.append("  always @(posedge clk) begin")
    out.append("    cyc <= cyc + 1;")
    out.append(f"    if (cyc == 32'd{cycles}) begin")
    out.append(f'      $display("done %h", {xor});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
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

def gen_wheel(cycles: int) -> str:
    """Timestep-bound clock microbench: one #delay clock generator and one
    trivial clocked process, so ~2 wheel ticks per cycle dominate over the
    single per-cycle activation. Isolates the timing-wheel / JIT<->runtime ABI
    boundary cost (rt_wheel_advance), which activation-bound benches hide."""
    return (
        "module top;\n"
        "  reg clk = 0;\n"
        "  reg [31:0] acc = 0;\n"
        "  reg [31:0] cyc = 0;\n"
        "  always #5 clk = ~clk;\n"
        "  always @(posedge clk) begin\n"
        "    acc <= acc + 32'd2654435761;\n"
        "    cyc <= cyc + 32'd1;\n"
        f"    if (cyc == 32'd{cycles}) begin\n"
        '      $display("done %h", acc);\n'
        "      $finish;\n"
        "    end\n"
        "  end\n"
        "endmodule\n"
    )

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

def gen_combchain(depth: int, cycles: int) -> str:
    """Deep combinational module chain: DEPTH pass-through cell modules, each
    one continuous assign, wired input->output through ports. Every clock edge
    ripples one wave of DEPTH single-assign activations down the chain, so the
    bench is dominated by per-wave wake/subscription/port-copy cost -- the
    target of comb-process fusion.
    2026-07); per-activation cost measured ~4x the thunk_comb figure."""
    out = []
    out.append("module stage(input wire [31:0] in, output wire [31:0] out);")
    out.append("  assign out = in + 32'h1;")
    out.append("endmodule")
    out.append("")
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  reg [31:0] src = 0;")
    out.append("  integer c = 0;")
    out.append("  wire [31:0] w0;")
    out.append("  assign w0 = src;")
    for i in range(depth):
        out.append(f"  wire [31:0] w{i + 1};")
        out.append(f"  stage s{i}(.in(w{i}), .out(w{i + 1}));")
    out.append("  always #5 clk = ~clk;")
    # The finisher reads the far end one full cycle after the last src update
    # (NBA src, wave settles within the same timestep), so the read is settled
    # and deterministic.
    out.append("  always @(posedge clk) begin")
    out.append("    src <= src + 3;")
    out.append("    c <= c + 1;")
    out.append(f"    if (c == {cycles}) begin")
    out.append(f'      $display("done %h", w{depth});')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"

def _primes(count: int, start: int = 3):
    """First COUNT odd primes from START."""
    primes = []
    cand = start
    while len(primes) < count:
        if all(cand % p for p in range(3, int(cand**0.5) + 1, 2)) and cand % 2:
            primes.append(cand)
        cand += 2
    return primes

def gen_timers(n: int, horizon: int) -> str:
    """N free-running `always #p` toggles with distinct prime periods: keeps N
    timers pending concurrently, spread across timing-wheel levels. The
    single-timer `wheel` bench never holds more than a couple of pending
    entries, so multi-timer occupancy (schedule/advance with populated levels)
    is only visible here. The watchdog samples at the first time >= HORIZON
    that no period divides, so no toggle coincides with the read (same-instant
    ordering would otherwise be legal nondeterminism)."""
    periods = _primes(n)
    end = horizon
    while any(end % p == 0 for p in periods):
        end += 1
    out = []
    out.append("module top;")
    for i, p in enumerate(periods):
        out.append(f"  reg x{i} = 0;")
        out.append(f"  always begin #{p} x{i} = ~x{i}; end")
    out.append("  reg [31:0] acc;")
    out.append("  initial begin")
    out.append(f"    #{end};")
    out.append("    acc = 0;")
    for g in range(0, n, 32):
        bits = ", ".join(f"x{i}" for i in range(g, min(g + 32, n)))
        out.append(f"    acc = acc + {{{bits}}};")
    out.append('    $display("done %h", acc);')
    out.append("    $finish;")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"

def gen_bigcase(n: int, cycles: int) -> str:
    """One clocked process holding an N-entry case statement over a hashed
    selector. Tracks two costs on the same design: LLVM compile time of a
    large single function (measured superlinear at tier 0: 1024 entries
    0.7 s, 4096 entries 11.6 s, 2026-07) and case-dispatch sim cost (lowers
    to a compare chain today, so per-cycle cost grows with N; a jump table
    would flatten it)."""
    sel_bits = max(1, (n - 1).bit_length())
    out = []
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  integer c = 0;")
    out.append("  reg [31:0] sel = 0;")
    out.append("  reg [31:0] out_r = 0;")
    out.append("  always #5 clk = ~clk;")
    out.append("  always @(posedge clk) begin")
    out.append(f"    case (sel[{sel_bits - 1}:0])")
    for i in range(n):
        out.append(f"      {sel_bits}'d{i}: out_r = out_r + 32'h{(i * 2654435761) & 0xffffffff:08x};")
    out.append("    endcase")
    out.append("    sel <= sel + 32'h9e3779b1;")
    out.append("    c <= c + 1;")
    out.append(f"    if (c == {cycles}) begin")
    out.append('      $display("done %h", out_r);')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"

def gen_onehot(n: int, cycles: int, order: str) -> str:
    """One clocked process holding the reverse-case one-hot priority-encoder
    idiom: `case (1'b1)` over N single-bit selects `sel[i]` of a packed vector.
    First set bit (in source order) wins (LRM 12.5). A walking one-hot `sel`
    rotates each cycle so every arm fires in turn. Lowers to an O(N) `sel[i] ===
    1'b1` compare chain today; a count-trailing-zeros + jump table would flatten
    dispatch and compile. `order` picks the arm order: lsb (sel[0]..sel[N-1]),
    msb (reversed), or s<seed> (shuffled) to exercise the permutation path."""
    idx = list(range(n))
    if order == "msb":
        idx = idx[::-1]
    elif order.startswith("s"):
        rng = random.Random(int(order[1:] or 0))
        rng.shuffle(idx)
    elif order not in ("lsb", "topo"):  # topo is the shared arg default.
        raise SystemExit(f"onehot: bad order {order!r} (lsb|msb|s<seed>)")
    out = []
    out.append("module top;")
    out.append("  reg clk = 0;")
    out.append("  integer c = 0;")
    out.append(f"  reg [{n - 1}:0] sel = {n}'d1;")
    out.append("  reg [31:0] out_r = 0;")
    out.append("  always #5 clk = ~clk;")
    out.append("  always @(posedge clk) begin")
    out.append("    case (1'b1)")
    for i in idx:
        out.append(f"      sel[{i}]: out_r <= out_r + 32'h{(i * 2654435761) & 0xffffffff:08x};")
    out.append("    endcase")
    out.append(f"    sel <= {{sel[{n - 2}:0], sel[{n - 1}]}};")
    out.append("    c <= c + 1;")
    out.append(f"    if (c == {cycles}) begin")
    out.append('      $display("done %h", out_r);')
    out.append("      $finish;")
    out.append("    end")
    out.append("  end")
    out.append("endmodule")
    return "\n".join(out) + "\n"

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("bench", choices=["regs", "tri", "chain", "thunk", "wheel",
                                      "combchain", "timers", "bigcase", "onehot",
                                      "wideff", "nbablocks", "partblocks", "nbamem"])
    ap.add_argument("--n", type=int, required=True,
                    help="nbablocks: number of blocks (processes)")
    ap.add_argument("--cycles", type=int, required=True)
    ap.add_argument("--fused", action="store_true")
    ap.add_argument("--seed", type=int, default=None, help="regs: shuffle decl order")
    ap.add_argument("--order", default="topo", help="tri: topo|revtopo|s<seed>")
    ap.add_argument("--shape", default="comb", help="thunk: comb|ff")
    ap.add_argument("--sig-per-block", type=int, default=1,
                    help="nbablocks/partblocks: NBAs per block")
    ap.add_argument("--select", default="static",
                    help="partblocks: static|dynamic; nbamem: whole|part")
    ap.add_argument("--width", type=int, default=32, help="nbamem: element width")
    a = ap.parse_args()
    if a.bench == "regs":
        sys.stdout.write(gen_regs(a.n, a.cycles, a.fused, a.seed))
    elif a.bench == "tri":
        sys.stdout.write(gen_tri(a.n, a.cycles, a.fused, a.order))
    elif a.bench == "chain":
        sys.stdout.write(gen_chain(a.n, a.cycles, a.fused, a.order))
    elif a.bench == "thunk":
        sys.stdout.write(gen_thunk(a.n, a.cycles, a.shape))
    elif a.bench == "combchain":
        sys.stdout.write(gen_combchain(a.n, a.cycles))
    elif a.bench == "timers":
        sys.stdout.write(gen_timers(a.n, a.cycles))
    elif a.bench == "bigcase":
        sys.stdout.write(gen_bigcase(a.n, a.cycles))
    elif a.bench == "onehot":
        sys.stdout.write(gen_onehot(a.n, a.cycles, a.order))
    elif a.bench == "wideff":
        sys.stdout.write(gen_wideff(a.n, a.cycles))
    elif a.bench == "nbablocks":
        sys.stdout.write(gen_nbablocks(a.n, a.sig_per_block, a.cycles))
    elif a.bench == "partblocks":
        sys.stdout.write(gen_partblocks(a.n, a.sig_per_block, a.cycles, a.select))
    elif a.bench == "nbamem":
        sys.stdout.write(gen_nbamem(a.n, a.width, a.cycles, a.select))
    else:
        sys.stdout.write(gen_wheel(a.cycles))
