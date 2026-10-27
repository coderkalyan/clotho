#!/usr/bin/env python3
"""Measure the per-site cost of clotho's scheduling machinery, in retired
instructions, by isolating one mechanism at a time.

Each family is a matched A/B pair whose two arms have identical arithmetic,
identical process/dispatch counts and identical fusion decisions, and differ
only in the mechanism under test. Cost is the SLOPE of retired instructions
against the parameter, taken between two workload lengths so that compile time
and process startup cancel exactly:

    cost = (I(n, long) - I(n, short)) / periods       per arm
    site = (cost_A - cost_B) / n_sites

The slope must be flat in n. If it is not, the two arms are not matched -- check
`--verify`, which diffs the emitted IR block histogram and asserts the only
delta is the mechanism under test. `--verify` reads `clotho build --emit=llvm`,
so it needs a clotho configured with `build` (a Debug build, or
`-DCLOTHO_BUILD_COMMAND=ON`).

Families:
  service   a blocking store to a signal WITH static waiters vs WITHOUT.
            `service_snapshot` (LlvmBackend.cpp:9734) emits nothing when
            `signal_has_waiters` is false, so the same store is measured with
            and without [pre-store load + emit_edge_facts + per-bucket branch +
            emit_static_scan_rt]. The producer is edge-sensitive so neither
            fusion path merges it in either arm.
  nba       `y <= e` vs `y = e` to targets nothing observes: isolates
            [shadow store + block_register + nba_block_enqueue + commit fn +
            chain] from the wake path.
  dispatch  n one-store processes vs one n-store process: isolates the
            per-activation round trip [bucket entry scan + push + flag set +
            pop + tag test + indirect call + flag clear + prologue] plus the
            per-activation operand reloads that a fused body would forward.
  work      the baseline arm of `service`: the arithmetic alone.

Also reports `ex_ret_ind_brch_instr` per period. That counter is exactly 1.000
per process activation (see `dispatch --calibrate`), which makes the dispatch
count of an arbitrary design measurable without instrumenting anything -- note
it also counts one per NBA commit function, per coroutine resume and per
cross-definition member call, so on a real design it is an upper bound on
process activations.

Throwaway measurement tooling, like the rest of bench-sched: results are not
checked in. Numbers recorded in docs/design/regions.md were taken with this.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

EVENTS = "instructions,ex_ret_ind_brch_instr"
PERIOD = 10  # time units per clock period in every generated design


# ---------------------------------------------------------------- generators

def gen_service(n, arm, cycles):
    """arm 'a': consumer sensitive to all n signals -> all n stores service.
    arm 'b': consumer sensitive to y0 only          -> one store services.
    The sched flag dedups the wake, so both arms perform exactly one wake."""
    decls = "\n".join(f"  reg [31:0] y{i};" for i in range(n))
    body = "\n".join(f"    y{i} = a ^ 32'h{0x1000 + i:04x};" for i in range(n))
    sens = " or ".join(f"y{i}" for i in range(n if arm == "a" else 1))
    return f"""module top;
  reg clk = 0;
  reg [31:0] a = 1;
  reg [31:0] t = 0;
{decls}
  always @(posedge clk) a <= a + 1;
  always @(posedge clk) begin
{body}
  end
  always @({sens}) t = y0 + 1;
  always #5 clk = ~clk;
  initial #{cycles} $finish;
endmodule
"""


def gen_nba(n, arm, cycles):
    """arm 'a': nonblocking stores. arm 'b': blocking. Targets are unobserved,
    so neither arm emits any wake-path code."""
    decls = "\n".join(f"  reg [31:0] y{i};" for i in range(n))
    op = "<=" if arm == "a" else " ="
    body = "\n".join(f"    y{i} {op} a ^ 32'h{0x1000 + i:04x};" for i in range(n))
    return f"""module top;
  reg clk = 0;
  reg [31:0] a = 1;
{decls}
  always @(posedge clk) a <= a + 1;
  always @(posedge clk) begin
{body}
  end
  always #5 clk = ~clk;
  initial #{cycles} $finish;
endmodule
"""


def gen_dispatch(n, arm, cycles):
    """arm 'a': n processes, one store each. arm 'b': one process, n stores.
    The per-process `posedge w{i}` term gives each process a distinct canonical
    sensitivity so identical-sensitivity fusion cannot merge them; `w{i}` never
    changes, so it contributes no wakes. Reading `a`, which is not in the
    sensitivity list, keeps every process out of comb fusion
    (CombFuse::ReadOutsideSens) in both arms."""
    decls = "\n".join(f"  reg [31:0] y{i};" for i in range(n))
    ws = "\n".join(f"  reg w{i} = 0;" for i in range(n))
    if arm == "a":
        procs = "\n".join(
            f"  always @(posedge clk or posedge w{i}) y{i} = a ^ 32'h{0x1000 + i:04x};"
            for i in range(n))
    else:
        body = "\n".join(f"    y{i} = a ^ 32'h{0x1000 + i:04x};" for i in range(n))
        procs = f"  always @(posedge clk) begin\n{body}\n  end"
    return f"""module top;
  reg clk = 0;
  reg [31:0] a = 1;
{decls}
{ws}
  always @(posedge clk) a <= a + 1;
{procs}
  always #5 clk = ~clk;
  initial #{cycles} $finish;
