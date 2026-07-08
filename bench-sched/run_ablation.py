#!/usr/bin/env python3
"""Tier-1 scheduling ablation runner.

Generates the bench matrix, runs clotho (sim time from --time stderr) and
iverilog (vvp wall clock), verifies every config's stdout matches its
baseline, writes results.csv. Throwaway experiment tooling.
"""

import csv
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "gen.py")
PERM = os.path.join(HERE, "permute_pico.py")
CLOTHO = os.path.join(HERE, "..", "build-rel", "clotho")
WORK = "/tmp/sched-ablation/run"
RUNS = 3
TIMEOUT = 300

SIM_RE = re.compile(r"simulation\s+([0-9.]+)")
LLVM_RE = re.compile(r"llvm compile\s+([0-9.]+)")


def sh(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, timeout=TIMEOUT, **kw)


def gen(path, bench, **kw):
    args = ["python3", GEN, bench]
    for k, v in kw.items():
        if v is True:
            args.append(f"--{k}")
        elif v is not None:
            args += [f"--{k}", str(v)]
    with open(path, "w") as f:
        subprocess.run(args, stdout=f, check=True)


def run_clotho(path):
    """Return (sim_ms_list, llvm_ms, stdout) or ('timeout', ...)."""
    sims, llvm, out = [], None, None
    for _ in range(RUNS):
        try:
            r = sh([CLOTHO, "--time", path])
        except subprocess.TimeoutExpired:
            return None, None, "TIMEOUT_KILL"
        m = SIM_RE.search(r.stderr)
        if not m:
            return None, None, f"ERROR: {r.stderr[-300:]}"
        sims.append(float(m.group(1)))
        llvm = float(LLVM_RE.search(r.stderr).group(1))
        out = r.stdout.strip()
    return sims, llvm, out


def run_iverilog(path):
    """Return (wall_s_list, compile_s, stdout)."""
    vvp = path + ".vvp"
    t0 = time.monotonic()
    c = sh(["iverilog", "-o", vvp, path])
    comp = time.monotonic() - t0
    if c.returncode != 0:
        return None, None, f"COMPILE ERROR: {c.stderr[-300:]}"
    walls, out = [], None
    for _ in range(RUNS):
        t0 = time.monotonic()
        try:
            r = sh(["vvp", vvp])
        except subprocess.TimeoutExpired:
            return None, comp, "TIMEOUT_KILL"
        walls.append(time.monotonic() - t0)
        out = "\n".join(
            l for l in r.stdout.strip().splitlines() if "$finish" not in l and "VCD" not in l
        )
    return walls, comp, out


def main():
    os.makedirs(WORK, exist_ok=True)
    jobs = []  # (bench, config, path)

    # regs: activation overhead. split vs fused over N; decl-order control at N=64.
    for n in [1, 8, 64, 256, 512]:
        p = f"{WORK}/regs_n{n}_split.v"
        gen(p, "regs", n=n, cycles=100000)
        jobs.append(("regs", f"n{n}/split", p))
        p = f"{WORK}/regs_n{n}_fused.v"
        gen(p, "regs", n=n, cycles=100000, fused=True)
        jobs.append(("regs", f"n{n}/fused", p))
    for s in [1, 2, 3]:
        p = f"{WORK}/regs_n64_perm{s}.v"
        gen(p, "regs", n=64, cycles=100000, seed=s)
        jobs.append(("regs", f"n64/perm{s}", p))

    # tri: dense-fanin worst case (exponential under bad order). Small K.
    for k in [8, 12, 16]:
        for order in ["topo", "revtopo", "s1", "s2", "s3", "s4"]:
            p = f"{WORK}/tri_k{k}_{order}.v"
            gen(p, "tri", n=k, cycles=200, order=order)
            jobs.append(("tri", f"k{k}/{order}", p))
        p = f"{WORK}/tri_k{k}_fused.v"
        gen(p, "tri", n=k, cycles=200, fused=True)
        jobs.append(("tri", f"k{k}/fused", p))

    # chain: sparse-fanin realistic case (bounded, ~(K+1)/2 activation waste).
    for k in [64, 256]:
        for order in ["topo", "revtopo", "s1", "s2", "s3", "s4"]:
            p = f"{WORK}/chain_k{k}_{order}.v"
            gen(p, "chain", n=k, cycles=2000, order=order)
            jobs.append(("chain", f"k{k}/{order}", p))
        p = f"{WORK}/chain_k{k}_fused.v"
        gen(p, "chain", n=k, cycles=2000, fused=True)
        jobs.append(("chain", f"k{k}/fused", p))

    # pico: baseline (preprocessed) + constrained permutations.
    pico = "/tmp/sched-ablation/pico_pre.v"
    if os.path.exists(pico):
        jobs.append(("pico", "base", pico))
        for s in [1, 2, 3, 4, 5]:
            p = f"{WORK}/pico_perm{s}.v"
            with open(p, "w") as f:
                subprocess.run(["python3", PERM, pico, str(s)], stdout=f, check=True,
                               stderr=subprocess.DEVNULL)
            jobs.append(("pico", f"perm{s}", p))

    rows = []
    baselines = {}  # bench -> expected stdout
    os.chdir("/tmp/sched-ablation")  # firmware/ symlink for pico
    for bench, config, path in jobs:
        print(f"=== {bench} {config}", flush=True)
        a_sims, a_llvm, a_out = run_clotho(path)
        i_walls, i_comp, i_out = run_iverilog(path)
        # Outputs differ per size (n64/k16/...), so key baselines on it; all
        # pico configs share one expected output.
        key = (bench, "" if bench == "pico" else config.split("/")[0])
        if key not in baselines and a_out and not a_out.startswith(("ERROR", "TIMEOUT")):
            baselines[key] = a_out
        ok_a = a_out == baselines.get(key)
        ok_i = i_out == baselines.get(key)
        rows.append({
            "bench": bench, "config": config,
            "clotho_sim_ms": round(min(a_sims), 2) if a_sims else a_out,
            "clotho_llvm_ms": a_llvm,
            "iverilog_wall_s": round(min(i_walls), 3) if i_walls else i_out,
            "out_ok_clotho": ok_a, "out_ok_iverilog": ok_i,
        })
        print(f"    clotho sim {rows[-1]['clotho_sim_ms']} ms | "
              f"iverilog {rows[-1]['iverilog_wall_s']} s | out {ok_a}/{ok_i}", flush=True)

    with open(f"{WORK}/results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {WORK}/results.csv")


if __name__ == "__main__":
    main()