endmodule
"""


FAMILIES = {
    "service": (gen_service, lambda n: n - 1,
                "blocking store to an observed signal"),
    "nba": (gen_nba, lambda n: n, "nonblocking store (per signal)"),
    "dispatch": (gen_dispatch, lambda n: n - 1, "process activation round trip"),
}


# ------------------------------------------------------------------ measuring

def counters(clotho, path, reps):
    """Minimum-of-reps retired counters for one `clotho run`."""
    best = None
    for _ in range(reps):
        proc = subprocess.run(["perf", "stat", "-x,", "-e", EVENTS, clotho, "run", path],
                              capture_output=True, text=True)
        got = {}
        for line in proc.stderr.strip().splitlines():
            field = line.split(",")
            if len(field) > 2 and field[0].isdigit():
                got[field[2].split(":")[0]] = int(field[0])
        if len(got) < 2:
            sys.exit(f"perf produced no counters for {path}:\n{proc.stderr[-500:]}")
        if best is None or got["instructions"] < best["instructions"]:
            best = got
    return best


def slope(clotho, gen, n, arm, short, long_, reps, tmp):
    """Retired counters per simulated clock period, compile time differenced out."""
    at = {}
    for cycles in (short, long_):
        path = tmp / f"{gen.__name__}_{n}_{arm}_{cycles}.v"
        path.write_text(gen(n, arm, cycles))
        at[cycles] = counters(clotho, str(path), reps)
    periods = (long_ - short) // PERIOD
    return {k: (at[long_][k] - at[short][k]) / periods for k in at[short]}


def block_histogram(clotho, source, tmp):
    """Emitted-IR basic block names, digits stripped, counted."""
    path = tmp / "verify.v"
    path.write_text(source)
    ir = subprocess.run([clotho, "build", "--emit=llvm", "-o", "-", str(path)],
                        capture_output=True, text=True).stdout
    hist = {}
    for line in ir.splitlines():
        match = re.match(r"^([A-Za-z_][\w.]*):\s", line)
        if match:
            key = re.sub(r"\d+$", "", match.group(1))
            hist[key] = hist.get(key, 0) + 1
    return hist, ir


WAKE_BLOCKS = ("nsta.head", "nsta.body", "nsta.wake")


def verify(clotho, family, n, tmp):
    """Report how the two arms differ, and check the invariant that makes the
    family's slope attributable to one mechanism.

    The invariant is per family, because each family varies something different:
      service   the wake path is the mechanism -> n-1 extra service sites, and
                everything else, function count included, identical.
      nba       staging is the mechanism -> one extra commit function, and the
                wake path identical (targets are unobserved in both arms).
      dispatch  process granularity is the mechanism -> function count differs
                by construction; what must NOT differ is the per-store wake
                path. The activation-count invariant is checked by measuring:
                the `ind/period` column is linear in n with slope exactly 1.
    """
    gen, _, _ = FAMILIES[family]
    ha, ira = block_histogram(clotho, gen(n, "a", 1000), tmp)
    hb, irb = block_histogram(clotho, gen(n, "b", 1000), tmp)
    print(f"{family} n={n}: block histogram delta (arm a - arm b)")
    for key in sorted(set(ha) | set(hb)):
        delta = ha.get(key, 0) - hb.get(key, 0)
        if delta:
            print(f"  {key:18s} {ha.get(key, 0):5d} {hb.get(key, 0):5d}  {delta:+d}")

    def bodies(ir):
        return ir.count("\ndefine ") - len(re.findall(r"@clotho\.nba\.commit_blk\d+\(", ir))

    fails = []
    if family != "dispatch" and bodies(ira) != bodies(irb):
        fails.append(f"body count differs ({bodies(ira)} vs {bodies(irb)})")
    if family in ("nba", "dispatch"):
        for key in WAKE_BLOCKS:
            if ha.get(key, 0) != hb.get(key, 0):
                fails.append(f"wake path differs at {key}")
    if family == "service":
        got = ha.get("nsta.head", 0) - hb.get("nsta.head", 0)
        if got != n - 1:
            fails.append(f"expected {n - 1} extra service sites, got {got}")
    print(f"  bodies {bodies(ira)} vs {bodies(irb)}, "
          f"functions {ira.count(chr(10) + 'define ')} vs {irb.count(chr(10) + 'define ')}")
    for why in fails:
        print(f"  MISMATCH: {why}")
    if not fails:
        print("  arms matched")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("family", choices=sorted(FAMILIES) + ["all"])
    parser.add_argument("--clotho", default="build/clotho")
    parser.add_argument("--sizes", default="2,8,16,32,64")
    parser.add_argument("--short", type=int, default=400_000, help="time units")
    parser.add_argument("--long", type=int, default=4_400_000, help="time units")
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--verify", action="store_true",
                        help="diff the arms' emitted IR instead of measuring")
    args = parser.parse_args()

    sizes = [int(s) for s in args.sizes.split(",")]
    families = sorted(FAMILIES) if args.family == "all" else [args.family]

    with tempfile.TemporaryDirectory(prefix="clotho-cost-") as raw:
        tmp = pathlib.Path(raw)
        for family in families:
            gen, sites, label = FAMILIES[family]
            if args.verify:
                verify(args.clotho, family, sizes[-1], tmp)
                continue
            print(f"\n{family}: {label}")
            print(f"{'n':>4} {'arm a':>10} {'arm b':>10} {'delta':>10} "
                  f"{'per site':>9} {'ind/period':>11}")
            for n in sizes:
                a = slope(args.clotho, gen, n, "a", args.short, args.long, args.reps, tmp)
                b = slope(args.clotho, gen, n, "b", args.short, args.long, args.reps, tmp)
                delta = a["instructions"] - b["instructions"]
                print(f"{n:4d} {a['instructions']:10.1f} {b['instructions']:10.1f} "
                      f"{delta:10.1f} {delta / max(sites(n), 1):9.2f} "
                      f"{a['ex_ret_ind_brch_instr']:11.2f}")


if __name__ == "__main__":
    main()
